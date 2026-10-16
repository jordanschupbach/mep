#pragma once

// Runs mep::git (the in-house git) as a JobManager in-process task, so a
// git command started from the UI thread -- mep.job_start({'git', ...}),
// mep.git.run, the editor's workspace/gutter code -- behaves exactly like
// the CLI job it replaces: lines arrive through on_stdout/on_stderr and
// the exit code through on_exit, all from JobManager::PollAll, while the
// work happens on a worker thread.
//
// MEP_GIT_BACKEND=cli in the environment (or mep::git::SetUseCli(true),
// the Lua `mep.git.use_cli`) routes everything to the git CLI again: the
// fallback DEPENDENCY_REMOVAL_PLAN.md keeps until the in-house path has
// shipped for a while.

#include <string>
#include <vector>

#include "job.h"

namespace mep::git {

bool InHouseEnabled();
void SetUseCli(bool cli);

// Starts `argv` (argv[0] == "git") in `cwd`: in-house when the command is
// in the supported subset, otherwise the git CLI. Returns the job id (0 if
// even the CLI could not be spawned).
int SpawnGit(const std::vector<std::string> &argv, const std::string &cwd, JobManager::Callbacks callbacks);

// Only the in-house path: returns 0 without spawning anything when `argv`
// isn't supported in-house (the caller decides what to do then).
int SpawnInHouse(const std::vector<std::string> &argv, const std::string &cwd, JobManager::Callbacks callbacks);

// Runs `fn` on a worker thread; its stdout/stderr text is split into lines
// for the callbacks, its return value is the exit code.
int SpawnGitTask(std::function<int(std::string &out, std::string &err, const std::atomic<bool> &cancelled)> fn,
                 JobManager::Callbacks callbacks, const std::string &debug_name);

}  // namespace mep::git
