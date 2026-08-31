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
    // `extra_env`: name/value pairs set in the child via setenv() (not
    // execve() with a built envp -- keeps the existing execvp() call and
    // its "inherit the parent's environ" default intact for everything
    // not explicitly listed here) after fork/forkpty but before exec, so
    // they land only in the child, never the caller's own process. Empty
    // by default -- every existing call site just inherits mep's own
    // environment unmodified, same as before this parameter existed.
    /**
     * @brief Spawns `argv` as a child process (fork/forkpty on POSIX; fails
     * gracefully with SpawnFailed() set on wasm/emscripten and Windows) and
     * starts a background reader thread that streams its output.
     * @param argv Command and arguments to execute (argv[0] is looked up via execvp).
     * @param cwd Working directory for the child; unchanged if empty.
     * @param raw_stdout If true, queue raw stdout chunks instead of splitting into lines.
     * @param use_pty If true, attach the child to a pseudo-terminal instead of plain pipes; implies raw_stdout and merges stdout+stderr.
     * @param extra_env Extra name/value pairs set via setenv() in the child only, after fork/forkpty but before exec.
     */
    Job(const std::vector<std::string> &argv, const std::string &cwd, bool raw_stdout = false, bool use_pty = false,
        std::vector<std::pair<std::string, std::string>> extra_env = {});
    /**
     * @brief Kills the child if still running and joins the reader thread, blocking until it exits.
     */
    ~Job();

    /**
     * @brief Deleted: a Job owns non-copyable OS resources (pid, fds, thread).
     */
    Job(const Job &) = delete;
    /**
     * @brief Deleted: a Job owns non-copyable OS resources (pid, fds, thread).
     */
    Job &operator=(const Job &) = delete;

    /**
     * @brief Reports whether the child process has exited (or the job failed to spawn).
     * @return True once the reader thread has observed the child's exit status.
     */
    bool Finished() const { return finished_.load(); }
    /**
     * @brief Returns the child's exit code once finished.
     * @return The process exit status, or -1 if not yet finished/killed/spawn failed.
     */
    int ExitCode() const { return exit_code_; }
    /**
     * @brief Reports whether Kill() or KillHard() was called on this job.
     * @return True if the job was killed rather than exiting on its own.
     */
    bool Killed() const { return killed_.load(); }
    /**
     * @brief Reports whether the constructor failed to spawn the child process.
     * @return True if spawning failed (e.g. empty argv, pipe/fork failure, or an unsupported platform).
     */
    bool SpawnFailed() const { return spawn_failed_; }

    /**
     * @brief Sends SIGTERM to the child's whole process group.
     */
    void Kill();

    /**
     * @brief Escalates to SIGKILL, which (unlike SIGTERM) can't be caught,
     * ignored, or blocked -- used once a prior Kill() has had a grace
     * period to let the child exit on its own and it still hasn't (e.g. a
     * REPL/TUI that traps SIGTERM, or simply a slow shutdown). See
     * JobManager::ShutdownAll.
     */
    void KillHard();

    /**
     * @brief Updates the PTY's window size (TIOCSWINSZ) so full-screen terminal
     * programs (a shell running $EDITOR, a pager, ...) wrap/paginate to
     * match the hosting pane. No-op if this isn't a PTY job.
     * @param cols New terminal width in columns.
     * @param rows New terminal height in rows.
     */
    void ResizePty(int cols, int rows);

    /**
     * @brief Writes to the child's stdin (e.g. `git apply --cached -`, a REPL).
     * @param data Bytes to write; written in full via a blocking write loop.
     * @return False once the pipe is closed/gone (or this isn't a POSIX build).
     */
    bool WriteStdin(const std::string &data);
    /**
     * @brief Closes the write end of the child's stdin, signalling EOF to it.
     * No-op for a PTY job, since stdin and stdout share one fd there.
     */
    void CloseStdin();

    /**
     * @brief Drains everything queued since the last call, in arrival order.
     * Only ever called from the main thread (JobManager::PollAll).
     * @return The lines (stdout and stderr, tagged) received since the last drain.
     */
    std::vector<JobLine> DrainLines();
    /**
     * @brief Raw-mode equivalent: unsplit stdout chunks (see `raw_stdout` above).
     * stderr is unaffected -- still line-split, still read via DrainLines
     * (JobLine::is_stderr) -- no consumer of raw stdout has needed raw
     * stderr too.
     * @return The raw stdout byte chunks received since the last drain.
     */
    std::vector<std::string> DrainRaw();

private:
    pid_t pid_ = -1;
    int stdout_fd_ = -1, stderr_fd_ = -1, stdin_fd_ = -1;
    bool raw_stdout_ = false;
    bool use_pty_ = false;
    std::vector<std::pair<std::string, std::string>> extra_env_;
    std::thread reader_thread_;
    std::mutex mu_;
    std::deque<JobLine> pending_;
    std::deque<std::string> pending_raw_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> killed_{false};
    bool spawn_failed_ = false;
    int exit_code_ = -1;

    /**
     * @brief Background-thread loop that polls the child's stdout/stderr fds,
     * splits stdout into lines (or queues raw chunks in raw_stdout_ mode) and
     * stderr into lines, pushing each onto the thread-safe pending queues,
     * until both fds are closed; then waits for the child to exit and
     * records its exit code, marking the job finished.
     */
    void ReaderLoop();
};

// Owns every live Job, polled once per frame from the main loop.
class JobManager {
public:
    /**
     * @brief Returns the process-wide JobManager singleton, constructing it on first use.
     * @return Reference to the single JobManager instance.
     */
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

    /**
     * @brief Spawns and registers a job; returns its id (0 on spawn failure, in
     * which case on_exit(-1) is invoked on the very next PollAll(), not
     * synchronously -- keeps the "callbacks only run from PollAll"
     * invariant simple for callers). `use_pty`: see Job's constructor --
     * implies raw mode regardless of which callback field is set.
     * `extra_env`: see Job's constructor.
     * @param argv Command and arguments to execute.
     * @param cwd Working directory for the child; unchanged if empty.
     * @param callbacks Handlers invoked from PollAll() for this job's stdout/stderr/exit.
     * @param use_pty If true, attach the child to a pseudo-terminal instead of plain pipes.
     * @param extra_env Extra name/value pairs set in the child's environment only.
     * @return The new job's id (never 0 on success; 0 if spawning failed).
     */
    int Spawn(const std::vector<std::string> &argv, const std::string &cwd, Callbacks callbacks,
              bool use_pty = false, std::vector<std::pair<std::string, std::string>> extra_env = {});

    // Raw write/close access for interactive jobs (REPLs, `git apply`).
    /**
     * @brief Writes to the given job's stdin.
     * @param id Id of the target job, as returned by Spawn().
     * @param data Bytes to write.
     * @return False if the job doesn't exist or its stdin pipe is closed/gone.
     */
    bool WriteStdin(int id, const std::string &data);
    /**
     * @brief Closes the write end of the given job's stdin, signalling EOF to it.
     * @param id Id of the target job, as returned by Spawn().
     */
    void CloseStdin(int id);
    /**
     * @brief Sends SIGTERM to the given job's process group.
     * @param id Id of the target job, as returned by Spawn().
     */
    void Kill(int id);
    /**
     * @brief Reports whether the given job is still registered and not yet finished.
     * @param id Id of the target job, as returned by Spawn().
     * @return True if the job exists and hasn't finished.
     */
    bool IsRunning(int id) const;
    /**
     * @brief Updates the given PTY job's terminal window size.
     * @param id Id of the target job, as returned by Spawn().
     * @param cols New terminal width in columns.
     * @param rows New terminal height in rows.
     */
    void ResizePty(int id, int cols, int rows);

    /**
     * @brief Call once per frame: drains every live job's buffered lines/exit
     * status and invokes its stored callbacks, then reaps finished jobs
     * whose callbacks have all fired.
     */
    void PollAll();

    /**
     * @brief Terminates every still-running job: SIGTERM to each, up to
     * `grace_ms` for them to exit on their own, then SIGKILL to whatever's
     * left. Called once from main() right after the render loop exits, so
     * a non-cooperative child (one that ignores SIGTERM) can't leave
     * Job::~Job()'s reader-thread join() blocking mep's own process exit
     * indefinitely -- relying on that destructor alone (reached only via
     * this JobManager singleton's own static teardown) meant a stuck child
     * held the whole app open until something outside mep (e.g. a repeated
     * Ctrl-C at the launching shell) killed it by force.
     * @param grace_ms Milliseconds to wait after SIGTERM before escalating to SIGKILL.
     */
    void ShutdownAll(int grace_ms = 500);

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

    /**
     * @brief Looks up a registered job by id.
     * @param id Id of the job to find, as returned by Spawn().
     * @return Pointer to the matching Job, or nullptr if no live entry has that id.
     */
    Job *Find(int id);
};

#endif  // MEP_JOB_H
