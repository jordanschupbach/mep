// mep-project-search-test: the builtin ripgrep (project_search.h) and the
// in-process Job that runs it -- glob matching, rg argv parsing, the
// ignore-aware walk, grep output shape, and cancellation.
//
// Usage: mep-project-search-test

#include "job.h"
#include "project_search.h"
#include "regex.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

/**
 * @brief Checks a condition, reporting it on stderr when false.
 * @param cond The condition that should hold.
 * @param what Description of what was expected.
 */
void Check(bool cond, const std::string &what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

/**
 * @brief Writes `text` to `path`, creating parent directories.
 * @param path File to write.
 * @param text Its contents.
 */
void WriteFile(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/**
 * @brief Runs a parsed rg argv against `cwd` and collects the output, sorted.
 * @param argv The rg argv.
 * @param cwd Directory to search from.
 * @param code Receives the exit status.
 * @return Output lines, sorted.
 */
std::vector<std::string> RunRg(const std::vector<std::string> &argv, const fs::path &cwd, int *code = nullptr) {
    std::vector<std::string> out;
    auto opts = mep_search::ParseRgArgs(argv);
    if (!opts) {
        Check(false, "argv parses");
        return out;
    }
    std::atomic<bool> cancelled{false};
    int rc = mep_search::Run(*opts, cwd.string(), [&](const std::string &l) { out.push_back(l); }, cancelled);
    if (code) *code = rc;
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * @brief Joins lines for failure messages.
 * @param v Lines.
 * @return Them, separated by " | ".
 */
std::string Join(const std::vector<std::string> &v) {
    std::string s;
    for (const auto &l : v) s += (s.empty() ? "" : " | ") + l;
    return s;
}

void TestGlob() {
    using mep_search::GlobMatch;
    Check(GlobMatch("*.cpp", "a.cpp"), "*.cpp matches a.cpp");
    Check(!GlobMatch("*.cpp", "src/a.cpp"), "* does not cross /");
    Check(GlobMatch("**/a.cpp", "a.cpp"), "**/ matches zero dirs");
    Check(GlobMatch("**/a.cpp", "x/y/a.cpp"), "**/ matches several dirs");
    Check(GlobMatch("a/**/b", "a/b"), "a/**/b matches a/b");
    Check(GlobMatch("a/**/b", "a/x/y/b"), "a/**/b matches a/x/y/b");
    Check(GlobMatch("build/**", "build/x/y"), "trailing /** matches everything inside");
    Check(GlobMatch("*.worktrees", "mep.worktrees"), "*.worktrees");
    Check(GlobMatch("f?o", "foo") && !GlobMatch("f?o", "f/o"), "? matches one non-/ char");
    Check(GlobMatch("[a-c]x", "bx") && !GlobMatch("[!a-c]x", "bx"), "[] classes and negation");
    Check(GlobMatch("\\*x", "*x") && !GlobMatch("\\*x", "ax"), "backslash escapes");
}

void TestPrefilter() {
    using mep_search::PrefilterLiterals;
    using V = std::vector<std::string>;
    Check(PrefilterLiterals("mep.live_grep") == V{"live_grep"}, "'.' splits runs; longest wins");
    Check(PrefilterLiterals("TODO|FIXME") == V{"TODO", "FIXME"}, "one literal per alternative");
    Check(PrefilterLiterals("foo|.*").empty(), "an alternative with no literal disables the prefilter");
    Check(PrefilterLiterals("abc*d") == V{"ab"}, "a quantified char is not required");
    Check(PrefilterLiterals("ab+c") == V{"ab"}, "+ keeps its char but ends the run");
    Check(PrefilterLiterals("x(foo|bar)yy") == V{"yy"}, "groups are skipped whole");
    Check(PrefilterLiterals("a[)(|]bc") == V{"bc"}, "class contents never end a group or split alternatives");
    Check(PrefilterLiterals("int\\s+\\w+\\(") == V{"int"}, "shorthand escapes break runs, punctuation escapes are literal");
    Check(PrefilterLiterals("a\\.b") == V{"a.b"}, "escaped dot is a literal dot");

    // Soundness: whenever the regex matches a line, the line contains one
    // of the prefilter's literals -- otherwise grep would drop real hits.
    const std::vector<std::string> patterns = {
        "mep.live_grep", "TODO|FIXME", "a(b|c)*d", "ab?c", "x{2}y", "(foo)+bar", "[a-c]+z|q", "\\bint\\b",
        "f(o|0)o.b[ae]r", "a\\(b", "(?:ab)*c", "ab*?c", "a[]x]b", "a[^]x]b|zz"};
    const std::vector<std::string> lines = {
        "mep.live_grep()", "mep_live_grep", "// TODO x", "FIXME", "ad", "abcbd", "ac", "abc", "xxy", "foofoobar",
        "bar", "bz", "q", "int x", "fooxbar", "f0o-ber", "a(b", "ababc", "c", "abbbc", "a]b", "axb", "a-b", "zz"};
    for (const std::string &p : patterns) {
        mep_regex::Regex re(p);
        if (!re.ok()) {
            Check(false, "test pattern compiles: " + p);
            continue;
        }
        const std::vector<std::string> lits = PrefilterLiterals(p);
        for (const std::string &l : lines) {
            if (!re.PartialMatch(l) || lits.empty()) continue;
            bool found = false;
            for (const std::string &lit : lits) found = found || l.find(lit) != std::string::npos;
            Check(found, "prefilter for '" + p + "' keeps matching line '" + l + "'");
        }
    }
}

void TestParse() {
    auto o = mep_search::ParseRgArgs({"rg", "--line-number", "--no-heading", "--color=never", "--glob", "!*.worktrees",
                                      "--", "-foo", "."});
    Check(o && o->line_number && o->pattern == "-foo" && o->paths == std::vector<std::string>{"."} &&
              o->globs == std::vector<std::string>{"!*.worktrees"},
          "live grep argv");
    o = mep_search::ParseRgArgs({"rg", "--files", "--hidden", "--glob", "!.git"});
    Check(o && o->files_only && o->hidden && o->paths.empty(), "find files argv");
    o = mep_search::ParseRgArgs({"rg", "-inS", "x"});
    Check(o && o->ignore_case && o->line_number && o->smart_case && o->pattern == "x", "short flag cluster");
    Check(!mep_search::ParseRgArgs({"rg", "--json", "x"}), "unsupported flag falls back to the real binary");
    Check(!mep_search::ParseRgArgs({"rg"}), "no pattern is not parsed");
}

void TestSearch(const fs::path &root) {
    fs::create_directories(root / ".git");
    WriteFile(root / ".gitignore", "build/\n*.log\n!keep.log\n/top_only.txt\n");
    WriteFile(root / "src/main.cpp", "int main() {\n  return 0; // TODO fix\n}\n");
    WriteFile(root / "src/util.h", "// util\nint helper();\n");
    WriteFile(root / "src/nested/.gitignore", "secret.txt\n");
    WriteFile(root / "src/nested/secret.txt", "TODO hidden by nested ignore\n");
    WriteFile(root / "src/nested/top_only.txt", "TODO not anchored here\n");
    WriteFile(root / "top_only.txt", "TODO anchored ignore\n");
    WriteFile(root / "build/out.cpp", "TODO in build\n");
    WriteFile(root / "debug.log", "TODO log\n");
    WriteFile(root / "keep.log", "TODO kept log\n");
    WriteFile(root / ".hidden/x.txt", "TODO hidden dir\n");
    WriteFile(root / "a.worktrees/x.txt", "TODO worktree\n");
    WriteFile(root / "bin.dat", std::string("TODO\0binary", 11));

    auto files = RunRg({"rg", "--files", "--hidden", "--glob", "!.git", "--glob", "!*.worktrees"}, root);
    std::vector<std::string> want_files = {".gitignore",         ".hidden/x.txt", "bin.dat",
                                           "keep.log",           "src/main.cpp",  "src/nested/.gitignore",
                                           "src/nested/top_only.txt", "src/util.h"};
    std::sort(want_files.begin(), want_files.end());
    Check(files == want_files, "--files honors gitignore/negation/anchoring/globs: got " + Join(files));

    int code = -1;
    auto hits = RunRg({"rg", "--line-number", "--no-heading", "--color=never", "--glob", "!*.worktrees", "--", "TODO",
                       "."},
                      root, &code);
    std::vector<std::string> want_hits = {"./keep.log:1:TODO kept log", "./src/main.cpp:2:  return 0; // TODO fix",
                                          "./src/nested/top_only.txt:1:TODO not anchored here"};
    Check(hits == want_hits, "grep skips ignored/hidden/binary, prints ./path:line:text: got " + Join(hits));
    Check(code == 0, "exit 0 on a match");

    hits = RunRg({"rg", "-n", "int\\s+\\w+\\(", "src"}, root);
    Check(hits == std::vector<std::string>{"src/main.cpp:1:int main() {", "src/util.h:2:int helper();"},
          "regex search under a subdirectory: got " + Join(hits));

    hits = RunRg({"rg", "-n", "-i", "todo FIX", "src"}, root);
    Check(hits == std::vector<std::string>{"src/main.cpp:2:  return 0; // TODO fix"}, "-i: got " + Join(hits));
    hits = RunRg({"rg", "-n", "-S", "todo fix", "src"}, root);
    Check(hits.size() == 1, "smart case folds an all-lowercase pattern");
    hits = RunRg({"rg", "-n", "-S", "Todo fix", "src"}, root);
    Check(hits.empty(), "smart case stays sensitive with an uppercase letter");

    hits = RunRg({"rg", "-n", "helper", "src/util.h"}, root);
    Check(hits == std::vector<std::string>{"2:int helper();"}, "a lone file argument omits the file name");

    RunRg({"rg", "no-such-text-anywhere", "."}, root, &code);
    Check(code == 1, "exit 1 with no match");
    RunRg({"rg", "(unclosed", "."}, root, &code);
    Check(code == 2, "exit 2 on a bad pattern");
}

void TestJobAndCancel(const fs::path &root) {
    // A tree big enough that cancelling has something to interrupt.
    for (int d = 0; d < 40; d++) {
        for (int f = 0; f < 50; f++) {
            WriteFile(root / "big" / std::to_string(d) / (std::to_string(f) + ".txt"), std::string(2000, 'e') + "\n");
        }
    }
    auto opts = *mep_search::ParseRgArgs({"rg", "-n", "e", "."});
    std::string cwd = root.string();
    auto job = std::make_unique<Job>([opts, cwd](const Job::EmitLine &emit, const std::atomic<bool> &cancelled) {
        return mep_search::Run(opts, cwd, emit, cancelled);
    });
    job->Kill();
    const auto t0 = std::chrono::steady_clock::now();
    while (!job->Finished() &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(job->Finished() && job->Killed(), "a killed in-process job finishes promptly");

    auto full = std::make_unique<Job>([opts, cwd](const Job::EmitLine &emit, const std::atomic<bool> &cancelled) {
        return mep_search::Run(opts, cwd, emit, cancelled);
    });
    size_t lines = 0;
    while (!full->Finished()) {
        lines += full->DrainLines().size();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    lines += full->DrainLines().size();
    Check(lines >= 2000 && full->ExitCode() == 0, "an in-process job streams every line and reports its exit code");
}

/**
 * @brief Runs an rg argv as an in-process Job (the way mep.job_start does) and waits for it.
 * @param argv The rg argv.
 * @param cwd Directory to search from.
 * @param seconds Receives how long it took.
 * @return The output lines.
 */
std::vector<std::string> RunAsJob(const std::vector<std::string> &argv, const fs::path &cwd, double *seconds) {
    auto opts = *mep_search::ParseRgArgs(argv);
    std::string dir = cwd.string();
    const auto t0 = std::chrono::steady_clock::now();
    Job job([opts, dir](const Job::EmitLine &emit, const std::atomic<bool> &cancelled) {
        return mep_search::Run(opts, dir, emit, cancelled);
    });
    std::vector<std::string> out;
    while (!job.Finished()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    for (auto &l : job.DrainLines()) out.push_back(l.text);
    *seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

void TestRegexOnJobThread(const fs::path &root) {
    // Long lines through the recursive regex engine, off the main thread:
    // overflowed a default-sized thread stack before Job(Task) got its own.
    WriteFile(root / "long.txt", std::string(1990, 'a') + "z\n" + std::string(1999, 'b') + "\n");
    double secs = 0;
    auto out = RunAsJob({"rg", "-n", "(a|b)*z", "."}, root, &secs);
    Check(out.size() == 1 && out[0].rfind("./long.txt:1:", 0) == 0, "long-line regex match on the job thread");
    // A catastrophic pattern gives up (step budget) instead of spinning.
    WriteFile(root / "long.txt", std::string(60, 'a') + "\n");
    out = RunAsJob({"rg", "-n", "(a+)+z", "."}, root, &secs);
    Check(out.empty() && secs < 10, "catastrophic backtracking is bounded (took " + std::to_string(secs) + "s)");
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/mep-project-search-XXXXXX";
    if (!mkdtemp(tmpl)) {
        std::perror("mkdtemp");
        return 2;
    }
    const fs::path root = tmpl;
    TestGlob();
    TestParse();
    TestPrefilter();
    TestSearch(root / "proj");
    TestJobAndCancel(root / "big");
    TestRegexOnJobThread(root / "long");
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures == 0) std::printf("mep-project-search-test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
