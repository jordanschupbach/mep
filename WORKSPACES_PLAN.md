# Workspaces Plan

Living roadmap for turning mep's flat tab strip into a three-level
hierarchy -- **projects** > **workspaces** > **tabs** -- where each
workspace is backed by its own git worktree, owns its own set of
buffers, and shows up in the top bar as `[project] [ws1] [ws2]` instead
of today's mode label + row of circles. Grown from the entry in
`TODO.org`; written so a new session can pick this up cold: read
**What exists today** and **Design decisions** first, then **Status**
for where things stand, then the relevant phase for what's left in it.

**How to resume:** check the checkboxes below, `git log --oneline` for
what's landed, then continue with the first unchecked item in the
lowest-numbered incomplete phase. Phases are ordered so each one only
depends on earlier ones -- Part I introduces the data model with zero
behavior change, Part II makes cwd/buffers actually per-workspace, Part
III adds git worktrees on top of that, Part IV is the visible UI, Part V
persistence, Part VI agent/MCP surface, Part VII tests and docs. Same
rigor as `WEBKIT_PARITY_PLAN.md`: implement -> `just build-native` ->
verify (Xvfb+xdotool screenshot where a GPU is available; otherwise the
agent RPC `state.dump` and the windowless test binary) -> tick the box
with a short "Verified via..." note -> next phase, pausing for the
user's go-ahead unless told to keep going.

---

## The original TODO (verbatim, from `TODO.org`)

> Add tabs, nested within workspaces, nested within projects feature
> - Need to have new git worktrees per workspace
> - Need to have buffers scoped to workspaces
> - Need to remove mode from top tab bar
> - Need to add project to left side of tab bar formatted like
>   `[project_name]` and tabs formatted like `[ws1] [ws2]` highlighted by
>   active workspace

Decisions taken with the user on 2026-09-03 that shape everything
below:

| Question | Decision |
|---|---|
| Where does a workspace's worktree live, on what branch? | Sibling directory `<parent>/<repo>.worktrees/<ws>`, on a **new branch named after the workspace** created from the current HEAD. The primary checkout is left untouched and is the default `main` workspace. |
| Project isn't a git repo? | Workspaces still work as named tab groups with scoped buffers, all sharing the project root as cwd. Worktree creation is skipped with a status-line message. |
| One project per instance or several? | **Several.** Projects are a real in-memory level; clicking `[project_name]` opens a switcher between loaded projects. |
| Persist across restarts? | **Yes, per project.** Workspace names, worktree paths, tab/pane trees, open files and cursors are saved under `MepDataDir()` and restored when the project is opened. |

---

## What exists today (the Part 0 baseline)

Everything here was verified by reading the code on 2026-09-03; line
numbers will drift, symbol names are the stable handles.

**Tabs and panes.** `Tab` (`src/editor.h:776`) is a two-field struct:
a `unique_ptr<SplitNode> root` and `active_pane_id`. `SplitNode`
(`editor.h:761`) is the recursive split tree, `Pane` (`editor.h:716`)
is one window onto a buffer with its own cursor/scroll/jumplist and a
**per-pane buffer strip** (`Pane::buffer_tabs`) that is distinct from
the top-level tab bar. All tabs live in `Editor::tabs_` with
`active_tab_` as an index (`editor.h:6328`), read through
`TabCount()/ActiveTabIndex()/ActiveTabRoot()/TabRoot(i)/TabActivePaneId(i)`
(`editor.h:1911-1950`) and mutated by `TabNew/TabDelete/TabNext/
TabPrevious/GoToTab` (`editor.cpp:8628-8700`). There is **no
workspace concept anywhere** -- the only "workspace" hits are LSP
`workspace/*` protocol strings.

**Buffers.** `Buffer` (`editor.h:546`) has no tab/workspace/project
field. `Editor::buffers_` is a flat vector and **`buffer_id` is the
index into it** -- the comment at `editor.h:555-568` explains that this
is load-bearing across panes, nine `unordered_map<int, XSession>`
side tables (`terminals_`, `images_`, `pdfs_`, `htmldocs_`, ...,
`editor.h:6288-6322`) and agent-rpc connections, which is why `:bd`
soft-deletes via `Buffer::deleted` instead of erasing. Creation goes
through `FindOrCreateBuffer(path, existed)` (`editor.cpp:4532`, a
linear scan by filename), switching through `BufferNext/BufferPrevious`
(`editor.cpp:8667/8682`), listing through `BufferLabelForLua` and
`mep.buffer_list()` (`lua_env.cpp:2548`), and the `<leader>bb` picker
(`main.cpp:11285`).

**Tab bar.** `DrawTabBar(int y)` (`main.cpp:18851`) is ~110 lines of
immediate-mode drawing with one advancing `x`: the **mode label**
(`main.cpp:18859`, also drawn in the status line at `main.cpp:19108`,
so removing it here loses nothing), the STT mic icon, one Nerd-Font
circle per tab (no labels), `+`/`x` buttons, and participant chips
docked right. Clicks go through `RegisterClickRegion(rect, fn)`.
Height is `TabBarHeight()` (`main.cpp:819`), colors come from the
`TabBar`/`TabActive`/`TabInactive` hl groups (`editor.cpp:812`).

**Projects.** A project today is only a **bookmark**: `projects.json`
in `MepDataDir()` (`editor.cpp:17563`), `ListProjects/AddProject/
RemoveProject`, and the Lua `mep.project_open(dir)` (`main.cpp:2397`)
which literally `mep.chdir(dir)`s and opens the README. **The project
root is the process cwd.** Every picker (`mep.find_files`,
`mep.live_grep`, `mep.todoscan`), the git sidebar's `mep.job_start`
calls (`main.cpp:2606-2624`, no `cwd` option passed), `LspAbspath`,
`:e` relative paths, and agent-rpc `file.open`/`session.info` all
resolve against it.

**Git and jobs.** `JobManager::Spawn(argv, cwd, callbacks, use_pty,
extra_env)` (`job.h:227`) already takes a per-call `cwd` (applied via
`chdir` in the forked child, `job.cpp:39`) and runs callbacks on the
main thread once per frame. Git is called for the gutter
(`GitGutterRefresh`, `editor.cpp:16861`, uses the buffer's own dir as
cwd), hunk staging (`editor.cpp:17037`, inherits cwd) and the Lua
sidebar. **Zero hits for `worktree`.** Terminals spawn with cwd `""`
(inherit) at `editor.cpp:4731`.

**Persistence.** `persist.h` offers `MepDataDir()`, `ReadJsonFile()`,
`WriteJsonFile()` and nothing else; there is **no session/layout
persistence** of any kind. The only tab-tree serializer is the
read-only `SplitNodeJson` used by agent-rpc `state.dump`
(`agent_rpc.cpp:522`).

**Commands.** Ex commands are one long `if/else if` chain in
`ExecuteCommandLine` (tab commands at `editor.cpp:15417`), with a
separate completion list at `editor.cpp:13251`, hard-coded chords in
`HandleTabShortcuts` (`editor.cpp:9241`: `Ctrl-T`, `Alt-1..9`), menu
entries at `main.cpp:11370`, and Lua-registered commands via
`mep.command`/`mep.leader_map`.

**Tests.** Standalone `int main()` binaries in `src/*_test.cpp` with a
local `CHECK()` macro (never `assert()` -- Release strips it, see
`agent_rpc_test.cpp:12`), declared by hand in `CMakeLists.txt`
(`mep-html-doc-test` at `:305` is the windowless model; `mep-agent-rpc-test`
at `:346` drives a real `mep` and needs a display). No ctest wiring.

---

## Design decisions

1. **Nesting is `Project > Workspace > Tab`, held in memory as nested
   vectors.** `Editor::tabs_`/`active_tab_` move into
   `Workspace::tabs`/`active_tab`; `Editor::projects_`/`active_project_`
   and `Project::workspaces`/`active_workspace` sit above. The existing
   `TabCount()/ActiveTabIndex()/TabRoot(i)/...` accessors stay as
   forwarding shims onto the active workspace so the ~40 call sites in
   `editor.cpp`, `main.cpp` and `agent_rpc.cpp` don't change in Phase 1.

2. **Workspaces and projects get stable integer ids, not indices.**
   Tabs can keep index identity (they always have), but a buffer, a
   terminal, a persisted layout and an agent-rpc reply all need to name
   a workspace that may be reordered or closed underneath them.
   `next_workspace_id_` is monotonic per Editor, never reused.

3. **Buffers stay in one global `buffers_` vector; scoping is a field.**
   `Buffer::workspace_id` (`-1` = unscoped: the startup dashboard
   buffer, `:MepScratch`, help/quickfix-style transient buffers). The
   index-as-id invariant is not touched. Every "all buffers" reader
   becomes either *workspace-scoped* (`bnext/bprev`, `:ls`, the
   `<leader>bb` picker, `mep.buffer_list()`, agent `buffer.list`) or
   deliberately *global* (`:wa`, the `:qa` unsaved-changes guard, the
   collab document set) -- the table in Phase 3 says which is which.
   `FindOrCreateBuffer` is keyed by `(workspace_id, canonical path)`, so
   the same out-of-tree file opened in two workspaces is two buffers;
   inside worktrees the paths differ anyway.

4. **The process cwd is always the active workspace's root, and jobs
   also get it explicitly.** Belt and braces: `SwitchWorkspace` does
   `chdir(root)` so every existing cwd-relative code path (pickers,
   `:e`, LSP, sidebar) is correct without being touched, *and* every
   `Spawn`/`job_start`/`term_start` call in the git/terminal paths passes
   `ws.root` as `cwd` so an async job that outlives a workspace switch
   still runs where it was started. `mep.getcwd()` == active workspace
   root is the documented invariant.

5. **Worktree layout is fixed and derived, not chosen per call.**
   `<parent-of-repo>/<repo-basename>.worktrees/<ws-name>` on branch
   `<ws-name>` (Lua-overridable via `mep.opt.worktree_dir` for people
   who want them elsewhere). The primary checkout is the `main`
   workspace: never gets a worktree, can't be deleted, and its branch is
   whatever the user has checked out. Deleting a workspace runs
   `git worktree remove` but **keeps the branch** -- deleting the
   branch is a git decision, not an editor one.

6. **Worktree creation is async and the workspace exists only on
   success.** `git worktree add` can take seconds on a big repo and
   can fail (name collides with a branch, dirty index, not a repo).
   `:wsnew` spawns it through `JobManager`, shows a `[ws creating...]`
   placeholder in the bar, and only materializes the `Workspace` when
   the job exits 0; a non-zero exit becomes a toast and nothing else
   changes. Same discipline as `mep.job_start`-driven pickers.

7. **Existing worktrees are adopted, not duplicated.** On project open
   `git worktree list --porcelain` is parsed; any worktree under the
   derived directory becomes a workspace (name = dir basename), any
   persisted workspace whose worktree is gone is pruned with a message.
   Worktrees elsewhere (created by hand) are listed but left alone
   unless the user `:wsadopt`s them.

8. **Non-git projects are first-class.** `Project::is_git == false`
   means `Workspace::root == Project::root` for every workspace and the
   worktree phases are no-ops. Everything else (scoped buffers, tab
   groups, persistence, UI) works identically, so the UI and model
   phases can be built and tested before git work starts.

9. **One tab bar row.** `[project] [ws1] [ws2*] | ● ○ ○ + x` then
   participant chips on the right. Workspace tabs are text labels
   (`*` suffix when any of that workspace's buffers is modified); the
   tabs *within* the active workspace keep today's circle glyphs after
   a separator, so the existing muscle memory for `+`/`x`/click stays.
   The mode label is simply deleted (it is already in the status line).
   Zen mode hides the whole bar as it does today.

10. **Persistence is per project, keyed by canonical root, written by
    the editor not by git.** `MepDataDir()/workspaces/<slug>-<hash>.json`
    where `<hash>` is a short hash of the canonical project path
    (slug alone would collide for two `~/src/foo` and `~/work/foo`).
    Written debounced on any structural change and unconditionally on
    quit; the JSON shape reuses `SplitNodeJson` so agent-rpc
    `state.dump` and the session file agree. Restore is best-effort:
    files that no longer exist are skipped, an unrestorable workspace
    degrades to one empty tab.

11. **LSP clients are keyed by `(filetype, workspace root)`.** Today one
    client per filetype is "workspace-wide" (`main.cpp:2856`), which is
    wrong the moment two worktrees are open: `rootUri` differs and
    edits in one tree would be reported against the other. This is the
    single most invasive consequence of the feature and gets its own
    phase rather than being hidden inside "thread the cwd through".

12. **Collab relay/CRDT is out of scope for now.** Participants and
    shared documents remain global; a collaborator's location gains a
    `workspace_id` field in `state.dump` but the relay protocol is not
    changed. Revisit once the single-user model has settled.

---

## Status

_All twelve phases implemented 2026-09-03 (one session, uncommitted in
the working tree). Verification so far is build + windowless tests; the
sandbox this was written in has no GPU (raylib's `InitWindow`
segfaults under Xvfb there), so the on-screen and RPC/MCP checks below
are marked **pending a display** and should be run with `just test-gui`
plus a look at the bar on the next machine that has one._

- [x] Part I -- Model (Phases 1-2). Verified via: clean `just
  build-native` with `tabs_`/`active_tab_` gone from `Editor`; every
  former site goes through `Tabs()`/`ActiveTab()`.
- [x] Part II -- Per-workspace cwd and buffers (Phases 3-5). Verified
  via: build; `mep-agent-rpc-test`'s new workspace section checks
  `session.info.cwd` == worktree root, relative `file.open` landing in
  the worktree, and `buffer.list` scoping (pending a display).
- [x] Part III -- Git worktrees (Phases 6-7). Verified via:
  `mep-workspace-test` (porcelain parsing incl. detached/prunable/bare/
  locked, dir derivation incl. overrides, name validation) passes;
  the RPC test creates/deletes a real worktree on a temp repo and checks
  the branch survives (pending a display).
- [x] Part IV -- Tab bar and switchers (Phases 8-9). Verified via:
  build only -- screenshots of 1/2/5 workspaces, the `creating`
  placeholder, the `*` marker, overflow eliding and zen mode are
  pending a display.
- [x] Part V -- Persistence (Phase 10). Verified via: session JSON
  round trip + malformed-file handling in `mep-workspace-test`; the
  quit/relaunch check is pending a display.
- [x] Part VI -- Agent RPC / MCP (Phase 11). Verified via: build;
  `mep-agent-rpc-test` and `mcp/server_test.ts` gained cases (pending a
  display). Deviation: `workspace.create` replies immediately with
  `creating: true` on git projects and the outcome arrives as
  `event.workspaceChanged` / an `event.notify` carrying git's stderr,
  rather than the reply itself being deferred until git exits.
- [x] Part VII -- Tests and docs (Phase 12). Verified via: `just test`
  runs `mep-html-doc-test`, `mep-workspace-test` and the collab tests
  green; README.org "Projects & workspaces" written; TODO.org ticked.

Known gaps / follow-ups (none block the TODO):
- LSP per-root sharing (Phase 5) is verified by reading the Lua only; a
  two-worktree clangd session should be checked by hand.
- Buffer labels still show the path as opened (relative to the
  workspace root by construction) rather than being re-relativised.
- `Ctrl-Shift-T` / `Ctrl-Alt-[`/`]` delivery through raylib is
  unverified; the `<leader>w*` maps cover the same actions.

Suggested order if you want something visible early: 1 -> 2 -> 8 (bar
with a single `main` workspace already looks right) -> 3 -> 4 -> 6 ->
7 -> 9 -> 5 -> 10 -> 11 -> 12. The numbered order below is the
dependency order.

---

## Part I -- Model (no behavior change)

### Phase 1 -- `Workspace` and `Project` structs, forwarding accessors

Goal: after this phase mep behaves byte-for-byte as before, but
`tabs_` no longer exists on `Editor`.

- [x] Add to `editor.h` next to `Tab`:
  ```cpp
  struct Workspace {
      int id = 0;                 // stable, from next_workspace_id_
      std::string name;           // "main", "feat-login"
      std::string root;           // absolute; worktree path or project root
      std::string branch;         // "" when not git-backed
      bool primary = false;       // the checkout itself; undeletable
      bool creating = false;      // Phase 6 placeholder state
      std::vector<Tab> tabs;
      int active_tab = 0;
      int next_pane_id = 0;       // moved from Editor (pane ids per workspace)
  };
  struct Project {
      int id = 0;
      std::string name;           // basename(root)
      std::string root;           // std::filesystem::canonical
      bool is_git = false;
      std::string git_toplevel;   // Phase 6; == root normally
      std::vector<Workspace> workspaces;
      int active_workspace = 0;
  };
  ```
  Decide whether `next_pane_id` stays global (simpler, ids unique
  across the editor) or per workspace -- recommend **global**, since
  agent-rpc `pane.get` and `pane.focus` take bare pane ids.
- [x] Replace `Editor::tabs_/active_tab_` with `projects_`,
  `active_project_`, `next_workspace_id_`, `next_project_id_`. Add
  `ActiveProject()`, `ActiveWorkspace()`, `MutableActiveWorkspace()`,
  `FindWorkspace(id)`, `WorkspaceCount()`, `ActiveWorkspaceIndex()`.
- [x] Rewrite `TabCount/ActiveTabIndex/ActiveTabRoot/MutableActiveTabRoot/
  ActivePaneId/TabRoot(i)/TabActivePaneId(i)` as one-liners on
  `ActiveWorkspace()`. Audit every direct `tabs_` / `active_tab_` touch
  in `editor.cpp` (`TabNew/TabDelete/TabNext/TabPrevious/GoToTab/
  ApplyLayout/ComputeRects/SplitCurrentPane/ClosePane`, `:qa` guard,
  `DropUnusedInitialBuffer`), `main.cpp` (`DrawEditor`, layout math,
  `ShouldShowDashboard`) and `agent_rpc.cpp` (`state.dump`, `pane.get`).
- [x] Bootstrap in the Editor constructor / `main()` startup: one
  `Project{root = canonical(cwd), name = basename}` with one
  `Workspace{name = "main", root = project.root, primary = true}`
  holding the initial tab. `mep file.txt` keeps cwd as the project
  root (a file outside the root is fine -- see decision 3).
- [x] Verify: build, open files, split, `:tabnew`, `:tabdelete`,
  `Alt-N`, `:layout spiral`, `state.dump` over agent-rpc shows the same
  `tabs[]` as before. Run `mep-agent-rpc-test` if a display is
  available.

### Phase 2 -- Workspace lifecycle primitives (in-memory only)

- [x] `Editor::WorkspaceNew(name, root, branch)` -> creates the struct
  with one empty tab, returns id. `WorkspaceSwitch(id)`,
  `WorkspaceNext/Previous`, `WorkspaceDelete(id, force)`,
  `WorkspaceRename(id, name)`. Name validation: non-empty, unique
  within the project, `[A-Za-z0-9._-]+` (it becomes a branch name and
  a directory name in Phase 6).
- [x] `WorkspaceDelete` refuses if the workspace has modified buffers
  unless `force`, refuses always for `primary`, and on success closes
  every pane, soft-deletes the workspace's buffers (Phase 4 makes that
  meaningful), kills its terminals (`terminals_` entries whose buffer
  belongs to it), and activates the nearest neighbour.
- [x] `WorkspaceSwitch` does the `chdir(root)` from decision 4 (a no-op
  until Phase 6 gives workspaces distinct roots) and fires a Lua hook
  `mep.on_workspace_changed` so user config can react.
- [x] Ex commands in `ExecuteCommandLine` + completion list:
  `:wsnew <name>`, `:wsdelete[!] [name]` / `:wsclose`, `:wsnext` /
  `:wsn`, `:wsprevious` / `:wsp`, `:wsrename <name>`, `:ws <name|N>`,
  `:wslist` (opens the picker from Phase 9). Chords in
  `HandleTabShortcuts`: `Ctrl-Shift-T` new workspace prompt,
  `Ctrl-Alt-]`/`Ctrl-Alt-[` next/previous (verify raylib delivers these;
  fall back to `<leader>w` leader maps if not).
- [x] Lua API in `lua_env.cpp`: `mep.workspace_list()`,
  `mep.workspace_current()`, `mep.workspace_new(name)`,
  `mep.workspace_switch(id|name)`, `mep.workspace_delete(id|name, force)`,
  `mep.workspace_rename(...)`. Menu entries beside "New Tab"/"Next
  Tab" in `main.cpp:11370`; help text next to the tab lines at
  `main.cpp:1964`.
- [x] Verify: `:wsnew a`, `:wsnew b`, `:wsn`, split panes in each,
  `:wsdelete a`, `state.dump` (still flat until Phase 11 -- fine).

## Part II -- Per-workspace cwd and buffers

### Phase 3 -- Root-aware jobs, pickers and terminals

Even before worktrees exist this makes the code honest about *which*
root it means.

- [x] Add `Editor::ActiveRoot()` returning `ActiveWorkspace().root`, and
  Lua `mep.workspace_root()`.
- [x] Terminals: `editor.cpp:4731` passes `ws.root` instead of `""`,
  and adds `MEP_WORKSPACE=<name>` and `MEP_PROJECT=<name>` to
  `extra_env` (handy for shell prompts).
- [x] Git: `GitStageHunk`'s `Spawn(..., "", ...)` -> `ws.root`;
  `GitGutterRefresh` already uses the buffer's dir, leave it. The Lua
  git sidebar's four `mep.job_start` calls (`main.cpp:2606-2624`) gain
  `cwd = mep.workspace_root()`.
- [x] Pickers: `mep.find_files`, `mep.live_grep`, `mep.todoscan` and
  the `dir`-parameterised variant at `main.cpp:11304` pass
  `cwd = mep.workspace_root()` explicitly rather than `'.'`. Exclude
  `*.worktrees` from `rg` globs so a sibling worktree dir never leaks
  into a project's own file list (matters when `worktree_dir` is set
  inside the repo).
- [x] `mep.project_open(dir)` stops being "chdir + readme" -- it
  becomes `mep.project_load(dir)` (Phase 9) followed by the existing
  readme/terminal layout applied inside the project's `main` workspace.
- [x] Verify: `:wsnew x`, `:terminal`, `pwd` in the terminal prints the
  workspace root (identical to project root until Phase 6);
  `<leader>ff` lists project files.

### Phase 4 -- Buffer scoping

- [x] `Buffer::workspace_id = -1`. `FindOrCreateBuffer(path, existed)`
  gains a `workspace_id` parameter (default: active) and matches on
  `(workspace_id, filename)`; `CreateEmptyBuffer` tags with the active
  workspace; `:MepScratch`'s find-or-create and the dashboard buffer
  stay `-1`.
- [x] Scoped vs global readers -- audit each site the exploration found
  (`editor.cpp:4519, 4845, 5138, 5421, 5788, 6783, 15377, 17798`,
  `BufferNext/BufferPrevious`, `BufferLabelForLua`,
  `PaneBuffersInActiveTab`, `mep.buffer_list()`, `<leader>bb`):

  | Reader | Scope | Why |
  |---|---|---|
  | `:bnext/:bprev`, `Pane::buffer_tabs` cycling | workspace | what the TODO asks for |
  | `:ls`, `mep.buffer_list()`, `<leader>bb`, agent `buffer.list` | workspace (with an `all=true` opt) | same |
  | `:b <name>` fuzzy match | workspace first, global fallback | least surprise |
  | `:wa`, `:qa` guard, `:xa` | **global** | never lose a modified buffer in a hidden workspace |
  | collab document set, LSP didOpen bookkeeping | global (per Phase 5 for LSP) | protocol-level |
  | `GitGutterRefresh` loop | workspace | avoids hammering git on hidden trees |

- [x] `WorkspaceDelete` now soft-deletes the workspace's buffers
  (`Buffer::deleted = true`), same mechanism as `:bd`, so ids stay
  stable and `FindOrCreateBuffer` can revive them if the workspace is
  recreated with the same root.
- [x] Buffer label (`BufferLabelForLua`, status line) shows the path
  relative to the workspace root, not the process cwd -- they're the
  same after decision 4 but writing it that way documents intent.
- [x] Verify: open `a.txt` in ws1, `:wsnew ws2`, `:ls` is empty, `:bn`
  does nothing, open `b.txt`, `:ws main` -> `:ls` shows only `a.txt`;
  modify `b.txt`, `:qa` refuses; `:wa` from `main` saves it.

### Phase 5 -- LSP per workspace root

- [x] Key the `filetype -> client` map (`main.cpp:2856`) by
  `(filetype, root)`. Client start passes `rootUri = file://<ws.root>`;
  `LspAbspath` (`editor.cpp:1019`) resolves against the buffer's
  workspace root.
- [x] Buffer -> client lookup goes through `Buffer::workspace_id`, so a
  hidden workspace's diagnostics keep arriving and are stored per
  buffer as today.
- [x] Shutting a workspace shuts its clients when no other workspace
  shares the same root (non-git projects share one root, so the
  refcount matters).
- [x] Verify with a language server that reports workspace-relative
  diagnostics (clangd on this repo: two worktrees, edit one, confirm
  the other's diagnostics don't change).

## Part III -- Git worktrees

### Phase 6 -- Repo detection and worktree creation

- [x] Small pure module `src/workspace_git.h/.cpp` (no Editor, no
  raylib -- so Phase 12 can unit-test it):
  `DeriveWorktreeDir(project_root, ws_name, override)`,
  `ParseWorktreeList(porcelain_text) -> vector<{path, branch, head}>`,
  `ValidWorkspaceName(name)`, `ProjectSlug(root)`/`ProjectHash(root)`.
- [x] On project load: async `git rev-parse --show-toplevel` (cwd =
  root). Exit 0 -> `is_git = true`, `git_toplevel = output`; then
  `git worktree list --porcelain` -> adopt per decision 7. The `main`
  workspace's `branch` comes from `git rev-parse --abbrev-ref HEAD`
  (refresh it lazily on workspace switch and after the git sidebar
  runs a checkout; it's display-only).
- [x] `:wsnew <name>` on a git project: mark a placeholder
  `Workspace{creating = true}` (drawn dimmed, unswitchable), spawn
  `git worktree add -b <name> <dir> HEAD` with cwd = `git_toplevel`.
  On exit 0 -> fill `root`/`branch`, clear `creating`, switch to it,
  status-line "workspace <name> on branch <name>". On failure ->
  remove the placeholder, toast with git's stderr (branch already
  exists is the common one: offer `:wsnew! <name>` which uses
  `git worktree add <dir> <name>` to attach to the existing branch).
- [x] `:wsdelete[!] <name>`: `git worktree remove [--force] <dir>`,
  then the in-memory delete from Phase 2. Without `!` git itself
  refuses on a dirty tree -- surface its message verbatim.
- [x] `:wsadopt <path-or-branch>` for hand-made worktrees; `:wsprune`
  runs `git worktree prune` and drops workspaces whose dir vanished.
- [x] Verify on this repo: `:wsnew scratch` -> `../mep.worktrees/scratch`
  exists, `git worktree list` shows it, terminal `pwd` is inside it,
  `<leader>ff` lists its files, `:wsdelete scratch` removes the dir and
  `git branch` still lists `scratch`.

### Phase 7 -- Worktree-aware git UI

- [x] Git sidebar header shows `branch @ workspace`; the status list is
  for the active workspace's tree (already true once Phase 3's `cwd`
  lands, this is just the label).
- [x] Workspace label `*` dirty marker (decision 9) is computed from
  modified buffers only, not `git status`, to avoid spawning git per
  frame. An optional `mep.opt.workspace_git_dirty = true` runs a
  debounced `git status --porcelain` per workspace and shows `+` for
  uncommitted changes.
- [x] `<leader>gw` picker: list of workspaces with branch, ahead/behind
  (from a single `git for-each-ref --format` call), dirty flag; enter
  switches, `d` deletes with confirm, `n` prompts for a new one.

## Part IV -- Tab bar and switchers

### Phase 8 -- Tab bar rewrite

Rewrites `main.cpp:18857-18902` only; participant chips stay.

- [x] Delete the mode label block (`main.cpp:18859-18861`). Confirm the
  status line still shows `-- INSERT --` etc.
- [x] Draw `[<project.name>]` in a new `ProjectLabel` hl group (add to
  `BuildHighlightGroups` beside `TabBar`, default = `TabActive` bold).
  Click -> project switcher (Phase 9). Long names ellipsised to ~24
  chars.
- [x] For each workspace draw `[<name>]` (`[<name>*]` when dirty,
  `[<name>...]` dimmed when `creating`) using `WorkspaceActive` /
  `WorkspaceInactive` hl groups (default to `TabActive`/`TabInactive`
  so existing themes keep working). Click -> `WorkspaceSwitch(id)`,
  middle-click -> `:wsdelete`, hover tooltip shows `root` and `branch`
  (reuse the participant-chip tooltip idiom in the same function).
- [x] Separator glyph, then the existing per-tab circles, `+`, `x` --
  unchanged code, just a later starting `x`.
- [x] Overflow: if workspace labels + tab circles exceed
  `screen_w - chips_w`, elide inactive workspace labels to their first
  3 chars, then to a `+N` count. Never let the bar wrap or the body
  scroll.
- [x] STT mic icon moves to just left of the participant chips so it
  doesn't fight the project label for the left edge.
- [x] Update README.org's tab-bar paragraph (around line 171) and the
  Lua UI docs (line ~245, "recursively draws each tab's split-pane
  tree").
- [x] Verify: screenshot with 1, 2, 5 workspaces; a creating
  placeholder; a dirty marker; zen mode hides the bar; the
  `TabActive`-only themes still render.

### Phase 9 -- Multiple projects and the switchers

- [x] `Editor::ProjectLoad(root)` -> canonicalise, dedupe by root
  (switch instead of reload), create the `main` workspace, kick off
  Phase 6 detection, restore Phase 10 state if present, `chdir`.
  `ProjectSwitch(id)`, `ProjectClose(id, force)` (closes every
  workspace with the Phase 2 rules), `ProjectNext/Previous`.
- [x] `mep.project_open(dir)` = `ProjectLoad` + the readme/terminal
  layout only when the project had no saved state. `mep.projects()`
  picker (the bookmark list) opens/switches; a new `mep.projects_open()`
  picker lists *loaded* projects with their workspace counts -- this is
  what clicking `[project_name]` opens. `:project <dir>`,
  `:projectclose[!]`, `:projectnext/prev`.
- [x] Startup: `mep` -> load cwd; `mep --project <dir>` (and a `MEP_PROJECT`
  env var for the launcher) loads that; `mep <file>` loads cwd and
  opens the file in `main`. Add the flag to the usage text.
- [x] Verify: load two projects, switch, each keeps its own workspaces,
  `<leader>ff` lists the right tree, closing one with a modified buffer
  refuses without `!`.

## Part V -- Persistence

### Phase 10 -- Save and restore per project

- [x] Schema (v1) written to `MepDataDir()/workspaces/<slug>-<hash>.json`:
  ```json
  {"version":1,"root":"/abs/project","active_workspace":"feat-x",
   "workspaces":[{"name":"main","root":"/abs/project","branch":"main",
     "primary":true,"active_tab":0,
     "tabs":[{"active_pane":3,"root":{...SplitNodeJson with pane:{buffer:"rel/path",cursor:[r,c],scroll:n,buffer_tabs:["a","b"]}}}]}]}
  ```
  Buffers are referenced by path relative to the *workspace* root so a
  moved repo still restores. Terminals, images, PDFs and other
  non-file buffers serialise as `{"kind":"terminal"}` and come back as
  a fresh instance of that kind (a terminal in the same pane, cwd
  restored) rather than being skipped, so layouts survive.
- [x] `Editor::SaveWorkspaceState(project)` -- debounced 500 ms after
  any structural change (workspace/tab/pane create/close/switch,
  buffer open/close, cursor moves are *not* triggers; the cursor is
  read at save time) plus on `:q*`/window close/`ProjectClose`.
  `RestoreWorkspaceState(project)` on `ProjectLoad`.
- [x] Restore is best-effort per decision 10: missing file -> pane gets
  an empty buffer and a one-line message summarising `N files
  skipped`; worktree dir gone -> workspace pruned (Phase 6
  `:wsprune` logic); malformed JSON -> ignored with a message and
  rewritten on next save. Never abort startup on a bad session file.
- [x] `--no-session` flag / `mep.opt.restore_workspaces = false` to
  skip restore; `:wssave` / `:wsrestore` for manual control.
- [x] Verify: build a two-workspace layout, quit, relaunch, layout and
  cursors are back; delete a file on disk, relaunch, message appears
  and everything else restores; corrupt the JSON by hand, relaunch
  cleanly.

## Part VI -- Agent RPC / MCP

### Phase 11 -- Workspace-aware endpoints

- [x] `state.dump` (`agent_rpc.cpp:522`): add
  `projects[] -> workspaces[] -> tabs[]` with ids/names/roots/branches,
  keep the top-level `tabs`/`active_tab` as the active workspace's for
  one release (mark deprecated in the reply) so existing agent code
  keeps working.
- [x] `buffer.list` (`:548`): filter to the active workspace; accept
  `{"workspace": id|"all"}`; each entry gains `workspace_id`.
  `session.info` (`:510`): add `project`, `workspace`, `workspace_root`;
  `cwd` stays (== root). `pane.get` (`:539`): search all workspaces,
  return `workspace_id` alongside. `file.open`: relative paths resolve
  against the active workspace root.
- [x] New methods: `workspace.list`, `workspace.switch {id|name}`,
  `workspace.create {name}` (async: replies once git finishes, with
  the same error text the toast shows), `workspace.delete {id, force}`,
  `project.list`, `project.switch`, `project.open {root}`. Events on
  `mep_poll_events`: `workspace_changed`, `project_changed`.
- [x] MCP wrappers in `mcp/server.ts` + `mcp/mep_client.ts`:
  `mep_workspace_list/switch/create/delete`, `mep_project_list/switch/open`;
  `mep_buffer_list` gains the `workspace` arg. `mcp/server_test.ts`
  cases for the new tools.
- [x] `agent_rpc_test.cpp`: create a workspace over RPC on a temp git
  repo, confirm `state.dump` nesting, `buffer.list` scoping, and that
  `file.open` of a relative path lands in the worktree.

## Part VII -- Tests and docs

### Phase 12 -- Windowless unit tests, README, TODO

- [x] `src/workspace_test.cpp` + `mep-workspace-test` target in
  `CMakeLists.txt` (mirror `mep-html-doc-test`, link only
  `workspace_git.cpp` and `persist.h`-level code): worktree dir
  derivation incl. the override, porcelain parsing (attached,
  detached, prunable, bare entries), name validation, session JSON
  round trip, restore of a file with a missing path.
- [x] Add a `just test` recipe that builds and runs every `*-test`
  binary that doesn't need a display, and a `just test-gui` for the
  ones that do (documented as needing Xvfb + a GPU-capable driver --
  the sandbox used for this plan segfaults in `InitWindow`).
- [x] README.org: new "Projects & workspaces" section after "Windows &
  tabs" covering the commands, the worktree layout, non-git behaviour,
  persistence location, and `mep.opt.worktree_dir`.
- [x] Replace the `TODO.org` entry with a pointer to this file and tick
  it when Part IV lands (the visible deliverable).

---

## Risks and things to watch

- **The buffer-index invariant.** Any temptation to "just erase the
  workspace's buffers" on delete must be resisted; use `deleted`. The
  nine side tables in `editor.h:6288-6322` are the sites that break.
- **Global cwd during async jobs.** Decision 4's explicit `cwd` on
  every spawn is what makes a workspace switch mid-`rg` safe. New
  `job_start` call sites added later must pass it too -- consider
  making `mep.job_start` default `cwd` to `mep.workspace_root()` rather
  than the process cwd so the safe thing is the default.
- **Worktree name == branch name.** A workspace called `main` on a
  repo whose default branch is `main` would collide; `:wsnew` should
  refuse names that match an existing branch unless `!` (attach
  semantics) is given.
- **`git worktree add` inside a worktree.** Always run it with cwd =
  `git_toplevel` of the *primary* checkout, never the active
  workspace's root, or nested worktree dirs appear.
- **Startup time.** Restore should not block the first frame on git:
  paint the bar from the JSON immediately, then let the async
  detection confirm or prune.
- **Wasm build.** `persist.h` is native-only and there is no git in the
  browser build; every git phase is `#ifndef __EMSCRIPTEN__` and the
  wasm build gets exactly one non-git project. Keep it compiling from
  Phase 1 onward.

## Explicitly out of scope (noted so it isn't re-litigated)

- Collab relay changes (decision 12): shared documents are not
  partitioned by workspace.
- Branch deletion on `:wsdelete`; `git stash`/rebase helpers; PR
  creation. `git worktree` is the whole surface.
- Per-workspace themes, fonts or keymaps.
- Detaching a tab into a separate OS window.
- A workspace spanning multiple repos (monorepo-of-repos); one project
  == one root.
