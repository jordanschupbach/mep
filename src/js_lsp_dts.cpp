// TypeScript declaration files for mep-js-lsp: see js_lsp_dts.h.
#include "js_lsp_dts.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#include "js_lsp_internal.h"
#include "json.h"

namespace fs = std::filesystem;

using jsparse::Node;
using jsparse::NodeType;

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Whitespace runs (line breaks included) as one space.
std::string OneLine(std::string_view s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (IsSpace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

bool EndsWith(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ---- reading a declaration file -------------------------------------------------------------------------------

class Reader {
public:
    Reader(JsDtsModule &m, const std::string &text, const jsparse::ParseResult &parse)
        : m_(m), text_(text), parse_(parse) {}

    void Run() { Statements(parse_.program->body_list(), m_.root); }

private:
    JsDtsModule &m_;
    const std::string &text_;
    const jsparse::ParseResult &parse_;

    std::string Slice(const Node *n) const {
        if (!n || n->end <= n->start || n->end > text_.size()) return std::string();
        return OneLine(std::string_view(text_).substr(n->start, n->end - n->start));
    }

    int LineOf(uint32_t off) const {
        int line = 0;
        // Only for a declaration's first sighting: a linear count from the previous one.
        if (off < last_off_) {
            last_off_ = 0;
            last_line_ = 0;
        }
        for (uint32_t i = last_off_; i < off && i < text_.size(); i++) {
            if (text_[i] == '\n') last_line_++;
        }
        last_off_ = off;
        line = last_line_;
        return line;
    }
    mutable uint32_t last_off_ = 0;
    mutable int last_line_ = 0;

    // The `/** ... */` directly above (at most one line break between), as markdown.
    std::string DocBefore(uint32_t start) const {
        const auto &cs = parse_.comments;
        auto it = std::lower_bound(cs.begin(), cs.end(), start,
                                   [](const jsparse::Comment &c, uint32_t off) { return c.end <= off; });
        if (it == cs.begin()) return std::string();
        const jsparse::Comment &c = *(it - 1);
        int newlines = 0;
        for (uint32_t i = c.end; i < start && i < text_.size(); i++) {
            if (text_[i] == '\n') newlines++;
            if (!IsSpace(text_[i])) return std::string();
        }
        if (newlines > 1 || !c.block || c.value.empty() || c.value[0] != '*') return std::string();
        return Trim(jslsp::JsDocMarkdown(c.value.substr(1)));
    }

    static std::string NameOf(const Node *k) {
        if (!k) return std::string();
        if (k->type == NodeType::Identifier) return std::string(k->str);
        if (k->type == NodeType::Literal && k->lit == jsparse::LitKind::String) return std::string(k->str2);
        if (k->type == NodeType::Literal && k->lit == jsparse::LitKind::Number) return std::string(k->str);
        return std::string();
    }

    // A function, method or signature's `<T>(params): R`, `params` scanned from `from` (just after
    // the name) to its matching ')'.
    JsDtsSignature Signature(const Node *fn, uint32_t from, uint32_t doc_at) const {
        JsDtsSignature s;
        const Node *tp = fn->typeParameters();
        if (tp) {
            s.type_params = Slice(tp);
            from = std::max(from, tp->end);
        }
        size_t open = text_.find('(', from);
        if (open != std::string::npos && open < fn->end) {
            int depth = 0;
            size_t close = open;
            for (; close < fn->end && close < text_.size(); close++) {
                const char c = text_[close];
                if (c == '(' || c == '[' || c == '{') depth++;
                if (c == ')' || c == ']' || c == '}') {
                    if (--depth == 0) break;
                }
            }
            s.params = OneLine(std::string_view(text_).substr(open, close + 1 - open));
        } else {
            s.params = "()";
        }
        s.ret = AnnotationText(fn->returnType());
        s.doc = DocBefore(doc_at);
        return s;
    }

    // A `: T` annotation's type as written: from the source after the colon, not the type node's
    // range, which leaves out parentheses (`: ((a: number) => number)`).
    std::string AnnotationText(const Node *a) const {
        if (!a) return std::string();
        if (a->type != NodeType::TSTypeAnnotation) return Slice(a);
        std::string t = Slice(a);
        if (!t.empty() && t[0] == ':') t = Trim(t.substr(1));
        else if (a->typeAnnotation()) t = Slice(a->typeAnnotation());
        // A trailing `)` of the annotation's own parentheses past the type node's end.
        const Node *inner = a->typeAnnotation();
        if (inner) {
            int depth = 0;
            for (char c : t) depth += c == '(' ? 1 : c == ')' ? -1 : 0;
            uint32_t i = std::max(a->end, inner->end);
            while (depth > 0 && i < text_.size()) {
                const char c = text_[i++];
                if (c == ')') {
                    t += ')';
                    depth--;
                } else if (!IsSpace(c)) {
                    break;
                }
            }
        }
        return t;
    }

    static std::string TypeOf(const Node *annotated, const Reader &r) {
        return annotated ? r.AnnotationText(annotated->typeAnnotation()) : std::string();
    }

    JsDtsDecl &Get(JsDtsScope &scope, const std::string &name, JsDtsDecl::KindT kind, uint32_t start, bool *fresh) {
        auto it = scope.decls.find(name);
        *fresh = it == scope.decls.end();
        JsDtsDecl &d = scope.decls[name];
        if (*fresh) {
            d.name = name;
            d.kind = kind;
            d.scope = &scope;
            d.line = LineOf(start);
        }
        return d;
    }

    static void TakeDoc(JsDtsDecl &d, const std::string &doc) {
        if (d.doc.empty()) d.doc = doc;
    }

    static JsDtsMember &MemberSlot(std::vector<JsDtsMember> &ms, const std::string &name, bool is_static,
                                   JsDtsMember::KindT kind) {
        for (JsDtsMember &m : ms) {
            if (m.name == name && m.is_static == is_static && m.kind == kind) return m;
        }
        ms.emplace_back();
        ms.back().name = name;
        ms.back().is_static = is_static;
        ms.back().kind = kind;
        return ms.back();
    }

    void ClassBody(const Node *body, JsDtsDecl &d) {
        if (!body) return;
        for (const Node *m : body->body_list()) {
            if (!m || m->Has(jsparse::kFlagComputed)) continue;
            const std::string name = NameOf(m->key());
            const bool is_static = m->Has(jsparse::kFlagStatic);
            if (m->type == NodeType::MethodDefinition || m->type == NodeType::TSAbstractMethodDefinition) {
                const Node *fn = m->value();
                if (!fn) continue;
                JsDtsSignature sig = Signature(fn, m->key() ? m->key()->end : fn->start, m->start);
                if (m->kind == "constructor") {
                    sig.ret.clear();
                    if (m->accessibility == "private") continue;
                    d.sigs.push_back(sig);
                    continue;
                }
                if (name.empty()) continue;
                if (m->kind == "get" || m->kind == "set") {
                    JsDtsMember &p = MemberSlot(d.members, name, is_static, JsDtsMember::Property);
                    if (m->kind == "get") p.type = sig.ret;
                    if (p.doc.empty()) p.doc = sig.doc;
                    p.access = std::string(m->accessibility);
                    continue;
                }
                JsDtsMember &mm = MemberSlot(d.members, name, is_static, JsDtsMember::Method);
                mm.sigs.push_back(sig);
                mm.access = std::string(m->accessibility);
                mm.abstract_ = m->type == NodeType::TSAbstractMethodDefinition;
                mm.optional = m->HasTs(jsparse::kTsOptional);
                continue;
            }
            if (m->type == NodeType::PropertyDefinition || m->type == NodeType::TSAbstractPropertyDefinition ||
                m->type == NodeType::AccessorProperty) {
                if (name.empty()) continue;
                JsDtsMember &p = MemberSlot(d.members, name, is_static, JsDtsMember::Property);
                p.type = TypeOf(m, *this);
                p.readonly = m->HasTs(jsparse::kTsReadonly);
                p.optional = m->HasTs(jsparse::kTsOptional);
                p.access = std::string(m->accessibility);
                p.abstract_ = m->type == NodeType::TSAbstractPropertyDefinition;
                if (p.doc.empty()) p.doc = DocBefore(m->start);
            }
        }
    }

    void InterfaceBody(const Node *body, JsDtsDecl &d) {
        if (!body) return;
        for (const Node *m : body->body_list()) {
            if (!m) continue;
            if (m->type == NodeType::TSConstructSignatureDeclaration) {
                JsDtsSignature sig = Signature(m, m->start, m->start);
                sig.ret.clear();
                d.sigs.push_back(sig);
                continue;
            }
            if (m->Has(jsparse::kFlagComputed)) continue;
            const std::string name = NameOf(m->key());
            if (name.empty()) continue;
            if (m->type == NodeType::TSMethodSignature && m->kind == "method") {
                JsDtsMember &mm = MemberSlot(d.members, name, false, JsDtsMember::Method);
                mm.sigs.push_back(Signature(m, m->key()->end, m->start));
                mm.optional = m->HasTs(jsparse::kTsOptional);
            } else if (m->type == NodeType::TSMethodSignature) {  // get / set
                JsDtsMember &p = MemberSlot(d.members, name, false, JsDtsMember::Property);
                const JsDtsSignature sig = Signature(m, m->key()->end, m->start);
                if (m->kind == "get") p.type = sig.ret;
                if (p.doc.empty()) p.doc = sig.doc;
            } else if (m->type == NodeType::TSPropertySignature) {
                JsDtsMember &p = MemberSlot(d.members, name, false, JsDtsMember::Property);
                p.type = TypeOf(m, *this);
                p.readonly = m->HasTs(jsparse::kTsReadonly);
                p.optional = m->HasTs(jsparse::kTsOptional);
                if (p.doc.empty()) p.doc = DocBefore(m->start);
            }
        }
    }

    // Declares what `n` declares in `scope` (its JSDoc is above `anchor`, the statement holding it);
    // returns the names.
    std::vector<std::string> Declare(const Node *n, const Node *anchor, JsDtsScope &scope) {
        std::vector<std::string> names;
        bool fresh = false;
        switch (n->type) {
            case NodeType::TSDeclareFunction:
            case NodeType::FunctionDeclaration: {
                const std::string name = n->id() ? std::string(n->id()->str) : "default";
                JsDtsDecl &d = Get(scope, name, JsDtsDecl::Function, anchor->start, &fresh);
                if (d.kind == JsDtsDecl::Namespace) d.kind = JsDtsDecl::Function;
                d.sigs.push_back(Signature(n, n->id() ? n->id()->end : n->start, anchor->start));
                TakeDoc(d, d.sigs.back().doc);
                names.push_back(name);
                break;
            }
            case NodeType::ClassDeclaration: {
                const std::string name = n->id() ? std::string(n->id()->str) : "default";
                JsDtsDecl &d = Get(scope, name, JsDtsDecl::Class, anchor->start, &fresh);
                d.kind = JsDtsDecl::Class;  // over a merged interface or namespace
                d.abstract_ = n->HasTs(jsparse::kTsAbstract);
                if (n->typeParameters()) d.type_params = Slice(n->typeParameters());
                if (n->superClass()) d.bases.insert(d.bases.begin(), Slice(n->superClass()));
                TakeDoc(d, DocBefore(anchor->start));
                ClassBody(n->body(), d);
                names.push_back(name);
                break;
            }
            case NodeType::TSInterfaceDeclaration: {
                if (!n->id()) break;
                const std::string name(n->id()->str);
                JsDtsDecl &d = Get(scope, name, JsDtsDecl::Interface, anchor->start, &fresh);
                if (n->typeParameters() && d.type_params.empty()) d.type_params = Slice(n->typeParameters());
                for (const Node *h : n->extends_list()) {
                    if (h) d.bases.push_back(Slice(h));
                }
                TakeDoc(d, DocBefore(anchor->start));
                InterfaceBody(n->body(), d);
                names.push_back(name);
                break;
            }
            case NodeType::TSEnumDeclaration: {
                if (!n->id()) break;
                const std::string name(n->id()->str);
                JsDtsDecl &d = Get(scope, name, JsDtsDecl::Enum, anchor->start, &fresh);
                d.kind = JsDtsDecl::Enum;
                d.const_enum = n->HasTs(jsparse::kTsConst);
                TakeDoc(d, DocBefore(anchor->start));
                const Node *body = n->body();
                if (body) {
                    for (const Node *em : body->members()) {
                        if (!em) continue;
                        JsDtsMember &mm = MemberSlot(d.members, NameOf(em->id()), false, JsDtsMember::EnumMember);
                        mm.type = Slice(em->initializer());
                        mm.doc = DocBefore(em->start);
                    }
                }
                names.push_back(name);
                break;
            }
            case NodeType::VariableDeclaration:
                for (const Node *vd : n->declarations()) {
                    if (!vd || !vd->id() || vd->id()->type != NodeType::Identifier) continue;
                    const std::string name(vd->id()->str);
                    JsDtsDecl &d = Get(scope, name, JsDtsDecl::Variable, anchor->start, &fresh);
                    d.var_kind = std::string(n->kind);
                    d.type = TypeOf(vd->id(), *this);
                    TakeDoc(d, DocBefore(anchor->start));
                    names.push_back(name);
                }
                break;
            case NodeType::TSTypeAliasDeclaration: {
                if (!n->id()) break;
                const std::string name(n->id()->str);
                JsDtsDecl &d = Get(scope, name, JsDtsDecl::TypeAlias, anchor->start, &fresh);
                if (fresh) {
                    if (n->typeParameters()) d.type_params = Slice(n->typeParameters());
                    d.type = TypeOf(n, *this);
                }
                TakeDoc(d, DocBefore(anchor->start));
                names.push_back(name);
                break;
            }
            case NodeType::TSModuleDeclaration: {
                const Node *id = n->id();
                if (!id || n->HasTs(jsparse::kTsGlobal)) break;
                if (id->type == NodeType::Literal) {
                    // `declare module "name" { ... }`
                    auto &slot = m_.ambient[std::string(id->str2)];
                    if (!slot) {
                        slot = std::make_shared<JsDtsScope>();
                        slot->parent = &m_.root;
                    }
                    if (n->body()) Statements(n->body()->body_list(), *slot);
                    break;
                }
                if (id->type != NodeType::Identifier) break;  // `namespace a.b {}`: not followed
                const std::string name(id->str);
                JsDtsDecl &d = Get(scope, name, JsDtsDecl::Namespace, anchor->start, &fresh);
                if (!d.ns) {
                    d.ns = std::make_shared<JsDtsScope>();
                    d.ns->parent = &scope;
                }
                TakeDoc(d, DocBefore(anchor->start));
                if (n->body() && n->body()->type == NodeType::TSModuleBlock) Statements(n->body()->body_list(), *d.ns);
                names.push_back(name);
                break;
            }
            default:
                break;
        }
        return names;
    }

    void Statements(const jsparse::NodeList &body, JsDtsScope &scope) {
        for (const Node *st : body) {
            if (!st) continue;
            switch (st->type) {
                case NodeType::ExportNamedDeclaration:
                    scope.explicit_exports = true;
                    if (st->declaration()) {
                        for (const std::string &name : Declare(st->declaration(), st, scope)) scope.exports[name] = name;
                    } else if (!st->source() && !st->HasTs(jsparse::kTsTypeOnly)) {
                        for (const Node *sp : st->specifiers()) {
                            if (!sp) continue;
                            const std::string local = NameOf(sp->local());
                            const std::string exported = sp->exported() ? NameOf(sp->exported()) : local;
                            if (!local.empty() && !exported.empty()) scope.exports[exported] = local;
                        }
                    }
                    break;
                case NodeType::ExportDefaultDeclaration: {
                    scope.explicit_exports = true;
                    const Node *d = st->declaration();
                    if (!d) break;
                    if (d->type == NodeType::Identifier) {
                        scope.exports["default"] = std::string(d->str);
                    } else {
                        const std::vector<std::string> names = Declare(d, st, scope);
                        if (!names.empty()) scope.exports["default"] = names[0];
                    }
                    break;
                }
                case NodeType::TSExportAssignment:
                    scope.explicit_exports = true;
                    if (st->expression() && st->expression()->type == NodeType::Identifier)
                        scope.exports["export="] = std::string(st->expression()->str);
                    break;
                case NodeType::ExportAllDeclaration:
                    scope.explicit_exports = true;  // another file's names: not followed
                    break;
                default:
                    Declare(st, st, scope);
                    break;
            }
        }
    }
};

// ---- files -----------------------------------------------------------------------------------------------------

bool FileStamp(const std::string &path, long long *stamp) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return false;
    const auto size = fs::file_size(path, ec);
    if (ec) return false;
    *stamp = static_cast<long long>(t.time_since_epoch().count()) ^ (static_cast<long long>(size) << 1);
    return true;
}

bool IsFile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && !ec;
}

bool IsDir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec) && !ec;
}

bool ReadAll(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

bool ReadJson(const fs::path &p, Json *out) {
    std::string text;
    return ReadAll(p.string(), &text) && Json::Parse(text, out) && out->is_object();
}

std::string ExpandHome(const std::string &p) {
    if (p.size() >= 1 && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        const char *home = std::getenv("HOME");
        if (home) return std::string(home) + p.substr(1);
    }
    return p;
}

// A relative path to a declaration file, as package.json's `types` gives it ("index.d.ts",
// "./dist/index", "lib/main.js").
std::string DtsForFile(const fs::path &base);

// The declarations of a package directory (or any directory imported as one): package.json's
// `types` / `typings` / `exports["."].types`, else index.d.ts.
std::string DtsForDir(const fs::path &dir) {
    if (!IsDir(dir)) return std::string();
    Json pkg;
    if (ReadJson(dir / "package.json", &pkg)) {
        std::vector<std::string> cands;
        for (const char *key : {"types", "typings"}) {
            const Json &v = pkg.get(key);
            if (v.is_string() && !v.as_string().empty()) cands.push_back(v.as_string());
        }
        const Json &ex = pkg.get("exports");
        const Json &dot = ex.is_object() && ex.get(".").is_object() ? ex.get(".") : ex;
        if (dot.is_object()) {
            const Json &t = dot.get("types");
            if (t.is_string()) cands.push_back(t.as_string());
            for (const char *cond : {"require", "import", "node", "default"}) {
                const Json &c = dot.get(cond);
                if (c.is_object() && c.get("types").is_string()) cands.push_back(c.get("types").as_string());
            }
        }
        for (const std::string &c : cands) {
            const fs::path p = (dir / c).lexically_normal();
            if (IsFile(p) && EndsWith(p.string(), ".ts")) return p.string();
            const std::string f = DtsForFile(p);
            if (!f.empty()) return f;
            if (IsFile(p / "index.d.ts")) return (p / "index.d.ts").string();
        }
    }
    if (IsFile(dir / "index.d.ts")) return (dir / "index.d.ts").lexically_normal().string();
    return std::string();
}

// `x.js` -> `x.d.ts` (`.mjs` -> `.d.mts`, `.cjs` -> `.d.cts`, a native `.node` -> `.d.ts`);
// an extensionless or other `x` -> `x.d.ts`.
std::string DtsForFile(const fs::path &base) {
    const std::string s = base.string();
    struct Ext {
        const char *ext, *dts;
    };
    for (const Ext &e : {Ext{".js", ".d.ts"}, Ext{".jsx", ".d.ts"}, Ext{".mjs", ".d.mts"}, Ext{".cjs", ".d.cts"},
                         Ext{".node", ".d.ts"}, Ext{".ts", ".d.ts"}}) {
        if (!EndsWith(s, e.ext)) continue;
        const std::string stem = s.substr(0, s.size() - std::strlen(e.ext));
        if (EndsWith(stem, ".d")) return IsFile(s) ? s : std::string();  // already a declaration file
        if (IsFile(stem + e.dts)) return stem + e.dts;
        if (std::strcmp(e.dts, ".d.ts") != 0 && IsFile(stem + ".d.ts")) return stem + ".d.ts";
        return std::string();
    }
    for (const char *dts : {".d.ts", ".d.mts", ".d.cts"}) {
        if (IsFile(s + dts)) return s + dts;
    }
    return std::string();
}

std::string DtsForPath(const fs::path &base) {
    const std::string f = DtsForFile(base);
    return f.empty() ? DtsForDir(base) : f;
}

// `@scope/pkg/sub` -> {"@scope/pkg", "sub"}.
void SplitPackage(const std::string &spec, std::string *pkg, std::string *sub) {
    size_t slash = spec.find('/');
    if (!spec.empty() && spec[0] == '@' && slash != std::string::npos) slash = spec.find('/', slash + 1);
    *pkg = slash == std::string::npos ? spec : spec.substr(0, slash);
    *sub = slash == std::string::npos ? std::string() : spec.substr(slash + 1);
}

// A specifier with nothing that could leave the directory it is looked up in.
bool SafeSpecifier(const std::string &spec) {
    if (spec.empty() || spec[0] == '/' || spec[0] == '.' || spec.find('\\') != std::string::npos) return false;
    size_t i = 0;
    while (i <= spec.size()) {
        size_t j = spec.find('/', i);
        if (j == std::string::npos) j = spec.size();
        const std::string part = spec.substr(i, j - i);
        if (part.empty() || part == "." || part == "..") return false;
        i = j + 1;
    }
    return true;
}

std::string InPackage(const fs::path &pkg_dir, const std::string &sub) {
    if (sub.empty()) return DtsForDir(pkg_dir);
    return DtsForPath(pkg_dir / sub);
}

std::mutex g_mutex;

struct CachedDts {
    long long stamp = 0;
    std::shared_ptr<const JsDtsModule> module;
};
std::map<std::string, CachedDts> g_dts;

}  // namespace

// ---- JsDtsScope / JsDtsModule ----------------------------------------------------------------------------------

const JsDtsDecl *JsDtsScope::Find(const std::string &name) const {
    for (const JsDtsScope *s = this; s; s = s->parent) {
        auto it = s->decls.find(name);
        if (it != s->decls.end()) return &it->second;
    }
    return nullptr;
}

const JsDtsDecl *JsDtsScope::Export(const std::string &name) const {
    if (!explicit_exports) {
        auto it = decls.find(name);
        return it == decls.end() ? nullptr : &it->second;
    }
    auto it = exports.find(name);
    return it == exports.end() ? nullptr : Find(it->second);
}

std::vector<std::pair<std::string, const JsDtsDecl *>> JsDtsScope::Exports() const {
    std::vector<std::pair<std::string, const JsDtsDecl *>> out;
    if (!explicit_exports) {
        for (const auto &kv : decls) out.emplace_back(kv.first, &kv.second);
        return out;
    }
    for (const auto &kv : exports) {
        if (kv.first == "default" || kv.first == "export=") continue;
        if (const JsDtsDecl *d = Find(kv.second)) out.emplace_back(kv.first, d);
    }
    return out;
}

const JsDtsDecl *JsDtsScope::ExportEquals() const {
    auto it = exports.find("export=");
    return it == exports.end() ? nullptr : Find(it->second);
}

namespace {

const JsDtsDecl *ClassOfTypeIn(const JsDtsScope *in, const std::string &type, int depth) {
    if (!in || depth > 8) return nullptr;
    // The first alternative of a union (or part of an intersection) that is not null-ish.
    std::vector<std::string> alts;
    {
        int d = 0;
        std::string cur;
        for (char c : type) {
            if (c == '<' || c == '(' || c == '[' || c == '{') d++;
            if (c == '>' || c == ')' || c == ']' || c == '}') d--;
            if (d == 0 && (c == '|' || c == '&')) {
                alts.push_back(Trim(cur));
                cur.clear();
                continue;
            }
            cur += c;
        }
        alts.push_back(Trim(cur));
    }
    for (std::string a : alts) {
        if (a.empty() || a == "null" || a == "undefined" || a == "void") continue;
        if (a.rfind("readonly ", 0) == 0) a = Trim(a.substr(9));
        if (EndsWith(a, "[]")) return nullptr;
        const size_t lt = a.find('<');
        if (lt != std::string::npos) a = Trim(a.substr(0, lt));
        // ns.Name: the first part by scope, the rest through namespaces.
        const JsDtsDecl *d = nullptr;
        size_t i = 0;
        while (i <= a.size()) {
            size_t dot = a.find('.', i);
            if (dot == std::string::npos) dot = a.size();
            const std::string part = a.substr(i, dot - i);
            for (char c : part) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
                      static_cast<unsigned char>(c) >= 0x80))
                    return nullptr;
            }
            if (part.empty()) return nullptr;
            if (!d) {
                d = in->Find(part);
            } else {
                d = d->ns ? d->ns->Export(part) : nullptr;
            }
            if (!d) return nullptr;
            i = dot + 1;
        }
        if (!d) return nullptr;
        if (d->kind == JsDtsDecl::Class || d->kind == JsDtsDecl::Interface) return d;
        if (d->kind == JsDtsDecl::TypeAlias) return ClassOfTypeIn(d->scope, d->type, depth + 1);
        return nullptr;
    }
    return nullptr;
}

}  // namespace

const JsDtsDecl *JsDtsModule::ClassOfType(const JsDtsScope *in, const std::string &type) const {
    return ClassOfTypeIn(in ? in : &root, type, 0);
}

std::vector<std::pair<const JsDtsDecl *, const JsDtsMember *>> JsDtsModule::Members(const JsDtsDecl *cls,
                                                                                    bool statics) const {
    std::vector<std::pair<const JsDtsDecl *, const JsDtsMember *>> out;
    std::set<std::string> names;
    std::set<const JsDtsDecl *> seen;
    std::vector<const JsDtsDecl *> queue{cls};
    for (size_t q = 0; q < queue.size() && q < 64; q++) {
        const JsDtsDecl *c = queue[q];
        if (!c || !seen.insert(c).second) continue;
        for (const JsDtsMember &m : c->members) {
            if (m.kind == JsDtsMember::EnumMember || m.is_static != statics || m.access == "private") continue;
            if (names.insert(m.name).second) out.emplace_back(c, &m);
        }
        for (const std::string &b : c->bases) queue.push_back(ClassOfType(c->scope, b));
    }
    return out;
}

const JsDtsMember *JsDtsModule::Member(const JsDtsDecl *cls, const std::string &name, bool statics,
                                       const JsDtsDecl **owner) const {
    std::set<const JsDtsDecl *> seen;
    std::vector<const JsDtsDecl *> queue{cls};
    for (size_t q = 0; q < queue.size() && q < 64; q++) {
        const JsDtsDecl *c = queue[q];
        if (!c || !seen.insert(c).second) continue;
        for (const JsDtsMember &m : c->members) {
            if (m.kind == JsDtsMember::EnumMember || m.is_static != statics || m.name != name) continue;
            if (owner) *owner = c;
            return &m;
        }
        for (const std::string &b : c->bases) queue.push_back(ClassOfType(c->scope, b));
    }
    return nullptr;
}

std::shared_ptr<const JsDtsModule> JsDtsParse(const std::string &text, const std::string &path) {
    auto m = std::make_shared<JsDtsModule>();
    m->path = path;
    jsparse::Options o;
    o.typescript = true;
    o.dts = true;
    o.source_type = jsparse::SourceType::Module;
    const jsparse::ParseResult parse = jsparse::Parse(text, o);
    if (parse.program) {
        Reader r(*m, text, parse);
        r.Run();
    }
    return m;
}

std::vector<std::string> JsDtsTypeDirs() {
    std::vector<std::string> out;
    const char *env = std::getenv("MEP_JS_TYPES");
    if (!env) return out;
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
    return out;
}

std::string JsDtsResolve(const std::string &doc_path, const std::string &specifier) {
    std::string spec = specifier;
    if (spec.empty()) return std::string();
    if (spec.rfind("node:", 0) == 0) return std::string();  // node's own modules: not ours to document
    const bool relative = spec == "." || spec == ".." || spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0;
    const bool absolute = fs::path(spec).is_absolute();
    // $MEP_JS_TYPES first, for bare specifiers.
    if (!relative && !absolute && SafeSpecifier(spec)) {
        for (const std::string &d : JsDtsTypeDirs()) {
            const fs::path base = fs::path(d) / spec;
            if (IsFile(base.string() + ".d.ts")) return base.string() + ".d.ts";
            if (IsFile(base / "index.d.ts")) return (base / "index.d.ts").string();
        }
    }
    if (absolute) return DtsForPath(fs::path(spec).lexically_normal());
    if (doc_path.empty()) return std::string();
    const fs::path dir = fs::path(doc_path).parent_path();
    if (relative) return DtsForPath((dir / spec).lexically_normal());
    if (!SafeSpecifier(spec)) return std::string();
    std::string pkg, sub;
    SplitPackage(spec, &pkg, &sub);
    // A package importing itself by name: the nearest package.json's `name`.
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        Json pj;
        if (ReadJson(d / "package.json", &pj)) {
            if (pj.get("name").is_string() && pj.get("name").as_string() == pkg) {
                const std::string f = InPackage(d, sub);
                if (!f.empty()) return f;
            }
            break;
        }
        if (d.parent_path() == d) break;
    }
    // node_modules, then @types, in each directory up.
    std::string types_name = pkg;
    if (!types_name.empty() && types_name[0] == '@') {
        types_name = types_name.substr(1);
        const size_t slash = types_name.find('/');
        if (slash != std::string::npos) types_name.replace(slash, 1, "__");
    }
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        const fs::path nm = d / "node_modules";
        if (IsDir(nm)) {
            std::string f = InPackage(nm / pkg, sub);
            if (f.empty()) f = InPackage(nm / "@types" / types_name, sub);
            if (!f.empty()) return f;
        }
        if (d.parent_path() == d) break;
    }
    return std::string();
}

std::shared_ptr<const JsDtsModule> JsDtsLoad(const std::string &path) {
    long long stamp = 0;
    if (path.empty() || !FileStamp(path, &stamp)) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_dts.find(path);
        if (it != g_dts.end() && it->second.stamp == stamp) return it->second.module;
    }
    std::string text;
    if (!ReadAll(path, &text)) return nullptr;
    std::shared_ptr<const JsDtsModule> mod = JsDtsParse(text, path);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dts[path] = {stamp, mod};
    return mod;
}

// ---- rendering -------------------------------------------------------------------------------------------------

std::string JsDtsSignatureLabel(const JsDtsSignature &s, const std::string &name) {
    return name + s.type_params + s.params + (s.ret.empty() ? "" : ": " + s.ret);
}

namespace {

std::string Footer(const JsDtsModule &m) {
    const fs::path p(m.path);
    const std::string dir = p.parent_path().filename().string();
    return "\n\n*declared in `" + (dir.empty() ? "" : dir + "/") + p.filename().string() + "`*";
}

// The distinct JSDoc of a run of overloads, in order.
std::string SigDocs(const std::vector<JsDtsSignature> &sigs) {
    std::vector<std::string> docs;
    for (const JsDtsSignature &s : sigs) {
        if (!s.doc.empty() && std::find(docs.begin(), docs.end(), s.doc) == docs.end()) docs.push_back(s.doc);
    }
    std::string out;
    for (const std::string &d : docs) out += (out.empty() ? "" : "\n\n") + d;
    return out;
}

std::string MemberList(const JsDtsModule &m, const JsDtsDecl &d) {
    std::string methods, props, statics;
    int n = 0;
    auto add = [&](std::string &dst, const std::string &name, const JsDtsDecl *owner) {
        if (++n > 60) return;
        dst += (dst.empty() ? "" : ", ") + std::string("`") + name + "`";
        if (owner != &d) dst += " *(" + owner->name + ")*";
    };
    for (const auto &om : m.Members(&d, false))
        add(om.second->kind == JsDtsMember::Method ? methods : props, om.second->name, om.first);
    if (d.kind == JsDtsDecl::Class) {
        for (const auto &om : m.Members(&d, true)) add(statics, om.second->name, om.first);
    }
    std::string out;
    if (!methods.empty()) out += "\n\n**Methods:** " + methods;
    if (!props.empty()) out += "\n\n**Properties:** " + props;
    if (!statics.empty()) out += "\n\n**Static:** " + statics;
    return out;
}

}  // namespace

std::string JsDtsDeclHover(const JsDtsModule &m, const JsDtsDecl &d) {
    std::string out = "```ts\n";
    std::string docs = d.doc;
    switch (d.kind) {
        case JsDtsDecl::Function:
            for (const JsDtsSignature &s : d.sigs) out += "function " + JsDtsSignatureLabel(s, d.name) + "\n";
            docs = SigDocs(d.sigs);
            break;
        case JsDtsDecl::Class: {
            out += std::string(d.abstract_ ? "abstract class " : "class ") + d.name + d.type_params;
            if (!d.bases.empty()) out += " extends " + d.bases[0];
            out += "\n";
            if (!d.abstract_) {
                for (const JsDtsSignature &s : d.sigs) out += "new " + JsDtsSignatureLabel(s, d.name) + "\n";
            }
            const std::string ctor_docs = SigDocs(d.sigs);
            if (!ctor_docs.empty()) docs += (docs.empty() ? "" : "\n\n") + ctor_docs;
            break;
        }
        case JsDtsDecl::Interface:
            out += "interface " + d.name + d.type_params;
            for (size_t i = 0; i < d.bases.size(); i++) out += (i ? ", " : " extends ") + d.bases[i];
            out += "\n";
            break;
        case JsDtsDecl::Enum:
            out += std::string(d.const_enum ? "const enum " : "enum ") + d.name + " {\n";
            for (const JsDtsMember &mm : d.members)
                out += "    " + mm.name + (mm.type.empty() ? "" : " = " + mm.type) + ",\n";
            out += "}\n";
            break;
        case JsDtsDecl::Variable:
            out += (d.var_kind.empty() ? std::string("var") : d.var_kind) + " " + d.name +
                   (d.type.empty() ? "" : ": " + d.type) + "\n";
            break;
        case JsDtsDecl::Namespace:
            out += "namespace " + d.name + "\n";
            break;
        case JsDtsDecl::TypeAlias:
            out += "type " + d.name + d.type_params + " = " + d.type + "\n";
            break;
    }
    out += "```";
    if (!docs.empty()) out += "\n\n" + docs;
    if (d.kind == JsDtsDecl::Class || d.kind == JsDtsDecl::Interface) out += MemberList(m, d);
    if (d.ns) {
        std::string names;
        int n = 0;
        for (const auto &kv : d.ns->Exports()) {
            if (++n > 40) break;
            names += (names.empty() ? "" : ", ") + std::string("`") + kv.first + "`";
        }
        if (!names.empty()) out += "\n\n**Namespace:** " + names;
    }
    return out + Footer(m);
}

std::string JsDtsMemberHover(const JsDtsModule &m, const JsDtsDecl &owner, const JsDtsMember &mem) {
    std::string out = "```ts\n";
    std::string docs = mem.doc;
    const std::string mods = std::string(mem.access.empty() ? "" : mem.access + " ") + (mem.is_static ? "static " : "");
    switch (mem.kind) {
        case JsDtsMember::Method:
            for (const JsDtsSignature &s : mem.sigs)
                out += "(method) " + mods + JsDtsSignatureLabel(s, owner.name + "." + mem.name) + "\n";
            docs = SigDocs(mem.sigs);
            break;
        case JsDtsMember::Property:
            out += "(property) " + mods + (mem.readonly ? "readonly " : "") + owner.name + "." + mem.name +
                   (mem.optional ? "?" : "") + (mem.type.empty() ? "" : ": " + mem.type) + "\n";
            break;
        case JsDtsMember::EnumMember:
            out += "(enum member) " + owner.name + "." + mem.name + (mem.type.empty() ? "" : " = " + mem.type) + "\n";
            if (docs.empty()) docs = owner.doc;
            break;
    }
    out += "```";
    if (!docs.empty()) out += "\n\n" + docs;
    return out + Footer(m);
}

std::string JsDtsModuleNote(const JsDtsModule &m, const JsDtsScope &scope) {
    std::string names;
    int n = 0;
    const auto exports = scope.Exports();
    for (const auto &kv : exports) {
        if (kv.second->kind == JsDtsDecl::Interface || kv.second->kind == JsDtsDecl::TypeAlias) continue;
        if (++n > 24) {
            names += ", ...";
            break;
        }
        names += (names.empty() ? "" : ", ") + std::string("`") + kv.first + "`";
    }
    std::string out = "**Exports:** " + (names.empty() ? std::string("*none*") : names);
    return out + Footer(m);
}
