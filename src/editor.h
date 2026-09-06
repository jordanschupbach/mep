#ifndef MEP_EDITOR_H
#define MEP_EDITOR_H

#include "pdf_doc.h"
#include "office_doc.h"
#include "sheet_doc.h"
#include "html_doc.h"
#include "org_doc.h"
#include "image_doc.h"
#include "vterm.h"

#include <stddef.h>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// PdfSession (below) needs PdfTextMatch/PdfHighlightRect by value (search
// results, held in std::vector members), not just a PdfDoc* -- a forward
// declaration isn't enough the way it is for ImageDoc/PdfDoc themselves
// (only ever held via unique_ptr). pdf_doc.h is as dependency-light as
// this header (no raylib), so including it directly here is safe.
// Same reasoning as pdf_doc.h above -- OfficeSession holds OfficeDoc (and
// its DocParagraph/DocSpan contents) by value, not behind a unique_ptr, so
// the full definitions need to be visible here.
// Same reasoning again -- SheetSession holds a Workbook by value.
// Same reasoning again -- HtmlSession holds an HtmlDoc (its DOM tree) by
// value.
// Same reasoning again -- KanbanSession/GanttSession hold an OrgOutline by
// value.

namespace mep::collab { class CollabSession; }

// editor.h is deliberately raylib-free (editor.cpp includes raylib.h,
// this header doesn't), so colors are stored as plain RGBA rather than
// raylib's Color -- main.cpp converts 1:1 when drawing (same layout).
struct ThemeColor {
    unsigned char r = 0, g = 0, b = 0, a = 255;
};

enum class Mode {
    Normal,
    Insert,
    Visual,
    VisualLine,
    VisualBlock,
    Command,
    SearchForward,
    SearchBackward,
    // Modal overlay interactions (NVIM_PARITY_PLAN.md Part I Phase 3):
    // take over input until confirmed/cancelled, then restore whatever
    // mode was active before they opened.
    Prompt,
    Confirm,
    Select,
    // Read-only informational float (NVIM_PARITY_PLAN.md Phase 17 gap:
    // git-gutter's "preview hunk"): shows text and closes on the very
    // next keypress -- no callback, unlike Prompt/Confirm/Select above.
    Preview,
    // A focused Sidebar (Part I Phase 7) takes over input the same way,
    // navigating its flattened section/widget list.
    Sidebar,
    // The fuzzy picker (Part I Phase 8): prompt + live-filtered list.
    Picker,
    // Roam backlink-graph view (Part VIII Phase 37's flagged gap, closed):
    // a full-screen node-link diagram, fuzzy-filterable like Picker but
    // navigating a 2D ring layout instead of a flat list. See
    // Editor::OpenRoamGraph.
    RoamGraph,
    // Whichkey (Part II Phase 11): collects keys after the leader, showing
    // what's registered under the accumulated prefix; executes on an exact
    // unique match.
    WhichKey,
    // Hints (Part III Phase 13): HintChar waits for the target character to
    // search for; HintLabel then collects the 1-2 char label picking which
    // occurrence to jump to.
    HintChar,
    HintLabel,
    // A focused `:terminal`/`:term` pane (Part VI Phase 27+): keystrokes
    // are encoded and forwarded live to the PTY-backed shell/program
    // instead of editing a buffer, the way Insert mode edits one. See
    // Editor::HandleTerminalInput.
    Terminal,
    // A focused image-viewer pane (an ImageSession buffer -- see below):
    // plain h/j/k/l and the arrow keys pan the decoded image instead of
    // moving a text cursor over Buffer::lines (which stays a dummy single
    // empty line for an image buffer, same as a terminal's). Unlike
    // Mode::Terminal, this does NOT capture every keystroke -- ':' and the
    // leader key are explicitly forwarded (see Editor::HandleImageInput)
    // so the command line and whichkey/leader mappings (including the
    // buffer picker) keep working while parked on an image; there's simply
    // nothing meaningful for any other key to do.
    Image,
    // A focused PDF-viewer pane (a PdfSession buffer -- see below): same
    // shape as Mode::Image (h/j/k/l pan, ':'/leader forwarded, everything
    // else a no-op) plus page navigation (Ctrl-f/Ctrl-b/PageDown/PageUp,
    // gg/G) since PDF content is paginated. See Editor::HandlePdfInput.
    Pdf,
    // A focused HTML-preview pane (an HtmlSession buffer -- see below,
    // opened by mep.browse_command's in-pane default via mep.html_open,
    // kBuiltinTextTools). Same shape as Mode::Image/Pdf (h/j/k/l scroll,
    // ':'/leader forwarded, everything else a no-op): a read-only rendered
    // view, not an editor over the page's markup -- plain :e on the same
    // .html file still opens it as ordinary editable text (this pane is
    // never reachable from LoadFile's extension dispatch the way Image/
    // Pdf/Office/Sheet are, precisely so :e keeps editing the source).
    // See Editor::HandleHtmlInput.
    Html,
    // A focused WYSIWYG office-document pane (an OfficeSession buffer --
    // see below, opened for .docx/.odt). Unlike Image/Pdf's single flat
    // mode, this is a real modal editor over rich-text paragraphs -- three
    // separate Mode values mirroring the main buffer's own Normal/Insert/
    // Visual split (not a sub-state flag inside one mode), because it has
    // the same three behaviorally distinct key-handling regimes the main
    // buffer does. Navigation/motions operate on whole paragraphs as the
    // "logical line" (h/l move within a paragraph's flat text, j/k move
    // to the prev/next paragraph, gg/G to the first/last) -- matching how
    // the main text buffer itself has no soft-wrap-aware motions either,
    // even though a paragraph is word-wrapped across several *visual*
    // lines for display; cursor hit-testing for the visual caret position
    // is a main.cpp rendering concern, not a motion-logic one. See
    // Editor::HandleOfficeNormalInput/HandleOfficeInsertInput/
    // HandleOfficeVisualInput.
    OfficeNormal,
    OfficeInsert,
    OfficeVisual,
    // A focused spreadsheet pane (a SheetSession buffer -- see below,
    // opened for .xlsx/.ods/.csv). Mirrors the Office pane's three-Mode
    // Normal/Insert/Visual split for the same reason (a real modal editor,
    // not a flat single-mode viewer), but SheetVisual selects a
    // rectangular *cell range* rather than a text span, and unlike
    // Office's word-wrap-oblivious-but-still-1D paragraph navigation,
    // grid navigation is genuinely 2D (h/l move columns, j/k move rows).
    // See Editor::HandleSheetNormalInput/HandleSheetInsertInput/
    // HandleSheetVisualInput.
    SheetNormal,
    SheetInsert,
    SheetVisual,
    // A focused Kanban-board pane (a KanbanSession, see below -- opened via
    // ":Kanban" on a .org buffer, closed back to plain text via ":Org").
    // Unlike Image/Pdf/Sheet/Office, the underlying Buffer::lines is NOT a
    // dummy stand-in for a separate binary model -- it's the buffer's real,
    // saved org text the whole time (see org_doc.h's top comment); a
    // KanbanSession is a disposable parsed-outline + UI-state cache layered
    // over it. hjkl move focus across columns/cards; 'i'/Enter enters
    // KanbanInsert to rename the focused card's title; 'n' appends a new
    // card; 'x'/'dd' deletes the focused card's whole subtree. See
    // Editor::HandleKanbanNormalInput/HandleKanbanInsertInput.
    KanbanNormal,
    KanbanInsert,
    // A focused Gantt-chart pane (a GanttSession, see below -- opened via
    // ":Gantt"). Same "real org text, disposable outline cache" model as
    // Kanban. Up/down moves the focused row, left/right pans the date
    // axis, +/- zooms; card/bar dragging (main.cpp's
    // UpdateGanttMouseInteraction) reschedules/resizes/creates a deadline;
    // double-clicking a row's label (or 'i') enters GanttInsert to rename
    // it. See Editor::HandleGanttNormalInput/HandleGanttInsertInput.
    GanttNormal,
    GanttInsert,
    // The hover-doc popup (ShowHover/DrawHoverPopup) with the cursor
    // parked inside its own text as a small read-only pseudo-buffer --
    // entered via Editor::EnterHoverFocus (Lua: mep.hover_focus_enter,
    // wired to pressing 'K' twice more at the same spot once hover is
    // already open -- see kBuiltinLsp's mep.lsp_hover), so the popup's
    // content can be navigated/selected/yanked with ordinary hjkl/v/V/y
    // instead of only being readable. Escape cancels an active selection
    // first, then on a second Escape leaves focus back to Mode::Normal
    // (Editor::HandleHoverFocusInput) -- the real buffer's cursor is never
    // touched while focused, so MaybeDismissHover's usual "cursor moved"
    // dismissal doesn't fire until the *next* Escape closes the popup for
    // real. See Editor::HandleHoverFocusInput.
    HoverFocus,
};

// Shared between Editor::HandleHoverFocusInput (scroll clamping) and
// main.cpp's DrawHoverPopup (the focused render path's visible window) so
// both agree on how many raw lines of hover text are shown at once.
constexpr int kHoverFocusVisibleRows = 20;

// Display name for the status line (main.cpp) and agent_rpc.cpp's
// event.modeChanged notification -- the one place outside main.cpp that
// also needs a human/agent-readable mode string, hence living here
// rather than staying main.cpp-private.
/**
 * @brief Returns a human/agent-readable display name for a mode.
 * @param m The mode to name.
 * @param replace_mode Whether Insert mode should be reported as "REPLACE" instead of "INSERT".
 * @return The mode's display name string.
 */
const char *ModeName(Mode m, bool replace_mode = false);

// A stable, distinct color per participant id (COLLAB_CURSORS_PLAN.md) --
// hashes `id` into a small fixed palette so the same participant always
// gets the same color across frames/reconnects (as long as their id
// string itself is stable), and different participants are visually
// distinguishable. No precedent for this existed anywhere in the
// codebase before this feature (confirmed during design) -- every
// existing collaboration-peer UI element used one uniform color.
/**
 * @brief Deterministically derives a stable display color for a collaboration participant.
 * @param id The participant's identifier string.
 * @return The RGBA color assigned to that participant id.
 */
ThemeColor ParticipantColor(const std::string &id);

struct CursorPos {
    int row = 0;
    int col = 0;
};

// Dependency-free fzf-style fuzzy subsequence scorer (NVIM_PARITY_PLAN.md
// Part I Phase 8): every character of `query` must appear in `str`, in
// order (not necessarily contiguous). Returns -1 if it doesn't match at
// all; otherwise a score where higher is a better match (consecutive runs
// and word-boundary starts score higher; longer overall spans and longer
// strings score lower, as a tiebreaker). Smart-case: case-insensitive
// unless `query` itself contains an uppercase letter. `positions`, if
// non-null, receives the matched byte offsets in `str` (for highlighting).
/**
 * @brief Scores how well `query` fuzzy-matches `str` as an ordered subsequence, fzf-style.
 * @param str The candidate string to match against.
 * @param query The subsequence query to search for within `str`.
 * @param positions Optional output vector to receive the matched byte offsets in `str`.
 * @return A higher-is-better match score, or -1 if `query` doesn't match `str` at all.
 */
int FuzzyScore(const std::string &str, const std::string &query, std::vector<int> *positions = nullptr);

// Filename/extension -> a Nerd Font icon glyph, UTF-8 encoded (NVIM_PARITY_
// PLAN.md Part II Phase 10). Same Private-Use-Area codepoints nvim-web-
// devicons/mep.nvim's own mep.icons ship (mep.nvim/lua/mep/icons/data.lua's
// M.nerd_font table) -- a widely-deployed, known-good set. Rendered via
// main.cpp's g_icon_font (icon_font_data.h, a pyftsubset subset of Symbols
// Nerd Font Mono covering exactly the codepoints this function and its
// main.cpp counterparts use), not g_font -- see DrawUiText's own comment.
/**
 * @brief Maps a filename or extension to its Nerd Font icon glyph.
 * @param name The filename (or bare extension) to look up.
 * @return A UTF-8 encoded icon glyph string.
 */
std::string IconForFilename(const std::string &name);

// Filename/extension -> highlight-group name (e.g. "Blue", "Green",
// "MutedFg") for coloring file tree rows by type. Names index into the
// active theme's BuildHighlightGroups() map (editor.cpp), so colors follow
// whatever :colorscheme is active rather than a fixed RGB value.
/**
 * @brief Maps a filename or extension to a highlight-group name for coloring file tree rows.
 * @param name The filename (or bare extension) to look up.
 * @return The highlight-group name to use for that file type.
 */
std::string HlGroupForFilename(const std::string &name);

// UTF-8-encodes a single Unicode codepoint. Icon tables below list plain hex
// ints (e.g. 0xf15b) rather than C++ universal-character-name escapes or raw
// multi-byte UTF-8 bytes directly in source: the short escape form is exactly
// four hex digits and the long form exactly eight, and a codepoint above the
// Basic Multilingual Plane (several of mep's own icons are) needs the long
// form, zero-padded -- easy to get subtly wrong by hand, where a plain int
// literal can't be. Icon codepoints are always in the Private Use Area or
// Supplementary PUA-A, never a surrogate half or otherwise invalid scalar,
// so this doesn't need to handle those cases.
/**
 * @brief UTF-8-encodes a single Unicode codepoint.
 * @param cp The Unicode codepoint to encode.
 * @return The UTF-8 byte sequence as a string.
 */
std::string Utf8FromCodepoint(int cp);

// The contents of one register (yank/delete target). Registers are global
// to the editor, not per-buffer, matching Vim -- you can yank in one
// buffer and paste in another.
struct Register {
    std::string text;
    bool linewise = false;
    // Visual Block yank/delete (Phase 9): `text` holds one block row per
    // line (newline-separated, same storage shape as linewise), and this
    // flag is how PasteAfter/PasteBefore tell "paste each row at the same
    // column, one below the other" apart from an ordinary linewise/
    // charwise paste. Mutually exclusive with linewise in practice.
    bool blockwise = false;
};

// One entry in a Pane's jumplist (Ctrl-O/Ctrl-I) -- unlike the single-slot
// ``/'' mechanism (Buffer::last_jump_from), the jumplist can span multiple
// buffers, so each entry records which one it belongs to.
struct JumpEntry {
    int buffer_id = 0;
    CursorPos pos;
};

// One buffer decoration (NVIM_PARITY_PLAN.md Part I Phase 4 --
// mep.nvim/Neovim's "extmark", scoped down): a highlight span, virtual
// text, and/or a gutter sign anchored at a position. Row/col are plain
// snapshotted coordinates, NOT gravity-adjusted through arbitrary edits
// the way real extmarks are -- a deliberate simplification (see
// NVIM_PARITY_PLAN.md Phase 4). Most consumers (colorizer, todoscan, git
// gutter, diagnostics, markdown) already recompute on a debounce after
// edits and simply clear+re-add their whole namespace each time, which
// sidesteps the gap entirely; a feature that genuinely needs a range to
// track live edits (e.g. snippet tabstops) manages its own position
// bookkeeping rather than relying on this store to do it for them.
struct Decoration {
    int id = 0;
    int row = 0;
    // Highlight span [col_start, col_end); ignored if whole_line is set.
    int col_start = 0;
    int col_end = 0;
    bool whole_line = false;
    std::string hl_group;  // empty = no highlight span
    // Draw a thin underline under [col_start, col_end) instead of
    // recoloring the span's text (NVIM_PARITY_PLAN.md Phase 21 gap: LSP
    // diagnostics want to mark the exact affected columns the way a
    // squiggly underline would, not tint the whole span/line). When set,
    // hl_group still supplies the underline's color but no longer swaps
    // the text's foreground -- ignored if whole_line is set or the span
    // is empty. Distinct from VTermCell::underline (src/vterm.h), which
    // is terminal-emulator cell state, not an editor decoration.
    bool underline = false;
    // Org emphasis (*bold*/\/italic\//+strike+, kBuiltinOrg): the editor's
    // single monospace font (g_font, main.cpp) has no real bold/italic
    // face, so these are faked at draw time -- bold as a double-draw
    // offset by 1px, italic as an rlgl shear transform around the same
    // DrawTextEx call recoloring already uses, both drawn over
    // [col_start, col_end) the same way hl_group's own text-recolor
    // redraw is (see DrawPane, main.cpp) -- ignored if whole_line is set
    // or the span is empty, same as underline. strikethrough is a plain
    // line through the span's vertical middle, the same primitive
    // underline already uses just at a different y, and the one style
    // here with a *real* (non-faked) equivalent already elsewhere in this
    // codebase -- see OfficeFontFor's own comment (main.cpp) on why the
    // docx/office viewer draws it as an overlay line too rather than a
    // font variant.
    bool bold = false;
    bool italic = false;
    bool strikethrough = false;
    // Virtual text: inline (drawn before col_start, doesn't touch the
    // buffer) or overlay (drawn *instead of* [col_start, col_start+len)).
    std::string virt_text;
    std::string virt_text_hl;
    bool virt_overlay = false;
    // Anchors virt_text to just past the *end of this row's own text*
    // (buf.lines[row].size()) instead of col_start -- for an annotation
    // that describes the whole line (e.g. a diagnostic message) rather
    // than concealing/replacing a specific span the way virt_overlay's
    // col_start-anchored text does. Without this, a line-describing
    // virt_text drawn at its diagnostic's own (usually mid-line) column
    // paints directly over whatever real buffer text follows it, with no
    // background cover -- confirmed exactly this bug report ("diagnostic
    // text shows up overtop of the text") before this field existed.
    bool virt_text_eol = false;
    std::string sign;  // gutter glyph; empty = none. Multi-byte UTF-8 (a
                        // Nerd Font icon glyph) is valid here, not just a
                        // single ASCII char -- main.cpp's DrawUiText is
                        // what actually renders it.
    std::string sign_hl;
    // Draws a small filled circle (sign_hl's own color) behind `sign` in
    // the gutter instead of just plain colored text -- a "badge" look
    // for e.g. a diagnostic count, so more than one stacked signal on a
    // line (2 errors vs. 1) is visually distinguishable at a glance, not
    // just by which single glyph happened to win priority.
    bool sign_badge = false;
    int priority = 0;
    // Colorizer swatch (Part III Phase 13): a literal RGB drawn as a small
    // filled square at col_start, bypassing the named-highlight-group
    // system entirely -- the whole point is showing the *exact* parsed
    // color, not its nearest theme role.
    bool has_swatch = false;
    ThemeColor swatch_color;
    // Literal RGB text-color override, bypassing the named hl_group lookup
    // entirely -- distinct from has_swatch/swatch_color (which draws a
    // small square *next to* col_start, not a text recolor). Used by
    // Editor::EnterTerminalNormalMode to carry a snapshotted terminal
    // cell's actual ANSI color, which (256-color/truecolor especially)
    // doesn't map onto any of the theme's own named roles the way
    // hl_group's other consumers (syntax highlighting, diagnostics) do.
    bool has_fg_color = false;
    ThemeColor fg_color;
};

// A fold range (NVIM_PARITY_PLAN.md Part I Phase 5). `provider` is a free-
// form tag (e.g. "manual", "org", "markdown", "treesitter") so a provider
// can find-and-replace just its own folds without disturbing another's.
struct Fold {
    int start_row = 0;
    int end_row = 0;  // inclusive
    bool closed = true;
    std::string provider = "manual";
};

// A generic reusable side/dock panel (NVIM_PARITY_PLAN.md Part I Phase 7):
// sections of clickable/hoverable widgets. Scoped down from the plan's
// full design -- rendered as a docked floating box (Phase 3's float
// primitive, edge-anchored instead of centered) rather than a real split
// participating in the pane layout, and keyboard-navigable only for now
// (mouse click-through is a documented follow-up, not blocking every
// later phase that builds a panel on top of this).
struct SidebarWidget {
    std::string id, text, icon, hl, tooltip;
    int on_click_ref = 0;  // Lua function ref (luaL_ref), 0 = none
    // Marks this row as "where the cursor currently is" for a consumer
    // that tracks an external position (kBuiltinStructure's outline, e.g.)
    // rather than the sidebar's own input focus -- drawn as a persistent
    // background tint in DrawSidebars regardless of whether the sidebar
    // itself is focused, unlike the focus-driven PickerSelected row
    // highlight (sidebar_cursor_) which only ever shows on the focused
    // sidebar. At most one widget should set this per render.
    bool current = false;
};
struct SidebarSection {
    std::string id, title;
    bool collapsed = false;
    std::vector<SidebarWidget> widgets;
};
struct SidebarInstance {
    int id = 0;
    std::string title;
    std::vector<SidebarSection> sections;
    std::string position = "right";  // "left"/"right"/"top"/"bottom"
    int size = 34;                   // width (left/right) or height (top/bottom), in cells
    bool open = false;
    // Catch-all for keys HandleSidebarInput doesn't already reserve
    // (j/k/q/arrows/Enter/Escape) -- called with the typed character so a
    // consumer (e.g. Phase 15's file tree) can bind its own keys (create/
    // rename/delete/refresh/toggle-hidden) without the generic sidebar
    // widget needing to know anything tree-specific. 0 = none registered.
    int on_key_ref = 0;
    // Popout preview source (mod1+m, Editor::ToggleSidebarPopout): called
    // with the widget id under the cursor ("" for a section header) each
    // time that changes while this sidebar is popped out, and expected to
    // answer -- synchronously or later, from a job callback -- through
    // mep.sidebar_set_preview. 0 = none registered, in which case the
    // popout is just a larger copy of the docked list with no preview
    // column.
    int on_preview_ref = 0;
    // Tab strip (mep.sidebar_set_tabs): view names drawn on a row under
    // the docked title and beside the popout's title. Tab/Shift-Tab while
    // focused, a click on a name, or mep.sidebar_set_active_tab switch
    // views, each firing on_tab_ref(index, 1-based) so the consumer
    // re-renders its sections for that view (the git panel's Status/Log/
    // Branches/Stash). Empty = no strip, nothing changes.
    std::vector<std::string> tabs;
    int active_tab = 0;  // 0-based index into `tabs`
    int on_tab_ref = 0;
    // Share of its dock's height this sidebar gets when several open
    // sidebars dock on the same left/right edge -- DrawSidebars merges
    // those into ONE column (width = the largest `size` among them, kept
    // in sync by SetSidebarSize) split vertically, each getting
    // stack_share / (sum of the open members' shares) of the column.
    // Adjusted by dragging the divider between two stacked sidebars or
    // mod1+Shift+j/k while one is focused (SetSidebarStackShares); the
    // default 1.0 everywhere means an even split.
    float stack_share = 1.0f;
    // First flattened-line index drawn at the top of the sidebar's content
    // area -- this sidebar's mirror of Pane::scroll_row. Kept per-instance
    // (rather than a single field alongside sidebar_cursor_) so a sidebar
    // that loses focus (mod1+hjkl back into the pane tree, or another
    // sidebar taking focus) keeps its own scroll position instead of
    // sharing one slot with whichever sidebar happens to be focused right
    // now. Updated once per rendered frame by Editor::UpdateScrollForSidebar
    // (main.cpp's DrawSidebars, the same call shape as UpdateScrollForPane).
    int scroll_offset = 0;
};

// One flattened, renderable/navigable line of a sidebar: either a section
// header (collapse toggle) or a widget row. Shared by main.cpp's renderer
// and Editor::HandleSidebarInput so the two can't disagree about layout.
struct SidebarLine {
    enum class Kind { SectionHeader, Widget } kind = Kind::SectionHeader;
    int section_index = 0;
    int widget_index = -1;  // -1 for a header line
    std::string text;
    std::string hl;
    bool current = false;  // mirrors SidebarWidget::current; see its comment
};

// A compact palette (mep.nvim's palettes.lua SPECS/FALLBACKS shape,
// scoped down): a handful of named "role" colors that BuildHighlightGroups
// (editor.cpp) expands into the full named highlight-group set every
// chrome/decoration consumer targets by name.
struct Palette {
    std::string name;
    ThemeColor bg, fg, red, green, yellow, blue, purple, cyan, orange, border;
};

// One highlight span over a picker's item display text or its preview
// column (Treesitter-backed syntax highlighting for `/`'s buffer-search
// picker, kBuiltinPickerSources' mep.buffer_search -- see
// Editor::SetPickerPreview and DrawPickerOverlay, main.cpp). `row` is
// unused (always 0) on a PickerItem's own single-line `display`; for the
// preview column it indexes into SplitLines(PickerPreview()) the same
// way Decoration::row indexes into buffer lines. col_start/col_end are
// byte offsets, [col_start, col_end) exclusive, matching Decoration's
// own convention.
struct PickerHlSpan {
    int row = 0;
    int col_start = 0;
    int col_end = 0;
    std::string hl_group;
};

// One entry in a picker's item list (NVIM_PARITY_PLAN.md Part I Phase 8).
// `data` is an opaque payload (e.g. a file path) handed back to the
// on_select callback verbatim -- `display` is what's matched/shown.
// `spans` is optional per-item syntax highlighting over `display`
// (PickerHlSpan above); empty for every existing plain-text picker.
struct PickerItem {
    std::string display;
    std::string data;
    std::vector<PickerHlSpan> spans;
};

// One insert-mode completion candidate (NVIM_PARITY_PLAN.md Phase 22
// follow-up: show which source a candidate came from, and an LSP item's
// signature/doc alongside it). `kind` is a short source tag ("lsp",
// "file", "snippet", "buffer") the popup renders as a badge; `detail`/
// `doc` come from LSP CompletionItem.detail/.documentation and drive the
// side info panel -- both empty for every non-LSP source.
struct CompletionCandidate {
    std::string text;
    std::string kind;
    std::string detail;
    std::string doc;
};

// One node in the Roam backlink-graph view (NVIM_PARITY_PLAN.md Phase 37's
// flagged "no fuzzy backlink-graph visualization" gap, closed). `hop` is
// graph-distance from the note the view was opened on: 0 = that note
// itself (always exactly one such node, rendered at the overlay's center),
// 1 = a note it directly links to or that directly links to it, 2 = a
// link/backlink of a hop-1 note. DrawRoamGraphOverlay's deterministic ring
// layout keys directly off `hop` (ring radius = f(hop)) -- no physics
// simulation, see the comment above Editor::OpenRoamGraph.
struct RoamGraphNode {
    std::string id;
    std::string title;
    std::string path;
    int hop = 0;
};

// One edge between two RoamGraphNode indices (into the same call's nodes
// vector). Undirected for rendering purposes even though the underlying
// `[[id:...]]` link it represents is directional.
struct RoamGraphEdge {
    int a = 0, b = 0;
};

// One labeled jump target (NVIM_PARITY_PLAN.md Part III Phase 13 hints).
struct HintMatch {
    int row = 0, col = 0;
    std::string label;
};

// One leader-key binding (NVIM_PARITY_PLAN.md Part II Phase 11 whichkey):
// `sequence` is the key(s) typed *after* the leader (e.g. "ff" for a
// "find files" bound at <leader>ff). Registered via Lua; executed when the
// accumulated WhichKey-mode prefix matches `sequence` exactly.
struct WhichKeyBinding {
    std::string sequence;
    std::string description;
    int lua_ref = 0;
};

// The in-memory content of one file (or scratch buffer). Undo history is
// per-buffer, not per-pane: two panes split on the same buffer share it,
// matching Vim.
struct Buffer {
    std::vector<std::string> lines{""};
    std::string filename;
    bool modified = false;
    // NVIM_PARITY_PLAN.md Part III Phase 12: never persisted, `:w` refuses
    // it the same way an unnamed buffer already does (empty filename) --
    // this flag only distinguishes it for :MepScratch's find-or-create and
    // its tab/status-line label ("[Scratch]" instead of "[No Name]").
    bool scratch = false;
    // `:bd`/`:bdelete` (Editor::BufferDelete) -- soft-delete, not a real
    // erase from buffers_: buffer_id is treated as a stable index
    // everywhere in this codebase (panes, terminals_, agent-rpc
    // connections' cursor_buffer_id, ...), and only one place
    // (DropUnusedInitialBuffer, a narrow startup-only case) ever actually
    // shifts it -- reindexing every one of those sites for a general
    // "delete any buffer at any time" command would be exactly the kind
    // of invasive, easy-to-miss-a-site change that class of bug comes
    // from. A deleted buffer is closed out of every pane/tab currently
    // showing it (like `:bd` in real vim) and hidden from buffer_list/
    // bnext/bprev (BufferLabelForLua/BufferNext/BufferPrevious all check
    // this), but its Buffer object -- content, undo history -- stays put
    // at its same index; FindOrCreateBuffer clears this again if the same
    // path is ever opened a second time.
    bool deleted = false;
    // WORKSPACES_PLAN.md decision 3: which Workspace (by stable id) this
    // buffer belongs to; -1 = unscoped (the startup dashboard buffer,
    // :MepScratch, transient help/quickfix-style buffers). Scoped readers
    // (:bnext, :ls, the buffer picker, agent buffer.list) filter on it;
    // global ones (:wa, the :qa guard) deliberately don't.
    int workspace_id = -1;

    std::vector<std::vector<std::string>> undo_stack;
    std::vector<std::vector<std::string>> redo_stack;

    // Marks (m{a-z}) are per-buffer in Vim, not global -- `a jumps to
    // nothing if it was set in a different buffer. Row is clamped (not
    // shifted) against edits that move lines around, a deliberate
    // simplification noted as a stretch goal in VIM_PARITY_PLAN.md.
    std::unordered_map<char, CursorPos> marks;
    // The position before the last "big" jump (G, gg, a search, or a mark
    // jump itself) in this buffer, for `` / '' to jump back to.
    CursorPos last_jump_from;
    bool has_last_jump = false;

    // Decorations, keyed by namespace id (see Editor::CreateNamespace).
    std::unordered_map<int, std::vector<Decoration>> decorations;
    int next_decoration_id = 1;

    // Folds (NVIM_PARITY_PLAN.md Part I Phase 5): possibly-nested inclusive
    // row ranges, each independently open/closed. No explicit tree -- a
    // "nested" fold is just an entry whose range sits inside another's;
    // operations that care about nesting (toggle-at-cursor) simply pick
    // the smallest range containing the row. Providers (org/markdown
    // headline depth, treesitter fold queries, ...) replace this whole
    // vector wholesale each recompute rather than diffing it.
    std::vector<Fold> folds;
    // vim-style 'foldlevel', stepped by zm/zr and snapped to an extreme by
    // zM/zR (Editor::AdjustFoldLevel/SetAllFoldsClosed): folds nested no
    // deeper than this stay open, anything deeper is closed. -1 means
    // "never explicitly set" -- treated as the deepest level present
    // (i.e. everything open), matching a freshly loaded/folded file where
    // nothing's been collapsed yet.
    int fold_level = -1;

    // Org inline-image rendering: row -> resolved file path, populated by
    // Lua's mep.org_image_scan() (kBuiltinOrgImages, main.cpp) from
    // [[file:...]] links whose target is a raster image, consulted by
    // DrawPane (main.cpp) when Editor::OrgImagesVisible() is on. Kept
    // fresh regardless of that toggle (rebuilt wholesale on every scan,
    // same "provider replaces its own entries wholesale" convention as
    // `folds` above) so switching the toggle on shows correct state
    // immediately rather than waiting for the next edit.
    std::unordered_map<int, std::string> org_image_rows;

    // Org LaTeX/math-mode rendering (<leader>otl, Editor::OrgLatexVisible()):
    // row -> a rendered fragment's PNG path plus how many line-heights tall
    // to display it, populated by Lua's mep.org_latex_scan() (kBuiltinOrgLatex,
    // main.cpp) from $$..$$/\[..\]/#+BEGIN_LaTeX (etc.) fragments, each
    // rendered offline via tectonic+pdftoppm. `slots` is per-entry (unlike
    // org images' shared compile-time kOrgInlineImageSlots) since a fragment's
    // rendered height varies hugely -- a bare "$x$" and a multi-line matrix
    // shouldn't claim the same vertical space -- computed by
    // mep_org_latex_render (kBuiltinOrgLatex) from the rendered PNG's own
    // pixel height (mep.image_size) divided by the current line height, so
    // it's already correct by the time it's stored here; DrawPane (main.cpp),
    // its own cursor-Y lookup, and Editor::UpdateScrollForPane (editor.cpp)
    // all just read it back, the same three-site agreement org images'
    // fixed constant needs, just sourced from data instead of a constant.
    // Unlike org_image_rows, only ever populated while the toggle is on --
    // a multi-line fragment's *other* raw source rows must not linger
    // visible once the toggle is off. Unlike an earlier version of this
    // (which hid those other rows behind a 'latex'-provider closed Fold),
    // there's no Fold involved any more: `end_row` records the last raw
    // buffer row the fragment's own source spanned, and DrawPane/its
    // cursor-Y lookup/Editor::UpdateScrollForPane all jump straight from
    // `row` to `end_row + 1` themselves (the same "consume more than one
    // raw row" shape a closed Fold has, just without a fold's own visible
    // "+-- N lines: ... ---" summary line taking a slot of its own --
    // there's nothing left to summarize once the image already shows
    // everything the block ever had to show).
    struct OrgLatexRender {
        std::string path;
        int slots = 1;
        int end_row = 0;
    };
    std::unordered_map<int, OrgLatexRender> org_latex_rows;

    // Inline math (a `$x^2$`/`\(x^2\)`/etc. fragment sharing its row with
    // other prose, e.g. "the value $x^2$ matters here"): unlike
    // org_latex_rows above, this can't replace the *whole* row -- mep's
    // row renderer (DrawLineFast, main.cpp) draws one row as one run of
    // text with no notion of a mid-row texture, so surrounding text has to
    // stay. Instead each span is drawn as a small texture painted directly
    // over its own [col_start, col_end) column range (paint the background
    // color first to conceal the raw "$...$" markup, same trick
    // Decoration's own virt_overlay uses for text), sized to fit *within*
    // that column range's own pixel width and the line's height -- never
    // stretched wider, so it can never bleed into whatever text follows it
    // on the same row, at the cost of an occasional cramped render for a
    // dense fragment packed into a short span. Populated by
    // mep_org_latex_register_inline (kBuiltinOrgLatex), only while the
    // toggle is on (see org_latex_rows' own comment for why that's a hard
    // requirement here, not just a nicety -- unlike that field this one
    // has no Fold to also gate on, so this map being non-empty is the
    // *only* thing hiding the raw source).
    struct OrgLatexInlineSpan {
        int col_start = 0;
        int col_end = 0;  // exclusive
        std::string path;
    };
    std::unordered_map<int, std::vector<OrgLatexInlineSpan>> org_latex_inline;

    /**
     * @brief Returns the number of lines currently in the buffer.
     * @return The line count.
     */
    int LineCount() const { return static_cast<int>(lines.size()); }
};

// Default "line width" org inline images size themselves against, in
// characters, and the fraction of that width an image targets --
// GetOrLoadOrgInlineImageTexture's caller (DrawPane, main.cpp) multiplies
// this by g_char_width to get a target pixel width, mirroring how a
// LaTeX export's default \includegraphics{width=0.6\linewidth} looks
// against an 80-column-wrapped paragraph. Images smaller than the target
// are stretched up to it (not just downscaled), so this is a target
// width, not just a cap. Still clamped to the pane's
// actual available width so a narrow pane never overflows.
constexpr int kOrgImageLineWidthChars = 80;
constexpr float kOrgImageWidthFraction = 0.6f;

// Fixed visual height (in line-heights) an inline-rendered org image
// occupies (Editor::OrgImagesVisible()/Buffer::org_image_rows): a fixed
// slot count, rather than one derived from each image's own pixel aspect
// ratio, keeps the scroll/cursor visual-slot bookkeeping (DrawPane's row
// loop and its cursor-Y lookup in main.cpp, and Editor::UpdateScrollForPane
// in editor.cpp -- three sites that must agree exactly, the same
// constraint folds' own "closed fold = 1 slot" rule is under) as simple as
// folds' fixed collapse-to-1 rule, just inverted (expand instead of
// collapse) -- an image is scaled (aspect-preserved, letterboxed against
// this many line-heights, upscaled past native size if needed to reach
// its target width) rather than this dictating the slot count itself.
//
// Sized so a square (1:1) image can actually reach kOrgImageLineWidthChars
// * kOrgImageWidthFraction wide without this vertical budget cutting it
// short first -- with the built-in JetBrains Mono font, a character's
// advance width is roughly 0.52 of the line height (font size + 6px, see
// LineHeight() in main.cpp), so that's 80 * 0.6 * 0.52 =~ 25 line-heights,
// rounded up. A wider (landscape) image hits its width target well before
// running into this cap; a taller-than-square one is letterboxed narrower
// than the width target instead of growing without bound.
constexpr int kOrgInlineImageSlots = 25;

// One window onto a buffer: its own cursor, scroll position, and Visual
// selection anchor. Identified by a stable id (not a pointer) since the
// split tree it lives in gets restructured on every split/close.
struct Pane {
    int id = 0;
    int buffer_id = 0;
    CursorPos cursor;
    CursorPos visual_anchor;
    int scroll_row = 0;
    // Set each frame by UpdateScrollForPane (called once per rendered pane
    // from main.cpp, the only place that knows pixel geometry) so H/M/L
    // have something to work with without editor.cpp needing to know
    // anything about rendering.
    int visible_lines = 1;
    // Per-pane buffer tabs (NVIM_PARITY_PLAN.md Part III Phase 14): buffer
    // ids opened in *this* pane, cycled independently of sibling panes and
    // distinct from the top-level tab bar (Tab). Lazily (re)seeded from
    // `buffer_id` by Editor::EnsureBufferTabSeeded() wherever it might be
    // stale, rather than kept in sync at every `buffer_id` assignment site.
    std::vector<int> buffer_tabs;
    int buffer_tab_index = 0;

    // Full jumplist (Ctrl-O/Ctrl-I), per-window like Vim's own -- distinct
    // from Buffer::last_jump_from (the single-slot ``/'' mechanism), which
    // stays as-is. `jumplist_index` follows Vim's own list+index model:
    // it's the index of the entry Ctrl-I would go to next, so it normally
    // sits at jumplist.size() ("live", nothing to go forward to) and
    // decrements/increments as Ctrl-O/Ctrl-I are pressed. See PushJump/
    // JumpListBack/JumpListForward in editor.cpp for the exact semantics.
    std::vector<JumpEntry> jumplist;
    int jumplist_index = 0;

    // Last cursor.row Editor::UpdateScrollForPane saw (-1 = never run
    // yet). Lets it tell "the cursor itself moved a lot this frame" (G,
    // gg, a search, a counted motion) from "the cursor barely moved but
    // scroll_row suddenly needs to move a lot anyway" (stepping onto/off
    // an org inline image or LaTeX fragment, which claim many visual
    // slots for one buffer row -- kOrgInlineImageSlots) -- see
    // UpdateScrollForPane's own comment for how this drives its
    // smoothing.
    int scroll_follow_last_cursor_row = -1;
};

enum class SplitDir { Leaf, Horizontal, Vertical };

// A node in a tab's split-pane tree. Leaf nodes hold a Pane; Horizontal/
// Vertical nodes hold 2+ children laid out top-to-bottom or left-to-right.
struct SplitNode {
    SplitDir dir = SplitDir::Leaf;
    Pane pane;  // valid when dir == Leaf
    std::vector<std::unique_ptr<SplitNode>> children;  // valid otherwise
    // Each child's share of this node's extent along the split axis,
    // summing to 1.0, parallel to `children` (empty means "equal shares",
    // the default). Only ever populated by Editor::ResizeActivePane;
    // anything that changes `children.size()` (split/close/ApplyLayout)
    // leaves the old vector's size mismatched on purpose rather than
    // patching it up -- every reader (ComputeRects, ResizeActivePane's own
    // EnsureShares) treats a size mismatch as "equal shares", so a resize
    // struct simply reverts to equal the next time its own child count
    // changes instead of carrying stale ratios forward.
    std::vector<float> shares;
};

struct Tab {
    // Stable identity from Editor::next_tab_id_ (never an index: :tabdelete
    // shifts every later tab down a slot), so Lua state keyed by tab -- the
    // built-in per-tab terminal's tab -> buffer map (kBuiltinTabTerminal)
    // -- survives tabs being closed/reordered around it. Runtime-only,
    // like pane ids: a session restore hands the rebuilt tabs fresh ids.
    int id = 0;
    std::unique_ptr<SplitNode> root;
    int active_pane_id = 0;
};

struct WorktreeEntry;  // workspace_git.h
class Json;            // json.h

// WORKSPACES_PLAN.md: one named group of tabs with its own root directory
// (a git worktree once Phase 6 lands, otherwise the project root) and its
// own set of buffers (Buffer::workspace_id). Identified by a stable id
// from Editor::next_workspace_id_ -- never an index, since workspaces can
// be reordered or closed underneath a buffer/terminal/agent that names one.
struct Workspace {
    int id = 0;
    std::string name;    // "main", "feat-login"; also the branch/dir name
    std::string root;    // absolute; worktree path or project root
    std::string branch;  // "" when not git-backed
    bool primary = false;   // the checkout itself: never a worktree, undeletable
    bool creating = false;  // `git worktree add` still running (drawn dimmed)
    // Phase 7: set from Lua's optional debounced `git status --porcelain`
    // poll (mep.opt.workspace_git_dirty); drawn as a `+` suffix. The `*`
    // suffix (modified buffers) is computed live instead, never from git.
    bool git_dirty = false;
    std::vector<Tab> tabs;
    int active_tab = 0;
};

// One loaded project root (Editor::projects_). Only a bookmark before
// WORKSPACES_PLAN.md; now the top of the Project > Workspace > Tab nesting.
struct Project {
    int id = 0;
    std::string name;  // basename(root)
    std::string root;  // std::filesystem::canonical
    bool is_git = false;
    std::string git_toplevel;  // primary checkout's toplevel; == root normally
    std::vector<Workspace> workspaces;
    int active_workspace = 0;
};

// A pane's position within its tab, normalized to [0,1]. Derived purely
// from the split tree (every split divides its rect into equal shares),
// so directional pane navigation needs no pixel geometry from main.cpp.
struct PaneRect {
    int pane_id = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class LuaEnv;

// One `:terminal`/`:term` pane's PTY-backed state (Part VI Phase 27+):
// keyed by the buffer id standing in for it in the split tree (a terminal
// pane's Buffer holds no real text -- DrawPane/DrawTerminalGrid render
// straight from `vterm` instead -- so this is where its actual content and
// process handle live). Editor owns these in `terminals_`; VTerm itself
// (vterm.h) is a self-contained VT100/ANSI emulator with no editor or
// job-system dependency, kept as a unique_ptr so this header doesn't need
// to include vterm.h.
struct TerminalSession {
    int buffer_id = 0;
    int job_id = 0;
    std::unique_ptr<VTerm> vterm;
    std::string title;      // argv[0], overridden by an OSC-title if the program sets one
    // wasm build only: set by TerminalSpawn right after
    // mep_js_pty_connect_start() returns a slot id, cleared by
    // PollTerminals() once mep_js_pty_connect_status() reports ready or
    // failed. Writes/resizes sent while true are harmless no-ops (the
    // underlying WebSocket just isn't OPEN yet); output/exit polling is
    // skipped until it clears.
    bool connecting = false;
    bool exited = false;
    int exit_code = 0;
    // Lines scrolled back from the live tail (0 = viewing the live
    // screen); Shift+PageUp/PageDown adjust this without leaving
    // Mode::Terminal or interrupting keystroke forwarding.
    int scroll_offset = 0;
    int last_rows = 0, last_cols = 0;  // last size ResizePty/VTerm::Resize was called with
};

// One image-viewer pane's decoded content, keyed by buffer id the same way
// TerminalSession is (see its own comment) -- an image buffer's
// Buffer::lines stays a dummy single empty line, real content lives here.
// ImageDoc (image_doc.h) is a raylib-free decoded RGBA8 pixel buffer, kept
// as a unique_ptr so this header doesn't need to include image_doc.h;
// main.cpp is the only place that turns it into a GPU Texture2D.
struct ImageSession {
    int buffer_id = 0;
    std::unique_ptr<ImageDoc> doc;
    // Top-left pixel of the visible viewport into the *scaled* (zoom
    // applied) image, panned by Editor::HandleImageInput and clamped
    // against the scaled image/viewport size by Editor::ResizeImageViewport.
    int pan_x = 0, pan_y = 0;
    // Last pane content size (pixels) DrawPane reported via
    // ResizeImageViewport -- what panning clamps against.
    int viewport_w = 0, viewport_h = 0;
    // 1.0 = the decoded image's native pixel size. +/- (Editor::
    // HandleImageInput) multiply/divide this by kImageZoomStep; = resets it
    // to whatever ZoomImageToFit computes so the whole image just fits the
    // current viewport.
    float zoom = 1.0f;
};

// Visual gap (screen pixels, unscaled by zoom) drawn between consecutive
// pages in the PDF viewer's continuous-scroll stack. Shared between
// Editor::HandlePdfInput's scroll-rebase math (when scroll_y has crossed
// far enough to cross a page boundary) and main.cpp's DrawPane (where it
// positions the next/previous page's texture) -- these two computations
// must agree exactly, or the page boundary rebase_scroll computes and the
// one DrawPane actually renders at drift apart into a visible jump/overlap.
inline constexpr float kPdfPageGapPx = 16.0f;

// Fixed spreadsheet grid geometry (screen pixels, unscaled) -- shared
// between Editor::ResizeSheetViewport's scroll-follow arithmetic and
// main.cpp's DrawPane sheet-grid renderer, same "must agree exactly"
// reasoning as kPdfPageGapPx above. Deliberately NOT tied to
// g_font_size/g_char_width the way the rest of the editor's layout is --
// sheet cells are fixed-size grid slots, not word-wrapped/font-measured
// text, which is what lets ResizeSheetViewport do its own scroll-follow
// entirely in this raylib-free file (see SheetSession::scroll_row's own
// comment).
inline constexpr int kSheetRowHeaderW = 50;
inline constexpr int kSheetColWidth = 90;
inline constexpr int kSheetRowHeight = 22;

// One PDF-viewer pane's state, keyed by buffer id the same way ImageSession
// is (see its comment) -- a PDF buffer's Buffer::lines is also a dummy
// single empty line. Unlike ImageDoc's fixed one-time decode, PdfDoc
// (pdf_doc.h) content must be *re-rendered* per page/zoom level.
//
// Supports zathura-style continuous vertical scrolling across page
// boundaries (not just panning within one page): `page` is the *anchor*
// page -- the one `scroll_y` is measured from the top of -- and a small
// virtualized window of rasters (anchor page, plus its immediate
// neighbors) is kept rendered at any time via Editor::
// EnsurePdfPagesRastered, evicting everything else. This keeps memory
// bounded regardless of document length (a 400-page PDF never rasterizes
// more than ~3 pages at once) while still letting h/j/k/l scroll smoothly
// through page boundaries instead of hard-cutting between pages.
struct PdfSession {
    int buffer_id = 0;
    std::unique_ptr<PdfDoc> doc;
    int page = 0;  // 0-indexed anchor page
    // Vertical: device pixels (post-zoom, i.e. "on-screen" pixels) scrolled
    // into `page` from its top -- can transiently go negative or past the
    // anchor page's on-screen height; Editor::HandlePdfInput re-bases it
    // (adjusting `page`) back into range after every scroll. Horizontal:
    // same panning semantics as ImageSession, clamped against the anchor
    // page's on-screen width.
    float scroll_y = 0;
    int pan_x = 0;
    int viewport_w = 0, viewport_h = 0;
    // Multiplier on top of each cached raster's already-rasterized pixels
    // (NOT on a page's native point size -- see Editor::
    // EnsurePdfPagesRastered). 1.0 = display the last render as-is;
    // Editor::HandlePdfInput keeps this within roughly [0.5, 2.0] by
    // folding larger drifts into a fresh render at a new rendered_scale
    // instead, so text stays crisp. Because rendered_scale absorbs the
    // drift (rendered_scale *= zoom; zoom = 1), the on-screen size of any
    // page is unaffected by a rescale -- only scroll_y/pan_x need
    // reinterpreting across a *page* change, never across a rescale.
    float zoom = 1.0f;
    float rendered_scale = 0.0f;  // px per PDF point every cached raster below shares
    // If false, pages render with PDFium's own colors (the PDF's actual
    // content); if true (the default), every rendered page is recolored to
    // match the editor's color scheme (ResolveHlGroup("Normal")/
    // ("NormalBg")) instead of white-paper-with-black-text. Toggled by
    // Ctrl-R (HandlePdfInput). The recoloring itself happens in main.cpp
    // at texture-upload time (see ThemedPdfChannel), not here -- this
    // struct stays raylib-free like ImageSession.
    bool theme_colors = true;

    // Lazily rendered, evicted down to {page-1, page, page+1} every frame
    // by Editor::EnsurePdfPagesRastered. Keyed by page index so it degrades
    // gracefully at document boundaries (no entry for page-1 at page 0).
    struct PageRaster {
        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        // Bumped whenever this specific page is (re)rendered (new page
        // entered the window, or a rescale invalidated every cached
        // raster). main.cpp's per-page texture cache compares this (and
        // theme_colors) against what it last uploaded.
        int generation = 0;
        // Device-pixel highlight rects for whichever of `search_matches`
        // land on this page, at this raster's own scale -- recomputed
        // alongside rgba (Editor::EnsurePdfPagesRastered) and whenever
        // search_matches itself changes (Editor::RunPdfSearch), so
        // main.cpp's draw code never needs to touch PdfDoc/PDFium itself.
        std::vector<PdfHighlightRect> highlights;
    };
    std::unordered_map<int, PageRaster> rasters;
    int next_raster_generation = 1;
    // Memoized PdfDoc::PageWidthPt/HeightPt results -- avoids repeated
    // FPDF_LoadPage/ClosePage round-trips for page-size queries the
    // scroll/rebase math needs every frame while actively scrolling.
    std::unordered_map<int, std::pair<double, double>> page_size_pt;

    // --- '/' text search (Editor::HandlePdfInput) ---
    // True while the user is typing a query after pressing '/' -- captures
    // all subsequent character/Backspace/Enter/Escape input instead of the
    // normal pan/zoom/page-nav keys (mirrors Mode::Command's own
    // input-capture shape, just scoped to this one PdfSession rather than
    // a separate Mode). search_input is the in-progress query text.
    bool search_active = false;
    std::string search_input;
    // The last *submitted* (Enter-confirmed) query; empty means no active
    // search / no highlights. Set together with search_matches by
    // Editor::RunPdfSearch.
    std::string search_query;
    std::vector<PdfTextMatch> search_matches;  // document-wide, in page order
    // Index into search_matches the last `n`/`p`/search-submit jumped to
    // (Editor::GotoPdfMatch); -1 if there's no current match (e.g. a
    // search with zero hits). Drawn more prominently than other matches.
    int search_current = -1;
};

// One HTML-preview pane's state, keyed by buffer id the same way Image/
// PdfSession are (see their comments) -- an html buffer's Buffer::lines
// also stays a dummy single empty line, real content lives here as an
// HtmlDoc. Unlike PdfSession's paginated continuous-scroll, layout here is
// one continuous flow (main.cpp's own word-wrap pass, not cached here --
// see html_doc.h's own header on why layout stays out of the raylib-free
// doc model, and DrawPane's html branch for why recomputing it fresh every
// frame is fine at the page sizes this renderer targets); `scroll_y` is
// just a plain pixel offset into that flow, clamped by DrawPane against
// whatever total content height that frame's layout pass computed.
struct HtmlSession {
    int buffer_id = 0;
    HtmlDoc doc;
    // The path/URL this was opened from, for the pane header label and
    // for resolving a local <img src="relative/path"> against the page's
    // own directory (DrawPane's html branch) -- not necessarily the same
    // as Buffer::filename, which mep.browse_command may point at a throw-
    // away curl-fetched temp file for a remote URL (kBuiltinTextTools).
    std::string source;
    // The user-facing URL/path this page was actually opened *from* --
    // for a remote page `source` is a throwaway curl-fetched temp file
    // (a fresh one per fetch, see OpenHtmlInPlace's own comment), so
    // reload (mep.browse_reload, kBuiltinTextTools -- needs to know what
    // to re-fetch) and the address bar's own prefill (mep.browse_open_bar)
    // both read this instead. Equal to `source` for a plain local-file
    // open, where there's no such indirection.
    std::string origin;
    float scroll_y = 0;
    int viewport_w = 0, viewport_h = 0;
    // Multiplies the pane's base font size -- same +/-/= convention as
    // ImageSession::zoom/PdfSession::zoom (Editor::HandleHtmlInput).
    float zoom = 1.0f;
    // If true (the default, matching PdfSession::theme_colors' own
    // default), the editor's own color scheme takes over: DrawPane skips
    // every element's own background/border color, forces all text to
    // ResolveHlGroup("Normal"), and recolors local <img> textures the same
    // luminance-to-theme-gradient way PdfSession::theme_colors already
    // does for a PDF page (see ThemedPdfChannel's own comment) -- a
    // reading-mode default, not a color-accurate filter. If false, the
    // page renders with its own CSS colors/images instead, same as a real
    // browser. Toggled by Ctrl-R (Editor::HandleHtmlInput).
    bool theme_colors = true;
};

// One WYSIWYG office-document pane's state, keyed by buffer id the same
// way Image/PdfSession are (see their comments) -- an office buffer's
// Buffer::lines also stays a dummy single empty line, real content lives
// here as an OfficeDoc. Unlike ImageDoc/PdfDoc, OfficeDoc is held *by
// value* rather than behind a unique_ptr: it's a plain data struct (see
// office_doc.h) with no opaque C-library handle to hide, so there's
// nothing gained by the extra indirection.
struct OfficeSession {
    int buffer_id = 0;
    OfficeDoc doc;
    // The file's own original bytes, kept from open time so a later save
    // (Phase 4 -- SaveDocx/OdtToMemory) doesn't need to re-read the file
    // from disk and can copy every untouched ZIP entry straight through
    // (mirrors how PdfDoc::Impl keeps its source bytes alive).
    std::vector<unsigned char> original_bytes;

    // Cursor position: paragraph index + a byte offset into that
    // paragraph's flat text -- deliberately word-wrap-*oblivious*, see
    // Mode::OfficeNormal's own comment for why this mirrors the main
    // buffer's own no-soft-wrap motion model rather than being a new
    // pattern.
    int cursor_para = 0, cursor_col = 0;

    // Visual-mode selection anchor (Mode::OfficeVisual); the selection
    // itself is always [min(anchor,cursor), max(anchor,cursor)) by
    // paragraph+col comparison, not stored separately.
    bool has_selection = false;
    int sel_anchor_para = 0, sel_anchor_col = 0;

    bool modified = false;
    // Snapshot-based undo/redo, mirroring Buffer::undo_stack/redo_stack's
    // own full-vector-copy convention exactly (not diffs) -- pushed once
    // per insert-session/operator/format-toggle, never per keystroke.
    std::vector<std::vector<DocParagraph>> undo_stack, redo_stack;

    int viewport_w = 0, viewport_h = 0;
    // Scroll position: index of the topmost paragraph currently drawn,
    // plus which of ITS word-wrapped visual lines to start at (in case a
    // single paragraph is taller than the viewport). Adjusted a paragraph
    // at a time to keep cursor_para in view, mirroring Pane::scroll_row's
    // own per-line adjustment -- not pixel-perfect sub-paragraph
    // scrolling if one paragraph is dramatically taller than the
    // viewport, an accepted v1 simplification.
    int scroll_para = 0;
    int scroll_line_in_para = 0;
    // Last cursor_para DrawPane's scroll-follow scan (main.cpp) saw (-1 =
    // never run yet) -- same "was this a deliberate far jump or an
    // incidental one" role as Pane::scroll_follow_last_cursor_row plays
    // for the plain text editor, driving the same per-frame catch-up cap
    // so a paragraph's anchored image/table (OfficeParagraphExtraHeight)
    // slides into view over a few frames instead of jumping in one.
    int scroll_follow_last_cursor_para = -1;

    // Zoom mirrors PdfSession::zoom/rendered_scale's own settle-band
    // pattern (see its comment): `zoom` is a residual multiplier that
    // HandleOfficeNormalInput keeps within a settled band, folding larger
    // drifts into base_font_pt instead -- kept per-session rather than
    // tied to the global g_font_size the way ImageSession/PdfSession's
    // zoom is also independent of it.
    float zoom = 1.0f;
    float base_font_pt = 15.0f;

    // Table-cell navigation (Editor::EnterOfficeTable/ExitOfficeTable/
    // MoveOfficeTableCell): -1 means "not currently inside a table",
    // otherwise an index into doc.tables. Deliberately separate from
    // cursor_para/cursor_col rather than reusing them for a cell address --
    // a table doesn't get its own paragraph-flow position (it's anchored
    // to one via DocParagraph::table_ref instead), so overloading
    // cursor_para/col to sometimes mean "row/col within this table" would
    // make every other piece of paragraph-cursor logic (word-wrap,
    // rendering, undo) need to know which meaning currently applies.
    int in_table_edit = -1;
    int table_cursor_row = 0, table_cursor_col = 0;
    // While editing one cell's text (i/a inside a table, mirroring
    // Mode::OfficeInsert's own paragraph-text editing) -- true means
    // char-by-char edits go to the cell at table_cursor_row/col instead of
    // a paragraph.
    bool table_cell_editing = false;
    // Byte offset into the cell currently being edited -- a table cell is
    // plain text (no spans), so this is a plain int cursor, not a
    // paragraph-style col that also has to account for spans.
    int table_cell_col = 0;

    // Word-wrap of the cursor's own paragraph, cached from the last time
    // DrawPane (main.cpp) rendered it -- editor.cpp is raylib-free and
    // can't word-wrap itself (needs MeasureTextEx), so
    // Editor::MoveOfficeCursorVisualLine (j/k's "move to the visual line
    // above/below", not just the prev/next paragraph) reads this
    // last-frame answer instead, via Editor::SetOfficeCursorWrapLines --
    // same "main.cpp computes, a plain setter bridges it into state here"
    // split ResizeOfficeViewport/SetOfficeScroll already establish for the
    // same underlying reason. cursor_wrap_lines_para guards against
    // trusting stale data left over from a different paragraph (e.g. right
    // after a jump that skipped a Draw cycle); callers must check it
    // against cursor_para before trusting cursor_wrap_lines.
    int cursor_wrap_lines_para = -1;
    std::vector<std::pair<int, int>> cursor_wrap_lines;  // [start,end) byte range per visual line
};

// One spreadsheet pane's state, keyed by buffer id the same way
// Image/Pdf/OfficeSession are -- a sheet buffer's Buffer::lines also
// stays a dummy single empty line, real content lives here as a
// Workbook. Held *by value* like OfficeDoc, for the same reason (a plain
// data struct, no opaque C-library handle to hide).
struct SheetSession {
    int buffer_id = 0;
    Workbook wb;
    // Kept from open time so a later save doesn't need to re-read the
    // file from disk and can copy every untouched ZIP entry straight
    // through -- mirrors OfficeSession::original_bytes exactly.
    std::vector<unsigned char> original_bytes;

    int active_sheet = 0;
    int cursor_row = 0, cursor_col = 0;

    // Visual-mode selection anchor (Mode::SheetVisual) -- selects a
    // rectangular cell *range*, unlike OfficeSession's text span; the
    // range itself is always [min(anchor,cursor), max(anchor,cursor)] by
    // row/col comparison on each axis independently, not stored
    // separately.
    bool has_selection = false;
    int sel_anchor_row = 0, sel_anchor_col = 0;

    bool modified = false;
    // Snapshot-based undo/redo, mirroring OfficeSession's own full-copy
    // convention -- pushed once per edit commit, never per keystroke.
    std::vector<Workbook> undo_stack, redo_stack;

    int viewport_w = 0, viewport_h = 0;
    // Top-left visible cell -- unlike Office's vertical-only scroll_para,
    // a grid scrolls in both dimensions. A real simplification vs. the
    // Office pane: sheet cells are fixed-width/height grid slots, not
    // word-wrapped flowing text, so (unlike ResizeOfficeViewport/
    // SetOfficeScroll, which had to punt scroll-follow into main.cpp
    // because it needs MeasureTextEx) ResizeSheetViewport can do the
    // whole scroll-follow job itself, entirely raylib-free, as ordinary
    // integer row/col arithmetic -- no main.cpp-side follow-up call
    // needed.
    int scroll_row = 0, scroll_col = 0;

    // Insert-mode live edit state (Mode::SheetInsert): editing the
    // current cell's text as a plain string, seeded from its `raw` on
    // entry, committed into the cell (SetCellRaw) on Enter/Escape --
    // mirrors the main Buffer's own insert-in-place editing, not
    // Office's per-paragraph span model (a cell has no rich-text
    // formatting to preserve across the edit).
    bool editing = false;
    std::string edit_buffer;
    int edit_cursor = 0;
};

// Fixed Kanban card/column geometry (screen pixels, unscaled) -- shared
// between main.cpp's DrawKanban/UpdateKanbanMouseInteraction, same "must
// agree exactly" reasoning as kSheetRowHeaderW/kSheetColWidth above.
inline constexpr int kKanbanColumnWidth = 260;
inline constexpr int kKanbanCardHeight = 64;
inline constexpr int kKanbanCardGap = 10;

// Gantt label-column width's *default* (screen pixels, unscaled) -- the
// user can drag it wider/narrower per-buffer afterward (GanttSession::
// label_col_w below). Row height isn't listed here at all: main.cpp's
// GanttRowHeight() derives it from the live font size instead of a fixed
// constant, so a row never clips its label regardless of zoom. Pixels-per-
// day is also not fixed (it's GanttSession::pixels_per_day, the user's
// zoom level).
inline constexpr int kGanttLabelColW = 220;

// Which overlay (if any) a .org buffer is currently being shown/edited
// through -- see org_doc.h's top comment for why this is a thin render/
// interaction overlay rather than an alternate storage model the way
// Sheet/Office/Pdf are. Absent from Editor::org_view_mode_ (the default
// map lookup) means Text -- the buffer's ordinary text-editing view.
enum class OrgViewMode { Text, Kanban, Gantt };

// One Kanban-board pane's state, keyed by buffer id (Editor::
// kanban_views_) the same way SheetSession is. `outline` is a disposable
// cache: every mutation method (Editor::KanbanSetCardColumn and friends)
// re-parses it from Buf().lines right after splicing the edited line(s)
// in, rather than patching it incrementally -- see org_doc.h's top
// comment for why.
struct KanbanSession {
    int buffer_id = 0;
    OrgOutline outline;
    int focused_column = 0;
    int focused_row = 0;

    // Mode::KanbanInsert live rename state -- mirrors SheetSession::
    // editing/edit_buffer/edit_cursor exactly (a card title is plain text,
    // nothing rich to preserve across the edit).
    bool editing = false;
    std::string edit_buffer;
    int edit_cursor = 0;
    int editing_headline_index = -1;

    // Press/move-threshold/release drag state, main.cpp's
    // UpdateKanbanMouseInteraction's own PaneDragState-style convention
    // (main.cpp:424-451's kind/threshold/start-pos shape), scoped per-
    // buffer here instead of one shared global since more than one pane
    // could show a Kanban view at once.
    bool dragging = false;
    int drag_headline_index = -1;
    float drag_start_x = 0, drag_start_y = 0;
    bool drag_threshold_passed = false;
    // True when this drag started from the "+ New Card" chip rather than an
    // existing card -- drag_headline_index is meaningless (-1) in that
    // case; DrawKanban draws a "New card" ghost instead of indexing
    // outline.headlines, and release creates the card via KanbanNewCard
    // (into drop_column) instead of moving one.
    bool drag_is_new_card = false;
    // A header drag is deliberately separate from a card/new-card drag:
    // its target is a gap between columns rather than a card row.
    bool drag_is_column = false;
    int drag_column_index = -1;
    int column_drop_slot = -1;
    // Live drop-target preview while dragging past the threshold -- only
    // committed (KanbanSetCardColumn/KanbanMoveCardBefore/KanbanNewCard) on
    // release.
    int drop_column = -1;
    int drop_row = -1;

    // The content area DrawKanban (main.cpp) most recently rendered this
    // board into -- refreshed every frame it's drawn, read back by
    // UpdateKanbanMouseInteraction (called afterward, once per frame, same
    // as main.cpp's own UpdatePaneMouseInteraction) so hit-testing a click/
    // drag never needs its own separate geometry computation.
    float content_x = 0, content_y = 0, content_w = 0, content_h = 0;
    // The "+ New Card" chip's own screen rect (same raylib-free float-quad
    // convention as content_x/y/w/h just above, not a Rectangle), same
    // refresh/read-back split -- UpdateKanbanMouseInteraction hit-tests a
    // press against it directly (a drag needs continuous IsMouseButtonDown
    // polling, not a RegisterClickRegion fire-once callback).
    float new_card_chip_x = 0, new_card_chip_y = 0, new_card_chip_w = 0, new_card_chip_h = 0;
};

// One Gantt-chart pane's state, keyed by buffer id (Editor::gantt_views_).
// Same disposable-outline-cache convention as KanbanSession.
struct GanttSession {
    int buffer_id = 0;
    OrgOutline outline;

    // Zoom (pixels drawn per calendar day) and pan (the day-ordinal --
    // OrgDayNumber's return value -- shown at the timeline's left edge).
    // ruler_scale only changes the calendar grid/labels; task placement
    // remains day-accurate at every scale.
    enum class RulerScale { Days, Months, Years };
    float pixels_per_day = 24.0f;
    long long anchor_day = 0;
    RulerScale ruler_scale = RulerScale::Days;
    int focused_row = 0;
    // Summary tasks can be folded without mutating the Org hierarchy.
    std::unordered_set<int> collapsed_headlines;
    bool pending_fold_command = false;  // leading `z` for Vim-style Gantt folds

    // Label column width, user-resizable by dragging the divider at
    // content_x + label_col_w (main.cpp's UpdateGanttMouseInteraction) --
    // starts at the old fixed kGanttLabelColW but is per-buffer mutable
    // from there on, unlike kGanttLabelColW itself.
    float label_col_w = static_cast<float>(kGanttLabelColW);
    bool resizing_label_col = false;

    enum class DragMode { Move, ResizeStart, ResizeEnd };
    bool dragging = false;
    int drag_headline_index = -1;
    DragMode drag_mode = DragMode::Move;
    float drag_start_x = 0;
    // Original (pre-drag) timestamps to diff the live mouse position
    // against every frame, and to fall back to if the drag is released
    // before crossing the drag threshold. drag_orig_deadline may be
    // !present (dragging a milestone's virtual right edge to create a
    // brand-new deadline) -- callers computing an "orig day" from it must
    // fall back to drag_orig_scheduled in that case (see DrawGantt's own
    // live-preview fallback).
    OrgTimestamp drag_orig_scheduled, drag_orig_deadline;

    // Mode::GanttInsert live rename state -- mirrors KanbanSession::
    // editing/edit_buffer/edit_cursor/editing_headline_index exactly (a
    // headline title is plain text here too).
    bool editing = false;
    std::string edit_buffer;
    int edit_cursor = 0;
    int editing_headline_index = -1;

    // Same purpose as KanbanSession::content_x/y/w/h above -- refreshed by
    // DrawGantt every frame, read back by UpdateGanttMouseInteraction.
    float content_x = 0, content_y = 0, content_w = 0, content_h = 0;
};

// Myers diff (NVIM_PARITY_PLAN.md Part IV Phase 17): the equivalent of
// Neovim's built-in vim.diff(), needed so git-gutter hunks don't have to
// shell `git diff` per keystroke. Declared here (not lua_env.cpp, where it
// originated) so Editor::GitGutterRefresh/GitStageHunk (kBuiltinGit's own
// port, editor.cpp) can call it directly -- mep.diff_lines (lua_env.cpp)
// is the other, still-Lua-facing caller.
struct DiffHunk {
    int old_start, old_count, new_start, new_count;
};

// Classic O(ND) Myers diff (Myers 1986), operating on opaque line indices
// via equality only -- returns the *edit script* as a sequence of (line
// present only in `a`) / (line present only in `b`) markers, coalesced
// into contiguous hunks.
/**
 * @brief Computes the Myers O(ND) diff between two line sequences, coalesced into contiguous hunks.
 * @param a The "old" line sequence.
 * @param b The "new" line sequence.
 * @return The edit script as a sequence of DiffHunk ranges.
 */
std::vector<DiffHunk> MyersDiffHunks(const std::vector<std::string> &a, const std::vector<std::string> &b);

// mep_lsp_filetype/mep_lsp_abspath's own port (LUA_TO_CPP_PLAN.md Phase
// 5 side quest): pure string utilities -- a bare file extension, and a
// path resolved against the process cwd -- with *no* dependency on
// g_lsp_clients/JSON-RPC framing (the actual stateful LSP machinery,
// still Lua, its own future phase per this plan's coupling note). They
// merely happen to be defined inside kBuiltinLsp's source for proximity;
// dozens of call sites across the org cluster and elsewhere use them for
// plain filetype/path logic, so -- mirroring Org-0's own reasoning --
// they're ported and registered as bare globals (same exact names) here,
// unblocking those call sites without touching real LSP state at all.
/**
 * @brief Returns a file's extension-derived filetype tag, mirroring mep_lsp_filetype.
 * @param fname The filename to inspect.
 * @return The filetype string (typically the lowercased extension).
 */
std::string LspFiletype(const std::string &fname);
/**
 * @brief Resolves a filename to an absolute path against the process's current working directory.
 * @param fname The filename or relative path to resolve.
 * @return The resolved absolute path.
 */
std::string LspAbspath(const std::string &fname);

// kBuiltinOrgRoam's own `title:lower():gsub('[^%w]+', '-'):gsub('^%-+',
// ''):gsub('%-+$', '')` port (mep.org_roam_new_note's filename slug):
// lowercase, every run of non-alphanumeric characters collapsed to a
// single '-', leading/trailing '-' stripped. No Editor state needed.
/**
 * @brief Slugifies an org-roam note title into a filename-safe form.
 * @param title The note title to slugify.
 * @return The lowercased title with non-alphanumeric runs collapsed to '-' and leading/trailing '-' stripped.
 */
std::string OrgRoamSlugify(const std::string &title);

// kBuiltinOrgExport's own ports (LUA_TO_CPP_PLAN.md Phase 5): the
// format-agnostic text utilities the export walk uses, none of which
// touch the Lua-configurable mep.org_export_marks table (which embeds a
// real Lua closure for `link` -- a user-customization point left
// alone, so mep_org_inline_convert itself, and the giant
// mep.org_export walk built on it, stay Lua). No Editor state needed.
//
// mep_org_export_heading's own port. `format` is "html"/"markdown"/
// anything else treated as ascii, matching the original's own
// if/elseif/else.
/**
 * @brief Renders an org headline as an exported heading in the given output format.
 * @param format The export format: "html", "markdown", or anything else treated as plain ascii.
 * @param level The headline's nesting level (number of leading '*'s).
 * @param title The headline's title text.
 * @return The formatted heading string.
 */
std::string OrgExportHeading(const std::string &format, int level, const std::string &title);
// mep_org_html_escape's own port (&/</> only, matching the original).
/**
 * @brief Escapes &, <, and > for safe inclusion in HTML output.
 * @param s The raw text to escape.
 * @return The HTML-escaped text.
 */
std::string OrgHtmlEscape(const std::string &s);
// mep_org_subtree_end_lines's own port: mep_org_subtree_end's own
// logic, operating on an arbitrary lines array (1-indexed `row`, same
// convention as the buffer-based original) instead of the live buffer
// -- the export walk resolves #+INCLUDE:/babel-result-splicing into a
// scratch array first, so subtree-skipping (:noexport:) needs a
// version that doesn't call mep.get_line/mep.line_count.
/**
 * @brief Finds the last row of the org subtree starting at `row`, operating on an arbitrary lines array.
 * @param lines The lines array to scan.
 * @param row The 1-indexed row of the subtree's headline.
 * @param todo_keywords The configured org TODO keywords, used to parse headlines correctly.
 * @return The 1-indexed row where the subtree ends.
 */
int OrgSubtreeEndLines(const std::vector<std::string> &lines, int row, const std::vector<std::string> &todo_keywords);
// mep_org_collect_macros's own port: every `#+MACRO: name body` line's
// name -> body-with-$N-placeholders.
/**
 * @brief Collects every `#+MACRO: name body` declaration in a buffer.
 * @param lines The buffer lines to scan.
 * @return A map from macro name to its body text (with $N placeholders).
 */
std::map<std::string, std::string> OrgCollectMacros(const std::vector<std::string> &lines);
// mep_org_expand_macro_line's own port: expands `{{{name(a,b)}}}` and
// `{{{name}}}` macro references against `macros` (from
// OrgCollectMacros), substituting each `$N` placeholder with the Nth
// comma-separated argument (1-indexed, "" if out of range). An
// undefined macro name is left as literal text.
/**
 * @brief Expands `{{{name(a,b)}}}`/`{{{name}}}` macro references in a line of text.
 * @param line The line of text containing macro references to expand.
 * @param macros The macro name-to-body map, as produced by OrgCollectMacros.
 * @return The line with every recognized macro reference substituted.
 */
std::string OrgExpandMacroLine(const std::string &line, const std::map<std::string, std::string> &macros);

// kBuiltinOrgBabel's own ports (LUA_TO_CPP_PLAN.md Phase 5): the block-
// detection parser and its direct dependencies -- pure buffer-scan/
// string-parsing, no async. This is a large (1174-line), heavily
// job/process/session-orchestration-and-closure-table-coupled block
// (a ~27-language descriptor table of Lua closures for var/print
// statement rendering and program-wrapping, real subprocess spawning
// for compile+run, REPL session management, results-block caching);
// per this plan's own note it needs its own multi-session budget.
// This first pass covers only what's cleanly portable without that
// machinery.
//
// mep_org_babel_should_wrap_main's own port. The `local`
// MEP_ORG_BABEL_WRAP_DEFAULT table (php='yes', every other wrap_main
// language defaults 'no') isn't Lua-configurable to begin with (it's
// chunk-private, no mep.* exposure), so hardcoding it here changes
// nothing observable.
/**
 * @brief Decides whether a babel source block's code should be wrapped in a `main` function before running.
 * @param lang_key The babel language key (e.g. "php").
 * @param args_str The block's raw header-args string.
 * @return True if the language/args combination calls for a main-wrapper.
 */
bool OrgBabelShouldWrapMain(const std::string &lang_key, const std::string &args_str);
// mep_org_babel_format_literal's own port: a bare integer/decimal
// passes through unquoted; anything else becomes a backslash/quote/
// newline/CR/tab-escaped double-quoted string literal.
/**
 * @brief Formats a raw babel `:var` value as a source-code literal.
 * @param raw The raw variable value text.
 * @return The value unquoted if it's a bare integer/decimal, otherwise an escaped double-quoted string literal.
 */
std::string OrgBabelFormatLiteral(const std::string &raw);
// mep_org_parse_vars's own port: `:var name=value` pairs (value is
// either a `"..."` string with `\"`/`\\` escapes, or a bare non-space
// token).
/**
 * @brief Parses a babel block's `:var name=value` header-arg pairs.
 * @param args_str The block's raw header-args string.
 * @return A map from variable name to its (possibly quoted) value text.
 */
std::map<std::string, std::string> OrgParseVars(const std::string &args_str);
// mep_org_parse_results's own port: the `:results` header-arg's
// space-separated mode keywords, as a set.
/**
 * @brief Parses a babel block's `:results` header-arg into its space-separated mode keywords.
 * @param args_str The block's raw header-args string.
 * @return The set of `:results` mode keywords present.
 */
std::set<std::string> OrgParseResults(const std::string &args_str);
// mep_org_src_block_at's own port: the #+begin_src/#+end_src block
// containing (or starting at) `row`, or not-found. Bare Lua global
// (`mep_org_src_block_at`, lua_env.cpp) -- kBuiltinOrgPolyglot (a
// separate DoString chunk) shares this exact name already.
struct OrgSrcBlock {
    bool found = false;
    int start_row = 0;
    int end_row = 0;
    bool has_lang = false;
    std::string lang;  // lowercased
    std::map<std::string, std::string> vars;
    bool has_tangle = false;
    std::string tangle;
    bool has_cache = false;
    std::string cache;
    bool has_file = false;
    std::string file;
    std::set<std::string> results_modes;
    std::string args_str;
    std::string body;
};

// mep_diag_wrap's own port (LUA_TO_CPP_PLAN.md Phase LSP): greedy word-
// wrap of `text` to `width` columns (mep.float_preview itself doesn't
// wrap). No Editor state needed. Always returns at least one line
// (an empty string for empty input, matching the original).
/**
 * @brief Greedily word-wraps text to a fixed column width.
 * @param text The text to wrap.
 * @param width The maximum column width per line.
 * @return The wrapped lines; always at least one, even for empty input.
 */
std::vector<std::string> LspDiagWrap(const std::string &text, int width);

// Org-mode "Phase Org-0" primitives (LUA_TO_CPP_PLAN.md): the headline
// parser + two buffer-scan helpers built on it are called from ~80 sites
// across the whole org-mode cluster (kBuiltinOrg and its 10 dependent
// blocks) as *bare Lua globals* -- not `mep.*` table members --
// `mep_org_parse_headline`/`mep_org_current_headline_row`/
// `mep_org_subtree_end`, defined with a bare `function` (no `local`) in
// kBuiltinOrg specifically so every other block's own separately-compiled
// DoString chunk can see them too (Lua globals, unlike locals, persist
// across chunks). Declared here so lua_env.cpp can register C
// replacements under those exact same bare-global names (lua_register,
// same idiom this file already uses for `print` -- see LuaEnv::LuaEnv) --
// every one of those ~80 call sites keeps working completely unchanged.
struct OrgHeadlineParse {
    bool is_headline = false;
    int level = 0;
    std::string todo;  // empty if has_todo is false
    bool has_todo = false;
    std::string priority;  // single letter, empty if has_priority is false
    bool has_priority = false;
    std::string title;
    std::string tags;  // colon-joined, e.g. "work:urgent"; empty if has_tags is false
    bool has_tags = false;
};

// mep_org_parse_headline's own port. `todo_keywords` is
// mep.org_todo_keywords' current value (Lua-configurable, so read fresh
// by the caller each call rather than cached here).
/**
 * @brief Parses a single line as an org headline, extracting its level, TODO state, priority, title, and tags.
 * @param line The line of text to parse.
 * @param todo_keywords The currently configured org TODO keywords to recognize.
 * @return The parsed headline fields; is_headline is false if `line` isn't a headline.
 */
OrgHeadlineParse ParseOrgHeadline(const std::string &line, const std::vector<std::string> &todo_keywords);

// A modal (vim-like) text editor: Normal/Insert/Visual/Command modes, a
// handful of motions and operators, undo/redo, buffers/panes/tabs, and a
// ":" command line that can dispatch into Lua. Does not attempt full Vim
// parity -- no registers, marks, text objects, macros, or regex search.
// Pane navigation is directional (NavigatePaneDirection), not just
// cycle-order, and reachable both via Ctrl-W h/j/k/l and mod1+h/j/k/l.
class Editor {
public:
    /**
     * @brief Constructs a new Editor with its default initial state (a single empty scratch buffer/pane/tab).
     */
    Editor();
    // Declared (not defaulted here) purely so the compiler-generated body
    // -- which needs to destroy TerminalSession's std::unique_ptr<VTerm>
    // member, requiring VTerm's complete type -- is emitted in editor.cpp
    // (which includes vterm.h) rather than wherever an Editor happens to
    // be destroyed (main.cpp, which has no reason to know about VTerm at
    // all): the classic "incomplete type in unique_ptr" fix.
    /**
     * @brief Destroys the Editor. Defined out-of-line so members like TerminalSession's unique_ptr<VTerm> can be destroyed with complete types visible.
     */
    ~Editor();
    // Exactly one Editor exists for the process's whole lifetime (main.cpp's
    // g_editor global) and it owns a huge amount of non-trivially-copyable
    // state (buffers, panes, terminal sessions, the CollabSession
    // unique_ptr, ...) -- copying or moving it is never meaningful, so both
    // are disabled explicitly rather than left to compiler-generated
    // (and almost certainly wrong) defaults.
    Editor(const Editor &) = delete;
    Editor &operator=(const Editor &) = delete;
    Editor(Editor &&) = delete;
    Editor &operator=(Editor &&) = delete;

    /**
     * @brief Sets the Lua environment used for editor-Lua interop.
     * @param lua Pointer to the LuaEnv instance to use.
     */
    void SetLuaEnv(LuaEnv *lua) { lua_ = lua; }
    /**
     * @brief Returns the associated Lua environment.
     * @return Pointer to the current LuaEnv instance.
     */
    LuaEnv *Lua() const { return lua_; }

    // Reads raylib's input state directly and advances editor state by one
    // frame's worth of key events. Call once per frame.
    /**
     * @brief Reads raylib's input state and advances editor state by one frame's worth of key events.
     */
    void HandleInput();

    // Adjusts the given pane's scroll so its cursor stays visible within
    // `visible_lines` rows. Call once per frame for every rendered pane
    // (each may have a different height depending on the split layout).
    // `wrap_cols` is the pane's current soft-wrap budget in characters (see
    // Wrap()) -- 0 disables wrap-aware slot counting, matching main.cpp's
    // own "wrap_cols<=0 means wrap is off" convention. A row wider than
    // wrap_cols counts as multiple visual slots here, the same way a
    // closed fold or org inline image already does, so the cursor's own
    // wrapped row can't scroll itself half off-screen.
    /**
     * @brief Adjusts a pane's scroll offset so its cursor stays visible within its visible rows.
     * @param pane_id The id of the pane to adjust.
     * @param visible_lines The number of visual rows currently visible in the pane.
     * @param wrap_cols The pane's soft-wrap budget in characters; 0 disables wrap-aware slot counting.
     */
    void UpdateScrollForPane(int pane_id, int visible_lines, int wrap_cols = 0);

    /**
     * @brief Returns the editor's current mode.
     * @return The current Mode.
     */
    Mode CurrentMode() const { return mode_; }
    // R (Replace mode): true while Mode::Insert should show "REPLACE" in
    // the status line instead of "INSERT" -- see replace_mode_'s own
    // comment for why this isn't a distinct Mode value.
    /**
     * @brief Returns whether Insert mode should currently be displayed as Replace mode.
     * @return True if Replace mode is active.
     */
    bool IsReplaceMode() const { return replace_mode_; }
    // :set number/nonumber -- whether main.cpp's renderer should draw a
    // line-number gutter.
    /**
     * @brief Returns whether the line-number gutter should be drawn.
     * @return True if line numbers are enabled (:set number).
     */
    bool ShowLineNumbers() const { return show_line_numbers_; }
    // :set relativenumber/norelativenumber -- whether each row's own
    // number is its distance from the cursor line rather than its
    // absolute line number. Independent of ShowLineNumbers() (real Vim
    // allows relativenumber with nonumber, showing "0" on the cursor
    // line) -- main.cpp's renderer reserves gutter space whenever
    // *either* is on, and shows the cursor line's own number as its
    // real absolute one (left-aligned, the Vim/Neovim "hybrid" look)
    // whenever this is on, matching the everyday "number relativenumber
    // together" setup this editor now defaults to.
    /**
     * @brief Returns whether line numbers should be shown relative to the cursor line.
     * @return True if relative numbering is enabled (:set relativenumber).
     */
    bool ShowRelativeNumbers() const { return show_relative_numbers_; }
    // :set cursorline/nocursorline -- whether main.cpp's renderer should
    // tint the active pane's cursor row with the "CursorLine" theme group.
    /**
     * @brief Returns whether the active pane's cursor row should be highlighted.
     * @return True if cursorline highlighting is enabled (:set cursorline).
     */
    bool ShowCursorLine() const { return show_cursorline_; }
    // :set wrap/nowrap -- whether main.cpp's renderer should soft-wrap a
    // row too wide for the pane onto extra visual rows (the buffer itself
    // still has exactly one logical line there; j/k and line numbering
    // stay row-based, matching real Vim's default 'wrap' behavior).
    /**
     * @brief Returns whether long lines should be soft-wrapped for display.
     * @return True if soft wrap is enabled (:set wrap).
     */
    bool Wrap() const { return wrap_; }
    // <leader>oti / mep.org_images_toggle -- whether main.cpp's renderer
    // should substitute a rendered texture for a Buffer::org_image_rows
    // row instead of its ordinary [[file:...]] text.
    /**
     * @brief Returns whether inline images should be rendered for org [[file:...]] links.
     * @return True if org inline image rendering is toggled on.
     */
    bool OrgImagesVisible() const { return org_images_visible_; }
    // <leader>otl / mep.org_latex_toggle -- whether main.cpp's renderer
    // should substitute a rendered texture for a Buffer::org_latex_rows row
    // instead of its ordinary math/LaTeX source text. Also consulted by
    // kBuiltinOrgLatex's own mep.org_latex_scan (Lua has no direct field
    // access), which no-ops (after clearing any stale rows/folds) while
    // this is off -- see Buffer::org_latex_rows' own comment for why, unlike
    // OrgImagesVisible(), this needs a real Lua-visible getter.
    /**
     * @brief Returns whether inline LaTeX/math fragments should be rendered as images.
     * @return True if org LaTeX rendering is toggled on.
     */
    bool OrgLatexVisible() const { return org_latex_visible_; }
    // Active pane/buffer -- what most of the UI (statusline, blinking
    // cursor, Visual highlight) cares about.
    /**
     * @brief Returns the buffer shown by the active pane.
     * @return A const reference to the active buffer.
     */
    const Buffer &CurrentBuffer() const { return Buf(); }
    // The active pane's own buffer id -- CurPane() itself is private
    // (most of the editor reaches it as `this`'s own member, no need for
    // a public accessor), but a couple of lua_env.cpp bindings that
    // aren't Editor members (mep.html_current_origin/mep.html_reload)
    // need the raw id to key into htmldocs_ via GetHtml/ReloadHtmlBuffer.
    /**
     * @brief Returns the buffer id shown by the active pane.
     * @return The active pane's buffer id.
     */
    int CurrentBufferId() const { return CurPane().buffer_id; }
    // Buffer ids currently shown by a pane in the active tab's own split
    // layout, in that tab's leaf-traversal order (CollectLeafBuffers) --
    // what mep.pane_buffers() exposes for a Lua-side feature that wants
    // to "find an already-open terminal pane" (mirrors mep.nvim's
    // nvim_tabpage_list_wins()/nvim_win_get_buf combination).
    /**
     * @brief Lists the buffer ids currently shown by panes in the active tab.
     * @return Buffer ids in the active tab's leaf-traversal order.
     */
    std::vector<int> PaneBuffersInActiveTab() const;
    // Moves focus straight to whichever pane in the active tab already
    // shows buffer_id (false, no-op, if none does) -- unlike
    // NavigatePaneDirection's spatial hjkl, a direct jump by buffer
    // identity. NVIM_PARITY_PLAN.md Phase 27 gap: mep.run_file/
    // mep.repl_start open their output in a new split and focus it once,
    // on creation, but there was no way back to it (or back to the
    // source buffer) afterward short of manual hjkl.
    /**
     * @brief Moves focus to whichever pane in the active tab already shows the given buffer.
     * @param buffer_id The buffer id to search for.
     * @return True if a pane showing that buffer was found and focused, false otherwise.
     */
    bool FocusPaneShowingBuffer(int buffer_id);
    // The cross-workspace, cross-tab version of FocusPaneShowingBuffer:
    // switches to the workspace (and project) owning `buffer_id`, then to
    // the first tab of it with a pane holding that buffer -- visible, or
    // as a hidden buffer tab, which gets activated -- and focuses that
    // pane. Falls back to showing the buffer in the current pane when no
    // pane holds it at all. What the AI-agents sidebar's Enter does
    // (kBuiltinAiTerminal, main.cpp) to land on an agent's terminal.
    /**
     * @brief Switches workspace/tab/pane as needed to focus a pane holding the given buffer, showing it in the current pane as a last resort.
     * @param buffer_id The buffer id to jump to.
     * @return False only if the buffer id is invalid or its workspace can't be activated.
     */
    bool JumpToBuffer(int buffer_id);
    /**
     * @brief Returns the workspace id a buffer belongs to (-1 for unscoped buffers or an invalid id).
     * @param buffer_id The buffer id.
     * @return The owning workspace's stable id, or -1.
     */
    int BufferWorkspaceId(int buffer_id) const {
        if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return -1;
        return buffers_[static_cast<size_t>(buffer_id)].workspace_id;
    }
    // Cursor row (0-indexed) of whichever pane in the active tab shows
    // buffer_id, without changing focus -- -1 if no pane shows it. Lets a
    // Lua consumer that keeps its own buffer id around (e.g.
    // kBuiltinStructure's split-pane source buffer) read that pane's live
    // cursor position for "is the cursor near this item" tracking even
    // while a different pane (or a sidebar) currently has focus, the way
    // mep.cursor()/GetCursorForLua only ever can for the *focused* pane.
    /**
     * @brief Returns the cursor row of whichever pane in the active tab shows the given buffer.
     * @param buffer_id The buffer id to look up.
     * @return The cursor row (0-indexed) of the pane showing that buffer, or -1 if none does.
     */
    int CursorRowForBuffer(int buffer_id) const;
    // Moves focus to whichever pane in the active tab's split layout is
    // topmost, then (among ties) leftmost -- PaneRect's y0 then x0. The
    // file tree's on_click calls this before mep.open so a click always
    // opens near the tree instead of in whatever pane happened to be
    // active before the sidebar took focus (e.g. the bottom pane of a
    // top/bottom split, if that's the one that was last edited).
    /**
     * @brief Moves focus to the topmost, then leftmost, pane in the active tab's split layout.
     */
    void FocusTopLeftPane();
    /**
     * @brief Returns the active pane's cursor position.
     * @return The current cursor row/column.
     */
    CursorPos Cursor() const { return CurPane().cursor; }
    /**
     * @brief Returns the active pane's current scroll row.
     * @return The topmost visible buffer row.
     */
    int ScrollRow() const { return CurPane().scroll_row; }
    /**
     * @brief Returns the in-progress ":" command-line text.
     * @return The current command-line buffer contents.
     */
    const std::string &CommandLine() const { return command_line_; }
    // The in-progress /{...} or ?{...} query, for the command bar to draw
    // while in Mode::SearchForward/SearchBackward (mirrors CommandLine()
    // above for Mode::Command).
    /**
     * @brief Returns the in-progress search query text.
     * @return The current forward/backward search query being typed.
     */
    const std::string &SearchQuery() const { return search_query_; }
    /**
     * @brief Returns the current status-line message.
     * @return The status message text.
     */
    const std::string &StatusMessage() const { return status_message_; }

    // --- Notifications (NVIM_PARITY_PLAN.md Part I Phase 6) ---
    // Single choke point every internal message should eventually funnel
    // through (mirrors mep.nvim hooking vim.notify once): pushes a toast
    // (auto-dismissed after a per-level timeout) and a persistent history
    // entry, and also updates the status line so existing lightweight
    // inline messages ("3 substitutions on 2 lines") keep showing there.
    enum class NotifyLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };
    struct NotifyEntry {
        int id = 0;
        std::string message;
        NotifyLevel level = NotifyLevel::Info;
        double created_at = 0.0;
        double expires_at = 0.0;  // 0 = never auto-dismiss
    };
    /**
     * @brief Pushes a notification: shows a toast, records it in history, and updates the status line.
     * @param msg The message text to show.
     * @param level The severity level, which controls toast duration and styling.
     */
    void Notify(const std::string &msg, NotifyLevel level = NotifyLevel::Info);
    // Called once per frame with the current wall-clock time (main.cpp's
    // GetTime()) to expire timed-out toasts -- editor.h/.cpp stay
    // raylib-free, so "now" is threaded in rather than queried here.
    /**
     * @brief Expires toasts whose timeout has elapsed as of the given time.
     * @param now The current wall-clock time in seconds.
     */
    void PruneExpiredToasts(double now);
    /**
     * @brief Dismisses a single toast notification immediately.
     * @param id The id of the toast to dismiss.
     */
    void DismissToast(int id);
    /**
     * @brief Dismisses every currently visible toast notification.
     */
    void DismissAllToasts();
    /**
     * @brief Clears the persistent notification history.
     */
    void ClearNotifyHistory();
    /**
     * @brief Returns the currently visible toast notifications.
     * @return A const reference to the active toast list.
     */
    const std::vector<NotifyEntry> &Toasts() const { return toasts_; }
    /**
     * @brief Returns the full notification history.
     * @return A const reference to the notification history list.
     */
    const std::vector<NotifyEntry> &NotifyHistory() const { return notify_history_; }
    // :MepNotifyPanel -- a sidebar (Phase 7) view onto NotifyHistory(),
    // rebuilt from it each time this is called.
    /**
     * @brief Opens or closes the :MepNotifyPanel sidebar view of the notification history.
     */
    void ToggleNotifyHistoryPanel();

    /**
     * @brief Returns whether the editor has requested the application quit.
     * @return True if the editor should quit.
     */
    bool ShouldQuit() const { return should_quit_; }
    // Polling-based buffer-change/save detection for Lua (mep.buffer_
    // change_epoch()/buffer_save_epoch()) -- a consumer stores the last
    // value it saw and re-runs its (possibly expensive) refresh only when
    // the value differs, checked once per frame via mep.on_frame rather
    // than a synchronous callback fired from inside the edit/save call
    // stack (which would risk re-entrant buffer mutation).
    /**
     * @brief Returns a counter incremented on every buffer change, for cheap polling-based change detection.
     * @return The current change epoch.
     */
    int ChangeEpoch() const { return change_epoch_; }
    /**
     * @brief Returns a counter incremented on every buffer save, for cheap polling-based save detection.
     * @return The current save epoch.
     */
    int SaveEpoch() const { return save_epoch_; }
    // Wall-clock seconds since program start, threaded in from main.cpp's
    // raylib GetTime() once per frame (editor.h/.cpp stay raylib-free, so
    // "now" is threaded in rather than queried here -- same pattern as
    // PruneExpiredToasts). mep.now() uses this rather than Lua's own
    // os.clock(), which measures CPU time, not wall-clock time -- under a
    // busy/idle-waiting render loop the two can drift apart enough to
    // make a debounce interval fire far later than intended.
    /**
     * @brief Sets the current wall-clock time, threaded in once per frame from main.cpp.
     * @param t The current wall-clock time in seconds.
     */
    void SetNow(double t) { now_ = t; }
    /**
     * @brief Returns the wall-clock time last set via SetNow.
     * @return The current time in seconds.
     */
    double Now() const { return now_; }
    // For the status line: the count typed so far (0 if none), so a
    // half-typed "3d" isn't invisible while it's pending.
    /**
     * @brief Returns the count prefix typed so far for a pending motion/operator.
     * @return The pending count, or 0 if none has been typed.
     */
    int PendingCount() const { return pending_count_; }
    // For the status line: the register named so far via "{a-z} (0 if
    // none), same reasoning as PendingCount above.
    /**
     * @brief Returns the register name typed so far via a pending "{a-z} prefix.
     * @return The pending register character, or 0 if none has been typed.
     */
    char PendingRegister() const { return pending_register_; }

    /**
     * @brief Returns whether a Visual-mode selection (charwise, linewise, or block) is currently active.
     * @return True if any Visual mode is active.
     */
    bool HasVisualSelection() const {
        return mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock;
    }
    /**
     * @brief Returns whether Visual Block mode is currently active.
     * @return True if the current mode is VisualBlock.
     */
    bool IsVisualBlock() const { return mode_ == Mode::VisualBlock; }
    // Returns the selection normalized so `start` <= `end` in buffer order.
    /**
     * @brief Computes the current Visual selection's start/end positions, normalized to buffer order.
     * @param start Output: the earlier of the two endpoints.
     * @param end Output: the later of the two endpoints.
     */
    void VisualRange(CursorPos &start, CursorPos &end) const;
    // Visual Block's rectangular extent: rows [top, bottom], columns
    // [left, right] (right is -1 if the block is in "to end of line on
    // each row" mode -- see block_to_eol_'s comment -- callers should
    // treat that as "the rest of the row", not a literal column).
    /**
     * @brief Computes Visual Block mode's rectangular extent.
     * @param top Output: the top row of the block.
     * @param bottom Output: the bottom row of the block.
     * @param left Output: the left column of the block.
     * @param right Output: the right column of the block, or -1 if the block extends to each row's end.
     */
    void VisualBlockRange(int &top, int &bottom, int &left, int &right) const;
    // Read-only text of the current Visual selection (charwise/linewise
    // joined with '\n' between lines, blockwise one row's slice per line
    // same as a blockwise register's own text) -- "" if none is active.
    // Shares its extraction logic with YankRange, but writes no register
    // and pushes no undo state, so it's safe to call from anywhere purely
    // to *read* the selection (mep.visual_selection(), send-selection
    // features, ...). See also ExtractRangeText (private).
    /**
     * @brief Returns the text currently covered by the active Visual selection.
     * @return The selected text, or an empty string if no selection is active.
     */
    std::string CurrentVisualSelectionText() const;

    // --- Multi-pane/tab read access (for main.cpp's renderer) ---
    /**
     * @brief Returns the number of open tabs.
     * @return The tab count.
     */
    int TabCount() const { return static_cast<int>(Tabs().size()); }
    /**
     * @brief Returns the index of the currently active tab.
     * @return The active tab's index.
     */
    int ActiveTabIndex() const { return ActiveWorkspace().active_tab; }
    /**
     * @brief Returns the split-tree root of the active tab.
     * @return A const pointer to the active tab's root SplitNode.
     */
    const SplitNode *ActiveTabRoot() const { return ActiveTab().root.get(); }
    // Non-const sibling of ActiveTabRoot -- main.cpp's own per-frame pane-
    // geometry capture (mirroring DrawPaneTree's layout math) stashes raw
    // SplitNode* pointers for its border-drag hit-testing (SetPaneBorderShare
    // takes one directly), which needs mutable access; DrawPaneTree/drawing
    // itself stays on the const overload above, unchanged.
    /**
     * @brief Returns a mutable split-tree root of the active tab, for geometry capture that needs raw SplitNode pointers.
     * @return A pointer to the active tab's root SplitNode.
     */
    SplitNode *MutableActiveTabRoot() { return ActiveTab().root.get(); }
    /**
     * @brief Returns the id of the currently focused pane in the active tab.
     * @return The active pane's id.
     */
    int ActivePaneId() const { return ActiveTab().active_pane_id; }
    /**
     * @brief Returns the stable id of the active tab (Tab::id), exposed to Lua as mep.current_tab_id().
     * @return The active tab's id.
     */
    int ActiveTabId() const { return ActiveTab().id; }
    // Same as ActiveTabRoot()/ActivePaneId() but for tab `index`
    // specifically (0 <= index < TabCount(), unchecked -- same contract as
    // every other accessor here) rather than always the active tab --
    // agent_rpc.cpp's state.dump walks every open tab, not just the one
    // currently in view.
    /**
     * @brief Returns the split-tree root of the tab at the given index.
     * @param index The tab index to look up (0 <= index < TabCount(), unchecked).
     * @return A const pointer to that tab's root SplitNode.
     */
    const SplitNode *TabRoot(int index) const { return Tabs()[static_cast<size_t>(index)].root.get(); }
    /**
     * @brief Returns the id of the active pane in the tab at the given index.
     * @param index The tab index to look up (0 <= index < TabCount(), unchecked).
     * @return That tab's active pane id.
     */
    int TabActivePaneId(int index) const { return Tabs()[static_cast<size_t>(index)].active_pane_id; }
    // --- Workspaces: lifecycle (WORKSPACES_PLAN.md Phase 2) ---
    // Creates an in-memory workspace in the active project with one empty
    // tab and returns its id, or -1 (status message set) if `name` is
    // invalid/duplicate. `root` empty = the project root; `branch` is
    // display-only. Does not switch to it.
    int WorkspaceNew(const std::string &name, const std::string &root, const std::string &branch);
    // Activates workspace `id` (in whichever loaded project holds it),
    // chdir()s to its root (decision 4) and bumps WorkspaceChangeEpoch().
    bool WorkspaceSwitch(int id);
    void WorkspaceNext();
    void WorkspacePrevious();
    // Refuses for the primary workspace, and (unless `force`) for one with
    // modified buffers; otherwise kills its terminals, soft-deletes its
    // buffers and activates the nearest neighbour. Phase 6's git worktree
    // removal wraps this (WorkspaceRemove).
    bool WorkspaceDelete(int id, bool force);
    // `:projectclear[!]` / <leader>pc: empties workspace `id` back to the
    // single-tab / single-pane / fresh-empty-buffer shape MakeWorkspace
    // gives a brand-new one -- kills its terminals and soft-deletes its
    // buffers (ReleaseWorkspaceResources) -- so mep.project_default_layout
    // (main.cpp) can rebuild the legacy readme/tree/terminal startup
    // layout on it. Refuses (unless `force`) while it has modified buffers.
    bool WorkspaceReset(int id, bool force);
    bool WorkspaceRename(int id, const std::string &name);
    // User-facing `:wsnew` entry: on a git project spawns `git worktree
    // add` asynchronously and materialises the workspace on success
    // (Phase 6); otherwise WorkspaceNew + WorkspaceSwitch immediately.
    // `attach_existing`: `:wsnew!` -- attach to an existing branch of the
    // same name instead of creating one.
    void WorkspaceCreate(const std::string &name, bool attach_existing);
    // User-facing `:wsdelete[!] [name]` entry: git worktree removal (Phase
    // 6) followed by WorkspaceDelete. Empty `arg` = the active workspace.
    void WorkspaceRemove(const std::string &arg, bool force);
    // "name" or 1-based index string -> workspace id in the active
    // project, -1 if neither matches.
    int ResolveWorkspaceArg(const std::string &arg) const;
    // --- Git worktrees (WORKSPACES_PLAN.md Phase 6; native only) ---
    // Async `git rev-parse --show-toplevel` then `git worktree list
    // --porcelain` for project `project_id`: sets Project::is_git/
    // git_toplevel, the primary workspace's branch, and adopts every
    // worktree under the derived worktree directory as a workspace
    // (decision 7). Called from main() at startup and by ProjectLoad.
    void ProjectDetectGit(int project_id);
    // `:wsadopt <path-or-branch>`: turns a hand-made worktree (one not
    // under the derived directory) into a workspace.
    void WorkspaceAdopt(const std::string &path_or_branch);
    // `:wsprune`: `git worktree prune`, then drops every workspace whose
    // directory has vanished.
    void WorkspacePrune();
    // mep.opt.worktree_dir: where new worktrees go instead of
    // `<parent>/<repo>.worktrees` (see DeriveWorktreeDir).
    void SetWorktreeDirOverride(const std::string &dir) { worktree_dir_override_ = dir; }
    // mep.opt.restore_workspaces / --no-session (Phase 10).
    void SetRestoreWorkspaces(bool on) { restore_workspaces_ = on; }
    bool RestoreWorkspaces() const { return restore_workspaces_ && session_enabled_; }
    // `--no-session`: neither restore nor write the per-project session
    // file (tests and throwaway runs must never touch the real data dir).
    void SetSessionPersistence(bool on) { session_enabled_ = on; }
    bool SessionPersistence() const { return session_enabled_; }
    const std::string &WorktreeDirOverride() const { return worktree_dir_override_; }
    // The project a workspace belongs to, or nullptr.
    Project *ProjectOfWorkspace(int workspace_id);
    const Project *ProjectOfWorkspace(int workspace_id) const;

    // --- Projects (WORKSPACES_PLAN.md Phase 9) ---
    // Loads `root` (canonicalised) as a project and makes it active: an
    // already-loaded root is just switched to. A fresh project gets a
    // primary "main" workspace, Phase 10's saved state (if any and
    // RestoreWorkspaces()), and Phase 6's async git detection. Returns the
    // project id, or -1 if `root` isn't a directory. `restored` (optional)
    // reports whether saved state or an existing load supplied the layout,
    // so mep.project_open can skip its readme/terminal default layout.
    int ProjectLoad(const std::string &root, bool *restored = nullptr);
    bool ProjectSwitch(int id);
    // Refuses for the last loaded project and (unless `force`) when any
    // of its workspaces has unsaved buffers; saves its state first.
    bool ProjectClose(int id, bool force);
    void ProjectNext();
    void ProjectPrevious();
    // "name", 1-based index string, or an absolute root -> project id; -1.
    int ResolveProjectArg(const std::string &arg) const;
    // `mep --project <dir>` / $MEP_PROJECT: re-roots the bootstrap project
    // (before any file is opened) instead of loading a second one.
    bool RerootInitialProject(const std::string &root);

    // --- Persistence (WORKSPACES_PLAN.md Phase 10) ---
    // Writes `MepDataDir()/workspaces/<slug>-<hash>.json` for the project.
    bool SaveWorkspaceState(int project_id);
    void SaveAllWorkspaceState();
    // Best-effort restore (decision 10): missing files are skipped with a
    // one-line summary, vanished worktrees pruned, malformed JSON ignored.
    // `keep_primary_tabs`: leave the primary workspace's current tabs
    // alone (`mep <file>` already put something there).
    bool RestoreWorkspaceState(int project_id, bool keep_primary_tabs);
    // Per-frame: saves 500 ms after the last structural change
    // (workspace/tab/pane/buffer-in-pane changes; cursor moves don't count).
    void TickWorkspacePersistence(double now);
    std::string WorkspaceStateFile(const Project &project) const;
    bool WorkspaceHasModifiedBuffers(int id) const;
    // A relative buffer path resolved against the buffer's own workspace
    // root when that workspace isn't the active one (whose root is the
    // process cwd already); absolute paths and unscoped buffers pass through.
    std::string ResolveBufferPath(const Buffer &buf, const std::string &path) const;
    // Bumped on every switch/create/delete/rename so Lua's
    // mep.on_workspace_changed can poll it (same idiom as
    // BufferChangeEpoch/mep.on_buffer_changed).
    int WorkspaceChangeEpoch() const { return workspace_change_epoch_; }

    // --- Project / workspace read access (WORKSPACES_PLAN.md) ---
    /** @brief Returns the active project (there is always at least one). */
    const Project &ActiveProject() const { return projects_[static_cast<size_t>(active_project_)]; }
    Project &MutableActiveProject() { return projects_[static_cast<size_t>(active_project_)]; }
    /** @brief Returns the active workspace of the active project. */
    const Workspace &ActiveWorkspace() const {
        const Project &p = ActiveProject();
        return p.workspaces[static_cast<size_t>(p.active_workspace)];
    }
    Workspace &MutableActiveWorkspace() {
        Project &p = MutableActiveProject();
        return p.workspaces[static_cast<size_t>(p.active_workspace)];
    }
    /** @brief Workspace with stable id `id` in any loaded project, or nullptr. */
    const Workspace *FindWorkspace(int id) const;
    Workspace *FindWorkspace(int id);
    /** @brief Workspace named `name` in the active project, or nullptr. */
    const Workspace *FindWorkspaceByName(const std::string &name) const;
    int WorkspaceCount() const { return static_cast<int>(ActiveProject().workspaces.size()); }
    int ActiveWorkspaceIndex() const { return ActiveProject().active_workspace; }
    int ProjectCount() const { return static_cast<int>(projects_.size()); }
    int ActiveProjectIndex() const { return active_project_; }
    const Project &ProjectAt(int index) const { return projects_[static_cast<size_t>(index)]; }
    const Project *FindProject(int id) const;
    Project *FindProject(int id);
    /** @brief The active workspace's root directory (decision 4: == process cwd). */
    const std::string &ActiveRoot() const { return ActiveWorkspace().root; }
    // Decision 3: a buffer is visible from the active workspace when it is
    // scoped to it or unscoped (-1: dashboard, scratch). The workspace-
    // scoped readers (:bnext, :ls, the picker, agent buffer.list) filter
    // on this; global ones (:wa, :qa guard) deliberately don't.
    bool BufferInActiveWorkspace(int buffer_id) const {
        if (buffer_id < 0 || buffer_id >= static_cast<int>(buffers_.size())) return false;
        const int ws = buffers_[static_cast<size_t>(buffer_id)].workspace_id;
        return ws == -1 || ws == ActiveWorkspace().id;
    }
    /**
     * @brief Returns the buffer with the given id.
     * @param buffer_id The buffer id to look up.
     * @return A const reference to that buffer.
     */
    const Buffer &GetBuffer(int buffer_id) const { return buffers_[static_cast<size_t>(buffer_id)]; }

    // --- Terminal panes (`:terminal`/`:term`, Part VI Phase 27+) ---
    // main.cpp's DrawPane checks this to render straight from the
    // session's VTerm grid instead of the (unused, empty) Buffer text a
    // terminal pane's buffer_id still nominally points at.
    /**
     * @brief Returns whether the given buffer id is backed by a live terminal session.
     * @param buffer_id The buffer id to check.
     * @return True if the buffer is a terminal pane.
     */
    bool IsTerminalBuffer(int buffer_id) const;
    /**
     * @brief Returns the terminal session for the given buffer id, if any.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the TerminalSession, or nullptr if the buffer isn't a terminal.
     */
    const TerminalSession *GetTerminal(int buffer_id) const;
    // Writes `text` into a real terminal buffer's own PTY (mep.terminal_write,
    // the "send this line to whichever :terminal pane I designated"
    // primitive a Lua-side vim-slime-style feature needs) -- false if
    // `buffer_id` isn't a live terminal (never one, or already exited).
    /**
     * @brief Writes text into a terminal buffer's underlying PTY.
     * @param buffer_id The terminal buffer id to write to.
     * @param text The text to send to the PTY.
     * @return True if the buffer is a live terminal and the write was sent, false otherwise.
     */
    bool WriteToTerminalBuffer(int buffer_id, const std::string &text);
    // Called once per frame by DrawPane with the terminal pane's current
    // character-cell size; no-ops if unchanged since the last call
    // (cheap to call unconditionally rather than threading a "did this
    // pane's pixel size change" flag down from main.cpp).
    /**
     * @brief Resizes a terminal pane's PTY and VTerm grid to match its current character-cell size.
     * @param buffer_id The terminal buffer id to resize.
     * @param rows The new number of terminal rows.
     * @param cols The new number of terminal columns.
     */
    void ResizeTerminal(int buffer_id, int rows, int cols);
    // Called once per frame from main.cpp's main loop, alongside
    // JobManager::Instance().PollAll() -- pumps buffered output for any
    // wasm-backed terminal session (see TerminalSpawn) into its VTerm; a
    // no-op on native builds, which get this for free via JobManager's
    // own callback-driven PollAll instead.
    /**
     * @brief Pumps buffered output for wasm-backed terminal sessions into their VTerm state; a no-op on native builds.
     */
    void PollTerminals();

    // --- Image-viewer panes (opened via LoadFile for a png/jpg/bmp/gif
    // path -- see IsImagePath in image_doc.h) ---
    // main.cpp's DrawPane checks this to render the decoded texture instead
    // of the (unused, empty) Buffer text an image pane's buffer_id still
    // nominally points at.
    /**
     * @brief Returns whether the given buffer id is backed by a decoded image.
     * @param buffer_id The buffer id to check.
     * @return True if the buffer is an image pane.
     */
    bool IsImageBuffer(int buffer_id) const;
    /**
     * @brief Returns the image session for the given buffer id, if any.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the ImageSession, or nullptr if the buffer isn't an image pane.
     */
    const ImageSession *GetImage(int buffer_id) const;
    // Called once per frame by DrawPane with the pane's current content
    // pixel size; also re-clamps pan_x/pan_y in case the pane shrank since
    // the last call (mirrors ResizeTerminal's per-frame-refresh pattern).
    /**
     * @brief Updates an image pane's viewport size and re-clamps its pan offset.
     * @param buffer_id The image buffer id to resize.
     * @param w The new viewport width in pixels.
     * @param h The new viewport height in pixels.
     */
    void ResizeImageViewport(int buffer_id, int w, int h);

    // --- PDF-viewer panes (opened via LoadFile for a .pdf path -- see
    // IsPdfPath in pdf_doc.h). Mirrors the Image-viewer block above; see
    // PdfSession's own comment for why it additionally owns a re-renderable
    // raster buffer instead of a fixed decode. ---
    /**
     * @brief Returns whether the given buffer id is backed by a PDF document.
     * @param buffer_id The buffer id to check.
     * @return True if the buffer is a PDF pane.
     */
    bool IsPdfBuffer(int buffer_id) const;
    /**
     * @brief Returns the PDF session for the given buffer id, if any.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the PdfSession, or nullptr if the buffer isn't a PDF pane.
     */
    const PdfSession *GetPdf(int buffer_id) const;
    // Pure geometry clamp only (mirrors ResizeImageViewport): re-clamps
    // pan_x against the anchor page's on-screen width. Never triggers a
    // re-render itself -- that's EnsurePdfPagesRastered's job, called
    // separately (every frame from DrawPane, after this). Keeping the
    // resize call side-effect-free avoids thrashing re-renders before
    // HandlePdfInput's own zoom-band logic has settled.
    /**
     * @brief Updates a PDF pane's viewport size and re-clamps its horizontal pan, without triggering a re-render.
     * @param buffer_id The PDF buffer id to resize.
     * @param w The new viewport width in pixels.
     * @param h The new viewport height in pixels.
     */
    void ResizePdfViewport(int buffer_id, int w, int h);
    // Renders whichever of {page-1, page, page+1} aren't already cached at
    // the current rendered_scale (clearing the whole cache first if
    // rendered_scale changed since the last call), and evicts everything
    // outside that window. Called once per frame from DrawPane, before
    // reading pdfs_[...].rasters to draw -- cheap on a cache hit (the
    // common case), so safe to call unconditionally every frame rather
    // than only on state-change edges.
    /**
     * @brief Ensures the anchor page and its immediate neighbors are rasterized at the current scale, evicting the rest.
     * @param buffer_id The PDF buffer id to update.
     */
    void EnsurePdfPagesRastered(int buffer_id);

    // --- HTML-preview panes (opened via mep.html_open, kBuiltinTextTools
    // -- deliberately *not* reachable from LoadFile's extension dispatch,
    // see Mode::Html's own comment on why). Mirrors the Image-viewer block
    // above. ---
    /**
     * @brief Returns whether the given buffer id is backed by an HTML preview.
     * @param buffer_id The buffer id to check.
     * @return True if the buffer is an HTML preview pane.
     */
    bool IsHtmlBuffer(int buffer_id) const;
    /**
     * @brief Returns the HTML session for the given buffer id, if any.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the HtmlSession, or nullptr if the buffer isn't an HTML pane.
     */
    const HtmlSession *GetHtml(int buffer_id) const;
    // Every live HTML session's buffer id (main.cpp's media playback sweep).
    std::vector<int> HtmlBufferIds() const;
    /**
     * @brief Updates an HTML preview pane's viewport size.
     * @param buffer_id The HTML buffer id to resize.
     * @param w The new viewport width in pixels.
     * @param h The new viewport height in pixels.
     */
    void ResizeHtmlViewport(int buffer_id, int w, int h);
    // Clamps scroll_y into [0, max_scroll] -- called from DrawPane's html
    // branch *after* that frame's own LayoutHtmlDoc call, since max_scroll
    // (total layout height minus viewport height) depends on real font
    // metrics this raylib-free file has no access to, the same reasoning
    // OfficeSession's own comment gives for why its scroll-follow math
    // lives in main.cpp instead of here.
    /**
     * @brief Clamps an HTML pane's vertical scroll offset to the current layout's valid range.
     * @param buffer_id The HTML buffer id to clamp.
     * @param max_scroll The maximum valid scroll offset (total layout height minus viewport height).
     */
    void ClampHtmlScroll(int buffer_id, float max_scroll);
    // Advances the page's <audio>/<video> clocks by `seconds` (DrawPane calls
    // this once per frame with GetFrameTime()); see AdvanceHtmlMediaClock.
    void AdvanceHtmlMedia(int buffer_id, double seconds);
    // Parses `bytes` (already-read HTML text) and opens it as a new
    // HtmlSession in the *current* pane (mirrors OpenImageInPlace/
    // OpenPdfInPlace exactly: dedup-by-`source` reuses an existing session
    // rather than re-parsing -- see this function's own .cpp comment for
    // why a re-fetched URL needs a fresh `source` string, not the same one
    // reused, to actually pick up new content). `source` is what
    // HtmlSession::source and the pane header end up showing; it's a
    // display/dedup key, not necessarily read from disk itself (the
    // caller already did that, or fetched it via curl into a temp file).
    // `origin` is HtmlSession::origin -- see its own comment.
    /**
     * @brief Parses HTML bytes and opens them as a new HtmlSession in the current pane, deduplicating by source.
     * @param origin The user-facing URL/path this page was opened from.
     * @param source The display/dedup key identifying where the bytes came from.
     * @param bytes Pointer to the raw HTML bytes to parse.
     * @param len Length of `bytes` in bytes.
     */
    void OpenHtmlInPlace(const std::string &origin, const std::string &source, const unsigned char *bytes,
                          size_t len);
    // Re-parses `bytes` INTO the existing HtmlSession at `buffer_id` --
    // unlike OpenHtmlInPlace, never creates a new buffer/session and never
    // does a dedup-by-source lookup; a hard in-place overwrite (fresh DOM,
    // scripts re-run, scroll reset to 0), used for both "reload this page"
    // (mep.browse_reload, kBuiltinTextTools -- same origin/source, fresh
    // bytes) and "navigate this pane to a different address" (the address
    // bar, mep.browse_open_bar -- a new origin/source entirely). A no-op
    // if `buffer_id` isn't a live HTML session.
    /**
     * @brief Re-parses HTML bytes into an existing HtmlSession in place, overwriting its DOM and resetting scroll.
     * @param buffer_id The buffer id of the existing HTML session to overwrite.
     * @param origin The user-facing URL/path this page was (re)opened from.
     * @param source The display/dedup key identifying where the bytes came from.
     * @param bytes Pointer to the raw HTML bytes to parse.
     * @param len Length of `bytes` in bytes.
     */
    void ReloadHtmlBuffer(int buffer_id, const std::string &origin, const std::string &source,
                           const unsigned char *bytes, size_t len);
    // Re-decodes `bytes` INTO the existing PdfSession at `buffer_id` --
    // unlike OpenPdfInPlace, never creates a new buffer/session and never
    // does a dedup-by-filename lookup; a hard in-place overwrite (fresh
    // PdfDoc, raster/search-match caches cleared, page/scroll/pan reset to
    // the top). Filename never changes across a reload (unlike HTML's
    // origin/source), so unlike ReloadHtmlBuffer this takes no path
    // argument. Used by the run button (kBuiltinRunButton's org branch,
    // main.cpp) to refresh an already-open PDF preview after recompiling
    // the same output path -- mep.open's own dedup-by-filename lookup in
    // OpenPdfInPlace would otherwise just find and reuse the stale session
    // instead of re-reading the freshly recompiled bytes. A no-op if
    // `buffer_id` isn't a live PDF session, or if `bytes` fails to parse
    // (the stale session is left alone rather than being torn down).
    /**
     * @brief Re-decodes PDF bytes into an existing PdfSession in place, resetting page/scroll/search state.
     * @param buffer_id The buffer id of the existing PDF session to overwrite.
     * @param bytes Pointer to the raw PDF bytes to parse.
     * @param len Length of `bytes` in bytes.
     */
    void ReloadPdfBuffer(int buffer_id, const unsigned char *bytes, size_t len);
    // Re-decodes `bytes` INTO the existing OfficeSession at `buffer_id` --
    // unlike OpenOfficeInPlace, never creates a new buffer/session and
    // never does a dedup-by-filename lookup; a hard in-place overwrite
    // (fresh OfficeDoc + original_bytes, cursor/selection/scroll/undo
    // reset). Same "refresh an already-open preview after recompiling the
    // same output path" role ReloadPdfBuffer/ReloadHtmlBuffer play for
    // their own formats (kBuiltinRunButton's org branch, main.cpp). `path`
    // is only needed to tell docx from odt (IsDocxPath/IsOdtPath) --
    // unlike ReloadHtmlBuffer's origin/source it isn't stored anywhere, so
    // callers just pass the same path the session was already opened
    // from. A no-op if `buffer_id` isn't a live office session, or if
    // `bytes` fails to parse (the stale session is left alone rather than
    // being torn down).
    /**
     * @brief Re-decodes office-document bytes into an existing OfficeSession in place, resetting cursor/scroll/undo state.
     * @param buffer_id The buffer id of the existing office session to overwrite.
     * @param path The file path (used only to distinguish docx from odt).
     * @param bytes Pointer to the raw document bytes to parse.
     * @param len Length of `bytes` in bytes.
     */
    void ReloadOfficeBuffer(int buffer_id, const std::string &path, const unsigned char *bytes, size_t len);
    // Converts `buffer_id` in place between the rendered HTML view and a
    // plain-text view of the same underlying file, keeping the same
    // buffer_id both ways (so :w/undo/the pane's tab all keep working
    // across the toggle) -- the pair behind the Ctrl-E/Ctrl-V view-toggle
    // (HandleHtmlInput/HandleNormalInput) and LoadFile's force_text
    // param. ConvertHtmlBufferToText re-reads the file from disk (an
    // HtmlSession only retains the parsed DOM, never the original source
    // text, so there's nothing "live" to convert from) and is a no-op if
    // buffer_id isn't currently an HTML buffer. ConvertTextBufferToHtml
    // parses the buffer's *current* in-memory lines instead of the disk
    // copy, so unsaved edits show up in the rendered view, and is a
    // no-op if buffer_id is already an HTML buffer. Both are pure data
    // conversions -- callers handle CurPane()/mode_/status_message_.
    /**
     * @brief Converts an HTML buffer to a plain-text view by re-reading the file from disk; no-op if not HTML.
     * @param buffer_id The buffer id to convert.
     */
    void ConvertHtmlBufferToText(int buffer_id);
    /**
     * @brief Converts a plain-text buffer to a rendered HTML view by parsing its current in-memory lines.
     * @param buffer_id The buffer id to convert.
     */
    void ConvertTextBufferToHtml(int buffer_id);

    // --- WYSIWYG office-document panes (opened via LoadFile for a
    // .docx/.odt path -- see IsDocxPath/IsOdtPath in office_doc.h).
    // Mirrors the Image/PDF-viewer blocks above. ---
    /**
     * @brief Returns whether the given buffer is an open WYSIWYG office-document (.docx/.odt) session.
     * @param buffer_id The buffer id to check.
     * @return True if the buffer has an active OfficeSession.
     */
    bool IsOfficeBuffer(int buffer_id) const;
    /**
     * @brief Returns the OfficeSession for a buffer, if it is an office-document pane.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the OfficeSession, or nullptr if none exists.
     */
    const OfficeSession *GetOffice(int buffer_id) const;
    // Pure geometry setter (mirrors ResizePdfViewport): records
    // viewport_w/h only, no scroll-follow logic -- unlike the plain text
    // buffer, deciding whether cursor_para is currently visible needs
    // word-wrap info (how many *visual* lines each paragraph occupies at
    // the pane's width), which requires the loaded fonts/MeasureTextEx
    // this raylib-free file deliberately doesn't have access to. That
    // computation lives in main.cpp's DrawPane (which owns the fonts),
    // computed on-demand each frame scoped to whatever's actually visible
    // -- not cached here (see OfficeSession's own comment: only
    // ~viewport-height's worth of paragraphs are ever wrapped in a given
    // frame, not the whole document) -- which then calls SetOfficeScroll
    // below if the cursor has scrolled out of view.
    /**
     * @brief Records an office pane's viewport size only; no scroll-follow logic (that needs word-wrap info main.cpp computes).
     * @param buffer_id The office buffer id whose viewport changed.
     * @param w The new viewport width in pixels.
     * @param h The new viewport height in pixels.
     */
    void ResizeOfficeViewport(int buffer_id, int w, int h);
    // Toolbar click handler (main.cpp's office DrawPane branch,
    // Phase 5) -- the same bold/italic/underline toggle
    // HandleOfficeVisualInput's b/i/u keys perform, reachable from a
    // mouse click instead. With an active Visual selection, toggles over
    // it and drops back to OfficeNormal (matching the keybinding path
    // exactly); with no selection (clicked from OfficeNormal), toggles
    // just the single character at the cursor as a small but well-defined
    // fallback rather than a no-op -- no "sticky" insert-mode formatting
    // either way, consistent with the v1 exclusion.
    /**
     * @brief Toggles bold/italic/underline formatting over the active selection, or a single char at the cursor.
     * @param which The format to toggle ('b', 'i', or 'u').
     */
    void ToggleOfficeFormat(char which);
    // True if `which` (b/i/u) is "on" at the cursor (OfficeNormal) or
    // uniformly on across the current selection (OfficeVisual) -- drives
    // the toolbar button's pressed-look. Read-only, mirrors
    // ToggleFormatOverRange's own all-on check but without mutating
    // anything.
    /**
     * @brief Returns whether a format is "on" at the cursor, or uniformly on across the current selection.
     * @param which The format to check ('b', 'i', or 'u').
     * @return True if that format is currently active.
     */
    bool OfficeFormatActive(char which) const;
    // Non-boolean-field counterparts of ToggleOfficeFormat/OfficeFormatActive
    // -- same Visual-selection-vs-single-char-at-cursor dispatch, but a
    // toolbar dropdown/color swatch already knows the exact value to set
    // (no "detect uniform on/off, then flip" step), so these always set
    // rather than toggle. SetOfficeFontFamily/SetOfficeFontSizePt/
    // SetOfficeColor/SetOfficeHighlight go through SetFormatFieldOverRange;
    // ClearOfficeColor/ClearOfficeHighlight turn has_color/has_highlight
    // back off. ToggleOfficeSuperscript/ToggleOfficeSubscript are still a
    // genuine toggle (mirrors ToggleOfficeFormat's own on/off convention)
    // but need their own function since setting one must also clear the
    // other (mutually exclusive, unlike bold/italic/underline/strike).
    /**
     * @brief Sets the font family over the active selection, or at the cursor if no selection.
     * @param family The font family to apply.
     */
    void SetOfficeFontFamily(OfficeFontFamily family);
    /**
     * @brief Sets the font size (in points) over the active selection, or at the cursor if no selection.
     * @param pt The font size in points.
     */
    void SetOfficeFontSizePt(float pt);
    /**
     * @brief Sets the text color over the active selection, or at the cursor if no selection.
     * @param r Red channel (0-255).
     * @param g Green channel (0-255).
     * @param b Blue channel (0-255).
     */
    void SetOfficeColor(unsigned char r, unsigned char g, unsigned char b);
    /**
     * @brief Clears an explicit text color, reverting to the default, over the active selection or at the cursor.
     */
    void ClearOfficeColor();
    /**
     * @brief Sets the highlight (background) color over the active selection, or at the cursor if no selection.
     * @param r Red channel (0-255).
     * @param g Green channel (0-255).
     * @param b Blue channel (0-255).
     */
    void SetOfficeHighlight(unsigned char r, unsigned char g, unsigned char b);
    /**
     * @brief Clears an explicit highlight color over the active selection, or at the cursor if no selection.
     */
    void ClearOfficeHighlight();
    /**
     * @brief Toggles superscript over the active selection, or at the cursor; clears subscript if it was on.
     */
    void ToggleOfficeSuperscript();
    /**
     * @brief Toggles subscript over the active selection, or at the cursor; clears superscript if it was on.
     */
    void ToggleOfficeSubscript();
    // Paragraph-level (not span/char-range) setters: alignment and list
    // membership apply to every paragraph the Visual selection touches (or
    // just the cursor's own paragraph outside Visual mode), unlike the
    // span-level setters above. SetOfficeListKind toggles off (back to
    // ListKind::None) when the target paragraph(s) already uniformly have
    // `kind` -- lets one button/key double as both "make this a bulleted
    // list" and "un-bullet it", matching ToggleOfficeFormat's own
    // click-again-to-undo convention.
    /**
     * @brief Sets the paragraph alignment for every paragraph the Visual selection touches, or just the cursor's paragraph.
     * @param align The alignment to apply.
     */
    void SetOfficeAlignment(DocParagraph::Align align);
    /**
     * @brief Returns whether an alignment is uniformly active across the affected paragraph(s).
     * @param align The alignment to check.
     * @return True if that alignment is uniformly set.
     */
    bool OfficeAlignmentActive(DocParagraph::Align align) const;
    /**
     * @brief Sets the list kind for the affected paragraph(s), toggling back to None if already uniformly `kind`.
     * @param kind The list kind to apply (or remove, if already uniformly active).
     */
    void SetOfficeListKind(DocParagraph::ListKind kind);
    /**
     * @brief Returns whether a list kind is uniformly active across the affected paragraph(s).
     * @param kind The list kind to check.
     * @return True if that list kind is uniformly set.
     */
    bool OfficeListKindActive(DocParagraph::ListKind kind) const;
    // Inserts `utf8` (a special character, or any literal text) at the
    // cursor -- the toolbar's special-character-grid click handler; not
    // format-related at all, just ApplyInsertToParagraph plus cursor
    // advance, same as a typed character in Mode::OfficeInsert.
    /**
     * @brief Inserts literal text (e.g. a special character from the toolbar grid) at the cursor.
     * @param utf8 The UTF-8 text to insert.
     */
    void InsertOfficeText(const std::string &utf8);
    // Opens a native prompt (BeginPromptNative) for a LaTeX-subset math
    // expression, then inserts it as a new span with DocFormat::math set
    // (main.cpp's office DrawPane branch renders a math=true run via
    // LayoutMathExpression/DrawMathLayout instead of literal glyphs).
    /**
     * @brief Prompts for a LaTeX-subset math expression and inserts it as a math-formatted span at the cursor.
     */
    void InsertOfficeMath();
    // Opens two chained native prompts (rows, then cols), then inserts a
    // DocTable anchored to the paragraph at the cursor (OfficeDoc::tables
    // grows by one; the cursor paragraph's table_ref points at it). Cells
    // start empty.
    /**
     * @brief Prompts for a row and column count, then inserts an empty table anchored at the cursor's paragraph.
     */
    void InsertOfficeTablePrompt();
    // Opens a native prompt for a local image file path, decodes it
    // (ImageDoc::LoadFromMemory) to confirm it's a real image and capture
    // its pixel dimensions, then inserts a DocImage anchored the same way
    // InsertOfficeTablePrompt anchors a DocTable. Silently no-ops (a
    // status message, not a crash) if the path doesn't decode.
    /**
     * @brief Prompts for a local image file path, decodes it, and inserts it as an image anchored at the cursor.
     */
    void InsertOfficeImagePrompt();
    // Table-cell navigation/editing while OfficeSession::in_table_edit is
    // set (see that field's own comment) -- Tab/Shift-Tab or hjkl move
    // between cells, i/a edit the current cell's plain text, Escape exits
    // back to normal paragraph navigation. Called from
    // HandleOfficeNormalInput/HandleOfficeInsertInput once a table anchor
    // is the cursor paragraph's own table_ref and the user has entered it.
    /**
     * @brief Enters table-cell navigation/editing mode for the table anchored at the cursor's paragraph.
     * @param table_ref The table's index into OfficeDoc::tables.
     */
    void EnterOfficeTable(int table_ref);
    /**
     * @brief Exits table-cell editing, returning to normal paragraph navigation.
     */
    void ExitOfficeTable();
    /**
     * @brief Moves the current table-edit cell selection by the given row/column offset.
     * @param dr The row offset to move by.
     * @param dc The column offset to move by.
     */
    void MoveOfficeTableCell(int dr, int dc);
    // Sets scroll_para/scroll_line_in_para directly -- the one mutation
    // point main.cpp's word-wrap-aware "is the cursor still visible"
    // check (DrawPane) uses to scroll-follow the cursor, since that
    // check's own logic must live in main.cpp (see ResizeOfficeViewport's
    // comment) but the state it adjusts lives here.
    /**
     * @brief Sets an office pane's scroll position directly (the mutation point main.cpp's cursor-visibility check drives).
     * @param buffer_id The office buffer id to scroll.
     * @param scroll_para The paragraph index to scroll to.
     * @param scroll_line_in_para The visual line offset within that paragraph.
     */
    void SetOfficeScroll(int buffer_id, int scroll_para, int scroll_line_in_para);
    // Sets OfficeSession::scroll_follow_last_cursor_para directly -- same
    // reasoning/pairing as SetOfficeScroll just above (the value the scan
    // reacts to lives in main.cpp, the state itself lives here).
    /**
     * @brief Sets the last cursor paragraph the scroll-follow logic reacted to.
     * @param buffer_id The office buffer id to update.
     * @param cursor_para The paragraph index the cursor is currently in.
     */
    void SetOfficeScrollFollowCursorPara(int buffer_id, int cursor_para);
    // Jumps the cursor to the start of `para` (column 0, selection
    // cleared) -- the Outline panel's click-to-jump (main.cpp's
    // DrawOfficeSidePanels) needs this alongside SetOfficeScroll itself,
    // see its own comment (editor.cpp) for why moving only the scroll
    // position isn't enough.
    /**
     * @brief Jumps the cursor to the start of a paragraph, clearing any selection (used by the Outline panel's click-to-jump).
     * @param buffer_id The office buffer id to update.
     * @param para The paragraph index to jump the cursor to.
     */
    void SetOfficeCursorPara(int buffer_id, int para);
    // Sets OfficeSession::cursor_wrap_lines(_para) directly -- same
    // main.cpp-computes/editor.cpp-stores pairing as SetOfficeScroll just
    // above, feeding MoveOfficeCursorVisualLine (below) the word-wrap
    // geometry it can't compute itself. Called once per frame from
    // DrawPane, right where it already word-wraps the cursor's own
    // paragraph for rendering (main.cpp).
    /**
     * @brief Stores the current cursor paragraph's word-wrap geometry, feeding MoveOfficeCursorVisualLine.
     * @param buffer_id The office buffer id to update.
     * @param cursor_para The paragraph index the wrap geometry belongs to.
     * @param lines The wrapped line ranges (start, end) within that paragraph.
     */
    void SetOfficeCursorWrapLines(int buffer_id, int cursor_para, std::vector<std::pair<int, int>> lines);
    // Toolbar zoom +/- buttons (and now Ctrl-scroll, HandleMouseWheel's
    // Office branch): multiplies OfficeSession::zoom by `factor` (>1 to
    // grow, <1 to shrink), clamped to [0.5, 3.0] -- a plain direct
    // multiplier, not the "settle-band folding into base_font_pt" pattern
    // OfficeSession::zoom's own comment describes PdfSession using. No
    // pan to re-center (unlike ApplyImageZoom/ApplyPdfZoom): an office
    // pane's position is cursor_para/cursor_col-derived, not a separate
    // pan_x/pan_y, so zooming doesn't need to touch it.
    /**
     * @brief Multiplies the office pane's zoom level, clamped to [0.5, 3.0].
     * @param factor The multiplier to apply (greater than 1 to grow, less than 1 to shrink).
     */
    void SetOfficeZoom(float factor);
    // 'u'/Ctrl-R in Mode::OfficeNormal (and now the toolbar's Undo/Redo
    // buttons, main.cpp) -- mirror Undo()/Redo()'s own push-the-opposite-
    // stack-then-swap shape, operating on OfficeSession::undo_stack/
    // redo_stack instead of Buffer's. Moved up to this file's public
    // section (was private) since a toolbar button click, unlike a
    // keybinding, calls it from outside the class.
    /**
     * @brief Undoes the last office-document edit, popping OfficeSession::undo_stack and pushing to redo_stack.
     */
    void UndoOffice();
    /**
     * @brief Redoes the last undone office-document edit, popping OfficeSession::redo_stack and pushing to undo_stack.
     */
    void RedoOffice();

    // --- Spreadsheet panes (opened via LoadFile for a .xlsx/.ods/.csv
    // path -- see IsXlsxPath/IsOdsPath/IsCsvPath in sheet_doc.h). Mirrors
    // the Office-pane block above. ---
    /**
     * @brief Returns whether the given buffer is an open spreadsheet (.xlsx/.ods/.csv) session.
     * @param buffer_id The buffer id to check.
     * @return True if the buffer has an active SheetSession.
     */
    bool IsSheetBuffer(int buffer_id) const;
    /**
     * @brief Returns the SheetSession for a buffer, if it is a spreadsheet pane.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the SheetSession, or nullptr if none exists.
     */
    const SheetSession *GetSheet(int buffer_id) const;
    // Unlike ResizeOfficeViewport, this DOES do the full scroll-follow
    // job itself (see SheetSession::scroll_row's own comment for why --
    // fixed-size grid cells need no font measurement) -- records
    // viewport_w/h and clamps scroll_row/scroll_col so the cursor cell
    // stays visible, no main.cpp-side follow-up call needed.
    /**
     * @brief Records a sheet pane's viewport size and clamps scroll_row/scroll_col so the cursor cell stays visible.
     * @param buffer_id The spreadsheet buffer id whose viewport changed.
     * @param w The new viewport width in pixels.
     * @param h The new viewport height in pixels.
     */
    void ResizeSheetViewport(int buffer_id, int w, int h);
    // Non-const on purpose, called directly from main.cpp's DrawPane
    // (which only ever sees a `const SheetSession*` via GetSheet) --
    // mirrors EnsurePdfPagesRastered's own shape: on-demand mutation of
    // cached state (here, EvaluateCell's memoized CellValue) driven by
    // what's actually being rendered, not a side effect a const accessor
    // could hide. Returns an Empty CellValue if buffer_id/row/col don't
    // resolve to a real cell.
    /**
     * @brief Evaluates (and memoizes) a spreadsheet cell's value, mutating the cached formula state on demand.
     * @param buffer_id The spreadsheet buffer id.
     * @param row The cell's row index.
     * @param col The cell's column index.
     * @return The cell's evaluated value, or an Empty CellValue if the coordinates don't resolve to a real cell.
     */
    CellValue EvaluateSheetCell(int buffer_id, int row, int col);

    // --- Kanban board / Gantt chart panes (opened via ":Kanban"/":Gantt"
    // on a .org buffer -- see IsOrgBuffer). Unlike every doc-type block
    // above, these do NOT store an alternate binary model that blocks the
    // ordinary text-save path -- Buf().lines stays the buffer's real,
    // saved org text throughout (see org_doc.h's top comment); ":w" needs
    // no Kanban/Gantt-specific code at all, and undo/redo is the ordinary
    // PushUndo()/Undo()/Redo() every ReplaceLinesForLua-based mutation
    // below already goes through -- no separate snapshot stack like
    // PushUndoSheet/PushUndoOffice. ---
    /**
     * @brief Returns whether the given buffer currently shows its org content as a Kanban board.
     * @param buffer_id The buffer id to check.
     * @return True if org_view_mode_ for this buffer is Kanban.
     */
    bool IsKanbanViewActive(int buffer_id) const;
    /**
     * @brief Returns whether the given buffer currently shows its org content as a Gantt chart.
     * @param buffer_id The buffer id to check.
     * @return True if org_view_mode_ for this buffer is Gantt.
     */
    bool IsGanttViewActive(int buffer_id) const;
    /**
     * @brief Returns the KanbanSession for a buffer, if a Kanban view is active for it.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the KanbanSession, or nullptr if none exists.
     */
    const KanbanSession *GetKanban(int buffer_id) const;
    // Non-const on purpose -- main.cpp's UpdateKanbanMouseInteraction
    // writes drag/focus/drop-target UI state directly (mirrors
    // EvaluateSheetCell's own non-const-for-main.cpp-driven-mutation
    // shape), reserving the Kanban*/Gantt* methods below for the actual
    // org-text-mutating actions.
    /**
     * @brief Returns a mutable KanbanSession for a buffer, for main.cpp to write drag/focus/drop-target UI state.
     * @param buffer_id The buffer id to look up.
     * @return A pointer to the KanbanSession, or nullptr if none exists.
     */
    KanbanSession *GetKanbanMutable(int buffer_id);
    /**
     * @brief Returns the GanttSession for a buffer, if a Gantt view is active for it.
     * @param buffer_id The buffer id to look up.
     * @return A const pointer to the GanttSession, or nullptr if none exists.
     */
    const GanttSession *GetGantt(int buffer_id) const;
    /**
     * @brief Returns a mutable GanttSession for a buffer, for main.cpp to write UI state.
     * @param buffer_id The buffer id to look up.
     * @return A pointer to the GanttSession, or nullptr if none exists.
     */
    GanttSession *GetGanttMutable(int buffer_id);

    // Column keyword list a Kanban board renders/navigates -- the parsed
    // outline's todo_keywords then done_keywords concatenated, in that
    // order. A plain read helper over KanbanSession::outline, so main.cpp's
    // DrawKanban/UpdateKanbanMouseInteraction and
    // HandleKanbanNormalInput/HandleKanbanInsertInput don't each duplicate
    // the concatenation.
    /**
     * @brief Returns the ordered list of Kanban column keywords (todo_keywords then done_keywords).
     * @param buffer_id The Kanban buffer id.
     * @return The column keyword names, in board order.
     */
    std::vector<std::string> KanbanColumns(int buffer_id) const;
    // Document-order indices into KanbanSession::outline.headlines whose
    // todo_keyword matches KanbanColumns(buffer_id)[column_index] -- i.e.
    // "which cards are in this column, top to bottom".
    /**
     * @brief Returns document-order indices of headlines whose todo_keyword matches the given column.
     * @param buffer_id The Kanban buffer id.
     * @param column_index Index into KanbanColumns(buffer_id) naming the column to list.
     * @return Indices into KanbanSession::outline.headlines for the cards in that column, top to bottom.
     */
    std::vector<int> KanbanCardsInColumn(int buffer_id, int column_index) const;
    // Document-order indices into GanttSession::outline.headlines that
    // have a SCHEDULED date (see org_doc.h/the plan's documented v1 gap:
    // a headline with no SCHEDULED at all isn't shown in the Gantt view).
    /**
     * @brief Returns document-order indices of headlines that have a SCHEDULED date, for Gantt rendering.
     * @param buffer_id The Gantt buffer id.
     * @return Indices into GanttSession::outline.headlines with a SCHEDULED date.
     */
    std::vector<int> GanttRows(int buffer_id) const;

    // ":Kanban"/":Gantt" ex-command bodies: parses (or re-parses, if a
    // session already exists for this buffer) Buf().lines into a fresh
    // OrgOutline and switches org_view_mode_ for CurrentBufferId(). A
    // status-message no-op if the current buffer isn't IsOrgBuffer().
    /**
     * @brief ":Kanban" ex-command body: (re-)parses the current buffer as an org outline and switches it to Kanban view.
     */
    void OpenKanbanView();
    /**
     * @brief ":Gantt" ex-command body: (re-)parses the current buffer as an org outline and switches it to Gantt view.
     */
    void OpenGanttView();
    // ":Org"/":Text" ex-command body -- sets org_view_mode_ back to Text
    // for CurrentBufferId(); the cached KanbanSession/GanttSession (if any)
    // is left in place so toggling back in doesn't lose scroll/zoom state.
    /**
     * @brief ":Org"/":Text" ex-command body: switches the current buffer back to plain Text view.
     */
    void CloseOrgView();

    // Every method below performs exactly one PushUndo() + Buf().lines
    // splice (via ReplaceLinesForLua's own erase/insert+PushUndo shape, or
    // a direct equivalent for a same-buffer line *move*), then re-parses
    // `outline` from the fresh lines -- see org_doc.h's top comment for
    // why the parsed outline is always rebuilt, never incrementally
    // patched. All operate on CurPane().buffer_id via Buf(), same
    // convention as PushUndoSheet/ReplaceLinesForLua -- callers (main.cpp)
    // must ensure the relevant pane is focused first.
    /**
     * @brief Moves a Kanban card to a different column by rewriting its headline's todo_keyword.
     * @param headline_index Document-order index of the headline (card) to move.
     * @param new_keyword The target column's todo/done keyword.
     */
    void KanbanSetCardColumn(int headline_index, const std::string &new_keyword);
    // Moves headline_index's whole subtree to sit immediately before
    // before_headline_index's subtree (or to the end of the buffer if
    // before_headline_index < 0). A no-op if either index is out of range
    // or before_headline_index falls *within* headline_index's own
    // subtree (would corrupt the move).
    /**
     * @brief Moves a card's whole headline subtree to sit immediately before another card's subtree.
     * @param headline_index Document-order index of the headline (card) to move.
     * @param before_headline_index Index of the headline to insert before, or negative to move to buffer end.
     */
    void KanbanMoveCardBefore(int headline_index, int before_headline_index);
    /**
     * @brief Renames a Kanban card by rewriting its headline's title text.
     * @param headline_index Document-order index of the headline (card) to rename.
     * @param new_title The new title text.
     */
    void KanbanRenameCard(int headline_index, const std::string &new_title);
    // Appends "* <column_keyword> <title>" as a new top-level headline at
    // the end of the buffer (v1 gap: no attempt to infer nesting
    // placement -- see the plan's documented scope boundaries). Returns the
    // new headline's index into KanbanSession::outline.headlines (always
    // the last entry, since it's appended at end-of-file) so a caller (the
    // "+ New Card" drag-drop handler) can immediately open it for rename.
    /**
     * @brief Appends a new top-level headline (card) at the end of the buffer in the given column.
     * @param column_keyword The todo/done keyword naming the column to place the new card in.
     * @param title The new card's title text.
     * @return The new headline's index into KanbanSession::outline.headlines.
     */
    int KanbanNewCard(const std::string &column_keyword, const std::string &title);
    /**
     * @brief Deletes a Kanban card by removing its headline (and subtree) from the buffer.
     * @param headline_index Document-order index of the headline (card) to delete.
     */
    void KanbanDeleteCard(int headline_index);
    // Enters Mode::KanbanInsert targeting a just-created card with an empty
    // edit buffer -- same end state as HandleKanbanNormalInput's 'i' path,
    // but callable from main.cpp's drag-and-drop-create flow (which must
    // enter insert mode itself right on drop, not wait for a key event).
    /**
     * @brief Enters Mode::KanbanInsert with an empty edit buffer targeting a just-created card.
     * @param headline_index Document-order index of the new headline (card) to rename.
     */
    void KanbanBeginRenameNewCard(int headline_index);

    // Column management: all three rewrite the file's single "#+TODO:"
    // line (inserting one, defaulting to {"TODO"}/{"DONE"}, if the file
    // has none yet -- EnsureOrgTodoLine below).
    //
    // No-op + status_message_ if `name` already names a column. Otherwise
    // appends to the TODO side (a new mid-pipeline stage, before any DONE
    // keywords) and returns its column index (KanbanColumns' concatenated
    // ordering).
    /**
     * @brief Adds a new Kanban column (mid-pipeline TODO stage), rewriting the file's "#+TODO:" line.
     * @param name The new column's keyword name.
     * @return The new column's index in KanbanColumns' concatenated ordering; unchanged behavior (no-op) if `name` already exists.
     */
    int KanbanAddColumn(const std::string &name);
    // column_index into KanbanColumns' concatenated ordering. Rewrites
    // every headline currently in this column (todo_keyword == the old
    // name) to the new keyword, then the "#+TODO:" line itself -- both
    // must happen together or a headline's keyword token would no longer
    // match anything ParseOrgOutline recognizes.
    /**
     * @brief Renames a Kanban column, rewriting every card currently in it plus the file's "#+TODO:" line.
     * @param column_index Index into KanbanColumns' concatenated ordering naming the column to rename.
     * @param new_name The column's new keyword name.
     */
    void KanbanRenameColumn(int column_index, const std::string &new_name);
    // Moves a column to the gap `before_column` in KanbanColumns' current
    // ordering (0 = before the first column; size() = after the last).
    // The TODO/DONE split stays at its existing position in that ordering,
    // so moving a column across it also changes whether Org treats it as a
    // done keyword.
    /**
     * @brief Moves a Kanban column to a new position in the column ordering, possibly crossing the TODO/DONE split.
     * @param column_index Index of the column to move.
     * @param before_column Target gap to move it to (0 = before the first column; size() = after the last).
     */
    void KanbanMoveColumn(int column_index, int before_column);
    // No-op + status_message_ if the column still has cards (deliberately
    // not auto-migrated or destructive -- see the plan) or if deleting it
    // would leave zero columns.
    /**
     * @brief Deletes a Kanban column, unless it still has cards or deleting it would leave zero columns.
     * @param column_index Index of the column to delete.
     */
    void KanbanDeleteColumn(int column_index);

    // Shifts both SCHEDULED and DEADLINE (whichever are present) by
    // delta_days -- a drag on the bar's body, preserving its duration.
    /**
     * @brief Shifts a Gantt headline's SCHEDULED and DEADLINE dates (whichever are present) by the same offset, preserving duration.
     * @param headline_index Document-order index of the headline to shift.
     * @param delta_days Number of days to shift by (may be negative).
     */
    void GanttShiftHeadline(int headline_index, int delta_days);
    // Resizes one edge (is_deadline=false moves SCHEDULED, true moves
    // DEADLINE) to land on new_day (an OrgDayNumber-style day-ordinal) --
    // a drag on the bar's left/right edge, or (is_deadline=true on a
    // headline with no DEADLINE yet) dragging a milestone's virtual right
    // edge to create one from scratch. A no-op for a headline with no
    // SCHEDULED at all (nothing to anchor a resize against) or (when
    // is_deadline is false) if new_day would land at or after the
    // existing DEADLINE.
    /**
     * @brief Resizes one edge of a Gantt bar (SCHEDULED or DEADLINE) to a new day, e.g. via edge-drag.
     * @param headline_index Document-order index of the headline to resize.
     * @param is_deadline False to move SCHEDULED, true to move (or create) DEADLINE.
     * @param new_day The target day, as an OrgDayNumber-style day-ordinal.
     */
    void GanttSetHeadlineDate(int headline_index, bool is_deadline, long long new_day);
    // Rewrites/creates a :PROGRESS: property in the headline's immediate
    // property drawer. Used by GanttNormal's `p` prompt.
    /**
     * @brief Sets a Gantt headline's progress percentage by rewriting its :PROGRESS: property.
     * @param headline_index Document-order index of the headline to update.
     * @param progress The progress percentage to store.
     */
    void GanttSetHeadlineProgress(int headline_index, int progress);
    // Enters Mode::GanttInsert to rename headline_index's title inline --
    // double-click on its label (or 'i') in the Gantt view.
    /**
     * @brief Enters Mode::GanttInsert to rename a headline's title inline.
     * @param headline_index Document-order index of the headline to rename.
     */
    void GanttBeginRename(int headline_index);
    /**
     * @brief Renames a Gantt headline by rewriting its title text.
     * @param headline_index Document-order index of the headline to rename.
     * @param new_title The new title text.
     */
    void GanttRenameHeadline(int headline_index, const std::string &new_title);

    // --- Lua-facing API (called from lua_env.cpp bindings) ---
    /**
     * @brief Returns the text of one buffer line, for Lua's mep.get_line().
     * @param row The 0-indexed line number.
     * @return The line's text, or an empty string if out of range.
     */
    std::string GetLineForLua(int row) const;  // 0-indexed
    /**
     * @brief Replaces the text of one buffer line, for Lua's mep.set_line().
     * @param row The 0-indexed line number.
     * @param text The new text for that line.
     */
    void SetLineForLua(int row, const std::string &text);
    /**
     * @brief Returns the current buffer's line count, for Lua's mep.line_count().
     * @return The number of lines in the current buffer.
     */
    int LineCountForLua() const;
    // One TODO/FIXME/HACK/NOTE occurrence (kBuiltinTodo, main.cpp): the
    // first match of `keyword` on `row`, [col_start, col_end) 0-indexed
    // like Decoration's own fields. The scanning half of Lua's own
    // mep.todo_mark_buffer -- kBuiltinTodo maps each match's keyword to a
    // glyph/hl via mep.todoscan_keywords (still Lua, user-configurable)
    // and calls mep.deco_add; the buffer scan itself lives here.
    struct TodoMatch {
        int row = 0;
        int col_start = 0;
        int col_end = 0;
        std::string keyword;
    };
    /**
     * @brief Scans the current buffer for TODO/FIXME/HACK/NOTE-style keyword occurrences.
     * @return The matches found, each with its row, column range, and matched keyword.
     */
    std::vector<TodoMatch> TodoScanMatches() const;
    // DAP breakpoints (kBuiltinDap, main.cpp): toggles a breakpoint at the
    // cursor's line in the current file and keeps the gutter decorations
    // (a filled-circle sign) in sync -- the state (per-file line list) and
    // toggle logic that used to be a Lua local `mep_dap_breakpoints` table
    // + hand-rolled linear scan. Lines are 1-indexed throughout, matching
    // mep.cursor()'s own convention and DAP's own `linesStartAt1` wire
    // format -- kBuiltinDap's mep.dap_start reads them back via
    // DapBreakpointLines to build a setBreakpoints request, so this stays
    // 1-indexed rather than converting to this header's usual 0-indexed
    // convention.
    /**
     * @brief Toggles a DAP breakpoint at the cursor's line in the current file, syncing the gutter decoration.
     */
    void DapToggleBreakpoint();
    /**
     * @brief Returns the 1-indexed breakpoint lines currently set for a file.
     * @param filename The file path to look up breakpoints for.
     * @return The 1-indexed line numbers with an active breakpoint.
     */
    std::vector<int> DapBreakpointLines(const std::string &filename) const;
    // TermSend registry (kBuiltinTermSend, main.cpp): which terminal
    // buffer each "source" buffer's mod1+CR sends its lines/selection to.
    // TermsendRegister validates `target` is a live terminal buffer
    // itself (notifying and returning false if not, matching the old Lua
    // mep.termsend_register's own contract exactly); TermsendTarget
    // re-checks liveness on every read rather than caching, so a target
    // buffer that got closed/repurposed since registration correctly
    // reads back as "none" (0) without needing to be explicitly
    // unregistered first.
    /**
     * @brief Registers `target` as the terminal buffer that `source`'s mod1+CR sends lines/selection to.
     * @param source The source buffer id.
     * @param target The terminal buffer id to send to; must be a live terminal buffer.
     * @return True if registration succeeded, false (and notifies) if `target` isn't a live terminal buffer.
     */
    bool TermsendRegister(int source, int target);
    /**
     * @brief Returns the terminal buffer currently registered for a source buffer, re-checking liveness.
     * @param source The source buffer id to look up.
     * @return The registered terminal buffer id, or 0 if none is registered or it's no longer alive.
     */
    int TermsendTarget(int source) const;  // 0 = none registered/alive
    /**
     * @brief Unregisters any terminal-send target for a source buffer.
     * @param source The source buffer id to unregister.
     */
    void TermsendUnregister(int source);
    /**
     * @brief Returns the terminal buffers in the active tab's panes, as candidate termsend targets.
     * @return The candidate terminal buffer ids.
     */
    std::vector<int> TermsendCandidates() const;  // terminal buffers in the active tab's panes
    // Activity-bar Todo panel persistence (kBuiltinActivityBar, main.cpp).
    // Two backing formats, picked by `path`'s extension:
    //   - "*.org" (the default, the project's TODO.org): every keyworded
    //     headline is one item, and saving applies the panel's changes as
    //     surgical edits (OrgTodoListItems/OrgTodoListApply, org_doc.h) so
    //     notes, plain headlines and the "#+TODO:" line survive. If that
    //     file is open in a buffer, the buffer is the source of truth:
    //     loads read its live (possibly unsaved) lines, and saves edit the
    //     buffer (one undo step) and write it to disk only if it had no
    //     pending changes of its own -- the sidebar never silently saves
    //     the user's unrelated in-progress edits.
    //   - anything else: the panel's original "0|text\n"/"1|text\n" line
    //     format, whole-file rewrite, no escaping of embedded '|'/newlines
    //     (kept exactly for anyone still pointing mep.activity_todo_file
    //     at their old .mep_todos.txt).
    using ActivityTodoItem = OrgTodoItem;
    /**
     * @brief Loads the Activity-bar Todo panel's items from an org file (live buffer preferred) or a "0|text\n"-format file.
     * @param path The file path to read.
     * @return The loaded todo items, in file order.
     */
    std::vector<ActivityTodoItem> ActivityTodoLoad(const std::string &path) const;
    /**
     * @brief Writes the Activity-bar Todo panel's items back: minimal edits for an org file (through its open buffer, if any), whole-file rewrite otherwise.
     * @param path The file path to write.
     * @param items The todo items to persist, in order.
     */
    void ActivityTodoSave(const std::string &path, const std::vector<ActivityTodoItem> &items);
    // Todo sidebar edits beyond the checklist model (kBuiltinActivityBar's
    // Enter = clock start/stop and 'e' = retitle): org files only -- the
    // legacy "0|text" format has no headline to clock or retitle, so the
    // panel edits its items and ActivityTodoSaves those instead. All go
    // through the same live-buffer-else-file rule as ActivityTodoLoad/
    // ActivityTodoSave (ReadLinesForPath/WriteLinesForPath), over org_doc.h's
    // OrgClockStartLines/OrgClockStopLines/OrgTodoListRetitle.
    struct ActivityTodoClock {
        int line = -1;              // 0-based line of the clocked headline; -1 = no clock running
        std::string start_ts;       // the open CLOCK line's bracketed start body
        long long start_epoch = 0;  // that start as local-time Unix seconds (0 if unparsable)
        std::string title;          // the clocked headline's title (keyword/priority/tags stripped)
    };
    /**
     * @brief Reports the running clock in an org todo file, if any.
     * @param path The org file to scan (live buffer preferred).
     * @return The running clock; `line == -1` when none is open.
     */
    ActivityTodoClock ActivityTodoClockStatus(const std::string &path) const;
    /**
     * @brief Starts a clock under a headline of an org todo file (refused while another clock is open there).
     * @param path The org file to edit.
     * @param line The 0-based line of the headline to clock.
     * @return True if a CLOCK line was written.
     */
    bool ActivityTodoClockStart(const std::string &path, int line);
    /**
     * @brief Stops the running clock in an org todo file.
     * @param path The org file to edit.
     * @return The elapsed whole minutes, or -1 when no clock was open.
     */
    int ActivityTodoClockStop(const std::string &path);
    /**
     * @brief Retitles a keyworded headline of an org todo file, keeping its keyword, priority and tags.
     * @param path The org file to edit.
     * @param line The 0-based line of the headline.
     * @param text The new title.
     * @return True if the headline was rewritten.
     */
    bool ActivityTodoRetitle(const std::string &path, int line, const std::string &text);
    /**
     * @brief Tags a keyworded headline of an org todo file :ARCHIVE:, hiding it (and its subtree) from the panel.
     * @param path The org file to edit.
     * @param line The 0-based line of the headline.
     * @return True if the headline was rewritten.
     */
    bool ActivityTodoArchive(const std::string &path, int line);
    /**
     * @brief Moves a keyworded headline of an org todo file past its previous or next sibling, subtree and all.
     * @param path The org file to edit.
     * @param line The 0-based line of the headline to move.
     * @param delta -1 to swap with the previous sibling, +1 for the next.
     * @return The headline's line index after the move, or -1 if it couldn't move (no such sibling, or a stale/non-headline `line`).
     */
    int ActivityTodoMove(const std::string &path, int line, int delta);
    // Inverse of ReadLinesForPath, ActivityTodoSave's own write rule
    // factored out: an open buffer for `path` gets its lines replaced as
    // one undo step and is written to disk only if it had no pending
    // edits of its own; otherwise the file is rewritten directly.
    /**
     * @brief Writes a whole file's lines back, through its open buffer when there is one.
     * @param path The file to write.
     * @param updated The new full text, one entry per line.
     * @return True if the buffer was updated or the file written.
     */
    bool WriteLinesForPath(const std::string &path, const std::vector<std::string> &updated);
    // Activity-bar Tests panel: which lines of a test run's combined
    // stdout/stderr look like a failure (case-insensitive "fail"
    // substring), 1-indexed to match the original Lua's own ipairs index
    // (kBuiltinActivityBar's widget id is 'f' .. index).
    struct ActivityTestFailureLine {
        int index = 0;
        std::string line;
    };
    /**
     * @brief Finds which lines of a test run's combined output look like failures.
     * @param output The combined stdout/stderr lines of a test run.
     * @return The matching lines, 1-indexed, in file order.
     */
    std::vector<ActivityTestFailureLine> ActivityTestFailureLines(const std::vector<std::string> &output) const;
    // Syntax highlighting fallback lexer (kBuiltinSyntax, main.cpp): for a
    // filetype with no vendored Treesitter grammar, a hand-rolled per-line
    // scan (comment-prefix / quoted-string / number / keyword-list) over
    // the *current* buffer, adding decorations into `ns` directly --
    // ported from the Lua mep_syntax_scan_line + its per-line driver loop
    // in one pass rather than round-tripping each line's matches back
    // through a Lua table first. `keywords`/`comment_prefix` come from the
    // still-Lua-configurable mep.syntax_keywords[ft]/
    // mep.syntax_comment_prefix[ft] tables.
    /**
     * @brief Runs the hand-rolled fallback lexer over the current buffer for filetypes with no Treesitter grammar.
     * @param ns The decoration namespace to add highlights into.
     * @param keywords The filetype's configured keyword list.
     * @param comment_prefix The filetype's line-comment prefix.
     */
    void SyntaxHighlightFallback(int ns, const std::vector<std::string> &keywords, const std::string &comment_prefix);
    // Org emphasis markup (*bold*//italic//_underline_/+strike+/=verbatim=/
    // ~code~) over the *current* buffer, added into `ns` -- ported from
    // the Lua mep_org_scan_emphasis + mep_syntax_highlight_org_emphasis
    // (kBuiltinSyntax, main.cpp), including their exact word-boundary
    // marker-open/close rules and the #+begin_X/#+end_X block skip (a
    // literal block's own '_'/'*' characters were never meant to be
    // reinterpreted as emphasis).
    /**
     * @brief Scans the current buffer for org emphasis markup and adds decorations for it.
     * @param ns The decoration namespace to add highlights into.
     */
    void OrgHighlightEmphasis(int ns);
    // Markdown (kBuiltinMarkdown, main.cpp), directly bound as the exact
    // same mep.* names the Lua functions they replace used to have (no
    // wrapper needed -- each is now fully self-contained in C++):
    // toggles a `- [ ]`/`- [x]`-style checkbox on the cursor's line,
    // notifying (like the original) if there isn't one there.
    /**
     * @brief Toggles a `- [ ]`/`- [x]`-style checkbox on the cursor's line, notifying if none is present.
     */
    void MdToggleCheckbox();
    // Recomputes every 'markdown'-provider fold (fenced code blocks +
    // heading-nesting) over the current buffer -- bound directly as
    // mep.md_fold.
    /**
     * @brief Recomputes every markdown fold (fenced code blocks and heading nesting) over the current buffer.
     */
    void MdComputeFolds();
    // GFM pipe-table align/insert-row/insert-col -- bound directly as
    // mep.md_table_align/mep.md_table_insert_row/mep.md_table_insert_col.
    // InsertRow/InsertCol both call MdTableAlign() themselves at the end,
    // matching the original Lua's own call chain.
    /**
     * @brief Aligns the GFM pipe table touching the cursor.
     */
    void MdTableAlign();
    /**
     * @brief Inserts a new row into the GFM pipe table at the cursor, then realigns it.
     */
    void MdTableInsertRow();
    /**
     * @brief Inserts a new column into the GFM pipe table at the cursor, then realigns it.
     */
    void MdTableInsertCol();
    // Link/emphasis concealment scan over the current buffer, adding
    // virt_overlay decorations into `ns` for every span found (skipping
    // fenced code blocks and the cursor's own line, which always renders
    // raw) -- the namespace's own create/clear and the auto/filetype
    // gating stay in kBuiltinMarkdown's mep.md_conceal (this is its
    // mep.md_conceal_scan call, once those checks pass).
    /**
     * @brief Scans the current buffer for markdown link/emphasis spans to conceal and adds overlay decorations for them.
     * @param ns The decoration namespace to add the concealment overlays into.
     */
    void MdConceal(int ns);
    // Completion (kBuiltinCompletion, main.cpp) -- only the pieces with
    // no LSP-state dependency: the two "add candidates" scans and the
    // final sort+cap. The LSP source itself, snippet-trigger source, and
    // the overall per-word-position dispatch all stay Lua (deep coupling
    // with kBuiltinLsp's/kBuiltinOrgPolyglot's own private state --
    // LUA_TO_CPP_PLAN.md's Phase LSP territory, not this one).
    //
    // Every buffer word (`[A-Za-z0-9_]+` run) longer than `prefix` and
    // starting with it, first-occurrence order, deduplicated *within this
    // scan only* -- the caller still needs to filter the result against
    // its own already-claimed (e.g. by a snippet trigger name) set itself.
    /**
     * @brief Finds every buffer word starting with `prefix` and longer than it, for completion candidates.
     * @param prefix The prefix text already typed before the cursor.
     * @return The matching words, in first-occurrence order, deduplicated within this scan.
     */
    std::vector<std::string> CompletionScanBufferWords(const std::string &prefix) const;
    // mep_completion_path_prefix's own port: does the text just before
    // `prefix` on `line` look like a filesystem path (`/`-triggered, a
    // leading-`.` dotfile/relative path, or inside a require(/import /
    // from  string) -- `col` is mep.cursor()'s own 1-indexed convention
    // (this only ever reads it, doesn't touch cursor state). Returns
    // found=false if it doesn't.
    struct CompletionPathPrefix {
        bool found = false;
        std::string dir;
        std::string base;
    };
    /**
     * @brief Checks whether the text just before `prefix` on `line` looks like a filesystem path.
     * @param prefix The completion prefix text already typed before the cursor.
     * @param col The 1-indexed cursor column (mep.cursor()'s convention).
     * @param line The full text of the current line.
     * @return The resolved directory/base if it looks like a path, with found=false otherwise.
     */
    CompletionPathPrefix CompletionPathPrefixFor(const std::string &prefix, int col, const std::string &line) const;
    // Snippet expansion + tabstop jumping (kBuiltinSnippets, main.cpp) --
    // the tabstop parser (`$N`/`${N}`/`${N:default}`/`\$`), the splice
    // (write the parsed body into the buffer, single-line via SetLineForLua
    // or multi-line via ReplaceLinesForLua) and the jump-between-tabstops
    // state machine all moved to C++; the snippet *bodies themselves*
    // (mep.snippets, a big per-filetype template registry) stay Lua data,
    // and mep.snippet_expand/mep.set_completion_accept_hook's callback
    // (main.cpp) stay Lua glue -- both just look up/derive a body and
    // before/after text, then call SnippetSplice.
    //
    // One tabstop's resolved position in an expanded snippet: `line_idx`
    // is 1-indexed *within the snippet body* (not the buffer), `col` is
    // the 1-indexed column on that expanded line, `num` is the tabstop's
    // own `$N` number (0 = the final/last-stop position, sorted after
    // every positive N per real snippet-engine convention).
    struct SnippetTabstop {
        int line_idx = 0;
        int col = 0;
        int num = 0;
    };
    // Splices `body` (template lines, 0-indexed `row` is where body[0]
    // lands) into the buffer, `before`/`after` wrapping the first/last
    // line (the text surrounding whatever triggered the expansion),
    // computes every tabstop, and jumps to the first one -- mirrors the
    // original's own mep_snippet_splice + immediate SnippetJump(1) call.
    /**
     * @brief Splices an expanded snippet body into the buffer and jumps to its first tabstop.
     * @param row The 0-indexed row where body[0] lands.
     * @param before The text surrounding the trigger, to prepend before the first body line.
     * @param after The text surrounding the trigger, to append after the last body line.
     * @param body The snippet's template lines to splice in.
     */
    void SnippetSplice(int row, const std::string &before, const std::string &after,
                        const std::vector<std::string> &body);
    // Moves to the tabstop `delta` positions from the current one (1 =
    // next, -1 = previous); a no-op if no snippet is active, clears the
    // active state once moved past either end. Bound directly as
    // mep.snippet_jump (no wrapper -- MepSnippetNext/Prev call it with a
    // literal 1/-1 same as before).
    /**
     * @brief Moves to the tabstop `delta` positions from the current one in the active snippet.
     * @param delta The number of tabstops to move (1 = next, -1 = previous).
     */
    void SnippetJump(int delta);
    // Picker file preview (kBuiltinPickerSources, main.cpp): reads up to
    // `max_lines` of `path` and sets it as the picker's preview pane text
    // (SetPickerPreview) -- mep_picker_preview_file's own port, including
    // its exact "(cannot open PATH)"/"..." truncation-marker messages.
    // Bound directly as mep.picker_preview_file.
    /**
     * @brief Reads up to `max_lines` of `path` and sets it as the picker's preview pane text.
     * @param path The file path to preview.
     * @param max_lines The maximum number of lines to read.
     */
    void PreviewFile(const std::string &path, int max_lines);
    // File tree (kBuiltinFileTree, main.cpp): the recursive directory
    // walk (mep_tree_build_widgets' own traversal half, port of --
    // hidden/gitignore filtering, expand-state-driven recursion into
    // expanded directories, dirs-first-then-alpha ordering) flattened
    // into one depth-first row list in a single call, instead of the
    // original's recursive-with-side-effects Lua function. The
    // expand/gitignore *state* itself (mep_tree_expanded/mep_tree_ignored)
    // stays Lua -- it's mutated from widget on_click closures, which need
    // Lua refs regardless -- so it's passed in fresh each call rather
    // than mirrored onto Editor.
    struct FileTreeRow {
        std::string full_path;
        std::string name;
        bool is_dir = false;
        int depth = 0;
        bool expanded = false;  // only meaningful when is_dir
    };
    /**
     * @brief Builds the file tree's flattened, depth-first row list for `root`, honoring expand/hidden/ignore state.
     * @param root The root directory to walk.
     * @param expanded_paths The directory paths currently expanded in the tree.
     * @param show_hidden Whether hidden (dotfile) entries should be included.
     * @param ignored_relpaths Paths (relative to root) that gitignore filtering excludes.
     * @return The flattened rows, dirs-first-then-alpha, in depth-first order.
     */
    std::vector<FileTreeRow> BuildFileTreeRows(const std::string &root,
                                                const std::vector<std::string> &expanded_paths, bool show_hidden,
                                                const std::vector<std::string> &ignored_relpaths) const;
    // First of README.md/README.org/README.txt/README present as a file
    // (not a dir) directly in `dir` -- empty string if none match.
    // mep_project_readme_path's own port (kBuiltinFileTree).
    /**
     * @brief Finds the first README.md/README.org/README.txt/README file directly inside `dir`.
     * @param dir The directory to search (non-recursively).
     * @return The matching file's path, or an empty string if none match.
     */
    std::string ProjectReadmePath(const std::string &dir) const;
    // Colorizer (kBuiltinTextTools, main.cpp): scans the current buffer
    // for #RRGGBBAA/#RRGGBB/#RGB hex literals, rgb()/rgba() calls, and
    // CSS3/SVG named colors, adding a swatch decoration for each --
    // mep.colorize's own port, including its exact overlap-avoidance
    // order (8-hex claims first, 6-hex only if unclaimed, 3-hex only if
    // unclaimed and doesn't itself claim, rgb()/rgba() and named colors
    // both independent of the claim-tracking). Bound directly as
    // mep.colorize -- owns its own namespace create/clear, nothing left
    // in Lua to wrap.
    /**
     * @brief Scans the current buffer for hex/rgb()/rgba()/named color literals and adds a swatch decoration for each.
     */
    void Colorize();
    // URL detection (kBuiltinTextTools): the URL-shaped span scan
    // (mep.nvim's own MEP_URL_PATTERN) -- UrlUnderCursor bound directly
    // as mep.url_under_cursor (returns "" for none, matching the
    // original's nil); ListUrls is mep.list_urls_scan, still wrapped by
    // a thin Lua mep.list_urls that opens the picker over the result.
    /**
     * @brief Finds the URL-shaped span under the cursor, if any.
     * @return The URL text, or an empty string if the cursor isn't on one.
     */
    std::string UrlUnderCursor() const;
    /**
     * @brief Scans the current buffer for every URL-shaped span.
     * @return The list of URLs found, in buffer order.
     */
    std::vector<std::string> ListUrls() const;
    // Git gutter (kBuiltinGit, main.cpp) -- unlike earlier phases, this
    // one moves the *async orchestration itself* to C++, not just a
    // synchronous helper Lua calls into: JobManager::Spawn already takes
    // real std::function callbacks (see LUA_TO_CPP_PLAN.md's "async is
    // not actually the blocker" note), so `git show`'s on_exit runs pure
    // C++ -- no Lua ref stored or invoked anywhere in this path. State
    // (git_hunks_/git_base_lines_, below) moved off Lua-local tables onto
    // Editor for the same reason. `base` (a git revision -- HEAD, a
    // branch, a SHA) is still read from the Lua-configurable
    // mep.git_gutter_base each call rather than cached here, matching
    // this plan's usual "config stays Lua, passed in as a parameter"
    // convention -- kBuiltinGit's own mep.git_gutter_refresh is now a
    // one-line wrapper threading that global through.
    /**
     * @brief Asynchronously diffs the current buffer against `base` and refreshes the git gutter hunks.
     * @param base The git revision (HEAD, a branch, or a SHA) to diff against.
     */
    void GitGutterRefresh(const std::string &base);
    // 1-indexed target row, or 0 if there are no hunks at all -- the
    // find-next/prev-hunk-relative-to-cursor half of mep.git_next_hunk/
    // mep.git_prev_hunk; the cursor move + opt-in preview-on-jump stay a
    // thin Lua wrapper (mep.git_hunk_preview_on_jump is a user-toggleable
    // global).
    /**
     * @brief Finds the git hunk after the cursor's current row.
     * @return The 1-indexed target row, or 0 if there are no hunks.
     */
    int GitNextHunkRow() const;
    /**
     * @brief Finds the git hunk before the cursor's current row.
     * @return The 1-indexed target row, or 0 if there are no hunks.
     */
    int GitPrevHunkRow() const;
    // The hunk-under-cursor's diff-format preview text ("(empty hunk)"
    // for a zero-line hunk, matching the original), or found=false if the
    // cursor isn't on a hunk -- kBuiltinGit's own mep.git_preview_hunk_text.
    /**
     * @brief Builds the diff-format preview text of the git hunk under the cursor.
     * @return The preview text ("(empty hunk)" for a zero-line hunk), with found=false if the cursor isn't on a hunk.
     */
    std::pair<bool, std::string> GitPreviewHunkText() const;
    // Reverts the hunk under the cursor's lines back to `base`'s version,
    // then re-runs GitGutterRefresh(base).
    /**
     * @brief Reverts the git hunk under the cursor back to `base`'s version and refreshes the gutter.
     * @param base The git revision to revert the hunk's lines back to.
     */
    void GitResetHunk(const std::string &base);
    // Builds a minimal single-hunk unified diff for the hunk under the
    // cursor and applies it with `git apply --cached --unidiff-zero -`
    // (JobManager::Spawn + WriteStdin + CloseStdin directly -- no Lua ref
    // needed for on_exit either). Bound directly as mep.git_stage_hunk,
    // no wrapper.
    /**
     * @brief Stages the git hunk under the cursor via `git apply --cached --unidiff-zero -`.
     */
    void GitStageHunk();
    // mep_org_current_headline_row's own port: the nearest headline at or
    // above `row` (1-indexed; 0 means "use the cursor's own row"), 0 if
    // none. mep_org_subtree_end's own port: the exclusive end of `row`'s
    // subtree (the next headline at level <= `row`'s own, or
    // line_count()+1) -- `row` must itself already be a valid headline
    // row, same undocumented precondition the original had (every real
    // call site already guarantees this by construction).
    /**
     * @brief Finds the nearest org headline at or above `row`.
     * @param row The 1-indexed row to search from (0 means the cursor's own row).
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return The headline's 1-indexed row, or 0 if none is found.
     */
    int OrgCurrentHeadlineRow(int row, const std::vector<std::string> &todo_keywords) const;
    /**
     * @brief Finds the exclusive end row of the org subtree rooted at the headline on `row`.
     * @param row The 1-indexed row of a valid headline.
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return The next headline row at or above `row`'s level, or line_count()+1 if none.
     */
    int OrgSubtreeEnd(int row, const std::vector<std::string> &todo_keywords) const;
    // kBuiltinOrgClock's own port (LUA_TO_CPP_PLAN.md Phase 5). Starts a
    // clock on the headline at/above the cursor: warns if one is already
    // running anywhere in the buffer, else inserts (or appends to an
    // existing) :LOGBOOK: drawer under that headline with an open
    // "CLOCK: [now]" line. mep.org_clock_effort stays a thin Lua wrapper
    // (it's just a one-line call into OrgPropertyGet + OrgCurrentHeadlineRow,
    // not worth its own binding).
    /**
     * @brief Starts an org clock on the headline at/above the cursor, warning if one is already running.
     */
    void OrgClockIn();
    // Finds the (first) open "CLOCK: [ts]" line in the buffer, closes it
    // with "--[now] =>  H:MM", and reports the duration.
    /**
     * @brief Closes the first open org clock line in the buffer and reports its duration.
     */
    void OrgClockOut();
    // Total clocked minutes across lines [a, b] (1-indexed, inclusive) --
    // mep_org_clock_minutes_in_range's own port.
    /**
     * @brief Totals the clocked minutes recorded across a range of lines.
     * @param a The 1-indexed start line, inclusive.
     * @param b The 1-indexed end line, inclusive.
     * @return The total clocked minutes in [a, b].
     */
    int OrgClockMinutesInRange(int a, int b) const;
    // One "indent + title  H:MM" entry per headline with clocked time,
    // used by kBuiltinOrgClock's thin mep.org_clock_table() wrapper to
    // build the mep.picker_open items list (picker itself stays Lua glue).
    /**
     * @brief Builds a "indent + title  H:MM" entry per headline with clocked time.
     * @return The clock table entries, for the clock table picker.
     */
    std::vector<std::string> OrgClockTableItems() const;
    // kBuiltinOrg's own mep.org_property_get/set/remove: reads/writes a
    // ":PROPERTIES: ... :END:" drawer entry under the headline at `row`
    // (<=0 means "use the nearest headline at/above the cursor", same
    // convention as OrgCurrentHeadlineRow). `key` is matched
    // case-insensitively, matching the original's `k:upper() ==
    // key:upper()`. Ported opportunistically while working the org
    // cluster (Phase 5): small, self-contained, and unblocks
    // mep.org_clock_effort/mep.org_drill_grade, which are otherwise thin
    // wrappers around a still-Lua property store.
    /**
     * @brief Reads a key's value from the :PROPERTIES: drawer under a headline.
     * @param row The 1-indexed headline row (<=0 means the nearest headline at/above the cursor).
     * @param key The property key to look up (matched case-insensitively).
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return The property's value, with found=false if the key isn't present.
     */
    std::pair<bool, std::string> OrgPropertyGet(int row, const std::string &key,
                                                 const std::vector<std::string> &todo_keywords) const;
    /**
     * @brief Writes a key/value entry into the :PROPERTIES: drawer under a headline, creating it if needed.
     * @param row The 1-indexed headline row (<=0 means the nearest headline at/above the cursor).
     * @param key The property key to write (matched case-insensitively).
     * @param value The value to store.
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     */
    void OrgPropertySet(int row, const std::string &key, const std::string &value,
                         const std::vector<std::string> &todo_keywords);
    /**
     * @brief Removes a key's entry from the :PROPERTIES: drawer under a headline.
     * @param row The 1-indexed headline row (<=0 means the nearest headline at/above the cursor).
     * @param key The property key to remove (matched case-insensitively).
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     */
    void OrgPropertyRemove(int row, const std::string &key, const std::vector<std::string> &todo_keywords);
    // kBuiltinOrgDrill's own `mep_org_sm2` + `mep.org_drill_grade` port:
    // runs the SM-2 spaced-repetition update against the DRILL_EF/
    // DRILL_REPS/DRILL_INTERVAL properties on the headline at `row`
    // (quality 0-5), then persists the updated properties plus a
    // recomputed DRILL_DUE date. The rest of kBuiltinOrgDrill
    // (mep.org_drill_collect_due/mep.org_drill_review) stays Lua: it
    // reads *other* files off disk via the still-Lua
    // mep_org_read_file_lines (a deliberate "stays Lua" choice, see
    // kBuiltinOrgAgenda's own comment) and drives a ui_confirm/ui_select
    // dialog chain.
    /**
     * @brief Runs the SM-2 spaced-repetition update against a headline's drill properties and persists the result.
     * @param row The 1-indexed headline row.
     * @param quality The recall quality grade (0-5).
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     */
    void OrgDrillGrade(int row, int quality, const std::vector<std::string> &todo_keywords);
    // mep_org_resolve_path's own port (LUA_TO_CPP_PLAN.md Phase 5):
    // resolves a link/header-arg path against the org file's own
    // directory (LspAbspath(mep.filename())-derived), unless already
    // absolute (a leading '/') or ~-relative. Bare Lua global for the
    // same reason as Org-0/mep_lsp_*: kBuiltinOrgBabel (a separate
    // DoString chunk) reuses it for :file/:tangle header-arg resolution.
    /**
     * @brief Resolves a link/header-arg path against the current org file's own directory.
     * @param path The path to resolve (already-absolute or ~-relative paths pass through unchanged).
     * @return The resolved path.
     */
    std::string OrgResolvePath(const std::string &path) const;
    // kBuiltinOrgImages' own mep.org_image_scan port: rebuilds the
    // current buffer's image-row registry (SetOrgImageRow/
    // ClearOrgImageRows) from its [[file:...]] links -- same
    // [[target][description]] handling as mep.org_link_at_cursor, same
    // file:-prefix-only dispatch mep.org_link_follow uses.
    /**
     * @brief Rebuilds the current buffer's image-row registry from its [[file:...]] links.
     */
    void OrgImageScan();
    // kBuiltinOrgAgenda's own `mep_org_expand_glob` port: `*` within the
    // final path component only (no recursive `**`), matched via
    // ListDirectory -- entries without a `*` (or with no `/` at all)
    // pass through unchanged, same as the original.
    /**
     * @brief Expands a `*`-wildcard pattern (within the final path component only) against the filesystem.
     * @param pattern The glob pattern to expand.
     * @return The matching paths, or `pattern` unchanged if it has no wildcard in its final component.
     */
    std::vector<std::string> OrgAgendaExpandGlob(const std::string &pattern) const;
    // kBuiltinOrgAgenda's own `mep.org_agenda_collect` per-file core: one
    // entry per headline in `lines` (an already-disk-read file, `path`
    // its origin), pairing each with any SCHEDULED:/DEADLINE: planning
    // line immediately below it. The outer "for each configured agenda
    // file, read its lines, scan them" loop stays Lua -- reading a file
    // that isn't the live buffer stays on Lua's stdlib `io`, see
    // kBuiltinOrgAgenda's own comment (main.cpp) -- but this, the actual
    // per-line parsing work, doesn't need to.
    struct OrgAgendaEntry {
        std::string file;
        int line = 0;
        std::string todo;
        bool has_todo = false;
        std::string title;
        std::string tags;
        bool has_tags = false;
        std::string priority;
        bool has_priority = false;
        std::string scheduled;
        bool has_scheduled = false;
        std::string deadline;
        bool has_deadline = false;
    };
    /**
     * @brief Scans an already-read org file's lines for headlines and pairs each with any planning line below it.
     * @param lines The file's lines, already read from disk.
     * @param path The file's origin path, recorded on each entry.
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return One agenda entry per headline found.
     */
    std::vector<OrgAgendaEntry> OrgAgendaScanLines(const std::vector<std::string> &lines, const std::string &path,
                                                    const std::vector<std::string> &todo_keywords) const;
    // kBuiltinOrgCapture's own `mep_org_expand_template` port: expands
    // org-capture's own small placeholder set (%U/%u -- inactive
    // timestamp with/without time, %T/%t -- active timestamp with/
    // without time, %a -- a `[[file:...]]` link to the current buffer,
    // %% -- literal '%') against a capture template string.
    /**
     * @brief Expands org-capture's placeholder set (%U/%u/%T/%t/%a/%%) against a capture template string.
     * @param tmpl The capture template text to expand.
     * @return The expanded template text.
     */
    std::string OrgExpandCaptureTemplate(const std::string &tmpl) const;
    // kBuiltinOrgCapture's own mep.org_refile port, same-buffer branch
    // only (see main.cpp's own comment on why the cross-file branch
    // stays Lua): moves the subtree at the headline the cursor is
    // currently on/inside to become the last child of `target_row`,
    // re-leveling every line's stars by the same delta the original's
    // own `reindent` closure computed. Returns the new 1-indexed cursor
    // row, or 0 if the cursor isn't on a headline.
    /**
     * @brief Moves the subtree at the cursor's headline to become the last child of `target_row`, re-leveling its stars.
     * @param target_row The 1-indexed row of the headline to refile under.
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return The new 1-indexed cursor row, or 0 if the cursor isn't on a headline.
     */
    int OrgRefileMove(int target_row, const std::vector<std::string> &todo_keywords);
    // kBuiltinOrgLatex's own fragment-detection port (LUA_TO_CPP_PLAN.md
    // Phase 5): finds every whole-line/whole-block LaTeX fragment
    // (#+BEGIN_LATEX/#+BEGIN_SRC latex blocks, \[..\]/$$..$$/\(..\) --
    // single-line or multi-line -- and a bare single-line $..$) plus,
    // on any line that isn't wholly one of those, every *inline*
    // fragment embedded within it ("the value $x^2$ matters here").
    // Pure buffer-scan -- deliberately does *not* cover the actual
    // render pipeline (mep_org_latex_render's tectonic-compile ->
    // pdftoppm-rasterize -> cache -> callback chain, main.cpp): that's
    // a substantial async job-orchestration-and-caching subsystem in
    // its own right, out of proportion to port opportunistically here.
    struct OrgLatexBlock {
        int start_row = 0;  // 1-indexed
        int end_row = 0;    // 1-indexed, inclusive
        std::string body;
    };
    struct OrgLatexInlineSpan {
        int row = 0;        // 1-indexed
        int col_start = 0;  // 1-indexed, inclusive
        int col_end = 0;    // 1-indexed, exclusive
        std::string body;
    };
    struct OrgLatexScanResult {
        std::vector<OrgLatexBlock> blocks;
        std::vector<OrgLatexInlineSpan> inlines;
    };
    /**
     * @brief Scans the current buffer for whole-block and inline LaTeX fragments.
     * @return Every LaTeX block and inline span found in the buffer.
     */
    OrgLatexScanResult OrgLatexScanFragments() const;
    // kBuiltinOrgBib's own hand-rolled BibTeX parser port
    // (LUA_TO_CPP_PLAN.md Phase 5): mep_org_bib_split_top_level/
    // mep_org_bib_expand_value/mep_org_bib_parse/
    // mep_org_bib_resolve_crossrefs, combined into one entry point.
    // Takes each already-disk-read .bib file's full text (reading .bib
    // files off disk stays Lua, same "headless file loading" convention
    // as kBuiltinOrgAgenda/kBuiltinOrgDrill's own read helpers), threads
    // the `@string{...}` macro table across all of them in order (a
    // macro must be defined before first use, matching real BibTeX,
    // including across files when the caller passes them in resolution
    // order), then resolves `crossref` fields once over the complete
    // combined entry set.
    struct OrgBibEntry {
        std::string type;
        std::string key;
        std::map<std::string, std::string> fields;
    };
    /**
     * @brief Parses BibTeX entries from already-read .bib file texts and resolves crossref fields across them.
     * @param file_texts The full text of each .bib file, in macro-resolution order.
     * @return The parsed and crossref-resolved bibliography entries.
     */
    std::vector<OrgBibEntry> OrgBibParseFiles(const std::vector<std::string> &file_texts) const;
    // kBuiltinOrgBib's own citation-recognition-at-cursor port: resolves
    // both modern org-cite (`[cite:@key]`, `[cite/style:@key1;@key2]`)
    // and legacy org-ref (`cite:key`, `citep:key`, ...) syntax under the
    // cursor to the same key list. Registered as a *bare global*
    // (`mep_org_bib_cite_at_cursor`, lua_env.cpp) -- kBuiltinOrgLinks'
    // own `mep.org_link_follow` (a separate DoString chunk) calls this
    // by that exact name, the same cross-chunk-sharing idiom Org-0/
    // mep_lsp_*/mep_org_resolve_path already use.
    struct OrgBibCiteSpan {
        int col_start;  // 1-indexed, inclusive
        int col_end;    // 1-indexed, inclusive
        std::vector<std::string> keys;
    };
    /**
     * @brief Resolves the org-cite or org-ref citation syntax under the cursor to its citation keys.
     * @return The cited keys found under the cursor, if any.
     */
    std::vector<std::string> OrgBibCiteAtCursor() const;
    // kBuiltinOrgRoam's own ports (LUA_TO_CPP_PLAN.md Phase 5): the
    // pure-computation pieces of the zettelkasten note-linking cluster.
    // Reading each note file off disk stays Lua (this cluster's usual
    // "headless file loading" convention), as does anything ending in a
    // picker/UI callback or touching a *different* file via
    // mep.pane_open -- these take an already-read lines array and do
    // the actual parsing/scanning.
    //
    // mep_org_roam_files' own port: every `.org` file directly inside
    // any of `dirs` (no recursion, matching the original).
    /**
     * @brief Finds every `.org` file directly inside any of `dirs` (no recursion).
     * @param dirs The directories to scan.
     * @return The matching file paths.
     */
    std::vector<std::string> OrgRoamFilesIn(const std::vector<std::string> &dirs) const;
    // mep_org_roam_title_of's own port: `#+TITLE:` (case-insensitive),
    // else the first headline's title, else not-found.
    /**
     * @brief Derives a roam note's title from its `#+TITLE:` line, else its first headline.
     * @param lines The note file's lines, already read from disk.
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return The derived title, with found=false if neither a `#+TITLE:` nor a headline exists.
     */
    std::pair<bool, std::string> OrgRoamTitleOf(const std::vector<std::string> &lines,
                                                 const std::vector<std::string> &todo_keywords) const;
    // mep.org_roam_ensure_id's own port: the current buffer's own :ID:
    // property (scanned before the first headline), or a freshly
    // generated one inserted as a new :PROPERTIES: drawer at the top of
    // the buffer. ID generation uses C++'s own <random> rather than
    // Lua's math.random -- the ID is an opaque unique-enough string, not
    // a value any code depends on matching a specific PRNG stream, so
    // this doesn't change any observable contract.
    /**
     * @brief Returns the current buffer's roam :ID: property, generating and inserting a fresh one if absent.
     * @return The buffer's roam id.
     */
    std::string OrgRoamEnsureId();
    // mep.org_roam_backlinks' own scan port: every 1-indexed line in
    // `lines` containing a `[[id:<target_id>` substring (a plain
    // substring search, not a pattern) -- the sidebar widget
    // construction itself (with its on_click Lua closures) stays Lua.
    /**
     * @brief Finds every line containing a `[[id:<target_id>` link substring.
     * @param lines The lines to search.
     * @param target_id The roam id being linked to.
     * @return The 1-indexed matching line numbers.
     */
    std::vector<int> OrgRoamFindBacklinkLines(const std::vector<std::string> &lines,
                                               const std::string &target_id) const;
    // mep_org_roam_index's own per-file port: this file's own :ID:
    // (scanned the same way OrgRoamEnsureId's read-half does; a file
    // with none is left out of the index entirely, matching the
    // original), its title (OrgRoamTitleOf), and every deduplicated
    // `[[id:...]]` link target across all its lines. The outer
    // "for each roam file, read it, index it" loop and the graph's own
    // hop-BFS/edge-building stay Lua.
    struct OrgRoamFileIndexEntry {
        bool has_id = false;
        std::string id;
        bool has_title = false;
        std::string title;
        std::vector<std::string> links;
    };
    /**
     * @brief Indexes a roam file's own :ID:, title, and every deduplicated `[[id:...]]` link target it contains.
     * @param lines The file's lines, already read from disk.
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     * @return The file's index entry (has_id=false if it has no :ID:).
     */
    OrgRoamFileIndexEntry OrgRoamParseFileIndex(const std::vector<std::string> &lines,
                                                 const std::vector<std::string> &todo_keywords) const;
    // kBuiltinOrgLinks's own ports (LUA_TO_CPP_PLAN.md Phase 5): table
    // alignment, link/footnote/timestamp detection-at-cursor, and
    // timestamp insert/shift/planning-line edits -- all pure
    // current-buffer scan-and-edit, no async. Left in Lua: link_insert/
    // timestamp_set_repeater/timestamp_insert_range (each a ui_input
    // chain), org_store_link (sets Lua-side `mep.org_stored_link`
    // state), org_link_follow's dispatch (mep.pane_open for file:
    // targets) and org_link_highlight/org_timestamp_highlight (each a
    // scan feeding an mep.deco_add loop -- a reasonable next cut, not
    // done here).
    //
    // mep.org_table_align's own port: aligns the contiguous run of
    // table rows touching the cursor.
    /**
     * @brief Aligns the contiguous run of org table rows touching the cursor.
     */
    void OrgTableAlign();
    // mep.org_link_at_cursor's own port.
    struct OrgLinkAtCursorResult {
        bool found = false;
        std::string target;
        bool has_desc = false;
        std::string desc;
    };
    /**
     * @brief Finds the org link under the cursor, if any.
     * @return The link's target and optional description, with found=false if the cursor isn't on one.
     */
    OrgLinkAtCursorResult OrgLinkAtCursor() const;
    // mep_org_timestamp_at's own port: the active `<...>` or inactive
    // `[...]` org timestamp at `col` on `line`, if any.
    struct OrgTimestampMatch {
        bool found = false;
        int col_start = 0;  // 1-indexed, inclusive
        int col_end = 0;    // 1-indexed, inclusive
        std::string body;
        bool active = false;
    };
    /**
     * @brief Finds the active `<...>` or inactive `[...]` org timestamp at `col` on `line`, if any.
     * @param line The line text to search.
     * @param col The 1-indexed column to check.
     * @return The matched timestamp's span and text, with found=false if none is present at that column.
     */
    OrgTimestampMatch OrgTimestampAt(const std::string &line, int col) const;
    // mep.org_timestamp_insert's own port: inserts an active/inactive
    // today-timestamp at the cursor.
    /**
     * @brief Inserts a today-dated org timestamp at the cursor.
     * @param active Whether to insert an active `<...>` timestamp instead of an inactive `[...]` one.
     */
    void OrgTimestampInsert(bool active);
    // mep.org_timestamp_shift's own port: shifts the timestamp under
    // the cursor by `delta_days` calendar days, preserving any trailing
    // repeater/weekday-independent text.
    /**
     * @brief Shifts the org timestamp under the cursor by a number of calendar days, preserving any repeater text.
     * @param delta_days The number of calendar days to shift by (may be negative).
     */
    void OrgTimestampShift(int delta_days);
    // mep.org_footnote_jump's own port: jumps from a `[fn:name]`
    // reference under the cursor to its definition line (or, absent
    // one, the first other reference), or vice versa.
    /**
     * @brief Jumps between a `[fn:name]` reference under the cursor and its definition (or vice versa).
     */
    void OrgFootnoteJump();
    // mep.org_set_planning's own port: inserts or replaces a
    // SCHEDULED:/DEADLINE: planning line directly below the current
    // headline.
    /**
     * @brief Inserts or replaces a SCHEDULED:/DEADLINE: planning line directly below the current headline.
     * @param kind Which planning keyword to set ("SCHEDULED" or "DEADLINE").
     * @param todo_keywords The configured TODO keywords used to identify headlines.
     */
    void OrgSetPlanning(const std::string &kind, const std::vector<std::string> &todo_keywords);
    // mep_org_src_block_at's own port: see the OrgSrcBlock struct's own
    // comment (near OrgExpandMacroLine, above) for the full picture.
    /**
     * @brief Finds the org source block containing `row`, if any.
     * @param row The 1-indexed row to check.
     * @return The enclosing source block's info.
     */
    OrgSrcBlock OrgSrcBlockAt(int row) const;
    // kBuiltinLsp's own ports (LUA_TO_CPP_PLAN.md Phase LSP): the pure
    // buffer-scan/edit pieces, not the LSP request/response machinery
    // itself. That machinery (mep.lsp_request's callback -- a Lua ref,
    // via LuaEnv::CallRefWithJson) is the actual reason every real
    // feature here (hover/definition/references/rename/code-action/...)
    // stays Lua: each one *is* "build params, issue a request, handle
    // the response in a callback", and that callback is structurally a
    // Lua closure by the existing request API's own design, not
    // something a buffer-scan port can route around. `g_lsp_clients`/
    // `LspClientState` (lua_env.cpp) already *are* real C++ state (not
    // Lua tables) with real JobManager-callback-driven JSON-RPC framing
    // -- moving that map's storage onto Editor specifically, as this
    // plan's own coupling note suggested, wouldn't by itself unblock
    // porting any of the above; the callback-threading is the actual
    // obstacle, and it lives in `mep.lsp_request`'s own Lua-ref
    // signature regardless of where the client map lives. Left for a
    // future pass alongside a native (std::function-callback) request
    // path, if this cluster is revisited.
    //
    // mep_lsp_word_at_cursor's own port: the `[%w_]` word touching the
    // cursor on the current line, or not-found -- rename's own default
    // prefill.
    /**
     * @brief Finds the `[%w_]` word touching the cursor on the current line, for rename's default prefill.
     * @return The word text, with found=false if the cursor isn't touching a word.
     */
    std::pair<bool, std::string> LspWordAtCursor() const;
    // mep_lsp_apply_text_edit's own port: splices one LSP TextEdit
    // (0-indexed line/character range + replacement text) into the
    // current buffer, preserving the untouched prefix/suffix of the
    // range's first/last line.
    /**
     * @brief Splices one LSP TextEdit into the current buffer, preserving the untouched prefix/suffix text.
     * @param start_line The 0-indexed start line of the edit range.
     * @param start_char The 0-indexed start character of the edit range.
     * @param end_line The 0-indexed end line of the edit range.
     * @param end_char The 0-indexed end character of the edit range.
     * @param new_text The replacement text.
     */
    void LspApplyTextEdit(int start_line, int start_char, int end_line, int end_char, const std::string &new_text);
    // mep_lsp_apply_edits_current_buffer's own port: applies a batch of
    // TextEdits bottom-up (descending start position) so an
    // earlier-applied edit's line/column shift never invalidates a
    // later one still queued.
    struct LspTextEdit {
        int start_line = 0;
        int start_char = 0;
        int end_line = 0;
        int end_char = 0;
        std::string new_text;
    };
    /**
     * @brief Applies a batch of LSP TextEdits to the current buffer, bottom-up so earlier edits don't shift later ones.
     * @param edits The edits to apply.
     */
    void LspApplyEditsCurrentBuffer(std::vector<LspTextEdit> edits);
    // Replaces lines [start_row, end_row) (0-indexed, end exclusive) with
    // `lines` -- a general "splice" primitive (used first by Phase 17's
    // reset-hunk, generically useful for any future multi-line edit like
    // an LSP text edit or formatter output).
    /**
     * @brief Replaces a range of lines in the current buffer.
     * @param start_row The 0-indexed start row of the range to replace.
     * @param end_row The 0-indexed end row of the range, exclusive.
     * @param lines The replacement lines.
     */
    void ReplaceLinesForLua(int start_row, int end_row, const std::vector<std::string> &lines);
    // Replaces a specific buffer's *entire* content by id, regardless of
    // which pane/buffer is currently active (Part VI Phase 27: streaming
    // terminal/Run/REPL output into a background buffer the user isn't
    // necessarily looking at right now).
    /**
     * @brief Replaces a specific buffer's entire content by id, regardless of which pane is active.
     * @param buffer_id The id of the buffer to replace.
     * @param lines The new full content of the buffer.
     */
    void SetBufferLinesForLua(int buffer_id, const std::vector<std::string> &lines);

    // --- Participant-addressed editing (COLLAB_CURSORS_PLAN.md) ---
    // Same shape as ReplaceLinesForLua/SetLineForLua above, but take an
    // explicit buffer_id + position instead of implicitly targeting
    // CurPane()/Buf() -- used by agent-rpc connections and (eventually)
    // collaboration peers so a remote participant's own independent
    // cursor can be edited at without disturbing the local user's real
    // one, or whatever buffer they currently have open. Each bounds-
    // checks buffer_id (no-op on an invalid one) and pushes its own
    // per-buffer undo entry (PushUndoForBuffer), so the local user can
    // always `u`-undo a remote edit like any other change.
    //
    // InsertTextAt deliberately does not reuse InsertChar/InsertNewline
    // (editor.cpp): InsertChar rejects any codepoint outside 32-126, and
    // the existing InsertTextForLua iterates its input byte-wise, so
    // non-ASCII text has always been silently dropped by buffer.
    // insertText -- a real, pre-existing bug, out of scope to fix here
    // (needs its own dedicated verification). InsertTextAt instead
    // splices the given text in as raw bytes (split on '\n', spliced
    // into buf.lines directly, the same technique InsertNewline's own
    // line-splitting already uses), which is correct for UTF-8 by
    // construction and avoids inheriting that bug in new code.
    /**
     * @brief Inserts raw text as UTF-8 bytes into a specific buffer at an explicit position, pushing its own undo entry.
     * @param buffer_id The id of the buffer to edit.
     * @param at The position to insert the text at.
     * @param text The text to insert.
     * @return The cursor position after the inserted text.
     */
    CursorPos InsertTextAt(int buffer_id, CursorPos at, const std::string &text);
    /**
     * @brief Sets the full text of one line in a specific buffer, pushing its own undo entry.
     * @param buffer_id The id of the buffer to edit.
     * @param row The 0-indexed row to set.
     * @param text The new line text.
     */
    void SetLineAt(int buffer_id, int row, const std::string &text);
    /**
     * @brief Replaces a range of lines in a specific buffer, pushing its own undo entry.
     * @param buffer_id The id of the buffer to edit.
     * @param start_row The 0-indexed start row of the range to replace.
     * @param end_row The 0-indexed end row of the range, exclusive.
     * @param lines The replacement lines.
     */
    void ReplaceLinesAt(int buffer_id, int start_row, int end_row, const std::vector<std::string> &lines);
    // ClampCursor()'s logic, generalized to an explicit buffer_id/position
    // instead of CurPane()/mode_ -- deliberately simpler than ClampCursor
    // (no fold-awareness: a background participant's position doesn't
    // interact with fold state the way the local cursor's navigation
    // does; acceptable v1 scope limit, not a correctness bug).
    /**
     * @brief Clamps a position to valid bounds within a specific buffer, without fold-awareness.
     * @param buffer_id The id of the buffer to clamp against.
     * @param pos The position to clamp.
     * @return The clamped position.
     */
    CursorPos ClampPositionInBuffer(int buffer_id, CursorPos pos) const;
    // PushUndo() (editor.cpp), but for an explicit buffer_id instead of
    // Buf() -- Buffer::undo_stack is already per-buffer, so this is a
    // small, low-risk generalization, not a new undo model.
    /**
     * @brief Pushes an undo checkpoint for a specific buffer by id.
     * @param buffer_id The id of the buffer to push an undo checkpoint for.
     */
    void PushUndoForBuffer(int buffer_id);

    // Creates a new empty buffer *without* switching any pane to it (Part
    // VI Phase 27: a dedicated Run/REPL output buffer, populated via
    // SetBufferLinesForLua before the caller splits a pane and switches
    // it into view there).
    /**
     * @brief Creates a new empty buffer without switching any pane to it.
     * @return The new buffer's id.
     */
    int CreateBufferForLua() { return CreateEmptyBuffer(); }
    // Finds-or-creates a buffer for `path` *without* switching any pane to
    // it (same "don't disturb what the human is looking at" contract as
    // CreateBufferForLua above) -- a thin public wrapper around the
    // private FindOrCreateBuffer, for callers (agent_rpc.cpp's default-
    // cursor-position logic, COLLAB_CURSORS_PLAN.md) that need to resolve
    // a file to a buffer_id but must not touch the active pane/cursor.
    /**
     * @brief Finds or creates a buffer for `path` without switching any pane to it.
     * @param path The file path to resolve to a buffer.
     * @return The buffer's id.
     */
    int FindOrCreateBufferForLua(const std::string &path) { return FindOrCreateBuffer(path); }
    /** @brief mep.buffer_delete: public shim over BufferDeleteById. */
    void BufferDeleteForLua(int buffer_id, bool force) { BufferDeleteById(buffer_id, force); }
    /**
     * @brief Returns the number of open buffers.
     * @return The buffer count.
     */
    int BufferCountForLua() const { return static_cast<int>(buffers_.size()); }
    /**
     * @brief Returns a buffer's display label, with "[+]"/"[Terminal] " decoration as applicable.
     * @param buffer_id The id of the buffer to label.
     * @return The display label text.
     */
    std::string BufferLabelForLua(int buffer_id) const;
    // Raw filename (empty for a terminal buffer or an unsaved "[No Name]"
    // buffer) -- unlike BufferLabelForLua, no "[+]"/"[Terminal] " display
    // decoration, so callers needing the real path (e.g. LSP didClose's
    // buffer-closed sweep, kBuiltinLsp) can match it back to their own
    // filename-keyed state.
    /**
     * @brief Returns a buffer's raw filename, with no display decoration (empty for a terminal or unsaved buffer).
     * @param buffer_id The id of the buffer to look up.
     * @return The raw filename.
     */
    std::string BufferFilenameForLua(int buffer_id) const;
    /**
     * @brief Switches the active pane to show a specific buffer.
     * @param buffer_id The id of the buffer to switch to.
     */
    void SwitchToBufferForLua(int buffer_id);
    /**
     * @brief Returns the names of every registered Lua command.
     * @return The list of command names.
     */
    std::vector<std::string> LuaCommandNames() const;
    /**
     * @brief Returns the active pane's cursor position via output parameters.
     * @param row Output parameter set to the cursor's row.
     * @param col Output parameter set to the cursor's column.
     */
    void GetCursorForLua(int *row, int *col) const;
    /**
     * @brief Sets the active pane's cursor position.
     * @param row The row to move the cursor to.
     * @param col The column to move the cursor to.
     */
    void SetCursorForLua(int row, int col);
    /**
     * @brief Inserts text at the cursor in the current buffer, as if typed.
     * @param text The text to insert.
     */
    void InsertTextForLua(const std::string &text);
    // Identical to pressing "c" on the current Visual selection
    // (DispatchVisualKey's own case 'c':) -- deletes it with the same
    // undo/register/cursor semantics as any other change, then leaves
    // mode_ in Insert with the cursor at the deletion point, ready for
    // InsertTextForLua to stream a replacement in. Falls back to
    // ApplyOperatorToSelectionOrCurrentLine's own no-selection behavior
    // ("cc" on the current line) if called outside Visual mode.
    /**
     * @brief Deletes the current Visual selection (as if "c" were pressed) and enters Insert mode at the deletion point, ready for streamed-in replacement text.
     */
    void ChangeVisualSelectionForLua();
    // The Lua equivalent of pressing <Esc>: leaves Insert (or Visual, if
    // still active) and returns to Normal mode. Used to hand control back
    // to Normal once a streamed-in AI replacement (ChangeVisualSelectionForLua
    // + repeated InsertTextForLua calls) finishes, mirroring what a human
    // typing the same replacement by hand would do when done.
    /**
     * @brief Leaves Insert or Visual mode and returns to Normal mode, as if <Esc> were pressed.
     */
    void EnterNormalForLua();
    // The Lua equivalent of pressing "i": pushes one undo checkpoint (same
    // as real Normal-mode 'i', DispatchNormalKey's own `case 'i':`) then
    // enters Insert mode. Used by speech-to-text to make dictated text
    // behave like the user typed it themselves -- one undo step per
    // dictation session -- when the buffer wasn't already in Insert mode
    // at the moment a transcript is ready to stream in.
    /**
     * @brief Pushes an undo checkpoint (as real Normal-mode "i" does) and enters Insert mode, so streamed-in dictated text behaves like the user typed it.
     */
    void EnterInsertForLua();
    /**
     * @brief Reports whether the editor is currently in Insert mode.
     * @return True if the current mode is Insert.
     */
    bool IsInsertModeForLua() const { return mode_ == Mode::Insert; }
    /**
     * @brief Sets the status line message shown to the user.
     * @param msg The message text.
     */
    void SetStatusMessage(const std::string &msg);
    /**
     * @brief Requests that the editor quit at the next opportunity.
     */
    void RequestQuit() { should_quit_ = true; }
    /**
     * @brief Registers a Lua-implemented editor command under a name.
     * @param name The command name.
     * @param lua_ref The Lua registry reference to invoke when the command runs.
     */
    void RegisterLuaCommand(const std::string &name, int lua_ref);
    // `description`: optional (empty = none), from mep.map's opts.desc --
    // recorded in mapping_descriptions_ (private, below) for
    // AllMappingDescriptions() to expose to the help picker.
    /**
     * @brief Registers a Lua-backed key mapping for Normal or Visual mode.
     * @param mode The mode the mapping applies to (only Normal, Visual, and VisualLine are supported; others are ignored).
     * @param key The key sequence to bind.
     * @param lua_ref The Lua registry reference to invoke when the key is pressed.
     * @param description Optional human-readable description shown in the help picker.
     */
    void RegisterLuaMapping(Mode mode, const std::string &key, int lua_ref, const std::string &description = "");
    // Every plain mep.map() binding that was given an opts.desc, for the
    // help picker's keybinding introspection (NVIM_PARITY_PLAN.md Phase
    // 25) -- separate from whichkey's own leader-sequence-only registry.
    struct MappingDescription {
        char mode;  // 'n' or 'v'
        std::string key;
        std::string description;
    };
    /**
     * @brief Returns every plain mep.map() binding that was given an opts.desc, for the help picker's keybinding introspection.
     * @return The list of mode/key/description entries.
     */
    std::vector<MappingDescription> AllMappingDescriptions() const;
    // Binds a single letter key under the mod1 modifier (see SetMod1) to a
    // Lua callback, globally across all modes. `key` is a bare letter
    // ("h") for mod1+letter, or "S-"/"C-" prefixed ("S-h", "C-h") for
    // mod1+Shift+letter / mod1+Ctrl+letter (whichever of Shift/Ctrl isn't
    // already mod1 itself). Overrides any prior mapping for that exact
    // key, including the startup defaults (mod1+v/s/h/j/k/l/S-h/j/k/l/
    // C-h/j/k/l/d).
    /**
     * @brief Binds a single letter key under the mod1 modifier to a Lua callback, globally across all modes, overriding any prior mapping for that exact key.
     * @param key A bare letter for mod1+letter, or "S-"/"C-" prefixed for mod1+Shift+letter / mod1+Ctrl+letter.
     * @param lua_ref The Lua registry reference to invoke when the key is pressed.
     */
    void RegisterMod1Mapping(const std::string &key, int lua_ref);
    // mep.map_g(key, fn): binds a single letter key after a leading "g"
    // in Normal mode (e.g. "d" for "gd") to a Lua callback -- for
    // g-prefixed actions mep's own built-in motions don't already claim
    // (gg/ge/gE/gu/gU/gJ/gv), since a bare mep.map only ever sees a
    // single already-unprefixed keystroke (RegisterLuaMapping's own doc
    // comment) and can't reach anything typed after a pending "g".
    /**
     * @brief Binds a single letter key after a leading "g" in Normal mode to a Lua callback.
     * @param key The letter following "g" (e.g. "d" for "gd").
     * @param lua_ref The Lua registry reference to invoke when the sequence is pressed.
     */
    void RegisterGMapping(const std::string &key, int lua_ref);
    // mep.map_g_visual(key, fn): same as RegisterGMapping, but consulted
    // from DispatchVisualKey's own pending_g_ handling instead -- Visual
    // mode's g-prefix dispatch is a separate function with its own
    // hardcoded gu/gU/gq/gg/ge/gE cases and never looks at g_mappings_
    // (see DispatchVisualKey), so a Normal-mode-only "gl" registered via
    // mep.map_g would silently no-op if typed while a Visual selection is
    // active -- this table is what lets e.g. "gl" replace-selection work
    // in Visual mode without colliding with Normal mode's own "gl".
    /**
     * @brief Binds a single letter key after a leading "g" in Visual mode to a Lua callback, separately from RegisterGMapping's Normal-mode table.
     * @param key The letter following "g" (e.g. "l" for "gl").
     * @param lua_ref The Lua registry reference to invoke when the sequence is pressed.
     */
    void RegisterVisualGMapping(const std::string &key, int lua_ref);
    // mep.map_bracket_prev(key, fn) / mep.map_bracket_next(key, fn):
    // same shape as RegisterGMapping, but for a leading "[" / "]"
    // instead of "g" (e.g. "e" for "[e"/"]e" LSP diagnostic navigation).
    // Kept as two separate tables/entry points (mirroring pending_g_'s
    // own single table) rather than one keyed by "[e"/"]e" strings, so
    // the consuming dispatch doesn't need to reconstruct which bracket
    // was pressed from state that's already been cleared by the time it
    // runs.
    /**
     * @brief Binds a single letter key after a leading "[" to a Lua callback (e.g. LSP diagnostic navigation).
     * @param key The letter following "[".
     * @param lua_ref The Lua registry reference to invoke when the sequence is pressed.
     */
    void RegisterBracketPrevMapping(const std::string &key, int lua_ref);
    /**
     * @brief Binds a single letter key after a leading "]" to a Lua callback (e.g. LSP diagnostic navigation).
     * @param key The letter following "]".
     * @param lua_ref The Lua registry reference to invoke when the sequence is pressed.
     */
    void RegisterBracketNextMapping(const std::string &key, int lua_ref);
    /**
     * @brief Sets which modifier key acts as mod1 for pane/window shortcuts.
     * @param name "alt" (default), "ctrl", "shift", or "super"/"cmd"/"meta".
     */
    void SetMod1(const std::string &name);
    // direction: "left"/"down"/"up"/"right". Moves focus to the pane best
    // positioned that way from the active one (most overlap along the
    // perpendicular axis, then closest); a no-op if there is none -- unless
    // a sidebar is docked on that edge and open, in which case focus steps
    // into it instead (the one focused last time if it's still open on
    // that edge, else the topmost of that edge's stack). Sidebars aren't
    // nodes in the split tree, so this is handled as a special case rather
    // than by extending FindNeighborPaneId's geometry: when called while a
    // sidebar is focused (Mode::Sidebar), the direction pointing back
    // toward the pane content blurs the sidebar (focus returns to whatever
    // pane was active, without closing it -- unlike q/mod1+d); up/down
    // step to the sidebar stacked above/below it in the same left/right
    // dock (see SidebarInstance::stack_share); other directions are a
    // no-op.
    /**
     * @brief Moves focus to the neighboring pane best positioned in a direction, or into a docked sidebar on that edge; a no-op if there is none.
     * @param direction "left"/"down"/"up"/"right".
     */
    void NavigatePaneDirection(const std::string &direction);
    // direction: "left"/"down"/"up"/"right". Moves the split boundary
    // between the active pane and its neighbor that way by `step` (a
    // fraction of the split's extent, kDefaultResizeStep if omitted/<=0).
    // Mirrors mep.nvim's mep.window.panes.resize: grows the active pane if
    // it has a neighbor in that direction, otherwise shrinks it against
    // whatever neighbor it does have on that axis (so opposite keys always
    // do opposite things); a no-op if the active pane is alone on that
    // axis. Walks up past split levels on the wrong axis (e.g. resizing
    // left/right skips over a Horizontal ancestor) to find the nearest one
    // that can actually satisfy the resize. When a sidebar is focused
    // instead of a pane, resizes that sidebar's fixed column/row size by a
    // few cells per call instead (on the axis matching its own dock edge --
    // `step` is ignored there, since it's a split-tree share fraction, not
    // a cell count; the whole dock follows, since same-edge sidebars share
    // one column), while up/down on a left/right-docked sidebar moves the
    // divider between it and the sidebar stacked next to it instead
    // (SetSidebarStackShares), with the same grow-against-next-else-shrink-
    // against-previous convention as panes.
    /**
     * @brief Grows or shrinks the active pane (or a focused sidebar) against its neighbor in a direction.
     * @param direction "left"/"down"/"up"/"right".
     * @param step Fraction of the split's extent to move the boundary by (kDefaultResizeStep if omitted/<=0); ignored for a focused sidebar.
     */
    void ResizeActivePane(const std::string &direction, float step = 0.0f);
    // Sets the active pane's share of its immediate parent split to an
    // absolute fraction (clamped to [kMinPaneShare, 1 - kMinPaneShare])
    // instead of nudging it by a step like ResizeActivePane -- only
    // meaningful when that parent has exactly two children (a no-op
    // otherwise), since a third sibling would leave it ambiguous which one
    // gives up the rest. Used by project_open to give the readme pane ~2/3
    // of the height and the terminal pane the remaining ~1/3. Exposed to
    // Lua as mep.pane_set_share().
    /**
     * @brief Sets the active pane's share of its immediate parent split to an absolute fraction; a no-op unless the parent has exactly two children.
     * @param fraction The desired share, clamped to [kMinPaneShare, 1 - kMinPaneShare].
     */
    void SetActivePaneShare(float fraction);

    // Adds a new full-width pane along the *bottom* of the active tab --
    // vim's `:botright split` -- by re-rooting the tab's split tree under
    // a Horizontal node whose children are [old root, new leaf], rather
    // than splitting the focused pane the way SplitCurrentPane does (which
    // would leave the new pane only as wide as that one pane inside a
    // vsplit). The new leaf shows `buffer_id` and becomes the active pane
    // (Terminal mode if it's a terminal, via SyncModeToActivePaneBuffer);
    // `share` is its fraction of the tab's height. Built for the per-tab
    // terminal toggle (kBuiltinTabTerminal), exposed to Lua as
    // mep.pane_split_bottom(). A no-op for an invalid/deleted buffer id.
    /**
     * @brief Opens a new full-width pane along the bottom of the active tab showing a buffer, and focuses it.
     * @param buffer_id The buffer the new pane shows (a no-op if invalid or deleted).
     * @param share The new pane's fraction of the tab's height, clamped to [kMinPaneShare, 1 - kMinPaneShare].
     */
    void SplitTabBottom(int buffer_id, float share);
    // Lua-facing FocusPaneById: reports whether `pane_id` was a real leaf
    // pane of the active tab (and so got focused) instead of silently
    // doing nothing, so a script can fall back to another target.
    /**
     * @brief Focuses a pane of the active tab by id, for Lua (mep.pane_focus()).
     * @param pane_id The pane to focus.
     * @return True if the pane exists in the active tab and is now focused, false (no-op) otherwise.
     */
    bool FocusPaneByIdForLua(int pane_id);

    // --- Mouse-driven pane/tab interaction (main.cpp's own per-frame hit-
    // testing against pane/border/tab-chip rects calls into these; there's
    // no Lua binding for any of them, this is chrome, not scriptable
    // editing state) ---
    //
    // Makes `pane_id` the active pane within the current tab (a no-op if
    // it isn't a real leaf pane in this tab) -- unlike NavigatePaneDirection,
    // jumps straight to an explicit target instead of walking a direction,
    // since a mouse click/drop already knows exactly which pane it landed
    // on. Used for "click any pane's header/tab chip to focus it" and by
    // the drag-and-drop methods below's own "you're now looking at where
    // you dropped it" convention.
    /**
     * @brief Makes a pane the active pane within the current tab, for mouse-driven focus.
     * @param pane_id The id of the pane to focus (a no-op if it isn't a real leaf pane in this tab).
     */
    void FocusPaneById(int pane_id);
    // Drag-and-drop "drop in the pane's center": moves buffer_id's tab out
    // of source_pane_id's buffer_tabs and into dest_pane_id's, then
    // focuses dest_pane_id. Mirrors PaneMoveBufferTabToNeighbor's exact
    // fixup/ClosePane-if-empty logic (erasing at the tab's own index, not
    // assuming it's the pane's currently-active tab -- a drag can start
    // from any chip, not just the focused one), parameterized by an
    // explicit destination pane id (resolved by mouse position) instead of
    // FindNeighborPaneId's direction-based lookup. Unlike that function,
    // this DOES move focus to the destination -- real drag-and-drop UX,
    // not a keyboard-driven move that deliberately leaves you in place.
    // A no-op if source_pane_id == dest_pane_id, buffer_id isn't actually
    // one of source's tabs, or either pane can't be found.
    /**
     * @brief Drag-and-drop "drop in the pane's center": moves a buffer's tab from one pane to another and focuses the destination.
     * @param source_pane_id The pane the tab is currently in.
     * @param buffer_id The id of the buffer whose tab is being moved.
     * @param dest_pane_id The pane the tab is moved into.
     */
    void MoveBufferTabToPane(int source_pane_id, int buffer_id, int dest_pane_id);
    // Drag-and-drop "drop on an edge": removes buffer_id from
    // source_pane_id (same fixup/ClosePane-if-empty as MoveBufferTabToPane
    // above), then splits dest_pane_id's own leaf node along `dir`
    // (Vertical = left/right, Horizontal = top/bottom -- matches
    // SplitCurrentPane's own axis convention), placing a *new* leaf for
    // buffer_id `before` (left/top) or after (right/bottom) dest's
    // existing content, mirroring SplitCurrentPane's exact node-mutation
    // shape. Focuses the new leaf. A no-op if source_pane_id ==
    // dest_pane_id or either pane can't be found.
    /**
     * @brief Drag-and-drop "drop on an edge": removes a buffer's tab from its source pane and splits the destination pane to hold it in a new leaf, which is focused.
     * @param source_pane_id The pane the tab is currently in.
     * @param buffer_id The id of the buffer whose tab is being moved.
     * @param dest_pane_id The pane whose leaf node is split to hold the new leaf.
     * @param dir The split axis (Vertical = left/right, Horizontal = top/bottom).
     * @param before Whether the new leaf is placed before (left/top) or after (right/bottom) the destination's existing content.
     */
    void SplitPaneWithBufferTab(int source_pane_id, int buffer_id, int dest_pane_id, SplitDir dir, bool before);
    // Drop target for a file dragged out of a sidebar (main.cpp's
    // PaneDragKind::FileDrop): opens `path` in pane `dest_pane_id` (center
    // zone, `split == false`) or in a fresh leaf split off that pane
    // (`dir`/`before` as in SplitPaneWithBufferTab). Goes through LoadFile
    // so images/PDFs/HTML/office files get their viewers exactly as `:e`
    // would; the new pane becomes the active one.
    void OpenFileInPane(int dest_pane_id, const std::string &path, bool split, SplitDir dir, bool before);
    // Border-drag resize: sets node->shares[child_index] to `new_share`
    // (clamped so both it and shares[child_index+1] stay >= kMinPaneShare),
    // taking the difference out of shares[child_index+1] so their combined
    // total is preserved -- the N-ary-sibling-aware analog of
    // SetActivePaneShare (which only ever touches a node with exactly 2
    // children, its own immediate parent). Calls EnsureShares itself, so
    // it's safe to call on a node whose shares aren't populated yet.
    /**
     * @brief Border-drag resize: sets a split child's share to a new value, taking the difference out of the next sibling so their combined total is preserved.
     * @param node The split node being resized.
     * @param child_index The index of the child whose share is being set.
     * @param new_share The desired share, clamped so both it and the next sibling's share stay >= kMinPaneShare.
     */
    void SetPaneBorderShare(SplitNode *node, int child_index, float new_share);
    // Ensures node->shares is populated (EnsureShares) and returns the
    // combined share of children[child_index] and [child_index+1] -- used
    // to capture a border-drag's fixed pair total at drag *start*, before
    // any dragging/SetPaneBorderShare call has happened yet. 0 if
    // child_index is out of range.
    /**
     * @brief Returns the combined share of two adjacent split children, captured at border-drag start before any resizing.
     * @param node The split node being resized.
     * @param child_index The index of the first of the two adjacent children (0 if out of range).
     * @return The pair's combined share.
     */
    float PaneBorderPairTotal(SplitNode *node, int child_index);

    // `args`: same contract as OpenTerminal (empty = interactive shell,
    // non-empty = `shell -c args`), but attaches the terminal to the
    // currently active pane in place instead of splitting -- for callers
    // that already arranged the pane layout themselves (e.g. the built-in
    // project_open's terminal-below-the-readme split, which needs the new
    // pane on the *bottom* rather than SplitCurrentPane's above/left
    // default). Exposed to Lua as mep.terminal_here().
    /**
     * @brief Attaches a terminal to the currently active pane in place, instead of splitting.
     * @param args Same contract as OpenTerminal: empty for an interactive shell, non-empty to run `shell -c args`.
     */
    void OpenTerminalInPlace(const std::string &args);
    // The argv form OpenTerminalInPlace itself builds on: runs `argv`
    // directly (execvp, no `$SHELL -c` in between -- so no shell quoting
    // for callers passing a multi-line argument like a system prompt) in
    // the currently active pane. `title` is what the pane's buffer tab
    // shows; empty falls back to argv[0]. Behind mep.terminal_here_argv.
    /**
     * @brief Attaches a terminal running `argv` directly (no shell wrapper) to the currently active pane in place.
     * @param argv Program and arguments; argv[0] is resolved via PATH.
     * @param title Buffer-tab title; empty uses argv[0].
     */
    void OpenTerminalInPlaceArgv(const std::vector<std::string> &argv, const std::string &title);

    // Also used directly by main.cpp to open a file passed on argv (native
    // builds only -- a no-op with a status message under Emscripten).
    // `force_text`: for an .html/.htm path, opens the plain-text view
    // instead of the rendered :Browse view -- the escape hatch behind the
    // literal `:e`/`:edit` ex-command (RunCommand), which is the only
    // caller that passes true. mep.open (Lua, the "just open this file,
    // default view" primitive every picker/sidebar/LSP jump uses instead
    // of `mep.cmd('e ' .. path)`) always passes false. See
    // ConvertHtmlBufferToText/ConvertTextBufferToHtml's own comments for
    // how an existing buffer for the same path gets reused across a
    // force_text mismatch instead of duplicated.
    /**
     * @brief Opens (or switches to) a buffer for a file path, resetting the pane's cursor/scroll as a fresh open.
     * @param path The file path to open.
     * @param force_text For an .html/.htm path, opens the plain-text view instead of the rendered :Browse view.
     */
    void LoadFile(const std::string &path, bool force_text = false);
    // Bare `:e`/`:e!` (RunCommand) -- re-reads the *current* buffer's own
    // file from disk in place, unlike LoadFile(path), which always opens
    // (or switches to) a buffer for a given path and resets the pane's
    // cursor/scroll as a fresh-open would. `force`: skip the unsaved-
    // changes guard (`:e!`), matching QuitCurrent/NewBuffer's own
    // force/E37 convention. Cursor position is preserved (clamped, in
    // case the file shrank) rather than reset to {0,0} -- reloading is
    // "refresh what's on disk", not "open a new file", and the classic
    // use case (an external tool regenerated this file, or `git checkout`
    // reverted it) is exactly the case where staying roughly in place
    // matters.
    /**
     * @brief Re-reads the current buffer's own file from disk in place, preserving the (clamped) cursor position.
     * @param force Skip the unsaved-changes guard (`:e!`).
     */
    void ReloadCurrentBuffer(bool force);
    // Startup-only cleanup for main()'s own `LoadFile(argv[1])` call:
    // every LoadFile branch acquires a buffer via FindOrCreateBuffer or
    // an OpenXInPlace helper, none of which special-case "there's
    // currently exactly one buffer and it's still the pristine, never-
    // edited one Editor::Editor() made" the way a real `vim file.txt`
    // reuses buffer 1 for the file named on its own command line -- so
    // opening a file this way always left that first buffer behind,
    // empty and reachable only by cycling into it via :bn/:bp. A no-op
    // unless buffer 0 is still exactly that pristine placeholder *and*
    // isn't the active buffer (i.e. LoadFile did create a new one to
    // switch to). Only safe to call this early in startup: it shifts
    // every remaining buffer's id down by one, and the active pane's own
    // buffer_id is the sole reference to any of them at this point (no
    // splits/tabs/terminals/other buffers exist yet to also fix up).
    /**
     * @brief Startup-only cleanup that removes the pristine placeholder buffer 0 left behind after main() opens a file via argv, shifting later buffer ids down by one.
     */
    void DropUnusedInitialBuffer();
    // Returns true on success; false (with a status message set) if the
    // path is empty or the write failed.
    /**
     * @brief Writes the current buffer's content to a file path.
     * @param path The file path to save to.
     * @return True on success; false (with a status message set) if the path is empty or the write failed.
     */
    bool SaveFile(const std::string &path);

    // Directory listing shared by mep.list_dir (Lua, lua_env.cpp -- the
    // file-tree sidebar's data source) and command-line path completion
    // (UpdateCmdlineCompletion): native builds read the real filesystem;
    // wasm builds go through the same loopback bridge as LoadFile/SaveFile
    // (empty when not launched via `just run-wasm`). Entries aren't sorted or
    // filtered -- callers that want directories-first-then-alpha order
    // (both current callers do) sort the result themselves.
    struct DirEntry {
        std::string name;
        bool is_dir;
    };
    /**
     * @brief Lists the entries of a directory, unsorted and unfiltered.
     * @param path The directory path to list.
     * @return The directory's entries (native builds read the real filesystem; wasm builds go through the loopback bridge).
     */
    std::vector<DirEntry> ListDirectory(const std::string &path) const;

    // Persisted project-root bookmark list (mep.projects() picker, Phase
    // 16). Same native-vs-wasm-bridge split as ListDirectory: native reads/
    // writes $XDG_DATA_HOME/mep/projects.json directly; wasm routes through
    // the `just run-wasm` loopback bridge (empty/no-op without one, e.g. a bare
    // browser tab).
    /**
     * @brief Returns the persisted list of bookmarked project-root paths.
     * @return The bookmarked project paths.
     */
    std::vector<std::string> ListProjects() const;
    /**
     * @brief Adds a project-root path to the persisted bookmark list.
     * @param path The project path to add.
     */
    void AddProject(const std::string &path);
    /**
     * @brief Removes a project-root path from the persisted bookmark list.
     * @param path The project path to remove.
     */
    void RemoveProject(const std::string &path);

    // --- Menu-facing API (called from main.cpp's menu bar actions) ---
    /**
     * @brief Undoes the last change, as if "u" were pressed.
     */
    void Undo();
    /**
     * @brief Redoes the last undone change, as if Ctrl-r were pressed.
     */
    void Redo();
    // Copy/Cut the current visual selection, or the current line if not in
    // Visual mode -- the same fallback a GUI menu's Copy/Cut needs when
    // there's no explicit selection.
    /**
     * @brief Copies the current Visual selection, or the current line if not in Visual mode, into the unnamed register.
     */
    void Copy();
    /**
     * @brief Cuts the current Visual selection, or the current line if not in Visual mode, into the unnamed register.
     */
    void Cut();
    /**
     * @brief Pastes the unnamed register's contents at the cursor.
     */
    void Paste();
    // System clipboard bridge (also behind mep.clipboard_get/set in
    // lua_env.cpp). Native builds go through raylib's GLFW clipboard (X11
    // CLIPBOARD selection, Wayland, Win32, Cocoa); the
    // wasm build goes through navigator.clipboard via mep_js_clipboard_*
    // (editor.cpp) and is best-effort there -- reads only see what a
    // paste event or a focus-time readText() managed to cache. Both are
    // no-ops before the window exists (IsWindowReady()), so nothing here
    // ever touches GLFW from a headless context. Reads normalize CRLF to
    // LF; an empty/unavailable clipboard reads as "".
    std::string SystemClipboardRead();
    void SystemClipboardWrite(const std::string &text);
    // Replaces the active pane's buffer with a single empty line. Refuses
    // (with a status message, like :q) if there are unsaved changes.
    /**
     * @brief Replaces the active pane's buffer with a single empty line.
     */
    void NewBuffer();
    // Opens the command line pre-filled with `prefix`, e.g. "e " for a
    // menu's Open action, so the user only has to type the filename.
    /**
     * @brief Opens the command line pre-filled with a prefix, for menu actions that only need the user to type the rest.
     * @param prefix The text to pre-fill the command line with.
     */
    void BeginCommand(const std::string &prefix);
    // Runs a command line as if the user typed ":<cmd>" and pressed Enter.
    /**
     * @brief Runs a command line as if the user typed ":<cmd>" and pressed Enter.
     * @param cmd The command text to run (without the leading colon).
     */
    void RunCommand(const std::string &cmd);

    // :CollabJoin / :CollabLeave. Collaboration follows the active text
    // buffer and merges remote CRDT operations at the next input frame.
    struct CollaborationPeerInfo { std::string id, name; int row = 0, col = 0; bool has_location = false; };
    /**
     * @brief Reports whether a :CollabJoin collaboration session is currently active.
     * @return True if collaboration is active.
     */
    bool CollaborationActive() const;
    /**
     * @brief Returns the remote human peers of the active collaboration session.
     * @return The current collaboration peers.
     */
    std::vector<CollaborationPeerInfo> CollaborationPeers() const;
    // Which buffer :CollabJoin is attached to (-1 if collaboration isn't
    // active) -- CollaborationPeerInfo itself carries no buffer identity
    // since collaboration is single-buffer; Participants() (below) is
    // what attaches this to each human peer for rendering.
    /**
     * @brief Returns the id of the buffer :CollabJoin is attached to.
     * @return The collaboration buffer id, or -1 if collaboration isn't active.
     */
    int CollaborationBufferId() const { return collaboration_buffer_id_; }

    // --- Participants (COLLAB_CURSORS_PLAN.md): a unified view of every
    // remote editor of this project -- human collaborators (:CollabJoin)
    // and connected AI agents (src/agent_rpc.cpp), for rendering a real
    // named/colored cursor per participant plus a tab-bar "who's here"
    // list. Pull-based (recomputed fresh each call, same pattern as
    // CollaborationPeers() itself, which this wraps for the Human half) --
    // no persisted list on Editor, nothing to keep push-synchronized.
    enum class ParticipantKind { Human, Agent };
    struct ParticipantInfo {
        std::string id, name;
        ParticipantKind kind = ParticipantKind::Human;
        int buffer_id = -1;
        int row = 0, col = 0;
        bool has_location = false;
        // Agent-only (always "" for a Human participant) -- "idle" |
        // "thinking" | "writing" | "awaiting_input" | "done", or "" if
        // the agent has never reported one. See agent_rpc.h's
        // AgentParticipant::status for where this comes from.
        std::string status;
        // Agent-only: the `:terminal` buffer the agent runs inside, -1 if
        // unknown (see AgentParticipant::terminal_buffer_id).
        int terminal_buffer_id = -1;
    };
    /**
     * @brief Returns a unified, freshly-recomputed view of every remote editor of this project: human collaborators and connected AI agents.
     * @return The current participants.
     */
    std::vector<ParticipantInfo> Participants() const;
    // Registers/updates a synthetic, locally-driven participant -- for a
    // Lua-side feature (e.g. mep.ai_send_buffer's streaming) that wants the
    // same tab-bar chip + in-buffer robot cursor a real socket-connected
    // mep-agent gets "for free", without actually going through
    // agent_rpc.cpp. Upserts by `id`; call again each streamed delta to
    // move the cursor. `status` follows the same agent status vocabulary
    // ParticipantInfo::status documents ("thinking"/"writing"/etc, or "").
    /**
     * @brief Registers or updates a synthetic, locally-driven participant, giving a Lua-side streaming feature its own tab-bar chip and in-buffer cursor.
     * @param id The participant's unique id (upserted by this key).
     * @param name The participant's display name.
     * @param buffer_id The id of the buffer the participant's cursor is in.
     * @param row The participant's cursor row.
     * @param col The participant's cursor column.
     * @param status The agent status vocabulary ("thinking"/"writing"/etc.), or "".
     */
    void SetLocalParticipant(const std::string &id, const std::string &name, int buffer_id, int row, int col, const std::string &status = "");
    // Removes a local participant added via SetLocalParticipant (e.g. once
    // a stream finishes or is cancelled) -- a no-op if `id` isn't one.
    /**
     * @brief Removes a local participant added via SetLocalParticipant.
     * @param id The id of the participant to remove (a no-op if `id` isn't one).
     */
    void ClearLocalParticipant(const std::string &id);
    // Moves the local user's own cursor to participant `id`'s last-known
    // position -- switching the active pane's buffer first if the
    // participant is somewhere else (an AI agent, unlike a human
    // collaborator, can be positioned in any buffer, not just the single
    // one :CollabJoin ever attaches to). Supersedes the old, narrower
    // JumpToCollaborator (single-buffer-only, no buffer-switch), which
    // this replaces -- click handlers for both the tab-bar participant
    // chips and the status-line collaboration chips now go through this
    // one function. Returns false (status_message_ explains why) if `id`
    // isn't a current participant or has no known location yet.
    /**
     * @brief Moves the local user's own cursor to a participant's last-known position, switching the active pane's buffer first if needed.
     * @param id The id of the participant to jump to.
     * @return False (with status_message_ explaining why) if `id` isn't a current participant or has no known location yet; true otherwise.
     */
    bool JumpToParticipant(const std::string &id);

    // --- Modal overlays (NVIM_PARITY_PLAN.md Part I Phase 3) ---
    // vim.ui.input/vim.ui.select/confirm-dialog equivalents: each takes
    // over input until confirmed/cancelled, restores whatever mode was
    // active before it opened, and invokes `on_done_ref` (a Lua function
    // registered via luaL_ref) exactly once, then unrefs it. Only one
    // overlay may be active at a time (mode_ already enforces that).
    //   Prompt:  on_done(text) on Enter, on_done() [nil] on Escape.
    //   Confirm: on_done(true/false) always (Escape counts as false).
    //   Select:  on_done(1-indexed index) on Enter, on_done() [nil] on Escape.
    /**
     * @brief Opens a vim.ui.input-equivalent modal text prompt, taking over input until confirmed or cancelled.
     * @param title The prompt's title text.
     * @param default_text The initial text pre-filled in the input.
     * @param on_done_ref A Lua function ref, called with the entered text on Enter or with nil on Escape, then unrefed.
     * @param masked Whether to render the input masked (e.g. for a password).
     */
    void BeginPrompt(const std::string &title, const std::string &default_text, int on_done_ref,
                      bool masked = false);
    // Same as BeginPrompt, but for a native C++ caller (a toolbar button's
    // own click handler in main.cpp, not Lua) that has no Lua function ref
    // to hand it -- HandlePromptInput calls `on_done` directly instead of
    // through lua_->CallRefWithString when this is set, and does NOT call
    // it on Escape (cancel), matching BeginPrompt's own on_done() case
    // simply being skipped rather than called with an empty string.
    /**
     * @brief Same as BeginPrompt, but for a native C++ caller with a std::function callback instead of a Lua ref; not invoked on Escape.
     * @param title The prompt's title text.
     * @param default_text The initial text pre-filled in the input.
     * @param on_done Callback invoked with the entered text on Enter.
     */
    void BeginPromptNative(const std::string &title, const std::string &default_text,
                            std::function<void(const std::string &)> on_done);
    /**
     * @brief Opens a confirm-dialog-equivalent modal, taking over input until answered.
     * @param message The confirmation message text.
     * @param default_yes The default answer highlighted/used.
     * @param on_done_ref A Lua function ref, called with true/false (Escape counts as false), then unrefed.
     */
    void BeginConfirm(const std::string &message, bool default_yes, int on_done_ref);
    /**
     * @brief Opens a vim.ui.select-equivalent modal item picker, taking over input until confirmed or cancelled.
     * @param title The picker's title text.
     * @param items The selectable item labels.
     * @param on_done_ref A Lua function ref, called with the 1-indexed chosen index on Enter or with nil on Escape, then unrefed.
     */
    void BeginSelect(const std::string &title, std::vector<std::string> items, int on_done_ref);
    // Preview: no callback -- purely informational (e.g. git-gutter's
    // hunk preview), dismissed by any keypress or a click, restoring
    // whatever mode was active before it opened. `text` may contain
    // embedded '\n's; the renderer splits and draws one line each.
    /**
     * @brief Opens a purely informational modal overlay (e.g. a git-gutter hunk preview), dismissed by any keypress or click.
     * @param title The preview's title text.
     * @param text The preview text, split on embedded '\n's for rendering.
     */
    void BeginPreview(const std::string &title, const std::string &text);

    // Read access for main.cpp's renderer.
    /**
     * @brief Returns the active prompt overlay's title text.
     * @return The prompt title.
     */
    const std::string &PromptTitle() const { return prompt_title_; }
    /**
     * @brief Returns the active prompt overlay's current input text.
     * @return The prompt input text.
     */
    const std::string &PromptInput() const { return prompt_input_; }
    /**
     * @brief Reports whether the active prompt overlay renders its input masked.
     * @return True if the input is masked.
     */
    bool PromptMasked() const { return prompt_masked_; }
    /**
     * @brief Returns the active confirm overlay's message text.
     * @return The confirm message.
     */
    const std::string &ConfirmMessage() const { return confirm_message_; }
    /**
     * @brief Returns the active confirm overlay's default answer.
     * @return True if the default answer is yes.
     */
    bool ConfirmDefaultYes() const { return confirm_default_yes_; }
    /**
     * @brief Returns the active select overlay's title text.
     * @return The select title.
     */
    const std::string &SelectTitle() const { return select_title_; }
    /**
     * @brief Returns the active select overlay's item labels.
     * @return The select items.
     */
    const std::vector<std::string> &SelectItems() const { return select_items_; }
    /**
     * @brief Returns the active select overlay's currently highlighted item index.
     * @return The 0-indexed selected item index.
     */
    int SelectIndex() const { return select_index_; }
    /**
     * @brief Returns the active preview overlay's title text.
     * @return The preview title.
     */
    const std::string &PreviewTitle() const { return preview_title_; }
    /**
     * @brief Returns the active preview overlay's text.
     * @return The preview text.
     */
    const std::string &PreviewText() const { return preview_text_; }

    // --- Theme engine (NVIM_PARITY_PLAN.md Part II Phase 9) ---
    // Applies a registered palette by name; false (no-op) if `name` isn't
    // registered. main.cpp's ResolveHlGroup() reads the resulting group
    // map -- everything colored through a named highlight group repaints
    // automatically on the next frame, nothing needs to be told. The one
    // exception is content baked into a cached GPU texture at draw time
    // (the PDF viewer's theme-colored recoloring, see ThemedPdfChannel in
    // main.cpp) -- that can't "just repaint" from a color lookup alone, so
    // ApplyTheme also bumps ThemeEpoch() for exactly that kind of cache to
    // key its own invalidation off of.
    /**
     * @brief Applies a registered theme palette by name, bumping ThemeEpoch() so caches that bake in resolved colors can invalidate.
     * @param name The registered theme's name.
     * @return True if applied; false (a no-op) if `name` isn't registered.
     */
    bool ApplyTheme(const std::string &name);
    /**
     * @brief Returns the currently applied theme's name.
     * @return The current theme name.
     */
    const std::string &CurrentThemeName() const { return current_theme_name_; }
    /**
     * @brief Returns the names of every registered theme.
     * @return The list of theme names.
     */
    std::vector<std::string> ThemeNames() const;
    // Bumped every time ApplyTheme actually changes current_theme_groups_.
    // Exists for caches that bake a ResolveHighlight-derived color into
    // something more expensive to redo than a per-frame redraw (currently
    // just the PDF viewer's recolored page textures) -- comparing this
    // catches a theme change even when nothing else about the cached
    // content changed, which comparing only *that* content's own version
    // (e.g. a PDF page's raster generation) would miss entirely.
    /**
     * @brief Returns a counter bumped every time ApplyTheme actually changes the active theme, for caches that bake in resolved colors to key invalidation off of.
     * @return The current theme epoch.
     */
    int ThemeEpoch() const { return theme_epoch_; }
    // Exact-name lookup into the active theme's built group map; returns
    // false if `name` isn't a known group (caller decides the fallback --
    // main.cpp's ResolveHlGroup falls back to its substring heuristic so
    // ad hoc decoration hl_group names keep working un-migrated).
    /**
     * @brief Looks up a highlight group's color in the active theme by exact name.
     * @param name The highlight group name.
     * @param out Output parameter set to the resolved color on success.
     * @return True if `name` is a known group; false otherwise (caller decides the fallback).
     */
    bool ResolveHighlight(const std::string &name, ThemeColor *out) const;
    // Raw palette lookup by theme name (not necessarily the active theme) --
    // for previews that want to show a theme's colors without switching the
    // whole app to it, e.g. the colorscheme picker's swatch preview. Returns
    // false if `name` isn't a registered theme.
    /**
     * @brief Looks up a registered theme's raw palette by name, without switching the active theme.
     * @param name The theme's name.
     * @param out Output parameter set to the theme's palette on success.
     * @return True if `name` is a registered theme; false otherwise.
     */
    bool ThemePalette(const std::string &name, Palette *out) const;
    // Resolves a VTermCell's fg color to a concrete ThemeColor -- shared by
    // main.cpp's VTermColorToRaylib (the live :terminal pane's own per-
    // frame render, which just wraps this and converts to a raylib Color)
    // and EnterTerminalNormalMode (which needs the exact same resolution
    // once, up front, to snapshot into Decorations that outlive the live
    // TerminalSession's own per-cell data). Defined here rather than
    // vterm.h/.cpp since it needs ResolveHighlight for the Default case
    // (defer to mep's own active theme) -- vterm.h itself stays free of
    // any such dependency, per its own header comment.
    /**
     * @brief Resolves a VTermColor (foreground or background) to a concrete ThemeColor, deferring to the active theme for the Default case.
     * @param c The terminal color to resolve.
     * @param is_fg Whether `c` is a foreground color (as opposed to background).
     * @return The resolved theme color.
     */
    ThemeColor ResolveVTermColor(const VTermColor &c, bool is_fg) const;

    // --- Decorations (NVIM_PARITY_PLAN.md Part I Phase 4) ---
    // Returns a stable id for `name`, creating one on first use (mirrors
    // nvim_create_namespace: idempotent by name).
    /**
     * @brief Returns a stable namespace id for a name, creating one on first use.
     * @param name The namespace name.
     * @return The namespace's id (idempotent by name, mirrors nvim_create_namespace).
     */
    int CreateNamespace(const std::string &name);
    // Clears/adds operate on the *current* buffer -- the common case for
    // every planned consumer (colorizer, todoscan, git gutter, ...), which
    // all recompute against whatever buffer they're attached to.
    /**
     * @brief Clears every decoration in a namespace within the current buffer.
     * @param ns The namespace id to clear.
     */
    void ClearNamespace(int ns);
    /**
     * @brief Adds a decoration to a namespace within the current buffer.
     * @param ns The namespace id to add to.
     * @param deco The decoration to add.
     * @return The added decoration's id.
     */
    int AddDecoration(int ns, Decoration deco);
    // Flattened view across every namespace in the current buffer, for
    // main.cpp's per-line rendering pass.
    /**
     * @brief Returns a flattened view across every namespace's decorations in the current buffer.
     * @return The current buffer's decorations, keyed by namespace id.
     */
    const std::unordered_map<int, std::vector<Decoration>> &CurrentBufferDecorations() const;
    /**
     * @brief Clears every decoration in a namespace within a specific buffer.
     * @param buffer_id The id of the buffer to clear decorations in.
     * @param ns The namespace id to clear.
     */
    void ClearNamespaceInBuffer(int buffer_id, int ns);
    /**
     * @brief Adds a decoration to a namespace within a specific buffer.
     * @param buffer_id The id of the buffer to add the decoration to.
     * @param ns The namespace id to add to.
     * @param deco The decoration to add.
     * @return The added decoration's id.
     */
    int AddDecorationToBuffer(int buffer_id, int ns, Decoration deco);

    // --- Folding (NVIM_PARITY_PLAN.md Part I Phase 5) ---
    // za-equivalent: toggles the innermost fold containing the cursor's
    // row, if any.
    /**
     * @brief Toggles the innermost fold containing the cursor's row, if any (za-equivalent).
     */
    void ToggleFoldAtCursor();
    // Toggles the innermost fold containing `row` directly, without moving
    // the cursor there first -- used by the gutter fold-marker click
    // dispatch (main.cpp's DrawPane), which knows the clicked buffer row
    // but shouldn't relocate the cursor just to toggle a fold near it.
    /**
     * @brief Toggles the innermost fold containing a row, without moving the cursor there first.
     * @param row The row to toggle the fold at.
     */
    void ToggleFoldAtRow(int row);
    /**
     * @brief Creates a new fold over a row range in the current buffer.
     * @param start_row The fold's start row.
     * @param end_row The fold's end row.
     * @param closed Whether the fold starts closed.
     * @param provider Tag identifying what created the fold, for later bulk-clearing via ClearFoldsFromProvider.
     */
    void CreateFold(int start_row, int end_row, bool closed, const std::string &provider = "manual");
    // Removes every fold tagged with `provider` (a provider recomputing
    // its folds calls this before re-adding, mirroring the decoration
    // namespace clear-and-replace pattern).
    /**
     * @brief Removes every fold tagged with a given provider.
     * @param provider The provider tag to clear folds for.
     */
    void ClearFoldsFromProvider(const std::string &provider);
    // True (and *fold_start_row set) if `row` is hidden inside a closed
    // fold -- i.e. inside one but not that fold's own start row, which
    // stays visible as the fold's summary line.
    /**
     * @brief Reports whether a row is hidden inside a closed fold.
     * @param row The row to check.
     * @param fold_start_row Output parameter set to the containing fold's start row when hidden.
     * @return True if `row` is hidden inside a closed fold.
     */
    bool IsRowHiddenByFold(int row, int *fold_start_row) const;
    /**
     * @brief Returns the current buffer's folds.
     * @return The current buffer's fold list.
     */
    const std::vector<Fold> &CurrentBufferFolds() const { return Buf().folds; }
    // j/k (ResolveMotion) step by *displayed* lines, not buffer rows: a
    // closed fold's hidden interior counts as a single line no matter how
    // many rows it spans. `dir` is +1 (down) or -1 (up).
    /**
     * @brief Steps a row by one displayed line, treating a closed fold's hidden interior as a single line.
     * @param row The starting row.
     * @param dir The step direction: +1 (down) or -1 (up).
     * @return The resulting row.
     */
    int StepVisibleRow(int row, int dir) const;

    // Org-mode headline folding (za/zm/zr/zR/zM): rebuilds the current
    // buffer's provider="org" folds from its `*`/`**`/... headline
    // structure. Cheap enough to call before every z-command rather than
    // hook every edit site -- see the .cpp definition for the exact
    // matching rule that preserves open/closed state across a recompute.
    /**
     * @brief Rebuilds the current buffer's provider="org" folds from its headline structure, preserving open/closed state across the recompute.
     */
    void RecomputeOrgFolds();
    /**
     * @brief Reports whether the current buffer is an org-mode buffer.
     * @return True if the current buffer is org-mode.
     */
    bool IsOrgBuffer() const;
    // Marker folding (za/zm/zr/zR/zM), enabled by default for every
    // filetype: rebuilds provider="marker" folds from literal `{{{`/`}}}`
    // text markers (vim's classic foldmethod=marker), same lazy
    // recompute-before-every-z-command pattern as RecomputeOrgFolds above
    // -- see the .cpp definition for the nesting/matching rule.
    /**
     * @brief Rebuilds the current buffer's provider="marker" folds from literal `{{{`/`}}}` text markers.
     */
    void RecomputeMarkerFolds();
    // zM/zR: force every fold in the current buffer open or closed, and
    // snap fold_level to the corresponding extreme (0 or the deepest
    // nesting present).
    /**
     * @brief Forces every fold in the current buffer open or closed, snapping fold_level to the corresponding extreme.
     * @param closed Whether to close (true) or open (false) every fold.
     */
    void SetAllFoldsClosed(bool closed);
    // zm/zr: vim's "one level more/less" fold stepping. `delta` is +1
    // (zr, open a level) or -1 (zm, close a level); see Buffer::fold_level
    // for what the stored level means between calls.
    /**
     * @brief Steps the current buffer's fold level by one level more/less open.
     * @param delta +1 to open a level (zr) or -1 to close a level (zm).
     */
    void AdjustFoldLevel(int delta);

    // --- Org inline images (<leader>oti / mep.org_images_toggle) ---
    // Registers/replaces the resolved image path for `row` in the current
    // buffer's org_image_rows -- called once per match by Lua's
    // mep.org_image_scan() (kBuiltinOrgImages, main.cpp), mirroring
    // CreateFold's "one call per range" shape rather than taking a whole
    // replacement map at once.
    /**
     * @brief Registers or replaces the resolved image path for a row in the current buffer's org_image_rows.
     * @param row The row the image reference is on.
     * @param path The resolved image file path.
     */
    void SetOrgImageRow(int row, const std::string &path);
    // Clears every entry -- called by mep.org_image_scan() before
    // rescanning, mirroring ClearFoldsFromProvider's clear-and-replace
    // pattern (there's only ever one "provider" of these, so no provider
    // tag is needed the way folds have one).
    /**
     * @brief Clears every registered org inline-image row entry in the current buffer.
     */
    void ClearOrgImageRows();
    // <leader>oti: flips org_images_visible_ and returns the new state (so
    // the Lua-side wrapper can notify the user and, if newly visible,
    // trigger an immediate mep.org_image_scan() rather than waiting for
    // the next debounced buffer-changed tick).
    /**
     * @brief Toggles org inline-image rendering visibility.
     * @return The new visibility state.
     */
    bool ToggleOrgImages();

    // --- Org LaTeX/math-mode rendering (<leader>otl / mep.org_latex_toggle) ---
    // Registers/replaces the rendered-PNG path, slot count, and last raw
    // source row (see Buffer::OrgLatexRender) for `row` -- called once per
    // fragment by Lua's mep.org_latex_scan() (kBuiltinOrgLatex, main.cpp),
    // mirroring SetOrgImageRow's "one call per match" shape. `end_row ==
    // row` for a single-line fragment.
    /**
     * @brief Registers or replaces the rendered LaTeX PNG for a source row range in the current buffer.
     * @param row The fragment's start row.
     * @param path The rendered PNG file path.
     * @param slots The rendered image's slot count (Buffer::OrgLatexRender).
     * @param end_row The fragment's end row (equal to `row` for a single-line fragment).
     */
    void SetOrgLatexRow(int row, const std::string &path, int slots, int end_row);
    // Clears every entry -- called by mep.org_latex_scan() before
    // rescanning (and when the toggle turns off), mirroring
    // ClearOrgImageRows.
    /**
     * @brief Clears every registered org LaTeX render entry in the current buffer.
     */
    void ClearOrgLatexRows();
    // <leader>otl: flips org_latex_visible_ and returns the new state, same
    // shape as ToggleOrgImages.
    /**
     * @brief Toggles org LaTeX/math-mode rendering visibility.
     * @return The new visibility state.
     */
    bool ToggleOrgLatex();
    // Appends one inline-math span (see Buffer::OrgLatexInlineSpan) for
    // `row` -- called once per match by mep_org_latex_register_inline
    // (kBuiltinOrgLatex). Appends rather than replaces (unlike
    // SetOrgLatexRow) since a single row can carry more than one inline
    // fragment ("$x$ and $y$ are related").
    /**
     * @brief Appends one inline-math LaTeX span for a row in the current buffer (a row may carry more than one).
     * @param row The row the inline fragment is on.
     * @param col_start The fragment's start column.
     * @param col_end The fragment's end column.
     * @param path The rendered PNG file path.
     */
    void AddOrgLatexInlineSpan(int row, int col_start, int col_end, const std::string &path);
    // Clears every entry -- called by mep.org_latex_scan() before
    // rescanning (and when the toggle turns off).
    /**
     * @brief Clears every registered org LaTeX inline-math span in the current buffer.
     */
    void ClearOrgLatexInlineSpans();

    // --- Sidebar/panel widget (NVIM_PARITY_PLAN.md Part I Phase 7) ---
    /**
     * @brief Creates a new sidebar panel widget.
     * @param title The sidebar's title text.
     * @param position The dock edge to attach the sidebar to.
     * @param size The sidebar's fixed size in cells along its dock axis.
     * @return The new sidebar's id.
     */
    int CreateSidebar(const std::string &title, const std::string &position, int size);
    /**
     * @brief Sets a sidebar's section/widget content, replacing whatever it had.
     * @param id The id of the sidebar to update.
     * @param sections The new section content.
     */
    void SetSidebarSections(int id, std::vector<SidebarSection> sections);
    /**
     * @brief Opens a sidebar, optionally focusing it.
     * @param id The id of the sidebar to open.
     * @param focus Whether to also move focus into the sidebar.
     */
    void OpenSidebar(int id, bool focus);
    /**
     * @brief Closes a sidebar.
     * @param id The id of the sidebar to close.
     */
    void CloseSidebar(int id);
    /**
     * @brief Toggles a sidebar open or closed, optionally focusing it when opened.
     * @param id The id of the sidebar to toggle.
     * @param focus Whether to focus the sidebar when it becomes open.
     */
    void ToggleSidebar(int id, bool focus);
    /**
     * @brief Reports whether a sidebar is currently open.
     * @param id The id of the sidebar to check.
     * @return True if the sidebar is open.
     */
    bool IsSidebarOpen(int id) const;
    // Dock helpers for the "same edge => one merged column" layout (see
    // SidebarInstance::stack_share): the open sidebars docked on an edge in
    // registration order (= top-to-bottom stacking order for left/right),
    // and the column size that edge's dock renders at -- the largest
    // `size` among its open members (SetSidebarSize keeps them equal, so
    // in practice they agree; the max is just the tie-break when a
    // sidebar created with a different size is opened into an existing
    // dock and hasn't been resized yet).
    /**
     * @brief Lists the open sidebars docked on an edge, in registration (stacking) order.
     * @param position "left"/"right"/"top"/"bottom".
     * @return The open sidebars' ids, top-to-bottom for a left/right dock.
     */
    std::vector<int> OpenSidebarIdsOn(const std::string &position) const;
    /**
     * @brief Returns the cell size the merged dock on an edge renders at.
     * @param position "left"/"right"/"top"/"bottom".
     * @return The largest `size` among the edge's open sidebars, or 0 if none is open.
     */
    int DockSize(const std::string &position) const;
    /**
     * @brief Splits the combined height of two vertically adjacent stacked sidebars between them.
     * @param upper_id The sidebar drawn above the divider.
     * @param lower_id The sidebar drawn below it.
     * @param upper_fraction The upper sidebar's share of the pair's combined height, clamped to [0.1, 0.9].
     */
    void SetSidebarStackShares(int upper_id, int lower_id, float upper_fraction);
    // Reorders a left/right dock's vertical stack: swaps `id` with the
    // open sidebar stacked directly above ("up") or below ("down") it on
    // the same edge. Stacking order IS registration order in sidebars_
    // (OpenSidebarIdsOn/DrawSidebars both walk it top-to-bottom), so this
    // swaps the two SidebarInstance entries in place -- every consumer
    // that derives layout from that order (the renderer, mod1+j/k stack
    // navigation, mod1+Shift+j/k divider resizing) follows automatically
    // with no separate order field to keep in sync. Each sidebar keeps
    // its own stack_share, so a taller sidebar stays taller after the
    // swap. Focus is untouched: a focused sidebar stays focused (with the
    // same cursor row) at its new slot, which is what mod1+Ctrl+j/k rely
    // on (PaneMoveBufferTabToNeighbor's Mode::Sidebar branch). A no-op for
    // an unknown/closed id, a top/bottom-docked sidebar, at either end of
    // the stack, or for a non-vertical direction.
    /**
     * @brief Swaps a sidebar with its neighbor above or below in the same left/right dock stack; a no-op if there is none.
     * @param id The sidebar to move.
     * @param direction "up" or "down".
     * @return True if the stack order changed.
     */
    bool SwapSidebarInStack(int id, const std::string &direction);
    /**
     * @brief Returns every currently registered sidebar instance.
     * @return The sidebar instances.
     */
    const std::vector<SidebarInstance> &Sidebars() const { return sidebars_; }
    /**
     * @brief Returns the id of the currently focused sidebar.
     * @return The focused sidebar's id, or 0 if none is focused.
     */
    int FocusedSidebarId() const { return focused_sidebar_id_; }
    /**
     * @brief Returns the currently focused sidebar's cursor row.
     * @return The sidebar cursor position.
     */
    int SidebarCursor() const { return sidebar_cursor_; }
    // Section-header/widget rows in display order -- one section header
    // line, then (if not collapsed) one line per widget, repeated per
    // section. Returns {} for an unknown id.
    /**
     * @brief Flattens a sidebar's sections/widgets into display-order rows (one section header line, then one per widget if not collapsed).
     * @param id The id of the sidebar to flatten.
     * @return The sidebar's display rows, or {} for an unknown id.
     */
    std::vector<SidebarLine> FlattenSidebar(int id) const;
    /**
     * @brief Looks up a sidebar instance by id.
     * @param id The id of the sidebar to look up.
     * @return The sidebar instance, or nullptr if `id` is unknown.
     */
    const SidebarInstance *FindSidebar(int id) const;
    /**
     * @brief Registers a Lua callback for keypresses while a sidebar is focused.
     * @param id The id of the sidebar to register the callback on.
     * @param lua_ref The Lua registry reference to invoke on a keypress.
     */
    void SetSidebarOnKey(int id, int lua_ref);
    /**
     * @brief Registers a Lua callback that supplies the popout preview for a sidebar (see SidebarInstance::on_preview_ref).
     * @param id The id of the sidebar to register the callback on.
     * @param lua_ref The Lua registry reference to invoke with the cursor widget's id whenever it changes while popped out.
     */
    void SetSidebarOnPreview(int id, int lua_ref);
    /** @brief Sets a sidebar's tab strip (see SidebarInstance::tabs); `active` is 0-based and clamped. */
    void SetSidebarTabs(int id, std::vector<std::string> tabs, int active);
    /** @brief Registers the Lua callback fired with the new 1-based index whenever a sidebar's active tab changes. */
    void SetSidebarOnTab(int id, int lua_ref);
    /** @brief Switches sidebar `id` to tab `index` (0-based, wrapped), resets its cursor/scroll and fires on_tab_ref; a no-op without tabs. */
    void SelectSidebarTab(int id, int index);
    int SidebarActiveTab(int id) const;

    // --- Floating editable pane (mep.float_open) ---
    // A real Pane hosting an ordinary buffer, drawn centered over
    // everything (main.cpp's DrawFloatPane) and holding the cursor while
    // open: CurPane() resolves to it, so every Normal/Insert/Visual/
    // Command path (and the status line, :w, the mouse wheel) works on it
    // exactly as on a docked pane. It is not part of any tab's split tree
    // -- the tab's own active_pane_id is left alone underneath -- so
    // closing it (Escape with nothing pending, :q/:close/:wq, the
    // header's x, a click outside the box) simply drops the node and
    // returns focus to wherever it came from: the sidebar row it was
    // opened from (the Todo panel's 'e') or the tab's active pane. Any
    // pane-tree operation (split, pane navigation, buffer delete) closes
    // it first rather than acting "through" it. First consumer: the Todo
    // sidebar's 'e', which opens TODO.org itself at the headline instead
    // of a one-line title prompt.
    /**
     * @brief Opens `path` in a floating pane with the cursor on `row` (0-based), replacing any open float.
     * @param save_on_close Whether closing the float writes the buffer if it was modified.
     * @param on_close_ref Lua function ref called once on close with `true` if the buffer was written by that close (0 = none); released afterwards.
     * @return true if the float opened.
     */
    bool OpenFloatPane(const std::string &path, int row, bool save_on_close, int on_close_ref = 0);
    /**
     * @brief Closes the floating pane, restoring the prior focus.
     * @param force_write When true, writes the buffer unconditionally (ignoring save_on_close and whether it was
     * modified) -- the ZZ "confirm" chord (DispatchNormalKey) uses this so a float opened with save_on_close=false
     * (an implicit dismiss like Escape/:q/clicking away discards) can still be explicitly confirmed. Otherwise
     * writes only if save_on_close was set and the buffer was modified, same as before this param existed.
     */
    void CloseFloatPane(bool force_write = false);
    bool IsFloatPaneOpen() const { return float_node_ != nullptr; }
    const Pane &FloatPane() const { return float_node_->pane; }
    int FloatPaneId() const { return float_node_ ? float_node_->pane.id : -1; }
    /** @brief Closes the float if the tab/workspace it was opened over is no longer active or its buffer was deleted. */
    void ValidateFloatPane();

    // --- Sidebar popout (mod1+m) ---
    // A focused sidebar can be "popped out" into a large centered float
    // (main.cpp's DrawSidebarPopout, sized like the picker overlay): the
    // same flattened rows on the left, driven by the very same
    // HandleSidebarInput/FocusSidebarRow/ActivateSidebarLine paths as the
    // docked panel (so every consumer's on_key bindings keep working
    // untouched), plus a preview column on the right fed by the sidebar's
    // on_preview_ref. The docked panel stays where it is underneath (the
    // pane layout doesn't reflow for what's meant to be a temporary
    // zoom); Escape/q/mod1+m/a click outside the box collapse the popout
    // back to it, and anything that takes focus away from the sidebar
    // (mod1+hjkl into the panes, opening a file, CloseSidebar) implicitly
    // ends it -- SidebarPopoutActive() is false the moment the popped-out
    // sidebar is no longer the focused one.
    /**
     * @brief Pops the focused sidebar out into the large centered float, or collapses it back if it already is.
     * @param id The sidebar to toggle, or 0 for whichever sidebar currently has focus; a no-op unless that sidebar is focused.
     */
    void ToggleSidebarPopout(int id = 0);
    /**
     * @brief Pops a sidebar out into the large centered float; a no-op unless it is the focused sidebar.
     * @param id The id of the sidebar to pop out.
     */
    void OpenSidebarPopout(int id);
    /**
     * @brief Collapses the popped-out sidebar (if any) back to its docked panel, keeping its focus and cursor.
     */
    void CloseSidebarPopout();
    /**
     * @brief Reports whether a sidebar popout is currently showing.
     * @return True if a sidebar is popped out and still has input focus.
     */
    bool SidebarPopoutActive() const {
        return sidebar_popout_id_ != 0 && mode_ == Mode::Sidebar && focused_sidebar_id_ == sidebar_popout_id_;
    }
    /**
     * @brief Returns the id of the popped-out sidebar.
     * @return The sidebar id, or 0 if none is popped out.
     */
    int SidebarPopoutId() const { return sidebar_popout_id_; }
    // Fires the popped-out sidebar's on_preview_ref if the widget under
    // its cursor changed since the last call (or its sections were
    // replaced, or the popout just opened). Called once per frame by
    // DrawSidebarPopout, the same call-shape as UpdateScrollForSidebar --
    // a per-frame diff rather than hooks on every cursor-moving path
    // (keys, wheel, clicks, SetSidebarSections re-populating the rows), so
    // none of those can forget to refresh it.
    /**
     * @brief Re-requests the popout preview from the sidebar's on_preview callback if the cursor's widget changed.
     */
    void RefreshSidebarPopoutPreview();
    /**
     * @brief Sets the text shown in the popout's preview column (mep.sidebar_set_preview).
     * @param text The preview text ("" hides the column's content); '\n'-separated lines.
     * @param spans Optional per-span highlight groups, same shape as the picker preview's.
     * @param title Optional caption drawn above the preview (a path, a diff command, ...).
     * @param current_row Optional 0-indexed preview line to tint as "the row this widget refers to", or -1 for none.
     */
    void SetSidebarPopoutPreview(const std::string &text, std::vector<PickerHlSpan> spans = {},
                                 const std::string &title = "", int current_row = -1) {
        sidebar_popout_preview_text_ = text;
        sidebar_popout_preview_spans_ = std::move(spans);
        sidebar_popout_preview_title_ = title;
        sidebar_popout_preview_current_row_ = current_row;
        sidebar_popout_preview_scroll_ = 0;
    }
    const std::string &SidebarPopoutPreview() const { return sidebar_popout_preview_text_; }
    const std::vector<PickerHlSpan> &SidebarPopoutPreviewSpans() const { return sidebar_popout_preview_spans_; }
    const std::string &SidebarPopoutPreviewTitle() const { return sidebar_popout_preview_title_; }
    int SidebarPopoutPreviewCurrentRow() const { return sidebar_popout_preview_current_row_; }
    int SidebarPopoutPreviewScroll() const { return sidebar_popout_preview_scroll_; }
    /**
     * @brief Scrolls the popout preview column by `delta` raw lines (mod1+j/k while popped out), clamped in range.
     * @param delta Lines to scroll; positive scrolls down.
     */
    void ScrollSidebarPopoutPreview(int delta);
    // Lines of `path` for a preview: the live (possibly unsaved) buffer's
    // lines if that file is open in one, else read from disk -- the same
    // "an open buffer is the source of truth" rule ActivityTodoLoad uses.
    // Bound as mep.read_lines.
    /**
     * @brief Reads a file's lines, preferring an open buffer's live contents over the on-disk copy.
     * @param path The file path to read.
     * @param max_lines Stop after this many lines (<= 0 for no cap); a trailing "..." marker is appended when truncated.
     * @param out Receives the lines.
     * @return False if the file is neither open nor readable.
     */
    bool ReadLinesForPath(const std::string &path, int max_lines, std::vector<std::string> *out) const;
    // The `id` string of the widget at the sidebar's current cursor line,
    // or "" if the cursor is on a section header (or the sidebar/cursor is
    // invalid) -- what a Phase 15-style on_key handler uses to know which
    // row a key like "rename" or "delete" applies to.
    /**
     * @brief Returns the widget id at a sidebar's current cursor line.
     * @param id The id of the sidebar to query.
     * @return The widget id, or "" if the cursor is on a section header or the sidebar/cursor is invalid.
     */
    std::string SidebarCursorWidgetId(int id) const;
    // Mouse click-through (main.cpp's own row/border hit-testing calls
    // into these -- see SidebarWidget's own header, which flagged this as
    // a deliberate follow-up): FocusSidebarRow focuses `id` and moves its
    // cursor to `line_index` (clamped in range) -- a single click's whole
    // job, mirroring OpenSidebar's own focus bookkeeping exactly, just
    // without forcing sidebar_cursor_ back to 0. ActivateSidebarLine runs
    // whatever `line_index` itself would do on Enter (toggle a section
    // header's collapsed state, or fire a widget's on_click_ref) --
    // shared by HandleSidebarInput's own Enter handling and a mouse
    // double-click, both just resolving to "activate the line at index N"
    // by a different route.
    /**
     * @brief Focuses a sidebar and moves its cursor to a clamped line index, for a mouse click.
     * @param id The id of the sidebar to focus.
     * @param line_index The line index to move the cursor to (clamped in range).
     */
    void FocusSidebarRow(int id, int line_index);
    // The widget id of flattened line `line_index` of sidebar `id` ("" for
    // a section header or out-of-range line). The file tree and git
    // sidebars use the file path as the id, which is what main.cpp's
    // drag-a-file-onto-a-pane gesture keys off.
    std::string SidebarLineWidgetId(int id, int line_index) const;
    /**
     * @brief Activates the sidebar line at an index, as Enter would (toggling a section header's collapsed state or firing a widget's on_click).
     * @param id The id of the sidebar containing the line.
     * @param line_index The line index to activate.
     */
    void ActivateSidebarLine(int id, int line_index);
    // Adjusts sidebar `id`'s scroll_offset so its cursor stays visible
    // within `visible_lines` rows, and clamps it back in range if the
    // sidebar's own content shrank (a collapsed section, a deleted file).
    // Call once per frame for every *open* sidebar (main.cpp's DrawSidebars,
    // which alone knows each one's rendered height) -- mirrors
    // UpdateScrollForPane's contract exactly, just without that one's
    // fold/wrap/org-image slot-counting: every sidebar row is exactly one
    // line tall, so plain index arithmetic is enough.
    /**
     * @brief Adjusts a sidebar's scroll offset so its cursor stays visible within a given viewport height, clamping it if content shrank.
     * @param id The id of the sidebar to update.
     * @param visible_lines The sidebar's currently rendered height, in lines.
     */
    void UpdateScrollForSidebar(int id, int visible_lines);
    // Absolute cell-count resize (border-drag's own per-frame update) --
    // the mouse-driven sibling of ResizeActivePane's Mode::Sidebar branch,
    // which only ever nudges by a fixed step. Clamped to the same minimum
    // that branch's own local kSidebarMinSize enforces (kept as a literal
    // here rather than a shared constant -- see this .cpp's own comment).
    /**
     * @brief Sets a sidebar's fixed size to an absolute cell count, for mouse border-drag resizing.
     * @param id The id of the sidebar to resize.
     * @param size The desired size in cells, clamped to a minimum.
     */
    void SetSidebarSize(int id, int size);

    // --- Fuzzy picker (NVIM_PARITY_PLAN.md Part I Phase 8) ---
    // Opens the picker over `items` (a *static* list -- a Lua-side
    // dynamic/async source re-populates it live via SetPickerItems as
    // results stream in, e.g. from a ripgrep job). on_query_change_ref
    // (may be 0/none) fires on every query edit, letting such a source
    // notice and re-search; on_select_ref fires once, with the chosen
    // item's `data` string on Enter or no args [nil] on Escape.
    // on_key_ref (may be 0/none) fires with a 1-char string on Ctrl+<letter>
    // for any letter besides N/P (already reserved for next/prev), so a
    // picker can offer its own shortcuts (e.g. mep.projects()'s Ctrl-A for
    // "add current directory", the same action as selecting that row).
    // on_select_change_ref (may be 0/none) fires with the newly-highlighted
    // item's `data` string every time the effective selection changes --
    // arrow/Ctrl-N/Ctrl-P navigation, or the query narrowing to a different
    // top match -- *before* Enter/Escape commit or cancel it. This is what
    // lets a picker like mep.themes() live-preview each candidate as the
    // cursor moves over it, not just on final selection.
    // `raw_results`: skip Phase 8's own client-side FuzzyScore re-filter
    // in PickerFilteredResults() and show `picker_items_` exactly as the
    // source provided them (NVIM_PARITY_PLAN.md Phase 8's "async
    // get_items(query, callback)" gap, closed for the one consumer that
    // actually needs it: mep.live_grep). A dynamic source that already
    // filtered server-side (ripgrep matching its own regex query) would
    // otherwise get its results double-filtered by a *different*
    // matching algorithm (fuzzy-subsequence) that can legitimately hide
    // a real regex match whose special characters (`.`, `*`, ...) don't
    // literally appear in the matched line. Static sources (find_files,
    // buffers, commands, ...) leave this false and keep today's
    // client-side fuzzy filtering exactly as before.
    /**
     * @brief Opens the fuzzy picker overlay over a list of items, taking over input until an item is chosen or cancelled.
     * @param title The picker's title text.
     * @param items The initial (possibly later replaced via SetPickerItems) item list.
     * @param on_select_ref Lua ref invoked with the chosen item's `data` string on Enter, or no args on Escape; unrefed either way.
     * @param on_query_change_ref Lua ref (may be 0/none) invoked on every query edit, for a dynamic source to re-search.
     * @param on_key_ref Lua ref (may be 0/none) invoked with a 1-char string on Ctrl+<letter> for a picker-specific shortcut.
     * @param on_select_change_ref Lua ref (may be 0/none) invoked with the newly-highlighted item's `data` string whenever the effective selection changes.
     * @param raw_results Whether to show `picker_items_` exactly as provided, skipping the client-side fuzzy re-filter.
     */
    void OpenPicker(const std::string &title, std::vector<PickerItem> items, int on_select_ref,
                     int on_query_change_ref, int on_key_ref = 0, int on_select_change_ref = 0,
                     bool raw_results = false);
    /**
     * @brief Closes the picker overlay, leaving its registered Lua callback refs for the caller to invoke/unref.
     */
    void ClosePicker();
    // Unlike bare ClosePicker() (which only flips picker_open_/mode_,
    // leaving ref cleanup to the caller -- HandlePickerInput's Escape/Enter
    // paths need the refs to still be valid *after* closing so they can
    // invoke on_select_ref first), this also unrefs all of the picker's
    // registered Lua callbacks. For callers like mep.picker_close() that
    // just want the picker gone without running any of them.
    /**
     * @brief Closes the picker overlay and unrefs all of its registered Lua callbacks without invoking any of them.
     */
    void ClosePickerDiscardingCallbacks();
    /**
     * @brief Replaces the open picker's item list, for a dynamic source streaming in results.
     * @param items The new item list.
     */
    void SetPickerItems(std::vector<PickerItem> items);
    /**
     * @brief Reports whether the picker overlay is currently open.
     * @return True if the picker is open.
     */
    bool IsPickerOpen() const { return picker_open_; }
    /**
     * @brief Returns the open picker's title text.
     * @return The picker title.
     */
    const std::string &PickerTitle() const { return picker_title_; }
    /**
     * @brief Returns the open picker's current query text.
     * @return The picker query.
     */
    const std::string &PickerQuery() const { return picker_query_; }
    /**
     * @brief Returns the open picker's currently highlighted result index.
     * @return The selected index.
     */
    int PickerSelected() const { return picker_selected_; }
    // Recomputed on demand (not cached) from the current query -- items
    // scoring < 0 (no match) are dropped, the rest sorted by score desc.
    // Returns `picker_items_` verbatim, unfiltered/unsorted, when the
    // picker was opened with raw_results (see OpenPicker above).
    /**
     * @brief Recomputes the picker's items filtered and sorted by fuzzy match score against the current query.
     * @return The filtered, score-sorted items, or `picker_items_` verbatim when opened with raw_results.
     */
    std::vector<PickerItem> PickerFilteredResults() const;

    // --- Picker preview pane (NVIM_PARITY_PLAN.md Phase 8 gap, closed) ---
    // mep.picker_set_preview(text): a source (e.g. find_files' on_select_
    // change callback, reading the highlighted file) sets the text shown
    // in a second column beside the results list; main.cpp's
    // DrawPickerOverlay only draws that column, and only widens the box
    // for it, when this is non-empty. Cleared on every OpenPicker() so a
    // *later* unrelated picker doesn't inherit a stale preview from
    // whatever was open before it. `spans` is optional Treesitter-backed
    // syntax highlighting over the preview text (PickerHlSpan above,
    // `row` indexing SplitLines(text)) -- defaults empty for every
    // existing plain-text preview source.
    /**
     * @brief Sets the text (and optional syntax highlight spans) shown in the picker's preview column, resetting its scroll.
     * @param text The preview text to show.
     * @param spans Optional Treesitter-backed highlight spans over the preview text.
     */
    void SetPickerPreview(const std::string &text, std::vector<PickerHlSpan> spans = {}) {
        picker_preview_text_ = text;
        picker_preview_spans_ = std::move(spans);
        picker_preview_scroll_ = 0;
    }
    /**
     * @brief Returns the picker's current preview text.
     * @return The preview text, empty if no preview is set.
     */
    const std::string &PickerPreview() const { return picker_preview_text_; }
    /**
     * @brief Returns the picker preview's syntax highlight spans.
     * @return The preview's highlight spans.
     */
    const std::vector<PickerHlSpan> &PickerPreviewSpans() const { return picker_preview_spans_; }
    /**
     * @brief Returns the picker preview column's current scroll offset.
     * @return The preview scroll offset.
     */
    int PickerPreviewScroll() const { return picker_preview_scroll_; }

    // --- Roam backlink-graph view (NVIM_PARITY_PLAN.md Phase 37's flagged
    // "no fuzzy backlink-graph visualization" gap, closed) ---
    // `nodes[0]` must be the note the view was opened on (hop 0); the rest
    // are placed by `hop` into concentric rings (see RoamGraphNode) around
    // it, evenly spaced by angle within their own ring -- computed once
    // here and left in `roam_graph_nodes_` for main.cpp's draw to consume,
    // not recomputed per frame. `on_select_ref`, like OpenPicker's
    // on_select, is called with the chosen node's `path` on Enter, or with
    // no argument on Escape (mirrors mep.picker_open's nil-on-cancel
    // convention) -- then unref'd either way by HandleRoamGraphInput.
    /**
     * @brief Opens the roam backlink-graph overlay, laying out nodes into concentric hop-rings around the anchor note.
     * @param title The graph view's title text.
     * @param nodes The graph's nodes; nodes[0] must be the note the view was opened on (hop 0).
     * @param edges The graph's edges between nodes.
     * @param on_select_ref Lua ref invoked with the chosen node's `path` on Enter, or no args on Escape; unrefed either way.
     */
    void OpenRoamGraph(const std::string &title, std::vector<RoamGraphNode> nodes,
                        std::vector<RoamGraphEdge> edges, int on_select_ref);
    // Unlike bare mode restore, this also unrefs on_select_ref without
    // calling it -- for mep.roam_graph_close() wanting the view gone with
    // no callback fired (mirrors ClosePickerDiscardingCallbacks).
    /**
     * @brief Closes the roam graph overlay and unrefs its on_select callback without invoking it.
     */
    void CloseRoamGraphDiscardingCallback();
    /**
     * @brief Reports whether the roam graph overlay is currently open.
     * @return True if the roam graph is open.
     */
    bool IsRoamGraphOpen() const { return roam_graph_open_; }
    /**
     * @brief Returns the open roam graph's title text.
     * @return The graph title.
     */
    const std::string &RoamGraphTitle() const { return roam_graph_title_; }
    /**
     * @brief Returns the open roam graph's nodes.
     * @return The graph nodes.
     */
    const std::vector<RoamGraphNode> &RoamGraphNodes() const { return roam_graph_nodes_; }
    /**
     * @brief Returns the open roam graph's edges.
     * @return The graph edges.
     */
    const std::vector<RoamGraphEdge> &RoamGraphEdges() const { return roam_graph_edges_; }
    /**
     * @brief Returns the roam graph's current fuzzy-filter query text.
     * @return The graph query.
     */
    const std::string &RoamGraphQuery() const { return roam_graph_query_; }
    /**
     * @brief Returns the roam graph's currently selected node index.
     * @return The selected node index.
     */
    int RoamGraphSelected() const { return roam_graph_selected_; }
    // Indices into RoamGraphNodes() that currently match the fuzzy filter
    // (FuzzyScore against each node's title, reusing Phase 8's scorer --
    // the plan's own suggestion to reuse the picker's fuzzy-match
    // convention). Node 0 (the center note) always matches regardless of
    // query, so the anchor never disappears. Empty query matches every
    // node. main.cpp's draw dims/greys out any node *not* in this set
    // instead of removing it, so the ring layout (and the edges into/out
    // of a dimmed node) stays legible instead of reflowing every
    // keystroke -- the "narrows which nodes are... highlighted" half of
    // the plan's requirement, not the "shown" half, a deliberate choice
    // documented in NVIM_PARITY_PLAN.md.
    /**
     * @brief Returns the indices of roam graph nodes currently matching the fuzzy query, for dimming the rest without reflowing the ring layout.
     * @return The matching node indices; node 0 (the anchor) always matches, and an empty query matches every node.
     */
    std::vector<int> RoamGraphFilteredIndices() const;

    // --- Whichkey (NVIM_PARITY_PLAN.md Part II Phase 11) ---
    /**
     * @brief Sets which key acts as the leader for which-key leader sequences.
     * @param key The new leader key.
     */
    void SetLeaderKey(char key) { leader_key_ = key; }
    /**
     * @brief Registers a leader-key sequence bound to a Lua callback, for the which-key overlay.
     * @param sequence The key sequence following the leader key.
     * @param description Human-readable description shown in the which-key overlay.
     * @param lua_ref The Lua registry reference to invoke when the sequence is completed.
     */
    void RegisterWhichKey(const std::string &sequence, const std::string &description, int lua_ref);
    // Enters Mode::WhichKey with an empty prefix -- called when the leader
    // key is pressed in Normal mode (see the char-dispatch loop).
    /**
     * @brief Enters Mode::WhichKey with an empty prefix, opening the which-key overlay.
     */
    void TriggerWhichKey();
    /**
     * @brief Returns the which-key overlay's currently typed prefix.
     * @return The current prefix text.
     */
    const std::string &WhichKeyPrefix() const { return whichkey_prefix_; }
    // Bindings whose sequence starts with the current prefix, each paired
    // with the sequence's remainder (what's still left to type) -- the raw
    // leaf list HandleWhichKeyInput's exact-match/dead-end checks use.
    /**
     * @brief Returns every registered which-key binding whose sequence starts with the current prefix, paired with its remaining text.
     * @return The matching sequence/remainder pairs.
     */
    std::vector<std::pair<std::string, std::string>> WhichKeyMatches() const;
    // mep.leader_group(prefix, label): names a group of bindings that
    // share `prefix` (e.g. "o" -> "org") so DrawWhichKeyOverlay can show
    // one collapsed "o  +org" row instead of every leaf under it spelled
    // out in full -- real which-key.nvim requires the same explicit
    // per-group naming (there's no reliable way to auto-derive "org" from
    // a mix of "Org: ..."/"Org-roam: ..." descriptions in general).
    /**
     * @brief Names a group of which-key bindings sharing a prefix, so the overlay can show one collapsed row for them.
     * @param prefix The shared key sequence prefix.
     * @param label The group's display label.
     */
    void RegisterWhichKeyGroup(const std::string &prefix, const std::string &label) {
        whichkey_groups_[NormalizeWhichKeySequence(prefix)] = label;
    }
    // Leader sequences are stored one byte per key. Enter is the one
    // non-printable key HandleWhichKeyInput accepts (so a binding like
    // <leader>a<CR> can exist); it's kept as a literal '\r' internally,
    // while Lua callers spell it "<CR>" (also "<cr>", "<Return>",
    // "<Enter>") the way vim does. These two convert between the spellings.
    /**
     * @brief Converts a Lua-facing leader sequence ("a<CR>") to the internal one-byte-per-key form ("a\r").
     * @param seq The sequence as written in mep.leader_map/mep.leader_group.
     * @return The normalized sequence.
     */
    static std::string NormalizeWhichKeySequence(const std::string &seq);
    /**
     * @brief Converts an internal leader sequence back to its display form, spelling Enter as "<CR>".
     * @param seq A sequence (or sequence suffix/prefix) in the internal form.
     * @return The human-readable form.
     */
    static std::string WhichKeySequenceDisplay(const std::string &seq);
    // What DrawWhichKeyOverlay actually lists: WhichKeyMatches() bucketed
    // by their next character, collapsed to one "+label" row per bucket
    // that both has more than one leaf *and* a registered group label;
    // every other bucket (a lone leaf, or an unlabeled multi-leaf one)
    // falls through to listing its own leaf/leaves exactly as before, so
    // an unnamed group degrades to today's flat behavior rather than
    // hiding anything.
    /**
     * @brief Buckets the current which-key matches by their next character, collapsing labeled multi-leaf buckets into a single "+label" row.
     * @return The display rows to render in the which-key overlay.
     */
    std::vector<std::pair<std::string, std::string>> WhichKeyDisplayEntries() const;
    // Every registered leader-sequence binding, unfiltered by any typed
    // prefix -- the leader-sequence half of the keybinding-introspection
    // picker (mep.leader_bindings(), NVIM_PARITY_PLAN.md Phase 25), the
    // other half being AllMappingDescriptions() (above) for plain
    // mep.map() bindings.
    /**
     * @brief Returns every registered leader-sequence binding, unfiltered by any typed prefix.
     * @return All which-key bindings.
     */
    const std::vector<WhichKeyBinding> &AllWhichKeyBindings() const { return whichkey_bindings_; }

    // --- Dashboard/scratch/zen (NVIM_PARITY_PLAN.md Part III Phase 12) ---
    // True exactly when the dashboard should render: single tab, single
    // pane, single buffer, that buffer untouched (empty, unmodified, no
    // filename) -- recomputed fresh each frame rather than cached, so it
    // disappears the instant any of that stops being true.
    bool ShouldShowDashboard() const;
    // Finds the existing scratch buffer if one exists in this session,
    // otherwise creates one; switches the current pane to it either way.
    void OpenScratchBuffer();
    void ToggleZenMode() { zen_mode_ = !zen_mode_; }
    // Switches SheetSession::active_sheet by one, wrapping around at
    // either end (Ctrl-PageDown/Ctrl-PageUp -- Excel's own convention for
    // this) -- undo/redo stay per-workbook, not per-sheet, so this doesn't
    // touch undo_stack/redo_stack at all, just re-clamps the cursor into
    // the newly-active sheet's used range via the same helper Undo/
    // RedoSheet's own post-swap clamp uses. Public (unlike Push/Undo/
    // RedoSheet just above private:) so mep.sheet_next/prev (lua_env.cpp)
    // and the :MepNextSheet/:MepPrevSheet ex-commands can call it.
    void NextSheet();
    void PrevSheet();
    bool IsZenMode() const { return zen_mode_; }
    bool IsSttRecording() const { return stt_recording_; }
    void SetSttRecording(bool v) { stt_recording_ = v; }

    // --- Hints (NVIM_PARITY_PLAN.md Part III Phase 13) ---
    // Enters Mode::HintChar, awaiting the character to search for.
    void BeginHints();
    bool IsHintActive() const { return mode_ == Mode::HintChar || mode_ == Mode::HintLabel; }
    const std::vector<HintMatch> &HintMatches() const { return hint_matches_; }
    const std::string &HintTyped() const { return hint_typed_; }

    // --- Window tiling-manager layer (NVIM_PARITY_PLAN.md Part III Phase 14) ---
    // Directional focus already existed pre-Phase-14 (NavigatePaneDirection,
    // bound as mep.nav_pane) -- nothing new needed there.
    //
    // Per-pane buffer tabs: opens `path` as a new tab within the *current*
    // pane (find-or-create the buffer, insert after the current tab).
    void PaneOpenBufferInTab(const std::string &path);
    void PaneNextBufferTab();
    void PanePrevBufferTab();
    // Click-to-switch (same reasoning as GoToTab): jumps directly to buffer
    // tab `index` in the current pane, unlike PaneNextBufferTab/
    // PanePrevBufferTab's relative stepping -- DrawPane's own per-pane
    // buffer-tab strip calls this for an immediate response. Only ever
    // registered as a click handler while that pane is the active one (see
    // DrawPane), so CurPane() always resolves back to the same pane the
    // click was drawn on.
    void GoToPaneBufferTab(int index);
    // Removes the current pane's active buffer tab; closes the pane itself
    // (existing ClosePane()) once its last tab is removed.
    void PaneCloseBufferTab();
    // The pane header's own close 'x' (DrawPane, main.cpp): removes the
    // current pane's active buffer tab exactly like PaneCloseBufferTab
    // (closing the pane once its last tab goes), THEN soft-deletes that
    // buffer out of every other pane still showing it (BufferDeleteById,
    // the `:bd` machinery) -- so the click both closes the tab and drops
    // the buffer from buffer_list/bnext/bprev, rather than leaving it
    // lingering as a hidden buffer the way PaneCloseBufferTab alone does.
    // Refuses (E37 in the status line, nothing changes) when the buffer
    // has unsaved changes, same guard as :bd without '!'. On the very
    // last window ClosePane can't remove the pane, so the pane instead
    // ends up showing BufferDeleteById's fallback buffer.
    void PaneCloseBufferTabAndDelete();
    // Moves the current pane's active buffer tab to the nearest pane in
    // `direction` (reuses NavigatePaneDirection's neighbor search); a no-op
    // if there's no pane that way. When a sidebar is focused instead of a
    // pane (Mode::Sidebar), the same mod1+Ctrl+j/k keys move that sidebar
    // itself instead: swaps it with the one stacked above/below it in its
    // left/right dock (SwapSidebarInStack), leaving it focused so the
    // cursor travels with it -- the sidebar analogue of "move the thing
    // I'm looking at that way", the way ResizeActivePane's Mode::Sidebar
    // branch is the analogue for mod1+Shift+hjkl. Left/right (and anything
    // on a top/bottom dock) is a no-op there.
    void PaneMoveBufferTabToNeighbor(const std::string &direction);
    std::vector<int> CurrentPaneBufferTabs() const { return CurPane().buffer_tabs; }
    int CurrentPaneBufferTabIndex() const { return CurPane().buffer_tab_index; }

    // --- Completion engine (NVIM_PARITY_PLAN.md Part V Phase 22) ---
    // Genuinely new UI (mep had no completion infra to plug sources into):
    // a passive popup that coexists with Insert mode rather than stealing
    // it -- HandleInsertInput intercepts only Ctrl-N/Ctrl-P/Tab/Enter/
    // Escape while it's open; every other key (including ordinary typing)
    // still reaches normal insert handling, and re-triggers
    // UpdateCompletionPopup() afterward so the list stays live as you type
    // (the plan's "debounced auto-trigger" -- recomputed once per
    // keystroke against a single buffer is cheap enough that no separate
    // timer/thread was needed to keep it feeling instant).
    // mep.set_completion_source(fn): fn(prefix) -> array of candidate
    // items, called after each insert-mode edit with the word prefix
    // immediately before the cursor (empty string closes the popup unless
    // it's a dot-trigger, see UpdateCompletionPopup). Each item is either
    // a plain string (kind/detail/doc left blank, for backward
    // compatibility with a custom source that predates this) or a table
    // {text=, kind=, detail=, doc=} -- kind is a short source tag ("lsp",
    // "file", "snippet", "buffer") shown as a badge in the popup; detail/
    // doc (LSP CompletionItem.detail/.documentation, usually a type
    // signature and a docstring) drive the side info panel next to it.
    void SetCompletionSourceRef(int lua_ref) { completion_source_ref_ = lua_ref; }
    bool IsCompletionOpen() const { return completion_open_; }
    const std::vector<CompletionCandidate> &CompletionItems() const { return completion_items_; }
    int CompletionSelected() const { return completion_selected_; }
    // mep.set_completion_accept_hook(fn): fn(text) called (Phase 23
    // LSP-snippet gap) right after AcceptCompletion splices `text` into the
    // buffer verbatim, with the cursor already left at the end of it (so the
    // hook can read mep.cursor()/mep.get_line() itself to find the span it
    // just wrote). A Lua-side hook can look `text` up against whatever
    // per-item metadata it cached (e.g. an LSP CompletionItem's
    // insertTextFormat) and, if it turns out to have been a Snippet-format
    // item, delete that raw span and re-expand it through
    // mep.snippet_expand's own tabstop engine instead of leaving literal
    // `$1`-style placeholder syntax sitting in the buffer. A no-op (word
    // stays as plain inserted text) for every other completion source,
    // which never sets this hook up to care.
    void SetCompletionAcceptHookRef(int lua_ref) { completion_accept_hook_ref_ = lua_ref; }
    // mep.set_completion_resolve_hook(fn): fn(text) -> {detail=, doc=} or
    // nil. Most LSP items arrive from textDocument/completion nearly
    // empty (pyright, e.g., sends label/kind/sortText/data only) -- detail/
    // documentation are usually filled in lazily via a follow-up
    // completionItem/resolve request. DrawCompletionDetailPanel (main.cpp)
    // calls this once per frame it's drawing the panel for a given item
    // that had no detail/doc in CompletionItems()' own snapshot; a Lua
    // hook can kick off (or check on) that async resolve itself and
    // return nil until the response lands, same "poll every frame, cheap
    // since it's only ever the one selected item" shape as
    // Editor::MaybeDismissHover.
    void SetCompletionResolveHookRef(int lua_ref) { completion_resolve_hook_ref_ = lua_ref; }
    // Calls the resolve hook (if set) for `text`; returns false (detail/
    // doc untouched) if there's no hook or it returned nil this frame.
    bool CompletionResolveInfo(const std::string &text, std::string *detail, std::string *doc) const;
    // mep.set_insert_tab_hook(fn): fn(shift) -> bool, asked on every
    // Insert-mode Tab/Shift-Tab press that the completion popup doesn't
    // already claim (see HandleInsertInput) -- Phase 23's tabstop-cycling
    // gap: previously Tab/Shift-Tab had no editor-wide binding at all
    // (only the explicit :MepSnippetNext/:MepSnippetPrev commands), so a
    // snippet mid-expansion couldn't be advanced with the key every other
    // snippet-capable editor uses. Returning true tells HandleInsertInput
    // the key was consumed (kBuiltinSnippets' hook jumps the tabstop and
    // returns true only while mep_snippet_state is active); returning
    // false leaves Tab a no-op, same as before this hook existed, since
    // Insert mode has no other built-in Tab behavior to fall back to.
    void SetInsertTabHookRef(int lua_ref) { insert_tab_hook_ref_ = lua_ref; }

    // mep.buffer_set_on_enter(buffer_id, fn): fn() called instead of
    // whatever bare Enter/KP_Enter already does in Normal mode -- nothing,
    // today; unlike Insert mode's CR this editor has never bound Normal
    // mode's own Enter to a motion the way real Vim's "+"/CR is, so
    // intercepting it here doesn't take anything away -- whenever the
    // active pane's buffer is `buffer_id`. Single-slot, last-registration-
    // wins, same scope cut as SetCompletionSourceRef/SetSidebarOnKey above:
    // one buffer needs this at a time, not a general per-buffer registry.
    // kBuiltinStructure's structure-split pane (main.cpp) is the first
    // caller -- lets <CR> jump to the entry under the cursor there the
    // same way ActivateSidebarLine already does for the full-sidebar
    // version (HandleSidebarInput), instead of requiring :MepStructureSplit
    // to be re-run from inside the pane.
    void SetBufferOnEnter(int buffer_id, int lua_ref) {
        enter_hook_buffer_id_ = buffer_id;
        enter_hook_ref_ = lua_ref;
    }

    // --- Command-line completion (`:` command bar) -----------------------
    // Same Tab/Ctrl-N/Ctrl-P/Enter/Escape shape as the Insert-mode popup
    // above, reusing PickerItem and (in main.cpp) DrawCompletionPopup, but
    // driven from HandleCommandInput rather than HandleInsertInput and
    // sourced from built-in/Lua command names or filesystem paths instead
    // of a Lua completion-source callback.
    bool IsCmdlineCompletionOpen() const { return cmdline_completion_open_; }
    const std::vector<PickerItem> &CmdlineCompletionItems() const { return cmdline_completion_items_; }
    int CmdlineCompletionSelected() const { return cmdline_completion_selected_; }

    // Rebuilds the active tab's *entire* split tree into one of a few
    // one-shot automatic layouts, over whatever panes currently exist
    // (existing pane ids/buffers/cursors are preserved, just re-parented):
    // "master-left"/"master-right"/"master-top"/"master-bottom" (one big
    // pane + an equal-share stack of the rest), "grid" (near-square rows x
    // cols), "grid-h"/"grid-v" (one row / one column of equal shares), or
    // "spiral" (each pane takes half of what's left, alternating axis --
    // the classic dwm/i3 fibonacci layout). Unknown `kind` is a no-op.
    void ApplyLayout(const std::string &kind);

    // --- Statusline widget hook (Part II Phase 11) ---
    // `ref` is called with no args each frame and expected to return an
    // array of {text=, hl=} segments (LuaEnv::CallRefForWidgets); main.cpp
    // falls back to its built-in status line format when this is 0/unset.
    void SetStatuslineRef(int ref) { statusline_ref_ = ref; }
    int StatuslineRef() const { return statusline_ref_; }

    // --- Active-todo status-bar widget (kBuiltinActivityBar) ---
    // What the bottom bar's todo chip shows: the clocked-in todo's title
    // and a live elapsed timer (main.cpp computes it from `start_epoch`
    // each frame), or "[No active TODO]" when nothing is set. Pushed from
    // Lua (mep.active_todo_set) whenever the Todo sidebar's clock state
    // is re-derived from TODO.org, never scanned from here.
    void SetActiveTodo(const std::string &text, long long start_epoch) {
        active_todo_ = true;
        active_todo_text_ = text;
        active_todo_start_epoch_ = start_epoch;
    }
    void ClearActiveTodo() {
        active_todo_ = false;
        active_todo_text_.clear();
        active_todo_start_epoch_ = 0;
    }
    bool HasActiveTodo() const { return active_todo_; }
    const std::string &ActiveTodoText() const { return active_todo_text_; }
    long long ActiveTodoStartEpoch() const { return active_todo_start_epoch_; }

    // --- Winbar breadcrumb click hook (Part II Phase 11 click-dispatch gap) ---
    // The per-pane header (main.cpp's DrawPane) renders the active buffer's
    // path as clickable breadcrumb segments -- this is mep's winbar
    // equivalent, just not (yet) a separate Lua-configurable widget row.
    // `ref`, if set, is called with the clicked directory segment's path
    // (e.g. "src" or "src/lua") on click; main.cpp's default handler (mep.
    // winbar_navigate, kBuiltinPickerSources) opens a file picker scoped to
    // that directory. 0/unset means breadcrumb segments render but clicks
    // on them do nothing.
    void SetWinbarClickRef(int ref) { winbar_click_ref_ = ref; }
    int WinbarClickRef() const { return winbar_click_ref_; }

    // Click-to-switch (Phase 11 click-dispatch gap): jump directly to tab
    // `index` (clamped into range), unlike TabNext/TabPrevious's relative
    // stepping -- public (unlike those two, invoked only via ex-commands
    // through RunCommand) because DrawTabBar's click handler calls it
    // directly for an immediate response, the same reasoning as
    // ToggleFoldAtRow below.
    void GoToTab(int index);

    // Same reasoning as GoToTab above: public because DrawTabBar's '+'/'x'
    // buttons (and HandleTabShortcuts' Ctrl-T) call these directly, unlike
    // TabNext/TabPrevious which only ever run through ex-commands.
    void TabNew(const std::string &file_arg);
    void TabDelete();

    // --- Hover tooltip (NVIM_PARITY_PLAN.md Phase 3 gap, closed) ---
    // A small non-modal floating window anchored near the cursor -- unlike
    // Prompt/Confirm/Select/Preview above, this does *not* change `mode_`
    // or steal input (mirrors DrawCompletionPopup's "coexists with the
    // active mode" shape, Phase 22): it's just an overlay main.cpp draws
    // on top of whatever mode is active whenever IsHoverOpen() is true.
    // Auto-dismiss-on-move (the plan's literal wording): the cursor
    // position at the moment ShowHover() was called is snapshotted; the
    // very next HandleInput() call where the cursor no longer matches (or
    // Normal mode was left, or Escape was pressed) closes it. Checked once
    // per frame from the top of HandleInput() via MaybeDismissHover().
    void ShowHover(const std::string &title, const std::string &text);
    void CloseHover() { hover_open_ = false; }
    bool IsHoverOpen() const { return hover_open_; }
    const std::string &HoverTitle() const { return hover_title_; }
    const std::string &HoverText() const { return hover_text_; }

    // --- Hover focus (see Mode::HoverFocus's own comment) ---
    // No-op if hover isn't currently open.
    void EnterHoverFocus();
    int HoverFocusRow() const { return hover_focus_row_; }
    int HoverFocusCol() const { return hover_focus_col_; }
    int HoverFocusScroll() const { return hover_focus_scroll_; }
    bool HoverFocusSelecting() const { return hover_focus_selecting_; }
    bool HoverFocusSelectLinewise() const { return hover_focus_select_linewise_; }
    int HoverFocusSelectAnchorRow() const { return hover_focus_select_anchor_row_; }
    int HoverFocusSelectAnchorCol() const { return hover_focus_select_anchor_col_; }

private:
    void JoinCollaboration(const std::string &url, const std::string &name);
    void LeaveCollaboration();
    void TickCollaboration();
    // Closes an open hover tooltip once the cursor has moved away from
    // where it was when ShowHover() opened it, or the mode is no longer
    // Normal, or Escape was just pressed. Called at the top of
    // HandleInput(), before dispatching to the active mode's handler.
    void MaybeDismissHover();
    void HandleNormalInput();
    // The per-character body of HandleNormalInput's own GetCharPressed()
    // drain loop, factored out so HandleNormalInput's bare-hjkl fast path
    // (see its own comment) can invoke the exact same leader-key/Lua-
    // mapping/ProcessNormalKey handling a queued character would have
    // gotten, without duplicating it.
    void HandleNormalChar(int cp, bool no_pending_state);
    void HandleInsertInput();
    void HandleVisualInput();
    void HandleCommandInput();
    // Shared by Mode::SearchForward/SearchBackward -- text editing is
    // identical to HandleCommandInput's, only what Enter does differs
    // (run a search instead of ExecuteCommandLine), so this is its own
    // small function rather than threading a "which mode" flag through
    // HandleCommandInput itself.
    void HandleSearchInput();
    void HandlePromptInput();
    void HandleConfirmInput();
    void HandleSelectInput();
    void HandlePreviewInput();
    void HandleHoverFocusInput();
    void HandleSidebarInput();
    void HandlePickerInput();
    void HandleRoamGraphInput();
    void HandleWhichKeyInput();
    void HandleHintCharInput();
    void HandleHintLabelInput();
    void HandleTerminalInput();
    TerminalSession *FindTerminal(int buffer_id);
    // Encodes one keypress as the bytes a real terminal would send for it
    // (arrows/Home/End/Delete/function keys as CSI or SS3 escape
    // sequences depending on term->ApplicationCursorKeys(), Ctrl+letter as
    // a C0 control code, everything else verbatim) and writes it to the
    // session's PTY. Split out of HandleTerminalInput so the Ctrl-\ Ctrl-N
    // exit chord (which must NOT forward its first half if the
    // second key turns out not to be Ctrl-N) can buffer one key before
    // deciding whether to call this.
    void SendTerminalKey(const TerminalSession &sess, int key, int codepoint, bool ctrl, bool shift = false);
    // Ctrl-\ Ctrl-N (Neovim's terminal-normal chord): snapshots the
    // session's current VTerm scrollback+grid into CurPane()'s own buffer
    // (see the comment above the definition for why this is a snapshot,
    // not a live view) and drops to Mode::Normal, so every existing
    // Normal-mode facility -- hjkl/word motions, Ctrl-W pane commands,
    // Visual-mode selection + yank, search, `:` commands -- works against
    // real terminal text with no new input-handling code of its own.
    void EnterTerminalNormalMode(TerminalSession &sess);
    // Platform split for a terminal session's actual process transport:
    // native uses JobManager/Job's existing PTY plumbing (forkpty(),
    // already used by mep.term_start); wasm has no subprocess/PTY concept
    // in-browser at all, so it instead tunnels to launcher/serve.ts's
    // `/pty` WebSocket endpoint, which spawns the real process
    // server-side (Deno.Command, piped stdio -- not a real PTY either,
    // just the least-bad approximation available; see the comment above
    // TerminalSpawn's wasm branch in editor.cpp). Both branches populate
    // sess.job_id with whatever opaque id that platform's backend uses,
    // and PollTerminals() is where the wasm branch's otherwise-passive
    // WebSocket delivery gets pumped into the session's VTerm once per
    // frame (native gets this for free via JobManager::PollAll's
    // callback, already wired elsewhere).
    void TerminalSpawn(TerminalSession &sess, const std::vector<std::string> &argv);
    void TerminalWrite(const TerminalSession &sess, const std::string &bytes);
    void TerminalResizeBackend(const TerminalSession &sess, int cols, int rows);
    // h/j/k/l and arrow keys pan; ':' and the leader key are forwarded
    // (EnterCommand/TriggerWhichKey) so the command line and whichkey/
    // leader mappings keep working; everything else is a no-op -- there's
    // no text to insert/operate on. See Mode::Image's own comment.
    void HandleImageInput();
    // Center-anchored zoom (clamped, re-centers pan on whatever image
    // point was at the viewport's center so it stays put) -- extracted
    // from HandleImageInput's own local apply_zoom lambda so
    // HandleMouseWheel's Image branch (Ctrl-scroll) can reuse the exact
    // same math instead of duplicating it, same reasoning as
    // RebasePdfScroll/ClampPdfPanX's own extraction.
    void ApplyImageZoom(ImageSession &sess, float new_zoom);
    // Finds-or-creates the buffer for `path` (same filename dedup
    // FindOrCreateBuffer uses) and, on a new open, decodes `bytes` via
    // ImageDoc and registers the ImageSession. Called from LoadFile once it
    // has read the file's raw bytes (native ifstream or the wasm
    // mep_js_read_file_binary bridge -- see the comment above that
    // function). Leaves CurPane()'s buffer switched to it on success; on a
    // decode failure, sets status_message_ and leaves the current pane
    // untouched.
    void OpenImageInPlace(const std::string &path, const unsigned char *bytes, size_t len);
    // Same shape as HandleImageInput (h/j/k/l + arrows scroll vertically/
    // pan horizontally, ':'/leader forwarded, everything else a no-op)
    // plus +/-/= zoom (HtmlSession::zoom, same convention as ImageSession's
    // own), 'r' reload and 'o' open-address-bar (both dispatch to a
    // registered Lua command, MepBrowseReload/MepBrowseOpen -- the actual
    // curl-fetch-if-remote logic lives in Lua, kBuiltinTextTools). No page
    // concept to navigate (Ctrl-f/gg/G etc, unlike HandlePdfInput) -- an
    // HTML page is one continuous flow.
    void HandleHtmlInput();
    // Shared by OpenHtmlInPlace's create-branch and ReloadHtmlBuffer:
    // (re)parses `bytes` into `sess` (fresh HtmlDoc, scripts re-run,
    // scroll reset to 0) and sets its origin/source.
    void PopulateHtmlSession(HtmlSession &sess, const std::string &origin, const std::string &source,
                              const unsigned char *bytes, size_t len);
    // Same shape as HandleImageInput, plus: h/j/k/l + arrows scroll
    // continuously (h/l pan horizontally within the anchor page; j/k
    // scroll vertically, crossing page boundaries smoothly rather than
    // hard-cutting -- see PdfSession's own comment). Ctrl-f/Ctrl-b/
    // PageDown/PageUp jump a full page, gg/G to the first/last (reusing
    // pending_g_ -- see OpenPdfInPlace's reset of it). +/-/= zoom like
    // HandleImageInput's apply_zoom, folding drift into rendered_scale
    // (clearing the raster cache) when it leaves its settled band.
    // Ctrl-R toggles PdfSession::theme_colors. '/' starts a text search
    // (delegates to HandlePdfSearchInput while sess.search_active), n/p
    // jump to the next/previous match (GotoPdfMatch) once one exists.
    void HandlePdfInput();
    // While sess.search_active: captures Escape (cancel, discarding
    // search_input but leaving any prior completed search's highlights
    // alone), Enter (submit -- runs RunPdfSearch and jumps to the first
    // match at/after the current page via GotoPdfMatch), Backspace, and
    // all other character input into sess.search_input, instead of the
    // normal pan/zoom/page-nav keys HandlePdfInput handles otherwise.
    void HandlePdfSearchInput(PdfSession &sess);
    // Case-insensitive document-wide search (PdfDoc::Search) for `query`,
    // replacing sess.search_query/search_matches and resetting
    // search_current to -1 (caller -- HandlePdfSearchInput or a
    // freshly-changed query -- is responsible for calling GotoPdfMatch to
    // pick an actual current match afterward). Also refreshes `highlights`
    // on every currently-cached page raster (RecomputePdfPageHighlights)
    // so already-rendered pages don't need a full re-render just because
    // the search changed.
    void RunPdfSearch(PdfSession &sess, const std::string &query);
    // Recomputes PageRaster::highlights for every page currently in
    // sess.rasters from sess.search_matches -- called by RunPdfSearch
    // (query changed) and by EnsurePdfPagesRastered for a page that just
    // entered the render window (its raster is new, so it has no
    // highlights yet even if a search was already active).
    void RecomputePdfPageHighlights(PdfSession &sess);
    // Jumps to search_matches[index] (wrapping around either end so n/p
    // cycle through the whole document): sets page/pan_x/scroll_y so the
    // match is in view (vertically centered where possible), and updates
    // search_current. No-op if search_matches is empty.
    void GotoPdfMatch(PdfSession &sess, int index);
    // Mirrors OpenImageInPlace exactly (dedup-by-filename, decode, register
    // the session, explicit pending_g_ reset to avoid gg/G leakage from
    // whatever mode was active before opening this PDF). Doesn't render
    // anything itself -- EnsurePdfPagesRastered (called from DrawPane
    // every frame, including the first) handles that lazily.
    void OpenPdfInPlace(const std::string &path, const unsigned char *bytes, size_t len);
    // Looks up (and memoizes into sess.page_size_pt) a page's point-space
    // size -- PdfPageSizePt(sess, i) -- and its resulting on-screen pixel
    // height at the session's current rendered_scale/zoom --
    // PdfPageScreenHeightPx(sess, i). Used by both EnsurePdfPagesRastered
    // (which pages are in the {page-1,page,page+1} window) and
    // HandlePdfInput's scroll-rebase math (crossing a page boundary needs
    // to know how tall the page just scrolled past/into is).
    std::pair<double, double> PdfPageSizePt(PdfSession &sess, int page_index);
    float PdfPageScreenHeightPx(PdfSession &sess, int page_index);
    // Crosses `page` forward/backward as scroll_y drifts past the current
    // anchor page's on-screen bounds, re-basing scroll_y relative to the
    // new anchor each time -- extracted from HandlePdfInput's own local
    // lambda of the same name so HandleMouseWheel's Pdf branch can reuse
    // the exact same page-boundary-crossing math instead of duplicating
    // it. See HandlePdfInput's call site for the original comment.
    void RebasePdfScroll(PdfSession &sess);
    // Clamps pan_x against the anchor page's on-screen width at the
    // current zoom/rendered_scale -- same extraction reasoning as
    // RebasePdfScroll.
    void ClampPdfPanX(PdfSession &sess);
    // Folds zoom drift outside its settled band into rendered_scale
    // (clearing the raster cache so cached pages re-render crisp at the
    // new resolution) -- extracted from HandlePdfInput's own local
    // settle_zoom lambda, same reasoning as RebasePdfScroll's own
    // extraction above.
    void SettlePdfZoom(PdfSession &sess);
    // Center-anchored zoom (same shape as ApplyImageZoom, centered on
    // (pan_x, scroll_y) instead of (pan_x, pan_y) since a PDF pane
    // scrolls vertically through scroll_y rather than a separate pan_y)
    // -- extracted from HandlePdfInput's own local apply_zoom lambda so
    // HandleMouseWheel's Pdf branch (Ctrl-scroll) can reuse it.
    void ApplyPdfZoom(PdfSession &sess, float new_zoom);
    // j/k (and Insert-mode Up/Down)'s shared motion: moves within the
    // cursor's *visual* (word-wrapped) line when OfficeSession::
    // cursor_wrap_lines has fresh data for the current paragraph (dir -1 =
    // up/k, +1 = down/j), preserving cursor_col's offset into that line the
    // way column motion normally preserves screen column; otherwise (the
    // paragraph's own first/last visual line, or no fresh cache yet) falls
    // back to stepping to the prev/next *paragraph*, clamping cursor_col
    // the same way the old paragraph-only motion always did. Returns true
    // if it crossed into a different paragraph, so HandleOfficeNormalInput
    // can still do its own table-auto-entry check on that transition (see
    // its old goto_para lambda) -- Visual/Insert callers ignore it, exactly
    // matching their own pre-existing goto_para/Up/Down behavior, which
    // never auto-entered a table either. h/l don't need an equivalent: they
    // already move cursor_col one byte at a time regardless of visual line
    // boundaries, which already reads as normal cross-wrap motion.
    bool MoveOfficeCursorVisualLine(OfficeSession *sess, int dir);
    // ':' and the leader key are forwarded exactly like HandlePdfInput.
    // 'i'/'a' enter Mode::OfficeInsert (PushUndoOffice snapshotting first,
    // vim's own "one undo per insert session" convention), 'v' enters
    // Mode::OfficeVisual.
    void HandleOfficeNormalInput();
    // Char insert, Enter (SplitParagraphAt), Backspace/Delete
    // (ApplyDeleteToParagraph within a paragraph, MergeParagraphs across a
    // paragraph boundary) -- mirrors HandleInsertInput's GetKeyPressed()-
    // for-Escape/Enter/Backspace-then-GetCharPressed()-for-typing split,
    // but operates on OfficeSession/DocParagraph instead of Buffer, so it
    // doesn't go through ProcessInsertKey (tightly coupled to
    // CursorPos/Buffer -- dot-repeat/macro recording, which that owns,
    // isn't in v1 scope for Office anyway, matching Visual-mode operations'
    // own noted scope-out in VIM_PARITY_PLAN.md's Phase 9).
    void HandleOfficeInsertInput();
    // Selection is [min(anchor,cursor), max(anchor,cursor)) by
    // paragraph+col comparison (OfficeSession::has_selection/sel_anchor_*).
    // b/i/u toggle bold/italic/underline over the selection
    // (ToggleFormatOverRange) and return to OfficeNormal, matching vim's
    // own "operator over a Visual selection returns to Normal" convention;
    // hjkl/gg/G extend the selection using the same motions as
    // HandleOfficeNormalInput.
    void HandleOfficeVisualInput();
    // Snapshot-based undo push (mirrors PushUndo()'s own call-site
    // convention -- once per insert-session/operator, never per
    // keystroke): copies doc.paragraphs onto undo_stack and clears
    // redo_stack. Called on entering Mode::OfficeInsert and before a
    // Visual-mode format toggle.
    void PushUndoOffice();
    // Shared by SetOfficeFontFamily/SetOfficeFontSizePt/SetOfficeColor/
    // ClearOfficeColor/SetOfficeHighlight/ClearOfficeHighlight/
    // ToggleOfficeSuperscript/ToggleOfficeSubscript -- the same Visual-
    // selection-vs-single-char-at-cursor dispatch ToggleOfficeFormat does
    // inline (editor.cpp), factored out once rather than repeated per
    // setter. Calls PushUndoOffice, applies `apply` to the current
    // buffer's OfficeSession via SetFormatFieldOverRange, and returns to
    // OfficeNormal if a Visual selection was consumed -- a no-op if the
    // active pane isn't an office buffer or has no paragraphs.
    void ApplyOfficeFormatFieldOverSelection(const std::function<void(DocFormat &)> &apply);
    // Read-only sample (cursor, or a Visual selection's first character)
    // of superscript/subscript state -- shared by ToggleOfficeSuperscript/
    // ToggleOfficeSubscript (decide on vs. off before applying) exactly
    // the way OfficeFormatActive's own inline sampling logic works for
    // bold/italic/underline/strike, factored out here since two callers
    // need the identical read.
    bool OfficeSuperscriptActiveInternal(bool super) const;
    // Mirrors OpenPdfInPlace exactly (dedup-by-filename, decode, register
    // the session, explicit pending_g_ reset). Keeps a copy of `bytes` in
    // the new session's original_bytes for a future save (Phase 4) to
    // copy untouched ZIP entries from.
    void OpenOfficeInPlace(const std::string &path, const unsigned char *bytes, size_t len);
    // 2D grid navigation: hjkl/arrows move the active cell one row/col;
    // gg/G/0/$ jump to the grid's corners (first/last used row, first/last
    // used column, per Sheet::max_row/max_col). ':' and the leader key are
    // forwarded exactly like HandleOfficeNormalInput. 'i'/'a' enter
    // Mode::SheetInsert seeded with the current cell's raw text
    // (PushUndoSheet first); 'v' enters Mode::SheetVisual; 'u'/Ctrl-R
    // undo/redo; a clear-cell/range key (Delete, or a dd-equivalent)
    // blanks the current cell or Visual selection via SetCellRaw(...,"").
    void HandleSheetNormalInput();
    // Edits SheetSession::edit_buffer as a plain string (no rich-text
    // spans to preserve -- a cell's raw text is either a literal or a
    // formula, nothing in between); Enter/Escape commits it into the
    // cell via SetCellRaw and exits to SheetNormal, Enter additionally
    // moving the cursor down one row (a real spreadsheet's own "Enter
    // commits and advances" convention, distinct from Office/the main
    // buffer's Insert-mode Escape/Enter handling).
    void HandleSheetInsertInput();
    // Selection is [min(anchor,cursor), max(anchor,cursor)] independently
    // on each axis (SheetSession::has_selection/sel_anchor_row/col) -- a
    // rectangular cell range, not a text span. hjkl extend it; a
    // clear-range key blanks every cell in it.
    void HandleSheetVisualInput();
    // Snapshot-based undo push (mirrors PushUndoOffice's own convention):
    // copies wb onto undo_stack and clears redo_stack. Called once per
    // edit commit (entering SheetInsert, or a Visual-mode clear), never
    // per keystroke.
    void PushUndoSheet();
    void UndoSheet();
    void RedoSheet();
    // Mirrors OpenOfficeInPlace exactly (dedup-by-filename, decode,
    // register the session, explicit pending_g_ reset). Keeps a copy of
    // `bytes` in the new session's original_bytes for a future save to
    // copy untouched ZIP entries from (xlsx/ods only -- csv has no
    // container to copy through).
    void OpenSheetInPlace(const std::string &path, const unsigned char *bytes, size_t len);
    // hjkl/arrows move focus across columns/cards (columns = the parsed
    // outline's todo_keywords then done_keywords, in that order); ':' and
    // the leader key are forwarded exactly like HandleSheetNormalInput.
    // Enter/'i' seeds KanbanSession::edit_buffer from the focused card's
    // title and enters Mode::KanbanInsert; 'n' prompts for a new card's
    // title (Mode::Prompt) and appends it via KanbanNewCard; 'x'/'dd'
    // deletes the focused card's subtree via KanbanDeleteCard.
    void HandleKanbanNormalInput();
    // Mirrors HandleSheetInsertInput's plain-string edit_buffer editing;
    // Enter commits via KanbanRenameCard and returns to KanbanNormal,
    // Escape discards.
    void HandleKanbanInsertInput();
    // If Buf().lines has no "#+TODO:" line yet (FindOrgTodoLineIndex
    // returns -1), inserts FormatTodoLine({"TODO"}, {"DONE"}) at line 0 --
    // the same implicit default ParseOrgOutline already falls back to when
    // parsing a file with none -- and reparses the current Kanban session's
    // outline, so KanbanAddColumn/RenameColumn/DeleteColumn can all assume
    // the line exists. A no-op if it already does.
    void EnsureOrgTodoLine();
    // Up/down moves the focused row (one per outline headline with a
    // SCHEDULED date); left/right pans GanttSession::anchor_day; +/- zooms
    // pixels_per_day; 't' cycles the day/month/year ruler grid; double-click
    // (or 'i') on a row's label enters
    // GanttInsert. ':' and the leader key are forwarded. Bar/edge dragging
    // (main.cpp's UpdateGanttMouseInteraction calling GanttShiftHeadline/
    // GanttSetHeadlineDate) and direct timeline clicks reschedule/resize.
    void HandleGanttNormalInput();
    // Mirrors HandleKanbanInsertInput's plain-string edit_buffer editing;
    // Enter commits via GanttRenameHeadline and returns to GanttNormal,
    // Escape discards.
    void HandleGanttInsertInput();
    // Sets mode_ to Mode::Image if the active pane's buffer is image-backed,
    // or drops Mode::Image back to Mode::Normal if it just stopped being
    // so -- called from every site that can change which buffer/pane is
    // active (NavigatePaneDirection, buffer/tab switching, pane close) so
    // plain hjkl always means the right thing for whatever's now focused.
    // Independent of the analogous Mode::Terminal <-> Mode::Normal checks
    // already at each of those sites (untouched by this).
    void SyncModeToActivePaneBuffer();
    SidebarInstance *FindSidebarMut(int id);
    // Shared cleanup for all three overlay modes: restores
    // overlay_previous_mode_. Callers invoke+unref the Lua callback
    // themselves first (the three modes each pass different argument
    // shapes to it).
    void RestoreFromOverlay();
    // Checked first, before mode dispatch: mod1+<letter> is global. Returns
    // true if a mapping fired (mode handlers are skipped that frame).
    bool HandleMod1Shortcuts();
    // mod1+j/k scrolls the picker's preview column instead of the ordinary
    // mod1+j/k pane-nav mapping while a picker with an active preview is
    // open -- see HandleMod1Shortcuts' own Mode::Picker special-case,
    // mirroring its existing Mode::Sidebar one. Clamped to
    // [0, line_count-1] so it can't scroll into nothing but also can't
    // scroll past the last line ever coming back into view.
    void ScrollPickerPreview(int delta);
    // Checked alongside HandleMod1Shortcuts, same reasoning: Ctrl-T (new
    // tab), Ctrl-Tab / Ctrl-Shift-Tab (next / previous tab) and
    // Alt-1..Alt-9 (switch to workspace by number) are fixed, global
    // shortcuts independent of whatever mod1_ is currently configured to
    // (mod1 only ever scans letters/Tab, never digits -- see
    // HandleMod1Shortcuts), so they can't just be mod1 mappings themselves.
    bool HandleTabShortcuts();

    bool DispatchNormalKey(int codepoint);
    bool TryLuaMapping(Mode mode, const std::string &key);

    // --- Repeat (`.`) & macros -------------------------------------------
    // Encoding for the non-printable keys that can appear inside a recorded
    // Insert-mode session (`last_change_keys_`/a macro buffer): printable
    // characters are stored as their own positive codepoint (as
    // GetCharPressed() yields), these small negative sentinels cover the
    // rest. Never passed to DispatchNormalKey (which rejects <= 0 anyway --
    // Normal-mode replay only ever contains plain printable keys); consumed
    // by ProcessInsertKey, and by PlayMacro's own Normal-mode special-casing
    // for the handful of commands (Escape to cancel, Ctrl-R, Ctrl-W) that
    // live outside DispatchNormalKey entirely.
    enum ReplayKey {
        kReplayEscape = -1,
        kReplayEnter = -2,
        kReplayBackspace = -3,
        kReplayDelete = -4,
        kReplayCtrlR = -5,
        kReplayCtrlW = -6,
        // Insert-mode-only editing chords (Phase 8) -- kept distinct from
        // Normal-mode's kReplayCtrlW above (pane-command prefix) even
        // though both start from the same physical Ctrl-W, since they mean
        // completely different things and only one is ever valid depending
        // on mode_ at replay time.
        kReplayInsertCtrlW = -7,
        kReplayInsertCtrlU = -8,
        // Ctrl-V (Visual Block entry): unlike every other printable Visual-
        // mode key, real input reaches it via HandleNormalInput's
        // GetKeyPressed()-queue Ctrl-combo scan (no char event while Ctrl is
        // held -- see that function's own comment), not GetCharPressed(), so
        // it has no natural printable-codepoint encoding of its own. Routed
        // through ProcessNormalKey with this sentinel (which special-cases
        // it ahead of DispatchNormalKey, itself never seeing it) so it
        // participates in `.`/macro recording exactly like plain 'v'/'V'
        // already did.
        kReplayCtrlV = -9,
    };
    // Normal-mode key entry point used by both real input (HandleNormalInput)
    // and replay (RepeatLastChange/PlayMacro): wraps DispatchNormalKey with
    // the bookkeeping that decides what becomes the next `.`-repeatable
    // change and what gets appended to an in-progress macro recording.
    void ProcessNormalKey(int codepoint);
    // Insert-mode equivalent of ProcessNormalKey, driving InsertChar/
    // InsertNewline/Backspace/DeleteForward/EnterNormal from either a real
    // keystroke or a replayed one (see ReplayKey above).
    void ProcessInsertKey(int key);
    // Visual-mode (Visual/VisualLine/VisualBlock) equivalent of
    // ProcessNormalKey/ProcessInsertKey: wraps DispatchVisualKey (the actual
    // per-key body formerly inlined in HandleVisualInput's own loop) with
    // the same macro/`.`-repeat bookkeeping. A Visual "change" spans every
    // key from mode entry (v/V/Ctrl-V, already recorded by ProcessNormalKey
    // -- see IsMidNormalCommand's Visual-mode carve-out below) through the
    // operator that finally commits it, so unlike Normal mode's "one
    // top-level key = one command" boundary, this doesn't reset
    // change_scratch_ at the start of every call; it only *finalizes* it,
    // and only at two points: (1) an operator that exits back to Normal
    // (d/x/y/~/u/U -- c and Visual Block I/A go to Insert instead, deferred
    // to ProcessInsertKey's own Escape-triggered commit, same as Normal-mode
    // `c{motion}`), detected as "we're in Normal mode with nothing else
    // pending right after this key's dispatch"; (2) '>'/'<', which
    // deliberately keep you in Visual mode afterward (see their own case in
    // DispatchVisualKey) so further presses can indent again -- detected as
    // "this exact keystroke edited the buffer and we're still in Visual
    // mode afterward" (the only way that combination happens), which
    // commits *without* clearing change_scratch_/change_recording_active_,
    // so a later `.` of an in-progress multi-`>` session replays the whole
    // accumulated sequence rather than just the last bare '>'.
    void ProcessVisualKey(int key);
    // The actual per-key Visual-mode dispatch (register/count/find/g-prefix/
    // text-object/motion resolution, then the d/x/y/~/c/u/U/>/</I/A/o
    // switch) -- called once per key by ProcessVisualKey. Every `codepoint`
    // must be a plain printable 1-127 char, same restriction DispatchNormalKey
    // has (no ReplayKey sentinels -- Escape is handled by HandleVisualInput
    // itself before the char loop, same as Normal mode's own bare Escape).
    void DispatchVisualKey(int codepoint);
    // True while any pending_* state means the *next* key is a continuation
    // of an in-progress command rather than the start of a new one --
    // shared by ProcessNormalKey's "did a command just finish" check and by
    // the real Escape handler / CancelPendingNormalState.
    bool IsMidNormalCommand() const;
    // Resets every pending_* flag to "nothing in progress", shared by the
    // real Escape key handler and by a replayed kReplayEscape.
    void CancelPendingNormalState();
    // `.`: re-runs the last recorded change (see last_change_keys_) via
    // ProcessNormalKey/ProcessInsertKey, exactly as if it were typed again.
    // `override_count`, if nonzero, replaces any count recorded at the
    // front of the change (or is prepended, if the original had none) --
    // matching Vim's "a count given to `.` replaces the original count."
    void RepeatLastChange(int override_count);
    void StartMacroRecording(char reg);
    void StopMacroRecording();
    // @{reg} / @@: replays a previously recorded macro `count` times.
    void PlayMacro(char reg, int count);

    // r{char}: overwrites `count` characters starting at the cursor with
    // `char` (a no-op if the line doesn't have `count` characters left from
    // the cursor -- matching Vim, which refuses rather than running past
    // the end of the line). Distinct from ReplaceChar below (R's per-
    // keystroke overwrite) despite the similar name.
    void ReplaceCharsAtCursor(int codepoint, int count);
    // R (Replace mode): overwrites the character under the cursor instead
    // of inserting before it, remembering what was overwritten (or that
    // there was nothing there, i.e. this keystroke extended the line) on
    // replace_overwritten_ so ReplaceBackspace can restore it.
    void ReplaceChar(int codepoint);
    void ReplaceBackspace();
    // Ctrl-W/Ctrl-U while in Insert or Replace mode.
    void DeleteWordBeforeCursorInInsert();
    void DeleteToLineStartInInsert();

    void EnterInsert();
    void EnterNormal();
    void EnterVisual(bool linewise);
    void EnterVisualBlock();
    // y/d/x over the current Visual Block selection -- op is 'y' or 'd'
    // ('x' in Visual mode is already routed to 'd' by the caller, same as
    // charwise/linewise Visual). Yanks into a blockwise register either
    // way (Vim's own delete-is-also-a-yank behavior, same as every other
    // delete in mep).
    void ApplyVisualBlockOperator(char op);
    // I (at the block's left edge) / A (at its right edge, or at each
    // row's own actual end if the block was extended with $ -- see
    // block_to_eol_). Pads short rows with spaces up to the insert column
    // first if needed (A past a short line), matching Vim.
    void EnterVisualBlockInsert(bool at_end);
    // Replicates block_insert_typed_ onto every row of the block below the
    // first, called from ProcessInsertKey when a block-insert session's
    // Escape closes it. A no-op if nothing was actually typed.
    void FinishVisualBlockInsert();
    // Blockwise counterpart to InsertCharwiseTextAt, used by PasteAfter/
    // PasteBefore when the register being pasted is blockwise: inserts
    // `block`'s rows starting at `at`, one per buffer row below it,
    // extending the buffer with blank lines if the block reaches past the
    // last line. `before`: true for P-style (insert starting at `at.col`
    // itself), false for p-style (starting one column after).
    void PasteBlockAt(CursorPos at, const std::vector<std::string> &block, bool before);
    // Lands the current pane on one jumplist entry (see PushJump/
    // JumpListBack/JumpListForward): switches CurPane().buffer_id first if
    // the entry belongs to a different buffer than the one currently
    // shown, then sets and clamps the cursor -- same shape as
    // SwitchToBufferForLua's own buffer-switch-then-clamp pattern.
    void GoToJumpEntry(const JumpEntry &entry);
    void EnterCommand();
    void EnterSearch(bool forward);
    // Live-updates the "__mep_incsearch" decoration namespace (VIM_PARITY_
    // PLAN.md Phase 4's incsearch stretch item) from the in-progress
    // search_query_: previews the cursor at the next match of the
    // in-progress query from search_anchor_ (restoring the cursor to
    // search_anchor_ first, so each keystroke re-searches from the same
    // origin rather than drifting off the previous keystroke's preview
    // position) and highlights it with the "IncSearch" hl group, or shows
    // nothing if the query is empty or has no match. Called after every
    // keystroke HandleSearchInput processes; also responsible for clearing
    // the namespace and restoring the cursor when the caller (Escape/Enter)
    // is done with the preview.
    void UpdateIncSearch();

    // Plain substring search (no regex -- see VIM_PARITY_PLAN.md's Phase 4
    // stretch item) for `pattern`, one occurrence in the given direction
    // from `from`, *exclusive* of `from` itself (so repeating a search at
    // a match doesn't just find the same spot again). Returns false if
    // `pattern` doesn't occur anywhere on the line(s) searched.
    // find/rfind that respect `:set ignorecase` (ignore_case_) -- used by
    // SearchOnce/ExSubstitute/ExGlobal instead of calling the std::string
    // methods directly, so all three honor the same option automatically.
    size_t CiFind(const std::string &hay, const std::string &needle, size_t from) const;
    size_t CiRfind(const std::string &hay, const std::string &needle, size_t limit) const;
    bool SearchOnce(const std::string &pattern, CursorPos from, bool forward, CursorPos *result) const;
    // SearchOnce from the cursor, wrapping around the whole buffer (and
    // setting `*wrapped`) if not found without wrapping.
    bool FindNext(const std::string &pattern, bool forward, CursorPos *result, bool *wrapped) const;
    // Moves the cursor to the next match of last_search_ in the given
    // direction (which n/N may flip relative to how the search was
    // originally typed), setting a wrap-around or "not found" status
    // message the way Vim does.
    void PerformSearch(bool forward);
    // */#: search for the word under the cursor (plain text, not
    // \<word\>-bounded regex -- see the SearchOnce note above).
    void SearchWordUnderCursor(bool forward);

    // Resolves a mark motion: `cmd` is '`' (exact position, exclusive
    // charwise as an operator target) or '\'' (first non-blank of the
    // mark's line, linewise); `mark` is the mark letter, or `cmd` itself
    // again for the special "previous jump position" mark (`` `` ``/
    // `''`). Returns false if the mark isn't set (or there's no previous
    // jump yet).
    bool ResolveMark(char cmd, char mark, CursorPos *target, bool *linewise) const;
    // Records `pos` as the current buffer's "previous jump position" --
    // call with the cursor's position *before* moving it, from every
    // "big jump" (G, gg, a search, a mark jump) so ``/'' has somewhere to
    // return to. Also pushes onto the current pane's full jumplist (see
    // PushJump) -- the two mechanisms are recorded together since they
    // fire from exactly the same set of "big jump" call sites, but kept as
    // separate storage (has_last_jump/last_jump_from vs. Pane::jumplist)
    // rather than rebuilding ``/'' on top of the jumplist, per
    // VIM_PARITY_PLAN.md's "don't replace the existing mechanism" note.
    void RecordJumpFrom(CursorPos pos);

    // Full jumplist (Ctrl-O/Ctrl-I), Vim's list+index model: pushes `pos`
    // (in buffer `buffer_id`) onto the current pane's jumplist, truncating
    // any forward ("Ctrl-I-able") history past the current index first,
    // same as a new undo-able edit truncates redo history.
    void PushJump(CursorPos pos, int buffer_id);
    // Ctrl-O / Ctrl-I: step backward/forward through the current pane's
    // jumplist, switching buffers if the landed-on entry belongs to a
    // different one. No-ops at either end of the list, matching Vim's
    // silent (bell-only) behavior there.
    void JumpListBack();
    void JumpListForward();

    // Ctrl-C Ctrl-C in a .org buffer: runs the same org-babel
    // "execute the source block at the cursor" machinery :MepOrgBabelExecute
    // already calls (mep.org_babel_execute(), registered by src/main.cpp's
    // embedded Lua) -- looked up in lua_commands_ (the same registry the
    // ':' command-line's own Lua-command fallback consults) and invoked
    // directly, rather than constructing and feeding a literal
    // ":MepOrgBabelExecute" string through the full command-line parser.
    // A no-op in a non-.org buffer, or (mep_org_src_block_at's own
    // existing "nil if not in a block" handling) when the cursor isn't
    // actually inside a #+begin_src block -- neither case is re-checked
    // here, avoiding a second copy of that logic in C++.
    void TryRunOrgBabelAtCursor();

    // Ctrl-C Ctrl-E <format_key>: dispatches to whichever MepOrgExport*
    // Lua command `format_key` selects (see the .cpp for the exact
    // letter->command mapping), the same lua_commands_ lookup +
    // CallRefWithString pattern TryRunOrgBabelAtCursor uses just above.
    // A no-op in a non-.org buffer or for an unrecognized letter --
    // silently absorbed rather than erroring, matching how an
    // unrecognized key after pending_ctrl_w_/pending_g_ is also just
    // dropped.
    void TryRunOrgExport(char format_key);

    // Shifts every mark in the current buffer to account for `count` lines
    // having been inserted (count > 0) or removed (count < 0, |count| lines
    // gone) starting at row `at_row` -- call *after* the underlying
    // Buf().lines.insert/erase so LineCount() already reflects the new
    // state. A mark pointing strictly inside a deleted range clamps to
    // `at_row` rather than going stale or out of bounds (VIM_PARITY_PLAN.md
    // Phase 5's noted stretch goal: marks otherwise just sit at their
    // original {row, col} snapshot and drift out from under the text).
    void ShiftMarksForLineEdit(int at_row, int count);

    // Same idea as ShiftMarksForLineEdit, but for Buf().folds' row ranges
    // (org src-block/headline folds included). Without this, a fold's
    // start_row/end_row only gets corrected the next time a z-fold command
    // runs RecomputeOrgFolds(); any edit in between leaves it pointing at
    // the wrong rows, which made j/k appear to stick or need a second
    // press right at a now-misaligned block boundary.
    void ShiftFoldsForLineEdit(int at_row, int count);

    int LineLen(int row) const;
    void ClampCursor();

    void InsertChar(int codepoint);
    void InsertNewline();
    void Backspace();
    void DeleteForward();

    void PushUndo();

    // Shared by Visual mode's d/x/y and the menu-bar Copy/Cut: operates on
    // the current selection, or the current line if there is none.
    void ApplyOperatorToSelectionOrCurrentLine(char op);

    // Returns the raw pending count (0 if none was typed) and resets it.
    // 0 vs. unset matters: e.g. bare `G` goes to the last line but `5G`
    // goes to line 5, so callers that treat the count as an absolute line
    // number (G, gg) need to distinguish "no count" from "count of 1"
    // themselves, while callers that just repeat a motion N times can use
    // std::max(1, TakeRawCount()).
    int TakeRawCount();

    // Returns the pending register spec (0 = none typed, meaning
    // "unnamed") and whether it was uppercase (append), resetting both.
    void TakeRegisterSpec(char *name, bool *append);
    // Register `name` if nonzero, else the unnamed register. "+ and "*
    // (the system clipboard) both resolve to the unnamed register: mep
    // keeps the unnamed register and the system clipboard in lockstep
    // (Vim's `clipboard=unnamedplus`), so the three are one and the same
    // -- see SyncUnnamedToSystemClipboard / PullSystemClipboard.
    Register &RegisterFor(char name);

    // Pushes the unnamed register's current text to the system clipboard.
    // Called after every write to registers_['"'] (YankRange,
    // ApplyVisualBlockOperator, the hover-doc yank) -- the one place the
    // "unnamed == system clipboard" invariant is maintained from mep's
    // side. Remembers what it pushed (clipboard_synced_text_) so
    // PullSystemClipboard can tell "still ours" from "changed elsewhere".
    void SyncUnnamedToSystemClipboard();
    // The other direction: if the system clipboard holds something other
    // than what mep last pushed (i.e. another app copied since), replace
    // the unnamed register with it -- charwise, or linewise when the text
    // ends in a newline, the same heuristic Vim applies to "* / "+.
    // Called before every read of the unnamed register (p/P, Ctrl-R,
    // Ctrl-Shift-V, terminal paste). A no-op when the clipboard is empty
    // or unchanged, so a blockwise/linewise yank made in mep keeps its
    // shape across a round-trip.
    void PullSystemClipboard();
    // Text of register `name` for an Insert/cmdline-mode Ctrl-R: pulls the
    // system clipboard first when `name` resolves to the unnamed register,
    // and normalizes an uppercase name to its lowercase register. Returns
    // "" for a name that isn't a register mep knows.
    std::string RegisterTextForPaste(int name);
    // Feeds `text` through ProcessInsertKey one codepoint at a time ('\n'
    // as kReplayEnter), i.e. "as if typed": it lands in macro/`.` recording
    // like ordinary typing, and picks up InsertNewline's auto-indent the
    // same way Vim's own Ctrl-R (not Ctrl-R Ctrl-O) does. Used by Insert-
    // mode Ctrl-R {reg} and Ctrl-Shift-V.
    void InsertTextAsTyped(const std::string &text);

    // Resolves a single-key motion (everything in Phase 1 of
    // VIM_PARITY_PLAN.md except the multi-key f/F/t/T and g-prefixed ones,
    // which DispatchNormalKey handles inline since they need an extra
    // pending state of their own). Shared between standalone motion
    // dispatch (moves the real cursor) and operator-pending dispatch
    // (computes a target from a hypothetical start) so the two, and Visual
    // mode's own motion handling, can't drift out of sync. Returns false
    // for an unrecognized key. `inclusive` is set true for motions where,
    // as an operator target, the character *under* `target` should be
    // included (e.g. `e`) rather than excluded (the default, e.g. `w`).
    bool ResolveMotion(char c, CursorPos from, int count, CursorPos *target, bool *linewise, bool *inclusive) const;

    // Resolves f/F/t/T: find `ch` on the current line, `count`'th
    // occurrence, in the direction implied by `cmd`. Returns false if
    // there's no such occurrence (the find fails and any pending operator
    // is cancelled, matching Vim).
    bool ResolveFind(char cmd, CursorPos from, char ch, int count, CursorPos *target) const;

    // Resolves a text object: `scope` is 'i' (inner) or 'a' (around),
    // `obj` names the object itself (w/W/"/'/`/(/)/b/{/}/B/[/]/</>/p).
    // Returns false for an unrecognized or unmatched object (e.g. `di"` on
    // a line with no quotes) -- callers should then leave the buffer
    // untouched, matching Vim's "operator just cancels" behavior.
    bool ResolveTextObject(char scope, char obj, CursorPos cursor, CursorPos *start, CursorPos *end,
                            bool *linewise) const;
    void WordObjectRange(CursorPos cursor, bool around, bool big, CursorPos *start, CursorPos *end) const;
    bool QuoteObjectRange(CursorPos cursor, char quote, bool around, CursorPos *start, CursorPos *end) const;
    bool BracketObjectRange(CursorPos cursor, char open_c, char close_c, bool around, CursorPos *start,
                             CursorPos *end) const;
    void ParagraphObjectRange(CursorPos cursor, bool around, CursorPos *start, CursorPos *end) const;
    // Finds the bracket pair enclosing (or, if the cursor sits exactly on
    // one, starting at) `cursor`, honoring nesting depth. Shared by %'s
    // motion (which searches forward from the cursor for the *nearest*
    // bracket rather than the *enclosing* one, so doesn't use this) and by
    // BracketObjectRange above.
    bool FindEnclosingBracketPair(CursorPos cursor, char open_c, char close_c, CursorPos *open_pos,
                                   CursorPos *close_pos) const;

    // Motion primitives. `linewise` is set to true when the motion should
    // be treated as a whole-line range by an operator (gg/G/j/k).
    // word (alnum/'_' runs and punctuation runs are separate words) vs.
    // WORD (only whitespace separates words) both cross line boundaries at
    // end/start of line, matching Vim.
    CursorPos MoveWordForward(CursorPos from) const;
    CursorPos MoveWordBackward(CursorPos from) const;
    CursorPos MoveWORDForward(CursorPos from) const;
    CursorPos MoveWORDBackward(CursorPos from) const;
    // `big` selects WORD- vs word-boundaries for `e`/`ge`.
    CursorPos MoveWordEndForward(CursorPos from, bool big) const;
    CursorPos MoveWordEndBackward(CursorPos from, bool big) const;
    CursorPos FirstNonBlank(int row) const;
    CursorPos MoveParagraphForward(CursorPos from) const;
    CursorPos MoveParagraphBackward(CursorPos from) const;
    // ( / ): sentence motions. Vim's sentence definition: a sentence ends
    // at '.', '!', or '?', optionally followed by any number of closing
    // ')', ']', '"', '\'' characters, followed by end-of-line or a space/
    // tab -- landing on the first non-blank character of the next
    // sentence. A blank line is also a sentence (and paragraph) boundary.
    // Modeled directly on MoveParagraphForward/Backward above (same
    // row/col walking style, same count-handling in ResolveMotion).
    CursorPos MoveSentenceForward(CursorPos from) const;
    CursorPos MoveSentenceBackward(CursorPos from) const;
    // Shared scan used by both directions above: finds the start of the
    // next sentence at-or-after `from` (the boundary-matching pattern is
    // awkward to scan right-to-left but trivial left-to-right, so
    // MoveSentenceBackward walks this forward from the top of the buffer
    // instead of implementing a separate backward matcher).
    CursorPos NextSentenceStart(CursorPos from) const;
    CursorPos MoveMatchingBracket(CursorPos from) const;

    // Applies operator `op` ('d', 'y', 'c', 'u'/'U' for gu/gU, '>'/'<' for
    // indent/dedent, or 'q' for gq) over the half-open charwise range
    // [start, end) on a single line (or spanning lines), or the inclusive
    // line range [start.row, end.row] when `linewise` is true ('>'/'<'/'q'
    // always treat it as a line range regardless of `linewise`, matching
    // Vim).
    void ApplyOperator(char op, CursorPos start, CursorPos end, bool linewise);
    void DeleteRange(CursorPos start, CursorPos end, bool linewise);
    // Writes into register `reg_name` (0 = unnamed) -- replacing its
    // contents, or appending if `append` (Vim's "A vs. "a) -- and mirrors
    // the final result into the unnamed register too, exactly as Vim does
    // regardless of which named register (if any) was targeted.
    void YankRange(CursorPos start, CursorPos end, bool linewise, char reg_name = 0, bool append = false);
    // Shared charwise/linewise substring-joining logic behind both
    // YankRange and the public CurrentVisualSelectionText() -- factored
    // out so the two can't silently diverge. `end` follows YankRange's
    // own convention: exclusive column for charwise, inclusive row for
    // linewise.
    std::string ExtractRangeText(CursorPos start, CursorPos end, bool linewise) const;
    // Per-character case transform over a range shaped the same way as
    // DeleteRange/YankRange above. `mode`: 'u' lowercase, 'U' uppercase,
    // '~' toggle.
    void ApplyCaseChange(CursorPos start, CursorPos end, bool linewise, char mode);
    // Indents (levels > 0) or dedents (levels < 0) lines [start_row,
    // end_row] by |levels| shiftwidths (a fixed 4 spaces -- no
    // 'shiftwidth'/'expandtab'/tabstop configuration).
    void IndentLines(int start_row, int end_row, int levels);
    // gq: reflows [start_row, end_row] to wrap at text_width_ columns
    // (":set textwidth="/"tw=", default 80), one blank-line-delimited
    // paragraph at a time, reusing each paragraph's first line's indent
    // for every wrapped line.
    void FormatLines(int start_row, int end_row);
    // J/gJ: joins [count, or 2 if unset] lines starting at the cursor.
    // with_space controls whether a single space is inserted at the join
    // point (J) or not (gJ); leading whitespace on each joined-in line is
    // stripped either way.
    void JoinLines(int count, bool with_space);
    // Splices possibly-multi-line `text` into the buffer at `pos`; returns
    // the position just past the inserted text. Shared by PasteAfter/
    // PasteBefore's charwise branch.
    CursorPos InsertCharwiseTextAt(CursorPos pos, const std::string &text);
    void PasteAfter(int count = 1, char reg_name = 0);
    void PasteBefore(int count = 1, char reg_name = 0);

    // Ctrl-D/Ctrl-U (half a screen) and Ctrl-F/Ctrl-B (a full screen):
    // scroll the current pane, carrying the cursor along by the same
    // number of lines so it stays at roughly the same screen row.
    void ScrollHalfPage(bool down);
    void ScrollFullPage(bool down);
    // zz/zt/zb: reposition the view so the cursor's line ends up
    // centered/at the top/at the bottom, without moving the cursor itself.
    void ScrollCursorTo(char where);

    // Mouse-wheel/trackpad scrolling, dispatched once per frame from
    // HandleInput() (before the mode-specific handler, so it applies
    // regardless of which sub-mode the current content type is in --
    // e.g. OfficeNormal/Insert/Visual all get the same office scroll
    // behavior). `dx`/`dy` are this frame's raw GetMouseWheelMoveV()
    // components -- fractional for a trackpad's smooth scroll, closer to
    // +/-1 per notch for a real mouse wheel. Every content type reuses
    // whatever single-step primitive its own keyboard handler already
    // uses (StepVisibleRow, goto_page, pan clamps, etc.) so wheel
    // scrolling can never drift out of sync with keyboard scrolling.
    void HandleMouseWheel(float dx, float dy);
    // Accumulates fractional wheel input into whole-unit steps for a
    // content type whose scroll position is fundamentally discrete (a
    // text/paragraph/grid row or column, not a pixel offset) -- without
    // this, most individual trackpad-scroll frames contribute less than
    // one line and would otherwise round away to nothing, making a slow
    // two-finger swipe feel unresponsive. `accum` is one of the
    // wheel_accum_*_ members below; `delta` is the raw wheel component;
    // `units_per_notch` is how many rows/cols one full mouse-wheel notch
    // (a delta of 1.0) should move. Returns the (possibly zero) whole
    // number of steps to apply this frame and leaves the leftover
    // fraction in `accum` for next frame.
    int WheelSteps(float &accum, float delta, float units_per_notch);
    void WheelScrollTextBuffer(float dx, float dy);
    void WheelScrollOffice(float dx, float dy);
    void WheelScrollSheet(float dx, float dy);
    void WheelScrollPdf(float dx, float dy);
    void WheelScrollImage(float dx, float dy);
    void WheelScrollHtml(float dx, float dy);
    void WheelScrollTerminal(float dy);
    // Scrolls the focused sidebar's scroll_offset directly (not the
    // cursor -- same "wheel pans the view, arrow keys/j/k move the
    // cursor" split every other WheelScroll* above already follows).
    // Clamped only against total row count here; the real bound
    // (total - visible_lines) gets applied next frame by
    // UpdateScrollForSidebar, the same lag ScrollHalfPage/ScrollFullPage's
    // own doc comment already accepts for pane scrolling.
    void WheelScrollSidebar(float dy);
    // Fractional-notch carry-over for each discrete-stepping content
    // type's WheelScroll* above -- see WheelSteps' own comment. Pixel-
    // based content (Pdf/Image/Html scroll_y/pan_x/pan_y) needs no
    // accumulator since it can apply a fractional wheel delta directly.
    float wheel_accum_text_row_ = 0.0f, wheel_accum_text_col_ = 0.0f;
    float wheel_accum_office_para_ = 0.0f, wheel_accum_office_col_ = 0.0f;
    float wheel_accum_sheet_row_ = 0.0f, wheel_accum_sheet_col_ = 0.0f;
    float wheel_accum_term_ = 0.0f;
    float wheel_accum_sidebar_ = 0.0f;
    // Ctrl-A/Ctrl-X: adds `delta` (negative for Ctrl-X) to the first
    // number at or after the cursor on the current line, preserving
    // leading-zero padding the way Vim's default nrformats does, and
    // leaving the cursor on the number's last digit. A no-op if the line
    // has no number from the cursor onward.
    void IncrementNumberAtCursor(long long delta);

    void ExecuteCommandLine(const std::string &cmd);
    // :normal/:norm {keys} -- runs `keys` as literal Normal-mode input via
    // the same ProcessNormalKey/ProcessInsertKey path real keystrokes use
    // (see Phase 7's repeat/macro machinery), so it composes with counts,
    // operators, registers, etc. for free. If `keys` leaves Insert mode
    // open (no closing Escape) or Visual mode active, closes it out at the
    // end, matching Vim. Doesn't support entering/driving Visual mode
    // itself: HandleVisualInput is a separate dispatch loop not wired into
    // Process*Key (see Phase 9's "known gap" note), so a `:normal v...`
    // sequence enters Visual via 'v' but the keys after it are dropped and
    // Visual mode is closed back out rather than acted on.
    void RunNormalKeys(const std::string &keys);

    // Ex-command range/address parsing, shared by :s/:g/:v/:d/:y/:m/:t.
    // `pos` is advanced past whatever was consumed; returns false (leaving
    // `pos` untouched) when nothing at `pos` looks like an address/range at
    // all, so callers can cleanly fall back to the legacy name/args dispatch
    // for ordinary commands (":w", ":tabnew", ...) that happen to start with
    // a letter one of the range-taking commands also uses (":sp"/":vs" vs.
    // ":s"/":v", ":tabnew" vs. ":t").
    bool ParseExAddress(const std::string &cmd, size_t &pos, int *row) const;
    bool ParseExRange(const std::string &cmd, size_t &pos, int *start_row, int *end_row) const;
    // :s/:pattern/replacement/[g] over lines [start_row, end_row] (0-indexed,
    // inclusive). Plain substring match, same as Phase 4's search -- no
    // regex, so no backreferences in `replacement` either.
    void ExSubstitute(int start_row, int end_row, const std::string &pattern, const std::string &replacement,
                       bool global_flag);
    // :g/pattern/{cmd} (invert=false) or :v/pattern/{cmd} / :g!/pattern/{cmd}
    // (invert=true), over lines [start_row, end_row]. Snapshots matching row
    // indices up front, then runs `subcmd` (via a recursive
    // ExecuteCommandLine call) on each with the cursor positioned there,
    // tracking the running line-count delta so later snapshotted rows still
    // land correctly after earlier ones insert/delete lines.
    void ExGlobal(int start_row, int end_row, bool invert, const std::string &pattern, const std::string &subcmd);
    // :m{addr} (move, is_copy=false) / :t{addr} a.k.a. :co (copy,
    // is_copy=true). `dest_row` is 0-indexed "insert after this row";
    // -1 means "insert before row 0" (Vim's address 0 for these two
    // commands specifically).
    void ExMoveOrCopy(int start_row, int end_row, int dest_row, bool is_copy);

    // :q/:q!  -- close the active pane, or quit the application if it's the
    // only pane left anywhere (matching Vim: :q on the last window quits).
    // Refuses (unless force) when the active buffer has unsaved changes.
    void QuitCurrent(bool force);
    // :qa/:qa!  -- quit unconditionally, refusing (unless force) if any
    // buffer anywhere has unsaved changes.
    void QuitAll(bool force);
    bool IsOnlyPaneOverall() const;
    bool AnyBufferModified() const;
    // :wa  -- writes every modified buffer that has a filename. Returns
    // true only if all modified buffers were written (used to gate :wqa).
    bool WriteAllModified();
    bool SaveBuffer(Buffer &buf, const std::string &path);

    // --- Buffer/pane/tab plumbing ---
    Buffer &Buf() { return buffers_[static_cast<size_t>(CurPane().buffer_id)]; }
    const Buffer &Buf() const { return buffers_[static_cast<size_t>(CurPane().buffer_id)]; }
    Pane &CurPane();
    const Pane &CurPane() const;
    SplitNode *FindNode(SplitNode *node, int pane_id) const;
    void CollectLeaves(const SplitNode *node, std::vector<int> &ids) const;
    // Same traversal as CollectLeaves, collecting each leaf's own
    // buffer_id instead of its pane id -- PaneBuffersInActiveTab's own
    // helper.
    void CollectLeafBuffers(const SplitNode *node, std::vector<int> &ids) const;
    // Same traversal again, stopping at the first leaf showing buffer_id
    // and returning its pane id (-1 if none) -- FocusPaneShowingBuffer's
    // own helper.
    int FindPaneIdForBuffer(const SplitNode *node, int buffer_id) const;
    // Shared by NavigatePaneDirection and PaneMoveBufferTabToNeighbor: the
    // best-overlap-then-nearest pane id in `direction` from `from_pane_id`,
    // or -1 if none.
    int FindNeighborPaneId(int from_pane_id, const std::string &direction) const;
    // Shared by ResizeActivePane: appends (node, child_index) for every
    // Split node on the path from `node` down to the leaf holding
    // `pane_id`, root first. Returns false (path left as-is) if `pane_id`
    // isn't found under `node`.
    bool FindPathToPane(SplitNode *node, int pane_id, std::vector<std::pair<SplitNode *, int>> &path) const;
    // Resets `node->shares` to N equal entries if it isn't already exactly
    // one per child (see SplitNode::shares).
    static void EnsureShares(SplitNode *node);
    void EnsureBufferTabSeeded(Pane &p) const;
    // Shared by MoveBufferTabToPane/SplitPaneWithBufferTab: removes
    // buffer_id from source_pane_id's own buffer_tabs (fixing up
    // buffer_tab_index/buffer_id, ClosePane()-ing source_pane_id if that
    // was its last tab) -- see its own .cpp comment for the exact
    // erase-at-the-tab's-own-index fixup rules. Returns false if buffer_id
    // isn't actually one of source_pane_id's tabs.
    bool RemoveBufferTabFromPane(int source_pane_id, int buffer_id);
    // Rebuilds a leaf's Pane list into a fresh tree per ApplyLayout's `kind`.
    std::unique_ptr<SplitNode> BuildSpiralLayout(const std::vector<Pane> &panes, bool horizontal_next) const;
    void UpdateCompletionPopup();
    void CompletionNext();
    void CompletionPrev();
    void AcceptCompletion();

    void UpdateCmdlineCompletion();
    void CmdlineCompletionNext();
    void CmdlineCompletionPrev();
    void AcceptCmdlineCompletion();
    // Removes the leaf pane `pane_id` from within *node_ptr, collapsing any
    // Split node left with only one child into that child. Returns true if
    // found and removed. Must be called with a Split node, not a Leaf --
    // callers handle the "closing the tab's only pane" case separately.
    bool RemovePaneNode(std::unique_ptr<SplitNode> &node_ptr, int pane_id);
    int CreateEmptyBuffer();
    // Returns the id of the buffer for `path`, reusing an already-open
    // buffer with the same filename if there is one. Reads the file if it
    // exists; if it doesn't, starts a new empty buffer named `path` (same
    // as Vim: ":e newfile.txt" is how you create a file, it's not an
    // error). Returns -1 (with a status message set) only on an actual
    // error -- e.g. no filesystem at all under Emscripten. If not null,
    // `existed` is set to whether the file was found on disk.
    int FindOrCreateBuffer(const std::string &path, bool *existed = nullptr);
    // Read-only cousin of FindOrCreateBuffer for ActivityTodoLoad/Save:
    // the id of the live (non-deleted, non-terminal) buffer whose file is
    // `path`, comparing absolute normalized paths so "TODO.org" opened
    // from the cwd matches the panel's absolute default, across every
    // workspace (a worktree's own TODO.org is a different absolute path
    // anyway). -1 if the file isn't open.
    int FindOpenBufferForPath(const std::string &path) const;

    void SplitCurrentPane(SplitDir dir, const std::string &file_arg);
    // `args`: empty runs an interactive shell ($SHELL, falling back to
    // /bin/sh); non-empty is run as a single command line via `shell -c
    // args` (so `:terminal htop` works the same way a real shell's own
    // command-line-in-one-string does). Opens a horizontal split (same as
    // a bare `:split`) with a fresh buffer standing in for the terminal
    // (see TerminalSession -- its actual content is the VTerm grid, not
    // that buffer's text) and enters Mode::Terminal immediately, keys
    // forwarding live to the child.
    void OpenTerminal(const std::string &args);
    void ClosePane();
    void CyclePane(int delta);
    void TabNext();
    void TabPrevious();
    // :bnext/:bn, :bprevious/:bp/:bprev -- cycle CurPane()'s own buffer_id
    // through the flat, ids-are-indices buffers_ vector (SwitchToBufferForLua's
    // own indexing, also what mep.buffer_list()/mep.buffer_switch use),
    // wrapping at either end and skipping any buffer `:bd`/`:bdelete`
    // (BufferDelete, below) has soft-deleted -- buffers_ itself only ever
    // grows (unlike TabNext/TabPrevious's Tabs(), which can shrink via
    // :tabdelete), so this is a live filter over a stable index space,
    // not wrap arithmetic around a since-removed id.
    void BufferNext();
    void BufferPrevious();
    // :bd/:bdelete, :bd!/:bdelete! -- see Buffer::deleted's own comment
    // for why this soft-deletes (marks CurPane()'s own buffer hidden from
    // buffer_list/bnext/bprev and closes it out of every pane/tab
    // currently showing it, falling back to another open buffer or a
    // fresh empty one) rather than actually erasing from buffers_.
    // `force`: skip the unsaved-changes guard (`:bd!`), same E37
    // convention as QuitCurrent/NewBuffer/ReloadCurrentBuffer.
    void BufferDelete(bool force);
    // BufferDelete's actual body, on an explicit buffer id instead of
    // CurPane()'s -- for PaneCloseBufferTabAndDelete, which has already
    // moved CurPane() off the buffer it wants gone by the time it deletes.
    void BufferDeleteById(int target, bool force);
    // Fills `out` with the normalized rect of every pane in *node's subtree.
    void ComputeRects(const SplitNode *node, float x0, float y0, float x1, float y1,
                       std::vector<PaneRect> &out) const;

    std::vector<Buffer> buffers_;
    // Keyed by buffer_id -- one entry per open `:terminal` pane, kept even
    // after the process exits (so its scrollback stays viewable) until
    // the pane itself closes.
    std::unordered_map<int, TerminalSession> terminals_;
    // Keyed by buffer_id -- one entry per open image-viewer pane. Unlike
    // terminals_, never reaped: an image buffer has no live process to
    // outlive its pane, and (like every other Buffer) staying reachable
    // from the buffer picker/`:buffers` after its last pane closes matches
    // how a closed text buffer's entry in buffers_ also just keeps existing.
    std::unordered_map<int, ImageSession> images_;
    // Keyed by buffer_id -- one entry per open PDF-viewer pane, same
    // never-reaped lifetime reasoning as images_ above.
    std::unordered_map<int, PdfSession> pdfs_;
    // Keyed by buffer_id -- one entry per open HTML-preview pane, same
    // never-reaped lifetime reasoning as images_ above.
    std::unordered_map<int, HtmlSession> htmldocs_;
    // Keyed by buffer_id -- one entry per open WYSIWYG office-document
    // pane, same never-reaped lifetime reasoning as images_ above.
    std::unordered_map<int, OfficeSession> officedocs_;
    // Keyed by buffer_id -- one entry per open spreadsheet pane, same
    // never-reaped lifetime reasoning as images_ above.
    std::unordered_map<int, SheetSession> sheetdocs_;
    // Which overlay (if any) each .org buffer is currently being shown
    // through -- absent means Text (see OrgViewMode's own comment).
    // Deliberately separate from kanban_views_/gantt_views_'s own
    // presence: a buffer can accumulate a cached KanbanSession AND a
    // cached GanttSession (the user toggled through both at some point)
    // while only one, or neither, is the buffer's *active* view.
    std::unordered_map<int, OrgViewMode> org_view_mode_;
    // Keyed by buffer_id -- one cached parsed-outline+UI-state entry per
    // .org buffer that's ever been shown as a Kanban board, same never-
    // reaped lifetime reasoning as images_ above (kept even while
    // org_view_mode_ has since switched back to Text, so toggling back in
    // doesn't lose scroll/focus state).
    std::unordered_map<int, KanbanSession> kanban_views_;
    // Same as kanban_views_, for the Gantt-chart view.
    std::unordered_map<int, GanttSession> gantt_views_;
    // Ctrl-\ Ctrl-N (the exit-terminal-mode chord, matching Vim/Neovim's
    // own convention) needs one key of lookahead: a bare Ctrl-\ has to
    // reach the child (some programs bind it, e.g. SIGQUIT) unless the
    // *next* key turns out to be Ctrl-N. True while that first half is
    // still pending a decision.
    bool terminal_pending_ctrl_bs_ = false;
    // WORKSPACES_PLAN.md decision 1: the tab list lives on the active
    // workspace of the active project; Tabs()/ActiveTab() below are the
    // shims every former `tabs_`/`active_tab_` site now goes through.
    std::vector<Project> projects_;
    int active_project_ = 0;
    int next_workspace_id_ = 1;
    int next_project_id_ = 1;
    // Global (not per workspace) so agent-rpc's bare pane ids stay unique
    // across the whole editor.
    int next_pane_id_ = 0;
    // Same global-uniqueness argument as next_pane_id_ (Tab::id's comment).
    int next_tab_id_ = 1;
    std::vector<Tab> &Tabs() { return MutableActiveWorkspace().tabs; }
    const std::vector<Tab> &Tabs() const { return ActiveWorkspace().tabs; }
    Tab &ActiveTab() { return Tabs()[static_cast<size_t>(ActiveWorkspace().active_tab)]; }
    const Tab &ActiveTab() const { return Tabs()[static_cast<size_t>(ActiveWorkspace().active_tab)]; }
    // Creates the one Project/Workspace the editor starts with (the
    // process cwd, workspace "main"); ProjectLoad (Phase 9) re-roots it.
    void BootstrapInitialProject();
    // Builds a one-empty-tab Workspace (not yet inserted anywhere).
    Workspace MakeWorkspace(const std::string &name, const std::string &root, const std::string &branch);
    // WorkspaceNew for a specific (not necessarily active) project.
    int WorkspaceNewIn(Project &project, const std::string &name, const std::string &root, const std::string &branch);
    static const Workspace *FindWorkspaceByNameIn(const Project &project, const std::string &name);
    // Phase 6: folds a parsed `git worktree list` into project
    // `project_id`'s workspaces (see ProjectDetectGit).
    void AdoptWorktrees(int project_id, const std::vector<WorktreeEntry> &entries);
    std::string worktree_dir_override_;
    bool restore_workspaces_ = true;
    bool session_enabled_ = true;
    // Kills a workspace's terminals and soft-deletes its buffers (shared by
    // WorkspaceDelete and ProjectClose).
    void ReleaseWorkspaceResources(int workspace_id);
    Json WorkspaceStateJson(const Project &project) const;
    Json SplitStateJson(const Workspace &ws, const SplitNode &node) const;
    // Rebuilds a split tree from SplitStateJson output with fresh pane ids;
    // `leaves` collects (new pane id, pane JSON) for the caller to open
    // buffers into afterwards, `id_map` old pane id -> new.
    std::unique_ptr<SplitNode> SplitFromStateJson(const Json &node, std::vector<std::pair<int, Json>> &leaves,
                                                  std::unordered_map<int, int> &id_map);
    // Applies one saved workspace's tabs (opens files, terminals); returns
    // the number of files skipped because they no longer exist.
    int RestoreWorkspaceTabs(Workspace &ws, const Json &ws_json);
    uint64_t LayoutFingerprint() const;
    uint64_t last_layout_fingerprint_ = 0;
    double layout_dirty_since_ = -1.0;
    bool layout_dirty_ = false;
    bool persistence_primed_ = false;
    // Decision 4: process cwd == active workspace root, re-applied on
    // every switch. No-op on the wasm build.
    void ChdirToActiveRoot();
    // Shared tail of every workspace/project activation: leaves Terminal
    // mode, re-syncs the mode to the newly visible pane, bumps the epoch.
    void AfterWorkspaceActivated();
    int workspace_change_epoch_ = 0;

    // Namespace name -> id, global (not per-buffer) so the same name
    // always maps to the same id everywhere, mirroring nvim_create_namespace.
    std::unordered_map<std::string, int> namespace_ids_;
    int next_namespace_id_ = 1;

    // Theme engine state. current_theme_groups_ is rebuilt (BuildHighlightGroups
    // in editor.cpp) whenever ApplyTheme() succeeds; main.cpp's ResolveHlGroup
    // reads it every frame rather than caching, so it's always current.
    std::string current_theme_name_;
    std::unordered_map<std::string, ThemeColor> current_theme_groups_;
    int theme_epoch_ = 0;  // bumped by ApplyTheme; see ThemeEpoch()'s own comment

    Mode mode_ = Mode::Normal;
    // R (Replace mode): true while Mode::Insert should overwrite instead of
    // insert. Kept as a flag on top of Mode::Insert rather than a distinct
    // Mode value, since every other mode check in the codebase only cares
    // about "is this Insert-like" -- reset on EnterInsert()/EnterNormal().
    bool replace_mode_ = false;
    // Per-keystroke record of what ReplaceChar overwrote, in typing order,
    // so ReplaceBackspace can restore characters one at a time; '\0' marks
    // a keystroke that extended the line rather than overwriting anything
    // (Backspace there removes it instead of "restoring" a character).
    std::vector<char> replace_overwritten_;

    // Insert-mode Ctrl-O (distinct from Normal-mode Ctrl-O's jumplist --
    // Vim itself disambiguates the two purely by which mode you're in when
    // you press it): drops into Mode::Normal for exactly one command, then
    // hops back to Insert automatically. Set when Ctrl-O is pressed in
    // Insert; ProcessNormalKey checks it after every dispatched key (the
    // same "is a command still mid-flight" test it already uses for change
    // recording) and flips back once the one command has actually finished.
    bool insert_one_shot_normal_ = false;
    // Remembers whether Replace mode (`R`) was active before the one-shot
    // started, since EnterNormal() unconditionally clears replace_mode_ --
    // restored when hopping back to Insert so `R<C-o>...` resumes as Replace,
    // not plain Insert.
    bool insert_one_shot_was_replace_ = false;

    // --- Modal overlay state (Prompt/Confirm/Select) ---
    Mode overlay_previous_mode_ = Mode::Normal;
    std::string prompt_title_, prompt_input_;
    int prompt_callback_ref_ = 0;
    // Set only by BeginPromptNative; HandlePromptInput checks this first
    // (and clears it right after, whichever branch fires) so a *later*
    // BeginPrompt(Lua-ref) call reusing the same overlay doesn't
    // accidentally also invoke a stale native callback from an earlier one.
    std::function<void(const std::string &)> prompt_native_callback_;
    // Masked/hidden-echo input (e.g. an API key prompt): the real typed
    // text still lives in prompt_input_ (and is what on_done_ref receives
    // unchanged) -- only main.cpp's DrawPromptOverlay renders '*' in its
    // place when this is set. See mep.ui_input's opts.masked/opts.password.
    bool prompt_masked_ = false;
    std::string confirm_message_;
    bool confirm_default_yes_ = false;
    int confirm_callback_ref_ = 0;
    std::string select_title_;
    std::vector<std::string> select_items_;
    int select_index_ = 0;
    int select_callback_ref_ = 0;
    std::string preview_title_, preview_text_;

    std::vector<SidebarInstance> sidebars_;
    int next_sidebar_id_ = 1;
    int focused_sidebar_id_ = 0;  // 0 = none
    int sidebar_cursor_ = 0;      // index into the focused sidebar's flattened line list
    // Floating pane (OpenFloatPane): the leaf node itself (null = none),
    // the sidebar row focus returns to on close (0 = none), the
    // workspace/tab it was opened over (ValidateFloatPane closes it when
    // either changes underneath), and the close-time save policy.
    std::unique_ptr<SplitNode> float_node_;
    int float_return_sidebar_id_ = 0;
    int float_return_sidebar_row_ = 0;
    int float_workspace_id_ = 0;
    int float_tab_index_ = 0;
    bool float_save_on_close_ = false;
    int float_on_close_ref_ = 0;
    // Popout (see ToggleSidebarPopout): which sidebar is popped out (0 =
    // none), and the single preview slot it shows -- one slot, not
    // per-instance, since only one sidebar can be popped out at a time.
    int sidebar_popout_id_ = 0;
    // Identity of the row the preview was last requested for (kind +
    // widget id, see RefreshSidebarPopoutPreview) and a force-refresh
    // flag set when the popout opens or the sidebar's sections change.
    std::string sidebar_popout_preview_key_;
    bool sidebar_popout_preview_dirty_ = false;
    std::string sidebar_popout_preview_title_, sidebar_popout_preview_text_;
    std::vector<PickerHlSpan> sidebar_popout_preview_spans_;
    int sidebar_popout_preview_current_row_ = -1;
    int sidebar_popout_preview_scroll_ = 0;

    bool picker_open_ = false;
    std::string picker_title_;
    std::string picker_query_;
    std::vector<PickerItem> picker_items_;
    int picker_selected_ = 0;
    int picker_on_select_ref_ = 0;
    int picker_on_query_change_ref_ = 0;
    int picker_on_key_ref_ = 0;
    int picker_on_select_change_ref_ = 0;
    bool picker_raw_results_ = false;
    std::string picker_preview_text_;
    std::vector<PickerHlSpan> picker_preview_spans_;
    // First preview line drawn (mod1+j/k, see HandleMod1Shortcuts) --
    // reset to 0 by SetPickerPreview so switching the highlighted result
    // doesn't leave a later item's preview scrolled to wherever the
    // previous one happened to be.
    int picker_preview_scroll_ = 0;

    bool roam_graph_open_ = false;
    std::string roam_graph_title_;
    std::vector<RoamGraphNode> roam_graph_nodes_;
    std::vector<RoamGraphEdge> roam_graph_edges_;
    std::string roam_graph_query_;
    int roam_graph_selected_ = 0;
    int roam_graph_on_select_ref_ = 0;

    std::vector<WhichKeyBinding> whichkey_bindings_;
    char leader_key_ = ' ';
    std::string whichkey_prefix_;
    // Sequence prefix (e.g. "o", "oe") -> group label (e.g. "org",
    // "export"), registered via mep.leader_group -- WhichKeyDisplayEntries'
    // own lookup table.
    std::unordered_map<std::string, std::string> whichkey_groups_;
    int statusline_ref_ = 0;
    bool active_todo_ = false;
    std::string active_todo_text_;
    long long active_todo_start_epoch_ = 0;
    int winbar_click_ref_ = 0;
    bool zen_mode_ = false;
    // Speech-to-text recording indicator (Lua-driven, see mep.stt_toggle):
    // purely a display flag for DrawTabBar's mic icon -- the actual
    // recording process/job lives entirely in Lua, this just tells the
    // tab bar whether to show it. Deliberately independent of
    // ParticipantInfo/local_participants_ (not an AI-participant chip).
    bool stt_recording_ = false;

    std::vector<HintMatch> hint_matches_;
    std::string hint_typed_;

    int completion_source_ref_ = 0;
    int completion_accept_hook_ref_ = 0;
    int completion_resolve_hook_ref_ = 0;
    int insert_tab_hook_ref_ = 0;
    // See SetBufferOnEnter's own comment above.
    int enter_hook_buffer_id_ = -1;
    int enter_hook_ref_ = 0;
    bool completion_open_ = false;
    std::vector<CompletionCandidate> completion_items_;
    int completion_selected_ = 0;
    int completion_word_start_col_ = 0;
    // UpdateCompletionPopup's own throttle state -- see its definition
    // (editor.cpp) for why: without this, it re-runs the completion
    // source (an O(buffer size) Lua scan for the default word-based
    // source) every single frame Insert mode is active with a 2+ char
    // prefix, not just on frames where a character was actually typed.
    // "\x01" (not a valid word-prefix -- always either empty for a dot/
    // member-access trigger or >=2 chars otherwise -- see
    // UpdateCompletionPopup) marks "no query yet", distinct from either.
    std::string completion_last_query_prefix_ = "\x01";
    double completion_last_query_time_ = -1e18;

    bool cmdline_completion_open_ = false;
    std::vector<PickerItem> cmdline_completion_items_;
    int cmdline_completion_selected_ = 0;
    int cmdline_completion_word_start_ = 0;

    // --- Hover tooltip state (see ShowHover/MaybeDismissHover above) ---
    bool hover_open_ = false;
    std::string hover_title_;
    std::string hover_text_;
    CursorPos hover_anchor_pos_{};

    // --- Hover focus state (see Mode::HoverFocus's own comment) ---
    // row_/col_ index into hover_text_ split on '\n', vim-normal-mode-caret
    // style (col clamped to the last character, not one-past-the-end).
    int hover_focus_row_ = 0, hover_focus_col_ = 0;
    // First visible raw line when the text is taller than
    // kHoverFocusVisibleRows; kept in sync with row_ by HandleHoverFocusInput
    // itself (minimal-scroll, like a real viewport) since editor.cpp has no
    // way to know the popup's actual on-screen row height to recompute it
    // from scratch each frame the way main.cpp's rendering could.
    int hover_focus_scroll_ = 0;
    bool hover_focus_pending_g_ = false, hover_focus_pending_y_ = false;
    bool hover_focus_selecting_ = false, hover_focus_select_linewise_ = false;
    int hover_focus_select_anchor_row_ = 0, hover_focus_select_anchor_col_ = 0;

    std::string command_line_;
    std::string status_message_;

    std::unique_ptr<mep::collab::CollabSession> collaboration_;
    int collaboration_buffer_id_ = -1;

    // Local (non-socket) participants -- see SetLocalParticipant. Small and
    // short-lived (one entry per in-flight Lua-driven AI stream), so a
    // linear vector keyed by id is fine.
    std::vector<ParticipantInfo> local_participants_;

    // --- Notification state ---
    std::vector<NotifyEntry> toasts_;
    std::vector<NotifyEntry> notify_history_;
    int next_notify_id_ = 1;
    // Cached from the most recent PruneExpiredToasts(now) call (once per
    // frame, before input handling) so Notify() -- called from all over,
    // including Lua callbacks with no time value of their own -- has a
    // "now" to stamp entries with without editor.h/.cpp needing to touch
    // raylib's clock directly.
    double last_notify_now_ = 0.0;
    int notify_sidebar_id_ = 0;
    static constexpr size_t kMaxNotifyHistory = 200;
    static constexpr int kMaxVisibleToasts = 5;

    // The in-progress /{...} or ?{...} query (Mode::SearchForward/
    // SearchBackward); last_search_/last_search_forward_ hold the most
    // recently *confirmed* search (survives leaving search mode, used by
    // n/N/*/#), separate from search_query_ the same way command_line_
    // doesn't need to remember past commands.
    std::string search_query_;
    std::string last_search_;
    bool last_search_forward_ = true;
    // The cursor position when the current /{...} or ?{...} prompt was
    // opened (EnterSearch) -- both the origin incsearch previews matches
    // from and what Escape restores the cursor to on cancel. Also doubles
    // as the range start for a search used as an operator motion
    // (`d/foo<Enter>`) when no operator is pending, mirroring
    // pending_op_start_ for the case where one is.
    CursorPos search_anchor_;

    // :set options (Phase 11) -- deliberately a small, fixed set of plain
    // bools rather than a general options table, matching the plan's own
    // "a small, real set of options mep can actually honor" scope.
    // text_width_ is the one numeric exception (":set textwidth=N"/"tw=N"),
    // for gq's ApplyOperator('q', ...) -> FormatLines wrap width.
    bool ignore_case_ = false;
    bool wrapscan_ = true;
    int text_width_ = 80;
    // :set wrap/nowrap -- whether a buffer row wider than the pane soft-
    // wraps onto extra *visual* rows (main.cpp's DrawPane) instead of
    // running off the right edge. Distinct from text_width_/gq's hard
    // reflow: this never touches buffer content, and the extra visual
    // rows a wrapped row claims are never their own line number (mirrors
    // how a closed fold or org inline image already claims more than one
    // visual slot for one buffer row -- see Wrap()'s own comment).
    bool wrap_ = true;
    // On by default (hybrid "number relativenumber"), unlike every other
    // :set option here -- a plain, unmodified mep should already look
    // like a real editor's default layout (line numbers + a gutter for
    // git/LSP/DAP signs, see ShowRelativeNumbers' own comment) rather
    // than needing an init.lua just to turn on what most users expect.
    bool show_line_numbers_ = true;
    bool show_relative_numbers_ = true;
    bool show_cursorline_ = true;
    // Org inline-image rendering (<leader>oti / mep.org_images_toggle):
    // global, not per-buffer, matching show_line_numbers_/show_cursorline_
    // above -- gates whether DrawPane (main.cpp) substitutes a rendered
    // texture for a row registered in Buffer::org_image_rows instead of
    // drawing its ordinary [[file:...]] text. The registry itself
    // (Buffer::org_image_rows) is kept fresh regardless of this flag (see
    // kBuiltinOrgImages' mep.on_buffer_changed hook) so toggling on shows
    // correct state immediately, with no edit needed first.
    bool org_images_visible_ = false;
    // Org LaTeX/math-mode rendering (<leader>otl / mep.org_latex_toggle):
    // same shape as org_images_visible_ above, but Buffer::org_latex_rows
    // is only ever populated while this is true -- see that field's own
    // comment for why (a multi-line fragment's hidden raw source, via a
    // 'latex'-provider Fold, is a real visible side effect this toggle
    // must own outright, not just gate the texture substitution).
    bool org_latex_visible_ = false;

    // Command-line/search history (Phase 11) -- Up/Down browse
    // command_history_/search_history_ from most-recent backward.
    // *_index_ == -1 means "not currently browsing" (fresh typing);
    // otherwise it's the index into the history vector currently shown.
    // *_saved_ holds whatever was typed before Up was first pressed, so
    // Down can walk back past the newest entry to it instead of "".
    std::vector<std::string> command_history_;
    int cmd_history_index_ = -1;
    std::string cmd_history_saved_;
    std::vector<std::string> search_history_;
    int search_history_index_ = -1;
    std::string search_history_saved_;

    // Keyed by register name: 'a'-'z' for named registers, '"' for the
    // unnamed register (what plain y/d/p use when no "{reg} was given --
    // it always mirrors the most recent yank/delete, matching Vim).
    // Numbered ("0-"9) and special ("%) registers are a documented stretch
    // goal in VIM_PARITY_PLAN.md, not implemented.
    std::unordered_map<char, Register> registers_;
    // What SyncUnnamedToSystemClipboard last pushed to the system
    // clipboard -- PullSystemClipboard compares against this to detect an
    // external copy (see both methods' comments above).
    std::string clipboard_synced_text_;
    // Insert-mode Ctrl-R seen, waiting for the register name that follows
    // (HandleInsertInput). Cancelled by any non-character key.
    bool insert_pending_ctrl_r_ = false;
    // Same, for the ':' command line and '/' '?' search prompts
    // (HandleCommandInput / HandleSearchInput) -- one flag serves both
    // since the two modes are mutually exclusive.
    bool prompt_pending_ctrl_r_ = false;
    // "{a-z}/"{A-Z} seen before an operator or p/P: which register it
    // should use (0 = none typed, meaning "unnamed"), and whether it was
    // uppercase (append to the existing contents rather than replace).
    // Set by the '"' prefix handling in DispatchNormalKey and consumed
    // (via TakeRegisterSpec) at the point the operator/paste actually
    // runs, so it survives an intervening count/motion/text-object the
    // same way pending_op_ does.
    bool awaiting_register_name_ = false;
    char pending_register_ = 0;
    bool pending_register_append_ = false;

    // Operator pending a motion (e.g. 'd' waiting for 'w'/'$'/'d'...). 0 if
    // none pending.
    char pending_op_ = 0;
    CursorPos pending_op_start_;
    // Count typed before the operator itself (e.g. the "2" in "2d3w"),
    // captured via TakeRawCount() when pending_op_ is set, separately from
    // whatever count precedes the motion that completes it -- Vim
    // multiplies the two together.
    int pending_op_count_ = 0;
    // 'g' waiting for a second key (gg / ge / gE).
    bool pending_g_ = false;
    // '[' / ']' waiting for a second key -- Lua-registered only
    // (mep.map_bracket_prev/mep.map_bracket_next, e.g. "[e"/"]e" for LSP
    // diagnostic navigation); mep has no built-in bracket motion of its
    // own beyond the unrelated i[/a[ text objects (pending_textobj_scope_).
    bool pending_bracket_prev_ = false;
    bool pending_bracket_next_ = false;
    // 'f'/'F'/'t'/'T' waiting for the target character.
    char pending_find_ = 0;
    // 'i' or 'a' waiting for the object key (w, ", (, p, ...) -- set after
    // an operator (or in Visual mode) when the next key is 'i'/'a', 0
    // otherwise. Kept separate from pending_find_/pending_g_ since it can
    // co-occur with a pending operator the same way those do.
    char pending_textobj_scope_ = 0;
    // For ';' and ',' to repeat the last f/F/t/T.
    char last_find_cmd_ = 0;
    char last_find_char_ = 0;
    // '`'/'\'' waiting for the mark letter (a jump *target*, so -- like
    // pending_find_ -- checked whether or not an operator is pending:
    // `` `a `` moves, `` d`a `` deletes to it).
    char pending_mark_jump_ = 0;
    // 'm' waiting for the mark letter to *set*. Kept separate from
    // pending_mark_jump_: unlike ` and ', m is never an operator target.
    bool pending_mark_set_ = false;
    // Ctrl-W waiting for a second key (window commands: w/W/c/s/v).
    bool pending_ctrl_w_ = false;
    // Ctrl-C waiting for a second chord key -- Ctrl-C (org-babel "execute
    // this source block") or Ctrl-E (org export-format dispatch, see
    // pending_org_export_ below), mirroring real Emacs org-mode's own
    // C-c C-c / C-c C-e bindings. Unlike pending_g_/pending_ctrl_w_'s
    // second key (an ordinary unmodified char, consumed via
    // HandleNormalChar's char loop), every key in a Ctrl-C-led chord
    // holds Ctrl, so none of them ever produce a GetCharPressed() char
    // event -- all are read from the raw GetKeyPressed() key-code queue
    // instead (see HandleNormalInput's own Ctrl-combo comment), which
    // makes a "next different key clears the pending state" rule
    // impractical to hook in cleanly here; a short timeout
    // (kCtrlCChordTimeoutSec) is used instead.
    bool pending_ctrl_c_ = false;
    double pending_ctrl_c_time_ = 0.0;
    // Ctrl-C Ctrl-E waiting for the export-format letter (h/p/o/m/a --
    // TryRunOrgExport's own switch has the exact mapping). Unlike
    // pending_ctrl_c_ above, this final key is a bare, unmodified letter
    // -- back to the ordinary pending_g_/pending_ctrl_w_ shape, consumed
    // via HandleNormalChar's char loop with no timeout needed.
    bool pending_org_export_ = false;
    // 'z' waiting for a second key (scroll commands: z/t/b).
    bool pending_z_ = false;
    // 'Z' waiting for a second key -- only ZZ is implemented (a float
    // pane's "confirm" chord, see CloseFloatPane's force_write param and
    // DispatchNormalKey); real vim's other Z-command, ZQ (discard and
    // quit), has no mep equivalent yet. A bare Z outside a float, or Z
    // followed by anything but a second Z, is silently swallowed.
    bool pending_capital_z_ = false;
    // Per-key state for HandleNormalInput's bare-hjkl fast path (see its
    // own comment for the wasm/webview lag this exists to fix). Index:
    // 0=h, 1=j, 2=k, 3=l.
    struct MotionRepeatState {
        // GetTime() this key was last observed to go down (IsKeyDown
        // false -> true), or -1 while it's up. A *duration* below some
        // threshold is deliberately not enough on its own to call this a
        // "hold" -- see HandleNormalInput's own comment on why -- this
        // only becomes meaningful once (now - down_since) clears
        // kMotionHoldConfirmSec.
        double down_since = -1.0;
        // GetTime() until which a queued repeat of this key should be
        // discarded rather than acted on -- extended every frame the key
        // is down and the hold is confirmed, so it's always fresh at the
        // moment of release; left alone (not reset) once release starts
        // it counting down, so a late-arriving stale notification within
        // that window gets discarded even though the key itself has
        // already gone back up.
        double discard_until = -1.0;
    };
    MotionRepeatState motion_repeat_[4];
    // Accumulates digits typed before a command (e.g. the "5" in "5j");
    // 0 means no count was typed. See TakeRawCount().
    int pending_count_ = 0;
    // 'q' waiting for the register letter to start recording into.
    bool pending_macro_record_ = false;
    // '@' waiting for the register letter (or a second '@' for "last
    // played") to replay.
    bool awaiting_macro_play_ = false;
    // 'r' waiting for the replacement character; the count (default 1) is
    // read at the 'r' keypress itself, same as most operators.
    bool pending_replace_ = false;
    int pending_replace_count_ = 0;

    // --- Repeat (`.`) state ------------------------------------------------
    // The most recently committed repeatable change, in the same key
    // encoding as ReplayKey/ProcessNormalKey/ProcessInsertKey use -- empty
    // until the first change. Only ever replaced wholesale (never edited in
    // place) by ProcessNormalKey/ProcessInsertKey once a change completes.
    std::vector<int> last_change_keys_;
    // In-progress accumulation for the change currently being typed;
    // committed into last_change_keys_ once it completes *and* it actually
    // edited the buffer (change_had_edit_), discarded otherwise (a bare
    // motion, a cancelled operator, a search, ...).
    std::vector<int> change_scratch_;
    bool change_recording_active_ = false;
    bool change_had_edit_ = false;
    // Bumped by PushUndo() -- used instead of comparing undo_stack sizes
    // directly, since that stack is capped (kMaxUndo) and silently stops
    // growing once full, which would otherwise make a real edit look like a
    // no-op change.
    int change_epoch_ = 0;
    // Bumped by SaveBuffer() -- lets Lua-side debounced "on save" consumers
    // (Phase 24 symbols refresh, etc.) detect a save via polling, the same
    // pattern as change_epoch_ above, without needing a synchronous
    // callback dispatched from inside the save call stack.
    int save_epoch_ = 0;
    double now_ = 0.0;
    // Set only while RepeatLastChange's own replay loop is running, so a
    // change replayed by `.` doesn't re-record itself as a *new* last
    // change (keeping a `.`-given count override from silently becoming
    // "sticky" for later bare `.` presses) and doesn't get appended into an
    // in-progress macro recording a second time (the literal '.' keystroke
    // that triggered the replay was already recorded by the outer call).
    bool replaying_change_ = false;

    // --- Macro state ---------------------------------------------------
    // q{a-z}/q{A-Z}: recorded keystrokes (ReplayKey encoding, same as
    // last_change_keys_ above), keyed by lowercase register letter --
    // mep's own choice, not literally Vim's registers_ (those hold yanked
    // *text*, not a keystroke sequence; sharing storage would need a
    // variant type for little real benefit here).
    std::unordered_map<char, std::vector<int>> macros_;
    bool recording_macro_ = false;
    char recording_macro_reg_ = 0;
    std::vector<int> macro_recording_buffer_;
    char last_played_macro_ = 0;  // for @@
    // Set only while PlayMacro's own replay loop is running -- same
    // "don't record your own replay" purpose as replaying_change_, but for
    // macro_recording_buffer_ instead: a macro that calls another macro via
    // "@x" records the literal "@x", not x's expansion (matching Vim: what
    // gets replayed re-resolves live at playback time, e.g. if register x
    // is later redefined). Deliberately does *not* suppress last_change_
    // keys_ bookkeeping -- a change made during macro playback should
    // still become the next `.`-repeatable change, same as if it had been
    // typed directly.
    bool replaying_macro_ = false;
    int macro_replay_depth_ = 0;

    // gv: the mode/anchor/cursor of the Visual selection last exited (by
    // Escape or an operator), captured in EnterNormal(). Global rather
    // than per-buffer/per-pane -- a small simplification versus Vim's
    // per-window memory.
    bool has_last_visual_ = false;
    bool last_visual_linewise_ = false;
    CursorPos last_visual_anchor_;
    CursorPos last_visual_cursor_;

    // --- Visual Block state (Phase 9) ---------------------------------
    // Set by '$' while in Mode::VisualBlock: the block's right edge
    // becomes "each row's own actual end" (a ragged right edge) instead of
    // a fixed column, matching Vim. Cleared on entering a fresh Visual
    // Block selection and by any other motion that changes the column
    // extent (approximates Vim's "sticky $" -- not a byte-for-byte match
    // of every edge case Vim handles, but the common one).
    bool block_to_eol_ = false;
    // Block-insert (I/A) in progress: which rows/column to replicate
    // block_insert_typed_ onto once Escape closes the Insert session (see
    // FinishVisualBlockInsert).
    bool block_insert_active_ = false;
    int block_insert_top_ = 0;
    int block_insert_bottom_ = 0;
    int block_insert_col_ = 0;
    bool block_insert_eol_ = false;
    std::string block_insert_typed_;

    static constexpr size_t kMaxUndo = 200;
    static constexpr size_t kMaxJumplist = 100;
    // ResizeActivePane's default step when the caller doesn't pass one:
    // 5% of the split's extent per call, and the floor either side of a
    // resize is clamped to so neither pane can be squeezed to nothing.
    static constexpr float kDefaultResizeStep = 0.05f;
    static constexpr float kMinPaneShare = 0.05f;
    // Max gap (wall-clock seconds, see Now()) between the two Ctrl-C
    // presses of a Ctrl-C Ctrl-C chord -- see pending_ctrl_c_'s own
    // comment for why a timeout, not a "next key clears it" rule.
    static constexpr double kCtrlCChordTimeoutSec = 0.6;

    bool should_quit_ = false;

    enum class ModKey { Alt, Control, Shift, Super };
    ModKey mod1_ = ModKey::Alt;
    bool IsMod1Down() const;

    LuaEnv *lua_ = nullptr;
    std::unordered_map<std::string, int> lua_commands_;         // name -> lua ref
    // DapToggleBreakpoint/DapBreakpointLines state: filename -> insertion-
    // ordered 1-indexed line list. Gutter decorations live in the
    // CreateNamespace("dap") namespace -- CreateNamespace is idempotent
    // by name, so no separate cached id is needed here.
    std::unordered_map<std::string, std::vector<int>> dap_breakpoints_;
    std::unordered_map<int, int> termsend_targets_;  // source buffer id -> target (terminal) buffer id
    // GitGutterRefresh/GitNextHunkRow/GitPrevHunkRow/GitPreviewHunkText/
    // GitResetHunk/GitStageHunk state: the most recently computed hunks
    // and the diff base's own line content (1-indexed DiffHunk fields
    // index into git_base_lines_ 1-based, i.e. git_base_lines_[i-1]).
    std::vector<DiffHunk> git_hunks_;
    std::vector<std::string> git_base_lines_;
    // Pointer into git_hunks_, valid only until the next GitGutterRefresh
    // call -- every caller uses it immediately, never stores it.
    const DiffHunk *GitHunkAtCursor() const;
    // SnippetSplice/SnippetJump state -- see SnippetTabstop's own comment.
    bool has_snippet_state_ = false;
    std::vector<SnippetTabstop> snippet_tabstops_;
    int snippet_index_ = 0;
    int snippet_base_row_ = 0;  // 0-indexed
    std::unordered_map<std::string, int> normal_mappings_;      // key -> lua ref
    std::unordered_map<std::string, int> visual_mappings_;      // key -> lua ref
    std::unordered_map<std::string, int> mod1_mappings_;        // key -> lua ref
    std::unordered_map<std::string, int> g_mappings_;            // g-prefixed key -> lua ref
    std::unordered_map<std::string, int> visual_g_mappings_;     // Visual-mode g-prefixed key -> lua ref (see RegisterVisualGMapping)
    std::unordered_map<std::string, int> bracket_prev_mappings_;  // [-prefixed key -> lua ref
    std::unordered_map<std::string, int> bracket_next_mappings_;  // ]-prefixed key -> lua ref
    // Optional human-readable description for a mep.map() binding, keyed
    // by "<mode-char>:<key>" (e.g. "n:gcc", "v:gc") so Normal/Visual
    // mappings of the same key text can't collide. Populated only when
    // the caller passes opts.desc -- entirely separate from whichkey's
    // own WhichKeyBinding registry (leader-sequence only), this is the
    // *general* mep.map registry AllMappingDescriptions() (public, above)
    // exposes for the help picker's keybinding introspection
    // (NVIM_PARITY_PLAN.md Phase 25).
    std::unordered_map<std::string, std::string> mapping_descriptions_;
};

#endif  // MEP_EDITOR_H
