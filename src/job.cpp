#include "job.h"

#include <algorithm>
#include <chrono>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#define MEP_JOB_POSIX 1
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

Job::Job(const std::vector<std::string> &argv, const std::string &cwd, bool raw_stdout, bool use_pty,
         std::vector<std::pair<std::string, std::string>> extra_env)
    : raw_stdout_(raw_stdout || use_pty), use_pty_(use_pty), extra_env_(std::move(extra_env)) {
#if MEP_JOB_POSIX
    if (argv.empty()) {
        spawn_failed_ = true;
        finished_ = true;
        return;
    }
    if (use_pty_) {
        int master_fd = -1;
        pid_t pid = forkpty(&master_fd, nullptr, nullptr, nullptr);
        if (pid < 0) {
            spawn_failed_ = true;
            finished_ = true;
            return;
        }
        if (pid == 0) {
            // Child: forkpty() already made the PTY slave our controlling
            // terminal and stdin/stdout/stderr -- just cwd + exec.
            setpgid(0, 0);
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
            for (const auto &kv : extra_env_) setenv(kv.first.c_str(), kv.second.c_str(), 1);
            std::vector<char *> cargv;
            cargv.reserve(argv.size() + 1);
            for (const auto &s : argv) cargv.push_back(const_cast<char *>(s.c_str()));
            cargv.push_back(nullptr);
            execvp(cargv[0], cargv.data());
            _exit(127);
        }
        // Parent: one fd serves as stdin (write) and the merged
        // stdout+stderr (read) -- a real PTY has no separate stderr
        // stream, matching what a terminal emulator would see.
        pid_ = pid;
        stdin_fd_ = stdout_fd_ = master_fd;
        stderr_fd_ = -1;
        reader_thread_ = std::thread(&Job::ReaderLoop, this);
        return;
    }
    int in_pipe[2], out_pipe[2], err_pipe[2];
    if (pipe(in_pipe) != 0) {
        spawn_failed_ = true;
        finished_ = true;
        return;
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        spawn_failed_ = true;
        finished_ = true;
        return;
    }
    if (pipe(err_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        spawn_failed_ = true;
        finished_ = true;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        spawn_failed_ = true;
        finished_ = true;
        return;
    }
    if (pid == 0) {
        // Child: wire pipes to std streams, cwd, exec. Any failure here
        // exits 127 (standard "command not found/exec failed" code) --
        // the parent sees that via waitpid, not via a shared error channel.
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        setpgid(0, 0);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        for (const auto &kv : extra_env_) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        std::vector<char *> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto &s : argv) cargv.push_back(const_cast<char *>(s.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    // Parent.
    pid_ = pid;
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    stderr_fd_ = err_pipe[0];
    reader_thread_ = std::thread(&Job::ReaderLoop, this);
#else
    (void)argv;
    (void)cwd;
    spawn_failed_ = true;
    finished_ = true;
#endif
}

Job::~Job() {
#if MEP_JOB_POSIX
    if (!finished_.load()) Kill();
    if (reader_thread_.joinable()) reader_thread_.join();
    if (stdin_fd_ >= 0) close(stdin_fd_);
#endif
}

void Job::Kill() {
#if MEP_JOB_POSIX
    if (pid_ > 0 && !finished_.load()) {
        killed_ = true;
        kill(-pid_, SIGTERM);
    }
#endif
}

void Job::KillHard() {
#if MEP_JOB_POSIX
    if (pid_ > 0 && !finished_.load()) {
        killed_ = true;
        kill(-pid_, SIGKILL);
    }
#endif
}

bool Job::WriteStdin(const std::string &data) {
#if MEP_JOB_POSIX
    if (stdin_fd_ < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(stdin_fd_, data.data() + off, data.size() - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
#else
    (void)data;
    return false;
#endif
}

void Job::CloseStdin() {
#if MEP_JOB_POSIX
    // A PTY's stdin/stdout share one fd -- closing it would kill the read
    // side too, which makes no sense for an interactive terminal (unlike
    // a one-shot pipe job like `git apply`, nothing here ever wants to
    // signal EOF to a shell by closing its input).
    if (use_pty_) return;
    if (stdin_fd_ >= 0) {
        close(stdin_fd_);
        stdin_fd_ = -1;
    }
#endif
}

void Job::ResizePty(int cols, int rows) {
#if MEP_JOB_POSIX
    if (!use_pty_ || stdout_fd_ < 0) return;
    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(std::max(1, cols));
    ws.ws_row = static_cast<unsigned short>(std::max(1, rows));
    ioctl(stdout_fd_, TIOCSWINSZ, &ws);
#endif
}

std::vector<JobLine> Job::DrainLines() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<JobLine> out(pending_.begin(), pending_.end());
    pending_.clear();
    return out;
}

std::vector<std::string> Job::DrainRaw() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out(pending_raw_.begin(), pending_raw_.end());
    pending_raw_.clear();
    return out;
}

#if MEP_JOB_POSIX
namespace {
/**
 * @brief Appends `chunk` to `partial`, splitting on '\n' (stripping a trailing
 * '\r' for CRLF-emitting tools) and pushing each complete line into
 * `pending` under `mu`. Leaves a trailing partial line in `partial` for
 * the next chunk to complete.
 * @param partial Accumulator holding the not-yet-newline-terminated tail from previous chunks; updated in place.
 * @param data Pointer to the newly read bytes to feed in.
 * @param len Number of bytes at `data`.
 * @param is_stderr Whether this chunk came from the child's stderr (tags each pushed JobLine).
 * @param mu Mutex guarding `pending`, locked around each push.
 * @param pending Queue that each complete line is appended to.
 */
void FeedChunk(std::string &partial, const char *data, size_t len, bool is_stderr, std::mutex &mu,
               std::deque<JobLine> &pending) {
    partial.append(data, len);
    size_t pos;
    while ((pos = partial.find('\n')) != std::string::npos) {
        std::string line = partial.substr(0, pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        {
            std::lock_guard<std::mutex> lk(mu);
            pending.push_back({is_stderr, line});
        }
        partial.erase(0, pos + 1);
    }
}
}  // namespace

void Job::ReaderLoop() {
    std::string stdout_partial, stderr_partial;
    bool out_open = true, err_open = (stderr_fd_ >= 0);  // PTY mode has no separate stderr fd
    char buf[4096];

    while (out_open || err_open) {
        struct pollfd fds[2];
        int nfds = 0;
        int out_idx = -1, err_idx = -1;
        if (out_open) {
            fds[nfds] = {stdout_fd_, POLLIN, 0};
            out_idx = nfds++;
        }
        if (err_open) {
            fds[nfds] = {stderr_fd_, POLLIN, 0};
            err_idx = nfds++;
        }
        int rc = poll(fds, nfds, 200);  // 200ms so a kill mid-read isn't stuck forever
        if (rc < 0) break;

        if (out_open && (fds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(stdout_fd_, buf, sizeof(buf));
            if (n <= 0) {
                if (!raw_stdout_ && !stdout_partial.empty()) {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending_.push_back({false, stdout_partial});
                }
                close(stdout_fd_);
                out_open = false;
            } else if (raw_stdout_) {
                std::lock_guard<std::mutex> lk(mu_);
                pending_raw_.emplace_back(buf, static_cast<size_t>(n));
            } else {
                FeedChunk(stdout_partial, buf, static_cast<size_t>(n), false, mu_, pending_);
            }
        }
        if (err_open && (fds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(stderr_fd_, buf, sizeof(buf));
            if (n <= 0) {
                if (!stderr_partial.empty()) {
                    std::lock_guard<std::mutex> lk(mu_);
                    pending_.push_back({true, stderr_partial});
                }
                close(stderr_fd_);
                err_open = false;
            } else {
                FeedChunk(stderr_partial, buf, static_cast<size_t>(n), true, mu_, pending_);
            }
        }
    }

    int status = 0;
    waitpid(pid_, &status, 0);
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    finished_ = true;
}
#endif  // MEP_JOB_POSIX

// --- JobManager --------------------------------------------------------

JobManager &JobManager::Instance() {
    static JobManager instance;
    return instance;
}

int JobManager::Spawn(const std::vector<std::string> &argv, const std::string &cwd, Callbacks callbacks,
                       bool use_pty, std::vector<std::pair<std::string, std::string>> extra_env) {
    Entry entry;
    entry.id = next_id_++;
    entry.job = std::make_shared<Job>(argv, cwd, callbacks.on_stdout_raw != nullptr, use_pty, std::move(extra_env));
    entry.callbacks = std::move(callbacks);
    entry.spawn_failed = entry.job->SpawnFailed();
    jobs_.push_back(std::move(entry));
    return jobs_.back().id;
}

void JobManager::ResizePty(int id, int cols, int rows) {
    if (Job *j = Find(id)) j->ResizePty(cols, rows);
}

bool JobManager::WriteStdin(int id, const std::string &data) {
    Job *j = Find(id);
    return j ? j->WriteStdin(data) : false;
}

void JobManager::CloseStdin(int id) {
    if (Job *j = Find(id)) j->CloseStdin();
}

void JobManager::Kill(int id) {
    if (Job *j = Find(id)) j->Kill();
}

bool JobManager::IsRunning(int id) const {
    for (const auto &e : jobs_) {
        if (e.id == id) return !e.job->Finished();
    }
    return false;
}

Job *JobManager::Find(int id) {
    for (auto &e : jobs_) {
        if (e.id == id) return e.job.get();
    }
    return nullptr;
}

void JobManager::PollAll() {
    // Index-based, over a size snapshotted before the loop starts -- a
    // callback below is free to call mep.job_start again (org-babel's own
    // compiled-language chaining, kBuiltinOrgBabel, already does this from
    // its compile job's on_exit; kBuiltinOrgLatex's tectonic->pdftoppm
    // chain does too), which re-enters Spawn() and can push_back onto
    // jobs_ *while this very loop is iterating it*. A `for (auto &e :
    // jobs_)` range-for (this used to be one) keeps a reference into
    // jobs_'s backing array across that call -- a push_back reallocating
    // mid-iteration frees that array out from under the still-running
    // callback, corrupting the heap (confirmed: reproducibly segfaulted,
    // stack landing in some unrelated later allocation/lock call, exactly
    // heap-corruption's usual signature) the moment enough jobs were ever
    // in flight at once to trigger a reallocation -- rare for babel's
    // usual one-job-at-a-time usage, routine for org-latex rendering
    // several fragments' tectonic+pdftoppm chains concurrently. Snapshotting
    // `n` also means a job started *by* a callback this frame is simply
    // left for next frame's PollAll rather than processed (or not) within
    // this one -- deterministic either way, and irrelevant in practice (a
    // freshly spawned job has nothing to drain yet regardless).
    //
    // Every callback is copied to a local (`auto on_x = jobs_[i].callbacks.on_x`)
    // before being invoked, rather than called through a reference straight
    // into jobs_[i] -- the copy is an independent, stack-owned std::function
    // that survives jobs_ reallocating during its own invocation; a
    // reference into jobs_[i] itself would not. jobs_[i] is otherwise only
    // ever re-indexed fresh (never cached across a callback call), and only
    // ever grows mid-loop (erasing happens once, after), so `i < n <=
    // jobs_.size()` holds throughout -- every jobs_[i] access below is in
    // bounds no matter what a callback did to the tail of the vector.
    size_t n = jobs_.size();
    for (size_t i = 0; i < n; i++) {
        std::shared_ptr<Job> job = jobs_[i].job;
        auto on_stdout_raw = jobs_[i].callbacks.on_stdout_raw;
        if (on_stdout_raw) {
            for (const std::string &chunk : job->DrainRaw()) on_stdout_raw(chunk);
        }
        auto on_stdout = jobs_[i].callbacks.on_stdout;
        auto on_stderr = jobs_[i].callbacks.on_stderr;
        for (const JobLine &line : job->DrainLines()) {
            if (line.is_stderr) {
                if (on_stderr) on_stderr(line.text);
            } else {
                if (on_stdout) on_stdout(line.text);
            }
        }
        if (job->Finished() && !jobs_[i].exit_reported) {
            jobs_[i].exit_reported = true;
            auto on_exit = jobs_[i].callbacks.on_exit;
            if (on_exit) {
                int code = job->Killed() || job->SpawnFailed() ? -1 : job->ExitCode();
                on_exit(code);
            }
        }
    }
    // Selects entries whose exit callback has already fired, so they can be erased below.
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Entry &e) { return e.exit_reported; }),
                jobs_.end());
}

void JobManager::ShutdownAll(int grace_ms) {
#if MEP_JOB_POSIX
    for (auto &e : jobs_) {
        if (e.job && !e.job->Finished()) e.job->Kill();
    }
    /**
     * @brief Checks whether any registered job still has a live child process.
     * @return True if at least one job's Job::Finished() is false.
     */
    auto still_running = [this] {
        for (auto &e : jobs_) {
            if (e.job && !e.job->Finished()) return true;
        }
        return false;
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
    while (still_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto &e : jobs_) {
        if (e.job && !e.job->Finished()) e.job->KillHard();
    }
#endif
    // Each Job's destructor joins its reader thread; by now every child is
    // either already dead or has just been SIGKILLed, so those joins
    // return promptly instead of the unbounded wait a bare SIGTERM alone
    // could leave behind.
    jobs_.clear();
}
