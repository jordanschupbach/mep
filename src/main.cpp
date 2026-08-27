#include "raylib.h"
#include "rlgl.h"  // rlPushMatrix/rlMultMatrixf/rlTranslatef -- org emphasis italic's shear transform

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "editor.h"
#include "font_data.h"
#include "icon_font_data.h"
#include "job.h"
#include "lua_env.h"
#include "vterm.h"
#include "image_doc.h"
#include "pdf_doc.h"
#include "office_doc.h"
#include "office_font_data.h"
#include "office_font_data_mono.h"
#include "office_font_data_serif.h"

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

// Nerd Font icon glyphs (Private Use Area codepoints), reloaded alongside
// g_font at the same size in ApplyFontSize -- a *separate* font rather
// than folding these into g_font's own atlas, since g_font is loaded with
// no explicit codepoint list (LoadFontFromMemory's default ASCII 32..126
// range) and raylib bakes one atlas per LoadFontFromMemory call from one
// font file; there's no "add more glyphs from a second file" API. Backing
// data is icon_font_data.h, a pyftsubset subset of Symbols Nerd Font Mono
// covering exactly kIconCodepoints below -- the full font is ~10,000
// glyphs/2.5MB, tiny fraction of which mep actually draws.
Font g_icon_font{};

// Every codepoint g_icon_font is loaded with -- doubles as the "does this
// codepoint have an icon glyph at all" check DrawUiText uses to route a
// given character to g_icon_font vs g_font, since GetGlyphIndex's "not
// found" return (0) is indistinguishable from a legitimate index 0 without
// this. Keep in sync with icon_font_data.h's own codepoint set (its header
// comment has the regeneration command) -- IconForFilename (editor.cpp)
// and every kIcon* constant below only ever draw correctly if the
// codepoint they return is also listed here.
constexpr int kIconCodepoints[] = {
    // Generic file/folder (also IconForFilename's own fallback).
    0xf15b, 0xf07b, 0xf07c,
    // Per-language/extension (IconForFilename, editor.cpp) -- mirrors
    // mep.nvim/lua/mep/icons/data.lua's M.nerd_font table.
    0xe620, 0xe606, 0xe60c, 0xe625, 0xe628, 0xe7ba, 0xe627, 0xe68b, 0xe791, 0xe738, 0xe61e, 0xf0fd, 0xe61d, 0xf031b,
    0xe608, 0xe795, 0xe760, 0xe60b, 0xe8eb, 0xe6b2, 0xf05c0, 0xe736, 0xe6b8, 0xe603, 0xf48a, 0xe609, 0xf0219, 0xe706,
    0xe62b, 0xe672, 0xf0331, 0xeaeb, 0xe60d, 0xf0721, 0xf410, 0xe64a, 0xf462, 0xe779, 0xf0868, 0xe702, 0xe652, 0xe60a,
    0xe633,
    // UI chrome: tab bar circles/buttons, notify toasts, LSP diagnostics,
    // todoscan gutter signs, DAP breakpoints (see each's own kIcon*
    // constant below for which is which).
    0xf111, 0xf10c, 0xf057, 0xf071, 0xf05a, 0xf0eb, 0xf188, 0xf00c, 0xf0ad, 0xf0e7, 0xf24a, 0xf00d, 0xf067,
};

bool IsIconCodepoint(int cp) {
    for (int c : kIconCodepoints) {
        if (c == cp) return true;
    }
    return false;
}

// Same "separate atlas, same reload point" pattern as g_icon_font just
// above, for LaTeX-math rendering (main.cpp's own HTML-pane math layout,
// further down): g_font's baked set is ASCII-only, and Greek letters/math
// operators aren't in it. Baked from the *same* embedded TTF
// (kJetBrainsMonoRegularTtf) rather than a second font file, just with a
// wider explicit codepoint list -- JetBrains Mono already covers this
// range, no new embed needed. Kept separate from g_font itself (rather
// than widening g_font's own bake) specifically so g_glyph_index's
// ASCII-only fast path (see its own comment, just above ApplyFontSize)
// never has to change.
Font g_math_font{};

// Greek letters (lower+upper) + a curated set of common LaTeX math
// operators/relations/arrows, *beyond* the ASCII 32..126 range ApplyFontSize
// prepends when baking g_math_font (below) -- covers \alpha.. \omega,
// \Gamma.. \Omega, \times \div \pm \leq \geq \neq \approx \infty \sum
// \prod \int \sqrt \to \Rightarrow \in \subset \cup \cap and friends. Not
// exhaustive (real LaTeX has thousands of symbol macros) -- an unmapped
// command falls back to showing its own name as plain text (see
// LayoutMathCommand), a legible degradation rather than a missing glyph.
constexpr int kMathCodepoints[] = {
    // Greek lowercase alpha..omega (includes the rarely-used final-sigma).
    0x3b1, 0x3b2, 0x3b3, 0x3b4, 0x3b5, 0x3b6, 0x3b7, 0x3b8, 0x3b9, 0x3ba, 0x3bb, 0x3bc, 0x3bd, 0x3be, 0x3bf, 0x3c0,
    0x3c1, 0x3c2, 0x3c3, 0x3c4, 0x3c5, 0x3c6, 0x3c7, 0x3c8, 0x3c9,
    // Greek uppercase Alpha..Omega.
    0x391, 0x392, 0x393, 0x394, 0x395, 0x396, 0x397, 0x398, 0x399, 0x39a, 0x39b, 0x39c, 0x39d, 0x39e, 0x39f, 0x3a0,
    0x3a1, 0x3a3, 0x3a4, 0x3a5, 0x3a6, 0x3a7, 0x3a8, 0x3a9,
    // Operators/relations/misc.
    0xd7, 0xf7, 0xb1, 0xb7, 0xb0, 0x2213, 0x2264, 0x2265, 0x2260, 0x2248, 0x2261, 0x223c, 0x221d, 0x221e, 0x2202,
    0x2207, 0x2211, 0x220f, 0x222b, 0x221a, 0x2032, 0x2033, 0x2234, 0x2235, 0x22a5, 0x2225, 0x2218, 0x2297, 0x2295,
    0x230a, 0x230b, 0x2308, 0x2309, 0x22ef, 0x2026,
    // Arrows/logic/set theory.
    0x2192, 0x2190, 0x2194, 0x21d2, 0x21d0, 0x21d4, 0x2200, 0x2203, 0x2208, 0x2209, 0x2282, 0x2286, 0x2283, 0x2287,
    0x222a, 0x2229, 0x2205,
};

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
// Liberation Serif/Mono: the other 2 selectable font families
// (DocFormat::font_family, office_doc.h) -- real, distinct glyph atlases
// (office_font_data_serif.h/office_font_data_mono.h), same OFL-licensed
// family and baking convention as the Sans set above, not just a stored
// label with no visual effect.
Font g_office_font_serif_regular;
Font g_office_font_serif_bold;
Font g_office_font_serif_italic;
Font g_office_font_serif_bolditalic;
Font g_office_font_mono_regular;
Font g_office_font_mono_bold;
Font g_office_font_mono_italic;
Font g_office_font_mono_bolditalic;

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

// Which office-toolbar dropdown/popup is open (main.cpp's DrawPane office
// branch) -- same single-int convention as g_open_menu above (a real
// dropdown widget infra doesn't exist outside the purpose-built top menu
// bar, so this mirrors that pattern rather than building a second one).
// -1 = none; otherwise one of the kOfficeDropdown* IDs below. No
// click-away-closes handling (v1 simplification) -- a dropdown closes only
// when its own toggle button is clicked again or one of its items is
// picked.
enum {
    kOfficeDropdownFontFamily = 1,
    kOfficeDropdownFontSize,
    kOfficeDropdownTextColor,
    kOfficeDropdownHighlight,
    kOfficeDropdownSpecialChars,
};
int g_office_dropdown_open = -1;

// Populated by DrawPane's office branch (a full-document wrap-height scan
// -- see the comment where it's filled in) and consumed by that same
// pane's own Docs-style status footer (word/page count, zoom) right after,
// and by UpdateOfficeScrollbarInteraction (scrollbar hit-testing/drag)
// later the same frame. A reused scratch struct, not a persistent one: if
// several office panes are visible (a split), each one's DrawPane call
// fills it in and fully consumes it before the next pane's call reuses it
// -- so the only cross-pane sharing is UpdateOfficeScrollbarInteraction/
// the zoom-drag state only ever tracking whichever office pane was drawn
// last that frame, same "one active thing at a time" convention as
// g_office_dropdown_open.
struct OfficeStatusInfo {
    bool valid = false;
    int buffer_id = 0;
    int word_count = 0;
    int page_estimate = 1;
    int page_total_estimate = 1;
    float zoom = 1.0f;
    // Scrollbar geometry/state, screen pixels: `track` is the vertical
    // strip the thumb travels in; scroll_fraction/visible_fraction locate
    // the thumb within it (0..1, top-of-viewport / visible-height, both as
    // a fraction of the total document height).
    Rectangle track{};
    Rectangle thumb{};
    float scroll_fraction = 0.0f;
    float visible_fraction = 1.0f;
    float total_height = 0.0f;
    // Cached layout inputs (this frame's pane width/zoom-derived values) so
    // UpdateOfficeScrollbarInteraction can redo the same paragraph-height
    // walk DrawPane's own full-document scan does, without needing pane
    // pixel geometry of its own.
    float max_width = 0.0f;
    float body_size = 0.0f;
    // Scratch space for the Outline panel's two-pass row draw (position
    // first, then look up which one is "active" before redrawing that
    // row's highlight) -- cleared and repopulated fresh each time
    // DrawOfficeSidePanels draws the panel, not carried across frames.
    std::vector<std::pair<int, Rectangle>> outline_rows;
};
OfficeStatusInfo g_office_status;

// Scrollbar drag state -- same "global struct, threshold-free since a
// scrollbar thumb has no click-vs-drag ambiguity" idiom as g_pane_drag,
// but kept separate: dragging a scroll position is a different action
// shape (target a fraction, not resize a split/sidebar) from anything
// PaneDragState already models.
struct OfficeScrollDragState {
    bool active = false;
    float grab_offset_y = 0.0f;  // mouse.y - thumb.y at drag start, kept constant through the drag
};
OfficeScrollDragState g_office_scroll_drag;

// Docs-style status line's zoom-slider drag state -- same "continuous drag,
// handled directly where it's drawn rather than via a separate Update*"
// shape as the slider itself (self-contained, unlike the scrollbar which
// needed this frame's DrawPane geometry so it lives in a sibling Update*
// function instead).
bool g_office_zoom_drag = false;

// Outline (left) / Format-Insert (right) panel open/tab state -- plain
// globals, same "one office pane in view at a time" convention as
// g_office_dropdown_open, rather than per-buffer OfficeSession fields:
// this is transient UI chrome state, not document state worth persisting
// per buffer.
bool g_office_outline_open = false;
bool g_office_format_open = false;
int g_office_format_tab = 0;  // 0 = Format, 1 = Insert
constexpr float kOfficeRailW = 40.0f;
constexpr float kOfficeOutlineW = 240.0f;
constexpr float kOfficeFormatW = 260.0f;
constexpr float kOfficeFooterH = 32.0f;

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

// --- Pane/tab mouse interaction --------------------------------------------
//
// Three features share this: (1) click any pane's header/tab chip to
// focus that pane (and switch to that tab, for a multi-tab chip) -- goes
// through g_click_regions/RegisterClickRegion like everything else in
// that system, no new machinery needed; (2) drag a chip out and drop it
// on another pane -- center merges it in as a new tab there, an edge
// zone splits that pane and inserts a fresh leaf for it, with a live
// highlight showing which zone the mouse is over; (3) drag a pane border
// to resize the two panes on either side. (2) and (3) need real drag
// state (press, then motion over possibly many frames, then release) --
// g_click_regions has no notion of that (see its own header), so this is
// a second, parallel mouse-interaction system rather than an extension
// of it. All three read the SAME per-frame geometry capture
// (ComputePaneScreenRects, called from DrawEditor right alongside
// DrawPaneTree, whose layout math it mirrors exactly) since there's no
// other cached pixel-space pane geometry to hit-test against --
// Editor::ComputeRects is the same math but normalized-space-only and
// not captured per-frame, so it can't answer "which pane/border is the
// mouse over right now" without redoing the walk anyway.

// One leaf pane's on-screen rect this frame.
struct PaneScreenRect {
    int pane_id;
    Rectangle rect;
};
std::vector<PaneScreenRect> g_pane_screen_rects;

// One draggable header/tab-chip region this frame -- pushed for every
// chip in a multi-tab pane's strip AND for a single-buffer pane's whole
// plain header (dragging that pane's sole buffer counts too; see
// DrawPane's own two call sites for why both count as "a tab").
struct PaneTabChipRect {
    int pane_id;
    int buffer_id;
    Rectangle rect;
};
std::vector<PaneTabChipRect> g_pane_tab_chip_rects;

// One resizable border's on-screen extent this frame -- `node`/
// `child_index` identify which two adjacent SplitNode children
// (node->children[child_index] and [child_index+1]) this border sits
// between, for Editor::SetPaneBorderShare. `node` is a raw pointer into
// the live pane tree, safe to hold only for the remainder of THIS frame
// or the duration of one continuous drag gesture (nothing else can
// restructure the tree while the mouse holds a drag) -- never carried
// across a tree-mutating call.
struct PaneBorderRect {
    SplitNode *node;
    int child_index;
    bool vertical;       // true = a left/right (Vertical-split) border, dragged horizontally; false = top/bottom (Horizontal-split), dragged vertically
    Rectangle grab_rect;  // thin strip, for hover/drag-start hit-testing
    Rectangle pair_rect;  // combined on-screen extent of children[child_index] and [child_index+1] (NOT node's own full rect, which may span more siblings) -- for converting drag mouse position into a share fraction
};
std::vector<PaneBorderRect> g_pane_border_rects;

// Mirrors DrawPaneTree's own layout recursion (main.cpp, further down --
// keep any change to one in sync with the other) to capture this frame's
// pixel-space geometry for every leaf pane and every resizable border,
// without duplicating drawing work. Called from DrawEditor, right before
// DrawPaneTree itself.
void ComputePaneScreenRects(SplitNode *node, float x, float y, float w, float h) {
    if (node->dir == SplitDir::Leaf) {
        g_pane_screen_rects.push_back({node->pane.id, Rectangle{x, y, w, h}});
        return;
    }
    int n = static_cast<int>(node->children.size());
    if (n == 0) return;
    bool has_shares = node->shares.size() == static_cast<size_t>(n);
    constexpr float kBorderGrabPx = 6.0f;
    if (node->dir == SplitDir::Horizontal) {
        float cy = y;
        for (int i = 0; i < n; i++) {
            float ch = has_shares ? h * node->shares[i] : h / n;
            float next_y = (i == n - 1) ? y + h : cy + ch;
            ComputePaneScreenRects(node->children[i].get(), x, cy, w, next_y - cy);
            if (i < n - 1) {
                float ch_next = has_shares ? h * node->shares[i + 1] : h / n;
                g_pane_border_rects.push_back({node, i, false, Rectangle{x, next_y - kBorderGrabPx / 2.0f, w, kBorderGrabPx},
                                                Rectangle{x, cy, w, ch + ch_next}});
            }
            cy = next_y;
        }
    } else {
        float cx = x;
        for (int i = 0; i < n; i++) {
            float cw = has_shares ? w * node->shares[i] : w / n;
            float next_x = (i == n - 1) ? x + w : cx + cw;
            ComputePaneScreenRects(node->children[i].get(), cx, y, next_x - cx, h);
            if (i < n - 1) {
                float cw_next = has_shares ? w * node->shares[i + 1] : w / n;
                g_pane_border_rects.push_back({node, i, true, Rectangle{next_x - kBorderGrabPx / 2.0f, y, kBorderGrabPx, h},
                                                Rectangle{cx, y, cw + cw_next, h}});
            }
            cx = next_x;
        }
    }
}

// One sidebar row's on-screen rect this frame -- captured directly inside
// DrawSidebars (a flat per-sidebar loop, unlike the pane tree, so no
// separate recursion is needed the way ComputePaneScreenRects mirrors
// DrawPaneTree). Used for click-to-select-and-focus and double-click-to-
// activate hit-testing.
struct SidebarRowRect {
    int sidebar_id;
    int line_index;
    Rectangle rect;
};
std::vector<SidebarRowRect> g_sidebar_row_rects;

// One sidebar's resizable inner edge (the border facing the pane tree,
// not the screen edge -- there's nothing to drag the outer edge against)
// this frame, also captured directly inside DrawSidebars. `sign` is +1 if
// dragging in the increasing-x/y direction *grows* the sidebar, -1 if it
// shrinks it -- differs by dock position (see DrawSidebars' own comment
// at its push site for the reasoning per edge).
struct SidebarBorderRect {
    int sidebar_id;
    bool horizontal;  // true = left/right sidebar (dragged horizontally, resizes width); false = top/bottom (dragged vertically, resizes height)
    int sign;
    Rectangle grab_rect;
};
std::vector<SidebarBorderRect> g_sidebar_border_rects;

// Double-click detection state for sidebar rows -- GLFW/raylib has no
// built-in notion of a double-click, so this is the same "two presses
// within a short wall-clock window" idiom Editor::pending_ctrl_c_ already
// uses for its own Ctrl-C Ctrl-C chord (see its own header comment for
// why a timeout, not "next event clears it", is the right tool once
// timing rather than an intervening keystroke is what distinguishes the
// two cases).
double g_last_sidebar_click_time = -1.0;
int g_last_sidebar_click_id = -1;
int g_last_sidebar_click_row = -1;
constexpr double kDoubleClickThresholdSec = 0.4;

// Same double-click idiom, for double-clicking a Gantt row's label to
// rename it in place (UpdateGanttMouseInteraction). Keyed by headline
// index rather than a (sidebar id, row) pair since only one Gantt view is
// ever being interacted with at a time.
double g_last_gantt_click_time = -1.0;
int g_last_gantt_click_headline = -1;

// Which Kanban column (if any) has its "..." rename/delete popup open --
// (buffer_id, column_index) so two panes each showing a different board
// can't cross-talk through a single shared index, mirroring
// g_office_dropdown_open's single-open-popup convention otherwise.
int g_kanban_col_menu_buffer = -1;
int g_kanban_col_menu_col = -1;

// Raster Gantt exports are captured on the frame after their button click.
// That lets DrawGantt paint a clean chart (without the interactive focus
// row) before LoadImageFromScreen reads the framebuffer.
struct PendingGanttRasterExport {
    int buffer_id = -1;
    std::string format;
};
PendingGanttRasterExport g_pending_gantt_raster_export;
int g_clean_gantt_export_buffer = -1;

enum class PaneDropZone { Center, Left, Right, Top, Bottom };

// Which zone of `rect` (mouse-drag drop target) `mouse` is closest to --
// the middle kCenterThreshold-sized region is Center (merge as a new
// tab), otherwise whichever edge the point is nearest wins (split that
// direction), giving the familiar VSCode-style pinwheel of drop zones.
PaneDropZone ComputePaneDropZone(Vector2 mouse, const Rectangle &rect) {
    float fx = std::clamp((mouse.x - rect.x) / rect.width, 0.0f, 1.0f);
    float fy = std::clamp((mouse.y - rect.y) / rect.height, 0.0f, 1.0f);
    constexpr float kCenterThreshold = 0.25f;
    float m = fx;
    PaneDropZone zone = PaneDropZone::Left;
    if (1.0f - fx < m) { m = 1.0f - fx; zone = PaneDropZone::Right; }
    if (fy < m) { m = fy; zone = PaneDropZone::Top; }
    if (1.0f - fy < m) { m = 1.0f - fy; zone = PaneDropZone::Bottom; }
    return (m > kCenterThreshold) ? PaneDropZone::Center : zone;
}

enum class PaneDragKind { None, TabMove, BorderResize, SidebarResize };

struct PaneDragState {
    PaneDragKind kind = PaneDragKind::None;
    Vector2 start_pos{};
    bool threshold_passed = false;  // false until the mouse has moved far enough to count as a real drag, not just a click-in-place

    // TabMove fields.
    int source_pane_id = 0;
    int dragged_buffer_id = 0;
    int target_pane_id = -1;
    PaneDropZone drop_zone = PaneDropZone::Center;

    // BorderResize fields.
    SplitNode *border_node = nullptr;
    int border_child_index = 0;
    bool border_vertical = false;
    float border_pair_total = 0.0f;  // shares[child_index]+shares[child_index+1] at drag start, held constant through the drag
    Rectangle border_pair_rect{};    // pixel extent that pair spans, captured at drag start (for mouse position -> fraction)

    // SidebarResize fields.
    int sidebar_id = 0;
    bool sidebar_horizontal = false;
    int sidebar_sign = 1;
    int sidebar_size_start = 0;    // SidebarInstance::size (cells) at drag start
    float sidebar_mouse_start = 0;  // mouse.x (horizontal) or mouse.y (vertical) at drag start
};
PaneDragState g_pane_drag;

constexpr float kPaneDragThresholdPx = 4.0f;

void DrawPaneDragOverlay();  // defined below, alongside UpdatePaneMouseInteraction; called from DrawEditor

int LineHeight() { return static_cast<int>(g_font_size) + 6; }
int MenuBarHeight() { return static_cast<int>(g_font_size) + 12; }
int MenuItemHeight() { return static_cast<int>(g_font_size) + 8; }
float MenuFontSize() { return std::max(kMinFontSize, g_font_size - 2); }
int TabBarHeight() { return static_cast<int>(MenuFontSize()) + 10; }
int PaneHeaderHeight() { return static_cast<int>(MenuFontSize()) + 8; }
// A Gantt row's height, derived from the live font size rather than a
// fixed constant (kGanttRowHeight used to be 28px flat) so a row's label
// never clips regardless of zoom -- same +16 padding proportion that
// constant was tuned at around the default font size. DrawGantt and
// UpdateGanttMouseInteraction both call this independently rather than
// sharing a cached value, matching how they already both call
// PaneHeaderHeight() separately.
// Each Gantt row carries both a task title and its visible scheduled/deadline
// range, so reserve a compact second line for the latter.
int GanttRowHeight() { return static_cast<int>(g_font_size * 2.0f) + 16; }

std::string GanttDateLabel(const OrgTimestamp &date) {
    if (!date.present) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", date.year, date.month, date.day);
    return buf;
}

const char *GanttRulerScaleName(GanttSession::RulerScale scale) {
    switch (scale) {
        case GanttSession::RulerScale::Days: return "Days";
        case GanttSession::RulerScale::Months: return "Months";
        case GanttSession::RulerScale::Years: return "Years";
    }
    return "Days";
}

int GanttRulerHeight(const GanttSession &sess) {
    // Month cells need a dedicated year band above their labels; the other
    // scales retain the compact one-line ruler.
    return PaneHeaderHeight() * (sess.ruler_scale == GanttSession::RulerScale::Months ? 2 : 1);
}

constexpr float kGanttBarHeight = 12.0f;

const char *GanttMonthAbbrev(int month) {
    static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return month >= 1 && month <= 12 ? kMonths[month - 1] : "";
}

long long GanttTodayDay() {
    std::time_t now = std::time(nullptr);
    std::tm *local = std::localtime(&now);
    return local ? OrgDayNumber(local->tm_year + 1900, local->tm_mon + 1, local->tm_mday) : 0;
}

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

std::string GanttExportPath(int buffer_id, const char *extension) {
    std::filesystem::path path(g_editor.GetBuffer(buffer_id).filename);
    if (path.empty()) path = "gantt";
    path.replace_extension();
    return path.string() + ".gantt." + extension;
}

std::string SvgEscape(const std::string &text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string SvgColor(Color color) {
    char out[8];
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", color.r, color.g, color.b);
    return out;
}

// Writes the currently visible Gantt canvas as editable SVG. It deliberately
// uses the session's rendered viewport (rather than inventing pagination), so
// exported grid lines, scale and labels match exactly what the user sees.
bool ExportGanttSvg(int buffer_id, const std::string &path) {
    const GanttSession *sess = g_editor.GetGantt(buffer_id);
    if (!sess || sess->content_w <= 0 || sess->content_h <= 0) return false;
    int width = static_cast<int>(sess->content_w), height = static_cast<int>(sess->content_h);
    float label_w = sess->label_col_w;
    float timeline_w = std::max(0.0f, sess->content_w - label_w);
    float ruler_h = static_cast<float>(GanttRulerHeight(*sess));
    int row_h = GanttRowHeight();
    std::ofstream out(path);
    if (!out) return false;
    Color bg = ResolveHlGroup("NormalBg"), border = ResolveHlGroup("Border"), accent = ResolveHlGroup("BorderActive");
    Color normal = ResolveHlGroup("Normal"), comment = ResolveHlGroup("Comment");
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
        << "\" viewBox=\"0 0 " << width << " " << height << "\"><rect width=\"100%\" height=\"100%\" fill=\""
        << SvgColor(bg) << "\"/>\n";
    int visible_days = std::max(1, static_cast<int>(timeline_w / sess->pixels_per_day) + 2);
    for (int i = 0; i < visible_days; ++i) {
        int yy, mm, dd;
        OrgDateFromDayNumber(sess->anchor_day + i, yy, mm, dd);
        bool month = dd == 1, year = month && mm == 1;
        bool show = sess->ruler_scale == GanttSession::RulerScale::Days ||
                    (sess->ruler_scale == GanttSession::RulerScale::Months && month) ||
                    (sess->ruler_scale == GanttSession::RulerScale::Years && year);
        if (!show) continue;
        float gx = label_w + i * sess->pixels_per_day;
        out << "<path d=\"M " << gx << " " << ruler_h << " V " << height << "\" stroke=\""
            << SvgColor(sess->ruler_scale == GanttSession::RulerScale::Days ? border : accent) << "\"/>\n";
        char label[16] = {};
        if (sess->ruler_scale == GanttSession::RulerScale::Days && sess->pixels_per_day > 14.0f) std::snprintf(label, sizeof(label), "%02d", dd);
        if (sess->ruler_scale == GanttSession::RulerScale::Months) std::snprintf(label, sizeof(label), "%04d-%02d", yy, mm);
        if (sess->ruler_scale == GanttSession::RulerScale::Years) std::snprintf(label, sizeof(label), "%04d", yy);
        if (*label) out << "<text x=\"" << gx + 2 << "\" y=\"" << ruler_h - 7
                         << "\" font-family=\"monospace\" font-size=\"12\" fill=\"" << SvgColor(comment)
                         << "\">" << label << "</text>\n";
    }
    out << "<path d=\"M " << label_w << " " << ruler_h << " H " << width << "\" stroke=\"" << SvgColor(border)
        << "\"/><path d=\"M " << label_w << " 0 V " << height << "\" stroke=\"" << SvgColor(border) << "\"/>\n";
    std::vector<int> rows = g_editor.GanttRows(buffer_id);
    std::unordered_map<std::string, int> id_to_headline;
    std::unordered_map<int, int> headline_to_row;
    for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
        headline_to_row[rows[ri]] = ri;
        if (!sess->outline.headlines[rows[ri]].id.empty()) id_to_headline[sess->outline.headlines[rows[ri]].id] = rows[ri];
    }
    for (int target_hi : rows) {
        const OrgHeadline &target = sess->outline.headlines[target_hi];
        for (const std::string &blocker : target.blockers) {
            auto source_it = id_to_headline.find(blocker);
            if (source_it == id_to_headline.end()) continue;
            const OrgHeadline &source = sess->outline.headlines[source_it->second];
            long long source_end = source.deadline.present ? OrgDayNumber(source.deadline.year, source.deadline.month, source.deadline.day)
                                                            : OrgDayNumber(source.scheduled.year, source.scheduled.month, source.scheduled.day);
            long long target_start = OrgDayNumber(target.scheduled.year, target.scheduled.month, target.scheduled.day);
            float sx = label_w + static_cast<float>(source_end - sess->anchor_day) * sess->pixels_per_day;
            float tx = label_w + static_cast<float>(target_start - sess->anchor_day) * sess->pixels_per_day;
            float sy = ruler_h + headline_to_row[source_it->second] * row_h + row_h / 2.0f;
            float ty = ruler_h + headline_to_row[target_hi] * row_h + row_h / 2.0f;
            float bend = std::max(sx + 12.0f, tx - 12.0f);
            out << "<path d=\"M " << sx << " " << sy << " H " << bend << " V " << ty << " H " << tx
                << "\" fill=\"none\" stroke=\"" << SvgColor(accent) << "\" stroke-width=\"2\"/>\n";
            out << "<path d=\"M " << tx << " " << ty << " L " << tx - 7 << " " << ty - 4 << " L " << tx - 7 << " "
                << ty + 4 << " Z\" fill=\"" << SvgColor(accent) << "\"/>\n";
        }
    }
    for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
        int hi = rows[ri];
        const OrgHeadline &hd = sess->outline.headlines[hi];
        float row_y = ruler_h + ri * row_h;
        long long start = OrgDayNumber(hd.scheduled.year, hd.scheduled.month, hd.scheduled.day);
        float bar_x = label_w + static_cast<float>(start - sess->anchor_day) * sess->pixels_per_day;
        float bar_w = hd.deadline.present
                          ? std::max(4.0f, static_cast<float>(OrgDayNumber(hd.deadline.year, hd.deadline.month, hd.deadline.day) - start) * sess->pixels_per_day)
                          : 8.0f;
        std::string dates = "Start " + GanttDateLabel(hd.scheduled) + (hd.deadline.present ? "  →  Due " + GanttDateLabel(hd.deadline) : "  →  Due —");
        out << "<text x=\"6\" y=\"" << row_y + 16 << "\" font-family=\"monospace\" font-size=\"14\" fill=\""
            << SvgColor(normal) << "\">" << SvgEscape(hd.title) << "</text><text x=\"6\" y=\"" << row_y + 31
            << "\" font-family=\"monospace\" font-size=\"11\" fill=\"" << SvgColor(comment) << "\">"
            << SvgEscape(dates) << "</text>\n";
        out << "<rect x=\"" << bar_x << "\" y=\"" << row_y + 5 << "\" width=\"" << bar_w << "\" height=\""
            << row_h - 10 << "\" rx=\"2\" fill=\"" << SvgColor(hd.is_done_keyword ? comment : accent) << "\"/>\n";
    }
    out << "</svg>\n";
    return static_cast<bool>(out);
}

bool ExportJpegPdf(Image image, const std::string &path) {
    // ExportImageToMemory in raylib 5 only implements PNG, even when the
    // regular file exporter supports JPEG. Use a short-lived JPEG next to
    // the destination and embed its bytes in the PDF, then remove it.
    std::string jpeg_path = path + ".mep-export.jpg";
    if (!ExportImage(image, jpeg_path.c_str())) return false;
    std::ifstream jpeg_file(jpeg_path, std::ios::binary);
    std::vector<unsigned char> jpeg((std::istreambuf_iterator<char>(jpeg_file)), std::istreambuf_iterator<char>());
    jpeg_file.close();
    std::error_code remove_error;
    std::filesystem::remove(jpeg_path, remove_error);
    if (jpeg.empty()) return false;
    int bytes = static_cast<int>(jpeg.size());
    int width = image.width, height = image.height;
    std::ostringstream objects;
    std::vector<long long> offsets;
    auto object = [&](int id, const std::string &body) {
        offsets.push_back(static_cast<long long>(objects.tellp()));
        objects << id << " 0 obj\n" << body << "\nendobj\n";
    };
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(width) + " " + std::to_string(height) +
                  "] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>");
    offsets.push_back(static_cast<long long>(objects.tellp()));
    objects << "4 0 obj\n<< /Type /XObject /Subtype /Image /Width " << width << " /Height " << height
            << " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " << bytes << " >>\nstream\n";
    objects.write(reinterpret_cast<const char *>(jpeg.data()), bytes);
    objects << "\nendstream\nendobj\n";
    // The JPEG stream emitted by raylib is already oriented for PDF image
    // painting; using a positive Y scale avoids vertically flipping it.
    std::string content = "q\n" + std::to_string(width) + " 0 0 " + std::to_string(height) + " 0 0 cm\n/Im0 Do\nQ\n";
    object(5, "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "%PDF-1.4\n";
    std::string body = objects.str();
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    long long xref = static_cast<long long>(std::strlen("%PDF-1.4\n")) + static_cast<long long>(body.size());
    out << "xref\n0 6\n0000000000 65535 f \n";
    for (long long offset : offsets) {
        char entry[32];
        std::snprintf(entry, sizeof(entry), "%010lld 00000 n \n", offset + static_cast<long long>(std::strlen("%PDF-1.4\n")));
        out << entry;
    }
    out << "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    return static_cast<bool>(out);
}

void ExportGanttRaster(int buffer_id, const char *extension) {
    const GanttSession *sess = g_editor.GetGantt(buffer_id);
    if (!sess) return;
    Image image = LoadImageFromScreen();
    ImageCrop(&image, Rectangle{sess->content_x, sess->content_y, sess->content_w, sess->content_h});
    std::string path = GanttExportPath(buffer_id, extension);
    bool ok = std::string(extension) == "pdf" ? ExportJpegPdf(image, path) : ExportImage(image, path.c_str());
    UnloadImage(image);
    g_editor.SetStatusMessage(ok ? "Exported Gantt to " + path : "Gantt export failed: " + path);
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
        case Mode::Html: return "HTML";
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

// The part of `path` after its last '/', or the whole thing if there isn't
// one -- DrawPane's own pane-header label (both the single-buffer and
// per-pane buffer-tab-strip cases) shows just this, not the full path.
std::string Basename(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Draws UI chrome text (sidebar widget rows, tab labels, toasts, ...) that
// may have an icon glyph (IconForFilename et al.) mixed into an otherwise
// ordinary string -- decodes by Unicode codepoint (GetCodepointNext, same
// as DrawLineFast's own reasoning) and routes each one to g_icon_font or
// g_font depending on IsIconCodepoint, since g_font's own atlas is ASCII-
// only (ApplyFontSize) and would just draw nothing for a PUA codepoint.
// Proportional advance (MeasureTextEx per glyph), not DrawLineFast's fixed-
// column grid -- this is chrome text, not a buffer line. `measure_only`
// skips the actual DrawTextEx calls but still walks/measures every glyph,
// so MeasureUiText below can share this same implementation instead of
// duplicating the decode loop. Returns the total width drawn/measured, the
// same thing a plain MeasureTextEx(g_font, text, ...).x would for an
// icon-free string, so callers that need it (click-region sizing, right-
// alignment, toast box width) don't need a second pass.
float DrawUiText(const std::string &text, Vector2 pos, float font_size, Color tint, bool measure_only = false) {
    float x = pos.x;
    const char *s = text.c_str();
    int len = static_cast<int>(text.size());
    for (int i = 0; i < len;) {
        int cp_size = 0;
        int cp = GetCodepointNext(&s[i], &cp_size);
        std::string glyph(s + i, cp_size);
        i += cp_size;
        const Font &f = IsIconCodepoint(cp) ? g_icon_font : g_font;
        if (!measure_only) DrawTextEx(f, glyph.c_str(), Vector2{x, pos.y}, font_size, 0, tint);
        x += MeasureTextEx(f, glyph.c_str(), font_size, 0).x;
    }
    return x - pos.x;
}

float MeasureUiText(const std::string &text, float font_size) {
    return DrawUiText(text, Vector2{0, 0}, font_size, BLANK, true);
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

    if (g_icon_font.texture.id != 0) UnloadFont(g_icon_font);
    // LoadFontFromMemory's codepoints parameter is (non-const) int* even
    // though it only ever reads from it -- const_cast is safe here since
    // kIconCodepoints is a compile-time array raylib can't actually
    // mutate through that pointer regardless of the signature.
    g_icon_font = LoadFontFromMemory(".ttf", kIconFontTtf, static_cast<int>(kIconFontTtfLen),
                                      static_cast<int>(g_font_size * 2), const_cast<int *>(kIconCodepoints),
                                      static_cast<int>(sizeof(kIconCodepoints) / sizeof(kIconCodepoints[0])));
    SetTextureFilter(g_icon_font.texture, TEXTURE_FILTER_BILINEAR);

    if (g_math_font.texture.id != 0) UnloadFont(g_math_font);
    constexpr int kMathExtraCount = sizeof(kMathCodepoints) / sizeof(kMathCodepoints[0]);
    static int math_codepoints[95 + kMathExtraCount];
    for (int c = 32; c <= 126; c++) math_codepoints[c - 32] = c;
    for (int i = 0; i < kMathExtraCount; i++) math_codepoints[95 + i] = kMathCodepoints[i];
    g_math_font = LoadFontFromMemory(".ttf", kJetBrainsMonoRegularTtf, static_cast<int>(kJetBrainsMonoRegularTtfLen),
                                      static_cast<int>(g_font_size * 2), math_codepoints,
                                      95 + kMathExtraCount);
    SetTextureFilter(g_math_font.texture, TEXTURE_FILTER_BILINEAR);
}

// The office pane's special-character insert palette (main.cpp's DrawPane
// office toolbar) -- also fed into LoadOfficeFonts' own codepoint list
// below so these actually render as their real glyphs, not raylib's
// missing-glyph fallback box, both in the palette popup itself and once
// inserted into a paragraph's own text (Editor::InsertOfficeText). U+2022
// BULLET is listed separately (kOfficeExtraCodepoints doesn't repeat it)
// since office_doc's own bullet-list rendering already needed it before
// this palette existed.
constexpr int kOfficeSpecialChars[] = {
    0x00A9, 0x00AE, 0x2122, 0x20AC, 0x00A3, 0x00A5, 0x00A7, 0x00B6,  // (c)(r)(tm) EUR GBP YEN section pilcrow
    0x00B0, 0x00B1, 0x00D7, 0x00F7, 0x2248, 0x2260, 0x2264, 0x2265,  // deg +- x div ~= != <= >=
    0x2026, 0x2013, 0x2014, 0x00BD, 0x00BC, 0x00BE, 0x2192, 0x2190,  // ... en-dash em-dash 1/2 1/4 3/4 -> <-
    0x2191, 0x2193, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03C0, 0x03A9,  // up down alpha beta gamma delta pi Omega
};
constexpr int kOfficeSpecialCharCount = sizeof(kOfficeSpecialChars) / sizeof(kOfficeSpecialChars[0]);

// Bakes the 4 office-pane fonts once, at startup only (see g_office_font_*'s
// own comment for why -- no ApplyFontSize-style reload-per-size-change).
// Baked at a fixed oversampled size (kOfficeFontBasePt * 2, mirroring
// ApplyFontSize's own 2x-supersample convention) large enough to cover the
// biggest heading size any session's zoom is likely to reach without
// visibly blurring when DrawTextEx scales the atlas down.
void LoadOfficeFonts() {
    constexpr int kOfficeFontBasePt = 48;
    // Default ASCII range (32..126, matching raylib's own nullptr-codepoints
    // default) plus U+2022 BULLET (office_doc's bullet-list rendering draws
    // it directly) plus the special-character palette's own set -- none of
    // those are in the ASCII range either, and would otherwise fall back to
    // Liberation Sans's "missing glyph" box both in the palette popup and
    // once actually inserted into a paragraph.
    constexpr int kCodepointCount = 96 + kOfficeSpecialCharCount;
    static int codepoints[kCodepointCount];
    for (int c = 32; c <= 126; c++) codepoints[c - 32] = c;
    codepoints[95] = 0x2022;
    for (int i = 0; i < kOfficeSpecialCharCount; i++) codepoints[96 + i] = kOfficeSpecialChars[i];
    g_office_font_regular = LoadFontFromMemory(".ttf", kLiberationSansRegularTtf,
                                                static_cast<int>(kLiberationSansRegularTtfLen),
                                                kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_bold = LoadFontFromMemory(".ttf", kLiberationSansBoldTtf,
                                             static_cast<int>(kLiberationSansBoldTtfLen),
                                             kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_italic = LoadFontFromMemory(".ttf", kLiberationSansItalicTtf,
                                               static_cast<int>(kLiberationSansItalicTtfLen),
                                               kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_bolditalic = LoadFontFromMemory(".ttf", kLiberationSansBoldItalicTtf,
                                                   static_cast<int>(kLiberationSansBoldItalicTtfLen),
                                                   kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_serif_regular = LoadFontFromMemory(".ttf", kLiberationSerifRegularTtf,
                                                       static_cast<int>(kLiberationSerifRegularTtfLen),
                                                       kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_serif_bold = LoadFontFromMemory(".ttf", kLiberationSerifBoldTtf,
                                                    static_cast<int>(kLiberationSerifBoldTtfLen),
                                                    kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_serif_italic = LoadFontFromMemory(".ttf", kLiberationSerifItalicTtf,
                                                      static_cast<int>(kLiberationSerifItalicTtfLen),
                                                      kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_serif_bolditalic = LoadFontFromMemory(".ttf", kLiberationSerifBoldItalicTtf,
                                                          static_cast<int>(kLiberationSerifBoldItalicTtfLen),
                                                          kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_mono_regular = LoadFontFromMemory(".ttf", kLiberationMonoRegularTtf,
                                                      static_cast<int>(kLiberationMonoRegularTtfLen),
                                                      kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_mono_bold = LoadFontFromMemory(".ttf", kLiberationMonoBoldTtf,
                                                   static_cast<int>(kLiberationMonoBoldTtfLen),
                                                   kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_mono_italic = LoadFontFromMemory(".ttf", kLiberationMonoItalicTtf,
                                                     static_cast<int>(kLiberationMonoItalicTtfLen),
                                                     kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    g_office_font_mono_bolditalic = LoadFontFromMemory(".ttf", kLiberationMonoBoldItalicTtf,
                                                         static_cast<int>(kLiberationMonoBoldItalicTtfLen),
                                                         kOfficeFontBasePt * 2, codepoints, kCodepointCount);
    Font *all_office_fonts[] = {&g_office_font_regular,       &g_office_font_bold,
                                 &g_office_font_italic,        &g_office_font_bolditalic,
                                 &g_office_font_serif_regular, &g_office_font_serif_bold,
                                 &g_office_font_serif_italic,  &g_office_font_serif_bolditalic,
                                 &g_office_font_mono_regular,  &g_office_font_mono_bold,
                                 &g_office_font_mono_italic,   &g_office_font_mono_bolditalic};
    for (Font *f : all_office_fonts) SetTextureFilter(f->texture, TEXTURE_FILTER_BILINEAR);
}

// Which of the 12 baked fonts (3 families x 4 weight/style) a run of text
// with this format draws with -- strike-through has no dedicated glyph
// variant, drawn as an overlay line instead (see the office DrawPane
// branch), so it doesn't affect font selection.
Font &OfficeFontFor(const DocFormat &fmt) {
    if (fmt.font_family == OfficeFontFamily::Serif) {
        if (fmt.bold && fmt.italic) return g_office_font_serif_bolditalic;
        if (fmt.bold) return g_office_font_serif_bold;
        if (fmt.italic) return g_office_font_serif_italic;
        return g_office_font_serif_regular;
    }
    if (fmt.font_family == OfficeFontFamily::Mono) {
        if (fmt.bold && fmt.italic) return g_office_font_mono_bolditalic;
        if (fmt.bold) return g_office_font_mono_bold;
        if (fmt.italic) return g_office_font_mono_italic;
        return g_office_font_mono_regular;
    }
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

// Extra vertical space a paragraph's anchored table/image (DocParagraph::
// table_ref/image_ref) adds *below* its own wrapped text lines -- shared
// by the scroll-follow scan and the real draw loop (both in DrawPane's
// Office branch) so they can't drift apart. Previously only the draw loop
// knew about this (the scan summed wrapped-line heights alone), so a
// paragraph with a tall image/table below it made the scroll-follow scan
// think less screen space was used than really was -- the cursor could
// walk visibly off-screen with no scroll happening at all ("stays
// fixed"), then the eventual catch-up (once the *undercounted* running
// total finally tripped content_h on its own) had to cover the whole
// missed distance in one shot ("jumps to the end of it"). Mirrors the
// draw loop's own height math exactly (image scaled down to fit
// max_width, same +8.0f gaps) without touching a Texture2D -- natural_w/
// natural_h come straight from DocImage, so this needs no GPU load.
float OfficeParagraphExtraHeight(const OfficeDoc &doc, const DocParagraph &para, float max_width, float body_size) {
    float extra = 0.0f;
    if (para.table_ref >= 0 && para.table_ref < static_cast<int>(doc.tables.size())) {
        const DocTable &tbl = doc.tables[static_cast<size_t>(para.table_ref)];
        float cell_font = body_size * 0.85f;
        float cell_h = cell_font + 12.0f;
        extra += cell_h * static_cast<float>(tbl.rows) + 8.0f;
    }
    if (para.image_ref >= 0 && para.image_ref < static_cast<int>(doc.images.size())) {
        const DocImage &img = doc.images[static_cast<size_t>(para.image_ref)];
        if (img.natural_w > 0 && img.natural_h > 0) {
            float draw_w = static_cast<float>(img.natural_w);
            float draw_h = static_cast<float>(img.natural_h);
            if (draw_w > max_width) draw_h *= max_width / draw_w;
            extra += draw_h + 8.0f;
        }
    }
    return extra;
}

// One contiguous [start,end) run within a paragraph sharing one DocFormat
// -- the renderer's unit of a single DrawTextEx call, built by walking
// p.spans (already sorted/non-overlapping, see office_doc.h) and filling
// the gaps between them with a default-format run.
struct OfficeFormatRun {
    int start = 0, end = 0;
    DocFormat fmt;
    // Presentation-only flag: this run came from an ordinary `$...$`
    // delimiter pair, not a persisted DocSpan. The delimiters are omitted
    // while typeset but remain in DocParagraph::text for source editing.
    bool dollar_delimited_math = false;
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

// Builds the WYSIWYG runs for one visual line. Plain text may opt into the
// editor's small MathJax-compatible subset with `$...$`; escaped dollars and
// `$$` display blocks deliberately remain literal here. Persisted math spans
// (created by the fx command) already carry DocFormat::math and pass through.
std::vector<OfficeFormatRun> BuildOfficeDisplayRuns(const DocParagraph &p, int a, int b,
                                                     bool reveal_active_source, int cursor_col) {
    std::vector<OfficeFormatRun> out;
    for (const OfficeFormatRun &base : BuildOfficeFormatRuns(p, a, b)) {
        if (base.fmt.math) { out.push_back(base); continue; }
        int pos = base.start;
        while (pos < base.end) {
            int open = -1;
            for (int i = pos; i < base.end; ++i) {
                if (p.text[i] != '$' || (i > base.start && p.text[i - 1] == '\\')) continue;
                if ((i + 1 < base.end && p.text[i + 1] == '$') || (i > base.start && p.text[i - 1] == '$')) continue;
                open = i; break;
            }
            if (open < 0) { if (pos < base.end) out.push_back({pos, base.end, base.fmt}); break; }
            int close = -1;
            for (int i = open + 1; i < base.end; ++i) {
                if (p.text[i] != '$' || p.text[i - 1] == '\\') continue;
                if ((i + 1 < base.end && p.text[i + 1] == '$') || p.text[i - 1] == '$') continue;
                close = i; break;
            }
            if (close < 0) { out.push_back({pos, base.end, base.fmt}); break; }
            if (open > pos) out.push_back({pos, open, base.fmt});
            // Cursor at either delimiter counts as being inside the chunk;
            // this makes it possible to correct/remove the delimiters too.
            if (reveal_active_source && cursor_col >= open && cursor_col <= close + 1) {
                out.push_back({open, close + 1, base.fmt});
            } else {
                DocFormat math_format = base.fmt;
                math_format.math = true;
                out.push_back({open + 1, close, math_format, true});
            }
            pos = close + 1;
        }
    }
    return out;
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

// Converts a character-column index into a byte offset within `line`,
// walking codepoint-by-codepoint the same way DrawLineFast (above) does --
// needed wherever a column index (Decoration::col_start/col_end, e.g.
// Editor::EnterTerminalNormalMode's captured terminal-color runs) has to
// become a std::string::substr byte range instead. Only coincides with the
// column index itself for pure-ASCII content (every other decoration
// consumer already assumes that, which holds for typical source code, but
// not for terminal output, which routinely isn't ASCII-only).
size_t ColumnToByteOffset(const std::string &line, int col) {
    if (col <= 0) return 0;
    const char *text = line.c_str();
    int byte_len = static_cast<int>(line.size());
    int column = 0;
    int i = 0;
    while (i < byte_len && column < col) {
        int codepoint_size = 0;
        GetCodepointNext(&text[i], &codepoint_size);
        i += codepoint_size;
        column++;
    }
    return static_cast<size_t>(i);
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
// Directory/tree/UI-action icon set (Phase 10). dir_open/dir_closed (the
// only two of this table actually consumed, by mep.tree_refresh below) are
// real Nerd Font glyphs now that g_icon_font exists (main.cpp's
// kIconCodepoints) -- tree_expand/tree_collapse/notify/todo/tests/git/add/
// clear stay plain ASCII placeholders since nothing reads them yet.
// Per-file icons go through mep.icon_for_file() (extension table lives in
// C++); this small fixed set doesn't need a lookup table, just constants.
const char *kBuiltinIcons =
    "mep.icons = {\n"
    "  dir_open = '', dir_closed = '', tree_expand = '>', tree_collapse = 'v',\n"
    "  notify = 'i', todo = 'o', tests = 'T', git = 'G', add = '+', clear = 'x',\n"
    "}\n";

// Colorizer + URL detection/open (Phase 13). Both are plain-Lua consumers
// of existing primitives (decorations for swatches, jobs for opening a
// URL) -- no new C++ needed beyond the swatch-rendering support in
// Decoration/DrawPane and mep.platform() for picking xdg-open/open/start.
const char *kBuiltinTextTools =
    // CSS3/SVG named-color keywords (148 entries, incl. both gray/grey
    // spellings and rebeccapurple) -- NVIM_PARITY_PLAN.md Phase 13 gap:
    // colorizer previously only recognized hex/rgb()/rgba() literals.
    "local MEP_CSS_COLORS = {\n"
    "  aliceblue=\"f0f8ff\", antiquewhite=\"faebd7\", aqua=\"00ffff\", aquamarine=\"7fffd4\",\n"
    "  azure=\"f0ffff\", beige=\"f5f5dc\", bisque=\"ffe4c4\", black=\"000000\",\n"
    "  blanchedalmond=\"ffebcd\", blue=\"0000ff\", blueviolet=\"8a2be2\", brown=\"a52a2a\",\n"
    "  burlywood=\"deb887\", cadetblue=\"5f9ea0\", chartreuse=\"7fff00\", chocolate=\"d2691e\",\n"
    "  coral=\"ff7f50\", cornflowerblue=\"6495ed\", cornsilk=\"fff8dc\", crimson=\"dc143c\",\n"
    "  cyan=\"00ffff\", darkblue=\"00008b\", darkcyan=\"008b8b\", darkgoldenrod=\"b8860b\",\n"
    "  darkgray=\"a9a9a9\", darkgreen=\"006400\", darkgrey=\"a9a9a9\", darkkhaki=\"bdb76b\",\n"
    "  darkmagenta=\"8b008b\", darkolivegreen=\"556b2f\", darkorange=\"ff8c00\", darkorchid=\"9932cc\",\n"
    "  darkred=\"8b0000\", darksalmon=\"e9967a\", darkseagreen=\"8fbc8f\", darkslateblue=\"483d8b\",\n"
    "  darkslategray=\"2f4f4f\", darkslategrey=\"2f4f4f\", darkturquoise=\"00ced1\", darkviolet=\"9400d3\",\n"
    "  deeppink=\"ff1493\", deepskyblue=\"00bfff\", dimgray=\"696969\", dimgrey=\"696969\",\n"
    "  dodgerblue=\"1e90ff\", firebrick=\"b22222\", floralwhite=\"fffaf0\", forestgreen=\"228b22\",\n"
    "  fuchsia=\"ff00ff\", gainsboro=\"dcdcdc\", ghostwhite=\"f8f8ff\", gold=\"ffd700\",\n"
    "  goldenrod=\"daa520\", gray=\"808080\", green=\"008000\", greenyellow=\"adff2f\",\n"
    "  grey=\"808080\", honeydew=\"f0fff0\", hotpink=\"ff69b4\", indianred=\"cd5c5c\",\n"
    "  indigo=\"4b0082\", ivory=\"fffff0\", khaki=\"f0e68c\", lavender=\"e6e6fa\",\n"
    "  lavenderblush=\"fff0f5\", lawngreen=\"7cfc00\", lemonchiffon=\"fffacd\", lightblue=\"add8e6\",\n"
    "  lightcoral=\"f08080\", lightcyan=\"e0ffff\", lightgoldenrodyellow=\"fafad2\", lightgray=\"d3d3d3\",\n"
    "  lightgreen=\"90ee90\", lightgrey=\"d3d3d3\", lightpink=\"ffb6c1\", lightsalmon=\"ffa07a\",\n"
    "  lightseagreen=\"20b2aa\", lightskyblue=\"87cefa\", lightslategray=\"778899\", lightslategrey=\"778899\",\n"
    "  lightsteelblue=\"b0c4de\", lightyellow=\"ffffe0\", lime=\"00ff00\", limegreen=\"32cd32\",\n"
    "  linen=\"faf0e6\", magenta=\"ff00ff\", maroon=\"800000\", mediumaquamarine=\"66cdaa\",\n"
    "  mediumblue=\"0000cd\", mediumorchid=\"ba55d3\", mediumpurple=\"9370db\", mediumseagreen=\"3cb371\",\n"
    "  mediumslateblue=\"7b68ee\", mediumspringgreen=\"00fa9a\", mediumturquoise=\"48d1cc\", mediumvioletred=\"c71585\",\n"
    "  midnightblue=\"191970\", mintcream=\"f5fffa\", mistyrose=\"ffe4e1\", moccasin=\"ffe4b5\",\n"
    "  navajowhite=\"ffdead\", navy=\"000080\", oldlace=\"fdf5e6\", olive=\"808000\",\n"
    "  olivedrab=\"6b8e23\", orange=\"ffa500\", orangered=\"ff4500\", orchid=\"da70d6\",\n"
    "  palegoldenrod=\"eee8aa\", palegreen=\"98fb98\", paleturquoise=\"afeeee\", palevioletred=\"db7093\",\n"
    "  papayawhip=\"ffefd5\", peachpuff=\"ffdab9\", peru=\"cd853f\", pink=\"ffc0cb\",\n"
    "  plum=\"dda0dd\", powderblue=\"b0e0e6\", purple=\"800080\", rebeccapurple=\"663399\",\n"
    "  red=\"ff0000\", rosybrown=\"bc8f8f\", royalblue=\"4169e1\", saddlebrown=\"8b4513\",\n"
    "  salmon=\"fa8072\", sandybrown=\"f4a460\", seagreen=\"2e8b57\", seashell=\"fff5ee\",\n"
    "  sienna=\"a0522d\", silver=\"c0c0c0\", skyblue=\"87ceeb\", slateblue=\"6a5acd\",\n"
    "  slategray=\"708090\", slategrey=\"708090\", snow=\"fffafa\", springgreen=\"00ff7f\",\n"
    "  steelblue=\"4682b4\", tan=\"d2b48c\", teal=\"008080\", thistle=\"d8bfd8\",\n"
    "  tomato=\"ff6347\", turquoise=\"40e0d0\", violet=\"ee82ee\", wheat=\"f5deb3\",\n"
    "  white=\"ffffff\", whitesmoke=\"f5f5f5\", yellow=\"ffff00\", yellowgreen=\"9acd32\",\n"
    "}\n"
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
    // Frontier-pattern word match so e.g. 'tan' inside 'instant' or 'red'
    // inside 'credit' don't false-positive -- exact lowercase keyword
    // only (CSS names are case-insensitive, but matching case-sensitively
    // avoids flagging identifiers like a `Red` class/variable name).
    "    for s, word in line:gmatch('()%f[%a](%a+)%f[%A]') do\n"
    "      local hexv = MEP_CSS_COLORS[word]\n"
    "      if hexv and not covered[s] then\n"
    "        local r, g, b = tonumber(hexv:sub(1,2),16), tonumber(hexv:sub(3,4),16), tonumber(hexv:sub(5,6),16)\n"
    "        mep.deco_add(mep_colorizer_ns, {row=i, col_start=s, col_end=s+#word, color={r,g,b}})\n"
    "      end\n"
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
    "end\n"
    // External browser window: unlike mep.open_url (shells out to
    // xdg-open/open/start, whatever the OS's own default browser is),
    // mep.browse opens `target` (a real URL, or a local path -- e.g. an
    // .html file under edit) in a dedicated native window mep itself
    // controls, with full JS execution -- launcher/browser.ts, running the
    // same jsr:@webview/webview + WebKitGTK stack the wasm build's own
    // launcher/webview_worker.ts already uses to host mep.html, just
    // pointed at arbitrary content instead. $MEP_BROWSER_LAUNCHER (set by
    // the justfile's `run` recipe to launcher/browser.ts's absolute path)
    // locates that script; a bare relative fallback below still works for
    // anyone running the built binary straight from a repo checkout
    // without `just run`. The inner `sh -c` promotes
    // $MEP_WEBVIEW_LD_LIBRARY_PATH (a Nix devShell export, see flake.nix)
    // to LD_LIBRARY_PATH for just this child process -- needed for
    // browser.ts's own libwebview.so dlopen (see its own header comment on
    // why that can't happen from inside browser.ts itself) -- mirroring
    // justfile's run-wasm recipe rather than exporting it globally, which
    // would risk shadowing libraries for mep's own native build tools.
    // Requires deno + WebKitGTK on the machine (same as `just run-wasm`);
    // on_exit surfaces a clear error instead of a silent no-op if either's
    // missing. This is the *explicit opt-out* path now -- mep.browse_command
    // (:Browse's own handler, below) defaults to the in-house in-pane
    // renderer (mep.html_open, lua_env.cpp) instead, since the whole point
    // of building one was to not need a real WebKitGTK/deno dependency for
    // the common case.\n"
    "function mep.browse(target)\n"
    "  local script = os.getenv('MEP_BROWSER_LAUNCHER') or 'launcher/browser.ts'\n"
    "  local ld_path = os.getenv('MEP_WEBVIEW_LD_LIBRARY_PATH') or ''\n"
    "  mep.notify('Opening ' .. target .. ' in external browser...')\n"
    "  mep.job_start({\n"
    "    'sh', '-c',\n"
    "    'ld=\"$1\"; script=\"$2\"; target=\"$3\"; ' ..\n"
    "      'if [ -n \"$ld\" ]; then export LD_LIBRARY_PATH=\"$ld${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}\"; fi; ' ..\n"
    "      'exec deno run --allow-net --allow-read --allow-write --allow-env --allow-ffi --unstable-ffi \"$script\" \"$target\"',\n"
    "    'mep-browse', ld_path, script, target,\n"
    "  }, {\n"
    "    on_exit = function(code)\n"
    "      if code ~= 0 then\n"
    "        mep.notify('mep.browse: failed to launch (needs deno + WebKitGTK -- see launcher/browser.ts)', 'error')\n"
    "      end\n"
    "    end,\n"
    "  })\n"
    "end\n"
    // In-pane default: a real URL is fetched (curl, no TLS/redirect
    // handling of mep's own -- curl does all of that) to a *fresh* temp
    // file every call (os.tmpname(), never reused) specifically so
    // Editor::OpenHtmlInPlace's dedup-by-source can't mistake a re-fetch
    // for the same stale page (see its own .cpp comment); a local path
    // just opens directly, no fetch needed.
    "function mep.browse_open_in_pane(target)\n"
    "  if target:match('^https?://') then\n"
    "    local tmpfile = os.tmpname()\n"
    "    mep.notify('Fetching ' .. target .. '...')\n"
    "    mep.job_start({'curl', '-sL', '-o', tmpfile, target}, {\n"
    "      on_exit = function(code)\n"
    "        if code == 0 then mep.html_open(tmpfile, target)\n"
    "        else mep.notify('mep.browse: failed to fetch ' .. target, 'error') end\n"
    "      end,\n"
    "    })\n"
    "  else\n"
    "    mep.html_open(target)\n"
    "  end\n"
    "end\n"
    // Fetches `target` (curl, if it looks like a remote URL) or just
    // passes it straight through (a local path, nothing to fetch), then
    // calls `land_fn(local_path, target)` -- shared by mep.browse_reload
    // (same origin, fresh fetch/re-read) and mep.browse_open_bar (a new
    // origin the user just typed), both of which pass mep.html_reload as
    // `land_fn` so the result lands in the *current* pane's existing
    // session (Editor::ReloadHtmlBuffer) rather than opening a new
    // buffer the way mep.browse_open_in_pane's own mep.html_open call
    // does -- matching a real browser's address bar, which navigates the
    // current tab in place rather than opening a new one.
    "local function mep_browse_fetch_then(target, land_fn)\n"
    "  if target:match('^https?://') then\n"
    "    local tmpfile = os.tmpname()\n"
    "    mep.notify('Fetching ' .. target .. '...')\n"
    "    mep.job_start({'curl', '-sL', '-o', tmpfile, target}, {\n"
    "      on_exit = function(code)\n"
    "        if code == 0 then land_fn(tmpfile, target)\n"
    "        else mep.notify('mep.browse: failed to fetch ' .. target, 'error') end\n"
    "      end,\n"
    "    })\n"
    "  else\n"
    "    land_fn(target, target)\n"
    "  end\n"
    "end\n"
    // 'r' while parked on the in-pane browser (Editor::HandleHtmlInput)
    // dispatches here via MepBrowseReload: re-fetches HtmlSession::origin
    // if it's a remote URL, or just re-reads the local file otherwise --
    // either way picks up whatever changed since the page was first
    // opened. A no-op if the current pane isn't an HTML pane
    // (mep.html_current_origin returns nil).
    "function mep.browse_reload()\n"
    "  local origin = mep.html_current_origin()\n"
    "  if not origin then return end\n"
    "  mep_browse_fetch_then(origin, mep.html_reload)\n"
    "end\n"
    "mep.command('MepBrowseReload', mep.browse_reload)\n"
    // 'o' while parked on the in-pane browser dispatches here: a small
    // address-bar-style prompt (mep.ui_input, prefilled with the current
    // page's own origin so a quick edit -- adding a path segment, say --
    // is a couple keystrokes) that navigates the *current* pane to
    // whatever's entered. A no-op if the current pane isn't an HTML pane.
    "function mep.browse_open_bar()\n"
    "  local current = mep.html_current_origin()\n"
    "  if not current then return end\n"
    "  mep.ui_input('Open:', current, function(input)\n"
    "    if input and input ~= '' then mep_browse_fetch_then(input, mep.html_reload) end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepBrowseOpen', mep.browse_open_bar)\n"
    // Shared target-resolution for :Browse/:BrowseExternal alike: an
    // explicit argument wins; otherwise the URL under the cursor;
    // otherwise the current buffer's own file if it looks like an HTML
    // file (previewing whatever you're editing); otherwise prompt for one
    // -- same fallback chain shape as mep.open_url_under_cursor, just
    // extended with the current-file case since that's the common
    // "preview this page I'm writing" path. `open_fn` is which of
    // mep.browse_open_in_pane/mep.browse actually opens the resolved
    // target.
    "local function mep_browse_resolve(args, open_fn)\n"
    "  local target = args and args ~= '' and args or nil\n"
    "  target = target or mep.url_under_cursor()\n"
    "  if not target then\n"
    "    local fname = mep.filename()\n"
    "    if fname ~= '' and fname:match('%.html?$') then target = fname end\n"
    "  end\n"
    "  if not target then\n"
    "    mep.ui_input('Browse:', 'https://', function(input)\n"
    "      if input and input ~= '' then open_fn(input) end\n"
    "    end)\n"
    "    return\n"
    "  end\n"
    "  open_fn(target)\n"
    "end\n"
    "function mep.browse_command(args) mep_browse_resolve(args, mep.browse_open_in_pane) end\n"
    "function mep.browse_external_command(args) mep_browse_resolve(args, mep.browse) end\n"
    "mep.command('MepBrowse', mep.browse_command)\n"
    "mep.command('Browse', mep.browse_command)\n"
    "mep.command('MepBrowseExternal', mep.browse_external_command)\n"
    "mep.command('BrowseExternal', mep.browse_external_command)\n"
    "mep.leader_map('bo', 'Browse', mep.browse_command)\n"
    "mep.leader_map('bO', 'Browse (external window)', mep.browse_external_command)\n";

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
    "          on_click = function() mep.open(full) end,\n"
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
    // Html view-toggle escape hatch (Editor::HandleSidebarInput's own
    // Ctrl-E/Ctrl-V comment) -- Enter (on_click, mep.open above) already
    // opens the default view (rendered for .html/.htm), so these only
    // matter for forcing the *other* view. `:e`, not mep.open, is what
    // gives Ctrl-E force-text semantics (LoadFile's own comment).\n"
    "  elseif k == 'C-e' and target then\n"
    "    mep.cmd('e ' .. target)\n"
    "  elseif k == 'C-v' and target then\n"
    "    mep.open(target)\n"
    "  elseif k == '?' then\n"
    "    mep.notify('Files: Enter=open/toggle  a=create  r=rename  d=delete  R=refresh  H=hidden  o=open-with-OS  Ctrl-E/Ctrl-V=text/browser view')\n"
    "  end\n"
    "end\n"
    "mep.command('MepFileTree', function() mep.tree_toggle() end)\n"
    "mep.leader_map('ff', 'Toggle file tree', function() mep.tree_toggle() end)\n"
    "local MEP_README_NAMES = {'README.md', 'README.org', 'README.txt', 'README'}\n"
    // Shared by mep.project_open below and mep.projects()'s picker preview
    // pane further down: first name in MEP_README_NAMES present (as a
    // file, not a dir) in `dir` wins, so README.org is only preferred over
    // README.md etc. by its position in that list. nil if none match.
    "local function mep_project_readme_path(dir)\n"
    "  local entries = mep.list_dir(dir)\n"
    "  for _, name in ipairs(MEP_README_NAMES) do\n"
    "    for _, e in ipairs(entries) do\n"
    "      if not e.is_dir and e.name == name then return dir .. '/' .. name end\n"
    "    end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "function mep.project_open(dir)\n"
    "  mep.chdir(dir)\n"
    "  local readme = mep_project_readme_path(dir)\n"
    "  local opened_readme = readme ~= nil\n"
    "  if opened_readme then mep.open(readme) end\n"
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
    "  local project_count = #items\n"
    "  local function add_current()\n"
    "    mep.project_add('.')\n"
    "    mep.notify('Added current directory as a project')\n"
    "  end\n"
    // Preview pane (project picker's own version of find_files/live_grep's
    // mep_picker_preview_file usage above): shows the highlighted
    // project's README, trying MEP_README_NAMES in order so README.org
    // (this editor's own convention) is picked over README.md etc. when a
    // project happens to have more than one. The '+'/'-' action rows
    // aren't project directories, so they just clear the pane instead.
    "  local function preview_project(item)\n"
    "    if item == '+ Add current directory' or item == '- Remove a project...' then\n"
    "      mep.picker_set_preview('')\n"
    "      return\n"
    "    end\n"
    "    local readme = mep_project_readme_path(item)\n"
    "    if readme then\n"
    "      mep_picker_preview_file(readme, 60)\n"
    "    else\n"
    "      mep.picker_set_preview('(no README found in ' .. item .. ')')\n"
    "    end\n"
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
    "  end, preview_project)\n"
    // on_select_change (just wired above) only fires once the highlighted
    // row actually *changes*, so the picker would otherwise open on the
    // first project with an empty preview pane until the user pressed an
    // arrow key -- preview it immediately here to match the "preview by
    // default" behavior on_select_change gives every row after the first.
    "  if project_count > 0 then preview_project(items[1].data) end\n"
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
    // `HEAD:<path>` is a pathspec, resolved by git relative to *its own*
    // cwd (same as any other git path argument) -- run with cwd set to
    // the buffer's own directory and just its basename, rather than
    // mep's own process cwd (which need not have any relationship to
    // where the file lives, or even be inside a git repo at all) and
    // fname's own full path (which, being absolute whenever mep was
    // opened with one, git would refuse outright: a pathspec can't be
    // absolute). Without this, `git show` silently fails, the "old"
    // side of the diff comes back empty, and every line in the buffer
    // shows as a brand new addition -- confirmed exactly this way.
    "  local dir = fname:match('^(.*)/[^/]+$') or '.'\n"
    "  local base = fname:match('([^/]+)$') or fname\n"
    "  local lines = {}\n"
    "  mep.job_start({'git', 'show', mep.git_gutter_base .. ':' .. base}, {\n"
    "    cwd = dir,\n"
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
    // Opt-in (off by default, same convention as mep.git_gutter_auto/
    // mep.colorize_auto): mep.float_preview dismisses on *any* keypress,
    // so auto-popping the hunk preview on every jump would make repeated
    // ]c/]c-style navigation need two presses per hop (one to dismiss the
    // still-open preview, one to actually jump) -- opt-in keeps that the
    // user's choice instead of a surprise default (NVIM_PARITY_PLAN.md
    // Phase 17 gap: hunk-jump never previewed the hunk it landed on).
    "mep.git_hunk_preview_on_jump = false\n"
    "local function mep_git_maybe_preview_hunk()\n"
    "  if mep.git_hunk_preview_on_jump then mep.git_preview_hunk() end\n"
    "end\n"
    "function mep.git_next_hunk()\n"
    "  local row = mep.cursor()\n"
    "  for _, h in ipairs(mep_git_hunks) do\n"
    "    if h.new_start > row then mep.set_cursor(h.new_start, 1) mep_git_maybe_preview_hunk() return end\n"
    "  end\n"
    "  if mep_git_hunks[1] then mep.set_cursor(mep_git_hunks[1].new_start, 1) mep_git_maybe_preview_hunk() end\n"
    "end\n"
    "function mep.git_prev_hunk()\n"
    "  local row = mep.cursor()\n"
    "  for i = #mep_git_hunks, 1, -1 do\n"
    "    local h = mep_git_hunks[i]\n"
    "    if h.new_start < row then mep.set_cursor(h.new_start, 1) mep_git_maybe_preview_hunk() return end\n"
    "  end\n"
    "  local last = mep_git_hunks[#mep_git_hunks]\n"
    "  if last then mep.set_cursor(last.new_start, 1) mep_git_maybe_preview_hunk() end\n"
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
    "        on_click = function() mep.open(path) end,\n"
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
    "  TODO  = {glyph = '', hl = 'Yellow'},\n"
    "  FIXME = {glyph = '', hl = 'Red'},\n"
    "  HACK  = {glyph = '', hl = 'Orange'},\n"
    "  NOTE  = {glyph = '', hl = 'Blue'},\n"
    "}\n"
    "local MEP_TODO_DEFAULT = {glyph = '', hl = 'Warn'}\n"
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
    "  local function on_stdout(line) items[#items + 1] = line end\n"
    "  local function open_results()\n"
    "    if #items == 0 then\n"
    "      mep.notify('No TODO/FIXME/HACK/NOTE matches found')\n"
    "      return\n"
    "    end\n"
    "    mep.picker_open('Todos', items, function(item)\n"
    "      if not item then return end\n"
    "      local file, lnum = item:match('^([^:]+):(%d+):')\n"
    "      if file then mep.open(file); mep.set_cursor(tonumber(lnum), 1) end\n"
    "    end)\n"
    "  end\n"
    "  mep.job_start({'rg', '-n', '--no-heading', pattern, '.'}, {\n"
    "    on_stdout = on_stdout,\n"
    "    on_exit = function(code)\n"
    // 127 is job.cpp's own exec-failed convention (see job.cpp) -- means
    // ripgrep isn't installed, not just "no matches" (a real 0-match run
    // also exits with a nonzero, non-127 code). NVIM_PARITY_PLAN.md Phase
    // 18 gap: previously just reported "no matches (or rg not installed)"
    // and gave up -- GNU grep's -n output is the same 'file:line:text'
    // shape rg's own --no-heading produces, so open_results()/the picker's
    // item parsing doesn't need to know which one ran.
    "      if code == 127 and #items == 0 then\n"
    "        mep.job_start({'grep', '-rn', '-E', '--exclude-dir=.git', pattern, '.'},\n"
    "          {on_stdout = on_stdout, on_exit = open_results})\n"
    "        return\n"
    "      end\n"
    "      open_results()\n"
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
    // NVIM_PARITY_PLAN.md Phase 20 gap: didClose was never sent, so a
    // server kept every ever-opened file "live" for the rest of the
    // session. Takes an explicit fname (not mep.filename()) since by the
    // time this fires the buffer showing it is already gone.
    "function mep.lsp_did_close(fname)\n"
    "  local id = mep.lsp_client_for()\n"
    "  if id then mep.lsp_notify(id, 'textDocument/didClose', {textDocument = {uri = mep_lsp_uri(fname)}}) end\n"
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
    "    mep.open(f)\n"
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
    "      if f then mep.open(f); mep.set_cursor(tonumber(lnum), 1) end\n"
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
    "    mep.open(f)\n"
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
    // `mep.open(f)` primitive those already use, edited, saved, and
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
    "        mep.open(f)\n"
    "        switched = true\n"
    "        mep_lsp_apply_edits_current_buffer(edits)\n"
    "        mep.cmd('w')\n"
    "      end\n"
    "      total = total + #edits\n"
    "    end\n"
    "  end\n"
    "  if switched then\n"
    "    mep.open(orig_fname)\n"
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
    // Real Vim's own bindings for these two specifically (not leader-key
    // ones, unlike the newer methods above) -- both free today: K has no
    // built-in meaning anywhere in Normal mode, and mep.map_g (this
    // library's own new primitive, added alongside this) reaches "d"
    // after a pending "g" the way plain mep.map never could (see its own
    // doc comment on why gd was unreachable before it existed).
    "mep.map('n', 'K', mep.lsp_hover, {desc = 'LSP: hover'})\n"
    "mep.map_g('d', mep.lsp_goto_definition)\n"
    // --- Diagnostics UI (Phase 21) -- built on the publishDiagnostics
    // store kBuiltinLsp already populates (mep_lsp_diagnostics, upvalue-
    // shared since this all lives in the same DoString chunk).
    "local mep_diag_ns = nil\n"
    "local MEP_DIAG_SEVERITY = {[1] = 'Error', [2] = 'Warn', [3] = 'Info', [4] = 'Hint'}\n"
    "local MEP_DIAG_WRAP_WIDTH = 70\n"
    "local function mep_diag_wrap(text, width)\n"
    "  local out, line = {}, ''\n"
    "  for word in text:gmatch('%S+') do\n"
    "    if line == '' then\n"
    "      line = word\n"
    "    elseif #line + 1 + #word <= width then\n"
    "      line = line .. ' ' .. word\n"
    "    else\n"
    "      out[#out + 1] = line\n"
    "      line = word\n"
    "    end\n"
    "  end\n"
    "  if line ~= '' then out[#out + 1] = line end\n"
    "  if #out == 0 then out[1] = '' end\n"
    "  return out\n"
    "end\n"
    "local function mep_diag_at_row(row)\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  local row_diags = {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    if d.range.start.line + 1 == row then row_diags[#row_diags + 1] = d end\n"
    "  end\n"
    "  table.sort(row_diags, function(a, b) return (a.severity or 1) < (b.severity or 1) end)\n"
    "  return row_diags\n"
    "end\n"
    // More than 2 diagnostics on one line can't be conveyed by a single
    // gutter badge + one line of virt_text -- pop up all of them
    // instead, word-wrapped (mep.float_preview itself does not wrap --
    // see mep_diag_wrap's own comment) so a long message is actually
    // readable rather than running off the edge of the box.
    "local function mep_diag_popup(row_diags)\n"
    "  local wrapped = {}\n"
    "  for i, d in ipairs(row_diags) do\n"
    "    if i > 1 then wrapped[#wrapped + 1] = '' end\n"
    "    local sev = MEP_DIAG_SEVERITY[d.severity or 1] or 'Error'\n"
    "    for _, l in ipairs(mep_diag_wrap(i .. '. [' .. sev .. '] ' .. d.message, MEP_DIAG_WRAP_WIDTH)) do\n"
    "      wrapped[#wrapped + 1] = l\n"
    "    end\n"
    "  end\n"
    "  mep.float_preview('Diagnostics on this line (' .. #row_diags .. ')', table.concat(wrapped, '\\n'))\n"
    "end\n"
    // One underline decoration per diagnostic (its own exact span, as
    // before), but only *one* sign+virt_text decoration per row instead
    // of one per diagnostic -- previously every diagnostic on a line
    // added its own virt_text, all independently col_start-anchored to
    // their own (often differing) columns, several of which could end
    // up drawn overlapping each other and the buffer's real text with no
    // background cover at all. Now: a single badge (sign_badge, filled
    // circle colored by the *worst* severity present, digit = how many)
    // plus one virt_text line (that worst diagnostic's own message,
    // "(N) " prefixed when there's more than one) anchored past the end
    // of the line's real text (virt_text_eol) instead of at any
    // diagnostic's own column.
    "function mep.lsp_render_diagnostics()\n"
    "  if not mep_diag_ns then mep_diag_ns = mep.ns_create('diagnostics') end\n"
    "  mep.ns_clear(mep_diag_ns)\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  local by_row, row_order = {}, {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    local sev = d.severity or 1\n"
    "    local hl = MEP_DIAG_SEVERITY[sev] or 'Error'\n"
    "    local row = d.range.start.line + 1\n"
    "    mep.deco_add(mep_diag_ns, {\n"
    "      row = row, col_start = d.range.start.character + 1, col_end = d.range['end'].character + 1,\n"
    "      hl_group = hl, underline = true,\n"
    "    })\n"
    "    if not by_row[row] then by_row[row] = {}; row_order[#row_order + 1] = row end\n"
    "    table.insert(by_row[row], d)\n"
    "  end\n"
    "  for _, row in ipairs(row_order) do\n"
    "    local row_diags = by_row[row]\n"
    "    table.sort(row_diags, function(a, b) return (a.severity or 1) < (b.severity or 1) end)\n"
    "    local worst = row_diags[1]\n"
    "    local hl = MEP_DIAG_SEVERITY[worst.severity or 1] or 'Error'\n"
    "    mep.deco_add(mep_diag_ns, {\n"
    "      row = row, sign = tostring(#row_diags), sign_hl = hl, sign_badge = true,\n"
    "      virt_text = '  ' .. (#row_diags > 1 and ('(' .. #row_diags .. ') ') or '') .. worst.message:gsub('\\n.*', ''),\n"
    "      virt_text_hl = hl, virt_text_eol = true, priority = 10,\n"
    "    })\n"
    "  end\n"
    "end\n"
    // :MepDiagShow: pop up the full (wrapped) list once there are more
    // than 2 diagnostics on the cursor's own line (matching the same
    // threshold mep_diag_nav's own jump-then-maybe-popup uses below),
    // otherwise the original one-line notify is still plenty.
    "function mep.lsp_diagnostic_at_cursor()\n"
    "  local row_diags = mep_diag_at_row(mep.cursor())\n"
    "  if #row_diags == 0 then mep.notify('No diagnostic on this line') return end\n"
    "  if #row_diags > 2 then\n"
    "    mep_diag_popup(row_diags)\n"
    "    return\n"
    "  end\n"
    "  local d = row_diags[1]\n"
    "  mep.notify((MEP_DIAG_SEVERITY[d.severity or 1] or 'Error') .. ': ' .. d.message,\n"
    "             (d.severity == 1 and 'error') or (d.severity == 2 and 'warn') or 'info')\n"
    "end\n"
    // Unlike mep.lsp_diagnostic_at_cursor above (popup only past the
    // 2-diagnostic threshold, else a one-line notify), this always pops
    // up the full list -- the point of a dedicated "show me everything
    // on this line" key.
    "function mep.lsp_line_diagnostics_popup()\n"
    "  local row_diags = mep_diag_at_row(mep.cursor())\n"
    "  if #row_diags == 0 then mep.notify('No diagnostics on this line') return end\n"
    "  mep_diag_popup(row_diags)\n"
    "end\n"
    // Jump to the next/previous row with a (matching) diagnostic -- rows
    // deduped (a real, if minor, pre-existing gap: multiple diagnostics
    // sharing a row used to make that row count once per diagnostic, so
    // "next" could re-land on the same row more than once in a row
    // before actually advancing) -- then, once landed, pop up the full
    // list if that line turns out to have more than 2 (errors_only
    // narrows which diagnostics count toward that threshold too, so "[e"
    // popping up means more than 2 *errors*, not diagnostics of any
    // severity, matching what "next/previous error" itself already means).
    "local function mep_diag_nav(delta, errors_only)\n"
    "  local diags = mep_lsp_diagnostics[mep_lsp_abspath(mep.filename())] or {}\n"
    "  if #diags == 0 then mep.notify('No diagnostics') return end\n"
    "  local rows, seen = {}, {}\n"
    "  for _, d in ipairs(diags) do\n"
    "    if not errors_only or (d.severity or 1) == 1 then\n"
    "      local row = d.range.start.line + 1\n"
    "      if not seen[row] then\n"
    "        seen[row] = true\n"
    "        rows[#rows + 1] = row\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  table.sort(rows)\n"
    "  if #rows == 0 then mep.notify('No matching diagnostics') return end\n"
    "  local cur = mep.cursor()\n"
    "  local target\n"
    "  if delta > 0 then\n"
    "    for _, r in ipairs(rows) do\n"
    "      if r > cur then\n"
    "        target = r\n"
    "        break\n"
    "      end\n"
    "    end\n"
    "    target = target or rows[1]\n"
    "  else\n"
    "    for i = #rows, 1, -1 do\n"
    "      if rows[i] < cur then\n"
    "        target = rows[i]\n"
    "        break\n"
    "      end\n"
    "    end\n"
    "    target = target or rows[#rows]\n"
    "  end\n"
    "  mep.set_cursor(target, 1)\n"
    "  local row_diags = mep_diag_at_row(target)\n"
    "  if errors_only then\n"
    "    local errors_here = {}\n"
    "    for _, d in ipairs(row_diags) do\n"
    "      if (d.severity or 1) == 1 then errors_here[#errors_here + 1] = d end\n"
    "    end\n"
    "    row_diags = errors_here\n"
    "  end\n"
    "  if #row_diags > 2 then mep_diag_popup(row_diags) end\n"
    "end\n"
    "function mep.lsp_next_diagnostic() mep_diag_nav(1, false) end\n"
    "function mep.lsp_prev_diagnostic() mep_diag_nav(-1, false) end\n"
    "function mep.lsp_next_error() mep_diag_nav(1, true) end\n"
    "function mep.lsp_prev_error() mep_diag_nav(-1, true) end\n"
    "mep.command('MepDiagShow', mep.lsp_diagnostic_at_cursor)\n"
    // 'L' is already vim's screen-bottom motion (H/M/L), so it can't be
    // reused here without silently breaking that motion -- 'Q' is free
    // (mep has no Ex-mode) and pairs naturally with 'K' for hover above.
    "mep.map('n', 'Q', mep.lsp_line_diagnostics_popup, {desc = 'LSP: diagnostics popup for current line'})\n"
    // Real Vim's own bracket-motion convention ("[x"/"]x" for
    // previous/next of some category), applied to mep's own error
    // navigation via mep.map_bracket_prev/next (this library's new
    // primitive -- see its own doc comment on why a bare mep.map can't
    // reach a key typed after a pending "["/"]").
    "mep.map_bracket_prev('e', mep.lsp_prev_error)\n"
    "mep.map_bracket_next('e', mep.lsp_next_error)\n"
    // Keeps the server's own copy of the document in sync with unsaved
    // edits -- mep.lsp_did_change/did_save were previously defined but
    // never actually wired to anything, so diagnostics/hover would only
    // ever reflect a file's on-disk state as of its last didOpen. Both
    // are already self-gating (mep.lsp_client_for() -> nil is a no-op),
    // so no separate on/off flag is needed here the way mep.lsp_auto_
    // attach below has one.
    "mep.on_buffer_changed(mep.lsp_did_change)\n"
    "mep.on_buffer_saved(mep.lsp_did_save)\n"
    // Auto-attach (mep.lsp_auto_attach, on by default) + re-render
    // already-cached diagnostics whenever the active *file* changes --
    // mirrors mep.syntax_auto's own mep_syntax_last_file watcher just
    // above in kBuiltinSyntax (mep.on_buffer_changed only fires on an
    // actual edit, not a plain buffer switch with none, e.g. :e/the file
    // tree/:bn/:bp/Ctrl-W onto a different buffer -- so a second,
    // non-debounced mep.on_frame watcher keyed on mep.filename() itself
    // is what catches "opened/switched to a new file" the same way that
    // one does for syntax highlighting).
    //
    // mep_lsp_seen_files guards against re-sending didOpen for a file
    // already attached: mep.lsp_attach()/lsp_did_open() unconditionally
    // resets that file's own version counter to 1 and sends a fresh
    // didOpen every time they're called (fine for the original one-shot
    // :MepLspAttach this was designed for) -- called again on every
    // revisit to an already-open file, as this watcher would without the
    // guard, that's a second didOpen for the same still-open URI, which
    // the LSP spec doesn't define and some servers reject outright.
    "mep.lsp_auto_attach = true\n"
    "local mep_lsp_last_file = nil\n"
    "local mep_lsp_seen_files = {}\n"
    "mep.on_frame(function()\n"
    "  local fname = mep.filename()\n"
    "  if fname == mep_lsp_last_file then return end\n"
    "  mep_lsp_last_file = fname\n"
    "  mep.lsp_render_diagnostics()\n"
    "  if not mep.lsp_auto_attach or fname == '' or mep_lsp_seen_files[fname] then return end\n"
    "  if not mep_lsp_filetype(fname) then return end\n"
    "  mep_lsp_seen_files[fname] = true\n"
    "  mep.lsp_attach()\n"
    "end)\n"
    // Mirror-image sweep of the watcher above: once a file drops out of
    // every open buffer (its last pane closed, `:bd`, quitting a split),
    // send the didClose that was never sent before (Phase 20 gap) and
    // drop it from mep_lsp_seen_files so a later re-open sends a fresh
    // didOpen rather than being treated as still-attached. Gated on
    // mep.buffer_count() (a cheap int, unlike mep.buffer_list()'s full
    // per-frame table) so the actual sweep only runs on an open/close
    // edge, not every frame.
    "local mep_lsp_last_buffer_count = -1\n"
    "mep.on_frame(function()\n"
    "  local n = mep.buffer_count()\n"
    "  if n == mep_lsp_last_buffer_count then return end\n"
    "  mep_lsp_last_buffer_count = n\n"
    "  local open = {}\n"
    "  for _, b in ipairs(mep.buffer_list()) do\n"
    "    local fname = mep.buffer_filename(tonumber(b.data))\n"
    "    if fname ~= '' then open[fname] = true end\n"
    "  end\n"
    "  for fname in pairs(mep_lsp_seen_files) do\n"
    "    if not open[fname] then\n"
    "      mep.lsp_did_close(fname)\n"
    "      mep_lsp_seen_files[fname] = nil\n"
    "    end\n"
    "  end\n"
    "end)\n";

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
    // Factored out of mep.snippet_expand so the LSP-completion accept hook
    // below can splice a raw insertText body the same way, without a
    // registry-lookup-by-name step it has no use for (the body's already
    // known -- it's the completion item's own text).
    "local function mep_snippet_splice(row, before, after, body)\n"
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
    "  mep_snippet_splice(row, before, after, body)\n"
    "end\n"
    // NVIM_PARITY_PLAN.md Phase 23 gap: an LSP completion item with
    // insertTextFormat=Snippet (2, per the LSP spec's InsertTextFormat
    // enum -- 1 is PlainText) got its raw `$1`/`$0' placeholder syntax
    // spliced in verbatim by AcceptCompletion, with no re-expansion
    // through the tabstop engine -- the C++ hook this consumes
    // (Editor::completion_accept_hook_ref_) and the iformat cache it
    // reads (mep_lsp_completion_iformat, kBuiltinCompletion) both already
    // existed; nothing had ever called mep.set_completion_accept_hook to
    // wire them together. Deletes exactly the span AcceptCompletion just
    // spliced in (computed from `text`'s own line count/last-line length,
    // not a trigger-word guess -- `text` can be multi-line, unlike a
    // registry snippet's fixed trigger word) and re-splices it as a real
    // snippet body via the same mep_snippet_splice this file's own
    // mep.snippet_expand uses.\n"
    "mep.set_completion_accept_hook(function(text)\n"
    "  if mep_lsp_completion_iformat[text] ~= 2 then return end\n"
    "  local row, col = mep.cursor()\n"
    "  local body = {}\n"
    "  for l in (text .. '\\n'):gmatch('(.-)\\n') do body[#body + 1] = l end\n"
    "  local start_row = row - (#body - 1)\n"
    "  local start_col = col - #body[#body]\n"
    "  local before = mep.get_line(start_row):sub(1, start_col - 1)\n"
    "  local after = mep.get_line(row):sub(col)\n"
    "  mep_snippet_splice(start_row, before, after, body)\n"
    "end)\n"
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
    "      for _, r2 in ipairs(bps) do mep.deco_add(mep_dap_ns, {row = r2, sign = '', sign_hl = 'Red'}) end\n"
    "      return\n"
    "    end\n"
    "  end\n"
    "  bps[#bps + 1] = row\n"
    "  mep.deco_add(mep_dap_ns, {row = row, sign = '', sign_hl = 'Red'})\n"
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
    "mep.syntax_keywords.pyi = mep.syntax_keywords.python\n"
    "mep.syntax_keywords.js = mep.syntax_keywords.javascript\n"
    "mep.syntax_keywords.mjs = mep.syntax_keywords.javascript\n"
    "mep.syntax_keywords.cjs = mep.syntax_keywords.javascript\n"
    "mep.syntax_comment_prefix = {lua = '--', python = '#', javascript = '//', c = '//', cpp = '//', h = '//', hpp = '//',\n"
    "  py = '#', pyi = '#', js = '//', mjs = '//', cjs = '//'}\n"
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
    // Org src-block language injection: `mep.org_babel_langs` (see
    // kBuiltinOrgBabel) keys its table by the literal babel language tag a
    // `#+begin_src <lang>` header writes ("python", "c++", "csharp", ...),
    // which is almost never the same string `mep.ts_captures`/
    // mep_lsp_filetype expect (a bare file extension: "py", "cpp", "cs",
    // ...) -- this table bridges the two, covering every language
    // mep.org_babel_langs itself supports (plus a couple of extra aliases
    // real org files commonly write, e.g. "shell"). A tag with no entry
    // here (or an entry with no vendored/dynamically-loadable grammar)
    // just renders unhighlighted inside its block, same as any other
    // filetype with no grammar -- not an error.
    "mep_org_babel_lang_ts_ft = {\n"
    "  lua = 'lua', python = 'py', sh = 'sh', bash = 'bash', shell = 'sh', zsh = 'zsh',\n"
    "  javascript = 'js', js = 'js', typescript = 'ts', ts = 'ts',\n"
    "  cpp = 'cpp', ['c++'] = 'cpp', c = 'c', csharp = 'cs', cs = 'cs', ['c#'] = 'cs',\n"
    "  ruby = 'rb', elixir = 'ex', julia = 'jl', clojure = 'clj', perl = 'pl',\n"
    "  r = 'r', R = 'R', php = 'php', rust = 'rs', go = 'go', fortran = 'f90',\n"
    "  scala = 'scala', zig = 'zig', nim = 'nim', crystal = 'cr', java = 'java',\n"
    "  kotlin = 'kt', haskell = 'hs', ocaml = 'ml', d = 'd',\n"
    "}\n"
    // Org emphasis markup (*bold*/\/italic\//_underline_/+strike+/=verbatim=/
    // ~code~), a hand-rolled scanner since the vendored org grammar has no
    // emphasis nodes to query (confirmed against third_party/grammars/org's
    // own node types -- headlines/tags/properties/timestamps/blocks/etc.
    // are there, emphasis marks aren't). Markers are left visible (matching
    // real Emacs org-mode's own default -- org-hide-emphasis-markers is off
    // by default there too) and the *whole* matched span, markers included,
    // gets the styling -- simpler than concealing them, and composes
    // cleanly with the bold/italic/underline/strikethrough Decoration
    // fields (main.cpp's DrawPane), which style whatever text is actually
    // in [col_start, col_end) rather than a separate virt_text substitute.
    //
    // Matching follows real org's own emphasis rules (org-emphasis-regexp-
    // components): a marker only opens if preceded by line-start or a
    // non-word character (rules out mid-identifier characters -- the `_`
    // in `snake_case`, the `*` in `a*b` multiplication) and immediately
    // followed by a non-space, non-marker character (rules out `* not
    // this*` and a headline's own leading `* `/`** `/etc., which always
    // has a space right after its last asterisk); a marker only closes if
    // immediately preceded by a non-space character and immediately
    // followed by line-end or a non-word character (rules out
    // `x_1`-style LaTeX subscripts and `$5-$10`-style adjacency, though
    // that specific case is kBuiltinOrgLatex's own problem, not this
    // scanner's -- included here only because the same rule shape happens
    // to help). Unlike kBuiltinOrgLatex's inline-math scanner (a different
    // file, same shape of problem), a real quote character isn't
    // ambiguous the way bare `$` is, so there's no need for the extra
    // not-followed-by-digit refinement that scanner has.
    "local MEP_ORG_EMPHASIS_MARKERS = {\n"
    "  ['*'] = 'bold', ['/'] = 'italic', ['_'] = 'underline',\n"
    "  ['+'] = 'strikethrough', ['='] = 'verbatim', ['~'] = 'code',\n"
    "}\n"
    "local function mep_org_emphasis_is_word(ch) return ch ~= '' and ch:match('%w') ~= nil end\n"
    "local function mep_org_scan_emphasis(line)\n"
    "  local spans = {}\n"
    "  local i, n = 1, #line\n"
    "  while i <= n do\n"
    "    local ch = line:sub(i, i)\n"
    "    local kind = MEP_ORG_EMPHASIS_MARKERS[ch]\n"
    "    if kind then\n"
    "      local pre = line:sub(i - 1, i - 1)\n"
    "      local nxt = line:sub(i + 1, i + 1)\n"
    "      if (i == 1 or not mep_org_emphasis_is_word(pre)) and nxt ~= '' and nxt ~= ' ' and nxt ~= ch then\n"
    "        local search_from = i + 1\n"
    "        local found_end = nil\n"
    "        while true do\n"
    "          local s = line:find(ch, search_from, true)\n"
    "          if not s then break end\n"
    "          local prev_char = line:sub(s - 1, s - 1)\n"
    "          local after_char = line:sub(s + 1, s + 1)\n"
    "          if prev_char ~= ' ' and prev_char ~= ch and not mep_org_emphasis_is_word(after_char) then\n"
    "            found_end = s\n"
    "            break\n"
    "          end\n"
    "          search_from = s + 1\n"
    "        end\n"
    "        if found_end then\n"
    "          spans[#spans + 1] = {col_start = i, col_end = found_end + 1, kind = kind}\n"
    "          i = found_end + 1\n"
    "        else\n"
    "          i = i + 1\n"
    "        end\n"
    "      else\n"
    "        i = i + 1\n"
    "      end\n"
    "    else\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  return spans\n"
    "end\n"
    // Skips scanning inside any #+begin_X ... #+end_X block (kBuiltinOrgLatex
    // block-detection, above, uses the same case-insensitive pattern) --
    // literal block contents (a code block's own `_`/`*` characters, an
    // example block's prose) were never meant to be re-interpreted as
    // emphasis, the same reasoning RecomputeOrgFolds' own comment
    // (editor.cpp) gives for why headline folding needs org's real grammar
    // instead of a naive line scan.
    "local function mep_syntax_highlight_org_emphasis(ns, lines)\n"
    "  local in_block = false\n"
    "  for i = 1, #lines do\n"
    "    local line = lines[i]\n"
    "    if in_block then\n"
    "      if line:match('^%s*#%+[Ee][Nn][Dd]_%a+') then in_block = false end\n"
    "    elseif line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_%a+') then\n"
    "      in_block = true\n"
    "    else\n"
    "      for _, span in ipairs(mep_org_scan_emphasis(line)) do\n"
    "        local d = {row = i, col_start = span.col_start, col_end = span.col_end}\n"
    "        if span.kind == 'bold' then d.bold = true\n"
    "        elseif span.kind == 'italic' then d.italic = true\n"
    "        elseif span.kind == 'underline' then d.underline = true\n"
    "        elseif span.kind == 'strikethrough' then d.strikethrough = true d.hl_group = 'Comment'\n"
    "        elseif span.kind == 'verbatim' then d.hl_group = 'Green'\n"
    "        elseif span.kind == 'code' then d.hl_group = 'Cyan'\n"
    "        end\n"
    "        mep.deco_add(ns, d)\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    // Scans `lines` (a 1-indexed array mirroring the current buffer, same
    // shape mep.syntax_highlight already builds) for
    // `#+begin_src <lang>` / `#+end_src` pairs and runs each one's own
    // body through mep.ts_captures under the *embedded* language's own
    // filetype, translating every capture's row from body-relative back to
    // absolute buffer row before adding it to `ns`. A light, standalone
    // rescan of the case-insensitive begin/end pattern (mirrors
    // mep_org_src_block_at's own in kBuiltinOrgBabel) rather than a shared
    // call -- that function is `local` to its own DoString chunk and
    // Lua chunks don't share locals, and this scan only ever needs a
    // block's language tag and body span, not the full header-arg parse
    // (:var/:tangle/:cache/...) that function also does.
    "local function mep_syntax_highlight_org_src_blocks(ns, lines)\n"
    "  local i = 1\n"
    "  local n = #lines\n"
    "  while i <= n do\n"
    "    local lang = lines[i]:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+(%S+)')\n"
    "    if lang then\n"
    "      local hdr = i\n"
    "      local endr = nil\n"
    "      for j = hdr + 1, n do\n"
    "        if lines[j]:match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') then endr = j break end\n"
    "      end\n"
    "      if endr then\n"
    "        local embed_ft = mep_org_babel_lang_ts_ft[lang:lower()]\n"
    "        if embed_ft then\n"
    "          local body = {}\n"
    "          for k = hdr + 1, endr - 1 do body[#body + 1] = lines[k] end\n"
    "          local captures = mep.ts_captures(embed_ft, table.concat(body, '\\n'))\n"
    "          if captures then\n"
    "            for _, cap in ipairs(captures) do\n"
    "              local hl = mep_ts_resolve_hl(cap.capture)\n"
    "              if hl then\n"
    "                mep.deco_add(ns, {row = hdr + cap.row, col_start = cap.col_start, col_end = cap.col_end, hl_group = hl})\n"
    "              end\n"
    "            end\n"
    "          end\n"
    "        end\n"
    "        i = endr + 1\n"
    "      else\n"
    "        i = i + 1\n"
    "      end\n"
    "    else\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
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
    "    if ft == 'org' then\n"
    "      mep_syntax_highlight_org_src_blocks(mep_syntax_ns, lines)\n"
    "      mep_syntax_highlight_org_emphasis(mep_syntax_ns, lines)\n"
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
    // Also remembers the (source, target) pane pair for mep.term_jump
    // (NVIM_PARITY_PLAN.md Phase 27 gap: no way back to a Run/REPL pane,
    // or back from it to where you started, short of manual hjkl) --
    // last-opened-wins, same single-slot scope cut as mep.org_stored_link.
    "local mep_term_jump_source, mep_term_jump_target = nil, nil\n"
    "local function mep_term_open_pane(title)\n"
    "  mep_term_jump_source = mep.current_buffer()\n"
    "  local buf_id = mep.buffer_new()\n"
    "  mep.cmd('split')\n"
    "  mep.buffer_switch(buf_id)\n"
    "  mep_term_jump_target = buf_id\n"
    "  return buf_id\n"
    "end\n"
    // Toggles between the most recently opened Run/REPL pane and whatever
    // buffer was focused right before it was opened.
    "function mep.term_jump()\n"
    "  if not mep_term_jump_target then mep.notify('No Run/REPL pane opened yet', 'warn') return end\n"
    "  local dest = (mep.current_buffer() == mep_term_jump_target) and mep_term_jump_source or mep_term_jump_target\n"
    "  if not dest or not mep.pane_focus_buffer(dest) then\n"
    "    mep.notify('That pane is no longer open', 'warn')\n"
    "  end\n"
    "end\n"
    "mep.command('MepTermJump', mep.term_jump)\n"
    "mep.leader_map('rj', 'Jump to/from Run/REPL pane', mep.term_jump)\n"
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

// vim-slime-style "send to a terminal buffer of your own choosing"
// (distinct from mep.repl_start above, which spawns and owns one REPL
// job per filetype): mod1+CR sends the current line then advances the
// cursor, or -- in Visual mode -- sends the whole selection. Registration
// is per *source* buffer (mep_termsend_targets, keyed by mep.
// current_buffer()), so several source buffers can each target a
// different terminal at once. The first send from a buffer -- or any
// send once its previously-registered target buffer has stopped being a
// terminal (closed, or repurposed) -- prompts (mep.ui_input) for a
// terminal buffer id, pre-filled with the first terminal buffer found
// among mep.pane_buffers() (panes in the *active tab*), so accepting it
// is just pressing Enter a second time. "Terminal buffer" here means
// either flavor mep has: a real `:terminal` (mep.is_terminal_buffer/
// mep.terminal_write, added alongside this feature) or one of this same
// file's own mep.run_file/mep.repl_start output buffers would *not*
// qualify (they're plain buffers Lua renders into, never registered
// with Editor::terminals_) -- deliberately scoped to genuine `:terminal`
// panes only, the ones a user would actually open to run something
// interactively and want code sent into.
const char *kBuiltinTermSend =
    // source bufnr -> target (terminal) bufnr
    "local mep_termsend_targets = {}\n"
    "local function mep_termsend_alive(bufnr)\n"
    "  return bufnr ~= nil and mep.is_terminal_buffer(bufnr)\n"
    "end\n"
    // Every terminal buffer currently shown by a pane in the active tab,
    // in mep.pane_buffers()'s own order -- what a fresh registration
    // prompt offers as its default (its first entry).
    "local function mep_termsend_candidates()\n"
    "  local out = {}\n"
    "  for _, id in ipairs(mep.pane_buffers()) do\n"
    "    if mep.is_terminal_buffer(id) then out[#out + 1] = id end\n"
    "  end\n"
    "  return out\n"
    "end\n"
    "function mep.termsend_register(source, target)\n"
    "  if not mep_termsend_alive(target) then\n"
    "    mep.notify('mep.termsend: buffer ' .. tostring(target) .. ' is not a terminal buffer', 'error')\n"
    "    return false\n"
    "  end\n"
    "  mep_termsend_targets[source] = target\n"
    "  return true\n"
    "end\n"
    // Resolves `source`'s own send target, prompting for one first if it
    // has none yet (or its previous one has stopped existing). Calls
    // on_ready(target_bufnr) once a target is known; a no-op if the
    // prompt is cancelled or the typed id isn't a terminal buffer.
    "local function mep_termsend_ensure(source, on_ready)\n"
    "  local target = mep_termsend_targets[source]\n"
    "  if mep_termsend_alive(target) then on_ready(target) return end\n"
    "  local candidates = mep_termsend_candidates()\n"
    "  local default = candidates[1] and tostring(candidates[1]) or ''\n"
    "  mep.ui_input('mep.termsend: terminal buffer id to send to', default, function(input)\n"
    "    if not input or input == '' then return end\n"
    "    local id = tonumber(input)\n"
    "    if not id then\n"
    "      mep.notify('mep.termsend: \"' .. input .. '\" is not a buffer id', 'error')\n"
    "      return\n"
    "    end\n"
    "    if mep.termsend_register(source, math.floor(id)) then\n"
    "      on_ready(mep_termsend_targets[source])\n"
    "    end\n"
    "  end)\n"
    "end\n"
    "local function mep_termsend_send(source, text, after)\n"
    "  mep_termsend_ensure(source, function(target)\n"
    "    mep.terminal_write(target, text .. '\\n')\n"
    "    if after then after() end\n"
    "  end)\n"
    "end\n"
    // Sends the line at the cursor, then moves the cursor down one line
    // -- REPL-cell-style "run and advance".
    "function mep.termsend_line()\n"
    "  local source = mep.current_buffer()\n"
    "  local row = mep.cursor()\n"
    "  local line = mep.get_line(row)\n"
    "  mep_termsend_send(source, line, function()\n"
    "    local target_row = math.min(row + 1, mep.line_count())\n"
    "    local _, col = mep.cursor()\n"
    "    mep.set_cursor(target_row, col)\n"
    "  end)\n"
    "end\n"
    // Sends the active Visual selection as one submission, then leaves
    // Visual mode (mep.cmd('normal!') from Visual mode always drops
    // straight to Normal -- Editor::RunNormalKeys bails out of its own
    // keystroke loop on a non-Normal/Insert mode and then unconditionally
    // calls EnterNormal(), regardless of what -- if anything -- args
    // contains). No cursor movement afterwards: with a manually-
    // highlighted range there's no single "next line" to advance to.
    "function mep.termsend_selection()\n"
    "  local source = mep.current_buffer()\n"
    "  local text = mep.visual_selection()\n"
    "  mep.cmd('normal!')\n"
    "  if text == '' then return end\n"
    "  mep_termsend_send(source, text, nil)\n"
    "end\n"
    // One binding for both modes: HandleMod1Shortcuts fires before mode
    // dispatch regardless of mode_, so this closure tells Normal from
    // Visual apart itself via mep.visual_selection() rather than needing
    // two separate mep.map registrations (mep.map only covers plain "n"/
    // "v" single-ASCII keys, not mod1 combos or Enter -- see mep.map_mod1's
    // own doc comment).
    "mep.map_mod1('CR', function()\n"
    "  if mep.visual_selection() ~= '' then\n"
    "    mep.termsend_selection()\n"
    "  else\n"
    "    mep.termsend_line()\n"
    "  end\n"
    "end)\n"
    "mep.command('MepTermSendLine', mep.termsend_line)\n"
    "mep.command('MepTermSendRegister', function(args)\n"
    "  local source = mep.current_buffer()\n"
    "  if args and args ~= '' then\n"
    "    mep.termsend_register(source, tonumber(args))\n"
    "  else\n"
    "    mep_termsend_targets[source] = nil\n"
    "    mep_termsend_ensure(source, function() end)\n"
    "  end\n"
    "end)\n";

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
    // Level 1 hottest -> level 6 coolest, same "distinguish nesting depth
    // by color temperature" idea org's own headline levels use elsewhere
    // in this codebase -- NVIM_PARITY_PLAN.md Phase 28 gap: every level
    // used to render identically as Purple, only the sign-column digit
    // told them apart.
    "local MEP_MD_HEADING_HL = {'Red', 'Orange', 'Yellow', 'Green', 'Blue', 'Purple'}\n"
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
    "        local hl = MEP_MD_HEADING_HL[level]\n"
    "        mep.deco_add(ns, {row = i, whole_line = true, hl_group = hl, sign = glyph, sign_hl = hl})\n"
    "      else\n"
    "        local covered = {}\n"
    "        for s, e in line:gmatch('()%*%*[^%*]+%*%*()') do\n"
    "          mep.deco_add(ns, {row = i, col_start = s, col_end = e, hl_group = 'Yellow'})\n"
    "          for k = s, e - 1 do covered[k] = true end\n"
    "        end\n"
    // Single-*/_ italic: NVIM_PARITY_PLAN.md Phase 28 gap -- md_highlight
    // never marked these at all (only **bold** and links), so italic text
    // rendered as plain, undecorated body text. `italic = true` reuses the
    // same rlgl-shear renderer org's own *Erm*/_italic_ emphasis already
    // drives (Decoration.italic, editor.h) rather than adding a second one.
    // `covered` skips single-*-flanking-bold spans already claimed above
    // (Lua patterns can't tell "**" from two adjacent unmatched "*"s).
    "        for s, e in line:gmatch('()%*[^%*\\n]+%*()') do\n"
    "          if not covered[s] then mep.deco_add(ns, {row = i, col_start = s, col_end = e, hl_group = 'Cyan', italic = true}) end\n"
    "        end\n"
    "        for s, e in line:gmatch('()_[^_\\n]+_()') do\n"
    "          mep.deco_add(ns, {row = i, col_start = s, col_end = e, hl_group = 'Cyan', italic = true})\n"
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

// Org-mode inline images: renders a `[[file:foo.png]]` link's target
// directly in the buffer instead of its literal text, toggled via
// <leader>oti (mep.org_images_toggle_ui, wrapping mep.org_images_toggle --
// lua_env.cpp's own binding onto Editor::ToggleOrgImages()). Detection
// lives here in Lua (mirrors kBuiltinOrgLinks' own mep.org_link_highlight,
// just above, which scans every line for `[[...]]` spans the same way);
// the C++ side (DrawPane's own image branch, main.cpp) only ever sees the
// resolved-path -> row registry this populates via mep.buf_set_image_row/
// buf_clear_image_rows -- the same "Lua decides *what*, C++ decides how to
// render it fast" split every other decoration-backed org feature in this
// file already uses (see e.g. mep.syntax_highlight -> mep.deco_add).
const char *kBuiltinOrgImages =
    // Resolves any org header-arg/link path (an inline image's own
    // `[[file:...]]` target, or -- since this is a plain global, reused
    // cross-chunk by kBuiltinOrgBabel below, which loads after this one --
    // a src block's `:file`/`:tangle` target too) against the *org file's
    // own directory* (mep_lsp_abspath(mep.filename()), already defined
    // above in kBuiltinLsp), real org-mode's own convention for a relative
    // path -- deliberately *not* the editor's cwd the way :e/:w
    // (mep_completion_path_prefix's own documented convention above) is.
    // Confirmed broken otherwise (both directions): opening an org file
    // from a different directory than mep was launched in left every
    // relative-path inline image unresolved on read, and a :file/:tangle
    // block wrote its output next to mep's own cwd instead of the org
    // file it was run from.
    "function mep_org_resolve_path(path)\n"
    "  if path:sub(1, 1) == '/' then return path end\n"
    "  if path:sub(1, 1) == '~' then\n"
    "    local home = os.getenv('HOME')\n"
    "    if home then return home .. path:sub(2) end\n"
    "    return path\n"
    "  end\n"
    "  local dir = mep_lsp_abspath(mep.filename()):match('^(.*)/[^/]*$')\n"
    "  return dir and (dir .. '/' .. path) or path\n"
    "end\n"
    "local MEP_ORG_IMAGE_EXTENSIONS = {png = true, jpg = true, jpeg = true, bmp = true, gif = true}\n"
    "local function mep_org_image_is_image_target(path)\n"
    "  local ext = path:match('%.([%a%d]+)$')\n"
    "  return ext ~= nil and MEP_ORG_IMAGE_EXTENSIONS[ext:lower()] == true\n"
    "end\n"
    // Rebuilds the current buffer's whole image-row registry from its
    // [[file:...]] links -- same split-on-'][' description handling as
    // mep.org_link_at_cursor above, and the same file:-prefix-only
    // dispatch mep.org_link_follow uses (a bare, unprefixed target isn't
    // treated as a file link anywhere else in this codebase either, so it
    // isn't here). A ::heading/::N suffix (mep.org_link_follow's own
    // intra-file-jump syntax) is stripped the same way before the
    // extension check, so `[[file:notes.org::*Setup]]` is never mistaken
    // for a (nonexistent) `.org::*Setup`-extension image.
    "function mep.org_image_scan()\n"
    "  mep.buf_clear_image_rows()\n"
    "  if mep_lsp_filetype(mep.filename()) ~= 'org' then return end\n"
    "  for i = 1, mep.line_count() do\n"
    "    local line, pos = mep.get_line(i), 1\n"
    "    while true do\n"
    "      local s, e, inner = line:find('%[%[(.-)%]%]', pos)\n"
    "      if not s then break end\n"
    "      pos = e + 1\n"
    "      local target = inner:match('^([^%]]+)%]%[.+$') or inner\n"
    "      if target:match('^file:') then\n"
    "        local path = target:sub(6):match('^([^#]*)') or target:sub(6)\n"
    "        if mep_org_image_is_image_target(path) then\n"
    "          mep.buf_set_image_row(i, mep_org_resolve_path(path))\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "end\n"
    "mep.command('MepOrgImageScan', mep.org_image_scan)\n"
    // Keeps the registry fresh regardless of the toggle's own on/off state
    // (see Buffer::org_image_rows' own comment) -- debounce-rerun on edits
    // (mep.on_buffer_changed) paired with a non-debounced file-switch
    // watcher, the exact same two-hook shape mep.syntax_highlight uses in
    // kBuiltinSyntax for the identical "buffer-changed doesn't fire on a
    // plain :e/:bn/picker switch with no edit" reason.
    "mep.on_buffer_changed(function()\n"
    "  if mep_lsp_filetype(mep.filename()) == 'org' then mep.org_image_scan() end\n"
    "end)\n"
    "local mep_org_image_last_file = nil\n"
    "mep.on_frame(function()\n"
    "  local fname = mep.filename()\n"
    "  if fname ~= mep_org_image_last_file then\n"
    "    mep_org_image_last_file = fname\n"
    "    if mep_lsp_filetype(fname) == 'org' then mep.org_image_scan() end\n"
    "  end\n"
    "end)\n"
    // Toggle: mep.org_images_toggle() (lua_env.cpp) flips Editor's own
    // org_images_visible_ and returns the new state -- this wrapper just
    // adds the user-facing notify + an immediate rescan when turning on,
    // so <leader>oti shows correct images right away instead of waiting
    // for the next debounced buffer-changed tick.
    "function mep.org_images_toggle_ui()\n"
    "  local visible = mep.org_images_toggle()\n"
    "  mep.notify('Org inline images: ' .. (visible and 'on' or 'off'))\n"
    "  if visible then mep.org_image_scan() end\n"
    "end\n"
    "mep.command('MepOrgImagesToggle', mep.org_images_toggle_ui)\n"
    "mep.leader_map('oti', 'Org: toggle inline images', mep.org_images_toggle_ui)\n";

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
    // Multi-language babel execution, ported from mep.nvim/lua/mep/org/
    // babel.lua's own `M.languages` (that module's own header comment
    // explains the source-of-truth policy: a language needs a real LSP
    // server and tree-sitter grammar registered there first) -- kept in
    // behavioral lockstep with it deliberately, so a fixture file
    // exercises the same ~27-language set in both editors. See flake.nix's
    // devShell `packages` list for the interpreter/compiler toolchain this
    // needs (mirrors mep.nvim/flake.nix's own babel-language section).
    //
    // Compiled languages (c/cpp/rust/go/fortran/d/java) go through an
    // extra compile step (mep_org_babel_spawn, below): the block body is
    // wrapped in each language's own entry-point form when `:main yes` (or
    // that language's own default -- see MEP_ORG_BABEL_WRAP_DEFAULT)
    // requests it, written to a temp source file, compiled to a temp
    // binary, then that binary is run -- two chained mep.job_start calls
    // instead of the one an interpreted language needs. A handful of
    // "interpreter that still needs its own subcommand" languages
    // (typescript/csharp/zig/nim/crystal) use `run_cmd` for the same
    // one-job shape with a non-default invocation (`dotnet run <file>`,
    // `zig run <file>`, ...). Java is the one language needing BOTH
    // `compile_cmd` and `run_compiled_cmd` overrides, since `javac -d
    // <dir>` produces a directory of .class files, not a single executable
    // binary_path can just exec.
    "local function mep_org_babel_extend(dst, src)\n"
    "  for _, v in ipairs(src) do dst[#dst + 1] = v end\n"
    "end\n"
    "mep.org_babel_langs = {}\n"
    "local L = mep.org_babel_langs\n"
    "-- Per-language descriptor: executable (checked via mep_org_babel_has_exe,\n"
    "-- with fallback_executable tried second), extension (temp script file\n"
    "-- suffix), var_stmt(name, literal) renders one :var prelude assignment,\n"
    "-- print_stmt(expr) renders \"print this expression\" for :results value mode\n"
    "-- (nil for a language with no single universal print expression -- :results\n"
    "-- value silently falls back to :results output's own plain-body behavior\n"
    "-- for those, same as real org-babel). compiled=true languages go through a\n"
    "-- real two-step compile-then-run (mep_org_babel_spawn); run_cmd overrides\n"
    "-- the plain `{exe, source_path}` invocation for an interpreter that needs\n"
    "-- its own subcommand first (dotnet run/zig run/nim r/crystal run);\n"
    "-- compile_cmd/run_compiled_cmd/detect_class are compiled-language-only\n"
    "-- overrides (see the go/java entries below). wrap_main, when present, is\n"
    "-- applied only when mep_org_babel_should_wrap_main(lang_key, args) says so\n"
    "-- (an explicit :main yes/no header-arg, else each language's own default --\n"
    "-- see MEP_ORG_BABEL_WRAP_DEFAULT below).\n"
    "L.lua = {\n"
    "  executable = 'lua', extension = '.lua',\n"
    "  var_stmt = function(n, l) return string.format('local %s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('print(%s)', e) end,\n"
    "}\n"
    "L.python = {\n"
    "  executable = 'python3', fallback_executable = 'python', extension = '.py',\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('print(%s)', e) end,\n"
    "}\n"
    "L.sh = {\n"
    "  executable = 'bash', fallback_executable = 'sh', extension = '.sh',\n"
    "  var_stmt = function(n, l) return string.format('%s=%s', n, l) end,\n"
    "}\n"
    "L.javascript = {\n"
    "  executable = 'node', extension = '.js',\n"
    "  var_stmt = function(n, l) return string.format('const %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('console.log(%s);', e) end,\n"
    "}\n"
    "L.cpp = {\n"
    "  executable = 'g++', fallback_executable = 'c++', extension = '.cpp', compiled = true,\n"
    "  var_stmt = function(n, l) return string.format('auto %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('std::cout << (%s) << std::endl;', e) end,\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = {}\n"
    "    for _, inc in ipairs(includes) do lines[#lines + 1] = '#include ' .. inc end\n"
    "    lines[#lines + 1] = 'int main() {'\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = '  return 0;'\n"
    "    lines[#lines + 1] = '}'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.c = {\n"
    "  executable = 'gcc', extension = '.c', compiled = true,\n"
    "  -- __auto_type (GCC/Clang extension) so :var needs no real type-guessing.\n"
    "  var_stmt = function(n, l) return string.format('__auto_type %s = %s;', n, l) end,\n"
    "  wrap_main = L.cpp.wrap_main,\n"
    "}\n"
    "L.ruby = {\n"
    "  executable = 'ruby', extension = '.rb',\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('puts(%s)', e) end,\n"
    "}\n"
    "L.typescript = {\n"
    "  -- bun (not node): runs a .ts file directly, no separate compile step.\n"
    "  executable = 'bun', extension = '.ts',\n"
    "  var_stmt = function(n, l) return string.format('const %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('console.log(%s);', e) end,\n"
    "}\n"
    "L.elixir = {\n"
    "  -- .exs (Elixir Script), not .ex -- what `elixir script.exs` expects.\n"
    "  executable = 'elixir', extension = '.exs',\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('IO.puts(%s)', e) end,\n"
    "}\n"
    "L.julia = {\n"
    "  executable = 'julia', extension = '.jl',\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('println(%s)', e) end,\n"
    "}\n"
    "L.clojure = {\n"
    "  -- bb (Babashka, fast-starting) tried before the full `clojure` CLI.\n"
    "  executable = 'bb', fallback_executable = 'clojure', extension = '.clj',\n"
    "  var_stmt = function(n, l) return string.format('(def %s %s)', n, l) end,\n"
    "  print_stmt = function(e) return string.format('(println %s)', e) end,\n"
    "}\n"
    "L.perl = {\n"
    "  executable = 'perl', extension = '.pl',\n"
    "  var_stmt = function(n, l) return string.format('my $%s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('print(%s, \"\\\\n\");', e) end,\n"
    "}\n"
    "L.r = {\n"
    "  executable = 'Rscript', extension = '.R',\n"
    "  var_stmt = function(n, l) return string.format('%s <- %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('print(%s)', e) end,\n"
    // Real org-babel-R's own `:results graphics` convenience: opens the
    // PNG device at the block's `:file` path before the body and closes
    // it after, so a `:file plot.png :results graphics file` block's body
    // can be nothing but plotting calls (plot/hist/...) with no explicit
    // png()/dev.off() of its own -- applied by mep_org_babel_prepare_script
    // below, only when both `:file` and `graphics` are present.
    "  graphics_wrap = function(path, body)\n"
    "    local lines = { string.format('png(%s)', mep_org_babel_format_literal(path)) }\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = 'dev.off()'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.php = {\n"
    "  executable = 'php', extension = '.php',\n"
    "  var_stmt = function(n, l) return string.format('$%s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('echo (%s) . PHP_EOL;', e) end,\n"
    "  -- Applied by default (:main no opts out) -- see MEP_ORG_BABEL_WRAP_DEFAULT:\n"
    "  -- a bare PHP snippet needs the <?php tag just to run as code at all.\n"
    "  wrap_main = function(_, body)\n"
    "    local lines = { '<?php' }\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.rust = {\n"
    "  executable = 'rustc', extension = '.rs', compiled = true,\n"
    "  var_stmt = function(n, l) return string.format('let %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('println!(\"{}\", %s);', e) end,\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = {}\n"
    "    for _, inc in ipairs(includes) do lines[#lines + 1] = 'use ' .. inc .. ';' end\n"
    "    lines[#lines + 1] = 'fn main() {'\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = '}'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.go = {\n"
    "  executable = 'go', extension = '.go', compiled = true,\n"
    "  -- `go build`'s subcommand comes before its -o flag, unlike gcc/g++/rustc.\n"
    "  compile_cmd = function(exe, source_path, binary_path)\n"
    "    return { exe, 'build', '-o', binary_path, source_path }\n"
    "  end,\n"
    "  var_stmt = function(n, l) return string.format('%s := %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('fmt.Println(%s)', e) end,\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = { 'package main', '', 'import (' }\n"
    "    for _, inc in ipairs(includes) do lines[#lines + 1] = '\\t\"' .. inc .. '\"' end\n"
    "    lines[#lines + 1] = ')'\n"
    "    lines[#lines + 1] = ''\n"
    "    lines[#lines + 1] = 'func main() {'\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = '}'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.fortran = {\n"
    "  -- .f90 (free-form modern Fortran), not .f/.for (old fixed-form).\n"
    "  executable = 'gfortran', extension = '.f90', compiled = true,\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('print *, %s', e) end,\n"
    "  -- A Fortran \"main program\" needs no entry-point keyword at all -- bare\n"
    "  -- statements followed by a lone `end` already form a complete program.\n"
    "  wrap_main = function(_, body)\n"
    "    local lines = {}\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = 'end'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.csharp = {\n"
    "  -- .NET \"file-based apps\" (`dotnet run <file>.cs`, no .csproj needed,\n"
    "  -- stable since .NET 10) + C# 9+ top-level statements means a bare-\n"
    "  -- statement body is already a complete program -- no wrap_main at all.\n"
    "  executable = 'dotnet', extension = '.cs',\n"
    "  run_cmd = function(exe, source_path) return { exe, 'run', source_path } end,\n"
    "  var_stmt = function(n, l) return string.format('var %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('Console.WriteLine(%s);', e) end,\n"
    "}\n"
    "L.scala = {\n"
    "  executable = 'scala', extension = '.scala',\n"
    "  var_stmt = function(n, l) return string.format('val %s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('println(%s)', e) end,\n"
    "  -- Scala 3 rejects top-level statements outside a real definition --\n"
    "  -- wraps in @main def, using its significant-whitespace syntax (no {}).\n"
    "  wrap_main = function(_, body)\n"
    "    local lines = { '@main def run(): Unit =' }\n"
    "    for _, line in ipairs(body) do lines[#lines + 1] = '  ' .. line end\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.zig = {\n"
    "  -- `zig run <file>` compiles and runs in one step -- run_cmd, not compiled.\n"
    "  executable = 'zig', extension = '.zig',\n"
    "  run_cmd = function(exe, source_path) return { exe, 'run', source_path } end,\n"
    "  var_stmt = function(n, l) return string.format('const %s = %s;', n, l) end,\n"
    "  -- Not std.debug.print (writes to stderr) -- through the stdout writer\n"
    "  -- wrap_main sets up (post-0.16 \"Writergate\": stdout needs an Io instance).\n"
    "  print_stmt = function(e) return string.format('try stdout.print(\"{}\\\\n\", .{%s});', e) end,\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = { 'const std = @import(\"std\");' }\n"
    "    for _, inc in ipairs(includes) do\n"
    "      lines[#lines + 1] = 'const ' .. inc .. ' = @import(\"' .. inc .. '\");'\n"
    "    end\n"
    "    lines[#lines + 1] = 'pub fn main(init: std.process.Init) !void {'\n"
    "    lines[#lines + 1] = '  var stdout_buffer: [4096]u8 = undefined;'\n"
    "    lines[#lines + 1] = '  var stdout_writer = std.Io.File.stdout().writer(init.io, &stdout_buffer);'\n"
    "    lines[#lines + 1] = '  const stdout = &stdout_writer.interface;'\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = '  try stdout.flush();'\n"
    "    lines[#lines + 1] = '}'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.nim = {\n"
    "  -- Nim allows bare top-level executable statements -- no wrap_main.\n"
    "  executable = 'nim', extension = '.nim',\n"
    "  run_cmd = function(exe, source_path)\n"
    "    -- nim derives its own \"module name\" from the basename and rejects one\n"
    "    -- that isn't a valid identifier (a tempname can land on a purely\n"
    "    -- numeric one) -- copy to a letter-prefixed sibling path first.\n"
    "    local dir, base = source_path:match('^(.*/)([^/]*)$')\n"
    "    local valid_path = (dir or '') .. 'm' .. base\n"
    "    local src = io.open(source_path, 'r')\n"
    "    local data = src:read('a')\n"
    "    src:close()\n"
    "    local dst = io.open(valid_path, 'w')\n"
    "    dst:write(data)\n"
    "    dst:close()\n"
    "    return { exe, 'r', '--hints:off', '--warnings:off', valid_path }\n"
    "  end,\n"
    "  var_stmt = function(n, l) return string.format('let %s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('echo %s', e) end,\n"
    "}\n"
    "L.crystal = {\n"
    "  -- Crystal, like Nim/Ruby, allows bare top-level statements -- no wrap_main.\n"
    "  executable = 'crystal', extension = '.cr',\n"
    "  run_cmd = function(exe, source_path) return { exe, 'run', source_path } end,\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('puts(%s)', e) end,\n"
    "}\n"
    "L.java = {\n"
    "  -- A genuine two-step compile-then-run (javac), unlike zig/nim/crystal.\n"
    "  -- executable = 'javac' (compile_cmd), the separate 'java' runtime is\n"
    "  -- hardcoded in run_compiled_cmd -- a real JDK install always ships both.\n"
    "  executable = 'javac', extension = '.java', compiled = true,\n"
    "  detect_class = function(lines)\n"
    "    for _, line in ipairs(lines) do\n"
    "      local name = line:match('public%s+class%s+([%a_][%w_]*)')\n"
    "      if name then return name end\n"
    "    end\n"
    "    for _, line in ipairs(lines) do\n"
    "      local name = line:match('%f[%w]class%s+([%a_][%w_]*)')\n"
    "      if name then return name end\n"
    "    end\n"
    "    return 'Main'\n"
    "  end,\n"
    "  -- binary_path is reused as a *directory* of .class files, not a single\n"
    "  -- executable -- `javac -d <dir>` has no single-artifact output mode.\n"
    "  -- Copies source_path to a sibling <ClassName>.java first: a `public\n"
    "  -- class HelloWorld` must live in a file named exactly HelloWorld.java.\n"
    "  compile_cmd = function(exe, source_path, binary_path, class_name)\n"
    "    local dir = source_path:match('^(.*/)') or ''\n"
    "    local named_path = dir .. class_name .. '.java'\n"
    "    local src = io.open(source_path, 'r')\n"
    "    local data = src:read('a')\n"
    "    src:close()\n"
    "    local dst = io.open(named_path, 'w')\n"
    "    dst:write(data)\n"
    "    dst:close()\n"
    "    return { exe, '-d', binary_path, named_path }\n"
    "  end,\n"
    "  run_compiled_cmd = function(binary_path, class_name) return { 'java', '-cp', binary_path, class_name } end,\n"
    "  var_stmt = function(n, l) return string.format('var %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('System.out.println(%s);', e) end,\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = {}\n"
    "    for _, inc in ipairs(includes) do lines[#lines + 1] = 'import ' .. inc .. ';' end\n"
    "    lines[#lines + 1] = 'class Main {'\n"
    "    lines[#lines + 1] = '  public static void main(String[] args) {'\n"
    "    for _, line in ipairs(body) do lines[#lines + 1] = '    ' .. line end\n"
    "    lines[#lines + 1] = '  }'\n"
    "    lines[#lines + 1] = '}'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.kotlin = {\n"
    "  -- Kotlin's .kts *script* mode allows bare top-level statements -- no wrap.\n"
    "  executable = 'kotlin', extension = '.kts',\n"
    "  var_stmt = function(n, l) return string.format('val %s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('println(%s)', e) end,\n"
    "}\n"
    "L.haskell = {\n"
    "  -- runghc (GHC's own script interpreter, no separate compile step).\n"
    "  executable = 'runghc', extension = '.hs',\n"
    "  var_stmt = function(n, l) return string.format('%s = %s', n, l) end,\n"
    "  print_stmt = function(e) return string.format('print (%s)', e) end,\n"
    "  -- Bare IO statements must be the right-hand side of a top-level `main`\n"
    "  -- binding, do-notation for more than one. KNOWN LIMITATION (matches\n"
    "  -- mep.nvim's own): :var's bare top-level binding lands inside this do\n"
    "  -- block, where a non-let binding is a syntax error -- :var + :main yes\n"
    "  -- together isn't supported for Haskell; either alone works fine.\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = {}\n"
    "    for _, inc in ipairs(includes) do lines[#lines + 1] = 'import ' .. inc end\n"
    "    lines[#lines + 1] = 'main = do'\n"
    "    for _, line in ipairs(body) do lines[#lines + 1] = '  ' .. line end\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.ocaml = {\n"
    "  -- OCaml's own toplevel run as a script -- allows bare top-level phrases.\n"
    "  -- No print_stmt: no single universal print expression (Printf.printf\n"
    "  -- needs a format specifier matched to the value's type).\n"
    "  executable = 'ocaml', extension = '.ml',\n"
    "  var_stmt = function(n, l) return string.format('let %s = %s;;', n, l) end,\n"
    "}\n"
    "L.d = {\n"
    "  -- A real two-step compile-then-run (dmd), unlike zig/nim/crystal/kotlin.\n"
    "  executable = 'dmd', extension = '.d', compiled = true,\n"
    "  -- dmd's output flag is one joined token (-of=<path>), not gcc/g++/rustc's\n"
    "  -- shared two-token `-o <path>` shape. Same module-name fix as nim's\n"
    "  -- run_cmd: dmd derives a module name from the basename and rejects a\n"
    "  -- purely numeric one.\n"
    "  compile_cmd = function(exe, source_path, binary_path)\n"
    "    local dir, base = source_path:match('^(.*/)([^/]*)$')\n"
    "    local valid_path = (dir or '') .. 'm' .. base\n"
    "    local src = io.open(source_path, 'r')\n"
    "    local data = src:read('a')\n"
    "    src:close()\n"
    "    local dst = io.open(valid_path, 'w')\n"
    "    dst:write(data)\n"
    "    dst:close()\n"
    "    return { exe, valid_path, '-of=' .. binary_path }\n"
    "  end,\n"
    "  var_stmt = function(n, l) return string.format('auto %s = %s;', n, l) end,\n"
    "  print_stmt = function(e) return string.format('writeln(%s);', e) end,\n"
    "  wrap_main = function(includes, body)\n"
    "    local lines = { 'import std.stdio;' }\n"
    "    for _, inc in ipairs(includes) do lines[#lines + 1] = 'import ' .. inc .. ';' end\n"
    "    lines[#lines + 1] = 'void main() {'\n"
    "    mep_org_babel_extend(lines, body)\n"
    "    lines[#lines + 1] = '}'\n"
    "    return lines\n"
    "  end,\n"
    "}\n"
    "L.bash = L.sh\n"
    "L.js = L.javascript\n"
    "L['c++'] = L.cpp\n"
    "L.ts = L.typescript\n"
    "L.cs = L.csharp\n"
    "L['c#'] = L.csharp\n"
    "-- lang key (lowercased babel token) -> which way :main's absence defaults\n"
    "-- for a wrap_main-having language. Every entry-point language defaults to\n"
    "-- 'no' (assume a self-contained program); PHP is the sole exception.\n"
    "local MEP_ORG_BABEL_WRAP_DEFAULT = { php = 'yes' }\n"
    "\n"
    "function mep_org_babel_should_wrap_main(lang_key, args_str)\n"
    "  local main = args_str:match(':main%s+(%S+)')\n"
    "  if main == 'yes' or main == 'no' then return main == 'yes' end\n"
    "  return (MEP_ORG_BABEL_WRAP_DEFAULT[lang_key] or 'no') == 'yes'\n"
    "end\n"
    "\n"
    "-- Whether `exe` resolves on PATH -- mirrors vim.fn.executable's contract\n"
    "-- via a plain `command -v` shellout (POSIX-portable, no injection risk:\n"
    "-- `exe` only ever comes from this file's own hardcoded language table,\n"
    "-- never from user/org-file content).\n"
    "function mep_org_babel_has_exe(exe)\n"
    "  local ok = os.execute('command -v ' .. exe .. ' >/dev/null 2>&1')\n"
    "  return ok == true\n"
    "end\n"
    "\n"
    "-- `lang_def`'s resolved executable (trying fallback_executable second), or\n"
    "-- nil if neither is on PATH.\n"
    "function mep_org_babel_resolve_exe(lang_def)\n"
    "  if mep_org_babel_has_exe(lang_def.executable) then return lang_def.executable end\n"
    "  if lang_def.fallback_executable and mep_org_babel_has_exe(lang_def.fallback_executable) then\n"
    "    return lang_def.fallback_executable\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "\n"
    "-- `lang_key`'s language def and resolved executable, or nil, err (a\n"
    "-- ready-to-notify message) if unsupported or has no interpreter on PATH.\n"
    "function mep_org_babel_resolve_lang(lang_key)\n"
    "  local lang_def = mep.org_babel_langs[lang_key]\n"
    "  if not lang_def then\n"
    "    return nil, 'No babel support for: ' .. tostring(lang_key)\n"
    "  end\n"
    "  local exe = mep_org_babel_resolve_exe(lang_def)\n"
    "  if not exe then\n"
    "    local wanted = lang_def.executable\n"
    "    if lang_def.fallback_executable then wanted = wanted .. '/' .. lang_def.fallback_executable end\n"
    "    return nil, 'No ' .. lang_key .. ' interpreter found on PATH (looked for ' .. wanted .. ')'\n"
    "  end\n"
    "  return lang_def, exe\n"
    "end\n"
    "\n"
    "-- `raw` (the right-hand side of a :var name=raw pair) as a literal for\n"
    "-- injection into a script prelude: a bare number passes through as-is,\n"
    "-- anything else becomes a backslash/quote-escaped double-quoted string --\n"
    "-- the same convention across lua/python/js/c-family/etc.\n"
    "function mep_org_babel_format_literal(raw)\n"
    "  raw = tostring(raw)\n"
    "  if raw:match('^%-?%d+%.?%d*$') then return raw end\n"
    "  local esc = raw:gsub('\\\\', '\\\\\\\\'):gsub('\"', '\\\\\"'):gsub('\\n', '\\\\n'):gsub('\\r', '\\\\r'):gsub('\\t', '\\\\t')\n"
    "  return '\"' .. esc .. '\"'\n"
    "end\n"
    "\n"
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
    // `:results` header-arg (Phase 34 gap): space-separated mode
    // keywords captured as a set, stopping at the next `:key` header-arg
    // (or end of string) -- `silent` suppresses the #+RESULTS: block
    // entirely, `table` reformats tabular output, `value`/`output` pick
    // the collection strategy (see mep_org_babel_prepare_script below).
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
    "    file = args_str:match(':file%s+(%S+)'),\n"
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
    // `raw` (Phase 34 gap, :results table; also used for a `:file`
    // block's own single-line `[[file:...]]` link result -- see
    // mep.org_babel_execute below): when true, out_lines is already Org
    // syntax and gets inserted verbatim (no `: ` prefix, no
    // #+begin_example fence) -- and the existing-block detector below
    // now also recognizes a prior table (lines starting with `|`) or
    // file link (`[[file:`) so re-running in place replaces it instead
    // of leaving a stale one behind.
    // Pure formatting: out_lines/raw -> the literal '#+RESULTS:'-headed
    // block of lines to insert -- no buffer access. Factored out of
    // mep.org_babel_insert_results (below, its only caller before this
    // change) so mep_org_babel_splice_results (kBuiltinOrgExport, this
    // file, added for export-time babel execution) can build the exact
    // same shape against a scratch lines array instead of the live
    // buffer, without duplicating the value/table/example-fence rules.
    "function mep_org_babel_format_results_block(out_lines, raw)\n"
    "  if raw then\n"
    "    local block = {'#+RESULTS:'}\n"
    "    for _, l in ipairs(out_lines) do block[#block + 1] = l end\n"
    "    return block\n"
    "  elseif #out_lines <= 1 then\n"
    "    return {'#+RESULTS:', ': ' .. (out_lines[1] or '')}\n"
    "  else\n"
    "    local block = {'#+RESULTS:', '#+begin_example'}\n"
    "    for _, l in ipairs(out_lines) do block[#block + 1] = l end\n"
    "    block[#block + 1] = '#+end_example'\n"
    "    return block\n"
    "  end\n"
    "end\n"
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
    "    elseif line_i:match('^%s*:') or line_i:match('^%s*|') or line_i:match('^%s*%[%[file:') then\n"
    "      while i <= mep.line_count() and ((mep.get_line(i) or ''):match('^%s*:') or (mep.get_line(i) or ''):match('^%s*|') or (mep.get_line(i) or ''):match('^%s*%[%[file:')) do i = i + 1 end\n"
    "      existing_end = i - 1\n"
    "    else\n"
    "      existing_end = existing_start\n"
    "    end\n"
    "  end\n"
    "  local block = mep_org_babel_format_results_block(out_lines, raw)\n"
    "  if existing_start then\n"
    "    mep.replace_lines(existing_start, existing_end + 1, block)\n"
    "  else\n"
    "    mep.replace_lines(insert_row + 1, insert_row + 1, block)\n"
    "  end\n"
    "end\n"
    "-- Splits a flat \"\\n\"-joined body string (mep_org_src_block_at's own\n"
    "-- `blk.body` shape) into a line array -- every wrap_main/print_stmt\n"
    "-- function above works on line arrays (ported directly from mep.nvim's\n"
    "-- own list-of-lines model), joined back to a single string only once, right\n"
    "-- before writing the temp source file.\n"
    "function mep_org_babel_split_lines(str)\n"
    "  local lines = {}\n"
    "  for l in (str .. '\\n'):gmatch('(.-)\\n') do lines[#lines + 1] = l end\n"
    "  -- gmatch on \"str .. '\\n'\" always yields one trailing empty match past the\n"
    "  -- real last line (the synthetic newline just appended) -- drop it, unless\n"
    "  -- str was itself empty (in which case one empty line is correct: an\n"
    "  -- empty block body is one empty line, not zero lines).\n"
    "  if #lines > 1 and lines[#lines] == '' then lines[#lines] = nil end\n"
    "  return lines\n"
    "end\n"
    "\n"
    "-- Build the script text (a line array) to actually run for `lang_key`/\n"
    "-- `lang_def`/`vars` (blk.vars, a name->raw_value map)/`body_lines`/\n"
    "-- `args_str`: :var prelude assignments (each via lang_def.var_stmt), then\n"
    "-- the body -- except in :results value mode (only meaningful for a\n"
    "-- language with a print_stmt), where the body's last non-blank line is\n"
    "-- treated as an expression and wrapped in print_stmt instead of run as-is.\n"
    "-- Deliberately the same simplification mep.nvim's own build_script makes\n"
    "-- (see its header comment): a bare expression as a block's last line, not\n"
    "-- real per-language value-capture machinery. graphics_wrap runs next, if\n"
    "-- the language has one, `resolved_file` (the block's :file target, or nil)\n"
    "-- was given, and `results_modes.graphics` is set -- see L.r's own\n"
    "-- graphics_wrap comment. wrap_main is applied last, if the language has\n"
    "-- one and mep_org_babel_should_wrap_main opts in.\n"
    "function mep_org_babel_prepare_script(lang_key, lang_def, vars, body_lines, args_str, results_modes, resolved_file)\n"
    "  local prelude = {}\n"
    "  for name, raw in pairs(vars) do\n"
    "    prelude[#prelude + 1] = lang_def.var_stmt(name, mep_org_babel_format_literal(raw))\n"
    "  end\n"
    "\n"
    "  local script = {}\n"
    "  mep_org_babel_extend(script, prelude)\n"
    "  if results_modes.value and lang_def.print_stmt then\n"
    "    local trimmed = {}\n"
    "    mep_org_babel_extend(trimmed, body_lines)\n"
    "    while #trimmed > 0 and trimmed[#trimmed]:match('^%s*$') do trimmed[#trimmed] = nil end\n"
    "    if #trimmed > 0 then\n"
    "      local last = trimmed[#trimmed]\n"
    "      trimmed[#trimmed] = nil\n"
    "      mep_org_babel_extend(script, trimmed)\n"
    "      script[#script + 1] = lang_def.print_stmt(last)\n"
    "    end\n"
    "  else\n"
    "    mep_org_babel_extend(script, body_lines)\n"
    "  end\n"
    "\n"
    "  if lang_def.graphics_wrap and resolved_file and results_modes.graphics then\n"
    "    script = lang_def.graphics_wrap(resolved_file, script)\n"
    "  end\n"
    "\n"
    "  if lang_def.wrap_main and mep_org_babel_should_wrap_main(lang_key, args_str) then\n"
    "    local includes = {}\n"
    "    local includes_str = args_str:match(':includes%s+(.-)%s*:') or args_str:match(':includes%s+(.*)$')\n"
    "    if includes_str then\n"
    "      for inc in includes_str:gmatch('%S+') do includes[#includes + 1] = inc end\n"
    "    end\n"
    "    script = lang_def.wrap_main(includes, script)\n"
    "  end\n"
    "  return script\n"
    "end\n"
    "\n"
    "-- Writes `script_lines` to a temp file and runs it (compiled or not),\n"
    "-- calling `on_finish(code, stdout_lines, stderr_lines, failure_verb)`\n"
    "-- exactly once when the whole run (for a compiled language: compile, then\n"
    "-- execute) settles. `stdout_lines` is always passed (even empty, on a\n"
    "-- compile failure) so the caller can still write out whatever partial\n"
    "-- output a failed *run* produced, matching mep.nvim's own M.execute\n"
    "-- contract exactly. `args_str` supplies :classname (Java only, via\n"
    "-- lang_def.detect_class).\n"
    // `cwd` (optional, defaults to '.' -- mep's own process cwd, the old
    // unconditional behavior) is the directory the spawned process itself
    // runs in: mep.org_babel_execute passes the *org file's own
    // directory* here so a script that saves output via a bare relative
    // path (R's `png("plot.png")`, however it got there -- L.r's own
    // graphics_wrap or hand-written; a Python block's own
    // `plt.savefig("plot.png")`; anything else with no `:file`-awareness
    // of its own at all) lands next to the org file, not next to wherever
    // mep happened to be launched from. mep.leetcode_run_tests' own call
    // site omits it -- a leetcode solution/test pair has no file-output
    // concept, so '.' (its own process's cwd) is exactly as meaningful as
    // any other directory would be there.
    "function mep_org_babel_spawn(lang_def, exe, script_lines, args_str, on_finish, cwd)\n"
    "  cwd = cwd or '.'\n"
    "  local source_path = os.tmpname() .. lang_def.extension\n"
    "  local f = io.open(source_path, 'w')\n"
    "  f:write(table.concat(script_lines, '\\n'))\n"
    "  f:close()\n"
    "\n"
    "  if lang_def.compiled then\n"
    "    local binary_path = os.tmpname()\n"
    "    local class_name = args_str:match(':classname%s+(%S+)')\n"
    "      or (lang_def.detect_class and lang_def.detect_class(script_lines))\n"
    "    local compile_cmd = lang_def.compile_cmd and lang_def.compile_cmd(exe, source_path, binary_path, class_name)\n"
    "      or { exe, source_path, '-o', binary_path }\n"
    "    local compile_err = {}\n"
    "    mep.job_start(compile_cmd, {\n"
    "      cwd = cwd,\n"
    "      on_stderr = function(line) compile_err[#compile_err + 1] = line end,\n"
    "      on_exit = function(compile_code)\n"
    "        os.remove(source_path)\n"
    "        if compile_code ~= 0 then\n"
    "          on_finish(compile_code, {}, compile_err, 'compilation')\n"
    "          return\n"
    "        end\n"
    "        local out, err = {}, {}\n"
    "        mep.job_start(\n"
    "          lang_def.run_compiled_cmd and lang_def.run_compiled_cmd(binary_path, class_name) or { binary_path },\n"
    "          {\n"
    "            cwd = cwd,\n"
    "            on_stdout = function(line) out[#out + 1] = line end,\n"
    "            on_stderr = function(line) err[#err + 1] = line end,\n"
    "            on_exit = function(run_code)\n"
    "              os.remove(binary_path)\n"
    "              on_finish(run_code, out, err, 'execution')\n"
    "            end,\n"
    "          }\n"
    "        )\n"
    "      end,\n"
    "    })\n"
    "    return\n"
    "  end\n"
    "\n"
    "  local out, err = {}, {}\n"
    "  mep.job_start(lang_def.run_cmd and lang_def.run_cmd(exe, source_path) or { exe, source_path }, {\n"
    "    cwd = cwd,\n"
    "    on_stdout = function(line) out[#out + 1] = line end,\n"
    "    on_stderr = function(line) err[#err + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      os.remove(source_path)\n"
    "      on_finish(code, out, err, 'execution')\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "\n"
    "-- The stderr line to surface in a failure notification: the first line,\n"
    "-- skipping any leading \"# <package>\" header (`go build`/`go run` always\n"
    "-- print one of these before the real error).\n"
    "function mep_org_babel_first_error_line(stderr)\n"
    "  for _, line in ipairs(stderr) do\n"
    "    if not line:match('^#%s') then return line end\n"
    "  end\n"
    "  return stderr[1]\n"
    "end\n"
    "\n"
    "-- Whether `path` exists on disk right now -- called with the *resolved*\n"
    "-- (mep_org_resolve_path, kBuiltinOrgImages) form of a block's :file\n"
    "-- target, never the raw header-arg text, so this always checks the same\n"
    "-- place the block's own subprocess actually ran in (mep_org_babel_spawn's\n"
    "-- own `cwd` -- see mep.org_babel_execute).\n"
    "function mep_org_babel_file_exists(path)\n"
    "  local f = io.open(path, 'r')\n"
    "  if f then f:close() return true end\n"
    "  return false\n"
    "end\n"
    "\n"
    "-- Writes a block's results as either its normal text/table result, or --\n"
    "-- when `blk.file` is set and `resolved_file` (mep_org_resolve_path'd,\n"
    "-- passed in by mep.org_babel_execute -- nil for the mep_org_babel_cache\n"
    "-- fallback path below, when the block didn't set :file at all) exists on\n"
    "-- disk right now -- a single [[file:...]] link (real org-mode's own\n"
    "-- convention for a `:results file` block; see L.r's own `graphics_wrap`\n"
    "-- comment for how a block actually produces that file), using `blk.file`\n"
    "-- itself (not `resolved_file`) as the link text so it stays exactly what\n"
    "-- the user typed -- portable if the org file and its image both move\n"
    "-- together, and openable by mep's own inline-image rendering\n"
    "-- (kBuiltinOrgImages' own mep.org_image_scan, <leader>oti). A :file block\n"
    "-- whose code didn't actually produce the target (a bug in it, or `:file`\n"
    "-- set on a block that was never going to write one) falls back to the\n"
    "-- normal text result instead, with a warning, rather than linking to\n"
    "-- nothing. mep.org_image_invalidate + a final mep.org_image_scan (both\n"
    "-- unconditional, whichever branch ran) make a just-finished run's own\n"
    "-- output show up immediately: invalidate drops any cached texture for\n"
    "-- `resolved_file` so a re-run's fresh file is *guaranteed* a real reload\n"
    "-- next frame rather than trusting GetOrLoadOrgInlineImageTexture's own\n"
    "-- mtime check, which isn't reliable here on its own -- confirmed\n"
    "-- reproducible in practice: a fast re-run's finished, fully-written file\n"
    "-- can land on the exact same one-second-granularity mtime a mid-write\n"
    "-- frame already cached a decode *failure* against (R's/Python's own PNG\n"
    "-- writer creates/truncates the file immediately and only finishes\n"
    "-- writing pixels at the very end), which mtime-only checking then never\n"
    "-- notices changed again -- and re-scanning picks up a row whose link\n"
    "-- text just changed shape (freshly appeared, or reverted to plain text)\n"
    "-- without waiting up to mep.on_buffer_changed's own 0.3s debounce.\n"
    // Pure decision logic (no buffer/image-cache access): what a
    // finished block's results should look like -- a :file result whose
    // target now actually exists on disk becomes a raw [[file:...]]
    // link line; everything else becomes its (optionally table-
    // formatted) plain output lines. Split out of
    // mep_org_babel_write_results (below, its only caller before this
    // change) so mep.org_babel_run_for_export (kBuiltinOrgExport, this
    // file, added for export-time babel execution) can reuse the exact
    // same decision when building a scratch #+RESULTS: block, without
    // ever touching mep.org_image_invalidate/scan or the live buffer --
    // those stay in mep_org_babel_write_results itself, gated on the new
    // third return value so it doesn't have to re-derive the :file
    // condition a second time.
    "function mep_org_babel_result_lines(blk, out_lines, resolved_file)\n"
    "  if blk.file and resolved_file and mep_org_babel_file_exists(resolved_file) then\n"
    "    return {'[[file:' .. blk.file .. ']]'}, true, true\n"
    "  end\n"
    "  if blk.file then\n"
    "    mep.notify('Babel: :file target was not created (' .. blk.file .. ')', 'warn')\n"
    "  end\n"
    "  local lines = blk.results_modes.table and mep_org_format_table(out_lines) or out_lines\n"
    "  return lines, blk.results_modes.table, false\n"
    "end\n"
    "function mep_org_babel_write_results(blk, out_lines, resolved_file)\n"
    "  local lines, raw, is_file = mep_org_babel_result_lines(blk, out_lines, resolved_file)\n"
    "  if is_file then mep.org_image_invalidate(resolved_file) end\n"
    "  mep.org_babel_insert_results(blk, lines, raw)\n"
    "  mep.org_image_scan()\n"
    "end\n"
    "function mep.org_babel_execute()\n"
    "  local blk = mep_org_src_block_at(mep.cursor())\n"
    "  if not blk then mep.notify('Not in a src block', 'warn') return end\n"
    "  local lang_def, exe_or_err = mep_org_babel_resolve_lang(blk.lang)\n"
    "  if not lang_def then mep.notify(exe_or_err, 'warn') return end\n"
    "  local exe = exe_or_err\n"
    // `blk_dir` (the org file's own directory) becomes the spawned
    // subprocess's own cwd (mep_org_babel_spawn's own `cwd` param) --
    // this, not resolved_file below, is what actually makes a plain
    // relative save path in the block's *own source code* (Python's
    // `plt.savefig("plot.png")`, hand-written R that never went through
    // graphics_wrap, ...) land next to the org file: mep has no way to
    // rewrite an arbitrary language's own literal string arguments, only
    // the directory the process that evaluates them runs in.
    // `resolved_file`, by contrast, is only ever used *within this Lua
    // process* (graphics_wrap's own png() argument, and the post-run
    // mep_org_babel_file_exists check below) -- see mep_org_resolve_path's
    // own comment (kBuiltinOrgImages) for why both matter and neither
    // alone is enough.
    "  local blk_dir = mep_lsp_abspath(mep.filename()):match('^(.*)/[^/]*$')\n"
    "  local resolved_file = blk.file and mep_org_resolve_path(blk.file) or nil\n"
    "\n"
    "  local cache_key = blk.lang .. '|' .. blk.args_str .. '|' .. blk.body\n"
    "  if blk.cache == 'yes' and mep_org_babel_cache[cache_key] then\n"
    "    if not blk.results_modes.silent then\n"
    "      mep_org_babel_write_results(blk, mep_org_babel_cache[cache_key], resolved_file)\n"
    "    end\n"
    "    mep.notify('Babel: cached result')\n"
    "    return\n"
    "  end\n"
    "\n"
    "  local body_lines = mep_org_babel_split_lines(blk.body)\n"
    "  local script_lines = mep_org_babel_prepare_script(\n"
    "    blk.lang, lang_def, blk.vars, body_lines, blk.args_str, blk.results_modes, resolved_file)\n"
    "\n"
    "  mep_org_babel_spawn(lang_def, exe, script_lines, blk.args_str, function(code, out_lines, err_lines, failure_verb)\n"
    "    if code ~= 0 then\n"
    "      mep.notify(\n"
    "        'Babel: '\n"
    "          .. failure_verb\n"
    "          .. ' failed ('\n"
    "          .. blk.lang\n"
    "          .. ', exit '\n"
    "          .. code\n"
    "          .. '): '\n"
    "          .. (mep_org_babel_first_error_line(err_lines) or 'no error output'),\n"
    "        'warn'\n"
    "      )\n"
    "    else\n"
    "      if blk.cache == 'yes' then\n"
    "        mep_org_babel_cache[cache_key] = out_lines\n"
    "        mep_org_babel_cache_save()\n"
    "      end\n"
    "      mep.notify('Babel: executed ' .. blk.lang .. ' block (exit ' .. code .. ')')\n"
    "    end\n"
    "    if not blk.results_modes.silent then\n"
    "      if code == 0 then\n"
    "        mep_org_babel_write_results(blk, out_lines, resolved_file)\n"
    "      else\n"
    "        local lines = blk.results_modes.table and mep_org_format_table(out_lines) or out_lines\n"
    "        mep.org_babel_insert_results(blk, lines, blk.results_modes.table)\n"
    "      end\n"
    "    end\n"
    "  end, blk_dir)\n"
    "end\n"
    "mep.command('MepOrgBabelExecute', mep.org_babel_execute)\n"
    // Tangle: concatenate same-`:tangle target` blocks in document
    // order, write to the target file -- resolved (mep_org_resolve_path,
    // kBuiltinOrgImages) against the org file's own directory, the same
    // fix mep.org_babel_execute's own :file handling gets just above, and
    // for the identical reason: a bare relative :tangle target used to
    // land next to mep's own cwd instead of the org file it came from.
    "function mep.org_babel_tangle()\n"
    "  local targets, order = {}, {}\n"
    "  for i = 1, mep.line_count() do\n"
    "    if mep.get_line(i):match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]') then\n"
    "      local blk = mep_org_src_block_at(i)\n"
    "      if blk and blk.tangle then\n"
    "        local target = mep_org_resolve_path(blk.tangle)\n"
    "        if not targets[target] then targets[target] = {} order[#order + 1] = target end\n"
    "        table.insert(targets[target], blk.body)\n"
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
    "mep.command('MepOrgBabelTangle', mep.org_babel_tangle)\n"
    // Export-time babel execution (kBuiltinOrgExport's mep_org_export_
    // prepare is the only caller): runs every #+BEGIN_SRC block in the
    // CURRENT buffer, in document order, sequentially -- each waits for
    // the previous mep.job_start call to actually finish, since spawning
    // is async -- via the exact same resolve/prepare/spawn/cache
    // pipeline mep.org_babel_execute uses for a single block, but WITHOUT
    // touching the live buffer: `callback(lines)` fires once with a
    // fresh COPY of the buffer's lines, with a formatted #+RESULTS: block
    // spliced in (replacing any stale one already there) right after
    // each executed block. `:eval no`/`never`/`query`/anything ending in
    // `-export` skips a block, matching real org's own eval-gating header
    // arg (a query can't be interactively answered during a batch
    // export, so it's treated as a skip rather than a hang). A block
    // whose language can't be resolved (missing interpreter) is skipped
    // with a warning notification rather than aborting the whole export.
    // Known gap, documented rather than silently wrong: operates on this
    // buffer's own lines only, before #+INCLUDE:/macro resolution (that
    // happens afterward, in mep_org_export_prepare) -- a code block
    // living inside an #+INCLUDE:'d file is never evaluated, only
    // whatever static content that file already had is spliced in
    // (unchanged from every other #+INCLUDE:'d line). Extending this to
    // resolve+evaluate includes first would need a lines-array variant
    // of mep_org_src_block_at (which reads mep.get_line by row today),
    // not just of the include-resolution step itself.
    "function mep.org_babel_run_for_export(callback)\n"
    "  local base_lines = {}\n"
    "  for i = 1, mep.line_count() do base_lines[i] = mep.get_line(i) end\n"
    "  local blocks = {}\n"
    "  for i = 1, #base_lines do\n"
    "    if base_lines[i]:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]') then\n"
    "      local blk = mep_org_src_block_at(i)\n"
    "      if blk then blocks[#blocks + 1] = blk end\n"
    "    end\n"
    "  end\n"
    "  if #blocks == 0 then\n"
    "    callback(base_lines)\n"
    "    return\n"
    "  end\n"
    "  local blk_dir = mep_lsp_abspath(mep.filename()):match('^(.*)/[^/]*$') or '.'\n"
    "  local results = {}\n"
    "  local function run_index(idx)\n"
    "    if idx > #blocks then\n"
    "      callback(mep_org_babel_splice_results(base_lines, blocks, results))\n"
    "      return\n"
    "    end\n"
    "    local blk = blocks[idx]\n"
    "    local eval_arg = blk.args_str:match(':eval%s+(%S+)')\n"
    "    local skip = eval_arg and (eval_arg == 'no' or eval_arg == 'never' or eval_arg:match('%-export$') or eval_arg:match('^query'))\n"
    "    if skip then\n"
    "      run_index(idx + 1)\n"
    "      return\n"
    "    end\n"
    "    local lang_def, exe_or_err = mep_org_babel_resolve_lang(blk.lang)\n"
    "    if not lang_def then\n"
    "      mep.notify('Babel (export): ' .. exe_or_err, 'warn')\n"
    "      run_index(idx + 1)\n"
    "      return\n"
    "    end\n"
    "    local resolved_file = blk.file and mep_org_resolve_path(blk.file) or nil\n"
    "    local cache_key = blk.lang .. '|' .. blk.args_str .. '|' .. blk.body\n"
    "    if blk.cache == 'yes' and mep_org_babel_cache[cache_key] then\n"
    "      local lines, raw = mep_org_babel_result_lines(blk, mep_org_babel_cache[cache_key], resolved_file)\n"
    "      results[blk.end_row] = {lines = lines, raw = raw}\n"
    "      run_index(idx + 1)\n"
    "      return\n"
    "    end\n"
    "    local body_lines = mep_org_babel_split_lines(blk.body)\n"
    "    local script_lines = mep_org_babel_prepare_script(\n"
    "      blk.lang, lang_def, blk.vars, body_lines, blk.args_str, blk.results_modes, resolved_file)\n"
    "    mep_org_babel_spawn(lang_def, exe_or_err, script_lines, blk.args_str, function(code, out_lines, err_lines, failure_verb)\n"
    "      if code ~= 0 then\n"
    "        mep.notify(\n"
    "          'Babel (export): ' .. failure_verb .. ' failed (' .. blk.lang .. ', exit ' .. code .. '): '\n"
    "            .. (mep_org_babel_first_error_line(err_lines) or 'no error output'),\n"
    "          'warn'\n"
    "        )\n"
    "        results[blk.end_row] = {lines = err_lines, raw = false}\n"
    "      else\n"
    "        if blk.cache == 'yes' then\n"
    "          mep_org_babel_cache[cache_key] = out_lines\n"
    "          mep_org_babel_cache_save()\n"
    "        end\n"
    "        local lines, raw = mep_org_babel_result_lines(blk, out_lines, resolved_file)\n"
    "        results[blk.end_row] = {lines = lines, raw = raw}\n"
    "      end\n"
    "      run_index(idx + 1)\n"
    "    end, blk_dir)\n"
    "  end\n"
    "  run_index(1)\n"
    "end\n"
    // Returns a NEW lines array: base_lines with, right after each block
    // in `blocks` that has a `results[blk.end_row]` entry, a formatted
    // #+RESULTS: block spliced in -- replacing (not duplicating) any
    // existing #+RESULTS: block already following that src block in
    // base_lines, mirroring mep.org_babel_insert_results' own existing-
    // block detection, ported to array indexing instead of
    // mep.get_line/mep.replace_lines.
    "function mep_org_babel_splice_results(base_lines, blocks, results)\n"
    "  local by_row = {}\n"
    "  for _, blk in ipairs(blocks) do\n"
    "    local r = results[blk.end_row]\n"
    "    if r then by_row[blk.end_row] = mep_org_babel_format_results_block(r.lines, r.raw) end\n"
    "  end\n"
    "  local out, i, n = {}, 1, #base_lines\n"
    "  while i <= n do\n"
    "    out[#out + 1] = base_lines[i]\n"
    "    local formatted = by_row[i]\n"
    "    if formatted then\n"
    "      local j = i + 1\n"
    "      if (base_lines[j] or ''):match('^%s*#%+RESULTS:%s*$') then\n"
    "        local k = j + 1\n"
    "        local line_k = base_lines[k] or ''\n"
    "        if line_k:match('^%s*#%+begin_example') then\n"
    "          while k <= n and not (base_lines[k] or ''):match('^%s*#%+end_example') do k = k + 1 end\n"
    "        elseif line_k:match('^%s*:') or line_k:match('^%s*|') or line_k:match('^%s*%[%[file:') then\n"
    "          while k <= n and ((base_lines[k] or ''):match('^%s*:') or (base_lines[k] or ''):match('^%s*|') or (base_lines[k] or ''):match('^%s*%[%[file:')) do k = k + 1 end\n"
    "          k = k - 1\n"
    "        else\n"
    "          k = j\n"  // no example fence/table/file-link follows -- only the bare '#+RESULTS:' line itself belongs to the old block, matching mep.org_babel_insert_results' own "existing_end = existing_start" fallback
    "        end\n"
    "        i = k + 1\n"
    "      else\n"
    "        i = i + 1\n"
    "      end\n"
    "      for _, l in ipairs(formatted) do out[#out + 1] = l end\n"
    "    else\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  return out\n"
    "end\n";

// Org LaTeX/math-mode inline rendering, a sibling feature to
// kBuiltinOrgImages (defined earlier in this file): <leader>otl /
// mep.org_latex_toggle_ui detects $$..$$/\[..\]/\(..\)/$..$ math fragments
// and #+BEGIN_LaTeX/#+BEGIN_SRC latex "chunks", each rendered offline to a
// tightly-cropped PNG (tectonic -> pdftoppm, see flake.nix's devShell)
// and displayed through the same
// inline-texture pipeline kBuiltinOrgImages uses (GetOrLoadOrgInlineImageTexture,
// main.cpp), via a parallel, independently-toggled registry
// (mep.buf_set_latex_row -> Buffer::org_latex_rows) so the two previews
// can be on independently. Only *whole-line* fragments are recognized --
// math mixed into a prose line ("the value $x$ matters here") stays plain
// text: mep's row renderer (DrawLineFast, main.cpp) draws one row as one
// run of text with no mechanism to splice a texture into the middle of
// it, the same "whole row only" constraint org images live under (see
// kOrgInlineImageSlots' own comment). A multi-line fragment's interior/
// closing raw-source lines are hidden behind a 'latex'-provider closed
// Fold, the same collapse machinery org/markdown headings use for their
// own folds (mep.fold_create/mep.fold_clear_provider) -- rebuilt
// wholesale on every scan, so a manually-opened "peek at raw source"
// fold doesn't survive the next debounced rescan, matching how
// mep.org_fold_all/markdown's own fence folding already behave.
const char *kBuiltinOrgLatex =
    "local function mep_org_latex_trim(s) return s:match('^%s*(.-)%s*$') end\n"
    "\n"
    "local function mep_org_latex_wrapped(s, open, close)\n"
    "  return #s > #open + #close and s:sub(1, #open) == open and s:sub(-#close) == close\n"
    "end\n"
    "\n"
    "-- Finds every $..$/\\(..\\)/\\[..\\]/$$..$$ fragment embedded *within* a line\n"
    "-- that isn't wholly consumed by one (mep.org_latex_scan's own whole-line\n"
    "-- forms already handle that case) -- \"the value $x^2$ matters here\".\n"
    "-- Returns a list of {col_start, col_end, body}, 1-indexed/exclusive-end,\n"
    "-- same convention as mep.deco_add's opts table.\n"
    "-- Bare `$` is the only ambiguous delimiter (currency: \"$5\", \"$5-$10\") --\n"
    "-- applies Pandoc's own tex_math_dollars heuristic: the char right after\n"
    "-- an opening $ and right before a closing $ must both be non-space, and\n"
    "-- the char right after a closing $ must not be a digit (rules out\n"
    "-- \"$5-$10\" and \"$20,$30\", which the naive \"next $ wins\" scan wouldn't).\n"
    "-- \\(..\\)/\\[..\\]/$$..$$ have unambiguous 2-char delimiters, no such\n"
    "-- heuristic needed there.\n"
    "local function mep_org_latex_scan_inline(line)\n"
    "  local spans = {}\n"
    "  local i, n = 1, #line\n"
    "  while i <= n do\n"
    "    local two = line:sub(i, i + 1)\n"
    "    local body, span_end\n"
    "    if two == '\\\\[' then\n"
    "      local s = line:find('\\\\]', i + 2, true)\n"
    "      if s then body, span_end = line:sub(i, s + 1), s + 1 end\n"
    "    elseif two == '$$' then\n"
    "      local s = line:find('$$', i + 2, true)\n"
    "      if s then body, span_end = line:sub(i, s + 1), s + 1 end\n"
    "    elseif two == '\\\\(' then\n"
    "      local s = line:find('\\\\)', i + 2, true)\n"
    "      if s then body, span_end = line:sub(i, s + 1), s + 1 end\n"
    "    elseif line:sub(i, i) == '$' then\n"
    "      local next_char = line:sub(i + 1, i + 1)\n"
    "      if next_char ~= '' and next_char ~= ' ' and next_char ~= '$' then\n"
    "        local search_from = i + 1\n"
    "        while true do\n"
    "          local s = line:find('%$', search_from, false)\n"
    "          if not s then break end\n"
    "          local prev_char = line:sub(s - 1, s - 1)\n"
    "          local after_char = line:sub(s + 1, s + 1)\n"
    "          if prev_char ~= ' ' and not after_char:match('%d') then\n"
    "            body, span_end = line:sub(i, s), s\n"
    "            break\n"
    "          end\n"
    "          search_from = s + 1\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "    if body and #body > 2 then\n"
    "      spans[#spans + 1] = {col_start = i, col_end = span_end + 1, body = body}\n"
    "      i = span_end + 1\n"
    "    else\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  return spans\n"
    "end\n"
    "\n"
    "local MEP_ORG_LATEX_PREAMBLE = table.concat({\n"
    "  '\\\\documentclass[11pt,preview,border=1pt,varwidth]{standalone}',\n"
    "  '\\\\usepackage{amsmath}',\n"
    "  '\\\\usepackage{amssymb}',\n"
    "  '\\\\begin{document}',\n"
    "}, '\\n')\n"
    "\n"
    "local function mep_org_latex_hash(s)\n"
    "  local h = 2166136261\n"
    "  for i = 1, #s do\n"
    "    h = ((h ~ s:byte(i)) * 16777619) & 0xffffffff\n"
    "  end\n"
    "  return string.format('%08x', h)\n"
    "end\n"
    "\n"
    "local function mep_org_latex_cache_dir()\n"
    "  local dir = (os.getenv('HOME') or '/tmp') .. '/.cache/mep/latex'\n"
    "  os.execute('mkdir -p ' .. dir)\n"
    "  return dir\n"
    "end\n"
    "local mep_org_latex_inflight = {}\n"
    "\n"
    "function mep_org_latex_render(tex_body, on_done)\n"
    "  local dpi = math.floor(72 * mep.font_size() / 11 + 0.5)\n"
    "  local key = mep_org_latex_hash(tex_body .. '|' .. dpi)\n"
    "  local dir = mep_org_latex_cache_dir()\n"
    "  local png_path = dir .. '/' .. key .. '.png'\n"
    "  if mep_org_babel_file_exists(png_path) then\n"
    "    on_done(png_path)\n"
    "    return\n"
    "  end\n"
    "  if mep_org_latex_inflight[key] then\n"
    "    table.insert(mep_org_latex_inflight[key], on_done)\n"
    "    return\n"
    "  end\n"
    "  mep_org_latex_inflight[key] = {on_done}\n"
    "  local function finish(result, err)\n"
    "    local waiters = mep_org_latex_inflight[key] or {}\n"
    "    mep_org_latex_inflight[key] = nil\n"
    "    for _, cb in ipairs(waiters) do cb(result, err) end\n"
    "  end\n"
    "  if not mep_org_babel_has_exe('tectonic') then\n"
    "    finish(nil, \"tectonic not found on PATH (see flake.nix's devShell)\")\n"
    "    return\n"
    "  end\n"
    "  local tex_path = dir .. '/' .. key .. '.tex'\n"
    "  local pdf_path = dir .. '/' .. key .. '.pdf'\n"
    "  local f = io.open(tex_path, 'w')\n"
    "  if not f then\n"
    "    finish(nil, 'could not write ' .. tex_path)\n"
    "    return\n"
    "  end\n"
    "  f:write(MEP_ORG_LATEX_PREAMBLE .. '\\n' .. tex_body .. '\\n\\\\end{document}\\n')\n"
    "  f:close()\n"
    "  local compile_err = {}\n"
    "  mep.job_start({'tectonic', '-X', 'compile', tex_path, '--outfmt', 'pdf'}, {\n"
    "    on_stderr = function(line) compile_err[#compile_err + 1] = line end,\n"
    "    on_exit = function(code)\n"
    "      os.remove(tex_path)\n"
    "      if code ~= 0 or not mep_org_babel_file_exists(pdf_path) then\n"
    "        finish(nil, mep_org_babel_first_error_line(compile_err) or 'tectonic compile failed')\n"
    "        return\n"
    "      end\n"
    "      if not mep_org_babel_has_exe('pdftoppm') then\n"
    "        os.remove(pdf_path)\n"
    "        finish(nil, \"pdftoppm not found on PATH (see flake.nix's devShell)\")\n"
    "        return\n"
    "      end\n"
    "      local raster_err = {}\n"
    "      mep.job_start({'pdftoppm', '-png', '-r', tostring(dpi), '-singlefile', pdf_path, dir .. '/' .. key}, {\n"
    "        on_stderr = function(line) raster_err[#raster_err + 1] = line end,\n"
    "        on_exit = function(code2)\n"
    "          os.remove(pdf_path)\n"
    "          if code2 == 0 and mep_org_babel_file_exists(png_path) then\n"
    "            finish(png_path)\n"
    "          else\n"
    "            finish(nil, mep_org_babel_first_error_line(raster_err) or 'pdftoppm failed')\n"
    "          end\n"
    "        end,\n"
    "      })\n"
    "    end,\n"
    "  })\n"
    "end\n"
    "\n"
    "local function mep_org_latex_register(start_row, end_row, tex_body)\n"
    "  mep_org_latex_render(tex_body, function(png_path, err)\n"
    "    if not png_path then\n"
    "      if err then mep.notify('LaTeX: ' .. err, 'warn') end\n"
    "      return\n"
    "    end\n"
    "    local w, h = mep.image_size(png_path)\n"
    "    if not w then return end\n"
    "    local line_height = math.floor(mep.font_size()) + 6\n"
    "    local slots = math.max(1, math.ceil(h / line_height))\n"
    "    mep.buf_set_latex_row(start_row, png_path, slots, end_row)\n"
    "  end)\n"
    "end\n"
    "\n"
    "-- Same idea as mep_org_latex_register, but for one inline span rather than\n"
    "-- a whole row -- no slot math (an inline fragment is always drawn to fit\n"
    "-- within one line, see the C++ draw-time scaling, main.cpp).\n"
    "local function mep_org_latex_register_inline(row, col_start, col_end, tex_body)\n"
    "  mep_org_latex_render(tex_body, function(png_path, err)\n"
    "    if not png_path then\n"
    "      if err then mep.notify('LaTeX: ' .. err, 'warn') end\n"
    "      return\n"
    "    end\n"
    "    mep.buf_add_latex_inline(row, col_start, col_end, png_path)\n"
    "  end)\n"
    "end\n"
    "\n"
    "function mep.org_latex_scan()\n"
    "  mep.buf_clear_latex_rows()\n"
    "  mep.buf_clear_latex_inline()\n"
    "  mep.fold_clear_provider('latex')\n"
    "  if not mep.org_latex_visible() then return end\n"
    "  if mep_lsp_filetype(mep.filename()) ~= 'org' then return end\n"
    "  local n = mep.line_count()\n"
    "  local i = 1\n"
    "  while i <= n do\n"
    "    local line = mep.get_line(i)\n"
    "    local trimmed = mep_org_latex_trim(line)\n"
    "    local body, end_row\n"
    "\n"
    "    if line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ll][Aa][Tt][Ee][Xx]%s*$') then\n"
    "      local lines, j = {}, i + 1\n"
    "      while j <= n and not mep.get_line(j):match('^%s*#%+[Ee][Nn][Dd]_[Ll][Aa][Tt][Ee][Xx]') do\n"
    "        lines[#lines + 1] = mep.get_line(j)\n"
    "        j = j + 1\n"
    "      end\n"
    "      if j <= n then body, end_row = table.concat(lines, '\\n'), j end\n"
    "    elseif line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+[Ll][Aa][Tt][Ee][Xx]') then\n"
    "      local lines, j = {}, i + 1\n"
    "      while j <= n and not mep.get_line(j):match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') do\n"
    "        lines[#lines + 1] = mep.get_line(j)\n"
    "        j = j + 1\n"
    "      end\n"
    "      if j <= n then body, end_row = table.concat(lines, '\\n'), j end\n"
    "    elseif mep_org_latex_wrapped(trimmed, '\\\\[', '\\\\]') then\n"
    "      body, end_row = trimmed, i\n"
    "    elseif trimmed == '\\\\[' then\n"
    "      local lines, j = {}, i + 1\n"
    "      while j <= n and mep_org_latex_trim(mep.get_line(j)) ~= '\\\\]' do\n"
    "        lines[#lines + 1] = mep.get_line(j)\n"
    "        j = j + 1\n"
    "      end\n"
    "      if j <= n then body, end_row = '\\\\[' .. table.concat(lines, '\\n') .. '\\\\]', j end\n"
    "    elseif mep_org_latex_wrapped(trimmed, '$$', '$$') then\n"
    "      body, end_row = trimmed, i\n"
    "    elseif trimmed == '$$' then\n"
    "      local lines, j = {}, i + 1\n"
    "      while j <= n and mep_org_latex_trim(mep.get_line(j)) ~= '$$' do\n"
    "        lines[#lines + 1] = mep.get_line(j)\n"
    "        j = j + 1\n"
    "      end\n"
    "      if j <= n then body, end_row = '$$' .. table.concat(lines, '\\n') .. '$$', j end\n"
    "    elseif mep_org_latex_wrapped(trimmed, '\\\\(', '\\\\)') then\n"
    "      body, end_row = trimmed, i\n"
    "    elseif #trimmed > 2 and trimmed:sub(1, 1) == '$' and trimmed:sub(-1) == '$'\n"
    "        and trimmed:sub(2, 2) ~= '$' and trimmed:sub(-2, -2) ~= '$' then\n"
    "      body, end_row = trimmed, i\n"
    "    end\n"
    "\n"
    "    if body and body ~= '' then\n"
    "      mep_org_latex_register(i, end_row, body)\n"
    "      i = end_row + 1\n"
    "    else\n"
    "      -- No whole-line form matched -- this line isn't *entirely* a\n"
    "      -- fragment, but may still have one or more embedded inline ones\n"
    "      -- (\"the value $x^2$ matters here\").\n"
    "      for _, span in ipairs(mep_org_latex_scan_inline(line)) do\n"
    "        mep_org_latex_register_inline(i, span.col_start, span.col_end, span.body)\n"
    "      end\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "end\n"
    "\n"
    "mep.command('MepOrgLatexScan', mep.org_latex_scan)\n"
    "\n"
    "function mep.org_latex_toggle_ui()\n"
    "  local visible = mep.org_latex_toggle()\n"
    "  mep.notify('Org LaTeX preview: ' .. (visible and 'on' or 'off'))\n"
    "  mep.org_latex_scan()\n"
    "end\n"
    "mep.command('MepOrgLatexToggle', mep.org_latex_toggle_ui)\n"
    "mep.leader_map('otl', 'Org: toggle LaTeX/math preview', mep.org_latex_toggle_ui)\n"
    "\n"
    "mep.on_buffer_changed(function()\n"
    "  if mep_lsp_filetype(mep.filename()) == 'org' then mep.org_latex_scan() end\n"
    "end)\n"
    "local mep_org_latex_last_file = nil\n"
    "mep.on_frame(function()\n"
    "  local fname = mep.filename()\n"
    "  if fname ~= mep_org_latex_last_file then\n"
    "    mep_org_latex_last_file = fname\n"
    "    if mep_lsp_filetype(fname) == 'org' then mep.org_latex_scan() end\n"
    "  end\n"
    "end)\n";

// Org-mode G: export (Phase 35). Flat single-pass line walk (no
// intermediate AST) driving three backends off one shared inline-mark
// tokenizer -- src blocks always render literally (never executed
// during export), matching the plan's own "ship without eval first"
// guidance.
const char *kBuiltinOrgExport =
    // A babel `:file` result inserts a plain org file link
    // ([[file:plot.png]], mep_org_babel_result_lines above) the same way
    // a hand-written [[file:...]] figure link does -- html_link strips
    // the 'file:' scheme (org's own link syntax, meaningless as a
    // literal HTML src/href) and auto-embeds an image-extension target
    // as <img> instead of a clickable <a>, matching real org-mode's own
    // ox-html behavior: a plot a code block just produced should show up
    // as a picture in the exported page, not a link to click through to.
    "local function mep_org_html_link_target(u) return (u:gsub('^file:', '')) end\n"
    "local function mep_org_is_image_link(u)\n"
    "  local ext = u:match('%.([%a]+)$')\n"
    "  ext = ext and ext:lower()\n"
    "  return ext == 'png' or ext == 'jpg' or ext == 'jpeg' or ext == 'gif' or ext == 'bmp' or ext == 'svg'\n"
    "end\n"
    // underline/strike: NVIM_PARITY_PLAN.md Phase 35 gap -- org's own
    // _underline_/+strikethrough+ syntax had no exporter marks at all
    // before this, so they fell through the generic line-copy branch and
    // reached the output as literal underscores/pluses. Markdown has no
    // native underline (raw <u> passthrough, same as most CommonMark
    // renderers accept for constructs the spec doesn't cover), but GFM's
    // own '~~text~~' *is* real strikethrough syntax, unlike bold/italic's
    // plain-Markdown equivalents above.
    "mep.org_export_marks = {\n"
    "  html = {bold_open = '<b>', bold_close = '</b>', italic_open = '<i>', italic_close = '</i>',\n"
    "    code_open = '<code>', code_close = '</code>',\n"
    "    underline_open = '<u>', underline_close = '</u>', strike_open = '<del>', strike_close = '</del>',\n"
    "    link = function(u, d)\n"
    "      local target = mep_org_html_link_target(u)\n"
    "      if mep_org_is_image_link(target) then\n"
    "        return '<img src=\"' .. target .. '\" alt=\"' .. mep_org_html_link_target(d) .. '\">'\n"
    "      end\n"
    "      return '<a href=\"' .. target .. '\">' .. d .. '</a>'\n"
    "    end},\n"
    "  markdown = {bold_open = '**', bold_close = '**', italic_open = '_', italic_close = '_',\n"
    "    code_open = '`', code_close = '`',\n"
    "    underline_open = '<u>', underline_close = '</u>', strike_open = '~~', strike_close = '~~',\n"
    "    link = function(u, d) return '[' .. d .. '](' .. u .. ')' end},\n"
    "  ascii = {bold_open = '', bold_close = '', italic_open = '', italic_close = '',\n"
    "    code_open = '', code_close = '', underline_open = '', underline_close = '', strike_open = '', strike_close = '',\n"
    "    link = function(u, d) return d .. ' <' .. u .. '>' end},\n"
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
    "  text = text:gsub('_([^_\\n]+)_', function(t) return stash_out(marks.underline_open .. t .. marks.underline_close) end)\n"
    "  text = text:gsub('%+([^%+\\n]+)%+', function(t) return stash_out(marks.strike_open .. t .. marks.strike_close) end)\n"
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
    // Same capture -> highlight-group resolution as kBuiltinSyntax's own
    // (local, chunk-private) mep_ts_resolve_hl -- duplicated rather than
    // shared since Lua chunks loaded via separate DoString calls don't
    // share locals, same reasoning mep_syntax_highlight_org_src_blocks'
    // own header gives for its light rescan duplication. mep.ts_capture_hl
    // itself (kBuiltinSyntax) is a real `mep.*` global, so the *table* is
    // shared -- only this lookup wrapper needs its own copy.
    "local function mep_org_export_resolve_hl(capture)\n"
    "  local hl = mep.ts_capture_hl[capture]\n"
    "  if hl then return hl end\n"
    "  local base = capture:match('^([^.]+)')\n"
    "  return base and mep.ts_capture_hl[base]\n"
    "end\n"
    // Renders one already-HTML-escaped-as-needed source line, wrapping any
    // Treesitter-captured ranges (mep.ts_captures' own convention: 1-
    // indexed col_start, exclusive col_end) in <span class=\"tok-X\">, X
    // being the same highlight-group name (Purple/Green/Cyan/...)
    // mep.ts_capture_hl already maps captures to -- doc_export.cpp's own
    // WalkLatexNode (PDF export) recognizes these exact class names too,
    // via mepTok<X> LaTeX colors sharing the same X suffix, so HTML and
    // PDF export end up with identical highlighting from one computation.
    // Overlapping/nested captures are resolved by keeping whichever one
    // reaches a position first and skipping any later span that starts
    // before text already emitted -- reproducing a real editor's
    // decoration-priority rules isn't worth it for a one-shot export.
    "local function mep_org_html_highlight_line(line, spans)\n"
    "  if not spans or #spans == 0 then return mep_org_html_escape(line) end\n"
    "  table.sort(spans, function(a, b) return a.col_start < b.col_start end)\n"
    "  local out, pos = {}, 1\n"
    "  for _, sp in ipairs(spans) do\n"
    "    if sp.col_start >= pos then\n"
    "      if sp.col_start > pos then out[#out + 1] = mep_org_html_escape(line:sub(pos, sp.col_start - 1)) end\n"
    "      local hl = mep_org_export_resolve_hl(sp.capture)\n"
    "      local text = mep_org_html_escape(line:sub(sp.col_start, sp.col_end - 1))\n"
    // Lowercased in the class value itself, not just the CSS rule below --
    // html_doc.cpp's CollectStyleRules lowercases every selector it parses
    // (tag names are case-insensitive in real HTML, so that's correct for
    // those, but it applies to .class/#id selectors too, which real CSS
    // keeps case-sensitive) while DomNode::attrs (ParseHtml) keeps
    // attribute *values* verbatim -- mep.ts_capture_hl's own highlight-
    // group names (Purple/Green/Comment/...) are capitalized, so a class
    // built from one directly would never match its own lowercased
    // selector in mep's own in-pane viewer. RenderOrgCodeBlockLatex
    // (doc_export.cpp) reads this same lowercase suffix back out of the
    // class attribute, and its own mepTok<X> LaTeX colors are named to
    // match (lowercase), so both backends stay in sync off this one
    // lowercasing point.
    "      out[#out + 1] = hl and ('<span class=\"tok-' .. hl:lower() .. '\">' .. text .. '</span>') or text\n"
    "      pos = sp.col_end\n"
    "    end\n"
    "  end\n"
    "  if pos <= #line then out[#out + 1] = mep_org_html_escape(line:sub(pos)) end\n"
    "  return table.concat(out)\n"
    "end\n"
    // Full HTML rendering for one #+begin_src/#+end_src block: a bordered,
    // headered wrapper (default CSS lives in mep_org_html_wrap_document,
    // below) around a <pre><code> whose body is Treesitter-highlighted the
    // same way mep_syntax_highlight_org_src_blocks (kBuiltinSyntax)
    // already highlights it live in the editor -- mep_org_babel_lang_ts_ft
    // (also kBuiltinSyntax) bridges the babel language tag on the
    // #+begin_src line to a Treesitter filetype. A tag with no grammar (or
    // no entry in that table) just renders unhighlighted, same fallback
    // every other Treesitter consumer in this codebase already has.
    //
    // Markup shape is deliberately constrained to what mep's own in-pane
    // HTML viewer (html_doc.cpp/main.cpp's renderer, not just real
    // browsers) can actually lay out -- that renderer's CSS selector
    // matcher (html_doc.cpp's ApplyMatchingRules) only ever matches a bare
    // ".class", "#id", or "tag" selector, never a descendant combinator
    // like ".org-code-block pre", so a rule written that way would just
    // silently never match anything there. <pre>/<code> get their own
    // dedicated classes (org-code-pre/org-code-body) instead of relying on
    // overriding the generic pre{}/code{} tag rules from within
    // .org-code-block -- those tag rules still apply to <pre>/<code>
    // anywhere else in the document (an ordinary ~verbatim~ span, say),
    // but class beats tag in this engine's own stated specificity order,
    // so the dedicated classes win inside a code block. The language tag
    // moves to a data-lang attribute rather than living in <code>'s own
    // class (doc_export.cpp's RenderOrgCodeBlockLatex reads it from
    // there) -- a per-block-varying class value could never be targeted
    // by a single CSS rule anyway. The copy control is a <span> (inline by
    // that renderer's own TagDefaults), not a <button> (defaults to block,
    // which would force it onto its own line below the language label,
    // breaking the one-line header) -- a literal space between the two
    // spans keeps them readably apart even without flexbox (unsupported
    // there; still declared in the CSS below as a real browser's own
    // progressive enhancement). mepCopyCode itself (onclick, defined in
    // mep_org_html_wrap_document's own <script>) is inert in mep's own
    // renderer -- js_engine.h's own header lists its entire DOM/BOM
    // binding surface, and it has no click/event dispatch of any kind --
    // but that's a silent no-op there, not a rendering break, so the
    // control is left in place for the real-browser case this was built
    // for.
    "local function mep_org_html_code_block(lang, body)\n"
    "  local embed_ft = lang ~= '' and mep_org_babel_lang_ts_ft[lang:lower()]\n"
    "  local spans_by_row = {}\n"
    "  if embed_ft and #body > 0 then\n"
    "    local captures = mep.ts_captures(embed_ft, table.concat(body, '\\n'))\n"
    "    if captures then\n"
    "      for _, cap in ipairs(captures) do\n"
    "        spans_by_row[cap.row] = spans_by_row[cap.row] or {}\n"
    "        table.insert(spans_by_row[cap.row], cap)\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  local rendered = {}\n"
    "  for idx, l in ipairs(body) do rendered[idx] = mep_org_html_highlight_line(l, spans_by_row[idx]) end\n"
    "  local lang_label = mep_org_html_escape(lang ~= '' and lang or 'text')\n"
    "  return '<div class=\"org-code-block\">'\n"
    "    .. '<div class=\"org-code-header\"><span class=\"org-code-lang\">' .. lang_label .. '</span> '\n"
    "    .. '<span class=\"org-code-copy\" role=\"button\" tabindex=\"0\" onclick=\"mepCopyCode(this)\">Copy</span></div>'\n"
    "    .. '<pre class=\"org-code-pre\"><code class=\"org-code-body\" data-lang=\"' .. mep_org_html_escape(lang) .. '\">'\n"
    "    .. table.concat(rendered, '\\n') .. '</code></pre></div>'\n"
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
    // Array-input counterpart of mep.org_resolve_includes below (its own
    // body is now just this, snapshotting the live buffer first) --
    // mep_org_export_prepare (further down) also calls this directly
    // against a babel-results-spliced scratch array, so #+INCLUDE:
    // resolution still applies to the code-execution export path exactly
    // as it already did to the plain one.
    "function mep_org_resolve_includes_lines(lines, base_dir)\n"
    "  local budget = {remaining = 20000}\n"
    "  local seen = {}\n"
    "  local out = {}\n"
    "  for i = 1, #lines do\n"
    "    local sub = mep_org_resolve_include_line(lines[i], base_dir, 1, seen, budget)\n"
    "    if sub then\n"
    "      for _, l in ipairs(sub) do out[#out + 1] = l end\n"
    "    else\n"
    "      out[#out + 1] = lines[i]\n"
    "    end\n"
    "  end\n"
    "  return out\n"
    "end\n"
    "function mep.org_resolve_includes()\n"
    "  local base_dir = (mep.filename() or ''):match('^(.*)/[^/]*$') or '.'\n"
    "  local lines = {}\n"
    "  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) end\n"
    "  return mep_org_resolve_includes_lines(lines, base_dir)\n"
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
    // `lines_override`, when given, is used verbatim in place of
    // mep.org_resolve_includes()'s own live-buffer read --
    // mep_org_export_prepare (further down) passes an already
    // include-resolved, macro-expanded, babel-results-spliced scratch
    // array here so code-block output can be substituted into the
    // export without ever touching the real buffer. Macro expansion
    // below still runs either way (a second, idempotent pass over an
    // already-expanded array is a harmless no-op scan, not a
    // correctness risk) so a caller passing a raw lines array (not run
    // through mep_org_export_prepare) still gets correct behavior.
    "function mep.org_export(format, lines_override)\n"
    "  local marks = mep.org_export_marks[format]\n"
    "  local lines = lines_override or mep.org_resolve_includes()\n"
    "  local macros = mep_org_collect_macros(function(i) return lines[i] end, #lines)\n"
    "  lines = (function()\n"
    "    local out = {}\n"
    "    for i, l in ipairs(lines) do out[i] = mep_org_expand_macro_line(l, macros) end\n"
    "    return out\n"
    "  end)()\n"
    "  local out, i, n = {}, 1, #lines\n"
    // HTML needs a real <ul>/<ol> wrapper around a run of consecutive
    // <li>s for the DOM-consuming backends (doc_export.h's ExportHtmlTo
    // Latex/ExportHtmlToOdt, and this file's own ExtractMathSpans-fed
    // renderer) to recognize it as a list at all -- a bare, unwrapped
    // <li> parses as a generic block element instead (html_doc.cpp's
    // TagDefaults has no special list-context handling for one seen
    // outside a <ul>/<ol>), silently losing the bullet/numbering.
    // `list_open` tracks which wrapper (if any) is currently open;
    // close_list() is called at the start of every OTHER branch so a
    // heading/blank line/etc. immediately closes a list run in progress.
    "  local list_open = nil\n"
    "  local function close_list()\n"
    "    if format == 'html' and list_open then\n"
    "      out[#out + 1] = '</' .. list_open .. '>'\n"
    "      list_open = nil\n"
    "    end\n"
    "  end\n"
    "  while i <= n do\n"
    "    local line = lines[i]\n"
    "    local h = mep_org_parse_headline(line)\n"
    "    if h then\n"
    "      close_list()\n"
    "      if h.tags and h.tags:find('noexport', 1, true) then\n"
    "        i = mep_org_subtree_end_lines(lines, i)\n"
    "      else\n"
    "        local title = mep_org_inline_convert(format == 'html' and mep_org_html_escape(h.title) or h.title, marks)\n"
    "        out[#out + 1] = mep_org_export_heading(format, h.level, title)\n"
    "        i = i + 1\n"
    "      end\n"
    "    elseif line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]') then\n"
    "      close_list()\n"
    "      local lang = line:match('^%s*#%+[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]%s+(%S+)') or ''\n"
    "      i = i + 1\n"
    "      local body = {}\n"
    "      while i <= n and not lines[i]:match('^%s*#%+[Ee][Nn][Dd]_[Ss][Rr][Cc]') do\n"
    "        body[#body + 1] = lines[i]\n"
    "        i = i + 1\n"
    "      end\n"
    "      if format == 'html' then\n"
    "        out[#out + 1] = mep_org_html_code_block(lang, body)\n"
    "      elseif format == 'markdown' then\n"
    "        out[#out + 1] = '```' .. lang\n"
    "        for _, l in ipairs(body) do out[#out + 1] = l end\n"
    "        out[#out + 1] = '```'\n"
    "      else\n"
    "        out[#out + 1] = '----'\n"
    "        for _, l in ipairs(body) do out[#out + 1] = l end\n"
    "        out[#out + 1] = '----'\n"
    "      end\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*:PROPERTIES:%s*$') then\n"
    "      while i <= n and not lines[i]:match('^%s*:END:%s*$') do i = i + 1 end\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*SCHEDULED:') or line:match('^%s*DEADLINE:') or line:match('^%s*#%+') then\n"
    "      i = i + 1\n"
    "    elseif line:match('^%s*$') then\n"
    "      close_list()\n"
    "      out[#out + 1] = ''\n"
    "      i = i + 1\n"
    // Org's own pipe-table syntax ("| a | b |", a "|---+---|" separator
    // row marking the header/body boundary) happens to already BE valid
    // GFM markdown table syntax verbatim -- markdown/ascii both fall
    // through to the generic line-copy branch below unchanged, which is
    // already correct for them. HTML needs a real <table> built here
    // though: without it, this row would just fall through as literal
    // '|'-delimited text (the pre-existing gap, before this change --
    // org tables were never HTML-ized at all), and more importantly
    // doc_export.h's DOM-consuming ExportHtmlToLatex/ExportHtmlToOdt
    // (main.cpp's CollectTableRows/TableMaxCols) have nothing to walk
    // without a real <table>/<tr>/<td> in the HTML this produces.
    "    elseif format == 'html' and line:match('^%s*|.-|%s*$') then\n"
    "      close_list()\n"
    "      out[#out + 1] = '<table>'\n"
    "      local header_done = false\n"
    "      while i <= n and lines[i]:match('^%s*|.-|%s*$') do\n"
    "        local row = lines[i]\n"
    "        if row:match('^%s*|[%s%-%+]*|?%s*$') then\n"
    "          header_done = true\n"
    "        else\n"
    "          local trimmed = row:match('^%s*|(.-)|%s*$') or ''\n"
    "          local cells = {}\n"
    "          for cell in (trimmed .. '|'):gmatch('(.-)|') do cells[#cells + 1] = cell:match('^%s*(.-)%s*$') end\n"
    "          local tag = header_done and 'td' or 'th'\n"
    "          local cells_html = {}\n"
    "          for _, c in ipairs(cells) do\n"
    "            cells_html[#cells_html + 1] = '<' .. tag .. '>' .. mep_org_inline_convert(mep_org_html_escape(c), marks) .. '</' .. tag .. '>'\n"
    "          end\n"
    "          out[#out + 1] = '<tr>' .. table.concat(cells_html) .. '</tr>'\n"
    "        end\n"
    "        i = i + 1\n"
    "      end\n"
    "      out[#out + 1] = '</table>'\n"
    "    else\n"
    "      local converted = mep_org_inline_convert(format == 'html' and mep_org_html_escape(line) or line, marks)\n"
    "      local is_bullet = line:match('^%s*[%-%*%+]%s')\n"
    "      local is_ordered = line:match('^%s*%d+[%.%)]%s')\n"
    "      if format == 'html' and (is_bullet or is_ordered) then\n"
    "        local want = is_bullet and 'ul' or 'ol'\n"
    "        if list_open and list_open ~= want then close_list() end\n"
    "        if not list_open then\n"
    "          out[#out + 1] = '<' .. want .. '>'\n"
    "          list_open = want\n"
    "        end\n"
    "        local item = is_bullet and converted:gsub('^%s*[%-%*%+]%s*', '') or converted:gsub('^%s*%d+[%.%)]%s*', '')\n"
    "        out[#out + 1] = '<li>' .. item .. '</li>'\n"
    "      else\n"
    "        close_list()\n"
    "        out[#out + 1] = converted\n"
    "      end\n"
    "      i = i + 1\n"
    "    end\n"
    "  end\n"
    "  close_list()\n"
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
    "  return out_file\n"
    "end\n"
    // #+TITLE:/#+AUTHOR:/#+DATE: -- used by all three "real document"
    // backends (HTML's own <title>/skeleton, and passed straight through
    // to doc_export.h's ExportHtmlToLatex/ExportHtmlToOdt for their own
    // \title{}/\author{} and dc:title/dc:creator). Scans `lines` rather
    // than the live buffer so it sees the same already-#+INCLUDE:-
    // resolved view mep_org_export_prepare hands to mep.org_export
    // itself -- a title set via an included file resolves the same way
    // it already did before this existed.
    "function mep_org_extract_meta(lines)\n"
    "  local meta = {}\n"
    "  for _, l in ipairs(lines) do\n"
    // Org keywords are case-insensitive in real org-mode (#+title:/
    // #+TITLE:/#+Title: all equivalent -- Emacs org-roam's own default
    // capture template writes the lowercase form), so each keyword below
    // is matched letter-by-letter as a [Xx] class rather than a literal
    // uppercase run -- a plain line:lower() would work for detecting the
    // keyword but would also lowercase the *captured* title/author/date
    // text itself.
    "    local t = l:match('^%s*#%+[Tt][Ii][Tt][Ll][Ee]:%s*(.*)$')\n"
    "    if t then meta.title = t end\n"
    "    local a = l:match('^%s*#%+[Aa][Uu][Tt][Hh][Oo][Rr]:%s*(.*)$')\n"
    "    if a then meta.author = a end\n"
    "    local d = l:match('^%s*#%+[Dd][Aa][Tt][Ee]:%s*(.*)$')\n"
    "    if d then meta.date = d end\n"
    "  end\n"
    "  return meta\n"
    "end\n"
    // mep.org_export('html', ...) itself still returns a bare body
    // fragment (unchanged -- other callers, e.g. a future embed-into-
    // another-page use, still want just that) -- this wraps it into a
    // real, standalone .html file (doctype/charset/<title>/a small
    // readable default stylesheet) for MepOrgExportHtml's own output,
    // the same way a real org-mode HTML export always produces a
    // complete document, never a bare fragment.
    // Code-block CSS below (.org-code-block and friends) matches the
    // ambient page rather than dropping in a dark editor-pane-style box --
    // same white background as body{}, a light gray header bar
    // (.org-code-header) as the only real color break, and a colored
    // left border as the block's own accent/boundary marker.
    // .tok-<Name> classes share their exact names with mep.ts_capture_hl's
    // own highlight-group values (mep_org_html_highlight_line, above) --
    // this is a fixed light-on-white palette tuned for that background
    // (not a straight reuse of the colors a dark-background version would
    // use, several of which -- pastel greens/yellows especially -- have
    // poor contrast against white), not a live readout of mep's own active
    // colorscheme (this file has no colorscheme state at all; it only ever
    // runs once, offline, at export time).
    //
    // Every rule here is a bare "tag"/".class" selector -- no descendant
    // combinators (".org-code-block pre"), no :hover-style pseudo-classes
    // doing load-bearing work, and no property this stylesheet actually
    // depends on being set *only* via "background:transparent"/
    // "color:inherit" tricks -- mep's own in-pane HTML viewer (not just a
    // real browser) has to render this same markup (see
    // mep_org_html_code_block's own comment for the specifics of that
    // renderer's own, much smaller, CSS support), and both of those don't
    // survive there: a descendant selector never matches anything in its
    // own selector engine, and "transparent"/"inherit" aren't real color
    // values its color parser recognizes, so a property set to either is
    // simply never applied at all rather than resolving to the fallback a
    // real browser would give it. .org-code-pre/.org-code-body (used by
    // mep_org_html_code_block instead of relying on the generic pre{}/
    // code{} rules below while inside a code block) spell out the same
    // background/foreground literally for exactly that reason -- that
    // renderer's own stated precedence rule (its own class beats tag)
    // still makes them win over the plain pre{}/code{} rules below, unlike
    // a descendant-selector override attempt. body{} explicitly sets a
    // white background, not just relying on an implicit page-white canvas
    // real browsers provide -- mep's own viewer paints its host pane's
    // (dark-themed) background behind an HTML doc with no explicit one,
    // which made every one of these light-palette colors (color:#222
    // included) nearly illegible against it before this line existed.
    "function mep_org_html_wrap_document(fragment, meta)\n"
    "  local title = meta.title and mep_org_html_escape(meta.title) or 'Untitled'\n"
    "  return '<!DOCTYPE html>\\n<html lang=\"en\">\\n<head>\\n<meta charset=\"utf-8\">\\n'\n"
    "    .. '<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\\n'\n"
    "    .. '<title>' .. title .. '</title>\\n'\n"
    "    .. '<style>body{max-width:48em;margin:2em auto;padding:0 1em;background:#fff;'\n"
    "    .. 'font-family:sans-serif;line-height:1.5;color:#222}'\n"
    "    .. 'pre{background:#f2f2f2;padding:0.6em;overflow:auto;border-radius:3px}'\n"
    "    .. 'code{background:#f2f2f2;padding:0.1em 0.3em;border-radius:3px}'\n"
    "    .. 'img{max-width:100%}'\n"
    "    .. 'h1,h2,h3{line-height:1.25}'\n"
    "    .. '.org-code-block{margin:1.2em 0;border-top:1px solid #d0d7de;border-right:1px solid #d0d7de;'\n"
    "    .. 'border-bottom:1px solid #d0d7de;border-left:4px solid #6b8afd;border-radius:4px;'\n"
    "    .. 'overflow:hidden;background:#fff;color:#24292e;font-size:0.95em}'\n"
    "    .. '.org-code-header{display:flex;justify-content:space-between;align-items:center;gap:0.6em;'\n"
    "    .. 'padding:0.35em 0.8em;background:#f6f8fa;border-bottom:1px solid #d0d7de;'\n"
    "    .. 'font-family:ui-monospace,Menlo,Consolas,monospace;font-size:0.8em;color:#57606a}'\n"
    "    .. '.org-code-lang{text-transform:lowercase;letter-spacing:0.02em}'\n"
    "    .. '.org-code-copy{background:#eaeef2;color:#24292e;border:none;border-radius:3px;'\n"
    "    .. 'padding:0.2em 0.7em;font-size:0.85em;cursor:pointer;font-family:inherit}'\n"
    "    .. '.org-code-copy:hover{background:#d0d7de}'\n"
    "    .. '.org-code-pre{margin:0;padding:0.8em 1em;background:#fff;'\n"
    "    .. 'border-radius:0;overflow:auto}'\n"
    "    .. '.org-code-body{background:#fff;padding:0;border-radius:0;color:#24292e;'\n"
    "    .. 'font-family:ui-monospace,Menlo,Consolas,monospace;font-size:0.9em;line-height:1.5}'\n"
    "    .. '.tok-comment{color:#6b7280;font-style:italic}.tok-green{color:#1a7f37}'\n"
    "    .. '.tok-cyan{color:#0b7285}.tok-purple{color:#8250df}.tok-blue{color:#0550ae}'\n"
    "    .. '.tok-orange{color:#953800}.tok-red{color:#cf222e}.tok-yellow{color:#9a6700}'\n"
    "    .. '</style>\\n'\n"
    // Clipboard write needs a real button-click event, which is why this
    // is a plain onclick handler (mep_org_html_code_block, above) rather
    // than an addEventListener pass over .org-code-copy after load --
    // either works, but onclick needs no DOMContentLoaded wiring at all.
    // navigator.clipboard is only available in a secure context (https,
    // or localhost) -- an exported .html opened directly via file:// (the
    // common case right after export) has neither, so the execCommand
    // fallback below is the path that actually fires for most users, not
    // a rarely-hit edge case.
    "    .. '<script>function mepCopyCode(btn){'\n"
    "    .. 'var block=btn.closest(\".org-code-block\");'\n"
    "    .. 'var code=block&&block.querySelector(\"code\");'\n"
    "    .. 'if(!code)return;'\n"
    "    .. 'var text=code.innerText;'\n"
    "    .. 'var restore=function(){setTimeout(function(){btn.textContent=\"Copy\";},1200);};'\n"
    "    .. 'btn.textContent=\"Copied!\";'\n"
    "    .. 'if(navigator.clipboard&&navigator.clipboard.writeText){'\n"
    "    .. 'navigator.clipboard.writeText(text).then(restore,restore);'\n"
    "    .. '}else{'\n"
    "    .. 'var ta=document.createElement(\"textarea\");'\n"
    "    .. 'ta.value=text;ta.style.position=\"fixed\";ta.style.opacity=\"0\";'\n"
    "    .. 'document.body.appendChild(ta);ta.select();'\n"
    "    .. 'try{document.execCommand(\"copy\");}catch(e){}'\n"
    "    .. 'document.body.removeChild(ta);restore();'\n"
    "    .. '}}</script>\\n</head>\\n<body>\\n'\n"
    "    .. (meta.title and ('<h1>' .. title .. '</h1>\\n') or '')\n"
    "    .. fragment .. '\\n</body>\\n</html>\\n'\n"
    "end\n"
    // Every MepOrgExport*/MepOrgCompile* command below funnels through
    // this: runs every code block in the buffer (mep.org_babel_run_for_
    // export, kBuiltinOrgBabel), then resolves #+INCLUDE:/#+MACRO: on
    // the resulting babel-results-spliced scratch array exactly the way
    // mep.org_resolve_includes always has for the live buffer, and hands
    // the fully-prepared lines array to `on_lines`. Code execution is
    // the *default* for every export backend (html/markdown/ascii/pdf/
    // odt alike, matching real org-mode's own org-export-use-babel
    // default) -- a block can still opt out per-block via :eval no (see
    // mep.org_babel_run_for_export's own header for the exact skip set).
    "function mep_org_export_prepare(on_lines)\n"
    "  mep.notify('Org export: running code blocks...')\n"
    "  mep.org_babel_run_for_export(function(spliced_lines)\n"
    "    local base_dir = (mep.filename() or ''):match('^(.*)/[^/]*$') or '.'\n"
    "    local resolved = mep_org_resolve_includes_lines(spliced_lines, base_dir)\n"
    "    local macros = mep_org_collect_macros(function(i) return resolved[i] end, #resolved)\n"
    "    local expanded = {}\n"
    "    for i, l in ipairs(resolved) do expanded[i] = mep_org_expand_macro_line(l, macros) end\n"
    "    on_lines(expanded)\n"
    "  end)\n"
    "end\n"
    // Named (not anonymous) so the same function reference can be passed
    // to both mep.command and mep.leader_map below -- mep.commands is
    // the command-palette *function*, not a name->handler lookup table,
    // so a leader binding can't route through a registered command name
    // the way it might in some other plugin ecosystems; every other
    // org leader binding in this codebase (mep.org_images_toggle_ui,
    // mep.org_latex_toggle_ui, ...) already follows this same named-
    // global-function pattern for exactly that reason.
    "function mep.org_export_html()\n"
    "  mep_org_export_prepare(function(lines)\n"
    "    mep_org_export_to_file(mep_org_html_wrap_document(mep.org_export('html', lines), mep_org_extract_meta(lines)), 'html')\n"
    "  end)\n"
    "end\n"
    "function mep.org_export_markdown()\n"
    "  mep_org_export_prepare(function(lines) mep_org_export_to_file(mep.org_export('markdown', lines), 'md') end)\n"
    "end\n"
    "function mep.org_export_ascii()\n"
    "  mep_org_export_prepare(function(lines) mep_org_export_to_file(mep.org_export('ascii', lines), 'txt') end)\n"
    "end\n"
    // PDF: org -> HTML (in-process) -> LaTeX (doc_export.h's
    // ExportHtmlToLatex, C++) -> a real .tex file next to the org file
    // -> tectonic (same devShell dependency + invocation shape
    // kBuiltinOrgLatex's own mep_org_latex_render already uses for
    // math-fragment previews, just compiling the *whole* document here
    // instead of one \documentclass{standalone} snippet, and keeping the
    // resulting PDF itself rather than rasterizing+discarding it). The
    // .tex file is deliberately left on disk afterward -- a legitimate
    // export artifact of its own, not just scratch.
    "function mep.org_export_pdf()\n"
    "  mep_org_export_prepare(function(lines)\n"
    "    local meta = mep_org_extract_meta(lines)\n"
    "    local html = mep.org_export('html', lines)\n"
    "    local base_dir = mep_lsp_abspath(mep.filename()):match('^(.*)/[^/]*$') or '.'\n"
    "    local latex = mep.doc_export_html_to_latex(html, meta.title or '', meta.author or '', base_dir)\n"
    "    local base = (mep.filename():gsub('%.org$', ''))\n"
    "    local tex_path = base .. '.tex'\n"
    "    local f = io.open(tex_path, 'w')\n"
    "    f:write(latex)\n"
    "    f:close()\n"
    "    mep.notify('Org export: compiling PDF (tectonic)...')\n"
    "    local tex_err = {}\n"
    "    mep.job_start({'tectonic', '-X', 'compile', tex_path, '--outfmt', 'pdf'}, {\n"
    "      cwd = base_dir,\n"
    "      on_stderr = function(line) tex_err[#tex_err + 1] = line end,\n"
    "      on_exit = function(code)\n"
    "        if code == 0 then\n"
    "          mep.notify('Exported to ' .. base .. '.pdf')\n"
    "        else\n"
    "          mep.notify('PDF export failed (tectonic exit ' .. code .. '): '\n"
    "            .. (tex_err[#tex_err] or 'see ' .. tex_path), 'error')\n"
    "        end\n"
    "      end,\n"
    "    })\n"
    "  end)\n"
    "end\n"
    // ODT: org -> HTML (in-process) -> a real .odt written directly by
    // doc_export.h's ExportHtmlToOdt (C++, zipped via miniz) -- no
    // external tool/subprocess needed for this backend, unlike PDF.
    "function mep.org_export_odt()\n"
    "  mep_org_export_prepare(function(lines)\n"
    "    local meta = mep_org_extract_meta(lines)\n"
    "    local html = mep.org_export('html', lines)\n"
    "    local base_dir = mep_lsp_abspath(mep.filename()):match('^(.*)/[^/]*$') or '.'\n"
    "    local base = (mep.filename():gsub('%.org$', ''))\n"
    "    local ok, err = mep.doc_export_html_to_odt(html, base .. '.odt', meta.title or '', meta.author or '', base_dir)\n"
    "    if ok then\n"
    "      mep.notify('Exported to ' .. base .. '.odt')\n"
    "    else\n"
    "      mep.notify('ODT export failed: ' .. (err or '?'), 'error')\n"
    "    end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgExportHtml', mep.org_export_html)\n"
    "mep.command('MepOrgExportMarkdown', mep.org_export_markdown)\n"
    "mep.command('MepOrgExportAscii', mep.org_export_ascii)\n"
    "mep.command('MepOrgExportPdf', mep.org_export_pdf)\n"
    "mep.command('MepOrgExportOdt', mep.org_export_odt)\n"
    // Subtree export deliberately keeps its pre-existing, narrower
    // behavior (no babel execution, no #+INCLUDE: resolution -- see
    // mep.org_export_subtree's own header) rather than being folded into
    // mep_org_export_prepare: it already reads the live buffer directly
    // by row range, and generalizing it to the babel-results-spliced
    // scratch-array model above would need its own lines-array variant
    // of mep_org_subtree_end/mep_org_current_headline_row, a real but
    // separate follow-up, not required for the whole-buffer export the
    // user actually asked for.
    "mep.command('MepOrgExportSubtreeHtml', function() mep_org_export_to_file(mep.org_export_subtree('html'), 'html') end)\n"
    "mep.command('MepOrgExportSubtreeMarkdown', function() mep_org_export_to_file(mep.org_export_subtree('markdown'), 'md') end)\n"
    "mep.leader_map('oeh', 'Org: export to HTML', mep.org_export_html)\n"
    "mep.leader_map('oep', 'Org: export to PDF', mep.org_export_pdf)\n"
    "mep.leader_map('oeo', 'Org: export to ODT', mep.org_export_odt)\n"
    "mep.leader_map('oem', 'Org: export to Markdown', mep.org_export_markdown)\n"
    "mep.leader_map('oea', 'Org: export to ASCII', mep.org_export_ascii)\n";

// Part VIII, Phase 37 -- Roam (zettelkasten note linking). One-note-per-
// file, file-level :ID: property drawer at the very top (before any
// headline) per org-roam v2 convention, `[[id:...][title]]` links reuse
// Phase 30's link machinery (org_link_follow's `id:` branch) unchanged.
const char *kBuiltinOrgRoam =
    // Defaults to ~/org-roam/wiki (a user config's own init.lua assignment
    // to mep.org_roam_dirs, evaluated after this file's own DoString call
    // in main(), simply overwrites this the same way it already overrides
    // any other kBuiltin* default) -- the directory itself (both levels,
    // ~/org-roam and ~/org-roam/wiki) is created lazily, on first actual
    // write (mep_org_roam_ensure_dir, called from org_roam_new_note/
    // org_roam_daily below), not eagerly here at startup: a user who never
    // touches org-roam shouldn't get a new folder under their home
    // directory just for having launched mep.
    "local mep_org_roam_default_dir = (os.getenv('HOME') or '.') .. '/org-roam/wiki'\n"
    "mep.org_roam_dirs = {mep_org_roam_default_dir}\n"
    "mep.org_roam_daily_dir = nil\n"
    "local mep_org_roam_sidebar_id = nil\n"
    // mep.fs_mkdir only creates one level (std::filesystem::create_directory,
    // not create_directories) -- recurses up to create any missing parent
    // (~/org-roam itself, for the ~/org-roam/wiki default) before the leaf,
    // same as `mkdir -p`. A no-op, not an error, on a dir that already
    // exists (create_directory's own contract).
    "local function mep_org_roam_ensure_dir(dir)\n"
    "  if not dir then return end\n"
    "  local parent = dir:match('^(.*)/[^/]+$')\n"
    "  if parent and parent ~= '' then mep_org_roam_ensure_dir(parent) end\n"
    "  mep.fs_mkdir(dir)\n"
    "end\n"
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
    // #+title:/#+TITLE:/#+Title: are all the same keyword in real
    // org-mode (case-insensitive) -- Emacs org-roam's own default
    // capture template writes the lowercase form, so matching only the
    // uppercase spelling here silently missed it and fell through to
    // the first-headline fallback below for any note using that
    // template, showing the wrong entry name in the roam pickers.
    "local function mep_org_roam_title_of(lines)\n"
    "  for _, line in ipairs(lines) do\n"
    "    local t = line:match('^#%+[Tt][Ii][Tt][Ll][Ee]:%s*(.*)$')\n"
    "    if t then return t end\n"
    "  end\n"
    "  for _, line in ipairs(lines) do\n"
    "    local h = mep_org_parse_headline(line)\n"
    "    if h then return h.title end\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    // Title-index cache (<leader>ors, mep.org_roam_sync below): find_notes
    // and insert_link both need every note's title, which means reading
    // and line-scanning every single file in mep.org_roam_dirs -- fine
    // for a handful of notes, real work for a large wiki, and pure waste
    // to redo from scratch on *every* picker invocation when the vault's
    // contents rarely change between them. Built lazily on first need
    // (nil cache) and reused after that; mep.org_roam_sync forces a
    // rebuild (nils it, so the very next accessor call repopulates it) --
    // an explicit refresh mirrors real org-roam's own org-roam-db-sync
    // mental model, for when notes were added/renamed/deleted outside
    // mep (another editor, git pull, ...) since the cache was last built.
    // org_roam_new_note (below) also nils this after creating a note, so
    // a brand new note shows up in the very next find/insert-link without
    // needing an explicit sync first.
    "local mep_org_roam_notes_cache = nil\n"
    "local function mep_org_roam_build_notes()\n"
    "  local notes = {}\n"
    "  for _, path in ipairs(mep_org_roam_files()) do\n"
    "    local lines = mep_org_read_file_lines(path)\n"
    "    local title = lines and mep_org_roam_title_of(lines)\n"
    "    notes[#notes + 1] = {path = path, title = title}\n"
    "  end\n"
    "  return notes\n"
    "end\n"
    "local function mep_org_roam_notes()\n"
    "  if not mep_org_roam_notes_cache then mep_org_roam_notes_cache = mep_org_roam_build_notes() end\n"
    "  return mep_org_roam_notes_cache\n"
    "end\n"
    "function mep.org_roam_sync()\n"
    "  mep_org_roam_notes_cache = mep_org_roam_build_notes()\n"
    "  mep.notify('Org-roam: synced ' .. #mep_org_roam_notes_cache .. ' note(s)')\n"
    "end\n"
    "mep.command('MepOrgRoamSync', mep.org_roam_sync)\n"
    "mep.leader_map('ors', 'Org-roam: sync notes', mep.org_roam_sync)\n"
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
    "  for _, note in ipairs(mep_org_roam_notes()) do\n"
    "    if note.path ~= mep.filename() and note.title then\n"
    "      items[#items + 1] = {display = note.title, data = note.path}\n"
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
    // Note-browser picker (<leader>orr): same file list/title lookup as
    // Insert Link just above, but opens the picked note directly in the
    // current pane (mep.pane_open) instead of inserting a link to it from
    // wherever the cursor happens to be -- the "find a note" half of
    // org-roam-node-find, where Insert Link is the "link to a note" half.
    // Live preview (mep_picker_preview_file, kBuiltinPickerSources) mirrors
    // mep.find_files' own on_select_change usage.
    "function mep.org_roam_find_notes()\n"
    "  local items = {}\n"
    "  for _, note in ipairs(mep_org_roam_notes()) do\n"
    "    items[#items + 1] = {display = note.title or note.path, data = note.path}\n"
    "  end\n"
    "  if #items == 0 then mep.notify('No roam notes found', 'warn') return end\n"
    "  mep.picker_open('Roam: Find Note', items, function(path)\n"
    "    if path then mep.pane_open(path) end\n"
    "  end, nil, nil, function(path)\n"
    "    if path then mep_picker_preview_file(path) end\n"
    "  end)\n"
    "end\n"
    "mep.command('MepOrgRoamFindNotes', mep.org_roam_find_notes)\n"
    "mep.leader_map('orr', 'Org-roam: find note', mep.org_roam_find_notes)\n"
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
    "  mep_org_roam_ensure_dir(dir)\n"
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
    "    mep_org_roam_ensure_dir(dir)\n"
    "    local slug = title:lower():gsub('[^%w]+', '-'):gsub('^%-+', ''):gsub('%-+$', '')\n"
    "    mep.pane_open(dir .. '/' .. slug .. '.org')\n"
    "    mep.replace_lines(1, 1, {'#+TITLE: ' .. title})\n"
    "    mep.org_roam_ensure_id()\n"
    "    mep_org_roam_notes_cache = nil\n"  // so the new note shows up next find/insert-link with no explicit sync needed
    "  end)\n"
    "end\n"
    "mep.command('MepOrgRoamNewNote', mep.org_roam_new_note)\n"
    "mep.leader_map('ora', 'Org-roam: add note', mep.org_roam_new_note)\n"
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
    // Case-insensitive keyword match -- see mep_org_roam_title_of's own
    // comment (kBuiltinOrgRoam) for why (org keywords are case-
    // insensitive; a plain uppercase-only match misses the lowercase
    // #+title: form real org-roam/many hand-written notes use).
    "    local t = line:match('^#%+[Tt][Ii][Tt][Ll][Ee]:%s*(.*)$')\n"
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
    "  local lang_def, exe_or_err = mep_org_babel_resolve_lang(lang)\n"
    "  if not lang_def then mep.notify(exe_or_err, 'warn') return end\n"
    "  local body_lines = mep_org_babel_split_lines(solution .. '\\n\\n' .. tests)\n"
    "  mep_org_babel_spawn(lang_def, exe_or_err, body_lines, '', function(code, out_lines, err_lines)\n"
    "    mep.notify('Leetcode tests: ' .. (code == 0 and 'PASSED' or 'FAILED') .. ' (exit ' .. code .. ')',\n"
    "      code == 0 and 'info' or 'error')\n"
    "    if code ~= 0 then\n"
    "      for _, l in ipairs(out_lines) do mep.notify(l, 'warn') end\n"
    "      for _, l in ipairs(err_lines) do mep.notify(l, 'warn') end\n"
    "    end\n"
    "  end)\n"
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
    // (over a matched line's file) live_grep further down. mod1+j/k scrolls
    // within whatever got loaded (Editor::HandleMod1Shortcuts' Mode::Picker
    // case), but a file longer than max_lines is still truncated here, not
    // read in full -- the "..." marker makes that cutoff visible rather
    // than silently showing an incomplete file with no sign anything's
    // missing.\n"
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
    // Tracks whatever on_select_change last saw highlighted, for Ctrl-I
    // below (on_key only gets the pressed key, not the current item) --
    // same idiom as mep.live_grep's own st table further down.\n"
    "  local highlighted = nil\n"
    "  local function ensure_open()\n"
    "    if opened then return end\n"
    "    opened = true\n"
    "    mep.picker_open('Find Files', lines, function(item)\n"
    "      if item then mep.open(item) end\n"
    "    end, nil, function(key)\n"
    // Ctrl-I: open the highlighted file as a new buffer tab in the
    // *current* pane (mep.pane_open, Phase 14) instead of replacing
    // whatever's already showing there -- picker_close (not letting Enter's
    // on_select run too) since the file's already open at this point.
    // Ctrl-E/Ctrl-V: html view-toggle escape hatch, same meaning as
    // Editor::HandleSidebarInput's own Ctrl-E/Ctrl-V (mep.tree_on_key) --
    // `:e` (mep.cmd), not mep.open, is what gives Ctrl-E force-text
    // semantics (LoadFile's own comment).\n"
    "      if key == 'i' and highlighted and highlighted ~= '' then\n"
    "        mep.pane_open(highlighted)\n"
    "        mep.picker_close()\n"
    "      elseif key == 'e' and highlighted and highlighted ~= '' then\n"
    "        mep.cmd('e ' .. highlighted)\n"
    "        mep.picker_close()\n"
    "      elseif key == 'v' and highlighted and highlighted ~= '' then\n"
    "        mep.open(highlighted)\n"
    "        mep.picker_close()\n"
    "      end\n"
    "    end, function(item)\n"
    "      if item and item ~= '' then\n"
    "        highlighted = item\n"
    // 200 lines rather than the 40-line default: with mod1+j/k preview
    // scrolling now available (see the comment on mep_picker_preview_file
    // itself), a 40-line cap left almost nothing to actually scroll
    // through.\n"
    "        mep_picker_preview_file(item, 200)\n"
    "      end\n"
    "    end)\n"
    // on_select_change only fires on an actual highlight *change* -- prime
    // it with the first result so Ctrl-I (and the preview column) work
    // immediately, without requiring an arrow-key press first.\n"
    "    highlighted = lines[1]\n"
    "    mep_picker_preview_file(lines[1], 200)\n"
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
    "      if file then mep.open(file) mep.set_cursor(tonumber(lnum), 1) end\n"
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
    "mep.leader_map('pr', 'Live grep', mep.live_grep)\n"
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
    "              if item then mep.open(item) end\n"
    "            end)\n"
    "          end,\n"
    "        })\n"
    "      else\n"
    "        mep.picker_open('Files in ' .. dir, lines, function(item)\n"
    "          if item then mep.open(item) end\n"
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
    {
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

std::string NotifyLevelGlyph(Editor::NotifyLevel level) {
    // Nerd Font icon glyphs (g_icon_font, see kIconCodepoints and
    // DrawUiText's own comment) -- ASCII "X"/"!"/"i"/"." until that font
    // existed, since g_font's own atlas is ASCII-only and a Unicode symbol
    // codepoint drawn through it rendered as tofu "?".
    switch (level) {
        case Editor::NotifyLevel::Error: return Utf8FromCodepoint(0xf057);  // nf-fa-times_circle
        case Editor::NotifyLevel::Warn: return Utf8FromCodepoint(0xf071);   // nf-fa-exclamation_triangle
        case Editor::NotifyLevel::Info: return Utf8FromCodepoint(0xf05a);   // nf-fa-info_circle
        case Editor::NotifyLevel::Debug: return Utf8FromCodepoint(0xf188);  // nf-fa-bug
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
        std::string text = NotifyLevelGlyph(it->level) + "  " + it->message;
        float text_w = MeasureUiText(text, font_size);
        float box_w = text_w + 24;
        float box_h = font_size + 14;
        float x = screen_w - box_w - 10;
        DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(box_w), static_cast<int>(box_h),
                      ResolveHlGroup("FloatBg"));
        DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(box_w),
                            static_cast<int>(box_h), NotifyLevelColor(it->level));
        DrawUiText(text, Vector2{x + 12, y + 7}, font_size, ResolveHlGroup("Normal"));
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
    int content_top = MenuBarHeight() + TabBarHeight();
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

        // Resizable inner edge (the one facing the pane tree, not the
        // screen edge -- there's nothing to drag the outer one against):
        // grab strip centered on that edge, `sign` set so "dragging in
        // the direction that widens/heightens the on-screen gap between
        // this edge and the sidebar's own outer edge" always *shrinks*
        // toward zero the same intuitive way border-dragging between two
        // panes already does.
        {
            constexpr float kBorderGrabPx = 6.0f;
            bool horizontal = sb.position == "left" || sb.position == "right";
            int sign = (sb.position == "left" || sb.position == "top") ? 1 : -1;
            Rectangle grab;
            if (sb.position == "left") grab = Rectangle{px + pw - kBorderGrabPx / 2.0f, static_cast<float>(py), kBorderGrabPx, static_cast<float>(ph)};
            else if (sb.position == "right") grab = Rectangle{px - kBorderGrabPx / 2.0f, static_cast<float>(py), kBorderGrabPx, static_cast<float>(ph)};
            else if (sb.position == "top") grab = Rectangle{static_cast<float>(px), py + ph - kBorderGrabPx / 2.0f, static_cast<float>(pw), kBorderGrabPx};
            else grab = Rectangle{static_cast<float>(px), py - kBorderGrabPx / 2.0f, static_cast<float>(pw), kBorderGrabPx};
            g_sidebar_border_rects.push_back({sb.id, horizontal, sign, grab});
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
            DrawUiText(lines[i].text, Vector2{static_cast<float>(px + 8), ly}, font_size, color);
            g_sidebar_row_rects.push_back(
                {sb.id, static_cast<int>(i), Rectangle{static_cast<float>(px), ly - 1, static_cast<float>(pw), static_cast<float>(line_h)}});
        }
        EndScissorMode();
    }
}

// Office pane's own left icon-rail/Outline panel and right Format/Insert
// panel -- hand-drawn (not routed through SidebarInstance/DrawSidebars
// above; see the WYSIWYG restyle plan for why: the Format panel needs
// richer controls -- toggle icon buttons, tab switching -- than that
// generic text-row system models). Docked to the sides of *this specific
// pane* (pane_x/pane_w/content_y/content_h, its own screen rect) rather
// than the app window -- called from inside DrawPane's own office branch,
// same as the toolbar, so every office pane gets its own rail/panels
// regardless of which pane currently has focus. Draws into (ocx, ocw) on
// return: the horizontal band left over after reserving the rail (and
// Outline/Format panel widths, if open) for the toolbar/page to use.
void DrawOfficeSidePanels(float pane_x, float pane_w, float content_y, float content_h, int buffer_id,
                          const OfficeSession *office_sess, float &ocx, float &ocw) {
    ocx = pane_x;
    ocw = pane_w;

    // --- Left: icon rail (always shown for an office pane) ---
    Rectangle rail{ocx, content_y, kOfficeRailW, content_h};
    DrawRectangle(static_cast<int>(rail.x), static_cast<int>(rail.y), static_cast<int>(rail.width), static_cast<int>(rail.height),
                  ResolveHlGroup("MenuBar"));
    DrawLineEx(Vector2{rail.width, rail.y}, Vector2{rail.width, rail.y + rail.height}, 1.0f, ResolveHlGroup("Border"));

    auto rail_icon = [&](float cy, bool active, const std::function<void(Vector2, Color)> &draw, const std::function<void()> &on_click) {
        Rectangle hit{rail.x + 4.0f, cy - 14.0f, rail.width - 8.0f, 28.0f};
        Vector2 mouse = GetMousePosition();
        bool hovered = CheckCollisionPointRec(mouse, hit);
        if (active) DrawRectangleRounded(hit, 0.3f, 6, ResolveHlGroup("AccentTint"));
        else if (hovered) DrawRectangleRounded(hit, 0.3f, 6, ResolveHlGroup("CursorLine"));
        Color color = active ? ResolveHlGroup("Accent") : ResolveHlGroup("MutedFg");
        draw(Vector2{rail.x + rail.width / 2.0f, cy}, color);
        if (on_click) RegisterClickRegion(hit, on_click);
    };

    // Outline toggle -- a little "page with lines" glyph. The only
    // functional icon on this rail; the rest below are drawn for visual
    // fidelity with the reference design but intentionally not wired to
    // anything yet (no on_click), same documented tradeoff as the format
    // panel's margins/page-color controls.
    rail_icon(rail.y + 30.0f, g_office_outline_open,
        [](Vector2 c, Color color) {
            Rectangle page{c.x - 7.0f, c.y - 9.0f, 14.0f, 18.0f};
            DrawRectangleLinesEx(page, 1.3f, color);
            for (int i = 0; i < 3; i++) {
                float ly = page.y + 4.0f + static_cast<float>(i) * 4.5f;
                DrawLineEx(Vector2{page.x + 2.5f, ly}, Vector2{page.x + page.width - 2.5f, ly}, 1.1f, color);
            }
        },
        [] { g_office_outline_open = !g_office_outline_open; });
    rail_icon(rail.y + 72.0f, false,
        [](Vector2 c, Color color) {
            DrawCircleLines(static_cast<int>(c.x - 2.0f), static_cast<int>(c.y - 2.0f), 5.0f, color);
            DrawLineEx(Vector2{c.x + 1.5f, c.y + 1.5f}, Vector2{c.x + 6.0f, c.y + 6.0f}, 1.4f, color);
        },
        nullptr);
    rail_icon(rail.y + 108.0f, false,
        [](Vector2 c, Color color) {
            Rectangle rb{c.x - 5.0f, c.y - 8.0f, 10.0f, 16.0f};
            DrawRectangleLinesEx(rb, 1.2f, color);
            DrawTriangle(Vector2{rb.x + rb.width, rb.y + rb.height}, Vector2{rb.x, rb.y + rb.height},
                        Vector2{rb.x + rb.width / 2.0f, rb.y + rb.height - 6.0f}, ResolveHlGroup("MenuBar"));
        },
        nullptr);
    rail_icon(rail.y + 144.0f, false,
        [](Vector2 c, Color color) {
            DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y), 7.0f, color);
            DrawLineEx(c, Vector2{c.x, c.y - 4.0f}, 1.2f, color);
            DrawLineEx(c, Vector2{c.x + 3.0f, c.y}, 1.2f, color);
        },
        nullptr);
    rail_icon(rail.y + rail.height - 26.0f, false,
        [](Vector2 c, Color color) {
            DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y), 7.0f, color);
            DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y), 3.0f, color);
        },
        nullptr);
    ocx += kOfficeRailW;
    ocw -= kOfficeRailW;

    // --- Outline panel (left, when toggled open) ---
    if (g_office_outline_open && ocw - kOfficeOutlineW > 100.0f) {
        Rectangle panel{ocx, content_y, kOfficeOutlineW, content_h};
        DrawRectangle(static_cast<int>(panel.x), static_cast<int>(panel.y), static_cast<int>(panel.width),
                      static_cast<int>(panel.height), ResolveHlGroup("OfficePage"));
        DrawLineEx(Vector2{panel.x + panel.width, panel.y}, Vector2{panel.x + panel.width, panel.y + panel.height}, 1.0f,
                  ResolveHlGroup("Border"));
        float py = panel.y + 10.0f;
        DrawTextEx(g_font, "Outline", Vector2{panel.x + 14.0f, py}, MenuFontSize(), 0, ResolveHlGroup("Normal"));
        Rectangle close_rect{panel.x + panel.width - 28.0f, py - 2.0f, 20.0f, 20.0f};
        Vector2 cc{close_rect.x + close_rect.width / 2.0f, close_rect.y + close_rect.height / 2.0f};
        DrawLineEx(Vector2{cc.x - 4.0f, cc.y - 4.0f}, Vector2{cc.x + 4.0f, cc.y + 4.0f}, 1.3f, ResolveHlGroup("MutedFg"));
        DrawLineEx(Vector2{cc.x - 4.0f, cc.y + 4.0f}, Vector2{cc.x + 4.0f, cc.y - 4.0f}, 1.3f, ResolveHlGroup("MutedFg"));
        RegisterClickRegion(close_rect, [] { g_office_outline_open = false; });
        py += MenuFontSize() + 12.0f;
        // Search box: visual only in this pass -- filtering headings by
        // typed text would need its own text-input focus mode (mirroring
        // Mode::Prompt), a larger addition deferred out of this restyle.
        Rectangle search{panel.x + 12.0f, py, panel.width - 24.0f, 26.0f};
        DrawRectangleRounded(search, 0.3f, 6, ResolveHlGroup("MenuBar"));
        DrawTextEx(g_font, "Search document", Vector2{search.x + 8.0f, search.y + 5.0f}, g_font_size * 0.8f, 0, ResolveHlGroup("MutedFg"));
        py += search.height + 10.0f;

        BeginScissorMode(static_cast<int>(panel.x), static_cast<int>(py), static_cast<int>(panel.width),
                          static_cast<int>(panel.y + panel.height - py));
        if (office_sess) {
            const OfficeDoc &doc = office_sess->doc;
            g_office_status.outline_rows.clear();
            int active_heading_para = -1;
            float row_h = g_font_size + 12.0f;
            float ry = py;
            for (int pi = 0; pi < static_cast<int>(doc.paragraphs.size()); pi++) {
                const DocParagraph &para = doc.paragraphs[pi];
                if (para.heading_level <= 0) continue;
                if (pi <= office_sess->cursor_para) active_heading_para = pi;
                bool active = false;  // resolved after the loop once the last heading <= cursor is known
                Rectangle row{panel.x, ry, panel.width, row_h};
                std::string text = para.text.empty() ? "(untitled heading)" : para.text;
                float indent = 14.0f + static_cast<float>(para.heading_level - 1) * 14.0f;
                float fsize = g_font_size * (para.heading_level == 1 ? 0.95f : 0.85f);
                while (!text.empty() && MeasureTextEx(g_font, text.c_str(), fsize, 0).x > panel.width - indent - 12.0f) text.pop_back();
                RegisterClickRegion(row, [buffer_id, pi] {
                    g_editor.SetOfficeCursorPara(buffer_id, pi);
                    g_editor.SetOfficeScroll(buffer_id, pi, 0);
                });
                g_office_status.outline_rows.push_back({pi, row});
                ry += row_h;
                (void)active;
            }
            // Highlight pass: now that active_heading_para is known, redraw
            // just that row's background before its text -- simplest way to
            // avoid a second full doc scan only to find the same index.
            for (const auto &entry : g_office_status.outline_rows) {
                if (entry.first != active_heading_para) continue;
                DrawRectangleRec(entry.second, ResolveHlGroup("AccentTint"));
            }
            ry = py;
            for (int pi = 0; pi < static_cast<int>(doc.paragraphs.size()); pi++) {
                const DocParagraph &para = doc.paragraphs[pi];
                if (para.heading_level <= 0) continue;
                std::string text = para.text.empty() ? "(untitled heading)" : para.text;
                float indent = 14.0f + static_cast<float>(para.heading_level - 1) * 14.0f;
                float fsize = g_font_size * (para.heading_level == 1 ? 0.95f : 0.85f);
                while (!text.empty() && MeasureTextEx(g_font, text.c_str(), fsize, 0).x > panel.width - indent - 12.0f) text.pop_back();
                Color tc = (pi == active_heading_para) ? ResolveHlGroup("Accent") : ResolveHlGroup("Normal");
                DrawTextEx(g_font, text.c_str(), Vector2{panel.x + indent, ry + (row_h - fsize) / 2.0f}, fsize, 0, tc);
                ry += row_h;
            }
        }
        EndScissorMode();
        ocx += kOfficeOutlineW;
        ocw -= kOfficeOutlineW;
    }

    // --- Format/Insert panel (right, when toggled open) ---
    if (g_office_format_open && ocw - kOfficeFormatW > 100.0f) {
        Rectangle panel{ocx + ocw - kOfficeFormatW, content_y, kOfficeFormatW, content_h};
        DrawRectangle(static_cast<int>(panel.x), static_cast<int>(panel.y), static_cast<int>(panel.width),
                      static_cast<int>(panel.height), ResolveHlGroup("OfficePage"));
        DrawLineEx(Vector2{panel.x, panel.y}, Vector2{panel.x, panel.y + panel.height}, 1.0f, ResolveHlGroup("Border"));
        float py = panel.y + 10.0f;
        float tab_w = 70.0f, tab_h = 26.0f;
        for (int t = 0; t < 2; t++) {
            Rectangle tab{panel.x + 14.0f + static_cast<float>(t) * (tab_w + 6.0f), py, tab_w, tab_h};
            bool active = g_office_format_tab == t;
            if (active) DrawRectangleRounded(tab, 0.3f, 6, ResolveHlGroup("AccentTint"));
            const char *label = t == 0 ? "Format" : "Insert";
            Vector2 ts = MeasureTextEx(g_font, label, g_font_size * 0.85f, 0);
            DrawTextEx(g_font, label, Vector2{tab.x + (tab.width - ts.x) / 2.0f, tab.y + (tab.height - ts.y * 0.85f) / 2.0f},
                      g_font_size * 0.85f, 0, active ? ResolveHlGroup("Accent") : ResolveHlGroup("Normal"));
            RegisterClickRegion(tab, [t] { g_office_format_tab = t; });
        }
        Rectangle close_rect{panel.x + panel.width - 28.0f, py - 2.0f, 20.0f, 20.0f};
        Vector2 cc{close_rect.x + close_rect.width / 2.0f, close_rect.y + close_rect.height / 2.0f};
        DrawLineEx(Vector2{cc.x - 4.0f, cc.y - 4.0f}, Vector2{cc.x + 4.0f, cc.y + 4.0f}, 1.3f, ResolveHlGroup("MutedFg"));
        DrawLineEx(Vector2{cc.x - 4.0f, cc.y + 4.0f}, Vector2{cc.x + 4.0f, cc.y - 4.0f}, 1.3f, ResolveHlGroup("MutedFg"));
        RegisterClickRegion(close_rect, [] { g_office_format_open = false; });
        py += tab_h + 14.0f;

        auto section_label = [&](const char *label) {
            DrawTextEx(g_font, label, Vector2{panel.x + 14.0f, py}, g_font_size * 0.85f, 0, ResolveHlGroup("MutedFg"));
            py += g_font_size * 0.85f + 8.0f;
        };
        auto icon_row_btn = [&](float *bx, bool active, float w, const std::function<void(Rectangle, Color)> &draw,
                                 const std::function<void()> &on_click) {
            Rectangle rect{*bx, py, w, 28.0f};
            Vector2 mouse = GetMousePosition();
            bool hovered = CheckCollisionPointRec(mouse, rect);
            if (active) DrawRectangleRounded(rect, 0.25f, 6, ResolveHlGroup("AccentTint"));
            else if (hovered) DrawRectangleRounded(rect, 0.25f, 6, ResolveHlGroup("CursorLine"));
            Color color = active ? ResolveHlGroup("Accent") : ResolveHlGroup("Normal");
            draw(rect, color);
            RegisterClickRegion(rect, on_click);
            *bx += w + 4.0f;
        };

        if (g_office_format_tab == 0) {
            section_label("Text");
            float bx = panel.x + 14.0f;
            struct SB { char which; const char *ch; };
            static const SB kSB[] = {{'b', "B"}, {'i', "I"}, {'u', "U"}, {'s', "S"}};
            for (const SB &sb : kSB) {
                char which = sb.which;
                icon_row_btn(&bx, office_sess && g_editor.OfficeFormatActive(which), 28.0f,
                    [&](Rectangle rect, Color color) {
                        Vector2 ts = MeasureTextEx(g_font, sb.ch, g_font_size, 0);
                        DrawTextEx(g_font, sb.ch, Vector2{rect.x + (rect.width - ts.x) / 2.0f, rect.y + (rect.height - g_font_size) / 2.0f},
                                  g_font_size, 0, color);
                    },
                    [which] { g_editor.ToggleOfficeFormat(which); });
            }
            py += 28.0f + 16.0f;

            section_label("Paragraph");
            bx = panel.x + 14.0f;
            struct AB { DocParagraph::Align align; int kind; };
            static const AB kAB[] = {
                {DocParagraph::Align::Left, 0}, {DocParagraph::Align::Center, 1},
                {DocParagraph::Align::Right, 2}, {DocParagraph::Align::Justify, 3}};
            for (const AB &ab : kAB) {
                DocParagraph::Align align = ab.align;
                int kind = ab.kind;
                icon_row_btn(&bx, office_sess && g_editor.OfficeAlignmentActive(align), 28.0f,
                    [kind](Rectangle rect, Color color) {
                        float pad = rect.width * 0.22f;
                        float full = rect.width - 2.0f * pad, bar_h = 2.0f;
                        float gap = (rect.height - 4.0f * bar_h) / 5.0f;
                        float widths[4] = {full, full * 0.62f, full, full * 0.8f};
                        for (int i = 0; i < 4; i++) {
                            float bw = (kind == 3) ? full : widths[i];
                            float by = rect.y + gap * static_cast<float>(i + 1) + bar_h * static_cast<float>(i);
                            float bx0 = rect.x + pad;
                            if (kind == 1) bx0 = rect.x + (rect.width - bw) / 2.0f;
                            else if (kind == 2) bx0 = rect.x + rect.width - pad - bw;
                            DrawRectangle(static_cast<int>(bx0), static_cast<int>(by), static_cast<int>(bw), static_cast<int>(bar_h), color);
                        }
                    },
                    [align] { g_editor.SetOfficeAlignment(align); });
            }
            py += 28.0f + 16.0f;

            section_label("Page");
            Rectangle margins{panel.x + 14.0f, py, panel.width - 28.0f, 26.0f};
            DrawRectangleRounded(margins, 0.2f, 6, ResolveHlGroup("MenuBar"));
            DrawTextEx(g_font, "Margins: Normal", Vector2{margins.x + 8.0f, margins.y + 5.0f}, g_font_size * 0.8f, 0, ResolveHlGroup("MutedFg"));
            py += margins.height + 8.0f;
            DrawTextEx(g_font, "Page color", Vector2{panel.x + 14.0f, py + 5.0f}, g_font_size * 0.8f, 0, ResolveHlGroup("MutedFg"));
            Rectangle swatch{panel.x + panel.width - 42.0f, py, 28.0f, 18.0f};
            DrawRectangleRec(swatch, ResolveHlGroup("OfficePage"));
            DrawRectangleLinesEx(swatch, 1.0f, ResolveHlGroup("Border"));
        } else {
            section_label("Insert");
            struct InsB { const char *label; std::function<void()> action; };
            const InsB items[] = {
                {"Image", [] { g_editor.InsertOfficeImagePrompt(); }},
                {"Table", [] { g_editor.InsertOfficeTablePrompt(); }},
                {"Equation", [] { g_editor.InsertOfficeMath(); }},
            };
            for (const InsB &it : items) {
                Rectangle row{panel.x + 12.0f, py, panel.width - 24.0f, 30.0f};
                Vector2 mouse = GetMousePosition();
                if (CheckCollisionPointRec(mouse, row)) DrawRectangleRounded(row, 0.2f, 6, ResolveHlGroup("CursorLine"));
                DrawTextEx(g_font, it.label, Vector2{row.x + 10.0f, row.y + (row.height - g_font_size) / 2.0f}, g_font_size, 0, ResolveHlGroup("Normal"));
                RegisterClickRegion(row, it.action);
                py += row.height + 4.0f;
            }
        }
        ocw -= kOfficeFormatW;
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
// (buffers/commands/...) are visually unchanged.
//
// The colorscheme picker gets its own always-on preview column of color
// swatches instead: it has no text worth previewing, and unlike the
// text-preview sources it doesn't need on_select_change to push anything
// -- the highlighted row's Palette is looked up fresh every frame below,
// so it's never a stale item behind.
bool IsSwatchPreviewPicker() { return g_editor.PickerTitle() == "Colorscheme"; }

void DrawPickerOverlay() {
    std::vector<PickerItem> results = g_editor.PickerFilteredResults();
    int selected = g_editor.PickerSelected();
    bool has_preview = !g_editor.PickerPreview().empty() || IsSwatchPreviewPicker();
    // Sized like mep.nvim's own picker (mep.nvim/lua/mep/picker/ui.lua's
    // create_layout: width = 0.9*columns, height = 0.8*lines, left/results
    // column = 0.38*width) -- 80% of width rather than mep.nvim's 90%,
    // since this overlay is one box rather than mep.nvim's three separate
    // floating windows and reads busier at the same fraction. Floors keep
    // it usable in a small window.
    int box_w = std::max(400, static_cast<int>(GetScreenWidth() * 0.8f));
    int box_h = std::max(300, static_cast<int>(GetScreenHeight() * 0.8f));
    FloatFrame f = DrawFloatFrame(box_w, box_h, g_editor.PickerTitle());

    std::string prompt_line = "> " + g_editor.PickerQuery();
    DrawTextEx(g_font, prompt_line.c_str(), Vector2{f.content_x, f.content_y}, g_font_size, 0, ResolveHlGroup("Normal"));
    {
        float cx = f.content_x + MeasureTextEx(g_font, prompt_line.c_str(), g_font_size, 0).x;
        DrawRectangle(static_cast<int>(cx), static_cast<int>(f.content_y), 2, static_cast<int>(g_font_size),
                      ResolveHlGroup("Normal"));
    }
    DrawLine(f.box_x + 4, static_cast<int>(f.content_y + g_font_size + 6), f.box_x + f.box_w - 4,
             static_cast<int>(f.content_y + g_font_size + 6), ResolveHlGroup("PickerBorder"));

    // 0.38 split matches mep.nvim's ui.lua (left_width = 0.38 * width).
    int list_w = has_preview ? static_cast<int>((f.box_x + f.box_w - f.content_x) * 0.38f) : (f.box_x + f.box_w) - static_cast<int>(f.content_x) - 4;

    float list_y = f.content_y + g_font_size + 14;
    int line_h = static_cast<int>(g_font_size) + 4;
    int max_rows = std::max(1, static_cast<int>((f.box_y + f.box_h - list_y) / line_h));
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
        // Mirrors mep.nvim's own preview window, which carries a " Preview
        // " title on its border -- this box has no separate border to
        // caption, so the label sits at the same row as the prompt line.
        DrawTextEx(g_font, "Preview", Vector2{static_cast<float>(div_x + 10), f.content_y}, g_font_size, 0,
                   ResolveHlGroup("Comment"));
        float px = static_cast<float>(div_x + 10);
        int preview_w = (f.box_x + f.box_w) - div_x - 20;
        BeginScissorMode(div_x, static_cast<int>(list_y) - 2, preview_w + 20, f.box_y + f.box_h - static_cast<int>(list_y));
        if (IsSwatchPreviewPicker()) {
            // One row per named palette role: a filled swatch of that
            // role's color, its hex value, and the role name -- looked up
            // fresh from the highlighted row every frame (no stale state
            // to prime or clear, unlike the text preview below).
            std::string theme_name =
                (selected >= 0 && selected < static_cast<int>(results.size())) ? results[selected].data : std::string();
            Palette pal;
            if (g_editor.ThemePalette(theme_name, &pal)) {
                struct SwatchRow { const char *label; ThemeColor color; };
                SwatchRow rows[] = {
                    {"bg", pal.bg},     {"fg", pal.fg},     {"red", pal.red},       {"green", pal.green},
                    {"yellow", pal.yellow}, {"blue", pal.blue}, {"purple", pal.purple}, {"cyan", pal.cyan},
                    {"orange", pal.orange}, {"border", pal.border},
                };
                int swatch_size = static_cast<int>(g_font_size);
                for (int i = 0; i < static_cast<int>(sizeof(rows) / sizeof(rows[0])); i++) {
                    float ry = list_y + i * line_h;
                    DrawRectangle(static_cast<int>(px), static_cast<int>(ry), swatch_size, swatch_size,
                                  ToRaylib(rows[i].color));
                    DrawRectangleLines(static_cast<int>(px), static_cast<int>(ry), swatch_size, swatch_size,
                                        ResolveHlGroup("PickerBorder"));
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", rows[i].color.r, rows[i].color.g, rows[i].color.b);
                    std::string label = std::string(hex) + "  " + rows[i].label;
                    DrawTextEx(g_font, label.c_str(),
                               Vector2{px + swatch_size + 8, ry + (swatch_size - g_font_size) / 2.0f}, g_font_size, 0,
                               ResolveHlGroup("Normal"));
                }
            } else {
                DrawTextEx(g_font, "-- no preview --", Vector2{px, list_y}, g_font_size, 0, ResolveHlGroup("Comment"));
            }
            EndScissorMode();
        } else {
            int max_chars = std::max(10, static_cast<int>(preview_w / g_char_width));
            int row = 0;
            int max_preview_rows = static_cast<int>((f.box_y + f.box_h - list_y) / line_h);
            // mod1+j/k (Editor::HandleMod1Shortcuts' own Mode::Picker special
            // case) scrolls by skipping raw lines here -- PickerPreviewScroll()
            // counts the same raw lines SplitLines() returns, not the wrapped
            // rows the loop below further splits a too-long line into.
            std::vector<std::string> preview_lines = SplitLines(g_editor.PickerPreview());
            int scroll = std::min(g_editor.PickerPreviewScroll(), std::max(0, static_cast<int>(preview_lines.size()) - 1));
            for (size_t li = static_cast<size_t>(scroll); li < preview_lines.size(); li++) {
                const std::string &raw_line = preview_lines[li];
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
    {
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
// rendering dependency -- see its header comment -- so this mapping can't
// live there). The actual resolution (Default/Indexed/Rgb, ANSI-16/256-cube
// math) lives in Editor::ResolveVTermColor (editor.cpp) instead of here now
// -- shared with Editor::EnterTerminalNormalMode, which needs the exact
// same resolution once, up front, to snapshot into Decorations that outlive
// the live TerminalSession's own per-cell data (see that function's own
// comment) -- this is just the raylib::Color wrapper around it.
Color VTermColorToRaylib(const VTermColor &c, bool is_fg) {
    ThemeColor tc = g_editor.ResolveVTermColor(c, is_fg);
    return Color{tc.r, tc.g, tc.b, tc.a};
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

// One embedded-office-image cache slot (see g_office_image_textures below).
struct OfficeImageCacheEntry {
    Texture2D tex{};
    bool ok = false;  // false = the stored bytes didn't decode
};
// Keyed by (buffer_id, image_ref) packed into one int64 -- unlike a real
// file on disk (g_org_inline_image_textures' mtime-recheck reasoning),
// DocImage::bytes is immutable once inserted (InsertOfficeImagePrompt only
// ever appends a new DocImage, never mutates an existing one), so a
// buffer_id+index pair is a stable cache key for the life of that buffer id
// -- same "decoded once, never re-uploaded" assumption g_image_textures
// above already makes for a whole ImageSession.
std::unordered_map<long long, OfficeImageCacheEntry> g_office_image_textures;

// Lazily decodes + GPU-uploads one embedded office-document image (DocImage::
// bytes), cached in g_office_image_textures. Returns nullptr if the bytes
// don't decode (shouldn't happen -- InsertOfficeImagePrompt already
// validated them at insert time -- but a table/image dropped in from a
// round-tripped file could in principle hold anything).
Texture2D *GetOrLoadOfficeImageTexture(int buffer_id, int image_ref, const DocImage &img) {
    long long key = (static_cast<long long>(buffer_id) << 32) | static_cast<unsigned int>(image_ref);
    auto it = g_office_image_textures.find(key);
    if (it != g_office_image_textures.end()) return it->second.ok ? &it->second.tex : nullptr;
    OfficeImageCacheEntry entry;
    ImageDoc doc;
    entry.ok = doc.LoadFromMemory(reinterpret_cast<const unsigned char *>(img.bytes.data()), img.bytes.size());
    if (entry.ok) {
        Image ri{};
        ri.data = const_cast<unsigned char *>(doc.Pixels());
        ri.width = doc.Width();
        ri.height = doc.Height();
        ri.mipmaps = 1;
        ri.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        entry.tex = LoadTextureFromImage(ri);  // copies pixel data to the GPU; doc goes out of scope right after
    }
    OfficeImageCacheEntry &stored = g_office_image_textures[key] = entry;
    return stored.ok ? &stored.tex : nullptr;
}

// One org-mode inline-image cache slot (see g_org_inline_image_textures
// below): the uploaded texture plus the file's mtime as of that upload, so
// GetOrLoadOrgInlineImageTexture can tell a stale entry from a fresh one.
struct OrgInlineImageCacheEntry {
    Texture2D tex{};
    bool ok = false;  // false = load failed (missing file/bad decode)
    time_t mtime = 0;
};
// Keyed by resolved path (not buffer_id, unlike g_image_textures above --
// the same image path can appear on rows in more than one open buffer, and
// conversely the *same row* can point at a different path after every
// mep.org_image_scan() rescan) -- one entry can be shared across every row/
// buffer that currently links to it. Unlike g_image_textures' own ImageDoc
// pixels (decoded once at buffer-open time and never mutated again), the
// file behind one of these paths is a plain path on disk that routinely
// *does* get regenerated in place -- most commonly by re-running the very
// org-babel `:file` block (kBuiltinOrgBabel) that produced it, same
// filename, new content, no buffer edit of its own to trigger anything.
// GetOrLoadOrgInlineImageTexture below re-stats and, on an mtime change,
// reloads+re-uploads rather than trusting a stale cache hit forever the
// way g_image_textures safely can. mtime alone is not a *reliable* change
// signal, though (only best-effort, for a file that changed with no other
// warning) -- see EvictOrgInlineImageTexture just below for the case that
// actually needs a guarantee.
std::unordered_map<std::string, OrgInlineImageCacheEntry> g_org_inline_image_textures;

// Lazily loads + GPU-uploads an inline org image (a [[file:path]] link's
// target, resolved by mep.org_image_scan() into Buffer::org_image_rows),
// mtime-cached in g_org_inline_image_textures. Returns nullptr if `path`
// can't be stat'd, read, or decoded -- DrawPane's own inline-image branch
// falls back to showing the raw link text in that case. Native file I/O
// only (plain std::ifstream/stat, no emscripten binary-file-bridge branch
// the way Editor::LoadFile's own image-open path has) -- a deliberate,
// documented scope cut mirroring this project's existing wasm-build
// narrowings (e.g. the babel results cache's own "native-only" comment):
// inline images in a text buffer aren't part of the wasm/webview build's
// own feature surface today.
Texture2D *GetOrLoadOrgInlineImageTexture(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return nullptr;
    OrgInlineImageCacheEntry &entry = g_org_inline_image_textures[path];
    if (entry.mtime == st.st_mtime) return entry.ok ? &entry.tex : nullptr;

    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> bytes;
    if (f) bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    ImageDoc doc;
    if (entry.ok) UnloadTexture(entry.tex);  // replace a stale GPU handle rather than leak it
    entry.ok = !bytes.empty() && doc.LoadFromMemory(bytes.data(), bytes.size());
    entry.mtime = st.st_mtime;
    if (!entry.ok) return nullptr;

    Image img{};
    img.data = const_cast<unsigned char *>(doc.Pixels());
    img.width = doc.Width();
    img.height = doc.Height();
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    entry.tex = LoadTextureFromImage(img);  // copies pixel data to the GPU; doc goes out of scope right after
    return &entry.tex;
}

// Drops any cached entry for `path` outright (unloading its texture first,
// if it had one) so the *next* GetOrLoadOrgInlineImageTexture call does an
// unconditional fresh read+decode+upload -- exposed to Lua as
// mep.org_image_invalidate (lua_env.cpp), called by mep.org_babel_execute
// (kBuiltinOrgBabel) right after a :file block finishes successfully.
//
// This exists because GetOrLoadOrgInlineImageTexture's own mtime check,
// while a perfectly fine passive heuristic in general, is provably not
// enough to guarantee a re-run's output actually gets picked up: a
// still-running org-babel :file block can rewrite the very file this
// texture was loaded from *while its subprocess is still executing* --
// R's/Python's own PNG writer typically creates/truncates the file
// immediately and only finishes writing real pixel data at the very end
// (dev.off(), the final savefig() flush, ...). A frame that happens to
// call GetOrLoadOrgInlineImageTexture in that window sees a real mtime
// change, attempts a reload, and decode-fails against the still-incomplete
// file -- caching that *failure* at whatever mtime the file had at that
// instant. If the whole run finishes within the same one-second mtime
// granularity most filesystems expose (routine for a fast plot), the
// final, fully-written file can carry that exact same st_mtime, and
// GetOrLoadOrgInlineImageTexture's own `entry.mtime == st.st_mtime` check
// then never notices anything changed again -- the poisoned failure
// sticks permanently, confirmed reproducible in practice (re-running a
// block showed "image not found" even after toggling <leader>oti off and
// on again, which only rebuilds the row registry, Buffer::org_image_rows,
// not this texture cache). Calling this once mep.org_babel_execute
// already knows for certain a fresh, fully-written file exists (the
// subprocess has already exited by then, so there's no more racing left
// to do) sidesteps the whole problem instead of trying to out-guess it
// with a finer-grained clock.
void EvictOrgInlineImageTexture(const std::string &path) {
    auto it = g_org_inline_image_textures.find(path);
    if (it == g_org_inline_image_textures.end()) return;
    if (it->second.ok) UnloadTexture(it->second.tex);
    g_org_inline_image_textures.erase(it);
}

// One HtmlSession::theme_colors-recolored <img> cache slot -- a separate
// cache from g_org_inline_image_textures (not a themed variant folded into
// that one) since org-mode inline images have no theme toggle of their own
// and shouldn't pay for tracking theme_epoch at all. Same mtime-cached
// "reload on change" convention as GetOrLoadOrgInlineImageTexture, plus a
// theme_epoch check (Editor::ThemeEpoch(), same reasoning as
// PdfTextureCacheEntry's own field) so switching color schemes while an
// html pane is already in theme mode re-recolors instead of keeping a
// stale palette.
struct ThemedHtmlImageCacheEntry {
    Texture2D tex{};
    bool ok = false;
    time_t mtime = 0;
    int theme_epoch = -1;
};
std::unordered_map<std::string, ThemedHtmlImageCacheEntry> g_themed_html_image_textures;

// Lazily loads, luminance-recolors (ThemedPdfChannel -- same "PDF page
// recoloring" transform, reused verbatim here), and GPU-uploads a local
// <img> for HtmlSession::theme_colors mode. Returns nullptr on the same
// conditions GetOrLoadOrgInlineImageTexture does (missing/unreadable/
// undecodable file). A fresh decode per cache miss (not reusing
// GetOrLoadOrgInlineImageTexture's own cached texture) since that one
// never retains CPU-side pixels past upload -- recoloring needs the raw
// pixels, so this keeps its own independent decode rather than growing
// the shared org-image cache entry with a field org-mode itself never
// needs.
Texture2D *GetOrLoadThemedHtmlImageTexture(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return nullptr;
    int theme_epoch = g_editor.ThemeEpoch();
    ThemedHtmlImageCacheEntry &entry = g_themed_html_image_textures[path];
    if (entry.mtime == st.st_mtime && entry.theme_epoch == theme_epoch) return entry.ok ? &entry.tex : nullptr;

    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> bytes;
    if (f) bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    ImageDoc doc;
    if (entry.ok) UnloadTexture(entry.tex);  // replace a stale GPU handle rather than leak it
    entry.ok = !bytes.empty() && doc.LoadFromMemory(bytes.data(), bytes.size());
    entry.mtime = st.st_mtime;
    entry.theme_epoch = theme_epoch;
    if (!entry.ok) return nullptr;

    int iw = doc.Width(), ih = doc.Height();
    Color fg = ResolveHlGroup("Normal");
    Color bg = ResolveHlGroup("NormalBg");
    std::vector<unsigned char> themed(static_cast<size_t>(iw) * static_cast<size_t>(ih) * 4);
    const unsigned char *src_pixels = doc.Pixels();
    size_t n = static_cast<size_t>(iw) * static_cast<size_t>(ih);
    for (size_t i = 0; i < n; i++) {
        const unsigned char *src = &src_pixels[i * 4];
        float luminance = (0.299f * src[0] + 0.587f * src[1] + 0.114f * src[2]) / 255.0f;
        unsigned char *dst = &themed[i * 4];
        dst[0] = ThemedPdfChannel(fg.r, bg.r, luminance);
        dst[1] = ThemedPdfChannel(fg.g, bg.g, luminance);
        dst[2] = ThemedPdfChannel(fg.b, bg.b, luminance);
        dst[3] = src[3];  // alpha untouched -- only color channels ride the theme gradient
    }

    Image img{};
    img.data = themed.data();
    img.width = iw;
    img.height = ih;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    entry.tex = LoadTextureFromImage(img);  // copies pixel data to the GPU; `themed` goes out of scope right after
    return &entry.tex;
}

// One org-latex texture cache slot (see g_org_latex_textures below): unlike
// g_org_inline_image_textures, no mtime is tracked -- a rendered fragment's
// PNG is written once to a content-hashed path (mep_org_latex_render,
// kBuiltinOrgLatex: the hash covers the fragment's own text *and* the DPI
// it was rendered at) and never rewritten in place, so the only two reasons
// to reload are "never loaded this path before" and "the theme changed
// since the last upload" -- theme_epoch alone (no mtime field at all) is
// enough to catch both.
struct OrgLatexTextureCacheEntry {
    Texture2D tex{};
    bool ok = false;
    int theme_epoch = -1;
    int w = 0, h = 0;
};
// Keyed by resolved path, same reasoning as g_org_inline_image_textures
// (shared across every row/buffer currently pointing at a given rendered
// fragment).
std::unordered_map<std::string, OrgLatexTextureCacheEntry> g_org_latex_textures;

// Lazily loads + GPU-uploads a rendered LaTeX/math fragment (org_latex_rows'
// own path, produced by tectonic+pdftoppm as plain black-on-white), synced
// to the editor's own color scheme the same way GetOrUpdatePdfPageTexture
// recolors a PDF page: each pixel's luminance is mapped onto the gradient
// between ResolveHlGroup("Normal") (black -> editor foreground) and
// ResolveHlGroup("NormalBg") (white -> editor background) via the same
// ThemedPdfChannel helper, so a fragment reads as editor text instead of a
// white index card pasted into a dark buffer. Re-recolors (and
// re-uploads) whenever Editor::ThemeEpoch() has moved on since the last
// upload -- see that function's own comment (editor.h) for why a raw
// generation/mtime check alone would miss a live colorscheme preview.
// Returns nullptr if `path` can't be stat'd, read, or decoded -- DrawPane's
// own latex branch falls back to showing a warning in that case, same
// contract as GetOrLoadOrgInlineImageTexture.
Texture2D *GetOrLoadOrgLatexTexture(const std::string &path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return nullptr;
    int theme_epoch = g_editor.ThemeEpoch();
    OrgLatexTextureCacheEntry &entry = g_org_latex_textures[path];
    if (entry.ok && entry.theme_epoch == theme_epoch) return &entry.tex;

    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> bytes;
    if (f) bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    ImageDoc doc;
    bool decoded = !bytes.empty() && doc.LoadFromMemory(bytes.data(), bytes.size());
    if (!decoded) {
        if (entry.ok) UnloadTexture(entry.tex);
        entry = OrgLatexTextureCacheEntry{};
        return nullptr;
    }

    int w = doc.Width(), h = doc.Height();
    Color fg = ResolveHlGroup("Normal");
    Color bg = ResolveHlGroup("NormalBg");
    std::vector<unsigned char> themed(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const unsigned char *src_pixels = doc.Pixels();
    for (size_t i = 0, n = static_cast<size_t>(w) * static_cast<size_t>(h); i < n; i++) {
        const unsigned char *src = &src_pixels[i * 4];
        float luminance = (0.299f * src[0] + 0.587f * src[1] + 0.114f * src[2]) / 255.0f;
        unsigned char *dst = &themed[i * 4];
        dst[0] = ThemedPdfChannel(fg.r, bg.r, luminance);
        dst[1] = ThemedPdfChannel(fg.g, bg.g, luminance);
        dst[2] = ThemedPdfChannel(fg.b, bg.b, luminance);
        dst[3] = src[3];  // preserve alpha as-is (tectonic's own margin, if any)
    }

    Image img{};
    img.data = themed.data();
    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    if (entry.ok && entry.w == w && entry.h == h) {
        UpdateTexture(entry.tex, img.data);
    } else {
        if (entry.ok) UnloadTexture(entry.tex);
        entry.tex = LoadTextureFromImage(img);
    }
    entry.ok = true;
    entry.theme_epoch = theme_epoch;
    entry.w = w;
    entry.h = h;
    return &entry.tex;
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

// Active pane gets a thicker outline (still the theme's own BorderActive
// color, just more of it) so which pane has the cursor reads at a glance --
// a plain 1px BorderActive/BorderInactive color swap was too subtle to
// notice in a quick glance across a busy split. Shared by DrawPane's own
// ordinary-buffer path at the bottom of this function and each special-
// content branch above it (terminal/image/pdf/office/sheet), which each
// `return` early with their own border draw instead of falling through.
//
// Left/right/bottom are drawn at double the top edge's thickness -- the
// top edge runs right along the pane header, whose own background color
// (TabActive/MenuBar) already shows active state there, so thickening it
// too would just double up against that instead of adding legibility.
// Four separate DrawRectangle strips rather than one DrawRectangleLinesEx
// call, since that only ever draws one uniform thickness on all sides.
void DrawPaneBorder(float x, float y, float w, float h, bool is_active) {
    Color border_color = is_active ? ResolveHlGroup("BorderActive") : ResolveHlGroup("BorderInactive");
    float top_thick = is_active ? 3.0f : 1.0f;
    float side_thick = is_active ? 6.0f : 1.0f;
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(top_thick),
                  border_color);
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(side_thick), static_cast<int>(h),
                  border_color);
    DrawRectangle(static_cast<int>(x + w - side_thick), static_cast<int>(y), static_cast<int>(side_thick),
                  static_cast<int>(h), border_color);
    DrawRectangle(static_cast<int>(x), static_cast<int>(y + h - side_thick), static_cast<int>(w),
                  static_cast<int>(side_thick), border_color);
}

// --- Mini LaTeX math layout -------------------------------------------------
//
// A from-scratch, intentionally small LaTeX-math typesetter for the
// \(..\)/\[..\]/$..$/$$..$$ spans html_doc.cpp's ExtractMathSpans pulls out
// of org-mode's (and any other MathJax-targeting page's) exported HTML into
// synthetic <math> DOM nodes. Not real TeX math typesetting (no proper
// italic-correction/kerning tables, no real radical-stretching, no matrix/
// align environments) -- just enough of superscript/subscript/fraction/
// sqrt/Greek-and-operator-symbol layout that a real equation reads as an
// equation rather than raw "\alpha^2 + \beta^2" source text. Lives here
// rather than html_doc.h/.cpp for the same reason the rest of this file's
// own HTML layout does (see that section's header, just below): it needs
// real font metrics (MeasureTextEx against g_math_font), which the
// raylib-free DOM layer doesn't have access to.
enum class MathKind { Text, Row, Frac, Sqrt };

// One node in a parsed (but not yet laid-out) math expression tree. A
// std::vector<MathNode> member on a type that also contains MathNode
// members is fine in C++17 (vector supports incomplete element types) --
// no indirection/unique_ptr needed for this self-referential shape.
struct MathNode {
    MathKind kind = MathKind::Text;
    std::string text;                // Text kind only: literal glyph(s) to draw
    bool italic = true;              // Text kind only: bare variables slant, digits/operators/symbols stay upright
    std::vector<MathNode> children;  // Row: the sequence; Frac: [numerator, denominator]; Sqrt: [radicand]
    std::vector<MathNode> sup;       // trailing ^{...} attached to *this* node, 0 or 1 element (itself Row-kind)
    std::vector<MathNode> sub;       // trailing _{...} attached to *this* node, 0 or 1 element (itself Row-kind)
};

// Recursive-descent parser over the raw LaTeX source between the \(../\[..
// delimiters ExtractMathSpans already stripped -- no separate tokenizer,
// the grammar is small enough to scan character-by-character directly.
struct MathParser {
    const std::string &s;
    size_t i = 0;
    explicit MathParser(const std::string &src) : s(src) {}

    void SkipSpace() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    }

    // One command name's macro-expansion, name -> Unicode codepoint (must
    // also be in kMathCodepoints above, or it'll draw as a missing glyph).
    static const std::unordered_map<std::string, int> &SymbolTable() {
        static const std::unordered_map<std::string, int> kTable = {
            {"alpha", 0x3b1},    {"beta", 0x3b2},     {"gamma", 0x3b3},   {"delta", 0x3b4},
            {"epsilon", 0x3b5},  {"varepsilon", 0x3b5}, {"zeta", 0x3b6},  {"eta", 0x3b7},
            {"theta", 0x3b8},    {"iota", 0x3b9},      {"kappa", 0x3ba},  {"lambda", 0x3bb},
            {"mu", 0x3bc},       {"nu", 0x3bd},         {"xi", 0x3be},     {"pi", 0x3c0},
            {"rho", 0x3c1},      {"sigma", 0x3c3},      {"tau", 0x3c4},    {"upsilon", 0x3c5},
            {"phi", 0x3c6},      {"varphi", 0x3c6},     {"chi", 0x3c7},    {"psi", 0x3c8},
            {"omega", 0x3c9},
            {"Gamma", 0x393},    {"Delta", 0x394},      {"Theta", 0x398},  {"Lambda", 0x39b},
            {"Xi", 0x39e},       {"Pi", 0x3a0},          {"Sigma", 0x3a3},  {"Upsilon", 0x3a5},
            {"Phi", 0x3a6},      {"Psi", 0x3a8},         {"Omega", 0x3a9},
            {"times", 0xd7},     {"div", 0xf7},          {"pm", 0xb1},      {"mp", 0x2213},
            {"cdot", 0xb7},      {"circ", 0x2218},       {"leq", 0x2264},   {"le", 0x2264},
            {"geq", 0x2265},     {"ge", 0x2265},         {"neq", 0x2260},   {"ne", 0x2260},
            {"approx", 0x2248},  {"equiv", 0x2261},      {"sim", 0x223c},   {"propto", 0x221d},
            {"infty", 0x221e},   {"partial", 0x2202},    {"nabla", 0x2207}, {"sum", 0x2211},
            {"prod", 0x220f},    {"int", 0x222b},        {"degree", 0xb0},  {"circ", 0x2218},
            {"to", 0x2192},      {"rightarrow", 0x2192}, {"gets", 0x2190},  {"leftarrow", 0x2190},
            {"leftrightarrow", 0x2194}, {"Rightarrow", 0x21d2}, {"Leftarrow", 0x21d0},
            {"Leftrightarrow", 0x21d4}, {"iff", 0x21d4},
            {"forall", 0x2200},  {"exists", 0x2203},     {"in", 0x2208},    {"notin", 0x2209},
            {"subset", 0x2282},  {"subseteq", 0x2286},   {"supset", 0x2283}, {"supseteq", 0x2287},
            {"cup", 0x222a},     {"cap", 0x2229},        {"emptyset", 0x2205}, {"varnothing", 0x2205},
            {"cdots", 0x22ef},   {"ldots", 0x2026},      {"dots", 0x2026},
            {"therefore", 0x2234}, {"because", 0x2235},  {"perp", 0x22a5},  {"parallel", 0x2225},
            {"otimes", 0x2297},  {"oplus", 0x2295},      {"lfloor", 0x230a}, {"rfloor", 0x230b},
            {"lceil", 0x2308},   {"rceil", 0x2309},      {"prime", 0x2032},
        };
        return kTable;
    }

    // Parses "{ row }" (consuming both braces) or, absent a brace, a single
    // ParseAtom() -- used for both sup/sub arguments and \frac/\sqrt args,
    // matching real LaTeX's "one token or a braced group" argument rule.
    MathNode ParseGroupOrAtom() {
        SkipSpace();
        if (i < s.size() && s[i] == '{') {
            i++;
            MathNode row = ParseRow('}');
            if (i < s.size() && s[i] == '}') i++;
            return row;
        }
        return ParseAtom();
    }

    MathNode ParseCommand() {
        i++;  // consume '\'
        size_t start = i;
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) i++;
        std::string name = s.substr(start, i - start);
        if (name.empty()) {
            // "\\{", "\\}", "\\%", "\\,", "\\;", "\\ ", a literal-escape or
            // a spacing command -- neither has a dedicated glyph here, so
            // render the escaped char itself (harmless for the common
            // "\{"/"\}"/"\%" case; spacing commands like "\," just show as
            // a small stray character, an accepted cosmetic gap).
            MathNode n;
            n.kind = MathKind::Text;
            n.italic = false;
            if (i < s.size()) n.text = std::string(1, s[i++]);
            return n;
        }
        if (name == "left" || name == "right") return ParseAtom();  // sizing hint -- render the delimiter plain
        if (name == "frac" || name == "dfrac" || name == "tfrac") {
            MathNode n;
            n.kind = MathKind::Frac;
            n.children.push_back(ParseGroupOrAtom());
            n.children.push_back(ParseGroupOrAtom());
            return n;
        }
        if (name == "sqrt") {
            MathNode n;
            n.kind = MathKind::Sqrt;
            n.children.push_back(ParseGroupOrAtom());
            return n;
        }
        if (name == "text" || name == "mathrm" || name == "operatorname" || name == "mathbf") {
            MathNode grp = ParseGroupOrAtom();
            MathNode n;
            n.kind = MathKind::Row;
            auto upright_copy = [](MathNode m) {
                m.italic = false;
                return m;
            };
            if (grp.kind == MathKind::Row) {
                for (auto &c : grp.children) n.children.push_back(upright_copy(c));
            } else {
                n.children.push_back(upright_copy(grp));
            }
            return n;
        }
        auto it = SymbolTable().find(name);
        if (it != SymbolTable().end()) {
            MathNode n;
            n.kind = MathKind::Text;
            n.italic = false;
            n.text = Utf8FromCodepoint(it->second);
            return n;
        }
        // Unknown command -- show its name literally rather than dropping
        // it silently, so an unrecognized macro is at least legible/
        // debuggable instead of just vanishing from the equation.
        MathNode n;
        n.kind = MathKind::Text;
        n.italic = false;
        n.text = name;
        return n;
    }

    // One atom, *without* consuming a trailing ^/_ (ParseRow attaches
    // those to whatever atom precedes them).
    MathNode ParseAtom() {
        SkipSpace();
        if (i >= s.size()) return MathNode{};
        if (s[i] == '\\') return ParseCommand();
        char c = s[i++];
        MathNode n;
        n.kind = MathKind::Text;
        n.text = std::string(1, c);
        n.italic = std::isalpha(static_cast<unsigned char>(c)) != 0;
        return n;
    }

    // A left-to-right sequence of atoms (each optionally followed by ^/_),
    // stopping at `end` (or end-of-string if `end` is '\0', the top-level
    // call's own sentinel).
    MathNode ParseRow(char end) {
        MathNode row;
        row.kind = MathKind::Row;
        while (true) {
            SkipSpace();
            if (i >= s.size() || (end != '\0' && s[i] == end)) break;
            // Every parser branch is expected to consume input, but this is
            // also exercised continuously while an Office user is typing an
            // incomplete LaTeX expression. Keep that transient, malformed
            // state fail-safe: one byte of literal text is better than an
            // accidental non-progressing render loop freezing the UI.
            const size_t before_atom = i;
            MathNode atom = ParseGroupOrAtom();
            if (i == before_atom) {
                MathNode literal;
                literal.kind = MathKind::Text;
                literal.italic = false;
                literal.text = std::string(1, s[i++]);
                row.children.push_back(std::move(literal));
                continue;
            }
            for (;;) {
                SkipSpace();
                if (i < s.size() && s[i] == '^') {
                    i++;
                    atom.sup.push_back(ParseGroupOrAtom());
                } else if (i < s.size() && s[i] == '_') {
                    i++;
                    atom.sub.push_back(ParseGroupOrAtom());
                } else {
                    break;
                }
            }
            row.children.push_back(std::move(atom));
        }
        return row;
    }
};

// A relative glyph run inside a laid-out math expression -- (rel_x, rel_y)
// are offsets from the expression's own top-left, filled in once and never
// touched again once placed inline (mirrors HtmlRun's own "fixed at
// placement time" contract, just nested one level deeper).
struct MathGlyphRun {
    float rel_x = 0, rel_y = 0, font_size = 0;
    std::string text;
    bool italic = false;
};
// A horizontal bar (a fraction's rule, or a sqrt's overline), same
// relative-offset convention as MathGlyphRun.
struct MathBarRun {
    float rel_x = 0, rel_y = 0, w = 0;
};
struct MathLayoutResult {
    float width = 0, height = 0;
    // Distance from this box's top edge to its "alignment line" -- the row
    // this expression sits within (LayoutMathRow) shifts every sibling so
    // their own baselines line up on one shared value, the same way real
    // text baselines align across mixed inline font sizes.
    float baseline = 0;
    std::vector<MathGlyphRun> glyphs;
    std::vector<MathBarRun> bars;
};

MathLayoutResult LayoutMathAtom(const MathNode &n, float font_size);

// Composes `terms` left-to-right, each already carrying its own optional
// sup/sub (see MathNode::sup/sub), aligning every term's own baseline to
// the row's shared (tallest-above-baseline) value.
MathLayoutResult LayoutMathRow(const std::vector<MathNode> &terms, float font_size) {
    constexpr float kScriptScale = 0.7f;    // sup/sub shrink factor, roughly TeX's own scriptstyle ratio
    constexpr float kScriptRaise = 0.55f;   // superscript raised this fraction of font_size above the baseline
    constexpr float kScriptDrop = 0.18f;    // subscript dropped this fraction of font_size below the baseline
    struct Placed {
        MathLayoutResult layout;
        float x = 0;
    };
    std::vector<Placed> placed;
    float x = 0;
    float shared_baseline = 0;
    for (const MathNode &t : terms) {
        MathLayoutResult core = LayoutMathAtom(t, font_size);
        if (t.sup.empty() && t.sub.empty()) {
            shared_baseline = std::max(shared_baseline, core.baseline);
            placed.push_back({std::move(core), x});
            x += placed.back().layout.width;
            continue;
        }
        MathLayoutResult combined;
        float script_x = core.width + 1.0f;
        float max_script_w = 0;
        for (auto &g : core.glyphs) combined.glyphs.push_back(g);
        for (auto &b : core.bars) combined.bars.push_back(b);
        if (!t.sup.empty()) {
            MathLayoutResult sup = LayoutMathAtom(t.sup[0], font_size * kScriptScale);
            float sup_y = core.baseline - font_size * kScriptRaise - sup.baseline;
            for (auto g : sup.glyphs) {
                g.rel_x += script_x;
                g.rel_y += sup_y;
                combined.glyphs.push_back(g);
            }
            for (auto b : sup.bars) {
                b.rel_x += script_x;
                b.rel_y += sup_y;
                combined.bars.push_back(b);
            }
            max_script_w = std::max(max_script_w, sup.width);
        }
        if (!t.sub.empty()) {
            MathLayoutResult sub = LayoutMathAtom(t.sub[0], font_size * kScriptScale);
            float sub_y = core.baseline + font_size * kScriptDrop - sub.baseline;
            for (auto g : sub.glyphs) {
                g.rel_x += script_x;
                g.rel_y += sub_y;
                combined.glyphs.push_back(g);
            }
            for (auto b : sub.bars) {
                b.rel_x += script_x;
                b.rel_y += sub_y;
                combined.bars.push_back(b);
            }
            max_script_w = std::max(max_script_w, sub.width);
        }
        combined.width = script_x + max_script_w;
        combined.baseline = core.baseline;
        combined.height = core.height;
        for (auto &g : combined.glyphs) combined.height = std::max(combined.height, g.rel_y + g.font_size);
        shared_baseline = std::max(shared_baseline, combined.baseline);
        placed.push_back({std::move(combined), x});
        x += placed.back().layout.width;
    }
    MathLayoutResult row;
    row.width = x;
    float max_below = 0;
    for (Placed &p : placed) {
        float dy = shared_baseline - p.layout.baseline;
        for (auto g : p.layout.glyphs) {
            g.rel_x += p.x;
            g.rel_y += dy;
            row.glyphs.push_back(g);
        }
        for (auto b : p.layout.bars) {
            b.rel_x += p.x;
            b.rel_y += dy;
            row.bars.push_back(b);
        }
        max_below = std::max(max_below, p.layout.height - p.layout.baseline + dy);
    }
    row.baseline = shared_baseline;
    row.height = shared_baseline + max_below;
    return row;
}

// Lays out `n` itself (ignoring any sup/sub attached to it -- LayoutMathRow
// composes those onto whichever row this atom is a term of).
MathLayoutResult LayoutMathAtom(const MathNode &n, float font_size) {
    switch (n.kind) {
        case MathKind::Row:
            return LayoutMathRow(n.children, font_size);
        case MathKind::Frac: {
            constexpr float kFracScale = 0.92f;
            float sub_fs = font_size * kFracScale;
            MathLayoutResult num = n.children.size() > 0 ? LayoutMathAtom(n.children[0], sub_fs) : MathLayoutResult{};
            MathLayoutResult den = n.children.size() > 1 ? LayoutMathAtom(n.children[1], sub_fs) : MathLayoutResult{};
            float gap = std::max(2.0f, font_size * 0.12f);
            float w = std::max(num.width, den.width) + 6.0f;
            MathLayoutResult r;
            r.width = w;
            float num_x = (w - num.width) / 2.0f;
            float den_x = (w - den.width) / 2.0f;
            for (auto g : num.glyphs) {
                g.rel_x += num_x;
                r.glyphs.push_back(g);
            }
            for (auto b : num.bars) {
                b.rel_x += num_x;
                r.bars.push_back(b);
            }
            float bar_y = num.height + gap;
            r.bars.push_back({0, bar_y, w});
            float den_y = bar_y + gap;
            for (auto g : den.glyphs) {
                g.rel_x += den_x;
                g.rel_y += den_y;
                r.glyphs.push_back(g);
            }
            for (auto b : den.bars) {
                b.rel_x += den_x;
                b.rel_y += den_y;
                r.bars.push_back(b);
            }
            r.height = den_y + den.height;
            r.baseline = bar_y + gap * 0.4f;
            return r;
        }
        case MathKind::Sqrt: {
            MathLayoutResult inner = n.children.empty() ? MathLayoutResult{} : LayoutMathAtom(n.children[0], font_size);
            std::string radical = Utf8FromCodepoint(0x221a);
            float rad_w = MeasureTextEx(g_math_font, radical.c_str(), font_size, 0).x;
            constexpr float kOverlineGap = 3.0f;
            MathLayoutResult r;
            float pad = 3.0f;
            r.width = rad_w + pad + inner.width + pad;
            r.glyphs.push_back({0, kOverlineGap, font_size, radical, false});
            for (auto g : inner.glyphs) {
                g.rel_x += rad_w + pad;
                g.rel_y += kOverlineGap;
                r.glyphs.push_back(g);
            }
            for (auto b : inner.bars) {
                b.rel_x += rad_w + pad;
                b.rel_y += kOverlineGap;
                r.bars.push_back(b);
            }
            r.bars.push_back({rad_w, kOverlineGap, inner.width + pad});
            r.height = kOverlineGap + std::max(font_size, inner.height);
            r.baseline = kOverlineGap + inner.baseline;
            return r;
        }
        case MathKind::Text:
        default: {
            MathLayoutResult r;
            if (n.text.empty()) return r;
            Vector2 sz = MeasureTextEx(g_math_font, n.text.c_str(), font_size, 0);
            r.width = sz.x;
            r.height = font_size;
            r.baseline = font_size * 0.78f;  // approximates cap-height-to-baseline for this bake
            r.glyphs.push_back({0, 0, font_size, n.text, n.italic});
            return r;
        }
    }
}

// Entry point: parses+lays out one \(..\)/\[..\]/$..$/$$..$$ span's raw
// LaTeX source (delimiters already stripped by ExtractMathSpans) at
// `font_size`. Always succeeds -- an unparseable/unknown construct
// degrades to plain-looking text (see ParseCommand's own fallback) rather
// than failing outright, same tolerance as the rest of this HTML renderer.
MathLayoutResult LayoutMathExpression(const std::string &latex, float font_size) {
    MathParser parser(latex);
    MathNode top = parser.ParseRow('\0');
    return LayoutMathAtom(top, font_size);
}

// Same shear-for-italic technique as DrawHtmlRun (below) -- bare variables
// (MathNode::italic) draw slanted, matching standard math-mode convention;
// symbols/digits/operators stay upright.
void DrawMathLayout(float x, float y, const MathLayoutResult &m, Color color) {
    for (const MathGlyphRun &g : m.glyphs) {
        if (g.text.empty()) continue;
        Vector2 pos{x + g.rel_x, y + g.rel_y};
        if (!g.italic) {
            DrawTextEx(g_math_font, g.text.c_str(), pos, g.font_size, 0, color);
            continue;
        }
        rlPushMatrix();
        float baseline_y = pos.y + g.font_size;
        rlTranslatef(pos.x, baseline_y, 0);
        // clang-format off
        float shear[16] = {
            1.0f,   0.0f, 0.0f, 0.0f,
            -0.22f, 1.0f, 0.0f, 0.0f,
            0.0f,   0.0f, 1.0f, 0.0f,
            0.0f,   0.0f, 0.0f, 1.0f,
        };
        // clang-format on
        rlMultMatrixf(shear);
        rlTranslatef(-pos.x, -baseline_y, 0);
        DrawTextEx(g_math_font, g.text.c_str(), pos, g.font_size, 0, color);
        rlPopMatrix();
    }
    for (const MathBarRun &b : m.bars) {
        DrawRectangle(static_cast<int>(x + b.rel_x), static_cast<int>(y + b.rel_y), static_cast<int>(std::max(1.0f, b.w)),
                      1, color);
    }
}

// --- HTML-preview pane layout ---------------------------------------------
//
// Word-wrap/positioning for an HtmlDoc, mirroring the exact split
// OfficeDoc's own paragraphs/rendering has: html_doc.h's DOM+ComputedStyle
// model is raylib-free and knows nothing about pixels, so turning it into
// positioned, word-wrapped runs (which needs real font metrics --
// MeasureTextEx) lives here instead. Recomputed fresh every DrawPane call
// rather than cached on HtmlSession (see its own comment) -- the small
// hand-written pages this renderer targets are cheap enough to lay out
// every frame that a cache-invalidation scheme (width changed? DOM
// mutated by a script? both are easy to miss) isn't worth the complexity.
//
// Remote/local <img> is out of scope for actual pixel rendering (see
// html_doc.h's own header) -- shown as a bracketed [image: alt-or-
// filename] text marker instead of decoding it, deliberately: real image
// support needs a texture cache with its own load/eviction lifecycle
// (compare ImageSession/GetOrLoadImageTexture), a second subsystem this
// already-large feature doesn't also take on in v1.
struct HtmlRun {
    float x = 0, y = 0;
    float font_size = 0;
    std::string text;
    Color color{};
    bool bold = false, italic = false, underline = false, strikethrough = false;
};
struct HtmlRule {
    float x = 0, y = 0, w = 0;
};
// A resolved-and-sized local <img>, positioned the same way an HtmlRun is
// -- (x,y) in the laid-out document's own coordinate space, fixed once at
// placement time. `path` is re-resolved to a texture at draw time via
// GetOrLoadOrgInlineImageTexture (mtime-cached, already used for org
// inline images -- see that function's own header) rather than carrying a
// Texture2D here directly, since a fresh HtmlLayout is built every frame
// and shouldn't itself own GPU handles.
struct HtmlImageRun {
    float x = 0, y = 0, w = 0, h = 0;
    std::string path;
};
// A positioned \(..\)/\[..\]/$..$/$$..$$ span, already laid out by
// LayoutMathExpression -- `layout` is drawn via DrawMathLayout at (x,y).
struct HtmlMathRun {
    float x = 0, y = 0;
    Color color{};
    MathLayoutResult layout;
};
// A block element's own background-color box (ComputedStyle.has_bg,
// html_doc.cpp's ApplyDeclarations -- parsed there but never actually
// drawn anywhere before this struct existed). Pushed in document order
// (HtmlLayoutBlock pushes a parent's entry *before* recursing into its
// children, filling in `h` only once that child layout is known), so
// drawing this vector front-to-back naturally paints an ancestor's box
// first and a more specific descendant's box (e.g. mep_org_html_code_
// block's own two-tone header-over-body split, main.cpp's kBuiltinOrgExport)
// on top of it, not the other way around.
struct HtmlBgRect {
    float x = 0, y = 0, w = 0, h = 0;
    Color color{};
};
// A block element's own border box (ComputedStyle.border_top/right/bottom/
// left, html_doc.cpp's ApplyDeclarations) -- same "pushed by HtmlLayoutBlock
// before recursing, height filled in once known" shape as HtmlBgRect right
// above, and drawn the same front-to-back-in-document-order way so a
// descendant's border (e.g. mep_org_html_code_block's own header/body
// split having its own border-bottom, main.cpp's kBuiltinOrgExport) never
// gets painted over by an ancestor's. Each edge's width is 0 when that
// side has no border, which both DrawPane's draw loop and this struct's
// own producer treat as "don't draw this edge" rather than a separate
// per-edge bool.
struct HtmlBorderRect {
    float x = 0, y = 0, w = 0, h = 0;
    float top_w = 0, right_w = 0, bottom_w = 0, left_w = 0;
    Color top_c{}, right_c{}, bottom_c{}, left_c{};
};
struct HtmlLayout {
    std::vector<HtmlRun> runs;
    std::vector<HtmlRule> rules;
    std::vector<HtmlImageRun> images;
    std::vector<HtmlMathRun> math_runs;
    std::vector<HtmlBgRect> backgrounds;
    std::vector<HtmlBorderRect> borders;
    float total_height = 0;
};

struct HtmlLayoutCtx {
    float layout_width;
    float base_font_size;
    Color default_color;
    // Directory <img src="relative/path"> is resolved against -- the open
    // HtmlSession's own source file's parent dir (empty for a page with no
    // real on-disk source, in which case only absolute local paths resolve).
    std::string base_dir;
    float zoom = 1.0f;  // matches HtmlSession::zoom -- local images scale with the same pane zoom as text does
};

float HtmlLineHeight(float font_size) { return font_size + 6.0f; }

Color HtmlResolveColor(const ComputedStyle &s, const HtmlLayoutCtx &ctx) {
    if (!s.has_color) return ctx.default_color;
    return Color{s.color_r, s.color_g, s.color_b, 255};
}

struct HtmlPendingWord {
    std::string text;  // "\n" is a sentinel forced line break (from <br>), never real text
    float font_size;
    Color color;
    bool bold, italic, underline, strikethrough;
    // Set for a local <img> or a \(..\)/\[..\] math span placed inline --
    // at most one of the two is ever true. `text` is unused for either
    // (kept empty by their own construction sites below).
    bool is_image = false;
    std::string image_path;
    float image_w = 0, image_h = 0;
    bool is_math = false;
    MathLayoutResult math;
};

// Resolves an <img src> value against ctx.base_dir -- absolute local paths
// pass through unchanged; a remote (http/https) src has no local file to
// fetch here (WEBKIT_PARITY_PLAN.md Part IV Phase 13 is where subresource
// fetching would land) and returns "" so callers fall back to the
// existing [image: ...] placeholder text instead of a broken texture load.
std::string ResolveHtmlImagePath(const std::string &src, const std::string &base_dir) {
    if (src.empty()) return "";
    if (src.compare(0, 7, "http://") == 0 || src.compare(0, 8, "https://") == 0) return "";
    if (src[0] == '/') return src;
    if (base_dir.empty()) return src;
    return base_dir + "/" + src;
}

// Places `words` left-to-right starting at `indent_x`, wrapping to a new
// line (advancing `cursor_y` by that completed line's own tallest word --
// lines can mix font sizes/images/math, e.g. a <span style="font-size:...">
// or an inline equation) whenever the next word wouldn't fit within
// ctx.layout_width. Returns the cursor_y just past the last line. A run's
// own (x,y) is fixed at the moment it's placed -- unlike a real reflow
// engine, nothing here revisits an earlier run once its line is decided.
// Every word on a line is vertically centered within that line's own
// height (word_line_height's tallest entry) rather than all sharing the
// line's top y -- a real browser's inline layout keeps mixed-size content
// baseline-aligned, which (absent real font-metrics/ascent-descent
// tracking here) centering approximates far better than top-alignment
// does: a smaller run next to a taller one visually sits mid-row instead
// of looking pinned to the row's top edge. This is why runs/images/math
// can't be pushed to `out` as each word is placed (the old shape) -- a
// line's height, and therefore every word's centering offset within it,
// isn't known until the *last* word on that line is seen, so placement is
// buffered per-line and only pushed once flush_line() knows `lh`.
float HtmlFlushWords(std::vector<HtmlPendingWord> &words, float indent_x, float cursor_y, const HtmlLayoutCtx &ctx,
                      HtmlLayout &out) {
    if (words.empty()) return cursor_y;
    auto word_width = [](const HtmlPendingWord &w) -> float {
        if (w.is_image) return w.image_w;
        if (w.is_math) return w.math.width;
        return MeasureTextEx(g_font, w.text.c_str(), w.font_size, 0).x;
    };
    auto word_line_height = [](const HtmlPendingWord &w) -> float {
        if (w.is_image) return w.image_h + 6.0f;
        if (w.is_math) return std::max(w.math.height, w.font_size) + 6.0f;
        return HtmlLineHeight(w.font_size);
    };
    float x = indent_x;
    struct PlacedWord {
        const HtmlPendingWord *w;
        float x;
    };
    std::vector<PlacedWord> line;
    auto flush_line = [&]() {
        if (line.empty()) return;
        float lh = 0;
        for (const PlacedWord &pw : line) lh = std::max(lh, word_line_height(*pw.w));
        for (const PlacedWord &pw : line) {
            const HtmlPendingWord &w = *pw.w;
            float y = cursor_y + (lh - word_line_height(w)) / 2.0f;
            if (w.is_image) {
                out.images.push_back({pw.x, y, w.image_w, w.image_h, w.image_path});
            } else if (w.is_math) {
                out.math_runs.push_back({pw.x, y, w.color, w.math});
            } else {
                out.runs.push_back(
                    {pw.x, y, w.font_size, w.text, w.color, w.bold, w.italic, w.underline, w.strikethrough});
            }
        }
        cursor_y += lh;
        line.clear();
        x = indent_x;
    };
    for (const HtmlPendingWord &w : words) {
        if (w.text == "\n" && !w.is_image && !w.is_math) {
            flush_line();
            continue;
        }
        float word_w = word_width(w);
        float space_w = line.empty() ? 0 : MeasureTextEx(g_font, " ", w.font_size, 0).x;
        // ctx.layout_width is the page's absolute right edge (measured
        // from the same x=0 indent_x itself is), constant regardless of
        // indent -- matching every box-width formula elsewhere in this
        // file (e.g. HtmlLayoutBlock's `ctx.layout_width - indent_x`
        // background/border width). NOT indent_x + layout_width: that
        // would treat layout_width as a *width* to add on top of indent_x,
        // silently growing the right edge (and therefore the wrap column)
        // the more a line is indented -- invisible at the small indents
        // nested lists produce, but badly wrong for a deliberately
        // narrowed+centered block (ComputedStyle::has_max_width), whose
        // indent_x can be hundreds of pixels.
        if (!line.empty() && x + space_w + word_w > ctx.layout_width) flush_line();
        if (!line.empty()) x += MeasureTextEx(g_font, " ", w.font_size, 0).x;
        line.push_back({&w, x});
        x += word_w;
    }
    flush_line();
    return cursor_y;
}

// Splits `text` on whitespace into words appended to `out`, each stamped
// with `style`'s resolved formatting -- the DOM has no per-word style (a
// Text node's effective style is always its parent element's, already
// fully cascaded by ComputeStyles), so every word from the same text node
// shares one HtmlPendingWord template. Known limitation: a word is always
// drawn with a space before it once it's not the line's first (HtmlFlush
// Words) -- source markup like "<span>word</span>." (no whitespace before
// the '.') ends up with a space there anyway, since which two *sibling*
// text nodes/elements had no whitespace between them in the original
// source isn't tracked across this function's own per-text-node calls.
// Correctly tracking that needs a shared "did the previous emitted content
// end without trailing whitespace" flag threaded through every caller
// (HtmlCollectInlineChild's element recursion too, not just adjacent text
// nodes) -- judged not worth the added bookkeeping/regression risk for a
// cosmetic-only gap.
void HtmlCollectTextWords(const std::string &text, const ComputedStyle &style, const HtmlLayoutCtx &ctx,
                           std::vector<HtmlPendingWord> &out) {
    float fs = ctx.base_font_size * style.font_scale;
    Color color = HtmlResolveColor(style, ctx);
    size_t i = 0, n = text.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(text[i]))) i++;
        if (i > start) {
            out.push_back({text.substr(start, i - start), fs, color, style.bold, style.italic, style.underline,
                            style.strikethrough});
        }
    }
}

void HtmlLayoutBlock(DomNode *node, float indent_x, float &cursor_y, const HtmlLayoutCtx &ctx, HtmlLayout &out);

// Concatenates every descendant Text node's raw content in document order
// -- <pre>'s own layout and a <math> node's own raw LaTeX source (both
// below) need the literal text, not word-split.
void HtmlCollectRawText(DomNode *node, std::string &out) {
    for (auto &c : node->children) {
        if (c->type == DomNodeType::Text) out += c->text;
        else if (!c->style.display_none)
            HtmlCollectRawText(c.get(), out);
    }
}

// Handles exactly one inline-flow child `c` of some container (its
// cascaded style already resolved onto c->style by ComputeStyles;
// `parent_style` is only needed for the plain-Text case, whose own style
// is its *parent* element's). Recurses into a container child (<span>,
// <a>, <b>, ...) by calling itself over that child's own children --
// stops descending at a nested block element (shouldn't normally occur
// inside genuinely inline markup, but malformed pages do happen; treated
// as inline anyway rather than breaking the flow, same tolerance
// html_doc.cpp's own parser already extends to bad markup). Both
// HtmlLayoutBlock's per-child dispatch (further down, for an inline child
// of a block container) and this function's own container-recursion call
// it the same way, so a leaf inline element (<img>, a <math> span, <br>)
// is handled identically regardless of whether it's nested inside another
// inline container or a *direct* child of a block element -- e.g.
// "<p><img src=...></p>", exactly org-mode's own figure-export shape.
// (An earlier version of this dispatch split those two call sites, and
// the block-child one silently collected nothing for a childless element
// like <img> in that position -- not even the bracketed placeholder.)
void HtmlCollectInlineChild(DomNode *c, const ComputedStyle &parent_style, const HtmlLayoutCtx &ctx,
                             std::vector<HtmlPendingWord> &out) {
    if (c->type == DomNodeType::Text) {
        HtmlCollectTextWords(c->text, parent_style, ctx, out);
        return;
    }
    if (c->style.display_none) return;
    if (c->tag == "br") {
        out.push_back({"\n", ctx.base_font_size, ctx.default_color, false, false, false, false});
        return;
    }
    if (c->tag == "img") {
        auto alt_it = c->attrs.find("alt");
        auto src_it = c->attrs.find("src");
        std::string src = src_it != c->attrs.end() ? src_it->second : std::string();
        std::string resolved = ResolveHtmlImagePath(src, ctx.base_dir);
        Texture2D *tex = resolved.empty() ? nullptr : GetOrLoadOrgInlineImageTexture(resolved);
        if (tex) {
            float natural_w = static_cast<float>(tex->width) * ctx.zoom;
            float natural_h = static_cast<float>(tex->height) * ctx.zoom;
            auto attr_px = [&](const char *name) -> float {
                auto it = c->attrs.find(name);
                if (it == c->attrs.end()) return 0;
                char *end = nullptr;
                double v = std::strtod(it->second.c_str(), &end);
                return end != it->second.c_str() ? static_cast<float>(v) : 0;
            };
            float want_w = attr_px("width"), want_h = attr_px("height");
            float w, h;
            if (want_w > 0 && want_h > 0) {
                w = want_w;
                h = want_h;
            } else if (want_w > 0) {
                w = want_w;
                h = natural_h * (want_w / natural_w);
            } else if (want_h > 0) {
                h = want_h;
                w = natural_w * (want_h / natural_h);
            } else {
                w = natural_w;
                h = natural_h;
            }
            if (w > ctx.layout_width) {
                float scale = ctx.layout_width / w;
                w *= scale;
                h *= scale;
            }
            HtmlPendingWord word;
            word.font_size = ctx.base_font_size * c->style.font_scale;
            word.color = HtmlResolveColor(c->style, ctx);
            word.is_image = true;
            word.image_path = resolved;
            word.image_w = w;
            word.image_h = h;
            out.push_back(std::move(word));
            return;
        }
        std::string label = "[image: " +
                             (alt_it != c->attrs.end() && !alt_it->second.empty()
                                  ? alt_it->second
                                  : (src_it != c->attrs.end() ? Basename(src_it->second) : std::string("?"))) +
                             "]";
        out.push_back({label, ctx.base_font_size * c->style.font_scale, HtmlResolveColor(c->style, ctx), false,
                        true, false, false});
        return;
    }
    if (c->tag == "math") {
        std::string latex;
        HtmlCollectRawText(c, latex);
        HtmlPendingWord word;
        word.font_size = ctx.base_font_size * c->style.font_scale;
        word.color = HtmlResolveColor(c->style, ctx);
        word.is_math = true;
        word.math = LayoutMathExpression(latex, word.font_size);
        out.push_back(std::move(word));
        return;
    }
    for (auto &gc : c->children) HtmlCollectInlineChild(gc.get(), c->style, ctx, out);
}

// Per-line cursor state HtmlLayoutPreNode (below) mutates as it walks a
// <pre>'s descendant tree in document order -- line_index (not a running y,
// so a text node that starts mid-line and ends after several more '\n's
// doesn't need to hand a partially-advanced y back to its caller) times
// line_h against start_y gives each emitted run's actual y, matching
// HtmlLayoutPreformatted's own final cursor_y update below exactly.
struct HtmlPreCursor {
    float x = 0;
    int line_index = 0;
    float indent_x = 0;
    float start_y = 0;
    float font_size = 0;
    float line_h = 0;
    bool stripped_leading_newline = false;
};

// Recurses through a <pre>'s subtree carrying each element's own resolved
// style down to whatever text it directly contains -- unlike a flattened
// single-style render, a <span class="tok-X"> child (mep_org_html_code_
// block's own per-token syntax-highlighting markup, kBuiltinOrgExport)
// keeps its own color here instead of losing it to <pre>'s single ambient
// style. Splits only on literal '\n' bytes within each text node (never on
// width -- still no word-wrap); cur.x carries across sibling text/span
// boundaries on the same line and only resets at indent_x on a real '\n',
// so a highlighted token flows immediately after the plain text before it
// with no gap, exactly like one flat run of that whole line would have.
void HtmlLayoutPreNode(DomNode *node, const ComputedStyle &style, HtmlPreCursor &cur, const HtmlLayoutCtx &ctx,
                        HtmlLayout &out) {
    if (node->type == DomNodeType::Text) {
        std::string text = node->text;
        if (!cur.stripped_leading_newline) {
            cur.stripped_leading_newline = true;
            if (!text.empty() && text.front() == '\n') text.erase(text.begin());
        }
        Color color = HtmlResolveColor(style, ctx);
        size_t start = 0, n = text.size();
        while (start <= n) {
            size_t nl = text.find('\n', start);
            std::string piece = text.substr(start, (nl == std::string::npos ? n : nl) - start);
            if (!piece.empty()) {
                float y = cur.start_y + static_cast<float>(cur.line_index) * cur.line_h;
                out.runs.push_back({cur.x, y, cur.font_size, piece, color, style.bold, style.italic, false, false});
                cur.x += MeasureTextEx(g_font, piece.c_str(), cur.font_size, 0).x;
            }
            if (nl == std::string::npos) break;
            cur.x = cur.indent_x;
            cur.line_index++;
            start = nl + 1;
        }
        return;
    }
    if (node->style.display_none) return;
    for (auto &c : node->children) HtmlLayoutPreNode(c.get(), node->style, cur, ctx, out);
}

// <pre>: one run per contiguous same-styled span per source line,
// positioned verbatim with no word-wrap -- wrapping would require either
// truncating or re-flowing whitespace-significant content (code
// indentation), neither of which stays "preformatted".
void HtmlLayoutPreformatted(DomNode *node, float indent_x, float &cursor_y, const HtmlLayoutCtx &ctx,
                             HtmlLayout &out) {
    HtmlPreCursor cur;
    cur.x = indent_x;
    cur.indent_x = indent_x;
    cur.start_y = cursor_y;
    cur.font_size = ctx.base_font_size * node->style.font_scale;
    cur.line_h = HtmlLineHeight(cur.font_size);
    for (auto &c : node->children) HtmlLayoutPreNode(c.get(), node->style, cur, ctx, out);
    // +1: matches the old flattened version's own loop, which always
    // advanced cursor_y once more for the final (possibly unterminated)
    // line after the last real '\n' -- line_index only counts '\n's
    // actually seen, one short of the true line count.
    cursor_y = cur.start_y + static_cast<float>(cur.line_index + 1) * cur.line_h;
}

constexpr float kHtmlListIndentPx = 24.0f;

void HtmlLayoutBlock(DomNode *node, float indent_x, float &cursor_y, const HtmlLayoutCtx &ctx, HtmlLayout &out) {
    if (node->style.display_none) return;
    float line_h = HtmlLineHeight(ctx.base_font_size * node->style.font_scale);
    cursor_y += node->style.margin_top_lines * line_h;

    // "max-width: Nem" + a horizontal auto margin (ComputedStyle::
    // has_max_width/margin_h_auto, html_doc.cpp) -- the standard idiom a
    // real page (including this renderer's own kBuiltinOrgExport output)
    // uses to center a readable column narrower than the viewport, most
    // commonly on <body>. Narrows `indent_x` (centered within whatever
    // width was already available at this nesting level) and `eff_ctx`'s
    // layout_width for this node's own boxes AND its entire subtree below
    // -- every remaining reference in this function reads `eff_ctx`/the
    // adjusted `indent_x`, not the original `ctx` parameter, including the
    // recursive HtmlLayoutBlock calls further down, which is what actually
    // propagates the narrower width to children. Doesn't nest specially
    // (an already-narrowed descendant with its own max-width just narrows
    // further from whatever it was handed) -- consistent with real CSS,
    // and the only case that matters in practice since % isn't supported.
    HtmlLayoutCtx narrowed_ctx = ctx;
    bool narrowed = false;
    float unnarrowed_indent_x = indent_x;
    if (node->style.has_max_width && node->style.margin_h_auto) {
        float max_w_px = ctx.base_font_size * node->style.font_scale * node->style.max_width_em;
        float avail = std::max(0.0f, ctx.layout_width - indent_x);
        if (max_w_px < avail) {
            indent_x += (avail - max_w_px) / 2.0f;
            // ctx.layout_width is the page's absolute right-edge x
            // (constant regardless of indent -- see HtmlFlushWords' own
            // comment on this same convention), so the narrowed column's
            // right edge is the *new* indent_x plus its own width, not
            // max_w_px alone -- every box-width formula below (`eff_ctx.
            // layout_width - indent_x`) would otherwise double-subtract
            // this centering offset and draw a column narrower than
            // intended, off-center toward the left.
            narrowed_ctx.layout_width = indent_x + max_w_px;
            narrowed = true;
        }
    }
    const HtmlLayoutCtx &eff_ctx = narrowed ? narrowed_ctx : ctx;

    // Pushed *before* this node's own content is laid out (a child's own
    // background, pushed during the recursion below, lands at a later
    // vector index than this one) so drawing HtmlLayout::backgrounds
    // front-to-back paints this box first and a more specific descendant's
    // on top of it, not the other way -- see HtmlBgRect's own comment.
    // `h` is only known once cursor_y reflects this node's full content
    // height, so it's filled in by finish_bg() at every one of this
    // function's own return points instead of at push time.
    float block_top = cursor_y;
    size_t bg_index = node->style.has_bg ? out.backgrounds.size() : static_cast<size_t>(-1);
    if (bg_index != static_cast<size_t>(-1)) {
        // A centered max-width element's *background* still spans the full
        // width it would have without the constraint (unnarrowed_indent_x/
        // ctx.layout_width, not the narrowed column) -- real CSS special-
        // cases exactly this for <body> (its background paints the whole
        // canvas even though its box is narrower), which is what keeps a
        // centered page's side margins showing the page's own background
        // instead of this pane's dark theme color bleeding through. Every
        // other box (border, content wrap) still uses the narrowed column
        // -- only the background fill gets the wider canvas treatment.
        float bg_x = narrowed ? unnarrowed_indent_x : indent_x;
        float bg_w = narrowed ? (ctx.layout_width - unnarrowed_indent_x) : (eff_ctx.layout_width - indent_x);
        out.backgrounds.push_back(
            {bg_x, block_top, bg_w, 0.0f, Color{node->style.bg_r, node->style.bg_g, node->style.bg_b, 255}});
    }
    auto finish_bg = [&]() {
        if (bg_index != static_cast<size_t>(-1)) out.backgrounds[bg_index].h = cursor_y - block_top;
    };
    const ComputedStyle &cs = node->style;
    bool has_border = cs.border_top.present || cs.border_right.present || cs.border_bottom.present ||
                       cs.border_left.present;
    size_t border_index = has_border ? out.borders.size() : static_cast<size_t>(-1);
    if (border_index != static_cast<size_t>(-1)) {
        HtmlBorderRect br;
        br.x = indent_x;
        br.y = block_top;
        br.w = eff_ctx.layout_width - indent_x;
        if (cs.border_top.present) {
            br.top_w = cs.border_top.width_px;
            br.top_c = Color{cs.border_top.r, cs.border_top.g, cs.border_top.b, 255};
        }
        if (cs.border_right.present) {
            br.right_w = cs.border_right.width_px;
            br.right_c = Color{cs.border_right.r, cs.border_right.g, cs.border_right.b, 255};
        }
        if (cs.border_bottom.present) {
            br.bottom_w = cs.border_bottom.width_px;
            br.bottom_c = Color{cs.border_bottom.r, cs.border_bottom.g, cs.border_bottom.b, 255};
        }
        if (cs.border_left.present) {
            br.left_w = cs.border_left.width_px;
            br.left_c = Color{cs.border_left.r, cs.border_left.g, cs.border_left.b, 255};
        }
        out.borders.push_back(br);
    }
    auto finish_border = [&]() {
        if (border_index != static_cast<size_t>(-1)) out.borders[border_index].h = cursor_y - block_top;
    };

    if (node->tag == "hr") {
        cursor_y += line_h / 2.0f;
        out.rules.push_back({indent_x, cursor_y, eff_ctx.layout_width - indent_x});
        cursor_y += line_h / 2.0f;
        finish_bg();
        finish_border();
        cursor_y += node->style.margin_bottom_lines * line_h;
        return;
    }
    if (node->style.preserve_whitespace) {
        HtmlLayoutPreformatted(node, indent_x, cursor_y, eff_ctx, out);
        finish_bg();
        finish_border();
        cursor_y += node->style.margin_bottom_lines * line_h;
        return;
    }
    if (node->tag == "math") {
        // Display math (block per WalkAndStyle's own "display" attr
        // override, html_doc.cpp) -- centered on its own line, same
        // treatment real browsers give \[..\]/$$..$$.
        std::string latex;
        HtmlCollectRawText(node, latex);
        float fs = eff_ctx.base_font_size * node->style.font_scale;
        MathLayoutResult ml = LayoutMathExpression(latex, fs);
        float box_x = indent_x + std::max(0.0f, (eff_ctx.layout_width - indent_x - ml.width) / 2.0f);
        out.math_runs.push_back({box_x, cursor_y, HtmlResolveColor(node->style, eff_ctx), std::move(ml)});
        cursor_y += out.math_runs.back().layout.height;
        finish_bg();
        finish_border();
        cursor_y += node->style.margin_bottom_lines * line_h;
        return;
    }

    float my_indent = indent_x + static_cast<float>(node->style.list_depth) * kHtmlListIndentPx;
    std::vector<HtmlPendingWord> words;
    if (node->style.is_list_item) {
        std::string marker =
            node->style.ordered_list_item ? (std::to_string(node->style.list_item_index) + ". ") : "* ";
        words.push_back({marker, eff_ctx.base_font_size * node->style.font_scale, HtmlResolveColor(node->style, eff_ctx),
                          node->style.bold, node->style.italic, false, false});
    }
    auto flush_words = [&]() {
        if (words.empty()) return;
        cursor_y = HtmlFlushWords(words, my_indent, cursor_y, eff_ctx, out);
        words.clear();
    };

    for (auto &c : node->children) {
        if (c->type == DomNodeType::Text) {
            HtmlCollectTextWords(c->text, node->style, eff_ctx, words);
            continue;
        }
        if (c->style.display_none) continue;
        if (c->style.block) {
            flush_words();
            HtmlLayoutBlock(c.get(), my_indent, cursor_y, eff_ctx, out);
        } else {
            HtmlCollectInlineChild(c.get(), node->style, eff_ctx, words);
        }
    }
    flush_words();
    finish_bg();
    finish_border();
    cursor_y += node->style.margin_bottom_lines * line_h;
}

HtmlLayout LayoutHtmlDoc(const HtmlDoc &doc, const HtmlLayoutCtx &ctx) {
    HtmlLayout out;
    if (!doc.root) return out;
    float cursor_y = 0;
    for (auto &c : doc.root->children) {
        if (c->type != DomNodeType::Element || c->style.display_none) continue;
        HtmlLayoutBlock(c.get(), 0, cursor_y, ctx, out);
    }
    out.total_height = cursor_y;
    return out;
}

// One run's bold/italic/underline/strikethrough, at that run's own
// font_size -- unlike the org-emphasis decoration renderer this mirrors
// (DrawPane's main per-row decoration loop, further down), there's no
// already-drawn-upright copy underneath to erase-and-redraw over first:
// each HtmlRun is only ever drawn once, here, so italic's shear can wrap
// the actual draw call directly with no background-cover step.
void DrawHtmlRun(float x, float y, const HtmlRun &run) {
    bool sheared = run.italic;
    if (sheared) {
        rlPushMatrix();
        float baseline_y = y + run.font_size;
        rlTranslatef(x, baseline_y, 0);
        // clang-format off
        float shear[16] = {
            1.0f,   0.0f, 0.0f, 0.0f,
            -0.22f, 1.0f, 0.0f, 0.0f,
            0.0f,   0.0f, 1.0f, 0.0f,
            0.0f,   0.0f, 0.0f, 1.0f,
        };
        // clang-format on
        rlMultMatrixf(shear);
        rlTranslatef(-x, -baseline_y, 0);
    }
    DrawTextEx(g_font, run.text.c_str(), Vector2{x, y}, run.font_size, 0, run.color);
    // Same double-draw-offset-1px bold fake as org emphasis (g_font has no
    // real bold face) -- drawn inside the same shear so a bold+italic run
    // doesn't end up half-sheared.
    if (run.bold) DrawTextEx(g_font, run.text.c_str(), Vector2{x + 1, y}, run.font_size, 0, run.color);
    if (sheared) rlPopMatrix();
    if (run.underline || run.strikethrough) {
        int text_w = std::max(1, static_cast<int>(MeasureTextEx(g_font, run.text.c_str(), run.font_size, 0).x));
        if (run.underline) {
            DrawRectangle(static_cast<int>(x), static_cast<int>(y + run.font_size + 2), text_w, 1, run.color);
        }
        if (run.strikethrough) {
            DrawRectangle(static_cast<int>(x), static_cast<int>(y + run.font_size / 2), text_w, 1, run.color);
        }
    }
}

// --- Kanban board / Gantt chart panes ---------------------------------------
//
// Rendering only -- these never mutate KanbanSession/GanttSession's outline
// or Buf().lines directly (aside from the content_x/y/w/h geometry cache
// they refresh every frame for UpdateKanbanMouseInteraction/
// UpdateGanttMouseInteraction, called once per frame from the same site
// UpdatePaneMouseInteraction() is). A card/bar click that doesn't cross the
// drag threshold registers its own plain ClickRegion (focus + set focused
// card/row); an actual drag past the threshold is entirely the Update*
// functions' job, since they alone see mouse state across frames.

void DrawKanban(const Pane &pane, float x, float y, float w, float h, bool is_active) {
    KanbanSession *sess = g_editor.GetKanbanMutable(pane.buffer_id);
    if (!sess) return;
    sess->content_x = x;
    sess->content_y = y;
    sess->content_w = w;
    sess->content_h = h;

    std::vector<std::string> columns = g_editor.KanbanColumns(pane.buffer_id);
    BeginScissorMode(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h));
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                  ResolveHlGroup("NormalBg"));
    int header_h = PaneHeaderHeight();
    // A toolbar row above the per-column headers -- "+ New Card" (a
    // draggable chip, hit-tested directly by UpdateKanbanMouseInteraction
    // rather than a RegisterClickRegion, since a drag needs continuous
    // IsMouseButtonDown polling) and "+ Column" (a plain click). Pushes
    // everything below down by one extra header_h; UpdateKanbanMouseInteraction
    // mirrors this offset exactly the same way it already independently
    // calls PaneHeaderHeight() rather than sharing a cached value.
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), header_h, ResolveHlGroup("MenuBar"));
    sess->new_card_chip_x = x + 8;
    sess->new_card_chip_y = y + 4;
    sess->new_card_chip_w = 120;
    sess->new_card_chip_h = static_cast<float>(header_h) - 8;
    bool new_card_being_dragged = sess->dragging && sess->drag_threshold_passed && sess->drag_is_new_card;
    if (!new_card_being_dragged) {
        Rectangle chip_rect{sess->new_card_chip_x, sess->new_card_chip_y, sess->new_card_chip_w, sess->new_card_chip_h};
        DrawRectangleRec(chip_rect, ResolveHlGroup("CursorLine"));
        DrawRectangleLinesEx(chip_rect, 1, ResolveHlGroup("Border"));
        DrawTextEx(g_font, "+ New Card", Vector2{chip_rect.x + 6, chip_rect.y + 3}, g_font_size * 0.9f, 0,
                   ResolveHlGroup("Normal"));
    }
    Rectangle add_col_rect{sess->new_card_chip_x + sess->new_card_chip_w + 8, y + 4, 90,
                            static_cast<float>(header_h) - 8};
    DrawRectangleRec(add_col_rect, ResolveHlGroup("CursorLine"));
    DrawRectangleLinesEx(add_col_rect, 1, ResolveHlGroup("Border"));
    DrawTextEx(g_font, "+ Column", Vector2{add_col_rect.x + 6, add_col_rect.y + 3}, g_font_size * 0.9f, 0,
               ResolveHlGroup("Normal"));
    int pane_id_for_add = pane.id;
    RegisterClickRegion(add_col_rect, [pane_id_for_add] {
        g_editor.FocusPaneById(pane_id_for_add);
        g_editor.BeginPromptNative("New column name", "", [](const std::string &name) {
            if (!name.empty()) g_editor.KanbanAddColumn(name);
        });
    });

    float col_header_y = y + header_h;
    float col_x = x;
    for (int ci = 0; ci < static_cast<int>(columns.size()); ci++) {
        float col_w = static_cast<float>(kKanbanColumnWidth);
        std::vector<int> cards = g_editor.KanbanCardsInColumn(pane.buffer_id, ci);

        bool column_being_dragged = sess->dragging && sess->drag_threshold_passed && sess->drag_is_column &&
                                    sess->drag_column_index == ci;
        DrawRectangle(static_cast<int>(col_x), static_cast<int>(col_header_y), static_cast<int>(col_w) - 4, header_h,
                      ResolveHlGroup(column_being_dragged ? "CursorLine" : "MenuBar"));
        std::string header_text = columns[ci] + " (" + std::to_string(cards.size()) + ")";
        BeginScissorMode(static_cast<int>(col_x), static_cast<int>(col_header_y), static_cast<int>(col_w) - 24,
                          header_h);
        DrawTextEx(g_font, header_text.c_str(), Vector2{col_x + 6, col_header_y + 4}, g_font_size, 0,
                   ResolveHlGroup("Normal"));
        EndScissorMode();
        if (ci > 0) {
            DrawLine(static_cast<int>(col_x), static_cast<int>(y), static_cast<int>(col_x), static_cast<int>(y + h),
                      ResolveHlGroup("Border"));
        }

        // Column menu ("..." -> Rename/Delete).
        Rectangle kebab_rect{col_x + col_w - 24, col_header_y + 4, 18, static_cast<float>(header_h) - 8};
        DrawTextEx(g_font, "...", Vector2{kebab_rect.x + 2, kebab_rect.y}, g_font_size, 0, ResolveHlGroup("Comment"));
        int pane_id_for_col = pane.id, col_i = ci;
        RegisterClickRegion(kebab_rect, [pane_id_for_col, col_i] {
            g_editor.FocusPaneById(pane_id_for_col);
            int buffer_id = g_editor.CurrentBufferId();
            if (g_kanban_col_menu_buffer == buffer_id && g_kanban_col_menu_col == col_i) {
                g_kanban_col_menu_buffer = -1;
                g_kanban_col_menu_col = -1;
            } else {
                g_kanban_col_menu_buffer = buffer_id;
                g_kanban_col_menu_col = col_i;
            }
        });
        if (g_kanban_col_menu_buffer == pane.buffer_id && g_kanban_col_menu_col == ci) {
            int item_h = MenuItemHeight();
            Rectangle popup_rect{col_x, col_header_y + header_h, 130, static_cast<float>(item_h) * 2};
            DrawRectangleRec(popup_rect, ResolveHlGroup("MenuBar"));
            DrawRectangleLinesEx(popup_rect, 1, ResolveHlGroup("Border"));
            Rectangle rename_row{col_x, col_header_y + header_h, 130, static_cast<float>(item_h)};
            Rectangle delete_row{col_x, col_header_y + header_h + item_h, 130, static_cast<float>(item_h)};
            DrawTextEx(g_font, "Rename", Vector2{rename_row.x + 8, rename_row.y + 4}, g_font_size, 0,
                       ResolveHlGroup("Normal"));
            DrawTextEx(g_font, "Delete", Vector2{delete_row.x + 8, delete_row.y + 4}, g_font_size, 0,
                       ResolveHlGroup("Normal"));
            std::string col_name = columns[ci];
            RegisterClickRegion(rename_row, [pane_id_for_col, col_i, col_name] {
                g_editor.FocusPaneById(pane_id_for_col);
                g_kanban_col_menu_buffer = -1;
                g_kanban_col_menu_col = -1;
                g_editor.BeginPromptNative("Rename column", col_name, [col_i](const std::string &name) {
                    if (!name.empty()) g_editor.KanbanRenameColumn(col_i, name);
                });
            });
            RegisterClickRegion(delete_row, [pane_id_for_col, col_i] {
                g_editor.FocusPaneById(pane_id_for_col);
                g_kanban_col_menu_buffer = -1;
                g_kanban_col_menu_col = -1;
                g_editor.KanbanDeleteColumn(col_i);
            });
        }

        float card_y = y + header_h * 2 + kKanbanCardGap;
        for (int ri = 0; ri < static_cast<int>(cards.size()); ri++) {
            int hi = cards[ri];
            const OrgHeadline &hd = sess->outline.headlines[hi];
            bool is_focused = is_active && ci == sess->focused_column && ri == sess->focused_row &&
                               g_editor.CurrentMode() == Mode::KanbanNormal;
            bool is_being_dragged = sess->dragging && sess->drag_threshold_passed && sess->drag_headline_index == hi;

            Rectangle card_rect{col_x + 4, card_y, col_w - 12, static_cast<float>(kKanbanCardHeight)};
            if (!is_being_dragged) {
                DrawRectangleRec(card_rect, ResolveHlGroup(is_focused ? "Visual" : "CursorLine"));
                if (is_focused) DrawRectangleLinesEx(card_rect, 2, ResolveHlGroup("BorderActive"));

                bool editing_this =
                    sess->editing && sess->editing_headline_index == hi && g_editor.CurrentMode() == Mode::KanbanInsert;
                std::string title_text = editing_this ? sess->edit_buffer : hd.title;
                std::string prio = hd.priority ? (std::string("[#") + hd.priority + "] ") : "";
                std::string line1 = prio + title_text;
                // Clipped to the card's own rect -- a title longer than the
                // column is wide would otherwise bleed into the next
                // column rather than just being cut off.
                BeginScissorMode(static_cast<int>(card_rect.x), static_cast<int>(card_rect.y),
                                  static_cast<int>(card_rect.width), static_cast<int>(card_rect.height));
                DrawTextEx(g_font, line1.c_str(), Vector2{card_rect.x + 6, card_rect.y + 4}, g_font_size, 0,
                           ResolveHlGroup("Normal"));
                if (!hd.tags.empty()) {
                    std::string tag_text;
                    for (const auto &t : hd.tags) tag_text += ":" + t;
                    tag_text += ":";
                    DrawTextEx(g_font, tag_text.c_str(), Vector2{card_rect.x + 6, card_rect.y + 4 + g_font_size + 4},
                               g_font_size * 0.85f, 0, ResolveHlGroup("Comment"));
                }
                if (editing_this && is_active) {
                    std::string pre = sess->edit_buffer.substr(0, std::min<size_t>(sess->edit_cursor, sess->edit_buffer.size()));
                    float cx = card_rect.x + 6 + MeasureTextEx(g_font, (prio + pre).c_str(), g_font_size, 0).x;
                    DrawRectangle(static_cast<int>(cx), static_cast<int>(card_rect.y + 4), 2,
                                  static_cast<int>(g_font_size), ResolveHlGroup("Normal"));
                }
                EndScissorMode();

                int pane_id = pane.id, col_i = ci, row_i = ri;
                RegisterClickRegion(card_rect, [pane_id, col_i, row_i] {
                    g_editor.FocusPaneById(pane_id);
                    if (KanbanSession *s = g_editor.GetKanbanMutable(g_editor.CurrentBufferId())) {
                        s->focused_column = col_i;
                        s->focused_row = row_i;
                    }
                });
            }
            card_y += kKanbanCardHeight + kKanbanCardGap;
        }

        // Drop-target preview strip while a card is dragged over this column.
        if (sess->dragging && sess->drag_threshold_passed && !sess->drag_is_column && sess->drop_column == ci) {
            float preview_y = y + header_h * 2 + kKanbanCardGap +
                               sess->drop_row * (kKanbanCardHeight + kKanbanCardGap) - kKanbanCardGap / 2.0f;
            DrawRectangle(static_cast<int>(col_x + 4), static_cast<int>(preview_y), static_cast<int>(col_w - 12), 3,
                          ResolveHlGroup("BorderActive"));
        }

        col_x += col_w;
    }

    // A column target is a gap, not another column: the vertical marker
    // makes it unambiguous whether the release will insert before or after
    // the header under the pointer.
    if (sess->dragging && sess->drag_threshold_passed && sess->drag_is_column &&
        sess->column_drop_slot >= 0 && sess->column_drop_slot <= static_cast<int>(columns.size())) {
        float marker_x = x + sess->column_drop_slot * kKanbanColumnWidth;
        DrawRectangle(static_cast<int>(marker_x) - 2, static_cast<int>(col_header_y), 4, header_h,
                      ResolveHlGroup("BorderActive"));
    }

    // The dragged card (or new-card ghost) itself, following the live mouse
    // position.
    if (sess->dragging && sess->drag_threshold_passed && !sess->drag_is_column) {
        std::string ghost_title = "New card";
        bool have_title = sess->drag_is_new_card;
        if (!have_title && sess->drag_headline_index >= 0 &&
            sess->drag_headline_index < static_cast<int>(sess->outline.headlines.size())) {
            ghost_title = sess->outline.headlines[sess->drag_headline_index].title;
            have_title = true;
        }
        if (have_title) {
            Vector2 mouse = GetMousePosition();
            Rectangle card_rect{mouse.x - 20, mouse.y - 12, static_cast<float>(kKanbanColumnWidth) - 12,
                                 static_cast<float>(kKanbanCardHeight)};
            DrawRectangleRec(card_rect, ResolveHlGroup("Visual"));
            DrawRectangleLinesEx(card_rect, 2, ResolveHlGroup("BorderActive"));
            BeginScissorMode(static_cast<int>(card_rect.x), static_cast<int>(card_rect.y),
                              static_cast<int>(card_rect.width), static_cast<int>(card_rect.height));
            DrawTextEx(g_font, ghost_title.c_str(), Vector2{card_rect.x + 6, card_rect.y + 4}, g_font_size, 0,
                       ResolveHlGroup("Normal"));
            EndScissorMode();
        }
    }
    if (sess->dragging && sess->drag_threshold_passed && sess->drag_is_column &&
        sess->drag_column_index >= 0 && sess->drag_column_index < static_cast<int>(columns.size())) {
        Vector2 mouse = GetMousePosition();
        Rectangle header_ghost{mouse.x - 20, mouse.y - static_cast<float>(header_h) / 2,
                               static_cast<float>(kKanbanColumnWidth) - 4, static_cast<float>(header_h)};
        DrawRectangleRec(header_ghost, ResolveHlGroup("Visual"));
        DrawRectangleLinesEx(header_ghost, 2, ResolveHlGroup("BorderActive"));
        DrawTextEx(g_font, columns[sess->drag_column_index].c_str(), Vector2{header_ghost.x + 6, header_ghost.y + 4},
                   g_font_size, 0, ResolveHlGroup("Normal"));
    }

    // Lowest-priority fallback: focus the pane and dismiss any open column
    // menu on a click that missed everything more specific above
    // (registered last, so first-match-wins ordering still gives every
    // card/button/popup-row its own click priority -- see DrawPane's own
    // kanban_or_gantt_active exclusion comment for why the generic
    // pane-body catch-all is skipped there instead of double-registering).
    RegisterClickRegion(Rectangle{x, y, w, h}, [pane_id = pane.id] {
        g_editor.FocusPaneById(pane_id);
        g_kanban_col_menu_buffer = -1;
        g_kanban_col_menu_col = -1;
    });

    EndScissorMode();
}

void DrawGanttDependencies(const GanttSession &sess, const std::vector<int> &rows, float timeline_x, float y, int row_h,
                           float ruler_h) {
    std::unordered_map<std::string, int> id_to_headline;
    std::unordered_map<int, int> headline_to_row;
    for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri) {
        headline_to_row[rows[ri]] = ri;
        const std::string &id = sess.outline.headlines[rows[ri]].id;
        if (!id.empty()) id_to_headline[id] = rows[ri];
    }
    Color arrow_color = Fade(ResolveHlGroup("BorderActive"), 0.8f);
    for (int target_hi : rows) {
        const OrgHeadline &target = sess.outline.headlines[target_hi];
        for (const std::string &blocker : target.blockers) {
            auto source_it = id_to_headline.find(blocker);
            if (source_it == id_to_headline.end()) continue;  // dependency outside this Gantt view
            const OrgHeadline &source = sess.outline.headlines[source_it->second];
            long long source_end = source.deadline.present ? OrgDayNumber(source.deadline.year, source.deadline.month, source.deadline.day)
                                                            : OrgDayNumber(source.scheduled.year, source.scheduled.month, source.scheduled.day);
            long long target_start = OrgDayNumber(target.scheduled.year, target.scheduled.month, target.scheduled.day);
            float sx = timeline_x + static_cast<float>(source_end - sess.anchor_day) * sess.pixels_per_day;
            float tx = timeline_x + static_cast<float>(target_start - sess.anchor_day) * sess.pixels_per_day;
            float sy = y + ruler_h + headline_to_row[source_it->second] * row_h + row_h / 2.0f;
            float ty = y + ruler_h + headline_to_row[target_hi] * row_h + row_h / 2.0f;
            float bend = std::max(sx + 12.0f, tx - 12.0f);
            DrawLineEx(Vector2{sx, sy}, Vector2{bend, sy}, 2, arrow_color);
            DrawLineEx(Vector2{bend, sy}, Vector2{bend, ty}, 2, arrow_color);
            DrawLineEx(Vector2{bend, ty}, Vector2{tx, ty}, 2, arrow_color);
            DrawTriangle(Vector2{tx, ty}, Vector2{tx - 7, ty - 4}, Vector2{tx - 7, ty + 4}, arrow_color);
        }
    }
}

void DrawGantt(const Pane &pane, float x, float y, float w, float h, bool is_active) {
    GanttSession *sess = g_editor.GetGanttMutable(pane.buffer_id);
    if (!sess) return;
    sess->content_x = x;
    sess->content_y = y;
    sess->content_w = w;
    sess->content_h = h;

    std::vector<int> rows = g_editor.GanttRows(pane.buffer_id);
    BeginScissorMode(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h));
    DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                  ResolveHlGroup("NormalBg"));

    float label_w = sess->label_col_w;
    float timeline_x = x + label_w;
    float timeline_w = std::max(0.0f, w - label_w);
    float ruler_h = static_cast<float>(GanttRulerHeight(*sess));
    int row_h = GanttRowHeight();

    int visible_days = std::max(1, static_cast<int>(timeline_w / sess->pixels_per_day) + 2);
    if (sess->ruler_scale == GanttSession::RulerScale::Months) {
        int anchor_year, anchor_month, anchor_date;
        OrgDateFromDayNumber(sess->anchor_day, anchor_year, anchor_month, anchor_date);
        long long last_day = sess->anchor_day + visible_days;
        long long month_start = OrgDayNumber(anchor_year, anchor_month, 1);
        for (int yy = anchor_year, mm = anchor_month; month_start <= last_day;) {
            int next_year = yy, next_month = mm + 1;
            if (next_month == 13) { next_month = 1; next_year++; }
            long long next_start = OrgDayNumber(next_year, next_month, 1);
            float gx = timeline_x + static_cast<float>(month_start - sess->anchor_day) * sess->pixels_per_day;
            float next_x = timeline_x + static_cast<float>(next_start - sess->anchor_day) * sess->pixels_per_day;
            DrawLine(static_cast<int>(gx), static_cast<int>(y), static_cast<int>(gx), static_cast<int>(y + h),
                     ResolveHlGroup("BorderActive"));
            const char *month_name = GanttMonthAbbrev(mm);
            float month_text_w = MeasureTextEx(g_font, month_name, g_font_size * 0.8f, 0).x;
            DrawTextEx(g_font, month_name, Vector2{(gx + next_x - month_text_w) / 2.0f, y + PaneHeaderHeight() + 4},
                       g_font_size * 0.8f, 0, ResolveHlGroup("Comment"));
            yy = next_year;
            mm = next_month;
            month_start = next_start;
        }
        // The year-cell loop starts one January before the viewport, so a
        // partially visible current year still gets a centered label.
        for (int yy = anchor_year; OrgDayNumber(yy, 1, 1) <= last_day; ++yy) {
            long long year_start = OrgDayNumber(yy, 1, 1), next_year_start = OrgDayNumber(yy + 1, 1, 1);
            float gx = timeline_x + static_cast<float>(year_start - sess->anchor_day) * sess->pixels_per_day;
            float next_x = timeline_x + static_cast<float>(next_year_start - sess->anchor_day) * sess->pixels_per_day;
            char year_text[8];
            std::snprintf(year_text, sizeof(year_text), "%04d", yy);
            float year_text_w = MeasureTextEx(g_font, year_text, g_font_size * 0.8f, 0).x;
            DrawTextEx(g_font, year_text, Vector2{(gx + next_x - year_text_w) / 2.0f, y + 4}, g_font_size * 0.8f, 0,
                       ResolveHlGroup("Comment"));
        }
        DrawLine(static_cast<int>(timeline_x), static_cast<int>(y + PaneHeaderHeight()), static_cast<int>(x + w),
                 static_cast<int>(y + PaneHeaderHeight()), ResolveHlGroup("Border"));
    } else {
        for (int i = 0; i < visible_days; i++) {
            long long day = sess->anchor_day + i;
            float gx = timeline_x + i * sess->pixels_per_day;
            int yy, mm, dd;
            OrgDateFromDayNumber(day, yy, mm, dd);
            bool year_boundary = dd == 1 && mm == 1;
            bool draw_gridline = sess->ruler_scale == GanttSession::RulerScale::Days || year_boundary;
            if (!draw_gridline) continue;
            DrawLine(static_cast<int>(gx), static_cast<int>(y + ruler_h), static_cast<int>(gx), static_cast<int>(y + h),
                     ResolveHlGroup(sess->ruler_scale == GanttSession::RulerScale::Days ? "Border" : "BorderActive"));
            if (sess->ruler_scale == GanttSession::RulerScale::Days && sess->pixels_per_day > 14.0f) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%02d", dd);
                DrawTextEx(g_font, buf, Vector2{gx + 2, y + 4}, g_font_size * 0.8f, 0, ResolveHlGroup("Comment"));
            } else if (sess->ruler_scale == GanttSession::RulerScale::Years) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%04d", yy);
                DrawTextEx(g_font, buf, Vector2{gx + 2, y + 4}, g_font_size * 0.8f, 0, ResolveHlGroup("Comment"));
            }
        }
    }
    // Label/timeline divider -- draggable (UpdateGanttMouseInteraction) to
    // resize label_col_w, drawn a bit heavier than the plain day-gridlines
    // above so it reads as a handle.
    DrawLine(static_cast<int>(timeline_x), static_cast<int>(y + ruler_h), static_cast<int>(x + w),
              static_cast<int>(y + ruler_h), ResolveHlGroup("Border"));
    long long today = GanttTodayDay();
    float today_x = timeline_x + static_cast<float>(today - sess->anchor_day) * sess->pixels_per_day;
    if (today_x >= timeline_x && today_x <= x + w) {
        Color today_color = ResolveHlGroup("Red");
        DrawRectangle(static_cast<int>(today_x) - 1, static_cast<int>(y), 3, static_cast<int>(h), Fade(today_color, 0.85f));
        DrawTextEx(g_font, "Today", Vector2{today_x + 4, y + 4}, g_font_size * 0.7f, 0, today_color);
    }
    bool divider_hot = sess->resizing_label_col;
    DrawRectangle(static_cast<int>(timeline_x) - 1, static_cast<int>(y), divider_hot ? 3 : 1, static_cast<int>(h),
                  ResolveHlGroup(divider_hot ? "BorderActive" : "Border"));

    // Draw below bars/labels so dependency arrows stay visible in the open
    // chart space but never cover a task's own interval or text.
    DrawGanttDependencies(*sess, rows, timeline_x, y, row_h, ruler_h);

    for (int ri = 0; ri < static_cast<int>(rows.size()); ri++) {
        int hi = rows[ri];
        const OrgHeadline &hd = sess->outline.headlines[hi];
        float row_y = y + ruler_h + ri * row_h;
        bool is_group = false;
        for (const OrgHeadline &candidate : sess->outline.headlines) {
            if (candidate.parent_index == hi) { is_group = true; break; }
        }
        bool is_focused = g_clean_gantt_export_buffer != pane.buffer_id && is_active && ri == sess->focused_row &&
                           (g_editor.CurrentMode() == Mode::GanttNormal || g_editor.CurrentMode() == Mode::GanttInsert);
        if (is_focused) {
            DrawRectangle(static_cast<int>(x), static_cast<int>(row_y), static_cast<int>(w), row_h,
                          ResolveHlGroup("CursorLine"));
        }

        bool editing_this =
            sess->editing && sess->editing_headline_index == hi && g_editor.CurrentMode() == Mode::GanttInsert;
        float indent = static_cast<float>(std::max(0, hd.level - 1)) * 14.0f;
        std::string label = is_group ? (sess->collapsed_headlines.count(hi) ? "+ " : "- ") : "  ";
        label += editing_this ? sess->edit_buffer : hd.title;
        BeginScissorMode(static_cast<int>(x), static_cast<int>(row_y), static_cast<int>(label_w), row_h);
        DrawTextEx(g_font, label.c_str(), Vector2{x + 6 + indent, row_y + 4}, g_font_size, 0, ResolveHlGroup("Normal"));
        if (is_group) DrawTextEx(g_font, label.c_str(), Vector2{x + 7 + indent, row_y + 4}, g_font_size, 0, ResolveHlGroup("Normal"));
        std::string date_range = "Start " + GanttDateLabel(hd.scheduled);
        if (hd.deadline.present) date_range += "  →  Due " + GanttDateLabel(hd.deadline);
        else date_range += "  →  Due —";
        if (!hd.assignee.empty()) date_range += "  |  " + hd.assignee;
        date_range += "  |  " + std::to_string(hd.progress) + "%";
        DrawTextEx(g_font, date_range.c_str(), Vector2{x + 6 + indent, row_y + g_font_size + 7}, g_font_size * 0.78f, 0,
                   ResolveHlGroup("Comment"));
        if (editing_this && is_active) {
            std::string indent(static_cast<size_t>(std::max(0, hd.level - 1)) * 2, ' ');
            std::string pre = sess->edit_buffer.substr(0, std::min<size_t>(sess->edit_cursor, sess->edit_buffer.size()));
            float cx = x + 6 + MeasureTextEx(g_font, (indent + pre).c_str(), g_font_size, 0).x;
            DrawRectangle(static_cast<int>(cx), static_cast<int>(row_y + 4), 2, static_cast<int>(g_font_size),
                          ResolveHlGroup("Normal"));
        }
        EndScissorMode();

        bool being_dragged = sess->dragging && sess->drag_headline_index == hi;
        OrgTimestamp live_scheduled = hd.scheduled, live_deadline = hd.deadline;
        if (being_dragged) {
            // Live preview follows the mouse -- computed the same way
            // UpdateGanttMouseInteraction commits it on release, so the
            // bar never visibly "snaps" at drop time.
            float dx = GetMousePosition().x - sess->drag_start_x;
            int delta_days = static_cast<int>(std::lround(dx / sess->pixels_per_day));
            if (sess->drag_mode == GanttSession::DragMode::Move) {
                live_scheduled = ShiftTimestamp(sess->drag_orig_scheduled, delta_days);
                if (sess->drag_orig_deadline.present) live_deadline = ShiftTimestamp(sess->drag_orig_deadline, delta_days);
            } else if (sess->drag_mode == GanttSession::DragMode::ResizeStart) {
                live_scheduled = ShiftTimestamp(sess->drag_orig_scheduled, delta_days);
            } else {
                OrgTimestamp base = sess->drag_orig_deadline.present ? sess->drag_orig_deadline : sess->drag_orig_scheduled;
                live_deadline = ShiftTimestamp(base, delta_days);
            }
        }

        long long start_day = OrgDayNumber(live_scheduled.year, live_scheduled.month, live_scheduled.day);
        float bar_x = timeline_x + static_cast<float>(start_day - sess->anchor_day) * sess->pixels_per_day;
        Color bar_color = hd.is_done_keyword ? ResolveHlGroup("Comment") : ResolveHlGroup("BorderActive");

        if (live_deadline.present) {
            long long end_day = OrgDayNumber(live_deadline.year, live_deadline.month, live_deadline.day);
            float bar_w = std::max(4.0f, static_cast<float>(end_day - start_day) * sess->pixels_per_day);
            Rectangle bar_rect{bar_x, row_y + (row_h - kGanttBarHeight) / 2.0f, bar_w, kGanttBarHeight};
            DrawRectangleRounded(bar_rect, 0.45f, 6, Fade(bar_color, 0.35f));
            if (hd.progress > 0) {
                Rectangle completed = bar_rect;
                completed.width = std::max(2.0f, bar_rect.width * hd.progress / 100.0f);
                DrawRectangleRounded(completed, 0.45f, 6, bar_color);
            }
            DrawRectangleRoundedLines(bar_rect, 0.45f, 6, is_group ? ResolveHlGroup("Normal") : bar_color);
            if (is_focused) DrawRectangleRoundedLinesEx(bar_rect, 0.45f, 6, 2, ResolveHlGroup("BorderActive"));
            if (bar_w >= 44.0f) {
                std::string progress = std::to_string(hd.progress) + "%";
                DrawTextEx(g_font, progress.c_str(), Vector2{bar_rect.x + 5, bar_rect.y - 2}, g_font_size * 0.58f, 0,
                           ResolveHlGroup("Normal"));
            }
            if (!being_dragged) {
                int pane_id = pane.id, row_i = ri;
                RegisterClickRegion(bar_rect, [pane_id, row_i] {
                    g_editor.FocusPaneById(pane_id);
                    if (GanttSession *s = g_editor.GetGanttMutable(g_editor.CurrentBufferId())) s->focused_row = row_i;
                });
            }
        } else if (hd.effort.has_value()) {
            // Effort-only duration -- read-only in v1 (documented gap: no
            // drag-resize since editing "H:MM" isn't supported yet).
            Rectangle bar_rect{bar_x, row_y + (row_h - kGanttBarHeight) / 2.0f, 40, kGanttBarHeight};
            DrawRectangleRounded(bar_rect, 0.45f, 6, Fade(bar_color, 0.6f));
        } else {
            // Milestone: SCHEDULED only, no duration info at all -- a
            // small filled circle (DrawCircle, not a hand-rolled diamond
            // via DrawTriangleFan: raylib's triangle winding-order
            // requirement made an earlier version of this invisible),
            // matching org-mode gantt exporters' own convention for a
            // zero-length task.
            float cy = row_y + row_h / 2.0f;
            DrawCircle(static_cast<int>(bar_x), static_cast<int>(cy), 6.0f, bar_color);
            if (!being_dragged) {
                int pane_id = pane.id, row_i = ri;
                Rectangle hit{bar_x - 6, cy - 6, 12, 12};
                RegisterClickRegion(hit, [pane_id, row_i] {
                    g_editor.FocusPaneById(pane_id);
                    if (GanttSession *s = g_editor.GetGanttMutable(g_editor.CurrentBufferId())) s->focused_row = row_i;
                });
            }
        }
    }

    RegisterClickRegion(Rectangle{x, y, w, h}, [pane_id = pane.id] { g_editor.FocusPaneById(pane_id); });

    EndScissorMode();
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
    const TerminalSession *term_sess = g_editor.GetTerminal(pane.buffer_id);
    const ImageSession *img_sess = g_editor.GetImage(pane.buffer_id);
    const PdfSession *pdf_sess = g_editor.GetPdf(pane.buffer_id);
    const OfficeSession *office_sess = g_editor.GetOffice(pane.buffer_id);
    if (office_sess) header_bg = ResolveHlGroup("MenuBar");
    const SheetSession *sheet_sess = g_editor.GetSheet(pane.buffer_id);
    const HtmlSession *html_sess = g_editor.GetHtml(pane.buffer_id);
    // Unlike every session pointer above (each keyed by buffer *identity*
    // -- a buffer can only ever be one of these), a Kanban/Gantt view is a
    // buffer *state* (org_view_mode_) layered over an ordinary org text
    // buffer -- hence gating on IsKanbanViewActive/IsGanttViewActive, not
    // just whether a cached session happens to exist for this buffer id
    // (see OrgViewMode's own comment, editor.h).
    KanbanSession *kanban_sess = g_editor.IsKanbanViewActive(pane.buffer_id) ? g_editor.GetKanbanMutable(pane.buffer_id) : nullptr;
    GanttSession *gantt_sess = g_editor.IsGanttViewActive(pane.buffer_id) ? g_editor.GetGanttMutable(pane.buffer_id) : nullptr;
    std::string suffix = buf.modified ? " [+]" : "";
    float label_y = y + (header_h - font_size) / 2.0f;
    if (pane.buffer_tabs.size() > 1) {
        // Per-pane buffer-tab strip: more than one buffer open in this pane
        // splits the header evenly, one filename chip per tab, highlighting
        // whichever is active -- takes over the header entirely regardless
        // of the active buffer's content type (the content body below is
        // still whatever that buffer actually is; only the header changes
        // shape). Same even-split arithmetic as DrawPaneTree's own child
        // rects, so segments abut exactly with no rounding gaps.
        int n = static_cast<int>(pane.buffer_tabs.size());
        float seg_x = x;
        for (int i = 0; i < n; i++) {
            float next_x = (i == n - 1) ? x + w : x + w * (i + 1) / n;
            float seg_w = next_x - seg_x;
            const Buffer &tb = g_editor.GetBuffer(pane.buffer_tabs[i]);
            std::string name = tb.scratch ? "[Scratch]" : (tb.filename.empty() ? "[No Name]" : Basename(tb.filename));
            if (tb.modified) name += " [+]";
            bool tab_active = (i == pane.buffer_tab_index);
            Color seg_bg =
                tab_active ? ResolveHlGroup("TabActive") : (is_active ? ResolveHlGroup("TabInactive") : ResolveHlGroup("MenuBar"));
            DrawRectangle(static_cast<int>(seg_x), static_cast<int>(y), static_cast<int>(seg_w), header_h, seg_bg);
            BeginScissorMode(static_cast<int>(seg_x), static_cast<int>(y), static_cast<int>(seg_w), header_h);
            float text_w = MeasureTextEx(g_font, name.c_str(), font_size, 0).x;
            float text_x = seg_x + std::max(0.0f, (seg_w - text_w) / 2.0f);
            DrawTextEx(g_font, name.c_str(), Vector2{text_x, label_y}, font_size, 0,
                       ResolveHlGroup(tab_active ? "Normal" : "Comment"));
            EndScissorMode();
            if (i > 0) {
                DrawLine(static_cast<int>(seg_x), static_cast<int>(y), static_cast<int>(seg_x),
                          static_cast<int>(y + header_h), ResolveHlGroup("Border"));
            }
            // Registered regardless of is_active now (not just the
            // already-focused pane): the click action focuses `pane.id`
            // *first*, so CurPane() (what GoToPaneBufferTab acts on)
            // always resolves to this pane by the time it runs, whether
            // or not it already had focus -- lets clicking a chip on any
            // pane both switch to and jump straight into that tab in one
            // click. Also pushed to g_pane_tab_chip_rects (main.cpp's own
            // pane-mouse-interaction system, not g_click_regions) so it's
            // draggable too.
            {
                Rectangle chip_rect{seg_x, y, seg_w, static_cast<float>(header_h)};
                g_pane_tab_chip_rects.push_back({pane.id, pane.buffer_tabs[i], chip_rect});
                int pane_id = pane.id;
                RegisterClickRegion(chip_rect, [pane_id, i] {
                    g_editor.FocusPaneById(pane_id);
                    g_editor.GoToPaneBufferTab(i);
                });
            }
            seg_x = next_x;
        }
    } else {
        DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), header_h, header_bg);
        // A single-buffer pane's whole plain header counts as "the tab"
        // for click-to-focus and drag-and-drop purposes too, not just a
        // multi-tab strip's own chips -- there's still exactly one
        // buffer to drag out, it's just not drawn as a chip when there's
        // nothing else to distinguish it from.
        {
            // Gantt's later SVG/PNG/PDF controls occupy the rightmost 132px
            // and must not be shadowed by this generic focus/tab chip.
            float header_click_w = gantt_sess ? std::max(0.0f, w - 132.0f) : w;
            Rectangle header_rect{x, y, header_click_w, static_cast<float>(header_h)};
            g_pane_tab_chip_rects.push_back({pane.id, pane.buffer_id, header_rect});
            int pane_id = pane.id;
            RegisterClickRegion(header_rect, [pane_id] { g_editor.FocusPaneById(pane_id); });
        }
        if (term_sess) {
            const std::string &live_title =
                (term_sess->vterm && !term_sess->vterm->Title().empty()) ? term_sess->vterm->Title() : term_sess->title;
            std::string label = "Terminal: " + live_title;
            if (term_sess->exited) label += " [exited: " + std::to_string(term_sess->exit_code) + "]";
            DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        } else if (img_sess) {
            std::string label = "Image: " + buf.filename;
            if (img_sess->doc) {
                label += " (" + std::to_string(img_sess->doc->Width()) + "x" +
                          std::to_string(img_sess->doc->Height()) + ") " +
                          std::to_string(static_cast<int>(img_sess->zoom * 100.0f + 0.5f)) + "%";
            }
            DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        } else if (html_sess) {
            std::string title = html_sess->doc.title.empty() ? html_sess->source : html_sess->doc.title;
            std::string label = "HTML: " + title + "  " + std::to_string(static_cast<int>(html_sess->zoom * 100.0f + 0.5f)) + "%" +
                                 (html_sess->theme_colors ? "  [theme, Ctrl-R]" : "  [page colors, Ctrl-R]");
            DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        } else if (pdf_sess && pdf_sess->search_active) {
            // Takes over the header the same way Mode::Command's cmdline
            // takes over the bottom bar -- a blinking-cursor '/' input line
            // instead of the normal "PDF: file (page N/M) zoom%" label
            // while typing.
            std::string line = "/" + pdf_sess->search_input;
            DrawTextEx(g_font, line.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
            {
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
            // Just the filename -- no more "(para X/Y) Z%" (the Docs-style
            // status line below now carries page/word-count/zoom instead).
            std::string label = (buf.filename.empty() ? "Untitled Document" : buf.filename) + (buf.modified ? " [+]" : "");
            Vector2 ts = MeasureTextEx(g_font, label.c_str(), font_size, 0);
            DrawTextEx(g_font, label.c_str(), Vector2{x + std::max(6.0f, (w - ts.x) / 2.0f), label_y}, font_size, 0,
                       ResolveHlGroup("Normal"));
        } else if (sheet_sess) {
            std::string label = "Sheet: " + buf.filename;
            if (!sheet_sess->wb.sheets.empty()) {
                const Sheet &sh = sheet_sess->wb.sheets[sheet_sess->active_sheet];
                label += " (" + sh.name + ") " + CellAddressToString(sheet_sess->cursor_row, sheet_sess->cursor_col);
            }
            if (buf.modified) label += " [+]";
            DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        } else if (kanban_sess) {
            std::string label = "Kanban: " + buf.filename;
            if (buf.modified) label += " [+]";
            DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        } else if (gantt_sess) {
            char zoom_buf[16];
            std::snprintf(zoom_buf, sizeof(zoom_buf), "%.0fpx/day", gantt_sess->pixels_per_day);
            std::string label = "Gantt: " + buf.filename + "  " + GanttRulerScaleName(gantt_sess->ruler_scale) +
                                " grid (t)  " + zoom_buf + "  f:fit p:progress za/zm:fold";
            if (buf.modified) label += " [+]";
            DrawTextEx(g_font, label.c_str(), Vector2{x + 6, label_y}, font_size, 0, ResolveHlGroup("Normal"));
            // Export controls live in the pane header, outside the Gantt
            // canvas itself, so PNG/PDF screenshots contain only the chart.
            constexpr const char *formats[] = {"SVG", "PNG", "PDF"};
            float export_x = x + w - 132;
            for (int i = 0; i < 3; ++i) {
                Rectangle button{export_x + i * 44.0f, static_cast<float>(y) + 3, 40, static_cast<float>(header_h) - 6};
                DrawRectangleRec(button, ResolveHlGroup("CursorLine"));
                DrawRectangleLinesEx(button, 1, ResolveHlGroup("Border"));
                DrawTextEx(g_font, formats[i], Vector2{button.x + 4, button.y + 2}, font_size * 0.75f, 0,
                           ResolveHlGroup("Normal"));
                int export_buffer_id = pane.buffer_id;
                std::string format = i == 0 ? "svg" : (i == 1 ? "png" : "pdf");
                RegisterClickRegion(button, [export_buffer_id, format] {
                    if (format == "svg") {
                        std::string path = GanttExportPath(export_buffer_id, "svg");
                        bool ok = ExportGanttSvg(export_buffer_id, path);
                        g_editor.SetStatusMessage(ok ? "Exported Gantt to " + path : "Gantt export failed: " + path);
                    } else {
                        g_pending_gantt_raster_export = {export_buffer_id, format};
                        g_clean_gantt_export_buffer = export_buffer_id;
                        g_editor.SetStatusMessage("Exporting Gantt...");
                    }
                });
            }
        } else {
            // Just the filename (or [Scratch]/[No Name]), centered -- not
            // the full path anymore: the old winbar-equivalent breadcrumb
            // (each path component its own clickable segment, calling the
            // registered winbar-click Lua ref -- mep.winbar_navigate by
            // default, kBuiltinPickerSources) got too noisy for a narrow
            // split's header. mep.set_winbar_click/WinbarClickRef are still
            // there for a future consumer; nothing fires them from here now.
            std::string label = (buf.scratch ? "[Scratch]" : buf.filename.empty() ? "[No Name]" : Basename(buf.filename)) + suffix;
            float text_w = MeasureTextEx(g_font, label.c_str(), font_size, 0).x;
            float text_x = x + std::max(0.0f, (w - text_w) / 2.0f);
            DrawTextEx(g_font, label.c_str(), Vector2{text_x, label_y}, font_size, 0, ResolveHlGroup("Normal"));
        }
    }

    float content_y = y + header_h;
    float content_h = h - header_h;

    // Click anywhere in the pane's own content area (below the header,
    // which already has its own focus-on-click handling above) to focus
    // this pane -- deliberately focus-only, not click-to-place-cursor
    // (no such feature exists yet in any content-type branch below; this
    // register call doesn't interfere with one being added later, since
    // g_click_regions just fires every matching region's action, and a
    // future click-to-place-cursor handler would be a second, more
    // specific region layered on top). Registered unconditionally,
    // regardless of buffer/content type, same as the header registration.
    //
    // Excludes the office pane's own toolbar rows (its own real height
    // computed the identical way the office branch below computes
    // toolbar_h, before that branch's own more specific button regions
    // register): g_click_regions is first-match-wins by registration
    // order, and this catch-all registers *before* any content-type
    // branch's own buttons -- left covering the toolbar's own pixels, it
    // would silently win over every toolbar button underneath it,
    // dispatching FocusPaneById instead (a real, latent bug caught live
    // under Xvfb -- clicking any toolbar button just no-op'd invisibly).
    // While an office toolbar dropdown is open, its popup (content-area
    // pixels, but registered much later -- after the document's own paint
    // pass) would otherwise lose every click to this earlier-registered
    // catch-all under the same first-match-wins ordering as the toolbar-vs-
    // catch-all bug above. Skip registering it entirely in that case so
    // popup item clicks (registered later) are the only match.
    bool office_dropdown_open = office_sess && !office_sess->doc.paragraphs.empty() && g_office_dropdown_open != -1;
    // Same reasoning as office_dropdown_open just above: DrawKanban/
    // DrawGantt (below) register their own per-card/per-bar click regions,
    // *and* their own whole-content-area fallback focus click (registered
    // last, so it only wins where nothing more specific does) -- this
    // catch-all registering first here would otherwise shadow every one of
    // those under the same first-match-wins ordering.
    bool kanban_or_gantt_active = kanban_sess || gantt_sess;
    if (!office_dropdown_open && !kanban_or_gantt_active) {
        float focus_click_x = x, focus_click_y = content_y, focus_click_w = w, focus_click_h = content_h;
        if (office_sess && !office_sess->doc.paragraphs.empty()) {
            float office_toolbar_h = static_cast<float>(header_h) * 2.0f;
            focus_click_y += office_toolbar_h;
            focus_click_h = std::max(0.0f, focus_click_h - office_toolbar_h);
            // Also exclude the footer bar (bottom) and the rail/Outline/
            // Format panels (left/right) -- same reasoning as the toolbar
            // exclusion just above: this catch-all registers before those
            // widgets' own more specific click regions (inside the real
            // office branch below), so first-match-wins would otherwise
            // swallow every click on them.
            focus_click_h = std::max(0.0f, focus_click_h - kOfficeFooterH);
            float left_excl = kOfficeRailW + (g_office_outline_open ? kOfficeOutlineW : 0.0f);
            float right_excl = g_office_format_open ? kOfficeFormatW : 0.0f;
            focus_click_x += left_excl;
            focus_click_w = std::max(0.0f, focus_click_w - left_excl - right_excl);
        }
        RegisterClickRegion(Rectangle{focus_click_x, focus_click_y, focus_click_w, focus_click_h},
                            [pane_id = pane.id] { g_editor.FocusPaneById(pane_id); });
    }

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
            DrawPaneBorder(x, y, w, h, is_active);
            return;
        }
        // Falls through to the ordinary buffer-drawing path below, same
        // as any other buffer.
    }

    // Unlike Sheet/Office (whose Buffer::lines is a dummy single empty
    // line, so falling through to the ordinary text-drawing path at the
    // end of this function is harmless), a Kanban/Gantt buffer's `lines`
    // is the real, multi-line org text -- so these draw their own content
    // and return early, exactly like the Terminal/Image blocks above,
    // rather than letting the plain-text renderer draw underneath/after.
    if (kanban_sess) {
        DrawKanban(pane, x, content_y, w, content_h, is_active);
        DrawPaneBorder(x, y, w, h, is_active);
        return;
    }
    if (gantt_sess) {
        DrawGantt(pane, x, content_y, w, content_h, is_active);
        DrawPaneBorder(x, y, w, h, is_active);
        return;
    }

    if (img_sess && img_sess->doc) {
        g_editor.ResizeImageViewport(pane.buffer_id, static_cast<int>(w), static_cast<int>(content_h));
        Texture2D tex = GetOrLoadImageTexture(pane.buffer_id, *img_sess);
        BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                          static_cast<int>(content_h));
        DrawTextureEx(tex, Vector2{x - img_sess->pan_x, content_y - img_sess->pan_y}, 0.0f, img_sess->zoom, WHITE);
        EndScissorMode();
        DrawPaneBorder(x, y, w, h, is_active);
        return;
    }

    if (html_sess) {
        g_editor.ResizeHtmlViewport(pane.buffer_id, static_cast<int>(w), static_cast<int>(content_h));
        constexpr float kHtmlPad = 12.0f;
        HtmlLayoutCtx ctx{std::max(50.0f, w - kHtmlPad * 2.0f), g_font_size * html_sess->zoom, ResolveHlGroup("Normal")};
        // <img src="relative/path"> resolves against the open file's own
        // directory (a remote source -- opened via mep.browse_command's own
        // curl-to-tempfile fetch -- has no useful "directory" of its own for
        // this, so its images just fall back to the bracketed placeholder;
        // see ResolveHtmlImagePath's own header).
        ctx.base_dir = std::filesystem::path(html_sess->source).parent_path().string();
        ctx.zoom = html_sess->zoom;
        HtmlLayout layout = LayoutHtmlDoc(html_sess->doc, ctx);
        // Layout depends on real font metrics (MeasureTextEx), so unlike
        // ResizePdfViewport's pure-geometry clamp, the max scroll_y this
        // frame's own content actually supports can only be known here,
        // after LayoutHtmlDoc runs -- see ClampHtmlScroll's own comment.
        g_editor.ClampHtmlScroll(pane.buffer_id, std::max(0.0f, layout.total_height - content_h));

        BeginScissorMode(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                          static_cast<int>(content_h));
        DrawRectangle(static_cast<int>(x), static_cast<int>(content_y), static_cast<int>(w),
                      static_cast<int>(content_h), ResolveHlGroup("NormalBg"));
        float top = content_y - html_sess->scroll_y;
        bool theme = html_sess->theme_colors;
        // Element background-color boxes (layout.backgrounds, pushed in
        // document order by HtmlLayoutBlock) -- drawn before every other
        // layer below so glyphs/rules/images always land on top of their
        // own box, never under it. Front-to-back in document order also
        // means a nested box with its own distinct background (e.g.
        // mep_org_html_code_block's own header-over-body split, main.cpp's
        // kBuiltinOrgExport) paints over its ancestor's, matching real
        // stacking order despite this renderer having no z-index concept.
        // Skipped entirely in theme mode -- the base NormalBg fill just
        // above already covers the whole pane, so a page's own background
        // boxes would otherwise paint page-colored islands over it.
        if (!theme) {
            for (const HtmlBgRect &bg : layout.backgrounds) {
                float ry = top + bg.y;
                if (ry + bg.h < content_y || ry > content_y + content_h) continue;
                DrawRectangle(static_cast<int>(x + kHtmlPad + bg.x), static_cast<int>(ry), static_cast<int>(bg.w),
                              static_cast<int>(bg.h), bg.color);
            }
        }
        // Element border boxes (layout.borders, same document-order/paint-
        // order reasoning as layout.backgrounds just above) -- each edge is
        // its own filled rectangle rather than DrawRectangleLines, since
        // the four widths/colors can all differ (a plain DrawRectangleLines
        // call has one uniform width/color for all four sides). In theme
        // mode every edge uses the same theme border color instead of its
        // own CSS color, matching HtmlRule's ("<hr>") own always-theme-
        // colored line just below (which never had a CSS color to begin
        // with, so it needs no such branch itself).
        Color theme_border = ResolveHlGroup("Border");
        for (const HtmlBorderRect &br : layout.borders) {
            float ry = top + br.y;
            if (ry + br.h < content_y || ry > content_y + content_h) continue;
            float bx = x + kHtmlPad + br.x;
            if (br.top_w > 0.0f) DrawRectangle(static_cast<int>(bx), static_cast<int>(ry), static_cast<int>(br.w),
                                                static_cast<int>(br.top_w), theme ? theme_border : br.top_c);
            if (br.bottom_w > 0.0f)
                DrawRectangle(static_cast<int>(bx), static_cast<int>(ry + br.h - br.bottom_w), static_cast<int>(br.w),
                              static_cast<int>(br.bottom_w), theme ? theme_border : br.bottom_c);
            if (br.left_w > 0.0f) DrawRectangle(static_cast<int>(bx), static_cast<int>(ry), static_cast<int>(br.left_w),
                                                 static_cast<int>(br.h), theme ? theme_border : br.left_c);
            if (br.right_w > 0.0f)
                DrawRectangle(static_cast<int>(bx + br.w - br.right_w), static_cast<int>(ry),
                              static_cast<int>(br.right_w), static_cast<int>(br.h), theme ? theme_border : br.right_c);
        }
        for (const HtmlRule &r : layout.rules) {
            float ry = top + r.y;
            if (ry < content_y - 4 || ry > content_y + content_h + 4) continue;
            DrawLine(static_cast<int>(x + kHtmlPad + r.x), static_cast<int>(ry),
                      static_cast<int>(x + kHtmlPad + r.x + r.w), static_cast<int>(ry), ResolveHlGroup("Border"));
        }
        Color theme_fg = ResolveHlGroup("Normal");
        for (const HtmlRun &run : layout.runs) {
            float ry = top + run.y;
            if (ry + run.font_size < content_y || ry > content_y + content_h) continue;  // cheap vertical culling
            if (!theme) {
                DrawHtmlRun(x + kHtmlPad + run.x, ry, run);
            } else {
                HtmlRun themed_run = run;
                themed_run.color = theme_fg;
                DrawHtmlRun(x + kHtmlPad + run.x, ry, themed_run);
            }
        }
        for (const HtmlImageRun &img : layout.images) {
            float ry = top + img.y;
            if (ry + img.h < content_y || ry > content_y + content_h) continue;
            Texture2D *tex = theme ? GetOrLoadThemedHtmlImageTexture(img.path) : GetOrLoadOrgInlineImageTexture(img.path);
            if (!tex) continue;  // e.g. the file was removed/moved since layout ran this same frame
            Rectangle src{0, 0, static_cast<float>(tex->width), static_cast<float>(tex->height)};
            Rectangle dst{x + kHtmlPad + img.x, ry, img.w, img.h};
            DrawTexturePro(*tex, src, dst, Vector2{0, 0}, 0.0f, WHITE);
        }
        for (const HtmlMathRun &m : layout.math_runs) {
            float ry = top + m.y;
            if (ry + m.layout.height < content_y || ry > content_y + content_h) continue;
            DrawMathLayout(x + kHtmlPad + m.x, ry, m.layout, theme ? theme_fg : m.color);
        }
        EndScissorMode();
        DrawPaneBorder(x, y, w, h, is_active);
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

        DrawPaneBorder(x, y, w, h, is_active);
        return;
    }

    if (office_sess && !office_sess->doc.paragraphs.empty()) {
        // Toolbar: 2 rows (row 1 formatting -- font family/size, B/I/U/S,
        // alignment, superscript/subscript, text/highlight color; row 2
        // insert/actions -- bullet/numbered list, special chars, math,
        // table, image, undo/redo, zoom) reflecting the format at the
        // cursor (OfficeNormal) or uniformly across the selection
        // (OfficeVisual), same mouse-click-equivalent-of-a-keybinding
        // shape the original single-row B/I/U toolbar established.
        // Shrinks content_y/content_h in place (safe: every path through
        // this office branch ends in `return`, so nothing after it in
        // DrawPane reads the pre-toolbar values for this call).
        // Keep the command ribbon deliberately compact: it should support
        // writing without competing with the page for vertical space.
        //
        // Docs-style status footer: reserved from the BOTTOM of *this
        // pane's own* content area, not the app's global status line --
        // each office pane gets its own, the same way its toolbar already
        // is per-pane rather than shared chrome.
        content_h = std::max(0.0f, content_h - kOfficeFooterH);
        float office_footer_y = y + h - kOfficeFooterH;
        // Outline (left) icon-rail/panel and Format/Insert (right) panel --
        // docked to the sides of *this pane*, not the app window (same
        // reasoning as the footer above). Returns the horizontal band
        // (ocx/ocw) left over for the toolbar and page to actually use.
        float ocx = x, ocw = w;
        DrawOfficeSidePanels(x, w, content_y, content_h, pane.buffer_id, office_sess, ocx, ocw);

        float row_h = std::clamp(static_cast<float>(header_h) * 0.78f, 30.0f, 38.0f);
        float toolbar_h = row_h * 2.0f;
        DrawRectangle(static_cast<int>(ocx), static_cast<int>(content_y), static_cast<int>(ocw),
                      static_cast<int>(toolbar_h), ResolveHlGroup("MenuBar"));
        DrawRectangle(static_cast<int>(ocx), static_cast<int>(content_y + toolbar_h - 1.0f), static_cast<int>(ocw), 1,
                      ResolveHlGroup("Border"));
        float btn_h = row_h - 6.0f;
        float row1_y = content_y + 3.0f;
        float row2_y = content_y + row_h + 3.0f;
        float row1_x = ocx + 8, row2_x = ocx + 8;

        // Shared button chrome: no fill at rest, a light hover tint, a
        // blue tint when toggled on -- draws the background and returns
        // the color the caller's own content (text or hand-drawn icon)
        // should use, so every button (text-label or icon) stays visually
        // consistent without duplicating this logic per button kind.
        auto btn_bg = [&](Rectangle rect, bool active) -> Color {
            bool hovered = CheckCollisionPointRec(GetMousePosition(), rect);
            if (active) DrawRectangleRounded(rect, 0.25f, 6, ResolveHlGroup("AccentTint"));
            else if (hovered) DrawRectangleRounded(rect, 0.25f, 6, ResolveHlGroup("CursorLine"));
            return active ? ResolveHlGroup("Accent") : ResolveHlGroup("Normal");
        };

        // Draws one text-label button at (*bx, by), advancing *bx past it;
        // returns the button's own rect (callers open a dropdown directly
        // below it).
        auto draw_btn = [&](float *bx, float by, const std::string &label, bool active,
                             const std::function<void()> &on_click) -> Rectangle {
            Vector2 ls = MeasureTextEx(g_font, label.c_str(), font_size * 0.92f, 0);
            float bw = ls.x + 14.0f;
            Rectangle rect{*bx, by, bw, btn_h};
            Color content = btn_bg(rect, active);
            DrawTextEx(g_font, label.c_str(),
                      Vector2{rect.x + (rect.width - ls.x) / 2.0f, rect.y + (rect.height - font_size * 0.92f) / 2.0f},
                      font_size * 0.92f, 0, content);
            RegisterClickRegion(rect, on_click);
            *bx += bw + 2.0f;
            return rect;
        };

        // Same button chrome, but the content is hand-drawn (raylib
        // primitives) rather than text -- no icon font glyphs exist for
        // any of these (checked before writing this: the embedded icon
        // font only has file-type + a dozen status glyphs, see the WYSIWYG
        // restyle plan), so bold/italic/underline/strike still draw as
        // real styled letters, and align/list/undo/redo/color/table/image
        // draw as small vector shapes sized to the button rect.
        auto draw_icon_btn = [&](float *bx, float by, float bw, bool active,
                                  const std::function<void(Rectangle, Color)> &draw_content,
                                  const std::function<void()> &on_click) -> Rectangle {
            Rectangle rect{*bx, by, bw, btn_h};
            Color content = btn_bg(rect, active);
            draw_content(rect, content);
            RegisterClickRegion(rect, on_click);
            *bx += bw + 2.0f;
            return rect;
        };

        auto add_sep = [&](float *bx, float by) {
            *bx += 5.0f;
            DrawLineEx(Vector2{*bx, by + 4.0f}, Vector2{*bx, by + btn_h - 4.0f}, 1.0f, ResolveHlGroup("Border"));
            *bx += 9.0f;
        };

        // Text-label button with a small hand-drawn chevron -- g_font only
        // has ASCII glyphs loaded (LoadFontFromMemory with nullptr
        // codepoints, ApplyFontSize above), so a unicode "\xE2\x96\xBE"
        // chevron in the label string rendered as a tofu '?' glyph; a
        // drawn triangle sidesteps font coverage entirely.
        auto draw_dropdown_btn = [&](float *bx, float by, const std::string &label, bool active,
                                      const std::function<void()> &on_click) -> Rectangle {
            Vector2 ls = MeasureTextEx(g_font, label.c_str(), font_size * 0.92f, 0);
            float bw = ls.x + 22.0f;
            return draw_icon_btn(bx, by, bw, active,
                [&, label, ls](Rectangle rect, Color color) {
                    DrawTextEx(g_font, label.c_str(),
                              Vector2{rect.x + 7.0f, rect.y + (rect.height - font_size * 0.92f) / 2.0f},
                              font_size * 0.92f, 0, color);
                    float tx = rect.x + rect.width - 12.0f, ty = rect.y + rect.height / 2.0f;
                    // Vertex order matters here -- DrawTriangle culls the
                    // "wrong" winding (verified against this file's other,
                    // already-working DrawTriangle calls): (right, left,
                    // bottom) renders, (left, right, bottom) silently does
                    // not.
                    DrawTriangle(Vector2{tx + 4.0f, ty - 2.0f}, Vector2{tx - 4.0f, ty - 2.0f}, Vector2{tx, ty + 3.0f}, color);
                },
                on_click);
        };

        // --- Row 1: font family/size, B/I/U/S, alignment, super/subscript, colors ---
        Rectangle font_family_btn =
            draw_dropdown_btn(&row1_x, row1_y, "Font", g_office_dropdown_open == kOfficeDropdownFontFamily, [] {
                g_office_dropdown_open = g_office_dropdown_open == kOfficeDropdownFontFamily ? -1 : kOfficeDropdownFontFamily;
            });
        Rectangle font_size_btn =
            draw_dropdown_btn(&row1_x, row1_y, "Size", g_office_dropdown_open == kOfficeDropdownFontSize, [] {
                g_office_dropdown_open = g_office_dropdown_open == kOfficeDropdownFontSize ? -1 : kOfficeDropdownFontSize;
            });
        add_sep(&row1_x, row1_y);
        // B/I/U/S: real styled glyphs, not just letters -- bold draws a
        // faux-bold double-strike, italic reuses the same rlgl shear
        // technique DrawMathLayout/DrawHtmlRun already use for slanted
        // text, underline/strike draw a line at the letter's own width.
        struct StyleBtn { char which; const char *ch; bool bold, italic, underline, strike; };
        static const StyleBtn kStyleBtns[] = {
            {'b', "B", true, false, false, false}, {'i', "I", false, true, false, false},
            {'u', "U", false, false, true, false}, {'s', "S", false, false, false, true}};
        for (const StyleBtn &sb : kStyleBtns) {
            char which = sb.which;
            draw_icon_btn(&row1_x, row1_y, btn_h, g_editor.OfficeFormatActive(which),
                [&, sb](Rectangle rect, Color color) {
                    Vector2 ts = MeasureTextEx(g_font, sb.ch, font_size, 0);
                    Vector2 pos{rect.x + (rect.width - ts.x) / 2.0f, rect.y + (rect.height - font_size) / 2.0f};
                    if (sb.italic) {
                        rlPushMatrix();
                        float baseline_y = pos.y + font_size;
                        rlTranslatef(pos.x, baseline_y, 0);
                        float shear[16] = {1.0f, 0.0f, 0.0f, 0.0f, -0.22f, 1.0f, 0.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f, 0.0f, 0.0f,   0.0f, 0.0f, 1.0f};
                        rlMultMatrixf(shear);
                        rlTranslatef(-pos.x, -baseline_y, 0);
                        DrawTextEx(g_font, sb.ch, pos, font_size, 0, color);
                        rlPopMatrix();
                    } else {
                        DrawTextEx(g_font, sb.ch, pos, font_size, 0, color);
                        if (sb.bold) DrawTextEx(g_font, sb.ch, Vector2{pos.x + 0.7f, pos.y}, font_size, 0, color);
                    }
                    if (sb.underline) {
                        float uy = pos.y + font_size * 0.95f;
                        DrawLineEx(Vector2{pos.x, uy}, Vector2{pos.x + ts.x, uy}, 1.3f, color);
                    }
                    if (sb.strike) {
                        float sy = pos.y + font_size * 0.52f;
                        DrawLineEx(Vector2{pos.x, sy}, Vector2{pos.x + ts.x, sy}, 1.3f, color);
                    }
                },
                [which] { g_editor.ToggleOfficeFormat(which); });
        }
        add_sep(&row1_x, row1_y);
        // Alignment: four small horizontal bars, shaped/positioned per
        // kind (left/center/right-anchored, or all full-width for justify)
        // -- the standard alignment-icon visual language, hand-drawn since
        // no icon font glyph exists for it.
        struct AlignBtn { DocParagraph::Align align; int kind; };
        static const AlignBtn kAlignBtns[] = {
            {DocParagraph::Align::Left, 0}, {DocParagraph::Align::Center, 1},
            {DocParagraph::Align::Right, 2}, {DocParagraph::Align::Justify, 3}};
        for (const AlignBtn &ab : kAlignBtns) {
            DocParagraph::Align align = ab.align;
            int kind = ab.kind;
            draw_icon_btn(&row1_x, row1_y, btn_h, g_editor.OfficeAlignmentActive(align),
                [kind](Rectangle rect, Color color) {
                    float pad = rect.width * 0.2f;
                    float full = rect.width - 2.0f * pad;
                    float bar_h = 2.0f;
                    float gap = (rect.height - 4.0f * bar_h) / 5.0f;
                    float widths[4] = {full, full * 0.62f, full, full * 0.8f};
                    for (int i = 0; i < 4; i++) {
                        float bw = (kind == 3) ? full : widths[i];
                        float by = rect.y + gap * static_cast<float>(i + 1) + bar_h * static_cast<float>(i);
                        float bx0 = rect.x + pad;
                        if (kind == 1) bx0 = rect.x + (rect.width - bw) / 2.0f;
                        else if (kind == 2) bx0 = rect.x + rect.width - pad - bw;
                        DrawRectangle(static_cast<int>(bx0), static_cast<int>(by), static_cast<int>(bw),
                                      static_cast<int>(bar_h), color);
                    }
                },
                [align] { g_editor.SetOfficeAlignment(align); });
        }
        add_sep(&row1_x, row1_y);
        // Superscript/subscript: a plain "x" plus a smaller raised/lowered
        // "2", drawn rather than using unicode superscript/subscript
        // digits (U+00B2/U+2082) -- g_font is ASCII-only (see
        // draw_dropdown_btn's own comment on why), so those rendered as
        // tofu '?' glyphs.
        draw_icon_btn(&row1_x, row1_y, btn_h, false,
            [&](Rectangle rect, Color color) {
                Vector2 xs = MeasureTextEx(g_font, "x", font_size * 0.85f, 0);
                float small = font_size * 0.6f;
                float total_w = xs.x + small * 0.7f;
                float x0 = rect.x + (rect.width - total_w) / 2.0f;
                DrawTextEx(g_font, "x", Vector2{x0, rect.y + (rect.height - font_size * 0.85f) / 2.0f + 3.0f}, font_size * 0.85f, 0, color);
                DrawTextEx(g_font, "2", Vector2{x0 + xs.x, rect.y + (rect.height - small) / 2.0f - 4.0f}, small, 0, color);
            },
            [] { g_editor.ToggleOfficeSuperscript(); });
        draw_icon_btn(&row1_x, row1_y, btn_h, false,
            [&](Rectangle rect, Color color) {
                Vector2 xs = MeasureTextEx(g_font, "x", font_size * 0.85f, 0);
                float small = font_size * 0.6f;
                float total_w = xs.x + small * 0.7f;
                float x0 = rect.x + (rect.width - total_w) / 2.0f;
                DrawTextEx(g_font, "x", Vector2{x0, rect.y + (rect.height - font_size * 0.85f) / 2.0f - 3.0f}, font_size * 0.85f, 0, color);
                DrawTextEx(g_font, "2", Vector2{x0 + xs.x, rect.y + (rect.height - small) / 2.0f + 4.0f}, small, 0, color);
            },
            [] { g_editor.ToggleOfficeSubscript(); });
        add_sep(&row1_x, row1_y);
        // Text color / highlight: an "A" glyph with a colored bar under it
        // -- the bar is a static representative swatch (red / yellow), not
        // a live reflection of the current run's actual color (that state
        // isn't cheaply sampled the way OfficeFormatActive's bool toggles
        // are), same documented simplification as the format panel's
        // margins/page-color controls.
        Rectangle text_color_btn = draw_icon_btn(&row1_x, row1_y, btn_h,
            g_office_dropdown_open == kOfficeDropdownTextColor,
            [&](Rectangle rect, Color color) {
                Vector2 ts = MeasureTextEx(g_font, "A", font_size * 0.85f, 0);
                DrawTextEx(g_font, "A", Vector2{rect.x + (rect.width - ts.x) / 2.0f, rect.y + 2.0f}, font_size * 0.85f, 0, color);
                float bw = rect.width * 0.55f;
                DrawRectangle(static_cast<int>(rect.x + (rect.width - bw) / 2.0f), static_cast<int>(rect.y + rect.height - 6.0f),
                              static_cast<int>(bw), 3, Color{211, 47, 47, 255});
            },
            [] { g_office_dropdown_open = g_office_dropdown_open == kOfficeDropdownTextColor ? -1 : kOfficeDropdownTextColor; });
        Rectangle highlight_btn = draw_icon_btn(&row1_x, row1_y, btn_h,
            g_office_dropdown_open == kOfficeDropdownHighlight,
            [&](Rectangle rect, Color color) {
                Vector2 ts = MeasureTextEx(g_font, "A", font_size * 0.85f, 0);
                DrawTextEx(g_font, "A", Vector2{rect.x + (rect.width - ts.x) / 2.0f, rect.y + 2.0f}, font_size * 0.85f, 0, color);
                float bw = rect.width * 0.55f;
                DrawRectangle(static_cast<int>(rect.x + (rect.width - bw) / 2.0f), static_cast<int>(rect.y + rect.height - 6.0f),
                              static_cast<int>(bw), 3, Color{255, 212, 42, 255});
            },
            [] { g_office_dropdown_open = g_office_dropdown_open == kOfficeDropdownHighlight ? -1 : kOfficeDropdownHighlight; });

        // --- Row 2: lists, special chars, math, table, image, undo/redo ---
        draw_icon_btn(&row2_x, row2_y, btn_h, g_editor.OfficeListKindActive(DocParagraph::ListKind::Bullet),
            [](Rectangle rect, Color color) {
                float pad = rect.width * 0.16f;
                float gap = rect.height / 4.0f;
                for (int i = 0; i < 3; i++) {
                    float cy = rect.y + gap * static_cast<float>(i + 1);
                    DrawCircle(static_cast<int>(rect.x + pad), static_cast<int>(cy), 1.6f, color);
                    DrawRectangle(static_cast<int>(rect.x + pad * 2.4f), static_cast<int>(cy - 1.0f),
                                  static_cast<int>(rect.width - pad * 3.4f), 2, color);
                }
            },
            [] { g_editor.SetOfficeListKind(DocParagraph::ListKind::Bullet); });
        draw_icon_btn(&row2_x, row2_y, btn_h, g_editor.OfficeListKindActive(DocParagraph::ListKind::Numbered),
            [&](Rectangle rect, Color color) {
                float pad = rect.width * 0.14f;
                float gap = rect.height / 4.0f;
                float num_size = std::max(7.0f, rect.height * 0.3f);
                for (int i = 0; i < 3; i++) {
                    float cy = rect.y + gap * static_cast<float>(i + 1);
                    std::string d = std::to_string(i + 1) + ".";
                    DrawTextEx(g_font, d.c_str(), Vector2{rect.x + pad * 0.4f, cy - num_size * 0.5f}, num_size, 0, color);
                    DrawRectangle(static_cast<int>(rect.x + pad * 3.0f), static_cast<int>(cy - 1.0f),
                                  static_cast<int>(rect.width - pad * 4.0f), 2, color);
                }
            },
            [] { g_editor.SetOfficeListKind(DocParagraph::ListKind::Numbered); });
        add_sep(&row2_x, row2_y);
        Rectangle special_btn = draw_dropdown_btn(&row2_x, row2_y, "Sym", g_office_dropdown_open == kOfficeDropdownSpecialChars, [] {
            g_office_dropdown_open = g_office_dropdown_open == kOfficeDropdownSpecialChars ? -1 : kOfficeDropdownSpecialChars;
        });
        draw_btn(&row2_x, row2_y, "fx", false, [] { g_editor.InsertOfficeMath(); });
        add_sep(&row2_x, row2_y);
        draw_icon_btn(&row2_x, row2_y, btn_h, false,
            [](Rectangle rect, Color color) {
                float pad = rect.width * 0.18f;
                Rectangle grid{rect.x + pad, rect.y + pad * 0.6f, rect.width - 2.0f * pad, rect.height - 1.2f * pad};
                DrawRectangleLinesEx(grid, 1.2f, color);
                DrawLineEx(Vector2{grid.x, grid.y + grid.height / 2.0f}, Vector2{grid.x + grid.width, grid.y + grid.height / 2.0f}, 1.0f, color);
                DrawLineEx(Vector2{grid.x + grid.width / 2.0f, grid.y}, Vector2{grid.x + grid.width / 2.0f, grid.y + grid.height}, 1.0f, color);
            },
            [] { g_editor.InsertOfficeTablePrompt(); });
        draw_icon_btn(&row2_x, row2_y, btn_h, false,
            [](Rectangle rect, Color color) {
                float pad = rect.width * 0.16f;
                Rectangle frame{rect.x + pad, rect.y + pad * 0.7f, rect.width - 2.0f * pad, rect.height - 1.4f * pad};
                DrawRectangleLinesEx(frame, 1.2f, color);
                DrawCircle(static_cast<int>(frame.x + frame.width * 0.3f), static_cast<int>(frame.y + frame.height * 0.32f),
                          std::max(1.2f, frame.height * 0.11f), color);
                Vector2 p1{frame.x + 1.5f, frame.y + frame.height - 1.5f};
                Vector2 p2{frame.x + frame.width * 0.4f, frame.y + frame.height * 0.42f};
                Vector2 p3{frame.x + frame.width - 1.5f, frame.y + frame.height - 1.5f};
                DrawTriangle(p2, p1, p3, color);
            },
            [] { g_editor.InsertOfficeImagePrompt(); });
        add_sep(&row2_x, row2_y);
        // Undo/redo: an arced ring (DrawRing) with a triangular arrowhead
        // at its open end, mirrored for redo -- the standard curved-arrow
        // shape, built from primitives rather than an icon glyph.
        auto draw_undo_redo_icon = [](Rectangle rect, Color color, bool redo) {
            float cx = rect.x + rect.width / 2.0f;
            float cy = rect.y + rect.height / 2.0f + 1.0f;
            float r = rect.height * 0.28f;
            float a0 = redo ? 200.0f : -20.0f;
            float a1 = redo ? 470.0f : 250.0f;
            DrawRing(Vector2{cx, cy}, r - 1.3f, r + 1.3f, a0, a1, 20, color);
            float tip_ang = (redo ? a0 : a1) * DEG2RAD;
            float tipx = cx + r * cosf(tip_ang), tipy = cy + r * sinf(tip_ang);
            float tangent = tip_ang + (redo ? -1.5708f : 1.5708f);
            Vector2 dir{cosf(tangent), sinf(tangent)};
            Vector2 normal{-dir.y, dir.x};
            float s = r * 0.7f;
            Vector2 p1{tipx + dir.x * s, tipy + dir.y * s};
            Vector2 p2{tipx - normal.x * s * 0.7f, tipy - normal.y * s * 0.7f};
            Vector2 p3{tipx + normal.x * s * 0.7f, tipy + normal.y * s * 0.7f};
            DrawTriangle(p1, p2, p3, color);
        };
        draw_icon_btn(&row2_x, row2_y, btn_h, false,
            [&](Rectangle rect, Color color) { draw_undo_redo_icon(rect, color, false); },
            [] { g_editor.UndoOffice(); });
        draw_icon_btn(&row2_x, row2_y, btn_h, false,
            [&](Rectangle rect, Color color) { draw_undo_redo_icon(rect, color, true); },
            [] { g_editor.RedoOffice(); });

        content_y += toolbar_h;
        content_h -= toolbar_h;

        // A document is a page, not an edge-to-edge terminal buffer.  Keep
        // the work surface quiet and center a bounded paper sheet within it
        // so reading long-form DOCX/ODT content has the same visual anchor
        // as the reference editor, even in a very wide pane. Sized/centered
        // against ocx/ocw (the band left over after DrawOfficeSidePanels'
        // rail/Outline/Format reservation above), not the pane's raw x/w.
        const float canvas_pad = std::max(14.0f, std::min(34.0f, ocw * 0.035f));
        const float page_w = std::max(160.0f, std::min(ocw - 2.0f * canvas_pad, 900.0f * office_sess->zoom));
        const float page_x = ocx + (ocw - page_w) * 0.5f;
        // Ruler (this pane had none before this restyle -- see WYSIWYG
        // restyle plan): a thin bar between the toolbar and the page,
        // spanning the page's own width so its tick marks/margin markers
        // line up with the text column below it. Reserves ruler_h out of
        // the vertical space page_top/page_h used to have all to
        // themselves.
        const float ruler_h = 20.0f;
        const float page_top = content_y + ruler_h + canvas_pad * 0.4f;
        const float page_h = std::max(40.0f, content_h - ruler_h - canvas_pad * 0.85f);
        const Color canvas = ResolveHlGroup("NormalBg");
        const Color paper = ResolveHlGroup("OfficePage");
        DrawRectangle(static_cast<int>(ocx), static_cast<int>(content_y), static_cast<int>(ocw), static_cast<int>(content_h), canvas);
        {
            float pad = std::max(24.0f, page_w * 0.09f);
            Rectangle ruler{page_x, content_y + 2.0f, page_w, ruler_h - 4.0f};
            DrawRectangleRounded(ruler, 0.3f, 4, ResolveHlGroup("MenuBar"));
            // One tick per inch-equivalent unit (matches the page's own
            // canonical 900px-wide-at-100%-zoom convention above, so ticks
            // stay aligned with the text column as zoom changes) plus
            // triangular margin markers at the left/right text inset.
            float unit_px = 100.0f * office_sess->zoom;
            int unit = 1;
            for (float ux = unit_px; ux < page_w - 1.0f; ux += unit_px, unit++) {
                float tx = ruler.x + ux;
                DrawLineEx(Vector2{tx, ruler.y + ruler.height * 0.35f}, Vector2{tx, ruler.y + ruler.height}, 1.0f, ResolveHlGroup("MutedFg"));
            }
            auto margin_marker = [&](float mx) {
                DrawTriangle(Vector2{mx - 4.0f, ruler.y + ruler.height}, Vector2{mx + 4.0f, ruler.y + ruler.height},
                            Vector2{mx, ruler.y + ruler.height * 0.4f}, ResolveHlGroup("Accent"));
            };
            margin_marker(ruler.x + pad);
            margin_marker(ruler.x + page_w - pad);
        }
        DrawRectangleRounded(Rectangle{page_x + 2.0f, page_top + 3.0f, page_w, page_h}, 0.012f, 8, Fade(BLACK, 0.16f));
        DrawRectangleRounded(Rectangle{page_x, page_top, page_w, page_h}, 0.012f, 8, paper);
        DrawRectangleRoundedLines(Rectangle{page_x, page_top, page_w, page_h}, 0.012f, 8, ResolveHlGroup("Border"));

        g_editor.ResizeOfficeViewport(pane.buffer_id, static_cast<int>(page_w), static_cast<int>(page_h));
        const OfficeDoc &doc = office_sess->doc;
        int para_count = static_cast<int>(doc.paragraphs.size());
        float pad = std::max(24.0f, page_w * 0.09f);
        float max_width = std::max(50.0f, page_w - 2.0f * pad);
        float body_size = office_sess->base_font_pt * office_sess->zoom;

        auto line_height_for = [&](int heading_level) { return body_size * OfficeHeadingMultiplier(heading_level) * 1.35f; };

        // Wraps the cursor's own paragraph once up front (reused by both
        // the scroll-follow scan below and the draw loop) to find which of
        // ITS visual lines holds cursor_col.
        int cp = std::clamp(office_sess->cursor_para, 0, para_count - 1);
        float cursor_para_size = body_size * OfficeHeadingMultiplier(doc.paragraphs[cp].heading_level);
        std::vector<OfficeWrapLine> cursor_wrap = WordWrapOfficeParagraph(doc.paragraphs[cp], max_width, cursor_para_size);
        // Picks the LAST line whose start the cursor has reached, not the
        // first whose end it hasn't exceeded -- those differ exactly when a
        // wrap point is contiguous (WordWrapOfficeParagraph's own non-
        // whitespace-overflow branch: line[k].end == line[k+1].start, no
        // dropped separator between them, e.g. wrapping mid-sentence on an
        // ordinary word boundary). A cursor_col sitting exactly on that
        // shared boundary value is honestly ambiguous, but "first line
        // whose end it doesn't exceed" always resolves it to the EARLIER
        // line -- so moving the cursor to a wrapped line's own start
        // (MoveOfficeCursorVisualLine, editor.cpp's j/k) always rendered on
        // the line *above* instead, at that line's rightmost edge, looking
        // like the motion silently failed. "Last start reached" resolves
        // the same tie to the line actually moved to, and still resolves a
        // *dropped*-whitespace wrap point (a real one-byte gap: line[k].end
        // + 1 == line[k+1].start) to the earlier line, same as before,
        // since line[k+1].start > cursor_col there.
        int cursor_line_in_para = 0;
        for (int li = 0; li < static_cast<int>(cursor_wrap.size()); li++) {
            if (cursor_wrap[li].start > office_sess->cursor_col) break;
            cursor_line_in_para = li;
        }
        // Feeds Editor::MoveOfficeCursorVisualLine (editor.cpp's j/k) the
        // same wrap it just computed -- see OfficeSession::cursor_wrap_lines'
        // own comment for why that state has to be pushed in from here
        // rather than computed where it's read.
        {
            std::vector<std::pair<int, int>> wrap_pairs;
            wrap_pairs.reserve(cursor_wrap.size());
            for (const OfficeWrapLine &wl : cursor_wrap) wrap_pairs.emplace_back(wl.start, wl.end);
            g_editor.SetOfficeCursorWrapLines(pane.buffer_id, cp, std::move(wrap_pairs));
        }

        // Scroll-follow: word-wrap-aware equivalent of Editor::
        // UpdateScrollForPane's own "snap up if the cursor is above the
        // current scroll position, else advance a row at a time until it's
        // back in view" shape -- can't live in editor.cpp (raylib-free, no
        // MeasureTextEx), see ResizeOfficeViewport's own comment for why.
        // steps_left mirrors UpdateScrollForPane's own cursor_delta/cap
        // smoothing (its comment has the full reasoning): capped to how
        // far cursor_para itself moved since the last call, so a
        // paragraph's anchored image/table -- OfficeParagraphExtraHeight,
        // added to `used` below; the scan used to ignore it completely, a
        // documented v1 gap that let the cursor walk visibly off-screen
        // with no scroll happening at all before an eventual multi-step
        // catch-up -- slides into view over a few frames (this scan reruns
        // every rendered frame) instead of jumping in one, while a
        // genuinely large cursor move (a picker jump, Ctrl-Home-equivalent)
        // still lands in a single frame since steps_left grows with it too.
        int para_delta = (office_sess->scroll_follow_last_cursor_para < 0)
                              ? para_count
                              : std::abs(cp - office_sess->scroll_follow_last_cursor_para);
        g_editor.SetOfficeScrollFollowCursorPara(pane.buffer_id, cp);
        int steps_left = std::max(1, para_delta);

        int scroll_para = office_sess->scroll_para;
        int scroll_line = office_sess->scroll_line_in_para;
        bool cursor_before_scroll = (cp < scroll_para) || (cp == scroll_para && cursor_line_in_para < scroll_line);
        if (cursor_before_scroll) {
            // Steps scroll_para/scroll_line backward one wrap-line (or, at
            // a paragraph's own first line, one paragraph) at a time until
            // it reaches (cp, cursor_line_in_para) or steps_left runs out
            // -- can never step past it into a lower paragraph, since this
            // check runs before every decrement and cp/cursor_line_in_para
            // is itself a coordinate this same stepping scheme visits.
            for (; steps_left > 0 && !(scroll_para == cp && scroll_line <= cursor_line_in_para); steps_left--) {
                if (scroll_line > 0) {
                    scroll_line--;
                } else if (scroll_para > 0) {
                    scroll_para--;
                    const DocParagraph &ppara = doc.paragraphs[scroll_para];
                    std::vector<OfficeWrapLine> pwl = WordWrapOfficeParagraph(
                        ppara, max_width, body_size * OfficeHeadingMultiplier(ppara.heading_level));
                    scroll_line = std::max(0, static_cast<int>(pwl.size()) - 1);
                } else {
                    break;
                }
            }
        } else {
            for (; steps_left > 0; steps_left--) {
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
                        if (used > page_h) {
                            overflowed = true;
                            break;
                        }
                    }
                    if (!overflowed && !found) {
                        used += OfficeParagraphExtraHeight(doc, para, max_width, body_size);
                        if (used > page_h) overflowed = true;
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

        // Docs-style status line (word/page-of estimate, zoom) and
        // scrollbar both need the *whole* document's wrapped height, unlike
        // the scroll-follow scan above (which only ever walks forward/
        // backward from the current scroll position). A fresh top-to-
        // bottom pass every frame is simplest and cheap enough for
        // realistic doc sizes (word-wrap is O(text length), and this frame
        // already pays a comparable cost drawing the visible portion) --
        // no cache, see WYSIWYG restyle plan for why that's an accepted
        // v1 tradeoff rather than a perf bug.
        {
            float total_h = 0.0f, height_at_scroll = 0.0f;
            int words = 0;
            for (int pi = 0; pi < para_count; pi++) {
                const DocParagraph &para = doc.paragraphs[pi];
                float plh = line_height_for(para.heading_level);
                int line_count = static_cast<int>(
                    ((pi == cp) ? cursor_wrap : WordWrapOfficeParagraph(para, max_width, body_size * OfficeHeadingMultiplier(para.heading_level)))
                        .size());
                float ph = plh * static_cast<float>(std::max(1, line_count)) +
                           OfficeParagraphExtraHeight(doc, para, max_width, body_size);
                if (pi < scroll_para) height_at_scroll += ph;
                else if (pi == scroll_para) height_at_scroll += plh * static_cast<float>(scroll_line);
                total_h += ph;
                bool in_word = false;
                for (char ch : para.text) {
                    bool space = ch == ' ' || ch == '\t' || ch == '\n';
                    if (!space && !in_word) words++;
                    in_word = !space;
                }
            }
            total_h = std::max(total_h, 1.0f);
            g_office_status.valid = true;
            g_office_status.buffer_id = pane.buffer_id;
            g_office_status.word_count = words;
            g_office_status.zoom = office_sess->zoom;
            g_office_status.total_height = total_h;
            g_office_status.visible_fraction = std::clamp(page_h / total_h, 0.02f, 1.0f);
            g_office_status.scroll_fraction = std::clamp(height_at_scroll / total_h, 0.0f, 1.0f);
            g_office_status.page_estimate = static_cast<int>(height_at_scroll / page_h) + 1;
            g_office_status.page_total_estimate = std::max(1, static_cast<int>(std::ceil(total_h / page_h)));
            // Right edge of ocx/ocw (the toolbar/page band, not the pane's
            // raw x/w) so the scrollbar sits just left of the Format panel
            // when it's open, rather than under it. Inset a little further
            // when it's ocx+ocw that coincides with the pane's own right
            // edge, to clear DrawPaneBorder's own 6px active-pane border
            // strip (drawn later, over that same edge) -- otherwise the
            // border paints right over the track.
            g_office_status.track = Rectangle{ocx + ocw - 17.0f, content_y, 8.0f, content_h - 4.0f};
            g_office_status.max_width = max_width;
            g_office_status.body_size = body_size;

            // Docs-style status footer: page/word-count estimate + language
            // on the left, a zoom slider + percentage on the right --
            // anchored to *this pane's own* bottom edge (office_footer_y,
            // computed above), not the app's global status line.
            float status_font_size = std::max(kMinFontSize, g_font_size - 2.0f);
            DrawRectangle(static_cast<int>(x), static_cast<int>(office_footer_y), static_cast<int>(w),
                          static_cast<int>(kOfficeFooterH), ResolveHlGroup("MenuBar"));
            DrawRectangle(static_cast<int>(x), static_cast<int>(office_footer_y), static_cast<int>(w), 1,
                          ResolveHlGroup("Border"));
            std::string left_label = "Page ~" + std::to_string(g_office_status.page_estimate) + " of ~" +
                                std::to_string(g_office_status.page_total_estimate) + "    " +
                                std::to_string(g_office_status.word_count) + " words    English (US)";
            DrawTextEx(g_font, left_label.c_str(), Vector2{x + 12.0f, office_footer_y + (kOfficeFooterH - status_font_size) / 2.0f},
                      status_font_size, 0, ResolveHlGroup("MutedFg"));

            int zoom_pct = static_cast<int>(g_office_status.zoom * 100.0f + 0.5f);
            std::string pct_label = std::to_string(zoom_pct) + "%";
            float pct_w = MeasureTextEx(g_font, pct_label.c_str(), status_font_size, 0).x;
            float pct_x = x + w - 12.0f - pct_w;
            DrawTextEx(g_font, pct_label.c_str(), Vector2{pct_x, office_footer_y + (kOfficeFooterH - status_font_size) / 2.0f},
                      status_font_size, 0, ResolveHlGroup("MutedFg"));

            constexpr float kSliderW = 90.0f, kZoomMin = 0.5f, kZoomMax = 2.0f;
            Rectangle slider{pct_x - kSliderW - 16.0f, office_footer_y + kOfficeFooterH / 2.0f - 2.0f, kSliderW, 4.0f};
            DrawRectangleRounded(slider, 0.5f, 4, ResolveHlGroup("Border"));
            float zfrac = std::clamp((g_office_status.zoom - kZoomMin) / (kZoomMax - kZoomMin), 0.0f, 1.0f);
            Vector2 thumb_c{slider.x + slider.width * zfrac, slider.y + slider.height / 2.0f};
            DrawCircle(static_cast<int>(thumb_c.x), static_cast<int>(thumb_c.y), 6.0f, ResolveHlGroup("Accent"));
            Rectangle slider_hit{slider.x - 6.0f, slider.y - 8.0f, slider.width + 12.0f, 20.0f};
            Vector2 zmouse = GetMousePosition();
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(zmouse, slider_hit)) {
                g_office_zoom_drag = true;
            }
            if (g_office_zoom_drag) {
                if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    float f = std::clamp((zmouse.x - slider.x) / slider.width, 0.0f, 1.0f);
                    float target = kZoomMin + f * (kZoomMax - kZoomMin);
                    if (g_office_status.zoom > 0.001f) g_editor.SetOfficeZoom(target / g_office_status.zoom);
                } else {
                    g_office_zoom_drag = false;
                }
            }
        }

        BeginScissorMode(static_cast<int>(page_x), static_cast<int>(page_top), static_cast<int>(page_w),
                          static_cast<int>(page_h));
        Color text_color = ResolveHlGroup("Normal");
        Color sel_color = ResolveHlGroup("AccentTint");
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
        // Math is the default presentation for a math-formatted run. While
        // actively inserting *inside* one, reveal its LaTeX source as a
        // single editable unit; using the same source width for layout and
        // cursor placement keeps the caret from drifting over a typeset
        // fraction/superscript while the user is changing its code.
        const bool office_insert = is_active && g_editor.CurrentMode() == Mode::OfficeInsert;
        auto run_size_for = [&](const OfficeFormatRun &run, float paragraph_size) {
            float run_size = run.fmt.font_size_pt > 0.0f ? run.fmt.font_size_pt * office_sess->zoom : paragraph_size;
            if (run.fmt.superscript || run.fmt.subscript) run_size *= 0.65f;
            return run_size;
        };
        auto math_source_is_active = [&](int paragraph, const OfficeFormatRun &run) {
            if (!run.fmt.math || !office_insert || paragraph != cp) return false;
            // A long math run can be split into multiple OfficeFormatRuns
            // by visual wrapping. Locate its original document span so all
            // wrapped fragments reveal source together while editing.
            for (const DocSpan &span : doc.paragraphs[paragraph].spans) {
                if (!span.fmt.math || office_sess->cursor_col < span.start || office_sess->cursor_col > span.end) continue;
                if (run.start < span.end && run.end > span.start) return true;
            }
            return false;
        };
        auto run_width = [&](int paragraph, const OfficeFormatRun &run, const std::string &text, float paragraph_size) {
            const float run_size = run_size_for(run, paragraph_size);
            if (run.fmt.math && !math_source_is_active(paragraph, run)) return LayoutMathExpression(text, run_size).width;
            return MeasureTextEx(OfficeFontFor(run.fmt), text.c_str(), run_size, 0).x;
        };
        float draw_y = page_top + pad * 0.75f;
        for (int pi = scroll_para; pi < para_count && draw_y < page_top + page_h - pad * 0.5f; pi++) {
            const DocParagraph &para = doc.paragraphs[pi];
            float size = body_size * OfficeHeadingMultiplier(para.heading_level);
            float lh = size * 1.35f;
            std::vector<OfficeWrapLine> wl_local = (pi == cp) ? cursor_wrap : WordWrapOfficeParagraph(para, max_width, size);
            int start_li = (pi == scroll_para) ? scroll_line : 0;
            for (int li = start_li; li < static_cast<int>(wl_local.size()); li++) {
                if (draw_y > page_top + page_h - pad * 0.5f) break;
                const OfficeWrapLine &line = wl_local[li];
                std::vector<OfficeFormatRun> runs = BuildOfficeDisplayRuns(
                    para, line.start, line.end, office_insert && pi == cp, office_sess->cursor_col);
                float line_x = page_x + pad;
                if (para.align == DocParagraph::Align::Center || para.align == DocParagraph::Align::Right) {
                    float total_w = 0.0f;
                    for (const auto &r : runs) {
                        std::string t = para.text.substr(r.start, r.end - r.start);
                        for (auto &ch : t) {
                            if (ch == '\t') ch = ' ';
                        }
                        total_w += run_width(pi, r, t, size);
                    }
                    line_x = para.align == DocParagraph::Align::Center ? page_x + (page_w - total_w) / 2.0f
                                                                          : page_x + page_w - pad - total_w;
                }
                if (para.list_kind == DocParagraph::ListKind::Bullet && li == 0) {
                    DrawTextEx(OfficeFontFor(DocFormat{}), "\xE2\x80\xA2 ", Vector2{line_x, draw_y}, size, 0, text_color);
                    line_x += MeasureTextEx(OfficeFontFor(DocFormat{}), "\xE2\x80\xA2 ", size, 0).x;
                } else if (para.list_kind == DocParagraph::ListKind::Numbered && li == 0) {
                    // Displayed number is "position within the current run
                    // of consecutive Numbered paragraphs", computed here
                    // rather than stored -- see DocParagraph::ListKind's
                    // own comment (office_doc.h) for why.
                    int num = 1;
                    for (int k = pi - 1; k >= 0 && doc.paragraphs[k].list_kind == DocParagraph::ListKind::Numbered; k--) num++;
                    std::string marker = std::to_string(num) + ". ";
                    DrawTextEx(OfficeFontFor(DocFormat{}), marker.c_str(), Vector2{line_x, draw_y}, size, 0, text_color);
                    line_x += MeasureTextEx(OfficeFontFor(DocFormat{}), marker.c_str(), size, 0).x;
                }
                if (office_visual && pi >= sel_pa && pi <= sel_pb) {
                    int hl_start = (pi == sel_pa) ? sel_ca : 0;
                    int hl_end = (pi == sel_pb) ? sel_cb : static_cast<int>(para.text.size());
                    hl_start = std::max(hl_start, line.start);
                    hl_end = std::min(hl_end, line.end);
                    if (hl_end > hl_start) {
                        std::vector<OfficeFormatRun> pre_runs = BuildOfficeDisplayRuns(
                            para, line.start, hl_start, office_insert && pi == cp, office_sess->cursor_col);
                        float hl_x0 = line_x;
                        for (const auto &r : pre_runs) {
                            std::string t = para.text.substr(r.start, r.end - r.start);
                            for (auto &ch : t) {
                                if (ch == '\t') ch = ' ';
                            }
                            hl_x0 += run_width(pi, r, t, size);
                        }
                        std::vector<OfficeFormatRun> hl_runs = BuildOfficeDisplayRuns(
                            para, hl_start, hl_end, office_insert && pi == cp, office_sess->cursor_col);
                        float hl_w = 0.0f;
                        for (const auto &r : hl_runs) {
                            std::string t = para.text.substr(r.start, r.end - r.start);
                            for (auto &ch : t) {
                                if (ch == '\t') ch = ' ';
                            }
                            hl_w += run_width(pi, r, t, size);
                        }
                        DrawRectangle(static_cast<int>(hl_x0), static_cast<int>(draw_y), static_cast<int>(hl_w),
                                      static_cast<int>(size), sel_color);
                    }
                }
                float run_x = line_x;
                bool cursor_on_rendered_math = false;
                float rendered_math_cursor_x = 0.0f, rendered_math_cursor_w = 0.0f, rendered_math_cursor_h = size;
                for (const auto &r : runs) {
                    std::string t = para.text.substr(r.start, r.end - r.start);
                    for (auto &ch : t) {
                        if (ch == '\t') ch = ' ';
                    }
                    // Explicit font_size_pt overrides the paragraph's own
                    // heading/body size; superscript/subscript additionally
                    // scale down and shift the baseline. Only this draw
                    // loop uses the per-run size -- the cursor-position and
                    // selection-highlight measurement loops just below
                    // still use the outer uniform `size`, so a mixed-size
                    // line's cursor/highlight pixel position can be
                    // slightly approximate; never a correctness issue
                    // (cursor *column* is character-index-based, not
                    // pixel-based) but a documented v1 cosmetic gap.
                    float run_size = run_size_for(r, size);
                    float run_y = draw_y;
                    if (r.fmt.superscript) run_y -= size * 0.3f;
                    else if (r.fmt.subscript) run_y += size * 0.25f;
                    Color run_color = r.fmt.has_color ? Color{r.fmt.color_r, r.fmt.color_g, r.fmt.color_b, 255} : text_color;
                    float rw;
                    if (r.fmt.math && !math_source_is_active(pi, r)) {
                        // DrawMathLayout's own y is the top of its bounding
                        // box (matching how HtmlLayoutBlock's math branch
                        // stores/advances a top-down cursor_y by ml.height),
                        // same convention DrawTextEx already uses for
                        // `position` here -- no baseline adjustment needed.
                        MathLayoutResult ml = LayoutMathExpression(t, run_size);
                        if (r.fmt.has_highlight) {
                            DrawRectangle(static_cast<int>(run_x), static_cast<int>(run_y), static_cast<int>(ml.width),
                                          static_cast<int>(ml.height),
                                          Color{r.fmt.highlight_r, r.fmt.highlight_g, r.fmt.highlight_b, 255});
                        }
                        DrawMathLayout(run_x, run_y, ml, run_color);
                        rw = ml.width;
                    } else {
                        Font &f = OfficeFontFor(r.fmt);
                        rw = MeasureTextEx(f, t.c_str(), run_size, 0).x;
                        if (r.fmt.has_highlight) {
                            DrawRectangle(static_cast<int>(run_x), static_cast<int>(draw_y), static_cast<int>(rw),
                                          static_cast<int>(size),
                                          Color{r.fmt.highlight_r, r.fmt.highlight_g, r.fmt.highlight_b, 255});
                        }
                        DrawTextEx(f, t.c_str(), Vector2{run_x, run_y}, run_size, 0, run_color);
                        if (r.fmt.underline || r.fmt.strike) {
                            float uy = run_y + (r.fmt.strike ? run_size * 0.5f : run_size * 0.95f);
                            DrawLineEx(Vector2{run_x, uy}, Vector2{run_x + rw, uy}, 1.0f, run_color);
                        }
                    }
                    const bool cursor_in_run = office_sess->cursor_col >= r.start && office_sess->cursor_col <= r.end;
                    const bool cursor_on_delimiter = r.dollar_delimited_math &&
                        office_sess->cursor_col >= r.start - 1 && office_sess->cursor_col <= r.end;
                    if (is_active && pi == cp && li == cursor_line_in_para && !office_insert && r.fmt.math &&
                        (cursor_in_run || cursor_on_delimiter)) {
                        cursor_on_rendered_math = true;
                        rendered_math_cursor_x = run_x;
                        rendered_math_cursor_w = rw;
                        rendered_math_cursor_h = std::max(size, r.fmt.math ? LayoutMathExpression(t, run_size).height : size);
                    }
                    run_x += rw;
                }
                if (is_active && pi == cp && li == cursor_line_in_para) {
                    if (cursor_on_rendered_math) {
                        DrawRectangleLines(static_cast<int>(rendered_math_cursor_x - 2.0f), static_cast<int>(draw_y - 2.0f),
                                           static_cast<int>(rendered_math_cursor_w + 4.0f),
                                           static_cast<int>(rendered_math_cursor_h + 4.0f), text_color);
                    } else {
                        std::vector<OfficeFormatRun> pre = BuildOfficeDisplayRuns(
                            para, line.start, office_sess->cursor_col, office_insert && pi == cp, office_sess->cursor_col);
                        float cursor_x = line_x;
                        for (const auto &r : pre) {
                            std::string t = para.text.substr(r.start, r.end - r.start);
                            for (auto &ch : t) {
                                if (ch == '\t') ch = ' ';
                            }
                            cursor_x += run_width(pi, r, t, size);
                        }
                        if (office_insert) {
                            DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(draw_y), 2, static_cast<int>(size),
                                          text_color);
                        } else {
                            // Block cursor (OfficeNormal/OfficeVisual) --
                            // same block-in-Normal/line-in-Insert
                            // convention as the plain-text editor's own
                            // cursor (DrawEditor), sized to the next
                            // character's own measured width (rich text
                            // isn't monospace, unlike Buffer's g_char_width)
                            // rather than a fixed pixel width, with that
                            // character's glyph redrawn on top in the
                            // page's own background so it stays legible
                            // instead of vanishing under a solid block --
                            // same "punch the character back through"
                            // touch the plain editor's block cursor does.
                            int next_col = std::min(static_cast<int>(para.text.size()), office_sess->cursor_col + 1);
                            std::vector<OfficeFormatRun> at_cursor =
                                BuildOfficeDisplayRuns(para, office_sess->cursor_col, next_col, false, office_sess->cursor_col);
                            float block_w = size * 0.55f;  // fallback: end of paragraph/line, no next character
                            if (!at_cursor.empty() && at_cursor[0].end > at_cursor[0].start) {
                                const OfficeFormatRun &r = at_cursor[0];
                                std::string t = para.text.substr(r.start, r.end - r.start);
                                for (auto &ch : t) {
                                    if (ch == '\t') ch = ' ';
                                }
                                block_w = std::max(4.0f, run_width(pi, r, t, size));
                            }
                            DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(draw_y), static_cast<int>(block_w),
                                          static_cast<int>(size), Fade(text_color, 0.55f));
                            if (!at_cursor.empty() && at_cursor[0].end > at_cursor[0].start && !at_cursor[0].fmt.math) {
                                const OfficeFormatRun &r = at_cursor[0];
                                std::string t = para.text.substr(r.start, r.end - r.start);
                                for (auto &ch : t) {
                                    if (ch == '\t') ch = ' ';
                                }
                                float run_size2 = run_size_for(r, size);
                                float ry2 = draw_y;
                                if (r.fmt.superscript) ry2 -= size * 0.3f;
                                else if (r.fmt.subscript) ry2 += size * 0.25f;
                                DrawTextEx(OfficeFontFor(r.fmt), t.c_str(), Vector2{cursor_x, ry2}, run_size2, 0, paper);
                            }
                        }
                    }
                }
                draw_y += lh;
            }

            // A table/image anchored to this paragraph (DocParagraph::
            // table_ref/image_ref) renders immediately below its text, as
            // its own block -- not word-wrapped/flowed inline with the
            // paragraph's own text the way a real inline object would be,
            // a deliberate v1 simplification (matches DocTable/DocImage's
            // own scope notes in office_doc.h). Not accounted for by the
            // scroll-follow scan above (which only sums wrapped-line
            // heights), so a very tall table/image can still push the
            // cursor below the viewport before scroll catches up -- a
            // known, documented v1 gap rather than a silent one.
            if (para.table_ref >= 0 && para.table_ref < static_cast<int>(doc.tables.size())) {
                const DocTable &tbl = doc.tables[static_cast<size_t>(para.table_ref)];
                bool in_this_table = office_sess->in_table_edit == para.table_ref;
                float cell_font = body_size * 0.85f;
                float cell_h = cell_font + 12.0f;
                float col_w = tbl.cols > 0 ? std::max(50.0f, max_width / static_cast<float>(tbl.cols)) : max_width;
                Font &cell_fontobj = OfficeFontFor(DocFormat{});
                for (int r = 0; r < tbl.rows; r++) {
                    float ry = draw_y;
                    if (ry <= page_top + page_h - pad * 0.5f) {
                        for (int c = 0; c < tbl.cols; c++) {
                            float cx = page_x + pad + col_w * static_cast<float>(c);
                            bool cell_active = is_active && in_this_table && r == office_sess->table_cursor_row &&
                                               c == office_sess->table_cursor_col;
                            DrawRectangle(static_cast<int>(cx), static_cast<int>(ry), static_cast<int>(col_w),
                                          static_cast<int>(cell_h),
                                          cell_active ? ResolveHlGroup("AccentTint") : ResolveHlGroup("MenuBar"));
                            DrawRectangleLines(static_cast<int>(cx), static_cast<int>(ry), static_cast<int>(col_w),
                                                static_cast<int>(cell_h), ResolveHlGroup("Border"));
                            const std::string &full = tbl.Cell(r, c);
                            std::string shown = full;
                            while (!shown.empty() &&
                                   MeasureTextEx(cell_fontobj, shown.c_str(), cell_font, 0).x > col_w - 8.0f) {
                                shown.pop_back();
                            }
                            DrawTextEx(cell_fontobj, shown.c_str(),
                                      Vector2{cx + 4.0f, ry + (cell_h - cell_font) / 2.0f}, cell_font, 0, text_color);
                            if (cell_active && office_sess->table_cell_editing) {
                                int cc = std::clamp(office_sess->table_cell_col, 0, static_cast<int>(full.size()));
                                float cur_x =
                                    cx + 4.0f + MeasureTextEx(cell_fontobj, full.substr(0, static_cast<size_t>(cc)).c_str(),
                                                              cell_font, 0).x;
                                DrawRectangle(static_cast<int>(cur_x), static_cast<int>(ry), 2,
                                              static_cast<int>(cell_h), text_color);
                            }
                        }
                    }
                    draw_y += cell_h;
                }
                draw_y += 8.0f;
            }
            if (para.image_ref >= 0 && para.image_ref < static_cast<int>(doc.images.size())) {
                const DocImage &img = doc.images[static_cast<size_t>(para.image_ref)];
                Texture2D *tex = GetOrLoadOfficeImageTexture(pane.buffer_id, para.image_ref, img);
                if (tex && img.natural_w > 0 && img.natural_h > 0) {
                    float draw_w = static_cast<float>(img.natural_w);
                    float draw_h = static_cast<float>(img.natural_h);
                    if (draw_w > max_width) {
                        float scale = max_width / draw_w;
                        draw_w *= scale;
                        draw_h *= scale;
                    }
                    if (draw_y <= page_top + page_h - pad * 0.5f) {
                        DrawTexturePro(*tex, Rectangle{0.0f, 0.0f, static_cast<float>(img.natural_w), static_cast<float>(img.natural_h)},
                                      Rectangle{page_x + pad, draw_y, draw_w, draw_h}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
                    }
                    draw_y += draw_h + 8.0f;
                }
            }
        }
        EndScissorMode();

        // --- Scrollbar: real vertical scrollbar (this pane had none before
        // this restyle -- see WYSIWYG restyle plan) at the pane's right
        // edge, track spanning the whole content area (not just the page
        // rect) so it stays reachable regardless of zoom/page width.
        // Geometry/fractions computed into g_office_status just above;
        // dragging is handled by UpdateOfficeScrollbarInteraction (mirrors
        // UpdateKanbanMouseInteraction/UpdateGanttMouseInteraction's own
        // "Draw computes this frame's geometry, a sibling Update* consumes
        // it next" split).
        {
            const Rectangle &track = g_office_status.track;
            DrawRectangleRounded(track, 0.5f, 6, ResolveHlGroup("MenuBar"));
            float thumb_h = std::max(24.0f, track.height * g_office_status.visible_fraction);
            float thumb_y = track.y + (track.height - thumb_h) * g_office_status.scroll_fraction;
            g_office_status.thumb = Rectangle{track.x, thumb_y, track.width, thumb_h};
            bool hovered = CheckCollisionPointRec(GetMousePosition(), Rectangle{track.x - 2, track.y, track.width + 4, track.height});
            DrawRectangleRounded(g_office_status.thumb, 0.5f, 6,
                                 hovered || g_office_scroll_drag.active ? ResolveHlGroup("MutedFg") : Color{189, 193, 198, 255});
        }

        // --- Dropdown popups (drawn last, after the document content, so
        // they paint over it -- opened by whichever toggle button above is
        // active). Deliberately outside the scissored content region above
        // so a popup isn't clipped to the content area. content_y here is
        // already past the toolbar (incremented above), so popups anchor
        // directly at content_y rather than content_y + toolbar_h.
        auto draw_popup_bg = [&](Rectangle r) {
            DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                          static_cast<int>(r.height), ResolveHlGroup("OfficePage"));
            DrawRectangleLinesEx(r, 1.0f, ResolveHlGroup("Border"));
        };
        if (g_office_dropdown_open == kOfficeDropdownFontFamily) {
            struct FamItem { OfficeFontFamily fam; const char *label; };
            static const FamItem items[] = {
                {OfficeFontFamily::Sans, "Sans (Liberation Sans)"}, {OfficeFontFamily::Serif, "Serif (Liberation Serif)"},
                {OfficeFontFamily::Mono, "Mono (Liberation Mono)"}};
            float pw = 220.0f, item_h = row_h;
            Rectangle popup{font_family_btn.x, content_y, pw, item_h * 3};
            draw_popup_bg(popup);
            float iy = popup.y;
            for (const FamItem &it : items) {
                Rectangle rect{popup.x, iy, pw, item_h};
                if (CheckCollisionPointRec(GetMousePosition(), rect)) DrawRectangleRec(rect, ResolveHlGroup("CursorLine"));
                DrawTextEx(g_font, it.label, Vector2{rect.x + 6, rect.y + (item_h - font_size) / 2.0f}, font_size, 0,
                          ResolveHlGroup("Normal"));
                OfficeFontFamily fam = it.fam;
                RegisterClickRegion(rect, [fam] {
                    g_editor.SetOfficeFontFamily(fam);
                    g_office_dropdown_open = -1;
                });
                iy += item_h;
            }
        } else if (g_office_dropdown_open == kOfficeDropdownFontSize) {
            static const float sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48};
            float pw = 70.0f, item_h = row_h * 0.85f;
            int n = static_cast<int>(sizeof(sizes) / sizeof(sizes[0]));
            Rectangle popup{font_size_btn.x, content_y, pw, item_h * n};
            draw_popup_bg(popup);
            float iy = popup.y;
            for (float sz : sizes) {
                Rectangle rect{popup.x, iy, pw, item_h};
                if (CheckCollisionPointRec(GetMousePosition(), rect)) DrawRectangleRec(rect, ResolveHlGroup("CursorLine"));
                std::string label = std::to_string(static_cast<int>(sz));
                DrawTextEx(g_font, label.c_str(), Vector2{rect.x + 8, rect.y + (item_h - font_size) / 2.0f}, font_size,
                          0, ResolveHlGroup("Normal"));
                RegisterClickRegion(rect, [sz] {
                    g_editor.SetOfficeFontSizePt(sz);
                    g_office_dropdown_open = -1;
                });
                iy += item_h;
            }
        } else if (g_office_dropdown_open == kOfficeDropdownTextColor || g_office_dropdown_open == kOfficeDropdownHighlight) {
            bool is_highlight = g_office_dropdown_open == kOfficeDropdownHighlight;
            struct Swatch { unsigned char r, g, b; };
            static const Swatch swatches[] = {
                {0, 0, 0},       {220, 50, 50},   {230, 140, 30}, {220, 200, 40},
                {40, 160, 70},   {50, 110, 220},  {130, 70, 200}, {150, 150, 150},
                {255, 255, 255}, {255, 240, 150}, {180, 230, 180}, {180, 220, 255}};
            float cell = 24.0f, gap = 4.0f, cols = 4.0f;
            int n = static_cast<int>(sizeof(swatches) / sizeof(swatches[0]));
            int rows = (n + static_cast<int>(cols) - 1) / static_cast<int>(cols);
            Rectangle anchor = is_highlight ? highlight_btn : text_color_btn;
            float pw = cols * (cell + gap) + gap;
            float ph = static_cast<float>(rows) * (cell + gap) + gap + row_h * 0.7f;
            Rectangle popup{anchor.x, content_y, pw, ph};
            draw_popup_bg(popup);
            // "Clear" row above the swatch grid -- removes any explicit
            // color/highlight override, falling back to the default.
            Rectangle clear_rect{popup.x, popup.y, pw, row_h * 0.7f};
            DrawTextEx(g_font, "Clear", Vector2{clear_rect.x + 6, clear_rect.y + 4}, font_size * 0.85f, 0,
                      ResolveHlGroup("Normal"));
            RegisterClickRegion(clear_rect, [is_highlight] {
                if (is_highlight) g_editor.ClearOfficeHighlight(); else g_editor.ClearOfficeColor();
                g_office_dropdown_open = -1;
            });
            for (int i = 0; i < n; i++) {
                int col = i % static_cast<int>(cols);
                int row = i / static_cast<int>(cols);
                float sx = popup.x + gap + static_cast<float>(col) * (cell + gap);
                float sy = clear_rect.y + clear_rect.height + gap + static_cast<float>(row) * (cell + gap);
                Color c{swatches[i].r, swatches[i].g, swatches[i].b, 255};
                DrawRectangle(static_cast<int>(sx), static_cast<int>(sy), static_cast<int>(cell), static_cast<int>(cell), c);
                DrawRectangleLines(static_cast<int>(sx), static_cast<int>(sy), static_cast<int>(cell), static_cast<int>(cell),
                                    ResolveHlGroup("Border"));
                unsigned char r = swatches[i].r, gc = swatches[i].g, b = swatches[i].b;
                RegisterClickRegion(Rectangle{sx, sy, cell, cell}, [is_highlight, r, gc, b] {
                    if (is_highlight) g_editor.SetOfficeHighlight(r, gc, b); else g_editor.SetOfficeColor(r, gc, b);
                    g_office_dropdown_open = -1;
                });
            }
        } else if (g_office_dropdown_open == kOfficeDropdownSpecialChars) {
            float cell = 32.0f, gap = 4.0f, cols = 8.0f;
            int n = kOfficeSpecialCharCount;
            int rows = (n + static_cast<int>(cols) - 1) / static_cast<int>(cols);
            float pw = cols * (cell + gap) + gap;
            float ph = static_cast<float>(rows) * (cell + gap) + gap;
            Rectangle popup{special_btn.x, content_y, pw, ph};
            draw_popup_bg(popup);
            Font &sym_font = g_office_font_regular;
            for (int i = 0; i < n; i++) {
                int col = i % static_cast<int>(cols);
                int row = i / static_cast<int>(cols);
                float sx = popup.x + gap + static_cast<float>(col) * (cell + gap);
                float sy = popup.y + gap + static_cast<float>(row) * (cell + gap);
                std::string glyph = Utf8FromCodepoint(kOfficeSpecialChars[i]);
                Vector2 gs = MeasureTextEx(sym_font, glyph.c_str(), cell * 0.7f, 0);
                DrawTextEx(sym_font, glyph.c_str(), Vector2{sx + (cell - gs.x) / 2.0f, sy + (cell - gs.y) / 2.0f},
                          cell * 0.7f, 0, ResolveHlGroup("Normal"));
                RegisterClickRegion(Rectangle{sx, sy, cell, cell}, [glyph] {
                    g_editor.InsertOfficeText(glyph);
                    g_office_dropdown_open = -1;
                });
            }
        }

        DrawPaneBorder(x, y, w, h, is_active);
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
        if (is_active && sheet_sess->editing) {
            std::string pre = bar_text.substr(0, std::min<size_t>(sheet_sess->edit_cursor, bar_text.size()));
            float cx = x + 6 + MeasureTextEx(g_font, pre.c_str(), font_size, 0).x;
            DrawRectangle(static_cast<int>(cx), static_cast<int>(content_y + (bar_h - font_size) / 2.0f), 2,
                          static_cast<int>(font_size), ResolveHlGroup("Normal"));
        }
        // Sheet indicator: right-aligned in the formula bar, only shown
        // once there's more than one sheet to disambiguate (Ctrl-PageDown/
        // Ctrl-PageUp switch between them -- see HandleSheetNormalInput).
        // No click-to-switch tab strip in v1 -- same "not attempted this
        // phase" scope cut the plan's own spreadsheet-pane notes make for
        // mouse click-to-select-cell.
        if (sheet_sess->wb.sheets.size() > 1) {
            std::string tab_text = sh.name + "  (" + std::to_string(sheet_sess->active_sheet + 1) + "/" +
                                    std::to_string(sheet_sess->wb.sheets.size()) + ")";
            Vector2 tab_sz = MeasureTextEx(g_font, tab_text.c_str(), font_size, 0);
            DrawTextEx(g_font, tab_text.c_str(), Vector2{x + w - tab_sz.x - 6, content_y + (bar_h - font_size) / 2.0f},
                       font_size, 0, ResolveHlGroup("Comment"));
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
        DrawPaneBorder(x, y, w, h, is_active);
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

    // Sign column: one character wide, *always* reserved (unlike
    // number_w below) for git/LSP-diagnostic/DAP-breakpoint/todo signs
    // (Decoration::sign, set via mep.deco_add) and fold markers, so
    // those stay visible even with both :set number and :set
    // relativenumber off. Previously this whole gutter only existed at
    // all once :set number had reserved it, so every sign-producing
    // feature was invisible unless line numbers happened to be on too.
    float sign_w = g_char_width;
    // :set number/relativenumber -- a right-aligned gutter wide enough
    // for the buffer's largest line number plus one trailing space,
    // drawn once here and used to shift every other x-coordinate below
    // (selection highlight, cursor, and the line text itself) rather
    // than threading it through each of them individually.
    float number_w = 0.0f;
    if (g_editor.ShowLineNumbers() || g_editor.ShowRelativeNumbers()) {
        int digits = 1;
        for (int n = buf.LineCount(); n >= 10; n /= 10) digits++;
        number_w = (digits + 1) * g_char_width;
    }
    float gutter_w = sign_w + number_w;
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

    // Bounded by *visual* slots, not buffer rows: `visible_lines` is how
    // many lines the pane's pixel height fits, but a closed fold collapses
    // however many buffer rows it hides into one of them, so a row-count
    // bound here (this used to be `min(scroll_row + visible_lines,
    // LineCount())`) stopped the loop after only a handful of folds even
    // though most of the pane's height was still blank -- the fewer than
    // `visible_lines` rows scanned could each expand into a fold covering
    // far more than one row apiece. `row` is declared outside the loop so
    // its value after the last iteration -- one past whatever was actually
    // drawn, fold-collapsed or not -- gives the cursor-drawing check below
    // the same visible-range bound this loop itself used, instead of the
    // stale buffer-row-based `last_line`.
    int visual_slot = 0;  // a closed fold collapses N buffer rows into 1 of these
    int row = pane.scroll_row;
    for (; row < buf.LineCount() && visual_slot < visible_lines; row++) {
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

        // Org inline images (<leader>oti / mep.org_images_toggle,
        // Editor::OrgImagesVisible()): a row registered by Lua's
        // mep.org_image_scan() (buf.org_image_rows) renders as a scaled
        // texture instead of its ordinary [[file:...]] text, claiming
        // kOrgInlineImageSlots visual slots instead of 1 -- folds' own
        // mirror image (expand instead of collapse), same "detect on this
        // row, draw a substitute, advance visual_slot by more than one,
        // continue" shape. `!fold_here` keeps this mutually exclusive with
        // the closed-fold branch just below (org's own heading-based
        // folds never start on a src-block/results line in practice, so
        // this never actually has to arbitrate between the two).
        if (!fold_here && g_editor.OrgImagesVisible()) {
            auto img_it = buf.org_image_rows.find(row);
            if (img_it != buf.org_image_rows.end()) {
                float slot_h = static_cast<float>(line_height) * kOrgInlineImageSlots;
                float pane_avail_w = std::max(40.0f, w - (text_x - x) - kMarginX);
                float target_w = kOrgImageLineWidthChars * g_char_width * kOrgImageWidthFraction;
                float avail_w = std::min(pane_avail_w, target_w);
                Texture2D *tex = GetOrLoadOrgInlineImageTexture(img_it->second);
                if (tex) {
                    float scale = std::min(avail_w / static_cast<float>(tex->width),
                                            slot_h / static_cast<float>(tex->height));
                    DrawTextureEx(*tex, Vector2{text_x, ly}, 0.0f, scale, WHITE);
                } else {
                    std::string msg = "[[file: image not found: " + img_it->second + "]]";
                    DrawTextEx(g_font, msg.c_str(), Vector2{text_x, ly}, g_font_size, 0, ResolveHlGroup("Warn"));
                }
                visual_slot += kOrgInlineImageSlots - 1;  // visual_slot++ above already accounted for 1
                continue;  // the for-loop's own `row++` advances past this one row
            }
        }

        // Org LaTeX/math-mode rendering (<leader>otl / mep.org_latex_toggle,
        // Editor::OrgLatexVisible()): mirrors the org-image branch just
        // above, but the target size is the fragment's *own* rendered PNG
        // dimensions (mep_org_latex_render, kBuiltinOrgLatex, already chose
        // a DPI matching the current font size -- see its own comment) --
        // never stretched to a linewidth fraction the way a photo/plot is,
        // since that would blow a bare "$x$" up to the same width as a full
        // page-width figure. `slots` is per-entry (buf.org_latex_rows'
        // OrgLatexRender), not the shared kOrgInlineImageSlots constant.
        // A multi-line fragment's remaining raw source rows (row+1 ..
        // render.end_row) are skipped outright -- not shown as a folded
        // "+-- N lines: ... ---" summary the way an earlier version of
        // this did, since the image already shows everything those rows
        // had to show and a summary line here was just dead weight taking
        // its own slot for no reason.
        if (!fold_here && g_editor.OrgLatexVisible()) {
            auto latex_it = buf.org_latex_rows.find(row);
            if (latex_it != buf.org_latex_rows.end()) {
                const Buffer::OrgLatexRender &render = latex_it->second;
                float slot_h = static_cast<float>(line_height) * render.slots;
                float pane_avail_w = std::max(40.0f, w - (text_x - x) - kMarginX);
                Texture2D *tex = GetOrLoadOrgLatexTexture(render.path);
                if (tex) {
                    float scale = std::min({pane_avail_w / static_cast<float>(tex->width),
                                             slot_h / static_cast<float>(tex->height), 1.0f});
                    DrawTextureEx(*tex, Vector2{text_x, ly}, 0.0f, scale, WHITE);
                } else {
                    std::string msg = "[LaTeX: render not found -- " + render.path + "]";
                    DrawTextEx(g_font, msg.c_str(), Vector2{text_x, ly}, g_font_size, 0, ResolveHlGroup("Warn"));
                }
                visual_slot += render.slots - 1;  // visual_slot++ above already accounted for 1
                row = render.end_row;             // skip the fragment's remaining raw source rows outright
                continue;  // the for-loop's own `row++` advances past this one row
            }
        }

        if (fold_here) {
            int hidden = fold_here->end_row - fold_here->start_row;
            std::string summary = "+-- " + std::to_string(hidden + 1) + " lines: " + buf.lines[row] + " ---";
            DrawTextEx(g_font, summary.c_str(), Vector2{text_x, ly}, g_font_size, 0, ResolveHlGroup("SidebarTitle"));
            // Fold marker click-to-toggle (Phase 11 click-dispatch gap):
            // mep has no separate statuscolumn widget row, so the fold
            // marker lives in the gutter's own trailing-space column
            // (sign_w's slot when number/relativenumber are both off,
            // otherwise number_w's -- either way, always reserved now).
            // Only wired for the active pane -- ToggleFoldAtRow operates
            // on Buf(), the currently active buffer, so a click on a
            // background split's gutter would silently toggle the wrong
            // buffer's fold otherwise.
            if (is_active) {
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
        if (is_active) {
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
        if (number_w > 0.0f) {
            // :set relativenumber: every row but the cursor's own shows
            // its distance from it instead of an absolute number: the
            // cursor line itself still shows its real absolute number
            // (left-aligned -- a "you are here" anchor distinct from
            // every right-aligned relative row around it), matching
            // real Vim/Neovim's "number relativenumber together" look.
            bool current_line = row == pane.cursor.row;
            bool relative = g_editor.ShowRelativeNumbers();
            std::string num = (relative && !current_line) ? std::to_string(std::abs(row - pane.cursor.row))
                                                            : std::to_string(row + 1);
            float num_w = MeasureTextEx(g_font, num.c_str(), g_font_size, 0).x;
            float num_x = (relative && current_line) ? (text_x - number_w) : (text_x - g_char_width - num_w);
            DrawTextEx(g_font, num.c_str(), Vector2{num_x, ly}, g_font_size, 0, ResolveHlGroup("LineNr"));
        }

        // Decorations (Phase 4): whole-line tint first (background,
        // beneath the text), then the base line, then per-span highlight
        // recolor + virtual text on top (foreground, so it's visible over
        // the just-drawn text) -- and finally the gutter sign, drawn into
        // sign_w's always-reserved column (no longer gated on :set
        // number having reserved gutter space -- this was the "always-on
        // sign column" this comment used to call a documented follow-up).
        std::string sign;
        std::string sign_hl;
        bool sign_badge = false;
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
            if (!d.sign.empty() && d.priority > sign_priority) {
                sign = d.sign;
                sign_hl = d.sign_hl;
                sign_badge = d.sign_badge;
                sign_priority = d.priority;
            }
        }
        DrawLineFast(buf.lines[row], text_x, ly, g_font_size, ResolveHlGroup("Normal"));
        for (const Decoration *dp : row_decos) {
            const Decoration &d = *dp;
            if (!d.whole_line && !d.underline && !d.bold && !d.italic && (!d.hl_group.empty() || d.has_fg_color) &&
                d.col_end > d.col_start) {
                const std::string &line = buf.lines[row];
                int a, b;
                if (d.has_fg_color) {
                    // A terminal-color run's col_start/col_end are column
                    // indices (see Decoration::has_fg_color's own
                    // comment), not byte offsets -- convert before
                    // substr-ing, unlike the hl_group branch below, which
                    // keeps the existing byte-offset-as-column assumption
                    // every other decoration producer (tree-sitter, LSP)
                    // already relies on for its own (typically ASCII)
                    // content.
                    a = std::min(static_cast<int>(line.size()), static_cast<int>(ColumnToByteOffset(line, d.col_start)));
                    b = std::min(static_cast<int>(line.size()), static_cast<int>(ColumnToByteOffset(line, d.col_end)));
                } else {
                    a = std::min(static_cast<int>(line.size()), d.col_start);
                    b = std::min(static_cast<int>(line.size()), d.col_end);
                }
                if (b > a) {
                    std::string span = line.substr(a, b - a);
                    // has_fg_color (Editor::EnterTerminalNormalMode's
                    // snapshotted terminal-cell colors) is a literal RGB,
                    // bypassing the named hl_group lookup entirely -- see
                    // Decoration::has_fg_color's own comment for why.
                    Color c = d.has_fg_color ? Color{d.fg_color.r, d.fg_color.g, d.fg_color.b, d.fg_color.a}
                                              : ResolveHlGroup(d.hl_group);
                    // x-position: has_fg_color's `a` is already a byte
                    // offset converted *from* the column index above, so
                    // recovering the column back out of it would just be
                    // undoing that conversion -- use d.col_start (the
                    // column index) directly instead. The hl_group branch
                    // keeps using `a` unchanged (== d.col_start already,
                    // for the byte-offset-as-column content it assumes).
                    float span_x = text_x + (d.has_fg_color ? d.col_start : a) * g_char_width;
                    DrawTextEx(g_font, span.c_str(), Vector2{span_x, ly}, g_font_size, 0, c);
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
            // Per-span strikethrough (org emphasis +strike+, kBuiltinOrg):
            // same primitive as underline just at the span's own vertical
            // middle instead of its baseline.
            if (!d.whole_line && d.strikethrough && d.col_end > d.col_start) {
                const std::string &line = buf.lines[row];
                int a = std::min(static_cast<int>(line.size()), d.col_start);
                int b = std::min(static_cast<int>(line.size()), d.col_end);
                if (b > a) {
                    Color c = d.hl_group.empty() ? ResolveHlGroup("Normal") : ResolveHlGroup(d.hl_group);
                    float ux = text_x + a * g_char_width;
                    float uw = (b - a) * g_char_width;
                    DrawRectangle(static_cast<int>(ux), static_cast<int>(ly + line_height / 2), static_cast<int>(uw),
                                  1, c);
                }
            }
            // Per-span bold (org emphasis *bold*, kBuiltinOrg): g_font is a
            // single JetBrains Mono Regular face (ApplyFontSize, this file)
            // with no real bold weight, so this fakes one the way many
            // terminal emulators/GUI toolkits do for a font family lacking
            // a true bold face -- draw the span twice, the second copy
            // offset 1px right, thickening every stroke slightly.
            if (!d.whole_line && d.bold && d.col_end > d.col_start) {
                const std::string &line = buf.lines[row];
                int a = std::min(static_cast<int>(line.size()), d.col_start);
                int b = std::min(static_cast<int>(line.size()), d.col_end);
                if (b > a) {
                    std::string span = line.substr(a, b - a);
                    Color c = d.hl_group.empty() ? ResolveHlGroup("Normal") : ResolveHlGroup(d.hl_group);
                    float bx = text_x + a * g_char_width;
                    DrawTextEx(g_font, span.c_str(), Vector2{bx, ly}, g_font_size, 0, c);
                    DrawTextEx(g_font, span.c_str(), Vector2{bx + 1, ly}, g_font_size, 0, c);
                }
            }
            // Per-span italic (org emphasis /italic/, kBuiltinOrg): same
            // "no real face for this weight" problem bold has just above,
            // but a slant can't be faked with a second offset draw --
            // instead this pushes a horizontal-shear matrix onto rlgl's
            // transform stack (rlPushMatrix/rlMultMatrixf, rlgl.h) around
            // one DrawTextEx call, which is itself just batched rlgl quads
            // under the hood and so comes out sheared like anything else
            // drawn under that matrix. Sheared around the span's own
            // baseline (translate there, shear, translate back) rather
            // than the world origin, so the glyphs lean without also
            // drifting away from their own line.
            if (!d.whole_line && d.italic && d.col_end > d.col_start) {
                const std::string &line = buf.lines[row];
                int a = std::min(static_cast<int>(line.size()), d.col_start);
                int b = std::min(static_cast<int>(line.size()), d.col_end);
                if (b > a) {
                    std::string span = line.substr(a, b - a);
                    Color c = d.hl_group.empty() ? ResolveHlGroup("Normal") : ResolveHlGroup(d.hl_group);
                    float ix = text_x + a * g_char_width;
                    float span_w = (b - a) * g_char_width;
                    float baseline_y = ly + line_height;
                    // Unlike bold's double-draw (which lands its second
                    // copy 1px from the first, close enough to the
                    // DrawLineFast-drawn original underneath that the
                    // overlap is invisible) or a plain color swap (same
                    // position, same shape), shearing moves the glyph
                    // shape itself -- the sheared redraw doesn't coincide
                    // with the still-upright original DrawLineFast already
                    // drew there, so without covering it first the two
                    // visibly ghost together. Padded a bit past the span's
                    // own column bounds since the shear pushes each
                    // glyph's top edge rightward past its own column
                    // width. Same NormalBg-cover-then-redraw trick the
                    // inline-math renderer uses just above in this same
                    // function, and the same tradeoff: it flattens
                    // whatever whole-line background tint (CursorLine,
                    // Visual selection) this row might have underneath,
                    // just for this span.
                    float pad = line_height * 0.25f;
                    DrawRectangle(static_cast<int>(ix - pad), static_cast<int>(ly), static_cast<int>(span_w + pad * 2),
                                  line_height, ResolveHlGroup("NormalBg"));
                    rlPushMatrix();
                    rlTranslatef(ix, baseline_y, 0);
                    // clang-format off
                    float shear[16] = {
                        1.0f,  0.0f, 0.0f, 0.0f,
                        -0.22f, 1.0f, 0.0f, 0.0f,
                        0.0f,  0.0f, 1.0f, 0.0f,
                        0.0f,  0.0f, 0.0f, 1.0f,
                    };
                    // clang-format on
                    rlMultMatrixf(shear);
                    rlTranslatef(-ix, -baseline_y, 0);
                    DrawTextEx(g_font, span.c_str(), Vector2{ix, ly}, g_font_size, 0, c);
                    rlPopMatrix();
                }
            }
            if (!d.virt_text.empty()) {
                // virt_text_eol: anchored just past the row's own last
                // character (plus one char of breathing room) rather than
                // d.col_start, for an annotation describing the whole
                // line (a diagnostic message) instead of concealing/
                // replacing a specific span -- see the field's own
                // comment (editor.h) for why col_start-anchoring alone
                // painted diagnostic text directly over real buffer text.
                float vx = d.virt_text_eol ? text_x + (static_cast<float>(buf.lines[row].size()) + 1) * g_char_width
                                            : text_x + d.col_start * g_char_width;
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
        // Org inline math (<leader>otl, Editor::OrgLatexVisible()):
        // Buffer::org_latex_inline's own comment explains why this can't
        // reuse the whole-row org_latex_rows path -- a fragment here
        // shares its row with other text, so instead of replacing the
        // row it's painted directly over its own [col_start, col_end)
        // span: first a same-color-as-background rectangle to conceal the
        // raw "$...$" markup underneath (identical trick to a decoration's
        // own virt_overlay, just with a texture standing in for text
        // rather than replacement text), then the fragment's texture.
        // Sized to the line height *only* (not squeezed to fit inside the
        // original span's own pixel width the way an earlier version of
        // this did -- that made a short render look like it was floating,
        // centered, inside an oversized box whenever the raw "$...$"
        // source was longer than its typeset form, which is the common
        // case) and margined by one character-width -- "a single space"
        // -- on each side. The cover itself still has to span at least
        // the *original* [col_start, col_end) no matter how compact the
        // render is, since anything short of col_end is still raw markup
        // that would otherwise show through, so a render more compact
        // than its own source leaves extra covered-but-empty space around
        // it rather than uncovered raw text -- there's no reflowing the
        // fixed-column text after it to close that gap, so the render is
        // centered within whatever the cover ends up being (its own
        // width plus a margin on each side, or the wider original span
        // when that's the bigger of the two) so any such leftover space
        // splits evenly across both sides instead of piling up on one.
        // Only when the render is *wider* than its own source (needs more
        // room than the "$...$" it's replacing had) does the cover expand
        // past the original col_end to fit it -- still a "bleeds into
        // whatever comes next" risk with no reflow to prevent it, but the
        // narrower, common direction is now handled cleanly.
        if (g_editor.OrgLatexVisible()) {
            auto inline_it = buf.org_latex_inline.find(row);
            if (inline_it != buf.org_latex_inline.end()) {
                for (const Buffer::OrgLatexInlineSpan &span : inline_it->second) {
                    float span_x = text_x + span.col_start * g_char_width;
                    float span_w = (span.col_end - span.col_start) * g_char_width;
                    Texture2D *tex = GetOrLoadOrgLatexTexture(span.path);
                    if (!tex) continue;
                    float scale = (static_cast<float>(line_height) * 0.9f) / static_cast<float>(tex->height);
                    float draw_w = tex->width * scale;
                    float draw_h = tex->height * scale;
                    float margin = g_char_width;
                    float cover_w = std::max(span_w, draw_w + margin * 2.0f);
                    float draw_x = span_x + (cover_w - draw_w) / 2.0f;
                    DrawRectangle(static_cast<int>(span_x), static_cast<int>(ly), static_cast<int>(cover_w),
                                  line_height, ResolveHlGroup("NormalBg"));
                    DrawTextureEx(*tex, Vector2{draw_x, ly + (line_height - draw_h) / 2.0f}, 0.0f, scale, WHITE);
                }
            }
        }
        if (!sign.empty()) {
            if (sign_badge) {
                // A filled circle (sign_hl's own color) behind the sign
                // glyph -- e.g. a diagnostic count -- centered in the
                // sign column (sign_w's own one-char width) and on the
                // row's own line height. Text drawn in "NormalBg" punches
                // a legible hole in it, the same contrast trick
                // virt_overlay's own cover rectangle already uses.
                float cx = x + kMarginX + g_char_width / 2.0f;
                float cy = ly + static_cast<float>(line_height) / 2.0f;
                DrawCircle(static_cast<int>(cx), static_cast<int>(cy), g_char_width * 0.58f, ResolveHlGroup(sign_hl));
                float sign_w_text = MeasureUiText(sign, g_font_size);
                DrawUiText(sign, Vector2{cx - sign_w_text / 2.0f, ly}, g_font_size, ResolveHlGroup("NormalBg"));
            } else {
                DrawUiText(sign, Vector2{x + kMarginX, ly}, g_font_size, ResolveHlGroup(sign_hl));
            }
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

    // `row` here is the drawing loop's own variable, left at one past
    // whatever it actually covered (fold-collapsed ranges included) --
    // the same visible-range bound that loop used, so this stays in sync
    // with it by construction instead of via a separately-computed value.
    if (is_active && !IsCommandLineMode(g_editor.CurrentMode()) && pane.cursor.row >= pane.scroll_row &&
        pane.cursor.row < row) {
        // Buffer row -> visual slot, accounting for any closed folds
        // between the top of the view and the cursor (each collapses to 1
        // slot regardless of how many rows it hides -- the cursor itself
        // is never hidden inside one, see ClampCursor), any org inline
        // image (each *expands* to kOrgInlineImageSlots instead -- see the
        // draw loop's own image branch above), and any org LaTeX fragment
        // (expands to its own per-entry slots, org_latex_rows -- see the
        // draw loop's own latex branch above); must stay in exact agreement
        // with all of it and with Editor::UpdateScrollForPane, editor.cpp.
        int cursor_slot = 0;
        for (int r = pane.scroll_row; r < pane.cursor.row;) {
            const Fold *f = nullptr;
            for (const Fold &fold : buf.folds) {
                if (fold.closed && fold.start_row == r) f = &fold;
            }
            auto latex_it = buf.org_latex_rows.find(r);
            if (f) {
                r = f->end_row + 1;
                cursor_slot += 1;
            } else if (g_editor.OrgImagesVisible() && buf.org_image_rows.count(r)) {
                r += 1;
                cursor_slot += kOrgInlineImageSlots;
            } else if (g_editor.OrgLatexVisible() && latex_it != buf.org_latex_rows.end()) {
                r = latex_it->second.end_row + 1;  // skip the fragment's remaining raw source rows outright
                cursor_slot += latex_it->second.slots;
            } else {
                r += 1;
                cursor_slot += 1;
            }
        }
        float cursor_y = content_y + cursor_slot * line_height;
        float cursor_x = text_x + pane.cursor.col * g_char_width;
        // A cursor resting on an image/latex row itself has no meaningful
        // column position (the row's own text is replaced by the texture,
        // not drawn at all) -- an outline around the whole reserved slot
        // stands in for the usual per-character/per-column cursor cell.
        bool cursor_on_image = g_editor.OrgImagesVisible() && buf.org_image_rows.count(pane.cursor.row) != 0;
        auto cursor_latex_it = buf.org_latex_rows.find(pane.cursor.row);
        bool cursor_on_latex = g_editor.OrgLatexVisible() && cursor_latex_it != buf.org_latex_rows.end();
        int cursor_slots = cursor_on_image ? kOrgInlineImageSlots : (cursor_on_latex ? cursor_latex_it->second.slots : 1);
        float row_extent = (cursor_on_image || cursor_on_latex) ? static_cast<float>(line_height) * cursor_slots
                                                                  : static_cast<float>(line_height);
        if (cursor_on_image || cursor_on_latex) {
            float avail_w = std::max(40.0f, w - (text_x - x) - kMarginX);
            DrawRectangleLines(static_cast<int>(text_x), static_cast<int>(cursor_y), static_cast<int>(avail_w),
                                static_cast<int>(row_extent), ResolveHlGroup("Normal"));
        } else if (g_editor.CurrentMode() == Mode::Insert) {
            DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(cursor_y), 2, static_cast<int>(g_font_size),
                          ResolveHlGroup("Normal"));
        } else {
            Color cursor_bg = ResolveHlGroup("Normal");
            DrawRectangle(static_cast<int>(cursor_x), static_cast<int>(cursor_y), static_cast<int>(g_char_width),
                          line_height, Color{cursor_bg.r, cursor_bg.g, cursor_bg.b, 180});
            const std::string &line = buf.lines[pane.cursor.row];
            // Skip the usual "punch the raw character back through the
            // cursor block" redraw when the cursor sits inside a
            // concealed inline-math span (Buffer::org_latex_inline) --
            // that redraw reads straight from the buffer with no idea
            // this column's real content is hidden under a texture, so
            // without this check moving the cursor onto a "$...$" fragment
            // exposed the very markup it's supposed to stay concealed
            // behind.
            bool cursor_in_concealed_latex = false;
            if (g_editor.OrgLatexVisible()) {
                auto it = buf.org_latex_inline.find(pane.cursor.row);
                if (it != buf.org_latex_inline.end()) {
                    for (const Buffer::OrgLatexInlineSpan &span : it->second) {
                        if (pane.cursor.col >= span.col_start && pane.cursor.col < span.col_end) {
                            cursor_in_concealed_latex = true;
                            break;
                        }
                    }
                }
            }
            if (!cursor_in_concealed_latex && pane.cursor.col < static_cast<int>(line.size())) {
                char ch[2] = {line[pane.cursor.col], '\0'};
                DrawTextEx(g_font, ch, Vector2{cursor_x, cursor_y}, g_font_size, 0, ResolveHlGroup("NormalBg"));
            }
        }
        // Completion popup (Phase 22): positioned just below the cursor.
        if (!cursor_on_image && !cursor_on_latex && g_editor.CurrentMode() == Mode::Insert &&
            g_editor.IsCompletionOpen()) {
            DrawCompletionPopup(cursor_x, cursor_y + line_height);
        }
        hover_cursor_x = cursor_x;
        hover_cursor_y = cursor_y + row_extent;
        hover_cursor_valid = true;
    }

    EndScissorMode();

    DrawPaneBorder(x, y, w, h, is_active);
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
// Mirrors mep.nvim's own chrome.tabline (mep.nvim/lua/mep/chrome/
// tabline.lua): a leading mode indicator, then one clickable circle per
// tab -- filled for the active tab, hollow for the rest -- then '+'/'x'
// to open a new tab / close the current one. Real Nerd Font circle/plus/
// times glyphs (g_icon_font via DrawUiText) now that one's loaded --
// mep.nvim's own Unicode bullets (U+25CF/U+25CB) would've rendered as
// tofu through g_font's ASCII-only atlas, which is why these used to be
// plain ASCII '*'/'o' instead.
void DrawTabBar(int y) {
    int screen_w = GetScreenWidth();
    int bar_h = TabBarHeight();
    float font_size = MenuFontSize();
    DrawRectangle(0, y, screen_w, bar_h, ResolveHlGroup("TabBar"));
    float cy = y + (bar_h - font_size) / 2.0f;

    float x = 4;
    std::string mode_label = std::string(" ") + ModeName(g_editor.CurrentMode(), g_editor.IsReplaceMode()) + " ";
    DrawTextEx(g_font, mode_label.c_str(), Vector2{x, cy}, font_size, 0, ResolveHlGroup("StatusLineFg"));
    x += MeasureTextEx(g_font, mode_label.c_str(), font_size, 0).x + 4;

    for (int i = 0; i < g_editor.TabCount(); i++) {
        bool active = (i == g_editor.ActiveTabIndex());
        std::string glyph = " " + Utf8FromCodepoint(active ? 0xf111 : 0xf10c) + " ";  // nf-fa-circle / circle_o
        Color c = ResolveHlGroup(active ? "TabActive" : "TabInactive");
        float w = MeasureUiText(glyph, font_size);
        DrawUiText(glyph, Vector2{x, cy}, font_size, c);
        // Click-to-switch (Phase 11 click-dispatch gap): a click anywhere on
        // this tab's circle jumps straight to it via GoToTab, same as :tabn N.
        RegisterClickRegion(Rectangle{x, static_cast<float>(y), w, static_cast<float>(bar_h)},
                             [i] { g_editor.GoToTab(i); });
        x += w;
    }

    std::string add_label = " " + Utf8FromCodepoint(0xf067) + " ";  // nf-fa-plus
    float add_w = MeasureUiText(add_label, font_size);
    DrawUiText(add_label, Vector2{x, cy}, font_size, ResolveHlGroup("StatusLineFg"));
    RegisterClickRegion(Rectangle{x, static_cast<float>(y), add_w, static_cast<float>(bar_h)}, [] { g_editor.TabNew(""); });
    x += add_w;

    std::string close_label = " " + Utf8FromCodepoint(0xf00d) + " ";  // nf-fa-times
    float close_w = MeasureUiText(close_label, font_size);
    DrawUiText(close_label, Vector2{x, cy}, font_size, ResolveHlGroup("StatusLineFg"));
    RegisterClickRegion(Rectangle{x, static_cast<float>(y), close_w, static_cast<float>(bar_h)},
                         [] { g_editor.TabDelete(); });
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
    // dispatch) is correct even with the one-frame lag. Same reasoning for
    // the pane/tab mouse-interaction geometry lists just below.
    g_click_regions.clear();
    g_pane_screen_rects.clear();
    g_pane_tab_chip_rects.clear();
    g_pane_border_rects.clear();
    g_sidebar_row_rects.clear();
    g_sidebar_border_rects.clear();
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
    // Visible by default (mep.nvim's own showtabline=2), not just once a
    // second tab exists -- Ctrl-T/the tab bar's own '+' button are the
    // discovery path for tabs at all, which a bar that only appears after
    // the fact can't provide.
    bool show_tabs = !zen;
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
        // NOTE: the office pane's own Outline/Format rail+panels are NOT
        // reserved here -- unlike SidebarInstances (app-wide chrome), they
        // dock to the sides of *that specific pane* and are drawn/reserved
        // entirely within DrawPane's own office branch (DrawOfficeSidePanels,
        // called there), the same way its toolbar is per-pane already.
        pane_x = left_w;
        pane_w = std::max(100.0f, screen_w - left_w - right_w);
    }

    if (pane_area_h > 0) {
        if (g_editor.ShouldShowDashboard()) {
            DrawDashboard(pane_x, static_cast<float>(content_top), pane_w, static_cast<float>(pane_area_h));
        } else {
            ComputePaneScreenRects(g_editor.MutableActiveTabRoot(), pane_x, static_cast<float>(content_top), pane_w,
                                    static_cast<float>(pane_area_h));
            DrawPaneTree(g_editor.ActiveTabRoot(), pane_x, static_cast<float>(content_top), pane_w,
                         static_cast<float>(pane_area_h), g_editor.ActivePaneId());
            DrawPaneDragOverlay();
        }
    }

    // Statusline: mode, filename, modified marker, cursor position (of the
    // active pane) -- or, if mep.set_statusline() registered a widget-list
    // callback (Phase 11), that instead: segments drawn left-to-right, each
    // through its own `hl` (falling back to "StatusLineFg"). Hidden
    // entirely in zen mode. An office (docx/odt) pane draws its own Docs-
    // style status footer inside DrawPane instead (scoped to that pane, not
    // this app-wide bar), so this stays the plain vim-style line always --
    // see DrawPane's office branch for the footer.
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
            // Collaboration presence stays in the editor's ordinary chrome,
            // not a modal: each compact chip identifies a peer and their
            // shared cursor location. Clicking a chip jumps there.
            const auto collaborators = g_editor.CollaborationPeers();
            float peer_x = static_cast<float>(kMarginX) + MeasureTextEx(g_font, left.c_str(), status_font_size, 0).x + 18.0f;
            for (const auto &peer : collaborators) {
                std::string chip = peer.name.empty() ? "Anonymous" : peer.name;
                chip += peer.has_location ? " @" + std::to_string(peer.row + 1) + ":" + std::to_string(peer.col + 1) : " connecting";
                float chip_w = MeasureTextEx(g_font, chip.c_str(), status_font_size, 0).x + 14.0f;
                const float right_start = static_cast<float>(screen_w - kMarginX) - MeasureTextEx(g_font, right.c_str(), status_font_size, 0).x;
                if (peer_x + chip_w + 8.0f >= right_start) break;
                Rectangle chip_rect{peer_x, static_cast<float>(status_y + 2), chip_w, static_cast<float>(status_bar_height - 4)};
                DrawRectangleRec(chip_rect, ResolveHlGroup("Visual"));
                DrawTextEx(g_font, chip.c_str(), Vector2{peer_x + 7.0f, static_cast<float>(status_y + 3)}, status_font_size, 0, ResolveHlGroup("StatusLineFg"));
                RegisterClickRegion(chip_rect, [peer_id = peer.id] { g_editor.JumpToCollaborator(peer_id); });
                peer_x += chip_w + 5.0f;
            }
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
        {
            float cx = kMarginX + MeasureTextEx(g_font, line.c_str(), g_font_size, 0).x;
            DrawRectangle(static_cast<int>(cx), cmd_y + 3, 2, static_cast<int>(g_font_size), ResolveHlGroup("Normal"));
        }
    } else if (!g_editor.StatusMessage().empty()) {
        DrawTextEx(g_font, g_editor.StatusMessage().c_str(),
                   Vector2{static_cast<float>(kMarginX), static_cast<float>(cmd_y + 3)}, g_font_size, 0, ResolveHlGroup("Normal"));
    }

    if (show_tabs) DrawTabBar(menu_bar_height);
    // Sidebars are persistent chrome (a neighboring pane you can focus/
    // resize, per the pane_x/pane_w reservation above) rather than a
    // transient dialog, so they belong on this same layer -- drawn before
    // every modal overlay below, not after, so a floating overlay (the
    // which-key hover included) always sits on top of an open sidebar
    // instead of being painted over by it.
    if (!zen) DrawSidebars();
    // DrawMenuBar draws both the bar itself *and* its open dropdown (if
    // any) in one call -- moved here, after DrawSidebars, for the
    // dropdown's sake: same reasoning as the comment above (a File/Edit/...
    // dropdown is exactly this kind of floating overlay, and the File menu
    // in particular opens right where a left-docked file tree sits, so it
    // used to paint underneath one). The bar row itself (y in
    // [0, menu_bar_height)) is never touched by DrawSidebars either way --
    // sidebars start at content_top, below both the menu bar and tab bar --
    // so moving the whole call has no effect on the bar's own draw order.
    if (!zen) DrawMenuBar();
    // Same reasoning as the comment just above (drawn after sidebars, not
    // before, so it sits on top instead of being painted over by one) --
    // this used to be drawn inline with the command-line text itself,
    // *before* DrawSidebars, so an open sidebar (a file tree, most
    // visibly) would paint right over the left edge of this popup, e.g.
    // ":e <Tab>"'s path completion cut off mid-list.
    if (g_editor.CurrentMode() == Mode::Command && g_editor.IsCmdlineCompletionOpen()) {
        DrawCmdlineCompletionPopup(static_cast<float>(kMarginX), static_cast<float>(cmd_y));
    }
    if (g_editor.CurrentMode() == Mode::Prompt) DrawPromptOverlay();
    if (g_editor.CurrentMode() == Mode::Confirm) DrawConfirmOverlay();
    if (g_editor.CurrentMode() == Mode::Select) DrawSelectOverlay();
    if (g_editor.CurrentMode() == Mode::Preview) DrawPreviewOverlay();
    if (g_editor.CurrentMode() == Mode::Picker) DrawPickerOverlay();
    if (g_editor.CurrentMode() == Mode::RoamGraph) DrawRoamGraphOverlay();
    if (g_editor.CurrentMode() == Mode::WhichKey) DrawWhichKeyOverlay();
    if (g_show_help_overlay) DrawHelpOverlay();
    DrawToastStack();

    EndDrawing();
}

// Shared by DispatchChromeClicks and UpdatePaneMouseInteraction: any of
// these modal overlay modes already captures input itself and draws over
// the normal pane/chrome content beneath it, so a click/drag meant for
// the overlay must not also fall through to e.g. switch tabs or start
// resizing a border underneath it.
bool IsModalOverlayMode(Mode m) {
    switch (m) {
        case Mode::Picker:
        case Mode::RoamGraph:
        case Mode::Sidebar:
        case Mode::Prompt:
        case Mode::Confirm:
        case Mode::Select:
        case Mode::WhichKey:
        case Mode::Preview:
            return true;
        default:
            return false;
    }
}

// Consumes this frame's mouse click (if any) against whatever click regions
// DrawEditor() just registered (tab bar, gutter fold markers, pane header
// breadcrumb, pane-body focus -- see g_click_regions above). First matching
// region wins, mirroring the menu bar's own one-hit-per-click dispatch.
// Same modal-overlay gate as UpdatePaneMouseInteraction, EXCEPT for
// Mode::Sidebar (see that function's own comment for why): a pane-body
// click region is exactly how focus leaves a sidebar back into the pane
// tree, so this dispatcher must still run while a sidebar has focus.
void DispatchChromeClicks() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Mode mode = g_editor.CurrentMode();
    if (IsModalOverlayMode(mode) && mode != Mode::Sidebar) return;
    Vector2 mouse = GetMousePosition();
    for (const ClickRegion &r : g_click_regions) {
        if (PointInRect(mouse, r.rect)) {
            r.action();
            return;
        }
    }
}

// Drives the pane-border-resize and tab-chip-drag-and-drop state machines
// (g_pane_drag) every frame: starts a drag on mouse-down over a border/
// chip (once the mouse has moved past kPaneDragThresholdPx -- short of
// that, it might still just be a plain click, which g_click_regions/
// DispatchChromeClicks already handles independently), updates it every
// frame the button stays down, and commits it on release. Also owns
// cursor-shape feedback (hover, not just mid-drag) -- reset to
// MOUSE_CURSOR_DEFAULT every frame first since raylib never does that on
// its own. Same modal-overlay gate as DispatchChromeClicks, EXCEPT for
// Mode::Sidebar: that mode means a sidebar has keyboard focus, which is
// precisely when its rows/border need to keep responding to the mouse
// (unlike the other modal modes here, a focused sidebar isn't drawn over
// separately from -- it *is* -- the chrome this function targets). If a
// genuinely modal mode is somehow entered mid-drag (some keybinding fired
// while the mouse was still down), the in-progress drag is simply
// abandoned rather than left to resolve against stale geometry.
// Drives KanbanSession's own press/move-threshold/release drag state
// (scoped per-buffer inside the session itself, unlike g_pane_drag above --
// see KanbanSession's own comment for why) every frame the Kanban view is
// the focused pane's active mode. Hit-testing uses the exact same column-
// width/card-height layout constants DrawKanban lays cards out with,
// against KanbanSession::content_x/y/w/h (refreshed every frame DrawKanban
// runs), so the two can never drift apart.
void UpdateKanbanMouseInteraction() {
    if (g_editor.CurrentMode() != Mode::KanbanNormal) return;
    int buffer_id = g_editor.CurrentBufferId();
    KanbanSession *sess = g_editor.GetKanbanMutable(buffer_id);
    if (!sess || sess->content_w <= 0 || sess->content_h <= 0) return;
    Vector2 mouse = GetMousePosition();
    Rectangle content{sess->content_x, sess->content_y, sess->content_w, sess->content_h};
    int header_h = PaneHeaderHeight();

    // A card area row's top, in Y offset from content_y -- two header_h
    // rows (the toolbar + the per-column headers) plus the card gap, must
    // match DrawKanban's own `card_y = y + header_h * 2 + kKanbanCardGap`
    // exactly.
    float card_area_top = static_cast<float>(header_h) * 2.0f + kKanbanCardGap;

    if (!sess->dragging) {
        if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || !PointInRect(mouse, content)) return;
        Rectangle chip_rect{sess->new_card_chip_x, sess->new_card_chip_y, sess->new_card_chip_w,
                             sess->new_card_chip_h};
        if (sess->new_card_chip_w > 0 && PointInRect(mouse, chip_rect)) {
            sess->dragging = true;
            sess->drag_is_new_card = true;
            sess->drag_headline_index = -1;
            sess->drag_start_x = mouse.x;
            sess->drag_start_y = mouse.y;
            sess->drag_threshold_passed = false;
            sess->drop_column = -1;
            sess->drop_row = 0;
            return;
        }
        std::vector<std::string> columns = g_editor.KanbanColumns(buffer_id);
        int col_i = static_cast<int>((mouse.x - sess->content_x) / kKanbanColumnWidth);
        if (col_i < 0 || col_i >= static_cast<int>(columns.size())) return;
        float rel_content_y = mouse.y - sess->content_y;
        // Start column drags only from the header label, never from its
        // kebab menu (which remains an ordinary click target).
        if (rel_content_y >= header_h && rel_content_y < header_h * 2 &&
            mouse.x < sess->content_x + (col_i + 1) * kKanbanColumnWidth - 24) {
            sess->dragging = true;
            sess->drag_is_column = true;
            sess->drag_column_index = col_i;
            sess->drag_start_x = mouse.x;
            sess->drag_start_y = mouse.y;
            sess->drag_threshold_passed = false;
            sess->column_drop_slot = col_i;
            return;
        }
        std::vector<int> cards = g_editor.KanbanCardsInColumn(buffer_id, col_i);
        float rel_y = mouse.y - sess->content_y - card_area_top;
        int row_i = static_cast<int>(rel_y / (kKanbanCardHeight + kKanbanCardGap));
        if (row_i < 0 || row_i >= static_cast<int>(cards.size())) return;
        float card_top = card_area_top + row_i * (kKanbanCardHeight + kKanbanCardGap);
        float rel_y_in_card = mouse.y - sess->content_y - card_top;
        if (rel_y_in_card < 0 || rel_y_in_card > kKanbanCardHeight) return;
        sess->dragging = true;
        sess->drag_is_new_card = false;
        sess->drag_headline_index = cards[row_i];
        sess->drag_start_x = mouse.x;
        sess->drag_start_y = mouse.y;
        sess->drag_threshold_passed = false;
        sess->drop_column = col_i;
        sess->drop_row = row_i;
        return;
    }

    constexpr float kDragThresholdPx = 4.0f;
    if (!sess->drag_threshold_passed) {
        float dx = mouse.x - sess->drag_start_x, dy = mouse.y - sess->drag_start_y;
        if (dx * dx + dy * dy > kDragThresholdPx * kDragThresholdPx) sess->drag_threshold_passed = true;
    }
    if (sess->drag_threshold_passed) {
        std::vector<std::string> columns = g_editor.KanbanColumns(buffer_id);
        if (sess->drag_is_column) {
            // Snap to the nearest column edge. The resulting slot is in the
            // pre-move order, exactly what KanbanMoveColumn accepts.
            float relative_x = mouse.x - sess->content_x;
            sess->column_drop_slot = std::clamp(
                static_cast<int>(std::floor((relative_x + kKanbanColumnWidth / 2.0f) / kKanbanColumnWidth)), 0,
                static_cast<int>(columns.size()));
        } else {
        int col_i = std::clamp(static_cast<int>((mouse.x - sess->content_x) / kKanbanColumnWidth), 0,
                                 static_cast<int>(columns.size()) - 1);
        std::vector<int> cards = g_editor.KanbanCardsInColumn(buffer_id, col_i);
        float rel_y = mouse.y - sess->content_y - card_area_top + (kKanbanCardHeight + kKanbanCardGap) / 2.0f;
        int row_i = std::clamp(static_cast<int>(rel_y / (kKanbanCardHeight + kKanbanCardGap)), 0,
                                 static_cast<int>(cards.size()));
        sess->drop_column = col_i;
        sess->drop_row = row_i;
        }
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        std::vector<std::string> columns = g_editor.KanbanColumns(buffer_id);
        if (sess->drag_threshold_passed && sess->drag_is_column) {
            g_editor.KanbanMoveColumn(sess->drag_column_index, sess->column_drop_slot);
        } else if (sess->drag_threshold_passed && sess->drag_is_new_card && sess->drop_column >= 0 &&
            sess->drop_column < static_cast<int>(columns.size())) {
            int hi = g_editor.KanbanNewCard(columns[sess->drop_column], "New card");
            if (hi >= 0) g_editor.KanbanBeginRenameNewCard(hi);
        } else if (sess->drag_threshold_passed && !sess->drag_is_new_card && sess->drag_headline_index >= 0 &&
                   sess->drag_headline_index < static_cast<int>(sess->outline.headlines.size()) &&
                   sess->drop_column >= 0) {
            int hi = sess->drag_headline_index;
            std::string old_keyword = sess->outline.headlines[hi].todo_keyword;
            std::string new_keyword =
                (sess->drop_column < static_cast<int>(columns.size())) ? columns[sess->drop_column] : old_keyword;
            if (new_keyword != old_keyword) {
                // Cross-column drop: changes status only -- the card's
                // position within its new column follows natural document
                // order rather than the exact drop_row (a documented v1
                // simplification; only a same-column drag gets an exact
                // reorder, just below).
                g_editor.KanbanSetCardColumn(hi, new_keyword);
            } else {
                std::vector<int> cards = g_editor.KanbanCardsInColumn(buffer_id, sess->drop_column);
                int before;
                if (sess->drop_row >= 0 && sess->drop_row < static_cast<int>(cards.size())) {
                    before = cards[sess->drop_row];
                } else if (!cards.empty() && cards.back() + 1 < static_cast<int>(sess->outline.headlines.size())) {
                    // Dropped past this column's last card: land right
                    // after it (the next headline in *document* order,
                    // regardless of its own column) rather than jumping to
                    // the very end of the file -- KanbanMoveCardBefore's
                    // own -1 case is reserved for "there's nothing after
                    // it at all".
                    before = cards.back() + 1;
                } else {
                    before = -1;
                }
                if (before != hi) g_editor.KanbanMoveCardBefore(hi, before);
            }
        }
        sess->dragging = false;
        sess->drag_is_new_card = false;
        sess->drag_is_column = false;
        sess->drag_headline_index = -1;
        sess->drag_column_index = -1;
        sess->drag_threshold_passed = false;
        sess->drop_column = -1;
        sess->drop_row = -1;
        sess->column_drop_slot = -1;
    }
}

// Same shape as UpdateKanbanMouseInteraction, for a Gantt bar's body
// (Move), either edge (ResizeStart/ResizeEnd), or -- for a milestone with
// no DEADLINE yet -- the last edge_zone px of its 40px virtual hit-box,
// which starts a ResizeEnd drag that *creates* one (GanttSetHeadlineDate
// already writes a brand-new DEADLINE: line for this case; DrawGantt's own
// live-preview math already falls back to drag_orig_scheduled when
// drag_orig_deadline is absent, mirrored below at release time). Unlike
// Kanban's column/row snap-to-slot drop, the live delta is continuous
// pixels-to-days (rounded), matching DrawGantt's own live preview math
// exactly so the bar never visibly jumps at release.
void UpdateGanttMouseInteraction() {
    if (g_editor.CurrentMode() != Mode::GanttNormal) return;
    int buffer_id = g_editor.CurrentBufferId();
    GanttSession *sess = g_editor.GetGanttMutable(buffer_id);
    if (!sess || sess->content_w <= 0 || sess->content_h <= 0) return;
    Vector2 mouse = GetMousePosition();
    Rectangle content{sess->content_x, sess->content_y, sess->content_w, sess->content_h};
    float label_w = sess->label_col_w;
    float divider_x = sess->content_x + label_w;
    float ruler_h = static_cast<float>(GanttRulerHeight(*sess));
    int row_h = GanttRowHeight();

    // The label/timeline divider takes priority over everything else --
    // resizing it while a bar happens to sit right at that x would
    // otherwise be ambiguous.
    if (sess->resizing_label_col) {
        SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            sess->label_col_w = std::clamp(mouse.x - sess->content_x, 80.0f, sess->content_w - 120.0f);
        } else {
            sess->resizing_label_col = false;
        }
        return;
    }
    if (!sess->dragging && PointInRect(mouse, content) && std::abs(mouse.x - divider_x) <= 4.0f &&
        mouse.y >= sess->content_y + ruler_h) {
        SetMouseCursor(MOUSE_CURSOR_RESIZE_EW);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            sess->resizing_label_col = true;
            return;
        }
    }

    if (!sess->dragging) {
        bool left_pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        bool right_pressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        if ((!left_pressed && !right_pressed) || !PointInRect(mouse, content)) return;
        if (mouse.y < sess->content_y + ruler_h) return;
        std::vector<int> rows = g_editor.GanttRows(buffer_id);
        int row_i = static_cast<int>((mouse.y - sess->content_y - ruler_h) / row_h);
        if (row_i < 0 || row_i >= static_cast<int>(rows.size())) return;
        int hi = rows[row_i];

        // A click in the label column (left of the divider): double-click
        // renames the row inline, otherwise it's just a focus click --
        // either way it's not a bar drag, so handle and return.
        if (mouse.x < divider_x) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return;
            bool has_children = false;
            for (const OrgHeadline &candidate : sess->outline.headlines) {
                if (candidate.parent_index == hi) { has_children = true; break; }
            }
            float toggle_right = sess->content_x + 6 + std::max(0, sess->outline.headlines[hi].level - 1) * 14.0f + 14;
            if (has_children && mouse.x <= toggle_right) {
                if (sess->collapsed_headlines.count(hi)) sess->collapsed_headlines.erase(hi);
                else sess->collapsed_headlines.insert(hi);
                return;
            }
            double now = GetTime();
            bool is_double =
                g_last_gantt_click_headline == hi && (now - g_last_gantt_click_time) < kDoubleClickThresholdSec;
            sess->focused_row = row_i;
            if (is_double) {
                g_editor.GanttBeginRename(hi);
                g_last_gantt_click_time = -1.0;
                g_last_gantt_click_headline = -1;
            } else {
                g_last_gantt_click_time = now;
                g_last_gantt_click_headline = hi;
            }
            return;
        }

        const OrgHeadline &hd = sess->outline.headlines[hi];
        long long start_day = OrgDayNumber(hd.scheduled.year, hd.scheduled.month, hd.scheduled.day);
        long long clicked_day = sess->anchor_day + static_cast<long long>(std::floor((mouse.x - divider_x) / sess->pixels_per_day));
        sess->focused_row = row_i;
        // Right-click is a direct deadline picker, including for a
        // milestone with no deadline yet. The editor primitive validates
        // that it remains after the scheduled start date.
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            g_editor.GanttSetHeadlineDate(hi, /*is_deadline=*/true, clicked_day);
            return;
        }
        float bar_x = divider_x + static_cast<float>(start_day - sess->anchor_day) * sess->pixels_per_day;
        float bar_w = 40.0f;
        if (hd.deadline.present) {
            long long end_day = OrgDayNumber(hd.deadline.year, hd.deadline.month, hd.deadline.day);
            bar_w = std::max(4.0f, static_cast<float>(end_day - start_day) * sess->pixels_per_day);
        }
        // A left-click anywhere on a row's timeline directly chooses its
        // scheduled start. Starting on the existing bar still enters the
        // drag path below, so a horizontal drag continues to move/resize it.
        if (mouse.x < bar_x - 6 || mouse.x > bar_x + bar_w + 6) {
            g_editor.GanttSetHeadlineDate(hi, /*is_deadline=*/false, clicked_day);
            return;
        }
        float edge_zone = std::min(8.0f, bar_w / 3.0f);
        sess->dragging = true;
        sess->drag_headline_index = hi;
        sess->drag_start_x = mouse.x;
        sess->drag_orig_scheduled = hd.scheduled;
        sess->drag_orig_deadline = hd.deadline;
        if (mouse.x >= bar_x + bar_w - edge_zone) {
            // Right edge, whether or not a DEADLINE exists yet -- with none
            // yet, this is how one gets created (see GanttSetHeadlineDate).
            sess->drag_mode = GanttSession::DragMode::ResizeEnd;
        } else if (hd.deadline.present && mouse.x <= bar_x + edge_zone) {
            sess->drag_mode = GanttSession::DragMode::ResizeStart;
        } else {
            sess->drag_mode = GanttSession::DragMode::Move;
        }
        return;
    }

    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        constexpr float kDragThresholdPx = 4.0f;
        bool threshold_passed = std::abs(mouse.x - sess->drag_start_x) > kDragThresholdPx;
        if (threshold_passed && sess->drag_headline_index >= 0) {
            int delta_days = static_cast<int>(std::lround((mouse.x - sess->drag_start_x) / sess->pixels_per_day));
            if (delta_days != 0) {
                if (sess->drag_mode == GanttSession::DragMode::Move) {
                    g_editor.GanttShiftHeadline(sess->drag_headline_index, delta_days);
                } else {
                    // ResizeEnd falls back to drag_orig_scheduled when no
                    // DEADLINE existed at drag-start (creating one from
                    // scratch) -- drag_orig_deadline would otherwise be an
                    // absent/zeroed timestamp (day 0), matching DrawGantt's
                    // own live-preview fallback so the bar never jumps.
                    const OrgTimestamp &orig =
                        (sess->drag_mode == GanttSession::DragMode::ResizeStart)
                            ? sess->drag_orig_scheduled
                            : (sess->drag_orig_deadline.present ? sess->drag_orig_deadline : sess->drag_orig_scheduled);
                    long long orig_day = OrgDayNumber(orig.year, orig.month, orig.day);
                    g_editor.GanttSetHeadlineDate(sess->drag_headline_index,
                                                    sess->drag_mode == GanttSession::DragMode::ResizeEnd,
                                                    orig_day + delta_days);
                }
            }
        } else if (sess->drag_headline_index >= 0) {
            // A short left click on an existing bar is the direct start-date
            // picker counterpart to the right-click deadline picker above.
            long long clicked_day = sess->anchor_day + static_cast<long long>(
                std::floor((sess->drag_start_x - divider_x) / sess->pixels_per_day));
            g_editor.GanttSetHeadlineDate(sess->drag_headline_index, /*is_deadline=*/false, clicked_day);
        }
        sess->dragging = false;
        sess->drag_headline_index = -1;
    }
}

// Scrollbar drag/click-to-jump for the office pane -- same "Draw computes
// this frame's geometry into a global, a sibling Update* consumes it"
// split as UpdateKanbanMouseInteraction/UpdateGanttMouseInteraction, since
// this scrollbar (like those two) needs freshly-refreshed per-frame
// geometry (g_office_status, set by DrawPane's office branch) rather than
// its own independently-tracked layout.
void UpdateOfficeScrollbarInteraction() {
    if (!g_office_status.valid) {
        g_office_scroll_drag.active = false;
        return;
    }
    const OfficeSession *office_sess = g_editor.GetOffice(g_office_status.buffer_id);
    if (!office_sess || office_sess->doc.paragraphs.empty()) {
        g_office_scroll_drag.active = false;
        return;
    }
    const Rectangle &track = g_office_status.track;
    const Rectangle &thumb = g_office_status.thumb;
    Vector2 mouse = GetMousePosition();

    // Converts a target 0..1 fraction of the document's total wrapped
    // height into a (scroll_para, scroll_line_in_para) pair by walking
    // paragraphs from the top -- the inverse of DrawPane's own
    // height-at-scroll accumulation, reusing the same cached max_width/
    // body_size layout inputs (g_office_status) so both walks agree.
    auto scroll_to_fraction = [&](float fraction) {
        fraction = std::clamp(fraction, 0.0f, 1.0f);
        float target_h = fraction * g_office_status.total_height;
        const OfficeDoc &doc = office_sess->doc;
        int para_count = static_cast<int>(doc.paragraphs.size());
        float acc = 0.0f;
        for (int pi = 0; pi < para_count; pi++) {
            const DocParagraph &para = doc.paragraphs[pi];
            float size = g_office_status.body_size * OfficeHeadingMultiplier(para.heading_level);
            float plh = size * 1.35f;
            int line_count = std::max(
                1, static_cast<int>(WordWrapOfficeParagraph(para, g_office_status.max_width, size).size()));
            float para_h = plh * static_cast<float>(line_count) +
                           OfficeParagraphExtraHeight(doc, para, g_office_status.max_width, g_office_status.body_size);
            if (acc + para_h > target_h || pi == para_count - 1) {
                int line = std::clamp(static_cast<int>((target_h - acc) / plh), 0, line_count - 1);
                g_editor.SetOfficeScroll(g_office_status.buffer_id, pi, line);
                return;
            }
            acc += para_h;
        }
    };

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (CheckCollisionPointRec(mouse, thumb)) {
            g_office_scroll_drag.active = true;
            g_office_scroll_drag.grab_offset_y = mouse.y - thumb.y;
        } else if (CheckCollisionPointRec(mouse, track)) {
            float target_top_y = mouse.y - thumb.height * 0.5f;
            float fraction = (track.height > thumb.height) ? (target_top_y - track.y) / (track.height - thumb.height) : 0.0f;
            scroll_to_fraction(fraction);
        }
    }
    if (g_office_scroll_drag.active) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float new_top_y = mouse.y - g_office_scroll_drag.grab_offset_y;
            float fraction = (track.height > thumb.height) ? (new_top_y - track.y) / (track.height - thumb.height) : 0.0f;
            scroll_to_fraction(fraction);
        } else {
            g_office_scroll_drag.active = false;
        }
    }
}

void UpdatePaneMouseInteraction() {
    Mode mode = g_editor.CurrentMode();
    if (IsModalOverlayMode(mode) && mode != Mode::Sidebar) {
        g_pane_drag = PaneDragState{};
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
        return;
    }

    Vector2 mouse = GetMousePosition();
    MouseCursor want_cursor = MOUSE_CURSOR_DEFAULT;

    if (g_pane_drag.kind == PaneDragKind::None) {
        bool over_border = false;
        for (const PaneBorderRect &b : g_pane_border_rects) {
            if (PointInRect(mouse, b.grab_rect)) {
                over_border = true;
                want_cursor = b.vertical ? MOUSE_CURSOR_RESIZE_EW : MOUSE_CURSOR_RESIZE_NS;
                break;
            }
        }
        if (!over_border) {
            for (const SidebarBorderRect &sb : g_sidebar_border_rects) {
                if (PointInRect(mouse, sb.grab_rect)) {
                    over_border = true;
                    want_cursor = sb.horizontal ? MOUSE_CURSOR_RESIZE_EW : MOUSE_CURSOR_RESIZE_NS;
                    break;
                }
            }
        }
        if (!over_border) {
            for (const PaneTabChipRect &c : g_pane_tab_chip_rects) {
                if (PointInRect(mouse, c.rect)) {
                    want_cursor = MOUSE_CURSOR_POINTING_HAND;
                    break;
                }
            }
        }
        if (!over_border && want_cursor == MOUSE_CURSOR_DEFAULT) {
            for (const SidebarRowRect &r : g_sidebar_row_rects) {
                if (PointInRect(mouse, r.rect)) {
                    want_cursor = MOUSE_CURSOR_POINTING_HAND;
                    break;
                }
            }
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            for (const PaneBorderRect &b : g_pane_border_rects) {
                if (!PointInRect(mouse, b.grab_rect)) continue;
                g_pane_drag.kind = PaneDragKind::BorderResize;
                g_pane_drag.start_pos = mouse;
                g_pane_drag.threshold_passed = false;
                g_pane_drag.border_node = b.node;
                g_pane_drag.border_child_index = b.child_index;
                g_pane_drag.border_vertical = b.vertical;
                g_pane_drag.border_pair_total = g_editor.PaneBorderPairTotal(b.node, b.child_index);
                g_pane_drag.border_pair_rect = b.pair_rect;
                break;
            }
            if (g_pane_drag.kind == PaneDragKind::None) {
                for (const SidebarBorderRect &sb : g_sidebar_border_rects) {
                    if (!PointInRect(mouse, sb.grab_rect)) continue;
                    g_pane_drag.kind = PaneDragKind::SidebarResize;
                    g_pane_drag.start_pos = mouse;
                    g_pane_drag.threshold_passed = false;
                    g_pane_drag.sidebar_id = sb.sidebar_id;
                    g_pane_drag.sidebar_horizontal = sb.horizontal;
                    g_pane_drag.sidebar_sign = sb.sign;
                    g_pane_drag.sidebar_size_start = 0;
                    for (const SidebarInstance &inst : g_editor.Sidebars()) {
                        if (inst.id == sb.sidebar_id) {
                            g_pane_drag.sidebar_size_start = inst.size;
                            break;
                        }
                    }
                    g_pane_drag.sidebar_mouse_start = sb.horizontal ? mouse.x : mouse.y;
                    break;
                }
            }
            if (g_pane_drag.kind == PaneDragKind::None) {
                for (const PaneTabChipRect &c : g_pane_tab_chip_rects) {
                    if (!PointInRect(mouse, c.rect)) continue;
                    g_pane_drag.kind = PaneDragKind::TabMove;
                    g_pane_drag.start_pos = mouse;
                    g_pane_drag.threshold_passed = false;
                    g_pane_drag.source_pane_id = c.pane_id;
                    g_pane_drag.dragged_buffer_id = c.buffer_id;
                    g_pane_drag.target_pane_id = -1;
                    break;
                }
            }
            // Sidebar rows don't drag (only click-to-select and double-
            // click-to-activate) -- handled directly here rather than via
            // g_click_regions/DispatchChromeClicks, since double-click
            // detection needs to compare against the *previous* click's
            // own time/position, state g_click_regions' plain fire-once
            // closures have no way to carry.
            if (g_pane_drag.kind == PaneDragKind::None) {
                for (const SidebarRowRect &r : g_sidebar_row_rects) {
                    if (!PointInRect(mouse, r.rect)) continue;
                    double now = GetTime();
                    bool is_double = g_last_sidebar_click_id == r.sidebar_id && g_last_sidebar_click_row == r.line_index &&
                                      (now - g_last_sidebar_click_time) < kDoubleClickThresholdSec;
                    g_editor.FocusSidebarRow(r.sidebar_id, r.line_index);
                    if (is_double) {
                        g_editor.ActivateSidebarLine(r.sidebar_id, r.line_index);
                        // A third rapid click starts fresh rather than
                        // counting as yet another double-click against
                        // this same activation.
                        g_last_sidebar_click_time = -1.0;
                        g_last_sidebar_click_id = -1;
                        g_last_sidebar_click_row = -1;
                    } else {
                        g_last_sidebar_click_time = now;
                        g_last_sidebar_click_id = r.sidebar_id;
                        g_last_sidebar_click_row = r.line_index;
                    }
                    break;
                }
            }
        }
    } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!g_pane_drag.threshold_passed) {
            float dx = mouse.x - g_pane_drag.start_pos.x, dy = mouse.y - g_pane_drag.start_pos.y;
            if (dx * dx + dy * dy > kPaneDragThresholdPx * kPaneDragThresholdPx) g_pane_drag.threshold_passed = true;
        }
        if (g_pane_drag.threshold_passed) {
            if (g_pane_drag.kind == PaneDragKind::BorderResize) {
                want_cursor = g_pane_drag.border_vertical ? MOUSE_CURSOR_RESIZE_EW : MOUSE_CURSOR_RESIZE_NS;
                const Rectangle &r = g_pane_drag.border_pair_rect;
                float frac = g_pane_drag.border_vertical ? (mouse.x - r.x) / r.width : (mouse.y - r.y) / r.height;
                g_editor.SetPaneBorderShare(g_pane_drag.border_node, g_pane_drag.border_child_index,
                                             std::clamp(frac, 0.0f, 1.0f) * g_pane_drag.border_pair_total);
            } else if (g_pane_drag.kind == PaneDragKind::SidebarResize) {
                want_cursor = g_pane_drag.sidebar_horizontal ? MOUSE_CURSOR_RESIZE_EW : MOUSE_CURSOR_RESIZE_NS;
                float mouse_now = g_pane_drag.sidebar_horizontal ? mouse.x : mouse.y;
                float delta_px = (mouse_now - g_pane_drag.sidebar_mouse_start) * g_pane_drag.sidebar_sign;
                float unit = g_pane_drag.sidebar_horizontal ? g_char_width : static_cast<float>(LineHeight());
                int new_size = g_pane_drag.sidebar_size_start + static_cast<int>(std::lround(delta_px / unit));
                g_editor.SetSidebarSize(g_pane_drag.sidebar_id, new_size);
            } else if (g_pane_drag.kind == PaneDragKind::TabMove) {
                want_cursor = MOUSE_CURSOR_POINTING_HAND;
                g_pane_drag.target_pane_id = -1;
                for (const PaneScreenRect &p : g_pane_screen_rects) {
                    if (PointInRect(mouse, p.rect)) {
                        g_pane_drag.target_pane_id = p.pane_id;
                        g_pane_drag.drop_zone = ComputePaneDropZone(mouse, p.rect);
                        break;
                    }
                }
            }
        }
    } else if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (g_pane_drag.threshold_passed && g_pane_drag.kind == PaneDragKind::TabMove &&
            g_pane_drag.target_pane_id >= 0) {
            if (g_pane_drag.drop_zone == PaneDropZone::Center) {
                g_editor.MoveBufferTabToPane(g_pane_drag.source_pane_id, g_pane_drag.dragged_buffer_id,
                                              g_pane_drag.target_pane_id);
            } else {
                bool side = g_pane_drag.drop_zone == PaneDropZone::Left || g_pane_drag.drop_zone == PaneDropZone::Right;
                bool before = g_pane_drag.drop_zone == PaneDropZone::Left || g_pane_drag.drop_zone == PaneDropZone::Top;
                g_editor.SplitPaneWithBufferTab(g_pane_drag.source_pane_id, g_pane_drag.dragged_buffer_id,
                                                 g_pane_drag.target_pane_id, side ? SplitDir::Vertical : SplitDir::Horizontal,
                                                 before);
            }
        }
        // A plain click (threshold never passed) needs no handling here --
        // g_click_regions/DispatchChromeClicks already fires that
        // independently for a press-then-release-in-place.
        g_pane_drag = PaneDragState{};
    }

    SetMouseCursor(want_cursor);
}

// Highlights which drop zone (center/left/right/top/bottom) a tab-chip
// drag currently in progress would land in, drawn over the target pane's
// own content -- called from DrawEditor right after DrawPaneTree, using
// g_pane_drag/g_pane_screen_rects as of the end of the *previous*
// frame's UpdatePaneMouseInteraction (that one-frame lag is the same
// "imperceptible at interactive framerates" tradeoff g_click_regions
// already makes, see its own header).
void DrawPaneDragOverlay() {
    if (g_pane_drag.kind != PaneDragKind::TabMove || !g_pane_drag.threshold_passed || g_pane_drag.target_pane_id < 0) {
        return;
    }
    Rectangle target{};
    bool found = false;
    for (const PaneScreenRect &p : g_pane_screen_rects) {
        if (p.pane_id == g_pane_drag.target_pane_id) {
            target = p.rect;
            found = true;
            break;
        }
    }
    if (!found) return;
    Rectangle hl = target;
    switch (g_pane_drag.drop_zone) {
        case PaneDropZone::Left: hl.width /= 2.0f; break;
        case PaneDropZone::Right: hl.x += hl.width / 2.0f; hl.width /= 2.0f; break;
        case PaneDropZone::Top: hl.height /= 2.0f; break;
        case PaneDropZone::Bottom: hl.y += hl.height / 2.0f; hl.height /= 2.0f; break;
        case PaneDropZone::Center: default: break;  // full target rect
    }
    Color base = ResolveHlGroup("IncSearch");  // reuse an existing theme-derived highlight color rather than a hardcoded one
    DrawRectangle(static_cast<int>(hl.x), static_cast<int>(hl.y), static_cast<int>(hl.width), static_cast<int>(hl.height),
                  Color{base.r, base.g, base.b, 90});
    DrawRectangleLinesEx(hl, 2.0f, Color{base.r, base.g, base.b, 220});

    // A small floating label near the cursor naming the dragged buffer,
    // so it's clear what's being moved once the source pane's own chip
    // is out of view/covered by other panes.
    const Buffer &db = g_editor.GetBuffer(g_pane_drag.dragged_buffer_id);
    std::string label = db.scratch ? "[Scratch]" : (db.filename.empty() ? "[No Name]" : Basename(db.filename));
    Vector2 mp = GetMousePosition();
    float fs = MenuFontSize();
    float tw = MeasureTextEx(g_font, label.c_str(), fs, 0).x;
    Rectangle label_bg{mp.x + 12, mp.y + 12, tw + 12, fs + 8};
    DrawRectangleRec(label_bg, ResolveHlGroup("MenuBar"));
    DrawRectangleLinesEx(label_bg, 1.0f, ResolveHlGroup("Border"));
    DrawTextEx(g_font, label.c_str(), Vector2{label_bg.x + 6, label_bg.y + 4}, fs, 0, ResolveHlGroup("Normal"));
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
        if (g_pending_gantt_raster_export.buffer_id >= 0) {
            ExportGanttRaster(g_pending_gantt_raster_export.buffer_id, g_pending_gantt_raster_export.format.c_str());
            g_pending_gantt_raster_export = {};
            g_clean_gantt_export_buffer = -1;
        }
        // Only after DrawEditor() has (re)populated g_click_regions this
        // frame, and only when the menu bar didn't already claim this
        // click (a menu dropdown can overlap the tab bar/pane area
        // beneath it).
        if (!menu_consumed) DispatchChromeClicks();
        // Same reasoning (needs this frame's freshly (re)populated pane/
        // border/chip geometry, and shouldn't fire under an open menu
        // dropdown either) -- also runs every frame regardless of a
        // fresh click, unlike DispatchChromeClicks, since a drag already
        // in progress needs its own continuous per-frame update even
        // when IsMouseButtonPressed() is false this frame.
        if (!menu_consumed) UpdatePaneMouseInteraction();
        // Same reasoning again -- needs this frame's freshly-refreshed
        // KanbanSession/GanttSession::content_x/y/w/h (set by DrawKanban/
        // DrawGantt), and a drag already in progress needs its own
        // continuous per-frame update the same way pane-chrome dragging
        // does above.
        if (!menu_consumed) UpdateKanbanMouseInteraction();
        if (!menu_consumed) UpdateGanttMouseInteraction();
        if (!menu_consumed) UpdateOfficeScrollbarInteraction();
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

// Same reasoning: g_font_size lives in the anonymous namespace above;
// lua_env.cpp's own mep.font_size() binding (kBuiltinOrgLatex's DPI/
// slot-count math) reaches it through this accessor.
float GetFontSizePx() { return g_font_size; }

// Same reasoning: EvictOrgInlineImageTexture (and the cache it operates on)
// live in the anonymous namespace above; lua_env.cpp's own
// mep.org_image_invalidate binding reaches it through this externally-
// linked wrapper.
void InvalidateOrgInlineImageTexture(const std::string &path) { EvictOrgInlineImageTexture(path); }

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
    lua->DoString(kBuiltinTermSend);
    lua->DoString(kBuiltinMarkdown);
    lua->DoString(kBuiltinOrg);
    lua->DoString(kBuiltinOrgLinks);
    lua->DoString(kBuiltinOrgImages);
    lua->DoString(kBuiltinOrgCapture);
    lua->DoString(kBuiltinOrgAgenda);
    lua->DoString(kBuiltinOrgClock);
    lua->DoString(kBuiltinOrgBabel);
    lua->DoString(kBuiltinOrgLatex);
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
        g_editor.DropUnusedInitialBuffer();
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

    // Explicit, bounded teardown of every spawned child (:terminal shells,
    // git/LSP jobs, ...) before returning -- without this, killing them
    // fell to JobManager's own static-teardown destructor chain, whose
    // Job::~Job() does an *unbounded* reader-thread join() after a single
    // SIGTERM. A child that ignores SIGTERM (or is just slow) left that
    // join blocked forever, so mep's process never actually exited on
    // window-close/:qa -- it sat there until something outside mep (e.g.
    // a couple of Ctrl-C's at the launching shell) killed it by force.
    JobManager::Instance().ShutdownAll();
#endif

    UnloadFont(g_font);
    CloseWindow();
    return 0;
}
