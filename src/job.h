#ifndef MEP_JOB_H
#define MEP_JOB_H

#include <sys/types.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Async external-process runner (NVIM_PARITY_PLAN.md Part I Phase 1):
// spawns argv on a background thread (POSIX fork+exec), streams stdout/
// stderr line-by-line into thread-safe queues, and reports completion --
// but never invokes any caller-supplied callback off the main thread.
// JobManager::PollAll() (called once per frame) is the only place
// callbacks run, so editor state (Buffer, Pane, ...) can be touched from
// them without extra synchronization, matching how the rest of this
// single-threaded renderer/editor is written.
//
// Unsupported on the wasm/emscripten build (no processes in a browser
// sandbox) and on Windows (no fork) -- Spawn() fails gracefully there,
// invoking on_exit(-1) synchronously, the same way the rest of the
// codebase already degrades when an external tool (git, rg) is missing.
struct JobLine {
    bool is_stderr = false;
    std::string text;
};

class Job {
public:
    // `raw_stdout`: skip line-splitting for stdout entirely and queue raw
    // chunks exactly as read() delivered them instead (DrainRaw(), not
    // DrainLines()) -- needed by anything using a byte-count-framed
    // protocol rather than a line-oriented one (Part V Phase 20's LSP
    // client: a Content-Length-prefixed JSON body has no trailing
    // newline, so back-to-back messages with no gap between them would
    // otherwise get silently concatenated by line-splitting into one
    // corrupt "line" -- a real bug caught during that phase's
    // verification).
    // `use_pty`: spawn the child attached to a pseudo-terminal (via
    // forkpty()) instead of plain pipes (Part VI Phase 27's terminal
    // widget) -- a real PTY, not a pipe, because plain pipes make a
    // shell/most interactive programs behave differently (no color, no
    // readline-style prompt editing, fully-buffered instead of
    // line-buffered output) since they detect stdout isn't a tty.
    // Implies raw_stdout (a terminal's output is arbitrary bytes,
    // including mid-stream ANSI escapes, not line-oriented at all) and
    // merges stdout+stderr onto the one PTY stream a real terminal would
    // also merge them onto.
    Job(const std::vector<std::string> &argv, const std::string &cwd, bool raw_stdout = false, bool use_pty = false);
    ~Job();

    Job(const Job &) = delete;
    Job &operator=(const Job &) = delete;

    bool Finished() const { return finished_.load(); }
    int ExitCode() const { return exit_code_; }
    bool Killed() const { return killed_.load(); }
    bool SpawnFailed() const { return spawn_failed_; }

    // Sends SIGTERM to the child's whole process group.
    void Kill();

    // Updates the PTY's window size (TIOCSWINSZ) so full-screen terminal
    // programs (a shell running $EDITOR, a pager, ...) wrap/paginate to
    // match the hosting pane. No-op if this isn't a PTY job.
    void ResizePty(int cols, int rows);

    // Writes to the child's stdin (e.g. `git apply --cached -`, a REPL).
    // Returns false once the pipe is closed/gone.
    bool WriteStdin(const std::string &data);
    void CloseStdin();

    // Drains everything queued since the last call, in arrival order.
    // Only ever called from the main thread (JobManager::PollAll).
    std::vector<JobLine> DrainLines();
    // Raw-mode equivalent: unsplit stdout chunks (see `raw_stdout` above).
    // stderr is unaffected -- still line-split, still read via DrainLines
    // (JobLine::is_stderr) -- no consumer of raw stdout has needed raw
    // stderr too.
    std::vector<std::string> DrainRaw();

private:
    pid_t pid_ = -1;
    int stdout_fd_ = -1, stderr_fd_ = -1, stdin_fd_ = -1;
    bool raw_stdout_ = false;
    bool use_pty_ = false;
    std::thread reader_thread_;
    std::mutex mu_;
    std::deque<JobLine> pending_;
    std::deque<std::string> pending_raw_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> killed_{false};
    bool spawn_failed_ = false;
    int exit_code_ = -1;

    void ReaderLoop();
};

// Owns every live Job, polled once per frame from the main loop.
class JobManager {
public:
    static JobManager &Instance();

    struct Callbacks {
        std::function<void(const std::string &)> on_stdout;
        std::function<void(const std::string &)> on_stderr;
        std::function<void(int)> on_exit;  // exit code, or -1 if killed/failed to spawn
        // Set this (instead of on_stdout) for a byte-count-framed
        // protocol; see Job's `raw_stdout` constructor parameter. Setting
        // it switches this job into raw mode at Spawn() time.
        std::function<void(const std::string &)> on_stdout_raw;
    };

    // Spawns and registers a job; returns its id (0 on spawn failure, in
    // which case on_exit(-1) is invoked on the very next PollAll(), not
    // synchronously -- keeps the "callbacks only run from PollAll"
    // invariant simple for callers). `use_pty`: see Job's constructor --
    // implies raw mode regardless of which callback field is set.
    int Spawn(const std::vector<std::string> &argv, const std::string &cwd, Callbacks callbacks,
              bool use_pty = false);

    // Raw write/close access for interactive jobs (REPLs, `git apply`).
    bool WriteStdin(int id, const std::string &data);
    void CloseStdin(int id);
    void Kill(int id);
    bool IsRunning(int id) const;
    void ResizePty(int id, int cols, int rows);

    // Call once per frame: drains every live job's buffered lines/exit
    // status and invokes its stored callbacks, then reaps finished jobs
    // whose callbacks have all fired.
    void PollAll();

private:
    struct Entry {
        int id = 0;
        std::shared_ptr<Job> job;
        Callbacks callbacks;
        bool exit_reported = false;
        bool spawn_failed = false;
    };
    std::vector<Entry> jobs_;
    int next_id_ = 1;

    Job *Find(int id);
};

#endif  // MEP_JOB_H
