// The features of mep's own C# language server over the analysis
// (csharp_lsp_analysis.h): the project index, completion, hover, signature
// help, outline, folds, definition, references, rename. See csharp_lsp.h.
#include "csharp_format.h"
#include "csharp_lsp.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>

#include "csharp_lsp_analysis.h"

using namespace cslsp;
using csharp::GreenNode;
using csharp::SyntaxTrivia;

namespace {

/** @brief Everything one request needs about one file. */
struct Analysis {
    Doc doc;
    Binding binding;
    SourceIndex current;
    Universe universe;
    Model model;
    Analysis(const std::vector<std::string> &lines, const CsProject *project)
        : doc(lines), binding(Bind(doc)), current(MakeIndex(doc)), universe(&current, project),
          model(doc, binding, universe, current) {}

private:
    static SourceIndex MakeIndex(const Doc &d) {
        SourceIndex idx;
        IndexDeclarations(d, "", &idx);
        return idx;
    }
};

std::string Lower(const std::string &s) {
    std::string out;
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }

std::string Fence(const std::string &code) { return "```csharp\n" + code + "\n```"; }

std::string MemberDisplay(const MemberInfo &m) {
    if (!m.signature.empty()) return m.signature;
    return ShowType(m.type) + " " + m.name + m.params;
}

CsLspKind CompletionKindOf(const std::string &kind) {
    if (kind == "method") return CsLspKind::Method;
    if (kind == "constructor") return CsLspKind::Constructor;
    if (kind == "property" || kind == "indexer") return CsLspKind::Property;
    if (kind == "field") return CsLspKind::Field;
    if (kind == "event") return CsLspKind::Event;
    if (kind == "const") return CsLspKind::Constant;
    if (kind == "enum member") return CsLspKind::EnumMember;
    if (kind == "interface") return CsLspKind::Interface;
    if (kind == "enum") return CsLspKind::Enum;
    if (kind == "struct" || kind == "record struct") return CsLspKind::Struct;
    return CsLspKind::Class;
}

std::string LocalKindName(SymKind k) {
    switch (k) {
    case SymKind::Parameter:
        return "parameter";
    case SymKind::RangeVariable:
        return "range variable";
    case SymKind::LocalFunction:
        return "local function";
    case SymKind::Label:
        return "label";
    default:
        return "local variable";
    }
}

/** @brief A local's type as a reader sees it ("int", "List<string>", or "var" when not visible). */
std::string LocalTypeText(const Analysis &a, int sym) {
    const LocalSymbol &s = a.binding.symbols[static_cast<size_t>(sym)];
    if (s.kind == SymKind::LocalFunction) {
        return s.decl ? DeclHeader(a.doc, s.decl) : s.name;
    }
    if (s.type && a.doc.Text(s.type) != "var") return a.doc.Text(s.type);
    ResolvedType t = a.model.TypeOfSymbol(sym);
    if (t.Known()) return ShowType(t.name.Empty() ? ParseTypeName(t.FullName()) : t.name);
    return s.type ? "var" : "";
}

/** @brief The previous non-missing token before `pos` (skipping the word being typed). */
const SyntaxNode *TokenBefore(const Doc &doc, int pos) {
    const SyntaxNode *t = doc.tree->FindToken(pos > 0 ? pos - 1 : 0);
    while (t && (t->IsMissing() || t->Span().start >= pos)) t = t->PreviousToken();
    return t;
}

const std::vector<std::string> &Keywords() {
    static const std::vector<std::string> kws = [] {
        std::vector<std::string> v;
        for (csharp::SyntaxKind k : csharp::SyntaxFacts::GetReservedKeywordKinds()) {
            v.push_back(csharp::ToUtf8(csharp::SyntaxFacts::GetText(k)));
        }
        for (const char *c : {"var", "async", "await", "dynamic", "nameof", "record", "init", "required", "get", "set",
                              "value", "when", "where", "yield", "partial", "global", "file", "with", "and", "or",
                              "not", "managed", "unmanaged", "notnull", "scoped", "field"}) {
            v.push_back(c);
        }
        return v;
    }();
    return kws;
}

}  // namespace

// --- Project ----------------------------------------------------------------------------------------

void CsProjectAddFile(CsProject *project, const std::string &path, const std::vector<std::string> &lines) {
    project->version++;
    if (std::find(project->paths.begin(), project->paths.end(), path) != project->paths.end()) {
        project->defs.erase(std::remove_if(project->defs.begin(), project->defs.end(),
                                           [&](const CsProjectDef &d) { return d.path == path; }),
                            project->defs.end());
    } else {
        project->paths.push_back(path);
    }
    Doc doc(lines);
    SourceIndex idx;
    IndexDeclarations(doc, path, &idx);
    for (const SourceType &t : idx.types) {
        CsProjectDef d;
        d.kind = t.kind;
        d.ns = t.ns;
        d.owner = t.owner;
        d.name = t.name;
        d.signature = t.signature;
        d.doc = t.doc;
        for (size_t i = 0; i < t.bases.size(); i++) d.bases += (i ? ", " : "") + t.bases[i];
        d.path = path;
        d.range = t.range;
        project->defs.push_back(d);
    }
    for (const SourceMember &m : idx.members) {
        CsProjectDef d;
        d.kind = m.kind;
        d.owner = m.owner_full;
        d.name = m.name;
        d.signature = m.signature;
        d.doc = m.doc;
        d.type = m.type;
        d.params = m.params;
        d.is_static = m.is_static;
        d.is_extension = m.is_extension;
        d.path = path;
        d.range = m.range;
        project->defs.push_back(d);
    }
}

// --- Completion ----------------------------------------------------------------------------------------

std::vector<CsLspCompletionItem> CsLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                  const CsProject *project) {
    std::vector<CsLspCompletionItem> items;
    Analysis a(lines, project);
    const Doc &doc = a.doc;
    const int pos = doc.OffsetAt(line, col);
    // The word being typed, and where it starts on the line.
    const std::string &text = line >= 0 && line < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(line)] : "";
    int start = std::min(col, static_cast<int>(text.size()));
    while (start > 0 && IsIdentChar(text[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = text.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    const std::string lprefix = Lower(prefix);
    int wordStartPos = doc.OffsetAt(line, start);
    // Inside a comment or string: nothing.
    {
        const SyntaxNode *t = doc.tree->FindToken(pos);
        if (t) {
            for (const SyntaxTrivia &tr : t->leading) {
                TextSpan s = tr.Span();
                if (pos > s.start && pos < s.end + (tr.Kind() == SyntaxKind::SingleLineCommentTrivia ? 1 : 0) &&
                    (tr.Kind() == SyntaxKind::SingleLineCommentTrivia || tr.Kind() == SyntaxKind::MultiLineCommentTrivia ||
                     tr.Kind() == SyntaxKind::SingleLineDocumentationCommentTrivia)) {
                    return items;
                }
            }
            SyntaxKind k = t->Kind();
            if ((k == SyntaxKind::StringLiteralToken || k == SyntaxKind::CharacterLiteralToken ||
                 k == SyntaxKind::InterpolatedStringTextToken) &&
                t->Span().start < pos && pos < t->Span().end) {
                return items;
            }
        }
    }

    std::set<std::string> seen;
    auto add = [&](const std::string &label, CsLspKind kind, const std::string &detail, const std::string &docText,
                   const std::string &sort) {
        if (label.empty()) return;
        if (!lprefix.empty() && Lower(label).rfind(lprefix, 0) != 0) return;
        if (!seen.insert(label).second) return;
        CsLspCompletionItem it;
        it.label = label;
        it.insert_text = label;
        it.kind = kind;
        it.detail = detail;
        it.documentation = docText;
        it.replace_start = start;
        it.replace_end = col;
        it.sort_text = sort + label;
        items.push_back(it);
    };
    auto addMembers = [&](const std::vector<MemberInfo> &ms) {
        std::map<std::string, int> overloads;
        for (const MemberInfo &m : ms) overloads[m.name]++;
        for (const MemberInfo &m : ms) {
            if (m.kind == "constructor" || m.kind == "indexer") continue;
            std::string detail = MemberDisplay(m);
            int n = overloads[m.name];
            if (n > 1) detail += " (+" + std::to_string(n - 1) + " overload" + (n > 2 ? "s" : "") + ")";
            add(m.name, CompletionKindOf(m.kind), detail, m.doc, m.is_extension ? "2" : "1");
        }
    };

    // After `x.`, `x?.`, `N.`: the receiver.
    const SyntaxNode *before = TokenBefore(doc, wordStartPos);
    if (before && (before->Kind() == SyntaxKind::DotToken || before->Kind() == SyntaxKind::QuestionToken ||
                   before->Kind() == SyntaxKind::MinusGreaterThanToken)) {
        const SyntaxNode *p = before->parent;
        const SyntaxNode *receiver = nullptr;
        if (p && (p->Kind() == SyntaxKind::SimpleMemberAccessExpression || p->Kind() == SyntaxKind::PointerMemberAccessExpression)) {
            receiver = p->Field(Field::Expression);
        } else if (p && p->Kind() == SyntaxKind::QualifiedName) {
            receiver = p->Field(Field::Left);
        } else if (p && p->Kind() == SyntaxKind::MemberBindingExpression) {
            const SyntaxNode *ca = Ancestor(p, {SyntaxKind::ConditionalAccessExpression});
            if (ca) receiver = ca->Field(Field::Expression);
        }
        if (!receiver) return items;
        ResolvedType t = a.model.TypeOf(receiver);
        if (!t.ns.empty()) {
            for (const std::string &n : a.universe.ChildNamespaces(t.ns)) add(n, CsLspKind::Module, "namespace " + t.ns + "." + n, "", "1");
            for (const ResolvedType &ty : a.universe.TypesIn(t.ns)) {
                std::string name = ShowType(ParseTypeName(ty.FullName()));
                size_t lt = name.find('<');
                std::string label = lt == std::string::npos ? name : name.substr(0, lt);
                add(label, CompletionKindOf(ty.Kind()), ty.bcl ? ty.bcl->signature : (ty.source ? ty.source->signature : ""),
                    ty.bcl ? ty.bcl->doc : (ty.source ? ty.source->doc : ""), "1");
            }
            return items;
        }
        if (!t.Known()) return items;
        addMembers(a.model.MembersOf(t, "", receiver));
        if (t.is_static_ref) {
            // Nested types.
            std::string full = t.FullName();
            for (const ResolvedType &ty : a.universe.TypesIn(full)) {
                std::string name = ShowType(ParseTypeName(ty.FullName()));
                add(name.substr(0, name.find('<')), CompletionKindOf(ty.Kind()), "", "", "1");
            }
        }
        return items;
    }

    // Otherwise: what is in scope.
    const SyntaxNode *at = doc.tree->FindToken(std::max(0, pos - 1));
    for (size_t i = 0; i < a.binding.symbols.size(); i++) {
        const LocalSymbol &s = a.binding.symbols[i];
        if (s.kind == SymKind::Label) continue;
        if (!s.scope) continue;
        TextSpan sc = s.scope->FullSpan();
        if (pos < sc.start || pos > sc.end) continue;
        if (s.kind != SymKind::LocalFunction && s.name_token->Span().start >= pos) continue;
        CsLspKind kind = s.kind == SymKind::LocalFunction ? CsLspKind::Function : CsLspKind::Variable;
        add(s.name, kind, "(" + LocalKindName(s.kind) + ") " + LocalTypeText(a, static_cast<int>(i)) + " " + s.name, "", "0");
    }
    if (at) addMembers(a.model.EnclosingMembers(at, ""));
    Context ctx = ContextAt(doc, at ? at : doc.tree->Root());
    for (const ResolvedType &ty : a.universe.VisibleTypes(ctx.usings, ctx.ns)) {
        std::string name = ShowType(ParseTypeName(ty.FullName()));
        std::string label = name.substr(0, name.find('<'));
        add(label, CompletionKindOf(ty.Kind()), ty.bcl ? ty.bcl->signature : (ty.source ? ty.source->signature : ""),
            ty.bcl ? ty.bcl->doc : (ty.source ? ty.source->doc : ""), "3");
    }
    for (const std::string &n : a.universe.ChildNamespaces("")) add(n, CsLspKind::Module, "namespace " + n, "", "4");
    for (const std::string &k : Keywords()) add(k, CsLspKind::Keyword, "", "", "5");
    return items;
}

// --- Hover ---------------------------------------------------------------------------------------

namespace {

std::string TypeHover(const ResolvedType &t) {
    std::string sig = t.bcl ? t.bcl->signature : (t.source ? t.source->signature : "");
    std::string docText = t.bcl ? t.bcl->doc : (t.source ? t.source->doc : "");
    std::string full = t.bcl ? std::string(t.bcl->name) : (t.source ? t.source->FullName() : "");
    std::string out = Fence(sig.empty() ? full : sig);
    if (t.bcl) out += "\n\n`" + full + "`";
    if (!docText.empty()) out += "\n\n" + docText;
    return out;
}

std::string MembersHover(const std::vector<MemberInfo> &ms) {
    std::string code;
    size_t shown = 0;
    for (const MemberInfo &m : ms) {
        if (shown == 8) {
            code += "\n// +" + std::to_string(ms.size() - shown) + " more overloads";
            break;
        }
        if (!code.empty()) code += "\n";
        code += MemberDisplay(m);
        shown++;
    }
    std::string out = Fence(code);
    for (const MemberInfo &m : ms) {
        if (!m.doc.empty()) {
            out += "\n\n" + m.doc;
            break;
        }
    }
    return out;
}

/** @brief What a name token refers to, when it is not a local: members (via receiver or enclosing type), a type, a
 *         namespace. */
struct NameTarget {
    std::vector<MemberInfo> members;
    ResolvedType type;
    std::string ns;
    const SyntaxNode *decl = nullptr;  // a declaration in this file whose name this token is
};

NameTarget ResolveName(const Analysis &a, const SyntaxNode *tok) {
    NameTarget out;
    std::string name = Doc::TokenValue(tok);
    const SyntaxNode *p = tok->parent;
    // The token is a declaration's name?
    if (p && DeclNameToken(p) == tok) {
        out.decl = p;
        return out;
    }
    if (p && p->Kind() == SyntaxKind::VariableDeclarator && p->parent && p->parent->parent &&
        (p->parent->parent->Kind() == SyntaxKind::FieldDeclaration || p->parent->parent->Kind() == SyntaxKind::EventFieldDeclaration)) {
        out.decl = p->parent->parent;
        return out;
    }
    if (!p) return out;
    const SyntaxNode *nameNode = p;  // IdentifierName / GenericName
    const SyntaxNode *gp = nameNode->parent;
    if (gp && (gp->Kind() == SyntaxKind::SimpleMemberAccessExpression || gp->Kind() == SyntaxKind::QualifiedName) &&
        (gp->Field(Field::Name) == nameNode || gp->Field(Field::Right) == nameNode)) {
        const SyntaxNode *left = gp->Field(Field::Expression) ? gp->Field(Field::Expression) : gp->Field(Field::Left);
        ResolvedType lt = a.model.TypeOf(left);
        if (!lt.ns.empty()) {
            ResolvedType t = a.model.ResolveTypeText(lt.ns + "." + a.doc.Text(nameNode), gp);
            if (t.Known()) out.type = t;
            else if (a.universe.IsNamespace(lt.ns + "." + name)) out.ns = lt.ns + "." + name;
            return out;
        }
        out.members = a.model.MembersOf(lt, name, gp);
        if (out.members.empty() && lt.is_static_ref) {
            ResolvedType t = a.model.ResolveTypeText(a.doc.Text(gp), gp);
            if (t.Known()) out.type = t;
        }
        return out;
    }
    if (gp && gp->Kind() == SyntaxKind::MemberBindingExpression) {
        const SyntaxNode *ca = Ancestor(gp, {SyntaxKind::ConditionalAccessExpression});
        if (ca) out.members = a.model.MembersOf(a.model.TypeOf(ca->Field(Field::Expression)), name, gp);
        return out;
    }
    // A type position or an expression name.
    out.members = a.model.EnclosingMembers(nameNode, name);
    if (!out.members.empty()) return out;
    ResolvedType t = a.model.ResolveTypeText(a.doc.Text(nameNode), nameNode);
    if (!t.Known() && nameNode->parent && nameNode->parent->Kind() == SyntaxKind::Attribute) {
        t = a.model.ResolveTypeText(a.doc.Text(nameNode) + "Attribute", nameNode);
    }
    if (t.Known()) {
        out.type = t;
        return out;
    }
    if (a.universe.IsNamespace(name)) out.ns = name;
    return out;
}

}  // namespace

CsLspHoverInfo CsLspHover(const std::vector<std::string> &lines, int line, int col, const CsProject *project) {
    CsLspHoverInfo info;
    Analysis a(lines, project);
    const SyntaxNode *tok = a.doc.TokenAt(line, col);
    if (!tok || tok->IsMissing()) return info;
    info.range = a.doc.RangeOf(tok);
    SyntaxKind k = tok->Kind();
    if (csharp::SyntaxFacts::IsPredefinedType(k)) {
        ResolvedType t = a.model.ResolveTypeText(csharp::ToUtf8(tok->TokenText()), tok);
        if (t.Known()) {
            info.found = true;
            info.text = TypeHover(t);
        }
        return info;
    }
    if (k != SyntaxKind::IdentifierToken) return info;
    int sym = SymbolAt(a.binding, tok);
    if (sym >= 0) {
        const LocalSymbol &s = a.binding.symbols[static_cast<size_t>(sym)];
        info.found = true;
        if (s.kind == SymKind::LocalFunction) {
            info.text = Fence("(local function) " + LocalTypeText(a, sym));
            std::string d = DocSummary(a.doc, s.decl);
            if (!d.empty()) info.text += "\n\n" + d;
        } else if (s.kind == SymKind::Label) {
            info.text = Fence("(label) " + s.name);
        } else {
            info.text = Fence("(" + LocalKindName(s.kind) + ") " + LocalTypeText(a, sym) + " " + s.name);
        }
        return info;
    }
    NameTarget t = ResolveName(a, tok);
    if (t.decl) {
        info.found = true;
        info.text = Fence(DeclHeader(a.doc, t.decl));
        std::string d = DocSummary(a.doc, t.decl);
        if (!d.empty()) info.text += "\n\n" + d;
        return info;
    }
    if (!t.members.empty()) {
        info.found = true;
        info.text = MembersHover(t.members);
        return info;
    }
    if (t.type.Known()) {
        info.found = true;
        info.text = TypeHover(t.type);
        return info;
    }
    if (!t.ns.empty()) {
        info.found = true;
        info.text = Fence("namespace " + t.ns);
    }
    return info;
}

// --- Signature help ------------------------------------------------------------------------------------

namespace {

/** @brief The parameter substrings of a member's parameter list "(int a, string b = \"x\")". */
std::vector<std::string> SplitParams(const std::string &params) {
    std::vector<std::string> out;
    if (params.size() < 2) return out;
    std::string inner = params.substr(1, params.size() - 2);
    int depth = 0;
    bool inStr = false;
    std::string cur;
    for (char c : inner) {
        if (c == '"') inStr = !inStr;
        if (!inStr) {
            if (c == '<' || c == '(' || c == '[' || c == '{') depth++;
            if (c == '>' || c == ')' || c == ']' || c == '}') depth--;
            if (c == ',' && depth == 0) {
                size_t f = cur.find_first_not_of(' ');
                out.push_back(f == std::string::npos ? "" : cur.substr(f));
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    size_t f = cur.find_first_not_of(' ');
    if (f != std::string::npos) out.push_back(cur.substr(f));
    return out;
}

}  // namespace

CsLspSignatureHelp CsLspSignatures(const std::vector<std::string> &lines, int line, int col, const CsProject *project) {
    CsLspSignatureHelp help;
    Analysis a(lines, project);
    const int pos = a.doc.OffsetAt(line, col);
    const SyntaxNode *tok = a.doc.tree->FindToken(std::max(0, pos - 1));
    // The innermost argument list around the cursor.
    const SyntaxNode *list = nullptr;
    for (const SyntaxNode *n = tok; n; n = n->parent) {
        if (n->Kind() == SyntaxKind::ArgumentList || n->Kind() == SyntaxKind::BracketedArgumentList) {
            const SyntaxNode *open = n->Field(Field::OpenParenToken) ? n->Field(Field::OpenParenToken) : n->Field(Field::OpenBracketToken);
            const SyntaxNode *close = n->Field(Field::CloseParenToken) ? n->Field(Field::CloseParenToken) : n->Field(Field::CloseBracketToken);
            if (open && pos >= open->Span().end && (!close || close->IsMissing() || pos <= close->Span().start)) {
                list = n;
                break;
            }
        }
    }
    if (!list) return help;
    const SyntaxNode *call = list->parent;
    std::vector<MemberInfo> candidates;
    if (call->Kind() == SyntaxKind::InvocationExpression) {
        const SyntaxNode *target = call->Field(Field::Expression);
        if (target && target->Kind() == SyntaxKind::SimpleMemberAccessExpression) {
            const SyntaxNode *nm = target->Field(Field::Name);
            candidates = a.model.MembersOf(a.model.TypeOf(target->Field(Field::Expression)),
                                           Doc::TokenValue(nm ? nm->Field(Field::Identifier) : nullptr), target);
        } else if (target && (target->Kind() == SyntaxKind::IdentifierName || target->Kind() == SyntaxKind::GenericName)) {
            const SyntaxNode *id = target->Field(Field::Identifier);
            int sym = SymbolAt(a.binding, id);
            if (sym >= 0 && a.binding.symbols[static_cast<size_t>(sym)].kind == SymKind::LocalFunction) {
                const LocalSymbol &s = a.binding.symbols[static_cast<size_t>(sym)];
                MemberInfo mi;
                mi.name = s.name;
                mi.kind = "method";
                mi.signature = DeclHeader(a.doc, s.decl);
                const SyntaxNode *pl = s.decl->Field(Field::ParameterList);
                mi.params = pl ? a.doc.Text(pl) : "()";
                candidates.push_back(mi);
            } else {
                candidates = a.model.EnclosingMembers(target, Doc::TokenValue(id));
            }
        }
    } else if (call->Kind() == SyntaxKind::ObjectCreationExpression) {
        ResolvedType t = a.model.ResolveTypeNode(call->Field(Field::Type));
        for (MemberInfo &m : a.universe.Members(t, "", false)) {
            if (m.kind == "constructor") candidates.push_back(m);
        }
    } else if (call->Kind() == SyntaxKind::ElementAccessExpression) {
        for (MemberInfo &m : a.universe.Members(a.model.TypeOf(call->Field(Field::Expression)), "")) {
            if (m.kind == "indexer") candidates.push_back(m);
        }
    }
    // Methods only (a property of the same name is not a call).
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](const MemberInfo &m) {
                                        return m.kind != "method" && m.kind != "constructor" && m.kind != "indexer";
                                    }),
                     candidates.end());
    if (candidates.empty()) return help;
    // The active argument: separators before the cursor.
    int active = 0;
    for (const SyntaxNode *c : list->FieldItems(Field::Arguments)) {
        if (c->IsToken() && c->Kind() == SyntaxKind::CommaToken && c->Span().end <= pos) active++;
    }
    int argCount = 0;
    for (const SyntaxNode *c : list->FieldItems(Field::Arguments)) {
        if (!c->IsToken()) argCount++;
    }
    help.found = true;
    help.active_param = active;
    for (size_t i = 0; i < candidates.size(); i++) {
        const MemberInfo &m = candidates[i];
        CsLspSignature sig;
        std::string params = m.params;
        std::vector<std::string> ps = SplitParams(params);
        if (m.is_extension && !ps.empty() && (m.bcl || ps.front().rfind("this ", 0) == 0)) ps.erase(ps.begin());
        std::string head = m.kind == "constructor" ? m.name : (ShowType(m.type) + " " + m.name);
        std::string label = head + "(";
        for (size_t k = 0; k < ps.size(); k++) {
            if (k) label += ", ";
            label += ps[k];
            sig.params.push_back(ps[k]);
        }
        label += ")";
        sig.label = label;
        sig.documentation = m.doc;
        if (static_cast<int>(ps.size()) >= std::max(argCount, active + 1) && help.active_signature == 0 &&
            (i == 0 || static_cast<int>(SplitParams(candidates[static_cast<size_t>(help.active_signature)].params).size()) <
                           std::max(argCount, active + 1))) {
            help.active_signature = static_cast<int>(i);
        }
        help.signatures.push_back(sig);
    }
    return help;
}

// --- Symbols ---------------------------------------------------------------------------------------------

namespace {

CsLspSymbolKind SymbolKindOf(SyntaxKind k) {
    switch (k) {
    case SyntaxKind::NamespaceDeclaration:
    case SyntaxKind::FileScopedNamespaceDeclaration:
        return CsLspSymbolKind::Namespace;
    case SyntaxKind::InterfaceDeclaration:
        return CsLspSymbolKind::Interface;
    case SyntaxKind::EnumDeclaration:
        return CsLspSymbolKind::Enum;
    case SyntaxKind::StructDeclaration:
    case SyntaxKind::RecordStructDeclaration:
        return CsLspSymbolKind::Struct;
    case SyntaxKind::MethodDeclaration:
        return CsLspSymbolKind::Method;
    case SyntaxKind::LocalFunctionStatement:
        return CsLspSymbolKind::Function;
    case SyntaxKind::ConstructorDeclaration:
    case SyntaxKind::DestructorDeclaration:
        return CsLspSymbolKind::Constructor;
    case SyntaxKind::PropertyDeclaration:
    case SyntaxKind::IndexerDeclaration:
        return CsLspSymbolKind::Property;
    case SyntaxKind::FieldDeclaration:
        return CsLspSymbolKind::Field;
    case SyntaxKind::EventDeclaration:
    case SyntaxKind::EventFieldDeclaration:
        return CsLspSymbolKind::Event;
    case SyntaxKind::EnumMemberDeclaration:
        return CsLspSymbolKind::EnumMember;
    case SyntaxKind::OperatorDeclaration:
    case SyntaxKind::ConversionOperatorDeclaration:
        return CsLspSymbolKind::Operator;
    case SyntaxKind::DelegateDeclaration:
        return CsLspSymbolKind::Function;
    default:
        return CsLspSymbolKind::Class;
    }
}

}  // namespace

std::vector<CsLspSymbol> CsLspSymbols(const std::vector<std::string> &lines) {
    std::vector<CsLspSymbol> out;
    Doc doc(lines);
    std::function<void(const SyntaxNode *, int)> walk = [&](const SyntaxNode *n, int parent) {
        if (n->IsToken()) return;
        SyntaxKind k = n->Kind();
        int self = parent;
        auto push = [&](const std::string &name, const std::string &detail, CsLspSymbolKind kind, const SyntaxNode *sel) {
            CsLspSymbol s;
            s.name = name;
            s.detail = detail;
            s.kind = kind;
            s.range = doc.RangeOf(n);
            s.sel_range = sel ? doc.RangeOf(sel) : s.range;
            s.parent = parent;
            out.push_back(s);
            return static_cast<int>(out.size() - 1);
        };
        switch (k) {
        case SyntaxKind::NamespaceDeclaration:
        case SyntaxKind::FileScopedNamespaceDeclaration: {
            std::string name = doc.Text(n->Field(Field::Name));
            self = push(name, "", CsLspSymbolKind::Namespace, n->Field(Field::Name));
            break;
        }
        case SyntaxKind::FieldDeclaration:
        case SyntaxKind::EventFieldDeclaration: {
            const SyntaxNode *vd = n->Field(Field::Declaration);
            std::string type = vd ? doc.Text(vd->Field(Field::Type)) : "";
            bool isConst = HasModifier(n, SyntaxKind::ConstKeyword);
            for (const SyntaxNode *v : vd ? vd->FieldItems(Field::Variables) : std::vector<const SyntaxNode *>{}) {
                if (v->Kind() != SyntaxKind::VariableDeclarator) continue;
                const SyntaxNode *id = v->Field(Field::Identifier);
                push(Doc::TokenValue(id), type, isConst ? CsLspSymbolKind::Constant : SymbolKindOf(k), id);
            }
            return;
        }
        case SyntaxKind::MethodDeclaration:
        case SyntaxKind::LocalFunctionStatement: {
            const SyntaxNode *id = n->Field(Field::Identifier);
            std::string params = doc.Text(n->Field(Field::ParameterList));
            std::string tps = doc.Text(n->Field(Field::TypeParameterList));
            self = push(Doc::TokenValue(id) + tps + params, doc.Text(n->Field(Field::ReturnType)), SymbolKindOf(k), id);
            break;
        }
        case SyntaxKind::ConstructorDeclaration:
        case SyntaxKind::DestructorDeclaration: {
            const SyntaxNode *id = n->Field(Field::Identifier);
            std::string name = (k == SyntaxKind::DestructorDeclaration ? "~" : "") + Doc::TokenValue(id) +
                               doc.Text(n->Field(Field::ParameterList));
            self = push(name, "", SymbolKindOf(k), id);
            break;
        }
        case SyntaxKind::PropertyDeclaration:
        case SyntaxKind::EventDeclaration:
        case SyntaxKind::EnumMemberDeclaration: {
            const SyntaxNode *id = n->Field(Field::Identifier);
            push(Doc::TokenValue(id), doc.Text(n->Field(Field::Type)), SymbolKindOf(k), id);
            return;
        }
        case SyntaxKind::IndexerDeclaration:
            push("this" + doc.Text(n->Field(Field::ParameterList)), doc.Text(n->Field(Field::Type)), SymbolKindOf(k),
                 n->Field(Field::ThisKeyword));
            return;
        case SyntaxKind::OperatorDeclaration:
            self = push("operator " + doc.Text(n->Field(Field::OperatorToken)) + doc.Text(n->Field(Field::ParameterList)),
                        doc.Text(n->Field(Field::ReturnType)), SymbolKindOf(k), n->Field(Field::OperatorToken));
            break;
        case SyntaxKind::ConversionOperatorDeclaration:
            self = push(doc.Text(n->Field(Field::ImplicitOrExplicitKeyword)) + " operator " + doc.Text(n->Field(Field::Type)),
                        "", SymbolKindOf(k), n->Field(Field::Type));
            break;
        default:
            if (IsTypeDeclKind(k) && k != SyntaxKind::ExtensionBlockDeclaration) {
                const SyntaxNode *id = n->Field(Field::Identifier);
                std::string tps = doc.Text(n->Field(Field::TypeParameterList));
                std::string kw = doc.Text(n->Field(Field::Keyword));
                if (k == SyntaxKind::RecordStructDeclaration) kw = "record struct";
                self = push(Doc::TokenValue(id) + tps, kw, SymbolKindOf(k), id);
            } else if (k == SyntaxKind::ExtensionBlockDeclaration) {
                self = push("extension" + doc.Text(n->Field(Field::ParameterList)), "", CsLspSymbolKind::Class, n->FirstToken());
            }
            break;
        }
        for (const SyntaxNode *c : n->children) {
            // Only declarations and the bodies that may hold local functions.
            if (c->IsToken()) continue;
            walk(c, self);
        }
    };
    walk(doc.tree->Root(), -1);
    return out;
}

// --- Folds -------------------------------------------------------------------------------------------------

std::vector<CsLspFold> CsLspFolds(const std::vector<std::string> &lines) {
    std::vector<CsLspFold> out;
    Doc doc(lines);
    std::set<std::pair<int, int>> seen;
    auto addFold = [&](int s, int e, const std::string &kind) {
        if (e > s && seen.insert({s, e}).second) out.push_back(CsLspFold{s, e, kind});
    };
    auto lineOf = [&](int pos) {
        int l = 0, c = 0;
        doc.tree->LineCol(pos, &l, &c);
        return l;
    };
    // Comments and directives, from the trivia of every token.
    std::vector<std::pair<int, std::string>> regions;  // #region start lines
    int commentStart = -1, commentEnd = -1;
    auto flushComments = [&]() {
        if (commentStart >= 0 && commentEnd > commentStart) addFold(commentStart, commentEnd, "comment");
        commentStart = commentEnd = -1;
    };
    std::function<void(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->IsToken()) {
            auto trivia = [&](const SyntaxTrivia &t) {
                SyntaxKind k = t.Kind();
                TextSpan s = t.Span();
                if (k == SyntaxKind::SingleLineCommentTrivia) {
                    int l = lineOf(s.start);
                    if (commentStart >= 0 && l == commentEnd + 1) {
                        commentEnd = l;
                    } else {
                        flushComments();
                        commentStart = commentEnd = l;
                    }
                    return;
                }
                if (k == SyntaxKind::WhitespaceTrivia || k == SyntaxKind::EndOfLineTrivia) return;
                flushComments();
                if (k == SyntaxKind::MultiLineCommentTrivia || k == SyntaxKind::SingleLineDocumentationCommentTrivia ||
                    k == SyntaxKind::MultiLineDocumentationCommentTrivia) {
                    int e = lineOf(s.end);
                    if (k == SyntaxKind::SingleLineDocumentationCommentTrivia && e > 0) e = lineOf(std::max(s.start, s.end - 1));
                    addFold(lineOf(s.start), e, "comment");
                } else if (k == SyntaxKind::RegionDirectiveTrivia) {
                    regions.push_back({lineOf(s.start), ""});
                } else if (k == SyntaxKind::EndRegionDirectiveTrivia && !regions.empty()) {
                    addFold(regions.back().first, lineOf(s.start), "region");
                    regions.pop_back();
                }
            };
            for (const SyntaxTrivia &t : n->leading) trivia(t);
            if (!n->IsMissing() && n->green->full_width > 0) flushComments();
            for (const SyntaxTrivia &t : n->trailing) trivia(t);
            return;
        }
        SyntaxKind k = n->Kind();
        bool foldable = IsTypeDeclKind(k);
        switch (k) {
        case SyntaxKind::NamespaceDeclaration:
        case SyntaxKind::MethodDeclaration:
        case SyntaxKind::ConstructorDeclaration:
        case SyntaxKind::DestructorDeclaration:
        case SyntaxKind::OperatorDeclaration:
        case SyntaxKind::ConversionOperatorDeclaration:
        case SyntaxKind::PropertyDeclaration:
        case SyntaxKind::IndexerDeclaration:
        case SyntaxKind::EventDeclaration:
        case SyntaxKind::LocalFunctionStatement:
        case SyntaxKind::Block:
        case SyntaxKind::AccessorList:
        case SyntaxKind::SwitchStatement:
        case SyntaxKind::SwitchExpression:
        case SyntaxKind::ObjectInitializerExpression:
        case SyntaxKind::CollectionInitializerExpression:
        case SyntaxKind::ArrayInitializerExpression:
        case SyntaxKind::WithInitializerExpression:
        case SyntaxKind::AnonymousObjectCreationExpression:
        case SyntaxKind::CollectionExpression:
        case SyntaxKind::ArgumentList:
        case SyntaxKind::ParameterList:
        case SyntaxKind::InterpolatedStringExpression:
        case SyntaxKind::SwitchSection:
            foldable = true;
            break;
        default:
            break;
        }
        if (foldable) {
            TextSpan s = n->Span();
            int sl = lineOf(s.start), el = lineOf(s.end);
            // A block's opening brace on its own line folds from the line above (the statement's header).
            if (k == SyntaxKind::Block && n->parent && n->parent->Kind() != SyntaxKind::Block) {
                int pl = lineOf(n->parent->Span().start);
                if (pl < sl) sl = pl;
            }
            // Leave the closing brace / paren line visible.
            if (el > sl && el < static_cast<int>(lines.size())) {
                const std::string &e = lines[static_cast<size_t>(el)];
                size_t f = e.find_first_not_of(" \t");
                if (f != std::string::npos && (e[f] == '}' || e[f] == ')' || e[f] == ']')) el--;
            }
            addFold(sl, el, "");
        }
        if (k == SyntaxKind::CompilationUnit || k == SyntaxKind::NamespaceDeclaration ||
            k == SyntaxKind::FileScopedNamespaceDeclaration) {
            std::vector<const SyntaxNode *> us = n->FieldItems(Field::Usings);
            if (us.size() >= 2) addFold(lineOf(us.front()->Span().start), lineOf(us.back()->Span().end), "imports");
        }
        for (const SyntaxNode *c : n->children) walk(c);
    };
    walk(doc.tree->Root());
    flushComments();
    std::sort(out.begin(), out.end(), [](const CsLspFold &a, const CsLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}

// --- Navigation --------------------------------------------------------------------------------------------

CsLspLocation CsLspDefinition(const std::vector<std::string> &lines, int line, int col, const CsProject *project) {
    CsLspLocation loc;
    Analysis a(lines, project);
    const SyntaxNode *tok = a.doc.TokenAt(line, col);
    if (!tok || tok->Kind() != SyntaxKind::IdentifierToken) return loc;
    int sym = SymbolAt(a.binding, tok);
    if (sym >= 0) {
        loc.found = true;
        loc.range = a.doc.RangeOf(a.binding.symbols[static_cast<size_t>(sym)].name_token);
        return loc;
    }
    NameTarget t = ResolveName(a, tok);
    if (t.decl) {
        loc.found = true;
        loc.range = a.doc.RangeOf(tok);
        return loc;
    }
    for (const MemberInfo &m : t.members) {
        if (m.source) {
            loc.found = true;
            loc.path = m.source->path;
            loc.range = m.source->range;
            return loc;
        }
    }
    if (t.type.source) {
        loc.found = true;
        loc.path = t.type.source->path;
        loc.range = t.type.source->range;
    }
    return loc;
}

namespace {

/** @brief The tokens a rename / references request covers: a local's declaration and uses, or every identifier in
 *         the file spelling a name the file declares (a type or member). */
std::vector<CsLspReference> Occurrences(const Analysis &a, const SyntaxNode *tok, bool *found, CsRange *target) {
    std::vector<CsLspReference> out;
    *found = false;
    if (!tok || tok->Kind() != SyntaxKind::IdentifierToken) return out;
    int sym = SymbolAt(a.binding, tok);
    if (sym >= 0) {
        const LocalSymbol &s = a.binding.symbols[static_cast<size_t>(sym)];
        *found = true;
        *target = a.doc.RangeOf(tok);
        out.push_back(CsLspReference{a.doc.RangeOf(s.name_token), true, false});
        for (size_t i = 0; i < s.refs.size(); i++) {
            out.push_back(CsLspReference{a.doc.RangeOf(s.refs[i]), false, static_cast<bool>(s.ref_is_write[i])});
        }
        return out;
    }
    std::string name = Doc::TokenValue(tok);
    bool declared = false;
    for (const SourceType &t : a.current.types) {
        if (t.name == name) declared = true;
    }
    for (const SourceMember &m : a.current.members) {
        if (m.name == name) declared = true;
    }
    if (!declared) return out;
    *found = true;
    *target = a.doc.RangeOf(tok);
    std::function<void(const SyntaxNode *)> walk = [&](const SyntaxNode *n) {
        if (n->IsToken()) {
            if (n->Kind() == SyntaxKind::IdentifierToken && Doc::TokenValue(n) == name && SymbolAt(a.binding, n) < 0) {
                bool isDecl = n->parent && (DeclNameToken(n->parent) == n ||
                                            (n->parent->Kind() == SyntaxKind::VariableDeclarator &&
                                             n->parent->Field(Field::Identifier) == n));
                out.push_back(CsLspReference{a.doc.RangeOf(n), isDecl, false});
            }
            return;
        }
        for (const SyntaxNode *c : n->children) walk(c);
    };
    walk(a.doc.tree->Root());
    return out;
}

}  // namespace

std::vector<CsLspReference> CsLspReferences(const std::vector<std::string> &lines, int line, int col) {
    Analysis a(lines, nullptr);
    bool found = false;
    CsRange target;
    return Occurrences(a, a.doc.TokenAt(line, col), &found, &target);
}

CsLspRenameTarget CsLspPrepareRename(const std::vector<std::string> &lines, int line, int col) {
    CsLspRenameTarget t;
    Analysis a(lines, nullptr);
    const SyntaxNode *tok = a.doc.TokenAt(line, col);
    bool found = false;
    CsRange target;
    Occurrences(a, tok, &found, &target);
    if (!found) return t;
    t.found = true;
    t.range = target;
    t.placeholder = Doc::TokenValue(tok);
    return t;
}

std::vector<CsLspTextEdit> CsLspRename(const std::vector<std::string> &lines, int line, int col,
                                       const std::string &new_name, std::string *error) {
    std::vector<CsLspTextEdit> edits;
    std::u16string n16 = csharp::ToUtf16(new_name.size() && new_name[0] == '@' ? new_name.substr(1) : new_name);
    if (!csharp::SyntaxFacts::IsValidIdentifier(csharp::Text(n16)) ||
        (new_name[0] != '@' && csharp::SyntaxFacts::GetKeywordKind(csharp::Text(n16)) != SyntaxKind::None)) {
        *error = "'" + new_name + "' is not a valid C# identifier";
        return edits;
    }
    Analysis a(lines, nullptr);
    bool found = false;
    CsRange target;
    std::vector<CsLspReference> refs = Occurrences(a, a.doc.TokenAt(line, col), &found, &target);
    if (!found) {
        *error = "Nothing to rename here";
        return edits;
    }
    for (const CsLspReference &r : refs) edits.push_back(CsLspTextEdit{r.range, new_name});
    return edits;
}

std::vector<CsLspTextEdit> CsLspFormat(const std::vector<std::string> &lines) {
    // The formatter `gf` runs (csharp_format.h), as one edit over the lines that changed.
    std::vector<CsLspTextEdit> out;
    std::string text;
    for (size_t k = 0; k < lines.size(); k++) text += lines[k] + (k + 1 < lines.size() ? "\n" : "");
    const csfmt::Result r = csfmt::Format(text);
    if (!r.ok || r.text == text) return out;
    std::vector<std::string> got;
    std::string cur;
    for (char c : r.text) {
        if (c == '\n') {
            got.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    got.push_back(cur);
    size_t head = 0;
    while (head < lines.size() && head < got.size() && lines[head] == got[head]) head++;
    size_t tail = 0;
    while (tail < lines.size() - head && tail < got.size() - head &&
           lines[lines.size() - 1 - tail] == got[got.size() - 1 - tail]) {
        tail++;
    }
    std::string repl;
    for (size_t k = head; k < got.size() - tail; k++) repl += got[k] + "\n";
    CsLspTextEdit e;
    e.range.start_line = static_cast<int>(head);
    e.range.end_line = static_cast<int>(lines.size() - tail);
    e.new_text = repl;
    out.push_back(e);
    return out;
}
