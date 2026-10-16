// Write-side git commands for mep's git UI. See ops.h.

#include "git/ops.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <functional>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "git/log.h"
#include "git/util.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mep::git {

namespace {

using TreeMap = std::map<std::string, TreeEntry>;

bool HeadCommit(Repo &repo, Oid &out) { return repo.Resolve("HEAD", out); }

bool CommitTreeMap(Repo &repo, const Oid &commit_or_tree, TreeMap &out) {
    out.clear();
    if (commit_or_tree.IsNull()) return true;
    Oid tree;
    if (!repo.PeelToTree(commit_or_tree, tree)) return false;
    return repo.FlattenTree(tree, out);
}

std::string BranchShort(Repo &repo) {
    std::string h = repo.HeadRef();
    if (StartsWith(h, "refs/heads/")) return h.substr(11);
    return "";
}

std::string OnelineOf(Repo &repo, const Oid &oid) {
    Commit c;
    if (!repo.ReadCommit(oid, c)) return repo.Abbrev(oid);
    return repo.Abbrev(oid) + " " + CommitSubject(c.message);
}

bool Same(const TreeEntry *a, const TreeEntry *b) {
    if (!a || !b) return a == b;
    return a->mode == b->mode && a->oid == b->oid;
}

#if !defined(_WIN32)
mode_t Umask() {
    static mode_t m = [] {
        mode_t u = ::umask(022);
        ::umask(u);
        return u;
    }();
    return m;
}
#endif

bool RemoveDirIfEmpty(const std::string &abs) {
    std::vector<DirEntry> ents;
    if (!ListDir(abs, ents) || !ents.empty()) return false;
#if !defined(_WIN32)
    return ::rmdir(abs.c_str()) == 0;
#else
    return RemoveAll(abs);
#endif
}

// Writes a blob to the worktree at `rel` (replacing whatever is there).
bool WriteToWorktree(Repo &repo, const std::string &rel, uint32_t mode, const Oid &oid, std::string *err) {
    std::string abs = JoinPath(repo.WorkTree(), rel);
    if ((mode & 0170000) == 0160000) {
        MakeDirs(abs);  // submodule: just the (empty) directory, like git
        return true;
    }
    std::string data;
    if (!repo.ReadBlob(oid, data)) {
        if (err) *err = "error: unable to read sha1 file of " + rel + " (" + oid.Hex() + ")";
        return false;
    }
    MakeDirs(DirName(abs));
    FileStat st;
    if (LStat(abs, st)) {
        if (st.is_dir) {
            // Only an empty directory may give way to a file; anything in
            // it is untracked work (SwitchTrees refuses before we get here).
            if (!RemoveDirIfEmpty(abs)) {
                if (err) *err = "error: Updating '" + rel + "' would lose untracked files in it";
                return false;
            }
        } else {
            RemoveFile(abs);
        }
    }
#if !defined(_WIN32)
    if ((mode & 0170000) == 0120000) {
        if (::symlink(data.c_str(), abs.c_str()) != 0) {
            if (err) *err = "error: unable to create symlink " + rel + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }
#endif
    bool exec = (mode & 0111) != 0;
    if (!WriteFile(abs, data, exec)) {
        if (err) *err = "error: unable to write file " + rel;
        return false;
    }
#if !defined(_WIN32)
    ::chmod(abs.c_str(), (exec ? 0777 : 0666) & ~Umask());
#endif
    return true;
}

// An index entry for the worktree file at `rel` (writes its blob when `write`).
bool EntryFromWorktree(Repo &repo, const std::string &rel, const IndexEntry *existing, IndexEntry &out, bool write,
                       std::string *err) {
    std::string abs = JoinPath(repo.WorkTree(), rel);
    FileStat st;
    if (!LStat(abs, st)) return false;
    out = existing ? *existing : IndexEntry{};
    out.path = rel;
    out.flags = 0;
    out.flags_ext = 0;
    if (st.is_dir) {
        // An embedded repository: record its HEAD as a gitlink.
        std::string e;
        std::unique_ptr<Repo> sub = Repo::Discover(abs, &e);
        if (!sub || !sub->Resolve("HEAD", out.oid)) {
            if (err) *err = "error: '" + rel + "' does not have a commit checked out";
            return false;
        }
        out.mode = 0160000;
        FillStat(out, st);
        out.size = 0;
        return true;
    }
    std::string data;
    if (!ReadWorktreeFile(abs, st, data)) return false;
    out.mode = ModeFromStat(st, repo.Cfg(), existing ? existing->mode : 0);
    if (write) {
        if (!repo.Write(ObjType::Blob, data, out.oid, err)) return false;
    } else {
        out.oid = HashObject(ObjType::Blob, data);
    }
    FillStat(out, st);
    return true;
}

// git's verify_uptodate: is the worktree file still what the index says?
bool WorktreeUpToDate(Repo &repo, const Index &idx, const IndexEntry &e) {
    if ((e.mode & 0170000) == 0160000) return true;
    std::string abs = JoinPath(repo.WorkTree(), e.path);
    FileStat st;
    if (!LStat(abs, st)) return true;  // missing is fine
    if (st.is_dir) return false;
    uint32_t wt_mode = ModeFromStat(st, repo.Cfg(), e.mode);
    if (wt_mode != e.mode) return false;
    bool racy = !idx.loaded_from_file || e.mtime_s >= static_cast<uint32_t>(idx.file_mtime_s);
    if (StatMatches(e, st, repo.Cfg()) && !racy) return true;
    Oid h;
    if (!HashWorktreeFile(abs, st, h)) return false;
    return h == e.oid;
}

void RemoveFromWorktree(Repo &repo, const std::string &rel) {
    std::string abs = JoinPath(repo.WorkTree(), rel);
    FileStat st;
    if (LStat(abs, st)) {
        // A gitlink's directory goes only if empty: like git, never delete
        // a checked-out submodule (or anything else) recursively here.
        if (st.is_dir) RemoveDirIfEmpty(abs);
        else RemoveFile(abs);
    }
    RemoveEmptyParents(abs, repo.WorkTree());
}

void RefreshEntryStat(Repo &repo, IndexEntry &e) {
    FileStat st;
    if (LStat(JoinPath(repo.WorkTree(), e.path), st)) {
        FillStat(e, st);
        if ((e.mode & 0170000) == 0160000) e.size = 0;
    }
}

IndexEntry EntryFromTree(const TreeEntry &t, const std::string &path) {
    IndexEntry e;
    e.path = path;
    e.mode = t.mode;
    e.oid = t.oid;
    return e;
}

// unpack_trees' two-way merge, as git checkout / merge --ff use it: moves the
// index and worktree from `old_tree` to `new_tree`, keeping local changes
// that don't collide, refusing (with git's messages) when they would be lost.
bool SwitchTrees(Repo &repo, Index &idx, const TreeMap &old_tree, const TreeMap &new_tree, const char *op, Out &o) {
    if (idx.HasConflicts()) {
        o.err += "error: you need to resolve your current index first\n";
        for (const IndexEntry &e : idx.entries)
            if (e.Stage() == 2 || (e.Stage() == 1 && idx.Find(e.path, 2) < 0 && idx.Find(e.path, 3) < 0) ||
                (e.Stage() == 3 && idx.Find(e.path, 2) < 0))
                o.err += e.path + ": needs merge\n";
        return false;
    }
    std::set<std::string> paths;
    for (const IndexEntry &e : idx.entries) paths.insert(e.path);
    for (const auto &kv : old_tree) paths.insert(kv.first);
    for (const auto &kv : new_tree) paths.insert(kv.first);

    struct Action {
        enum Kind { Update, Delete } kind;
        std::string path;
        TreeEntry entry;
    };
    std::vector<Action> actions;
    std::vector<std::string> dirty, untracked;
    IgnoreMatcher ign(repo);
    for (const std::string &p : paths) {
        long ci = idx.Find(p);
        const IndexEntry *cur = ci >= 0 ? &idx.entries[static_cast<size_t>(ci)] : nullptr;
        auto oi = old_tree.find(p);
        auto ni = new_tree.find(p);
        const TreeEntry *oldt = oi == old_tree.end() ? nullptr : &oi->second;
        const TreeEntry *newt = ni == new_tree.end() ? nullptr : &ni->second;
        TreeEntry curt;
        if (cur) curt = TreeEntry{cur->mode, p, cur->oid};
        const TreeEntry *curp = cur ? &curt : nullptr;
        if (cur && cur->SkipWorktree()) continue;
        if (cur) {
            if ((!oldt && !newt) || (!oldt && newt && Same(curp, newt)) || (oldt && newt && Same(oldt, newt)) ||
                (oldt && newt && !Same(oldt, newt) && Same(curp, newt))) {
                continue;  // keep
            } else if (oldt && !newt && Same(curp, oldt)) {
                if (!WorktreeUpToDate(repo, idx, *cur)) dirty.push_back(p);
                else actions.push_back({Action::Delete, p, {}});
            } else if (oldt && newt && Same(curp, oldt) && !Same(curp, newt)) {
                if (!WorktreeUpToDate(repo, idx, *cur)) dirty.push_back(p);
                else actions.push_back({Action::Update, p, *newt});
            } else {
                dirty.push_back(p);
            }
        } else if (newt) {
            if (oldt) {
                if (Same(oldt, newt)) continue;
                dirty.push_back(p);
                continue;
            }
            // New path: must not clobber an untracked (non-ignored) file.
            FileStat st;
            std::string abs = JoinPath(repo.WorkTree(), p);
            if (LStat(abs, st) && !st.is_dir && !ign.IsIgnored(p, false)) {
                std::string data;
                Oid h;
                if (!HashWorktreeFile(abs, st, h) || h != newt->oid) {
                    untracked.push_back(p);
                    continue;
                }
            }
            actions.push_back({Action::Update, p, *newt});
        }
        // else: index lacks it and so does new: nothing to do.
    }
    // Directory/file conflicts: a file about to be written where the
    // worktree has a directory, or under a path that is a file, may only
    // replace what this same switch deletes -- never untracked work.
    {
        std::set<std::string> deleting;
        for (const Action &a : actions)
            if (a.kind == Action::Delete) deleting.insert(a.path);
        std::function<bool(const std::string &)> only_deletions = [&](const std::string &rel) -> bool {
            std::vector<DirEntry> ents;
            ListDir(JoinPath(repo.WorkTree(), rel), ents);
            for (const DirEntry &e : ents) {
                std::string c = rel + "/" + e.name;
                if (e.kind == DirEntry::Dir) {
                    if (!only_deletions(c)) return false;
                } else if (!deleting.count(c)) {
                    return false;
                }
            }
            return true;
        };
        for (const Action &a : actions) {
            if (a.kind != Action::Update) continue;
            FileStat st;
            if ((a.entry.mode & 0170000) != 0160000 && LStat(JoinPath(repo.WorkTree(), a.path), st) && st.is_dir &&
                !only_deletions(a.path)) {
                untracked.push_back(a.path);
                continue;
            }
            size_t slash = 0;
            while ((slash = a.path.find('/', slash)) != std::string::npos) {
                std::string parent = a.path.substr(0, slash);
                slash++;
                FileStat ps;
                if (LStat(JoinPath(repo.WorkTree(), parent), ps) && !ps.is_dir && !deleting.count(parent)) {
                    untracked.push_back(parent);
                    break;
                }
            }
        }
        std::sort(untracked.begin(), untracked.end());
        untracked.erase(std::unique(untracked.begin(), untracked.end()), untracked.end());
    }
    if (!dirty.empty() || !untracked.empty()) {
        std::string what = std::string(op);
        if (!dirty.empty()) {
            o.err += "error: Your local changes to the following files would be overwritten by " + what + ":\n";
            for (const std::string &p : dirty) o.err += "\t" + p + "\n";
            o.err += std::string(what == "checkout" ? "Please commit your changes or stash them before you switch branches.\n"
                                                    : "Please commit your changes or stash them before you merge.\n");
        }
        if (!untracked.empty()) {
            o.err += "error: The following untracked working tree files would be overwritten by " + what + ":\n";
            for (const std::string &p : untracked) o.err += "\t" + p + "\n";
            o.err += std::string(what == "checkout" ? "Please move or remove them before you switch branches.\n"
                                                    : "Please move or remove them before you merge.\n");
        }
        o.err += "Aborting\n";
        return false;
    }
    // Deletions first (a file may become a directory), then updates.
    for (const Action &a : actions) {
        if (a.kind != Action::Delete) continue;
        RemoveFromWorktree(repo, a.path);
        idx.Remove(a.path);
    }
    for (const Action &a : actions) {
        if (a.kind != Action::Update) continue;
        std::string werr;
        if (!WriteToWorktree(repo, a.path, a.entry.mode, a.entry.oid, &werr)) {
            o.err += werr + "\n";
            return false;
        }
        IndexEntry e = EntryFromTree(a.entry, a.path);
        RefreshEntryStat(repo, e);
        idx.Put(e);
    }
    return true;
}

// ------------------------------------------------------------ hooks

int RunHook(Repo &repo, const std::string &name, const std::vector<std::string> &args, Out &o) {
    std::string dir;
    if (auto hp = repo.Cfg().Get("core.hookspath")) {
        dir = *hp;
        if (StartsWith(dir, "~/")) {
            const char *home = std::getenv("HOME");
            if (home) dir = std::string(home) + dir.substr(1);
        }
        dir = AbsolutePath(dir, repo.IsBare() ? repo.GitDir() : repo.WorkTree());
    } else {
        dir = repo.CommonDir() + "/hooks";
    }
    std::string hook = dir + "/" + name;
#if !defined(_WIN32)
    if (::access(hook.c_str(), X_OK) != 0 || IsDirectory(hook)) return 0;
#else
    if (!FileExists(hook)) return 0;
#endif
    std::vector<std::string> argv = {hook};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<std::pair<std::string, std::string>> env = {{"GIT_INDEX_FILE", repo.IndexFile()},
                                                            {"GIT_EDITOR", ":"}};
    std::string out, err;
    std::string cwd = repo.IsBare() ? repo.GitDir() : repo.WorkTree();
    int code = RunProcess(argv, cwd, env, &out, &err);
    if (code == -1) {
        // No shebang (ENOEXEC): run it through the shell, as git does.
        std::vector<std::string> sh = {"/bin/sh", hook};
        sh.insert(sh.end(), args.begin(), args.end());
        code = RunProcess(sh, cwd, env, &out, &err);
    }
    o.err += out + err;
    return code;
}

std::string CleanupMessage(const std::string &msg, const std::string &mode, char comment) {
    if (mode == "verbatim") return msg;
    bool skip_comments = mode == "strip";
    std::string out;
    int empties = 0;
    size_t pos = 0;
    while (pos < msg.size()) {
        size_t nl = msg.find('\n', pos);
        std::string line = msg.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? msg.size() : nl + 1;
        if (skip_comments && !line.empty() && line[0] == comment) continue;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (!line.empty()) {
            if (empties > 0 && !out.empty()) out += '\n';
            empties = 0;
            out += line + "\n";
        } else {
            empties++;
        }
    }
    return out;
}

std::string StatSummary(Repo &repo, const Oid &from_tree, const Oid &to_tree) {
    std::vector<DiffPair> pairs;
    DiffTrees(repo, from_tree, to_tree, {}, pairs);
    if (repo.Cfg().GetBool("diff.renames", true)) DetectRenames(repo, pairs);
    DiffOptions dopt;
    std::string stat = FormatStat(repo, pairs, dopt);
    // last line is the summary
    std::string summary;
    if (!stat.empty()) {
        std::string s = stat.substr(0, stat.size() - 1);
        size_t nl = s.rfind('\n');
        summary = (nl == std::string::npos ? s : s.substr(nl + 1)) + "\n";
    }
    auto mode6 = [](uint32_t m) {
        char b[16];
        std::snprintf(b, sizeof b, "%06o", m);
        return std::string(b);
    };
    for (const DiffPair &p : pairs) {
        if (p.status == 'A') summary += " create mode " + mode6(p.b.mode) + " " + p.b.path + "\n";
        else if (p.status == 'D') summary += " delete mode " + mode6(p.a.mode) + " " + p.a.path + "\n";
        else if (p.status == 'R')
            summary += " rename " + p.a.path + " => " + p.b.path + " (" + std::to_string(p.score) + "%)\n";
        else if (p.a.mode != p.b.mode) summary += " mode change " + mode6(p.a.mode) + " => " + mode6(p.b.mode) + " " + p.b.path + "\n";
    }
    return summary;
}

}  // namespace

// ------------------------------------------------------------ status text

std::string LongStatus(Repo &repo, const StatusResult &st) {
    std::string out;
    const BranchInfo &b = st.branch;
    if (b.detached) {
        out += "HEAD detached at " + repo.Abbrev(b.head) + "\n";
    } else {
        out += "On branch " + b.branch + "\n";
        if (b.unborn) {
            out += "\nNo commits yet\n\n";
        } else if (b.has_upstream) {
            if (b.gone) {
                out += "Your branch is based on '" + b.upstream + "', but the upstream is gone.\n";
                out += "  (use \"git branch --unset-upstream\" to fixup)\n";
            } else if (!b.ahead && !b.behind) {
                out += "Your branch is up to date with '" + b.upstream + "'.\n";
            } else if (b.ahead && !b.behind) {
                out += "Your branch is ahead of '" + b.upstream + "' by " + std::to_string(b.ahead) +
                       (b.ahead == 1 ? " commit.\n" : " commits.\n");
                out += "  (use \"git push\" to publish your local commits)\n";
            } else if (b.behind && !b.ahead) {
                out += "Your branch is behind '" + b.upstream + "' by " + std::to_string(b.behind) +
                       (b.behind == 1 ? " commit, and can be fast-forwarded.\n" : " commits, and can be fast-forwarded.\n");
                out += "  (use \"git pull\" to update your local branch)\n";
            } else {
                out += "Your branch and '" + b.upstream + "' have diverged,\nand have " + std::to_string(b.ahead) +
                       " and " + std::to_string(b.behind) + " different commits each, respectively.\n";
                out += "  (use \"git pull\" if you want to integrate the remote branch with yours)\n";
            }
            out += "\n";
        }
    }
    auto label = [](char c) -> std::string {
        switch (c) {
            case 'A': return "new file:   ";
            case 'M': return "modified:   ";
            case 'D': return "deleted:    ";
            case 'R': return "renamed:    ";
            case 'T': return "typechange: ";
            case 'C': return "copied:     ";
            default: return "unknown:    ";
        }
    };
    std::vector<const StatusEntry *> staged, unstaged, unmerged;
    for (const StatusEntry &e : st.changes) {
        bool um = (e.x == 'U' || e.y == 'U' || (e.x == 'A' && e.y == 'A') || (e.x == 'D' && e.y == 'D'));
        if (um) {
            unmerged.push_back(&e);
            continue;
        }
        if (e.x != ' ' && e.x != '?') staged.push_back(&e);
        if (e.y != ' ' && e.y != '?') unstaged.push_back(&e);
    }
    if (FileExists(repo.GitDir() + "/MERGE_HEAD")) {
        if (!unmerged.empty())
            out += "You have unmerged paths.\n  (fix conflicts and run \"git commit\")\n"
                   "  (use \"git merge --abort\" to abort the merge)\n\n";
        else
            out += "All conflicts fixed but you are still merging.\n  (use \"git commit\" to conclude merge)\n\n";
    }
    auto q = [](const std::string &p) { return QuotePath(p); };
    if (!staged.empty()) {
        out += "Changes to be committed:\n";
        out += b.unborn ? "  (use \"git rm --cached <file>...\" to unstage)\n"
                        : "  (use \"git restore --staged <file>...\" to unstage)\n";
        for (const StatusEntry *e : staged) {
            out += "\t" + label(e->x);
            if (e->x == 'R') out += q(e->orig_path) + " -> ";
            out += q(e->path) + "\n";
        }
        out += "\n";
    }
    if (!unmerged.empty()) {
        bool del = false;
        for (const StatusEntry *e : unmerged)
            if (e->x == 'D' || e->y == 'D') del = true;
        out += del ? "Unmerged paths:\n  (use \"git add/rm <file>...\" as appropriate to mark resolution)\n"
                   : "Unmerged paths:\n  (use \"git add <file>...\" to mark resolution)\n";
        for (const StatusEntry *e : unmerged) {
            std::string xy{e->x, e->y};
            std::string how = xy == "DD" ? "both deleted:    " : xy == "AU" ? "added by us:     " : xy == "UD" ? "deleted by them: "
                            : xy == "UA" ? "added by them:   " : xy == "DU" ? "deleted by us:   " : xy == "AA" ? "both added:      "
                                                                                                     : "both modified:   ";
            out += "\t" + how + q(e->path) + "\n";
        }
        out += "\n";
    }
    if (!unstaged.empty()) {
        bool has_del = false, dirty_sub = false;
        for (const StatusEntry *e : unstaged) {
            if (e->y == 'D') has_del = true;
            if (e->submodule && e->sub_state.size() == 4 && (e->sub_state[2] == 'M' || e->sub_state[3] == 'U'))
                dirty_sub = true;
        }
        out += "Changes not staged for commit:\n";
        out += has_del ? "  (use \"git add/rm <file>...\" to update what will be committed)\n"
                       : "  (use \"git add <file>...\" to update what will be committed)\n";
        out += "  (use \"git restore <file>...\" to discard changes in working directory)\n";
        if (dirty_sub) out += "  (commit or discard the untracked or modified content in submodules)\n";
        for (const StatusEntry *e : unstaged) {
            out += "\t" + label(e->y) + q(e->path);
            if (e->submodule && e->sub_state.size() == 4) {
                std::vector<std::string> what;
                if (e->sub_state[1] == 'C') what.push_back("new commits");
                if (e->sub_state[2] == 'M') what.push_back("modified content");
                if (e->sub_state[3] == 'U') what.push_back("untracked content");
                if (!what.empty()) {
                    out += " (";
                    for (size_t k = 0; k < what.size(); k++) out += (k ? ", " : "") + what[k];
                    out += ")";
                }
            }
            out += "\n";
        }
        out += "\n";
    }
    if (!st.untracked.empty()) {
        out += "Untracked files:\n  (use \"git add <file>...\" to include in what will be committed)\n";
        for (const std::string &u : st.untracked) out += "\t" + q(u) + "\n";
        out += "\n";
    }
    if (staged.empty()) {
        if (!unstaged.empty() || !unmerged.empty()) out += "no changes added to commit (use \"git add\" and/or \"git commit -a\")\n";
        else if (!st.untracked.empty())
            out += "nothing added to commit but untracked files present (use \"git add\" to track)\n";
        else if (b.unborn) out += "nothing to commit (create/copy files and use \"git add\" to track)\n";
        else out += "nothing to commit, working tree clean\n";
    }
    return out;
}

// ------------------------------------------------------------ add / rm / reset

int OpAdd(Repo &repo, bool all, const std::vector<std::string> &paths_in, bool force, Out &o) {
    if (repo.IsBare()) {
        o.err += "fatal: this operation must be run in a work tree\n";
        return 128;
    }
    std::vector<std::string> paths;
    for (const std::string &p : paths_in) {
        std::string n = NormalizePath(repo.Prefix() + p);
        paths.push_back(n == "." ? "" : n);
    }
    if (paths.empty() && !all) {
        o.err += "Nothing specified, nothing added.\n";
        return 0;
    }
    std::vector<std::string> spec = paths;
    if (all && paths.empty()) spec.clear();
    Index idx;
    std::string err;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    IgnoreMatcher ign(repo);
    // Explicitly named paths: must exist (on disk or in the index); ignored ones need -f.
    std::vector<std::string> ignored_named;
    for (const std::string &p : paths) {
        if (p.empty()) continue;
        std::string abs = JoinPath(repo.WorkTree(), p);
        FileStat st;
        bool on_disk = LStat(abs, st);
        bool in_index = false;
        size_t i = idx.LowerBound(p);
        if (i < idx.entries.size() && (idx.entries[i].path == p || StartsWith(idx.entries[i].path, p + "/"))) in_index = true;
        if (!on_disk && !in_index) {
            o.err += "fatal: pathspec '" + p + "' did not match any files\n";
            return 128;
        }
        if (on_disk && !in_index && !force && ign.IsIgnored(p, st.is_dir)) ignored_named.push_back(p);
    }
    if (!ignored_named.empty()) {
        o.err += "The following paths are ignored by one of your .gitignore files:\n";
        for (const std::string &p : ignored_named) o.err += p + "\n";
        o.err += "hint: Use -f if you really want to add them.\n"
                 "hint: Disable this message with \"git config set advice.addIgnoredFile false\"\n";
        return 1;
    }
    // Tracked: update modified, drop deleted, resolve conflicts.
    std::vector<IndexEntry> updated;
    std::vector<std::string> removed;
    std::set<std::string> conflicted;
    for (const IndexEntry &e : idx.entries) {
        if (!PathspecMatch(spec, e.path)) continue;
        if (e.SkipWorktree() || e.AssumeValid()) continue;
        if (e.Stage()) {
            conflicted.insert(e.path);
            continue;
        }
        std::string abs = JoinPath(repo.WorkTree(), e.path);
        FileStat st;
        if (!LStat(abs, st) || (st.is_dir && (e.mode & 0170000) != 0160000)) {
            removed.push_back(e.path);
            continue;
        }
        bool racy = !idx.loaded_from_file || e.mtime_s >= static_cast<uint32_t>(idx.file_mtime_s);
        if ((e.mode & 0170000) != 0160000 && StatMatches(e, st, repo.Cfg()) && !racy &&
            ModeFromStat(st, repo.Cfg(), e.mode) == e.mode && !e.IntentToAdd())
            continue;
        IndexEntry ne;
        if (!EntryFromWorktree(repo, e.path, &e, ne, true, &err)) {
            o.err += err + "\n";
            return 128;
        }
        updated.push_back(ne);
    }
    for (const std::string &p : conflicted) {
        IndexEntry ne;
        std::string abs = JoinPath(repo.WorkTree(), p);
        FileStat st;
        if (!LStat(abs, st)) {
            removed.push_back(p);
            continue;
        }
        if (!EntryFromWorktree(repo, p, nullptr, ne, true, &err)) {
            o.err += err + "\n";
            return 128;
        }
        updated.push_back(ne);
    }
    for (const std::string &p : removed) idx.Remove(p);
    for (IndexEntry &e : updated) idx.Put(e);
    // Untracked files.
    std::vector<std::string> untracked;
    ListUntracked(repo, idx, true, false, spec, untracked, nullptr);
    if (force) {
        // -f also adds ignored files under the named paths.
        std::vector<std::string> ign_list, dummy;
        ListUntracked(repo, idx, true, true, spec, dummy, &ign_list);
        for (const std::string &p : ign_list) untracked.push_back(p);
    }
    for (std::string p : untracked) {
        bool dir = !p.empty() && p.back() == '/';
        if (dir) p.pop_back();
        if (dir && force) {
            // An ignored directory named with -f: add everything inside.
            std::vector<std::string> stack = {p};
            while (!stack.empty()) {
                std::string d = stack.back();
                stack.pop_back();
                std::vector<DirEntry> ents;
                ListDir(JoinPath(repo.WorkTree(), d), ents);
                for (const DirEntry &de : ents) {
                    std::string rel = d + "/" + de.name;
                    if (de.kind == DirEntry::Dir) {
                        if (de.name != ".git") stack.push_back(rel);
                        continue;
                    }
                    IndexEntry ne;
                    if (EntryFromWorktree(repo, rel, nullptr, ne, true, &err)) idx.Put(ne);
                }
            }
            continue;
        }
        IndexEntry ne;
        if (!EntryFromWorktree(repo, p, nullptr, ne, true, &err)) {
            o.err += err + "\n";
            return 128;
        }
        if (dir) {
            o.err += "warning: adding embedded git repository: " + p + "\n";
        }
        idx.Put(ne);
    }
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    return 0;
}

int OpRm(Repo &repo, bool cached, bool recursive, bool force, const std::vector<std::string> &paths_in, Out &o) {
    Index idx;
    std::string err;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    std::vector<std::string> targets;
    for (const std::string &p0 : paths_in) {
        std::string p = NormalizePath(repo.Prefix() + p0);
        bool matched = false;
        for (const IndexEntry &e : idx.entries) {
            if (e.path == p) {
                matched = true;
                targets.push_back(e.path);
            } else if (StartsWith(e.path, p + "/")) {
                if (!recursive) {
                    o.err += "fatal: not removing '" + p + "' recursively without -r\n";
                    return 128;
                }
                matched = true;
                targets.push_back(e.path);
            }
        }
        if (!matched) {
            o.err += "fatal: pathspec '" + p + "' did not match any files\n";
            return 128;
        }
    }
    if (!force && !cached) {
        for (const std::string &p : targets) {
            long i = idx.Find(p);
            if (i < 0) continue;
            if (!WorktreeUpToDate(repo, idx, idx.entries[static_cast<size_t>(i)])) {
                o.err += "error: the following file has local modifications:\n    " + p +
                         "\n(use --cached to keep the file, or -f to force removal)\n";
                return 1;
            }
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const std::string &p : targets) {
        idx.Remove(p);
        o.out += "rm '" + p + "'\n";
        if (!cached) RemoveFromWorktree(repo, p);
    }
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    return 0;
}

int OpReset(Repo &repo, const std::string &rev, const std::vector<std::string> &paths_in, bool hard, Out &o) {
    Index idx;
    std::string err;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    Oid target;
    bool have_target = true;
    if (!repo.RevParse(rev.empty() ? "HEAD" : rev, target, &err)) {
        if (rev.empty() || rev == "HEAD") {
            have_target = false;  // unborn: reset to the empty tree
        } else {
            o.err += err + "\n";
            return 128;
        }
    }
    TreeMap tree;
    if (have_target && !CommitTreeMap(repo, target, tree)) {
        o.err += "fatal: Could not parse object '" + rev + "'.\n";
        return 128;
    }
    std::vector<std::string> paths;
    for (const std::string &p : paths_in) {
        std::string n = NormalizePath(repo.Prefix() + p);
        paths.push_back(n == "." ? "" : n);
    }
    if (paths.empty()) {
        Index old = idx;
        std::vector<IndexEntry> fresh;
        for (const auto &[path, t] : tree) {
            IndexEntry e = EntryFromTree(t, path);
            long i = old.Find(path);
            if (i >= 0 && old.entries[static_cast<size_t>(i)].oid == t.oid && old.entries[static_cast<size_t>(i)].mode == t.mode) {
                e = old.entries[static_cast<size_t>(i)];  // keep the stat data
                e.flags &= static_cast<uint16_t>(~0x3000);
            }
            fresh.push_back(e);
        }
        if (hard) {
            TreeMap old_tree;
            Oid head;
            if (HeadCommit(repo, head)) CommitTreeMap(repo, head, old_tree);
            for (const IndexEntry &e : old.entries)
                if (!tree.count(e.path)) RemoveFromWorktree(repo, e.path);
            for (IndexEntry &e : fresh) {
                long i = old.Find(e.path);
                bool clean = i >= 0 && old.entries[static_cast<size_t>(i)].oid == e.oid &&
                             WorktreeUpToDate(repo, old, old.entries[static_cast<size_t>(i)]) &&
                             FileExists(JoinPath(repo.WorkTree(), e.path));
                if (!clean) {
                    if (!WriteToWorktree(repo, e.path, e.mode, e.oid, &err)) {
                        o.err += err + "\n";
                        return 128;
                    }
                    RefreshEntryStat(repo, e);
                }
            }
        }
        idx.entries = std::move(fresh);
        if (!idx.Save(repo.IndexFile(), &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
        if (hard) {
            if (have_target) o.out += "HEAD is now at " + OnelineOf(repo, target) + "\n";
        } else {
            // "Unstaged changes after reset:" listing, like git.
            std::vector<DiffPair> wt;
            DiffIndexWorktree(repo, idx, {}, wt);
            if (!wt.empty()) {
                o.out += "Unstaged changes after reset:\n";
                for (const DiffPair &p : wt) o.out += std::string(1, p.status) + "\t" + p.a.path + "\n";
            }
        }
        return 0;
    }
    // Path-limited: index entries under the paths follow the target tree.
    std::vector<std::string> drop;
    for (const IndexEntry &e : idx.entries)
        if (PathspecMatch(paths, e.path) && !tree.count(e.path)) drop.push_back(e.path);
    for (const std::string &p : drop) idx.Remove(p);
    for (const auto &[path, t] : tree) {
        if (!PathspecMatch(paths, path)) continue;
        long i = idx.Find(path);
        if (i >= 0 && idx.entries[static_cast<size_t>(i)].oid == t.oid && idx.entries[static_cast<size_t>(i)].mode == t.mode)
            continue;
        IndexEntry e = EntryFromTree(t, path);
        idx.Put(e);
    }
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    std::vector<DiffPair> wt;
    DiffIndexWorktree(repo, idx, {}, wt);
    if (!wt.empty()) {
        o.out += "Unstaged changes after reset:\n";
        for (const DiffPair &p : wt) o.out += std::string(1, p.status) + "\t" + p.a.path + "\n";
    }
    return 0;
}

// ------------------------------------------------------------ commit

int OpCommit(Repo &repo, const CommitOptions &co, Out &o) {
    std::string err;
    if (co.all) {
        Out tmp;
        // -a: stage modifications and deletions of tracked files only.
        Index idx;
        idx.Load(repo.IndexFile(), nullptr);
        std::vector<DiffPair> wt;
        DiffIndexWorktree(repo, idx, {}, wt);
        for (const DiffPair &p : wt) {
            if (p.status == 'D') {
                idx.Remove(p.a.path);
            } else {
                long i = idx.Find(p.a.path);
                IndexEntry ne;
                if (EntryFromWorktree(repo, p.a.path, i >= 0 ? &idx.entries[static_cast<size_t>(i)] : nullptr, ne, true, &err))
                    idx.Put(ne);
            }
        }
        if (!idx.Save(repo.IndexFile(), &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
    }
    Index idx;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    if (idx.HasConflicts()) {
        o.err += "error: Committing is not possible because you have unmerged files.\n"
                 "hint: Fix them up in the work tree, and then use 'git add/rm <file>'\n"
                 "hint: as appropriate to mark resolution and make a commit.\n"
                 "fatal: Exiting because of an unresolved conflict.\n";
        return 128;
    }
    Signature author, committer;
    if (!repo.Ident(false, author, &err) || !repo.Ident(true, committer, &err)) {
        o.err += err + "\n";
        return 128;
    }
    Oid head;
    bool has_head = HeadCommit(repo, head);
    Commit head_commit;
    if (has_head && !repo.ReadCommit(head, head_commit)) {
        o.err += "fatal: could not parse HEAD commit\n";
        return 128;
    }
    if (co.amend && !has_head) {
        o.err += "fatal: You have nothing to amend.\n";
        return 128;
    }
    Oid tree;
    if (!WriteTreeFromIndex(repo, idx, tree, &err)) {
        o.err += err + "\nfatal: Error building trees\n";
        return 128;
    }
    if (!co.amend && !co.allow_empty) {
        Oid head_tree = has_head ? head_commit.tree : HashObject(ObjType::Tree, "");
        if (tree == head_tree) {
            StatusOptions so;
            StatusResult sr;
            Status(repo, so, sr, &err);
            o.out += LongStatus(repo, sr);
            return 1;
        }
    }
    if (!co.no_verify) {
        if (RunHook(repo, "pre-commit", {}, o) != 0) return 1;
        // The hook may have changed the index.
        if (!idx.Load(repo.IndexFile(), &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
        if (!WriteTreeFromIndex(repo, idx, tree, &err)) {
            o.err += err + "\nfatal: Error building trees\n";
            return 128;
        }
    }
    std::string msg;
    if (co.have_message) {
        msg = co.message;
        if (!msg.empty() && msg.back() != '\n') msg += "\n";
    } else if (!co.message_file.empty()) {
        std::string path = co.message_file == "-" ? "" : AbsolutePath(co.message_file, JoinPath(repo.WorkTree(), repo.Prefix()));
        if (path.empty() || !ReadFile(path, msg)) {
            o.err += "fatal: could not read log file '" + co.message_file + "'\n";
            return 128;
        }
    } else {
        o.err += "error: no commit message given (mep's in-house commit needs -m or -F)\n";
        return 1;
    }
    std::string editmsg = repo.GitDir() + "/COMMIT_EDITMSG";
    if (AbsolutePath(co.message_file, JoinPath(repo.WorkTree(), repo.Prefix())) != editmsg || co.have_message)
        WriteFile(editmsg, co.cleanup == "verbatim" ? msg : CleanupMessage(msg, "whitespace", '#'));
    else if (co.cleanup != "verbatim")
        WriteFile(editmsg, CleanupMessage(msg, "whitespace", '#'));
    if (RunHook(repo, "prepare-commit-msg", {editmsg, "message"}, o) != 0) return 1;
    if (!co.no_verify && RunHook(repo, "commit-msg", {editmsg}, o) != 0) return 1;
    ReadFile(editmsg, msg);
    std::string cleanup = co.cleanup;
    if (cleanup == "default") cleanup = "whitespace";
    char comment = '#';
    if (auto cc = repo.Cfg().Get("core.commentchar"); cc && cc->size() == 1) comment = (*cc)[0];
    msg = CleanupMessage(msg, cleanup, comment);
    if (Trim(msg).empty()) {
        o.err += "Aborting commit due to empty commit message.\n";
        return 1;
    }
    Commit c;
    c.tree = tree;
    if (co.amend) {
        c.parents = head_commit.parents;
        c.author = head_commit.author;
    } else {
        if (has_head) c.parents.push_back(head);
        c.author = author;
    }
    c.committer = committer;
    c.message = msg;
    Oid new_oid;
    if (!repo.Write(ObjType::Commit, SerializeCommit(c), new_oid, &err)) {
        o.err += "error: " + err + "\n";
        return 128;
    }
    std::string subject = CommitSubject(msg);
    std::string reflog = co.amend ? "commit (amend): " + subject
                                  : (has_head ? "commit: " + subject : "commit (initial): " + subject);
    Oid expected = has_head ? head : Oid{};
    if (!repo.UpdateRef("HEAD", new_oid, reflog, &err, &expected)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    RunHook(repo, "post-commit", {}, o);
    std::string branch = BranchShort(repo);
    if (branch.empty()) branch = "detached HEAD";
    o.out += "[" + branch + (c.parents.empty() ? " (root-commit)" : "") + " " + repo.Abbrev(new_oid) + "] " + subject + "\n";
    Oid parent_tree;
    if (!c.parents.empty()) {
        Commit pc;
        if (repo.ReadCommit(c.parents[0], pc)) parent_tree = pc.tree;
    }
    o.out += StatSummary(repo, parent_tree, tree);
    return 0;
}

// ------------------------------------------------------------ branches

namespace {
bool ValidBranchName(const std::string &n) {
    if (n.empty() || n[0] == '-' || n == "HEAD" || n.find("..") != std::string::npos || EndsWith(n, ".lock") ||
        EndsWith(n, "/") || EndsWith(n, ".") || n.find("@{") != std::string::npos || n.find("//") != std::string::npos)
        return false;
    for (char ch : n) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' ||
            c == '\\')
            return false;
    }
    if (n[0] == '/' || n.find("/.") != std::string::npos || n[0] == '.') return false;
    return true;
}

// Every worktree's checked-out branch: (branch ref, worktree path).
std::vector<std::pair<std::string, std::string>> CheckedOutBranches(Repo &repo) {
    std::vector<std::pair<std::string, std::string>> out;
    auto head_of = [](const std::string &file) {
        std::string t;
        if (!ReadFile(file, t)) return std::string();
        t = Trim(t);
        return StartsWith(t, "ref: ") ? t.substr(5) : std::string();
    };
    std::string main_ref = head_of(repo.CommonDir() + "/HEAD");
    if (!main_ref.empty()) out.push_back({main_ref, repo.MainWorkTree()});
    std::vector<DirEntry> ents;
    ListDir(repo.CommonDir() + "/worktrees", ents);
    for (const DirEntry &e : ents) {
        std::string admin = repo.CommonDir() + "/worktrees/" + e.name;
        std::string r = head_of(admin + "/HEAD");
        std::string gd;
        ReadFile(admin + "/gitdir", gd);
        if (!r.empty()) out.push_back({r, DirName(Trim(gd))});
    }
    return out;
}
}  // namespace

int OpBranchCreate(Repo &repo, const std::string &name, const std::string &start, bool force, Out &o) {
    if (!ValidBranchName(name)) {
        o.err += "fatal: '" + name + "' is not a valid branch name\n";
        return 128;
    }
    std::string ref = "refs/heads/" + name;
    if (repo.RefExists(ref) && !force) {
        o.err += "fatal: a branch named '" + name + "' already exists\n";
        return 128;
    }
    Oid at, commit;
    std::string err;
    std::string from = start.empty() ? "HEAD" : start;
    if (!repo.RevParse(from, at, &err) || !repo.PeelToCommit(at, commit)) {
        o.err += "fatal: not a valid object name: '" + from + "'\n";
        return 128;
    }
    if (!repo.UpdateRef(ref, commit, "branch: Created from " + from, &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    return 0;
}

int OpBranchDelete(Repo &repo, const std::vector<std::string> &names, bool force, Out &o) {
    (void)force;  // -D: no merged check
    int code = 0;
    for (const std::string &name : names) {
        std::string ref = "refs/heads/" + name;
        Oid oid;
        if (!repo.Resolve(ref, oid)) {
            o.err += "error: branch '" + name + "' not found\n";
            code = 1;
            continue;
        }
        bool used = false;
        for (const auto &[r, path] : CheckedOutBranches(repo)) {
            if (r == ref) {
                o.err += "error: cannot delete branch '" + name + "' used by worktree at '" + path + "'\n";
                used = true;
                break;
            }
        }
        if (used) {
            code = 1;
            continue;
        }
        std::string err;
        if (!repo.DeleteRef(ref, &err)) {
            o.err += "error: " + err + "\n";
            code = 1;
            continue;
        }
        ConfigRemoveSection(repo.CommonDir() + "/config", "branch", name);
        o.out += "Deleted branch " + name + " (was " + repo.Abbrev(oid) + ").\n";
    }
    return code;
}

// ------------------------------------------------------------ checkout

namespace {
int SwitchTo(Repo &repo, const Oid &commit, const std::string &new_head_ref, const std::string &reflog_target,
             const char *op, Out &o) {
    Index idx;
    std::string err;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    Oid head;
    TreeMap old_tree, new_tree;
    bool has_head = HeadCommit(repo, head);
    if (has_head) CommitTreeMap(repo, head, old_tree);
    if (!CommitTreeMap(repo, commit, new_tree)) {
        o.err += "fatal: reference is not a tree\n";
        return 128;
    }
    if (!SwitchTrees(repo, idx, old_tree, new_tree, op, o)) return 1;
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    std::string old_desc = BranchShort(repo);
    if (old_desc.empty()) old_desc = has_head ? head.Hex() : "(invalid)";
    std::string msg = "checkout: moving from " + old_desc + " to " + reflog_target;
    if (!new_head_ref.empty()) {
        if (!repo.SetSymbolicRef("HEAD", new_head_ref, msg, &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
    } else {
        // Detach: HEAD holds the commit itself.
        std::string head_file = repo.RefFile("HEAD");
        if (!WriteFileAtomic(head_file, commit.Hex() + "\n", &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
        repo.AppendReflog("HEAD", has_head ? head : Oid{}, commit, msg);
    }
    return 0;
}
}  // namespace

int OpCheckout(Repo &repo, const std::string &target, bool detach, Out &o) {
    std::string err;
    std::string ref = "refs/heads/" + target;
    Oid oid;
    if (!detach && repo.Resolve(ref, oid)) {
        if (repo.HeadRef() == ref) {
            o.err += "Already on '" + target + "'\n";
            return 0;
        }
        for (const auto &[r, path] : CheckedOutBranches(repo)) {
            if (r == ref && RealPath(path) != repo.WorkTree()) {
                o.err += "fatal: '" + target + "' is already used by worktree at '" + path + "'\n";
                return 128;
            }
        }
        int code = SwitchTo(repo, oid, ref, target, "checkout", o);
        if (code == 0) {
            o.err += "Switched to branch '" + target + "'\n";
            BranchInfo bi;
            GetBranchInfo(repo, bi);
            if (bi.has_upstream && !bi.gone) {
                if (!bi.ahead && !bi.behind) o.out += "Your branch is up to date with '" + bi.upstream + "'.\n";
                else if (bi.ahead && !bi.behind)
                    o.out += "Your branch is ahead of '" + bi.upstream + "' by " + std::to_string(bi.ahead) +
                             (bi.ahead == 1 ? " commit.\n" : " commits.\n") + "  (use \"git push\" to publish your local commits)\n";
                else if (bi.behind && !bi.ahead)
                    o.out += "Your branch is behind '" + bi.upstream + "' by " + std::to_string(bi.behind) +
                             (bi.behind == 1 ? " commit" : " commits") + ", and can be fast-forwarded.\n  (use \"git pull\" to update your local branch)\n";
            }
        }
        return code;
    }
    // DWIM: a remote-tracking branch of that name in exactly one remote.
    if (!detach && target.find('/') == std::string::npos) {
        std::vector<std::string> matches;
        for (const RefEntry &r : repo.ListRefs("refs/remotes/")) {
            std::string rest = r.name.substr(13);
            size_t slash = rest.find('/');
            if (slash != std::string::npos && rest.substr(slash + 1) == target && r.symref.empty()) matches.push_back(r.name);
        }
        if (matches.size() == 1) {
            Oid at;
            repo.Resolve(matches[0], at);
            std::string remote_short = matches[0].substr(13);
            std::string remote = remote_short.substr(0, remote_short.find('/'));
            // Create the tracking branch, then switch to it (git checkout's DWIM).
            if (!repo.UpdateRef(ref, at, "branch: Created from " + remote_short, &err)) {
                o.err += "fatal: " + err + "\n";
                return 128;
            }
            int code = SwitchTo(repo, at, ref, target, "checkout", o);
            if (code != 0) {
                repo.DeleteRef(ref, nullptr);
                return code;
            }
            std::string cfg = repo.CommonDir() + "/config";
            ConfigSet(cfg, "branch." + target + ".remote", remote, &err);
            ConfigSet(cfg, "branch." + target + ".merge", "refs/heads/" + target, &err);
            repo.ReloadConfig();
            o.out += "branch '" + target + "' set up to track '" + remote_short + "'.\n";
            o.err += "Switched to a new branch '" + target + "'\n";
            return 0;
        }
    }
    Oid at, commit;
    if (!repo.RevParse(target, at, &err) || !repo.PeelToCommit(at, commit)) {
        o.err += "error: pathspec '" + target + "' did not match any file(s) known to git\n";
        return 1;
    }
    int code = SwitchTo(repo, commit, "", target, "checkout", o);
    if (code == 0) {
        o.err += "Note: switching to '" + target + "'.\n\nYou are in 'detached HEAD' state.\n";
        o.err += "HEAD is now at " + OnelineOf(repo, commit) + "\n";
    }
    return code;
}

int OpCheckoutNewBranch(Repo &repo, const std::string &name, const std::string &start, Out &o) {
    if (!ValidBranchName(name)) {
        o.err += "fatal: '" + name + "' is not a valid branch name\n";
        return 128;
    }
    std::string ref = "refs/heads/" + name;
    if (repo.RefExists(ref)) {
        o.err += "fatal: a branch named '" + name + "' already exists\n";
        return 128;
    }
    std::string err;
    Oid head;
    bool has_head = HeadCommit(repo, head);
    if (start.empty() && !has_head) {
        // Unborn HEAD: just point HEAD at the new (still unborn) branch.
        repo.SetSymbolicRef("HEAD", ref, "", &err);
        o.err += "Switched to a new branch '" + name + "'\n";
        return 0;
    }
    Oid at = head;
    if (!start.empty()) {
        Oid o2;
        if (!repo.RevParse(start, o2, &err) || !repo.PeelToCommit(o2, at)) {
            o.err += "fatal: '" + start + "' is not a commit and a branch '" + name + "' cannot be created from it\n";
            return 128;
        }
        if (at != head) {
            int code = SwitchTo(repo, at, "", "", "checkout", o);
            if (code) return code;
        }
    }
    if (!repo.UpdateRef(ref, at, "branch: Created from " + (start.empty() ? std::string("HEAD") : start), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    std::string old_desc = BranchShort(repo);
    if (old_desc.empty()) old_desc = head.Hex();
    if (!repo.SetSymbolicRef("HEAD", ref, "checkout: moving from " + old_desc + " to " + name, &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    o.err += "Switched to a new branch '" + name + "'\n";
    return 0;
}

int OpCheckoutPaths(Repo &repo, const std::string &tree_ish, const std::vector<std::string> &paths_in, Out &o) {
    std::string err;
    Index idx;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    std::vector<std::string> paths;
    for (const std::string &p : paths_in) {
        std::string n = NormalizePath(repo.Prefix() + p);
        paths.push_back(n == "." ? "" : n);
    }
    if (tree_ish.empty()) {
        // checkout -- path: from the index.
        bool any = false;
        for (const IndexEntry &e : idx.entries) {
            if (e.Stage() || !PathspecMatch(paths, e.path)) continue;
            any = true;
        }
        for (const std::string &p : paths) {
            bool m = false;
            for (const IndexEntry &e : idx.entries)
                if (PathspecMatch({p}, e.path)) m = true;
            if (!m) {
                o.err += "error: pathspec '" + p + "' did not match any file(s) known to git\n";
                return 1;
            }
        }
        if (!any) return 0;
        for (IndexEntry &e : idx.entries) {
            if (e.Stage() || !PathspecMatch(paths, e.path)) continue;
            if (WorktreeUpToDate(repo, idx, e) && FileExists(JoinPath(repo.WorkTree(), e.path))) continue;
            if (!WriteToWorktree(repo, e.path, e.mode, e.oid, &err)) {
                o.err += err + "\n";
                return 1;
            }
            RefreshEntryStat(repo, e);
        }
        if (!idx.Save(repo.IndexFile(), &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
        return 0;
    }
    Oid at;
    if (!repo.RevParse(tree_ish, at, &err)) {
        o.err += "fatal: invalid reference: " + tree_ish + "\n";
        return 128;
    }
    TreeMap tree;
    if (!CommitTreeMap(repo, at, tree)) {
        o.err += "fatal: reference is not a tree: " + tree_ish + "\n";
        return 128;
    }
    for (const std::string &p : paths) {
        bool m = false;
        for (const auto &kv : tree)
            if (PathspecMatch({p}, kv.first)) {
                m = true;
                break;
            }
        if (!m) {
            o.err += "error: pathspec '" + p + "' did not match any file(s) known to git\n";
            return 1;
        }
    }
    int n = 0;
    for (const auto &[path, t] : tree) {
        if (!PathspecMatch(paths, path)) continue;
        if (!WriteToWorktree(repo, path, t.mode, t.oid, &err)) {
            o.err += err + "\n";
            return 1;
        }
        IndexEntry e = EntryFromTree(t, path);
        RefreshEntryStat(repo, e);
        idx.Put(e);
        n++;
    }
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    o.err += "Updated " + std::to_string(n) + (n == 1 ? " path" : " paths") + " from " +
             (tree_ish == "HEAD" ? std::string("the index") : repo.Abbrev(at)) + "\n";
    return 0;
}

// ------------------------------------------------------------ clean

int OpClean(Repo &repo, bool force, bool dirs, bool dry_run, int force_count, const std::vector<std::string> &paths_in,
            Out &o) {
    if (!force && !dry_run && repo.Cfg().GetBool("clean.requireforce", true)) {
        o.err += "fatal: clean.requireForce is true and -f not given: refusing to clean\n";
        return 128;
    }
    Index idx;
    std::string err;
    idx.Load(repo.IndexFile(), &err);
    std::vector<std::string> paths;
    for (const std::string &p : paths_in) {
        std::string n = NormalizePath(repo.Prefix() + p);
        paths.push_back(n == "." ? "" : n);
    }
    std::vector<std::string> untracked;
    ListUntracked(repo, idx, !dirs, false, paths, untracked, nullptr);
    IgnoreMatcher ign(repo);
    std::function<bool(const std::string &)> remove_dir = [&](const std::string &abs) -> bool {
        // Removes an untracked directory tree -- but like `git clean -d`
        // without -x, never the *ignored* files in it (a .env, build
        // output), and never a nested repository unless -ff.
        if (force_count < 2 && FileExists(abs + "/.git")) return false;
        std::vector<DirEntry> ents;
        ListDir(abs, ents);
        bool all_gone = true;
        std::string rel_dir = abs.substr(repo.WorkTree().size() + 1);
        for (const DirEntry &e : ents) {
            std::string c = abs + "/" + e.name;
            bool is_dir = e.kind == DirEntry::Dir;
            if (ign.IsIgnored(rel_dir + "/" + e.name, is_dir)) {
                all_gone = false;
                continue;
            }
            if (is_dir) {
                if (!remove_dir(c)) all_gone = false;
            } else if (!RemoveFile(c)) {
                all_gone = false;
            }
        }
        if (all_gone) {
#if !defined(_WIN32)
            ::rmdir(abs.c_str());
#else
            RemoveAll(abs);
#endif
        }
        return all_gone;
    };
    for (const std::string &u : untracked) {
        bool is_dir = !u.empty() && u.back() == '/';
        std::string abs = JoinPath(repo.WorkTree(), is_dir ? u.substr(0, u.size() - 1) : u);
        if (is_dir) {
            if (!dirs) continue;
            if (force_count < 2 && FileExists(abs + "/.git")) {
                o.out += "Skipping repository " + u + "\n";
                continue;
            }
            o.out += (dry_run ? "Would remove " : "Removing ") + u + "\n";
            if (!dry_run) remove_dir(abs);
        } else {
            o.out += (dry_run ? "Would remove " : "Removing ") + u + "\n";
            if (!dry_run) RemoveFile(abs);
        }
    }
    return 0;
}

// ------------------------------------------------------------ stash

namespace {
bool ResolveStash(Repo &repo, const std::string &ref_in, Oid &w, Commit &wc, size_t &index, std::string &err) {
    std::string ref = ref_in.empty() ? "stash@{0}" : ref_in;
    // Bare numbers mean stash@{n}.
    if (!ref.empty() && std::all_of(ref.begin(), ref.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
        ref = "stash@{" + ref + "}";
    std::vector<ReflogEntry> log = repo.ReadReflog("refs/stash");
    if (log.empty()) {
        err = "error: No stash entries found.";
        return false;
    }
    size_t open = ref.find("@{");
    index = 0;
    if (open != std::string::npos) {
        std::string n = ref.substr(open + 2, ref.size() - open - 3);
        index = static_cast<size_t>(std::strtoul(n.c_str(), nullptr, 10));
        if (index >= log.size()) {
            err = "error: " + ref + " is not a valid reference";
            return false;
        }
        w = log[log.size() - 1 - index].new_oid;
    } else if (!repo.RevParse(ref, w, &err)) {
        err = "error: " + ref + " is not a valid reference";
        return false;
    }
    if (!repo.ReadCommit(w, wc) || wc.parents.size() < 2) {
        err = "error: '" + ref + "' is not a stash-like commit";
        return false;
    }
    return true;
}
}  // namespace

int OpStashPush(Repo &repo, bool include_untracked, const std::string &message, Out &o) {
    std::string err;
    Oid head;
    if (!HeadCommit(repo, head)) {
        o.err += "You do not have the initial commit yet\n";
        return 1;
    }
    Signature who;
    if (!repo.Ident(true, who, &err)) {
        o.err += err + "\n";
        return 128;
    }
    Index idx;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    Commit hc;
    repo.ReadCommit(head, hc);
    Oid i_tree;
    if (!WriteTreeFromIndex(repo, idx, i_tree, &err)) {
        o.err += "error: could not write index\n";
        return 1;
    }
    std::vector<DiffPair> wt;
    DiffIndexWorktree(repo, idx, {}, wt);
    std::vector<std::string> untracked;
    if (include_untracked) {
        ListUntracked(repo, idx, true, false, {}, untracked, nullptr);
        untracked.erase(std::remove_if(untracked.begin(), untracked.end(),
                                       [](const std::string &p) { return !p.empty() && p.back() == '/'; }),
                        untracked.end());
    }
    if (i_tree == hc.tree && wt.empty() && untracked.empty()) {
        o.out += "No local changes to save\n";
        return 0;
    }
    std::string branch = BranchShort(repo);
    if (branch.empty()) branch = "(no branch)";
    std::string base_desc = branch + ": " + OnelineOf(repo, head);
    // index commit
    Commit ic;
    ic.tree = i_tree;
    ic.parents = {head};
    ic.author = ic.committer = who;
    ic.message = "index on " + base_desc + "\n";
    Oid i_commit;
    if (!repo.Write(ObjType::Commit, SerializeCommit(ic), i_commit, &err)) {
        o.err += err + "\n";
        return 1;
    }
    // untracked commit
    Oid u_commit;
    if (!untracked.empty()) {
        Index uidx;
        for (const std::string &p : untracked) {
            IndexEntry e;
            if (!EntryFromWorktree(repo, p, nullptr, e, true, &err)) continue;
            uidx.Put(e);
        }
        Oid u_tree;
        if (!WriteTreeFromIndex(repo, uidx, u_tree, &err)) {
            o.err += err + "\n";
            return 1;
        }
        Commit uc;
        uc.tree = u_tree;
        uc.author = uc.committer = who;
        uc.message = "untracked files on " + base_desc + "\n";
        if (!repo.Write(ObjType::Commit, SerializeCommit(uc), u_commit, &err)) {
            o.err += err + "\n";
            return 1;
        }
    }
    // worktree commit: index + worktree changes of tracked files
    Index widx = idx;
    for (const DiffPair &p : wt) {
        if (p.status == 'D') {
            widx.Remove(p.a.path);
            continue;
        }
        long i = widx.Find(p.a.path);
        IndexEntry e;
        if (!EntryFromWorktree(repo, p.a.path, i >= 0 ? &widx.entries[static_cast<size_t>(i)] : nullptr, e, true, &err))
            continue;
        widx.Put(e);
    }
    Oid w_tree;
    if (!WriteTreeFromIndex(repo, widx, w_tree, &err)) {
        o.err += err + "\n";
        return 1;
    }
    std::string stash_msg = message.empty() ? "WIP on " + base_desc : "On " + branch + ": " + message;
    Commit wc;
    wc.tree = w_tree;
    wc.parents = {head, i_commit};
    if (!u_commit.IsNull()) wc.parents.push_back(u_commit);
    wc.author = wc.committer = who;
    wc.message = stash_msg + "\n";
    Oid w_commit;
    if (!repo.Write(ObjType::Commit, SerializeCommit(wc), w_commit, &err)) {
        o.err += err + "\n";
        return 1;
    }
    if (!repo.UpdateRef("refs/stash", w_commit, stash_msg, &err, nullptr, true)) {
        o.err += "fatal: " + err + "\n";
        return 1;
    }
    // Reset index and worktree to HEAD, remove the stashed untracked files.
    TreeMap head_tree;
    CommitTreeMap(repo, head, head_tree);
    Index fresh;
    for (const auto &[path, t] : head_tree) {
        long i = idx.Find(path);
        IndexEntry e = EntryFromTree(t, path);
        bool same_index = i >= 0 && idx.entries[static_cast<size_t>(i)].oid == t.oid &&
                          idx.entries[static_cast<size_t>(i)].mode == t.mode;
        bool wt_changed = false;
        for (const DiffPair &p : wt)
            if (p.a.path == path) wt_changed = true;
        if (same_index && !wt_changed) {
            e = idx.entries[static_cast<size_t>(i)];
        } else {
            if (!WriteToWorktree(repo, path, t.mode, t.oid, &err)) {
                o.err += err + "\n";
                return 1;
            }
            RefreshEntryStat(repo, e);
        }
        fresh.entries.push_back(e);
    }
    for (const IndexEntry &e : idx.entries)
        if (!head_tree.count(e.path)) RemoveFromWorktree(repo, e.path);
    fresh.version = idx.version;
    if (!fresh.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 1;
    }
    for (const std::string &p : untracked) RemoveFromWorktree(repo, p);
    o.out += "Saved working directory and index state " + stash_msg + "\n";
    return 0;
}

int OpStashApply(Repo &repo, const std::string &ref, bool pop, Out &o, bool *need_cli) {
    *need_cli = false;
    std::string err;
    Oid w;
    Commit wc;
    size_t index = 0;
    if (!ResolveStash(repo, ref, w, wc, index, err)) {
        o.err += err + "\n";
        return 1;
    }
    Index idx;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    if (idx.HasConflicts()) {
        o.err += "error: cannot apply a stash in the middle of a merge\n";
        return 1;
    }
    Commit bc;
    if (!repo.ReadCommit(wc.parents[0], bc)) {
        o.err += "error: bad stash base\n";
        return 1;
    }
    Oid c_tree;
    if (!WriteTreeFromIndex(repo, idx, c_tree, &err)) {
        o.err += "error: cannot apply a stash in the middle of a merge\n";
        return 1;
    }
    TreeMap base, ours, theirs, untracked;
    CommitTreeMap(repo, bc.tree, base);
    CommitTreeMap(repo, c_tree, ours);
    CommitTreeMap(repo, wc.tree, theirs);
    if (wc.parents.size() >= 3) CommitTreeMap(repo, wc.parents[2], untracked);
    // Decide every path; anything needing a content merge goes to the CLI.
    struct Change {
        std::string path;
        const TreeEntry *t;  // null: delete
    };
    std::vector<Change> changes;
    std::set<std::string> paths;
    for (const auto &kv : base) paths.insert(kv.first);
    for (const auto &kv : ours) paths.insert(kv.first);
    for (const auto &kv : theirs) paths.insert(kv.first);
    std::vector<std::string> dirty, exists;
    for (const std::string &p : paths) {
        auto b = base.find(p), c = ours.find(p), t = theirs.find(p);
        const TreeEntry *B = b == base.end() ? nullptr : &b->second;
        const TreeEntry *C = c == ours.end() ? nullptr : &c->second;
        const TreeEntry *T = t == theirs.end() ? nullptr : &t->second;
        if (Same(T, B) || Same(C, T)) continue;
        if (!Same(C, B)) {
            *need_cli = true;  // both sides changed: a real three-way merge
            return 0;
        }
        long i = idx.Find(p);
        if (i >= 0 && !WorktreeUpToDate(repo, idx, idx.entries[static_cast<size_t>(i)])) dirty.push_back(p);
        if (i < 0 && T && FileExists(JoinPath(repo.WorkTree(), p))) exists.push_back(p);
        changes.push_back({p, T});
    }
    for (const auto &[p, t] : untracked)
        if (FileExists(JoinPath(repo.WorkTree(), p))) exists.push_back(p);
    if (!dirty.empty()) {
        o.err += "error: Your local changes to the following files would be overwritten by merge:\n";
        for (const std::string &p : dirty) o.err += "\t" + p + "\n";
        o.err += "Please commit your changes or stash them before you merge.\nAborting\n";
        return 1;
    }
    if (!exists.empty()) {
        for (const std::string &p : exists) o.err += p + " already exists, no checkout\n";
        o.err += "error: could not restore untracked files from stash\n";
        return 1;
    }
    for (const auto &[p, t] : untracked) {
        if (!WriteToWorktree(repo, p, t.mode, t.oid, &err)) {
            o.err += err + "\n";
            return 1;
        }
    }
    for (const Change &ch : changes) {
        if (!ch.t) {
            RemoveFromWorktree(repo, ch.path);
            continue;  // index keeps the entry: an unstaged deletion
        }
        if (!WriteToWorktree(repo, ch.path, ch.t->mode, ch.t->oid, &err)) {
            o.err += err + "\n";
            return 1;
        }
        if (idx.Find(ch.path) < 0) {
            // New in the stash: staged as added, like git.
            IndexEntry e = EntryFromTree(*ch.t, ch.path);
            RefreshEntryStat(repo, e);
            idx.Put(e);
        }
    }
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    StatusOptions so;
    StatusResult sr;
    if (Status(repo, so, sr, &err)) o.out += LongStatus(repo, sr);
    if (pop) {
        Out d;
        int code = OpStashDrop(repo, "stash@{" + std::to_string(index) + "}", d);
        o.out += d.out;
        o.err += d.err;
        return code;
    }
    return 0;
}

int OpStashDrop(Repo &repo, const std::string &ref, Out &o) {
    std::string err;
    Oid w;
    Commit wc;
    size_t index = 0;
    if (!ResolveStash(repo, ref, w, wc, index, err)) {
        o.err += err + "\n";
        return 1;
    }
    std::vector<ReflogEntry> log = repo.ReadReflog("refs/stash");
    size_t pos = log.size() - 1 - index;
    log.erase(log.begin() + static_cast<long>(pos));
    // --rewrite: the entry after the dropped one now follows its predecessor.
    if (pos < log.size()) log[pos].old_oid = pos > 0 ? log[pos - 1].new_oid : Oid{};
    std::string shown = "stash@{" + std::to_string(index) + "}";
    if (log.empty()) {
        repo.DeleteRef("refs/stash", &err);
    } else {
        std::string path = repo.RefFile("refs/stash");
        if (!WriteFileAtomic(path, log.back().new_oid.Hex() + "\n", &err) || !repo.WriteReflog("refs/stash", log)) {
            o.err += "error: " + err + "\n";
            return 1;
        }
    }
    o.out += "Dropped " + shown + " (" + w.Hex() + ")\n";
    return 0;
}

int OpStashShow(Repo &repo, const std::string &ref, bool patch, bool stat, Out &o) {
    std::string err;
    Oid w;
    Commit wc;
    size_t index = 0;
    if (!ResolveStash(repo, ref, w, wc, index, err)) {
        o.err += err + "\n";
        return 1;
    }
    Commit bc;
    repo.ReadCommit(wc.parents[0], bc);
    std::vector<DiffPair> pairs;
    DiffTrees(repo, bc.tree, wc.tree, {}, pairs);
    if (repo.Cfg().GetBool("diff.renames", true)) DetectRenames(repo, pairs);
    DiffOptions dopt;
    if (!patch && !stat) stat = true;
    if (stat) o.out += FormatStat(repo, pairs, dopt);
    if (patch) {
        if (stat && !pairs.empty()) o.out += "\n";
        o.out += FormatPatch(repo, pairs, dopt);
    }
    return 0;
}

int OpStashList(Repo &repo, const std::string &format, Out &o) {
    LogOptions lo;
    lo.reflog_walk = true;
    lo.reflog_ref = "refs/stash";
    if (!format.empty()) {
        lo.format = LogOptions::Format::User;
        lo.user_format = format;
    } else {
        lo.format = LogOptions::Format::User;
        lo.user_format = "%gd: %gs";
    }
    return RunLog(repo, lo, o.out, o.err);
}

// ------------------------------------------------------------ worktrees

namespace {
struct WorktreeInfo {
    std::string path, admin;  // admin empty for the main worktree
    Oid head;
    std::string branch;  // full ref
    bool bare = false, detached = false, locked = false, prunable = false;
    std::string lock_reason;
};

std::vector<WorktreeInfo> ListWorktrees(Repo &repo) {
    std::vector<WorktreeInfo> out;
    WorktreeInfo main;
    bool bare = repo.Cfg().GetBool("core.bare", false) || (repo.IsBare() && repo.CommonDir() == repo.GitDir());
    main.path = bare ? repo.CommonDir() : repo.MainWorkTree();
    main.bare = bare;
    {
        std::string t;
        if (ReadFile(repo.CommonDir() + "/HEAD", t)) {
            t = Trim(t);
            if (StartsWith(t, "ref: ")) {
                main.branch = t.substr(5);
                repo.Resolve(main.branch, main.head);
            } else {
                main.detached = true;
                Oid::FromHex(t, main.head);
            }
        }
    }
    out.push_back(main);
    std::vector<DirEntry> ents;
    ListDir(repo.CommonDir() + "/worktrees", ents);
    std::vector<WorktreeInfo> linked;
    for (const DirEntry &e : ents) {
        if (e.kind != DirEntry::Dir) continue;
        WorktreeInfo w;
        w.admin = repo.CommonDir() + "/worktrees/" + e.name;
        std::string gd;
        if (ReadFile(w.admin + "/gitdir", gd)) {
            gd = Trim(gd);
            if (!gd.empty() && gd[0] != '/') gd = AbsolutePath(gd, w.admin);
            w.path = DirName(gd);
            if (!FileExists(gd)) w.prunable = true;
        } else {
            w.prunable = true;
            w.path = w.admin;
        }
        std::string t;
        if (ReadFile(w.admin + "/HEAD", t)) {
            t = Trim(t);
            if (StartsWith(t, "ref: ")) {
                w.branch = t.substr(5);
                repo.Resolve(w.branch, w.head);
            } else {
                w.detached = true;
                Oid::FromHex(t, w.head);
            }
        }
        std::string lock;
        if (ReadFile(w.admin + "/locked", lock)) {
            w.locked = true;
            w.lock_reason = Trim(lock);
        }
        linked.push_back(w);
    }
    std::sort(linked.begin(), linked.end(), [](const WorktreeInfo &a, const WorktreeInfo &b) { return a.path < b.path; });
    out.insert(out.end(), linked.begin(), linked.end());
    return out;
}

// Populates a fresh worktree directory and its index from `commit`.
bool PopulateWorktree(Repo &repo, const std::string &dir, const std::string &index_file, const Oid &commit,
                      std::string &err) {
    TreeMap tree;
    if (!CommitTreeMap(repo, commit, tree)) {
        err = "fatal: reference is not a tree";
        return false;
    }
    Index idx;
    for (const auto &[path, t] : tree) {
        std::string abs = JoinPath(dir, path);
        IndexEntry e = EntryFromTree(t, path);
        if ((t.mode & 0170000) == 0160000) {
            MakeDirs(abs);
        } else {
            std::string data;
            if (!repo.ReadBlob(t.oid, data)) {
                err = "error: unable to read " + path;
                return false;
            }
            MakeDirs(DirName(abs));
#if !defined(_WIN32)
            if ((t.mode & 0170000) == 0120000) {
                if (::symlink(data.c_str(), abs.c_str()) != 0) {
                    err = "error: unable to create symlink " + path;
                    return false;
                }
            } else
#endif
            {
                bool exec = (t.mode & 0111) != 0;
                if (!WriteFile(abs, data, exec)) {
                    err = "error: unable to write " + path;
                    return false;
                }
#if !defined(_WIN32)
                ::chmod(abs.c_str(), (exec ? 0777 : 0666) & ~Umask());
#endif
            }
        }
        FileStat st;
        if (LStat(abs, st)) {
            FillStat(e, st);
            if ((t.mode & 0170000) == 0160000) e.size = 0;
        }
        idx.entries.push_back(e);
    }
    return idx.Save(index_file, &err);
}
}  // namespace

int OpWorktreeAdd(Repo &repo, const std::string &path_in, const std::string &commitish, const std::string &new_branch,
                  bool detach, bool force, Out &o) {
    std::string err;
    std::string base = repo.IsBare() ? repo.GitDir() : JoinPath(repo.WorkTree(), repo.Prefix());
    std::string path = AbsolutePath(path_in, base);
    if (FileExists(path)) {
        std::vector<DirEntry> ents;
        if (!IsDirectory(path) || !ListDir(path, ents) || !ents.empty()) {
            o.err += "fatal: '" + path_in + "' already exists\n";
            return 128;
        }
    }
    std::string branch_ref;
    Oid commit;
    std::string from = commitish.empty() ? "HEAD" : commitish;
    if (!new_branch.empty()) {
        if (!ValidBranchName(new_branch)) {
            o.err += "fatal: '" + new_branch + "' is not a valid branch name\n";
            return 128;
        }
        if (repo.RefExists("refs/heads/" + new_branch) && !force) {
            o.err += "fatal: a branch named '" + new_branch + "' already exists\n";
            return 128;
        }
        Oid at;
        if (!repo.RevParse(from, at, &err) || !repo.PeelToCommit(at, commit)) {
            o.err += "fatal: invalid reference: " + from + "\n";
            return 128;
        }
        branch_ref = "refs/heads/" + new_branch;
    } else if (!detach && repo.RefExists("refs/heads/" + from)) {
        branch_ref = "refs/heads/" + from;
        repo.Resolve(branch_ref, commit);
        if (!force) {
            for (const auto &[r, wpath] : CheckedOutBranches(repo)) {
                if (r == branch_ref) {
                    o.err += "fatal: '" + from + "' is already used by worktree at '" + wpath + "'\n";
                    return 128;
                }
            }
        }
    } else {
        Oid at;
        if (!repo.RevParse(from, at, &err) || !repo.PeelToCommit(at, commit)) {
            o.err += "fatal: invalid reference: " + from + "\n";
            return 128;
        }
    }
    // Admin directory name: the basename, made unique.
    std::string name = BaseName(path);
    for (char &c : name)
        if (c == ' ' || c == ':' || c == '\\') c = '-';
    if (name.empty() || name[0] == '.') name = "wt" + name;
    std::string admin_root = repo.CommonDir() + "/worktrees";
    std::string admin = admin_root + "/" + name;
    for (int counter = 1; FileExists(admin); counter++) admin = admin_root + "/" + name + std::to_string(counter);
    if (!MakeDirs(admin) || !MakeDirs(path)) {
        o.err += "fatal: could not create directory of '" + admin + "'\n";
        return 128;
    }
    WriteFile(admin + "/locked", "initializing\n");
    WriteFile(admin + "/gitdir", RealPath(path) + "/.git\n");
    WriteFile(admin + "/commondir", "../..\n");
    WriteFile(path + "/.git", "gitdir: " + RealPath(admin) + "\n");
    // HEAD is the commit first (git does the same), then the branch.
    WriteFile(admin + "/HEAD", commit.Hex() + "\n");
    if (!new_branch.empty()) {
        o.err += "Preparing worktree (new branch '" + new_branch + "')\n";
        if (!repo.UpdateRef(branch_ref, commit, "branch: Created from " + from, &err)) {
            o.err += "fatal: " + err + "\n";
            return 128;
        }
    } else if (!branch_ref.empty()) {
        o.err += "Preparing worktree (checking out '" + from + "')\n";
    } else {
        o.err += "Preparing worktree (detached HEAD " + repo.Abbrev(commit) + ")\n";
    }
    if (!PopulateWorktree(repo, path, admin + "/index", commit, err)) {
        o.err += err + "\n";
        return 128;
    }
    if (!branch_ref.empty()) WriteFile(admin + "/HEAD", "ref: " + branch_ref + "\n");
    RemoveFile(admin + "/locked");
    o.out += "HEAD is now at " + OnelineOf(repo, commit) + "\n";
    return 0;
}

int OpWorktreeList(Repo &repo, bool porcelain, Out &o) {
    std::vector<WorktreeInfo> list = ListWorktrees(repo);
    if (porcelain) {
        for (const WorktreeInfo &w : list) {
            o.out += "worktree " + w.path + "\n";
            if (w.bare) {
                o.out += "bare\n\n";
                continue;
            }
            o.out += "HEAD " + w.head.Hex() + "\n";
            if (w.detached) o.out += "detached\n";
            else o.out += "branch " + w.branch + "\n";
            if (w.locked) o.out += w.lock_reason.empty() ? "locked\n" : "locked " + w.lock_reason + "\n";
            if (w.prunable) o.out += "prunable gitdir file points to non-existent location\n";
            o.out += "\n";
        }
        return 0;
    }
    size_t width = 0;
    for (const WorktreeInfo &w : list) width = std::max(width, w.path.size());
    for (const WorktreeInfo &w : list) {
        std::string line = w.path + std::string(width - w.path.size() + 1, ' ');
        if (w.bare) line += "(bare)";
        else {
            line += repo.Abbrev(w.head) + " ";
            line += w.detached ? "(detached HEAD)" : "[" + (StartsWith(w.branch, "refs/heads/") ? w.branch.substr(11) : w.branch) + "]";
            if (w.locked) line += " locked";
            if (w.prunable) line += " prunable";
        }
        o.out += line + "\n";
    }
    return 0;
}

int OpWorktreeRemove(Repo &repo, const std::string &path_in, int force, Out &o) {
    std::string base = repo.IsBare() ? repo.GitDir() : JoinPath(repo.WorkTree(), repo.Prefix());
    std::string want = RealPath(AbsolutePath(path_in, base));
    std::vector<WorktreeInfo> list = ListWorktrees(repo);
    const WorktreeInfo *found = nullptr;
    for (const WorktreeInfo &w : list) {
        if (RealPath(w.path) == want || (!w.admin.empty() && BaseName(w.admin) == path_in)) {
            found = &w;
            break;
        }
    }
    if (!found) {
        o.err += "fatal: '" + path_in + "' is not a working tree\n";
        return 128;
    }
    if (found->admin.empty()) {
        o.err += "fatal: '" + path_in + "' is a main working tree\n";
        return 128;
    }
    if (found->locked && force < 2) {
        o.err += "fatal: cannot remove a locked working tree, lock reason: " + found->lock_reason +
                 "\nuse 'remove -f -f' to override or unlock first\n";
        return 128;
    }
    if (!force && IsDirectory(found->path)) {
        std::string err;
        std::unique_ptr<Repo> wt = Repo::Discover(found->path, &err);
        if (wt) {
            StatusOptions so;
            so.untracked_set = true;
            so.untracked = StatusOptions::Untracked::All;
            StatusResult sr;
            if (!Status(*wt, so, sr, &err)) {
                o.err += "fatal: failed to run 'git status' on '" + path_in + "'\n";
                return 128;
            }
            if (!sr.changes.empty() || !sr.untracked.empty()) {
                o.err += "fatal: '" + path_in + "' contains modified or untracked files, use --force to delete it\n";
                return 128;
            }
        }
    }
    if (IsDirectory(found->path) && !RemoveAll(found->path)) {
        o.err += "error: failed to delete '" + found->path + "'\n";
        return 128;
    }
    RemoveAll(found->admin);
    std::vector<DirEntry> rest;
    if (ListDir(repo.CommonDir() + "/worktrees", rest) && rest.empty()) RemoveAll(repo.CommonDir() + "/worktrees");
    return 0;
}

int OpWorktreePrune(Repo &repo, Out &o) {
    (void)o;
    for (const WorktreeInfo &w : ListWorktrees(repo)) {
        if (w.admin.empty() || w.locked || !w.prunable) continue;
        RemoveAll(w.admin);
    }
    std::vector<DirEntry> rest;
    if (ListDir(repo.CommonDir() + "/worktrees", rest) && rest.empty()) RemoveAll(repo.CommonDir() + "/worktrees");
    return 0;
}

// ------------------------------------------------------------ merge --ff

int OpMergeFastForward(Repo &repo, const std::string &target, bool ff_only, Out &o, bool *need_cli) {
    *need_cli = false;
    std::string err;
    Oid at, theirs;
    if (!repo.RevParse(target, at, &err) || !repo.PeelToCommit(at, theirs)) {
        o.err += "merge: " + target + " - not something we can merge\n";
        return 1;
    }
    Oid head;
    bool has_head = HeadCommit(repo, head);
    if (has_head && repo.IsAncestor(theirs, head)) {
        o.out += "Already up to date.\n";
        return 0;
    }
    if (has_head && !repo.IsAncestor(head, theirs)) {
        if (ff_only) {
            o.err += "fatal: Not possible to fast-forward, aborting.\n";
            return 128;
        }
        *need_cli = true;  // a real merge: still the git CLI (see git.h)
        return 0;
    }
    std::string ff_cfg = repo.Cfg().Get("merge.ff").value_or("true");
    if (ff_cfg == "false") {
        *need_cli = true;
        return 0;
    }
    Index idx;
    if (!idx.Load(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    TreeMap old_tree, new_tree;
    if (has_head) CommitTreeMap(repo, head, old_tree);
    CommitTreeMap(repo, theirs, new_tree);
    if (!SwitchTrees(repo, idx, old_tree, new_tree, "merge", o)) return 1;
    if (!idx.Save(repo.IndexFile(), &err)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    std::string msg = "merge " + target + ": Fast-forward";
    Oid expected = has_head ? head : Oid{};
    if (!repo.UpdateRef("HEAD", theirs, msg, &err, &expected)) {
        o.err += "fatal: " + err + "\n";
        return 128;
    }
    WriteFile(repo.GitDir() + "/ORIG_HEAD", (has_head ? head : theirs).Hex() + "\n");
    o.out += "Updating " + (has_head ? repo.Abbrev(head) : std::string("0000000")) + ".." + repo.Abbrev(theirs) +
             "\nFast-forward\n";
    Commit hc, tc;
    Oid from_tree;
    if (has_head && repo.ReadCommit(head, hc)) from_tree = hc.tree;
    if (repo.ReadCommit(theirs, tc)) {
        std::vector<DiffPair> pairs;
        DiffTrees(repo, from_tree, tc.tree, {}, pairs);
        DetectRenames(repo, pairs);
        DiffOptions dopt;
        o.out += FormatStat(repo, pairs, dopt);
    }
    return 0;
}

}  // namespace mep::git
