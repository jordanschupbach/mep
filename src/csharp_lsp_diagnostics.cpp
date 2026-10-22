// The C# language server's diagnostics: Roslyn's own syntax errors (from
// mep's port of its parser), then lints a single file can show without a
// compiler -- each one Roslyn's own warning where Roslyn has one (same code,
// same wording), calibrated so that code which compiles cleanly gets next to
// nothing. See CsLspDiagnostics in csharp_lsp.h.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <set>

#include "csharp_lsp_analysis.h"

using namespace cslsp;
using csharp::DiagnosticSeverity;
using csharp::ErrorCode;
using csharp::GreenNode;
using csharp::SyntaxTrivia;

namespace {

std::string Message(ErrorCode code, const std::string &arg = "") {
    csharp::SyntaxDiagnosticInfo d;
    d.code = code;
    std::u16string a = csharp::ToUtf16(arg);
    if (!arg.empty()) {
        d.args[0] = csharp::Text(a);
        d.arg_count = 1;
    }
    return csharp::FormatDiagnosticMessage(d);
}

CsLspDiagnostic Make(const Doc &doc, TextSpan span, CsLspSeverity sev, ErrorCode code, const std::string &arg = "",
                     bool unnecessary = false) {
    CsLspDiagnostic d;
    d.range = doc.RangeOf(span);
    d.severity = sev;
    d.code = csharp::ErrorId(code);
    d.message = Message(code, arg);
    d.unnecessary = unnecessary;
    return d;
}

/** @brief Every DisabledTextTrivia's text in the file (code under a false #if), for "maybe used there". */
std::string DisabledText(const Doc &doc) {
    std::string out;
    std::function<void(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->IsToken()) {
            for (const SyntaxTrivia &t : n->leading) {
                if (t.Kind() == SyntaxKind::DisabledTextTrivia) out += doc.Text(t.Span()) + "\n";
            }
            for (const SyntaxTrivia &t : n->trailing) {
                if (t.Kind() == SyntaxKind::DisabledTextTrivia) out += doc.Text(t.Span()) + "\n";
            }
            return;
        }
        for (const SyntaxNode *c : n->children) walk(c);
    };
    walk(doc.tree->Root());
    return out;
}

bool ContainsWord(const std::string &text, const std::string &word) {
    size_t pos = 0;
    auto isId = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; };
    while ((pos = text.find(word, pos)) != std::string::npos) {
        bool left = pos == 0 || !isId(text[pos - 1]);
        bool right = pos + word.size() >= text.size() || !isId(text[pos + word.size()]);
        if (left && right) return true;
        pos += word.size();
    }
    return false;
}

/** @brief Whether an expression is a compile-time constant as far as syntax shows (Roslyn's CS0219 only fires
 *         for locals assigned constants: a write with a side effect counts as a use). */
bool IsConstantish(const SyntaxNode *e) {
    if (!e) return false;
    switch (e->Kind()) {
    case SyntaxKind::NumericLiteralExpression:
    case SyntaxKind::StringLiteralExpression:
    case SyntaxKind::Utf8StringLiteralExpression:
    case SyntaxKind::CharacterLiteralExpression:
    case SyntaxKind::TrueLiteralExpression:
    case SyntaxKind::FalseLiteralExpression:
    case SyntaxKind::NullLiteralExpression:
    case SyntaxKind::DefaultLiteralExpression:
    case SyntaxKind::DefaultExpression:
        return true;
    case SyntaxKind::ParenthesizedExpression:
        return IsConstantish(e->Field(Field::Expression));
    case SyntaxKind::UnaryMinusExpression:
    case SyntaxKind::UnaryPlusExpression:
    case SyntaxKind::BitwiseNotExpression:
    case SyntaxKind::LogicalNotExpression:
        return IsConstantish(e->Field(Field::Operand));
    case SyntaxKind::AddExpression:
    case SyntaxKind::SubtractExpression:
    case SyntaxKind::MultiplyExpression:
    case SyntaxKind::BitwiseOrExpression:
    case SyntaxKind::BitwiseAndExpression:
    case SyntaxKind::LeftShiftExpression:
        return IsConstantish(e->Field(Field::Left)) && IsConstantish(e->Field(Field::Right));
    case SyntaxKind::CastExpression:
        return IsConstantish(e->Field(Field::Expression));
    default:
        return false;
    }
}

/** @brief Whether writing `e` to a variable of declared type `type` writes a constant: null and default always;
 *         another constant only without a user-defined conversion or boxing (the declared type `var` or a built-in
 *         value type or string). */
bool ConstantWrite(const Doc &doc, const SyntaxNode *type, const SyntaxNode *e) {
    if (!IsConstantish(e)) return false;
    const SyntaxNode *x = e;
    while (x && x->Kind() == SyntaxKind::ParenthesizedExpression) x = x->Field(Field::Expression);
    if (x && (x->Kind() == SyntaxKind::NullLiteralExpression || x->Kind() == SyntaxKind::DefaultLiteralExpression ||
              x->Kind() == SyntaxKind::DefaultExpression)) {
        return true;
    }
    std::string t = type ? doc.Text(type) : "var";
    t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), t.end());
    if (!t.empty() && t.back() == '?') t.pop_back();
    static const std::set<std::string> builtin = {"var", "int", "uint", "long", "ulong", "short", "ushort", "byte",
                                                  "sbyte", "char", "bool", "string", "float", "double", "decimal",
                                                  "nint", "nuint"};
    return builtin.count(t) > 0;
}

// --- Reachability -----------------------------------------------------------------------------

bool EndsUnreachably(const SyntaxNode *st);

/** @brief Whether a loop body has a `break` that leaves this loop (not one of an inner loop or switch). */
bool HasBreakFor(const SyntaxNode *body) {
    std::function<bool(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->IsToken()) return false;
        switch (n->Kind()) {
        case SyntaxKind::BreakStatement:
            return true;
        case SyntaxKind::WhileStatement:
        case SyntaxKind::DoStatement:
        case SyntaxKind::ForStatement:
        case SyntaxKind::ForEachStatement:
        case SyntaxKind::ForEachVariableStatement:
        case SyntaxKind::SwitchStatement:
        case SyntaxKind::LocalFunctionStatement:
        case SyntaxKind::ParenthesizedLambdaExpression:
        case SyntaxKind::SimpleLambdaExpression:
        case SyntaxKind::AnonymousMethodExpression:
            return false;
        default:
            break;
        }
        for (const SyntaxNode *c : n->children) {
            if (walk(c)) return true;
        }
        return false;
    };
    return walk(body);
}

/** @brief Whether a statement list's end cannot be reached (some statement in it never completes). */
bool ListEndsUnreachably(const std::vector<const SyntaxNode *> &stmts) {
    for (const SyntaxNode *s : stmts) {
        if (s->Kind() == SyntaxKind::LabeledStatement) return false;  // a goto may land after it (conservative)
        if (EndsUnreachably(s)) return true;
    }
    return false;
}

bool IsTrueLiteral(const SyntaxNode *e) {
    while (e && e->Kind() == SyntaxKind::ParenthesizedExpression) e = e->Field(Field::Expression);
    return e && e->Kind() == SyntaxKind::TrueLiteralExpression;
}

bool EndsUnreachably(const SyntaxNode *st) {
    if (!st) return false;
    switch (st->Kind()) {
    case SyntaxKind::ReturnStatement:
    case SyntaxKind::ThrowStatement:
    case SyntaxKind::BreakStatement:
    case SyntaxKind::ContinueStatement:
    case SyntaxKind::GotoStatement:
    case SyntaxKind::GotoCaseStatement:
    case SyntaxKind::GotoDefaultStatement:
    case SyntaxKind::YieldBreakStatement:
        return true;
    case SyntaxKind::ExpressionStatement: {
        // `throw` as an expression statement does not parse; nothing else here never completes.
        return false;
    }
    case SyntaxKind::Block:
        return ListEndsUnreachably(st->FieldItems(Field::Statements));
    case SyntaxKind::IfStatement: {
        const SyntaxNode *els = st->Field(Field::Else);
        if (!els) return false;
        return EndsUnreachably(st->Field(Field::Statement)) && EndsUnreachably(els->Field(Field::Statement));
    }
    case SyntaxKind::WhileStatement:
        return IsTrueLiteral(st->Field(Field::Condition)) && !HasBreakFor(st->Field(Field::Statement));
    case SyntaxKind::DoStatement:
        return IsTrueLiteral(st->Field(Field::Condition)) && !HasBreakFor(st->Field(Field::Statement));
    case SyntaxKind::ForStatement:
        return (!st->Field(Field::Condition) || IsTrueLiteral(st->Field(Field::Condition))) &&
               !HasBreakFor(st->Field(Field::Statement));
    case SyntaxKind::LockStatement:
    case SyntaxKind::UsingStatement:
    case SyntaxKind::FixedStatement:
    case SyntaxKind::UnsafeStatement:
    case SyntaxKind::CheckedStatement:
    case SyntaxKind::UncheckedStatement:
        return EndsUnreachably(st->Field(Field::Statement) ? st->Field(Field::Statement) : st->Field(Field::Block));
    case SyntaxKind::TryStatement: {
        if (!EndsUnreachably(st->Field(Field::Block))) {
            // A finally that never completes makes the whole statement end unreachably.
            const SyntaxNode *fin = st->Field(Field::Finally);
            return fin && EndsUnreachably(fin->Field(Field::Block));
        }
        for (const SyntaxNode *c : st->FieldItems(Field::Catches)) {
            if (!EndsUnreachably(c->Field(Field::Block))) return false;
        }
        return true;
    }
    case SyntaxKind::SwitchStatement: {
        bool hasDefault = false;
        for (const SyntaxNode *sec : st->FieldItems(Field::Sections)) {
            for (const SyntaxNode *l : sec->FieldItems(Field::Labels)) {
                if (l->Kind() == SyntaxKind::DefaultSwitchLabel) hasDefault = true;
            }
            std::vector<const SyntaxNode *> stmts = sec->FieldItems(Field::Statements);
            // A break anywhere that targets this switch makes its end reachable.
            for (const SyntaxNode *s : stmts) {
                if (HasBreakFor(s)) return false;
            }
            if (!ListEndsUnreachably(stmts)) return false;
        }
        return hasDefault;
    }
    case SyntaxKind::LabeledStatement:
        return EndsUnreachably(st->Field(Field::Statement));
    default:
        return false;
    }
}

/** @brief Where Roslyn puts CS0162 for an unreachable statement: a block is not reported itself but its first
 *         statement is; an unreachable `throw` is never reported (nor an empty statement or a local function). */
const SyntaxNode *Reportable(const SyntaxNode *s) {
    switch (s->Kind()) {
    case SyntaxKind::Block:
        for (const SyntaxNode *c : s->FieldItems(Field::Statements)) {
            const SyntaxNode *r = Reportable(c);
            if (r) return r;
        }
        return nullptr;
    case SyntaxKind::ThrowStatement:
    case SyntaxKind::EmptyStatement:
    case SyntaxKind::LocalFunctionStatement:
        return nullptr;
    default:
        return s;
    }
}

void CheckUnreachable(const Doc &doc, const Binding &b, const SyntaxNode *n, std::vector<CsLspDiagnostic> &out) {
    if (n->IsToken()) return;
    if (n->Kind() == SyntaxKind::Block || n->Kind() == SyntaxKind::SwitchSection) {
        std::vector<const SyntaxNode *> stmts = n->FieldItems(Field::Statements);
        bool dead = false;
        for (const SyntaxNode *s : stmts) {
            if (dead && s->Kind() == SyntaxKind::LabeledStatement) {
                // A label some goto names makes what follows reachable again.
                int sym = SymbolAt(b, s->Field(Field::Identifier));
                if (sym >= 0 && !b.symbols[static_cast<size_t>(sym)].refs.empty()) dead = false;
            }
            if (dead) {
                const SyntaxNode *r = Reportable(s);
                if (r) {
                    out.push_back(Make(doc, r->Span(), CsLspSeverity::Warning, ErrorCode::WRN_UnreachableCode, "", true));
                    break;
                }
                continue;
            }
            if (EndsUnreachably(s)) dead = true;
        }
    }
    for (const SyntaxNode *c : n->children) CheckUnreachable(doc, b, c, out);
}

// --- Names used in a file ---------------------------------------------------------------------

/** @brief Every identifier spelled in the file outside the using directives, with string contents (nameof,
 *         reflection) and disabled text. */
std::set<std::string> NamesUsed(const Doc &doc, const SyntaxNode *exclude_decl_token = nullptr) {
    std::set<std::string> names;
    std::function<void(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->Kind() == SyntaxKind::UsingDirective) return;
        if (n->IsToken()) {
            if (n == exclude_decl_token) return;
            if (n->Kind() == SyntaxKind::IdentifierToken) names.insert(Doc::TokenValue(n));
            if (n->Kind() == SyntaxKind::StringLiteralToken || n->Kind() == SyntaxKind::InterpolatedStringTextToken) {
                std::string v = csharp::ToUtf8(n->ValueText());
                std::string cur;
                for (char c : v + " ") {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') cur += c;
                    else {
                        if (!cur.empty()) names.insert(cur);
                        cur.clear();
                    }
                }
            }
            return;
        }
        for (const SyntaxNode *c : n->children) walk(c);
    };
    walk(doc.tree->Root());
    return names;
}

/** @brief How many identifier tokens in the file spell `name` (declarations included). */
int CountName(const Doc &doc, const std::string &name) {
    int count = 0;
    std::function<void(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->IsToken()) {
            if (n->Kind() == SyntaxKind::IdentifierToken && Doc::TokenValue(n) == name) count++;
            if ((n->Kind() == SyntaxKind::StringLiteralToken || n->Kind() == SyntaxKind::InterpolatedStringTextToken) &&
                ContainsWord(csharp::ToUtf8(n->ValueText()), name)) {
                count++;
            }
            for (const SyntaxTrivia &t : n->leading) {
                if (t.structure) {
                    // Doc comments: <see cref="Name"/> counts as a use.
                    std::string s = doc.Text(t.Span());
                    if (ContainsWord(s, name)) count++;
                }
            }
            return;
        }
        for (const SyntaxNode *c : n->children) walk(c);
    };
    walk(doc.tree->Root());
    return count;
}

bool IsPartialType(const SyntaxNode *typeDecl) { return HasModifier(typeDecl, SyntaxKind::PartialKeyword); }

bool HasAttributes(const SyntaxNode *decl) { return !decl->FieldItems(Field::AttributeLists).empty(); }

bool IsPrivate(const SyntaxNode *decl) {
    for (SyntaxKind k : {SyntaxKind::PublicKeyword, SyntaxKind::ProtectedKeyword, SyntaxKind::InternalKeyword}) {
        if (HasModifier(decl, k)) return false;
    }
    return true;
}

// --- Unused usings ----------------------------------------------------------------------------

void CheckUsings(const Doc &doc, const SyntaxNode *container, const std::set<std::string> &used,
                 bool fileHasDirectives, bool hasQuery, std::vector<CsLspDiagnostic> &out) {
    std::set<std::string> seen;
    const BclIndex &bcl = Bcl();
    for (const SyntaxNode *u : container->FieldItems(Field::Usings)) {
        if (u->Kind() != SyntaxKind::UsingDirective) continue;
        const SyntaxNode *name = u->Field(Field::NamespaceOrType);
        if (!name) name = u->Field(Field::Name);
        if (!name) continue;
        std::string text = doc.Text(name);
        text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), text.end());
        bool isStatic = u->Field(Field::StaticKeyword) != nullptr;
        bool isAlias = u->Field(Field::Alias) != nullptr;
        bool isGlobal = u->Field(Field::GlobalKeyword) != nullptr;
        std::string key = std::string(isStatic ? "static " : "") + (isAlias ? doc.Text(u->Field(Field::Alias)) + "=" : "") + text;
        if (!seen.insert(key).second) {
            out.push_back(Make(doc, name->Span(), CsLspSeverity::Warning, ErrorCode::WRN_DuplicateUsing, text));
            continue;
        }
        if (isStatic || isAlias || isGlobal || fileHasDirectives) continue;
        // Only a namespace whose every type the BCL table knows: is any of its type names (or extension
        // method names) spelled in the file?
        if (!std::binary_search(bcl.namespaces.begin(), bcl.namespaces.end(), text)) continue;
        bool usedHere = false;
        if (text == "System.Linq" && hasQuery) usedHere = true;
        for (size_t i = 0; i < bcl.type_count && !usedHere; i++) {
            TypeName tn = ParseTypeName(bcl.types[i].name);
            if (tn.name.size() <= text.size() || tn.name.compare(0, text.size(), text) != 0 || tn.name[text.size()] != '.') continue;
            std::string rest = tn.name.substr(text.size() + 1);
            size_t dot = rest.find('.');
            std::string first = dot == std::string::npos ? rest : rest.substr(0, dot);
            size_t tick = first.find('`');
            if (tick != std::string::npos) first = first.substr(0, tick);
            if (used.count(first)) usedHere = true;
            // [Obsolete] means ObsoleteAttribute.
            if (first.size() > 9 && first.compare(first.size() - 9, 9, "Attribute") == 0 &&
                used.count(first.substr(0, first.size() - 9))) {
                usedHere = true;
            }
        }
        if (!usedHere) {
            for (const auto &kv : bcl.extensions) {
                const CsBclMember &m = bcl.members[kv.second];
                TypeName owner = ParseTypeName(m.owner);
                size_t dot = owner.name.rfind('.');
                if (dot != std::string::npos && owner.name.substr(0, dot) == text && used.count(m.name)) {
                    usedHere = true;
                    break;
                }
            }
        }
        if (!usedHere) {
            // A nested namespace referenced through this one (using System; ... IO.File).
            for (const std::string &ns : bcl.namespaces) {
                if (ns.size() > text.size() && ns.compare(0, text.size(), text) == 0 && ns[text.size()] == '.') {
                    std::string rest = ns.substr(text.size() + 1);
                    if (used.count(rest.substr(0, rest.find('.')))) {
                        usedHere = true;
                        break;
                    }
                }
            }
        }
        if (!usedHere) {
            CsLspDiagnostic d = Make(doc, u->Span(), CsLspSeverity::Hint, ErrorCode::HDN_UnusedUsingDirective, "", true);
            out.push_back(d);
        }
    }
    for (const SyntaxNode *m : container->FieldItems(Field::Members)) {
        if (m->Kind() == SyntaxKind::NamespaceDeclaration || m->Kind() == SyntaxKind::FileScopedNamespaceDeclaration) {
            CheckUsings(doc, m, used, fileHasDirectives, hasQuery, out);
        }
    }
}

// --- Members ----------------------------------------------------------------------------------

void CheckMembers(const Doc &doc, const SyntaxNode *n, const std::string &disabled, std::vector<CsLspDiagnostic> &out) {
    if (n->IsToken()) return;
    bool isType = n->Kind() == SyntaxKind::ClassDeclaration || n->Kind() == SyntaxKind::StructDeclaration ||
                  n->Kind() == SyntaxKind::RecordDeclaration || n->Kind() == SyntaxKind::RecordStructDeclaration;
    if (isType && !IsPartialType(n) && !HasAttributes(n)) {
        std::string typeName = Doc::TokenValue(n->Field(Field::Identifier));
        for (const SyntaxNode *m : n->FieldItems(Field::Members)) {
            if (m->Kind() == SyntaxKind::FieldDeclaration && IsPrivate(m) && !HasAttributes(m) &&
                !HasModifier(m, SyntaxKind::ConstKeyword) && n->Kind() != SyntaxKind::StructDeclaration &&
                n->Kind() != SyntaxKind::RecordStructDeclaration) {
                const SyntaxNode *vd = m->Field(Field::Declaration);
                for (const SyntaxNode *v : vd ? vd->FieldItems(Field::Variables) : std::vector<const SyntaxNode *>{}) {
                    if (v->Kind() != SyntaxKind::VariableDeclarator) continue;
                    const SyntaxNode *id = v->Field(Field::Identifier);
                    std::string name = Doc::TokenValue(id);
                    if (name.empty() || ContainsWord(disabled, name)) continue;
                    if (CountName(doc, name) <= 1) {
                        // Never named again: CS0169; CS0414 when it is initialized with a constant (Roslyn counts
                        // any other initializer as a use, as for locals).
                        const SyntaxNode *init = v->Field(Field::Initializer);
                        if (init && !ConstantWrite(doc, vd->Field(Field::Type), init->Field(Field::Value))) continue;
                        ErrorCode code = init ? ErrorCode::WRN_UnreferencedFieldAssg : ErrorCode::WRN_UnreferencedField;
                        out.push_back(Make(doc, id->Span(), CsLspSeverity::Warning, code, typeName + "." + name, true));
                    }
                }
            }
            if (m->Kind() == SyntaxKind::MethodDeclaration && IsPrivate(m) && !HasAttributes(m) &&
                !m->Field(Field::ExplicitInterfaceSpecifier) && !HasModifier(m, SyntaxKind::PartialKeyword) &&
                !HasModifier(m, SyntaxKind::ExternKeyword) && !HasModifier(m, SyntaxKind::OverrideKeyword) &&
                !HasModifier(m, SyntaxKind::VirtualKeyword) && !HasModifier(m, SyntaxKind::AbstractKeyword)) {
                const SyntaxNode *id = m->Field(Field::Identifier);
                std::string name = Doc::TokenValue(id);
                if (name == "Main" || name.empty() || ContainsWord(disabled, name)) continue;
                if (CountName(doc, name) <= 1) {
                    CsLspDiagnostic d;
                    d.range = doc.RangeOf(id);
                    d.severity = CsLspSeverity::Hint;
                    d.code = "IDE0051";
                    d.message = "Private member '" + typeName + "." + name + "' is unused";
                    d.unnecessary = true;
                    out.push_back(d);
                }
            }
        }
    }
    for (const SyntaxNode *c : n->children) CheckMembers(doc, c, disabled, out);
}

/** @brief Whether `name` is a field (not a property) of the type declaration enclosing `n`. */
bool IsFieldOfEnclosingType(const SyntaxNode *n, const std::string &name) {
    const SyntaxNode *t = n;
    while (t && !IsTypeDeclKind(t->Kind())) t = t->parent;
    if (!t) return false;
    for (const SyntaxNode *m : t->FieldItems(Field::Members)) {
        if (m->Kind() != SyntaxKind::FieldDeclaration) continue;
        const SyntaxNode *vd = m->Field(Field::Declaration);
        for (const SyntaxNode *v : vd ? vd->FieldItems(Field::Variables) : std::vector<const SyntaxNode *>{}) {
            if (v->Kind() == SyntaxKind::VariableDeclarator && Doc::TokenValue(v->Field(Field::Identifier)) == name) return true;
        }
    }
    return false;
}

bool BodyOnlyThrows(const SyntaxNode *decl) {
    const SyntaxNode *body = decl->Field(Field::Body);
    if (body) {
        std::vector<const SyntaxNode *> st = body->FieldItems(Field::Statements);
        return st.empty() || (st.size() == 1 && st[0]->Kind() == SyntaxKind::ThrowStatement);
    }
    const SyntaxNode *arrow = decl->Field(Field::ExpressionBody);
    const SyntaxNode *e = arrow ? arrow->Field(Field::Expression) : nullptr;
    return e && e->Kind() == SyntaxKind::ThrowExpression;
}

}  // namespace

std::vector<CsLspDiagnostic> CsLspDiagnostics(const std::vector<std::string> &lines, const std::string &path) {
    (void)path;
    std::vector<CsLspDiagnostic> out;
    Doc doc(lines);
    bool hasError = false;
    for (const csharp::Diagnostic &d : doc.tree->Diagnostics()) {
        CsLspDiagnostic x;
        x.range = doc.RangeOf(d.span);
        x.severity = d.severity == DiagnosticSeverity::Error     ? CsLspSeverity::Error
                     : d.severity == DiagnosticSeverity::Warning ? CsLspSeverity::Warning
                                                                 : CsLspSeverity::Information;
        x.code = d.Id();
        x.message = d.message;
        if (d.severity == DiagnosticSeverity::Error) hasError = true;
        out.push_back(x);
    }
    if (hasError) return out;  // the lints wait until the file parses

    const std::string disabled = DisabledText(doc);
    Binding b = Bind(doc);

    // Locals, labels, local functions.
    for (const LocalSymbol &s : b.symbols) {
        if (ContainsWord(disabled, s.name)) continue;
        size_t reads = 0;
        for (size_t i = 0; i < s.refs.size(); i++) {
            if (!s.ref_is_write[i]) reads++;
        }
        TextSpan at = s.name_token->Span();
        switch (s.kind) {
        case SymKind::Local:
            if (s.decl && s.decl->Kind() != SyntaxKind::VariableDeclarator) break;  // `var (a, b) = ...`, out var
            if (s.refs.empty() && !s.init) {
                out.push_back(Make(doc, at, CsLspSeverity::Warning, ErrorCode::WRN_UnreferencedVar, s.name, true));
            } else if (reads == 0) {
                bool allConst = !s.writes.empty();
                for (const SyntaxNode *w : s.writes) {
                    if (!ConstantWrite(doc, s.type, w)) allConst = false;
                }
                if (allConst) {
                    out.push_back(Make(doc, at, CsLspSeverity::Warning, ErrorCode::WRN_UnreferencedVarAssg, s.name, true));
                }
            }
            break;
        case SymKind::CatchVariable:
            if (s.refs.empty()) {
                out.push_back(Make(doc, at, CsLspSeverity::Warning, ErrorCode::WRN_UnreferencedVar, s.name, true));
            }
            break;
        case SymKind::LocalFunction:
            if (s.refs.empty()) {
                // A local function used only from another unused local function still counts as used here.
                out.push_back(
                    Make(doc, at, CsLspSeverity::Warning, ErrorCode::WRN_UnreferencedLocalFunction, s.name, true));
            }
            break;
        case SymKind::Label:
            if (s.refs.empty()) {
                out.push_back(Make(doc, at, CsLspSeverity::Warning, ErrorCode::WRN_UnreferencedLabel, "", true));
            }
            break;
        case SymKind::Parameter: {
            // Unused parameters of methods with real bodies: a hint (IDE0060).
            if (!s.refs.empty() || s.name.empty() || s.name[0] == '_') break;
            const SyntaxNode *list = s.decl ? s.decl->parent : nullptr;
            const SyntaxNode *owner = list ? list->parent : nullptr;
            if (!owner) break;
            SyntaxKind ok = owner->Kind();
            if (ok != SyntaxKind::MethodDeclaration && ok != SyntaxKind::ConstructorDeclaration &&
                ok != SyntaxKind::LocalFunctionStatement && ok != SyntaxKind::OperatorDeclaration) {
                break;
            }
            if (HasModifier(owner, SyntaxKind::OverrideKeyword) || HasModifier(owner, SyntaxKind::VirtualKeyword) ||
                HasModifier(owner, SyntaxKind::AbstractKeyword) || HasModifier(owner, SyntaxKind::ExternKeyword) ||
                HasModifier(owner, SyntaxKind::PartialKeyword) || owner->Field(Field::ExplicitInterfaceSpecifier) ||
                (!owner->Field(Field::Body) && !owner->Field(Field::ExpressionBody)) || BodyOnlyThrows(owner)) {
                break;
            }
            const SyntaxNode *typeDecl = Ancestor(owner->parent, {SyntaxKind::InterfaceDeclaration});
            if (typeDecl) break;
            if (s.name == "sender" || s.name == "e" || s.name == "args") break;  // event handlers, Main
            if (ContainsWord(disabled, s.name)) break;
            CsLspDiagnostic d;
            d.range = doc.RangeOf(at);
            d.severity = CsLspSeverity::Hint;
            d.code = "IDE0060";
            d.message = "Remove unused parameter '" + s.name + "'";
            d.unnecessary = true;
            out.push_back(d);
            break;
        }
        default:
            break;
        }
    }

    CheckUnreachable(doc, b, doc.tree->Root(), out);

    // Statement-level checks: a mistaken empty statement, self-assignment, self-comparison.
    std::function<void(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->IsToken()) return;
        switch (n->Kind()) {
        case SyntaxKind::IfStatement:
        case SyntaxKind::WhileStatement:
        case SyntaxKind::ForStatement:
        case SyntaxKind::ForEachStatement:
        case SyntaxKind::ForEachVariableStatement:
        case SyntaxKind::UsingStatement:
        case SyntaxKind::LockStatement:
        case SyntaxKind::FixedStatement:
        case SyntaxKind::ElseClause: {
            const SyntaxNode *body = n->Field(Field::Statement);
            bool loop = n->Kind() == SyntaxKind::WhileStatement || n->Kind() == SyntaxKind::ForStatement ||
                        n->Kind() == SyntaxKind::ForEachStatement || n->Kind() == SyntaxKind::ForEachVariableStatement;
            if (body && loop && body->Kind() == SyntaxKind::EmptyStatement) {
                // A loop's empty body is a warning only when a block follows (`while (x); { ... }`).
                const SyntaxNode *next = body->LastToken() ? body->LastToken()->NextToken() : nullptr;
                if (!next || next->Kind() != SyntaxKind::OpenBraceToken) body = nullptr;
            }
            if (body && body->Kind() == SyntaxKind::EmptyStatement && !body->IsMissing()) {
                out.push_back(Make(doc, body->Span(), CsLspSeverity::Warning, ErrorCode::WRN_PossibleMistakenNullStatement));
            }
            break;
        }
        case SyntaxKind::SimpleAssignmentExpression:
        case SyntaxKind::EqualsExpression:
        case SyntaxKind::NotEqualsExpression:
        case SyntaxKind::LessThanExpression:
        case SyntaxKind::LessThanOrEqualExpression:
        case SyntaxKind::GreaterThanExpression:
        case SyntaxKind::GreaterThanOrEqualExpression: {
            const SyntaxNode *l = n->Field(Field::Left);
            const SyntaxNode *r = n->Field(Field::Right);
            // The same local, parameter or field on both sides (a property is left alone: it may have effects).
            auto same = [&](const SyntaxNode *x, const SyntaxNode *y) -> bool {
                if (!x || !y) return false;
                if (x->Kind() == SyntaxKind::IdentifierName && y->Kind() == SyntaxKind::IdentifierName) {
                    const SyntaxNode *xt = x->Field(Field::Identifier);
                    const SyntaxNode *yt = y->Field(Field::Identifier);
                    int xs = SymbolAt(b, xt), ys = SymbolAt(b, yt);
                    if (xs >= 0 || ys >= 0) return xs == ys;
                    return Doc::TokenValue(xt) == Doc::TokenValue(yt) && IsFieldOfEnclosingType(x, Doc::TokenValue(xt));
                }
                if (x->Kind() == SyntaxKind::SimpleMemberAccessExpression &&
                    y->Kind() == SyntaxKind::SimpleMemberAccessExpression) {
                    const SyntaxNode *xe = x->Field(Field::Expression), *ye = y->Field(Field::Expression);
                    if (!xe || !ye || xe->Kind() != SyntaxKind::ThisExpression || ye->Kind() != SyntaxKind::ThisExpression) return false;
                    std::string xn = doc.Text(x->Field(Field::Name)), yn = doc.Text(y->Field(Field::Name));
                    return xn == yn && IsFieldOfEnclosingType(x, xn);
                }
                return false;
            };
            // `new T { X = X }` / `with { X = X }`: the left is the new object's member, not a variable.
            bool inInitializer = n->parent && (n->parent->Kind() == SyntaxKind::ObjectInitializerExpression ||
                                               n->parent->Kind() == SyntaxKind::WithInitializerExpression ||
                                               n->parent->Kind() == SyntaxKind::AnonymousObjectMemberDeclarator);
            if (!inInitializer && same(l, r)) {
                bool assign = n->Kind() == SyntaxKind::SimpleAssignmentExpression;
                out.push_back(Make(doc, n->Span(), CsLspSeverity::Warning,
                                   assign ? ErrorCode::WRN_AssignmentToSelf : ErrorCode::WRN_ComparisonToSelf));
            }
            break;
        }
        default:
            break;
        }
        for (const SyntaxNode *c : n->children) walk(c);
    };
    walk(doc.tree->Root());

    CheckMembers(doc, doc.tree->Root(), disabled, out);

    // Usings.
    bool fileHasDirectives = doc.tree->GreenRoot()->ContainsDirectives();
    bool hasQuery = false;
    std::function<void(const SyntaxNode *)> findQuery = [&](const SyntaxNode *n) {
        if (hasQuery || n->IsToken()) return;
        if (n->Kind() == SyntaxKind::QueryExpression) hasQuery = true;
        for (const SyntaxNode *c : n->children) findQuery(c);
    };
    findQuery(doc.tree->Root());
    CheckUsings(doc, doc.tree->Root(), NamesUsed(doc), fileHasDirectives, hasQuery, out);

    // `#pragma warning disable CS0219` / `restore`: honoured for the lints, as the compiler honours them.
    struct Pragma {
        int pos;
        bool disable;
        std::set<std::string> codes;  // empty: all
    };
    std::vector<Pragma> pragmas;
    std::function<void(const SyntaxNode *)> findPragmas = [&](const SyntaxNode *n) {
        if (n->IsToken()) {
            for (const SyntaxTrivia &t : n->leading) {
                if (t.Kind() != SyntaxKind::PragmaWarningDirectiveTrivia || !t.structure) continue;
                Pragma pr;
                pr.pos = t.Span().start;
                const SyntaxNode *kw = t.structure->Field(Field::DisableOrRestoreKeyword);
                pr.disable = kw && kw->Kind() == SyntaxKind::DisableKeyword;
                for (const SyntaxNode *c : t.structure->FieldItems(Field::ErrorCodes)) {
                    if (c->IsToken()) continue;
                    std::string code = doc.Text(c);
                    if (!code.empty() && std::isdigit(static_cast<unsigned char>(code[0]))) {
                        while (code.size() < 4) code = "0" + code;
                        code = "CS" + code;
                    }
                    pr.codes.insert(code);
                }
                pragmas.push_back(pr);
            }
            return;
        }
        for (const SyntaxNode *c : n->children) findPragmas(c);
    };
    findPragmas(doc.tree->Root());
    if (!pragmas.empty()) {
        std::vector<CsLspDiagnostic> kept;
        for (const CsLspDiagnostic &d : out) {
            if (d.severity == CsLspSeverity::Error) {
                kept.push_back(d);
                continue;
            }
            int pos = doc.OffsetAt(d.range.start_line, d.range.start_col);
            bool suppressed = false;
            for (const Pragma &pr : pragmas) {
                if (pr.pos > pos) break;
                if (pr.codes.empty() || pr.codes.count(d.code)) suppressed = pr.disable;
            }
            if (!suppressed) kept.push_back(d);
        }
        out.swap(kept);
    }

    std::stable_sort(out.begin(), out.end(), [](const CsLspDiagnostic &a, const CsLspDiagnostic &c) {
        return a.range.start_line != c.range.start_line ? a.range.start_line < c.range.start_line
                                                        : a.range.start_col < c.range.start_col;
    });
    return out;
}

std::string CsLspFormatDiagnostic(const std::string &path, const CsLspDiagnostic &d) {
    const char *sev = d.severity == CsLspSeverity::Error     ? "error"
                      : d.severity == CsLspSeverity::Warning ? "warning"
                      : d.severity == CsLspSeverity::Hint    ? "hint"
                                                             : "info";
    char buf[64];
    std::snprintf(buf, sizeof buf, ":%d:%d: ", d.range.start_line + 1, d.range.start_col + 1);
    return path + buf + sev + ": " + d.message + " [" + d.code + "]";
}

bool CsLspDiagnosticFails(const CsLspDiagnostic &d) {
    return d.severity == CsLspSeverity::Error || d.severity == CsLspSeverity::Warning;
}
