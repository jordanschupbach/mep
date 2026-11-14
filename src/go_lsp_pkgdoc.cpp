// Doc comments of Go packages: see go_lsp_pkgdoc.h.
#include "go_lsp_pkgdoc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#include "go_ast.h"
#include "go_parse.h"

namespace gopkgdoc {

namespace {

namespace fs = std::filesystem;
using namespace goast;

std::string ReadAll(const std::string &path, bool *ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool IsDir(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

bool IsFile(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string Env(const char *name) {
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::vector<std::string> Fields(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string Unquote(std::string s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '`') && s.back() == s.front()) return s.substr(1, s.size() - 2);
    return s;
}

std::string GoflagValue(const std::string &flag) {
    for (const std::string &f : Fields(Env("GOFLAGS"))) {
        for (const std::string &prefix : {"-" + flag + "=", "--" + flag + "="}) {
            if (f.compare(0, prefix.size(), prefix) == 0) return f.substr(prefix.size());
        }
    }
    return "";
}

// The last element of an import path, as the package's likely name (`/v2` suffixes skipped).
std::string LastElement(const std::string &path) {
    std::string p = path;
    size_t slash = p.rfind('/');
    std::string last = slash == std::string::npos ? p : p.substr(slash + 1);
    if (last.size() >= 2 && last[0] == 'v' && std::all_of(last.begin() + 1, last.end(), ::isdigit) &&
        slash != std::string::npos) {
        p = p.substr(0, slash);
        slash = p.rfind('/');
        last = slash == std::string::npos ? p : p.substr(slash + 1);
    }
    return last;
}

std::string RecvType(const FuncDecl *f) {
    if (!f->Recv || f->Recv->List.empty()) return "";
    const Node *t = f->Recv->List[0]->Type;
    for (;;) {
        if (const auto *s = As<StarExpr>(t)) t = s->X;
        else if (const auto *p = As<ParenExpr>(t)) t = p->X;
        else if (const auto *ix = As<IndexExpr>(t)) t = ix->X;
        else if (const auto *il = As<IndexListExpr>(t)) t = il->X;
        else break;
    }
    const auto *id = As<Ident>(t);
    return id ? id->Name : "";
}

struct FileParse {
    std::string path, text;
    goparse::Result r;
    std::string Source(Pos a, Pos b) const {
        if (!IsValid(a) || !IsValid(b) || b < a) return "";
        const size_t s = static_cast<size_t>(a - 1), e = std::min(text.size(), static_cast<size_t>(b - 1));
        return s < e ? text.substr(s, e - s) : "";
    }
};

// At most `max` lines of a declaration's source, the rest elided.
std::string Clip(const std::string &s, int max = 40) {
    std::string out;
    int lines = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\n' && ++lines >= max) return Trim(out) + "\n\t// ...";
        out += s[i];
    }
    return Trim(out);
}

std::string OneLine(const std::string &s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == '\n') break;
        if (c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

void AddFile(Package *pkg, const FileParse &fp, bool all) {
    const File *f = fp.r.file;
    if (!f || !f->Name) return;
    if (pkg->name.empty()) pkg->name = f->Name->Name;
    if (f->Name->Name != pkg->name) return;  // another package in the directory (package main tools...)
    if (f->Doc) {
        const std::string d = CommentGroupText(f->Doc);
        if (!d.empty() && pkg->doc.find(d) == std::string::npos) pkg->doc += (pkg->doc.empty() ? "" : "\n") + d;
    }
    for (const ImportSpec *im : f->Imports) {
        std::string path;
        if (im->Path) goparse::Unquote(fp.Source(NodePos(im->Path), NodeEnd(im->Path)), &path);
        pkg->imports.push_back({im->Name ? im->Name->Name : "", path});
    }
    auto add = [&](const std::string &kind, const std::string &name, const std::string &sig, const CommentGroup *doc,
                   Pos at) {
        Entry e;
        e.kind = kind;
        e.name = name;
        e.signature = sig;
        e.doc = CommentGroupText(doc);
        e.file = fp.path;
        e.line = fp.r.Line(at);
        pkg->entries.push_back(e);
    };
    for (const Node *d : f->Decls) {
        if (const auto *fn = As<FuncDecl>(d)) {
            if (!fn->Name || (!all && !IsExported(fn->Name->Name))) continue;
            const std::string recv = RecvType(fn);
            if (!recv.empty() && !all && !IsExported(recv)) continue;
            const Pos end = fn->Body ? NodePos(fn->Body) : NodeEnd(fn);
            add(recv.empty() ? "func" : "method", recv.empty() ? fn->Name->Name : recv + "." + fn->Name->Name,
                Trim(fp.Source(NodePos(fn), end)), fn->Doc, fn->Name->NamePos);
            continue;
        }
        const auto *g = As<GenDecl>(d);
        if (!g || g->Tok_ == Tok::IMPORT) continue;
        const std::string kw = g->Tok_ == Tok::TYPE ? "type" : g->Tok_ == Tok::CONST ? "const" : "var";
        const std::string group = Clip(fp.Source(NodePos(g), NodeEnd(g)));
        for (const Node *spec : g->Specs) {
            if (const auto *ts = As<TypeSpec>(spec)) {
                if (!ts->Name || (!all && !IsExported(ts->Name->Name))) continue;
                const CommentGroup *doc = ts->Doc ? ts->Doc : g->Doc;
                add("type", ts->Name->Name, "type " + Clip(fp.Source(NodePos(ts), NodeEnd(ts))), doc, ts->Name->NamePos);
                // Exported struct fields and interface methods, as Type.Name.
                const Node *t = ts->Type;
                const FieldList *fl = nullptr;
                if (const auto *st = As<StructType>(t)) fl = st->Fields;
                if (const auto *it = As<InterfaceType>(t)) fl = it->Methods;
                if (!fl) continue;
                for (const Field *field : fl->List) {
                    for (const Ident *n : field->Names) {
                        if (!all && !IsExported(n->Name)) continue;
                        add(As<InterfaceType>(t) ? "method" : "field", ts->Name->Name + "." + n->Name,
                            Trim(fp.Source(NodePos(field), NodeEnd(field))),
                            field->Doc ? field->Doc : field->Comment, n->NamePos);
                    }
                }
            } else if (const auto *vs = As<ValueSpec>(spec)) {
                const CommentGroup *doc = vs->Doc ? vs->Doc : (vs->Comment ? vs->Comment : g->Doc);
                const bool grouped = IsValid(g->Lparen) && g->Specs.size() > 1;
                for (const Ident *n : vs->Names) {
                    if (!all && !IsExported(n->Name)) continue;
                    add(kw, n->Name, grouped ? group : kw + " " + Trim(fp.Source(NodePos(vs), NodeEnd(vs))),
                        doc, n->NamePos);
                }
            }
        }
    }
}

bool GoSource(const std::string &name) {
    if (name.size() < 4 || name.compare(name.size() - 3, 3, ".go") != 0) return false;
    if (name.size() > 8 && name.compare(name.size() - 8, 8, "_test.go") == 0) return false;
    return name[0] != '.' && name[0] != '_';
}

// Doc comment text as Markdown: indented lines become a code block.
std::string DocMarkdown(const std::string &doc) {
    std::string out;
    bool code = false;
    std::istringstream in(doc);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) lines.push_back(line);
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &l = lines[i];
        const bool indented = !l.empty() && (l[0] == '\t' || l[0] == ' ');
        if (indented && !code) {
            out += "```go\n";
            code = true;
        } else if (!indented && code && !l.empty()) {
            while (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0) out.pop_back();
            out += "```\n";
            code = false;
        }
        if (code) {
            out += (l.empty() ? std::string() : l.substr(l[0] == '\t' ? 1 : std::min<size_t>(l.find_first_not_of(' '), 4)));
        } else {
            out += l;
        }
        out += "\n";
    }
    if (code) {
        while (out.size() >= 2 && out.compare(out.size() - 2, 2, "\n\n") == 0) out.pop_back();
        out += "```\n";
    }
    return Trim(out);
}

std::string RenderEntry(const Entry &e, const Package &pkg, bool with_location) {
    std::string out = "```go\n" + e.signature + "\n```";
    out += "\n\n" + e.kind + " in package `" + (pkg.import_path.empty() ? pkg.name : pkg.import_path) + "`";
    if (!e.doc.empty()) out += "\n\n" + DocMarkdown(e.doc);
    if (with_location) out += "\n\n" + e.file + ":" + std::to_string(e.line);
    return out;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The entries a symbol path (["Frame"], ["Frame", "Filter"]) names; case-insensitive when it is all lower case.
std::vector<const Entry *> Lookup(const Package &pkg, const std::vector<std::string> &sym) {
    std::vector<const Entry *> out;
    std::string want;
    for (size_t i = 0; i < sym.size(); i++) want += (i ? "." : "") + sym[i];
    for (const Entry &e : pkg.entries)
        if (e.name == want) out.push_back(&e);
    if (out.empty() && Lower(want) == want) {
        for (const Entry &e : pkg.entries)
            if (Lower(e.name) == want) out.push_back(&e);
    }
    return out;
}

std::string RenderSymbol(const Package &pkg, const std::vector<std::string> &sym) {
    const std::vector<const Entry *> hits = Lookup(pkg, sym);
    if (hits.empty()) return "";
    std::string out;
    for (const Entry *e : hits) {
        if (!out.empty()) out += "\n\n---\n\n";
        out += RenderEntry(*e, pkg, true);
        if (e->kind == "type") {
            // Its methods, and the functions that return it (go doc's constructors).
            std::string methods;
            const std::string prefix = e->name + ".";
            for (const Entry &m : pkg.entries) {
                const bool method = m.kind == "method" && m.name.compare(0, prefix.size(), prefix) == 0 &&
                                    m.signature.compare(0, 4, "func") == 0;
                bool ctor = false;
                if (m.kind == "func") {
                    const size_t close = m.signature.find(')');
                    const std::string results = close == std::string::npos ? "" : m.signature.substr(close);
                    for (const std::string &form : {" " + e->name, "*" + e->name, "(" + e->name})
                        if (results.find(form) != std::string::npos) ctor = true;
                }
                if (method || ctor) methods += "\n" + OneLine(m.signature);
            }
            if (!methods.empty()) out += "\n\n```go" + methods + "\n```";
        }
    }
    return out;
}

// One line for an entry: a value's own line of its group, a type's first line closed with `{ ... }`.
std::string Summary(const Entry &e) {
    std::string sig = OneLine(e.signature);
    if ((e.kind == "const" || e.kind == "var") && e.signature.find('\n') != std::string::npos) {
        std::istringstream in(e.signature);
        std::string l;
        while (std::getline(in, l)) {
            const std::string t = Trim(l);
            if (t.compare(0, e.name.size(), e.name) == 0 &&
                (t.size() == e.name.size() || !(std::isalnum(static_cast<unsigned char>(t[e.name.size()])) ||
                                                 t[e.name.size()] == '_'))) {
                sig = e.kind + " " + OneLine(t);
                break;
            }
        }
    }
    const size_t comment = sig.find(" //");
    if (comment != std::string::npos) sig = sig.substr(0, comment);
    if (!sig.empty() && sig.back() == '{') sig += " ... }";
    for (char &c : sig)
        if (c == '\t') c = ' ';
    return sig;
}

std::string RenderPackage(const Package &pkg) {
    std::string out = "```go\npackage " + pkg.name;
    if (!pkg.import_path.empty()) out += " // import \"" + pkg.import_path + "\"";
    out += "\n```";
    if (!pkg.doc.empty()) out += "\n\n" + DocMarkdown(pkg.doc);
    std::string decls;
    for (const Entry &e : pkg.entries)
        if (e.kind != "field" && !(e.kind == "method" && e.signature.compare(0, 4, "func") != 0))
            decls += "\n" + Summary(e);
    if (!decls.empty()) out += "\n\n```go" + decls + "\n```";
    out += "\n\n" + pkg.dir;
    return out;
}

std::vector<std::string> SplitDots(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string GorootDir() {
    std::string root = Env("GOROOT");
    if (IsDir(root + "/src")) return root;
    const std::string path = Env("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        const size_t colon = path.find(':', start);
        const std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty() && IsFile(dir + "/go")) {
            std::error_code ec;
            const fs::path real = fs::canonical(dir + "/go", ec);
            if (!ec) {
                const std::string r = real.parent_path().parent_path().string();
                if (IsDir(r + "/src/fmt")) return r;
                if (IsDir(r + "/share/go/src/fmt")) return r + "/share/go";
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

std::vector<std::string> Gopaths() {
    std::vector<std::string> out;
    std::string gp = Env("GOPATH");
    size_t start = 0;
    while (!gp.empty() && start <= gp.size()) {
        const size_t colon = gp.find(':', start);
        const std::string d = gp.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!d.empty()) out.push_back(d);
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    if (out.empty() && !Env("HOME").empty()) out.push_back(Env("HOME") + "/go");
    return out;
}

std::string ModCache() {
    const std::string mc = Env("GOMODCACHE");
    if (!mc.empty()) return mc;
    const std::vector<std::string> gp = Gopaths();
    return gp.empty() ? "" : gp[0] + "/pkg/mod";
}

bool HasPrefixPath(const std::string &path, const std::string &prefix, std::string *rest) {
    if (path == prefix) {
        *rest = "";
        return true;
    }
    if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/') {
        *rest = path.substr(prefix.size());
        return true;
    }
    return false;
}

}  // namespace

Package ReadSources(const std::vector<std::pair<std::string, std::string>> &files, bool all) {
    Package pkg;
    // Files whose name says the package (doc.go first), then the rest in name order.
    std::vector<FileParse> parsed;
    for (const auto &f : files) {
        FileParse fp;
        fp.path = f.first;
        fp.text = f.second;
        goparse::Options o;
        o.all_errors = false;
        fp.r = goparse::ParseFile(fp.text, o);
        parsed.push_back(std::move(fp));
    }
    // The package's name: the most common one (a directory may hold a stray `package main` tool).
    std::map<std::string, int> names;
    for (const FileParse &fp : parsed)
        if (fp.r.file && fp.r.file->Name && fp.r.file->Name->Name != "main") names[fp.r.file->Name->Name]++;
    for (const auto &n : names)
        if (pkg.name.empty() || n.second > names[pkg.name]) pkg.name = n.first;
    for (const FileParse &fp : parsed) AddFile(&pkg, fp, all);
    pkg.ok = !pkg.name.empty();
    return pkg;
}

Package ReadPackage(const std::string &dir, bool all) {
    std::vector<std::pair<std::string, std::string>> files;
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (GoSource(name) && it->is_regular_file(ec)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    // Parsed packages are kept while their files' names, sizes and times are unchanged: a
    // generated binding (datamunge's datamunge.go is 54k lines) takes about a second to parse,
    // and a hover on a method call reads it several times.
    std::string stamp;
    for (const std::string &name : names) {
        const fs::path path = fs::path(dir) / name;
        std::error_code e1, e2;
        const auto size = fs::file_size(path, e1);
        const auto time = fs::last_write_time(path, e2);
        stamp += name + ":" + std::to_string(e1 ? 0 : size) + ":" +
                 std::to_string(e2 ? 0 : static_cast<long long>(time.time_since_epoch().count())) + ";";
    }
    static std::map<std::pair<std::string, bool>, std::pair<std::string, Package>> cache;
    const auto key = std::make_pair(dir, all);
    const auto hit = cache.find(key);
    if (hit != cache.end() && hit->second.first == stamp) return hit->second.second;
    for (const std::string &name : names) {
        bool ok = false;
        const std::string path = (fs::path(dir) / name).string();
        std::string text = ReadAll(path, &ok);
        if (ok) files.push_back({path, std::move(text)});
    }
    Package pkg = ReadSources(files, all);
    pkg.dir = dir;
    if (cache.size() > 64) cache.clear();
    cache[key] = {stamp, pkg};
    return pkg;
}

Module ParseGoMod(const std::string &text) {
    Module m;
    std::istringstream in(text);
    std::string line, block;
    while (std::getline(in, line)) {
        const size_t comment = line.find("//");
        if (comment != std::string::npos) line = line.substr(0, comment);
        line = Trim(line);
        if (line.empty()) continue;
        if (!block.empty()) {
            if (line == ")") {
                block.clear();
                continue;
            }
            line = block + " " + line;
        }
        std::vector<std::string> w = Fields(line);
        if (w.size() == 2 && w[1] == "(" && (w[0] == "require" || w[0] == "replace" || w[0] == "exclude" ||
                                            w[0] == "retract" || w[0] == "tool" || w[0] == "godebug")) {
            block = w[0];
            continue;
        }
        for (std::string &x : w) x = Unquote(x);
        if (w[0] == "module" && w.size() >= 2) {
            m.path = w[1];
            m.ok = true;
        } else if (w[0] == "require" && w.size() >= 3) {
            m.require.push_back({w[1], w[2]});
        } else if (w[0] == "replace") {
            const auto arrow = std::find(w.begin(), w.end(), "=>");
            if (arrow == w.end() || arrow == w.begin() + 1 || arrow + 1 == w.end()) continue;
            Module::Replace r;
            r.old_path = w[1];
            if (arrow - w.begin() == 3) r.old_version = w[2];
            r.new_path = *(arrow + 1);
            if (arrow + 2 != w.end()) r.new_version = *(arrow + 2);
            m.replace.push_back(r);
        }
    }
    return m;
}

Module FindModule(const std::string &dir) {
    Module m;
    std::string modfile = GoflagValue("modfile");
    std::error_code ec;
    fs::path d = fs::absolute(dir.empty() ? "." : dir, ec);
    if (modfile.empty()) {
        for (fs::path p = d; !p.empty(); p = p.parent_path()) {
            if (IsFile((p / "go.mod").string())) {
                modfile = (p / "go.mod").string();
                break;
            }
            if (p == p.parent_path()) break;
        }
    }
    if (modfile.empty()) return m;
    bool ok = false;
    const std::string text = ReadAll(modfile, &ok);
    if (!ok) return m;
    m = ParseGoMod(text);
    m.root = fs::absolute(modfile, ec).parent_path().string();
    return m;
}

std::string EscapeModulePath(const std::string &path) {
    std::string out;
    for (char c : path) {
        if (std::isupper(static_cast<unsigned char>(c))) {
            out += '!';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

std::string ResolveImport(const std::string &path, const std::string &dir, std::string *why) {
    auto found = [why](const std::string &d, const std::string &how) {
        if (why) *why = how;
        return d;
    };
    if (path.empty()) return "";
    const Module m = FindModule(dir);
    std::string rest;
    if (m.ok) {
        if (HasPrefixPath(path, m.path, &rest) && IsDir(m.root + rest)) return found(m.root + rest, "main module");
        for (const Module::Replace &r : m.replace) {
            if (!HasPrefixPath(path, r.old_path, &rest)) continue;
            const std::string &np = r.new_path;
            if (np.compare(0, 2, "./") == 0 || np.compare(0, 3, "../") == 0 || (!np.empty() && np[0] == '/')) {
                std::error_code ec;
                const fs::path base = np[0] == '/' ? fs::path(np) : fs::path(m.root) / np;
                const std::string d = fs::weakly_canonical(base, ec).string() + rest;
                if (IsDir(d)) return found(d, "replace " + r.old_path + " => " + np);
            } else if (!r.new_version.empty()) {
                const std::string d = ModCache() + "/" + EscapeModulePath(np) + "@" + r.new_version + rest;
                if (IsDir(d)) return found(d, "replace " + r.old_path + " => " + np + " " + r.new_version);
            }
        }
        const bool vendor_first = GoflagValue("mod") == "vendor";
        const std::string vendored = m.root + "/vendor/" + path;
        if (vendor_first && IsDir(vendored)) return found(vendored, "vendor");
        // The longest required module path that prefixes the import.
        const std::pair<std::string, std::string> *best = nullptr;
        for (const auto &req : m.require) {
            std::string r2;
            if (HasPrefixPath(path, req.first, &r2) && (!best || req.first.size() > best->first.size())) best = &req;
        }
        if (best) {
            HasPrefixPath(path, best->first, &rest);
            const std::string d = ModCache() + "/" + EscapeModulePath(best->first) + "@" + best->second + rest;
            if (IsDir(d)) return found(d, "module cache");
        }
        if (IsDir(vendored)) return found(vendored, "vendor");
    }
    const std::string goroot = GorootDir();
    if (!goroot.empty() && IsDir(goroot + "/src/" + path)) return found(goroot + "/src/" + path, "GOROOT");
    for (const std::string &gp : Gopaths())
        if (IsDir(gp + "/src/" + path)) return found(gp + "/src/" + path, "GOPATH");
    return "";
}

std::string Doc(const std::string &name_in, const std::string &dir,
                const std::vector<std::pair<std::string, std::string>> &context) {
    std::string name = Trim(name_in);
    if (name.empty()) return "";
    // An import path: everything up to the last slash, then the first dot after it.
    const size_t slash = name.rfind('/');
    if (slash != std::string::npos) {
        const size_t dot = name.find('.', slash);
        const std::string path = name.substr(0, dot);
        const std::vector<std::string> sym = dot == std::string::npos ? std::vector<std::string>() : SplitDots(name.substr(dot + 1));
        const std::string pdir = ResolveImport(path, dir);
        if (pdir.empty()) return "";
        Package pkg = ReadPackage(pdir);
        pkg.import_path = path;
        return sym.empty() ? RenderPackage(pkg) : RenderSymbol(pkg, sym);
    }
    const std::vector<std::string> parts = SplitDots(name);
    if (parts.empty()) return "";
    // The context's and the directory's imports, by name.
    Package here = ReadPackage(dir, true);
    if (!context.empty()) {
        // The open buffer replaces its saved file.
        std::vector<std::pair<std::string, std::string>> files = context;
        std::set<std::string> have;
        std::error_code ec;
        for (const auto &f : context) have.insert(fs::weakly_canonical(f.first, ec).string());
        std::vector<std::string> names;
        for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
            const std::string fname = it->path().filename().string();
            if (!GoSource(fname)) continue;
            if (have.count(fs::weakly_canonical(it->path(), ec).string())) continue;
            names.push_back(it->path().string());
        }
        std::sort(names.begin(), names.end());
        for (const std::string &p : names) {
            bool ok = false;
            std::string text = ReadAll(p, &ok);
            if (ok) files.push_back({p, std::move(text)});
        }
        here = ReadSources(files, true);
        here.dir = dir;
    }
    std::string first = parts[0];
    // `pkg.Sym`: an import of these files, by its name.
    std::vector<std::string> candidates;
    for (const auto &im : here.imports) {
        if (im.first == first) candidates.push_back(im.second);
        else if (im.first.empty() && LastElement(im.second) == first) candidates.push_back(im.second);
    }
    for (const auto &im : here.imports) {
        // The package clause can differ from the path's last element (go-foo -> foo).
        if (!im.first.empty() || LastElement(im.second) == first) continue;
        const std::string pdir = ResolveImport(im.second, dir);
        if (pdir.empty()) continue;
        if (ReadPackage(pdir).name == first) candidates.push_back(im.second);
    }
    const std::vector<std::string> rest(parts.begin() + 1, parts.end());
    for (const std::string &path : candidates) {
        const std::string pdir = ResolveImport(path, dir);
        if (pdir.empty()) continue;
        Package pkg = ReadPackage(pdir);
        pkg.import_path = path;
        const std::string out = rest.empty() ? RenderPackage(pkg) : RenderSymbol(pkg, rest);
        if (!out.empty()) return out;
    }
    // A symbol of the package in DIR (unexported ones too: it is the user's own code).
    if (here.ok) {
        const Module m = FindModule(dir);
        if (m.ok) {
            std::error_code ec;
            const std::string rel = fs::relative(fs::absolute(dir, ec), m.root, ec).generic_string();
            here.import_path = rel == "." || rel.empty() ? m.path : m.path + "/" + rel;
        }
        const std::string out = RenderSymbol(here, parts);
        if (!out.empty()) return out;
    }
    // A package by its path (`datamunge`, `encoding/json`, a module's own path).
    const std::string pdir = ResolveImport(parts.size() == 1 ? name : parts[0], dir);
    if (!pdir.empty()) {
        Package pkg = ReadPackage(pdir);
        pkg.import_path = parts[0];
        return rest.empty() ? RenderPackage(pkg) : RenderSymbol(pkg, rest);
    }
    return "";
}

// --- members of typed values (hover on `x.Method`) ------------------------------------------------

namespace {

// A named type: the package declaring it and the type's name.
struct NamedType {
    Package pkg;
    std::string name;
    bool ok() const { return pkg.ok && !name.empty(); }
};

bool IdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool IsIdent(const std::string &s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    return std::all_of(s.begin(), s.end(), IdentChar);
}

// The index of the bracket matching the closing one at CLOSE, skipping string and rune literals; npos if none.
size_t MatchOpen(const std::string &s, size_t close) {
    const char c = s[close], o = c == ')' ? '(' : c == ']' ? '[' : '{';
    int depth = 0;
    for (size_t i = close + 1; i-- > 0;) {
        const char ch = s[i];
        if (ch == '"' || ch == '`' || ch == '\'') {
            // Back to the literal's opening quote.
            size_t j = i;
            while (j-- > 0) {
                if (s[j] == ch && (ch == '`' || j == 0 || s[j - 1] != '\\')) break;
            }
            if (j == std::string::npos) return std::string::npos;
            i = j;
            continue;
        }
        if (ch == c) depth++;
        else if (ch == o && --depth == 0) return i;
    }
    return std::string::npos;
}

// S split at its top-level commas.
std::vector<std::string> TopLevelSplit(const std::string &s) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (quote) {
            if (c == '\\' && quote != '`') i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '`' || c == '\'') quote = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (c == ',' && depth == 0) {
            out.push_back(Trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(Trim(s.substr(start)));
    return out;
}

// The imports visible from DIR and the context files, by the name they are used with.
struct Scope {
    std::string dir;
    std::vector<std::pair<std::string, std::string>> imports;  // name, path
};

Scope MakeScope(const std::string &dir, const std::vector<std::pair<std::string, std::string>> &context) {
    Scope sc;
    sc.dir = dir;
    const Package here = context.empty() ? ReadPackage(dir, true) : ReadSources(context, true);
    for (const auto &im : here.imports) {
        std::string name = im.first;
        if (name.empty()) {
            const std::string pdir = ResolveImport(im.second, dir);
            name = pdir.empty() ? LastElement(im.second) : ReadPackage(pdir).name;
            if (name.empty()) name = LastElement(im.second);
        }
        sc.imports.push_back({name, im.second});
    }
    return sc;
}

// The package imported as NAME, or !ok.
Package ImportedPackage(const Scope &sc, const std::string &name) {
    for (const auto &im : sc.imports) {
        if (im.first != name) continue;
        const std::string pdir = ResolveImport(im.second, sc.dir);
        if (pdir.empty()) continue;
        Package pkg = ReadPackage(pdir);
        pkg.import_path = im.second;
        if (pkg.ok) return pkg;
    }
    return Package();
}

// A type expression (`*Frame`, `datamunge.Frame`) written in package FROM, as a named type.
NamedType ResolveTypeExpr(std::string t, const Package &from) {
    t = Trim(t);
    while (!t.empty() && (t[0] == '*' || t[0] == '&')) t = Trim(t.substr(1));
    const size_t bracket = t.find('[');  // a generic instance: Box[int]
    if (bracket != std::string::npos && bracket > 0) t = t.substr(0, bracket);
    NamedType out;
    const size_t dot = t.find('.');
    if (dot == std::string::npos) {
        if (!IsIdent(t)) return out;
        out.pkg = from;
        out.name = t;
        return out;
    }
    const std::string q = t.substr(0, dot), name = t.substr(dot + 1);
    if (!IsIdent(q) || !IsIdent(name)) return out;
    Scope sc;
    sc.dir = from.dir;
    for (const auto &im : from.imports) {
        std::string n = im.first;
        if (n.empty()) {
            const std::string pdir = ResolveImport(im.second, from.dir);
            n = pdir.empty() ? LastElement(im.second) : ReadPackage(pdir).name;
        }
        sc.imports.push_back({n, im.second});
    }
    out.pkg = ImportedPackage(sc, q);
    out.name = name;
    return out;
}

// The first result type of a func or method signature (`func (d *T) M(a int) (*U, error)` -> `*U`).
std::string FirstResult(const std::string &sig, const std::string &name) {
    size_t at = std::string::npos;
    for (size_t p = sig.find(name); p != std::string::npos; p = sig.find(name, p + 1)) {
        const size_t after = p + name.size();
        if ((p == 0 || !IdentChar(sig[p - 1])) && after < sig.size() && (sig[after] == '(' || sig[after] == '[')) {
            at = after;
            break;
        }
    }
    if (at == std::string::npos) return "";
    if (sig[at] == '[') {  // type parameters
        int depth = 0;
        for (; at < sig.size(); at++) {
            if (sig[at] == '[') depth++;
            else if (sig[at] == ']' && --depth == 0) { at++; break; }
        }
    }
    if (at >= sig.size() || sig[at] != '(') return "";
    int depth = 0;
    size_t end = at;
    for (; end < sig.size(); end++) {
        if (sig[end] == '(') depth++;
        else if (sig[end] == ')' && --depth == 0) break;
    }
    if (end >= sig.size()) return "";
    std::string results = Trim(sig.substr(end + 1));
    const size_t brace = results.find('{');
    if (brace != std::string::npos) results = Trim(results.substr(0, brace));
    if (results.empty()) return "";
    if (results[0] == '(') {
        if (results.back() != ')' || MatchOpen(results, results.size() - 1) != 0) return "";
        const size_t close = results.size() - 1;
        const std::vector<std::string> parts = TopLevelSplit(results.substr(1, close - 1));
        if (parts.empty()) return "";
        // Named results: `(n int, err error)` -> the type after the name.
        const std::vector<std::string> words = Fields(parts[0]);
        return words.size() == 2 ? words[1] : parts[0];
    }
    return results;
}

// The entry for member MEMBER of type T: its own method or field, else one promoted from an embedded type.
const Entry *FindMember(const NamedType &t, const std::string &member, Package *owner, int depth = 0) {
    if (!t.ok() || depth > 8) return nullptr;
    const std::string want = t.name + "." + member;
    for (const Entry &e : t.pkg.entries) {
        if ((e.kind == "method" || e.kind == "field") && e.name == want) {
            *owner = t.pkg;
            for (const Entry &o : owner->entries)
                if (o.name == want && o.kind == e.kind) return &o;
        }
    }
    // Embedded fields: the struct body's lines that are a lone (qualified) type name.
    for (const Entry &e : t.pkg.entries) {
        if (e.kind != "type" || e.name != t.name) continue;
        const size_t open = e.signature.find("struct {");
        if (open == std::string::npos) continue;
        std::istringstream in(e.signature.substr(open + 8));
        std::string line;
        while (std::getline(in, line)) {
            std::string f = line;
            const size_t comment = f.find("//");
            if (comment != std::string::npos) f = f.substr(0, comment);
            f = Trim(f);
            if (!f.empty() && f[0] == '*') f = Trim(f.substr(1));
            if (f.empty() || f == "}") continue;
            const size_t dot = f.find('.');
            const std::string last = dot == std::string::npos ? f : f.substr(dot + 1);
            if (!IsIdent(last) || (dot != std::string::npos && !IsIdent(f.substr(0, dot)))) continue;
            if (const Entry *hit = FindMember(ResolveTypeExpr(f, t.pkg), member, owner, depth + 1)) return hit;
        }
    }
    return nullptr;
}

NamedType TypeOfExpr(const std::string &expr, const Scope &sc, const std::string &text, int depth);

// The type of variable NAME from its latest declaration in TEXT.
NamedType TypeOfVar(const std::string &name, const Scope &sc, const std::string &text, int depth) {
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    auto word_at = [&](const std::string &l, size_t p) {
        return (p == 0 || !IdentChar(l[p - 1])) && p + name.size() <= l.size() &&
               (p + name.size() == l.size() || !IdentChar(l[p + name.size()]));
    };
    for (size_t i = lines.size(); i-- > 0;) {
        std::string l = lines[i];
        const size_t comment = l.find("//");
        if (comment != std::string::npos) l = l.substr(0, comment);
        const std::string t = Trim(l);
        // `var name T` / `var name = expr` / `var name T = expr`
        if (t.compare(0, 4, "var ") == 0) {
            const std::string rest = Trim(t.substr(4));
            if (rest.compare(0, name.size(), name) == 0 && word_at(rest, 0)) {
                std::string after = Trim(rest.substr(name.size()));
                const size_t eq = after.find('=');
                const std::string type = Trim(eq == std::string::npos ? after : after.substr(0, eq));
                if (!type.empty()) {
                    Package local;
                    local.ok = true;
                    local.dir = sc.dir;
                    for (const auto &im : sc.imports) local.imports.push_back(im);
                    const NamedType nt = ResolveTypeExpr(type, local);
                    if (nt.ok()) return nt;
                }
                if (eq != std::string::npos) return TypeOfExpr(after.substr(eq + 1), sc, text, depth + 1);
            }
        }
        // `name := expr`, `name, err := expr`, `name = expr` (the first result)
        for (const char *op : {":=", "="}) {
            const size_t at = t.find(op);
            if (at == std::string::npos) continue;
            if (std::string(op) == "=" && at > 0 && std::string("!<>=:+-*/%&|^").find(t[at - 1]) != std::string::npos) continue;
            if (std::string(op) == "=" && at + 1 < t.size() && t[at + 1] == '=') continue;
            std::string lhs = Trim(t.substr(0, at));
            if (lhs.compare(0, 4, "for ") == 0 || lhs.compare(0, 3, "if ") == 0) lhs = Trim(lhs.substr(lhs.find(' ')));
            const std::vector<std::string> names = TopLevelSplit(lhs);
            if (names.empty() || names[0] != name) break;
            std::string rhs = Trim(t.substr(at + std::string(op).size()));
            // A call continued over the following lines: up to its closing parenthesis.
            for (size_t j = i + 1, opened = 0; j < lines.size(); j++) {
                int balance = 0;
                char quote = 0;
                for (size_t k = 0; k < rhs.size(); k++) {
                    const char c = rhs[k];
                    if (quote) {
                        if (c == '\\' && quote != '`') k++;
                        else if (c == quote) quote = 0;
                    } else if (c == '"' || c == '`' || c == '\'') quote = c;
                    else if (c == '(' || c == '[' || c == '{') balance++;
                    else if (c == ')' || c == ']' || c == '}') balance--;
                }
                if (balance <= 0 || ++opened > 40) break;
                std::string next = lines[j];
                const size_t cmt = next.find("//");
                if (cmt != std::string::npos) next = next.substr(0, cmt);
                rhs += " " + Trim(next);
            }
            if (rhs.compare(0, 6, "range ") == 0) return NamedType();
            if (names.size() > 1) {
                // `x, err := f()`: the call's first result; `a, b := x, y`: the first value.
                const std::vector<std::string> values = TopLevelSplit(rhs);
                if (values.size() == names.size()) rhs = values[0];
            }
            if (!rhs.empty() && rhs.back() == '{' && rhs.find('(') == std::string::npos) rhs.pop_back();
            const size_t semi = rhs.find(';');  // `if x, err := f(); err != nil {`
            if (semi != std::string::npos) rhs = rhs.substr(0, semi);
            return TypeOfExpr(rhs, sc, text, depth + 1);
        }
        // A parameter or field `name *pkg.T` in a signature line.
        for (size_t p = l.find(name); p != std::string::npos; p = l.find(name, p + 1)) {
            if (!word_at(l, p)) continue;
            const size_t q = p + name.size();
            if (q >= l.size() || l[q] != ' ') continue;
            if (p == 0 || (l[p - 1] != '(' && l[p - 1] != ' ' && l[p - 1] != '\t')) continue;
            if (l.find("func") == std::string::npos) continue;
            std::string type;
            size_t r = q + 1;
            while (r < l.size() && (IdentChar(l[r]) || l[r] == '.' || l[r] == '*')) type += l[r++];
            Package local;
            local.ok = true;
            local.dir = sc.dir;
            for (const auto &im : sc.imports) local.imports.push_back(im);
            const NamedType nt = ResolveTypeExpr(type, local);
            if (nt.ok()) return nt;
        }
    }
    return NamedType();
}

NamedType TypeOfExpr(const std::string &expr_in, const Scope &sc, const std::string &text, int depth) {
    const std::string expr = Trim(expr_in);
    if (expr.empty() || depth > 12) return NamedType();
    if (IsIdent(expr)) return TypeOfVar(expr, sc, text, depth);
    if (expr[0] == '&') {  // &pkg.T{...}
        const size_t brace = expr.find('{');
        Package local;
        local.ok = true;
        local.dir = sc.dir;
        for (const auto &im : sc.imports) local.imports.push_back(im);
        return ResolveTypeExpr(expr.substr(1, brace == std::string::npos ? std::string::npos : brace - 1), local);
    }
    if (expr.back() == '(' || expr.back() != ')') return NamedType();
    const size_t open = MatchOpen(expr, expr.size() - 1);
    if (open == std::string::npos || open == 0) return NamedType();
    const std::string callee = Trim(expr.substr(0, open));
    const std::string args = expr.substr(open + 1, expr.size() - open - 2);
    size_t dot = callee.rfind('.');
    const std::string fname = dot == std::string::npos ? callee : callee.substr(dot + 1);
    if (!IsIdent(fname)) return NamedType();
    // Must(v, err) / pkg.Must(f()): the value.
    if (fname == "Must") {
        const std::vector<std::string> a = TopLevelSplit(args);
        if (!a.empty()) return TypeOfExpr(a[0], sc, text, depth + 1);
        return NamedType();
    }
    if (dot == std::string::npos) return NamedType();
    const std::string prefix = Trim(callee.substr(0, dot));
    // pkg.F(...): the function's first result.
    if (IsIdent(prefix)) {
        const Package pkg = ImportedPackage(sc, prefix);
        if (pkg.ok) {
            for (const Entry &e : pkg.entries)
                if (e.kind == "func" && e.name == fname) return ResolveTypeExpr(FirstResult(e.signature, fname), pkg);
            return NamedType();
        }
    }
    // recv.M(...): the method's first result.
    const NamedType recv = TypeOfExpr(prefix, sc, text, depth + 1);
    Package owner;
    const Entry *m = FindMember(recv, fname, &owner);
    if (!m || m->kind != "method") return NamedType();
    return ResolveTypeExpr(FirstResult(m->signature, fname), owner);
}

}  // namespace

std::string MemberDoc(const std::string &expr, const std::string &member, const std::string &dir,
                      const std::vector<std::pair<std::string, std::string>> &context, const std::string &text) {
    if (!IsIdent(member)) return "";
    const Scope sc = MakeScope(dir, context);
    const NamedType t = TypeOfExpr(expr, sc, text, 0);
    Package owner;
    const Entry *e = FindMember(t, member, &owner);
    if (!e) return "";
    return RenderEntry(*e, owner, true);
}

std::string ExportsTable(const Package &pkg) {
    std::string out;
    for (const Entry &e : pkg.entries)
        out += e.kind + "\t" + e.name + "\t" + Summary(e) + "\t" + e.file + ":" + std::to_string(e.line) + "\n";
    return out;
}

}  // namespace gopkgdoc
