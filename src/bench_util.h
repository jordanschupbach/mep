#pragma once

// Shared harness for mep's benchmark binaries (plans/PERFORMANCE_PLAN.md;
// first written for CRDT_PERFORMANCE_PLAN.md Phase 1) -- header-only,
// matching json.h's own header-only convention, since this is a small
// leaf utility with no reason to be its own translation unit. No
// external benchmark framework (no GoogleTest/Benchmark anywhere in
// this repo): each bench binary is a plain standalone `main()`, same
// house style as every `src/*_test.cpp`.
//
// Every binary built on this understands the same command line, which is
// what lets tools/bench.py treat them as one suite:
//
//   --list            print every benchmark id, one per line, and exit
//   --filter PAT      run only matching benchmarks (repeatable; see Matches)
//   --quick           fewer repetitions and smaller minimum time
//   --min-time MS     keep repeating a measurement until this much time is spent (default 300)
//   --reps N          at least this many timed repetitions (default 5)
//   --no-record       do not append to bench_results/history.jsonl
//   --json PATH       also append this run's records to PATH (the runner's collection file)
//
// Benchmark ids are `suite/group` (an entry, the unit --list prints) and
// `suite/group/scenario` (one timed measurement inside it). History
// records keep each binary's original (binary, scenario) naming so the
// accumulated history.jsonl stays comparable across this change.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
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
 * @return the short hash (with a `+dirty` suffix when the work tree has changes), or "unknown"
 */
inline std::string GitShortHash() {
    auto run = [](const char *cmd) {
        FILE *pipe = popen(cmd, "r");
        if (!pipe) return std::string();
        char buf[256] = {0};
        std::string result;
        while (fgets(buf, sizeof(buf), pipe)) result += buf;
        pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
        return result;
    };
    std::string hash = run("git rev-parse --short HEAD 2>/dev/null");
    if (hash.empty()) return "unknown";
    if (!run("git status --porcelain --untracked-files=no 2>/dev/null | head -1").empty()) hash += "+dirty";
    return hash;
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
 *
 * `ms` is the headline number (the median when the scenario was repeated); the optional spread fields are
 * filled by Measure() and left at zero by callers that time a single run themselves.
 */
struct Result {
    std::string scenario;
    long long n = 0;
    double ms = 0.0;
    double min_ms = 0.0;
    double p95_ms = 0.0;
    int reps = 0;
    double OpsPerSec() const { return ms > 0.0 ? (static_cast<double>(n) / ms) * 1000.0 : 0.0; }
};

/** @brief Command-line options shared by every bench binary. */
struct Options {
    std::vector<std::string> filters;
    bool list = false;
    bool record = true;
    bool quick = false;
    double min_time_ms = 300.0;
    int min_reps = 5;
    int max_reps = 1000;
    std::string history = "bench_results/history.jsonl";
    std::string json_out;
};

inline Options &Opts() {
    static Options o;
    return o;
}

/**
 * @brief Shell-style glob (`*`, `?`) over a whole string; `*` also crosses `/`.
 */
inline bool Glob(const char *p, const char *s) {
    const char *star = nullptr;
    const char *resume = nullptr;
    while (*s) {
        if (*p == '*') {
            star = p++;
            resume = s;
        } else if (*p == '?' || *p == *s) {
            p++;
            s++;
        } else if (star) {
            p = star + 1;
            s = ++resume;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == 0;
}

/**
 * @brief Glob match where a pattern with no glob characters is a prefix: `editor` matches `editor/open_cpp`.
 */
inline bool GlobMatch(const std::string &pattern, const std::string &id) {
    std::string p = pattern;
    if (p.find_first_of("*?") == std::string::npos) p += "*";
    return Glob(p.c_str(), id.c_str());
}

/**
 * @brief Whether a measurement with full id `id` was asked for (always true without --filter).
 */
inline bool Wanted(const std::string &id) {
    if (Opts().filters.empty()) return true;
    for (const std::string &f : Opts().filters)
        if (GlobMatch(f, id)) return true;
    return false;
}

/**
 * @brief Whether the entry `id` could contain a wanted measurement: the filter matches the entry itself, or
 * its first path segments (as many as the entry has) match it -- so `editor/open*` runs entry `editor/open`
 * and `editor/open/cpp_60k` too.
 */
inline bool EntryWanted(const std::string &id) {
    if (Opts().filters.empty()) return true;
    const size_t depth = static_cast<size_t>(std::count(id.begin(), id.end(), '/')) + 1;
    for (const std::string &f : Opts().filters) {
        if (GlobMatch(f, id)) return true;
        // The filter's first `depth` segments against the entry itself.
        size_t pos = std::string::npos;
        size_t start = 0;
        for (size_t k = 0; k < depth; k++) {
            pos = f.find('/', start);
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        if (pos != std::string::npos && Glob(f.substr(0, pos).c_str(), id.c_str())) return true;
    }
    return false;
}

namespace detail {

inline std::string JsonEscape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    return out;
}

inline std::string &CurrentEntry() {
    static std::string e;
    return e;
}

/**
 * @brief The full id of `scenario` inside entry `entry`. A scenario already named after the entry's last
 * segment (mepml's `synth_1000/ts_parse` inside entry `mepml/synth_1000`) is not repeated.
 */
inline std::string ScenarioId(const std::string &entry, const std::string &scenario) {
    if (entry.empty()) return scenario;
    const std::string last = entry.substr(entry.rfind('/') + 1);
    if (scenario.size() > last.size() && scenario.compare(0, last.size(), last) == 0 && scenario[last.size()] == '/')
        return entry + scenario.substr(last.size());
    return entry + "/" + scenario;
}

}  // namespace detail

/**
 * @brief Prints one result to stdout and appends it as one JSON line to `path` (the git-tracked, append-only
 * history -- tools/bench_report.py and tools/bench.py read it back) and to --json's file when given.
 * Respects --no-record, and --filter at scenario granularity: a scenario outside the filter is dropped.
 * @param binary which bench binary produced this result (e.g. "text_crdt_bench"), for filtering in the report
 * @param path the JSONL history file to append to (created if missing)
 * @param r the result to record
 */
inline void RecordResult(const std::string &binary, const std::string &path, const Result &r) {
    const std::string &entry = detail::CurrentEntry();
    const std::string id = entry.empty() ? binary + "/" + r.scenario : detail::ScenarioId(entry, r.scenario);
    if (!Wanted(id) && !(entry.empty() ? false : Wanted(entry))) return;
    std::printf("%-56s n=%-9lld %10.3f ms", id.c_str(), r.n, r.ms);
    if (r.reps > 1) std::printf("  (min %.3f, p95 %.3f, %d reps)", r.min_ms, r.p95_ms, r.reps);
    std::printf("\n");
    std::fflush(stdout);
    static const std::string commit = GitShortHash();
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "{\"timestamp\":\"%s\",\"commit\":\"%s\",\"binary\":\"%s\",\"scenario\":\"%s\",\"id\":\"%s\","
                  "\"n\":%lld,\"ms\":%.6g,\"ops_per_sec\":%.6g,\"min_ms\":%.6g,\"p95_ms\":%.6g,\"reps\":%d}\n",
                  NowIso8601().c_str(), commit.c_str(), detail::JsonEscape(binary).c_str(),
                  detail::JsonEscape(r.scenario).c_str(), detail::JsonEscape(id).c_str(), r.n, r.ms, r.OpsPerSec(),
                  r.min_ms > 0 ? r.min_ms : r.ms, r.p95_ms > 0 ? r.p95_ms : r.ms, r.reps > 0 ? r.reps : 1);
    if (Opts().record) {
        std::ofstream out(path.empty() ? Opts().history : path, std::ios::app);
        if (out) out << line;
    }
    if (!Opts().json_out.empty()) {
        std::ofstream out(Opts().json_out, std::ios::app);
        if (out) out << line;
    }
}

/**
 * @brief Times `fn` repeatedly and summarizes: one untimed warm-up, then at least --reps timed runs and at
 * least --min-time of total time (capped at max_reps, and at 3 reps for anything over a second per run).
 * @param fn the operation; it must leave its state so that calling it again does the same work
 * @return the median/min/p95 of the timed runs
 */
template <class F>
Result TimeRepeated(const std::string &scenario, long long n, F &&fn) {
    const Options &o = Opts();
    Timer first;
    fn();
    const double est = first.ElapsedMs();
    int min_reps = o.quick ? 3 : o.min_reps;
    double min_time = o.quick ? o.min_time_ms / 4 : o.min_time_ms;
    if (est > 1000.0) min_reps = std::min(min_reps, 3);
    if (est > 5000.0) min_reps = 1;
    std::vector<double> ms;
    double total = 0.0;
    while (static_cast<int>(ms.size()) < min_reps ||
           (total < min_time && static_cast<int>(ms.size()) < o.max_reps)) {
        Timer t;
        fn();
        const double e = t.ElapsedMs();
        ms.push_back(e);
        total += e;
        if (est > 1000.0 && static_cast<int>(ms.size()) >= min_reps) break;
    }
    std::sort(ms.begin(), ms.end());
    Result r;
    r.scenario = scenario;
    r.n = n;
    r.ms = ms[ms.size() / 2];
    r.min_ms = ms.front();
    r.p95_ms = ms[std::min(ms.size() - 1, static_cast<size_t>(std::ceil(static_cast<double>(ms.size()) * 0.95)) - 1)];
    r.reps = static_cast<int>(ms.size());
    return r;
}

class Session;
/** @brief The Session the running binary's main() made, for entries registered through MEP_BENCH. */
inline Session *&Current() {
    static Session *s = nullptr;
    return s;
}

/**
 * @brief A bench binary's set of entries plus its run loop: parses the shared options, then runs (or lists)
 * every registered entry the filter selects. Entries also self-register into Global() via MEP_BENCH.
 */
class Session {
public:
    struct Entry {
        std::string id;
        std::function<void()> fn;
    };

    static std::vector<Entry> &Global() {
        static std::vector<Entry> g;
        return g;
    }

    /**
     * @param binary the history `binary` field for this executable's records
     */
    Session(int argc, char **argv, std::string binary) : binary_(std::move(binary)) {
        Current() = this;
        Options &o = Opts();
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
            if (a == "--list") o.list = true;
            else if (a == "--filter") o.filters.push_back(next());
            else if (a == "--quick") o.quick = true;
            else if (a == "--no-record") o.record = false;
            else if (a == "--json") o.json_out = next();
            else if (a == "--min-time") o.min_time_ms = std::atof(next().c_str());
            else if (a == "--reps") o.min_reps = std::max(1, std::atoi(next().c_str()));
            else if (a == "--help" || a == "-h") help_ = true;
            else rest_.push_back(a);
        }
    }

    /** @brief Arguments the shared parser did not consume (binary-specific flags and positional args). */
    const std::vector<std::string> &Rest() const { return rest_; }
    const std::string &Binary() const { return binary_; }

    void Add(std::string id, std::function<void()> fn) { entries_.push_back({std::move(id), std::move(fn)}); }

    /**
     * @brief Measures `fn` (see TimeRepeated) under the current entry's id and records it, unless the
     * scenario is filtered out (in which case `fn` never runs).
     */
    template <class F>
    void Measure(const std::string &scenario, long long n, F &&fn) {
        const std::string &entry = detail::CurrentEntry();
        const std::string id = detail::ScenarioId(entry, scenario);
        if (!Wanted(id) && !Wanted(entry)) return;
        RecordResult(binary_, Opts().history, TimeRepeated(scenario, n, std::forward<F>(fn)));
    }

    /**
     * @brief Lists or runs the entries.
     * @return process exit status
     */
    int Run() {
        std::vector<Entry> all = Global();
        all.insert(all.end(), entries_.begin(), entries_.end());
        if (help_) {
            std::printf("usage: %s [--list] [--filter PAT]... [--quick] [--min-time MS] [--reps N] [--no-record] "
                        "[--json PATH]\n",
                        binary_.c_str());
            return 0;
        }
        if (Opts().list) {
            for (const Entry &e : all) std::printf("%s\n", e.id.c_str());
            return 0;
        }
        int ran = 0;
        for (const Entry &e : all) {
            if (!EntryWanted(e.id)) continue;
            detail::CurrentEntry() = e.id;
            e.fn();
            ran++;
        }
        detail::CurrentEntry().clear();
        if (ran == 0 && !Opts().filters.empty()) std::fprintf(stderr, "%s: no benchmark matches the filter\n", binary_.c_str());
        return 0;
    }

private:
    std::string binary_;
    std::vector<std::string> rest_;
    std::vector<Entry> entries_;
    bool help_ = false;
};

/** @brief Measures through the current Session (see Session::Measure). */
template <class F>
void Measure(const std::string &scenario, long long n, F &&fn) {
    Current()->Measure(scenario, n, std::forward<F>(fn));
}

struct Registrar {
    Registrar(const char *id, void (*fn)()) { Session::Global().push_back({id, fn}); }
};

/** @brief Keeps the optimizer from deleting a computation whose result is otherwise unused. */
template <class T>
inline void DoNotOptimize(const T &value) {
#if defined(_MSC_VER)
    static volatile const void *sink;
    sink = &value;
#else
    asm volatile("" : : "r,m"(value) : "memory");
#endif
}

}  // namespace mep::bench

#define MEP_BENCH_CONCAT2(a, b) a##b
#define MEP_BENCH_CONCAT(a, b) MEP_BENCH_CONCAT2(a, b)
/**
 * @brief Defines and registers a benchmark entry: `MEP_BENCH("core/regex") { mep::bench::Measure(...); }`.
 */
#define MEP_BENCH(id)                                                                                               \
    static void MEP_BENCH_CONCAT(mep_bench_fn_, __LINE__)();                                                        \
    static const ::mep::bench::Registrar MEP_BENCH_CONCAT(mep_bench_reg_, __LINE__)(                                \
        id, &MEP_BENCH_CONCAT(mep_bench_fn_, __LINE__));                                                            \
    static void MEP_BENCH_CONCAT(mep_bench_fn_, __LINE__)()
