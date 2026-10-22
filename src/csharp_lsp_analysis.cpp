// The C# language server's model of a file: the document, binding of
// locals, the file's declarations and the context of a position. See
// csharp_lsp_analysis.h.
#include "csharp_lsp_analysis.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>

namespace cslsp {

using csharp::GreenNode;
using csharp::SyntaxTrivia;

// --- Doc ---------------------------------------------------------------------------------

Doc::Doc(const std::vector<std::string> &lines_) : lines(lines_) {
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += '\n';
        text += lines[i];
    }
    tree = SyntaxTree::Parse(text);
}

CsRange Doc::RangeOf(TextSpan span) const {
    CsRange r;
    tree->LineCol(span.start, &r.start_line, &r.start_col);
    tree->LineCol(span.end, &r.end_line, &r.end_col);
    return r;
}

std::string Doc::TokenValue(const SyntaxNode *token) {
    if (!token) return "";
    if (token->Kind() == SyntaxKind::IdentifierToken) return csharp::ToUtf8(token->ValueText());
    return csharp::ToUtf8(token->TokenText());
}

const SyntaxNode *Doc::TokenAt(int line, int col) const {
    int pos = OffsetAt(line, col);
    const SyntaxNode *t = tree->FindToken(pos);
    auto wordy = [](const SyntaxNode *tok) {
        if (!tok) return false;
        SyntaxKind k = tok->Kind();
        return k == SyntaxKind::IdentifierToken || csharp::SyntaxFacts::IsKeywordKind(k) ||
               csharp::SyntaxFacts::IsLiteral(k);
    };
    if (t && t->Span().Contains(pos) && wordy(t)) return t;
    const SyntaxNode *left = tree->FindTokenOnLeft(pos);
    if (left && left->Span().end == pos && wordy(left)) return left;
    if (t && t->Span().Contains(pos)) return t;
    return left && left->Span().IntersectsOrTouches(pos) ? left : t;
}

// --- Tree helpers --------------------------------------------------------------------------

const SyntaxNode *Ancestor(const SyntaxNode *n, std::initializer_list<SyntaxKind> kinds) {
    for (; n; n = n->parent ? n->parent : (n->parent_trivia ? n->parent_trivia->token : nullptr)) {
        for (SyntaxKind k : kinds) {
            if (n->Kind() == k) return n;
        }
    }
    return nullptr;
}

bool IsTypeDeclKind(SyntaxKind k) {
    switch (k) {
    case SyntaxKind::ClassDeclaration:
    case SyntaxKind::StructDeclaration:
    case SyntaxKind::InterfaceDeclaration:
    case SyntaxKind::RecordDeclaration:
    case SyntaxKind::RecordStructDeclaration:
    case SyntaxKind::EnumDeclaration:
    case SyntaxKind::DelegateDeclaration:
    case SyntaxKind::ExtensionBlockDeclaration:
        return true;
    default:
        return false;
    }
}

bool HasModifier(const SyntaxNode *decl, SyntaxKind kind) {
    if (!decl) return false;
    for (const SyntaxNode *m : decl->FieldItems(Field::Modifiers)) {
        if (m->Kind() == kind) return true;
    }
    return false;
}

const SyntaxNode *DeclNameToken(const SyntaxNode *decl) {
    if (!decl) return nullptr;
    switch (decl->Kind()) {
    case SyntaxKind::FieldDeclaration:
    case SyntaxKind::EventFieldDeclaration: {
        const SyntaxNode *vd = decl->Field(Field::Declaration);
        std::vector<const SyntaxNode *> vars = vd ? vd->FieldItems(Field::Variables) : std::vector<const SyntaxNode *>{};
        return vars.empty() ? nullptr : vars.front()->Field(Field::Identifier);
    }
    case SyntaxKind::ConstructorDeclaration:
    case SyntaxKind::DestructorDeclaration:
    case SyntaxKind::MethodDeclaration:
    case SyntaxKind::PropertyDeclaration:
    case SyntaxKind::EventDeclaration:
    case SyntaxKind::EnumMemberDeclaration:
    case SyntaxKind::LocalFunctionStatement:
    case SyntaxKind::VariableDeclarator:
    case SyntaxKind::Parameter:
    case SyntaxKind::TypeParameter:
        return decl->Field(Field::Identifier);
    case SyntaxKind::IndexerDeclaration:
        return decl->Field(Field::ThisKeyword);
    case SyntaxKind::OperatorDeclaration:
        return decl->Field(Field::OperatorToken);
    case SyntaxKind::ConversionOperatorDeclaration:
        return decl->Field(Field::Type) ? decl->Field(Field::Type)->FirstToken() : nullptr;
    case SyntaxKind::NamespaceDeclaration:
    case SyntaxKind::FileScopedNamespaceDeclaration: {
        const SyntaxNode *name = decl->Field(Field::Name);
        return name ? name->LastToken() : nullptr;
    }
    default:
        if (IsTypeDeclKind(decl->Kind())) return decl->Field(Field::Identifier);
        return nullptr;
    }
}

namespace {

/** @brief The text of a doc comment's XML element children, plain. */
void XmlPlain(const Doc &doc, const SyntaxNode *n, std::string &out) {
    if (n->IsToken()) {
        if (n->Kind() == SyntaxKind::XmlTextLiteralToken || n->Kind() == SyntaxKind::XmlEntityLiteralToken) {
            out += csharp::ToUtf8(n->ValueText());
        } else if (n->Kind() == SyntaxKind::XmlTextLiteralNewLineToken) {
            out += ' ';
        }
        return;
    }
    if (n->Kind() == SyntaxKind::XmlEmptyElement) {
        // <see cref="X"/>, <paramref name="x"/>, <see langword="null"/>
        for (const SyntaxNode *attr : n->FieldItems(Field::Attributes)) {
            if (attr->Kind() == SyntaxKind::XmlCrefAttribute) {
                const SyntaxNode *cref = attr->Field(Field::Cref);
                out += "`" + doc.Text(cref) + "`";
            } else if (attr->Kind() == SyntaxKind::XmlNameAttribute) {
                const SyntaxNode *id = attr->Field(Field::Identifier);
                out += "`" + doc.Text(id) + "`";
            } else if (attr->Kind() == SyntaxKind::XmlTextAttribute) {
                std::string v;
                for (const SyntaxNode *t : attr->FieldItems(Field::TextTokens)) v += csharp::ToUtf8(t->ValueText());
                out += "`" + v + "`";
            }
        }
        return;
    }
    for (const SyntaxNode *c : n->children) XmlPlain(doc, c, out);
}

std::string CollapseSpaces(const std::string &s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

}  // namespace

std::string DocSummary(const Doc &doc, const SyntaxNode *decl) {
    if (!decl) return "";
    const SyntaxNode *first = decl->FirstToken();
    if (!first) return "";
    std::string summary, whole;
    for (const SyntaxTrivia &t : first->leading) {
        if (!t.structure) continue;
        if (t.Kind() != SyntaxKind::SingleLineDocumentationCommentTrivia &&
            t.Kind() != SyntaxKind::MultiLineDocumentationCommentTrivia) {
            continue;
        }
        for (const SyntaxNode *c : t.structure->children) {
            if (c->Kind() != SyntaxKind::XmlElement) continue;
            const SyntaxNode *start = c->Field(Field::StartTag);
            const SyntaxNode *name = start ? start->Field(Field::Name) : nullptr;
            std::string tag = name ? doc.Text(name) : "";
            if (tag == "summary") {
                std::string s;
                for (const SyntaxNode *k : c->FieldItems(Field::Content)) XmlPlain(doc, k, s);
                summary += CollapseSpaces(s);
            }
        }
    }
    return summary;
}

std::string DeclHeader(const Doc &doc, const SyntaxNode *decl) {
    if (!decl) return "";
    // From after the attributes to before the body / accessors / initializer / members.
    int start = decl->Span().start;
    for (const SyntaxNode *c : decl->children) {
        if (c->Kind() == SyntaxKind::AttributeList) {
            start = std::max(start, c->Span().end);
        }
    }
    const SyntaxNode *first_after_attrs = nullptr;
    for (const SyntaxNode *c : decl->children) {
        if (c->Kind() != SyntaxKind::AttributeList) {
            first_after_attrs = c;
            break;
        }
    }
    if (first_after_attrs) start = first_after_attrs->Span().start;
    int end = decl->Span().end;
    for (const SyntaxNode *c : decl->children) {
        switch (c->Kind()) {
        case SyntaxKind::Block:
        case SyntaxKind::ArrowExpressionClause:
        case SyntaxKind::AccessorList:
        case SyntaxKind::EqualsValueClause:
        case SyntaxKind::OpenBraceToken:
            end = std::min(end, c->Span().start);
            break;
        default:
            break;
        }
        if (c->Kind() == SyntaxKind::SemicolonToken && c->Span().start < end) end = c->Span().start;
    }
    if (decl->Kind() == SyntaxKind::FieldDeclaration || decl->Kind() == SyntaxKind::EventFieldDeclaration) {
        // "private int x" -- up to the first declarator's name.
        const SyntaxNode *name = DeclNameToken(decl);
        if (name) end = name->Span().end;
    }
    if (end < start) end = start;
    std::string s = doc.Text(TextSpan{start, end});
    return CollapseSpaces(s);
}

// --- Binding -----------------------------------------------------------------------------------

namespace {

bool IsTypePosition(const SyntaxNode *name) {
    const SyntaxNode *p = name->parent;
    if (!p) return false;
    switch (p->Kind()) {
    case SyntaxKind::TypeArgumentList:
    case SyntaxKind::SimpleBaseType:
    case SyntaxKind::PrimaryConstructorBaseType:
    case SyntaxKind::TypeConstraint:
    case SyntaxKind::ArrayType:
    case SyntaxKind::NullableType:
    case SyntaxKind::PointerType:
    case SyntaxKind::RefType:
    case SyntaxKind::ScopedType:
    case SyntaxKind::TupleElement:
    case SyntaxKind::QualifiedName:
    case SyntaxKind::AliasQualifiedName:
    case SyntaxKind::ExplicitInterfaceSpecifier:
    case SyntaxKind::TypeOfExpression:
    case SyntaxKind::SizeOfExpression:
    case SyntaxKind::DefaultExpression:
    case SyntaxKind::UsingDirective:
    case SyntaxKind::NameMemberCref:
    case SyntaxKind::QualifiedCref:
    case SyntaxKind::Attribute:
        return true;
    default:
        break;
    }
    for (Field f : {Field::Type, Field::ReturnType, Field::ElementType}) {
        if (p->Field(f) == name) {
            // `x is T` / `x as T` keep their right side as a type; a variable declaration's `var x` too.
            return true;
        }
    }
    if (p->Kind() == SyntaxKind::IsExpression || p->Kind() == SyntaxKind::AsExpression) {
        return p->Field(Field::Right) == name;
    }
    return false;
}

/** @brief Whether an IdentifierName names a member (x.Name, ?.Name, Name: arg, new T { Name = }) rather than a
 *         variable. */
bool IsMemberNamePosition(const SyntaxNode *name) {
    const SyntaxNode *p = name->parent;
    if (!p) return false;
    switch (p->Kind()) {
    case SyntaxKind::SimpleMemberAccessExpression:
    case SyntaxKind::PointerMemberAccessExpression:
        return p->Field(Field::Name) == name;
    case SyntaxKind::MemberBindingExpression:
    case SyntaxKind::NameColon:
    case SyntaxKind::NameEquals:
        return true;
    case SyntaxKind::SimpleAssignmentExpression: {
        const SyntaxNode *init = p->parent;
        if (p->Field(Field::Left) == name && init &&
            (init->Kind() == SyntaxKind::ObjectInitializerExpression || init->Kind() == SyntaxKind::WithInitializerExpression)) {
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

/** @brief Whether a reference is only written (x = ..., out x, a deconstruction target). */
bool IsPureWrite(const SyntaxNode *name) {
    const SyntaxNode *n = name;
    const SyntaxNode *p = n->parent;
    // Parentheses around the target: (x) = 1.
    while (p && p->Kind() == SyntaxKind::ParenthesizedExpression) {
        n = p;
        p = p->parent;
    }
    if (!p) return false;
    if (p->Kind() == SyntaxKind::SimpleAssignmentExpression && p->Field(Field::Left) == n) return true;
    if (p->Kind() == SyntaxKind::Argument) {
        // An `out x` argument counts as a use (Roslyn reports no CS0219 for it).
        // (x, y) = ...: an argument of a tuple on an assignment's left.
        const SyntaxNode *tuple = p->parent;
        if (tuple && tuple->Kind() == SyntaxKind::TupleExpression) {
            const SyntaxNode *t = tuple;
            while (t->parent && t->parent->Kind() == SyntaxKind::Argument && t->parent->parent &&
                   t->parent->parent->Kind() == SyntaxKind::TupleExpression) {
                t = t->parent->parent;
            }
            const SyntaxNode *a = t->parent;
            if (a && a->Kind() == SyntaxKind::SimpleAssignmentExpression && a->Field(Field::Left) == t) return true;
        }
    }
    return false;
}

class Binder {
public:
    Binder(const Doc &doc, Binding &b) : doc_(doc), b_(b) {}

    void Run() {
        Push(doc_.tree->Root());
        PredeclareStatements(doc_.tree->Root());
        Visit(doc_.tree->Root());
        Pop();
    }

private:
    const Doc &doc_;
    Binding &b_;
    struct Scope {
        const SyntaxNode *node;
        std::map<std::string, int> names;
    };
    std::vector<Scope> scopes_;
    std::vector<std::map<std::string, int>> labels_;  // per member body

    void Push(const SyntaxNode *n) { scopes_.push_back(Scope{n, {}}); }
    void Pop() { scopes_.pop_back(); }

    int Declare(SymKind kind, const SyntaxNode *name_token, const SyntaxNode *decl, const SyntaxNode *type,
                const SyntaxNode *init) {
        if (!name_token || name_token->IsMissing()) return -1;
        std::string name = Doc::TokenValue(name_token);
        if (name.empty() || name == "_") return -1;  // a discard
        if (b_.by_token.count(name_token)) return b_.by_token[name_token];
        LocalSymbol s;
        s.kind = kind;
        s.name = name;
        s.name_token = name_token;
        s.decl = decl;
        s.type = type;
        s.init = init;
        s.scope = scopes_.empty() ? nullptr : scopes_.back().node;
        if (init) s.writes.push_back(init);
        int index = static_cast<int>(b_.symbols.size());
        b_.symbols.push_back(std::move(s));
        b_.by_token[name_token] = index;
        if (!scopes_.empty()) scopes_.back().names[name] = index;
        return index;
    }

    int Lookup(const std::string &name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->names.find(name);
            if (f != it->names.end()) return f->second;
        }
        return -1;
    }

    void Reference(const SyntaxNode *token, int symbol, bool write, const SyntaxNode *assigned) {
        if (symbol < 0) return;
        LocalSymbol &s = b_.symbols[static_cast<size_t>(symbol)];
        s.refs.push_back(token);
        s.ref_is_write.push_back(write);
        if (write && assigned) s.writes.push_back(assigned);
        b_.by_token[token] = symbol;
    }

    /** @brief Local functions and labels are visible before their declaration: declare them on entry. */
    void PredeclareStatements(const SyntaxNode *container) {
        for (const SyntaxNode *c : container->children) {
            const SyntaxNode *st = c;
            if (st->Kind() == SyntaxKind::GlobalStatement) st = st->Field(Field::Statement);
            if (!st) continue;
            if (st->Kind() == SyntaxKind::LocalFunctionStatement) {
                Declare(SymKind::LocalFunction, st->Field(Field::Identifier), st, st->Field(Field::ReturnType), nullptr);
            }
        }
    }

    /** @brief Labels are visible in their block (and the blocks inside it): declare a block's on entry. */
    void DeclareLabels(const SyntaxNode *block) {
        for (const SyntaxNode *st : block->FieldItems(Field::Statements)) {
            for (const SyntaxNode *l = st; l && l->Kind() == SyntaxKind::LabeledStatement; l = l->Field(Field::Statement)) {
                int idx = Declare(SymKind::Label, l->Field(Field::Identifier), l, nullptr, nullptr);
                if (idx < 0) continue;
                // Labels live in their own namespace: take them out of the variable scope.
                scopes_.back().names.erase(b_.symbols[static_cast<size_t>(idx)].name);
                labels_.back()[b_.symbols[static_cast<size_t>(idx)].name] = idx;
            }
        }
    }

    void DeclareParameters(const SyntaxNode *list) {
        if (!list) return;
        for (const SyntaxNode *p : list->FieldItems(Field::Parameters)) {
            if (p->Kind() != SyntaxKind::Parameter) continue;
            Declare(SymKind::Parameter, p->Field(Field::Identifier), p, p->Field(Field::Type), p->Field(Field::Default));
        }
    }

    void DeclareDesignation(const SyntaxNode *d, SymKind kind, const SyntaxNode *type, const SyntaxNode *decl) {
        if (!d) return;
        if (d->Kind() == SyntaxKind::SingleVariableDesignation) {
            Declare(kind, d->Field(Field::Identifier), decl ? decl : d, type, nullptr);
        } else if (d->Kind() == SyntaxKind::ParenthesizedVariableDesignation) {
            for (const SyntaxNode *v : d->FieldItems(Field::Variables)) {
                if (!v->IsToken()) DeclareDesignation(v, kind, nullptr, nullptr);
            }
        }
    }

    bool IsBodyOwner(SyntaxKind k) {
        switch (k) {
        case SyntaxKind::MethodDeclaration:
        case SyntaxKind::ConstructorDeclaration:
        case SyntaxKind::DestructorDeclaration:
        case SyntaxKind::OperatorDeclaration:
        case SyntaxKind::ConversionOperatorDeclaration:
        case SyntaxKind::GetAccessorDeclaration:
        case SyntaxKind::SetAccessorDeclaration:
        case SyntaxKind::InitAccessorDeclaration:
        case SyntaxKind::AddAccessorDeclaration:
        case SyntaxKind::RemoveAccessorDeclaration:
        case SyntaxKind::UnknownAccessorDeclaration:
        case SyntaxKind::LocalFunctionStatement:
        case SyntaxKind::ParenthesizedLambdaExpression:
        case SyntaxKind::SimpleLambdaExpression:
        case SyntaxKind::AnonymousMethodExpression:
        case SyntaxKind::IndexerDeclaration:
        case SyntaxKind::PropertyDeclaration:
            return true;
        default:
            return false;
        }
    }

    void VisitChildren(const SyntaxNode *n) {
        for (const SyntaxNode *c : n->children) Visit(c);
    }

    void Visit(const SyntaxNode *n) {
        if (n->IsToken()) return;
        SyntaxKind k = n->Kind();
        switch (k) {
        case SyntaxKind::IdentifierName:
        case SyntaxKind::GenericName: {
            if (k == SyntaxKind::GenericName) {
                if (const SyntaxNode *tal = n->Field(Field::TypeArgumentList)) Visit(tal);
            }
            const SyntaxNode *tok = n->Field(Field::Identifier);
            if (!tok || tok->Kind() != SyntaxKind::IdentifierToken) return;
            if (IsMemberNamePosition(n) || IsTypePosition(n)) return;
            const SyntaxNode *p = n->parent;
            if (p && p->Kind() == SyntaxKind::GotoStatement) {
                std::string name = Doc::TokenValue(tok);
                for (auto it = labels_.rbegin(); it != labels_.rend(); ++it) {
                    if (it->count("\x01")) break;  // a lambda or local function: no jumping out
                    auto f = it->find(name);
                    if (f != it->end()) {
                        Reference(tok, f->second, false, nullptr);
                        break;
                    }
                }
                return;
            }
            int sym = Lookup(Doc::TokenValue(tok));
            if (sym < 0) return;
            bool write = IsPureWrite(n);
            const SyntaxNode *assigned = nullptr;
            if (write && p && p->Kind() == SyntaxKind::SimpleAssignmentExpression) assigned = p->Field(Field::Right);
            Reference(tok, sym, write, assigned);
            return;
        }
        case SyntaxKind::Block:
        case SyntaxKind::SwitchSection:
            Push(n);
            labels_.emplace_back();
            PredeclareStatements(n);
            DeclareLabels(n);
            VisitChildren(n);
            labels_.pop_back();
            Pop();
            return;
        case SyntaxKind::CompilationUnit:
            VisitChildren(n);
            return;
        case SyntaxKind::ClassDeclaration:
        case SyntaxKind::StructDeclaration:
        case SyntaxKind::RecordDeclaration:
        case SyntaxKind::RecordStructDeclaration:
        case SyntaxKind::InterfaceDeclaration:
            // A primary constructor's parameters are visible in the whole type.
            Push(n);
            DeclareParameters(n->Field(Field::ParameterList));
            for (const SyntaxNode *c : n->children) {
                if (c->Kind() != SyntaxKind::ParameterList) Visit(c);
                else VisitParameterDefaults(c);
            }
            Pop();
            return;
        case SyntaxKind::LocalDeclarationStatement: {
            const SyntaxNode *vd = n->Field(Field::Declaration);
            bool isUsing = n->Field(Field::UsingKeyword) != nullptr;
            VisitVariableDeclaration(vd, isUsing ? SymKind::UsingVariable : SymKind::Local);
            return;
        }
        case SyntaxKind::ForStatement:
        case SyntaxKind::UsingStatement:
        case SyntaxKind::FixedStatement: {
            Push(n);
            for (const SyntaxNode *c : n->children) {
                if (c->Kind() == SyntaxKind::VariableDeclaration) {
                    VisitVariableDeclaration(c, k == SyntaxKind::UsingStatement ? SymKind::UsingVariable : SymKind::Local);
                } else {
                    Visit(c);
                }
            }
            Pop();
            return;
        }
        case SyntaxKind::ForEachStatement: {
            Push(n);
            const SyntaxNode *expr = n->Field(Field::Expression);
            if (expr) Visit(expr);  // the collection is outside the variable's scope
            Declare(SymKind::ForEachVariable, n->Field(Field::Identifier), n, n->Field(Field::Type), expr);
            const SyntaxNode *body = n->Field(Field::Statement);
            if (body) Visit(body);
            Pop();
            return;
        }
        case SyntaxKind::ForEachVariableStatement: {
            Push(n);
            const SyntaxNode *expr = n->Field(Field::Expression);
            if (expr) Visit(expr);
            const SyntaxNode *var = n->Field(Field::Variable);
            if (var) Visit(var);
            const SyntaxNode *body = n->Field(Field::Statement);
            if (body) Visit(body);
            Pop();
            return;
        }
        case SyntaxKind::CatchClause: {
            Push(n);
            const SyntaxNode *decl = n->Field(Field::Declaration);
            if (decl) Declare(SymKind::CatchVariable, decl->Field(Field::Identifier), decl, decl->Field(Field::Type), nullptr);
            for (const SyntaxNode *c : n->children) {
                if (c != decl) Visit(c);
            }
            Pop();
            return;
        }
        case SyntaxKind::DeclarationExpression: {
            // out var x / var (a, b) = ...
            const SyntaxNode *type = n->Field(Field::Type);
            const SyntaxNode *p = n->parent;
            SymKind kind = (p && p->Kind() == SyntaxKind::Argument) ? SymKind::OutVariable : SymKind::Local;
            DeclareDesignation(n->Field(Field::Designation), kind, type, n);
            return;
        }
        case SyntaxKind::DeclarationPattern:
        case SyntaxKind::VarPattern:
        case SyntaxKind::RecursivePattern: {
            for (const SyntaxNode *c : n->children) {
                if (c->Kind() == SyntaxKind::SingleVariableDesignation ||
                    c->Kind() == SyntaxKind::ParenthesizedVariableDesignation) {
                    DeclareDesignation(c, SymKind::PatternVariable, n->Field(Field::Type), nullptr);
                } else {
                    Visit(c);
                }
            }
            return;
        }
        case SyntaxKind::SwitchExpressionArm:
        case SyntaxKind::QueryExpression:
            Push(n);
            VisitChildren(n);
            Pop();
            return;
        case SyntaxKind::FromClause:
        case SyntaxKind::JoinClause: {
            for (const SyntaxNode *c : n->children) {
                if (c->Kind() != SyntaxKind::IdentifierToken) Visit(c);
            }
            Declare(SymKind::RangeVariable, n->Field(Field::Identifier), n, n->Field(Field::Type),
                    n->Field(Field::InExpression) ? n->Field(Field::InExpression) : n->Field(Field::Expression));
            return;
        }
        case SyntaxKind::LetClause: {
            const SyntaxNode *e = n->Field(Field::Expression);
            if (e) Visit(e);
            Declare(SymKind::RangeVariable, n->Field(Field::Identifier), n, nullptr, e);
            return;
        }
        case SyntaxKind::JoinIntoClause:
        case SyntaxKind::QueryContinuation:
            Declare(SymKind::RangeVariable, n->Field(Field::Identifier), n, nullptr, nullptr);
            VisitChildren(n);
            return;
        default:
            break;
        }

        if (IsBodyOwner(k)) {
            Push(n);
            labels_.emplace_back();
            labels_.back()["\x01"] = -1;  // the boundary of a body
            if (k == SyntaxKind::SimpleLambdaExpression) {
                const SyntaxNode *p = n->Field(Field::Parameter);
                if (p) Declare(SymKind::Parameter, p->Field(Field::Identifier), p, p->Field(Field::Type), nullptr);
            } else if (k == SyntaxKind::IndexerDeclaration) {
                DeclareParameters(n->Field(Field::ParameterList));
            } else {
                DeclareParameters(n->Field(Field::ParameterList));
            }
            for (const SyntaxNode *c : n->children) {
                if (c->Kind() == SyntaxKind::ParameterList || c->Kind() == SyntaxKind::BracketedParameterList) {
                    VisitParameterDefaults(c);
                } else if (c->Kind() == SyntaxKind::Parameter && k == SyntaxKind::SimpleLambdaExpression) {
                    continue;
                } else if (c->Kind() == SyntaxKind::IdentifierToken) {
                    continue;
                } else {
                    Visit(c);
                }
            }
            labels_.pop_back();
            Pop();
            return;
        }
        VisitChildren(n);
    }

    void VisitParameterDefaults(const SyntaxNode *list) {
        for (const SyntaxNode *p : list->FieldItems(Field::Parameters)) {
            if (p->IsToken()) continue;
            for (const SyntaxNode *c : p->children) {
                if (c->Kind() == SyntaxKind::EqualsValueClause || c->Kind() == SyntaxKind::AttributeList) Visit(c);
            }
        }
    }

    void VisitVariableDeclaration(const SyntaxNode *vd, SymKind kind) {
        if (!vd) return;
        const SyntaxNode *type = vd->Field(Field::Type);
        if (type) Visit(type);
        for (const SyntaxNode *v : vd->FieldItems(Field::Variables)) {
            if (v->Kind() != SyntaxKind::VariableDeclarator) continue;
            const SyntaxNode *init = v->Field(Field::Initializer);
            const SyntaxNode *value = init ? init->Field(Field::Value) : nullptr;
            // C#: the variable is in scope in its own initializer (an error to read it there).
            Declare(kind, v->Field(Field::Identifier), v, type, value);
            for (const SyntaxNode *c : v->children) {
                if (!c->IsToken()) Visit(c);
            }
        }
    }
};

}  // namespace

Binding Bind(const Doc &doc) {
    Binding b;
    Binder(doc, b).Run();
    return b;
}

int SymbolAt(const Binding &b, const SyntaxNode *token) {
    auto it = b.by_token.find(token);
    return it == b.by_token.end() ? -1 : it->second;
}

// --- Declarations -------------------------------------------------------------------------------

std::string SourceType::FullName() const {
    std::string s = ns;
    if (!owner.empty()) s += (s.empty() ? "" : ".") + owner;
    s += (s.empty() ? "" : ".") + name;
    if (!type_params.empty()) s += "`" + std::to_string(type_params.size());
    return s;
}

namespace {

std::string ParamsText(const Doc &doc, const SyntaxNode *list) {
    if (!list) return "";
    return CollapseSpaces(doc.Text(list));
}

std::string TypeKindName(SyntaxKind k) {
    switch (k) {
    case SyntaxKind::ClassDeclaration:
        return "class";
    case SyntaxKind::StructDeclaration:
        return "struct";
    case SyntaxKind::InterfaceDeclaration:
        return "interface";
    case SyntaxKind::EnumDeclaration:
        return "enum";
    case SyntaxKind::RecordDeclaration:
        return "record";
    case SyntaxKind::RecordStructDeclaration:
        return "record struct";
    case SyntaxKind::DelegateDeclaration:
        return "delegate";
    case SyntaxKind::ExtensionBlockDeclaration:
        return "extension";
    default:
        return "class";
    }
}

void IndexMembers(const Doc &doc, const std::string &path, const SyntaxNode *typeDecl, const SourceType &owner,
                  SourceIndex *out, const std::string &ns, const std::string &ownerPath);

void IndexContainer(const Doc &doc, const std::string &path, const SyntaxNode *container, const std::string &ns,
                    const std::string &ownerPath, SourceIndex *out) {
    std::string currentNs = ns;
    for (const SyntaxNode *c : container->children) {
        SyntaxKind k = c->Kind();
        if (k == SyntaxKind::FileScopedNamespaceDeclaration) {
            // The rest of the file is in it (its members are its children).
            std::string inner = doc.Text(c->Field(Field::Name));
            inner.erase(std::remove_if(inner.begin(), inner.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), inner.end());
            IndexContainer(doc, path, c, ns.empty() ? inner : ns + "." + inner, "", out);
        } else if (k == SyntaxKind::NamespaceDeclaration) {
            std::string inner = doc.Text(c->Field(Field::Name));
            inner.erase(std::remove_if(inner.begin(), inner.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), inner.end());
            IndexContainer(doc, path, c, ns.empty() ? inner : ns + "." + inner, "", out);
        } else if (IsTypeDeclKind(k) && k != SyntaxKind::ExtensionBlockDeclaration) {
            SourceType t;
            t.ns = currentNs;
            t.owner = ownerPath;
            const SyntaxNode *id = c->Field(Field::Identifier);
            t.name = Doc::TokenValue(id);
            if (t.name.empty()) continue;
            const SyntaxNode *tpl = c->Field(Field::TypeParameterList);
            if (tpl) {
                for (const SyntaxNode *tp : tpl->FieldItems(Field::Parameters)) {
                    if (tp->Kind() == SyntaxKind::TypeParameter) t.type_params.push_back(Doc::TokenValue(tp->Field(Field::Identifier)));
                }
            }
            t.kind = TypeKindName(k);
            if (k == SyntaxKind::ClassDeclaration && HasModifier(c, SyntaxKind::StaticKeyword)) t.kind = "static class";
            const SyntaxNode *bl = c->Field(Field::BaseList);
            if (bl) {
                for (const SyntaxNode *b : bl->FieldItems(Field::Types)) {
                    if (b->IsToken()) continue;
                    t.bases.push_back(CollapseSpaces(doc.Text(b->Field(Field::Type))));
                }
            }
            t.doc = DocSummary(doc, c);
            t.signature = DeclHeader(doc, c);
            t.path = path;
            if (id) t.range = doc.RangeOf(id);
            out->types.push_back(t);
            SourceType copy = t;
            if (k != SyntaxKind::DelegateDeclaration) {
                IndexMembers(doc, path, c, copy, out, currentNs, ownerPath.empty() ? t.name : ownerPath + "." + t.name);
            } else {
                SourceMember m;
                m.owner_full = copy.FullName();
                m.name = "Invoke";
                m.kind = "method";
                m.type = CollapseSpaces(doc.Text(c->Field(Field::ReturnType)));
                m.params = ParamsText(doc, c->Field(Field::ParameterList));
                m.signature = t.signature;
                m.doc = t.doc;
                m.path = path;
                m.range = t.range;
                out->members.push_back(m);
            }
        } else if (k == SyntaxKind::GlobalStatement) {
            // Top-level statements: local functions are members of the Program class.
            const SyntaxNode *st = c->Field(Field::Statement);
            if (st && st->Kind() == SyntaxKind::LocalFunctionStatement) {
                SourceMember m;
                m.owner_full = "Program";
                m.name = Doc::TokenValue(st->Field(Field::Identifier));
                m.kind = "method";
                m.type = CollapseSpaces(doc.Text(st->Field(Field::ReturnType)));
                m.params = ParamsText(doc, st->Field(Field::ParameterList));
                m.signature = DeclHeader(doc, st);
                m.doc = DocSummary(doc, st);
                m.is_static = true;
                m.path = path;
                if (st->Field(Field::Identifier)) m.range = doc.RangeOf(st->Field(Field::Identifier));
                out->members.push_back(m);
            }
        }
    }
}

void IndexMembers(const Doc &doc, const std::string &path, const SyntaxNode *typeDecl, const SourceType &owner,
                  SourceIndex *out, const std::string &ns, const std::string &ownerPath) {
    const std::string ownerFull = owner.FullName();
    auto add = [&](const SyntaxNode *decl, const SyntaxNode *nameTok, const std::string &kind, const std::string &type,
                   const std::string &params) {
        if (!nameTok) return;
        SourceMember m;
        m.owner_full = ownerFull;
        m.name = Doc::TokenValue(nameTok);
        if (kind == "indexer") m.name = "this[]";
        if (kind == "constructor") m.name = owner.name;
        m.kind = kind;
        m.type = type;
        m.params = params;
        m.signature = DeclHeader(doc, decl);
        m.doc = DocSummary(doc, decl);
        m.is_static = HasModifier(decl, SyntaxKind::StaticKeyword) || HasModifier(decl, SyntaxKind::ConstKeyword) ||
                      kind == "enum member";
        m.path = path;
        m.range = doc.RangeOf(nameTok);
        if (kind == "method") {
            const SyntaxNode *pl = decl->Field(Field::ParameterList);
            std::vector<const SyntaxNode *> ps = pl ? pl->FieldItems(Field::Parameters) : std::vector<const SyntaxNode *>{};
            if (!ps.empty() && !ps.front()->IsToken() && HasModifier(ps.front(), SyntaxKind::ThisKeyword)) m.is_extension = true;
        }
        out->members.push_back(m);
    };
    // Positional record parameters are properties.
    if (const SyntaxNode *pl = typeDecl->Field(Field::ParameterList)) {
        if (typeDecl->Kind() == SyntaxKind::RecordDeclaration || typeDecl->Kind() == SyntaxKind::RecordStructDeclaration) {
            for (const SyntaxNode *p : pl->FieldItems(Field::Parameters)) {
                if (p->IsToken()) continue;
                add(p, p->Field(Field::Identifier), "property", CollapseSpaces(doc.Text(p->Field(Field::Type))), "");
            }
        }
    }
    for (const SyntaxNode *m : typeDecl->FieldItems(Field::Members)) {
        switch (m->Kind()) {
        case SyntaxKind::MethodDeclaration:
            add(m, m->Field(Field::Identifier), "method", CollapseSpaces(doc.Text(m->Field(Field::ReturnType))),
                ParamsText(doc, m->Field(Field::ParameterList)));
            break;
        case SyntaxKind::ConstructorDeclaration:
            add(m, m->Field(Field::Identifier), "constructor", "", ParamsText(doc, m->Field(Field::ParameterList)));
            break;
        case SyntaxKind::PropertyDeclaration:
            add(m, m->Field(Field::Identifier), "property", CollapseSpaces(doc.Text(m->Field(Field::Type))), "");
            break;
        case SyntaxKind::IndexerDeclaration:
            add(m, m->Field(Field::ThisKeyword), "indexer", CollapseSpaces(doc.Text(m->Field(Field::Type))),
                ParamsText(doc, m->Field(Field::ParameterList)));
            break;
        case SyntaxKind::EventDeclaration:
            add(m, m->Field(Field::Identifier), "event", CollapseSpaces(doc.Text(m->Field(Field::Type))), "");
            break;
        case SyntaxKind::FieldDeclaration:
        case SyntaxKind::EventFieldDeclaration: {
            const SyntaxNode *vd = m->Field(Field::Declaration);
            if (!vd) break;
            std::string type = CollapseSpaces(doc.Text(vd->Field(Field::Type)));
            std::string kind = m->Kind() == SyntaxKind::EventFieldDeclaration ? "event"
                               : HasModifier(m, SyntaxKind::ConstKeyword) ? "const"
                                                                           : "field";
            for (const SyntaxNode *v : vd->FieldItems(Field::Variables)) {
                if (v->Kind() == SyntaxKind::VariableDeclarator) add(m, v->Field(Field::Identifier), kind, type, "");
            }
            break;
        }
        case SyntaxKind::EnumMemberDeclaration:
            add(m, m->Field(Field::Identifier), "enum member", owner.name, "");
            break;
        default:
            break;  // nested types: below
        }
    }
    // Nested types.
    std::vector<const SyntaxNode *> nested;
    for (const SyntaxNode *m : typeDecl->FieldItems(Field::Members)) {
        if (IsTypeDeclKind(m->Kind())) nested.push_back(m);
    }
    if (!nested.empty()) {
        // Index them through a synthetic container: reuse IndexContainer over each.
        for (const SyntaxNode *m : nested) {
            SyntaxNode fake;
            fake.green = typeDecl->green;
            fake.children = {m};
            IndexContainer(doc, path, &fake, ns, ownerPath, out);
        }
    }
}

}  // namespace

void IndexDeclarations(const Doc &doc, const std::string &path, SourceIndex *out) {
    IndexContainer(doc, path, doc.tree->Root(), "", "", out);
}

// --- Context -------------------------------------------------------------------------------------

namespace {

std::string NameText(const Doc &doc, const SyntaxNode *n) {
    std::string s = doc.Text(n);
    s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), s.end());
    if (s.rfind("global::", 0) == 0) s = s.substr(8);
    return s;
}

void AddUsings(const Doc &doc, const SyntaxNode *container, std::vector<std::string> &usings) {
    for (const SyntaxNode *u : container->FieldItems(Field::Usings)) {
        if (u->Kind() != SyntaxKind::UsingDirective) continue;
        if (u->Field(Field::Alias) || u->Field(Field::StaticKeyword)) continue;
        const SyntaxNode *name = u->Field(Field::NamespaceOrType);
        if (!name) name = u->Field(Field::Name);
        if (name) usings.push_back(NameText(doc, name));
    }
}

}  // namespace

Context ContextAt(const Doc &doc, const SyntaxNode *node) {
    Context ctx;
    // SDK projects' implicit global usings.
    ctx.usings = {"System", "System.Collections.Generic", "System.IO", "System.Linq", "System.Net.Http",
                  "System.Threading", "System.Threading.Tasks"};
    AddUsings(doc, doc.tree->Root(), ctx.usings);
    std::vector<std::string> nsParts;
    std::vector<std::string> enclosing;
    for (const SyntaxNode *n = node; n; n = n->parent) {
        SyntaxKind k = n->Kind();
        if (k == SyntaxKind::NamespaceDeclaration || k == SyntaxKind::FileScopedNamespaceDeclaration) {
            nsParts.insert(nsParts.begin(), NameText(doc, n->Field(Field::Name)));
            AddUsings(doc, n, ctx.usings);
        } else if (IsTypeDeclKind(k)) {
            enclosing.insert(enclosing.begin(), Doc::TokenValue(n->Field(Field::Identifier)));
            if (!ctx.type_decl) ctx.type_decl = n;
        } else if (!ctx.member_decl && n->parent && IsTypeDeclKind(n->parent->Kind()) && !n->IsToken() &&
                   !IsTypeDeclKind(k)) {
            ctx.member_decl = n;
            ctx.in_static = HasModifier(n, SyntaxKind::StaticKeyword);
        }
    }
    // A file-scoped namespace is not an ancestor of the members before... it is: its members are its children.
    for (const SyntaxNode *c : doc.tree->Root()->children) {
        if (c->Kind() == SyntaxKind::FileScopedNamespaceDeclaration && nsParts.empty()) {
            if (node->position >= c->position) {
                nsParts.push_back(NameText(doc, c->Field(Field::Name)));
                AddUsings(doc, c, ctx.usings);
            }
        }
    }
    for (const std::string &p : nsParts) ctx.ns += (ctx.ns.empty() ? "" : ".") + p;
    // Each enclosing namespace is in scope too.
    std::string acc;
    for (const std::string &p : nsParts) {
        acc += (acc.empty() ? "" : ".") + p;
        ctx.usings.push_back(acc);
    }
    ctx.enclosing = enclosing;
    return ctx;
}

}  // namespace cslsp
