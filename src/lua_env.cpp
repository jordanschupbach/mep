#include "lua_env.h"
#include "http_client.h"
#include "http_server.h"
#include "url_util.h"
#include "doc_export.h"
#include "editor.h"
#include "job.h"
#include "tcp_client.h"
#include "treesitter.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <filesystem>
#if defined(__linux__)
#include <unistd.h>
#endif
#endif

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "gfx/platform.h"

#include "json.h"
#include "persist.h"

// Defined in main.cpp -- returns the fixed-width glyph advance (in pixels)
// of the active font, needed below to convert a fraction of the window's
// pixel width into a character-column count for mep.sidebar_default_cols.
extern float GetCharWidthPx();
// Defined in main.cpp -- returns the current editor font size in pixels
// (g_font_size). Needed by kBuiltinOrgLatex's mep.font_size() binding; see
// its own comment for why.
extern float GetFontSizePx();
// Defined in main.cpp -- drops any cached inline-image texture for an
// already-resolved path, forcing an unconditional reload on next use. See
// its own comment (EvictOrgInlineImageTexture, main.cpp) for why
// mep.org_babel_execute (kBuiltinOrgBabel) needs this rather than trusting
// GetOrLoadOrgInlineImageTexture's own mtime-based cache alone.
extern void InvalidateOrgInlineImageTexture(const std::string &path);

namespace {

const char *kEditorRegistryKey = "__mep_editor";
const char *kLuaEnvRegistryKey = "__mep_luaenv";

/**
 * @brief Retrieves the Editor pointer stashed in the Lua registry at startup, for use by mep.* bindings.
 * @param L Lua state whose registry holds the editor pointer.
 * @return The Editor instance this Lua state is bound to.
 */
Editor *GetEditor(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEditorRegistryKey);
    Editor *ed = static_cast<Editor *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ed;
}

/**
 * @brief Retrieves the LuaEnv pointer stashed in the Lua registry at startup, for use by mep.* bindings.
 * @param L Lua state whose registry holds the LuaEnv pointer.
 * @return The LuaEnv instance that owns this Lua state.
 */
LuaEnv *GetLuaEnv(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kLuaEnvRegistryKey);
    LuaEnv *env = static_cast<LuaEnv *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return env;
}

// Reads a ref for `field` out of the table at stack index `tbl_idx`, or
// LUA_NOREF if absent/not a function.
/**
 * @brief Reads a function-valued field out of a Lua table and stores it in the registry as a callable reference.
 * @param L Lua state.
 * @param tbl_idx Stack index of the table to read the field from.
 * @param field Name of the field to look up.
 * @return A registry reference to the field's function, or LUA_NOREF if the field is absent or not a function.
 */
int RefField(lua_State *L, int tbl_idx, const char *field) {
    lua_getfield(L, tbl_idx, field);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

// mep.get_line(row) -> string. `row` is 1-indexed, matching Lua/Vim
// convention; the editor's own indices are 0-based.
/**
 * @brief Implements mep.get_line(row): returns the text of a 1-indexed line.
 * @param L Lua state; arg 1 is the 1-indexed row number.
 * @return Number of values pushed (1: the line's text).
 */
int l_get_line(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    std::string text = GetEditor(L)->GetLineForLua(row);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

/**
 * @brief Implements mep.set_line(row, text): replaces the content of a 1-indexed line.
 * @param L Lua state; arg 1 is the 1-indexed row number, arg 2 is the replacement text.
 * @return Number of values pushed (0).
 */
int l_set_line(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    size_t len = 0;
    const char *s = luaL_checklstring(L, 2, &len);
    GetEditor(L)->SetLineForLua(row, std::string(s, len));
    return 0;
}

// mep.replace_lines(start, end, lines): replaces lines [start, end)
// (1-indexed, end exclusive) with the given array of strings.
/**
 * @brief Implements mep.replace_lines(start, end, lines): replaces a 1-indexed, end-exclusive line range with a new set of lines.
 * @param L Lua state; arg 1 is the 1-indexed start row, arg 2 the 1-indexed exclusive end row, arg 3 an array of replacement strings.
 * @return Number of values pushed (0).
 */
int l_replace_lines(lua_State *L) {
    int start_row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int end_row = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> lines;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 3));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        lines.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    GetEditor(L)->ReplaceLinesForLua(start_row, end_row, lines);
    return 0;
}

/**
 * @brief Implements mep.line_count(): returns the number of lines in the current buffer.
 * @param L Lua state.
 * @return Number of values pushed (1: the line count).
 */
int l_line_count(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->LineCountForLua());
    return 1;
}

// mep.visual_selection() -> string ("" if no Visual selection is active).
// Charwise/linewise selections join lines with '\n'; a Visual Block
// selection returns one row's slice per line, also '\n'-joined, matching
// how a blockwise register's own text looks (see Editor::YankRange).
// Read-only: unlike y/d in Visual mode, this writes no register and
// leaves the selection itself untouched, so it's safe to call just to
// *look* at what's selected (e.g. before sending it to a REPL/AI chat).
/**
 * @brief Implements mep.visual_selection(): returns the currently selected text without altering the selection or any register.
 * @param L Lua state.
 * @return Number of values pushed (1: the selection's text, "" if none).
 */
int l_visual_selection(lua_State *L) {
    std::string text = GetEditor(L)->CurrentVisualSelectionText();
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// --- Spell checking (src/spell.h, backed by Editor's SpellChecker) ---------
// Thin bindings the kBuiltinSpell Lua module (main.cpp) drives: it owns the
// squiggle-decoration hook, leader-key menu, and suggestion picker, and calls
// through to these for the actual dictionary work.

// mep.spell_ready() -> bool. False until the wordlist has loaded (in which
// case the Lua module skips all squiggle/correction work).
int l_spell_ready(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->SpellReady());
    return 1;
}

// mep.spell_bad(word) -> bool. True if `word` should be flagged.
int l_spell_bad(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, GetEditor(L)->SpellIsBad(std::string(s, len)));
    return 1;
}

// mep.spell_suggest(word) -> { "best", ... }. Empty table if none.
int l_spell_suggest(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    std::vector<std::string> sugg = GetEditor(L)->SpellSuggest(std::string(s, len));
    lua_createtable(L, static_cast<int>(sugg.size()), 0);
    for (size_t i = 0; i < sugg.size(); i++) {
        lua_pushlstring(L, sugg[i].data(), sugg[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// mep.spell_add(word): add to the personal "good" dictionary (nvim zg).
int l_spell_add(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->SpellAddGood(std::string(s, len));
    return 0;
}

// mep.spell_wrong(word): mark as always-misspelled (nvim zw).
int l_spell_wrong(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->SpellAddWrong(std::string(s, len));
    return 0;
}

// mep.spell_enabled([bool]) -> bool. With no argument, queries; with a
// boolean argument, sets and returns the new state.
int l_spell_enabled(lua_State *L) {
    Editor *ed = GetEditor(L);
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) ed->SetSpellEnabled(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, ed->SpellEnabled());
    return 1;
}

// mep.spell_fix_selection() -> count. Fixes every misspelled word in the
// current Visual selection with its top suggestion, in one undo step.
int l_spell_fix_selection(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->FixSpellingInVisualSelection());
    return 1;
}

// mep.spell_fix_word() -> count (0 or 1). Fixes the word under the cursor.
int l_spell_fix_word(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->FixSpellingWordUnderCursor());
    return 1;
}

// mep.cursor() -> row, col (both 1-indexed).
/**
 * @brief Implements mep.cursor(): returns the current cursor position.
 * @param L Lua state.
 * @return Number of values pushed (2: 1-indexed row and column).
 */
int l_cursor(lua_State *L) {
    int row = 0, col = 0;
    GetEditor(L)->GetCursorForLua(&row, &col);
    lua_pushinteger(L, row + 1);
    lua_pushinteger(L, col + 1);
    return 2;
}

/**
 * @brief Implements mep.set_cursor(row, col): moves the cursor to a 1-indexed position.
 * @param L Lua state; arg 1 is the 1-indexed row, arg 2 the 1-indexed column.
 * @return Number of values pushed (0).
 */
int l_set_cursor(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int col = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    GetEditor(L)->SetCursorForLua(row, col);
    return 0;
}

// mep.current_buffer() -> the active pane's own buffer id.
/**
 * @brief Implements mep.current_buffer(): returns the active pane's buffer id.
 * @param L Lua state.
 * @return Number of values pushed (1: the current buffer id).
 */
int l_current_buffer(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->CurrentBufferId());
    return 1;
}

// mep.participant_set(id, name, buffer_id, row, col [, status]): upserts a
// synthetic local participant -- e.g. a Lua-driven AI stream -- so it gets
// the same tab-bar chip + in-buffer robot cursor a real connected mep-agent
// gets via agent_rpc.cpp. row/col are 1-indexed like mep.cursor()/
// mep.set_cursor(). `status` is optional, same vocabulary as a real agent's
// (""/"idle"/"thinking"/"writing"/"awaiting_input"/"done").
/**
 * @brief Implements mep.participant_set(id, name, buffer_id, row, col [, status]): upserts a synthetic local participant's tab-bar chip and in-buffer cursor.
 * @param L Lua state; args are id, name, buffer_id, 1-indexed row, 1-indexed col, and an optional status string.
 * @return Number of values pushed (0).
 */
int l_participant_set(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 3));
    int row = static_cast<int>(luaL_checkinteger(L, 4)) - 1;
    int col = static_cast<int>(luaL_checkinteger(L, 5)) - 1;
    std::string status;
    if (lua_gettop(L) >= 6 && lua_isstring(L, 6)) status = lua_tostring(L, 6);
    GetEditor(L)->SetLocalParticipant(id, name, buffer_id, row, col, status);
    return 0;
}

// mep.participant_clear(id): removes a participant added via
// mep.participant_set (e.g. once an AI stream finishes or is cancelled).
/**
 * @brief Implements mep.participant_clear(id): removes a previously-added synthetic local participant.
 * @param L Lua state; arg 1 is the participant id.
 * @return Number of values pushed (0).
 */
int l_participant_clear(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    GetEditor(L)->ClearLocalParticipant(id);
    return 0;
}

// mep.participants() -> array of {id=, name=, kind='human'|'agent',
// buffer_id=, row=, col= (1-indexed, only when has_location), has_location=,
// status=, terminal_buffer_id=} -- the same Editor::Participants() snapshot
// the tab-bar chips draw from (collab peers, socket-connected mep-agents,
// and mep.participant_set locals alike). A fresh pull each call, so poll
// it (mep.on_frame with a change hash, the way the AI-agents sidebar in
// kBuiltinAiTerminal does) rather than expecting an event.
/**
 * @brief Implements mep.participants(): lists every current participant (human collaborators, connected agents, synthetic locals).
 * @param L Lua state.
 * @return Number of values pushed (1: array of participant tables).
 */
int l_participants(lua_State *L) {
    std::vector<Editor::ParticipantInfo> parts = GetEditor(L)->Participants();
    lua_createtable(L, static_cast<int>(parts.size()), 0);
    for (size_t i = 0; i < parts.size(); i++) {
        const Editor::ParticipantInfo &p = parts[i];
        lua_createtable(L, 0, 9);
        lua_pushlstring(L, p.id.data(), p.id.size());
        lua_setfield(L, -2, "id");
        lua_pushlstring(L, p.name.data(), p.name.size());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, p.kind == Editor::ParticipantKind::Agent ? "agent" : "human");
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, p.buffer_id);
        lua_setfield(L, -2, "buffer_id");
        lua_pushboolean(L, p.has_location);
        lua_setfield(L, -2, "has_location");
        if (p.has_location) {
            lua_pushinteger(L, p.row + 1);
            lua_setfield(L, -2, "row");
            lua_pushinteger(L, p.col + 1);
            lua_setfield(L, -2, "col");
        }
        lua_pushlstring(L, p.status.data(), p.status.size());
        lua_setfield(L, -2, "status");
        lua_pushinteger(L, p.terminal_buffer_id);
        lua_setfield(L, -2, "terminal_buffer_id");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

/**
 * @brief Implements mep.insert_text(text): inserts text at the cursor position.
 * @param L Lua state; arg 1 is the text to insert.
 * @return Number of values pushed (0).
 */
int l_insert_text(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->InsertTextForLua(std::string(s, len));
    return 0;
}

// mep.clipboard_get() -> text: the system clipboard's current text ("" if
// empty/unavailable -- see Editor::SystemClipboardRead for the wasm
// caveats). mep.clipboard_set(text): copies text to the system clipboard.
// Deliberately *not* routed through the unnamed register: these are the
// raw clipboard for scripts that want it (e.g. a "copy file path" command),
// and leave mep's own registers untouched.
/**
 * @brief Implements mep.clipboard_get(): returns the system clipboard's text.
 * @param L Lua state; no args.
 * @return Number of values pushed (1: the clipboard text, "" if none).
 */
int l_clipboard_get(lua_State *L) {
    std::string text = GetEditor(L)->SystemClipboardRead();
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

/**
 * @brief Implements mep.clipboard_set(text): copies text to the system clipboard.
 * @param L Lua state; arg 1 is the text.
 * @return Number of values pushed (0).
 */
int l_clipboard_set(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->SystemClipboardWrite(std::string(s, len));
    return 0;
}

// mep.visual_change(): the Lua equivalent of pressing "c" on the current
// Visual selection -- deletes it (with the same undo/register semantics
// as any other change) and leaves the cursor, in Insert mode, at the
// deletion point, ready for mep.insert_text to stream a replacement in.
/**
 * @brief Implements mep.visual_change(): deletes the current Visual selection and enters Insert mode at the deletion point, like pressing "c".
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_visual_change(lua_State *L) {
    GetEditor(L)->ChangeVisualSelectionForLua();
    return 0;
}

// mep.enter_normal(): the Lua equivalent of pressing <Esc> -- returns to
// Normal mode from Insert or Visual.
/**
 * @brief Implements mep.enter_normal(): returns to Normal mode from Insert or Visual, like pressing Escape.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_enter_normal(lua_State *L) {
    GetEditor(L)->EnterNormalForLua();
    return 0;
}

// mep.enter_insert(): the Lua equivalent of pressing "i" -- pushes one
// undo checkpoint and enters Insert mode. Used by speech-to-text so a
// streamed-in transcript behaves like typed text (one undo step per
// dictation) when the buffer wasn't already in Insert mode.
/**
 * @brief Implements mep.enter_insert(): pushes an undo checkpoint and enters Insert mode, like pressing "i".
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_enter_insert(lua_State *L) {
    GetEditor(L)->EnterInsertForLua();
    return 0;
}

// mep.is_insert_mode() -> true while Mode::Insert is active.
/**
 * @brief Implements mep.is_insert_mode(): reports whether Insert mode is currently active.
 * @param L Lua state.
 * @return Number of values pushed (1: true if in Insert mode).
 */
int l_is_insert_mode(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsInsertModeForLua());
    return 1;
}

// mep.stt_set_recording(bool): sets the tab bar's speech-to-text
// recording indicator on/off -- purely a display flag, see
// Editor::stt_recording_'s own comment; the recording process itself is
// entirely Lua/job-driven (mep.stt_toggle).
/**
 * @brief Implements mep.stt_set_recording(bool): sets the tab bar's speech-to-text recording indicator.
 * @param L Lua state; arg 1 is the boolean recording state.
 * @return Number of values pushed (0).
 */
int l_stt_set_recording(lua_State *L) {
    GetEditor(L)->SetSttRecording(lua_toboolean(L, 1));
    return 0;
}

// mep.notify(msg [, level]): level is "debug"/"info"(default)/"warn"/
// "error". Single choke point for the whole app's messages (Phase 6) --
// feeds a toast + persistent history entry, and still updates the status
// line the way this always has.
/**
 * @brief Implements mep.notify(msg [, level]): shows a toast and records a persistent history entry/status line message.
 * @param L Lua state; arg 1 is the message text, optional arg 2 is the level ("debug"/"info"/"warn"/"error", default "info").
 * @return Number of values pushed (0).
 */
int l_notify(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    Editor::NotifyLevel level = Editor::NotifyLevel::Info;
    if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
        std::string lvl = lua_tostring(L, 2);
        if (lvl == "error") level = Editor::NotifyLevel::Error;
        else if (lvl == "warn") level = Editor::NotifyLevel::Warn;
        else if (lvl == "debug") level = Editor::NotifyLevel::Debug;
    }
    GetEditor(L)->Notify(std::string(s, len), level);
    return 0;
}

/**
 * @brief Implements mep.quit(): requests that the editor exit.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_quit(lua_State *L) {
    GetEditor(L)->RequestQuit();
    return 0;
}

// mep.command(name, fn): defines a ":name" ex-command implemented in Lua.
/**
 * @brief Implements mep.command(name, fn): registers a Lua function as a ":name" ex-command.
 * @param L Lua state; arg 1 is the command name, arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_command(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterLuaCommand(name, ref);
    return 0;
}

// mep.map(mode, key, fn, opts): binds a single key in normal ("n") or
// visual ("v"/"V") mode to a Lua function. Overrides the builtin for that
// key. `opts` is optional: {desc = "..."} records a human-readable
// description (Editor::AllMappingDescriptions()) for the help picker's
// keybinding introspection (NVIM_PARITY_PLAN.md Phase 25) -- mirrors
// mep.leader_map's own (positional, not opts-table) description arg;
// opts here since mep.map already has 3 positional args and more optional
// fields are plausible later, unlike leader_map's fixed shape.
/**
 * @brief Implements mep.map(mode, key, fn, opts): binds a key in Normal or Visual mode to a Lua function, optionally recording a description for the help picker.
 * @param L Lua state; arg 1 is the mode string ("n"/"v"/"V"), arg 2 the key, arg 3 the callback function, optional arg 4 a table with a "desc" field.
 * @return Number of values pushed (0).
 */
int l_map(lua_State *L) {
    const char *mode_str = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    std::string description;
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "desc");
        if (lua_isstring(L, -1)) description = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    Mode mode = (mode_str[0] == 'v' || mode_str[0] == 'V') ? Mode::Visual : Mode::Normal;
    GetEditor(L)->RegisterLuaMapping(mode, std::string(key), ref, description);
    return 0;
}

// mep.mapping_descriptions() -> array of {mode=, key=, desc=} for every
// mep.map() binding that was given an opts.desc -- what the help picker's
// keybinding introspection (NVIM_PARITY_PLAN.md Phase 25) reads to list
// "what does this key do" instead of just "what commands exist" (see
// kBuiltinDocs's mep.keymaps() picker in main.cpp, the consumer this
// registry was originally built for but went unconsumed until now).
/**
 * @brief Implements mep.mapping_descriptions(): lists every mep.map() binding that was given an opts.desc.
 * @param L Lua state.
 * @return Number of values pushed (1: array of {mode=, key=, desc=} tables).
 */
int l_mapping_descriptions(lua_State *L) {
    std::vector<Editor::MappingDescription> descs = GetEditor(L)->AllMappingDescriptions();
    lua_createtable(L, static_cast<int>(descs.size()), 0);
    for (size_t i = 0; i < descs.size(); i++) {
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, &descs[i].mode, 1);
        lua_setfield(L, -2, "mode");
        lua_pushlstring(L, descs[i].key.data(), descs[i].key.size());
        lua_setfield(L, -2, "key");
        lua_pushlstring(L, descs[i].description.data(), descs[i].description.size());
        lua_setfield(L, -2, "desc");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.leader_bindings() -> array of {seq=, desc=} for every mep.leader_map()
// registration, unfiltered by whichkey's own currently-typed prefix (unlike
// the transient WhichKey overlay, which only ever shows matches for
// whatever prefix is typed so far). The leader-sequence half of what
// mep.keymaps() (kBuiltinDocs, main.cpp) lists, alongside
// mep.mapping_descriptions()'s plain mep.map() side above.
/**
 * @brief Implements mep.leader_bindings(): lists every mep.leader_map() registration, unfiltered by any currently-typed prefix.
 * @param L Lua state.
 * @return Number of values pushed (1: array of {seq=, desc=} tables).
 */
int l_leader_bindings(lua_State *L) {
    std::vector<WhichKeyBinding> bindings = GetEditor(L)->AllWhichKeyBindings();
    lua_createtable(L, static_cast<int>(bindings.size()), 0);
    for (size_t i = 0; i < bindings.size(); i++) {
        lua_createtable(L, 0, 2);
        // Display form: Enter comes back as "<CR>", the same spelling
        // mep.leader_map accepted it in.
        std::string seq = Editor::WhichKeySequenceDisplay(bindings[i].sequence);
        lua_pushlstring(L, seq.data(), seq.size());
        lua_setfield(L, -2, "seq");
        lua_pushlstring(L, bindings[i].description.data(), bindings[i].description.size());
        lua_setfield(L, -2, "desc");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.map_mod1(key, fn, repeat?): binds a single letter key under the mod1
// modifier (see mep.set_mod1), globally across all modes. `key` is a bare
// letter ("h") for mod1+letter, or "S-"/"C-" prefixed ("S-h", "C-h") for
// mod1+Shift+letter / mod1+Ctrl+letter. Two non-letter keys are also
// recognized, each its own special case in HandleMod1Shortcuts since
// neither falls in the A-Z scan the letter case uses: "Tab"/"S-Tab", and
// "CR"/"S-CR" (Enter -- no Ctrl variant, matching Tab). Overrides any
// prior mapping for that exact key, including the startup defaults.
//
// `repeat` (default false) lets holding the key keep re-firing it at the OS
// key-repeat rate instead of only on the initial press -- most mod1 actions
// (split, close buffer, popout toggle, ...) would misbehave if held (e.g.
// spawning a stack of splits, or flickering a toggle), so this defaults
// off; resize_pane's S-h/j/k/l bindings below are the one default case
// that opts in, matching how holding plain h/j/k/l already repeats in a
// buffer.
/**
 * @brief Implements mep.map_mod1(key, fn, repeat?): binds a single letter key (or "Tab"/"S-Tab"/"CR"/"S-CR") under the mod1 modifier, globally across all modes.
 * @param L Lua state; arg 1 is the key, arg 2 the callback function, arg 3 (optional, default false) whether holding the key repeats it.
 * @return Number of values pushed (0).
 */
int l_map_mod1(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    bool repeat = lua_toboolean(L, 3) != 0;
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterMod1Mapping(key, ref, repeat);
    return 0;
}

// mep.map_g(key, fn): binds a single letter key after a leading "g" in
// Normal mode (e.g. "d" for "gd") to a Lua callback -- for g-prefixed
// actions mep's own built-in motions don't already claim (gg/ge/gE/gu/
// gU/gJ/gv). A bare mep.map only ever sees a single already-unprefixed
// keystroke, so it can't reach anything typed after a pending "g" the
// way this can. Fires as a freestanding action, never composed with a
// pending operator (see Editor::DispatchNormalKey's own comment on why:
// unlike gg/ge/gE, a Lua-side g-action -- e.g. an LSP goto-definition --
// can't resolve a target position synchronously for ApplyOperator).
/**
 * @brief Implements mep.map_g(key, fn): binds a Lua callback to a key following a leading "g" in Normal mode.
 * @param L Lua state; arg 1 is the key following "g", arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_map_g(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterGMapping(key, ref);
    return 0;
}

// mep.map_g_visual(key, fn): same as mep.map_g, but for Visual mode's own
// separate g-prefix dispatch (DispatchVisualKey) instead of Normal's --
// the two never share a table, so e.g. "gl" can mean something different
// (or nothing) in Visual mode than it does in Normal mode.
/**
 * @brief Implements mep.map_g_visual(key, fn): binds a Lua callback to a key following a leading "g" in Visual mode.
 * @param L Lua state; arg 1 is the key following "g", arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_map_g_visual(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterVisualGMapping(key, ref);
    return 0;
}

// mep.map_bracket_prev(key, fn) / mep.map_bracket_next(key, fn): same
// shape as mep.map_g, but for a leading "[" / "]" instead of "g" (e.g.
// "e" for "[e"/"]e" LSP diagnostic navigation) -- mep has no built-in
// bracket motion of its own beyond the unrelated i[/a[ text objects, so
// there's nothing these could collide with.
/**
 * @brief Implements mep.map_bracket_prev(key, fn): binds a Lua callback to a key following a leading "[" in Normal mode.
 * @param L Lua state; arg 1 is the key following "[", arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_map_bracket_prev(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterBracketPrevMapping(key, ref);
    return 0;
}

/**
 * @brief Implements mep.map_bracket_next(key, fn): binds a Lua callback to a key following a leading "]" in Normal mode.
 * @param L Lua state; arg 1 is the key following "]", arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_map_bracket_next(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterBracketNextMapping(key, ref);
    return 0;
}

// mep.set_mod1(name): "alt" (default), "ctrl", "shift", or "super".
/**
 * @brief Implements mep.set_mod1(name): selects which physical modifier key acts as "mod1" for mep.map_mod1 bindings.
 * @param L Lua state; arg 1 is the modifier name ("alt", "ctrl", "shift", or "super").
 * @return Number of values pushed (0).
 */
int l_set_mod1(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    GetEditor(L)->SetMod1(name);
    return 0;
}

// mep.nav_pane(direction): moves focus to the pane best positioned
// "left"/"down"/"up"/"right" of the active one; a no-op if there's none.
/**
 * @brief Implements mep.nav_pane(direction): moves focus to the neighboring pane in the given direction, if any.
 * @param L Lua state; arg 1 is the direction ("left"/"down"/"up"/"right").
 * @return Number of values pushed (0).
 */
int l_nav_pane(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    GetEditor(L)->NavigatePaneDirection(dir);
    return 0;
}

// mep.focus_top_left_pane(): moves focus to the topmost, then leftmost,
// pane in the active tab's split layout -- used by the file tree so a
// click always opens near the tree rather than in whatever pane was last
// active before the sidebar took focus.
/**
 * @brief Implements mep.focus_top_left_pane(): moves focus to the topmost, then leftmost, pane in the active tab's split layout.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_focus_top_left_pane(lua_State *L) {
    GetEditor(L)->FocusTopLeftPane();
    return 0;
}

// mep.resize_pane(direction, step?): moves the split boundary between the
// active pane and its neighbor "left"/"down"/"up"/"right" by `step` (a
// 0..1 fraction of the split's extent, defaults to 5% if omitted).
/**
 * @brief Implements mep.resize_pane(direction, step?): moves the split boundary between the active pane and its neighbor.
 * @param L Lua state; arg 1 is the direction ("left"/"down"/"up"/"right"), optional arg 2 the 0..1 step fraction (default 5%).
 * @return Number of values pushed (0).
 */
int l_resize_pane(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    float step = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    GetEditor(L)->ResizeActivePane(dir, step);
    return 0;
}

// mep.pane_set_share(fraction): sets the active pane's share of its
// immediate parent split to an absolute 0..1 fraction, rather than nudging
// it by a step like mep.resize_pane -- only meaningful when that parent
// has exactly two children. Used by the built-in project_open to give the
// readme pane ~2/3 of the height and the terminal pane the rest.
/**
 * @brief Implements mep.pane_set_share(fraction): sets the active pane's share of its parent split to an absolute fraction.
 * @param L Lua state; arg 1 is the 0..1 share fraction.
 * @return Number of values pushed (0).
 */
int l_pane_set_share(lua_State *L) {
    float fraction = static_cast<float>(luaL_checknumber(L, 1));
    GetEditor(L)->SetActivePaneShare(fraction);
    return 0;
}

// mep.current_tab_id() -> the active tab's stable id (Tab::id) -- unlike
// its index, unchanged by :tabdelete/:tabnew around it, so a script can
// key per-tab state on it (kBuiltinTabTerminal's tab -> terminal map).
/**
 * @brief Implements mep.current_tab_id(): returns the active tab's stable id.
 * @param L Lua state.
 * @return Number of values pushed (1: the tab id).
 */
int l_current_tab_id(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->ActiveTabId());
    return 1;
}

// mep.current_pane_id() -> the focused pane's id in the active tab; pair
// with mep.pane_focus(id) to come back to it after a split/close moved
// focus elsewhere.
/**
 * @brief Implements mep.current_pane_id(): returns the active pane's id.
 * @param L Lua state.
 * @return Number of values pushed (1: the pane id).
 */
int l_current_pane_id(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->ActivePaneId());
    return 1;
}

// mep.pane_focus(pane_id) -> bool: focuses that pane of the active tab
// (Editor::FocusPaneById, the same jump a header click makes); false, and
// a no-op, if no such pane exists in this tab any more.
/**
 * @brief Implements mep.pane_focus(pane_id): focuses a pane of the active tab by id.
 * @param L Lua state; arg 1 is the pane id.
 * @return Number of values pushed (1: true if the pane exists and is now focused).
 */
int l_pane_focus(lua_State *L) {
    int pane_id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->FocusPaneByIdForLua(pane_id));
    return 1;
}

// mep.pane_split_bottom(buffer_id?, share?): opens a new full-width pane
// along the bottom of the active tab (vim's `:botright split`, Editor::
// SplitTabBottom) showing buffer_id -- the current buffer when nil, the
// same way a bare `:split` duplicates it -- focused, taking `share`
// (default 0.3) of the tab's height. Unlike `:split`, the new pane spans
// the whole tab even when the focused pane sits inside a vsplit.
/**
 * @brief Implements mep.pane_split_bottom(buffer_id?, share?): opens and focuses a full-width bottom pane in the active tab.
 * @param L Lua state; optional arg 1 the buffer to show (default: the current buffer), optional arg 2 its height share (default 0.3).
 * @return Number of values pushed (0).
 */
int l_pane_split_bottom(lua_State *L) {
    Editor *ed = GetEditor(L);
    int buffer_id = lua_isnoneornil(L, 1) ? ed->CurrentBufferId() : static_cast<int>(luaL_checkinteger(L, 1));
    float share = static_cast<float>(luaL_optnumber(L, 2, 0.3));
    ed->SplitTabBottom(buffer_id, share);
    return 0;
}

// mep.pane_split_left(buffer_id?, share?): opens a new full-height pane
// along the left of the active tab (Editor::SplitTabLeft), the mirror
// image of mep.pane_split_bottom above -- same "the current buffer when
// nil" and "spans the whole tab even when the focused pane sits inside a
// split" behavior, just along the left edge instead of the bottom.
/**
 * @brief Implements mep.pane_split_left(buffer_id?, share?): opens and focuses a full-height left pane in the active tab.
 * @param L Lua state; optional arg 1 the buffer to show (default: the current buffer), optional arg 2 its width share (default 0.3).
 * @return Number of values pushed (0).
 */
int l_pane_split_left(lua_State *L) {
    Editor *ed = GetEditor(L);
    int buffer_id = lua_isnoneornil(L, 1) ? ed->CurrentBufferId() : static_cast<int>(luaL_checkinteger(L, 1));
    float share = static_cast<float>(luaL_optnumber(L, 2, 0.3));
    ed->SplitTabLeft(buffer_id, share);
    return 0;
}

// mep.pane_split_right(buffer_id?, share?): mep.pane_split_left's exact
// mirror image (Editor::SplitTabRight) -- same "the current buffer when
// nil" and "spans the whole tab even when the focused pane sits inside a
// split" behavior, just along the right edge instead of the left.
/**
 * @brief Implements mep.pane_split_right(buffer_id?, share?): opens and focuses a full-height right pane in the active tab.
 * @param L Lua state; optional arg 1 the buffer to show (default: the current buffer), optional arg 2 its width share (default 0.3).
 * @return Number of values pushed (0).
 */
int l_pane_split_right(lua_State *L) {
    Editor *ed = GetEditor(L);
    int buffer_id = lua_isnoneornil(L, 1) ? ed->CurrentBufferId() : static_cast<int>(luaL_checkinteger(L, 1));
    float share = static_cast<float>(luaL_optnumber(L, 2, 0.3));
    ed->SplitTabRight(buffer_id, share);
    return 0;
}

// mep.vsplit_right(path): opens `path` in a new vertical-split pane to
// the RIGHT of the focused one (focused afterward) -- the mirror image
// of a bare `:vsplit path`/mep.cmd('vsplit') (which, matching vim's own
// default splitright=off, opens to the left). No ex-command/`:` spelling
// of this exists since nothing else in mep needs it today; added for the
// org-mode Run button's output preview (kBuiltinRunButton, main.cpp),
// which reads left-to-right (source on the left, result on the right)
// better than vim's own split default gives it for free.
/**
 * @brief Implements mep.vsplit_right(path): opens path in a new vertical split to the right of the focused pane.
 * @param L Lua state; arg 1 is the file path to open in the new pane.
 * @return Number of values pushed (0).
 */
int l_vsplit_right(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->SplitPaneRight(path);
    return 0;
}

// mep.split_below(path?): opens `path` (or duplicates the current buffer,
// like a bare `:split`, when omitted) in a new horizontal-split pane
// below the focused one -- the mirror image of mep.vsplit_right above,
// one axis over.
/**
 * @brief Implements mep.split_below(path?): opens a new horizontal split below the focused pane.
 * @param L Lua state; optional arg 1 the file path to open (default: duplicate the current buffer).
 * @return Number of values pushed (0).
 */
int l_split_below(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "");
    GetEditor(L)->SplitPaneBelow(path);
    return 0;
}

// mep.cmd(str): runs str as if typed after ":" and Enter pressed. General
// escape hatch for Lua to drive any ex-command (":vsplit", ":w", ...).
/**
 * @brief Implements mep.cmd(str): runs str as if typed after ":" and Enter were pressed.
 * @param L Lua state; arg 1 is the ex-command string.
 * @return Number of values pushed (0).
 */
int l_cmd(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->RunCommand(std::string(s, len));
    return 0;
}

// mep.open(path): opens `path` in its default view -- for an .html/.htm
// file, that's the rendered :Browse viewer, not plain text (Editor::
// LoadFile's own force_text comment). Every picker/sidebar/LSP jump that
// just wants to "open this file" (file tree, find_files, buffers,
// live_grep, LSP goto/references, todos, git status, roam, leetcode)
// calls this instead of mep.cmd('e ' .. path), so the literal `:e`/
// `:edit` ex-command can stay the one deliberate force-text escape hatch
// without every other open path inheriting it too.
/**
 * @brief Implements mep.open(path): opens a file in its default view (e.g. the rendered viewer for HTML rather than plain text).
 * @param L Lua state; arg 1 is the path to open.
 * @return Number of values pushed (0).
 */
int l_open(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->LoadFile(std::string(s, len));
    return 0;
}

// mep.pick_pane_open(path): like mep.open, but when the active tab has more
// than one candidate pane to host the file, first lets the user choose
// which window it opens in -- each candidate window gets a big centered
// letter (a, b, c ... in split order) and pressing it opens `path` there
// (main.cpp's pane picker, armed via Editor::RequestPanePick right after
// this frame's HandleInput). One candidate opens immediately with no
// prompt; none falls back to opening in the current pane. The file tree's
// Enter-on-a-file uses this so a file no longer always lands in one fixed
// default pane.
/**
 * @brief Implements mep.pick_pane_open(path): opens a file, prompting for the destination window when several exist.
 * @param L Lua state; arg 1 is the file path to open.
 * @return Number of values pushed (0).
 */
int l_pick_pane_open(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->RequestPanePick(std::string(s, len));
    return 0;
}

// mep.sidebar_default_cols(fraction?): character-column width equal to
// `fraction` (0..1, default 0.2) of the current window's pixel width --
// lets a sidebar's opening size scale with the window instead of being a
// hardcoded column count that grows awkwardly wide/narrow as the font size
// changes (see mep.tree_refresh's mep.sidebar_create call).
/**
 * @brief Implements mep.sidebar_default_cols(fraction?): computes a character-column width equal to a fraction of the window's pixel width.
 * @param L Lua state; optional arg 1 is the 0..1 fraction of window width (default 0.2).
 * @return Number of values pushed (1: the column count, at least 10).
 */
int l_sidebar_default_cols(lua_State *L) {
    double frac = luaL_optnumber(L, 1, 0.2);
    float char_width = GetCharWidthPx();
    int cols = 34;
    if (char_width > 0.0f) {
        cols = static_cast<int>((static_cast<double>(gfx::GetScreenWidth()) * frac) / static_cast<double>(char_width));
    }
    lua_pushinteger(L, std::max(cols, 10));
    return 1;
}

// mep.terminal_here(args?): like `:terminal args`, but attaches the shell to
// the currently active pane instead of splitting -- for scripts that already
// arranged the split layout themselves (e.g. the built-in project_open,
// which wants the terminal pane on the bottom rather than :terminal's
// above/left default).
/**
 * @brief Implements mep.terminal_here(args?): attaches a shell to the currently active pane rather than splitting.
 * @param L Lua state; optional arg 1 is the shell command arguments.
 * @return Number of values pushed (0).
 */
int l_terminal_here(lua_State *L) {
    GetEditor(L)->OpenTerminalInPlace(luaL_optstring(L, 1, ""));
    return 0;
}

// mep.terminal_here_argv(argv, title?): like mep.terminal_here, but runs
// `argv` (an array of strings, argv[1] resolved via PATH) directly with
// no `$SHELL -c` wrapper -- for callers whose arguments would otherwise
// need shell quoting (kBuiltinAiTerminal passes a multi-line system
// prompt as one argument). `title` is the buffer tab's label; defaults
// to argv[1].
/**
 * @brief Implements mep.terminal_here_argv(argv, title?): attaches a terminal running `argv` directly (no shell) to the active pane.
 * @param L Lua state; arg 1 is an array of strings (program + arguments), optional arg 2 the tab title.
 * @return Number of values pushed (0).
 */
int l_terminal_here_argv(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "terminal_here_argv: argv[%d] is not a string", static_cast<int>(i));
        }
        size_t len = 0;
        const char *s = lua_tolstring(L, -1, &len);
        argv.emplace_back(s, len);
        lua_pop(L, 1);
    }
    if (argv.empty()) return luaL_error(L, "terminal_here_argv: argv is empty");
    GetEditor(L)->OpenTerminalInPlaceArgv(argv, luaL_optstring(L, 2, ""));
    return 0;
}

// mep.is_terminal_buffer(buffer_id) -> bool: true for a real `:terminal`
// buffer (Editor::terminals_), false for anything else -- including a
// mep.term_start-backed Run/REPL output buffer (kBuiltinRun), which is a
// plain buffer Lua renders into and the C++ side never tracks as one.
/**
 * @brief Implements mep.is_terminal_buffer(buffer_id): reports whether a buffer is a real `:terminal` buffer.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: true only for a live `:terminal` buffer).
 */
int l_is_terminal_buffer(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->IsTerminalBuffer(buffer_id));
    return 1;
}

// mep.terminal_write(buffer_id, text) -> bool: writes `text` verbatim
// (no newline appended -- callers wanting one write it themselves, same
// convention as mep.job_write) into a real `:terminal` buffer's own
// PTY. False if `buffer_id` isn't a live terminal.
/**
 * @brief Implements mep.terminal_write(buffer_id, text): writes text verbatim into a live `:terminal` buffer's PTY.
 * @param L Lua state; arg 1 is the terminal buffer id, arg 2 the text to write.
 * @return Number of values pushed (1: false if buffer_id isn't a live terminal).
 */
int l_terminal_write(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    size_t len = 0;
    const char *s = luaL_checklstring(L, 2, &len);
    lua_pushboolean(L, GetEditor(L)->WriteToTerminalBuffer(buffer_id, std::string(s, len)));
    return 1;
}

// mep.job_start(argv, opts) -> id. `argv` is an array of strings (argv[1]
// is the executable). `opts` (optional table): `cwd`, `on_stdout(line)`,
// `on_stderr(line)`, `on_exit(code)` -- code is -1 if the job was killed
// or never started (missing binary, spawn failure).
/**
 * @brief Implements mep.job_start(argv, opts) -> id: spawns a subprocess and wires its stdout/stderr/exit to optional Lua callbacks.
 * @param L Lua state; arg 1 is an array of argv strings, optional arg 2 a table with cwd/on_stdout/on_stderr/on_exit.
 * @return Number of values pushed (1: the new job's id).
 */
// --- Jupyter notebook (notebook_doc.h / Editor::Notebook*) ------------------
// All operate on the active pane's buffer (what the keybindings in
// kBuiltinNotebook, main.cpp, want); an explicit index argument is a
// 0-based cell index, nil/-1 meaning "the cell under the cursor".

namespace {
NotebookCellType NotebookTypeFromString(const char *s) {
    std::string t = s ? s : "code";
    if (t == "markdown" || t == "md") return NotebookCellType::Markdown;
    if (t == "raw") return NotebookCellType::Raw;
    return NotebookCellType::Code;
}
int NotebookIndexArg(lua_State *L, int idx) {
    return lua_isnoneornil(L, idx) ? -1 : static_cast<int>(luaL_checkinteger(L, idx));
}
}  // namespace

/**
 * @brief Implements mep.notebook_is_buffer(buffer_id?): whether a buffer (default: the current one) is an open .ipynb notebook.
 * @param L Lua state; optional arg 1 is a buffer id.
 * @return Number of values pushed (1: boolean).
 */
int l_notebook_is_buffer(lua_State *L) {
    Editor *ed = GetEditor(L);
    int id = lua_isnoneornil(L, 1) ? ed->CurrentBufferId() : static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, ed->IsNotebookBuffer(id));
    return 1;
}

/**
 * @brief Implements mep.notebook_run_cell(index?): queues a code cell (default: the one under the cursor) on the kernel.
 * @param L Lua state; optional arg 1 is a 0-based cell index.
 * @return Number of values pushed (1: true if the cell was queued).
 */
int l_notebook_run_cell(lua_State *L) {
    Editor *ed = GetEditor(L);
    lua_pushboolean(L, ed->NotebookRunCell(ed->CurrentBufferId(), NotebookIndexArg(L, 1)));
    return 1;
}

/** @brief Implements mep.notebook_run_all(): queues every code cell of the current notebook in order. */
int l_notebook_run_all(lua_State *L) {
    Editor *ed = GetEditor(L);
    ed->NotebookRunAll(ed->CurrentBufferId());
    return 0;
}

/** @brief Implements mep.notebook_run_and_advance(): runs the cell under the cursor and moves to the next (Shift+Enter). */
int l_notebook_run_and_advance(lua_State *L) {
    GetEditor(L)->NotebookRunCellAtCursor(/*advance=*/true, /*insert_below=*/false);
    return 0;
}

/** @brief Implements mep.notebook_run_and_insert(): runs the cell under the cursor and inserts a new one below (Alt+Enter). */
int l_notebook_run_and_insert(lua_State *L) {
    GetEditor(L)->NotebookRunCellAtCursor(/*advance=*/false, /*insert_below=*/true);
    return 0;
}

/** @brief Implements mep.notebook_interrupt(): sends KeyboardInterrupt to the running cell and drops the queue. */
int l_notebook_interrupt(lua_State *L) {
    Editor *ed = GetEditor(L);
    ed->NotebookInterrupt(ed->CurrentBufferId());
    return 0;
}

/** @brief Implements mep.notebook_restart_kernel(): kills and relaunches the current notebook's kernel. */
int l_notebook_restart_kernel(lua_State *L) {
    Editor *ed = GetEditor(L);
    ed->NotebookRestartKernel(ed->CurrentBufferId());
    return 0;
}

/**
 * @brief Implements mep.notebook_clear_outputs(index?): clears one cell's outputs (default: the cursor's cell); pass -1 for every cell.
 * @param L Lua state; optional arg 1 is a 0-based cell index or -1.
 * @return Number of values pushed (0).
 */
int l_notebook_clear_outputs(lua_State *L) {
    Editor *ed = GetEditor(L);
    int idx = lua_isnoneornil(L, 1) ? ed->NotebookCellAtCursor() : static_cast<int>(luaL_checkinteger(L, 1));
    if (lua_isnoneornil(L, 1) && idx < 0) return 0;
    ed->NotebookClearOutputs(ed->CurrentBufferId(), idx);
    return 0;
}

/**
 * @brief Implements mep.notebook_insert_cell(below, type?): inserts an empty cell above/below the cursor's cell and moves into it.
 * @param L Lua state; arg 1 is a boolean (true = below), optional arg 2 is "code"/"markdown"/"raw".
 * @return Number of values pushed (1: the new cell's 0-based index, or -1).
 */
int l_notebook_insert_cell(lua_State *L) {
    bool below = lua_toboolean(L, 1) != 0;
    NotebookCellType type = NotebookTypeFromString(luaL_optstring(L, 2, "code"));
    lua_pushinteger(L, GetEditor(L)->NotebookInsertCell(-1, below, type));
    return 1;
}

/** @brief Implements mep.notebook_delete_cell(index?): deletes a cell (default: the cursor's). */
int l_notebook_delete_cell(lua_State *L) {
    GetEditor(L)->NotebookDeleteCell(NotebookIndexArg(L, 1));
    return 0;
}

/**
 * @brief Implements mep.notebook_set_cell_type(type, index?): changes a cell's type ("code"/"markdown"/"raw").
 * @param L Lua state; arg 1 is the type name, optional arg 2 a 0-based cell index.
 * @return Number of values pushed (0).
 */
int l_notebook_set_cell_type(lua_State *L) {
    NotebookCellType type = NotebookTypeFromString(luaL_checkstring(L, 1));
    GetEditor(L)->NotebookSetCellType(NotebookIndexArg(L, 2), type);
    return 0;
}

/**
 * @brief Implements mep.notebook_move_cell(delta): swaps the cursor's cell with its neighbor (-1 up, +1 down).
 * @param L Lua state; arg 1 is the delta.
 * @return Number of values pushed (0).
 */
int l_notebook_move_cell(lua_State *L) {
    GetEditor(L)->NotebookMoveCell(-1, static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

/**
 * @brief Implements mep.notebook_cell_index(): the 0-based index of the cell under the cursor, or -1.
 * @param L Lua state.
 * @return Number of values pushed (1: integer).
 */
int l_notebook_cell_index(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->NotebookCellAtCursor());
    return 1;
}

/**
 * @brief Implements mep.notebook_cell_count(): how many cells the current notebook buffer's text has.
 * @param L Lua state.
 * @return Number of values pushed (1: integer, 0 for a non-notebook buffer).
 */
int l_notebook_cell_count(lua_State *L) {
    Editor *ed = GetEditor(L);
    const NotebookSession *sess = ed->NotebookRefresh(ed->CurrentBufferId());
    lua_pushinteger(L, sess ? static_cast<lua_Integer>(sess->spans.size()) : 0);
    return 1;
}

/**
 * @brief Implements mep.notebook_goto_cell(index): moves the cursor to a cell's first body row (index clamped).
 * @param L Lua state; arg 1 is a 0-based cell index.
 * @return Number of values pushed (1: true if the cursor moved).
 */
int l_notebook_goto_cell(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->NotebookGotoCell(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

/**
 * @brief Implements mep.notebook_set_python(command): sets the interpreter notebook kernels launch with (default "python3").
 * @param L Lua state; arg 1 is the executable name or path.
 * @return Number of values pushed (0).
 */
int l_notebook_set_python(lua_State *L) {
    GetEditor(L)->SetNotebookPython(luaL_checkstring(L, 1));
    return 0;
}

namespace {
// Reads a kernel "mode" string into the enum (default Script -- the
// safest for an unknown value, since a stateless run can't corrupt a
// long-lived process's namespace).
NotebookKernelSpec::Mode NotebookModeFromString(const char *s) {
    std::string m = s ? s : "";
    if (m == "python") return NotebookKernelSpec::Mode::Python;
    if (m == "protocol") return NotebookKernelSpec::Mode::Protocol;
    return NotebookKernelSpec::Mode::Script;
}
}  // namespace

/**
 * @brief Implements mep.notebook_set_kernels(list): replaces the code-cell kernel registry.
 * @param L Lua state; arg 1 is an array of {name=, display_name=, language=, command=<array>, mode=} tables.
 * @return Number of values pushed (0).
 */
int l_notebook_set_kernels(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<NotebookKernelSpec> specs;
    lua_Integer n = luaL_len(L, 1);
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_istable(L, -1)) {
            NotebookKernelSpec spec;
            lua_getfield(L, -1, "name");
            spec.name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            lua_getfield(L, -1, "display_name");
            spec.display_name = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            lua_getfield(L, -1, "language");
            spec.language = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            lua_getfield(L, -1, "mode");
            spec.mode = NotebookModeFromString(lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr);
            lua_pop(L, 1);
            lua_getfield(L, -1, "command");
            if (lua_istable(L, -1)) {
                lua_Integer cn = luaL_len(L, -1);
                for (lua_Integer c = 1; c <= cn; c++) {
                    lua_rawgeti(L, -1, c);
                    if (lua_isstring(L, -1)) spec.command.emplace_back(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            } else if (lua_isstring(L, -1)) {
                spec.command.emplace_back(lua_tostring(L, -1));
            }
            lua_pop(L, 1);
            specs.push_back(std::move(spec));
        }
        lua_pop(L, 1);
    }
    GetEditor(L)->SetNotebookKernels(std::move(specs));
    return 0;
}

/**
 * @brief Implements mep.notebook_kernels(): array of {name=, display_name=, language=} for every registered kernel.
 * @param L Lua state.
 * @return Number of values pushed (1).
 */
int l_notebook_kernels(lua_State *L) {
    const std::vector<NotebookKernelSpec> &specs = GetEditor(L)->NotebookKernels();
    lua_createtable(L, static_cast<int>(specs.size()), 0);
    for (size_t i = 0; i < specs.size(); i++) {
        lua_newtable(L);
        lua_pushstring(L, specs[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, specs[i].display_name.c_str());
        lua_setfield(L, -2, "display_name");
        lua_pushstring(L, specs[i].language.c_str());
        lua_setfield(L, -2, "language");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

/**
 * @brief Implements mep.notebook_cell_kernel(index?): the kernel name a cell effectively runs on (its own or the default).
 * @param L Lua state; optional arg 1 is a 0-based cell index (default: the cursor's cell).
 * @return Number of values pushed (1: string, or nil outside a notebook).
 */
int l_notebook_cell_kernel(lua_State *L) {
    Editor *ed = GetEditor(L);
    std::string name = ed->NotebookCellKernelName(ed->CurrentBufferId(), NotebookIndexArg(L, 1));
    if (name.empty() && !ed->IsNotebookBuffer(ed->CurrentBufferId())) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, name.c_str());
    }
    return 1;
}

/**
 * @brief Implements mep.notebook_set_cell_kernel(name, index?): sets which kernel a cell runs on.
 * @param L Lua state; arg 1 is the kernel name (or "" to follow the notebook default), optional arg 2 a 0-based cell index.
 * @return Number of values pushed (1: true on success).
 */
int l_notebook_set_cell_kernel(lua_State *L) {
    Editor *ed = GetEditor(L);
    const char *name = luaL_optstring(L, 1, "");
    lua_pushboolean(L, ed->NotebookSetCellKernel(ed->CurrentBufferId(), NotebookIndexArg(L, 2), name));
    return 1;
}

/**
 * @brief Implements mep.notebook_default_kernel(): the kernel name cells run on when they pick none.
 * @param L Lua state.
 * @return Number of values pushed (1: string, or nil outside a notebook).
 */
int l_notebook_default_kernel(lua_State *L) {
    Editor *ed = GetEditor(L);
    std::string name = ed->NotebookDefaultKernelName(ed->CurrentBufferId());
    if (name.empty()) lua_pushnil(L);
    else lua_pushstring(L, name.c_str());
    return 1;
}

/**
 * @brief Implements mep.notebook_cell_language(row): the treesitter filetype for the cell containing a 1-based buffer row.
 * @param L Lua state; arg 1 is a 1-based buffer row.
 * @return Number of values pushed (1: string like "py"/"r"/"md", or nil for none).
 */
int l_notebook_cell_language(lua_State *L) {
    Editor *ed = GetEditor(L);
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;   // Lua rows are 1-based
    std::string lang = ed->NotebookCellLanguageAtRow(ed->CurrentBufferId(), row);
    if (lang.empty()) lua_pushnil(L);
    else lua_pushstring(L, lang.c_str());
    return 1;
}

// Returns the current code cell's source bounds and its selected kernel
// language. Bounds are 1-based and end-exclusive, matching replace_lines.
int l_notebook_lsp_context(lua_State *L) {
    Editor *ed = GetEditor(L);
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int first = 0, end = 0;
    std::string language;
    if (!ed->NotebookCellLspContext(ed->CurrentBufferId(), row, &first, &end, &language)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, first + 1); lua_setfield(L, -2, "first_row");
    lua_pushinteger(L, end + 1); lua_setfield(L, -2, "end_row");
    lua_pushstring(L, language.c_str()); lua_setfield(L, -2, "language");
    return 1;
}

/**
 * @brief Implements mep.notebook_status(): {status=, python=, cells=, running=, queued=} for the current notebook, or nil.
 * @param L Lua state.
 * @return Number of values pushed (1).
 */
int l_notebook_status(lua_State *L) {
    Editor *ed = GetEditor(L);
    const NotebookSession *sess = ed->NotebookRefresh(ed->CurrentBufferId());
    if (!sess) {
        lua_pushnil(L);
        return 1;
    }
    // The reported status/python/error describe the notebook's default
    // kernel (what a cell runs on unless it picks another); `running`/
    // `queued` are notebook-wide since runs are sequential across kernels.
    std::string default_kernel = ed->NotebookDefaultKernelName(ed->CurrentBufferId());
    const NotebookSession::KernelProc *kp = ed->NotebookKernelState(ed->CurrentBufferId(), default_kernel);
    lua_newtable(L);
    lua_pushstring(L, kp ? kp->status.c_str() : "not started");
    lua_setfield(L, -2, "status");
    lua_pushstring(L, kp ? kp->version.c_str() : "");
    lua_setfield(L, -2, "python");
    lua_pushstring(L, default_kernel.c_str());
    lua_setfield(L, -2, "kernel");
    lua_pushinteger(L, static_cast<lua_Integer>(sess->doc.cells.size()));
    lua_setfield(L, -2, "cells");
    lua_pushboolean(L, sess->running_uid != 0);
    lua_setfield(L, -2, "running");
    lua_pushinteger(L, static_cast<lua_Integer>(sess->run_queue.size()));
    lua_setfield(L, -2, "queued");
    lua_pushstring(L, kp ? kp->last_error.c_str() : "");
    lua_setfield(L, -2, "error");
    return 1;
}

/**
 * @brief Implements mep.notebook_cell_outputs(index): array of {kind=, name=, text=, has_image=, execution_count=} for a cell's
 * outputs (cell_type/execution_count/source on the table too), or nil -- what tests/agents use to read results back.
 * @param L Lua state; arg 1 is a 0-based cell index.
 * @return Number of values pushed (1).
 */
int l_notebook_cell_outputs(lua_State *L) {
    Editor *ed = GetEditor(L);
    const NotebookSession *sess = ed->NotebookRefresh(ed->CurrentBufferId());
    int idx = static_cast<int>(luaL_checkinteger(L, 1));
    if (!sess || idx < 0 || idx >= static_cast<int>(sess->doc.cells.size())) {
        lua_pushnil(L);
        return 1;
    }
    const NotebookCell &cell = sess->doc.cells[static_cast<size_t>(idx)];
    lua_newtable(L);
    lua_pushstring(L, cell.type == NotebookCellType::Code ? "code" : cell.type == NotebookCellType::Markdown ? "markdown" : "raw");
    lua_setfield(L, -2, "cell_type");
    lua_pushinteger(L, cell.execution_count);
    lua_setfield(L, -2, "execution_count");
    lua_pushstring(L, cell.source.c_str());
    lua_setfield(L, -2, "source");
    lua_pushstring(L, cell.run_state == NotebookCell::RunState::Idle      ? "idle"
                      : cell.run_state == NotebookCell::RunState::Queued ? "queued"
                                                                          : "running");
    lua_setfield(L, -2, "run_state");
    lua_newtable(L);
    int n = 0;
    for (const NotebookOutput &o : cell.outputs) {
        lua_newtable(L);
        const char *kind = o.kind == NotebookOutput::Kind::Stream          ? "stream"
                           : o.kind == NotebookOutput::Kind::ExecuteResult ? "execute_result"
                           : o.kind == NotebookOutput::Kind::DisplayData   ? "display_data"
                                                                           : "error";
        lua_pushstring(L, kind);
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, o.name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, o.text.c_str());
        lua_setfield(L, -2, "text");
        lua_pushboolean(L, !o.image_png.empty());
        lua_setfield(L, -2, "has_image");
        lua_pushinteger(L, o.execution_count);
        lua_setfield(L, -2, "execution_count");
        lua_pushstring(L, o.ename.c_str());
        lua_setfield(L, -2, "ename");
        lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "outputs");
    return 1;
}

int l_job_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv.emplace_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }

    std::string cwd;
    int on_stdout_ref = LUA_NOREF, on_stderr_ref = LUA_NOREF, on_exit_ref = LUA_NOREF;
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "cwd");
        if (lua_isstring(L, -1)) cwd = lua_tostring(L, -1);
        lua_pop(L, 1);
        on_stdout_ref = RefField(L, 2, "on_stdout");
        on_stderr_ref = RefField(L, 2, "on_stderr");
        on_exit_ref = RefField(L, 2, "on_exit");
    }

    // WORKSPACES_PLAN.md decision 4 / "Risks": the safe default is the
    // active workspace root, not the process cwd -- an async job started
    // just before a workspace switch then still runs where it was asked.
    if (cwd.empty()) cwd = GetEditor(L)->ActiveRoot();

    LuaEnv *env = GetLuaEnv(L);
    JobManager::Callbacks cb;
    if (on_stdout_ref != LUA_NOREF) {
        /**
         * @brief Forwards a line of the job's stdout to the registered Lua on_stdout callback.
         * @param line One line of stdout output.
         */
        cb.on_stdout = [env, on_stdout_ref](const std::string &line) { env->CallRefWithString(on_stdout_ref, line); };
    }
    if (on_stderr_ref != LUA_NOREF) {
        /**
         * @brief Forwards a line of the job's stderr to the registered Lua on_stderr callback.
         * @param line One line of stderr output.
         */
        cb.on_stderr = [env, on_stderr_ref](const std::string &line) { env->CallRefWithString(on_stderr_ref, line); };
    }
    /**
     * @brief Invokes the registered Lua on_exit callback with the job's exit code, then releases all three callback refs.
     * @param code Process exit code (-1 if killed or never started).
     */
    cb.on_exit = [env, on_stdout_ref, on_stderr_ref, on_exit_ref](int code) {
        if (on_exit_ref != LUA_NOREF) env->CallRefWithInt(on_exit_ref, code);
        if (on_stdout_ref != LUA_NOREF) env->UnrefFunction(on_stdout_ref);
        if (on_stderr_ref != LUA_NOREF) env->UnrefFunction(on_stderr_ref);
        if (on_exit_ref != LUA_NOREF) env->UnrefFunction(on_exit_ref);
    };

    int id = JobManager::Instance().Spawn(argv, cwd, std::move(cb));
    lua_pushinteger(L, id);
    return 1;
}

/**
 * @brief Implements mep.job_write(id, text): writes text to a running job's stdin.
 * @param L Lua state; arg 1 is the job id, arg 2 the text to write.
 * @return Number of values pushed (1: true if the write succeeded).
 */
int l_job_write(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    size_t len = 0;
    const char *s = luaL_checklstring(L, 2, &len);
    bool ok = JobManager::Instance().WriteStdin(id, std::string(s, len));
    lua_pushboolean(L, ok);
    return 1;
}

/**
 * @brief Implements mep.job_close_stdin(id): closes a running job's stdin pipe.
 * @param L Lua state; arg 1 is the job id.
 * @return Number of values pushed (0).
 */
int l_job_close_stdin(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    JobManager::Instance().CloseStdin(id);
    return 0;
}

/**
 * @brief Implements mep.job_kill(id): terminates a running job.
 * @param L Lua state; arg 1 is the job id.
 * @return Number of values pushed (0).
 */
int l_job_kill(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    JobManager::Instance().Kill(id);
    return 0;
}

/**
 * @brief Implements mep.job_is_running(id): reports whether a job is still running.
 * @param L Lua state; arg 1 is the job id.
 * @return Number of values pushed (1: true if the job is still running).
 */
int l_job_is_running(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, JobManager::Instance().IsRunning(id));
    return 1;
}

// mep.ui_input(title, default_text, on_done [, opts]): vim.ui.input
// equivalent. on_done(text) on Enter, on_done() [nil] on Escape.
// opts is optional: {masked = true} (alias: {password = true}) renders
// the typed text as '*' in the prompt overlay -- for sensitive input
// like an API key -- while on_done still receives the real, unmasked
// text (the mask is a display-only substitution in main.cpp's
// DrawPromptOverlay, never applied to the underlying buffer).
/**
 * @brief Implements mep.ui_input(title, default_text, on_done [, opts]): shows a text prompt overlay and calls on_done with the entered text (or nothing on Escape).
 * @param L Lua state; arg 1 is the prompt title, arg 2 the default text, arg 3 the callback, optional arg 4 a table with masked/password.
 * @return Number of values pushed (0).
 */
int l_ui_input(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    const char *def = luaL_optstring(L, 2, "");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    bool masked = false;
    if (lua_gettop(L) >= 4 && lua_istable(L, 4)) {
        lua_getfield(L, 4, "masked");
        if (lua_toboolean(L, -1)) masked = true;
        lua_pop(L, 1);
        lua_getfield(L, 4, "password");
        if (lua_toboolean(L, -1)) masked = true;
        lua_pop(L, 1);
    }
    GetEditor(L)->BeginPrompt(title, def, ref, masked);
    return 0;
}

// mep.ui_confirm(message, default_yes, on_done): on_done(true/false) always.
/**
 * @brief Implements mep.ui_confirm(message, default_yes, on_done): shows a yes/no confirmation prompt and calls on_done with the choice.
 * @param L Lua state; arg 1 is the message, arg 2 the default choice, arg 3 the callback.
 * @return Number of values pushed (0).
 */
int l_ui_confirm(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    bool default_yes = lua_toboolean(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->BeginConfirm(msg, default_yes, ref);
    return 0;
}

// mep.ui_select(items, title, on_done): vim.ui.select equivalent (a
// simpler fixed-list chooser, distinct from the fuzzy mep.picker widget).
// on_done(1-indexed index) on Enter, on_done() [nil] on Escape.
/**
 * @brief Implements mep.ui_select(items, title, on_done): shows a fixed-list chooser and calls on_done with the chosen 1-indexed index (or nothing on Escape).
 * @param L Lua state; arg 1 is an array of item strings, arg 2 the title, arg 3 the callback.
 * @return Number of values pushed (0).
 */
int l_ui_select(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *title = luaL_optstring(L, 2, "");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    std::vector<std::string> items;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        items.emplace_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->BeginSelect(title, std::move(items), ref);
    return 0;
}

// mep.float_preview(title, text): shows `text` (may embed '\n's, one
// line each) in a centered floating box -- DrawFloatFrame's box style,
// main.cpp's DrawPreviewOverlay -- dismissed by any keypress or a click.
// Read-only/informational, unlike ui_input/ui_confirm/ui_select above:
// no callback, nothing to decide. Added for git-gutter's hunk preview
// (NVIM_PARITY_PLAN.md Phase 17 gap) but deliberately generic/reusable.
/**
 * @brief Implements mep.float_preview(title, text): shows read-only text in a centered floating box, dismissed by any keypress or click.
 * @param L Lua state; arg 1 is the box title, arg 2 the text to show (may contain '\n').
 * @return Number of values pushed (0).
 */
int l_float_preview(lua_State *L) {
    const char *title = luaL_optstring(L, 1, "");
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    GetEditor(L)->BeginPreview(title, std::string(text, len));
    return 0;
}

// mep.buffer_delete(id, force?): drops buffer `id` (:bdelete's
// Editor::BufferDeleteById -- panes showing it fall back to another
// buffer). `force` (default false) discards unsaved changes. Used by the
// git panel to drop the COMMIT_EDITMSG buffer once its float has closed.
/**
 * @brief Implements mep.buffer_delete(id, force?): deletes a buffer by id.
 * @param L Lua state; arg 1 buffer id, optional arg 2 force (discard unsaved changes).
 * @return Number of values pushed (0).
 */
int l_buffer_delete(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool force = lua_toboolean(L, 2) != 0;
    GetEditor(L)->BufferDeleteForLua(id, force);
    return 0;
}

// mep.buffer_set_drag_resolver(buffer_id, fn): registers `fn(row) -> path
// or nil` as this buffer's "drag a row out onto a pane" hook (see
// Editor::SetBufferDragResolver's own comment, editor.h) -- e.g. the file
// tree buffer's row-to-path parser, or the Buffers sidebar's row-to-
// filename lookup, restoring/extending the drag-and-drop-to-open gesture
// for buffers that aren't a docked SidebarInstance (which gets it for
// free from g_sidebar_row_rects/SidebarLineWidgetId already). Pass fn as
// nil/false to unregister.
/**
 * @brief Implements mep.buffer_set_drag_resolver(buffer_id, fn): registers a row->path resolver so a buffer's own rows can be dragged onto a pane to open.
 * @param L Lua state; arg 1 buffer id, arg 2 the resolver function (or nil/false to unregister).
 * @return Number of values pushed (0).
 */
int l_buffer_set_drag_resolver(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int ref = 0;
    if (lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    GetEditor(L)->SetBufferDragResolver(id, ref);
    return 0;
}

// mep.float_open(path, line?, opts?) -> bool: opens `path` in a floating
// editable pane centered over everything (Editor::OpenFloatPane; drawn by
// main.cpp's DrawFloatPane), cursor on 1-based `line` (default 1). Unlike
// float_preview below this is a real Pane over a real buffer -- every
// editing mode, :w, the mouse wheel all work inside it -- and it closes
// on Escape with nothing pending, :q/:close/:wq, or a click outside the
// box. opts.save_on_close (default true) writes the buffer on close if
// it was modified. Opened from a focused sidebar (the Todo panel's 'e'),
// closing returns focus to that sidebar row.
/**
 * @brief Implements mep.float_open(path, line?, opts?): opens a file in a floating editable pane.
 * @param L Lua state; arg 1 is the path, optional arg 2 the 1-based line, optional arg 3 an options table ({save_on_close=bool}).
 * @return Number of values pushed (1: true if the float opened).
 */
int l_float_open(lua_State *L) {
    size_t len = 0;
    const char *path = luaL_checklstring(L, 1, &len);
    int line = static_cast<int>(luaL_optinteger(L, 2, 1));
    bool save_on_close = true;
    int on_close_ref = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "save_on_close");
        if (!lua_isnil(L, -1)) save_on_close = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        // opts.on_close(saved): fired once the float is gone, `saved` =
        // this close wrote the buffer (so "opened, typed nothing, Escape"
        // arrives as false -- the git commit flow's cancel).
        lua_getfield(L, 3, "on_close");
        if (lua_isfunction(L, -1)) {
            on_close_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pushboolean(L, GetEditor(L)->OpenFloatPane(std::string(path, len), line - 1, save_on_close, on_close_ref));
    return 1;
}

/**
 * @brief Implements mep.float_close(): closes the floating editable pane if one is open.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_float_close(lua_State *L) {
    GetEditor(L)->CloseFloatPane();
    return 0;
}

/**
 * @brief Implements mep.float_is_open(): reports whether a floating editable pane is open.
 * @param L Lua state.
 * @return Number of values pushed (1: boolean).
 */
int l_float_is_open(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsFloatPaneOpen());
    return 1;
}

// mep.hover_show(title, text): a small floating window anchored near the
// cursor (NVIM_PARITY_PLAN.md Phase 3's "hover tooltip" gap, closed) --
// unlike float_preview above, this does *not* take over input/mode: it's
// a passive overlay (Editor::hover_open_) that main.cpp draws on top of
// whatever mode is active, auto-dismissed by Editor::MaybeDismissHover
// the moment the cursor moves, Escape is pressed, or Normal mode is left.
// First real consumer: mep.lsp_hover() (main.cpp's kBuiltinLsp), which
// previously only surfaced hover text via mep.notify (a toast, not a
// floating popup).
/**
 * @brief Implements mep.hover_show(title, text): shows a passive floating tooltip anchored near the cursor.
 * @param L Lua state; arg 1 is the title, arg 2 the text to show.
 * @return Number of values pushed (0).
 */
int l_hover_show(lua_State *L) {
    const char *title = luaL_optstring(L, 1, "");
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    GetEditor(L)->ShowHover(title, std::string(text, len));
    return 0;
}

// mep.hover_close(): dismiss an open hover tooltip early (e.g. a new
// hover request superseding a still-open one).
/**
 * @brief Implements mep.hover_close(): dismisses an open hover tooltip.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_hover_close(lua_State *L) {
    GetEditor(L)->CloseHover();
    return 0;
}

// mep.hover_is_open() -> bool. Lets a caller like mep.lsp_hover tell "K
// opened a fresh popup" from "K was pressed again while one's already
// showing" without keeping its own possibly-stale copy of that state.
/**
 * @brief Implements mep.hover_is_open(): reports whether a hover tooltip is currently showing.
 * @param L Lua state.
 * @return Number of values pushed (1: true if a hover tooltip is open).
 */
int l_hover_is_open(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsHoverOpen());
    return 1;
}

// mep.hover_focus_enter(): moves the cursor into the open hover popup's
// own text (Mode::HoverFocus) so it can be navigated/selected/yanked like
// a normal buffer -- see Mode::HoverFocus's doc comment (editor.h) and
// Editor::HandleHoverFocusInput. No-op if hover isn't open.
/**
 * @brief Implements mep.hover_focus_enter(): moves the cursor into the open hover popup's text so it can be navigated/yanked; a no-op if hover isn't open.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_hover_focus_enter(lua_State *L) {
    GetEditor(L)->EnterHoverFocus();
    return 0;
}

// mep.ns_create(name) -> id. Stable per name (nvim_create_namespace-like).
/**
 * @brief Implements mep.ns_create(name): looks up (or creates) a stable decoration namespace id for a given name.
 * @param L Lua state; arg 1 is the namespace name.
 * @return Number of values pushed (1: the namespace id).
 */
int l_ns_create(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushinteger(L, GetEditor(L)->CreateNamespace(name));
    return 1;
}

/**
 * @brief Implements mep.ns_clear(ns): removes every decoration previously added under a namespace id.
 * @param L Lua state; arg 1 is the namespace id.
 * @return Number of values pushed (0).
 */
int l_ns_clear(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->ClearNamespace(ns);
    return 0;
}

// mep.deco_add(ns, opts) -> id. opts: row (1-indexed, required),
// col_start/col_end (1-indexed, exclusive end), whole_line, hl_group,
// underline (draw a thin underline under [col_start, col_end) using
// hl_group's color instead of recoloring the span's text; ignored if
// whole_line is set), virt_text, virt_text_hl, virt_overlay, sign
// (single-char string), sign_hl, priority.
// Shared by l_deco_add and l_buffer_deco_add (Part VI Phase 27 needed the
// latter -- terminal/Run/REPL output streams into a background buffer
// that isn't necessarily the active pane's).
/**
 * @brief Parses a decoration options table (row, col_start/col_end, styling flags, hl_group, virtual text, sign, priority, color swatch) into a Decoration struct. Shared by l_deco_add and l_buffer_deco_add.
 * @param L Lua state.
 * @param idx Stack index of the options table.
 * @return The populated Decoration.
 */
Decoration ReadDecorationTable(lua_State *L, int idx) {
    Decoration d;
    lua_getfield(L, idx, "row");
    d.row = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
    lua_pop(L, 1);
    lua_getfield(L, idx, "col_start");
    d.col_start = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
    lua_pop(L, 1);
    lua_getfield(L, idx, "col_end");
    d.col_end = static_cast<int>(luaL_optinteger(L, -1, d.col_start + 1)) - 1;
    lua_pop(L, 1);
    lua_getfield(L, idx, "whole_line");
    d.whole_line = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "underline");
    d.underline = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "bold");
    d.bold = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "italic");
    d.italic = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "strikethrough");
    d.strikethrough = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "hl_group");
    if (lua_isstring(L, -1)) d.hl_group = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "virt_text");
    if (lua_isstring(L, -1)) d.virt_text = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "virt_text_hl");
    if (lua_isstring(L, -1)) d.virt_text_hl = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "virt_overlay");
    d.virt_overlay = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "virt_text_eol");
    d.virt_text_eol = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "sign");
    if (lua_isstring(L, -1)) d.sign = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "sign_hl");
    if (lua_isstring(L, -1)) d.sign_hl = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "sign_badge");
    d.sign_badge = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "priority");
    d.priority = static_cast<int>(luaL_optinteger(L, -1, 0));
    lua_pop(L, 1);
    // color = {r, g, b}: a colorizer swatch (Phase 13) -- a literal RGB
    // square drawn at col_start, bypassing the named-highlight-group
    // system (hl_group) entirely, since the point is showing the *exact*
    // parsed color rather than its nearest theme role.
    lua_getfield(L, idx, "color");
    if (lua_istable(L, -1)) {
        d.has_swatch = true;
        lua_rawgeti(L, -1, 1);
        d.swatch_color.r = static_cast<unsigned char>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 2);
        d.swatch_color.g = static_cast<unsigned char>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 3);
        d.swatch_color.b = static_cast<unsigned char>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return d;
}

/**
 * @brief Implements mep.deco_add(ns, opts): adds a decoration (highlight/virtual text/sign) under a namespace and returns its id.
 * @param L Lua state; arg 1 is the namespace id, arg 2 the decoration options table.
 * @return Number of values pushed (1: the new decoration's id).
 */
int l_deco_add(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    Decoration d = ReadDecorationTable(L, 2);
    lua_pushinteger(L, GetEditor(L)->AddDecoration(ns, d));
    return 1;
}

// mep.ts_captures(filetype, text) -> array of {row, col_start, col_end,
// capture} (1-indexed row/col_start, col_end exclusive -- same
// convention mep.deco_add's opts table expects), or nil if no
// Treesitter grammar is vendored for `filetype` (see treesitter.cpp).
// The caller (kBuiltinSyntax's mep.syntax_highlight in main.cpp) maps
// each `capture` name to a highlight group and feeds it straight into
// mep.deco_add, exactly like the hand-rolled lexer it upgrades.
/**
 * @brief Implements mep.ts_captures(filetype, text): runs the vendored Treesitter grammar (if any) for a filetype over source text.
 * @param L Lua state; arg 1 is the filetype, arg 2 the source text.
 * @return Number of values pushed (1: array of {row, col_start, col_end, capture} spans, or nil if no grammar is vendored).
 */
int l_ts_captures(lua_State *L) {
    const char *filetype = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    if (!TreesitterHasGrammar(filetype)) {
        lua_pushnil(L);
        return 1;
    }
    std::vector<TSHighlightSpan> spans = TreesitterHighlight(filetype, std::string(text, len));
    lua_createtable(L, static_cast<int>(spans.size()), 0);
    for (size_t i = 0; i < spans.size(); i++) {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, spans[i].row + 1);
        lua_setfield(L, -2, "row");
        lua_pushinteger(L, spans[i].col_start + 1);
        lua_setfield(L, -2, "col_start");
        lua_pushinteger(L, spans[i].col_end + 1);
        lua_setfield(L, -2, "col_end");
        lua_pushlstring(L, spans[i].capture.c_str(), spans[i].capture.size());
        lua_setfield(L, -2, "capture");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.todo_scan_matches() -> array of {row, col_start, col_end, kw}
// (1-indexed row/col_start, col_end exclusive, same convention as
// mep.ts_captures/mep.deco_add above) for every TODO/FIXME/HACK/NOTE
// occurrence in the current buffer. kBuiltinTodo's mep.todo_mark_buffer
// (main.cpp) maps each match's `kw` to a glyph/hl via the user-configurable
// mep.todoscan_keywords table and calls mep.deco_add -- the buffer scan
// itself lives in Editor::TodoScanMatches (editor.cpp).
/**
 * @brief Implements mep.todo_scan_matches(): scans the current buffer for TODO/FIXME/HACK/NOTE-style markers.
 * @param L Lua state.
 * @return Number of values pushed (1: array of {row, col_start, col_end, kw} matches).
 */
int l_todo_scan_matches(lua_State *L) {
    std::vector<Editor::TodoMatch> matches = GetEditor(L)->TodoScanMatches();
    lua_createtable(L, static_cast<int>(matches.size()), 0);
    for (size_t i = 0; i < matches.size(); i++) {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, matches[i].row + 1);
        lua_setfield(L, -2, "row");
        lua_pushinteger(L, matches[i].col_start + 1);
        lua_setfield(L, -2, "col_start");
        lua_pushinteger(L, matches[i].col_end + 1);
        lua_setfield(L, -2, "col_end");
        lua_pushlstring(L, matches[i].keyword.data(), matches[i].keyword.size());
        lua_setfield(L, -2, "kw");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.dap_toggle_breakpoint(): see Editor::DapToggleBreakpoint.
/**
 * @brief Implements mep.dap_toggle_breakpoint(): toggles a debugger breakpoint at the cursor's current line.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_dap_toggle_breakpoint(lua_State *L) {
    GetEditor(L)->DapToggleBreakpoint();
    return 0;
}

// mep.dap_breakpoint_lines(filename) -> array of 1-indexed line numbers
// (Editor::DapBreakpointLines) -- kBuiltinDap's mep.dap_start (main.cpp)
// wraps each into a DAP {line = ...} object for the setBreakpoints
// request, once its own 'initialized' notification handler fires.
/**
 * @brief Implements mep.dap_breakpoint_lines(filename): lists the breakpoint line numbers set for a file.
 * @param L Lua state; arg 1 is the filename.
 * @return Number of values pushed (1: array of 1-indexed line numbers).
 */
int l_dap_breakpoint_lines(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    std::vector<int> lines = GetEditor(L)->DapBreakpointLines(filename);
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    for (size_t i = 0; i < lines.size(); i++) {
        lua_pushinteger(L, lines[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.termsend_register(source, target) -> bool: see Editor::TermsendRegister
// (notifies + returns false itself if `target` isn't a live terminal buffer).
/**
 * @brief Implements mep.termsend_register(source, target): registers a source buffer's default termsend target.
 * @param L Lua state; arg 1 is the source buffer id, arg 2 the target terminal buffer id.
 * @return Number of values pushed (1: false if target isn't a live terminal buffer).
 */
int l_termsend_register(lua_State *L) {
    int source = static_cast<int>(luaL_checkinteger(L, 1));
    int target = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, GetEditor(L)->TermsendRegister(source, target));
    return 1;
}

// mep.termsend_target(source) -> target buffer id, or nil if none
// registered or its registered target is no longer a live terminal buffer.
/**
 * @brief Implements mep.termsend_target(source): looks up the registered termsend target buffer for a source buffer.
 * @param L Lua state; arg 1 is the source buffer id.
 * @return Number of values pushed (1: the target buffer id, or nil if none registered or the target is no longer a live terminal).
 */
int l_termsend_target(lua_State *L) {
    int source = static_cast<int>(luaL_checkinteger(L, 1));
    int target = GetEditor(L)->TermsendTarget(source);
    if (target <= 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, target);
    return 1;
}

/**
 * @brief Implements mep.termsend_unregister(source): removes a source buffer's registered termsend target.
 * @param L Lua state; arg 1 is the source buffer id.
 * @return Number of values pushed (0).
 */
int l_termsend_unregister(lua_State *L) {
    int source = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->TermsendUnregister(source);
    return 0;
}

// mep.termsend_candidates() -> array of terminal buffer ids currently
// shown by a pane in the active tab (Editor::TermsendCandidates) -- what
// kBuiltinTermSend's registration prompt offers as its default.
/**
 * @brief Implements mep.termsend_candidates(): lists terminal buffer ids currently shown by a pane in the active tab.
 * @param L Lua state.
 * @return Number of values pushed (1: array of terminal buffer ids).
 */
int l_termsend_candidates(lua_State *L) {
    std::vector<int> ids = GetEditor(L)->TermsendCandidates();
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    for (size_t i = 0; i < ids.size(); i++) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.activity_todo_load(path) -> array of {done=bool, text=string,
// line=int, level=int, keyword=string}: see Editor::ActivityTodoLoad.
// `line` is the 1-indexed line of the item's headline in an org file (nil
// for the legacy line format), the identity mep.activity_todo_save uses to
// match an item back to its headline; `level` lets the panel indent
// subtasks.
/**
 * @brief Implements mep.activity_todo_load(path): loads an activity todo list from a file.
 * @param L Lua state; arg 1 is the file path.
 * @return Number of values pushed (1: array of {done=, text=, line=, level=, keyword=} items).
 */
int l_activity_todo_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::vector<Editor::ActivityTodoItem> items = GetEditor(L)->ActivityTodoLoad(path);
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (size_t i = 0; i < items.size(); i++) {
        lua_createtable(L, 0, 5);
        lua_pushboolean(L, items[i].done);
        lua_setfield(L, -2, "done");
        lua_pushlstring(L, items[i].text.data(), items[i].text.size());
        lua_setfield(L, -2, "text");
        if (items[i].line >= 0) {
            lua_pushinteger(L, items[i].line + 1);
            lua_setfield(L, -2, "line");
        }
        lua_pushinteger(L, items[i].level);
        lua_setfield(L, -2, "level");
        lua_pushlstring(L, items[i].keyword.data(), items[i].keyword.size());
        lua_setfield(L, -2, "keyword");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.activity_todo_save(path, items): `items` is an array of {done=,
// text=, line=?} -- `line` (1-indexed, as loaded) ties an item to its org
// headline; an item without one is a new entry to append.
/**
 * @brief Implements mep.activity_todo_save(path, items): writes an activity todo list back to a file.
 * @param L Lua state; arg 1 is the file path, arg 2 an array of {done=, text=, line=?} items.
 * @return Number of values pushed (0).
 */
int l_activity_todo_save(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<Editor::ActivityTodoItem> items;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        Editor::ActivityTodoItem item;
        lua_getfield(L, -1, "done");
        item.done = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "text");
        if (lua_isstring(L, -1)) item.text = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "line");
        if (lua_isinteger(L, -1)) item.line = static_cast<int>(lua_tointeger(L, -1)) - 1;
        lua_pop(L, 1);
        lua_pop(L, 1);  // the item table itself
        items.push_back(std::move(item));
    }
    GetEditor(L)->ActivityTodoSave(path, items);
    return 0;
}

// mep.activity_todo_clock_status(path) -> nil when no clock is open in
// the org file, else {line = 1-indexed headline line (the same identity
// mep.activity_todo_load's items carry), start_ts = the CLOCK stamp's
// text, start = that stamp as local-time Unix seconds, title = the
// headline's title}. See Editor::ActivityTodoClockStatus.
/**
 * @brief Implements mep.activity_todo_clock_status(path): reports the running clock in an org todo file.
 * @param L Lua state; arg 1 is the org file path.
 * @return Number of values pushed (1: a {line, start_ts, start, title} table, or nil when no clock is open).
 */
int l_activity_todo_clock_status(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    Editor::ActivityTodoClock clock = GetEditor(L)->ActivityTodoClockStatus(path);
    if (clock.line < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, clock.line + 1);
    lua_setfield(L, -2, "line");
    lua_pushlstring(L, clock.start_ts.data(), clock.start_ts.size());
    lua_setfield(L, -2, "start_ts");
    lua_pushinteger(L, clock.start_epoch);
    lua_setfield(L, -2, "start");
    lua_pushlstring(L, clock.title.data(), clock.title.size());
    lua_setfield(L, -2, "title");
    return 1;
}

// mep.activity_todo_clock_start(path, line) -> the start's Unix seconds
// (the exact second, where the CLOCK stamp itself only keeps the minute),
// or nil if nothing was written (a clock already open in the file, `line`
// not a headline, or not an org file). `line` is 1-indexed.
/**
 * @brief Implements mep.activity_todo_clock_start(path, line): starts a clock under a headline of an org todo file.
 * @param L Lua state; arg 1 is the org file path, arg 2 the 1-indexed headline line.
 * @return Number of values pushed (1: the start time as Unix seconds, or nil on refusal).
 */
int l_activity_todo_clock_start(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int line = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!GetEditor(L)->ActivityTodoClockStart(path, line)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, now);
    return 1;
}

// mep.activity_todo_clock_stop(path) -> elapsed whole minutes, or nil
// when no clock was open.
/**
 * @brief Implements mep.activity_todo_clock_stop(path): closes the running clock in an org todo file.
 * @param L Lua state; arg 1 is the org file path.
 * @return Number of values pushed (1: the elapsed whole minutes, or nil when no clock was open).
 */
int l_activity_todo_clock_stop(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int minutes = GetEditor(L)->ActivityTodoClockStop(path);
    if (minutes < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, minutes);
    return 1;
}

// mep.activity_todo_retitle(path, line, text) -> bool: rewrites the
// 1-indexed keyworded headline's title, keeping keyword/priority/tags.
/**
 * @brief Implements mep.activity_todo_retitle(path, line, text): retitles a headline of an org todo file.
 * @param L Lua state; arg 1 is the org file path, arg 2 the 1-indexed headline line, arg 3 the new title.
 * @return Number of values pushed (1: true if the headline was rewritten).
 */
int l_activity_todo_retitle(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int line = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    size_t len = 0;
    const char *text = luaL_checklstring(L, 3, &len);
    lua_pushboolean(L, GetEditor(L)->ActivityTodoRetitle(path, line, std::string(text, len)));
    return 1;
}

// mep.activity_todo_archive(path, line) -> bool: tags the 1-indexed
// keyworded headline :ARCHIVE: so the Todo panel stops listing it (and
// its subtree); the headline itself stays in the file untouched otherwise.
/**
 * @brief Implements mep.activity_todo_archive(path, line): archives a headline of an org todo file.
 * @param L Lua state; arg 1 is the org file path, arg 2 the 1-indexed headline line.
 * @return Number of values pushed (1: true if the headline was rewritten).
 */
int l_activity_todo_archive(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int line = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    lua_pushboolean(L, GetEditor(L)->ActivityTodoArchive(path, line));
    return 1;
}

// mep.activity_todo_move(path, line, delta) -> new 1-indexed line, or nil:
// swaps the 1-indexed keyworded headline's whole subtree with its previous
// (delta < 0) or next (delta > 0) sibling, reordering the checklist.
/**
 * @brief Implements mep.activity_todo_move(path, line, delta): reorders a headline of an org todo file among its siblings.
 * @param L Lua state; arg 1 is the org file path, arg 2 the 1-indexed headline line, arg 3 the direction (-1 or 1).
 * @return Number of values pushed (1: the moved headline's new 1-indexed line, or nil if it didn't move).
 */
int l_activity_todo_move(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int line = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    int delta = static_cast<int>(luaL_checkinteger(L, 3));
    int new_line = GetEditor(L)->ActivityTodoMove(path, line, delta);
    if (new_line < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, new_line + 1);
    }
    return 1;
}

// mep.active_todo_set(text, start) / mep.active_todo_set(nil): what the
// status bar's todo chip shows -- `text` with a live timer counting from
// `start` (Unix seconds), or the red "[No active TODO]" state when nil.
/**
 * @brief Implements mep.active_todo_set(text, start): sets (or, with nil, clears) the status bar's active-todo chip.
 * @param L Lua state; arg 1 is the todo title or nil, arg 2 the clock start as Unix seconds (defaults to now).
 * @return Number of values pushed (0).
 */
int l_active_todo_set(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        GetEditor(L)->ClearActiveTodo();
        return 0;
    }
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    long long start = luaL_optinteger(L, 2, static_cast<lua_Integer>(std::time(nullptr)));
    GetEditor(L)->SetActiveTodo(std::string(text, len), start);
    return 0;
}

// mep.setenv(name, value)/mep.unsetenv(name): set or clear a variable in
// mep's own process environment -- kBuiltinDirenv's way of actually
// applying (or reverting) a project's `direnv export json` output. A job
// or terminal spawned afterward (Job's fork()+execvp(), job.cpp) inherits
// the ambient process environment automatically, so nothing else needs to
// plumb this through explicitly; anything already running when this is
// called keeps its own already-inherited copy, same as a real shell.
/**
 * @brief Implements mep.setenv(name, value): sets a variable in this process's environment.
 * @param L Lua state; arg 1 the variable name, arg 2 its value.
 * @return Number of values pushed (0).
 */
int l_setenv(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);
    ::setenv(name, value, 1);
    return 0;
}

/**
 * @brief Implements mep.unsetenv(name): clears a variable from this process's environment.
 * @param L Lua state; arg 1 the variable name.
 * @return Number of values pushed (0).
 */
int l_unsetenv(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    ::unsetenv(name);
    return 0;
}

// mep.direnv_set_active(active): what the status bar's direnv chip shows
// -- see Editor::SetDirenvActive.
/**
 * @brief Implements mep.direnv_set_active(active): sets the status bar's direnv chip state.
 * @param L Lua state; arg 1 whether direnv's environment is currently applied.
 * @return Number of values pushed (0).
 */
int l_direnv_set_active(lua_State *L) {
    bool active = lua_toboolean(L, 1) != 0;
    GetEditor(L)->SetDirenvActive(active);
    return 0;
}

// mep.activity_test_failure_lines(output) -> array of {index, line}
// (1-indexed): see Editor::ActivityTestFailureLines.
/**
 * @brief Implements mep.activity_test_failure_lines(output): scans test-runner output lines for failure locations.
 * @param L Lua state; arg 1 is an array of output line strings.
 * @return Number of values pushed (1: array of {index, line} matches, 1-indexed).
 */
int l_activity_test_failure_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> output;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        output.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    std::vector<Editor::ActivityTestFailureLine> fails = GetEditor(L)->ActivityTestFailureLines(output);
    lua_createtable(L, static_cast<int>(fails.size()), 0);
    for (size_t i = 0; i < fails.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, fails[i].index);
        lua_setfield(L, -2, "index");
        lua_pushlstring(L, fails[i].line.data(), fails[i].line.size());
        lua_setfield(L, -2, "line");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.syntax_highlight_fallback(ns, keywords, comment_prefix): the
// no-vendored-grammar fallback lexer over the *current* buffer -- see
// Editor::SyntaxHighlightFallback. `keywords` is a plain array of
// strings; `comment_prefix` may be nil/omitted (no comment highlighting
// for that filetype).
/**
 * @brief Implements mep.syntax_highlight_fallback(ns, keywords, comment_prefix): runs the no-vendored-grammar fallback lexer over the current buffer.
 * @param L Lua state; arg 1 is the namespace id, arg 2 an array of keyword strings, optional arg 3 a comment prefix string.
 * @return Number of values pushed (0).
 */
int l_syntax_highlight_fallback(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> keywords;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isstring(L, -1)) keywords.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    std::string comment_prefix = (lua_gettop(L) >= 3 && lua_isstring(L, 3)) ? lua_tostring(L, 3) : "";
    GetEditor(L)->SyntaxHighlightFallback(ns, keywords, comment_prefix);
    return 0;
}

// mep.org_highlight_emphasis(ns): org emphasis markup over the *current*
// buffer -- see Editor::OrgHighlightEmphasis.
/**
 * @brief Implements mep.org_highlight_emphasis(ns): highlights org emphasis markup over the current buffer.
 * @param L Lua state; arg 1 is the namespace id.
 * @return Number of values pushed (0).
 */
int l_org_highlight_emphasis(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->OrgHighlightEmphasis(ns);
    return 0;
}

// mep.md_toggle_checkbox()/mep.md_fold()/mep.md_table_align()/
// mep.md_table_insert_row()/mep.md_table_insert_col(): kBuiltinMarkdown's
// (main.cpp) checkbox toggle, fold computation, and GFM table commands --
// see the matching Editor:: methods (editor.cpp) for the ported logic.
// Bound directly under their original mep.* names since each is now
// fully self-contained in C++, with no surrounding Lua glue left to keep.
/**
 * @brief Implements mep.md_toggle_checkbox(): toggles the Markdown checkbox on the current line.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_md_toggle_checkbox(lua_State *L) {
    GetEditor(L)->MdToggleCheckbox();
    return 0;
}
/**
 * @brief Implements mep.md_fold(): computes Markdown heading-based folds for the current buffer.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_md_fold(lua_State *L) {
    GetEditor(L)->MdComputeFolds();
    return 0;
}
/**
 * @brief Implements mep.md_table_align(): reflows the GFM table under the cursor into aligned columns.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_md_table_align(lua_State *L) {
    GetEditor(L)->MdTableAlign();
    return 0;
}
/**
 * @brief Implements mep.md_table_insert_row(): inserts a new row into the GFM table under the cursor.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_md_table_insert_row(lua_State *L) {
    GetEditor(L)->MdTableInsertRow();
    return 0;
}
/**
 * @brief Implements mep.md_table_insert_col(): inserts a new column into the GFM table under the cursor.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_md_table_insert_col(lua_State *L) {
    GetEditor(L)->MdTableInsertCol();
    return 0;
}

// mep.md_conceal_scan(ns): the link/emphasis concealment scan itself
// (Editor::MdConceal) -- kBuiltinMarkdown's mep.md_conceal keeps the
// namespace create/clear and the auto/filetype gating in Lua, calling
// this once those checks pass.
/**
 * @brief Implements mep.md_conceal_scan(ns): runs the link/emphasis concealment scan over the current buffer.
 * @param L Lua state; arg 1 is the namespace id.
 * @return Number of values pushed (0).
 */
int l_md_conceal_scan(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->MdConceal(ns);
    return 0;
}

// mep.completion_scan_buffer_words(prefix) -> array of plain word
// strings: see Editor::CompletionScanBufferWords. The caller
// (kBuiltinCompletion's mep.completion_buffer_words, main.cpp) wraps each
// into {text=w, kind='buffer'} itself, after filtering against whatever
// it's already claimed via snippet trigger names.
/**
 * @brief Implements mep.completion_scan_buffer_words(prefix): scans the current buffer for words starting with a prefix.
 * @param L Lua state; arg 1 is the prefix to match.
 * @return Number of values pushed (1: array of matching word strings).
 */
int l_completion_scan_buffer_words(lua_State *L) {
    const char *prefix = luaL_checkstring(L, 1);
    std::vector<std::string> words = GetEditor(L)->CompletionScanBufferWords(prefix);
    lua_createtable(L, static_cast<int>(words.size()), 0);
    for (size_t i = 0; i < words.size(); i++) {
        lua_pushlstring(L, words[i].data(), words[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.completion_path_prefix(prefix, col, line) -> dir, base or nil: see
// Editor::CompletionPathPrefixFor. `col` is mep.cursor()'s own 1-indexed
// convention.
/**
 * @brief Implements mep.completion_path_prefix(prefix, col, line): detects a path fragment ending at the cursor for path completion.
 * @param L Lua state; arg 1 is the text before the cursor, arg 2 the 1-indexed column, arg 3 the full line.
 * @return Number of values pushed (2: dir and base strings if a path prefix was found; 1: nil otherwise).
 */
int l_completion_path_prefix(lua_State *L) {
    const char *prefix = luaL_checkstring(L, 1);
    int col = static_cast<int>(luaL_checkinteger(L, 2));
    const char *line = luaL_checkstring(L, 3);
    Editor::CompletionPathPrefix r = GetEditor(L)->CompletionPathPrefixFor(prefix, col, line);
    if (!r.found) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, r.dir.data(), r.dir.size());
    lua_pushlstring(L, r.base.data(), r.base.size());
    return 2;
}

// mep.completion_rank(items, max_items): sorts `items` (an array of
// {text=, ...} tables -- any other fields pass through untouched) by
// #text then alphabetically, capped to `max_items`. Preserves the
// original item tables verbatim (no field-shape assumptions beyond
// `text` existing) -- kBuiltinCompletion's own mep_completion_rank port.
/**
 * @brief Implements mep.completion_rank(items, max_items): sorts completion items by text length then alphabetically, capped to a maximum count.
 * @param L Lua state; arg 1 is an array of {text=, ...} item tables, optional arg 2 the maximum number of items to keep.
 * @return Number of values pushed (1: the sorted, capped array of item tables).
 */
int l_completion_rank(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer max_items = luaL_optinteger(L, 2, -1);
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    std::vector<std::pair<std::string, lua_Integer>> keys;
    keys.reserve(static_cast<size_t>(n));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        lua_getfield(L, -1, "text");
        keys.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "", i);
        lua_pop(L, 2);
    }
    // Orders shorter completion text first, then alphabetically among equal lengths.
    std::stable_sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) {
        if (a.first.size() != b.first.size()) return a.first.size() < b.first.size();
        return a.first < b.first;
    });
    lua_Integer out_n = (max_items >= 0 && static_cast<lua_Integer>(keys.size()) > max_items)
                            ? max_items
                            : static_cast<lua_Integer>(keys.size());
    lua_createtable(L, static_cast<int>(out_n), 0);
    for (lua_Integer i = 0; i < out_n; i++) {
        lua_rawgeti(L, 1, keys[static_cast<size_t>(i)].second);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.snippet_splice(row, before, after, body): see Editor::SnippetSplice.
// `row` is mep.cursor()'s own 1-indexed convention (converted here);
// `body` is a plain array of template-line strings.
/**
 * @brief Implements mep.snippet_splice(row, before, after, body): splices a multi-line snippet body into the buffer at a row.
 * @param L Lua state; arg 1 is the 1-indexed row, arg 2 the text before the insertion point, arg 3 the text after it, arg 4 an array of template-line strings.
 * @return Number of values pushed (0).
 */
int l_snippet_splice(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    const char *before = luaL_checkstring(L, 2);
    const char *after = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    std::vector<std::string> body;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 4));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 4, i);
        body.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    GetEditor(L)->SnippetSplice(row, before, after, body);
    return 0;
}

// mep.snippet_jump(delta): see Editor::SnippetJump. Bound directly under
// its original name -- MepSnippetNext/MepSnippetPrev call it unchanged.
/**
 * @brief Implements mep.snippet_jump(delta): moves to the next/previous snippet tab-stop.
 * @param L Lua state; arg 1 is the jump direction/count.
 * @return Number of values pushed (0).
 */
int l_snippet_jump(lua_State *L) {
    int delta = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->SnippetJump(delta);
    return 0;
}


// mep.ts_apply_captures(ns, captures, hl_map, row_offset): resolves each
// capture's highlight group via hl_map (falling back to the capture's
// first dot-segment, e.g. 'function.builtin' -> 'function', mirroring
// nvim's own capture-group fallback convention) and adds a decoration
// for it in `ns`, with `row_offset` (default 0) added to each capture's
// row -- the capture-resolve loop kBuiltinSyntax's mep.syntax_highlight
// and its org-src-block-embedded highlighting both used to repeat by
// hand (a local mep_ts_resolve_hl + mep.deco_add). `captures` is
// mep.ts_captures's own 1-indexed output shape; `row_offset` is added
// post-1-indexing (0 for a top-level call, a block's header row for an
// org-embedded one).
/**
 * @brief Implements mep.ts_apply_captures(ns, captures, hl_map, row_offset): resolves each Treesitter capture to a highlight group via hl_map and adds a decoration for it.
 * @param L Lua state; arg 1 is the namespace id, arg 2 an array of capture spans (mep.ts_captures's shape), arg 3 a capture-name-to-highlight-group table, optional arg 4 a row offset added to each capture's row (default 0).
 * @return Number of values pushed (0).
 */
int l_ts_apply_captures(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    int row_offset = static_cast<int>(luaL_optinteger(L, 4, 0));

    std::unordered_map<std::string, std::string> hl_map;
    lua_pushnil(L);
    while (lua_next(L, 3) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
            hl_map[lua_tostring(L, -2)] = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    Editor *ed = GetEditor(L);
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "row");
        int row = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
        lua_pop(L, 1);
        lua_getfield(L, -1, "col_start");
        int col_start = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
        lua_pop(L, 1);
        lua_getfield(L, -1, "col_end");
        int col_end = static_cast<int>(luaL_optinteger(L, -1, col_start + 1)) - 1;
        lua_pop(L, 1);
        lua_getfield(L, -1, "capture");
        std::string capture = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        lua_pop(L, 1);  // the capture table itself

        auto it = hl_map.find(capture);
        if (it == hl_map.end()) {
            size_t dot = capture.find('.');
            std::string base = dot == std::string::npos ? capture : capture.substr(0, dot);
            it = hl_map.find(base);
        }
        if (it != hl_map.end()) {
            Decoration d;
            d.row = row + row_offset;
            d.col_start = col_start;
            d.col_end = col_end;
            d.hl_group = it->second;
            ed->AddDecoration(ns, d);
        }
    }
    return 0;
}

// mep.ts_fold_ranges(filetype, text) -> array of {start_row, end_row}
// (1-indexed, inclusive -- mep.fold_create's own convention), or nil if
// `filetype` has no Treesitter fold query (see treesitter_queries.h's
// kFolds* -- only the core compiled-in languages with real block/body
// grammar nodes have one; markdown/org keep their own heading-based fold
// providers instead). The caller (kBuiltinSyntax's mep.syntax_fold in
// main.cpp) feeds each range straight into mep.fold_create under a
// dedicated 'treesitter' provider.
/**
 * @brief Implements mep.ts_fold_ranges(filetype, text): runs the vendored Treesitter fold query (if any) over source text.
 * @param L Lua state; arg 1 is the filetype, arg 2 the source text.
 * @return Number of values pushed (1: array of {start_row, end_row} 1-indexed inclusive ranges, or nil if no fold query exists for filetype).
 */
int l_ts_fold_ranges(lua_State *L) {
    const char *filetype = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    if (!TreesitterHasFoldQuery(filetype)) {
        lua_pushnil(L);
        return 1;
    }
    std::vector<TSFoldRange> ranges = TreesitterFoldRanges(filetype, std::string(text, len));
    lua_createtable(L, static_cast<int>(ranges.size()), 0);
    for (size_t i = 0; i < ranges.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, ranges[i].start_row + 1);
        lua_setfield(L, -2, "start_row");
        lua_pushinteger(L, ranges[i].end_row + 1);
        lua_setfield(L, -2, "end_row");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.ts_structure(filetype, text) -> array of {row, col, start_row,
// end_row, name, kind, depth} (row/col 1-indexed, matching mep.set_cursor's
// convention; start_row/end_row also 1-indexed for the same reason
// mep.ts_fold_ranges' is), in document order, or nil if `filetype` has no
// Treesitter structure query (treesitter_structure_queries.h's own curated
// language set -- narrower than TreesitterHasGrammar's, see that header's
// top comment). The caller (kBuiltinStructure's mep.structure_refresh,
// main.cpp) builds either a scratch-buffer outline or a sidebar section
// from this directly; `depth` is already nesting-resolved (TreesitterStructure's
// own containment-stack pass), so the Lua side never recomputes it.
// `start_row`/`end_row` together give the definition's real span, used by
// kBuiltinStructure's own "which item is the cursor in/near" tracking --
// `row` alone (the name token) isn't enough for containment since it's
// often not the first line of a multi-line signature.
/**
 * @brief Implements mep.ts_structure(filetype, text): runs the vendored Treesitter structure query (if any) over source text to build a document outline.
 * @param L Lua state; arg 1 is the filetype, arg 2 the source text.
 * @return Number of values pushed (1: array of {row, col, start_row, end_row, name, kind, depth} nodes in document order, or nil if no structure query exists for filetype).
 */
int l_ts_structure(lua_State *L) {
    const char *filetype = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    if (!TreesitterHasStructureQuery(filetype)) {
        lua_pushnil(L);
        return 1;
    }
    std::vector<TSStructureNode> nodes = TreesitterStructure(filetype, std::string(text, len));
    lua_createtable(L, static_cast<int>(nodes.size()), 0);
    for (size_t i = 0; i < nodes.size(); i++) {
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, nodes[i].row + 1);
        lua_setfield(L, -2, "row");
        lua_pushinteger(L, nodes[i].col + 1);
        lua_setfield(L, -2, "col");
        lua_pushinteger(L, nodes[i].start_row + 1);
        lua_setfield(L, -2, "start_row");
        lua_pushinteger(L, nodes[i].end_row + 1);
        lua_setfield(L, -2, "end_row");
        lua_pushlstring(L, nodes[i].name.data(), nodes[i].name.size());
        lua_setfield(L, -2, "name");
        lua_pushlstring(L, nodes[i].kind.data(), nodes[i].kind.size());
        lua_setfield(L, -2, "kind");
        lua_pushinteger(L, nodes[i].depth);
        lua_setfield(L, -2, "depth");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.buffer_set_lines(buffer_id, lines) (Part VI Phase 27).
/**
 * @brief Implements mep.buffer_set_lines(buffer_id, lines): replaces the entire contents of a (possibly background) buffer.
 * @param L Lua state; arg 1 is the buffer id, arg 2 an array of replacement line strings.
 * @return Number of values pushed (0).
 */
int l_buffer_set_lines(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> lines;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        lines.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    GetEditor(L)->SetBufferLinesForLua(buffer_id, lines);
    return 0;
}

// mep.buffer_get_lines(buffer_id) -> array of a specific (possibly
// background) buffer's lines, or nil for an out-of-range id --
// mep.buffer_set_lines' read-side counterpart. kBuiltinFileTree's oil-style
// directory buffers are the first caller: their :w hook can fire for a
// buffer that isn't the active pane's (:wa), so mep.get_line won't do.
/**
 * @brief Implements mep.buffer_get_lines(buffer_id): returns a buffer's lines.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: the array of lines, or nil).
 */
int l_buffer_get_lines(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    Editor *ed = GetEditor(L);
    if (buffer_id < 0 || buffer_id >= ed->BufferCountForLua()) {
        lua_pushnil(L);
        return 1;
    }
    const std::vector<std::string> &lines = ed->GetBuffer(buffer_id).lines;
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    for (size_t i = 0; i < lines.size(); i++) {
        lua_pushlstring(L, lines[i].data(), lines[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

/**
 * @brief Implements mep.buffer_ns_clear(buffer_id, ns): clears every decoration under a namespace in a specific (possibly background) buffer.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the namespace id.
 * @return Number of values pushed (0).
 */
int l_buffer_ns_clear(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int ns = static_cast<int>(luaL_checkinteger(L, 2));
    GetEditor(L)->ClearNamespaceInBuffer(buffer_id, ns);
    return 0;
}

/**
 * @brief Implements mep.buffer_deco_add(buffer_id, ns, opts): adds a decoration to a specific (possibly background) buffer.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the namespace id, arg 3 the decoration options table.
 * @return Number of values pushed (1: the new decoration's id).
 */
int l_buffer_deco_add(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int ns = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    Decoration d = ReadDecorationTable(L, 3);
    lua_pushinteger(L, GetEditor(L)->AddDecorationToBuffer(buffer_id, ns, d));
    return 1;
}

// mep.buffer_new() -> buffer id, an empty buffer *not* switched to (Part
// VI Phase 27: a dedicated Run/REPL output buffer, created before the
// caller `:split`s a pane and `mep.buffer_switch`es it into view there).
/**
 * @brief Implements mep.buffer_new(): creates a new empty buffer without switching any pane to it.
 * @param L Lua state.
 * @return Number of values pushed (1: the new buffer's id).
 */
int l_buffer_new(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->CreateBufferForLua());
    return 1;
}

// mep.term_start(argv, opts) -> job id. Like mep.job_start, but spawns
// against a real PTY (Part VI Phase 27) instead of plain pipes -- opts
// may set `cwd`, `on_stdout_raw`, `on_exit`, same shape as job_start's
// opts otherwise.
/**
 * @brief Implements mep.term_start(argv, opts): spawns a subprocess against a real PTY (raw stdout only, no separate stderr stream) instead of plain pipes.
 * @param L Lua state; arg 1 is an array of argv strings, optional arg 2 a table with cwd/on_stdout_raw/on_exit.
 * @return Number of values pushed (1: the new job's id).
 */
int l_term_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv.emplace_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    std::string cwd;
    int on_stdout_raw_ref = LUA_NOREF, on_exit_ref = LUA_NOREF;
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "cwd");
        if (lua_isstring(L, -1)) cwd = lua_tostring(L, -1);
        lua_pop(L, 1);
        on_stdout_raw_ref = RefField(L, 2, "on_stdout_raw");
        on_exit_ref = RefField(L, 2, "on_exit");
    }
    LuaEnv *env = GetLuaEnv(L);
    JobManager::Callbacks cb;
    if (on_stdout_raw_ref != LUA_NOREF) {
        /**
         * @brief Forwards a raw chunk of the PTY's combined stdout/stderr output to the registered Lua on_stdout_raw callback.
         * @param chunk Raw bytes read from the PTY.
         */
        cb.on_stdout_raw = [env, on_stdout_raw_ref](const std::string &chunk) {
            env->CallRefWithString(on_stdout_raw_ref, chunk);
        };
    }
    /**
     * @brief Invokes the registered Lua on_exit callback with the job's exit code, then releases both callback refs.
     * @param code Process exit code.
     */
    cb.on_exit = [env, on_stdout_raw_ref, on_exit_ref](int code) {
        if (on_exit_ref != LUA_NOREF) env->CallRefWithInt(on_exit_ref, code);
        if (on_stdout_raw_ref != LUA_NOREF) env->UnrefFunction(on_stdout_raw_ref);
        if (on_exit_ref != LUA_NOREF) env->UnrefFunction(on_exit_ref);
    };
    int id = JobManager::Instance().Spawn(argv, cwd, std::move(cb), /*use_pty=*/true);
    lua_pushinteger(L, id);
    return 1;
}

/**
 * @brief Implements mep.term_resize(id, cols, rows): resizes a PTY-backed job's terminal dimensions.
 * @param L Lua state; arg 1 is the job id, arg 2 the column count, arg 3 the row count.
 * @return Number of values pushed (0).
 */
int l_term_resize(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int cols = static_cast<int>(luaL_checkinteger(L, 2));
    int rows = static_cast<int>(luaL_checkinteger(L, 3));
    JobManager::Instance().ResizePty(id, cols, rows);
    return 0;
}

// mep.fold_create(start_row, end_row, closed, provider) -- rows 1-indexed.
/**
 * @brief Implements mep.fold_create(start_row, end_row, closed, provider): creates a fold over a line range.
 * @param L Lua state; arg 1 is the 1-indexed start row, arg 2 the 1-indexed end row, optional arg 3 the initial closed state (default true), optional arg 4 the provider name (default "manual").
 * @return Number of values pushed (0).
 */
int l_fold_create(lua_State *L) {
    int start_row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int end_row = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    bool closed = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
    std::string provider = luaL_optstring(L, 4, "manual");
    GetEditor(L)->CreateFold(start_row, end_row, closed, provider);
    return 0;
}

/**
 * @brief Implements mep.fold_clear_provider(provider): removes every fold previously created under a given provider name.
 * @param L Lua state; arg 1 is the provider name.
 * @return Number of values pushed (0).
 */
int l_fold_clear_provider(lua_State *L) {
    const char *provider = luaL_checkstring(L, 1);
    GetEditor(L)->ClearFoldsFromProvider(provider);
    return 0;
}

/**
 * @brief Implements mep.fold_toggle(): opens or closes the fold at the cursor.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_fold_toggle(lua_State *L) {
    GetEditor(L)->ToggleFoldAtCursor();
    return 0;
}

// mep.buf_set_image_row(row, path) -- row 1-indexed, matching mep.deco_add/
// mep.fold_create's own convention. Registers `path` (already resolved by
// the caller, mep.org_image_scan in kBuiltinOrgImages) as the current
// buffer's inline-image target for `row`.
/**
 * @brief Implements mep.buf_set_image_row(row, path): registers an inline-image target for a line of the current buffer.
 * @param L Lua state; arg 1 is the 1-indexed row, arg 2 the already-resolved image path.
 * @return Number of values pushed (0).
 */
int l_buf_set_image_row(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    const char *path = luaL_checkstring(L, 2);
    GetEditor(L)->SetOrgImageRow(row, path);
    return 0;
}

// mep.buf_clear_image_rows(): clears the current buffer's whole registry --
// mep.org_image_scan calls this before rescanning.
/**
 * @brief Implements mep.buf_clear_image_rows(): clears the current buffer's whole inline-image row registry.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_buf_clear_image_rows(lua_State *L) {
    GetEditor(L)->ClearOrgImageRows();
    return 0;
}

// mep.org_images_toggle() -> new visibility (bool). Bound to <leader>oti
// via kBuiltinOrgImages' own mep.leader_map call.
/**
 * @brief Implements mep.org_images_toggle(): toggles inline org image rendering on/off.
 * @param L Lua state.
 * @return Number of values pushed (1: the new visibility state).
 */
int l_org_images_toggle(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->ToggleOrgImages());
    return 1;
}

// mep.org_image_invalidate(path): forces the next render of `path` (an
// already-resolved absolute path, mep_org_resolve_path) to reload from
// disk unconditionally, instead of trusting the mtime-based cache
// GetOrLoadOrgInlineImageTexture otherwise uses. Called by
// mep.org_babel_execute (kBuiltinOrgBabel) right after a :file block
// finishes -- see InvalidateOrgInlineImageTexture's own comment (main.cpp)
// for the race this closes.
/**
 * @brief Implements mep.org_image_invalidate(path): forces the next render of an inline org image to reload from disk, bypassing the mtime cache.
 * @param L Lua state; arg 1 is the already-resolved absolute image path.
 * @return Number of values pushed (0).
 */
int l_org_image_invalidate(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    InvalidateOrgInlineImageTexture(path);
    return 0;
}

// mep.buf_set_latex_row(row, path, slots, end_row) -- row/end_row
// 1-indexed, same convention as mep.buf_set_image_row. `path` is a
// rendered fragment's PNG (already produced by mep_org_latex_render,
// kBuiltinOrgLatex); `slots` is that fragment's own display height in
// line-heights; `end_row` is the last raw source row the fragment's own
// text spanned (== row for a single-line fragment) -- see
// Buffer::OrgLatexRender.
/**
 * @brief Implements mep.buf_set_latex_row(row, path, slots, end_row): registers a rendered LaTeX fragment for a source-line span of the current buffer.
 * @param L Lua state; arg 1 is the 1-indexed row, arg 2 the rendered fragment's PNG path, arg 3 the fragment's display height in line-heights, arg 4 the 1-indexed last raw source row the fragment spans.
 * @return Number of values pushed (0).
 */
int l_buf_set_latex_row(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    const char *path = luaL_checkstring(L, 2);
    int slots = static_cast<int>(luaL_checkinteger(L, 3));
    int end_row = static_cast<int>(luaL_checkinteger(L, 4)) - 1;
    GetEditor(L)->SetOrgLatexRow(row, path, slots, end_row);
    return 0;
}

// mep.buf_clear_latex_rows(): clears the current buffer's whole registry --
// mep.org_latex_scan calls this before rescanning (and when the toggle
// turns off, since -- unlike images -- a populated entry here implies a
// 'latex'-provider Fold is also hiding real source text; see
// Buffer::org_latex_rows' own comment).
/**
 * @brief Implements mep.buf_clear_latex_rows(): clears the current buffer's whole rendered-LaTeX row registry.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_buf_clear_latex_rows(lua_State *L) {
    GetEditor(L)->ClearOrgLatexRows();
    return 0;
}

// mep.org_latex_toggle() -> new visibility (bool). Bound to <leader>otl via
// kBuiltinOrgLatex's own mep.leader_map call.
/**
 * @brief Implements mep.org_latex_toggle(): toggles inline org LaTeX rendering on/off.
 * @param L Lua state.
 * @return Number of values pushed (1: the new visibility state).
 */
int l_org_latex_toggle(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->ToggleOrgLatex());
    return 1;
}

// mep.org_latex_visible() -> bool. Lua-side readable state (unlike
// OrgImagesVisible(), org_latex_scan itself needs to consult this -- see
// Buffer::org_latex_rows' own comment for why the two toggles' scan
// functions differ here).
/**
 * @brief Implements mep.org_latex_visible(): reports whether inline org LaTeX rendering is currently on.
 * @param L Lua state.
 * @return Number of values pushed (1: the current visibility state).
 */
int l_org_latex_visible(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->OrgLatexVisible());
    return 1;
}

// mep.buf_add_latex_inline(row, col_start, col_end, path) -- same 1-indexed/
// exclusive-col_end convention as mep.deco_add's opts table. Appends one
// inline-math span (Buffer::OrgLatexInlineSpan) for `row`; called once per
// match by mep_org_latex_register_inline (kBuiltinOrgLatex).
/**
 * @brief Implements mep.buf_add_latex_inline(row, col_start, col_end, path): appends one inline-math span for a line of the current buffer.
 * @param L Lua state; arg 1 is the 1-indexed row, arg 2 the 1-indexed start column, arg 3 the 1-indexed exclusive end column, arg 4 the rendered fragment's path.
 * @return Number of values pushed (0).
 */
int l_buf_add_latex_inline(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int col_start = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    int col_end = static_cast<int>(luaL_checkinteger(L, 3)) - 1;
    const char *path = luaL_checkstring(L, 4);
    GetEditor(L)->AddOrgLatexInlineSpan(row, col_start, col_end, path);
    return 0;
}

// mep.buf_clear_latex_inline(): clears the current buffer's whole inline-span
// registry -- mep.org_latex_scan calls this before rescanning (and when the
// toggle turns off; see Buffer::org_latex_inline's own comment for why that
// matters here even more than it does for buf_clear_latex_rows).
/**
 * @brief Implements mep.buf_clear_latex_inline(): clears the current buffer's whole inline-LaTeX span registry.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_buf_clear_latex_inline(lua_State *L) {
    GetEditor(L)->ClearOrgLatexInlineSpans();
    return 0;
}

// mep.font_size() -> the current editor pixel font size (g_font_size,
// main.cpp -- Ctrl+=/Ctrl+- adjust it). kBuiltinOrgLatex uses this to pick
// a rasterization DPI that keeps a rendered fragment's glyphs visually
// close to surrounding buffer text, and to replicate LineHeight()'s own
// `font_size + 6` formula for its own slot-count math (editor.cpp has no
// pixel/font-metric knowledge of its own to do this instead).
/**
 * @brief Implements mep.font_size(): returns the current editor pixel font size.
 * @param L Lua state.
 * @return Number of values pushed (1: the font size in pixels).
 */
int l_font_size(lua_State *L) {
    lua_pushnumber(L, static_cast<double>(GetFontSizePx()));
    return 1;
}

// mep.image_size(path) -> width, height (integers) or nil if `path` isn't a
// readable PNG. Parses just the PNG signature + IHDR chunk (bytes 16-23),
// not a full decode -- plenty for mep_org_latex_render (kBuiltinOrgLatex) to
// turn a just-rendered fragment's pixel height into a slot count without
// pulling in a real image decoder or touching the GPU texture cache
// (GetOrLoadOrgInlineImageTexture) a frame before DrawPane otherwise would.
/**
 * @brief Implements mep.image_size(path): reads a PNG's pixel dimensions from just its signature + IHDR chunk, without a full decode.
 * @param L Lua state; arg 1 is the filesystem path to a PNG file.
 * @return Number of values pushed (2: width and height, or 0 if the file isn't a readable PNG).
 */
int l_image_size(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::ifstream f(path, std::ios::binary);
    unsigned char header[24];
    if (!f || !f.read(reinterpret_cast<char *>(header), sizeof(header))) return 0;
    static const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (std::memcmp(header, kPngSig, sizeof(kPngSig)) != 0) return 0;
    /**
     * @brief Reads a big-endian 32-bit integer out of the PNG header at a given byte offset.
     * @param off Byte offset into `header` to read from.
     * @return The 4 bytes at `off` interpreted as a big-endian uint32_t.
     */
    auto be32 = [&](int off) {
        return (static_cast<uint32_t>(header[off]) << 24) | (static_cast<uint32_t>(header[off + 1]) << 16) |
               (static_cast<uint32_t>(header[off + 2]) << 8) | static_cast<uint32_t>(header[off + 3]);
    };
    lua_pushinteger(L, static_cast<lua_Integer>(be32(16)));
    lua_pushinteger(L, static_cast<lua_Integer>(be32(20)));
    return 2;
}

// mep.image_set_nav(buffer_id, prev_fn?, next_fn?): configures an image
// buffer's "<"/">" nav-header (Editor::SetImageNav/ImageSession::
// nav_prev_ref/nav_next_ref, DrawPane's image branch, main.cpp) -- omit or
// pass nil for either to clear it. See kBuiltinLanguageUiR's merged Plot
// pane for the intended use: every time it points an image pane at a
// different figure file, it re-applies these two callbacks to the
// (possibly new) buffer id.
/**
 * @brief Implements mep.image_set_nav(buffer_id, prev_fn?, next_fn?): sets or clears an image
 * buffer's "<"/">" nav-header click callbacks.
 * @param L Lua state; arg 1 is the image buffer id, optional args 2/3 are the prev/next callbacks (nil clears).
 * @return Number of values pushed (0).
 */
int l_image_set_nav(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int prev_ref = 0, next_ref = 0;
    if (!lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 2);
        prev_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (!lua_isnoneornil(L, 3)) {
        lua_pushvalue(L, 3);
        next_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    GetEditor(L)->SetImageNav(buffer_id, prev_ref, next_ref);
    return 0;
}

// mep.image_set_return(buffer_id, fn?): configures an image buffer's
// Shift+I callback (Editor::SetImageReturn/ImageSession::return_ref) --
// omit or pass nil to clear it. kBuiltinFileTree's image-viewer toggle is
// the first caller, pairing this with its own mep.buffer_set_on_image_toggle
// on the tree buffer so Shift+I switches between the two. Same re-apply-on-
// every-reopen need as mep.image_set_nav above.
/**
 * @brief Implements mep.image_set_return(buffer_id, fn?): sets or clears an image buffer's
 * Shift+I callback.
 * @param L Lua state; arg 1 is the image buffer id, optional arg 2 is the callback (nil clears).
 * @return Number of values pushed (0).
 */
int l_image_set_return(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int ref = 0;
    if (!lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 2);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    GetEditor(L)->SetImageReturn(buffer_id, ref);
    return 0;
}

// mep.image_set_theme(buffer_id, theme_colors): sets an image buffer's
// default theme_colors state (Editor::SetImageTheme/ImageSession::
// theme_colors, DrawPane's image branch, main.cpp) -- Ctrl-R still toggles
// it interactively from there same as a PDF/HTML pane; this just sets
// where it starts. See ImageSession::theme_colors' own comment for why
// this defaults false and is opt-in per buffer, unlike PdfSession/
// HtmlSession's own theme_colors.
/**
 * @brief Implements mep.image_set_theme(buffer_id, theme_colors): sets an image buffer's
 * default theme_colors state.
 * @param L Lua state; arg 1 is the image buffer id, arg 2 the state to set.
 * @return Number of values pushed (0).
 */
int l_image_set_theme(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    bool theme_colors = lua_toboolean(L, 2);
    GetEditor(L)->SetImageTheme(buffer_id, theme_colors);
    return 0;
}

// mep.image_get_theme(buffer_id) -> bool|nil: returns the current Ctrl-R
// viewing mode for an image buffer, or nil for a non-image buffer. Navigation
// UIs use this before mep.open changes the active image buffer so they can
// preserve the user's chosen viewing mode on the replacement image.
/**
 * @brief Implements mep.image_get_theme(buffer_id): gets an image buffer's current theme-colors state.
 * @param L Lua state; arg 1 is the image buffer id.
 * @return One value: boolean for an image buffer, otherwise nil.
 */
int l_image_get_theme(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    bool theme_colors = false;
    if (GetEditor(L)->GetImageTheme(buffer_id, theme_colors)) lua_pushboolean(L, theme_colors);
    else lua_pushnil(L);
    return 1;
}

std::vector<PickerHlSpan> ReadPreviewSpans(lua_State *L, int idx);  // defined below, with mep.sidebar_set_preview

// mep.sidebar_create(title, position, size) -> id.
/**
 * @brief Implements mep.sidebar_create(title, position, size, tab_group): creates a new sidebar panel.
 * @param L Lua state; arg 1 is the sidebar title, optional arg 2 the dock position (default "right"), optional arg 3 the size in columns (default 34), optional arg 4 a tab_group string merging it with every other open sidebar sharing that string (and position) into one tabbed stack slot.
 * @return Number of values pushed (1: the new sidebar's id).
 */
int l_sidebar_create(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    const char *position = luaL_optstring(L, 2, "right");
    int size = static_cast<int>(luaL_optinteger(L, 3, 34));
    const char *tab_group = luaL_optstring(L, 4, "");
    lua_pushinteger(L, GetEditor(L)->CreateSidebar(title, position, size, tab_group));
    return 1;
}

// mep.sidebar_set_sections(id, sections): sections is an array of
// {id=, title=, collapsed=, widgets={{id=,text=,icon=,hl=,tooltip=,on_click=fn,spans={{col_start=,col_end=,hl=},...},image=path,image_rows=n},...}}.
/**
 * @brief Implements mep.sidebar_set_sections(id, sections): replaces a sidebar's whole content with a new set of collapsible sections of widgets.
 * @param L Lua state; arg 1 is the sidebar id, arg 2 an array of section tables (each with id/title/collapsed/widgets).
 * @return Number of values pushed (0).
 */
int l_sidebar_set_sections(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<SidebarSection> sections;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        SidebarSection sec;
        lua_getfield(L, -1, "id");
        if (lua_isstring(L, -1)) sec.id = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "title");
        if (lua_isstring(L, -1)) sec.title = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "collapsed");
        sec.collapsed = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "widgets");
        if (lua_istable(L, -1)) {
            lua_Integer wn = static_cast<lua_Integer>(lua_rawlen(L, -1));
            for (lua_Integer j = 1; j <= wn; j++) {
                lua_rawgeti(L, -1, j);
                luaL_checktype(L, -1, LUA_TTABLE);
                SidebarWidget w;
                lua_getfield(L, -1, "id");
                if (lua_isstring(L, -1)) w.id = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "text");
                if (lua_isstring(L, -1)) w.text = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "icon");
                if (lua_isstring(L, -1)) w.icon = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "hl");
                if (lua_isstring(L, -1)) w.hl = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "tooltip");
                if (lua_isstring(L, -1)) w.tooltip = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "current");
                w.current = lua_toboolean(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "wrap");
                w.wrap = lua_toboolean(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "wrap_indent");
                if (lua_isinteger(L, -1)) w.wrap_indent = static_cast<int>(lua_tointeger(L, -1));
                lua_pop(L, 1);
                lua_getfield(L, -1, "trailing_icon");
                if (lua_isstring(L, -1)) w.trailing_icon = lua_tostring(L, -1);
                lua_pop(L, 1);
                w.trailing_on_click_ref = RefField(L, -1, "trailing_on_click");
                w.on_click_ref = RefField(L, -1, "on_click");
                // Optional `spans` (SidebarWidget::spans): mep.ts_captures'
                // own {col_start=, col_end=, hl=} shape, 1-indexed and
                // col_end-exclusive, read by the same helper the popout
                // preview's spans go through (`row` is read too but unused).
                lua_getfield(L, -1, "spans");
                w.spans = ReadPreviewSpans(L, lua_gettop(L));
                lua_pop(L, 1);
                lua_getfield(L, -1, "image");
                if (lua_isstring(L, -1)) w.image = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "image_rows");
                if (lua_isinteger(L, -1)) w.image_rows = std::max(1, static_cast<int>(lua_tointeger(L, -1)));
                lua_pop(L, 1);
                lua_pop(L, 1);  // the widget table itself
                sec.widgets.push_back(std::move(w));
            }
        }
        lua_pop(L, 1);  // "widgets" field
        lua_pop(L, 1);  // the section table itself
        sections.push_back(std::move(sec));
    }
    GetEditor(L)->SetSidebarSections(id, std::move(sections));
    return 0;
}

/**
 * @brief Implements mep.sidebar_open(id [, focus]): opens a sidebar, optionally moving focus into it.
 * @param L Lua state; arg 1 is the sidebar id, optional arg 2 whether to focus it (default true).
 * @return Number of values pushed (0).
 */
int l_sidebar_open(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool focus = lua_gettop(L) < 2 || lua_toboolean(L, 2);
    GetEditor(L)->OpenSidebar(id, focus);
    return 0;
}

// mep.sidebar_open_pane([id]): opens a sidebar's content as an ordinary
// tabbed buffer in the focused pane, the way mep.open/mep.pane_open opens
// a file -- so a sidebar can be split/moved/tab-cycled/merged with mod1's
// usual pane chords instead of staying docked to a screen edge. See
// SidebarInstance::tab_group's own comment (editor.h) and
// Mode::SidebarPane for the alternative this largely superseded for
// kBuiltinLanguageUiR's own Plot/Data/Help/Objects/Packages/History. With
// `id` omitted (or 0), same "whichever sidebar has focus" default as
// mep.sidebar_popout_toggle -- what mod1+o (kDefaultMod1Bindings) calls, so
// every sidebar (left- or right-docked) gets this without needing its own
// bespoke _open_pane wrapper like kBuiltinGit's mep.git_open_pane.
/**
 * @brief Implements mep.sidebar_open_pane([id]): opens a sidebar's content as a tabbed buffer in
 * the focused pane.
 * @param L Lua state; optional arg 1 is the sidebar id (default: whichever sidebar has focus).
 * @return Number of values pushed (0).
 */
int l_sidebar_open_pane(lua_State *L) {
    GetEditor(L)->SidebarOpenPane(static_cast<int>(luaL_optinteger(L, 1, 0)));
    return 0;
}

/**
 * @brief Implements mep.sidebar_close(id): closes a sidebar.
 * @param L Lua state; arg 1 is the sidebar id.
 * @return Number of values pushed (0).
 */
int l_sidebar_close(lua_State *L) {
    GetEditor(L)->CloseSidebar(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

/**
 * @brief Implements mep.sidebar_toggle(id [, focus]): opens a sidebar if closed, closes it if open.
 * @param L Lua state; arg 1 is the sidebar id, optional arg 2 whether to focus it when opening (default true).
 * @return Number of values pushed (0).
 */
int l_sidebar_toggle(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool focus = lua_gettop(L) < 2 || lua_toboolean(L, 2);
    GetEditor(L)->ToggleSidebar(id, focus);
    return 0;
}

/**
 * @brief Implements mep.sidebar_is_open(id): reports whether a sidebar is currently open.
 * @param L Lua state; arg 1 is the sidebar id.
 * @return Number of values pushed (1: true if open).
 */
int l_sidebar_is_open(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsSidebarOpen(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

// mep.sidebar_set_on_key(id, fn): fn(char) for any key HandleSidebarInput
// doesn't already reserve (Phase 15's file tree uses this for create/
// rename/delete/refresh/toggle-hidden).
/**
 * @brief Implements mep.sidebar_set_on_key(id, fn): registers a callback for keys the sidebar's own input handler doesn't already reserve.
 * @param L Lua state; arg 1 is the sidebar id, arg 2 the callback function, invoked with the pressed character.
 * @return Number of values pushed (0).
 */
int l_sidebar_set_on_key(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetSidebarOnKey(id, ref);
    return 0;
}

// mep.sidebar_set_tabs(id, {name, ...}, active?): gives sidebar `id` a tab
// strip (SidebarInstance::tabs) -- one view name per entry, `active` the
// 1-based one to start on (default 1). Tab/Shift-Tab while focused, a
// click on a name, or mep.sidebar_set_active_tab switch; each fires the
// mep.sidebar_set_on_tab callback with the new 1-based index, which is
// where the consumer re-renders its sections for that view.
/**
 * @brief Implements mep.sidebar_set_tabs(id, names, active?): sets a sidebar's tab strip.
 * @param L Lua state; arg 1 sidebar id, arg 2 array of tab names, optional arg 3 the 1-based active tab.
 * @return Number of values pushed (0).
 */
int l_sidebar_set_tabs(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> tabs;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        tabs.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    int active = static_cast<int>(luaL_optinteger(L, 3, 1)) - 1;
    GetEditor(L)->SetSidebarTabs(id, std::move(tabs), active);
    return 0;
}

/**
 * @brief Implements mep.sidebar_set_on_tab(id, fn): fn(index) runs whenever the sidebar's active tab changes (1-based).
 * @param L Lua state; arg 1 sidebar id, arg 2 the callback.
 * @return Number of values pushed (0).
 */
int l_sidebar_set_on_tab(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetSidebarOnTab(id, ref);
    return 0;
}

/**
 * @brief Implements mep.sidebar_set_active_tab(id, index): switches a sidebar to the 1-based tab (wrapping), firing its on_tab callback.
 * @param L Lua state; arg 1 sidebar id, arg 2 the 1-based tab index.
 * @return Number of values pushed (0).
 */
int l_sidebar_set_active_tab(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int index = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    GetEditor(L)->SelectSidebarTab(id, index);
    return 0;
}

/**
 * @brief Implements mep.sidebar_active_tab(id) -> 1-based index of the sidebar's active tab (1 when it has none).
 * @param L Lua state; arg 1 sidebar id.
 * @return Number of values pushed (1).
 */
int l_sidebar_active_tab(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushinteger(L, GetEditor(L)->SidebarActiveTab(id) + 1);
    return 1;
}

// mep.sidebar_set_on_preview(id, fn): fn(widget_id) is called whenever the
// row under the cursor changes while sidebar `id` is popped out (mod1+m,
// Editor::ToggleSidebarPopout) -- "" for a section header. The callback
// answers via mep.sidebar_set_preview, either right away or later from a
// job's on_exit (the git status sidebar's `git diff`), so an async source
// needs no special plumbing.
/**
 * @brief Implements mep.sidebar_set_on_preview(id, fn): registers the callback that supplies a sidebar's popout preview.
 * @param L Lua state; arg 1 is the sidebar id, arg 2 the callback, invoked with the cursor widget's id ("" on a section header).
 * @return Number of values pushed (0).
 */
int l_sidebar_set_on_preview(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetSidebarOnPreview(id, ref);
    return 0;
}

// Reads mep.picker_set_preview/mep.sidebar_set_preview's shared spans
// argument -- a Lua array of {row=, col_start=, col_end=, hl=} (all
// 1-indexed, col_end exclusive) -- at stack index `idx`.
/**
 * @brief Reads a Lua array of {row=, col_start=, col_end=, hl=} preview highlight spans into PickerHlSpan structs.
 * @param L Lua state.
 * @param idx Stack index of the array table (anything but a table reads as no spans).
 * @return The parsed spans; entries without an `hl` string are skipped.
 */
std::vector<PickerHlSpan> ReadPreviewSpans(lua_State *L, int idx) {
    std::vector<PickerHlSpan> spans;
    if (!lua_istable(L, idx)) return spans;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "row");
            int row = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
            lua_pop(L, 1);
            lua_getfield(L, -1, "col_start");
            int col_start = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
            lua_pop(L, 1);
            lua_getfield(L, -1, "col_end");
            int col_end = static_cast<int>(luaL_optinteger(L, -1, col_start + 2)) - 1;
            lua_pop(L, 1);
            lua_getfield(L, -1, "hl");
            if (lua_isstring(L, -1)) {
                PickerHlSpan span;
                span.row = row;
                span.col_start = col_start;
                span.col_end = col_end;
                span.hl_group = lua_tostring(L, -1);
                spans.push_back(std::move(span));
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return spans;
}

// mep.sidebar_set_preview(text [, spans [, title [, current_row]]]): the
// popout's preview-column content -- see Editor::SetSidebarPopoutPreview.
// `spans` has mep.picker_set_preview's exact shape; `current_row` is the
// 1-indexed preview line to tint as the row the widget refers to (the
// definition line of a structure item, a todo's headline).
/**
 * @brief Implements mep.sidebar_set_preview(text, spans, title, current_row): sets the popped-out sidebar's preview column.
 * @param L Lua state; arg 1 is the preview text ("" clears it), optional arg 2 an array of {row=, col_start=, col_end=, hl=} spans, optional arg 3 a caption, optional arg 4 the 1-indexed line to tint.
 * @return Number of values pushed (0).
 */
int l_sidebar_set_preview(lua_State *L) {
    size_t len = 0;
    const char *text = luaL_optlstring(L, 1, "", &len);
    std::vector<PickerHlSpan> spans = ReadPreviewSpans(L, 2);
    const char *title = luaL_optstring(L, 3, "");
    int current_row = static_cast<int>(luaL_optinteger(L, 4, 0)) - 1;
    GetEditor(L)->SetSidebarPopoutPreview(std::string(text, len), std::move(spans), title, current_row);
    return 0;
}

// mep.sidebar_popout_toggle([id]): pop the focused sidebar (or `id`, if
// it is the focused one) out into the large centered float, or collapse
// it back -- what mod1+m is bound to (kDefaultMod1Bindings, main.cpp).
/**
 * @brief Implements mep.sidebar_popout_toggle([id]): toggles the focused sidebar's popout float.
 * @param L Lua state; optional arg 1 is the sidebar id (default: whichever sidebar has focus).
 * @return Number of values pushed (0).
 */
int l_sidebar_popout_toggle(lua_State *L) {
    int id = static_cast<int>(luaL_optinteger(L, 1, 0));
    GetEditor(L)->ToggleSidebarPopout(id);
    return 0;
}

// mep.notify_sidebar_id()/mep.notify_refresh_pane(): the Notifications
// panel's ensure+render step, split out the same way mep.activity_todo_
// panel/mep_activity_todo_sidebar_id are for the Lua-owned sidebars --
// kBuiltinActivityBar's own mep.notify_open_pane (Lua, <leader>nn) calls
// these then positions/inserts the pane itself via mep.right_sidebar_
// position_pane + mep.sidebar_open_pane(id), since this panel is owned
// entirely in C++ (Editor::RefreshNotifyPane/NotifySidebarId) unlike the
// Lua-owned Todo/Tests sidebars.
/**
 * @brief Implements mep.notify_sidebar_id(): returns the Notifications SidebarInstance id, creating it if needed.
 * @param L Lua state.
 * @return Number of values pushed (1: the sidebar id).
 */
int l_notify_sidebar_id(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->NotifySidebarId());
    return 1;
}

/**
 * @brief Implements mep.notify_refresh_pane(): ensures the Notifications sidebar exists, refreshes
 * its content, and closes any currently-docked view.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_notify_refresh_pane(lua_State *L) {
    GetEditor(L)->RefreshNotifyPane();
    return 0;
}

/**
 * @brief Implements mep.sidebar_popout_close(): collapses the popped-out sidebar (if any) back to its docked panel.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_sidebar_popout_close(lua_State *L) {
    GetEditor(L)->CloseSidebarPopout();
    return 0;
}

/**
 * @brief Implements mep.sidebar_is_popout([id]): reports whether a sidebar (default: any) is currently popped out.
 * @param L Lua state; optional arg 1 is the sidebar id.
 * @return Number of values pushed (1: boolean).
 */
int l_sidebar_is_popout(lua_State *L) {
    Editor *ed = GetEditor(L);
    int id = static_cast<int>(luaL_optinteger(L, 1, 0));
    bool active = ed->SidebarPopoutActive() && (id == 0 || ed->SidebarPopoutId() == id);
    lua_pushboolean(L, active);
    return 1;
}

// mep.read_lines(path [, max_lines]) -> array of lines, or nil if the
// file is neither open in a buffer nor readable. Prefers an open buffer's
// live (possibly unsaved) lines -- see Editor::ReadLinesForPath.
/**
 * @brief Implements mep.read_lines(path, max_lines): returns a file's lines, preferring an open buffer's live contents.
 * @param L Lua state; arg 1 is the path, optional arg 2 caps the line count (a trailing "..." marks truncation).
 * @return Number of values pushed (1: the array of lines, or nil).
 */
int l_read_lines(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int max_lines = static_cast<int>(luaL_optinteger(L, 2, 0));
    std::vector<std::string> lines;
    if (!GetEditor(L)->ReadLinesForPath(path, max_lines, &lines)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    for (size_t i = 0; i < lines.size(); i++) {
        lua_pushlstring(L, lines[i].data(), lines[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.buffer_set_on_enter(buffer_id, fn): fn() replaces whatever bare
// Enter/KP_Enter already does in Normal mode (nothing, by default -- see
// SetBufferOnEnter's own comment, editor.h) while `buffer_id` is the
// active pane's buffer. One callback per buffer; re-registering replaces it.
/**
 * @brief Implements mep.buffer_set_on_enter(buffer_id, fn): registers a callback that replaces bare Enter/KP_Enter's default Normal-mode behavior for a buffer.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_buffer_set_on_enter(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetBufferOnEnter(buffer_id, ref);
    return 0;
}

// mep.buffer_set_on_write(buffer_id, fn): fn() replaces `:w`/`:wq`/`ZZ`'s
// real disk write for `buffer_id` (Editor::SetBufferOnWrite's own comment,
// editor.h) while it's the active pane's buffer.
/**
 * @brief Implements mep.buffer_set_on_write(buffer_id, fn): registers a callback that replaces a buffer's real disk write.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_buffer_set_on_write(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetBufferOnWrite(buffer_id, ref);
    return 0;
}

// mep.buffer_set_on_image_toggle(buffer_id, fn): fn() replaces the builtin
// Shift+I (insert at first non-blank) for `buffer_id` (Editor::
// SetBufferOnImageToggle's own comment, editor.h) while it's the active
// pane's buffer. One callback per buffer; re-registering replaces it.
/**
 * @brief Implements mep.buffer_set_on_image_toggle(buffer_id, fn): registers a callback that
 * replaces bare Shift+I's default Normal-mode behavior for a buffer.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_buffer_set_on_image_toggle(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetBufferOnImageToggle(buffer_id, ref);
    return 0;
}

// mep.buffer_set_on_key(buffer_id, fn): fn(key) is offered every plain
// Normal-mode keypress (a one-character string) while `buffer_id` is the
// active pane's buffer and no operator/count/prefix is pending; returning
// true swallows the key, anything else lets the builtin command run
// (Editor::SetBufferOnKey's own comment, editor.h). Pass nil to clear.
/**
 * @brief Implements mep.buffer_set_on_key(buffer_id, fn): registers a buffer-scoped Normal-mode key filter.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the callback function (or nil to clear).
 * @return Number of values pushed (0).
 */
int l_buffer_set_on_key(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int ref = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_pushvalue(L, 2);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    GetEditor(L)->SetBufferOnKey(buffer_id, ref);
    return 0;
}

/**
 * @brief Implements mep.buffer_set_filename(id, name): sets a buffer's raw filename.
 * @param L Lua state; arg 1 is the buffer id, arg 2 the new filename.
 * @return Number of values pushed (0).
 */
int l_buffer_set_filename(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    const char *name = luaL_checkstring(L, 2);
    GetEditor(L)->SetBufferFilenameForLua(id, name);
    return 0;
}

/**
 * @brief Implements mep.buffer_set_hide_line_numbers(id, hide): opts a buffer out of the number/relativenumber gutter.
 * @param L Lua state; arg 1 is the buffer id, arg 2 whether to hide line numbers.
 * @return Number of values pushed (0).
 */
int l_buffer_set_hide_line_numbers(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool hide = lua_toboolean(L, 2) != 0;
    GetEditor(L)->SetBufferHideLineNumbers(id, hide);
    return 0;
}

/**
 * @brief Implements mep.buffer_set_wrap(id, wrap): opts a buffer out of :set wrap's soft-wrap.
 * @param L Lua state; arg 1 is the buffer id, arg 2 whether soft-wrap stays enabled for it.
 * @return Number of values pushed (0).
 */
int l_buffer_set_wrap(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool wrap = lua_toboolean(L, 2) != 0;
    GetEditor(L)->SetBufferNoWrap(id, !wrap);
    return 0;
}

/**
 * @brief Implements mep.buffer_modified(id): returns whether a buffer has unsaved changes.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: true if modified).
 */
int l_buffer_modified(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->BufferModifiedForLua(id));
    return 1;
}

/**
 * @brief Implements mep.sidebar_cursor(): returns the currently-open sidebar's cursor row.
 * @param L Lua state.
 * @return Number of values pushed (1: the 1-indexed cursor row).
 */
int l_sidebar_cursor(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->SidebarCursor() + 1);
    return 1;
}

/**
 * @brief Implements mep.sidebar_cursor_widget_id(id): looks up the widget id under the cursor in a given sidebar.
 * @param L Lua state; arg 1 is the sidebar id.
 * @return Number of values pushed (1: the widget id string, or nil if none).
 */
int l_sidebar_cursor_widget_id(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    std::string wid = GetEditor(L)->SidebarCursorWidgetId(id);
    if (wid.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, wid.c_str());
    }
    return 1;
}

// mep.sidebar_is_focused(id) -> bool: whether sidebar `id` currently
// holds keyboard focus (Mode::Sidebar with this id) -- what makes
// mep.sidebar_cursor()'s row belong to it, and what a re-render that
// wants to put the cursor back (mep.sidebar_focus_row) must check first,
// since focusing from any other mode would capture that mode as the one
// to restore to.
/**
 * @brief Implements mep.sidebar_is_focused(id): reports whether a sidebar holds keyboard focus.
 * @param L Lua state; arg 1 is the sidebar id.
 * @return Number of values pushed (1: boolean).
 */
int l_sidebar_is_focused(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    Editor *ed = GetEditor(L);
    lua_pushboolean(L, ed->CurrentMode() == Mode::Sidebar && ed->FocusedSidebarId() == id);
    return 1;
}

// mep.sidebar_focus_row(id, row): focuses sidebar `id` with its cursor on
// 1-indexed flattened `row` (clamped in range) -- Editor::FocusSidebarRow,
// the same call a mouse click on a row makes. A Lua panel uses it to keep
// the cursor on the same row across a re-render that removed rows.
/**
 * @brief Implements mep.sidebar_focus_row(id, row): focuses a sidebar and moves its cursor to a row.
 * @param L Lua state; arg 1 is the sidebar id, arg 2 the 1-indexed row (clamped in range).
 * @return Number of values pushed (0).
 */
int l_sidebar_focus_row(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int row = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    GetEditor(L)->FocusSidebarRow(id, std::max(0, row));
    return 0;
}

// Reads a Lua array of {col_start=, col_end=, hl=} spans (1-indexed,
// col_end exclusive -- same convention as mep.ts_captures/mep.deco_add)
// at stack index `idx` into `out`, tagging each with `row` (already
// 0-indexed; the caller picks it -- 0 for a PickerItem's own single-line
// `display`, the preview line index for mep.picker_set_preview). Used by
// both ReadPickerItems' per-item `hl` field and l_picker_set_preview's
// own spans argument -- kBuiltinPickerSources' mep.buffer_search
// (main.cpp) is the first source to populate either.
/**
 * @brief Reads a Lua array of {col_start=, col_end=, hl=} highlight spans into PickerHlSpan structs, tagging each with a caller-supplied row.
 * @param L Lua state.
 * @param idx Stack index of the array table to read.
 * @param row 0-indexed row to stamp onto every span read.
 * @param out Vector to append the parsed spans onto; spans with no `hl` field are skipped.
 */
void ReadPickerHlSpans(lua_State *L, int idx, int row, std::vector<PickerHlSpan> &out) {
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        if (lua_istable(L, -1)) {
            PickerHlSpan span;
            span.row = row;
            lua_getfield(L, -1, "col_start");
            span.col_start = static_cast<int>(luaL_optinteger(L, -1, 1)) - 1;
            lua_pop(L, 1);
            lua_getfield(L, -1, "col_end");
            span.col_end = static_cast<int>(luaL_optinteger(L, -1, span.col_start + 2)) - 1;
            lua_pop(L, 1);
            lua_getfield(L, -1, "hl");
            if (lua_isstring(L, -1)) span.hl_group = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (!span.hl_group.empty()) out.push_back(std::move(span));
        }
        lua_pop(L, 1);
    }
}

// Reads a Lua array of items (each either a plain string, or a
// {display=, data=, hl=} table) at stack index `idx` into `out`. `hl`
// (optional) is an array of {col_start=, col_end=, hl=} spans over
// `display` -- see ReadPickerHlSpans above.
/**
 * @brief Reads a Lua array of picker items (each a plain string, or a {display=, data=, hl=} table) into PickerItem structs.
 * @param L Lua state.
 * @param idx Stack index of the array table to read.
 * @param out Vector to append the parsed items onto.
 */
void ReadPickerItems(lua_State *L, int idx, std::vector<PickerItem> &out) {
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        PickerItem item;
        if (lua_isstring(L, -1)) {
            item.display = item.data = lua_tostring(L, -1);
        } else if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "display");
            if (lua_isstring(L, -1)) item.display = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "data");
            item.data = lua_isstring(L, -1) ? lua_tostring(L, -1) : item.display;
            lua_pop(L, 1);
            lua_getfield(L, -1, "hl");
            if (lua_istable(L, -1)) ReadPickerHlSpans(L, lua_gettop(L), 0, item.spans);
            lua_pop(L, 1);
        }
        out.push_back(std::move(item));
        lua_pop(L, 1);
    }
}

// mep.picker_open(title, items, on_select [, on_query_change [, on_key [,
// on_select_change]]]).
// on_key(key): fires on Ctrl+<letter> for any letter besides N/P (already
// reserved for next/prev) -- e.g. mep.projects()'s Ctrl-A for "add current
// directory". Pass nil for on_query_change (or on_key) to reach a later
// positional arg without one.
// on_select_change(data): fires with the newly-highlighted item's `data`
// every time the highlighted row changes -- arrow/Ctrl-N/Ctrl-P navigation,
// or the query narrowing to a different top match -- *before* Enter/Escape
// commit or cancel. Lets a picker live-preview each candidate as the cursor
// moves over it (e.g. mep.themes() re-tinting the UI live while browsing
// colorschemes), not just once on final selection.
/**
 * @brief Implements mep.picker_open(title, items, on_select, ...): opens the fuzzy picker overlay with the given items and callbacks.
 * @param L Lua state; arg 1 title string, arg 2 items array, arg 3 on_select function, optional arg 4 on_query_change, optional arg 5 on_key, optional arg 6 on_select_change, optional arg 7 raw_results bool.
 * @return Number of values pushed (0).
 */
int l_picker_open(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    std::vector<PickerItem> items;
    ReadPickerItems(L, 2, items);
    lua_pushvalue(L, 3);
    int on_select = luaL_ref(L, LUA_REGISTRYINDEX);
    int on_query_change = LUA_NOREF;
    if (lua_gettop(L) >= 4 && lua_isfunction(L, 4)) {
        lua_pushvalue(L, 4);
        on_query_change = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    int on_key = LUA_NOREF;
    if (lua_gettop(L) >= 5 && lua_isfunction(L, 5)) {
        lua_pushvalue(L, 5);
        on_key = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    int on_select_change = LUA_NOREF;
    if (lua_gettop(L) >= 6 && lua_isfunction(L, 6)) {
        lua_pushvalue(L, 6);
        on_select_change = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    // 7th arg (optional bool): raw_results -- see Editor::OpenPicker's own
    // comment. Only mep.live_grep (kBuiltinPickerSources) passes true.
    bool raw_results = lua_gettop(L) >= 7 && lua_toboolean(L, 7);
    GetEditor(L)->OpenPicker(title, std::move(items), on_select, on_query_change == LUA_NOREF ? 0 : on_query_change,
                              on_key == LUA_NOREF ? 0 : on_key,
                              on_select_change == LUA_NOREF ? 0 : on_select_change, raw_results);
    return 0;
}

/**
 * @brief Implements mep.picker_set_items(items): replaces the open picker's item list in place.
 * @param L Lua state; arg 1 is the array of items.
 * @return Number of values pushed (0).
 */
int l_picker_set_items(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<PickerItem> items;
    ReadPickerItems(L, 1, items);
    GetEditor(L)->SetPickerItems(std::move(items));
    return 0;
}

// mep.picker_set_preview(text [, spans]): sets the text shown in the
// picker's preview column (NVIM_PARITY_PLAN.md Phase 8 gap, closed) --
// see Editor::SetPickerPreview's own comment. Pass "" to hide the column
// again (e.g. a source whose highlighted item has nothing previewable).
// `spans` (optional) is an array of {row=, col_start=, col_end=, hl=}
// (1-indexed row/col_start, col_end exclusive -- same convention as
// mep.ts_captures) giving per-span syntax highlighting over `text`; a
// span's `row` indexes 1-based into `text` split on '\n'. First consumer
// is kBuiltinPickerSources' mep.buffer_search (main.cpp).
/**
 * @brief Implements mep.picker_set_preview(text, spans): sets (or clears) the picker's preview-column text and optional per-span syntax highlighting.
 * @param L Lua state; arg 1 is the preview text ("" hides the column), optional arg 2 an array of {row=, col_start=, col_end=, hl=} spans.
 * @return Number of values pushed (0).
 */
int l_picker_set_preview(lua_State *L) {
    size_t len = 0;
    const char *text = luaL_optlstring(L, 1, "", &len);
    std::vector<PickerHlSpan> spans = ReadPreviewSpans(L, 2);
    GetEditor(L)->SetPickerPreview(std::string(text, len), std::move(spans));
    return 0;
}

// mep.picker_is_open() -> true while the fuzzy picker overlay is up, so an
// async source (e.g. kBuiltinRunners' target loader) can tell its own
// picker is still showing before calling mep.picker_set_items.
/**
 * @brief Implements mep.picker_is_open(): reports whether the picker overlay is open.
 * @param L Lua state.
 * @return Number of values pushed (1: boolean).
 */
int l_picker_is_open(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsPickerOpen());
    return 1;
}

// mep.picker_set_tabs(names, active): shows a tab strip above the open
// picker's prompt (names = array of labels, active = 1-indexed) -- see
// Editor::SetPickerTabs. The caller swaps items itself (typically from
// on_key's "<Tab>"/"<S-Tab>"); pass {} to hide the strip.
/**
 * @brief Implements mep.picker_set_tabs(names, active): sets the open picker's tab-strip labels and 1-indexed active tab.
 * @param L Lua state; arg 1 array of label strings, arg 2 1-indexed active tab.
 * @return Number of values pushed (0).
 */
int l_picker_set_tabs(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> tabs;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_isstring(L, -1)) tabs.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    int active = static_cast<int>(luaL_optinteger(L, 2, 1)) - 1;
    GetEditor(L)->SetPickerTabs(std::move(tabs), active);
    return 0;
}

/**
 * @brief Implements mep.picker_close(): closes the open picker without invoking its on_select callback.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_picker_close(lua_State *L) {
    GetEditor(L)->ClosePickerDiscardingCallbacks();
    return 0;
}

// mep.roam_graph_open(title, nodes, edges, on_select): opens the Roam
// backlink-graph view (NVIM_PARITY_PLAN.md Phase 37's flagged "no fuzzy
// backlink-graph visualization" gap, closed -- see Editor::OpenRoamGraph's
// own comment, and main.cpp's DrawRoamGraphOverlay, for the deterministic
// ring-layout scope decision this makes instead of a full force-directed
// simulation). `nodes` is an array of {id=, title=, path=, hop=} tables
// (exactly one node should have hop=0: the note the view is centered on);
// `edges` is an array of {a=, b=} 1-based indices into `nodes`, one per
// link between two nodes in the set. `on_select(path)` is called with the
// chosen node's `path` on Enter, or with no argument if the view is
// dismissed via Escape -- the same nil-on-cancel convention as
// mep.picker_open's on_select.
/**
 * @brief Implements mep.roam_graph_open(title, nodes, edges, on_select): opens the Roam backlink-graph overlay.
 * @param L Lua state; arg 1 title string, arg 2 array of {id=, title=, path=, hop=} node tables, arg 3 array of {a=, b=} 1-based edge index pairs, arg 4 on_select function called with the chosen node's path (or no argument on cancel).
 * @return Number of values pushed (0).
 */
int l_roam_graph_open(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    std::vector<RoamGraphNode> nodes;
    lua_Integer n_nodes = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n_nodes; i++) {
        lua_rawgeti(L, 2, i);
        RoamGraphNode node;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "id");
            if (lua_isstring(L, -1)) node.id = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "title");
            if (lua_isstring(L, -1)) node.title = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "path");
            if (lua_isstring(L, -1)) node.path = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "hop");
            if (lua_isnumber(L, -1)) node.hop = static_cast<int>(lua_tointeger(L, -1));
            lua_pop(L, 1);
        }
        nodes.push_back(std::move(node));
        lua_pop(L, 1);
    }

    std::vector<RoamGraphEdge> edges;
    lua_Integer n_edges = static_cast<lua_Integer>(lua_rawlen(L, 3));
    for (lua_Integer i = 1; i <= n_edges; i++) {
        lua_rawgeti(L, 3, i);
        RoamGraphEdge edge;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "a");
            int a = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
            lua_pop(L, 1);
            lua_getfield(L, -1, "b");
            int b = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
            lua_pop(L, 1);
            edge.a = a - 1;  // Lua is 1-based; RoamGraphEdge indices are 0-based.
            edge.b = b - 1;
        }
        edges.push_back(edge);
        lua_pop(L, 1);
    }

    lua_pushvalue(L, 4);
    int on_select = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->OpenRoamGraph(title, std::move(nodes), std::move(edges), on_select);
    return 0;
}

/**
 * @brief Implements mep.roam_graph_close(): closes the Roam graph overlay without invoking its on_select callback.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_roam_graph_close(lua_State *L) {
    GetEditor(L)->CloseRoamGraphDiscardingCallback();
    return 0;
}

// mep.buffer_list() -> array of {display=label, data=buffer_id (as string)}.
/**
 * @brief Implements mep.buffer_list(): lists every open buffer as {display=, data=} picker items.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of buffer entries).
 */
int l_buffer_list(lua_State *L) {
    const Editor *ed = GetEditor(L);
    // Workspace-scoped by default (WORKSPACES_PLAN.md Phase 4);
    // mep.buffer_list(true) lists every workspace's buffers.
    const bool all = lua_toboolean(L, 1) != 0;
    int n = ed->BufferCountForLua();
    lua_newtable(L);
    int out_i = 1;
    for (int id = 0; id < n; id++) {
        if (!all && !ed->BufferInActiveWorkspace(id)) continue;
        std::string label = ed->BufferLabelForLua(id);
        if (label.empty()) continue;
        lua_newtable(L);
        lua_pushstring(L, label.c_str());
        lua_setfield(L, -2, "display");
        lua_pushstring(L, std::to_string(id).c_str());
        lua_setfield(L, -2, "data");
        lua_rawseti(L, -2, out_i++);
    }
    return 1;
}

/**
 * @brief Implements mep.buffer_switch(id): switches the focused pane to show a given buffer.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (0).
 */
int l_buffer_switch(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->SwitchToBufferForLua(id);
    return 0;
}

// mep.buffer_filename(id) -> raw path, '' for a terminal/unsaved buffer.
/**
 * @brief Implements mep.buffer_filename(id): returns a buffer's file path.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: the path, or "" for a terminal/unsaved buffer).
 */
int l_buffer_filename(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushstring(L, GetEditor(L)->BufferFilenameForLua(id).c_str());
    return 1;
}

// mep.buffer_count() -> number of open buffers (cheap poll target, unlike
// mep.buffer_list() which allocates a full label table every call).
/**
 * @brief Implements mep.buffer_count(): returns the number of currently open buffers.
 * @param L Lua state.
 * @return Number of values pushed (1: the buffer count).
 */
int l_buffer_count(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->BufferCountForLua());
    return 1;
}

// mep.pane_buffers() -> array of buffer ids currently shown by a pane in
// the active tab's own split layout (Editor::PaneBuffersInActiveTab) --
// for a script that wants to "find an already-open terminal pane"
// without spawning one itself (kBuiltinTermSend).
/**
 * @brief Implements mep.pane_buffers(): lists the buffer ids shown by panes in the active tab's split layout.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of buffer ids).
 */
int l_pane_buffers(lua_State *L) {
    std::vector<int> ids = GetEditor(L)->PaneBuffersInActiveTab();
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    for (size_t i = 0; i < ids.size(); i++) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.pane_focus_buffer(id) -> bool (true if some pane in the active tab
// already shows buffer id, now focused; false, no-op, otherwise).
/**
 * @brief Implements mep.pane_focus_buffer(id): focuses whichever pane in the active tab already shows a given buffer.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: true if a pane was found and focused, false otherwise).
 */
int l_pane_focus_buffer(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->FocusPaneShowingBuffer(id));
    return 1;
}

// mep.jump_to_buffer(id) -> bool: mep.pane_focus_buffer's cross-workspace,
// cross-tab big sibling (Editor::JumpToBuffer) -- switches to the buffer's
// workspace, then the tab and pane holding it (activating a hidden buffer
// tab if that's where it is), showing it in the current pane as a last
// resort. False only for an invalid id / an unswitchable workspace.
/**
 * @brief Implements mep.jump_to_buffer(id): switches workspace/tab/pane to land on a buffer wherever it lives.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: boolean success).
 */
int l_jump_to_buffer(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->JumpToBuffer(id));
    return 1;
}

// mep.buffer_text_cols(id) -> how many text columns the pane (in the active
// tab) showing `id` draws, or nil if no pane shows it / it was never drawn
// -- see Pane::text_cols. Lets a Lua-rendered buffer (kBuiltinLanguageUiR's
// Help tab) hard-wrap prose to the pane's real width.
/**
 * @brief Implements mep.buffer_text_cols(id): returns the text column budget of the pane showing a buffer.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: the column count, or nil if no pane in the active tab shows that buffer).
 */
int l_buffer_text_cols(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int cols = GetEditor(L)->TextColsForBuffer(id);
    if (cols <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, cols);
    }
    return 1;
}

// mep.buffer_workspace(id) -> workspace id owning the buffer, or nil for an
// unscoped buffer (-1: dashboard/scratch) or an invalid id. Pair with
// mep.workspace_list() for the name.
/**
 * @brief Implements mep.buffer_workspace(id): the stable id of the workspace a buffer belongs to.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: integer workspace id, or nil).
 */
int l_buffer_workspace(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int ws = GetEditor(L)->BufferWorkspaceId(id);
    if (ws < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, ws);
    }
    return 1;
}

// mep.terminal_info(buffer_id) -> {title=, exited=, exit_code=} for a real
// `:terminal` buffer, nil otherwise. `title` is the live one: whatever the
// program last set via an OSC 0/1/2 title sequence (Claude Code keeps its
// current task summary there, which is what the AI-agents sidebar shows as
// the running task), falling back to the argv[0]/`:terminal <cmd>` label.
/**
 * @brief Implements mep.terminal_info(buffer_id): live title and exit state of a `:terminal` buffer.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: table, or nil for a non-terminal buffer).
 */
int l_terminal_info(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    const TerminalSession *sess = GetEditor(L)->GetTerminal(id);
    if (!sess) {
        lua_pushnil(L);
        return 1;
    }
    const std::string &title = (sess->vterm && !sess->vterm->Title().empty()) ? sess->vterm->Title() : sess->title;
    lua_createtable(L, 0, 3);
    lua_pushlstring(L, title.data(), title.size());
    lua_setfield(L, -2, "title");
    lua_pushboolean(L, sess->exited);
    lua_setfield(L, -2, "exited");
    lua_pushinteger(L, sess->exit_code);
    lua_setfield(L, -2, "exit_code");
    return 1;
}

// mep.buffer_cursor_row(id) -> row (1-indexed, matching mep.cursor()) of
// whichever pane in the active tab shows buffer id, or nil if no pane
// shows it -- unlike mep.cursor(), doesn't require that pane to be
// focused (kBuiltinStructure's split-pane outline uses this to track its
// source buffer's cursor while the outline pane itself has focus).
/**
 * @brief Implements mep.buffer_cursor_row(id): returns the cursor row of whichever pane in the active tab shows a given buffer.
 * @param L Lua state; arg 1 is the buffer id.
 * @return Number of values pushed (1: the 1-indexed row, or nil if no pane in the active tab shows that buffer).
 */
int l_buffer_cursor_row(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int row = GetEditor(L)->CursorRowForBuffer(id);
    if (row < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, row + 1);
    }
    return 1;
}

// mep.command_names() -> array of registered mep.command() names.
/**
 * @brief Implements mep.command_names(): lists the names of every command registered with mep.command().
 * @param L Lua state.
 * @return Number of values pushed (1: the array of command names).
 */
int l_command_names(lua_State *L) {
    std::vector<std::string> names = GetEditor(L)->LuaCommandNames();
    lua_newtable(L);
    for (size_t i = 0; i < names.size(); i++) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.colorscheme(name) -> bool (false if `name` isn't a registered palette).
/**
 * @brief Implements mep.colorscheme(name): applies a named color theme.
 * @param L Lua state; arg 1 is the theme name.
 * @return Number of values pushed (1: true on success, false if the name isn't a registered palette).
 */
int l_colorscheme(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushboolean(L, GetEditor(L)->ApplyTheme(name));
    return 1;
}

/**
 * @brief Implements mep.theme_names(): lists the names of every registered color theme.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of theme names).
 */
int l_theme_names(lua_State *L) {
    std::vector<std::string> names = GetEditor(L)->ThemeNames();
    lua_newtable(L);
    for (size_t i = 0; i < names.size(); i++) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

/**
 * @brief Implements mep.current_theme(): returns the name of the currently active color theme.
 * @param L Lua state.
 * @return Number of values pushed (1: the current theme name).
 */
int l_current_theme(lua_State *L) {
    lua_pushstring(L, GetEditor(L)->CurrentThemeName().c_str());
    return 1;
}

// Per-pane buffer tabs + auto-layouts (Phase 14).
/**
 * @brief Implements mep.pane_open(path): opens a file as a buffer tab in the focused pane.
 * @param L Lua state; arg 1 is the file path.
 * @return Number of values pushed (0).
 */
int l_pane_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->PaneOpenBufferInTab(path);
    return 0;
}
/**
 * @brief Implements mep.pane_next_buffer(): switches the focused pane to its next buffer tab.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_pane_next_buffer(lua_State *L) {
    GetEditor(L)->PaneNextBufferTab();
    return 0;
}
/**
 * @brief Implements mep.pane_prev_buffer(): switches the focused pane to its previous buffer tab.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_pane_prev_buffer(lua_State *L) {
    GetEditor(L)->PanePrevBufferTab();
    return 0;
}
/**
 * @brief Implements mep.pane_close_buffer(): closes the focused pane's current buffer tab.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_pane_close_buffer(lua_State *L) {
    GetEditor(L)->PaneCloseBufferTab();
    return 0;
}
/**
 * @brief Implements mep.pane_move_buffer(dir): moves the focused pane's current buffer tab to a neighboring pane.
 * @param L Lua state; arg 1 is the direction string.
 * @return Number of values pushed (0).
 */
int l_pane_move_buffer(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    GetEditor(L)->PaneMoveBufferTabToNeighbor(dir);
    return 0;
}
/**
 * @brief Implements mep.layout(kind): applies a named pane auto-layout.
 * @param L Lua state; arg 1 is the layout kind name.
 * @return Number of values pushed (0).
 */
int l_layout(lua_State *L) {
    const char *kind = luaL_checkstring(L, 1);
    GetEditor(L)->ApplyLayout(kind);
    return 0;
}

// mep.list_dir(path) -> array of {name=, is_dir=, mtime=}, directories first
// then files, both alphabetical (Phase 15 file tree). Editor::ListDirectory
// handles native vs. wasm (routed through the `just run-wasm` loopback bridge,
// same as :e/:w/:source -- empty when there's no bridge, e.g. a bare
// browser tab) so this is just the Lua table conversion. `mtime` (native
// builds only, always 0 under wasm) is an opaque comparable number, not a
// real timestamp -- see DirEntry::mtime's own comment (editor.h).
/**
 * @brief Implements mep.list_dir(path): lists a directory's entries, directories first then alphabetically.
 * @param L Lua state; arg 1 is the directory path.
 * @return Number of values pushed (1: the array of {name=, is_dir=, mtime=} entries).
 */
int l_list_dir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::vector<Editor::DirEntry> entries = GetEditor(L)->ListDirectory(path);
    // Sort directories before files, then alphabetically by name.
    std::sort(entries.begin(), entries.end(), [](const Editor::DirEntry &a, const Editor::DirEntry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir;  // directories first
        return a.name < b.name;
    });
    lua_newtable(L);
    for (size_t i = 0; i < entries.size(); i++) {
        lua_newtable(L);
        lua_pushstring(L, entries[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, entries[i].is_dir);
        lua_setfield(L, -2, "is_dir");
        lua_pushnumber(L, entries[i].mtime);
        lua_setfield(L, -2, "mtime");
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.is_image_path(path): thin wrapper around the native IsImagePath
// (image_doc.h/cpp, the extension check Editor::LoadFile's own IsImagePath
// branch uses) so Lua callers can filter a mep.list_dir listing down to
// images without re-duplicating the extension list -- kBuiltinFileTree's
// image-viewer toggle is the first caller.
/**
 * @brief Implements mep.is_image_path(path): checks whether a path's extension is a
 * recognized image format.
 * @param L Lua state; arg 1 is the path (only its extension is examined).
 * @return Number of values pushed (1: true/false).
 */
int l_is_image_path(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, IsImagePath(path));
    return 1;
}

// Filesystem write primitives for the file tree's create/rename/delete
// keybindings (Phase 15) -- native-only, same as list_dir/persist.h.
/**
 * @brief Implements mep.fs_mkdir(path): creates a directory (native builds only).
 * @param L Lua state; arg 1 is the directory path.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::create_directory(path, ec);
    lua_pushboolean(L, !ec);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

/**
 * @brief Implements mep.fs_create_file(path): creates an empty file if it doesn't already exist (native builds only).
 * @param L Lua state; arg 1 is the file path.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_create_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    bool ok = false;
#if !defined(__EMSCRIPTEN__)
    // NOLINTBEGIN(cppcoreguidelines-owning-memory) -- fopen/fclose matched
    // within this same scope (existence-probe pattern, not a real leak);
    // this codebase has no GSL dependency for gsl::owner<> annotations.
    FILE *f = std::fopen(path, "ab");
    if (f) {
        ok = true;
        std::fclose(f);
    }
    // NOLINTEND(cppcoreguidelines-owning-memory)
#endif
    lua_pushboolean(L, ok);
    return 1;
}

/**
 * @brief Implements mep.fs_rename(from, to): renames/moves a file or directory (native builds only), retargeting any open buffers under `from`.
 * @param L Lua state; arg 1 is the source path, arg 2 the destination path.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_rename(lua_State *L) {
    const char *from = luaL_checkstring(L, 1);
    const char *to = luaL_checkstring(L, 2);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) GetEditor(L)->RetargetBuffersForRenamedPath(from, to);
    lua_pushboolean(L, !ec);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

/**
 * @brief Implements mep.fs_delete(path): recursively deletes a file or directory (native builds only), closing any open buffers under it.
 * @param L Lua state; arg 1 is the path to delete.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_delete(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (!ec) GetEditor(L)->CloseBuffersForRemovedPath(path);
    lua_pushboolean(L, !ec);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

/**
 * @brief Implements mep.fs_copy(from, to): recursively copies a file or directory (native builds only).
 * @param L Lua state; arg 1 is the source path, arg 2 the destination path.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_copy(lua_State *L) {
    const char *from = luaL_checkstring(L, 1);
    const char *to = luaL_checkstring(L, 2);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks,
                           ec);
    lua_pushboolean(L, !ec);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

// Persisted project-root list (Phase 16), stored at
// $XDG_DATA_HOME/mep/projects.json as {"projects": [path, ...]} natively,
// or via the `just run-wasm` loopback bridge under wasm (no real filesystem/env
// vars in that sandbox to compute the path from) -- see
// Editor::ListProjects/AddProject/RemoveProject for the platform split.
/**
 * @brief Implements mep.project_list(): lists the persisted project-root paths.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of project paths).
 */
int l_project_list(lua_State *L) {
    std::vector<std::string> list = GetEditor(L)->ListProjects();
    lua_newtable(L);
    for (size_t i = 0; i < list.size(); i++) {
        lua_pushstring(L, list[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

/**
 * @brief Implements mep.project_add(path): adds a path to the persisted project-root list.
 * @param L Lua state; arg 1 is the path to add.
 * @return Number of values pushed (0).
 */
int l_project_add(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->AddProject(path);
    return 0;
}

/**
 * @brief Implements mep.project_remove(path): removes a path from the persisted project-root list.
 * @param L Lua state; arg 1 is the path to remove.
 * @return Number of values pushed (0).
 */
int l_project_remove(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->RemoveProject(path);
    return 0;
}

// --- Workspaces / projects (WORKSPACES_PLAN.md Phase 2) --------------------

/**
 * @brief Pushes one workspace as a Lua table {id, name, root, branch, primary, creating, active, index}.
 * @param L Lua state.
 * @param ws The workspace to describe.
 * @param active Whether it is the active workspace of its project.
 * @param index Its 1-based position within its project.
 */
void PushWorkspaceTable(lua_State *L, const Workspace &ws, bool active, int index) {
    lua_newtable(L);
    lua_pushinteger(L, ws.id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, ws.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, ws.root.c_str());
    lua_setfield(L, -2, "root");
    lua_pushstring(L, ws.branch.c_str());
    lua_setfield(L, -2, "branch");
    lua_pushboolean(L, ws.primary);
    lua_setfield(L, -2, "primary");
    lua_pushboolean(L, ws.creating);
    lua_setfield(L, -2, "creating");
    lua_pushboolean(L, ws.git_dirty);
    lua_setfield(L, -2, "git_dirty");
    lua_pushboolean(L, active);
    lua_setfield(L, -2, "active");
    lua_pushinteger(L, index);
    lua_setfield(L, -2, "index");
}

/**
 * @brief Resolves a Lua workspace argument: an integer is a stable workspace id, a string is a name or 1-based index.
 * @param L Lua state.
 * @param idx Stack index of the argument (nil/absent = the active workspace).
 * @return The workspace id, or -1 if nothing matches.
 */
int WorkspaceIdFromLuaArg(lua_State *L, int idx) {
    const Editor *ed = GetEditor(L);
    if (lua_isnoneornil(L, idx)) return ed->ActiveWorkspace().id;
    if (lua_isinteger(L, idx)) {
        int id = static_cast<int>(lua_tointeger(L, idx));
        return ed->FindWorkspace(id) ? id : -1;
    }
    const char *name = lua_tostring(L, idx);
    return name ? ed->ResolveWorkspaceArg(name) : -1;
}

/**
 * @brief Implements mep.workspace_list(): every workspace of the active project, in bar order.
 * @param L Lua state.
 * @return Number of values pushed (1: array of workspace tables).
 */
int l_workspace_list(lua_State *L) {
    const Editor *ed = GetEditor(L);
    const Project &project = ed->ActiveProject();
    lua_newtable(L);
    for (size_t i = 0; i < project.workspaces.size(); i++) {
        PushWorkspaceTable(L, project.workspaces[i], static_cast<int>(i) == project.active_workspace,
                           static_cast<int>(i) + 1);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

/**
 * @brief Implements mep.workspace_current(): the active workspace's table.
 * @param L Lua state.
 * @return Number of values pushed (1).
 */
int l_workspace_current(lua_State *L) {
    const Editor *ed = GetEditor(L);
    PushWorkspaceTable(L, ed->ActiveWorkspace(), true, ed->ActiveWorkspaceIndex() + 1);
    return 1;
}

/**
 * @brief Implements mep.workspace_root(): the active workspace's root directory (== mep.getcwd() by decision 4).
 * @param L Lua state.
 * @return Number of values pushed (1: the path).
 */
int l_workspace_root(lua_State *L) {
    lua_pushstring(L, GetEditor(L)->ActiveRoot().c_str());
    return 1;
}

// mep.bundled_help_root(): the installed rendered help directory. Project-local
// help/ remains supported by kBuiltinHelp, but normal projects should not
// make the built-in Help command appear to do nothing just because they do
// not carry a copy of the documentation.
int l_bundled_help_root(lua_State *L) {
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    auto is_help_root = [&ec](const std::filesystem::path &path) {
        return std::filesystem::is_regular_file(path / "intro.html", ec);
    };

#if defined(__linux__)
    // A packaged native binary lives in <prefix>/bin; its data is installed
    // in <prefix>/share/mep/help. Resolve /proc rather than relying on the
    // process CWD, which follows the active workspace.
    std::array<char, 4096> exe_path{};
    ssize_t len = readlink("/proc/self/exe", exe_path.data(), exe_path.size() - 1);
    if (len > 0) {
        std::filesystem::path installed = std::filesystem::path(std::string(exe_path.data(), static_cast<size_t>(len)))
                                              .parent_path().parent_path() / "share/mep/help";
        if (is_help_root(installed)) {
            lua_pushstring(L, installed.string().c_str());
            return 1;
        }
    }
#endif

#if defined(MEP_SOURCE_HELP_DIR)
    std::filesystem::path source = MEP_SOURCE_HELP_DIR;
    if (is_help_root(source)) {
        lua_pushstring(L, source.string().c_str());
        return 1;
    }
#endif
#endif
    lua_pushliteral(L, "");
    return 1;
}

/**
 * @brief Implements mep.workspace_new(name [, attach_existing]): `:wsnew[!] name`.
 * @param L Lua state; arg 1 is the name, optional arg 2 attaches to an existing branch.
 * @return Number of values pushed (0).
 */
int l_workspace_new(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    GetEditor(L)->WorkspaceCreate(name, lua_toboolean(L, 2) != 0);
    return 0;
}

/**
 * @brief Implements mep.workspace_switch(id|name): activates a workspace.
 * @param L Lua state.
 * @return Number of values pushed (1: true on success).
 */
int l_workspace_switch(lua_State *L) {
    int id = WorkspaceIdFromLuaArg(L, 1);
    lua_pushboolean(L, id >= 0 && GetEditor(L)->WorkspaceSwitch(id));
    return 1;
}

/**
 * @brief Implements mep.workspace_delete(id|name [, force]): `:wsdelete[!]`.
 * @param L Lua state.
 * @return Number of values pushed (1: true on success).
 */
int l_workspace_delete(lua_State *L) {
    Editor *ed = GetEditor(L);
    std::string arg;
    if (lua_isinteger(L, 1)) {
        const Workspace *ws = ed->FindWorkspace(static_cast<int>(lua_tointeger(L, 1)));
        if (!ws) {
            lua_pushboolean(L, false);
            return 1;
        }
        arg = ws->name;
    } else if (const char *name = lua_tostring(L, 1)) {
        arg = name;
    }
    const int before = ed->WorkspaceCount();
    ed->WorkspaceRemove(arg, lua_toboolean(L, 2) != 0);
    lua_pushboolean(L, ed->WorkspaceCount() < before);
    return 1;
}

/**
 * @brief Implements mep.workspace_reset([force]) -> ok, err: empties the active workspace back to one fresh pane.
 * @param L Lua state.
 * @return Number of values pushed (1 on success: true; 2 on failure: false plus the status-line reason).
 */
int l_workspace_reset(lua_State *L) {
    Editor *ed = GetEditor(L);
    if (ed->WorkspaceReset(ed->ActiveWorkspace().id, lua_toboolean(L, 1) != 0)) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, false);
    lua_pushstring(L, ed->StatusMessage().c_str());
    return 2;
}

/**
 * @brief Implements mep.workspace_rename(id|name, new_name).
 * @param L Lua state.
 * @return Number of values pushed (1: true on success).
 */
int l_workspace_rename(lua_State *L) {
    int id = WorkspaceIdFromLuaArg(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lua_pushboolean(L, id >= 0 && GetEditor(L)->WorkspaceRename(id, name));
    return 1;
}

/**
 * @brief Implements mep.workspace_next() / mep.workspace_previous().
 */
int l_workspace_next(lua_State *L) {
    GetEditor(L)->WorkspaceNext();
    return 0;
}
int l_workspace_previous(lua_State *L) {
    GetEditor(L)->WorkspacePrevious();
    return 0;
}

/** @brief Pushes one project as {id, name, root, is_git, git_toplevel, workspace_count, active, index}. */
void PushProjectTable(lua_State *L, const Project &p, bool active, int index) {
    lua_newtable(L);
    lua_pushinteger(L, p.id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, p.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushstring(L, p.root.c_str());
    lua_setfield(L, -2, "root");
    lua_pushboolean(L, p.is_git);
    lua_setfield(L, -2, "is_git");
    lua_pushstring(L, p.git_toplevel.c_str());
    lua_setfield(L, -2, "git_toplevel");
    lua_pushinteger(L, static_cast<lua_Integer>(p.workspaces.size()));
    lua_setfield(L, -2, "workspace_count");
    lua_pushboolean(L, active);
    lua_setfield(L, -2, "active");
    lua_pushinteger(L, index);
    lua_setfield(L, -2, "index");
}

/** @brief Integer = project id, string = name / 1-based index / root; nil = active project. */
int ProjectIdFromLuaArg(lua_State *L, int idx) {
    const Editor *ed = GetEditor(L);
    if (lua_isnoneornil(L, idx)) return ed->ActiveProject().id;
    if (lua_isinteger(L, idx)) {
        int id = static_cast<int>(lua_tointeger(L, idx));
        return ed->FindProject(id) ? id : -1;
    }
    const char *name = lua_tostring(L, idx);
    return name ? ed->ResolveProjectArg(name) : -1;
}

/** @brief Implements mep.project_load(dir) -> id, restored: loads (or switches to) a project (WORKSPACES_PLAN.md Phase 9). */
int l_project_load(lua_State *L) {
    bool restored = false;
    int id = GetEditor(L)->ProjectLoad(luaL_checkstring(L, 1), &restored);
    if (id < 0) {
        lua_pushnil(L);
        lua_pushboolean(L, false);
        return 2;
    }
    lua_pushinteger(L, id);
    lua_pushboolean(L, restored);
    return 2;
}
/** @brief Implements mep.project_current(). */
int l_project_current(lua_State *L) {
    const Editor *ed = GetEditor(L);
    PushProjectTable(L, ed->ActiveProject(), true, ed->ActiveProjectIndex() + 1);
    return 1;
}
/** @brief Implements mep.project_loaded_list(): every loaded project, in load order. */
int l_project_loaded_list(lua_State *L) {
    const Editor *ed = GetEditor(L);
    lua_newtable(L);
    for (int i = 0; i < ed->ProjectCount(); i++) {
        PushProjectTable(L, ed->ProjectAt(i), i == ed->ActiveProjectIndex(), i + 1);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
/** @brief Implements mep.project_switch(id|name). */
int l_project_switch(lua_State *L) {
    int id = ProjectIdFromLuaArg(L, 1);
    lua_pushboolean(L, id >= 0 && GetEditor(L)->ProjectSwitch(id));
    return 1;
}
/** @brief Implements mep.project_close(id|name [, force]). */
int l_project_close(lua_State *L) {
    int id = ProjectIdFromLuaArg(L, 1);
    lua_pushboolean(L, id >= 0 && GetEditor(L)->ProjectClose(id, lua_toboolean(L, 2) != 0));
    return 1;
}
int l_project_next(lua_State *L) {
    GetEditor(L)->ProjectNext();
    return 0;
}
int l_project_previous(lua_State *L) {
    GetEditor(L)->ProjectPrevious();
    return 0;
}
/** @brief Implements mep.workspace_state_save() / mep.workspace_state_restore(): `:wssave` / `:wsrestore`. */
int l_workspace_state_save(lua_State *L) {
    Editor *ed = GetEditor(L);
    lua_pushboolean(L, ed->SaveWorkspaceState(ed->ActiveProject().id));
    return 1;
}
int l_workspace_state_restore(lua_State *L) {
    Editor *ed = GetEditor(L);
    lua_pushboolean(L, ed->RestoreWorkspaceState(ed->ActiveProject().id, false));
    return 1;
}

/** @brief Implements mep.workspace_adopt(path_or_branch): `:wsadopt`. */
int l_workspace_adopt(lua_State *L) {
    GetEditor(L)->WorkspaceAdopt(luaL_checkstring(L, 1));
    return 0;
}
/** @brief Implements mep.workspace_prune(): `:wsprune`. */
int l_workspace_prune(lua_State *L) {
    GetEditor(L)->WorkspacePrune();
    return 0;
}
/** @brief Implements mep.workspace_set_git_dirty(id, bool): Phase 7's optional `+` marker. */
int l_workspace_set_git_dirty(lua_State *L) {
    Workspace *ws = GetEditor(L)->FindWorkspace(static_cast<int>(luaL_checkinteger(L, 1)));
    if (ws) ws->git_dirty = lua_toboolean(L, 2) != 0;
    return 0;
}
/** @brief Implements mep.workspace_set_restore(bool): backs mep.opt.restore_workspaces. */
int l_workspace_set_restore(lua_State *L) {
    GetEditor(L)->SetRestoreWorkspaces(lua_toboolean(L, 1) != 0);
    return 0;
}
/** @brief Implements mep.workspace_set_worktree_dir(dir): backs mep.opt.worktree_dir. */
int l_workspace_set_worktree_dir(lua_State *L) {
    const char *dir = lua_tostring(L, 1);
    GetEditor(L)->SetWorktreeDirOverride(dir ? dir : "");
    return 0;
}

/**
 * @brief Implements mep.workspace_change_epoch(): counter bumped on every workspace/project change, for mep.on_workspace_changed.
 * @param L Lua state.
 * @return Number of values pushed (1).
 */
int l_workspace_change_epoch(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->WorkspaceChangeEpoch());
    return 1;
}

/**
 * @brief Implements mep.getcwd(): returns the process's current working directory (native builds only).
 * @param L Lua state.
 * @return Number of values pushed (1: the cwd path, or "" on error or under wasm).
 */
int l_getcwd(lua_State *L) {
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    lua_pushstring(L, ec ? "" : cwd.c_str());
#else
    lua_pushstring(L, "");
#endif
    return 1;
}

/**
 * @brief Implements mep.chdir(path): changes the process's current working directory (native builds only).
 * @param L Lua state; arg 1 is the target directory.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_chdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::current_path(path, ec);
    lua_pushboolean(L, !ec);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

// Generic JSON<->Lua marshaling (NVIM_PARITY_PLAN.md Part V Phase 20): LSP
// (and later DAP) messages are arbitrarily-shaped JSON, so a one-off
// per-field reader (the pattern every earlier Lua binding in this file
// uses) doesn't scale -- this is the first consumer that genuinely needs
// generic conversion in both directions.
//
// A JSON array marshals to a Lua array-table (1-indexed, `n` field for
// disambiguating a trailing-nil/empty array from an object); a JSON object
// marshals to a plain Lua table keyed by string; JSON null marshals to a
// unique lightuserdata sentinel (not Lua nil) so an object field
// explicitly set to null survives the round trip distinguishably from an
// absent field -- callers that don't care can treat it as falsy.
void *kJsonNullSentinel = reinterpret_cast<void *>(0x1);

/**
 * @brief Recursively pushes a Json value onto the Lua stack, encoding JSON null as a unique lightuserdata sentinel rather than Lua nil so an object field explicitly set to null survives the round trip distinguishably from an absent field.
 * @param L Lua state.
 * @param v Json value to push.
 */
void PushJson(lua_State *L, const Json &v) {
    switch (v.type()) {
        case Json::Type::Null:
            lua_pushlightuserdata(L, kJsonNullSentinel);
            break;
        case Json::Type::Bool:
            lua_pushboolean(L, v.as_bool());
            break;
        case Json::Type::Number:
            lua_pushnumber(L, v.as_double());
            break;
        case Json::Type::String:
            lua_pushlstring(L, v.as_string().data(), v.as_string().size());
            break;
        case Json::Type::Array: {
            lua_newtable(L);
            const auto &items = v.items();
            for (size_t i = 0; i < items.size(); i++) {
                PushJson(L, items[i]);
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            break;
        }
        case Json::Type::Object: {
            lua_newtable(L);
            for (const auto &kv : v.fields()) {
                PushJson(L, kv.second);
                lua_setfield(L, -2, kv.first.c_str());
            }
            break;
        }
    }
}

/**
 * @brief Recursively converts a Lua value at a stack index into a Json value; a table with a contiguous 1..n integer key run becomes a Json array, otherwise a Json object keyed by its string fields.
 * @param L Lua state.
 * @param idx Stack index of the value to convert.
 * @return The converted Json value.
 */
Json LuaToJson(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);
    int t = lua_type(L, idx);
    if (t == LUA_TNIL) return Json();
    if (t == LUA_TLIGHTUSERDATA && lua_touserdata(L, idx) == kJsonNullSentinel) return Json();
    if (t == LUA_TBOOLEAN) return Json(static_cast<bool>(lua_toboolean(L, idx)));
    if (t == LUA_TNUMBER) return Json(lua_tonumber(L, idx));
    if (t == LUA_TSTRING) {
        size_t len = 0;
        const char *s = lua_tolstring(L, idx, &len);
        return Json(std::string(s, len));
    }
    if (t == LUA_TTABLE) {
        // Array if it has a contiguous 1..n integer key run (lua_rawlen);
        // Lua tables built by literal `{...}` syntax or by this file's own
        // sequential push loops always satisfy that. Otherwise, object.
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
        if (n > 0) {
            Json arr = Json::Array();
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, i);
                arr.push_back(LuaToJson(L, -1));
                lua_pop(L, 1);
            }
            return arr;
        }
        Json obj = Json::Object();
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                obj[lua_tostring(L, -2)] = LuaToJson(L, -1);
            }
            lua_pop(L, 1);
        }
        return obj;
    }
    return Json();
}

// Same as PushJson, except JSON null pushes as a real Lua nil rather
// than the kJsonNullSentinel, *including nested nulls* (this recurses
// into itself, not PushJson, for array/object children -- a thin
// wrapper delegating non-null values to PushJson would still sentinel-
// encode nulls nested inside them). kBuiltinAi's own mep_ai_json_decode
// (LUA_TO_CPP_PLAN.md Phase AI) always returned plain nil for null
// (its own hand-rolled decoder predates/is independent of the LSP
// null-sentinel convention, and downstream AI/Leetcode code relies on
// a null field reading falsy, e.g. `tool_calls[idx].id` from a
// streamed delta that hasn't set an id yet) -- ported to reuse this
// file's own Json engine (now with real surrogate-pair \u handling,
// json.h) without changing that observable behavior.
/**
 * @brief Same as PushJson, but pushes JSON null (including nested nulls) as a real Lua nil instead of the sentinel, and integer-subtypes any numerically whole JSON number so tostring() shows "1" rather than "1.0".
 * @param L Lua state.
 * @param v Json value to push.
 */
void PushJsonNilNull(lua_State *L, const Json &v) {
    switch (v.type()) {
        case Json::Type::Null:
            lua_pushnil(L);
            break;
        case Json::Type::Bool:
            lua_pushboolean(L, v.as_bool());
            break;
        case Json::Type::Number: {
            // The original hand-rolled decoder's own `tonumber(...)` gives a
            // real Lua *integer* subtype for source text with no '.'/'e'
            // (so `tostring()` shows "1", not "1.0") -- Json::Number only
            // ever stores a double, losing that source-text distinction, so
            // this approximates it by integer-subtyping any numerically
            // whole value instead (the overwhelmingly common real case --
            // token counts, tool-call/content-block indices -- and only
            // differs from the original for a JSON number deliberately
            // written with a redundant ".0" that's still numerically whole,
            // which round-tripped as a Lua float either way there too).
            double d = v.as_double();
            long long as_ll = static_cast<long long>(d);
            if (static_cast<double>(as_ll) == d) {
                lua_pushinteger(L, as_ll);
            } else {
                lua_pushnumber(L, d);
            }
            break;
        }
        case Json::Type::String:
            lua_pushlstring(L, v.as_string().data(), v.as_string().size());
            break;
        case Json::Type::Array: {
            lua_newtable(L);
            const auto &items = v.items();
            for (size_t i = 0; i < items.size(); i++) {
                PushJsonNilNull(L, items[i]);
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            break;
        }
        case Json::Type::Object: {
            lua_newtable(L);
            for (const auto &kv : v.fields()) {
                PushJsonNilNull(L, kv.second);
                lua_setfield(L, -2, kv.first.c_str());
            }
            break;
        }
    }
}

// LSP documentSymbol kind numbers -> the same short names
// kBuiltinSymbols' own MEP_SYMBOL_KIND table used to map (main.cpp) --
// https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#symbolKind,
// trimmed to the subset that table covered rather than all 26.
/**
 * @brief Maps an LSP SymbolKind number to its short display name, matching the subset kBuiltinSymbols' own lookup table covered.
 * @param kind LSP SymbolKind integer.
 * @return The short kind name, or "?" if unrecognized.
 */
const char *LspSymbolKindName(int kind) {
    switch (kind) {
        case 2: return "module";
        case 5: return "class";
        case 6: return "method";
        case 7: return "property";
        case 9: return "enum";
        case 10: return "enummember";
        case 12: return "function";
        case 13: return "variable";
        case 14: return "constant";
        default: return "?";
    }
}

struct LspSymbolRow {
    int row = 0;
    std::string text;
};

// Recursively flattens an LSP textDocument/documentSymbol response (an
// array of DocumentSymbol objects, each possibly nested via `children`)
// into a depth-first list of display rows -- the tree-walk half of
// kBuiltinSymbols' mep.lsp_symbols_refresh (main.cpp), which used to be a
// local recursive `add(sym, depth)` Lua closure. `row` is 0-indexed here;
// the caller (mep.set_cursor via a Lua on_click closure, since sidebar
// widgets are still Lua-ref-driven) adds 1 back.
/**
 * @brief Recursively flattens an LSP documentSymbol response (each entry possibly nested via `children`) into a depth-first list of display rows.
 * @param syms Array of LSP DocumentSymbol JSON objects.
 * @param depth Current nesting depth, used to indent each row's text.
 * @param out Vector appended with one {row, text} entry per symbol, depth-first.
 */
void FlattenLspSymbols(const Json &syms, int depth, std::vector<LspSymbolRow> *out) {
    for (const Json &sym : syms.items()) {
        const Json &range = sym.contains("range") && !sym.get("range").is_null() ? sym.get("range")
                                                                                   : sym.get("location").get("range");
        int row = range.is_object() ? range.get("start").get("line").as_int(0) : 0;
        std::string text(static_cast<size_t>(depth) * 2, ' ');
        text += sym.get("name").as_string();
        text += "  [";
        text += LspSymbolKindName(sym.get("kind").as_int(0));
        text += "]";
        out->push_back({row, std::move(text)});
        const Json &children = sym.get("children");
        if (children.is_array()) FlattenLspSymbols(children, depth + 1, out);
    }
}

// mep.lsp_symbols_flatten(result) -> array of {row, text} (1-indexed row),
// depth-first over `result` (a textDocument/documentSymbol response, once
// mep_lsp_result has already unwrapped it from the raw JSON-RPC message).
namespace {
// mep_doc_split_param's own port: "name: type" (python/typescript/rust/
// kotlin convention) first, else "type name" (c/go/java convention, name
// = trailing identifier) -- see kBuiltinDocs' own comment (main.cpp) for
// why this is a best-effort heuristic, not a real per-grammar parser.
struct DocSigParam {
    std::string name;
    std::string type;
    bool has_type = false;
};

/**
 * @brief Splits a signature-help parameter label into a name and (if present) a type, trying "name: type" first and falling back to "type name".
 * @param label Raw parameter label text.
 * @return The parsed name/type/has_type triple.
 */
DocSigParam SplitDocParam(const std::string &label) {
    // "name: type"
    size_t k = 0;
    while (k < label.size() && (std::isalnum(static_cast<unsigned char>(label[k])) || label[k] == '_')) k++;
    if (k > 0) {
        size_t p = k;
        while (p < label.size() && std::isspace(static_cast<unsigned char>(label[p]))) p++;
        if (p < label.size() && label[p] == ':') {
            p++;
            while (p < label.size() && std::isspace(static_cast<unsigned char>(label[p]))) p++;
            if (p < label.size()) return {label.substr(0, k), label.substr(p), true};
        }
    }
    // "type name" -- name is the maximal trailing [%w_] run, preceded by
    // whitespace with a non-empty type before it.
    size_t name_start = label.size();
    while (name_start > 0 &&
           (std::isalnum(static_cast<unsigned char>(label[name_start - 1])) || label[name_start - 1] == '_')) {
        name_start--;
    }
    if (name_start < label.size() && name_start > 0) {
        size_t ws_start = name_start;
        while (ws_start > 0 && std::isspace(static_cast<unsigned char>(label[ws_start - 1]))) ws_start--;
        if (ws_start < name_start) {
            std::string ptype = label.substr(0, ws_start);
            if (!ptype.empty()) return {label.substr(name_start), ptype, true};
        }
    }
    return {label, "", false};
}

// mep_doc_return_type's own port: best-effort scrape of the trailing
// "-> Type" (python/rust) or ": Type" (typescript) after the first ')'
// that's immediately (modulo whitespace) followed by one of those tokens.
/**
 * @brief Scrapes the best-effort return type following the first ')' immediately (modulo whitespace) followed by a given token, such as "->" or ":".
 * @param sig Full signature text to scan.
 * @param token The token expected right after ')' (e.g. "->" or ":").
 * @return The trimmed return-type text, or "" if the token wasn't found after any ')'.
 */
std::string DocReturnTypeAfterToken(const std::string &sig, const std::string &token) {
    for (size_t i = 0; i < sig.size(); i++) {
        if (sig[i] != ')') continue;
        size_t p = i + 1;
        while (p < sig.size() && std::isspace(static_cast<unsigned char>(sig[p]))) p++;
        if (sig.compare(p, token.size(), token) == 0) {
            p += token.size();
            while (p < sig.size() && std::isspace(static_cast<unsigned char>(sig[p]))) p++;
            size_t end = sig.size();
            while (end > p && std::isspace(static_cast<unsigned char>(sig[end - 1]))) end--;
            if (end > p) return sig.substr(p, end - p);
        }
    }
    return "";
}

/**
 * @brief Best-effort return-type scrape trying the "->" (python/rust) convention first, then ":" (typescript).
 * @param sig Full signature text to scan.
 * @return Whether a return type was found, paired with the scraped type text (empty if not found).
 */
std::pair<bool, std::string> DocReturnType(const std::string &sig) {
    std::string r = DocReturnTypeAfterToken(sig, "->");
    if (!r.empty()) return {true, r};
    r = DocReturnTypeAfterToken(sig, ":");
    if (!r.empty()) return {true, r};
    return {false, ""};
}

// mep_doc_params_from_signature's own port, operating on the Json form
// of an LSP SignatureInformation object (`active`, already unwrapped from
// the JSON-RPC response by the still-Lua caller).
/**
 * @brief Parses the parameter list out of an LSP SignatureInformation JSON object's active signature, resolving each parameter's label (either a plain string or a [start,end] offset pair into the signature label) and splitting it into name/type.
 * @param active The JSON SignatureInformation object.
 * @return The parsed parameters, in declaration order; parameters with no recoverable label text are omitted.
 */
std::vector<DocSigParam> DocParamsFromSignature(const Json &active) {
    std::string label = active.get("label").as_string();
    std::vector<DocSigParam> params;
    const Json &parameters = active.get("parameters");
    if (!parameters.is_array()) return params;
    for (const Json &p : parameters.items()) {
        const Json &plabel = p.get("label");
        std::string ptext;
        bool have_text = false;
        if (plabel.is_string()) {
            ptext = plabel.as_string();
            have_text = true;
        } else if (plabel.is_array() && plabel.items().size() >= 2) {
            int start = std::max(0, plabel.items()[0].as_int(0));
            int end = std::min(static_cast<int>(label.size()), plabel.items()[1].as_int(static_cast<int>(label.size())));
            if (end > start) {
                ptext = label.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
                have_text = true;
            }
        }
        if (have_text && !ptext.empty()) params.push_back(SplitDocParam(ptext));
    }
    return params;
}
}  // namespace

// mep.docs_signature_info(active, sig) -> label, params, ret: given an
// LSP SignatureInformation object (`active`) and the syntactic-fallback
// signature text (`sig`), returns the label to use (active.label if
// non-empty, else `sig`), the parsed parameter list (array of
// {name=, type=}, type nil when none was recoverable), and the
// best-effort return type scraped off the label (nil if none found) --
// kBuiltinDocs' own mep_doc_params_from_signature + mep_doc_return_type +
// the label-fallback line that used to precede them (main.cpp), now all
// in one call.
/**
 * @brief Implements mep.docs_signature_info(active, sig): given an LSP SignatureInformation object and a syntactic-fallback signature string, returns the label to display, the parsed parameter list, and a best-effort scraped return type.
 * @param L Lua state; arg 1 is the LSP SignatureInformation table, arg 2 the fallback signature text.
 * @return Number of values pushed (3: label string, params array of {name=, type=}, return type string or nil).
 */
int l_docs_signature_info(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *sig = luaL_checkstring(L, 2);
    Json active = LuaToJson(L, 1);
    std::string active_label = active.get("label").as_string();
    std::string label = !active_label.empty() ? active_label : sig;

    std::vector<DocSigParam> params = DocParamsFromSignature(active);
    std::pair<bool, std::string> ret = DocReturnType(label);

    lua_pushlstring(L, label.data(), label.size());
    lua_createtable(L, static_cast<int>(params.size()), 0);
    for (size_t i = 0; i < params.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushlstring(L, params[i].name.data(), params[i].name.size());
        lua_setfield(L, -2, "name");
        if (params[i].has_type) {
            lua_pushlstring(L, params[i].type.data(), params[i].type.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "type");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    if (ret.first) {
        lua_pushlstring(L, ret.second.data(), ret.second.size());
    } else {
        lua_pushnil(L);
    }
    return 3;
}

// mep.picker_preview_file(path, max_lines): see Editor::PreviewFile.
/**
 * @brief Implements mep.picker_preview_file(path, max_lines): loads a file's leading lines into the picker's preview column.
 * @param L Lua state; arg 1 is the file path, optional arg 2 the maximum line count (default 40).
 * @return Number of values pushed (0).
 */
int l_picker_preview_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int max_lines = static_cast<int>(luaL_optinteger(L, 2, 40));
    GetEditor(L)->PreviewFile(path, max_lines);
    return 0;
}

// mep.tree_build_rows(root, expanded_list, show_hidden, ignored_list) ->
// array of {path=, name=, is_dir=, depth=, expanded=}: see
// Editor::BuildFileTreeRows. `expanded_list`/`ignored_list` are plain
// arrays of path strings (the caller's own mep_tree_expanded/
// mep_tree_ignored tables, flattened via `for k in pairs(t) do ... end`
// since Lua set-membership tables aren't naturally iterable as C arrays).
/**
 * @brief Implements mep.tree_build_rows(root, expanded_list, show_hidden, ignored_list): builds the flattened row list for the file-tree sidebar.
 * @param L Lua state; arg 1 is the root directory, arg 2 an array of expanded directory paths, arg 3 whether to show hidden entries, arg 4 an array of ignored paths.
 * @return Number of values pushed (1: the array of {path=, name=, is_dir=, depth=, expanded=} rows).
 */
int l_tree_build_rows(lua_State *L) {
    const char *root = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    bool show_hidden = lua_toboolean(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);

    std::vector<std::string> expanded;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isstring(L, -1)) expanded.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    std::vector<std::string> ignored;
    n = static_cast<lua_Integer>(lua_rawlen(L, 4));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 4, i);
        if (lua_isstring(L, -1)) ignored.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    std::vector<Editor::FileTreeRow> rows = GetEditor(L)->BuildFileTreeRows(root, expanded, show_hidden, ignored);
    lua_createtable(L, static_cast<int>(rows.size()), 0);
    for (size_t i = 0; i < rows.size(); i++) {
        lua_createtable(L, 0, 5);
        lua_pushlstring(L, rows[i].full_path.data(), rows[i].full_path.size());
        lua_setfield(L, -2, "path");
        lua_pushlstring(L, rows[i].name.data(), rows[i].name.size());
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, rows[i].is_dir);
        lua_setfield(L, -2, "is_dir");
        lua_pushinteger(L, rows[i].depth);
        lua_setfield(L, -2, "depth");
        lua_pushboolean(L, rows[i].expanded);
        lua_setfield(L, -2, "expanded");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.project_readme_path(dir) -> path or nil: see Editor::ProjectReadmePath.
/**
 * @brief Implements mep.project_readme_path(dir): finds a directory's README file, if any.
 * @param L Lua state; arg 1 is the directory path.
 * @return Number of values pushed (1: the README path, or nil if none was found).
 */
int l_project_readme_path(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    std::string path = GetEditor(L)->ProjectReadmePath(dir);
    if (path.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, path.data(), path.size());
    }
    return 1;
}

// mep.colorize(): see Editor::Colorize. Bound directly, no wrapper.
/**
 * @brief Implements mep.colorize(): re-runs syntax highlighting for the current buffer.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_colorize(lua_State *L) {
    GetEditor(L)->Colorize();
    return 0;
}

// mep.url_under_cursor() -> string or nil: see Editor::UrlUnderCursor.
// Bound directly, no wrapper.
/**
 * @brief Implements mep.url_under_cursor(): returns the URL under the cursor, if any.
 * @param L Lua state.
 * @return Number of values pushed (1: the URL string, or nil if the cursor isn't over one).
 */
int l_url_under_cursor(lua_State *L) {
    std::string url = GetEditor(L)->UrlUnderCursor();
    if (url.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, url.data(), url.size());
    }
    return 1;
}

// mep.list_urls_scan() -> array of URL strings: see Editor::ListUrls.
// kBuiltinTextTools' own mep.list_urls (main.cpp) wraps this with the
// picker.
/**
 * @brief Implements mep.list_urls_scan(): scans the current buffer for URLs.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of URL strings found).
 */
int l_list_urls_scan(lua_State *L) {
    std::vector<std::string> urls = GetEditor(L)->ListUrls();
    lua_createtable(L, static_cast<int>(urls.size()), 0);
    for (size_t i = 0; i < urls.size(); i++) {
        lua_pushlstring(L, urls[i].data(), urls[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

namespace {
// SGR (Select Graphic Rendition) color codes -> mep highlight group --
// mep_ansi_sgr_hl's own port (kBuiltinRun, main.cpp).
/**
 * @brief Maps an SGR (Select Graphic Rendition) color code to the matching mep highlight group name.
 * @param code SGR parameter code (e.g. 31 or 91 for red).
 * @return The highlight group name, or nullptr if the code isn't a recognized color.
 */
const char *AnsiSgrHl(int code) {
    switch (code) {
        case 31:
        case 91:
            return "Red";
        case 32:
        case 92:
            return "Green";
        case 33:
        case 93:
            return "Yellow";
        case 34:
        case 94:
            return "Blue";
        case 35:
        case 95:
            return "Purple";
        case 36:
        case 96:
            return "Cyan";
        default:
            return nullptr;
    }
}

struct AnsiSpan {
    int row;  // 1-indexed
    int col_start;
    int col_end;
    std::string hl;
};

struct AnsiRenderResult {
    std::vector<std::string> lines;  // lines[0] is Lua's lines[1], etc.
    std::vector<AnsiSpan> spans;
};

// mep_ansi_render's own port: re-parses a raw accumulated output byte
// stream into plain lines + SGR color-span decorations every call (same
// "re-parse from scratch, not incremental" choice the original made --
// see kBuiltinRun's own header comment on why). Only `m` (SGR) escape
// sequences are interpreted; every other CSI sequence is skipped over
// (consumed, not rendered) -- this is color-only handling, not a real
// cursor-addressable terminal grid.
/**
 * @brief Re-parses a raw accumulated terminal output byte stream into plain lines plus SGR color-span decorations; only 'm' (SGR) escape sequences are interpreted, every other CSI sequence is consumed but ignored.
 * @param raw Raw output bytes to parse, potentially containing ANSI escape sequences.
 * @return The split lines and the color spans found over them.
 */
AnsiRenderResult AnsiRender(const std::string &raw) {
    AnsiRenderResult result;
    result.lines.emplace_back();
    int row = 1;
    bool has_hl = false;
    std::string cur_hl;
    int span_start = -1;  // -1 = no open span
    /**
     * @brief Closes the currently open highlight span (if any) at a given end column, appending it to the result's span list.
     * @param end_col Column at which the open span ends.
     */
    auto close_span = [&](int end_col) {
        if (has_hl && span_start >= 0) result.spans.push_back({row, span_start, end_col, cur_hl});
        span_start = -1;
    };
    size_t i = 0;
    const size_t n = raw.size();
    while (i < n) {
        char c = raw[i];
        if (c == '\x1b' && i + 1 < n && raw[i + 1] == '[') {
            size_t seq_end = i + 2;
            while (seq_end < n && !std::isalpha(static_cast<unsigned char>(raw[seq_end]))) seq_end++;
            if (seq_end >= n) break;  // no terminating letter -- matches "if not seq_end then break"
            std::string params = raw.substr(i + 2, seq_end - (i + 2));
            char cmd = raw[seq_end];
            if (cmd == 'm') {
                close_span(static_cast<int>(result.lines[static_cast<size_t>(row - 1)].size()) + 1);
                bool new_has_hl = has_hl;
                std::string new_hl = cur_hl;
                std::string with_sep = params + ";";
                size_t seg_start = 0;
                for (size_t p = 0; p < with_sep.size(); p++) {
                    if (with_sep[p] != ';') continue;
                    std::string code = with_sep.substr(seg_start, p - seg_start);
                    seg_start = p + 1;
                    bool all_digits = !code.empty();
                    for (char ch : code) {
                        if (!std::isdigit(static_cast<unsigned char>(ch))) {
                            all_digits = false;
                            break;
                        }
                    }
                    if (code.empty() || code == "0") {
                        new_has_hl = false;
                        new_hl.clear();
                    } else if (all_digits) {
                        // Malformed (non-digit) codes are skipped -- keeps
                        // whatever new_hl already was, same as Lua's own
                        // `mep_ansi_sgr_hl(code) or new_hl` for an
                        // unrecognized-but-numeric code; a non-numeric
                        // code can't happen from `tonumber` in the
                        // original (it'd just yield nil, same
                        // fall-through) so this mirrors it defensively
                        // instead of risking a parse throw.
                        const char *hl = AnsiSgrHl(std::stoi(code));
                        if (hl) {
                            new_has_hl = true;
                            new_hl = hl;
                        }
                    }
                }
                has_hl = new_has_hl;
                cur_hl = new_hl;
                if (has_hl) span_start = static_cast<int>(result.lines[static_cast<size_t>(row - 1)].size()) + 1;
            }
            i = seq_end + 1;
        } else if (c == '\n') {
            close_span(static_cast<int>(result.lines[static_cast<size_t>(row - 1)].size()) + 1);
            row++;
            result.lines.emplace_back();
            if (has_hl) span_start = 1;
            i++;
        } else if (c == '\r') {
            i++;
        } else {
            result.lines[static_cast<size_t>(row - 1)] += c;
            i++;
        }
    }
    close_span(static_cast<int>(result.lines[static_cast<size_t>(row - 1)].size()) + 1);
    return result;
}
}  // namespace

// mep.ansi_render(raw) -> lines, spans: see AnsiRender above.
// kBuiltinRun's own mep_term_redraw (main.cpp) feeds `spans` straight
// into mep.buffer_deco_add, same {row=, col_start=, col_end=, hl=} shape
// mep_ansi_render always returned.
/**
 * @brief Implements mep.ansi_render(raw): parses raw terminal output into plain lines and SGR color-highlight spans.
 * @param L Lua state; arg 1 is the raw output text.
 * @return Number of values pushed (2: the array of lines, and the array of {row=, col_start=, col_end=, hl=} spans).
 */
int l_ansi_render(lua_State *L) {
    const char *raw = luaL_checkstring(L, 1);
    AnsiRenderResult result = AnsiRender(raw);
    lua_createtable(L, static_cast<int>(result.lines.size()), 0);
    for (size_t k = 0; k < result.lines.size(); k++) {
        lua_pushlstring(L, result.lines[k].data(), result.lines[k].size());
        lua_rawseti(L, -2, static_cast<int>(k + 1));
    }
    lua_createtable(L, static_cast<int>(result.spans.size()), 0);
    for (size_t k = 0; k < result.spans.size(); k++) {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, result.spans[k].row);
        lua_setfield(L, -2, "row");
        lua_pushinteger(L, result.spans[k].col_start);
        lua_setfield(L, -2, "col_start");
        lua_pushinteger(L, result.spans[k].col_end);
        lua_setfield(L, -2, "col_end");
        lua_pushlstring(L, result.spans[k].hl.data(), result.spans[k].hl.size());
        lua_setfield(L, -2, "hl");
        lua_rawseti(L, -2, static_cast<int>(k + 1));
    }
    return 2;
}

namespace {
// mep_leetcode_html_to_text's own gsub('<[bB][rR]%s*/?>', '\n') port:
// <br>/<br/>/<br />/<BR> etc, replaced with a newline.
/**
 * @brief Replaces every <br>, <br/>, <br />, or <BR> style tag with a newline.
 * @param s Input HTML text.
 * @return The text with <br> tag variants replaced by '\n'.
 */
std::string StripBrTags(const std::string &s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<' && i + 2 < s.size() && (s[i + 1] == 'b' || s[i + 1] == 'B') &&
            (s[i + 2] == 'r' || s[i + 2] == 'R')) {
            size_t j = i + 3;
            while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) j++;
            if (j < s.size() && s[j] == '/') j++;
            if (j < s.size() && s[j] == '>') {
                out += '\n';
                i = j + 1;
                continue;
            }
        }
        out += s[i];
        i++;
    }
    return out;
}

// gsub('</%a+>', '\n') port: any closing tag replaced with a newline.
/**
 * @brief Replaces every HTML closing tag (e.g. </p>) with a newline.
 * @param s Input HTML text.
 * @return The text with closing tags replaced by '\n'.
 */
std::string StripClosingTags(const std::string &s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<' && i + 1 < s.size() && s[i + 1] == '/') {
            size_t j = i + 2;
            size_t letters_start = j;
            while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j]))) j++;
            if (j > letters_start && j < s.size() && s[j] == '>') {
                out += '\n';
                i = j + 1;
                continue;
            }
        }
        out += s[i];
        i++;
    }
    return out;
}

// gsub('<[^>]*>', '') port: every remaining tag stripped entirely.
/**
 * @brief Strips every remaining HTML tag from the text entirely.
 * @param s Input HTML text.
 * @return The text with all tags removed.
 */
std::string StripAnyTags(const std::string &s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            size_t j = i + 1;
            while (j < s.size() && s[j] != '>') j++;
            if (j < s.size() && s[j] == '>') {
                i = j + 1;
                continue;
            }
        }
        out += s[i];
        i++;
    }
    return out;
}

/**
 * @brief Replaces every non-overlapping literal occurrence of a substring with another string.
 * @param s Input string (taken by value so it can be mutated and returned).
 * @param from Substring to search for.
 * @param to Replacement text.
 * @return The string with every occurrence of `from` replaced by `to`.
 */
std::string ReplaceAllLiteral(std::string s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// mep_leetcode_html_to_text's own port: a rough HTML -> plain-text pass
// over LeetCode's `content` field (kBuiltinLeetcode, main.cpp) -- not a
// real HTML parser, good enough for a read-only problem statement, not
// meant to round-trip. The three tag-stripping passes and six entity
// decodes run in the exact same sequential order the original's chained
// gsub calls did (order matters here -- e.g. &amp;lt; decoding to &lt;
// only happens *after* the &lt; pass already ran, so it's deliberately
// left as literal "&lt;" in the output, not further decoded to "<").
/**
 * @brief Converts a LeetCode problem's HTML `content` field into plain-text lines via a rough sequential tag-strip and entity-decode pass (not a real HTML parser).
 * @param html Raw HTML content to convert.
 * @return The resulting plain-text lines with leading/trailing blank lines trimmed.
 */
std::vector<std::string> LeetcodeHtmlToText(const std::string &html) {
    std::string text = StripBrTags(html);
    text = StripClosingTags(text);
    text = StripAnyTags(text);
    text = ReplaceAllLiteral(text, "&lt;", "<");
    text = ReplaceAllLiteral(text, "&gt;", ">");
    text = ReplaceAllLiteral(text, "&amp;", "&");
    text = ReplaceAllLiteral(text, "&nbsp;", " ");
    text = ReplaceAllLiteral(text, "&quot;", "\"");
    text = ReplaceAllLiteral(text, "&#39;", "'");

    std::vector<std::string> out;
    std::string with_nl = text + "\n";
    size_t start = 0;
    for (size_t p = 0; p < with_nl.size(); p++) {
        if (with_nl[p] != '\n') continue;
        std::string line = with_nl.substr(start, p - start);
        start = p + 1;
        size_t a = 0, b = line.size();
        while (a < b && std::isspace(static_cast<unsigned char>(line[a]))) a++;
        while (b > a && std::isspace(static_cast<unsigned char>(line[b - 1]))) b--;
        std::string trimmed = line.substr(a, b - a);
        bool prev_nonempty = !out.empty() && !out.back().empty();
        if (!trimmed.empty() || prev_nonempty) out.push_back(trimmed);
    }
    while (!out.empty() && out.front().empty()) out.erase(out.begin());
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}
}  // namespace

// mep.leetcode_html_to_text(html) -> array of plain-text lines: see
// LeetcodeHtmlToText above.
/**
 * @brief Implements mep.leetcode_html_to_text(html): converts LeetCode problem HTML into an array of plain-text lines.
 * @param L Lua state; arg 1 is the HTML string (defaults to "" if omitted).
 * @return Number of values pushed (1: the array of plain-text lines).
 */
int l_leetcode_html_to_text(lua_State *L) {
    const char *html = luaL_optstring(L, 1, "");
    std::vector<std::string> lines = LeetcodeHtmlToText(html);
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    for (size_t i = 0; i < lines.size(); i++) {
        lua_pushlstring(L, lines[i].data(), lines[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

namespace {
// Reads the current value of mep.org_todo_keywords (a Lua-configurable
// global array, default {'TODO','DOING','DONE'} set by kBuiltinOrg) --
// mep_org_parse_headline's own implicit dependency, read fresh here
// rather than passed as a parameter since every one of its ~80 call
// sites calls it with just a line, matching the original Lua closure's
// own signature exactly.
/**
 * @brief Reads the current value of the Lua global mep.org_todo_keywords, the configurable list of org TODO state words.
 * @param L Lua state to read the global from.
 * @return The configured TODO keywords, or an empty vector if mep/mep.org_todo_keywords is missing or not a table.
 */
std::vector<std::string> ReadOrgTodoKeywords(lua_State *L) {
    std::vector<std::string> kws;
    lua_getglobal(L, "mep");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "org_todo_keywords");
        if (lua_istable(L, -1)) {
            lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_isstring(L, -1)) kws.emplace_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return kws;
}

/**
 * @brief Pushes an OrgHeadlineParse result onto the Lua stack as a {level=,todo=,priority=,title=,tags=} table, or nil if it isn't a headline.
 * @param L Lua state to push onto.
 * @param h Parsed headline result to convert.
 */
void PushOrgHeadlineParse(lua_State *L, const OrgHeadlineParse &h) {
    if (!h.is_headline) {
        lua_pushnil(L);
        return;
    }
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, h.level);
    lua_setfield(L, -2, "level");
    if (h.has_todo) {
        lua_pushlstring(L, h.todo.data(), h.todo.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "todo");
    if (h.has_priority) {
        lua_pushlstring(L, h.priority.data(), h.priority.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "priority");
    lua_pushlstring(L, h.title.data(), h.title.size());
    lua_setfield(L, -2, "title");
    if (h.has_tags) {
        lua_pushlstring(L, h.tags.data(), h.tags.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "tags");
}
}  // namespace

// mep_org_parse_headline(line) -> {level=,todo=,priority=,title=,tags=}
// or nil: see ParseOrgHeadline (editor.h/.cpp). Registered as a *bare
// global* (lua_register, not the mep.* table) -- see editor.h's own
// comment on OrgHeadlineParse for why.
/**
 * @brief Implements the bare global mep_org_parse_headline(line): parses an org headline into its components.
 * @param L Lua state; arg 1 is the line text to parse.
 * @return Number of values pushed (1: a {level=,todo=,priority=,title=,tags=} table, or nil if the line isn't a headline).
 */
int l_org_parse_headline_global(lua_State *L) {
    const char *line = luaL_checkstring(L, 1);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    PushOrgHeadlineParse(L, ParseOrgHeadline(line, kws));
    return 1;
}

// mep_org_current_headline_row([row]) -> row or nil: see
// Editor::OrgCurrentHeadlineRow. Bare global, same reason.
/**
 * @brief Implements the bare global mep_org_current_headline_row([row]): finds the nearest headline row at or above the given/current row.
 * @param L Lua state; optional arg 1 is the row to search from (defaults to 0, meaning the current cursor row).
 * @return Number of values pushed (1: the headline row number, or nil if none was found).
 */
int l_org_current_headline_row_global(lua_State *L) {
    int row = static_cast<int>(luaL_optinteger(L, 1, 0));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    int r = GetEditor(L)->OrgCurrentHeadlineRow(row, kws);
    if (r <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, r);
    }
    return 1;
}

// mep_org_subtree_end(row) -> row: see Editor::OrgSubtreeEnd. Bare
// global, same reason.
/**
 * @brief Implements the bare global mep_org_subtree_end(row): finds the last row belonging to the subtree rooted at a headline row.
 * @param L Lua state; arg 1 is the headline row.
 * @return Number of values pushed (1: the subtree's last row number).
 */
int l_org_subtree_end_global(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    lua_pushinteger(L, GetEditor(L)->OrgSubtreeEnd(row, kws));
    return 1;
}

// mep.org_clock_in()/mep.org_clock_out(): see Editor::OrgClockIn/OrgClockOut.
/**
 * @brief Implements mep.org_clock_in(): starts an org clock entry at the current headline.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_org_clock_in(lua_State *L) {
    GetEditor(L)->OrgClockIn();
    return 0;
}

/**
 * @brief Implements mep.org_clock_out(): stops the currently running org clock entry.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_org_clock_out(lua_State *L) {
    GetEditor(L)->OrgClockOut();
    return 0;
}

// mep.org_clock_table_items() -> array of "indent+title  H:MM" strings:
// see Editor::OrgClockTableItems. kBuiltinOrgClock's own
// mep.org_clock_table() is a thin wrapper feeding this into
// mep.picker_open (which needs a Lua callback ref, so stays Lua glue).
/**
 * @brief Implements mep.org_clock_table_items(): builds the list of clocked-time entries for the buffer.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of "indent+title  H:MM" strings).
 */
int l_org_clock_table_items(lua_State *L) {
    std::vector<std::string> items = GetEditor(L)->OrgClockTableItems();
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (size_t i = 0; i < items.size(); i++) {
        lua_pushlstring(L, items[i].data(), items[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.org_property_get(row, key) -> value or nil: see Editor::OrgPropertyGet.
// row may be nil/0 ("use the nearest headline at/above the cursor").
/**
 * @brief Implements mep.org_property_get(row, key): reads an org property drawer value from a headline.
 * @param L Lua state; arg 1 is the headline row (0/nil for the nearest headline at/above the cursor), arg 2 is the property key.
 * @return Number of values pushed (1: the property value, or nil if not set).
 */
int l_org_property_get(lua_State *L) {
    int row = static_cast<int>(luaL_optinteger(L, 1, 0));
    const char *key = luaL_checkstring(L, 2);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    std::pair<bool, std::string> result = GetEditor(L)->OrgPropertyGet(row, key, kws);
    if (!result.first) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, result.second.data(), result.second.size());
    }
    return 1;
}

// mep.org_property_set(row, key, value): see Editor::OrgPropertySet.
/**
 * @brief Implements mep.org_property_set(row, key, value): writes an org property drawer value on a headline.
 * @param L Lua state; arg 1 is the headline row (0/nil for the nearest headline), arg 2 is the property key, arg 3 is the value to set.
 * @return Number of values pushed (0).
 */
int l_org_property_set(lua_State *L) {
    int row = static_cast<int>(luaL_optinteger(L, 1, 0));
    const char *key = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgPropertySet(row, key, value, kws);
    return 0;
}

// mep.org_property_remove(row, key): see Editor::OrgPropertyRemove.
/**
 * @brief Implements mep.org_property_remove(row, key): removes a property from a headline's property drawer.
 * @param L Lua state; arg 1 is the headline row (0/nil for the nearest headline), arg 2 is the property key to remove.
 * @return Number of values pushed (0).
 */
int l_org_property_remove(lua_State *L) {
    int row = static_cast<int>(luaL_optinteger(L, 1, 0));
    const char *key = luaL_checkstring(L, 2);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgPropertyRemove(row, key, kws);
    return 0;
}

// mep.org_drill_grade(row, quality): see Editor::OrgDrillGrade.
/**
 * @brief Implements mep.org_drill_grade(row, quality): records a spaced-repetition drill grade against a headline.
 * @param L Lua state; arg 1 is the headline row, arg 2 is the recall quality score.
 * @return Number of values pushed (0).
 */
int l_org_drill_grade(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1));
    int quality = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgDrillGrade(row, quality, kws);
    return 0;
}

// mep.org_agenda_expand_glob(pattern) -> array of paths: see
// Editor::OrgAgendaExpandGlob.
/**
 * @brief Implements mep.org_agenda_expand_glob(pattern): expands a glob pattern into matching agenda file paths.
 * @param L Lua state; arg 1 is the glob pattern.
 * @return Number of values pushed (1: the array of matching file paths).
 */
int l_org_agenda_expand_glob(lua_State *L) {
    const char *pattern = luaL_checkstring(L, 1);
    std::vector<std::string> results = GetEditor(L)->OrgAgendaExpandGlob(pattern);
    lua_createtable(L, static_cast<int>(results.size()), 0);
    for (size_t i = 0; i < results.size(); i++) {
        lua_pushlstring(L, results[i].data(), results[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.org_agenda_scan_lines(lines, path) -> array of
// {file=,line=,todo=,title=,tags=,priority=,scheduled=,deadline=}: see
// Editor::OrgAgendaScanLines.
/**
 * @brief Implements mep.org_agenda_scan_lines(lines, path): scans a set of lines for org agenda entries (headlines with TODO/scheduling info).
 * @param L Lua state; arg 1 is a Lua array of line strings, arg 2 is the source file path to attach to each entry.
 * @return Number of values pushed (1: the array of {file=,line=,todo=,title=,tags=,priority=,scheduled=,deadline=} tables).
 */
int l_org_agenda_scan_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *path = luaL_checkstring(L, 2);
    std::vector<std::string> lines;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        lines.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    std::vector<Editor::OrgAgendaEntry> entries = GetEditor(L)->OrgAgendaScanLines(lines, path, kws);
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (size_t i = 0; i < entries.size(); i++) {
        const Editor::OrgAgendaEntry &e = entries[i];
        lua_createtable(L, 0, 8);
        lua_pushlstring(L, e.file.data(), e.file.size());
        lua_setfield(L, -2, "file");
        lua_pushinteger(L, e.line);
        lua_setfield(L, -2, "line");
        if (e.has_todo) {
            lua_pushlstring(L, e.todo.data(), e.todo.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "todo");
        lua_pushlstring(L, e.title.data(), e.title.size());
        lua_setfield(L, -2, "title");
        if (e.has_tags) {
            lua_pushlstring(L, e.tags.data(), e.tags.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "tags");
        if (e.has_priority) {
            lua_pushlstring(L, e.priority.data(), e.priority.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "priority");
        if (e.has_scheduled) {
            lua_pushlstring(L, e.scheduled.data(), e.scheduled.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "scheduled");
        if (e.has_deadline) {
            lua_pushlstring(L, e.deadline.data(), e.deadline.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "deadline");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.org_capture_expand_template(tmpl) -> expanded string: see
// Editor::OrgExpandCaptureTemplate. (kBuiltinOrgCapture's own
// mep_org_expand_template port -- named differently to avoid colliding
// with kBuiltinOrg's unrelated mep.org_expand_template, the "<s Tab"
// easy-templates command.)
/**
 * @brief Implements mep.org_capture_expand_template(tmpl): expands %-escapes in an org capture template string.
 * @param L Lua state; arg 1 is the template string.
 * @return Number of values pushed (1: the expanded string).
 */
int l_org_expand_capture_template(lua_State *L) {
    const char *tmpl = luaL_checkstring(L, 1);
    std::string expanded = GetEditor(L)->OrgExpandCaptureTemplate(tmpl);
    lua_pushlstring(L, expanded.data(), expanded.size());
    return 1;
}

// mep.org_refile_move(target_row) -> new cursor row or nil: see
// Editor::OrgRefileMove.
/**
 * @brief Implements mep.org_refile_move(target_row): moves the current subtree under a target headline.
 * @param L Lua state; arg 1 is the row of the destination headline.
 * @return Number of values pushed (1: the new cursor row, or nil if the move failed).
 */
int l_org_refile_move(lua_State *L) {
    int target_row = static_cast<int>(luaL_checkinteger(L, 1));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    int dest = GetEditor(L)->OrgRefileMove(target_row, kws);
    if (dest <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, dest);
    }
    return 1;
}

// mep.org_latex_scan_fragments() -> {blocks = {{start_row=,end_row=,
// body=}, ...}, inlines = {{row=,col_start=,col_end=,body=}, ...}}: see
// Editor::OrgLatexScanFragments.
/**
 * @brief Implements mep.org_latex_scan_fragments(): scans the buffer for LaTeX fragments (block and inline).
 * @param L Lua state.
 * @return Number of values pushed (1: a {blocks={{start_row=,end_row=,body=},...}, inlines={{row=,col_start=,col_end=,body=},...}} table).
 */
int l_org_latex_scan_fragments(lua_State *L) {
    Editor::OrgLatexScanResult result = GetEditor(L)->OrgLatexScanFragments();
    lua_createtable(L, 0, 2);
    lua_createtable(L, static_cast<int>(result.blocks.size()), 0);
    for (size_t i = 0; i < result.blocks.size(); i++) {
        const Editor::OrgLatexBlock &b = result.blocks[i];
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, b.start_row);
        lua_setfield(L, -2, "start_row");
        lua_pushinteger(L, b.end_row);
        lua_setfield(L, -2, "end_row");
        lua_pushlstring(L, b.body.data(), b.body.size());
        lua_setfield(L, -2, "body");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "blocks");
    lua_createtable(L, static_cast<int>(result.inlines.size()), 0);
    for (size_t i = 0; i < result.inlines.size(); i++) {
        const Editor::OrgLatexInlineSpan &s = result.inlines[i];
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, s.row);
        lua_setfield(L, -2, "row");
        lua_pushinteger(L, s.col_start);
        lua_setfield(L, -2, "col_start");
        lua_pushinteger(L, s.col_end);
        lua_setfield(L, -2, "col_end");
        lua_pushlstring(L, s.body.data(), s.body.size());
        lua_setfield(L, -2, "body");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "inlines");
    return 1;
}

// mep.org_bib_parse_files({text1, text2, ...}) -> array of
// {type=,key=,fields={name=value,...}}: see Editor::OrgBibParseFiles.
/**
 * @brief Implements mep.org_bib_parse_files({text1, text2, ...}): parses BibTeX-style file contents into bibliography entries.
 * @param L Lua state; arg 1 is a Lua array of file-content strings.
 * @return Number of values pushed (1: the array of {type=,key=,fields={name=value,...}} tables).
 */
int l_org_bib_parse_files(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> texts;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        texts.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    std::vector<Editor::OrgBibEntry> entries = GetEditor(L)->OrgBibParseFiles(texts);
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    for (size_t i = 0; i < entries.size(); i++) {
        const Editor::OrgBibEntry &e = entries[i];
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, e.type.data(), e.type.size());
        lua_setfield(L, -2, "type");
        lua_pushlstring(L, e.key.data(), e.key.size());
        lua_setfield(L, -2, "key");
        lua_createtable(L, 0, static_cast<int>(e.fields.size()));
        for (const auto &kv : e.fields) {
            lua_pushlstring(L, kv.second.data(), kv.second.size());
            lua_setfield(L, -2, kv.first.c_str());
        }
        lua_setfield(L, -2, "fields");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep_org_bib_cite_at_cursor() -> array of citation keys under the
// cursor, or an empty table if none: see Editor::OrgBibCiteAtCursor.
// Bare global, same reason as the Org-0/mep_lsp_*/mep_org_resolve_path
// primitives.
/**
 * @brief Implements the bare global mep_org_bib_cite_at_cursor(): finds citation keys at the cursor.
 * @param L Lua state.
 * @return Number of values pushed (1: the array of citation keys under the cursor, empty if none).
 */
int l_org_bib_cite_at_cursor_global(lua_State *L) {
    std::vector<std::string> keys = GetEditor(L)->OrgBibCiteAtCursor();
    lua_createtable(L, static_cast<int>(keys.size()), 0);
    for (size_t i = 0; i < keys.size(); i++) {
        lua_pushlstring(L, keys[i].data(), keys[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

namespace {
// Helpers shared by the OrgRoam bindings below: read a Lua array of
// strings (arg index `idx`) into a std::vector<std::string>, or push
// one back.
/**
 * @brief Reads a Lua array of strings at the given stack index into a std::vector<std::string>.
 * @param L Lua state to read from.
 * @param idx Stack index of the Lua array table.
 * @return The array's elements as strings (missing/non-string elements read as "").
 */
std::vector<std::string> ReadStringArray(lua_State *L, int idx) {
    std::vector<std::string> out;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        out.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    return out;
}

/**
 * @brief Pushes a std::vector<std::string> onto the Lua stack as a 1-indexed array table of strings.
 * @param L Lua state to push onto.
 * @param items Strings to push.
 */
void PushStringArray(lua_State *L, const std::vector<std::string> &items) {
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (size_t i = 0; i < items.size(); i++) {
        lua_pushlstring(L, items[i].data(), items[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}
}  // namespace

// mep.org_roam_files_in({dir1, dir2, ...}) -> array of paths: see
// Editor::OrgRoamFilesIn.
/**
 * @brief Implements mep.org_roam_files_in({dir1, dir2, ...}): lists org-roam note files under the given directories.
 * @param L Lua state; arg 1 is a Lua array of directory paths.
 * @return Number of values pushed (1: the array of file paths).
 */
int l_org_roam_files_in(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> dirs = ReadStringArray(L, 1);
    PushStringArray(L, GetEditor(L)->OrgRoamFilesIn(dirs));
    return 1;
}

// mep.org_roam_title_of({line1, line2, ...}) -> title string or nil:
// see Editor::OrgRoamTitleOf.
/**
 * @brief Implements mep.org_roam_title_of({line1, line2, ...}): extracts an org-roam note's title from its file lines.
 * @param L Lua state; arg 1 is a Lua array of the file's lines.
 * @return Number of values pushed (1: the title string, or nil if none was found).
 */
int l_org_roam_title_of(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> lines = ReadStringArray(L, 1);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    std::pair<bool, std::string> result = GetEditor(L)->OrgRoamTitleOf(lines, kws);
    if (!result.first) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, result.second.data(), result.second.size());
    }
    return 1;
}

// mep.org_roam_ensure_id() -> id string: see Editor::OrgRoamEnsureId.
/**
 * @brief Implements mep.org_roam_ensure_id(): ensures the current org-roam note has an :ID:, generating one if missing.
 * @param L Lua state.
 * @return Number of values pushed (1: the note's ID string).
 */
int l_org_roam_ensure_id(lua_State *L) {
    std::string id = GetEditor(L)->OrgRoamEnsureId();
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

// mep.org_roam_find_backlink_lines({line1, ...}, target_id) -> array of
// 1-indexed line numbers: see Editor::OrgRoamFindBacklinkLines.
/**
 * @brief Implements mep.org_roam_find_backlink_lines({line1, ...}, target_id): finds lines linking to a given org-roam ID.
 * @param L Lua state; arg 1 is a Lua array of the file's lines, arg 2 is the target note's ID.
 * @return Number of values pushed (1: the array of 1-indexed line numbers containing a link to target_id).
 */
int l_org_roam_find_backlink_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> lines = ReadStringArray(L, 1);
    const char *target_id = luaL_checkstring(L, 2);
    std::vector<int> result = GetEditor(L)->OrgRoamFindBacklinkLines(lines, target_id);
    lua_createtable(L, static_cast<int>(result.size()), 0);
    for (size_t i = 0; i < result.size(); i++) {
        lua_pushinteger(L, result[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.org_roam_parse_file_index({line1, ...}) -> {has_id=,id=,title=,
// links={...}} or {has_id=false} if the file has no :ID:: see
// Editor::OrgRoamParseFileIndex.
/**
 * @brief Implements mep.org_roam_parse_file_index({line1, ...}): extracts an org-roam file's ID, title, and outgoing links for index-building.
 * @param L Lua state; arg 1 is a Lua array of the file's lines.
 * @return Number of values pushed (1: a {has_id=,id=,title=,links={...}} table, or {has_id=false} if the file has no :ID:).
 */
int l_org_roam_parse_file_index(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> lines = ReadStringArray(L, 1);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    Editor::OrgRoamFileIndexEntry entry = GetEditor(L)->OrgRoamParseFileIndex(lines, kws);
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, entry.has_id);
    lua_setfield(L, -2, "has_id");
    if (entry.has_id) {
        lua_pushlstring(L, entry.id.data(), entry.id.size());
        lua_setfield(L, -2, "id");
        if (entry.has_title) {
            lua_pushlstring(L, entry.title.data(), entry.title.size());
        } else {
            lua_pushnil(L);
        }
        lua_setfield(L, -2, "title");
        PushStringArray(L, entry.links);
        lua_setfield(L, -2, "links");
    }
    return 1;
}

// mep.org_roam_slugify(title) -> slug string: see OrgRoamSlugify.
/**
 * @brief Implements mep.org_roam_slugify(title): converts a note title into a filesystem-safe slug.
 * @param L Lua state; arg 1 is the title string.
 * @return Number of values pushed (1: the slugified string).
 */
int l_org_roam_slugify(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    std::string slug = OrgRoamSlugify(title);
    lua_pushlstring(L, slug.data(), slug.size());
    return 1;
}

// mep.org_table_align(): see Editor::OrgTableAlign.
/**
 * @brief Implements mep.org_table_align(): reformats the org table under the cursor to align its columns.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_org_table_align(lua_State *L) {
    GetEditor(L)->OrgTableAlign();
    return 0;
}

// mep.org_link_at_cursor() -> target, desc (desc nil if none), or nil:
// see Editor::OrgLinkAtCursor.
/**
 * @brief Implements mep.org_link_at_cursor(): reads the org link under the cursor, if any.
 * @param L Lua state.
 * @return Number of values pushed (1 or 2: the link target and, if present, its description; or 1 nil if no link is under the cursor).
 */
int l_org_link_at_cursor(lua_State *L) {
    Editor::OrgLinkAtCursorResult r = GetEditor(L)->OrgLinkAtCursor();
    if (!r.found) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, r.target.data(), r.target.size());
    if (r.has_desc) {
        lua_pushlstring(L, r.desc.data(), r.desc.size());
    } else {
        lua_pushnil(L);
    }
    return 2;
}

// mep.org_timestamp_insert(active): see Editor::OrgTimestampInsert.
/**
 * @brief Implements mep.org_timestamp_insert(active): inserts an org timestamp at the cursor.
 * @param L Lua state; arg 1 is a boolean, true for an active (<...>) timestamp, false for inactive ([...]).
 * @return Number of values pushed (0).
 */
int l_org_timestamp_insert(lua_State *L) {
    bool active = lua_toboolean(L, 1);
    GetEditor(L)->OrgTimestampInsert(active);
    return 0;
}

// mep.org_timestamp_shift(delta_days): see Editor::OrgTimestampShift.
/**
 * @brief Implements mep.org_timestamp_shift(delta_days): shifts the org timestamp under/near the cursor by a number of days.
 * @param L Lua state; arg 1 is the number of days to shift by (may be negative).
 * @return Number of values pushed (0).
 */
int l_org_timestamp_shift(lua_State *L) {
    int delta_days = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->OrgTimestampShift(delta_days);
    return 0;
}

// mep.org_timestamp_at(line, col) -> {col_start=,col_end=,body=,
// active=} or nil: see Editor::OrgTimestampAt. Exposed so the still-Lua
// mep.org_timestamp_set_repeater can keep calling it.
/**
 * @brief Implements mep.org_timestamp_at(line, col): finds an org timestamp overlapping a given column in a line.
 * @param L Lua state; arg 1 is the line text, arg 2 is the column to check.
 * @return Number of values pushed (1: a {col_start=,col_end=,body=,active=} table, or nil if no timestamp is at that column).
 */
int l_org_timestamp_at(lua_State *L) {
    const char *line = luaL_checkstring(L, 1);
    int col = static_cast<int>(luaL_checkinteger(L, 2));
    Editor::OrgTimestampMatch m = GetEditor(L)->OrgTimestampAt(line, col);
    if (!m.found) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, m.col_start);
    lua_setfield(L, -2, "col_start");
    lua_pushinteger(L, m.col_end);
    lua_setfield(L, -2, "col_end");
    lua_pushlstring(L, m.body.data(), m.body.size());
    lua_setfield(L, -2, "body");
    lua_pushboolean(L, m.active);
    lua_setfield(L, -2, "active");
    return 1;
}

// mep.org_footnote_jump(): see Editor::OrgFootnoteJump.
/**
 * @brief Implements mep.org_footnote_jump(): jumps between an org footnote reference and its definition.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_org_footnote_jump(lua_State *L) {
    GetEditor(L)->OrgFootnoteJump();
    return 0;
}

// mep.org_set_planning(kind): see Editor::OrgSetPlanning.
/**
 * @brief Implements mep.org_set_planning(kind): sets/updates a SCHEDULED or DEADLINE planning line on the current headline.
 * @param L Lua state; arg 1 is the planning kind ("SCHEDULED" or "DEADLINE").
 * @return Number of values pushed (0).
 */
int l_org_set_planning(lua_State *L) {
    const char *kind = luaL_checkstring(L, 1);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgSetPlanning(kind, kws);
    return 0;
}

// mep.org_export_heading(format, level, title) -> heading string: see
// OrgExportHeading.
/**
 * @brief Implements mep.org_export_heading(format, level, title): formats a heading string for the given export format.
 * @param L Lua state; arg 1 is the export format name, arg 2 is the heading level, arg 3 is the heading title text.
 * @return Number of values pushed (1: the formatted heading string).
 */
int l_org_export_heading(lua_State *L) {
    const char *format = luaL_checkstring(L, 1);
    int level = static_cast<int>(luaL_checkinteger(L, 2));
    const char *title = luaL_checkstring(L, 3);
    std::string result = OrgExportHeading(format, level, title);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep.org_html_escape(s) -> escaped string: see OrgHtmlEscape.
/**
 * @brief Implements mep.org_html_escape(s): HTML-escapes a string for org HTML export.
 * @param L Lua state; arg 1 is the string to escape.
 * @return Number of values pushed (1: the escaped string).
 */
int l_org_html_escape(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    std::string result = OrgHtmlEscape(s);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep.org_subtree_end_lines({line1, ...}, row) -> row: see
// OrgSubtreeEndLines.
/**
 * @brief Implements mep.org_subtree_end_lines({line1, ...}, row): finds the last row of the subtree rooted at row, given a plain line array instead of a buffer.
 * @param L Lua state; arg 1 is a Lua array of the buffer's lines, arg 2 is the headline row.
 * @return Number of values pushed (1: the subtree's last row number).
 */
int l_org_subtree_end_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> lines = ReadStringArray(L, 1);
    int row = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    lua_pushinteger(L, OrgSubtreeEndLines(lines, row, kws));
    return 1;
}

// mep.org_collect_macros({line1, ...}) -> {name = body, ...}: see
// OrgCollectMacros.
/**
 * @brief Implements mep.org_collect_macros({line1, ...}): collects #+MACRO: definitions from a set of lines.
 * @param L Lua state; arg 1 is a Lua array of lines to scan.
 * @return Number of values pushed (1: a {name = body, ...} table of macro definitions).
 */
int l_org_collect_macros(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> lines = ReadStringArray(L, 1);
    std::map<std::string, std::string> macros = OrgCollectMacros(lines);
    lua_createtable(L, 0, static_cast<int>(macros.size()));
    for (const auto &kv : macros) {
        lua_pushlstring(L, kv.second.data(), kv.second.size());
        lua_setfield(L, -2, kv.first.c_str());
    }
    return 1;
}

// mep.org_expand_macro_line(line, macros) -> expanded string: see
// OrgExpandMacroLine.
/**
 * @brief Implements mep.org_expand_macro_line(line, macros): expands {{{macro(...)}}} references in a line using a table of macro definitions.
 * @param L Lua state; arg 1 is the line text, arg 2 is a Lua table mapping macro names to their bodies.
 * @return Number of values pushed (1: the expanded line).
 */
int l_org_expand_macro_line(lua_State *L) {
    const char *line = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::map<std::string, std::string> macros;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
            macros[lua_tostring(L, -2)] = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
    std::string result = OrgExpandMacroLine(line, macros);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep_org_babel_should_wrap_main(lang_key, args_str) -> bool: see
// OrgBabelShouldWrapMain. Bare global (not a mep.* field) -- kBuiltinOrgBabel
// and kBuiltinOrgPolyglot (two separate DoString chunks) both call this
// by its bare-global name, same reason as mep_org_src_block_at.
/**
 * @brief Implements the bare global mep_org_babel_should_wrap_main(lang_key, args_str): decides whether a babel source block needs a "main" wrapper for the given language and header args.
 * @param L Lua state; arg 1 is the language key, arg 2 is the block's header args string.
 * @return Number of values pushed (1: boolean, whether wrapping is needed).
 */
int l_org_babel_should_wrap_main_global(lua_State *L) {
    const char *lang_key = luaL_checkstring(L, 1);
    const char *args_str = luaL_checkstring(L, 2);
    lua_pushboolean(L, OrgBabelShouldWrapMain(lang_key, args_str));
    return 1;
}

// mep.org_babel_format_literal(raw) -> formatted literal string: see
// OrgBabelFormatLiteral. `raw` is coerced to a string on the Lua side
// first (luaL_checkstring already stringifies a number arg the same
// way `tostring(raw)` did).
/**
 * @brief Implements mep.org_babel_format_literal(raw): formats a raw babel result value as a literal-example string.
 * @param L Lua state; arg 1 is the raw value (coerced to a string by luaL_checkstring).
 * @return Number of values pushed (1: the formatted literal string).
 */
int l_org_babel_format_literal(lua_State *L) {
    const char *raw = luaL_checkstring(L, 1);
    std::string result = OrgBabelFormatLiteral(raw);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep.org_parse_vars(args_str) -> {name = value, ...}: see OrgParseVars.
/**
 * @brief Implements mep.org_parse_vars(args_str): parses a babel header args string's :var entries.
 * @param L Lua state; arg 1 is the header args string.
 * @return Number of values pushed (1: a {name = value, ...} table of parsed variables).
 */
int l_org_parse_vars(lua_State *L) {
    const char *args_str = luaL_checkstring(L, 1);
    std::map<std::string, std::string> vars = OrgParseVars(args_str);
    lua_createtable(L, 0, static_cast<int>(vars.size()));
    for (const auto &kv : vars) {
        lua_pushlstring(L, kv.second.data(), kv.second.size());
        lua_setfield(L, -2, kv.first.c_str());
    }
    return 1;
}

// mep.org_parse_results(args_str) -> {[mode] = true, ...}: see
// OrgParseResults.
/**
 * @brief Implements mep.org_parse_results(args_str): parses a babel header args string's :results modes.
 * @param L Lua state; arg 1 is the header args string.
 * @return Number of values pushed (1: a {[mode] = true, ...} set-table of the requested results modes).
 */
int l_org_parse_results(lua_State *L) {
    const char *args_str = luaL_checkstring(L, 1);
    std::set<std::string> modes = OrgParseResults(args_str);
    lua_createtable(L, 0, static_cast<int>(modes.size()));
    for (const std::string &m : modes) {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, m.c_str());
    }
    return 1;
}

namespace {
// Shared by l_org_src_block_at_global below: pushes an OrgSrcBlock as
// the same table shape the original Lua mep_org_src_block_at built.
/**
 * @brief Pushes an OrgSrcBlock onto the Lua stack as a {start_row=,end_row=,lang=,vars=,tangle=,cache=,file=,results_modes=,args_str=,body=} table.
 * @param L Lua state to push onto.
 * @param blk Source block to convert.
 */
void PushOrgSrcBlock(lua_State *L, const OrgSrcBlock &blk) {
    lua_createtable(L, 0, 10);
    lua_pushinteger(L, blk.start_row);
    lua_setfield(L, -2, "start_row");
    lua_pushinteger(L, blk.end_row);
    lua_setfield(L, -2, "end_row");
    if (blk.has_lang) {
        lua_pushlstring(L, blk.lang.data(), blk.lang.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "lang");
    lua_createtable(L, 0, static_cast<int>(blk.vars.size()));
    for (const auto &kv : blk.vars) {
        lua_pushlstring(L, kv.second.data(), kv.second.size());
        lua_setfield(L, -2, kv.first.c_str());
    }
    lua_setfield(L, -2, "vars");
    if (blk.has_tangle) {
        lua_pushlstring(L, blk.tangle.data(), blk.tangle.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "tangle");
    if (blk.has_cache) {
        lua_pushlstring(L, blk.cache.data(), blk.cache.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "cache");
    if (blk.has_file) {
        lua_pushlstring(L, blk.file.data(), blk.file.size());
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, -2, "file");
    lua_createtable(L, 0, static_cast<int>(blk.results_modes.size()));
    for (const std::string &m : blk.results_modes) {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, m.c_str());
    }
    lua_setfield(L, -2, "results_modes");
    lua_pushlstring(L, blk.args_str.data(), blk.args_str.size());
    lua_setfield(L, -2, "args_str");
    lua_pushlstring(L, blk.body.data(), blk.body.size());
    lua_setfield(L, -2, "body");
}
}  // namespace

// mep_org_src_block_at(row) -> {start_row=,end_row=,lang=,vars=,
// tangle=,cache=,file=,results_modes=,args_str=,body=} or nil: see
// Editor::OrgSrcBlockAt. Bare global, same reason as the Org-0/
// mep_lsp_*/mep_org_resolve_path/mep_org_bib_cite_at_cursor primitives
// -- kBuiltinOrgPolyglot shares this exact name.
/**
 * @brief Implements the bare global mep_org_src_block_at(row): finds the babel source block containing a given row.
 * @param L Lua state; arg 1 is the row to check.
 * @return Number of values pushed (1: the source block table, or nil if row isn't inside one).
 */
int l_org_src_block_at_global(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1));
    OrgSrcBlock blk = GetEditor(L)->OrgSrcBlockAt(row);
    if (!blk.found) {
        lua_pushnil(L);
        return 1;
    }
    PushOrgSrcBlock(L, blk);
    return 1;
}

// mep_ai_json_encode(v) -> compact JSON string: see LuaToJson/Json::dump.
// Bare global (not a mep.* field, matching the original's own bare-
// global name) -- kBuiltinAi and kBuiltinLeetcode (two separate
// DoString chunks) both call this by its bare-global name.
/**
 * @brief Implements the bare global mep_ai_json_encode(v): encodes a Lua value as a compact JSON string.
 * @param L Lua state; arg 1 is the Lua value to encode.
 * @return Number of values pushed (1: the JSON string).
 */
int l_ai_json_encode_global(lua_State *L) {
    Json v = LuaToJson(L, 1);
    std::string out = v.dump();
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// mep_ai_json_decode(s) -> decoded Lua value, or nil on malformed input:
// see Json::Parse/PushJsonNilNull. Bare global, same reason as
// mep_ai_json_encode above. Uses PushJsonNilNull (not PushJson) so JSON
// null decodes to plain Lua nil, matching the original hand-rolled
// decoder's own behavior exactly (see PushJsonNilNull's own comment).
/**
 * @brief Implements the bare global mep_ai_json_decode(s): decodes a JSON string into a Lua value, with JSON null becoming Lua nil.
 * @param L Lua state; arg 1 is the JSON string to decode.
 * @return Number of values pushed (1: the decoded Lua value, or nil if s is malformed JSON).
 */
int l_ai_json_decode_global(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    Json v;
    if (!Json::Parse(s, &v)) {
        lua_pushnil(L);
        return 1;
    }
    PushJsonNilNull(L, v);
    return 1;
}

// mep_lsp_word_at_cursor() -> word string or nil: see Editor::LspWordAtCursor.
/**
 * @brief Implements the bare global mep_lsp_word_at_cursor(): reads the identifier word under the cursor.
 * @param L Lua state.
 * @return Number of values pushed (1: the word string, or nil if the cursor isn't over a word).
 */
int l_lsp_word_at_cursor(lua_State *L) {
    std::pair<bool, std::string> result = GetEditor(L)->LspWordAtCursor();
    if (!result.first) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, result.second.data(), result.second.size());
    }
    return 1;
}

// mep_lsp_apply_text_edit(e): e = {range={start={line=,character=},
// ['end']={line=,character=}}, newText=}: see Editor::LspApplyTextEdit.
/**
 * @brief Implements the bare global mep_lsp_apply_text_edit(e): applies a single LSP TextEdit to the current buffer.
 * @param L Lua state; arg 1 is an LSP-shaped {range={start={line=,character=},['end']={line=,character=}}, newText=} table.
 * @return Number of values pushed (0).
 */
int l_lsp_apply_text_edit(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "range");
    lua_getfield(L, -1, "start");
    lua_getfield(L, -1, "line");
    int start_line = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "character");
    int start_char = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 2);  // pop start table, character
    lua_getfield(L, -1, "end");
    lua_getfield(L, -1, "line");
    int end_line = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "character");
    int end_char = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 2);  // pop end table, character
    lua_pop(L, 1);  // pop range table
    lua_getfield(L, 1, "newText");
    const char *new_text = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    GetEditor(L)->LspApplyTextEdit(start_line, start_char, end_line, end_char, new_text);
    return 0;
}

namespace {
// Reads one LSP TextEdit-shaped table (already on top of the stack) into
// an Editor::LspTextEdit. Shared by l_lsp_apply_edits_current_buffer
// below.
/**
 * @brief Reads an LSP TextEdit-shaped table at the given stack index into an Editor::LspTextEdit.
 * @param L Lua state to read from.
 * @param idx Stack index of the {range={start={line=,character=},['end']={line=,character=}}, newText=} table.
 * @return The parsed text edit.
 */
Editor::LspTextEdit ReadLspTextEdit(lua_State *L, int idx) {
    Editor::LspTextEdit e;
    lua_getfield(L, idx, "range");
    lua_getfield(L, -1, "start");
    lua_getfield(L, -1, "line");
    e.start_line = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "character");
    e.start_char = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 2);
    lua_getfield(L, -1, "end");
    lua_getfield(L, -1, "line");
    e.end_line = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "character");
    e.end_char = static_cast<int>(luaL_checkinteger(L, -1));
    lua_pop(L, 2);
    lua_pop(L, 1);
    lua_getfield(L, idx, "newText");
    e.new_text = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return e;
}
}  // namespace

// mep_lsp_apply_edits_current_buffer({e1, e2, ...}): see
// Editor::LspApplyEditsCurrentBuffer.
/**
 * @brief Implements the bare global mep_lsp_apply_edits_current_buffer({e1, e2, ...}): applies a batch of LSP TextEdits to the current buffer.
 * @param L Lua state; arg 1 is a Lua array of LSP TextEdit-shaped tables.
 * @return Number of values pushed (0).
 */
int l_lsp_apply_edits_current_buffer(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<Editor::LspTextEdit> edits;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        edits.push_back(ReadLspTextEdit(L, -1));
        lua_pop(L, 1);
    }
    GetEditor(L)->LspApplyEditsCurrentBuffer(std::move(edits));
    return 0;
}

// mep.lsp_diag_wrap(text, width) -> array of wrapped lines: see LspDiagWrap.
/**
 * @brief Implements mep.lsp_diag_wrap(text, width): word-wraps LSP diagnostic text to a given column width.
 * @param L Lua state; arg 1 is the diagnostic text, arg 2 is the wrap width.
 * @return Number of values pushed (1: the array of wrapped lines).
 */
int l_lsp_diag_wrap(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    int width = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<std::string> lines = LspDiagWrap(text, width);
    PushStringArray(L, lines);
    return 1;
}

// mep_org_resolve_path(path) -> resolved path: see Editor::OrgResolvePath.
// Bare global, same reason as the Org-0/mep_lsp_* primitives.
/**
 * @brief Implements the bare global mep_org_resolve_path(path): resolves an org file/link path relative to the current buffer.
 * @param L Lua state; arg 1 is the path to resolve.
 * @return Number of values pushed (1: the resolved path string).
 */
int l_org_resolve_path_global(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::string resolved = GetEditor(L)->OrgResolvePath(path);
    lua_pushlstring(L, resolved.data(), resolved.size());
    return 1;
}

// mep.org_image_scan(): see Editor::OrgImageScan.
/**
 * @brief Implements mep.org_image_scan(): scans the current org buffer for image links to render inline.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_org_image_scan(lua_State *L) {
    GetEditor(L)->OrgImageScan();
    return 0;
}

// mep_lsp_filetype(fname) -> extension string or nil: see LspFiletype
// (editor.h/.cpp). Bare global, same reason as the Org-0 primitives
// above.
/**
 * @brief Implements the bare global mep_lsp_filetype(fname): maps a filename to its LSP filetype/extension identifier.
 * @param L Lua state; arg 1 is the filename.
 * @return Number of values pushed (1: the filetype string, or nil if unrecognized).
 */
int l_lsp_filetype_global(lua_State *L) {
    const char *fname = luaL_checkstring(L, 1);
    std::string ft = LspFiletype(fname);
    if (ft.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, ft.data(), ft.size());
    }
    return 1;
}

// mep_lsp_abspath(fname) -> absolute path string: see LspAbspath.
/**
 * @brief Implements the bare global mep_lsp_abspath(fname): converts a filename into an absolute path.
 * @param L Lua state; arg 1 is the filename to resolve.
 * @return Number of values pushed (1: the absolute path string).
 */
int l_lsp_abspath_global(lua_State *L) {
    const char *fname = luaL_checkstring(L, 1);
    std::string ap = LspAbspath(fname);
    lua_pushlstring(L, ap.data(), ap.size());
    return 1;
}

// mep.git_gutter_refresh_native(base): see Editor::GitGutterRefresh.
// kBuiltinGit's own mep.git_gutter_refresh (main.cpp) is a one-line
// wrapper threading mep.git_gutter_base through.
/**
 * @brief Implements mep.git_gutter_refresh_native(base): recomputes the git diff gutter markers against a base ref.
 * @param L Lua state; arg 1 is the git base ref/commit to diff against.
 * @return Number of values pushed (0).
 */
int l_git_gutter_refresh_native(lua_State *L) {
    const char *base = luaL_checkstring(L, 1);
    GetEditor(L)->GitGutterRefresh(base);
    return 0;
}

// mep.git_next_hunk_row()/mep.git_prev_hunk_row() -> 1-indexed row or
// nil: see Editor::GitNextHunkRow/GitPrevHunkRow.
/**
 * @brief Implements mep.git_next_hunk_row(): finds the row of the next git diff hunk after the cursor.
 * @param L Lua state.
 * @return Number of values pushed (1: the 1-indexed row, or nil if there is none).
 */
int l_git_next_hunk_row(lua_State *L) {
    int row = GetEditor(L)->GitNextHunkRow();
    if (row <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, row);
    }
    return 1;
}
/**
 * @brief Implements mep.git_prev_hunk_row(): finds the row of the previous git diff hunk before the cursor.
 * @param L Lua state.
 * @return Number of values pushed (1: the 1-indexed row, or nil if there is none).
 */
int l_git_prev_hunk_row(lua_State *L) {
    int row = GetEditor(L)->GitPrevHunkRow();
    if (row <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, row);
    }
    return 1;
}

// mep.git_preview_hunk_text() -> text or nil: see Editor::GitPreviewHunkText.
/**
 * @brief Implements mep.git_preview_hunk_text(): builds the preview text for the git diff hunk at the cursor.
 * @param L Lua state.
 * @return Number of values pushed (1: the preview text, or nil if the cursor isn't in a hunk).
 */
int l_git_preview_hunk_text(lua_State *L) {
    std::pair<bool, std::string> r = GetEditor(L)->GitPreviewHunkText();
    if (!r.first) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, r.second.data(), r.second.size());
    }
    return 1;
}

// mep.git_reset_hunk_native(base): see Editor::GitResetHunk.
/**
 * @brief Implements mep.git_reset_hunk_native(base): reverts the git diff hunk at the cursor back to its base-ref content.
 * @param L Lua state; arg 1 is the git base ref/commit to reset the hunk against.
 * @return Number of values pushed (0).
 */
int l_git_reset_hunk_native(lua_State *L) {
    const char *base = luaL_checkstring(L, 1);
    GetEditor(L)->GitResetHunk(base);
    return 0;
}

// mep.git_stage_hunk(): see Editor::GitStageHunk. Bound directly, no wrapper.
/**
 * @brief Implements mep.git_stage_hunk(): stages the git diff hunk at the cursor.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_git_stage_hunk(lua_State *L) {
    GetEditor(L)->GitStageHunk();
    return 0;
}

/**
 * @brief Implements mep.lsp_symbols_flatten(syms): flattens a nested LSP DocumentSymbol/SymbolInformation tree into a flat list of display rows.
 * @param L Lua state; arg 1 is the Lua table of LSP symbols (as decoded from the server's response).
 * @return Number of values pushed (1: the array of {row=,text=} tables, one per flattened symbol).
 */
int l_lsp_symbols_flatten(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    Json syms = LuaToJson(L, 1);
    std::vector<LspSymbolRow> rows;
    FlattenLspSymbols(syms, 0, &rows);
    lua_createtable(L, static_cast<int>(rows.size()), 0);
    for (size_t i = 0; i < rows.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, rows[i].row + 1);
        lua_setfield(L, -2, "row");
        lua_pushlstring(L, rows[i].text.data(), rows[i].text.size());
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// Persisted babel result cache (NVIM_PARITY_PLAN.md Phase 34 follow-up:
// on-disk `:cache yes` persistence -- previously in-memory only, lost on
// restart). Stored at $XDG_DATA_HOME/mep/babel_cache.json as
// {"entries": {cache_key: [line, ...]}}, same MepDataDir()/ReadJsonFile/
// WriteJsonFile convention as the persisted project list
// (Editor::ListProjects/AddProject above) and the same generic
// PushJson/LuaToJson marshaling LSP already uses -- the cache is just an
// arbitrary Lua table (string key -> array of strings) from this side of
// the boundary, so no bespoke per-field (de)serialization is needed.
// Native-only, like the rest of persist.h; the wasm build's babel cache
// stays in-memory-only for the session, unchanged from before this
// change.
#if !defined(__EMSCRIPTEN__)
namespace {
/**
 * @brief Returns the on-disk path of the persisted babel result cache file.
 * @return Path to babel_cache.json under the mep data directory.
 */
std::string BabelCachePath() { return MepDataDir() + "/babel_cache.json"; }
}  // namespace
#endif

/**
 * @brief Implements mep.babel_cache_load(): loads the persisted babel result cache from disk.
 * @param L Lua state.
 * @return Number of values pushed (1: the cache's "entries" table, or an empty table if there is no persisted cache -- native build only; the wasm build has no on-disk cache and always returns empty).
 */
int l_babel_cache_load(lua_State *L) {
#if !defined(__EMSCRIPTEN__)
    Json doc;
    if (ReadJsonFile(BabelCachePath(), &doc) && doc.is_object()) {
        const Json &entries = doc.get("entries");
        if (entries.is_object()) {
            PushJson(L, entries);
            return 1;
        }
    }
#endif
    lua_newtable(L);
    return 1;
}

/**
 * @brief Implements mep.babel_cache_save(entries): persists the babel result cache to disk (a no-op on the wasm build).
 * @param L Lua state; arg 1 is the cache's {cache_key: [line, ...]} entries table.
 * @return Number of values pushed (0).
 */
int l_babel_cache_save(lua_State *L) {
#if !defined(__EMSCRIPTEN__)
    luaL_checktype(L, 1, LUA_TTABLE);
    Json doc = Json::Object();
    doc["entries"] = LuaToJson(L, 1);
    WriteJsonFile(BabelCachePath(), doc);
#endif
    return 0;
}

// Persisted pane-header Run button config (RUNBUTTON_PLAN): per-project
// overrides -- compiler/interpreter and extra flags (pkg-config output
// and the like) a user typed into the Run button's right-click Setup
// popup -- keyed by project root (mep.getcwd()) so a project's flags
// don't leak into an unrelated one. Stored at $XDG_DATA_HOME/mep/
// run_config.json as {"entries": {project_root: {ext: {...}}}}, same
// MepDataDir()/ReadJsonFile/WriteJsonFile/PushJson/LuaToJson convention
// as the babel cache just above -- the whole table is an opaque Lua
// value from this side of the boundary (kBuiltinRunButton, main.cpp),
// so no bespoke per-field (de)serialization is needed here either.
#if !defined(__EMSCRIPTEN__)
namespace {
/**
 * @brief Returns the on-disk path of the persisted Run button config file.
 * @return Path to run_config.json under the mep data directory.
 */
std::string RunConfigPath() { return MepDataDir() + "/run_config.json"; }
}  // namespace
#endif

/**
 * @brief Implements mep.run_config_load(): loads the persisted Run button config from disk.
 * @param L Lua state.
 * @return Number of values pushed (1: the config's "entries" table, or an empty table if there is no persisted config -- native build only; the wasm build always returns empty).
 */
int l_run_config_load(lua_State *L) {
#if !defined(__EMSCRIPTEN__)
    Json doc;
    if (ReadJsonFile(RunConfigPath(), &doc) && doc.is_object()) {
        const Json &entries = doc.get("entries");
        if (entries.is_object()) {
            PushJson(L, entries);
            return 1;
        }
    }
#endif
    lua_newtable(L);
    return 1;
}

/**
 * @brief Implements mep.run_config_save(entries): persists the Run button config to disk (a no-op on the wasm build).
 * @param L Lua state; arg 1 is the config's {project_root: {ext: {...}}} entries table.
 * @return Number of values pushed (0).
 */
int l_run_config_save(lua_State *L) {
#if !defined(__EMSCRIPTEN__)
    luaL_checktype(L, 1, LUA_TTABLE);
    Json doc = Json::Object();
    doc["entries"] = LuaToJson(L, 1);
    WriteJsonFile(RunConfigPath(), doc);
#endif
    return 0;
}

// --- LSP client (NVIM_PARITY_PLAN.md Part V Phase 20) ----------------------
//
// A Content-Length-framed JSON-RPC 2.0 client over Phase 1's Job
// subsystem, using Job's *raw* (unsplit) stdout mode rather than its
// line-buffered one. Line buffering was the first approach tried, on the
// theory that a compact-JSON message body never contains a raw newline
// byte so it'd always arrive as exactly one "line" -- true, but it missed
// that the *wire format itself* has no trailing newline after a body, so
// back-to-back messages sent with no gap (lua-language-server sends
// several notifications immediately on startup) get silently concatenated
// by line-splitting into one corrupt, unparseable "line": message 1's
// body immediately followed by message 2's "Content-Length: N" header,
// both on what line-splitting sees as a single line. Caught during
// verification (a debug trace of the exact bytes crossing the pipe showed
// the concatenation directly). Fixed by parsing Content-Length framing
// against a raw byte accumulator instead, which is what a byte-count-
// framed protocol actually calls for.
struct LspClientState {
    LuaEnv *env = nullptr;
    int client_id = 0;      // set right after JobManager::Spawn (l_lsp_start) or
                             // TcpJsonRpcManager::Connect (l_lsp_connect) returns
    std::string buffer;     // raw bytes accumulated, header+body(es) consumed as they complete
    int expected_len = -1;  // -1 = still accumulating headers for the next message
    int next_request_id = 1;
    std::unordered_map<int, int> pending;                    // request id -> Lua callback ref
    std::unordered_map<std::string, int> notification_refs;  // method -> Lua callback ref
    // method -> Lua callback ref, for server-initiated *requests* (they
    // carry both "id" and "method", unlike a plain notification) -- e.g.
    // DAP's runInTerminal. See DispatchLspMessage's request branch below.
    std::unordered_map<std::string, int> request_refs;
    // True for a DAP client (mep.lsp_start/lsp_connect's `dap` opt).
    // Real-adapter testing (spawning an actual lldb-dap and sending it a
    // JSON-RPC-2.0-shaped `initialize`) turned up a genuine, previously
    // unverified protocol mismatch: only the Content-Length *header*
    // framing is shared between LSP and DAP -- the JSON body underneath
    // is a completely different shape. LSP: {jsonrpc, id, method, params}
    // requests, responses keyed by "id". DAP: {seq, type: "request",
    // command, arguments} requests; {seq, type: "response", request_seq,
    // success, command, body} responses; {seq, type: "event", event,
    // body} server-initiated events -- no "jsonrpc"/"method"/"id" fields
    // at all. l_lsp_request/DispatchLspMessage below branch on this flag
    // to build/parse the right shape; every other part of this client
    // (framing, buffering, the pending/notification_refs/request_refs
    // maps, JobManager vs. TcpJsonRpcManager transport selection) is
    // unaffected and stays shared between LSP and DAP.
    bool dap_mode = false;
};

std::unordered_map<int, std::shared_ptr<LspClientState>> g_lsp_clients;

/**
 * @brief Serializes a JSON-RPC message and wraps it in LSP's Content-Length header framing for transmission.
 * @param msg JSON-RPC message to frame.
 * @return The complete "Content-Length: N\r\n\r\n<body>" byte string ready to write to the server's stdin.
 */
std::string LspFrame(const Json &msg) {
    std::string body = msg.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/**
 * @brief Writes a framed JSON-RPC message to a client's transport -- a spawned
 * process's stdin (JobManager) or a raw TCP socket (TcpJsonRpcManager),
 * chosen by whether `client_id` falls in the kTcpClientIdBase+ range
 * TcpJsonRpcManager::Connect hands out. The only place either manager's
 * write path is called from, so every caller (requests, notifications,
 * and DispatchLspMessage's own auto-responses to server requests) is
 * transport-agnostic.
 * @param client_id Id of the target LSP/DAP client.
 * @param framed Already Content-Length-framed bytes (see LspFrame).
 */
void SendLspRaw(int client_id, const std::string &framed) {
    if (client_id >= kTcpClientIdBase) {
        TcpJsonRpcManager::Instance().Send(client_id, framed);
    } else {
        JobManager::Instance().WriteStdin(client_id, framed);
    }
}

/**
 * @brief Parses one complete DAP message (see LspClientState::dap_mode) and
 * routes it to the matching pending-request callback (a "response", matched
 * by request_seq), event handler (an "event"), or request handler (a
 * server-initiated "request", answered with a synthesized DAP response).
 * A response/event handler is called with a normalized {result=...} or
 * {error={message=...}} table (response) or the event's own body (event) --
 * the same shape kBuiltinDap's Lua already expects from the LSP path, so
 * that code needs no transport-aware branching of its own.
 * @param state DAP client state whose pending requests/notification/request handlers are consulted.
 * @param msg Parsed DAP message.
 */
void DispatchDapMessage(LspClientState &state, const Json &msg) {
    std::string type = msg.contains("type") ? msg.get("type").as_string("") : "";
    if (type == "response") {
        int request_seq = msg.contains("request_seq") ? msg.get("request_seq").as_int(-1) : -1;
        auto it = state.pending.find(request_seq);
        if (it == state.pending.end()) return;
        int ref = it->second;
        state.pending.erase(it);
        Json normalized = Json::Object();
        if (msg.contains("success") ? msg.get("success").as_bool(true) : true) {
            normalized["result"] = msg.contains("body") ? msg.get("body") : Json::Object();
        } else {
            Json err = Json::Object();
            err["message"] = msg.contains("message") ? msg.get("message") : Json("DAP request failed");
            normalized["error"] = err;
        }
        state.env->CallRefWithJson(ref, normalized);
        state.env->UnrefFunction(ref);
    } else if (type == "event") {
        const std::string &event = msg.contains("event") ? msg.get("event").as_string() : std::string();
        auto it = state.notification_refs.find(event);
        if (it != state.notification_refs.end()) {
            state.env->CallRefWithJson(it->second, msg.contains("body") ? msg.get("body") : Json::Object());
        }
    } else if (type == "request") {
        // Server-initiated request (e.g. runInTerminal) -- DAP requires a
        // response here too, same reasoning as the LSP path's request
        // branch below: always answer, with {} if no handler is registered.
        const std::string &command = msg.contains("command") ? msg.get("command").as_string() : std::string();
        Json arguments = msg.contains("arguments") ? msg.get("arguments") : Json::Object();
        auto it = state.request_refs.find(command);
        Json result = it != state.request_refs.end() ? state.env->CallRefWithJsonReturningJson(it->second, arguments)
                                                       : Json::Object();
        Json response = Json::Object();
        response["seq"] = Json(state.next_request_id++);
        response["type"] = Json("response");
        response["request_seq"] = msg.contains("seq") ? msg.get("seq") : Json(0);
        response["success"] = Json(true);
        response["command"] = Json(command);
        response["body"] = result;
        SendLspRaw(state.client_id, LspFrame(response));
    }
}

/**
 * @brief Parses one complete JSON-RPC message body and routes it to the matching
 * pending-request callback, notification handler, or (for a server-initiated
 * *request* -- both "id" and "method" present) request handler, writing a
 * JSON-RPC response back for the latter. Dispatches to DispatchDapMessage
 * instead when the client is in DAP mode (see LspClientState::dap_mode) --
 * DAP and LSP share Content-Length framing but not the JSON body shape.
 * @param state LSP client state whose pending requests/notification/request handlers are consulted; silently returns if body doesn't parse as JSON.
 * @param body Decoded JSON-RPC message body (no framing headers).
 */
void DispatchLspMessage(LspClientState &state, const std::string &body) {
    Json msg;
    if (!Json::Parse(body, &msg)) return;
    if (state.dap_mode) {
        DispatchDapMessage(state, msg);
        return;
    }
    if (msg.contains("id") && msg.contains("method")) {
        // A server-initiated *request* (e.g. DAP's runInTerminal) -- unlike
        // a plain notification, the protocol requires a reply. Previously
        // this fell into the notification branch below and got no reply at
        // all, leaving the adapter waiting forever for one (a real,
        // documented gap -- see kBuiltinDap's own comment in main.cpp).
        // Always answer, even with an empty {} result, if no handler is
        // registered -- an unanswered request is worse than a no-op one.
        const std::string &method = msg.get("method").as_string();
        Json params = msg.contains("params") ? msg.get("params") : Json::Object();
        auto it = state.request_refs.find(method);
        Json result = it != state.request_refs.end() ? state.env->CallRefWithJsonReturningJson(it->second, params)
                                                       : Json::Object();
        Json response = Json::Object();
        response["jsonrpc"] = Json("2.0");
        response["id"] = msg.get("id");
        response["result"] = result;
        SendLspRaw(state.client_id, LspFrame(response));
    } else if (msg.contains("id")) {
        int id = msg.get("id").as_int();
        auto it = state.pending.find(id);
        if (it != state.pending.end()) {
            int ref = it->second;
            state.pending.erase(it);
            state.env->CallRefWithJson(ref, msg);
            state.env->UnrefFunction(ref);
        }
    } else if (msg.contains("method")) {
        const std::string &method = msg.get("method").as_string();
        auto it = state.notification_refs.find(method);
        if (it != state.notification_refs.end()) {
            state.env->CallRefWithJson(it->second, msg.contains("params") ? msg.get("params") : Json());
        }
    }
}

// Consumes as many complete Content-Length-framed messages as `state.buffer`
// currently holds, leaving any trailing partial message buffered for the
// next chunk to complete.
/**
 * @brief Consumes as many complete Content-Length-framed messages as state.buffer currently holds, dispatching each and leaving any trailing partial message buffered for the next chunk.
 * @param state LSP client state whose raw byte buffer is parsed and drained.
 */
void PumpLspBuffer(LspClientState &state) {
    for (;;) {
        if (state.expected_len < 0) {
            size_t header_end = state.buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) return;  // headers incomplete, wait for more
            size_t pos = 0;
            while (pos < header_end) {
                size_t eol = state.buffer.find("\r\n", pos);
                if (eol == std::string::npos || eol > header_end) eol = header_end;
                if (state.buffer.compare(pos, 15, "Content-Length:") == 0) {
                    state.expected_len = std::atoi(state.buffer.c_str() + pos + 15);
                }
                pos = eol + 2;
            }
            state.buffer.erase(0, header_end + 4);
            if (state.expected_len < 0) return;  // malformed: no Content-Length header seen
        }
        if (state.buffer.size() < static_cast<size_t>(state.expected_len)) return;  // body incomplete
        std::string body = state.buffer.substr(0, static_cast<size_t>(state.expected_len));
        state.buffer.erase(0, static_cast<size_t>(state.expected_len));
        state.expected_len = -1;
        DispatchLspMessage(state, body);
    }
}

// NVIM_PARITY_PLAN.md Phase 20 gap: a request pending when the server
// process/connection dies used to never fire its callback at all --
// state->pending's callback refs, and the Lua coroutines/closures waiting
// on them, just leaked/hung forever. Fires each with a synthetic JSON-RPC
// error response (same shape a real error reply would have, so callers
// already checking `.error` need no new code path) and mirrors
// l_lsp_stop's own g_lsp_clients cleanup so a later
// mep.lsp_is_running/lsp_request against this client_id correctly sees it
// as gone rather than silently queuing forever. Shared by l_lsp_start's
// on_exit (a spawned process died) and l_lsp_connect's on_exit (a TCP
// connection closed) -- from this dispatch layer's point of view they're
// the same event.
/**
 * @brief Fires every still-pending request's callback on `state` with a synthetic JSON-RPC error and removes it from g_lsp_clients.
 * @param state The client whose pending requests should be failed and which should be forgotten.
 */
void FireLspExitErrorAndCleanup(const std::shared_ptr<LspClientState> &state) {
    for (auto &kv : state->pending) {
        Json err = Json::Object();
        err["jsonrpc"] = Json("2.0");
        err["id"] = Json(kv.first);
        Json err_obj = Json::Object();
        err_obj["code"] = Json(-32000);
        err_obj["message"] = Json("LSP/DAP server exited");
        err["error"] = err_obj;
        state->env->CallRefWithJson(kv.second, err);
        state->env->UnrefFunction(kv.second);
    }
    state->pending.clear();
    if (state->client_id != 0) g_lsp_clients.erase(state->client_id);
}

/**
 * @brief Implements mep.lsp_start(argv[, opts]): spawns an LSP server process and registers it as a tracked client.
 * @param L Lua state; arg 1 is a Lua array of argv strings for the server command, optional arg 2 is an {cwd=, dap=} options table.
 * @return Number of values pushed (1: the new client id, or 0 if the spawn failed).
 */
int l_lsp_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv.emplace_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    std::string cwd;
    bool dap_mode = false;
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "cwd");
        if (lua_isstring(L, -1)) cwd = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "dap");
        dap_mode = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    auto state = std::make_shared<LspClientState>();
    state->env = GetLuaEnv(L);
    state->dap_mode = dap_mode;
    JobManager::Callbacks cb;
    /**
     * @brief Job stdout callback: appends raw output bytes to the client's buffer and pumps out any complete LSP messages.
     * @param chunk Raw stdout bytes received from the server process.
     */
    cb.on_stdout_raw = [state](const std::string &chunk) {
        state->buffer += chunk;
        PumpLspBuffer(*state);
    };
    /**
     * @brief Job exit callback: see FireLspExitErrorAndCleanup.
     * @param (unused) The server process's exit code.
     */
    cb.on_exit = [state](int) { FireLspExitErrorAndCleanup(state); };
    int id = JobManager::Instance().Spawn(argv, cwd, std::move(cb));
    if (id != 0) {
        state->client_id = id;
        g_lsp_clients[id] = state;
    }
    lua_pushinteger(L, id);
    return 1;
}

// mep.lsp_connect(host, port) -> client_id (always in the kTcpClientIdBase+
// range; never 0). A TCP-transport sibling of mep.lsp_start for DAP
// adapters that speak JSON-RPC over a raw socket instead of stdio (R's
// vscDebugger: mep.dap_start spawns the R process itself separately via
// mep.term_start, waits for it to start listening, then calls this to
// open the actual DAP channel). The returned id works with every other
// mep.lsp_* function exactly like a mep.lsp_start id does -- g_lsp_clients/
// LspClientState/DispatchLspMessage don't know or care which transport
// backs a given client.
/**
 * @brief Implements mep.lsp_connect(host, port): opens a TCP connection and registers it as a tracked JSON-RPC client.
 * @param L Lua state; arg 1 is the host, arg 2 is the port.
 * @return Number of values pushed (1: the new client id, always in the kTcpClientIdBase+ range).
 */
int l_lsp_connect(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    int port = static_cast<int>(luaL_checkinteger(L, 2));

    auto state = std::make_shared<LspClientState>();
    state->env = GetLuaEnv(L);
    // Its only consumer today (kBuiltinDap's R/vscDebugger path) is DAP
    // over TCP -- always DAP-shaped messages (see LspClientState::dap_mode).
    state->dap_mode = true;
    TcpJsonRpcManager::Callbacks cb;
    /**
     * @brief TCP data callback: appends raw bytes to the client's buffer and pumps out any complete LSP/DAP messages.
     * @param chunk Raw bytes received from the socket.
     */
    cb.on_data_raw = [state](const std::string &chunk) {
        state->buffer += chunk;
        PumpLspBuffer(*state);
    };
    /**
     * @brief TCP exit callback: see FireLspExitErrorAndCleanup.
     * @param (unused) Always -1 for a socket (no process exit code).
     */
    cb.on_exit = [state](int) { FireLspExitErrorAndCleanup(state); };
    int id = TcpJsonRpcManager::Instance().Connect(host, port, std::move(cb));
    state->client_id = id;
    g_lsp_clients[id] = state;
    lua_pushinteger(L, id);
    return 1;
}

// mep.lsp_request(client_id, method, params [, callback]) -> request id (or
// -1 if the client doesn't exist). `callback(message)` is invoked once
// with the full JSON-RPC response object (check `.error`/`.result`
// yourself) -- omit for a fire-and-forget request nobody needs a reply to.
/**
 * @brief Implements mep.lsp_request(client_id, method, params [, callback]): sends a JSON-RPC request to an LSP client, optionally registering a callback for its reply.
 * @param L Lua state; arg 1 is the client id, arg 2 is the method name, arg 3 is the params value (or nil), optional arg 4 is a callback function invoked with the full response.
 * @return Number of values pushed (1: the request id, or -1 if the client doesn't exist).
 */
int l_lsp_request(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    const char *method = luaL_checkstring(L, 2);
    Json params = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? LuaToJson(L, 3) : Json::Object();
    int cb_ref = 0;
    if (lua_gettop(L) >= 4 && lua_isfunction(L, 4)) {
        lua_pushvalue(L, 4);
        cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    auto it = g_lsp_clients.find(client_id);
    if (it == g_lsp_clients.end()) {
        lua_pushinteger(L, -1);
        return 1;
    }
    LspClientState &state = *it->second;
    int req_id = state.next_request_id++;
    if (cb_ref != 0) state.pending[req_id] = cb_ref;
    Json msg = Json::Object();
    if (state.dap_mode) {
        // Real DAP wire shape (see LspClientState::dap_mode) -- confirmed
        // against an actually-spawned lldb-dap: it rejects a JSON-RPC-2.0
        // {jsonrpc, id, method, params} message outright ("DAP session
        // error: missing value at (root).type").
        msg["seq"] = Json(req_id);
        msg["type"] = Json("request");
        msg["command"] = Json(method);
        msg["arguments"] = params;
    } else {
        msg["jsonrpc"] = Json("2.0");
        msg["id"] = Json(req_id);
        msg["method"] = Json(method);
        msg["params"] = params;
    }
    SendLspRaw(client_id, LspFrame(msg));
    lua_pushinteger(L, req_id);
    return 1;
}

/**
 * @brief Implements mep.lsp_notify(client_id, method, params): sends a fire-and-forget JSON-RPC notification to an LSP client.
 * @param L Lua state; arg 1 is the client id, arg 2 is the method name, arg 3 is the params value (or nil).
 * @return Number of values pushed (0).
 */
int l_lsp_notify(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    const char *method = luaL_checkstring(L, 2);
    Json params = (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) ? LuaToJson(L, 3) : Json::Object();
    if (g_lsp_clients.find(client_id) == g_lsp_clients.end()) return 0;
    Json msg = Json::Object();
    msg["jsonrpc"] = Json("2.0");
    msg["method"] = Json(method);
    msg["params"] = params;
    SendLspRaw(client_id, LspFrame(msg));
    return 0;
}

// mep.lsp_on_request(client_id, method, fn): like mep.lsp_on_notification,
// but for a server-initiated *request* (has both "id" and "method") --
// DAP's runInTerminal is the motivating case. fn(params) is called and
// whatever it returns (a table, or nothing) becomes the JSON-RPC
// response's `result`; DispatchLspMessage sends {} automatically if no
// handler is registered for a given method, so a request is never simply
// left unanswered.
/**
 * @brief Implements mep.lsp_on_request(client_id, method, fn): registers fn(params) to run each time the server sends a given request, replying with fn's return value; a second registration for the same method replaces the first.
 * @param L Lua state; arg 1 is the client id, arg 2 is the request method name, arg 3 is the handler function.
 * @return Number of values pushed (0).
 */
int l_lsp_on_request(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    const char *method = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    auto it = g_lsp_clients.find(client_id);
    if (it == g_lsp_clients.end()) return 0;
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int &slot = it->second->request_refs[method];
    if (slot != 0) GetLuaEnv(L)->UnrefFunction(slot);
    slot = ref;
    return 0;
}

// mep.lsp_on_notification(client_id, method, fn): fn(params) each time the
// server sends that notification (e.g. "textDocument/publishDiagnostics").
// One handler per method per client -- registering again replaces it.
/**
 * @brief Implements mep.lsp_on_notification(client_id, method, fn): registers fn(params) to run each time the server sends a given notification; a second registration for the same method replaces the first.
 * @param L Lua state; arg 1 is the client id, arg 2 is the notification method name, arg 3 is the handler function.
 * @return Number of values pushed (0).
 */
int l_lsp_on_notification(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    const char *method = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    auto it = g_lsp_clients.find(client_id);
    if (it == g_lsp_clients.end()) return 0;
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int &slot = it->second->notification_refs[method];
    if (slot != 0) GetLuaEnv(L)->UnrefFunction(slot);
    slot = ref;
    return 0;
}

/**
 * @brief Implements mep.lsp_stop(client_id): kills an LSP server process and removes its tracked client state.
 * @param L Lua state; arg 1 is the client id to stop.
 * @return Number of values pushed (0).
 */
int l_lsp_stop(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    if (client_id >= kTcpClientIdBase) {
        TcpJsonRpcManager::Instance().Close(client_id);
    } else {
        JobManager::Instance().Kill(client_id);
    }
    g_lsp_clients.erase(client_id);
    return 0;
}

/**
 * @brief Implements mep.lsp_is_running(client_id): checks whether an LSP client's server process/connection is still running.
 * @param L Lua state; arg 1 is the client id to check.
 * @return Number of values pushed (1: boolean, whether the process/connection is running).
 */
int l_lsp_is_running(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    bool running = client_id >= kTcpClientIdBase ? TcpJsonRpcManager::Instance().IsRunning(client_id)
                                                  : JobManager::Instance().IsRunning(client_id);
    lua_pushboolean(L, running);
    return 1;
}

// mep.set_completion_source(fn): fn(prefix) -> array of candidate words
// (Phase 22).
/**
 * @brief Implements mep.set_completion_source(fn): registers fn(prefix) -> array of candidate words as the buffer's completion source.
 * @param L Lua state; arg 1 is the completion-source function.
 * @return Number of values pushed (0).
 */
int l_set_completion_source(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetCompletionSourceRef(ref);
    return 0;
}

// mep.set_completion_accept_hook(fn): see SetCompletionAcceptHookRef's
// comment (editor.h) -- Phase 23's LSP insertTextFormat=Snippet wiring.
/**
 * @brief Implements mep.set_completion_accept_hook(fn): registers fn as the hook invoked when a completion item is accepted (used for LSP insertTextFormat=Snippet expansion).
 * @param L Lua state; arg 1 is the accept-hook function.
 * @return Number of values pushed (0).
 */
int l_set_completion_accept_hook(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetCompletionAcceptHookRef(ref);
    return 0;
}

// mep.set_completion_resolve_hook(fn): see SetCompletionResolveHookRef's
// comment (editor.h) -- Phase 22 follow-up's completionItem/resolve gap.
int l_set_completion_resolve_hook(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetCompletionResolveHookRef(ref);
    return 0;
}

// mep.set_insert_tab_hook(fn): fn(shift) -> bool. Phase 23's Tab/Shift-Tab
// tabstop cycling -- see SetInsertTabHookRef's comment (editor.h).
int l_set_insert_tab_hook(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetInsertTabHookRef(ref);
    return 0;
}

// mep.set_on_directory_open(fn): fn(path). Editor::LoadFile calls this
// whenever ":e"/"mep.open" is asked to open a path that's a directory --
// see SetDirectoryOpenHookRef's own comment (editor.h). kBuiltinFileTree's
// mep.tree_open_in_pane is the intended (and, so far, only) registrant.
int l_set_on_directory_open(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetDirectoryOpenHookRef(ref);
    return 0;
}

int l_filename(lua_State *L) {
    lua_pushstring(L, GetEditor(L)->CurrentBuffer().filename.c_str());
    return 1;
}

// --- The browser pane's networking surface ---------------------------------
// mep.http_get(url [, timeout_ms]) -> {ok=, status=, status_text=, url=,
// content_type=, body=, error=}: a blocking GET (http_client.h). What
// :Browse uses to load a page from a URL -- plain sockets for http://
// (so a page served from localhost needs nothing but mep), curl for
// https://. `ok` is "a response arrived", whatever its status.
/**
 * @brief Implements mep.http_get(url, timeout_ms): performs a blocking HTTP GET.
 * @param L Lua state; arg 1 the URL, optional arg 2 the timeout in milliseconds (default 10000).
 * @return Number of values pushed (1: the response table).
 */
int l_http_get(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    const int timeout_ms = static_cast<int>(luaL_optinteger(L, 2, 10000));
    HttpResponse response = HttpGet(url, timeout_ms);
    lua_createtable(L, 0, 7);
    lua_pushboolean(L, response.ok);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, response.status);
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, response.status_text.data(), response.status_text.size());
    lua_setfield(L, -2, "status_text");
    lua_pushlstring(L, response.url.data(), response.url.size());
    lua_setfield(L, -2, "url");
    const std::string content_type = response.ContentType();
    lua_pushlstring(L, content_type.data(), content_type.size());
    lua_setfield(L, -2, "content_type");
    lua_pushlstring(L, response.body.data(), response.body.size());
    lua_setfield(L, -2, "body");
    lua_pushlstring(L, response.error.data(), response.error.size());
    lua_setfield(L, -2, "error");
    return 1;
}

namespace {
// Every static server this process has started (mep.http_serve). Process
// lifetime: a server keeps running until mep.http_stop or exit, whatever
// buffer/workspace is active -- it serves files, it doesn't belong to a pane.
std::vector<std::unique_ptr<HttpStaticServer>> &HttpServers() {
    static std::vector<std::unique_ptr<HttpStaticServer>> servers;
    return servers;
}
}  // namespace

// mep.http_serve(dir [, port]) -> port | nil, err: serves `dir` on
// 127.0.0.1 (http_server.h). Port 0/omitted lets the OS pick a free one;
// asking again for a directory that is already being served returns that
// server's port instead of starting a second one.
/**
 * @brief Implements mep.http_serve(dir, port): starts an in-process static file server on 127.0.0.1.
 * @param L Lua state; arg 1 the directory, optional arg 2 the port (0 = any free port).
 * @return Number of values pushed (1: the bound port; or 2: nil and an error message).
 */
int l_http_serve(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    const int port = static_cast<int>(luaL_optinteger(L, 2, 0));
    std::error_code ec;
    const std::string canonical = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec).string();
    for (const auto &server : HttpServers()) {
        if (server->Running() && server->Root() == canonical && (port == 0 || port == server->Port())) {
            lua_pushinteger(L, server->Port());
            return 1;
        }
    }
    auto server = std::make_unique<HttpStaticServer>();
    std::string error;
    if (!server->Start(dir, port, &error)) {
        lua_pushnil(L);
        lua_pushlstring(L, error.data(), error.size());
        return 2;
    }
    lua_pushinteger(L, server->Port());
    HttpServers().push_back(std::move(server));
    return 1;
}

/**
 * @brief Implements mep.http_stop(port): stops the server on `port`, or every server when omitted/0.
 * @param L Lua state; optional arg 1 the port.
 * @return Number of values pushed (1: how many servers were stopped).
 */
int l_http_stop(lua_State *L) {
    const int port = static_cast<int>(luaL_optinteger(L, 1, 0));
    auto &servers = HttpServers();
    int stopped = 0;
    for (auto it = servers.begin(); it != servers.end();) {
        if (port == 0 || (*it)->Port() == port) {
            (*it)->Stop();
            it = servers.erase(it);
            stopped++;
        } else {
            ++it;
        }
    }
    lua_pushinteger(L, stopped);
    return 1;
}

/**
 * @brief Implements mep.http_servers(): lists the running static servers.
 * @param L Lua state.
 * @return Number of values pushed (1: array of {port=, root=, requests=}).
 */
int l_http_servers(lua_State *L) {
    const auto &servers = HttpServers();
    lua_createtable(L, static_cast<int>(servers.size()), 0);
    int index = 1;
    for (const auto &server : servers) {
        if (!server->Running()) continue;
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, server->Port());
        lua_setfield(L, -2, "port");
        lua_pushlstring(L, server->Root().data(), server->Root().size());
        lua_setfield(L, -2, "root");
        lua_pushinteger(L, static_cast<lua_Integer>(server->RequestCount()));
        lua_setfield(L, -2, "requests");
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// mep.url_normalize(text) -> what the omnibar would load for `text`
// (urlutil::NormalizeOmnibarInput): "localhost:8000" -> "http://localhost:8000",
// ":8080/x" -> "http://localhost:8080/x", "example.com" -> "https://example.com";
// a URL with a scheme or a local path comes back unchanged.
/**
 * @brief Implements mep.url_normalize(text): normalizes omnibar input into a loadable target.
 * @param L Lua state; arg 1 the typed text.
 * @return Number of values pushed (1: the normalized target).
 */
int l_url_normalize(lua_State *L) {
    const std::string out = urlutil::NormalizeOmnibarInput(luaL_checkstring(L, 1));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

/**
 * @brief Implements mep.url_resolve(base, ref): resolves a reference against an absolute URL.
 * @param L Lua state; arg 1 the base URL, arg 2 the reference.
 * @return Number of values pushed (1: the absolute URL).
 */
int l_url_resolve(lua_State *L) {
    const std::string out = urlutil::ResolveUrl(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

/**
 * @brief Implements mep.html_settle([timeout_ms]): runs the focused html pane's page (timers, promises, animation frames) until it goes idle or the budget is spent.
 * @param L Lua state; optional arg 1 is the wall-clock budget in ms (default 3000).
 * @return 1 (pushes whether the page went idle within the budget).
 */
int l_html_settle(lua_State *L) {
    Editor *e = GetEditor(L);
    const int budget = static_cast<int>(luaL_optinteger(L, 1, 3000));
    lua_pushboolean(L, e->SettleHtmlScripts(e->CurrentBufferId(), budget));
    return 1;
}

/**
 * @brief Implements mep.html_title(): the focused html pane's document title (what a page's script last set).
 * @param L Lua state.
 * @return Number of values pushed (1: the title, or nil when the focused buffer isn't an html pane).
 */
int l_html_title(lua_State *L) {
    Editor *ed = GetEditor(L);
    const int buffer_id = ed->CurrentBufferId();
    if (!ed->IsHtmlBuffer(buffer_id)) {
        lua_pushnil(L);
        return 1;
    }
    const std::string title = ed->HtmlTitle(buffer_id);
    lua_pushlstring(L, title.data(), title.size());
    return 1;
}

/**
 * @brief Implements mep.html_omnibar_edit(): puts the focused html pane's omnibar into edit mode.
 * @param L Lua state.
 * @return Number of values pushed (0).
 */
int l_html_omnibar_edit(lua_State *L) {
    Editor *ed = GetEditor(L);
    ed->BeginHtmlOmnibarEdit(ed->CurrentBufferId());
    return 0;
}

// mep.html_open(path [, origin]): opens `path` (a real local .html file's
// path -- see html_doc.h) as a rendered HtmlSession preview pane in the
// current pane, parsing+running its scripts (Editor::OpenHtmlInPlace).
// Deliberately not reachable from :e's own LoadFile dispatch (see
// Mode::Html's own comment in editor.h) -- mep.browse_command's in-pane
// default (kBuiltinTextTools, main.cpp) is the only intended caller; a
// remote URL goes through that same function too, but only after
// mep.browse_command has already fetched it to a local temp file via
// curl, since this function itself never does any network I/O. `origin`
// (HtmlSession::origin) is the user-facing URL/path to remember for
// reload/the address bar -- defaults to `path` itself, correct for a
// plain local-file open; mep.browse_open_in_pane passes the real URL
// explicitly for its own curl-fetched-tempfile case.
int l_html_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *origin = luaL_optstring(L, 2, path);
#if !defined(__EMSCRIPTEN__)
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        GetEditor(L)->Notify("Can't open \"" + std::string(path) + "\"", Editor::NotifyLevel::Error);
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Absolute-ized (rather than stored as whatever relative/absolute
    // string the caller passed) so a <img src="relative/path"> inside the
    // page can be resolved against this file's own directory regardless of
    // what mep's cwd is by the time DrawPane's HTML branch (main.cpp) lays
    // that out -- cwd can change (mep.fs_chdir) any time after this open
    // call, well before the next frame's own layout pass runs.
    std::error_code abs_ec;
    std::filesystem::path abs_path = std::filesystem::absolute(path, abs_ec);
    std::string source = abs_ec ? path : abs_path.string();
    GetEditor(L)->OpenHtmlInPlace(origin, source, bytes.data(), bytes.size());
#else
    // The wasm sandbox has no local filesystem of its own to read a
    // fetched/local html file from outside the bridge (see serve.ts's own
    // header) -- out of scope for the same reason mep.browse's external
    // WebKitGTK window already is on that build.
    GetEditor(L)->Notify("mep.html_open: not supported in the wasm build", Editor::NotifyLevel::Error);
#endif
    return 0;
}

// mep.html_current_origin(): the current pane's HtmlSession::origin, or
// nil if the current pane isn't an HTML pane -- lets mep.browse_reload/
// mep.browse_open_bar (kBuiltinTextTools) know what to re-fetch/prefill
// without a numeric buffer-id round-trip through Lua (both this and
// mep.html_reload below always act on "whatever pane is current", the
// same implicit-current-buffer convention mep.filename()/mep.line_count()
// already use throughout).
int l_html_current_origin(lua_State *L) {
    const Editor *ed = GetEditor(L);
    const HtmlSession *sess = ed->GetHtml(ed->CurrentBufferId());
    if (!sess) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, sess->origin.data(), sess->origin.size());
    return 1;
}

// mep.html_reload(path [, origin]): re-reads local file `path` and
// re-parses it INTO the current pane's existing HtmlSession in place
// (Editor::ReloadHtmlBuffer) -- unlike mep.html_open, never creates a new
// buffer/session or does a dedup-by-source lookup; a hard overwrite of
// whichever HTML pane is currently active. `origin` defaults to `path`,
// same convention as mep.html_open. A no-op (with a notification) if the
// current pane isn't an HTML pane, or if `path` can't be read.
int l_html_reload(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *origin = luaL_optstring(L, 2, path);
#if !defined(__EMSCRIPTEN__)
    Editor *ed = GetEditor(L);
    int buffer_id = ed->CurrentBufferId();
    if (!ed->GetHtml(buffer_id)) {
        ed->Notify("Not an HTML pane", Editor::NotifyLevel::Warn);
        return 0;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ed->Notify("Can't open \"" + std::string(path) + "\"", Editor::NotifyLevel::Error);
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ed->ReloadHtmlBuffer(buffer_id, origin, path, bytes.data(), bytes.size());
#else
    GetEditor(L)->Notify("mep.html_reload: not supported in the wasm build", Editor::NotifyLevel::Error);
#endif
    return 0;
}

// mep.html_navigate(path [, origin]) is html_reload's history-recording
// counterpart, used by the in-pane browser for links and the address bar.
int l_html_navigate(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *origin = luaL_optstring(L, 2, path);
#if !defined(__EMSCRIPTEN__)
    Editor *ed = GetEditor(L);
    int buffer_id = ed->CurrentBufferId();
    if (!ed->GetHtml(buffer_id)) {
        ed->Notify("Not an HTML pane", Editor::NotifyLevel::Warn);
        return 0;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ed->Notify("Can't open \"" + std::string(path) + "\"", Editor::NotifyLevel::Error);
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code abs_ec;
    std::filesystem::path abs_path = std::filesystem::absolute(path, abs_ec);
    std::string source = abs_ec ? path : abs_path.string();
    ed->NavigateHtmlBuffer(buffer_id, origin, source, bytes.data(), bytes.size());
#else
    GetEditor(L)->Notify("mep.html_navigate: not supported in the wasm build", Editor::NotifyLevel::Error);
#endif
    return 0;
}

// mep.pdf_reload(path): re-reads local file `path` and re-decodes it INTO
// the current pane's existing PdfSession in place (Editor::ReloadPdfBuffer)
// -- same "hard overwrite of whichever <type> pane is currently active,
// used to refresh an already-recompiled output file" role mep.html_reload
// plays for HTML (kBuiltinRunButton's org branch, main.cpp, is the
// intended caller for both). A no-op (with a notification) if the current
// pane isn't a PDF pane, or if `path` can't be read.
int l_pdf_reload(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#if !defined(__EMSCRIPTEN__)
    Editor *ed = GetEditor(L);
    int buffer_id = ed->CurrentBufferId();
    if (!ed->GetPdf(buffer_id)) {
        ed->Notify("Not a PDF pane", Editor::NotifyLevel::Warn);
        return 0;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ed->Notify("Can't open \"" + std::string(path) + "\"", Editor::NotifyLevel::Error);
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ed->ReloadPdfBuffer(buffer_id, bytes.data(), bytes.size());
#else
    GetEditor(L)->Notify("mep.pdf_reload: not supported in the wasm build", Editor::NotifyLevel::Error);
#endif
    return 0;
}

// mep.is_pdf_buffer(buffer_id) -> bool: whether `buffer_id` is a PDF
// pane -- lets Lua-side dispatch (the Structure sidebar, kBuiltinStructure)
// distinguish a PDF buffer from a normal text one by something other
// than sniffing the ".pdf" file extension.
int l_is_pdf_buffer(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->GetPdf(buffer_id) != nullptr);
    return 1;
}

// mep.pdf_outline(buffer_id) -> array of {title, page, depth}, or nil if
// buffer_id isn't a PDF pane or its document has no /Outlines dict at
// all (most PDFs don't). `page` is 1-indexed (matching mep.get_line's/
// TSStructureNode's own row convention throughout this codebase) with 0
// used as the "unresolved" sentinel -- PdfDoc::Outline's own -1 (a named
// destination this engine doesn't resolve, pdf_outline.h) shifted by the
// same +1 every real page index gets, landing on 0, which (unlike -1)
// is never a legitimate 1-indexed page number, so Lua-side code can
// just check `item.page == 0` rather than needing a second "resolved"
// boolean field.
int l_pdf_outline(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    const PdfSession *sess = GetEditor(L)->GetPdf(buffer_id);
    if (!sess || !sess->doc) {
        lua_pushnil(L);
        return 1;
    }
    std::vector<PdfOutlineItem> items = sess->doc->Outline();
    if (items.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    for (size_t i = 0; i < items.size(); ++i) {
        lua_newtable(L);
        lua_pushstring(L, items[i].title.c_str());
        lua_setfield(L, -2, "title");
        lua_pushinteger(L, items[i].page + 1);
        lua_setfield(L, -2, "page");
        lua_pushinteger(L, items[i].depth);
        lua_setfield(L, -2, "depth");
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.pdf_goto_page(buffer_id, page): jumps buffer_id's own PDF viewer
// to `page` (1-indexed, clamped to the valid range) -- a no-op if
// buffer_id isn't a PDF pane. Built for the Structure sidebar's PDF-
// outline click-to-jump (kBuiltinStructure, main.cpp), which has no
// text row/col to hand mep.set_cursor the way every other filetype's
// structure entries do.
int l_pdf_goto_page(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int page = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    GetEditor(L)->GotoPdfPage(buffer_id, page);
    return 0;
}

// mep.pdf_current_page(buffer_id) -> 1-indexed current page, or 0 if
// buffer_id isn't a PDF pane. Lets the Structure sidebar highlight the
// bookmark closest to the page currently on screen, the PDF-outline
// equivalent of mep_structure_current_index's cursor-row-based "current
// item" highlight for text buffers.
int l_pdf_current_page(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    const PdfSession *sess = GetEditor(L)->GetPdf(buffer_id);
    lua_pushinteger(L, sess ? sess->page + 1 : 0);
    return 1;
}

// mep.office_reload(path): re-reads local file `path` (docx or odt) and
// re-decodes it INTO the current pane's existing OfficeSession in place
// (Editor::ReloadOfficeBuffer) -- same role mep.html_reload/mep.pdf_reload
// play for their own formats. A no-op (with a notification) if the
// current pane isn't an office pane, or if `path` can't be read.
int l_office_reload(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#if !defined(__EMSCRIPTEN__)
    Editor *ed = GetEditor(L);
    int buffer_id = ed->CurrentBufferId();
    if (!ed->GetOffice(buffer_id)) {
        ed->Notify("Not an office document pane", Editor::NotifyLevel::Warn);
        return 0;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ed->Notify("Can't open \"" + std::string(path) + "\"", Editor::NotifyLevel::Error);
        return 0;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ed->ReloadOfficeBuffer(buffer_id, path, bytes.data(), bytes.size());
#else
    GetEditor(L)->Notify("mep.office_reload: not supported in the wasm build", Editor::NotifyLevel::Error);
#endif
    return 0;
}

// mep.doc_export_html_to_latex(html, title, author, base_dir): thin
// binding over doc_export.h's ExportHtmlToLatex -- see its own header for
// the actual conversion. Pure string in, string out, no I/O of its own
// (kBuiltinOrgExport, main.cpp, writes the returned LaTeX to a .tex file
// itself, then spawns tectonic the same way mep_org_latex_render already
// does for math-fragment previews). Native + wasm both fine here -- no
// filesystem access except reading a local <img src>, guarded the same
// way ExportHtmlToOdt below already is.
int l_doc_export_html_to_latex(lua_State *L) {
    const char *html = luaL_checkstring(L, 1);
    const char *title = luaL_optstring(L, 2, "");
    const char *author = luaL_optstring(L, 3, "");
    const char *base_dir = luaL_optstring(L, 4, "");
    std::string latex = ExportHtmlToLatex(html, title, author, base_dir);
    lua_pushlstring(L, latex.data(), latex.size());
    return 1;
}

// mep.doc_export_html_to_odt(html, out_path, title, author, base_dir):
// thin binding over doc_export.h's ExportHtmlToOdt. Returns true on
// success; on failure returns false plus an error string (so a caller
// can `local ok, err = mep.doc_export_html_to_odt(...)`).
int l_doc_export_html_to_odt(lua_State *L) {
    const char *html = luaL_checkstring(L, 1);
    const char *out_path = luaL_checkstring(L, 2);
    const char *title = luaL_optstring(L, 3, "");
    const char *author = luaL_optstring(L, 4, "");
    const char *base_dir = luaL_optstring(L, 5, "");
#if !defined(__EMSCRIPTEN__)
    std::string error;
    bool ok = ExportHtmlToOdt(html, out_path, title, author, base_dir, error);
    lua_pushboolean(L, ok);
    if (ok) return 1;
    lua_pushlstring(L, error.data(), error.size());
    return 2;
#else
    // Same "no local filesystem outside the bridge" constraint as
    // mep.html_open above -- writing an arbitrary .odt to a native path
    // isn't meaningful in the wasm sandbox.
    lua_pushboolean(L, false);
    lua_pushliteral(L, "mep.doc_export_html_to_odt: not supported in the wasm build");
    return 2;
#endif
}

// Myers diff (NVIM_PARITY_PLAN.md Part IV Phase 17): DiffHunk/
// MyersDiffHunks moved to editor.h/editor.cpp so Editor::GitGutterRefresh
// (kBuiltinGit's own C++ port) can call it directly too -- this is now
// just the mep.diff_lines Lua marshaling wrapper around it.

int l_diff_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> a, b;
    lua_Integer na = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= na; i++) {
        lua_rawgeti(L, 1, i);
        a.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    lua_Integer nb = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= nb; i++) {
        lua_rawgeti(L, 2, i);
        b.emplace_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    std::vector<DiffHunk> hunks = MyersDiffHunks(a, b);
    lua_newtable(L);
    for (size_t i = 0; i < hunks.size(); i++) {
        lua_newtable(L);
        lua_pushinteger(L, hunks[i].old_start);
        lua_setfield(L, -2, "old_start");
        lua_pushinteger(L, hunks[i].old_count);
        lua_setfield(L, -2, "old_count");
        lua_pushinteger(L, hunks[i].new_start);
        lua_setfield(L, -2, "new_start");
        lua_pushinteger(L, hunks[i].new_count);
        lua_setfield(L, -2, "new_count");
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.hint_jump() (Phase 13).
int l_hint_jump(lua_State *L) {
    GetEditor(L)->BeginHints();
    return 0;
}

// mep.quick_jump(query?): Editor::BeginQuickJump (TODO.org "quickjump
// capability") -- the typed-query jump kBuiltinQuickJump binds to `s`. An
// optional query is fed exactly as if typed (so a unique match jumps at
// once), which together with mep.quick_jump_matches()/quick_jump_pick()
// below makes the whole flow scriptable -- and testable over the agent
// socket without synthetic keyboard input.
int l_quick_jump(lua_State *L) {
    Editor *ed = GetEditor(L);
    ed->BeginQuickJump();
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) ed->QuickJumpFeed(lua_tostring(L, 1));
    return 0;
}

// mep.quick_jump_matches() -> array of {pane_id=, row=, col=, label=}
// (row/col 1-indexed like mep.cursor(); label "" for an unlabeled match),
// active pane's matches first -- empty when quick jump isn't active.
int l_quick_jump_matches(lua_State *L) {
    const Editor *ed = GetEditor(L);
    lua_newtable(L);
    if (!ed->IsQuickJumpActive()) return 1;
    const std::vector<QuickJumpMatch> &matches = ed->QuickJumpMatches();
    for (size_t i = 0; i < matches.size(); i++) {
        lua_newtable(L);
        lua_pushinteger(L, matches[i].pane_id);
        lua_setfield(L, -2, "pane_id");
        lua_pushinteger(L, matches[i].row + 1);
        lua_setfield(L, -2, "row");
        lua_pushinteger(L, matches[i].col + 1);
        lua_setfield(L, -2, "col");
        lua_pushstring(L, matches[i].label.c_str());
        lua_setfield(L, -2, "label");
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.quick_jump_pick(label) -> true if a current match carried that label
// (and the jump happened, focusing its pane when it's another one).
int l_quick_jump_pick(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->QuickJumpPick(luaL_checkstring(L, 1)) ? 1 : 0);
    return 1;
}

// mep.platform() -> "linux"/"macos"/"windows"/"wasm" (Phase 13 URL open).
int l_platform(lua_State *L) {
#if defined(__EMSCRIPTEN__)
    lua_pushstring(L, "wasm");
#elif defined(__APPLE__)
    lua_pushstring(L, "macos");
#elif defined(_WIN32)
    lua_pushstring(L, "windows");
#else
    lua_pushstring(L, "linux");
#endif
    return 1;
}

// mep.scratch() / mep.toggle_zen() (Phase 12).
int l_scratch(lua_State *L) {
    GetEditor(L)->OpenScratchBuffer();
    return 0;
}
int l_toggle_zen(lua_State *L) {
    GetEditor(L)->ToggleZenMode();
    return 0;
}
// mep.sheet_next() / mep.sheet_prev() -- Lua-reachable equivalent of the
// Ctrl-PageDown/Ctrl-PageUp keys HandleSheetNormalInput already binds
// (spreadsheet-pane Phase 4), for whichkey/custom-mapping consumers.
int l_sheet_next(lua_State *L) {
    GetEditor(L)->NextSheet();
    return 0;
}
int l_sheet_prev(lua_State *L) {
    GetEditor(L)->PrevSheet();
    return 0;
}
// mep.on_frame(fn): fn runs once per frame -- the polling-based building
// block for debounced buffer-changed/buffer-saved consumers (see
// LuaEnv::RunFrameHooks in lua_env.h for why this is polling, not a
// synchronous edit-time callback).
int l_on_frame(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetLuaEnv(L)->RegisterFrameHook(ref);
    return 0;
}
int l_buffer_change_epoch(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->ChangeEpoch());
    return 1;
}
int l_buffer_save_epoch(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->SaveEpoch());
    return 1;
}
// mep.now(): wall-clock seconds since program start (see Editor::Now()
// for why this exists instead of relying on Lua's os.clock(), which is
// CPU time).
int l_now(lua_State *L) {
    lua_pushnumber(L, GetEditor(L)->Now());
    return 1;
}

// mep.set_statusline(fn): fn(), called each frame, returns an array of
// {text=, hl=} segments replacing the built-in status line (Phase 11).
int l_set_statusline(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetStatuslineRef(ref);
    return 0;
}

// mep.set_winbar_click(fn): fn(dir_path), called when a directory segment of
// the per-pane header's path breadcrumb (mep's winbar equivalent) is
// clicked (Phase 11 click-dispatch gap). kBuiltinPickerSources wires this to
// mep.winbar_navigate by default.
int l_set_winbar_click(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetWinbarClickRef(ref);
    return 0;
}

// mep.set_leader(key): single-char string, the whichkey trigger (Phase 11).
int l_set_leader(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    if (key[0] != '\0') GetEditor(L)->SetLeaderKey(key[0]);
    return 0;
}

// mep.leader_map(sequence, description, fn[, icon[, icon_hl]]): binds a key
// sequence typed after the leader (e.g. mep.leader_map('ff', 'Find files',
// mep.find_files)). `icon` is an optional Nerd Font codepoint and `icon_hl`
// names its optional highlight group in the which-key popup.
int l_leader_map(lua_State *L) {
    const char *sequence = luaL_checkstring(L, 1);
    const char *description = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int icon = lua_isnoneornil(L, 4) ? 0 : static_cast<int>(luaL_checkinteger(L, 4));
    const char *icon_hl = lua_isnoneornil(L, 5) ? "" : luaL_checkstring(L, 5);
    GetEditor(L)->RegisterWhichKey(sequence, description, ref, icon, icon_hl);
    return 0;
}

// mep.leader_group(prefix, label[, icon[, icon_hl]]): names a group of leader.map bindings
// sharing `prefix` (e.g. mep.leader_group('o', 'org')) so the whichkey
// popup shows one collapsed, optionally icon-decorated row instead of every
// leaf under it. `icon`, when supplied, is a Nerd Font codepoint; `icon_hl`
// optionally names the highlight group used to color it.
int l_leader_group(lua_State *L) {
    const char *prefix = luaL_checkstring(L, 1);
    const char *label = luaL_checkstring(L, 2);
    int icon = lua_isnoneornil(L, 3) ? 0 : static_cast<int>(luaL_checkinteger(L, 3));
    const char *icon_hl = lua_isnoneornil(L, 4) ? "" : luaL_checkstring(L, 4);
    GetEditor(L)->RegisterWhichKeyGroup(prefix, label, icon, icon_hl);
    return 0;
}

// mep.icon_for_file(name) -> a short ASCII glyph (Phase 10).
int l_icon_for_file(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushstring(L, IconForFilename(name).c_str());
    return 1;
}

// mep.hl_for_file(name) -> a highlight-group name for coloring by file type.
int l_hl_for_file(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushstring(L, HlGroupForFilename(name).c_str());
    return 1;
}

// mep.fuzzy_score(str, query) -> score (-1 if no match), positions (1-indexed array).
int l_fuzzy_score(lua_State *L) {
    const char *str = luaL_checkstring(L, 1);
    const char *query = luaL_checkstring(L, 2);
    std::vector<int> positions;
    int score = FuzzyScore(str, query, &positions);
    lua_pushinteger(L, score);
    lua_newtable(L);
    for (size_t i = 0; i < positions.size(); i++) {
        lua_pushinteger(L, positions[i] + 1);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 2;
}

// Replaces Lua's default print() so `:lua print(...)` and init.lua output
// show up in the editor (status line) as well as on stdout.
int l_print(lua_State *L) {
    int n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) out += "\t";
        out.append(s, len);
        lua_pop(L, 1);
    }
    std::fprintf(stdout, "%s\n", out.c_str());
    Editor *ed = GetEditor(L);
    if (ed) ed->SetStatusMessage(out);
    return 0;
}

// --- In-pane 3D modeler (MODEL3D.md) -- unlike the raster image editor
// (which has no mep.* surface at all, only UI automation), the modeler
// gets a real Lua API so a scene can be built/queried/saved without any
// mouse/keyboard automation -- see MEP_AGENT_API.md's "in-pane 3D modeler"
// section. Every l_model_* function below is a thin wrapper around one
// Editor::Model3D* method, matching the rest of this file's convention;
// each has a matching agent_rpc.cpp `model.*` method (thin RPC wrapper
// around the same Editor:: method) and mcp_bridge.cpp `mep_model_*` tool.

// Reads an {x=,y=,z=} table field into *out, leaving *out untouched if the
// field is absent (any component defaults to 0 if the sub-table omits it).
// Shared by every l_model_* function taking a position/rotation/scale/
// target argument.
/**
 * @brief Reads an optional {x=,y=,z=} table field into a Vec3f.
 * @param L Lua state.
 * @param idx Stack index of the table containing the field.
 * @param name Field name to read.
 * @param out Receives the parsed vector; untouched if the field is absent.
 * @return True if the field was present (a table).
 */
bool ReadVec3Field(lua_State *L, int idx, const char *name, Vec3f *out) {
    lua_getfield(L, idx, name);
    bool present = lua_istable(L, -1);
    if (present) {
        lua_getfield(L, -1, "x");
        out->x = static_cast<float>(luaL_optnumber(L, -1, 0));
        lua_pop(L, 1);
        lua_getfield(L, -1, "y");
        out->y = static_cast<float>(luaL_optnumber(L, -1, 0));
        lua_pop(L, 1);
        lua_getfield(L, -1, "z");
        out->z = static_cast<float>(luaL_optnumber(L, -1, 0));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return present;
}

/**
 * @brief Pushes a Vec3f as an {x=,y=,z=} Lua table.
 * @param L Lua state.
 * @param v The vector to push.
 */
void PushVec3(lua_State *L, const Vec3f &v) {
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(v.x));
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, static_cast<lua_Number>(v.y));
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, static_cast<lua_Number>(v.z));
    lua_setfield(L, -2, "z");
}

/**
 * @brief Maps a primitive-kind name ("cube"/"sphere"/"cylinder"/"cone"/"plane"/"torus"/"wedge",
 * case-insensitive) to a PrimitiveKind.
 * @param name The primitive kind name.
 * @return The matching PrimitiveKind, or PrimitiveKind::None if unrecognized.
 */
PrimitiveKind ParsePrimitiveKindName(const std::string &name) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    if (s == "cube") return PrimitiveKind::Cube;
    if (s == "sphere") return PrimitiveKind::Sphere;
    if (s == "cylinder") return PrimitiveKind::Cylinder;
    if (s == "cone") return PrimitiveKind::Cone;
    if (s == "plane") return PrimitiveKind::Plane;
    if (s == "torus") return PrimitiveKind::Torus;
    if (s == "wedge") return PrimitiveKind::Wedge;
    return PrimitiveKind::None;
}

/**
 * @brief Pushes one Object3D as a Lua table (id, name, kind, visible, position/rotation/scale,
 * color, tri_count).
 * @param L Lua state.
 * @param scene The scene the object belongs to (for its mesh's triangle count).
 * @param obj The object to push.
 */
void PushObject3DTable(lua_State *L, const Scene &scene, const Object3D &obj) {
    lua_newtable(L);
    lua_pushinteger(L, obj.id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, obj.name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, obj.visible);
    lua_setfield(L, -2, "visible");
    PushVec3(L, obj.position);
    lua_setfield(L, -2, "position");
    PushVec3(L, obj.rotation_deg);
    lua_setfield(L, -2, "rotation");
    PushVec3(L, obj.scale);
    lua_setfield(L, -2, "scale");
    lua_newtable(L);
    lua_pushnumber(L, static_cast<lua_Number>(obj.color.r));
    lua_setfield(L, -2, "r");
    lua_pushnumber(L, static_cast<lua_Number>(obj.color.g));
    lua_setfield(L, -2, "g");
    lua_pushnumber(L, static_cast<lua_Number>(obj.color.b));
    lua_setfield(L, -2, "b");
    lua_pushnumber(L, static_cast<lua_Number>(obj.color.a));
    lua_setfield(L, -2, "a");
    lua_setfield(L, -2, "color");
    int tri_count = (obj.mesh_index >= 0 && obj.mesh_index < static_cast<int>(scene.meshes.size()))
                        ? scene.meshes[static_cast<size_t>(obj.mesh_index)].TriangleCount()
                        : 0;
    lua_pushinteger(L, tri_count);
    lua_setfield(L, -2, "tri_count");
}

// mep.model_new() -> buffer_id. Creates a fresh, empty 3D-modeler scene
// (no source file) and switches to it -- the "build from scratch" entry
// point, since every other way into Mode::Model3D goes through importing
// an existing file.
int l_model_new(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->NewModel3DScene());
    return 1;
}

// mep.model_list_objects(buffer_id) -> array of object tables (see PushObject3DTable).
int l_model_list_objects(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    const Model3DSession *sess = GetEditor(L)->GetModel3D(buffer_id);
    if (!sess) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    lua_newtable(L);
    int i = 1;
    for (const Object3D &obj : sess->scene.objects) {
        PushObject3DTable(L, sess->scene, obj);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// mep.model_scene_stats(buffer_id) -> {object_count=, triangle_count=}.
int l_model_scene_stats(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    const Model3DSession *sess = GetEditor(L)->GetModel3D(buffer_id);
    if (!sess) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(sess->scene.objects.size()));
    lua_setfield(L, -2, "object_count");
    lua_pushinteger(L, sess->scene.TotalTriangleCount());
    lua_setfield(L, -2, "triangle_count");
    return 1;
}

// mep.model_primitive_info([kind]) -> table. With no `kind`, returns
// {cube={pivot=,dimensions=}, sphere={...}, ...} for every primitive kind;
// with `kind`, returns just that one kind's {pivot=,dimensions=} table.
// No buffer_id -- this is static reference data, not scene state (MODEL3D.md
// Phase 1.5's "not queryable at runtime" gap, closed here).
int l_model_primitive_info(lua_State *L) {
    static const struct {
        const char *name;
        PrimitiveKind kind;
    } kAll[] = {{"cube", PrimitiveKind::Cube},     {"sphere", PrimitiveKind::Sphere}, {"cylinder", PrimitiveKind::Cylinder},
                {"cone", PrimitiveKind::Cone},      {"plane", PrimitiveKind::Plane},   {"torus", PrimitiveKind::Torus},
                {"wedge", PrimitiveKind::Wedge}};
    auto PushInfo = [L](PrimitiveKind kind) {
        std::string pivot, dimensions;
        DescribePrimitiveKind(kind, &pivot, &dimensions);
        lua_newtable(L);
        lua_pushstring(L, pivot.c_str());
        lua_setfield(L, -2, "pivot");
        lua_pushstring(L, dimensions.c_str());
        lua_setfield(L, -2, "dimensions");
    };
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        std::string kind_name = luaL_checkstring(L, 1);
        PrimitiveKind kind = ParsePrimitiveKindName(kind_name);
        if (kind == PrimitiveKind::None) return luaL_error(L, "unknown primitive kind: %s", kind_name.c_str());
        PushInfo(kind);
        return 1;
    }
    lua_newtable(L);
    for (const auto &entry : kAll) {
        PushInfo(entry.kind);
        lua_setfield(L, -2, entry.name);
    }
    return 1;
}

// mep.model_add_primitive(buffer_id, kind[, transform]) -> object_id.
// `transform`, if given, is a {position=, rotation=, scale=} table applied
// right after creation (each sub-field optional, {x=,y=,z=} tables).
int l_model_add_primitive(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    std::string kind_name = luaL_checkstring(L, 2);
    PrimitiveKind kind = ParsePrimitiveKindName(kind_name);
    if (kind == PrimitiveKind::None) return luaL_error(L, "unknown primitive kind: %s", kind_name.c_str());
    int id = GetEditor(L)->Model3DAddPrimitive(buffer_id, kind);
    if (id < 0) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    if (lua_istable(L, 3)) {
        Vec3f position, rotation, scale{1, 1, 1};
        bool has_position = ReadVec3Field(L, 3, "position", &position);
        bool has_rotation = ReadVec3Field(L, 3, "rotation", &rotation);
        bool has_scale = ReadVec3Field(L, 3, "scale", &scale);
        if (has_position || has_rotation || has_scale) {
            GetEditor(L)->Model3DSetTransform(buffer_id, id, has_position, position, has_rotation, rotation, has_scale, scale);
        }
    }
    lua_pushinteger(L, id);
    return 1;
}

// mep.model_delete_object(buffer_id, object_id) -> bool.
// mep.model_delete_object(buffer_id, object_id, cascade?) -> bool. cascade (default false) also
// deletes every transitive descendant instead of just un-parenting them.
int l_model_delete_object(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    bool cascade = lua_toboolean(L, 3) != 0;
    lua_pushboolean(L, GetEditor(L)->Model3DDeleteObject(buffer_id, object_id, cascade));
    return 1;
}

// mep.model_duplicate_object(buffer_id, object_id, cascade?) -> new_object_id (-1 on failure). cascade
// (default false) also duplicates every transitive descendant, re-parented to mirror the original
// hierarchy under the new copy.
int l_model_duplicate_object(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    bool cascade = lua_toboolean(L, 3) != 0;
    lua_pushinteger(L, GetEditor(L)->Model3DDuplicateObject(buffer_id, object_id, cascade));
    return 1;
}

// mep.model_set_transform(buffer_id, object_id, {position=, rotation=, scale=}) -> bool.
// Each of position/rotation/scale is optional -- only the ones given are
// applied (see Editor::Model3DSetTransform's own has_* flags).
int l_model_set_transform(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    Vec3f position, rotation, scale;
    bool has_position = ReadVec3Field(L, 3, "position", &position);
    bool has_rotation = ReadVec3Field(L, 3, "rotation", &rotation);
    bool has_scale = ReadVec3Field(L, 3, "scale", &scale);
    lua_pushboolean(
        L, GetEditor(L)->Model3DSetTransform(buffer_id, object_id, has_position, position, has_rotation, rotation, has_scale, scale));
    return 1;
}

// mep.model_set_material(buffer_id, object_id, {r=, g=, b=, a=}) -> bool. r/g/b/a are 0..1 floats;
// a defaults to 1.0 if omitted.
int l_model_set_material(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    RgbaColorF color;
    lua_getfield(L, 3, "r");
    color.r = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 3, "g");
    color.g = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 3, "b");
    color.b = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 3, "a");
    color.a = static_cast<float>(luaL_optnumber(L, -1, 1.0));
    lua_pop(L, 1);
    lua_pushboolean(L, GetEditor(L)->Model3DSetMaterial(buffer_id, object_id, color));
    return 1;
}

// mep.model_set_texture(buffer_id, object_id, path) -> bool. `path` is an image file
// (PNG/JPG/BMP/...); pass "" to clear the texture (falls back to flat color). The texture is
// sampled and then tinted by the object's own color, same as glTF's baseColorTexture +
// baseColorFactor.
int l_model_set_texture(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    const char *path = luaL_checkstring(L, 3);
    lua_pushboolean(L, GetEditor(L)->Model3DSetTexture(buffer_id, object_id, path));
    return 1;
}

// mep.model_rename_object(buffer_id, object_id, name) -> bool.
int l_model_rename_object(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    const char *name = luaL_checkstring(L, 3);
    lua_pushboolean(L, GetEditor(L)->Model3DRenameObject(buffer_id, object_id, name));
    return 1;
}

// mep.model_set_visible(buffer_id, object_id, visible) -> bool.
int l_model_set_visible(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    bool visible = lua_toboolean(L, 3);
    lua_pushboolean(L, GetEditor(L)->Model3DSetVisible(buffer_id, object_id, visible));
    return 1;
}

// mep.model_select(buffer_id, {object_id, ...}) -- replaces the current selection, silently
// dropping any id that doesn't exist.
int l_model_select(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<int> ids;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        ids.push_back(static_cast<int>(luaL_optinteger(L, -1, -1)));
        lua_pop(L, 1);
    }
    GetEditor(L)->Model3DSetSelection(buffer_id, ids);
    return 0;
}

// mep.model_get_selection(buffer_id) -> array of currently selected object ids.
int l_model_get_selection(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    const Model3DSession *sess = GetEditor(L)->GetModel3D(buffer_id);
    if (!sess) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    lua_newtable(L);
    for (size_t i = 0; i < sess->selection.size(); i++) {
        lua_pushinteger(L, sess->selection[i]);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.model_camera_set(buffer_id, {target=, yaw=, pitch=, distance=, fov=}) -- each field
// optional, only given ones are applied.
int l_model_camera_set(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    Model3DCameraParams params;
    params.has_target = ReadVec3Field(L, 2, "target", &params.target);
    lua_getfield(L, 2, "yaw");
    params.has_yaw = !lua_isnil(L, -1);
    params.yaw = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 2, "pitch");
    params.has_pitch = !lua_isnil(L, -1);
    params.pitch = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 2, "distance");
    params.has_distance = !lua_isnil(L, -1);
    params.distance = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 2, "fov");
    params.has_fov = !lua_isnil(L, -1);
    params.fov = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    GetEditor(L)->Model3DSetCamera(buffer_id, params);
    return 0;
}

// mep.model_camera_get(buffer_id) -> {target=, yaw=, pitch=, distance=, fov=}.
int l_model_camera_get(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    const Model3DSession *sess = GetEditor(L)->GetModel3D(buffer_id);
    if (!sess) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    lua_newtable(L);
    PushVec3(L, sess->camera_target);
    lua_setfield(L, -2, "target");
    lua_pushnumber(L, static_cast<lua_Number>(sess->camera_yaw));
    lua_setfield(L, -2, "yaw");
    lua_pushnumber(L, static_cast<lua_Number>(sess->camera_pitch));
    lua_setfield(L, -2, "pitch");
    lua_pushnumber(L, static_cast<lua_Number>(sess->camera_distance));
    lua_setfield(L, -2, "distance");
    lua_pushnumber(L, static_cast<lua_Number>(sess->camera_fov));
    lua_setfield(L, -2, "fov");
    return 1;
}

// mep.model_undo(buffer_id) / mep.model_redo(buffer_id).
int l_model_undo(lua_State *L) {
    GetEditor(L)->UndoModel3D(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}
int l_model_redo(lua_State *L) {
    GetEditor(L)->RedoModel3D(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

// mep.model_set_view(buffer_id, {show_grid=, wireframe=, snap=, show_textures=, unlit=}) -- each
// field optional, not undoable.
int l_model_set_view(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "show_grid");
    bool has_show_grid = !lua_isnil(L, -1);
    bool show_grid = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 2, "wireframe");
    bool has_wireframe = !lua_isnil(L, -1);
    bool wireframe = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 2, "snap");
    bool has_snap = !lua_isnil(L, -1);
    bool snap = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 2, "show_textures");
    bool has_show_textures = !lua_isnil(L, -1);
    bool show_textures = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 2, "unlit");
    bool has_unlit = !lua_isnil(L, -1);
    bool unlit = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    GetEditor(L)->Model3DSetView(buffer_id, has_show_grid, show_grid, has_wireframe, wireframe, has_snap, snap,
                                  has_show_textures, show_textures, has_unlit, unlit);
    return 0;
}

// mep.model_frame_all(buffer_id) -- reframes the orbit camera to fit the whole scene.
int l_model_frame_all(lua_State *L) {
    GetEditor(L)->Model3DFrameAll(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

// mep.model_set_transforms(buffer_id, {{object_id=, position=, rotation=, scale=}, ...}) -> count
// actually applied. One round-trip for many objects, added after live dogfooding found repositioning
// an 11-object scene one call per object too slow.
int l_model_set_transforms(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<Model3DTransformUpdate> updates;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        int idx = lua_gettop(L);
        Model3DTransformUpdate u;
        lua_getfield(L, idx, "object_id");
        u.object_id = static_cast<int>(luaL_optinteger(L, -1, -1));
        lua_pop(L, 1);
        u.has_position = ReadVec3Field(L, idx, "position", &u.position);
        u.has_rotation = ReadVec3Field(L, idx, "rotation", &u.rotation_deg);
        u.has_scale = ReadVec3Field(L, idx, "scale", &u.scale);
        updates.push_back(u);
        lua_pop(L, 1);
    }
    lua_pushinteger(L, GetEditor(L)->Model3DSetTransformsBatch(buffer_id, updates));
    return 1;
}

// mep.model_set_materials(buffer_id, {{object_id=, r=, g=, b=, a=}, ...}) -> count actually applied.
// Same batching motivation as mep.model_set_transforms above.
int l_model_set_materials(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<Model3DMaterialUpdate> updates;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        int idx = lua_gettop(L);
        Model3DMaterialUpdate u;
        lua_getfield(L, idx, "object_id");
        u.object_id = static_cast<int>(luaL_optinteger(L, -1, -1));
        lua_pop(L, 1);
        lua_getfield(L, idx, "r");
        u.color.r = static_cast<float>(luaL_optnumber(L, -1, 0));
        lua_pop(L, 1);
        lua_getfield(L, idx, "g");
        u.color.g = static_cast<float>(luaL_optnumber(L, -1, 0));
        lua_pop(L, 1);
        lua_getfield(L, idx, "b");
        u.color.b = static_cast<float>(luaL_optnumber(L, -1, 0));
        lua_pop(L, 1);
        lua_getfield(L, idx, "a");
        u.color.a = static_cast<float>(luaL_optnumber(L, -1, 1.0));
        lua_pop(L, 1);
        updates.push_back(u);
        lua_pop(L, 1);
    }
    lua_pushinteger(L, GetEditor(L)->Model3DSetMaterialsBatch(buffer_id, updates));
    return 1;
}

// mep.model_delete_objects(buffer_id, {object_id, ...}, cascade?) -> count actually deleted.
int l_model_delete_objects(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<int> ids;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        ids.push_back(static_cast<int>(luaL_optinteger(L, -1, -1)));
        lua_pop(L, 1);
    }
    bool cascade = lua_toboolean(L, 3) != 0;
    lua_pushinteger(L, GetEditor(L)->Model3DDeleteObjectsBatch(buffer_id, ids, cascade));
    return 1;
}

// mep.model_duplicate_mirrored(buffer_id, object_id, axis) -> new_object_id (-1 on failure). axis is
// "x"/"y"/"z" -- mirrors position across that axis through the origin, and reflects the copy's own
// rotation to match (exact for a simple single-axis rotation, see the .cpp for the general caveat).
int l_model_duplicate_mirrored(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    const char *axis = luaL_checkstring(L, 3);
    lua_pushinteger(L, GetEditor(L)->Model3DDuplicateMirrored(buffer_id, object_id, axis[0]));
    return 1;
}

// mep.model_radial_array(buffer_id, object_id, count, axis) -> array of the `count - 1` new object
// ids (the original is left as-is and not included). Evenly arrays copies around `axis` through the
// origin -- e.g. a fin offset from the origin on X, arrayed 4x around Y, lands one at each 90-degree
// step, each still facing outward the way the original did.
int l_model_radial_array(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    int count = static_cast<int>(luaL_checkinteger(L, 3));
    const char *axis = luaL_checkstring(L, 4);
    std::vector<int> ids = GetEditor(L)->Model3DRadialArray(buffer_id, object_id, count, axis[0]);
    lua_newtable(L);
    for (size_t i = 0; i < ids.size(); i++) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// mep.model_group_objects(buffer_id, object_ids) -> new group_id (-1 on failure). Creates an empty
// group node (no mesh, invisible in the viewport) at the centroid of object_ids and parents each of
// them under it -- Object3D::parent is an organizational/group-move link only, never composed into a
// child's own transform, so this doesn't move or change how anything renders.
int l_model_group_objects(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<int> ids;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        ids.push_back(static_cast<int>(luaL_optinteger(L, -1, -1)));
        lua_pop(L, 1);
    }
    lua_pushinteger(L, GetEditor(L)->Model3DGroupObjects(buffer_id, ids));
    return 1;
}

// mep.model_set_parent(buffer_id, object_id, parent_id) -> bool. parent_id may be -1/nil to clear
// (un-parent). Fails (returns false) on a nonexistent object/parent, parent_id == object_id, or a
// parent_id that's already a descendant of object_id (would create a cycle).
int l_model_set_parent(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    int parent_id = lua_isnoneornil(L, 3) ? -1 : static_cast<int>(luaL_checkinteger(L, 3));
    lua_pushboolean(L, GetEditor(L)->Model3DSetParent(buffer_id, object_id, parent_id));
    return 1;
}

// mep.model_list_vertices(buffer_id, object_id) -> array of {index=, x=, y=, z=} (local mesh space,
// pre-object-transform). Empty for an object with no mesh (an empty group node).
int l_model_list_vertices(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    const Model3DSession *sess = GetEditor(L)->GetModel3D(buffer_id);
    if (!sess) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    const Object3D *obj = sess->scene.FindObject(object_id);
    if (!obj) return luaL_error(L, "no such object: %d", object_id);
    lua_newtable(L);
    if (obj->mesh_index >= 0 && obj->mesh_index < static_cast<int>(sess->scene.meshes.size())) {
        const MeshData &md = sess->scene.meshes[static_cast<size_t>(obj->mesh_index)];
        bool has_normals = !md.normals.empty();
        for (int v = 0; v < md.VertexCount(); v++) {
            lua_newtable(L);
            lua_pushinteger(L, v);
            lua_setfield(L, -2, "index");
            lua_pushnumber(L, static_cast<lua_Number>(md.positions[static_cast<size_t>(v) * 3 + 0]));
            lua_setfield(L, -2, "x");
            lua_pushnumber(L, static_cast<lua_Number>(md.positions[static_cast<size_t>(v) * 3 + 1]));
            lua_setfield(L, -2, "y");
            lua_pushnumber(L, static_cast<lua_Number>(md.positions[static_cast<size_t>(v) * 3 + 2]));
            lua_setfield(L, -2, "z");
            if (has_normals) {
                lua_pushnumber(L, static_cast<lua_Number>(md.normals[static_cast<size_t>(v) * 3 + 0]));
                lua_setfield(L, -2, "nx");
                lua_pushnumber(L, static_cast<lua_Number>(md.normals[static_cast<size_t>(v) * 3 + 1]));
                lua_setfield(L, -2, "ny");
                lua_pushnumber(L, static_cast<lua_Number>(md.normals[static_cast<size_t>(v) * 3 + 2]));
                lua_setfield(L, -2, "nz");
            }
            lua_rawseti(L, -2, v + 1);
        }
    }
    return 1;
}

// mep.model_list_triangles(buffer_id, object_id) -> array of {index, a, b, c} (vertex-unit indices of
// each triangle's 3 corners). Read-only mesh-connectivity introspection -- without this, an agent has
// no way to discover which vertices actually form a triangle together (the thing subdivide/extrude/
// inset/make_face all key off of) besides the positions alone.
int l_model_list_triangles(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    const Model3DSession *sess = GetEditor(L)->GetModel3D(buffer_id);
    if (!sess) return luaL_error(L, "not a 3D-modeler buffer: %d", buffer_id);
    const Object3D *obj = sess->scene.FindObject(object_id);
    if (!obj) return luaL_error(L, "no such object: %d", object_id);
    lua_newtable(L);
    if (obj->mesh_index >= 0 && obj->mesh_index < static_cast<int>(sess->scene.meshes.size())) {
        const MeshData &md = sess->scene.meshes[static_cast<size_t>(obj->mesh_index)];
        for (int t = 0; t < md.TriangleCount(); t++) {
            lua_newtable(L);
            lua_pushinteger(L, t);
            lua_setfield(L, -2, "index");
            lua_pushinteger(L, md.indices[static_cast<size_t>(t) * 3 + 0]);
            lua_setfield(L, -2, "a");
            lua_pushinteger(L, md.indices[static_cast<size_t>(t) * 3 + 1]);
            lua_setfield(L, -2, "b");
            lua_pushinteger(L, md.indices[static_cast<size_t>(t) * 3 + 2]);
            lua_setfield(L, -2, "c");
            lua_rawseti(L, -2, t + 1);
        }
    }
    return 1;
}

// mep.model_set_vertex_position(buffer_id, object_id, vertex_index, {x=, y=, z=}) -> bool. Position
// is local mesh space (pre-object-transform). Transparently gives the object its own private mesh
// copy first if it currently shares one with another object (see Scene::EnsureUniqueMesh).
int l_model_set_vertex_position(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    int vertex_index = static_cast<int>(luaL_checkinteger(L, 3));
    luaL_checktype(L, 4, LUA_TTABLE);
    Vec3f pos;
    lua_getfield(L, 4, "x");
    pos.x = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 4, "y");
    pos.y = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 4, "z");
    pos.z = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_pushboolean(L, GetEditor(L)->Model3DSetVertexPosition(buffer_id, object_id, vertex_index, pos));
    return 1;
}

// mep.model_delete_vertices(buffer_id, object_id, vertex_indices) -> bool. Deletes the given
// vertices (vertex-units indices) and every triangle referencing any of them, leaving a hole rather
// than retriangulating. Safely clones a shared mesh first, same as mep.model_set_vertex_position.
int l_model_delete_vertices(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<int> vertex_indices;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 3));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        vertex_indices.push_back(static_cast<int>(luaL_optinteger(L, -1, -1)));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, GetEditor(L)->Model3DDeleteVertices(buffer_id, object_id, vertex_indices));
    return 1;
}

// mep.model_merge_vertices(buffer_id, object_id, vertex_indices) -> bool. Welds the given vertices
// (vertex-units indices) into a single vertex at their averaged position/normal/texcoord, dropping any
// triangle that becomes degenerate as a result. Safely clones a shared mesh first, same as
// mep.model_set_vertex_position. Fewer than 2 distinct valid indices is a harmless no-op.
int l_model_merge_vertices(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<int> vertex_indices;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 3));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        vertex_indices.push_back(static_cast<int>(luaL_optinteger(L, -1, -1)));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, GetEditor(L)->Model3DMergeVertices(buffer_id, object_id, vertex_indices));
    return 1;
}

// mep.model_recalculate_normals(buffer_id, object_id) -> bool. Recomputes an object's mesh's per-vertex
// normals from its current triangle geometry (smooth/averaged, see MeshData::RecalculateNormals). Has
// zero visible effect on this app's own rendering (its shaders never read normals) -- fixes up normals
// left stale by vertex edits for tools that do read them on export, e.g. Blender.
int l_model_recalculate_normals(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, GetEditor(L)->Model3DRecalculateNormals(buffer_id, object_id));
    return 1;
}

namespace {
std::vector<int> ReadIntArray(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TTABLE);
    std::vector<int> out;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, index));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, index, i);
        out.push_back(static_cast<int>(luaL_optinteger(L, -1, -1)));
        lua_pop(L, 1);
    }
    return out;
}

void PushIntArray(lua_State *L, const std::vector<int> &values) {
    lua_newtable(L);
    for (size_t i = 0; i < values.size(); i++) {
        lua_pushinteger(L, values[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}
}  // namespace

// mep.model_subdivide_faces(buffer_id, object_id, vertex_indices) -> table of new centroid vertex
// indices (empty if nothing was subdivided). Centroid-subdivides every triangle whose all 3 corners are
// in vertex_indices -- this app's stand-in for real face-selection tooling, same as merge/extrude.
int l_model_subdivide_faces(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<int> vertex_indices = ReadIntArray(L, 3);
    PushIntArray(L, GetEditor(L)->Model3DSubdivideFaces(buffer_id, object_id, vertex_indices));
    return 1;
}

// mep.model_extrude_faces(buffer_id, object_id, vertex_indices, distance) -> table of new "cap" vertex
// indices (empty on no-op/failure). Extrudes the face formed by every triangle whose all 3 corners are
// in vertex_indices, along that face's own geometrically-derived normal.
int l_model_extrude_faces(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<int> vertex_indices = ReadIntArray(L, 3);
    float distance = static_cast<float>(luaL_checknumber(L, 4));
    PushIntArray(L, GetEditor(L)->Model3DExtrudeFaces(buffer_id, object_id, vertex_indices, distance));
    return 1;
}

// mep.model_dissolve_vertex(buffer_id, object_id, vertex_index) -> bool. Removes one vertex and patches
// the surrounding faces back together where possible (falls back to a plain hole-leaving removal
// otherwise -- see Editor::Model3DDissolveVertex).
int l_model_dissolve_vertex(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    int vertex_index = static_cast<int>(luaL_checkinteger(L, 3));
    lua_pushboolean(L, GetEditor(L)->Model3DDissolveVertex(buffer_id, object_id, vertex_index));
    return 1;
}

// mep.model_inset_faces(buffer_id, object_id, vertex_indices, amount) -> table of new cap vertex
// indices (empty on no-op/failure). Insets the face formed by every triangle whose all 3 corners are in
// vertex_indices, moving each duplicate toward the face group's own centroid by amount (0..1, clamped).
int l_model_inset_faces(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<int> vertex_indices = ReadIntArray(L, 3);
    float amount = static_cast<float>(luaL_checknumber(L, 4));
    PushIntArray(L, GetEditor(L)->Model3DInsetFaces(buffer_id, object_id, vertex_indices, amount));
    return 1;
}

// mep.model_add_vertex(buffer_id, object_id, {x,y,z}) -> vertex_index (-1 on failure). Appends one new,
// isolated vertex -- no triangle references it until connected via mep.model_make_face or similar.
int l_model_add_vertex(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    Vec3f pos;
    lua_getfield(L, 3, "x");
    pos.x = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 3, "y");
    pos.y = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_getfield(L, 3, "z");
    pos.z = static_cast<float>(luaL_optnumber(L, -1, 0));
    lua_pop(L, 1);
    lua_pushinteger(L, GetEditor(L)->Model3DAddVertex(buffer_id, object_id, pos));
    return 1;
}

// mep.model_make_face(buffer_id, object_id, vertex_indices) -> bool. Connects existing vertices into
// new triangle(s), fan-triangulated from the first one -- Blender's own "Make Edge/Face" in spirit.
int l_model_make_face(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<int> vertex_indices = ReadIntArray(L, 3);
    lua_pushboolean(L, GetEditor(L)->Model3DMakeFace(buffer_id, object_id, vertex_indices));
    return 1;
}

// mep.model_merge_by_distance(buffer_id, object_id, threshold) -> removed_count. Automatically welds
// every group of vertices whose positions are all mutually within threshold of each other -- Blender's
// own "Merge by Distance"/"Remove Doubles". threshold=0 welds only exact position duplicates.
int l_model_merge_by_distance(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    float threshold = static_cast<float>(luaL_checknumber(L, 3));
    lua_pushinteger(L, GetEditor(L)->Model3DMergeByDistance(buffer_id, object_id, threshold));
    return 1;
}

// mep.model_flip_normals(buffer_id, object_id) -> bool. Reverses every triangle's winding and negates
// every vertex normal -- the fix for geometry that renders inside-out.
int l_model_flip_normals(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int object_id = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, GetEditor(L)->Model3DFlipNormals(buffer_id, object_id));
    return 1;
}

const luaL_Reg kMepFuncs[] = {
    {"get_line", l_get_line},
    {"set_line", l_set_line},
    {"replace_lines", l_replace_lines},
    {"line_count", l_line_count},
    {"visual_selection", l_visual_selection},
    {"spell_ready", l_spell_ready},
    {"spell_bad", l_spell_bad},
    {"spell_suggest", l_spell_suggest},
    {"spell_add", l_spell_add},
    {"spell_wrong", l_spell_wrong},
    {"spell_enabled", l_spell_enabled},
    {"spell_fix_selection", l_spell_fix_selection},
    {"spell_fix_word", l_spell_fix_word},
    {"cursor", l_cursor},
    {"set_cursor", l_set_cursor},
    {"current_buffer", l_current_buffer},
    {"participant_set", l_participant_set},
    {"participant_clear", l_participant_clear},
    {"participants", l_participants},
    {"visual_change", l_visual_change},
    {"enter_normal", l_enter_normal},
    {"enter_insert", l_enter_insert},
    {"is_insert_mode", l_is_insert_mode},
    {"stt_set_recording", l_stt_set_recording},
    {"insert_text", l_insert_text},
    {"clipboard_get", l_clipboard_get},
    {"clipboard_set", l_clipboard_set},
    {"notify", l_notify},
    {"command", l_command},
    {"map", l_map},
    {"mapping_descriptions", l_mapping_descriptions},
    {"leader_bindings", l_leader_bindings},
    {"map_mod1", l_map_mod1},
    {"map_g", l_map_g},
    {"map_g_visual", l_map_g_visual},
    {"map_bracket_prev", l_map_bracket_prev},
    {"map_bracket_next", l_map_bracket_next},
    {"set_mod1", l_set_mod1},
    {"nav_pane", l_nav_pane},
    {"focus_top_left_pane", l_focus_top_left_pane},
    {"resize_pane", l_resize_pane},
    {"pane_set_share", l_pane_set_share},
    {"current_tab_id", l_current_tab_id},
    {"current_pane_id", l_current_pane_id},
    {"pane_focus", l_pane_focus},
    {"pane_split_bottom", l_pane_split_bottom},
    {"pane_split_left", l_pane_split_left},
    {"pane_split_right", l_pane_split_right},
    {"vsplit_right", l_vsplit_right},
    {"split_below", l_split_below},
    {"cmd", l_cmd},
    {"open", l_open},
    {"pick_pane_open", l_pick_pane_open},
    {"terminal_here", l_terminal_here},
    {"terminal_here_argv", l_terminal_here_argv},
    {"is_terminal_buffer", l_is_terminal_buffer},
    {"terminal_write", l_terminal_write},
    {"sidebar_default_cols", l_sidebar_default_cols},
    {"quit", l_quit},
    {"job_start", l_job_start},
    {"notebook_is_buffer", l_notebook_is_buffer},
    {"notebook_run_cell", l_notebook_run_cell},
    {"notebook_run_all", l_notebook_run_all},
    {"notebook_run_and_advance", l_notebook_run_and_advance},
    {"notebook_run_and_insert", l_notebook_run_and_insert},
    {"notebook_interrupt", l_notebook_interrupt},
    {"notebook_restart_kernel", l_notebook_restart_kernel},
    {"notebook_clear_outputs", l_notebook_clear_outputs},
    {"notebook_insert_cell", l_notebook_insert_cell},
    {"notebook_delete_cell", l_notebook_delete_cell},
    {"notebook_set_cell_type", l_notebook_set_cell_type},
    {"notebook_move_cell", l_notebook_move_cell},
    {"notebook_cell_index", l_notebook_cell_index},
    {"notebook_cell_count", l_notebook_cell_count},
    {"notebook_goto_cell", l_notebook_goto_cell},
    {"notebook_set_python", l_notebook_set_python},
    {"notebook_set_kernels", l_notebook_set_kernels},
    {"notebook_kernels", l_notebook_kernels},
    {"notebook_cell_kernel", l_notebook_cell_kernel},
    {"notebook_set_cell_kernel", l_notebook_set_cell_kernel},
    {"notebook_default_kernel", l_notebook_default_kernel},
    {"notebook_cell_language", l_notebook_cell_language},
    {"notebook_lsp_context", l_notebook_lsp_context},
    {"notebook_status", l_notebook_status},
    {"notebook_cell_outputs", l_notebook_cell_outputs},
    {"job_write", l_job_write},
    {"job_close_stdin", l_job_close_stdin},
    {"job_kill", l_job_kill},
    {"job_is_running", l_job_is_running},
    {"ui_input", l_ui_input},
    {"ui_confirm", l_ui_confirm},
    {"ui_select", l_ui_select},
    {"float_preview", l_float_preview},
    {"float_open", l_float_open},
    {"float_close", l_float_close},
    {"float_is_open", l_float_is_open},
    {"hover_show", l_hover_show},
    {"hover_close", l_hover_close},
    {"hover_is_open", l_hover_is_open},
    {"hover_focus_enter", l_hover_focus_enter},
    {"ns_create", l_ns_create},
    {"ns_clear", l_ns_clear},
    {"deco_add", l_deco_add},
    {"ts_captures", l_ts_captures},
    {"ts_fold_ranges", l_ts_fold_ranges},
    {"ts_structure", l_ts_structure},
    {"todo_scan_matches", l_todo_scan_matches},
    {"dap_toggle_breakpoint", l_dap_toggle_breakpoint},
    {"dap_breakpoint_lines", l_dap_breakpoint_lines},
    {"termsend_register", l_termsend_register},
    {"termsend_target", l_termsend_target},
    {"termsend_unregister", l_termsend_unregister},
    {"termsend_candidates", l_termsend_candidates},
    {"activity_todo_load", l_activity_todo_load},
    {"activity_todo_save", l_activity_todo_save},
    {"activity_todo_clock_status", l_activity_todo_clock_status},
    {"activity_todo_clock_start", l_activity_todo_clock_start},
    {"activity_todo_clock_stop", l_activity_todo_clock_stop},
    {"activity_todo_retitle", l_activity_todo_retitle},
    {"activity_todo_archive", l_activity_todo_archive},
    {"activity_todo_move", l_activity_todo_move},
    {"active_todo_set", l_active_todo_set},
    {"setenv", l_setenv},
    {"unsetenv", l_unsetenv},
    {"direnv_set_active", l_direnv_set_active},
    {"activity_test_failure_lines", l_activity_test_failure_lines},
    {"syntax_highlight_fallback", l_syntax_highlight_fallback},
    {"org_highlight_emphasis", l_org_highlight_emphasis},
    {"md_toggle_checkbox", l_md_toggle_checkbox},
    {"md_fold", l_md_fold},
    {"md_table_align", l_md_table_align},
    {"md_table_insert_row", l_md_table_insert_row},
    {"md_table_insert_col", l_md_table_insert_col},
    {"md_conceal_scan", l_md_conceal_scan},
    {"completion_scan_buffer_words", l_completion_scan_buffer_words},
    {"completion_path_prefix", l_completion_path_prefix},
    {"completion_rank", l_completion_rank},
    {"snippet_splice", l_snippet_splice},
    {"snippet_jump", l_snippet_jump},
    {"docs_signature_info", l_docs_signature_info},
    {"picker_preview_file", l_picker_preview_file},
    {"tree_build_rows", l_tree_build_rows},
    {"project_readme_path", l_project_readme_path},
    {"colorize", l_colorize},
    {"url_under_cursor", l_url_under_cursor},
    {"list_urls_scan", l_list_urls_scan},
    {"ansi_render", l_ansi_render},
    {"leetcode_html_to_text", l_leetcode_html_to_text},
    {"git_gutter_refresh_native", l_git_gutter_refresh_native},
    {"git_next_hunk_row", l_git_next_hunk_row},
    {"git_prev_hunk_row", l_git_prev_hunk_row},
    {"git_preview_hunk_text", l_git_preview_hunk_text},
    {"git_reset_hunk_native", l_git_reset_hunk_native},
    {"git_stage_hunk", l_git_stage_hunk},
    {"ts_apply_captures", l_ts_apply_captures},
    {"buffer_set_lines", l_buffer_set_lines},
    {"buffer_delete", l_buffer_delete},
    {"buffer_set_drag_resolver", l_buffer_set_drag_resolver},
    {"buffer_ns_clear", l_buffer_ns_clear},
    {"buffer_deco_add", l_buffer_deco_add},
    {"term_start", l_term_start},
    {"term_resize", l_term_resize},
    {"buffer_new", l_buffer_new},
    {"buffer_set_on_enter", l_buffer_set_on_enter},
    {"buffer_set_on_write", l_buffer_set_on_write},
    {"buffer_set_on_image_toggle", l_buffer_set_on_image_toggle},
    {"buffer_set_on_key", l_buffer_set_on_key},
    {"buffer_get_lines", l_buffer_get_lines},
    {"buffer_set_filename", l_buffer_set_filename},
    {"buffer_set_hide_line_numbers", l_buffer_set_hide_line_numbers},
    {"buffer_set_wrap", l_buffer_set_wrap},
    {"buffer_modified", l_buffer_modified},
    {"fold_create", l_fold_create},
    {"fold_clear_provider", l_fold_clear_provider},
    {"fold_toggle", l_fold_toggle},
    {"buf_set_image_row", l_buf_set_image_row},
    {"buf_clear_image_rows", l_buf_clear_image_rows},
    {"org_images_toggle", l_org_images_toggle},
    {"org_image_invalidate", l_org_image_invalidate},
    {"org_image_scan", l_org_image_scan},
    {"org_agenda_expand_glob", l_org_agenda_expand_glob},
    {"org_agenda_scan_lines", l_org_agenda_scan_lines},
    {"org_capture_expand_template", l_org_expand_capture_template},
    {"org_refile_move", l_org_refile_move},
    {"org_latex_scan_fragments", l_org_latex_scan_fragments},
    {"org_bib_parse_files", l_org_bib_parse_files},
    {"org_roam_files_in", l_org_roam_files_in},
    {"org_roam_title_of", l_org_roam_title_of},
    {"org_roam_ensure_id", l_org_roam_ensure_id},
    {"org_roam_find_backlink_lines", l_org_roam_find_backlink_lines},
    {"org_roam_parse_file_index", l_org_roam_parse_file_index},
    {"org_roam_slugify", l_org_roam_slugify},
    {"org_table_align", l_org_table_align},
    {"org_link_at_cursor", l_org_link_at_cursor},
    {"org_timestamp_insert", l_org_timestamp_insert},
    {"org_timestamp_shift", l_org_timestamp_shift},
    {"org_timestamp_at", l_org_timestamp_at},
    {"org_footnote_jump", l_org_footnote_jump},
    {"org_set_planning", l_org_set_planning},
    {"org_export_heading", l_org_export_heading},
    {"org_html_escape", l_org_html_escape},
    {"org_subtree_end_lines", l_org_subtree_end_lines},
    {"org_collect_macros", l_org_collect_macros},
    {"org_expand_macro_line", l_org_expand_macro_line},
    {"org_babel_format_literal", l_org_babel_format_literal},
    {"org_parse_vars", l_org_parse_vars},
    {"org_parse_results", l_org_parse_results},
    {"lsp_word_at_cursor", l_lsp_word_at_cursor},
    {"lsp_apply_text_edit", l_lsp_apply_text_edit},
    {"lsp_apply_edits_current_buffer", l_lsp_apply_edits_current_buffer},
    {"lsp_diag_wrap", l_lsp_diag_wrap},
    {"org_clock_in", l_org_clock_in},
    {"org_clock_out", l_org_clock_out},
    {"org_clock_table_items", l_org_clock_table_items},
    {"org_property_get", l_org_property_get},
    {"org_property_set", l_org_property_set},
    {"org_property_remove", l_org_property_remove},
    {"org_drill_grade", l_org_drill_grade},
    {"buf_set_latex_row", l_buf_set_latex_row},
    {"buf_clear_latex_rows", l_buf_clear_latex_rows},
    {"org_latex_toggle", l_org_latex_toggle},
    {"org_latex_visible", l_org_latex_visible},
    {"buf_add_latex_inline", l_buf_add_latex_inline},
    {"buf_clear_latex_inline", l_buf_clear_latex_inline},
    {"font_size", l_font_size},
    {"image_size", l_image_size},
    {"image_set_nav", l_image_set_nav},
    {"image_set_return", l_image_set_return},
    {"image_set_theme", l_image_set_theme},
    {"image_get_theme", l_image_get_theme},
    {"sidebar_create", l_sidebar_create},
    {"sidebar_set_sections", l_sidebar_set_sections},
    {"sidebar_open", l_sidebar_open},
    {"sidebar_open_pane", l_sidebar_open_pane},
    {"sidebar_close", l_sidebar_close},
    {"sidebar_toggle", l_sidebar_toggle},
    {"sidebar_is_open", l_sidebar_is_open},
    {"sidebar_set_on_key", l_sidebar_set_on_key},
    {"sidebar_set_tabs", l_sidebar_set_tabs},
    {"sidebar_set_on_tab", l_sidebar_set_on_tab},
    {"sidebar_set_active_tab", l_sidebar_set_active_tab},
    {"sidebar_active_tab", l_sidebar_active_tab},
    {"sidebar_set_on_preview", l_sidebar_set_on_preview},
    {"sidebar_set_preview", l_sidebar_set_preview},
    {"sidebar_popout_toggle", l_sidebar_popout_toggle},
    {"notify_sidebar_id", l_notify_sidebar_id},
    {"notify_refresh_pane", l_notify_refresh_pane},
    {"sidebar_popout_close", l_sidebar_popout_close},
    {"sidebar_is_popout", l_sidebar_is_popout},
    {"read_lines", l_read_lines},
    {"sidebar_cursor", l_sidebar_cursor},
    {"sidebar_cursor_widget_id", l_sidebar_cursor_widget_id},
    {"sidebar_is_focused", l_sidebar_is_focused},
    {"sidebar_focus_row", l_sidebar_focus_row},
    {"picker_open", l_picker_open},
    {"picker_set_items", l_picker_set_items},
    {"picker_set_preview", l_picker_set_preview},
    {"picker_close", l_picker_close},
    {"picker_set_tabs", l_picker_set_tabs},
    {"picker_is_open", l_picker_is_open},
    {"roam_graph_open", l_roam_graph_open},
    {"roam_graph_close", l_roam_graph_close},
    {"fuzzy_score", l_fuzzy_score},
    {"buffer_list", l_buffer_list},
    {"buffer_switch", l_buffer_switch},
    {"buffer_filename", l_buffer_filename},
    {"buffer_count", l_buffer_count},
    {"pane_buffers", l_pane_buffers},
    {"pane_focus_buffer", l_pane_focus_buffer},
    {"jump_to_buffer", l_jump_to_buffer},
    {"buffer_workspace", l_buffer_workspace},
    {"terminal_info", l_terminal_info},
    {"buffer_cursor_row", l_buffer_cursor_row},
    {"buffer_text_cols", l_buffer_text_cols},
    {"command_names", l_command_names},
    {"colorscheme", l_colorscheme},
    {"theme_names", l_theme_names},
    {"current_theme", l_current_theme},
    {"icon_for_file", l_icon_for_file},
    {"hl_for_file", l_hl_for_file},
    {"set_leader", l_set_leader},
    {"leader_map", l_leader_map},
    {"leader_group", l_leader_group},
    {"set_statusline", l_set_statusline},
    {"set_winbar_click", l_set_winbar_click},
    {"scratch", l_scratch},
    {"toggle_zen", l_toggle_zen},
    {"sheet_next", l_sheet_next},
    {"sheet_prev", l_sheet_prev},
    {"on_frame", l_on_frame},
    {"buffer_change_epoch", l_buffer_change_epoch},
    {"buffer_save_epoch", l_buffer_save_epoch},
    {"now", l_now},
    {"list_dir", l_list_dir},
    {"is_image_path", l_is_image_path},
    {"fs_mkdir", l_fs_mkdir},
    {"fs_create_file", l_fs_create_file},
    {"fs_rename", l_fs_rename},
    {"fs_delete", l_fs_delete},
    {"fs_copy", l_fs_copy},
    {"project_list", l_project_list},
    {"project_add", l_project_add},
    {"project_remove", l_project_remove},
    {"babel_cache_load", l_babel_cache_load},
    {"babel_cache_save", l_babel_cache_save},
    {"run_config_load", l_run_config_load},
    {"run_config_save", l_run_config_save},
    {"chdir", l_chdir},
    {"getcwd", l_getcwd},
    {"workspace_list", l_workspace_list},
    {"workspace_current", l_workspace_current},
    {"workspace_root", l_workspace_root},
    {"bundled_help_root", l_bundled_help_root},
    {"workspace_new", l_workspace_new},
    {"workspace_switch", l_workspace_switch},
    {"workspace_delete", l_workspace_delete},
    {"workspace_reset", l_workspace_reset},
    {"workspace_rename", l_workspace_rename},
    {"workspace_next", l_workspace_next},
    {"workspace_previous", l_workspace_previous},
    {"workspace_change_epoch", l_workspace_change_epoch},
    {"workspace_adopt", l_workspace_adopt},
    {"workspace_prune", l_workspace_prune},
    {"workspace_set_worktree_dir", l_workspace_set_worktree_dir},
    {"workspace_set_restore", l_workspace_set_restore},
    {"workspace_set_git_dirty", l_workspace_set_git_dirty},
    {"workspace_state_save", l_workspace_state_save},
    {"workspace_state_restore", l_workspace_state_restore},
    {"project_load", l_project_load},
    {"project_current", l_project_current},
    {"project_loaded_list", l_project_loaded_list},
    {"project_switch", l_project_switch},
    {"project_close", l_project_close},
    {"project_next", l_project_next},
    {"project_previous", l_project_previous},
    {"diff_lines", l_diff_lines},
    {"filename", l_filename},
    {"html_open", l_html_open},
    {"http_get", l_http_get},
    {"http_serve", l_http_serve},
    {"http_stop", l_http_stop},
    {"http_servers", l_http_servers},
    {"url_normalize", l_url_normalize},
    {"url_resolve", l_url_resolve},
    {"html_title", l_html_title},
    {"html_settle", l_html_settle},
    {"html_omnibar_edit", l_html_omnibar_edit},
    {"html_current_origin", l_html_current_origin},
    {"html_reload", l_html_reload},
    {"html_navigate", l_html_navigate},
    {"pdf_reload", l_pdf_reload},
    {"is_pdf_buffer", l_is_pdf_buffer},
    {"pdf_outline", l_pdf_outline},
    {"pdf_goto_page", l_pdf_goto_page},
    {"pdf_current_page", l_pdf_current_page},
    {"office_reload", l_office_reload},
    {"doc_export_html_to_latex", l_doc_export_html_to_latex},
    {"doc_export_html_to_odt", l_doc_export_html_to_odt},
    {"set_completion_source", l_set_completion_source},
    {"set_completion_accept_hook", l_set_completion_accept_hook},
    {"set_completion_resolve_hook", l_set_completion_resolve_hook},
    {"set_insert_tab_hook", l_set_insert_tab_hook},
    {"set_on_directory_open", l_set_on_directory_open},
    {"lsp_start", l_lsp_start},
    {"lsp_request", l_lsp_request},
    {"lsp_symbols_flatten", l_lsp_symbols_flatten},
    {"lsp_notify", l_lsp_notify},
    {"lsp_on_notification", l_lsp_on_notification},
    {"lsp_on_request", l_lsp_on_request},
    {"lsp_connect", l_lsp_connect},
    {"lsp_stop", l_lsp_stop},
    {"lsp_is_running", l_lsp_is_running},
    {"hint_jump", l_hint_jump},
    {"quick_jump", l_quick_jump},
    {"quick_jump_matches", l_quick_jump_matches},
    {"quick_jump_pick", l_quick_jump_pick},
    {"platform", l_platform},
    {"pane_open", l_pane_open},
    {"pane_next_buffer", l_pane_next_buffer},
    {"pane_prev_buffer", l_pane_prev_buffer},
    {"pane_close_buffer", l_pane_close_buffer},
    {"pane_move_buffer", l_pane_move_buffer},
    {"layout", l_layout},
    {"model_new", l_model_new},
    {"model_list_objects", l_model_list_objects},
    {"model_scene_stats", l_model_scene_stats},
    {"model_primitive_info", l_model_primitive_info},
    {"model_add_primitive", l_model_add_primitive},
    {"model_delete_object", l_model_delete_object},
    {"model_duplicate_object", l_model_duplicate_object},
    {"model_set_transform", l_model_set_transform},
    {"model_set_material", l_model_set_material},
    {"model_set_texture", l_model_set_texture},
    {"model_rename_object", l_model_rename_object},
    {"model_set_visible", l_model_set_visible},
    {"model_select", l_model_select},
    {"model_get_selection", l_model_get_selection},
    {"model_camera_set", l_model_camera_set},
    {"model_camera_get", l_model_camera_get},
    {"model_undo", l_model_undo},
    {"model_redo", l_model_redo},
    {"model_set_view", l_model_set_view},
    {"model_frame_all", l_model_frame_all},
    {"model_set_transforms", l_model_set_transforms},
    {"model_set_materials", l_model_set_materials},
    {"model_delete_objects", l_model_delete_objects},
    {"model_duplicate_mirrored", l_model_duplicate_mirrored},
    {"model_radial_array", l_model_radial_array},
    {"model_group_objects", l_model_group_objects},
    {"model_set_parent", l_model_set_parent},
    {"model_list_vertices", l_model_list_vertices},
    {"model_list_triangles", l_model_list_triangles},
    {"model_set_vertex_position", l_model_set_vertex_position},
    {"model_delete_vertices", l_model_delete_vertices},
    {"model_merge_vertices", l_model_merge_vertices},
    {"model_recalculate_normals", l_model_recalculate_normals},
    {"model_subdivide_faces", l_model_subdivide_faces},
    {"model_extrude_faces", l_model_extrude_faces},
    {"model_dissolve_vertex", l_model_dissolve_vertex},
    {"model_inset_faces", l_model_inset_faces},
    {"model_add_vertex", l_model_add_vertex},
    {"model_make_face", l_model_make_face},
    {"model_merge_by_distance", l_model_merge_by_distance},
    {"model_flip_normals", l_model_flip_normals},
    {nullptr, nullptr},
};

}  // namespace

LuaEnv::LuaEnv(Editor *editor) : editor_(editor) {
    L_ = luaL_newstate();
    luaL_openlibs(L_);

    lua_pushlightuserdata(L_, editor_);
    lua_setfield(L_, LUA_REGISTRYINDEX, kEditorRegistryKey);
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, kLuaEnvRegistryKey);

    luaL_newlib(L_, kMepFuncs);
    // A comparable sentinel for JSON `null` (Phase 20 LSP): PushJson uses
    // the same lightuserdata value, so Lua code can tell an explicit null
    // field apart from an absent one (`result == mep.json_null`) -- Lua's
    // own `nil` can't be stored as a table value, so it can't serve as
    // that sentinel itself.
    lua_pushlightuserdata(L_, kJsonNullSentinel);
    lua_setfield(L_, -2, "json_null");
    lua_setglobal(L_, "mep");

    lua_pushcfunction(L_, l_print);
    lua_setglobal(L_, "print");

    // Org-mode "Phase Org-0" primitives (LUA_TO_CPP_PLAN.md): registered
    // as bare globals, matching the exact names kBuiltinOrg's own
    // (removed) Lua implementations used -- every one of the ~80 call
    // sites across the org-mode cluster (main.cpp) keeps working
    // unchanged, since Lua globals (unlike locals) persist across every
    // separately-compiled DoString chunk this file's kBuiltin* strings
    // run through. Registered here, before any of them load.
    lua_pushcfunction(L_, l_org_parse_headline_global);
    lua_setglobal(L_, "mep_org_parse_headline");
    lua_pushcfunction(L_, l_org_current_headline_row_global);
    lua_setglobal(L_, "mep_org_current_headline_row");
    lua_pushcfunction(L_, l_org_subtree_end_global);
    lua_setglobal(L_, "mep_org_subtree_end");

    // mep_lsp_filetype/mep_lsp_abspath (LUA_TO_CPP_PLAN.md Phase 5 side
    // quest): pure string utilities historically defined inside
    // kBuiltinLsp, but used pervasively outside the LSP cluster itself
    // (org images/babel/polyglot/export/roam, ...). See LspFiletype/
    // LspAbspath's own comment (editor.h) for why these -- and only
    // these two -- are safe to port ahead of the rest of LSP. Bare
    // globals, same reason as the Org-0 primitives above.
    lua_pushcfunction(L_, l_lsp_filetype_global);
    lua_setglobal(L_, "mep_lsp_filetype");
    lua_pushcfunction(L_, l_lsp_abspath_global);
    lua_setglobal(L_, "mep_lsp_abspath");

    // mep_org_resolve_path (LUA_TO_CPP_PLAN.md Phase 5): kBuiltinOrgImages
    // and kBuiltinOrgBabel (two separate DoString chunks) both call this
    // by its bare-global name. Registered here for the same reason as
    // the other bare globals above.
    lua_pushcfunction(L_, l_org_resolve_path_global);
    lua_setglobal(L_, "mep_org_resolve_path");

    // mep_org_bib_cite_at_cursor (LUA_TO_CPP_PLAN.md Phase 5):
    // kBuiltinOrgBib and kBuiltinOrgLinks (two separate DoString chunks)
    // both call this by its bare-global name. Registered here for the
    // same reason as the other bare globals above.
    lua_pushcfunction(L_, l_org_bib_cite_at_cursor_global);
    lua_setglobal(L_, "mep_org_bib_cite_at_cursor");

    // mep_org_src_block_at (LUA_TO_CPP_PLAN.md Phase 5): kBuiltinOrgBabel
    // and kBuiltinOrgPolyglot (two separate DoString chunks) both call
    // this by its bare-global name. Registered here for the same reason
    // as the other bare globals above.
    lua_pushcfunction(L_, l_org_src_block_at_global);
    lua_setglobal(L_, "mep_org_src_block_at");
    lua_pushcfunction(L_, l_org_babel_should_wrap_main_global);
    lua_setglobal(L_, "mep_org_babel_should_wrap_main");

    // mep_ai_json_encode/mep_ai_json_decode (LUA_TO_CPP_PLAN.md Phase AI):
    // kBuiltinAi and kBuiltinLeetcode (two separate DoString chunks) both
    // call these by their bare-global names.
    lua_pushcfunction(L_, l_ai_json_encode_global);
    lua_setglobal(L_, "mep_ai_json_encode");
    lua_pushcfunction(L_, l_ai_json_decode_global);
    lua_setglobal(L_, "mep_ai_json_decode");
}

LuaEnv::~LuaEnv() {
    if (L_) lua_close(L_);
}

bool LuaEnv::DoString(const std::string &code) {
    if (luaL_dostring(L_, code.c_str()) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaEnv::DoFile(const std::string &path) {
    if (luaL_dofile(L_, path.c_str()) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void LuaEnv::CallRef(int ref) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
    }
}

void LuaEnv::CallRefWithString(int ref, const std::string &arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushlstring(L_, arg.data(), arg.size());
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
    }
}

void LuaEnv::CallRefWithInt(int ref, long long arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L_, arg);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
    }
}

void LuaEnv::CallRefWithBool(int ref, bool arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushboolean(L_, arg);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
    }
}

void LuaEnv::CallRefWithJson(int ref, const Json &arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    PushJson(L_, arg);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
    }
}

Json LuaEnv::CallRefWithJsonReturningJson(int ref, const Json &arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return Json::Object();
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    PushJson(L_, arg);
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return Json::Object();
    }
    Json result = lua_isnoneornil(L_, -1) ? Json::Object() : LuaToJson(L_, -1);
    lua_pop(L_, 1);
    return result;
}

bool LuaEnv::CallRefWithStringForStrings(int ref, const std::string &arg, std::vector<std::string> *out) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushlstring(L_, arg.data(), arg.size());
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L_, -1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L_, -1, i);
        if (lua_isstring(L_, -1)) out->emplace_back(lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return true;
}

bool LuaEnv::CallRefWithIntForString(int ref, long long arg, std::string *out) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L_, arg);
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    if (!lua_isstring(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    *out = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    return true;
}

bool LuaEnv::CallRefWithStringForCompletionItems(int ref, const std::string &arg, std::vector<std::string> *texts,
                                                  std::vector<std::string> *kinds, std::vector<std::string> *details,
                                                  std::vector<std::string> *docs) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushlstring(L_, arg.data(), arg.size());
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    auto string_field = [this](const char *field) -> std::string {
        lua_getfield(L_, -1, field);
        std::string s = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
        lua_pop(L_, 1);
        return s;
    };
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L_, -1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1)) {
            texts->push_back(string_field("text"));
            kinds->push_back(string_field("kind"));
            details->push_back(string_field("detail"));
            docs->push_back(string_field("doc"));
        } else if (lua_isstring(L_, -1)) {
            texts->emplace_back(lua_tostring(L_, -1));
            kinds->emplace_back();
            details->emplace_back();
            docs->emplace_back();
        }
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return true;
}

bool LuaEnv::CallRefWithStringForDetailDoc(int ref, const std::string &arg, std::string *detail, std::string *doc) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushlstring(L_, arg.data(), arg.size());
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    lua_getfield(L_, -1, "detail");
    *detail = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
    lua_pop(L_, 1);
    lua_getfield(L_, -1, "doc");
    *doc = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
    lua_pop(L_, 1);
    lua_pop(L_, 1);
    return true;
}

bool LuaEnv::CallRefWithBoolForBool(int ref, bool arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushboolean(L_, arg);
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    bool result = lua_toboolean(L_, -1);
    lua_pop(L_, 1);
    return result;
}

bool LuaEnv::CallRefWithStringForBool(int ref, const std::string &arg) {
    if (ref == LUA_NOREF || ref == LUA_REFNIL || ref == 0) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_pushlstring(L_, arg.data(), arg.size());
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    bool result = lua_toboolean(L_, -1);
    lua_pop(L_, 1);
    return result;
}

void LuaEnv::UnrefFunction(int ref) {
    if (ref != LUA_NOREF && ref != LUA_REFNIL) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
}

bool LuaEnv::CallRefForWidgets(int ref, std::vector<std::pair<std::string, std::string>> *out) {
    if (ref == 0 || ref == LUA_NOREF || ref == LUA_REFNIL) return false;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L_, -1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, "text");
            std::string text = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
            lua_pop(L_, 1);
            lua_getfield(L_, -1, "hl");
            std::string hl = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
            lua_pop(L_, 1);
            out->emplace_back(text, hl);
        }
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return true;
}

void LuaEnv::RegisterFrameHook(int ref) { frame_hook_refs_.push_back(ref); }

// A hook that returns `true` is a one-shot state machine announcing it's
// done (e.g. mep.activity_todo_start_agent's "wait for the workspace to
// switch, then wait for the terminal to come up" pair) -- without this,
// every such hook had no way to ever stop running and degenerated into a
// permanent no-op ("if done then return end") called every frame for the
// rest of the process, forever accumulating one more pointless Lua call
// per frame each time the action that registered it (e.g. starting an AI
// agent from the Todo panel) ran again. Existing hooks that return nothing
// (nil, the overwhelming majority -- the debounced buffer-changed/saved
// pollers this primitive was built for) are unaffected and stay
// registered exactly as before.
bool LuaEnv::CallFrameHookRef(int ref) {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L_, -1);
        if (editor_) editor_->SetStatusMessage(std::string("Lua error: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        return false;
    }
    bool done = lua_toboolean(L_, -1);
    lua_pop(L_, 1);
    return done;
}

void LuaEnv::RunFrameHooks() {
    // Index-based, bounded to this frame's starting count, and re-reading
    // frame_hook_refs_[i] fresh each iteration rather than caching an
    // iterator/pointer across the call: a hook can itself call
    // mep.on_frame (chaining a second wait-state, as the agent-start hooks
    // above do), which push_back's into this same vector and may
    // reallocate its buffer out from under a range-for/iterator -- this
    // loop only ever indexes into whatever buffer currently backs the
    // vector, so that reallocation is harmless. Newly-registered hooks are
    // left in place for next frame's pass rather than run immediately.
    size_t count = frame_hook_refs_.size();
    size_t write = 0;
    for (size_t i = 0; i < count; i++) {
        int ref = frame_hook_refs_[i];
        if (CallFrameHookRef(ref)) {
            luaL_unref(L_, LUA_REGISTRYINDEX, ref);
        } else {
            frame_hook_refs_[write++] = ref;
        }
    }
    for (size_t i = count; i < frame_hook_refs_.size(); i++) frame_hook_refs_[write++] = frame_hook_refs_[i];
    frame_hook_refs_.resize(write);
}

std::vector<std::string> LuaEnv::GetOrgTodoKeywords() const { return ReadOrgTodoKeywords(L_); }
