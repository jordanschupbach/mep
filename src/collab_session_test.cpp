#include "collab_session.h"
#include "json.h"

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

/**
 * @brief Returns the CRDT node ids covering visible positions [from, to) of `snapshot`, in document order --
 * used to check whether a span of text is still backed by its original nodes (vs. having been deleted and
 * reinserted as brand-new ones).
 * @param snapshot A TextCrdt::Snapshot() result.
 * @param from Start of the visible-position range, inclusive.
 * @param to End of the visible-position range, exclusive.
 * @return One "actor:counter" string per visible node in the range, in document order.
 */
std::vector<std::string> VisibleIdsInRange(const Json &snapshot, size_t from, size_t to) {
    std::vector<std::string> ids;
    size_t pos = 0;
    for (const Json &node : snapshot.get("nodes").items()) {
        if (node.get("deleted").as_bool()) continue;
        if (pos >= from && pos < to) {
            const Json &id = node.get("id");
            ids.push_back(id.get("actor").as_string() + ":" + std::to_string(id.get("counter").as_int()));
        }
        pos++;
    }
    return ids;
}

// CRDT_PERFORMANCE_PLAN.md Phase 3 regression test: two disjoint edits to the same text between two
// DiffIntoCrdt calls must each land as their own minimal splice, not get collapsed into one wide replace that
// needlessly deletes and reinserts the unchanged content sitting between them. Runs standalone -- DiffIntoCrdt
// only touches a local TextCrdt, no relay needed -- so it always executes under `just test` even though the
// rest of this binary's checks (in main(), below) require a live capability-link relay and are skipped without
// one.
/**
 * @brief Asserts that DiffIntoCrdt applies two disjoint edits as independent splices, preserving the CRDT node
 * identity of the unchanged text sitting between them rather than replacing the whole span.
 */
void TestDisjointEditsPreserveUnchangedNodes() {
    // Two edits on different lines, with an unchanged line between them -- DiffIntoCrdt localizes changed
    // regions by line (via MyersDiffHunks) before narrowing within each one, so this is the shape of edit
    // that distinguishes it from a single common-prefix/suffix diff: two edits on the *same* line still
    // collapse into one hunk (by design -- see DiffIntoCrdt's doc comment), but edits on different lines with
    // an unchanged line between them must not disturb that unchanged line's node identity.
    mep::collab::TextCrdt document("tester");
    const std::string old_text = "head line one\nunchanged middle line\ntail line three";
    document.Insert(0, old_text);
    const std::string unchanged = "unchanged middle line";
    const size_t old_from = old_text.find(unchanged);
    CHECK(old_from != std::string::npos);
    const auto ids_before = VisibleIdsInRange(document.Snapshot(), old_from, old_from + unchanged.size());
    CHECK(ids_before.size() == unchanged.size());

    const std::string new_text = "changed HEAD\nunchanged middle line\nchanged TAIL";
    mep::collab::DiffIntoCrdt(document, old_text, new_text);
    CHECK(document.Text() == new_text);

    const size_t new_from = new_text.find(unchanged);
    CHECK(new_from != std::string::npos);
    const auto ids_after = VisibleIdsInRange(document.Snapshot(), new_from, new_from + unchanged.size());
    // A single common-prefix/suffix diff would find no common prefix ('h' vs 'c') or suffix ('e' vs 'L') at
    // all here and replace the entire string, tombstoning and recreating the unchanged middle line's nodes
    // too -- which this asserts against by requiring the exact same ids to still cover it.
    CHECK(ids_after == ids_before);
}

// Invoked with one ws:// capability link by the local integration harness.
/**
 * @brief Integration test entry point: connects two CollabSession clients to the same room and checks that
 * initial text, presence, and a later edit all converge between them.
 * @param argc Argument count; must be 2 (program name plus the capability-link URL).
 * @param argv Argument vector; argv[1] is the ws:// capability-link URL to connect to.
 * @return 0 on success, 2 if the URL argument is missing.
 */
int main(int argc, char **argv) {
    TestDisjointEditsPreserveUnchangedNodes();
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
