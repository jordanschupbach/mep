#include "project_search.h"

#include "regex.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace mep_search {

namespace {

/**
 * @brief Matches one `[...]` class at glob[gi] (just past the '[') against `c`.
 * @param glob The glob.
 * @param gi Index just past the opening '['; set to just past the closing ']' on return.
 * @param c The character to test.
 * @return True if `c` is in the class (after any `!`/`^` negation).
 */
bool MatchClass(const std::string &glob, size_t &gi, char c) {
    bool negate = false;
    if (gi < glob.size() && (glob[gi] == '!' || glob[gi] == '^')) {
        negate = true;
        gi++;
    }
    bool hit = false;
    bool first = true;
    while (gi < glob.size() && (first || glob[gi] != ']')) {
        first = false;
        char lo = glob[gi];
        if (lo == '\\' && gi + 1 < glob.size()) lo = glob[++gi];
        gi++;
        char hi = lo;
        if (gi + 1 < glob.size() && glob[gi] == '-' && glob[gi + 1] != ']') {
            hi = glob[gi + 1];
            if (hi == '\\' && gi + 2 < glob.size()) hi = glob[++gi + 1];
            gi += 2;
        }
        if (c >= lo && c <= hi) hit = true;
    }
    if (gi < glob.size()) gi++;  // the ']'
    return hit != negate;
}

/**
 * @brief GlobMatch's recursive worker over glob[gi..] and path[si..].
 * @param glob The glob.
 * @param gi Current glob index.
 * @param path The path.
 * @param si Current path index.
 * @return True if the remainders match.
 */
bool GlobAt(const std::string &glob, size_t gi, const std::string &path, size_t si) {
    while (gi < glob.size()) {
        const char g = glob[gi];
        if (g == '*') {
            if (gi + 1 < glob.size() && glob[gi + 1] == '*') {
                size_t after = gi + 2;
                while (after < glob.size() && glob[after] == '*') after++;
                if (after < glob.size() && glob[after] == '/') {
                    // `**/`: zero or more whole directories.
                    after++;
                    if (GlobAt(glob, after, path, si)) return true;
                    for (size_t k = si; k < path.size(); k++) {
                        if (path[k] == '/' && GlobAt(glob, after, path, k + 1)) return true;
                    }
                    return false;
                }
                // `**` elsewhere (typically a trailing `/**`): anything at all.
                for (size_t k = si; k <= path.size(); k++) {
                    if (GlobAt(glob, after, path, k)) return true;
                }
                return false;
            }
            for (size_t k = si;; k++) {
                if (GlobAt(glob, gi + 1, path, k)) return true;
                if (k >= path.size() || path[k] == '/') return false;
            }
        }
        if (si >= path.size()) return false;
        if (g == '?') {
            if (path[si] == '/') return false;
            gi++;
            si++;
            continue;
        }
        if (g == '[') {
            size_t ci = gi + 1;
            if (path[si] == '/' || !MatchClass(glob, ci, path[si])) return false;
            gi = ci;
            si++;
            continue;
        }
        char lit = g;
        if (g == '\\' && gi + 1 < glob.size()) lit = glob[++gi];
        if (path[si] != lit) return false;
        gi++;
        si++;
    }
    return si == path.size();
}

// One line of a .gitignore (or --glob), already split into its parts.
struct Rule {
    std::string base;  // absolute dir the pattern is relative to ("" for a --glob)
    std::string glob;
    bool negate = false;
    bool dir_only = false;
    bool anchored = false;  // contains a '/' (other than a trailing one): match the relative path, not the name
};

/**
 * @brief Parses one gitignore line into a Rule.
 * @param line The raw line.
 * @param base The directory the file lives in.
 * @param out Filled on success.
 * @return False for a blank line or comment.
 */
bool ParseRule(std::string line, const std::string &base, Rule &out) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // Trailing spaces are ignored unless escaped.
    while (!line.empty() && line.back() == ' ' && !(line.size() >= 2 && line[line.size() - 2] == '\\')) line.pop_back();
    if (line.empty() || line[0] == '#') return false;
    Rule r;
    r.base = base;
    if (line[0] == '!') {
        r.negate = true;
        line.erase(0, 1);
    } else if (line[0] == '\\' && line.size() > 1 && (line[1] == '!' || line[1] == '#')) {
        line.erase(0, 1);
    }
    if (!line.empty() && line.back() == '/') {
        r.dir_only = true;
        line.pop_back();
    }
    if (line.empty()) return false;
    if (line.find('/') != std::string::npos) r.anchored = true;
    if (line[0] == '/') line.erase(0, 1);
    r.glob = line;
    out = std::move(r);
    return true;
}

/**
 * @brief Appends every rule in an ignore file to `rules`, if the file exists.
 * @param file Path of the ignore file.
 * @param base Directory its patterns are relative to.
 * @param rules Rule list to append to.
 */
void LoadIgnoreFile(const fs::path &file, const std::string &base, std::vector<Rule> &rules) {
    std::ifstream in(file);
    if (!in) return;
    std::string line;
    Rule r;
    while (std::getline(in, line)) {
        if (ParseRule(line, base, r)) rules.push_back(r);
    }
}

/**
 * @brief Tests a single rule against an entry.
 * @param r The rule.
 * @param abs Absolute, '/'-separated path of the entry.
 * @param name The entry's own name.
 * @param is_dir Whether it is a directory.
 * @return True if the rule's pattern applies to this entry.
 */
bool RuleMatches(const Rule &r, const std::string &abs, const std::string &name, bool is_dir) {
    if (r.dir_only && !is_dir) return false;
    if (!r.anchored) return GlobMatch(r.glob, name);
    if (abs.size() <= r.base.size() || abs.compare(0, r.base.size(), r.base) != 0) return false;
    size_t start = r.base.size();
    if (abs[start] == '/') start++;
    return GlobMatch(r.glob, abs.substr(start));
}

/**
 * @brief Gitignore precedence: the last matching rule decides.
 * @param rules Rules, lowest precedence first.
 * @param abs Absolute path of the entry.
 * @param name The entry's name.
 * @param is_dir Whether it is a directory.
 * @return True if the entry is ignored.
 */
bool IsIgnored(const std::vector<Rule> &rules, const std::string &abs, const std::string &name, bool is_dir) {
    for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
        if (RuleMatches(*it, abs, name, is_dir)) return !it->negate;
    }
    return false;
}

/**
 * @brief Whether a pattern can be searched as a plain substring.
 * @param p The pattern.
 * @return True if it contains no regex metacharacter.
 */
bool IsLiteral(const std::string &p) {
    return p.find_first_of("\\.^$|?*+()[]{}") == std::string::npos;
}

/**
 * @brief Finds the end of the group or class opening at pattern[i].
 * @param p The pattern.
 * @param i Index of a '(' or '['.
 * @return Index of its closing ')' or ']' (p.size() if unclosed).
 */
size_t SkipGroupOrClass(const std::string &p, size_t i) {
    int depth = 0;
    bool in_class = false;
    for (; i < p.size(); i++) {
        const char c = p[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (in_class) {
            if (c == ']') {
                in_class = false;
                if (depth == 0) return i;
            }
            continue;
        }
        if (c == '[') {
            in_class = true;
            // A ']' first in the class (after any '^') is a literal member.
            if (i + 1 < p.size() && p[i + 1] == '^') i++;
            if (i + 1 < p.size() && p[i + 1] == ']') i++;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && --depth == 0) {
            return i;
        }
    }
    return p.size();
}

/**
 * @brief The longest literal run a match of one alternative must contain.
 * @param branch One top-level alternative of a pattern.
 * @return That run; empty when nothing literal is required.
 */
std::string RequiredRun(const std::string &branch) {
    std::string best, run;
    bool last_literal = false;  // the previous atom is run's final character
    auto end_run = [&] {
        if (run.size() > best.size()) best = run;
        run.clear();
        last_literal = false;
    };
    for (size_t i = 0; i < branch.size(); i++) {
        const char c = branch[i];
        if (c == '*' || c == '?' || c == '{') {
            // The atom just appended was optional (or of uncertain count):
            // take it back off. `{n}` with n >= 1 would be safe to keep,
            // but dropping it only shortens the prefilter, never breaks it.
            if (last_literal) run.pop_back();
            end_run();
            if (c == '{') {
                while (i < branch.size() && branch[i] != '}') i++;
            }
            if (i + 1 < branch.size() && branch[i + 1] == '?') i++;  // lazy
            continue;
        }
        if (c == '+') {
            end_run();  // the atom is still required once; the run cannot continue past it
            if (i + 1 < branch.size() && branch[i + 1] == '?') i++;
            continue;
        }
        if (c == '(' || c == '[') {
            // A group or class: skip it whole (its contents may be optional
            // or alternatives), and the run cannot span it.
            end_run();
            i = SkipGroupOrClass(branch, i);
            continue;
        }
        if (c == '.' || c == '^' || c == '$') {
            end_run();
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= branch.size()) break;
            const char e = branch[++i];
            if (std::isalnum(static_cast<unsigned char>(e))) {
                end_run();  // \d \w \b ... (or an escape this analysis does not know)
                continue;
            }
            run += e;
            last_literal = true;
            continue;
        }
        run += c;
        last_literal = true;
    }
    end_run();
    return best;
}

/**
 * @brief Literals at least one of which every match of `pattern` contains:
 * one per top-level alternative. A cheap substring prefilter in front of
 * the backtracking regex, which is far too slow to run on every line.
 * @param pattern The regex.
 * @return The literals, or empty when some alternative requires none.
 */
std::vector<std::string> RequiredLiterals(const std::string &pattern) {
    std::vector<std::string> out;
    int depth = 0;
    bool in_class = false;
    size_t start = 0;
    for (size_t i = 0; i <= pattern.size(); i++) {
        if (i < pattern.size()) {
            const char c = pattern[i];
            if (c == '\\') {
                i++;
                continue;
            }
            if (in_class) {
                if (c == ']') in_class = false;
                continue;
            }
            if (c == '[') {
                in_class = true;
                if (i + 1 < pattern.size() && pattern[i + 1] == '^') i++;
                if (i + 1 < pattern.size() && pattern[i + 1] == ']') i++;
                continue;
            }
            if (c == '(') depth++;
            if (c == ')') depth--;
            if (c != '|' || depth != 0) continue;
        }
        std::string lit = RequiredRun(pattern.substr(start, i - start));
        if (lit.empty()) return {};
        out.push_back(std::move(lit));
        start = i + 1;
    }
    return out;
}

/**
 * @brief Whether `hay` contains any of `needles`.
 * @param hay Text to search.
 * @param needles Substrings.
 * @return True on the first one found.
 */
bool ContainsAny(std::string_view hay, const std::vector<std::string> &needles) {
    for (const std::string &n : needles) {
        if (hay.find(n) != std::string_view::npos) return true;
    }
    return false;
}

/**
 * @brief ASCII-lowercases a string.
 * @param s Input.
 * @return Lowercased copy.
 */
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Lines longer than this are never handed to the regex engine: it is a
// recursive backtracker (regex.h), so its stack use grows with the
// subject's length, and these searches run on a job's background thread,
// whose stack is fixed up front: 64MB on POSIX (job.cpp's Job(Task)),
// several times what 2000 characters were measured to need; Windows has
// only std::thread's default 1MB. Only a minified file or a data blob has
// lines this long; the literal path still searches them.
#if defined(_WIN32)
constexpr size_t kMaxRegexLine = 400;
#else
constexpr size_t kMaxRegexLine = 2000;
#endif
// Backtracking budget per line (Regex::SearchBounded): ample for any
// sane pattern on a line under kMaxRegexLine, while a catastrophic one
// gives up on that line in milliseconds instead of hanging the search
// (and, with it, the job a later keystroke is trying to kill).
constexpr size_t kMaxRegexSteps = 2000000;
constexpr size_t kBinaryProbe = 64 * 1024;
constexpr uintmax_t kMaxFileSize = 64ull * 1024 * 1024;

class Searcher {
public:
    Searcher(const Options &opts, const std::function<void(const std::string &)> &emit,
             const std::atomic<bool> &cancelled)
        : opts_(opts), emit_(emit), cancelled_(cancelled) {}

    /**
     * @brief Compiles the pattern and the --glob rules.
     * @return False if the pattern does not compile.
     */
    bool Init() {
        for (const std::string &g : opts_.globs) {
            Rule r;
            std::string s = g;
            if (!s.empty() && s[0] == '!') {
                r.negate = true;
                s.erase(0, 1);
            }
            if (!s.empty() && s.back() == '/') {
                r.dir_only = true;
                s.pop_back();
            }
            if (s.empty()) continue;
            r.anchored = s.find('/') != std::string::npos;
            if (s[0] == '/') s.erase(0, 1);
            r.glob = s;
            (r.negate ? excludes_ : whitelist_).push_back(r);
        }
        if (opts_.files_only) return true;
        fold_ = opts_.ignore_case;
        if (opts_.smart_case && !opts_.ignore_case) {
            fold_ = std::none_of(opts_.pattern.begin(), opts_.pattern.end(),
                                 [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
        }
        literal_ = opts_.fixed_strings || IsLiteral(opts_.pattern);
        if (literal_) {
            prefilter_ = {fold_ ? Lower(opts_.pattern) : opts_.pattern};
            return true;
        }
        regex_ = std::make_unique<mep_regex::Regex>(opts_.pattern, fold_);
        if (!regex_->ok()) return false;
        prefilter_ = RequiredLiterals(opts_.pattern);
        if (fold_) {
            for (std::string &l : prefilter_) l = Lower(l);
        }
        return true;
    }

    /**
     * @brief Searches one path argument (file or directory).
     * @param shown How the path is printed ("" for the default cwd).
     * @param abs Its absolute location.
     */
    void SearchRoot(const std::string &shown, const fs::path &abs) {
        std::error_code ec;
        fs::file_status st = fs::status(abs, ec);
        if (ec) return;
        if (fs::is_regular_file(st)) {
            // An explicitly named file is searched whatever ignore rules say, as with rg.
            HandleFile(shown, abs);
            return;
        }
        if (!fs::is_directory(st)) return;
        std::vector<Rule> rules;
        const std::string abs_str = abs.lexically_normal().generic_string();
        std::string root = abs_str;
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        if (!opts_.no_ignore) LoadParentRules(fs::path(root), rules);
        Walk(shown, fs::path(root), rules);
    }

    bool matched() const { return matched_; }

private:
    const Options &opts_;
    const std::function<void(const std::string &)> &emit_;
    const std::atomic<bool> &cancelled_;
    std::vector<Rule> excludes_, whitelist_;
    std::unique_ptr<mep_regex::Regex> regex_;
    // Substrings at least one of which every matching line contains (the
    // whole pattern, for a literal search), lowercased under case folding;
    // empty when the regex has none to offer. Checked against the whole
    // file first, then per line, so the regex itself only ever runs on
    // candidate lines.
    std::vector<std::string> prefilter_;
    std::string folded_;  // the file, lowercased, when fold_
    bool literal_ = false;
    bool fold_ = false;
    bool matched_ = false;
    bool in_git_repo_ = false;

    /**
     * @brief Loads the ignore rules that apply to `root` from above it: the
     * global excludes file, .git/info/exclude, and every .gitignore from
     * the repository root down to (not including) `root` itself.
     * @param root The directory being searched.
     * @param rules Rule list to fill.
     */
    void LoadParentRules(const fs::path &root, std::vector<Rule> &rules) {
        std::error_code ec;
        fs::path git_root;
        for (fs::path p = root;; p = p.parent_path()) {
            if (fs::exists(p / ".git", ec)) {
                git_root = p;
                break;
            }
            if (p == p.parent_path() || p.empty()) break;
        }
        if (git_root.empty()) return;
        in_git_repo_ = true;
        const std::string base = git_root.generic_string();
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        if (xdg && *xdg) {
            LoadIgnoreFile(fs::path(xdg) / "git" / "ignore", base, rules);
        } else if (home && *home) {
            LoadIgnoreFile(fs::path(home) / ".config" / "git" / "ignore", base, rules);
        }
        LoadIgnoreFile(git_root / ".git" / "info" / "exclude", base, rules);
        // .gitignore files strictly above `root` (root's own is loaded by Walk).
        std::vector<fs::path> chain;
        for (fs::path p = root.parent_path(); ; p = p.parent_path()) {
            if (p.generic_string().size() < base.size()) break;
            chain.push_back(p);
            if (p == git_root || p == p.parent_path()) break;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            LoadIgnoreFile(*it / ".gitignore", it->generic_string(), rules);
            LoadIgnoreFile(*it / ".ignore", it->generic_string(), rules);
            LoadIgnoreFile(*it / ".rgignore", it->generic_string(), rules);
        }
    }

    /**
     * @brief Whether a --glob exclusion (or a missing whitelist match) drops this entry.
     * @param name Entry name.
     * @param rel Path relative to the search root.
     * @param is_dir Whether it is a directory.
     * @return True if the entry should be skipped.
     */
    bool GlobExcluded(const std::string &name, const std::string &rel, bool is_dir) const {
        for (const Rule &r : excludes_) {
            if (r.dir_only && !is_dir) continue;
            if (GlobMatch(r.glob, r.anchored ? rel : name)) return true;
        }
        if (!is_dir && !whitelist_.empty()) {
            for (const Rule &r : whitelist_) {
                if (GlobMatch(r.glob, r.anchored ? rel : name)) return false;
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Recursively walks a directory, honoring hidden/ignore/glob filters.
     * @param shown Printed form of `dir` ("" for the default cwd).
     * @param dir Absolute directory path.
     * @param rules Ignore rules in effect; this level's are appended and removed again.
     * @param rel Path of `dir` relative to the search root ("" at the root).
     */
    void Walk(const std::string &shown, const fs::path &dir, std::vector<Rule> &rules, const std::string &rel = "") {
        if (cancelled_.load()) return;
        const size_t rules_before = rules.size();
        const std::string dir_str = dir.generic_string();
        if (!opts_.no_ignore) {
            if (in_git_repo_ || fs::exists(dir / ".git")) {
                in_git_repo_ = true;
                LoadIgnoreFile(dir / ".gitignore", dir_str, rules);
            }
            LoadIgnoreFile(dir / ".ignore", dir_str, rules);
            LoadIgnoreFile(dir / ".rgignore", dir_str, rules);
        }
        struct Entry {
            std::string name;
            bool is_dir;
        };
        std::vector<Entry> entries;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name == ".git") continue;
            if (!opts_.hidden && !name.empty() && name[0] == '.') continue;
            std::error_code sec;
            fs::file_status lst = it->symlink_status(sec);
            if (sec) continue;
            bool is_dir;
            if (fs::is_symlink(lst)) {
                // rg does not follow symlinks by default: a link to a file
                // is searched, a link to a directory is not descended.
                fs::file_status st = it->status(sec);
                if (sec || !fs::is_regular_file(st)) continue;
                is_dir = false;
            } else if (fs::is_directory(lst)) {
                is_dir = true;
            } else if (fs::is_regular_file(lst)) {
                is_dir = false;
            } else {
                continue;
            }
            entries.push_back({std::move(name), is_dir});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
        for (const Entry &e : entries) {
            if (cancelled_.load()) break;
            const std::string abs = dir_str + "/" + e.name;
            const std::string child_rel = rel.empty() ? e.name : rel + "/" + e.name;
            if (GlobExcluded(e.name, child_rel, e.is_dir)) continue;
            if (!opts_.no_ignore && IsIgnored(rules, abs, e.name, e.is_dir)) continue;
            std::string child_shown = shown.empty() ? e.name : (shown.back() == '/' ? shown + e.name : shown + "/" + e.name);
            if (e.is_dir) {
                Walk(child_shown, fs::path(abs), rules, child_rel);
            } else {
                HandleFile(child_shown, fs::path(abs));
            }
        }
        rules.resize(rules_before);
    }

    /**
     * @brief Lists or greps one file.
     * @param shown The file's printed path.
     * @param abs Its absolute path.
     */
    void HandleFile(const std::string &shown, const fs::path &abs) {
        if (opts_.files_only) {
            matched_ = true;
            emit_(shown);
            return;
        }
        std::error_code ec;
        const uintmax_t size = fs::file_size(abs, ec);
        if (ec || size == 0 || size > kMaxFileSize) return;
        std::ifstream in(abs, std::ios::binary);
        if (!in) return;
        std::string data(static_cast<size_t>(size), '\0');
        in.read(&data[0], static_cast<std::streamsize>(size));
        data.resize(static_cast<size_t>(in.gcount()));
        if (std::memchr(data.data(), '\0', std::min(data.size(), kBinaryProbe)) != nullptr) return;

        std::string_view hay = data;
        if (fold_) {
            folded_ = Lower(data);
            hay = folded_;
        }
        if (!prefilter_.empty() && !ContainsAny(hay, prefilter_)) return;

        const std::string prefix = opts_.with_filename ? shown + ":" : std::string();
        size_t pos = 0;
        long lnum = 0;
        std::string line;
        while (pos < data.size()) {
            if ((lnum & 1023) == 0 && cancelled_.load()) return;
            size_t nl = data.find('\n', pos);
            if (nl == std::string::npos) nl = data.size();
            lnum++;
            const size_t line_start = pos;
            pos = nl + 1;
            if (!prefilter_.empty() && !ContainsAny(hay.substr(line_start, nl - line_start), prefilter_)) continue;
            line.assign(data, line_start, nl - line_start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!literal_ && !(line.size() <= kMaxRegexLine && regex_->SearchBounded(line, kMaxRegexSteps).ok())) {
                continue;
            }
            matched_ = true;
            if (opts_.line_number) {
                emit_(prefix + std::to_string(lnum) + ":" + line);
            } else {
                emit_(prefix + line);
            }
        }
    }
};

}  // namespace

bool GlobMatch(const std::string &glob, const std::string &path) { return GlobAt(glob, 0, path, 0); }

std::vector<std::string> PrefilterLiterals(const std::string &pattern) { return RequiredLiterals(pattern); }

std::optional<Options> ParseRgArgs(const std::vector<std::string> &argv) {
    if (argv.empty() || argv[0] != "rg") return std::nullopt;
    Options o;
    std::vector<std::string> positional;
    bool have_pattern = false;
    bool only_positional = false;
    for (size_t i = 1; i < argv.size(); i++) {
        const std::string &a = argv[i];
        if (only_positional || a.empty() || a[0] != '-' || a == "-") {
            positional.push_back(a);
            continue;
        }
        if (a == "--") {
            only_positional = true;
            continue;
        }
        // Flags taking a value, in both `--flag value` and `--flag=value` form.
        auto value = [&](const std::string &name, std::string &out) -> int {
            if (a == name) {
                if (i + 1 >= argv.size()) return -1;
                out = argv[++i];
                return 1;
            }
            if (a.size() > name.size() && a.compare(0, name.size() + 1, name + "=") == 0) {
                out = a.substr(name.size() + 1);
                return 1;
            }
            return 0;
        };
        std::string v;
        int r;
        if ((r = value("--glob", v)) || (r = value("-g", v))) {
            if (r < 0) return std::nullopt;
            o.globs.push_back(v);
            continue;
        }
        if ((r = value("--regexp", v)) || (r = value("-e", v))) {
            if (r < 0 || have_pattern) return std::nullopt;  // multiple -e: not supported
            o.pattern = v;
            have_pattern = true;
            continue;
        }
        if ((r = value("--color", v)) || (r = value("--colors", v))) {
            if (r < 0) return std::nullopt;
            continue;  // never colored: output is a pipe either way
        }
        if (a == "--files") o.files_only = true;
        else if (a == "--hidden") o.hidden = true;
        else if (a == "--no-ignore" || a == "--no-ignore-vcs") o.no_ignore = true;
        else if (a == "--ignore-case") o.ignore_case = true;
        else if (a == "--case-sensitive") o.ignore_case = o.smart_case = false;
        else if (a == "--smart-case") o.smart_case = true;
        else if (a == "--fixed-strings") o.fixed_strings = true;
        else if (a == "--line-number") o.line_number = true;
        else if (a == "--no-line-number") o.line_number = false;
        else if (a == "--with-filename") o.with_filename = o.with_filename_set = true;
        else if (a == "--no-filename") { o.with_filename = false; o.with_filename_set = true; }
        else if (a == "--no-heading" || a == "--no-messages") {}
        else if (a[1] != '-') {
            // A cluster of single-letter switches (-n, -in, -uu, ...).
            for (size_t k = 1; k < a.size(); k++) {
                switch (a[k]) {
                    case 'n': o.line_number = true; break;
                    case 'N': o.line_number = false; break;
                    case 'i': o.ignore_case = true; break;
                    case 's': o.ignore_case = o.smart_case = false; break;
                    case 'S': o.smart_case = true; break;
                    case 'F': o.fixed_strings = true; break;
                    case 'H': o.with_filename = o.with_filename_set = true; break;
                    case 'I': o.with_filename = false; o.with_filename_set = true; break;
                    case '.': o.hidden = true; break;
                    // -u: no ignore files; -uu: plus hidden; -uuu: plus binary (not supported, treated as -uu).
                    case 'u': if (o.no_ignore) o.hidden = true; o.no_ignore = true; break;
                    default: return std::nullopt;
                }
            }
        } else {
            return std::nullopt;
        }
    }
    size_t first_path = 0;
    if (!o.files_only && !have_pattern) {
        if (positional.empty()) return std::nullopt;
        o.pattern = positional[0];
        first_path = 1;
    }
    o.paths.assign(positional.begin() + static_cast<std::ptrdiff_t>(first_path), positional.end());
    // rg reads stdin when given a pattern but no path and stdin is not a
    // tty; that has never been useful through mep.job_start (see live
    // grep's own comment in main.cpp), so search the cwd instead.
    return o;
}

int Run(const Options &opts, const std::string &cwd, const std::function<void(const std::string &)> &emit,
        const std::atomic<bool> &cancelled) {
    Options o = opts;
    std::error_code ec;
    fs::path base = cwd.empty() ? fs::current_path(ec) : fs::path(cwd);
    if (!o.with_filename_set) {
        // rg drops the file name only when its sole argument is one file.
        o.with_filename = !(o.paths.size() == 1 && fs::is_regular_file(base / o.paths[0], ec));
    }
    Searcher s(o, emit, cancelled);
    if (!s.Init()) return 2;
    if (o.paths.empty()) {
        s.SearchRoot("", base);
    } else {
        for (const std::string &p : o.paths) {
            if (cancelled.load()) break;
            fs::path abs = fs::path(p).is_absolute() ? fs::path(p) : base / p;
            s.SearchRoot(p, fs::absolute(abs, ec));
        }
    }
    return s.matched() ? 0 : 1;
}

}  // namespace mep_search
