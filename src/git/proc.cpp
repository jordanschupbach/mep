// A small blocking subprocess runner for hooks and the git-CLI fallbacks
// (non-fast-forward merge, rebase, conflicting stash apply), used from a
// JobManager worker thread -- never the UI thread.

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "git/git.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace mep::git {

int RunProcess(const std::vector<std::string> &argv, const std::string &cwd,
               const std::vector<std::pair<std::string, std::string>> &env, std::string *out, std::string *err,
               const std::string *stdin_data) {
#if defined(_WIN32)
    (void)argv;
    (void)cwd;
    (void)env;
    (void)out;
    (void)err;
    (void)stdin_data;
    return -1;
#else
    if (argv.empty()) return -1;
    int out_pipe[2], err_pipe[2], in_pipe[2];
    if (::pipe(out_pipe) != 0) return -1;
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return -1;
    }
    if (::pipe(in_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return -1;
    }
    for (int fd : {out_pipe[0], err_pipe[0], in_pipe[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
    posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
    // posix_spawn has no portable chdir action; run through a tiny shell
    // wrapper only for the directory change, exec'ing the real program.
    std::vector<std::string> args;
    if (!cwd.empty()) {
        args = {"/bin/sh", "-c", "cd \"$1\" && shift && exec \"$@\"", "sh", cwd};
        args.insert(args.end(), argv.begin(), argv.end());
    } else {
        args = argv;
    }
    std::vector<char *> cargv;
    for (std::string &a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    std::vector<std::string> envs;
    for (char **e = environ; *e; ++e) {
        std::string s = *e;
        bool overridden = false;
        for (const auto &kv : env)
            if (s.compare(0, kv.first.size() + 1, kv.first + "=") == 0) overridden = true;
        if (!overridden) envs.push_back(s);
    }
    for (const auto &kv : env) envs.push_back(kv.first + "=" + kv.second);
    std::vector<char *> cenv;
    for (std::string &s : envs) cenv.push_back(s.data());
    cenv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), cenv.data());
    posix_spawn_file_actions_destroy(&fa);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        return -1;
    }
    if (stdin_data && !stdin_data->empty()) {
        size_t done = 0;
        while (done < stdin_data->size()) {
            ssize_t n = ::write(in_pipe[1], stdin_data->data() + done, stdin_data->size() - done);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            done += static_cast<size_t>(n);
        }
    }
    ::close(in_pipe[1]);
    struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    int open_fds = 2;
    char buf[65536];
    while (open_fds > 0) {
        int n = ::poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t r = ::read(fds[i].fd, buf, sizeof buf);
            if (r <= 0) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                open_fds--;
                continue;
            }
            std::string *dst = i == 0 ? out : err;
            if (dst) dst->append(buf, static_cast<size_t>(r));
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        // 126/127 from the cd-wrapper's exec means the program couldn't run.
        if (code == 126 || code == 127) return -1;
        return code;
    }
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

}  // namespace mep::git
