#include "collab_session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
// Always active regardless of NDEBUG -- this project's own default
// CMAKE_BUILD_TYPE is Release (see CMakeLists.txt), which defines NDEBUG,
// and NDEBUG compiles assert(...) to nothing at all, including any
// side-effecting call inside it. A plain assert() here would silently
// skip every convergence check in exactly that build, and this test
// would report success regardless of whether sync actually worked (see
// agent_rpc_test.cpp's own CHECK() for the same reasoning -- this
// mirrors it).
/**
 * @brief Prints a CHECK-failure message (with file/line) to stderr and aborts the process.
 * @param expr The source text of the failed condition.
 * @param file The source file the check ran in.
 * @param line The source line the check ran on.
 */
void CheckFailed(const char *expr, const char *file, int line) {
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, file, line);
    std::fflush(stderr);
    std::abort();
}
}  // namespace
#define CHECK(cond) ((cond) ? (void)0 : CheckFailed(#cond, __FILE__, __LINE__))

// Invoked with one ws:// capability link by the local integration harness.
/**
 * @brief Integration test entry point: connects two CollabSession clients to the same room and checks that
 * initial text, presence, and a later edit all converge between them.
 * @param argc Argument count; must be 2 (program name plus the capability-link URL).
 * @param argv Argument vector; argv[1] is the ws:// capability-link URL to connect to.
 * @return 0 on success, 2 if the URL argument is missing.
 */
int main(int argc, char **argv) {
    if (argc != 2) { std::cerr << "usage: mep-collab-session-test ws://...\n"; return 2; }
    mep::collab::CollabSession alice(argv[1], "Alice", "alpha");
    mep::collab::CollabSession bob(argv[1], "Bob", "");
    alice.Start(); bob.Start();
    std::string left = "alpha", right, merged;
    for (int i = 0; i < 100; ++i) {
        if (alice.Synchronize(left, &merged)) left = merged;
        if (bob.Synchronize(right, &merged)) right = merged;
        if (left == right && left == "alpha" && alice.connected() && bob.connected()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(left == "alpha" && right == "alpha");
    alice.SetPresence(4, 2);
    bool saw_alice = false;
    for (int i = 0; i < 100 && !saw_alice; ++i) {
        alice.Synchronize(left, &merged);
        bob.Synchronize(right, &merged);
        for (const auto &peer : bob.Collaborators()) saw_alice = peer.name == "Alice" && peer.has_location && peer.row == 4 && peer.col == 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(saw_alice);
    left += " A";
    for (int i = 0; i < 100; ++i) {
        if (alice.Synchronize(left, &merged)) left = merged;
        if (bob.Synchronize(right, &merged)) right = merged;
        if (left == right && left.find(" A") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    alice.Stop(); bob.Stop();
    CHECK(left == right && left.find(" A") != std::string::npos);
    std::cout << "collab_session_test passed: " << left << "\n";
}
