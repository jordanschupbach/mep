#pragma once

// mep::git -- an in-house implementation of the *local* half of git
// (DEPENDENCY_REMOVAL_PLAN.md Phase 4): repository discovery, the object
// database (loose objects + packfiles with deltas), refs and reflogs,
// config, the index, status, diff, log/graph, and the write operations
// mep's git UI uses (add/reset/commit/branch/checkout/stash/clean/
// worktree, fast-forward merge).
//
// Network operations (fetch/pull/push/clone) are deliberately NOT here:
// they need TLS/SSH and stay on the git CLI until Phases 2/3 land. So do
// non-fast-forward merges and rebases (cli.cpp runs the git CLI for those
// from the worker thread, see RunCommand).
//
// Everything here is plain blocking code with no Editor/Lua/JobManager
// dependency, so mep-git-test can exercise it directly; git_job.cpp is the
// thin layer that runs it on a JobManager task thread.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mep::git {

// ---------------------------------------------------------------- objects

struct Oid {
    std::array<uint8_t, 20> b{};
    bool IsNull() const;
    std::string Hex() const;
    static bool FromHex(std::string_view hex, Oid &out);
    auto operator<=>(const Oid &) const = default;
    bool operator==(const Oid &) const = default;
};
struct OidHash {
    size_t operator()(const Oid &o) const {
        size_t h = 0;
        for (size_t i = 0; i < sizeof(size_t); i++) h = (h << 8) | o.b[i];
        return h;
    }
};

enum class ObjType : int { None = 0, Commit = 1, Tree = 2, Blob = 3, Tag = 4 };
const char *TypeName(ObjType t);
ObjType TypeFromName(std::string_view name);

// The object id git gives `data` stored as an object of type `t`.
Oid HashObject(ObjType t, std::string_view data);

struct Signature {
    std::string name, email;
    int64_t time = 0;
    int tz = 0;  // minutes east of UTC
    std::string tz_str;  // as written ("+0200")
};
bool ParseSignature(std::string_view line, Signature &out);
std::string FormatSignature(const Signature &s);
std::string FormatTz(int minutes);

struct Commit {
    Oid tree;
    std::vector<Oid> parents;
    Signature author, committer;
    std::string encoding;
    std::string message;  // everything after the header's blank line
};
bool ParseCommit(std::string_view data, Commit &out);
std::string SerializeCommit(const Commit &c);
std::string CommitSubject(const std::string &message);  // git's %s: first paragraph, lines joined by ' '
std::string CommitBody(const std::string &message);     // git's %b

struct TreeEntry {
    uint32_t mode = 0;
    std::string name;
    Oid oid;
    bool IsTree() const { return (mode & 0170000) == 0040000; }
};
bool ParseTree(std::string_view data, std::vector<TreeEntry> &out);
// Sorts in git's tree order (directories compare as if suffixed by '/').
std::string SerializeTree(std::vector<TreeEntry> entries);

struct Tag {
    Oid object;
    ObjType type = ObjType::None;
    std::string name;
    Signature tagger;
    bool has_tagger = false;
    std::string message;
};
bool ParseTag(std::string_view data, Tag &out);

// ---------------------------------------------------------------- config

class Config {
public:
    struct Entry {
        std::string key;  // "section.subsection.name": section and name lowercased
        std::string value;
        bool has_value = true;
    };
    // Loads the standard set: system, global (XDG + ~/.gitconfig), repo
    // `config`, and `config.worktree` when extensions.worktreeConfig is on.
    void LoadStandard(const std::string &common_dir, const std::string &git_dir);
    bool LoadFile(const std::string &path, int depth = 0);
    std::optional<std::string> Get(std::string_view key) const;
    std::vector<std::string> GetAll(std::string_view key) const;
    bool GetBool(std::string_view key, bool def) const;
    int64_t GetInt(std::string_view key, int64_t def) const;
    const std::vector<Entry> &Entries() const { return entries_; }

    static std::string CanonicalKey(std::string_view key);

private:
    std::vector<Entry> entries_;
    std::string git_dir_;  // for includeIf gitdir:
};

// Edits a config *file* in place, the way `git config` does: replaces the
// last occurrence of `key` (or appends to its section, creating it).
bool ConfigSet(const std::string &file, const std::string &key, const std::string &value, std::string *err);
// Removes `[section "sub"]` and everything in it (git branch -D does this).
bool ConfigRemoveSection(const std::string &file, const std::string &section, const std::string &subsection);

// ---------------------------------------------------------------- refs

struct RefEntry {
    std::string name;
    Oid oid;             // resolved target
    std::string symref;  // non-empty for a symbolic ref: its immediate target
};

struct ReflogEntry {
    Oid old_oid, new_oid;
    Signature who;
    std::string message;
};

// ---------------------------------------------------------------- repo

class Odb;

class Repo {
public:
    // Finds the repository containing `start` (a directory), the way git
    // does: walks up looking for `.git` (a directory or a `gitdir:` file),
    // honouring GIT_DIR / GIT_WORK_TREE / GIT_CEILING_DIRECTORIES.
    static std::unique_ptr<Repo> Discover(const std::string &start, std::string *err);
    ~Repo();
    Repo(const Repo &) = delete;
    Repo &operator=(const Repo &) = delete;

    const std::string &GitDir() const { return git_dir_; }
    const std::string &CommonDir() const { return common_dir_; }
    const std::string &WorkTree() const { return work_tree_; }  // "" for a bare repo
    // The starting directory relative to WorkTree(), with a trailing '/'
    // (or "" at the top) -- what pathspecs and `./` paths resolve against.
    const std::string &Prefix() const { return prefix_; }
    bool IsBare() const { return work_tree_.empty(); }
    const Config &Cfg() const { return config_; }
    void ReloadConfig();
    std::string IndexFile() const;

    // Objects.
    bool Read(const Oid &oid, ObjType &type, std::string &data);
    bool ReadType(const Oid &oid, ObjType &type);
    bool Has(const Oid &oid);
    bool Write(ObjType type, std::string_view data, Oid &out, std::string *err = nullptr);
    bool ReadCommit(const Oid &oid, Commit &out);
    bool ReadTree(const Oid &oid, std::vector<TreeEntry> &out);
    bool ReadBlob(const Oid &oid, std::string &out);
    // Peels tags and commits down to a tree id.
    bool PeelToTree(const Oid &oid, Oid &tree);
    bool PeelToCommit(const Oid &oid, Oid &commit);
    // Flattens a tree recursively into path -> entry (blobs, symlinks and
    // gitlinks; never trees).
    bool FlattenTree(const Oid &tree, std::map<std::string, TreeEntry> &out, const std::string &prefix = "");

    // Abbreviation: git's core.abbrev (auto = from the approximate object
    // count, min 7), extended until unique.
    int DefaultAbbrev();
    std::string Abbrev(const Oid &oid, int len = -1);
    // Resolves a hex prefix (>= 4 chars). Returns false if none or ambiguous.
    bool ResolvePrefix(std::string_view hex, Oid &out, bool *ambiguous = nullptr);

    // Refs.
    // One level: on success either `oid` is set, or `symref` names the target.
    bool ReadRefRaw(const std::string &name, Oid &oid, std::string &symref);
    bool Resolve(const std::string &name, Oid &out, std::string *final_ref = nullptr);
    bool RefExists(const std::string &name);
    // HEAD's symbolic target ("refs/heads/main"), or "" when detached.
    std::string HeadRef();
    std::vector<RefEntry> ListRefs(std::string_view prefix = "refs/");
    bool UpdateRef(const std::string &name, const Oid &new_oid, const std::string &msg, std::string *err = nullptr,
                   const Oid *expected_old = nullptr, bool force_reflog = false);
    bool SetSymbolicRef(const std::string &name, const std::string &target, const std::string &msg,
                        std::string *err = nullptr);
    bool DeleteRef(const std::string &name, std::string *err = nullptr);
    std::vector<ReflogEntry> ReadReflog(const std::string &name);
    bool WriteReflog(const std::string &name, const std::vector<ReflogEntry> &entries);
    void AppendReflog(const std::string &name, const Oid &old_oid, const Oid &new_oid, const std::string &msg,
                      bool force = false);
    std::string RefFile(const std::string &name) const;
    std::string LogFile(const std::string &name) const;
    // git's "shorten_unambiguous_ref" (what %(refname:short) prints).
    std::string ShortenRef(const std::string &full);
    // DWIM a short name ("main", "origin/main", "v1") to a full ref name.
    std::string DwimRef(const std::string &name);

    // Revisions: names, hex (full or abbreviated), ~N, ^N, ^{tree},
    // ^{commit}, @{N} reflog entries (stash@{1}), rev:path, :path, :N:path.
    bool RevParse(std::string_view spec, Oid &out, std::string *err = nullptr);
    // The (mode) of `rev:path`, when the spec named a path.
    bool RevParseWithMode(std::string_view spec, Oid &out, uint32_t *mode, std::string *err);

    // Identity for a new commit (GIT_{AUTHOR,COMMITTER}_{NAME,EMAIL,DATE},
    // user.name/email, author.*/committer.*), time "now" unless the env says.
    bool Ident(bool committer, Signature &out, std::string *err) const;

    // Upstream of a local branch (full ref, e.g. refs/remotes/origin/main),
    // or "" if none configured.
    std::string UpstreamOf(const std::string &branch_ref);
    // Commits reachable from a but not b, and vice versa.
    bool AheadBehind(const Oid &a, const Oid &b, int &ahead, int &behind);
    bool IsAncestor(const Oid &ancestor, const Oid &descendant);

    // Linked worktrees: (path, admin dir) for every entry under
    // $GIT_COMMON_DIR/worktrees, plus the main worktree's path.
    std::string MainWorkTree();

private:
    Repo();
    bool Init(const std::string &git_dir, const std::string &work_tree, const std::string &prefix, std::string *err);
    void LoadPackedRefs();

    std::string git_dir_, common_dir_, work_tree_, prefix_;
    Config config_;
    std::unique_ptr<Odb> odb_;
    bool packed_loaded_ = false;
    std::map<std::string, Oid> packed_;
    std::map<std::string, Oid> packed_peeled_;
    int default_abbrev_ = -1;
};

// ---------------------------------------------------------------- index

struct IndexEntry {
    uint32_t ctime_s = 0, ctime_ns = 0, mtime_s = 0, mtime_ns = 0;
    uint32_t dev = 0, ino = 0, mode = 0, uid = 0, gid = 0, size = 0;
    Oid oid;
    uint16_t flags = 0;      // assume-valid, extended, stage (name length is recomputed on write)
    uint16_t flags_ext = 0;  // skip-worktree, intent-to-add (v3+)
    std::string path;
    int Stage() const { return (flags >> 12) & 3; }
    void SetStage(int s) { flags = static_cast<uint16_t>((flags & ~0x3000) | ((s & 3) << 12)); }
    bool IntentToAdd() const { return (flags_ext & 0x2000) != 0; }
    bool SkipWorktree() const { return (flags_ext & 0x4000) != 0; }
    bool AssumeValid() const { return (flags & 0x8000) != 0; }
};

class Index {
public:
    // Missing file = empty index (success).
    bool Load(const std::string &path, std::string *err);
    // Writes atomically via `<path>.lock`. Fails if the lock exists.
    bool Save(const std::string &path, std::string *err);

    std::vector<IndexEntry> entries;  // sorted by (path, stage)
    uint32_t version = 2;
    int64_t file_mtime_s = 0, file_mtime_ns = 0;  // of the loaded file, for the racy-git check
    bool loaded_from_file = false;

    // Index of the entry (path, stage), or -1.
    long Find(std::string_view path, int stage = 0) const;
    // First entry with path >= `path` (any stage).
    size_t LowerBound(std::string_view path) const;
    // Inserts or replaces the stage-0 entry, dropping any conflict stages.
    void Put(IndexEntry e);
    // Removes every stage of `path`. Returns whether anything was removed.
    bool Remove(std::string_view path);
    bool HasConflicts() const;
};

// Lstat-derived fields, in the index's own (truncated 32-bit) units.
struct FileStat {
    bool exists = false;
    bool is_dir = false, is_reg = false, is_link = false;
    bool exec = false;
    uint32_t ctime_s = 0, ctime_ns = 0, mtime_s = 0, mtime_ns = 0, dev = 0, ino = 0, uid = 0, gid = 0, size = 0;
    uint64_t full_size = 0;
};
bool LStat(const std::string &path, FileStat &st);
// The git mode a file would get in the index (100644/100755/120000).
uint32_t ModeFromStat(const FileStat &st, const Config &cfg, uint32_t existing_mode);
void FillStat(IndexEntry &e, const FileStat &st);
// True when the cached stat data says the file is unchanged (git's
// ie_match_stat, minus the content check).
bool StatMatches(const IndexEntry &e, const FileStat &st, const Config &cfg);
// Hashes the worktree file as a blob (symlink -> its target).
bool HashWorktreeFile(const std::string &abs, const FileStat &st, Oid &out, std::string *content = nullptr);
bool ReadWorktreeFile(const std::string &abs, const FileStat &st, std::string &out);

// Builds trees for the index's stage-0 entries and writes any that are
// missing. Fails on unmerged entries.
bool WriteTreeFromIndex(Repo &repo, const Index &idx, Oid &out, std::string *err);

// ---------------------------------------------------------------- ignore

class IgnoreMatcher {
public:
    explicit IgnoreMatcher(Repo &repo);
    // `rel` is the worktree-relative path ('/'-separated, no trailing '/').
    // Loads .gitignore files of every parent directory lazily.
    bool IsIgnored(const std::string &rel, bool is_dir);

private:
    struct Rule {
        std::string pattern;
        std::string base;  // directory of the .gitignore, "" or "dir/"
        bool negate = false, dir_only = false, anchored = false, no_slash = false;
    };
    struct Dir {
        std::vector<Rule> rules;
    };
    const Dir &DirRules(const std::string &dir);
    void ParseFile(const std::string &path, const std::string &base, std::vector<Rule> &out);
    int MatchList(const std::vector<Rule> &rules, const std::string &rel, const std::string &name, bool is_dir);
    Repo &repo_;
    std::vector<Rule> global_;  // info/exclude + core.excludesFile (lowest precedence)
    std::unordered_map<std::string, Dir> dirs_;
    bool ignore_case_ = false;
};

// ---------------------------------------------------------------- status

struct StatusEntry {
    char x = ' ', y = ' ';
    std::string path;       // new path for a rename
    std::string orig_path;  // rename source
    int score = 0;          // rename similarity (percent)
    uint32_t mode_head = 0, mode_index = 0, mode_wt = 0;
    Oid oid_head, oid_index;
    bool submodule = false;
    std::string sub_state = "N...";
    uint32_t stage_mode[4] = {0, 0, 0, 0};  // unmerged: modes/ids of stages 1-3
    Oid stage_oid[4];
};

struct StatusOptions {
    enum class Untracked { No, Normal, All } untracked = Untracked::Normal;
    bool untracked_set = false;  // false: take status.showUntrackedFiles
    bool ignored = false;
    bool renames = true;
    bool ahead_behind = true;  // count ahead/behind the upstream (a history walk)
    std::vector<std::string> pathspec;  // worktree-relative prefixes
};

struct BranchInfo {
    bool detached = false, unborn = false;
    std::string head_ref;   // refs/heads/x
    std::string branch;     // x
    Oid head;
    std::string upstream;   // short name, e.g. origin/main
    bool has_upstream = false, gone = false;
    int ahead = 0, behind = 0;
};

struct StatusResult {
    std::vector<StatusEntry> changes;  // sorted by path
    std::vector<std::string> untracked, ignored;
    BranchInfo branch;
};

bool GetBranchInfo(Repo &repo, BranchInfo &out, bool with_ab = true);
bool Status(Repo &repo, const StatusOptions &opts, StatusResult &out, std::string *err,
            const std::atomic<bool> *cancelled = nullptr);
// Untracked (and with `ignored`, ignored) paths, git's own collapsing of
// wholly-untracked directories into "dir/" unless `all`.
void ListUntracked(Repo &repo, const Index &idx, bool all, bool want_ignored, const std::vector<std::string> &pathspec,
                   std::vector<std::string> &untracked, std::vector<std::string> *ignored,
                   const std::atomic<bool> *cancelled = nullptr);
bool PathspecMatch(const std::vector<std::string> &pathspec, const std::string &path, bool is_dir = false);

// ---------------------------------------------------------------- diff

struct DiffSide {
    std::string path;
    uint32_t mode = 0;  // 0: absent
    Oid oid;            // null for a worktree file not hashed yet
    std::string abs;    // set when the content lives in the worktree
    bool loaded = false;
    std::string data;
    bool dirty = false;  // a submodule with local changes ("Subproject commit X-dirty")
};

struct DiffPair {
    char status = 'M';  // A D M R T
    DiffSide a, b;
    int score = 0;  // rename similarity, percent
    std::string sub_state;  // porcelain v2 "S<c><m><u>" for a submodule
};

// Pairs of changed paths between two flattened trees (path -> entry).
std::vector<DiffPair> DiffMaps(const std::map<std::string, TreeEntry> &a, const std::map<std::string, TreeEntry> &b);
bool DiffTrees(Repo &repo, const Oid &a, const Oid &b, const std::vector<std::string> &pathspec,
               std::vector<DiffPair> &out);
// HEAD-tree (or `tree`) vs index (git diff --cached).
bool DiffTreeIndex(Repo &repo, const Oid &tree, const Index &idx, const std::vector<std::string> &pathspec,
                   std::vector<DiffPair> &out);
// Index vs worktree (git diff).
bool DiffIndexWorktree(Repo &repo, const Index &idx, const std::vector<std::string> &pathspec,
                       std::vector<DiffPair> &out);
void DetectRenames(Repo &repo, std::vector<DiffPair> &pairs, int min_score_percent = 50);
bool LoadSide(Repo &repo, DiffSide &s);

struct DiffOptions {
    int context = 3;
    bool patch = true;
    bool stat = false;
    int stat_width = 80;
};
std::string FormatPatch(Repo &repo, std::vector<DiffPair> &pairs, const DiffOptions &opts);
// git's dense combined diff (`diff --cc`) of a merge commit against all of
// its parents. `num_paths` receives how many paths differ from every parent.
std::string FormatCombined(Repo &repo, const Commit &merge, int &num_paths);
std::string FormatStat(Repo &repo, std::vector<DiffPair> &pairs, const DiffOptions &opts);

// Line diff: hunks of (old_start, old_count, new_start, new_count),
// 0-based starts, git's xdiff conventions after compaction.
struct LineHunk {
    long a0, a_len, b0, b_len;
};
std::vector<LineHunk> DiffLines(const std::vector<std::string_view> &a, const std::vector<std::string_view> &b);
std::vector<std::string_view> SplitLines(std::string_view s);  // keeps '\n'
bool IsBinary(std::string_view s);

// ---------------------------------------------------------------- dates

std::string FormatDateShort(int64_t t, int tz);
std::string FormatDateNormal(int64_t t, int tz);  // "Thu Oct 15 12:00:00 2026 -0700"
std::string FormatDateRelative(int64_t t, int64_t now);
std::string FormatDateIso(int64_t t, int tz);
bool ParseDate(std::string_view s, int64_t &t, int &tz);

// ---------------------------------------------------------------- commands

// Runs a git command line (argv[0] may be "git") in `cwd`, writing what
// the real git would print to `out` / `err` and returning its exit code.
// Commands outside the in-house subset return -2 without doing anything
// (IsSupported() answers that up front); network commands are never
// in-house. Non-fast-forward `merge`, `rebase`, and a `stash apply` that
// needs a real three-way content merge run the git CLI from here (on the
// calling thread).
int RunCommand(const std::vector<std::string> &argv, const std::string &cwd, std::string &out, std::string &err,
               const std::atomic<bool> *cancelled = nullptr);
bool IsSupported(const std::vector<std::string> &argv);
// Blocking subprocess helper (hooks, the CLI fallbacks above).
int RunProcess(const std::vector<std::string> &argv, const std::string &cwd,
               const std::vector<std::pair<std::string, std::string>> &env, std::string *out, std::string *err,
               const std::string *stdin_data = nullptr);

// Replaces the index's copy of `rel` with `content` (hunk staging).
bool StageContent(Repo &repo, const std::string &rel, const std::string &content, std::string *err);

}  // namespace mep::git
