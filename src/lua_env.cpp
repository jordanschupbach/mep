#include "lua_env.h"
#include "doc_export.h"
#include "editor.h"
#include "job.h"
#include "treesitter.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
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

Editor *GetEditor(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEditorRegistryKey);
    Editor *ed = static_cast<Editor *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ed;
}

LuaEnv *GetLuaEnv(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kLuaEnvRegistryKey);
    LuaEnv *env = static_cast<LuaEnv *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return env;
}

// Reads a ref for `field` out of the table at stack index `tbl_idx`, or
// LUA_NOREF if absent/not a function.
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
int l_get_line(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    std::string text = GetEditor(L)->GetLineForLua(row);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int l_set_line(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    size_t len = 0;
    const char *s = luaL_checklstring(L, 2, &len);
    GetEditor(L)->SetLineForLua(row, std::string(s, len));
    return 0;
}

// mep.replace_lines(start, end, lines): replaces lines [start, end)
// (1-indexed, end exclusive) with the given array of strings.
int l_replace_lines(lua_State *L) {
    int start_row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int end_row = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> lines;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 3));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 3, i);
        lines.push_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    GetEditor(L)->ReplaceLinesForLua(start_row, end_row, lines);
    return 0;
}

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
int l_visual_selection(lua_State *L) {
    std::string text = GetEditor(L)->CurrentVisualSelectionText();
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// mep.cursor() -> row, col (both 1-indexed).
int l_cursor(lua_State *L) {
    int row = 0, col = 0;
    GetEditor(L)->GetCursorForLua(&row, &col);
    lua_pushinteger(L, row + 1);
    lua_pushinteger(L, col + 1);
    return 2;
}

int l_set_cursor(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int col = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    GetEditor(L)->SetCursorForLua(row, col);
    return 0;
}

// mep.current_buffer() -> the active pane's own buffer id.
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
int l_participant_clear(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    GetEditor(L)->ClearLocalParticipant(id);
    return 0;
}

int l_insert_text(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->InsertTextForLua(std::string(s, len));
    return 0;
}

// mep.visual_change(): the Lua equivalent of pressing "c" on the current
// Visual selection -- deletes it (with the same undo/register semantics
// as any other change) and leaves the cursor, in Insert mode, at the
// deletion point, ready for mep.insert_text to stream a replacement in.
int l_visual_change(lua_State *L) {
    GetEditor(L)->ChangeVisualSelectionForLua();
    return 0;
}

// mep.enter_normal(): the Lua equivalent of pressing <Esc> -- returns to
// Normal mode from Insert or Visual.
int l_enter_normal(lua_State *L) {
    GetEditor(L)->EnterNormalForLua();
    return 0;
}

// mep.enter_insert(): the Lua equivalent of pressing "i" -- pushes one
// undo checkpoint and enters Insert mode. Used by speech-to-text so a
// streamed-in transcript behaves like typed text (one undo step per
// dictation) when the buffer wasn't already in Insert mode.
int l_enter_insert(lua_State *L) {
    GetEditor(L)->EnterInsertForLua();
    return 0;
}

// mep.is_insert_mode() -> true while Mode::Insert is active.
int l_is_insert_mode(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsInsertModeForLua());
    return 1;
}

// mep.stt_set_recording(bool): sets the tab bar's speech-to-text
// recording indicator on/off -- purely a display flag, see
// Editor::stt_recording_'s own comment; the recording process itself is
// entirely Lua/job-driven (mep.stt_toggle).
int l_stt_set_recording(lua_State *L) {
    GetEditor(L)->SetSttRecording(lua_toboolean(L, 1));
    return 0;
}

// mep.notify(msg [, level]): level is "debug"/"info"(default)/"warn"/
// "error". Single choke point for the whole app's messages (Phase 6) --
// feeds a toast + persistent history entry, and still updates the status
// line the way this always has.
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

int l_quit(lua_State *L) {
    GetEditor(L)->RequestQuit();
    return 0;
}

// mep.command(name, fn): defines a ":name" ex-command implemented in Lua.
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
int l_map_bracket_prev(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterBracketPrevMapping(key, ref);
    return 0;
}

int l_map_bracket_next(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterBracketNextMapping(key, ref);
    return 0;
}

// mep.set_mod1(name): "alt" (default), "ctrl", "shift", or "super".
int l_set_mod1(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    GetEditor(L)->SetMod1(name);
    return 0;
}

// mep.nav_pane(direction): moves focus to the pane best positioned
// "left"/"down"/"up"/"right" of the active one; a no-op if there's none.
int l_nav_pane(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    GetEditor(L)->NavigatePaneDirection(dir);
    return 0;
}

// mep.focus_top_left_pane(): moves focus to the topmost, then leftmost,
// pane in the active tab's split layout -- used by the file tree so a
// click always opens near the tree rather than in whatever pane was last
// active before the sidebar took focus.
int l_focus_top_left_pane(lua_State *L) {
    GetEditor(L)->FocusTopLeftPane();
    return 0;
}

// mep.resize_pane(direction, step?): moves the split boundary between the
// active pane and its neighbor "left"/"down"/"up"/"right" by `step` (a
// 0..1 fraction of the split's extent, defaults to 5% if omitted).
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
int l_pane_set_share(lua_State *L) {
    float fraction = static_cast<float>(luaL_checknumber(L, 1));
    GetEditor(L)->SetActivePaneShare(fraction);
    return 0;
}

// mep.cmd(str): runs str as if typed after ":" and Enter pressed. General
// escape hatch for Lua to drive any ex-command (":vsplit", ":w", ...).
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
int l_sidebar_default_cols(lua_State *L) {
    double frac = luaL_optnumber(L, 1, 0.2);
    float char_width = GetCharWidthPx();
    int cols = 34;
    if (char_width > 0.0f) {
        cols = static_cast<int>((static_cast<double>(GetScreenWidth()) * frac) / char_width);
    }
    lua_pushinteger(L, std::max(cols, 10));
    return 1;
}

// mep.terminal_here(args?): like `:terminal args`, but attaches the shell to
// the currently active pane instead of splitting -- for scripts that already
// arranged the split layout themselves (e.g. the built-in project_open,
// which wants the terminal pane on the bottom rather than :terminal's
// above/left default).
int l_terminal_here(lua_State *L) {
    GetEditor(L)->OpenTerminalInPlace(luaL_optstring(L, 1, ""));
    return 0;
}

// mep.is_terminal_buffer(buffer_id) -> bool: true for a real `:terminal`
// buffer (Editor::terminals_), false for anything else -- including a
// mep.term_start-backed Run/REPL output buffer (kBuiltinRun), which is a
// plain buffer Lua renders into and the C++ side never tracks as one.
int l_is_terminal_buffer(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, GetEditor(L)->IsTerminalBuffer(buffer_id));
    return 1;
}

// mep.terminal_write(buffer_id, text) -> bool: writes `text` verbatim
// (no newline appended -- callers wanting one write it themselves, same
// convention as mep.job_write) into a real `:terminal` buffer's own
// PTY. False if `buffer_id` isn't a live terminal.
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
int l_job_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv.push_back(luaL_checkstring(L, -1));
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

    LuaEnv *env = GetLuaEnv(L);
    JobManager::Callbacks cb;
    if (on_stdout_ref != LUA_NOREF) {
        cb.on_stdout = [env, on_stdout_ref](const std::string &line) { env->CallRefWithString(on_stdout_ref, line); };
    }
    if (on_stderr_ref != LUA_NOREF) {
        cb.on_stderr = [env, on_stderr_ref](const std::string &line) { env->CallRefWithString(on_stderr_ref, line); };
    }
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

int l_job_write(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    size_t len = 0;
    const char *s = luaL_checklstring(L, 2, &len);
    bool ok = JobManager::Instance().WriteStdin(id, std::string(s, len));
    lua_pushboolean(L, ok);
    return 1;
}

int l_job_close_stdin(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    JobManager::Instance().CloseStdin(id);
    return 0;
}

int l_job_kill(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    JobManager::Instance().Kill(id);
    return 0;
}

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
int l_ui_select(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *title = luaL_optstring(L, 2, "");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    std::vector<std::string> items;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        items.push_back(luaL_checkstring(L, -1));
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
int l_hover_show(lua_State *L) {
    const char *title = luaL_optstring(L, 1, "");
    size_t len = 0;
    const char *text = luaL_checklstring(L, 2, &len);
    GetEditor(L)->ShowHover(title, std::string(text, len));
    return 0;
}

// mep.hover_close(): dismiss an open hover tooltip early (e.g. a new
// hover request superseding a still-open one).
int l_hover_close(lua_State *L) {
    GetEditor(L)->CloseHover();
    return 0;
}

// mep.hover_is_open() -> bool. Lets a caller like mep.lsp_hover tell "K
// opened a fresh popup" from "K was pressed again while one's already
// showing" without keeping its own possibly-stale copy of that state.
int l_hover_is_open(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsHoverOpen());
    return 1;
}

// mep.hover_focus_enter(): moves the cursor into the open hover popup's
// own text (Mode::HoverFocus) so it can be navigated/selected/yanked like
// a normal buffer -- see Mode::HoverFocus's doc comment (editor.h) and
// Editor::HandleHoverFocusInput. No-op if hover isn't open.
int l_hover_focus_enter(lua_State *L) {
    GetEditor(L)->EnterHoverFocus();
    return 0;
}

// mep.ns_create(name) -> id. Stable per name (nvim_create_namespace-like).
int l_ns_create(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushinteger(L, GetEditor(L)->CreateNamespace(name));
    return 1;
}

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
int l_dap_toggle_breakpoint(lua_State *L) {
    GetEditor(L)->DapToggleBreakpoint();
    return 0;
}

// mep.dap_breakpoint_lines(filename) -> array of 1-indexed line numbers
// (Editor::DapBreakpointLines) -- kBuiltinDap's mep.dap_start (main.cpp)
// wraps each into a DAP {line = ...} object for the setBreakpoints
// request, once its own 'initialized' notification handler fires.
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
int l_termsend_register(lua_State *L) {
    int source = static_cast<int>(luaL_checkinteger(L, 1));
    int target = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, GetEditor(L)->TermsendRegister(source, target));
    return 1;
}

// mep.termsend_target(source) -> target buffer id, or nil if none
// registered or its registered target is no longer a live terminal buffer.
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

int l_termsend_unregister(lua_State *L) {
    int source = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->TermsendUnregister(source);
    return 0;
}

// mep.termsend_candidates() -> array of terminal buffer ids currently
// shown by a pane in the active tab (Editor::TermsendCandidates) -- what
// kBuiltinTermSend's registration prompt offers as its default.
int l_termsend_candidates(lua_State *L) {
    std::vector<int> ids = GetEditor(L)->TermsendCandidates();
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    for (size_t i = 0; i < ids.size(); i++) {
        lua_pushinteger(L, ids[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.activity_todo_load(path) -> array of {done=bool, text=string}: see
// Editor::ActivityTodoLoad.
int l_activity_todo_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::vector<Editor::ActivityTodoItem> items = GetEditor(L)->ActivityTodoLoad(path);
    lua_createtable(L, static_cast<int>(items.size()), 0);
    for (size_t i = 0; i < items.size(); i++) {
        lua_createtable(L, 0, 2);
        lua_pushboolean(L, items[i].done);
        lua_setfield(L, -2, "done");
        lua_pushlstring(L, items[i].text.data(), items[i].text.size());
        lua_setfield(L, -2, "text");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// mep.activity_todo_save(path, items): `items` is an array of {done=, text=}.
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
        lua_pop(L, 1);  // the item table itself
        items.push_back(std::move(item));
    }
    GetEditor(L)->ActivityTodoSave(path, items);
    return 0;
}

// mep.activity_test_failure_lines(output) -> array of {index, line}
// (1-indexed): see Editor::ActivityTestFailureLines.
int l_activity_test_failure_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> output;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        output.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
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
int l_syntax_highlight_fallback(lua_State *L) {
    int ns = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> keywords;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isstring(L, -1)) keywords.push_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    std::string comment_prefix = (lua_gettop(L) >= 3 && lua_isstring(L, 3)) ? lua_tostring(L, 3) : "";
    GetEditor(L)->SyntaxHighlightFallback(ns, keywords, comment_prefix);
    return 0;
}

// mep.org_highlight_emphasis(ns): org emphasis markup over the *current*
// buffer -- see Editor::OrgHighlightEmphasis.
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
int l_md_toggle_checkbox(lua_State *L) {
    GetEditor(L)->MdToggleCheckbox();
    return 0;
}
int l_md_fold(lua_State *L) {
    GetEditor(L)->MdComputeFolds();
    return 0;
}
int l_md_table_align(lua_State *L) {
    GetEditor(L)->MdTableAlign();
    return 0;
}
int l_md_table_insert_row(lua_State *L) {
    GetEditor(L)->MdTableInsertRow();
    return 0;
}
int l_md_table_insert_col(lua_State *L) {
    GetEditor(L)->MdTableInsertCol();
    return 0;
}

// mep.md_conceal_scan(ns): the link/emphasis concealment scan itself
// (Editor::MdConceal) -- kBuiltinMarkdown's mep.md_conceal keeps the
// namespace create/clear and the auto/filetype gating in Lua, calling
// this once those checks pass.
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
int l_snippet_splice(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    const char *before = luaL_checkstring(L, 2);
    const char *after = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    std::vector<std::string> body;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 4));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 4, i);
        body.push_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    GetEditor(L)->SnippetSplice(row, before, after, body);
    return 0;
}

// mep.snippet_jump(delta): see Editor::SnippetJump. Bound directly under
// its original name -- MepSnippetNext/MepSnippetPrev call it unchanged.
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
int l_buffer_set_lines(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<std::string> lines;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        lines.push_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    GetEditor(L)->SetBufferLinesForLua(buffer_id, lines);
    return 0;
}

int l_buffer_ns_clear(lua_State *L) {
    int buffer_id = static_cast<int>(luaL_checkinteger(L, 1));
    int ns = static_cast<int>(luaL_checkinteger(L, 2));
    GetEditor(L)->ClearNamespaceInBuffer(buffer_id, ns);
    return 0;
}

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
int l_buffer_new(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->CreateBufferForLua());
    return 1;
}

// mep.term_start(argv, opts) -> job id. Like mep.job_start, but spawns
// against a real PTY (Part VI Phase 27) instead of plain pipes -- opts
// may set `cwd`, `on_stdout_raw`, `on_exit`, same shape as job_start's
// opts otherwise.
int l_term_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv.push_back(luaL_checkstring(L, -1));
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
        cb.on_stdout_raw = [env, on_stdout_raw_ref](const std::string &chunk) {
            env->CallRefWithString(on_stdout_raw_ref, chunk);
        };
    }
    cb.on_exit = [env, on_stdout_raw_ref, on_exit_ref](int code) {
        if (on_exit_ref != LUA_NOREF) env->CallRefWithInt(on_exit_ref, code);
        if (on_stdout_raw_ref != LUA_NOREF) env->UnrefFunction(on_stdout_raw_ref);
        if (on_exit_ref != LUA_NOREF) env->UnrefFunction(on_exit_ref);
    };
    int id = JobManager::Instance().Spawn(argv, cwd, std::move(cb), /*use_pty=*/true);
    lua_pushinteger(L, id);
    return 1;
}

int l_term_resize(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    int cols = static_cast<int>(luaL_checkinteger(L, 2));
    int rows = static_cast<int>(luaL_checkinteger(L, 3));
    JobManager::Instance().ResizePty(id, cols, rows);
    return 0;
}

// mep.fold_create(start_row, end_row, closed, provider) -- rows 1-indexed.
int l_fold_create(lua_State *L) {
    int start_row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    int end_row = static_cast<int>(luaL_checkinteger(L, 2)) - 1;
    bool closed = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
    std::string provider = luaL_optstring(L, 4, "manual");
    GetEditor(L)->CreateFold(start_row, end_row, closed, provider);
    return 0;
}

int l_fold_clear_provider(lua_State *L) {
    const char *provider = luaL_checkstring(L, 1);
    GetEditor(L)->ClearFoldsFromProvider(provider);
    return 0;
}

int l_fold_toggle(lua_State *L) {
    GetEditor(L)->ToggleFoldAtCursor();
    return 0;
}

// mep.buf_set_image_row(row, path) -- row 1-indexed, matching mep.deco_add/
// mep.fold_create's own convention. Registers `path` (already resolved by
// the caller, mep.org_image_scan in kBuiltinOrgImages) as the current
// buffer's inline-image target for `row`.
int l_buf_set_image_row(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
    const char *path = luaL_checkstring(L, 2);
    GetEditor(L)->SetOrgImageRow(row, path);
    return 0;
}

// mep.buf_clear_image_rows(): clears the current buffer's whole registry --
// mep.org_image_scan calls this before rescanning.
int l_buf_clear_image_rows(lua_State *L) {
    GetEditor(L)->ClearOrgImageRows();
    return 0;
}

// mep.org_images_toggle() -> new visibility (bool). Bound to <leader>oti
// via kBuiltinOrgImages' own mep.leader_map call.
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
int l_buf_clear_latex_rows(lua_State *L) {
    GetEditor(L)->ClearOrgLatexRows();
    return 0;
}

// mep.org_latex_toggle() -> new visibility (bool). Bound to <leader>otl via
// kBuiltinOrgLatex's own mep.leader_map call.
int l_org_latex_toggle(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->ToggleOrgLatex());
    return 1;
}

// mep.org_latex_visible() -> bool. Lua-side readable state (unlike
// OrgImagesVisible(), org_latex_scan itself needs to consult this -- see
// Buffer::org_latex_rows' own comment for why the two toggles' scan
// functions differ here).
int l_org_latex_visible(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->OrgLatexVisible());
    return 1;
}

// mep.buf_add_latex_inline(row, col_start, col_end, path) -- same 1-indexed/
// exclusive-col_end convention as mep.deco_add's opts table. Appends one
// inline-math span (Buffer::OrgLatexInlineSpan) for `row`; called once per
// match by mep_org_latex_register_inline (kBuiltinOrgLatex).
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
int l_font_size(lua_State *L) {
    lua_pushnumber(L, GetFontSizePx());
    return 1;
}

// mep.image_size(path) -> width, height (integers) or nil if `path` isn't a
// readable PNG. Parses just the PNG signature + IHDR chunk (bytes 16-23),
// not a full decode -- plenty for mep_org_latex_render (kBuiltinOrgLatex) to
// turn a just-rendered fragment's pixel height into a slot count without
// pulling in a real image decoder or touching the GPU texture cache
// (GetOrLoadOrgInlineImageTexture) a frame before DrawPane otherwise would.
int l_image_size(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::ifstream f(path, std::ios::binary);
    unsigned char header[24];
    if (!f || !f.read(reinterpret_cast<char *>(header), sizeof(header))) return 0;
    static const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (std::memcmp(header, kPngSig, sizeof(kPngSig)) != 0) return 0;
    auto be32 = [&](int off) {
        return (static_cast<uint32_t>(header[off]) << 24) | (static_cast<uint32_t>(header[off + 1]) << 16) |
               (static_cast<uint32_t>(header[off + 2]) << 8) | static_cast<uint32_t>(header[off + 3]);
    };
    lua_pushinteger(L, static_cast<lua_Integer>(be32(16)));
    lua_pushinteger(L, static_cast<lua_Integer>(be32(20)));
    return 2;
}

// mep.sidebar_create(title, position, size) -> id.
int l_sidebar_create(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    const char *position = luaL_optstring(L, 2, "right");
    int size = static_cast<int>(luaL_optinteger(L, 3, 34));
    lua_pushinteger(L, GetEditor(L)->CreateSidebar(title, position, size));
    return 1;
}

// mep.sidebar_set_sections(id, sections): sections is an array of
// {id=, title=, collapsed=, widgets={{id=,text=,icon=,hl=,tooltip=,on_click=fn},...}}.
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

int l_sidebar_open(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool focus = lua_gettop(L) < 2 || lua_toboolean(L, 2);
    GetEditor(L)->OpenSidebar(id, focus);
    return 0;
}

int l_sidebar_close(lua_State *L) {
    GetEditor(L)->CloseSidebar(static_cast<int>(luaL_checkinteger(L, 1)));
    return 0;
}

int l_sidebar_toggle(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    bool focus = lua_gettop(L) < 2 || lua_toboolean(L, 2);
    GetEditor(L)->ToggleSidebar(id, focus);
    return 0;
}

int l_sidebar_is_open(lua_State *L) {
    lua_pushboolean(L, GetEditor(L)->IsSidebarOpen(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

// mep.sidebar_set_on_key(id, fn): fn(char) for any key HandleSidebarInput
// doesn't already reserve (Phase 15's file tree uses this for create/
// rename/delete/refresh/toggle-hidden).
int l_sidebar_set_on_key(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetSidebarOnKey(id, ref);
    return 0;
}

int l_sidebar_cursor(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->SidebarCursor() + 1);
    return 1;
}

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

// Reads a Lua array of items (each either a plain string, or a
// {display=, data=} table) at stack index `idx` into `out`.
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

int l_picker_set_items(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<PickerItem> items;
    ReadPickerItems(L, 1, items);
    GetEditor(L)->SetPickerItems(std::move(items));
    return 0;
}

// mep.picker_set_preview(text): sets the text shown in the picker's
// preview column (NVIM_PARITY_PLAN.md Phase 8 gap, closed) -- see
// Editor::SetPickerPreview's own comment. Pass "" to hide the column
// again (e.g. a source whose highlighted item has nothing previewable).
int l_picker_set_preview(lua_State *L) {
    size_t len = 0;
    const char *text = luaL_optlstring(L, 1, "", &len);
    GetEditor(L)->SetPickerPreview(std::string(text, len));
    return 0;
}

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

int l_roam_graph_close(lua_State *L) {
    GetEditor(L)->CloseRoamGraphDiscardingCallback();
    return 0;
}

// mep.buffer_list() -> array of {display=label, data=buffer_id (as string)}.
int l_buffer_list(lua_State *L) {
    Editor *ed = GetEditor(L);
    int n = ed->BufferCountForLua();
    lua_newtable(L);
    int out_i = 1;
    for (int id = 0; id < n; id++) {
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

int l_buffer_switch(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->SwitchToBufferForLua(id);
    return 0;
}

// mep.buffer_filename(id) -> raw path, '' for a terminal/unsaved buffer.
int l_buffer_filename(lua_State *L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushstring(L, GetEditor(L)->BufferFilenameForLua(id).c_str());
    return 1;
}

// mep.buffer_count() -> number of open buffers (cheap poll target, unlike
// mep.buffer_list() which allocates a full label table every call).
int l_buffer_count(lua_State *L) {
    lua_pushinteger(L, GetEditor(L)->BufferCountForLua());
    return 1;
}

// mep.pane_buffers() -> array of buffer ids currently shown by a pane in
// the active tab's own split layout (Editor::PaneBuffersInActiveTab) --
// for a script that wants to "find an already-open terminal pane"
// without spawning one itself (kBuiltinTermSend).
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
int l_colorscheme(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushboolean(L, GetEditor(L)->ApplyTheme(name));
    return 1;
}

int l_theme_names(lua_State *L) {
    std::vector<std::string> names = GetEditor(L)->ThemeNames();
    lua_newtable(L);
    for (size_t i = 0; i < names.size(); i++) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

int l_current_theme(lua_State *L) {
    lua_pushstring(L, GetEditor(L)->CurrentThemeName().c_str());
    return 1;
}

// Per-pane buffer tabs + auto-layouts (Phase 14).
int l_pane_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->PaneOpenBufferInTab(path);
    return 0;
}
int l_pane_next_buffer(lua_State *L) {
    GetEditor(L)->PaneNextBufferTab();
    return 0;
}
int l_pane_prev_buffer(lua_State *L) {
    GetEditor(L)->PanePrevBufferTab();
    return 0;
}
int l_pane_close_buffer(lua_State *L) {
    GetEditor(L)->PaneCloseBufferTab();
    return 0;
}
int l_pane_move_buffer(lua_State *L) {
    const char *dir = luaL_checkstring(L, 1);
    GetEditor(L)->PaneMoveBufferTabToNeighbor(dir);
    return 0;
}
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
int l_list_dir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::vector<Editor::DirEntry> entries = GetEditor(L)->ListDirectory(path);
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

int l_fs_create_file(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    bool ok = false;
#if !defined(__EMSCRIPTEN__)
    FILE *f = std::fopen(path, "ab");
    if (f) {
        ok = true;
        std::fclose(f);
    }
#endif
    lua_pushboolean(L, ok);
    return 1;
}

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
int l_project_list(lua_State *L) {
    std::vector<std::string> list = GetEditor(L)->ListProjects();
    lua_newtable(L);
    for (size_t i = 0; i < list.size(); i++) {
        lua_pushstring(L, list[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

int l_project_add(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->AddProject(path);
    return 0;
}

int l_project_remove(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    GetEditor(L)->RemoveProject(path);
    return 0;
}

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
                ptext = label.substr(start, end - start);
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
int l_tree_build_rows(lua_State *L) {
    const char *root = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    bool show_hidden = lua_toboolean(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);

    std::vector<std::string> expanded;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isstring(L, -1)) expanded.push_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    std::vector<std::string> ignored;
    n = static_cast<lua_Integer>(lua_rawlen(L, 4));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 4, i);
        if (lua_isstring(L, -1)) ignored.push_back(lua_tostring(L, -1));
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
int l_colorize(lua_State *L) {
    GetEditor(L)->Colorize();
    return 0;
}

// mep.url_under_cursor() -> string or nil: see Editor::UrlUnderCursor.
// Bound directly, no wrapper.
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
AnsiRenderResult AnsiRender(const std::string &raw) {
    AnsiRenderResult result;
    result.lines.emplace_back();
    int row = 1;
    bool has_hl = false;
    std::string cur_hl;
    int span_start = -1;  // -1 = no open span
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
                close_span(static_cast<int>(result.lines[row - 1].size()) + 1);
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
                if (has_hl) span_start = static_cast<int>(result.lines[row - 1].size()) + 1;
            }
            i = seq_end + 1;
        } else if (c == '\n') {
            close_span(static_cast<int>(result.lines[row - 1].size()) + 1);
            row++;
            result.lines.emplace_back();
            if (has_hl) span_start = 1;
            i++;
        } else if (c == '\r') {
            i++;
        } else {
            result.lines[row - 1] += c;
            i++;
        }
    }
    close_span(static_cast<int>(result.lines[row - 1].size()) + 1);
    return result;
}
}  // namespace

// mep.ansi_render(raw) -> lines, spans: see AnsiRender above.
// kBuiltinRun's own mep_term_redraw (main.cpp) feeds `spans` straight
// into mep.buffer_deco_add, same {row=, col_start=, col_end=, hl=} shape
// mep_ansi_render always returned.
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
std::vector<std::string> ReadOrgTodoKeywords(lua_State *L) {
    std::vector<std::string> kws;
    lua_getglobal(L, "mep");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "org_todo_keywords");
        if (lua_istable(L, -1)) {
            lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_isstring(L, -1)) kws.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return kws;
}

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
int l_org_parse_headline_global(lua_State *L) {
    const char *line = luaL_checkstring(L, 1);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    PushOrgHeadlineParse(L, ParseOrgHeadline(line, kws));
    return 1;
}

// mep_org_current_headline_row([row]) -> row or nil: see
// Editor::OrgCurrentHeadlineRow. Bare global, same reason.
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
int l_org_subtree_end_global(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    lua_pushinteger(L, GetEditor(L)->OrgSubtreeEnd(row, kws));
    return 1;
}

// mep.org_clock_in()/mep.org_clock_out(): see Editor::OrgClockIn/OrgClockOut.
int l_org_clock_in(lua_State *L) {
    GetEditor(L)->OrgClockIn();
    return 0;
}

int l_org_clock_out(lua_State *L) {
    GetEditor(L)->OrgClockOut();
    return 0;
}

// mep.org_clock_table_items() -> array of "indent+title  H:MM" strings:
// see Editor::OrgClockTableItems. kBuiltinOrgClock's own
// mep.org_clock_table() is a thin wrapper feeding this into
// mep.picker_open (which needs a Lua callback ref, so stays Lua glue).
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
int l_org_property_set(lua_State *L) {
    int row = static_cast<int>(luaL_optinteger(L, 1, 0));
    const char *key = luaL_checkstring(L, 2);
    const char *value = luaL_checkstring(L, 3);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgPropertySet(row, key, value, kws);
    return 0;
}

// mep.org_property_remove(row, key): see Editor::OrgPropertyRemove.
int l_org_property_remove(lua_State *L) {
    int row = static_cast<int>(luaL_optinteger(L, 1, 0));
    const char *key = luaL_checkstring(L, 2);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgPropertyRemove(row, key, kws);
    return 0;
}

// mep.org_drill_grade(row, quality): see Editor::OrgDrillGrade.
int l_org_drill_grade(lua_State *L) {
    int row = static_cast<int>(luaL_checkinteger(L, 1));
    int quality = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgDrillGrade(row, quality, kws);
    return 0;
}

// mep.org_agenda_expand_glob(pattern) -> array of paths: see
// Editor::OrgAgendaExpandGlob.
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
int l_org_agenda_scan_lines(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *path = luaL_checkstring(L, 2);
    std::vector<std::string> lines;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        lines.push_back(luaL_optstring(L, -1, ""));
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
int l_org_expand_capture_template(lua_State *L) {
    const char *tmpl = luaL_checkstring(L, 1);
    std::string expanded = GetEditor(L)->OrgExpandCaptureTemplate(tmpl);
    lua_pushlstring(L, expanded.data(), expanded.size());
    return 1;
}

// mep.org_refile_move(target_row) -> new cursor row or nil: see
// Editor::OrgRefileMove.
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
int l_org_bib_parse_files(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> texts;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        texts.push_back(luaL_optstring(L, -1, ""));
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
std::vector<std::string> ReadStringArray(lua_State *L, int idx) {
    std::vector<std::string> out;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        out.push_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    return out;
}

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
int l_org_roam_files_in(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> dirs = ReadStringArray(L, 1);
    PushStringArray(L, GetEditor(L)->OrgRoamFilesIn(dirs));
    return 1;
}

// mep.org_roam_title_of({line1, line2, ...}) -> title string or nil:
// see Editor::OrgRoamTitleOf.
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
int l_org_roam_ensure_id(lua_State *L) {
    std::string id = GetEditor(L)->OrgRoamEnsureId();
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

// mep.org_roam_find_backlink_lines({line1, ...}, target_id) -> array of
// 1-indexed line numbers: see Editor::OrgRoamFindBacklinkLines.
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
int l_org_roam_slugify(lua_State *L) {
    const char *title = luaL_checkstring(L, 1);
    std::string slug = OrgRoamSlugify(title);
    lua_pushlstring(L, slug.data(), slug.size());
    return 1;
}

// mep.org_table_align(): see Editor::OrgTableAlign.
int l_org_table_align(lua_State *L) {
    GetEditor(L)->OrgTableAlign();
    return 0;
}

// mep.org_link_at_cursor() -> target, desc (desc nil if none), or nil:
// see Editor::OrgLinkAtCursor.
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
int l_org_timestamp_insert(lua_State *L) {
    bool active = lua_toboolean(L, 1);
    GetEditor(L)->OrgTimestampInsert(active);
    return 0;
}

// mep.org_timestamp_shift(delta_days): see Editor::OrgTimestampShift.
int l_org_timestamp_shift(lua_State *L) {
    int delta_days = static_cast<int>(luaL_checkinteger(L, 1));
    GetEditor(L)->OrgTimestampShift(delta_days);
    return 0;
}

// mep.org_timestamp_at(line, col) -> {col_start=,col_end=,body=,
// active=} or nil: see Editor::OrgTimestampAt. Exposed so the still-Lua
// mep.org_timestamp_set_repeater can keep calling it.
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
int l_org_footnote_jump(lua_State *L) {
    GetEditor(L)->OrgFootnoteJump();
    return 0;
}

// mep.org_set_planning(kind): see Editor::OrgSetPlanning.
int l_org_set_planning(lua_State *L) {
    const char *kind = luaL_checkstring(L, 1);
    std::vector<std::string> kws = ReadOrgTodoKeywords(L);
    GetEditor(L)->OrgSetPlanning(kind, kws);
    return 0;
}

// mep.org_export_heading(format, level, title) -> heading string: see
// OrgExportHeading.
int l_org_export_heading(lua_State *L) {
    const char *format = luaL_checkstring(L, 1);
    int level = static_cast<int>(luaL_checkinteger(L, 2));
    const char *title = luaL_checkstring(L, 3);
    std::string result = OrgExportHeading(format, level, title);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep.org_html_escape(s) -> escaped string: see OrgHtmlEscape.
int l_org_html_escape(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    std::string result = OrgHtmlEscape(s);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep.org_subtree_end_lines({line1, ...}, row) -> row: see
// OrgSubtreeEndLines.
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
int l_org_babel_format_literal(lua_State *L) {
    const char *raw = luaL_checkstring(L, 1);
    std::string result = OrgBabelFormatLiteral(raw);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

// mep.org_parse_vars(args_str) -> {name = value, ...}: see OrgParseVars.
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
int l_lsp_diag_wrap(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    int width = static_cast<int>(luaL_checkinteger(L, 2));
    std::vector<std::string> lines = LspDiagWrap(text, width);
    PushStringArray(L, lines);
    return 1;
}

// mep_org_resolve_path(path) -> resolved path: see Editor::OrgResolvePath.
// Bare global, same reason as the Org-0/mep_lsp_* primitives.
int l_org_resolve_path_global(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    std::string resolved = GetEditor(L)->OrgResolvePath(path);
    lua_pushlstring(L, resolved.data(), resolved.size());
    return 1;
}

// mep.org_image_scan(): see Editor::OrgImageScan.
int l_org_image_scan(lua_State *L) {
    GetEditor(L)->OrgImageScan();
    return 0;
}

// mep_lsp_filetype(fname) -> extension string or nil: see LspFiletype
// (editor.h/.cpp). Bare global, same reason as the Org-0 primitives
// above.
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
int l_lsp_abspath_global(lua_State *L) {
    const char *fname = luaL_checkstring(L, 1);
    std::string ap = LspAbspath(fname);
    lua_pushlstring(L, ap.data(), ap.size());
    return 1;
}

// mep.git_gutter_refresh_native(base): see Editor::GitGutterRefresh.
// kBuiltinGit's own mep.git_gutter_refresh (main.cpp) is a one-line
// wrapper threading mep.git_gutter_base through.
int l_git_gutter_refresh_native(lua_State *L) {
    const char *base = luaL_checkstring(L, 1);
    GetEditor(L)->GitGutterRefresh(base);
    return 0;
}

// mep.git_next_hunk_row()/mep.git_prev_hunk_row() -> 1-indexed row or
// nil: see Editor::GitNextHunkRow/GitPrevHunkRow.
int l_git_next_hunk_row(lua_State *L) {
    int row = GetEditor(L)->GitNextHunkRow();
    if (row <= 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, row);
    }
    return 1;
}
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
int l_git_reset_hunk_native(lua_State *L) {
    const char *base = luaL_checkstring(L, 1);
    GetEditor(L)->GitResetHunk(base);
    return 0;
}

// mep.git_stage_hunk(): see Editor::GitStageHunk. Bound directly, no wrapper.
int l_git_stage_hunk(lua_State *L) {
    GetEditor(L)->GitStageHunk();
    return 0;
}

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
std::string BabelCachePath() { return MepDataDir() + "/babel_cache.json"; }
}  // namespace
#endif

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

std::string LspFrame(const Json &msg) {
    std::string body = msg.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

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

int l_lsp_start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> argv;
    lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 1));
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv.push_back(luaL_checkstring(L, -1));
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

int l_lsp_stop(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    JobManager::Instance().Kill(client_id);
    g_lsp_clients.erase(client_id);
    return 0;
}

int l_lsp_is_running(lua_State *L) {
    int client_id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, JobManager::Instance().IsRunning(client_id));
    return 1;
}

// mep.set_completion_source(fn): fn(prefix) -> array of candidate words
// (Phase 22).
int l_set_completion_source(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->SetCompletionSourceRef(ref);
    return 0;
}

// mep.set_completion_accept_hook(fn): see SetCompletionAcceptHookRef's
// comment (editor.h) -- Phase 23's LSP insertTextFormat=Snippet wiring.
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
    Editor *ed = GetEditor(L);
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
        a.push_back(luaL_optstring(L, -1, ""));
        lua_pop(L, 1);
    }
    lua_Integer nb = static_cast<lua_Integer>(lua_rawlen(L, 2));
    for (lua_Integer i = 1; i <= nb; i++) {
        lua_rawgeti(L, 2, i);
        b.push_back(luaL_optstring(L, -1, ""));
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
        if (lua_isstring(L_, -1)) out->push_back(lua_tostring(L_, -1));
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
            texts->push_back(lua_tostring(L_, -1));
            kinds->push_back("");
            details->push_back("");
            docs->push_back("");
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
            out->push_back({text, hl});
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
