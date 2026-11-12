// LuaCATS meta stubs for mep-lua-lsp: see lua_lsp_meta.h.
#include "lua_lsp_meta.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#include "json.h"

namespace fs = std::filesystem;

namespace {

bool IsWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

void SkipSpaces(const std::string &s, size_t &i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
}

// The index of the bracket closing the one at `open`, or npos. Counts every
// kind of bracket together, which is enough for type expressions.
size_t Matching(const std::string &s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        const char c = s[i];
        if (c == '(' || c == '[' || c == '{' || c == '<') depth++;
        else if (c == ')' || c == ']' || c == '}' || c == '>') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::string ReadType(const std::string &s, size_t &i);

// One alternative of a type: a name, `fun(...): R`, `{...}`, `(...)`, a literal, with `<...>`, `[]` and `?` after.
bool ReadAtom(const std::string &s, size_t &i, std::string *out) {
    SkipSpaces(s, i);
    if (i >= s.size()) return false;
    const size_t start = i;
    if (s.compare(i, 4, "fun(") == 0) {
        const size_t close = Matching(s, i + 3);
        if (close == std::string::npos) return false;
        std::string atom = s.substr(i, close + 1 - i);
        i = close + 1;
        size_t k = i;
        SkipSpaces(s, k);
        if (k < s.size() && s[k] == ':') {
            k++;
            const std::string ret = ReadType(s, k);
            if (!ret.empty()) {
                atom += ": " + ret;
                i = k;
            }
        }
        *out = atom;
        return true;
    }
    if (s[i] == '(' || s[i] == '{') {
        const size_t close = Matching(s, i);
        if (close == std::string::npos) return false;
        i = close + 1;
    } else if (s[i] == '"' || s[i] == '\'' || s[i] == '`') {
        const size_t close = s.find(s[i], i + 1);
        if (close == std::string::npos) return false;
        i = close + 1;
    } else if (s.compare(i, 3, "...") == 0) {
        i += 3;
    } else {
        while (i < s.size() && (IsWord(s[i]) || s[i] == '.' || s[i] == '-')) i++;
        if (i == start) return false;
    }
    for (;;) {
        if (i < s.size() && s[i] == '<') {
            const size_t close = Matching(s, i);
            if (close == std::string::npos) break;
            i = close + 1;
        } else if (s.compare(i, 2, "[]") == 0) {
            i += 2;
        } else if (i < s.size() && s[i] == '?') {
            i++;
        } else {
            break;
        }
    }
    *out = s.substr(start, i - start);
    return true;
}

std::string ReadType(const std::string &s, size_t &i) {
    std::string out;
    if (!ReadAtom(s, i, &out)) return std::string();
    for (;;) {
        size_t k = i;
        SkipSpaces(s, k);
        if (k >= s.size() || s[k] != '|') break;
        k++;
        std::string next;
        if (!ReadAtom(s, k, &next)) break;
        out += "|" + next;
        i = k;
    }
    return out;
}

// Top-level comma-separated pieces of s[a, b).
std::vector<std::string> SplitTop(const std::string &s, size_t a, size_t b) {
    std::vector<std::string> out;
    int depth = 0;
    size_t from = a;
    for (size_t i = a; i < b; i++) {
        const char c = s[i];
        if (c == '(' || c == '[' || c == '{' || c == '<') depth++;
        else if (c == ')' || c == ']' || c == '}' || c == '>') depth--;
        else if (c == ',' && depth == 0) {
            out.push_back(Trim(s.substr(from, i - from)));
            from = i + 1;
        }
    }
    const std::string last = Trim(s.substr(from, b - from));
    if (!last.empty() || !out.empty()) out.push_back(last);
    return out;
}

std::string Join(const std::vector<std::string> &v) {
    std::string out;
    for (const std::string &s : v) {
        if (!out.empty()) out += "\n";
        out += s;
    }
    // Trim blank lines at either end.
    while (!out.empty() && out.back() == '\n') out.pop_back();
    size_t a = 0;
    while (a < out.size() && out[a] == '\n') a++;
    return out.substr(a);
}

struct Pending {
    std::vector<std::string> doc;
    std::vector<LuaMetaParam> params;
    std::vector<LuaMetaReturn> returns;
    std::vector<std::string> overloads;
    std::string cls, enm, type;
    bool deprecated = false;
};

class Parser {
public:
    Parser(LuaMetaModule &m) : m_(m) {}

    void Run(const std::string &text) {
        std::istringstream in(text);
        std::string raw;
        int line_no = -1;
        while (std::getline(in, raw)) {
            line_no++;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            const std::string line = Trim(raw);
            if (!enum_open_.empty()) {
                EnumLine(line);
                continue;
            }
            if (line.compare(0, 3, "---") == 0 && (line.size() == 3 || line[3] != '-')) {
                Annotation(line.substr(3), line_no);
                continue;
            }
            if (line.empty() || line.compare(0, 2, "--") == 0) {
                p_ = Pending();
                current_class_.clear();
                continue;
            }
            Code(line, line_no);
            p_ = Pending();
            current_class_.clear();
        }
        if (m_.root_type.empty()) {
            if (m_.classes.count(m_.meta_name)) m_.root_type = m_.meta_name;
        }
    }

private:
    LuaMetaModule &m_;
    Pending p_;
    std::string current_class_;
    std::map<std::string, std::string> path_type_;  // "datamunge.Dual" (as written) -> type
    std::string enum_open_;                          // the enum whose table is being read

    std::string TypeOfPath(const std::string &path) const {
        auto it = path_type_.find(path);
        return it == path_type_.end() ? path : it->second;
    }

    void Annotation(const std::string &body, int line_no) {
        if (body.empty() || body[0] != '@') {
            std::string d = body;
            if (!d.empty() && d[0] == ' ') d.erase(0, 1);
            p_.doc.push_back(d);
            return;
        }
        size_t i = 1;
        while (i < body.size() && (IsWord(body[i]))) i++;
        const std::string tag = body.substr(1, i - 1);
        SkipSpaces(body, i);
        if (tag == "meta") {
            m_.meta_name = Trim(body.substr(i));
        } else if (tag == "class") {
            if (i < body.size() && body[i] == '(') {  // (exact) / (partial)
                const size_t c = body.find(')', i);
                i = c == std::string::npos ? body.size() : c + 1;
                SkipSpaces(body, i);
            }
            size_t s = i;
            while (i < body.size() && (IsWord(body[i]) || body[i] == '.')) i++;
            const std::string name = body.substr(s, i - s);
            if (name.empty()) return;
            LuaMetaClass &c = m_.classes[name];
            c.name = name;
            if (c.doc.empty()) c.doc = Join(p_.doc);
            c.line = line_no;
            SkipSpaces(body, i);
            if (i < body.size() && body[i] == ':') {
                for (const std::string &b : SplitTop(body, i + 1, body.size())) {
                    if (!b.empty()) c.bases.push_back(b);
                }
            }
            p_.cls = name;
            current_class_ = name;
        } else if (tag == "enum") {
            if (i < body.size() && body[i] == '(') {
                const size_t c = body.find(')', i);
                i = c == std::string::npos ? body.size() : c + 1;
                SkipSpaces(body, i);
            }
            size_t s = i;
            while (i < body.size() && (IsWord(body[i]) || body[i] == '.')) i++;
            const std::string name = body.substr(s, i - s);
            if (name.empty()) return;
            LuaMetaEnum &e = m_.enums[name];
            e.name = name;
            e.doc = Join(p_.doc);
            e.line = line_no;
            p_.enm = name;
        } else if (tag == "field") {
            if (current_class_.empty()) return;
            for (const char *scope : {"public ", "private ", "protected ", "package "}) {
                if (body.compare(i, std::strlen(scope), scope) == 0) {
                    i += std::strlen(scope);
                    SkipSpaces(body, i);
                }
            }
            size_t s = i;
            while (i < body.size() && IsWord(body[i])) i++;
            LuaMetaField f;
            f.name = body.substr(s, i - s);
            if (f.name.empty()) return;  // [key] fields: not modelled
            if (i < body.size() && body[i] == '?') i++;
            f.type = ReadType(body, i);
            f.doc = Trim(body.substr(i));
            if (!f.doc.empty() && f.doc[0] == '#') f.doc = Trim(f.doc.substr(1));
            m_.classes[current_class_].fields.push_back(f);
            LuaMetaMember mm;
            mm.kind = LuaMetaMember::Field;
            mm.name = f.name;
            mm.type = f.type;
            mm.doc = f.doc;
            m_.members[current_class_].push_back(mm);
        } else if (tag == "param") {
            LuaMetaParam prm;
            size_t s = i;
            if (body.compare(i, 3, "...") == 0) {
                i += 3;
            } else {
                while (i < body.size() && IsWord(body[i])) i++;
            }
            prm.name = body.substr(s, i - s);
            if (prm.name.empty()) return;
            if (i < body.size() && body[i] == '?') {
                prm.optional = true;
                i++;
            }
            prm.type = ReadType(body, i);
            if (!prm.type.empty() && prm.type.back() == '?' && prm.type.find('|') == std::string::npos) {
                // `x integer?` is optional too.
                prm.optional = true;
            }
            prm.doc = Trim(body.substr(i));
            if (!prm.doc.empty() && prm.doc[0] == '#') prm.doc = Trim(prm.doc.substr(1));
            p_.params.push_back(prm);
        } else if (tag == "return") {
            LuaMetaReturn r;
            r.type = ReadType(body, i);
            std::string rest = Trim(body.substr(i));
            const size_t hash = rest.find('#');
            if (hash != std::string::npos) {
                r.name = Trim(rest.substr(0, hash));
                r.doc = Trim(rest.substr(hash + 1));
            } else {
                r.doc = rest;
            }
            p_.returns.push_back(r);
        } else if (tag == "overload") {
            const std::string o = Trim(body.substr(i));
            if (!o.empty()) {
                if (!p_.cls.empty()) m_.classes[p_.cls].overloads.push_back(o);
                else p_.overloads.push_back(o);
            }
        } else if (tag == "type") {
            p_.type = ReadType(body, i);
        } else if (tag == "deprecated") {
            p_.deprecated = true;
        }
        // @alias, @see, @nodiscard, @generic, @async, @diagnostic, ...: not needed for hover.
    }

    void AddFunction(const std::string &path, bool method_sep, const std::string &args, int line_no) {
        std::string owner_path, name;
        const size_t cut = path.find_last_of(".:");
        if (cut == std::string::npos) {
            name = path;
        } else {
            owner_path = path.substr(0, cut);
            name = path.substr(cut + 1);
        }
        auto f = std::make_unique<LuaMetaFunc>();
        f->owner = owner_path.empty() ? std::string() : TypeOfPath(owner_path);
        f->name = name;
        f->method = method_sep;
        f->doc = Join(p_.doc);
        f->returns = p_.returns;
        f->overloads = p_.overloads;
        f->deprecated = p_.deprecated;
        f->line = line_no;
        for (const std::string &a : SplitTop(args, 0, args.size())) {
            if (a.empty()) continue;
            LuaMetaParam prm;
            prm.name = a;
            for (const LuaMetaParam &q : p_.params) {
                if (q.name == a) prm = q;
            }
            f->params.push_back(prm);
        }
        LuaMetaMember mm;
        mm.kind = LuaMetaMember::Function;
        mm.name = name;
        mm.func = f.get();
        m_.members[f->owner].push_back(mm);
        m_.funcs.push_back(std::move(f));
    }

    // Reads a path such as a.b.c or a.b:c at the start of s; false when s has none.
    static bool ReadPath(const std::string &s, size_t &i, std::string *path, bool *method) {
        const size_t start = i;
        *method = false;
        for (;;) {
            const size_t w = i;
            while (i < s.size() && IsWord(s[i])) i++;
            if (i == w || std::isdigit(static_cast<unsigned char>(s[w]))) {
                i = start;
                return false;
            }
            if (i < s.size() && (s[i] == '.' || s[i] == ':') && i + 1 < s.size() && IsWord(s[i + 1]) && !*method) {
                if (s[i] == ':') *method = true;
                i++;
                continue;
            }
            break;
        }
        *path = s.substr(start, i - start);
        return true;
    }

    void Code(const std::string &line, int line_no) {
        size_t i = 0;
        bool local = false;
        if (line.compare(0, 6, "local ") == 0) {
            local = true;
            i = 6;
            SkipSpaces(line, i);
        }
        if (line.compare(i, 9, "function ") == 0) {
            i += 9;
            SkipSpaces(line, i);
            std::string path;
            bool method = false;
            if (!ReadPath(line, i, &path, &method)) return;
            SkipSpaces(line, i);
            if (i >= line.size() || line[i] != '(') return;
            const size_t close = line.find(')', i);
            if (close == std::string::npos) return;
            if (local) return;  // a local helper function: not part of the module
            AddFunction(path, method, line.substr(i + 1, close - i - 1), line_no);
            return;
        }
        if (line.compare(0, 7, "return ") == 0) {
            const std::string what = Trim(line.substr(7));
            if (m_.root_type.empty() && !what.empty()) {
                auto it = path_type_.find(what);
                if (it != path_type_.end()) m_.root_type = it->second;
                else if (m_.classes.count(what)) m_.root_type = what;
            }
            return;
        }
        std::string path;
        bool method = false;
        if (!ReadPath(line, i, &path, &method) || method) return;
        SkipSpaces(line, i);
        if (i >= line.size() || line[i] != '=' || (i + 1 < line.size() && line[i + 1] == '=')) return;
        i++;
        SkipSpaces(line, i);
        const std::string value = line.substr(i);
        if (value.compare(0, 8, "function") == 0 && (value.size() == 8 || !IsWord(value[8]))) {
            const size_t open = value.find('(');
            const size_t close = value.find(')', open == std::string::npos ? 0 : open);
            if (open != std::string::npos && close != std::string::npos && !local)
                AddFunction(path, false, value.substr(open + 1, close - open - 1), line_no);
            return;
        }
        const size_t dot = path.rfind('.');
        const std::string owner = dot == std::string::npos ? std::string() : TypeOfPath(path.substr(0, dot));
        const std::string name = dot == std::string::npos ? path : path.substr(dot + 1);
        if (!p_.cls.empty()) {
            path_type_[path] = p_.cls;
            if (!local && dot != std::string::npos) {
                LuaMetaMember mm;
                mm.kind = LuaMetaMember::Class;
                mm.name = name;
                mm.type = p_.cls;
                m_.members[owner].push_back(mm);
            }
            return;
        }
        if (!p_.enm.empty()) {
            path_type_[path] = p_.enm;
            if (!local && dot != std::string::npos) {
                LuaMetaMember mm;
                mm.kind = LuaMetaMember::Enum;
                mm.name = name;
                mm.type = p_.enm;
                m_.members[owner].push_back(mm);
            }
            if (!value.empty() && value[0] == '{') {
                enum_open_ = p_.enm;
                EnumLine(value.substr(1));
            }
            return;
        }
        if (local || dot == std::string::npos) return;
        // A plain value: a field of its owner.
        LuaMetaMember mm;
        mm.kind = LuaMetaMember::Field;
        mm.name = name;
        mm.doc = Join(p_.doc);
        if (!p_.type.empty()) mm.type = p_.type;
        else if (!value.empty() && (value[0] == '"' || value[0] == '\'' || value[0] == '[')) mm.type = "string";
        else if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '-')) mm.type = "number";
        else if (value.compare(0, 4, "true") == 0 || value.compare(0, 5, "false") == 0) mm.type = "boolean";
        else if (!value.empty() && value[0] == '{') mm.type = "table";
        m_.members[owner].push_back(mm);
    }

    // `Name = value,` entries of an enum table, up to its closing brace.
    void EnumLine(const std::string &line) {
        LuaMetaEnum &e = m_.enums[enum_open_];
        size_t i = 0;
        while (i < line.size()) {
            SkipSpaces(line, i);
            if (i >= line.size()) break;
            if (line[i] == '}') {
                enum_open_.clear();
                return;
            }
            if (line.compare(i, 2, "--") == 0) break;
            std::string key;
            if (line[i] == '[') {
                const size_t close = line.find(']', i);
                if (close == std::string::npos) break;
                key = Trim(line.substr(i + 1, close - i - 1));
                if (key.size() >= 2 && (key[0] == '"' || key[0] == '\'')) key = key.substr(1, key.size() - 2);
                i = close + 1;
            } else {
                const size_t s = i;
                while (i < line.size() && IsWord(line[i])) i++;
                key = line.substr(s, i - s);
                if (key.empty()) {
                    i++;
                    continue;
                }
            }
            SkipSpaces(line, i);
            std::string value;
            if (i < line.size() && line[i] == '=') {
                i++;
                const size_t s = i;
                int depth = 0;
                while (i < line.size()) {
                    const char c = line[i];
                    if (c == '{' || c == '(') depth++;
                    else if (c == ')') depth--;
                    else if (c == '}') {
                        if (depth == 0) break;
                        depth--;
                    } else if ((c == ',' || c == ';') && depth == 0) break;
                    i++;
                }
                value = Trim(line.substr(s, i - s));
            }
            if (!key.empty()) e.members.emplace_back(key, value);
            if (i < line.size() && (line[i] == ',' || line[i] == ';')) i++;
        }
    }
};

// ---- files -----------------------------------------------------------------------------------------------------

bool FileStamp(const std::string &path, long long *stamp) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return false;
    const auto size = fs::file_size(path, ec);
    if (ec) return false;
    *stamp = static_cast<long long>(t.time_since_epoch().count()) ^ (static_cast<long long>(size) << 1);
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

// JSON with // and /* */ comments and trailing commas (".luarc.jsonc") as plain JSON.
std::string StripJsonc(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == '"') {
            const size_t start = i;
            for (i++; i < s.size() && s[i] != '"'; i++) {
                if (s[i] == '\\') i++;
            }
            out += s.substr(start, i - start + 1);
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') i++;
            out += '\n';
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            const size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? s.size() : e + 1;
            continue;
        }
        if (c == ',') {
            size_t j = i + 1;
            while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) j++;
            if (j < s.size() && (s[j] == '}' || s[j] == ']')) continue;
        }
        out += c;
    }
    return out;
}

std::string ExpandHome(const std::string &p) {
    if (p.size() >= 1 && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        const char *home = std::getenv("HOME");
        if (home) return std::string(home) + p.substr(1);
    }
    return p;
}

std::vector<std::string> LuarcLibraries(const std::string &luarc) {
    std::vector<std::string> out;
    std::string text;
    if (!ReadAll(luarc, &text)) return out;
    Json j;
    if (!Json::Parse(StripJsonc(text), &j) || !j.is_object()) return out;
    const fs::path dir = fs::path(luarc).parent_path();
    auto take = [&](const Json &arr) {
        if (!arr.is_array()) return;
        for (const Json &e : arr.items()) {
            if (!e.is_string()) continue;
            std::string p = e.as_string();
            if (p.empty() || p[0] == '$') continue;  // ${3rd}/..., ${addons}: lua-language-server's own
            p = ExpandHome(p);
            fs::path fp(p);
            if (fp.is_relative()) fp = dir / fp;
            out.push_back(fp.lexically_normal().string());
        }
    };
    take(j.get("workspace.library"));
    take(j.get("Lua.workspace.library"));
    take(j.get("workspace").get("library"));
    take(j.get("Lua").get("workspace").get("library"));
    return out;
}

std::mutex g_mutex;

struct CachedStub {
    long long stamp = 0;
    std::shared_ptr<const LuaMetaModule> module;  // null: not a meta file
};
std::map<std::string, CachedStub> g_stubs;

struct CachedLuarc {
    long long stamp = 0;
    std::vector<std::string> libs;
};
std::map<std::string, CachedLuarc> g_luarcs;

}  // namespace

// ---- LuaMetaModule ---------------------------------------------------------------------------------------------

const LuaMetaClass *LuaMetaModule::Class(const std::string &name) const {
    auto it = classes.find(name);
    return it == classes.end() ? nullptr : &it->second;
}

const LuaMetaEnum *LuaMetaModule::Enum(const std::string &name) const {
    auto it = enums.find(name);
    return it == enums.end() ? nullptr : &it->second;
}

std::vector<const LuaMetaMember *> LuaMetaModule::Members(const std::string &type) const {
    std::vector<const LuaMetaMember *> out;
    std::set<std::string> seen_names, seen_types;
    std::vector<std::string> queue{type};
    for (size_t q = 0; q < queue.size() && q < 64; q++) {
        const std::string t = queue[q];
        if (!seen_types.insert(t).second) continue;
        auto it = members.find(t);
        if (it != members.end()) {
            for (const LuaMetaMember &m : it->second) {
                if (seen_names.insert(m.name).second) out.push_back(&m);
            }
        }
        if (const LuaMetaClass *c = Class(t)) {
            for (const std::string &b : c->bases) queue.push_back(b);
        }
    }
    return out;
}

const LuaMetaMember *LuaMetaModule::Member(const std::string &type, const std::string &name) const {
    std::set<std::string> seen_types;
    std::vector<std::string> queue{type};
    for (size_t q = 0; q < queue.size() && q < 64; q++) {
        const std::string t = queue[q];
        if (!seen_types.insert(t).second) continue;
        auto it = members.find(t);
        if (it != members.end()) {
            for (const LuaMetaMember &m : it->second) {
                if (m.name == name) return &m;
            }
        }
        if (const LuaMetaClass *c = Class(t)) {
            for (const std::string &b : c->bases) queue.push_back(b);
        }
    }
    return nullptr;
}

std::shared_ptr<const LuaMetaModule> LuaMetaParse(const std::string &text, const std::string &path) {
    auto m = std::make_shared<LuaMetaModule>();
    m->path = path;
    Parser p(*m);
    p.Run(text);
    return m;
}

std::vector<std::string> LuaMetaLibraryDirs(const std::string &doc_path) {
    std::vector<std::string> out;
    if (const char *env = std::getenv("MEP_LUA_LIBRARY")) {
#if defined(_WIN32)
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string cur;
        for (const char *p = env;; p++) {
            if (*p == sep || *p == 0) {
                if (!cur.empty()) out.push_back(ExpandHome(cur));
                cur.clear();
                if (*p == 0) break;
            } else {
                cur += *p;
            }
        }
    }
    if (doc_path.empty()) return out;
    std::error_code ec;
    fs::path dir = fs::path(doc_path).parent_path();
    for (int depth = 0; depth < 64 && !dir.empty(); depth++) {
        for (const char *name : {".luarc.json", ".luarc.jsonc"}) {
            const fs::path f = dir / name;
            long long stamp = 0;
            if (!FileStamp(f.string(), &stamp)) continue;
            std::vector<std::string> libs;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_luarcs.find(f.string());
                if (it != g_luarcs.end() && it->second.stamp == stamp) libs = it->second.libs;
                else {
                    libs = LuarcLibraries(f.string());
                    g_luarcs[f.string()] = {stamp, libs};
                }
            }
            out.insert(out.end(), libs.begin(), libs.end());
            return out;  // the nearest one wins, as lua-language-server's workspace root does
        }
        const fs::path up = dir.parent_path();
        if (up == dir) break;
        dir = up;
    }
    return out;
}

std::shared_ptr<const LuaMetaModule> LuaMetaFind(const std::vector<std::string> &dirs, const std::string &name) {
    if (name.empty()) return nullptr;
    std::string rel;
    for (char c : name) {
        if (c == '.') rel += '/';
        else if (IsWord(c) || c == '-') rel += c;
        else return nullptr;  // no "..", no absolute paths
    }
    for (const std::string &d : dirs) {
        for (const std::string &cand : {d + "/" + rel + ".lua", d + "/" + rel + "/init.lua"}) {
            long long stamp = 0;
            if (!FileStamp(cand, &stamp)) continue;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it = g_stubs.find(cand);
                if (it != g_stubs.end() && it->second.stamp == stamp) {
                    if (it->second.module) return it->second.module;
                    continue;
                }
            }
            std::string text;
            if (!ReadAll(cand, &text)) continue;
            // Only a declared meta file: an ordinary Lua module is code, not documentation.
            std::shared_ptr<const LuaMetaModule> mod;
            std::istringstream in(text.substr(0, 4096));
            std::string l;
            for (int k = 0; k < 20 && std::getline(in, l); k++) {
                if (Trim(l).compare(0, 8, "---@meta") == 0) {
                    mod = LuaMetaParse(text, cand);
                    break;
                }
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stubs[cand] = {stamp, mod};
            if (mod) return mod;
        }
    }
    return nullptr;
}

// ---- rendering -------------------------------------------------------------------------------------------------

std::string LuaMetaClassOfType(const std::string &type) {
    std::string t = Trim(type);
    // A union: the first alternative that is not nil.
    std::vector<std::string> alts;
    {
        int depth = 0;
        size_t from = 0;
        for (size_t i = 0; i < t.size(); i++) {
            const char c = t[i];
            if (c == '(' || c == '[' || c == '{' || c == '<') depth++;
            else if (c == ')' || c == ']' || c == '}' || c == '>') depth--;
            else if (c == '|' && depth == 0) {
                alts.push_back(Trim(t.substr(from, i - from)));
                from = i + 1;
            }
        }
        alts.push_back(Trim(t.substr(from)));
    }
    for (std::string a : alts) {
        while (!a.empty() && a.back() == '?') a.pop_back();
        if (a.empty() || a == "nil") continue;
        for (char c : a) {
            if (!IsWord(c) && c != '.') return std::string();
        }
        return a;
    }
    return std::string();
}

std::string LuaMetaParamText(const LuaMetaParam &p) {
    std::string s = p.name;
    if (p.optional && p.name != "...") s += "?";
    if (!p.type.empty()) {
        std::string t = p.type;
        if (p.optional && t.size() > 1 && t.back() == '?' && t.find('|') == std::string::npos) t.pop_back();
        s += ": " + t;
    }
    return s;
}

namespace {

std::string ReturnsText(const std::vector<LuaMetaReturn> &rs) {
    std::string s;
    for (const LuaMetaReturn &r : rs) {
        if (!s.empty()) s += ", ";
        s += r.type.empty() ? "any" : r.type;
    }
    return s;
}

std::string Footer(const LuaMetaModule &m) {
    const std::string file = fs::path(m.path).filename().string();
    return "\n\n*" + (m.meta_name.empty() ? file : "`" + m.meta_name + "`") + " (LuaCATS stub" +
           (m.meta_name.empty() ? "" : " " + file) + ")*";
}

std::string Display(const LuaMetaFunc &f) {
    if (f.owner.empty()) return f.name;
    return f.owner + (f.method ? ":" : ".") + f.name;
}

}  // namespace

std::string LuaMetaSignature(const LuaMetaFunc &f, const std::string &display) {
    std::string s = (display.empty() ? Display(f) : display) + "(";
    for (size_t i = 0; i < f.params.size(); i++) s += (i ? ", " : "") + LuaMetaParamText(f.params[i]);
    return s + ")";
}

std::string LuaMetaFuncHover(const LuaMetaModule &m, const LuaMetaFunc &f, const std::string &display) {
    std::string out = "```lua\nfunction " + LuaMetaSignature(f, display);
    if (!f.returns.empty()) out += " -> " + ReturnsText(f.returns);
    out += "\n```";
    if (f.deprecated) out += "\n\n**Deprecated.**";
    if (!f.doc.empty()) out += "\n\n" + f.doc;
    bool param_docs = false, return_docs = false;
    for (const LuaMetaParam &p : f.params) param_docs = param_docs || !p.doc.empty();
    for (const LuaMetaReturn &r : f.returns) return_docs = return_docs || !r.doc.empty();
    if (param_docs) {
        out += "\n\n**Parameters**\n";
        for (const LuaMetaParam &p : f.params) {
            out += "\n- `" + p.name + "`";
            if (!p.type.empty()) out += " `" + p.type + "`";
            if (p.optional) out += " *(optional)*";
            if (!p.doc.empty()) out += ": " + p.doc;
        }
    }
    if (return_docs) {
        out += "\n\n**Returns**\n";
        for (const LuaMetaReturn &r : f.returns) {
            out += "\n- `" + (r.type.empty() ? std::string("any") : r.type) + "`";
            if (!r.name.empty()) out += " " + r.name;
            if (!r.doc.empty()) out += ": " + r.doc;
        }
    }
    if (!f.overloads.empty()) {
        out += "\n\n**Overloads**\n\n```lua";
        for (const std::string &o : f.overloads) out += "\n" + o;
        out += "\n```";
    }
    return out + Footer(m);
}

std::vector<LuaMetaParam> LuaMetaFunParams(const std::string &fun) {
    std::vector<LuaMetaParam> out;
    const size_t open = fun.find('(');
    if (open == std::string::npos) return out;
    const size_t close = Matching(fun, open);
    if (close == std::string::npos) return out;
    for (const std::string &piece : SplitTop(fun, open + 1, close)) {
        if (piece.empty()) continue;
        LuaMetaParam p;
        const size_t colon = piece.find(':');
        p.name = Trim(piece.substr(0, colon));
        if (colon != std::string::npos) p.type = Trim(piece.substr(colon + 1));
        if (!p.name.empty() && p.name.back() == '?') {
            p.optional = true;
            p.name.pop_back();
        }
        out.push_back(p);
    }
    return out;
}

std::string LuaMetaFunReturn(const std::string &fun) {
    const size_t open = fun.find('(');
    if (open == std::string::npos) return std::string();
    const size_t close = Matching(fun, open);
    if (close == std::string::npos) return std::string();
    size_t i = close + 1;
    SkipSpaces(fun, i);
    if (i >= fun.size() || fun[i] != ':') return std::string();
    i++;
    return ReadType(fun, i);
}

std::string LuaMetaClassHover(const LuaMetaModule &m, const LuaMetaClass &c) {
    std::string out = "```lua\nclass " + c.name;
    for (size_t i = 0; i < c.bases.size(); i++) out += (i ? ", " : " : ") + c.bases[i];
    out += "\n```";
    if (!c.doc.empty()) out += "\n\n" + c.doc;
    if (!c.overloads.empty()) {
        out += "\n\n**Constructors**\n\n```lua";
        for (const std::string &o : c.overloads) {
            std::string sig = c.name + "(";
            const auto ps = LuaMetaFunParams(o);
            for (size_t i = 0; i < ps.size(); i++) sig += (i ? ", " : "") + LuaMetaParamText(ps[i]);
            out += "\n" + sig + ")";
        }
        out += "\n```";
    }
    // Fields and methods, inherited ones included.
    std::string fields, methods, statics;
    for (const LuaMetaMember *mm : m.Members(c.name)) {
        if (mm->kind == LuaMetaMember::Field) {
            fields += "\n- `" + mm->name + "`" + (mm->type.empty() ? "" : " `" + mm->type + "`") +
                      (mm->doc.empty() ? "" : ": " + mm->doc);
        } else if (mm->kind == LuaMetaMember::Function) {
            std::string &dst = mm->func->method ? methods : statics;
            dst += (dst.empty() ? "" : ", ") + std::string("`") + mm->name + "`";
        }
    }
    if (!fields.empty()) out += "\n\n**Fields**\n" + fields;
    if (!methods.empty()) out += "\n\n**Methods:** " + methods;
    if (!statics.empty()) out += "\n\n**Static functions:** " + statics;
    return out + Footer(m);
}

std::string LuaMetaEnumHover(const LuaMetaModule &m, const LuaMetaEnum &e) {
    std::string out = "```lua\nenum " + e.name + "\n```";
    if (!e.doc.empty()) out += "\n\n" + e.doc;
    if (!e.members.empty()) {
        out += "\n\n```lua";
        for (const auto &kv : e.members) out += "\n" + kv.first + (kv.second.empty() ? "" : " = " + kv.second);
        out += "\n```";
    }
    return out + Footer(m);
}
