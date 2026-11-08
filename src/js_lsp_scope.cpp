// mep-js-lsp's scope analysis: see js_lsp_internal.h for the model. Also
// the document's position helpers, the eslint comment directives, and the
// vocabulary lookups (with the little type inference hover and completion
// use for `obj.` members).
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "js_lsp_internal.h"

using jsparse::Node;
using jsparse::NodeType;
using namespace jslsp;

// ---- positions ----------------------------------------------------------------------------------------------

void JsLspDocument::Pos(uint32_t off, int *line, int *col) const {
    const int o = static_cast<int>(std::min<size_t>(off, text.size()));
    auto it = std::upper_bound(line_starts.begin(), line_starts.end(), o);
    const int l = static_cast<int>(it - line_starts.begin()) - 1;
    *line = l;
    *col = o - line_starts[static_cast<size_t>(l)];
}

uint32_t JsLspDocument::Off(int line, int col) const {
    if (line < 0) return 0;
    if (static_cast<size_t>(line) >= line_starts.size()) return static_cast<uint32_t>(text.size());
    const int ls = line_starts[static_cast<size_t>(line)];
    const int len = static_cast<int>(lines[static_cast<size_t>(line)].size());
    return static_cast<uint32_t>(ls + std::max(0, std::min(col, len)));
}

JsLspRange JsLspDocument::Range(uint32_t a, uint32_t b) const {
    JsLspRange r;
    Pos(a, &r.line, &r.col);
    Pos(std::max(a, b), &r.end_line, &r.end_col);
    return r;
}

int JsLspDocument::LineOf(uint32_t off) const {
    int l = 0, c = 0;
    Pos(off, &l, &c);
    return l;
}

std::string JsLspDocument::Slice(uint32_t a, uint32_t b) const {
    b = std::min<uint32_t>(b, static_cast<uint32_t>(text.size()));
    return b > a ? text.substr(a, b - a) : std::string();
}

Scope *JsLspDocument::ScopeAt(uint32_t off) const {
    Scope *s = Global();
    if (!s) return nullptr;
    for (;;) {
        Scope *next = nullptr;
        for (Scope *k : s->kids) {
            // A scope that ends in `}` does not contain the position just after it.
            const bool brace = k->end > 0 && k->end <= text.size() && text[k->end - 1] == '}';
            if (k->start <= off && (brace ? off < k->end : off <= k->end)) next = k;
        }
        if (!next) return s;
        s = next;
    }
}

const Hit *JsLspDocument::HitAt(uint32_t off) const {
    const Hit *best = nullptr;
    auto it = std::upper_bound(hits.begin(), hits.end(), off, [](uint32_t o, const Hit &h) { return o < h.start; });
    while (it != hits.begin()) {
        --it;
        if (it->start + 400 < off) break;
        if (off <= it->end && (!best || it->start > best->start)) best = &*it;
    }
    return best;
}

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string Trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// A JSDoc block's text (between `/**` and `*/`) as markdown.
std::string JsDocMarkdown(std::string_view body) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        if (nl == std::string_view::npos) nl = body.size();
        std::string l(body.substr(i, nl - i));
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t k = 0;
        while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
        if (k < l.size() && l[k] == '*') {
            k++;
            if (k < l.size() && l[k] == ' ') k++;
        } else {
            k = std::min(k, l.size());
        }
        lines.push_back(l.substr(k));
        i = nl + 1;
    }
    while (!lines.empty() && Trim(lines.front()).empty()) lines.erase(lines.begin());
    while (!lines.empty() && Trim(lines.back()).empty()) lines.pop_back();
    std::string out;
    bool in_tags = false;
    for (const std::string &raw : lines) {
        std::string l = raw;
        const std::string t = Trim(l);
        if (!t.empty() && t[0] == '@') {
            // @tag {Type} name rest
            size_t p = 1;
            while (p < t.size() && !IsSpace(t[p])) p++;
            std::string tag = t.substr(1, p - 1);
            std::string rest = Trim(t.substr(p));
            std::string type;
            if (!rest.empty() && rest[0] == '{') {
                int depth = 0;
                size_t q = 0;
                for (; q < rest.size(); q++) {
                    if (rest[q] == '{') depth++;
                    if (rest[q] == '}' && --depth == 0) break;
                }
                type = rest.substr(1, q > 0 ? q - 1 : 0);
                rest = q < rest.size() ? Trim(rest.substr(q + 1)) : std::string();
            }
            std::string line = "*@" + tag + "*";
            if (tag == "param" || tag == "arg" || tag == "argument" || tag == "property" || tag == "prop") {
                size_t q = 0;
                while (q < rest.size() && !IsSpace(rest[q])) q++;
                std::string name = rest.substr(0, q);
                std::string desc = Trim(rest.substr(q));
                if (desc.size() > 1 && desc[0] == '-' && desc[1] == ' ') desc = desc.substr(2);
                if (!name.empty()) line += " `" + name + "`";
                if (!type.empty()) line += " `" + type + "`";
                if (!desc.empty()) line += " — " + desc;
            } else {
                if (!type.empty()) line += " `" + type + "`";
                if (!rest.empty()) line += " " + rest;
            }
            out += (out.empty() ? "" : "\n\n") + line;
            in_tags = true;
            continue;
        }
        if (in_tags && !t.empty()) {
            out += " " + t;
            continue;
        }
        if (!out.empty()) out += "\n";
        out += l;
    }
    return out;
}

bool IsDirectiveComment(std::string_view v) {
    size_t i = 0;
    while (i < v.size() && (IsSpace(v[i]) || v[i] == '*')) i++;
    v = v.substr(i);
    for (const char *p : {"eslint", "global ", "globals ", "exported ", "jshint", "istanbul", "@ts-", "prettier-ignore",
                          "c8 ", "jscs:", "webpackChunkName", "#__PURE__", "@__PURE__"}) {
        if (v.substr(0, std::strlen(p)) == p) return true;
    }
    return false;
}

}  // namespace

std::string JsLspDocument::DocCommentFor(const Node *n) const {
    // Climb to the statement (or member) the documentation is written above.
    const Node *a = n;
    for (;;) {
        const Node *p = Parent(a);
        if (!p) break;
        if (p->type == NodeType::VariableDeclarator || p->type == NodeType::ExportNamedDeclaration ||
            p->type == NodeType::ExportDefaultDeclaration) {
            a = p;
            continue;
        }
        if (p->type == NodeType::VariableDeclaration) {
            if (p->declarations().size() == 1 || p->declarations()[0] == a) a = p;
            continue;
        }
        if ((p->type == NodeType::Property || p->type == NodeType::MethodDefinition ||
             p->type == NodeType::PropertyDefinition) &&
            (p->value() == a || p->key() == a)) {
            a = p;
            break;
        }
        if (p->type == NodeType::AssignmentExpression && p->right() == a) {
            a = p;
            continue;
        }
        if (p->type == NodeType::ExpressionStatement) {
            a = p;
            break;
        }
        break;
    }
    const auto &cs = parse.comments;
    auto it = std::lower_bound(cs.begin(), cs.end(), a->start,
                               [](const jsparse::Comment &c, uint32_t off) { return c.end <= off; });
    if (it == cs.begin()) return std::string();
    size_t k = static_cast<size_t>(it - cs.begin()) - 1;
    auto gap_ok = [&](uint32_t from, uint32_t to) {
        int newlines = 0;
        for (uint32_t i = from; i < to && i < text.size(); i++) {
            if (text[i] == '\n') newlines++;
            if (!IsSpace(text[i])) return false;
        }
        return newlines <= 1;
    };
    const jsparse::Comment &c = cs[k];
    if (!gap_ok(c.end, a->start)) return std::string();
    if (IsDirectiveComment(c.value)) return std::string();
    if (c.block) {
        if (c.value.empty() || c.value[0] != '*') return std::string();
        return JsDocMarkdown(c.value.substr(1));
    }
    // A run of // comments, each alone on its line.
    std::vector<std::string> got;
    uint32_t next_start = a->start;
    for (size_t j = k + 1; j-- > 0;) {
        const jsparse::Comment &cj = cs[j];
        if (cj.block || !gap_ok(cj.end, next_start)) break;
        const int l = LineOf(cj.start);
        const std::string &line = lines[static_cast<size_t>(l)];
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos ||
            static_cast<uint32_t>(line_starts[static_cast<size_t>(l)]) + first != cj.start)
            break;
        if (IsDirectiveComment(cj.value)) break;
        std::string v(cj.value);
        if (!v.empty() && v[0] == ' ') v.erase(0, 1);
        got.push_back(v);
        next_start = cj.start;
    }
    std::reverse(got.begin(), got.end());
    std::string out;
    for (const std::string &s : got) out += (out.empty() ? "" : "\n") + s;
    return out;
}

// ---- names ----------------------------------------------------------------------------------------------------

namespace jslsp {

bool IsReservedWord(std::string_view s) {
    static const std::set<std::string_view> kWords = {
        "break",  "case",    "catch",     "class",     "const",     "continue", "debugger", "default", "delete",
        "do",     "else",    "enum",      "export",    "extends",   "false",    "finally",  "for",     "function",
        "if",     "import",  "in",        "instanceof", "new",      "null",     "return",   "super",   "switch",
        "this",   "throw",   "true",      "try",       "typeof",    "var",      "void",     "while",   "with",
        "yield",  "let",     "static",    "implements", "interface", "package", "private",  "protected", "public",
        "await",  "arguments", "eval"};
    return kWords.count(s) > 0;
}

bool IsIdentifierName(std::string_view s) {
    if (s.empty()) return false;
    auto start = [](unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; };
    if (!start(static_cast<unsigned char>(s[0]))) return false;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!start(c) && !std::isdigit(c)) return false;
    }
    return true;
}

std::string_view NameOf(const Node *id) { return id ? id->str : std::string_view(); }

int EditDistance(std::string_view a, std::string_view b, int cap) {
    const size_t n = a.size(), m = b.size();
    if ((n > m ? n - m : m - n) > static_cast<size_t>(cap)) return cap + 1;
    std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; i++) d[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; j++) d[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; i++) {
        int row_min = cap + 1;
        for (size_t j = 1; j <= m; j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            row_min = std::min(row_min, d[i][j]);
        }
        if (row_min > cap) return cap + 1;
    }
    return d[n][m];
}

bool SameNode(const Node *a, const Node *b, bool tokens) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return false;
    if (a->type == NodeType::Literal) {
        if (a->lit != b->lit) return false;
        if (a->lit == jsparse::LitKind::Number) return a->num == b->num || (a->num != a->num && b->num != b->num);
        if (a->lit == jsparse::LitKind::String || a->lit == jsparse::LitKind::BigInt) return a->str2 == b->str2;
        if (a->lit == jsparse::LitKind::RegExp) return tokens && a->str == b->str;  // two literals, two objects
        return a->str == b->str;
    }
    const uint32_t mask = jsparse::kFlagComputed | jsparse::kFlagOptional | jsparse::kFlagPrefix |
                          jsparse::kFlagStatic | jsparse::kFlagAsync | jsparse::kFlagGenerator;
    if ((a->flags & mask) != (b->flags & mask) || a->op != b->op || a->kind != b->kind) return false;
    if (a->type == NodeType::Identifier || a->type == NodeType::PrivateIdentifier ||
        a->type == NodeType::JSXIdentifier || a->type == NodeType::TemplateElement) {
        if (a->str != b->str) return false;
    }
    // Calls, `new`, functions and classes make a new value each time they run.
    if (!tokens && (a->type == NodeType::CallExpression || a->type == NodeType::NewExpression ||
        a->type == NodeType::FunctionExpression || a->type == NodeType::ArrowFunctionExpression ||
        a->type == NodeType::ClassExpression || a->type == NodeType::ObjectExpression ||
        a->type == NodeType::ArrayExpression || a->type == NodeType::UpdateExpression ||
        a->type == NodeType::AssignmentExpression || a->type == NodeType::AwaitExpression ||
        a->type == NodeType::YieldExpression || a->type == NodeType::TaggedTemplateExpression ||
        a->type == NodeType::ImportExpression))
        return false;
    for (int i = 0; i < 4; i++) {
        if (!SameNode(a->kid[i], b->kid[i], tokens)) return false;
    }
    for (int i = 0; i < 2; i++) {
        if (a->list[i].size() != b->list[i].size()) return false;
        for (uint32_t j = 0; j < a->list[i].size(); j++) {
            if (!SameNode(a->list[i][j], b->list[i][j], tokens)) return false;
        }
    }
    return true;
}

}  // namespace jslsp

// ---- the scope walk --------------------------------------------------------------------------------------------

namespace {

bool HasUseStrict(const Node *body_owner) {
    // Program or a function's BlockStatement body: the directive prologue.
    for (const Node *s : body_owner->body_list()) {
        if (!s || s->type != NodeType::ExpressionStatement || !s->Has(jsparse::kFlagHasDirective)) break;
        if (s->str == "use strict") return true;
    }
    return false;
}

// Whether the value of expression `e` is thrown away: it is a statement, a
// for-update, or a sequence item that is.
bool ResultUnused(const JsLspDocument &doc, const Node *e) {
    for (;;) {
        const Node *p = doc.Parent(e);
        if (!p) return false;
        if (p->type == NodeType::ExpressionStatement) return true;
        if (p->type == NodeType::ForStatement) return p->update() == e || p->init() == e;
        if (p->type == NodeType::SequenceExpression) {
            if (p->expressions().size() > 0 && p->expressions()[p->expressions().size() - 1] != e) return true;
            e = p;
            continue;
        }
        if (p->type == NodeType::ChainExpression || p->type == NodeType::ParenthesizedExpression) {
            e = p;
            continue;
        }
        return false;
    }
}

class Builder {
public:
    explicit Builder(JsLspDocument &doc) : doc_(doc) {}

    void Run() {
        const Node *prog = doc_.parse.program;
        // A .js file that parses as a module but has no import/export is as likely a CommonJS or
        // browser script (sloppy, top-level names global): only real module syntax makes it one.
        bool module = doc_.parse.source_type == jsparse::SourceType::Module;
        if (module && doc_.dialect != JsLspDialect::Module && prog) {
            module = false;
            for (const Node *s : prog->body_list()) {
                if (s && (s->type == NodeType::ImportDeclaration || s->type == NodeType::ExportNamedDeclaration ||
                          s->type == NodeType::ExportDefaultDeclaration || s->type == NodeType::ExportAllDeclaration ||
                          s->type == NodeType::TSExportAssignment ||
                          (s->type == NodeType::TSImportEqualsDeclaration && s->moduleReference() &&
                           s->moduleReference()->type == NodeType::TSExternalModuleReference)))
                    module = true;
            }
        }
        Scope *g = NewScope(module ? ScopeKind::Module : ScopeKind::Global, prog);
        g->start = 0;
        g->end = static_cast<uint32_t>(doc_.text.size());
        if (!prog) return;
        g->strict = g->kind == ScopeKind::Module || HasUseStrict(prog);
        if (doc_.dts) ambient_ = 1;
        for (const Node *s : prog->body_list()) Visit(s);
        Resolve();
    }

private:
    JsLspDocument &doc_;
    Scope *cur_ = nullptr;
    int ambient_ = 0;                  // inside `declare ...` / a .d.ts: nothing here is reported unused
    std::vector<Scope *> infer_scope_; // where a conditional type's `infer X` declares X

    struct AmbientIf {
        Builder &b;
        bool on;
        AmbientIf(Builder &bb, bool o) : b(bb), on(o) {
            if (on) b.ambient_++;
        }
        ~AmbientIf() {
            if (on) b.ambient_--;
        }
        AmbientIf(const AmbientIf &) = delete;
        AmbientIf &operator=(const AmbientIf &) = delete;
    };

    static uint8_t NsOf(VarKind k) {
        switch (k) {
            case VarKind::Class:
            case VarKind::ClassName:
            case VarKind::Import:
            case VarKind::Enum:
            case VarKind::Namespace:
                return kNsAny;
            case VarKind::Interface:
            case VarKind::TypeAlias:
            case VarKind::TypeParam:
                return kNsType;
            default:
                return kNsValue;
        }
    }
    static bool JsDeclKind(VarKind k) { return k == VarKind::Var || k == VarKind::Function || k == VarKind::Param; }

    Scope *NewScope(ScopeKind kind, const Node *node) {
        doc_.scopes.push_back(std::make_unique<Scope>());
        Scope *s = doc_.scopes.back().get();
        s->kind = kind;
        s->node = node;
        s->parent = cur_;
        if (node) {
            s->start = node->start;
            s->end = node->end;
        }
        if (cur_) {
            s->strict = cur_->strict;
            cur_->kids.push_back(s);
        }
        cur_ = s;
        return s;
    }
    void PopScope() { cur_ = cur_->parent; }

    Scope *FunctionScope(Scope *s) {
        while (s && !s->IsFunctionLike()) s = s->parent;
        return s;
    }

    // `signature`: a function overload signature (TSDeclareFunction), which the implementation
    // (or another signature) merges with instead of redeclaring.
    Var *Declare(Scope *s, const Node *id, VarKind kind, const Node *decl, const Node *init, bool signature = false) {
        std::string_view name = NameOf(id);
        const uint8_t ns = NsOf(kind);
        if (Var *v = s->Find(name)) {
            if (id) {
                v->ids.push_back(id);
                doc_.decl_var[id] = v;
                if (!doc_.typescript) {
                    if (JsDeclKind(kind)) v->redeclared = true;
                } else if (signature) {
                    v->overloaded = true;
                } else if (kind == VarKind::Function && v->signature_only) {
                    v->signature_only = false;  // the implementation after its overloads
                } else if (JsDeclKind(kind) && JsDeclKind(v->kind) && !v->signature_only) {
                    v->redeclared = true;
                }
                // Declaration merging: a type-only name that gains a value takes the value's kind
                // (and so does an import a local value merges with: it can only have been a type).
                if (((v->ns & kNsValue) == 0 && (ns & kNsValue) != 0) ||
                    (doc_.typescript && v->kind == VarKind::Import && kind != VarKind::Import &&
                     (ns & kNsValue) != 0)) {
                    v->kind = kind;
                    v->decl = decl;
                    v->init = init;
                    v->has_value = v->has_value || init != nullptr;
                }
                v->ns = static_cast<uint8_t>(v->ns | ns);
                v->ambient = v->ambient && ambient_ > 0;
            }
            return v;
        }
        doc_.vars.push_back(std::make_unique<Var>());
        Var *v = doc_.vars.back().get();
        v->name = name;
        v->kind = kind;
        v->scope = s;
        v->decl = decl;
        v->init = init;
        v->has_value = init != nullptr;
        v->ns = ns;
        v->ambient = ambient_ > 0;
        v->signature_only = signature;
        v->overloaded = signature;
        if (id) {
            v->ids.push_back(id);
            doc_.decl_var[id] = v;
        }
        s->vars.push_back(v);
        s->names[name] = v;
        return v;
    }

    Ref *AddRef(const Node *id, bool read, bool write, uint8_t ns = kNsValue) {
        doc_.refs.push_back(std::make_unique<Ref>());
        Ref *r = doc_.refs.back().get();
        r->id = id;
        r->from = cur_;
        r->read = read;
        r->write = write;
        r->ns = ns;
        doc_.id_ref[id] = r;
        return r;
    }

    void Member(const Node *key) {
        if (key && (key->type == NodeType::Identifier || key->type == NodeType::PrivateIdentifier)) {
            Hit h;
            h.start = key->start;
            h.end = key->end;
            h.node = key;
            h.member = true;
            doc_.hits.push_back(h);
        }
    }

    struct PatternInfo {
        VarKind kind;
        Scope *target;
        const Node *decl;
        int param_index = -1;
    };

    void DeclarePattern(const Node *p, const PatternInfo &info, bool in_pattern, bool rest_sibling, bool shorthand,
                        const Node *init) {
        if (!p) return;
        // TypeScript: `x: T`, `{a}: T`, `...rest: T[]`, and a parameter's decorators.
        for (const Node *d : p->decorators()) Visit(d);
        if (p->typeAnnotation() && p->type != NodeType::AssignmentPattern) VisitType(p->typeAnnotation());
        switch (p->type) {
            case NodeType::Identifier: {
                Var *v = Declare(info.target, p, info.kind, info.decl, init);
                if (v->ids.size() == 1) {
                    v->param_index = info.param_index;
                    v->in_pattern = in_pattern;
                    v->rest_sibling = rest_sibling;
                }
                if (shorthand) v->shorthand_pattern = true;
                break;
            }
            case NodeType::ObjectPattern: {
                const auto &props = p->properties();
                const bool has_rest = props.size() > 0 && props[props.size() - 1] &&
                                      props[props.size() - 1]->type == NodeType::RestElement;
                for (const Node *prop : props) {
                    if (!prop) continue;
                    if (prop->type == NodeType::RestElement) {
                        DeclarePattern(prop->argument(), info, true, false, false, nullptr);
                        continue;
                    }
                    if (prop->Has(jsparse::kFlagComputed)) {
                        Visit(prop->key());
                    } else if (!prop->Has(jsparse::kFlagShorthand)) {
                        Member(prop->key());
                    }
                    DeclarePattern(prop->value(), info, true, has_rest, prop->Has(jsparse::kFlagShorthand), nullptr);
                }
                break;
            }
            case NodeType::ArrayPattern:
                for (const Node *e : p->elements()) DeclarePattern(e, info, true, false, false, nullptr);
                break;
            case NodeType::AssignmentPattern:
                DeclarePattern(p->left(), info, in_pattern, rest_sibling, shorthand, init);
                Visit(p->right());
                break;
            case NodeType::RestElement:
                DeclarePattern(p->argument(), info, in_pattern, false, false, nullptr);
                break;
            default:
                Visit(p);
                break;
        }
    }

    void AssignTarget(const Node *p, bool read_too, bool shorthand = false) {
        if (!p) return;
        switch (p->type) {
            case NodeType::Identifier: {
                Ref *r = AddRef(p, read_too, true);
                r->shorthand = shorthand;
                break;
            }
            case NodeType::ObjectPattern:
                for (const Node *prop : p->properties()) {
                    if (!prop) continue;
                    if (prop->type == NodeType::RestElement) {
                        AssignTarget(prop->argument(), false);
                        continue;
                    }
                    if (prop->Has(jsparse::kFlagComputed)) {
                        Visit(prop->key());
                    } else if (!prop->Has(jsparse::kFlagShorthand)) {
                        Member(prop->key());
                    }
                    AssignTarget(prop->value(), false, prop->Has(jsparse::kFlagShorthand));
                }
                break;
            case NodeType::ArrayPattern:
                for (const Node *e : p->elements()) AssignTarget(e, false);
                break;
            case NodeType::AssignmentPattern:
                AssignTarget(p->left(), false, shorthand);
                Visit(p->right());
                break;
            case NodeType::RestElement:
                AssignTarget(p->argument(), false);
                break;
            case NodeType::ParenthesizedExpression:
            case NodeType::TSNonNullExpression:
                AssignTarget(p->expression(), read_too);
                break;
            case NodeType::TSAsExpression:
            case NodeType::TSSatisfiesExpression:
            case NodeType::TSTypeAssertion:
                AssignTarget(p->expression(), read_too);
                VisitType(p->typeAnnotation());
                break;
            default:
                Visit(p);
                break;
        }
    }

    static bool IsSignature(const Node *fn) {
        return fn->type == NodeType::TSDeclareFunction || fn->type == NodeType::TSEmptyBodyFunctionExpression;
    }

    // `<T extends C = D, U>`: the names go into the current scope, then their constraints and
    // defaults are visited (they may name each other).
    void DeclareTypeParams(const Node *decl) {
        if (!decl) return;
        for (const Node *tp : decl->params()) {
            if (tp && tp->name() && tp->name()->type == NodeType::Identifier)
                Declare(cur_, tp->name(), VarKind::TypeParam, tp, nullptr);
        }
        for (const Node *tp : decl->params()) {
            if (!tp) continue;
            VisitType(tp->constraint());
            VisitType(tp->defaultType());
        }
    }

    // A parameter of a signature (a function type, an overload, an interface method): no
    // variable, only its types.
    void ParamTypesOnly(const Node *p) {
        if (!p) return;
        for (const Node *d : p->decorators()) Visit(d);
        switch (p->type) {
            case NodeType::TSParameterProperty:
                ParamTypesOnly(p->parameter());
                return;
            case NodeType::AssignmentPattern:
                ParamTypesOnly(p->left());
                Visit(p->right());
                return;
            case NodeType::RestElement:
                VisitType(p->typeAnnotation());
                ParamTypesOnly(p->argument());
                return;
            case NodeType::ObjectPattern:
                VisitType(p->typeAnnotation());
                for (const Node *prop : p->properties()) {
                    if (!prop) continue;
                    if (prop->type == NodeType::RestElement) {
                        ParamTypesOnly(prop->argument());
                    } else {
                        if (prop->Has(jsparse::kFlagComputed)) Visit(prop->key());
                        ParamTypesOnly(prop->value());
                    }
                }
                return;
            case NodeType::ArrayPattern:
                VisitType(p->typeAnnotation());
                for (const Node *e : p->elements()) ParamTypesOnly(e);
                return;
            default:
                VisitType(p->typeAnnotation());
                return;
        }
    }

    void VisitFunction(const Node *fn) {
        const bool arrow = fn->type == NodeType::ArrowFunctionExpression;
        const bool signature = IsSignature(fn);
        AmbientIf amb(*this, fn->HasTs(jsparse::kTsDeclare));
        bool name_scope = false;
        if (fn->type == NodeType::FunctionExpression && fn->id()) {
            NewScope(ScopeKind::FunctionName, fn);
            Declare(cur_, fn->id(), VarKind::FunctionName, fn, fn);
            name_scope = true;
        }
        Scope *s = NewScope(ScopeKind::Function, fn);
        DeclareTypeParams(fn->typeParameters());
        if (signature) {
            for (const Node *p : fn->params()) ParamTypesOnly(p);
            VisitType(fn->returnType());
            PopScope();
            return;
        }
        const Node *body = fn->body();
        if (body && body->type == NodeType::BlockStatement && HasUseStrict(body)) s->strict = true;
        if (!arrow) {
            doc_.vars.push_back(std::make_unique<Var>());
            Var *a = doc_.vars.back().get();
            a->name = "arguments";
            a->kind = VarKind::Arguments;
            a->scope = s;
            a->decl = fn;
            s->vars.push_back(a);
            s->names["arguments"] = a;
        }
        const auto &params = fn->params();
        for (uint32_t i = 0; i < params.size(); i++) {
            const Node *p = params[i];
            if (!p) continue;
            // `function f(this: Window)`: a type for `this`, not a parameter.
            if (p->type == NodeType::Identifier && p->str == "this") {
                VisitType(p->typeAnnotation());
                continue;
            }
            PatternInfo info{VarKind::Param, s, fn, static_cast<int>(i)};
            if (p->type == NodeType::TSParameterProperty) {
                // `constructor(private x)`: a parameter that is also a member.
                for (const Node *d : p->decorators()) Visit(d);
                const Node *inner = p->parameter();
                const Node *id = inner && inner->type == NodeType::AssignmentPattern ? inner->left() : inner;
                DeclarePattern(inner, info, false, false, false, nullptr);
                if (id && id->type == NodeType::Identifier) {
                    auto it = doc_.decl_var.find(id);
                    if (it != doc_.decl_var.end()) it->second->param_property = true;
                }
                continue;
            }
            DeclarePattern(p, info, p->type != NodeType::Identifier, false, false, nullptr);
        }
        VisitType(fn->returnType());
        if (body && body->type == NodeType::BlockStatement) {
            for (const Node *st : body->body_list()) Visit(st);
        } else {
            Visit(body);
        }
        PopScope();
        if (name_scope) PopScope();
    }

    static bool IsClassMember(const Node *m) {
        switch (m->type) {
            case NodeType::MethodDefinition:
            case NodeType::PropertyDefinition:
            case NodeType::TSAbstractMethodDefinition:
            case NodeType::TSAbstractPropertyDefinition:
            case NodeType::AccessorProperty:
            case NodeType::TSAbstractAccessorProperty:
                return true;
            default:
                return false;
        }
    }

    void VisitClass(const Node *cls) {
        AmbientIf amb(*this, cls->HasTs(jsparse::kTsDeclare));
        for (const Node *d : cls->decorators()) Visit(d);
        Visit(cls->superClass());
        Scope *s = NewScope(ScopeKind::Class, cls);
        s->strict = true;
        if (cls->type == NodeType::ClassExpression && cls->id()) Declare(s, cls->id(), VarKind::ClassName, cls, cls);
        DeclareTypeParams(cls->typeParameters());
        VisitType(cls->typeArguments());  // `extends Base<T>`
        for (const Node *i : cls->implements()) VisitType(i);
        const Node *body = cls->body();
        if (body) {
            for (const Node *m : body->body_list()) {
                if (!m) continue;
                if (IsClassMember(m)) {
                    for (const Node *d : m->decorators()) Visit(d);
                    if (m->Has(jsparse::kFlagComputed))
                        Visit(m->key());
                    else
                        Member(m->key());
                    VisitType(m->typeAnnotation());
                    AmbientIf decl(*this, m->HasTs(jsparse::kTsDeclare));
                    Visit(m->value());
                } else {
                    Visit(m);
                }
            }
        }
        PopScope();
    }

    // ---- TypeScript types ----

    // The leftmost name of `A`, `A.B.C` (TSQualifiedName) or `a.b` (a heritage expression): a
    // reference; the rest are member names.
    void EntityRef(const Node *n, uint8_t ns, bool in_type) {
        while (n && (n->type == NodeType::TSQualifiedName || n->type == NodeType::MemberExpression)) {
            if (n->type == NodeType::MemberExpression && n->Has(jsparse::kFlagComputed)) {
                Visit(n->property());
            } else {
                Member(n->right());
            }
            n = n->left();
        }
        if (n && n->type == NodeType::Identifier && n->str != "this") {
            Ref *r = AddRef(n, true, false, ns);
            r->in_type = in_type;
        } else if (n && n->type != NodeType::Identifier) {
            Visit(n);
        }
    }

    void TypeMemberKey(const Node *m) {
        if (!m->key()) return;
        if (m->Has(jsparse::kFlagComputed))
            Visit(m->key());
        else
            Member(m->key());
    }

    void VisitType(const Node *n) {
        if (!n) return;
        switch (n->type) {
            case NodeType::Identifier:
            case NodeType::TSThisType:
            case NodeType::TSLiteralType:
                break;
            case NodeType::TSTypeReference:
                EntityRef(n->typeName(), kNsType, true);
                VisitType(n->typeArguments());
                break;
            case NodeType::TSQualifiedName:
                EntityRef(n, kNsType, true);
                break;
            case NodeType::TSInterfaceHeritage:
            case NodeType::TSClassImplements:
                EntityRef(n->expression(), kNsType, true);
                VisitType(n->typeArguments());
                break;
            case NodeType::TSTypeQuery: {
                // `typeof x` in a type: the value x.
                const Node *e = n->exprName();
                if (e && e->type == NodeType::TSImportType)
                    VisitType(e);
                else
                    EntityRef(e, kNsValue, true);
                VisitType(n->typeArguments());
                break;
            }
            case NodeType::TSImportType:
                VisitType(n->typeArguments());
                break;
            case NodeType::TSPropertySignature:
                TypeMemberKey(n);
                VisitType(n->typeAnnotation());
                break;
            case NodeType::TSMethodSignature:
            case NodeType::TSCallSignatureDeclaration:
            case NodeType::TSConstructSignatureDeclaration:
            case NodeType::TSFunctionType:
            case NodeType::TSConstructorType:
                if (n->type == NodeType::TSMethodSignature) TypeMemberKey(n);
                NewScope(ScopeKind::Type, n);
                DeclareTypeParams(n->typeParameters());
                for (const Node *p : n->params()) ParamTypesOnly(p);
                VisitType(n->returnType());
                PopScope();
                break;
            case NodeType::TSIndexSignature:
                for (const Node *p : n->parameters()) ParamTypesOnly(p);
                VisitType(n->typeAnnotation());
                break;
            case NodeType::TSMappedType: {
                NewScope(ScopeKind::Type, n);
                if (n->key() && n->key()->type == NodeType::Identifier) {
                    Var *v = Declare(cur_, n->key(), VarKind::TypeParam, n, nullptr);
                    v->no_unused = true;
                }
                VisitType(n->constraint());
                VisitType(n->nameType());
                VisitType(n->typeAnnotation());
                PopScope();
                break;
            }
            case NodeType::TSConditionalType:
                VisitType(n->checkType());
                infer_scope_.push_back(NewScope(ScopeKind::Type, n));
                VisitType(n->extendsType());
                infer_scope_.pop_back();
                VisitType(n->trueType());
                PopScope();
                VisitType(n->falseType());
                break;
            case NodeType::TSInferType:
                if (const Node *tp = n->typeParameter()) {
                    if (tp->name() && tp->name()->type == NodeType::Identifier)
                        Declare(infer_scope_.empty() ? cur_ : infer_scope_.back(), tp->name(), VarKind::TypeParam, tp,
                                nullptr);
                    VisitType(tp->constraint());
                }
                break;
            case NodeType::TSTypePredicate:
                VisitType(n->typeAnnotation());
                break;
            case NodeType::TSNamedTupleMember:
                VisitType(n->elementType());
                break;
            case NodeType::TSTypeParameterDeclaration:
                DeclareTypeParams(n);
                break;
            default: {
                if (!IsTypeNode(n->type)) {
                    Visit(n);
                    break;
                }
                // A conditional type nested in another's `extends` has its own infer scope; any
                // other type node passes the current one through.
                std::vector<Node *> kids;
                jsparse::CollectChildren(n, kids);
                for (const Node *k : kids) VisitType(k);
                break;
            }
        }
    }

    static bool IsTypeNode(NodeType t) {
        switch (t) {
            case NodeType::TSTypeAnnotation:
            case NodeType::TSTypeParameterDeclaration:
            case NodeType::TSTypeParameter:
            case NodeType::TSTypeParameterInstantiation:
            case NodeType::TSAnyKeyword:
            case NodeType::TSUnknownKeyword:
            case NodeType::TSNumberKeyword:
            case NodeType::TSObjectKeyword:
            case NodeType::TSBooleanKeyword:
            case NodeType::TSBigIntKeyword:
            case NodeType::TSStringKeyword:
            case NodeType::TSSymbolKeyword:
            case NodeType::TSVoidKeyword:
            case NodeType::TSUndefinedKeyword:
            case NodeType::TSNullKeyword:
            case NodeType::TSNeverKeyword:
            case NodeType::TSIntrinsicKeyword:
            case NodeType::TSThisType:
            case NodeType::TSTypeReference:
            case NodeType::TSQualifiedName:
            case NodeType::TSArrayType:
            case NodeType::TSTupleType:
            case NodeType::TSNamedTupleMember:
            case NodeType::TSOptionalType:
            case NodeType::TSRestType:
            case NodeType::TSUnionType:
            case NodeType::TSIntersectionType:
            case NodeType::TSFunctionType:
            case NodeType::TSConstructorType:
            case NodeType::TSTypeLiteral:
            case NodeType::TSPropertySignature:
            case NodeType::TSMethodSignature:
            case NodeType::TSIndexSignature:
            case NodeType::TSCallSignatureDeclaration:
            case NodeType::TSConstructSignatureDeclaration:
            case NodeType::TSTypeOperator:
            case NodeType::TSIndexedAccessType:
            case NodeType::TSConditionalType:
            case NodeType::TSInferType:
            case NodeType::TSMappedType:
            case NodeType::TSLiteralType:
            case NodeType::TSTemplateLiteralType:
            case NodeType::TSTypeQuery:
            case NodeType::TSImportType:
            case NodeType::TSTypePredicate:
            case NodeType::TSInterfaceBody:
            case NodeType::TSInterfaceHeritage:
            case NodeType::TSClassImplements:
                return true;
            default:
                return false;
        }
    }

    // `declare` on a statement (or the declaration an export wraps).
    static bool Declared(const Node *n) { return n && n->HasTs(jsparse::kTsDeclare); }

    void VisitModuleDecl(const Node *n) {
        const Node *id = n->id();
        // `declare module "x" {}` and `declare global {}` augment someone else's names: ambient.
        const bool external = !id || id->type == NodeType::Literal || n->HasTs(jsparse::kTsGlobal) || n->kind == "global";
        AmbientIf amb(*this, Declared(n) || external);
        if (external) {
            // nothing to declare
        } else if (id && id->type == NodeType::Identifier) {
            Declare(cur_, id, VarKind::Namespace, n, nullptr);
        } else if (id && id->type == NodeType::TSQualifiedName) {
            // `namespace A.B.C {}`: A is the variable, B and C its members.
            const Node *q = id;
            while (q && q->type == NodeType::TSQualifiedName) {
                Member(q->right());
                q = q->left();
            }
            if (q && q->type == NodeType::Identifier) Declare(cur_, q, VarKind::Namespace, n, nullptr);
        }
        const Node *body = n->body();
        if (!body) return;
        NewScope(ScopeKind::TsModule, n);
        cur_->strict = true;
        if (body->type == NodeType::TSModuleBlock) {
            for (const Node *st : body->body_list()) Visit(st);
        } else {
            Visit(body);
        }
        PopScope();
    }

    void MarkExported(const Node *p) {
        if (!p) return;
        switch (p->type) {
            case NodeType::Identifier: {
                auto it = doc_.decl_var.find(p);
                if (it != doc_.decl_var.end()) it->second->exported = true;
                break;
            }
            case NodeType::VariableDeclaration:
                for (const Node *d : p->declarations()) {
                    if (d) MarkExported(d->id());
                }
                break;
            case NodeType::VariableDeclarator:
                MarkExported(p->id());
                break;
            case NodeType::ObjectPattern:
                for (const Node *prop : p->properties()) {
                    if (prop) MarkExported(prop->type == NodeType::RestElement ? prop->argument() : prop->value());
                }
                break;
            case NodeType::ArrayPattern:
                for (const Node *e : p->elements()) MarkExported(e);
                break;
            case NodeType::AssignmentPattern:
                MarkExported(p->left());
                break;
            case NodeType::RestElement:
                MarkExported(p->argument());
                break;
            case NodeType::FunctionDeclaration:
            case NodeType::ClassDeclaration:
            case NodeType::TSDeclareFunction:
            case NodeType::TSInterfaceDeclaration:
            case NodeType::TSTypeAliasDeclaration:
            case NodeType::TSEnumDeclaration:
            case NodeType::TSImportEqualsDeclaration:
                MarkExported(p->id());
                break;
            case NodeType::TSModuleDeclaration: {
                const Node *id = p->id();
                while (id && id->type == NodeType::TSQualifiedName) id = id->left();
                MarkExported(id);
                break;
            }
            default:
                break;
        }
    }

    void JsxName(const Node *n) {
        if (!n) return;
        if (n->type == NodeType::JSXIdentifier) {
            const std::string_view name = n->str;
            if (name.empty() || name == "this") return;
            const unsigned char c0 = static_cast<unsigned char>(name[0]);
            if (std::islower(c0) || name.find('-') != std::string_view::npos) return;  // an intrinsic: <div>
            Ref *r = AddRef(n, true, false);
            r->jsx = true;
        } else if (n->type == NodeType::JSXMemberExpression) {
            const Node *root = n;
            while (root && root->type == NodeType::JSXMemberExpression) {
                Member(root->property());
                root = root->object();
            }
            if (root && root->type == NodeType::JSXIdentifier && root->str != "this") {
                Ref *r = AddRef(root, true, false);
                r->jsx = true;
            }
        }
    }

    void Generic(const Node *n) {
        std::vector<Node *> kids;
        jsparse::CollectChildren(n, kids);
        for (const Node *k : kids) Visit(k);
    }

    void Visit(const Node *n) {
        if (!n) return;
        if (IsTypeNode(n->type)) {
            VisitType(n);
            return;
        }
        switch (n->type) {
            case NodeType::Identifier:
                AddRef(n, true, false);
                break;
            // ---- TypeScript declarations ----
            case NodeType::TSInterfaceDeclaration: {
                AmbientIf amb(*this, Declared(n));
                if (n->id()) Declare(cur_, n->id(), VarKind::Interface, n, nullptr);
                NewScope(ScopeKind::Type, n);
                DeclareTypeParams(n->typeParameters());
                for (const Node *h : n->extends_list()) VisitType(h);
                VisitType(n->body());
                PopScope();
                break;
            }
            case NodeType::TSTypeAliasDeclaration: {
                AmbientIf amb(*this, Declared(n));
                if (n->id()) Declare(cur_, n->id(), VarKind::TypeAlias, n, nullptr);
                NewScope(ScopeKind::Type, n);
                DeclareTypeParams(n->typeParameters());
                VisitType(n->typeAnnotation());
                PopScope();
                break;
            }
            case NodeType::TSEnumDeclaration: {
                AmbientIf amb(*this, Declared(n));
                if (n->id()) Declare(cur_, n->id(), VarKind::Enum, n, n);
                // The members are in scope in each other's initializers: `B = A << 1`.
                NewScope(ScopeKind::Block, n);
                const Node *body = n->body();
                const jsparse::NodeList &members = body ? body->members() : n->members();
                for (const Node *m : members) {
                    if (m && m->id() && m->id()->type == NodeType::Identifier) {
                        Var *v = Declare(cur_, m->id(), VarKind::EnumMember, m, m->initializer());
                        v->no_unused = true;
                    }
                }
                for (const Node *m : members) {
                    if (m) Visit(m->initializer());
                }
                PopScope();
                break;
            }
            case NodeType::TSModuleDeclaration:
                VisitModuleDecl(n);
                break;
            case NodeType::TSDeclareFunction: {
                AmbientIf amb(*this, Declared(n));
                if (n->id()) Declare(cur_, n->id(), VarKind::Function, n, n, true);
                VisitFunction(n);
                break;
            }
            case NodeType::TSEmptyBodyFunctionExpression:
                VisitFunction(n);
                break;
            case NodeType::TSImportEqualsDeclaration: {
                // `import x = require("m")` / `import x = A.B`.
                if (n->id()) {
                    Var *v = Declare(cur_, n->id(), VarKind::Import, n, nullptr);
                    if (n->HasTs(jsparse::kTsTypeOnly)) v->type_only_import = true;
                }
                const Node *ref = n->moduleReference();
                if (ref && ref->type != NodeType::TSExternalModuleReference) EntityRef(ref, kNsAny, false);
                break;
            }
            case NodeType::TSExportAssignment: {
                // `export = x`
                const Node *e = n->expression();
                if (e && e->type == NodeType::Identifier) {
                    Ref *r = AddRef(e, true, false, kNsAny);
                    r->export_ref = true;
                } else {
                    Visit(e);
                }
                break;
            }
            case NodeType::TSNamespaceExportDeclaration:
                break;
            case NodeType::Literal:
            case NodeType::ThisExpression:
            case NodeType::Super:
            case NodeType::TemplateElement:
            case NodeType::PrivateIdentifier:
            case NodeType::JSXText:
            case NodeType::JSXEmptyExpression:
            case NodeType::EmptyStatement:
            case NodeType::DebuggerStatement:
            case NodeType::ExportAllDeclaration:
                break;
            case NodeType::MetaProperty:
                break;
            case NodeType::BreakStatement:
            case NodeType::ContinueStatement:
                if (n->label()) LabelHit(n->label());
                break;
            case NodeType::LabeledStatement:
                LabelHit(n->label());
                Visit(n->body());
                break;
            case NodeType::BlockStatement:
                NewScope(ScopeKind::Block, n);
                for (const Node *s : n->body_list()) Visit(s);
                PopScope();
                break;
            case NodeType::StaticBlock:
                NewScope(ScopeKind::Function, n);
                for (const Node *s : n->body_list()) Visit(s);
                PopScope();
                break;
            case NodeType::WithStatement: {
                Visit(n->object());
                doc_.uses_with_or_eval = true;
                Scope *s = NewScope(ScopeKind::With, n);
                s->dynamic = true;
                Visit(n->body());
                PopScope();
                break;
            }
            case NodeType::SwitchStatement:
                Visit(n->discriminant());
                NewScope(ScopeKind::Switch, n);
                for (const Node *c : n->cases()) {
                    if (!c) continue;
                    Visit(c->test());
                    for (const Node *s : c->consequent_list()) Visit(s);
                }
                PopScope();
                break;
            case NodeType::TryStatement:
                Visit(n->block());
                if (const Node *h = n->handler()) {
                    NewScope(ScopeKind::Catch, h);
                    if (h->param()) {
                        PatternInfo info{VarKind::CatchParam, cur_, h};
                        DeclarePattern(h->param(), info, h->param()->type != NodeType::Identifier, false, false,
                                       nullptr);
                    }
                    Visit(h->body());
                    PopScope();
                }
                Visit(n->finalizer());
                break;
            case NodeType::ForStatement: {
                const Node *init = n->init();
                const bool scoped = init && init->type == NodeType::VariableDeclaration && init->kind != "var";
                if (scoped) NewScope(ScopeKind::For, n);
                Visit(init);
                Visit(n->test());
                Visit(n->update());
                Visit(n->body());
                if (scoped) PopScope();
                break;
            }
            case NodeType::ForInStatement:
            case NodeType::ForOfStatement: {
                const Node *left = n->left();
                if (left && left->type == NodeType::VariableDeclaration) {
                    const bool scoped = left->kind != "var";
                    if (scoped) NewScope(ScopeKind::For, n);
                    const VarKind kind = KindOf(left->kind);
                    Scope *target = scoped ? cur_ : FunctionScope(cur_);
                    for (const Node *d : left->declarations()) {
                        if (!d) continue;
                        PatternInfo info{kind, target, d};
                        DeclarePattern(d->id(), info, false, false, false, nullptr);
                        MarkPatternInit(d->id());
                        Visit(d->init());
                    }
                    Visit(n->right());
                    Visit(n->body());
                    if (scoped) PopScope();
                } else {
                    AssignTarget(left, false);
                    Visit(n->right());
                    Visit(n->body());
                }
                break;
            }
            case NodeType::FunctionDeclaration:
                if (n->id()) Declare(cur_, n->id(), VarKind::Function, n, n);
                VisitFunction(n);
                break;
            case NodeType::FunctionExpression:
            case NodeType::ArrowFunctionExpression:
                VisitFunction(n);
                break;
            case NodeType::ClassDeclaration: {
                AmbientIf amb(*this, Declared(n));
                if (n->id()) Declare(cur_, n->id(), VarKind::Class, n, n);
                VisitClass(n);
                break;
            }
            case NodeType::ClassExpression:
                VisitClass(n);
                break;
            case NodeType::VariableDeclaration: {
                AmbientIf amb(*this, Declared(n));
                const VarKind kind = KindOf(n->kind);
                Scope *target = kind == VarKind::Var ? FunctionScope(cur_) : cur_;
                for (const Node *d : n->declarations()) {
                    if (!d) continue;
                    PatternInfo info{kind, target, d};
                    const Node *id = d->id();
                    DeclarePattern(id, info, false, false, false,
                                   id && id->type == NodeType::Identifier ? d->init() : nullptr);
                    if (d->init() && id && id->type != NodeType::Identifier) MarkPatternInit(id);
                    Visit(d->init());
                }
                break;
            }
            case NodeType::ImportDeclaration: {
                Scope *target = doc_.Global();
                for (const Node *sp : n->specifiers()) {
                    if (!sp || !sp->local()) continue;
                    if (sp->type == NodeType::ImportSpecifier && sp->imported() &&
                        sp->imported()->start != sp->local()->start)
                        Member(sp->imported());
                    Var *v = Declare(target, sp->local(), VarKind::Import, sp, nullptr);
                    if (n->HasTs(jsparse::kTsTypeOnly) || sp->HasTs(jsparse::kTsTypeOnly)) v->type_only_import = true;
                }
                break;
            }
            case NodeType::ExportNamedDeclaration:
                if (n->declaration()) {
                    Visit(n->declaration());
                    MarkExported(n->declaration());
                }
                if (!n->source()) {
                    for (const Node *sp : n->specifiers()) {
                        if (!sp || !sp->local()) continue;
                        if (sp->local()->type == NodeType::Identifier) {
                            Ref *r = AddRef(sp->local(), true, false, kNsAny);
                            r->export_ref = true;
                        }
                        if (sp->exported() && sp->exported()->start != sp->local()->start) Member(sp->exported());
                    }
                }
                break;
            case NodeType::ExportDefaultDeclaration: {
                const Node *d = n->declaration();
                if (d && d->type == NodeType::Identifier) {
                    // `export default Name`: whatever Name is, a value or (TypeScript) an interface.
                    Ref *r = AddRef(d, true, false, doc_.typescript ? kNsAny : kNsValue);
                    r->export_ref = doc_.typescript;
                    break;
                }
                Visit(d);
                if (d && (d->type == NodeType::FunctionDeclaration || d->type == NodeType::ClassDeclaration ||
                          d->type == NodeType::TSInterfaceDeclaration || d->type == NodeType::TSDeclareFunction))
                    MarkExported(d);
                break;
            }
            case NodeType::ObjectExpression:
                for (const Node *p : n->properties()) {
                    if (!p) continue;
                    if (p->type != NodeType::Property) {
                        Visit(p);
                        continue;
                    }
                    if (p->Has(jsparse::kFlagComputed)) {
                        Visit(p->key());
                    } else if (!p->Has(jsparse::kFlagShorthand)) {
                        Member(p->key());
                    }
                    if (p->Has(jsparse::kFlagShorthand) && p->value() && p->value()->type == NodeType::Identifier) {
                        Ref *r = AddRef(p->value(), true, false);
                        r->shorthand = true;
                    } else {
                        Visit(p->value());
                    }
                }
                break;
            case NodeType::MemberExpression:
                Visit(n->object());
                if (n->Has(jsparse::kFlagComputed))
                    Visit(n->property());
                else
                    Member(n->property());
                break;
            case NodeType::UnaryExpression:
                if (n->op == "typeof" && n->argument() && n->argument()->type == NodeType::Identifier) {
                    Ref *r = AddRef(n->argument(), true, false);
                    r->type_of = true;
                } else {
                    Visit(n->argument());
                }
                break;
            case NodeType::UpdateExpression:
                if (n->argument() && n->argument()->type == NodeType::Identifier) {
                    Ref *r = AddRef(n->argument(), true, true);
                    r->for_itself = ResultUnused(doc_, n);
                } else {
                    Visit(n->argument());
                }
                break;
            case NodeType::AssignmentExpression: {
                const Node *left = n->left();
                const bool compound = n->op != "=";
                if (left && left->type == NodeType::Identifier) {
                    Ref *r = AddRef(left, compound, true);
                    const bool unused = ResultUnused(doc_, n);
                    if (compound) r->for_itself = unused;
                    const size_t before = doc_.refs.size();
                    Visit(n->right());
                    // `x = x + 1;`: the read only feeds x itself.
                    if (!compound && unused) {
                        const Node *rhs = n->right();
                        if (rhs && (rhs->type == NodeType::BinaryExpression ||
                                    rhs->type == NodeType::LogicalExpression)) {
                            for (size_t i = before; i < doc_.refs.size(); i++) {
                                Ref *rr = doc_.refs[i].get();
                                if (rr->id->str == left->str && rr->from == cur_ &&
                                    (rr->id == rhs->left() || rr->id == rhs->right()))
                                    rr->for_itself = true;
                            }
                        }
                    }
                } else {
                    AssignTarget(left, compound);
                    Visit(n->right());
                }
                break;
            }
            case NodeType::CallExpression:
            case NodeType::NewExpression: {
                const Node *callee = n->callee();
                if (callee && callee->type == NodeType::Identifier) {
                    if (callee->str == "eval" && n->type == NodeType::CallExpression) {
                        doc_.uses_with_or_eval = true;
                        for (Scope *s = cur_; s; s = s->parent) s->dynamic = true;
                    }
                }
                Visit(callee);
                VisitType(n->typeArguments());
                for (const Node *a : n->arguments()) Visit(a);
                break;
            }
            case NodeType::JSXElement:
                doc_.has_jsx = true;
                if (const Node *o = n->opening()) {
                    JsxName(o->name());
                    VisitType(o->typeArguments());
                    for (const Node *a : o->attributes()) {
                        if (!a) continue;
                        if (a->type == NodeType::JSXAttribute)
                            Visit(a->value());
                        else
                            Visit(a);
                    }
                }
                for (const Node *c : n->children()) Visit(c);
                if (const Node *c = n->closing()) JsxName(c->name());
                break;
            case NodeType::JSXFragment:
                doc_.has_jsx = true;
                for (const Node *c : n->children()) Visit(c);
                break;
            default:
                Generic(n);
                break;
        }
    }

    void LabelHit(const Node *label) {
        if (!label) return;
        Hit h;
        h.start = label->start;
        h.end = label->end;
        h.node = label;
        h.label = true;
        doc_.hits.push_back(h);
    }

    // A destructuring declaration with an initializer: every name in it is written.
    void MarkPatternInit(const Node *p) {
        if (!p) return;
        if (p->type == NodeType::Identifier) {
            auto it = doc_.decl_var.find(p);
            if (it != doc_.decl_var.end()) it->second->has_value = true;
            return;
        }
        std::vector<Node *> kids;
        jsparse::CollectChildren(p, kids);
        for (const Node *k : kids) {
            const Node *pp = doc_.Parent(k);
            // Skip the keys of `{key: value}` and default values.
            if (pp && pp->type == NodeType::Property && pp->key() == k && !pp->Has(jsparse::kFlagShorthand)) continue;
            if (pp && pp->type == NodeType::AssignmentPattern && pp->right() == k) continue;
            MarkPatternInit(k);
        }
    }

    static VarKind KindOf(std::string_view k) {
        if (k == "var") return VarKind::Var;
        if (k == "let") return VarKind::Let;
        if (k == "const") return VarKind::Const;
        return VarKind::Using;
    }

    void Resolve() {
        for (auto &up : doc_.refs) {
            Ref *r = up.get();
            const std::string_view name = r->id->str;
            for (Scope *s = r->from; s; s = s->parent) {
                Var *v = s->Find(name);
                if (v && (v->ns & r->ns)) {
                    r->var = v;
                    v->refs.push_back(r);
                    break;
                }
            }
            if (!r->var) {
                doc_.through.push_back(r);
                if (r->write) doc_.implicit_globals.insert(name);
                // A node script: the global require/module/exports (not a local of that name).
                if (name == "require" || name == "module" || name == "exports") doc_.commonjs = true;
            }
        }
        for (auto &up : doc_.vars) {
            for (const Node *id : up->ids) {
                Hit h;
                h.start = id->start;
                h.end = id->end;
                h.node = id;
                h.var = up.get();
                doc_.hits.push_back(h);
            }
        }
        for (auto &up : doc_.refs) {
            Hit h;
            h.start = up->id->start;
            h.end = up->id->end;
            h.node = up->id;
            h.var = up->var;
            h.ref = up.get();
            doc_.hits.push_back(h);
        }
        // A shorthand `{a}` has its key and value over the same range: the reference/declaration wins.
        std::stable_sort(doc_.hits.begin(), doc_.hits.end(), [](const Hit &a, const Hit &b) {
            if (a.start != b.start) return a.start < b.start;
            return (a.var || a.ref) && !(b.var || b.ref);
        });
    }
};

void BuildParents(JsLspDocument &doc) {
    if (!doc.parse.program) return;
    std::vector<const Node *> stack{doc.parse.program};
    std::vector<Node *> kids;
    while (!stack.empty()) {
        const Node *n = stack.back();
        stack.pop_back();
        jsparse::CollectChildren(n, kids);
        for (const Node *k : kids) {
            doc.parent[k] = n;
            stack.push_back(k);
        }
    }
}

std::vector<std::string> SplitList(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string t = Trim(cur);
        const size_t colon = t.find(':');
        if (colon != std::string::npos) t = Trim(t.substr(0, colon));
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (char c : s) {
        if (c == ',' || c == '\n') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

void ReadDirectives(JsLspDocument &doc) {
    Directives &d = doc.directives;
    for (const jsparse::Comment &c : doc.parse.comments) {
        std::string v = Trim(std::string(c.value));
        if (c.block && !v.empty() && v[0] == '*') v = Trim(v.substr(1));
        // ` -- reason` ends the directive.
        const size_t dash = v.find(" --");
        std::string body = dash == std::string::npos ? v : v.substr(0, dash);
        auto word = [&](const char *w) {
            const size_t n = std::strlen(w);
            return body.size() >= n && body.compare(0, n, w) == 0 && (body.size() == n || IsSpace(body[n]));
        };
        auto rest_after = [&](const char *w) { return std::string_view(body).substr(std::strlen(w)); };
        auto rules = [&](const char *w) {
            std::vector<std::string> out;
            for (const std::string &r : SplitList(rest_after(w))) {
                std::string t = r;
                const size_t sp = t.find(' ');
                if (sp != std::string::npos) t = t.substr(0, sp);
                // A plugin-qualified rule can still name ours: `@typescript-eslint/no-unused-vars`.
                const size_t slash = t.rfind('/');
                if (slash != std::string::npos) t = t.substr(slash + 1);
                out.push_back(t);
            }
            return out;
        };
        if (word("eslint-disable-next-line")) {
            d.lines[doc.LineOf(c.end) + 1].push_back(rules("eslint-disable-next-line"));
        } else if (word("eslint-disable-line")) {
            for (int l = doc.LineOf(c.start); l <= doc.LineOf(c.end); l++)
                d.lines[l].push_back(rules("eslint-disable-line"));
        } else if (word("eslint-disable")) {
            Directives::Region r;
            r.from = c.end;
            r.rules = rules("eslint-disable");
            d.regions.push_back(r);
        } else if (word("eslint-enable")) {
            const std::vector<std::string> rs = rules("eslint-enable");
            for (Directives::Region &r : d.regions) {
                if (r.to != UINT32_MAX) continue;
                if (rs.empty() || r.rules == rs) r.to = c.start;
            }
        } else if (c.block && (word("global") || word("globals"))) {
            for (const std::string &g : SplitList(rest_after(word("global") ? "global" : "globals"))) {
                // `/* global a b */` separates by spaces too.
                size_t i = 0;
                while (i < g.size()) {
                    while (i < g.size() && IsSpace(g[i])) i++;
                    size_t j = i;
                    while (j < g.size() && !IsSpace(g[j])) j++;
                    if (j > i) d.globals.insert(g.substr(i, j - i));
                    i = j;
                }
            }
        } else if (c.block && word("exported")) {
            for (const std::string &g : SplitList(rest_after("exported"))) {
                size_t i = 0;
                while (i < g.size()) {
                    while (i < g.size() && IsSpace(g[i])) i++;
                    size_t j = i;
                    while (j < g.size() && !IsSpace(g[j])) j++;
                    if (j > i) d.exported.insert(g.substr(i, j - i));
                    i = j;
                }
            }
        } else if (c.block && word("eslint-env")) {
            for (const std::string &e : SplitList(rest_after("eslint-env"))) d.envs.push_back(e);
        }
    }
}

}  // namespace

namespace jslsp {
void Analyze(JsLspDocument &doc) {
    BuildParents(doc);
    ReadDirectives(doc);
    Builder b(doc);
    b.Run();
}
}  // namespace jslsp

// ---- parsing ----------------------------------------------------------------------------------------------------

namespace {

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

jsparse::ParseResult ParseAutoJsx(std::string_view src, jsparse::Options o, bool jsx_first) {
    o.jsx = jsx_first;
    jsparse::ParseResult a = jsparse::ParseAuto(src, o);
    if (!a.has_error) return a;
    o.jsx = !jsx_first;
    jsparse::ParseResult b = jsparse::ParseAuto(src, o);
    if (!b.has_error) return b;
    // Both failed: the one that got further.
    return b.first_error_offset > a.first_error_offset ? std::move(b) : std::move(a);
}

}  // namespace

JsLspDialect JsLspDialectForPath(const std::string &path) {
    const std::string p = Lower(path);
    if (EndsWith(p, ".mjs")) return JsLspDialect::Module;
    if (EndsWith(p, ".cjs")) return JsLspDialect::Script;
    if (EndsWith(p, ".jsx")) return JsLspDialect::Jsx;
    if (EndsWith(p, ".tsx")) return JsLspDialect::Tsx;
    if (EndsWith(p, ".ts") || EndsWith(p, ".mts") || EndsWith(p, ".cts")) {
        // .d.ts, .d.mts, .d.cts, and TypeScript 5's arbitrary-extension declarations (`x.d.css.ts`).
        const size_t slash = p.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        if (base.find(".d.") != std::string::npos) return JsLspDialect::Dts;
        return JsLspDialect::Ts;
    }
    return JsLspDialect::Auto;
}

std::shared_ptr<const JsLspDocument> JsLspAnalyze(const std::vector<std::string> &lines, JsLspDialect dialect) {
    auto doc = std::make_shared<JsLspDocument>();
    doc->lines = lines;
    doc->dialect = dialect;
    if (doc->lines.empty()) doc->lines.emplace_back();
    for (size_t i = 0; i < doc->lines.size(); i++) {
        doc->line_starts.push_back(static_cast<int>(doc->text.size()) + (i ? 1 : 0));
        if (i) doc->text += '\n';
        doc->text += doc->lines[i];
    }
    jsparse::Options o;
    o.allow_hash_bang = 1;
    switch (dialect) {
        case JsLspDialect::Module:
            o.source_type = jsparse::SourceType::Module;
            o.jsx = false;
            doc->parse = jsparse::Parse(doc->text, o);
            if (doc->parse.has_error) {
                o.jsx = true;
                jsparse::ParseResult j = jsparse::Parse(doc->text, o);
                if (!j.has_error) doc->parse = std::move(j);
            }
            break;
        case JsLspDialect::Script:
            o.source_type = jsparse::SourceType::Script;
            doc->parse = jsparse::Parse(doc->text, o);
            if (doc->parse.has_error) {
                o.jsx = true;
                jsparse::ParseResult j = jsparse::Parse(doc->text, o);
                if (!j.has_error) doc->parse = std::move(j);
            }
            break;
        case JsLspDialect::Jsx:
            doc->parse = ParseAutoJsx(doc->text, o, true);
            break;
        case JsLspDialect::Ts:
        case JsLspDialect::Tsx:
        case JsLspDialect::Dts:
            // TypeScript: the source type does not matter to tsc's parser (import/export parse
            // anywhere); whether the file is a module is decided by what it contains (Builder::Run).
            doc->typescript = true;
            doc->dts = dialect == JsLspDialect::Dts;
            o.typescript = true;
            o.jsx = dialect == JsLspDialect::Tsx;
            o.dts = doc->dts;
            o.source_type = jsparse::SourceType::Module;
            doc->parse = jsparse::Parse(doc->text, o);
            break;
        case JsLspDialect::Auto:
            doc->parse = ParseAutoJsx(doc->text, o, false);
            break;
    }
    jslsp::Analyze(*doc);
    return doc;
}

// ---- vocabulary ------------------------------------------------------------------------------------------------

namespace {

struct ApiIndexData {
    std::unordered_map<std::string_view, const JsLspApi *> by_name;
    std::unordered_map<std::string_view, std::vector<const JsLspApi *>> members;
    std::vector<const JsLspApi *> globals;
};

const ApiIndexData &ApiIndex() {
    static const ApiIndexData data = [] {
        ApiIndexData d;
        size_t n = 0;
        const JsLspApi *t = JsLspApiTable(&n);
        for (size_t i = 0; i < n; i++) {
            const std::string_view name = t[i].name;
            d.by_name.emplace(name, &t[i]);
            const size_t dot = name.rfind('.');
            if (dot == std::string_view::npos) {
                d.globals.push_back(&t[i]);
            } else {
                d.members[name.substr(0, dot)].push_back(&t[i]);
            }
        }
        return d;
    }();
    return data;
}

}  // namespace

namespace jslsp {

const Api *FindApi(std::string_view name) {
    const auto &m = ApiIndex().by_name;
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second;
}

const std::vector<const Api *> &ApiMembers(std::string_view owner) {
    static const std::vector<const Api *> kNone;
    const auto &m = ApiIndex().members;
    auto it = m.find(owner);
    return it == m.end() ? kNone : it->second;
}

const std::vector<const Api *> &ApiGlobals() { return ApiIndex().globals; }

bool IsHostGlobal(std::string_view name) {
    static const std::set<std::string_view> kNames = {
        // the language (some are not in lib.d.ts as values)
        "undefined", "globalThis", "Infinity", "NaN", "arguments", "eval",
        // node
        "require", "module", "exports", "__dirname", "__filename", "process", "Buffer", "global", "setImmediate",
        "clearImmediate", "queueMicrotask", "structuredClone",
        // test runners: jest, vitest, mocha, jasmine, qunit, node:test, cypress, playwright, ava
        "describe", "it", "test", "expect", "jest", "vi", "beforeEach", "afterEach", "beforeAll", "afterAll",
        "before", "after", "context", "suite", "setup", "teardown", "suiteSetup", "suiteTeardown", "xit",
        "xdescribe", "xtest", "fit", "fdescribe", "pending", "fail", "spyOn", "jasmine", "assert", "sinon", "chai",
        "should", "cy", "Cypress", "QUnit", "browser", "page",
        // bundlers, AMD, workers, extensions, other runtimes
        "__DEV__", "__webpack_require__", "__webpack_public_path__", "__non_webpack_require__", "define",
        "requirejs", "importScripts", "chrome", "Deno", "Bun", "WScript", "ActiveXObject", "Packages", "java",
        // jQuery
        "$", "jQuery",
    };
    return kNames.count(name) > 0;
}

std::string TypeNameOf(std::string_view t) {
    // The first member of a union that is not null/undefined.
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : t) {
        if (c == '<' || c == '(' || c == '[' || c == '{') depth++;
        if (c == '>' || c == ')' || c == ']' || c == '}') depth--;
        if (depth == 0 && (c == '|' || c == '&')) {
            parts.push_back(Trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(Trim(cur));
    for (std::string p : parts) {
        if (p.empty() || p == "null" || p == "undefined" || p == "void") continue;
        if (p.size() > 2 && p.compare(p.size() - 2, 2, "[]") == 0) return "Array";
        if (p.rfind("readonly ", 0) == 0) p = p.substr(9);
        if (p[0] == '(' || p[0] == '{' || p[0] == '"' || p[0] == '\'' || p[0] == '`') return std::string();
        const size_t lt = p.find('<');
        if (lt != std::string::npos) p = p.substr(0, lt);
        if (p == "string") return "String";
        if (p == "number") return "Number";
        if (p == "boolean") return "Boolean";
        if (p == "bigint") return "BigInt";
        if (p == "symbol") return "Symbol";
        if (p == "any" || p == "unknown" || p == "object" || p == "never" || p == "this") return std::string();
        if (!IsIdentifierName(p)) return std::string();
        if (p.size() == 1) return std::string();  // a type parameter: T, U
        return p;
    }
    return std::string();
}

}  // namespace jslsp

namespace jslsp {

// The return type of a TypeScript signature's first overload.
std::string ReturnTypeOf(std::string_view sig) {
    const size_t nl = sig.find('\n');
    if (nl != std::string_view::npos) sig = sig.substr(0, nl);
    size_t i = 0;
    if (i < sig.size() && sig[i] == '<') {
        int d = 0;
        for (; i < sig.size(); i++) {
            if (sig[i] == '<') d++;
            if (sig[i] == '>' && (i == 0 || sig[i - 1] != '=') && --d == 0) {
                i++;
                break;
            }
        }
    }
    if (i >= sig.size() || sig[i] != '(') return std::string();
    int d = 0;
    for (; i < sig.size(); i++) {
        const char c = sig[i];
        if (c == '(' || c == '[' || c == '{') d++;
        if (c == ')' || c == ']' || c == '}') {
            if (--d == 0) break;
        }
    }
    if (i + 2 >= sig.size() || sig[i + 1] != ':') return std::string();
    return TypeNameOf(sig.substr(i + 2));
}

}  // namespace jslsp

namespace jslsp {

std::string InferType(const JsLspDocument &doc, const Node *e, int depth) {
    if (!e || depth > 8) return std::string();
    switch (e->type) {
        case NodeType::Literal:
            switch (e->lit) {
                case jsparse::LitKind::String:
                    return "String";
                case jsparse::LitKind::Number:
                    return "Number";
                case jsparse::LitKind::Bool:
                    return "Boolean";
                case jsparse::LitKind::RegExp:
                    return "RegExp";
                case jsparse::LitKind::BigInt:
                    return "BigInt";
                default:
                    return std::string();
            }
        case NodeType::TemplateLiteral:
            return "String";
        case NodeType::ArrayExpression:
            return "Array";
        case NodeType::ObjectExpression:
            return "Object";
        case NodeType::FunctionExpression:
        case NodeType::ArrowFunctionExpression:
            return "Function";
        case NodeType::ParenthesizedExpression:
        case NodeType::ChainExpression:
            return InferType(doc, e->expression(), depth + 1);
        case NodeType::AwaitExpression:
            return std::string();
        case NodeType::NewExpression: {
            const Node *c = e->callee();
            if (c && c->type == NodeType::Identifier) {
                auto it = doc.id_ref.find(c);
                const Var *v = it == doc.id_ref.end() ? nullptr : it->second->var;
                if (v && (v->kind == VarKind::Class || v->kind == VarKind::Import || v->kind == VarKind::Function))
                    return std::string(c->str);
                if (!v && FindApi(c->str)) return std::string(c->str);
            }
            return std::string();
        }
        case NodeType::Identifier: {
            auto it = doc.id_ref.find(e);
            const Var *v = it == doc.id_ref.end() ? nullptr : it->second->var;
            if (!v) {
                // A declaring identifier stands for its variable too.
                auto dit = doc.decl_var.find(e);
                if (dit != doc.decl_var.end()) v = dit->second;
            }
            if (v) {
                // TypeScript: the declared type, when it names one (`s: string`, `el: HTMLElement`).
                if (!v->ids.empty() && v->ids[0]->typeAnnotation()) {
                    const Node *a = v->ids[0]->typeAnnotation();
                    const Node *t = a->typeAnnotation();
                    if (t) {
                        const std::string n = TypeNameOf(doc.Slice(t->start, t->end));
                        if (!n.empty()) return n;
                    }
                }
                if (v->init && (v->kind == VarKind::Const || v->kind == VarKind::Let || v->kind == VarKind::Var))
                    return InferType(doc, v->init, depth + 1);
                if (v->kind == VarKind::Class && v->decl) return std::string();
                return std::string();
            }
            const Api *a = FindApi(e->str);
            if (a && (std::strcmp(a->kind, "variable") == 0 || std::strcmp(a->kind, "property") == 0))
                return TypeNameOf(a->signature);
            return std::string();
        }
        case NodeType::CallExpression: {
            const Node *c = e->callee();
            if (!c) return std::string();
            if (c->type == NodeType::Identifier) {
                if (doc.id_ref.count(c) && doc.id_ref.at(c)->var) return std::string();
                const Api *a = FindApi(c->str);
                return a ? ReturnTypeOf(a->signature) : std::string();
            }
            if (c->type == NodeType::MemberExpression && !c->Has(jsparse::kFlagComputed) && c->property()) {
                // Statics of a global (`Array.from`), else the receiver's prototype.
                const Node *o = c->object();
                if (o && o->type == NodeType::Identifier && !(doc.id_ref.count(o) && doc.id_ref.at(o)->var)) {
                    if (const Api *a = FindApi(std::string(o->str) + "." + std::string(c->property()->str)))
                        return ReturnTypeOf(a->signature);
                }
                const std::string t = InferType(doc, o, depth + 1);
                if (t.empty()) return std::string();
                if (const Api *a = FindApi(t + ".prototype." + std::string(c->property()->str)))
                    return ReturnTypeOf(a->signature);
            }
            return std::string();
        }
        case NodeType::MemberExpression: {
            if (e->Has(jsparse::kFlagComputed) || !e->property()) {
                return std::string();
            }
            const Node *o = e->object();
            const std::string prop(e->property()->str);
            if (o && o->type == NodeType::Identifier && !(doc.id_ref.count(o) && doc.id_ref.at(o)->var)) {
                if (const Api *a = FindApi(std::string(o->str) + "." + prop)) return TypeNameOf(a->signature);
            }
            const std::string t = InferType(doc, o, depth + 1);
            if (t.empty()) return std::string();
            if (const Api *a = FindApi(t + ".prototype." + prop)) {
                if (std::strcmp(a->kind, "property") == 0) return TypeNameOf(a->signature);
            }
            return std::string();
        }
        case NodeType::ConditionalExpression:
            return InferType(doc, e->consequent(), depth + 1);
        case NodeType::LogicalExpression:
            return InferType(doc, e->right(), depth + 1);
        case NodeType::BinaryExpression:
            if (e->op == "+") {
                const std::string l = InferType(doc, e->left(), depth + 1);
                const std::string r = InferType(doc, e->right(), depth + 1);
                if (l == "String" || r == "String") return "String";
                if (l == "Number" && r == "Number") return "Number";
                return std::string();
            }
            if (e->op == "==" || e->op == "===" || e->op == "!=" || e->op == "!==" || e->op == "<" ||
                e->op == ">" || e->op == "<=" || e->op == ">=" || e->op == "in" || e->op == "instanceof")
                return "Boolean";
            return "Number";
        default:
            return std::string();
    }
}

}  // namespace jslsp
