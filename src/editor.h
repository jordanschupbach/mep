#ifndef MEP_EDITOR_H
#define MEP_EDITOR_H

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    // A focused Sidebar (Part I Phase 7) takes over input the same way,
    // navigating its flattened section/widget list.
    Sidebar,
    // The fuzzy picker (Part I Phase 8): prompt + live-filtered list.
    Picker,
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

// Filename/extension -> a short ASCII glyph (NVIM_PARITY_PLAN.md Part II
// Phase 10). ASCII-only, not emoji/nerd-font codepoints: the embedded font's
// glyph subset doesn't cover those (confirmed the hard way by Phase 6's
// notification icons, which rendered as tofu "?" until switched to ASCII) --
// upgrading to a richer style is possible later if/when a font atlas with
// those codepoints is loaded, without changing this function's callers.
std::string IconForFilename(const std::string &name);

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
    // Virtual text: inline (drawn before col_start, doesn't touch the
    // buffer) or overlay (drawn *instead of* [col_start, col_start+len)).
    std::string virt_text;
    std::string virt_text_hl;
    bool virt_overlay = false;
    char sign = 0;  // gutter glyph; 0 = none (single byte for now)
    std::string sign_hl;
    int priority = 0;
    // Colorizer swatch (Part III Phase 13): a literal RGB drawn as a small
    // filled square at col_start, bypassing the named-highlight-group
    // system entirely -- the whole point is showing the *exact* parsed
    // color, not its nearest theme role.
    bool has_swatch = false;
    ThemeColor swatch_color;
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

    int LineCount() const { return static_cast<int>(lines.size()); }
};

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
    // Active pane/buffer -- what most of the UI (statusline, blinking
    // cursor, Visual highlight) cares about.
    const Buffer &CurrentBuffer() const { return Buf(); }
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

    // --- Multi-pane/tab read access (for main.cpp's renderer) ---
    int TabCount() const { return static_cast<int>(tabs_.size()); }
    int ActiveTabIndex() const { return active_tab_; }
    // Display label for a tab: the filename (or "[No Name]") of whichever
    // pane is active within it.
    std::string TabLabel(int tab_index) const;
    const SplitNode *ActiveTabRoot() const { return tabs_[active_tab_].root.get(); }
    int ActivePaneId() const { return tabs_[active_tab_].active_pane_id; }
    const Buffer &GetBuffer(int buffer_id) const { return buffers_[buffer_id]; }

    // --- Terminal panes (`:terminal`/`:term`, Part VI Phase 27+) ---
    // main.cpp's DrawPane checks this to render straight from the
    // session's VTerm grid instead of the (unused, empty) Buffer text a
    // terminal pane's buffer_id still nominally points at.
    bool IsTerminalBuffer(int buffer_id) const;
    const TerminalSession *GetTerminal(int buffer_id) const;
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
    void RegisterLuaMapping(Mode mode, const std::string &key, int lua_ref);
    // Binds a single letter key under the mod1 modifier (see SetMod1) to a
    // Lua callback, globally across all modes. `key` is a bare letter
    // ("h") for mod1+letter, or "S-"/"C-" prefixed ("S-h", "C-h") for
    // mod1+Shift+letter / mod1+Ctrl+letter (whichever of Shift/Ctrl isn't
    // already mod1 itself). Overrides any prior mapping for that exact
    // key, including the startup defaults (mod1+v/s/h/j/k/l/S-h/j/k/l/
    // C-h/j/k/l/d).
    void RegisterMod1Mapping(const std::string &key, int lua_ref);
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
    void LoadFile(const std::string &path);
    // Returns true on success; false (with a status message set) if the
    // path is empty or the write failed.
    bool SaveFile(const std::string &path);

    // Directory listing shared by mep.list_dir (Lua, lua_env.cpp -- the
    // file-tree sidebar's data source) and command-line path completion
    // (UpdateCmdlineCompletion): native builds read the real filesystem;
    // wasm builds go through the same loopback bridge as LoadFile/SaveFile
    // (empty when not launched via `just run`). Entries aren't sorted or
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
    // the `just run` loopback bridge (empty/no-op without one, e.g. a bare
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

    // --- Modal overlays (NVIM_PARITY_PLAN.md Part I Phase 3) ---
    // vim.ui.input/vim.ui.select/confirm-dialog equivalents: each takes
    // over input until confirmed/cancelled, restores whatever mode was
    // active before it opened, and invokes `on_done_ref` (a Lua function
    // registered via luaL_ref) exactly once, then unrefs it. Only one
    // overlay may be active at a time (mode_ already enforces that).
    //   Prompt:  on_done(text) on Enter, on_done() [nil] on Escape.
    //   Confirm: on_done(true/false) always (Escape counts as false).
    //   Select:  on_done(1-indexed index) on Enter, on_done() [nil] on Escape.
    void BeginPrompt(const std::string &title, const std::string &default_text, int on_done_ref);
    void BeginConfirm(const std::string &message, bool default_yes, int on_done_ref);
    void BeginSelect(const std::string &title, std::vector<std::string> items, int on_done_ref);

    // Read access for main.cpp's renderer.
    const std::string &PromptTitle() const { return prompt_title_; }
    const std::string &PromptInput() const { return prompt_input_; }
    const std::string &ConfirmMessage() const { return confirm_message_; }
    bool ConfirmDefaultYes() const { return confirm_default_yes_; }
    const std::string &SelectTitle() const { return select_title_; }
    const std::vector<std::string> &SelectItems() const { return select_items_; }
    int SelectIndex() const { return select_index_; }

    // --- Theme engine (NVIM_PARITY_PLAN.md Part II Phase 9) ---
    // Applies a registered palette by name; false (no-op) if `name` isn't
    // registered. main.cpp's ResolveHlGroup() reads the resulting group
    // map -- everything colored through a named highlight group repaints
    // automatically on the next frame, nothing needs to be told.
    bool ApplyTheme(const std::string &name);
    const std::string &CurrentThemeName() const { return current_theme_name_; }
    std::vector<std::string> ThemeNames() const;
    // Exact-name lookup into the active theme's built group map; returns
    // false if `name` isn't a known group (caller decides the fallback --
    // main.cpp's ResolveHlGroup falls back to its substring heuristic so
    // ad hoc decoration hl_group names keep working un-migrated).
    bool ResolveHighlight(const std::string &name, ThemeColor *out) const;

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
    void OpenPicker(const std::string &title, std::vector<PickerItem> items, int on_select_ref,
                     int on_query_change_ref, int on_key_ref = 0);
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
    std::vector<PickerItem> PickerFilteredResults() const;

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

private:
    void HandleNormalInput();
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
    void HandleSidebarInput();
    void HandlePickerInput();
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
    void TerminalKillBackend(TerminalSession &sess);
    SidebarInstance *FindSidebarMut(int id);
    // Shared cleanup for all three overlay modes: restores
    // overlay_previous_mode_. Callers invoke+unref the Lua callback
    // themselves first (the three modes each pass different argument
    // shapes to it).
    void RestoreFromOverlay();
    // Checked first, before mode dispatch: mod1+<letter> is global. Returns
    // true if a mapping fired (mode handlers are skipped that frame).
    bool HandleMod1Shortcuts();

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
    void EnterCommand();
    void EnterSearch(bool forward);

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
    // return to.
    void RecordJumpFrom(CursorPos pos);

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
    void CollectBufferIds(const SplitNode *node, std::vector<int> &ids) const;
    // Kills and forgets any TerminalSession whose buffer_id no longer
    // backs a pane in any tab -- called after anything that can actually
    // remove panes (ClosePane, TabDelete), since a terminal's PTY process
    // has no other owner to tell it to die once its only pane is gone.
    void ReapOrphanedTerminals();
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
    void TabNew(const std::string &file_arg);
    void TabDelete();
    void TabNext();
    void TabPrevious();
    // Fills `out` with the normalized rect of every pane in *node's subtree.
    void ComputeRects(const SplitNode *node, float x0, float y0, float x1, float y1,
                       std::vector<PaneRect> &out) const;

    std::vector<Buffer> buffers_;
    // Keyed by buffer_id -- one entry per open `:terminal` pane, kept even
    // after the process exits (so its scrollback stays viewable) until
    // the pane itself closes.
    std::unordered_map<int, TerminalSession> terminals_;
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

    // --- Modal overlay state (Prompt/Confirm/Select) ---
    Mode overlay_previous_mode_ = Mode::Normal;
    std::string prompt_title_, prompt_input_;
    int prompt_callback_ref_ = 0;
    std::string confirm_message_;
    bool confirm_default_yes_ = false;
    int confirm_callback_ref_ = 0;
    std::string select_title_;
    std::vector<std::string> select_items_;
    int select_index_ = 0;
    int select_callback_ref_ = 0;

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

    std::vector<WhichKeyBinding> whichkey_bindings_;
    char leader_key_ = ' ';
    std::string whichkey_prefix_;
    int statusline_ref_ = 0;
    bool zen_mode_ = false;

    std::vector<HintMatch> hint_matches_;
    std::string hint_typed_;

    int completion_source_ref_ = 0;
    bool completion_open_ = false;
    std::vector<PickerItem> completion_items_;
    int completion_selected_ = 0;
    int completion_word_start_col_ = 0;

    bool cmdline_completion_open_ = false;
    std::vector<PickerItem> cmdline_completion_items_;
    int cmdline_completion_selected_ = 0;
    int cmdline_completion_word_start_ = 0;

    std::string command_line_;
    std::string status_message_;

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

    // :set options (Phase 11) -- deliberately a small, fixed set of plain
    // bools rather than a general options table, matching the plan's own
    // "a small, real set of options mep can actually honor" scope.
    bool ignore_case_ = false;
    bool wrapscan_ = true;
    bool show_line_numbers_ = false;

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
    // 'z' waiting for a second key (scroll commands: z/t/b).
    bool pending_z_ = false;
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
    // ResizeActivePane's default step when the caller doesn't pass one:
    // 5% of the split's extent per call, and the floor either side of a
    // resize is clamped to so neither pane can be squeezed to nothing.
    static constexpr float kDefaultResizeStep = 0.05f;
    static constexpr float kMinPaneShare = 0.05f;

    bool should_quit_ = false;

    enum class ModKey { Alt, Control, Shift, Super };
    ModKey mod1_ = ModKey::Alt;
    bool IsMod1Down() const;

    LuaEnv *lua_ = nullptr;
    std::unordered_map<std::string, int> lua_commands_;         // name -> lua ref
    std::unordered_map<std::string, int> normal_mappings_;      // key -> lua ref
    std::unordered_map<std::string, int> visual_mappings_;      // key -> lua ref
    std::unordered_map<std::string, int> mod1_mappings_;        // key -> lua ref
};

#endif  // MEP_EDITOR_H
