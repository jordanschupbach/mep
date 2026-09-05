#include "lua_env.h"
#include "doc_export.h"
#include "editor.h"
#include "job.h"
#include "treesitter.h"

#include <algorithm>
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
#endif

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "raylib.h"

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
        lua_pushlstring(L, bindings[i].sequence.data(), bindings[i].sequence.size());
        lua_setfield(L, -2, "seq");
        lua_pushlstring(L, bindings[i].description.data(), bindings[i].description.size());
        lua_setfield(L, -2, "desc");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.map_mod1(key, fn): binds a single letter key under the mod1 modifier
// (see mep.set_mod1), globally across all modes. `key` is a bare letter
// ("h") for mod1+letter, or "S-"/"C-" prefixed ("S-h", "C-h") for
// mod1+Shift+letter / mod1+Ctrl+letter. Two non-letter keys are also
// recognized, each its own special case in HandleMod1Shortcuts since
// neither falls in the A-Z scan the letter case uses: "Tab"/"S-Tab", and
// "CR"/"S-CR" (Enter -- no Ctrl variant, matching Tab). Overrides any
// prior mapping for that exact key, including the startup defaults.
/**
 * @brief Implements mep.map_mod1(key, fn): binds a single letter key (or "Tab"/"S-Tab"/"CR"/"S-CR") under the mod1 modifier, globally across all modes.
 * @param L Lua state; arg 1 is the key, arg 2 the callback function.
 * @return Number of values pushed (0).
 */
int l_map_mod1(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterMod1Mapping(key, ref);
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
        cols = static_cast<int>((static_cast<double>(GetScreenWidth()) * frac) / static_cast<double>(char_width));
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

// mep.sidebar_create(title, position, size) -> id.
/**
 * @brief Implements mep.sidebar_create(title, position, size): creates a new sidebar panel.
 * @param L Lua state; arg 1 is the sidebar title, optional arg 2 the dock position (default "right"), optional arg 3 the size in columns (default 34).
 * @return Number of values pushed (1: the new sidebar's id).
 */
int l_sidebar_create(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    const char *position = luaL_optstring(L, 2, "right");
    int size = static_cast<int>(luaL_optinteger(L, 3, 34));
    lua_pushinteger(L, GetEditor(L)->CreateSidebar(title, position, size));
    return 1;
}

// mep.sidebar_set_sections(id, sections): sections is an array of
// {id=, title=, collapsed=, widgets={{id=,text=,icon=,hl=,tooltip=,on_click=fn},...}}.
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
                w.on_click_ref = RefField(L, -1, "on_click");
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
// active pane's buffer. Single-slot, last-registration-wins.
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

// mep.list_dir(path) -> array of {name=, is_dir=}, directories first then
// files, both alphabetical (Phase 15 file tree). Editor::ListDirectory
// handles native vs. wasm (routed through the `just run-wasm` loopback bridge,
// same as :e/:w/:source -- empty when there's no bridge, e.g. a bare
// browser tab) so this is just the Lua table conversion.
/**
 * @brief Implements mep.list_dir(path): lists a directory's entries, directories first then alphabetically.
 * @param L Lua state; arg 1 is the directory path.
 * @return Number of values pushed (1: the array of {name=, is_dir=} entries).
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
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
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
 * @brief Implements mep.fs_rename(from, to): renames/moves a file or directory (native builds only).
 * @param L Lua state; arg 1 is the source path, arg 2 the destination path.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_rename(lua_State *L) {
    const char *from = luaL_checkstring(L, 1);
    const char *to = luaL_checkstring(L, 2);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    lua_pushboolean(L, !ec);
#else
    lua_pushboolean(L, false);
#endif
    return 1;
}

/**
 * @brief Implements mep.fs_delete(path): recursively deletes a file or directory (native builds only).
 * @param L Lua state; arg 1 is the path to delete.
 * @return Number of values pushed (1: true on success, false on error or under wasm).
 */
int l_fs_delete(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
#if !defined(__EMSCRIPTEN__)
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
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
    int client_id = 0;      // set right after JobManager::Spawn returns (l_lsp_start)
    std::string buffer;     // raw bytes accumulated, header+body(es) consumed as they complete
    int expected_len = -1;  // -1 = still accumulating headers for the next message
    int next_request_id = 1;
    std::unordered_map<int, int> pending;                    // request id -> Lua callback ref
    std::unordered_map<std::string, int> notification_refs;  // method -> Lua callback ref
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
 * @brief Parses one complete JSON-RPC message body and routes it to the matching pending-request callback or notification handler.
 * @param state LSP client state whose pending requests/notification handlers are consulted; silently returns if body doesn't parse as JSON.
 * @param body Decoded JSON-RPC message body (no framing headers).
 */
void DispatchLspMessage(LspClientState &state, const std::string &body) {
    Json msg;
    if (!Json::Parse(body, &msg)) return;
    if (msg.contains("id") && !msg.contains("method")) {
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

/**
 * @brief Implements mep.lsp_start(argv[, opts]): spawns an LSP server process and registers it as a tracked client.
 * @param L Lua state; arg 1 is a Lua array of argv strings for the server command, optional arg 2 is an {cwd=} options table.
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
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "cwd");
        if (lua_isstring(L, -1)) cwd = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    auto state = std::make_shared<LspClientState>();
    state->env = GetLuaEnv(L);
    JobManager::Callbacks cb;
    /**
     * @brief Job stdout callback: appends raw output bytes to the client's buffer and pumps out any complete LSP messages.
     * @param chunk Raw stdout bytes received from the server process.
     */
    cb.on_stdout_raw = [state](const std::string &chunk) {
        state->buffer += chunk;
        PumpLspBuffer(*state);
    };
    // NVIM_PARITY_PLAN.md Phase 20 gap: a request pending when the server
    // process dies used to never fire its callback at all (no on_exit was
    // registered here) -- state->pending's callback refs, and the Lua
    // coroutines/closures waiting on them, just leaked/hung forever. Fires
    // each with a synthetic JSON-RPC error response (same shape a real
    // error reply would have, so callers already checking `.error` need
    // no new code path) and mirrors l_lsp_stop's own g_lsp_clients cleanup
    // so a later mep.lsp_is_running/lsp_request against this client_id
    // correctly sees it as gone rather than silently queuing forever.
    /**
     * @brief Job exit callback: fires every still-pending request's callback with a synthetic JSON-RPC error and removes the client from g_lsp_clients.
     * @param (unused) The server process's exit code.
     */
    cb.on_exit = [state](int) {
        for (auto &kv : state->pending) {
            Json err = Json::Object();
            err["jsonrpc"] = Json("2.0");
            err["id"] = Json(kv.first);
            Json err_obj = Json::Object();
            err_obj["code"] = Json(-32000);
            err_obj["message"] = Json("LSP server exited");
            err["error"] = err_obj;
            state->env->CallRefWithJson(kv.second, err);
            state->env->UnrefFunction(kv.second);
        }
        state->pending.clear();
        if (state->client_id != 0) g_lsp_clients.erase(state->client_id);
    };
    int id = JobManager::Instance().Spawn(argv, cwd, std::move(cb));
    if (id != 0) {
        state->client_id = id;
        g_lsp_clients[id] = state;
    }
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
    msg["jsonrpc"] = Json("2.0");
    msg["id"] = Json(req_id);
    msg["method"] = Json(method);
    msg["params"] = params;
    JobManager::Instance().WriteStdin(client_id, LspFrame(msg));
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
    JobManager::Instance().WriteStdin(client_id, LspFrame(msg));
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
    JobManager::Instance().Kill(client_id);
    g_lsp_clients.erase(client_id);
    return 0;
}

/**
 * @brief Implements mep.lsp_is_running(client_id): checks whether an LSP client's server process is still running.
 * @param L Lua state; arg 1 is the client id to check.
 * @return Number of values pushed (1: boolean, whether the process is running).
 */
int l_lsp_is_running(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, JobManager::Instance().IsRunning(client_id));
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

int l_filename(lua_State *L) {
    lua_pushstring(L, GetEditor(L)->CurrentBuffer().filename.c_str());
    return 1;
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

// mep.leader_map(sequence, description, fn): binds a key sequence typed
// after the leader (e.g. mep.leader_map('ff', 'Find files', mep.find_files)).
int l_leader_map(lua_State *L) {
    const char *sequence = luaL_checkstring(L, 1);
    const char *description = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterWhichKey(sequence, description, ref);
    return 0;
}

// mep.leader_group(prefix, label): names a group of leader.map bindings
// sharing `prefix` (e.g. mep.leader_group('o', 'org')) so the whichkey
// popup shows one collapsed "o  +org" row instead of every leaf under it.
int l_leader_group(lua_State *L) {
    const char *prefix = luaL_checkstring(L, 1);
    const char *label = luaL_checkstring(L, 2);
    GetEditor(L)->RegisterWhichKeyGroup(prefix, label);
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

const luaL_Reg kMepFuncs[] = {
    {"get_line", l_get_line},
    {"set_line", l_set_line},
    {"replace_lines", l_replace_lines},
    {"line_count", l_line_count},
    {"visual_selection", l_visual_selection},
    {"cursor", l_cursor},
    {"set_cursor", l_set_cursor},
    {"current_buffer", l_current_buffer},
    {"participant_set", l_participant_set},
    {"participant_clear", l_participant_clear},
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
    {"cmd", l_cmd},
    {"open", l_open},
    {"terminal_here", l_terminal_here},
    {"is_terminal_buffer", l_is_terminal_buffer},
    {"terminal_write", l_terminal_write},
    {"sidebar_default_cols", l_sidebar_default_cols},
    {"quit", l_quit},
    {"job_start", l_job_start},
    {"job_write", l_job_write},
    {"job_close_stdin", l_job_close_stdin},
    {"job_kill", l_job_kill},
    {"job_is_running", l_job_is_running},
    {"ui_input", l_ui_input},
    {"ui_confirm", l_ui_confirm},
    {"ui_select", l_ui_select},
    {"float_preview", l_float_preview},
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
    {"buffer_ns_clear", l_buffer_ns_clear},
    {"buffer_deco_add", l_buffer_deco_add},
    {"term_start", l_term_start},
    {"term_resize", l_term_resize},
    {"buffer_new", l_buffer_new},
    {"buffer_set_on_enter", l_buffer_set_on_enter},
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
    {"sidebar_create", l_sidebar_create},
    {"sidebar_set_sections", l_sidebar_set_sections},
    {"sidebar_open", l_sidebar_open},
    {"sidebar_close", l_sidebar_close},
    {"sidebar_toggle", l_sidebar_toggle},
    {"sidebar_is_open", l_sidebar_is_open},
    {"sidebar_set_on_key", l_sidebar_set_on_key},
    {"sidebar_set_on_preview", l_sidebar_set_on_preview},
    {"sidebar_set_preview", l_sidebar_set_preview},
    {"sidebar_popout_toggle", l_sidebar_popout_toggle},
    {"sidebar_popout_close", l_sidebar_popout_close},
    {"sidebar_is_popout", l_sidebar_is_popout},
    {"read_lines", l_read_lines},
    {"sidebar_cursor", l_sidebar_cursor},
    {"sidebar_cursor_widget_id", l_sidebar_cursor_widget_id},
    {"picker_open", l_picker_open},
    {"picker_set_items", l_picker_set_items},
    {"picker_set_preview", l_picker_set_preview},
    {"picker_close", l_picker_close},
    {"roam_graph_open", l_roam_graph_open},
    {"roam_graph_close", l_roam_graph_close},
    {"fuzzy_score", l_fuzzy_score},
    {"buffer_list", l_buffer_list},
    {"buffer_switch", l_buffer_switch},
    {"buffer_filename", l_buffer_filename},
    {"buffer_count", l_buffer_count},
    {"pane_buffers", l_pane_buffers},
    {"pane_focus_buffer", l_pane_focus_buffer},
    {"buffer_cursor_row", l_buffer_cursor_row},
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
    {"fs_mkdir", l_fs_mkdir},
    {"fs_create_file", l_fs_create_file},
    {"fs_rename", l_fs_rename},
    {"fs_delete", l_fs_delete},
    {"project_list", l_project_list},
    {"project_add", l_project_add},
    {"project_remove", l_project_remove},
    {"babel_cache_load", l_babel_cache_load},
    {"babel_cache_save", l_babel_cache_save},
    {"chdir", l_chdir},
    {"getcwd", l_getcwd},
    {"workspace_list", l_workspace_list},
    {"workspace_current", l_workspace_current},
    {"workspace_root", l_workspace_root},
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
    {"html_current_origin", l_html_current_origin},
    {"html_reload", l_html_reload},
    {"doc_export_html_to_latex", l_doc_export_html_to_latex},
    {"doc_export_html_to_odt", l_doc_export_html_to_odt},
    {"set_completion_source", l_set_completion_source},
    {"set_completion_accept_hook", l_set_completion_accept_hook},
    {"set_completion_resolve_hook", l_set_completion_resolve_hook},
    {"set_insert_tab_hook", l_set_insert_tab_hook},
    {"lsp_start", l_lsp_start},
    {"lsp_request", l_lsp_request},
    {"lsp_symbols_flatten", l_lsp_symbols_flatten},
    {"lsp_notify", l_lsp_notify},
    {"lsp_on_notification", l_lsp_on_notification},
    {"lsp_stop", l_lsp_stop},
    {"lsp_is_running", l_lsp_is_running},
    {"hint_jump", l_hint_jump},
    {"platform", l_platform},
    {"pane_open", l_pane_open},
    {"pane_next_buffer", l_pane_next_buffer},
    {"pane_prev_buffer", l_pane_prev_buffer},
    {"pane_close_buffer", l_pane_close_buffer},
    {"pane_move_buffer", l_pane_move_buffer},
    {"layout", l_layout},
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

void LuaEnv::RunFrameHooks() {
    for (int ref : frame_hook_refs_) CallRef(ref);
}

std::vector<std::string> LuaEnv::GetOrgTodoKeywords() const { return ReadOrgTodoKeywords(L_); }
