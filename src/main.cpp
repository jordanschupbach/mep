#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
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
#include "image_doc.h"
#include "pdf_doc.h"
#include "office_doc.h"
#include "office_font_data.h"

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

// The 4 Liberation Sans weight/style variants for WYSIWYG office panes
// (Editor::OfficeSession) -- unlike g_font, these are loaded once at a
// fixed oversampled base size and never reloaded, since office text is
// drawn at many different sizes in the same frame (body text, several
// heading levels, per-session zoom) rather than one global size the way
// the main buffer's g_font_size is; raylib's DrawTextEx scales a baked
// atlas to any requested size, just like drawing g_font at other sizes
// already does elsewhere (e.g. MenuFontSize()).
Font g_office_font_regular;
Font g_office_font_bold;
Font g_office_font_italic;
Font g_office_font_bolditalic;

// GPU upload cache for image-viewer panes (Editor::ImageSession), keyed by
// buffer_id -- an ImageDoc is decoded once and never mutated, so its
// Texture2D is uploaded lazily on first draw and reused every frame after,
// same lifetime as buffers_ itself (never evicted; see the comment above
// Editor::images_).
std::unordered_map<int, Texture2D> g_image_textures;

// GPU upload cache for PDF-viewer panes (Editor::PdfSession), keyed by
// (buffer_id, page index) -- PdfSession virtualizes its raster cache down
// to {page-1, page, page+1} (see Editor::EnsurePdfPagesRastered), and this
// mirrors that: only a handful of entries ever exist per open PDF
// regardless of document length. Unlike g_image_textures, a page's raster
// *does* change (rescale re-renders it, or it can be evicted and later
// re-entered at a different generation), so each entry also tracks the
// PageRaster::generation, PdfSession::theme_colors, and Editor::ThemeEpoch()
// it was last uploaded from -- GetOrUpdatePdfPageTexture reuploads whenever
// any of the three has moved on. theme_epoch is the one that matters most
// in practice: theme_colors/generation alone missed the case of the active
// *theme itself* changing (e.g. live-previewing a colorscheme via the theme
// picker) while a page stayed inside the small rendered-page window the
// whole time -- that page's raster generation never changed and
// theme_colors never flipped, so without theme_epoch its stale texture
// would keep showing the old theme's colors until it happened to scroll
// out of the window and back. See ThemeEpoch()'s own comment in editor.h.
struct PdfTextureCacheEntry {
    Texture2D tex{};
    int generation = -1;
    bool theme_colors = false;
    int theme_epoch = -1;
    int w = 0, h = 0;
};
std::map<std::pair<int, int>, PdfTextureCacheEntry> g_pdf_page_textures;

// PDF "theme colors" mode (default on; Ctrl-R in HandlePdfInput toggles
// PdfSession::theme_colors): recolors a rendered page to match the
// editor's own color scheme instead of showing white paper with black
// text. Maps each pixel's luminance to a point on the gradient between the
// editor's foreground and background colors -- PDF white background ->
// editor background, PDF black text -> editor foreground, grays
// interpolate smoothly. This deliberately desaturates colored content
// (headings, images, diagrams) the same way most e-reader "night mode"
// implementations do; it's a text-reading aid, not a color-accurate
// filter.
unsigned char ThemedPdfChannel(unsigned char fg, unsigned char bg, float luminance) {
    return static_cast<unsigned char>(fg + (static_cast<float>(bg) - static_cast<float>(fg)) * luminance);
}

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

// Generic click-region registry (NVIM_PARITY_PLAN.md Phase 11's "generic
// click dispatch on widgets" gap): rebuilt fresh every frame by whichever
// DrawXxx functions render a clickable region (tab bar, gutter fold
// markers, pane header breadcrumb) alongside their normal drawing, then
// consumed once by DispatchChromeClicks() right after DrawEditor() -- one
// frame of lag between "drawn here" and "clickable here" (identical to the
// existing g_menu_starts/g_menu_widths cache above), imperceptible at
// interactive framerates. Deliberately *not* a generic Lua-facing "widget"
// abstraction (unlike the statusline's {text,hl} schema) -- this wires real
// clicks onto the existing hardcoded tab/gutter/pane-header rendering
// rather than rebuilding them on a new customizable widget model, which
// NVIM_PARITY_PLAN.md documents as a separate, larger, still-deferred
// refactor.
struct ClickRegion {
    Rectangle rect;
    std::function<void()> action;
};
std::vector<ClickRegion> g_click_regions;

void RegisterClickRegion(Rectangle rect, std::function<void()> action) {
    g_click_regions.push_back({rect, std::move(action)});
}

bool PointInRect(Vector2 p, const Rectangle &r) {
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}

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
        case Mode::Preview: return "PREVIEW";
        case Mode::Sidebar: return "SIDEBAR";
        case Mode::Picker: return "PICKER";
        case Mode::RoamGraph: return "ROAM-GRAPH";
        case Mode::WhichKey: return "WHICHKEY";
        case Mode::HintChar:
        case Mode::HintLabel:
            return "HINT";
        case Mode::Terminal: return "TERMINAL";
        case Mode::Image: return "IMAGE";
        case Mode::Pdf: return "PDF";
        case Mode::OfficeNormal: return "NORMAL";
        case Mode::OfficeInsert: return "INSERT";
        case Mode::OfficeVisual: return "VISUAL";
        case Mode::SheetNormal: return "NORMAL";
        case Mode::SheetInsert: return "INSERT";
        case Mode::SheetVisual: return "VISUAL";
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

// Bakes the 4 office-pane fonts once, at startup only (see g_office_font_*'s
// own comment for why -- no ApplyFontSize-style reload-per-size-change).
// Baked at a fixed oversampled size (kOfficeFontBasePt * 2, mirroring
// ApplyFontSize's own 2x-supersample convention) large enough to cover the
// biggest heading size any session's zoom is likely to reach without
// visibly blurring when DrawTextEx scales the atlas down.
void LoadOfficeFonts() {
    constexpr int kOfficeFontBasePt = 48;
    // Default ASCII range (32..126, matching raylib's own nullptr-codepoints
    // default) plus U+2022 BULLET -- office_doc's bullet-list rendering
    // draws that codepoint directly, which isn't in the ASCII range and
    // would otherwise fall back to Liberation Sans's "missing glyph" box.
    static int codepoints[96];
    for (int c = 32; c <= 126; c++) codepoints[c - 32] = c;
    codepoints[95] = 0x2022;
    g_office_font_regular = LoadFontFromMemory(".ttf", kLiberationSansRegularTtf,
                                                static_cast<int>(kLiberationSansRegularTtfLen),
                                                kOfficeFontBasePt * 2, codepoints, 96);
    g_office_font_bold = LoadFontFromMemory(".ttf", kLiberationSansBoldTtf,
                                             static_cast<int>(kLiberationSansBoldTtfLen),
                                             kOfficeFontBasePt * 2, codepoints, 96);
    g_office_font_italic = LoadFontFromMemory(".ttf", kLiberationSansItalicTtf,
                                               static_cast<int>(kLiberationSansItalicTtfLen),
                                               kOfficeFontBasePt * 2, codepoints, 96);
    g_office_font_bolditalic = LoadFontFromMemory(".ttf", kLiberationSansBoldItalicTtf,
                                                   static_cast<int>(kLiberationSansBoldItalicTtfLen),
                                                   kOfficeFontBasePt * 2, codepoints, 96);
    SetTextureFilter(g_office_font_regular.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_office_font_bold.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_office_font_italic.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_office_font_bolditalic.texture, TEXTURE_FILTER_BILINEAR);
}

// Which of the 4 baked fonts a run of text with this format draws with --
// strike-through has no dedicated glyph variant, drawn as an overlay line
// instead (see the office DrawPane branch), so it doesn't affect font
// selection.
Font &OfficeFontFor(const DocFormat &fmt) {
    if (fmt.bold && fmt.italic) return g_office_font_bolditalic;
    if (fmt.bold) return g_office_font_bold;
    if (fmt.italic) return g_office_font_italic;
    return g_office_font_regular;
}

// Body-text-relative size multiplier for a paragraph's heading level (0 =
// body). v1 simplification: a handful of fixed ratios, not a real style
// cascade (see office_doc.h's own exclusion list).
float OfficeHeadingMultiplier(int heading_level) {
    switch (heading_level) {
        case 1: return 1.8f;
        case 2: return 1.5f;
        case 3: return 1.3f;
        case 4: return 1.15f;
        default: return heading_level > 0 ? 1.1f : 1.0f;
    }
}

// One word-wrapped visual line of a paragraph: [start,end) byte range into
// DocParagraph::text (half-open, like DocSpan). Every paragraph's lines
// are contiguous and cover the whole text (a wrapped-away trailing space
// is simply excluded from both the line that dropped it and the next
// line's start, see the tokenizer loop below) -- contiguity is what lets
// cursor<->visual-line mapping stay exact.
struct OfficeWrapLine {
    int start = 0, end = 0;
};

// Greedy word-wrap: tokenize into maximal non-whitespace runs ("words")
// and single whitespace characters (space/tab/newline), then pack tokens
// onto lines against max_width -- literally the algorithm the design plan
// specifies ("split on spaces, MeasureTextEx per word"), not a
// codepoint-precise typesetting engine. A word longer than max_width on
// its own is never itself split mid-word (an accepted v1 gap: it simply
// overflows the pane's right edge). Font/size selection for a token uses
// FormatAt at the token's start -- spans from both parsers already align
// to word boundaries in practice, so a token straddling a format change
// is not a case real documents hit.
std::vector<OfficeWrapLine> WordWrapOfficeParagraph(const DocParagraph &p, float max_width, float font_size) {
    std::vector<OfficeWrapLine> lines;
    const std::string &text = p.text;
    int n = static_cast<int>(text.size());
    if (max_width < 1.0f) max_width = 1.0f;
    if (n == 0) {
        lines.push_back({0, 0});
        return lines;
    }
    auto token_width = [&](int s, int e) -> float {
        if (e == s + 1 && text[s] == '\t') {
            Font &f = OfficeFontFor(FormatAt(p, s));
            return MeasureTextEx(f, " ", font_size, 0).x * 4.0f;
        }
        Font &f = OfficeFontFor(FormatAt(p, s));
        std::string tok = text.substr(s, e - s);
        return MeasureTextEx(f, tok.c_str(), font_size, 0).x;
    };
    int line_start = 0;
    float cur_width = 0.0f;
    int i = 0;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            lines.push_back({line_start, i});
            line_start = i + 1;
            cur_width = 0.0f;
            i++;
            continue;
        }
        int tok_end;
        if (c == ' ' || c == '\t') {
            tok_end = i + 1;
        } else {
            tok_end = i;
            while (tok_end < n) {
                unsigned char tc = static_cast<unsigned char>(text[tok_end]);
                if (tc == ' ' || tc == '\t' || tc == '\n') break;
                tok_end++;
            }
        }
        float w = token_width(i, tok_end);
        bool is_ws = (c == ' ' || c == '\t');
        if (cur_width + w > max_width && i > line_start) {
            if (is_ws) {
                // Drops the whitespace token that would have overflowed --
                // it's simply consumed, not carried to the next line
                // (matches the ordinary "trailing space vanishes at a
                // wrap point" convention).
                lines.push_back({line_start, i});
                line_start = tok_end;
                cur_width = 0.0f;
                i = tok_end;
                continue;
            }
            lines.push_back({line_start, i});
            line_start = i;
            cur_width = 0.0f;
        }
        cur_width += w;
        i = tok_end;
    }
    lines.push_back({line_start, n});
    return lines;
}

// One contiguous [start,end) run within a paragraph sharing one DocFormat
// -- the renderer's unit of a single DrawTextEx call, built by walking
// p.spans (already sorted/non-overlapping, see office_doc.h) and filling
// the gaps between them with a default-format run.
struct OfficeFormatRun {
    int start = 0, end = 0;
    DocFormat fmt;
};

std::vector<OfficeFormatRun> BuildOfficeFormatRuns(const DocParagraph &p, int a, int b) {
    std::vector<OfficeFormatRun> runs;
    int pos = a;
    for (const DocSpan &sp : p.spans) {
        if (sp.end <= a) continue;
        if (sp.start >= b) break;
        int s = std::max(sp.start, a);
        int e = std::min(sp.end, b);
        if (s > pos) runs.push_back({pos, s, DocFormat{}});
        if (e > s) runs.push_back({s, e, sp.fmt});
        pos = std::max(pos, e);
    }
    if (pos < b) runs.push_back({pos, b, DocFormat{}});
    return runs;
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
    // Configurable diff base (Phase 17 gap): a single global ref name,
    // not per-buffer -- same-global convention as mep_git_hunks/
    // mep_git_base_lines above, and simpler for the common case of
    // reviewing one buffer's history against a moving point (a branch,
    // HEAD~1, a SHA) rather than pinning a base per file. Overridable
    // with `:MepGitGutter base <ref>`; `:MepGitGutter base` with no ref
    // reports the current one. Every `git show <ref>:<file>` shell-out
    // in this module reads this instead of a hardcoded 'HEAD'.
    "mep.git_gutter_base = 'HEAD'\n"
    "function mep.git_gutter_refresh()\n"
    "  local fname = mep.filename()\n"
    "  if fname == '' then return end\n"
    "  if not mep_git_ns then mep_git_ns = mep.ns_create('git') end\n"
    "  local lines = {}\n"
    "  mep.job_start({'git', 'show', mep.git_gutter_base .. ':' .. fname}, {\n"
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
    // Preview hunk (Phase 17 gap): shows the hunk under the cursor as a
    // unified-diff body (old lines '-', new lines '+') in a dismiss-on-
    // any-key float (mep.float_preview -> Editor::BeginPreview,
    // DrawPreviewOverlay in main.cpp) *before* mep.git_stage_hunk/
    // git_reset_hunk act on it -- same hunk-at-cursor lookup and the
    // same mep_git_base_lines those two already use, just rendered
    // instead of applied.
    "function mep.git_preview_hunk()\n"
    "  local h = mep_git_hunk_at_cursor()\n"
    "  if not h then mep.notify('No hunk under cursor', 'warn') return end\n"
    "  local lines = {}\n"
    "  for i = h.old_start, h.old_start + h.old_count - 1 do lines[#lines + 1] = '-' .. (mep_git_base_lines[i] or '') end\n"
    "  for i = h.new_start, h.new_start + h.new_count - 1 do lines[#lines + 1] = '+' .. mep.get_line(i) end\n"
    "  if #lines == 0 then lines[1] = '(empty hunk)' end\n"
    "  mep.float_preview('Hunk preview  (base: ' .. mep.git_gutter_base .. ')', table.concat(lines, '\\n'))\n"
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
    // `:MepGitGutter` alone just recomputes against the current base
    // (unchanged default behavior); `:MepGitGutter base <ref>` (Phase
    // 17 gap) repoints mep.git_gutter_base at any git revision -- a
    // branch, a SHA, HEAD~1, etc. -- and immediately recomputes against
    // it; `:MepGitGutter base` with no ref just reports the current
    // one. mep.command() callbacks now receive the rest of the command
    // line verbatim (Editor::ExecuteCommandLine's lua_commands_ lookup),
    // so one registration covers both forms instead of needing a
    // separate :MepGitGutterBase command.
    "function mep.git_gutter_command(args)\n"
    "  local sub, rest = (args or ''):match('^(%S*)%s*(.*)$')\n"
    "  if sub == 'base' then\n"
    "    local ref = rest:match('^%s*(.-)%s*$')\n"
    "    if ref == '' then\n"
    "      mep.notify('Git diff base: ' .. mep.git_gutter_base)\n"
    "    else\n"
    "      mep.git_gutter_base = ref\n"
    "      mep.notify('Git diff base set to ' .. ref)\n"
    "      mep.git_gutter_refresh()\n"
    "    end\n"
    "  else\n"
    "    mep.git_gutter_refresh()\n"
    "  end\n"
    "end\n"
    "mep.command('MepGitGutter', mep.git_gutter_command)\n"
    "mep.command('MepGitPreviewHunk', mep.git_preview_hunk)\n"
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
    // Per-keyword sign glyph + highlight group, keyed by keyword text
    // (case-sensitive, matching MEP_TODO_KEYWORDS' own case) -- same flat
    // config-table shape as mep.syntax_keywords/mep.syntax_comment_prefix.
    // Override an existing entry or add a new one at runtime, e.g.
    // mep.todoscan_keywords.XXX = {glyph = 'X', hl = 'Purple'} (remember to
    // also add 'XXX' to MEP_TODO_KEYWORDS above so it's actually scanned
    // for). A keyword missing from this table -- or a config missing one
    // of the two fields -- falls back to MEP_TODO_DEFAULT below field by
    // field, so nothing breaks for keywords the user hasn't customized.
    "mep.todoscan_keywords = {\n"
    "  TODO  = {glyph = 'T', hl = 'Yellow'},\n"
    "  FIXME = {glyph = 'F', hl = 'Red'},\n"
    "  HACK  = {glyph = 'H', hl = 'Orange'},\n"
    "  NOTE  = {glyph = 'N', hl = 'Blue'},\n"
    "}\n"
    "local MEP_TODO_DEFAULT = {glyph = 'T', hl = 'Warn'}\n"
    "local mep_todo_ns = nil\n"
    "function mep.todo_mark_buffer()\n"
    "  if not mep_todo_ns then mep_todo_ns = mep.ns_create('todoscan') end\n"
    "  mep.ns_clear(mep_todo_ns)\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    for _, kw in ipairs(MEP_TODO_KEYWORDS) do\n"
    "      local s = line:find(kw, 1, true)\n"
    "      if s then\n"
    "        local cfg = mep.todoscan_keywords[kw] or MEP_TODO_DEFAULT\n"
    "        local glyph = cfg.glyph or MEP_TODO_DEFAULT.glyph\n"
    "        local hl = cfg.hl or MEP_TODO_DEFAULT.hl\n"
    "        mep.deco_add(mep_todo_ns, {row = i, col_start = s, col_end = s + #kw, hl_group = hl, sign = glyph, sign_hl = hl})\n"
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
    // NVIM_PARITY_PLAN.md's own "LSP server registry ... is just data, add
    // entries as needed" note (mep.nvim's lua/mep/lsp/servers.lua has 35).
    // Grown here from the original 3 (lua/clangd/pyright) using the exact
    // same {cmd, filetypes} shape -- real command names and root-relevant
    // filetypes for each server, matching how each project actually
    // invokes it over stdio. Kept to one canonical server per language to
    // avoid two entries racing to claim the same filetype (see
    // mep.lsp_attach's filetypes-list fallback below); `basedpyright` is
    // included as a registered *alternative* to `pyright` with no
    // filetypes claim of its own for that reason -- select it explicitly
    // via `mep.lsp_servers.basedpyright.cmd` if preferred over `pyright`.
    "  gopls = {cmd = {'gopls'}, filetypes = {'go'}},\n"
    "  rust_analyzer = {cmd = {'rust-analyzer'}, filetypes = {'rs'}},\n"
    "  typescript_language_server = {cmd = {'typescript-language-server', '--stdio'},\n"
    "    filetypes = {'ts', 'tsx', 'js', 'jsx', 'mjs', 'cjs'}},\n"
    "  jdtls = {cmd = {'jdtls'}, filetypes = {'java'}},\n"
    "  solargraph = {cmd = {'solargraph', 'stdio'}, filetypes = {'rb'}},\n"
    "  intelephense = {cmd = {'intelephense', '--stdio'}, filetypes = {'php'}},\n"
    "  omnisharp = {cmd = {'omnisharp', '-lsp'}, filetypes = {'cs'}},\n"
    "  hls = {cmd = {'haskell-language-server-wrapper', '--lsp'}, filetypes = {'hs'}},\n"
    "  ocamllsp = {cmd = {'ocamllsp'}, filetypes = {'ml', 'mli'}},\n"
    "  zls = {cmd = {'zls'}, filetypes = {'zig'}},\n"
    "  elixirls = {cmd = {'elixir-ls'}, filetypes = {'ex', 'exs'}},\n"
    "  bashls = {cmd = {'bash-language-server', 'start'}, filetypes = {'sh', 'bash'}},\n"
    "  yamlls = {cmd = {'yaml-language-server', '--stdio'}, filetypes = {'yaml', 'yml'}},\n"
    "  jsonls = {cmd = {'vscode-json-language-server', '--stdio'}, filetypes = {'json', 'jsonc'}},\n"
    "  html = {cmd = {'vscode-html-language-server', '--stdio'}, filetypes = {'html', 'htm'}},\n"
    "  cssls = {cmd = {'vscode-css-language-server', '--stdio'}, filetypes = {'css', 'scss', 'less'}},\n"
    "  dockerls = {cmd = {'docker-langserver', '--stdio'}, filetypes = {'dockerfile'}},\n"
    "  terraformls = {cmd = {'terraform-ls', 'serve'}, filetypes = {'tf', 'tfvars'}},\n"
    "  marksman = {cmd = {'marksman', 'server'}, filetypes = {'md', 'markdown'}},\n"
    "  taplo = {cmd = {'taplo', 'lsp', 'stdio'}, filetypes = {'toml'}},\n"
    "  vimls = {cmd = {'vim-language-server', '--stdio'}, filetypes = {'vim'}},\n"
    "  clojure_lsp = {cmd = {'clojure-lsp'}, filetypes = {'clj', 'cljs', 'cljc'}},\n"
    "  kotlin_language_server = {cmd = {'kotlin-language-server'}, filetypes = {'kt', 'kts'}},\n"
    "  svelte = {cmd = {'svelteserver', '--stdio'}, filetypes = {'svelte'}},\n"
    "  basedpyright = {cmd = {'basedpyright-langserver', '--stdio'}, filetypes = {}},\n"
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
    // Direct key lookup first (the fast, pre-existing path -- also what
    // keeps `mep.lsp_servers.lua`'s key doubling as its own filetype
    // working unchanged). Real bug found while growing this registry:
    // most entries are keyed by *server name* (`clangd`, `pyright`, ...),
    // not by filetype, so a direct `mep.lsp_servers[ft]` lookup silently
    // never matched them -- every server but `lua` was unreachable from
    // `mep.lsp_attach` even though its `filetypes` field was right there.
    // Falls back to scanning every entry's `filetypes` list, which is the
    // field the registry always documented as the real dispatch key.
    "  local server = mep.lsp_servers[ft]\n"
    "  if not server then\n"
    "    for _, s in pairs(mep.lsp_servers) do\n"
    "      for _, ft2 in ipairs(s.filetypes or {}) do\n"
    "        if ft2 == ft then server = s break end\n"
    "      end\n"
    "      if server then break end\n"
    "    end\n"
    "  end\n"
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
    "        implementation = {},\n"
    "        typeDefinition = {},\n"
    "        signatureHelp = {signatureInformation = {documentationFormat = {'plaintext'}}},\n"
    "        rename = {},\n"
    "        codeAction = {},\n"
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
    "    mep.hover_show('Hover', text)\n"
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
    // Word under the cursor -- rename's own default-prefill (mep.ui_input's
    // second arg), no existing helper for this anywhere else in the
    // codebase to reuse.
    "function mep_lsp_word_at_cursor()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local s, e = col, col\n"
    "  while s > 1 and line:sub(s - 1, s - 1):match('[%w_]') do s = s - 1 end\n"
    "  while e <= #line and line:sub(e, e):match('[%w_]') do e = e + 1 end\n"
    "  if s >= e then return nil end\n"
    "  return line:sub(s, e - 1)\n"
    "end\n"
    // Shared by the two new goto-* requests below: same request/response
    // shape as mep.lsp_goto_definition above (Location | Location[]), just
    // a different method name and not-found message -- factored out here
    // rather than duplicated a third/fourth time (mep.lsp_goto_definition
    // itself is left as its own pre-existing function, untouched).
    "function mep_lsp_goto(method, not_found_msg)\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, method, {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    local loc = result and (result.uri and result or result[1])\n"
    "    if not loc then mep.notify(not_found_msg) return end\n"
    "    local f = loc.uri:gsub('^file://', '')\n"
    "    mep.cmd('e ' .. f)\n"
    "    mep.set_cursor(loc.range.start.line + 1, loc.range.start.character + 1)\n"
    "  end)\n"
    "end\n"
    "function mep.lsp_goto_implementation()\n"
    "  mep_lsp_goto('textDocument/implementation', 'No implementation found')\n"
    "end\n"
    "function mep.lsp_goto_type_definition()\n"
    "  mep_lsp_goto('textDocument/typeDefinition', 'No type definition found')\n"
    "end\n"
    "function mep.lsp_signature_help()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  mep.lsp_request(id, 'textDocument/signatureHelp', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    local sigs = result and result.signatures\n"
    "    if not sigs or #sigs == 0 then return end\n"
    "    local active = sigs[(result.activeSignature or 0) + 1] or sigs[1]\n"
    "    local text = active.label or ''\n"
    "    local doc = active.documentation\n"
    "    if doc then\n"
    "      local doctext = type(doc) == 'table' and doc.value or tostring(doc)\n"
    "      if doctext and doctext ~= '' then text = text .. '\\n' .. doctext end\n"
    "    end\n"
    // mep.hover_show, not mep.notify: a real anchored floating popup now
    // exists (added for mep.lsp_hover above) and auto-dismisses on cursor
    // move/mode change, which is exactly the right lifetime for signature
    // help too -- reused as-is rather than inventing a second widget.
    "    mep.hover_show('Signature Help', text)\n"
    "  end)\n"
    "end\n"
    // Opt-in-by-default auto-trigger while typing inside a call's argument
    // list -- unlike hover's other consumers, signature help is only
    // useful *during* typing, not summoned after the fact, so this
    // defaults on (mep.lsp_signature_help_auto = true) unlike every other
    // '_auto' flag elsewhere in this file, which default off. Trigger
    // heuristic is a cheap same-line scan for an unmatched '(' before the
    // cursor -- the same "regex/same-line-scan, not a real parser"
    // tradeoff Phase 25's own doc-gen fallback already made -- riding
    // kBuiltinEditHooks' existing debounced mep.on_buffer_changed rather
    // than a new per-keystroke hook.
    "mep.lsp_signature_help_auto = true\n"
    "local function mep_lsp_in_call_args()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row):sub(1, col - 1)\n"
    "  local depth = 0\n"
    "  for i = #line, 1, -1 do\n"
    "    local c = line:sub(i, i)\n"
    "    if c == ')' then depth = depth + 1\n"
    "    elseif c == '(' then\n"
    "      if depth == 0 then return true end\n"
    "      depth = depth - 1\n"
    "    end\n"
    "  end\n"
    "  return false\n"
    "end\n"
    "mep.on_buffer_changed(function()\n"
    "  if mep.lsp_signature_help_auto and mep.lsp_client_for() and mep_lsp_in_call_args() then\n"
    "    mep.lsp_signature_help()\n"
    "  end\n"
    "end, 0.2)\n"
    // General TextEdit application, character-range-aware -- unlike
    // mep.lsp_format's own line-based apply above (which only works
    // because formatting edits happen to already be whole-line/whole-
    // file). Rename/code-action edits are typically just a few characters
    // mid-line, so reusing replace_lines the way mep.lsp_format does would
    // clobber the rest of the line; this splices newText between the
    // edit's start/end *character* offsets instead. Splits newText on
    // '\n' with an explicit pos-cursor loop rather than mep.lsp_format's
    // own `(e.newText..'\\n'):gmatch('(.-)\\n')` + "drop a trailing empty
    // element" trick -- that trick silently eats a genuine trailing
    // newline (e.g. newText = "foo\\n", a whole-new-line insertion) by
    // merging it back into the following line, which mep.lsp_format never
    // notices only because its own edits happen to never end in '\\n'
    // followed by more content. Caught by hand-tracing this function
    // against a code-action edit that inserts "marker\\n" at column 0
    // during verification (see report) -- a real bug, fixed before ever
    // reaching the live test.
    "function mep_lsp_apply_text_edit(e)\n"
    "  local sl, sc = e.range.start.line + 1, e.range.start.character + 1\n"
    "  local el, ec = e.range['end'].line + 1, e.range['end'].character + 1\n"
    "  local new_lines = {}\n"
    "  local text, pos = e.newText, 1\n"
    "  while true do\n"
    "    local nl = text:find('\\n', pos, true)\n"
    "    if not nl then new_lines[#new_lines + 1] = text:sub(pos) break end\n"
    "    new_lines[#new_lines + 1] = text:sub(pos, nl - 1)\n"
    "    pos = nl + 1\n"
    "  end\n"
    "  local first_line = mep.get_line(sl)\n"
    "  local last_line = (el == sl) and first_line or mep.get_line(el)\n"
    "  local prefix = first_line:sub(1, sc - 1)\n"
    "  local suffix = last_line:sub(ec)\n"
    "  new_lines[1] = prefix .. new_lines[1]\n"
    "  new_lines[#new_lines] = new_lines[#new_lines] .. suffix\n"
    "  mep.replace_lines(sl, el + 1, new_lines)\n"
    "end\n"
    // Applies a batch of TextEdits to the *currently open* buffer, bottom-
    // up (descending start position) so an earlier-applied edit's
    // line/column shift never invalidates a later one still queued.
    "function mep_lsp_apply_edits_current_buffer(edits)\n"
    "  table.sort(edits, function(a, b)\n"
    "    if a.range.start.line ~= b.range.start.line then return a.range.start.line > b.range.start.line end\n"
    "    return a.range.start.character > b.range.start.character\n"
    "  end)\n"
    "  for _, e in ipairs(edits) do mep_lsp_apply_text_edit(e) end\n"
    "end\n"
    // Applies an LSP WorkspaceEdit (rename/code-action's own response
    // shape), possibly spanning multiple files -- normalizes both
    // `changes` (a plain uri->TextEdit[] map) and `documentChanges`
    // (TextDocumentEdit[], the shape clangd and other newer servers
    // prefer) into the same form first. No headless "edit a buffer
    // without displaying it" path exists anywhere in this codebase
    // (mep.lsp_format's own edit-apply above is single-buffer-only, and
    // mep.lsp_goto_definition/references' cross-file jumps are the only
    // precedent for touching another file at all) -- so a target file
    // that isn't already the current buffer is opened with the same
    // `mep.cmd('e ' .. f)` pattern those already use, edited, saved, and
    // the original buffer + cursor position are restored afterward.
    // Returns the total edit count actually applied.
    "function mep.lsp_apply_workspace_edit(edit)\n"
    "  if not edit then return 0 end\n"
    "  local changes = edit.changes\n"
    "  if (not changes or next(changes) == nil) and edit.documentChanges then\n"
    "    changes = {}\n"
    "    for _, dc in ipairs(edit.documentChanges) do\n"
    "      if dc.textDocument and dc.edits then changes[dc.textDocument.uri] = dc.edits end\n"
    "    end\n"
    "  end\n"
    "  if not changes then return 0 end\n"
    "  local orig_fname = mep.filename()\n"
    "  local orig_abspath = mep_lsp_abspath(orig_fname)\n"
    "  local orig_row, orig_col = mep.cursor()\n"
    "  local switched, total = false, 0\n"
    "  for uri, edits in pairs(changes) do\n"
    "    local f = uri:gsub('^file://', '')\n"
    "    if #edits > 0 then\n"
    "      if f == orig_abspath then\n"
    "        mep_lsp_apply_edits_current_buffer(edits)\n"
    "      else\n"
    "        mep.cmd('e ' .. f)\n"
    "        switched = true\n"
    "        mep_lsp_apply_edits_current_buffer(edits)\n"
    "        mep.cmd('w')\n"
    "      end\n"
    "      total = total + #edits\n"
    "    end\n"
    "  end\n"
    "  if switched then\n"
    "    mep.cmd('e ' .. orig_fname)\n"
    "    mep.set_cursor(orig_row, orig_col)\n"
    "  end\n"
    "  return total\n"
    "end\n"
    "function mep.lsp_rename()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  local default_name = mep_lsp_word_at_cursor() or ''\n"
    "  mep.ui_input('New name:', default_name, function(new_name)\n"
    "    if not new_name or new_name == '' or new_name == default_name then return end\n"
    "    mep.lsp_request(id, 'textDocument/rename', {\n"
    "      textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "      newName = new_name,\n"
    "    }, function(msg)\n"
    "      local result = mep_lsp_result(msg)\n"
    "      if not result then mep.notify('Rename: server returned no edits', 'warn') return end\n"
    "      local n = mep.lsp_apply_workspace_edit(result)\n"
    "      if n == 0 then\n"
    "        mep.notify('Rename: server returned no edits', 'warn')\n"
    "      else\n"
    "        mep.notify('Renamed to ' .. new_name .. ' (' .. n .. ' edit' .. (n == 1 and '' or 's') .. ')')\n"
    "      end\n"
    "    end)\n"
    "  end)\n"
    "end\n"
    // A CodeAction result entry is either a full CodeAction (optional
    // `edit`/`command`) or a bare Command (`command` is then a string, not
    // a table) -- both shapes handled here. `codeAction/resolve` (lazy
    // edit resolution some servers use instead of inlining `edit`
    // up front) is not implemented -- not hit by any server exercised
    // during verification, documented as a known gap below.
    "function mep_lsp_apply_code_action(id, action)\n"
    "  local applied = false\n"
    "  if action.edit then\n"
    "    applied = mep.lsp_apply_workspace_edit(action.edit) > 0\n"
    "  end\n"
    "  local cmd = action.command\n"
    "  if type(cmd) == 'table' then\n"
    "    mep.lsp_request(id, 'workspace/executeCommand', {command = cmd.command, arguments = cmd.arguments})\n"
    "    applied = true\n"
    "  elseif type(cmd) == 'string' then\n"
    "    mep.lsp_request(id, 'workspace/executeCommand', {command = cmd, arguments = action.arguments})\n"
    "    applied = true\n"
    "  end\n"
    "  mep.notify((applied and 'Applied: ' or 'No-op: ') .. (action.title or 'code action'))\n"
    "end\n"
    "function mep.lsp_code_action()\n"
    "  local id = mep.lsp_client_for()\n"
    "  if not id then mep.notify('No LSP attached', 'warn') return end\n"
    "  local row = mep.cursor()\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  local line_diags = {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    if d.range.start.line + 1 == row then line_diags[#line_diags + 1] = d end\n"
    "  end\n"
    "  mep.lsp_request(id, 'textDocument/codeAction', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())},\n"
    "    range = {start = mep_lsp_position(), ['end'] = mep_lsp_position()},\n"
    "    context = {diagnostics = line_diags},\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    if not result or #result == 0 then mep.notify('No code actions available') return end\n"
    "    if #result == 1 then mep_lsp_apply_code_action(id, result[1]) return end\n"
    "    local items = {}\n"
    "    for i, a in ipairs(result) do items[i] = {display = a.title or ('Action ' .. i), data = tostring(i)} end\n"
    "    mep.picker_open('Code Actions', items, function(sel)\n"
    "      if sel then mep_lsp_apply_code_action(id, result[tonumber(sel)]) end\n"
    "    end)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepLspAttach', mep.lsp_attach)\n"
    "mep.command('MepLspHover', mep.lsp_hover)\n"
    "mep.command('MepLspDefinition', mep.lsp_goto_definition)\n"
    "mep.command('MepLspReferences', mep.lsp_references)\n"
    "mep.command('MepLspFormat', mep.lsp_format)\n"
    "mep.command('MepLspImplementation', mep.lsp_goto_implementation)\n"
    "mep.command('MepLspTypeDefinition', mep.lsp_goto_type_definition)\n"
    "mep.command('MepLspSignatureHelp', mep.lsp_signature_help)\n"
    "mep.command('MepLspRename', mep.lsp_rename)\n"
    "mep.command('MepLspCodeAction', mep.lsp_code_action)\n"
    // Leader-key defaults: 'lt'/'rn'/'ca' deliberately match mep.nvim's
    // own keymaps.lua defaults (<leader>lt/rn/ca) for the methods it also
    // binds under <leader>; implementation/signature-help have no such
    // precedent there (mep.nvim uses bare 'gi'/'<C-k>' for those instead)
    // since mep.map only binds single ASCII keys in Normal/Visual mode --
    // no 'g'-prefixed two-key sequences or Ctrl-modified letters -- so
    // 'li'/'lk' extend the same 'l' (LSP) leader group by mnemonic instead.
    "mep.leader_map('li', 'LSP: goto implementation', mep.lsp_goto_implementation)\n"
    "mep.leader_map('lt', 'LSP: goto type definition', mep.lsp_goto_type_definition)\n"
    "mep.leader_map('lk', 'LSP: signature help', mep.lsp_signature_help)\n"
    "mep.leader_map('rn', 'LSP: rename', mep.lsp_rename)\n"
    "mep.leader_map('ca', 'LSP: code action', mep.lsp_code_action)\n"
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
    "      hl_group = hl, underline = true, sign = MEP_DIAG_GLYPH[sev] or 'E', sign_hl = hl,\n"
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
// default, now joined by two more sources folded into the same function --
// mep still only has one completion-source *slot*
// (mep.set_completion_source(fn)), so "multiple sources" here means one
// registered function that internally merges buffer words, Phase 23
// snippet trigger names, filesystem paths, and (when an LSP client is
// attached) textDocument/completion results, all through one seen-set so
// duplicate text offered by more than one source collapses to a single
// entry, then caps the combined list to MEP_COMPLETION_MAX_ITEMS.
// UpdateCompletionPopup (editor.cpp) already throttles how often this
// whole function runs (skip when the prefix hasn't changed + a 50ms
// minimum interval between genuine prefix changes) -- everything added
// below rides that same throttle rather than adding a new one, so none of
// it reintroduces the per-keystroke cost that throttle exists to prevent
// (see UpdateCompletionPopup's own comment for the history/motivation).
const char *kBuiltinCompletion =
    // Caps the combined candidate list before it ever reaches C++.
    // DrawCompletionPopup (main.cpp) measures the text width of *every*
    // item to size the popup box, on every frame the popup is open (not
    // just on keystrokes) -- an unbounded list (thousands of buffer words
    // sharing a common 2-char prefix in a huge file, or an LSP server
    // returning its whole project symbol table) would be an unbounded
    // per-frame cost even though the visible rows are already capped to 8.
    // Ranks by length-then-alphabetical (closer matches to the typed
    // prefix sort first) as a cheap relevance proxy, since none of these
    // sources hands back a real fuzzy-match score.
    "MEP_COMPLETION_MAX_ITEMS = 50\n"
    "function mep_completion_rank(words)\n"
    "  table.sort(words, function(a, b) if #a ~= #b then return #a < #b end return a < b end)\n"
    "  if #words <= MEP_COMPLETION_MAX_ITEMS then return words end\n"
    "  local capped = {}\n"
    "  for i = 1, MEP_COMPLETION_MAX_ITEMS do capped[i] = words[i] end\n"
    "  return capped\n"
    "end\n"
    // Path completion source (new): returns dir, base if the text right
    // before the current alnum-prefix (as computed by
    // UpdateCompletionPopup, editor.cpp) looks like a filesystem path,
    // nil otherwise. Two triggers:
    //   1. The character immediately before the prefix is '/' or '.' --
    //      covers `src/ed`, `./sr`, `../foo/ba` (the prefix scan already
    //      stops right at that character, so it's exactly the boundary
    //      to inspect here).
    //   2. The prefix opens right after a quote that itself follows
    //      `require(`, `import `, or `from ` earlier on the line -- the
    //      "typing a path inside an import/require string literal" case
    //      -- even with no `/` typed yet.
    // Both resolve relative to the current working directory (matching
    // :e/:w's own convention), not the edited file's directory -- a
    // known, documented simplification; a real `dirname(mep.filename())`
    // base would be a small follow-up.
    "function mep_completion_path_prefix(prefix, row, col, line)\n"
    "  local start = col - 1 - #prefix\n"
    "  local before = line:sub(1, start)\n"
    "  local trigger = before:sub(-1)\n"
    "  if trigger == '/' or trigger == '.' then\n"
    "    local token = before:match('[%w_%.%-/]*$') or ''\n"
    "    local dir = token:match('^(.*/)') or ''\n"
    "    return (dir == '' and '.' or dir), prefix\n"
    "  end\n"
    "  if trigger == '\"' or trigger == \"'\" then\n"
    "    local ctx = before:sub(1, -2):sub(-40)\n"
    "    if ctx:find('require%s*%(%s*$') or ctx:find('import%s') or ctx:find('from%s') then\n"
    "      return '.', prefix\n"
    "    end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    // LSP completion cache (new source): textDocument/completion is
    // async, unlike every other source here, so it can't be answered
    // inline the way buffer words can. Cached by *word start* position
    // (stable while a word is being typed) rather than by the growing
    // prefix, so a response that arrives after the word's first
    // keystroke is still reused -- filtered locally against whatever the
    // prefix has grown to -- on every later keystroke of that same word,
    // instead of firing a fresh request each time.
    // mep_lsp_completion_pending guards against firing a second request
    // for the same word while the first is still in flight.
    "mep_lsp_completion_cache = {items = {}, row = nil, start_col = nil}\n"
    "mep_lsp_completion_pending = {row = nil, start_col = nil}\n"
    // text -> LSP InsertTextFormat (1 = PlainText, 2 = Snippet) for
    // whatever candidate text mep_lsp_completion_cache most recently
    // offered -- Phase 23's completion-accept hook (kBuiltinSnippets)
    // consults this after AcceptCompletion (editor.cpp) splices the raw
    // text in, to tell a real `${1:x}`-style Snippet item apart from an
    // ordinary PlainText one before deciding whether to re-expand it
    // through the tabstop engine. Keyed by text rather than by
    // row/start_col like the cache above since that's all the accept hook
    // is handed back; a global text->format table can only misfire if two
    // *different* items from the same response resolve to byte-identical
    // insertText with different formats, which would already be a
    // pathological server response.
    "mep_lsp_completion_iformat = {}\n"
    "function mep_lsp_completion_request(client, row, start_col)\n"
    "  mep.lsp_request(client, 'textDocument/completion', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    // CompletionList ({isIncomplete=, items=...}) vs a bare
    // CompletionItem[] -- servers use either shape; `result.items` is
    // simply absent on the array form, so the `or result` fallback
    // covers it without needing to distinguish the two explicitly.
    "    local list = (result and (result.items or result)) or {}\n"
    "    local items = {}\n"
    "    for _, it in ipairs(list) do\n"
    "      local text = it.insertText or it.label\n"
    "      if text and #text > 0 then\n"
    "        items[#items + 1] = text\n"
    "        mep_lsp_completion_iformat[text] = it.insertTextFormat\n"
    "      end\n"
    "    end\n"
    "    mep_lsp_completion_cache = {items = items, row = row, start_col = start_col}\n"
    "  end)\n"
    "end\n"
    "function mep.completion_buffer_words(prefix)\n"
    "  local seen, words = {}, {}\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    // Path context takes over the whole source when detected -- an
    // identifier match like "function" sitting next to a half-typed
    // filename would just be noise, so buffer words/snippets/LSP are
    // skipped rather than merged in this case.
    "  local path_dir, path_base = mep_completion_path_prefix(prefix, row, col, line)\n"
    "  if path_dir then\n"
    "    for _, e in ipairs(mep.list_dir(path_dir)) do\n"
    "      if #e.name > #path_base and e.name:sub(1, #path_base) == path_base and not seen[e.name] then\n"
    "        seen[e.name] = true\n"
    "        words[#words + 1] = e.is_dir and (e.name .. '/') or e.name\n"
    "      end\n"
    "    end\n"
    "    return mep_completion_rank(words)\n"
    "  end\n"
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
    // LSP source: merge cached results (filtered against the current
    // prefix) if they're for this same word, else kick off (or leave in
    // flight) an async request for it -- see mep_lsp_completion_request's
    // comment above for the caching scheme.
    "  local client = mep.lsp_client_for and mep.lsp_client_for()\n"
    "  if client then\n"
    "    local start_col = col - 1 - #prefix\n"
    "    if mep_lsp_completion_cache.row == row and mep_lsp_completion_cache.start_col == start_col then\n"
    "      for _, w in ipairs(mep_lsp_completion_cache.items) do\n"
    "        if #w > #prefix and w:sub(1, #prefix) == prefix and not seen[w] then\n"
    "          seen[w] = true\n"
    "          words[#words + 1] = w\n"
    "        end\n"
    "      end\n"
    "    elseif not (mep_lsp_completion_pending.row == row and mep_lsp_completion_pending.start_col == start_col) then\n"
    "      mep_lsp_completion_pending = {row = row, start_col = start_col}\n"
    "      mep_lsp_completion_request(client, row, start_col)\n"
    "    end\n"
    "  end\n"
    "  return mep_completion_rank(words)\n"
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
// what that picker already does.
//
// Keybinding introspection (the plan's other half of this bullet) now has
// a real consumer: mep.keymaps()/:MepKeymaps below, a Phase 8-style
// searchable picker over mep.mapping_descriptions() (plain mep.map()
// bindings) and mep.leader_bindings() (leader-sequence bindings) --
// mapping_descriptions_ existed as a registry with nothing reading it
// (mep.map's own callers rarely attach a desc, and nothing surfaced
// leader_map's own long-standing WhichKeyBinding descriptions outside the
// transient which-key overlay either); this is the first thing that reads
// either as a persistent, filterable list rather than a one-shot popup.
//
// Doc-template generation: MEP_DOC_LANG maps a bare file extension (what
// mep_lsp_filetype returns) to a canonical language key -- note this
// fixes a real pre-existing bug, not just an extension: the old
// MEP_DOC_TEMPLATES was keyed directly by mep_lsp_filetype's result
// ('py', 'js', ...) but its own table used language *names* ('python',
// 'javascript') as keys, so `MEP_DOC_TEMPLATES[ft]` only ever actually
// matched for 'lua' (where the extension and the name happen to be
// spelled the same) -- .py/.js files silently hit "No doc template for
// this filetype" this whole time. Also extends coverage to every other
// filetype with a real doc-comment convention this codebase already has
// LSP or syntax-highlighting support for (c/cpp via clangd, go, rust,
// java, ruby), beyond the original lua/python/javascript three.
const char *kBuiltinDocs =
    "local MEP_DOC_LANG = {\n"
    "  lua = 'lua',\n"
    "  py = 'python',\n"
    "  js = 'javascript', jsx = 'javascript', mjs = 'javascript', cjs = 'javascript',\n"
    "  ts = 'javascript', tsx = 'javascript',\n"
    "  c = 'c', h = 'c', cpp = 'c', cc = 'c', cxx = 'c', hpp = 'c', hh = 'c', hxx = 'c',\n"
    "  go = 'go', rs = 'rust', java = 'java', rb = 'ruby',\n"
    "}\n"
    // Each template is `function(sig, params, ret)`: `sig` is the raw
    // signature text (the source line when no LSP data is available, or
    // the LSP-reported SignatureInformation.label when it is); `params`
    // is an array of {name=, type=} (type may be nil) -- empty when
    // there's no LSP-derived breakdown, in which case every template
    // below degrades to exactly its original lua/python/javascript-only
    // single-line-signature output (verified line-for-line against the
    // pre-existing behavior); `ret` is a return-type string or nil.
    "local MEP_DOC_TEMPLATES = {\n"
    "  lua = function(sig, params, ret)\n"
    "    local lines = {'--- ' .. sig}\n"
    "    if #params > 0 then\n"
    "      for _, p in ipairs(params) do\n"
    "        lines[#lines + 1] = '-- @param ' .. p.name .. (p.type and (' ' .. p.type) or '')\n"
    "      end\n"
    "    else\n"
    "      lines[#lines + 1] = '-- @param'\n"
    "    end\n"
    "    lines[#lines + 1] = '-- @return' .. (ret and (' ' .. ret) or '')\n"
    "    return lines\n"
    "  end,\n"
    "  python = function(sig, params, ret)\n"
    "    local lines = {'\"\"\"', sig}\n"
    "    if #params > 0 then\n"
    "      lines[#lines + 1] = ''\n"
    "      lines[#lines + 1] = 'Args:'\n"
    "      for _, p in ipairs(params) do\n"
    "        lines[#lines + 1] = '    ' .. p.name .. (p.type and (' (' .. p.type .. ')') or '') .. ': '\n"
    "      end\n"
    "    end\n"
    "    if ret then\n"
    "      lines[#lines + 1] = ''\n"
    "      lines[#lines + 1] = 'Returns:'\n"
    "      lines[#lines + 1] = '    ' .. ret .. ': '\n"
    "    end\n"
    "    lines[#lines + 1] = '\"\"\"'\n"
    "    return lines\n"
    "  end,\n"
    "  javascript = function(sig, params, ret)\n"
    "    local lines = {'/**', ' * ' .. sig}\n"
    "    for _, p in ipairs(params) do\n"
    "      lines[#lines + 1] = ' * @param ' .. (p.type and ('{' .. p.type .. '} ') or '') .. p.name\n"
    "    end\n"
    "    if ret then lines[#lines + 1] = ' * @returns {' .. ret .. '}' end\n"
    "    lines[#lines + 1] = ' */'\n"
    "    return lines\n"
    "  end,\n"
    "  c = function(sig, params, ret)\n"
    "    local lines = {'/**', ' * @brief ' .. sig}\n"
    "    for _, p in ipairs(params) do\n"
    "      lines[#lines + 1] = ' * @param ' .. p.name .. (p.type and (' ' .. p.type) or '')\n"
    "    end\n"
    "    if ret then lines[#lines + 1] = ' * @return ' .. ret end\n"
    "    lines[#lines + 1] = ' */'\n"
    "    return lines\n"
    "  end,\n"
    "  java = function(sig, params, ret)\n"
    "    local lines = {'/**', ' * ' .. sig}\n"
    "    for _, p in ipairs(params) do\n"
    "      lines[#lines + 1] = ' * @param ' .. p.name .. (p.type and (' ' .. p.type) or '')\n"
    "    end\n"
    "    if ret then lines[#lines + 1] = ' * @return ' .. ret end\n"
    "    lines[#lines + 1] = ' */'\n"
    "    return lines\n"
    "  end,\n"
    "  rust = function(sig, params, ret)\n"
    "    local lines = {'/// ' .. sig}\n"
    "    if #params > 0 then\n"
    "      lines[#lines + 1] = '///'\n"
    "      lines[#lines + 1] = '/// # Arguments'\n"
    "      lines[#lines + 1] = '///'\n"
    "      for _, p in ipairs(params) do\n"
    "        lines[#lines + 1] = '/// * `' .. p.name .. '` - ' .. (p.type or '')\n"
    "      end\n"
    "    end\n"
    "    if ret then\n"
    "      lines[#lines + 1] = '///'\n"
    "      lines[#lines + 1] = '/// # Returns'\n"
    "      lines[#lines + 1] = '///'\n"
    "      lines[#lines + 1] = '/// ' .. ret\n"
    "    end\n"
    "    return lines\n"
    "  end,\n"
    "  go = function(sig, params, ret)\n"
    "    local name = sig:match('func%s*%b()%s*([%w_]+)') or sig:match('func%s+([%w_]+)')\n"
    "    local lines = {'// ' .. (name or sig)}\n"
    "    for _, p in ipairs(params) do\n"
    "      lines[#lines + 1] = '// ' .. p.name .. (p.type and (' ' .. p.type) or '')\n"
    "    end\n"
    "    if ret then lines[#lines + 1] = '// returns ' .. ret end\n"
    "    return lines\n"
    "  end,\n"
    "  ruby = function(sig, params, ret)\n"
    "    local lines = {'# ' .. sig}\n"
    "    for _, p in ipairs(params) do\n"
    "      lines[#lines + 1] = '# @param ' .. p.name .. (p.type and (' [' .. p.type .. ']') or '')\n"
    "    end\n"
    "    if ret then lines[#lines + 1] = '# @return [' .. ret .. ']' end\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    // Splits one LSP ParameterInformation label into {name, type}: tries
    // "name: type" first (python/typescript/rust/kotlin convention), then
    // falls back to "type name" (c/go/java convention, name = trailing
    // identifier) -- best-effort on both fronts (a bare "int a" label
    // returns name='a', type=nil since there's no colon to split on, not
    // the theoretically-correct type='int'; getting this exactly right
    // per-language would need a real per-grammar parser, not a two-regex
    // heuristic) but strictly better than the single opaque signature
    // line this replaces.\n"
    "local function mep_doc_split_param(label)\n"
    "  local name, ptype = label:match('^([%w_]+)%s*:%s*(.+)$')\n"
    "  if name then return name, ptype end\n"
    "  ptype, name = label:match('^(.-)%s+([%w_]+)$')\n"
    "  if name and ptype and ptype ~= '' then return name, ptype end\n"
    "  return label, nil\n"
    "end\n"
    // SignatureInformation.parameters[i].label is either a plain string
    // (used as-is) or a [start, end] pair of *character offsets into the
    // signature's own label* (sliced out here) -- both are valid per the
    // LSP spec, and a server is free to use either.
    "local function mep_doc_params_from_signature(active)\n"
    "  local label = active.label or ''\n"
    "  local params = {}\n"
    "  for _, p in ipairs(active.parameters or {}) do\n"
    "    local ptext = nil\n"
    "    if type(p.label) == 'string' then\n"
    "      ptext = p.label\n"
    "    elseif type(p.label) == 'table' then\n"
    "      ptext = label:sub((p.label[1] or 0) + 1, p.label[2] or #label)\n"
    "    end\n"
    "    if ptext and ptext ~= '' then\n"
    "      local name, ptype = mep_doc_split_param(ptext)\n"
    "      params[#params + 1] = {name = name, type = ptype}\n"
    "    end\n"
    "  end\n"
    "  return params\n"
    "end\n"
    // Best-effort return-type scrape off the trailing "-> Type"
    // (python/rust) or ": Type" (typescript) after the closing paren --
    // nil (no @return/Returns line at all) for languages/signatures
    // where neither shows up, e.g. C/Java's return type leads the
    // signature instead of trailing it, which this doesn't attempt to
    // recover.
    "local function mep_doc_return_type(sig)\n"
    "  local ret = sig:match('%)%s*%->%s*(.-)%s*$')\n"
    "  if ret and ret ~= '' then return ret end\n"
    "  ret = sig:match('%)%s*:%s*(.-)%s*$')\n"
    "  if ret and ret ~= '' then return ret end\n"
    "  return nil\n"
    "end\n"
    // Phase 25's original regex/same-line-scan generator, kept verbatim
    // as the always-available fallback (no LSP attached, or the attached
    // server's signatureHelp response comes back empty/erroring).
    "local function mep_docs_generate_syntactic(row, sig, tmpl)\n"
    "  mep.replace_lines(row, row, tmpl(sig, {}, nil))\n"
    "end\n"
    "function mep.docs_generate()\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  local lang = ft and MEP_DOC_LANG[ft]\n"
    "  local tmpl = lang and MEP_DOC_TEMPLATES[lang]\n"
    "  if not tmpl then mep.notify('No doc template for this filetype', 'warn') return end\n"
    "  local row = mep.cursor()\n"
    "  local sig = mep.get_line(row):match('^%s*(.-)%s*$')\n"
    "  local client = mep.lsp_client_for()\n"
    "  if not client then\n"
    "    mep_docs_generate_syntactic(row, sig, tmpl)\n"
    "    return\n"
    "  end\n"
    // LSP attached: prefer the real signature/parameter/return info from
    // textDocument/signatureHelp (Phase 20) over the regex scan, for
    // accurate param names/types and (when the server provides one) a
    // real return type instead of just echoing the source line back as
    // one opaque comment. This is necessarily async -- mep.lsp_request
    // has no blocking mode and, per Phase 20's own documented gap, no
    // request-timeout mechanism exists anywhere in this file either -- so
    // the skeleton is inserted once the callback actually fires, whether
    // that's with real signature data or (on an error/empty result) the
    // same syntactic fallback used when no LSP is attached at all. If the
    // server never replies (hangs or dies mid-request), nothing gets
    // inserted; a real, honest limitation, not one silently masked by a
    // fake timeout.
    "  mep.lsp_request(client, 'textDocument/signatureHelp', {\n"
    "    textDocument = {uri = mep_lsp_uri(mep.filename())}, position = mep_lsp_position(),\n"
    "  }, function(msg)\n"
    "    local result = mep_lsp_result(msg)\n"
    "    local sigs = result and result.signatures\n"
    "    local active = sigs and (sigs[(result.activeSignature or 0) + 1] or sigs[1])\n"
    "    if not active then\n"
    "      mep_docs_generate_syntactic(row, sig, tmpl)\n"
    "      return\n"
    "    end\n"
    "    local label = (active.label and active.label ~= '') and active.label or sig\n"
    "    local params = mep_doc_params_from_signature(active)\n"
    "    local ret = mep_doc_return_type(label)\n"
    "    mep.replace_lines(row, row, tmpl(label, params, ret))\n"
    "  end)\n"
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
    "mep.command('MepHelp', mep.commands)\n"
    // Keybinding introspection: a Phase 8-style searchable picker (typing
    // filters, <CR> selects, same as mep.commands()) listing every
    // described mapping from both registries -- mep.mapping_descriptions()
    // (plain mep.map() bindings, mode + key) and mep.leader_bindings()
    // (leader-sequence bindings, prefixed '<leader>' the way the
    // which-key overlay itself displays them). Unlike mep.commands(),
    // selecting an entry doesn't *run* it -- these are raw key sequences
    // read out of context, not named actions safe to fire blind (a
    // Normal-mode single-key mapping like 'gcc' assumes real buffer/
    // cursor state a picker selection can't reconstruct) -- so <CR> just
    // closes the picker once you've found what you were looking for,
    // matching how e.g. mep.docs_lookup's devdocs-search picker sources
    // elsewhere in this file are look-not-do too.
    "local MEP_MODE_NAMES = {n = 'Normal', v = 'Visual'}\n"
    "function mep.keymaps()\n"
    "  local items = {}\n"
    "  for _, m in ipairs(mep.mapping_descriptions()) do\n"
    "    items[#items + 1] = string.format('%-8s %-14s %s', MEP_MODE_NAMES[m.mode] or m.mode, m.key, m.desc)\n"
    "  end\n"
    "  for _, w in ipairs(mep.leader_bindings()) do\n"
    "    items[#items + 1] = string.format('%-8s %-14s %s', 'Leader', '<leader>' .. w.seq, w.desc)\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No described keybindings registered', 'warn') return end\n"
    "  table.sort(items)\n"
    "  mep.picker_open('Keymaps', items, function() end)\n"
    "end\n"
    "mep.command('MepKeymaps', mep.keymaps)\n"
    "mep.leader_map('hk', 'Help: keymaps', mep.keymaps)\n";

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
    // NVIM_PARITY_PLAN.md's "Full DAP adapter registry ... same
    // incremental philosophy; the registry is just data, add entries as
    // needed" -- grown here from the original 2 (cpp/python), same
    // {cmd, filetypes} shape, keyed by mep.dap_start(lang)'s own `lang`
    // argument (an arbitrary language id, not a filetype -- unlike
    // mep.lsp_servers, nothing in this codebase auto-derives it from the
    // current buffer yet). `rust`/`c` reuse lldb-dap (it's a real
    // multi-language adapter, not C++-only, LLVM's official DAP server
    // speaking stdio directly -- no socket hop needed). `go` and
    // `javascript` are included even though, like the plan's own noted
    // `delve` stdio gap, their real-world adapters (`dlv dap`,
    // `js-debug-adapter`) normally listen on a TCP port rather than
    // stdio -- documented here rather than silently omitted, matching
    // this file's existing honesty about delve.\n"
    "  rust = {cmd = {'lldb-dap'}, filetypes = {'rs'}},\n"
    "  c = {cmd = {'lldb-dap'}, filetypes = {'c'}},\n"
    // Real, documented stdio gap (like delve): `dlv dap` normally speaks
    // DAP over a TCP listener (`--listen`), not stdin/stdout, so this
    // entry -- like mep.nvim's own -- won't actually connect through
    // mep.lsp_start's stdio-pipe transport without a `dlv`/adapter change
    // neither side of this registry entry can fix by itself.\n"
    "  go = {cmd = {'dlv', 'dap'}, filetypes = {'go'}},\n"
    // Same stdio-vs-socket caveat as `go` above: the real `js-debug`
    // adapter (`js-debug-adapter`) listens on a TCP port; this entry
    // documents the intended command, not a verified-working stdio path.\n"
    "  javascript = {cmd = {'js-debug-adapter'}, filetypes = {'js', 'ts'}},\n"
    // netcoredbg's `--interpreter=vscode` flag is the one common adapter
    // here that *does* speak DAP over stdio directly, same as lldb-dap.\n"
    "  csharp = {cmd = {'netcoredbg', '--interpreter=vscode'}, filetypes = {'cs'}},\n"
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
// decoration + Phase 9 highlight-group pipeline. Full buffer text handed
// over on every call: on demand via :MepSyntax, automatically on buffer
// switch, and debounce-rerun on edits (mep.syntax_auto, on by default --
// see the two mep.on_buffer_changed/mep.on_frame hooks after
// :MepSyntax's registration below). mep.ts_captures's own C++ side
// (treesitter.cpp) reparses each call incrementally against its cached
// previous TSTree for that filetype rather than from scratch -- but the
// call *pattern* here is still "hand over the whole current text",
// consistent with this codebase's existing "on-demand full rescan" scope
// decisions elsewhere (see Phase 13's
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
    // Per-level heading colors (kHighlightsMarkdown emits
    // text.title.1..6 for ATX/setext headings, one level = one distinct
    // highlight group, roughly outermost-to-innermost through the
    // palette) -- text.title itself is kept as a catch-all in case some
    // other markdown-like grammar (or a future query edit) still emits
    // the flat capture.
    "  ['text.title'] = 'Purple', ['text.title.1'] = 'Purple', ['text.title.2'] = 'Blue',\n"
    "  ['text.title.3'] = 'Cyan', ['text.title.4'] = 'Green', ['text.title.5'] = 'Yellow',\n"
    "  ['text.title.6'] = 'Orange',\n"
    "  ['text.strong'] = 'Yellow', ['text.emphasis'] = 'Cyan',\n"
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
    // Treesitter fold-query execution (Phase 19's other noted gap,
    // alongside the highlighter above -- see kFolds* in
    // treesitter_queries.h): a *separate* function/command/flag from
    // syntax highlighting, not folded into mep.syntax_highlight itself,
    // because collapsing code is a much more visible, opinionated side
    // effect than coloring it -- defaulting *that* to on-by-default the
    // way syntax highlighting is would surprise every existing user who
    // just opens a file expecting to see it, not see it partially
    // collapsed. mep.ts_fold_ranges (lua_env.cpp, backed by
    // TreesitterFoldRanges in treesitter.cpp) returns nil for filetypes
    // with no fold query (markdown/org keep their own heading-based fold
    // providers already registered elsewhere in this file -- running
    // this too for them would just double up on the same regions under a
    // different provider name).
    "function mep.syntax_fold()\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  if not ft then return end\n"
    "  local lines = {}\n"
    "  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end\n"
    "  local ranges = mep.ts_fold_ranges(ft, table.concat(lines, '\\n'))\n"
    "  if not ranges then return end\n"
    "  mep.fold_clear_provider('treesitter')\n"
    "  for _, r in ipairs(ranges) do\n"
    "    mep.fold_create(r.start_row, r.end_row, false, 'treesitter')\n"
    "  end\n"
    "end\n"
    "mep.command('MepSyntaxFold', mep.syntax_fold)\n"
    "mep.syntax_fold_auto = false\n"
    "mep.on_buffer_changed(function() if mep.syntax_fold_auto then mep.syntax_fold() end end)\n"
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
    // NVIM_PARITY_PLAN.md's "Full babel/run/REPL language matrices (~25/
    // ~13/~13 languages in mep.nvim) ... start with 2-4 per feature, grow
    // incrementally" -- grown here. Every entry below is a single
    // `interpreter [flags] <file>` invocation (mep.run_file appends the
    // filename as the command's last argument, so anything needing a
    // separate compile step -- C, C++, Rust, ordinary .kt -- doesn't fit
    // this table's shape and is deliberately left out, same scope cut
    // NVIM_PARITY_PLAN.md's babel phase already documents for itself).
    // `go`/`java`/`swift`/`crystal`/`dart`/`kotlin` (.kts) are the
    // exceptions that *do* fit: each has a real single-command
    // compile-and-run or direct-source-execution mode (`go run`, JDK 11+
    // `java <file>.java`, `swift <file>.swift`, `crystal run`, `dart run`,
    // `kotlin <file>.kts`).\n"
    "  ruby = {'ruby'}, perl = {'perl'}, php = {'php'}, r = {'Rscript'},\n"
    "  go = {'go', 'run'}, java = {'java'}, typescript = {'ts-node'}, julia = {'julia'},\n"
    "  swift = {'swift'}, crystal = {'crystal', 'run'}, dart = {'dart', 'run'},\n"
    "  elixir = {'elixir'}, kotlin = {'kotlin'},\n"
    "}\n"
    // Real, pre-existing bug matching the one already found and fixed for
    // mep.syntax_keywords/MEP_DOC_TEMPLATES elsewhere in this file:
    // mep.run_file() looks entries up by mep_lsp_filetype()'s bare
    // extension ('py', 'js'), but several entries above are keyed by
    // language *name* (or a name spelled differently from its extension)
    // -- so those were silently unreachable through mep.run_file (only
    // entries whose key happens to equal their own extension, like 'lua'/
    // 'sh'/'go'/'java'/'swift'/'dart', ever actually matched). Aliased the
    // same way the syntax-keywords table already was, rather than rekeying
    // the existing entries.\n"
    "mep.run_languages.py = mep.run_languages.python\n"
    "mep.run_languages.js = mep.run_languages.javascript\n"
    "mep.run_languages.mjs = mep.run_languages.javascript\n"
    "mep.run_languages.cjs = mep.run_languages.javascript\n"
    "mep.run_languages.rb = mep.run_languages.ruby\n"
    "mep.run_languages.pl = mep.run_languages.perl\n"
    "mep.run_languages.R = mep.run_languages.r\n"
    "mep.run_languages.ts = mep.run_languages.typescript\n"
    "mep.run_languages.tsx = mep.run_languages.typescript\n"
    "mep.run_languages.jl = mep.run_languages.julia\n"
    "mep.run_languages.cr = mep.run_languages.crystal\n"
    "mep.run_languages.ex = mep.run_languages.elixir\n"
    "mep.run_languages.kts = mep.run_languages.kotlin\n"
    "mep.repl_languages = {\n"
    "  lua = {'lua'}, python = {'python3', '-u'},\n"
    // Same growth as mep.run_languages above, but these are the *REPL*
    // launch command itself (mep.repl_start passes it straight to
    // mep.term_start with no filename appended), so a language only needs
    // an interactive prompt to qualify -- 'haskell' (ghci) has one even
    // though it's absent from mep.run_languages (no fitting single-shot
    // run-a-file command for a compiled language).\n"
    "  javascript = {'node'}, ruby = {'irb'}, sh = {'sh'}, php = {'php', '-a'},\n"
    "  r = {'R', '--no-save'}, julia = {'julia'}, haskell = {'ghci'}, elixir = {'iex'},\n"
    "}\n"
    "mep.repl_languages.py = mep.repl_languages.python\n"
    "mep.repl_languages.js = mep.repl_languages.javascript\n"
    "mep.repl_languages.mjs = mep.repl_languages.javascript\n"
    "mep.repl_languages.cjs = mep.repl_languages.javascript\n"
    "mep.repl_languages.rb = mep.repl_languages.ruby\n"
    "mep.repl_languages.R = mep.repl_languages.r\n"
    "mep.repl_languages.jl = mep.repl_languages.julia\n"
    "mep.repl_languages.hs = mep.repl_languages.haskell\n"
    "mep.repl_languages.ex = mep.repl_languages.elixir\n"
    "mep.repl_languages.exs = mep.repl_languages.elixir\n"
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
    "mep.command('MepMarkdown', mep.md_render)\n"
    // GFM pipe tables (mirrors mep.org_table_align's design -- Phase 30's
    // `mep_org_table_row`/`mep.org_table_align`, kBuiltinOrgLinks below --
    // but GFM tables carry per-column alignment in the separator row
    // (`:---`/`---:`/`:---:`/`---`) that org tables don't have, so this
    // is its own parser/renderer rather than a literal copy-paste).
    "local function mep_md_table_row(line)\n"
    "  if not line:match('^%s*|') then return nil end\n"
    "  local inner = line:match('^%s*|(.-)|?%s*$')\n"
    "  local cells = {}\n"
    "  for cell in (inner .. '|'):gmatch('(.-)|') do cells[#cells + 1] = cell:match('^%s*(.-)%s*$') end\n"
    "  local is_sep = #cells > 0\n"
    "  for _, c in ipairs(cells) do if not c:match('^:?%-+:?$') then is_sep = false break end end\n"
    "  if is_sep then\n"
    "    local aligns = {}\n"
    "    for i, c in ipairs(cells) do\n"
    "      local l, r = c:sub(1, 1) == ':', c:sub(-1) == ':'\n"
    "      aligns[i] = (l and r) and 'center' or (r and 'right') or (l and 'left') or 'none'\n"
    "    end\n"
    "    return 'sep', aligns\n"
    "  end\n"
    "  return cells\n"
    "end\n"
    "local function mep_md_sep_cell(w, al)\n"
    "  if al == 'left' then return ':' .. string.rep('-', math.max(1, w - 1))\n"
    "  elseif al == 'right' then return string.rep('-', math.max(1, w - 1)) .. ':'\n"
    "  elseif al == 'center' then return ':' .. string.rep('-', math.max(1, w - 2)) .. ':'\n"
    "  else return string.rep('-', w) end\n"
    "end\n"
    // Finds the contiguous run of |-lines touching the cursor (same
    // approach as mep.org_table_align), computes per-column max width
    // across all data rows, then rewrites every row: data cells padded
    // per that column's alignment (left default, right, or center-split),
    // and the separator row rebuilt to the same width while preserving
    // whichever alignment colons it already carried.
    "function mep.md_table_align()\n"
    "  local row = mep.cursor()\n"
    "  if not mep_md_table_row(mep.get_line(row)) then mep.notify('Not on a table row', 'warn') return end\n"
    "  local top = row\n"
    "  while top > 1 and mep_md_table_row(mep.get_line(top - 1)) do top = top - 1 end\n"
    "  local bot, n = row, mep.line_count()\n"
    "  while bot < n and mep_md_table_row(mep.get_line(bot + 1)) do bot = bot + 1 end\n"
    "  local widths, aligns, rows = {}, {}, {}\n"
    "  for i = top, bot do\n"
    "    local r, a = mep_md_table_row(mep.get_line(i))\n"
    "    rows[#rows + 1] = {i, r, a}\n"
    "    if r == 'sep' then\n"
    "      for ci, al in pairs(a) do aligns[ci] = al end\n"
    "    else\n"
    "      for ci, cell in ipairs(r) do widths[ci] = math.max(widths[ci] or 3, #cell) end\n"
    "    end\n"
    "  end\n"
    "  for _, entry in ipairs(rows) do\n"
    "    local i, r, a = entry[1], entry[2], entry[3]\n"
    "    local parts = {}\n"
    "    if r == 'sep' then\n"
    "      for ci, w in ipairs(widths) do parts[#parts + 1] = ' ' .. mep_md_sep_cell(w, aligns[ci] or 'none') .. ' ' end\n"
    "    else\n"
    "      for ci, w in ipairs(widths) do\n"
    "        local cell, al = r[ci] or '', aligns[ci] or 'none'\n"
    "        local pad = math.max(0, w - #cell)\n"
    "        local padded\n"
    "        if al == 'right' then padded = string.rep(' ', pad) .. cell\n"
    "        elseif al == 'center' then\n"
    "          local lp = math.floor(pad / 2)\n"
    "          padded = string.rep(' ', lp) .. cell .. string.rep(' ', pad - lp)\n"
    "        else padded = cell .. string.rep(' ', pad) end\n"
    "        parts[#parts + 1] = ' ' .. padded .. ' '\n"
    "      end\n"
    "    end\n"
    "    mep.set_line(i, '|' .. table.concat(parts, '|') .. '|')\n"
    "  end\n"
    "end\n"
    "mep.command('MepMdTableAlign', mep.md_table_align)\n"
    // Insert row/column, mirroring the spirit of the phase-30 tables
    // feature set (org itself has no insert-row/column commands to
    // mirror -- align is its only table verb -- so these are new,
    // scoped-down helpers: insert row goes directly below the cursor's
    // row with the same column count; insert column always appends a new
    // empty column at the end of every row in the block, not at the
    // cursor's column, to avoid needing to parse *which* cell the cursor
    // is inside of). Both re-run md_table_align afterwards so the table
    // stays lined up.
    "function mep.md_table_insert_row()\n"
    "  local row = mep.cursor()\n"
    "  local r = mep_md_table_row(mep.get_line(row))\n"
    "  if not r or r == 'sep' then mep.notify('Not on a table data row', 'warn') return end\n"
    "  local blank = '|' .. string.rep(' |', #r)\n"
    "  mep.replace_lines(row, row + 1, {mep.get_line(row), blank})\n"
    "  mep.set_cursor(row + 1, 2)\n"
    "  mep.md_table_align()\n"
    "end\n"
    "mep.command('MepMdTableInsertRow', mep.md_table_insert_row)\n"
    "function mep.md_table_insert_col()\n"
    "  local row = mep.cursor()\n"
    "  if not mep_md_table_row(mep.get_line(row)) then mep.notify('Not on a table row', 'warn') return end\n"
    "  local top = row\n"
    "  while top > 1 and mep_md_table_row(mep.get_line(top - 1)) do top = top - 1 end\n"
    "  local bot, n = row, mep.line_count()\n"
    "  while bot < n and mep_md_table_row(mep.get_line(bot + 1)) do bot = bot + 1 end\n"
    "  local newlines = {}\n"
    "  for i = top, bot do\n"
    "    local r = mep_md_table_row(mep.get_line(i))\n"
    "    local suffix = (r == 'sep') and '---|' or ' |'\n"
    "    newlines[#newlines + 1] = mep.get_line(i):gsub('%s+$', '') .. suffix\n"
    "  end\n"
    "  mep.replace_lines(top, bot + 1, newlines)\n"
    "  mep.md_table_align()\n"
    "end\n"
    "mep.command('MepMdTableInsertCol', mep.md_table_insert_col)\n"
    // Link/emphasis concealment: hides `**`/`__`/`*`/`_` emphasis markers
    // and `[`/`]`/`(`/`)`/link-target text, showing plain bold/italic
    // text and just the link's visible text -- except on the cursor's
    // own line, which always renders raw so it can be edited normally.
    // Built on the `virt_overlay` decoration primitive (src/editor.h's
    // Decoration::virt_overlay, previously wired up but never actually
    // exercised for concealment by any builtin plugin -- see the
    // renderer fix alongside this for why it needed one). A hand-rolled
    // single-pass line scanner, not two independent regexes, specifically
    // so `**bold**` can't also be misparsed as `*bold*` wrapped in stray
    // `*`s (deliberately scoped down from a general conceal engine, per
    // NVIM_PARITY_PLAN.md -- covers emphasis + inline/reference links
    // only, not code spans, autolinks, or images).
    "local mep_md_conceal_ns = nil\n"
    "mep.md_conceal_auto = true\n"
    "local function mep_md_conceal_spans(line)\n"
    "  local spans, i, n = {}, 1, #line\n"
    "  while i <= n do\n"
    "    local two = line:sub(i, i + 1)\n"
    "    if two == '**' or two == '__' then\n"
    "      local close = line:find(two, i + 2, true)\n"
    "      if close then\n"
    "        spans[#spans + 1] = {s = i, e = close + 2, text = line:sub(i + 2, close - 1), hl = 'Yellow'}\n"
    "        i = close + 2\n"
    "      else i = i + 1 end\n"
    "    else\n"
    "      local c = line:sub(i, i)\n"
    "      if c == '*' or c == '_' then\n"
    "        local before = (i > 1) and line:sub(i - 1, i - 1) or ''\n"
    // Avoid treating a mid-identifier '_' (snake_case) as an emphasis
    // delimiter: only conceal when the char just before the opening
    // delimiter isn't itself a word character.
    "        local close = (not before:match('%w')) and line:find(c, i + 1, true) or nil\n"
    "        if close and close > i + 1 and not line:sub(close + 1, close + 1):match('%w') then\n"
    "          spans[#spans + 1] = {s = i, e = close + 1, text = line:sub(i + 1, close - 1), hl = 'Cyan'}\n"
    "          i = close + 1\n"
    "        else i = i + 1 end\n"
    "      elseif c == '[' then\n"
    "        local closeb = line:find(']', i + 1, true)\n"
    "        local after = closeb and line:sub(closeb + 1, closeb + 1) or ''\n"
    "        if closeb and after == '(' then\n"
    "          local closep = line:find(')', closeb + 2, true)\n"
    "          if closep then\n"
    "            spans[#spans + 1] = {s = i, e = closep + 1, text = line:sub(i + 1, closeb - 1), hl = 'Blue'}\n"
    "            i = closep + 1\n"
    "          else i = i + 1 end\n"
    "        elseif closeb and after == '[' then\n"
    "          local closer2 = line:find(']', closeb + 2, true)\n"
    "          if closer2 then\n"
    "            spans[#spans + 1] = {s = i, e = closer2 + 1, text = line:sub(i + 1, closeb - 1), hl = 'Blue'}\n"
    "            i = closer2 + 1\n"
    "          else i = i + 1 end\n"
    "        else i = i + 1 end\n"
    "      else i = i + 1 end\n"
    "    end\n"
    "  end\n"
    "  return spans\n"
    "end\n"
    "function mep.md_conceal()\n"
    "  if not mep_md_conceal_ns then mep_md_conceal_ns = mep.ns_create('markdown-conceal') end\n"
    "  mep.ns_clear(mep_md_conceal_ns)\n"
    "  if not mep.md_conceal_auto then return end\n"
    "  local ft = mep_lsp_filetype(mep.filename())\n"
    "  if ft ~= 'md' and ft ~= 'markdown' then return end\n"
    "  local cur_row = mep.cursor()\n"
    "  local in_fence = false\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    if line:match('^```') then\n"
    "      in_fence = not in_fence\n"
    "    elseif not in_fence and i ~= cur_row then\n"
    "      for _, sp in ipairs(mep_md_conceal_spans(line)) do\n"
    "        mep.deco_add(mep_md_conceal_ns, {\n"
    "          row = i, col_start = sp.s, col_end = sp.e,\n"
    "          virt_text = sp.text, virt_text_hl = sp.hl, virt_overlay = true, priority = 10,\n"
    "        })\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepMdConceal', mep.md_conceal)\n"
    "mep.on_buffer_changed(function() if mep.md_conceal_auto then mep.md_conceal() end end)\n"
    "local mep_md_conceal_last_row, mep_md_conceal_last_file = nil, nil\n"
    // Frame-polled (same idiom as mep.syntax_auto's own file-switch
    // watcher just above kBuiltinMarkdown's load site): on_buffer_changed
    // alone only fires on edits, not on plain cursor movement, and
    // concealment specifically needs to react to the cursor leaving/
    // entering a line even with zero edits.
    "mep.on_frame(function()\n"
    "  if not mep.md_conceal_auto then return end\n"
    "  local fname = mep.filename()\n"
    "  local row = mep.cursor()\n"
    "  if fname ~= mep_md_conceal_last_file or row ~= mep_md_conceal_last_row then\n"
    "    mep_md_conceal_last_file, mep_md_conceal_last_row = fname, row\n"
    "    mep.md_conceal()\n"
    "  end\n"
    "end)\n";

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
    // Repeater UI, part 1/2 (part 2 is mep_org_repeater_bump below):
    // cycling a headline whose SCHEDULED/DEADLINE planning line carries a
    // repeater (`+1w` etc.) into the *final* configured keyword doesn't
    // stick as done -- it bumps the repeater to its next occurrence and
    // resets the state to the *first* keyword instead, matching real
    // org-mode's org-todo-repeat behavior (marking a recurring TODO done
    // reschedules it rather than completing it). Only the immediately-
    // following planning line is checked (the only place this codebase's
    // own org_set_planning ever writes one), and only the transition INTO
    // the last keyword is intercepted -- cycling among earlier keywords,
    // or past the last one to clear it, is unaffected.\n"
    "  local last_kw = mep.org_todo_keywords[#mep.org_todo_keywords]\n"
    "  if next_kw and next_kw == last_kw and h.todo ~= last_kw then\n"
    "    local planning_row = row + 1\n"
    "    local planning_line = mep.get_line(planning_row) or ''\n"
    "    local advanced = false\n"
    "    for _, kind in ipairs({'SCHEDULED', 'DEADLINE'}) do\n"
    "      local s, e = planning_line:find(kind .. ':%s*<[^>]+>')\n"
    "      if s then\n"
    "        local inner = planning_line:match(kind .. ':%s*<([^>]+)>')\n"
    "        local newbody = mep_org_repeater_bump(inner)\n"
    "        if newbody then\n"
    "          planning_line = planning_line:sub(1, s - 1) .. kind .. ': <' .. newbody .. '>' .. planning_line:sub(e + 1)\n"
    "          advanced = true\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "    if advanced then\n"
    "      mep.set_line(planning_row, planning_line)\n"
    "      local stars, rest = mep.get_line(row):match('^(%*+%s+)(.*)$')\n"
    "      if h.todo then rest = rest:sub(#h.todo + 2) end\n"
    "      mep.set_line(row, stars .. mep.org_todo_keywords[1] .. ' ' .. rest)\n"
    "      mep.notify('Repeating task rescheduled')\n"
    "      return\n"
    "    end\n"
    "  end\n"
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
    "end\n"
    // Repeater UI, part 2/2 (part 1 is org_todo_cycle's repeater-detection
    // block above): given a timestamp *body* (no brackets, e.g. "2026-08-
    // 19 Wed +1w"), advance it by exactly one repeater interval, then keep
    // adding intervals until reaching today-or-later -- unlike
    // org_next_occurrence (which only "catches up" a date that's already
    // in the past, and leaves a future date untouched), this always
    // advances by at least one interval, matching real org-mode's mark-
    // done behavior of always moving a repeating timestamp forward even
    // when it's not yet due.
    "function mep_org_repeater_bump(ts_body)\n"
    "  local y, mo, d = ts_body:match('(%d%d%d%d)-(%d%d)-(%d%d)')\n"
    "  if not y then return nil end\n"
    "  local rep = ts_body:match('([%+%.]+%d+[dwmy])')\n"
    "  if not rep then return nil end\n"
    "  local days = mep_org_repeater_days(rep)\n"
    "  if not days then return nil end\n"
    "  local t = os.time({year = tonumber(y), month = tonumber(mo), day = tonumber(d), hour = 12}) + days * 86400\n"
    "  local today = os.date('%Y-%m-%d')\n"
    "  while os.date('%Y-%m-%d', t) < today do t = t + days * 86400 end\n"
    "  return os.date('%Y-%m-%d %a', t) .. ' ' .. rep\n"
    "end\n"
    // List-editing helpers (bulleted -/+/indented-* and ordered 1./1)
    // items): pure line-pattern parsing, same style as headlines. A `*`
    // bullet must be indented (mirrors the headline `*`-at-column-0 rule)
    // to disambiguate from a headline; `-`/`+` have no such restriction.
    // Fills the "list-editing helpers" gap this file's own plan notes
    // flagged as the lowest-value remaining Phase 29 item -- now closed.
    "local ORG_LIST_ORDERED = '^(%s*)(%d+)([%.%)])%s+(.*)$'\n"
    "local ORG_LIST_DASH_PLUS = '^(%s*)([%-%+])%s+(.*)$'\n"
    "local ORG_LIST_STAR = '^(%s+)(%*)%s+(.*)$'\n"
    "function mep_org_parse_list_item(line)\n"
    "  local indent, num, sep, content = line:match(ORG_LIST_ORDERED)\n"
    "  if indent then return {indent = indent, kind = 'ordered', number = tonumber(num), sep = sep, content = content} end\n"
    "  local dindent, marker, dcontent = line:match(ORG_LIST_DASH_PLUS)\n"
    "  if dindent then return {indent = dindent, kind = 'bullet', marker = marker, content = dcontent} end\n"
    "  local sindent, smarker, scontent = line:match(ORG_LIST_STAR)\n"
    "  if sindent then return {indent = sindent, kind = 'bullet', marker = smarker, content = scontent} end\n"
    "  return nil\n"
    "end\n"
    "local function mep_org_list_render_marker(item)\n"
    "  if item.kind == 'ordered' then return item.indent .. item.number .. item.sep .. ' ' end\n"
    "  return item.indent .. item.marker .. ' '\n"
    "end\n"
    "local function mep_org_indent_len(line) return #(line:match('^(%s*)') or '') end\n"
    // Renumber the contiguous run of ordered-list siblings (same indent)
    // starting at the first one at/before `row`, from 1. A more-deeply-
    // indented line (nested list/continuation text) is treated as part of
    // the sibling above it rather than a run-breaking sibling itself.
    // Deliberately simpler than real org-mode: no blank-line-gap
    // tolerance, always restarts from 1.
    "function mep.org_list_renumber(row)\n"
    "  row = row or mep.cursor()\n"
    "  local item = mep_org_parse_list_item(mep.get_line(row))\n"
    "  if not item or item.kind ~= 'ordered' then mep.notify('Not on an ordered list item', 'warn') return nil end\n"
    "  local indent_len = #item.indent\n"
    "  local first = row\n"
    "  while first > 1 do\n"
    "    local prev_line = mep.get_line(first - 1)\n"
    "    local prev = mep_org_parse_list_item(prev_line)\n"
    "    if prev and prev.kind == 'ordered' and prev.indent == item.indent then first = first - 1\n"
    "    elseif prev_line ~= '' and mep_org_indent_len(prev_line) > indent_len then first = first - 1\n"
    "    else break end\n"
    "  end\n"
    "  local n, i = 0, first\n"
    "  while i <= mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    local it = mep_org_parse_list_item(line)\n"
    "    if it and it.kind == 'ordered' and it.indent == item.indent then\n"
    "      n = n + 1\n"
    "      local rendered = item.indent .. n .. it.sep .. ' ' .. it.content\n"
    "      if rendered ~= line then mep.set_line(i, rendered) end\n"
    "      i = i + 1\n"
    "    elseif line ~= '' and mep_org_indent_len(line) > indent_len then\n"
    "      i = i + 1\n"
    "    else\n"
    "      break\n"
    "    end\n"
    "  end\n"
    "  return n\n"
    "end\n"
    "mep.command('MepOrgListRenumber', function() mep.org_list_renumber() end)\n"
    // Indent/outdent the item at `row` (default cursor), and any more-
    // deeply-indented continuation lines directly beneath it, by one unit
    // (2 spaces) -- distinct from Tab/Shift-Tab's *headline* promote/
    // demote, this only ever touches plain-list markers.
    "function mep.org_list_shift_item(row, direction)\n"
    "  row = row or mep.cursor()\n"
    "  local item = mep_org_parse_list_item(mep.get_line(row))\n"
    "  if not item then mep.notify('Not on a list item', 'warn') return nil end\n"
    "  if direction < 0 and item.indent == '' then mep.notify('Already at left margin', 'warn') return nil end\n"
    "  local base_indent_len = #item.indent\n"
    "  local last, i = row, row + 1\n"
    "  while i <= mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    if line == '' or mep_org_indent_len(line) <= base_indent_len then break end\n"
    "    last = i\n"
    "    i = i + 1\n"
    "  end\n"
    "  for j = row, last do\n"
    "    local line = mep.get_line(j)\n"
    "    mep.set_line(j, direction > 0 and ('  ' .. line) or (line:gsub('^%s%s?', '', 1)))\n"
    "  end\n"
    "  return last - row + 1\n"
    "end\n"
    "mep.command('MepOrgListIndent', function() mep.org_list_shift_item(mep.cursor(), 1) end)\n"
    "mep.command('MepOrgListOutdent', function() mep.org_list_shift_item(mep.cursor(), -1) end)\n"
    // Insert a new list item below the cursor's item, preserving marker
    // style: same bullet char for -/+/*, next number (+ renumbering the
    // rest of the run) for an ordered item, a fresh unchecked [ ] for a
    // checkbox item. An empty current item (no text after its marker)
    // exits the list instead, matching real org-mode's own Enter-on-
    // empty-item behavior. Command-triggered rather than an Insert-mode
    // <CR> intercept -- this engine's mep.map only binds single keys in
    // Normal/Visual mode, the same constraint org_expand_template's own
    // comment already documents for easy-templates.\n"
    "function mep.org_list_new_item()\n"
    "  local row = mep.cursor()\n"
    "  local item = mep_org_parse_list_item(mep.get_line(row))\n"
    "  if not item then mep.notify('Not on a list item', 'warn') return end\n"
    "  local without_checkbox = item.content:gsub('^%[[ xX]%]%s*', '')\n"
    "  if without_checkbox:match('^%s*$') then\n"
    "    mep.set_line(row, '')\n"
    "    mep.set_cursor(row, 1)\n"
    "    return\n"
    "  end\n"
    "  local new_item = {indent = item.indent, kind = item.kind, marker = item.marker, sep = item.sep,\n"
    "    number = item.kind == 'ordered' and (item.number + 1) or nil}\n"
    "  local marker = mep_org_list_render_marker(new_item)\n"
    "  if item.content:match('^%[[ xX]%]') then marker = marker .. '[ ] ' end\n"
    "  mep.replace_lines(row + 1, row + 1, {marker})\n"
    "  mep.set_cursor(row + 1, #marker + 1)\n"
    "  if item.kind == 'ordered' then mep.org_list_renumber(row + 1) end\n"
    "end\n"
    "mep.command('MepOrgListNewItem', mep.org_list_new_item)\n"
    // Insert-sibling-heading (org-insert-heading[-respect-content]): a new
    // empty headline at the *same level* as the one containing the
    // cursor, placed after that headline's whole subtree (not just at the
    // cursor) so nested children aren't disrupted -- fills the "insert
    // sibling not bound to a dedicated command" gap this file's own plan
    // notes flagged.\n"
    "function mep.org_insert_heading(todo)\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return end\n"
    "  local level = mep.org_headline_level(row)\n"
    "  local insert_at = mep_org_subtree_end(row)\n"
    "  local line = string.rep('*', level) .. (todo and (' ' .. todo .. ' ') or ' ')\n"
    "  mep.replace_lines(insert_at, insert_at, {line})\n"
    "  mep.set_cursor(insert_at, #line + 1)\n"
    "end\n"
    "mep.command('MepOrgInsertHeading', function() mep.org_insert_heading() end)\n"
    "mep.command('MepOrgInsertTodoHeading', function() mep.org_insert_heading(mep.org_todo_keywords[1]) end)\n";

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
    // The target prompt below defaults to the most recently org_store_
    // link-ed target, if any -- see mep.org_store_link further down.
    "function mep.org_link_insert()\n"
    "  local default_target = (mep.org_stored_link and mep.org_stored_link.target) or ''\n"
    "  mep.ui_input('Link target:', default_target, function(target)\n"
    "    if not target or target == '' then return end\n"
    "    mep.ui_input('Description (optional):', '', function(desc)\n"
    "      local text = (desc and desc ~= '') and ('[[' .. target .. '][' .. desc .. ']]') or ('[[' .. target .. ']]')\n"
    "      mep.insert_text(text)\n"
    "    end)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgLinkInsert', mep.org_link_insert)\n"
    // Store-link (org-store-link): capture a link to the headline
    // containing the cursor for later recall as org_link_insert's default
    // target prompt. Prefers a CUSTOM_ID, then an ID, then falls back to
    // a "*Title" heading-search link -- same preference order real org-
    // mode uses. Scoped down to a single most-recently-stored slot rather
    // than a full link ring/history list (documented scope cut -- a real
    // ring would need its own picker UI with no obvious place to surface
    // it in this codebase's ex-command-driven org module).\n"
    "mep.org_stored_link = nil\n"
    "function mep.org_store_link()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not inside a headline', 'warn') return end\n"
    "  local h = mep_org_parse_headline(mep.get_line(row))\n"
    "  local custom_id = mep.org_property_get(row, 'CUSTOM_ID')\n"
    "  local id = not custom_id and mep.org_property_get(row, 'ID')\n"
    "  local target\n"
    "  if custom_id then target = '#' .. custom_id\n"
    "  elseif id then target = 'id:' .. id\n"
    "  else target = '*' .. h.title end\n"
    "  mep.org_stored_link = {target = target, description = h.title}\n"
    "  mep.notify('Stored link: ' .. target)\n"
    "end\n"
    "mep.command('MepOrgStoreLink', mep.org_store_link)\n"
    // Follow: dispatch by target-type prefix. Citations (org-cite
    // `[cite:@key]` or a legacy org-ref `citeTYPE:key` -- Phase 39's
    // mep_org_bib_cite_at_cursor, a cross-chunk global defined in
    // kBuiltinOrgBib, loaded after this chunk but resolved by the time
    // this function is actually called) are checked first since they
    // don't use the `[[...]]` bracket form below.
    "function mep.org_link_follow()\n"
    "  local cite_keys = mep_org_bib_cite_at_cursor and mep_org_bib_cite_at_cursor()\n"
    "  if cite_keys and #cite_keys > 0 then mep.org_bib_cite_goto() return end\n"
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
    // Repeater-insert UI: mep.org_timestamp_insert only ever writes a bare
    // date (no repeater), and org_timestamp_shift only preserves an
    // *existing* repeater -- there was no way to add one in the first
    // place. Prompts for a repeater string (e.g. "+1w", "++1d", ".+1m",
    // or empty to remove one) and rewrites the timestamp under the
    // cursor with it.\n"
    "function mep.org_timestamp_set_repeater()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  local s, e, body, active = mep_org_timestamp_at(line, col)\n"
    "  if not s then mep.notify('No timestamp under cursor', 'warn') return end\n"
    "  local base = body:gsub('%s*[%+%.]+%d+[dwmy]%s*$', '')\n"
    "  local existing = body:match('([%+%.]+%d+[dwmy])') or ''\n"
    "  mep.ui_input('Repeater (e.g. +1w, ++1d, .+1m; empty to remove):', existing, function(rep)\n"
    "    if not rep then return end\n"
    "    if rep ~= '' and not rep:match('^[%+%.]+%d+[dwmy]$') then mep.notify('Invalid repeater syntax', 'warn') return end\n"
    "    local newbody = base .. (rep ~= '' and (' ' .. rep) or '')\n"
    "    local openc, closec = active and '<' or '[', active and '>' or ']'\n"
    "    local cur = mep.get_line(row)\n"
    "    mep.set_line(row, cur:sub(1, s - 1) .. openc .. newbody .. closec .. cur:sub(e + 1))\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgTimestampRepeater', mep.org_timestamp_set_repeater)\n"
    // Timestamp ranges: <start>--<end> (or [start]--[end]). Each half is
    // still just an ordinary timestamp for insert/shift purposes --
    // mep_org_timestamp_at above already matches each bracketed half
    // independently of the '--' connector between them, so
    // MepOrgTimestampIncr/Decr already work correctly on either half of a
    // range with no changes needed there. What's added here: a dedicated
    // insert command (prompting for both ends) and highlighting that
    // marks a whole recognized range with a color distinct from a lone
    // timestamp.\n"
    "local ORG_TS_RANGE_PATTERNS = {\n"
    "  '<%d%d%d%d%-%d%d%-%d%d[^>]->%-%-<%d%d%d%d%-%d%d%-%d%d[^>]->',\n"
    "  '%[%d%d%d%d%-%d%d%-%d%d[^%]]-%]%-%-%[%d%d%d%d%-%d%d%-%d%d[^%]]-%]',\n"
    "}\n"
    "function mep_org_timestamp_range_at(line, col)\n"
    "  for _, pat in ipairs(ORG_TS_RANGE_PATTERNS) do\n"
    "    local pos = 1\n"
    "    while true do\n"
    "      local s, e = line:find(pat, pos)\n"
    "      if not s then break end\n"
    "      if col >= s and col <= e then return s, e end\n"
    "      pos = e + 1\n"
    "    end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function mep.org_timestamp_insert_range()\n"
    "  local row, col = mep.cursor()\n"
    "  local today = os.date('%Y-%m-%d')\n"
    "  mep.ui_input('Range start (YYYY-MM-DD):', today, function(startd)\n"
    "    if not startd then return end\n"
    "    mep.ui_input('Range end (YYYY-MM-DD):', today, function(endd)\n"
    "      if not endd then return end\n"
    "      local sy, sm, sd = startd:match('(%d+)-(%d+)-(%d+)')\n"
    "      local ey, em, ed = endd:match('(%d+)-(%d+)-(%d+)')\n"
    "      if not sy or not ey then mep.notify('Invalid date (want YYYY-MM-DD)', 'warn') return end\n"
    "      local sw = os.date('%a', os.time({year = tonumber(sy), month = tonumber(sm), day = tonumber(sd), hour = 12}))\n"
    "      local ew = os.date('%a', os.time({year = tonumber(ey), month = tonumber(em), day = tonumber(ed), hour = 12}))\n"
    "      local text = '<' .. startd .. ' ' .. sw .. '>--<' .. endd .. ' ' .. ew .. '>'\n"
    "      local line = mep.get_line(row)\n"
    "      mep.set_line(row, line:sub(1, col - 1) .. text .. line:sub(col))\n"
    "      mep.set_cursor(row, col + #text)\n"
    "    end)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgTimestampRange', mep.org_timestamp_insert_range)\n"
    // Highlighting: a whole recognized range gets one color (Cyan),
    // any other single active/inactive timestamp gets another (Blue) --
    // same command-triggered "recolor the span" approach as org_link_
    // highlight above (not wired to an automatic per-keystroke render
    // pipeline; run it explicitly, or from wherever a caller already
    // re-renders org decorations).\n"
    "local mep_org_ts_ns = nil\n"
    "function mep.org_timestamp_highlight()\n"
    "  if not mep_org_ts_ns then mep_org_ts_ns = mep.ns_create('org-timestamps') end\n"
    "  mep.ns_clear(mep_org_ts_ns)\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line = mep.get_line(i)\n"
    "    local covered = {}\n"
    "    for _, pat in ipairs(ORG_TS_RANGE_PATTERNS) do\n"
    "      local pos = 1\n"
    "      while true do\n"
    "        local s, e = line:find(pat, pos)\n"
    "        if not s then break end\n"
    "        mep.deco_add(mep_org_ts_ns, {row = i, col_start = s, col_end = e + 1, hl = 'Cyan'})\n"
    "        for k = s, e do covered[k] = true end\n"
    "        pos = e + 1\n"
    "      end\n"
    "    end\n"
    "    for _, pat in ipairs({'<%d%d%d%d%-%d%d%-%d%d[^>]->', '%[%d%d%d%d%-%d%d%-%d%d[^%]]-%]'}) do\n"
    "      local pos = 1\n"
    "      while true do\n"
    "        local s, e = line:find(pat, pos)\n"
    "        if not s then break end\n"
    "        if not covered[s] then mep.deco_add(mep_org_ts_ns, {row = i, col_start = s, col_end = e + 1, hl = 'Blue'}) end\n"
    "        pos = e + 1\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepOrgTimestampHighlight', mep.org_timestamp_highlight)\n"
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
    // Review step: the captured entry is inserted immediately (so it's
    // fully editable in a real buffer -- there's no floating-editable-
    // popup primitive at the Lua/C++ layer, only single-line ui_input/
    // modal ui_select/ui_confirm per Phase 3, so this is the closest
    // faithful adaptation of real org-capture's own insert-then-C-c C-c-
    // or-C-c C-k review step), but is tracked as *pending* until
    // explicitly committed or aborted rather than being final the moment
    // prompts resolve.\n"
    "mep.org_capture_pending = nil\n"
    "    local function finish(text)\n"
    "      text = text:gsub('%%%?', '')\n"
    "      local target_file = tmpl.file or mep.filename()\n"
    "      if target_file ~= mep.filename() then mep.pane_open(target_file) end\n"
    "      local n = mep.line_count()\n"
    "      local lines = {}\n"
    "      for line in (text .. '\\n'):gmatch('(.-)\\n') do lines[#lines + 1] = line end\n"
    "      mep.replace_lines(n + 1, n + 1, lines)\n"
    "      mep.set_cursor(n + 1, 1)\n"
    "      mep.org_capture_pending = {file = target_file, start_row = n + 1, end_row = n + 1 + #lines}\n"
    "      mep.notify('Captured to ' .. target_file .. ' -- review, then :MepOrgCaptureCommit or :MepOrgCaptureAbort')\n"
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
    // Commit: keep the pending capture as-is (the buffer already holds
    // whatever the user edited it to during review) -- just clears the
    // pending marker. Abort: deletes exactly the inserted line range,
    // discarding it entirely.\n"
    "function mep.org_capture_commit()\n"
    "  if not mep.org_capture_pending then mep.notify('No pending capture', 'warn') return end\n"
    "  mep.org_capture_pending = nil\n"
    "  mep.notify('Capture committed')\n"
    "end\n"
    "function mep.org_capture_abort()\n"
    "  local p = mep.org_capture_pending\n"
    "  if not p then mep.notify('No pending capture', 'warn') return end\n"
    "  if mep.filename() ~= p.file then mep.pane_open(p.file) end\n"
    "  mep.replace_lines(p.start_row, p.end_row, {})\n"
    "  mep.org_capture_pending = nil\n"
    "  mep.notify('Capture aborted')\n"
    "end\n"
    "mep.command('MepOrgCaptureCommit', mep.org_capture_commit)\n"
    "mep.command('MepOrgCaptureAbort', mep.org_capture_abort)\n"
    // Refile: move the current subtree to become the last child of a
    // headline chosen via a fuzzy picker (mep.picker_open, the same
    // engine find_files/live_grep/buffers use -- already a real
    // completion-based target picker, not a flat prompt), re-leveling it
    // to fit one level deeper. Targets include every other headline in
    // the current buffer *plus* every headline in mep.org_refile_files
    // (an opt-in list of other paths, empty by default, same convention
    // as Phase 32's mep.org_agenda_files) read headlessly via Phase 32's
    // mep_org_read_file_lines -- closing the "same buffer only" scope cut
    // this file's own plan notes flagged. Cross-file items are prefixed
    // with their filename; picker `data` can only be a string (see
    // ReadPickerItems in lua_env.cpp), so the target file (empty string
    // for same-buffer) and row are packed into one string joined by a
    // literal \\1 byte that can never appear in a path or line number.
    // Also "refile and follow": the cursor ends up at the refiled
    // subtree's new location rather than staying at the (now-empty) old
    // spot.\n"
    "mep.org_refile_files = mep.org_refile_files or {}\n"
    "function mep.org_refile()\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return end\n"
    "  local e = mep_org_subtree_end(row)\n"
    "  local src_file = mep.filename()\n"
    "  local items = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    if i < row or i >= e then\n"
    "      local h = mep_org_parse_headline(mep.get_line(i))\n"
    "      if h then items[#items + 1] = {display = string.rep('  ', h.level - 1) .. h.title, data = '\\1' .. i} end\n"
    "    end\n"
    "  end\n"
    "  for _, path in ipairs(mep.org_refile_files) do\n"
    "    if path ~= src_file then\n"
    "      local flines = mep_org_read_file_lines(path)\n"
    "      if flines then\n"
    "        for i, line in ipairs(flines) do\n"
    "          local h = mep_org_parse_headline(line)\n"
    "          if h then items[#items + 1] = {display = '[' .. path .. '] ' .. string.rep('  ', h.level - 1) .. h.title, data = path .. '\\1' .. i} end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No refile targets', 'warn') return end\n"
    "  local lines = {}\n"
    "  for i = row, e - 1 do lines[#lines + 1] = mep.get_line(i) end\n"
    "  local src_level = mep_org_parse_headline(lines[1]).level\n"
    "  mep.picker_open('Refile to', items, function(data)\n"
    "    if not data then return end\n"
    "    local target_file, target_row_s = data:match('^(.-)\\1(%d+)$')\n"
    "    local target_row = tonumber(target_row_s)\n"
    "    local function reindent(target_level)\n"
    "      local delta = (target_level + 1) - src_level\n"
    "      local out = {}\n"
    "      for _, line in ipairs(lines) do\n"
    "        local stars = line:match('^(%*+)')\n"
    "        if stars and delta ~= 0 then\n"
    "          out[#out + 1] = string.rep('*', math.max(1, #stars + delta)) .. line:sub(#stars + 1)\n"
    "        else\n"
    "          out[#out + 1] = line\n"
    "        end\n"
    "      end\n"
    "      return out\n"
    "    end\n"
    "    if target_file == '' then\n"
    "      local target_level = mep.org_headline_level(target_row)\n"
    "      local target_end = mep_org_subtree_end(target_row)\n"
    "      local reindented = reindent(target_level)\n"
    "      local dest_row\n"
    "      if target_row > row then\n"
    "        mep.replace_lines(target_end, target_end, reindented)\n"
    "        mep.replace_lines(row, e, {})\n"
    "        dest_row = target_end - (e - row)\n"
    "      else\n"
    "        mep.replace_lines(row, e, {})\n"
    "        mep.replace_lines(target_end, target_end, reindented)\n"
    "        dest_row = target_end\n"
    "      end\n"
    "      mep.set_cursor(dest_row, 1)\n"
    "      mep.notify('Refiled')\n"
    "    else\n"
    "      mep.replace_lines(row, e, {})\n"
    "      mep.pane_open(target_file)\n"
    "      local target_level = mep.org_headline_level(target_row)\n"
    "      local target_end = mep_org_subtree_end(target_row)\n"
    "      local reindented = reindent(target_level)\n"
    "      mep.replace_lines(target_end, target_end, reindented)\n"
    "      mep.set_cursor(target_end, 1)\n"
    "      mep.notify('Refiled to ' .. target_file)\n"
    "    end\n"
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
    // Grown from the original 4 per NVIM_PARITY_PLAN.md's own "start with
    // 2-4 ... then grow the table" guidance (mep.nvim supports ~25). Keyed
    // by the literal `#+begin_src <lang>` header word org files actually
    // write (not a file extension, so no alias layer is needed the way
    // mep.run_languages/mep.repl_languages above needed one). Still
    // interpreted-language-only, matching this phase's own documented
    // scope cut (no `:tangle`/`compile_cmd` step exists yet for a
    // compiled language like C/Rust/Go to build before running).\n"
    "  ruby = {'ruby'}, perl = {'perl'}, php = {'php'}, javascript = {'node'},\n"
    "  r = {'Rscript'}, julia = {'julia'},\n"
    "}\n"
    "local mep_org_babel_cache = mep.babel_cache_load()\n"
    "local function mep_org_babel_cache_save() mep.babel_cache_save(mep_org_babel_cache) end\n"
    // `:var name=value` parsing (real bug fix): a value may be
    // double-quoted (`"..."`, with `\"`/`\\` escapes, so it can contain
    // spaces/quotes) or a bare non-space token, mirroring real Org's
    // :var syntax closely enough for babel's purposes -- the previous
    // `%S+`-only pattern silently truncated any value containing a
    // space.
    "local function mep_org_parse_vars(args_str)\n"
    "  local vars = {}\n"
    "  local pos = 1\n"
    "  while true do\n"
    "    local s, e, name = args_str:find(':var%s+([%w_]+)=', pos)\n"
    "    if not s then break end\n"
    "    local vstart = e + 1\n"
    "    if args_str:sub(vstart, vstart) == '\"' then\n"
    "      local buf, j = {}, vstart + 1\n"
    "      while j <= #args_str do\n"
    "        local c = args_str:sub(j, j)\n"
    "        if c == '\\\\' and j < #args_str then\n"
    "          buf[#buf + 1] = args_str:sub(j + 1, j + 1)\n"
    "          j = j + 2\n"
    "        elseif c == '\"' then\n"
    "          j = j + 1\n"
    "          break\n"
    "        else\n"
    "          buf[#buf + 1] = c\n"
    "          j = j + 1\n"
    "        end\n"
    "      end\n"
    "      vars[name] = table.concat(buf)\n"
    "      pos = j\n"
    "    else\n"
    "      local vs, ve, tok = args_str:find('(%S+)', vstart)\n"
    "      vars[name] = tok or ''\n"
    "      pos = ve and (ve + 1) or vstart\n"
    "    end\n"
    "  end\n"
    "  return vars\n"
    "end\n"
    // Per-language literal-string encoding (real bug fix): the prelude
    // used to splice a :var value into the generated script via naive
    // concatenation with no quoting at all (`name = <value>`) -- besides
    // breaking on spaces/quotes, an unquoted value was live code, i.e. a
    // code-injection hole (`:var x=os.execute('rm -rf ~')` ran
    // literally). Every value is now always encoded as that language's
    // own quoted string-literal syntax, so it can only ever be data.
    "local function mep_org_quote_value(lang, val)\n"
    "  val = tostring(val)\n"
    "  if lang == 'sh' or lang == 'bash' then\n"
    "    return \"'\" .. val:gsub(\"'\", \"'\\\\''\") .. \"'\"\n"
    "  elseif lang == 'lua' then\n"
    "    return string.format('%q', val)\n"
    "  else\n"
    "    local esc = val:gsub('\\\\', '\\\\\\\\')\n"
    "    esc = esc:gsub('\"', '\\\\\"')\n"
    "    esc = esc:gsub('\\n', '\\\\n')\n"
    "    esc = esc:gsub('\\r', '\\\\r')\n"
    "    esc = esc:gsub('\\t', '\\\\t')\n"
    "    return '\"' .. esc .. '\"'\n"
    "  end\n"
    "end\n"
    // `:results` header-arg (Phase 34 gap): space-separated mode
    // keywords captured as a set, stopping at the next `:key` header-arg
    // (or end of string) -- `silent` suppresses the #+RESULTS: block
    // entirely, `table` reformats tabular output, `value`/`output` pick
    // the collection strategy (see mep_org_value_wrap below).
    "local function mep_org_parse_results(args_str)\n"
    "  local results_str = args_str:match(':results%s+([^:]*)') or ''\n"
    "  results_str = results_str:gsub('%s+$', '')\n"
    "  local modes = {}\n"
    "  for w in results_str:gmatch('%S+') do modes[w] = true end\n"
    "  return modes\n"
    "end\n"
    "local function mep_org_src_block_at(row)\n"
    "  local start_row\n"
    "  for i = row, 1, -1 do\n"
    "    if mep.get_line(i):match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]') then start_row = i break end\n"
    "    if mep.get_line(i):match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') then return nil end\n"
    "  end\n"
    "  if not start_row then return nil end\n"
    "  local end_row\n"
    "  for i = start_row + 1, mep.line_count() do\n"
    "    if mep.get_line(i):match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') then end_row = i break end\n"
    "  end\n"
    "  if not end_row or end_row < row then return nil end\n"
    "  local header = mep.get_line(start_row)\n"
    // Directive keywords (#+begin_src/#+end_src, above) and the language
    // tag are both case-insensitive in real org-mode ("#+BEGIN_SRC Lua"
    // is exactly as valid as "#+begin_src lua") -- lowercased here so
    // mep.org_babel_langs' lookup (keyed by lowercase "lua"/"python"/etc.,
    // see mep.org_babel_execute below) still finds it regardless of how
    // the source file capitalized it. Only the language tag is
    // lowercased, not args_str -- a :var value's own case must round-trip
    // untouched.
    "  local lang = header:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+(%S+)')\n"
    "  if lang then lang = lang:lower() end\n"
    "  local args_str = header:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+%S+%s*(.*)$') or ''\n"
    "  local vars = mep_org_parse_vars(args_str)\n"
    "  local body = {}\n"
    "  for i = start_row + 1, end_row - 1 do body[#body + 1] = mep.get_line(i) end\n"
    "  return {start_row = start_row, end_row = end_row, lang = lang, vars = vars,\n"
    "    tangle = args_str:match(':tangle%s+(%S+)'), cache = args_str:match(':cache%s+(%S+)'),\n"
    "    results_modes = mep_org_parse_results(args_str),\n"
    "    args_str = args_str, body = table.concat(body, '\\n')}\n"
    "end\n"
    // `:results table` best-effort reformat: tab-separated wins over
    // comma-separated if both appear anywhere in the output; first row
    // becomes the header (with a `|---+---|` separator) when there's
    // more than one row, matching plain Org table syntax.
    "local function mep_org_format_table(lines)\n"
    "  if #lines == 0 then return lines end\n"
    "  local delim\n"
    "  for _, l in ipairs(lines) do\n"
    "    if l:find('\\t') then delim = '\\t' break end\n"
    "  end\n"
    "  if not delim then\n"
    "    for _, l in ipairs(lines) do\n"
    "      if l:find(',') then delim = ',' break end\n"
    "    end\n"
    "  end\n"
    "  if not delim then return lines end\n"
    "  local rows = {}\n"
    "  for _, l in ipairs(lines) do\n"
    "    local cells = {}\n"
    "    for cell in (l .. delim):gmatch('(.-)' .. delim) do cells[#cells + 1] = cell end\n"
    "    rows[#rows + 1] = cells\n"
    "  end\n"
    "  local function row_line(cells) return '| ' .. table.concat(cells, ' | ') .. ' |' end\n"
    "  local out = {row_line(rows[1])}\n"
    "  if #rows > 1 then\n"
    "    local seps = {}\n"
    "    for _ = 1, #rows[1] do seps[#seps + 1] = '---' end\n"
    "    out[2] = '|' .. table.concat(seps, '+') .. '|'\n"
    "    for i = 2, #rows do out[#out + 1] = row_line(rows[i]) end\n"
    "  end\n"
    "  return out\n"
    "end\n"
    // `raw` (Phase 34 gap, :results table): when true, out_lines is
    // already Org table syntax and gets inserted verbatim (no `: `
    // prefix, no #+begin_example fence) -- and the existing-block
    // detector below now also recognizes a prior table (lines starting
    // with `|`) so re-running in place replaces it instead of leaving
    // stale rows behind.
    "function mep.org_babel_insert_results(blk, out_lines, raw)\n"
    "  local insert_row = blk.end_row\n"
    "  local existing_start, existing_end\n"
    "  if (mep.get_line(insert_row + 1) or ''):match('^%s*#%+RESULTS:%s*$') then\n"
    "    existing_start = insert_row + 1\n"
    "    local i = existing_start + 1\n"
    "    local line_i = mep.get_line(i) or ''\n"
    "    if line_i:match('^%s*#%+begin_example') then\n"
    "      while i <= mep.line_count() and not (mep.get_line(i) or ''):match('^%s*#%+end_example') do i = i + 1 end\n"
    "      existing_end = i\n"
    "    elseif line_i:match('^%s*:') or line_i:match('^%s*|') then\n"
    "      while i <= mep.line_count() and ((mep.get_line(i) or ''):match('^%s*:') or (mep.get_line(i) or ''):match('^%s*|')) do i = i + 1 end\n"
    "      existing_end = i - 1\n"
    "    else\n"
    "      existing_end = existing_start\n"
    "    end\n"
    "  end\n"
    "  local block\n"
    "  if raw then\n"
    "    block = {'#+RESULTS:'}\n"
    "    for _, l in ipairs(out_lines) do block[#block + 1] = l end\n"
    "  elseif #out_lines <= 1 then\n"
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
    // `:results value` best-effort last-expression capture (Phase 34
    // gap; python/lua only -- sh/bash have no real value/output
    // distinction, same as real Org): a heuristic, not a parser -- if
    // the body's last non-blank line isn't a statement keyword, comment,
    // or (heuristically) a top-level assignment, re-evaluate it in an
    // appended `print(...)` so its value lands in the captured output.
    // Documented as heuristic/best-effort, not full AST-based analysis.
    "local mep_org_value_stmt_kw = {\n"
    "  ['if'] = true, ['for'] = true, ['while'] = true, ['def'] = true, ['class'] = true,\n"
    "  ['with'] = true, ['try'] = true, ['import'] = true, ['from'] = true, ['return'] = true,\n"
    "  ['print'] = true, ['local'] = true, ['function'] = true, ['do'] = true, ['end'] = true,\n"
    "  ['break'] = true, ['pass'] = true, ['raise'] = true, ['assert'] = true, ['del'] = true,\n"
    "  ['global'] = true, ['elif'] = true, ['else'] = true,\n"
    "}\n"
    "local function mep_org_value_wrap(lang, body)\n"
    "  local lines = {}\n"
    "  for l in (body .. '\\n'):gmatch('(.-)\\n') do lines[#lines + 1] = l end\n"
    "  local last_idx\n"
    "  for i = #lines, 1, -1 do\n"
    "    if lines[i]:match('%S') then last_idx = i break end\n"
    "  end\n"
    "  if not last_idx then return body end\n"
    "  local trimmed = lines[last_idx]:match('^%s*(.-)%s*$')\n"
    "  local first_word = trimmed:match('^([%a_][%w_]*)')\n"
    "  if trimmed == '' or trimmed:match('^#') then return body end\n"
    "  if first_word and mep_org_value_stmt_kw[first_word] then return body end\n"
    "  local stripped = trimmed:gsub('==', ''):gsub('~=', ''):gsub('<=', ''):gsub('>=', '')\n"
    "  if stripped:find('=') then return body end\n"
    "  lines[#lines + 1] = 'print(' .. trimmed .. ')'\n"
    "  return table.concat(lines, '\\n')\n"
    "end\n"
    "function mep.org_babel_execute()\n"
    "  local blk = mep_org_src_block_at(mep.cursor())\n"
    "  if not blk then mep.notify('Not in a src block', 'warn') return end\n"
    "  local cmd = mep.org_babel_langs[blk.lang]\n"
    "  if not cmd then mep.notify('No babel support for: ' .. tostring(blk.lang), 'warn') return end\n"
    "  local cache_key = blk.lang .. '|' .. blk.args_str .. '|' .. blk.body\n"
    "  if blk.cache == 'yes' and mep_org_babel_cache[cache_key] then\n"
    "    if not blk.results_modes.silent then\n"
    "      local lines = blk.results_modes.table and mep_org_format_table(mep_org_babel_cache[cache_key]) or mep_org_babel_cache[cache_key]\n"
    "      mep.org_babel_insert_results(blk, lines, blk.results_modes.table)\n"
    "    end\n"
    "    mep.notify('Babel: cached result')\n"
    "    return\n"
    "  end\n"
    "  local prelude = ''\n"
    "  for name, val in pairs(blk.vars) do\n"
    "    local quoted = mep_org_quote_value(blk.lang, val)\n"
    "    prelude = prelude .. name .. ((blk.lang == 'sh' or blk.lang == 'bash') and ('=' .. quoted) or (' = ' .. quoted)) .. '\\n'\n"
    "  end\n"
    "  local body = blk.body\n"
    "  if blk.results_modes.value and (blk.lang == 'python' or blk.lang == 'lua') then\n"
    "    body = mep_org_value_wrap(blk.lang, body)\n"
    "  end\n"
    "  local tmpfile = os.tmpname()\n"
    "  local f = io.open(tmpfile, 'w')\n"
    "  f:write(prelude .. body)\n"
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
    "      if not blk.results_modes.silent then\n"
    "        local lines = blk.results_modes.table and mep_org_format_table(out_lines) or out_lines\n"
    "        mep.org_babel_insert_results(blk, lines, blk.results_modes.table)\n"
    "      end\n"
    "      if blk.cache == 'yes' then\n"
    "        mep_org_babel_cache[cache_key] = out_lines\n"
    "        mep_org_babel_cache_save()\n"
    "      end\n"
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
    "    if mep.get_line(i):match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]') then\n"
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
    // Lines-array counterpart of mep_org_subtree_end (Phase 35 gap
    // support): the main export walk operates on a resolved-includes
    // array rather than the live buffer, so tag-based subtree skipping
    // (:noexport:) needs a version indexing into that array instead of
    // calling mep.get_line/mep.line_count.
    "local function mep_org_subtree_end_lines(lines, row)\n"
    "  local h = mep_org_parse_headline(lines[row])\n"
    "  local level = h and h.level\n"
    "  for i = row + 1, #lines do\n"
    "    local hi = mep_org_parse_headline(lines[i])\n"
    "    if hi and hi.level <= level then return i end\n"
    "  end\n"
    "  return #lines + 1\n"
    "end\n"
    // `#+INCLUDE:` resolution (Phase 35 gap): a pre-pass run before the
    // rest of export, recursively splicing in referenced files' content
    // (`"path"` or bare path; optional `:lines "N-M"` slice). Guarded
    // against runaway/circular includes two ways: `seen` tracks paths
    // currently on the active inclusion chain (pushed before recursing,
    // popped after) so a file including itself directly or transitively
    // is caught and reported inline rather than looping forever, and a
    // shared `budget` caps total resolved lines across the whole
    // resolution so a large acyclic chain can't blow up unbounded
    // either -- both failure modes bail out with a bracketed status
    // message spliced into the output rather than hanging or crashing.
    "local function mep_org_resolve_include_line(line, base_dir, depth, seen, budget)\n"
    "  local target, opts = line:match('^%s*#%+INCLUDE:%s*\"([^\"]+)\"%s*(.*)$')\n"
    "  if not target then target, opts = line:match('^%s*#%+INCLUDE:%s*(%S+)%s*(.*)$') end\n"
    "  if not target then return nil end\n"
    "  if depth > 8 then return {'[include: max depth exceeded: ' .. target .. ']'} end\n"
    "  local path = target:match('^/') and target or (base_dir .. '/' .. target)\n"
    "  if seen[path] then return {'[include: cycle detected: ' .. target .. ']'} end\n"
    "  local lines = mep_org_read_file_lines(path)\n"
    "  if not lines then return {'[include: file not found: ' .. target .. ']'} end\n"
    "  local from, to\n"
    "  local lrange = opts and opts:match(':lines%s+\"?(%d*%-%d*)\"?')\n"
    "  if lrange then\n"
    "    local f, t = lrange:match('^(%d*)%-(%d*)$')\n"
    "    from = tonumber(f)\n"
    "    to = tonumber(t)\n"
    "  end\n"
    "  local selected = {}\n"
    "  for i, l in ipairs(lines) do\n"
    "    if (not from or i >= from) and (not to or i <= to) then selected[#selected + 1] = l end\n"
    "  end\n"
    "  budget.remaining = budget.remaining - #selected\n"
    "  if budget.remaining < 0 then return {'[include: size budget exceeded: ' .. target .. ']'} end\n"
    "  seen[path] = true\n"
    "  local out = {}\n"
    "  local sub_dir = path:match('^(.*)/[^/]*$') or '.'\n"
    "  for _, l in ipairs(selected) do\n"
    "    local sub = mep_org_resolve_include_line(l, sub_dir, depth + 1, seen, budget)\n"
    "    if sub then\n"
    "      for _, sl in ipairs(sub) do out[#out + 1] = sl end\n"
    "    else\n"
    "      out[#out + 1] = l\n"
    "    end\n"
    "  end\n"
    "  seen[path] = nil\n"
    "  return out\n"
    "end\n"
    "function mep.org_resolve_includes()\n"
    "  local base_dir = (mep.filename() or ''):match('^(.*)/[^/]*$') or '.'\n"
    "  local budget = {remaining = 20000}\n"
    "  local seen = {}\n"
    "  local out = {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    local sub = mep_org_resolve_include_line(mep.get_line(i), base_dir, 1, seen, budget)\n"
    "    if sub then\n"
    "      for _, l in ipairs(sub) do out[#out + 1] = l end\n"
    "    else\n"
    "      out[#out + 1] = mep.get_line(i)\n"
    "    end\n"
    "  end\n"
    "  return out\n"
    "end\n"
    // `#+MACRO:` collection/expansion (Phase 35 gap): a `#+MACRO: name
    // body-with-$1-$2` line defines a macro; `{{{name(a,b)}}}` (or
    // `{{{name}}}` for a no-arg macro) elsewhere expands it, substituting
    // each `$N` placeholder with the Nth comma-separated argument.
    // Text-level substitution only -- no escaped-comma support within an
    // argument, matching the scope of a best-effort implementation.
    "local function mep_org_split_args(s)\n"
    "  local args = {}\n"
    "  for part in (s .. ','):gmatch('(.-),') do args[#args + 1] = part end\n"
    "  return args\n"
    "end\n"
    "local function mep_org_expand_macro_line(line, macros)\n"
    "  local expanded = line:gsub('{{{([%w_%-]+)%(([^}]*)%)}}}', function(name, argstr)\n"
    "    local def = macros[name]\n"
    "    if not def then return '{{{' .. name .. '(' .. argstr .. ')}}}' end\n"
    "    local args = mep_org_split_args(argstr)\n"
    "    return (def:gsub('%$(%d+)', function(n) return args[tonumber(n)] or '' end))\n"
    "  end)\n"
    "  expanded = expanded:gsub('{{{([%w_%-]+)}}}', function(name) return macros[name] or ('{{{' .. name .. '}}}') end)\n"
    "  return expanded\n"
    "end\n"
    "local function mep_org_collect_macros(get_line, n)\n"
    "  local macros = {}\n"
    "  for i = 1, n do\n"
    "    local name, body = get_line(i):match('^%s*#%+MACRO:%s*([%w_%-]+)%s+(.*)$')\n"
    "    if name then macros[name] = body end\n"
    "  end\n"
    "  return macros\n"
    "end\n"
    "function mep.org_export(format)\n"
    "  local marks = mep.org_export_marks[format]\n"
    "  local lines = mep.org_resolve_includes()\n"
    "  local macros = mep_org_collect_macros(function(i) return lines[i] end, #lines)\n"
    "  lines = (function()\n"
    "    local out = {}\n"
    "    for i, l in ipairs(lines) do out[i] = mep_org_expand_macro_line(l, macros) end\n"
    "    return out\n"
    "  end)()\n"
    "  local out, i, n = {}, 1, #lines\n"
    "  while i <= n do\n"
    "    local line = lines[i]\n"
    "    local h = mep_org_parse_headline(line)\n"
    "    if h then\n"
    "      if h.tags and h.tags:find('noexport', 1, true) then\n"
    "        i = mep_org_subtree_end_lines(lines, i)\n"
    "      else\n"
    "        local title = mep_org_inline_convert(format == 'html' and mep_org_html_escape(h.title) or h.title, marks)\n"
    "        out[#out + 1] = mep_org_export_heading(format, h.level, title)\n"
    "        i = i + 1\n"
    "      end\n"
    "    elseif line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]') then\n"
    "      local lang = line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+(%S+)') or ''\n"
    "      out[#out + 1] = (format == 'html') and ('<pre><code class=\"language-' .. lang .. '\">')\n"
    "        or (format == 'markdown') and ('```' .. lang) or '----'\n"
    "      i = i + 1\n"
    "      while i <= n and not lines[i]:match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') do\n"
    "        out[#out + 1] = (format == 'html') and mep_org_html_escape(lines[i]) or lines[i]\n"
    "        i = i + 1\n"
    "      end\n"
    "      out[#out + 1] = (format == 'html') and '</code></pre>' or (format == 'markdown') and '```' or '----'\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*:PROPERTIES:%s*$') then\n"
    "      while i <= n and not lines[i]:match('^%s*:END:%s*$') do i = i + 1 end\n"
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
    // level 1. Macro expansion applies here too (macros collected from
    // the whole buffer, since #+MACRO: is typically file-level); include
    // resolution does not -- kept out of scope for the narrower subtree
    // path to limit blast radius (documented cut).
    "function mep.org_export_subtree(format)\n"
    "  local row = mep_org_current_headline_row()\n"
    "  if not row then mep.notify('Not on a headline', 'warn') return nil end\n"
    "  local macros = mep_org_collect_macros(mep.get_line, mep.line_count())\n"
    "  local base_level, marks, out = mep.org_headline_level(row), mep.org_export_marks[format], {}\n"
    "  for i = row, mep_org_subtree_end(row) - 1 do\n"
    "    local line = mep_org_expand_macro_line(mep.get_line(i), macros)\n"
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
    "mep.command('MepOrgRoamNewNote', mep.org_roam_new_note)\n"
    // Backlink-graph view (NVIM_PARITY_PLAN.md Phase 37's flagged "no fuzzy
    // backlink-graph visualization" gap, closed): a real nodes+edges graph
    // rooted at the current note, rendered by main.cpp's DrawRoamGraphOverlay
    // as an actual node-link diagram (not the flat backlinks sidebar above
    // under a different name). Reuses this file's own link-scanning idea
    // (mep_org_roam_files/mep_org_read_file_lines/mep_org_roam_title_of,
    // the same helpers org_roam_backlinks uses above) rather than
    // reimplementing note discovery -- the only genuinely new parsing here
    // is extracting *every* `[[id:...]]` target out of a note's lines
    // (mep_org_roam_index below), where org_roam_backlinks above only ever
    // checked for one specific id's presence.
    //
    // Bounded to 2 hops out from the current note (hop 0) rather than the
    // whole vault at once: hop 1 is every note the current note directly
    // links to, plus every note that directly links to the current note
    // (both directions, matching what the flat backlinks sidebar already
    // considers "linking here" plus the outgoing links a reader would
    // also want to see). Hop 2 is deliberately narrower -- only the
    // *forward* links of hop-1 notes, not also their backlinks -- an
    // explicit scope cut so this stays one bounded pass over each hop-1
    // note's own already-parsed link list (O(hop1 count)) instead of a
    // second full vault scan per hop-1 note (O(hop1 count * vault size)).
    // A `MAX_NODES` cap on top of that keeps a large, densely-linked vault
    // from rendering an unreadable ring. See NVIM_PARITY_PLAN.md's Phase
    // 37 section for the full writeup of this and the ring-layout (vs.
    // force-directed) decision on the rendering side.
    "local function mep_org_roam_index()\n"
    "  local idx = {}\n"
    "  for _, path in ipairs(mep_org_roam_files()) do\n"
    "    local lines = mep_org_read_file_lines(path)\n"
    "    if lines then\n"
    "      local id\n"
    "      for i = 1, #lines do\n"
    "        if mep_org_parse_headline(lines[i]) then break end\n"
    "        local m = lines[i]:match('^%s*:ID:%s*(%S+)')\n"
    "        if m then id = m break end\n"
    "      end\n"
    "      if id then\n"
    "        local links, seen = {}, {}\n"
    "        for _, line in ipairs(lines) do\n"
    "          for lid in line:gmatch('%[%[id:([%w%-]+)') do\n"
    "            if not seen[lid] then seen[lid] = true links[#links + 1] = lid end\n"
    "          end\n"
    "        end\n"
    "        idx[id] = {path = path, title = mep_org_roam_title_of(lines) or path, links = links}\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return idx\n"
    "end\n"
    "function mep.org_roam_graph()\n"
    "  local my_id\n"
    "  for i = 1, mep.line_count() do\n"
    "    if mep.org_is_headline(i) then break end\n"
    "    my_id = mep.get_line(i):match('^%s*:ID:%s*(%S+)')\n"
    "    if my_id then break end\n"
    "  end\n"
    "  if not my_id then mep.notify('This note has no :ID: yet (run :MepOrgRoamEnsureId first)', 'warn') return end\n"
    "  local idx = mep_org_roam_index()\n"
    "  local my_lines = {}\n"
    "  for i = 1, mep.line_count() do my_lines[i] = mep.get_line(i) end\n"
    "  local my_links, seen = {}, {}\n"
    "  for _, line in ipairs(my_lines) do\n"
    "    for lid in line:gmatch('%[%[id:([%w%-]+)') do\n"
    "      if not seen[lid] then seen[lid] = true my_links[#my_links + 1] = lid end\n"
    "    end\n"
    "  end\n"
    // Override/insert our own entry from the live buffer (not the on-disk
    // copy mep_org_roam_index() just read) so an unsaved title or link
    // edit on the *current* note shows up immediately -- other notes keep
    // the same disk-read limitation org_roam_backlinks above already has.
    "  local my_title = mep_org_roam_title_of(my_lines) or mep.filename() or my_id\n"
    "  idx[my_id] = {path = mep.filename(), title = my_title, links = my_links}\n"
    "  local hop, order = {[my_id] = 0}, {my_id}\n"
    "  local function add(id, h)\n"
    "    if idx[id] and hop[id] == nil then hop[id] = h order[#order + 1] = id end\n"
    "  end\n"
    "  for _, lid in ipairs(my_links) do add(lid, 1) end\n"
    "  for oid, entry in pairs(idx) do\n"
    "    if oid ~= my_id then\n"
    "      for _, lid in ipairs(entry.links) do\n"
    "        if lid == my_id then add(oid, 1) break end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  local hop1_ids = {}\n"
    "  for id, h in pairs(hop) do if h == 1 then hop1_ids[#hop1_ids + 1] = id end end\n"
    "  for _, id in ipairs(hop1_ids) do\n"
    "    for _, lid in ipairs(idx[id].links) do add(lid, 2) end\n"
    "  end\n"
    "  local MAX_NODES = 40\n"
    "  if #order > MAX_NODES then\n"
    "    local capped = {}\n"
    "    for i = 1, MAX_NODES do capped[i] = order[i] end\n"
    "    order = capped\n"
    "  end\n"
    "  local id_to_index, nodes = {}, {}\n"
    "  for i, id in ipairs(order) do\n"
    "    id_to_index[id] = i\n"
    "    nodes[i] = {id = id, title = idx[id].title, path = idx[id].path, hop = hop[id]}\n"
    "  end\n"
    "  local edges, seen_edge = {}, {}\n"
    "  for _, id in ipairs(order) do\n"
    "    for _, lid in ipairs(idx[id].links) do\n"
    "      local a, b = id_to_index[id], id_to_index[lid]\n"
    "      if a and b and a ~= b then\n"
    "        local key = a < b and (a .. ':' .. b) or (b .. ':' .. a)\n"
    "        if not seen_edge[key] then seen_edge[key] = true edges[#edges + 1] = {a = a, b = b} end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if #nodes <= 1 then mep.notify('No linked roam notes found for this note', 'warn') return end\n"
    "  mep.roam_graph_open('Roam Graph: ' .. my_title, nodes, edges, function(path)\n"
    "    if not path then return end\n"
    "    mep.pane_open(path)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepRoamGraph', mep.org_roam_graph)\n"
    "mep.map('n', '<leader>rg', mep.org_roam_graph, {desc = 'Roam: graph view'})\n";

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
    // Top-level (brace-depth-0, quote-depth-0) splitter: used both for
    // separating a `@type{...}` body into its comma-delimited fields and
    // for `#`-concatenation splitting of a single field's value. Quote-
    // awareness matters for both: a field value like `"A, B"` must not
    // be split on the comma inside it, and a title like `"A # B"` must
    // not be split on the `#` as if it were BibTeX's concatenation
    // operator. Quotes only toggle at brace-depth 0, since a `\"` inside
    // a `{...}` group is already protected by the braces.
    "local function mep_org_bib_split_top_level(s, sep)\n"
    "  local parts, depth, in_quotes, start = {}, 0, false, 1\n"
    "  for i = 1, #s do\n"
    "    local c = s:sub(i, i)\n"
    "    if c == '\"' and depth == 0 then in_quotes = not in_quotes\n"
    "    elseif c == '{' and not in_quotes then depth = depth + 1\n"
    "    elseif c == '}' and not in_quotes then depth = depth - 1\n"
    "    elseif c == sep and depth == 0 and not in_quotes then\n"
    "      parts[#parts + 1] = s:sub(start, i - 1)\n"
    "      start = i + 1\n"
    "    end\n"
    "  end\n"
    "  parts[#parts + 1] = s:sub(start)\n"
    "  return parts\n"
    "end\n"
    // Expands a raw field-value token against the `@string{...}` macro
    // table: `{...}`/`\"...\"` literals are unwrapped as before; a bare
    // identifier (no delimiters) is looked up as a macro; `#`-joined
    // pieces (`abbrev # \", Supplement\"`) are expanded piecewise and
    // concatenated, matching BibTeX's string-concatenation operator.
    // An undefined macro name is left as literal text rather than
    // erroring, since this is a best-effort hand-rolled parser.
    "local function mep_org_bib_expand_value(val, strings)\n"
    "  local parts = mep_org_bib_split_top_level(val, '#')\n"
    "  if #parts == 1 and (val:sub(1, 1) == '{' or val:sub(1, 1) == '\"') then\n"
    "    if val:sub(1, 1) == '{' and val:sub(-1) == '}' then return val:sub(2, -2)\n"
    "    elseif val:sub(1, 1) == '\"' and val:sub(-1) == '\"' then return val:sub(2, -2)\n"
    "    else return val end\n"
    "  end\n"
    "  local buf = {}\n"
    "  for _, p in ipairs(parts) do\n"
    "    local t = p:match('^%s*(.-)%s*$')\n"
    "    if t:sub(1, 1) == '{' and t:sub(-1) == '}' then buf[#buf + 1] = t:sub(2, -2)\n"
    "    elseif t:sub(1, 1) == '\"' and t:sub(-1) == '\"' then buf[#buf + 1] = t:sub(2, -2)\n"
    "    else buf[#buf + 1] = strings[t:lower()] or t end\n"
    "  end\n"
    "  return table.concat(buf)\n"
    "end\n"
    // `strings` accumulates `@string{name = \"value\"}` macro definitions
    // as entries are scanned in file order (and across files, when the
    // caller threads the same table through multiple mep_org_bib_parse
    // calls) -- matching real BibTeX's requirement that a macro be
    // defined before first use. `@comment`/`@preamble` blocks are
    // recognized and skipped rather than mis-parsed as entries.
    "local function mep_org_bib_parse(text, strings)\n"
    "  strings = strings or {}\n"
    "  local entries = {}\n"
    "  for etype, braced in text:gmatch('@(%a+)%s*(%b{})') do\n"
    "    local body = braced:sub(2, -2)\n"
    "    local lower_type = etype:lower()\n"
    "    if lower_type == 'string' then\n"
    "      local name, val = body:match('^%s*([%w_%-]+)%s*=%s*(.-)%s*$')\n"
    "      if name then strings[name:lower()] = mep_org_bib_expand_value(val, strings) end\n"
    "    elseif lower_type ~= 'comment' and lower_type ~= 'preamble' then\n"
    "      local key, fieldstr = body:match('^%s*([^,]+),(.*)$')\n"
    "      if key then\n"
    "        local fields = {}\n"
    "        for _, part in ipairs(mep_org_bib_split_top_level(fieldstr, ',')) do\n"
    "          local name, val = part:match('^%s*([%w_%-]+)%s*=%s*(.*)$')\n"
    "          if name then\n"
    "            val = val:match('^%s*(.-)%s*$')\n"
    "            fields[name:lower()] = mep_org_bib_expand_value(val, strings)\n"
    "          end\n"
    "        end\n"
    "        entries[#entries + 1] = {type = lower_type, key = key:match('^%s*(.-)%s*$'), fields = fields}\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return entries\n"
    "end\n"
    // `crossref` resolution: run once over the *complete* entry set (all
    // resolved .bib files combined), since the referenced parent entry
    // commonly appears later in the file (e.g. an @inproceedings before
    // its @proceedings). Only fields the child doesn't already define
    // are filled in from the parent.
    "local function mep_org_bib_resolve_crossrefs(entries)\n"
    "  local by_key = {}\n"
    "  for _, e in ipairs(entries) do by_key[e.key] = e end\n"
    "  for _, e in ipairs(entries) do\n"
    "    local xref = e.fields.crossref\n"
    "    if xref then\n"
    "      local parent = by_key[xref] or by_key[xref:lower()]\n"
    "      if parent then\n"
    "        for fname, fval in pairs(parent.fields) do\n"
    "          if e.fields[fname] == nil then e.fields[fname] = fval end\n"
    "        end\n"
    "      end\n"
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
    // Loads and parses every resolved .bib file into one combined entry
    // list: `strings` (the @string macro table) is threaded across all
    // files in resolution order so a macro defined in one file is
    // usable by entries in a later one (matching a multi-file BibTeX
    // run), then mep_org_bib_resolve_crossrefs runs once over the
    // *complete* set so a crossref target defined in any file (in any
    // position) is found. Shared by insertion, goto, and preview below.
    "local function mep_org_bib_load_entries()\n"
    "  local entries, strings = {}, {}\n"
    "  for _, path in ipairs(mep.org_bib_resolve_files()) do\n"
    "    local f = io.open(path, 'r')\n"
    "    if f then\n"
    "      local text = f:read('*a')\n"
    "      f:close()\n"
    "      for _, e in ipairs(mep_org_bib_parse(text, strings)) do entries[#entries + 1] = e end\n"
    "    end\n"
    "  end\n"
    "  return mep_org_bib_resolve_crossrefs(entries)\n"
    "end\n"
    "function mep.org_bib_insert_citation()\n"
    "  local entries = mep_org_bib_load_entries()\n"
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
    "mep.command('MepOrgBibInsertCitation', mep.org_bib_insert_citation)\n"
    // Citation recognition at cursor: modern org-cite `[cite:@key]` /
    // `[cite/style:@key1;@key2]` (keys `;`-separated, each optionally
    // `@`-prefixed) *and* legacy org-ref link-type variants --
    // `cite:key`, `citep:key`, `citet:key`, `citeauthor:key`,
    // `citeyear:key` (keys `,`-separated, no `@`) -- resolve to the same
    // key list so goto/preview below work identically for either
    // syntax. No regex: a manual char-by-char scan, matching this
    // file's existing hand-rolled-parser convention (see
    // mep_org_bib_split_top_level above).
    "local mep_org_bib_legacy_prefixes = {'citeauthor', 'citeyear', 'citep', 'citet', 'cite'}\n"
    "local function mep_org_bib_is_word_char(c) return c ~= '' and c:match('[%w_]') ~= nil end\n"
    "local function mep_org_bib_is_key_char(c) return c ~= '' and c:match('[%w_%-:]') ~= nil end\n"
    "local function mep_org_bib_cite_spans(line)\n"
    "  local spans = {}\n"
    // org-cite: [cite:...] / [cite/style:...]
    "  local pos = 1\n"
    "  while true do\n"
    "    local s, e, body = line:find('%[cite[%a/]-:(.-)%]', pos)\n"
    "    if not s then break end\n"
    "    local keys = {}\n"
    "    for _, part in ipairs(mep_org_bib_split_top_level(body, ';')) do\n"
    "      local k = part:match('^%s*@?(%S+)%s*$')\n"
    "      if k then keys[#keys + 1] = k end\n"
    "    end\n"
    "    if #keys > 0 then spans[#spans + 1] = {s = s, e = e, keys = keys} end\n"
    "    pos = e + 1\n"
    "  end\n"
    // legacy org-ref: citeTYPE:key1,key2 -- not preceded by '[' (that's
    // the org-cite form above) or another word char, and not itself an
    // org-cite `@key` (org-ref keys are bare, never `@`-prefixed).
    "  local i = 1\n"
    "  while i <= #line do\n"
    "    local matched = false\n"
    "    local prev = line:sub(i - 1, i - 1)\n"
    "    if prev ~= '[' and not mep_org_bib_is_word_char(prev) then\n"
    "      for _, prefix in ipairs(mep_org_bib_legacy_prefixes) do\n"
    "        local plen = #prefix\n"
    "        if line:sub(i, i + plen - 1) == prefix and line:sub(i + plen, i + plen) == ':' then\n"
    "          local after = i + plen + 1\n"
    "          if line:sub(after, after) ~= '@' and mep_org_bib_is_key_char(line:sub(after, after)) then\n"
    "            local j, keys, key_start = after, {}, after\n"
    "            while j <= #line do\n"
    "              local c = line:sub(j, j)\n"
    "              if mep_org_bib_is_key_char(c) then j = j + 1\n"
    "              elseif c == ',' and mep_org_bib_is_key_char(line:sub(j + 1, j + 1)) then\n"
    "                keys[#keys + 1] = line:sub(key_start, j - 1)\n"
    "                j = j + 1\n"
    "                key_start = j\n"
    "              else break end\n"
    "            end\n"
    "            keys[#keys + 1] = line:sub(key_start, j - 1)\n"
    "            spans[#spans + 1] = {s = i, e = j - 1, keys = keys}\n"
    "            i = j\n"
    "            matched = true\n"
    "            break\n"
    "          end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "    if not matched then i = i + 1 end\n"
    "  end\n"
    "  table.sort(spans, function(a, b) return a.s < b.s end)\n"
    "  return spans\n"
    "end\n"
    // Global (not local) -- called from kBuiltinOrgLinks' org_link_follow
    // (a separate Lua chunk) the same way that chunk already calls the
    // other cross-chunk global mep_org_parse_headline.
    "function mep_org_bib_cite_at_cursor()\n"
    "  local row, col = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  for _, span in ipairs(mep_org_bib_cite_spans(line)) do\n"
    "    if col >= span.s and col <= span.e then return span.keys end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    // Jump-to-entry: a plain byte-search for `{key` immediately followed
    // by `,`/whitespace/end-of-file (i.e. the entry's opening line,
    // `@type{key,`) rather than a full re-parse -- cheap and avoids
    // teaching the parser to track source positions.
    "local function mep_org_bib_goto_key(key)\n"
    "  for _, path in ipairs(mep.org_bib_resolve_files()) do\n"
    "    local f = io.open(path, 'r')\n"
    "    if f then\n"
    "      local text = f:read('*a')\n"
    "      f:close()\n"
    "      local pos = 1\n"
    "      while true do\n"
    "        local s = text:find(key, pos, true)\n"
    "        if not s then break end\n"
    "        local before = text:sub(s - 1, s - 1)\n"
    "        local after = text:sub(s + #key, s + #key)\n"
    "        if before == '{' and (after == ',' or after == '' or after:match('%s')) then\n"
    "          local line = 1\n"
    "          for _ in text:sub(1, s - 1):gmatch('\\n') do line = line + 1 end\n"
    "          mep.pane_open(path)\n"
    "          mep.set_cursor(line, 1)\n"
    "          return true\n"
    "        end\n"
    "        pos = s + 1\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  return false\n"
    "end\n"
    "function mep.org_bib_cite_goto()\n"
    "  local keys = mep_org_bib_cite_at_cursor()\n"
    "  if not keys or #keys == 0 then mep.notify('No citation under cursor', 'warn') return end\n"
    "  if #keys == 1 then\n"
    "    if not mep_org_bib_goto_key(keys[1]) then mep.notify('Citation key not found in bibliography: ' .. keys[1], 'warn') end\n"
    "    return\n"
    "  end\n"
    "  local items = {}\n"
    "  for _, k in ipairs(keys) do items[#items + 1] = {display = k, data = k} end\n"
    "  mep.picker_open('Goto Citation', items, function(key)\n"
    "    if key and not mep_org_bib_goto_key(key) then mep.notify('Citation key not found in bibliography: ' .. key, 'warn') end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgBibCiteGoto', mep.org_bib_cite_goto)\n"
    // Citation preview / hover info: uses mep.hover_show (NVIM_PARITY_
    // PLAN.md Phase 3's hover-tooltip gap, closed) -- a real cursor-
    // anchored floating popup, not a toast.
    "function mep.org_bib_cite_preview()\n"
    "  local keys = mep_org_bib_cite_at_cursor()\n"
    "  if not keys or #keys == 0 then mep.notify('No citation under cursor', 'warn') return end\n"
    "  local entries = mep_org_bib_load_entries()\n"
    "  local by_key = {}\n"
    "  for _, e in ipairs(entries) do by_key[e.key] = e end\n"
    "  local lines = {}\n"
    "  for _, k in ipairs(keys) do\n"
    "    local e = by_key[k]\n"
    "    if e then\n"
    "      local f = e.fields\n"
    "      local venue = f.journal or f.booktitle\n"
    "      lines[#lines + 1] = k .. ': ' .. (f.author or '?') .. ' (' .. (f.year or 'n.d.') .. ') ' ..\n"
    "        (f.title or '') .. (venue and (' -- ' .. venue) or '')\n"
    "    else\n"
    "      lines[#lines + 1] = k .. ': not found in bibliography'\n"
    "    end\n"
    "  end\n"
    "  mep.hover_show('Citation', table.concat(lines, '\\n'))\n"
    "end\n"
    "mep.command('MepOrgBibCitePreview', mep.org_bib_cite_preview)\n";

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
    // UTF-8-encodes a single Unicode codepoint (up to the 0x10FFFF max, so
    // always 1-4 bytes) -- used by mep_ai_json_decode's \\uXXXX handling
    // below. Lua 5.4's bitwise operators make this a direct transliteration
    // of the standard UTF-8 encoding table rather than needing bit32/manual
    // arithmetic shims.
    "local function mep_ai_utf8_encode(cp)\n"
    "  if cp < 0x80 then\n"
    "    return string.char(cp)\n"
    "  elseif cp < 0x800 then\n"
    "    return string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F))\n"
    "  elseif cp < 0x10000 then\n"
    "    return string.char(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))\n"
    "  else\n"
    "    return string.char(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))\n"
    "  end\n"
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
    // \\uXXXX: decode the 4 hex digits into a codepoint. A high surrogate
    // (0xD800-0xDBFF) must be combined with an immediately-following low
    // surrogate (\\uDC00-\\uDFFF) into one real codepoint before UTF-8
    // encoding it -- JSON (like JS) represents astral characters (e.g.
    // emoji) as a surrogate *pair* of two \\u escapes, neither of which is
    // independently a valid standalone codepoint. Decoding each half on
    // its own (the bug this replaces) would silently corrupt any such
    // character. A malformed/truncated escape falls back to U+FFFD
    // (replacement character) rather than erroring the whole decode.
    "          local cp = tonumber(s:sub(pos + 2, pos + 5), 16) or 0xFFFD\n"
    "          pos = pos + 6\n"
    "          if cp >= 0xD800 and cp <= 0xDBFF and s:sub(pos, pos + 1) == '\\\\u' then\n"
    "            local lo = tonumber(s:sub(pos + 2, pos + 5), 16)\n"
    "            if lo and lo >= 0xDC00 and lo <= 0xDFFF then\n"
    "              cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)\n"
    "              pos = pos + 6\n"
    "            end\n"
    "          end\n"
    "          buf[#buf + 1] = mep_ai_utf8_encode(cp)\n"
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
    // in the Lua variable for the session -- never written to disk). The
    // fallback prompt passes opts.masked so the key is never shown in the
    // clear while being typed (main.cpp's DrawPromptOverlay renders '*'
    // in its place; the real text still reaches `cb` below unmasked).
    "local function mep_ai_get_key(cb)\n"
    "  if mep.ai_api_key then cb(mep.ai_api_key) return end\n"
    "  local env_var = mep.ai_provider == 'anthropic' and 'ANTHROPIC_API_KEY' or 'OPENAI_API_KEY'\n"
    "  local v = os.getenv(env_var)\n"
    "  if v and v ~= '' then mep.ai_api_key = v cb(v) return end\n"
    "  mep.ui_input('API key (' .. env_var .. ' not set):', '', function(key)\n"
    "    if key and key ~= '' then mep.ai_api_key = key cb(key) end\n"
    "  end, {masked = true})\n"
    "end\n"
    // mep.ai_agent_messages (and mep_ai_request's own single-shot `messages`
    // argument) are always kept in one provider-agnostic, OpenAI-Chat-
    // Completions-shaped form: {role = 'user'|'assistant'|'tool', content =
    // ..., tool_calls = {...}, tool_call_id = ...}. Anthropic's Messages
    // API has no 'tool' role and expects tool_use/tool_result as typed
    // *content blocks* on ordinary user/assistant messages instead of
    // OpenAI's flat tool_calls/role='tool' shape, so this converts one
    // shared conversation into Anthropic's wire shape just before sending
    // -- the only place that needs to know about the difference.
    "local function mep_ai_to_anthropic_messages(messages)\n"
    "  local out = {}\n"
    "  for _, m in ipairs(messages) do\n"
    "    if m.role == 'tool' then\n"
    "      out[#out + 1] = {role = 'user', content = {\n"
    "        {type = 'tool_result', tool_use_id = m.tool_call_id, content = m.content or ''}}}\n"
    "    elseif m.role == 'assistant' and m.tool_calls then\n"
    "      local blocks = {}\n"
    "      if m.content and m.content ~= '' then blocks[#blocks + 1] = {type = 'text', text = m.content} end\n"
    "      for _, tc in ipairs(m.tool_calls) do\n"
    "        blocks[#blocks + 1] = {type = 'tool_use', id = tc.id, name = tc['function'].name,\n"
    "          input = mep_ai_json_decode(tc['function'].arguments) or {}}\n"
    "      end\n"
    "      out[#out + 1] = {role = 'assistant', content = blocks}\n"
    "    else\n"
    "      out[#out + 1] = {role = m.role, content = m.content or ''}\n"
    "    end\n"
    "  end\n"
    "  return out\n"
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
    "      body = {model = mep.ai_anthropic_model, max_tokens = mep.ai_max_tokens, stream = true, messages = mep_ai_to_anthropic_messages(messages)}\n"
    "      if tools then body.tools = tools end\n"
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
    // Anthropic tool-use streams a content_block_start naming the tool
    // (id/name, empty input) at the block's index, then zero or more
    // content_block_delta{delta.type='input_json_delta'} frames whose
    // partial_json fragments concatenate into the final input JSON --
    // distinct from OpenAI's tool_calls[].function.arguments delta shape
    // above, so it needs its own accumulation path into the same shared
    // `tool_calls` table (still keyed 1-indexed by content-block index).
    "          if obj.type == 'content_block_start' and obj.content_block and obj.content_block.type == 'tool_use' then\n"
    "            local idx = (obj.index or 0) + 1\n"
    "            tool_calls[idx] = {id = obj.content_block.id, name = obj.content_block.name or '', args = ''}\n"
    "          elseif obj.type == 'content_block_delta' and obj.delta and obj.delta.type == 'input_json_delta' then\n"
    "            local idx = (obj.index or 0) + 1\n"
    "            if tool_calls[idx] then tool_calls[idx].args = tool_calls[idx].args .. (obj.delta.partial_json or '') end\n"
    "          end\n"
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
    // True Visual-mode "send selection": mep.visual_selection() only
    // returns real text while a Visual selection is still live (it reads
    // Editor::mode_ directly), and the only way a piece of Lua runs
    // *while* mep's mode_ is still Visual -- rather than after Escape/':'
    // has already dropped back to Normal, losing the selection -- is a
    // mep.map('v', ...) callback (TryLuaMapping calls straight into Lua
    // without an intervening mode change). So this is bound directly to a
    // Visual-mode key below, not just left as an ex-command someone has
    // to type ':' to reach (typing ':' would already have exited Visual
    // by the time it ran).
    "function mep.ai_send_selection()\n"
    "  local sel = mep.visual_selection()\n"
    "  if sel == '' then mep.notify('No Visual selection', 'warn') return end\n"
    "  mep.ai_send_text(sel)\n"
    "end\n"
    "mep.command('MepAiSendBuffer', mep.ai_send_buffer)\n"
    "mep.command('MepAiSendSelection', mep.ai_send_selection)\n"
    "mep.map('v', 'K', mep.ai_send_selection, {desc = 'AI: send selection'})\n"
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
    // tool-calling loop -- for both providers: mep_ai_openai_tools_schema
    // feeds OpenAI's {type='function', function={name,description,
    // parameters}} shape, mep_ai_anthropic_tools_schema below feeds
    // Anthropic's flatter {name,description,input_schema} shape (see
    // Anthropic's Messages API tool-use docs), and mep_ai_request's own
    // Anthropic branch (mep_ai_to_anthropic_messages) converts the shared
    // tool-call/tool-result history into Anthropic's content-block shape
    // so the recursive turn loop below works unmodified either way.
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
    // Anthropic's tool schema (Messages API): a flat {name, description,
    // input_schema} per tool -- no {type='function', function={...}}
    // wrapper the way OpenAI's Chat Completions API needs, and the JSON
    // Schema itself is called `input_schema` rather than `parameters`.
    // Otherwise mirrors mep_ai_openai_tools_schema's shape/purpose 1:1,
    // built from the same mep.ai_tools list plus run_command.
    "local function mep_ai_anthropic_tools_schema()\n"
    "  local out = {}\n"
    "  for _, t in ipairs(mep.ai_tools) do\n"
    "    out[#out + 1] = {name = t.name, description = t.description, input_schema = t.parameters}\n"
    "  end\n"
    "  out[#out + 1] = {name = 'run_command', description = 'Run a shell command',\n"
    "    input_schema = {type = 'object', properties = {command = {type = 'string'}}, required = {'command'}}}\n"
    "  return out\n"
    "end\n"
    "function mep.ai_agent_turn()\n"
    "  local assistant_msg = {role = 'assistant', content = ''}\n"
    "  mep.ai_agent_messages[#mep.ai_agent_messages + 1] = assistant_msg\n"
    "  mep_ai_agent_render()\n"
    "  local tools_schema = mep.ai_provider == 'anthropic' and mep_ai_anthropic_tools_schema() or mep_ai_openai_tools_schema()\n"
    "  mep_ai_request(mep.ai_agent_messages, tools_schema,\n"
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
// executing via Phase 34's babel language table.
//
// Live fetch/submit against LeetCode's own unofficial GraphQL+REST API
// (there is no public/official API for this -- every community LeetCode
// CLI/editor plugin reverse-engineers the same two surfaces; this mirrors
// mep.nvim's own mep/leetcode/api.lua closely):
//  - Fetch (mep.leetcode_fetch_problem / mep.leetcode_fetch /
//    :MepLeetcodeFetch) needs no credentials -- verified live against
//    https://leetcode.com/graphql (POST {query, variables={titleSlug}})
//    for a real, free problem ("two-sum") from this environment. Writes
//    a local .org file in the same Prompt/Solution/Tests shape
//    leetcode_run_tests above expects, stashing :SLUG:/:DIFFICULTY:/
//    :QUESTION_ID: as a property drawer under the Prompt headline
//    (mep.org_property_get/_set, Phase 32) so a later submit can find
//    them again without re-fetching.
//  - Submit (mep.leetcode_submit / :MepLeetcodeSubmit) needs a real
//    LeetCode session: mep.leetcode_session_cookie holds a raw `Cookie:`
//    header value (`LEETCODE_SESSION=...; csrftoken=...`) copied out of
//    a logged-in browser's devtools -- LeetCode has no username/password
//    login API -- prompted for once via a masked mep.ui_input and kept
//    only in memory for the session, same convention as Phase 41 AI's
//    mep_ai_get_key/mep.ai_api_key. POSTs to /problems/<slug>/submit/,
//    then polls /submissions/detail/<id>/check/ for a verdict, waiting
//    between attempts via a `sh -c 'sleep N'` job rather than a Lua
//    timer/defer binding (mep exposes none) so the wait is async instead
//    of freezing the editor. This half is implemented and its request/
//    response shapes were cross-checked against a well-known, actively
//    maintained community implementation (emacs leetcode.el) plus a
//    real captured GraphQL response, but it was NOT exercised end-to-end
//    against a live authenticated account -- no LeetCode credentials
//    were available in this environment. See NVIM_PARITY_PLAN.md's
//    Phase 42 section for the precise verified/unverified split.
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
    "        local lang = mep.get_line(j):match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+(%S+)')\n"
    "        if lang then\n"
    "          local body, k = {}, j + 1\n"
    "          while k <= e - 1 and not mep.get_line(k):match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') do\n"
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
    "mep.command('MepLeetcodeRunTests', mep.leetcode_run_tests)\n"
    "mep.leetcode_fetch_lang = 'python'\n"
    "mep.leetcode_session_cookie = nil\n"
    // Shared POST/GET-JSON-over-curl helper for both the GraphQL fetch and
    // the submit/poll REST calls below. Reuses mep_ai_json_encode/
    // mep_ai_json_decode (plain globals, not locals -- see kBuiltinAi
    // above, whose DoString runs immediately before this file's, per the
    // lua->DoString(...) sequence near the bottom of this file) rather
    // than hand-rolling a second JSON codec.
    "local function mep_leetcode_http_json(method, url, headers, body_table, cb)\n"
    "  local argv = {'curl', '-s', '--fail-with-body', '-X', method, url}\n"
    "  for _, h in ipairs(headers or {}) do argv[#argv + 1] = h end\n"
    "  local tmpfile = nil\n"
    "  if body_table then\n"
    "    argv[#argv + 1] = '-H'\n"
    "    argv[#argv + 1] = 'Content-Type: application/json'\n"
    "    tmpfile = os.tmpname()\n"
    "    local f = io.open(tmpfile, 'w')\n"
    "    f:write(mep_ai_json_encode(body_table))\n"
    "    f:close()\n"
    "    argv[#argv + 1] = '--data-binary'\n"
    "    argv[#argv + 1] = '@' .. tmpfile\n"
    "  end\n"
    "  local out = {}\n"
    "  mep.job_start(argv, {\n"
    "    on_stdout = function(l) out[#out + 1] = l end,\n"
    "    on_stderr = function(l) out[#out + 1] = l end,\n"
    "    on_exit = function(code)\n"
    "      if tmpfile then os.remove(tmpfile) end\n"
    "      local raw = table.concat(out, '\\n')\n"
    "      if code ~= 0 then\n"
    "        local decoded = mep_ai_json_decode(raw)\n"
    "        local msg = (decoded and (decoded.error or decoded.detail)) or raw\n"
    "        cb('request failed (exit ' .. code .. '): ' .. tostring(msg), nil)\n"
    "        return\n"
    "      end\n"
    "      local decoded = mep_ai_json_decode(raw)\n"
    "      if not decoded then cb('could not parse response: ' .. raw, nil) return end\n"
    "      cb(nil, decoded)\n"
    "    end,\n"
    "  })\n"
    "end\n"
    // Async sleep for the submit poll loop below, via the job system
    // rather than a real timer binding (mep exposes no defer/timer API to
    // Lua) -- a tiny `sh -c 'sleep N'` subprocess whose on_exit is the
    // resumption, so the wait doesn't block the editor's main loop.
    "local function mep_leetcode_sleep(seconds, cb)\n"
    "  mep.job_start({'sh', '-c', 'sleep ' .. tostring(seconds)}, {on_exit = function() cb() end})\n"
    "end\n"
    "local LEETCODE_QUESTION_QUERY = 'query questionData($titleSlug: String!) { question(titleSlug: $titleSlug) ' ..\n"
    "  '{ questionId title titleSlug content difficulty codeSnippets { lang langSlug code } sampleTestCase } }'\n"
    // Fetching a problem's statement/starter code needs no credentials --
    // verified live against https://leetcode.com/graphql for a free
    // problem (two-sum) from this environment. A premium-only problem or
    // a bad slug both come back as a 200 with a null `question`, handled
    // the same way below. If mep.leetcode_session_cookie is already set
    // (e.g. MepLeetcodeSubmit prompted for it earlier this session) it's
    // forwarded anyway, since a logged-in request only ever sees a
    // superset of what a logged-out one sees.
    "function mep.leetcode_fetch_problem(slug, cb)\n"
    "  local headers = {'-H', 'Referer: https://leetcode.com/problems/' .. slug .. '/'}\n"
    "  if mep.leetcode_session_cookie then\n"
    "    headers[#headers + 1] = '-H'\n"
    "    headers[#headers + 1] = 'Cookie: ' .. mep.leetcode_session_cookie\n"
    "  end\n"
    "  mep_leetcode_http_json('POST', 'https://leetcode.com/graphql', headers,\n"
    "    {query = LEETCODE_QUESTION_QUERY, variables = {titleSlug = slug}, operationName = 'questionData'},\n"
    "    function(err, decoded)\n"
    "      if err then cb('mep.leetcode: ' .. err, nil) return end\n"
    "      local question = decoded.data and decoded.data.question\n"
    "      if not question then cb('mep.leetcode: no such problem: ' .. slug, nil) return end\n"
    "      cb(nil, question)\n"
    "    end)\n"
    "end\n"
    // Session cookie: explicit override, else env var, else a masked
    // prompt -- kept only in the Lua variable for the session, never
    // written to disk. Same convention as Phase 41 AI's mep_ai_get_key.
    // The single value is the whole `Cookie:` header (LEETCODE_SESSION
    // plus csrftoken together) rather than two separate config values, so
    // it's a straight paste from the browser's Network tab.
    "local function mep_leetcode_get_cookie(cb)\n"
    "  if mep.leetcode_session_cookie then cb(mep.leetcode_session_cookie) return end\n"
    "  local v = os.getenv('LEETCODE_SESSION_COOKIE')\n"
    "  if v and v ~= '' then mep.leetcode_session_cookie = v cb(v) return end\n"
    "  mep.ui_input('LeetCode cookie (LEETCODE_SESSION=...; csrftoken=...; $LEETCODE_SESSION_COOKIE not set):', '',\n"
    "    function(val)\n"
    "      if val and val ~= '' then mep.leetcode_session_cookie = val cb(val) end\n"
    "    end, {masked = true})\n"
    "end\n"
    "local function mep_leetcode_csrf_of(cookie)\n"
    "  return cookie:match('csrftoken=([^;%s]+)')\n"
    "end\n"
    // Rough HTML -> plain-text pass over LeetCode's own `content` field:
    // turns <br>/block-closing tags into line breaks, strips every
    // remaining tag, decodes a handful of common entities, collapses
    // blank-line runs. Not a real HTML parser -- good enough for a
    // read-only problem statement dropped into an org buffer, not meant
    // to round-trip. (Mirrors mep.nvim's mep/leetcode/create.lua
    // html_to_text almost line for line.)\n"
    "local function mep_leetcode_html_to_text(html)\n"
    "  local text = html or ''\n"
    "  text = text:gsub('<[bB][rR]%s*/?>', '\\n')\n"
    "  text = text:gsub('</%a+>', '\\n')\n"
    "  text = text:gsub('<[^>]*>', '')\n"
    "  text = text:gsub('&lt;', '<'):gsub('&gt;', '>'):gsub('&amp;', '&'):gsub('&nbsp;', ' ')\n"
    "  text = text:gsub('&quot;', string.char(34)):gsub('&#39;', string.char(39))\n"
    "  local out = {}\n"
    "  for line in (text .. '\\n'):gmatch('([^\\n]*)\\n') do\n"
    "    local trimmed = line:match('^%s*(.-)%s*$')\n"
    "    if trimmed ~= '' or (out[#out] and out[#out] ~= '') then out[#out + 1] = trimmed end\n"
    "  end\n"
    "  while out[1] == '' do table.remove(out, 1) end\n"
    "  while #out > 0 and out[#out] == '' do table.remove(out) end\n"
    "  return out\n"
    "end\n"
    // LeetCode's own per-language slugs (as seen in codeSnippets[].langSlug
    // and expected back by submit) mapped to/from mep.org_babel_langs' own
    // keys -- only the languages that table actually runs (sh/bash/
    // python/lua) are considered; of those only python/bash have a real
    // LeetCode counterpart (LeetCode has no sh or Lua submissions), so a
    // fetched snippet or submit attempt in sh/lua is simply not offered,
    // the same graceful-miss every other "not in the curated set" case
    // elsewhere in this project uses.
    "local mep_leetcode_babel_to_slug = {python = 'python3', bash = 'bash'}\n"
    "local mep_leetcode_comment_prefix = {python = '# ', sh = '# ', bash = '# ', lua = '-- '}\n"
    "local function mep_leetcode_snippet_for(question, babel_lang)\n"
    "  local slug = mep_leetcode_babel_to_slug[babel_lang]\n"
    "  if not slug then return nil end\n"
    "  for _, snippet in ipairs(question.codeSnippets or {}) do\n"
    "    if snippet.langSlug == slug then return snippet.code end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    // Fetch a problem by slug or full problem URL and write
    // mep.leetcode_dir/<slug>.org from it (Prompt/Solution/Tests, same
    // shape leetcode_run_tests/leetcode_picker above already expect),
    // then open it. `lang` (default mep.leetcode_fetch_lang) picks which
    // codeSnippets entry seeds the Solution block.
    "function mep.leetcode_fetch(slug_or_url, lang)\n"
    "  lang = lang or mep.leetcode_fetch_lang or 'python'\n"
    "  local slug = (slug_or_url:match('leetcode%.com/problems/([%w%-]+)') or slug_or_url):match('^%s*(.-)%s*$')\n"
    "  slug = slug:gsub('/+$', '')\n"
    "  if not mep.leetcode_dir then mep.notify('mep.leetcode_dir not configured', 'warn') return end\n"
    "  mep.leetcode_fetch_problem(slug, function(err, question)\n"
    "    if err then mep.notify(err, 'error') return end\n"
    "    local lines = {}\n"
    "    lines[#lines + 1] = '#+TITLE: ' .. question.title\n"
    "    lines[#lines + 1] = ''\n"
    "    lines[#lines + 1] = '* Prompt'\n"
    "    lines[#lines + 1] = ':PROPERTIES:'\n"
    "    lines[#lines + 1] = ':SLUG: ' .. question.titleSlug\n"
    "    if question.difficulty and question.difficulty ~= '' then lines[#lines + 1] = ':DIFFICULTY: ' .. question.difficulty end\n"
    "    if question.questionId then lines[#lines + 1] = ':QUESTION_ID: ' .. tostring(question.questionId) end\n"
    "    lines[#lines + 1] = ':END:'\n"
    "    for _, l in ipairs(mep_leetcode_html_to_text(question.content)) do lines[#lines + 1] = l end\n"
    "    lines[#lines + 1] = ''\n"
    "    lines[#lines + 1] = '* Solution'\n"
    "    lines[#lines + 1] = '#+begin_src ' .. lang\n"
    "    local solution = mep_leetcode_snippet_for(question, lang)\n"
    "    if solution then\n"
    "      for sline in (solution .. '\\n'):gmatch('([^\\n]*)\\n') do lines[#lines + 1] = sline end\n"
    "    end\n"
    "    lines[#lines + 1] = '#+end_src'\n"
    "    lines[#lines + 1] = ''\n"
    "    lines[#lines + 1] = '* Tests'\n"
    "    lines[#lines + 1] = '#+begin_src ' .. lang\n"
    "    if question.sampleTestCase and question.sampleTestCase ~= '' then\n"
    "      local prefix = mep_leetcode_comment_prefix[lang] or '# '\n"
    "      lines[#lines + 1] = prefix .. 'LeetCode sample test case input (raw, not a runnable harness -- write real assertions here):'\n"
    "      for sline in (question.sampleTestCase .. '\\n'):gmatch('([^\\n]*)\\n') do lines[#lines + 1] = prefix .. sline end\n"
    "    end\n"
    "    lines[#lines + 1] = '#+end_src'\n"
    "    mep.fs_mkdir(mep.leetcode_dir)\n"
    "    local path = mep.leetcode_dir .. '/' .. question.titleSlug .. '.org'\n"
    "    local f = io.open(path, 'w')\n"
    "    if not f then mep.notify('Could not write ' .. path, 'error') return end\n"
    "    f:write(table.concat(lines, '\\n') .. '\\n')\n"
    "    f:close()\n"
    "    mep.notify('Wrote ' .. path, 'info')\n"
    "    mep.pane_open(path)\n"
    "  end)\n"
    "end\n"
    "mep.command('MepLeetcodeFetch', function()\n"
    "  mep.ui_input('LeetCode problem slug or URL:', '', function(v)\n"
    "    if v and v ~= '' then mep.leetcode_fetch(v) end\n"
    "  end)\n"
    "end)\n"
    // Submit `code`/`lang_slug` for `slug`/`question_id`, then poll the
    // check endpoint until a verdict lands (`SUCCESS` state) or attempts
    // run out. `cb(err, result)` -- `result` is the raw check-endpoint
    // response (status_msg/total_correct/total_testcases/status_runtime/
    // status_memory) on success.
    "local function mep_leetcode_api_submit(slug, question_id, lang_slug, code, cookie, cb)\n"
    "  local csrf = mep_leetcode_csrf_of(cookie)\n"
    "  if not csrf then cb('mep.leetcode: could not find csrftoken in the configured cookie', nil) return end\n"
    "  local headers = {'-H', 'Cookie: ' .. cookie, '-H', 'x-csrftoken: ' .. csrf,\n"
    "    '-H', 'Referer: https://leetcode.com/problems/' .. slug .. '/'}\n"
    "  mep_leetcode_http_json('POST', 'https://leetcode.com/problems/' .. slug .. '/submit/', headers,\n"
    "    {lang = lang_slug, question_id = question_id, typed_code = code},\n"
    "    function(err, decoded)\n"
    "      if err then cb('mep.leetcode: ' .. err, nil) return end\n"
    "      local submission_id = decoded.submission_id\n"
    "      if not submission_id then cb('mep.leetcode: submit did not return a submission id (check credentials)', nil) return end\n"
    "      local check_url = 'https://leetcode.com/submissions/detail/' .. tostring(submission_id) .. '/check/'\n"
    "      local attempt = 0\n"
    "      local function poll()\n"
    "        attempt = attempt + 1\n"
    "        mep_leetcode_http_json('GET', check_url, headers, nil, function(gerr, result)\n"
    "          if gerr then cb('mep.leetcode: ' .. gerr, nil)\n"
    "          elseif result.state == 'SUCCESS' then cb(nil, result)\n"
    "          elseif attempt >= 20 then cb('mep.leetcode: timed out waiting for a verdict', nil)\n"
    "          else mep_leetcode_sleep(1.5, poll) end\n"
    "        end)\n"
    "      end\n"
    "      poll()\n"
    "    end)\n"
    "end\n"
    // Submits the current buffer's Solution block for the problem named
    // by its own :SLUG: property (under the Prompt headline -- written by
    // mep.leetcode_fetch above). :QUESTION_ID: is reused if present,
    // else fetched fresh first (e.g. a file never live-fetched before).
    "function mep.leetcode_submit()\n"
    "  local prompt_row = nil\n"
    "  for i = 1, mep.line_count() do\n"
    "    local h = mep_org_parse_headline(mep.get_line(i))\n"
    "    if h and h.title == 'Prompt' then prompt_row = i break end\n"
    "  end\n"
    "  if not prompt_row then mep.notify('No Prompt headline in this buffer -- fetch this problem first via :MepLeetcodeFetch', 'warn') return end\n"
    "  local slug = mep.org_property_get(prompt_row, 'SLUG')\n"
    "  if not slug then mep.notify('No :SLUG: property under Prompt -- fetch this problem first via :MepLeetcodeFetch', 'warn') return end\n"
    "  local question_id = mep.org_property_get(prompt_row, 'QUESTION_ID')\n"
    "  local lang, solution = mep_leetcode_find_src_under('Solution')\n"
    "  if not solution then mep.notify('Missing Solution src block', 'warn') return end\n"
    "  local lc_lang = mep_leetcode_babel_to_slug[lang]\n"
    "  if not lc_lang then mep.notify('No LeetCode language mapping for: ' .. tostring(lang), 'warn') return end\n"
    "  mep_leetcode_get_cookie(function(cookie)\n"
    "    local function do_submit(qid)\n"
    "      mep.notify('Submitting to LeetCode...', 'info')\n"
    "      mep_leetcode_api_submit(slug, qid, lc_lang, solution, cookie, function(err, result)\n"
    "        if err then mep.notify(err, 'error') return end\n"
    "        mep.notify('LeetCode: ' .. (result.status_msg or '?') .. ' (' .. tostring(result.total_correct or '?') ..\n"
    "          '/' .. tostring(result.total_testcases or '?') .. ')', (result.status_msg == 'Accepted') and 'info' or 'warn')\n"
    "      end)\n"
    "    end\n"
    "    if question_id then\n"
    "      do_submit(question_id)\n"
    "    else\n"
    "      mep.leetcode_fetch_problem(slug, function(err, question)\n"
    "        if err then mep.notify(err, 'error') return end\n"
    "        do_submit(question.questionId)\n"
    "      end)\n"
    "    end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepLeetcodeSubmit', mep.leetcode_submit)\n";

const char *kBuiltinPickerSources =
    "function mep.themes()\n"
    "  local before = mep.current_theme()\n"
    // Live preview (NVIM_PARITY_PLAN.md Phase 9 gap): the 6th arg fires on
    // every highlighted-row change (arrow/Ctrl-N/Ctrl-P, or the query
    // narrowing to a new top match) and re-tints the whole UI immediately,
    // *before* Enter/Escape commit or cancel -- on_select below still runs
    // on Enter (re-applies the same theme, harmless) or Escape (item == nil,
    // restores `before`), so cancelling always reverts whatever was
    // previewed while browsing.
    "  mep.picker_open('Colorscheme', mep.theme_names(), function(item)\n"
    "    if item then mep.colorscheme(item) else mep.colorscheme(before) end\n"
    "  end, nil, nil, function(item) mep.colorscheme(item) end)\n"
    "end\n"
    "mep.command('MepTheme', mep.themes)\n"
    // mep.nvim's own convention for this exact picker (mep.nvim/lua/mep/
    // theme/config.lua's `keymaps.picker = {'<leader>ut'}`) -- ported
    // here since mep.themes()/mep.picker_open/mep.leader_map were all
    // already implemented but never actually wired to an entry point.
    "mep.leader_map('ut', 'Theme picker', mep.themes)\n"
    // mep_picker_preview_file(path, max_lines): reads up to max_lines (40
    // default) of `path` and hands it to mep.picker_set_preview -- the
    // Phase 8 preview-pane gap, closed. Shared by find_files below and
    // (over a matched line's file) live_grep further down. Truncation
    // note ("...") rather than silently cutting off makes the preview
    // pane's own scope cut (no scrolling, see DrawPickerOverlay) visible
    // to the user instead of just quietly showing an incomplete file.\n"
    "function mep_picker_preview_file(path, max_lines)\n"
    "  local f = io.open(path, 'r')\n"
    "  if not f then mep.picker_set_preview('(cannot open ' .. path .. ')') return end\n"
    "  local lines, truncated = {}, false\n"
    "  for line in f:lines() do\n"
    "    if #lines >= (max_lines or 40) then truncated = true break end\n"
    "    lines[#lines + 1] = line\n"
    "  end\n"
    "  f:close()\n"
    "  if truncated then lines[#lines + 1] = '...' end\n"
    "  mep.picker_set_preview(table.concat(lines, '\\n'))\n"
    "end\n"
    // Asynchronous population (NVIM_PARITY_PLAN.md Phase 8 gap, closed):
    // the picker now opens as soon as the *first* result line arrives
    // (ensure_open, below) instead of waiting for rg/find to exit --
    // real for a large tree, where the old on_exit-only version left the
    // UI looking unresponsive (no picker at all) until the whole scan
    // finished. Results still stream in afterward via periodic
    // mep.picker_set_items flushes (debounced to ~12/sec, mep.now() --
    // same idiom as mep.on_buffer_changed's interval check above) rather
    // than one C++ call per line, which would re-copy/re-clamp the
    // picker's item list thousands of times for a big repo. Preview pane:
    // on_select_change (5th arg... 4th is nil for "no on_query_change")
    // shows the highlighted file's first lines via the helper above.\n"
    "function mep.find_files()\n"
    "  local lines = {}\n"
    "  local last_flush = mep.now()\n"
    "  local function flush() mep.picker_set_items(lines) end\n"
    "  local opened = false\n"
    "  local function ensure_open()\n"
    "    if opened then return end\n"
    "    opened = true\n"
    "    mep.picker_open('Find Files', lines, function(item)\n"
    "      if item then mep.cmd('e ' .. item) end\n"
    "    end, nil, nil, function(item)\n"
    "      if item and item ~= '' then mep_picker_preview_file(item) end\n"
    "    end)\n"
    "  end\n"
    "  local function on_line(line)\n"
    "    lines[#lines + 1] = line\n"
    "    ensure_open()\n"
    "    local now = mep.now()\n"
    "    if now - last_flush > 0.08 then last_flush = now flush() end\n"
    "  end\n"
    "  mep.job_start({'rg', '--files', '--hidden', '--glob', '!.git'}, {\n"
    "    on_stdout = on_line,\n"
    "    on_exit = function(code)\n"
    "      if #lines == 0 then\n"
    "        mep.job_start({'find', '.', '-type', 'f', '-not', '-path', '*/.git/*'}, {\n"
    "          on_stdout = on_line,\n"
    "          on_exit = function() ensure_open() flush() end,\n"
    "        })\n"
    "      else\n"
    "        ensure_open()\n"
    "        flush()\n"
    "      end\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "mep.leader_map('pf', 'Find files', mep.find_files)\n"
    // Live grep (NVIM_PARITY_PLAN.md Phase 8 gap, closed): typing in the
    // picker's prompt re-runs ripgrep against the *live* pattern and
    // streams matches in, rather than fuzzy-filtering one static list
    // gathered up front (that's what find_files/buffers/commands above
    // still do, correctly, for genuinely static sources). Debounced
    // (kLiveGrepDebounce) so fast typing doesn't spawn a process per
    // keystroke -- mep.on_frame polls a "pending query" the on_query_
    // change callback just stashes a timestamp for, same polling-debounce
    // shape as mep.on_buffer_changed. A monotonic generation counter
    // guards against a just-superseded search's late stdout/on_exit
    // (already-running job killed, but any in-flight callback queued
    // before the kill still fires once via JobManager::PollAll) from
    // clobbering a newer search's results. Opened with raw_results=true
    // (mep.picker_open's 7th arg) since ripgrep already did the
    // filtering -- Phase 8's own client-side fuzzy re-filter would
    // otherwise redundantly re-match every line and could even hide a
    // real regex match whose special characters aren't literally present
    // in the matched text (see Editor::OpenPicker's comment).\n"
    "local mep_live_grep_state = {active = false, pending_query = nil, pending_time = 0, job_id = nil, gen = 0}\n"
    "function mep.live_grep()\n"
    "  local st = mep_live_grep_state\n"
    "  st.active = true\n"
    "  st.pending_query = nil\n"
    "  st.gen = st.gen + 1\n"
    "  if st.job_id and mep.job_is_running(st.job_id) then mep.job_kill(st.job_id) end\n"
    "  st.job_id = nil\n"
    "  mep.picker_open('Live Grep', {}, function(item)\n"
    "    st.active = false\n"
    "    if st.job_id and mep.job_is_running(st.job_id) then mep.job_kill(st.job_id) end\n"
    "    if item then\n"
    "      local file, lnum = item:match('^(.*):(%d+)$')\n"
    "      if file then mep.cmd('e ' .. file) mep.set_cursor(tonumber(lnum), 1) end\n"
    "    end\n"
    "  end, function(query)\n"
    "    st.pending_query = query\n"
    "    st.pending_time = mep.now()\n"
    "  end, nil, function(item)\n"
    "    if item and item ~= '' then\n"
    "      local file, lnum = item:match('^(.*):(%d+)$')\n"
    "      if file then mep_picker_preview_file(file, 200) end\n"
    "    end\n"
    "  end, true)\n"
    "end\n"
    "mep.leader_map('pg', 'Live grep', mep.live_grep)\n"
    "local kLiveGrepDebounce = 0.12\n"
    "mep.on_frame(function()\n"
    "  local st = mep_live_grep_state\n"
    "  if not st.active or not st.pending_query then return end\n"
    "  if mep.now() - st.pending_time < kLiveGrepDebounce then return end\n"
    "  local query = st.pending_query\n"
    "  st.pending_query = nil\n"
    "  if st.job_id and mep.job_is_running(st.job_id) then mep.job_kill(st.job_id) end\n"
    "  st.job_id = nil\n"
    "  if query == '' then mep.picker_set_items({}) return end\n"
    "  st.gen = st.gen + 1\n"
    "  local my_gen = st.gen\n"
    "  local items = {}\n"
    "  local last_flush = mep.now()\n"
    "  local function flush() if my_gen == st.gen then mep.picker_set_items(items) end end\n"
    // Explicit trailing '.' path (not just relying on rg's "no PATH
    // argument -> search cwd" default): with no PATH argument at all,
    // ripgrep instead reads stdin whenever stdin isn't a tty -- true of
    // every mep.job_start child (job.cpp always wires stdin to a pipe,
    // never left open+unwritten+unclosed on purpose since mep.job_write/
    // job_close_stdin exist for callers that *do* want to feed one) -- a
    // real hang caught during this feature's own Xvfb verification: the
    // job never produced output or exited, IsRunning() stayed true
    // forever. Confirmed via a bare reproduction (rg with a pattern, no
    // path, stdin an open-but-silent pipe) before landing this fix.\n"
    "  st.job_id = mep.job_start({'rg', '--line-number', '--no-heading', '--color=never', '--', query, '.'}, {\n"
    "    on_stdout = function(line)\n"
    "      if my_gen ~= st.gen then return end\n"
    "      local file, lnum = line:match('^(.-):(%d+):.*$')\n"
    "      if not file then return end\n"
    "      items[#items + 1] = {display = line, data = file .. ':' .. lnum}\n"
    "      local now = mep.now()\n"
    "      if now - last_flush > 0.08 then last_flush = now flush() end\n"
    "    end,\n"
    "    on_exit = function() flush() end,\n"
    "  })\n"
    "end)\n"
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
    "end\n"
    // Default winbar breadcrumb click handler (Phase 11 click-dispatch
    // gap): clicking a directory segment of the per-pane header's path
    // (main.cpp's DrawPane) navigates into it by opening a file picker
    // scoped to that directory, same rg-with-find-fallback pattern as
    // mep.find_files above, just rooted at `dir` instead of '.'.
    "function mep.winbar_navigate(dir)\n"
    "  if not dir or dir == '' then return end\n"
    "  local lines = {}\n"
    "  mep.job_start({'rg', '--files', '--hidden', '--glob', '!.git', dir}, {\n"
    "    on_stdout = function(line) lines[#lines + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      if #lines == 0 then\n"
    "        mep.job_start({'find', dir, '-type', 'f', '-not', '-path', '*/.git/*'}, {\n"
    "          on_stdout = function(line) lines[#lines + 1] = line end,\n"
    "          on_exit = function()\n"
    "            mep.picker_open('Files in ' .. dir, lines, function(item)\n"
    "              if item then mep.cmd('e ' .. item) end\n"
    "            end)\n"
    "          end,\n"
    "        })\n"
    "      else\n"
    "        mep.picker_open('Files in ' .. dir, lines, function(item)\n"
    "          if item then mep.cmd('e ' .. item) end\n"
    "        end)\n"
    "      end\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "mep.set_winbar_click(mep.winbar_navigate)\n";

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
    const std::string &real = g_editor.PromptInput();
    // Masked prompts (mep.ui_input's opts.masked/opts.password, e.g. the
    // AI module's API-key fallback) render '*' in place of the real text
    // -- Editor::prompt_input_ (what on_done_ref eventually receives)
    // keeps the real typed text untouched; only this rendered line
    // substitutes it. One '*' per byte rather than per codepoint, so
    // multi-byte UTF-8 input renders extra stars -- an accepted
    // approximation given masked input is realistically ASCII (API keys,
    // passwords).
    std::string line = g_editor.PromptMasked() ? std::string(real.size(), '*') : real;
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

// Read-only informational float (Phase 17 gap: git-gutter's "preview
// hunk", mep.float_preview) -- same DrawFloatFrame box the overlays
// above use, sized to fit its (possibly multi-line) text, dismissed by
// any keypress or click (Editor::HandlePreviewInput). Lines starting
// with '+'/'-' (a unified-diff hunk body, the mep.float_preview caller
// this was added for) are tinted with the same Add/Red groups the git
// gutter's own decorations use, so the preview visually matches the
// signs the user is previewing.
void DrawPreviewOverlay() {
    std::vector<std::string> lines = SplitLines(g_editor.PreviewText());
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 6;
    float max_w = MeasureTextEx(g_font, g_editor.PreviewTitle().c_str(), MenuFontSize(), 0).x;
    for (const auto &line : lines) max_w = std::max(max_w, MeasureTextEx(g_font, line.c_str(), font_size, 0).x);
    int box_w = std::min(GetScreenWidth() - 80, static_cast<int>(max_w) + 40);
    box_w = std::max(box_w, 260);
    float hint_size = MenuFontSize();
    // DrawFloatFrame reserves its own title row (MenuFontSize()+8) on top
    // of whatever content starts at content_y, and the "press any key"
    // hint below needs a row of its own too -- omitting either from the
    // box_h budget let the hint overlap the last content line for
    // short (e.g. one-line) previews.
    int title_h = g_editor.PreviewTitle().empty() ? 0 : static_cast<int>(MenuFontSize()) + 8;
    int box_h = std::min(GetScreenHeight() - 80,
                          10 + title_h + static_cast<int>(lines.size()) * line_h + static_cast<int>(hint_size) + 24);
    FloatFrame f = DrawFloatFrame(box_w, box_h, g_editor.PreviewTitle());
    for (size_t i = 0; i < lines.size(); i++) {
        float y = f.content_y + i * line_h;
        const std::string &line = lines[i];
        Color color = ResolveHlGroup("Normal");
        if (!line.empty() && line[0] == '+') color = ResolveHlGroup("Add");
        else if (!line.empty() && line[0] == '-') color = ResolveHlGroup("Red");
        DrawTextEx(g_font, line.c_str(), Vector2{f.content_x, y}, font_size, 0, color);
    }
    std::string hint = "Press any key to close";
    float hint_w = MeasureTextEx(g_font, hint.c_str(), hint_size, 0).x;
    DrawTextEx(g_font, hint.c_str(),
               Vector2{static_cast<float>(f.box_x + f.box_w) - hint_w - 14,
                       static_cast<float>(f.box_y + f.box_h - hint_size - 10)},
               hint_size, 0, ResolveHlGroup("Comment"));
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

// Fuzzy picker: prompt line + live-filtered results list (Phase 8).
// Preview pane (NVIM_PARITY_PLAN.md Phase 8 gap, closed): when a source
// has called mep.picker_set_preview(text) (currently find_files/
// live_grep, kBuiltinPickerSources), the box widens and splits into a
// narrower results column on the left and a scrolled-to-nothing (just
// top-anchored, not following the cursor -- these are short peeks, not
// an editable view) text column on the right, divided by a vertical
// rule. No preview means the box stays exactly the single-column shape
// it always was -- existing pickers that never call SetPickerPreview
// (buffers/commands/themes/...) are visually unchanged.
void DrawPickerOverlay() {
    bool has_preview = !g_editor.PickerPreview().empty();
    int box_w = has_preview ? std::min(GetScreenWidth() - 80, 920) : std::min(GetScreenWidth() - 80, 640);
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

    int list_w = has_preview ? static_cast<int>((f.box_x + f.box_w - f.content_x) * 0.42f) : (f.box_x + f.box_w) - static_cast<int>(f.content_x) - 4;

    std::vector<PickerItem> results = g_editor.PickerFilteredResults();
    float list_y = f.content_y + g_font_size + 14;
    int line_h = static_cast<int>(g_font_size) + 4;
    int max_rows = std::max(1, static_cast<int>((f.box_y + f.box_h - list_y) / line_h));
    int selected = g_editor.PickerSelected();
    int start = std::max(0, selected - max_rows + 1);
    BeginScissorMode(f.box_x, static_cast<int>(list_y) - 2, list_w, f.box_y + f.box_h - static_cast<int>(list_y));
    for (int i = start; i < static_cast<int>(results.size()) && i < start + max_rows; i++) {
        float ry = list_y + (i - start) * line_h;
        if (i == selected) {
            DrawRectangle(f.box_x + 4, static_cast<int>(ry) - 1, list_w - 8, line_h, ResolveHlGroup("PickerSelected"));
        }
        DrawTextEx(g_font, results[i].display.c_str(), Vector2{f.content_x, ry}, g_font_size, 0, ResolveHlGroup("Normal"));
    }
    if (results.empty()) {
        DrawTextEx(g_font, "-- no matches --", Vector2{f.content_x, list_y}, g_font_size, 0, ResolveHlGroup("Comment"));
    }
    EndScissorMode();

    if (has_preview) {
        int div_x = f.box_x + list_w + 6;
        DrawLine(div_x, static_cast<int>(list_y) - 4, div_x, f.box_y + f.box_h - 6, ResolveHlGroup("PickerBorder"));
        float px = static_cast<float>(div_x + 10);
        int preview_w = (f.box_x + f.box_w) - div_x - 20;
        BeginScissorMode(div_x, static_cast<int>(list_y) - 2, preview_w + 20, f.box_y + f.box_h - static_cast<int>(list_y));
        int max_chars = std::max(10, static_cast<int>(preview_w / g_char_width));
        int row = 0;
        int max_preview_rows = static_cast<int>((f.box_y + f.box_h - list_y) / line_h);
        for (const std::string &raw_line : SplitLines(g_editor.PickerPreview())) {
            if (row >= max_preview_rows) break;
            if (raw_line.empty()) { row++; continue; }
            size_t pos = 0;
            while (pos < raw_line.size() && row < max_preview_rows) {
                size_t take = std::min(raw_line.size() - pos, static_cast<size_t>(max_chars));
                std::string chunk = raw_line.substr(pos, take);
                DrawTextEx(g_font, chunk.c_str(), Vector2{px, list_y + row * line_h}, g_font_size, 0,
                           ResolveHlGroup("Normal"));
                pos += take;
                row++;
            }
        }
        EndScissorMode();
    }
}

// Roam backlink-graph view (NVIM_PARITY_PLAN.md Phase 37's flagged "no
// fuzzy backlink-graph visualization" gap, closed). A real 2D node-link
// diagram, but a deliberately scoped-down one: a deterministic ring
// layout, not a force-directed physics simulation -- see the writeup above
// Editor::OpenRoamGraph in editor.cpp and NVIM_PARITY_PLAN.md's Phase 37
// section for why. Hop 0 (the note the view opened on) sits dead center;
// hop-1 nodes (its direct links + direct backlinks) ring it at radius r1;
// hop-2 nodes (links/backlinks of those) ring it at radius r2. Recomputed
// fresh every frame -- node counts here are small (a few dozen at most,
// per the Lua side's hop-2 cap), so there's no need to cache it.
static std::vector<Vector2> ComputeRoamGraphPositions(const std::vector<RoamGraphNode> &nodes, float cx, float cy,
                                                        float r1, float r2) {
    constexpr float kTwoPi = 6.28318530718f;
    std::vector<Vector2> pos(nodes.size(), Vector2{cx, cy});
    std::vector<int> hop1, hop2;
    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        if (nodes[i].hop == 1) hop1.push_back(i);
        else if (nodes[i].hop >= 2) hop2.push_back(i);
    }
    for (size_t k = 0; k < hop1.size(); k++) {
        float ang = kTwoPi * static_cast<float>(k) / static_cast<float>(hop1.size());
        pos[hop1[k]] = Vector2{cx + r1 * cosf(ang), cy + r1 * sinf(ang)};
    }
    for (size_t k = 0; k < hop2.size(); k++) {
        float ang = kTwoPi * static_cast<float>(k) / static_cast<float>(hop2.size());
        pos[hop2[k]] = Vector2{cx + r2 * cosf(ang), cy + r2 * sinf(ang)};
    }
    return pos;
}

void DrawRoamGraphOverlay() {
    int box_w = std::min(GetScreenWidth() - 60, 1100);
    int box_h = std::min(GetScreenHeight() - 60, 780);
    FloatFrame f = DrawFloatFrame(box_w, box_h, g_editor.RoamGraphTitle());

    std::string prompt_line = "/ " + g_editor.RoamGraphQuery();
    DrawTextEx(g_font, prompt_line.c_str(), Vector2{f.content_x, f.content_y}, g_font_size, 0, ResolveHlGroup("Normal"));
    if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
        float cx = f.content_x + MeasureTextEx(g_font, prompt_line.c_str(), g_font_size, 0).x;
        DrawRectangle(static_cast<int>(cx), static_cast<int>(f.content_y), 2, static_cast<int>(g_font_size),
                      ResolveHlGroup("Normal"));
    }
    float divider_y = f.content_y + g_font_size + 6;
    DrawLine(f.box_x + 4, static_cast<int>(divider_y), f.box_x + f.box_w - 4, static_cast<int>(divider_y),
             ResolveHlGroup("PickerBorder"));

    const std::vector<RoamGraphNode> &nodes = g_editor.RoamGraphNodes();
    const std::vector<RoamGraphEdge> &edges = g_editor.RoamGraphEdges();
    if (nodes.empty()) {
        DrawTextEx(g_font, "-- no linked notes --", Vector2{f.content_x, divider_y + 14}, g_font_size, 0,
                   ResolveHlGroup("Comment"));
        return;
    }

    float area_top = divider_y + 8;
    float area_bottom = static_cast<float>(f.box_y + f.box_h) - 10;
    float area_left = static_cast<float>(f.box_x) + 10;
    float area_right = static_cast<float>(f.box_x + f.box_w) - 10;
    float cx = (area_left + area_right) / 2.0f;
    float cy = (area_top + area_bottom) / 2.0f;
    float max_r = std::min(area_right - area_left, area_bottom - area_top) / 2.0f - 40.0f;
    max_r = std::max(max_r, 60.0f);
    float r1 = max_r * 0.5f;
    float r2 = max_r;

    std::vector<Vector2> pos = ComputeRoamGraphPositions(nodes, cx, cy, r1, r2);
    std::vector<int> filtered = g_editor.RoamGraphFilteredIndices();
    std::vector<bool> visible(nodes.size(), false);
    for (int i : filtered) visible[i] = true;
    int selected_idx = -1;
    if (g_editor.RoamGraphSelected() >= 0 && g_editor.RoamGraphSelected() < static_cast<int>(filtered.size())) {
        selected_idx = filtered[g_editor.RoamGraphSelected()];
    }

    // Edges first, so nodes/labels draw on top of them.
    Color edge_color = ResolveHlGroup("PickerBorder");
    Color edge_dim = Fade(edge_color, 0.25f);
    for (const RoamGraphEdge &e : edges) {
        if (e.a < 0 || e.a >= static_cast<int>(nodes.size()) || e.b < 0 || e.b >= static_cast<int>(nodes.size())) {
            continue;
        }
        bool both_visible = visible[e.a] && visible[e.b];
        DrawLineEx(pos[e.a], pos[e.b], both_visible ? 1.6f : 1.0f, both_visible ? edge_color : edge_dim);
    }

    Color normal_c = ResolveHlGroup("Normal");
    Color center_c = ResolveHlGroup("PickerTitle");
    Color dim_c = ResolveHlGroup("Comment");
    Color select_c = ResolveHlGroup("PickerSelected");
    Color border_c = ResolveHlGroup("FloatBorder");
    float label_size = std::max(10.0f, g_font_size * 0.8f);

    // The fuzzy filter narrows which nodes are *highlighted* rather than
    // which are drawn at all (see RoamGraphFilteredIndices' own comment):
    // a node that doesn't match the current query dims to `dim_c` instead
    // of disappearing, so the ring layout (and the edges touching it)
    // stays legible as you type instead of reflowing every keystroke.
    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        bool is_center = nodes[i].hop == 0;
        bool is_visible = visible[i];
        float radius = is_center ? 10.0f : (nodes[i].hop == 1 ? 7.0f : 5.0f);
        Color fill = !is_visible ? dim_c : (is_center ? center_c : normal_c);
        if (i == selected_idx) {
            DrawCircleV(pos[i], radius + 5.0f, Fade(select_c, 0.85f));
        }
        DrawCircleV(pos[i], radius, fill);
        DrawCircleLines(static_cast<int>(pos[i].x), static_cast<int>(pos[i].y), radius, border_c);

        std::string label = nodes[i].title.empty() ? nodes[i].path : nodes[i].title;
        if (label.size() > 22) label = label.substr(0, 21) + "...";
        Vector2 msz = MeasureTextEx(g_font, label.c_str(), label_size, 0);
        Vector2 lp{pos[i].x - msz.x / 2.0f, pos[i].y + radius + 3.0f};
        Color text_c = !is_visible ? dim_c : (i == selected_idx ? select_c : normal_c);
        DrawTextEx(g_font, label.c_str(), lp, label_size, 0, text_c);
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

// Hover tooltip (NVIM_PARITY_PLAN.md Phase 3 gap, closed): a small
// floating window anchored just below the cursor -- same "coexists with
// whatever mode is active, doesn't dim the screen" shape as
// DrawCompletionPopup above (unlike DrawPreviewOverlay/DrawFloatFrame's
// modal centered box), since a hover tooltip is meant to feel transient
// and not interrupt editing. Text may be long/markdown-ish (LSP hover
// contents), so this wraps by character count rather than assuming
// short single-line items the way the completion list can. Dismissal
// (cursor move / mode change / Escape) is Editor::MaybeDismissHover's
// job, not this function's -- it just draws whenever IsHoverOpen().
void DrawHoverPopup(float x, float y) {
    if (!g_editor.IsHoverOpen()) return;
    const std::string &title = g_editor.HoverTitle();
    const std::string &text = g_editor.HoverText();
    float font_size = g_font_size;
    int line_h = static_cast<int>(font_size) + 4;
    int max_box_w = std::min(GetScreenWidth() - 40, 640);
    int max_chars_per_line = std::max(20, static_cast<int>((max_box_w - 20) / g_char_width));
    std::vector<std::string> wrapped;
    for (const std::string &raw_line : SplitLines(text)) {
        if (raw_line.empty()) { wrapped.push_back(""); continue; }
        size_t pos = 0;
        while (pos < raw_line.size()) {
            size_t take = std::min(raw_line.size() - pos, static_cast<size_t>(max_chars_per_line));
            wrapped.push_back(raw_line.substr(pos, take));
            pos += take;
        }
    }
    // Cap total height so a huge hover payload (e.g. a long docstring)
    // doesn't run off-screen; not scrollable -- this is a tooltip, not a
    // picker/sidebar, and truncating is the same tradeoff DrawFloatFrame-
    // based overlays already make for oversized content.
    int max_rows = std::min(static_cast<int>(wrapped.size()), 20);
    float max_w = title.empty() ? 0.0f : MeasureTextEx(g_font, title.c_str(), MenuFontSize(), 0).x;
    for (int i = 0; i < max_rows; i++) max_w = std::max(max_w, MeasureTextEx(g_font, wrapped[i].c_str(), font_size, 0).x);
    int box_w = std::min(max_box_w, static_cast<int>(max_w) + 20);
    int title_h = title.empty() ? 0 : static_cast<int>(MenuFontSize()) + 6;
    int box_h = title_h + max_rows * line_h + 10;
    if (x + box_w > GetScreenWidth()) x = GetScreenWidth() - box_w;
    if (x < 0) x = 0;
    // Prefer drawing below the cursor (the `y` passed in); flip above it
    // if there isn't room below, same idea DrawCmdlineCompletionPopup
    // uses for the command bar's upward-growing list.
    if (y + box_h > GetScreenHeight()) y = std::max(0.0f, y - box_h - line_h);
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), box_w, box_h, ResolveHlGroup("Picker"));
    DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), box_w, box_h, ResolveHlGroup("PickerBorder"));
    float ty = y + 5;
    if (!title.empty()) {
        DrawTextEx(g_font, title.c_str(), Vector2{x + 8, ty}, MenuFontSize(), 0, ResolveHlGroup("PickerTitle"));
        ty += title_h;
    }
    for (int i = 0; i < max_rows; i++) {
        DrawTextEx(g_font, wrapped[i].c_str(), Vector2{x + 8, ty + i * line_h}, font_size, 0, ResolveHlGroup("Normal"));
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

// Lazily uploads (once per buffer id, cached in g_image_textures -- see its
// own comment) the decoded RGBA8 pixels from an ImageSession's ImageDoc as
// a GPU texture, and returns it. `sess.doc` is decoded once by
// Editor::OpenImageInPlace and never mutated, so nothing here ever needs to
// re-upload once cached.
Texture2D GetOrLoadImageTexture(int buffer_id, const ImageSession &sess) {
    auto it = g_image_textures.find(buffer_id);
    if (it != g_image_textures.end()) return it->second;
    Image img{};
    img.data = const_cast<unsigned char *>(sess.doc->Pixels());
    img.width = sess.doc->Width();
    img.height = sess.doc->Height();
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    Texture2D tex = LoadTextureFromImage(img);  // copies pixel data to the GPU; img.data stays ImageDoc's
    g_image_textures[buffer_id] = tex;
    return tex;
}

// Same idea as GetOrLoadImageTexture, but keyed per (buffer_id, page) --
// PdfSession virtualizes its own CPU-side raster cache the same way (see
// Editor::EnsurePdfPagesRastered) -- and re-uploaded whenever that page's
// raster or the session's theme_colors flag has moved on (see the cache
// entry struct's comment). When theme_colors is set, the raw RGBA raster
// is recolored through ThemedPdfChannel into a scratch buffer before
// upload; the raw raster itself is never mutated, so toggling back to
// standard colors doesn't need a re-render, just a re-upload.
Texture2D GetOrUpdatePdfPageTexture(int buffer_id, int page_index, const PdfSession::PageRaster &raster,
                                     bool theme_colors) {
    int theme_epoch = g_editor.ThemeEpoch();
    auto key = std::make_pair(buffer_id, page_index);
    auto it = g_pdf_page_textures.find(key);
    if (it != g_pdf_page_textures.end() && it->second.generation == raster.generation &&
        it->second.theme_colors == theme_colors && it->second.theme_epoch == theme_epoch) {
        return it->second.tex;
    }

    const unsigned char *pixels = raster.rgba.data();
    std::vector<unsigned char> themed;
    if (theme_colors) {
        Color fg = ResolveHlGroup("Normal");
        Color bg = ResolveHlGroup("NormalBg");
        themed.resize(raster.rgba.size());
        size_t n = static_cast<size_t>(raster.w) * static_cast<size_t>(raster.h);
        for (size_t i = 0; i < n; i++) {
            const unsigned char *src = &raster.rgba[i * 4];
            float luminance = (0.299f * src[0] + 0.587f * src[1] + 0.114f * src[2]) / 255.0f;
            unsigned char *dst = &themed[i * 4];
            dst[0] = ThemedPdfChannel(fg.r, bg.r, luminance);
            dst[1] = ThemedPdfChannel(fg.g, bg.g, luminance);
            dst[2] = ThemedPdfChannel(fg.b, bg.b, luminance);
            dst[3] = 255;
        }
        pixels = themed.data();
    }

    Image img{};
    img.data = const_cast<unsigned char *>(pixels);
    img.width = raster.w;
    img.height = raster.h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    if (it != g_pdf_page_textures.end() && it->second.w == raster.w && it->second.h == raster.h) {
        UpdateTexture(it->second.tex, img.data);
        it->second.generation = raster.generation;
        it->second.theme_colors = theme_colors;
        it->second.theme_epoch = theme_epoch;
        return it->second.tex;
    }
    if (it != g_pdf_page_textures.end()) UnloadTexture(it->second.tex);
    PdfTextureCacheEntry entry;
    entry.tex = LoadTextureFromImage(img);
    entry.generation = raster.generation;
    entry.theme_colors = theme_colors;
    entry.theme_epoch = theme_epoch;
    entry.w = raster.w;
    entry.h = raster.h;
    g_pdf_page_textures[key] = entry;
    return entry.tex;
}

// Evicts GPU textures for any page of `buffer_id` that Editor::
// EnsurePdfPagesRastered no longer keeps a CPU-side raster for (i.e. it
// scrolled out of the {page-1, page, page+1} window) -- keeps GPU memory
// bounded the same way the CPU-side cache is bounded, regardless of how
// many pages of a long document have been scrolled through.
void PrunePdfPageTextures(int buffer_id, const PdfSession &sess) {
    for (auto it = g_pdf_page_textures.begin(); it != g_pdf_page_textures.end();) {
        if (it->first.first == buffer_id && sess.rasters.find(it->first.second) == sess.rasters.end()) {
            UnloadTexture(it->second.tex);
            it = g_pdf_page_textures.erase(it);
        } else {
            ++it;
        }
    }
}

void DrawPane(const Pane &pane, float x, float y, float w, float h, bool is_active) {
    int line_height = LineHeight();
    int header_h = PaneHeaderHeight();
    float font_size = MenuFontSize();
    // Captured inside the is_active cursor block below, consumed after
    // EndScissorMode() -- see the hover-tooltip comment down there.
    float hover_cursor_x = 0, hover_cursor_y = 0;
    bool hover_cursor_valid = false;

    const Buffer &buf = g_editor.GetBuffer(pane.buffer_id);
    Color header_bg = is_active ? ResolveHlGroup("TabActive") : ResolveHlGroup("MenuBar");
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), header_h, header_bg);
    const TerminalSession *term_sess = g_editor.GetTerminal(pane.buffer_id);
    const ImageSession *img_sess = g_editor.GetImage(pane.buffer_id);
    const PdfSession *pdf_sess = g_editor.GetPdf(pane.buffer_id);
    const OfficeSession *office_sess = g_editor.GetOffice(pane.buffer_id);
    const SheetSession *sheet_sess = g_editor.GetSheet(pane.buffer_id);
    // Per-pane buffer tabs (Phase 14) suffix, and the [+] modified marker --
    // appended as plain text after whatever's drawn below, breadcrumb or not.
    std::string suffix = buf.modified ? " [+]" : "";
    if (pane.buffer_tabs.size() > 1) {
        suffix += "  [" + std::to_string(pane.buffer_tab_index + 1) + "/" + std::to_string(pane.buffer_tabs.size()) +
                  "]";
    }
    float label_y = y + (header_h - font_size) / 2.0f;
    if (term_sess) {
        const std::string &live_title =
            (term_sess->vterm && !term_sess->vterm->Title().empty()) ? term_sess->vterm->Title() : term_sess->title;
        std::string label = "Terminal: " + live_title;
        if (term_sess->exited) label += " [exited: " + std::to_string(term_sess->exit_code) + "]";
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    } else if (img_sess) {
        std::string label = "Image: " + buf.filename;
        if (img_sess->doc) {
            label += " (" + std::to_string(img_sess->doc->Width()) + "x" + std::to_string(img_sess->doc->Height()) +
                      ") " + std::to_string(static_cast<int>(img_sess->zoom * 100.0f + 0.5f)) + "%";
        }
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    } else if (pdf_sess && pdf_sess->search_active) {
        // Takes over the header the same way Mode::Command's cmdline takes
        // over the bottom bar -- a blinking-cursor '/' input line instead
        // of the normal "PDF: file (page N/M) zoom%" label while typing.
        std::string line = "/" + pdf_sess->search_input;
        DrawTextEx(g_font, line.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
            float cx = x + 6 + MeasureTextEx(g_font, line.c_str(), font_size, 0).x;
            DrawRectangle(static_cast<int>(cx), static_cast<int>(label_y), 2, static_cast<int>(font_size),
                          ResolveHlGroup("Normal"));
        }
    } else if (pdf_sess) {
        std::string label = "PDF: " + buf.filename;
        if (pdf_sess->doc) {
            label += " (page " + std::to_string(pdf_sess->page + 1) + "/" +
                     std::to_string(pdf_sess->doc->PageCount()) + ") " +
                     std::to_string(static_cast<int>(pdf_sess->zoom * 100.0f + 0.5f)) + "%" +
                     (pdf_sess->theme_colors ? "  [theme, Ctrl-R]" : "  [original, Ctrl-R]");
            if (!pdf_sess->search_query.empty()) {
                label += pdf_sess->search_matches.empty()
                             ? "  /" + pdf_sess->search_query + " (no matches)"
                             : "  /" + pdf_sess->search_query + " (" +
                                   std::to_string(pdf_sess->search_current + 1) + "/" +
                                   std::to_string(pdf_sess->search_matches.size()) + ", n/p)";
            }
        }
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    } else if (office_sess) {
        std::string label = "Office: " + buf.filename;
        if (!office_sess->doc.paragraphs.empty()) {
            label += " (para " + std::to_string(office_sess->cursor_para + 1) + "/" +
                     std::to_string(office_sess->doc.paragraphs.size()) + ") " +
                     std::to_string(static_cast<int>(office_sess->zoom * 100.0f + 0.5f)) + "%";
        }
        if (buf.modified) label += " [+]";
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    } else if (sheet_sess) {
        std::string label = "Sheet: " + buf.filename;
        if (!sheet_sess->wb.sheets.empty()) {
            const Sheet &sh = sheet_sess->wb.sheets[sheet_sess->active_sheet];
            label += " (" + sh.name + ") " + CellAddressToString(sheet_sess->cursor_row, sheet_sess->cursor_col);
        }
        if (buf.modified) label += " [+]";
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    } else if (buf.scratch || buf.filename.empty()) {
        std::string label = (buf.scratch ? "[Scratch]" : "[No Name]") + suffix;
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    } else {
        // Winbar-equivalent breadcrumb (Phase 11 click-dispatch gap): each
        // path component before the filename is its own clickable segment
        // -- mep has no separate winbar chrome row, so this per-pane header
        // (already showing the path) is where a breadcrumb naturally lives.
        // Clicking a directory segment calls the registered winbar-click
        // Lua ref (mep.winbar_navigate by default, kBuiltinPickerSources)
        // with the path up to and including that segment; the final
        // (filename) segment isn't clickable -- it's already the open file.
        std::vector<std::string> parts;
        {
            size_t start = 0;
            while (start <= buf.filename.size()) {
                size_t slash = buf.filename.find('/', start);
                if (slash == std::string::npos) {
                    parts.push_back(buf.filename.substr(start));
                    break;
                }
                if (slash > start) parts.push_back(buf.filename.substr(start, slash - start));
                start = slash + 1;
            }
        }
        float seg_x = x + 6;
        std::string accum;
        for (size_t pi = 0; pi < parts.size(); pi++) {
            bool is_last = (pi + 1 == parts.size());
            std::string seg_text = parts[pi] + (is_last ? "" : "/");
            float seg_w = MeasureTextEx(g_font, seg_text.c_str(), font_size, 0).x;
            Color seg_color = is_last ? ResolveHlGroup("Normal") : ResolveHlGroup("Comment");
            DrawTextEx(g_font, seg_text.c_str(), Vector2{seg_x, label_y}, font_size, 0, seg_color);
            if (!is_last) {
                accum = accum.empty() ? parts[pi] : accum + "/" + parts[pi];
                std::string dir_path = accum;
                RegisterClickRegion(Rectangle{seg_x, y, seg_w, static_cast<float>(header_h)}, [dir_path] {
                    if (g_editor.Lua() && g_editor.WinbarClickRef() != 0) {
                        g_editor.Lua()->CallRefWithString(g_editor.WinbarClickRef(), dir_path);
                    }
                });
            }
            seg_x += seg_w;
        }
        DrawTextEx(g_font, suffix.c_str(), Vector2{seg_x, label_y}, font_size, 0, ResolveHlGroup("Normal"));
    }

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

    if (img_sess && img_sess->doc) {
        g_editor.ResizeImageViewport(pane.buffer_id, static_cast<int>(w), static_cast<int>(content_h));
        Texture2D tex = GetOrLoadImageTexture(pane.buffer_id, *img_sess);
        BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                          static_cast<int>(content_h));
        DrawTextureEx(tex, Vector2{x - img_sess->pan_x, content_y - img_sess->pan_y}, 0.0f, img_sess->zoom, WHITE);
        EndScissorMode();
        Color img_border = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
        DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                            img_border);
        return;
    }

    if (pdf_sess && pdf_sess->doc && pdf_sess->doc->PageCount() > 0) {
        g_editor.ResizePdfViewport(pane.buffer_id, static_cast<int>(w), static_cast<int>(content_h));
        g_editor.EnsurePdfPagesRastered(pane.buffer_id);
        PrunePdfPageTextures(pane.buffer_id, *pdf_sess);

        BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                          static_cast<int>(content_h));
        // Draws up to 3 stacked pages -- the anchor page (pdf_sess->page,
        // positioned so scroll_y device-pixels have already scrolled past
        // its top edge) plus whichever of its immediate neighbors
        // EnsurePdfPagesRastered has cached -- continuously, so scrolling
        // crosses page boundaries smoothly instead of jumping between
        // single-page views. Mirrors kPdfPageGapPx from Editor::
        // HandlePdfInput's scroll-rebase math exactly (see its comment).
        float anchor_y = content_y - pdf_sess->scroll_y;
        // Search-match highlights piggyback IncSearch's theme color (the
        // text buffer's own live-search-preview highlight -- see its
        // comment in BuildHighlightGroups) rather than a fixed yellow, so
        // this stays theme-consistent the same way the page recoloring
        // does: dim alpha for "just another match," brighter for
        // search_current specifically.
        Color match_c = ResolveHlGroup("IncSearch");
        Color match_other = Color{match_c.r, match_c.g, match_c.b, 90};
        Color match_cur = Color{match_c.r, match_c.g, match_c.b, 190};
        auto draw_page = [&](int idx, float top_y) -> float {
            auto rit = pdf_sess->rasters.find(idx);
            if (rit == pdf_sess->rasters.end()) return 0.0f;
            const PdfSession::PageRaster &pr = rit->second;
            Texture2D tex = GetOrUpdatePdfPageTexture(pane.buffer_id, idx, pr, pdf_sess->theme_colors);
            Vector2 pos{x - pdf_sess->pan_x, top_y};
            DrawTextureEx(tex, pos, 0.0f, pdf_sess->zoom, WHITE);
            for (const PdfHighlightRect &hr : pr.highlights) {
                bool current = hr.match_index == pdf_sess->search_current;
                DrawRectangle(static_cast<int>(pos.x + hr.x0 * pdf_sess->zoom),
                              static_cast<int>(pos.y + hr.y0 * pdf_sess->zoom),
                              static_cast<int>((hr.x1 - hr.x0) * pdf_sess->zoom),
                              static_cast<int>((hr.y1 - hr.y0) * pdf_sess->zoom), current ? match_cur : match_other);
            }
            return static_cast<float>(pr.h) * pdf_sess->zoom;
        };
        float anchor_h = draw_page(pdf_sess->page, anchor_y);
        if (pdf_sess->page > 0) {
            auto rit = pdf_sess->rasters.find(pdf_sess->page - 1);
            if (rit != pdf_sess->rasters.end()) {
                float prev_h = static_cast<float>(rit->second.h) * pdf_sess->zoom;
                draw_page(pdf_sess->page - 1, anchor_y - kPdfPageGapPx - prev_h);
            }
        }
        draw_page(pdf_sess->page + 1, anchor_y + anchor_h + kPdfPageGapPx);
        EndScissorMode();

        Color pdf_border = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
        DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                            pdf_border);
        return;
    }

    if (office_sess && !office_sess->doc.paragraphs.empty()) {
        // Toolbar row (Phase 5): Bold/Italic/Underline buttons reflecting
        // the format at the cursor (OfficeNormal) or uniformly across the
        // selection (OfficeVisual) -- the mouse-click equivalent of the
        // b/i/u keybindings (Editor::ToggleOfficeFormat), satisfying the
        // "richer top bar" the office pane was asked for up front. Shrinks
        // content_y/content_h in place (safe: every path through this
        // office branch ends in `return`, so nothing after it in DrawPane
        // reads the pre-toolbar values for this call).
        float toolbar_h = static_cast<float>(header_h);
        DrawRectangle(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                      static_cast<int>(toolbar_h), ResolveHlGroup("MenuBar"));
        struct ToolbarBtn { char which; const char *label; };
        static const ToolbarBtn kBtns[] = {{'b', "B"}, {'i', "I"}, {'u', "U"}};
        float btn_x = x + 6;
        float btn_w = 28.0f, btn_h = toolbar_h - 6.0f;
        float btn_y = content_y + 3.0f;
        for (const ToolbarBtn &btn : kBtns) {
            bool active = g_editor.OfficeFormatActive(btn.which);
            Color bg = active ? ResolveHlGroup("Visual") : ResolveHlGroup("TabActive");
            DrawRectangle(static_cast<int>(btn_x), static_cast<int>(btn_y), static_cast<int>(btn_w),
                          static_cast<int>(btn_h), bg);
            Vector2 label_size = MeasureTextEx(g_font, btn.label, font_size, 0);
            DrawTextEx(g_font, btn.label,
                      Vector2{btn_x + (btn_w - label_size.x) / 2.0f, btn_y + (btn_h - font_size) / 2.0f}, font_size, 0,
                      ResolveHlGroup("Normal"));
            char which = btn.which;
            RegisterClickRegion(Rectangle{btn_x, btn_y, btn_w, btn_h}, [which] { g_editor.ToggleOfficeFormat(which); });
            btn_x += btn_w + 6.0f;
        }
        content_y += toolbar_h;
        content_h -= toolbar_h;

        g_editor.ResizeOfficeViewport(pane.buffer_id, static_cast<int>(w), static_cast<int>(content_h));
        const OfficeDoc &doc = office_sess->doc;
        int para_count = static_cast<int>(doc.paragraphs.size());
        float pad = 10.0f;
        float max_width = std::max(50.0f, w - 2.0f * pad);
        float body_size = office_sess->base_font_pt * office_sess->zoom;

        auto line_height_for = [&](int heading_level) { return body_size * OfficeHeadingMultiplier(heading_level) * 1.35f; };

        // Wraps the cursor's own paragraph once up front (reused by both
        // the scroll-follow scan below and the draw loop) to find which of
        // ITS visual lines holds cursor_col.
        int cp = std::clamp(office_sess->cursor_para, 0, para_count - 1);
        float cursor_para_size = body_size * OfficeHeadingMultiplier(doc.paragraphs[cp].heading_level);
        std::vector<OfficeWrapLine> cursor_wrap = WordWrapOfficeParagraph(doc.paragraphs[cp], max_width, cursor_para_size);
        int cursor_line_in_para = 0;
        for (int li = 0; li < static_cast<int>(cursor_wrap.size()); li++) {
            cursor_line_in_para = li;
            if (office_sess->cursor_col <= cursor_wrap[li].end) break;
        }

        // Scroll-follow: word-wrap-aware equivalent of Editor::
        // UpdateScrollForPane's own "snap up if the cursor is above the
        // current scroll position, else advance a row at a time until it's
        // back in view" shape -- can't live in editor.cpp (raylib-free, no
        // MeasureTextEx), see ResizeOfficeViewport's own comment for why.
        int scroll_para = office_sess->scroll_para;
        int scroll_line = office_sess->scroll_line_in_para;
        bool cursor_before_scroll = (cp < scroll_para) || (cp == scroll_para && cursor_line_in_para < scroll_line);
        if (cursor_before_scroll) {
            scroll_para = cp;
            scroll_line = cursor_line_in_para;
        } else {
            for (;;) {
                float used = 0.0f;
                bool found = false;
                for (int pi = scroll_para; pi < para_count; pi++) {
                    const DocParagraph &para = doc.paragraphs[pi];
                    float lh = line_height_for(para.heading_level);
                    std::vector<OfficeWrapLine> wl_scan;
                    const std::vector<OfficeWrapLine> *wlp;
                    if (pi == cp) {
                        wlp = &cursor_wrap;
                    } else {
                        wl_scan = WordWrapOfficeParagraph(para, max_width, body_size * OfficeHeadingMultiplier(para.heading_level));
                        wlp = &wl_scan;
                    }
                    const std::vector<OfficeWrapLine> &wl = *wlp;
                    int start_li = (pi == scroll_para) ? scroll_line : 0;
                    bool overflowed = false;
                    for (int li = start_li; li < static_cast<int>(wl.size()); li++) {
                        if (pi == cp && li == cursor_line_in_para) found = true;
                        used += lh;
                        if (used > content_h) {
                            overflowed = true;
                            break;
                        }
                    }
                    if (overflowed || found) break;
                }
                if (found || scroll_para >= cp) break;
                const DocParagraph &spara = doc.paragraphs[scroll_para];
                std::vector<OfficeWrapLine> swl =
                    WordWrapOfficeParagraph(spara, max_width, body_size * OfficeHeadingMultiplier(spara.heading_level));
                if (scroll_line + 1 < static_cast<int>(swl.size())) {
                    scroll_line++;
                } else if (scroll_para + 1 < para_count) {
                    scroll_para++;
                    scroll_line = 0;
                } else {
                    break;
                }
            }
        }
        if (scroll_para != office_sess->scroll_para || scroll_line != office_sess->scroll_line_in_para) {
            g_editor.SetOfficeScroll(pane.buffer_id, scroll_para, scroll_line);
        }

        BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                          static_cast<int>(content_h));
        Color text_color = ResolveHlGroup("Normal");
        Color sel_color = ResolveHlGroup("Visual");
        bool office_visual = is_active && g_editor.CurrentMode() == Mode::OfficeVisual && office_sess->has_selection;
        int sel_pa = 0, sel_ca = 0, sel_pb = 0, sel_cb = 0;
        if (office_visual) {
            int ap = office_sess->sel_anchor_para, ac = office_sess->sel_anchor_col;
            int ccp = office_sess->cursor_para, ccc = office_sess->cursor_col;
            if (ap < ccp || (ap == ccp && ac <= ccc)) {
                sel_pa = ap; sel_ca = ac; sel_pb = ccp; sel_cb = ccc;
            } else {
                sel_pa = ccp; sel_ca = ccc; sel_pb = ap; sel_cb = ac;
            }
        }
        float draw_y = content_y;
        for (int pi = scroll_para; pi < para_count && draw_y < content_y + content_h; pi++) {
            const DocParagraph &para = doc.paragraphs[pi];
            float size = body_size * OfficeHeadingMultiplier(para.heading_level);
            float lh = size * 1.35f;
            std::vector<OfficeWrapLine> wl_local = (pi == cp) ? cursor_wrap : WordWrapOfficeParagraph(para, max_width, size);
            int start_li = (pi == scroll_para) ? scroll_line : 0;
            for (int li = start_li; li < static_cast<int>(wl_local.size()); li++) {
                if (draw_y > content_y + content_h) break;
                const OfficeWrapLine &line = wl_local[li];
                std::vector<OfficeFormatRun> runs = BuildOfficeFormatRuns(para, line.start, line.end);
                float line_x = x + pad;
                if (para.align == DocParagraph::Align::Center || para.align == DocParagraph::Align::Right) {
                    float total_w = 0.0f;
                    for (const auto &r : runs) {
                        std::string t = para.text.substr(r.start, r.end - r.start);
                        for (auto &ch : t) {
                            if (ch == '\t') ch = ' ';
                        }
                        total_w += MeasureTextEx(OfficeFontFor(r.fmt), t.c_str(), size, 0).x;
                    }
                    line_x = para.align == DocParagraph::Align::Center ? x + (w - total_w) / 2.0f : x + w - pad - total_w;
                }
                if (para.bullet && li == 0) {
                    DrawTextEx(OfficeFontFor(DocFormat{}), "\xE2\x80\xA2 ", Vector2{line_x, draw_y}, size, 0, text_color);
                    line_x += MeasureTextEx(OfficeFontFor(DocFormat{}), "\xE2\x80\xA2 ", size, 0).x;
                }
                if (office_visual && pi >= sel_pa && pi <= sel_pb) {
                    int hl_start = (pi == sel_pa) ? sel_ca : 0;
                    int hl_end = (pi == sel_pb) ? sel_cb : static_cast<int>(para.text.size());
                    hl_start = std::max(hl_start, line.start);
                    hl_end = std::min(hl_end, line.end);
                    if (hl_end > hl_start) {
                        std::vector<OfficeFormatRun> pre_runs = BuildOfficeFormatRuns(para, line.start, hl_start);
                        float hl_x0 = line_x;
                        for (const auto &r : pre_runs) {
                            std::string t = para.text.substr(r.start, r.end - r.start);
                            for (auto &ch : t) {
                                if (ch == '\t') ch = ' ';
                            }
                            hl_x0 += MeasureTextEx(OfficeFontFor(r.fmt), t.c_str(), size, 0).x;
                        }
                        std::vector<OfficeFormatRun> hl_runs = BuildOfficeFormatRuns(para, hl_start, hl_end);
                        float hl_w = 0.0f;
                        for (const auto &r : hl_runs) {
                            std::string t = para.text.substr(r.start, r.end - r.start);
                            for (auto &ch : t) {
                                if (ch == '\t') ch = ' ';
                            }
                            hl_w += MeasureTextEx(OfficeFontFor(r.fmt), t.c_str(), size, 0).x;
                        }
                        DrawRectangle(static_cast<int>(hl_x0), static_cast<int>(draw_y), static_cast<int>(hl_w),
                                      static_cast<int>(size), sel_color);
                    }
                }
                float run_x = line_x;
                for (const auto &r : runs) {
                    std::string t = para.text.substr(r.start, r.end - r.start);
                    for (auto &ch : t) {
                        if (ch == '\t') ch = ' ';
                    }
                    Font &f = OfficeFontFor(r.fmt);
                    DrawTextEx(f, t.c_str(), Vector2{run_x, draw_y}, size, 0, text_color);
                    float rw = MeasureTextEx(f, t.c_str(), size, 0).x;
                    if (r.fmt.underline || r.fmt.strike) {
                        float uy = draw_y + (r.fmt.strike ? size * 0.5f : size * 0.95f);
                        DrawLineEx(Vector2{run_x, uy}, Vector2{run_x + rw, uy}, 1.0f, text_color);
                    }
                    run_x += rw;
                }
                if (is_active && pi == cp && li == cursor_line_in_para) {
                    std::vector<OfficeFormatRun> pre = BuildOfficeFormatRuns(para, line.start, office_sess->cursor_col);
                    float cursor_x = line_x;
                    for (const auto &r : pre) {
                        std::string t = para.text.substr(r.start, r.end - r.start);
                        for (auto &ch : t) {
                            if (ch == '\t') ch = ' ';
                        }
                        cursor_x += MeasureTextEx(OfficeFontFor(r.fmt), t.c_str(), size, 0).x;
                    }
                    DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(draw_y), 2, static_cast<int>(size),
                                  text_color);
                }
                draw_y += lh;
            }
        }
        EndScissorMode();
        Color office_border = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
        DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                            office_border);
        return;
    }

    if (sheet_sess && !sheet_sess->wb.sheets.empty()) {
        g_editor.ResizeSheetViewport(pane.buffer_id, static_cast<int>(w), static_cast<int>(content_h));
        const Sheet &sh = sheet_sess->wb.sheets[sheet_sess->active_sheet];

        // Formula bar: shows the active cell's raw text (or the live
        // edit buffer while SheetInsert is active) -- doubles as both a
        // real-spreadsheet-familiar "what's actually in this cell"
        // readout and the Insert-mode live-typing display.
        float bar_h = static_cast<float>(header_h);
        DrawRectangle(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w), static_cast<int>(bar_h),
                      ResolveHlGroup("MenuBar"));
        std::string bar_text;
        if (sheet_sess->editing) {
            bar_text = sheet_sess->edit_buffer;
        } else {
            const Cell *cur = sh.FindCell(sheet_sess->cursor_row, sheet_sess->cursor_col);
            bar_text = cur ? cur->raw : "";
        }
        DrawTextEx(g_font, bar_text.c_str(), Vector2{x + 6, content_y + (bar_h - font_size) / 2.0f}, font_size, 0,
                   ResolveHlGroup("Normal"));
        if (is_active && sheet_sess->editing && fmodf(static_cast<float>(GetTime()), 1.0f) < 0.6f) {
            std::string pre = bar_text.substr(0, std::min<size_t>(sheet_sess->edit_cursor, bar_text.size()));
            float cx = x + 6 + MeasureTextEx(g_font, pre.c_str(), font_size, 0).x;
            DrawRectangle(static_cast<int>(cx), static_cast<int>(content_y + (bar_h - font_size) / 2.0f), 2,
                          static_cast<int>(font_size), ResolveHlGroup("Normal"));
        }

        float grid_y = content_y + bar_h;
        float grid_h = content_h - bar_h;
        BeginScissorMode(static_cast<int>(x), static_cast<int>(grid_y), static_cast<int>(w), static_cast<int>(grid_h));

        Color header_row_bg = ResolveHlGroup("MenuBar");
        Color text_color = ResolveHlGroup("Normal");
        Color cursor_bg = ResolveHlGroup("Visual");
        Color sel_bg = Color{cursor_bg.r, cursor_bg.g, cursor_bg.b, 90};

        float col_header_h = static_cast<float>(kSheetRowHeight);
        float row_header_w = static_cast<float>(kSheetRowHeaderW);
        float col_w = static_cast<float>(kSheetColWidth);
        float row_h = static_cast<float>(kSheetRowHeight);

        int visible_rows = std::max(1, static_cast<int>((grid_h - col_header_h) / row_h));
        int visible_cols = std::max(1, static_cast<int>((w - row_header_w) / col_w));

        DrawRectangle(static_cast<int>(x), static_cast<int>(grid_y), static_cast<int>(w), static_cast<int>(col_header_h),
                      header_row_bg);
        for (int vc = 0; vc <= visible_cols; vc++) {
            int col = sheet_sess->scroll_col + vc;
            float cx = x + row_header_w + vc * col_w;
            if (cx > x + w) break;
            std::string letters = ColumnIndexToLetters(col);
            Vector2 sz = MeasureTextEx(g_font, letters.c_str(), font_size, 0);
            DrawTextEx(g_font, letters.c_str(),
                       Vector2{cx + (col_w - sz.x) / 2.0f, grid_y + (col_header_h - font_size) / 2.0f}, font_size, 0,
                       text_color);
        }

        float body_y = grid_y + col_header_h;
        float body_h = grid_h - col_header_h;
        DrawRectangle(static_cast<int>(x), static_cast<int>(body_y), static_cast<int>(row_header_w),
                      static_cast<int>(body_h), header_row_bg);

        bool sheet_visual = is_active && g_editor.CurrentMode() == Mode::SheetVisual && sheet_sess->has_selection;
        int sel_r0 = 0, sel_r1 = -1, sel_c0 = 0, sel_c1 = -1;
        if (sheet_visual) {
            sel_r0 = std::min(sheet_sess->sel_anchor_row, sheet_sess->cursor_row);
            sel_r1 = std::max(sheet_sess->sel_anchor_row, sheet_sess->cursor_row);
            sel_c0 = std::min(sheet_sess->sel_anchor_col, sheet_sess->cursor_col);
            sel_c1 = std::max(sheet_sess->sel_anchor_col, sheet_sess->cursor_col);
        }

        for (int vr = 0; vr <= visible_rows; vr++) {
            int row = sheet_sess->scroll_row + vr;
            float ry = body_y + vr * row_h;
            if (ry > grid_y + grid_h) break;
            std::string rownum = std::to_string(row + 1);
            Vector2 sz = MeasureTextEx(g_font, rownum.c_str(), font_size, 0);
            DrawTextEx(g_font, rownum.c_str(), Vector2{x + row_header_w - sz.x - 6, ry + (row_h - font_size) / 2.0f},
                       font_size, 0, text_color);

            for (int vc = 0; vc <= visible_cols; vc++) {
                int col = sheet_sess->scroll_col + vc;
                float cx = x + row_header_w + vc * col_w;
                if (cx > x + w) break;

                bool is_cursor = is_active && row == sheet_sess->cursor_row && col == sheet_sess->cursor_col;
                bool in_sel = sheet_visual && row >= sel_r0 && row <= sel_r1 && col >= sel_c0 && col <= sel_c1;
                if (is_cursor) {
                    DrawRectangle(static_cast<int>(cx), static_cast<int>(ry), static_cast<int>(col_w),
                                  static_cast<int>(row_h), cursor_bg);
                } else if (in_sel) {
                    DrawRectangle(static_cast<int>(cx), static_cast<int>(ry), static_cast<int>(col_w),
                                  static_cast<int>(row_h), sel_bg);
                }

                if (sh.FindCell(row, col)) {
                    CellValue v = g_editor.EvaluateSheetCell(pane.buffer_id, row, col);
                    std::string text = FormatCellValue(v);
                    // Truncated to the column width, not wrapped/ellipsized
                    // -- a v1 simplification (no per-column width overrides
                    // to expand into, unlike a real spreadsheet's
                    // overflow-into-the-next-empty-cell behavior).
                    while (!text.empty() && MeasureTextEx(g_font, text.c_str(), font_size, 0).x > col_w - 6) {
                        text.pop_back();
                    }
                    Color cell_color = v.kind == CellKind::Error ? ResolveHlGroup("ErrorText") : text_color;
                    float text_x = (v.kind == CellKind::Number)
                                        ? cx + col_w - 6 - MeasureTextEx(g_font, text.c_str(), font_size, 0).x
                                        : cx + 4;
                    DrawTextEx(g_font, text.c_str(), Vector2{text_x, ry + (row_h - font_size) / 2.0f}, font_size, 0,
                               cell_color);
                }
                DrawRectangleLines(static_cast<int>(cx), static_cast<int>(ry), static_cast<int>(col_w),
                                   static_cast<int>(row_h), Color{text_color.r, text_color.g, text_color.b, 40});
            }
        }
        EndScissorMode();
        Color sheet_border = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
        DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                            sheet_border);
        return;
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

    // buf.decorations is keyed by namespace, not by row -- scanning every
    // decoration in every namespace for every visible row (as this used
    // to, inline in the row loop below) is O(total_decorations ×
    // visible_rows), and syntax highlighting alone adds one decoration
    // per Treesitter capture for the *whole file*, not just what's on
    // screen. A file with a few thousand lines can put this well into
    // the hundreds of thousands of iterations *per frame* -- costly
    // enough on its own to blow the frame budget, and on the wasm/webview
    // build (software-rendered WebGL, see flake.nix's own
    // LIBGL_ALWAYS_SOFTWARE comment) slow frames compound into exactly
    // the "held key keeps moving after release" symptom: raylib's
    // key-repeat queue is small (16 slots) and gets fully drained every
    // poll, so it can't itself hold a backlog -- but a run of slow frames
    // means several polls' worth of real autorepeat events pile up in
    // the OS/X11/GLFW layer before mep gets back around to draining any
    // of them, and each catch-up poll is itself another slow frame. Built
    // once per pane per frame (O(total_decorations)) instead of inline
    // per row turns the row loop's own decoration cost into O(rows +
    // decorations actually on visible rows) -- for everything else in
    // this function, "visible" already means "cheap"; decorations were
    // the one exception.
    std::unordered_map<int, std::vector<const Decoration *>> decos_by_row;
    for (const auto &ns_decos : buf.decorations) {
        for (const Decoration &d : ns_decos.second) decos_by_row[d.row].push_back(&d);
    }
    // Decorations from different namespaces land in the same per-row
    // vector above in whatever order buf.decorations (keyed by namespace
    // id, an unordered_map) happens to iterate in -- not a guarantee
    // callers can rely on. A stable sort by priority (default 0) means a
    // namespace that actually needs to draw on top of another one (e.g.
    // markdown concealment's virt_overlay needing to paint over a
    // Treesitter-highlighted span underneath it) can do so deterministic-
    // ally just by setting a higher priority, while everything that
    // doesn't care about priority (the overwhelming majority of existing
    // decorations, all defaulting to 0) keeps its original per-namespace
    // insertion order relative to same-priority decorations.
    for (auto &row_entry : decos_by_row) {
        std::stable_sort(row_entry.second.begin(), row_entry.second.end(),
                          [](const Decoration *a, const Decoration *b) { return a->priority < b->priority; });
    }
    static const std::vector<const Decoration *> kNoDecos;

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

        // :set cursorline (Phase 11 option) -- a full-width tint drawn
        // beneath everything else on this visual row, so fold-summary
        // text, the selection rectangles, and the line-number gutter all
        // still draw on top of it unchanged. A closed fold's own hidden
        // rows never reach this loop as their own `row` value (the jump
        // below skips straight past them), so the cursor being anywhere
        // inside a closed fold is checked against the fold's [start, end]
        // range here rather than a plain `pane.cursor.row == row`.
        if (is_active && g_editor.ShowCursorLine() && !IsCommandLineMode(g_editor.CurrentMode()) &&
            (pane.cursor.row == row ||
             (fold_here && pane.cursor.row >= fold_here->start_row && pane.cursor.row <= fold_here->end_row))) {
            DrawRectangle(static_cast<int>(x), static_cast<int>(ly), static_cast<int>(w), line_height,
                          ResolveHlGroup("CursorLine"));
        }

        if (fold_here) {
            int hidden = fold_here->end_row - fold_here->start_row;
            std::string summary = "+-- " + std::to_string(hidden + 1) + " lines: " + buf.lines[row] + " ---";
            DrawTextEx(g_font, summary.c_str(), Vector2{text_x, ly}, g_font_size, 0, ResolveHlGroup("SidebarTitle"));
            // Fold marker click-to-toggle (Phase 11 click-dispatch gap):
            // mep has no separate statuscolumn widget row, so the fold
            // marker lives in the number gutter's existing reserved
            // trailing-space column (only when :set number has reserved
            // one). Only wired for the active pane -- ToggleFoldAtRow
            // operates on Buf(), the currently active buffer, so a click on
            // a background split's gutter would silently toggle the wrong
            // buffer's fold otherwise.
            if (gutter_w > 0.0f && is_active) {
                DrawTextEx(g_font, "+", Vector2{text_x - g_char_width, ly}, g_font_size, 0, ResolveHlGroup("LineNr"));
                int marker_row = row;
                RegisterClickRegion(
                    Rectangle{text_x - g_char_width, ly, g_char_width, static_cast<float>(line_height)},
                    [marker_row] { g_editor.ToggleFoldAtRow(marker_row); });
            }
            row = fold_here->end_row;
            continue;
        }
        // Open (unclosed) fold starting here: no summary line to replace
        // the row's own text with, but still worth a gutter marker so
        // there's something to click to close it (matches vim's
        // foldcolumn '-' convention for an open fold's start row).
        if (gutter_w > 0.0f && is_active) {
            for (const Fold &f : buf.folds) {
                if (!f.closed && f.start_row == row) {
                    DrawTextEx(g_font, "-", Vector2{text_x - g_char_width, ly}, g_font_size, 0, ResolveHlGroup("LineNr"));
                    int marker_row = row;
                    RegisterClickRegion(
                        Rectangle{text_x - g_char_width, ly, g_char_width, static_cast<float>(line_height)},
                        [marker_row] { g_editor.ToggleFoldAtRow(marker_row); });
                    break;
                }
            }
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
        auto row_decos_it = decos_by_row.find(row);
        const std::vector<const Decoration *> &row_decos =
            (row_decos_it != decos_by_row.end()) ? row_decos_it->second : kNoDecos;
        for (const Decoration *dp : row_decos) {
            const Decoration &d = *dp;
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
        DrawLineFast(buf.lines[row], text_x, ly, g_font_size, ResolveHlGroup("Normal"));
        for (const Decoration *dp : row_decos) {
            const Decoration &d = *dp;
            if (!d.whole_line && !d.underline && !d.hl_group.empty() && d.col_end > d.col_start) {
                const std::string &line = buf.lines[row];
                int a = std::min(static_cast<int>(line.size()), d.col_start);
                int b = std::min(static_cast<int>(line.size()), d.col_end);
                if (b > a) {
                    std::string span = line.substr(a, b - a);
                    DrawTextEx(g_font, span.c_str(), Vector2{text_x + a * g_char_width, ly}, g_font_size, 0,
                               ResolveHlGroup(d.hl_group));
                }
            }
            // Per-span underline (Phase 21 gap): a 1px DrawRectangle at
            // the text baseline, mirroring how VTermCell::underline is
            // rendered elsewhere (see the `cell->underline` block in
            // DrawTerminalGrid) -- same visual, but keyed off a column
            // range instead of a terminal cell.
            if (!d.whole_line && d.underline && d.col_end > d.col_start) {
                const std::string &line = buf.lines[row];
                int a = std::min(static_cast<int>(line.size()), d.col_start);
                int b = std::min(static_cast<int>(line.size()), d.col_end);
                if (b > a) {
                    Color c = d.hl_group.empty() ? ResolveHlGroup("Normal") : ResolveHlGroup(d.hl_group);
                    float ux = text_x + a * g_char_width;
                    float uw = (b - a) * g_char_width;
                    DrawRectangle(static_cast<int>(ux), static_cast<int>(ly + line_height - 2),
                                  static_cast<int>(uw), 1, c);
                }
            }
            if (!d.virt_text.empty()) {
                float vx = text_x + d.col_start * g_char_width;
                if (d.virt_overlay) {
                    // Cover whichever is wider: the replacement text, or
                    // the original [col_start, col_end) span it's
                    // standing in for. A caller concealing markup down to
                    // shorter text (markdown's "**bold**" -> "bold", or a
                    // whole link down to its link text) sets col_end to
                    // the *original* markup's end -- using only the
                    // replacement's own measured width here would leave
                    // the original span's tail end (anything past the
                    // replacement's width) undrawn-over and still
                    // visible, defeating the conceal.
                    float span_w = (d.col_end > d.col_start) ? (d.col_end - d.col_start) * g_char_width : 0.0f;
                    float overlay_w = std::max(span_w, MeasureTextEx(g_font, d.virt_text.c_str(), g_font_size, 0).x);
                    DrawRectangle(static_cast<int>(vx), static_cast<int>(ly), static_cast<int>(overlay_w),
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
        hover_cursor_x = cursor_x;
        hover_cursor_y = cursor_y + line_height;
        hover_cursor_valid = true;
    }

    EndScissorMode();

    Color border_color = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
    DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                        border_color);
    // Hover tooltip (Phase 3 gap): drawn *after* EndScissorMode/the pane
    // border above, unlike the completion popup a few lines up -- hover
    // text can be much wider/taller (multi-line LSP docs) than a narrow
    // split pane, so clipping it to the pane rect the way the completion
    // list already (harmlessly, for short candidate words) tolerates
    // would visibly truncate real hover content.
    if (hover_cursor_valid && g_editor.IsHoverOpen()) {
        DrawHoverPopup(hover_cursor_x, hover_cursor_y);
    }
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
        // Click-to-switch (Phase 11 click-dispatch gap): a click anywhere on
        // this tab's box jumps straight to it via GoToTab, same as :tabn N.
        RegisterClickRegion(Rectangle{x, static_cast<float>(y), w, static_cast<float>(bar_h)},
                             [i] { g_editor.GoToTab(i); });
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
    // Rebuilt fresh this frame by DrawTabBar/DrawPane below -- see the
    // g_click_regions declaration for why clearing here (rather than after
    // dispatch) is correct even with the one-frame lag.
    g_click_regions.clear();
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
    if (g_editor.CurrentMode() == Mode::Preview) DrawPreviewOverlay();
    if (g_editor.CurrentMode() == Mode::Picker) DrawPickerOverlay();
    if (g_editor.CurrentMode() == Mode::RoamGraph) DrawRoamGraphOverlay();
    if (g_editor.CurrentMode() == Mode::WhichKey) DrawWhichKeyOverlay();
    if (!zen) DrawSidebars();
    if (g_show_help_overlay) DrawHelpOverlay();
    DrawToastStack();

    EndDrawing();
}

// Consumes this frame's mouse click (if any) against whatever click regions
// DrawEditor() just registered (tab bar, gutter fold markers, pane header
// breadcrumb -- see g_click_regions above). Only fires while in a mode where
// clicking chrome behind an overlay would be surprising: any of the modal
// overlay modes (Picker/Sidebar/Prompt/Confirm/Select/WhichKey/Preview)
// already captures input itself and draws over this chrome, so a click
// meant for the overlay must not also fall through to e.g. switch tabs
// underneath it. First matching region wins, mirroring the menu bar's own
// one-hit-per-click dispatch.
void DispatchChromeClicks() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    switch (g_editor.CurrentMode()) {
        case Mode::Picker:
        case Mode::RoamGraph:
        case Mode::Sidebar:
        case Mode::Prompt:
        case Mode::Confirm:
        case Mode::Select:
        case Mode::WhichKey:
        case Mode::Preview:
            return;
        default:
            break;
    }
    Vector2 mouse = GetMousePosition();
    for (const ClickRegion &r : g_click_regions) {
        if (PointInRect(mouse, r.rect)) {
            r.action();
            return;
        }
    }
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
// without `just run-wasm` -- there's no native window to close there, so
// this is a deliberate no-op in that case).
EM_JS(void, mep_js_request_native_quit, (), {
    if (window.mepQuit) window.mepQuit();
});
#endif

void UpdateDrawFrame() {
    // Last-resort safety net: this is the sole per-frame entry point (both
    // the native while-loop and the Emscripten main loop below call
    // nothing else per frame) -- an uncaught C++ exception ANYWHERE in a
    // frame's job-poll/input/draw handling would otherwise propagate past
    // both loops straight into std::terminate, crashing the whole app and
    // losing every unsaved buffer with no save prompt. Converts that into
    // a visible error notification instead and lets the *next* frame
    // proceed normally -- if the throw landed mid-DrawEditor (after its
    // own BeginDrawing but before EndDrawing), the current frame may render
    // incompletely/skip its buffer swap, which is a one-frame visual
    // glitch at worst (raylib's BeginDrawing doesn't require a matching
    // EndDrawing to have happened first) -- vastly preferable to losing
    // all open work. Catching std::exception specifically, not `...`: an
    // unknown non-exception throw (not used anywhere in this codebase)
    // still terminates, rather than silently swallowing something a
    // std::exception& can't describe via what().
    try {
        JobManager::Instance().PollAll();
        g_editor.PollTerminals();
        g_editor.PruneExpiredToasts(GetTime());
        g_editor.SetNow(GetTime());
        if (g_editor.Lua()) g_editor.Lua()->RunFrameHooks();
        HandleFontSizeShortcuts();
        bool menu_consumed = HandleMenuInput();
        if (!menu_consumed) g_editor.HandleInput();
        DrawEditor();
        // Only after DrawEditor() has (re)populated g_click_regions this
        // frame, and only when the menu bar didn't already claim this
        // click (a menu dropdown can overlap the tab bar/pane area
        // beneath it).
        if (!menu_consumed) DispatchChromeClicks();
    } catch (const std::exception &e) {
        g_editor.Notify(std::string("Internal error (recovered): ") + e.what(), Editor::NotifyLevel::Error);
    }
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
    LoadOfficeFonts();
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
