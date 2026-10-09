// Coverage for text_diff.cppm/.cpp's Myers diff.
//
// Two things are being pinned down here. The first is the ordinary
// contract: hunk starts are 1-indexed positions into their own side,
// and applying the hunks turns `a` into `b`. The second is the bound -- kMaxEditDistance and the trimmed
// backtracking trace that goes with it, added after a 15MB PDF reached
// this function and asked for ~35GB of trace (see text_diff.cppm). The
// randomized section is what actually guards the trimming: it checks
// every generated diff against a from-scratch replay of the edit
// script, which catches an off-by-one in the trace indexing that a
// handful of fixed cases would sail past.

import mep.diff;

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using mep::diff::DiffHunk;
using mep::diff::MyersDiffHunks;

std::vector<std::string> Lines(std::initializer_list<const char *> l) {
    return std::vector<std::string>(l.begin(), l.end());
}

// Rebuilds `b` from `a` by applying the hunks, which is the real
// statement of correctness: every hunk replaces old_count lines of `a`
// starting at old_start with the new_count lines of `b` starting at
// new_start (both 1-indexed), and everything between hunks is shared.
std::vector<std::string> Apply(const std::vector<std::string> &a, const std::vector<std::string> &b,
                                const std::vector<DiffHunk> &hunks) {
    std::vector<std::string> out;
    int a_at = 0;
    for (const DiffHunk &h : hunks) {
        // old_start/new_start are 1-indexed positions into a/b, for an
        // insertion too (it names the line the inserted text pushes down).
        int cut = h.old_start - 1;
        while (a_at < cut && a_at < static_cast<int>(a.size())) out.push_back(a[static_cast<size_t>(a_at++)]);
        a_at += h.old_count;
        for (int i = 0; i < h.new_count; i++) {
            int bi = h.new_start - 1 + i;
            if (bi >= 0 && bi < static_cast<int>(b.size())) out.push_back(b[static_cast<size_t>(bi)]);
        }
    }
    while (a_at < static_cast<int>(a.size())) out.push_back(a[static_cast<size_t>(a_at++)]);
    return out;
}

void TestIdenticalIsNoHunks() {
    const auto a = Lines({"one", "two", "three"});
    CHECK(MyersDiffHunks(a, a).empty());
    CHECK(MyersDiffHunks({}, {}).empty());
}

void TestSingleLineChange() {
    const auto a = Lines({"one", "two", "three"});
    const auto b = Lines({"one", "TWO", "three"});
    const auto h = MyersDiffHunks(a, b);
    CHECK(h.size() == 1);
    CHECK(h[0].old_start == 2 && h[0].old_count == 1);
    CHECK(h[0].new_start == 2 && h[0].new_count == 1);
    CHECK(Apply(a, b, h) == b);
}

void TestInsertionAnchoredAtPrecedingLine() {
    const auto a = Lines({"one", "two"});
    const auto b = Lines({"one", "inserted", "two"});
    const auto h = MyersDiffHunks(a, b);
    CHECK(h.size() == 1);
    CHECK(h[0].old_count == 0);
    CHECK(h[0].old_start == 2);  // the line the insertion pushes down
    CHECK(h[0].new_start == 2 && h[0].new_count == 1);
    CHECK(Apply(a, b, h) == b);

    // At the very top: both starts are line 1.
    const auto c = Lines({"first", "one", "two"});
    const auto h2 = MyersDiffHunks(a, c);
    CHECK(h2.size() == 1);
    CHECK(h2[0].old_count == 0 && h2[0].old_start == 1 && h2[0].new_start == 1);
    CHECK(Apply(a, c, h2) == c);
}

void TestDeletionAndEmptySides() {
    const auto a = Lines({"one", "two", "three"});
    const auto b = Lines({"one", "three"});
    const auto h = MyersDiffHunks(a, b);
    CHECK(h.size() == 1);
    CHECK(h[0].old_start == 2 && h[0].old_count == 1 && h[0].new_count == 0);
    CHECK(Apply(a, b, h) == b);

    // A brand-new file: every line is an addition.
    const auto add = MyersDiffHunks({}, a);
    CHECK(add.size() == 1);
    CHECK(add[0].old_count == 0 && add[0].new_count == 3);
    // ...and a emptied one, all deletions.
    const auto del = MyersDiffHunks(a, {});
    CHECK(del.size() == 1);
    CHECK(del[0].old_count == 3 && del[0].new_count == 0);
}

void TestSeparateHunksStaySeparate() {
    const auto a = Lines({"a", "b", "c", "d", "e", "f", "g"});
    const auto b = Lines({"a", "B", "c", "d", "e", "F", "g"});
    const auto h = MyersDiffHunks(a, b);
    CHECK(h.size() == 2);
    CHECK(h[0].new_start == 2 && h[1].new_start == 6);
    CHECK(Apply(a, b, h) == b);
}

// Two sides with nothing in common at all -- the shape that used to
// make this quadratic in the input. It must come back promptly, and
// with a usable answer rather than an empty one.
void TestHugeEditDistanceDegradesToOneHunk() {
    std::vector<std::string> a, b;
    for (int i = 0; i < 40000; i++) a.push_back("old line " + std::to_string(i));
    b.push_back("");
    const auto h = MyersDiffHunks(a, b);
    CHECK(h.size() == 1);
    CHECK(h[0].old_count == 40000);
    CHECK(h[0].new_count == 1);

    // Same input as a brand-new file (the other half of the real case:
    // a viewer buffer's single empty line against a tracked file).
    const auto h2 = MyersDiffHunks(b, a);
    CHECK(h2.size() == 1);
    CHECK(h2[0].old_count == 1 && h2[0].new_count == 40000);
}

// A big file with a small edit stays exact however big it is: D is what
// the bound is on, not N.
void TestLargeFileSmallEditStaysExact() {
    std::vector<std::string> a;
    for (int i = 0; i < 50000; i++) a.push_back("line " + std::to_string(i));
    std::vector<std::string> b = a;
    b[30000] = "changed";
    const auto h = MyersDiffHunks(a, b);
    CHECK(h.size() == 1);
    CHECK(h[0].old_start == 30001 && h[0].old_count == 1 && h[0].new_count == 1);
}

void TestRandomizedRoundTrip() {
    std::mt19937 rng(12345);
    for (int iter = 0; iter < 400; iter++) {
        std::uniform_int_distribution<int> len(0, 40);
        std::uniform_int_distribution<int> sym(0, 4);
        std::vector<std::string> a, b;
        int na = len(rng), nb = len(rng);
        for (int i = 0; i < na; i++) a.push_back(std::string(1, static_cast<char>('a' + sym(rng))));
        for (int i = 0; i < nb; i++) b.push_back(std::string(1, static_cast<char>('a' + sym(rng))));
        const auto h = MyersDiffHunks(a, b);
        CHECK(Apply(a, b, h) == b);
        // Hunks must be ordered and non-overlapping in both sequences.
        int prev_a = 0, prev_b = 0;
        for (const DiffHunk &x : h) {
            CHECK(x.old_count > 0 || x.new_count > 0);
            CHECK(x.old_start >= prev_a);
            CHECK(x.new_start >= prev_b);
            prev_a = x.old_start + x.old_count;
            prev_b = x.new_start + x.new_count;
        }
    }
}

}  // namespace

int main() {
    TestIdenticalIsNoHunks();
    TestSingleLineChange();
    TestInsertionAnchoredAtPrecedingLine();
    TestDeletionAndEmptySides();
    TestSeparateHunksStaySeparate();
    TestHugeEditDistanceDegradesToOneHunk();
    TestLargeFileSmallEditStaysExact();
    TestRandomizedRoundTrip();
    std::printf("text_diff_test: all checks passed\n");
    return 0;
}
