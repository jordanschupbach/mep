#ifndef MEP_WORKSPACE_GIT_H
#define MEP_WORKSPACE_GIT_H

// Pure helpers behind WORKSPACES_PLAN.md's git-worktree-backed workspaces:
// no Editor, no raylib, no process spawning -- just string/path logic, so
// mep-workspace-test (src/workspace_test.cpp) can exercise every branch
// without a display or a real repo. The Editor-side code that actually
// spawns `git worktree ...` through JobManager lives in editor.cpp.

#include <string>
#include <vector>

class Json;  // json.h

// One entry of `git worktree list --porcelain`.
struct WorktreeEntry {
    std::string path;    // absolute worktree path ("worktree <path>")
    std::string head;    // commit sha ("HEAD <sha>"), "" for a bare entry
    std::string branch;  // short branch name ("branch refs/heads/x" -> "x"), "" when detached/bare
    bool bare = false;
    bool detached = false;
    bool prunable = false;
    bool locked = false;
};

// A workspace name doubles as a branch name and a directory name (decision
// 5), so it's restricted to [A-Za-z0-9._-]+, must not start with '.' or
// '-' (git refuses both as branch names; '-' also reads as a flag), and
// must not end in ".lock" or contain "..".
bool ValidWorkspaceName(const std::string &name);

// `<parent-of-repo>/<repo-basename>.worktrees/<ws-name>`, or
// `<override>/<ws-name>` when `override_dir` (mep.opt.worktree_dir) is
// non-empty -- a relative override is taken relative to `project_root`.
// Never normalises symlinks; callers pass an already-canonical root.
std::string DeriveWorktreeDir(const std::string &project_root, const std::string &ws_name,
                              const std::string &override_dir = "");

// Parses the blank-line-separated stanzas of `git worktree list
// --porcelain`. Unknown attribute lines are ignored; a stanza without a
// `worktree` line is dropped.
std::vector<WorktreeEntry> ParseWorktreeList(const std::string &porcelain_text);

// Filesystem-safe basename of a project root ("mep" for "/home/x/src/mep",
// "root" for "/"), lowercase, non-[a-z0-9_-] runs collapsed to '-'.
std::string ProjectSlug(const std::string &root);

// Short (8 hex chars) stable hash of the canonical project path, so two
// projects that share a basename get distinct state files (decision 10).
std::string ProjectHash(const std::string &root);

// `<data_dir>/workspaces/<slug>-<hash>.json` -- the per-project session
// file location (Phase 10). Does not create the directory.
std::string WorkspaceStatePath(const std::string &data_dir, const std::string &root);

// Shape check for one saved split-tree node (the `tabs[].root` written by
// Editor::SplitStateJson): a leaf carries a `pane` object with a known
// `kind`, and a split carries a non-empty `children` array. A `shares` entry
// only has to *be* an array -- a size that doesn't match `children` is what a
// save taken between a split and the next resize legitimately holds (see
// SplitNode::shares), so it is dropped at rebuild time, not rejected here.
// Checked up front, before any of
// the tree is rebuilt, so a truncated or hand-edited session file makes
// Editor::RestoreWorkspaceLayout decline the whole workspace and fall back
// to the default layout rather than leaving a half-built one on screen.
// Pure (no Editor, no filesystem) so workspace_test can cover the branches
// a real session file never reaches.
bool ValidWorkspaceStateTree(const Json &node);

// The same check for a whole workspace's `tabs` array: non-empty, and
// every entry an object whose `root` passes ValidWorkspaceStateTree.
bool ValidWorkspaceStateTabs(const Json &tabs);

#endif  // MEP_WORKSPACE_GIT_H
