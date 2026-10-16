#pragma once

// Internal: the write-side commands (add/reset/rm/commit/branch/checkout/
// clean/stash/worktree/merge --ff). Each returns git's exit code and
// writes git-shaped stdout/stderr text.

#include <string>
#include <vector>

#include "git/git.h"

namespace mep::git {

struct Out {
    std::string out, err;
};

int OpAdd(Repo &repo, bool all, const std::vector<std::string> &paths, bool force, Out &o);
int OpRm(Repo &repo, bool cached, bool recursive, bool force, const std::vector<std::string> &paths, Out &o);
int OpReset(Repo &repo, const std::string &rev, const std::vector<std::string> &paths, bool hard, Out &o);
struct CommitOptions {
    std::string message;
    bool have_message = false;
    std::string message_file;
    std::string cleanup = "default";  // strip | whitespace | verbatim | default
    bool amend = false, allow_empty = false, no_verify = false, all = false;
};
int OpCommit(Repo &repo, const CommitOptions &co, Out &o);
int OpBranchCreate(Repo &repo, const std::string &name, const std::string &start, bool force, Out &o);
int OpBranchDelete(Repo &repo, const std::vector<std::string> &names, bool force, Out &o);
int OpCheckout(Repo &repo, const std::string &target, bool detach, Out &o);
int OpCheckoutNewBranch(Repo &repo, const std::string &name, const std::string &start, Out &o);
int OpCheckoutPaths(Repo &repo, const std::string &tree_ish, const std::vector<std::string> &paths, Out &o);
int OpClean(Repo &repo, bool force, bool dirs, bool dry_run, int force_count, const std::vector<std::string> &paths,
            Out &o);
int OpStashPush(Repo &repo, bool include_untracked, const std::string &message, Out &o);
int OpStashApply(Repo &repo, const std::string &ref, bool pop, Out &o, bool *need_cli);
int OpStashDrop(Repo &repo, const std::string &ref, Out &o);
int OpStashShow(Repo &repo, const std::string &ref, bool patch, bool stat, Out &o);
int OpStashList(Repo &repo, const std::string &format, Out &o);
int OpWorktreeAdd(Repo &repo, const std::string &path, const std::string &commitish, const std::string &new_branch,
                  bool detach, bool force, Out &o);
int OpWorktreeList(Repo &repo, bool porcelain, Out &o);
int OpWorktreeRemove(Repo &repo, const std::string &path, int force, Out &o);
int OpWorktreePrune(Repo &repo, Out &o);
// Fast-forward (or already-up-to-date) merge; sets *need_cli for anything else.
int OpMergeFastForward(Repo &repo, const std::string &target, bool ff_only, Out &o, bool *need_cli);

// Long-format `git status` (what `git status` prints with no options).
std::string LongStatus(Repo &repo, const StatusResult &st);

}  // namespace mep::git
