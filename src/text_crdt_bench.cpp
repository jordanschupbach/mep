// CRDT_PERFORMANCE_PLAN.md Phase 1: persistent benchmarks for
// mep::collab::TextCrdt, run BEFORE any internals rewrite to record a
// real baseline (see bench_results/history.jsonl), then re-run
// unmodified after Phase 2's treap rewrite to prove the win -- same
// scenarios, same binary, just a different TextCrdt implementation
// underneath.

#include "collab_crdt.h"
#include "bench_util.h"
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using mep::bench::RecordResult;
using mep::bench::Result;
using mep::bench::Timer;
using mep::collab::CrdtOperation;
using mep::collab::TextCrdt;

namespace {

constexpr const char *kHistoryPath = "bench_results/history.jsonl";
constexpr const char *kBinary = "text_crdt_bench";

/**
 * @brief Sequential single-character inserts at the end of the document, staged at doubling cumulative sizes, each
 * stage timed separately -- the key scenario for showing per-op cost growth as the document grows (a healthy
 * O(log N)-per-op implementation holds roughly flat ops/sec across stages; today's O(N)-per-op VisibleIds() walk
 * degrades roughly linearly with document size).
 */
void BenchSequentialTyping() {
    TextCrdt doc("bench");
    size_t done = 0;
    // Capped at 10000 (not higher, unlike the buffer-bench equivalent):
    // the current O(N)-per-op TextCrdt makes this scenario O(N^2)
    // overall by design (it's the one demonstrating that degradation),
    // and a `just bench` run needs to stay fast enough to run
    // routinely -- this range already shows the trend clearly (ops/sec
    // roughly halving each time N doubles).
    for (size_t target : {1000UL, 2000UL, 4000UL, 6000UL, 8000UL, 10000UL}) {
        size_t stage_n = target - done;
        Timer t;
        for (size_t i = 0; i < stage_n; i++) doc.Insert(done + i, "x");
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"sequential_typing_at_" + std::to_string(target), static_cast<long long>(stage_n), ms});
        done = target;
    }
    if (doc.VisibleSize() != done) {
        std::fprintf(stderr, "BenchSequentialTyping: expected %zu visible bytes, got %zu\n", done, doc.VisibleSize());
        std::exit(1);
    }
}

/**
 * @brief One large multi-KB Insert() call (e.g. a paste), against documents of increasing pre-existing size --
 * isolates the cost of a single big splice rather than many small ones.
 */
void BenchLargePaste() {
    std::string chunk(50000, 'y');
    for (size_t preexisting : {0UL, 20000UL, 100000UL}) {
        TextCrdt doc("bench");
        if (preexisting > 0) doc.Insert(0, std::string(preexisting, 'z'));
        Timer t;
        doc.Insert(doc.VisibleSize(), chunk);
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"large_paste_into_doc_of_" + std::to_string(preexisting), static_cast<long long>(chunk.size()), ms});
    }
}

/**
 * @brief A mix of random-position single-character inserts and deletes against a pre-populated document --
 * approximates real editing (not just append-only typing), where offsets land throughout the document.
 */
void BenchRandomEdits() {
    std::mt19937 rng(42);
    for (size_t preexisting : {5000UL, 20000UL}) {
        TextCrdt doc("bench");
        doc.Insert(0, std::string(preexisting, 'a'));
        constexpr size_t kOps = 2000;
        Timer t;
        for (size_t i = 0; i < kOps; i++) {
            size_t size = doc.VisibleSize();
            if (size == 0) break;
            size_t pos = rng() % size;
            if (rng() % 2 == 0) {
                doc.Insert(pos, "q");
            } else {
                doc.Erase(pos, 1);
            }
        }
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"random_edits_in_doc_of_" + std::to_string(preexisting), static_cast<long long>(kOps), ms});
    }
}

/**
 * @brief Many-site convergence: N replicas each locally type M characters (interleaved, simulating N people typing
 * concurrently), then every op is broadcast to every other replica -- measures total merge/integration time and
 * confirms all replicas converge to the same text (a correctness check embedded in the benchmark, not just a
 * timing number).
 */
void BenchManySiteConvergence() {
    for (auto [sites, ops_per_site] : {std::pair<int, int>{4, 200}, std::pair<int, int>{16, 100}}) {
        std::vector<TextCrdt> replicas;
        for (int i = 0; i < sites; i++) replicas.emplace_back("site" + std::to_string(i));
        std::vector<CrdtOperation> all_ops;
        Timer t;
        for (int i = 0; i < sites; i++) {
            for (int j = 0; j < ops_per_site; j++) {
                auto ops = replicas[static_cast<size_t>(i)].Insert(replicas[static_cast<size_t>(i)].VisibleSize(), "s");
                for (auto &op : ops) all_ops.push_back(op);
            }
        }
        for (int i = 0; i < sites; i++) {
            for (const auto &op : all_ops) replicas[static_cast<size_t>(i)].Apply(op);
        }
        double ms = t.ElapsedMs();
        long long total_ops = static_cast<long long>(sites) * ops_per_site;
        RecordResult(kBinary, kHistoryPath,
                      {"convergence_" + std::to_string(sites) + "sites_x" + std::to_string(ops_per_site), total_ops, ms});
        const std::string &expected = replicas[0].Text();
        for (int i = 1; i < sites; i++) {
            if (replicas[static_cast<size_t>(i)].Text() != expected) {
                std::fprintf(stderr, "BenchManySiteConvergence: replica %d diverged\n", i);
                std::exit(1);
            }
        }
    }
}

/**
 * @brief Snapshot()/Restore() round-trip cost against documents of increasing size (including tombstones, which
 * Snapshot serializes too).
 */
void BenchSnapshotRestore() {
    for (size_t size : {5000UL, 20000UL}) {
        TextCrdt doc("bench");
        doc.Insert(0, std::string(size, 'a'));
        doc.Erase(0, size / 4);  // realistic tombstone fraction
        Timer t;
        Json snap = doc.Snapshot();
        double snap_ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"snapshot_doc_of_" + std::to_string(size), static_cast<long long>(size), snap_ms});

        TextCrdt restored;
        Timer t2;
        bool ok = restored.Restore(snap);
        double restore_ms = t2.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"restore_doc_of_" + std::to_string(size), static_cast<long long>(size), restore_ms});
        if (!ok || restored.Text() != doc.Text()) {
            std::fprintf(stderr, "BenchSnapshotRestore: round-trip mismatch at size %zu\n", size);
            std::exit(1);
        }
    }
}

}  // namespace

int main() {
    BenchSequentialTyping();
    BenchLargePaste();
    BenchRandomEdits();
    BenchManySiteConvergence();
    BenchSnapshotRestore();
    std::printf("text_crdt_bench done\n");
    return 0;
}
