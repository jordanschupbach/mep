// mep-git-test: cross-checks mep::git (the in-house git, src/git/) against
// the real git CLI. git is a *test* dependency only: it builds the scratch
// repositories and is the oracle every in-house answer is compared with.
//
// Usage: mep-git-test [--repo <path>]   (--repo also benchmarks status/log
// on that repository; it defaults to the source tree this was built from
// when MEP_SOURCE_DIR is known.)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "git/git.h"
#include "git/sha1.h"
#include "git/util.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace mep::git;

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const std::string &what, const std::string &detail = "") {
    if (ok) {
        g_pass++;
        return;
    }
    g_fail++;
    std::printf("FAIL: %s\n", what.c_str());
    if (!detail.empty()) std::printf("%s\n", detail.c_str());
}

std::string FirstDiff(const std::string &want, const std::string &got) {
    std::vector<std::string> a, b;
    auto split = [](const std::string &s, std::vector<std::string> &v) {
        size_t pos = 0;
        while (pos < s.size()) {
            size_t nl = s.find('\n', pos);
            if (nl == std::string::npos) nl = s.size();
            v.push_back(s.substr(pos, nl - pos));
            pos = nl + 1;
        }
    };
    split(want, a);
    split(got, b);
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
        std::string x = i < a.size() ? a[i] : "<eof>";
        std::string y = i < b.size() ? b[i] : "<eof>";
        if (x != y) {
            std::string ctx;
            for (size_t k = i >= 3 ? i - 3 : 0; k < i; k++) ctx += "    " + (k < a.size() ? a[k] : "") + "\n";
            return "  first difference at line " + std::to_string(i + 1) + " (of " + std::to_string(a.size()) + "/" +
                   std::to_string(b.size()) + "):\n" + ctx + "  git: [" + x + "]\n  mep: [" + y + "]";
        }
    }
    return "  (outputs differ only in a trailing newline)";
}

struct Shell {
    std::string dir;
    int Run(const std::string &cmd, std::string *out = nullptr) {
        std::string o, e;
        int code = RunProcess({"/bin/sh", "-c", cmd}, dir, {}, &o, &e);
        if (out) *out = o;
        if (code != 0 && !out) std::printf("  (shell: `%s` exited %d: %s)\n", cmd.c_str(), code, e.c_str());
        return code;
    }
    std::string Out(const std::string &cmd) {
        std::string o;
        Run(cmd, &o);
        return o;
    }
};

std::vector<std::string> Words(const std::string &s) {
    std::vector<std::string> v;
    std::string cur;
    bool q = false;
    for (char c : s) {
        if (c == '\'') {
            q = !q;
            continue;
        }
        if (c == ' ' && !q) {
            if (!cur.empty()) v.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

// Runs `git <args>` and the in-house RunCommand with the same argv and
// compares exit code + stdout.
void Same(const std::string &dir, const std::vector<std::string> &args, const std::string &label = "") {
    std::vector<std::string> argv = {"git"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string gout, gerr, mout, merr;
    int gcode = RunProcess(argv, dir, {}, &gout, &gerr);
    int mcode = RunCommand(argv, dir, mout, merr);
    std::string name = label;
    if (name.empty())
        for (const std::string &a : args) name += a + " ";
    if (mcode == -2) {
        Check(false, "[" + name + "] not supported in-house");
        return;
    }
    Check(gcode == mcode, "[" + name + "] exit code", "  git: " + std::to_string(gcode) + " (" + gerr + ")  mep: " + std::to_string(mcode) + " (" + merr + ")");
    Check(gout == mout, "[" + name + "] stdout", FirstDiff(gout, mout));
}
void Same(const std::string &dir, const std::string &args) { Same(dir, Words(args)); }

int Mep(const std::string &dir, const std::string &args, std::string *out = nullptr, std::string *err = nullptr) {
    std::vector<std::string> argv = {"git"};
    for (const std::string &w : Words(args)) argv.push_back(w);
    std::string o, e;
    int code = RunCommand(argv, dir, o, e);
    if (out) *out = o;
    if (err) *err = e;
    return code;
}

void Fsck(Shell &sh, const std::string &what) {
    std::string out;
    int code = sh.Run("git fsck --strict --no-progress 2>&1", &out);
    // dangling objects are fine (stash drops, amended commits)
    std::string bad;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? out.size() : nl + 1;
        if (line.rfind("dangling", 0) == 0 || line.empty() || line.rfind("Checking", 0) == 0) continue;
        bad += line + "\n";
    }
    Check(code == 0 && bad.empty(), "git fsck clean after " + what, bad);
}

// Every scratch repository lives under one root, removed at exit when all
// checks passed (kept, and printed, for a look when something failed).
std::string g_root;
std::string MakeTemp(const char *tag) {
    if (g_root.empty()) {
        std::string base = fs::temp_directory_path().string();
        if (const char *t = std::getenv("TMPDIR")) base = t;
        char buf[4096];
        std::snprintf(buf, sizeof buf, "%s/mep-git-test-XXXXXX", base.c_str());
#if !defined(_WIN32)
        char *d = mkdtemp(buf);
        g_root = d ? RealPath(d) : std::string(buf);
#else
        g_root = buf;
        MakeDirs(g_root);
#endif
    }
    static int counter = 0;
    std::string dir = g_root + "/" + tag + "-" + std::to_string(++counter);
    MakeDirs(dir);
    return RealPath(dir);
}

void SetEnv() {
    std::string home = MakeTemp("home");
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home + "/.config").c_str(), 1);
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("GIT_AUTHOR_NAME", "A U Thor", 1);
    setenv("GIT_AUTHOR_EMAIL", "author@example.com", 1);
    setenv("GIT_COMMITTER_NAME", "C O Mitter", 1);
    setenv("GIT_COMMITTER_EMAIL", "committer@example.com", 1);
    setenv("GIT_AUTHOR_DATE", "@1700000000 +0100", 1);
    setenv("GIT_COMMITTER_DATE", "@1700000000 +0100", 1);
    unsetenv("GIT_DIR");
    unsetenv("GIT_WORK_TREE");
    unsetenv("GIT_INDEX_FILE");
    std::string cfg = home + "/.gitconfig";
    WriteFile(cfg, "[init]\n\tdefaultBranch = main\n[advice]\n\tdetachedHead = false\n");
}

// Each commit at a distinct, increasing time (git log's date order needs it).
int g_tick = 0;
std::string Tick() {
    g_tick++;
    std::string d = "@" + std::to_string(1700000000 + g_tick * 60) + " +0100";
    setenv("GIT_AUTHOR_DATE", d.c_str(), 1);
    setenv("GIT_COMMITTER_DATE", d.c_str(), 1);
    return d;
}

void DoCommit(Shell &sh, const std::string &msg) {
    Tick();
    sh.Run("git commit -q -m '" + msg + "'");
}

// ------------------------------------------------------------ tests

void TestSha1() {
    auto hex = [](const std::array<uint8_t, 20> &d) {
        Oid o;
        o.b = d;
        return o.Hex();
    };
    Check(hex(Sha1::Hash("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty");
    Check(hex(Sha1::Hash("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 abc");
    std::string m(1000000, 'a');
    Check(hex(Sha1::Hash(m)) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "sha1 million a");
    Check(HashObject(ObjType::Blob, "").Hex() == "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391", "empty blob id");
}

void TestWildmatch() {
    Check(WildMatch("*.o", "a.o"), "wm *.o");
    Check(!WildMatch("*.o", "d/a.o"), "wm * stops at /");
    Check(WildMatch("**/a.o", "x/y/a.o"), "wm **/");
    Check(WildMatch("**/a.o", "a.o"), "wm **/ zero dirs");
    Check(WildMatch("a/**/b", "a/b"), "wm a/**/b");
    Check(WildMatch("a/**/b", "a/x/y/b"), "wm a/**/b deep");
    Check(WildMatch("[a-c]x", "bx"), "wm range");
    Check(!WildMatch("[!a-c]x", "bx"), "wm negated range");
    Check(WildMatch("[[:digit:]]*", "9z"), "wm class");
}

// Every git argv mep itself issues (src/main.cpp's kBuiltinGit/kBuiltinFileTree,
// src/editor.cpp) must be in-house -- except the network ones and rebase.
void TestSupportedArgv() {
    const std::vector<std::vector<std::string>> local = {
        {"git", "-C", "/tmp", "status", "--ignored", "--porcelain"},
        {"git", "rev-parse", "--abbrev-ref", "HEAD"},
        {"git", "--no-optional-locks", "status", "--porcelain", "--branch"},
        {"git", "log", "--date=short", "--format=%h\t%ad\t%an\t%D\t%s", "-n", "300"},
        {"git", "log", "--graph", "--all", "--color=always", "--date=short", "--format=%x1f%h%x1f%ad%x1f%an%x1f%D%x1f%s", "-n", "500"},
        {"git", "for-each-ref", "--sort=-committerdate",
         "--format=%(HEAD)\t%(refname)\t%(refname:short)\t%(upstream:short)\t%(upstream:track)\t%(committerdate:relative)\t%(subject)",
         "refs/heads", "refs/remotes"},
        {"git", "stash", "list", "--format=%gd\t%cr\t%gs"},
        {"git", "--no-optional-locks", "status", "--porcelain", "--branch", "--untracked-files=no"},
        {"git", "diff", "--cached", "--", "a b.txt"},
        {"git", "diff", "--", "x"},
        {"git", "show", "--stat", "--patch", "--format=medium", "abc1234"},
        {"git", "log", "--oneline", "--decorate", "-n", "60", "origin/main", "--"},
        {"git", "stash", "show", "-p", "--stat", "stash@{0}"},
        {"git", "rev-parse", "--git-path", "COMMIT_EDITMSG"},
        {"git", "status"},
        {"git", "commit", "-F", "/x/COMMIT_EDITMSG", "--cleanup=strip"},
        {"git", "commit", "-F", "/x/COMMIT_EDITMSG", "--cleanup=strip", "--amend"},
        {"git", "log", "-1", "--format=%B"},
        {"git", "checkout", "main"},
        {"git", "checkout", "-b", "topic"},
        {"git", "checkout", "HEAD", "--", "f.txt"},
        {"git", "stash", "pop", "stash@{1}"},
        {"git", "stash", "apply", "stash@{1}"},
        {"git", "stash", "drop", "stash@{1}"},
        {"git", "add", "-A"},
        {"git", "reset"},
        {"git", "add", "--", "f.txt"},
        {"git", "reset", "--", "f.txt"},
        {"git", "clean", "-fd", "--", "junk/"},
        {"git", "branch", "-D", "old"},
        {"git", "merge", "feature"},
        {"git", "stash", "push", "--include-untracked"},
        {"git", "stash", "push", "--include-untracked", "-m", "msg"},
        {"git", "--no-optional-locks", "status", "--porcelain=v2", "--branch", "--show-stash"},
        {"git", "for-each-ref", "--format=%(objectname) %(refname)"},
        {"git", "status", "--porcelain"},
        {"git", "for-each-ref", "--format=%(refname:short) %(upstream:track)", "refs/heads"},
        {"git", "worktree", "add", "/w/x", "x"},
        {"git", "worktree", "add", "-b", "x", "/w/x", "HEAD"},
        {"git", "worktree", "remove", "/w/x"},
        {"git", "worktree", "remove", "--force", "/w/x"},
        {"git", "worktree", "list", "--porcelain"},
        {"git", "worktree", "prune"},
        {"git", "rev-parse", "--show-toplevel"},
        {"git", "show", "HEAD:./f.txt"},
        {"git", "show", ":./f.txt"},
    };
    for (const auto &argv : local) {
        std::string s;
        for (const std::string &a : argv) s += a + " ";
        Check(IsSupported(argv), "in-house: " + s);
    }
    const std::vector<std::vector<std::string>> cli = {
        {"git", "push"}, {"git", "pull"}, {"git", "fetch", "--all", "--prune"}, {"git", "push", "-u", "origin", "b"},
        {"git", "push", "origin", "--delete", "b"}, {"git", "rebase", "main"}, {"git", "clone", "x"},
    };
    for (const auto &argv : cli) Check(!IsSupported(argv), "CLI: " + argv[1]);
}

void TestBasicRepo() {
    std::string dir = MakeTemp("basic");
    Shell sh{dir};
    sh.Run("git init -q . && git config core.autocrlf false");
    Same(dir, "status --porcelain --branch");
    Same(dir, "status --porcelain=v2 --branch");
    Same(dir, "status");
    WriteFile(dir + "/a.txt", "one\ntwo\nthree\n");
    WriteFile(dir + "/b.txt", "bee\n");
    MakeDirs(dir + "/sub/deep");
    WriteFile(dir + "/sub/c.txt", "sea\n");
    WriteFile(dir + "/sub/deep/d.txt", "dee\n");
    WriteFile(dir + "/.gitignore", "*.o\nbuild/\n/top-only\n!keep.o\n");
    Same(dir, "status --porcelain --branch");
    Same(dir, "status --porcelain --ignored");
    Same(dir, "status --porcelain -uall");
    sh.Run("git add -A");
    Same(dir, "status --porcelain --branch");
    Same(dir, "status --porcelain=v2 --branch --show-stash");
    DoCommit(sh, "initial");
    // A mix of everything.
    WriteFile(dir + "/a.txt", "one\nTWO\nthree\nfour\n");
    sh.Run("git add a.txt");
    WriteFile(dir + "/a.txt", "one\nTWO\nthree\nfour\nfive\n");
    RemoveFile(dir + "/b.txt");
    WriteFile(dir + "/new.txt", "new\n");
    MakeDirs(dir + "/untracked_dir/inner");
    WriteFile(dir + "/untracked_dir/inner/x", "x\n");
    MakeDirs(dir + "/only_ignored");
    WriteFile(dir + "/only_ignored/z.o", "o\n");
    MakeDirs(dir + "/mixed");
    WriteFile(dir + "/mixed/m.o", "o\n");
    WriteFile(dir + "/mixed/m.c", "c\n");
    MakeDirs(dir + "/build/x");
    WriteFile(dir + "/build/x/out", "bin\n");
    WriteFile(dir + "/keep.o", "kept\n");
    WriteFile(dir + "/top-only", "t\n");
    WriteFile(dir + "/sub/top-only", "t\n");
    WriteFile(dir + "/sub/sub.o", "o\n");
    MakeDirs(dir + "/emptydir");
    WriteFile(dir + "/sp ace.txt", "s\n");
    WriteFile(dir + "/\xc3\xa9t\xc3\xa9.txt", "utf8\n");
    sh.Run("git mv sub/c.txt sub/c-renamed.txt");
    sh.Run("chmod +x sub/deep/d.txt");
    Same(dir, "status --porcelain --branch");
    Same(dir, "status --porcelain --ignored");
    Same(dir, "status --porcelain -uall --ignored");
    Same(dir, "status --porcelain -uno");
    Same(dir, "status --porcelain=v2 --branch");
    Same(dir, "status --porcelain --no-renames");
    Same(dir, "status --porcelain --branch --untracked-files=no");
    Same(dir, "status");
    Same(dir, "diff");
    Same(dir, "diff --cached");
    Same(dir, "diff -- a.txt");
    Same(dir, "diff --cached -- a.txt");
    Same(dir, "diff --stat");
    Same(dir, "diff --cached --stat");
    Same(dir, "diff --name-status --cached");
    Same(dir, "rev-parse --abbrev-ref HEAD");
    Same(dir, "rev-parse --show-toplevel");
    Same(dir, "rev-parse HEAD");
    Same(dir, "show HEAD:a.txt");
    Same(dir, "show :a.txt");
    Same(dir + "/sub", "show HEAD:./c.txt");
    Same(dir + "/sub", "show HEAD:sub/c.txt");
    // Errors the git gutter keys on.
    {
        std::string o, e;
        int code = Mep(dir, "show HEAD:new.txt", &o, &e);
        Check(code == 128 && e.find("exists on disk, but not in 'HEAD'") != std::string::npos, "show HEAD:untracked error", e);
        code = Mep(dir, "show HEAD:nope.txt", &o, &e);
        Check(code == 128 && e.find("does not exist in 'HEAD'") != std::string::npos, "show HEAD:missing error", e);
    }
    // A subdirectory cwd: porcelain paths stay repo-relative.
    Same(dir + "/sub", "status --porcelain --branch");
    Same(dir, "-C sub status --porcelain");
}

// A history with branches, merges, an octopus and criss-crosses for log/graph.
std::string BuildHistory() {
    std::string dir = MakeTemp("history");
    Shell sh{dir};
    sh.Run("git init -q . && git config core.autocrlf false");
    auto edit = [&](const std::string &f, const std::string &line) {
        std::string cur;
        ReadFile(dir + "/" + f, cur);
        WriteFile(dir + "/" + f, cur + line + "\n");
        sh.Run("git add -A");
    };
    edit("f", "base");
    DoCommit(sh, "base commit");
    for (int i = 0; i < 3; i++) {
        edit("f", "main " + std::to_string(i));
        DoCommit(sh, "main work " + std::to_string(i));
    }
    sh.Run("git checkout -q -b feature HEAD~2");
    edit("g", "feature 1");
    DoCommit(sh, "feature one");
    edit("g", "feature 2");
    DoCommit(sh, "feature two\n\nwith a body\nof two lines");
    sh.Run("git checkout -q -b topic main~1");
    edit("h", "topic");
    DoCommit(sh, "topic work");
    sh.Run("git checkout -q main");
    edit("f", "main again");
    DoCommit(sh, "main again");
    Tick();
    sh.Run("git merge -q --no-ff -m 'Merge branch feature' feature");
    sh.Run("git checkout -q -b side1 main~2");
    edit("s1", "1");
    DoCommit(sh, "side one");
    sh.Run("git checkout -q -b side2 main~2");
    edit("s2", "2");
    DoCommit(sh, "side two");
    sh.Run("git checkout -q -b side3 main~2");
    edit("s3", "3");
    DoCommit(sh, "side three");
    sh.Run("git checkout -q main");
    Tick();
    sh.Run("git merge -q -m 'Octopus merge' side1 side2 side3");
    Tick();
    sh.Run("git merge -q --no-ff -m 'Merge topic' topic");
    edit("f", "after merges");
    DoCommit(sh, "after merges");
    sh.Run("git tag v1.0 && git tag -a -m 'annotated' v1.1 HEAD~1");
    sh.Run("git checkout -q -b crisscross feature");
    edit("g", "cc");
    DoCommit(sh, "criss cross");
    sh.Run("git checkout -q main");
    // A fake remote with an upstream.
    sh.Run("git init -q --bare ../remote-" + BaseName(dir) + ".git && git remote add origin ../remote-" + BaseName(dir) +
           ".git && git push -q origin main feature 2>/dev/null && git branch -q -u origin/main main");
    edit("f", "local only");
    DoCommit(sh, "local only");
    sh.Run("git checkout -q --detach v1.0~1 && git checkout -q main");
    return dir;
}

void TestLogAndRefs(const std::string &dir, const std::string &label) {
    Same(dir, Words("log --date=short --format=%h\t%ad\t%an\t%D\t%s -n 300"), label + " log tab format");
    Same(dir, Words("log --graph --all --color=always --date=short --format=%x1f%h%x1f%ad%x1f%an%x1f%D%x1f%s -n 500"),
         label + " log graph color");
    Same(dir, Words("log --graph --all --date=short --format=%x1f%h%x1f%ad%x1f%an%x1f%D%x1f%s -n 6"), label + " graph -n 6");
    Same(dir, Words("log --graph --oneline --all"), label + " graph oneline");
    Same(dir, Words("log --oneline --decorate -n 60 feature --"), label + " oneline decorate");
    Same(dir, Words("log --oneline --decorate -n 60 origin/main --"), label + " oneline decorate remote");
    Same(dir, Words("log -1 --format=%B"), label + " log -1 %B");
    Same(dir, Words("log -1 --format=%B feature"), label + " log -1 %B body");
    Same(dir, Words("log"), label + " log medium");
    Same(dir, Words("log --topo-order --format=%h %p %s"), label + " topo order");
    Same(dir, Words("log --first-parent --format=%h %s"), label + " first parent");
    Same(dir, Words("for-each-ref --sort=-committerdate --format=%(HEAD)\t%(refname)\t%(refname:short)\t%(upstream:short)\t%(upstream:track)\t%(committerdate:relative)\t%(subject) refs/heads refs/remotes"),
         label + " for-each-ref branches");
    Same(dir, Words("for-each-ref --format=%(objectname) %(refname)"), label + " for-each-ref all");
    Same(dir, Words("for-each-ref --format=%(refname:short) %(upstream:track) refs/heads"), label + " for-each-ref track");
    Same(dir, Words("for-each-ref"), label + " for-each-ref default");
    Same(dir, Words("status --porcelain --branch"), label + " status branch ahead");
    Same(dir, Words("status --porcelain=v2 --branch --show-stash"), label + " status v2 ahead");
    Same(dir, Words("status"), label + " status long, ahead");
    Same(dir, Words("rev-parse HEAD~2 main~2^2 main~3^3 v1.1 v1.1^{commit} HEAD^{tree} feature~1 HEAD@{1} main^0"), label + " rev-parse revs");
    // Every commit's show output.
    std::string list;
    RunProcess({"git", "rev-list", "--all"}, dir, {}, &list, nullptr);
    size_t pos = 0;
    int n = 0;
    while (pos < list.size()) {
        size_t nl = list.find('\n', pos);
        std::string h = list.substr(pos, nl - pos);
        pos = nl + 1;
        Same(dir, {"show", "--stat", "--patch", "--format=medium", h}, label + " show " + h.substr(0, 7));
        Same(dir, {"show", h}, label + " show plain " + h.substr(0, 7));
        n++;
    }
    Check(n > 5, label + " showed commits");
}

void TestDiffAlgorithm() {
    std::string dir = MakeTemp("diff");
    Shell sh{dir};
    sh.Run("git init -q .");
    std::string a, b;
    // Functions moved/inserted: exercises the slider/indent heuristic.
    a = "#include <x>\n\nint f() {\n    return 1;\n}\n\nint g() {\n    return 2;\n}\n";
    b = "#include <x>\n\nint f() {\n    return 1;\n}\n\nint h() {\n    return 3;\n}\n\nint g() {\n    return 2;\n}\n";
    WriteFile(dir + "/c.c", a);
    std::string text;
    for (int i = 0; i < 200; i++) text += "line " + std::to_string(i) + "\n";
    WriteFile(dir + "/big.txt", text);
    WriteFile(dir + "/nonl.txt", "a\nb");
    WriteFile(dir + "/bin.dat", std::string("\x00\x01\x02 binary", 10));
    WriteFile(dir + "/del.txt", "gone\n");
    WriteFile(dir + "/moved.txt", "a moved file with enough content to be found as a rename\nline two\nline three\n");
    sh.Run("git add -A");
    DoCommit(sh, "c1");
    WriteFile(dir + "/c.c", b);
    std::string t2 = text;
    t2.replace(t2.find("line 10\n"), 8, "LINE TEN\n");
    t2.replace(t2.find("line 100\n"), 9, "");
    t2 += "tail\n";
    t2.insert(t2.find("line 150\n"), "inserted a\ninserted b\n");
    WriteFile(dir + "/big.txt", t2);
    WriteFile(dir + "/nonl.txt", "a\nb\nc");
    WriteFile(dir + "/bin.dat", std::string("\x00\x01\x03 binary!", 11));
    RemoveFile(dir + "/del.txt");
    sh.Run("git mv moved.txt renamed.txt && echo 'line four' >> renamed.txt");
    Same(dir, "diff");
    Same(dir, "diff --stat");
    sh.Run("git add -A");
    Same(dir, "diff --cached");
    Same(dir, "diff --cached --stat");
    Same(dir, "status --porcelain");
    DoCommit(sh, "c2");
    Same(dir, "show --stat --patch --format=medium HEAD");
    Same(dir, "show HEAD~1");
    // Long names (stat name truncation) and a wide change count.
    MakeDirs(dir + "/a/very/long/directory/name/that/goes/on/and/on");
    std::string big;
    for (int i = 0; i < 300; i++) big += std::to_string(i) + "\n";
    WriteFile(dir + "/a/very/long/directory/name/that/goes/on/and/on/file-with-a-long-name.txt", big);
    sh.Run("git add -A");
    DoCommit(sh, "c3");
    Same(dir, "show --stat --patch --format=medium HEAD");
}

void TestStatusEdgeCases() {
    std::string dir = MakeTemp("edge");
    Shell sh{dir};
    sh.Run("git init -q .");
    WriteFile(dir + "/a.txt", "a\n");
    WriteFile(dir + "/exec.sh", "#!/bin/sh\n");
    WriteFile(dir + "/x", "file that becomes a dir\n");
    WriteFile(dir + "/skip.txt", "s\n");
    WriteFile(dir + "/assume.txt", "u\n");
    WriteFile(dir + "/conflict.txt", "base\n");
    sh.Run("ln -s a.txt link && git add -A");
    DoCommit(sh, "base");
    // Unborn-branch status in a second repo.
    {
        std::string d2 = MakeTemp("unborn");
        Shell s2{d2};
        s2.Run("git init -q .");
        WriteFile(d2 + "/f", "f\n");
        s2.Run("git add f");
        Same(d2, "status --porcelain --branch");
        Same(d2, "status --porcelain=v2 --branch");
        Same(d2, "status");
        std::string o, e;
        Check(Mep(d2, "log", &o, &e) == 128 && e.find("does not have any commits yet") != std::string::npos, "log on unborn", e);
    }
    // Conflicts.
    sh.Run("git checkout -q -b other && echo other > conflict.txt && echo theirs > both.txt && git add -A && git commit -qm other");
    sh.Run("git checkout -q main && echo mine > conflict.txt && echo ours > both.txt && git add -A && git commit -qm mine");
    sh.Run("git merge -q other >/dev/null 2>&1; true");
    Same(dir, "status --porcelain --branch");
    Same(dir, "status --porcelain=v2");
    Same(dir, "status");
    {
        std::string o, e;
        Check(Mep(dir, "commit -m x", &o, &e) == 128 && e.find("unmerged") != std::string::npos, "commit refuses with conflicts", e);
        Check(Mep(dir, "checkout HEAD~1", &o, &e) == 1, "checkout refuses with conflicts", e);
    }
    Check(Mep(dir, "add -- conflict.txt both.txt") == 0, "add resolves conflicts");
    Same(dir, "status --porcelain");
    sh.Run("git commit -qm merged");
    // Type and mode changes, symlink retarget, intent-to-add, skip/assume.
    sh.Run("chmod +x exec.sh && rm link && ln -s exec.sh link");
    RemoveFile(dir + "/x");
    MakeDirs(dir + "/x");
    WriteFile(dir + "/x/inner", "now a dir\n");
    WriteFile(dir + "/ita.txt", "intent\n");
    sh.Run("git add -N ita.txt");
    sh.Run("git update-index --skip-worktree skip.txt && echo changed > skip.txt");
    sh.Run("git update-index --assume-unchanged assume.txt && echo changed > assume.txt");
    Same(dir, "status --porcelain");
    Same(dir, "status --porcelain=v2");
    Same(dir, "diff");
    Same(dir, "diff --stat");
    // Racy: same size, rewritten right after staging.
    WriteFile(dir + "/a.txt", "1\n");
    sh.Run("git add a.txt");
    WriteFile(dir + "/a.txt", "2\n");
    Same(dir, "status --porcelain");
    // In-house add -A must leave the same index as git's own add -A.
    std::string twin = MakeTemp("twin");
    sh.Run("cp -R . " + twin);
    Shell st{twin};
    Mep(dir, "add -A");
    st.Run("git add -A");
    Check(sh.Out("git ls-files -s --debug | grep -v ctime | grep -v mtime | grep -v dev: | grep -v uid: | grep -v size:") ==
              st.Out("git ls-files -s --debug | grep -v ctime | grep -v mtime | grep -v dev: | grep -v uid: | grep -v size:"),
          "add -A index matches git's", sh.Out("git ls-files -s") + "---\n" + st.Out("git ls-files -s"));
    Same(dir, "status --porcelain");
    Fsck(sh, "edge cases");
    // Submodule.
    std::string subsrc = MakeTemp("subsrc");
    Shell ss{subsrc};
    ss.Run("git init -q . && echo s > s.txt && git add -A && git commit -qm sub");
    sh.Run("git -c protocol.file.allow=always submodule -q add " + subsrc + " sub >/dev/null 2>&1 && git commit -qam 'add sub'");
    Same(dir, "status --porcelain");
    WriteFile(dir + "/sub/s.txt", "modified in sub\n");
    Same(dir, "status --porcelain");
    Same(dir, "status --porcelain=v2");
    Shell sub{dir + "/sub"};
    sub.Run("git commit -qam 'sub moved'");
    Same(dir, "status --porcelain");
    Same(dir, "diff");
    WriteFile(dir + "/sub/untracked-in-sub", "u\n");
    Same(dir, "status --porcelain");
    // Bare repository.
    std::string bare = MakeTemp("bare");
    Shell sb{bare};
    sb.Run("git clone -q --bare " + dir + " b.git");
    Same(bare + "/b.git", "log --oneline -3");
    Same(bare + "/b.git", "for-each-ref");
    {
        std::string o, e;
        Check(Mep(bare + "/b.git", "status --porcelain", &o, &e) == 128, "status in bare repo fails", e);
    }
}

// Merges whose result differs from every parent: git show's dense
// combined diff (`diff --cc`).
void TestCombinedDiff() {
    std::string dir = MakeTemp("combined");
    Shell sh{dir};
    sh.Run("git init -q .");
    std::string base;
    for (int i = 1; i <= 40; i++) base += (i % 10 == 0 ? "func_" + std::to_string(i) + "() {" : "line " + std::to_string(i)) + "\n";
    WriteFile(dir + "/f.txt", base);
    WriteFile(dir + "/g.txt", "a\nb\nc\n");
    WriteFile(dir + "/h.txt", "same\n");
    sh.Run("git add -A");
    DoCommit(sh, "base");
    sh.Run("git checkout -q -b side");
    sh.Run("sed -i.bak 's/^line 5$/five side/; s/^line 25$/twenty-five side/; s/^line 33$/thirty-three side/' f.txt && rm f.txt.bak");
    WriteFile(dir + "/g.txt", "a\nB side\nc\n");
    sh.Run("git rm -q h.txt");
    sh.Run("git add -A");
    DoCommit(sh, "side");
    sh.Run("git checkout -q main");
    sh.Run("sed -i.bak 's/^line 5$/five main/; s/^line 28$/twenty-eight main/; /^line 37$/d' f.txt && rm f.txt.bak");
    WriteFile(dir + "/g.txt", "a\nB main\nc\nd\n");
    sh.Run("git add -A");
    DoCommit(sh, "main");
    Tick();
    sh.Run("git merge -q side >/dev/null 2>&1; true");
    // Resolve by hand, adding something neither side had.
    std::string merged = base;
    merged.replace(merged.find("line 5\n"), 7, "five resolved\nextra line\n");
    merged.replace(merged.find("line 25\n"), 8, "twenty-five side\n");
    merged.replace(merged.find("line 28\n"), 8, "twenty-eight main\n");
    merged.replace(merged.find("line 33\n"), 8, "thirty-three side\n");
    merged.replace(merged.find("line 37\n"), 8, "");
    WriteFile(dir + "/f.txt", merged);
    WriteFile(dir + "/g.txt", "a\nB both\nc\nd\n");
    WriteFile(dir + "/h.txt", "resurrected\n");
    sh.Run("git add -A");
    sh.Run("git commit -q -m 'evil merge'");
    Same(dir, "show --stat --patch --format=medium HEAD");
    Same(dir, "show HEAD");
    Same(dir, "show --stat HEAD");
    // An octopus with a file changed in every branch.
    sh.Run("git checkout -q -b o1 HEAD~1 && echo o1 > o.txt && git add o.txt && git commit -qm o1");
    sh.Run("git checkout -q -b o2 main~1 && echo o2 > o.txt && git add o.txt && git commit -qm o2");
    sh.Run("git checkout -q main");
    Tick();
    sh.Run("git merge -q -s ours -m 'octopus-ish' o1 o2 >/dev/null 2>&1; true");
    sh.Run("echo octo > o.txt && git add o.txt && git commit -q --amend -m 'octopus with o.txt'");
    Same(dir, "show --stat --patch --format=medium HEAD");
    Same(dir, "log -p --oneline -3");
}

void TestWriteOps() {
    std::string dir = MakeTemp("write");
    Shell sh{dir};
    sh.Run("git init -q .");
    WriteFile(dir + "/a.txt", "a\n");
    WriteFile(dir + "/b.txt", "b\n");
    MakeDirs(dir + "/d");
    WriteFile(dir + "/d/c.txt", "c\n");
    WriteFile(dir + "/.gitignore", "*.log\n");
    std::string out, err;
    // add -A in-house, then the real git must agree on the index.
    Check(Mep(dir, "add -A", &out, &err) == 0, "add -A", err);
    Check(sh.Out("git status --porcelain") == "A  .gitignore\nA  a.txt\nA  b.txt\nA  d/c.txt\n", "index after add -A",
          sh.Out("git status --porcelain"));
    Check(sh.Out("git ls-files -s") == sh.Out("git ls-files -s"), "ls-files");
    // commit in-house with a pre-commit and commit-msg hook.
    MakeDirs(dir + "/.git/hooks");
    WriteFile(dir + "/.git/hooks/pre-commit", "#!/bin/sh\necho pre-commit-ran > \"$GIT_DIR_X\"hook.out\nexit 0\n", true);
    WriteFile(dir + "/.git/hooks/commit-msg", "#!/bin/sh\necho 'Signed-off-by: hook' >> \"$1\"\n", true);
    WriteFile(dir + "/.git/COMMIT_EDITMSG", "First commit\n\n# a comment\nbody line   \n\n\n");
    Tick();
    Check(Mep(dir, "commit -F .git/COMMIT_EDITMSG --cleanup=strip", &out, &err) == 0, "commit", err);
    Check(FileExists(dir + "/hook.out"), "pre-commit hook ran");
    RemoveFile(dir + "/hook.out");
    Check(sh.Out("git log -1 --format=%B") == "First commit\n\nbody line\nSigned-off-by: hook\n\n", "commit message",
          sh.Out("git log -1 --format=%B"));
    Check(sh.Out("git status --porcelain") == "", "clean after commit", sh.Out("git status --porcelain"));
    Check(sh.Out("git reflog -1 --format=%gs") == "commit (initial): First commit\n", "reflog initial",
          sh.Out("git reflog -1 --format=%gs"));
    Fsck(sh, "commit");
    // A failing pre-commit hook aborts.
    WriteFile(dir + "/.git/hooks/pre-commit", "#!/bin/sh\nexit 1\n", true);
    WriteFile(dir + "/a.txt", "a2\n");
    Mep(dir, "add a.txt");
    Check(Mep(dir, "commit -m nope", &out, &err) == 1, "failing pre-commit aborts");
    RemoveFile(dir + "/.git/hooks/pre-commit");
    RemoveFile(dir + "/.git/hooks/commit-msg");
    // Unstage / restage / reset -- path.
    Check(Mep(dir, "reset -- a.txt") == 0, "reset -- path");
    Check(sh.Out("git status --porcelain") == " M a.txt\n", "after reset -- a.txt", sh.Out("git status --porcelain"));
    Mep(dir, "add -- a.txt");
    Check(sh.Out("git status --porcelain") == "M  a.txt\n", "after add -- a.txt", sh.Out("git status --porcelain"));
    Mep(dir, "reset");
    Check(sh.Out("git status --porcelain") == " M a.txt\n", "after reset", sh.Out("git status --porcelain"));
    // Deleted file staged via add -- path.
    RemoveFile(dir + "/b.txt");
    Mep(dir, "add -- b.txt");
    Check(sh.Out("git status --porcelain") == " M a.txt\nD  b.txt\n", "add -- deleted", sh.Out("git status --porcelain"));
    Mep(dir, "reset -- b.txt");
    // checkout HEAD -- path discards.
    Check(Mep(dir, "checkout HEAD -- b.txt", &out, &err) == 0, "checkout HEAD -- b.txt", err);
    Check(Mep(dir, "checkout HEAD -- a.txt", &out, &err) == 0, "checkout HEAD -- a.txt", err);
    Check(sh.Out("git status --porcelain") == "", "clean after checkout -- paths", sh.Out("git status --porcelain"));
    Check(Mep(dir, "checkout HEAD -- nope.txt", &out, &err) == 1, "checkout HEAD -- missing fails");
    // clean -fd on an untracked dir and file, ignored left alone.
    MakeDirs(dir + "/junk/deeper");
    WriteFile(dir + "/junk/deeper/x", "x\n");
    WriteFile(dir + "/stray.txt", "s\n");
    WriteFile(dir + "/keep.log", "l\n");
    Check(Mep(dir, "clean -fd -- junk/", &out, &err) == 0 && !FileExists(dir + "/junk"), "clean -fd dir", err);
    Check(Mep(dir, "clean -fd -- stray.txt") == 0 && !FileExists(dir + "/stray.txt"), "clean -fd file");
    Check(FileExists(dir + "/keep.log"), "clean leaves ignored files");
    // Ignored files inside an untracked directory survive `clean -fd`, as with git.
    MakeDirs(dir + "/mixed");
    WriteFile(dir + "/mixed/gone.txt", "u\n");
    WriteFile(dir + "/mixed/secret.log", "keep me\n");
    Check(Mep(dir, "clean -fd -- mixed/") == 0 && !FileExists(dir + "/mixed/gone.txt") &&
              FileExists(dir + "/mixed/secret.log"),
          "clean -fd keeps ignored files in untracked dirs");
    RemoveAll(dir + "/mixed");
    // Branches + checkout.
    Check(Mep(dir, "checkout -b feature", &out, &err) == 0, "checkout -b", err);
    Check(sh.Out("git rev-parse --abbrev-ref HEAD") == "feature\n", "on feature");
    WriteFile(dir + "/f.txt", "feature file\n");
    WriteFile(dir + "/a.txt", "a feature\n");
    Mep(dir, "add -A");
    Tick();
    Check(Mep(dir, "commit -m 'feature work'", &out, &err) == 0, "commit on feature", err);
    Check(Mep(dir, "checkout main", &out, &err) == 0, "checkout main", err);
    Check(!FileExists(dir + "/f.txt"), "checkout removed feature-only file");
    Check(sh.Out("git status --porcelain") == "", "clean after checkout main", sh.Out("git status --porcelain"));
    // Refuse to clobber local changes.
    WriteFile(dir + "/a.txt", "local edit\n");
    Check(Mep(dir, "checkout feature", &out, &err) == 1 && err.find("would be overwritten") != std::string::npos,
          "checkout refuses dirty", err);
    // Non-conflicting local change carries over.
    Mep(dir, "checkout HEAD -- a.txt");
    WriteFile(dir + "/b.txt", "b local\n");
    Check(Mep(dir, "checkout feature", &out, &err) == 0, "checkout carries local change", err);
    Check(sh.Out("git status --porcelain") == " M b.txt\n", "local change kept", sh.Out("git status --porcelain"));
    // Untracked file in the way.
    Mep(dir, "checkout HEAD -- b.txt");
    Mep(dir, "checkout main");
    WriteFile(dir + "/f.txt", "untracked in the way\n");
    Check(Mep(dir, "checkout feature", &out, &err) == 1 && err.find("untracked working tree files") != std::string::npos,
          "checkout refuses untracked clobber", err);
    RemoveFile(dir + "/f.txt");
    // Fast-forward merge.
    Check(Mep(dir, "merge feature", &out, &err) == 0 && out.find("Fast-forward") != std::string::npos, "merge ff", err + out);
    Check(sh.Out("git rev-parse main") == sh.Out("git rev-parse feature"), "main fast-forwarded");
    Check(sh.Out("git status --porcelain") == "", "clean after ff", sh.Out("git status --porcelain"));
    Check(Mep(dir, "merge feature", &out, &err) == 0 && out == "Already up to date.\n", "merge up to date", out);
    // Branch delete.
    Check(Mep(dir, "branch -D main", &out, &err) == 1, "cannot delete checked-out branch");
    Mep(dir, "checkout -b tmp");
    Check(Mep(dir, "checkout main") == 0, "back to main");
    Check(Mep(dir, "branch -D tmp", &out, &err) == 0 && sh.Out("git branch --list tmp").empty(), "branch -D", err);
    Fsck(sh, "branch/checkout/merge");
    // Detached checkout.
    Check(Mep(dir, "checkout HEAD~1", &out, &err) == 0, "detached checkout", err);
    Check(sh.Out("git rev-parse --abbrev-ref HEAD") == "HEAD\n", "detached");
    Same(dir, "status --porcelain --branch");
    Mep(dir, "checkout main");
    // Amend.
    WriteFile(dir + "/a.txt", "amended\n");
    Mep(dir, "add a.txt");
    Tick();
    Check(Mep(dir, "commit --amend -m 'feature work (amended)'", &out, &err) == 0, "amend", err);
    Check(sh.Out("git log -1 --format=%s%n%an") == "feature work (amended)\nA U Thor\n", "amended commit",
          sh.Out("git log -1 --format=%s%n%an"));
    Check(sh.Out("git rev-list --count HEAD") == "2\n", "amend replaced the commit");
    Fsck(sh, "amend");
    // Directory/file switches.
    sh.Run("git checkout -q -b dfile && echo f > df && git add df && git commit -qm 'df is a file'");
    sh.Run("git checkout -q -b ddir main && mkdir df && echo i > df/inner && git add df && git commit -qm 'df is a dir'");
    Check(Mep(dir, "checkout dfile", &out, &err) == 0 && !IsDirectory(dir + "/df") && FileExists(dir + "/df"),
          "checkout dir -> file", err);
    Check(Mep(dir, "checkout ddir", &out, &err) == 0 && FileExists(dir + "/df/inner"), "checkout file -> dir", err);
    Check(sh.Out("git status --porcelain") == "", "clean after D/F switches", sh.Out("git status --porcelain"));
    // An untracked directory where the target has a file: refuse, keep it.
    sh.Run("git checkout -q main");
    MakeDirs(dir + "/df");
    WriteFile(dir + "/df/precious.txt", "untracked work\n");
    Check(Mep(dir, "checkout dfile", &out, &err) == 1 && FileExists(dir + "/df/precious.txt"),
          "checkout refuses to replace an untracked directory", err);
    // An untracked file where the target needs a directory: refuse.
    RemoveAll(dir + "/df");
    WriteFile(dir + "/df", "untracked file\n");
    Check(Mep(dir, "checkout ddir", &out, &err) == 1 && FileExists(dir + "/df"),
          "checkout refuses to replace an untracked file with a directory", err);
    RemoveFile(dir + "/df");
    Fsck(sh, "D/F checkouts");
}

void TestStash() {
    std::string dir = MakeTemp("stash");
    Shell sh{dir};
    sh.Run("git init -q .");
    WriteFile(dir + "/a.txt", "a\n");
    WriteFile(dir + "/b.txt", "b\n");
    sh.Run("git add -A");
    DoCommit(sh, "base");
    std::string out, err;
    Check(Mep(dir, "stash push --include-untracked", &out, &err) == 0 && out == "No local changes to save\n",
          "stash nothing", out + err);
    WriteFile(dir + "/a.txt", "a staged\n");
    sh.Run("git add a.txt");
    WriteFile(dir + "/a.txt", "a staged then more\n");
    WriteFile(dir + "/new.txt", "staged new\n");
    sh.Run("git add new.txt");
    RemoveFile(dir + "/b.txt");
    MakeDirs(dir + "/u");
    WriteFile(dir + "/u/untracked.txt", "u\n");
    std::string before = sh.Out("git status --porcelain");
    Tick();
    Check(Mep(dir, "stash push --include-untracked -m 'my stash'", &out, &err) == 0, "stash push -u", err);
    Check(out == "Saved working directory and index state On main: my stash\n", "stash push output", out);
    Check(sh.Out("git status --porcelain") == "", "clean after stash push", sh.Out("git status --porcelain"));
    Same(dir, Words("stash list --format=%gd\t%cr\t%gs"), "stash list");
    Same(dir, "stash show -p --stat stash@{0}");
    Fsck(sh, "stash push");
    Check(sh.Out("git stash show --include-untracked --name-only stash@{0}").find("u/untracked.txt") != std::string::npos,
          "untracked files are in the stash");
    // Second stash, then apply the older one by index.
    WriteFile(dir + "/b.txt", "b changed\n");
    Tick();
    Check(Mep(dir, "stash push --include-untracked", &out, &err) == 0, "second stash", err);
    Same(dir, Words("stash list --format=%gd\t%cr\t%gs"), "stash list 2");
    Check(Mep(dir, "stash apply stash@{1}", &out, &err) == 0, "stash apply stash@{1}", err);
    std::string after = sh.Out("git status --porcelain");
    // git stash apply (no --index) restores the worktree; new files come back staged.
    Check(after == " M a.txt\n D b.txt\nA  new.txt\n?? u/\n", "state after apply", after + "\n(before: " + before + ")");
    // Clean and pop the newer one.
    sh.Run("git reset -q --hard && git clean -qfd");
    Check(Mep(dir, "stash pop stash@{0}", &out, &err) == 0, "stash pop", err);
    Check(sh.Out("git status --porcelain") == " M b.txt\n", "after pop", sh.Out("git status --porcelain"));
    Same(dir, Words("stash list --format=%gd\t%cr\t%gs"), "stash list after pop");
    sh.Run("git checkout -q -- .");
    Check(Mep(dir, "stash drop stash@{0}", &out, &err) == 0, "stash drop", err);
    Check(sh.Out("git stash list") == "", "stash list empty", sh.Out("git stash list"));
    Check(!FileExists(dir + "/.git/refs/stash"), "refs/stash gone");
    Fsck(sh, "stash apply/pop/drop");
    // Conflicting apply goes through the CLI and reports like git.
    WriteFile(dir + "/a.txt", "stashed version\n");
    Tick();
    Mep(dir, "stash push");
    WriteFile(dir + "/a.txt", "committed version\n");
    sh.Run("git commit -qam conflicting");
    int code = Mep(dir, "stash apply", &out, &err);
    Check(code != 0, "conflicting stash apply fails (CLI fallback)", out + err);
}

void TestWorktrees() {
    std::string dir = MakeTemp("wt");
    Shell sh{dir};
    sh.Run("git init -q .");
    WriteFile(dir + "/a.txt", "a\n");
    MakeDirs(dir + "/sub");
    WriteFile(dir + "/sub/s.txt", "s\n");
    sh.Run("git add -A");
    DoCommit(sh, "base");
    sh.Run("git branch existing");
    std::string out, err;
    std::string wt1 = dir + "-wt1", wt2 = dir + "-wt2";
    Check(Mep(dir, "worktree add -b ws1 " + wt1 + " HEAD", &out, &err) == 0, "worktree add -b", err);
    Check(Mep(dir, "worktree add " + wt2 + " existing", &out, &err) == 0, "worktree add existing", err);
    Check(Mep(dir, "worktree add " + dir + "-wt3 existing", &out, &err) == 128 && err.find("already used by worktree") != std::string::npos,
          "worktree add refuses checked-out branch", err);
    Same(dir, "worktree list --porcelain");
    Shell sh1{wt1};
    Check(sh1.Out("git status --porcelain") == "", "new worktree clean", sh1.Out("git status --porcelain"));
    Check(sh1.Out("git rev-parse --abbrev-ref HEAD") == "ws1\n", "worktree branch");
    Same(wt1, "status --porcelain --branch");
    Same(wt1, Words("log --format=%h %D %s"), "log in worktree");
    Same(wt1, "rev-parse --show-toplevel");
    Fsck(sh, "worktree add");
    // Commit inside the worktree in-house.
    WriteFile(wt1 + "/w.txt", "w\n");
    Mep(wt1, "add -A");
    Tick();
    Check(Mep(wt1, "commit -m 'in worktree'", &out, &err) == 0, "commit in worktree", err);
    Check(sh.Out("git log -1 --format=%s ws1") == "in worktree\n", "worktree commit landed on ws1");
    Check(sh.Out("git status --porcelain") == "", "main worktree untouched", sh.Out("git status --porcelain"));
    // Dirty worktree refuses removal without --force.
    WriteFile(wt2 + "/dirty.txt", "d\n");
    Check(Mep(dir, "worktree remove " + wt2, &out, &err) == 128 && err.find("contains modified or untracked files") != std::string::npos,
          "worktree remove refuses dirty", err);
    Check(Mep(dir, "worktree remove --force " + wt2, &out, &err) == 0 && !FileExists(wt2), "worktree remove --force", err);
    Check(Mep(dir, "worktree remove " + wt1, &out, &err) == 0 && !FileExists(wt1), "worktree remove clean", err);
    Same(dir, "worktree list --porcelain");
    Check(sh.Out("git worktree list --porcelain").find("wt1") == std::string::npos, "git agrees worktree is gone");
    Check(sh.Out("git branch --list ws1") != "", "branch survives worktree removal");
    Fsck(sh, "worktree remove");
    // prune
    Mep(dir, "worktree add -b ws3 " + dir + "-wt3 HEAD");
    RemoveAll(dir + "-wt3");
    Check(Mep(dir, "worktree prune") == 0, "worktree prune");
    Check(sh.Out("git worktree list --porcelain").find("wt3") == std::string::npos, "pruned");
}

void TestPacked(const std::string &dir) {
    Shell sh{dir};
    sh.Run("git gc -q --aggressive 2>/dev/null; git repack -adq");
    TestLogAndRefs(dir, "packed");
    Same(dir, "status --porcelain --branch");
}

void TestIndexVersions() {
    for (int v : {2, 3, 4}) {
        std::string dir = MakeTemp(("idx" + std::to_string(v)).c_str());
        Shell sh{dir};
        sh.Run("git init -q . && git config index.version " + std::to_string(v));
        for (int i = 0; i < 20; i++) {
            MakeDirs(dir + "/dir" + std::to_string(i % 4));
            WriteFile(dir + "/dir" + std::to_string(i % 4) + "/file-" + std::to_string(i) + ".txt", std::to_string(i) + "\n");
        }
        sh.Run("git add -A");
        if (v == 3) sh.Run("git update-index --skip-worktree dir0/file-0.txt");
        DoCommit(sh, "c");
        Same(dir, "status --porcelain");
        // Read + write round trip must be byte-compatible enough for git.
        Index idx;
        std::string err;
        Check(idx.Load(dir + "/.git/index", &err), "index v" + std::to_string(v) + " load", err);
        Check(idx.entries.size() == 20, "index v" + std::to_string(v) + " entries");
        Check(idx.Save(dir + "/.git/index", &err), "index v" + std::to_string(v) + " save", err);
        Check(sh.Out("git status --porcelain") == "", "git status clean after v" + std::to_string(v) + " write",
              sh.Out("git status --porcelain"));
        Check(sh.Out("git ls-files -s | wc -l").find("20") != std::string::npos, "ls-files after write");
        if (v == 3) Check(sh.Out("git ls-files -v dir0/file-0.txt").rfind("S ", 0) == 0, "skip-worktree kept");
        // in-house add then git agrees
        WriteFile(dir + "/dir1/file-1.txt", "changed\n");
        Mep(dir, "add -A");
        Check(sh.Out("git status --porcelain") == "M  dir1/file-1.txt\n", "add on v" + std::to_string(v),
              sh.Out("git status --porcelain"));
    }
}

void TestStageContent() {
    std::string dir = MakeTemp("stagehunk");
    Shell sh{dir};
    sh.Run("git init -q .");
    WriteFile(dir + "/f.txt", "1\n2\n3\n");
    sh.Run("git add -A");
    DoCommit(sh, "c");
    WriteFile(dir + "/f.txt", "1\nTWO\n3\nfour\n");
    std::unique_ptr<Repo> r = Repo::Discover(dir, nullptr);
    std::string err;
    Check(r && StageContent(*r, "f.txt", "1\nTWO\n3\n", &err), "stage content", err);
    Check(sh.Out("git status --porcelain") == "MM f.txt\n", "partial stage", sh.Out("git status --porcelain"));
    Check(sh.Out("git diff --cached") .find("+TWO") != std::string::npos, "staged hunk");
    Check(sh.Out("git diff").find("+four") != std::string::npos, "unstaged hunk remains");
}

void TestBigRepo(const std::string &path) {
    if (path.empty() || !FileExists(path + "/.git")) return;
    std::printf("== this repository: %s\n", path.c_str());
    auto time_it = [&](const std::vector<std::string> &argv, std::string &out) {
        auto t0 = std::chrono::steady_clock::now();
        std::string err;
        RunCommand(argv, path, out, err);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    std::string out;
    double t_status = time_it({"git", "status", "--porcelain", "--branch"}, out);
    double t_status2 = time_it({"git", "status", "--porcelain", "--branch"}, out);
    double t_ign = time_it({"git", "status", "--ignored", "--porcelain"}, out);
    double t_log = time_it({"git", "log", "--date=short", "--format=%h\t%ad\t%an\t%D\t%s", "-n", "300"}, out);
    double t_graph = time_it({"git", "log", "--graph", "--all", "--color=always", "--date=short",
                              "--format=%x1f%h%x1f%ad%x1f%an%x1f%D%x1f%s", "-n", "500"}, out);
    std::printf("   status: %.3fs (warm %.3fs)  status --ignored: %.3fs  log -n 300: %.3fs  graph -n 500: %.3fs\n",
                t_status, t_status2, t_ign, t_log, t_graph);
    Check(t_status2 < 1.0, "status on this repo under a second");
    Same(path, {"status", "--porcelain", "--branch"}, "this repo status");
    Same(path, {"status", "--porcelain=v2", "--branch"}, "this repo status v2");
    Same(path, {"log", "--date=short", "--format=%h\t%ad\t%an\t%D\t%s", "-n", "300"}, "this repo log");
    Same(path, {"log", "--graph", "--all", "--color=always", "--date=short", "--format=%x1f%h%x1f%ad%x1f%an%x1f%D%x1f%s", "-n", "500"},
         "this repo graph");
    Same(path, {"for-each-ref", "--sort=-committerdate",
                "--format=%(HEAD)\t%(refname)\t%(refname:short)\t%(upstream:short)\t%(upstream:track)\t%(committerdate:relative)\t%(subject)",
                "refs/heads", "refs/remotes"},
         "this repo for-each-ref");
    Same(path, {"show", "--stat", "--patch", "--format=medium", "HEAD~1"}, "this repo show HEAD~1");
    Same(path, {"worktree", "list", "--porcelain"}, "this repo worktree list");
    Same(path, {"rev-parse", "--show-toplevel"}, "this repo toplevel");
}

}  // namespace

int main(int argc, char **argv) {
    std::string repo_path;
#ifdef MEP_SOURCE_DIR
    repo_path = MEP_SOURCE_DIR;
#endif
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--repo" && i + 1 < argc) repo_path = argv[++i];
    std::string git_version;
    if (RunProcess({"git", "--version"}, ".", {}, &git_version, nullptr) != 0) {
        std::printf("git not found: skipping mep-git-test (git is needed as the test oracle)\n");
        return 0;
    }
    // Benchmark/compare against this repository with the user's real
    // environment first, before SetEnv isolates HOME and config.
    TestBigRepo(repo_path);
    SetEnv();
    TestSha1();
    TestWildmatch();
    TestSupportedArgv();
    TestBasicRepo();
    std::string hist = BuildHistory();
    TestLogAndRefs(hist, "loose");
    TestPacked(hist);
    TestDiffAlgorithm();
    TestCombinedDiff();
    TestStatusEdgeCases();
    TestIndexVersions();
    TestWriteOps();
    TestStash();
    TestWorktrees();
    TestStageContent();
    std::printf("mep-git-test: %d passed, %d failed (%s)\n", g_pass, g_fail, Trim(git_version).c_str());
    if (g_fail) std::printf("scratch repositories kept in %s\n", g_root.c_str());
    else if (!g_root.empty()) RemoveAll(g_root);
    return g_fail ? 1 : 0;
}
