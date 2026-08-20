#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "editor.h"
#include "font_data.h"
#include "job.h"
#include "lua_env.h"
#include "vterm.h"

namespace {

constexpr int kInitialWidth = 1000;
constexpr int kInitialHeight = 650;
constexpr float kDefaultFontSize = 33.75f;
constexpr float kMinFontSize = 8.0f;
constexpr float kMaxFontSize = 48.0f;
constexpr float kFontSizeStep = 2.0f;
constexpr int kMarginX = 8;
constexpr int kMenuPaddingX = 14;
constexpr int kMenuItemPaddingX = 16;

Editor g_editor;
Font g_font;
float g_font_size = kDefaultFontSize;
float g_char_width = 0;

struct MenuItem {
    std::string label;
    std::function<void()> action;
};
struct Menu {
    std::string label;
    std::vector<MenuItem> items;
};

std::vector<Menu> g_menus;
// Cached output of ComputeMenuLabelLayout() below -- the menu bar never
// changes after BuildMenus() (except when the font size does), so this was
// pure waste recomputed via MeasureTextEx from scratch, with a fresh heap
// allocation, twice a frame (HandleMenuInput() and DrawMenuBar() each did
// their own): once when checking for a menu-bar click, again when drawing
// it. Small relative to the per-character text-rendering cost fixed
// alongside this, but free to cut once noticed.
std::vector<float> g_menu_starts, g_menu_widths;
void RecomputeMenuLabelLayout();  // defined below; also called from HandleFontSizeShortcuts()
int g_open_menu = -1;
bool g_show_help_overlay = false;
std::string g_help_overlay_text;

int LineHeight() { return static_cast<int>(g_font_size) + 6; }
int MenuBarHeight() { return static_cast<int>(g_font_size) + 12; }
int MenuItemHeight() { return static_cast<int>(g_font_size) + 8; }
float MenuFontSize() { return std::max(kMinFontSize, g_font_size - 2); }
int TabBarHeight() { return static_cast<int>(MenuFontSize()) + 10; }
int PaneHeaderHeight() { return static_cast<int>(MenuFontSize()) + 8; }

// Command mode and the two search modes all show a text-editing line in
// the command bar instead of the buffer's own blinking cursor, so this is
// checked at both those call sites below rather than listing all three
// modes twice.
bool IsCommandLineMode(Mode m) {
    return m == Mode::Command || m == Mode::SearchForward || m == Mode::SearchBackward;
}

Color ToRaylib(ThemeColor c) { return Color{c.r, c.g, c.b, c.a}; }

// Resolves a highlight-group name to a color via the active theme (Phase 9,
// NVIM_PARITY_PLAN.md Part II): an exact lookup into the theme's built group
// map first (chrome call sites below all use exact names like "StatusLine"),
// falling back to a substring heuristic sourced from the same theme's base
// role colors for un-migrated decoration hl_group names (e.g. "MepGitAdd")
// that don't match a group 1:1.
Color ResolveHlGroup(const std::string &name) {
    auto get = [](const char *group, ThemeColor fallback) {
        ThemeColor v;
        return g_editor.ResolveHighlight(group, &v) ? v : fallback;
    };
    if (name.empty()) return ToRaylib(get("Normal", {211, 211, 211, 255}));
    ThemeColor c;
    if (g_editor.ResolveHighlight(name, &c)) return ToRaylib(c);
    auto contains = [&](const char *s) { return name.find(s) != std::string::npos; };
    if (contains("Error") || contains("Delete") || contains("Red")) return ToRaylib(get("Red", {224, 108, 117, 255}));
    if (contains("Warn") || contains("Yellow")) return ToRaylib(get("Yellow", {229, 192, 123, 255}));
    if (contains("Add") || contains("Green")) return ToRaylib(get("Green", {152, 195, 121, 255}));
    if (contains("Info") || contains("Blue") || contains("Hint")) return ToRaylib(get("Blue", {97, 175, 239, 255}));
    if (contains("Purple") || contains("Magenta")) return ToRaylib(get("Purple", {198, 120, 221, 255}));
    if (contains("Cyan")) return ToRaylib(get("Cyan", {86, 182, 194, 255}));
    if (contains("Comment") || contains("Gray") || contains("Grey")) return ToRaylib(get("Border", {130, 130, 135, 255}));
    return ToRaylib(get("Normal", {200, 200, 200, 255}));
}

const char *ModeName(Mode m, bool replace_mode = false) {
    switch (m) {
        case Mode::Normal: return "NORMAL";
        case Mode::Insert: return replace_mode ? "REPLACE" : "INSERT";
        case Mode::Visual: return "VISUAL";
        case Mode::VisualLine: return "V-LINE";
        case Mode::VisualBlock: return "V-BLOCK";
        case Mode::Command: return "COMMAND";
        case Mode::SearchForward:
        case Mode::SearchBackward:
            return "SEARCH";
        case Mode::Prompt: return "INPUT";
        case Mode::Confirm: return "CONFIRM";
        case Mode::Select: return "SELECT";
        case Mode::Sidebar: return "SIDEBAR";
        case Mode::Picker: return "PICKER";
        case Mode::WhichKey: return "WHICHKEY";
        case Mode::HintChar:
        case Mode::HintLabel:
            return "HINT";
        case Mode::Terminal: return "TERMINAL";
    }
    return "?";
}

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

// raylib's GetGlyphIndex(font, codepoint) -- called by DrawTextEx once and
// then again inside DrawTextCodepoint, so twice per character drawn -- is a
// linear scan over every loaded glyph (95 of them here: the default ASCII
// 32..126 set LoadFontFromMemory falls back to when given no explicit
// codepoint list). Paid on every visible character of every line, every
// frame, that's the single largest per-frame cost in the app once a pane
// has any real amount of text in it, and it's what made typing feel
// laggy: the editor was never more than a frame behind a keystroke, but
// frames themselves were slow enough (compounded by the software-rendered
// GL launcher/serve.ts forces WebKitGTK into) that bursts of fast typing
// visibly fell behind. Cached once here into a flat array so the hot
// per-character draw path (DrawLineFast below) is an O(1) lookup instead.
int g_glyph_index[95] = {};

void CacheGlyphIndices() {
    for (int c = 32; c <= 126; c++) g_glyph_index[c - 32] = GetGlyphIndex(g_font, c);
}

// Reloads the font at 2x the target draw size (supersampled for crisper
// scaling) and recomputes the monospace character width used for layout.
void ApplyFontSize(float size) {
    g_font_size = std::max(kMinFontSize, std::min(size, kMaxFontSize));
    if (g_font.texture.id != 0) UnloadFont(g_font);
    g_font = LoadFontFromMemory(".ttf", kJetBrainsMonoRegularTtf,
                                 static_cast<int>(kJetBrainsMonoRegularTtfLen),
                                 static_cast<int>(g_font_size * 2), nullptr, 0);
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
    g_char_width = MeasureTextEx(g_font, "M", g_font_size, 0).x;
    CacheGlyphIndices();
}

// Draws one line of monospace text, advancing by the app's own fixed
// g_char_width per column -- already what every other piece of layout
// (cursor position, selection highlight rectangles) assumes -- rather
// than each glyph's individual advanceX, and using the index cache above
// instead of raylib's per-call GetGlyphIndex scan. Mirrors raylib's own
// DrawTextEx/DrawTextCodepoint (rtext.c) for the actual quad math, just
// without the repeated lookup; falls back to the (slower but correct)
// DrawTextCodepoint for anything outside the cached ASCII range, e.g. a
// non-ASCII byte in a loaded file -- InsertChar only ever produces 32..126
// itself, but file content read from disk isn't bound by that.
void DrawLineFast(const std::string &line, float x, float y, float font_size, Color tint) {
    // Decodes by Unicode codepoint (GetCodepointNext), same as raylib's own
    // DrawTextEx -- not by byte -- so a multi-byte UTF-8 character (never
    // produced by InsertChar itself, but loaded file content isn't limited
    // to ASCII) advances the cursor column once, not once per byte, and
    // draws as the one glyph it is rather than one garbled glyph per byte.
    float scale = font_size / g_font.baseSize;
    float pad = static_cast<float>(g_font.glyphPadding);
    int col = 0;
    const char *text = line.c_str();
    int byte_len = static_cast<int>(line.size());
    for (int i = 0; i < byte_len;) {
        int codepoint_size = 0;
        int codepoint = GetCodepointNext(&text[i], &codepoint_size);
        i += codepoint_size;
        float cx = x + static_cast<float>(col) * g_char_width;
        col++;
        if (codepoint == ' ' || codepoint == '\t') continue;
        if (codepoint < 32 || codepoint > 126) {
            DrawTextCodepoint(g_font, codepoint, Vector2{cx, y}, font_size, tint);
            continue;
        }
        int index = g_glyph_index[codepoint - 32];
        Rectangle dst{cx + g_font.glyphs[index].offsetX * scale - pad * scale,
                      y + g_font.glyphs[index].offsetY * scale - pad * scale,
                      (g_font.recs[index].width + 2.0f * pad) * scale,
                      (g_font.recs[index].height + 2.0f * pad) * scale};
        Rectangle src{g_font.recs[index].x - pad, g_font.recs[index].y - pad,
                      g_font.recs[index].width + 2.0f * pad, g_font.recs[index].height + 2.0f * pad};
        DrawTexturePro(g_font.texture, src, dst, Vector2{0, 0}, 0.0f, tint);
    }
}

void HandleFontSizeShortcuts() {
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (!ctrl || !shift) return;
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressedRepeat(KEY_EQUAL)) {
        ApplyFontSize(g_font_size + kFontSizeStep);
        RecomputeMenuLabelLayout();
    } else if (IsKeyPressed(KEY_MINUS) || IsKeyPressedRepeat(KEY_MINUS)) {
        ApplyFontSize(g_font_size - kFontSizeStep);
        RecomputeMenuLabelLayout();
    }
}

const char *kKeybindingsText =
    "mep -- keybindings\n"
    "\n"
    "Normal mode\n"
    "  h j k l         move\n"
    "  0  $            line start / end\n"
    "  gg  G           buffer start / end\n"
    "  w  b            word forward / back\n"
    "  i a I A o O     enter Insert\n"
    "  d y c           operator (+motion, or doubled: dd yy cc)\n"
    "  x               delete char\n"
    "  p  P            paste after / before\n"
    "  u  Ctrl-r       undo / redo\n"
    "  v  V            Visual / Visual-Line\n"
    "  :               command line\n"
    "\n"
    "Command line\n"
    "  :w [file]  :wa   :q  :q!  :wq   :e file  :<N>\n"
    "  :qa  :qa!  :wqa                write/quit across all buffers\n"
    "  :lua <code>      run Lua directly\n"
    "  :source <file>   run a Lua script (native only)\n"
    "\n"
    "Windows & tabs\n"
    "  :split [file]  :vsplit [file]   split pane (horizontal / vertical)\n"
    "  :close                         close current pane\n"
    "  Ctrl-W w / W                   cycle to next / previous pane\n"
    "  Ctrl-W c / s / v               close / split-h / split-v pane\n"
    "  Ctrl-W h j k l                 move focus left / down / up / right\n"
    "  :tabnew [file]  :tabdelete     new / close tab\n"
    "  :tabnext  :tabprevious         switch tabs\n"
    "\n"
    "  Alt+s / Alt+v                  split (horizontal / vertical)\n"
    "  Alt+h j k l                    move focus left / down / up / right\n"
    "  Alt+Shift+h j k l              resize pane left / down / up / right\n"
    "  Alt+Ctrl+h j k l               move active tab into pane that way\n"
    "  Alt+d                          remove active tab (closes pane if last)\n"
    "  Alt+n / Alt+Tab                next buffer tab in pane\n"
    "  Alt+p / Alt+Shift+Tab          previous buffer tab in pane\n"
    "\n"
    "Global\n"
    "  Ctrl+Shift+=/-   grow / shrink font\n"
    "  Alt is mod1 by default -- rebind with mep.set_mod1() in init.lua";

// Bound via mep.map_mod1() rather than hardcoded, so init.lua can freely
// override any of these (mep.map_mod1 last-write-wins per key) -- see
// ConfigFilePath()/main() for load order. Mirrors mep.nvim's
// mep.window.panes manual keymaps (window/config.lua): mod1+hjkl focus,
// mod1+Shift+hjkl resize, mod1+Ctrl+hjkl move-tab-to-neighbor, mod1+d
// remove the active tab (closing the pane once its last tab is gone),
// mod1+n/mod1+Tab and mod1+p/mod1+Shift+Tab cycle the active pane's buffer
// tab list.
const char *kDefaultMod1Bindings =
    "mep.map_mod1('s', function() mep.cmd('split') end)\n"
    "mep.map_mod1('v', function() mep.cmd('vsplit') end)\n"
    "mep.map_mod1('h', function() mep.nav_pane('left') end)\n"
    "mep.map_mod1('j', function() mep.nav_pane('down') end)\n"
    "mep.map_mod1('k', function() mep.nav_pane('up') end)\n"
    "mep.map_mod1('l', function() mep.nav_pane('right') end)\n"
    "mep.map_mod1('S-h', function() mep.resize_pane('left') end)\n"
    "mep.map_mod1('S-j', function() mep.resize_pane('down') end)\n"
    "mep.map_mod1('S-k', function() mep.resize_pane('up') end)\n"
    "mep.map_mod1('S-l', function() mep.resize_pane('right') end)\n"
    "mep.map_mod1('C-h', function() mep.pane_move_buffer('left') end)\n"
    "mep.map_mod1('C-j', function() mep.pane_move_buffer('down') end)\n"
    "mep.map_mod1('C-k', function() mep.pane_move_buffer('up') end)\n"
    "mep.map_mod1('C-l', function() mep.pane_move_buffer('right') end)\n"
    "mep.map_mod1('d', function() mep.pane_close_buffer() end)\n"
    "mep.map_mod1('n', function() mep.pane_next_buffer() end)\n"
    "mep.map_mod1('p', function() mep.pane_prev_buffer() end)\n"
    "mep.map_mod1('Tab', function() mep.pane_next_buffer() end)\n"
    "mep.map_mod1('S-Tab', function() mep.pane_prev_buffer() end)\n";

// Debounced buffer-changed/buffer-saved consumer helpers, built on top of
// mep.on_frame + mep.buffer_change_epoch()/buffer_save_epoch() (new C++
// primitives -- see LuaEnv::RunFrameHooks in lua_env.h for why this is
// polling rather than a synchronous edit-time callback). This is the
// root-cause fix for the "not debounced/automatic, on-demand only" scope
// cut repeated across several earlier phases (colorizer, git gutter,
// todoscan live-marking, symbols outline refresh-on-save, file-tree cache
// invalidation) -- each of those phases now just calls
// mep.on_buffer_changed/mep.on_buffer_saved instead of staying on-demand.
// Defined first so every later kBuiltinXxx chunk can call it.
const char *kBuiltinEditHooks =
    "function mep.on_buffer_changed(fn, interval_sec)\n"
    "  interval_sec = interval_sec or 0.3\n"
    "  local last_epoch, last_run = -1, 0\n"
    "  mep.on_frame(function()\n"
    "    local epoch = mep.buffer_change_epoch()\n"
    "    if epoch ~= last_epoch then\n"
    "      local now = mep.now()\n"
    "      if now - last_run >= interval_sec then\n"
    "        last_epoch, last_run = epoch, now\n"
    "        fn()\n"
    "      end\n"
    "    end\n"
    "  end)\n"
    "end\n"
    "function mep.on_buffer_saved(fn)\n"
    "  local last_epoch = -1\n"
    "  mep.on_frame(function()\n"
    "    local epoch = mep.buffer_save_epoch()\n"
    "    if epoch ~= last_epoch then\n"
    "      last_epoch = epoch\n"
    "      fn()\n"
    "    end\n"
    "  end)\n"
    "end\n";

// Built-in picker sources (find files / buffers / commands), ported from
// mep.nvim's picker source set (Phase 8). Defined in Lua rather than C++
// so they're just ordinary `mep.picker_open()` consumers -- user config
// can override any of these functions the same way it overrides mappings.
// Directory/tree/UI-action icon set (Phase 10) -- plain ASCII, same
// rendering-support-check rationale as IconForFilename (editor.cpp).
// Per-file icons go through mep.icon_for_file() (extension table lives in
// C++); this small fixed set doesn't need a lookup table, just constants.
const char *kBuiltinIcons =
    "mep.icons = {\n"
    "  dir_open = 'v', dir_closed = '>', tree_expand = '>', tree_collapse = 'v',\n"
    "  notify = 'i', todo = 'o', tests = 'T', git = 'G', add = '+', clear = 'x',\n"
    "}\n";

// Colorizer + URL detection/open (Phase 13). Both are plain-Lua consumers
// of existing primitives (decorations for swatches, jobs for opening a
// URL) -- no new C++ needed beyond the swatch-rendering support in
// Decoration/DrawPane and mep.platform() for picking xdg-open/open/start.
const char *kBuiltinTextTools =
    "local mep_colorizer_ns = nil\n"
    "function mep.colorize()\n"
    "  if not mep_colorizer_ns then mep_colorizer_ns = mep.ns_create('colorizer') end\n"
    "  mep.ns_clear(mep_colorizer_ns)\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    local covered = {}\n"
    "    for s, hex in line:gmatch('()#(%x%x%x%x%x%x%x%x)') do\n"
    "      local r, g, b = tonumber(hex:sub(1,2),16), tonumber(hex:sub(3,4),16), tonumber(hex:sub(5,6),16)\n"
    "      mep.deco_add(mep_colorizer_ns, {row=i, col_start=s, col_end=s+9, color={r,g,b}})\n"
    "      for k = s, s + 8 do covered[k] = true end\n"
    "    end\n"
    "    for s, hex in line:gmatch('()#(%x%x%x%x%x%x)') do\n"
    "      if not covered[s] then\n"
    "        local r, g, b = tonumber(hex:sub(1,2),16), tonumber(hex:sub(3,4),16), tonumber(hex:sub(5,6),16)\n"
    "        mep.deco_add(mep_colorizer_ns, {row=i, col_start=s, col_end=s+7, color={r,g,b}})\n"
    "        for k = s, s + 6 do covered[k] = true end\n"
    "      end\n"
    "    end\n"
    "    for s, hex in line:gmatch('()#(%x%x%x)') do\n"
    "      if not covered[s] then\n"
    "        local r = tonumber(hex:sub(1,1) .. hex:sub(1,1), 16)\n"
    "        local g = tonumber(hex:sub(2,2) .. hex:sub(2,2), 16)\n"
    "        local b = tonumber(hex:sub(3,3) .. hex:sub(3,3), 16)\n"
    "        mep.deco_add(mep_colorizer_ns, {row=i, col_start=s, col_end=s+4, color={r,g,b}})\n"
    "      end\n"
    "    end\n"
    "    for s, r, g, b in line:gmatch('()rgba?%(%s*(%d+)%s*,%s*(%d+)%s*,%s*(%d+)') do\n"
    "      mep.deco_add(mep_colorizer_ns, {row=i, col_start=s, col_end=s+3, color={tonumber(r),tonumber(g),tonumber(b)}})\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepColorize', mep.colorize)\n"
    // Opt-in auto-recompute (off by default -- a silent always-on
    // behavior change would surprise existing configs/large files);
    // :lua mep.colorize_auto = true to enable.
    "mep.colorize_auto = false\n"
    "mep.on_buffer_changed(function() if mep.colorize_auto then mep.colorize() end end)\n"
    "local MEP_URL_PATTERN = \"https?://[%w%-%._~:/?#%[%]@!$&'()*+,;=%%]+\"\n"
    "function mep.url_under_cursor()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local init = 1\n"
    "  while true do\n"
    "    local s, e = line:find(MEP_URL_PATTERN, init)\n"
    "    if not s then return nil end\n"
    "    if col >= s and col <= e then return line:sub(s, e) end\n"
    "    init = e + 1\n"
    "  end\n"
    "end\n"
    "function mep.open_url(url)\n"
    "  local plat = mep.platform()\n"
    "  local cmd = 'xdg-open'\n"
    "  if plat == 'macos' then cmd = 'open' elseif plat == 'windows' then cmd = 'start' end\n"
    "  mep.job_start({cmd, url})\n"
    "end\n"
    "function mep.open_url_under_cursor()\n"
    "  local url = mep.url_under_cursor()\n"
    "  if url then mep.open_url(url); mep.notify('Opening ' .. url)\n"
    "  else mep.notify('No URL under cursor', 'warn') end\n"
    "end\n"
    "function mep.list_urls()\n"
    "  local items = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    local init = 1\n"
    "    while true do\n"
    "      local s, e = line:find(MEP_URL_PATTERN, init)\n"
    "      if not s then break end\n"
    "      items[#items + 1] = line:sub(s, e)\n"
    "      init = e + 1\n"
    "    end\n"
    "  end\n"
    "  mep.picker_open('URLs', items, function(item)\n"
    "    if item then mep.open_url(item) end\n"
    "  end)\n"
    "end\n";

// File tree sidebar (Phase 15): built entirely in Lua atop the Phase 7
// sidebar widget (mep.sidebar_*), Phase 10 icons, and the new
// mep.list_dir/fs_* primitives -- the generic sidebar stays feature-free,
// tree-specific behavior (expand/collapse/create/rename/delete/refresh/
// toggle-hidden) all lives here via sidebar_set_on_key.
const char *kBuiltinFileTree =
    "local mep_tree_root = nil\n"
    "local mep_tree_expanded = {}\n"
    "local mep_tree_sidebar_id = nil\n"
    "local mep_tree_show_hidden = false\n"
    "local mep_tree_ignored = {}\n"
    "local function mep_tree_join(dir, name)\n"
    "  if dir:sub(-1) == '/' then return dir .. name end\n"
    "  return dir .. '/' .. name\n"
    "end\n"
    "local function mep_tree_refresh_ignored()\n"
    "  mep_tree_ignored = {}\n"
    "  mep.job_start({'git', '-C', mep_tree_root, 'status', '--ignored', '--porcelain'}, {\n"
    "    on_stdout = function(line)\n"
    "      if line:sub(1,3) == '!! ' then mep_tree_ignored[line:sub(4)] = true end\n"
    "    end,\n"
    "    on_exit = function() mep.tree_refresh() end,\n"
    "  })\n"
    "end\n"
    "local function mep_tree_build_widgets(dir, depth, widgets)\n"
    "  local entries = mep.list_dir(dir)\n"
    "  for _, e in ipairs(entries) do\n"
    "    local hidden = e.name:sub(1,1) == '.'\n"
    "    local rel = dir:sub(#mep_tree_root + 2) \n"
    "    local relpath = (rel ~= '' and (rel .. '/') or '') .. e.name\n"
    "    if (mep_tree_show_hidden or not hidden) and not mep_tree_ignored[relpath] then\n"
    "      local full = mep_tree_join(dir, e.name)\n"
    "      local indent = string.rep('  ', depth)\n"
    "      if e.is_dir then\n"
    "        local marker = mep_tree_expanded[full] and mep.icons.dir_open or mep.icons.dir_closed\n"
    "        widgets[#widgets + 1] = {\n"
    "          id = full, text = indent .. marker .. ' ' .. e.name,\n"
    "          on_click = function()\n"
    "            if mep_tree_expanded[full] then mep_tree_expanded[full] = nil\n"
    "            else mep_tree_expanded[full] = true end\n"
    "            mep.tree_refresh()\n"
    "          end,\n"
    "        }\n"
    "        if mep_tree_expanded[full] then mep_tree_build_widgets(full, depth + 1, widgets) end\n"
    "      else\n"
    "        widgets[#widgets + 1] = {\n"
    "          id = full, text = indent .. mep.icon_for_file(e.name) .. ' ' .. e.name,\n"
    "          on_click = function() mep.cmd('e ' .. full) end,\n"
    "        }\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "function mep.tree_refresh()\n"
    "  if not mep_tree_root then return end\n"
    "  if not mep_tree_sidebar_id then\n"
    "    mep_tree_sidebar_id = mep.sidebar_create('Files', 'left', mep.sidebar_default_cols(0.20))\n"
    "    mep.sidebar_set_on_key(mep_tree_sidebar_id, mep.tree_on_key)\n"
    "  end\n"
    "  local widgets = {}\n"
    "  mep_tree_build_widgets(mep_tree_root, 0, widgets)\n"
    "  mep.sidebar_set_sections(mep_tree_sidebar_id, {\n"
    "    {id = 'tree', title = mep_tree_root, collapsed = false, widgets = widgets},\n"
    "  })\n"
    "end\n"
    "function mep.tree_open(dir)\n"
    "  mep_tree_root = dir or '.'\n"
    "  mep_tree_expanded[mep_tree_root] = true\n"
    "  mep.tree_refresh()\n"
    "  mep.sidebar_open(mep_tree_sidebar_id)\n"
    "  mep_tree_refresh_ignored()\n"
    "end\n"
    "function mep.tree_toggle()\n"
    "  if not mep_tree_sidebar_id then mep.tree_open('.') return end\n"
    "  mep.sidebar_toggle(mep_tree_sidebar_id)\n"
    "end\n"
    "function mep.tree_on_key(k)\n"
    "  local target = mep.sidebar_cursor_widget_id(mep_tree_sidebar_id)\n"
    "  if k == 'R' then\n"
    "    mep.tree_refresh(); mep_tree_refresh_ignored()\n"
    "  elseif k == 'H' then\n"
    "    mep_tree_show_hidden = not mep_tree_show_hidden; mep.tree_refresh()\n"
    "  elseif k == 'o' and target then\n"
    "    mep.open_url('file://' .. target)\n"
    "  elseif k == 'a' then\n"
    "    local base = target or mep_tree_root\n"
    "    mep.ui_input('New file/dir (end with / for dir):', '', function(name)\n"
    "      if not name or name == '' then return end\n"
    "      local full = mep_tree_join(base, name)\n"
    "      if name:sub(-1) == '/' then mep.fs_mkdir(full:sub(1, -2)) else mep.fs_create_file(full) end\n"
    "      mep.tree_refresh()\n"
    "    end)\n"
    "  elseif k == 'r' and target then\n"
    "    mep.ui_input('Rename to:', target, function(name)\n"
    "      if not name or name == '' then return end\n"
    "      mep.fs_rename(target, name)\n"
    "      mep.tree_refresh()\n"
    "    end)\n"
    "  elseif k == 'd' and target then\n"
    "    mep.ui_confirm('Delete ' .. target .. '?', false, function(yes)\n"
    "      if yes then mep.fs_delete(target); mep.tree_refresh() end\n"
    "    end)\n"
    "  elseif k == '?' then\n"
    "    mep.notify('Files: Enter=open/toggle  a=create  r=rename  d=delete  R=refresh  H=hidden  o=open-with-OS')\n"
    "  end\n"
    "end\n"
    "mep.command('MepFileTree', function() mep.tree_toggle() end)\n"
    "mep.leader_map('ff', 'Toggle file tree', function() mep.tree_toggle() end)\n"
    "local MEP_README_NAMES = {'README.md', 'README.org', 'README.txt', 'README'}\n"
    "function mep.project_open(dir)\n"
    "  mep.chdir(dir)\n"
    "  local opened_readme = false\n"
    "  for _, name in ipairs(MEP_README_NAMES) do\n"
    "    local entries = mep.list_dir(dir)\n"
    "    for _, e in ipairs(entries) do\n"
    "      if not e.is_dir and e.name == name then\n"
    "        mep.cmd('e ' .. dir .. '/' .. name)\n"
    "        opened_readme = true\n"
    "        break\n"
    "      end\n"
    "    end\n"
    "    if opened_readme then break end\n"
    "  end\n"
    // A bare `:terminal` always opens its new pane above/left of whatever
    // was focused (vim's default split direction) -- so to land the
    // terminal *below* the readme, split first (the readme's own pane
    // duplicates upward and keeps focus) then drop into the pane pushed
    // down to the bottom and turn it into a terminal in place.
    "  if opened_readme then\n"
    "    mep.cmd('split')\n"
    "    mep.nav_pane('down')\n"
    "    mep.terminal_here()\n"
    "    mep.pane_set_share(1/3)\n"
    // Back to the readme pane (also drops Mode::Terminal -- see
    // NavigatePaneDirection) before the tree steals focus below, so the
    // final nav_pane('right') has the right pane (not the terminal) to
    // land back on.
    "    mep.nav_pane('up')\n"
    "  end\n"
    "  mep.tree_open(dir)\n"
    // mep.tree_open focuses the sidebar (matches tree_toggle's open-means-
    // focus behavior); step focus back out into the pane tree so the user
    // lands with the cursor in the readme, not the file list.
    "  if opened_readme then mep.nav_pane('right') end\n"
    "  mep.notify('Opened project: ' .. dir)\n"
    "end\n"
    "local function mep_project_basename(p)\n"
    "  local trimmed = p:gsub('/+$', '')\n"
    "  if trimmed == '' then return p end\n"
    "  return trimmed:match('([^/]+)$') or p\n"
    "end\n"
    "function mep.projects_remove_picker()\n"
    "  local items = {}\n"
    "  for _, p in ipairs(mep.project_list()) do\n"
    "    items[#items + 1] = {display = mep_project_basename(p), data = p}\n"
    "  end\n"
    "  mep.picker_open('Remove project', items, function(item)\n"
    "    if item then mep.project_remove(item); mep.notify('Removed project: ' .. item) end\n"
    "  end)\n"
    "end\n"
    "function mep.projects()\n"
    "  local items = {}\n"
    "  for _, p in ipairs(mep.project_list()) do\n"
    "    items[#items + 1] = {display = mep_project_basename(p), data = p}\n"
    "  end\n"
    "  items[#items + 1] = '+ Add current directory'\n"
    "  items[#items + 1] = '- Remove a project...'\n"
    "  local function add_current()\n"
    "    mep.project_add('.')\n"
    "    mep.notify('Added current directory as a project')\n"
    "  end\n"
    "  mep.picker_open('Projects', items, function(item)\n"
    "    if not item then return end\n"
    "    if item == '+ Add current directory' then\n"
    "      add_current()\n"
    "    elseif item == '- Remove a project...' then\n"
    "      mep.projects_remove_picker()\n"
    "    else\n"
    "      mep.project_open(item)\n"
    "    end\n"
    "  end, nil, function(key)\n"
    "    if key == 'a' then\n"
    "      add_current()\n"
    "      mep.picker_close()\n"
    "    end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepProjects', mep.projects)\n"
    "mep.command('MepProjectAdd', function() mep.project_add('.') end)\n"
    "mep.leader_map('po', 'Projects', mep.projects)\n";

// Git integration (Phase 17): gutter hunks (built on mep.diff_lines, the
// Myers-diff C++ primitive) + hunk nav/stage/reset + a status sidebar.
// All git-specific logic lives here in Lua; the only new C++ underneath is
// the diff algorithm itself and mep.replace_lines (a generic multi-line
// splice, not git-specific).
const char *kBuiltinGit =
    "local mep_git_ns = nil\n"
    "local mep_git_hunks = {}\n"
    "local mep_git_base_lines = {}\n"
    "local mep_git_status_sidebar_id = nil\n"
    "function mep.git_gutter_refresh()\n"
    "  local fname = mep.filename()\n"
    "  if fname == '' then return end\n"
    "  if not mep_git_ns then mep_git_ns = mep.ns_create('git') end\n"
    "  local lines = {}\n"
    "  mep.job_start({'git', 'show', 'HEAD:' .. fname}, {\n"
    "    on_stdout = function(l) lines[#lines + 1] = l end,\n"
    "    on_exit = function(code)\n"
    "      mep_git_base_lines = lines\n"
    "      local cur = {}\n"
    "      for i = 1, mep.line_count() do cur[i] = mep.get_line(i) end\n"
    "      mep_git_hunks = mep.diff_lines(lines, cur)\n"
    "      mep.ns_clear(mep_git_ns)\n"
    "      for _, h in ipairs(mep_git_hunks) do\n"
    "        if h.old_count == 0 then\n"
    "          for r = h.new_start, h.new_start + h.new_count - 1 do\n"
    "            mep.deco_add(mep_git_ns, {row = r, whole_line = true, hl_group = 'Add', sign = '+', sign_hl = 'Add'})\n"
    "          end\n"
    "        elseif h.new_count == 0 then\n"
    "          mep.deco_add(mep_git_ns, {row = math.max(1, h.new_start), whole_line = false, sign = '_', sign_hl = 'Red'})\n"
    "        else\n"
    "          for r = h.new_start, h.new_start + h.new_count - 1 do\n"
    "            mep.deco_add(mep_git_ns, {row = r, whole_line = true, hl_group = 'Yellow', sign = '~', sign_hl = 'Yellow'})\n"
    "          end\n"
    "        end\n"
    "      end\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "function mep.git_next_hunk()\n"
    "  local row = mep.cursor()\n"
    "  for _, h in ipairs(mep_git_hunks) do\n"
    "    if h.new_start > row then mep.set_cursor(h.new_start, 1) return end\n"
    "  end\n"
    "  if mep_git_hunks[1] then mep.set_cursor(mep_git_hunks[1].new_start, 1) end\n"
    "end\n"
    "function mep.git_prev_hunk()\n"
    "  local row = mep.cursor()\n"
    "  for i = #mep_git_hunks, 1, -1 do\n"
    "    local h = mep_git_hunks[i]\n"
    "    if h.new_start < row then mep.set_cursor(h.new_start, 1) return end\n"
    "  end\n"
    "  local last = mep_git_hunks[#mep_git_hunks]\n"
    "  if last then mep.set_cursor(last.new_start, 1) end\n"
    "end\n"
    "local function mep_git_hunk_at_cursor()\n"
    "  local row = mep.cursor()\n"
    "  for _, h in ipairs(mep_git_hunks) do\n"
    "    local lo, hi = h.new_start, h.new_start + math.max(1, h.new_count) - 1\n"
    "    if row >= lo and row <= hi then return h end\n"
    "  end\n"
    "end\n"
    "function mep.git_reset_hunk()\n"
    "  local h = mep_git_hunk_at_cursor()\n"
    "  if not h then mep.notify('No hunk under cursor', 'warn') return end\n"
    "  local repl = {}\n"
    "  for i = h.old_start, h.old_start + h.old_count - 1 do repl[#repl + 1] = mep_git_base_lines[i] end\n"
    "  mep.replace_lines(h.new_start, h.new_start + h.new_count, repl)\n"
    "  mep.git_gutter_refresh()\n"
    "end\n"
    "function mep.git_stage_hunk()\n"
    "  local h = mep_git_hunk_at_cursor()\n"
    "  local fname = mep.filename()\n"
    "  if not h or fname == '' then mep.notify('No hunk under cursor', 'warn') return end\n"
    "  local lines = {}\n"
    "  lines[#lines + 1] = 'diff --git a/' .. fname .. ' b/' .. fname\n"
    "  lines[#lines + 1] = '--- a/' .. fname\n"
    "  lines[#lines + 1] = '+++ b/' .. fname\n"
    "  local old_hdr = h.old_count == 0 and (h.old_start .. ',0') or (h.old_start .. ',' .. h.old_count)\n"
    "  local new_hdr = h.new_count == 0 and (h.new_start .. ',0') or (h.new_start .. ',' .. h.new_count)\n"
    "  lines[#lines + 1] = '@@ -' .. old_hdr .. ' +' .. new_hdr .. ' @@'\n"
    "  for i = h.old_start, h.old_start + h.old_count - 1 do lines[#lines + 1] = '-' .. mep_git_base_lines[i] end\n"
    "  for i = h.new_start, h.new_start + h.new_count - 1 do lines[#lines + 1] = '+' .. mep.get_line(i) end\n"
    "  local patch = table.concat(lines, '\\n') .. '\\n'\n"
    "  local id = mep.job_start({'git', 'apply', '--cached', '--unidiff-zero', '-'}, {\n"
    "    on_exit = function(code)\n"
    "      if code == 0 then mep.notify('Staged hunk') else mep.notify('git apply failed', 'error') end\n"
    "    end,\n"
    "  })\n"
    "  mep.job_write(id, patch)\n"
    "  mep.job_close_stdin(id)\n"
    "end\n"
    "function mep.git_status_refresh()\n"
    "  if not mep_git_status_sidebar_id then\n"
    "    mep_git_status_sidebar_id = mep.sidebar_create('Git Status', 'left', 40)\n"
    "    mep.sidebar_set_on_key(mep_git_status_sidebar_id, mep.git_status_on_key)\n"
    "  end\n"
    "  local widgets = {}\n"
    "  mep.job_start({'git', 'status', '--porcelain'}, {\n"
    "    on_stdout = function(line)\n"
    "      local status, path = line:sub(1, 2), line:sub(4)\n"
    "      widgets[#widgets + 1] = {\n"
    "        id = path, text = status .. '  ' .. path,\n"
    "        on_click = function() mep.cmd('e ' .. path) end,\n"
    "      }\n"
    "    end,\n"
    "    on_exit = function()\n"
    "      mep.sidebar_set_sections(mep_git_status_sidebar_id, {\n"
    "        {id = 'status', title = 'Changes', collapsed = false, widgets = widgets},\n"
    "      })\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "function mep.git_status_on_key(k)\n"
    "  local target = mep.sidebar_cursor_widget_id(mep_git_status_sidebar_id)\n"
    "  if not target then\n"
    "    if k == '?' then\n"
    "      mep.notify('Git: Enter=open  s=stage  u=unstage  d=discard  c=commit  R=refresh')\n"
    "    elseif k == 'c' then\n"
    "      mep.ui_input('Commit message:', '', function(msg)\n"
    "        if msg and msg ~= '' then\n"
    "          mep.job_start({'git', 'commit', '-m', msg}, {\n"
    "            on_exit = function(code)\n"
    "              if code == 0 then mep.notify('Committed') else mep.notify('git commit failed', 'error') end\n"
    "              mep.git_status_refresh()\n"
    "            end,\n"
    "          })\n"
    "        end\n"
    "      end)\n"
    "    elseif k == 'R' then\n"
    "      mep.git_status_refresh()\n"
    "    end\n"
    "    return\n"
    "  end\n"
    "  if k == 's' then\n"
    "    mep.job_start({'git', 'add', target}, {on_exit = function() mep.git_status_refresh() end})\n"
    "  elseif k == 'u' then\n"
    "    mep.job_start({'git', 'reset', target}, {on_exit = function() mep.git_status_refresh() end})\n"
    "  elseif k == 'd' then\n"
    "    mep.ui_confirm('Discard changes to ' .. target .. '?', false, function(yes)\n"
    "      if yes then\n"
    "        mep.job_start({'git', 'checkout', '--', target}, {on_exit = function() mep.git_status_refresh() end})\n"
    "      end\n"
    "    end)\n"
    "  elseif k == 'R' then\n"
    "    mep.git_status_refresh()\n"
    "  end\n"
    "end\n"
    "mep.command('MepGitStatus', function() mep.git_status_refresh(); mep.sidebar_open(mep_git_status_sidebar_id) end)\n"
    "mep.command('MepGitGutter', mep.git_gutter_refresh)\n"
    // Opt-in auto-recompute; :lua mep.git_gutter_auto = true to enable.
    // A longer default debounce interval than colorize/todo_mark since
    // this spawns a git subprocess per recompute.
    "mep.git_gutter_auto = false\n"
    "mep.on_buffer_changed(function() if mep.git_gutter_auto then mep.git_gutter_refresh() end end, 0.6)\n";

// Todoscan (Phase 18): project-wide keyword scan (ripgrep-backed -- no
// synchronous walk+match fallback for the project-wide scan specifically,
// unlike find_files, since without ripgrep there is no fast alternative
// short of a slow pure-Lua recursive grep; the picker just reports zero
// matches with a hint) + live in-buffer marking via decorations.
const char *kBuiltinTodo =
    "local MEP_TODO_KEYWORDS = {'TODO', 'FIXME', 'HACK', 'NOTE'}\n"
    "local mep_todo_ns = nil\n"
    "function mep.todo_mark_buffer()\n"
    "  if not mep_todo_ns then mep_todo_ns = mep.ns_create('todoscan') end\n"
    "  mep.ns_clear(mep_todo_ns)\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    for _, kw in ipairs(MEP_TODO_KEYWORDS) do\n"
    "      local s = line:find(kw, 1, true)\n"
    "      if s then\n"
    "        mep.deco_add(mep_todo_ns, {row = i, col_start = s, col_end = s + #kw, hl_group = 'Warn', sign = 'T', sign_hl = 'Warn'})\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepTodoMark', mep.todo_mark_buffer)\n"
    // Opt-in auto-recompute; :lua mep.todo_mark_auto = true to enable.
    "mep.todo_mark_auto = false\n"
    "mep.on_buffer_changed(function() if mep.todo_mark_auto then mep.todo_mark_buffer() end end)\n"
    "function mep.todoscan()\n"
    "  local pattern = table.concat(MEP_TODO_KEYWORDS, '|')\n"
    "  local items = {}\n"
    "  mep.job_start({'rg', '-n', '--no-heading', pattern, '.'}, {\n"
    "    on_stdout = function(line) items[#items + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      if #items == 0 then\n"
    "        mep.notify('No TODO/FIXME/HACK/NOTE matches found (or ripgrep not installed)')\n"
    "        return\n"
    "      end\n"
    "      mep.picker_open('Todos', items, function(item)\n"
    "        if not item then return end\n"
    "        local file, lnum = item:match('^([^:]+):(%d+):')\n"
    "        if file then mep.cmd('e ' .. file); mep.set_cursor(tonumber(lnum), 1) end\n"
    "      end)\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "mep.command('MepTodoScan', mep.todoscan)\n";

// LSP client orchestration (Phase 20): the wire protocol (framing,
// request/response correlation, JSON<->Lua marshaling) is the new C++ in
// lua_env.cpp (mep.lsp_*); everything protocol-shaped -- the server
// registry, the initialize handshake, document sync, and the request
// wrappers + keybindings -- is plain Lua on top of it, same split as every
// other phase this session.
const char *kBuiltinLsp =
    "mep.lsp_servers = {\n"
    "  lua = {cmd = {'lua-language-server'}, filetypes = {'lua'}},\n"
    "  clangd = {cmd = {'clangd'}, filetypes = {'c', 'cpp', 'h', 'hpp', 'cc', 'cxx'}},\n"
    "  pyright = {cmd = {'pyright-langserver', '--stdio'}, filetypes = {'py'}},\n"
    "}\n"
    // filetype -> client_id (one client per filetype, workspace-wide)
    "local mep_lsp_clients = {}\n"
    // filename -> array of LSP Diagnostic
    "local mep_lsp_diagnostics = {}\n"
    // filename -> version counter
    "local mep_lsp_doc_versions = {}\n"
    // Global (not local): shared with kBuiltinSnippets, a separate
    // DoString chunk that also needs filetype detection -- a `local`
    // here would only be visible within this chunk's own closures.
    "function mep_lsp_filetype(fname)\n"
    "  return fname:match('%.([%w_]+)$')\n"
    "end\n"
    // Diagnostics arrive keyed by the server's own (always-absolute)
    // file:// URI, but mep.filename() is whatever path the file was
    // opened with (often relative) -- every lookup into
    // mep_lsp_diagnostics normalizes through this so the two can't
    // silently miss each other (a real bug caught during verification:
    // the first version compared the relative filename directly against
    // the absolute URI-derived key and never matched).
    "function mep_lsp_abspath(fname)\n"
    "  if fname:sub(1, 1) == '/' then return fname end\n"
    "  return mep.getcwd() .. '/' .. fname\n"
    "end\n"
    "function mep_lsp_uri(fname)\n"
    "  return 'file://' .. mep_lsp_abspath(fname)\n"
    "end\n"
    "function mep.lsp_client_for(fname)\n"
    "  local ft = mep_lsp_filetype(fname or mep.filename())\n"
    "  return ft and mep_lsp_clients[ft]\n"
    "end\n"
    "function mep.lsp_attach()\n"
    "  local fname = mep.filename()\n"
    "  if fname == '' then return end\n"
    "  local ft = mep_lsp_filetype(fname)\n"
    "  if not ft then return end\n"
    "  local server = mep.lsp_servers[ft]\n"
    "  if not server then return end\n"
    "  if mep_lsp_clients[ft] and mep.lsp_is_running(mep_lsp_clients[ft]) then\n"
    "    mep.lsp_did_open()\n"
    "    return\n"
    "  end\n"
    "  local id = mep.lsp_start(server.cmd, {cwd = '.'})\n"
    "  if id <= 0 then\n"
    "    mep.notify('LSP: failed to start ' .. server.cmd[1] .. ' (not on PATH?)', 'warn')\n"
    "    return\n"
    "  end\n"
    "  mep_lsp_clients[ft] = id\n"
    "  mep.lsp_request(id, 'initialize', {\n"
    "    processId = mep.platform() == 'wasm' and mep.json_null or nil,\n"
    "    rootUri = mep_lsp_uri('.'),\n"
    "    capabilities = {\n"
    "      textDocument = {\n"
    "        hover = {contentFormat = {'plaintext'}},\n"
    "        completion = {completionItem = {snippetSupport = false}},\n"
    "        publishDiagnostics = {},\n"
    "        documentSymbol = {},\n"
    "      },\n"
    "    },\n"
    "  }, function(msg)\n"
    "    mep.lsp_notify(id, 'initialized', {})\n"
    "    mep.notify('LSP attached: ' .. server.cmd[1])\n"
    "    mep.lsp_on_notification(id, 'textDocument/publishDiagnostics', function(params)\n"
    "      local uri = params.uri or ''\n"
    "      local f = uri:gsub('^file://', '')\n"
    "      mep_lsp_diagnostics[f] = params.diagnostics or {}\n"
    "      if f == mep_lsp_abspath(mep.filename()) then mep.lsp_render_diagnostics() end\n"
    "    end)\n"
    "    mep.lsp_did_open()\n"
    "  end)\n"
    "end\n"
    "function mep.lsp_did_open()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then return end\n"
    "  local fname = mep.filename()\n"
    "  local text = {}\n"
    "  for i = 1, mep.line_count() do text[i] = mep.get_line(i) end\n"
    "  mep_lsp_doc_versions[fname] = 1\n"
    "  mep.lsp_notify(id, 'textDocument/didOpen', {\n"
    "    textDocument = {uri = mep_lsp_uri(fname), languageId = mep_lsp_filetype(fname) or '',\n"
    "                    version = 1, text = table.concat(text, '\\n')},\n"
    "  })\n"
    "end\n"
    "function mep.lsp_did_change()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then return end\n"
    "  local fname = mep.filename()\n"
    "  local v = (mep_lsp_doc_versions[fname] or 1) + 1\n"
    "  mep_lsp_doc_versions[fname] = v\n"
    "  local text = {}\n"
    "  for i = 1, mep.line_count() do text[i] = mep.get_line(i) end\n"
    "  mep.lsp_notify(id, 'textDocument/didChange', {\n"
    "    textDocument = {uri = mep_lsp_uri(fname), version = v},\n"
    "    contentChanges = {{text = table.concat(text, '\\n')}},\n"
    "  })\n"
    "end\n"
    "function mep.lsp_did_save()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if id then mep.lsp_notify(id, 'textDocument/didSave', {textDocument = {uri = mep_lsp_uri(mep.filename())}}) end\n"
    "end\n"
    "function mep_lsp_position()\n"
    "  local row, col = mep.cursor()\n"
    "  return {line = row - 1, character = col - 1}\n"
    "end\n"
    // JSON `null` marshals to mep.json_null, a lightuserdata sentinel --
    // truthy in Lua (unlike real nil), so `if msg.result then` alone
    // treats "the server explicitly said no result" as if a result were
    // present, then errors trying to index/length a userdata (a real bug
    // caught during verification: definition-not-found crashed exactly
    // this way). Every LSP response callback normalizes through this
    // first instead of touching msg.result directly.
    "function mep_lsp_result(msg)\n"
    "  local r = msg.result\n"
    "  if r == mep.json_null then return nil end\n"
    "  return r\n"
    "end\n"
    "function mep.lsp_hover()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, 'textDocument/hover', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    if not result then mep.notify('No hover info') return end\n"
    "    local contents = result.contents\n"
    "    local text = type(contents) == 'table' and (contents.value or table.concat(contents, '\\n')) or tostring(contents)\n"
    "    mep.notify(text)\n"
    "  end)\n"
    "end\n"
    "function mep.lsp_goto_definition()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, 'textDocument/definition', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    local loc = result and (result.uri and result or result[1])\n"
    "    if not loc then mep.notify('No definition found') return end\n"
    "    local f = loc.uri:gsub('^file://', '')\n"
    "    mep.cmd('e ' .. f)\n"
    "    mep.set_cursor(loc.range.start.line + 1, loc.range.start.character + 1)\n"
    "  end)\n"
    "end\n"
    "function mep.lsp_references()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, 'textDocument/references', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "    context = {includeDeclaration = true},\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    if not result or #result == 0 then mep.notify('No references found') return end\n"
    "    local items = {}\n"
    "    for _, loc in ipairs(result) do\n"
    "      local f = loc.uri:gsub('^file://', '')\n"
    "      items[#items + 1] = {display = f .. ':' .. (loc.range.start.line + 1), data = f .. ':' .. (loc.range.start.line + 1)}\n"
    "    end\n"
    "    mep.picker_open('References', items, function(item)\n"
    "      if not item then return end\n"
    "      local f, lnum = item:match('^(.*):(%d+)$')\n"
    "      if f then mep.cmd('e ' .. f); mep.set_cursor(tonumber(lnum), 1) end\n"
    "    end)\n"
    "  end)\n"
    "end\n"
    "function mep.lsp_format()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, 'textDocument/formatting', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())},\n"
    "    options = {tabSize = 4, insertSpaces = true},\n"
    "  }, function(msg)\n"
    "    local edits = mep_lsp_result(msg)\n"
    "    if not edits then return end\n"
    "    table.sort(edits, function(a, b) return a.range.start.line > b.range.start.line end)\n"
    "    for _, e in ipairs(edits) do\n"
    "      local lines = {}\n"
    "      for s in (e.newText .. '\\n'):gmatch('(.-)\\n') do lines[#lines + 1] = s end\n"
    "      if #lines > 0 and lines[#lines] == '' then lines[#lines] = nil end\n"
    "      mep.replace_lines(e.range.start.line + 1, e.range['end'].line + 2, lines)\n"
    "    end\n"
    "    mep.notify('Formatted')\n"
    "  end)\n"
    "end\n"
    "mep.command('MepLspAttach', mep.lsp_attach)\n"
    "mep.command('MepLspHover', mep.lsp_hover)\n"
    "mep.command('MepLspDefinition', mep.lsp_goto_definition)\n"
    "mep.command('MepLspReferences', mep.lsp_references)\n"
    "mep.command('MepLspFormat', mep.lsp_format)\n"
    // --- Diagnostics UI (Phase 21) -- built on the publishDiagnostics
    // store kBuiltinLsp already populates (mep_lsp_diagnostics, upvalue-
    // shared since this all lives in the same DoString chunk).
    "local mep_diag_ns = nil\n"
    "local MEP_DIAG_SEVERITY = {[1] = 'Error', [2] = 'Warn', [3] = 'Info', [4] = 'Hint'}\n"
    "local MEP_DIAG_GLYPH = {[1] = 'E', [2] = 'W', [3] = 'I', [4] = 'H'}\n"
    "function mep.lsp_render_diagnostics()\n"
    "  if not mep_diag_ns then mep_diag_ns = mep.ns_create('diagnostics') end\n"
    "  mep.ns_clear(mep_diag_ns)\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    local sev = d.severity or 1\n"
    "    local hl = MEP_DIAG_SEVERITY[sev] or 'Error'\n"
    "    local row = d.range.start.line + 1\n"
    "    mep.deco_add(mep_diag_ns, {\n"
    "      row = row, col_start = d.range.start.character + 1, col_end = d.range['end'].character + 1,\n"
    "      hl_group = hl, sign = MEP_DIAG_GLYPH[sev] or 'E', sign_hl = hl,\n"
    "      virt_text = '  ' .. d.message:gsub('\\n.*', ''), virt_text_hl = hl,\n"
    "    })\n"
    "  end\n"
    "end\n"
    "function mep.lsp_diagnostic_at_cursor()\n"
    "  local row = mep.cursor()\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    if d.range.start.line + 1 == row then\n"
    "      mep.notify((MEP_DIAG_SEVERITY[d.severity or 1]) .. ': ' .. d.message,\n"
    "                 (d.severity == 1 and 'error') or (d.severity == 2 and 'warn') or 'info')\n"
    "      return\n"
    "    end\n"
    "  end\n"
    "  mep.notify('No diagnostic on this line')\n"
    "end\n"
    "local function mep_diag_nav(delta, errors_only)\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  if #diags == 0 then mep.notify('No diagnostics') return end\n"
    "  local rows = {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    if not errors_only or (d.severity or 1) == 1 then rows[#rows + 1] = d.range.start.line + 1 end\n"
    "  end\n"
    "  table.sort(rows)\n"
    "  if #rows == 0 then mep.notify('No matching diagnostics') return end\n"
    "  local cur = mep.cursor()\n"
    "  if delta > 0 then\n"
    "    for _, r in ipairs(rows) do if r > cur then mep.set_cursor(r, 1) return end end\n"
    "    mep.set_cursor(rows[1], 1)\n"
    "  else\n"
    "    for i = #rows, 1, -1 do if rows[i] < cur then mep.set_cursor(rows[i], 1) return end end\n"
    "    mep.set_cursor(rows[#rows], 1)\n"
    "  end\n"
    "end\n"
    "function mep.lsp_next_diagnostic() mep_diag_nav(1, false) end\n"
    "function mep.lsp_prev_diagnostic() mep_diag_nav(-1, false) end\n"
    "function mep.lsp_next_error() mep_diag_nav(1, true) end\n"
    "function mep.lsp_prev_error() mep_diag_nav(-1, true) end\n"
    "mep.command('MepDiagShow', mep.lsp_diagnostic_at_cursor)\n";

// Completion sources (Phase 22): buffer-word is the always-available
// default (registered here); mep.set_completion_source(fn) lets user
// config or a later phase (LSP, once a completion request wrapper is
// added) swap in a richer source.
const char *kBuiltinCompletion =
    "function mep.completion_buffer_words(prefix)\n"
    "  local seen, words = {}, {}\n"
    // Snippet trigger names for the current filetype (Phase 23) count as
    // completion candidates too -- accepting one inserts the trigger word
    // itself, same as any buffer word; expanding it into the full snippet
    // body still needs the separate explicit mep.snippet_trigger() key.
    "  local ft = mep_lsp_filetype and mep_lsp_filetype(mep.filename())\n"
    "  local snip_set = ft and mep.snippets and mep.snippets[ft]\n"
    "  if snip_set then\n"
    "    for name, _ in pairs(snip_set) do\n"
    "      if #name > #prefix and name:sub(1, #prefix) == prefix then\n"
    "        seen[name] = true\n"
    "        words[#words + 1] = name\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  for i = 1, mep.line_count() do\n"
    "    for w in mep.get_line(i):gmatch('[%w_]+') do\n"
    "      if #w > #prefix and w:sub(1, #prefix) == prefix and not seen[w] then\n"
    "        seen[w] = true\n"
    "        words[#words + 1] = w\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  table.sort(words)\n"
    "  return words\n"
    "end\n"
    "mep.set_completion_source(mep.completion_buffer_words)\n";

// Snippet engine (Phase 23), **scoped down significantly** from the plan:
// tabstops are numbered ($1, $2, $0) only -- no `${1:placeholder}` default-
// text syntax -- and tabstop positions are computed *once* at expand time
// as fixed {row, col} offsets, not tracked live through further edits via
// Phase 4 decoration gravity like the plan calls for (a real gravity-
// tracked implementation needs the decoration system extended with
// insert/delete-aware position updates, which nothing has needed yet).
// Practically: jumping between tabstops immediately after expansion works
// correctly; editing heavily *before* jumping to a later tabstop can leave
// it stale. A curated 2-language starter set (lua, python) rather than
// the full mep.nvim registry.
const char *kBuiltinSnippets =
    "mep.snippets = {\n"
    "  lua = {\n"
    "    func = {'function $1($2)', '  $0', 'end'},\n"
    "    loc = {'local $1 = $2'},\n"
    "    forpairs = {'for $1, $2 in pairs($3) do', '  $0', 'end'},\n"
    "  },\n"
    "  python = {\n"
    "    def = {'def $1($2):', '    $0'},\n"
    "    class = {'class $1:', '    def __init__(self$2):', '        $0'},\n"
    "  },\n"
    "}\n"
    "local mep_snippet_state = nil\n"
    "local function mep_snippet_scan_line(tmpl)\n"
    "  local out, tabstops, i = {}, {}, 1\n"
    "  while i <= #tmpl do\n"
    "    local c = tmpl:sub(i, i)\n"
    "    if c == '$' then\n"
    "      local numstr = tmpl:match('^%d+', i + 1)\n"
    "      if numstr then\n"
    "        tabstops[#tabstops + 1] = {col = #out + 1, num = tonumber(numstr)}\n"
    "        i = i + 1 + #numstr\n"
    "      else\n"
    "        out[#out + 1] = c\n"
    "        i = i + 1\n"
    "      end\n"
    "    else\n"
    "      out[#out + 1] = c\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  return table.concat(out), tabstops\n"
    "end\n"
    "function mep.snippet_jump(delta)\n"
    "  if not mep_snippet_state then return end\n"
    "  local st = mep_snippet_state\n"
    "  st.index = st.index + delta\n"
    "  if st.index < 1 or st.index > #st.tabstops then mep_snippet_state = nil return end\n"
    "  local ts = st.tabstops[st.index]\n"
    "  mep.set_cursor(st.base_row + ts.line_idx - 1, ts.col)\n"
    "end\n"
    "function mep.snippet_expand(name)\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  local set = ft and mep.snippets[ft]\n"
    "  local body = set and set[name]\n"
    "  if not body then mep.notify('No snippet: ' .. tostring(name), 'warn') return end\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local trig_start = math.max(1, col - #name)\n"
    "  local before = line:sub(1, trig_start - 1)\n"
    "  local after = line:sub(col)\n"
    "  local out_lines, all_tabstops = {}, {}\n"
    "  for li, tmpl in ipairs(body) do\n"
    "    local cleaned, stops = mep_snippet_scan_line(tmpl)\n"
    "    local col_offset = (li == 1) and #before or 0\n"
    "    for _, s in ipairs(stops) do\n"
    "      all_tabstops[#all_tabstops + 1] = {line_idx = li, col = s.col + col_offset, num = s.num}\n"
    "    end\n"
    "    out_lines[li] = (li == 1 and before or '') .. cleaned .. (li == #body and after or '')\n"
    "  end\n"
    "  if #out_lines == 1 then mep.set_line(row, out_lines[1]) else mep.replace_lines(row, row + 1, out_lines) end\n"
    "  table.sort(all_tabstops, function(a, b)\n"
    "    local an, bn = (a.num == 0 and 999 or a.num), (b.num == 0 and 999 or b.num)\n"
    "    return an < bn\n"
    "  end)\n"
    "  mep_snippet_state = {tabstops = all_tabstops, index = 0, base_row = row}\n"
    "  mep.snippet_jump(1)\n"
    "end\n"
    "function mep.snippet_trigger()\n"
    "  local row, col = mep.cursor()\n"
    "  local word = mep.get_line(row):sub(1, col - 1):match('[%w_]+$')\n"
    "  if word then mep.snippet_expand(word) end\n"
    "end\n"
    "function mep.snippets_picker()\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  local set = ft and mep.snippets[ft]\n"
    "  if not set then mep.notify('No snippets for this filetype') return end\n"
    "  local items = {}\n"
    "  for name, _ in pairs(set) do items[#items + 1] = name end\n"
    "  table.sort(items)\n"
    "  mep.picker_open('Snippets', items, function(item) if item then mep.snippet_expand(item) end end)\n"
    "end\n"
    "mep.command('MepSnippets', mep.snippets_picker)\n"
    "mep.command('MepSnippetNext', function() mep.snippet_jump(1) end)\n"
    "mep.command('MepSnippetPrev', function() mep.snippet_jump(-1) end)\n";

// Symbols outline (Phase 24): textDocument/documentSymbol into a Phase 7
// sidebar, reusing exactly the LSP request wrapper and mep_lsp_result/
// mep_lsp_uri/mep_lsp_filetype helpers Phase 20/21 already defined.
const char *kBuiltinSymbols =
    "local mep_symbols_sidebar_id = nil\n"
    "local MEP_SYMBOL_KIND = {[2]='module',[5]='class',[6]='method',[7]='property',[9]='enum',\n"
    "  [10]='enummember',[12]='function',[13]='variable',[14]='constant'}\n"
    "function mep.lsp_symbols_refresh()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, 'textDocument/documentSymbol', {textDocument = {uri = mep_lsp_uri(mep.filename())}},\n"
    "  function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    if not result or #result == 0 then mep.notify('No symbols (or client lacks documentSymbol)') return end\n"
    "    if not mep_symbols_sidebar_id then mep_symbols_sidebar_id = mep.sidebar_create('Symbols', 'right', 32) end\n"
    "    local widgets = {}\n"
    "    local function add(sym, depth)\n"
    "      local kind = MEP_SYMBOL_KIND[sym.kind] or '?'\n"
    "      local rng = sym.range or (sym.location and sym.location.range)\n"
    "      local line = rng and rng.start.line or 0\n"
    "      widgets[#widgets + 1] = {\n"
    "        id = tostring(line), text = string.rep('  ', depth) .. sym.name .. '  [' .. kind .. ']',\n"
    "        on_click = function() mep.set_cursor(line + 1, 1) end,\n"
    "      }\n"
    "      if sym.children then for _, c in ipairs(sym.children) do add(c, depth + 1) end end\n"
    "    end\n"
    "    for _, s in ipairs(result) do add(s, 0) end\n"
    "    mep.sidebar_set_sections(mep_symbols_sidebar_id,\n"
    "      {{id = 'symbols', title = mep.filename(), collapsed = false, widgets = widgets}})\n"
    "    mep.sidebar_open(mep_symbols_sidebar_id)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepSymbols', mep.lsp_symbols_refresh)\n"
    // Opt-in refresh-on-save; :lua mep.symbols_auto_refresh = true to
    // enable. Only refreshes if the symbols sidebar is already open --
    // lsp_symbols_refresh() always opens it, and auto-popping a closed
    // sidebar open on every save would be a lot more intrusive than the
    // colorize/gutter/todo-mark auto-consumers (which just silently
    // update decorations no matter what's visible).
    "mep.symbols_auto_refresh = false\n"
    "mep.on_buffer_saved(function()\n"
    "  if mep.symbols_auto_refresh and mep_symbols_sidebar_id and mep.sidebar_is_open(mep_symbols_sidebar_id) then\n"
    "    mep.lsp_symbols_refresh()\n"
    "  end\n"
    "end)\n";

// Docs (generate + lookup) + Help picker (Phase 25). Help picker reuses
// mep.commands() (Phase 8/13's command palette) as-is -- "a live index
// over registered commands... <CR> runs the selected entry" is exactly
// what that picker already does; keybinding introspection (the plan's
// other half of this bullet) is deferred, no keybind-description registry
// exists to introspect (mep.map's callers never attach one).
const char *kBuiltinDocs =
    "local MEP_DOC_TEMPLATES = {\n"
    "  lua = function(sig) return {'--- ' .. sig, '-- @param', '-- @return'} end,\n"
    "  python = function(sig) return {'\"\"\"', sig, '\"\"\"'} end,\n"
    "  javascript = function(sig) return {'/**', ' * ' .. sig, ' */'} end,\n"
    "}\n"
    "function mep.docs_generate()\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  local tmpl = ft and MEP_DOC_TEMPLATES[ft]\n"
    "  if not tmpl then mep.notify('No doc template for this filetype', 'warn') return end\n"
    "  local row = mep.cursor()\n"
    "  local sig = mep.get_line(row):match('^%s*(.-)%s*$')\n"
    "  mep.replace_lines(row, row, tmpl(sig))\n"
    "end\n"
    "mep.command('MepDocGen', mep.docs_generate)\n"
    "function mep.docs_lookup()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local word = line:sub(1, col - 1):match('[%w_]+$') or line:sub(col):match('^[%w_]+')\n"
    "  if not word then mep.notify('No word under cursor', 'warn') return end\n"
    "  mep.open_url('https://devdocs.io/#q=' .. word)\n"
    "end\n"
    "mep.command('MepDocLookup', mep.docs_lookup)\n"
    "mep.command('MepHelp', mep.commands)\n";

// DAP client (Phase 26): reuses Phase 20's mep.lsp_start/request/notify
// wholesale -- despite the name, that's a generic Content-Length-framed
// JSON-RPC client, not LSP-specific, and DAP uses the identical wire
// framing. **Scoped down significantly, and not verified against a live
// adapter in this session** (no DAP adapter -- lldb-dap/debugpy/etc --
// was confirmed available, unlike Phase 20's lua-language-server): session
// start (spawn, initialize -> launch -> configurationDone), breakpoint
// toggle (gutter sign via Phase 4 decorations) + setBreakpoints, and
// continue/step-over/step-in/step-out/terminate. **Not implemented**:
// the stack/scopes/variables sidebar, the REPL console, and responding to
// server-initiated requests (e.g. runInTerminal) -- DAP requires the
// client to send a response back for those (a real protocol gap this
// client doesn't cover yet, since Phase 20 only ever needed to handle
// server->client *notifications*, not server->client *requests*).
const char *kBuiltinDap =
    "mep.dap_adapters = {\n"
    "  cpp = {cmd = {'lldb-dap'}, filetypes = {'c', 'cpp'}},\n"
    "  python = {cmd = {'python3', '-m', 'debugpy.adapter'}, filetypes = {'py'}},\n"
    "}\n"
    "local mep_dap_client = nil\n"
    "local mep_dap_ns = nil\n"
    "local mep_dap_breakpoints = {}\n"
    "function mep.dap_toggle_breakpoint()\n"
    "  if not mep_dap_ns then mep_dap_ns = mep.ns_create('dap') end\n"
    "  local row = mep.cursor()\n"
    "  local f = mep.filename()\n"
    "  mep_dap_breakpoints[f] = mep_dap_breakpoints[f] or {}\n"
    "  local bps = mep_dap_breakpoints[f]\n"
    "  for i, r in ipairs(bps) do\n"
    "    if r == row then table.remove(bps, i)\n"
    "      mep.ns_clear(mep_dap_ns)\n"
    "      for _, r2 in ipairs(bps) do mep.deco_add(mep_dap_ns, {row = r2, sign = 'B', sign_hl = 'Red'}) end\n"
    "      return\n"
    "    end\n"
    "  end\n"
    "  bps[#bps + 1] = row\n"
    "  mep.deco_add(mep_dap_ns, {row = row, sign = 'B', sign_hl = 'Red'})\n"
    "end\n"
    "function mep.dap_start(lang)\n"
    "  local adapter = mep.dap_adapters[lang]\n"
    "  if not adapter then mep.notify('No DAP adapter for ' .. tostring(lang), 'warn') return end\n"
    "  local id = mep.lsp_start(adapter.cmd, {cwd = '.'})\n"
    "  if id <= 0 then mep.notify('Failed to start ' .. adapter.cmd[1], 'error') return end\n"
    "  mep_dap_client = id\n"
    "  mep.lsp_request(id, 'initialize', {adapterID = lang, linesStartAt1 = true, columnsStartAt1 = true},\n"
    "  function()\n"
    "    mep.lsp_on_notification(id, 'stopped', function(body)\n"
    "      mep.notify('Stopped: ' .. (body.reason or '?'))\n"
    "    end)\n"
    "    mep.lsp_on_notification(id, 'initialized', function()\n"
    "      local f = mep.filename()\n"
    "      local bps = mep_dap_breakpoints[f] or {}\n"
    "      local lines = {}\n"
    "      for _, r in ipairs(bps) do lines[#lines + 1] = {line = r} end\n"
    "      mep.lsp_request(id, 'setBreakpoints', {source = {path = mep.getcwd() .. '/' .. f}, breakpoints = lines})\n"
    "      mep.lsp_request(id, 'configurationDone', {})\n"
    "    end)\n"
    "    mep.lsp_request(id, 'launch', {program = mep.getcwd() .. '/' .. mep.filename()})\n"
    "    mep.notify('DAP session started: ' .. adapter.cmd[1])\n"
    "  end)\n"
    "end\n"
    "function mep.dap_continue() if mep_dap_client then mep.lsp_request(mep_dap_client, 'continue', {threadId = 1}) end end\n"
    "function mep.dap_step_over() if mep_dap_client then mep.lsp_request(mep_dap_client, 'next', {threadId = 1}) end end\n"
    "function mep.dap_step_into() if mep_dap_client then mep.lsp_request(mep_dap_client, 'stepIn', {threadId = 1}) end end\n"
    "function mep.dap_step_out() if mep_dap_client then mep.lsp_request(mep_dap_client, 'stepOut', {threadId = 1}) end end\n"
    "function mep.dap_terminate()\n"
    "  if mep_dap_client then mep.lsp_request(mep_dap_client, 'terminate', {}); mep.lsp_stop(mep_dap_client); mep_dap_client = nil end\n"
    "end\n"
    "mep.command('MepDapBreakpoint', mep.dap_toggle_breakpoint)\n"
    "mep.command('MepDapContinue', mep.dap_continue)\n"
    "mep.command('MepDapTerminate', mep.dap_terminate)\n";

// Syntax highlighting (Phase 19): a real Treesitter integration --
// vendored libtree-sitter plus a curated set of grammar sources (c, cpp,
// lua, python, javascript; see CMakeLists.txt) parsing the buffer and
// running each grammar's own upstream queries/highlights.scm
// (src/treesitter_queries.h), through mep.ts_captures (lua_env.cpp,
// backed by src/treesitter.cpp). For any other filetype -- no grammar
// vendored -- mep.syntax_highlight() falls back to the original
// hand-rolled per-line lexer below (comment-prefix / quoted-string /
// number / keyword-list). Both paths render through the same Phase 4
// decoration + Phase 9 highlight-group pipeline. Full-buffer reparse on
// every call: on demand via :MepSyntax, automatically on buffer switch,
// and debounce-rerun on edits (mep.syntax_auto, on by default -- see the
// two mep.on_buffer_changed/mep.on_frame hooks after :MepSyntax's
// registration below) -- rather than an incrementally-updated
// TSTree kept across edits -- consistent with this codebase's existing
// "on-demand full rescan" scope decisions elsewhere (see Phase 13's
// colorizer/todo-mark). No fold-query support (Phase 5's fold providers
// stay org/markdown/manual-only): these are highlight queries only.
const char *kBuiltinSyntax =
    "mep.syntax_keywords = {\n"
    "  lua = {'and','break','do','else','elseif','end','false','for','function','goto','if','in',\n"
    "    'local','nil','not','or','repeat','return','then','true','until','while'},\n"
    "  python = {'and','as','assert','break','class','continue','def','del','elif','else','except',\n"
    "    'False','finally','for','from','global','if','import','in','is','lambda','None','nonlocal',\n"
    "    'not','or','pass','raise','return','True','try','while','with','yield'},\n"
    "  c = {'auto','break','case','char','const','continue','default','do','double','else','enum',\n"
    "    'extern','float','for','goto','if','int','long','register','return','short','signed',\n"
    "    'sizeof','static','struct','switch','typedef','union','unsigned','void','volatile','while'},\n"
    "  javascript = {'break','case','catch','class','const','continue','default','delete','do','else',\n"
    "    'export','extends','false','finally','for','function','if','import','in','instanceof','let',\n"
    "    'new','null','return','super','switch','this','throw','true','try','typeof','var','void','while'},\n"
    "}\n"
    "mep.syntax_keywords.cpp = mep.syntax_keywords.c\n"
    "mep.syntax_keywords.hpp = mep.syntax_keywords.c\n"
    "mep.syntax_keywords.h = mep.syntax_keywords.c\n"
    // mep_lsp_filetype keys everything by bare file extension ('py',
    // 'js'), not language name -- 'lua'/'c' above already happen to be
    // both, but 'python'/'javascript' never matched anything through
    // this lookup. Aliased here for filetypes Treesitter doesn't cover
    // (the aliases below are actually reached by every filetype the
    // fallback lexer still serves; py/js themselves are handled by
    // Treesitter now and never fall through to this path).
    "mep.syntax_keywords.py = mep.syntax_keywords.python\n"
    "mep.syntax_keywords.js = mep.syntax_keywords.javascript\n"
    "mep.syntax_keywords.mjs = mep.syntax_keywords.javascript\n"
    "mep.syntax_keywords.cjs = mep.syntax_keywords.javascript\n"
    "mep.syntax_comment_prefix = {lua = '--', python = '#', javascript = '//', c = '//', cpp = '//', h = '//', hpp = '//',\n"
    "  py = '#', js = '//', mjs = '//', cjs = '//'}\n"
    // Treesitter capture name -> mep highlight group, checked full-name
    // first then by the capture's first dot-segment (e.g.
    // 'function.builtin' falls back to the 'function' entry if it's not
    // listed itself) -- mirrors nvim's own capture-group fallback
    // convention. Captures with no entry (variable, property, operator,
    // punctuation.*, ...) render in the buffer's plain Normal color,
    // same as they would have under single-flat-color rendering.
    "mep.ts_capture_hl = {\n"
    "  comment = 'Comment', ['comment.documentation'] = 'Comment', debug = 'Comment',\n"
    "  string = 'Green', ['string.escape'] = 'Green', ['string.special'] = 'Green', escape = 'Green',\n"
    "  number = 'Cyan', boolean = 'Cyan', constant = 'Cyan', ['constant.builtin'] = 'Cyan',\n"
    "  ['variable.builtin'] = 'Cyan', character = 'Cyan', float = 'Cyan', attribute = 'Cyan',\n"
    "  keyword = 'Purple', ['keyword.function'] = 'Purple', ['keyword.operator'] = 'Purple',\n"
    "  ['keyword.return'] = 'Purple', conditional = 'Purple', ['repeat'] = 'Purple',\n"
    "  preproc = 'Purple', label = 'Purple', storage = 'Purple', storageclass = 'Purple',\n"
    "  include = 'Purple', import = 'Purple', use = 'Purple', exception = 'Purple',\n"
    "  charset = 'Purple', media = 'Purple', keyframes = 'Purple', supports = 'Purple', mixin = 'Purple',\n"
    "  ['return'] = 'Purple',\n"
    "  ['function'] = 'Blue', ['function.builtin'] = 'Blue', ['function.call'] = 'Blue',\n"
    "  ['function.method'] = 'Blue', ['function.special'] = 'Blue',\n"
    "  method = 'Blue', ['method.call'] = 'Blue', constructor = 'Blue', tag = 'Blue',\n"
    "  type = 'Orange', namespace = 'Orange', module = 'Orange', interface = 'Orange', union = 'Orange',\n"
    "  error = 'Red', ['Error'] = 'Red', warn = 'Yellow',\n"
    // Markdown (queries/highlights.scm's own capture convention differs
    // from the dotted nvim-style names above: text.* for prose spans).
    "  ['text.title'] = 'Purple', ['text.strong'] = 'Yellow', ['text.emphasis'] = 'Cyan',\n"
    "  ['text.literal'] = 'Green', ['text.uri'] = 'Blue', ['text.reference'] = 'Blue',\n"
    // Org (queries/highlights.scm ships only this one example query,
    // using its own Org-prefixed capture names rather than the nvim-style
    // dotted convention every other vendored grammar's query uses).
    "  OrgHeadlineLevel1 = 'Purple', OrgHeadlineLevel2 = 'Purple', OrgHeadlineLevel3 = 'Purple',\n"
    "  OrgStars1 = 'Purple', OrgStars2 = 'Purple', OrgStars3 = 'Purple',\n"
    "  OrgKeywordTodo = 'Red', OrgKeywordDone = 'Green',\n"
    "  OrgPriority = 'Yellow', OrgPriorityCookie = 'Yellow', OrgProgressCookie = 'Yellow',\n"
    "  OrgPercentCookie = 'Yellow', OrgCookieNum = 'Yellow', OrgCheckbox = 'Yellow', OrgCheckInProgress = 'Yellow',\n"
    "  OrgCheckDone = 'Green',\n"
    "  OrgTag = 'Cyan', OrgTagList = 'Cyan', OrgProperty = 'Cyan', OrgPropertyName = 'Cyan',\n"
    "  OrgFootnoteDefinition = 'Cyan', OrgFootnoteLabel = 'Cyan', OrgCellNumber = 'Cyan',\n"
    "  OrgTimestampActive = 'Cyan', OrgTimestampInactive = 'Cyan', OrgTimestampDate = 'Cyan',\n"
    "  OrgTimestampDay = 'Cyan', OrgTimestampTime = 'Cyan', OrgTimestampDelay = 'Cyan', OrgTimestampRepeat = 'Cyan',\n"
    "  OrgComment = 'Comment', OrgDrawer = 'Comment', OrgDrawerName = 'Comment', OrgPropertyDrawer = 'Comment',\n"
    "  OrgTableHorizontalRuler = 'Comment', OrgTableHRBar = 'Comment',\n"
    "  OrgBlock = 'Purple', OrgBlockName = 'Purple', OrgDynamicBlock = 'Purple', OrgDynamicBlockName = 'Purple',\n"
    "  OrgDirective = 'Purple', OrgDirectiveName = 'Purple', OrgListBullet = 'Purple',\n"
    "  OrgPropertyValue = 'Green', OrgDirectiveValue = 'Green',\n"
    "  OrgCellFormula = 'Orange',\n"
    "}\n"
    "local function mep_ts_resolve_hl(capture)\n"
    "  local hl = mep.ts_capture_hl[capture]\n"
    "  if hl then return hl end\n"
    "  local base = capture:match('^([^.]+)')\n"
    "  return base and mep.ts_capture_hl[base]\n"
    "end\n"
    "local mep_syntax_ns = nil\n"
    "local function mep_syntax_scan_line(line, kwset, cprefix)\n"
    "  local decos, i, n = {}, 1, #line\n"
    "  while i <= n do\n"
    "    local c = line:sub(i, i)\n"
    "    if cprefix and line:sub(i, i + #cprefix - 1) == cprefix then\n"
    "      decos[#decos + 1] = {s = i, e = n + 1, hl = 'Comment'}\n"
    "      break\n"
    "    elseif c == '\"' or c == \"'\" then\n"
    "      local q, j = c, i + 1\n"
    "      while j <= n and line:sub(j, j) ~= q do\n"
    "        if line:sub(j, j) == '\\\\' then j = j + 1 end\n"
    "        j = j + 1\n"
    "      end\n"
    "      decos[#decos + 1] = {s = i, e = math.min(j + 1, n + 1), hl = 'Green'}\n"
    "      i = j + 1\n"
    "    elseif c:match('%d') then\n"
    "      local j = i\n"
    "      while j <= n and line:sub(j, j):match('[%d%.]') do j = j + 1 end\n"
    "      decos[#decos + 1] = {s = i, e = j, hl = 'Cyan'}\n"
    "      i = j\n"
    "    elseif c:match('[%a_]') then\n"
    "      local j = i\n"
    "      while j <= n and line:sub(j, j):match('[%w_]') do j = j + 1 end\n"
    "      if kwset[line:sub(i, j - 1)] then decos[#decos + 1] = {s = i, e = j, hl = 'Purple'} end\n"
    "      i = j\n"
    "    else\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  return decos\n"
    "end\n"
    "function mep.syntax_highlight()\n"
    "  if not mep_syntax_ns then mep_syntax_ns = mep.ns_create('syntax') end\n"
    "  mep.ns_clear(mep_syntax_ns)\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  if not ft then return end\n"
    "  local lines = {}\n"
    "  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end\n"
    // Real grammar available: parse + run its highlights.scm query
    // (mep.ts_captures, backed by src/treesitter.cpp) and stop -- this
    // *is* Treesitter syntax highlighting, not a fallback path.
    "  local captures = mep.ts_captures(ft, table.concat(lines, '\\n'))\n"
    "  if captures then\n"
    "    for _, cap in ipairs(captures) do\n"
    "      local hl = mep_ts_resolve_hl(cap.capture)\n"
    "      if hl then\n"
    "        mep.deco_add(mep_syntax_ns, {row = cap.row, col_start = cap.col_start, col_end = cap.col_end, hl_group = hl})\n"
    "      end\n"
    "    end\n"
    "    return\n"
    "  end\n"
    // No grammar vendored for this filetype: fall back to the
    // hand-rolled per-line lexer.
    "  local keywords = mep.syntax_keywords[ft]\n"
    "  if not keywords then return end\n"
    "  local kwset = {}\n"
    "  for _, k in ipairs(keywords) do kwset[k] = true end\n"
    "  local cprefix = mep.syntax_comment_prefix[ft]\n"
    "  for i = 1, #lines do\n"
    "    for _, d in ipairs(mep_syntax_scan_line(lines[i], kwset, cprefix)) do\n"
    "      mep.deco_add(mep_syntax_ns, {row = i, col_start = d.s, col_end = d.e, hl_group = d.hl})\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepSyntax', mep.syntax_highlight)\n"
    // On by default (unlike colorizer/git-gutter/todoscan's opt-in
    // _auto flags above): a bare "open a file, see colored syntax" is
    // the expected baseline for an editor, not an enhancement layer.
    // mep.on_buffer_changed already debounce-reruns this on edits (and,
    // via its -1 sentinel, once for whatever buffer is active when this
    // hook is first registered at startup) -- but it only watches
    // buffer_change_epoch(), which nothing bumps on a plain buffer
    // *switch* with no edit (:e, the buffer/file picker, :bn/:bp,
    // Ctrl-W pane navigation onto a different buffer). So it's paired
    // with a second, non-debounced watcher keyed on mep.filename()
    // itself: switching to a different file re-highlights immediately,
    // exactly once, regardless of the edit-debounce interval.
    "mep.syntax_auto = true\n"
    "mep.on_buffer_changed(function() if mep.syntax_auto then mep.syntax_highlight() end end)\n"
    "local mep_syntax_last_file = nil\n"
    "mep.on_frame(function()\n"
    "  if not mep.syntax_auto then return end\n"
    "  local fname = mep.filename()\n"
    "  if fname ~= mep_syntax_last_file then\n"
    "    mep_syntax_last_file = fname\n"
    "    mep.syntax_highlight()\n"
    "  end\n"
    "end)\n";

// Embedded terminal/PTY + Run + REPL (Phase 27). **Scoped down
// significantly** from a real interactive terminal: output is rendered
// into an ordinary buffer (dedicated per Run/REPL session, opened in a
// split via Phase 14), re-parsed from the full accumulated byte stream on
// every chunk (simpler and more obviously correct than incremental
// line-append, at the cost of being O(output size) per chunk -- fine at
// the output sizes a Run/REPL session actually produces). ANSI SGR color
// codes (30-37/90-97) are parsed into real decoration spans through the
// same Phase 4/9 pipeline everything else colors text through; every
// *other* escape sequence (cursor movement, clear-screen, ...) is
// silently discarded, not interpreted -- this is genuinely "basic ANSI
// handling: colors," not a cursor-addressable terminal grid, so a
// full-screen TUI program (vim, htop, an interactive Python REPL's
// fancier prompt) will render as garbled scrolling text rather than a
// real screen. **No raw-keystroke-by-keystroke input forwarding** either
// (the plan's "forward keystrokes to it") -- REPL interaction is line-
// oriented (send-line/selection/buffer, vim-slime style) via
// mep.job_write, which covers the actual "run code and see output" and
// "send an expression to a REPL" workflows without needing a full
// terminal-emulator input model.
const char *kBuiltinRun =
    "mep.run_languages = {\n"
    "  lua = {'lua'}, python = {'python3'}, javascript = {'node'}, sh = {'sh'},\n"
    "}\n"
    "mep.repl_languages = {\n"
    "  lua = {'lua'}, python = {'python3', '-u'},\n"
    "}\n"
    "local mep_term_ns = nil\n"
    // job_id -> {raw=, buffer_id=}
    "local mep_term_sessions = {}\n"
    "local function mep_ansi_sgr_hl(code)\n"
    "  local map = {[31]='Red',[91]='Red',[32]='Green',[92]='Green',[33]='Yellow',[93]='Yellow',\n"
    "    [34]='Blue',[94]='Blue',[35]='Purple',[95]='Purple',[36]='Cyan',[96]='Cyan'}\n"
    "  return map[code]\n"
    "end\n"
    // Re-parses the whole accumulated raw byte stream into plain lines +
    // color-span decorations every time (see comment above for why).
    "local function mep_ansi_render(raw)\n"
    "  local lines, spans = {''}, {}\n"
    "  local cur_hl, span_start, row = nil, nil, 1\n"
    "  local function close_span(end_col)\n"
    "    if cur_hl and span_start then\n"
    "      spans[#spans + 1] = {row = row, col_start = span_start, col_end = end_col, hl = cur_hl}\n"
    "    end\n"
    "    span_start = nil\n"
    "  end\n"
    "  local i, n = 1, #raw\n"
    "  while i <= n do\n"
    "    local c = raw:sub(i, i)\n"
    "    if c == '\\27' and raw:sub(i + 1, i + 1) == '[' then\n"
    "      local seq_end = raw:find('%a', i + 2)\n"
    "      if not seq_end then break end\n"
    "      local params, cmd = raw:sub(i + 2, seq_end - 1), raw:sub(seq_end, seq_end)\n"
    "      if cmd == 'm' then\n"
    "        close_span(#lines[row] + 1)\n"
    "        local new_hl, any = cur_hl, false\n"
    "        for code in (params .. ';'):gmatch('(%d*);') do\n"
    "          any = true\n"
    "          if code == '' or code == '0' then new_hl = nil else new_hl = mep_ansi_sgr_hl(tonumber(code)) or new_hl end\n"
    "        end\n"
    "        if not any then new_hl = nil end\n"
    "        cur_hl = new_hl\n"
    "        if cur_hl then span_start = #lines[row] + 1 end\n"
    "      end\n"
    "      i = seq_end + 1\n"
    "    elseif c == '\\n' then\n"
    "      close_span(#lines[row] + 1)\n"
    "      row = row + 1\n"
    "      lines[row] = ''\n"
    "      if cur_hl then span_start = 1 end\n"
    "      i = i + 1\n"
    "    elseif c == '\\r' then\n"
    "      i = i + 1\n"
    "    else\n"
    "      lines[row] = lines[row] .. c\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  close_span(#lines[row] + 1)\n"
    "  return lines, spans\n"
    "end\n"
    "local function mep_term_redraw(job_id)\n"
    "  local sess = mep_term_sessions[job_id]\n"
    "  if not sess then return end\n"
    "  if not mep_term_ns then mep_term_ns = mep.ns_create('terminal') end\n"
    "  local lines, spans = mep_ansi_render(sess.raw)\n"
    "  mep.buffer_set_lines(sess.buffer_id, lines)\n"
    "  mep.buffer_ns_clear(sess.buffer_id, mep_term_ns)\n"
    "  for _, s in ipairs(spans) do\n"
    "    mep.buffer_deco_add(sess.buffer_id, mep_term_ns, {row = s.row, col_start = s.col_start, col_end = s.col_end, hl_group = s.hl})\n"
    "  end\n"
    "end\n"
    // Opens a fresh split, switches it to a new dedicated buffer, and
    // returns that buffer's id -- the shared setup both Run and REPL use.
    "local function mep_term_open_pane(title)\n"
    "  local buf_id = mep.buffer_new()\n"
    "  mep.cmd('split')\n"
    "  mep.buffer_switch(buf_id)\n"
    "  return buf_id\n"
    "end\n"
    "function mep.run_file()\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  local cmd = ft and mep.run_languages[ft]\n"
    "  if not cmd then mep.notify('No run command for this filetype', 'warn') return end\n"
    "  local buf_id = mep_term_open_pane('Run')\n"
    "  local argv = {}\n"
    "  for _, a in ipairs(cmd) do argv[#argv + 1] = a end\n"
    "  argv[#argv + 1] = mep.filename()\n"
    // `job_id` must be declared *before* the table literal below: a
    // closure inside `mep.term_start(argv, {...})`'s argument expression
    // captures whatever `job_id` resolves to at the point the closure
    // literal is compiled -- if `local job_id = mep.term_start(...)`
    // declares it on the same statement, the closures inside that same
    // expression close over a not-yet-existing local (nil/global) instead
    // of the one about to be assigned. A real bug caught during
    // verification (`attempt to index a nil value (local 'sess')` the
    // first time this ran) and fixed by declaring `job_id` first.
    "  local job_id\n"
    "  job_id = mep.term_start(argv, {\n"
    "    cwd = '.',\n"
    "    on_stdout_raw = function(chunk)\n"
    "      local sess = mep_term_sessions[job_id]\n"
    "      sess.raw = sess.raw .. chunk\n"
    "      mep_term_redraw(job_id)\n"
    "    end,\n"
    "    on_exit = function(code)\n"
    "      local sess = mep_term_sessions[job_id]\n"
    "      if sess then sess.raw = sess.raw .. '\\n[exited ' .. code .. ']'; mep_term_redraw(job_id) end\n"
    "    end,\n"
    "  })\n"
    "  mep_term_sessions[job_id] = {raw = '', buffer_id = buf_id}\n"
    "end\n"
    "mep.command('MepRun', mep.run_file)\n"
    // filetype -> job_id
    "local mep_repl_sessions = {}\n"
    "function mep.repl_start(lang)\n"
    "  lang = lang or mep_lsp_filetype(mep.filename())\n"
    "  local cmd = lang and mep.repl_languages[lang]\n"
    "  if not cmd then mep.notify('No REPL for ' .. tostring(lang), 'warn') return end\n"
    "  if mep_repl_sessions[lang] and mep.lsp_is_running(mep_repl_sessions[lang]) then\n"
    "    mep.notify('REPL already running for ' .. lang) return\n"
    "  end\n"
    "  local buf_id = mep_term_open_pane('REPL: ' .. lang)\n"
    "  local job_id\n"  // see mep.run_file's comment on why this must be pre-declared
    "  job_id = mep.term_start(cmd, {\n"
    "    cwd = '.',\n"
    "    on_stdout_raw = function(chunk)\n"
    "      local sess = mep_term_sessions[job_id]\n"
    "      if sess then sess.raw = sess.raw .. chunk; mep_term_redraw(job_id) end\n"
    "    end,\n"
    "    on_exit = function() mep_repl_sessions[lang] = nil end,\n"
    "  })\n"
    "  mep_term_sessions[job_id] = {raw = '', buffer_id = buf_id}\n"
    "  mep_repl_sessions[lang] = job_id\n"
    "end\n"
    "function mep.repl_send(text)\n"
    "  local lang = mep_lsp_filetype(mep.filename())\n"
    "  local job_id = lang and mep_repl_sessions[lang]\n"
    "  if not job_id then mep.notify('No REPL running -- :lua mep.repl_start() first', 'warn') return end\n"
    "  mep.job_write(job_id, text .. '\\n')\n"
    "end\n"
    "function mep.repl_send_line()\n"
    "  mep.repl_send(mep.get_line(mep.cursor()))\n"
    "end\n"
    "function mep.repl_send_buffer()\n"
    "  local lines = {}\n"
    "  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end\n"
    "  mep.repl_send(table.concat(lines, '\\n'))\n"
    "end\n"
    "mep.command('MepReplStart', function() mep.repl_start() end)\n"
    "mep.command('MepReplSendLine', mep.repl_send_line)\n"
    "mep.command('MepReplSendBuffer', mep.repl_send_buffer)\n";

// Markdown rendering (Phase 28), **scoped down**: heading colors + sign-
// column level glyph, checkbox toggle, fenced-code-block shading + Phase
// 5 folding (heading-depth folding too), link/emphasis concealment,
// front-matter shading. **The GFM pipe-table box-drawn overlay renderer
// -- the plan's own "hardest single piece in this phase" -- is not
// implemented**; `|`-table lines render as plain text, same as upstream
// mep.nvim's own documented behavior before Phase 30's org tables design
// lands. Each sub-feature is its own function (independently callable/
// toggleable per the plan's ask, just not wired to individual per-feature
// enable flags -- `:MepMarkdown` runs all of them together).
const char *kBuiltinMarkdown =
    "local mep_md_ns = nil\n"
    "local function mep_md_ns_get()\n"
    "  if not mep_md_ns then mep_md_ns = mep.ns_create('markdown') end\n"
    "  return mep_md_ns\n"
    "end\n"
    "function mep.md_toggle_checkbox()\n"
    "  local row = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local pre, mark = line:match('^(%s*[%-%*%+]%s*%[)([ xX])')\n"
    "  if not pre then mep.notify('No checkbox on this line', 'warn') return end\n"
    "  local newmark = (mark == ' ') and 'x' or ' '\n"
    "  mep.set_line(row, line:sub(1, #pre) .. newmark .. line:sub(#pre + 2))\n"
    "end\n"
    "mep.command('MepMdCheckbox', mep.md_toggle_checkbox)\n"
    "function mep.md_fold()\n"
    "  mep.fold_clear_provider('markdown')\n"
    // stack entries: {level, row}
    "  local stack = {}\n"
    "  local in_fence = false\n"
    "  local fence_start = nil\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    if line:match('^```') then\n"
    "      if in_fence then\n"
    "        mep.fold_create(fence_start, i, true, 'markdown')\n"
    "        in_fence = false\n"
    "      else\n"
    "        in_fence = true\n"
    "        fence_start = i\n"
    "      end\n"
    "    elseif not in_fence then\n"
    "      local hashes = line:match('^(#+)%s')\n"
    "      if hashes then\n"
    "        local level = #hashes\n"
    "        while #stack > 0 and stack[#stack].level >= level do\n"
    "          local top = table.remove(stack)\n"
    "          if top.row < i - 1 then mep.fold_create(top.row, i - 1, true, 'markdown') end\n"
    "        end\n"
    "        stack[#stack + 1] = {level = level, row = i}\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  local n = mep.line_count()\n"
    "  while #stack > 0 do\n"
    "    local top = table.remove(stack)\n"
    "    if top.row < n then mep.fold_create(top.row, n, true, 'markdown') end\n"
    "  end\n"
    "end\n"
    "function mep.md_highlight()\n"
    "  local ns = mep_md_ns_get()\n"
    "  mep.ns_clear(ns)\n"
    "  local in_fence, fence_hl = false, nil\n"
    "  local in_frontmatter = false\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    if i == 1 and line == '---' then\n"
    "      in_frontmatter = true\n"
    "      mep.deco_add(ns, {row = i, whole_line = true, hl_group = 'Comment'})\n"
    "    elseif in_frontmatter then\n"
    "      mep.deco_add(ns, {row = i, whole_line = true, hl_group = 'Comment'})\n"
    "      if line == '---' then in_frontmatter = false end\n"
    "    elseif line:match('^```') then\n"
    "      in_fence = not in_fence\n"
    "      mep.deco_add(ns, {row = i, whole_line = true, hl_group = 'Comment'})\n"
    "    elseif in_fence then\n"
    "      mep.deco_add(ns, {row = i, whole_line = true, hl_group = 'Green'})\n"
    "    else\n"
    "      local hashes = line:match('^(#+)%s')\n"
    "      if hashes then\n"
    "        local level = math.min(#hashes, 6)\n"
    "        local glyph = tostring(level)\n"
    "        mep.deco_add(ns, {row = i, whole_line = true, hl_group = 'Purple', sign = glyph, sign_hl = 'Purple'})\n"
    "      else\n"
    "        for s, e in line:gmatch('()%*%*[^%*]+%*%*()') do\n"
    "          mep.deco_add(ns, {row = i, col_start = s, col_end = e, hl_group = 'Yellow'})\n"
    "        end\n"
    "        for s, e in line:gmatch('()%[[^%]]*%]%([^%)]*%)()') do\n"
    "          mep.deco_add(ns, {row = i, col_start = s, col_end = e, hl_group = 'Blue'})\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "function mep.md_render()\n"
    "  mep.md_highlight()\n"
    "  mep.md_fold()\n"
    "end\n"
    "mep.command('MepMarkdown', mep.md_render)\n";

// Org-mode A: outline, folding, TODO/tags/properties/checkboxes (Phase
// 29) -- the foundation every later org phase builds on, implemented as a
// pure line-pattern headline model (`^%*+ `) over plain buffer text, no
// new C++: folding (Phase 5), decorations (Phase 4), and prompts
// (Phase 3) already cover everything this needs.
const char *kBuiltinOrg =
    "mep.org_todo_keywords = {'TODO', 'DOING', 'DONE'}\n"
    "function mep_org_parse_headline(line)\n"
    "  local stars, rest = line:match('^(%*+)%s+(.*)$')\n"
    "  if not stars then return nil end\n"
    "  local todo = nil\n"
    "  for _, kw in ipairs(mep.org_todo_keywords) do\n"
    "    if rest:sub(1, #kw + 1) == kw .. ' ' then todo = kw; rest = rest:sub(#kw + 2); break end\n"
    "  end\n"
    "  local priority = rest:match('^%[#(%a)%]%s*')\n"
    "  if priority then rest = rest:gsub('^%[#%a%]%s*', '') end\n"
    "  local tags = rest:match(':([%w_:@]+):%s*$')\n"
    "  local title = tags and rest:gsub(':[%w_:@]+:%s*$', ''):gsub('%s+$', '') or rest\n"
    "  return {level = #stars, todo = todo, priority = priority, title = title, tags = tags}\n"
    "end\n"
    "function mep.org_is_headline(row) return mep_org_parse_headline(mep.get_line(row)) ~= nil end\n"
    "function mep.org_headline_level(row)\n"
    "  local h = mep_org_parse_headline(mep.get_line(row))\n"
    "  return h and h.level\n"
    "end\n"
    "function mep_org_current_headline_row(row)\n"
    "  row = row or mep.cursor()\n"
    "  for i = row, 1, -1 do if mep.org_is_headline(i) then return i end end\n"
    "  return nil\n"
    "end\n"
    // Exclusive end: the next headline at level <= this one's, or EOF+1.
    "function mep_org_subtree_end(row)\n"
    "  local level = mep.org_headline_level(row)\n"
    "  for i = row + 1, mep.line_count() do\n"
    "    local l = mep.org_headline_level(i)\n"
    "    if l and l <= level then return i end\n"
    "  end\n"
    "  return mep.line_count() + 1\n"
    "end\n"
    "function mep.org_next_headline()\n"
    "  for i = mep.cursor() + 1, mep.line_count() do\n"
    "    if mep.org_is_headline(i) then mep.set_cursor(i, 1) return end\n"
    "  end\n"
    "end\n"
    "function mep.org_prev_headline()\n"
    "  for i = mep.cursor() - 1, 1, -1 do\n"
    "    if mep.org_is_headline(i) then mep.set_cursor(i, 1) return end\n"
    "  end\n"
    "end\n"
    "function mep.org_promote()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local line = mep.get_line(row)\n"
    "  if #(line:match('^(%*+)')) <= 1 then mep.notify('Already at top level', 'warn') return end\n"
    "  mep.set_line(row, line:sub(2))\n"
    "end\n"
    "function mep.org_demote()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if row then mep.set_line(row, '*' .. mep.get_line(row)) end\n"
    "end\n"
    "local function mep_org_shift_subtree(delta)\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  if delta < 0 and #(mep.get_line(row):match('^(%*+)')) <= 1 then\n"
    "    mep.notify('Already at top level', 'warn') return\n"
    "  end\n"
    "  local e = mep_org_subtree_end(row) - 1\n"
    "  for i = row, e do\n"
    "    local line = mep.get_line(i)\n"
    "    if line:match('^%*+') then\n"
    "      mep.set_line(i, delta > 0 and ('*' .. line) or line:sub(2))\n"
    "    end\n"
    "  end\n"
    "end\n"
    "function mep.org_promote_subtree() mep_org_shift_subtree(-1) end\n"
    "function mep.org_demote_subtree() mep_org_shift_subtree(1) end\n"
    "function mep.org_todo_cycle()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local h = mep_org_parse_headline(mep.get_line(row))\n"
    "  local idx = 0\n"
    "  for i, kw in ipairs(mep.org_todo_keywords) do if kw == h.todo then idx = i end end\n"
    "  local next_kw = mep.org_todo_keywords[idx + 1]\n"
    "  local stars, rest = mep.get_line(row):match('^(%*+%s+)(.*)$')\n"
    "  if h.todo then rest = rest:sub(#h.todo + 2) end\n"
    "  if next_kw then rest = next_kw .. ' ' .. rest end\n"
    "  mep.set_line(row, stars .. rest)\n"
    "end\n"
    "function mep.org_priority_cycle()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local line = mep.get_line(row)\n"
    "  local cur = line:match('%[#(%a)%]')\n"
    "  local order, idx = {'A', 'B', 'C'}, 1\n"
    "  for i, p in ipairs({'A', 'B', 'C'}) do if p == cur then idx = i + 1 end end\n"
    "  local newp = order[idx]\n"
    "  line = line:gsub('%[#%a%]%s*', '')\n"
    "  if newp then\n"
    "    local stars, rest = line:match('^(%*+%s+)(.*)$')\n"
    "    line = stars .. '[#' .. newp .. '] ' .. rest\n"
    "  end\n"
    "  mep.set_line(row, line)\n"
    "end\n"
    "function mep.org_set_tags()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local h = mep_org_parse_headline(mep.get_line(row))\n"
    "  mep.ui_input('Tags (colon-separated):', h.tags or '', function(tags)\n"
    "    if not tags then return end\n"
    "    local line = mep.get_line(row):gsub(':[%w_:@]+:%s*$', ''):gsub('%s+$', '')\n"
    "    if tags ~= '' then line = line .. '  :' .. tags .. ':' end\n"
    "    mep.set_line(row, line)\n"
    "  end)\n"
    "end\n"
    // Own tags plus every ancestor's (org tag inheritance).
    "function mep.org_tags_at(row)\n"
    "  local h = mep_org_parse_headline(mep.get_line(row))\n"
    "  if not h then return {} end\n"
    "  local seen, level = {}, h.level\n"
    "  if h.tags then for t in h.tags:gmatch('[^:]+') do seen[t] = true end end\n"
    "  for i = row - 1, 1, -1 do\n"
    "    local hl = mep_org_parse_headline(mep.get_line(i))\n"
    "    if hl and hl.level < level then\n"
    "      if hl.tags then for t in hl.tags:gmatch('[^:]+') do seen[t] = true end end\n"
    "      level = hl.level\n"
    "    end\n"
    "  end\n"
    "  local out = {}\n"
    "  for t, _ in pairs(seen) do out[#out + 1] = t end\n"
    "  table.sort(out)\n"
    "  return out\n"
    "end\n"
    "function mep.org_tags_picker()\n"
    "  local all = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h and h.tags then for t in h.tags:gmatch('[^:]+') do all[t] = true end end\n"
    "  end\n"
    "  local items = {}\n"
    "  for t, _ in pairs(all) do items[#items + 1] = t end\n"
    "  table.sort(items)\n"
    "  if #items == 0 then mep.notify('No tags in this buffer') return end\n"
    "  mep.picker_open('Tags', items, function(tag)\n"
    "    if tag then mep.org_sparse_tree(function(h) return h.tags and h.tags:find(':' .. tag .. ':', 1, true) end) end\n"
    "  end)\n"
    "end\n"
    // Property drawers (:PROPERTIES: ... :END:), buffer-local.
    "function mep.org_property_get(row, key)\n"
    "  row = row or mep_org_current_headline_row()\n"
    "  if not row then return nil end\n"
    "  local e, in_drawer = mep_org_subtree_end(row), false\n"
    "  for i = row + 1, e - 1 do\n"
    "    local line = mep.get_line(i)\n"
    "    if line:match('^%s*:PROPERTIES:%s*$') then in_drawer = true\n"
    "    elseif line:match('^%s*:END:%s*$') then break\n"
    "    elseif in_drawer then\n"
    "      local k, v = line:match('^%s*:([%w_]+):%s*(.*)$')\n"
    "      if k and k:upper() == key:upper() then return v end\n"
    "    end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function mep.org_property_set(row, key, value)\n"
    "  row = row or mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local e = mep_org_subtree_end(row)\n"
    "  local drawer_start, drawer_end = nil, nil\n"
    "  for i = row + 1, e - 1 do\n"
    "    local line = mep.get_line(i)\n"
    "    if line:match('^%s*:PROPERTIES:%s*$') then drawer_start = i\n"
    "    elseif line:match('^%s*:END:%s*$') and drawer_start then drawer_end = i break end\n"
    "  end\n"
    "  if not drawer_start then\n"
    "    mep.replace_lines(row + 1, row + 1, {':PROPERTIES:', ':END:'})\n"
    "    drawer_start, drawer_end = row + 1, row + 2\n"
    "  end\n"
    "  for i = drawer_start + 1, drawer_end - 1 do\n"
    "    local k = mep.get_line(i):match('^%s*:([%w_]+):')\n"
    "    if k and k:upper() == key:upper() then mep.set_line(i, ':' .. key .. ': ' .. value) return end\n"
    "  end\n"
    "  mep.replace_lines(drawer_end, drawer_end, {':' .. key .. ': ' .. value})\n"
    "end\n"
    "function mep.org_property_remove(row, key)\n"
    "  row = row or mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local e = mep_org_subtree_end(row)\n"
    "  for i = row + 1, e - 1 do\n"
    "    local k = mep.get_line(i):match('^%s*:([%w_]+):')\n"
    "    if k and k:upper() == key:upper() then mep.replace_lines(i, i + 1, {}) return end\n"
    "  end\n"
    "end\n"
    // Checkbox toggle (same syntax as Phase 28's markdown checkbox) +
    // ancestor statistics-cookie ([n/m] / [n%]) recompute.
    "function mep.org_update_statistics_cookie()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local e, total, done = mep_org_subtree_end(row) - 1, 0, 0\n"
    "  for i = row + 1, e do\n"
    "    local mark = mep.get_line(i):match('^%s*[%-%*%+]%s*%[([ xX])%]')\n"
    "    if mark then\n"
    "      total = total + 1\n"
    "      if mark ~= ' ' then done = done + 1 end\n"
    "    end\n"
    "  end\n"
    "  if total == 0 then return end\n"
    "  local line = mep.get_line(row)\n"
    "  if line:match('%[%d+/%d+%]') then\n"
    "    mep.set_line(row, (line:gsub('%[%d+/%d+%]', '[' .. done .. '/' .. total .. ']')))\n"
    "  elseif line:match('%[%d+%%%%%]') then\n"
    "    mep.set_line(row, (line:gsub('%[%d+%%%%%]', '[' .. math.floor(done / total * 100) .. '%%]')))\n"
    "  end\n"
    "end\n"
    "function mep.org_toggle_checkbox()\n"
    "  mep.md_toggle_checkbox()\n"
    "  mep.org_update_statistics_cookie()\n"
    "end\n"
    // Headline-depth folding (all headlines closed) + <Tab>-equivalent
    // per-headline toggle (reuses za's C++ implementation directly).
    "function mep.org_fold_all()\n"
    "  mep.fold_clear_provider('org')\n"
    "  local stack = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h then\n"
    "      while #stack > 0 and stack[#stack].level >= h.level do\n"
    "        local top = table.remove(stack)\n"
    "        if top.row < i - 1 then mep.fold_create(top.row, i - 1, true, 'org') end\n"
    "      end\n"
    "      stack[#stack + 1] = {level = h.level, row = i}\n"
    "    end\n"
    "  end\n"
    "  local n = mep.line_count()\n"
    "  while #stack > 0 do\n"
    "    local top = table.remove(stack)\n"
    "    if top.row < n then mep.fold_create(top.row, n, true, 'org') end\n"
    "  end\n"
    "end\n"
    "function mep.org_cycle() mep.fold_toggle() end\n"
    // Narrow/widen: a fold-based approximation, per the plan's own note.
    "function mep.org_narrow()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local e = mep_org_subtree_end(row) - 1\n"
    "  mep.fold_clear_provider('org-narrow')\n"
    "  if row > 1 then mep.fold_create(1, row - 1, true, 'org-narrow') end\n"
    "  if e < mep.line_count() then mep.fold_create(e + 1, mep.line_count(), true, 'org-narrow') end\n"
    "end\n"
    "function mep.org_widen() mep.fold_clear_provider('org-narrow') end\n"
    // Sparse tree: fold every subtree that contains no match and isn't
    // itself a match's ancestor path, leaving matches (and the headlines
    // leading to them) visible.
    "function mep.org_sparse_tree(predicate)\n"
    "  mep.fold_clear_provider('org')\n"
    "  local matches = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h and predicate(h, i) then matches[i] = true end\n"
    "  end\n"
    "  local function subtree_has_match(row)\n"
    "    for i = row, mep_org_subtree_end(row) - 1 do if matches[i] then return true end end\n"
    "    return false\n"
    "  end\n"
    "  local stack = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h then\n"
    "      while #stack > 0 and stack[#stack].level >= h.level do\n"
    "        local top = table.remove(stack)\n"
    "        if not subtree_has_match(top.row) and top.row < i - 1 then\n"
    "          mep.fold_create(top.row, i - 1, true, 'org')\n"
    "        end\n"
    "      end\n"
    "      stack[#stack + 1] = {level = h.level, row = i}\n"
    "    end\n"
    "  end\n"
    "  local n = mep.line_count()\n"
    "  while #stack > 0 do\n"
    "    local top = table.remove(stack)\n"
    "    if not subtree_has_match(top.row) and top.row < n then mep.fold_create(top.row, n, true, 'org') end\n"
    "  end\n"
    "end\n"
    "function mep.org_sparse_tree_todo() mep.org_sparse_tree(function(h) return h.todo ~= nil end) end\n"
    // Tag-match predicate parser: `+tag-tag|tag2` (AND within a `|`-
    // separated group, `-` negates, no parenthesized grouping -- matches
    // mep.nvim's own documented simplification). Shared by Phase 29's
    // sparse-tree search and Phase 37's roam tag filtering.
    "function mep.org_tag_match(expr)\n"
    "  local or_groups = {}\n"
    "  for group in expr:gmatch('[^|]+') do\n"
    "    local terms = {}\n"
    "    for sign, tag in group:gmatch('([%+%-]?)([%w_@]+)') do\n"
    "      terms[#terms + 1] = {negate = sign == '-', tag = tag}\n"
    "    end\n"
    "    if #terms > 0 then or_groups[#or_groups + 1] = terms end\n"
    "  end\n"
    "  return function(row)\n"
    "    local set = {}\n"
    "    for _, t in ipairs(mep.org_tags_at(row)) do set[t] = true end\n"
    "    for _, terms in ipairs(or_groups) do\n"
    "      local ok = true\n"
    "      for _, term in ipairs(terms) do\n"
    "        if term.negate == (set[term.tag] == true) then ok = false break end\n"
    "      end\n"
    "      if ok then return true end\n"
    "    end\n"
    "    return false\n"
    "  end\n"
    "end\n"
    "function mep.org_sparse_tree_match(expr)\n"
    "  local matcher = mep.org_tag_match(expr)\n"
    "  mep.org_sparse_tree(function(h, row) return matcher(row) end)\n"
    "end\n"
    "mep.command('MepOrgMatch', function()\n"
    "  mep.ui_input('Tag match (e.g. +work-done|urgent):', '', function(expr)\n"
    "    if expr and expr ~= '' then mep.org_sparse_tree_match(expr) end\n"
    "  end)\n"
    "end)\n"
    "mep.command('MepOrgNext', mep.org_next_headline)\n"
    "mep.command('MepOrgPrev', mep.org_prev_headline)\n"
    "mep.command('MepOrgPromote', mep.org_promote)\n"
    "mep.command('MepOrgDemote', mep.org_demote)\n"
    "mep.command('MepOrgPromoteSubtree', mep.org_promote_subtree)\n"
    "mep.command('MepOrgDemoteSubtree', mep.org_demote_subtree)\n"
    "mep.command('MepOrgTodo', mep.org_todo_cycle)\n"
    "mep.command('MepOrgPriority', mep.org_priority_cycle)\n"
    "mep.command('MepOrgTags', mep.org_set_tags)\n"
    "mep.command('MepOrgTagsPicker', mep.org_tags_picker)\n"
    "mep.command('MepOrgCheckbox', mep.org_toggle_checkbox)\n"
    "mep.command('MepOrgFold', mep.org_fold_all)\n"
    "mep.command('MepOrgCycle', mep.org_cycle)\n"
    "mep.command('MepOrgNarrow', mep.org_narrow)\n"
    "mep.command('MepOrgWiden', mep.org_widen)\n"
    "mep.command('MepOrgSparseTodo', mep.org_sparse_tree_todo)\n"
    // Sibling sort (alpha/TODO/priority): the sibling group is found by
    // walking from `row`'s nearest shallower ancestor (or the top of the
    // file), then scanning forward, jumping each headline straight to
    // its own subtree_end -- this correctly skips both this headline's
    // own descendants AND any deeper headlines nested under an
    // intermediate sibling, without them being mistaken for siblings.
    "function mep.org_sort_siblings(mode)\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return end\n"
    "  local level = mep.org_headline_level(row)\n"
    "  local anc = nil\n"
    "  for i = row - 1, 1, -1 do\n"
    "    local hl = mep.org_headline_level(i)\n"
    "    if hl and hl < level then anc = i break end\n"
    "  end\n"
    "  local scope_start = anc and (anc + 1) or 1\n"
    "  local scope_end = anc and mep_org_subtree_end(anc) or (mep.line_count() + 1)\n"
    "  local blocks, i = {}, scope_start\n"
    "  while i < scope_end do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h then\n"
    "      local e = mep_org_subtree_end(i)\n"
    "      if h.level == level then\n"
    "        local lines = {}\n"
    "        for k = i, e - 1 do lines[#lines + 1] = mep.get_line(k) end\n"
    "        blocks[#blocks + 1] = {headline = h, lines = lines}\n"
    "      end\n"
    "      i = e\n"
    "    else\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  if #blocks <= 1 then mep.notify('Nothing to sort') return end\n"
    "  local key\n"
    "  if mode == 'todo' then key = function(b) return b.headline.todo or '~' end\n"
    "  elseif mode == 'priority' then key = function(b) return b.headline.priority or 'Z' end\n"
    "  else key = function(b) return b.headline.title:lower() end end\n"
    "  table.sort(blocks, function(a, b) return key(a) < key(b) end)\n"
    "  local out = {}\n"
    "  for _, b in ipairs(blocks) do for _, l in ipairs(b.lines) do out[#out + 1] = l end end\n"
    "  mep.replace_lines(scope_start, scope_end, out)\n"
    "  mep.notify('Sorted ' .. #blocks .. ' siblings by ' .. mode)\n"
    "end\n"
    "mep.command('MepOrgSortAlpha', function() mep.org_sort_siblings('alpha') end)\n"
    "mep.command('MepOrgSortTodo', function() mep.org_sort_siblings('todo') end)\n"
    "mep.command('MepOrgSortPriority', function() mep.org_sort_siblings('priority') end)\n"
    // Easy templates (<s+Tab-equivalent): since there's no Tab-in-Insert-
    // mode interception wired for this specific gesture, expansion is
    // command-triggered, mirroring how Phase 23's own snippet_trigger
    // already works (a dedicated key/command, not literal Tab).
    "mep.org_easy_templates = {\n"
    "  s = {'#+begin_src %?', '#+end_src'}, e = {'#+begin_example', '#+end_example'},\n"
    "  q = {'#+begin_quote', '#+end_quote'}, c = {'#+begin_center', '#+end_center'},\n"
    "}\n"
    // Cursor position at the point this command normally runs is
    // ambiguous by design: called right after typing the trigger while
    // still in Insert mode, `col` points *past* the last char; called
    // after Escape (the expected normal workflow), vim's own Escape
    // moves the cursor back onto the last-typed char, so `col` points
    // *at* it instead. Try the inclusive slice first, fall back to the
    // exclusive one, so both call sites work.
    "function mep.org_expand_template()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local prefix, after = line:sub(1, col), line:sub(col + 1)\n"
    "  local trig = prefix:match('<(%a)$')\n"
    "  if not trig then\n"
    "    prefix, after = line:sub(1, col - 1), line:sub(col)\n"
    "    trig = prefix:match('<(%a)$')\n"
    "  end\n"
    "  if not trig then mep.notify('No <X template trigger before cursor', 'warn') return end\n"
    "  local tmpl = mep.org_easy_templates[trig]\n"
    "  if not tmpl then mep.notify('Unknown template: <' .. trig, 'warn') return end\n"
    "  local indent = line:match('^%s*') or ''\n"
    "  local before = prefix:sub(1, #prefix - 2)\n"
    "  local open_line = (before .. tmpl[1]):gsub('%%%?', '')\n"
    "  mep.replace_lines(row, row + 1, {open_line, indent .. tmpl[2] .. after})\n"
    "  mep.set_cursor(row, #open_line + 1)\n"
    "end\n"
    "mep.command('MepOrgTemplate', mep.org_expand_template)\n"
    // Repeater occurrence math (Phase 32 agenda needs this to show a
    // recurring SCHEDULED/DEADLINE on its *next* due date, not just its
    // original one forever). `+`/`++` both "catch up" to the first
    // occurrence on/after today by repeatedly adding the interval, which
    // is the correct behavior for both (the difference between them is
    // about *warning-window* display, not the occurrence date itself);
    // `.+` (interval from last completion, not from the stored date)
    // can't be modeled without completion-date tracking, which this
    // implementation doesn't have -- documented as a scope cut.
    "function mep_org_repeater_days(rep)\n"
    "  local n, unit = rep:match('^[%+%.]+(%d+)([dwmy])$')\n"
    "  if not n then return nil end\n"
    "  n = tonumber(n)\n"
    "  if unit == 'd' then return n\n"
    "  elseif unit == 'w' then return n * 7\n"
    "  elseif unit == 'm' then return n * 30\n"
    "  elseif unit == 'y' then return n * 365 end\n"
    "end\n"
    "function mep.org_next_occurrence(ts, today_str)\n"
    "  local y, mo, d = ts:match('(%d%d%d%d)-(%d%d)-(%d%d)')\n"
    "  if not y then return ts end\n"
    "  local rep = ts:match('([%+%.]+%d+[dwmy])')\n"
    "  if not rep then return y .. '-' .. mo .. '-' .. d end\n"
    "  local days = mep_org_repeater_days(rep)\n"
    "  if not days then return y .. '-' .. mo .. '-' .. d end\n"
    "  local t = os.time({year = tonumber(y), month = tonumber(mo), day = tonumber(d), hour = 12})\n"
    "  local today = (today_str or os.date('%Y-%m-%d')):match('%d%d%d%d%-%d%d%-%d%d')\n"
    "  while os.date('%Y-%m-%d', t) < today do t = t + days * 86400 end\n"
    "  return os.date('%Y-%m-%d', t)\n"
    "end\n";

// Org-mode B: tables (new design, mep.nvim has none), links, footnotes,
// timestamps/scheduling (Phase 30). Reuses the (now-global) headline
// helpers from kBuiltinOrg, mep.open_url (Phase 13) for URL/mailto
// targets, and mep.pane_open (Phase 14) for file: targets.
const char *kBuiltinOrgLinks =
    // Table alignment: | a | b | -> padded columns, |---+---| separators
    // recomputed to match. Operates on the contiguous run of `|`-lines
    // touching the cursor.
    "local function mep_org_table_row(line)\n"
    "  if not line:match('^%s*|') then return nil end\n"
    "  if line:match('^%s*|%-') then return 'sep' end\n"
    "  local inner = line:match('^%s*|(.-)|?%s*$')\n"
    "  local cells = {}\n"
    "  for cell in (inner .. '|'):gmatch('(.-)|') do cells[#cells + 1] = cell:match('^%s*(.-)%s*$') end\n"
    "  return cells\n"
    "end\n"
    "function mep.org_table_align()\n"
    "  local row = mep.cursor()\n"
    "  if not mep_org_table_row(mep.get_line(row)) then mep.notify('Not on a table row', 'warn') return end\n"
    "  local top = row\n"
    "  while top > 1 and mep_org_table_row(mep.get_line(top - 1)) do top = top - 1 end\n"
    "  local bot, n = row, mep.line_count()\n"
    "  while bot < n and mep_org_table_row(mep.get_line(bot + 1)) do bot = bot + 1 end\n"
    "  local widths, rows = {}, {}\n"
    "  for i = top, bot do\n"
    "    local r = mep_org_table_row(mep.get_line(i))\n"
    "    rows[#rows + 1] = {i, r}\n"
    "    if r ~= 'sep' then\n"
    "      for ci, cell in ipairs(r) do widths[ci] = math.max(widths[ci] or 0, #cell) end\n"
    "    end\n"
    "  end\n"
    "  for _, entry in ipairs(rows) do\n"
    "    local i, r = entry[1], entry[2]\n"
    "    local parts = {}\n"
    "    if r == 'sep' then\n"
    "      for _, w in ipairs(widths) do parts[#parts + 1] = string.rep('-', w + 2) end\n"
    "      mep.set_line(i, '|' .. table.concat(parts, '+') .. '|')\n"
    "    else\n"
    "      for ci, w in ipairs(widths) do\n"
    "        local cell = r[ci] or ''\n"
    "        parts[#parts + 1] = ' ' .. cell .. string.rep(' ', w - #cell) .. ' '\n"
    "      end\n"
    "      mep.set_line(i, '|' .. table.concat(parts, '|') .. '|')\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepOrgTableAlign', mep.org_table_align)\n"
    // Links: [[target]] / [[target][description]].
    "function mep.org_link_at_cursor()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local pos = 1\n"
    "  while true do\n"
    "    local s, e, inner = line:find('%[%[(.-)%]%]', pos)\n"
    "    if not s then return nil end\n"
    "    if col >= s and col <= e then\n"
    "      local target, desc = inner:match('^([^%]]+)%]%[(.+)$')\n"
    "      return target or inner, desc\n"
    "    end\n"
    "    pos = e + 1\n"
    "  end\n"
    "end\n"
    "function mep.org_link_insert()\n"
    "  mep.ui_input('Link target:', '', function(target)\n"
    "    if not target or target == '' then return end\n"
    "    mep.ui_input('Description (optional):', '', function(desc)\n"
    "      local text = (desc and desc ~= '') and ('[[' .. target .. '][' .. desc .. ']]') or ('[[' .. target .. ']]')\n"
    "      mep.insert_text(text)\n"
    "    end)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgLinkInsert', mep.org_link_insert)\n"
    // Follow: dispatch by target-type prefix.
    "function mep.org_link_follow()\n"
    "  local target = mep.org_link_at_cursor()\n"
    "  if not target then mep.notify('No link under cursor', 'warn') return end\n"
    "  if target:match('^https?://') or target:match('^mailto:') then\n"
    "    mep.open_url(target:gsub('^mailto:', ''))\n"
    "  elseif target:match('^file:') then\n"
    "    local rest = target:sub(6)\n"
    "    local path, heading = rest:match('^([^#]*)::%*(.+)$')\n"
    "    local path2, lineno = rest:match('^([^#]*)::(%d+)$')\n"
    "    path = path or path2 or rest\n"
    "    mep.pane_open(path)\n"
    "    if lineno then mep.set_cursor(tonumber(lineno), 1)\n"
    "    elseif heading then\n"
    "      for i = 1, mep.line_count() do\n"
    "        local h = mep_org_parse_headline(mep.get_line(i))\n"
    "        if h and h.title == heading then mep.set_cursor(i, 1) break end\n"
    "      end\n"
    "    end\n"
    "  elseif target:match('^id:') then\n"
    "    local id = target:sub(4)\n"
    "    for i = 1, mep.line_count() do\n"
    "      if mep.org_is_headline(i) and mep.org_property_get(i, 'ID') == id then mep.set_cursor(i, 1) return end\n"
    "    end\n"
    "    mep.notify('ID not found: ' .. id, 'warn')\n"
    "  elseif target:match('^#') then\n"
    "    local id = target:sub(2)\n"
    "    for i = 1, mep.line_count() do\n"
    "      if mep.org_is_headline(i) and mep.org_property_get(i, 'CUSTOM_ID') == id then mep.set_cursor(i, 1) return end\n"
    "    end\n"
    "    mep.notify('CUSTOM_ID not found: ' .. id, 'warn')\n"
    "  else\n"
    "    local heading = target:match('^%*(.+)$') or target\n"
    "    for i = 1, mep.line_count() do\n"
    "      local h = mep_org_parse_headline(mep.get_line(i))\n"
    "      if h and h.title == heading then mep.set_cursor(i, 1) return end\n"
    "    end\n"
    "    mep.notify('Target not found: ' .. target, 'warn')\n"
    "  end\n"
    "end\n"
    "mep.command('MepOrgLinkFollow', mep.org_link_follow)\n"
    // Link concealment: whole-span highlight (matches the markdown
    // approach -- recolor rather than hide the marker characters).
    "local mep_org_link_ns = nil\n"
    "function mep.org_link_highlight()\n"
    "  if not mep_org_link_ns then mep_org_link_ns = mep.ns_create('org-links') end\n"
    "  mep.ns_clear(mep_org_link_ns)\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line, pos = mep.get_line(i), 1\n"
    "    while true do\n"
    "      local s, e = line:find('%[%[.-%]%]', pos)\n"
    "      if not s then break end\n"
    "      mep.deco_add(mep_org_link_ns, {row = i, col_start = s, col_end = e + 1, hl = 'Blue'})\n"
    "      pos = e + 1\n"
    "    end\n"
    "  end\n"
    "end\n"
    // Footnotes: [fn:name] reference, jump to/from its `[fn:name] body`
    // definition line (or the first other reference if no definition).
    "function mep.org_footnote_jump()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local name\n"
    "  local pos = 1\n"
    "  while true do\n"
    "    local s, e, n = line:find('%[fn:([%w_%-]+)%]', pos)\n"
    "    if not s then break end\n"
    "    if col >= s and col <= e then name = n break end\n"
    "    pos = e + 1\n"
    "  end\n"
    "  if not name then mep.notify('No footnote under cursor', 'warn') return end\n"
    "  for i = 1, mep.line_count() do\n"
    "    if i ~= row and mep.get_line(i):match('^%[fn:' .. name .. '%]') then mep.set_cursor(i, 1) return end\n"
    "  end\n"
    "  for i = 1, mep.line_count() do\n"
    "    if i ~= row and mep.get_line(i):find('%[fn:' .. name .. '%]') then mep.set_cursor(i, 1) return end\n"
    "  end\n"
    "  mep.notify('No counterpart found for [fn:' .. name .. ']', 'warn')\n"
    "end\n"
    "mep.command('MepOrgFootnoteJump', mep.org_footnote_jump)\n"
    // Timestamps: <active> / [inactive], insert-at-cursor and
    // increment/decrement-by-day (real calendar math via os.time/date).
    "local function mep_org_timestamp_at(line, col)\n"
    "  local pos = 1\n"
    "  while true do\n"
    "    local s, e, body = line:find('<(%d%d%d%d%-%d%d%-%d%d[^>]-)>', pos)\n"
    "    if not s then break end\n"
    "    if col >= s and col <= e then return s, e, body, true end\n"
    "    pos = e + 1\n"
    "  end\n"
    "  pos = 1\n"
    "  while true do\n"
    "    local s, e, body = line:find('%[(%d%d%d%d%-%d%d%-%d%d[^%]]-)%]', pos)\n"
    "    if not s then break end\n"
    "    if col >= s and col <= e then return s, e, body, false end\n"
    "    pos = e + 1\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function mep.org_timestamp_insert(active)\n"
    "  local row, col = mep.cursor()\n"
    "  local body = os.date('%Y-%m-%d %a')\n"
    "  local ts = active and ('<' .. body .. '>') or ('[' .. body .. ']')\n"
    "  local line = mep.get_line(row)\n"
    "  mep.set_line(row, line:sub(1, col - 1) .. ts .. line:sub(col))\n"
    "  mep.set_cursor(row, col + #ts)\n"
    "end\n"
    "mep.command('MepOrgTimestamp', function() mep.org_timestamp_insert(true) end)\n"
    "mep.command('MepOrgTimestampInactive', function() mep.org_timestamp_insert(false) end)\n"
    "function mep.org_timestamp_shift(delta_days)\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local s, e, body, active = mep_org_timestamp_at(line, col)\n"
    "  if not s then mep.notify('No timestamp under cursor', 'warn') return end\n"
    "  local y, mo, d = body:match('^(%d%d%d%d)-(%d%d)-(%d%d)')\n"
    "  local rest = body:match('^%d%d%d%d%-%d%d%-%d%d%s*%a*(.*)$') or ''\n"
    "  local t = os.time({year = tonumber(y), month = tonumber(mo), day = tonumber(d), hour = 12})\n"
    "  t = t + delta_days * 86400\n"
    "  local newbody = os.date('%Y-%m-%d %a', t) .. rest\n"
    "  local openc, closec = active and '<' or '[', active and '>' or ']'\n"
    "  mep.set_line(row, line:sub(1, s - 1) .. openc .. newbody .. closec .. line:sub(e + 1))\n"
    "end\n"
    "mep.command('MepOrgTimestampIncr', function() mep.org_timestamp_shift(1) end)\n"
    "mep.command('MepOrgTimestampDecr', function() mep.org_timestamp_shift(-1) end)\n"
    // SCHEDULED:/DEADLINE: planning lines, inserted directly below the
    // current headline (creating or replacing an existing planning line).
    "function mep.org_set_planning(kind)\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then return end\n"
    "  local body = os.date('%Y-%m-%d %a')\n"
    "  local text = kind .. ': <' .. body .. '>'\n"
    "  local next_line = mep.get_line(row + 1) or ''\n"
    "  if next_line:match('^%s*' .. kind .. ':') then\n"
    "    mep.set_line(row + 1, text)\n"
    "  else\n"
    "    mep.replace_lines(row + 1, row + 1, {text})\n"
    "  end\n"
    "end\n"
    "mep.command('MepOrgScheduled', function() mep.org_set_planning('SCHEDULED') end)\n"
    "mep.command('MepOrgDeadline', function() mep.org_set_planning('DEADLINE') end)\n";

// Org-mode C: capture, refile, archive (Phase 31). Capture reuses
// mep.ui_input for %^{PROMPT} placeholders and mep.picker_open for the
// template picker; refile/archive both reuse mep.pane_open (Phase 14) to
// touch the target file, which need not be the buffer currently open.
const char *kBuiltinOrgCapture =
    "mep.org_capture_templates = {\n"
    "  {key = 't', name = 'Task', template = '* TODO %?\\n  %U'},\n"
    "  {key = 'n', name = 'Note', template = '* %?\\n  %U'},\n"
    "}\n"
    "function mep_org_expand_template(tmpl)\n"
    "  local out = tmpl\n"
    "  out = out:gsub('%%U', '[' .. os.date('%Y-%m-%d %a %H:%M') .. ']')\n"
    "  out = out:gsub('%%u', '[' .. os.date('%Y-%m-%d %a') .. ']')\n"
    "  out = out:gsub('%%T', '<' .. os.date('%Y-%m-%d %a %H:%M') .. '>')\n"
    "  out = out:gsub('%%t', '<' .. os.date('%Y-%m-%d %a') .. '>')\n"
    "  out = out:gsub('%%a', '[[file:' .. (mep.filename() or '') .. ']]')\n"
    "  out = out:gsub('%%%%', '%%')\n"
    "  return out\n"
    "end\n"
    "function mep.org_capture()\n"
    "  local items = {}\n"
    "  for _, t in ipairs(mep.org_capture_templates) do items[#items + 1] = t.key .. ' - ' .. t.name end\n"
    "  mep.picker_open('Capture', items, function(choice)\n"
    "    if not choice then return end\n"
    "    local key = choice:sub(1, 1)\n"
    "    local tmpl\n"
    "    for _, t in ipairs(mep.org_capture_templates) do if t.key == key then tmpl = t end end\n"
    "    if not tmpl then return end\n"
    "    local expanded = mep_org_expand_template(tmpl.template)\n"
    "    local prompts = {}\n"
    "    expanded = expanded:gsub('%%%^{(.-)}', function(label)\n"
    "      prompts[#prompts + 1] = label\n"
    "      return '\\0PROMPT' .. #prompts .. '\\0'\n"
    "    end)\n"
    "    local function finish(text)\n"
    "      text = text:gsub('%%%?', '')\n"
    "      local target_file = tmpl.file or mep.filename()\n"
    "      if target_file ~= mep.filename() then mep.pane_open(target_file) end\n"
    "      local n = mep.line_count()\n"
    "      local lines = {}\n"
    "      for line in (text .. '\\n'):gmatch('(.-)\\n') do lines[#lines + 1] = line end\n"
    "      mep.replace_lines(n + 1, n + 1, lines)\n"
    "      mep.set_cursor(n + 1, 1)\n"
    "      mep.notify('Captured to ' .. target_file)\n"
    "    end\n"
    "    local function resolve_prompts(idx, text)\n"
    "      if idx > #prompts then finish(text) return end\n"
    "      mep.ui_input(prompts[idx] .. ':', '', function(val)\n"
    "        text = text:gsub('\\0PROMPT' .. idx .. '\\0', val or '')\n"
    "        resolve_prompts(idx + 1, text)\n"
    "      end)\n"
    "    end\n"
    "    resolve_prompts(1, expanded)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgCapture', mep.org_capture)\n"
    // Refile: move the current subtree to become the last child of a
    // headline chosen via picker, re-leveling it to fit one level deeper.
    "function mep.org_refile()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return end\n"
    "  local e = mep_org_subtree_end(row)\n"
    "  local items = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    if i < row or i >= e then\n"
    "      local h = mep_org_parse_headline(mep.get_line(i))\n"
    "      if h then items[#items + 1] = {display = string.rep('  ', h.level - 1) .. h.title, data = tostring(i)} end\n"
    "    end\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No refile targets', 'warn') return end\n"
    "  local lines = {}\n"
    "  for i = row, e - 1 do lines[#lines + 1] = mep.get_line(i) end\n"
    "  mep.picker_open('Refile to', items, function(data)\n"
    "    if not data then return end\n"
    "    local target_row = tonumber(data)\n"
    "    local target_level = mep.org_headline_level(target_row)\n"
    "    local target_end = mep_org_subtree_end(target_row)\n"
    "    local src_level = mep_org_parse_headline(lines[1]).level\n"
    "    local delta = (target_level + 1) - src_level\n"
    "    local reindented = {}\n"
    "    for _, line in ipairs(lines) do\n"
    "      local stars = line:match('^(%*+)')\n"
    "      if stars and delta ~= 0 then\n"
    "        reindented[#reindented + 1] = string.rep('*', math.max(1, #stars + delta)) .. line:sub(#stars + 1)\n"
    "      else\n"
    "        reindented[#reindented + 1] = line\n"
    "      end\n"
    "    end\n"
    "    if target_row > row then\n"
    "      mep.replace_lines(target_end, target_end, reindented)\n"
    "      mep.replace_lines(row, e, {})\n"
    "    else\n"
    "      mep.replace_lines(row, e, {})\n"
    "      mep.replace_lines(target_end, target_end, reindented)\n"
    "    end\n"
    "    mep.notify('Refiled')\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgRefile', mep.org_refile)\n"
    // Archive: move the subtree to <file>_archive.org with provenance
    // properties recording where/when it came from, then delete it here.
    "function mep.org_archive()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return end\n"
    "  local e = mep_org_subtree_end(row)\n"
    "  local lines = {}\n"
    "  for i = row, e - 1 do lines[#lines + 1] = mep.get_line(i) end\n"
    "  local src_file = mep.filename()\n"
    "  local archive_file = (src_file:gsub('%.org$', '')) .. '_archive.org'\n"
    "  local out = {lines[1],\n"
    "    '  :PROPERTIES:',\n"
    "    '  :ARCHIVE_TIME: ' .. os.date('%Y-%m-%d %a %H:%M'),\n"
    "    '  :ARCHIVE_FILE: ' .. src_file,\n"
    "    '  :ARCHIVE_OLPATH: ' .. lines[1],\n"
    "    '  :END:'}\n"
    "  for i = 2, #lines do out[#out + 1] = lines[i] end\n"
    "  local cur = src_file\n"
    "  mep.pane_open(archive_file)\n"
    "  local n = mep.line_count()\n"
    "  if n == 1 and mep.get_line(1) == '' then n = 0 end\n"
    "  mep.replace_lines(n + 1, n + 1, out)\n"
    "  mep.cmd('w')\n"
    "  mep.pane_open(cur)\n"
    "  mep.replace_lines(row, e, {})\n"
    "  mep.notify('Archived to ' .. archive_file)\n"
    "end\n"
    "mep.command('MepOrgArchive', mep.org_archive)\n";

// Org-mode D: agenda (Phase 32). "Headless" file loading uses Lua's
// stdlib `io` directly (real file reads, no new C++) rather than the
// live buffer for files that aren't currently open -- mep_org_parse_
// headline takes a raw line string, not a buffer row, so it's directly
// reusable against disk-read lines.
const char *kBuiltinOrgAgenda =
    "mep.org_agenda_files = {}\n"
    "function mep_org_read_file_lines(path)\n"
    "  local f = io.open(path, 'r')\n"
    "  if not f then return nil end\n"
    "  local lines = {}\n"
    "  for line in f:lines() do lines[#lines + 1] = line end\n"
    "  f:close()\n"
    "  return lines\n"
    "end\n"
    "local function mep_org_date_of(ts) return ts and ts:match('^(%d%d%d%d%-%d%d%-%d%d)') end\n"
    "local function mep_org_has_repeater(ts) return ts and ts:match('[%+%.]+%d+[dwmy]') ~= nil end\n"
    "local function mep_org_occurs_on(ts, date) return ts ~= nil and mep.org_next_occurrence(ts, date) == date end\n"
    // Glob expansion for agenda_files entries: only `*` within the final
    // path component (no recursive `**`), matched via mep.list_dir
    // (Phase 15) -- entries without a `*` pass through unchanged.
    // Marks each `*` with a control byte first, then escapes every other
    // magic pattern character, then swaps the marker for `.*` -- doing
    // the escape pass directly would have nothing to distinguish a
    // glob's own `*` from one already-escaped, since a bare `*` isn't in
    // the escaped-character set to begin with.
    "local function mep_org_glob_to_pattern(glob)\n"
    "  local marked = glob:gsub('%*', '\\1')\n"
    "  local escaped = marked:gsub('([%.%-%+%[%]%(%)%$%^%%])', '%%%1')\n"
    "  return '^' .. escaped:gsub('\\1', '.*') .. '$'\n"
    "end\n"
    "local function mep_org_expand_glob(pattern)\n"
    "  local dir, filepat = pattern:match('^(.*)/([^/]*)$')\n"
    "  if not dir or not filepat:find('%*') then return {pattern} end\n"
    "  local results = {}\n"
    "  local ok, entries = pcall(mep.list_dir, dir)\n"
    "  if ok and entries then\n"
    "    local pat = mep_org_glob_to_pattern(filepat)\n"
    "    for _, e in ipairs(entries) do\n"
    "      if not e.is_dir and e.name:match(pat) then results[#results + 1] = dir .. '/' .. e.name end\n"
    "    end\n"
    "  end\n"
    "  return results\n"
    "end\n"
    "function mep.org_agenda_add_current()\n"
    "  local f = mep.filename()\n"
    "  for _, p in ipairs(mep.org_agenda_files) do if p == f then mep.notify('Already in agenda files') return end end\n"
    "  mep.org_agenda_files[#mep.org_agenda_files + 1] = f\n"
    "  mep.notify('Added to agenda files: ' .. f)\n"
    "end\n"
    "mep.command('MepOrgAgendaAddFile', mep.org_agenda_add_current)\n"
    // Walks every configured agenda file (literal path or glob pattern)'s
    // headlines, pairing each with any SCHEDULED:/DEADLINE: planning
    // line immediately below it.
    "function mep.org_agenda_collect()\n"
    "  local entries = {}\n"
    "  for _, pattern in ipairs(mep.org_agenda_files) do\n"
    "   for _, path in ipairs(mep_org_expand_glob(pattern)) do\n"
    "    local lines = mep_org_read_file_lines(path)\n"
    "    if lines then\n"
    "      for i, line in ipairs(lines) do\n"
    "        local h = mep_org_parse_headline(line)\n"
    "        if h then\n"
    "          local nextline = lines[i + 1]\n"
    "          local scheduled = nextline and nextline:match('SCHEDULED:%s*<([^>]+)>')\n"
    "          local deadline = nextline and nextline:match('DEADLINE:%s*<([^>]+)>')\n"
    "          entries[#entries + 1] = {file = path, line = i, todo = h.todo, title = h.title,\n"
    "            tags = h.tags, priority = h.priority, scheduled = scheduled, deadline = deadline}\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "   end\n"
    "  end\n"
    "  return entries\n"
    "end\n"
    "local function mep_org_agenda_open_picker(title, items)\n"
    "  if #items == 0 then mep.notify('No agenda entries') return end\n"
    "  mep.picker_open(title, items, function(data)\n"
    "    if not data then return end\n"
    "    local file, line = data:match('^(.*):(%d+)$')\n"
    "    mep.pane_open(file)\n"
    "    mep.set_cursor(tonumber(line), 1)\n"
    "  end)\n"
    "end\n"
    "function mep.org_agenda_todo()\n"
    "  local items = {}\n"
    "  for _, e in ipairs(mep.org_agenda_collect()) do\n"
    "    if e.todo then\n"
    "      items[#items + 1] = {display = e.todo .. '  ' .. e.title .. '  (' .. e.file .. ':' .. e.line .. ')',\n"
    "        data = e.file .. ':' .. e.line}\n"
    "    end\n"
    "  end\n"
    "  mep_org_agenda_open_picker('Agenda: TODO', items)\n"
    "end\n"
    "mep.command('MepOrgAgendaTodo', mep.org_agenda_todo)\n"
    // Occurrence-aware: mep_org_occurs_on resolves a repeater (`+1w`
    // etc.) to its next due date on/after `date` before comparing, so a
    // recurring SCHEDULED/DEADLINE shows up on each real occurrence, not
    // just its original literal date forever.
    "function mep.org_agenda_day(date)\n"
    "  date = date or os.date('%Y-%m-%d')\n"
    "  local items = {}\n"
    "  for _, e in ipairs(mep.org_agenda_collect()) do\n"
    "    local sd, dd = mep_org_occurs_on(e.scheduled, date), mep_org_occurs_on(e.deadline, date)\n"
    "    if sd or dd then\n"
    "      local tag = dd and '[DEADLINE] ' or '[SCHEDULED] '\n"
    "      items[#items + 1] = {display = tag .. (e.todo or '') .. ' ' .. e.title .. '  (' .. e.file .. ':' .. e.line .. ')',\n"
    "        data = e.file .. ':' .. e.line}\n"
    "    end\n"
    "  end\n"
    "  mep_org_agenda_open_picker('Agenda: ' .. date, items)\n"
    "end\n"
    "mep.command('MepOrgAgendaToday', function() mep.org_agenda_day() end)\n"
    "function mep.org_agenda_week()\n"
    "  local items, entries, t = {}, mep.org_agenda_collect(), os.time()\n"
    "  for d = 0, 6 do\n"
    "    local date = os.date('%Y-%m-%d', t + d * 86400)\n"
    "    for _, e in ipairs(entries) do\n"
    "      local sd, dd = mep_org_occurs_on(e.scheduled, date), mep_org_occurs_on(e.deadline, date)\n"
    "      if sd or dd then\n"
    "        local tag = dd and '[DEADLINE] ' or '[SCHEDULED] '\n"
    "        items[#items + 1] = {display = date .. '  ' .. tag .. (e.todo or '') .. ' ' .. e.title,\n"
    "          data = e.file .. ':' .. e.line}\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  mep_org_agenda_open_picker('Agenda: Week', items)\n"
    "end\n"
    "mep.command('MepOrgAgendaWeek', mep.org_agenda_week)\n"
    // Non-repeating deadlines stay listed here every day until the
    // headline's TODO state reaches the last (done) keyword. Repeating
    // deadlines are excluded -- org_next_occurrence always resolves to
    // today-or-later, so a repeater is by definition never "overdue" in
    // the backlog sense; it just shows on its next due date instead
    // (Agenda Day/Week above).
    "function mep.org_agenda_overdue()\n"
    "  local today, items = os.date('%Y-%m-%d'), {}\n"
    "  local done_kw = mep.org_todo_keywords[#mep.org_todo_keywords]\n"
    "  for _, e in ipairs(mep.org_agenda_collect()) do\n"
    "    if e.deadline and not mep_org_has_repeater(e.deadline) then\n"
    "      local dd = mep_org_date_of(e.deadline)\n"
    "      if dd and dd < today and e.todo ~= done_kw then\n"
    "        items[#items + 1] = {display = 'OVERDUE ' .. dd .. '  ' .. (e.todo or '') .. ' ' .. e.title .. '  (' .. e.file .. ':' .. e.line .. ')',\n"
    "          data = e.file .. ':' .. e.line}\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  mep_org_agenda_open_picker('Agenda: Overdue', items)\n"
    "end\n"
    "mep.command('MepOrgAgendaOverdue', mep.org_agenda_overdue)\n"
    "function mep.org_agenda_search(query)\n"
    "  local items, q = {}, query:lower()\n"
    "  for _, e in ipairs(mep.org_agenda_collect()) do\n"
    "    if e.title:lower():find(q, 1, true) or (e.tags and e.tags:lower():find(q, 1, true)) then\n"
    "      items[#items + 1] = {display = (e.todo or '') .. ' ' .. e.title .. '  (' .. e.file .. ':' .. e.line .. ')',\n"
    "        data = e.file .. ':' .. e.line}\n"
    "    end\n"
    "  end\n"
    "  mep_org_agenda_open_picker('Agenda: ' .. query, items)\n"
    "end\n"
    "mep.command('MepOrgAgendaSearch', function()\n"
    "  mep.ui_input('Search:', '', function(q) if q and q ~= '' then mep.org_agenda_search(q) end end)\n"
    "end)\n";

// Org-mode E: clocking (Phase 33) -- sort/narrow/sparse-tree already
// shipped with Phase 29 (mep.org_sparse_tree/org_narrow/org_widen).
// Clock state lives entirely in `CLOCK:` lines inside a :LOGBOOK:
// drawer, found by buffer scan rather than session state, so it's
// durable across restarts by construction (matches the plan's own
// design note).
const char *kBuiltinOrgClock =
    "function mep.org_clock_in()\n"
    "  for i = 1, mep.line_count() do\n"
    "    if mep.get_line(i):match('^%s*CLOCK:%s*%[[^%]]+%]%s*$') then\n"
    "      mep.notify('A clock is already running', 'warn') return\n"
    "    end\n"
    "  end\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return end\n"
    "  local e = mep_org_subtree_end(row)\n"
    "  local logbook_start\n"
    "  for i = row + 1, e - 1 do\n"
    "    if mep.get_line(i):match('^%s*:LOGBOOK:%s*$') then logbook_start = i break end\n"
    "  end\n"
    "  local entry = '  CLOCK: [' .. os.date('%Y-%m-%d %a %H:%M') .. ']'\n"
    "  if not logbook_start then\n"
    "    mep.replace_lines(row + 1, row + 1, {'  :LOGBOOK:', entry, '  :END:'})\n"
    "  else\n"
    "    mep.replace_lines(logbook_start + 1, logbook_start + 1, {entry})\n"
    "  end\n"
    "  mep.notify('Clock started')\n"
    "end\n"
    "mep.command('MepOrgClockIn', mep.org_clock_in)\n"
    "function mep.org_clock_out()\n"
    "  for i = 1, mep.line_count() do\n"
    "    local start_ts = mep.get_line(i):match('^%s*CLOCK:%s*%[([^%]]+)%]%s*$')\n"
    "    if start_ts then\n"
    "      local y, mo, d, hh, mm = start_ts:match('(%d%d%d%d)-(%d%d)-(%d%d) %a+ (%d%d):(%d%d)')\n"
    "      local start_t = os.time({year = tonumber(y), month = tonumber(mo), day = tonumber(d),\n"
    "        hour = tonumber(hh), min = tonumber(mm)})\n"
    "      local mins = math.floor((os.time() - start_t) / 60)\n"
    "      local dur = string.format('%d:%02d', math.floor(mins / 60), mins % 60)\n"
    "      mep.set_line(i, '  CLOCK: [' .. start_ts .. ']--[' .. os.date('%Y-%m-%d %a %H:%M') .. '] =>  ' .. dur)\n"
    "      mep.notify('Clock stopped: ' .. dur)\n"
    "      return\n"
    "    end\n"
    "  end\n"
    "  mep.notify('No running clock', 'warn')\n"
    "end\n"
    "mep.command('MepOrgClockOut', mep.org_clock_out)\n"
    "function mep.org_clock_effort(row)\n"
    "  return mep.org_property_get(row or mep_org_current_headline_row(), 'Effort')\n"
    "end\n"
    // Clock-table: recursive per-headline totals over closed CLOCK
    // entries (`=> H:MM`), summed across the whole subtree (including
    // descendants), matching org's own clocktable semantics.
    "local function mep_org_clock_minutes_in_range(a, b)\n"
    "  local total = 0\n"
    "  for i = a, b do\n"
    "    local h1, h2 = mep.get_line(i):match('=>%s*(%d+):(%d%d)%s*$')\n"
    "    if h1 then total = total + tonumber(h1) * 60 + tonumber(h2) end\n"
    "  end\n"
    "  return total\n"
    "end\n"
    "function mep.org_clock_table()\n"
    "  local items = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h then\n"
    "      local mins = mep_org_clock_minutes_in_range(i, mep_org_subtree_end(i) - 1)\n"
    "      if mins > 0 then\n"
    "        items[#items + 1] = string.rep('  ', h.level - 1) .. h.title .. '  ' ..\n"
    "          string.format('%d:%02d', math.floor(mins / 60), mins % 60)\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No clocked time found') return end\n"
    "  mep.picker_open('Clock Table', items, function() end)\n"
    "end\n"
    "mep.command('MepOrgClockTable', mep.org_clock_table)\n";

// Org-mode F: babel/code execution (Phase 34). Reuses mep.job_start
// (plain-pipe capture, not term_start's PTY) since babel wants captured
// stdout lines for a #+RESULTS: block, not a live ANSI terminal pane.
// Starts with sh/bash/python/lua per the plan's own "start small"
// guidance -- polyglot/LSP-in-src-blocks stays Phase 36, deferred.
const char *kBuiltinOrgBabel =
    "mep.org_babel_langs = {\n"
    "  sh = {'sh'}, bash = {'bash'}, python = {'python3'}, lua = {'lua'},\n"
    "}\n"
    "local mep_org_babel_cache = {}\n"
    "local function mep_org_src_block_at(row)\n"
    "  local start_row\n"
    "  for i = row, 1, -1 do\n"
    "    if mep.get_line(i):match('^%s*#%+begin_src') then start_row = i break end\n"
    "    if mep.get_line(i):match('^%s*#%+end_src') then return nil end\n"
    "  end\n"
    "  if not start_row then return nil end\n"
    "  local end_row\n"
    "  for i = start_row + 1, mep.line_count() do\n"
    "    if mep.get_line(i):match('^%s*#%+end_src') then end_row = i break end\n"
    "  end\n"
    "  if not end_row or end_row < row then return nil end\n"
    "  local header = mep.get_line(start_row)\n"
    "  local lang = header:match('^%s*#%+begin_src%s+(%S+)')\n"
    "  local args_str = header:match('^%s*#%+begin_src%s+%S+%s*(.*)$') or ''\n"
    "  local vars = {}\n"
    "  for name, val in args_str:gmatch(':var%s+([%w_]+)=(%S+)') do vars[name] = val end\n"
    "  local body = {}\n"
    "  for i = start_row + 1, end_row - 1 do body[#body + 1] = mep.get_line(i) end\n"
    "  return {start_row = start_row, end_row = end_row, lang = lang, vars = vars,\n"
    "    tangle = args_str:match(':tangle%s+(%S+)'), cache = args_str:match(':cache%s+(%S+)'),\n"
    "    args_str = args_str, body = table.concat(body, '\\n')}\n"
    "end\n"
    "function mep.org_babel_insert_results(blk, out_lines)\n"
    "  local insert_row = blk.end_row\n"
    "  local existing_start, existing_end\n"
    "  if (mep.get_line(insert_row + 1) or ''):match('^%s*#%+RESULTS:%s*$') then\n"
    "    existing_start = insert_row + 1\n"
    "    local i = existing_start + 1\n"
    "    local line_i = mep.get_line(i) or ''\n"
    "    if line_i:match('^%s*#%+begin_example') then\n"
    "      while i <= mep.line_count() and not (mep.get_line(i) or ''):match('^%s*#%+end_example') do i = i + 1 end\n"
    "      existing_end = i\n"
    "    elseif line_i:match('^%s*:') then\n"
    "      while i <= mep.line_count() and (mep.get_line(i) or ''):match('^%s*:') do i = i + 1 end\n"
    "      existing_end = i - 1\n"
    "    else\n"
    "      existing_end = existing_start\n"
    "    end\n"
    "  end\n"
    "  local block\n"
    "  if #out_lines <= 1 then\n"
    "    block = {'#+RESULTS:', ': ' .. (out_lines[1] or '')}\n"
    "  else\n"
    "    block = {'#+RESULTS:', '#+begin_example'}\n"
    "    for _, l in ipairs(out_lines) do block[#block + 1] = l end\n"
    "    block[#block + 1] = '#+end_example'\n"
    "  end\n"
    "  if existing_start then\n"
    "    mep.replace_lines(existing_start, existing_end + 1, block)\n"
    "  else\n"
    "    mep.replace_lines(insert_row + 1, insert_row + 1, block)\n"
    "  end\n"
    "end\n"
    "function mep.org_babel_execute()\n"
    "  local blk = mep_org_src_block_at(mep.cursor())\n"
    "  if not blk then mep.notify('Not in a src block', 'warn') return end\n"
    "  local cmd = mep.org_babel_langs[blk.lang]\n"
    "  if not cmd then mep.notify('No babel support for: ' .. tostring(blk.lang), 'warn') return end\n"
    "  local cache_key = blk.lang .. '|' .. blk.args_str .. '|' .. blk.body\n"
    "  if blk.cache == 'yes' and mep_org_babel_cache[cache_key] then\n"
    "    mep.org_babel_insert_results(blk, mep_org_babel_cache[cache_key])\n"
    "    mep.notify('Babel: cached result')\n"
    "    return\n"
    "  end\n"
    "  local prelude = ''\n"
    "  for name, val in pairs(blk.vars) do\n"
    "    prelude = prelude .. name .. ((blk.lang == 'sh' or blk.lang == 'bash') and ('=' .. val) or (' = ' .. val)) .. '\\n'\n"
    "  end\n"
    "  local tmpfile = os.tmpname()\n"
    "  local f = io.open(tmpfile, 'w')\n"
    "  f:write(prelude .. blk.body)\n"
    "  f:close()\n"
    "  local argv = {}\n"
    "  for _, a in ipairs(cmd) do argv[#argv + 1] = a end\n"
    "  argv[#argv + 1] = tmpfile\n"
    "  local out_lines = {}\n"
    "  mep.job_start(argv, {\n"
    "    cwd = '.',\n"
    "    on_stdout = function(line) out_lines[#out_lines + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      os.remove(tmpfile)\n"
    "      mep.org_babel_insert_results(blk, out_lines)\n"
    "      if blk.cache == 'yes' then mep_org_babel_cache[cache_key] = out_lines end\n"
    "      mep.notify('Babel: executed ' .. blk.lang .. ' block (exit ' .. code .. ')')\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "mep.command('MepOrgBabelExecute', mep.org_babel_execute)\n"
    // Tangle: concatenate same-`:tangle target` blocks in document
    // order, write to the target file.
    "function mep.org_babel_tangle()\n"
    "  local targets, order = {}, {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    if mep.get_line(i):match('^%s*#%+begin_src') then\n"
    "      local blk = mep_org_src_block_at(i)\n"
    "      if blk and blk.tangle then\n"
    "        if not targets[blk.tangle] then targets[blk.tangle] = {} order[#order + 1] = blk.tangle end\n"
    "        table.insert(targets[blk.tangle], blk.body)\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  for _, target in ipairs(order) do\n"
    "    local f = io.open(target, 'w')\n"
    "    f:write(table.concat(targets[target], '\\n\\n') .. '\\n')\n"
    "    f:close()\n"
    "  end\n"
    "  mep.notify('Tangled ' .. #order .. ' file(s)')\n"
    "end\n"
    "mep.command('MepOrgBabelTangle', mep.org_babel_tangle)\n";

// Org-mode G: export (Phase 35). Flat single-pass line walk (no
// intermediate AST) driving three backends off one shared inline-mark
// tokenizer -- src blocks always render literally (never executed
// during export), matching the plan's own "ship without eval first"
// guidance.
const char *kBuiltinOrgExport =
    "mep.org_export_marks = {\n"
    "  html = {bold_open = '<b>', bold_close = '</b>', italic_open = '<i>', italic_close = '</i>',\n"
    "    code_open = '<code>', code_close = '</code>', link = function(u, d) return '<a href=\"' .. u .. '\">' .. d .. '</a>' end},\n"
    "  markdown = {bold_open = '**', bold_close = '**', italic_open = '_', italic_close = '_',\n"
    "    code_open = '`', code_close = '`', link = function(u, d) return '[' .. d .. '](' .. u .. ')' end},\n"
    "  ascii = {bold_open = '', bold_close = '', italic_open = '', italic_close = '',\n"
    "    code_open = '', code_close = '', link = function(u, d) return d .. ' <' .. u .. '>' end},\n"
    "}\n"
    "local function mep_org_html_escape(s) return (s:gsub('&', '&amp;'):gsub('<', '&lt;'):gsub('>', '&gt;')) end\n"
    // Every construct's generated markup is stashed behind a `\0M<n>\0`
    // placeholder as soon as it's produced, then all placeholders are
    // restored in one final pass -- otherwise e.g. HTML's `</b>` (which
    // contains a `/`) would be visible to the *next* pattern (italic's
    // bare `/.../`) and get wrongly re-matched into it, corrupting the
    // generated markup. The trailing `\0` in each placeholder makes
    // `\0M1\0` and `\0M12\0` mutually non-overlapping substrings, so
    // restoring in any order is safe.
    "local function mep_org_inline_convert(text, marks)\n"
    "  local stash = {}\n"
    "  local function stash_out(html)\n"
    "    stash[#stash + 1] = html\n"
    "    return '\\0M' .. #stash .. '\\0'\n"
    "  end\n"
    "  text = text:gsub('%[%[([^%]]+)%]%[([^%]]+)%]%]', function(u, d) return stash_out(marks.link(u, d)) end)\n"
    "  text = text:gsub('%[%[([^%]]+)%]%]', function(u) return stash_out(marks.link(u, u)) end)\n"
    "  text = text:gsub('%*([^%*\\n]+)%*', function(t) return stash_out(marks.bold_open .. t .. marks.bold_close) end)\n"
    "  text = text:gsub('/([^/\\n]+)/', function(t) return stash_out(marks.italic_open .. t .. marks.italic_close) end)\n"
    "  text = text:gsub('=([^=\\n]+)=', function(t) return stash_out(marks.code_open .. t .. marks.code_close) end)\n"
    "  for idx, html in ipairs(stash) do\n"
    "    text = text:gsub('\\0M' .. idx .. '\\0', function() return html end)\n"
    "  end\n"
    "  return text\n"
    "end\n"
    "local function mep_org_export_heading(format, level, title)\n"
    "  if format == 'html' then return '<h' .. level .. '>' .. title .. '</h' .. level .. '>'\n"
    "  elseif format == 'markdown' then return string.rep('#', level) .. ' ' .. title\n"
    "  else return string.rep('  ', level - 1) .. title:upper() end\n"
    "end\n"
    "function mep.org_export(format)\n"
    "  local marks = mep.org_export_marks[format]\n"
    "  local out, i, n = {}, 1, mep.line_count()\n"
    "  while i <= n do\n"
    "    local line = mep.get_line(i)\n"
    "    local h = mep_org_parse_headline(line)\n"
    "    if h then\n"
    "      if h.tags and h.tags:find('noexport', 1, true) then\n"
    "        i = mep_org_subtree_end(i)\n"
    "      else\n"
    "        local title = mep_org_inline_convert(format == 'html' and mep_org_html_escape(h.title) or h.title, marks)\n"
    "        out[#out + 1] = mep_org_export_heading(format, h.level, title)\n"
    "        i = i + 1\n"
    "      end\n"
    "    elseif line:match('^%s*#%+begin_src') then\n"
    "      local lang = line:match('^%s*#%+begin_src%s+(%S+)') or ''\n"
    "      out[#out + 1] = (format == 'html') and ('<pre><code class=\"language-' .. lang .. '\">')\n"
    "        or (format == 'markdown') and ('```' .. lang) or '----'\n"
    "      i = i + 1\n"
    "      while i <= n and not mep.get_line(i):match('^%s*#%+end_src') do\n"
    "        out[#out + 1] = (format == 'html') and mep_org_html_escape(mep.get_line(i)) or mep.get_line(i)\n"
    "        i = i + 1\n"
    "      end\n"
    "      out[#out + 1] = (format == 'html') and '</code></pre>' or (format == 'markdown') and '```' or '----'\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*:PROPERTIES:%s*$') then\n"
    "      while i <= n and not mep.get_line(i):match('^%s*:END:%s*$') do i = i + 1 end\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*SCHEDULED:') or line:match('^%s*DEADLINE:') or line:match('^%s*#%+') then\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*$') then\n"
    "      out[#out + 1] = ''\n"
    "      i = i + 1\n"
    "    else\n"
    "      local converted = mep_org_inline_convert(format == 'html' and mep_org_html_escape(line) or line, marks)\n"
    "      if format == 'html' and line:match('^%s*[%-%*%+]%s') then\n"
    "        out[#out + 1] = '<li>' .. (converted:gsub('^%s*[%-%*%+]%s*', '')) .. '</li>'\n"
    "      else\n"
    "        out[#out + 1] = converted\n"
    "      end\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  return table.concat(out, '\\n')\n"
    "end\n"
    // Subtree export: same walk, scoped to [row, subtree_end), with
    // headline levels renormalized so the subtree's own headline becomes
    // level 1.
    "function mep.org_export_subtree(format)\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return nil end\n"
    "  local base_level, marks, out = mep.org_headline_level(row), mep.org_export_marks[format], {}\n"
    "  for i = row, mep_org_subtree_end(row) - 1 do\n"
    "    local line = mep.get_line(i)\n"
    "    local h = mep_org_parse_headline(line)\n"
    "    if h then\n"
    "      local title = mep_org_inline_convert(format == 'html' and mep_org_html_escape(h.title) or h.title, marks)\n"
    "      out[#out + 1] = mep_org_export_heading(format, h.level - base_level + 1, title)\n"
    "    else\n"
    "      out[#out + 1] = mep_org_inline_convert(format == 'html' and mep_org_html_escape(line) or line, marks)\n"
    "    end\n"
    "  end\n"
    "  return table.concat(out, '\\n')\n"
    "end\n"
    "local function mep_org_export_to_file(text, ext)\n"
    "  local out_file = (mep.filename():gsub('%.org$', '')) .. '.' .. ext\n"
    "  local f = io.open(out_file, 'w')\n"
    "  f:write(text)\n"
    "  f:close()\n"
    "  mep.notify('Exported to ' .. out_file)\n"
    "end\n"
    "mep.command('MepOrgExportHtml', function() mep_org_export_to_file(mep.org_export('html'), 'html') end)\n"
    "mep.command('MepOrgExportMarkdown', function() mep_org_export_to_file(mep.org_export('markdown'), 'md') end)\n"
    "mep.command('MepOrgExportAscii', function() mep_org_export_to_file(mep.org_export('ascii'), 'txt') end)\n"
    "mep.command('MepOrgExportSubtreeHtml', function() mep_org_export_to_file(mep.org_export_subtree('html'), 'html') end)\n"
    "mep.command('MepOrgExportSubtreeMarkdown', function() mep_org_export_to_file(mep.org_export_subtree('markdown'), 'md') end)\n";

// Part VIII, Phase 37 -- Roam (zettelkasten note linking). One-note-per-
// file, file-level :ID: property drawer at the very top (before any
// headline) per org-roam v2 convention, `[[id:...][title]]` links reuse
// Phase 30's link machinery (org_link_follow's `id:` branch) unchanged.
const char *kBuiltinOrgRoam =
    "mep.org_roam_dirs = {}\n"
    "mep.org_roam_daily_dir = nil\n"
    "local mep_org_roam_sidebar_id = nil\n"
    "local function mep_org_roam_gen_id() return os.date('%Y%m%d%H%M%S') .. '-' .. tostring(math.random(1000, 9999)) end\n"
    "local function mep_org_roam_files()\n"
    "  local files = {}\n"
    "  for _, dir in ipairs(mep.org_roam_dirs) do\n"
    "    for _, e in ipairs(mep.list_dir(dir)) do\n"
    "      if e.name:match('%.org$') then files[#files + 1] = dir .. '/' .. e.name end\n"
    "    end\n"
    "  end\n"
    "  return files\n"
    "end\n"
    "local function mep_org_roam_title_of(lines)\n"
    "  for _, line in ipairs(lines) do\n"
    "    local t = line:match('^#%+TITLE:%s*(.*)$')\n"
    "    if t then return t end\n"
    "  end\n"
    "  for _, line in ipairs(lines) do\n"
    "    local h = mep_org_parse_headline(line)\n"
    "    if h then return h.title end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function mep.org_roam_ensure_id()\n"
    "  for i = 1, mep.line_count() do\n"
    "    if mep.org_is_headline(i) then break end\n"
    "    local id = mep.get_line(i):match('^%s*:ID:%s*(%S+)')\n"
    "    if id then return id end\n"
    "  end\n"
    "  local id = mep_org_roam_gen_id()\n"
    "  mep.replace_lines(1, 1, {':PROPERTIES:', ':ID:       ' .. id, ':END:'})\n"
    "  return id\n"
    "end\n"
    "mep.command('MepOrgRoamEnsureId', mep.org_roam_ensure_id)\n"
    "function mep.org_roam_insert_link()\n"
    "  local items = {}\n"
    "  for _, path in ipairs(mep_org_roam_files()) do\n"
    "    if path ~= mep.filename() then\n"
    "      local lines = mep_org_read_file_lines(path)\n"
    "      local title = lines and mep_org_roam_title_of(lines)\n"
    "      if title then items[#items + 1] = {display = title, data = path} end\n"
    "    end\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No roam notes found', 'warn') return end\n"
    "  mep.picker_open('Roam: Insert Link', items, function(path)\n"
    "    if not path then return end\n"
    "    local cur = mep.filename()\n"
    "    mep.pane_open(path)\n"
    "    local id = mep.org_roam_ensure_id()\n"
    "    local title = mep_org_roam_title_of(mep_org_read_file_lines(path)) or path\n"
    "    mep.cmd('w')\n"
    "    mep.pane_open(cur)\n"
    "    mep.insert_text('[[id:' .. id .. '][' .. title .. ']]')\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgRoamInsertLink', mep.org_roam_insert_link)\n"
    // Backlinks sidebar: every note whose text contains a `[[id:<my-id>`
    // link, computed by scanning every configured roam file.
    "function mep.org_roam_backlinks()\n"
    "  local my_id\n"
    "  for i = 1, mep.line_count() do\n"
    "    if mep.org_is_headline(i) then break end\n"
    "    my_id = mep.get_line(i):match('^%s*:ID:%s*(%S+)')\n"
    "    if my_id then break end\n"
    "  end\n"
    "  if not my_id then mep.notify('This note has no :ID: yet', 'warn') return end\n"
    "  local widgets = {}\n"
    "  for _, path in ipairs(mep_org_roam_files()) do\n"
    "    if path ~= mep.filename() then\n"
    "      local lines = mep_org_read_file_lines(path)\n"
    "      if lines then\n"
    "        for i, line in ipairs(lines) do\n"
    "          if line:find('[[id:' .. my_id, 1, true) then\n"
    "            local title = mep_org_roam_title_of(lines) or path\n"
    "            widgets[#widgets + 1] = {id = path .. ':' .. i, text = title,\n"
    "              on_click = function() mep.pane_open(path) mep.set_cursor(i, 1) end}\n"
    "          end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if not mep_org_roam_sidebar_id then mep_org_roam_sidebar_id = mep.sidebar_create('Backlinks', 'right', 34) end\n"
    "  mep.sidebar_set_sections(mep_org_roam_sidebar_id,\n"
    "    {{id = 'backlinks', title = #widgets .. ' linking here', collapsed = false, widgets = widgets}})\n"
    "  mep.sidebar_open(mep_org_roam_sidebar_id)\n"
    "end\n"
    "mep.command('MepOrgRoamBacklinks', mep.org_roam_backlinks)\n"
    "function mep.org_roam_daily()\n"
    "  local dir = mep.org_roam_daily_dir or mep.org_roam_dirs[1]\n"
    "  if not dir then mep.notify('No roam directory configured', 'warn') return end\n"
    "  mep.pane_open(dir .. '/' .. os.date('%Y-%m-%d') .. '.org')\n"
    "  if mep.line_count() == 1 and mep.get_line(1) == '' then\n"
    "    mep.replace_lines(1, 1, {'#+TITLE: ' .. os.date('%Y-%m-%d')})\n"
    "    mep.org_roam_ensure_id()\n"
    "  end\n"
    "end\n"
    "mep.command('MepOrgRoamDaily', mep.org_roam_daily)\n"
    "function mep.org_roam_new_note()\n"
    "  mep.ui_input('Note title:', '', function(title)\n"
    "    if not title or title == '' then return end\n"
    "    local dir = mep.org_roam_dirs[1]\n"
    "    if not dir then mep.notify('No roam directory configured', 'warn') return end\n"
    "    local slug = title:lower():gsub('[^%w]+', '-'):gsub('^%-+', ''):gsub('%-+$', '')\n"
    "    mep.pane_open(dir .. '/' .. slug .. '.org')\n"
    "    mep.replace_lines(1, 1, {'#+TITLE: ' .. title})\n"
    "    mep.org_roam_ensure_id()\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgRoamNewNote', mep.org_roam_new_note)\n";

// Phase 38 -- Flashcards (SM-2 spaced repetition). State lives as org
// properties in each headline's drawer (Phase 29's property machinery),
// so cards are ordinary org headlines tagged `:drill:` -- no separate
// storage format.
const char *kBuiltinOrgDrill =
    "mep.org_drill_tag = 'drill'\n"
    "mep.org_drill_files = {}\n"
    "local function mep_org_sm2(ef, reps, interval, quality)\n"
    "  if quality < 3 then\n"
    "    reps, interval = 0, 1\n"
    "  else\n"
    "    reps = reps + 1\n"
    "    if reps == 1 then interval = 1\n"
    "    elseif reps == 2 then interval = 6\n"
    "    else interval = math.floor(interval * ef + 0.5) end\n"
    "  end\n"
    "  ef = ef + (0.1 - (5 - quality) * (0.08 + (5 - quality) * 0.02))\n"
    "  if ef < 1.3 then ef = 1.3 end\n"
    "  return ef, reps, interval\n"
    "end\n"
    "function mep.org_drill_grade(row, quality)\n"
    "  local ef = tonumber(mep.org_property_get(row, 'DRILL_EF')) or 2.5\n"
    "  local reps = tonumber(mep.org_property_get(row, 'DRILL_REPS')) or 0\n"
    "  local interval = tonumber(mep.org_property_get(row, 'DRILL_INTERVAL')) or 0\n"
    "  ef, reps, interval = mep_org_sm2(ef, reps, interval, quality)\n"
    "  mep.org_property_set(row, 'DRILL_EF', string.format('%.2f', ef))\n"
    "  mep.org_property_set(row, 'DRILL_REPS', tostring(reps))\n"
    "  mep.org_property_set(row, 'DRILL_INTERVAL', tostring(interval))\n"
    "  mep.org_property_set(row, 'DRILL_DUE', os.date('%Y-%m-%d', os.time() + interval * 86400))\n"
    "end\n"
    "local function mep_org_tags_at_lines(lines, row)\n"
    "  local h = mep_org_parse_headline(lines[row])\n"
    "  if not h then return {} end\n"
    "  local tags, level = {}, h.level\n"
    "  if h.tags then for t in h.tags:gmatch('[^:]+') do tags[t] = true end end\n"
    "  for i = row - 1, 1, -1 do\n"
    "    local hl = mep_org_parse_headline(lines[i])\n"
    "    if hl and hl.level < level then\n"
    "      if hl.tags then for t in hl.tags:gmatch('[^:]+') do tags[t] = true end end\n"
    "      level = hl.level\n"
    "    end\n"
    "  end\n"
    "  return tags\n"
    "end\n"
    "function mep.org_drill_collect_due()\n"
    "  local today, cards = os.date('%Y-%m-%d'), {}\n"
    "  for _, path in ipairs(mep.org_drill_files) do\n"
    "    local lines = mep_org_read_file_lines(path)\n"
    "    if lines then\n"
    "      for i, line in ipairs(lines) do\n"
    "        local h = mep_org_parse_headline(line)\n"
    "        if h and mep_org_tags_at_lines(lines, i)[mep.org_drill_tag] then\n"
    "          local due\n"
    "          for j = i + 1, #lines do\n"
    "            if mep_org_parse_headline(lines[j]) or lines[j]:match('^%s*:END:%s*$') then break end\n"
    "            due = lines[j]:match(':DRILL_DUE:%s*(%S+)')\n"
    "            if due then break end\n"
    "          end\n"
    "          if not due or due <= today then cards[#cards + 1] = {file = path, line = i, title = h.title} end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return cards\n"
    "end\n"
    "function mep.org_drill_review()\n"
    "  local cards = mep.org_drill_collect_due()\n"
    "  if #cards == 0 then mep.notify('No cards due') return end\n"
    "  local grades, quality_of = {'Again', 'Hard', 'Good', 'Easy'}, {[1] = 0, [2] = 3, [3] = 4, [4] = 5}\n"
    "  local function next_card(card_idx)\n"
    "    if card_idx > #cards then mep.notify('Review complete: ' .. #cards .. ' card(s)') return end\n"
    "    local c = cards[card_idx]\n"
    "    mep.pane_open(c.file)\n"
    "    mep.set_cursor(c.line, 1)\n"
    "    mep.ui_confirm('Reveal answer for: ' .. c.title .. '?', true, function(revealed)\n"
    "      mep.ui_select(grades, 'Grade', function(sel)\n"
    "        if sel then mep.org_drill_grade(c.line, quality_of[sel]) end\n"
    "        next_card(card_idx + 1)\n"
    "      end)\n"
    "    end)\n"
    "  end\n"
    "  next_card(1)\n"
    "end\n"
    "mep.command('MepOrgDrillReview', mep.org_drill_review)\n";

// Phase 39 -- Bib (bibliography / org-ref). A hand-rolled BibTeX parser
// using Lua's `%b{}` balanced-match pattern item for brace-nested field
// values (titles like `{Foo {Bar} Baz}`), which a naive non-greedy regex
// can't handle correctly.
const char *kBuiltinOrgBib =
    "mep.org_bib_files = {}\n"
    "local function mep_org_bib_split_top_level(s, sep)\n"
    "  local parts, depth, start = {}, 0, 1\n"
    "  for i = 1, #s do\n"
    "    local c = s:sub(i, i)\n"
    "    if c == '{' then depth = depth + 1\n"
    "    elseif c == '}' then depth = depth - 1\n"
    "    elseif c == sep and depth == 0 then\n"
    "      parts[#parts + 1] = s:sub(start, i - 1)\n"
    "      start = i + 1\n"
    "    end\n"
    "  end\n"
    "  parts[#parts + 1] = s:sub(start)\n"
    "  return parts\n"
    "end\n"
    "local function mep_org_bib_parse(text)\n"
    "  local entries = {}\n"
    "  for etype, braced in text:gmatch('@(%a+)%s*(%b{})') do\n"
    "    local body = braced:sub(2, -2)\n"
    "    local key, fieldstr = body:match('^%s*([^,]+),(.*)$')\n"
    "    if key then\n"
    "      local fields = {}\n"
    "      for _, part in ipairs(mep_org_bib_split_top_level(fieldstr, ',')) do\n"
    "        local name, val = part:match('^%s*([%w_%-]+)%s*=%s*(.*)$')\n"
    "        if name then\n"
    "          val = val:match('^%s*(.-)%s*$')\n"
    "          if val:sub(1, 1) == '{' and val:sub(-1) == '}' then val = val:sub(2, -2)\n"
    "          elseif val:sub(1, 1) == '\"' and val:sub(-1) == '\"' then val = val:sub(2, -2) end\n"
    "          fields[name:lower()] = val\n"
    "        end\n"
    "      end\n"
    "      entries[#entries + 1] = {type = etype:lower(), key = key:match('^%s*(.-)%s*$'), fields = fields}\n"
    "    end\n"
    "  end\n"
    "  return entries\n"
    "end\n"
    // File resolution: current buffer's directory, then each configured
    // project root, matching mep.nvim's documented search order.
    "function mep.org_bib_resolve_files()\n"
    "  local files = {}\n"
    "  for _, f in ipairs(mep.org_bib_files) do\n"
    "    if f:sub(1, 1) == '/' then\n"
    "      files[#files + 1] = f\n"
    "    else\n"
    "      local dir = mep.filename():match('^(.*)/[^/]+$') or '.'\n"
    "      local candidate = dir .. '/' .. f\n"
    "      local test = io.open(candidate, 'r')\n"
    "      if test then\n"
    "        test:close()\n"
    "        files[#files + 1] = candidate\n"
    "      else\n"
    "        for _, proj in ipairs(mep.project_list()) do\n"
    "          local c2 = proj .. '/' .. f\n"
    "          local t2 = io.open(c2, 'r')\n"
    "          if t2 then t2:close() files[#files + 1] = c2 break end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return files\n"
    "end\n"
    "function mep.org_bib_insert_citation()\n"
    "  local entries = {}\n"
    "  for _, path in ipairs(mep.org_bib_resolve_files()) do\n"
    "    local f = io.open(path, 'r')\n"
    "    if f then\n"
    "      local text = f:read('*a')\n"
    "      f:close()\n"
    "      for _, e in ipairs(mep_org_bib_parse(text)) do entries[#entries + 1] = e end\n"
    "    end\n"
    "  end\n"
    "  if #entries == 0 then mep.notify('No bibliography entries found', 'warn') return end\n"
    "  local items = {}\n"
    "  for _, e in ipairs(entries) do\n"
    "    local label = e.key .. '  ' .. (e.fields.title or '') .. (e.fields.author and (' -- ' .. e.fields.author) or '')\n"
    "    items[#items + 1] = {display = label, data = e.key}\n"
    "  end\n"
    "  mep.picker_open('Insert Citation', items, function(key)\n"
    "    if key then mep.insert_text('[cite:@' .. key .. ']') end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgBibInsertCitation', mep.org_bib_insert_citation)\n";

// Part IX, Phase 40 -- Activity bar (notifications/todo/tests/git).
// Notifications (:MepNotifyPanel) and Git (:MepGitStatus) panels already
// exist natively/from Phase 17 -- this phase adds the two new panels
// (Todo, Tests) plus one aggregating entry point. The plan's "slim
// icon-only button column" chrome affordance is scoped down to a picker
// over the four panels (see Phase 40's plan notes) rather than a new
// persistent always-visible C++ widget, given the four *panels*
// themselves are the functional core.
const char *kBuiltinActivityBar =
    "mep.activity_todo_file = nil\n"
    "local mep_activity_todo_sidebar_id = nil\n"
    "local function mep_activity_todo_path() return mep.activity_todo_file or (mep.getcwd() .. '/.mep_todos.txt') end\n"
    "local function mep_activity_todo_load()\n"
    "  local items = {}\n"
    "  local f = io.open(mep_activity_todo_path(), 'r')\n"
    "  if f then\n"
    "    for line in f:lines() do\n"
    "      local done, text = line:match('^(%d)|(.*)$')\n"
    "      if done then items[#items + 1] = {done = done == '1', text = text} end\n"
    "    end\n"
    "    f:close()\n"
    "  end\n"
    "  return items\n"
    "end\n"
    "local function mep_activity_todo_save(items)\n"
    "  local f = io.open(mep_activity_todo_path(), 'w')\n"
    "  for _, it in ipairs(items) do f:write((it.done and '1' or '0') .. '|' .. it.text .. '\\n') end\n"
    "  f:close()\n"
    "end\n"
    "function mep.activity_todo_panel()\n"
    "  local items = mep_activity_todo_load()\n"
    "  local widgets = {}\n"
    "  for i, it in ipairs(items) do\n"
    "    widgets[#widgets + 1] = {id = tostring(i), text = (it.done and '[x] ' or '[ ] ') .. it.text,\n"
    "      on_click = function()\n"
    "        local cur = mep_activity_todo_load()\n"
    "        if cur[i] then cur[i].done = not cur[i].done end\n"
    "        mep_activity_todo_save(cur)\n"
    "        mep.activity_todo_panel()\n"
    "      end}\n"
    "  end\n"
    "  if not mep_activity_todo_sidebar_id then mep_activity_todo_sidebar_id = mep.sidebar_create('Todo', 'right', 40) end\n"
    "  mep.sidebar_set_sections(mep_activity_todo_sidebar_id, {{id = 'todos', title = '', collapsed = false, widgets = widgets}})\n"
    "  mep.sidebar_open(mep_activity_todo_sidebar_id)\n"
    "end\n"
    "function mep.activity_todo_add()\n"
    "  mep.ui_input('New todo:', '', function(text)\n"
    "    if not text or text == '' then return end\n"
    "    local items = mep_activity_todo_load()\n"
    "    items[#items + 1] = {done = false, text = text}\n"
    "    mep_activity_todo_save(items)\n"
    "    mep.activity_todo_panel()\n"
    "  end)\n"
    "end\n"
    "function mep.activity_todo_clear_done()\n"
    "  local kept = {}\n"
    "  for _, it in ipairs(mep_activity_todo_load()) do if not it.done then kept[#kept + 1] = it end end\n"
    "  mep_activity_todo_save(kept)\n"
    "  mep.activity_todo_panel()\n"
    "end\n"
    "mep.command('MepActivityTodoPanel', mep.activity_todo_panel)\n"
    "mep.command('MepActivityTodoAdd', mep.activity_todo_add)\n"
    "mep.command('MepActivityTodoClearDone', mep.activity_todo_clear_done)\n"
    // Tests panel: a configured command, else auto-detected from
    // project marker files (CMakeLists.txt -> ctest, package.json ->
    // npm test). Failure lines get a click handler; since sidebar
    // widgets have no multi-line detail view, "full output" is a toast
    // of that one line -- see the phase's scope-cut note.
    "mep.activity_test_cmd = nil\n"
    "local mep_activity_test_sidebar_id = nil\n"
    "local mep_activity_test_results = {}\n"
    "local function mep_activity_test_detect_cmd()\n"
    "  if mep.activity_test_cmd then return mep.activity_test_cmd end\n"
    "  local f = io.open('CMakeLists.txt', 'r')\n"
    "  if f then f:close() return {'ctest', '--test-dir', 'build'} end\n"
    "  local f2 = io.open('package.json', 'r')\n"
    "  if f2 then f2:close() return {'npm', 'test'} end\n"
    "  return nil\n"
    "end\n"
    "function mep.activity_test_panel()\n"
    "  local widgets, r = {}, mep_activity_test_results\n"
    "  if r.code ~= nil then\n"
    "    widgets[#widgets + 1] = {id = 'summary', text = (r.code == 0 and 'PASSED' or 'FAILED') .. ' (exit ' .. r.code .. ')'}\n"
    "    for i, line in ipairs(r.output or {}) do\n"
    "      if line:lower():find('fail', 1, true) then\n"
    "        widgets[#widgets + 1] = {id = 'f' .. i, text = '  ' .. line, hl = 'Error',\n"
    "          on_click = function() mep.notify(line) end}\n"
    "      end\n"
    "    end\n"
    "  else\n"
    "    widgets[#widgets + 1] = {id = 'none', text = '(no results yet -- run tests)'}\n"
    "  end\n"
    "  if not mep_activity_test_sidebar_id then mep_activity_test_sidebar_id = mep.sidebar_create('Tests', 'right', 44) end\n"
    "  mep.sidebar_set_sections(mep_activity_test_sidebar_id, {{id = 'tests', title = '', collapsed = false, widgets = widgets}})\n"
    "  mep.sidebar_open(mep_activity_test_sidebar_id)\n"
    "end\n"
    "function mep.activity_test_run()\n"
    "  local cmd = mep_activity_test_detect_cmd()\n"
    "  if not cmd then mep.notify('No test command configured or detected', 'warn') return end\n"
    "  local lines = {}\n"
    "  mep.job_start(cmd, {\n"
    "    on_stdout = function(line) lines[#lines + 1] = line end,\n"
    "    on_stderr = function(line) lines[#lines + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      mep_activity_test_results = {code = code, output = lines}\n"
    "      mep.activity_test_panel()\n"
    "    end,\n"
    "  })\n"
    "  mep.notify('Running tests...')\n"
    "end\n"
    "mep.command('MepActivityTestRun', mep.activity_test_run)\n"
    "mep.command('MepActivityTestPanel', mep.activity_test_panel)\n"
    // Aggregating entry point: a picker over the four panels rather than
    // a persistent icon column (see the phase's own scope-cut note).
    "function mep.activity_bar_open()\n"
    "  mep.picker_open('Activity', {'Notifications', 'Todo', 'Tests', 'Git'}, function(choice)\n"
    "    if choice == 'Notifications' then mep.cmd('MepNotifyPanel')\n"
    "    elseif choice == 'Todo' then mep.activity_todo_panel()\n"
    "    elseif choice == 'Tests' then mep.activity_test_panel()\n"
    "    elseif choice == 'Git' then mep.cmd('MepGitStatus') end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepActivityBar', mep.activity_bar_open)\n";

// Part X, Phase 41 -- AI integration (LLM streaming + tool-calling
// agent). HTTP via a `curl` subprocess (Phase 1 jobs) rather than a real
// HTTP client -- matches mep.nvim's own approach, and SSE `data: {...}`
// frames are newline-delimited so the ordinary line-split on_stdout
// (not Phase 20's raw byte mode) is sufficient here. No JSON
// encode/decode is exposed to Lua anywhere else in the codebase, so
// this phase carries its own minimal hand-rolled encoder/parser.
const char *kBuiltinAi =
    "mep.ai_provider = 'openai'\n"
    "mep.ai_model = 'gpt-4o-mini'\n"
    "mep.ai_base_url = 'https://api.openai.com/v1'\n"
    "mep.ai_anthropic_base_url = 'https://api.anthropic.com'\n"
    "mep.ai_anthropic_model = 'claude-3-5-sonnet-latest'\n"
    "mep.ai_api_key = nil\n"
    "mep.ai_max_tokens = 2048\n"
    "local mep_ai_active_job = nil\n"
    "function mep_ai_json_encode(v)\n"
    "  local t = type(v)\n"
    "  if t == 'string' then\n"
    "    local esc = v:gsub('[\\\\\"\\n\\r\\t]', {['\\\\'] = '\\\\\\\\', ['\"'] = '\\\\\"', ['\\n'] = '\\\\n', ['\\r'] = '\\\\r', ['\\t'] = '\\\\t'})\n"
    "    return '\"' .. esc .. '\"'\n"
    "  elseif t == 'number' then return tostring(v)\n"
    "  elseif t == 'boolean' then return v and 'true' or 'false'\n"
    "  elseif t == 'table' then\n"
    "    if #v > 0 then\n"
    "      local parts = {}\n"
    "      for _, item in ipairs(v) do parts[#parts + 1] = mep_ai_json_encode(item) end\n"
    "      return '[' .. table.concat(parts, ',') .. ']'\n"
    "    else\n"
    "      local parts = {}\n"
    "      for k, val in pairs(v) do parts[#parts + 1] = mep_ai_json_encode(tostring(k)) .. ':' .. mep_ai_json_encode(val) end\n"
    "      if #parts == 0 then return '{}' end\n"
    "      return '{' .. table.concat(parts, ',') .. '}'\n"
    "    end\n"
    "  end\n"
    "  return 'null'\n"
    "end\n"
    "function mep_ai_json_decode(s)\n"
    "  local pos = 1\n"
    "  local function skip_ws() while pos <= #s and s:sub(pos, pos):match('%s') do pos = pos + 1 end end\n"
    "  local parse_value\n"
    "  local function parse_string()\n"
    "    pos = pos + 1\n"
    "    local buf = {}\n"
    "    while true do\n"
    "      local c = s:sub(pos, pos)\n"
    "      if c == '\"' then pos = pos + 1 break end\n"
    "      if c == '\\\\' then\n"
    "        local n = s:sub(pos + 1, pos + 1)\n"
    "        local map = {n = '\\n', t = '\\t', r = '\\r', ['\"'] = '\"', ['\\\\'] = '\\\\', ['/'] = '/'}\n"
    "        if n == 'u' then\n"
    "          pos = pos + 6\n"
    "          buf[#buf + 1] = '?'\n"
    "        else\n"
    "          buf[#buf + 1] = map[n] or n\n"
    "          pos = pos + 2\n"
    "        end\n"
    "      else\n"
    "        buf[#buf + 1] = c\n"
    "        pos = pos + 1\n"
    "      end\n"
    "    end\n"
    "    return table.concat(buf)\n"
    "  end\n"
    "  local function parse_number()\n"
    "    local start = pos\n"
    "    while pos <= #s and s:sub(pos, pos):match('[%d%.%-%+eE]') do pos = pos + 1 end\n"
    "    return tonumber(s:sub(start, pos - 1))\n"
    "  end\n"
    "  parse_value = function()\n"
    "    skip_ws()\n"
    "    local c = s:sub(pos, pos)\n"
    "    if c == '\"' then return parse_string()\n"
    "    elseif c == '{' then\n"
    "      pos = pos + 1\n"
    "      local obj = {}\n"
    "      skip_ws()\n"
    "      if s:sub(pos, pos) == '}' then pos = pos + 1 return obj end\n"
    "      while true do\n"
    "        skip_ws()\n"
    "        local key = parse_string()\n"
    "        skip_ws()\n"
    "        pos = pos + 1\n"
    "        obj[key] = parse_value()\n"
    "        skip_ws()\n"
    "        if s:sub(pos, pos) == ',' then pos = pos + 1 else pos = pos + 1 break end\n"
    "      end\n"
    "      return obj\n"
    "    elseif c == '[' then\n"
    "      pos = pos + 1\n"
    "      local arr = {}\n"
    "      skip_ws()\n"
    "      if s:sub(pos, pos) == ']' then pos = pos + 1 return arr end\n"
    "      while true do\n"
    "        arr[#arr + 1] = parse_value()\n"
    "        skip_ws()\n"
    "        if s:sub(pos, pos) == ',' then pos = pos + 1 else pos = pos + 1 break end\n"
    "      end\n"
    "      return arr\n"
    "    elseif c == 't' then pos = pos + 4 return true\n"
    "    elseif c == 'f' then pos = pos + 5 return false\n"
    "    elseif c == 'n' then pos = pos + 4 return nil\n"
    "    else return parse_number() end\n"
    "  end\n"
    "  local ok, result = pcall(parse_value)\n"
    "  if ok then return result end\n"
    "  return nil\n"
    "end\n"
    // API key: explicit override, else env var, else a prompt (kept only
    // in the Lua variable for the session -- never written to disk).
    // mep.ui_input has no masked-echo mode, so this prompt is visible;
    // documented as a scope cut.
    "local function mep_ai_get_key(cb)\n"
    "  if mep.ai_api_key then cb(mep.ai_api_key) return end\n"
    "  local env_var = mep.ai_provider == 'anthropic' and 'ANTHROPIC_API_KEY' or 'OPENAI_API_KEY'\n"
    "  local v = os.getenv(env_var)\n"
    "  if v and v ~= '' then mep.ai_api_key = v cb(v) return end\n"
    "  mep.ui_input('API key (' .. env_var .. ' not set):', '', function(key)\n"
    "    if key and key ~= '' then mep.ai_api_key = key cb(key) end\n"
    "  end)\n"
    "end\n"
    // Streams a chat turn. on_delta(text) fires per streamed chunk;
    // on_done(tool_calls) fires once, with an array of {id,name,args}
    // (empty if the model didn't call a tool).
    "local function mep_ai_request(messages, tools, on_delta, on_done)\n"
    "  mep_ai_get_key(function(key)\n"
    "    local url, headers, body\n"
    "    if mep.ai_provider == 'anthropic' then\n"
    "      url = mep.ai_anthropic_base_url .. '/v1/messages'\n"
    "      headers = {'-H', 'x-api-key: ' .. key, '-H', 'anthropic-version: 2023-06-01', '-H', 'Content-Type: application/json'}\n"
    "      body = {model = mep.ai_anthropic_model, max_tokens = mep.ai_max_tokens, stream = true, messages = messages}\n"
    "    else\n"
    "      url = mep.ai_base_url .. '/chat/completions'\n"
    "      headers = {'-H', 'Authorization: Bearer ' .. key, '-H', 'Content-Type: application/json'}\n"
    "      body = {model = mep.ai_model, stream = true, messages = messages}\n"
    "      if tools then body.tools = tools end\n"
    "    end\n"
    "    local tmpfile = os.tmpname()\n"
    "    local f = io.open(tmpfile, 'w')\n"
    "    f:write(mep_ai_json_encode(body))\n"
    "    f:close()\n"
    "    local argv = {'curl', '-sN', '-X', 'POST', url}\n"
    "    for _, h in ipairs(headers) do argv[#argv + 1] = h end\n"
    "    argv[#argv + 1] = '--data-binary'\n"
    "    argv[#argv + 1] = '@' .. tmpfile\n"
    "    local tool_calls = {}\n"
    "    mep_ai_active_job = mep.job_start(argv, {\n"
    "      on_stdout = function(line)\n"
    "        if line:sub(1, 6) ~= 'data: ' then return end\n"
    "        local payload = line:sub(7)\n"
    "        if payload == '[DONE]' then return end\n"
    "        local obj = mep_ai_json_decode(payload)\n"
    "        if not obj then return end\n"
    "        if mep.ai_provider == 'anthropic' then\n"
    "          if obj.delta and obj.delta.text then on_delta(obj.delta.text) end\n"
    "        else\n"
    "          local choice = obj.choices and obj.choices[1]\n"
    "          local delta = choice and choice.delta\n"
    "          if delta and delta.content then on_delta(delta.content) end\n"
    "          if delta and delta.tool_calls then\n"
    "            for _, tc in ipairs(delta.tool_calls) do\n"
    "              local idx = (tc.index or 0) + 1\n"
    "              tool_calls[idx] = tool_calls[idx] or {id = tc.id, name = '', args = ''}\n"
    "              if tc.id then tool_calls[idx].id = tc.id end\n"
    "              if tc['function'] then\n"
    "                if tc['function'].name then tool_calls[idx].name = tool_calls[idx].name .. tc['function'].name end\n"
    "                if tc['function'].arguments then tool_calls[idx].args = tool_calls[idx].args .. tc['function'].arguments end\n"
    "              end\n"
    "            end\n"
    "          end\n"
    "        end\n"
    "      end,\n"
    "      on_exit = function(code)\n"
    "        os.remove(tmpfile)\n"
    "        mep_ai_active_job = nil\n"
    "        local calls = {}\n"
    "        for _, tc in pairs(tool_calls) do calls[#calls + 1] = tc end\n"
    "        on_done(calls)\n"
    "      end,\n"
    "    })\n"
    "  end)\n"
    "end\n"
    "function mep.ai_cancel()\n"
    "  if mep_ai_active_job then mep.job_kill(mep_ai_active_job) mep_ai_active_job = nil mep.notify('AI stream cancelled') end\n"
    "end\n"
    "mep.command('MepAiCancel', mep.ai_cancel)\n"
    // Send-buffer / send-range: streams the response in at the cursor,
    // one mep.insert_text(delta) call per chunk -- an ever-advancing
    // cursor position is inherently gravity-tracked without needing a
    // dedicated decoration-based tracker.
    "function mep.ai_send_text(prompt)\n"
    "  local n = mep.line_count()\n"
    "  mep.replace_lines(n + 1, n + 1, {'', '--- AI response ---', ''})\n"
    "  mep.set_cursor(mep.line_count(), 1)\n"
    "  mep_ai_request({{role = 'user', content = prompt}}, nil,\n"
    "    function(delta) mep.insert_text(delta) end,\n"
    "    function(tool_calls) mep.notify('AI response complete') end)\n"
    "end\n"
    "function mep.ai_send_buffer()\n"
    "  local lines = {}\n"
    "  for i = 1, mep.line_count() do lines[#lines + 1] = mep.get_line(i) end\n"
    "  mep.ai_send_text(table.concat(lines, '\\n'))\n"
    "end\n"
    "function mep.ai_send_range(start_row, end_row)\n"
    "  local lines = {}\n"
    "  for i = start_row, end_row do lines[#lines + 1] = mep.get_line(i) end\n"
    "  mep.ai_send_text(table.concat(lines, '\\n'))\n"
    "end\n"
    "mep.command('MepAiSendBuffer', mep.ai_send_buffer)\n"
    // Tools: read_file/list_dir/run_command, each gated by a permission
    // prompt. run_command always re-prompts (no blanket approval, per
    // the plan); the other two support an allow-always-this-session
    // choice, tracked only in memory.
    "mep.ai_tools = {\n"
    "  {name = 'read_file', description = 'Read a file from disk',\n"
    "    parameters = {type = 'object', properties = {path = {type = 'string'}}, required = {'path'}},\n"
    "    run = function(args)\n"
    "      local f = io.open(args.path, 'r')\n"
    "      if not f then return 'ERROR: could not open ' .. tostring(args.path) end\n"
    "      local content = f:read('*a')\n"
    "      f:close()\n"
    "      return content\n"
    "    end},\n"
    "  {name = 'list_dir', description = 'List a directory',\n"
    "    parameters = {type = 'object', properties = {path = {type = 'string'}}, required = {'path'}},\n"
    "    run = function(args)\n"
    "      local names = {}\n"
    "      for _, e in ipairs(mep.list_dir(args.path)) do names[#names + 1] = e.name .. (e.is_dir and '/' or '') end\n"
    "      return table.concat(names, '\\n')\n"
    "    end},\n"
    "}\n"
    "local mep_ai_tool_allow_always = {}\n"
    "local function mep_ai_run_tool(name, args_json, cb)\n"
    "  local args = mep_ai_json_decode(args_json) or {}\n"
    "  if name == 'run_command' then\n"
    "    mep.ui_confirm('Allow run_command: ' .. tostring(args.command) .. '?', false, function(ok)\n"
    "      if not ok then cb('DENIED by user') return end\n"
    "      local out = {}\n"
    "      mep.job_start({'sh', '-c', args.command}, {\n"
    "        on_stdout = function(l) out[#out + 1] = l end,\n"
    "        on_stderr = function(l) out[#out + 1] = l end,\n"
    "        on_exit = function(code) cb(table.concat(out, '\\n') .. '\\n(exit ' .. code .. ')') end,\n"
    "      })\n"
    "    end)\n"
    "    return\n"
    "  end\n"
    "  local tool\n"
    "  for _, t in ipairs(mep.ai_tools) do if t.name == name then tool = t end end\n"
    "  if not tool then cb('ERROR: unknown tool ' .. tostring(name)) return end\n"
    "  if mep_ai_tool_allow_always[name] then cb(tool.run(args)) return end\n"
    "  mep.ui_select({'Allow once', 'Allow always this session', 'Deny'},\n"
    "    'Permission: ' .. name .. '(' .. args_json .. ')', function(sel)\n"
    "      if sel == 1 then cb(tool.run(args))\n"
    "      elseif sel == 2 then mep_ai_tool_allow_always[name] = true cb(tool.run(args))\n"
    "      else cb('DENIED by user') end\n"
    "    end)\n"
    "end\n"
    // Agent mode: a persistent sidebar transcript (Phase 7) driven by a
    // floating mep.ui_input prompt (Phase 3), full multi-turn
    // tool-calling loop. run_command is deliberately NOT advertised to
    // Anthropic (no tools schema built for that provider yet -- see
    // scope-cut note), so its tool loop is single-turn (stream only).
    "mep.ai_agent_messages = {}\n"
    "local mep_ai_agent_sidebar_id = nil\n"
    "local function mep_ai_agent_render()\n"
    "  local widgets = {}\n"
    "  for _, m in ipairs(mep.ai_agent_messages) do\n"
    "    local prefix = m.role == 'user' and '> ' or (m.role == 'tool' and '[tool] ' or '')\n"
    "    for line in ((m.content or '') .. '\\n'):gmatch('(.-)\\n') do\n"
    "      widgets[#widgets + 1] = {id = tostring(#widgets), text = prefix .. line}\n"
    "    end\n"
    "  end\n"
    "  if not mep_ai_agent_sidebar_id then mep_ai_agent_sidebar_id = mep.sidebar_create('AI Agent', 'right', 50) end\n"
    "  mep.sidebar_set_sections(mep_ai_agent_sidebar_id, {{id = 'agent', title = '', collapsed = false, widgets = widgets}})\n"
    "  mep.sidebar_open(mep_ai_agent_sidebar_id)\n"
    "end\n"
    "local function mep_ai_openai_tools_schema()\n"
    "  local out = {}\n"
    "  for _, t in ipairs(mep.ai_tools) do\n"
    "    out[#out + 1] = {type = 'function', ['function'] = {name = t.name, description = t.description, parameters = t.parameters}}\n"
    "  end\n"
    "  out[#out + 1] = {type = 'function', ['function'] = {name = 'run_command', description = 'Run a shell command',\n"
    "    parameters = {type = 'object', properties = {command = {type = 'string'}}, required = {'command'}}}}\n"
    "  return out\n"
    "end\n"
    "function mep.ai_agent_turn()\n"
    "  local assistant_msg = {role = 'assistant', content = ''}\n"
    "  mep.ai_agent_messages[#mep.ai_agent_messages + 1] = assistant_msg\n"
    "  mep_ai_agent_render()\n"
    "  mep_ai_request(mep.ai_agent_messages, mep.ai_provider == 'openai' and mep_ai_openai_tools_schema() or nil,\n"
    "    function(delta)\n"
    "      assistant_msg.content = assistant_msg.content .. delta\n"
    "      mep_ai_agent_render()\n"
    "    end,\n"
    "    function(tool_calls)\n"
    "      if #tool_calls == 0 then mep.notify('AI turn complete') return end\n"
    "      assistant_msg.tool_calls = {}\n"
    "      for _, tc in ipairs(tool_calls) do\n"
    "        assistant_msg.tool_calls[#assistant_msg.tool_calls + 1] =\n"
    "          {id = tc.id, type = 'function', ['function'] = {name = tc.name, arguments = tc.args}}\n"
    "      end\n"
    "      local pending = #tool_calls\n"
    "      for _, tc in ipairs(tool_calls) do\n"
    "        mep_ai_run_tool(tc.name, tc.args, function(result)\n"
    "          mep.ai_agent_messages[#mep.ai_agent_messages + 1] = {role = 'tool', tool_call_id = tc.id, content = result}\n"
    "          mep_ai_agent_render()\n"
    "          pending = pending - 1\n"
    "          if pending == 0 then mep.ai_agent_turn() end\n"
    "        end)\n"
    "      end\n"
    "    end)\n"
    "end\n"
    "function mep.ai_agent_prompt()\n"
    "  mep.ui_input('Ask AI agent:', '', function(text)\n"
    "    if not text or text == '' then return end\n"
    "    mep.ai_agent_messages[#mep.ai_agent_messages + 1] = {role = 'user', content = text}\n"
    "    mep_ai_agent_render()\n"
    "    mep.ai_agent_turn()\n"
    "  end)\n"
    "end\n"
    "mep.command('MepAiAgent', mep.ai_agent_prompt)\n";

// Phase 42 -- Leetcode (stretch, lowest priority). Local-only: problems
// as `.org` files with Prompt/Solution/Tests headline structure, tests
// run by splicing the Solution src block above the Tests src block and
// executing via Phase 34's babel language table. Fetch/submit against
// LeetCode's unofficial API is explicitly deferred, per the plan.
const char *kBuiltinLeetcode =
    "mep.leetcode_dir = nil\n"
    "local function mep_leetcode_title_of(lines)\n"
    "  for _, line in ipairs(lines) do\n"
    "    local t = line:match('^#%+TITLE:%s*(.*)$')\n"
    "    if t then return t end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function mep.leetcode_picker()\n"
    "  if not mep.leetcode_dir then mep.notify('mep.leetcode_dir not configured', 'warn') return end\n"
    "  local items = {}\n"
    "  for _, e in ipairs(mep.list_dir(mep.leetcode_dir)) do\n"
    "    if e.name:match('%.org$') then\n"
    "      local path = mep.leetcode_dir .. '/' .. e.name\n"
    "      local lines = mep_org_read_file_lines(path)\n"
    "      local title = (lines and mep_leetcode_title_of(lines)) or e.name\n"
    "      items[#items + 1] = {display = title, data = path}\n"
    "    end\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No leetcode problems found', 'warn') return end\n"
    "  mep.picker_open('Leetcode Problems', items, function(path)\n"
    "    if path then mep.pane_open(path) end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepLeetcodePicker', mep.leetcode_picker)\n"
    "local function mep_leetcode_find_src_under(heading_name)\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h and h.title == heading_name then\n"
    "      local e = mep_org_subtree_end(i)\n"
    "      for j = i + 1, e - 1 do\n"
    "        local lang = mep.get_line(j):match('^%s*#%+begin_src%s+(%S+)')\n"
    "        if lang then\n"
    "          local body, k = {}, j + 1\n"
    "          while k <= e - 1 and not mep.get_line(k):match('^%s*#%+end_src') do\n"
    "            body[#body + 1] = mep.get_line(k)\n"
    "            k = k + 1\n"
    "          end\n"
    "          return lang, table.concat(body, '\\n')\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return nil, nil\n"
    "end\n"
    "function mep.leetcode_run_tests()\n"
    "  local lang, solution = mep_leetcode_find_src_under('Solution')\n"
    "  local _, tests = mep_leetcode_find_src_under('Tests')\n"
    "  if not solution or not tests then mep.notify('Missing Solution or Tests src block', 'warn') return end\n"
    "  local cmd = mep.org_babel_langs[lang]\n"
    "  if not cmd then mep.notify('No babel support for: ' .. tostring(lang), 'warn') return end\n"
    "  local tmpfile = os.tmpname()\n"
    "  local f = io.open(tmpfile, 'w')\n"
    "  f:write(solution .. '\\n\\n' .. tests)\n"
    "  f:close()\n"
    "  local argv = {}\n"
    "  for _, a in ipairs(cmd) do argv[#argv + 1] = a end\n"
    "  argv[#argv + 1] = tmpfile\n"
    "  local out = {}\n"
    "  mep.job_start(argv, {\n"
    "    cwd = '.',\n"
    "    on_stdout = function(l) out[#out + 1] = l end,\n"
    "    on_stderr = function(l) out[#out + 1] = l end,\n"
    "    on_exit = function(code)\n"
    "      os.remove(tmpfile)\n"
    "      mep.notify('Leetcode tests: ' .. (code == 0 and 'PASSED' or 'FAILED') .. ' (exit ' .. code .. ')',\n"
    "        code == 0 and 'info' or 'error')\n"
    "      if code ~= 0 then for _, l in ipairs(out) do mep.notify(l, 'warn') end end\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "mep.command('MepLeetcodeRunTests', mep.leetcode_run_tests)\n";

const char *kBuiltinPickerSources =
    "function mep.themes()\n"
    "  local before = mep.current_theme()\n"
    "  mep.picker_open('Colorscheme', mep.theme_names(), function(item)\n"
    "    if item then mep.colorscheme(item) else mep.colorscheme(before) end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepTheme', mep.themes)\n"
    // mep.nvim's own convention for this exact picker (mep.nvim/lua/mep/
    // theme/config.lua's `keymaps.picker = {'<leader>ut'}`) -- ported
    // here since mep.themes()/mep.picker_open/mep.leader_map were all
    // already implemented but never actually wired to an entry point.
    "mep.leader_map('ut', 'Theme picker', mep.themes)\n"
    "function mep.find_files()\n"
    "  local lines = {}\n"
    "  mep.job_start({'rg', '--files', '--hidden', '--glob', '!.git'}, {\n"
    "    on_stdout = function(line) lines[#lines + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      if #lines == 0 then\n"
    "        mep.job_start({'find', '.', '-type', 'f', '-not', '-path', '*/.git/*'}, {\n"
    "          on_stdout = function(line) lines[#lines + 1] = line end,\n"
    "          on_exit = function()\n"
    "            mep.picker_open('Find Files', lines, function(item)\n"
    "              if item then mep.cmd('e ' .. item) end\n"
    "            end)\n"
    "          end,\n"
    "        })\n"
    "      else\n"
    "        mep.picker_open('Find Files', lines, function(item)\n"
    "          if item then mep.cmd('e ' .. item) end\n"
    "        end)\n"
    "      end\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "mep.leader_map('pf', 'Find files', mep.find_files)\n"
    "function mep.buffers()\n"
    "  mep.picker_open('Buffers', mep.buffer_list(), function(item)\n"
    "    if item then mep.buffer_switch(tonumber(item)) end\n"
    "  end)\n"
    "end\n"
    "mep.leader_map('bb', 'Buffers', mep.buffers)\n"
    "function mep.commands()\n"
    "  mep.picker_open('Commands', mep.command_names(), function(item)\n"
    "    if item then mep.cmd(item) end\n"
    "  end)\n"
    "end\n";

const char *kAboutText =
    "mep\n"
    "\n"
    "A modal text editor with embedded Lua scripting.\n"
    "raylib + Lua 5.4, compiled to wasm or native.\n"
    "\n"
    "github.com/jordanschupbach/mep";

void ShowOverlay(const std::string &text) {
    g_help_overlay_text = text;
    g_show_help_overlay = true;
}

void BuildMenus() {
    g_menus = {
        {"File",
         {
             {"New", [] { g_editor.NewBuffer(); }},
             {"Open...", [] { g_editor.BeginCommand("e "); }},
             {"Save", [] { g_editor.RunCommand("w"); }},
             {"Save As...", [] { g_editor.BeginCommand("w "); }},
             {"Quit", [] { g_editor.RunCommand("q"); }},
         }},
        {"Edit",
         {
             {"Undo", [] { g_editor.Undo(); }},
             {"Redo", [] { g_editor.Redo(); }},
             {"Cut", [] { g_editor.Cut(); }},
             {"Copy", [] { g_editor.Copy(); }},
             {"Paste", [] { g_editor.Paste(); }},
         }},
        {"Window",
         {
             {"Split Horizontal", [] { g_editor.RunCommand("split"); }},
             {"Split Vertical", [] { g_editor.RunCommand("vsplit"); }},
             {"Close Pane", [] { g_editor.RunCommand("close"); }},
             {"New Tab", [] { g_editor.RunCommand("tabnew"); }},
             {"Close Tab", [] { g_editor.RunCommand("tabdelete"); }},
             {"Next Tab", [] { g_editor.RunCommand("tabnext"); }},
             {"Previous Tab", [] { g_editor.RunCommand("tabprevious"); }},
         }},
        {"Help",
         {
             {"Keybindings", [] { ShowOverlay(kKeybindingsText); }},
             {"About", [] { ShowOverlay(kAboutText); }},
         }},
    };
}

// Computes the x-position and width of each top-level menu label, laid
// out left to right starting at kMenuPaddingX, into g_menu_starts/
// g_menu_widths. Call whenever g_menus or the font size changes; the menu
// bar's own per-frame code just reads the cached result.
void RecomputeMenuLabelLayout() {
    g_menu_starts.resize(g_menus.size());
    g_menu_widths.resize(g_menus.size());
    float x = kMenuPaddingX;
    float font_size = MenuFontSize();
    for (size_t i = 0; i < g_menus.size(); i++) {
        float w = MeasureTextEx(g_font, g_menus[i].label.c_str(), font_size, 0).x + kMenuPaddingX * 2;
        g_menu_starts[i] = x;
        g_menu_widths[i] = w;
        x += w;
    }
}

float DropdownWidth(const Menu &menu) {
    float font_size = MenuFontSize();
    float w = 0;
    for (const auto &item : menu.items) {
        w = std::max(w, MeasureTextEx(g_font, item.label.c_str(), font_size, 0).x);
    }
    return w + kMenuItemPaddingX * 2;
}

// Returns true if the menu bar or help overlay consumed input this frame,
// in which case the editor itself should not process input.
bool HandleMenuInput() {
    if (g_show_help_overlay) {
        if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            g_show_help_overlay = false;
        }
        return true;
    }

    Vector2 mouse = GetMousePosition();
    bool clicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    int bar_height = MenuBarHeight();

    int hovered = -1;
    for (size_t i = 0; i < g_menus.size(); i++) {
        if (mouse.y >= 0 && mouse.y < bar_height && mouse.x >= g_menu_starts[i] &&
            mouse.x < g_menu_starts[i] + g_menu_widths[i]) {
            hovered = static_cast<int>(i);
        }
    }

    if (g_open_menu >= 0) {
        if (hovered >= 0 && hovered != g_open_menu) g_open_menu = hovered;

        const Menu &menu = g_menus[g_open_menu];
        float dd_x = g_menu_starts[g_open_menu];
        float dd_y = bar_height;
        float dd_w = DropdownWidth(menu);
        float dd_h = static_cast<float>(menu.items.size() * MenuItemHeight());
        bool inside_dropdown =
            mouse.x >= dd_x && mouse.x < dd_x + dd_w && mouse.y >= dd_y && mouse.y < dd_y + dd_h;

        if (clicked) {
            if (inside_dropdown) {
                int idx = static_cast<int>((mouse.y - dd_y) / MenuItemHeight());
                if (idx >= 0 && idx < static_cast<int>(menu.items.size())) {
                    menu.items[idx].action();
                }
                g_open_menu = -1;
            } else if (hovered < 0) {
                g_open_menu = -1;
            }
        }
        if (IsKeyPressed(KEY_ESCAPE)) g_open_menu = -1;
        return true;
    }

    if (clicked && hovered >= 0) {
        g_open_menu = hovered;
        return true;
    }
    return false;
}

void DrawMenuBar() {
    int screen_w = GetScreenWidth();
    int bar_height = MenuBarHeight();
    float font_size = MenuFontSize();

    DrawRectangle(0, 0, screen_w, bar_height, ResolveHlGroup("MenuBar"));

    for (size_t i = 0; i < g_menus.size(); i++) {
        bool active = (static_cast<int>(i) == g_open_menu);
        if (active) {
            DrawRectangle(static_cast<int>(g_menu_starts[i]), 0, static_cast<int>(g_menu_widths[i]), bar_height,
                          ResolveHlGroup("MenuHighlight"));
        }
        float text_y = (bar_height - font_size) / 2.0f;
        DrawTextEx(g_font, g_menus[i].label.c_str(),
                   Vector2{g_menu_starts[i] + kMenuPaddingX, text_y}, font_size, 0, ResolveHlGroup("MenuBarFg"));
    }

    if (g_open_menu >= 0) {
        const Menu &menu = g_menus[g_open_menu];
        float dd_x = g_menu_starts[g_open_menu];
        float dd_y = bar_height;
        float dd_w = DropdownWidth(menu);
        int item_h = MenuItemHeight();
        Vector2 mouse = GetMousePosition();

        DrawRectangle(static_cast<int>(dd_x), static_cast<int>(dd_y), static_cast<int>(dd_w),
                      static_cast<int>(menu.items.size() * item_h), ResolveHlGroup("Picker"));
        for (size_t i = 0; i < menu.items.size(); i++) {
            float item_y = dd_y + i * item_h;
            bool hovered_item = mouse.x >= dd_x && mouse.x < dd_x + dd_w && mouse.y >= item_y &&
                                 mouse.y < item_y + item_h;
            if (hovered_item) {
                DrawRectangle(static_cast<int>(dd_x), static_cast<int>(item_y), static_cast<int>(dd_w), item_h,
                              ResolveHlGroup("MenuHighlight"));
            }
            float text_y = item_y + (item_h - font_size) / 2.0f;
            DrawTextEx(g_font, menu.items[i].label.c_str(), Vector2{dd_x + kMenuItemPaddingX, text_y}, font_size, 0,
                       ResolveHlGroup("MenuBarFg"));
        }
        DrawRectangleLines(static_cast<int>(dd_x), static_cast<int>(dd_y), static_cast<int>(dd_w),
                            static_cast<int>(menu.items.size() * item_h), ResolveHlGroup("PickerBorder"));
    }
}

// Generic floating overlay frame: dims the screen, draws a centered
// bordered box with an optional title line, returns where content should
// start drawing. Shared by the Prompt/Confirm/Select overlays below and
// meant to be reused by later phases (help popups, tooltips, picker/
// sidebar floats -- NVIM_PARITY_PLAN.md Part I Phase 3).
struct FloatFrame {
    int box_x, box_y, box_w, box_h;
    float content_x, content_y;
};

FloatFrame DrawFloatFrame(int w, int h, const std::string &title) {
    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    DrawRectangle(0, 0, screen_w, screen_h, ResolveHlGroup("Overlay"));
    int box_x = (screen_w - w) / 2;
    int box_y = (screen_h - h) / 2;
    DrawRectangle(box_x, box_y, w, h, ResolveHlGroup("FloatBg"));
    DrawRectangleLines(box_x, box_y, w, h, ResolveHlGroup("FloatBorder"));
    float content_y = static_cast<float>(box_y + 10);
    float title_size = MenuFontSize();
    if (!title.empty()) {
        DrawTextEx(g_font, title.c_str(), Vector2{static_cast<float>(box_x + 14), content_y}, title_size, 0,
                   ResolveHlGroup("PickerTitle"));
        content_y += title_size + 8;
    }
    return {box_x, box_y, w, h, static_cast<float>(box_x + 14), content_y};
}

void DrawPromptOverlay() {
    int box_w = std::min(GetScreenWidth() - 80, 560);
    FloatFrame f = DrawFloatFrame(box_w, static_cast<int>(g_font_size) + 60, g_editor.PromptTitle());
    std::string line = g_editor.PromptInput();
    DrawTextEx(g_font, line.c_str(), Vector2{f.content_x, f.content_y}, g_font_size, 0, ResolveHlGroup("Normal"));
    if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
        float cx = f.content_x + MeasureTextEx(g_font, line.c_str(), g_font_size, 0).x;
        DrawRectangle(static_cast<int>(cx), static_cast<int>(f.content_y), 2, static_cast<int>(g_font_size),
                      ResolveHlGroup("Normal"));
    }
}

void DrawConfirmOverlay() {
    const std::string &msg = g_editor.ConfirmMessage();
    int box_w = std::min(GetScreenWidth() - 80,
                          static_cast<int>(MeasureTextEx(g_font, msg.c_str(), g_font_size, 0).x) + 60);
    box_w = std::max(box_w, 260);
    FloatFrame f = DrawFloatFrame(box_w, static_cast<int>(g_font_size) * 2 + 50, "");
    DrawTextEx(g_font, msg.c_str(), Vector2{f.content_x, f.content_y}, g_font_size, 0, ResolveHlGroup("Normal"));
    std::string hint = g_editor.ConfirmDefaultYes() ? "[y]es / [n]o (Enter = yes, Esc = no)"
                                                     : "[y]es / [n]o (Enter = no, Esc = no)";
    float hint_size = MenuFontSize();
    DrawTextEx(g_font, hint.c_str(), Vector2{f.content_x, f.content_y + g_font_size + 10}, hint_size, 0, ResolveHlGroup("Comment"));
}

void DrawSelectOverlay() {
    const std::vector<std::string> &items = g_editor.SelectItems();
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 6;
    float max_w = MeasureTextEx(g_font, g_editor.SelectTitle().c_str(), MenuFontSize(), 0).x;
    for (const auto &it : items) max_w = std::max(max_w, MeasureTextEx(g_font, it.c_str(), font_size, 0).x);
    int box_w = std::min(GetScreenWidth() - 80, static_cast<int>(max_w) + 60);
    int box_h = std::min(GetScreenHeight() - 80, static_cast<int>(items.size()) * line_h + 60);
    FloatFrame f = DrawFloatFrame(box_w, box_h, g_editor.SelectTitle());
    int sel = g_editor.SelectIndex();
    for (size_t i = 0; i < items.size(); i++) {
        float y = f.content_y + i * line_h;
        if (static_cast<int>(i) == sel) {
            DrawRectangle(f.box_x + 6, static_cast<int>(y) - 1, f.box_w - 12, line_h, ResolveHlGroup("PickerSelected"));
        }
        DrawTextEx(g_font, items[i].c_str(), Vector2{f.content_x, y}, font_size, 0, ResolveHlGroup("Normal"));
    }
}

Color NotifyLevelColor(Editor::NotifyLevel level) {
    switch (level) {
        case Editor::NotifyLevel::Error: return ResolveHlGroup("Error");
        case Editor::NotifyLevel::Warn: return ResolveHlGroup("Warn");
        case Editor::NotifyLevel::Info: return ResolveHlGroup("Info");
        case Editor::NotifyLevel::Debug: return ResolveHlGroup("Debug");
    }
    return ResolveHlGroup("Normal");
}

const char *NotifyLevelGlyph(Editor::NotifyLevel level) {
    // Plain ASCII, not Unicode symbol codepoints -- the embedded font's
    // glyph subset doesn't cover the latter (they rendered as tofu "?"),
    // and ASCII-first is the plan's own icon-rendering fallback anyway
    // (Phase 10).
    switch (level) {
        case Editor::NotifyLevel::Error: return "X";
        case Editor::NotifyLevel::Warn: return "!";
        case Editor::NotifyLevel::Info: return "i";
        case Editor::NotifyLevel::Debug: return ".";
    }
    return "";
}

// Toast stack, top-right corner, newest closest to the corner (Phase 6).
void DrawToastStack() {
    const auto &toasts = g_editor.Toasts();
    if (toasts.empty()) return;
    int screen_w = GetScreenWidth();
    float font_size = MenuFontSize();
    float y = 8.0f;
    for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
        std::string text = std::string(NotifyLevelGlyph(it->level)) + "  " + it->message;
        float text_w = MeasureTextEx(g_font, text.c_str(), font_size, 0).x;
        float box_w = text_w + 24;
        float box_h = font_size + 14;
        float x = screen_w - box_w - 10;
        DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(box_w), static_cast<int>(box_h),
                      ResolveHlGroup("FloatBg"));
        DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(box_w),
                            static_cast<int>(box_h), NotifyLevelColor(it->level));
        DrawTextEx(g_font, text.c_str(), Vector2{x + 12, y + 7}, font_size, 0, ResolveHlGroup("Normal"));
        y += box_h + 6;
    }
}

// Renders every open sidebar as a docked box flush against its configured
// edge (Phase 7) -- stacked if more than one shares an edge. Content comes
// from Editor::FlattenSidebar so rendering and keyboard navigation
// (Editor::HandleSidebarInput) can never disagree about line layout.
void DrawSidebars() {
    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 4;
    // Only ever called outside zen mode (DrawEditor's `if (!zen)
    // DrawSidebars()`), so these mirror DrawEditor's own layout constants
    // for that case -- content_top/content_bottom bound the exact same
    // vertical band the pane tree itself is reserved to (its pane_area_h),
    // so a docked sidebar sits alongside pane content instead of painting
    // over the menu bar/tab bar above it or the status/command bars below
    // it, the same way DrawEditor's pane_x/pane_w reservation already
    // keeps it from painting over pane content horizontally.
    int content_top = MenuBarHeight() + (g_editor.TabCount() > 1 ? TabBarHeight() : 0);
    int content_bottom = screen_h - 2 * LineHeight();  // status bar + command bar
    float left_offset = 0, right_offset = 0, top_offset = 0, bottom_offset = 0;
    // FocusedSidebarId() alone isn't enough now that mod1+hjkl can blur a
    // sidebar back into the pane tree without closing it (NavigatePane
    // Direction) -- that leaves focused_sidebar_id_ set (so mod1+hjkl back
    // the other way can refocus the same sidebar) but the sidebar no
    // longer actually has input focus, so its cursor highlight shouldn't
    // draw as if it still does.
    int focused_id = (g_editor.CurrentMode() == Mode::Sidebar) ? g_editor.FocusedSidebarId() : 0;

    for (const SidebarInstance &sb : g_editor.Sidebars()) {
        if (!sb.open) continue;
        std::vector<SidebarLine> lines = g_editor.FlattenSidebar(sb.id);
        int px, py, pw, ph;
        int px_w = sb.size * static_cast<int>(g_char_width);
        int header_h = line_h + 6;
        int content_h = static_cast<int>(lines.size()) * line_h + header_h + 10;
        if (sb.position == "left") {
            px = static_cast<int>(left_offset);
            py = content_top;
            pw = px_w;
            ph = content_bottom - content_top;
            left_offset += px_w;
        } else if (sb.position == "top") {
            px = 0;
            py = content_top + static_cast<int>(top_offset);
            pw = screen_w;
            ph = std::min(content_h, (content_bottom - content_top) / 2);
            top_offset += ph;
        } else if (sb.position == "bottom") {
            ph = std::min(content_h, (content_bottom - content_top) / 2);
            px = 0;
            py = content_bottom - ph - static_cast<int>(bottom_offset);
            pw = screen_w;
            bottom_offset += ph;
        } else {  // "right"
            px = screen_w - px_w - static_cast<int>(right_offset);
            py = content_top;
            pw = px_w;
            ph = content_bottom - content_top;
            right_offset += px_w;
        }

        DrawRectangle(px, py, pw, ph, ResolveHlGroup("Sidebar"));
        DrawRectangleLines(px, py, pw, ph, ResolveHlGroup("SidebarBorder"));

        // Title/line text is drawn at a fixed x (px + 8) with no wrapping,
        // so a long entry (a deep file-tree path, a long symbol name) or a
        // narrow sidebar (mod1+Shift+h/l, ResizeActivePane's sidebar
        // branch) would otherwise overflow past pw into whatever's drawn
        // next -- the pane tree to one side. Scissor to the sidebar's own
        // rect the same way DrawPane clips its content.
        BeginScissorMode(px, py, pw, ph);
        DrawTextEx(g_font, sb.title.c_str(), Vector2{static_cast<float>(px + 8), static_cast<float>(py + 6)},
                   MenuFontSize(), 0, ResolveHlGroup("SidebarTitle"));

        bool is_focused = sb.id == focused_id;
        int cursor = g_editor.SidebarCursor();
        for (size_t i = 0; i < lines.size(); i++) {
            float ly = py + header_h + i * line_h;
            if (is_focused && static_cast<int>(i) == cursor) {
                DrawRectangle(px + 2, static_cast<int>(ly) - 1, pw - 4, line_h, ResolveHlGroup("PickerSelected"));
            }
            Color color = lines[i].hl.empty() ? ResolveHlGroup("Normal") : ResolveHlGroup(lines[i].hl);
            DrawTextEx(g_font, lines[i].text.c_str(), Vector2{static_cast<float>(px + 8), ly}, font_size, 0, color);
        }
        EndScissorMode();
    }
}

// Fuzzy picker: prompt line + live-filtered results list (Phase 8). A
// preview pane is a documented follow-up, not implemented here.
void DrawPickerOverlay() {
    int box_w = std::min(GetScreenWidth() - 80, 640);
    int box_h = std::min(GetScreenHeight() - 80, 420);
    FloatFrame f = DrawFloatFrame(box_w, box_h, g_editor.PickerTitle());

    std::string prompt_line = "> " + g_editor.PickerQuery();
    DrawTextEx(g_font, prompt_line.c_str(), Vector2{f.content_x, f.content_y}, g_font_size, 0, ResolveHlGroup("Normal"));
    if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
        float cx = f.content_x + MeasureTextEx(g_font, prompt_line.c_str(), g_font_size, 0).x;
        DrawRectangle(static_cast<int>(cx), static_cast<int>(f.content_y), 2, static_cast<int>(g_font_size),
                      ResolveHlGroup("Normal"));
    }
    DrawLine(f.box_x + 4, static_cast<int>(f.content_y + g_font_size + 6), f.box_x + f.box_w - 4,
             static_cast<int>(f.content_y + g_font_size + 6), ResolveHlGroup("PickerBorder"));

    std::vector<PickerItem> results = g_editor.PickerFilteredResults();
    float list_y = f.content_y + g_font_size + 14;
    int line_h = static_cast<int>(g_font_size) + 4;
    int max_rows = std::max(1, static_cast<int>((f.box_y + f.box_h - list_y) / line_h));
    int selected = g_editor.PickerSelected();
    int start = std::max(0, selected - max_rows + 1);
    for (int i = start; i < static_cast<int>(results.size()) && i < start + max_rows; i++) {
        float ry = list_y + (i - start) * line_h;
        if (i == selected) {
            DrawRectangle(f.box_x + 4, static_cast<int>(ry) - 1, f.box_w - 8, line_h, ResolveHlGroup("PickerSelected"));
        }
        DrawTextEx(g_font, results[i].display.c_str(), Vector2{f.content_x, ry}, g_font_size, 0, ResolveHlGroup("Normal"));
    }
    if (results.empty()) {
        DrawTextEx(g_font, "-- no matches --", Vector2{f.content_x, list_y}, g_font_size, 0, ResolveHlGroup("Comment"));
    }
}

// Whichkey popup (Phase 11): lists what's bound under the currently typed
// leader prefix, narrowing as more keys are typed; an exact unique match
// fires immediately (see Editor::HandleWhichKeyInput), so this never has to
// render a "chosen" state -- only ever "still choosing". Docked along the
// bottom of the screen (mirroring which-key.nvim) rather than centered like
// the other floats -- DrawFloatFrame always centers, so this positions its
// own box instead of going through it.
void DrawWhichKeyOverlay() {
    std::vector<std::pair<std::string, std::string>> matches = g_editor.WhichKeyMatches();
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 6;

    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    int margin_x = 40;
    int box_w = std::max(screen_w - margin_x * 2, 200);

    std::string title = "<leader>" + g_editor.WhichKeyPrefix();
    float title_size = MenuFontSize();
    int title_h = title.empty() ? 0 : static_cast<int>(title_size) + 8;

    // Now that the bar spans the screen width, flow entries into as many
    // columns as fit rather than one long column (mirrors which-key.nvim's
    // grid layout instead of leaving most of the width empty).
    int item_w = 0;
    for (const auto &m : matches) {
        std::string line = m.first + "  " + m.second;
        item_w = std::max(item_w, static_cast<int>(MeasureTextEx(g_font, line.c_str(), font_size, 0).x));
    }
    item_w += 28;
    int content_w = box_w - 28;
    int columns = std::max(1, item_w > 0 ? content_w / item_w : 1);
    int rows = matches.empty() ? 1 : (static_cast<int>(matches.size()) + columns - 1) / columns;
    int box_h = std::min(screen_h - 80, rows * line_h + title_h + 20);
    box_h = std::max(box_h, line_h + 20 + title_h);

    // Sit just above the status/command bars rather than overlapping them --
    // command_bar_height/status_bar_height mirror DrawEditor's own layout
    // (zen mode hides the status bar but keeps the command line).
    int line_height = LineHeight();
    int command_bar_height = line_height;
    int status_bar_height = g_editor.IsZenMode() ? 0 : line_height;
    int bottom_margin = command_bar_height + status_bar_height + 8;

    DrawRectangle(0, 0, screen_w, screen_h, ResolveHlGroup("Overlay"));
    int box_x = (screen_w - box_w) / 2;
    int box_y = screen_h - bottom_margin - box_h;
    DrawRectangle(box_x, box_y, box_w, box_h, ResolveHlGroup("FloatBg"));
    DrawRectangleLines(box_x, box_y, box_w, box_h, ResolveHlGroup("FloatBorder"));

    float content_x = static_cast<float>(box_x + 14);
    float content_y = static_cast<float>(box_y + 10);
    if (!title.empty()) {
        DrawTextEx(g_font, title.c_str(), Vector2{content_x, content_y}, title_size, 0, ResolveHlGroup("PickerTitle"));
        content_y += title_size + 8;
    }
    for (size_t i = 0; i < matches.size(); i++) {
        int col = static_cast<int>(i) % columns;
        int row = static_cast<int>(i) / columns;
        float x = content_x + col * item_w;
        float y = content_y + row * line_h;
        DrawTextEx(g_font, matches[i].first.c_str(), Vector2{x, y}, font_size, 0, ResolveHlGroup("PickerTitle"));
        float key_w = MeasureTextEx(g_font, matches[i].first.c_str(), font_size, 0).x;
        DrawTextEx(g_font, matches[i].second.c_str(), Vector2{x + key_w + 16, y}, font_size, 0,
                   ResolveHlGroup("Normal"));
    }
}

void DrawHelpOverlay() {
    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    DrawRectangle(0, 0, screen_w, screen_h, ResolveHlGroup("Overlay"));

    std::vector<std::string> lines = SplitLines(g_help_overlay_text);
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 6;

    float max_w = 0;
    for (const auto &line : lines) {
        max_w = std::max(max_w, MeasureTextEx(g_font, line.c_str(), font_size, 0).x);
    }

    int box_w = std::min(screen_w - 40, static_cast<int>(max_w) + 40);
    int box_h = std::min(screen_h - 40, static_cast<int>(lines.size()) * line_h + 50);
    int box_x = (screen_w - box_w) / 2;
    int box_y = (screen_h - box_h) / 2;

    DrawRectangle(box_x, box_y, box_w, box_h, ResolveHlGroup("FloatBg"));
    DrawRectangleLines(box_x, box_y, box_w, box_h, ResolveHlGroup("FloatBorder"));

    for (size_t i = 0; i < lines.size(); i++) {
        float y = box_y + 16 + i * line_h;
        DrawTextEx(g_font, lines[i].c_str(), Vector2{static_cast<float>(box_x + 18), y}, font_size, 0, ResolveHlGroup("Normal"));
    }

    std::string hint = "Press Escape or click to close";
    float hint_size = MenuFontSize();
    float hint_w = MeasureTextEx(g_font, hint.c_str(), hint_size, 0).x;
    DrawTextEx(g_font, hint.c_str(),
               Vector2{static_cast<float>(box_x + box_w) - hint_w - 14, static_cast<float>(box_y + box_h - hint_size - 10)},
               hint_size, 0, ResolveHlGroup("Comment"));
}

// Draws one pane's header (buffer name) and content within the rectangle
// (x, y, w, h), clipped so long lines or a pane's neighbors never bleed
// into each other. Only the active pane shows the blinking cursor and any
// Visual-mode selection -- Normal/Insert/Visual/Command mode is global to
// whichever pane has focus, not per-pane.
// Completion popup (Phase 22): a small list anchored at (x, y), the cursor's
// screen position -- deliberately not a Phase 3 float (those dim/center the
// whole screen, wrong for something meant to sit unobtrusively next to the
// cursor while the user keeps typing).
void DrawCompletionPopup(float x, float y) {
    const std::vector<PickerItem> &items = g_editor.CompletionItems();
    if (items.empty()) return;
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 4;
    float max_w = 60;
    for (const auto &it : items) max_w = std::max(max_w, MeasureTextEx(g_font, it.display.c_str(), font_size, 0).x);
    int box_w = static_cast<int>(max_w) + 16;
    int max_rows = std::min(static_cast<int>(items.size()), 8);
    int box_h = max_rows * line_h + 6;
    if (x + box_w > GetScreenWidth()) x = GetScreenWidth() - box_w;
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), box_w, box_h, ResolveHlGroup("Picker"));
    DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), box_w, box_h, ResolveHlGroup("PickerBorder"));
    int selected = g_editor.CompletionSelected();
    int start = std::max(0, std::min(selected - max_rows + 1, static_cast<int>(items.size()) - max_rows));
    for (int i = start; i < static_cast<int>(items.size()) && i < start + max_rows; i++) {
        float ry = y + 3 + (i - start) * line_h;
        if (i == selected) {
            DrawRectangle(static_cast<int>(x) + 1, static_cast<int>(ry), box_w - 2, line_h,
                          ResolveHlGroup("PickerSelected"));
        }
        DrawTextEx(g_font, items[i].display.c_str(), Vector2{x + 6, ry}, font_size, 0, ResolveHlGroup("Normal"));
    }
}

// Command-line completion popup: same look as DrawCompletionPopup above,
// but anchored by its bottom edge (bottom_y) rather than its top -- the
// command bar sits at the very bottom of the window, so the list has to
// grow upward off of it rather than downward off a cursor.
void DrawCmdlineCompletionPopup(float x, float bottom_y) {
    const std::vector<PickerItem> &items = g_editor.CmdlineCompletionItems();
    if (items.empty()) return;
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 4;
    float max_w = 60;
    for (const auto &it : items) max_w = std::max(max_w, MeasureTextEx(g_font, it.display.c_str(), font_size, 0).x);
    int box_w = static_cast<int>(max_w) + 16;
    int max_rows = std::min(static_cast<int>(items.size()), 8);
    int box_h = max_rows * line_h + 6;
    if (x + box_w > GetScreenWidth()) x = GetScreenWidth() - box_w;
    float y = bottom_y - box_h;
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), box_w, box_h, ResolveHlGroup("Picker"));
    DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), box_w, box_h, ResolveHlGroup("PickerBorder"));
    int selected = g_editor.CmdlineCompletionSelected();
    int start = std::max(0, std::min(selected - max_rows + 1, static_cast<int>(items.size()) - max_rows));
    for (int i = start; i < static_cast<int>(items.size()) && i < start + max_rows; i++) {
        float ry = y + 3 + (i - start) * line_h;
        if (i == selected) {
            DrawRectangle(static_cast<int>(x) + 1, static_cast<int>(ry), box_w - 2, line_h,
                          ResolveHlGroup("PickerSelected"));
        }
        DrawTextEx(g_font, items[i].display.c_str(), Vector2{x + 6, ry}, font_size, 0, ResolveHlGroup("Normal"));
    }
}

// Maps a VTerm cell color to an actual raylib Color (vterm.h itself has no
// rendering dependency -- see its header comment -- so this mapping lives
// here instead). Default defers to mep's own theme so a terminal pane
// still matches whatever colorscheme is active; Indexed covers both the
// classic 16-color ANSI palette (fixed, standard-ish xterm values) and the
// 16-255 6x6x6 color cube + grayscale ramp most modern CLI tools (ls
// --color, git, npm, ripgrep, ...) actually use; Rgb is 24-bit true color.
Color VTermColorToRaylib(const VTermColor &c, bool is_fg) {
    static const Color kAnsi16[16] = {
        Color{0, 0, 0, 255},       Color{205, 49, 49, 255},   Color{13, 188, 121, 255}, Color{229, 229, 16, 255},
        Color{36, 114, 200, 255},  Color{188, 63, 188, 255},  Color{17, 168, 205, 255}, Color{229, 229, 229, 255},
        Color{102, 102, 102, 255}, Color{241, 76, 76, 255},   Color{35, 209, 139, 255}, Color{245, 245, 67, 255},
        Color{59, 142, 234, 255},  Color{214, 112, 214, 255}, Color{41, 184, 219, 255}, Color{255, 255, 255, 255},
    };
    switch (c.kind) {
        case VTermColorKind::Default:
            return ResolveHlGroup(is_fg ? "Normal" : "NormalBg");
        case VTermColorKind::Rgb:
            return Color{c.r, c.g, c.b, 255};
        case VTermColorKind::Indexed:
        default: {
            int idx = c.index;
            if (idx < 16) return kAnsi16[idx];
            if (idx < 232) {
                int n = idx - 16;
                int r = n / 36, g = (n / 6) % 6, b = n % 6;
                auto ramp = [](int v) { return static_cast<unsigned char>(v == 0 ? 0 : v * 40 + 55); };
                return Color{ramp(r), ramp(g), ramp(b), 255};
            }
            unsigned char v = static_cast<unsigned char>(8 + (idx - 232) * 10);
            return Color{v, v, v, 255};
        }
    }
}

// Renders a `:terminal` pane straight from its VTerm grid (Editor::
// GetTerminal) rather than Buffer::lines -- a terminal pane's buffer holds
// no real text, see TerminalSession's header comment. scroll_offset > 0
// (Shift+PageUp/PageDown, HandleTerminalInput) blends scrollback lines in
// from the top instead of the live screen.
void DrawTerminalGrid(const TerminalSession &sess, float x, float y, float w, float h) {
    const VTerm *term = sess.vterm.get();
    if (!term) return;
    int rows = term->Rows(), cols = term->Cols();
    float cw = g_char_width, lh = LineHeight();

    int sb_lines = term->ScrollbackLines();
    for (int r = 0; r < rows; r++) {
        float ry = y + r * lh;
        if (ry + lh < y || ry > y + h) continue;
        // Addresses scrollback (indices [0, sb_lines)) and the live grid
        // (indices [sb_lines, sb_lines+rows)) as one combined history;
        // scroll_offset shifts the rows-tall visible window back from the
        // bottom of it. At scroll_offset 0 this must reduce to "row r of
        // the visible area is live grid row r" -- an earlier version of
        // this (r - rows + scroll_offset) instead produced a negative
        // index for every row whenever scroll_offset was 0, so the
        // terminal rendered as permanently blank regardless of actual
        // output; verified fixed by the At()/ScrollbackAt() split below
        // actually being reached instead of always falling to the blank
        // fallback.
        int combined_index = sb_lines - sess.scroll_offset + r;
        for (int c = 0; c < cols; c++) {
            const VTermCell *cell;
            VTermCell blank_fallback;
            if (combined_index < 0) {
                cell = &blank_fallback;
            } else if (combined_index < sb_lines) {
                cell = &term->ScrollbackAt(combined_index, c);
            } else {
                cell = &term->At(combined_index - sb_lines, c);
            }
            float cx = x + c * cw;
            const VTermColor &fg_c = cell->reverse ? cell->bg : cell->fg;
            const VTermColor &bg_c = cell->reverse ? cell->fg : cell->bg;
            Color bg = VTermColorToRaylib(bg_c, false);
            Color fg = VTermColorToRaylib(fg_c, true);
            if (cell->faint) fg = Color{static_cast<unsigned char>(fg.r / 2), static_cast<unsigned char>(fg.g / 2),
                                         static_cast<unsigned char>(fg.b / 2), fg.a};
            if (bg_c.kind != VTermColorKind::Default || cell->reverse) {
                DrawRectangle(static_cast<int>(cx), static_cast<int>(ry), static_cast<int>(cw) + 1,
                              static_cast<int>(lh), bg);
            }
            if (cell->ch != " " && !cell->ch.empty()) {
                DrawTextEx(g_font, cell->ch.c_str(), Vector2{cx, ry}, g_font_size, 0, fg);
                // Bold approximated by a 1px-offset second draw (no bold
                // glyph variant of the loaded font is guaranteed to
                // exist) rather than a brighter color -- keeps bold
                // readable even for colors already at full brightness.
                if (cell->bold) DrawTextEx(g_font, cell->ch.c_str(), Vector2{cx + 1, ry}, g_font_size, 0, fg);
            }
            if (cell->underline) {
                DrawRectangle(static_cast<int>(cx), static_cast<int>(ry + lh - 2), static_cast<int>(cw), 1, fg);
            }
        }
    }

    if (sess.scroll_offset == 0 && term->CursorVisible() && !sess.exited) {
        if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
            float cx = x + term->CursorCol() * cw;
            float cy = y + term->CursorRow() * lh;
            Color cursor_bg = ResolveHlGroup("Normal");
            DrawRectangle(static_cast<int>(cx), static_cast<int>(cy), static_cast<int>(cw), static_cast<int>(lh),
                          Color{cursor_bg.r, cursor_bg.g, cursor_bg.b, 180});
            const VTermCell &under = term->At(term->CursorRow(), term->CursorCol());
            if (under.ch != " " && !under.ch.empty()) {
                DrawTextEx(g_font, under.ch.c_str(), Vector2{cx, cy}, g_font_size, 0, ResolveHlGroup("NormalBg"));
            }
        }
    }
}

void DrawPane(const Pane &pane, float x, float y, float w, float h, bool is_active) {
    int line_height = LineHeight();
    int header_h = PaneHeaderHeight();
    float font_size = MenuFontSize();

    const Buffer &buf = g_editor.GetBuffer(pane.buffer_id);
    Color header_bg = is_active ? ResolveHlGroup("TabActive") : ResolveHlGroup("MenuBar");
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), header_h, header_bg);
    const TerminalSession *term_sess = g_editor.GetTerminal(pane.buffer_id);
    std::string label;
    if (term_sess) {
        const std::string &live_title =
            (term_sess->vterm && !term_sess->vterm->Title().empty()) ? term_sess->vterm->Title() : term_sess->title;
        label = "Terminal: " + live_title;
        if (term_sess->exited) label += " [exited: " + std::to_string(term_sess->exit_code) + "]";
    } else {
        label = (buf.scratch ? "[Scratch]" : (buf.filename.empty() ? "[No Name]" : buf.filename)) +
                (buf.modified ? " [+]" : "");
    }
    // Per-pane buffer tabs (Phase 14): shown only once a pane holds more
    // than one, so a plain single-buffer pane's header looks unchanged.
    if (pane.buffer_tabs.size() > 1) {
        label += "  [" + std::to_string(pane.buffer_tab_index + 1) + "/" + std::to_string(pane.buffer_tabs.size()) +
                 "]";
    }
    DrawTextEx(g_font, label.c_str(), Vector2{x + 6, y + (header_h - font_size) / 2.0f}, font_size, 0, ResolveHlGroup("Normal"));

    float content_y = y + header_h;
    float content_h = h - header_h;

    if (term_sess) {
        // Kept sized to the pane's real geometry regardless of which view
        // (below) is currently drawn, so a full-screen program inside it
        // (or the live grid itself, once shown again) is never wrapping
        // against a stale size.
        int cols = std::max(1, static_cast<int>(w / g_char_width));
        int trows = std::max(1, static_cast<int>(content_h / line_height));
        g_editor.ResizeTerminal(pane.buffer_id, trows, cols);

        // Ctrl-\ Ctrl-N (Editor::EnterTerminalNormalMode) snapshots this
        // pane's terminal text into Buffer::lines and drops to
        // Mode::Normal so ordinary buffer navigation/Visual-yank/search
        // work on it -- shown by falling through to the ordinary
        // buffer-drawing path below instead of the live grid, exactly
        // while that state applies to *this* (the active) pane. A
        // background terminal pane always keeps showing its live grid
        // regardless of the active pane's mode -- you still want to see
        // it still running while browsing a different one.
        bool show_live_grid = !is_active || g_editor.CurrentMode() != Mode::Normal;
        if (show_live_grid) {
            BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                              static_cast<int>(content_h));
            DrawTerminalGrid(*term_sess, x, content_y, w, content_h);
            EndScissorMode();
            Color term_border = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
            DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                                term_border);
            return;
        }
        // Falls through to the ordinary buffer-drawing path below, same
        // as any other buffer.
    }

    int visible_lines = std::max(1, static_cast<int>(content_h / line_height));
    g_editor.UpdateScrollForPane(pane.id, visible_lines);

    BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                      static_cast<int>(content_h));

    bool has_selection = is_active && g_editor.HasVisualSelection();
    bool linewise_selection = g_editor.CurrentMode() == Mode::VisualLine;
    bool block_selection = g_editor.CurrentMode() == Mode::VisualBlock;
    CursorPos sel_start{}, sel_end{};
    int block_top = 0, block_bottom = 0, block_left = 0, block_right = 0;
    if (block_selection) {
        g_editor.VisualBlockRange(block_top, block_bottom, block_left, block_right);
    } else if (has_selection) {
        g_editor.VisualRange(sel_start, sel_end);
    }

    // :set number -- a right-aligned gutter wide enough for the buffer's
    // largest line number plus one trailing space, drawn once here and
    // used to shift every other x-coordinate below (selection highlight,
    // cursor, and the line text itself) rather than threading it through
    // each of them individually.
    float gutter_w = 0.0f;
    if (g_editor.ShowLineNumbers()) {
        int digits = 1;
        for (int n = buf.LineCount(); n >= 10; n /= 10) digits++;
        gutter_w = (digits + 1) * g_char_width;
    }
    float text_x = x + kMarginX + gutter_w;

    int last_line = std::min(pane.scroll_row + visible_lines, buf.LineCount());
    int visual_slot = 0;  // a closed fold collapses N buffer rows into 1 of these
    for (int row = pane.scroll_row; row < last_line; row++) {
        float ly = content_y + visual_slot * line_height;
        visual_slot++;

        // Closed fold starting here: render a one-line summary in its
        // place and skip straight past its hidden rows (Phase 5) -- a row
        // *inside* a closed fold, as opposed to its start, never reaches
        // this loop body at all once `row` jumps past it below.
        const Fold *fold_here = nullptr;
        for (const Fold &f : buf.folds) {
            if (f.closed && f.start_row == row) {
                if (!fold_here || (f.end_row - f.start_row) > (fold_here->end_row - fold_here->start_row)) {
                    fold_here = &f;
                }
            }
        }
        if (fold_here) {
            int hidden = fold_here->end_row - fold_here->start_row;
            std::string summary = "+-- " + std::to_string(hidden + 1) + " lines: " + buf.lines[row] + " ---";
            DrawTextEx(g_font, summary.c_str(), Vector2{text_x, ly}, g_font_size, 0, ResolveHlGroup("SidebarTitle"));
            row = fold_here->end_row;
            continue;
        }

        if (block_selection && row >= block_top && row <= block_bottom) {
            int line_len = static_cast<int>(buf.lines[row].size());
            int cs = block_left;
            int ce = (block_right < 0) ? line_len + 1 : block_right + 1;
            float x0 = text_x + cs * g_char_width;
            float x1 = text_x + ce * g_char_width;
            Color sel_color = ResolveHlGroup("Visual");
            DrawRectangle(static_cast<int>(x0), static_cast<int>(ly), static_cast<int>(x1 - x0), line_height,
                          Color{sel_color.r, sel_color.g, sel_color.b, 160});
        } else if (has_selection && row >= sel_start.row && row <= sel_end.row) {
            int line_len = static_cast<int>(buf.lines[row].size());
            int cs = (linewise_selection || row > sel_start.row) ? 0 : sel_start.col;
            int ce = (linewise_selection || row < sel_end.row) ? line_len + 1 : sel_end.col + 1;
            float x0 = text_x + cs * g_char_width;
            float x1 = text_x + ce * g_char_width;
            Color sel_color = ResolveHlGroup("Visual");
            DrawRectangle(static_cast<int>(x0), static_cast<int>(ly), static_cast<int>(x1 - x0), line_height,
                          Color{sel_color.r, sel_color.g, sel_color.b, 160});
        }
        if (gutter_w > 0.0f) {
            std::string num = std::to_string(row + 1);
            float num_w = MeasureTextEx(g_font, num.c_str(), g_font_size, 0).x;
            DrawTextEx(g_font, num.c_str(), Vector2{text_x - g_char_width - num_w, ly}, g_font_size, 0, ResolveHlGroup("LineNr"));
        }

        // Decorations (Phase 4): whole-line tint first (background,
        // beneath the text), then the base line, then per-span highlight
        // recolor + virtual text on top (foreground, so it's visible over
        // the just-drawn text) -- and finally the gutter sign, if there's
        // room for one (only when :set number has already reserved gutter
        // space; an always-on sign column is a documented follow-up).
        char sign = 0;
        std::string sign_hl;
        int sign_priority = -1;
        for (const auto &ns_decos : buf.decorations) {
            for (const Decoration &d : ns_decos.second) {
                if (d.row != row) continue;
                if (d.whole_line && !d.hl_group.empty()) {
                    Color c = ResolveHlGroup(d.hl_group);
                    DrawRectangle(static_cast<int>(x), static_cast<int>(ly), static_cast<int>(w), line_height,
                                  Color{c.r, c.g, c.b, 40});
                }
                if (d.sign != 0 && d.priority > sign_priority) {
                    sign = d.sign;
                    sign_hl = d.sign_hl;
                    sign_priority = d.priority;
                }
            }
        }
        DrawLineFast(buf.lines[row], text_x, ly, g_font_size, ResolveHlGroup("Normal"));
        for (const auto &ns_decos : buf.decorations) {
            for (const Decoration &d : ns_decos.second) {
                if (d.row != row) continue;
                if (!d.whole_line && !d.hl_group.empty() && d.col_end > d.col_start) {
                    const std::string &line = buf.lines[row];
                    int a = std::min(static_cast<int>(line.size()), d.col_start);
                    int b = std::min(static_cast<int>(line.size()), d.col_end);
                    if (b > a) {
                        std::string span = line.substr(a, b - a);
                        DrawTextEx(g_font, span.c_str(), Vector2{text_x + a * g_char_width, ly}, g_font_size, 0,
                                   ResolveHlGroup(d.hl_group));
                    }
                }
                if (!d.virt_text.empty()) {
                    float vx = text_x + d.col_start * g_char_width;
                    if (d.virt_overlay) {
                        DrawRectangle(static_cast<int>(vx), static_cast<int>(ly),
                                      static_cast<int>(MeasureTextEx(g_font, d.virt_text.c_str(), g_font_size, 0).x),
                                      line_height, ResolveHlGroup("NormalBg"));
                    }
                    DrawTextEx(g_font, d.virt_text.c_str(), Vector2{vx, ly}, g_font_size, 0,
                               ResolveHlGroup(d.virt_text_hl));
                }
                // Colorizer swatch (Phase 13): a small filled square in the
                // literal parsed color, drawn just before col_start.
                if (d.has_swatch) {
                    float sx = text_x + d.col_start * g_char_width;
                    float sw = std::max(4.0f, g_char_width - 2);
                    DrawRectangle(static_cast<int>(sx), static_cast<int>(ly + (line_height - sw) / 2.0f),
                                  static_cast<int>(sw), static_cast<int>(sw),
                                  Color{d.swatch_color.r, d.swatch_color.g, d.swatch_color.b, 255});
                }
            }
        }
        if (sign != 0 && gutter_w > 0.0f) {
            std::string glyph(1, sign);
            DrawTextEx(g_font, glyph.c_str(), Vector2{x + kMarginX, ly}, g_font_size, 0, ResolveHlGroup(sign_hl));
        }
        // Hint labels (Phase 13): drawn last so they sit on top of
        // everything else on their row.
        if (is_active && g_editor.IsHintActive()) {
            for (const HintMatch &hm : g_editor.HintMatches()) {
                if (hm.row != row) continue;
                float hx = text_x + hm.col * g_char_width;
                float label_w = MeasureTextEx(g_font, hm.label.c_str(), g_font_size, 0).x + 4;
                DrawRectangle(static_cast<int>(hx), static_cast<int>(ly), static_cast<int>(label_w), line_height,
                              ResolveHlGroup("PickerSelected"));
                DrawTextEx(g_font, hm.label.c_str(), Vector2{hx + 2, ly}, g_font_size, 0, ResolveHlGroup("Warn"));
            }
        }
    }

    if (is_active && !IsCommandLineMode(g_editor.CurrentMode()) && pane.cursor.row >= pane.scroll_row &&
        pane.cursor.row < last_line) {
        // Buffer row -> visual slot, accounting for any closed folds
        // between the top of the view and the cursor (each collapses to
        // 1 slot regardless of how many rows it hides) -- the cursor
        // itself is never hidden inside one (see ClampCursor).
        int cursor_slot = 0;
        for (int r = pane.scroll_row; r < pane.cursor.row;) {
            const Fold *f = nullptr;
            for (const Fold &fold : buf.folds) {
                if (fold.closed && fold.start_row == r) f = &fold;
            }
            r = f ? f->end_row + 1 : r + 1;
            cursor_slot++;
        }
        float cursor_y = content_y + cursor_slot * line_height;
        float cursor_x = text_x + pane.cursor.col * g_char_width;
        if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
            if (g_editor.CurrentMode() == Mode::Insert) {
                DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(cursor_y), 2, static_cast<int>(g_font_size),
                              ResolveHlGroup("Normal"));
            } else {
                Color cursor_bg = ResolveHlGroup("Normal");
                DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(cursor_y), static_cast<int>(g_char_width),
                              line_height, Color{cursor_bg.r, cursor_bg.g, cursor_bg.b, 180});
                const std::string &line = buf.lines[pane.cursor.row];
                if (pane.cursor.col < static_cast<int>(line.size())) {
                    char ch[2] = {line[pane.cursor.col], '\0'};
                    DrawTextEx(g_font, ch, Vector2{cursor_x, cursor_y}, g_font_size, 0, ResolveHlGroup("NormalBg"));
                }
            }
        }
        // Completion popup (Phase 22): positioned just below the cursor,
        // not blink-gated (unlike the cursor glyph above) since a
        // flickering completion list would be actively distracting.
        if (g_editor.CurrentMode() == Mode::Insert && g_editor.IsCompletionOpen()) {
            DrawCompletionPopup(cursor_x, cursor_y + line_height);
        }
    }

    EndScissorMode();

    Color border_color = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
    DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                        border_color);
}

// Recursively lays out a tab's split tree: Horizontal/Vertical nodes divide
// their rectangle by SplitNode::shares (falling back to equal shares when
// unset) top-to-bottom or left-to-right; Leaf nodes render via DrawPane.
// Mirrors Editor::ComputeRects, which uses the same shares for nav geometry.
void DrawPaneTree(const SplitNode *node, float x, float y, float w, float h, int active_pane_id) {
    if (node->dir == SplitDir::Leaf) {
        DrawPane(node->pane, x, y, w, h, node->pane.id == active_pane_id);
        return;
    }
    int n = static_cast<int>(node->children.size());
    if (n == 0) return;
    bool has_shares = node->shares.size() == static_cast<size_t>(n);
    if (node->dir == SplitDir::Horizontal) {
        float cy = y;
        for (int i = 0; i < n; i++) {
            float ch = has_shares ? h * node->shares[i] : h / n;
            float next_y = (i == n - 1) ? y + h : cy + ch;
            DrawPaneTree(node->children[i].get(), x, cy, w, next_y - cy, active_pane_id);
            cy = next_y;
        }
    } else {
        float cx = x;
        for (int i = 0; i < n; i++) {
            float cw = has_shares ? w * node->shares[i] : w / n;
            float next_x = (i == n - 1) ? x + w : cx + cw;
            DrawPaneTree(node->children[i].get(), cx, y, next_x - cx, h, active_pane_id);
            cx = next_x;
        }
    }
}

// Shown only when there's more than one tab, matching Vim's tabline.
void DrawTabBar(int y) {
    int screen_w = GetScreenWidth();
    int bar_h = TabBarHeight();
    float font_size = MenuFontSize();
    DrawRectangle(0, y, screen_w, bar_h, ResolveHlGroup("TabBar"));

    float x = 4;
    for (int i = 0; i < g_editor.TabCount(); i++) {
        std::string label = " " + std::to_string(i + 1) + ": " + g_editor.TabLabel(i) + " ";
        float w = MeasureTextEx(g_font, label.c_str(), font_size, 0).x + 12;
        bool active = (i == g_editor.ActiveTabIndex());
        Color bg = active ? ResolveHlGroup("TabActive") : ResolveHlGroup("TabInactive");
        DrawRectangle(static_cast<int>(x), y + 2, static_cast<int>(w), bar_h - 4, bg);
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, y + (bar_h - font_size) / 2.0f}, font_size, 0, ResolveHlGroup("Normal"));
        x += w + 3;
    }
}

// Startup dashboard (Phase 12): shown in place of the pane tree only while
// Editor::ShouldShowDashboard() holds (single empty untouched buffer, one
// window) -- disappears the instant that stops being true, since this is
// just a per-frame check, not a one-shot flag to remember to clear.
void DrawDashboard(float x, float y, float w, float h) {
    std::vector<std::string> lines = SplitLines(kAboutText);
    lines.push_back("");
    lines.push_back("i to start typing  :e to open a file  <leader> for keys  :q to quit");
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 8;
    float max_w = 0;
    for (const auto &line : lines) max_w = std::max(max_w, MeasureTextEx(g_font, line.c_str(), font_size, 0).x);
    float box_h = static_cast<float>(lines.size() * line_h);
    float start_y = y + std::max(0.0f, (h - box_h) / 2.0f);
    for (size_t i = 0; i < lines.size(); i++) {
        float lw = MeasureTextEx(g_font, lines[i].c_str(), font_size, 0).x;
        float lx = x + std::max(0.0f, (w - lw) / 2.0f);
        DrawTextEx(g_font, lines[i].c_str(), Vector2{lx, start_y + i * line_h}, font_size, 0,
                   ResolveHlGroup("Comment"));
    }
}

void DrawEditor() {
    int screen_w = GetScreenWidth();
    int screen_h = GetScreenHeight();
    int line_height = LineHeight();
    bool zen = g_editor.IsZenMode();
    // Zen mode (Phase 12) hides chrome -- menu bar, tab bar, status line,
    // sidebars -- but not the command line (still needed to type `:` while
    // zen is on) or transient overlays/toasts (functionally necessary
    // regardless of chrome visibility).
    int status_bar_height = zen ? 0 : line_height;
    int command_bar_height = line_height;
    int menu_bar_height = zen ? 0 : MenuBarHeight();
    bool show_tabs = !zen && g_editor.TabCount() > 1;
    int tab_bar_height = show_tabs ? TabBarHeight() : 0;
    int content_top = menu_bar_height + tab_bar_height;
    int pane_area_h = screen_h - content_top - status_bar_height - command_bar_height;

    BeginDrawing();
    ClearBackground(ResolveHlGroup("NormalBg"));

    // Zen centers the pane tree with generous side padding when the screen
    // is wide enough to spare it -- pure cosmetic, doesn't touch the pane
    // tree's own geometry/state.
    float pane_x = 0.0f, pane_w = static_cast<float>(screen_w);
    if (zen) {
        float pad = std::max(0.0f, (screen_w - 900.0f) / 2.0f);
        pane_x = pad;
        pane_w = screen_w - 2 * pad;
    } else {
        // Reserve real screen space for open left/right sidebars (zen hides
        // sidebars entirely, so this only applies outside it) rather than
        // letting DrawSidebars' opaque panels just overlay pane content --
        // needed for sidebars to feel like actual neighboring panes now
        // that mod1+hjkl can focus/resize them. Sums every open sidebar on
        // each edge the same way DrawSidebars accumulates left_offset/
        // right_offset, so the two stay in agreement.
        float left_w = 0.0f, right_w = 0.0f;
        for (const SidebarInstance &sb : g_editor.Sidebars()) {
            if (!sb.open) continue;
            if (sb.position == "left") left_w += sb.size * g_char_width;
            else if (sb.position == "right") right_w += sb.size * g_char_width;
        }
        pane_x = left_w;
        pane_w = std::max(100.0f, screen_w - left_w - right_w);
    }

    if (pane_area_h > 0) {
        if (g_editor.ShouldShowDashboard()) {
            DrawDashboard(pane_x, static_cast<float>(content_top), pane_w, static_cast<float>(pane_area_h));
        } else {
            DrawPaneTree(g_editor.ActiveTabRoot(), pane_x, static_cast<float>(content_top), pane_w,
                         static_cast<float>(pane_area_h), g_editor.ActivePaneId());
        }
    }

    // Statusline: mode, filename, modified marker, cursor position (of the
    // active pane) -- or, if mep.set_statusline() registered a widget-list
    // callback (Phase 11), that instead: segments drawn left-to-right, each
    // through its own `hl` (falling back to "StatusLineFg"). Hidden
    // entirely in zen mode.
    if (!zen) {
        const Buffer &buf = g_editor.CurrentBuffer();
        CursorPos cursor = g_editor.Cursor();
        int status_y = screen_h - command_bar_height - status_bar_height;
        DrawRectangle(0, status_y, screen_w, status_bar_height, ResolveHlGroup("StatusLine"));
        float status_font_size = std::max(kMinFontSize, g_font_size - 2);
        std::vector<std::pair<std::string, std::string>> widgets;
        bool has_widgets = g_editor.Lua() && g_editor.Lua()->CallRefForWidgets(g_editor.StatuslineRef(), &widgets);
        if (has_widgets) {
            float wx = static_cast<float>(kMarginX);
            for (const auto &seg : widgets) {
                Color c = seg.second.empty() ? ResolveHlGroup("StatusLineFg") : ResolveHlGroup(seg.second);
                DrawTextEx(g_font, seg.first.c_str(), Vector2{wx, static_cast<float>(status_y + 3)}, status_font_size,
                           0, c);
                wx += MeasureTextEx(g_font, seg.first.c_str(), status_font_size, 0).x;
            }
        } else {
            std::string count_indicator =
                g_editor.PendingCount() > 0 ? std::to_string(g_editor.PendingCount()) + "  " : "";
            std::string register_indicator =
                g_editor.PendingRegister() != 0 ? std::string("\"") + g_editor.PendingRegister() + "  " : "";
            std::string buf_label = buf.scratch ? "[Scratch]" : (buf.filename.empty() ? "[No Name]" : buf.filename);
            std::string left = std::string("-- ") + ModeName(g_editor.CurrentMode(), g_editor.IsReplaceMode()) +
                                " --  " + register_indicator + count_indicator + buf_label +
                                (buf.modified ? " [+]" : "");
            std::string right = "Ln " + std::to_string(cursor.row + 1) + ", Col " + std::to_string(cursor.col + 1);
            DrawTextEx(g_font, left.c_str(), Vector2{static_cast<float>(kMarginX), static_cast<float>(status_y + 3)},
                       status_font_size, 0, ResolveHlGroup("StatusLineFg"));
            float right_w = MeasureTextEx(g_font, right.c_str(), status_font_size, 0).x;
            DrawTextEx(g_font, right.c_str(),
                       Vector2{static_cast<float>(screen_w - kMarginX) - right_w, static_cast<float>(status_y + 3)},
                       status_font_size, 0, ResolveHlGroup("StatusLineFg"));
        }
    }

    // Command line / last message.
    int cmd_y = screen_h - command_bar_height;
    DrawRectangle(0, cmd_y, screen_w, command_bar_height, ResolveHlGroup("NormalBg"));
    if (IsCommandLineMode(g_editor.CurrentMode())) {
        char prefix = ':';
        const std::string *text = &g_editor.CommandLine();
        if (g_editor.CurrentMode() == Mode::SearchForward) {
            prefix = '/';
            text = &g_editor.SearchQuery();
        } else if (g_editor.CurrentMode() == Mode::SearchBackward) {
            prefix = '?';
            text = &g_editor.SearchQuery();
        }
        std::string line = prefix + *text;
        DrawTextEx(g_font, line.c_str(), Vector2{static_cast<float>(kMarginX), static_cast<float>(cmd_y + 3)},
                   g_font_size, 0, ResolveHlGroup("Normal"));
        if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
            float cx = kMarginX + MeasureTextEx(g_font, line.c_str(), g_font_size, 0).x;
            DrawRectangle(static_cast<int>(cx), cmd_y + 3, 2, static_cast<int>(g_font_size), ResolveHlGroup("Normal"));
        }
        if (g_editor.CurrentMode() == Mode::Command && g_editor.IsCmdlineCompletionOpen()) {
            DrawCmdlineCompletionPopup(static_cast<float>(kMarginX), static_cast<float>(cmd_y));
        }
    } else if (!g_editor.StatusMessage().empty()) {
        DrawTextEx(g_font, g_editor.StatusMessage().c_str(),
                   Vector2{static_cast<float>(kMarginX), static_cast<float>(cmd_y + 3)}, g_font_size, 0, ResolveHlGroup("Normal"));
    }

    if (show_tabs) DrawTabBar(menu_bar_height);
    if (!zen) DrawMenuBar();
    if (g_editor.CurrentMode() == Mode::Prompt) DrawPromptOverlay();
    if (g_editor.CurrentMode() == Mode::Confirm) DrawConfirmOverlay();
    if (g_editor.CurrentMode() == Mode::Select) DrawSelectOverlay();
    if (g_editor.CurrentMode() == Mode::Picker) DrawPickerOverlay();
    if (g_editor.CurrentMode() == Mode::WhichKey) DrawWhichKeyOverlay();
    if (!zen) DrawSidebars();
    if (g_show_help_overlay) DrawHelpOverlay();
    DrawToastStack();

    EndDrawing();
}

#if defined(__EMSCRIPTEN__)
// :q on the last pane, :qa, :qa!, :wqa, etc. all end up here.
// emscripten_cancel_main_loop() below only stops *this build's own*
// requestAnimationFrame loop -- it has no way to reach out of the wasm
// sandbox to close the native webview window or end the Deno process
// hosting it. Left at that, the window stays open with nothing left
// driving its render/input loop: frozen, unresponsive, and the process
// never exits -- which is exactly what looks like mep "hanging" on
// :qa!. launcher/webview_worker.ts binds window.mepQuit (absent when
// mep.html is opened directly in a plain browser tab, e.g. build/web/
// without `just run` -- there's no native window to close there, so
// this is a deliberate no-op in that case).
EM_JS(void, mep_js_request_native_quit, (), {
    if (window.mepQuit) window.mepQuit();
});
#endif

void UpdateDrawFrame() {
    JobManager::Instance().PollAll();
    g_editor.PollTerminals();
    g_editor.PruneExpiredToasts(GetTime());
    g_editor.SetNow(GetTime());
    if (g_editor.Lua()) g_editor.Lua()->RunFrameHooks();
    HandleFontSizeShortcuts();
    bool menu_consumed = HandleMenuInput();
    if (!menu_consumed) g_editor.HandleInput();
    DrawEditor();
#if defined(__EMSCRIPTEN__)
    if (g_editor.ShouldQuit()) {
        mep_js_request_native_quit();
        emscripten_cancel_main_loop();
    }
#endif
}

#if !defined(__EMSCRIPTEN__)
// Native-only: real files and a user config are meaningful here; neither
// is under Emscripten, which has no persistent/host filesystem.
std::string ConfigFilePath() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/mep/init.lua";
    const char *home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/mep/init.lua";
    return "";
}
#endif

}  // namespace

// g_char_width itself lives in the anonymous namespace above (internal
// linkage), so lua_env.cpp -- which needs it for mep.sidebar_default_cols --
// reaches it through this externally-linked accessor instead.
float GetCharWidthPx() { return g_char_width; }

int main(int argc, char **argv) {
    // Re-applies the default theme, redundantly with Editor::Editor()'s own
    // call: g_editor (this TU) and the palette table (editor.cpp's TU) are
    // separate translation units, and C++ doesn't guarantee global
    // constructor order *across* TUs -- if g_editor's constructor happened
    // to run before editor.cpp's palette globals were initialized, that
    // first ApplyTheme("mep-dark") silently found no palettes and left
    // current_theme_groups_ empty (ClearBackground/ResolveHlGroup then fell
    // back to a single hardcoded gray for everything). main() itself is
    // only ever reached after *all* global constructors everywhere have
    // run, so this call is always safe.
    g_editor.ApplyTheme("mep-dark");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(kInitialWidth, kInitialHeight, "mep");
    // raylib maps Escape to "close the window" by default; Escape is also
    // our Insert/Visual/Command -> Normal key, so that default must go.
    SetExitKey(KEY_NULL);

#if !defined(__EMSCRIPTEN__)
    // The web build fills whatever size the browser/webview gives its
    // canvas (see web/shell.html); there's no OS window to maximize.
    MaximizeWindow();
#endif

    ApplyFontSize(kDefaultFontSize);
    BuildMenus();
    RecomputeMenuLabelLayout();

    // Heap-allocated and deliberately never freed: on the wasm build, main()
    // itself returns right after emscripten_set_main_loop below (see the
    // comment there), which would destroy a stack-local LuaEnv while
    // g_editor still held a pointer to it -- every Lua call after that
    // point (anything but this function's own use of it, right here, still
    // within main()) would then run on a dangling LuaEnv/lua_State,
    // corrupting memory. Confirmed exactly this: the one-time DoString call
    // below always worked, but *any* later Lua call -- a mod1 mapping, a
    // bare `:lua 1+1`, anything -- reproducibly crashed with a wasm-level
    // out-of-bounds/segfault trap, on native code paths that don't even
    // reach any of mep's own registered Lua functions. Native never hit
    // this since its main() only returns after the whole event loop ends.
    LuaEnv *lua = new LuaEnv(&g_editor);
    g_editor.SetLuaEnv(lua);
    // Runs before any user config, so init.lua's own mep.map_mod1() calls
    // (same key) simply overwrite these. Not filesystem-dependent, so this
    // runs the same way on both native and wasm builds.
    lua->DoString(kDefaultMod1Bindings);
    lua->DoString(kBuiltinEditHooks);
    lua->DoString(kBuiltinIcons);
    lua->DoString(kBuiltinPickerSources);
    lua->DoString(kBuiltinTextTools);
    lua->DoString(kBuiltinFileTree);
    lua->DoString(kBuiltinGit);
    lua->DoString(kBuiltinTodo);
    lua->DoString(kBuiltinLsp);
    lua->DoString(kBuiltinCompletion);
    lua->DoString(kBuiltinSnippets);
    lua->DoString(kBuiltinSymbols);
    lua->DoString(kBuiltinDocs);
    lua->DoString(kBuiltinDap);
    lua->DoString(kBuiltinSyntax);
    lua->DoString(kBuiltinRun);
    lua->DoString(kBuiltinMarkdown);
    lua->DoString(kBuiltinOrg);
    lua->DoString(kBuiltinOrgLinks);
    lua->DoString(kBuiltinOrgCapture);
    lua->DoString(kBuiltinOrgAgenda);
    lua->DoString(kBuiltinOrgClock);
    lua->DoString(kBuiltinOrgBabel);
    lua->DoString(kBuiltinOrgExport);
    lua->DoString(kBuiltinOrgRoam);
    lua->DoString(kBuiltinOrgDrill);
    lua->DoString(kBuiltinOrgBib);
    lua->DoString(kBuiltinActivityBar);
    lua->DoString(kBuiltinAi);
    lua->DoString(kBuiltinLeetcode);

#if !defined(__EMSCRIPTEN__)
    if (argc > 1) {
        g_editor.LoadFile(argv[1]);
    }
    std::string config_path = ConfigFilePath();
    if (!config_path.empty()) {
        FILE *f = std::fopen(config_path.c_str(), "rb");
        if (f) {
            std::fclose(f);
            lua->DoFile(config_path);
        }
    }
#endif

#if defined(__EMSCRIPTEN__)
    // simulate_infinite_loop=0: with =1, Emscripten unwinds the C call
    // stack up to main() via its own JS-exception-based mechanism to fake
    // "never returning" -- which doesn't coexist reliably with Asyncify's
    // own unwind/rewind (used by the EM_ASYNC_JS file-bridge calls in
    // editor.cpp): the async call resolves and the file write completes,
    // but control never finds its way back into the suspended frame, so
    // the app hangs forever on the first :e/:w/:source. With =0, main()
    // is allowed to actually return after registering the loop, which
    // Asyncify handles correctly.
    emscripten_set_main_loop(UpdateDrawFrame, 0, 0);
    return 0;
#else
    SetTargetFPS(60);
    while (!WindowShouldClose() && !g_editor.ShouldQuit()) {
        UpdateDrawFrame();
    }
#endif

    UnloadFont(g_font);
    CloseWindow();
    return 0;
}
