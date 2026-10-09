#include "job.h"
#include "frame_activity.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#define MEP_JOB_POSIX 1
#include <fcntl.h>  // O_CLOEXEC, for RequestStop's self-pipe
#include <poll.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <util.h>  // forkpty lives here on macOS (BSD), not in <pty.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

Job::Job(const std::vector<std::string> &argv, const std::string &cwd, bool raw_stdout, bool use_pty,
         std::vector<std::pair<std::string, std::string>> extra_env, bool die_with_parent)
    : raw_stdout_(raw_stdout || use_pty), use_pty_(use_pty), extra_env_(std::move(extra_env)) {
#if MEP_JOB_POSIX
    // In the child, right after fork: SIGTERM when mep goes (and at once,
    // if it already went between the fork and here).
    const pid_t parent_pid = getpid();
    auto tie_to_parent = [die_with_parent, parent_pid] {
#if defined(__linux__)
        if (!die_with_parent) return;
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent_pid) _exit(1);
#else
        (void)die_with_parent;
        (void)parent_pid;
#endif
    };
    if (argv.empty()) {
        spawn_failed_ = true;
        finished_ = true;
        return;
    }
    // O_CLOEXEC: this is mep's own wakeup channel, and a child holding its
    // write end open past exec would be one more fd in a terminal's
    // environment for no reason.
#if defined(__linux__)
    if (pipe2(stop_fds_, O_CLOEXEC) != 0) stop_fds_[0] = stop_fds_[1] = -1;  // the 200ms poll timeout still bounds the stop
#else
    // macOS has no pipe2: pipe + FD_CLOEXEC after the fact. The gap
    // between the two calls only matters for a concurrent fork, and every
    // Job spawn happens on the thread constructing the Job -- the window
    // is the same one every other fcntl-after-open in this file accepts.
    if (pipe(stop_fds_) != 0) {
        stop_fds_[0] = stop_fds_[1] = -1;  // the 200ms poll timeout still bounds the stop
    } else {
        fcntl(stop_fds_[0], F_SETFD, FD_CLOEXEC);
        fcntl(stop_fds_[1], F_SETFD, FD_CLOEXEC);
    }
#endif
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
            tie_to_parent();
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
            // Strip KITTY_* -- mep launched from a real kitty window
            // inherits these, and a shell that sees KITTY_INSTALLATION_DIR
            // set will unconditionally source kitty's shell-integration
            // script (common in .bashrc/.zshrc: `[ -n "$KITTY_INSTALLATION_
            // DIR" ] && source .../kitty.bash`), regardless of $TERM. That
            // script wraps its prompt additions in literal `\[...\]`
            // readline-invisibility markers and performs a terminal
            // handshake only real kitty answers; since this PTY isn't
            // kitty, the handshake never completes and the raw `\[`/`\]`
            // markers leak into the cell grid as visible text -- the
            // "funny characters ... lots of square brackets" bug. Clearing
            // these before exec makes the child correctly see itself as
            // not running inside kitty, matching reality.
            for (const char *k : {"KITTY_WINDOW_ID", "KITTY_PID", "KITTY_INSTALLATION_DIR", "KITTY_LISTEN_ON",
                                   "KITTY_PUBLIC_KEY", "KITTY_SHELL_INTEGRATION"}) {
                unsetenv(k);
            }
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
        tie_to_parent();
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
    (void)die_with_parent;
    spawn_failed_ = true;
    finished_ = true;
#endif
}

Job::Job(Task task) : is_task_(true) {
#if defined(__EMSCRIPTEN__)
    // No threads to lean on here: run it to completion right now. The
    // lines still only reach callbacks via PollAll, as for any job.
    RunTask(std::move(task));
#elif MEP_JOB_POSIX
    // Its own pthread rather than std::thread, for the stack: a task may
    // run mep's recursive-backtracking regex (regex.h) over long lines,
    // and a secondary thread's default stack (512KB on macOS) overflows on
    // a few hundred characters of `(a|b)*` -- measured. 64MB is address
    // space, not memory: pages are only committed as they are touched.
    pending_task_ = std::move(task);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kTaskStackBytes);
    /**
     * @brief pthread entry point: runs the job's pending task.
     * @param self The Job.
     * @return Always null.
     */
    auto entry = [](void *self) -> void * {
        Job *job = static_cast<Job *>(self);
        job->RunTask(std::move(job->pending_task_));
        return nullptr;
    };
    if (pthread_create(&task_thread_, &attr, entry, this) == 0) {
        task_thread_started_ = true;
    } else {
        spawn_failed_ = true;
        finished_ = true;
    }
    pthread_attr_destroy(&attr);
#else
    reader_thread_ = std::thread(&Job::RunTask, this, std::move(task));
#endif
}

void Job::RunTask(Task task) {
    /**
     * @brief Queues one line of the task's output, blocking while the queue is over its cap.
     * @param line The line to queue.
     */
    EmitLine emit = [this](const std::string &line) {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (pending_.size() < kMaxPendingTaskLines || task_cancelled_.load()) {
                    pending_.push_back({false, line});
                    break;
                }
            }
            mep::WakeMainLoop();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Coalesced: one wake per batch is enough for the main loop to
        // come and drain, so don't pay for it on every line.
        if (pending_lines_since_wake_++ % 256 == 0) mep::WakeMainLoop();
    };
    exit_code_ = task ? task(emit, task_cancelled_) : -1;
    finished_ = true;
    mep::WakeMainLoop();
}

Job::~Job() {
    if (is_task_) {
        task_cancelled_ = true;
        if (reader_thread_.joinable()) reader_thread_.join();
#if MEP_JOB_POSIX
        if (task_thread_started_) pthread_join(task_thread_, nullptr);
#endif
        return;
    }
#if MEP_JOB_POSIX
    if (!finished_.load()) Kill();
    // Before the join: nothing can consume this job's output any more, so
    // the reader thread has no reason to keep waiting for an EOF it may
    // never see (backlogged raw output, a PTY slave held open by a process
    // that escaped the process group) -- see Job::RequestStop in job.h.
    RequestStop();
    if (reader_thread_.joinable()) reader_thread_.join();
    if (stdin_fd_ >= 0) close(stdin_fd_);
    for (int &fd : stop_fds_) {
        if (fd >= 0) close(fd);
        fd = -1;
    }
#endif
}

void Job::Kill() {
    if (is_task_) {
        if (!finished_.load()) {
            killed_ = true;
            task_cancelled_ = true;
        }
        return;
    }
#if MEP_JOB_POSIX
    if (pid_ > 0 && !finished_.load()) {
        killed_ = true;
        // A PTY job is a terminal session, and SIGHUP -- the hangup a real
        // terminal emulator delivers when its window closes -- is the
        // signal an interactive shell actually acts on: bash and zsh
        // deliberately ignore SIGTERM as job-control shells (measured on
        // this machine's login shell: SIGHUP gone in 5ms, SIGTERM still
        // running 3s later). Without the hangup, one :terminal pane left
        // open made every quit sit through ShutdownAll's entire grace
        // period and die only to the SIGKILL at the end of it -- half a
        // second added to :qa for a shell sitting at its prompt. SIGTERM
        // still follows, for a non-shell child that ignores hangups but
        // would exit on a terminate.
        if (use_pty_) SignalChild(SIGHUP);
        SignalChild(SIGTERM);
    }
#endif
}

void Job::Interrupt() {
#if MEP_JOB_POSIX
    if (pid_ > 0 && !finished_.load()) kill(-pid_, SIGINT);
#endif
}

void Job::KillHard() {
    if (is_task_) {
        Kill();
        return;
    }
#if MEP_JOB_POSIX
    if (pid_ > 0 && !finished_.load()) {
        killed_ = true;
        SignalChild(SIGKILL);
    }
#endif
}

void Job::SignalChild(int sig) {
#if MEP_JOB_POSIX
    if (pid_ <= 0) return;
    kill(pid_, sig);
    kill(-pid_, sig);
#else
    (void)sig;
#endif
}

void Job::RequestStop() {
    stopping_ = true;
    task_cancelled_ = true;
#if MEP_JOB_POSIX
    // Unblocks a reader parked in poll() right now; the flag above is what
    // it then acts on, so a failed/short write costs only the wait for
    // that poll's own timeout.
    if (stop_fds_[1] >= 0) {
        const char byte = 0;
        ssize_t written = write(stop_fds_[1], &byte, 1);
        (void)written;
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

std::vector<std::string> Job::DrainRaw(size_t max_bytes, size_t max_newlines) {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    size_t bytes = 0, newlines = 0;
    while (!pending_raw_.empty() && bytes < max_bytes && newlines < max_newlines) {
        std::string &chunk = pending_raw_.front();
        bytes += chunk.size();
        // Only counted when a ceiling actually applies: for the unlimited
        // default (every consumer but a terminal) this loop is the old
        // "take everything", and scanning the bytes for newlines would be
        // pure waste.
        if (max_newlines != std::numeric_limits<size_t>::max()) {
            newlines += static_cast<size_t>(std::count(chunk.begin(), chunk.end(), '\n'));
        }
        pending_raw_bytes_ -= chunk.size();
        out.push_back(std::move(chunk));
        pending_raw_.pop_front();
    }
    return out;
}

bool Job::HasPendingRaw() {
    std::lock_guard<std::mutex> lk(mu_);
    return !pending_raw_.empty();
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
        // Asked to stop (job being destroyed / mep shutting down): leave
        // the fds to ~Job rather than waiting for an EOF that the
        // backpressure branch below, or a PTY slave still open somewhere,
        // may never let this loop observe. Checked before the poll so the
        // worst case is one 200ms timeout, not forever.
        if (stopping_.load()) break;
        // Backpressure (see pending_raw_bytes_'s own comment, job.h): while
        // raw-mode output is backlogged past the cap, simply don't ask
        // poll() about stdout_fd_ this round -- the child's own write()s
        // block once the PTY's kernel buffer (bounded, unlike our queue)
        // fills up, so it naturally stalls until DrainRaw() catches up
        // instead of mep buffering an unbounded amount of unprocessed
        // output in RAM. Re-checked every iteration (the same 200ms poll
        // timeout below already paces this loop), so reading resumes the
        // moment the main thread drains enough to drop back under the cap.
        bool out_backlogged = false;
        if (out_open && raw_stdout_) {
            std::lock_guard<std::mutex> lk(mu_);
            out_backlogged = pending_raw_bytes_ >= kMaxPendingRawBytes;
        }
        struct pollfd fds[3];
        int nfds = 0;
        int out_idx = -1, err_idx = -1;
        if (out_open && !out_backlogged) {
            fds[nfds] = {stdout_fd_, POLLIN, 0};
            out_idx = nfds++;
        }
        if (err_open) {
            fds[nfds] = {stderr_fd_, POLLIN, 0};
            err_idx = nfds++;
        }
        // RequestStop's self-pipe: watched so a stop lands at once rather
        // than at the end of this poll's timeout (which, with stdout left
        // out of the set by the backpressure branch above, is all this
        // loop would otherwise be waiting on).
        if (stop_fds_[0] >= 0) fds[nfds++] = {stop_fds_[0], POLLIN, 0};
        int rc = poll(fds, static_cast<nfds_t>(nfds), 200);  // 200ms so a kill mid-read isn't stuck forever
        if (rc < 0) break;
        if (stopping_.load()) break;
        if (rc > 0) mep::WakeMainLoop();  // output (or its end) for the main loop to pick up

        if (out_open && out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
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
                pending_raw_bytes_ += static_cast<size_t>(n);
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
    if (stopping_.load()) {
        // Stopped early, so the child may well still be alive -- a
        // blocking waitpid here would just move the hang this bail-out
        // exists to prevent from poll() to waitpid(). Reap it if it goes
        // promptly (the usual case: ShutdownAll has already SIGTERMed and
        // SIGKILLed it), escalate to SIGKILL ourselves if it hasn't, and
        // give up after that rather than hold up mep's exit -- an
        // unreaped child is inherited by init, which reaps it.
        // The 700ms escalation deliberately sits *after* ShutdownAll's own
        // 500ms grace-then-SIGKILL, so quitting mep keeps giving children
        // exactly the grace period that sets; it is here for the
        // standalone ~Job case (a terminal pane closed one at a time),
        // where nothing else escalates a SIGTERM the child ignores.
        const auto start = std::chrono::steady_clock::now();
        bool escalated = false;
        for (;;) {
            if (waitpid(pid_, &status, WNOHANG) != 0) break;  // reaped, or already gone
            const auto waited = std::chrono::steady_clock::now() - start;
            if (waited >= std::chrono::milliseconds(1200)) return;
            if (!escalated && waited >= std::chrono::milliseconds(700)) {
                escalated = true;
                KillHard();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } else {
        waitpid(pid_, &status, 0);
    }
    exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    finished_ = true;
    mep::WakeMainLoop();
}
#endif  // MEP_JOB_POSIX

// --- JobManager --------------------------------------------------------

JobManager &JobManager::Instance() {
    static JobManager instance;
    return instance;
}

int JobManager::Spawn(const std::vector<std::string> &argv, const std::string &cwd, Callbacks callbacks,
                       bool use_pty, std::vector<std::pair<std::string, std::string>> extra_env, bool die_with_parent) {
    Entry entry;
    entry.id = next_id_++;
    for (const std::string &a : argv) { entry.debug_cmd += a; entry.debug_cmd += ' '; }
    entry.job = std::make_shared<Job>(argv, cwd, callbacks.on_stdout_raw != nullptr, use_pty, std::move(extra_env),
                                      die_with_parent);
    entry.callbacks = std::move(callbacks);
    entry.spawn_failed = entry.job->SpawnFailed();
    jobs_.push_back(std::move(entry));
    return jobs_.back().id;
}

int JobManager::SpawnTask(Job::Task task, Callbacks callbacks, const std::string &debug_name) {
    Entry entry;
    entry.id = next_id_++;
    entry.debug_cmd = debug_name;
    entry.job = std::make_shared<Job>(std::move(task));
    callbacks.on_stdout_raw = nullptr;  // a task only ever emits lines
    entry.callbacks = std::move(callbacks);
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

void JobManager::KillHard(int id) {
    if (Job *j = Find(id)) j->KillHard();
}

void JobManager::Interrupt(int id) {
    if (Job *j = Find(id)) j->Interrupt();
}

bool JobManager::IsRunning(int id) const {
    for (const auto &e : jobs_) {
        if (e.id == id) return !e.job->Finished();
    }
    return false;
}

int JobManager::Pid(int id) const {
    for (const auto &e : jobs_) {
        if (e.id == id) return e.job->Pid();
    }
    return -1;
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
    static const bool kProf = std::getenv("MEP_PDF_PROF") != nullptr;
    size_t n = jobs_.size();
    for (size_t i = 0; i < n; i++) {
        auto j0 = std::chrono::steady_clock::now();
        double t_raw = 0, t_lines = 0, t_exit = 0;
        size_t n_lines = 0;
        std::shared_ptr<Job> job = jobs_[i].job;
        auto on_stdout_raw = jobs_[i].callbacks.on_stdout_raw;
        if (on_stdout_raw) {
            auto should_poll_raw = jobs_[i].callbacks.should_poll_raw;
            if (!should_poll_raw || should_poll_raw()) {
                // 0 means "no ceiling" (see Callbacks' own comment).
                const size_t max_bytes = jobs_[i].callbacks.max_raw_bytes_per_poll;
                const size_t max_newlines = jobs_[i].callbacks.max_raw_newlines_per_poll;
                for (const std::string &chunk :
                     job->DrainRaw(max_bytes ? max_bytes : std::numeric_limits<size_t>::max(),
                                   max_newlines ? max_newlines : std::numeric_limits<size_t>::max())) {
                    mep::NoteActivity();
                    on_stdout_raw(chunk);
                }
            }
        }
        if (kProf) { t_raw = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - j0).count(); }
        auto on_stdout = jobs_[i].callbacks.on_stdout;
        auto on_stderr = jobs_[i].callbacks.on_stderr;
        auto l0 = std::chrono::steady_clock::now();
        for (const JobLine &line : job->DrainLines()) {
            ++n_lines;
            mep::NoteActivity();
            if (line.is_stderr) {
                if (on_stderr) on_stderr(line.text);
            } else {
                if (on_stdout) on_stdout(line.text);
            }
        }
        if (kProf) { t_lines = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - l0).count(); }
        auto e0 = std::chrono::steady_clock::now();
        // A finished job whose raw-stdout consumer is applying backpressure
        // (should_poll_raw returned false above) may still hold chunks the
        // consumer has not taken: reporting the exit now would erase the
        // entry (below) with that tail unread, so the consumer sees EOF
        // early. Keep the entry until it has drained everything -- finished_
        // is only set once both pipes hit EOF (ReaderLoop), so "finished and
        // nothing pending" really is "everything delivered". (The YouTube
        // audio decoder hit this on every short video and at the end of
        // every long one: ffmpeg finished while the player still held two
        // seconds of PCM in its queue, and the rest of the track vanished.)
        const bool raw_tail_pending = on_stdout_raw && job->HasPendingRaw();
        if (job->Finished() && !jobs_[i].exit_reported && !raw_tail_pending) {
            jobs_[i].exit_reported = true;
            mep::NoteActivity();
            auto on_exit = jobs_[i].callbacks.on_exit;
            if (on_exit) {
                int code = job->Killed() || job->SpawnFailed() ? -1 : job->ExitCode();
                on_exit(code);
            }
        }
        if (kProf) {
            t_exit = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e0).count();
            double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - j0).count();
            if (total > 500.0)
                std::fprintf(stderr, "[PDFPROF] job[%d] '%s': total %.0f ms (raw %.0f, lines %.0f/%zu, exit %.0f)\n",
                             jobs_[i].id, jobs_[i].debug_cmd.c_str(), total, t_raw, t_lines, n_lines, t_exit);
        }
    }
    // Selects entries whose exit callback has already fired, so they can be erased below.
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Entry &e) { return e.exit_reported; }),
                jobs_.end());
}

void JobManager::ShutdownAll(int grace_ms) {
#if MEP_JOB_POSIX
    for (const auto &e : jobs_) {
        if (!e.job) continue;
        if (!e.job->Finished()) e.job->Kill();
        // Up front rather than leaving it to each ~Job below: every
        // reader thread then unwinds concurrently with the grace period
        // instead of costing a poll timeout each, in series, once
        // jobs_.clear() starts destroying them.
        e.job->RequestStop();
    }
    /**
     * @brief Checks whether any registered job still has a live child process.
     * @return True if at least one job's Job::Finished() is false.
     */
    auto still_running = [this] {
        for (const auto &e : jobs_) {
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
