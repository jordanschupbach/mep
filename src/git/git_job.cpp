#include "git/git_job.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "git/git.h"

namespace mep::git {

namespace {
std::atomic<int> g_use_cli{-1};  // -1: not decided yet (read the environment)

void EmitLines(const std::string &text, const Job::EmitLine &emit) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        // Same shape as a process job's line splitting: CRLF loses the CR.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        emit(line);
        pos = nl + 1;
    }
}
}  // namespace

bool InHouseEnabled() {
    int v = g_use_cli.load();
    if (v < 0) {
        const char *e = std::getenv("MEP_GIT_BACKEND");
        v = (e && std::strcmp(e, "cli") == 0) ? 1 : 0;
        g_use_cli.store(v);
    }
    return v == 0;
}

void SetUseCli(bool cli) { g_use_cli.store(cli ? 1 : 0); }

int SpawnGitTask(std::function<int(std::string &out, std::string &err, const std::atomic<bool> &cancelled)> fn,
                 JobManager::Callbacks callbacks, const std::string &debug_name) {
    Job::TaskEx task = [fn = std::move(fn)](const Job::EmitLine &emit_out, const Job::EmitLine &emit_err,
                                            const std::atomic<bool> &cancelled) {
        std::string out, err;
        int code = fn(out, err, cancelled);
        EmitLines(out, emit_out);
        EmitLines(err, emit_err);
        return code;
    };
    return JobManager::Instance().SpawnTaskEx(std::move(task), std::move(callbacks), debug_name);
}

int SpawnInHouse(const std::vector<std::string> &argv, const std::string &cwd, JobManager::Callbacks callbacks) {
    if (!InHouseEnabled() || !IsSupported(argv)) return 0;
    std::string name = "git (in-house)";
    for (size_t i = 1; i < argv.size() && i < 4; i++) name += " " + argv[i];
    return SpawnGitTask(
        [argv, cwd](std::string &out, std::string &err, const std::atomic<bool> &cancelled) {
            int code = RunCommand(argv, cwd, out, err, &cancelled);
            return code == -2 ? 128 : code;
        },
        std::move(callbacks), name);
}

int SpawnGit(const std::vector<std::string> &argv, const std::string &cwd, JobManager::Callbacks callbacks) {
    if (int id = SpawnInHouse(argv, cwd, callbacks)) return id;
    return JobManager::Instance().Spawn(argv, cwd, std::move(callbacks));
}

}  // namespace mep::git
