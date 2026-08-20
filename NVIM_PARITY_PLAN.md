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

**Part V — Language intelligence** — done; verified against a real LSP server, and Phase 19 against a standalone harness driving the real vendored parser/grammars, except Phase 26 (implemented, not live-verified — no DAP adapter available)
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

Last updated: 2026-08-19 (plan created; no phases started yet).

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
- [ ] Hover tooltip (small floating window anchored near cursor/mouse,
      auto-dismiss on move) — **deferred**: no feature needs it yet
      (sidebar/chrome hover-tooltips aren't built); will land alongside
      whichkey/chrome in Phase 11 when there's a real consumer.

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
- [x] Placement: **scoped down to one mode** — docked-float only (edge-
      anchored floating box via Phase 3's `DrawFloatFrame`), not a real
      split that participates in pane-layout equalize. A real split
      integration is deferred until a feature actually needs sidebar +
      editor pane resizing together; nothing so far does.
- [x] Open/close/toggle (`OpenSidebar`/`CloseSidebar`/`ToggleSidebar`),
      section collapse/expand via the shared flatten-to-lines helper.
      Keyboard grow/shrink resize keys not yet bound — deferred with
      placement above (mouse-drag resize remains a stretch goal per the
      plan).
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
      / `DrawFloatFrame`. **No preview pane** — see below.
- [x] Source abstraction: **static item list only** — `SetPickerItems`
      lets Lua push a new list at any time (used for the async
      find-files source below), but there's no first-class async
      `get_items(query, callback)`-with-cancellation abstraction; a
      source that wants "live" behavior does its own job spawn +
      `SetPickerItems` from the exit/stdout callback (exactly what
      `mep.find_files()` does). Revisit if a source needs true
      per-keystroke cancel-and-restart (e.g. live grep).
- [x] Debounce: **not implemented** — client-side filtering is fast
      enough to run on every keystroke uncached; no dynamic/debounced
      source exists yet to need it.
- [x] Navigation keys (Ctrl-N/P, arrows, Enter, Escape, Backspace),
      single-instance (`OpenPicker` replaces any existing picker state).
- [x] Built-in sources: **find files** (ripgrep `--files`, falling back
      to `find` when ripgrep is unavailable/empty) and **command
      palette** (`mep.command_names()`, introspecting `lua_commands_`)
      shipped. **Buffer list** shipped too (not originally itemized
      under "ported first" but trivial given `mep.buffer_list()`).
      **Live grep** and **buffer-local line search** — **deferred**,
      no ripgrep-JSON-streaming-into-picker plumbing yet.
- [ ] Preview pane — **deferred**, no consumer needed it yet (find-files
      is the only file-targeting source so far and opens directly).
- [x] Exposed as a reusable Lua API: `mep.picker_open(title, items,
      on_select, on_query_change?)`, `mep.picker_set_items`,
      `mep.picker_close`, `mep.fuzzy_score` — `mep.find_files()`/
      `mep.buffers()`/`mep.commands()` (in `kBuiltinPickerSources`,
      `main.cpp`) are themselves just ordinary consumers of this API,
      the way user config is expected to build custom pickers.

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
- [x] `:colorscheme <name>`/`:colo` ex-command, `Editor::ApplyTheme
      (name)`, `mep.colorscheme(name)`/`mep.theme_names()`/
      `mep.current_theme()` Lua bindings.
- [x] Theme picker: `mep.themes()` (in `kBuiltinPickerSources`) —
      snapshot-current-theme-before-open + apply on Enter + re-apply the
      snapshot on Escape gives commit-on-select and revert-on-cancel for
      free, without needing a per-highlight navigation callback. **Live
      preview while arrow-navigating (before Enter)** is **not**
      implemented — the picker has no "selection changed" hook, only
      query-changed and on-select; would need one added to Phase 8's
      picker to support it.
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
      it. Upgrading later needs a fallback font atlas loaded first; this
      function's callers wouldn't need to change.
- [x] Directory open/closed + tree-expand-marker glyphs: `mep.icons.
      dir_open`/`dir_closed`/`tree_expand`/`tree_collapse` (ASCII `v`/`>`).
- [x] UI-action icon set: `mep.icons.{notify,todo,tests,git,add,clear}`,
      a small bundled Lua table (`kBuiltinIcons`, main.cpp) — no consumer
      yet (activity bar/sidebars using it lands in later parts).
- [x] Font/glyph rendering support check: this *is* Phase 6's finding,
      carried forward — documented inline at `IconForFilename`'s
      declaration rather than re-verified, since nothing changed about
      the embedded font between phases.

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
- [ ] Tabline widget-ification — **deferred**; the existing tab bar
      (mode-colored active/inactive boxes) already runs through the
      theme engine (Phase 9) but wasn't rebuilt on the widget
      abstraction or given new/close-tab buttons.
- [ ] Statuscolumn (gutter unification) — **deferred**; the line-number
      gutter and Phase 4/5's sign/fold rendering already coexist
      correctly in `DrawPane`'s row loop, just not as one named widget
      row a user could reconfigure.
- [x] Active-window border highlight: `BorderActive`/`BorderInactive`
      theme groups (landed as part of Phase 9's chrome migration,
      already satisfies this bullet).
- [ ] Generic click dispatch on widgets — **deferred** (no clickable
      widget exists yet to dispatch to; the menu bar's existing ad hoc
      click handling is untouched and still works).
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
- [ ] Dock/split dual presentation — **not implemented**; the status
      sidebar is docked-float only, same single-placement-mode scope cut
      Phase 7 documented for the generic sidebar widget itself.

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
      `references`, `documentSymbol`, `formatting`. **Not implemented**:
      `implementation`, `typeDefinition`, `signatureHelp`, `completion`
      (LSP-sourced), `codeAction`, `rename` — the request/response
      plumbing (`mep.lsp_request`) is fully generic, so each of these is
      a small addition on the same pattern, not a structural gap; simply
      not written given the sheer remaining scope of the rest of this
      plan.
- [x] Server registry: `mep.lsp_servers` (lua/clangd/pyright entries) —
      3 servers as the plan's own "start with 3-5" asks; only the Lua
      entry was actually exercised (lua-language-server was the one
      confirmed available in this environment).
- [x] `PATH`-executable gating: `mep.lsp_start` spawns via the existing
      Phase 1 Job/`execvp` path (fails gracefully, notifies, if not
      found) — nothing installs anything, matching policy.
- [x] Keybindings exposed as `mep.lsp_hover`/`goto_definition`/
      `references`/`format` + `:MepLsp*` ex-commands (user binds keys to
      them, consistent with every other phase's approach) — goto-
      declaration/implementation/type-definition/rename/code-action
      deferred along with their underlying requests above.

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
      slot, not a multi-source-with-dedup list. Buffer-word (always on,
      `mep.completion_buffer_words`, the default) also folds in matching
      Phase 23 snippet trigger names. **LSP-sourced completion and a
      path source are not implemented** — `mep.lsp_request(..,
      'textDocument/completion', ..)` would be a small addition on the
      same pattern as Phase 20's other requests, just not written.
- [x] Accept (Tab/Enter) / abort (Escape). No item cap (buffer-word
      matches are naturally small at the buffer sizes exercised) and no
      cross-source dedup (only one source is ever active at a time, so
      there's nothing to dedup against yet). Verified via Xvfb: typed a
      partial word, popup appeared with real buffer-word candidates,
      Ctrl-N navigated, Tab accepted and replaced the partial word.

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
      `insertTextFormat=Snippet` handling not implemented** — Phase 20's
      completion request itself isn't wired up yet (see above), so
      there's nothing to receive that format from yet.
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
      (Phase 20's `signatureHelp` request isn't implemented, per Phase 20
      above) — 3 curated per-language templates (lua/python/javascript).
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
- ⚠️ **Scope cut**: no compiled-language support (no `c`/`compile_cmd`
      step — interpreted languages only). `:results` output-mode
      header-arg (e.g. `:results silent`/`:results table`) is parsed
      nowhere — output always becomes a `#+RESULTS:` block. `:includes`/
      `:main` (compiled-language wrapper generation) not applicable
      without compiled-language support. Var substitution is a naive
      textual prelude line, not real per-language literal encoding
      (a `:var` value containing spaces or quotes will not round-trip
      correctly).

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
- ⚠️ **Scope cut**: org-cite (`[cite:@key]`) syntax only — no
      legacy org-ref `cite:key`/`citep:key`/etc. link-type variants.
      No BibTeX `@string` macro expansion or cross-referencing
      (`crossref` field). No citation preview/hover.

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
- [x] Send-buffer (`mep.ai_send_buffer`/`:MepAiSendBuffer`) and
      send-range (`mep.ai_send_range(a, b)`, callable but with no
      dedicated visual-selection command — see scope cut) stream the
      response in via repeated `mep.insert_text(delta)` calls at an
      ever-advancing cursor position, which is inherently
      gravity-tracked without needing a dedicated Phase-4
      decoration-based tracker.
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
- ⚠️ **Scope cut**: no true Visual-mode "send-selection" — there is no
      Lua-facing API anywhere in the codebase for reading the current
      visual selection's range (would need new C++), so this phase
      exposes `mep.ai_send_range(a, b)` (explicit line numbers) instead
      of a `:MepAiSendSelection` bound to actual Visual-mode state.
      `mep.ui_input` has no masked/hidden-echo mode, so the API-key
      prompt fallback is visible input, not masked (still never
      written to disk, satisfying the "never persisted" half of the
      requirement). Anthropic's tool-calling isn't wired up (no tools
      schema built for that provider — `mep_ai_openai_tools_schema`
      only) — agent mode's tool loop is effectively OpenAI-only;
      Anthropic works for plain streaming send/agent-without-tools.
      The hand-rolled JSON decoder doesn't handle `\uXXXX` escapes
      beyond emitting a literal `?` placeholder (no real unicode
      codepoint decoding).

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
- **Full theme palette set** (28 in `mep.nvim`) — port a handful first,
  the data format makes adding more later trivial and low-value to
  front-load.
- **Mouse-drag sidebar resizing** — keymap-driven resize only, matching
  `mep.nvim`'s own explicit scope note (reliable drag-across-border
  detection is a real UI-framework feature this doesn't have).
- **True org buffer narrowing** — fold-based approximation only, same
  documented limitation `mep.nvim` itself carries.
- **A general-purpose regex engine** — org/markdown/babel header-args
  and every other "pattern matching" need in this plan should stay on
  plain-substring/line-pattern matching, consistent with
  `VIM_PARITY_PLAN.md`'s own search/substitute decision. Only reconsider
  if a specific feature turns out to be unusably weak without it.
