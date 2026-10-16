// git config files: parsing (with include.path / includeIf), lookup, and
// the two small in-place edits mep needs (set a key, drop a section).

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "git/git.h"
#include "git/util.h"

namespace mep::git {

namespace {
std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ExpandTilde(const std::string &p) {
    if (p.size() >= 1 && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        const char *home = std::getenv("HOME");
        if (home) return std::string(home) + p.substr(1);
    }
    return p;
}

// Parses one config file's text into entries. Returns false on a syntax
// error (entries up to that point are kept, like git's own reader would
// refuse the whole file -- mep just stops there).
struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string section;  // "section" or "section.subsection" (canonical)

    int Peek() const { return i < s.size() ? static_cast<unsigned char>(s[i]) : -1; }
    int Get() { return i < s.size() ? static_cast<unsigned char>(s[i++]) : -1; }

    bool ParseSection() {
        // after '['
        std::string name;
        while (Peek() != -1 && (std::isalnum(Peek()) || Peek() == '-' || Peek() == '.')) name += static_cast<char>(Get());
        if (Peek() == ']') {
            Get();
            // [section.sub] (deprecated): whole thing lowercased
            section = Lower(name);
            return true;
        }
        if (Peek() != ' ' && Peek() != '\t') return false;
        while (Peek() == ' ' || Peek() == '\t') Get();
        if (Get() != '"') return false;
        std::string sub;
        for (;;) {
            int c = Get();
            if (c == -1 || c == '\n') return false;
            if (c == '"') break;
            if (c == '\\') {
                c = Get();
                if (c == -1 || c == '\n') return false;
            }
            sub += static_cast<char>(c);
        }
        if (Get() != ']') return false;
        section = Lower(name) + "." + sub;
        return true;
    }

    bool ParseValue(std::string &out) {
        bool quote = false;
        size_t keep = 0;  // length of out that must survive trailing-space trimming
        out.clear();
        for (;;) {
            int c = Get();
            if (c == -1 || c == '\n') {
                if (quote) return false;
                out.resize(std::max(keep, out.size() - 0));
                while (out.size() > keep && (out.back() == ' ' || out.back() == '\t' || out.back() == '\r'))
                    out.pop_back();
                return true;
            }
            if (!quote && (c == ';' || c == '#')) {
                while (Peek() != -1 && Peek() != '\n') Get();
                continue;
            }
            if (c == '\\') {
                c = Get();
                switch (c) {
                    case '\n': continue;
                    case 't': out += '\t'; break;
                    case 'b': if (!out.empty()) out.pop_back(); break;
                    case 'n': out += '\n'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '\r':
                        if (Peek() == '\n') {
                            Get();
                            continue;
                        }
                        return false;
                    default: return false;
                }
                keep = out.size();
                continue;
            }
            if (c == '"') {
                quote = !quote;
                keep = out.size();
                continue;
            }
            if (!quote && (c == ' ' || c == '\t') && out.size() == keep && out.empty()) continue;  // leading ws
            out += static_cast<char>(c);
            if (quote) keep = out.size();
        }
    }
};
}  // namespace

std::string Config::CanonicalKey(std::string_view key) {
    size_t first = key.find('.');
    size_t last = key.rfind('.');
    if (first == std::string_view::npos) return Lower(std::string(key));
    std::string out = Lower(std::string(key.substr(0, first)));
    if (last != first) out += std::string(key.substr(first, last - first));
    out += Lower(std::string(key.substr(last)));
    return out;
}

bool Config::LoadFile(const std::string &path, int depth) {
    if (depth > 10) return false;
    std::string text;
    if (!ReadFile(path, text)) return false;
    Parser p{text, 0, ""};
    std::string dir = DirName(path);
    while (p.i < text.size()) {
        int c = p.Get();
        if (c == '\n' || c == ' ' || c == '\t' || c == '\r') continue;
        if (c == 0xEF && p.i == 1 && text.size() >= 3 && static_cast<unsigned char>(text[1]) == 0xBB) {
            p.i = 3;  // UTF-8 BOM
            continue;
        }
        if (c == '#' || c == ';') {
            while (p.Peek() != -1 && p.Peek() != '\n') p.Get();
            continue;
        }
        if (c == '[') {
            if (!p.ParseSection()) return false;
            continue;
        }
        if (!std::isalpha(c)) return false;
        std::string name(1, static_cast<char>(std::tolower(c)));
        while (p.Peek() != -1 && (std::isalnum(p.Peek()) || p.Peek() == '-'))
            name += static_cast<char>(std::tolower(p.Get()));
        while (p.Peek() == ' ' || p.Peek() == '\t') p.Get();
        Entry e;
        e.key = p.section + "." + name;
        if (p.Peek() == '=') {
            p.Get();
            if (!p.ParseValue(e.value)) return false;
        } else {
            // bare boolean (or a comment / end of line after the name)
            while (p.Peek() != -1 && p.Peek() != '\n') {
                int d = p.Get();
                if (d == '#' || d == ';') {
                    while (p.Peek() != -1 && p.Peek() != '\n') p.Get();
                    break;
                }
                if (d != ' ' && d != '\t' && d != '\r') return false;
            }
            e.has_value = false;
            e.value = "true";
        }
        if (e.key == "include.path") {
            std::string inc = ExpandTilde(e.value);
            if (!inc.empty() && inc[0] != '/') inc = dir + "/" + inc;
            entries_.push_back(e);
            LoadFile(inc, depth + 1);
            continue;
        }
        if (e.key.rfind("includeif.", 0) == 0 && e.key.size() > 15 &&
            e.key.compare(e.key.size() - 5, 5, ".path") == 0) {
            std::string cond = e.key.substr(10, e.key.size() - 15);
            bool match = false;
            auto gitdir_match = [&](std::string pat, bool icase) {
                pat = ExpandTilde(pat);
                if (pat.rfind("./", 0) == 0) pat = dir + pat.substr(1);
                else if (!pat.empty() && pat[0] != '/') pat = "**/" + pat;
                if (!pat.empty() && pat.back() == '/') pat += "**";
                std::string gd = git_dir_;
                if (icase) {
                    pat = Lower(pat);
                    gd = Lower(gd);
                }
                return WildMatch(pat, gd) || WildMatch(pat, gd + "/");
            };
            if (cond.rfind("gitdir:", 0) == 0) match = gitdir_match(cond.substr(7), false);
            else if (cond.rfind("gitdir/i:", 0) == 0) match = gitdir_match(cond.substr(9), true);
            else if (cond.rfind("onbranch:", 0) == 0 && !git_dir_.empty()) {
                std::string head;
                if (ReadFile(git_dir_ + "/HEAD", head) && head.rfind("ref: refs/heads/", 0) == 0) {
                    std::string br = Trim(head.substr(16));
                    std::string pat = cond.substr(9);
                    if (!pat.empty() && pat.back() == '/') pat += "**";
                    match = WildMatch(pat, br);
                }
            }
            if (match) {
                std::string inc = ExpandTilde(e.value);
                if (!inc.empty() && inc[0] != '/') inc = dir + "/" + inc;
                LoadFile(inc, depth + 1);
            }
            continue;
        }
        entries_.push_back(std::move(e));
    }
    return true;
}

void Config::LoadStandard(const std::string &common_dir, const std::string &git_dir) {
    entries_.clear();
    git_dir_ = git_dir;
    if (!std::getenv("GIT_CONFIG_NOSYSTEM")) {
        if (const char *sys = std::getenv("GIT_CONFIG_SYSTEM")) {
            LoadFile(sys);
        } else {
            LoadFile("/etc/gitconfig");
#if defined(__APPLE__)
            // Apple's git (Xcode / Command Line Tools) keeps its system
            // config inside the toolchain.
            if (!LoadFile("/Library/Developer/CommandLineTools/usr/share/git-core/gitconfig"))
                LoadFile("/Applications/Xcode.app/Contents/Developer/usr/share/git-core/gitconfig");
#endif
        }
    }
    if (const char *g = std::getenv("GIT_CONFIG_GLOBAL")) {
        LoadFile(g);
    } else {
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        const char *home = std::getenv("HOME");
        if (xdg && *xdg) LoadFile(std::string(xdg) + "/git/config");
        else if (home) LoadFile(std::string(home) + "/.config/git/config");
        if (home) LoadFile(std::string(home) + "/.gitconfig");
    }
    if (!common_dir.empty()) {
        LoadFile(common_dir + "/config");
        if (GetBool("extensions.worktreeconfig", false)) LoadFile(git_dir + "/config.worktree");
    }
    if (const char *cnt = std::getenv("GIT_CONFIG_COUNT")) {
        long n = std::strtol(cnt, nullptr, 10);
        for (long k = 0; k < n; k++) {
            const char *key = std::getenv(("GIT_CONFIG_KEY_" + std::to_string(k)).c_str());
            const char *val = std::getenv(("GIT_CONFIG_VALUE_" + std::to_string(k)).c_str());
            if (key && val) entries_.push_back({CanonicalKey(key), val, true});
        }
    }
}

std::optional<std::string> Config::Get(std::string_view key) const {
    std::string k = CanonicalKey(key);
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->key == k) return it->value;
    return std::nullopt;
}

std::vector<std::string> Config::GetAll(std::string_view key) const {
    std::string k = CanonicalKey(key);
    std::vector<std::string> out;
    for (const Entry &e : entries_)
        if (e.key == k) out.push_back(e.value);
    return out;
}

bool Config::GetBool(std::string_view key, bool def) const {
    std::optional<std::string> v = Get(key);
    if (!v) return def;
    std::string l = Lower(*v);
    if (l == "true" || l == "yes" || l == "on" || l == "1") return true;
    if (l == "false" || l == "no" || l == "off" || l == "0" || l.empty()) return false;
    char *end = nullptr;
    long n = std::strtol(l.c_str(), &end, 10);
    if (end && *end == 0) return n != 0;
    return def;
}

int64_t Config::GetInt(std::string_view key, int64_t def) const {
    std::optional<std::string> v = Get(key);
    if (!v || v->empty()) return def;
    char *end = nullptr;
    long long n = std::strtoll(v->c_str(), &end, 10);
    if (end && (*end == 'k' || *end == 'K')) n *= 1024;
    else if (end && (*end == 'm' || *end == 'M')) n *= 1024 * 1024;
    else if (end && (*end == 'g' || *end == 'G')) n *= 1024LL * 1024 * 1024;
    return n;
}

// ------------------------------------------------------------ editing

namespace {
struct KeyParts {
    std::string section, sub, name;
};
KeyParts SplitKey(const std::string &key) {
    KeyParts k;
    size_t first = key.find('.'), last = key.rfind('.');
    k.section = Lower(key.substr(0, first));
    if (last != first) k.sub = key.substr(first + 1, last - first - 1);
    k.name = Lower(key.substr(last + 1));
    return k;
}
// Section header of `line` as (section, sub); empty section if not a header.
bool HeaderOf(const std::string &line, std::string &section, std::string &sub) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size() || line[i] != '[') return false;
    Parser p{line, i + 1, ""};
    if (!p.ParseSection()) return false;
    size_t dot = p.section.find('.');
    section = p.section.substr(0, dot);
    sub = dot == std::string::npos ? "" : p.section.substr(dot + 1);
    return true;
}
std::string QuoteValue(const std::string &v) {
    bool need = !v.empty() && (v.front() == ' ' || v.back() == ' ' || v.front() == '\t' || v.back() == '\t');
    std::string out;
    for (char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            if (c == '#' || c == ';') need = true;
            out += c;
        }
    }
    return need ? "\"" + out + "\"" : out;
}
std::vector<std::string> SplitLinesKeep(const std::string &text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}
}  // namespace

bool ConfigSet(const std::string &file, const std::string &key, const std::string &value, std::string *err) {
    KeyParts k = SplitKey(key);
    std::string text;
    ReadFile(file, text);
    std::vector<std::string> lines = SplitLinesKeep(text);
    long last_in_section = -1, replace = -1;
    bool in_section = false;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string sec, sub;
        if (HeaderOf(lines[i], sec, sub)) {
            in_section = sec == k.section && sub == k.sub;
            if (in_section) last_in_section = static_cast<long>(i);
            continue;
        }
        if (!in_section) continue;
        last_in_section = static_cast<long>(i);
        std::string t = Trim(lines[i]);
        size_t n = 0;
        while (n < t.size() && (std::isalnum(static_cast<unsigned char>(t[n])) || t[n] == '-')) n++;
        if (n && Lower(t.substr(0, n)) == k.name) replace = static_cast<long>(i);
    }
    std::string line = "\t" + k.name + " = " + QuoteValue(value);
    if (replace >= 0) {
        lines[static_cast<size_t>(replace)] = line;
    } else if (last_in_section >= 0) {
        lines.insert(lines.begin() + last_in_section + 1, line);
    } else {
        std::string hdr = "[" + k.section;
        if (!k.sub.empty()) {
            hdr += " \"";
            for (char c : k.sub) {
                if (c == '"' || c == '\\') hdr += '\\';
                hdr += c;
            }
            hdr += "\"";
        }
        hdr += "]";
        lines.push_back(hdr);
        lines.push_back(line);
    }
    std::string out;
    for (const std::string &l : lines) out += l + "\n";
    return WriteFileAtomic(file, out, err);
}

bool ConfigRemoveSection(const std::string &file, const std::string &section, const std::string &subsection) {
    std::string text;
    if (!ReadFile(file, text)) return false;
    std::vector<std::string> lines = SplitLinesKeep(text);
    std::string out;
    bool skipping = false, removed = false;
    for (const std::string &l : lines) {
        std::string sec, sub;
        if (HeaderOf(l, sec, sub)) {
            skipping = sec == Lower(section) && sub == subsection;
            if (skipping) removed = true;
        }
        if (!skipping) out += l + "\n";
    }
    if (!removed) return false;
    return WriteFileAtomic(file, out, nullptr);
}

}  // namespace mep::git
