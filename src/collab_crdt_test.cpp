#include "collab_crdt.h"
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
// Always active regardless of NDEBUG -- this project's own default
// CMAKE_BUILD_TYPE is Release (see CMakeLists.txt), which defines NDEBUG,
// and NDEBUG compiles assert(...) to nothing at all, including any
// side-effecting call inside it. A plain assert() here would silently
// skip calling Restore()/comparing Text() in exactly that build, and this
// test would report success regardless of whether the CRDT logic actually
// worked (see agent_rpc_test.cpp's own CHECK() for the same reasoning --
// this mirrors it).
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
 * @brief Exercises TextCrdt convergence between two replicas (concurrent inserts, a delete, and snapshot/restore round-trip), asserting their text stays in sync.
 * @return 0 on success (CHECK failures abort the process)
 */
int main() {
    mep::collab::TextCrdt a("alice"), b("bob");
    const auto hello = a.Insert(0, "hello"); b.Apply(hello);
    const auto left = a.Insert(5, " A"), right = b.Insert(5, " B");
    a.Apply(right); b.Apply(left); CHECK(a.Text() == b.Text());
    const auto erase = a.Erase(0, 1); b.Apply(erase); CHECK(a.Text() == b.Text());
    Json snapshot = a.Snapshot();
    mep::collab::TextCrdt restored; CHECK(restored.Restore(snapshot)); CHECK(restored.Text() == a.Text());
    std::cout << "collab_crdt_test passed\n";
}
