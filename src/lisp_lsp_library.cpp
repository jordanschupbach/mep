// Project libraries for mep-lisp-lsp; see lisp_lsp_library.h.

#include "lisp_lsp_library.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <system_error>

#include "lisp_reader.h"

namespace fs = std::filesystem;

namespace {

// --- small helpers ---------------------------------------------------------------------------------------

std::string Lower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string Upper(std::string s) {
    for (char &c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

std::string Trim(const std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    lines.push_back(cur);
    return lines;
}

bool FileStamp(const std::string &path, long long *stamp, long long *size_out = nullptr) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return false;
    const auto size = fs::file_size(path, ec);
    if (ec) return false;
    *stamp = static_cast<long long>(t.time_since_epoch().count()) ^ (static_cast<long long>(size) << 1);
    if (size_out != nullptr) *size_out = static_cast<long long>(size);
    return true;
}

bool ReadAll(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

/** @brief A package or system designator's canonical name: `:foo`, `#:foo`, `"FOO"`, `foo`, `'foo`. */
std::string DesignatorName(const LispReadResult &r, int node) {
    const LispNode &n = r.at(node);
    if (n.kind == LispNodeKind::Quote && !n.kids.empty()) return DesignatorName(r, n.kids[0]);
    if (n.kind == LispNodeKind::Symbol || n.kind == LispNodeKind::String) return n.name;
    return std::string();
}

/** @brief The operator name of a list form, ignoring its package (`asdf:defsystem` -> "DEFSYSTEM"), or "". */
std::string HeadName(const LispReadResult &r, const LispNode &f) {
    if (f.kind != LispNodeKind::List || f.kids.empty()) return std::string();
    const LispNode &h = r.at(f.kids[0]);
    return h.kind == LispNodeKind::Symbol ? h.name : std::string();
}

/** @brief The source text of a range, lines joined with single spaces. */
std::string SourceText(const std::vector<std::string> &lines, const LispRange &rg, size_t limit) {
    std::string out;
    for (int l = rg.line; l <= rg.end_line && l >= 0 && static_cast<size_t>(l) < lines.size(); l++) {
        const std::string &t = lines[static_cast<size_t>(l)];
        const size_t from = l == rg.line ? std::min(t.size(), static_cast<size_t>(rg.col)) : 0;
        const size_t to = l == rg.end_line ? std::min(t.size(), static_cast<size_t>(rg.end_col)) : t.size();
        std::string piece = to > from ? t.substr(from, to - from) : std::string();
        if (l != rg.line) {
            piece = Trim(piece);
            if (!piece.empty()) out += ' ';
        }
        out += piece;
        if (out.size() > limit) {
            out = out.substr(0, limit) + " ...";
            break;
        }
    }
    return out;
}

/** @brief The text inside a list's parentheses. */
std::string InnerText(const std::vector<std::string> &lines, const LispNode &list) {
    std::string t = SourceText(lines, list.range, 4000);
    if (t.size() >= 2 && t.front() == '(' && t.back() == ')') t = t.substr(1, t.size() - 2);
    return Trim(t);
}

const char kArrow[] = "\xe2\x86\x92";  // →

// --- source indexing -------------------------------------------------------------------------------------

class SourceIndexer {
public:
    SourceIndexer(const std::vector<std::string> &lines, LispLibFile *out) : lines_(lines), out_(out) {}

    void Run() {
        r_ = LispRead(lines_);
        for (int top : r_.top) Form(top, 0);
    }

private:
    const std::vector<std::string> &lines_;
    LispLibFile *out_;
    LispReadResult r_;
    std::string current_ = "COMMON-LISP-USER";

    LispLibPackage *FindPackage(const std::string &name) {
        for (LispLibPackage &p : out_->packages) {
            if (p.name == name) return &p;
            for (const std::string &n : p.nicknames) {
                if (n == name) return &p;
            }
        }
        return nullptr;
    }

    void Form(int node, int depth) {
        const LispNode &f = r_.at(node);
        if (f.conditional || f.kind != LispNodeKind::List || f.kids.empty() || depth > 4) return;
        const std::string op = HeadName(r_, f);
        if (op.empty()) return;
        if (op == "PROGN" || op == "EVAL-WHEN") {
            for (size_t i = op == "PROGN" ? 1 : 2; i < f.kids.size(); i++) Form(f.kids[i], depth + 1);
        } else if (op == "IN-PACKAGE" && f.kids.size() > 1) {
            const std::string name = DesignatorName(r_, f.kids[1]);
            if (!name.empty()) current_ = name == "CL-USER" ? "COMMON-LISP-USER" : name;
        } else if (op == "DEFPACKAGE" || op == "DEFINE-PACKAGE") {
            DefPackage(f);
        } else if (op == "EXPORT" && f.kids.size() > 1) {
            Export(f);
        } else if (op == "DEFUN" || op == "DEFMACRO") {
            DefFunction(f, op == "DEFUN" ? LispLibKind::Function : LispLibKind::Macro);
        } else if (op == "DEFGENERIC") {
            DefGeneric(f);
        } else if (op == "DEFMETHOD") {
            DefMethod(f, 1, nullptr);
        } else if (op == "DEFCLASS" || op == "DEFINE-CONDITION") {
            DefClass(f, op == "DEFCLASS" ? LispLibKind::Class : LispLibKind::Condition);
        } else if (op == "DEFSTRUCT") {
            DefStruct(f);
        } else if (op == "DEFTYPE") {
            DefType(f);
        } else if (op == "DEFCONSTANT" || op == "DEFVAR" || op == "DEFPARAMETER") {
            DefVariable(f, op == "DEFCONSTANT" ? LispLibKind::Constant : LispLibKind::Variable);
        }
    }

    void DefPackage(const LispNode &f) {
        if (f.kids.size() < 2) return;
        LispLibPackage p;
        p.name = DesignatorName(r_, f.kids[1]);
        if (p.name.empty()) return;
        p.line = r_.at(f.kids[1]).range.line;
        for (size_t i = 2; i < f.kids.size(); i++) {
            const LispNode &opt = r_.at(f.kids[i]);
            if (opt.kind != LispNodeKind::List || opt.kids.empty()) continue;
            const std::string o = r_.at(opt.kids[0]).name;
            for (size_t j = 1; j < opt.kids.size(); j++) {
                const std::string v = DesignatorName(r_, opt.kids[j]);
                if (o == "DOCUMENTATION" && r_.at(opt.kids[j]).kind == LispNodeKind::String) p.doc = v;
                if (v.empty()) continue;
                if (o == "NICKNAMES") p.nicknames.push_back(v);
                if (o == "USE") p.uses.push_back(v);
                if (o == "EXPORT") p.exports.insert(v);
            }
        }
        if (LispLibPackage *old = FindPackage(p.name)) {
            *old = p;  // a redefinition replaces it, as DEFPACKAGE does
        } else {
            out_->packages.push_back(p);
        }
    }

    void Export(const LispNode &f) {
        std::string pkg = current_;
        if (f.kids.size() > 2) {
            const std::string named = DesignatorName(r_, f.kids[2]);
            if (!named.empty()) pkg = named;
        }
        LispLibPackage *p = FindPackage(pkg);
        if (p == nullptr) return;
        const LispNode &arg = r_.at(f.kids[1]);
        if (arg.kind != LispNodeKind::Quote || arg.kids.empty()) return;
        const LispNode &v = r_.at(arg.kids[0]);
        if (v.kind == LispNodeKind::Symbol) p->exports.insert(v.name);
        if (v.kind == LispNodeKind::List) {
            for (int k : v.kids) {
                if (r_.at(k).kind == LispNodeKind::Symbol) p->exports.insert(r_.at(k).name);
            }
        }
    }

    /** @brief Fills a definition's name and package from a name node (`foo`, `pkg:foo`, `(setf pkg:foo)`). */
    bool Name(int node, LispLibDef *d) {
        const LispNode &n = r_.at(node);
        const LispNode *sym = &n;
        bool setf = false;
        if (n.kind == LispNodeKind::List && n.kids.size() == 2 && LispIsSymbolNamed(r_.at(n.kids[0]), "SETF")) {
            sym = &r_.at(n.kids[1]);
            setf = true;
        }
        if (sym->kind != LispNodeKind::Symbol || sym->uninterned || sym->name.empty()) return false;
        d->name = setf ? "(SETF " + sym->name + ")" : sym->name;
        d->package = sym->package.empty() ? current_ : sym->package;
        if (d->package == "CL-USER") d->package = "COMMON-LISP-USER";
        d->line = sym->range.line;
        d->col = sym->range.col;
        d->end_col = sym->range.end_line == sym->range.line ? sym->range.end_col : sym->range.col + 1;
        return true;
    }

    /** @brief The docstring of a body starting at kid `from`: a string followed by more forms, past declarations. */
    std::string BodyDoc(const LispNode &f, size_t from) {
        for (size_t i = from; i < f.kids.size(); i++) {
            const LispNode &k = r_.at(f.kids[i]);
            if (k.kind == LispNodeKind::String) return i + 1 < f.kids.size() ? k.name : std::string();
            if (k.kind == LispNodeKind::List && !k.kids.empty() && LispIsSymbolNamed(r_.at(k.kids[0]), "DECLARE")) {
                continue;
            }
            break;
        }
        return std::string();
    }

    void Push(LispLibDef d) {
        if (d.kind == LispLibKind::Function || d.kind == LispLibKind::Macro) {
            // The class a constructor or factory returns, from its first signature line.
            for (const std::string &line : SplitLines(d.doc)) {
                size_t at = line.find(kArrow);
                size_t skip = sizeof(kArrow) - 1;
                if (at == std::string::npos) {
                    at = line.find("->");
                    skip = 2;
                }
                if (at == std::string::npos || Trim(line).rfind('(', 0) != 0) continue;
                std::string word = Trim(line.substr(at + skip));
                const size_t end = word.find_first_of(" \t,;");
                if (end != std::string::npos) word = word.substr(0, end);
                d.returns = Upper(word);
                break;
            }
        }
        out_->defs.push_back(std::move(d));
    }

    void DefFunction(const LispNode &f, LispLibKind kind) {
        if (f.kids.size() < 3) return;
        LispLibDef d;
        d.kind = kind;
        if (!Name(f.kids[1], &d)) return;
        const LispNode &ll = r_.at(f.kids[2]);
        if (ll.kind == LispNodeKind::List) d.lambda_list = InnerText(lines_, ll);
        d.doc = BodyDoc(f, 3);
        Push(std::move(d));
    }

    void DefGeneric(const LispNode &f) {
        if (f.kids.size() < 3) return;
        LispLibDef d;
        d.kind = LispLibKind::Generic;
        if (!Name(f.kids[1], &d)) return;
        const LispNode &ll = r_.at(f.kids[2]);
        if (ll.kind == LispNodeKind::List) d.lambda_list = InnerText(lines_, ll);
        std::vector<int> methods;
        for (size_t i = 3; i < f.kids.size(); i++) {
            const LispNode &opt = r_.at(f.kids[i]);
            if (opt.kind != LispNodeKind::List || opt.kids.size() < 2) continue;
            const LispNode &o = r_.at(opt.kids[0]);
            if (!LispIsKeyword(o)) continue;
            if (o.name == "DOCUMENTATION" && r_.at(opt.kids[1]).kind == LispNodeKind::String) d.doc = r_.at(opt.kids[1]).name;
            if (o.name == "METHOD") methods.push_back(f.kids[i]);
        }
        const LispLibDef generic = d;
        Push(std::move(d));
        for (int m : methods) DefMethod(r_.at(m), 0, &generic);
    }

    /** @brief A defmethod (`name_at` = 1) or a defgeneric's `(:method ...)` option (`name_at` = 0). */
    void DefMethod(const LispNode &f, size_t name_at, const LispLibDef *generic) {
        LispLibDef d;
        d.kind = LispLibKind::Method;
        if (generic != nullptr) {
            d.name = generic->name;
            d.package = generic->package;
            d.line = r_.at(f.kids[0]).range.line;
            d.col = r_.at(f.kids[0]).range.col;
            d.end_col = r_.at(f.kids[0]).range.end_col;
        } else if (f.kids.size() < 3 || !Name(f.kids[name_at], &d)) {
            return;
        }
        size_t i = name_at + 1;
        while (i < f.kids.size() && r_.at(f.kids[i]).kind != LispNodeKind::List) i++;  // qualifiers
        if (i >= f.kids.size()) return;
        const LispNode &ll = r_.at(f.kids[i]);
        d.lambda_list = InnerText(lines_, ll);
        for (int p : ll.kids) {
            const LispNode &param = r_.at(p);
            if (param.kind == LispNodeKind::Symbol && !param.name.empty() && param.name[0] == '&') break;
            if (param.kind != LispNodeKind::List || param.kids.size() != 2) continue;
            const LispNode &spec = r_.at(param.kids[1]);
            if (spec.kind == LispNodeKind::Symbol) {
                d.specializer = spec.name;
                break;
            }
        }
        d.doc = BodyDoc(f, i + 1);
        Push(std::move(d));
    }

    void DefClass(const LispNode &f, LispLibKind kind) {
        if (f.kids.size() < 3) return;
        LispLibDef d;
        d.kind = kind;
        if (!Name(f.kids[1], &d)) return;
        const LispNode &supers = r_.at(f.kids[2]);
        if (supers.kind == LispNodeKind::List) {
            for (int s : supers.kids) {
                if (r_.at(s).kind == LispNodeKind::Symbol) d.supers.push_back(r_.at(s).name);
            }
        }
        for (size_t i = 4; i < f.kids.size(); i++) {
            const LispNode &opt = r_.at(f.kids[i]);
            if (opt.kind == LispNodeKind::List && opt.kids.size() >= 2 &&
                r_.at(opt.kids[0]).name == "DOCUMENTATION" && r_.at(opt.kids[1]).kind == LispNodeKind::String) {
                d.doc = r_.at(opt.kids[1]).name;
            }
        }
        const std::string class_name = d.name;
        const std::string class_pkg = d.package;
        Push(std::move(d));
        // Slot readers and accessors.
        if (f.kids.size() < 4 || r_.at(f.kids[3]).kind != LispNodeKind::List) return;
        for (int s : r_.at(f.kids[3]).kids) {
            const LispNode &slot = r_.at(s);
            if (slot.kind != LispNodeKind::List) continue;
            std::string slot_doc;
            std::vector<int> names;
            for (size_t j = 1; j + 1 < slot.kids.size(); j += 2) {
                const std::string key = r_.at(slot.kids[j]).name;
                if (key == "DOCUMENTATION" && r_.at(slot.kids[j + 1]).kind == LispNodeKind::String) {
                    slot_doc = r_.at(slot.kids[j + 1]).name;
                }
                if (key == "READER" || key == "ACCESSOR") names.push_back(slot.kids[j + 1]);
            }
            for (int n : names) {
                LispLibDef a;
                a.kind = LispLibKind::Accessor;
                if (!Name(n, &a)) continue;
                a.lambda_list = "object";
                a.specializer = class_name;
                a.doc = slot_doc;
                (void)class_pkg;
                Push(std::move(a));
            }
        }
    }

    void DefStruct(const LispNode &f) {
        if (f.kids.size() < 2) return;
        LispLibDef d;
        d.kind = LispLibKind::Struct;
        const LispNode &n = r_.at(f.kids[1]);
        if (!Name(n.kind == LispNodeKind::List && !n.kids.empty() ? n.kids[0] : f.kids[1], &d)) return;
        if (f.kids.size() > 2 && r_.at(f.kids[2]).kind == LispNodeKind::String) d.doc = r_.at(f.kids[2]).name;
        Push(std::move(d));
    }

    void DefType(const LispNode &f) {
        if (f.kids.size() < 3) return;
        LispLibDef d;
        d.kind = LispLibKind::Type;
        if (!Name(f.kids[1], &d)) return;
        const LispNode &ll = r_.at(f.kids[2]);
        if (ll.kind == LispNodeKind::List) d.lambda_list = InnerText(lines_, ll);
        d.doc = BodyDoc(f, 3);
        if (f.kids.size() > 3) d.value = SourceText(lines_, r_.at(f.kids.back()).range, 300);
        Push(std::move(d));
    }

    void DefVariable(const LispNode &f, LispLibKind kind) {
        if (f.kids.size() < 2) return;
        LispLibDef d;
        d.kind = kind;
        if (!Name(f.kids[1], &d)) return;
        if (f.kids.size() > 2) d.value = SourceText(lines_, r_.at(f.kids[2]).range, 120);
        if (f.kids.size() > 3 && r_.at(f.kids[3]).kind == LispNodeKind::String) d.doc = r_.at(f.kids[3]).name;
        Push(std::move(d));
    }
};

// --- .asd files ----------------------------------------------------------------------------------------------

/** @brief Collects the .lisp files of a :components list under `dir`. */
void Components(const LispReadResult &r, int list, const fs::path &dir, std::vector<std::string> *out, int depth) {
    const LispNode &l = r.at(list);
    if (l.kind == LispNodeKind::Quote && !l.kids.empty()) {
        Components(r, l.kids[0], dir, out, depth);
        return;
    }
    if (l.kind != LispNodeKind::List || depth > 8) return;
    for (int c : l.kids) {
        const LispNode &comp = r.at(c);
        if (comp.kind != LispNodeKind::List || comp.kids.size() < 2) continue;
        const std::string type = r.at(comp.kids[0]).name;
        std::string name = DesignatorName(r, comp.kids[1]);
        if (r.at(comp.kids[1]).kind == LispNodeKind::Symbol) name = Lower(name);
        std::string pathname;
        int sub = -1;
        for (size_t i = 2; i + 1 < comp.kids.size(); i += 2) {
            const std::string key = r.at(comp.kids[i]).name;
            if (key == "PATHNAME" && r.at(comp.kids[i + 1]).kind == LispNodeKind::String) {
                pathname = r.at(comp.kids[i + 1]).name;
            }
            if (key == "COMPONENTS") sub = comp.kids[i + 1];
        }
        if (type == "FILE" || type == "CL-SOURCE-FILE") {
            std::string file = pathname.empty() ? name : pathname;
            if (fs::path(file).extension() != ".lisp") file += ".lisp";
            out->push_back((dir / file).lexically_normal().string());
        } else if (type == "MODULE" && sub >= 0) {
            Components(r, sub, dir / (pathname.empty() ? name : pathname), out, depth + 1);
        }
    }
}

// --- caches ----------------------------------------------------------------------------------------------

std::mutex g_mutex;

struct CachedFile {
    long long stamp = 0;
    std::shared_ptr<const LispLibFile> file;
};
std::map<std::string, CachedFile> g_files;

struct CachedAsd {
    long long stamp = 0;
    std::vector<LispLibSystem> systems;
};
std::map<std::string, CachedAsd> g_asds;

using Clock = std::chrono::steady_clock;

struct CachedScan {
    Clock::time_point when;
    std::vector<std::string> paths;
};
std::map<std::string, CachedScan> g_scans;

struct CachedView {
    std::vector<const LispLibFile *> key;
    std::shared_ptr<const LispLibrary> view;
};
std::map<std::string, CachedView> g_views;

constexpr size_t kMaxAsdFiles = 256;
constexpr size_t kMaxScanEntries = 40000;
constexpr size_t kMaxIndexedFiles = 64;
constexpr long long kMaxFileBytes = 64LL << 20;

bool SkipDir(const std::string &name) {
    if (name.empty() || name[0] == '.') return true;  // .git, .venv, .direnv, .cache ...
    if (name.rfind("build", 0) == 0 || name.rfind("result", 0) == 0) return true;
    return name == "node_modules" || name == "target" || name == "_build" || name == "__pycache__";
}

/** @brief Every file with extension `ext` under `root`, at most `max_depth` directories down, bounded. */
void Walk(const fs::path &root, int max_depth, const std::string &ext, std::vector<std::string> *out, size_t *budget,
          bool skip_dirs) {
    std::vector<std::pair<fs::path, int>> stack{{root, 0}};
    while (!stack.empty() && out->size() < kMaxAsdFiles) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        std::vector<fs::path> subdirs;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec || *budget == 0) break;
            (*budget)--;
            const fs::directory_entry &e = *it;
            const std::string name = e.path().filename().string();
            std::error_code ec2;
            if (e.is_directory(ec2)) {
                if (depth < max_depth && !(skip_dirs && SkipDir(name)) && !e.is_symlink(ec2)) subdirs.push_back(e.path());
            } else if (e.path().extension() == ext) {
                out->push_back(e.path().lexically_normal().string());
            }
        }
        std::sort(subdirs.begin(), subdirs.end());
        for (auto s = subdirs.rbegin(); s != subdirs.rend(); ++s) stack.push_back({*s, depth + 1});
    }
}

/** @brief Walk() through a short-lived cache: a directory tree is searched at most every few seconds. */
std::vector<std::string> CachedWalk(const fs::path &root, int depth, bool skip_dirs, int ttl_seconds) {
    const std::string key = root.string() + "|" + std::to_string(depth) + (skip_dirs ? "s" : "a");
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_scans.find(key);
        if (it != g_scans.end() && Clock::now() - it->second.when < std::chrono::seconds(ttl_seconds)) {
            return it->second.paths;
        }
    }
    std::vector<std::string> paths;
    size_t budget = kMaxScanEntries;
    Walk(root, depth, ".asd", &paths, &budget, skip_dirs);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_scans[key] = {Clock::now(), paths};
    return paths;
}

std::vector<std::string> EnvList(const char *var) {
    std::vector<std::string> out;
    const char *env = std::getenv(var);
    if (env == nullptr) return out;
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string cur;
    for (const char *p = env;; p++) {
        if (*p == sep || *p == 0) {
            if (!cur.empty()) {
                if (cur[0] == '~') {
                    if (const char *home = std::getenv("HOME")) cur = home + cur.substr(1);
                }
                out.push_back(cur);
            }
            cur.clear();
            if (*p == 0) break;
        } else {
            cur += *p;
        }
    }
    return out;
}

std::vector<LispLibSystem> SystemsOf(const std::string &asd) {
    long long stamp = 0;
    if (!FileStamp(asd, &stamp)) return {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_asds.find(asd);
        if (it != g_asds.end() && it->second.stamp == stamp) return it->second.systems;
    }
    std::string text;
    std::vector<LispLibSystem> systems;
    if (ReadAll(asd, &text)) systems = LispLibParseAsd(SplitLines(text), asd);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_asds[asd] = {stamp, systems};
    return systems;
}

std::shared_ptr<const LispLibFile> FileOf(const std::string &path, const std::string &system) {
    long long stamp = 0;
    long long size = 0;
    if (!FileStamp(path, &stamp, &size) || size > kMaxFileBytes) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_files.find(path);
        if (it != g_files.end() && it->second.stamp == stamp) return it->second.file;
    }
    std::string text;
    if (!ReadAll(path, &text)) return nullptr;
    auto f = std::make_shared<LispLibFile>(LispLibParseSource(SplitLines(text), path));
    f->system = system;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_files[path] = {stamp, f};
    return f;
}

/** @brief The `<name>.asd` files the CL_SOURCE_REGISTRY entries offer for some system names. */
std::vector<std::string> RegistryAsds(const std::set<std::string> &names) {
    std::vector<std::string> out;
    if (names.empty()) return out;
    for (std::string entry : EnvList("CL_SOURCE_REGISTRY")) {
        if (entry.empty() || entry[0] == '(') continue;  // a configuration DSL form: not followed
        const bool recursive = entry.size() > 2 && entry.compare(entry.size() - 2, 2, "//") == 0;
        while (entry.size() > 1 && entry.back() == '/') entry.pop_back();
        std::vector<std::string> asds;
        if (recursive) {
            asds = CachedWalk(entry, 5, false, 60);
        } else {
            for (const std::string &n : names) asds.push_back(entry + "/" + n + ".asd");
        }
        for (const std::string &a : asds) {
            if (names.count(Lower(fs::path(a).stem().string())) == 0) continue;
            long long stamp = 0;
            if (FileStamp(a, &stamp)) out.push_back(a);
        }
    }
    return out;
}

const char *const kIgnoredPackages[] = {
    "COMMON-LISP", "CL", "KEYWORD", "COMMON-LISP-USER", "CL-USER", "ASDF", "UIOP", "QL", "QUICKLISP", "CCL",
    "EXT", "SI", "SYS", "SYSTEM", "EXCL", "LW", "MOP", "CLOSER-MOP", "C2MOP", "ASDF/USER", "UIOP/PACKAGE",
};

/** @brief The nearest directory above a document holding `.git`, or "" when there is none. */
fs::path ProjectRoot(const std::string &doc_path) {
    if (doc_path.empty()) return fs::path();
    std::error_code ec;
    const fs::path start = fs::absolute(fs::path(doc_path), ec).parent_path();
    for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / ".git", ec)) return dir;
        if (dir.parent_path() == dir) break;
    }
    return fs::path();
}

/** @brief The `.asd` files in the project root's `build*` directories (where CMake writes generated bindings). */
std::vector<std::string> BuildDirAsds(const std::string &doc_path) {
    std::vector<std::string> out;
    const fs::path root = ProjectRoot(doc_path);
    if (root.empty()) return out;
    std::error_code ec;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        std::error_code ec2;
        if (name.rfind("build", 0) != 0 || !it->is_directory(ec2) || it->is_symlink(ec2)) continue;
        for (const std::string &a : CachedWalk(it->path(), 8, true, 5)) out.push_back(a);
    }
    return out;
}

bool IgnoredPackage(const std::string &p) {
    if (p.rfind("SB-", 0) == 0) return true;
    for (const char *n : kIgnoredPackages) {
        if (p == n) return true;
    }
    return false;
}

}  // namespace

// --- public: parsing -------------------------------------------------------------------------------------

LispLibFile LispLibParseSource(const std::vector<std::string> &lines, const std::string &path) {
    LispLibFile f;
    f.path = path;
    SourceIndexer ix(lines, &f);
    ix.Run();
    return f;
}

std::vector<LispLibSystem> LispLibParseAsd(const std::vector<std::string> &lines, const std::string &asd_path) {
    std::vector<LispLibSystem> out;
    const LispReadResult r = LispRead(lines);
    const fs::path dir = fs::path(asd_path).parent_path();
    for (int top : r.top) {
        const LispNode &f = r.at(top);
        if (HeadName(r, f) != "DEFSYSTEM" || f.kids.size() < 2) continue;
        LispLibSystem s;
        s.name = Lower(DesignatorName(r, f.kids[1]));
        s.asd = asd_path;
        if (s.name.empty()) continue;
        fs::path base = dir;
        int components = -1;
        for (size_t i = 2; i + 1 < f.kids.size(); i += 2) {
            const std::string key = r.at(f.kids[i]).name;
            const int v = f.kids[i + 1];
            if (key == "PATHNAME" && r.at(v).kind == LispNodeKind::String) base = dir / r.at(v).name;
            if (key == "COMPONENTS") components = v;
            if (key == "DEPENDS-ON" && r.at(v).kind == LispNodeKind::List) {
                for (int d : r.at(v).kids) {
                    const std::string name = Lower(DesignatorName(r, d));
                    if (!name.empty()) s.depends_on.push_back(name);
                }
            }
        }
        if (components >= 0) Components(r, components, base, &s.files, 0);
        out.push_back(s);
    }
    return out;
}

std::vector<std::string> LispLibDocSignatures(const std::string &doc, const std::string &lower_name) {
    std::vector<std::string> out;
    bool fence = false;
    for (const std::string &raw : SplitLines(doc)) {
        const std::string t = Trim(raw);
        if (t.rfind("```", 0) == 0) {
            fence = !fence;
            continue;
        }
        if (fence || raw.empty() || (raw[0] != ' ' && raw[0] != '\t') || t.empty() || t[0] != '(') continue;
        int depth = 0;
        size_t close = std::string::npos;
        bool in_string = false;
        for (size_t i = 0; i < t.size(); i++) {
            const char c = t[i];
            if (in_string) {
                if (c == '\\') i++;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == '(') depth++;
            else if (c == ')' && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close == std::string::npos) continue;
        const std::string inner = t.substr(1, close - 1);
        const size_t sp = inner.find_first_of(" \t");
        const std::string head = inner.substr(0, sp);
        if (Lower(head) != lower_name) continue;
        out.push_back(sp == std::string::npos ? std::string() : Trim(inner.substr(sp)));
    }
    return out;
}

std::string LispLibDocMarkdown(const std::string &doc) {
    const std::vector<std::string> lines = SplitLines(doc);
    std::string out;
    bool fence = false;
    size_t i = 0;
    auto indented = [](const std::string &l) { return !l.empty() && (l[0] == ' ' || l[0] == '\t') && !Trim(l).empty(); };
    while (i < lines.size()) {
        const std::string &l = lines[i];
        if (Trim(l).rfind("```", 0) == 0) {
            fence = !fence;
            out += Trim(l) + "\n";
            i++;
            continue;
        }
        if (fence || !indented(l)) {
            out += l + "\n";
            i++;
            continue;
        }
        // A run of indented lines (blank lines inside it kept) becomes one code block.
        size_t j = i;
        size_t last = i;
        while (j < lines.size()) {
            if (indented(lines[j])) last = j;
            else if (!Trim(lines[j]).empty() || Trim(lines[j]).rfind("```", 0) == 0) break;
            j++;
        }
        size_t common = std::string::npos;
        for (size_t k = i; k <= last; k++) {
            if (!indented(lines[k])) continue;
            common = std::min(common, lines[k].find_first_not_of(" \t"));
        }
        const bool lisp = Trim(lines[i])[0] == '(';
        if (!out.empty() && out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") != 0) out += "\n";
        out += lisp ? "```lisp\n" : "```\n";
        for (size_t k = i; k <= last; k++) out += (lines[k].size() > common ? lines[k].substr(common) : std::string()) + "\n";
        out += "```\n";
        i = last + 1;
        if (i < lines.size() && !Trim(lines[i]).empty()) out += "\n";
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// --- public: the merged view -----------------------------------------------------------------------------

void LispLibrary::Build() {
    // Primary names first, then nicknames, so a nickname never shadows a package's own name.
    for (const auto &f : files) {
        for (const LispLibPackage &p : f->packages) {
            if (packages_.emplace(p.name, &p).second) package_files_[&p] = f.get();
        }
    }
    for (const auto &f : files) {
        for (const LispLibPackage &p : f->packages) {
            for (const std::string &n : p.nicknames) packages_.emplace(n, &p);
        }
    }
    for (const auto &f : files) {
        for (const LispLibDef &d : f->defs) {
            const LispLibPackage *p = Package(d.package);
            defs_[p != nullptr ? p->name : d.package][d.name].push_back(&d);
            def_files_[&d] = f.get();
        }
    }
}

const LispLibPackage *LispLibrary::Package(const std::string &name) const {
    auto it = packages_.find(name);
    return it == packages_.end() ? nullptr : it->second;
}

const LispLibFile *LispLibrary::FileOf(const LispLibPackage *p) const {
    auto it = package_files_.find(p);
    return it == package_files_.end() ? nullptr : it->second;
}

const LispLibFile *LispLibrary::FileOfDef(const LispLibDef *d) const {
    auto it = def_files_.find(d);
    return it == def_files_.end() ? nullptr : it->second;
}

const std::vector<const LispLibDef *> &LispLibrary::Defs(const std::string &pkg, const std::string &symbol) const {
    static const std::vector<const LispLibDef *> kNone;
    auto it = defs_.find(pkg);
    if (it == defs_.end()) return kNone;
    auto jt = it->second.find(symbol);
    return jt == it->second.end() ? kNone : jt->second;
}

std::vector<std::string> LispLibrary::Symbols(const std::string &pkg) const {
    std::vector<std::string> out;
    auto it = defs_.find(pkg);
    if (it == defs_.end()) return out;
    for (const auto &kv : it->second) out.push_back(kv.first);
    return out;
}

// --- public: discovery -----------------------------------------------------------------------------------

std::vector<std::string> LispLibAsdFiles(const std::string &doc_path) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::vector<std::string> &v) {
        for (const std::string &p : v) {
            if (out.size() < kMaxAsdFiles && seen.insert(p).second) out.push_back(p);
        }
    };
    for (const std::string &e : EnvList("MEP_LISP_LIBRARY")) {
        std::error_code ec;
        if (fs::is_directory(e, ec)) add(CachedWalk(e, 6, true, 5));
        else if (fs::path(e).extension() == ".asd") add({fs::path(e).lexically_normal().string()});
    }
    if (doc_path.empty()) return out;
    std::error_code ec;
    const fs::path start = fs::absolute(fs::path(doc_path), ec).parent_path();
    const fs::path root = ProjectRoot(doc_path);
    if (!root.empty()) {
        add(CachedWalk(root, 6, true, 5));
        return out;
    }
    // No project root: the document's own directory, and .asd files right in the ones above it.
    add(CachedWalk(start, 3, true, 5));
    int up = 0;
    for (fs::path dir = start.parent_path(); !dir.empty() && up < 8; dir = dir.parent_path(), up++) {
        add(CachedWalk(dir, 0, true, 5));
        if (dir.parent_path() == dir) break;
    }
    return out;
}

std::shared_ptr<const LispLibrary> LispLibraryFor(const std::string &doc_path, const LispLibRefs &refs) {
    std::set<std::string> wanted_packages;
    for (const std::string &p : refs.packages) {
        if (!IgnoredPackage(p)) wanted_packages.insert(p);
    }
    if (wanted_packages.empty() && refs.systems.empty()) return nullptr;

    std::vector<LispLibSystem> systems;
    for (const std::string &asd : LispLibAsdFiles(doc_path)) {
        for (LispLibSystem &s : SystemsOf(asd)) systems.push_back(std::move(s));
    }
    std::vector<std::shared_ptr<const LispLibFile>> files;
    std::set<std::string> file_paths;
    std::set<std::string> known;  // package names and nicknames indexed so far
    auto add_file = [&](const std::string &path, const std::string &system) {
        if (files.size() >= kMaxIndexedFiles || !file_paths.insert(path).second) return;
        auto f = FileOf(path, system);
        if (!f) return;
        for (const LispLibPackage &p : f->packages) {
            known.insert(p.name);
            for (const std::string &n : p.nicknames) known.insert(n);
        }
        files.push_back(f);
    };
    std::set<std::string> loaded_systems;
    std::set<std::string> loaded_names;
    std::function<void(const LispLibSystem &)> add_system;
    add_system = [&](const LispLibSystem &s) {
        // The first system found by a name wins (project, then registry, then build directories):
        // an installed or generated copy of it elsewhere would only duplicate every definition.
        if (!loaded_names.insert(s.name).second) return;
        loaded_systems.insert(s.name + "|" + s.asd);
        for (const std::string &dep : s.depends_on) {
            for (const LispLibSystem &o : systems) {
                if (o.name == dep) add_system(o);
            }
        }
        for (const std::string &f : s.files) add_file(f, s.name);
    };
    // $MEP_LISP_LIBRARY may name .lisp files directly.
    for (const std::string &e : EnvList("MEP_LISP_LIBRARY")) {
        if (fs::path(e).extension() == ".lisp") add_file(fs::path(e).lexically_normal().string(), std::string());
    }
    // 1. Systems named like what the document loads or refers to.
    std::set<std::string> names = refs.systems;
    for (const std::string &p : wanted_packages) names.insert(Lower(p));
    for (const LispLibSystem &s : systems) {
        if (names.count(s.name) > 0) add_system(s);
    }
    auto unresolved = [&]() {
        std::set<std::string> out;
        for (const std::string &p : wanted_packages) {
            if (known.count(p) == 0) out.insert(Lower(p));
        }
        return out;
    };
    // What is still missing: packages no indexed file defines, systems not found.
    auto still_missing = [&]() {
        std::set<std::string> out = unresolved();
        for (const std::string &s : refs.systems) {
            const auto it = loaded_systems.lower_bound(s + "|");
            if (it == loaded_systems.end() || it->rfind(s + "|", 0) != 0) out.insert(s);
        }
        return out;
    };
    // 2. The source registry, for names the project did not provide.
    const std::set<std::string> missing = still_missing();
    if (!missing.empty()) {
        for (const std::string &asd : RegistryAsds(missing)) {
            for (const LispLibSystem &s : SystemsOf(asd)) {
                systems.push_back(s);
                if (missing.count(s.name) > 0) add_system(s);
            }
        }
    }
    // 3. The project's build directories, where CMake (Rosetta's rosetta_generate_common_lisp among it)
    //    writes generated sources: only when something is still missing.
    if (const std::set<std::string> left = still_missing(); !left.empty()) {
        for (const std::string &asd : BuildDirAsds(doc_path)) {
            for (const LispLibSystem &s : SystemsOf(asd)) {
                systems.push_back(s);
                if (left.count(s.name) > 0) add_system(s);
            }
        }
    }
    // 4. Still unknown packages: every system found (bounded), since a package need not be named after
    //    its system.
    if (!unresolved().empty()) {
        for (const LispLibSystem &s : systems) {
            if (unresolved().empty()) break;
            add_system(s);
        }
    }
    if (files.empty()) return nullptr;

    std::vector<const LispLibFile *> key;
    for (const auto &f : files) key.push_back(f.get());
    const std::string cache_key = doc_path.empty() ? std::string("-") : fs::path(doc_path).parent_path().string();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_views.find(cache_key);
        if (it != g_views.end() && it->second.key == key) return it->second.view;
    }
    auto view = std::make_shared<LispLibrary>();
    view->files = files;
    view->Build();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_views.size() > 32) g_views.clear();
    g_views[cache_key] = {key, view};
    return view;
}
