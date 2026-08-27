#ifndef MEP_EDITOR_H
#define MEP_EDITOR_H

#include <functional>
#include <memory>
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
#include "pdf_doc.h"
// Same reasoning as pdf_doc.h above -- OfficeSession holds OfficeDoc (and
// its DocParagraph/DocSpan contents) by value, not behind a unique_ptr, so
// the full definitions need to be visible here.
#include "office_doc.h"
// Same reasoning again -- SheetSession holds a Workbook by value.
#include "sheet_doc.h"
// Same reasoning again -- HtmlSession holds an HtmlDoc (its DOM tree) by
// value.
#include "html_doc.h"
// Same reasoning again -- KanbanSession/GanttSession hold an OrgOutline by
// value.
#include "org_doc.h"

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
};

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
int FuzzyScore(const std::string &str, const std::string &query, std::vector<int> *positions = nullptr);

// Filename/extension -> a Nerd Font icon glyph, UTF-8 encoded (NVIM_PARITY_
// PLAN.md Part II Phase 10). Same Private-Use-Area codepoints nvim-web-
// devicons/mep.nvim's own mep.icons ship (mep.nvim/lua/mep/icons/data.lua's
// M.nerd_font table) -- a widely-deployed, known-good set. Rendered via
// main.cpp's g_icon_font (icon_font_data.h, a pyftsubset subset of Symbols
// Nerd Font Mono covering exactly the codepoints this function and its
// main.cpp counterparts use), not g_font -- see DrawUiText's own comment.
std::string IconForFilename(const std::string &name);

// UTF-8-encodes a single Unicode codepoint. Icon tables below list plain hex
// ints (e.g. 0xf15b) rather than C++ universal-character-name escapes or raw
// multi-byte UTF-8 bytes directly in source: the short escape form is exactly
// four hex digits and the long form exactly eight, and a codepoint above the
// Basic Multilingual Plane (several of mep's own icons are) needs the long
// form, zero-padded -- easy to get subtly wrong by hand, where a plain int
// literal can't be. Icon codepoints are always in the Private Use Area or
// Supplementary PUA-A, never a surrogate half or otherwise invalid scalar,
// so this doesn't need to handle those cases.
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
};

// One flattened, renderable/navigable line of a sidebar: either a section
// header (collapse toggle) or a widget row. Shared by main.cpp's renderer
// and Editor::HandleSidebarInput so the two can't disagree about layout.
struct SidebarLine {
    enum class Kind { SectionHeader, Widget } kind;
    int section_index = 0;
    int widget_index = -1;  // -1 for a header line
    std::string text;
    std::string hl;
};

// A compact palette (mep.nvim's palettes.lua SPECS/FALLBACKS shape,
// scoped down): a handful of named "role" colors that BuildHighlightGroups
// (editor.cpp) expands into the full named highlight-group set every
// chrome/decoration consumer targets by name.
struct Palette {
    std::string name;
    ThemeColor bg, fg, red, green, yellow, blue, purple, cyan, orange, border;
};

// One entry in a picker's item list (NVIM_PARITY_PLAN.md Part I Phase 8).
// `data` is an opaque payload (e.g. a file path) handed back to the
// on_select callback verbatim -- `display` is what's matched/shown.
struct PickerItem {
    std::string display;
    std::string data;
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
    std::unique_ptr<SplitNode> root;
    int active_pane_id = 0;
};

// A pane's position within its tab, normalized to [0,1]. Derived purely
// from the split tree (every split divides its rect into equal shares),
// so directional pane navigation needs no pixel geometry from main.cpp.
struct PaneRect {
    int pane_id = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class LuaEnv;
class VTerm;
struct VTermColor;
class ImageDoc;

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

// A modal (vim-like) text editor: Normal/Insert/Visual/Command modes, a
// handful of motions and operators, undo/redo, buffers/panes/tabs, and a
// ":" command line that can dispatch into Lua. Does not attempt full Vim
// parity -- no registers, marks, text objects, macros, or regex search.
// Pane navigation is directional (NavigatePaneDirection), not just
// cycle-order, and reachable both via Ctrl-W h/j/k/l and mod1+h/j/k/l.
class Editor {
public:
    Editor();
    // Declared (not defaulted here) purely so the compiler-generated body
    // -- which needs to destroy TerminalSession's std::unique_ptr<VTerm>
    // member, requiring VTerm's complete type -- is emitted in editor.cpp
    // (which includes vterm.h) rather than wherever an Editor happens to
    // be destroyed (main.cpp, which has no reason to know about VTerm at
    // all): the classic "incomplete type in unique_ptr" fix.
    ~Editor();

    void SetLuaEnv(LuaEnv *lua) { lua_ = lua; }
    LuaEnv *Lua() const { return lua_; }

    // Reads raylib's input state directly and advances editor state by one
    // frame's worth of key events. Call once per frame.
    void HandleInput();

    // Adjusts the given pane's scroll so its cursor stays visible within
    // `visible_lines` rows. Call once per frame for every rendered pane
    // (each may have a different height depending on the split layout).
    void UpdateScrollForPane(int pane_id, int visible_lines);

    Mode CurrentMode() const { return mode_; }
    // R (Replace mode): true while Mode::Insert should show "REPLACE" in
    // the status line instead of "INSERT" -- see replace_mode_'s own
    // comment for why this isn't a distinct Mode value.
    bool IsReplaceMode() const { return replace_mode_; }
    // :set number/nonumber -- whether main.cpp's renderer should draw a
    // line-number gutter.
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
    bool ShowRelativeNumbers() const { return show_relative_numbers_; }
    // :set cursorline/nocursorline -- whether main.cpp's renderer should
    // tint the active pane's cursor row with the "CursorLine" theme group.
    bool ShowCursorLine() const { return show_cursorline_; }
    // <leader>oti / mep.org_images_toggle -- whether main.cpp's renderer
    // should substitute a rendered texture for a Buffer::org_image_rows
    // row instead of its ordinary [[file:...]] text.
    bool OrgImagesVisible() const { return org_images_visible_; }
    // <leader>otl / mep.org_latex_toggle -- whether main.cpp's renderer
    // should substitute a rendered texture for a Buffer::org_latex_rows row
    // instead of its ordinary math/LaTeX source text. Also consulted by
    // kBuiltinOrgLatex's own mep.org_latex_scan (Lua has no direct field
    // access), which no-ops (after clearing any stale rows/folds) while
    // this is off -- see Buffer::org_latex_rows' own comment for why, unlike
    // OrgImagesVisible(), this needs a real Lua-visible getter.
    bool OrgLatexVisible() const { return org_latex_visible_; }
    // Active pane/buffer -- what most of the UI (statusline, blinking
    // cursor, Visual highlight) cares about.
    const Buffer &CurrentBuffer() const { return Buf(); }
    // The active pane's own buffer id -- CurPane() itself is private
    // (most of the editor reaches it as `this`'s own member, no need for
    // a public accessor), but a couple of lua_env.cpp bindings that
    // aren't Editor members (mep.html_current_origin/mep.html_reload)
    // need the raw id to key into htmldocs_ via GetHtml/ReloadHtmlBuffer.
    int CurrentBufferId() const { return CurPane().buffer_id; }
    // Buffer ids currently shown by a pane in the active tab's own split
    // layout, in that tab's leaf-traversal order (CollectLeafBuffers) --
    // what mep.pane_buffers() exposes for a Lua-side feature that wants
    // to "find an already-open terminal pane" (mirrors mep.nvim's
    // nvim_tabpage_list_wins()/nvim_win_get_buf combination).
    std::vector<int> PaneBuffersInActiveTab() const;
    CursorPos Cursor() const { return CurPane().cursor; }
    int ScrollRow() const { return CurPane().scroll_row; }
    const std::string &CommandLine() const { return command_line_; }
    // The in-progress /{...} or ?{...} query, for the command bar to draw
    // while in Mode::SearchForward/SearchBackward (mirrors CommandLine()
    // above for Mode::Command).
    const std::string &SearchQuery() const { return search_query_; }
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
    void Notify(const std::string &msg, NotifyLevel level = NotifyLevel::Info);
    // Called once per frame with the current wall-clock time (main.cpp's
    // GetTime()) to expire timed-out toasts -- editor.h/.cpp stay
    // raylib-free, so "now" is threaded in rather than queried here.
    void PruneExpiredToasts(double now);
    void DismissToast(int id);
    void DismissAllToasts();
    void ClearNotifyHistory();
    const std::vector<NotifyEntry> &Toasts() const { return toasts_; }
    const std::vector<NotifyEntry> &NotifyHistory() const { return notify_history_; }
    // :MepNotifyPanel -- a sidebar (Phase 7) view onto NotifyHistory(),
    // rebuilt from it each time this is called.
    void ToggleNotifyHistoryPanel();

    bool ShouldQuit() const { return should_quit_; }
    // Polling-based buffer-change/save detection for Lua (mep.buffer_
    // change_epoch()/buffer_save_epoch()) -- a consumer stores the last
    // value it saw and re-runs its (possibly expensive) refresh only when
    // the value differs, checked once per frame via mep.on_frame rather
    // than a synchronous callback fired from inside the edit/save call
    // stack (which would risk re-entrant buffer mutation).
    int ChangeEpoch() const { return change_epoch_; }
    int SaveEpoch() const { return save_epoch_; }
    // Wall-clock seconds since program start, threaded in from main.cpp's
    // raylib GetTime() once per frame (editor.h/.cpp stay raylib-free, so
    // "now" is threaded in rather than queried here -- same pattern as
    // PruneExpiredToasts). mep.now() uses this rather than Lua's own
    // os.clock(), which measures CPU time, not wall-clock time -- under a
    // busy/idle-waiting render loop the two can drift apart enough to
    // make a debounce interval fire far later than intended.
    void SetNow(double t) { now_ = t; }
    double Now() const { return now_; }
    // For the status line: the count typed so far (0 if none), so a
    // half-typed "3d" isn't invisible while it's pending.
    int PendingCount() const { return pending_count_; }
    // For the status line: the register named so far via "{a-z} (0 if
    // none), same reasoning as PendingCount above.
    char PendingRegister() const { return pending_register_; }

    bool HasVisualSelection() const {
        return mode_ == Mode::Visual || mode_ == Mode::VisualLine || mode_ == Mode::VisualBlock;
    }
    bool IsVisualBlock() const { return mode_ == Mode::VisualBlock; }
    // Returns the selection normalized so `start` <= `end` in buffer order.
    void VisualRange(CursorPos &start, CursorPos &end) const;
    // Visual Block's rectangular extent: rows [top, bottom], columns
    // [left, right] (right is -1 if the block is in "to end of line on
    // each row" mode -- see block_to_eol_'s comment -- callers should
    // treat that as "the rest of the row", not a literal column).
    void VisualBlockRange(int &top, int &bottom, int &left, int &right) const;
    // Read-only text of the current Visual selection (charwise/linewise
    // joined with '\n' between lines, blockwise one row's slice per line
    // same as a blockwise register's own text) -- "" if none is active.
    // Shares its extraction logic with YankRange, but writes no register
    // and pushes no undo state, so it's safe to call from anywhere purely
    // to *read* the selection (mep.visual_selection(), send-selection
    // features, ...). See also ExtractRangeText (private).
    std::string CurrentVisualSelectionText() const;

    // --- Multi-pane/tab read access (for main.cpp's renderer) ---
    int TabCount() const { return static_cast<int>(tabs_.size()); }
    int ActiveTabIndex() const { return active_tab_; }
    const SplitNode *ActiveTabRoot() const { return tabs_[active_tab_].root.get(); }
    // Non-const sibling of ActiveTabRoot -- main.cpp's own per-frame pane-
    // geometry capture (mirroring DrawPaneTree's layout math) stashes raw
    // SplitNode* pointers for its border-drag hit-testing (SetPaneBorderShare
    // takes one directly), which needs mutable access; DrawPaneTree/drawing
    // itself stays on the const overload above, unchanged.
    SplitNode *MutableActiveTabRoot() { return tabs_[active_tab_].root.get(); }
    int ActivePaneId() const { return tabs_[active_tab_].active_pane_id; }
    const Buffer &GetBuffer(int buffer_id) const { return buffers_[buffer_id]; }

    // --- Terminal panes (`:terminal`/`:term`, Part VI Phase 27+) ---
    // main.cpp's DrawPane checks this to render straight from the
    // session's VTerm grid instead of the (unused, empty) Buffer text a
    // terminal pane's buffer_id still nominally points at.
    bool IsTerminalBuffer(int buffer_id) const;
    const TerminalSession *GetTerminal(int buffer_id) const;
    // Writes `text` into a real terminal buffer's own PTY (mep.terminal_write,
    // the "send this line to whichever :terminal pane I designated"
    // primitive a Lua-side vim-slime-style feature needs) -- false if
    // `buffer_id` isn't a live terminal (never one, or already exited).
    bool WriteToTerminalBuffer(int buffer_id, const std::string &text);
    // Called once per frame by DrawPane with the terminal pane's current
    // character-cell size; no-ops if unchanged since the last call
    // (cheap to call unconditionally rather than threading a "did this
    // pane's pixel size change" flag down from main.cpp).
    void ResizeTerminal(int buffer_id, int rows, int cols);
    // Called once per frame from main.cpp's main loop, alongside
    // JobManager::Instance().PollAll() -- pumps buffered output for any
    // wasm-backed terminal session (see TerminalSpawn) into its VTerm; a
    // no-op on native builds, which get this for free via JobManager's
    // own callback-driven PollAll instead.
    void PollTerminals();

    // --- Image-viewer panes (opened via LoadFile for a png/jpg/bmp/gif
    // path -- see IsImagePath in image_doc.h) ---
    // main.cpp's DrawPane checks this to render the decoded texture instead
    // of the (unused, empty) Buffer text an image pane's buffer_id still
    // nominally points at.
    bool IsImageBuffer(int buffer_id) const;
    const ImageSession *GetImage(int buffer_id) const;
    // Called once per frame by DrawPane with the pane's current content
    // pixel size; also re-clamps pan_x/pan_y in case the pane shrank since
    // the last call (mirrors ResizeTerminal's per-frame-refresh pattern).
    void ResizeImageViewport(int buffer_id, int w, int h);

    // --- PDF-viewer panes (opened via LoadFile for a .pdf path -- see
    // IsPdfPath in pdf_doc.h). Mirrors the Image-viewer block above; see
    // PdfSession's own comment for why it additionally owns a re-renderable
    // raster buffer instead of a fixed decode. ---
    bool IsPdfBuffer(int buffer_id) const;
    const PdfSession *GetPdf(int buffer_id) const;
    // Pure geometry clamp only (mirrors ResizeImageViewport): re-clamps
    // pan_x against the anchor page's on-screen width. Never triggers a
    // re-render itself -- that's EnsurePdfPagesRastered's job, called
    // separately (every frame from DrawPane, after this). Keeping the
    // resize call side-effect-free avoids thrashing re-renders before
    // HandlePdfInput's own zoom-band logic has settled.
    void ResizePdfViewport(int buffer_id, int w, int h);
    // Renders whichever of {page-1, page, page+1} aren't already cached at
    // the current rendered_scale (clearing the whole cache first if
    // rendered_scale changed since the last call), and evicts everything
    // outside that window. Called once per frame from DrawPane, before
    // reading pdfs_[...].rasters to draw -- cheap on a cache hit (the
    // common case), so safe to call unconditionally every frame rather
    // than only on state-change edges.
    void EnsurePdfPagesRastered(int buffer_id);

    // --- HTML-preview panes (opened via mep.html_open, kBuiltinTextTools
    // -- deliberately *not* reachable from LoadFile's extension dispatch,
    // see Mode::Html's own comment on why). Mirrors the Image-viewer block
    // above. ---
    bool IsHtmlBuffer(int buffer_id) const;
    const HtmlSession *GetHtml(int buffer_id) const;
    void ResizeHtmlViewport(int buffer_id, int w, int h);
    // Clamps scroll_y into [0, max_scroll] -- called from DrawPane's html
    // branch *after* that frame's own LayoutHtmlDoc call, since max_scroll
    // (total layout height minus viewport height) depends on real font
    // metrics this raylib-free file has no access to, the same reasoning
    // OfficeSession's own comment gives for why its scroll-follow math
    // lives in main.cpp instead of here.
    void ClampHtmlScroll(int buffer_id, float max_scroll);
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
    void ReloadHtmlBuffer(int buffer_id, const std::string &origin, const std::string &source,
                           const unsigned char *bytes, size_t len);
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
    void ConvertHtmlBufferToText(int buffer_id);
    void ConvertTextBufferToHtml(int buffer_id);

    // --- WYSIWYG office-document panes (opened via LoadFile for a
    // .docx/.odt path -- see IsDocxPath/IsOdtPath in office_doc.h).
    // Mirrors the Image/PDF-viewer blocks above. ---
    bool IsOfficeBuffer(int buffer_id) const;
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
    void ToggleOfficeFormat(char which);
    // True if `which` (b/i/u) is "on" at the cursor (OfficeNormal) or
    // uniformly on across the current selection (OfficeVisual) -- drives
    // the toolbar button's pressed-look. Read-only, mirrors
    // ToggleFormatOverRange's own all-on check but without mutating
    // anything.
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
    void SetOfficeFontFamily(OfficeFontFamily family);
    void SetOfficeFontSizePt(float pt);
    void SetOfficeColor(unsigned char r, unsigned char g, unsigned char b);
    void ClearOfficeColor();
    void SetOfficeHighlight(unsigned char r, unsigned char g, unsigned char b);
    void ClearOfficeHighlight();
    void ToggleOfficeSuperscript();
    void ToggleOfficeSubscript();
    // Paragraph-level (not span/char-range) setters: alignment and list
    // membership apply to every paragraph the Visual selection touches (or
    // just the cursor's own paragraph outside Visual mode), unlike the
    // span-level setters above. SetOfficeListKind toggles off (back to
    // ListKind::None) when the target paragraph(s) already uniformly have
    // `kind` -- lets one button/key double as both "make this a bulleted
    // list" and "un-bullet it", matching ToggleOfficeFormat's own
    // click-again-to-undo convention.
    void SetOfficeAlignment(DocParagraph::Align align);
    bool OfficeAlignmentActive(DocParagraph::Align align) const;
    void SetOfficeListKind(DocParagraph::ListKind kind);
    bool OfficeListKindActive(DocParagraph::ListKind kind) const;
    // Inserts `utf8` (a special character, or any literal text) at the
    // cursor -- the toolbar's special-character-grid click handler; not
    // format-related at all, just ApplyInsertToParagraph plus cursor
    // advance, same as a typed character in Mode::OfficeInsert.
    void InsertOfficeText(const std::string &utf8);
    // Opens a native prompt (BeginPromptNative) for a LaTeX-subset math
    // expression, then inserts it as a new span with DocFormat::math set
    // (main.cpp's office DrawPane branch renders a math=true run via
    // LayoutMathExpression/DrawMathLayout instead of literal glyphs).
    void InsertOfficeMath();
    // Opens two chained native prompts (rows, then cols), then inserts a
    // DocTable anchored to the paragraph at the cursor (OfficeDoc::tables
    // grows by one; the cursor paragraph's table_ref points at it). Cells
    // start empty.
    void InsertOfficeTablePrompt();
    // Opens a native prompt for a local image file path, decodes it
    // (ImageDoc::LoadFromMemory) to confirm it's a real image and capture
    // its pixel dimensions, then inserts a DocImage anchored the same way
    // InsertOfficeTablePrompt anchors a DocTable. Silently no-ops (a
    // status message, not a crash) if the path doesn't decode.
    void InsertOfficeImagePrompt();
    // Table-cell navigation/editing while OfficeSession::in_table_edit is
    // set (see that field's own comment) -- Tab/Shift-Tab or hjkl move
    // between cells, i/a edit the current cell's plain text, Escape exits
    // back to normal paragraph navigation. Called from
    // HandleOfficeNormalInput/HandleOfficeInsertInput once a table anchor
    // is the cursor paragraph's own table_ref and the user has entered it.
    void EnterOfficeTable(int table_ref);
    void ExitOfficeTable();
    void MoveOfficeTableCell(int dr, int dc);
    // Sets scroll_para/scroll_line_in_para directly -- the one mutation
    // point main.cpp's word-wrap-aware "is the cursor still visible"
    // check (DrawPane) uses to scroll-follow the cursor, since that
    // check's own logic must live in main.cpp (see ResizeOfficeViewport's
    // comment) but the state it adjusts lives here.
    void SetOfficeScroll(int buffer_id, int scroll_para, int scroll_line_in_para);
    // Sets OfficeSession::scroll_follow_last_cursor_para directly -- same
    // reasoning/pairing as SetOfficeScroll just above (the value the scan
    // reacts to lives in main.cpp, the state itself lives here).
    void SetOfficeScrollFollowCursorPara(int buffer_id, int cursor_para);
    // Jumps the cursor to the start of `para` (column 0, selection
    // cleared) -- the Outline panel's click-to-jump (main.cpp's
    // DrawOfficeSidePanels) needs this alongside SetOfficeScroll itself,
    // see its own comment (editor.cpp) for why moving only the scroll
    // position isn't enough.
    void SetOfficeCursorPara(int buffer_id, int para);
    // Sets OfficeSession::cursor_wrap_lines(_para) directly -- same
    // main.cpp-computes/editor.cpp-stores pairing as SetOfficeScroll just
    // above, feeding MoveOfficeCursorVisualLine (below) the word-wrap
    // geometry it can't compute itself. Called once per frame from
    // DrawPane, right where it already word-wraps the cursor's own
    // paragraph for rendering (main.cpp).
    void SetOfficeCursorWrapLines(int buffer_id, int cursor_para, std::vector<std::pair<int, int>> lines);
    // Toolbar zoom +/- buttons (and now Ctrl-scroll, HandleMouseWheel's
    // Office branch): multiplies OfficeSession::zoom by `factor` (>1 to
    // grow, <1 to shrink), clamped to [0.5, 3.0] -- a plain direct
    // multiplier, not the "settle-band folding into base_font_pt" pattern
    // OfficeSession::zoom's own comment describes PdfSession using. No
    // pan to re-center (unlike ApplyImageZoom/ApplyPdfZoom): an office
    // pane's position is cursor_para/cursor_col-derived, not a separate
    // pan_x/pan_y, so zooming doesn't need to touch it.
    void SetOfficeZoom(float factor);
    // 'u'/Ctrl-R in Mode::OfficeNormal (and now the toolbar's Undo/Redo
    // buttons, main.cpp) -- mirror Undo()/Redo()'s own push-the-opposite-
    // stack-then-swap shape, operating on OfficeSession::undo_stack/
    // redo_stack instead of Buffer's. Moved up to this file's public
    // section (was private) since a toolbar button click, unlike a
    // keybinding, calls it from outside the class.
    void UndoOffice();
    void RedoOffice();

    // --- Spreadsheet panes (opened via LoadFile for a .xlsx/.ods/.csv
    // path -- see IsXlsxPath/IsOdsPath/IsCsvPath in sheet_doc.h). Mirrors
    // the Office-pane block above. ---
    bool IsSheetBuffer(int buffer_id) const;
    const SheetSession *GetSheet(int buffer_id) const;
    // Unlike ResizeOfficeViewport, this DOES do the full scroll-follow
    // job itself (see SheetSession::scroll_row's own comment for why --
    // fixed-size grid cells need no font measurement) -- records
    // viewport_w/h and clamps scroll_row/scroll_col so the cursor cell
    // stays visible, no main.cpp-side follow-up call needed.
    void ResizeSheetViewport(int buffer_id, int w, int h);
    // Non-const on purpose, called directly from main.cpp's DrawPane
    // (which only ever sees a `const SheetSession*` via GetSheet) --
    // mirrors EnsurePdfPagesRastered's own shape: on-demand mutation of
    // cached state (here, EvaluateCell's memoized CellValue) driven by
    // what's actually being rendered, not a side effect a const accessor
    // could hide. Returns an Empty CellValue if buffer_id/row/col don't
    // resolve to a real cell.
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
    bool IsKanbanViewActive(int buffer_id) const;
    bool IsGanttViewActive(int buffer_id) const;
    const KanbanSession *GetKanban(int buffer_id) const;
    // Non-const on purpose -- main.cpp's UpdateKanbanMouseInteraction
    // writes drag/focus/drop-target UI state directly (mirrors
    // EvaluateSheetCell's own non-const-for-main.cpp-driven-mutation
    // shape), reserving the Kanban*/Gantt* methods below for the actual
    // org-text-mutating actions.
    KanbanSession *GetKanbanMutable(int buffer_id);
    const GanttSession *GetGantt(int buffer_id) const;
    GanttSession *GetGanttMutable(int buffer_id);

    // Column keyword list a Kanban board renders/navigates -- the parsed
    // outline's todo_keywords then done_keywords concatenated, in that
    // order. A plain read helper over KanbanSession::outline, so main.cpp's
    // DrawKanban/UpdateKanbanMouseInteraction and
    // HandleKanbanNormalInput/HandleKanbanInsertInput don't each duplicate
    // the concatenation.
    std::vector<std::string> KanbanColumns(int buffer_id) const;
    // Document-order indices into KanbanSession::outline.headlines whose
    // todo_keyword matches KanbanColumns(buffer_id)[column_index] -- i.e.
    // "which cards are in this column, top to bottom".
    std::vector<int> KanbanCardsInColumn(int buffer_id, int column_index) const;
    // Document-order indices into GanttSession::outline.headlines that
    // have a SCHEDULED date (see org_doc.h/the plan's documented v1 gap:
    // a headline with no SCHEDULED at all isn't shown in the Gantt view).
    std::vector<int> GanttRows(int buffer_id) const;

    // ":Kanban"/":Gantt" ex-command bodies: parses (or re-parses, if a
    // session already exists for this buffer) Buf().lines into a fresh
    // OrgOutline and switches org_view_mode_ for CurrentBufferId(). A
    // status-message no-op if the current buffer isn't IsOrgBuffer().
    void OpenKanbanView();
    void OpenGanttView();
    // ":Org"/":Text" ex-command body -- sets org_view_mode_ back to Text
    // for CurrentBufferId(); the cached KanbanSession/GanttSession (if any)
    // is left in place so toggling back in doesn't lose scroll/zoom state.
    void CloseOrgView();

    // Every method below performs exactly one PushUndo() + Buf().lines
    // splice (via ReplaceLinesForLua's own erase/insert+PushUndo shape, or
    // a direct equivalent for a same-buffer line *move*), then re-parses
    // `outline` from the fresh lines -- see org_doc.h's top comment for
    // why the parsed outline is always rebuilt, never incrementally
    // patched. All operate on CurPane().buffer_id via Buf(), same
    // convention as PushUndoSheet/ReplaceLinesForLua -- callers (main.cpp)
    // must ensure the relevant pane is focused first.
    void KanbanSetCardColumn(int headline_index, const std::string &new_keyword);
    // Moves headline_index's whole subtree to sit immediately before
    // before_headline_index's subtree (or to the end of the buffer if
    // before_headline_index < 0). A no-op if either index is out of range
    // or before_headline_index falls *within* headline_index's own
    // subtree (would corrupt the move).
    void KanbanMoveCardBefore(int headline_index, int before_headline_index);
    void KanbanRenameCard(int headline_index, const std::string &new_title);
    // Appends "* <column_keyword> <title>" as a new top-level headline at
    // the end of the buffer (v1 gap: no attempt to infer nesting
    // placement -- see the plan's documented scope boundaries). Returns the
    // new headline's index into KanbanSession::outline.headlines (always
    // the last entry, since it's appended at end-of-file) so a caller (the
    // "+ New Card" drag-drop handler) can immediately open it for rename.
    int KanbanNewCard(const std::string &column_keyword, const std::string &title);
    void KanbanDeleteCard(int headline_index);
    // Enters Mode::KanbanInsert targeting a just-created card with an empty
    // edit buffer -- same end state as HandleKanbanNormalInput's 'i' path,
    // but callable from main.cpp's drag-and-drop-create flow (which must
    // enter insert mode itself right on drop, not wait for a key event).
    void KanbanBeginRenameNewCard(int headline_index);

    // Column management: all three rewrite the file's single "#+TODO:"
    // line (inserting one, defaulting to {"TODO"}/{"DONE"}, if the file
    // has none yet -- EnsureOrgTodoLine below).
    //
    // No-op + status_message_ if `name` already names a column. Otherwise
    // appends to the TODO side (a new mid-pipeline stage, before any DONE
    // keywords) and returns its column index (KanbanColumns' concatenated
    // ordering).
    int KanbanAddColumn(const std::string &name);
    // column_index into KanbanColumns' concatenated ordering. Rewrites
    // every headline currently in this column (todo_keyword == the old
    // name) to the new keyword, then the "#+TODO:" line itself -- both
    // must happen together or a headline's keyword token would no longer
    // match anything ParseOrgOutline recognizes.
    void KanbanRenameColumn(int column_index, const std::string &new_name);
    // Moves a column to the gap `before_column` in KanbanColumns' current
    // ordering (0 = before the first column; size() = after the last).
    // The TODO/DONE split stays at its existing position in that ordering,
    // so moving a column across it also changes whether Org treats it as a
    // done keyword.
    void KanbanMoveColumn(int column_index, int before_column);
    // No-op + status_message_ if the column still has cards (deliberately
    // not auto-migrated or destructive -- see the plan) or if deleting it
    // would leave zero columns.
    void KanbanDeleteColumn(int column_index);

    // Shifts both SCHEDULED and DEADLINE (whichever are present) by
    // delta_days -- a drag on the bar's body, preserving its duration.
    void GanttShiftHeadline(int headline_index, int delta_days);
    // Resizes one edge (is_deadline=false moves SCHEDULED, true moves
    // DEADLINE) to land on new_day (an OrgDayNumber-style day-ordinal) --
    // a drag on the bar's left/right edge, or (is_deadline=true on a
    // headline with no DEADLINE yet) dragging a milestone's virtual right
    // edge to create one from scratch. A no-op for a headline with no
    // SCHEDULED at all (nothing to anchor a resize against) or (when
    // is_deadline is false) if new_day would land at or after the
    // existing DEADLINE.
    void GanttSetHeadlineDate(int headline_index, bool is_deadline, long long new_day);
    // Rewrites/creates a :PROGRESS: property in the headline's immediate
    // property drawer. Used by GanttNormal's `p` prompt.
    void GanttSetHeadlineProgress(int headline_index, int progress);
    // Enters Mode::GanttInsert to rename headline_index's title inline --
    // double-click on its label (or 'i') in the Gantt view.
    void GanttBeginRename(int headline_index);
    void GanttRenameHeadline(int headline_index, const std::string &new_title);

    // --- Lua-facing API (called from lua_env.cpp bindings) ---
    std::string GetLineForLua(int row) const;  // 0-indexed
    void SetLineForLua(int row, const std::string &text);
    int LineCountForLua() const;
    // Replaces lines [start_row, end_row) (0-indexed, end exclusive) with
    // `lines` -- a general "splice" primitive (used first by Phase 17's
    // reset-hunk, generically useful for any future multi-line edit like
    // an LSP text edit or formatter output).
    void ReplaceLinesForLua(int start_row, int end_row, const std::vector<std::string> &lines);
    // Replaces a specific buffer's *entire* content by id, regardless of
    // which pane/buffer is currently active (Part VI Phase 27: streaming
    // terminal/Run/REPL output into a background buffer the user isn't
    // necessarily looking at right now).
    void SetBufferLinesForLua(int buffer_id, const std::vector<std::string> &lines);
    // Creates a new empty buffer *without* switching any pane to it (Part
    // VI Phase 27: a dedicated Run/REPL output buffer, populated via
    // SetBufferLinesForLua before the caller splits a pane and switches
    // it into view there).
    int CreateBufferForLua() { return CreateEmptyBuffer(); }
    int BufferCountForLua() const { return static_cast<int>(buffers_.size()); }
    std::string BufferLabelForLua(int buffer_id) const;
    void SwitchToBufferForLua(int buffer_id);
    std::vector<std::string> LuaCommandNames() const;
    void GetCursorForLua(int *row, int *col) const;
    void SetCursorForLua(int row, int col);
    void InsertTextForLua(const std::string &text);
    void SetStatusMessage(const std::string &msg);
    void RequestQuit() { should_quit_ = true; }
    void RegisterLuaCommand(const std::string &name, int lua_ref);
    // `description`: optional (empty = none), from mep.map's opts.desc --
    // recorded in mapping_descriptions_ (private, below) for
    // AllMappingDescriptions() to expose to the help picker.
    void RegisterLuaMapping(Mode mode, const std::string &key, int lua_ref, const std::string &description = "");
    // Every plain mep.map() binding that was given an opts.desc, for the
    // help picker's keybinding introspection (NVIM_PARITY_PLAN.md Phase
    // 25) -- separate from whichkey's own leader-sequence-only registry.
    struct MappingDescription {
        char mode;  // 'n' or 'v'
        std::string key;
        std::string description;
    };
    std::vector<MappingDescription> AllMappingDescriptions() const;
    // Binds a single letter key under the mod1 modifier (see SetMod1) to a
    // Lua callback, globally across all modes. `key` is a bare letter
    // ("h") for mod1+letter, or "S-"/"C-" prefixed ("S-h", "C-h") for
    // mod1+Shift+letter / mod1+Ctrl+letter (whichever of Shift/Ctrl isn't
    // already mod1 itself). Overrides any prior mapping for that exact
    // key, including the startup defaults (mod1+v/s/h/j/k/l/S-h/j/k/l/
    // C-h/j/k/l/d).
    void RegisterMod1Mapping(const std::string &key, int lua_ref);
    // mep.map_g(key, fn): binds a single letter key after a leading "g"
    // in Normal mode (e.g. "d" for "gd") to a Lua callback -- for
    // g-prefixed actions mep's own built-in motions don't already claim
    // (gg/ge/gE/gu/gU/gJ/gv), since a bare mep.map only ever sees a
    // single already-unprefixed keystroke (RegisterLuaMapping's own doc
    // comment) and can't reach anything typed after a pending "g".
    void RegisterGMapping(const std::string &key, int lua_ref);
    // mep.map_bracket_prev(key, fn) / mep.map_bracket_next(key, fn):
    // same shape as RegisterGMapping, but for a leading "[" / "]"
    // instead of "g" (e.g. "e" for "[e"/"]e" LSP diagnostic navigation).
    // Kept as two separate tables/entry points (mirroring pending_g_'s
    // own single table) rather than one keyed by "[e"/"]e" strings, so
    // the consuming dispatch doesn't need to reconstruct which bracket
    // was pressed from state that's already been cleared by the time it
    // runs.
    void RegisterBracketPrevMapping(const std::string &key, int lua_ref);
    void RegisterBracketNextMapping(const std::string &key, int lua_ref);
    // name: "alt" (default), "ctrl", "shift", or "super"/"cmd"/"meta".
    void SetMod1(const std::string &name);
    // direction: "left"/"down"/"up"/"right". Moves focus to the pane best
    // positioned that way from the active one (most overlap along the
    // perpendicular axis, then closest); a no-op if there is none -- unless
    // a sidebar is docked on that edge and open, in which case focus steps
    // into it instead (the innermost one, if more than one shares the
    // edge). Sidebars aren't nodes in the split tree, so this is handled
    // as a special case rather than by extending FindNeighborPaneId's
    // geometry: when called while a sidebar is focused (Mode::Sidebar),
    // the direction pointing back toward the pane content blurs the
    // sidebar (focus returns to whatever pane was active, without closing
    // it -- unlike Escape/q); other directions are a no-op.
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
    // few cells per call instead (only on the axis matching its own dock
    // edge -- `step` is ignored there, since it's a split-tree share
    // fraction, not a cell count).
    void ResizeActivePane(const std::string &direction, float step = 0.0f);
    // Sets the active pane's share of its immediate parent split to an
    // absolute fraction (clamped to [kMinPaneShare, 1 - kMinPaneShare])
    // instead of nudging it by a step like ResizeActivePane -- only
    // meaningful when that parent has exactly two children (a no-op
    // otherwise), since a third sibling would leave it ambiguous which one
    // gives up the rest. Used by project_open to give the readme pane ~2/3
    // of the height and the terminal pane the remaining ~1/3. Exposed to
    // Lua as mep.pane_set_share().
    void SetActivePaneShare(float fraction);

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
    void SplitPaneWithBufferTab(int source_pane_id, int buffer_id, int dest_pane_id, SplitDir dir, bool before);
    // Border-drag resize: sets node->shares[child_index] to `new_share`
    // (clamped so both it and shares[child_index+1] stay >= kMinPaneShare),
    // taking the difference out of shares[child_index+1] so their combined
    // total is preserved -- the N-ary-sibling-aware analog of
    // SetActivePaneShare (which only ever touches a node with exactly 2
    // children, its own immediate parent). Calls EnsureShares itself, so
    // it's safe to call on a node whose shares aren't populated yet.
    void SetPaneBorderShare(SplitNode *node, int child_index, float new_share);
    // Ensures node->shares is populated (EnsureShares) and returns the
    // combined share of children[child_index] and [child_index+1] -- used
    // to capture a border-drag's fixed pair total at drag *start*, before
    // any dragging/SetPaneBorderShare call has happened yet. 0 if
    // child_index is out of range.
    float PaneBorderPairTotal(SplitNode *node, int child_index);

    // `args`: same contract as OpenTerminal (empty = interactive shell,
    // non-empty = `shell -c args`), but attaches the terminal to the
    // currently active pane in place instead of splitting -- for callers
    // that already arranged the pane layout themselves (e.g. the built-in
    // project_open's terminal-below-the-readme split, which needs the new
    // pane on the *bottom* rather than SplitCurrentPane's above/left
    // default). Exposed to Lua as mep.terminal_here().
    void OpenTerminalInPlace(const std::string &args);

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
    void LoadFile(const std::string &path, bool force_text = false);
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
    void DropUnusedInitialBuffer();
    // Returns true on success; false (with a status message set) if the
    // path is empty or the write failed.
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
    std::vector<DirEntry> ListDirectory(const std::string &path) const;

    // Persisted project-root bookmark list (mep.projects() picker, Phase
    // 16). Same native-vs-wasm-bridge split as ListDirectory: native reads/
    // writes $XDG_DATA_HOME/mep/projects.json directly; wasm routes through
    // the `just run-wasm` loopback bridge (empty/no-op without one, e.g. a bare
    // browser tab).
    std::vector<std::string> ListProjects() const;
    void AddProject(const std::string &path);
    void RemoveProject(const std::string &path);

    // --- Menu-facing API (called from main.cpp's menu bar actions) ---
    void Undo();
    void Redo();
    // Copy/Cut the current visual selection, or the current line if not in
    // Visual mode -- the same fallback a GUI menu's Copy/Cut needs when
    // there's no explicit selection.
    void Copy();
    void Cut();
    void Paste();
    // Replaces the active pane's buffer with a single empty line. Refuses
    // (with a status message, like :q) if there are unsaved changes.
    void NewBuffer();
    // Opens the command line pre-filled with `prefix`, e.g. "e " for a
    // menu's Open action, so the user only has to type the filename.
    void BeginCommand(const std::string &prefix);
    // Runs a command line as if the user typed ":<cmd>" and pressed Enter.
    void RunCommand(const std::string &cmd);

    // :CollabJoin / :CollabLeave. Collaboration follows the active text
    // buffer and merges remote CRDT operations at the next input frame.
    struct CollaborationPeerInfo { std::string id, name; int row = 0, col = 0; bool has_location = false; };
    bool CollaborationActive() const;
    std::vector<CollaborationPeerInfo> CollaborationPeers() const;
    bool JumpToCollaborator(const std::string &peer_id);

    // --- Modal overlays (NVIM_PARITY_PLAN.md Part I Phase 3) ---
    // vim.ui.input/vim.ui.select/confirm-dialog equivalents: each takes
    // over input until confirmed/cancelled, restores whatever mode was
    // active before it opened, and invokes `on_done_ref` (a Lua function
    // registered via luaL_ref) exactly once, then unrefs it. Only one
    // overlay may be active at a time (mode_ already enforces that).
    //   Prompt:  on_done(text) on Enter, on_done() [nil] on Escape.
    //   Confirm: on_done(true/false) always (Escape counts as false).
    //   Select:  on_done(1-indexed index) on Enter, on_done() [nil] on Escape.
    void BeginPrompt(const std::string &title, const std::string &default_text, int on_done_ref,
                      bool masked = false);
    // Same as BeginPrompt, but for a native C++ caller (a toolbar button's
    // own click handler in main.cpp, not Lua) that has no Lua function ref
    // to hand it -- HandlePromptInput calls `on_done` directly instead of
    // through lua_->CallRefWithString when this is set, and does NOT call
    // it on Escape (cancel), matching BeginPrompt's own on_done() case
    // simply being skipped rather than called with an empty string.
    void BeginPromptNative(const std::string &title, const std::string &default_text,
                            std::function<void(const std::string &)> on_done);
    void BeginConfirm(const std::string &message, bool default_yes, int on_done_ref);
    void BeginSelect(const std::string &title, std::vector<std::string> items, int on_done_ref);
    // Preview: no callback -- purely informational (e.g. git-gutter's
    // hunk preview), dismissed by any keypress or a click, restoring
    // whatever mode was active before it opened. `text` may contain
    // embedded '\n's; the renderer splits and draws one line each.
    void BeginPreview(const std::string &title, const std::string &text);

    // Read access for main.cpp's renderer.
    const std::string &PromptTitle() const { return prompt_title_; }
    const std::string &PromptInput() const { return prompt_input_; }
    bool PromptMasked() const { return prompt_masked_; }
    const std::string &ConfirmMessage() const { return confirm_message_; }
    bool ConfirmDefaultYes() const { return confirm_default_yes_; }
    const std::string &SelectTitle() const { return select_title_; }
    const std::vector<std::string> &SelectItems() const { return select_items_; }
    int SelectIndex() const { return select_index_; }
    const std::string &PreviewTitle() const { return preview_title_; }
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
    bool ApplyTheme(const std::string &name);
    const std::string &CurrentThemeName() const { return current_theme_name_; }
    std::vector<std::string> ThemeNames() const;
    // Bumped every time ApplyTheme actually changes current_theme_groups_.
    // Exists for caches that bake a ResolveHighlight-derived color into
    // something more expensive to redo than a per-frame redraw (currently
    // just the PDF viewer's recolored page textures) -- comparing this
    // catches a theme change even when nothing else about the cached
    // content changed, which comparing only *that* content's own version
    // (e.g. a PDF page's raster generation) would miss entirely.
    int ThemeEpoch() const { return theme_epoch_; }
    // Exact-name lookup into the active theme's built group map; returns
    // false if `name` isn't a known group (caller decides the fallback --
    // main.cpp's ResolveHlGroup falls back to its substring heuristic so
    // ad hoc decoration hl_group names keep working un-migrated).
    bool ResolveHighlight(const std::string &name, ThemeColor *out) const;
    // Raw palette lookup by theme name (not necessarily the active theme) --
    // for previews that want to show a theme's colors without switching the
    // whole app to it, e.g. the colorscheme picker's swatch preview. Returns
    // false if `name` isn't a registered theme.
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
    ThemeColor ResolveVTermColor(const VTermColor &c, bool is_fg) const;

    // --- Decorations (NVIM_PARITY_PLAN.md Part I Phase 4) ---
    // Returns a stable id for `name`, creating one on first use (mirrors
    // nvim_create_namespace: idempotent by name).
    int CreateNamespace(const std::string &name);
    // Clears/adds operate on the *current* buffer -- the common case for
    // every planned consumer (colorizer, todoscan, git gutter, ...), which
    // all recompute against whatever buffer they're attached to.
    void ClearNamespace(int ns);
    int AddDecoration(int ns, Decoration deco);
    // Flattened view across every namespace in the current buffer, for
    // main.cpp's per-line rendering pass.
    const std::unordered_map<int, std::vector<Decoration>> &CurrentBufferDecorations() const;
    void ClearNamespaceInBuffer(int buffer_id, int ns);
    int AddDecorationToBuffer(int buffer_id, int ns, Decoration deco);

    // --- Folding (NVIM_PARITY_PLAN.md Part I Phase 5) ---
    // za-equivalent: toggles the innermost fold containing the cursor's
    // row, if any.
    void ToggleFoldAtCursor();
    // Toggles the innermost fold containing `row` directly, without moving
    // the cursor there first -- used by the gutter fold-marker click
    // dispatch (main.cpp's DrawPane), which knows the clicked buffer row
    // but shouldn't relocate the cursor just to toggle a fold near it.
    void ToggleFoldAtRow(int row);
    void CreateFold(int start_row, int end_row, bool closed, const std::string &provider = "manual");
    // Removes every fold tagged with `provider` (a provider recomputing
    // its folds calls this before re-adding, mirroring the decoration
    // namespace clear-and-replace pattern).
    void ClearFoldsFromProvider(const std::string &provider);
    // True (and *fold_start_row set) if `row` is hidden inside a closed
    // fold -- i.e. inside one but not that fold's own start row, which
    // stays visible as the fold's summary line.
    bool IsRowHiddenByFold(int row, int *fold_start_row) const;
    const std::vector<Fold> &CurrentBufferFolds() const { return Buf().folds; }
    // j/k (ResolveMotion) step by *displayed* lines, not buffer rows: a
    // closed fold's hidden interior counts as a single line no matter how
    // many rows it spans. `dir` is +1 (down) or -1 (up).
    int StepVisibleRow(int row, int dir) const;

    // Org-mode headline folding (za/zm/zr/zR/zM): rebuilds the current
    // buffer's provider="org" folds from its `*`/`**`/... headline
    // structure. Cheap enough to call before every z-command rather than
    // hook every edit site -- see the .cpp definition for the exact
    // matching rule that preserves open/closed state across a recompute.
    void RecomputeOrgFolds();
    bool IsOrgBuffer() const;
    // zM/zR: force every fold in the current buffer open or closed, and
    // snap fold_level to the corresponding extreme (0 or the deepest
    // nesting present).
    void SetAllFoldsClosed(bool closed);
    // zm/zr: vim's "one level more/less" fold stepping. `delta` is +1
    // (zr, open a level) or -1 (zm, close a level); see Buffer::fold_level
    // for what the stored level means between calls.
    void AdjustFoldLevel(int delta);

    // --- Org inline images (<leader>oti / mep.org_images_toggle) ---
    // Registers/replaces the resolved image path for `row` in the current
    // buffer's org_image_rows -- called once per match by Lua's
    // mep.org_image_scan() (kBuiltinOrgImages, main.cpp), mirroring
    // CreateFold's "one call per range" shape rather than taking a whole
    // replacement map at once.
    void SetOrgImageRow(int row, const std::string &path);
    // Clears every entry -- called by mep.org_image_scan() before
    // rescanning, mirroring ClearFoldsFromProvider's clear-and-replace
    // pattern (there's only ever one "provider" of these, so no provider
    // tag is needed the way folds have one).
    void ClearOrgImageRows();
    // <leader>oti: flips org_images_visible_ and returns the new state (so
    // the Lua-side wrapper can notify the user and, if newly visible,
    // trigger an immediate mep.org_image_scan() rather than waiting for
    // the next debounced buffer-changed tick).
    bool ToggleOrgImages();

    // --- Org LaTeX/math-mode rendering (<leader>otl / mep.org_latex_toggle) ---
    // Registers/replaces the rendered-PNG path, slot count, and last raw
    // source row (see Buffer::OrgLatexRender) for `row` -- called once per
    // fragment by Lua's mep.org_latex_scan() (kBuiltinOrgLatex, main.cpp),
    // mirroring SetOrgImageRow's "one call per match" shape. `end_row ==
    // row` for a single-line fragment.
    void SetOrgLatexRow(int row, const std::string &path, int slots, int end_row);
    // Clears every entry -- called by mep.org_latex_scan() before
    // rescanning (and when the toggle turns off), mirroring
    // ClearOrgImageRows.
    void ClearOrgLatexRows();
    // <leader>otl: flips org_latex_visible_ and returns the new state, same
    // shape as ToggleOrgImages.
    bool ToggleOrgLatex();
    // Appends one inline-math span (see Buffer::OrgLatexInlineSpan) for
    // `row` -- called once per match by mep_org_latex_register_inline
    // (kBuiltinOrgLatex). Appends rather than replaces (unlike
    // SetOrgLatexRow) since a single row can carry more than one inline
    // fragment ("$x$ and $y$ are related").
    void AddOrgLatexInlineSpan(int row, int col_start, int col_end, const std::string &path);
    // Clears every entry -- called by mep.org_latex_scan() before
    // rescanning (and when the toggle turns off).
    void ClearOrgLatexInlineSpans();

    // --- Sidebar/panel widget (NVIM_PARITY_PLAN.md Part I Phase 7) ---
    int CreateSidebar(const std::string &title, const std::string &position, int size);
    void SetSidebarSections(int id, std::vector<SidebarSection> sections);
    void OpenSidebar(int id, bool focus);
    void CloseSidebar(int id);
    void ToggleSidebar(int id, bool focus);
    bool IsSidebarOpen(int id) const;
    const std::vector<SidebarInstance> &Sidebars() const { return sidebars_; }
    int FocusedSidebarId() const { return focused_sidebar_id_; }
    int SidebarCursor() const { return sidebar_cursor_; }
    // Section-header/widget rows in display order -- one section header
    // line, then (if not collapsed) one line per widget, repeated per
    // section. Returns {} for an unknown id.
    std::vector<SidebarLine> FlattenSidebar(int id) const;
    const SidebarInstance *FindSidebar(int id) const;
    void SetSidebarOnKey(int id, int lua_ref);
    // The `id` string of the widget at the sidebar's current cursor line,
    // or "" if the cursor is on a section header (or the sidebar/cursor is
    // invalid) -- what a Phase 15-style on_key handler uses to know which
    // row a key like "rename" or "delete" applies to.
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
    void FocusSidebarRow(int id, int line_index);
    void ActivateSidebarLine(int id, int line_index);
    // Absolute cell-count resize (border-drag's own per-frame update) --
    // the mouse-driven sibling of ResizeActivePane's Mode::Sidebar branch,
    // which only ever nudges by a fixed step. Clamped to the same minimum
    // that branch's own local kSidebarMinSize enforces (kept as a literal
    // here rather than a shared constant -- see this .cpp's own comment).
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
    void OpenPicker(const std::string &title, std::vector<PickerItem> items, int on_select_ref,
                     int on_query_change_ref, int on_key_ref = 0, int on_select_change_ref = 0,
                     bool raw_results = false);
    void ClosePicker();
    // Unlike bare ClosePicker() (which only flips picker_open_/mode_,
    // leaving ref cleanup to the caller -- HandlePickerInput's Escape/Enter
    // paths need the refs to still be valid *after* closing so they can
    // invoke on_select_ref first), this also unrefs all of the picker's
    // registered Lua callbacks. For callers like mep.picker_close() that
    // just want the picker gone without running any of them.
    void ClosePickerDiscardingCallbacks();
    void SetPickerItems(std::vector<PickerItem> items);
    bool IsPickerOpen() const { return picker_open_; }
    const std::string &PickerTitle() const { return picker_title_; }
    const std::string &PickerQuery() const { return picker_query_; }
    int PickerSelected() const { return picker_selected_; }
    // Recomputed on demand (not cached) from the current query -- items
    // scoring < 0 (no match) are dropped, the rest sorted by score desc.
    // Returns `picker_items_` verbatim, unfiltered/unsorted, when the
    // picker was opened with raw_results (see OpenPicker above).
    std::vector<PickerItem> PickerFilteredResults() const;

    // --- Picker preview pane (NVIM_PARITY_PLAN.md Phase 8 gap, closed) ---
    // mep.picker_set_preview(text): a source (e.g. find_files' on_select_
    // change callback, reading the highlighted file) sets the text shown
    // in a second column beside the results list; main.cpp's
    // DrawPickerOverlay only draws that column, and only widens the box
    // for it, when this is non-empty. Cleared on every OpenPicker() so a
    // *later* unrelated picker doesn't inherit a stale preview from
    // whatever was open before it.
    void SetPickerPreview(const std::string &text) {
        picker_preview_text_ = text;
        picker_preview_scroll_ = 0;
    }
    const std::string &PickerPreview() const { return picker_preview_text_; }
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
    void OpenRoamGraph(const std::string &title, std::vector<RoamGraphNode> nodes,
                        std::vector<RoamGraphEdge> edges, int on_select_ref);
    // Unlike bare mode restore, this also unrefs on_select_ref without
    // calling it -- for mep.roam_graph_close() wanting the view gone with
    // no callback fired (mirrors ClosePickerDiscardingCallbacks).
    void CloseRoamGraphDiscardingCallback();
    bool IsRoamGraphOpen() const { return roam_graph_open_; }
    const std::string &RoamGraphTitle() const { return roam_graph_title_; }
    const std::vector<RoamGraphNode> &RoamGraphNodes() const { return roam_graph_nodes_; }
    const std::vector<RoamGraphEdge> &RoamGraphEdges() const { return roam_graph_edges_; }
    const std::string &RoamGraphQuery() const { return roam_graph_query_; }
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
    std::vector<int> RoamGraphFilteredIndices() const;

    // --- Whichkey (NVIM_PARITY_PLAN.md Part II Phase 11) ---
    void SetLeaderKey(char key) { leader_key_ = key; }
    void RegisterWhichKey(const std::string &sequence, const std::string &description, int lua_ref);
    // Enters Mode::WhichKey with an empty prefix -- called when the leader
    // key is pressed in Normal mode (see the char-dispatch loop).
    void TriggerWhichKey();
    const std::string &WhichKeyPrefix() const { return whichkey_prefix_; }
    // Bindings whose sequence starts with the current prefix, each paired
    // with the sequence's remainder (what's still left to type) -- what
    // DrawWhichKeyOverlay lists, and what narrows as the prefix grows.
    std::vector<std::pair<std::string, std::string>> WhichKeyMatches() const;
    // Every registered leader-sequence binding, unfiltered by any typed
    // prefix -- the leader-sequence half of the keybinding-introspection
    // picker (mep.leader_bindings(), NVIM_PARITY_PLAN.md Phase 25), the
    // other half being AllMappingDescriptions() (above) for plain
    // mep.map() bindings.
    std::vector<WhichKeyBinding> AllWhichKeyBindings() const { return whichkey_bindings_; }

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
    // Moves the current pane's active buffer tab to the nearest pane in
    // `direction` (reuses NavigatePaneDirection's neighbor search); a no-op
    // if there's no pane that way.
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
    // words, called after each insert-mode edit with the word prefix
    // immediately before the cursor (empty string closes the popup).
    void SetCompletionSourceRef(int lua_ref) { completion_source_ref_ = lua_ref; }
    bool IsCompletionOpen() const { return completion_open_; }
    const std::vector<PickerItem> &CompletionItems() const { return completion_items_; }
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
    // session's PTY. Split out of HandleTerminalInput so the Ctrl-\
    // Ctrl-N exit chord (which must NOT forward its first half if the
    // second key turns out not to be Ctrl-N) can buffer one key before
    // deciding whether to call this.
    void SendTerminalKey(TerminalSession &sess, int key, int codepoint, bool ctrl);
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
    void TerminalWrite(TerminalSession &sess, const std::string &bytes);
    void TerminalResizeBackend(TerminalSession &sess, int cols, int rows);
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
    // tab) and Alt-1..Alt-9 (jump to tab by number) are fixed, global
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
    // Register `name` if nonzero, else the unnamed register.
    Register &RegisterFor(char name);

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

    // Applies operator `op` ('d', 'y', 'c', 'u'/'U' for gu/gU, or '>'/'<'
    // for indent/dedent) over the half-open charwise range [start, end) on
    // a single line (or spanning lines), or the inclusive line range
    // [start.row, end.row] when `linewise` is true ('>'/'<' always treat
    // it as a line range regardless of `linewise`, matching Vim).
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
    // Fractional-notch carry-over for each discrete-stepping content
    // type's WheelScroll* above -- see WheelSteps' own comment. Pixel-
    // based content (Pdf/Image/Html scroll_y/pan_x/pan_y) needs no
    // accumulator since it can apply a fractional wheel delta directly.
    float wheel_accum_text_row_ = 0.0f, wheel_accum_text_col_ = 0.0f;
    float wheel_accum_office_para_ = 0.0f, wheel_accum_office_col_ = 0.0f;
    float wheel_accum_sheet_row_ = 0.0f, wheel_accum_sheet_col_ = 0.0f;
    float wheel_accum_term_ = 0.0f;
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
    Buffer &Buf() { return buffers_[CurPane().buffer_id]; }
    const Buffer &Buf() const { return buffers_[CurPane().buffer_id]; }
    Pane &CurPane();
    const Pane &CurPane() const;
    SplitNode *FindNode(SplitNode *node, int pane_id) const;
    void CollectLeaves(const SplitNode *node, std::vector<int> &ids) const;
    // Same traversal as CollectLeaves, collecting each leaf's own
    // buffer_id instead of its pane id -- PaneBuffersInActiveTab's own
    // helper.
    void CollectLeafBuffers(const SplitNode *node, std::vector<int> &ids) const;
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
    std::unique_ptr<SplitNode> BuildSpiralLayout(std::vector<Pane> panes, bool horizontal_next) const;
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
    // wrapping at either end. Unlike TabNext/TabPrevious's tabs_ (which can
    // shrink -- :tabdelete), buffers_ only ever grows (there's no :bdelete),
    // so there's no need to skip a since-removed id the way that wrap
    // arithmetic would otherwise have to.
    void BufferNext();
    void BufferPrevious();
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
    std::vector<Tab> tabs_;
    int active_tab_ = 0;
    int next_pane_id_ = 0;

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
    int statusline_ref_ = 0;
    int winbar_click_ref_ = 0;
    bool zen_mode_ = false;

    std::vector<HintMatch> hint_matches_;
    std::string hint_typed_;

    int completion_source_ref_ = 0;
    int completion_accept_hook_ref_ = 0;
    int insert_tab_hook_ref_ = 0;
    bool completion_open_ = false;
    std::vector<PickerItem> completion_items_;
    int completion_selected_ = 0;
    int completion_word_start_col_ = 0;
    // UpdateCompletionPopup's own throttle state -- see its definition
    // (editor.cpp) for why: without this, it re-runs the completion
    // source (an O(buffer size) Lua scan for the default word-based
    // source) every single frame Insert mode is active with a 2+ char
    // prefix, not just on frames where a character was actually typed.
    std::string completion_last_query_prefix_;
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

    std::string command_line_;
    std::string status_message_;

    std::unique_ptr<mep::collab::CollabSession> collaboration_;
    int collaboration_buffer_id_ = -1;

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
    bool ignore_case_ = false;
    bool wrapscan_ = true;
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
    std::unordered_map<std::string, int> normal_mappings_;      // key -> lua ref
    std::unordered_map<std::string, int> visual_mappings_;      // key -> lua ref
    std::unordered_map<std::string, int> mod1_mappings_;        // key -> lua ref
    std::unordered_map<std::string, int> g_mappings_;            // g-prefixed key -> lua ref
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
