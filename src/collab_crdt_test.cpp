#include "collab_crdt.h"
#include "json.h"

#include <cassert>
#include <iostream>

/**
 * @brief Exercises TextCrdt convergence between two replicas (concurrent inserts, a delete, and snapshot/restore round-trip), asserting their text stays in sync.
 * @return 0 on success (assertion failures abort the process)
 */
int main() {
    mep::collab::TextCrdt a("alice"), b("bob");
    const auto hello = a.Insert(0, "hello"); b.Apply(hello);
    const auto left = a.Insert(5, " A"), right = b.Insert(5, " B");
    a.Apply(right); b.Apply(left); assert(a.Text() == b.Text());
    const auto erase = a.Erase(0, 1); b.Apply(erase); assert(a.Text() == b.Text());
    Json snapshot = a.Snapshot(); mep::collab::TextCrdt restored; assert(restored.Restore(snapshot)); assert(restored.Text() == a.Text());
    std::cout << "collab_crdt_test passed\n";
}
