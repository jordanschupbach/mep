#include "collab_session.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

// Invoked with one ws:// capability link by the local integration harness.
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
    assert(left == "alpha" && right == "alpha");
    alice.SetPresence(4, 2);
    bool saw_alice = false;
    for (int i = 0; i < 100 && !saw_alice; ++i) {
        alice.Synchronize(left, &merged);
        bob.Synchronize(right, &merged);
        for (const auto &peer : bob.Collaborators()) saw_alice = peer.name == "Alice" && peer.has_location && peer.row == 4 && peer.col == 2;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(saw_alice);
    left += " A";
    for (int i = 0; i < 100; ++i) {
        if (alice.Synchronize(left, &merged)) left = merged;
        if (bob.Synchronize(right, &merged)) right = merged;
        if (left == right && left.find(" A") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    alice.Stop(); bob.Stop();
    assert(left == right && left.find(" A") != std::string::npos);
    std::cout << "collab_session_test passed: " << left << "\n";
}
