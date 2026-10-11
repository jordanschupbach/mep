// mep-job-test: Job's teardown is prompt, whatever the child does.
//
// Every case here is a way a child used to keep a Job's destructor
// blocked -- forever, in the last two -- which is what left mep's window
// on screen, frozen, after :qa until the process was killed by hand. They
// are unit-testable because none of them needs an editor or a window:
// just a Job, a child, and a stopwatch around the destructor. The last
// case is about JobManager instead: every line a job prints reaching
// on_stdout before its on_exit runs.
//
// Usage: mep-job-test

#include "job.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

/**
 * @brief Reports a failed expectation on stderr and records it for the exit status.
 * @param what Description of the expectation that failed.
 */
void Fail(const std::string &what) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

/**
 * @brief Checks a condition, reporting it as a failure when false.
 * @param cond The condition that should hold.
 * @param what Description of what was expected.
 */
void Check(bool cond, const std::string &what) {
    if (!cond) Fail(what);
}

/**
 * @brief The user's login shell -- the program a bare `:terminal` runs, and the one
 * whose signal handling this test is about (a job-control shell ignores SIGTERM).
 * @return $SHELL if set, else /bin/bash.
 */
std::string LoginShell() {
    const char *env = std::getenv("SHELL");
    return (env && *env) ? std::string(env) : std::string("/bin/bash");
}

/**
 * @brief Milliseconds spent destroying a job, i.e. what a caller tearing one down pays.
 * @param job The job to destroy; the unique_ptr is consumed.
 * @return Wall-clock milliseconds the destructor took.
 */
double DestroyMs(std::unique_ptr<Job> job) {
    const auto t0 = std::chrono::steady_clock::now();
    job.reset();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Generous next to the real numbers (a few milliseconds each), small
// enough that any of the hangs this file is about trips it: those were
// unbounded, and the SIGTERM-ignoring shell took the full half-second
// grace period plus a SIGKILL before this fix.
constexpr double kPromptMs = 1500.0;

/**
 * @brief A plain pipe job whose child exits on SIGTERM: the ordinary case, and the
 * one that already worked -- here so a regression in it is not mistaken for one of
 * the others below.
 */
void TestPipeJobKill() {
    auto job = std::make_unique<Job>(std::vector<std::string>{"sleep", "60"}, "");
    Check(!job->SpawnFailed(), "sleep should spawn");
    job->Kill();
    const double ms = DestroyMs(std::move(job));
    std::printf("  pipe job, child exits on SIGTERM:        %7.1f ms\n", ms);
    Check(ms < kPromptMs, "destroying a SIGTERMed pipe job should be prompt");
}

/**
 * @brief A terminal running the login shell. An interactive, job-control shell
 * ignores SIGTERM outright, so closing one used to cost ShutdownAll's whole grace
 * period and a SIGKILL; a PTY job is hung up (SIGHUP) now, which is both what a
 * real terminal emulator does and the signal the shell acts on.
 */
void TestPtyShellHangsUp() {
    auto job = std::make_unique<Job>(std::vector<std::string>{LoginShell()}, "", /*raw_stdout=*/true, /*use_pty=*/true);
    Check(!job->SpawnFailed(), "the login shell should spawn on a PTY");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // let it finish its rc files
    const pid_t pid = static_cast<pid_t>(job->Pid());
    job->Kill();
    const double ms = DestroyMs(std::move(job));
    std::printf("  PTY job, interactive shell:              %7.1f ms\n", ms);
    Check(ms < kPromptMs, "hanging up a terminal's shell should end it at once, not on a later SIGKILL");
    Check(kill(pid, 0) != 0, "the shell should be gone, not merely abandoned");
}

/**
 * @brief A terminal whose output nobody drains. Once the raw-output backlog hits
 * Job's cap the reader stops polling stdout by design, so it can never see EOF
 * either: before RequestStop, this destructor never returned.
 */
void TestBackloggedPtyJob() {
    auto job = std::make_unique<Job>(std::vector<std::string>{"yes"}, "", /*raw_stdout=*/true, /*use_pty=*/true);
    Check(!job->SpawnFailed(), "yes should spawn on a PTY");
    // Nothing calls DrainRaw(), so this fills the 8MB cap and stays there.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    Check(job->HasPendingRaw(), "the backlog should have built up with nobody draining it");
    const double ms = DestroyMs(std::move(job));
    std::printf("  PTY job, backlogged with nobody draining: %6.1f ms\n", ms);
    Check(ms < kPromptMs, "destroying a backlogged job should not wait for an EOF that cannot come");
}

/**
 * @brief A terminal whose PTY slave is still held open by a process that left the
 * killed process group (`setsid`), so the master reports no EOF however dead the
 * direct child is -- the second way this destructor used to block forever.
 */
void TestEscapedPtyHolder() {
    auto job = std::make_unique<Job>(
        std::vector<std::string>{LoginShell(), "-c", "setsid sleep 30 & exec sleep 30"}, "",
        /*raw_stdout=*/true, /*use_pty=*/true);
    Check(!job->SpawnFailed(), "the shell should spawn on a PTY");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // let setsid actually run
    job->Kill();
    const double ms = DestroyMs(std::move(job));
    std::printf("  PTY job, slave held by an escaped child:  %6.1f ms\n", ms);
    Check(ms < kPromptMs, "destroying a job whose PTY stays open should not wait for EOF");
}

/**
 * @brief A job that prints a lot and exits while the main loop is still busy with
 * its first lines (in mep, a Lua on_stdout per line): every line must reach
 * on_stdout before on_exit. PollAll used to check Finished() after draining, so
 * the lines the reader queued meanwhile were dropped with the finished entry (the
 * Tests panel's ctest listing lost its tail this way).
 */
void TestExitAfterAllLines() {
    constexpr int kLines = 50000;
    int lines = 0, seen_at_exit = -1;
    JobManager::Callbacks cb;
    cb.on_stdout = [&lines](const std::string &) {
        // Slow on the first line only: long enough for seq to print the rest and exit.
        if (lines++ == 0) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    };
    cb.on_exit = [&lines, &seen_at_exit](int) { seen_at_exit = lines; };
    JobManager::Instance().Spawn({"seq", "1", std::to_string(kLines)}, "", std::move(cb));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (seen_at_exit < 0 && std::chrono::steady_clock::now() < deadline) JobManager::Instance().PollAll();
    std::printf("  lines delivered before on_exit:       %6d/%d\n", seen_at_exit, kLines);
    Check(seen_at_exit == kLines, "on_exit should only run once all of a job's output has been delivered");
}

}  // namespace

int main() {
    std::printf("job_test: how long tearing a job down takes\n");
    TestPipeJobKill();
    TestPtyShellHangsUp();
    TestBackloggedPtyJob();
    TestEscapedPtyHolder();
    TestExitAfterAllLines();
    if (g_failures > 0) {
        std::fprintf(stderr, "job_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("job_test: all checks passed\n");
    return 0;
}
