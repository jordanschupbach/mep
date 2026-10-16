// Ignore rules, the untracked walk, worktree-vs-index comparison, status.

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "git/git.h"
#include "git/util.h"

namespace mep::git {

// ------------------------------------------------------------ ignore

IgnoreMatcher::IgnoreMatcher(Repo &repo) : repo_(repo) {
    ignore_case_ = repo.Cfg().GetBool("core.ignorecase", false);
    ParseFile(repo.GitDir() + "/info/exclude", "", global_);
    if (repo.CommonDir() != repo.GitDir()) ParseFile(repo.CommonDir() + "/info/exclude", "", global_);
    std::string excludes;
    if (auto v = repo.Cfg().Get("core.excludesfile")) {
        excludes = *v;
        if (StartsWith(excludes, "~/")) {
            const char *home = std::getenv("HOME");
            if (home) excludes = std::string(home) + excludes.substr(1);
        }
    } else {
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        if (xdg && *xdg) excludes = std::string(xdg) + "/git/ignore";
        else if (home) excludes = std::string(home) + "/.config/git/ignore";
    }
    // Lowest precedence first in global_: info/exclude beats excludesFile,
    // so excludesFile rules go in front (MatchList takes the last match).
    std::vector<Rule> ex;
    if (!excludes.empty()) ParseFile(excludes, "", ex);
    global_.insert(global_.begin(), ex.begin(), ex.end());
}

void IgnoreMatcher::ParseFile(const std::string &path, const std::string &base, std::vector<Rule> &out) {
    std::string text;
    if (!ReadFile(path, text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Trailing spaces are ignored unless escaped.
        while (!line.empty() && line.back() == ' ' && !(line.size() >= 2 && line[line.size() - 2] == '\\'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        Rule r;
        r.base = base;
        if (line[0] == '!') {
            r.negate = true;
            line = line.substr(1);
        } else if (StartsWith(line, "\\!") || StartsWith(line, "\\#")) {
            line = line.substr(1);
        }
        if (line.empty()) continue;
        if (line.back() == '/') {
            r.dir_only = true;
            line.pop_back();
            if (line.empty()) continue;
        }
        r.no_slash = line.find('/') == std::string::npos;
        if (line[0] == '/') line = line.substr(1);
        r.pattern = line;
        out.push_back(std::move(r));
    }
}

const IgnoreMatcher::Dir &IgnoreMatcher::DirRules(const std::string &dir) {
    auto it = dirs_.find(dir);
    if (it != dirs_.end()) return it->second;
    Dir d;
    ParseFile(JoinPath(repo_.WorkTree(), dir + ".gitignore"), dir, d.rules);
    return dirs_.emplace(dir, std::move(d)).first->second;
}

// 1 ignored, 0 explicitly not ignored (negated), -1 no rule matched.
int IgnoreMatcher::MatchList(const std::vector<Rule> &rules, const std::string &rel, const std::string &name,
                             bool is_dir) {
    for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
        const Rule &r = *it;
        if (r.dir_only && !is_dir) continue;
        bool m;
        if (r.no_slash) {
            m = WildMatch(r.pattern, name, false, ignore_case_);
        } else {
            if (!StartsWith(rel, r.base)) continue;
            m = WildMatch(r.pattern, std::string_view(rel).substr(r.base.size()), true, ignore_case_);
        }
        if (m) return r.negate ? 0 : 1;
    }
    return -1;
}

bool IgnoreMatcher::IsIgnored(const std::string &rel, bool is_dir) {
    // A path under an ignored directory is ignored, whatever its own rules say.
    size_t slash = 0;
    std::function<bool(const std::string &, bool)> self = [&](const std::string &p, bool d) -> bool {
        std::string name = BaseName(p);
        std::string parent = p.find('/') == std::string::npos ? "" : p.substr(0, p.rfind('/') + 1);
        // deepest .gitignore first
        std::string dir = parent;
        for (;;) {
            int r = MatchList(DirRules(dir).rules, p, name, d);
            if (r >= 0) return r == 1;
            if (dir.empty()) break;
            std::string trimmed = dir.substr(0, dir.size() - 1);
            size_t s = trimmed.rfind('/');
            dir = s == std::string::npos ? "" : trimmed.substr(0, s + 1);
        }
        int r = MatchList(global_, p, name, d);
        return r == 1;
    };
    while ((slash = rel.find('/', slash)) != std::string::npos) {
        if (self(rel.substr(0, slash), true)) return true;
        slash++;
    }
    return self(rel, is_dir);
}

// ------------------------------------------------------------ pathspec

bool PathspecMatch(const std::vector<std::string> &pathspec, const std::string &path, bool is_dir) {
    if (pathspec.empty()) return true;
    for (const std::string &ps_in : pathspec) {
        std::string ps = ps_in;
        while (!ps.empty() && ps.back() == '/') ps.pop_back();
        if (ps.empty() || ps == ".") return true;
        if (path == ps) return true;
        if (StartsWith(path, ps + "/")) return true;
        // A directory on the way down to the pathspec.
        if (is_dir && StartsWith(ps, path + "/")) return true;
        if (ps.find_first_of("*?[") != std::string::npos && WildMatch(ps, path, false)) return true;
    }
    return false;
}

// ------------------------------------------------------------ untracked

namespace {
struct UntrackedWalk {
    Repo &repo;
    const Index &idx;
    IgnoreMatcher ign;
    bool all, want_ignored;
    const std::vector<std::string> &pathspec;
    std::vector<std::string> &untracked;
    std::vector<std::string> *ignored;
    const std::atomic<bool> *cancelled;
    std::unordered_set<std::string> tracked_dirs;  // "a/b" for every dir holding an index entry
    std::unordered_set<std::string> tracked_files;
    std::unordered_set<std::string> gitlinks;

    void Init() {
        for (const IndexEntry &e : idx.entries) {
            tracked_files.insert(e.path);
            if ((e.mode & 0170000) == 0160000) gitlinks.insert(e.path);
            size_t s = 0;
            while ((s = e.path.find('/', s)) != std::string::npos) {
                tracked_dirs.insert(e.path.substr(0, s));
                s++;
            }
        }
    }

    bool Cancelled() const { return cancelled && cancelled->load(); }

    static bool IsNestedRepo(const std::string &abs) {
        std::string dg = abs + "/.git";
        if (IsDirectory(dg)) return FileExists(dg + "/HEAD");
        std::string text;
        if (ReadFile(dg, text)) return StartsWith(Trim(text), "gitdir:");
        return false;
    }

    // Classifies an untracked directory's contents without listing them:
    // has_untracked (a non-ignored file somewhere), has_any (any file at all).
    // Collects ignored entries (top-most ignored path) into `ign_out` when
    // given. `stop_early`: return as soon as the answer can't change (the
    // first file when ign_out is null and only has_any matters, or the
    // first untracked file).
    enum class Need { AnyFile, AnyUntracked, Everything };
    void ScanImpl(const std::string &rel_dir, bool &has_untracked, bool &has_any, std::vector<std::string> *ign_out,
                  Need need) {
        std::vector<DirEntry> ents;
        if (!ListDir(JoinPath(repo.WorkTree(), rel_dir), ents)) return;
        std::sort(ents.begin(), ents.end(), [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
        auto done = [&]() {
            if (need == Need::AnyFile) return has_any;
            if (need == Need::AnyUntracked) return has_untracked;
            return false;
        };
        for (const DirEntry &e : ents) {
            if (Cancelled()) return;
            if (e.name == ".git") continue;
            std::string path = rel_dir + "/" + e.name;
            bool is_dir = e.kind == DirEntry::Dir;
            if (is_dir) {
                std::string abs = JoinPath(repo.WorkTree(), path);
                bool excluded = need == Need::AnyFile ? false : ign.IsIgnored(path, true);
                if (IsNestedRepo(abs)) {
                    has_any = true;
                    if (excluded) {
                        if (ign_out) ign_out->push_back(path + "/");
                    } else {
                        has_untracked = true;
                    }
                } else if (excluded) {
                    bool u = false, any = false;
                    ScanImpl(path, u, any, nullptr, Need::AnyFile);
                    if (any) {
                        has_any = true;
                        if (ign_out) ign_out->push_back(path + "/");
                    }
                } else {
                    ScanImpl(path, has_untracked, has_any, ign_out, need);
                }
            } else {
                has_any = true;
                if (need != Need::AnyFile) {
                    if (ign.IsIgnored(path, false)) {
                        if (ign_out) ign_out->push_back(path);
                    } else {
                        has_untracked = true;
                    }
                }
            }
            if (done()) return;
        }
    }

    void ListAllIgnored(const std::string &rel_dir) {
        std::vector<DirEntry> ents;
        if (!ListDir(JoinPath(repo.WorkTree(), rel_dir), ents)) return;
        for (const DirEntry &e : ents) {
            if (Cancelled()) return;
            if (e.name == ".git") continue;
            std::string path = rel_dir + "/" + e.name;
            if (e.kind == DirEntry::Dir) {
                if (IsNestedRepo(JoinPath(repo.WorkTree(), path))) ignored->push_back(path + "/");
                else ListAllIgnored(path);
            } else {
                ignored->push_back(path);
            }
        }
    }

    void Walk(const std::string &rel_dir) {
        std::vector<DirEntry> ents;
        if (!ListDir(rel_dir.empty() ? repo.WorkTree() : JoinPath(repo.WorkTree(), rel_dir), ents)) return;
        for (const DirEntry &e : ents) {
            if (Cancelled()) return;
            if (e.name == ".git") continue;
            std::string path = rel_dir.empty() ? e.name : rel_dir + "/" + e.name;
            bool is_dir = e.kind == DirEntry::Dir;
            if (!PathspecMatch(pathspec, path, is_dir)) continue;
            if (is_dir) {
                if (gitlinks.count(path) || tracked_files.count(path)) continue;
                if (tracked_dirs.count(path)) {
                    Walk(path);
                    continue;
                }
                std::string abs = JoinPath(repo.WorkTree(), path);
                bool excluded = ign.IsIgnored(path, true);
                if (IsNestedRepo(abs)) {
                    if (excluded) {
                        if (want_ignored && ignored) ignored->push_back(path + "/");
                    } else {
                        untracked.push_back(path + "/");
                    }
                    continue;
                }
                if (excluded) {
                    if (!want_ignored || !ignored) continue;
                    if (all) {
                        // -uall lists what an ignored directory holds, file by file.
                        ListAllIgnored(path);
                        continue;
                    }
                    bool u = false, any = false;
                    ScanImpl(path, u, any, nullptr, Need::AnyFile);
                    if (any) ignored->push_back(path + "/");
                    continue;
                }
                if (all) {
                    Walk(path);
                    continue;
                }
                bool has_u = false, any = false;
                std::vector<std::string> inner_ignored;
                ScanImpl(path, has_u, any, want_ignored ? &inner_ignored : nullptr,
                         want_ignored ? Need::Everything : Need::AnyUntracked);
                if (has_u) {
                    untracked.push_back(path + "/");
                    if (want_ignored && ignored)
                        for (auto &p : inner_ignored) ignored->push_back(p);
                } else if (any && want_ignored && ignored) {
                    ignored->push_back(path + "/");
                }
                continue;
            }
            if (tracked_files.count(path)) continue;
            if (ign.IsIgnored(path, false)) {
                if (want_ignored && ignored) ignored->push_back(path);
            } else {
                untracked.push_back(path);
            }
        }
    }
};
}  // namespace

void ListUntracked(Repo &repo, const Index &idx, bool all, bool want_ignored, const std::vector<std::string> &pathspec,
                   std::vector<std::string> &untracked, std::vector<std::string> *ignored,
                   const std::atomic<bool> *cancelled) {
    UntrackedWalk w{repo, idx, IgnoreMatcher(repo), all, want_ignored, pathspec, untracked, ignored, cancelled, {}, {}, {}};
    w.Init();
    w.Walk("");
    std::sort(untracked.begin(), untracked.end());
    if (ignored) std::sort(ignored->begin(), ignored->end());
}

// ------------------------------------------------------------ worktree diff

namespace {
// Does the submodule at `abs` differ from `recorded` (HEAD moved, or dirty)?
struct SubmoduleState {
    bool checked_out = false, commit_changed = false, modified = false, untracked = false;
    Oid head;
};
SubmoduleState GetSubmoduleState(const std::string &abs, const Oid &recorded) {
    SubmoduleState s;
    if (!IsDirectory(abs) || !FileExists(abs + "/.git")) return s;
    std::string err;
    std::unique_ptr<Repo> sub = Repo::Discover(abs, &err);
    if (!sub || sub->WorkTree() != RealPath(abs)) return s;
    s.checked_out = true;
    if (!sub->Resolve("HEAD", s.head) || s.head != recorded) s.commit_changed = true;
    StatusOptions o;
    o.untracked_set = true;
    o.untracked = StatusOptions::Untracked::Normal;
    o.ahead_behind = false;
    StatusResult r;
    if (Status(*sub, o, r, &err)) {
        s.modified = !r.changes.empty();
        s.untracked = !r.untracked.empty();
    }
    return s;
}
}  // namespace

bool DiffIndexWorktree(Repo &repo, const Index &idx, const std::vector<std::string> &pathspec,
                       std::vector<DiffPair> &out) {
    const Config &cfg = repo.Cfg();
    std::string last_conflict;
    for (const IndexEntry &e : idx.entries) {
        if (!PathspecMatch(pathspec, e.path)) continue;
        if (e.Stage() != 0) {
            // git diff shows unmerged paths as "* Unmerged path"; skip here.
            continue;
        }
        if (e.SkipWorktree() || e.AssumeValid()) continue;
        std::string abs = JoinPath(repo.WorkTree(), e.path);
        FileStat st;
        DiffPair p;
        p.a.path = p.b.path = e.path;
        p.a.mode = e.mode;
        p.a.oid = e.oid;
        if ((e.mode & 0170000) == 0160000) {
            SubmoduleState ss = GetSubmoduleState(abs, e.oid);
            if (ss.commit_changed || ss.modified || ss.untracked) {
                p.status = 'M';
                p.b.mode = e.mode;
                p.b.oid = ss.head;
                p.b.dirty = ss.modified || ss.untracked;
                p.sub_state = std::string("S") + (ss.commit_changed ? 'C' : '.') + (ss.modified ? 'M' : '.') +
                              (ss.untracked ? 'U' : '.');
                out.push_back(std::move(p));
            }
            continue;
        }
        if (!LStat(abs, st) || st.is_dir) {
            if (e.IntentToAdd()) continue;
            p.status = 'D';
            p.b.mode = 0;
            out.push_back(std::move(p));
            continue;
        }
        if (e.IntentToAdd()) {
            p.status = 'A';
            p.a.mode = 0;
            p.a.oid = Oid{};
            p.b.mode = ModeFromStat(st, cfg, e.mode);
            p.b.abs = abs;
            out.push_back(std::move(p));
            continue;
        }
        uint32_t wt_mode = ModeFromStat(st, cfg, e.mode);
        bool type_changed = (wt_mode & 0170000) != (e.mode & 0170000);
        bool racy = !idx.loaded_from_file || e.mtime_s > static_cast<uint32_t>(idx.file_mtime_s) ||
                    (e.mtime_s == static_cast<uint32_t>(idx.file_mtime_s) &&
                     e.mtime_ns >= static_cast<uint32_t>(idx.file_mtime_ns));
        if (!type_changed && StatMatches(e, st, cfg) && !racy) continue;
        if (type_changed) {
            p.status = 'T';
            p.b.mode = wt_mode;
            p.b.abs = abs;
            out.push_back(std::move(p));
            continue;
        }
        Oid h;
        std::string content;
        if (!HashWorktreeFile(abs, st, h, &content)) continue;
        if (h == e.oid && wt_mode == e.mode) continue;
        p.status = 'M';
        p.b.mode = wt_mode;
        p.b.oid = h;
        p.b.abs = abs;
        p.b.data = std::move(content);
        p.b.loaded = true;
        out.push_back(std::move(p));
    }
    (void)last_conflict;
    return true;
}

bool DiffTreeIndex(Repo &repo, const Oid &tree, const Index &idx, const std::vector<std::string> &pathspec,
                   std::vector<DiffPair> &out) {
    std::map<std::string, TreeEntry> head;
    if (!tree.IsNull() && !repo.FlattenTree(tree, head)) return false;
    std::map<std::string, TreeEntry> index;
    for (const IndexEntry &e : idx.entries) {
        if (e.Stage() != 0 || e.IntentToAdd()) continue;
        index[e.path] = TreeEntry{e.mode, e.path, e.oid};
    }
    if (!pathspec.empty()) {
        for (auto it = head.begin(); it != head.end();) it = PathspecMatch(pathspec, it->first) ? std::next(it) : head.erase(it);
        for (auto it = index.begin(); it != index.end();) it = PathspecMatch(pathspec, it->first) ? std::next(it) : index.erase(it);
    }
    out = DiffMaps(head, index);
    return true;
}

// ------------------------------------------------------------ status

bool GetBranchInfo(Repo &repo, BranchInfo &out, bool with_ab) {
    out = BranchInfo{};
    out.head_ref = repo.HeadRef();
    if (out.head_ref.empty()) {
        out.detached = true;
        repo.Resolve("HEAD", out.head);
        return true;
    }
    out.branch = StartsWith(out.head_ref, "refs/heads/") ? out.head_ref.substr(11) : out.head_ref;
    if (!repo.Resolve(out.head_ref, out.head)) out.unborn = true;
    std::string up = repo.UpstreamOf(out.head_ref);
    if (!up.empty()) {
        out.has_upstream = true;
        out.upstream = repo.ShortenRef(up);
        Oid up_oid;
        if (!repo.Resolve(up, up_oid)) {
            out.gone = true;
        } else if (with_ab && !out.unborn) {
            repo.AheadBehind(out.head, up_oid, out.ahead, out.behind);
        }
    }
    return true;
}

bool Status(Repo &repo, const StatusOptions &opts, StatusResult &out, std::string *err,
            const std::atomic<bool> *cancelled) {
    out = StatusResult{};
    if (repo.IsBare()) {
        if (err) *err = "fatal: this operation must be run in a work tree";
        return false;
    }
    GetBranchInfo(repo, out.branch, opts.ahead_behind);
    Index idx;
    if (!idx.Load(repo.IndexFile(), err)) return false;
    Oid head_tree;
    if (!out.branch.unborn) {
        Oid head = out.branch.head;
        if (!head.IsNull() && !repo.PeelToTree(head, head_tree)) {
            if (err) *err = "fatal: bad HEAD";
            return false;
        }
    }
    // Staged.
    std::vector<DiffPair> staged;
    DiffTreeIndex(repo, head_tree, idx, opts.pathspec, staged);
    if (opts.renames && repo.Cfg().GetBool("status.renames", repo.Cfg().GetBool("diff.renames", true)))
        DetectRenames(repo, staged);
    // Unstaged.
    std::vector<DiffPair> unstaged;
    DiffIndexWorktree(repo, idx, opts.pathspec, unstaged);
    if (cancelled && cancelled->load()) return false;

    std::map<std::string, StatusEntry> by_path;
    for (const DiffPair &p : staged) {
        StatusEntry &e = by_path[p.b.mode ? p.b.path : p.a.path];
        e.path = p.b.mode ? p.b.path : p.a.path;
        e.x = p.status;
        if (p.status == 'R') {
            e.orig_path = p.a.path;
            e.score = p.score;
        }
        e.mode_head = p.a.mode;
        e.oid_head = p.a.oid;
        e.mode_index = p.b.mode;
        e.oid_index = p.b.oid;
        if ((p.a.mode & 0170000) == 0160000 || (p.b.mode & 0170000) == 0160000) {
            e.submodule = true;
            e.sub_state = "S...";
        }
    }
    for (const DiffPair &p : unstaged) {
        StatusEntry &e = by_path[p.a.path];
        if (e.path.empty()) {
            e.path = p.a.path;
            long i = idx.Find(p.a.path);
            if (i >= 0 && !idx.entries[static_cast<size_t>(i)].IntentToAdd()) {
                e.mode_index = idx.entries[static_cast<size_t>(i)].mode;
                e.oid_index = idx.entries[static_cast<size_t>(i)].oid;
                e.mode_head = e.mode_index;
                e.oid_head = e.oid_index;
            }
        }
        e.y = p.status;
        e.mode_wt = p.b.mode;
        if ((p.a.mode & 0170000) == 0160000) e.submodule = true;
        if (!p.sub_state.empty()) e.sub_state = p.sub_state;
    }
    // Unmerged paths.
    {
        std::map<std::string, int> stages;
        for (const IndexEntry &ie : idx.entries)
            if (ie.Stage() && PathspecMatch(opts.pathspec, ie.path)) stages[ie.path] |= 1 << ie.Stage();
        for (const auto &[path, mask] : stages) {
            StatusEntry &e = by_path[path];
            e.path = path;
            // git's stagemask: 1 = base, 2 = ours, 4 = theirs
            static const char *kCodes[8] = {"??", "DD", "AU", "UD", "UA", "DU", "AA", "UU"};
            int m = ((mask & 2) ? 1 : 0) | ((mask & 4) ? 2 : 0) | ((mask & 8) ? 4 : 0);
            const char *code = kCodes[m];
            e.x = code[0];
            e.y = code[1];
            for (int s = 1; s <= 3; s++) {
                long si = idx.Find(path, s);
                if (si < 0) continue;
                e.stage_mode[s] = idx.entries[static_cast<size_t>(si)].mode;
                e.stage_oid[s] = idx.entries[static_cast<size_t>(si)].oid;
            }
            FileStat wst;
            if (LStat(JoinPath(repo.WorkTree(), path), wst)) e.mode_wt = ModeFromStat(wst, repo.Cfg(), e.stage_mode[2] ? e.stage_mode[2] : 0100644);
        }
    }
    for (auto &kv : by_path) out.changes.push_back(std::move(kv.second));

    StatusOptions::Untracked mode = opts.untracked;
    if (!opts.untracked_set) {
        std::string v = repo.Cfg().Get("status.showuntrackedfiles").value_or("normal");
        if (v == "no" || v == "false") mode = StatusOptions::Untracked::No;
        else if (v == "all") mode = StatusOptions::Untracked::All;
        else mode = StatusOptions::Untracked::Normal;
    }
    if (mode != StatusOptions::Untracked::No || opts.ignored) {
        std::vector<std::string> untracked;
        ListUntracked(repo, idx, mode == StatusOptions::Untracked::All, opts.ignored, opts.pathspec, untracked,
                      opts.ignored ? &out.ignored : nullptr, cancelled);
        if (mode != StatusOptions::Untracked::No) out.untracked = std::move(untracked);
    }
    return true;
}

}  // namespace mep::git
