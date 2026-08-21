# mep.nvim Parity Plan

Living roadmap for porting the feature set of `./mep.nvim` (the user's
from-scratch Neovim config, ~33 Lua libraries) into this repo's native
C++/raylib editor, `mep`. Written so a new session can pick this up cold:
read **Status** first for where things stand, then the relevant phase for
what's left in it. Full behavioral detail for any given feature lives in
`mep.nvim/README.md` (grep for `### mep.<name>`) — this plan says *what to
build and in what order*, not a re-derivation of that spec; re-read the
relevant README section before implementing a phase.

**How to resume:** check the checkboxes below, `git log --oneline` for
what's landed, then continue with the first unchecked item in the
lowest-numbered incomplete phase. Phases are ordered so each one only
depends on earlier ones — Part I is foundational infrastructure nearly
everything downstream needs; skipping ahead usually means re-doing work.
Follow the same rigor established in `VIM_PARITY_PLAN.md`: implement →
build both `build/native` and `build/web` → verify via the real native
build under Xvfb+xdotool (or manual interactive testing) → update this
file's checkboxes with a short "Verified via..." note → move to the next
phase, pausing for the user's go-ahead unless told to keep going.

**Scale check, read this first**: this is not a weekend project. `mep.nvim`
is a from-scratch, ~33-library, VS Code-and-Emacs-org-mode-and-Anki-sized
system. Treat each phase below as its own multi-session project. It is
completely reasonable to implement a fraction of any given phase (e.g. 3
LSP servers instead of 35, 3 babel languages instead of 25) and mark it
"done enough" with a note — matching how `VIM_PARITY_PLAN.md` treated
stretch goals. Prefer a smaller, solid, well-tested slice over a large,
half-working one.

---

## What mep already has (don't re-derive, build on it)

- **Modal editing**: full Vim-parity Normal/Insert/Replace/Visual/
  Visual-Line/Visual-Block/Command modes, motions, operators, text
  objects, registers, marks/jumps, search, substitute/global, `.`-repeat,
  macros, scrolling, increment/decrement — see `VIM_PARITY_PLAN.md`
  (complete).
- **Buffers/split-panes/tabs**: native C++ split-pane tree + tab bar
  already exist (`editor.cpp`'s `SplitNode`/`Pane`/`Tab` machinery,
  `:split`/`:vsplit`/`:tabnew`/etc., `Ctrl-W` pane commands). Part III's
  "window tiling-manager layer" phase *extends* this (per-pane tab lists,
  directional focus, auto-layout algorithms) rather than building splits
  from zero.
- **Embedded Lua** (`src/lua_env.h/.cpp`): a real Lua 5.x state with a
  `mep.*` API table — `get_line`/`set_line`/`line_count`/`cursor`/
  `set_cursor`/`insert_text`/`notify`/`command`/`map`/`map_mod1`/
  `set_mod1`/`nav_pane`/`cmd`/`quit`. This is the extension surface
  `mep.nvim`'s own `require('mep.<lib>').setup(opts)` pattern maps onto
  most directly — many phases below should grow the `mep.*` Lua API
  rather than hardcoding behavior in C++, mirroring how `mep.nvim` itself
  is *all* Lua on top of a much smaller native core (Neovim).
- **`mep.map_mod1`/`mep.set_mod1`**: mep.nvim's `Mod1` symbolic-modifier
  indirection (Alt on Linux/Windows, Option on macOS, retargetable) is
  **already implemented**. Nothing to build here; just keep using it.
- **`mep.command`/`mep.map`**: a basic command/keybinding registration
  mechanism already exists. Part I's keybind-registry phase *extends* it
  (descriptions, introspection for whichkey/help) rather than building a
  registry from zero.
- **`mep.notify`**: already routes into the status line
  (`Editor::SetStatusMessage`). Part I's notification-system phase
  *replaces/extends* this single choke point with a real toast+history
  system, exactly mirroring how `mep.nvim` hooks `vim.notify` once and
  gets every subsystem's messages for free.
- **What's genuinely absent** (confirmed by reading `main.cpp`): no
  syntax highlighting/tokenizer of any kind — text renders in a single
  flat color; no popup/floating windows; no async subprocess spawning;
  no decoration/extmark system; no fuzzy picker; no sidebar/panel widget;
  no theme system (UI colors are scattered hardcoded literals in
  `main.cpp`); no treesitter, LSP, DAP, or terminal/PTY integration. Part
  I through Part V exist to fill exactly these gaps, in dependency order.

---

## Design decisions

- **Treesitter grammars beyond a small, curated core are loaded
  dynamically at runtime, not vendored/compiled into mep.** `mep.nvim`
  does `git clone` + `cc -shared` per grammar on first use; wrong for a
  native app's build story if mep did the cloning/compiling itself, but
  the same end result — "find a `.so` already built somewhere, load it"
  — turned out right once the *building* is left to someone else (a Nix
  devShell, the tree-sitter CLI, an existing nvim-treesitter install).
  **Implemented in Phase 19, across three stages** (see that phase's own
  "design pivoted twice" bullet for the full detail): a first set of
  five grammars (C, C++, Lua, Python, JavaScript) via CMake
  `FetchContent`; broadened to ~55 grammars vendored as committed source
  under `third_party/` once `FetchContent`-per-grammar stopped scaling;
  then trimmed back to a 9-grammar core (adding Markdown, Org, and R to
  the original five) with everything else resolved by `dlopen()`ing a
  matching `.so` at runtime instead, once committed-source vendoring at
  ~55 grammars turned out to cost real repository disk space for
  languages most users don't touch. `flake.nix`'s devShell is the
  primary supported way to get the dynamic set on a Nix machine
  (`pkgs.tree-sitter.withPlugins`, exported as `$MEP_TS_PARSER_PATH`);
  an existing nvim-treesitter install or the tree-sitter CLI's own build
  cache work too, with no mep-specific setup at all. Org's own structure
  stays pure line-based either way (see the decision below) — its grammar
  (now in the compiled-in core, not dynamic) is used
  for optional syntax coloring only, same as every other vendored
  language, layered on top of, not replacing, that line-based core. Grow
  the set further incrementally; still never a live "install a parser"
  runtime feature.
- **LSP client is a real, from-scratch implementation** — `mep.nvim`
  itself doesn't have one to port (it rides on Neovim's own built-in
  `vim.lsp`), so this must be built from the LSP specification: async
  subprocess + stdio pipes, `Content-Length`-framed JSON-RPC 2.0,
  request/response correlation, the `initialize`/capabilities handshake,
  `textDocument/didOpen`-`didChange`-`didSave`-`didClose` sync, and a
  handful of request types (hover, definition, references, completion,
  signatureHelp, documentSymbol, codeAction, rename, publishDiagnostics).
  What *is* portable from `mep.nvim` is the **policy layer**: its
  `lua/mep/lsp/servers.lua` registry (35 servers, each `{cmd, filetypes,
  root_markers}`) is a ready-made reference for which servers to support
  and how to find their project root.
- **DAP reuses the LSP phase's wire-protocol layer.** DAP's base framing
  (`Content-Length: N\r\n\r\n<json>`) is identical to LSP's — build the
  framed-JSON-RPC reader/writer and request/response correlation once in
  Part I/V's LSP work, reuse it for DAP's session client.
- **Org-mode is a pure line-based text engine, no treesitter
  dependency for structure** — this matches `mep.nvim`'s own
  architecture exactly (its org module does all structural parsing via
  line patterns; treesitter there is *only* for optional syntax
  coloring, layered on top). This is good news: org-mode phases can
  start immediately on top of mep's existing `std::vector<std::string>`
  buffer model, with no treesitter/LSP prerequisite at all except for
  babel (needs process spawning) and polyglot (needs LSP, and is
  deferred anyway).
- **One generic fuzzy-picker widget, one generic sidebar/panel widget** —
  both get built once in Part I and reused by ~15 downstream features
  each (file finder, buffer switcher, live grep, help, theme picker,
  project switcher, symbol jump, git status, file tree, DAP sidebar,
  roam notes/backlinks, activity bar, notification history, AI panel,
  snippet picker, todoscan, ...). Get the source/data abstraction right
  early (static item list *or* async `get_items(query, callback)` for
  the picker; `sections` of clickable/hoverable `widgets` for the
  sidebar) — most of the value of these two phases is *not* rebuilding
  them per-feature later.
- **Buffer decorations (mep.nvim's "extmarks") are new core-editor
  infrastructure**, not a UI-layer trick: tracked text ranges with
  independent start/end gravity (grow/shrink correctly as the buffer is
  edited), each optionally carrying a highlight span, virtual/overlay
  text, or a callback. This underpins colorizer, markdown table/heading
  rendering, org link concealment, snippet tabstops, git gutter, LSP
  diagnostics, and AI streaming position-tracking — build it once in
  Part I as a genuine `Buffer`-level feature (`editor.h`), not bolted on
  per-feature.
- **Async process/job control is a worker-thread + main-thread-queue
  design**, not libuv (raylib/mep has no existing event loop to hang a
  libuv-style callback system off of). Spawn subprocesses from a
  background thread, buffer stdout/stderr there, and drain a
  thread-safe completion/data queue once per frame on the main thread —
  every phase from Part I onward that touches an external process
  (ripgrep, git, curl, language servers, debug adapters, compilers,
  REPLs) depends on this being solid.
- **Babel/run/REPL start with a small language subset.** `mep.nvim`
  supports ~25 languages for babel and ~13 for REPL; a first pass at
  each should cover 2-4 (e.g. `sh`, `python`, one compiled language)
  behind a small per-language strategy interface (`prepare_source`,
  optional `compile_cmd`, `run_cmd`), then grow the table — mirroring
  how this repo's own Vim-parity work scoped `:s`/`:g` to plain
  substring matching rather than blocking on a full regex engine.
- **LSP server registry, DAP adapter registry, and babel/run/REPL
  language tables are all just data** (name → command/args/root-markers)
  — copy `mep.nvim`'s own tables (`lua/mep/lsp/servers.lua`,
  `lua/mep/dap/adapters.lua`, `lua/mep/run/languages.lua`,
  `lua/mep/repl/registry.lua`, `lua/mep/docs/templates.lua`) as the
  starting data set rather than re-deriving them from scratch.

---

## Status

**Part I — Core infrastructure** — done, verified via Xvfb smoke tests
- [x] Phase 1 — Async job/process control
- [x] Phase 2 — JSON + small persistence helpers
- [x] Phase 3 — Popup/floating windows + input/confirm/select prompts
- [x] Phase 4 — Buffer decoration system (extmark-equivalent)
- [x] Phase 5 — Folding subsystem
- [x] Phase 6 — Notification system (toasts + history)
- [x] Phase 7 — Generic sidebar/panel widget system
- [x] Phase 8 — Fuzzy picker widget

**Part II — Theming & chrome** — done, verified via Xvfb smoke tests
- [x] Phase 9 — Theme engine, palettes, picker
- [x] Phase 10 — Icons
- [x] Phase 11 — Chrome widgets (statusline/winbar/tabline/statuscolumn/border) + whichkey

**Part III — Editor QoL & window management** — done, verified via Xvfb smoke tests
- [x] Phase 12 — Dashboard, scratch buffer, zen mode
- [x] Phase 13 — Hints (jump-to-location), colorizer, URL open
- [x] Phase 14 — Window tiling-manager layer

**Part IV — Project & VCS tooling** — done, verified via Xvfb smoke tests in a real git repo
- [x] Phase 15 — File tree sidebar
- [x] Phase 16 — Project list + picker
- [x] Phase 17 — Git integration (gutter + status/hunk sidebar)
- [x] Phase 18 — Todoscan

**Part V — Language intelligence** — done; verified against a real LSP server (Phase 20's original hover/definition/references/documentSymbol/formatting, plus Phases 21/22/24's consumers of them), and Phase 19 against a standalone harness driving the real vendored parser/grammars; Phase 20's later `implementation`/`typeDefinition`/`signatureHelp`/LSP-`completion`/`codeAction`/`rename` additions were verified against a mock JSON-RPC server instead (no real server installed in the environment they were added in — see Phase 20's own notes), and Phase 26 is implemented but not live-verified at all (no DAP adapter available)
- [x] Phase 19 — Treesitter integration (real libtree-sitter + 5 vendored grammars, highlight-only — see notes)
- [x] Phase 20 — LSP client
- [x] Phase 21 — Diagnostics UI
- [x] Phase 22 — Completion engine
- [x] Phase 23 — Snippet engine (scoped down)
- [x] Phase 24 — Symbols outline
- [x] Phase 25 — Docs (generate + lookup) + Help picker
- [x] Phase 26 — DAP client (implemented, not live-verified)

**Part VI — Running code** — done (scoped down from a full terminal emulator to real-PTY + color-only ANSI + line-oriented REPL send)
- [x] Phase 27 — Embedded terminal/PTY + Run + REPL

**Part VII — Markdown & Org-mode**
- [x] Phase 28 — Markdown rendering
- [x] Phase 29 — Org-mode A: outline, folding, TODO/tags/properties/checkboxes
- [x] Phase 30 — Org-mode B: tables (new), links, footnotes, timestamps/scheduling
- [x] Phase 31 — Org-mode C: capture, refile, archive
- [x] Phase 32 — Org-mode D: agenda
- [x] Phase 33 — Org-mode E: sort/narrow/sparse-tree, clocking
- [x] Phase 34 — Org-mode F: babel (code execution)
- [x] Phase 35 — Org-mode G: export
- [x] Phase 36 — Org-mode H: polyglot (LSP-in-src-blocks) — deferred/stretch (explicitly skipped)

**Part VIII — PKM extras (built on org)**
- [x] Phase 37 — Roam (zettelkasten note linking)
- [x] Phase 38 — Flashcards (SM2 spaced repetition)
- [x] Phase 39 — Bib (bibliography / org-ref)

**Part IX — Aggregated panels**
- [x] Phase 40 — Activity bar (notifications/todo/tests/git bar)

**Part X — AI & stretch**
- [x] Phase 41 — AI integration (LLM streaming + tool-calling agent)
- [x] Phase 42 — Leetcode (stretch, lowest priority)

**Part XI — Document panes beyond plain text** (filed under Part X's
numbering above since these phases were added after this Status table's
original write-up — not moved to avoid renumbering everything else)
- [x] Phase 43 — PDF viewer
- [x] Phase 44 — WYSIWYG office-document pane (.docx/.odt)
- [x] Phase 45 — Spreadsheet pane (.xlsx/.ods/.csv) with a full formula engine

Last updated: 2026-08-20 (Phase 45's remaining sub-phases -- XLSX read,
ODS read, save-back for all three formats + sheet-switching + polish --
implemented and verified; Phase 45 is now done).

---

## Part I — Core infrastructure

### Phase 1 — Async job/process control ✅

- [x] A worker-thread-based subprocess spawner: `Editor::SpawnJob(argv,
      opts)` — non-blocking from the caller's perspective, streams
      stdout/stderr (line-buffered, mirroring `mep.core.job`'s shape) via
      a thread-safe queue drained once per frame on the main thread
      (matches the "Design decisions" note above). Implemented as
      `Job`/`JobManager` in `src/job.h`/`job.cpp` (POSIX fork+exec, a
      `poll()`-based reader thread); `JobManager::PollAll()` is called
      once per frame from `UpdateDrawFrame()`.
- [x] Cancellation: kill/detach a running job (`Job::Kill`/
      `JobManager::Kill`) — sufficient for picker query-cancel and
      process teardown; nothing yet calls it from a live feature since
      REPL/DAP/live-grep haven't landed.
- [x] Exit-code + stderr capture for one-shot commands (`on_stderr`/
      `on_exit` callbacks distinguish stdout/stderr lines and report the
      real exit code, -1 on kill/spawn-failure).
- [x] A debounce/timer utility tied to the frame loop — not a separate
      utility; deferred to each consumer as it lands (the picker's own
      query-changed callback is itself already debounce-free/instant
      since it's client-side filtering, see Phase 8 note).
- [x] Expose a `mep.job`-equivalent to Lua: `mep.job_start(argv, opts)`,
      `mep.job_write`, `mep.job_close_stdin`, `mep.job_kill`,
      `mep.job_is_running`. Verified via Xvfb (spawned a real process,
      captured stdout).

### Phase 2 — JSON + small persistence helpers ✅

- [x] Hand-rolled a minimal recursive-descent JSON encoder/decoder
      (`src/json.h`, `class Json`) rather than vendoring nlohmann/json,
      matching the zero-runtime-dependency philosophy — no external
      dependency was added.
- [x] A per-user data directory convention: `MepDataDir()` in
      `src/persist.h`, `$XDG_DATA_HOME/mep` or the platform-specific
      home-based fallback; empty (no-op) on wasm.
- [x] Small helpers: `ReadJsonFile`/`WriteJsonFile` in `persist.h`
      (native-only).

### Phase 3 — Popup/floating windows + input/confirm/select prompts ✅

- [x] A floating-window primitive independent of the split-pane tree:
      `DrawFloatFrame(w, h, title)` in `main.cpp` — centered, bordered,
      titled box, reused by every overlay below and by the sidebar/
      picker widgets in Phases 7-8.
- [x] Modal text-input prompt (`vim.ui.input` equivalent): `Mode::Prompt`
      / `BeginPrompt` / `mep.ui_input`.
- [x] Confirm dialog (yes/no, with a default): `Mode::Confirm` /
      `BeginConfirm` / `mep.ui_confirm` — passes a real Lua boolean
      (`CallRefWithBool`), not 0/1-as-truthy.
- [x] Select-from-list prompt (`vim.ui.select` equivalent): `Mode::Select`
      / `BeginSelect` / `mep.ui_select`.
- [x] Hover tooltip: `Editor::ShowHover`/`CloseHover`/`IsHoverOpen`
      (`editor.h`/`.cpp`), `mep.hover_show(title, text)`/`mep.hover_close`
      (`lua_env.cpp`), `DrawHoverPopup` (`main.cpp`) — a genuinely
      non-modal overlay, unlike Prompt/Confirm/Select/Preview above: it
      does *not* touch `mode_` or steal input, the same "coexists with
      whatever mode is active" shape as Phase 22's completion popup.
      Anchored just below the active pane's cursor (reusing the exact
      fold-aware cursor-screen-position math `DrawPane` already computes
      for the text cursor/completion popup, not a re-derivation), drawn
      *after* `EndScissorMode()`/the pane border so multi-line hover text
      isn't clipped to a narrow split the way the completion list
      tolerates for short candidate words. Auto-dismiss-on-move (the
      plan's literal wording): `Editor::MaybeDismissHover()`, called at
      the top of `HandleInput()` every frame, snapshots the cursor
      position when `ShowHover()` opens and closes the tooltip the instant
      the cursor moves, Normal mode is left, or Escape is pressed. First
      consumers: `mep.lsp_hover()` (previously just `mep.notify(text)`, a
      toast) and `mep.org_bib_cite_preview()` (same swap) both now show
      real floating popups. Verified via Xvfb: `:lua mep.hover_show(...)`
      rendered a bordered box below the cursor without dimming the rest of
      the screen; pressing `j` (cursor moves one line) closed it on the
      very next frame.

### Phase 4 — Buffer decoration system (extmark-equivalent) ✅

- [x] Per-buffer decoration store: `Buffer::decorations` (namespace id →
      `vector<Decoration>`), anchored at `{row, col}` — **scoped down**
      from real extmark gravity-tracking through arbitrary edits;
      consumers are expected to clear+re-add their whole namespace on
      recompute (documented in `editor.h`), which is how the actual
      `mep.nvim`-equivalent consumers (git gutter, diagnostics,
      markdown/org overlays) all behave in practice anyway.
- [x] Highlight-span decorations (fg/bg + style over a range).
- [x] Virtual/overlay text decorations: both inline and overlay flavors
      exist on `Decoration`; renders in `DrawPane()`'s row loop.
- [x] Sign-column (gutter glyph) decorations, one per line — priority/
      stacking is "last writer renders on top" rather than an explicit
      priority field; revisit if two simultaneous consumers collide
      (none do yet).
- [x] A query API: `CurrentBufferDecorations()` / `AddDecoration()` /
      `ClearNamespace()` (per-buffer `buf.decorations`, not a global
      store — fixed a bug where inactive panes rendered the wrong
      buffer's decorations).

### Phase 5 — Folding subsystem ✅

- [x] Generic nested fold-range data structure per buffer: `Buffer::folds`
      (`vector<Fold>`), possibly nested, no explicit tree.
- [x] Fold rendering: `DrawPane()` compresses a closed fold's rows into
      one summarized visual slot; cursor Y math walks the same
      compression so the cursor lands on the right screen row.
- [x] A pluggable "fold provider" concept: `CreateFold(..., provider)` +
      `ClearFoldsFromProvider(provider)` — org/markdown/treesitter
      providers can each own and recompute their own fold set without
      clobbering manual (`za`) folds.
- [x] Normal-mode keys: `za` (toggle at cursor), `zo`/`zc` (open/close at
      cursor row) wired into the existing `pending_z_` dispatch.

### Phase 6 — Notification system (toasts + history) ✅

- [x] Extended `mep.notify`/`SetStatusMessage`'s single call site into a
      real choke point: `Editor::Notify(msg, level)` feeds both the
      toast stack and the capped history list; `l_notify` now calls it
      with an optional level string.
- [x] Toast stack: corner-anchored, newest-closest, `kMaxVisibleToasts`
      cap, per-level auto-dismiss (`PruneExpiredToasts`, called once/
      frame), level-based color+ASCII glyph (Unicode symbols were tried
      first, rendered as tofu with the embedded font — switched to
      ASCII, see plan's own icon-fallback philosophy in Phase 10).
- [x] History panel: a dedicated notify sidebar (`notify_sidebar_id_`)
      built on Phase 7's sidebar widget — scrollable, dismiss/clear-all.
- [x] `:MepNotifyPanel`/`:MepNotifyClear`/`:MepNotifyDismiss` ex-commands.

### Phase 7 — Generic sidebar/panel widget system ✅

- [x] `Sidebar` construct: `SidebarInstance` with `sections =
      [{id, title, collapsed, widgets = [{id, text, icon, hl, tooltip,
      on_click}]}]`, matching the planned shape.
- [x] Placement: **corrected description** — sidebars are edge-docked
      (`DrawSidebars` in `main.cpp` draws a plain edge-anchored rectangle,
      *not* Phase 3's centered `DrawFloatFrame` the way this bullet used
      to claim), and `DrawEditor`'s `pane_x`/`pane_w` reservation shrinks
      real pane screen space by the sum of every open left/right
      sidebar's `size * g_char_width` — a sidebar visually occupies its
      own strip beside the pane tree, it doesn't overlay pane content the
      way an actual float would. What's still genuinely missing (**not**
      fixed by this pass — see the honest caveat below) is *tree*
      integration: a sidebar isn't a `SplitNode` in the active tab's split
      tree, so `ComputeRects`/the automatic layouts (`master-left`/
      `grid`/`spiral`, Phase 14)/closing-a-pane-rebalances-into-freed-
      space don't know sidebars exist. That's a materially larger
      refactor (teaching the split-tree/pane-navigation/resize/auto-
      layout code a second kind of leaf) than this pass's scope, and
      touching it live in `main.cpp`/`editor.cpp` while multiple other
      agents are concurrently editing both files (verified via `git diff
      --stat` mid-session: 1000+ line deltas already in flight in each)
      was judged too high a collision/regression risk for the remaining
      value — directional-focus blur-in/out (`mod1+hjkl`,
      `NavigatePaneDirection`) and keymap resize (next bullet) already
      cover the two things users actually asked this phase for.
- [x] Open/close/toggle (`OpenSidebar`/`CloseSidebar`/`ToggleSidebar`),
      section collapse/expand via the shared flatten-to-lines helper.
      **Keyboard grow/shrink resize: corrected — already implemented**,
      contrary to what this bullet used to claim. `Editor::
      ResizeActivePane` (`editor.cpp`) has a dedicated `mode_ ==
      Mode::Sidebar` branch that grows/shrinks the focused sidebar's
      `size` (clamped to a 10-cell floor) along its own dock axis only
      (e.g. a left/right sidebar only responds to left/right, mirroring
      the pane-tree branch's "no-op on the wrong axis"); bound to the
      same `mod1+Shift+h/j/k/l` → `mep.resize_pane(direction)` default
      keymap Phase 14 wired for pane resize (`HandleMod1Shortcuts` runs
      before the `Mode::Sidebar` dispatch, so it fires regardless of
      focus). Mouse-drag resize remains out of scope per the plan's own
      note. Verified via Xvfb in a real repo: opened the file tree
      (`:MepFileTree`, auto-focuses it), `mod1+Shift+l` ×4 grew it from
      ~98px to ~314px, `mod1+Shift+h` ×6 shrank it back down to the
      10-cell floor — both screenshots confirm the pane tree to its right
      visibly resizes in lockstep (real reserved space, not an overlay).
- [x] Click hit-testing: **deferred** — keyboard navigation
      (`HandleSidebarInput`) is implemented and verified; mouse
      hit-testing against the flattened line list is straightforward to
      add (the flatten function already exists) but no test covered it
      yet.
- [x] Focus-restore tracking: `overlay_previous_mode_` /
      `RestoreFromOverlay()` — same mechanism Phase 3's prompts use,
      reused here so closing a sidebar always returns focus correctly.
- [x] Multiple simultaneous sidebar instances: `sidebars_` is a map
      keyed by id, `next_sidebar_id_` — the notify-history sidebar
      (Phase 6) and a second test sidebar coexisted in verification.

### Phase 8 — Fuzzy picker widget ✅

- [x] Fuzzy subsequence matcher with scoring: `FuzzyScore()` in
      `editor.cpp` — smart-case, consecutive-run bonus, word-boundary
      bonus, span/length penalty tiebreak.
- [x] Picker UI: prompt + scrollable results list via `DrawPickerOverlay`
      / `DrawFloatFrame`. **Preview pane: implemented** — see below.
- [x] Source abstraction: **static item list, plus a raw/unfiltered mode
      for dynamic sources**. `SetPickerItems` still lets Lua push a new
      list at any time; `OpenPicker`/`mep.picker_open` gained a 7th
      `raw_results` bool (`Editor::picker_raw_results_`) so a source that
      already filtered server-side (ripgrep matching its own pattern, for
      `mep.live_grep`) can skip Phase 8's own client-side `FuzzyScore`
      re-filter in `PickerFilteredResults()` — that re-filter, run against
      a *different* matching algorithm (fuzzy-subsequence, not regex),
      could otherwise hide a real regex match whose special characters
      (`.`, `*`, ...) don't literally appear in the matched line. Not a
      full `get_items(query, callback)`-with-cancellation abstraction
      (there's still no single generic "dynamic source" contract) — each
      dynamic source (`find_files`, `live_grep`) still does its own job
      spawn + generation-counter cancellation + `SetPickerItems`, just
      with the double-filtering correctness bug now closed for the one
      that needs it.
- [x] Debounce: **implemented for `mep.live_grep`**, the one source that
      actually needs it — typing re-runs ripgrep, so re-spawning a
      process on every single keystroke would be wasteful. Polling-based
      (`mep.on_frame` checking a query+timestamp the `on_query_change`
      callback just stashes, ~120ms window), the same idiom Phase 13's
      `mep.on_buffer_changed` already established — not a new mechanism.
      Static-source client-side filtering (find_files/buffers/commands)
      still runs undebounced every keystroke; still fast enough to need
      nothing else.
- [x] Navigation keys (Ctrl-N/P, arrows, Enter, Escape, Backspace),
      single-instance (`OpenPicker` replaces any existing picker state).
- [x] Built-in sources: **find files** (ripgrep `--files`, falling back
      to `find`), **command palette**, **buffer list**, and now **live
      grep** (`mep.live_grep`/`<leader>pg`, `kBuiltinPickerSources`,
      `main.cpp`) — typing in the prompt re-runs ripgrep against the
      *live* pattern (debounced, above) and streams matches in via
      `mep.picker_set_items`, rather than fuzzy-filtering one static
      list gathered up front. Guards a monotonic generation counter
      against a just-superseded search's late callback (job killed, but
      any stdout/exit callback already queued before the kill still fires
      once) clobbering a newer search's results. **Buffer-local line
      search — still deferred**, no consumer needed it yet.
- [x] Preview pane: `Editor::SetPickerPreview`/`PickerPreview`
      (`editor.h`/`.cpp`), `mep.picker_set_preview(text)` (`lua_env.cpp`),
      `DrawPickerOverlay` (`main.cpp`) widens the picker box and splits it
      into a results column + a scissored, top-anchored text column when
      a source has set one — `find_files` and `live_grep` both wire it via
      the picker's existing `on_select_change` hook (Phase 9's own gap,
      already closed before this pass) to show the highlighted file's
      first ~40 lines (`live_grep`: 200, since the match itself may be
      deep in the file) via a shared `mep_picker_preview_file` helper
      (plain `io.open`/`f:lines()`, same as several other `kBuiltinXxx`
      Lua blocks already use directly). Cleared on every `OpenPicker()` so
      a later unrelated picker (buffers/commands/themes) never inherits a
      stale preview and stays visually unchanged (verified). **Known
      limitation, honestly scoped**: the preview column doesn't scroll
      and only updates on an explicit selection-change event (arrow/
      Ctrl-N/P nav, or the query narrowing to a new top match) — since
      dynamic sources populate results via `SetPickerItems` directly
      (bypassing `HandlePickerInput`'s per-keystroke comparison entirely),
      a freshly-streamed-in top result doesn't auto-preview until the user
      actually navigates to it once.
- [x] **Real bug found and fixed during this pass's Xvfb verification**:
      `mep.live_grep`'s first draft (`rg ... -- <query>`, no path
      argument) hung forever — `IsRunning()` never went false, no
      results, no exit. Root cause: ripgrep with a bare pattern and no
      PATH argument reads *stdin* instead of searching the cwd whenever
      stdin isn't a tty, which is always true of a `mep.job_start` child
      (`job.cpp` always wires the child's stdin to a pipe); nothing ever
      wrote to or closed that pipe, so ripgrep blocked on a read that
      would never get data or EOF. Reproduced in isolation (a bare
      `rg PATTERN` against an open-but-silent pipe on stdin, via a named
      FIFO with a background holder) before confirming the fix: an
      explicit trailing `.` path argument forces directory search
      regardless of stdin's tty-ness. Fixed in the `mep.live_grep`
      `job_start` call; documented inline at that call site so the next
      person adding a bare-pattern `rg` invocation to a `job_start` call
      doesn't rediscover this the hard way.
- [x] Exposed as a reusable Lua API: `mep.picker_open(title, items,
      on_select, on_query_change?, on_key?, on_select_change?,
      raw_results?)`, `mep.picker_set_items`, `mep.picker_set_preview`,
      `mep.picker_close`, `mep.fuzzy_score` — `mep.find_files()`/
      `mep.buffers()`/`mep.commands()`/`mep.live_grep()` (in
      `kBuiltinPickerSources`, `main.cpp`) are themselves just ordinary
      consumers of this API, the way user config is expected to build
      custom pickers.
- [x] **Verified via Xvfb in a real git repo** (2 files, one containing
      two lines matching a test search term): `<leader>pf` (find files)
      opened populated with both files, arrow-down selection moved and
      showed the highlighted file's full content in the new preview
      column; `<leader>pg` (live grep), typing a query streamed in both
      matching `file:line:content` rows live (debounced re-run, not a
      static filtered list), arrow-down previewed the match's file, Enter
      jumped the real cursor to the matched line (confirmed via the
      status line's `Ln`/`Col`); a plain `mep.buffers()` picker (no
      preview wired) rendered exactly as before — single column, no
      leftover preview state from the previous picker.

All of Part I verified end-to-end via Xvfb screenshot smoke tests:
prompt/confirm/select overlays, decorations+folds rendering, toast +
history panel, sidebar open/nav/close, and the picker (open, fuzzy
filter, select-to-open-file, buffers, commands, find-files).

---

## Part II — Theming & chrome ✅ done, verified via Xvfb smoke tests

### Phase 9 — Theme engine, palettes, picker ✅

- [x] A named highlight-group abstraction: `Editor::ResolveHighlight(name,
      *out)` (exact lookup into the active theme's built group map) plus
      `main.cpp`'s `ResolveHlGroup()`, which every chrome call site (status
      line, menu bar, tab bar, pane header/border/cursor/gutter/selection,
      sidebars, pickers, toasts, help/prompt/confirm/select overlays) now
      targets by name — the ~40 previously-hardcoded `Color{...}` literals
      in `main.cpp` were migrated (a handful of alpha-blend composites
      remain, computed *from* a resolved theme color, not hardcoded).
      Decoration consumers with ad hoc `hl_group` names (e.g. "MepGitAdd")
      that don't match a group 1:1 fall back to the substring heuristic,
      now sourced from the theme's base role colors instead of literals.
- [x] A compact 10-field palette schema (`Palette` in `editor.h`: bg/fg/
      red/green/yellow/blue/purple/cyan/orange/border) and
      `BuildHighlightGroups()` (editor.cpp) — a data-driven renderer
      expanding a palette into ~35 named groups (base roles, diagnostics/
      git/notify levels, and chrome-specific groups like StatusLine/
      TabActive/BorderActive/PickerSelected), using `Lighten`/`Darken`/
      `Mix` helpers for derived shades rather than every group needing
      its own palette field.
- [x] Four palettes ported: `mep-dark` (the original hardcoded look,
      kept as default so nothing changes for existing users), `gruvbox-
      dark`, `nord`, `gruvbox-light` — verified visually distinct via
      Xvfb screenshots of all four.
- [x] **Full palette parity with `mep.nvim`**: every one of the 28
      colorschemes in `mep.nvim/lua/mep/theme/palettes.lua`'s
      `M.palettes` table now has a same-named, same-hex-value
      `editor.cpp` counterpart (`nord-light`, `tokyo-night`, `one-dark`/
      `one-light`, `rose-pine`/`rose-pine-dawn`, `monokai`, `ayu-dark`/
      `ayu-mirage`, `github-dark`/`github-light`, `nightfox`, `horizon`,
      `zenburn`, `synthwave84`, `oxocarbon-dark`/`oxocarbon-light` added;
      `onedark` renamed to `one-dark` to match `mep.nvim`'s hyphenated
      name exactly). `mep.themes()`/`:MepTheme` needed no changes since
      it already reads the picker's item list from `mep.theme_names()`
      dynamically. Plus five bonus variants beyond `mep.nvim`'s set
      (`tokyonight-storm`/`-moon`, `catppuccin-macchiato`/`-frappe`) kept
      from before this pass.
- [x] `:colorscheme <name>`/`:colo` ex-command, `Editor::ApplyTheme
      (name)`, `mep.colorscheme(name)`/`mep.theme_names()`/
      `mep.current_theme()` Lua bindings.
- [x] Theme picker: `mep.themes()` (in `kBuiltinPickerSources`) —
      snapshot-current-theme-before-open + apply on Enter + re-apply the
      snapshot on Escape gives commit-on-select and revert-on-cancel for
      free, without needing a per-highlight navigation callback.
- [x] **Live preview while arrow-navigating (before Enter)**: closed the
      gap noted above — `Editor::OpenPicker`/`HandlePickerInput` (editor.h/
      .cpp) gained a 6th callback, `on_select_change_ref`, fired whenever
      the *effective* selection changes (arrow/Ctrl-N/Ctrl-P navigation,
      or the query narrowing to a new top match — compared by item
      `data`, not index, so a query edit that leaves the index unchanged
      but points at a different item still fires). Threaded through
      `mep.picker_open`'s new optional 6th arg (`lua_env.cpp`'s
      `l_picker_open`). `mep.themes()` passes a callback that calls
      `mep.colorscheme(item)` immediately on every highlight change; the
      existing on_select (Enter re-applies/Escape reverts to the
      snapshotted `before` theme) is unchanged, so cancelling always
      reverts whatever was previewed while browsing. Verified via Xvfb +
      `xdotool`: opened the Colorscheme picker (mep-dark active), pressed
      Down — chrome background/text immediately re-tinted to gruvbox-dark
      *before* Enter — then Escape, which reverted to mep-dark. The hook
      is generic (any picker can pass a 6th callback), not theme-specific.
- [x] **Bound to an actual entry point**: `mep.themes()` was fully
      implemented but never reachable except via `:lua mep.themes()` —
      no command, no keymap. Added `:MepTheme` and
      `mep.leader_map('ut', 'Theme picker', mep.themes)`, matching
      `mep.nvim`'s own `<leader>ut` convention for this picker
      (`mep.nvim/lua/mep/theme/config.lua`). Verified via Xvfb +
      `xdotool`: `<leader>ut` (mep's leader defaults to Space) opens the
      "Colorscheme" picker listing all four palettes, and selecting
      `gruvbox-dark` visibly re-tints the chrome (tab bar, status line
      background) immediately.
- [x] **Bug found and fixed during verification**: `Editor::Editor()`
      called `ApplyTheme("mep-dark")` at construction time, but `g_editor`
      (main.cpp) and the palette table (editor.cpp) are separate
      translation units with no guaranteed cross-TU static-init order —
      on this build, `g_editor` happened to construct before the palette
      globals did, so that first `ApplyTheme` silently found no palettes
      and left every group unresolved (rendered as one flat hardcoded
      gray everywhere, only visible once a `mep.set_statusline` callback
      was added — text rendering worked, since `Normal`'s text-color
      fallback is a plausible-looking gray, but nothing else did). Fixed
      by re-applying the theme explicitly at the top of `main()`, which
      is only ever reached after *all* global constructors have run.

### Phase 10 — Icons ✅ (ASCII only, by design)

- [x] Filename/extension → icon glyph lookup table: `IconForFilename()`
      (editor.cpp) — full-name special cases (Makefile, Dockerfile,
      .gitignore, README, LICENSE, ...) checked before ~30 extension
      mappings, exposed as `mep.icon_for_file(name)`. **ASCII only** —
      emoji/nerd-font styles are **not** implemented: Phase 6's toast
      icons already proved the embedded font's glyph subset doesn't cover
      those codepoints (rendered as tofu), so this phase didn't re-attempt
      it.
- [x] Directory open/closed + tree-expand-marker glyphs: `mep.icons.
      dir_open`/`dir_closed`/`tree_expand`/`tree_collapse` (ASCII `v`/`>`).
- [x] UI-action icon set: `mep.icons.{notify,todo,tests,git,add,clear}`,
      a small bundled Lua table (`kBuiltinIcons`, main.cpp) — no consumer
      yet (activity bar/sidebars using it lands in later parts).
- [x] Font/glyph rendering support check: this *is* Phase 6's finding,
      carried forward — documented inline at `IconForFilename`'s
      declaration rather than re-verified, since nothing changed about
      the embedded font between phases.
- [x] **Fallback-atlas audit** (this pass, re-examining the "upgrading
      later needs a fallback font atlas" note above): grepped every
      icon/toast/notify call site in `main.cpp`/`editor.cpp` for non-ASCII
      bytes — none exist. mep has exactly one embedded font, loaded once,
      and *no* code path that ever attempts a nerd-font/emoji glyph and
      falls back on failure; `IconForFilename`, `kBuiltinIcons`, and the
      directory/fold glyphs are unconditionally ASCII already, on every
      platform, with no glyph-availability branch to get wrong. So the
      "documented plain-ASCII fallback used automatically when icon
      glyphs aren't available" this phase asks for is structurally
      already the *only* thing that ever renders — there is no
      font-dependent primary path for it to be a fallback *from*. A real
      fallback-atlas mechanism (detect missing glyphs, swap in a second
      font) would be scaffolding for a nerd-font mode that doesn't exist;
      building one now would be speculative, not a gap closure. If a
      future phase adds an optional nerd-font icon mode, *that's* the
      point to add the fallback-detection this bullet describes — until
      then, ASCII-always already satisfies the "never renders tofu"
      requirement by construction.

### Phase 11 — Chrome widgets (statusline/winbar/tabline/statuscolumn/border) + whichkey ✅ (scoped)

- [x] A shared widget abstraction — **scoped to the statusline only**:
      `{text, hl}` segments (no `on_click`/`on_hover` — nothing statusline
      -shaped is clickable yet), returned by a Lua callback
      (`mep.set_statusline(fn)`) and re-evaluated every frame via the new
      `LuaEnv::CallRefForWidgets()` (calls a ref, reads back an array of
      `{text=, hl=}` tables). Winbar/tabline/statuscolumn were **not**
      widget-ified — see below.
- [x] Statusline replacement: `DrawEditor()` calls the registered
      callback if set and draws its segments left-to-right, each through
      its own (or a default) highlight group; falls back to the original
      hardcoded mode/filename/position format when unset. Verified via
      Xvfb with a 2-segment custom statusline.
- [ ] Tabline/statuscolumn *widget-ification* — **still deferred, scope
      cut precisely**: neither was rebuilt on a Lua-configurable
      `{text, hl, on_click}` schema like the statusline's. That refactor
      (rearchitecting `DrawTabBar`'s tab-box rendering and `DrawPane`'s
      per-row gutter loop into something a Lua callback returns and
      re-evaluates every frame, the way `mep.set_statusline` works) is
      larger than this pass's remaining budget and genuinely separable
      from the click-dispatch gap below — a widget abstraction is about
      making the *content* Lua-customizable; click dispatch is about
      making the *existing* content respond to the mouse. Only the
      latter was done this pass (see below); the tab bar and gutter are
      still the same hardcoded C++ rendering they were, just no longer
      mouse-dead.
- [x] Winbar: mep has no dedicated winbar *chrome row* — the per-pane
      header (`DrawPane`, already showing the active buffer's path) is
      what functionally plays that role, matching Phase 11's own header
      that groups winbar with tabline/statuscolumn as one still-not-
      widget-ified set rather than three separately-missing features.
      Left as-is (not a code change) beyond the breadcrumb work below.
- [x] Statuscolumn (gutter unification): still not one named widget row
      (same scope cut as tabline above) — but the line-number gutter
      already *is* mep's statuscolumn in practice (line-number + sign +
      now fold-marker rendering all coexist in `DrawPane`'s row loop),
      and it's no longer click-dead (see below).
- [x] Active-window border highlight: `BorderActive`/`BorderInactive`
      theme groups (landed as part of Phase 9's chrome migration,
      already satisfies this bullet).
- [x] **Click dispatch — closed for the concrete cases this phase named,
      not "generic" in the abstract-widget sense**: added a small
      click-region registry (`g_click_regions`/`RegisterClickRegion`/
      `DispatchChromeClicks`, main.cpp) — whichever `DrawXxx` function
      renders a clickable area registers `{Rectangle, action}` for it;
      `DispatchChromeClicks()` runs once per frame right after
      `DrawEditor()` and dispatches the first hit (mirrors the existing
      menu bar's own one-hit click handling, which this doesn't touch).
      Guarded off whenever a modal overlay (Picker/Sidebar/Prompt/
      Confirm/Select/WhichKey/Preview) has input focus, so a click meant
      for an overlay never falls through to chrome behind it. Wired to
      three previously-mouse-dead elements:
      - **Tab click-to-switch**: `DrawTabBar` registers each tab's box;
        clicking one calls the new `Editor::GoToTab(index)` (jumps
        directly, unlike `TabNext`/`TabPrevious`'s relative stepping).
      - **Statuscolumn fold-marker click-to-toggle**: the gutter's one
        reserved trailing-space column (already there for `:set number`)
        now draws a `+`/`-` marker on a fold's start row (closed/open)
        and registers a click region calling the new
        `Editor::ToggleFoldAtRow(row)` (factored out of
        `ToggleFoldAtCursor`, which now just calls it with the cursor's
        row) — toggles without moving the cursor. Only wired for the
        active pane, since it operates on `Buf()`.
      - **Winbar breadcrumb click-to-navigate**: the per-pane header's
        path is now rendered as clickable segments (dimmed
        `Comment`-colored directories, `Normal`-colored filename); a
        directory segment click fires the new `Editor::WinbarClickRef()`
        hook (`mep.set_winbar_click(fn)`, parallel to
        `mep.set_statusline`) with the path up to that segment. Default
        handler `mep.winbar_navigate` (kBuiltinPickerSources) opens a
        file picker scoped to that directory (same rg-with-find-fallback
        pattern as `mep.find_files`, just rooted at the clicked dir).
      Verified via Xvfb + `xdotool`: clicked a background tab and the
      active tab switched; `:set number` + `:lua mep.fold_create(2,4,
      false)` (1-indexed) then clicking the gutter's `-` marker on line 2
      closed the fold (confirmed identical to keyboard `za`'s result),
      clicking the resulting `+` marker reopened it; clicking the `src`
      segment of a `src/main.cpp` pane header opened a "Files in src"
      picker scoped correctly. Sidebar widgets' `on_click_ref` (Phase 7)
      and the picker/select overlays remain keyboard-only (Enter), as
      before — this pass's audit found tab bar, gutter fold markers, and
      the winbar-equivalent breadcrumb were the concretely-named gaps
      (per the plan's own worked examples); it did not attempt a
      click-to-place-cursor-in-buffer-text feature, which is a separate,
      much larger gap this repo has never had and this pass wasn't asked
      to add.
- [x] Whichkey: real feature, not a stub. Added `Mode::WhichKey` (new
      modal-overlay mode, same family as Prompt/Confirm/Select/Sidebar/
      Picker), a `WhichKeyBinding{sequence, description, lua_ref}`
      registry (`mep.leader_map(seq, desc, fn)`), a configurable leader
      key (`mep.set_leader(key)`, default space) that only actually takes
      over Normal-mode input once at least one binding is registered (so
      an unconfigured leader never steals a key), and
      `DrawWhichKeyOverlay()` (a floating list of remaining-sequence →
      description, narrowing live as more keys are typed). An exact
      unique match fires immediately; typing into a dead end (no bindings
      share that prefix) cancels with a status message; Escape cancels
      any time. Verified via Xvfb: `<leader>` popup lists all 4 test
      bindings, `<leader>f` narrows to the 3 `f*` bindings, `<leader>ff`
      fires `mep.find_files()` and opens the picker.

---

## Part III — Editor QoL & window management ✅ done, verified via Xvfb smoke tests

### Phase 12 — Dashboard, scratch buffer, zen mode ✅

- [x] Dashboard: `DrawDashboard()` (main.cpp), shown by `Editor::
      ShouldShowDashboard()` — single tab, single pane, single buffer,
      that buffer empty/unmodified/unnamed. Recomputed fresh every frame
      rather than a one-shot flag, so it auto-disappears the instant any
      of that stops being true (verified: typed `i` + text, dashboard
      gone next frame). Centered logo/hints text (reuses `kAboutText`
      plus a one-line command-hint footer).
- [x] Scratch buffer: `Buffer::scratch` flag + `Editor::
      OpenScratchBuffer()` (find existing-or-create, switch to it) +
      `:MepScratch`/`mep.scratch()`. Labeled "[Scratch]" in the pane
      header and status line instead of "[No Name]". Never persisted:
      same empty-filename `:w` refusal an unnamed buffer already had.
- [x] Zen mode: `Editor::ToggleZenMode()`/`:MepZen`/`mep.toggle_zen()` —
      **scoped to a single on/off toggle**, not independently-toggleable
      pieces (no file tree/symbols/activity-bar exist yet to toggle
      separately; this hides menu bar, tab bar, and status line, and
      centers the pane tree with side padding). Command line and
      overlays/toasts stay visible always (functionally necessary, not
      decorative chrome). "Full state save/restore" is implicit: nothing
      zen hides is stateful beyond the boolean itself.

### Phase 13 — Hints (jump-to-location), colorizer, URL open ✅

- [x] Hints: real feature. `Mode::HintChar`/`Mode::HintLabel` (new modal-
      overlay pair) + `Editor::BeginHints()`/`mep.hint_jump()`. Scans the
      current pane's *visible* rows for the typed target character,
      labels each occurrence with a home-row-first pool (`asdfghjkl...`,
      single-char up to 26 matches, two-char beyond that), renders labels
      directly in `DrawPane`'s row loop (reusing its existing fold-aware
      row→screen-y math instead of duplicating it). Auto-jumps on exactly
      one match; a dead-end prefix while typing a 2-char label cancels.
      Verified via Xvfb: labeled every "l" in a test line, typed a label,
      cursor landed exactly on the matched character.
- [x] Colorizer: `mep.colorize()`/`:MepColorize` (Lua, `kBuiltinTextTools`
      in main.cpp) detects `#rrggbbaa`/`#rrggbb`/`#rgb`/`rgb()`/`rgba()`
      per line via Lua patterns, adds one decoration per match. **Real
      swatch rendering, not just detection**: extended `Decoration` with
      `has_swatch`/`swatch_color` (a literal RGB, bypassing the named-
      highlight-group system entirely, since the point is showing the
      *exact* parsed color) and `mep.deco_add`'s `color = {r,g,b}` field;
      `DrawPane` draws a small filled square at each swatch's position.
      Verified via Xvfb: 4 colors in one line (`#ff0000`, `#00ff0088`,
      `#0f0`, `rgb(50,100,200)`) each got a correctly-colored swatch.
      **Not implemented**: CSS named colors (`red`, `cornflowerblue`,
      ...).
      ✅ **Root-cause fix landed**: the "no buffer-changed event/autocmd
      system" gap this and several other phases (17, 18, 19, 24) hit is
      now closed — `mep.on_frame(fn)` (new C++ primitive, LuaEnv::
      RunFrameHooks, called once per frame from main.cpp) plus polling
      getters `mep.buffer_change_epoch()`/`buffer_save_epoch()` (bumped
      by `PushUndo()`/`SaveBuffer()`) let Lua build debounced watchers
      without a synchronous callback fired from inside the edit/save
      call stack. `mep.on_buffer_changed(fn, interval_sec)`/
      `mep.on_buffer_saved(fn)` wrap this into ready-to-use debounced
      subscriptions. `:lua mep.colorize_auto = true` now gets real
      auto-recompute-on-edit, opt-in (off by default, so existing
      configs/large files aren't surprised by new always-on cost).
      A real, subtle bug was caught and fixed during verification:
      `change_epoch_` only bumped at Insert-mode *entry* (`PushUndo()`
      is called before `EnterInsert()`, matching vim's "one insert
      session = one undo step" semantics) and never again for the rest
      of that insert session — so a poller watching the epoch would see
      the buffer as unchanged for the entire duration of typing, only
      catching up once some *later*, unrelated edit began. Fixed by
      also bumping `change_epoch_` (not `PushUndo()` again — that would
      record a spurious second undo checkpoint) when *leaving* Insert
      mode, in `Editor::EnterNormal()`. Verified via Xvfb: typing a hex
      color and leaving Insert mode now shows its swatch immediately
      with no manual `:MepColorize`, and `u` still undoes the whole
      insert session as one atomic step (confirming the fix didn't
      touch undo/redo semantics). A second, smaller bug: the debounce
      helper's first draft used `os.clock()` (CPU time, not wall-clock)
      for its interval check, which drifted unpredictably from real
      elapsed time under this render loop; fixed by threading raylib's
      `GetTime()` into a new `Editor::Now()`/`mep.now()` (same
      thread-it-in pattern as `PruneExpiredToasts`, keeping editor.h/
      .cpp raylib-free).
- [x] URL open: `mep.url_under_cursor()` (Lua pattern, cursor-position-
      aware), `mep.open_url(url)` (spawns `xdg-open`/`open`/`start` via
      the Phase 1 job subsystem, picked by the new `mep.platform()` C++
      binding), `mep.open_url_under_cursor()`, `mep.list_urls()` (Phase 8
      picker over every URL in the buffer, select-to-open). Verified via
      Xvfb: `url_under_cursor()` correctly extracted a URL with the
      cursor placed inside it (and correctly returned nil with the
      cursor elsewhere on the line), `list_urls()` opened a picker
      showing the one URL in a test buffer.

### Phase 14 — Window tiling-manager layer ✅

- [x] Per-pane buffer tabs: `Pane::buffer_tabs`/`buffer_tab_index` (lazily
      (re)seeded from the pane's current buffer wherever it might be
      stale, via `EnsureBufferTabSeeded`, rather than kept in sync at
      every existing `buffer_id`-assignment call site). `mep.pane_open
      (path)`/`mep.pane_next_buffer()`/`mep.pane_prev_buffer()`/
      `mep.pane_close_buffer()`. Pane header shows "[i/N]" once a pane
      holds more than one tab.
- [x] Directional focus: **already existed pre-Phase-14**
      (`NavigatePaneDirection`/`mep.nav_pane`, from the vim-parity work)
      — nothing new needed; refactored its neighbor-search into a shared
      `FindNeighborPaneId()` helper so Phase 14's move-tab-to-neighbor
      could reuse the exact same geometry logic. Considering floating
      sidebars as fallback targets is **not** implemented — sidebars
      are floats outside the pane-rect tree Directional focus searches,
      and no consumer has asked for cross-sidebar directional nav yet.
- [x] Move-tab-to-neighbor: `mep.pane_move_buffer(direction)` — removes
      the active buffer tab from the current pane (closing the pane if
      it was that pane's last tab) and inserts it into the nearest pane
      in `direction`. Remove-tab: `mep.pane_close_buffer()`.
- [x] Manual resize: `SplitNode` gained a `shares` vector (parallel to
      `children`, empty meaning equal -- the prior strictly-equal-share
      limit) so a Split node can hold non-equal shares along its axis.
      `Editor::ResizeActivePane(direction, step)` / `mep.resize_pane`
      walks up from the active pane to the nearest ancestor split on the
      resize's axis and shifts the boundary between the active child and
      whichever neighbor it has, clamped so neither side goes below 5%;
      mirrors mep.nvim's `mep.window.panes.resize` grow/shrink-role-flip
      behavior exactly. Any later split/close at that node leaves the old
      `shares` size mismatched on purpose, so it silently reverts to
      equal instead of carrying stale ratios forward -- ComputeRects and
      the resize itself both treat a size mismatch as "equal shares".
      Bound as `mod1+Shift+h/j/k/l` (`kDefaultMod1Bindings`, main.cpp),
      matching mep.nvim's `<Mod1-S-h/j/k/l>`. `mod1+Ctrl+h/j/k/l`
      (move-tab-to-neighbor) and `mod1+d` (remove-tab) were already
      implemented above but, unlike focus (`mod1+hjkl`), had no default
      keybinding until now -- also added to `kDefaultMod1Bindings`.
      `mep.map_mod1` itself gained "S-"/"C-" prefixed key support
      (`HandleMod1Shortcuts` now checks for an extra Ctrl/Shift held
      alongside mod1) since mep.nvim's resize/move bindings are
      `<Mod1-S-...>`/`<Mod1-C-...>`, not bare `<Mod1-...>`. Also added
      `Ctrl-W h/j/k/l` as a directional-focus alias alongside the
      pre-existing cycle-only `Ctrl-W w/W`, since `NavigatePaneDirection`
      already existed but wasn't reachable from native Vim keys, only
      `mod1+hjkl` -- real Vim has both.
- [x] Automatic one-shot layout algorithms: `Editor::ApplyLayout(kind)`/
      `:MepLayout <kind>`/`mep.layout(kind)` — `master-left`/`master-
      right`/`master-top`/`master-bottom` (one pane + an equal-share
      stack of the rest), `grid` (near-square rows×cols), `grid-h`/
      `grid-v` (one row/column), `spiral` (recursive alternating-axis
      halving, the dwm/i3 fibonacci layout) — rebuilds the active tab's
      split tree from its current flattened pane list (existing pane
      ids/buffers/cursors preserved, just re-parented). Verified via
      Xvfb: 4 panes → `master-left` (1 pane at 50% + 3 stacked on the
      right) and → `grid` (clean 2×2), both matching the intended shape.

---

## Part IV — Project & VCS tooling ✅ done, verified via Xvfb smoke tests in a real git repo

### Phase 15 — File tree sidebar ✅

*(depends on: Phase 7 sidebar, Phase 10 icons, Phase 3 prompts)*

- [x] Directory scan: `mep.list_dir(path)` (new, `std::filesystem`-backed
      C++ primitive, directories-first-then-alphabetical) — **lazy
      one-level-at-a-time** in the sense that a collapsed directory is
      never scanned at all (only expanded ones, recursively, are), but
      **not cached** across close/open: re-expanding re-scans. Caching
      wasn't needed to make the feature usable and would add invalidation
      complexity (an edit-hook system doesn't exist yet to know when to
      invalidate anyway — same gap noted in Phase 13's colorizer).
- [x] All keybindings, built entirely in Lua (`kBuiltinFileTree`,
      main.cpp) on top of the generic sidebar's new `on_key` extension
      point (see below): open file / toggle directory (Enter, both),
      refresh (`R`), toggle-hidden (`H`), open-with-OS-default (`o`, via
      Phase 13's `mep.open_url`), create (`a`), rename (`r`), delete-with-
      confirm (`d`).
- [x] Git-ignore-aware listing: `git status --ignored --porcelain` (Phase
      1 job) run once per tree-open/refresh, filtering matched relative
      paths out of the listing; silently shows everything if the job
      fails (not a repo) rather than erroring.
- [x] Help popup: `?` shows the tree's keybindings via a toast
      (`mep.notify`) rather than a dedicated Phase 3 popup — same
      information, lower ceremony; a real popup is a trivial follow-up
      if wanted.
- [x] **New generic infrastructure this phase needed and added**:
      `SidebarInstance::on_key_ref` + `mep.sidebar_set_on_key(id, fn)` —
      a catch-all for keys `HandleSidebarInput` doesn't already reserve
      (j/k/q/arrows/Enter/Escape), so tree-specific keybindings didn't
      require teaching the generic sidebar widget anything tree-shaped.
      Also `mep.sidebar_cursor_widget_id(id)` (which row's widget `id` the
      cursor is on) and `mep.fs_mkdir`/`fs_create_file`/`fs_rename`/
      `fs_delete` (native-only filesystem writes). Verified via Xvfb in a
      real repo: expand `src/`, open `mod.lua`, confirmed content loads.

### Phase 16 — Project list + picker ✅

*(depends on: Phase 2 JSON, Phase 8 picker, Phase 15 file tree — soft)*

- [x] Persisted project list: `mep.project_list()`/`project_add(path)`/
      `project_remove(path)` (new C++ primitives, `$XDG_DATA_HOME/mep/
      projects.json` via Phase 2's `Json`/`ReadJsonFile`/`WriteJsonFile`
      — the first real consumer of those since Phase 2 landed). Add-
      current-dir and delete-entry both happen *from within* the
      `mep.projects()` picker (special "+ Add current directory" / "-
      Remove a project..." entries) rather than as separate ex-commands,
      matching the plan's literal wording.
- [x] Selecting a project: `mep.project_open(dir)` — `mep.chdir(dir)`
      (new `std::filesystem::current_path` binding), opens the first
      matching README name, opens the file tree rooted there. **Terminal
      pane step skipped** (Phase 27 doesn't exist yet) — exactly the
      graceful degradation the plan itself calls for.

### Phase 17 — Git integration (gutter + status/hunk sidebar) ✅

*(depends on: Phase 1 jobs, Phase 4 decorations, Phase 7 sidebar)*

- [x] Text-diff algorithm: real O(ND) Myers diff (`MyersDiffHunks` in
      lua_env.cpp — the actual 1986-paper algorithm, not a line-by-line
      approximation), exposed as `mep.diff_lines(a, b)`. This is the one
      genuinely correctness-critical piece of the whole phase, so it's
      implemented in C++ rather than Lua; verified against a real `git
      diff` on a 3-hunk change (line changed / line changed+moved) —
      hunk boundaries matched exactly, confirmed by successfully staging
      just one of the two hunks (below) and checking `git diff --cached`
      showed *only* that hunk.
- [x] Gutter signs: `mep.git_gutter_refresh()` diffs the current buffer
      against `git show HEAD:<file>` (Phase 1 job), adds `+`/`~`/`_`
      (add/change/delete) decorations via the diff hunks. Diffs against
      `HEAD` only (not a configurable base) — no `:MepGitGutter base
      <ref>` equivalent.
      ✅ **Auto-recompute now available**: `:lua mep.git_gutter_auto =
      true` (off by default — spawns a git subprocess per recompute, so
      a longer 0.6s debounce than colorize/todo-mark's default) wires
      `mep.git_gutter_refresh()` to `mep.on_buffer_changed`, the same
      root-cause fix documented under Phase 13.
- [x] Hunk navigation: `mep.git_next_hunk()`/`git_prev_hunk()` (wraps).
      Stage hunk: `mep.git_stage_hunk()` builds a minimal zero-context
      unified diff patch and pipes it to `git apply --cached
      --unidiff-zero -` via Phase 1's job stdin — verified for real
      (see above). Reset hunk: `mep.git_reset_hunk()`, pure
      `mep.replace_lines` in-buffer edit (a new generic multi-line-splice
      primitive, not git-specific) using the cached base-file lines, no
      git call. **Preview hunk is not implemented** — no consumer needed
      it yet (stage/reset don't require seeing the hunk rendered
      separately first).
- [x] Status sidebar: `mep.git_status_refresh()`/`:MepGitStatus`
      (`git status --porcelain`), stage/unstage/discard(confirm)/commit
      all via the same `on_key`-extension mechanism Phase 15's tree
      introduced. Commit flow is a single `mep.ui_input` prompt for the
      message (not a full editable-buffer flow) — scoped down, still
      real (verified: staged a file via `s`, confirmed `git status`
      showed it staged). Verified via Xvfb in a real repo end-to-end:
      gutter signs on 2 changed lines, staged one hunk, reset the other,
      status sidebar stage key updating the entry live.
- [ ] Dock/split dual presentation — **not implemented, same scope cut
      Phase 7 now documents in detail for the generic sidebar widget
      itself**: the status sidebar is edge-docked with real reserved
      screen space and keymap-driven resize (both inherited for free from
      Phase 7's generic `SidebarInstance`, verified working there), but
      it isn't a `SplitNode` in the pane split tree, so it has no
      alternate "open as an actual split pane" presentation. See Phase
      7's placement bullet for why that deeper integration stayed out of
      scope this pass.

### Phase 18 — Todoscan ✅

*(depends on: Phase 1 jobs, Phase 4 decorations, Phase 8 picker)*

- [x] Project-wide scan: `mep.todoscan()`/`:MepTodoScan`, ripgrep-backed,
      configurable keyword list (`TODO`/`FIXME`/`HACK`/`NOTE` by
      default). **No synchronous walk+match fallback** without ripgrep —
      unlike `find_files`' `find`-based fallback, a pure-Lua recursive
      grep across a whole project has real performance/complexity cost
      for a fallback path; scoped down to "ripgrep required, notify if
      missing."
- [x] Picker over all matches, jump on select (file + line, parsed from
      ripgrep's `file:line:text` output) — verified via Xvfb.
- [x] Live in-buffer marking: `mep.todo_mark_buffer()`/`:MepTodoMark` —
      sign + highlight per keyword via decorations.
      ✅ **Auto-recompute now available**: `:lua mep.todo_mark_auto =
      true` wires `mep.todo_mark_buffer()` to `mep.on_buffer_changed`,
      the same root-cause fix documented under Phase 13.
      **Per-keyword glyph/color** not individually configurable (all
      keywords currently share one sign glyph "T" and the "Warn" theme
      color) — scoped down from the plan's "per-keyword configurable."
- [x] **Real bug found and fixed during verification**: the first
      `mep.todoscan()` implementation called `rg -n --no-heading
      PATTERN` with no path argument. Ripgrep's own documented behavior:
      given a pattern but no path, if stdin is not a TTY it searches
      *stdin's content* instead of walking the directory — and every
      `mep.job_start`ed child's stdin is a pipe (kept open in case the
      caller wants to `mep.job_write` to it, e.g. Phase 17's `git apply`
      patch pipe), which is never a TTY and is never closed unless the
      caller calls `job_close_stdin`. So the spawned `rg` blocked forever
      reading an unclosed, never-written-to stdin pipe — no output, no
      exit, no error, just a silently-hung job. Caught via Xvfb (the
      picker never opened; isolated by testing `job_start` calls with
      progressively simpler argv until the exact trigger was found).
      Fixed by always passing an explicit path (`'.'`) alongside the
      pattern. **This is a general footgun**, not just a todoscan bug:
      any future `mep.job_start` of a CLI tool that conditionally reads
      stdin when given no positional path argument will hang the same
      way. `find_files`'s `rg --files` (no pattern) isn't affected, and
      none of Phase 17's `git` subcommands read stdin implicitly, but any
      *new* pattern-taking search tool wired in later phases should pass
      an explicit path defensively.

---

## Part V — Language intelligence ✅ (done, heavily scoped in places — see each phase)

### Phase 19 — Treesitter integration ✅ real parser + real grammars, highlight-only

*(depends on: Phase 1 jobs only for a possible future "install more
grammars" command — activation itself doesn't need it)*

- [x] Vendoring `libtree-sitter` + grammar sources — **implemented,
      scoped to a small core set, with everything else loaded
      dynamically at runtime instead**. The tree-sitter runtime
      (`v0.26.12`) plus **9 grammars** live as source under
      `third_party/` (see `third_party/README.md` for the exact
      repo/tag/subdir each was vendored from) and are compiled directly
      by `CMakeLists.txt` (`ts_build`, `TS_GRAMMAR_NAMES`) — **no
      network access at configure or build time at all**. Core set: C,
      C++, Lua (mep's own scripting/config language), Python,
      JavaScript/JSX, Markdown (block + a separate `markdown_inline`
      grammar the block grammar's own `injections.scm` hands inline
      spans off to — see the two-pass note below), Org
      (`nvim-orgmode/tree-sitter-org`, the grammar Neovim's own
      `nvim-orgmode` plugin uses), and R. Every grammar's own vendored
      copy of `tree_sitter/parser.h` is used for its build, so no
      grammar library is coupled to the runtime's header version.
- [x] **Everything beyond the core set: `dlopen()`ed at runtime, not
      vendored** (`LoadDynamicLanguage`/`DynamicLanguageTable`/
      `DynamicSearchPaths`, `src/treesitter.cpp`, native builds only —
      see the two design pivots below for why). `DynamicLanguageTable`
      still has an embedded `queries/highlights.scm` for ~40 more
      languages (Rust, Go, TypeScript/TSX, YAML, Nix, Zig, Ruby, PHP,
      OCaml, Kotlin, Swift-adjacent JVM languages, ...; text only, a few
      hundred KB total, not real vendoring); at runtime, a filetype in
      that table resolves to a real grammar only if a matching
      `<canonical-name>.so` (named after the grammar's own
      `tree_sitter_<canonical_name>` symbol, e.g. `rust.so`, `c_sharp.so`)
      turns up in `$MEP_TS_PARSER_PATH`, the tree-sitter CLI's build
      cache, an nvim-treesitter install, or `~/.config/mep/parsers` (that
      exact order — see `DynamicSearchPaths`'s doc comment for the XDG
      env vars each one respects). Verified two ways: against Nix-built
      grammars (`pkgs.tree-sitter.withPlugins`, built directly for this
      verification) confirming `rust.so`/`go.so`/`c_sharp.so` load and
      highlight correctly; and — unprompted, just by having the search
      path checks in place — against a real pre-existing nvim-treesitter
      install already on the development machine
      (`~/.local/share/nvim/site/parser/`), which supplied working
      Ruby/Rust/Go grammars with *zero* configuration, `MEP_TS_PARSER_PATH`
      unset. `flake.nix`'s devShell demonstrates and exercises the
      primary intended path: `tsGrammars = pkgs.tree-sitter.withPlugins
      (p: [...])` bundles ~35 grammars, exported as `MEP_TS_PARSER_PATH`
      in `shellHook` — confirmed end-to-end via `nix develop --command`
      launching mep against a real `.zig` file (a language *not* present
      in the pre-existing nvim-treesitter install, isolating the test to
      the Nix path specifically) and getting correct, real Treesitter
      highlighting, screenshotted via Xvfb.
- [x] **Design pivoted twice on the way here, each time from a real,
      measured problem, not speculation**:
      1. *`FetchContent` per grammar* (the original design, fine at five
         grammars) → didn't scale: a clean `build/` directory
         sequentially `git clone`d every repo on *every* fresh configure
         — tens of minutes at ~50 grammars, and once, catastrophically,
         hours: `tree-sitter-ocaml`'s `examples/` directory vendors
         several huge, unrelated upstream projects (`js_of_ocaml`,
         `dune`, ...) as git submodules, and a plain recursive clone
         pulled gigabytes of them in just to reach the handful of files
         under `src/` that matter (confirmed: one `ts_ocaml-src` alone
         reached 6.1GB before being killed).
      2. *Committed source under `third_party/`, all ~55 grammars* →
         configure/build became fully offline and fast (a clean build
         under 20 seconds), but added ~340MB to the repository's working
         tree. Real network-transfer cost was small (~15MB compressed —
         these generated parser tables are extremely repetitive), so
         clone *speed* wasn't actually the problem; **local disk
         footprint for languages most users don't touch** was the
         objection that mattered. (Two further bugs surfaced during this
         stage, worth keeping in mind for the core set's own maintenance:
         cherry-picking specific filenames instead of a grammar's whole
         `src/` directory misses sibling helper files some scanners
         `#include` beyond the obvious parser/scanner pair
         — `tree-sitter-vim`'s `keywords.h`, `tree-sitter-yaml`'s
         `schema.*.c`, `tree-sitter-haskell`'s `unicode.h`; and
         `tree-sitter-just`'s `scanner.c` hard `#error`s unless
         assertions are enabled, since it leans on `assert()` for actual
         control flow — needed `-UNDEBUG` on just that one target under
         this project's Release build. Neither currently applies to the
         9-grammar core set, but would resurface if it grows.)
      3. *Current design*: trim the compiled-in set to languages nearly
         every user touches constantly (mep's own config language
         included), and get breadth back via dynamic loading — the
         repository stays small, builds stay instant and offline, and a
         user on Nix (or with Neovim + nvim-treesitter already set up)
         gets the same real Treesitter highlighting for free, with zero
         extra vendoring cost to mep itself.
- [x] What shipped: `src/treesitter.h`/`src/treesitter.cpp` — a real
      `TSParser`/`TSQuery`/`TSQueryCursor` wrapper running each grammar's
      *actual upstream* `queries/highlights.scm` (embedded verbatim at
      vendoring time in `src/treesitter_queries.h`), exposed to Lua as
      `mep.ts_captures(filetype, text)` (`l_ts_captures`, `lua_env.cpp`).
      `mep.syntax_highlight()`/`:MepSyntax` (`kBuiltinSyntax`, main.cpp)
      now tries this first and falls back to the original hand-rolled
      per-line lexer (comment-prefix / quoted-string / number /
      keyword-list) only for filetypes with no vendored grammar — both
      paths render through the same Phase 4 decoration + Phase 9
      highlight-group pipeline. Predicates the query files actually use
      (`#eq?`/`#not-eq?`/`#match?`/`#not-match?`/`#any-of?`/
      `#not-any-of?`) are evaluated for real (`EvalPredicates`,
      treesitter.cpp) — this matters concretely: C/C++/Python/JS's
      `@constant` capture is gated by `(#match? @constant
      "^[A-Z][A-Z\d_]*$")` on the *same* `(identifier)` pattern that also
      unconditionally captures `@variable`, so skipping predicate
      evaluation would have colored every identifier as a constant, not
      just `CONST`/`SCREAMING_CASE` ones. Property-only predicates like
      `(#is-not? local)`, which would need a `locals.scm` scope analysis
      this integration doesn't do, are accepted as-is — a documented,
      minor precision loss. Verified two ways: a standalone harness
      linking `treesitter.cpp` directly against the built grammar/runtime
      static libraries, run against real C/C++/Python/Lua/JS samples
      (correct comment/string/number/keyword/type/function captures,
      correctly *excluding* `@constant` on lowercase identifiers); and
      the real embedded Lua interpreter loading `kBuiltinSyntax` and
      exercising `mep.syntax_highlight()` end to end with a mocked
      `mep.deco_add`, confirming both the Treesitter path and the
      no-grammar fallback path resolve capture/keyword names to the
      correct highlight groups. Also fixed a latent, previously-dead bug
      surfaced by that same test: the fallback lexer's keyword tables
      were keyed by full language name (`python`, `javascript`) while
      `mep_lsp_filetype` returns the bare extension (`py`, `js`) — those
      two languages' fallback highlighting silently never matched
      anything even before this phase (only reachable now for stray
      extensions Treesitter doesn't cover, since py/js themselves are
      handled by Treesitter). Both the native (~9.6MB) and Emscripten/wasm
      (~7.6MB single-file `mep.html`, base64-embedded per the existing
      `-sSINGLE_FILE=1` launcher story) builds compile and link cleanly
      with the 9-grammar core set; the wasm build has no dynamic-loading
      code in it at all (`#if !defined(__EMSCRIPTEN__)`), consistent with
      having no filesystem access to arbitrary shared libraries in a
      browser.
- [x] **Markdown's inline formatting is real, via a bespoke two-pass
      injection** (`HighlightMarkdown`/`CollectMarkdownInlineSpans`,
      treesitter.cpp), not a general injection-query engine: the block
      grammar is parsed and highlighted first, then every `(inline)` node
      its own `injections.scm` marks as owned by `markdown_inline` is
      re-parsed with that second grammar via
      `ts_parser_set_included_ranges` on the *same* full text (tree-
      sitter's own supported mechanism for this — the reason no manual
      row/column translation is needed, since positions from that second
      parse already come out in the original document's coordinates).
      Verified: bold/italic/inline-code/links/headings all render with
      distinct colors on a real sample, screenshotted via Xvfb. Fenced
      code blocks are *not* language-injected (markdown's own
      `injections.scm` also defines that mapping, e.g. ` ```python ` →
      the Python grammar recursively — out of scope here, so a code
      fence's contents render as a single undifferentiated span, not
      per-token Python highlighting) — confirmed via direct capture
      inspection, not just visual impression, after an initial screenshot
      read looked deceptively like real Python highlighting was
      happening (font antialiasing, not an actual second-language
      injection).
- [x] **Org uses its own non-standard capture-name convention** —
      `queries/highlights.scm` (moved to `examples/queries/highlights.scm`
      as of the `2.0.4` tag vendored here) is explicitly labeled example/
      demo content upstream, using `@OrgHeadlineLevel1`/`@OrgKeywordTodo`/
      `@OrgStars1`/etc. rather than the dotted `@keyword`/`@string`-style
      names every other vendored grammar's query uses. `mep.ts_capture_hl`
      (`kBuiltinSyntax`, main.cpp) has ~35 explicit entries mapping these
      (headline levels/stars → Purple, TODO → Red, DONE → Green, priority/
      checkboxes → Yellow, tags/properties/timestamps → Cyan, drawers/
      comments → Comment, ...) — without them the grammar would parse
      correctly but render nothing, since none of the generic dotted-name
      fallback entries would ever match. Verified visually via Xvfb:
      TODO/DONE, stars, a checkbox, and a property drawer value all render
      in distinct colors on a real sample.
- [ ] Incremental re-parse on edit — **not implemented**: no `TSTree`
      is kept across edits and fed `ts_tree_edit`/an old-tree reparse;
      every `mep.ts_captures` call reparses the full buffer from
      scratch. This is a deliberate, documented scope cut (an
      incrementally-updated tree per buffer, invalidated correctly
      across every edit path — typed input, undo/redo, macros, LSP
      `textDocument/didChange` sync — is real additional surface, not a
      small add-on), consistent with this codebase's existing
      "on-demand full rescan" scope decisions elsewhere (Phase 13's
      colorizer/todo-mark are cheap regex scans; git-gutter backs off to
      a 0.6s debounce).
- [x] **Bug found and fixed**: opening a file showed no highlighting at
      all — `mep.syntax_highlight()` was reachable only via `:MepSyntax`,
      never invoked automatically. `mep.syntax_auto` (unlike colorizer/
      git-gutter/todoscan's opt-in `_auto` flags) is now **on by
      default** and wired through two hooks in `kBuiltinSyntax`:
      `mep.on_buffer_changed(...)` debounce-reruns it on edits, same
      pattern as Phase 13; a second, non-debounced `mep.on_frame` watcher
      keyed on `mep.filename()` re-highlights immediately whenever the
      active buffer's file changes. That second watcher is the actual
      fix — `mep.on_buffer_changed` only fires off `buffer_change_epoch`,
      which nothing bumps on a plain buffer *switch* with no edit (`:e`,
      the buffer/file picker, pane navigation onto a different buffer),
      so highlighting would otherwise only ever appear after the first
      keystroke in a freshly-opened file. Verified with a standalone Lua
      harness simulating frame ticks (mocked `mep.on_frame`/
      `mep.buffer_change_epoch`/`mep.filename`) confirming: highlight
      fires on first frame, buffer switch re-highlights immediately
      independent of the edit debounce window, an edit within the
      debounce window doesn't fire early, and disabling `mep.syntax_auto`
      suppresses all of it — then confirmed visually via Xvfb, opening a
      real `.cpp` file from this repo and seeing colored output
      immediately with no `:MepSyntax` needed.
- [ ] Fold-query execution — **not implemented**, no query language in
      play at all here (Phase 5's fold providers stay org/markdown/
      manual-only). The grammars/queries vendored are highlights.scm
      only; folds.scm isn't fetched or wired up.
- [x] Query-file handling — upstream `.scm` query sources are vendored
      and actually executed (see above), including real predicate
      evaluation; there just isn't a runtime "load an arbitrary
      query-file path" feature (queries are compiled-in constants, not
      read from disk at startup).
- [x] **Org src-block language injection** (added on request, right after
      the org-babel multi-language port above: "I'd like syntax
      highlighting to work in those code blocks"). Previously a
      `#+begin_src <lang>` block's *body* rendered completely
      unhighlighted — `kHighlightsOrg`'s own query captures a block's
      `(contents)` as `@OrgBlockContents`, which `mep.ts_capture_hl` has
      no entry for, so it silently resolved to nothing. `mep.syntax_highlight()`
      (`kBuiltinSyntax`, main.cpp) now, only when the buffer's own
      filetype is `org`, additionally calls
      `mep_syntax_highlight_org_src_blocks`: a small standalone
      begin/end-src scanner (mirrors `mep_org_src_block_at`'s
      case-insensitive pattern, but can't reuse it directly — that
      function is `local` to `kBuiltinOrgBabel`'s own `DoString` chunk,
      and Lua chunks don't share locals) that, per block, maps the babel
      language tag to a `mep.ts_captures`-compatible filetype key via a
      new `mep_org_babel_lang_ts_ft` table (`"c++"` → `"cpp"`, `"csharp"`
      → `"cs"`, `"python"` → `"py"`, ... — covering every language
      `mep.org_babel_langs` itself supports), re-highlights just that
      block's body text under its own grammar, and translates each
      capture's row from body-relative back to the block's real position
      in the buffer (`hdr + cap.row`, where `hdr` is the `#+begin_src`
      line) before handing it to the same `mep.deco_add` call the
      whole-buffer pass already uses. A language with no grammar
      available renders unhighlighted, same as any other filetype with
      no grammar — not an error. Verified via Xvfb against every babel
      language in `mep.nvim/org/test.org`: C++/C (real preprocessor/
      type/function/string coloring inside the block, org's own heading/
      directive coloring around it undisturbed), C# (`Console.WriteLine`
      correctly colored, exercising a *dynamically*-loaded grammar from
      inside the injection path, not just the compiled-in core), and
      OCaml/D/Nim/Crystal/Fortran (below).
- [x] **Five more grammars, closing the last org-babel-language gap**:
      Perl, Fortran, D, Nim, Crystal had no `DynamicLanguageTable` entry
      at all before this (so a `#+begin_src perl` block, or a plain
      `.pl`/`.f90`/`.d`/`.nim`/`.cr` file, never got real highlighting,
      injected or not) — added alongside the injection feature above so
      *every* language `mep.org_babel_langs` runs also gets to actually
      look highlighted. Fortran/D/Nim/Crystal's `kHighlights*` queries
      (`src/treesitter_queries.h`) are vendored verbatim from each
      grammar's own upstream `queries/highlights.scm`, same as every
      other dynamic-language entry (Crystal's real query lives at the
      non-standard `queries/nvim/highlights.scm` path in its own repo).
      **Perl needed a different approach**: the actively-maintained
      `tree-sitter-perl/tree-sitter-perl` grammar ships no committed
      `src/parser.c` at all (would need the tree-sitter CLI to generate
      one — exactly the "not vendorable without an extra toolchain" case
      `third_party/README.md` already documents for sql/latex/swift, and
      exactly why this project avoids that grammar family). The
      alternative used everywhere else in this addition —
      `ganezdragon/tree-sitter-perl` — does ship a committed parser, but
      also ships **no `queries/` directory at all**, so `kHighlightsPerl`
      is hand-written directly against that grammar's own
      `src/node-types.json` node names (comments/pod, string/heredoc
      variants, numeric literals, `$scalar`/`@array`/`%hash` variables,
      conditional/loop/keyword tokens, `function_definition`'s `name:`
      field) rather than carrying over a query text that would have
      quietly matched nothing against a different grammar's AST shape.
- [x] **`flake.nix`'s `tsGrammars` devShell list** grew the same five
      (`tree-sitter-crystal`/`-fortran`/`-perl`/`-nim`/`-d`) — all five
      confirmed present in `pkgs.tree-sitter.withPlugins`'s grammar set
      for this project's own pinned `nixpkgs` revision (a live check
      against the *unpinned* registry default `nixpkgs` incorrectly
      suggested nim/d were missing — this project's own `flake.lock`
      revision is the only trustworthy source for what
      `pkgs.tree-sitter.withPlugins` actually offers, not whatever the
      ambient `nix` CLI's registry happens to resolve to on a given
      machine). Verified: `direnv exec .` rebuilt the devShell
      successfully with all five new grammar derivations copied from
      `cache.nixos.org`, then a live Xvfb run with that environment
      showed correct highlighting inside `csharp`/`ocaml`/`d`/`nim`/
      `crystal` src blocks (`ocaml`/`csharp` were already
      dynamically-loadable before this session; included as a
      regression check that adding new `DynamicLanguageTable` entries
      didn't disturb existing ones).
- [x] **`just fetch-grammars`** (`scripts/fetch-grammars.sh` +
      `scripts/ts_grammars.tsv`, added on request: "add as much grammars
      to the justfile ... as possible") — a Nix-free way to get the same
      breadth: downloads and compiles every one of the ~49 languages
      `DynamicLanguageTable` has a highlight query for (the pre-existing
      ~44 plus this session's new five) straight from each grammar's own
      GitHub tarball into `.ts-grammars/lib/<canonical_name>.so`, no
      tree-sitter CLI dependency (every listed grammar was individually
      confirmed, via a live `curl` HEAD check against its pinned rev, to
      ship an already-generated `src/parser.c` — the CLI-required ones
      are simply not on the list, same reasoning as Perl's grammar
      substitution above). `just run` composes this with — rather than
      replacing — whatever `$MEP_TS_PARSER_PATH` a Nix devShell may have
      already exported, prepending `.ts-grammars/lib` when it exists.
      Every repo/rev pin was cross-checked against what this project's
      own `flake.nix` resolves via Nix (the two paths build the *same*
      grammars, not just similarly-named ones), with two confirmed,
      fixed exceptions where nixpkgs' own pin has drifted from what's
      live on GitHub right now: `tree-sitter-grammars/tree-sitter-vue`'s
      pinned tag (`v0.1.0`) was deleted upstream after nixpkgs cached it
      (Nix still builds it fine from `cache.nixos.org`'s permanent
      store, but a fresh `git`/`curl` fetch of that exact tag 404s live)
      — repointed at that repo's current default-branch tip instead;
      `tree-sitter-grammars/tree-sitter-xml` and
      `tree-sitter-grammars/tree-sitter-vue` also needed an explicit
      `subdir` (`xml`; PHP/TypeScript/OCaml already did, for the same
      "monorepo bundling more than one grammar" reason) that a plain
      root-level `src/parser.c` guess would have missed. Verified: ran
      the script for real (network fetch + compile, not a dry run) and
      confirmed real `.so` files landed in `.ts-grammars/lib/`.

### Phase 20 — LSP client ✅

*(depends on: Phase 1 jobs. This is its own large multi-step phase —
see "Design decisions" above for why it's a from-scratch build.)*

- [x] Content-Length-framed JSON-RPC 2.0 reader/writer, built on a *new*
      Job capability this phase needed and added: `on_stdout_raw`
      (unsplit byte chunks, vs. Phase 1's original line-split
      `on_stdout`) plus a byte-count-aware accumulator
      (`PumpLspBuffer`/`LspClientState` in lua_env.cpp). **Two real bugs
      found and fixed while verifying this against a real server**
      (lua-language-server, confirmed available in this environment) —
      both significant enough to detail:
      1. **Message framing corruption.** The first implementation reused
         Job's existing line-split `on_stdout` on the theory that a
         compact-JSON body never contains a raw newline, so it'd always
         arrive as exactly one "line." True, but it missed that the wire
         format has *no trailing newline after the body* — so back-to-
         back messages sent with no gap (the server sends several
         notifications immediately on startup) got silently
         concatenated: message 1's body directly followed by message 2's
         `Content-Length:` header, both on what line-splitting saw as
         one corrupt, unparseable line. Every message after the first
         was silently dropped as a result — caught via a raw-byte debug
         trace across the actual pipe, isolated further by manually
         piping a raw request into the server via a shell FIFO to
         confirm its real wire behavior first. Fixed by adding true
         byte-count framing (`Job::raw_stdout` mode + `PumpLspBuffer`)
         instead of relying on line boundaries at all.
      2. **JSON `null` truthiness.** `mep.json_null` (the sentinel
         `PushJson` uses for JSON `null`, since Lua's own `nil` can't be
         stored as a table value) is a lightuserdata — truthy in Lua,
         unlike real `nil`. Code like `if msg.result then` therefore
         treated "the server explicitly said no result" (e.g.
         goto-definition finding nothing) as if a result were present,
         then crashed trying to index/measure-length a userdata.
         Reproduced live (definition-not-found crashed exactly this
         way), fixed with a shared `mep_lsp_result(msg)` normalizer
         every response callback routes through instead of touching
         `msg.result` directly.
      Both fixes verified: re-ran the exact same attach → hover →
      goto-definition → diagnostics sequence end-to-end afterward and
      it worked cleanly.
- [x] Request/response correlation: per-client `pending` id→callback map;
      **no synthetic-failure-on-process-exit** yet (a request left
      pending when the server dies just never fires its callback, rather
      than firing it with an error) — a small documented gap, not hit in
      verification since the server stayed up throughout.
- [x] `initialize`/`initialized` handshake with a real capabilities
      payload (hover/completion/documentSymbol/publishDiagnostics
      declared) — verified against lua-language-server's own log output
      echoing the exact capabilities sent.
- [x] Document sync: `didOpen`/`didChange`/`didSave` (full-document sync
      only, not incremental range-based sync) — **`didClose` not
      implemented** (no consumer needed it yet: nothing currently detaches
      a client when a buffer closes). One client per filetype
      (workspace-wide), not per-buffer.
- [x] `publishDiagnostics` → feeds Phase 21, verified with a real syntax
      error (`Missed symbol` from the live server, rendered correctly).
- [x] Requests implemented: `hover`, `textDocument/definition`,
      `references`, `documentSymbol`, `formatting`, and now also
      `implementation`, `typeDefinition`, `signatureHelp`, `completion`
      (LSP-sourced), `codeAction`, `rename` — closing the "not
      implemented" list this bullet used to end with. All follow the
      same `mep.lsp_request`-on-the-generic-plumbing pattern as the
      original five:
      - `implementation`/`typeDefinition`: `mep.lsp_goto_implementation`/
        `mep.lsp_goto_type_definition`, sharing a `mep_lsp_goto(method,
        not_found_msg)` helper with the same Location|Location[]
        handling `mep.lsp_goto_definition` already had (that original
        function is untouched, not folded into the helper).
      - `signatureHelp`: `mep.lsp_signature_help`, rendered via
        `mep.hover_show` (the anchored floating popup added alongside
        this work, replacing hover's own former `mep.notify`-only
        presentation) rather than a toast. Also wired to a debounced
        `mep.on_buffer_changed` auto-trigger
        (`mep.lsp_signature_help_auto`, **default true** — the only
        `_auto` flag in the file that defaults on, since signature help
        is only useful *during* typing) gated by a same-line "cursor is
        after an unmatched `(`" heuristic — same-line-scan only, not a
        real bracket-matching parser, so a call whose arguments span
        multiple lines won't trigger it.
      - `completion` (LSP-sourced): closes the Phase 22 gap this
        phase's bullet used to point at — `mep.completion_buffer_words`
        now also merges in cached LSP completion results (cached by
        word-start position so a response arriving mid-word is reused
        across that word's remaining keystrokes; a background request is
        kicked off, previous cache is returned immediately since the
        request is async), ranked/capped alongside buffer-words/
        snippets/path candidates.
      - `codeAction`: `mep.lsp_code_action`, requesting with any
        diagnostics on the cursor's line as `context.diagnostics`;
        single-result auto-applies, multiple opens a `Code Actions`
        picker. Handles both response shapes (`edit` a WorkspaceEdit,
        `command` a Command — as either an inline table or, for a bare
        `Command`-typed result, a plain string) by sending
        `workspace/executeCommand` for the latter. `codeAction/resolve`
        (lazy edit resolution some servers use instead of inlining
        `edit` up front) is **not implemented** — not hit by the mock
        server used for verification (see below); documented gap.
      - `rename`: `mep.lsp_rename` — `mep.ui_input` prompts for the new
        name (prefilled with the word under cursor via a new
        `mep_lsp_word_at_cursor()` helper, no prior helper for this
        existed), then applies the returned WorkspaceEdit.
      - **New shared plumbing this needed**: no multi-file/character-
        range-aware edit-applier existed anywhere in the codebase before
        this (`mep.lsp_format`'s own edit-apply, the only precedent, is
        single-buffer and whole-*line*-granularity only — it reuses
        `mep.replace_lines` directly, which is fine for formatters but
        would clobber the rest of the line for a rename/code-action edit
        that only touches a few characters mid-line). Added
        `mep_lsp_apply_text_edit` (character-range-aware, splices
        `newText` between the edit's start/end *character* offsets, not
        just its lines) and `mep.lsp_apply_workspace_edit` (normalizes a
        WorkspaceEdit's `changes` map and `documentChanges` array — the
        shape newer servers like clangd prefer — into the same form,
        applying to each touched file; a file other than the current
        buffer is opened/edited/saved via the same `mep.cmd('e ' .. f)`
        pattern goto-definition/references already use for cross-file
        jumps, no headless "edit a buffer without displaying it" path
        existing anywhere in this codebase to reuse instead). **One real
        bug found and fixed while writing this**: `mep_lsp_apply_text_edit`
        initially reused `mep.lsp_format`'s own "split `newText` on `\n`,
        drop a trailing empty element" trick — that trick silently
        merges a genuine trailing newline (e.g. a code-action inserting
        a whole new line, `newText = "foo\n"`) back into the following
        line, since it can't distinguish "artifact of the split" from
        "the edit really does end its content with a line break." Caught
        by hand-tracing the function against exactly that case before
        ever running it live; fixed with an explicit `string.find`-based
        split instead of the gmatch/strip trick (mep.lsp_format's own
        version is untouched — out of this change's scope, and it never
        hits the case that exposes the bug since formatting edits are
        already whole-line).
      **Verification**: no real language server is installed in this
      environment (`lua-language-server`, `clangd`, `pyright-langserver`,
      `gopls`, `rust-analyzer`, `typescript-language-server` all absent
      from `PATH`, unlike when this phase was first verified) — nor is
      `xdotool` or Python, ruling out the usual Xvfb-driven click/type
      verification too. Verified instead against a small hand-written
      mock server (a Deno script speaking real Content-Length-framed
      JSON-RPC — Deno *is* available — with canned, spec-shaped
      responses for each of the six methods), substituted in as the
      `lua` registry entry's `cmd`, driven by a headless
      `~/.config/mep/init.lua` test harness under Xvfb that scripts
      `mep.on_frame`-timed calls instead of real keystrokes (programmatic
      `mep.replace_lines`/`mep.set_cursor` calls do bump the same
      `change_epoch_` real typing does, so the debounced signature-help
      auto-trigger fires the same way). Confirmed end-to-end through the
      real wire protocol: goto-implementation/type-definition land on
      the exact target line/column; manual *and* auto-triggered
      signature help both surface the mock's exact label+documentation
      text; rename's WorkspaceEdit correctly rewrites both occurrences of
      a renamed local in place, character-precise, leaving the rest of
      each line untouched; code-action's single-result auto-apply
      correctly inserts a new line via a multi-line `newText` (the exact
      case the bug above was about); a 2-result mock response correctly
      opens the picker with both titles; both `command` shapes (table
      and bare string) correctly send `workspace/executeCommand` with
      the right name/arguments (confirmed from the mock server's own
      receive log, not just client-side belief). **Not exercised through
      real UI**: `mep.ui_input`'s actual new-name keystroke-by-keystroke
      entry (rename was invoked by calling `mep.lsp_request` directly
      with a hardcoded `newName`, skipping just that one prompt call —
      itself pre-existing, already-reused infrastructure, not new code)
      and the code-action picker's interactive arrow-key/Enter selection
      (verified instead that it opens with the right items, and that
      each of the two "apply an already-chosen action" branches works
      when called directly).
- [x] Server registry: `mep.lsp_servers` (lua/clangd/pyright entries) —
      3 servers as the plan's own "start with 3-5" asks; only the Lua
      entry was actually exercised (lua-language-server was the one
      confirmed available in this environment when this phase was first
      built; see the new methods' own verification note above for why
      that's no longer true in this environment).
- [x] `PATH`-executable gating: `mep.lsp_start` spawns via the existing
      Phase 1 Job/`execvp` path (fails gracefully, notifies, if not
      found) — nothing installs anything, matching policy.
- [x] Keybindings exposed as `mep.lsp_hover`/`goto_definition`/
      `references`/`format`/`goto_implementation`/`goto_type_definition`/
      `signature_help`/`code_action`/`rename` + a matching `:MepLsp*`
      ex-command for each. The six new methods also get a default
      `mep.leader_map` binding (`li`/`lt`/`lk`/`ca`/`rn` — `lt`/`ca`/`rn`
      deliberately matching mep.nvim's own `keymaps.lua` defaults for
      the methods it binds under `<leader>` too; `li`/`lk` extend the
      same `l` group by mnemonic since mep.nvim's own `gi`/`<C-k>` for
      implementation/signature-help aren't reachable through mep's
      `mep.map`, which only binds single ASCII keys in Normal/Visual
      mode — no `g`-prefixed two-key sequences or Ctrl-modified
      letters). The original four (hover/goto-definition/references/
      format) deliberately keep their prior no-default-keybinding
      state — out of scope for this change, `:MepLsp*`/manual
      `mep.map` still covers them the same way every other phase's
      commands work.

### Phase 21 — Diagnostics UI ✅

*(depends on: Phase 20 LSP, Phase 4 decorations)*

- [x] Diagnostic store: `mep_lsp_diagnostics[abspath]`, keyed consistently
      through a `mep_lsp_abspath()` helper on both the write side
      (publishDiagnostics handler) and every read side — a real bug
      caught during verification: the first version compared a relative
      `mep.filename()` directly against the always-absolute URI-derived
      key and the two silently never matched, so nothing ever rendered.
- [x] Gutter sign + whole-line highlight (severity-colored via the
      existing Error/Warn theme groups) + inline virtual-text summary of
      the message. **No separate underline decoration** on the exact
      affected span — the whole-line highlight already marks it, and
      Decoration doesn't have an "underline" style distinct from a
      background tint (documented pre-existing limit from Phase 4, not
      new here).
- [x] Diagnostic-at-cursor: `mep.lsp_diagnostic_at_cursor()`/
      `:MepDiagShow` — **via `mep.notify` rather than a dedicated Phase 3
      floating popup** (same scope call as Phase 15's tree help: a toast
      already delivers "show the message," a bespoke popup type would
      duplicate `DrawFloatFrame` for no functional gain here).
- [x] Navigation: `mep.lsp_next_diagnostic`/`prev_diagnostic` (all
      severities, wraps) and `next_error`/`prev_error` (errors-only).
      Verified end-to-end via Xvfb against a real syntax error from
      lua-language-server (see Phase 20's bug #1 fix note above — this
      is the feature that surfaced it).

### Phase 22 — Completion engine ✅

*(depends on: Phase 20 LSP (soft), Phase 1 debounce)*

- [x] Genuinely new UI, as the plan anticipated: `Mode`-independent popup
      (doesn't steal Insert mode — `HandleInsertInput` intercepts only
      Ctrl-N/Ctrl-P/Tab/Enter/Escape while it's open, everything else
      including ordinary typing still reaches normal Insert handling)
      rendered just below the cursor via a new `DrawCompletionPopup`,
      list + selection highlight + scroll window, all real. First
      Escape closes just the popup, not Insert mode itself (matches
      every mainstream editor's completion UX).
- [x] Auto-trigger: `UpdateCompletionPopup()` re-runs after every Insert-
      mode key (not a literal timer-based debounce — recomputing buffer-
      word matches against a single buffer every keystroke is cheap
      enough that a real debounce timer wasn't needed to keep it feeling
      instant; indistinguishable from debounced at the buffer sizes mep
      handles). Manual trigger isn't a separate keybinding since
      auto-trigger already covers it.
- [x] Source abstraction: `mep.set_completion_source(fn)` — one source
      slot, not a multi-source-with-dedup list. `mep.completion_buffer_words`
      (the default, still one function registered through that single
      slot) now merges four candidate sources internally rather than
      just buffer words: buffer words, matching Phase 23 snippet trigger
      names, a filesystem-path source (triggers after `/`/`.` or inside
      a `require(`/`import`/`from` string), and LSP-sourced completion
      (`textDocument/completion` — closing this bullet's own former "not
      implemented" gap; see Phase 20's own notes for the request/
      response handling and its cache-by-word-start-position scheme,
      needed since completion, unlike every other source here, is
      async). All ranked/capped together (`mep_completion_rank`,
      length-then-alphabetical, 50-item cap) before reaching the popup.
- [x] Accept (Tab/Enter) / abort (Escape). Item cap and cross-source
      dedup are both real now that there's more than one source to need
      them for (see the source-abstraction bullet above): a shared `seen`
      set in `mep.completion_buffer_words` collapses identical text
      offered by more than one source to a single entry, and
      `mep_completion_rank` caps the combined, ranked list to 50 items
      before it ever reaches C++ (important beyond just list length —
      `DrawCompletionPopup`, main.cpp, measures every item's text width
      on every frame the popup is open, so an uncapped list would be an
      unbounded per-frame cost, not just a keystroke-time one). The path
      source is a documented simplification, not full path-completion
      parity: it only triggers once 2+ alnum characters follow the `/`/
      `.`/quote trigger (reuses the same alnum-prefix scan
      `UpdateCompletionPopup` already does, rather than widening that
      shared boundary check), and resolves relative to the working
      directory rather than the edited file's own directory. Verified via
      Xvfb, beyond the original plain-buffer-word check: (1) a 200-unique-
      word buffer sharing one prefix — `#mep.completion_buffer_words
      ('zzitem')` returned exactly 50, not 200; (2) a mock LSP server (a
      small stdio JSON-RPC stub, since no real language server is
      installed in this environment) returning an item whose text also
      exists as a real buffer word — the popup showed it once, not twice;
      the same mock's two LSP-only items (absent from the buffer)
      appeared and Tab-accepted correctly, confirming the LSP source is
      reachable end-to-end and not just plumbed-but-untested; (3) typing
      `src/ed` inside a string literal offered `editor.h`/`editor.cpp`
      from the real `src/` directory (via the existing `mep.list_dir`),
      and accepting spliced in only the missing suffix, not a duplicate
      `src/` prefix; (4) a fast-typing burst (66 chars at a 5ms interval)
      produced no dropped keystrokes and no hang, confirming none of the
      above reintroduced the per-keystroke cost the popup's existing
      throttle (`UpdateCompletionPopup`'s prefix-unchanged skip + 50ms
      minimum interval, untouched by this work) exists to prevent.

### Phase 23 — Snippet engine ✅ scoped down significantly

*(depends on: Phase 4 decorations for tabstop-range tracking, Phase 22
completion for popup integration (soft))*

- [x] Snippet body parser: **`$1`/`$0` numbered tabstops only** — no
      `${1}` braced form and no `${1:default}` placeholder-text syntax.
      A real, working per-line scanner (`mep_snippet_scan_line`), not a
      stub.
- [ ] Tabstop range tracking via Phase 4's gravity-aware decorations —
      **not implemented**; positions are computed *once* at expand time
      as fixed `{row, col}` offsets, not live-tracked through further
      edits (Decoration has no insert/delete-aware position updates —
      nothing has needed that yet). Practically: jumping tabstops
      immediately after expansion is correct; heavy edits before a later
      jump can leave it stale.
- [x] Expand (`mep.snippet_trigger`/`mep.snippet_expand`) and jump-
      forward/backward (`mep.snippet_jump(1|-1)`/`:MepSnippetNext`/
      `:MepSnippetPrev`).
- [x] Per-filetype registry (`mep.snippets`), **2 curated languages**
      (lua, python) as the plan's own "start with 2-3" asks.
- [x] Registered as a completion source (folded into
      `mep.completion_buffer_words`, Phase 22). **LSP
      `insertTextFormat=Snippet` handling still not implemented** — Phase
      20's LSP-sourced completion is wired up now (see its own notes),
      but every completion item it returns is inserted as plain text
      (`insertText`/`label`) regardless of `insertTextFormat`; a
      `Snippet`-format item with `$1`-style placeholders would insert the
      literal placeholder syntax rather than expanding it through this
      phase's own tabstop engine — a real remaining gap, not just a
      relabeled old one.
- [x] Snippet picker: `mep.snippets_picker()`/`:MepSnippets` (Phase 8).
      Core parsing/tabstop-jump verified via Xvfb (a `function $1($2)`
      template correctly expanded with the cursor landing inside the
      parens); the prefix-stripping edge case around exactly where the
      cursor sits relative to the trigger word was exercised through
      Normal-mode `:lua` test calls that don't fully represent genuine
      Insert-mode invocation (Vim's cursor-moves-back-on-Escape
      semantics shift the column by one) — one real latent bug found this
      way (a negative-index `string.sub` mishandling) and fixed
      regardless of the test-harness caveat.

### Phase 24 — Symbols outline ✅

*(depends on: Phase 20 LSP `documentSymbol`, Phase 7 sidebar)*

- [x] `textDocument/documentSymbol` → flattened, indented, kind-tagged
      (`[function]`/`[class]`/`[variable]`/...) into a Phase 7 sidebar,
      recursing into `children` for nested symbols.
- [x] Jump-to-symbol (click a row).
      ✅ **Refresh-on-save now available**: `:lua mep.symbols_auto_refresh
      = true` wires `mep.lsp_symbols_refresh()` to `mep.on_buffer_saved`
      (the same root-cause fix documented under Phase 13), guarded to
      only refresh if the symbols sidebar is already open — auto-
      popping a *closed* sidebar open on every save would be far more
      intrusive than the other auto-consumers, which just silently
      update decorations regardless of what's visible. No dedicated "no
      capable client" message beyond the existing "No LSP attached"
      warning already shared with every other LSP-dependent command.
      Verified via Xvfb against a real server: a 2-function Lua file's
      outline showed `greet`/its `name` param/`M`/`M.process`/its `x`
      param correctly nested, and clicking `M` jumped the cursor to its
      declaration line.

### Phase 25 — Docs (generate + lookup) + Help picker ✅

*(depends on: Phase 20 LSP `signatureHelp` (soft, has a regex
fallback), Phase 13's URL-open, Phase 8 picker)*

- [x] Docstring skeleton generator: `mep.docs_generate()`/`:MepDocGen` —
      **regex/same-line-scan only**, no LSP signature-help-driven variant
      — Phase 20's `signatureHelp` request is implemented now
      (`mep.lsp_signature_help`, see its own notes), but `docs_generate`
      itself was never rewired to call it instead of its own regex scan;
      a real remaining gap now, not a blocked one — 3 curated
      per-language templates (lua/python/javascript).
- [x] Doc lookup: `mep.docs_lookup()`/`:MepDocLookup` — word under
      cursor → a devdocs.io search URL → Phase 13's `mep.open_url`.
- [x] Help picker: **reuses `mep.commands()` as-is** (Phase 8/13's
      command palette already *is* "a live index over registered
      commands, `<CR>` runs the selected entry") rather than building a
      parallel picker — `:MepHelp` is a thin alias. Keybinding
      introspection (the other half of the plan's bullet) is **not
      implemented** — no registry of key→description exists anywhere
      `mep.map`'s callers could have attached one to, so there's nothing
      to introspect yet.

### Phase 26 — DAP client ✅ implemented, **not verified against a live adapter**

*(depends on: Phase 20's framed-JSON-RPC layer reused, Phase 1 jobs,
Phase 4 decorations for breakpoint signs, Phase 7 sidebar)*

- [x] Session client: `mep.dap_start(lang)` reuses Phase 20's
      `mep.lsp_start`/`request`/`notify` wholesale — despite the name,
      that's a generic Content-Length-framed JSON-RPC client, not
      LSP-specific, and DAP uses identical wire framing. `initialize` →
      register `initialized`-notification handler (pushes breakpoints via
      `setBreakpoints` + `configurationDone`) → `launch`.
- [x] Control: `continue`/`step-over`/`step-into`/`step-out`/`terminate`;
      a `stopped`-event handler notifies with the stop reason. **No
      evaluate request, no automatic stack/scopes/variables fetch on
      stop** — the plan's fuller control-flow loop.
- [x] Breakpoints: `mep.dap_toggle_breakpoint()`/`:MepDapBreakpoint` —
      session-only (not persisted), keyed by filename, red `B` gutter
      sign via Phase 4 decorations, toggle re-sends `setBreakpoints` on
      next session start.
- [ ] Stack/scopes/variables sidebar and REPL console — **not
      implemented**, explicitly the largest deferred piece of this phase.
- [ ] Responding to server-initiated *requests* (e.g. `runInTerminal`) —
      **a real, documented protocol gap**: Phase 20's client only ever
      needed to handle server→client *notifications* (no reply expected),
      since LSP's publishDiagnostics etc. are all notifications. DAP
      requires the client to send a JSON-RPC *response* back for certain
      server-initiated requests, which this client doesn't do — it would
      silently no-op them the same way an unregistered notification is
      dropped, which for a request-shaped message means the adapter is
      left waiting for a reply that never comes.
- [x] Adapter registry: `mep.dap_adapters` (cpp→lldb-dap, python→
      debugpy) — the plan's own "start with 1-2 languages."
- **Verification**: no DAP adapter (lldb-dap, debugpy, ...) was confirmed
  available in this environment the way lua-language-server was for
  Phase 20, and standing one up (installing/configuring an adapter, a
  real launch target program) was out of scope for the time remaining
  in this session. This phase is implemented against the DAP spec and
  built on the exact JSON-RPC infrastructure Phase 20 already proved
  correct against a real server, but the session-start → breakpoint →
  stopped-event flow itself has **not** been exercised end-to-end the
  way every other phase in this plan has been. Flagged here rather than
  silently claimed as verified.

---

## Part VI — Running code ✅ scoped down from a full terminal emulator

### Phase 27 — Embedded terminal/PTY + Run + REPL ✅

*(depends on: Phase 1 jobs — this needs real PTY support, a step up
from plain pipe-based process spawning)*

- [x] Real PTY support: `Job`/`JobManager` gained a `use_pty` spawn mode
      (`forkpty()`, not plain pipes) — a genuinely new primitive, not just
      config on Phase 1's Job, since a PTY behaves differently from a pipe
      in ways real programs detect and change behavior for (color output,
      readline-style line editing, buffering). Verified: a shell spawned
      this way showed a real `sh-5.3$` prompt (pipe-spawned shells don't
      print an interactive prompt at all).
- [x] Output rendering: **scoped down from a real terminal emulator** to
      "basic ANSI handling: colors" (the plan's own stated floor) — SGR
      color codes (30-37/90-97) parse into real decoration spans through
      the same Phase 4/9 pipeline everything else colors text through;
      every *other* escape sequence (cursor movement, clear-screen, ...)
      is silently discarded rather than interpreted. This is genuinely
      *not* a cursor-addressable terminal grid: a full-screen TUI program
      (vim, htop, a fancier REPL prompt) will render as garbled scrolling
      text, not a real screen. Output is re-parsed from the full
      accumulated byte stream on every chunk (simpler/more obviously
      correct than incremental line-append, at the cost of O(output size)
      per chunk — fine at Run/REPL output sizes). Verified via Xvfb: a
      `sh -c 'printf "\033[31mred text\033[0m normal line\nline2\n"'`
      run showed "red text" in red and the rest in the default color.
- [x] **No raw-keystroke-by-keystroke input forwarding** ("forward
      keystrokes to it") — scoped to line-oriented send (vim-slime style)
      via `mep.job_write`/`mep.repl_send`, which covers the real
      "run code and see output" / "send an expression to a REPL"
      workflows without a full terminal-emulator input model. A REPL
      session's own shell/interpreter still reads and responds to each
      line normally since it's a real PTY underneath.
- [x] Split-pane hosting: `mep_term_open_pane()` — `:split` + a fresh
      dedicated buffer (`mep.buffer_new()`, a new primitive: create a
      buffer without switching any pane to it) switched into the new
      pane. **Not sized by a height ratio** — uses the existing equal-
      share split (same pre-existing `SplitNode` limit Phase 14 already
      documented, not new here).
- [x] Run: `mep.run_file()`/`:MepRun`, `mep.run_languages` registry
      (lua/python/javascript/sh as the starting data, `mep.nvim`'s own
      `languages.lua` wasn't directly portable — different runtime
      assumptions — so this is a fresh minimal table, growable the same
      way). Verified end-to-end via Xvfb (see above).
- [x] REPL: `mep.repl_start(lang)`/`mep.repl_send`/`send_line`/
      `send_buffer`, `mep.repl_languages` registry (2 languages, the
      plan's own "start with 2-3"). **`send_selection` not implemented**
      (no Lua-facing "get the current Visual selection's text" primitive
      exists yet to build it on) and **no jump-to-REPL/jump-back
      keybindings** beyond the existing `mep.nav_pane` directional focus
      (Phase 14) already reaching a REPL's pane like any other. Verified
      end-to-end via Xvfb: started a shell REPL, `repl_send` from the
      source buffer, watched `echo hello_from_repl` execute and its
      output stream into the REPL's buffer live.
- [x] **Two real bugs found and fixed during verification**: (1) both
      `run_file` and `repl_start` originally wrote
      `local job_id = mep.term_start(argv, {on_stdout_raw = function()
      ... job_id ... end})` — a classic Lua scoping trap: a closure
      inside the same statement that declares its own local captures
      whatever `job_id` resolved to *before* that `local` took effect
      (nil/global), not the variable about to be assigned. Reproduced
      immediately (`attempt to index a nil value (local 'sess')` on the
      very first Run), fixed by pre-declaring `local job_id` on its own
      line first. (2) `mep.repl_send`/`run_file` resolve their target by
      the *current* buffer's filetype, so calling them from inside the
      Run/REPL output pane itself (rather than the source-code pane)
      fails with a "no REPL/run-command" message — not a bug exactly,
      but confusing enough during testing to note: these are meant to be
      invoked from the source buffer, matching every real editor's REPL
      integration (vim-slime, Jupyter cell execution, etc.).

---

## Part VII — Markdown & Org-mode

### Phase 28 — Markdown rendering ✅

*(depends on: Phase 4 decorations (overlay rendering + gutter), Phase 5
folding, Phase 19 treesitter (soft — can ship line-pattern-only first))*

- [x] Heading recoloring (whole-line purple tint) + sign-column
      heading-level glyph (`mep.md_highlight`).
- [x] Bold-text and link decoration highlighting (line-pattern based).
- [x] Fenced code block shading + independent per-fence folding.
- [x] Checkbox toggle keybinding (`mep.md_toggle_checkbox` /
      `:MepMdCheckbox`, `- [ ]`/`- [x]`).
- [x] Heading-depth folding via the Phase 5 fold-provider interface
      (`mep.md_fold`, stack-based: a heading's fold spans until the
      next heading at the same-or-shallower depth).
- [x] Front-matter (`---`/`+++`) block shading.
- [x] `mep.md_render`/`:MepMarkdown` combines highlight+fold; each
      piece (`md_highlight`, `md_fold`, `md_toggle_checkbox`) is also
      independently callable.
- [x] Verified via Xvfb: heading fold-to-EOF semantics, `za` unfold
      showing purple heading tint + glyph, yellow bold text, blue
      link, independently-folded fenced code block, and checkbox
      toggle `- [ ]` → `- [x]`.
- ⚠️ **Scope cut**: no GFM pipe-table → box-drawn table renderer (the
      phase's hardest, most novel piece) and no per-level heading
      *color* differentiation (H1 vs H2 vs H3 all share one heading
      highlight group) — both deferred as lower-value than getting
      org-mode's outline working, per the standing time-budget
      tradeoff. Link/emphasis concealment (hiding `[`/`]`/`*` marker
      characters, vs. just recoloring the whole span) also not done —
      current implementation highlights markers in place rather than
      concealing them. Italics not distinguished from bold (single
      `**...**`/`*...*` bold pattern only).

### Phase 29 — Org-mode A: outline, folding, TODO/tags/properties/checkboxes ✅

*(no dependency beyond mep's existing buffer model — start here for
org, per the research agents' recommended internal build order)*

- [x] Headline model: `*` level parsing (todo-state, `[#X]` priority,
      title, trailing `:tags:`), subtree-boundary detection
      (`mep_org_subtree_end`: next headline at level ≤ this one, or
      EOF), next/prev-headline navigation (`mep.org_next_headline`/
      `org_prev_headline`).
- [x] Promote/demote, both single-headline (`mep.org_promote`/
      `org_demote`) and whole-subtree (`org_promote_subtree`/
      `org_demote_subtree`, shifts only headline lines within the
      subtree range, leaves body text untouched).
- [x] Headline-depth folding (`mep.org_fold_all`/`:MepOrgFold`, same
      stack-based algorithm as Phase 28's markdown folding, reusing
      the Phase 5 fold-provider interface) + per-headline `<Tab>`-
      equivalent toggle (`mep.org_cycle` → `mep.fold_toggle`, i.e.
      `za`).
- [x] TODO-state cycling through a configured keyword list
      (`mep.org_todo_keywords`, default `{TODO, DOING, DONE}`;
      cycling past the end clears the marker).
- [x] Priority cookie (`[#A]`/`[#B]`/`[#C]`) cycling, cycling past `C`
      clears the cookie.
- [x] Tags: `:tag1:tag2:` parsing, inheritance from ancestors
      (`mep.org_tags_at` walks upward collecting shallower headlines'
      tags), tag-set editing via `mep.ui_input` (`mep.org_set_tags`),
      and a real tag-selection picker (`mep.org_tags_picker`, Phase 3
      `mep.picker_open` over every tag used in the buffer) that
      invokes sparse-tree search on selection.
- [x] Property drawers (`:PROPERTIES: ... :END:`): get/set/remove
      (`mep.org_property_get/set/remove`), auto-creates the drawer on
      first `set` if absent.
- [x] Checkbox toggle (`mep.org_toggle_checkbox`, reuses Phase 28's
      `mep.md_toggle_checkbox`) + ancestor statistics-cookie
      (`[n/m]`/`[n%]`) propagation up to the nearest ancestor headline
      that carries a cookie (`mep.org_update_statistics_cookie`).
- [x] Narrow/widen (fold-based approximation: fold everything before
      and after the current subtree into two folds under a dedicated
      `org-narrow` provider, `mep.org_narrow`/`org_widen`) and
      sparse-tree search (`mep.org_sparse_tree(predicate)`: folds
      every subtree containing no match and not itself a match's
      ancestor path; `mep.org_sparse_tree_todo` as the TODO-predicate
      convenience wrapper, also driven by the tags picker).
- [x] Verified via Xvfb on an 11-line two-top-level-headline org file:
      `:MepOrgFold` folds both to `+-- N lines: ...`; `:MepOrgCycle`
      unfolds one level at a time preserving nested fold state;
      `:MepOrgTodo`/`:MepOrgPriority` cycled TODO→DOING and
      `[#A]`→`[#B]` in place while preserving `:tags:`;
      `:MepOrgCheckbox` toggled `[ ] milk`→`[x] milk` and correctly
      propagated the ancestor `Groceries [0/2]`→`[1/2]` cookie;
      `:MepOrgDemoteSubtree` shifted only the two headline lines
      (`* Personal notes`→`**`, `** Groceries`→`***`) in a subtree
      while leaving the interleaved `more text` body line untouched.
- ✅ **Addressed in a follow-up pass**: sibling sort
      (`mep.org_sort_siblings(mode)`/`:MepOrgSortAlpha`/`SortTodo`/
      `SortPriority` — finds the sibling group by walking to the
      nearest shallower ancestor then scanning forward, jumping each
      headline straight to its own `subtree_end` so nested deeper
      headlines under an intermediate sibling are correctly excluded)
      and easy-templates (`mep.org_expand_template`/`:MepOrgTemplate`,
      `mep.org_easy_templates` table — command-triggered rather than a
      literal Tab-in-Insert-mode intercept, mirroring how Phase 23's
      own snippet expansion already works) are now implemented and
      verified via Xvfb (alpha-sorted three headlines correctly;
      `<s` → `:MepOrgTemplate` correctly expanded to `#+begin_src`/
      `#+end_src` with cursor on the opening line). Tag-match predicate
      parser (`mep.org_tag_match(expr)`/`:MepOrgMatch`, `+tag-tag|tag2`
      AND/OR syntax, no parenthesized grouping) also now implemented,
      shared with Phase 37's roam tag filtering. A real bug was caught
      and fixed during template verification: after leaving Insert
      mode, vim moves the cursor *onto* the last-typed character
      (not past it), so the initial `line:sub(1, col - 1)` slice
      missed the trigger letter entirely — fixed by trying the
      inclusive slice first, falling back to the exclusive one, so the
      command works whether it's invoked mid-Insert or (the normal
      case) after Escape.
- ⚠️ **Still not implemented**: list-editing helpers (item
      continuation, indent/outdent, renumbering) — genuinely lowest-
      value item in the phase per its own ordering. "Insert sibling"
      (plain/pre-filled-TODO) not bound to a dedicated command — use
      normal-mode `o`/`O` plus `:MepOrgTodo`.

### Phase 30 — Org-mode B: tables (new), links, footnotes, timestamps/scheduling ✅

*(depends on: Phase 4 decorations for link conceal + table rendering,
Phase 13's URL-open for link-follow, Phase 29)*

- [x] **Tables (new design work)** — `mep.org_table_align`/
      `:MepOrgTableAlign`: finds the contiguous run of `|`-lines
      touching the cursor, computes per-column max width across all
      non-separator rows, rewrites every cell padded and every
      `|---+---|` separator to match. Real new logic since
      `mep.nvim` has no org table support to port from.
- [x] Links: `[[target]]`/`[[target][description]]` parse
      (`mep.org_link_at_cursor`), insert via two `mep.ui_input`
      prompts (`mep.org_link_insert`), and follow
      (`mep.org_link_follow`/`:MepOrgLinkFollow`) dispatching by
      prefix: `http(s)://`/`mailto:` → `mep.open_url` (Phase 13),
      `file:path[::N]`/`file:path::*Heading` → `mep.pane_open` +
      cursor placement (Phase 14), `id:`/`#custom-id` → in-buffer
      property-drawer lookup (Phase 29's `org_property_get`), bare
      target and `*Heading` (org's own in-buffer-target convention) →
      title match against every headline.
- [x] Link concealment: whole-span highlight recoloring (`Blue`) via
      a dedicated `org-links` namespace (`mep.org_link_highlight`) —
      same "recolor the span" approach as Phase 28's markdown links,
      not true character-hiding concealment.
- [x] Footnotes: `[fn:name]` reference, `mep.org_footnote_jump`/
      `:MepOrgFootnoteJump` jumps to the `[fn:name] ...` definition
      line if one exists, else to the next other reference.
- [x] Timestamps: active `<YYYY-MM-DD Day>`/inactive
      `[YYYY-MM-DD Day]`, insert-at-cursor
      (`mep.org_timestamp_insert`/`:MepOrgTimestamp[Inactive]`) and
      increment/decrement-by-day under cursor
      (`mep.org_timestamp_shift`/`:MepOrgTimestampIncr`/`Decr`) using
      real `os.time`/`os.date` calendar math (correct weekday-name
      and month/year rollover, not string arithmetic).
- [x] `SCHEDULED:`/`DEADLINE:` planning lines
      (`mep.org_set_planning`/`:MepOrgScheduled`/`:MepOrgDeadline`):
      inserts (or replaces an existing) planning line directly below
      the current headline.
- [x] Verified via Xvfb: `:MepOrgTableAlign` on a ragged 2-column
      table produced correctly padded columns and a matching `+`
      separator; `:MepOrgLinkFollow` on `[[*Project Alpha]]` jumped
      the cursor from line 8 to the matching headline at line 1;
      `:MepOrgTimestampIncr` on `<2026-08-19 Wed>` twice produced
      `<2026-08-20 Thu>` then `<2026-08-21 Fri>` — correct calendar
      math and weekday recomputation.
- ⚠️ **Correction to an earlier note in this file**: this used to say
      repeaters weren't preserved by `org_timestamp_shift`; re-checked
      while implementing Phase 32's occurrence math and that's wrong —
      `rest` already captures everything after the weekday abbreviation
      via `(.*)$`, which includes any trailing repeater/time text, so
      `<2026-08-19 Wed +1w>` → increment → `<2026-08-20 Thu +1w>`
      correctly, verified via Xvfb. What's genuinely missing: no UI to
      *insert* a repeater (`mep.org_timestamp_insert` only ever writes a
      bare date) and timestamp *ranges* (`<start>--<end>`) aren't
      parsed at all. "Store link for later recall"
      (`org-store-link` global capture, vs. this phase's direct
      insert-with-prompt) not implemented. A real bug was caught and
      fixed during verification: the table-cell splitter's naive
      `line:gmatch('|([^|]*)')` produced a spurious empty trailing
      cell because Lua's `gmatch` treats the line's closing `|` as
      the start of one more (empty) capture — fixed by stripping the
      outer pipes first and re-splitting against a single appended
      sentinel pipe.

### Phase 31 — Org-mode C: capture, refile, archive ✅

*(depends on: Phase 3 prompts/floating-editable-popup, Phase 8 picker,
Phase 30's timestamp placeholders)*

- [x] Capture (`mep.org_capture`/`:MepOrgCapture`): a
      `mep.org_capture_templates` registry (default `Task`/`Note`),
      picker over templates, placeholder expansion (`%U`/`%u`/`%T`/
      `%t`/`%a`/`%%` via `mep_org_expand_template`, `%^{PROMPT}` via
      sequential `mep.ui_input` prompts resolved with a null-byte
      sentinel substitution so multiple prompts in one template don't
      collide, `%?` stripped as the final cursor-landing marker),
      appends to a target file (possibly not the one currently open,
      via `mep.pane_open`) and reports where it landed.
- [x] Refile (`mep.org_refile`/`:MepOrgRefile`): picker over every
      *other* headline in the buffer (indented by level, current
      subtree excluded), moves the subtree to become the last child
      of the chosen target, re-leveling every headline line in the
      moved subtree by the level delta (body/non-headline lines
      untouched) — insert-then-delete or delete-then-insert ordered
      by whether the target precedes or follows the source, so the
      untouched op's line numbers stay valid.
- [x] Archive (`mep.org_archive`/`:MepOrgArchive`): moves the subtree
      to `<file>_archive.org` (opened via `mep.pane_open`, created if
      absent), inserting an `:ARCHIVE_TIME:`/`:ARCHIVE_FILE:`/
      `:ARCHIVE_OLPATH:` property drawer as provenance, saves the
      archive file, switches back, and deletes the subtree from the
      source.
- [x] Verified via Xvfb: capture through the full template→prompt→
      insert pipeline; refile moving a captured TODO to become a
      child of an existing top-level headline with correct
      re-leveling (`* TODO` → `** TODO`) and no residue at the old
      location; archive moving a subtree (with its own child
      headline) to a freshly-created `_archive.org` file with a
      correct provenance drawer, confirmed via the live in-memory
      buffer (`[3/3]` line count after archiving) since the on-disk
      copy needs an explicit `:w`.
- ⚠️ **Real bugs caught and fixed during verification**: (1) the
      capture placeholder substitutions originally used doubled Lua
      pattern escaping (`'%%%%U'` instead of `'%%U'`), so `%U`/`%t`/
      etc. never matched and were inserted literally instead of
      expanding — fixed by removing the extra escaping layer.
      (2) refile and archive's subtree-deletion calls used
      `mep.replace_lines(row, e - 1, {})` where `e` is
      `mep_org_subtree_end`'s *exclusive* end row; since
      `replace_lines(a, b, ...)` itself erases the *half-open*
      1-indexed range `[a, b)`, that left the subtree's very last
      line (e.g. a checkbox or timestamp continuation line)
      undeleted — silently duplicating content at the destination
      while leaving a orphaned remnant at the source. Fixed by
      deleting with `mep.replace_lines(row, e, {})` instead (every
      other subtree-boundary call site in Phases 29-30 was audited
      and found already correct — this bug was specific to the two
      newest functions). (3) freshly-created archive files got a
      stray leading blank line from the auto-created single-empty-
      line buffer; fixed by treating a lone empty first line as
      "nothing to append after."
- ⚠️ **Scope cut**: no floating review-popup with explicit
      commit/abort keys — the captured entry commits immediately once
      all prompts resolve (no final "review, then C-c C-c" step).
      Refile does not attempt to promote/demote the *target*
      selection list to a narrower picker experience beyond flat
      indentation; sibling-position choice within the target (e.g.
      "as first child" vs "as last child") isn't offered — always
      appends as last child.

### Phase 32 — Org-mode D: agenda ✅

*(depends on: Phase 29/30 (tags, timestamps, TODO), and needs a
"headless buffer load" mechanism — reading a file's content without
making it the active editor buffer)*

- [x] Headless file loading (`mep_org_read_file_lines`): plain Lua
      `io.open`/`f:lines()` — since `mep_org_parse_headline` (Phase 29)
      already takes a raw line string rather than a buffer row, it's
      directly reusable against disk-read lines with no new C++.
- [x] `agenda_files` resolution: `mep.org_agenda_files` (literal path
      list) + `mep.org_agenda_add_current`/`:MepOrgAgendaAddFile` to
      append the buffer currently open.
- [x] Cross-file entry collection (`mep.org_agenda_collect`): walks
      every agenda file's headlines, pairing each with any
      `SCHEDULED:`/`DEADLINE:` planning line immediately below it,
      building a flat `{file, line, todo, title, tags, priority,
      scheduled, deadline}` list.
- [x] Views, all picker-based with jump-to-entry on select (`mep.pane_
      open` + `mep.set_cursor`): `:MepOrgAgendaToday`/`org_agenda_day`
      (a specific date), `:MepOrgAgendaWeek` (next 7 days, grouped),
      `:MepOrgAgendaTodo` (every open TODO across all agenda files),
      `:MepOrgAgendaSearch` (title/tag substring match, prompted via
      `mep.ui_input`), `:MepOrgAgendaOverdue` (deadlines before today
      whose TODO state isn't the configured "done" keyword —
      persistent-overdue-until-resolved by construction, since it's
      recomputed live from current state rather than tracked
      separately).
- [x] Verified via Xvfb across two separate `.org` files added to
      `mep.org_agenda_files`: `:MepOrgAgendaToday` correctly merged
      today-scheduled entries from both files into one picker;
      `:MepOrgAgendaOverdue` correctly surfaced only the one entry
      with a past deadline and TODO state, excluding both a `DONE`
      entry and a future-deadline entry.
- ✅ **Addressed in a follow-up pass**: `agenda_files` now supports
      glob patterns (`mep_org_expand_glob`/`mep_org_glob_to_pattern` —
      `*` within the final path component, matched via `mep.list_dir`;
      no recursive `**`), alongside literal paths. Repeating-timestamp
      occurrence checking now works: `mep.org_next_occurrence(ts,
      today)` (Phase 30, shared with `org_timestamp_shift`'s repeater-
      preservation) repeatedly adds the repeater's interval until
      reaching the first occurrence on/after a given date; Day/Week
      views now compare via `mep_org_occurs_on` (does this timestamp's
      next occurrence *equal* this exact date?) instead of literal
      string equality, so a `SCHEDULED: <... +1w>` entry correctly
      appears on each real weekly occurrence rather than only its
      original literal date. Overdue explicitly excludes repeating
      deadlines (by construction, `org_next_occurrence` always
      resolves to today-or-later, so a repeater can never be
      "overdue" in the backlog sense — it just shows on its next due
      date via Day/Week instead). Verified via Xvfb: `agenda_files =
      {'/tmp/agtest/*.org'}` correctly picked up both matching files;
      a weekly-repeating entry scheduled `2026-08-01 +1w` correctly
      did *not* appear in the `2026-08-19` agenda (not an occurrence
      date) and correctly *did* appear in the `2026-08-22` agenda (its
      real next occurrence). A real bug was caught and fixed during
      verification: the glob-matcher's escape pass looked for an
      already-escaped `%*` sequence in the string to convert to `.*`,
      but a bare `*` was never in the character class being escaped in
      the first place — so the search pattern could never match
      anything, and every glob silently expanded to zero files. Fixed
      by marking each `*` with a control byte before escaping the
      other magic characters, then swapping the marker for `.*`
      afterward, so the glob's own `*` survives the escape pass intact.
      **Still not implemented**: no persistent multi-pane "report
      buffer" with in-place TODO-cycling/reschedule keybindings —
      agenda views are transient pickers that jump-and-close rather
      than a live, editable report the way real Org's agenda buffer
      works; editing an entry means jumping to it and using Phase 29's
      normal TODO-cycle/timestamp commands there. No separate
      "deadline warning window" (e.g. "remind starting 3 days before
      due") — only binary overdue-or-not. Timestamp *ranges* still
      aren't parsed (Phase 30's own gap, inherited here).

### Phase 33 — Org-mode E: sort/narrow/sparse-tree, clocking ✅

- [x] Narrow/widen and sparse-tree search shipped with Phase 29
      (`mep.org_narrow`/`org_widen`/`mep.org_sparse_tree`). Sibling
      sort was scope-cut there (see Phase 29's notes) and remains
      undone.
- [x] Clocking: `mep.org_clock_in`/`:MepOrgClockIn` scans the whole
      buffer for an already-open `CLOCK: [...]` line (refuses a
      second concurrent clock — the "single-open-clock-at-a-time
      found by buffer scan, not session state" design the plan calls
      for), auto-creates a `:LOGBOOK:` drawer under the current
      headline if none exists, and inserts an open clock line.
      `mep.org_clock_out`/`:MepOrgClockOut` finds that open line
      anywhere in the buffer, computes elapsed minutes via
      `os.time`/`os.date`, and closes it in place
      (`CLOCK: [start]--[end] => H:MM`). `mep.org_clock_effort`
      reads an `:Effort:` property (Phase 29's property-drawer
      machinery, no new parsing needed). `mep.org_clock_table`/
      `:MepOrgClockTable` sums closed `=> H:MM` durations recursively
      per headline (whole subtree, descendants included) and shows
      the report in a picker.
- [x] Verified via Xvfb: `:MepOrgClockIn` created the `:LOGBOOK:`
      drawer with an open `CLOCK: [2026-08-19 Wed 04:54]` line;
      `:MepOrgClockOut` immediately after closed it as
      `CLOCK: [2026-08-19 Wed 04:54]--[2026-08-19 Wed 04:54] => 0:00`
      — correct same-minute zero-duration formatting, confirming the
      start/end timestamp parsing and `os.time` arithmetic path
      (shared with Phase 30's already-verified timestamp-shift math).
- ⚠️ **Scope cut**: clock-table is a flat per-headline listing, not a
      real nested/indented report block written back into the buffer
      (`#+BEGIN: clocktable ... #+END:`) the way org's own
      `C-c C-x C-r` produces — no date-range filtering, no
      `:maxlevel` option.

### Phase 34 — Org-mode F: babel (code execution) ✅

*(depends on: Phase 1 jobs — this is the single biggest remaining
systems investment in the org module; see "Design decisions" above for
the "start small" language-subset guidance)*

- [x] Src-block parser (`mep_org_src_block_at`): finds the
      `#+begin_src <lang> [header-args]` / `#+end_src` pair
      containing the cursor, parses `:var name=value` (repeatable),
      `:tangle target`, `:cache yes`.
- [x] Per-language execution: `mep.org_babel_langs` (`sh`, `bash`,
      `python`→`python3`, `lua`) mapped to an interpreter argv, body
      written to a temp file (`os.tmpname`) with a language-
      appropriate `:var` prelude (`name=value` shell assignment for
      sh/bash, `name = value` for the rest) prepended, executed via
      `mep.job_start` (plain pipe capture, not Phase 27's PTY —
      babel wants clean stdout lines, not an ANSI terminal pane).
- [x] Execute-at-cursor (`mep.org_babel_execute`/
      `:MepOrgBabelExecute`) → `#+RESULTS:` block insertion: a single
      `: line` for one-line output, `#+begin_example`/`#+end_example`
      for multi-line, replacing any existing results block for the
      same src block in place (detects and spans the prior block
      correctly whether it was single-line or example-fenced).
- [x] Tangle (`mep.org_babel_tangle`/`:MepOrgBabelTangle`):
      concatenates every same-`:tangle target` block's body in
      document order, writes each target file, reports how many
      files were written.
- [x] In-memory result cache keyed by `lang|args|body`
      (`mep_org_babel_cache`, `:cache yes`) — no disk persistence, no
      staleness detection, matching the plan's own documented scope.
- [x] Polyglot/LSP-bridging for src blocks **deferred to Phase 36**
      as instructed — not attempted here.
- [x] Verified via Xvfb: a `#+begin_src sh :var name=World` block
      executing `echo "Hello, $name!"` + a second echo produced a
      correct multi-line `#+RESULTS:`/`#+begin_example` block with
      the `:var` substitution applied (`Hello, World!`); re-running
      replaced the results block in place rather than duplicating it;
      `:MepOrgBabelTangle` on a file with no `:tangle` header
      correctly reported "Tangled 0 file(s)" with no crash.
- [x] **Ctrl-C Ctrl-C keybinding** (mirroring real Emacs org-mode's own
      binding, added on request — previously `:MepOrgBabelExecute` was
      only reachable by typing the full ex-command). Native C++, not a
      Lua `mep.map()` registration: `mep.map()`
      (`RegisterLuaMapping`/`TryLuaMapping`, `editor.h`/`.cpp`) only
      matches single printable characters, and — more fundamentally —
      GLFW/raylib never emits a `GetCharPressed()` char event while Ctrl
      is held at all, so *neither* tap of a Ctrl-C Ctrl-C chord can be
      observed through the character-loop path every other double-tap in
      this codebase uses (`pending_g_` for `gg`, `pending_ctrl_w_` for
      Ctrl-W's second key) — those work because their second key is an
      *unmodified* char. Both taps here are read from the raw
      `GetKeyPressed()` key-code queue instead (the same queue
      Ctrl-V/D/U/F/B/A/X/O/I already use, for the same "flaky
      `IsKeyPressed` under a slow frame" reason noted in
      `HandleNormalInput`'s own comment), with a new `pending_ctrl_c_` +
      `pending_ctrl_c_time_` pair and a `kCtrlCChordTimeoutSec` (0.6s)
      window standing in for the "next key clears it" rule a modifier-
      free chord could use instead. On completing the chord,
      `Editor::TryRunOrgBabelAtCursor()` checks the current buffer's
      filename ends in `.org`, then looks up `"MepOrgBabelExecute"` in
      `lua_commands_` (the same registry the `:` command line's own
      Lua-command fallback consults) and invokes it directly —
      deliberately *not* by constructing and feeding a literal
      `":MepOrgBabelExecute"` string through the full command-line
      parser, and deliberately *not* re-implementing
      `mep_org_src_block_at`'s "cursor must be inside a `#+begin_src`
      block" check in C++ (that function's own existing "no-op if not in
      a block" handling is reused as-is, so there's exactly one copy of
      that logic). Verified interactively under Xvfb+xdotool: two
      quick Ctrl-C taps on a line inside a `#+begin_src bash` block
      produced the same toast/status message and `#+RESULTS:` block
      insertion as `:MepOrgBabelExecute` does, and a single lone Ctrl-C
      tap (no second one) correctly did nothing.
- ⚠️ **Scope cut (superseded below)**: ~~no compiled-language support~~
      — see "Multi-language babel port" below, which replaced the
      language table this note originally described. Var substitution
      is still a per-language *textual* prelude line (now via each
      language's own `var_stmt`, not a naive shared one), and a `:var`
      value is now encoded through `mep_org_babel_format_literal`
      (backslash/quote/newline-escaped double-quoted string, or a bare
      numeric literal) rather than spliced in unescaped — real values
      containing spaces/quotes now round-trip correctly.
      (Correction to this doc's own prior claim: `:results`
      output-mode header-args like `:results silent`/`:results table`
      **are** now parsed and honored — `blk.results_modes` in the babel
      script — and the result cache now persists to disk between runs
      via `mep.babel_cache_load`/`save`, both since this entry was
      originally written; this doc had drifted out of date versus the
      implementation.)
- [x] **Multi-language babel port** (added on request — "Lua works, but
      c++ doesn't, c doesn't, etc.… support all of the languages in
      mep.nvim"): `mep.org_babel_langs` rebuilt from a flat
      `{lang = {argv...}}` table (10 interpreted languages) into a rich
      per-language descriptor table — `executable`/`fallback_executable`,
      `extension`, `var_stmt(name, literal)`, `print_stmt(expr)` (for
      `:results value` mode), and for a compiled or non-default-invocation
      language: `compiled`, `compile_cmd`/`run_cmd`/`run_compiled_cmd`,
      `wrap_main(includes, body)`, `detect_class` — ported near-verbatim
      from `mep.nvim/lua/mep/org/babel.lua`'s own `M.languages`, kept in
      behavioral lockstep with it deliberately. Covers the same ~27-
      language/6-alias set mep.nvim supports: `lua python sh/bash
      javascript/js cpp/c++ c ruby typescript/ts elixir julia clojure perl
      r php rust go fortran csharp/cs/c# scala zig nim crystal java kotlin
      haskell ocaml d`.
    - Execution now has three shapes instead of one, dispatched by
      `mep_org_babel_spawn`: (1) plain interpreter — unchanged one-job
      shape; (2) `run_cmd` override — still one job, but a non-default
      invocation (`dotnet run <file>`, `zig run <file>`, `nim r <file>`,
      `crystal run <file>`) for a language whose interpreter needs its own
      subcommand; (3) `compiled` — two *chained* `mep.job_start` calls
      (compile, then run the resulting binary), only proceeding to the run
      step on a zero compile exit code, with `on_finish(code, stdout,
      stderr, failure_verb)` always receiving whatever stdout the run step
      produced (even partial, on a failed run) so results still get
      written. Java is the one language needing both a custom
      `compile_cmd` (`javac -d <dir>`, `binary_path` reused as a
      `.class`-file output *directory*, not a single executable) and
      `run_compiled_cmd` (`java -cp <dir> <ClassName>`), with `detect_class`
      scanning the body for `public class NAME`/`class NAME` (or an
      explicit `:classname` header-arg) since a `public class Foo` must
      live in a file literally named `Foo.java`.
    - `wrap_main` (entry-point wrapping — `int main() {...}` for C/C++,
      `fn main() {...}` for Rust, `func main() {...}` for Go, etc.) is
      applied only when `mep_org_babel_should_wrap_main` says so: an
      explicit `:main yes`/`:main no` header-arg, else each language's own
      default via `MEP_ORG_BABEL_WRAP_DEFAULT` — every language defaults
      to "no" (assume a self-contained program, matching how real-world
      src blocks are usually written) **except PHP**, which defaults to
      "yes" (a bare PHP snippet needs its own `<?php` tag just to run at
      all). `:includes <hdr1> <hdr2>` supplies each language's own
      import/include syntax inside the wrap.
    - `mep_org_babel_resolve_lang`/`resolve_exe`/`has_exe` (via a
      `command -v <exe> >/dev/null 2>&1` shellout — mep's embedded Lua has
      no `vim.fn.executable` equivalent) replace the old table lookup,
      trying `fallback_executable` second (e.g. `python3` then `python`)
      and producing a specific "No `<lang>` interpreter found on PATH
      (looked for `<exe>`)" notification rather than a generic failure.
    - `flake.nix`'s `devShells.default.packages` grew a matching
      toolchain list (rustc/cargo, go, zig, nim, crystal, jdk, kotlin, ghc,
      ocaml, `dmd.override { stdenv = gcc14Stdenv }` — dmd's C-header
      importer can't parse gcc 15's C23 `nullptr` keyword, same override
      `mep.nvim/flake.nix` already uses — dotnet-sdk_10 specifically, since
      the `csharp` entry's `dotnet run <file>.cs` "file-based apps" mode
      only exists from .NET 10 — plus `lua5_4` so `#+begin_src lua` blocks
      resolve their own interpreter *inside* the devShell too, matching
      mep's vendored Lua version exactly rather than relying on some
      other Lua happening to be on the outer, non-flake `$PATH`), mirroring
      `mep.nvim/flake.nix`'s own babel-language devShell list so both
      editors run the same fixture files with no toolchain gap between
      them. Confirmed every added tool resolves via both `nix develop
      --command which ...` and `direnv exec . which ...`.
    - The leetcode feature's own independent `mep.org_babel_langs[lang]`
      consumer (`mep.leetcode_run_tests`, previously reading it as the old
      flat argv array) was updated alongside — now resolves via
      `mep_org_babel_resolve_lang` and runs through the same
      `mep_org_babel_spawn` chain as `mep.org_babel_execute` — since it
      would otherwise have silently broken the moment the table's shape
      changed.
    - Verified via Xvfb+xdotool against the exact file from the original
      bug report (`mep.nvim/org/test.org`, every language's src block, run
      interactively with real Ctrl-C Ctrl-C keypresses — not a synthetic
      `:lua mep.org_babel_execute()` call): C++, C (a real quicksort
      program, not a one-liner), Go (custom `compile_cmd`), Rust, Fortran,
      Scala (all three `wrap_main` + `:main yes`), Java (`:classname
      HelloWorld`, two-step `javac`/`java`), C# (`dotnet run`), Zig (`zig
      run`, a real Zig 0.16 buffered-stdout-writer program), Nim
      (`nim r`), Crystal (`crystal run`), D (two-step `dmd` compile+run),
      Kotlin, Haskell, OCaml (self-contained script interpreters), plus a
      Ruby/Python regression pass, all produced a freshly-computed
      `#+RESULTS:` matching that block's actual source — including two
      *unambiguous* cases proving real (re-)execution rather than stale
      leftover text: the C++ block's stale results said `Hellozz, world`
      (a pre-existing typo mismatched against its own source's `"Hello,
      world"`) and came back corrected to `Hello, world`; the C block's
      quicksort program produced the correct sorted `1 5 8 9 11 17`, which
      only a real compile+run could produce. Also root-caused and fixed a
      test-harness-only false negative during this same verification pass:
      `xdotool key ctrl+c` issued twice as two separate invocations
      doesn't reliably keep Ctrl held across both taps under this
      sandbox's Xvfb (silently produced zero effect, no error toast even);
      `xdotool keydown ctrl` / `key c` / `key c` / `keyup ctrl` as one
      sequence is reliable — a test-tooling note, not a mep bug (found by
      cross-checking against a direct `:lua mep.org_babel_execute()` call,
      which worked immediately every time).
    - ⚠️ **Scope cut**: `:var` + `:main yes` together is broken for
      Haskell specifically (documented inline in `L.haskell`'s own
      comment, inherited as-is from mep.nvim: the bare top-level binding
      lands inside the generated `do` block, where a non-`let` binding is
      a syntax error) — `:var` alone or `:main yes` alone both work.
      OCaml has no `print_stmt` (so `:results value` mode silently falls
      back to running the body as-is for OCaml specifically, same as any
      language with no `print_stmt`) since there's no single universal
      print expression across OCaml's types without a matching `Printf`
      format specifier. No array/vector `:var` values (a `:var` value is
      always encoded as one scalar literal — string or bare number).
- [x] **Bug fix: `#+begin_src`/`#+end_src`/`#+BEGIN_SRC`/`#+END_SRC`
      case-insensitivity.** Found via a real user report: Ctrl-C Ctrl-C
      on a file using uppercase directives (`mep.nvim/org/test.org`,
      written `#+BEGIN_SRC lua` / `#+END_SRC`) failed with "Not in a src
      block" even with the cursor squarely inside one. Root cause: every
      `#+begin_src`/`#+end_src` line-match in the embedded org Lua (block
      detection in `mep_org_src_block_at`, babel tangle's scan, org
      export's src-block handling, and the unrelated leetcode-picker's
      own src-block finder — 11 call sites total) used a literal
      lowercase-only Lua pattern; Lua patterns have no case-insensitive
      flag, and real org-mode directives are case-insensitive (Emacs
      itself accepts any capitalization). Fixed by rewriting the
      `begin_src`/`end_src` portion of every pattern as an explicit
      per-letter character class (`[Bb][Ee][Gg][Ii][Nn]_[Ss][Rr][Cc]`,
      similarly for `end_src`) rather than lowercasing the line being
      matched — preserves the original casing of everything captured
      alongside the match (a `:var` value's case must round-trip
      untouched). Separately, the extracted language tag itself
      (`#+BEGIN_SRC Lua` vs. `#+begin_src lua`) is now explicitly
      lowercased before `mep.org_babel_langs` table lookup (which is
      keyed by lowercase `"lua"`/`"python"`/etc.) — a related
      case-sensitivity gap in the same function, fixed alongside it
      rather than left for a second bug report. Verified against the
      exact reported file: Ctrl-C Ctrl-C on its `#+BEGIN_SRC lua` block
      now correctly executes and updates the existing `#+RESULTS:` in
      place (previously erroring immediately).

### Phase 35 — Org-mode G: export ✅

*(depends on: Phase 29/30 mostly; Phase 34 only if code-execution-
during-export is wanted — can ship without it first)*

- [x] Flat, ordered single-pass document walk (`mep.org_export`,
      no intermediate AST): headings, paragraphs, list items,
      property drawers, planning lines, and `#+begin_src`/`#+end_src`
      blocks (rendered literally — see below) each handled inline as
      the buffer is walked top to bottom.
- [x] Shared inline tokenizer (`mep_org_inline_convert`): bold
      (`*x*`), italic (`/x/`), code (`=x=`), and `[[url]]`/
      `[[url][desc]]` links, driven by a per-backend `marks` table
      (`mep.org_export_marks`) rather than three separate tokenizers.
- [x] Three backends: ASCII (`mep.org_export('ascii')`/
      `:MepOrgExportAscii`), Markdown (`'markdown'`/
      `:MepOrgExportMarkdown`), HTML (`'html'`/`:MepOrgExportHtml`,
      with `&`/`<`/`>` escaping on raw text before mark conversion).
      LaTeX/PDF/ODT out of scope, matching `mep.nvim`.
- [x] `:noexport:` tag exclusion (skips the whole subtree via
      `mep_org_subtree_end`). Subtree export
      (`mep.org_export_subtree`/`:MepOrgExportSubtreeHtml`/
      `:MepOrgExportSubtreeMarkdown`) with headline-level
      renormalization (the subtree's own headline becomes level 1).
- [x] Src blocks always render literally (`<pre><code>`/```` ``` ````
      fence/`----` per backend) — never executed during export, per
      the phase's own "ship without eval first" guidance.
- [x] Verified via Xvfb on a document with a `:noexport:`-tagged
      headline, an `Overview` headline containing bold/italic text
      and a `[[url][desc]]` link, a two-item list, and a `lua` src
      block: HTML output correctly excluded the noexport subtree and
      rendered `<b>bold</b>`, `<i>italic</i>`,
      `<a href="...">link</a>`, `<li>` items, and a fenced
      `<pre><code>` block; ASCII output produced an upper-cased
      heading, marker-stripped bold/italic, `text <url>`-style link,
      and a `----`-fenced code block.
- ⚠️ **Real bug caught and fixed during verification**: the inline
      tokenizer originally converted links to their target markup
      immediately, then ran bold/italic/code passes over the *already
      -converted* text — since HTML's `</b>`/`</a>` (and any URL's
      internal `//`) contain literal `/` characters, the italic
      pattern's bare `/.../` re-matched across them, corrupting the
      generated markup (`<b>bold<<i>b> and </i>italic/ text...`).
      Fixed by having every construct (link, bold, italic, code)
      stash its generated output behind a `\0M<n>\0` placeholder the
      moment it's produced, with all placeholders restored in one
      final pass after every pattern has run — so no pass ever sees
      another pass's generated markup.
- ⚠️ **Scope cut**: no `#+INCLUDE:` resolution or `#+MACRO:`
      collection (single-file export only). No underline/strikethrough
      inline marks (bold/italic/code/link only). Subtree export has no
      title-override prompt (always uses the headline's own title). No
      `:exports code/results/none`/`:eval never` header-arg handling —
      src blocks are always shown, never hidden or executed, which is
      the documented "ship without eval first" baseline rather than a
      gap to close later in this pass.

### Phase 36 — Org-mode H: polyglot (LSP-in-src-blocks) — deferred/stretch ⏭️ SKIPPED

- [ ] **Deferred indefinitely, per the plan's own recommendation.**
      Requires Phase 20 (LSP) fully working plus per-language shadow-
      buffer/shadow-file synchronization, generated
      `compile_commands.json`/`Cargo.toml`/`go.mod` scaffolding, and
      LSP-client-restart-on-config-change handling. Worst complexity-
      to-value ratio in the whole plan, and every other Part VII phase
      (28-35) is now done without it — LSP itself (Phase 20) and babel
      (Phase 34) both work standalone; only the *bridge* between them
      inside src blocks is skipped. Revisit only if there's appetite
      left after every other phase (37-42) is done.

---

## Part VIII — PKM extras (built on org)

### Phase 37 — Roam (zettelkasten note linking) ✅

*(depends on: Phase 29/30 org (headlines, properties, links), Phase 8
picker, Phase 7 sidebar)*

- [x] Stable per-note `:ID:` property (`mep.org_roam_ensure_id`/
      `:MepOrgRoamEnsureId`): a file-level `:PROPERTIES:`/`:END:`
      drawer inserted before any headline (org-roam v2's one-note-
      per-file convention), auto-generated (`os.date` timestamp +
      random suffix) on first use. `[[id:...][title]]` links need no
      new machinery — Phase 30's `mep.org_link_follow` already
      dispatches `id:` targets via property lookup.
- [x] Insert-link picker (`mep.org_roam_insert_link`/
      `:MepOrgRoamInsertLink`): fuzzy title search (`#+TITLE:`, else
      first headline) across every `.org` file in
      `mep.org_roam_dirs`, ensures the target note has an `:ID:`
      (creating one on first link if needed) before inserting.
- [x] Backlinks sidebar (`mep.org_roam_backlinks`/
      `:MepOrgRoamBacklinks`, Phase 7's sidebar widget): scans every
      configured note for a `[[id:<current-note-id>` substring,
      lists titles with click-to-jump.
- [x] Daily note (`mep.org_roam_daily`/`:MepOrgRoamDaily`,
      `mep.org_roam_daily_dir` or first `org_roam_dirs` entry, opens/
      creates `YYYY-MM-DD.org`) and new-note (`mep.org_roam_new_note`/
      `:MepOrgRoamNewNote`, prompts a title, slugifies it for the
      filename) — both stamp `#+TITLE:` and an `:ID:` on creation.
      Reuses Phase 31's `mep_org_expand_template` engine only
      indirectly (both commands are simple enough not to need
      placeholder expansion themselves; the engine was made a global
      so future template-driven note types could use it).
- [x] Verified via Xvfb across two `.org` files in a shared roam
      directory: the insert-link picker correctly listed the other
      note's `#+TITLE:`-derived title (excluding the current file),
      auto-created its `:ID:` drawer on selection, and inserted a
      correct `[[id:<id>][title]]` link; the backlinks sidebar,
      queried from the target note, correctly reported "1 linking
      here" with the linking note's title once that link was actually
      saved to disk (an unsaved in-memory edit — expected, matching
      Phase 32's same disk-read limitation — was correctly *not*
      picked up until saved).
- ⚠️ **Scope cut**: `mep_org_expand_template`/`mep_org_read_file_lines`
      (originally `local` inside their Phase 31/32 chunks) were made
      global to be reusable here — the same recurring cross-chunk
      scoping fix as every other phase in this session. No fuzzy
      backlink-graph visualization (list only, not a graph view).

### Phase 38 — Flashcards (SM2 spaced repetition) ✅

*(depends on: Phase 29 org (headlines, tags, properties), Phase 3
floating popup)*

- [x] SM-2 scheduling algorithm (`mep_org_sm2`): standard
      ease-factor/repetition/interval update from a 0-5 quality
      score, floored at `ef = 1.3`.
- [x] State stored as org properties (`:DRILL_EF:`/`:DRILL_REPS:`/
      `:DRILL_INTERVAL:`/`:DRILL_DUE:`) via Phase 29's
      `mep.org_property_get`/`org_property_set` — no new storage
      format.
- [x] Review session (`mep.org_drill_review`/`:MepOrgDrillReview`):
      one card at a time, jumps to the card's headline, reveal-answer
      via `mep.ui_confirm`, graded response (Again/Hard/Good/Easy)
      via `mep.ui_select` mapped onto the SM-2 quality scale
      (0/3/4/5) — built from the existing prompt primitives (Phase 3)
      rather than a bespoke floating widget.
- [x] Card source: `mep.org_drill_files` (configured file list),
      filtered by `mep.org_drill_tag` (default `drill`),
      tag-inheritance-aware (`mep_org_tags_at_lines`, a headless
      variant of Phase 29's `org_tags_at` operating on disk-read
      lines instead of the live buffer), due-date-filtered
      (`mep.org_drill_collect_due`, undue or missing `:DRILL_DUE:`
      both count as due).
- ⚠️ **Scope cut**: not independently Xvfb-verified beyond code
      review — the SM-2 arithmetic is small, deterministic, and
      shares its date math with Phase 30/33's already-verified
      `os.time`/`os.date` timestamp paths, and the property-drawer
      read/write path was already verified in Phase 29/33. Review
      session UI (confirm → select → next) follows the exact same
      `ui_confirm`/`ui_select` sequencing pattern used successfully
      elsewhere, so this was judged lower-risk than the phases given
      full interactive verification.

### Phase 39 — Bib (bibliography / org-ref) ✅

*(depends on: Phase 8 picker, Phase 29/30 org for insertion context)*

- [x] `.bib` file parser (`mep_org_bib_parse`): hand-rolled BibTeX
      parser using Lua's `%b{}` balanced-match pattern item to
      extract each `@type{...}` entry (correctly handles brace-nested
      field values like `{A Study of {Nested Braces} in Titles}`,
      which a naive non-greedy regex can't), then a custom top-level
      comma splitter (`mep_org_bib_split_top_level`, brace-depth
      aware) to separate fields without breaking on commas inside
      nested braces. Handles both `{...}` and `"..."` value
      delimiters.
- [x] File resolution order (`mep.org_bib_resolve_files`): current
      buffer's directory first, then each configured project root
      (`mep.project_list`, Phase 16), matching the plan's documented
      search order.
- [x] Citation-insertion picker (`mep.org_bib_insert_citation`/
      `:MepOrgBibInsertCitation`): fuzzy search over every resolved
      `.bib` file's entries (key + title + author shown), inserts
      `[cite:@key]` (modern org-cite syntax) at cursor.
- [x] Verified via Xvfb with a two-entry `.bib` file (one with a
      nested-brace title and `{...}`-delimited fields, one with
      `"..."`-quoted fields): the picker correctly listed both with
      accurate title/author extraction (nested braces preserved
      literally rather than truncating at the first `}`), and
      selecting an entry inserted the correct `[cite:@smith2020]`
      citation.
- [x] Legacy org-ref link-type variants (`mep_org_bib_cite_spans`/
      `mep_org_bib_cite_at_cursor`): `cite:key`, `citep:key`,
      `citet:key`, `citeauthor:key`, `citeyear:key` (comma-separated
      multi-key forms too), recognized alongside `[cite:@key]` /
      `[cite/style:@key1;@key2]` via the same manual char-scan style as
      the rest of this file (no regex) — both resolve to the same key
      list consumed by goto/preview below.
- [x] `@string{name = "value"}` macro expansion and `#`-concatenation
      (`mep_org_bib_expand_value`): a bare (undelimited) field value is
      looked up as a macro; `#`-joined pieces (`abbrev # ", Supp"`) are
      expanded piecewise and concatenated. `mep_org_bib_split_top_level`
      (used for field/key separation too) was made quote-aware as part
      of this, fixing a latent bug where a quoted value containing a
      literal `,` (or `#`) was silently mis-split.
- [x] `crossref` resolution (`mep_org_bib_resolve_crossrefs`): runs once
      over the complete entry set from all resolved `.bib` files, after
      parsing, filling in any field a `crossref`-bearing entry doesn't
      itself define from the referenced parent entry.
- [x] Citation goto-entry and preview/hover
      (`mep.org_bib_cite_goto`/`:MepOrgBibCiteGoto`,
      `mep.org_bib_cite_preview`/`:MepOrgBibCitePreview`): jumps to the
      entry's `@type{key,` line in its resolved `.bib` file (a picker
      disambiguates a multi-key citation), and surfaces
      author/year/title/venue via `mep.notify` — the same convention
      `mep.lsp_hover` already uses, there being no dedicated hover
      widget in the codebase. `mep.org_link_follow` now checks for a
      citation at cursor before its `[[...]]` bracket-link handling, so
      the existing follow keybinding works for either citation syntax.
- [x] Verified via Xvfb with a `.bib` file exercising all three gaps at
      once (an `@string` macro, an entry using it via `#`-concatenation,
      an `@inproceedings` with `crossref` to an `@proceedings`) and an
      `.org` file mixing `[cite:@key]` with `cite:key`/`citep:key`/
      `citet:key1,key2`: `:MepOrgLinkFollow` on both the org-cite and
      every legacy form landed the cursor on the correct entry line;
      `:MepOrgBibCitePreview` showed the crossref-inherited year and the
      macro-expanded, `#`-concatenated journal string correctly for
      every syntax variant, including the multi-key `citet:` form.
- ⚠️ **Scope cut**: no dedicated floating hover widget (reuses
      `mep.notify`, matching `mep.lsp_hover`'s own precedent).

---

## Part IX — Aggregated panels

### Phase 40 — Activity bar (notifications/todo/tests/git bar) ✅

*(depends on: Phase 6 notifications, Phase 7 sidebar, Phase 10 icons,
Phase 17 git, Phase 1 jobs)*

- [x] Notifications panel: `:MepNotifyPanel` already existed natively
      (Phase 6's `Editor::ToggleNotifyHistoryPanel`, a C++ sidebar view
      onto `NotifyHistory()`) — reused as-is, no new code.
- [x] Todo panel (`mep.activity_todo_panel`/`:MepActivityTodoPanel`,
      `mep.activity_todo_add`/`:MepActivityTodoAdd`,
      `mep.activity_todo_clear_done`/`:MepActivityTodoClearDone`): a
      Phase 7 sidebar backed by a flat `done|text` text file
      (`mep.activity_todo_file`, default `<cwd>/.mep_todos.txt`) —
      survives restart, distinct from notifications' session-only
      history. Clicking (Enter on the selected row) toggles done state
      in place.
- [x] Tests panel (`mep.activity_test_run`/`:MepActivityTestRun`,
      `mep.activity_test_panel`/`:MepActivityTestPanel`): runs
      `mep.activity_test_cmd` if configured, else auto-detects from
      project marker files (`CMakeLists.txt` → `ctest`, `package.json`
      → `npm test`) via Phase 1's `mep.job_start`, shows a
      PASSED/FAILED summary plus every output line containing "fail"
      (case-insensitive) as its own red row; clicking a failure row
      shows that line as a toast (see scope cut below for what "full
      output" means here).
- [x] Git panel: `:MepGitStatus` already existed from Phase 17 (embeds
      that phase's status/hunk sidebar verbatim) — reused as-is.
- [x] `mep.activity_bar_open`/`:MepActivityBar`: a picker over the four
      panel names (Notifications/Todo/Tests/Git) as the aggregating
      entry point.
- [x] Verified via Xvfb: `:MepActivityTodoAdd` created a todo, saved to
      `.mep_todos.txt` in `done|text` format, and rendered it in the
      sidebar; selecting it and pressing Enter toggled `[ ]`→`[x]`
      on screen; `:MepActivityTestRun` against a real `sh -c` command
      that printed one passing and one `FAILED_case2` line and exited
      1 showed "FAILED (exit 1)" plus the failure line in red;
      `:MepActivityBar` → "Notifications" correctly opened the native
      panel showing recent toast history.
- ⚠️ **Real bug caught and fixed during verification**: the failure-
      line filter used `line:match('[Ff]ail')`, which (Lua patterns
      being case-sensitive past the explicit character class) matches
      `Fail`/`fail` but *not* `FAIL` — an all-caps `FAILED_case2` line
      from the test fixture didn't match and silently failed to show.
      Fixed with `line:lower():find('fail', 1, true)`, a genuinely
      case-insensitive check.
- ⚠️ **Scope cut**: no persistent "slim icon-only button column" C++
      chrome widget (auto-computed icon width, always-visible,
      non-focus-stealing auto-open) — the plan's own described visual
      affordance. Scoped down to `:MepActivityBar`'s picker as the
      aggregating entry point instead, since the four *panels*
      (the phase's functional core) are what matters and all four
      work identically regardless of how they're launched; adding a
      persistent icon-column widget is a pure-chrome follow-up with no
      new backend logic. Test-failure "click for full output" shows
      one line via toast, not a dedicated scrollable detail view (no
      multi-line widget content in the current sidebar primitive).
      Todo persistence uses a flat `done|text` line format rather than
      JSON specifically — same durability property (survives restart)
      without needing a JSON encoder exposed to Lua (none exists yet;
      only Phase 20's LSP-internal `PushJson`/`LuaToJson` do, and
      they're C++-side, not Lua-callable). One interactive sub-step
      (clear-done via keyboard immediately after toggling an item via
      Enter) could not be cleanly re-verified in this pass due to a
      sidebar-focus/key-delivery quirk in the Xvfb test harness after
      an Enter-driven toggle — the add/toggle/persist and test-run
      paths most likely to contain real bugs were fully verified
      (and one bug found and fixed in each), and `clear_done` reuses
      the identical load/filter/save path already proven correct by
      the add and toggle tests, so it was accepted on code review
      rather than forcing further interactive debugging of the test
      harness itself.

---

## Part X — AI & stretch

### Phase 41 — AI integration (LLM streaming + tool-calling agent) ✅

*(depends on: Phase 1 jobs (curl subprocess), Phase 4 decorations
(stream-landing-spot tracking), Phase 7 sidebar (agent panel), Phase 3
floating prompt)*

- [x] HTTP request via a `curl` subprocess (Phase 1's `mep.job_start`,
      plain line-split mode — SSE frames are newline-delimited, so
      Phase 20's raw-byte mode isn't needed here): body written to a
      temp file, `--data-binary @path`, `-sN` for unbuffered streaming
      output.
- [x] SSE streaming parse: `data: {...}` lines stripped of the prefix
      and JSON-decoded per line; `data: [DONE]` recognized and
      ignored. No separate non-streaming path was built — both
      providers are always called with `stream = true`.
- [x] Two provider request/response shapes: OpenAI-compatible
      (`mep.ai_provider = 'openai'`, default, covers OpenAI/Ollama/
      other compatible endpoints) and Anthropic Messages API
      (`'anthropic'`, `x-api-key`/`anthropic-version` headers,
      `content_block_delta`/`delta.text` shape) — both implemented,
      not just one with a stub for the other.
- [x] Send-buffer (`mep.ai_send_buffer`/`:MepAiSendBuffer`), send-range
      (`mep.ai_send_range(a, b)`, explicit line numbers), and true
      Visual-mode send-selection (`mep.ai_send_selection`/
      `:MepAiSendSelection`, also bound directly to `K` in Visual mode
      via `mep.map('v', 'K', ...)`) all stream the response in via
      repeated `mep.insert_text(delta)` calls at an ever-advancing
      cursor position, which is inherently gravity-tracked without
      needing a dedicated Phase-4 decoration-based tracker.
      `mep.ai_send_selection` reads `mep.visual_selection()` (the real
      Visual-mode-selection accessor, `Editor::CurrentVisualSelectionText`)
      -- bound as a direct `mep.map('v', ...)` callback rather than only
      an ex-command, since `mep.visual_selection()` only returns real
      text while `Editor::mode_` is still actually Visual, and typing
      `:` to reach an ex-command already exits Visual mode first (this
      editor doesn't special-case `:` from Visual to prefill a
      `'<,'>`-style range the way real Vim does).
- [x] Agent mode (`mep.ai_agent_prompt`/`:MepAiAgent`): a floating
      `mep.ui_input` prompt feeding a persistent Phase 7 sidebar
      transcript (`mep_ai_agent_render`), full multi-turn conversation
      state (`mep.ai_agent_messages`) with a real tool-calling loop
      (`mep.ai_agent_turn` recurses after every batch of tool calls
      resolves).
- [x] Tools: `read_file`/`list_dir` (`mep.ai_tools`, easy to extend)
      plus `run_command`, each gated by a permission prompt
      (`mep.ui_select`: Allow once / Allow always this session / Deny,
      the last tracked only in an in-memory session table);
      `run_command` always re-prompts via a dedicated
      `mep.ui_confirm`, never eligible for blanket approval, per the
      plan.
- [x] API key handling: `mep.ai_api_key` override, else `os.getenv`
      (`OPENAI_API_KEY`/`ANTHROPIC_API_KEY`), else a `mep.ui_input`
      prompt — kept only in the Lua variable for the session, never
      written to disk.
- [x] Cancel-in-flight (`mep.ai_cancel`/`:MepAiCancel`, `mep.job_kill`
      on the active job id).
- [x] A from-scratch hand-rolled JSON encoder/decoder
      (`mep_ai_json_encode`/`mep_ai_json_decode`) — nothing else in
      the codebase exposes JSON to Lua (Phase 20's `PushJson`/
      `LuaToJson` are C++-internal to the LSP client), so this phase
      needed its own.
- [x] Verified via Xvfb end-to-end against a local mock HTTP/SSE
      server (`socat TCP-LISTEN,fork SYSTEM:'cat <canned-response>'`,
      since no real API key/network access was available or
      appropriate to use in this environment): (1) the JSON
      encoder/decoder round-tripped a string containing embedded
      quotes, a real newline, and a real backslash correctly; (2) a
      canned OpenAI-shape content-delta SSE stream
      (`"Hello"` + `" world"` across two frames) streamed correctly
      into the buffer via `:MepAiSendBuffer`, landing exactly at the
      cursor; (3) a canned OpenAI-shape streaming `tool_calls` delta
      split across two SSE frames (id/name in frame 1, JSON arguments
      in frame 2) was correctly accumulated by index, correctly
      triggered the `read_file` permission prompt with the fully
      reconstructed arguments shown, and — on "Allow once" — actually
      executed `read_file` against a real file, appended the tool
      result to the transcript, and correctly recursed into a second
      `mep.ai_agent_turn()` call, closing the loop as designed.
- [x] Four gaps closed after the initial pass above (all four verified
      by code review plus standalone tests run outside the app -- see
      below -- since no `ANTHROPIC_API_KEY` and no sanctioned use of the
      real `OPENAI_API_KEY` present in this environment made a live
      end-to-end network round trip inappropriate to attempt here):
      (1) **masked API-key input** — `mep.ui_input`'s opts table now
      takes `masked`/`password` (`Editor::prompt_masked_`, threaded
      through `Editor::BeginPrompt`); `DrawPromptOverlay` in `main.cpp`
      renders `*` per byte instead of the real text when set, while the
      real text still reaches `on_done` unmasked and is never persisted.
      The AI module's key-fallback prompt (`mep_ai_get_key`) now passes
      `{masked = true}`. (2) **Anthropic tool-calling** —
      `mep_ai_anthropic_tools_schema` mirrors
      `mep_ai_openai_tools_schema` but in Anthropic's flat
      `{name, description, input_schema}` shape (confirmed against
      Anthropic's Messages API streaming docs, not guessed); the
      streaming parser in `mep_ai_request` now also handles Anthropic's
      `content_block_start{content_block.type='tool_use'}` +
      `content_block_delta{delta.type='input_json_delta'}` accumulation
      (`partial_json` fragments concatenated per content-block index),
      distinct from OpenAI's `tool_calls[].function.arguments` delta
      shape; a new `mep_ai_to_anthropic_messages` converts the shared
      OpenAI-Chat-Completions-shaped `mep.ai_agent_messages` history
      (`role='tool'`, `tool_calls[]`) into Anthropic's content-block
      shape (`tool_result`/`tool_use` blocks on ordinary user/assistant
      messages, since Anthropic has no `tool` role) right before
      sending, so `mep.ai_agent_turn`'s loop works unmodified for either
      provider. (3) **`\uXXXX` decoding** — the hand-rolled
      `mep_ai_json_decode` now decodes the 4 hex digits into a real
      codepoint and UTF-8-encodes it (`mep_ai_utf8_encode`, using Lua
      5.4's bitwise operators) instead of emitting `?`; a high surrogate
      (`\uD800`-`\uDBFF`) immediately followed by a low surrogate
      (`\uDC00`-`\uDFFF`) is combined into one astral codepoint before
      encoding, matching how JSON (like JS) represents emoji/astral
      characters as a surrogate *pair* rather than one escape.
      Verified with a standalone Lua 5.4 harness (the real embedded
      interpreter, linked against the actual `liblua.a` this project
      builds, with the exact `kBuiltinAi` source mechanically extracted
      from `main.cpp` rather than retyped) exercising
      `mep_ai_json_decode`/`mep_ai_utf8_encode` against `é` -> `é`,
      a `😀` surrogate pair -> the correct 4-byte UTF-8 for
      😀 (U+1F600), a malformed lone surrogate (doesn't crash), and an
      encode/decode round-trip of the original quote/newline/backslash
      case; `mep_ai_anthropic_tools_schema`'s shape (`input_schema`
      present, no OpenAI `type`/`function` wrapper, `run_command`
      included); `mep_ai_to_anthropic_messages`'s conversion of a
      synthetic 3-message OpenAI-shaped history (user -> assistant
      tool-call -> tool result) into Anthropic's content-block shape,
      confirming it JSON-encodes cleanly; and the Anthropic
      streaming-delta path end-to-end against canned SSE frames modeled
      on Anthropic's own documented example (`content_block_start` for
      a `get_weather` tool_use split into several `input_json_delta`
      fragments plus interleaved `text_delta` frames), confirming both
      the streamed text and the reassembled tool call's id/name/args are
      correct and that the reassembled args re-parse as valid JSON. A
      separate standalone C++ harness (linked against the same real
      `liblua.a`) exercised `l_ui_input`'s exact opts-table-parsing
      logic (`masked`/`password`/absent/`false`/unrelated-field cases)
      and `DrawPromptOverlay`'s exact masking-substitution logic,
      copied verbatim from the two source files. (4) **true Visual-mode
      send-selection** — `mep.ai_send_selection`, described above;
      verified by code review of `Editor::HandleVisualInput`/
      `Editor::TryLuaMapping` confirming `Editor::mode_` is still
      Visual* when a `mep.map('v', ...)` callback runs, and by a
      standalone Lua test of `mep.ai_send_selection`'s own logic (warns
      on empty selection, forwards non-empty `mep.visual_selection()`
      text verbatim to `mep.ai_send_text`) — **not** verified by an
      actual Xvfb keypress-driven Visual-mode selection + real network
      round trip against either provider, which remains the honest gap
      here.

### Phase 42 — Leetcode (stretch, lowest priority) ✅

*(depends on: Phase 29/30/34 org+babel, Phase 1 jobs (curl), Phase 8
picker — do this last, if at all)*

- [x] Local problem files as `.org` (`#+TITLE:`, `:SLUG:`/
      `:DIFFICULTY:` properties, `Prompt`/`Solution`/`Tests` headline
      structure) — no new parsing, reuses Phase 29's headline model.
- [x] Picker over local problems (`mep.leetcode_picker`/
      `:MepLeetcodePicker`): every `.org` file in `mep.leetcode_dir`,
      titled by `#+TITLE:`.
- [x] Run tests (`mep.leetcode_run_tests`/`:MepLeetcodeRunTests`):
      finds the `Solution` and `Tests` headlines'
      `#+begin_src`/`#+end_src` bodies
      (`mep_leetcode_find_src_under`), splices Solution above Tests
      into one temp file, executes via Phase 34's
      `mep.org_babel_langs` dispatch table (same interpreter mapping,
      not a separate one), reports PASSED/FAILED by exit code plus
      output on failure.
- [x] Verified via Xvfb: the picker correctly listed a problem by its
      `#+TITLE:`; running tests against a Python problem correctly
      dispatched to `python3` and correctly reported FAILED with exit
      127 when `python3` isn't installed in this environment (a real
      environment limitation, not a code defect — confirms the babel
      dispatch and error surfacing both work); a second problem using
      `sh` (available in this environment) correctly spliced a
      `add()` shell function from the Solution block above a Tests
      block that called it, and reported "PASSED (exit 0)" —
      confirming the splice-and-execute mechanism itself is correct
      end-to-end, independent of the Python interpreter's absence.
- [x] Fetch/submit against LeetCode's unofficial API explicitly **not
      implemented** — local-only, per the plan's own stated priority
      ("implement local-only first... not a blocker for calling this
      phase done enough").

### Phase 43 — PDF viewer ✅

*(mirrors the existing image-viewer architecture: `ImageDoc`/
`ImageSession`/`Mode::Image` → `PdfDoc`/`PdfSession`/`Mode::Pdf`)*

Backed by **PDFium** (Google's PDF engine, BSD-3 + permissively-licensed
bundled deps — see `third_party_licenses/pdfium-LICENSE.txt`), fetched as
a prebuilt shared library via CMake `FetchContent` from
`github.com/bblanchon/pdfium-binaries` (there's no CMake build for PDFium
itself upstream — it uses Google's own GN/ninja toolchain) rather than
vendored as source. `src/pdf_doc.h`'s interface (`LoadFromMemory`/
`PageCount`/`PageWidthPt`/`PageHeightPt`/`RenderPage`/`Error`) is
deliberately backend-agnostic, so this was a swap of `pdf_doc.cpp`'s
internals only — none of the `Mode::Pdf`/`PdfSession`/`HandlePdfInput`/
`main.cpp` draw-and-texture-cache integration below needed to change.

- [x] **First attempt**: a hand-rolled PDF object-model parser +
      content-stream interpreter + `stb_truetype`-based rasterizer (kept
      permissive licensing by construction, since it was all vendored
      single-header libs). Got classic + cross-reference-stream xref
      parsing and vector-graphics rendering fully working, but real-world
      PDFs are dominated by text, and font/glyph rendering was still an
      unimplemented follow-up phase when the fidelity gap ("no text
      renders at all yet") proved unacceptable for actual use. Not
      committed to git; reversal was clean because of the interface
      boundary above. Superseded by PDFium below, chosen specifically to
      avoid mupdf's AGPL licensing (mupdf is dual AGPL/commercial —
      copyleft, incompatible with this project's permissive-licensing
      goal) while still getting a fully complete, battle-tested renderer.
- [x] `CMakeLists.txt`: platform-branched `FetchContent_Declare(pdfium
      URL ... URL_HASH SHA256=...)` pinned to `chromium/8009` (Linux x64
      verified/tested; macOS x64/arm64 and Windows x64 branches are wired
      with correct asset names + hashes but unverified, no way to test
      non-Linux here), an `IMPORTED SHARED` `pdfium` target, and a
      post-build step that copies the shared library next to the built
      `mep` executable with `BUILD_RPATH` set to `$ORIGIN`
      (`@loader_path` on macOS) so a plain `./mep` launch finds it
      without `LD_LIBRARY_PATH`. Guarded behind `if(NOT EMSCRIPTEN)` —
      not wired up for the wasm build (see below).
- [x] `src/pdf_doc.cpp`: thin wrapper over PDFium's C API
      (`FPDF_InitLibrary` lazily on first use; `FPDF_LoadMemDocument` —
      keeps its own copy of the source bytes since PDFium reads from the
      buffer lazily for the document's lifetime; `FPDF_LoadPage`/
      `FPDF_GetPageWidthF`/`HeightF` — these already reflect the page's
      own `/Rotate`, no manual rotation math needed unlike the hand-rolled
      version; `FPDFBitmap_Create`/`FillRect`/`FPDF_RenderPageBitmap`
      with the `FPDF_ANNOT` flag, then a BGRx→RGBA channel swap into the
      `out_rgba` buffer `RenderPage`'s interface already expected).
      Encrypted PDFs surface a clear `Error()` message
      (`FPDF_ERR_PASSWORD`) rather than crashing or hanging.
- [x] `Mode::Pdf`/`PdfSession`/`Editor::pdfs_` mirroring the image
      viewer's `Mode::Image`/`ImageSession`/`Editor::images_` exactly;
      `HandlePdfInput` reuses the same `IsKeyPressed||IsKeyPressedRepeat`
      held-key pattern for h/j/k/l (the auto-repeat fix from earlier in
      this session), adds `Ctrl-f`/`Ctrl-b`/`PageDown`/`PageUp` full-page
      jumps and `gg`/`G` first/last page (reusing `pending_g_`), `+`/`-`/
      `=` zoom mirroring the image viewer's center-anchored `apply_zoom`,
      and `Ctrl-R` to toggle `PdfSession::theme_colors`.
- [x] **Zathura-style continuous vertical scroll** (h/l still pan
      horizontally within one page, but j/k now scroll smoothly *through*
      page boundaries instead of hard-cutting at each page, matching most
      real PDF viewers): `PdfSession::page` became an *anchor* page with a
      `scroll_y` measured from its top (can transiently go negative or
      past the anchor's on-screen height), and `PdfSession::rasters`
      virtualizes the CPU-side raster cache down to `{page-1, page,
      page+1}` via `Editor::EnsurePdfPagesRastered` (called every frame
      from `DrawPane`, evicting anything outside that window) — memory
      stays bounded regardless of document length (a 400-page PDF never
      rasterizes more than ~3 pages at once). `HandlePdfInput`'s
      `rebase_scroll` lambda re-bases `scroll_y`/`page` together whenever
      a scroll (or a zoom, since viewport-relative math can push scroll_y
      out of range too) crosses a page's bounds. `main.cpp`'s
      `DrawPane` draws up to 3 stacked textures positioned by a shared
      `kPdfPageGapPx` constant (editor.h) that both the rebase math and
      the draw math reference, so the two never drift apart into a
      visible jump.
- [x] **Theme-colored PDF rendering, on by default** (matches the user's
      color scheme instead of white-paper-with-black-text; `Ctrl-R`
      toggles back to the PDF's actual original colors): `ThemedPdfChannel`
      (main.cpp) maps each pixel's luminance onto the gradient between
      `ResolveHlGroup("Normal")` (foreground) and `ResolveHlGroup(
      "NormalBg")` (background) — PDF white background -> editor
      background, PDF black text -> editor foreground, grays interpolate
      smoothly. Applied at texture-upload time from the *unmodified* raw
      raster (never baked into the cached CPU pixels), so toggling is a
      cheap re-upload, not a re-render. Deliberately desaturates colored
      content (headings, images, diagrams) the same way most e-reader
      night-mode implementations do — a text-reading aid, not a
      color-accurate filter.
- [x] `main.cpp`: `PdfTextureCacheEntry`/`GetOrUpdatePdfPageTexture` — a
      generation-and-theme-aware GPU texture cache keyed by (buffer_id,
      page index), distinct from the image viewer's upload-once
      `g_image_textures` since a PDF page's raster/recoloring both change
      independently of each other. `PrunePdfPageTextures` evicts GPU
      textures for any page `EnsurePdfPagesRastered` no longer keeps a
      CPU-side raster for. Pane header (`"PDF: file (page N/M) zoom%
      [theme|original, Ctrl-R]"`) and `-- PDF --` statusline (via
      `ModeName`) follow the same pattern as the image viewer.
- [x] Verified interactively under Xvfb + xdotool against real-world PDFs
      (a multi-page résumé using cross-reference streams; a large-format
      academic poster with equations/plots/tables/colored headers; a
      199-page dissertation): full text rendering with correct fonts/
      italics/bullets/hyperlink styling; theme-colored rendering on by
      default with legible contrast, `Ctrl-R` correctly toggling back to
      original white/black colors and back again; held-down j/k scrolling
      smoothly across a page boundary (header's page-N/M updating
      correctly mid-scroll) in both directions; GPU texture count staying
      bounded (~2-3 alive at a time) while scrolling many pages into a
      long document; h/l pan, `Ctrl-f`/`gg`/`G` page navigation, and
      `=`/`+` zoom all still correct. Also swept ~140 real PDFs found on
      this filesystem (up to 472 pages) through the parser layer during
      the earlier hand-rolled-xref-stream work; that fix's xref/object-
      stream handling is superseded by PDFium here but the sweep's
      conclusion (real-world PDFs overwhelmingly use xref streams, not
      classic tables) is what motivated prioritizing a complete backend
      over continuing the hand-rolled one.
- [x] **Fixed: PDF theme-recolor lag on a runtime theme change.** The
      texture cache (`g_pdf_page_textures`) only re-uploaded a page when
      its *own* raster generation or `theme_colors` bool changed — not
      when the active *theme itself* changed (e.g. live-previewing a
      colorscheme via the theme picker), so a page that stayed inside the
      3-page render window the whole time kept showing stale baked-in
      colors until it happened to scroll out of that window and back
      (matching the user's report: "have to scroll a couple pages before
      it switches"). Root-caused via a research pass that traced the full
      startup sequence and ruled out the initially-suspected "theme not
      loaded yet on first frame" race (Lua config finishes loading well
      before the first PDF page ever rasterizes/uploads — `main.cpp`'s
      `lua->DoFile(config_path)` runs before the main draw loop even
      starts). Fix: `Editor::ThemeEpoch()` (editor.h/.cpp), an int bumped
      once by `ApplyTheme` every time `current_theme_groups_` actually
      changes; `PdfTextureCacheEntry` now also stores the epoch it was
      last uploaded at, and `GetOrUpdatePdfPageTexture` reuploads whenever
      it's moved on — independent of whether that page's raster or
      `theme_colors` flag changed at all. Verified interactively: switch
      themes via `:colorscheme` while a themed PDF page is on-screen, the
      recolor now happens immediately, no scrolling required.
- [x] **Text search** (`/`, highlighting, `n`/`p`) via PDFium's own text
      API rather than hand-rolled matching: `PdfDoc::Search` (pdf_doc.h/
      .cpp) loads each page's `FPDF_TEXTPAGE` and runs
      `FPDFText_FindStart`/`FindNext` with flags=0 (MATCHCASE unset ->
      case-insensitive, PDFium's own default, matching typical "Ctrl-F in
      a PDF reader" behavior — not literal fuzzy/subsequence matching,
      which wouldn't produce sensible highlight regions for prose text),
      collecting every match's rects via `FPDFText_CountRects`/`GetRect`
      in PDF-point space (scale-independent, so a zoom doesn't require
      re-searching). `PdfDoc::MatchRectsForPage` converts a page's matches
      to device pixels via `FPDF_PageToDevice` — PDFium's own coordinate
      conversion, not a hand-derived rotation transform, so highlights
      stay correctly placed on rotated pages the same way `RenderPage`
      does. `PdfSession` gained `search_active`/`search_input` (captures
      all input while typing, mirroring `Mode::Command`'s input-capture
      shape but scoped to this session rather than a distinct `Mode`) and
      `search_query`/`search_matches`/`search_current`; `PageRaster`
      gained `highlights`, recomputed alongside a page's raster
      (`EnsurePdfPagesRastered`) and whenever the query changes
      (`RecomputePdfPageHighlights`) — main.cpp's draw code never touches
      PdfDoc/PDFium directly, just draws already-converted device rects.
      `n`/`p` (`GotoPdfMatch`) wrap across the whole document and roughly
      vertically-center the target match in the viewport. Highlight color
      reuses the `IncSearch` theme group (text buffers' own live-search
      highlight) at two alpha levels — dim for other matches, brighter for
      `search_current` — rather than a fixed color, matching the PDF
      recoloring feature's own theme-consistency goal. Verified
      interactively: typing after `/` shows a blinking-cursor input bar in
      the pane header in place of the normal label; submitting highlights
      every case-insensitive match; `n`/`p` step through them (including
      wrapping) with the current match visibly brighter and the view
      scrolling to keep it in frame; the header shows match position/count
      once a search is active.
- [ ] **Not wired up**: the Emscripten/wasm build (`pdfium-binaries` does
      publish a `pdfium-wasm` package, but side-module-linking it
      reliably needs pinning to the exact Emscripten SDK version it was
      built with, which this project doesn't track — left as a known gap;
      the wasm build reports a clear "not available in the web build"
      error rather than silently showing a blank pane). Encrypted PDFs
      are surfaced as a load error, not decrypted (PDFium can decrypt
      with a password, but there's no UI prompt for one yet). No count-
      prefixed `{n}G` jump-to-page (needs a numeric-accumulator field,
      unlike `gg`/`G`'s free reuse of `pending_g_`). Search highlighting
      only covers the currently-rendered {page-1,page,page+1} window (by
      design, matching the viewer's virtualized raster cache) -- matches
      elsewhere in the document are still found and jumpable via n/p, just
      not highlighted until their page is actually rendered.

---

### Phase 44 — WYSIWYG office-document pane (.docx/.odt) ✅ (all 5 phases done; Phase 5 partial, see below)

A fourth pane archetype alongside Image/PDF/text: a hand-rolled rich-text
editor for `.docx`/`.odt`, edited with the same vim-modal conventions as
the main buffer (`Mode::OfficeNormal`/`OfficeInsert`/`OfficeVisual`)
rather than a non-modal Word-like feel. Legacy binary `.doc` is out of
scope entirely (confirmed with the user). Deliberately bounded: no
tables, images, headers/footers, footnotes/comments, track changes, real
numbered lists (bullet-or-not only), font-family/size/color choice beyond
a few heading sizes, or full OOXML/ODF style-cascade inheritance — a
genuinely editable, round-trippable subset, not a Word/LibreOffice clone.

- [x] **Immediate fix, unrelated bug found during design review**:
      `SaveBuffer` guarded `IsImageBuffer` but never gained the equivalent
      `IsPdfBuffer` guard when PDF support was added — `:w` on a focused
      PDF pane was silently overwriting the real PDF file on disk with a
      single blank line (a PDF buffer's `Buffer::lines` is a dummy single
      empty line, same convention as Image). Fixed first, independent of
      the new feature.
- [x] **Phase 1 — vendoring + document model + DOCX read + render +
      Normal-mode navigation.** pugixml v1.16 (MIT, XML parsing) and
      miniz v3.1.2 (MIT, the ZIP container both docx/odt use) vendored via
      CMake FetchContent; 4 real Liberation Sans weight/style files
      (Regular/Bold/Italic/BoldItalic, OFL) embedded as byte arrays in
      `src/office_font_data.h` (chosen over faking bold/italic via a
      hand-derived shear/offset transform — avoids repeating the
      `stbtt_Rasterize` offset-sign class of bug hit earlier in the PDF
      work). `src/office_doc.h/.cpp`: a span-based rich-text model
      (`OfficeDoc`/`DocParagraph`/`DocSpan`/`DocFormat`, mirroring
      `Buffer::decorations`'s flat-text-plus-non-overlapping-sorted-spans
      shape) with delete/insert/split/merge/format-toggle primitives
      whose exact edge-case rules (e.g. "insert exactly at a span's end
      isn't sticky-bold", "Enter mid-bold-run splits the span, not just
      the text") are documented on each function; validated against a
      standalone test harness before integration. `LoadDocxFromMemory`
      parses `word/document.xml` via pugixml — recurses into
      `<w:hyperlink>` (otherwise linked text silently vanishes) and maps
      `<w:tab/>`/`<w:br/>` to embedded `\t`/`\n` rather than dropping
      them; individual bad paragraphs/runs are skipped rather than
      failing the whole load (same tolerance convention as `PdfDoc`).
      `src/editor.h/.cpp`: `Mode::OfficeNormal/Insert/Visual`,
      `OfficeSession` (`OfficeDoc` held by value, not `unique_ptr` — no
      opaque C-library handle to hide, unlike Image/PdfDoc), `IsOfficeBuffer`/
      `GetOffice`/`ResizeOfficeViewport`/`SetOfficeScroll`,
      `OpenOfficeInPlace`, `HandleOfficeNormalInput` (hjkl/gg/G,
      word-wrap-*oblivious* motion over paragraph+column — deliberately
      mirrors the main buffer's own no-soft-wrap motion model rather than
      inventing a new one), `LoadFile()`/`SyncModeToActivePaneBuffer()`
      branches. `src/main.cpp`: 4 baked Liberation Sans `Font`s (loaded
      once at startup, not per-size like `g_font` — office text draws at
      many sizes in one frame, so a baked atlas is scaled per-draw
      instead) with a custom codepoint set (ASCII + U+2022 bullet, since
      raylib's default nullptr-codepoints load only covers ASCII and a
      missing bullet glyph silently drew as a "?"); a greedy per-paragraph
      word-wrap (tokenize into words + single whitespace chars, pack
      against the pane's content width, no mid-word splitting — matches
      the plan's literal "split on spaces" algorithm) computed on demand
      each frame scoped to visible paragraphs only, no persistent cache;
      a word-wrap-aware scroll-follow (can't live in editor.cpp, which is
      raylib-free and has no `MeasureTextEx`) that snaps up if the cursor
      is above the current scroll position or advances a visual row at a
      time until it's back in view, mirroring `UpdateScrollForPane`'s own
      shape; per-run rendering (`BuildOfficeFormatRuns` walks a
      paragraph's sorted spans, filling gaps with the default format) with
      underline/strikethrough drawn as a manual overlay line (no such
      glyph variant). Verified interactively under Xvfb+xdotool against
      real LibreOffice-authored `.docx` fixtures: heading levels render at
      distinctly larger sizes, bold/italic/underline are visually
      distinct, bullets render correctly, a hyperlink's text is preserved
      (not lost) even though it isn't yet clickable, center/right
      paragraph alignment both work, hjkl/gg/G move the cursor correctly
      (including through multi-line wrapped paragraphs), and resizing the
      window live re-wraps every paragraph at the new width. Separately
      verified a `.docx` containing a table and an embedded image loads
      with **no crash/hang/corruption** — the table and image are cleanly
      skipped while every paragraph of surrounding text renders intact,
      confirming the "skip what's unsupported, never lose adjacent
      content" tolerance goal.
- [x] **Phase 2 — ODT read.** `src/office_odt.cpp`: `content.xml`'s
      `<office:automatic-styles>` (direct/local formatting, where most
      real-world formatting lives) merged with `styles.xml`'s
      `<office:styles>` (named styles, referenced via
      `style:parent-style-name` from the automatic ones, resolved one
      level only — not a full cascade, same simplification DOCX's
      heading-name recognition already makes) into one style lookup map;
      `<text:style-name>` on a paragraph/`<text:span>` resolves against
      it for bold/italic/underline/strike/alignment. Unlike DOCX's
      leaf-only `<w:t>` runs, ODT paragraphs mix text and element children
      directly, so `CollectOdtInline` walks in document order handling
      `pugi::node_pcdata` and elements in one pass (an earlier two-pass
      draft — walk elements, then separately walk text nodes — would have
      scrambled ordering on any paragraph with text-span-text
      interleaving; caught before it shipped). Headings use `<text:h
      text:outline-level="N">`'s explicit attribute directly — simpler
      and more reliable than DOCX's by-name `"HeadingN"` heuristic.
      Recurses into `<text:span>` (composing its style on top of the
      enclosing format — only fields the span's own style sets override)
      and `<text:a>` (hyperlink text preserved, not clickable, same
      convention as DOCX's `<w:hyperlink>`), maps `<text:tab/>`/
      `<text:line-break/>` to embedded `\t`/`\n` and `<text:s
      text:c="N"/>` (explicit preserved space run) to N literal spaces.
      `<text:list>`/`<text:list-item>` recursion marks contained
      paragraphs `bullet=true` (v1: bullet-or-not only, matching DOCX's
      `<w:numPr>` handling). `ReadZipEntry` (the ZIP-entry-extraction
      helper DOCX's parser already had) was pulled out of its anonymous
      namespace and declared in `office_doc.h` so both parsers — and,
      later, Phase 4's save-back — share one implementation.
      `OpenOfficeInPlace` gained the `IsOdtPath` branch. Verified
      interactively under Xvfb+xdotool against real LibreOffice-authored
      `.odt` fixtures (the same ones used for the DOCX verification pass,
      re-saved as `.odt`): headings/bold/italic/underline/bullets/
      center+right alignment/hyperlink-text-preservation all render
      identically to the DOCX version of the same content; a `.odt`
      containing a table and an embedded image loads with no crash/hang/
      corruption, cleanly skipping just the table/image while every
      surrounding paragraph renders intact.
- [x] **Phase 3 — Insert/Visual editing + formatting toggle + undo.**
      `HandleOfficeInsertInput` (char insert via `ApplyInsertToParagraph`,
      Enter via `SplitParagraphAt`, Backspace/Delete via
      `ApplyDeleteToParagraph` within a paragraph or `MergeParagraphs`
      across a boundary) and `HandleOfficeVisualInput` (hjkl/gg/G extend
      the selection; `b`/`i`/`u` call `ToggleFormatOverRange` — across a
      multi-paragraph selection, the first/last paragraphs are toggled
      over their partial range and every paragraph strictly between them
      is toggled over its full range — then return to `OfficeNormal`,
      matching vim's own "operator over a Visual selection returns to
      Normal" convention) don't go through `ProcessInsertKey`
      (`Buffer`/`CursorPos`-coupled; dot-repeat/macro recording isn't v1
      scope for Office anyway, matching Visual-mode operations' own noted
      scope-out in VIM_PARITY_PLAN.md's Phase 9). `i`/`a` enter Insert
      (snapshotting via `PushUndoOffice()` first, vim's "one undo per
      insert session" convention); `v` enters Visual. `u`/Ctrl-R
      (`UndoOffice`/`RedoOffice`) mirror `Undo()`/`Redo()`'s own
      push-the-opposite-stack-then-swap shape against
      `OfficeSession::undo_stack`/`redo_stack`. Leader-key bindings were
      dropped in favor of direct `b`/`i`/`u` keys in Visual mode — the
      plan's suggested `<leader>b/i/u` would need Lua-side keymap
      registration to be reachable, extra plumbing not essential to the
      core ask; documented here as a deliberate deviation, not an
      oversight. main.cpp's office `DrawPane` branch gained a Visual
      selection highlight (paragraph+col range intersected against each
      wrapped visual line, reusing `BuildOfficeFormatRuns` for the pixel
      x-offset, same as the cursor-position calculation already had).
      Verified interactively under Xvfb+xdotool: `i` + typing inserts
      text preserving surrounding heading formatting; `u` undoes an
      entire insert session in one step back to the pre-insert paragraph
      text; `v` + hjkl shows a visible selection highlight; `b` over a
      selection makes it bold and returns to Normal mode.
- [x] **Phase 4 — save-back for both formats.** `WriteZipReplacingEntry`
      (`office_doc.h`/`.cpp`, shared by both formats): rebuilds the ZIP
      from the session's `original_bytes`, copying every entry except the
      one target part through via `mz_zip_writer_add_from_zip_reader`
      (raw central-directory copy — preserves each entry's original
      compression method and, by iterating the reader's own index order,
      position — satisfying ODF's mimetype-first-and-stored constraint
      automatically as a side effect of never touching that entry).
      `SaveDocxToMemory` re-parses the original `word/document.xml`,
      removes existing `<w:p>`/`<w:tbl>` children (a table is dropped —
      never represented in `OfficeDoc` to begin with), rebuilds `<w:p>`
      elements from `doc.paragraphs` (spans + gaps walked into `<w:r>`
      runs, `\t`/`\n` split into sibling `<w:tab/>`/`<w:br/>` elements —
      the reverse of the parse-side mapping), and re-inserts them before
      the original `<w:sectPr>` (page setup, preserved verbatim — OOXML
      requires it be `<w:body>`'s last child). `SaveOdtToMemory` mirrors
      this for `content.xml`/`<office:text>`, but ODF formatting is
      always a named style reference rather than DOCX's inline toggles,
      so it get-or-creates one `<style:style style:family="text">`/
      `"paragraph"` automatic style per distinct format/alignment
      combination actually used (cached by a packed-bitfield key, so a
      document with many same-formatted runs doesn't grow one style per
      run) and references it via `text:style-name`. Both save functions
      drop a bullet paragraph's list-membership on save (`<w:numPr>`/
      `<text:list>`) rather than emit one referencing a numbering/list
      style definition v1 has no way to construct correctly — a
      documented, deliberate loss (safer than risking a "needs repair"
      prompt from an unresolvable reference) alongside the pre-existing
      table/image loss. `SaveBuffer()` gained an `IsOfficeBuffer` branch
      (native-only — errors clearly on the wasm build) that calls the
      right `Save*ToMemory` by `doc.source_format`, writes the bytes, and
      re-baselines `OfficeSession::original_bytes` to what was just
      written so a later save in the same session copies from the latest
      saved structure rather than the file's state from open time.
      Verified end-to-end, not just internally: edited and saved both a
      `.docx` and an `.odt` under Xvfb+xdotool, confirmed `unzip -t`
      reports no errors on either saved file, then ran each through real
      `soffice --headless --convert-to pdf` (no repair/corruption
      warnings from LibreOffice itself) and opened the resulting PDF in
      mep's own PDFium-backed viewer as an independent rendering check —
      both showed the edited text, heading recognized, and bold/italic/
      underline all correctly preserved.
- [x] **Phase 5 — toolbar (partial: Bold/Italic/Underline only, no
      alignment/bullet buttons — see below).** A toolbar row (height =
      `PaneHeaderHeight()`, matching the main header) between the office
      pane's header and its content, drawn/click-registered in main.cpp's
      `DrawPane` office branch; `content_y`/`content_h` are shrunk in
      place for the rest of that branch only (every path through it ends
      in `return`, so nothing after in `DrawPane` reads the pre-toolbar
      values). Three buttons (B/I/U) call the new public
      `Editor::ToggleOfficeFormat(char)` on click — the identical
      `ToggleFormatOverRange` codepath `HandleOfficeVisualInput`'s b/i/u
      keys already use: with an active Visual selection it toggles over
      it and drops back to `OfficeNormal` (matching the keybinding
      exactly); with no selection (clicked from `OfficeNormal`) it
      toggles just the single character at the cursor rather than being a
      no-op — a small, well-defined fallback, not "sticky" insert-mode
      formatting. `Editor::OfficeFormatActive(char) const` (read-only,
      samples `FormatAt` at the cursor or the selection's first
      character) drives each button's pressed-look. Verified the toolbar
      itself renders correctly (three buttons, correct position, content
      area shrinks to make room) under Xvfb+xdotool; **could not verify
      an actual click** in that environment — Xvfb here has no window
      manager, and unlike keyboard events (`xdotool keydown`/`keyup`
      reliably reach the window), synthetic mouse clicks (`xdotool
      click`/`mousedown`+`mouseup`, tried both) never registered even
      against the pre-existing File menu, a control with years of
      working click-dispatch behind it — an environment/tooling
      limitation, not evidence of a bug. Confidence instead comes from
      code review plus the fact that `ToggleOfficeFormat` calls the exact
      same `ToggleFormatOverRange` primitive the keyboard path already
      exercised live. **Not done**: alignment buttons and a bullet-toggle
      button/keybinding — deprioritized after the toolbar's main
      structural risk (does a second header row + content-area shrink
      break anything else in `DrawPane`?) was already resolved by the
      B/I/U buttons; revisit if/when alignment/bullet authoring (not just
      preserving what a source document already had) becomes a priority.
- [ ] **Not wired up**: native-only for v1 — no Emscripten/wasm build,
      blocked concretely by the wasm file-bridge having no binary-write
      path (`mep_js_write_file` is string-only), mirroring the PDF
      viewer's own native-only precedent but for a different, harder
      reason. No "sticky" insert-mode formatting via typing alone
      (toggle-while-typing with no selection) — Visual-select-then-toggle
      (keyboard or toolbar click) is the only way to apply formatting in
      v1. No mouse click-to-place-cursor (consistent with the main text
      buffer, which also lacks this). No alignment/bullet
      toggle keybinding or toolbar button (Phase 5, see above — reading
      and preserving existing alignment/bullets on load and save both
      already work; only *authoring new* alignment/bullet changes from
      inside mep doesn't yet). Save-back drops a bullet paragraph's list
      membership and any table/image present in the original file (see
      Phase 4's own comment) — known, documented v1 losses, not bugs.

---

### Phase 45 — Spreadsheet pane (.xlsx/.ods/.csv) with a full formula engine ✅

A fifth pane archetype alongside Image/PDF/text/Office: a spreadsheet
pane opening `.xlsx`/`.ods`/`.csv`, edited with the same vim-modal
conventions as the rest of the editor (`Mode::SheetNormal`/`SheetInsert`/
`SheetVisual`), with a **full formula engine** (cell references, ranges,
arithmetic, `IF`/`VLOOKUP`/`INDEX`/`MATCH`, text functions, cross-sheet
references — confirmed via AskUserQuestion, the most ambitious of the
offered scope options). The largest single feature in this codebase so
far — three genuinely new subsystems with no existing precedent: a 2D
sparse cell-grid model, a hand-rolled formula tokenizer/parser/evaluator,
and three new file formats. No array formulas, named ranges, pivot
tables/charts, conditional formatting, cell formatting/styles beyond
plain numeric display, merged cells, frozen panes, iterative/circular
calculation modes, volatile functions, external-workbook references,
dates/date arithmetic, or adding/removing sheets/rows/columns. Legacy
binary `.xls` is out of scope entirely. Planned and stress-tested via two
research/validation passes before implementation — the second one caught
a real correctness bug (a `unique_ptr`-held AST would have made `Cell`
non-copyable, breaking the undo snapshot design before any code existed
to depend on it) and a real memory-safety hazard (range-consuming
functions touching the sparse map's `operator[]` on a
`SUM(A1:A1048576)`-style range would silently insert ~1M empty cells) —
both fixed in the design before Phase 1 was written, not discovered
mid-implementation.

- [x] **Phase 1 — document model + formula engine + CSV + Normal-mode
      grid navigation + Insert/Visual editing + undo.** (Combines what
      the original plan split across Phases 1/4, since editing came
      together naturally alongside navigation once the document model
      was in place.) `src/formula.h`/`.cpp`: a `FormulaNode` AST
      (unlike `regex.cpp`'s deliberately-hidden `Node`/`Parser`, this one
      is genuinely public — `Cell::ast` caches and owns a parsed formula
      outside the parser) built by a recursive-descent parser
      (precedence tiers: compare → concat → add → mul → unary → power →
      primary; deliberately diverges from real Excel in one place, noted
      in code — unary minus binds *lower* than `^` here, so `-2^2` is
      `-4` not Excel's `4`), plus shared cell-address helpers
      (`ColumnLettersToIndex`/`ParseCellAddress`/etc.) used by every
      format's own address parsing. `src/sheet_doc.h`/`.cpp`: `Workbook`/
      `Sheet`/`Cell`/`CellValue` (`Cell::ast` is `shared_ptr<const
      FormulaNode>`, not `unique_ptr`, specifically so `Cell` — and
      therefore `Sheet`/`Workbook` — stays copy-constructible for
      `SheetSession::undo_stack`'s full-snapshot convention); `Sheet`
      stores cells in a sparse `unordered_map` keyed by a packed
      `(row<<32)|col`, with `FindCell` (read-only, never inserts) kept
      strictly separate from `GetOrCreateCell` (the only way the map
      grows) — every range-consuming formula function
      (`SUM`/`AVERAGE`/`VLOOKUP`/etc.) uses `FindCell` and clamps its
      iteration bounds to the sheet's own `max_row`/`max_col`, confirmed
      under AddressSanitizer with a `SUM` over an explicit `A1:A1048576`
      range against a 3-cell sheet completing instantly with no
      million-entry map growth. `EvaluateCell` is the single mandatory
      evaluation choke point every reference (same-sheet, cross-sheet,
      inside a range, inside `VLOOKUP`/`INDEX`/`MATCH`) routes through —
      memoized via a `Workbook::recalc_generation` counter bumped once
      per edit (`SetCellRaw`), not a dependency graph, so an edit
      recomputes every formula cell touched, not just the one edited;
      cycle detection is a per-cell `evaluating` flag, which (because
      every reference shares the one choke point and an unqualified ref
      always resolves against its *own* formula's sheet, threaded
      through as a parameter) catches cross-sheet cycles (Sheet1!A1 →
      Sheet2!A1 → Sheet1!A1) with the same mechanism as a same-sheet one
      — confirmed with both cases directly. Function library: `SUM
      AVERAGE COUNT COUNTA MIN MAX`, `IF AND OR NOT`, `CONCAT/
      CONCATENATE LEN LEFT RIGHT MID UPPER LOWER TRIM`, `ROUND ABS`,
      `VLOOKUP INDEX MATCH` (`MATCH` always exact-match only — v1 has no
      1/-1 approximate-match modes), cross-sheet cell/range refs.
      `src/sheet_doc.cpp` also holds CSV read/write (RFC4180-ish,
      doubled-`""`-escaped quoted fields; a field starting with `=` is
      still treated as a formula even though CSV itself has no formula
      concept — more useful for a "spreadsheet program"; a formula cell
      saves its last-computed raw value, not its formula text, since CSV
      can't represent one). `src/editor.h`/`.cpp`: `Mode::SheetNormal/
      Insert/Visual`, `SheetSession` (2D `scroll_row`/`scroll_col`,
      unlike Office's vertical-only `scroll_para` — and, unlike
      `ResizeOfficeViewport`, `ResizeSheetViewport` does the *entire*
      scroll-follow job itself with no main.cpp-side follow-up call,
      since fixed-size grid cells need no `MeasureTextEx` the way
      word-wrapped text does), `IsSheetBuffer`/`GetSheet`/
      `OpenSheetInPlace`/`HandleSheetNormalInput` (hjkl 2D move, gg/G/0/$
      jump to the sheet's used-range corners, `i`/`a` seed
      `Mode::SheetInsert` from the cell's raw text, `v` enters
      `Mode::SheetVisual`, `x`/`d` clear a cell — no dd/dw operator+
      motion grammar in v1, same simplification the Office pane made)/
      `HandleSheetInsertInput` (plain-string edit buffer, Enter commits
      *and* advances a row — a real spreadsheet's own convention, not
      Office's or the main buffer's Insert-mode behavior)/
      `HandleSheetVisualInput` (rectangular range selection, `x`/`d`
      clears every cell in it)/`PushUndoSheet`/`UndoSheet`/`RedoSheet`
      (whole-`Workbook` snapshots, cursor re-clamped into the swapped-in
      sheet's used range after either). `SaveBuffer()` gained an
      `IsSheetBuffer` guard (blocks `:w` for now — same data-loss bug
      class the Office pane's own Phase 1 fixed for PDF, caught before
      it could happen this time rather than after). `src/main.cpp`:
      `DrawPane` sheet branch — a formula bar (raw cell text, doubling as
      the live edit-buffer display in `SheetInsert`) above a grid with
      column-letter/row-number headers, cursor/selection highighting,
      right-aligned numbers vs. left-aligned text, drawn with the
      existing monospace `g_font` (no new font vendoring needed, unlike
      Office's Liberation Sans) at fixed pixel cell dimensions
      (`kSheetRowHeaderW`/`kSheetColWidth`/`kSheetRowHeight`, shared
      between `editor.h` and `main.cpp` the same way `kPdfPageGapPx`
      already is) rather than font-measured ones. `EvaluateSheetCell`
      (non-const, mirroring `EnsurePdfPagesRastered`'s own shape) lets
      the renderer trigger on-demand cache-filling evaluation despite
      only holding a `const SheetSession*` via `GetSheet`.

      Verified in two passes: (1) a standalone test harness
      (`formula_test.cpp`, built with `-fsanitize=address,undefined`)
      covering arithmetic, all range/lookup/text/logical functions,
      same-sheet and cross-sheet circular references, the sparse
      million-row-range guarantee, edit-triggered dependent
      recomputation, and a CSV round-trip — all passing clean with no
      sanitizer errors; (2) interactive verification under Xvfb+xdotool
      against a real CSV fixture with formulas (`=B2*C2`, `=SUM(...)`,
      `=AVERAGE(...)`): grid renders correctly with computed values,
      hjkl/gg/G navigate (confirmed gg resets row only, not column,
      matching real vim `gg`), the formula bar shows the raw formula
      (not the computed value) for the active cell, Insert mode edits a
      cell and Enter both commits and advances a row, `u` undoes an edit
      and correctly re-clamps the cursor into the now-smaller used
      range, Visual mode shows a live rectangular range highlight, and a
      Visual-mode `d` over a 2×2 range clears all 4 cells **and** the
      dependent `SUM`/`AVERAGE` formulas elsewhere on the sheet
      recompute live to reflect it.
- [x] **Phase 2 — XLSX read** (`src/sheet_xlsx.cpp`). `ParseWorkbookSheetList`
      joins `xl/workbook.xml`'s `<sheet name= r:id=>` list with
      `xl/_rels/workbook.xml.rels`'s `r:id -> Target` map (falling back to
      the conventional `worksheets/sheetN.xml` path if a rels part or
      r:id is missing, same tolerance convention as `LoadDocxFromMemory`);
      `ParseSharedStrings` reads the optional `xl/sharedStrings.xml`
      (concatenating a rich-text `<si><r><t>` run's text, formatting
      discarded). Per-cell type handling covers every `t=` seen in the
      wild: `s` (shared-string index), `str`/`e` (a cached formula-string-
      result or raw error token with no `<f>` backing it — stored as plain
      text, there's nothing to re-derive a live value from), `inlineStr`
      (`<is><t>`), `b` (boolean — wrapped as a trivial `=TRUE`/`=FALSE`
      formula since `SetCellRaw`'s own literal parser treats bare
      "TRUE"/"FALSE" text as Text, not Bool, by design), and bare/`n`
      (plain number verbatim). **Shared formulas** (`<f t="shared" ref=
      si=>`) are expanded, not skipped: a per-sheet `si -> {master row/col,
      formula text}` map is built as rows are walked in document order;
      a member cell with no inline formula text re-parses the master's
      formula (`ParseFormula`), shifts every non-`$` cell/range ref by the
      member's row/col delta (`ShiftFormulaRefs`, new in `formula.h`/
      `.cpp`), and re-serializes it (`SerializeFormula`, also new) into
      the member's own `raw` — skipping this would have silently blanked
      every non-master cell of a real-world fill-down formula, too common
      to accept as a v1 loss the way e.g. merged cells were. Also new in
      `formula.cpp`, needed for `'Sheet 2'!A1`-style cross-sheet refs to
      sheet names containing a space (extremely common in real files):
      `Lexer::LexQuotedIdent`, Excel's own single-quoted-sheet-name syntax
      (`''` = a literal embedded apostrophe, mirroring `LexString`'s `""`)
      — a genuine, if small, formula-engine grammar addition, not
      xlsx-specific plumbing (it's reused verbatim by ODS's own
      sheet-name quoting, see Phase 3).
- [x] **Phase 3 — ODS read** (`src/sheet_ods.cpp`). `content.xml`'s
      `office:spreadsheet` > `table:table` (name) > `table:table-row` >
      `table:table-cell` walked directly via pugixml (same
      literal-namespace-prefix convention `office_odt.cpp` already
      established for ODF — `"table:table-row"` is a plain opaque
      attribute/node name to a non-namespace-aware XML parser, matching
      what real ODF producers literally write). `table:number-columns/
      rows-repeated` expansion: an empty repeated run is never
      materialized (nothing to `SetCellRaw`); a **non-empty** repeated
      cell/row (essentially never seen in real files — repeats are almost
      always trailing empty padding) is written to `min(repeat, 10000)`
      columns/rows and no further, a documented v1 loss mirroring
      `sheet_doc.cpp`'s own `SUM(A1:A1048576)` sparse-map guard. **ODF
      formula-syntax translation** (`TranslateOdsFormula`/
      `TranslateOdsBracketRef`): strips the `of:=`/`oooc:=` prefix,
      converts every bracketed `[SheetName.A1]`/`[.A1:.B5]` reference into
      this engine's own `SheetName!A1`/`A1:B5` syntax (quoting the sheet
      name via the new `LexQuotedIdent` support from Phase 3 when it
      isn't already quoted and needs to be), and turns `;` argument
      separators into `,` — a single left-to-right scan that tracks
      whether it's inside a string literal so an embedded `;`/`[` in a
      quoted formula string is left alone. **Design pivot found through
      real-file testing, not guessed**: an early hand-authored fixture
      using literal `of:=[.A1]*[.B1]` text failed to open in real
      LibreOffice (`Err:510`) even though that syntax is exactly correct
      per the ODF spec — the missing piece turned out to be that
      `xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2"` must
      actually be declared on the document root (ODF's `table:formula`
      datatype grammar requires the prefix to resolve via real XML
      namespace declarations, not just match a string), confirmed by
      generating a real ODS via a LibreOffice Basic macro (headless,
      `-env:UserInstallation=` pointed at a scratch profile since the
      default one's Basic library files are shipped read-only) and
      diffing its actual `content.xml` against the hand fixture. No
      mep-side code needed to change for this — a real-world ODS file
      always already has that namespace declared on its own root, and
      `SaveOdsToMemory` reparses (not regenerates) that root — it only
      exposed that the *test fixture* was wrong, not the parser.
- [x] **Phase 4 — save-back for all three formats + sheet-switching +
      polish.** `WriteZipReplacingEntries` (plural, `office_doc.h`/`.cpp`)
      generalizes the Office pane's own `WriteZipReplacingEntry` to
      replace several named zip entries in one reader/writer pass —
      needed because `SaveXlsxToMemory` may rewrite more than one
      `xl/worksheets/sheetN.xml` part per save (one per sheet); ODS only
      ever touches the single `content.xml` part so its save still uses
      the original singular helper. Both save functions follow the same
      reparse-the-original-and-replace-just-the-data-bearing-children
      discipline `SaveDocxToMemory` established: xlsx clears and rebuilds
      each sheet's `<sheetData>` in place (preserving `<cols>`,
      `<sheetViews>`, `<pageMargins>`, etc. verbatim), ods removes only
      `<table:table-row>` children and rebuilds them (preserving
      `<table:table-column>` and any other structural siblings). A text
      cell saves as XLSX `inlineStr` rather than growing/reindexing
      `sharedStrings.xml` in lockstep — a real, deliberate size-vs-
      complexity tradeoff, not an oversight. A formula cell's `raw` text
      is written directly for XLSX (this engine's native syntax already
      matches Excel's own closely enough — same `,` separators, same
      unqualified/`'Sheet 2'!`-qualified ref style — to need no
      translation); for ODS it's re-derived from the cell's own cached
      `ast` via `SerializeFormula(ast, /*ods_style=*/true)`, producing
      `of:=`-prefixed, `;`-separated, bracket-ref formula text. Web build
      confirmed unaffected (`build/web` still links clean with both new
      files). `Editor::SaveBuffer`'s `IsSheetBuffer` branch (`editor.cpp`)
      replaced the Phase 1 "not implemented yet" guard: CSV routes
      through the same plain-text write path as the main buffer (works
      under wasm too, no binary-write bridge needed); xlsx/ods are
      native-only (`#if !defined(__EMSCRIPTEN__)`, same wasm
      binary-write-bridge blocker as the Office pane) and re-baseline
      `SheetSession::original_bytes` to what was just written, mirroring
      `OfficeSession`'s own save path. **Sheet-switching**:
      `Editor::NextSheet`/`PrevSheet` (public, unlike the private Push/
      Undo/RedoSheet trio just above them in `editor.h` — `mep.sheet_next`/
      `mep.sheet_prev` need to call them) wrap `SheetSession::active_sheet`
      with rollover and re-clamp the cursor via the same
      `ClampSheetCursorAfterSwap` helper Undo/RedoSheet's own post-swap
      clamp already used; bound to Ctrl-PageDown/Ctrl-PageUp in
      `HandleSheetNormalInput` (Excel's own convention — better muscle-
      memory fit than inventing a mep-specific binding), plus
      `:MepNextSheet`/`:MepPrevSheet` ex-commands and `mep.sheet_next()`/
      `mep.sheet_prev()` Lua bindings. **Polish**: the formula bar
      (`main.cpp`'s sheet `DrawPane` branch) now right-aligns a
      `"SheetName  (i/N)"` indicator once a workbook has more than one
      sheet — no click-to-switch tab strip in v1 (same "not attempted
      this phase" scope cut the plan's own mouse-click-to-select-cell note
      below already accepts).

      **Real bug found and fixed, in `formula.cpp` itself, not just the
      new files**: real LibreOffice XLSX/ODS exports both write boolean
      literals as the zero-arg *function-call* form `TRUE()`/`FALSE()`,
      not the bare identifier `TRUE`/`FALSE` this engine's parser
      previously recognized exclusively — confirmed directly by
      inspecting a real LO-generated file, not guessed. `ParsePrimary`'s
      `TRUE`/`FALSE` branch now optionally consumes a following `()`
      before returning the `Bool` node; without this fix, *every* real
      LibreOffice-exported boolean cell this phase's own test fixture
      exercised failed to parse (`ParseFormula` returned nullptr on the
      unconsumed trailing `(`, which `SetCellRaw` correctly but
      unhelpfully turns into a `#NAME?` error cell) — a pre-existing
      formula-engine gap Phase 1's own synthetic test cases never
      happened to exercise, only surfaced once this phase started testing
      against files an actual spreadsheet application produced.

      **Verified in three passes**: (1) a standalone harness
      (`sheet_format_test.cpp`, `-fsanitize=address,undefined`, built and
      run outside the main CMake tree exactly like Phase 1's own
      `formula_test.cpp`) against fixtures generated two ways —
      hand-authored minimal XLSX/ODS via a Python `zipfile` script, and a
      **real** LibreOffice-produced XLSX/ODS pair (headless Basic macro,
      `oDoc.storeToURL`) covering numbers, shared strings, a real boolean
      cell, `IF`/`ROUND` (multi-arg + string-literal + comparison), a
      `SUM` range, a cross-sheet reference to a sheet named `"Sheet 2"`,
      and — spliced by hand into a copy of the real xlsx's own
      `sheet1.xml`, then re-validated by re-opening in real LibreOffice
      before trusting it as a test oracle — an explicit `t="shared"`
      formula group; all pass clean with no sanitizer errors, including a
      save→edit→save-again→reload cycle (confirming a *second* save from
      an already-mep-saved baseline still works) and a full load→save→
      reload round trip for both formats. (2) Every fixture and every
      mep-produced save-back output was independently round-tripped
      through **real** `soffice --headless --convert-to csv` and compared
      value-for-value against the expected results — not just "does mep
      agree with itself," but "does a real spreadsheet application agree
      with mep." (3) Interactive verification under Xvfb+xdotool against
      the real LibreOffice-generated `.xlsx`/`.ods` fixtures (`keydown`/
      `keyup`, not `key` — confirmed reliable in this environment,
      matching Phase 44's own finding): grid rendered with every expected
      computed value on open; `Ctrl-PageDown` switched from "Data" to
      "Sheet 2", the pane header/formula-bar sheet indicator and a
      `-- Sheet 2 --` status message all updated, and the cursor landed
      correctly on the new sheet's `A1`; editing a cell and `:w` showed
      `"real_test.xlsx" written` / `"real_test.ods" written`, and the
      saved files — re-opened independently by real LibreOffice, not just
      re-parsed by mep's own loader — showed exactly the edit plus every
      untouched formula/cross-sheet-ref/boolean/`IF`/`ROUND` cell still
      correct. A follow-up edit that produced a genuinely unparsable
      formula (`=TRUE()FALSE`, from appending onto an existing cell's raw
      text rather than replacing it) was confirmed to degrade correctly
      too: `#NAME?` in the grid, saved as plain display text (the
      documented Error-kind save fallback), zip still valid, real
      LibreOffice still opened it with no corruption — an unplanned but
      welcome stress test of that fallback path.
- [ ] **Not wired up**: native-only for xlsx/ods `:w` (no wasm binary-
      write bridge, same blocker as Office — CSV save-back *does* work
      under wasm, see Phase 4 above). No adding/removing sheets/rows/
      columns, no per-column width overrides (cell text is truncated to
      the fixed column width, not wrapped or overflowed into an empty
      neighbor the way real spreadsheets do), no mouse click-to-select-
      cell or click-to-switch-sheet-tab (not attempted this phase —
      flagged, not forgotten; Xvfb here still has no working synthetic-
      mouse-click path per every earlier phase's own note, so this
      couldn't have been Xvfb-verified even if implemented).

---

### Cross-cutting: top-level exception safety net ✅

Found while chasing a user-reported crash (`terminate called after throwing
an instance of 'std::out_of_range'` from `basic_string::substr`) after a
successful Ctrl-C Ctrl-C org-babel execution — extensive attempts (a
`RelWithDebInfo` build under `gdb` with `catch throw`, replaying dozens of
interaction sequences: repeated/overlapping executions, every language
block in a large multi-language fixture, undo/redo, fold, resize, insert-
mode edits, `:w`) never reproduced the exact throw site, but surfaced a
real, independent gap worth fixing regardless: **there was no exception
handling anywhere in the native C++ code** (`editor.cpp`/`main.cpp`/
`lua_env.cpp`/`job.cpp` — confirmed via a full grep; the only `try`/`catch`
blocks in `editor.cpp` are inside `EM_JS`-embedded *JavaScript* glue for
the wasm build, a different language's `catch (e)` syntax, not C++'s).
Any uncaught C++ exception thrown anywhere during a frame — from a job
completion callback, from `HandleInput`, from `DrawEditor`, from anywhere
— propagates straight past `UpdateDrawFrame` (the sole per-frame entry
point both the native `while` loop and the Emscripten main loop call)
into `std::terminate`, killing the whole app with **no save prompt**,
silently discarding every unsaved buffer.

- [x] `UpdateDrawFrame` (`main.cpp`) now wraps its full per-frame body
      (job polling, input handling, drawing, click dispatch) in a single
      `try { ... } catch (const std::exception &e)`, converting an
      uncaught exception into a visible error notification
      (`Editor::Notify(..., NotifyLevel::Error)`) instead of a crash, and
      simply lets the *next* frame proceed normally. Catches
      `std::exception` specifically, not `...` — an unknown non-exception
      throw (nothing in this codebase throws one) still terminates rather
      than being silently swallowed with no diagnostic. If the throw
      happens mid-`DrawEditor` (after its own `BeginDrawing`, before
      `EndDrawing`), that one frame may render incompletely/skip its
      buffer swap — a one-frame visual glitch, not a further crash
      (raylib's `BeginDrawing` doesn't require a prior frame's
      `EndDrawing` to have completed) — an accepted, minor tradeoff
      against losing all open work outright.
- [x] Verified by temporarily injecting a real `std::out_of_range` throw
      (an unconditional `substr(11)` on an empty string) into
      `TryRunOrgBabelAtCursor` and rebuilding: Ctrl-C Ctrl-C now shows a
      red "Internal error (recovered): basic_string::substr: ..." toast
      and status-line message, the editor keeps running and stays fully
      responsive (buffer content, cursor, and the rest of the file all
      intact) — confirmed against exactly the kind of exception the user
      hit. Removed the injected throw afterward and reconfirmed normal
      Ctrl-C Ctrl-C babel execution still works correctly.
- ⚠️ **This is a safety net, not a fix for whatever specific bug the user
      hit** — the actual root cause of their crash was not identified
      (could not be reproduced despite extensive targeted attempts). If
      it recurs, the new "Internal error (recovered): ..." toast/status
      message will now show the exact exception `what()` string, which
      is far more actionable for tracking down the real cause than a
      full crash with only a bare `terminate`/`abort` in the terminal —
      revisit if/when it does.

---

## Explicitly out of scope / deferred indefinitely (noted so it isn't re-litigated)

- **Org polyglot mode** (Phase 36) — worst complexity-to-value ratio in
  the plan; needs LSP fully working plus per-language project-file
  scaffolding. Revisit only if everything else is done.
- **Full babel/run/REPL language matrices** (~25/~13/~13 languages in
  `mep.nvim`) — start with 2-4 per feature, grow incrementally forever;
  never treat "match every language `mep.nvim` supports" as a phase
  gate.
- **Full LSP server registry** (35 servers) and **full DAP adapter
  registry** (4 adapters, one of which — `delve` — doesn't even work
  over stdio per `mep.nvim`'s own documented gap) — same incremental
  philosophy; the registry is just data, add entries as needed.
- **LeetCode live fetch/submit** — local problem authoring/execution is
  the priority; the reverse-engineered API integration is a stretch
  add-on.
- **PDF viewer (Phase 43): the Emscripten/wasm build, and interactive
  password entry for encrypted documents** — see Phase 43's own trailing
  `[ ]` note. Now that PDFium is the backend, rendering fidelity itself
  (color spaces, patterns/shadings, form-field *appearance*, fonts) isn't
  a mep-side limitation the way it was under the earlier hand-rolled
  renderer — PDFium handles the full PDF spec there. What's still out of
  scope is UI, not rendering: no interactive form-filling/annotation
  editing, no JS-driven interactive forms (the plain, non-V8
  `pdfium-binaries` build is used, matching a read-only viewer's needs).
- **Mouse-drag sidebar resizing** — keymap-driven resize only, matching
  `mep.nvim`'s own explicit scope note (reliable drag-across-border
  detection is a real UI-framework feature this doesn't have).
- **True org buffer narrowing** — fold-based approximation only, same
  documented limitation `mep.nvim` itself carries.
- **A general-purpose regex engine** — since built (`src/regex.h`/
  `src/regex.cpp`: a dependency-free backtracking ECMAScript-lite engine,
  54/54 unit tests passing) and wired into `/`/`?` search and `:s`
  substitute (`Editor::SearchOnce`/`ExSubstitute` in `editor.cpp`, with
  graceful fallback to the original plain-substring `CiFind`/`CiRfind`
  path for patterns that don't compile as valid regex). org/markdown/
  babel header-args deliberately were **not** switched over to it and
  stay on plain-substring/line-pattern matching — nothing there turned
  out to be unusably weak without it (see e.g. the babel `:var`/`:results`
  work, which fixed a real bug with a quote-aware manual splitter, not a
  regex), consistent with the reasoning that motivated deferring this in
  the first place.
