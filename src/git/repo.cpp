// Repository discovery, refs (loose, packed, symbolic), reflogs, revision
// parsing, identities, upstream tracking.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "git/git.h"
#include "git/odb.h"
#include "git/util.h"

#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

namespace mep::git {

Repo::Repo() = default;
Repo::~Repo() = default;

namespace {
bool LooksLikeGitDir(const std::string &dir) {
    if (!FileExists(dir + "/HEAD")) return false;
    if (FileExists(dir + "/commondir")) return true;
    return IsDirectory(dir + "/objects") && IsDirectory(dir + "/refs");
}

// Resolves a `.git` file ("gitdir: <path>") to the directory it names.
bool ReadGitFile(const std::string &file, std::string &out) {
    std::string text;
    if (!ReadFile(file, text)) return false;
    text = Trim(text);
    if (!StartsWith(text, "gitdir:")) return false;
    std::string p = Trim(text.substr(7));
    out = AbsolutePath(p, DirName(file));
    return true;
}
}  // namespace

std::unique_ptr<Repo> Repo::Discover(const std::string &start_in, std::string *err) {
    std::string start = RealPath(start_in.empty() ? "." : start_in);
    std::unique_ptr<Repo> repo(new Repo());
    if (const char *env_dir = std::getenv("GIT_DIR"); env_dir && *env_dir) {
        std::string gd = AbsolutePath(env_dir, start);
        std::string wt;
        if (const char *ewt = std::getenv("GIT_WORK_TREE"); ewt && *ewt) wt = RealPath(AbsolutePath(ewt, start));
        else wt = start;
        std::string prefix;
        if (StartsWith(start + "/", wt + "/") && start.size() > wt.size()) prefix = start.substr(wt.size() + 1) + "/";
        if (!repo->Init(gd, wt, prefix, err)) return nullptr;
        return repo;
    }
    std::vector<std::string> ceilings;
    if (const char *c = std::getenv("GIT_CEILING_DIRECTORIES")) {
        std::string s = c;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) colon = s.size();
            std::string d = s.substr(pos, colon - pos);
            if (!d.empty()) ceilings.push_back(RealPath(d));
            pos = colon + 1;
        }
    }
    std::string dir = start;
    for (;;) {
        std::string dotgit = JoinPath(dir, ".git");
        std::string gd;
        if (IsDirectory(dotgit) && LooksLikeGitDir(dotgit)) gd = dotgit;
        else if (FileExists(dotgit) && !IsDirectory(dotgit) && ReadGitFile(dotgit, gd) && LooksLikeGitDir(gd)) {
        } else gd.clear();
        if (!gd.empty()) {
            std::string prefix;
            if (start.size() > dir.size()) prefix = start.substr(dir == "/" ? 1 : dir.size() + 1) + "/";
            if (!repo->Init(gd, dir, prefix, err)) return nullptr;
            return repo;
        }
        if (LooksLikeGitDir(dir)) {
            // A bare repository (or we are inside a .git directory).
            Config c;
            c.LoadFile(dir + "/config");
            if (c.GetBool("core.bare", false) || BaseName(dir) == ".git") {
                if (!repo->Init(dir, "", "", err)) return nullptr;
                return repo;
            }
        }
        if (dir == "/" || dir.empty()) break;
        std::string parent = DirName(dir);
        if (std::find(ceilings.begin(), ceilings.end(), parent) != ceilings.end()) break;
        if (parent == dir) break;
        dir = parent;
    }
    if (err) *err = "fatal: not a git repository (or any of the parent directories): .git";
    return nullptr;
}

bool Repo::Init(const std::string &git_dir, const std::string &work_tree, const std::string &prefix, std::string *err) {
    git_dir_ = RealPath(git_dir);
    std::string cd;
    if (ReadFile(git_dir_ + "/commondir", cd)) common_dir_ = RealPath(AbsolutePath(Trim(cd), git_dir_));
    else common_dir_ = git_dir_;
    work_tree_ = work_tree.empty() ? "" : RealPath(work_tree);
    prefix_ = prefix;
    config_.LoadStandard(common_dir_, git_dir_);
    if (!work_tree_.empty() && common_dir_ == git_dir_) {
        if (auto wt = config_.Get("core.worktree")) {
            std::string w = RealPath(AbsolutePath(*wt, git_dir_));
            work_tree_ = w;
        }
    }
    int64_t ver = config_.GetInt("core.repositoryformatversion", 0);
    if (ver > 1) {
        if (err) *err = "fatal: unknown repository format version " + std::to_string(ver);
        return false;
    }
    if (auto fmt = config_.Get("extensions.objectformat"); fmt && *fmt != "sha1") {
        if (err) *err = "fatal: unsupported object format " + *fmt;
        return false;
    }
    if (config_.Get("extensions.refstorage") && *config_.Get("extensions.refstorage") != "files") {
        if (err) *err = "fatal: unsupported ref storage";
        return false;
    }
    odb_ = std::make_unique<Odb>(common_dir_ + "/objects");
    if (const char *od = std::getenv("GIT_OBJECT_DIRECTORY"); od && *od) odb_ = std::make_unique<Odb>(od);
    return true;
}

void Repo::ReloadConfig() { config_.LoadStandard(common_dir_, git_dir_); }

std::string Repo::IndexFile() const {
    if (const char *f = std::getenv("GIT_INDEX_FILE"); f && *f) return AbsolutePath(f, work_tree_.empty() ? git_dir_ : work_tree_);
    return git_dir_ + "/index";
}

std::string Repo::MainWorkTree() {
    if (EndsWith(common_dir_, "/.git")) return common_dir_.substr(0, common_dir_.size() - 5);
    if (common_dir_ == git_dir_ && !work_tree_.empty()) return work_tree_;
    return common_dir_;
}

// ------------------------------------------------------------ objects

bool Repo::Read(const Oid &oid, ObjType &type, std::string &data) { return odb_->Read(oid, type, data); }
bool Repo::ReadType(const Oid &oid, ObjType &type) {
    std::string d;
    return odb_->Read(oid, type, d);
}
bool Repo::Has(const Oid &oid) { return odb_->Has(oid); }
bool Repo::Write(ObjType type, std::string_view data, Oid &out, std::string *err) {
    return odb_->WriteLoose(type, data, out, err);
}

bool Repo::ReadCommit(const Oid &oid, Commit &out) {
    ObjType t;
    std::string d;
    if (!Read(oid, t, d) || t != ObjType::Commit) return false;
    return ParseCommit(d, out);
}

bool Repo::ReadTree(const Oid &oid, std::vector<TreeEntry> &out) {
    ObjType t;
    std::string d;
    if (!Read(oid, t, d) || t != ObjType::Tree) return false;
    return ParseTree(d, out);
}

bool Repo::ReadBlob(const Oid &oid, std::string &out) {
    ObjType t;
    return Read(oid, t, out) && t == ObjType::Blob;
}

bool Repo::PeelToCommit(const Oid &oid, Oid &commit) {
    Oid cur = oid;
    for (int i = 0; i < 20; i++) {
        ObjType t;
        std::string d;
        if (!Read(cur, t, d)) return false;
        if (t == ObjType::Commit) {
            commit = cur;
            return true;
        }
        if (t != ObjType::Tag) return false;
        Tag tag;
        if (!ParseTag(d, tag)) return false;
        cur = tag.object;
    }
    return false;
}

bool Repo::PeelToTree(const Oid &oid, Oid &tree) {
    Oid cur = oid;
    for (int i = 0; i < 20; i++) {
        ObjType t;
        std::string d;
        if (!Read(cur, t, d)) return false;
        if (t == ObjType::Tree) {
            tree = cur;
            return true;
        }
        if (t == ObjType::Commit) {
            Commit c;
            if (!ParseCommit(d, c)) return false;
            tree = c.tree;
            return true;
        }
        if (t != ObjType::Tag) return false;
        Tag tag;
        if (!ParseTag(d, tag)) return false;
        cur = tag.object;
    }
    return false;
}

bool Repo::FlattenTree(const Oid &tree, std::map<std::string, TreeEntry> &out, const std::string &prefix) {
    std::vector<TreeEntry> ents;
    if (!ReadTree(tree, ents)) return false;
    for (TreeEntry &e : ents) {
        std::string path = prefix + e.name;
        if (e.IsTree()) {
            if (!FlattenTree(e.oid, out, path + "/")) return false;
        } else {
            e.name = path;
            out[path] = e;
        }
    }
    return true;
}

int Repo::DefaultAbbrev() {
    if (default_abbrev_ > 0) return default_abbrev_;
    int len = -1;
    if (auto v = config_.Get("core.abbrev")) {
        if (*v == "no") len = 40;
        else if (*v != "auto") len = std::clamp(std::atoi(v->c_str()), 4, 40);
    }
    if (len < 0) {
        uint64_t count = odb_->ApproxCount();
        int msb = 0;
        for (uint64_t c = count; c > 1; c >>= 1) msb++;
        len = msb + 1;
        len = (len + 1) / 2;
        if (len < 7) len = 7;
    }
    default_abbrev_ = len;
    return len;
}

std::string Repo::Abbrev(const Oid &oid, int len) {
    if (len < 0) len = DefaultAbbrev();
    if (len >= 40) return oid.Hex();
    int need = odb_->MaxCommonPrefix(oid) + 1;
    return oid.Hex().substr(0, static_cast<size_t>(std::max(len, std::min(need, 40))));
}

bool Repo::ResolvePrefix(std::string_view hex, Oid &out, bool *ambiguous) {
    if (ambiguous) *ambiguous = false;
    if (hex.size() < 4 || hex.size() > 40) return false;
    for (char c : hex)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    std::string lower(hex);
    for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.size() == 40) return Oid::FromHex(lower, out) && Has(out);
    std::vector<Oid> found;
    odb_->FindPrefix(lower, found, 2);
    if (found.size() == 1) {
        out = found[0];
        return true;
    }
    if (found.size() > 1 && ambiguous) *ambiguous = true;
    return false;
}

// ------------------------------------------------------------ refs

namespace {
bool IsPerWorktreeRef(const std::string &name) {
    if (!StartsWith(name, "refs/")) return true;  // HEAD, ORIG_HEAD, MERGE_HEAD, ...
    return StartsWith(name, "refs/bisect/") || StartsWith(name, "refs/worktree/") ||
           StartsWith(name, "refs/rewritten/");
}
}  // namespace

std::string Repo::RefFile(const std::string &name) const {
    return (IsPerWorktreeRef(name) ? git_dir_ : common_dir_) + "/" + name;
}

std::string Repo::LogFile(const std::string &name) const {
    return (IsPerWorktreeRef(name) ? git_dir_ : common_dir_) + "/logs/" + name;
}

void Repo::LoadPackedRefs() {
    if (packed_loaded_) return;
    packed_loaded_ = true;
    packed_.clear();
    packed_peeled_.clear();
    std::string text;
    if (!ReadFile(common_dir_ + "/packed-refs", text)) return;
    size_t pos = 0;
    std::string last;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '^') {
            Oid o;
            if (!last.empty() && Oid::FromHex(line.substr(1, 40), o)) packed_peeled_[last] = o;
            continue;
        }
        if (line.size() < 42) continue;
        Oid o;
        if (!Oid::FromHex(line.substr(0, 40), o)) continue;
        last = std::string(line.substr(41));
        while (!last.empty() && (last.back() == '\r' || last.back() == ' ')) last.pop_back();
        packed_[last] = o;
    }
}

bool Repo::ReadRefRaw(const std::string &name, Oid &oid, std::string &symref) {
    symref.clear();
    std::string text;
    if (ReadFile(RefFile(name), text)) {
        std::string t = Trim(text);
        if (StartsWith(t, "ref:")) {
            symref = Trim(t.substr(4));
            return true;
        }
        if (t.size() >= 40 && Oid::FromHex(std::string_view(t).substr(0, 40), oid)) return true;
        return false;
    }
    if (!StartsWith(name, "refs/")) return false;
    LoadPackedRefs();
    auto it = packed_.find(name);
    if (it == packed_.end()) return false;
    oid = it->second;
    return true;
}

bool Repo::Resolve(const std::string &name, Oid &out, std::string *final_ref) {
    std::string cur = name;
    for (int depth = 0; depth < 6; depth++) {
        std::string sym;
        Oid o;
        if (!ReadRefRaw(cur, o, sym)) return false;
        if (sym.empty()) {
            out = o;
            if (final_ref) *final_ref = cur;
            return true;
        }
        cur = sym;
    }
    return false;
}

bool Repo::RefExists(const std::string &name) {
    Oid o;
    std::string s;
    return ReadRefRaw(name, o, s);
}

std::string Repo::HeadRef() {
    Oid o;
    std::string sym;
    if (!ReadRefRaw("HEAD", o, sym)) {
        // An unborn HEAD still has a symref file; ReadRefRaw handles it.
        return "";
    }
    return sym;
}

std::vector<RefEntry> Repo::ListRefs(std::string_view prefix) {
    std::map<std::string, RefEntry> all;
    LoadPackedRefs();
    for (const auto &[name, oid] : packed_) {
        if (!StartsWith(name, prefix)) continue;
        all[name] = RefEntry{name, oid, ""};
    }
    std::function<void(const std::string &, const std::string &)> walk = [&](const std::string &dir,
                                                                             const std::string &rel) {
        std::vector<DirEntry> ents;
        if (!ListDir(dir, ents)) return;
        for (const DirEntry &e : ents) {
            std::string name = rel + e.name;
            std::string path = dir + "/" + e.name;
            if (e.kind == DirEntry::Dir) {
                walk(path, name + "/");
                continue;
            }
            if (EndsWith(e.name, ".lock")) continue;
            if (!StartsWith(name, prefix) && !StartsWith(prefix, name)) continue;
            if (!StartsWith(name, prefix)) continue;
            std::string text;
            if (!ReadFile(path, text)) continue;
            std::string t = Trim(text);
            RefEntry r;
            r.name = name;
            if (StartsWith(t, "ref:")) {
                r.symref = Trim(t.substr(4));
                if (!Resolve(r.symref, r.oid)) continue;
            } else if (t.size() < 40 || !Oid::FromHex(std::string_view(t).substr(0, 40), r.oid)) {
                continue;
            }
            all[name] = r;
        }
    };
    walk(common_dir_ + "/refs", "refs/");
    std::vector<RefEntry> out;
    out.reserve(all.size());
    for (auto &kv : all) out.push_back(std::move(kv.second));
    return out;
}

void Repo::AppendReflog(const std::string &name, const Oid &old_oid, const Oid &new_oid, const std::string &msg,
                        bool force) {
    std::string path = LogFile(name);
    bool log_all = config_.GetBool("core.logallrefupdates", !IsBare());
    bool should = force || FileExists(path) ||
                  (log_all && (name == "HEAD" || StartsWith(name, "refs/heads/") ||
                               StartsWith(name, "refs/remotes/") || StartsWith(name, "refs/notes/")));
    if (!should) return;
    Signature who;
    if (!Ident(true, who, nullptr)) {
        who.name = "mep";
        who.email = "mep@localhost";
        who.time = NowSeconds();
        who.tz = LocalTzMinutes(who.time);
        who.tz_str = FormatTz(who.tz);
    }
    // git's copy_reflog_msg: whitespace runs become one space, trailing stripped.
    std::string m;
    bool wasspace = true;
    for (char c : msg) {
        bool sp = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (wasspace && sp) continue;
        wasspace = sp;
        m += sp ? ' ' : c;
    }
    while (!m.empty() && m.back() == ' ') m.pop_back();
    std::string line = old_oid.Hex() + " " + new_oid.Hex() + " " + FormatSignature(who);
    if (!m.empty()) line += "\t" + m;
    line += "\n";
    MakeDirs(DirName(path));
    std::string existing;
    ReadFile(path, existing);
    existing += line;
    WriteFile(path, existing);
}

bool Repo::UpdateRef(const std::string &name_in, const Oid &new_oid, const std::string &msg, std::string *err,
                     const Oid *expected_old, bool force_reflog) {
    std::string name = name_in;
    bool via_head = false;
    if (name == "HEAD") {
        std::string sym = HeadRef();
        if (!sym.empty()) {
            name = sym;
            via_head = true;
        }
    }
    Oid old_oid;
    std::string sym;
    bool existed = ReadRefRaw(name, old_oid, sym) && sym.empty();
    if (!existed) old_oid = Oid{};
    if (expected_old && *expected_old != old_oid) {
        if (err) *err = "cannot lock ref '" + name + "': is at " + old_oid.Hex() + " but expected " + expected_old->Hex();
        return false;
    }
    std::string path = RefFile(name);
    MakeDirs(DirName(path));
    if (!WriteFileAtomic(path, new_oid.Hex() + "\n", err)) return false;
    AppendReflog(name, old_oid, new_oid, msg, force_reflog);
    if (!via_head && name != "HEAD" && HeadRef() == name) via_head = true;
    if (via_head) AppendReflog("HEAD", old_oid, new_oid, msg);
    return true;
}

bool Repo::SetSymbolicRef(const std::string &name, const std::string &target, const std::string &msg,
                          std::string *err) {
    Oid old_oid, new_oid;
    Resolve(name, old_oid);
    Resolve(target, new_oid);
    std::string path = RefFile(name);
    MakeDirs(DirName(path));
    if (!WriteFileAtomic(path, "ref: " + target + "\n", err)) return false;
    if (!msg.empty()) AppendReflog(name, old_oid, new_oid, msg);
    return true;
}

bool Repo::DeleteRef(const std::string &name, std::string *err) {
    std::string path = RefFile(name);
    bool removed = false;
    if (FileExists(path)) {
        removed = RemoveFile(path);
        RemoveEmptyParents(path, (IsPerWorktreeRef(name) ? git_dir_ : common_dir_) + "/refs");
    }
    LoadPackedRefs();
    if (packed_.count(name)) {
        std::string text, out;
        ReadFile(common_dir_ + "/packed-refs", text);
        size_t pos = 0;
        bool skip_peel = false;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line[0] == '^') {
                if (!skip_peel) out += line + "\n";
                continue;
            }
            skip_peel = false;
            if (line.size() > 41 && line[0] != '#' && line.substr(41) == name) {
                skip_peel = true;
                continue;
            }
            out += line + "\n";
        }
        if (!WriteFileAtomic(common_dir_ + "/packed-refs", out, err)) return false;
        packed_loaded_ = false;
        removed = true;
    }
    std::string log = LogFile(name);
    if (FileExists(log)) {
        RemoveFile(log);
        RemoveEmptyParents(log, (IsPerWorktreeRef(name) ? git_dir_ : common_dir_) + "/logs");
    }
    if (!removed && err) *err = "ref " + name + " not found";
    return removed;
}

std::vector<ReflogEntry> Repo::ReadReflog(const std::string &name) {
    std::vector<ReflogEntry> out;
    std::string text;
    if (!ReadFile(LogFile(name), text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string_view line(text.data() + pos, nl - pos);
        pos = nl + 1;
        if (line.size() < 83) continue;
        ReflogEntry e;
        if (!Oid::FromHex(line.substr(0, 40), e.old_oid) || !Oid::FromHex(line.substr(41, 40), e.new_oid)) continue;
        std::string_view rest = line.substr(82);
        size_t tab = rest.find('\t');
        ParseSignature(rest.substr(0, tab), e.who);
        if (tab != std::string_view::npos) e.message = std::string(rest.substr(tab + 1));
        out.push_back(std::move(e));
    }
    return out;
}

bool Repo::WriteReflog(const std::string &name, const std::vector<ReflogEntry> &entries) {
    std::string text;
    for (const ReflogEntry &e : entries) {
        text += e.old_oid.Hex() + " " + e.new_oid.Hex() + " " + FormatSignature(e.who);
        if (!e.message.empty()) text += "\t" + e.message;
        text += "\n";
    }
    std::string path = LogFile(name);
    MakeDirs(DirName(path));
    return WriteFileAtomic(path, text, nullptr);
}

namespace {
const char *kRevParseRules[] = {"%s", "refs/%s", "refs/tags/%s", "refs/heads/%s", "refs/remotes/%s",
                                "refs/remotes/%s/HEAD"};
std::string ApplyRule(const char *rule, const std::string &s) {
    std::string r = rule;
    size_t p = r.find("%s");
    return r.substr(0, p) + s + r.substr(p + 2);
}
// If `ref` matches `rule`, sets the short part.
bool MatchRule(const char *rule, const std::string &ref, std::string &short_name) {
    std::string r = rule;
    size_t p = r.find("%s");
    std::string pre = r.substr(0, p), post = r.substr(p + 2);
    if (ref.size() <= pre.size() + post.size()) return false;
    if (!StartsWith(ref, pre) || !EndsWith(ref, post)) return false;
    short_name = ref.substr(pre.size(), ref.size() - pre.size() - post.size());
    return true;
}
}  // namespace

std::string Repo::ShortenRef(const std::string &full) {
    const int nr = 6;
    for (int i = nr - 1; i > 0; --i) {
        std::string short_name;
        if (!MatchRule(kRevParseRules[i], full, short_name)) continue;
        int j;
        for (j = 0; j < i; j++) {
            if (RefExists(ApplyRule(kRevParseRules[j], short_name))) break;
        }
        if (j == i) return short_name;
    }
    return full;
}

std::string Repo::DwimRef(const std::string &name) {
    if (name == "HEAD" || name == "@") return "HEAD";
    auto pseudo = [](const std::string &n) {
        if (n.empty()) return false;
        for (char c : n)
            if (!(std::isupper(static_cast<unsigned char>(c)) || c == '_')) return false;
        return EndsWith(n, "HEAD") || n == "AUTO_MERGE";
    };
    for (const char *rule : kRevParseRules) {
        std::string full = ApplyRule(rule, name);
        if (!StartsWith(full, "refs/") && !pseudo(full)) continue;
        Oid o;
        if (Resolve(full, o)) return full;
    }
    return "";
}

// ------------------------------------------------------------ revisions

bool Repo::RevParse(std::string_view spec, Oid &out, std::string *err) {
    return RevParseWithMode(spec, out, nullptr, err);
}

bool Repo::RevParseWithMode(std::string_view spec_in, Oid &out, uint32_t *mode, std::string *err) {
    std::string spec(spec_in);
    auto fail = [&](const std::string &msg) {
        if (err) *err = msg;
        return false;
    };
    auto resolve_path = [&](const std::string &p) {
        // ./ and ../ are relative to the current directory, anything else to the top.
        if (StartsWith(p, "./") || StartsWith(p, "../") || p == "." || p == "..") {
            std::string n = NormalizePath(prefix_ + p);
            return n == "." ? std::string() : n;
        }
        return p;
    };
    // :path, :N:path -- the index
    if (!spec.empty() && spec[0] == ':' && spec.size() > 1 && spec[1] != '/') {
        int stage = 0;
        std::string path = spec.substr(1);
        if (path.size() >= 2 && path[0] >= '0' && path[0] <= '3' && path[1] == ':') {
            stage = path[0] - '0';
            path = path.substr(2);
        }
        path = resolve_path(path);
        Index idx;
        idx.Load(IndexFile(), nullptr);
        long i = idx.Find(path, stage);
        if (i < 0) {
            if (!work_tree_.empty() && FileExists(work_tree_ + "/" + path))
                return fail("fatal: path '" + path + "' exists on disk, but not in the index");
            return fail("fatal: path '" + path + "' does not exist (neither on disk nor in the index)");
        }
        out = idx.entries[static_cast<size_t>(i)].oid;
        if (mode) *mode = idx.entries[static_cast<size_t>(i)].mode;
        return true;
    }
    // rev:path
    size_t colon = std::string::npos;
    {
        int depth = 0;
        for (size_t i = 0; i < spec.size(); i++) {
            if (spec[i] == '{') depth++;
            else if (spec[i] == '}') depth--;
            else if (spec[i] == ':' && depth == 0) {
                colon = i;
                break;
            }
        }
    }
    if (colon != std::string::npos) {
        std::string rev = spec.substr(0, colon);
        std::string path = resolve_path(spec.substr(colon + 1));
        Oid base, tree;
        if (!RevParseWithMode(rev, base, nullptr, err)) return false;
        if (!PeelToTree(base, tree)) return fail("fatal: invalid object name '" + rev + "'.");
        if (path.empty()) {
            out = tree;
            if (mode) *mode = 040000;
            return true;
        }
        Oid cur = tree;
        uint32_t cur_mode = 040000;
        size_t pos = 0;
        while (pos <= path.size()) {
            size_t slash = path.find('/', pos);
            if (slash == std::string::npos) slash = path.size();
            std::string comp = path.substr(pos, slash - pos);
            pos = slash + 1;
            if (comp.empty()) continue;
            std::vector<TreeEntry> ents;
            if (!ReadTree(cur, ents)) {
                cur.b = {};
                break;
            }
            bool found = false;
            for (const TreeEntry &e : ents) {
                if (e.name == comp) {
                    cur = e.oid;
                    cur_mode = e.mode;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (!work_tree_.empty() && FileExists(work_tree_ + "/" + path))
                    return fail("fatal: path '" + path + "' exists on disk, but not in '" + rev + "'");
                return fail("fatal: path '" + path + "' does not exist in '" + rev + "'");
            }
        }
        out = cur;
        if (mode) *mode = cur_mode;
        return true;
    }
    // base + suffixes
    size_t i = 0;
    // The base name ends at the first ~ or ^ (or @{ that isn't the whole thing).
    size_t end = spec.size();
    for (size_t k = 0; k < spec.size(); k++) {
        if (spec[k] == '~' || spec[k] == '^') {
            end = k;
            break;
        }
        if (spec[k] == '@' && k + 1 < spec.size() && spec[k + 1] == '{') {
            end = k;
            break;
        }
    }
    std::string base = spec.substr(0, end);
    i = end;
    Oid cur;
    std::string cur_ref;
    if (base.empty() || base == "@") base = "HEAD";
    // reflog / upstream selector right after the name
    if (i < spec.size() && spec[i] == '@' && i + 1 < spec.size() && spec[i + 1] == '{') {
        size_t close = spec.find('}', i);
        if (close == std::string::npos) return fail("fatal: bad revision '" + spec + "'");
        std::string sel = spec.substr(i + 2, close - i - 2);
        i = close + 1;
        std::string ref = DwimRef(base);
        if (sel == "u" || sel == "upstream" || sel == "push") {
            if (ref == "HEAD") ref = HeadRef();
            std::string up = UpstreamOf(ref);
            if (up.empty() || !Resolve(up, cur)) return fail("fatal: no upstream configured for branch '" + base + "'");
        } else {
            bool all_digits = !sel.empty() && std::all_of(sel.begin(), sel.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            if (!all_digits) return fail("fatal: unsupported reflog selector '" + sel + "'");
            if (ref.empty()) {
                // stash@{0} where refs/stash exists, etc.
                return fail("fatal: ambiguous argument '" + spec + "': unknown revision or path not in the working tree.");
            }
            std::vector<ReflogEntry> log = ReadReflog(ref);
            size_t n = static_cast<size_t>(std::strtoul(sel.c_str(), nullptr, 10));
            if (n == 0 && log.empty()) {
                if (!Resolve(ref, cur)) return fail("fatal: bad revision '" + spec + "'");
            } else {
                if (n >= log.size()) return fail("fatal: log for '" + base + "' only has " + std::to_string(log.size()) + " entries");
                cur = log[log.size() - 1 - n].new_oid;
            }
        }
    } else {
        std::string ref = DwimRef(base);
        if (!ref.empty()) {
            Resolve(ref, cur);
            cur_ref = ref;
        } else {
            bool amb = false;
            if (!ResolvePrefix(base, cur, &amb)) {
                if (amb) return fail("error: short object ID " + base + " is ambiguous");
                return fail("fatal: ambiguous argument '" + spec +
                            "': unknown revision or path not in the working tree.\nUse '--' to separate paths from revisions, like this:\n'git <command> [<revision>...] -- [<file>...]'");
            }
        }
    }
    while (i < spec.size()) {
        char c = spec[i++];
        if (c == '~') {
            size_t n = 1;
            if (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) {
                n = 0;
                while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) n = n * 10 + static_cast<size_t>(spec[i++] - '0');
            }
            for (size_t k = 0; k < n; k++) {
                Oid commit;
                Commit cm;
                if (!PeelToCommit(cur, commit) || !ReadCommit(commit, cm) || cm.parents.empty())
                    return fail("fatal: ambiguous argument '" + spec + "': unknown revision or path not in the working tree.");
                cur = cm.parents[0];
            }
        } else if (c == '^') {
            if (i < spec.size() && spec[i] == '{') {
                size_t close = spec.find('}', i);
                if (close == std::string::npos) return fail("fatal: bad revision '" + spec + "'");
                std::string what = spec.substr(i + 1, close - i - 1);
                i = close + 1;
                Oid o;
                if (what == "commit") {
                    if (!PeelToCommit(cur, o)) return fail("fatal: bad revision '" + spec + "'");
                } else if (what == "tree") {
                    if (!PeelToTree(cur, o)) return fail("fatal: bad revision '" + spec + "'");
                } else if (what.empty() || what == "object") {
                    o = cur;
                    ObjType t;
                    std::string d;
                    while (Read(o, t, d) && t == ObjType::Tag) {
                        Tag tg;
                        if (!ParseTag(d, tg)) break;
                        o = tg.object;
                    }
                } else {
                    return fail("fatal: bad revision '" + spec + "'");
                }
                cur = o;
                continue;
            }
            size_t n = 1;
            if (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) {
                n = 0;
                while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) n = n * 10 + static_cast<size_t>(spec[i++] - '0');
            }
            Oid commit;
            if (!PeelToCommit(cur, commit)) return fail("fatal: bad revision '" + spec + "'");
            if (n == 0) {
                cur = commit;
                continue;
            }
            Commit cm;
            if (!ReadCommit(commit, cm) || cm.parents.size() < n)
                return fail("fatal: ambiguous argument '" + spec + "': unknown revision or path not in the working tree.");
            cur = cm.parents[n - 1];
        } else {
            return fail("fatal: bad revision '" + spec + "'");
        }
    }
    out = cur;
    if (mode) *mode = 0;
    return true;
}

// ------------------------------------------------------------ identity

bool Repo::Ident(bool committer, Signature &out, std::string *err) const {
    const char *kind = committer ? "COMMITTER" : "AUTHOR";
    auto env = [](const std::string &n) -> std::string {
        const char *v = std::getenv(n.c_str());
        return v ? v : "";
    };
    std::string name = env(std::string("GIT_") + kind + "_NAME");
    std::string email = env(std::string("GIT_") + kind + "_EMAIL");
    if (name.empty()) {
        if (auto v = config_.Get(committer ? "committer.name" : "author.name")) name = *v;
        else if (auto u = config_.Get("user.name")) name = *u;
    }
    if (email.empty()) {
        if (auto v = config_.Get(committer ? "committer.email" : "author.email")) email = *v;
        else if (auto u = config_.Get("user.email")) email = *u;
        else if (!env("EMAIL").empty()) email = env("EMAIL");
    }
    if (name.empty() || email.empty()) {
        if (err) {
            *err = std::string(committer ? "Committer" : "Author") +
                   " identity unknown\n\n*** Please tell me who you are.\n\nRun\n\n"
                   "  git config --global user.email \"you@example.com\"\n"
                   "  git config --global user.name \"Your Name\"\n\n"
                   "to set your account's default identity.\n"
                   "Omit --global to set the identity only in this repository.\n\n"
                   "fatal: unable to auto-detect email address";
        }
        return false;
    }
    out.name = name;
    out.email = email;
    std::string date = env(std::string("GIT_") + kind + "_DATE");
    int64_t t;
    int tz;
    if (!date.empty() && ParseDate(date, t, tz)) {
        out.time = t;
        out.tz = tz;
    } else {
        out.time = NowSeconds();
        out.tz = LocalTzMinutes(out.time);
    }
    out.tz_str = FormatTz(out.tz);
    return true;
}

// ------------------------------------------------------------ tracking

std::string Repo::UpstreamOf(const std::string &branch_ref) {
    if (!StartsWith(branch_ref, "refs/heads/")) return "";
    std::string br = branch_ref.substr(11);
    auto remote = config_.Get("branch." + br + ".remote");
    auto merge = config_.Get("branch." + br + ".merge");
    if (!remote || !merge || merge->empty()) return "";
    if (*remote == ".") return *merge;
    for (const std::string &spec_in : config_.GetAll("remote." + *remote + ".fetch")) {
        std::string spec = spec_in;
        if (!spec.empty() && spec[0] == '+') spec = spec.substr(1);
        size_t colon = spec.find(':');
        if (colon == std::string::npos) continue;
        std::string src = spec.substr(0, colon), dst = spec.substr(colon + 1);
        size_t star = src.find('*');
        if (star == std::string::npos) {
            if (src == *merge) return dst;
            continue;
        }
        std::string pre = src.substr(0, star), post = src.substr(star + 1);
        if (StartsWith(*merge, pre) && EndsWith(*merge, post) && merge->size() >= pre.size() + post.size()) {
            std::string mid = merge->substr(pre.size(), merge->size() - pre.size() - post.size());
            size_t dstar = dst.find('*');
            if (dstar == std::string::npos) return dst;
            return dst.substr(0, dstar) + mid + dst.substr(dstar + 1);
        }
    }
    return "";
}

namespace {
struct Walker {
    Repo &repo;
    std::unordered_map<Oid, int64_t, OidHash> dates;
    std::unordered_map<Oid, std::vector<Oid>, OidHash> parents;
    bool Load(const Oid &o) {
        if (dates.count(o)) return true;
        Commit c;
        if (!repo.ReadCommit(o, c)) return false;
        dates[o] = c.committer.time;
        parents[o] = c.parents;
        return true;
    }
};
}  // namespace

bool Repo::AheadBehind(const Oid &a, const Oid &b, int &ahead, int &behind) {
    ahead = behind = 0;
    if (a == b) return true;
    constexpr int kA = 1, kB = 2, kStale = 4;
    Walker w{*this, {}, {}};
    std::unordered_map<Oid, int, OidHash> flags;
    using Item = std::pair<int64_t, Oid>;
    auto cmp = [](const Item &x, const Item &y) { return x.first < y.first; };
    std::priority_queue<Item, std::vector<Item>, decltype(cmp)> q(cmp);
    if (!w.Load(a) || !w.Load(b)) return false;
    flags[a] |= kA;
    flags[b] |= kB;
    q.push({w.dates[a], a});
    q.push({w.dates[b], b});
    auto any_live = [&]() {
        // Cheap check: the queue holds at least one entry that isn't stale.
        std::vector<Item> tmp;
        bool live = false;
        auto copy = q;
        while (!copy.empty()) {
            if (!(flags[copy.top().second] & kStale)) {
                live = true;
                break;
            }
            copy.pop();
        }
        return live;
    };
    size_t iter = 0;
    while (!q.empty()) {
        if ((++iter & 63) == 0 && !any_live()) break;
        Oid c = q.top().second;
        q.pop();
        int f = flags[c];
        int prop = f & (kA | kB | kStale);
        if ((f & (kA | kB)) == (kA | kB)) prop |= kStale;
        flags[c] = f | (prop & kStale);
        for (const Oid &p : w.parents[c]) {
            if (!w.Load(p)) continue;
            int &pf = flags[p];
            if ((pf & prop) != prop) {
                pf |= prop;
                q.push({w.dates[p], p});
            }
        }
    }
    for (const auto &[oid, f] : flags) {
        if ((f & kA) && !(f & kB)) ahead++;
        else if ((f & kB) && !(f & kA)) behind++;
    }
    return true;
}

bool Repo::IsAncestor(const Oid &ancestor, const Oid &descendant) {
    if (ancestor == descendant) return true;
    std::unordered_set<Oid, OidHash> seen;
    std::deque<Oid> q{descendant};
    Commit anc;
    if (!ReadCommit(ancestor, anc)) return false;
    while (!q.empty()) {
        Oid c = q.front();
        q.pop_front();
        if (!seen.insert(c).second) continue;
        if (c == ancestor) return true;
        Commit cm;
        if (!ReadCommit(c, cm)) continue;
        // A day of clock-skew slop, like git's own date-based cutoffs.
        if (cm.committer.time + 86400 < anc.committer.time) continue;
        for (const Oid &p : cm.parents) q.push_back(p);
    }
    return false;
}

}  // namespace mep::git
