#pragma once

// Shared timing + persistent-results-recording helpers for mep's
// benchmark binaries (CRDT_PERFORMANCE_PLAN.md Phase 1) -- header-only,
// matching json.h's own header-only convention, since this is a small
// leaf utility with no reason to be its own translation unit. No
// external benchmark framework (no GoogleTest/Benchmark anywhere in
// this repo): each bench binary is a plain standalone `main()`, same
// house style as every `src/*_test.cpp`.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace mep::bench {

/**
 * @brief A simple monotonic stopwatch, started at construction.
 */
class Timer {
public:
    Timer() : start_(std::chrono::steady_clock::now()) {}
    /**
     * @brief Elapsed time since construction (or the last Reset()), in milliseconds.
     * @return elapsed milliseconds as a double, for sub-millisecond precision
     */
    double ElapsedMs() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }
    void Reset() { start_ = std::chrono::steady_clock::now(); }

private:
    std::chrono::steady_clock::time_point start_;
};

/**
 * @brief Runs the current git HEAD's short commit hash (via `git rev-parse`), for tagging results.
 * @return the short hash, or "unknown" if git isn't available/this isn't a repo checkout
 */
inline std::string GitShortHash() {
    FILE *pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!pipe) return "unknown";
    char buf[64] = {0};
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result.empty() ? "unknown" : result;
}

/**
 * @brief Current wall-clock time as an ISO-8601 UTC timestamp (second precision), for result records.
 * @return a string like "2026-09-10T12:34:56Z"
 */
inline std::string NowIso8601() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

/**
 * @brief One benchmark scenario's result: a workload size `n`, elapsed wall time, and derived ops/sec.
 */
struct Result {
    std::string scenario;
    long long n = 0;
    double ms = 0.0;
    double OpsPerSec() const { return ms > 0.0 ? (static_cast<double>(n) / ms) * 1000.0 : 0.0; }
};

/**
 * @brief Prints one result to stdout (human-readable, for interactive `just bench` runs) and appends it as one
 * JSON line to `path` (git-tracked history -- CRDT_PERFORMANCE_PLAN.md's persistence requirement). Never
 * overwrites/rotates the file -- callers accumulate history across every run, and `tools/bench_report.py` reads
 * the whole file back to compare the latest run against the previous one per scenario.
 * @param binary which bench binary produced this result (e.g. "text_crdt_bench"), for filtering in the report
 * @param path the JSONL history file to append to (created if missing)
 * @param r the result to record
 */
inline void RecordResult(const std::string &binary, const std::string &path, const Result &r) {
    std::printf("%-40s n=%-10lld %10.3f ms  %14.1f ops/sec\n", r.scenario.c_str(), r.n, r.ms, r.OpsPerSec());
    static const std::string commit = GitShortHash();
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << "{\"timestamp\":\"" << NowIso8601() << "\",\"commit\":\"" << commit << "\",\"binary\":\"" << binary
        << "\",\"scenario\":\"" << r.scenario << "\",\"n\":" << r.n << ",\"ms\":" << r.ms
        << ",\"ops_per_sec\":" << r.OpsPerSec() << "}\n";
}

}  // namespace mep::bench
