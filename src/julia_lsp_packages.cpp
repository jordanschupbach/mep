// mep-julia-lsp's index of the packages a document `using`s, beyond the
// standard library the vocabulary knows: `using Datamunge` finds
// Datamunge/src/Datamunge.jl the way Julia's load path would, analyses it
// with the same parser, and keeps its exported names, their docstrings and
// signatures, and where each is defined (hover, completion, signature help,
// definitions). Packages are cached by file and modification time.
//
// Where a package is found, in order:
//   - each JULIA_LOAD_PATH entry (default "@:@v#.#:@stdlib"): a directory of
//     packages (NAME/src/NAME.jl), or an environment (a Project.toml, or a
//     directory with one) that is the package or `develop`s it (a Manifest
//     `path`); "@" is JULIA_PROJECT or the document's project, "@v#.#" the
//     default environments under ~/.julia/environments;
//   - the project enclosing the document, when it is the package itself.
// Registry-installed packages (content-addressed under ~/.julia/packages)
// are not resolved; they stay unknown, as before.
#include "julia_lsp_internal.h"
#include "julia_lsp_packages.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace jllsp {
namespace {

std::mutex g_mutex;
thread_local bool g_building = false;
std::optional<std::vector<std::string>> g_load_path;  // tests

struct Cached {
    fs::file_time_type stamp;
    uintmax_t size = 0;
    std::shared_ptr<const Package> package;
};
std::map<std::string, Cached> g_cache;

bool ReadAll(const fs::path &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    *out = s.str();
    return true;
}

std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        at = nl + 1;
    }
    return lines;
}

// `key = "value"` at the top level of a TOML file, or in table @p table.
std::string TomlString(const std::string &text, const std::string &key, const std::string &table = "") {
    std::string current;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t ns = line.find_first_not_of(" \t");
        if (ns == std::string::npos || line[ns] == '#') continue;
        if (line[ns] == '[') {
            const size_t close = line.find(']', ns);
            current = line.substr(ns, close == std::string::npos ? std::string::npos : close + 1 - ns);
            if (current.size() > 2 && current[1] == '[') current = current.substr(1, current.size() - 2);
            continue;
        }
        if (current != table) continue;
        const size_t eq = line.find('=', ns);
        if (eq == std::string::npos) continue;
        std::string k = line.substr(ns, eq - ns);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k != key) continue;
        const size_t q1 = line.find('"', eq);
        const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return line.substr(q1 + 1, q2 - q1 - 1);
    }
    return "";
}

// The source of package @p name in environment @p project (a Project.toml).
std::string FromEnvironment(const fs::path &project, const std::string &name) {
    std::string text;
    if (!ReadAll(project, &text)) return "";
    const fs::path dir = project.parent_path();
    std::error_code ec;
    if (TomlString(text, "name") == name) {
        const fs::path src = dir / "src" / (name + ".jl");
        if (fs::is_regular_file(src, ec)) return src.string();
    }
    // A developed package: [[deps.NAME]] path = "..." in the manifest.
    for (const char *manifest : {"Manifest.toml", "JuliaManifest.toml"}) {
        std::string m;
        if (!ReadAll(dir / manifest, &m)) continue;
        const std::string path = TomlString(m, "path", "[deps." + name + "]");
        if (path.empty()) continue;
        fs::path root = path;
        if (root.is_relative()) root = dir / root;
        const fs::path src = root / "src" / (name + ".jl");
        if (fs::is_regular_file(src, ec)) return src.lexically_normal().string();
    }
    return "";
}

std::string FromEntry(const fs::path &entry, const std::string &name) {
    std::error_code ec;
    if (entry.filename() == "Project.toml" || entry.filename() == "JuliaProject.toml")
        return fs::is_regular_file(entry, ec) ? FromEnvironment(entry, name) : "";
    if (!fs::is_directory(entry, ec)) return "";
    for (const char *project : {"JuliaProject.toml", "Project.toml"})
        if (fs::is_regular_file(entry / project, ec)) return FromEnvironment(entry / project, name);
    // A package directory.
    const fs::path src = entry / name / "src" / (name + ".jl");
    return fs::is_regular_file(src, ec) ? src.string() : "";
}

fs::path EnclosingProject(const std::string &from) {
    if (from.empty()) return {};
    std::error_code ec;
    for (fs::path dir = fs::path(from).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        for (const char *project : {"JuliaProject.toml", "Project.toml"})
            if (fs::is_regular_file(dir / project, ec)) return dir / project;
        if (dir == dir.parent_path()) break;
    }
    return {};
}

std::vector<std::string> LoadPath() {
    if (g_load_path) return *g_load_path;
    const char *env = std::getenv("JULIA_LOAD_PATH");
    std::string text = env ? env : "@:@v#.#:@stdlib";
    // A trailing ':' (or an empty entry) stands for the default path.
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t colon = text.find(':', at);
        if (colon == std::string::npos) colon = text.size();
        const std::string item = text.substr(at, colon - at);
        if (item.empty()) for (const char *d : {"@", "@v#.#", "@stdlib"}) out.push_back(d);
        else out.push_back(item);
        at = colon + 1;
    }
    return out;
}

std::string Locate(const std::string &name, const std::string &from) {
    for (const std::string &entry : LoadPath()) {
        std::string found;
        if (entry == "@stdlib" || (entry.size() > 1 && entry[0] == '@' && entry != "@v#.#")) continue;
        if (entry == "@") {
            const char *project = std::getenv("JULIA_PROJECT");
            if (project && *project && std::string(project) != "@.") found = FromEntry(project, name);
            else {
                const fs::path enclosing = EnclosingProject(from);
                if (!enclosing.empty()) found = FromEnvironment(enclosing, name);
            }
        } else if (entry == "@v#.#") {
            const char *depot = std::getenv("JULIA_DEPOT_PATH");
            std::string first = depot && *depot ? std::string(depot).substr(0, std::string(depot).find(':')) : "";
            if (first.empty()) {
                const char *home = std::getenv("HOME");
                if (!home) continue;
                first = std::string(home) + "/.julia";
            }
            std::error_code ec;
            const fs::path environments = fs::path(first) / "environments";
            if (!fs::is_directory(environments, ec)) continue;
            std::vector<fs::path> versions;
            for (const auto &e : fs::directory_iterator(environments, ec))
                if (e.path().filename().string().rfind("v", 0) == 0) versions.push_back(e.path());
            std::sort(versions.rbegin(), versions.rend());
            for (const fs::path &v : versions)
                if (found.empty()) found = FromEntry(v, name);
        } else {
            found = FromEntry(entry, name);
        }
        if (!found.empty()) return found;
    }
    // The package being developed.
    const fs::path enclosing = EnclosingProject(from);
    return enclosing.empty() ? "" : FromEnvironment(enclosing, name);
}

// The name a definition binds: `area(self::Circle)` -> area, `Base.size(x)` -> size,
// `Base.:+(a, b)` -> +, `abstract type Circle <: Shape` -> Circle, `Kind` -> Kind.
std::string DefinedName(std::string sig, bool *qualified) {
    *qualified = false;
    for (const char *prefix : {"abstract type ", "mutable struct ", "struct ", "primitive type ", "@"})
        if (sig.rfind(prefix, 0) == 0) sig = sig.substr(std::string(prefix).size());
    // The callee: up to the call's '(' (an operator `:(==)` keeps its parentheses).
    size_t end = 0;
    while (end < sig.size()) {
        const char c = sig[end];
        if (c == ':' && end + 1 < sig.size() && sig[end + 1] == '(') {
            const size_t close = sig.find(')', end);
            if (close == std::string::npos) break;
            end = close + 1;
            continue;
        }
        if (c == '(' || c == ' ' || c == '{' || c == '=' || c == '<' || (c == ':' && end + 1 < sig.size() && sig[end + 1] == ':'))
            if (end > 0) break;
        ++end;
    }
    std::string callee = sig.substr(0, end);
    // Base.:+ / Base.f / Mod.Sub.f: the last component.
    for (size_t dot; (dot = callee.find('.')) != std::string::npos && dot + 1 < callee.size();) {
        *qualified = true;
        callee = callee.substr(dot + 1);
    }
    if (callee.size() > 1 && callee[0] == ':') {
        callee = callee.substr(1);
        if (callee.size() > 2 && callee.front() == '(' && callee.back() == ')') callee = callee.substr(1, callee.size() - 2);
    }
    if (callee.size() > 2 && callee.front() == '(' && callee.back() == ')') callee = callee.substr(1, callee.size() - 2);
    return callee;
}

// The signatures a Julia docstring opens with (its indented first lines).
std::vector<std::string> DocSignatures(const std::string &doc) {
    std::vector<std::string> out;
    std::istringstream in(doc);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("    ", 0) != 0) break;
        const size_t ns = line.find_first_not_of(' ');
        if (ns != std::string::npos) out.push_back(line.substr(ns));
    }
    return out;
}

struct Entry {
    std::string kind;
    std::vector<std::string> signatures, docs;
    uint32_t def = 0;
    bool have_def = false;
    bool extends_base = false;
};

void Collect(const Doc &d, int node, std::map<std::string, Entry> &entries, std::vector<std::string> &order,
             std::vector<std::string> &exports, std::map<std::string, std::string> &submodules, bool *complete) {
    const Node &n = d.N(node);
    switch (n.k) {
    case K::Block:
    case K::Toplevel:
        for (uint32_t k = 0; k < d.NKids(node); ++k) Collect(d, d.Kid(node, k), entries, order, exports, submodules, complete);
        return;
    case K::Doc:
        if (d.NKids(node) >= 2) Collect(d, d.Kid(node, 1), entries, order, exports, submodules, complete);
        return;
    case K::Const:
    case K::Global:
        if (d.NKids(node) >= 1) Collect(d, d.Kid(node, 0), entries, order, exports, submodules, complete);
        return;
    case K::Export:
        for (uint32_t k = 0; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            std::string name = d.N(c).k == K::MacroName ? std::string(d.Text(c)) : IdentName(d, c);
            if (d.N(c).k == K::MacroName && !name.empty() && name[0] != '@') name = "@" + name;
            if (!name.empty()) exports.push_back(name);
        }
        return;
    case K::Module: {
        // Submodules: only their docstring here (their names are `Package.Sub.name`).
        for (uint32_t k = 0; k < d.NKids(node); ++k) {
            const int c = d.Kid(node, k);
            if (d.N(c).k == K::Ident || d.N(c).k == K::Var) {
                const std::string name = IdentName(d, c);
                Entry &e = entries[name];
                if (e.kind.empty()) order.push_back(name);
                e.kind = "module";
                const std::string doc = DocstringFor(d, node);
                if (!doc.empty()) e.docs.push_back(doc);
                if (!e.have_def) e.def = n.b, e.have_def = true;
                submodules[name] = name;
                break;
            }
        }
        return;
    }
    case K::MacroCall: {
        const std::string text(d.Text(node).substr(0, 16));
        if (text.rfind("@reexport", 0) == 0 || text.rfind("@eval", 0) == 0 || text.rfind("@doc", 0) == 0) {
            if (text.rfind("@doc", 0) != 0) *complete = false;
        }
        // @subclass-like wrappers of a definition: their last argument.
        if (d.NKids(node) >= 2) {
            const int last = d.Kid(node, d.NKids(node) - 1);
            const K lk = d.N(last).k;
            if (lk == K::Struct || lk == K::Function || lk == K::Abstract)
                Collect(d, last, entries, order, exports, submodules, complete);
        }
        return;
    }
    case K::Call: {
        const std::string text(d.Text(node).substr(0, 8));
        if (text.rfind("include(", 0) == 0 || text.rfind("eval(", 0) == 0) *complete = false;
        return;
    }
    case K::Function:
    case K::Assign:
    case K::Struct:
    case K::Abstract:
    case K::Primitive:
    case K::Macro: {
        const std::string sig = SignatureText(d, node);
        if (sig.empty()) return;
        bool qualified = false;
        std::string name = DefinedName(sig, &qualified);
        if (n.k == K::Macro) name = "@" + name;
        if (name.empty() || name.find_first_of(" ,;") != std::string::npos) return;
        // Methods of functions owned elsewhere count only when they extend Base.
        const bool extends_base = qualified && sig.rfind("Base.", 0) == 0;
        if (qualified && !extends_base) return;
        Entry &e = entries[name];
        if (e.kind.empty()) {
            order.push_back(name);
            const bool call = sig.find('(') != std::string::npos && sig.find('(') < sig.find('=');
            e.kind = n.k == K::Struct || n.k == K::Abstract || n.k == K::Primitive ? "type"
                     : n.k == K::Macro ? "macro"
                     : n.k == K::Function || call ? "function"
                     : "constant";
        }
        e.extends_base = e.extends_base || extends_base;
        if (!e.have_def) e.def = n.b, e.have_def = true;
        const std::string doc = DocstringFor(d, node);
        if (!doc.empty()) {
            if (std::find(e.docs.begin(), e.docs.end(), doc) == e.docs.end()) e.docs.push_back(doc);
            for (const std::string &s : DocSignatures(doc)) {
                // A function's signatures are calls; a bare name (a generic function's own docstring) is not one.
                if (e.kind == "function" && s.find('(') == std::string::npos) continue;
                if (std::find(e.signatures.begin(), e.signatures.end(), s) == e.signatures.end()) e.signatures.push_back(s);
            }
        }
        return;
    }
    default:
        return;
    }
}

std::shared_ptr<const Package> Build(const std::string &name, const std::string &file) {
    std::string text;
    if (!ReadAll(file, &text)) return nullptr;
    // The package's own `using`s are not followed.
    g_building = true;
    const auto analysis = JuliaLspAnalyze(SplitLines(text), file);
    g_building = false;
    const Doc &d = analysis->d;
    auto package = std::make_shared<Package>();
    package->name = name;
    package->file = file;
    // The module the file defines (`module NAME ... end`), else the whole file.
    int body = d.root;
    if (d.root >= 0) {
        for (uint32_t k = 0; k < d.NKids(d.root); ++k) {
            int c = d.Kid(d.root, k);
            if (d.N(c).k == K::Doc && d.NKids(c) >= 2) c = d.Kid(c, 1);
            if (d.N(c).k != K::Module) continue;
            bool named = false;
            for (uint32_t j = 0; j < d.NKids(c); ++j) {
                const int m = d.Kid(c, j);
                if ((d.N(m).k == K::Ident || d.N(m).k == K::Var) && IdentName(d, m) == name) named = true;
                if (named && d.N(m).k == K::Block) body = m;
            }
            if (named) {
                package->doc = DocstringFor(d, c);
                break;
            }
        }
    }
    std::map<std::string, Entry> entries;
    std::vector<std::string> order, exports;
    std::map<std::string, std::string> submodules;
    bool complete = true;
    if (body >= 0) Collect(d, body, entries, order, exports, submodules, &complete);
    package->complete = complete;
    const std::set<std::string> exported(exports.begin(), exports.end());
    for (const std::string &e : exports) package->exports.push_back(e);
    // Exported names defined elsewhere (`using .Sub: f` then `export f`) still get a row.
    for (const std::string &e : exports)
        if (!entries.count(e)) {
            entries[e].kind = e[0] == '@' ? "macro" : "function";
            order.push_back(e);
        }
    auto keep = [&](const std::string &s) -> const char * {
        package->strings.push_back(std::make_unique<std::string>(s));
        return package->strings.back()->c_str();
    };
    for (const std::string &n : order) {
        const Entry &e = entries[n];
        PackageRow row;
        std::string sigs, docs;
        for (const std::string &s : e.signatures) sigs += (sigs.empty() ? "" : "\n") + s;
        for (const std::string &s : e.docs) docs += (docs.empty() ? "" : "\n\n\n---\n\n") + s;
        row.api = JuliaLspApi{keep(name), keep(n), keep(e.kind), keep(sigs), keep(docs)};
        row.exported = exported.count(n) != 0;
        row.extends_base = e.extends_base;
        if (e.have_def) {
            int line = 0, col = 0;
            d.Pos(e.def, &line, &col);
            row.line = line;
            row.col = col;
            row.has_location = true;
        }
        package->by_name.emplace(n, package->rows.size());
        package->rows.push_back(std::move(row));
    }
    return package;
}

}  // namespace

std::shared_ptr<const Package> FindPackage(const std::string &name, const std::string &from) {
    if (name.empty() || name.find('.') != std::string::npos) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::string file = Locate(name, from);
    if (file.empty()) return nullptr;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    const auto size = fs::file_size(file, ec);
    auto it = g_cache.find(file);
    if (it != g_cache.end() && it->second.stamp == stamp && it->second.size == size) return it->second.package;
    auto package = Build(name, file);
    if (package) g_cache[file] = Cached{stamp, size, package};
    return package;
}

bool BuildingPackage() { return g_building; }

std::vector<const PackageRow *> PackageRows(const Package &p, std::string_view name) {
    std::vector<const PackageRow *> out;
    auto [lo, hi] = p.by_name.equal_range(std::string(name));
    for (auto it = lo; it != hi; ++it) out.push_back(&p.rows[it->second]);
    return out;
}

}  // namespace jllsp

void JuliaLspSetLoadPath(const std::vector<std::string> *entries) {
    std::lock_guard<std::mutex> lock(jllsp::g_mutex);
    if (entries) jllsp::g_load_path = *entries;
    else jllsp::g_load_path.reset();
    jllsp::g_cache.clear();
}
