# Help documentation plan

The built-in help workspace (dashboard → `h Help`, `<leader>hh`, `:MepHelp`)
currently ships four stub pages: `intro`, `editing`, `shortcuts`,
`writing-help`. This plan is the full manifest for turning that into mep's
real manual.

Status legend: `[ ]` not started, `[~]` partial (a stub exists), `[x]` done.

---

## How help works today (the constraints every page is written against)

- `kBuiltinHelp` (`src/main.cpp`) scans a directory for `*.html`/`*.htm`,
  reads each file's `<title>` for its sidebar label, and lists them in one
  flat section sorted **`intro.html` first, then alphabetically by title**.
- The directory is the workspace's own `help/` if it has any pages, else
  `mep.bundled_help_root()` (`<prefix>/share/mep/help`, or
  `MEP_SOURCE_HELP_DIR` in a dev build). `CMakeLists.txt:612` installs
  `help/` wholesale, so a new page needs no build-system change.
- Pages render in mep's own in-pane HTML viewer, which passes the full
  twelve-level `examples/web/` ladder — CSS, tables, images and JS all work.
- `f` in a help page labels every visible link for one-key following;
  `mep.help_open_page` / the `on_frame` hook keep the sidebar in sync with
  whatever page a link navigated to.
- Sources are Org (`help/*.org`), exported to the HTML actually shipped.
  The exporter (`mep.org_export_html`, `kBuiltinOrgExport`) is **Lua inside
  a running editor** — there is no headless export path today.

### Part 0 — infrastructure prerequisites

These block the page work, or make it much worse if skipped. Do them first.

- [x] **0.1 Grouped sidebar.** One `sidebar_set_sections` section per help
  section, in a declared order (`MEP_HELP_SECTION_ORDER`), pages ordered
  within it, all but the current page's section collapsed. A page declares
  its placement in its own Org source via two `#+HTML_HEAD:` meta tags
  (`help-section`, `help-order`); an undeclared page falls back to an
  "Other" section, ordered by title, as before.
- [ ] **0.2 Help search.** `mep.picker_open` over every page's title +
  headings + body text, bound to `<leader>hf` and `/` inside a help pane;
  Enter opens the page. Without it an exhaustive manual is unnavigable.
- [x] **0.3 Page template + shared stylesheet.** `help/_template.org` (the
  starting point for a new page; `_`-prefixed sources are skipped by the
  exporter) and `help/help.css`, linked per page via `#+HTML_HEAD:` — which
  the exporter now emits *after* its own `<style>` block so a page can
  override it. Tuned against mep's renderer, whose quirks are documented in
  the stylesheet: block margins are added to its own line-based defaults
  rather than replacing them, and it ignores `@media` entirely (so
  browser-only rules live there). A third apparent quirk — tables rendering
  with the cell grid line drawn through each row's text — turned out to be a
  real renderer bug, and was fixed rather than designed around (see
  `HtmlLayoutTable`).
  Hand-maintained prev/next links are dropped: they cannot scale to 109
  pages and duplicate the sidebar. Pages close with "See also" instead.
- [x] **0.4 Headless export pipeline.** `mep --export-org <in> <out>` renders
  one Org file to standalone HTML with no window, frame loop or session
  (`RunHeadlessOrgExport`, `src/main.cpp`); `just help` runs it over every
  `help/*.org`. Needs no display, so it works in CI and over ssh.
- [x] **0.5 Freshness test.** `scripts/check_help.py`, run by `just
  help-check` and by `just test`. Staleness is caught by re-exporting every
  source and comparing bytes, not by mtimes (git does not preserve those, so
  a fresh clone would fail an mtime check and a rebase could pass one while
  shipping stale HTML). Also checks title/section metadata and internal
  links, and reports command/`<leader>` coverage — enforced only under
  `--strict`, since a hard gate would be permanently red until the last page
  lands. Baseline at the time of writing: 6/302 (2.0%).
- [ ] **0.6 Contextual help.** `<leader>hh` from a pane already showing a
  notebook / PDF / 3D model / org file opens that feature's page rather than
  `intro`. Sidebars' own `?` help overlay gains a "full docs" row.
- [ ] **0.7 `:help <topic>`.** A Vim-style command that jumps straight to a
  page (and, with an anchor, a heading) by name, with command-line completion
  over page names.

---

## Part 1 — Getting started (5 pages)

- [x] **1.1 `intro` — mep Help.** Rewrite as a real landing page: what mep
  is (modal editor, raylib, embedded Lua, native + wasm), a "learn in 5
  minutes" path, and a card grid linking every section below.
- [x] **1.2 `first-steps` — Your first ten minutes.** Open a file, move,
  edit, save, quit. Modes explained once, properly. The `<Space>` which-key
  popup as the discovery mechanism.
- [x] **1.3 `dashboard` — The start screen.** When it shows (every project
  and buffer pristine), `p` Projects / `h` Help, `j`/`k`, the hint row, and
  what makes it disappear.
- [x] **1.4 `installing` — Installing and running.** `just run`,
  `just run-wasm`, `just build-native`, `just build-web`, `just clean`; the
  nix/direnv dev shell; `mep [--project <dir>] [--no-session] [file]`;
  `$MEP_PROJECT`; `~/.config/mep/init.lua`; what the wasm build cannot do
  (no host filesystem, `--no-session`-equivalent, clipboard is best-effort).
- [x] **1.5 `vim-differences` — What is and isn't Vim.** The honest
  list: plain-substring search and `:s`/`:g` (no regex, no backreferences),
  directional `Ctrl-W hjkl`, two-way boundary `mod1+Shift+hjkl` resize,
  Visual-mode changes not `.`-repeatable, no `s`-as-substitute. Links to
  `VIM_PARITY_PLAN.md` for the phase-by-phase account.

## Part 2 — Core editing (8 pages)

- [x] **2.1 `modes` — Modes.** Normal / Insert / Replace / Visual /
  Visual-Line / Visual-Block / Command-line / Terminal, how to enter and
  leave each, `Ctrl-\ Ctrl-n`.
- [x] **2.2 `motions` — Motions.** `hjkl 0 ^ $ gg G w b e ge W B E gE { } %
  H M L f F t T ; ,`, counts (`5j`, `10G`), the pending-count/register
  display in the status line.
- [x] **2.3 `operators` — Operators and text objects.** `d y c`, `D C Y`,
  `gu gU`, `gq`/`gqq` reflow with `textwidth`, `> <`, `~`; doubling for the
  current line; the full text-object list (`iw aw i" a" i( a( ip ap` …);
  `x p P`, `r{char}`.
- [x] **2.4 `insert-mode` — Insert and Replace.** `i a I A o O`, `R` with
  Backspace-restores, `Ctrl-W`/`Ctrl-U`, `Ctrl-R {reg}`, `Ctrl-Shift-V`.
- [x] **2.5 `visual-mode` — Visual modes.** `v V Ctrl-V`, `o`, text objects
  as selection, repeatable `>`/`<`, Visual Block `d`/`y`/`I`/`A` and the
  ragged-right `$A`.
- [x] **2.6 `registers-marks` — Registers, marks, jumps.** `"{a-z}` /
  `"{A-Z}`; the unnamed register *is* the system clipboard (`"+`/`"*` as
  aliases, linewise vs charwise by trailing newline, X11 ownership caveat,
  `mep.clipboard_get/set`); `m{a-z}`, `` ` ``/`'`, `` `` ``/`''`, `gv`;
  `Ctrl-O`/`Ctrl-I` jumplist (per pane, crosses buffers).
- [x] **2.7 `undo-repeat-macros` — Undo, repeat, macros.** `u`/`Ctrl-r`
  (per-buffer, shared between panes on the same buffer), `.` and `3.`,
  `q{a-z}`/`q{A-Z}`/`@{a-z}`/`@@`, and the documented Visual-mode gap.
- [x] **2.8 `scrolling` — Scrolling and view.** `Ctrl-D/U/F/B`, `zz/zt/zb`,
  `Ctrl-A`/`Ctrl-X` with counts and leading-zero preservation, soft `wrap`
  and why `j`/`k` still move by buffer line.

## Part 3 — Search, substitute, the command line (4 pages)

- [x] **3.1 `search` — Searching.** `/ ? n N * #`, plain substring,
  `wrapscan` and the hit BOTTOM/TOP messages, `ignorecase`, search history
  with Up/Down.
- [x] **3.2 `substitute` — `:s`, `:g`, `:v`.** Ranges (`5,10`, `.,+3`,
  `'a,'b`, `%`), flags, `:g`/`:v` with `:normal`, and `:d :y :m :t/:co`.
- [x] **3.3 `command-line` — The `:` command line.** History, completion
  (command names, and file paths for path-taking commands), `:lua`,
  `:source`, `:normal`, `:<N>`, `:e` on a nonexistent path.
- [x] **3.4 `quick-jump` — `s` quick jump.** The flash/leap-style typed
  query across every visible text pane, label assignment rules, smart-case,
  Enter/Backspace/Esc, `mep.quick_jump*` from Lua, rebinding it.

## Part 4 — Files, buffers, panes, tabs (6 pages)

- [x] **4.1 `files` — Opening and saving.** `:e :w :wa :q :q! :qa :wq :wqa
  :x`, the unsaved-changes guards, `:bnext`/`:bprevious`/`:bdelete`,
  workspace-scoped buffer lists.
- [x] **4.2 `panes-tabs` — Panes and tabs.** `:split`/`:vsplit`/`:close`,
  `Ctrl-W w/W/h/j/k/l/c/s/v`, `:tabnew`/`:tabdelete`/`:tabnext`, `Ctrl-T`,
  `Ctrl-Tab`; shared buffers and shared undo; pane header `| _ x` buttons;
  buffer tabs per pane; drag-and-drop from the file tree with the drop-zone
  pinwheel.
- [x] **4.3 `mod1-keys` — The `mod1` layer.** `mod1+s/v`, `mod1+hjkl`
  directional focus (the tmux/i3 overlap heuristic), `mod1+Shift+hjkl`
  resize, `mod1+Ctrl+hjkl` move buffer tab, `mod1+d` close tab, `mod1+m`
  maximize / sidebar popout, `mod1+Enter`; `mep.set_mod1`, `mep.map_mod1`.
- [x] **4.4 `layouts-zen` — Layouts, zoom, zen.** `:MepLayout master-left/
  right/top/bottom` and friends, `:MepPaneZoom` / `<leader>zz`, `:MepZen`,
  `:MepScratch` / `<leader>bs`, font size `Ctrl+Shift+=` / `-`.
- [x] **4.5 `file-tree` — The file tree.** `<leader>ff`, hidden files
  (`<leader>fh`), refresh (`<leader>fr`), the native open dialog
  (`<leader>fo`), `:MepOil`, per-file icons, `?` help overlay, creating /
  renaming / deleting.
- [x] **4.6 `pickers` — Pickers.** Find files (`<leader>ff`/`pf`), live grep
  (`<leader>pr`), buffers (`<leader>bb`) with live preview and `C-v`/`C-s`/
  `C-t`/`C-i`/`C-d`, commands, themes (`<leader>ut`, with live preview),
  keymaps (`<leader>hk`), snippets, Activity; the shared key row along the
  bottom; `mep.picker_*` and `mep.buffer_open_with`.

## Part 5 — Projects and workspaces (3 pages)

- [x] **5.1 `projects` — Projects.** What a project is; `mep --project`,
  `$MEP_PROJECT`, `:project`, `mep.project_open`; `:projects` /
  `<leader>pp`; `:projectclose`/`next`/`previous`; the first-open default
  layout (README + terminal + file tree); `:projectclear` / `<leader>pc`.
- [x] **5.2 `workspaces` — Workspaces as git worktrees.** `:wsnew` (and
  `:wsnew!` onto an existing branch), the async `[name...]` label, cwd
  following the active workspace, per-workspace LSP clients and buffers,
  `:ws`/`:wsnext`/`:wsprevious`/`Ctrl-Alt-[`/`]`/`Alt-1..9`, `:wslist` /
  `<leader>ww`, `:wsrename`, `:wsdelete` (branch is kept), `:wsadopt`,
  `:wsprune`, `<leader>gw`, `mep.opt.worktree_dir`,
  `mep.opt.workspace_git_dirty`, non-git projects.
- [x] **5.3 `sessions` — Session save and restore.** What is persisted
  (workspaces, tabs, split trees, open files relative to the workspace root,
  cursors, terminals), where (`$XDG_DATA_HOME/mep/workspaces/<name>-<hash>.json`),
  when (500ms debounce + on quit), `--no-session`,
  `mep.opt.restore_workspaces`, `:wssave`/`:wsrestore`, and the recovery
  behaviour for missing files / vanished worktrees / a corrupt file.

## Part 6 — The interface (5 pages)

- [x] **6.1 `tab-bar` — The tab bar.** `[project] [ws1] [ws2*] │ ● ○ + x`;
  clicking, middle-clicking and hovering a workspace label; participant
  chips; the three search buttons; the sidebar toggle row (Files, Git,
  Symbols, Structure, Todo, Tests, Notifications, AI Agent).
- [x] **6.2 `status-line` — The status line.** Mode, filename, Ln/Col, the
  direnv chip, the Pomodoro chip, the active-todo chip with its live timer,
  collaborator chips, pending count/register display,
  `mep.set_statusline`, `mep.active_todo_set`.
- [x] **6.3 `sidebars` — Sidebars.** Docking and stacking on one edge,
  dragging the divider, `mod1+Shift+j/k` re-split, `mod1+j/k` focus,
  `mod1+Ctrl+j/k` swap, `mod1+m` popout with its preview column, the shared
  `?: help` footer, `current = true` auto-scroll, tab strips.
- [x] **6.4 `which-key` — Leader and which-key.** `<Space>` as leader, the
  group hierarchy (a/b/c/d/f/g/h/j/l/n/o/p/r/s/t/u/v/w/y/z and the `oe`/
  `ot`/`or` subgroups), icons and highlight groups, `mep.leader_map` /
  `mep.leader_group`, `<CR>` in a sequence, `mep.set_leader`.
- [x] **6.5 `notifications` — Notifications.** Toasts, the history panel
  (`<leader>nn`, `:MepNotifyPanel`), `:MepNotifyClear`, `:MepNotifyDismiss`,
  `mep.notify` and Lua `print()` routing, severity levels.

## Part 7 — Themes and appearance (2 pages)

- [x] **7.1 `themes` — Colorschemes.** `:colorscheme` / `:colo`, the picker
  with live preview (`<leader>ut`), the full 62-palette list, light/dark
  detection (the dashboard logo swap), `mep.colorscheme`,
  `mep.current_theme`, `mep.theme_names`.
- [x] **7.2 `highlight-groups` — Highlight groups and decorations.** Named
  groups, how syntax/LSP/spell/git all render through the one decoration
  pipeline, `mep.deco_add`/`buffer_deco_add`/`ns_create`/`ns_clear`,
  `mep.hl_for_file`, `mep.icon_for_file`, fonts (JetBrains Mono, the Nerd
  Font icon font, emoji and symbol fonts) and font sizing.

## Part 8 — Git (4 pages)

- [x] **8.1 `git-gutter` — The git gutter.** On by default; green/yellow/red
  marks and why they are rectangles; live unsaved-text diffing on a debounce;
  base `HEAD` by default (and why that differs from gitsigns/vim-gitgutter),
  `:MepGitGutter base index|<ref>`, `on|off|toggle` / `<leader>gd`,
  `mep.git_gutter_line_hl`, `mep.git_gutter_summary`.
- [x] **8.2 `git-hunks` — Working with hunks.** `]g`/`[g` (and `]c`/`[c`),
  `<leader>gv` preview float, `mep.git_hunk_preview_on_jump`, `<leader>ga`
  stage / `<leader>gr` reset, and why staging re-diffs against the index.
- [x] **8.3 `git-panel` — The git panel.** `<leader>gG` / `<leader>gg` /
  `:MepGitStatus`; the five views (Status, Log, Graph, Branches, Stash) with
  every key each defines; `Tab`/`1`-`5`; the shared `c`/`C` commit editor
  over `COMMIT_EDITMSG` (`ZZ` commits, `Escape` aborts), `P`/`l`/`f`/`R`/`?`;
  the ahead/behind head section; `<leader>gl/gL/gb/gs/gc/gp`; the `:MepGit*`
  command forms.
- [x] **8.4 `git-workspaces` — Branch-per-workspace workflow.** The
  worktree model end to end: create a workspace per branch, `<leader>gw`,
  ahead/behind, merging and rebasing from the Branches view, cleaning up.

## Part 9 — Code intelligence (8 pages)

- [x] **9.1 `lsp` — Language servers.** Auto-attach rules, one client per
  language per workspace root, the supported server table (clangd, gopls,
  pyright/basedpyright, lua-language-server, rust-analyzer, tsserver, jdtls,
  metals, elixir-ls, ocamllsp, hls, clojure-lsp, fortls, nimlsp,
  kotlin-language-server, bash-language-server, docker-langserver …),
  `:MepLspAttach`, `K` hover, `gd`/`gr`/`<leader>li`/`<leader>lt`,
  `<leader>lk` signature help, `<leader>rn` rename, `<leader>ca` code
  action, `:MepLspFormat`, diagnostics in the sign column and `:MepDiagShow`.
- [x] **9.2 `completion` — Completion.** The popup, its keys, and the single
  merged source: buffer words + snippet triggers + filesystem paths + LSP
  results through one seen-set, capped and throttled;
  `mep.set_completion_source` and the accept/resolve hooks.
- [x] **9.3 `snippets` — Snippets.** The per-language registry (c, go,
  js/ts, lua, python, rust, shell), trigger expansion, `$1`/`${1:default}`/
  `\$`, `:MepSnippetNext`/`Prev`, `<leader>yy` picker, and the documented
  stale-tabstop limitation when editing before jumping.
- [x] **9.4 `syntax` — Syntax highlighting.** Treesitter grammars (c, cpp,
  javascript, lua, markdown, org, python, r) with incremental reparse, the
  hand-rolled per-line lexer fallback for everything else, `:MepSyntax`,
  `mep.syntax_auto`, `:MepSyntaxFold`, `:MepColorize` swatches.
- [x] **9.5 `structure-symbols` — Structure and Symbols.** `<leader>ss` /
  `sS` treesitter structure pane and buffer-local split, `<CR>` to jump,
  `:MepStructure`/`:MepStructureSplit`; the LSP-backed Symbols sidebar
  (`:MepSymbols`); "you are here" tracking.
- [x] **9.6 `folding` — Folds.** `mep.fold_create`/`fold_toggle`/
  `fold_clear_provider`, the gutter markers, syntax folding, markdown
  heading-depth folding, org cycling.
- [x] **9.7 `formatting` — Formatting.** `gf` / `:MepFormat`, the per-
  filetype formatter table, filter vs file mode and the `{}` placeholder,
  why `--assume-filename`/`--stdin-filename` are passed, the `mep.lsp_format`
  fallback, format-on-save.
- [x] **9.8 `spell` — Spell checking.** The red squiggle, `<leader>z`
  group (`zt` toggle, `zn`/`zp` navigate, `zs` suggestions, `zf` fix,
  `zg` add, `zw` mark wrong), the personal dictionary, Visual-mode use,
  `mep.spell_*`.

## Part 10 — Running, testing, debugging (6 pages)

- [x] **10.1 `terminals` — Terminals.** `:terminal`/`:term` below the
  current pane; the per-tab terminal (`<leader><CR>`, `:tabterminal`,
  `mep.opt.tab_terminal_share`) and its hide/restore/respawn behaviour;
  Terminal mode and `Ctrl-\ Ctrl-n`; `Ctrl-Shift-V`; the exported
  `$MEP_WORKSPACE_NAME`, `$MEP_PROJECT_NAME`, `$MEP_AGENT_SOCKET`,
  `$MEP_TERMINAL_BUFFER`; `mep.terminal_here`/`terminal_here_argv`/
  `terminal_info`.
- [x] **10.2 `run-button` — Run the current file.** `<leader>rr` /
  `<leader>rs`, `mep.opt.run_button_defaults` (py/r/c/cpp and the R/cc/cxx
  aliases), per-project overrides and where they are stored, why flags go
  after the source file for compiled languages.
- [x] **10.3 `runners` — Command runner.** `<leader><Space>` / `:Runner`;
  just → make → ninja → cmake precedence (`mep.opt.runner_order`); the
  preview column with the dependency tree; the Workspaces and Variables tabs
  (`<leader>jw`, `<leader>jv`); async invocation.
- [x] **10.4 `repl` — REPL and send-to-terminal.** `:MepReplStart`,
  `:MepReplSendLine`/`SendBuffer`, `<leader>rj` jump to/from the Run pane,
  the vim-slime-style `:MepTermSendLine`/`SendRegister` with per-buffer
  terminal targets; the documented limits (line-oriented, SGR colors only,
  no TUI, no raw keystroke forwarding).
- [x] **10.5 `tests` — The Tests panel.** `<leader>tT`,
  `:MepActivityTestPanel`/`TestRun`, `gt` / `:MepLangTest` per-filetype test
  runners, `mep.opt.lang_test_*`, failure-line jumping.
- [x] **10.6 `debugging` — DAP debugging.** The `<leader>d` group (`dd`
  start/continue, `db` breakpoint, `dc` clear, `di`/`dn`/`do` step,
  `dr` restart, `dt` terminate, `du` toggle UI, `dv` evaluate), the Debug
  sidebar (Call Stack / Variables / Breakpoints), the Debug Console pane,
  breakpoint signs, `mep.opt.dap_r_port`, `:MepDap*`.

## Part 11 — Language UI modes (4 pages)

- [x] **11.1 `language-ui` — What a language UI mode is.** `<leader>lu` /
  `<leader>uu` / `:MepLanguageUi`, one active mode per tab, the shared
  layout, `mep.language_ui_modes` for adding your own.
- [x] **11.2 `r-mode` — R.** The console, environment/objects, packages and
  plots panes; `mep.opt.r_ui_cmd` and the share options; `gh` → R `help()`.
- [x] **11.3 `python-mode` — Python.** The console, variables, modules;
  `mep.opt.py_ui_cmd`; `gh` → pydoc.
- [x] **11.4 `c-mode` — C and C++.** The Assembly tab (sharing the run
  button's compiler/flags), the Hex view (`mep.opt.c_ui_hex_page`), Data and
  Build panes, `mep.opt.c_ui_filter_directives`; `gh` → `man`.

## Part 12 — Org mode (12 pages)

- [x] **12.1 `org-basics` — Org files in mep.** The render/edit split,
  `:Org`/`:Text` to leave a rendered view, headings, `:MepOrgCycle`/`Fold`,
  promote/demote (`:MepOrgPromote`/`Demote`(`Subtree`)),
  `:MepOrgInsertHeading`/`InsertTodoHeading`, `:MepOrgNarrow`/`Widen`,
  `:MepOrgNext`/`Prev`, `:MepOrgMatch`, emphasis highlighting.
- [x] **12.2 `org-todo` — TODO keywords, priorities, tags.** `:MepOrgTodo`,
  `:MepOrgPriority`, `:MepOrgTags`/`TagsPicker`, `:MepOrgCheckbox`,
  `:MepOrgSortTodo`/`SortAlpha`/`SortPriority`, `:MepOrgSparseTodo`,
  `:MepOrgArchive`, `:MepOrgRefile`.
- [x] **12.3 `org-dates` — Timestamps, scheduling, deadlines.**
  `:MepOrgTimestamp` (and `Inactive`/`Range`/`Repeater`/`Incr`/`Decr`/
  `Highlight`), `:MepOrgScheduled`, `:MepOrgDeadline`.
- [x] **12.4 `org-agenda` — Agenda.** `:MepOrgAgendaToday`/`Week`/`Todo`/
  `Overdue`/`Search`, `:MepOrgAgendaAddFile`, glob expansion.
- [x] **12.5 `org-clock` — Clocking and the Todo panel.**
  `:MepOrgClockIn`/`ClockOut`/`ClockTable`, the `:LOGBOOK:` `CLOCK:` lines,
  one running clock at a time; the Todo sidebar (`<leader>tt`,
  `:MepActivityTodoPanel`) with `a`/`e`/`d`/`x`/`Enter`/`o`/`R`/`?`, the
  float editor, `mep.activity_todo_file`, the status-bar chip.
- [x] **12.6 `org-capture` — Capture and templates.** `:MepOrgCapture`,
  `:MepOrgCaptureCommit`/`Abort`, `:MepOrgTemplate`, template expansion.
- [x] **12.7 `org-links` — Links, footnotes, bibliography.**
  `:MepOrgLinkFollow`/`LinkInsert`/`StoreLink`, `:MepOrgFootnoteJump`,
  `:MepOrgBibInsertCitation`/`CiteGoto`/`CitePreview`.
- [x] **12.8 `org-babel` — Executing code blocks.** `C-c C-c`,
  `:MepOrgBabelExecute`/`BabelTangle`, `#+RESULTS:`, `:results` modes,
  `:var` bindings, the per-language table (interpreted and compiled), the
  `mep-lua` block that runs inside mep's own Lua, and `:eval no`.
- [x] **12.9 `org-polyglot` — LSP inside code blocks.** How shadow buffers
  and per-block clients work, and the documented gaps (`:var` invisible to
  the server, no code actions/formatting/symbols bridging, no teardown).
- [x] **12.10 `org-visuals` — Images, LaTeX, tables.**
  `:MepOrgImagesToggle`/`ImageScan` / `<leader>oti`,
  `:MepOrgLatexToggle`/`LatexScan` / `<leader>otl`, `:MepOrgTableAlign`,
  `:MepOrgListIndent`/`Outdent`/`NewItem`/`Renumber`.
- [x] **12.11 `org-export` — Exporting.** `<leader>oe{a,h,m,o,p}` →
  ASCII / HTML / Markdown / ODT / PDF, subtree exports, the generated
  stylesheet and syntax-highlighted code blocks, `doc_export.h`'s
  HTML→LaTeX and HTML→ODT paths.
- [x] **12.12 `org-roam` — Zettelkasten.** `:MepOrgRoamNewNote`/`FindNotes`/
  `InsertLink`/`Backlinks`/`Daily`/`EnsureId`/`Sync`, `<leader>or{a,r,s}`,
  the Backlinks sidebar, `:MepRoamGraph`.

## Part 13 — Markdown and prose (2 pages)

- [ ] **13.1 `markdown` — Markdown.** `:MepMarkdown`, heading colors and
  sign-column glyphs, `:MepMdCheckbox`, fenced-block shading and folding,
  `:MepMdConceal`, front-matter shading, `:MepMdTableAlign`/`TableInsertRow`/
  `TableInsertCol`, and the documented gap (no box-drawn GFM table overlay).
- [ ] **13.2 `writing` — Writing prose in mep.** `gq`/`textwidth`, spell,
  zen mode, soft wrap, the Pomodoro chip, todo clocking for writing sessions.

## Part 14 — Documents, images, media (9 pages)

- [ ] **14.1 `viewers-overview` — What mep can open.** One table: extension
  → viewer/editor → the page that documents it. Covers text, org, markdown,
  html, xml/svg, pdf, docx/odt, xlsx/ods/csv, ipynb, png/jpg/bmp/gif,
  obj/gltf/glb/iqm/vox/m3d/blend, wav.
- [ ] **14.2 `pdf` — The PDF viewer.** Page navigation, `mep.pdf_goto_page`/
  `pdf_current_page`, the outline sidebar (`mep.pdf_outline`), links, text
  extraction and search, encrypted files, `mep.pdf_reload`.
- [ ] **14.3 `office` — Word processor documents.** `.docx` and `.odt`
  rendering and editing, `mep.office_reload`, export back out.
- [ ] **14.4 `sheets` — Spreadsheets.** `.xlsx`/`.ods`/`.csv`, cell
  navigation and editing, the formula engine (`src/formula.cpp`),
  `:MepNextSheet`/`:MepPrevSheet`, `mep.sheet_next`/`sheet_prev`.
- [ ] **14.5 `notebooks` — Jupyter notebooks.** The percent format and
  round-tripping to real nbformat JSON; cell cards and the toolbar; running
  (`Enter`, `Ctrl+Enter`, `C-c C-c`, `Shift+Enter`, `mod1+Enter`); the whole
  `<leader>j` group; `]j`/`[j`; the `:Notebook*` commands; per-cell kernels,
  `mep.opt.notebook_kernels`, the `python`/`protocol`/`script` modes,
  `mep.opt.notebook_python`, inline matplotlib figures, shared `In [n]`
  numbering, ordered run-all.
- [ ] **14.6 `images` — Viewing images.** Zoom, pan, theme-aware background,
  `mep.image_size`/`image_set_theme`/`image_set_nav`, navigating a directory
  of images, and `e` to edit.
- [ ] **14.7 `image-editor` — The image editor.** Every tool and hotkey
  (`b x l r c f i h`, `m o w v`, `[`/`]`, `u`/`Ctrl-R`, `+`/`-`/`=`, Shift-
  to-fill, middle-drag pan); layers panel; color swatches and the HSV
  picker; menubar; selections and `Delete`; `:w` flattening to PNG; the
  procedural generators (wood, turned wood, marble, noise, gradient,
  checkerboard, blur) and when to use them instead of drawing; `Esc` keeping
  session state. Mirror `IMAGE_EDITOR.md`'s "not yet implemented" list.
- [ ] **14.8 `model3d` — The 3D modeler.** Opening `.obj`/`.gltf`/`.glb`/
  `.iqm`/`.vox`/`.m3d`/`.blend` (and the async Blender conversion);
  `:Model3DNew`; camera controls and gizmos; primitives and their pivots;
  lathe objects; materials and textures; lights; the vertex/face editing
  ops (select, move, delete, merge, merge-by-distance, recalculate normals,
  subdivide, extrude, inset, dissolve, add-vertex, make-face, flip-normals,
  list-triangles) and their documented limits; grouping and parenting;
  symmetry (mirror, radial array); animation keyframes; render to image and
  to video; saving.
- [ ] **14.9 `audio-svg` — Audio and vector.** `.wav` waveform view; `.svg`
  and `.xml` rendering; what is view-only.

## Part 15 — Web (3 pages)

- [ ] **15.1 `browser` — The browser pane.** `:Browse` / `<leader>bo`,
  `:BrowseExternal` / `<leader>bO`; the omnibar (`o`, `Ctrl-L`, Enter, Esc);
  `H`/`L`/`r`/`f`/`Ctrl-E`; what loads (http via sockets, https via curl,
  stylesheets, scripts, images, `fetch()` back to origin); the live event
  loop (timers, rAF, promises, async, ES modules); real click and text-input
  handling.
- [ ] **15.2 `local-server` — Serving a directory.** `:Serve` /
  `<leader>bh`, `:ServeStop`, `:Servers`, the 127.0.0.1-only / GET+HEAD /
  no-escape / `no-store` guarantees, `mep.http_serve`/`http_stop`/
  `http_servers`/`http_get`.
- [ ] **15.3 `web-ladder` — Measuring the engine.** `:WebLadder`,
  `:WebLadderRun`, `mep-web-ladder-test` (`--eval`, `--strict`), what each
  of the twelve levels tests, and how to read a failure.

## Part 16 — AI (5 pages)

- [ ] **16.1 `copilot` — GitHub Copilot.** On by default; `Tab`,
  `Alt-Right`, `Alt-Ctrl-Right`, `C-]`; typing-along behaviour and why it
  never fights the completion popup; `:CopilotLogin` device flow and the
  fact mep never sees the token; `:Copilot on/off/toggle/restart/suggest`,
  `:CopilotStatus`, `:CopilotPanel`, `:CopilotLogout`; `copilot_server_cmd`;
  **what gets sent**, `copilot_exclude_patterns`, per-filetype and global
  opt-out.
- [ ] **16.2 `ai-commands` — Ask the model.** `:MepAiSend`/`SendBuffer`/
  `SendSelection`/`ReplaceSelection`/`Cancel`, `<leader>ai` context picker,
  API-key handling (prompted, memory-only).
- [ ] **16.3 `ai-terminal` — Claude Code in a pane.** `<leader>a<CR>` /
  `:aiterminal`; the appended system prompt; `mep.opt.ai_terminal_cmd` /
  `_share` / `_instructions` / `_mcp_server`; registering `mep-mcp` with
  `claude mcp add`.
- [ ] **16.4 `ai-agents` — The AI Agents sidebar.** `<leader>al` / `aa`,
  grouping by workspace, status badges, the OSC-title task row, Enter to
  jump across workspace/tab/pane, `n`/`r`, "outside mep" agents, and how
  pairing works via `$MEP_TERMINAL_BUFFER`.
- [ ] **16.5 `agent-api` — Driving mep from an agent.** A user-facing
  summary of `MEP_AGENT_API.md`: the socket, the tool families (session,
  buffer, cursor, pane, workspace, project, file, command, UI automation,
  image, model), the 0-indexed/end-exclusive conventions, the virtual
  cursor, `mep.participants` and the tab-bar chips, and the rule that agents
  edit through buffer tools rather than the filesystem.

## Part 17 — Learning games (2 pages)

- [ ] **17.1 `learn-decks` — Writing a deck.** `:card:` headlines, the
  deck-wide `#+LEARN_*` keywords, per-card properties (`:CATEGORY:`,
  `:QUESTION:`, `:HINT:`, `:DISTRACTORS:`, `:ALIASES:`, `:HIDE:`,
  `:POINTS:`), `{{answer|hint}}` blanks, ordered lists, 2-column fact
  tables, `:learn cloze`/`:learn bug :line N`/`:learn output` src blocks,
  image cards, and the plain-glossary fallback.
- [ ] **17.2 `learn-games` — Playing.** `:Learn` / `<leader>ol`; the full
  24-game table (command, `<leader>og?` key, data each needs); the shared
  pane keys (digits/letters, `t` to type, `n`/Enter, `o`, `r`, `q`); typed-
  answer matching rules; code anonymization; timed and survival and hot-seat
  modes; the `g` summary key writing org-drill `:DRILL_*:` properties and
  `#+LEARN_SRS:`; `:MepOrgDrillReview`; the reusable Lua building blocks.

## Part 18 — Collaboration (2 pages)

- [ ] **18.1 `collab` — Collaborative editing.** `:CollabJoin <link> [name]`,
  `:CollabStatus`, `:CollabLeave`; participant chips and click-to-jump; the
  CRDT model and what "transient room" means for history.
- [ ] **18.2 `collab-server` — Running `mep-collabd`.** Build and run,
  `MEP_COLLAB_ADMIN_TOKEN`, `MEP_COLLAB_PUBLIC_URL`, the session-creation
  API, TLS termination at Caddy/nginx, `wss://` validation, and the security
  note that the capability link is the credential.

## Part 19 — Configuration (4 pages)

- [ ] **19.1 `config` — `init.lua`.** Where it lives, when it runs (before
  session restore), what a minimal one looks like, `:source`, `:lua`,
  reloading.
- [ ] **19.2 `options` — Options.** Every `:set` option (`number`/`nu`,
  `relativenumber`/`rnu`, `cursorline`/`cul`, `wrap`, `ignorecase`,
  `wrapscan`, `textwidth`/`tw`) with defaults; and every `mep.opt.*` key
  grouped by feature, with its default and effect.
- [ ] **19.3 `keymaps` — Remapping.** `mep.map`, `mep.map_g`,
  `mep.map_g_visual`, `mep.map_bracket_next`/`_prev`, `mep.map_mod1`,
  `mep.leader_map`/`leader_group`, `mep.set_mod1`, `mep.set_leader`,
  `mep.command`; which builtins are plain Lua mappings and therefore
  overridable; `<leader>hk` / `:MepKeymaps` to see what is bound.
- [ ] **19.4 `recipes` — Configuration recipes.** A dozen copy-pasteable
  snippets: disable Copilot, add a formatter, add a notebook kernel, add a
  test runner, add a language UI mode, custom statusline, custom sidebar,
  a `mep-lua` babel block, relocating worktrees.

## Part 20 — Lua API reference (6 pages)

445 bindings. One page per family, each entry with signature, arguments,
return value and a one-line example. Generated from `src/lua_env.cpp`'s
doc comments where possible (see 0.5 — the coverage test should apply here
too).

- [ ] **20.1 `api-editing`** — buffers, lines, cursor, text, registers,
  clipboard, undo, marks, search, folds, decorations, namespaces.
- [ ] **20.2 `api-ui`** — panes, tabs, sidebars, pickers, floats, hovers,
  notifications, statusline, winbar, icons, themes, hints.
- [ ] **20.3 `api-workspace`** — workspaces, projects, files, directories,
  filesystem ops, jobs, processes, env, platform, cwd.
- [ ] **20.4 `api-lang`** — LSP, completion, snippets, syntax/treesitter,
  spell, format, docs, DAP, diff/git gutter.
- [ ] **20.5 `api-docs`** — org, markdown, html/browser, http, pdf, office,
  sheets, notebooks, images, 3D model, learn.
- [ ] **20.6 `api-events`** — `on_frame`, `on_buffer_changed`,
  `on_buffer_saved`, `on_workspace_changed`, `buffer_set_on_key`/`on_enter`/
  `on_write`/`on_image_toggle`, `set_on_directory_open`, the completion and
  inline-suggestion hooks, and the polling rationale.

## Part 21 — Reference and appendices (7 pages)

- [ ] **21.1 `command-index`** — Every ex command (the ~85 native ones plus
  all 207 `:Mep*`/other Lua-defined ones), alphabetical, one line each,
  linked to its page. Generated, not hand-maintained.
- [ ] **21.2 `key-index`** — Every binding: Normal, Insert, Visual,
  Terminal, `<leader>` sequences, `g`-prefixed, `]`/`[` pairs, `mod1`
  layer, and each sidebar's/viewer's own keys. Generated from
  `mep.mapping_descriptions`/`leader_bindings` plus a hand-written table for
  the C++-dispatched keys.
- [ ] **21.3 `filetypes`** — Extension → syntax grammar, LSP server,
  formatter, test runner, run-button entry, viewer. One big table.
- [ ] **21.4 `troubleshooting`** — LSP not attaching, Copilot sign-in,
  formatter not found, notebook kernel missing, Blender conversion, wasm
  build limits, clipboard vanishing on exit, a corrupt session file,
  worktree creation failures, help pages not appearing.
- [ ] **21.5 `platform-notes`** — Linux/X11 vs macOS vs Windows vs wasm:
  what differs (clipboard ownership, native file dialog, `mod1` key,
  `xdg-open`/`open`/`start`, `/proc`-based path resolution, fonts).
- [ ] **21.6 `architecture`** — For contributors: the `src/` layout, the
  raylib-free doc modules, the `kBuiltin*` Lua chunk system and its
  pitfalls (the 64 KB literal limit, trigraphs in Lua-in-C strings), how to
  add a feature, `just test`.
- [x] **21.7 `writing-help`** — Rewrite the existing stub for the final
  system: the Org source convention, `#+HELP_SECTION:`/`#+HELP_ORDER:`,
  the template, running `just help`, adding a page, linking, the freshness
  test, and the project-local `help/` override.
- [ ] **21.8 `glossary`** — buffer, pane, tab, workspace, project, sidebar,
  popout, float, picker, decoration, namespace, hunk, deck, card,
  language UI mode, runner, agent, participant.
- [ ] **21.9 `credits`** — raylib, Lua, treesitter, JetBrains Mono, the
  Nerd Font icon set, and `third_party_licenses/`.

---

## Totals

- Infrastructure items: 7
- Pages: 109 (3 of them rewrites of existing stubs; `shortcuts.html` is superseded by 21.2)

## Suggested order

1. Part 0 (0.1, 0.3, 0.4 first — nothing else is pleasant without them).
2. Part 1, then 21.7 (so the conventions are written down before bulk work).
3. Parts 2–4 (the editing core — highest value per page).
4. Parts 5–8, 19.
5. Parts 9–11, 13.
6. Parts 12, 14, 17 (the large feature areas).
7. Parts 15, 16, 18.
8. Parts 20, 21 (generated/reference; 0.5 should exist by now to keep them honest).

## Final pass

- [ ] **Cross-linking.** Pages written early could not link forward to pages
  that did not exist yet, because `check_help` (rightly) fails on a dangling
  link. Several "See also" entries were dropped for this reason: completion →
  copilot, folding → org-basics, spell → writing, lsp → org-babel,
  repl → notebooks, terminals → ai-terminal,
  r-mode and python-mode → notebooks, org-babel → learn-decks,
  org-roam → learn-games. Once every
  page exists, do one pass adding the links back.

## Open questions

1. **Export pipeline (blocks 0.4).** Add a headless `mep --export-org`
   flag, or drive the existing GUI exporter under Xvfb from a `just help`
   recipe, or drop Org and author the HTML directly? Recommendation:
   the headless flag — it is small, makes the docs testable in CI, and is
   reusable for `:MepOrgExport*` generally.
2. **Contributor docs in the user help?** Parts 20 and 21.6 are developer
   material. Keep them in the same workspace (with their own sidebar
   section), or split them out to `docs/`? Recommendation: same workspace,
   own section — mep's users largely *are* its configurers.
3. **Generated pages.** 21.1, 21.2, 21.3 and most of Part 20 should be
   generated from the source at build time rather than hand-written, which
   means a small generator tool. Worth it at this size; confirm before I
   build it.
