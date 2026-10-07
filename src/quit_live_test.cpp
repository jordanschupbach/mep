// mep-quit-live-test: :qa! actually ends the process, promptly, with the
// kinds of child that used to make it hang forever still attached.
//
// WHAT THIS COVERS THAT A WINDOWLESS TEST CANNOT. These regressions live
// in the shutdown path of a *real* mep -- a main loop that has exited,
// JobManager::ShutdownAll, and then each Job's reader thread being
// joined -- so they cannot be reproduced by driving Job directly without
// also rebuilding that whole sequence. What went wrong (and what this
// test fails on if it comes back): the reader thread's only exit
// condition was EOF on the child's stdout/stderr, and two perfectly
// ordinary panes never deliver it.
//
//   1. A terminal that is not on screen. Its raw-output drain gate
//      (Editor's should_poll_raw) is closed while the buffer is hidden,
//      so once kMaxPendingRawBytes is queued the reader stops polling
//      stdout *by design* -- and with the main loop gone, nothing will
//      ever drain it again, so it can never observe EOF either. It spun
//      on its poll timeout forever and ~Job's join() never returned.
//
//   2. A terminal whose PTY slave is still open in a process that left
//      the killed process group (anything that calls setsid). The direct
//      child dies; the master still reports no EOF, because that
//      grandchild holds the other end.
//
// In both cases mep's window stayed on screen, frozen, until the human
// killed the process by hand -- exactly the bug reported as ":qa doesn't
// close it all the way".
//
// Quitting also has to be *quick*, not merely finite, so this asserts a
// wall-clock bound as well. The two costs that bound catches, both
// measured on this machine before they were fixed: a terminal's shell
// sitting out ShutdownAll's whole grace period because an interactive
// shell ignores SIGTERM (half a second per quit), and exit()'s own
// teardown of every linked library after mep's work was already done
// (9.6 seconds on an NVIDIA driver, 17 under llvmpipe).
//
// Usage: mep-quit-live-test /path/to/mep [scratch directory]

#include "agent_rpc_test_harness.h"

#include <csignal>
#include <fstream>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: mep-quit-live-test /path/to/mep [scratch dir]\n");
        return 2;
    }
    const char *mep_path = argv[1];
    const std::string out_dir = argc > 2 ? std::string(argv[2])
                                         : "/tmp/mep-quit-live-" + std::to_string(static_cast<long>(getpid()));
    std::filesystem::create_directories(out_dir);
    const std::string open_me = out_dir + "/plain.txt";
    {
        std::ofstream out(open_me, std::ios::binary);
        CHECK(static_cast<bool>(out));
        out << "a plain file to switch to\n";
    }

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        execl(mep_path, mep_path, "--no-session", nullptr);
        _exit(127);
    }
    const std::string socket_path = MepAgentSocketDir() + "/" + std::to_string(static_cast<long>(pid)) + ".sock";
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        if (std::filesystem::exists(socket_path)) fd = ConnectOnce(socket_path);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK_CTX(fd >= 0, "mep never bound its agent socket within 10s");
    std::string buf;
    int next_id = 1;
    auto run = [&](const std::string &cmd) {
        Json params = Json::Object();
        params["cmd"] = Json(cmd);
        Json r = Call(fd, next_id++, "command.run", params, &buf);
        CHECK_CTX(r.contains("result"), ":" + cmd + " failed: " + r.dump());
    };

    // --- Shape 1: a hidden terminal with a backlog at the cap --------------
    // `yes` fills the 8MB raw-output queue in well under a second once
    // the pane it lives in is showing something else instead.
    run("terminal yes");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    run("e " + open_me);  // same pane: the terminal is now off screen
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // --- Shape 2: a PTY slave held open by an escaped grandchild -----------
    // `setsid` puts that sleep in a session of its own, so the process
    // group SIGTERM/SIGKILL of shutdown misses it entirely while it goes
    // on holding this terminal's PTY open. Short enough not to leave a
    // stray process around for long, far longer than this test's own
    // remaining runtime.
    run("split");
    run("terminal setsid sleep 45 & sleep 45");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // --- Shape 3: a shell at its prompt ------------------------------------
    // The everyday case, and the one that makes the quit *slow* rather
    // than endless: an interactive bash/zsh ignores SIGTERM, so without
    // the SIGHUP a terminal pane now gets, this pane alone added
    // ShutdownAll's entire grace period to every :qa.
    run("split");
    run("terminal");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));  // let it get to its prompt

    // --- :qa! must end the process, not just the main loop -----------------
    run("qa!");
    close(fd);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto started = std::chrono::steady_clock::now();
    int status = 0;
    pid_t waited = 0;
    for (;;) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            CHECK_CTX(false, "mep was still running 20s after :qa! -- its shutdown is hanging again");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK_CTX(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "mep exited abnormally after :qa! (status " + std::to_string(status) + ")");
    CHECK_CTX(!std::filesystem::exists(socket_path), "mep left its agent socket behind");
    // Roughly 0.2s in practice; generous enough not to be flaky on a busy
    // machine, far below either of the regressions described up top.
    CHECK_CTX(took < 5.0, "mep took " + std::to_string(took) + "s to quit -- it should be effectively instant");
    std::printf("quit_live_test passed (:qa! with three terminals -- backlogged, PTY-held and a live shell: %.2fs)\n", took);
    std::filesystem::remove_all(out_dir);
    return 0;
}
