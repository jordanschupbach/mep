#include "lua_env.h"

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

#include "editor.h"
#include "job.h"
#include "treesitter.h"
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

int l_insert_text(lua_State *L) {
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    GetEditor(L)->InsertTextForLua(std::string(s, len));
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
// mod1+Shift+letter / mod1+Ctrl+letter. Overrides any prior mapping for
// that exact key, including the startup defaults.
int l_map_mod1(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    GetEditor(L)->RegisterMod1Mapping(key, ref);
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
    lua_getfield(L, idx, "sign");
    if (lua_isstring(L, -1)) d.sign = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, idx, "sign_hl");
    if (lua_isstring(L, -1)) d.sign_hl = lua_tostring(L, -1);
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
    int id = JobManager::Instance().Spawn(argv, cwd, std::move(cb));
    if (id != 0) g_lsp_clients[id] = state;
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

// Myers diff (NVIM_PARITY_PLAN.md Part IV Phase 17): the equivalent of
// Neovim's built-in vim.diff(), needed so git-gutter hunks don't have to
// shell `git diff` per keystroke -- everything else git-specific (gutter
// signs, staging, patch generation, the status sidebar) is orchestrated in
// Lua (kBuiltinGit, main.cpp) on top of this one C++ primitive.
struct DiffHunk {
    int old_start, old_count, new_start, new_count;
};

// Classic O(ND) Myers diff (Myers 1986), operating on opaque line indices
// via equality only -- returns the *edit script* as a sequence of (line
// present only in `a`) / (line present only in `b`) markers, which the
// caller (BuildHunks) coalesces into contiguous hunks.
std::vector<DiffHunk> MyersDiffHunks(const std::vector<std::string> &a, const std::vector<std::string> &b) {
    int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    int max_d = n + m;
    if (max_d == 0) return {};
    // trace[d] stores the V array (x-coordinates of furthest-reaching D-paths
    // for each diagonal k) at step d, needed to walk the path back afterward.
    std::vector<std::vector<int>> trace;
    std::vector<int> v(2 * max_d + 1, 0);
    auto vidx = [max_d](int k) { return k + max_d; };
    int found_d = -1;
    for (int d = 0; d <= max_d; d++) {
        trace.push_back(v);
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[vidx(k - 1)] < v[vidx(k + 1)])) {
                x = v[vidx(k + 1)];
            } else {
                x = v[vidx(k - 1)] + 1;
            }
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) {
                x++;
                y++;
            }
            v[vidx(k)] = x;
            if (x >= n && y >= m) {
                found_d = d;
                break;
            }
        }
        if (found_d >= 0) break;
    }

    // Walk the recorded traces backward from (n,m) to (0,0), emitting
    // per-line ops, then coalesce contiguous runs into hunks below.
    struct Op {
        char kind;  // '=' / '-' (only in a) / '+' (only in b)
        int a_line, b_line;
    };
    std::vector<Op> ops;
    int x = n, y = m;
    for (int d = found_d; d > 0; d--) {
        const std::vector<int> &vd = trace[d];
        int k = x - y;
        int prev_k = (k == -d || (k != d && vd[vidx(k - 1)] < vd[vidx(k + 1)])) ? k + 1 : k - 1;
        int prev_x = vd[vidx(prev_k)];
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            ops.push_back({'=', x - 1, y - 1});
            x--;
            y--;
        }
        if (x == prev_x) {
            ops.push_back({'+', -1, y - 1});
            y--;
        } else {
            ops.push_back({'-', x - 1, -1});
            x--;
        }
    }
    while (x > 0 && y > 0) {
        ops.push_back({'=', x - 1, y - 1});
        x--;
        y--;
    }
    std::reverse(ops.begin(), ops.end());

    std::vector<DiffHunk> hunks;
    size_t i = 0;
    int a_pos = 0, b_pos = 0;  // 0-indexed count of `a`/`b` lines consumed so far
    while (i < ops.size()) {
        if (ops[i].kind == '=') {
            a_pos++;
            b_pos++;
            i++;
            continue;
        }
        // A pure insertion has no `a` anchor of its own -- gitsigns
        // convention: report it at the line *after* which it was inserted
        // (0 if at the very top), i.e. `a_pos` (0-indexed) before the hunk.
        int old_start = a_pos, new_start = b_pos;
        int old_count = 0, new_count = 0;
        while (i < ops.size() && ops[i].kind != '=') {
            if (ops[i].kind == '-') {
                old_count++;
                a_pos++;
            } else {
                new_count++;
                b_pos++;
            }
            i++;
        }
        hunks.push_back({old_start + 1, old_count, new_start + 1, new_count});
    }
    return hunks;
}

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

// mep.icon_for_file(name) -> a short ASCII glyph (Phase 10).
int l_icon_for_file(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_pushstring(L, IconForFilename(name).c_str());
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
    {"insert_text", l_insert_text},
    {"notify", l_notify},
    {"command", l_command},
    {"map", l_map},
    {"mapping_descriptions", l_mapping_descriptions},
    {"leader_bindings", l_leader_bindings},
    {"map_mod1", l_map_mod1},
    {"set_mod1", l_set_mod1},
    {"nav_pane", l_nav_pane},
    {"resize_pane", l_resize_pane},
    {"pane_set_share", l_pane_set_share},
    {"cmd", l_cmd},
    {"terminal_here", l_terminal_here},
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
    {"ns_create", l_ns_create},
    {"ns_clear", l_ns_clear},
    {"deco_add", l_deco_add},
    {"ts_captures", l_ts_captures},
    {"ts_fold_ranges", l_ts_fold_ranges},
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
    {"command_names", l_command_names},
    {"colorscheme", l_colorscheme},
    {"theme_names", l_theme_names},
    {"current_theme", l_current_theme},
    {"icon_for_file", l_icon_for_file},
    {"set_leader", l_set_leader},
    {"leader_map", l_leader_map},
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
    {"set_completion_source", l_set_completion_source},
    {"set_completion_accept_hook", l_set_completion_accept_hook},
    {"set_insert_tab_hook", l_set_insert_tab_hook},
    {"lsp_start", l_lsp_start},
    {"lsp_request", l_lsp_request},
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
