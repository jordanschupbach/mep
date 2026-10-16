// `git <command> ...` argv parsing and dispatch onto mep::git, printing
// what git itself would. See RunCommand in git.h.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "git/git.h"
#include "git/log.h"
#include "git/ops.h"
#include "git/util.h"

namespace mep::git {

namespace {

struct Argv {
    std::vector<std::string> globals_cwd;  // -C dirs, in order
    std::string cmd;
    std::vector<std::string> args;
    bool ok = true;
};

Argv ParseGlobals(const std::vector<std::string> &argv_in) {
    Argv a;
    size_t i = 0;
    if (!argv_in.empty() && (argv_in[0] == "git" || EndsWith(argv_in[0], "/git"))) i = 1;
    for (; i < argv_in.size(); i++) {
        const std::string &s = argv_in[i];
        if (s == "-C") {
            if (i + 1 >= argv_in.size()) {
                a.ok = false;
                return a;
            }
            a.globals_cwd.push_back(argv_in[++i]);
        } else if (s == "--no-optional-locks" || s == "--no-pager" || s == "-P" || s == "--literal-pathspecs") {
            continue;
        } else if (!s.empty() && s[0] == '-') {
            a.ok = false;  // -c, --git-dir, --work-tree, ...: leave to the real git
            return a;
        } else {
            a.cmd = s;
            a.args.assign(argv_in.begin() + static_cast<long>(i) + 1, argv_in.end());
            return a;
        }
    }
    a.ok = false;
    return a;
}

// Per-command option acceptance (anything else falls back to the CLI).
bool OptionKnown(const std::string &cmd, const std::string &opt, const std::string &sub) {
    auto in = [&](std::initializer_list<const char *> l) {
        for (const char *x : l)
            if (opt == x) return true;
        return false;
    };
    auto pre = [&](std::initializer_list<const char *> l) {
        for (const char *x : l)
            if (StartsWith(opt, x)) return true;
        return false;
    };
    if (opt == "--") return true;
    if (cmd == "status")
        return in({"--porcelain", "--porcelain=v1", "--porcelain=v2", "-s", "--short", "-b", "--branch", "--ignored",
                   "--ignored=traditional", "--show-stash", "--no-renames", "--renames", "--long", "-uno", "-unormal",
                   "-uall", "-u", "--no-ahead-behind", "--ahead-behind"}) ||
               pre({"--untracked-files"});
    if (cmd == "log" || cmd == "show")
        return in({"--oneline", "--decorate", "--no-decorate", "--decorate=short", "--decorate=no", "--graph", "--all",
                   "--color=always", "--color=never", "--color=auto", "--no-color", "--color", "--topo-order",
                   "--date-order", "--first-parent", "-g", "--walk-reflogs", "--stat", "-p", "-u", "--patch",
                   "--abbrev-commit", "--no-patch", "-s", "--no-walk"}) ||
               pre({"--date=", "--format=", "--pretty=", "-n", "--max-count="}) ||
               (opt.size() >= 2 && opt[0] == '-' && std::all_of(opt.begin() + 1, opt.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }));
    if (cmd == "diff")
        return in({"--cached", "--staged", "--stat", "--name-only", "--name-status", "--no-color", "--color=never",
                   "--quiet", "--exit-code", "-p", "--patch", "--no-renames", "-M"}) ||
               pre({"-U", "--unified="});
    if (cmd == "for-each-ref") return pre({"--format=", "--sort=", "--count="});
    if (cmd == "rev-parse")
        return in({"--abbrev-ref", "--show-toplevel", "--git-path", "--git-dir", "--git-common-dir",
                   "--absolute-git-dir", "--is-inside-work-tree", "--is-bare-repository", "--verify", "-q", "--quiet",
                   "--show-prefix", "--short", "--symbolic-full-name"}) ||
               pre({"--short="});
    if (cmd == "commit")
        return in({"-F", "--file", "-m", "--message", "--amend", "--allow-empty", "--no-verify", "-n", "-a", "--all",
                   "-q", "--quiet", "--no-edit"}) ||
               pre({"--cleanup=", "--file=", "--message="});
    if (cmd == "add") return in({"-A", "--all", "-f", "--force", "-u", "--update"});
    if (cmd == "rm") return in({"--cached", "-r", "-f", "--force", "-q", "--quiet"});
    if (cmd == "reset") return in({"--mixed", "--hard", "-q", "--quiet"});
    if (cmd == "clean") return in({"-f", "-d", "-n", "--dry-run", "-q", "--quiet", "-fd", "-df", "-ffd", "-fdf", "-dff", "--force"});
    if (cmd == "checkout") return in({"-b", "--detach", "-q", "--quiet"});
    if (cmd == "branch") return in({"-D", "-d", "--delete", "-f", "--force", "--list", "-q"});
    if (cmd == "merge") return in({"--ff", "--ff-only", "-q", "--quiet"});
    if (cmd == "stash") {
        if (sub == "push" || sub == "save") return in({"-u", "--include-untracked", "-m", "--message", "-q", "--quiet"});
        if (sub == "list") return pre({"--format="});
        if (sub == "show") return in({"-p", "--patch", "--stat", "-u"}) && opt != "-u";
        if (sub == "apply" || sub == "pop" || sub == "drop") return in({"-q", "--quiet"});
        return false;
    }
    if (cmd == "worktree") {
        if (sub == "add") return in({"-b", "--detach", "-f", "--force", "-q", "--quiet"});
        if (sub == "list") return in({"--porcelain", "-v", "--verbose"});
        if (sub == "remove") return in({"-f", "--force"});
        if (sub == "prune") return in({"-n", "--dry-run", "-v"});
        return false;
    }
    if (cmd == "hash-object") return in({"-w", "-t"});
    if (cmd == "cat-file") return in({"-t", "-s", "-e", "-p"});
    return false;
}

const std::set<std::string> kCommands = {"status", "log",   "show",  "diff",     "for-each-ref", "rev-parse",
                                         "commit", "add",   "rm",    "reset",    "clean",        "checkout",
                                         "branch", "merge", "stash", "worktree", "hash-object",  "cat-file"};

std::string Mode6(uint32_t m) {
    char b[16];
    std::snprintf(b, sizeof b, "%06o", m);
    return b;
}

// ------------------------------------------------------------ status

int CmdStatus(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err,
              const std::atomic<bool> *cancelled) {
    StatusOptions so;
    int porcelain = 0;  // 0 long, 1 v1/short, 2 v2
    bool branch = false, show_stash = false;
    bool after_dd = false;
    for (const std::string &a : args) {
        if (after_dd) {
            so.pathspec.push_back(NormalizePath(repo.Prefix() + a));
            continue;
        }
        if (a == "--") after_dd = true;
        else if (a == "--porcelain" || a == "--porcelain=v1" || a == "-s" || a == "--short") porcelain = 1;
        else if (a == "--porcelain=v2") porcelain = 2;
        else if (a == "--long") porcelain = 0;
        else if (a == "-b" || a == "--branch") branch = true;
        else if (a == "--ignored" || a == "--ignored=traditional") so.ignored = true;
        else if (a == "--show-stash") show_stash = true;
        else if (a == "--no-renames") so.renames = false;
        else if (a == "-uno" || a == "--untracked-files=no") {
            so.untracked = StatusOptions::Untracked::No;
            so.untracked_set = true;
        } else if (a == "-uall" || a == "--untracked-files=all" || a == "-u" || a == "--untracked-files") {
            so.untracked = StatusOptions::Untracked::All;
            so.untracked_set = true;
        } else if (a == "-unormal" || a == "--untracked-files=normal") {
            so.untracked = StatusOptions::Untracked::Normal;
            so.untracked_set = true;
        } else if (!a.empty() && a[0] != '-') {
            so.pathspec.push_back(NormalizePath(repo.Prefix() + a));
        }
    }
    so.ahead_behind = porcelain == 0 || branch;
    for (std::string &p : so.pathspec)
        if (p == ".") p.clear();
    StatusResult st;
    if (!Status(repo, so, st, &err, cancelled)) {
        if (!err.empty() && err.back() != '\n') err += "\n";
        return 128;
    }
    if (porcelain == 0) {
        out += LongStatus(repo, st);
        return 0;
    }
    const BranchInfo &b = st.branch;
    if (porcelain == 1) {
        if (branch) {
            std::string h = "## ";
            if (b.detached) h += "HEAD (no branch)";
            else if (b.unborn) h += "No commits yet on " + b.branch;
            else {
                h += b.branch;
                if (b.has_upstream) {
                    h += "..." + b.upstream;
                    if (b.gone) h += " [gone]";
                    else if (b.ahead && b.behind)
                        h += " [ahead " + std::to_string(b.ahead) + ", behind " + std::to_string(b.behind) + "]";
                    else if (b.ahead) h += " [ahead " + std::to_string(b.ahead) + "]";
                    else if (b.behind) h += " [behind " + std::to_string(b.behind) + "]";
                }
            }
            out += h + "\n";
        }
        for (const StatusEntry &e : st.changes) {
            out += e.x;
            out += e.y;
            out += ' ';
            if (!e.orig_path.empty()) out += QuotePath(e.orig_path, true, true) + " -> ";
            out += QuotePath(e.path, true, true) + "\n";
        }
        for (const std::string &u : st.untracked) out += "?? " + QuotePath(u, true, true) + "\n";
        for (const std::string &u : st.ignored) out += "!! " + QuotePath(u, true, true) + "\n";
        return 0;
    }
    // v2
    if (branch) {
        out += "# branch.oid " + (b.unborn || b.head.IsNull() ? std::string("(initial)") : b.head.Hex()) + "\n";
        out += "# branch.head " + (b.detached ? std::string("(detached)") : b.branch) + "\n";
        if (b.has_upstream) {
            out += "# branch.upstream " + b.upstream + "\n";
            if (!b.gone) out += "# branch.ab +" + std::to_string(b.ahead) + " -" + std::to_string(b.behind) + "\n";
        }
    }
    if (show_stash) {
        size_t n = repo.ReadReflog("refs/stash").size();
        if (n) out += "# stash " + std::to_string(n) + "\n";
    }
    for (const StatusEntry &e : st.changes) {
        char x = e.x == ' ' ? '.' : e.x, y = e.y == ' ' ? '.' : e.y;
        std::string sub = e.sub_state;
        bool unmerged = e.x == 'U' || e.y == 'U' || (e.x == 'A' && e.y == 'A') || (e.x == 'D' && e.y == 'D');
        if (unmerged) {
            out += std::string("u ") + x + y + " " + sub + " " + Mode6(e.stage_mode[1]) + " " + Mode6(e.stage_mode[2]) +
                   " " + Mode6(e.stage_mode[3]) + " " + Mode6(e.mode_wt) + " " + e.stage_oid[1].Hex() + " " +
                   e.stage_oid[2].Hex() + " " + e.stage_oid[3].Hex() + " " + QuotePath(e.path) + "\n";
            continue;
        }
        uint32_t mw = e.y == 'D' ? 0 : (e.mode_wt ? e.mode_wt : e.mode_index);
        std::string line = std::string(e.orig_path.empty() ? "1 " : "2 ") + x + y + " " + sub + " " + Mode6(e.mode_head) +
                           " " + Mode6(e.mode_index) + " " + Mode6(mw) + " " + e.oid_head.Hex() + " " + e.oid_index.Hex() + " ";
        if (!e.orig_path.empty()) line += "R" + std::to_string(e.score) + " " + QuotePath(e.path) + "\t" + QuotePath(e.orig_path);
        else line += QuotePath(e.path);
        out += line + "\n";
    }
    for (const std::string &u : st.untracked) out += "? " + QuotePath(u) + "\n";
    for (const std::string &u : st.ignored) out += "! " + QuotePath(u) + "\n";
    return 0;
}

// ------------------------------------------------------------ log / show

bool ParseLogArgs(const std::vector<std::string> &args, LogOptions &lo, std::vector<std::string> &paths,
                  std::string &err) {
    bool after_dd = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (after_dd) {
            paths.push_back(a);
            continue;
        }
        if (a == "--") {
            after_dd = true;
        } else if (a == "--oneline") {
            lo.format = LogOptions::Format::Oneline;
            lo.abbrev_commit = true;
        } else if (a == "--decorate" || a == "--decorate=short") {
            lo.decorate = 1;
        } else if (a == "--no-decorate" || a == "--decorate=no") {
            lo.decorate = 0;
        } else if (a == "--graph") {
            lo.graph = true;
        } else if (a == "--all") {
            lo.all = true;
        } else if (a == "--color=always" || a == "--color") {
            lo.color = true;
        } else if (a == "--color=never" || a == "--no-color" || a == "--color=auto") {
            lo.color = false;
        } else if (a == "--topo-order") {
            lo.topo = true;
        } else if (a == "--date-order") {
        } else if (a == "--first-parent") {
            lo.first_parent = true;
        } else if (a == "-g" || a == "--walk-reflogs") {
            lo.reflog_walk = true;
        } else if (a == "--stat") {
            lo.stat = true;
        } else if (a == "-p" || a == "-u" || a == "--patch") {
            lo.patch = true;
        } else if (a == "--no-patch" || a == "-s") {
            lo.patch = lo.stat = false;
        } else if (a == "--abbrev-commit") {
            lo.abbrev_commit = true;
        } else if (a == "--no-walk") {
            lo.no_walk = true;
        } else if (StartsWith(a, "--date=")) {
            std::string d = a.substr(7);
            if (d == "short") lo.date = LogOptions::Date::Short;
            else if (d == "relative") lo.date = LogOptions::Date::Relative;
            else if (d == "iso" || d == "iso8601") lo.date = LogOptions::Date::Iso;
            else if (d == "unix") lo.date = LogOptions::Date::Unix;
            else if (d == "raw") lo.date = LogOptions::Date::Raw;
            else if (d == "default") lo.date = LogOptions::Date::Default;
            else return false;
        } else if (StartsWith(a, "--format=") || StartsWith(a, "--pretty=")) {
            std::string f = a.substr(a.find('=') + 1);
            if (f == "oneline") lo.format = LogOptions::Format::Oneline;
            else if (f == "medium") lo.format = LogOptions::Format::Medium;
            else if (f == "short") lo.format = LogOptions::Format::Short;
            else if (f == "full") lo.format = LogOptions::Format::Full;
            else if (StartsWith(f, "format:")) {
                lo.format = LogOptions::Format::User;
                lo.user_format = f.substr(7);
                lo.tformat = false;
            } else if (StartsWith(f, "tformat:")) {
                lo.format = LogOptions::Format::User;
                lo.user_format = f.substr(8);
            } else if (f.find('%') != std::string::npos) {
                lo.format = LogOptions::Format::User;
                lo.user_format = f;
            } else {
                return false;
            }
        } else if (a == "-n") {
            if (i + 1 >= args.size()) return false;
            lo.max_count = std::strtol(args[++i].c_str(), nullptr, 10);
        } else if (StartsWith(a, "-n") && a.size() > 2) {
            lo.max_count = std::strtol(a.c_str() + 2, nullptr, 10);
        } else if (StartsWith(a, "--max-count=")) {
            lo.max_count = std::strtol(a.c_str() + 12, nullptr, 10);
        } else if (a.size() >= 2 && a[0] == '-' && std::isdigit(static_cast<unsigned char>(a[1]))) {
            lo.max_count = std::strtol(a.c_str() + 1, nullptr, 10);
        } else if (!a.empty() && a[0] == '-') {
            err = "fatal: unrecognized argument: " + a;
            return false;
        } else {
            lo.revs.push_back(a);
        }
    }
    return true;
}

int CmdLog(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    LogOptions lo;
    std::vector<std::string> paths;
    if (!ParseLogArgs(args, lo, paths, err)) {
        if (err.empty()) err = "fatal: unsupported log option";
        err += "\n";
        return 128;
    }
    if (lo.reflog_walk && !lo.revs.empty()) {
        lo.reflog_ref = lo.revs[0];
        lo.revs.clear();
    }
    int code = RunLog(repo, lo, out, err);
    if (!err.empty() && err.back() != '\n') err += "\n";
    return code;
}

int CmdShow(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    LogOptions lo;
    lo.no_walk = true;
    lo.patch = true;
    lo.combined_merges = true;
    lo.format = LogOptions::Format::Medium;
    std::vector<std::string> paths;
    bool stat_given = false, patch_given = false;
    for (const std::string &a : args) {
        if (a == "--stat") stat_given = true;
        if (a == "-p" || a == "--patch" || a == "-u") patch_given = true;
    }
    if (!ParseLogArgs(args, lo, paths, err)) {
        if (err.empty()) err = "fatal: unsupported show option";
        err += "\n";
        return 128;
    }
    if (stat_given && !patch_given) lo.patch = false;
    if (lo.revs.empty()) lo.revs.push_back("HEAD");
    // Each object: blobs print raw, trees list, tags print their header.
    std::string all_out;
    for (const std::string &r : lo.revs) {
        Oid o;
        uint32_t mode = 0;
        std::string e;
        if (!repo.RevParseWithMode(r, o, &mode, &e)) {
            err += (e.empty() ? "fatal: bad revision '" + r + "'" : e) + "\n";
            return 128;
        }
        ObjType t;
        std::string data;
        if ((mode & 0170000) == 0160000) {
            err += "fatal: bad object " + r + "\n";
            return 128;
        }
        if (!repo.Read(o, t, data)) {
            err += "fatal: bad object " + r + "\n";
            return 128;
        }
        if (t == ObjType::Blob) {
            all_out += data;
            continue;
        }
        if (t == ObjType::Tree) {
            std::vector<TreeEntry> ents;
            ParseTree(data, ents);
            all_out += "tree " + r + "\n\n";
            for (const TreeEntry &te : ents) all_out += te.name + (te.IsTree() ? "/" : "") + "\n";
            continue;
        }
        if (t == ObjType::Tag) {
            Tag tg;
            ParseTag(data, tg);
            all_out += "tag " + tg.name + "\n";
            if (tg.has_tagger) {
                all_out += "Tagger: " + tg.tagger.name + " <" + tg.tagger.email + ">\n";
                all_out += "Date:   " + FormatDateNormal(tg.tagger.time, tg.tagger.tz) + "\n";
            }
            all_out += "\n" + tg.message;
            if (!tg.message.empty() && tg.message.back() != '\n') all_out += "\n";
            all_out += "\n";
            Oid c;
            if (!repo.PeelToCommit(o, c)) continue;
            o = c;
        }
        LogOptions one = lo;
        one.revs = {o.Hex()};
        std::string piece;
        int code = RunLog(repo, one, piece, err);
        if (code) return code;
        all_out += piece;
    }
    out += all_out;
    return 0;
}

// ------------------------------------------------------------ diff

int CmdDiff(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    bool cached = false, stat = false, name_only = false, name_status = false, quiet = false, exit_code = false;
    bool patch = false, renames = true;
    int context = 3;
    std::vector<std::string> revs, paths;
    bool after_dd = false;
    for (const std::string &a : args) {
        if (after_dd) {
            std::string n = NormalizePath(repo.Prefix() + a);
            paths.push_back(n == "." ? "" : n);
            continue;
        }
        if (a == "--") after_dd = true;
        else if (a == "--cached" || a == "--staged") cached = true;
        else if (a == "--stat") stat = true;
        else if (a == "--name-only") name_only = true;
        else if (a == "--name-status") name_status = true;
        else if (a == "--quiet") quiet = exit_code = true;
        else if (a == "--exit-code") exit_code = true;
        else if (a == "-p" || a == "--patch") patch = true;
        else if (a == "--no-renames") renames = false;
        else if (StartsWith(a, "-U")) context = std::atoi(a.c_str() + 2);
        else if (StartsWith(a, "--unified=")) context = std::atoi(a.c_str() + 10);
        else if (!a.empty() && a[0] == '-') {
        } else revs.push_back(a);
    }
    if (!stat && !name_only && !name_status) patch = true;
    std::vector<DiffPair> pairs;
    Index idx;
    if (!idx.Load(repo.IndexFile(), &err)) {
        err = "fatal: " + err + "\n";
        return 128;
    }
    auto tree_of = [&](const std::string &r, Oid &tree) {
        Oid o;
        std::string e;
        if (!repo.RevParse(r, o, &e) || !repo.PeelToTree(o, tree)) {
            err += (e.empty() ? "fatal: bad revision '" + r + "'" : e) + "\n";
            return false;
        }
        return true;
    };
    if (revs.size() == 1 && revs[0].find("..") != std::string::npos) {
        std::string r = revs[0];
        size_t dd = r.find("..");
        revs = {r.substr(0, dd).empty() ? "HEAD" : r.substr(0, dd), r.substr(dd + 2).empty() ? "HEAD" : r.substr(dd + 2)};
    }
    if (revs.size() == 2) {
        Oid a, b;
        if (!tree_of(revs[0], a) || !tree_of(revs[1], b)) return 128;
        DiffTrees(repo, a, b, paths, pairs);
    } else if (cached) {
        Oid tree;
        if (revs.size() == 1) {
            if (!tree_of(revs[0], tree)) return 128;
        } else {
            Oid head;
            if (repo.Resolve("HEAD", head)) repo.PeelToTree(head, tree);
        }
        DiffTreeIndex(repo, tree, idx, paths, pairs);
    } else if (revs.size() == 1) {
        // tree vs worktree: tree vs index, then index entries refreshed from the worktree.
        Oid tree;
        if (!tree_of(revs[0], tree)) return 128;
        std::vector<DiffPair> wt;
        DiffIndexWorktree(repo, idx, paths, wt);
        std::map<std::string, TreeEntry> a, b;
        repo.FlattenTree(tree, a);
        for (const IndexEntry &e : idx.entries)
            if (!e.Stage() && PathspecMatch(paths, e.path)) b[e.path] = TreeEntry{e.mode, e.path, e.oid};
        std::map<std::string, std::string> abs_of;
        for (DiffPair &p : wt) {
            if (p.status == 'D') b.erase(p.a.path);
            else {
                if (p.b.oid.IsNull()) LoadSide(repo, p.b);
                b[p.a.path] = TreeEntry{p.b.mode, p.a.path, p.b.oid};
                abs_of[p.a.path] = p.b.abs;
            }
        }
        for (auto it = a.begin(); it != a.end();) it = PathspecMatch(paths, it->first) ? std::next(it) : a.erase(it);
        pairs = DiffMaps(a, b);
        for (DiffPair &p : pairs) {
            auto it = abs_of.find(p.b.path);
            if (it != abs_of.end()) p.b.abs = it->second;
        }
    } else {
        DiffIndexWorktree(repo, idx, paths, pairs);
    }
    if (renames && repo.Cfg().GetBool("diff.renames", true)) DetectRenames(repo, pairs);
    if (exit_code && !pairs.empty() && quiet) return 1;
    DiffOptions dopt;
    dopt.context = context;
    if (name_only)
        for (const DiffPair &p : pairs) out += QuotePath(p.b.mode ? p.b.path : p.a.path) + "\n";
    if (name_status)
        for (const DiffPair &p : pairs) {
            if (p.status == 'R') out += "R" + std::string(p.score < 100 ? "0" : "") + std::to_string(p.score) + "\t" + QuotePath(p.a.path) + "\t" + QuotePath(p.b.path) + "\n";
            else out += std::string(1, p.status) + "\t" + QuotePath(p.b.mode ? p.b.path : p.a.path) + "\n";
        }
    if (stat && !pairs.empty()) out += FormatStat(repo, pairs, dopt);
    if (patch) {
        if (stat && !pairs.empty()) out += "\n";
        out += FormatPatch(repo, pairs, dopt);
    }
    if (exit_code && !pairs.empty()) return 1;
    return 0;
}

// ------------------------------------------------------------ for-each-ref

struct RefInfo {
    std::string name;
    Oid oid;
    ObjType type = ObjType::None;
    bool loaded = false;
    Commit commit;
    bool is_commit = false;
    Tag tag;
    bool is_tag = false;
};

int CmdForEachRef(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    std::string format = "%(objectname) %(objecttype)\t%(refname)";
    std::vector<std::string> sorts, patterns;
    long count = -1;
    for (const std::string &a : args) {
        if (StartsWith(a, "--format=")) format = a.substr(9);
        else if (StartsWith(a, "--sort=")) sorts.push_back(a.substr(7));
        else if (StartsWith(a, "--count=")) count = std::strtol(a.c_str() + 8, nullptr, 10);
        else if (a == "--") continue;
        else if (!a.empty() && a[0] != '-') patterns.push_back(a);
    }
    std::vector<RefInfo> refs;
    for (const RefEntry &r : repo.ListRefs("refs/")) {
        if (!patterns.empty()) {
            bool m = false;
            for (std::string p : patterns) {
                if (p.find_first_of("*?[") != std::string::npos) {
                    if (WildMatch(p, r.name)) m = true;
                    continue;
                }
                while (!p.empty() && p.back() == '/') p.pop_back();
                if (r.name == p || StartsWith(r.name, p + "/")) m = true;
            }
            if (!m) continue;
        }
        RefInfo ri;
        ri.name = r.name;
        ri.oid = r.oid;
        refs.push_back(std::move(ri));
    }
    auto load = [&](RefInfo &ri) {
        if (ri.loaded) return;
        ri.loaded = true;
        std::string data;
        if (!repo.Read(ri.oid, ri.type, data)) return;
        if (ri.type == ObjType::Commit) ri.is_commit = ParseCommit(data, ri.commit);
        else if (ri.type == ObjType::Tag) ri.is_tag = ParseTag(data, ri.tag);
    };
    std::string head_ref = repo.HeadRef();
    int64_t now = NowSeconds();
    // Upstream tracking info, cached per ref.
    std::map<std::string, std::string> up_cache;
    auto upstream_of = [&](const std::string &ref) {
        auto it = up_cache.find(ref);
        if (it != up_cache.end()) return it->second;
        std::string u = repo.UpstreamOf(ref);
        up_cache[ref] = u;
        return u;
    };
    auto date_of = [&](RefInfo &ri, bool committer, Signature &s) {
        load(ri);
        if (ri.is_commit) {
            s = committer ? ri.commit.committer : ri.commit.author;
            return true;
        }
        if (ri.is_tag && ri.tag.has_tagger && !committer) {
            s = ri.tag.tagger;
            return true;
        }
        return false;
    };
    auto atom = [&](RefInfo &ri, const std::string &a) -> std::string {
        std::string name = a, mod;
        size_t colon = a.find(':');
        if (colon != std::string::npos) {
            name = a.substr(0, colon);
            mod = a.substr(colon + 1);
        }
        if (name == "refname") {
            if (mod == "short") return repo.ShortenRef(ri.name);
            if (StartsWith(mod, "lstrip=") || StartsWith(mod, "strip=")) {
                int n = std::atoi(mod.c_str() + mod.find('=') + 1);
                std::string s = ri.name;
                for (int k = 0; k < n; k++) {
                    size_t sl = s.find('/');
                    if (sl == std::string::npos) return "";
                    s = s.substr(sl + 1);
                }
                return s;
            }
            return ri.name;
        }
        if (name == "objectname") return mod == "short" ? repo.Abbrev(ri.oid) : ri.oid.Hex();
        if (name == "objecttype") {
            load(ri);
            return TypeName(ri.type);
        }
        if (name == "HEAD") return ri.name == head_ref ? "*" : " ";
        if (name == "symref") {
            Oid o;
            std::string sym;
            repo.ReadRefRaw(ri.name, o, sym);
            return mod == "short" && !sym.empty() ? repo.ShortenRef(sym) : sym;
        }
        if (name == "upstream") {
            std::string u = upstream_of(ri.name);
            if (u.empty()) return "";
            if (mod.empty()) return u;
            if (mod == "short") return repo.ShortenRef(u);
            if (mod == "track" || mod == "trackshort" || mod == "track,nobracket") {
                Oid uo, ho;
                if (!repo.Resolve(u, uo)) return mod == "trackshort" ? "" : (mod == "track" ? "[gone]" : "gone");
                repo.Resolve(ri.name, ho);
                int ahead = 0, behind = 0;
                repo.AheadBehind(ho, uo, ahead, behind);
                if (mod == "trackshort") {
                    if (ahead && behind) return "<>";
                    if (ahead) return ">";
                    if (behind) return "<";
                    return "=";
                }
                std::string s;
                if (ahead && behind) s = "ahead " + std::to_string(ahead) + ", behind " + std::to_string(behind);
                else if (ahead) s = "ahead " + std::to_string(ahead);
                else if (behind) s = "behind " + std::to_string(behind);
                if (s.empty()) return "";
                return mod == "track" ? "[" + s + "]" : s;
            }
            if (mod == "remotename") {
                std::string br = StartsWith(ri.name, "refs/heads/") ? ri.name.substr(11) : "";
                return repo.Cfg().Get("branch." + br + ".remote").value_or("");
            }
            return u;
        }
        if (name == "committerdate" || name == "authordate" || name == "creatordate" || name == "taggerdate") {
            Signature s;
            bool committer = name == "committerdate" || (name == "creatordate");
            if (!date_of(ri, committer, s)) {
                if (name == "creatordate" && !date_of(ri, false, s)) return "";
                if (name != "creatordate") return "";
            }
            if (mod == "relative") return FormatDateRelative(s.time, now);
            if (mod == "short") return FormatDateShort(s.time, s.tz);
            if (mod == "iso" || mod == "iso8601") return FormatDateIso(s.time, s.tz);
            if (mod == "unix") return std::to_string(s.time);
            return FormatDateNormal(s.time, s.tz);
        }
        if (name == "subject" || name == "contents:subject" || (name == "contents" && mod == "subject")) {
            load(ri);
            if (ri.is_commit) return CommitSubject(ri.commit.message);
            if (ri.is_tag) return CommitSubject(ri.tag.message);
            return "";
        }
        if (name == "body" || (name == "contents" && mod == "body")) {
            load(ri);
            if (ri.is_commit) return CommitBody(ri.commit.message);
            return "";
        }
        if (name == "authorname" || name == "committername") {
            load(ri);
            if (!ri.is_commit) return "";
            return name == "authorname" ? ri.commit.author.name : ri.commit.committer.name;
        }
        if (name == "authoremail" || name == "committeremail") {
            load(ri);
            if (!ri.is_commit) return "";
            return "<" + (name == "authoremail" ? ri.commit.author.email : ri.commit.committer.email) + ">";
        }
        return "";
    };
    // Sorting: keys applied in order given; the last --sort is primary in git.
    if (!sorts.empty()) {
        std::vector<std::string> keys(sorts.rbegin(), sorts.rend());
        std::vector<size_t> order(refs.size());
        for (size_t k = 0; k < order.size(); k++) order[k] = k;
        std::stable_sort(order.begin(), order.end(), [&](size_t ix, size_t iy) {
            RefInfo &x = refs[ix];
            RefInfo &y = refs[iy];
            for (const std::string &k0 : keys) {
                bool rev = !k0.empty() && k0[0] == '-';
                std::string k = rev ? k0.substr(1) : k0;
                int c = 0;
                if (k == "refname") c = x.name.compare(y.name);
                else if (k == "committerdate" || k == "authordate" || k == "creatordate") {
                    Signature sx, sy;
                    bool committer = k != "authordate";
                    int64_t tx = date_of(x, committer, sx) ? sx.time : (k == "creatordate" && date_of(x, false, sx) ? sx.time : 0);
                    int64_t ty = date_of(y, committer, sy) ? sy.time : (k == "creatordate" && date_of(y, false, sy) ? sy.time : 0);
                    c = tx < ty ? -1 : (tx > ty ? 1 : 0);
                } else {
                    c = atom(x, k).compare(atom(y, k));
                }
                if (c) return rev ? c > 0 : c < 0;
            }
            return x.name < y.name;
        });
        std::vector<RefInfo> sorted;
        sorted.reserve(refs.size());
        for (size_t k : order) sorted.push_back(std::move(refs[k]));
        refs = std::move(sorted);
    }
    long shown = 0;
    for (RefInfo &ri : refs) {
        if (count >= 0 && shown >= count) break;
        std::string line;
        for (size_t i = 0; i < format.size(); i++) {
            if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == '(') {
                size_t close = format.find(')', i);
                if (close == std::string::npos) {
                    err = "fatal: malformed format string " + format + "\n";
                    return 128;
                }
                line += atom(ri, format.substr(i + 2, close - i - 2));
                i = close;
            } else if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == '%') {
                line += '%';
                i++;
            } else if (format[i] == '%' && i + 2 < format.size() && std::isxdigit(static_cast<unsigned char>(format[i + 1])) &&
                       std::isxdigit(static_cast<unsigned char>(format[i + 2]))) {
                line += static_cast<char>(std::stoi(format.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                line += format[i];
            }
        }
        out += line + "\n";
        shown++;
    }
    return 0;
}

// ------------------------------------------------------------ rev-parse

std::string GitPath(Repo &repo, const std::string &p) {
    static const char *kCommon[] = {"branches", "common", "config", "hooks", "info", "logs", "lost-found", "objects",
                                    "packed-refs", "refs", "remotes", "rr-cache", "shallow", "worktrees", "modules",
                                    "description"};
    std::string top = p.substr(0, p.find('/'));
    bool common = false;
    for (const char *c : kCommon)
        if (top == c) common = true;
    if (p == "logs/HEAD" || StartsWith(p, "logs/HEAD") || StartsWith(p, "refs/bisect") ||
        StartsWith(p, "refs/worktree") || StartsWith(p, "logs/refs/bisect") || p == "info/sparse-checkout")
        common = false;
    return (common ? repo.CommonDir() : repo.GitDir()) + "/" + p;
}

int CmdRevParse(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    bool abbrev_ref = false, verify = false, quiet = false, symbolic_full = false;
    int short_len = 0;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "--abbrev-ref") abbrev_ref = true;
        else if (a == "--verify") verify = true;
        else if (a == "-q" || a == "--quiet") quiet = true;
        else if (a == "--symbolic-full-name") symbolic_full = true;
        else if (a == "--short") short_len = -1;
        else if (StartsWith(a, "--short=")) short_len = std::atoi(a.c_str() + 8);
        else if (a == "--show-toplevel") {
            if (repo.IsBare()) {
                err += "fatal: this operation must be run in a work tree\n";
                return 128;
            }
            out += repo.WorkTree() + "\n";
        } else if (a == "--show-prefix") {
            out += repo.Prefix() + "\n";
        } else if (a == "--git-dir" || a == "--absolute-git-dir") {
            out += repo.GitDir() + "\n";
        } else if (a == "--git-common-dir") {
            out += repo.CommonDir() + "\n";
        } else if (a == "--is-inside-work-tree") {
            out += repo.IsBare() ? "false\n" : "true\n";
        } else if (a == "--is-bare-repository") {
            out += repo.IsBare() ? "true\n" : "false\n";
        } else if (a == "--git-path") {
            if (i + 1 >= args.size()) {
                err += "fatal: --git-path requires an argument\n";
                return 128;
            }
            out += GitPath(repo, args[++i]) + "\n";
        } else if (a == "--") {
            continue;
        } else {
            if (abbrev_ref || symbolic_full) {
                std::string ref = repo.DwimRef(a);
                if (a == "HEAD" || a == "@") ref = repo.HeadRef().empty() ? "HEAD" : repo.HeadRef();
                std::string base = a;
                size_t at = a.find("@{");
                if (at != std::string::npos) {
                    std::string sel = a.substr(at + 2, a.size() - at - 3);
                    std::string b = a.substr(0, at);
                    std::string full = (b.empty() || b == "HEAD") ? repo.HeadRef() : repo.DwimRef(b);
                    if (sel == "u" || sel == "upstream") ref = repo.UpstreamOf(full);
                }
                if (ref.empty()) {
                    if (!quiet) err += "fatal: ambiguous argument '" + a + "': unknown revision or path not in the working tree.\n";
                    return 128;
                }
                if (abbrev_ref) out += (ref == "HEAD" ? std::string("HEAD") : repo.ShortenRef(ref)) + "\n";
                else out += ref + "\n";
                continue;
            }
            Oid o;
            std::string e;
            if (!repo.RevParse(a, o, &e)) {
                if (verify) {
                    if (!quiet) err += "fatal: Needed a single revision\n";
                    return 128;
                }
                err += (e.empty() ? "fatal: bad revision '" + a + "'" : e) + "\n";
                return 128;
            }
            if (short_len) out += repo.Abbrev(o, short_len > 0 ? short_len : -1) + "\n";
            else out += o.Hex() + "\n";
        }
    }
    return 0;
}

// ------------------------------------------------------------ small plumbing

int CmdHashObject(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    bool write = false;
    ObjType type = ObjType::Blob;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "-w") write = true;
        else if (a == "-t" && i + 1 < args.size()) type = TypeFromName(args[++i]);
        else if (a == "--") continue;
        else {
            std::string data;
            std::string path = AbsolutePath(a, JoinPath(repo.WorkTree(), repo.Prefix()));
            if (!ReadFile(path, data)) {
                err += "fatal: could not open '" + a + "' for reading: No such file or directory\n";
                return 128;
            }
            Oid o;
            if (write) {
                if (!repo.Write(type, data, o, &err)) return 128;
            } else {
                o = HashObject(type, data);
            }
            out += o.Hex() + "\n";
        }
    }
    return 0;
}

int CmdCatFile(Repo &repo, const std::vector<std::string> &args, std::string &out, std::string &err) {
    std::string mode, spec;
    for (const std::string &a : args) {
        if (a == "-t" || a == "-s" || a == "-e" || a == "-p") mode = a;
        else if (!a.empty() && a[0] != '-') {
            if (mode.empty() && TypeFromName(a) != ObjType::None && spec.empty()) mode = a;
            else spec = a;
        }
    }
    Oid o;
    if (!repo.RevParse(spec, o, &err)) {
        if (mode == "-e") return 1;
        err = "fatal: Not a valid object name " + spec + "\n";
        return 128;
    }
    ObjType t;
    std::string data;
    if (!repo.Read(o, t, data)) {
        if (mode == "-e") return 1;
        err = "fatal: Not a valid object name " + spec + "\n";
        return 128;
    }
    if (mode == "-e") return 0;
    if (mode == "-t") out += std::string(TypeName(t)) + "\n";
    else if (mode == "-s") out += std::to_string(data.size()) + "\n";
    else if (mode == "-p" && t == ObjType::Tree) {
        std::vector<TreeEntry> ents;
        ParseTree(data, ents);
        for (const TreeEntry &e : ents) {
            const char *tn = e.IsTree() ? "tree" : ((e.mode & 0170000) == 0160000 ? "commit" : "blob");
            out += Mode6(e.mode) + " " + tn + " " + e.oid.Hex() + "\t" + e.name + "\n";
        }
    } else {
        out += data;
    }
    return 0;
}

// ------------------------------------------------------------ dispatch helpers

int RunCli(const std::vector<std::string> &argv, const std::string &cwd, std::string &out, std::string &err) {
    std::vector<std::string> full = argv;
    if (full.empty() || (full[0] != "git" && !EndsWith(full[0], "/git"))) full.insert(full.begin(), "git");
    int code = RunProcess(full, cwd, {}, &out, &err);
    if (code == -1) {
        err += "error: this operation needs the git command-line tool, which could not be run\n";
        return 128;
    }
    return code;
}

}  // namespace

bool IsSupported(const std::vector<std::string> &argv) {
    Argv a = ParseGlobals(argv);
    if (!a.ok || !kCommands.count(a.cmd)) return false;
    std::string sub;
    size_t start = 0;
    if (a.cmd == "stash" || a.cmd == "worktree") {
        if (a.args.empty()) sub = a.cmd == "stash" ? "push" : "";
        else if (a.args[0][0] != '-') {
            sub = a.args[0];
            start = 1;
        } else if (a.cmd == "stash") {
            sub = "push";
        }
        if (a.cmd == "worktree" && sub.empty()) return false;
        static const std::set<std::string> kStash = {"push", "save", "list", "show", "apply", "pop", "drop"};
        static const std::set<std::string> kWt = {"add", "list", "remove", "prune"};
        if (a.cmd == "stash" && !kStash.count(sub)) return false;
        if (a.cmd == "worktree" && !kWt.count(sub)) return false;
    }
    bool after_dd = false;
    for (size_t i = start; i < a.args.size(); i++) {
        const std::string &s = a.args[i];
        if (after_dd) continue;
        if (s == "--") {
            after_dd = true;
            continue;
        }
        if (s.size() > 1 && s[0] == '-') {
            if (!OptionKnown(a.cmd, s, sub)) return false;
            // options that take a separate value
            if (((a.cmd == "log" || a.cmd == "show") && s == "-n") || (a.cmd == "commit" && (s == "-F" || s == "--file" || s == "-m" || s == "--message")) ||
                (a.cmd == "checkout" && s == "-b") || (a.cmd == "stash" && (s == "-m" || s == "--message")) ||
                (a.cmd == "worktree" && s == "-b") || (a.cmd == "rev-parse" && s == "--git-path") ||
                (a.cmd == "hash-object" && s == "-t"))
                i++;
        }
    }
    // log/show with a path limit is not in-house yet.
    if ((a.cmd == "log") && after_dd) {
        size_t dd = static_cast<size_t>(std::find(a.args.begin(), a.args.end(), "--") - a.args.begin());
        if (dd + 1 < a.args.size()) return false;
    }
    if (a.cmd == "hash-object" && std::find(a.args.begin(), a.args.end(), "--stdin") != a.args.end()) return false;
    return true;
}

int RunCommand(const std::vector<std::string> &argv, const std::string &cwd_in, std::string &out, std::string &err,
               const std::atomic<bool> *cancelled) {
    if (!IsSupported(argv)) return -2;
    Argv a = ParseGlobals(argv);
    std::string cwd = cwd_in.empty() ? "." : cwd_in;
    for (const std::string &c : a.globals_cwd) cwd = AbsolutePath(c, RealPath(cwd));
    std::string derr;
    std::unique_ptr<Repo> repo = Repo::Discover(cwd, &derr);
    if (!repo) {
        err += derr + "\n";
        return 128;
    }
    Repo &r = *repo;
    const std::vector<std::string> &args = a.args;
    Out o;
    int code = 0;
    bool need_cli = false;
    auto worktree_required = [&]() {
        if (r.IsBare()) {
            err += "fatal: this operation must be run in a work tree\n";
            return false;
        }
        return true;
    };
    const std::string &cmd = a.cmd;
    if (cmd == "status") {
        if (!worktree_required()) return 128;
        return CmdStatus(r, args, out, err, cancelled);
    }
    if (cmd == "log") return CmdLog(r, args, out, err);
    if (cmd == "show") return CmdShow(r, args, out, err);
    if (cmd == "diff") return CmdDiff(r, args, out, err);
    if (cmd == "for-each-ref") return CmdForEachRef(r, args, out, err);
    if (cmd == "rev-parse") return CmdRevParse(r, args, out, err);
    if (cmd == "hash-object") return CmdHashObject(r, args, out, err);
    if (cmd == "cat-file") return CmdCatFile(r, args, out, err);
    if (!worktree_required() && cmd != "branch" && cmd != "worktree") return 128;

    // Paths after "--" (and plain words) for the path-taking commands.
    auto split = [&](size_t from, std::vector<std::string> &opts, std::vector<std::string> &words) {
        bool after = false;
        for (size_t i = from; i < args.size(); i++) {
            if (after) {
                words.push_back(args[i]);
                continue;
            }
            if (args[i] == "--") {
                after = true;
                continue;
            }
            if (args[i].size() > 1 && args[i][0] == '-') opts.push_back(args[i]);
            else words.push_back(args[i]);
        }
        return after;
    };
    auto has = [](const std::vector<std::string> &v, std::initializer_list<const char *> l) {
        for (const char *x : l)
            if (std::find(v.begin(), v.end(), x) != v.end()) return true;
        return false;
    };

    if (cmd == "add") {
        std::vector<std::string> opts, paths;
        split(0, opts, paths);
        bool all = has(opts, {"-A", "--all"});
        if (has(opts, {"-u", "--update"})) {
            // -u: tracked files only.
            Index idx;
            idx.Load(r.IndexFile(), nullptr);
            std::vector<std::string> tracked;
            for (const IndexEntry &e : idx.entries) tracked.push_back(e.path);
            (void)tracked;
            all = true;
        }
        code = OpAdd(r, all, paths, has(opts, {"-f", "--force"}), o);
    } else if (cmd == "rm") {
        std::vector<std::string> opts, paths;
        split(0, opts, paths);
        code = OpRm(r, has(opts, {"--cached"}), has(opts, {"-r"}), has(opts, {"-f", "--force"}), paths, o);
    } else if (cmd == "reset") {
        std::vector<std::string> opts, words;
        bool dd = split(0, opts, words);
        std::string rev;
        std::vector<std::string> paths;
        if (dd) {
            // reset [rev] -- paths
            size_t pos = static_cast<size_t>(std::find(args.begin(), args.end(), "--") - args.begin());
            for (size_t i = 0; i < pos; i++)
                if (args[i][0] != '-') rev = args[i];
            paths.assign(args.begin() + static_cast<long>(pos) + 1, args.end());
        } else if (!words.empty()) {
            Oid tmp;
            if (r.RevParse(words[0], tmp, nullptr)) {
                rev = words[0];
                paths.assign(words.begin() + 1, words.end());
            } else {
                paths = words;
            }
        }
        code = OpReset(r, rev, paths, has(opts, {"--hard"}), o);
    } else if (cmd == "clean") {
        std::vector<std::string> opts, paths;
        split(0, opts, paths);
        int force_count = 0;
        bool dirs = false, dry = false;
        for (const std::string &op : opts) {
            if (op == "--force") force_count++;
            else if (op == "--dry-run") dry = true;
            else if (op[0] == '-' && op[1] != '-')
                for (char c : op.substr(1)) {
                    if (c == 'f') force_count++;
                    else if (c == 'd') dirs = true;
                    else if (c == 'n') dry = true;
                }
        }
        code = OpClean(r, force_count > 0, dirs, dry, force_count, paths, o);
    } else if (cmd == "commit") {
        CommitOptions co;
        for (size_t i = 0; i < args.size(); i++) {
            const std::string &s = args[i];
            if ((s == "-F" || s == "--file") && i + 1 < args.size()) co.message_file = args[++i];
            else if (StartsWith(s, "--file=")) co.message_file = s.substr(7);
            else if ((s == "-m" || s == "--message") && i + 1 < args.size()) {
                if (co.have_message) co.message += "\n";
                co.message += args[++i];
                if (!co.message.empty() && co.message.back() != '\n') co.message += "\n";
                co.have_message = true;
            } else if (StartsWith(s, "--message=")) {
                co.message += s.substr(10) + "\n";
                co.have_message = true;
            } else if (StartsWith(s, "--cleanup=")) co.cleanup = s.substr(10);
            else if (s == "--amend") co.amend = true;
            else if (s == "--allow-empty") co.allow_empty = true;
            else if (s == "--no-verify" || s == "-n") co.no_verify = true;
            else if (s == "-a" || s == "--all") co.all = true;
        }
        if (co.amend && !co.have_message && co.message_file.empty() &&
            std::find(args.begin(), args.end(), "--no-edit") != args.end()) {
            Oid head;
            Commit hc;
            if (r.Resolve("HEAD", head) && r.ReadCommit(head, hc)) {
                co.message = hc.message;
                co.have_message = true;
            }
        }
        code = OpCommit(r, co, o);
    } else if (cmd == "branch") {
        std::vector<std::string> opts, words;
        split(0, opts, words);
        if (has(opts, {"-D", "-d", "--delete"})) {
            bool force = has(opts, {"-D", "-f", "--force"});
            if (!force) {
                // -d: refuse unless merged into HEAD (or its upstream).
                std::vector<std::string> ok;
                for (const std::string &n : words) {
                    Oid b, h;
                    if (r.Resolve("refs/heads/" + n, b) && r.Resolve("HEAD", h) && !r.IsAncestor(b, h)) {
                        o.err += "error: the branch '" + n + "' is not fully merged\n"
                                 "hint: If you are sure you want to delete it, run 'git branch -D " + n + "'\n";
                        code = 1;
                        continue;
                    }
                    ok.push_back(n);
                }
                int c2 = OpBranchDelete(r, ok, false, o);
                if (c2) code = c2;
            } else {
                code = OpBranchDelete(r, words, true, o);
            }
        } else if (words.empty() || has(opts, {"--list"})) {
            std::string head = r.HeadRef();
            for (const RefEntry &re : r.ListRefs("refs/heads/")) {
                o.out += (re.name == head ? "* " : "  ") + re.name.substr(11) + "\n";
            }
        } else {
            code = OpBranchCreate(r, words[0], words.size() > 1 ? words[1] : "", has(opts, {"-f", "--force"}), o);
        }
    } else if (cmd == "checkout") {
        std::vector<std::string> opts, words;
        bool dd = split(0, opts, words);
        size_t dd_pos = static_cast<size_t>(std::find(args.begin(), args.end(), "--") - args.begin());
        if (has(opts, {"-b"})) {
            // checkout -b <name> [<start>]
            std::string name, start;
            for (size_t i = 0; i < args.size(); i++) {
                if (args[i] == "-b" && i + 1 < args.size()) {
                    name = args[++i];
                } else if (args[i][0] != '-' && !name.empty() && start.empty()) {
                    start = args[i];
                }
            }
            code = OpCheckoutNewBranch(r, name, start, o);
        } else if (dd) {
            std::string tree_ish;
            for (size_t i = 0; i < dd_pos; i++)
                if (args[i][0] != '-') tree_ish = args[i];
            std::vector<std::string> paths(args.begin() + static_cast<long>(dd_pos) + 1, args.end());
            code = OpCheckoutPaths(r, tree_ish, paths, o);
        } else if (words.size() == 1) {
            code = OpCheckout(r, words[0], has(opts, {"--detach"}), o);
        } else if (words.size() > 1) {
            code = OpCheckoutPaths(r, words[0], std::vector<std::string>(words.begin() + 1, words.end()), o);
        } else {
            o.err += "fatal: you must specify a branch to checkout\n";
            code = 128;
        }
    } else if (cmd == "merge") {
        std::vector<std::string> opts, words;
        split(0, opts, words);
        if (words.size() != 1) {
            need_cli = true;
        } else {
            code = OpMergeFastForward(r, words[0], has(opts, {"--ff-only"}), o, &need_cli);
        }
    } else if (cmd == "stash") {
        std::string sub = "push";
        size_t from = 0;
        if (!args.empty() && args[0][0] != '-') {
            sub = args[0];
            from = 1;
        }
        std::vector<std::string> opts, words;
        split(from, opts, words);
        if (sub == "push" || sub == "save") {
            std::string msg;
            for (size_t i = from; i < args.size(); i++)
                if ((args[i] == "-m" || args[i] == "--message") && i + 1 < args.size()) msg = args[++i];
            if (sub == "save" && !words.empty()) {
                for (const std::string &w : words) msg += (msg.empty() ? "" : " ") + w;
            } else if (!words.empty()) {
                need_cli = true;  // pathspec-limited stash
            }
            if (!need_cli) code = OpStashPush(r, has(opts, {"-u", "--include-untracked"}), msg, o);
        } else if (sub == "list") {
            std::string fmt;
            for (const std::string &op : opts)
                if (StartsWith(op, "--format=")) fmt = op.substr(9);
            code = OpStashList(r, fmt, o);
        } else if (sub == "show") {
            bool patch = has(opts, {"-p", "--patch"});
            bool stat = has(opts, {"--stat"});
            code = OpStashShow(r, words.empty() ? "" : words[0], patch, stat || !patch, o);
        } else if (sub == "apply" || sub == "pop") {
            code = OpStashApply(r, words.empty() ? "" : words[0], sub == "pop", o, &need_cli);
        } else if (sub == "drop") {
            code = OpStashDrop(r, words.empty() ? "" : words[0], o);
        }
    } else if (cmd == "worktree") {
        std::string sub = args.empty() ? "" : args[0];
        std::vector<std::string> opts, words;
        split(1, opts, words);
        if (sub == "add") {
            std::string new_branch;
            std::vector<std::string> pos;
            for (size_t i = 1; i < args.size(); i++) {
                if (args[i] == "-b" && i + 1 < args.size()) new_branch = args[++i];
                else if (args[i] == "--") continue;
                else if (args[i][0] != '-') pos.push_back(args[i]);
            }
            if (pos.empty()) {
                o.err += "usage: git worktree add [<options>] <path> [<commit-ish>]\n";
                code = 129;
            } else {
                code = OpWorktreeAdd(r, pos[0], pos.size() > 1 ? pos[1] : "", new_branch, has(opts, {"--detach"}),
                                     has(opts, {"-f", "--force"}), o);
            }
        } else if (sub == "list") {
            code = OpWorktreeList(r, has(opts, {"--porcelain"}), o);
        } else if (sub == "remove") {
            int force = 0;
            for (const std::string &op : opts)
                if (op == "-f" || op == "--force") force++;
            if (words.empty()) {
                o.err += "usage: git worktree remove [<options>] <worktree>\n";
                code = 129;
            } else {
                code = OpWorktreeRemove(r, words[0], force, o);
            }
        } else if (sub == "prune") {
            code = OpWorktreePrune(r, o);
        }
    }
    if (need_cli) {
        // Not (yet) in-house: non-fast-forward merge, conflicting stash
        // apply, pathspec stash. Run the real git for these.
        std::string cli_cwd = cwd_in.empty() ? "." : cwd_in;
        return RunCli(argv, cli_cwd, out, err);
    }
    out += o.out;
    err += o.err;
    return code;
}

}  // namespace mep::git
