// Prettier 3.6.2 comment handling: main/comments/{attach,print}.js,
// language-js/comments/{handle-comments,printer-methods}.js and
// language-js/print/comment.js.

#include <algorithm>

#include "js_format_internal.h"

namespace jsfmt {

// ---------------------------------------------------------------- attach.js

namespace {

bool CanAttachComment(const Node* n) {
    return n->type != NT::EmptyStatement && n->type != NT::TemplateElement && n->type != NT::ChainExpression &&
           n->type != NT::TSEmptyBodyFunctionExpression;
}

struct Ctx {
    CommentRec* comment;
    Node* preceding;
    Node* enclosing;
    Node* following;
    Node* ast;
    bool is_last;
};

}  // namespace

const std::vector<Node*>& Printer::SortedChildNodes(Node* n) {
    auto it = child_cache_.find(n);
    if (it != child_cache_.end()) return it->second;
    std::vector<Node*> raw;
    // getCommentChildNodes (acorn): `bar() // c\n {}` keeps comments off the FunctionExpression.
    if (n->type == NT::MethodDefinition && Is(n->value(), NT::FunctionExpression) &&
        n->value()->params().empty() && !n->value()->returnType() && !n->value()->typeParameters() &&
        n->value()->body()) {
        for (Node* d : n->decorators()) raw.push_back(d);
        raw.push_back(n->key());
        raw.push_back(n->value()->body());
    } else {
        jsparse::CollectChildren(n, raw);
    }
    std::vector<Node*> out;
    for (Node* c : raw) {
        if (!c) continue;
        if (CanAttachComment(c)) {
            out.push_back(c);
        } else {
            const auto& sub = SortedChildNodes(c);
            out.insert(out.end(), sub.begin(), sub.end());
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Node* a, const Node* b) {
        if (a->start != b->start) return a->start < b->start;
        return a->end < b->end;
    });
    return child_cache_[n] = std::move(out);
}

static int FindExpressionIndexForComment(const NodeList& quasis, uint32_t start) {
    const int64_t pos = static_cast<int64_t>(start) - 1;
    for (uint32_t i = 1; i < quasis.size(); ++i) {
        if (pos < static_cast<int64_t>(quasis[i]->start)) return static_cast<int>(i) - 1;
    }
    return 0;
}

void Printer::DecorateComment(Node* node, CommentRec* c, Node* enclosing) {
    Node* preceding = nullptr;
    Node* following = nullptr;
    while (true) {
        const auto& kids = SortedChildNodes(node);
        size_t left = 0, right = kids.size();
        bool descended = false;
        preceding = following = nullptr;
        while (left < right) {
            const size_t middle = (left + right) >> 1;
            Node* child = kids[middle];
            if (child->start <= c->start && c->end <= child->end) {
                node = child;
                enclosing = child;
                descended = true;
                break;
            }
            if (child->end <= c->start) {
                preceding = child;
                left = middle + 1;
                continue;
            }
            if (c->end <= child->start) {
                following = child;
                right = middle;
                continue;
            }
            // overlap: should not happen with a well-formed tree
            right = left;
        }
        if (!descended) break;
    }
    if (Is(enclosing, NT::TemplateLiteral)) {
        const NodeList& quasis = enclosing->quasis();
        const int idx = FindExpressionIndexForComment(quasis, c->start);
        if (preceding && FindExpressionIndexForComment(quasis, preceding->start) != idx) preceding = nullptr;
        if (following && FindExpressionIndexForComment(quasis, following->start) != idx) following = nullptr;
    }
    c->enclosing = enclosing;
    c->preceding = preceding;
    c->following = following;
}

void Printer::AddLeading(Node* n, CommentRec* c) {
    c->leading = true;
    c->trailing = false;
    comments_of_[n].push_back(c);
}
void Printer::AddTrailing(Node* n, CommentRec* c) {
    c->leading = false;
    c->trailing = true;
    comments_of_[n].push_back(c);
}
void Printer::AddDangling(Node* n, CommentRec* c) {
    c->leading = false;
    c->trailing = false;
    comments_of_[n].push_back(c);
}

namespace {

// /[\S\n  ]/ absent.
bool IsAllEmptyAndNoLineBreak(std::string_view s) {
    for (size_t i = 0; i < s.size(); i++) {
        const auto ch = static_cast<unsigned char>(s[i]);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\v' || ch == '\f') continue;
        if (ch == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            i++;
            continue;
        }
        if (ch == 0xEF && s.substr(i, 3) == "\xEF\xBB\xBF") {
            i += 2;
            continue;
        }
        if (ch == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80) {
            const auto b = static_cast<unsigned char>(s[i + 2]);
            if ((b >= 0x80 && b <= 0x8A) || b == 0xAF) {
                i += 2;
                continue;
            }
        }
        if (ch == 0xE3 && s.substr(i, 3) == "\xE3\x80\x80") {
            i += 2;
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

void Printer::Attach(Node* program, std::vector<CommentRec*> comments) {
    all_comments_ = comments;
    if (comments.empty()) return;
    for (CommentRec* c : comments) DecorateComment(program, c, nullptr);

    std::vector<CommentRec*> ties;
    auto break_ties = [&] {
        if (ties.empty()) return;
        Node* preceding = ties[0]->preceding;
        Node* following = ties[0]->following;
        int64_t gap_end = following->start;
        size_t first_leading = ties.size();
        for (; first_leading > 0; --first_leading) {
            CommentRec* c = ties[first_leading - 1];
            const std::string_view gap = text_.substr(c->end, static_cast<size_t>(gap_end) - c->end);
            bool is_gap = true;
            for (char ch : gap) {
                if (!(ch == '(' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f')) {
                    is_gap = false;
                    break;
                }
            }
            if (is_gap) {
                gap_end = c->start;
            } else {
                break;
            }
        }
        for (size_t i = 0; i < ties.size(); i++) {
            if (i < first_leading) {
                AddTrailing(preceding, ties[i]);
            } else {
                AddLeading(following, ties[i]);
            }
        }
        for (Node* n : {preceding, following}) {
            auto& cs = comments_of_[n];
            if (cs.size() > 1) {
                std::stable_sort(cs.begin(), cs.end(),
                                 [](const CommentRec* a, const CommentRec* b) { return a->start < b->start; });
            }
        }
        ties.clear();
    };

    for (size_t index = 0; index < comments.size(); index++) {
        CommentRec* c = comments[index];
        Node* preceding = c->preceding;
        Node* following = c->following;
        Node* enclosing = c->enclosing;
        const bool is_last = index + 1 == comments.size();

        // isOwnLineComment
        bool own_line;
        {
            uint32_t start = c->start;
            if (preceding) {
                for (size_t k = index; k-- > 0;) {
                    CommentRec* o = comments[k];
                    if (o->preceding != preceding || !IsAllEmptyAndNoLineBreak(text_.substr(o->end, start - o->end))) {
                        break;
                    }
                    start = o->start;
                }
            }
            own_line = HasNewline(start, true);
        }
        if (own_line) {
            c->placement = Placement::OwnLine;
            if (HandleOwnLine(c, program, is_last)) {
            } else if (following) {
                AddLeading(following, c);
            } else if (preceding) {
                AddTrailing(preceding, c);
            } else {
                AddDangling(enclosing ? enclosing : program, c);
            }
            continue;
        }
        bool end_of_line;
        {
            uint32_t end = c->end;
            if (following) {
                for (size_t k = index + 1; k < comments.size(); k++) {
                    CommentRec* o = comments[k];
                    if (o->following != following || !IsAllEmptyAndNoLineBreak(text_.substr(end, o->start - end))) {
                        break;
                    }
                    end = o->end;
                }
            }
            end_of_line = HasNewline(end);
        }
        if (end_of_line) {
            c->placement = Placement::EndOfLine;
            if (HandleEndOfLine(c, program, is_last)) {
            } else if (preceding) {
                AddTrailing(preceding, c);
            } else if (following) {
                AddLeading(following, c);
            } else {
                AddDangling(enclosing ? enclosing : program, c);
            }
            continue;
        }
        c->placement = Placement::Remaining;
        if (HandleRemaining(c, program, is_last)) {
        } else if (preceding && following) {
            if (!ties.empty() && ties.back()->following != following) break_ties();
            ties.push_back(c);
        } else if (preceding) {
            AddTrailing(preceding, c);
        } else if (following) {
            AddLeading(following, c);
        } else {
            AddDangling(enclosing ? enclosing : program, c);
        }
    }
    break_ties();
}

// ---------------------------------------------------------------- handle-comments.js

namespace {

bool IsRealFunctionLikeNode(const Node* n) {
    if (!n) return false;
    switch (n->type) {
        case NT::ArrowFunctionExpression:
        case NT::FunctionExpression:
        case NT::FunctionDeclaration:
        case NT::TSDeclareFunction:
        case NT::TSCallSignatureDeclaration:
        case NT::TSConstructSignatureDeclaration:
        case NT::TSMethodSignature:
        case NT::TSConstructorType:
        case NT::TSFunctionType:
            return true;
        default:
            return false;
    }
}

bool IsClassLike(const Node* n) {
    return n && (n->type == NT::ClassDeclaration || n->type == NT::ClassExpression ||
                 n->type == NT::TSInterfaceDeclaration);
}

bool IsPropertyLike(const Node* n) {
    if (!n) return false;
    switch (n->type) {
        case NT::PropertyDefinition:
        case NT::TSAbstractPropertyDefinition:
        case NT::TSAbstractMethodDefinition:
        case NT::MethodDefinition:
        case NT::AccessorProperty:
        case NT::TSAbstractAccessorProperty:
        case NT::TSParameterProperty:
            return true;
        default:
            return false;
    }
}

}  // namespace

// The handlers, as one struct so they can share the context.
struct Handlers {
    Printer& p;
    CommentRec* comment;
    Node* preceding;
    Node* enclosing;
    Node* following;
    Node* ast;
    bool is_last;

    int NextChar(int64_t i) const { return p.GetNextNonSpaceNonCommentCharacter(i); }
    bool IsSingleLineComment() const {
        return IsLineComment(comment) || !p.HasNewlineInRange(comment->start, comment->end);
    }

    void AddBlockStatementFirstComment(Node* n) {
        const NodeList& body = n->type == NT::ObjectExpression || n->type == NT::ObjectPattern
                                   ? n->properties()
                                   : (n->type == NT::ClassBody || n->type == NT::BlockStatement ||
                                      n->type == NT::Program || n->type == NT::StaticBlock ||
                                      n->type == NT::TSInterfaceBody)
                                         ? n->body_list()
                                         : n->properties();
        for (Node* s : body) {
            if (s && s->type != NT::EmptyStatement) {
                p.AddLeading(s, comment);
                return;
            }
        }
        p.AddDangling(n, comment);
    }
    void AddBlockOrNotComment(Node* n) {
        if (n->type == NT::BlockStatement) {
            AddBlockStatementFirstComment(n);
        } else {
            p.AddLeading(n, comment);
        }
    }

    bool ClosureTypeCast() {
        if (following && IsTypeCastComment(comment)) {
            p.AddLeading(following, comment);
            return true;
        }
        return false;
    }

    bool IfStatement() {
        if (!Is(enclosing, NT::IfStatement) || !following) return false;
        if (NextChar(comment->end) == ')') {
            p.AddTrailing(preceding, comment);
            return true;
        }
        if (preceding && preceding == enclosing->consequent() && following == enclosing->alternate()) {
            const int64_t else_idx = p.GetNextNonSpaceNonCommentCharacterIndex(enclosing->consequent()->end);
            if (static_cast<int64_t>(comment->start) < else_idx || enclosing->alternate()->type == NT::BlockStatement) {
                if (preceding->type == NT::BlockStatement) {
                    p.AddTrailing(preceding, comment);
                    return true;
                }
                if (IsSingleLineComment() && !p.HasNewlineInRange(preceding->start, comment->start)) {
                    p.AddTrailing(preceding, comment);
                    return true;
                }
                p.AddDangling(enclosing, comment);
                return true;
            }
        }
        if (following->type == NT::BlockStatement) {
            AddBlockStatementFirstComment(following);
            return true;
        }
        if (following->type == NT::IfStatement) {
            AddBlockOrNotComment(following->consequent());
            return true;
        }
        if (enclosing->consequent() == following) {
            p.AddLeading(following, comment);
            return true;
        }
        return false;
    }

    bool While() {
        if (!Is(enclosing, NT::WhileStatement) || !following) return false;
        if (NextChar(comment->end) == ')') {
            p.AddTrailing(preceding, comment);
            return true;
        }
        if (following->type == NT::BlockStatement) {
            AddBlockStatementFirstComment(following);
            return true;
        }
        if (enclosing->body() == following) {
            p.AddLeading(following, comment);
            return true;
        }
        return false;
    }

    bool TryStatement() {
        if ((!Is(enclosing, NT::TryStatement) && !Is(enclosing, NT::CatchClause)) || !following) return false;
        if (enclosing->type == NT::CatchClause && preceding) {
            p.AddTrailing(preceding, comment);
            return true;
        }
        if (following->type == NT::BlockStatement) {
            AddBlockStatementFirstComment(following);
            return true;
        }
        if (following->type == NT::TryStatement) {
            AddBlockOrNotComment(following->finalizer());
            return true;
        }
        if (following->type == NT::CatchClause) {
            AddBlockOrNotComment(following->body());
            return true;
        }
        return false;
    }

    bool MemberExpression() {
        if (p.IsMemberExpression(enclosing) && Is(following, NT::Identifier)) {
            p.AddLeading(enclosing, comment);
            return true;
        }
        return false;
    }

    bool ConditionalExpression() {
        const bool same_line = preceding && !p.HasNewlineInRange(preceding->end, comment->start);
        if ((!preceding || !same_line) && (Is(enclosing, NT::ConditionalExpression) || IsConditionalType(enclosing)) &&
            following) {
            p.AddLeading(following, comment);
            return true;
        }
        return false;
    }

    bool ObjectPropertyAssignment() {
        if (IsObjectProperty(enclosing) && enclosing->Has(jsparse::kFlagShorthand) && enclosing->key() == preceding &&
            Is(enclosing->value(), NT::AssignmentPattern)) {
            p.AddTrailing(enclosing->value()->left(), comment);
            return true;
        }
        return false;
    }

    bool ClassComments() {
        if (!IsClassLike(enclosing)) return false;
        if (!enclosing->decorators().empty() && !Is(following, NT::Decorator)) {
            p.AddTrailing(enclosing->decorators()[enclosing->decorators().size() - 1], comment);
            return true;
        }
        if (enclosing->body() && following == enclosing->body()) {
            AddBlockStatementFirstComment(enclosing->body());
            return true;
        }
        if (following) {
            const bool is_class = enclosing->type != NT::TSInterfaceDeclaration;
            if (is_class && enclosing->superClass() && following == enclosing->superClass() && preceding &&
                (preceding == enclosing->id() || preceding == enclosing->typeParameters())) {
                p.AddTrailing(preceding, comment);
                return true;
            }
            const NodeList* lists[2] = {is_class ? &enclosing->implements() : nullptr,
                                        is_class ? nullptr : &enclosing->extends_list()};
            const Key keys[2] = {Key::implements, Key::extends_};
            for (int i = 0; i < 2; i++) {
                if (!lists[i] || lists[i]->empty() || following != (*lists[i])[0]) continue;
                if (preceding && (preceding == enclosing->id() || preceding == enclosing->typeParameters() ||
                                  (is_class && preceding == enclosing->superClass()))) {
                    p.AddTrailing(preceding, comment);
                } else {
                    p.AddDangling(enclosing, comment);
                    comment->marker = keys[i];
                }
                return true;
            }
        }
        return false;
    }

    bool IgnoreComments() {
        if (IsPrettierIgnoreComment(comment) && Is(enclosing, NT::TSMappedType) && following &&
            following == enclosing->key()) {
            p.prettier_ignore_.insert(enclosing);
            comment->unignore = true;
            return true;
        }
        return false;
    }

    bool UnionTypeComments() {
        if (IsUnionType(enclosing)) {
            if (IsPrettierIgnoreComment(comment)) {
                p.prettier_ignore_.insert(following);
                comment->unignore = true;
            }
            if (preceding) {
                p.AddTrailing(preceding, comment);
                return true;
            }
            return false;
        }
        if (IsUnionType(following) && IsPrettierIgnoreComment(comment)) {
            p.prettier_ignore_.insert(following->types()[0]);
            comment->unignore = true;
        }
        return false;
    }

    bool TSFunctionTrailingComments() {
        if (!following && enclosing &&
            (enclosing->type == NT::TSMethodSignature || enclosing->type == NT::TSDeclareFunction ||
             enclosing->type == NT::TSAbstractMethodDefinition) &&
            NextChar(comment->end) == ';') {
            p.AddTrailing(enclosing, comment);
            return true;
        }
        return false;
    }

    bool TSMappedTypeComments() {
        if (!Is(enclosing, NT::TSMappedType)) return false;
        if (!preceding) {
            p.AddDangling(enclosing, comment);
            return true;
        }
        return false;
    }

    bool LastUnionElementInExpression() {
        if (IsUnionType(preceding) && enclosing &&
            ((enclosing->type == NT::TSArrayType && !following) || enclosing->type == NT::TSIntersectionType)) {
            p.AddTrailing(preceding->types()[preceding->types().size() - 1], comment);
            return true;
        }
        return false;
    }

    bool CommentsInDestructuringPattern() {
        if ((Is(enclosing, NT::ObjectPattern) || Is(enclosing, NT::ArrayPattern)) &&
            Is(following, NT::TSTypeAnnotation)) {
            if (preceding) {
                p.AddTrailing(preceding, comment);
            } else {
                p.AddDangling(enclosing, comment);
            }
            return true;
        }
        return false;
    }

    bool MethodNameComments() {
        if (enclosing && preceding && NextChar(comment->end) == '(' &&
            (enclosing->type == NT::Property || enclosing->type == NT::TSAbstractMethodDefinition) &&
            preceding->type == NT::Identifier && enclosing->key() == preceding &&
            NextChar(preceding->end) != ':') {
            p.AddTrailing(preceding, comment);
            return true;
        }
        if (Is(preceding, NT::Decorator) && IsPropertyLike(enclosing) &&
            (IsLineComment(comment) || comment->placement == Placement::OwnLine)) {
            p.AddTrailing(preceding, comment);
            return true;
        }
        return false;
    }

    bool FunctionNameComments() {
        if (NextChar(comment->end) != '(') return false;
        if (preceding && enclosing &&
            (enclosing->type == NT::FunctionDeclaration || enclosing->type == NT::FunctionExpression ||
             enclosing->type == NT::MethodDefinition)) {
            p.AddTrailing(preceding, comment);
            return true;
        }
        return false;
    }

    bool CommentAfterArrowParams() {
        if (!Is(enclosing, NT::ArrowFunctionExpression)) return false;
        const int64_t index = p.GetNextNonSpaceNonCommentCharacterIndex(comment->end);
        if (index != Printer::kNoIndex && p.text_.substr(static_cast<size_t>(index), 2) == "=>") {
            p.AddDangling(enclosing, comment);
            return true;
        }
        return false;
    }

    bool CommentInEmptyParens() {
        if (NextChar(comment->end) != ')') return false;
        if (enclosing && ((IsRealFunctionLikeNode(enclosing) && enclosing->params().empty()) ||
                          (p.IsCallLikeExpression(enclosing) && p.GetCallArguments(enclosing).empty()))) {
            p.AddDangling(enclosing, comment);
            return true;
        }
        if ((Is(enclosing, NT::MethodDefinition) || Is(enclosing, NT::TSAbstractMethodDefinition)) &&
            enclosing->value()->params().empty()) {
            p.AddDangling(enclosing->value(), comment);
            return true;
        }
        return false;
    }

    bool LastFunctionArg() {
        if (preceding &&
            (preceding->type == NT::Identifier || preceding->type == NT::AssignmentPattern ||
             preceding->type == NT::ObjectPattern || preceding->type == NT::ArrayPattern ||
             preceding->type == NT::RestElement || preceding->type == NT::TSParameterProperty) &&
            IsRealFunctionLikeNode(enclosing) && NextChar(comment->end) == ')') {
            p.AddTrailing(preceding, comment);
            return true;
        }
        if (!IsBlockComment(comment) && enclosing &&
            (enclosing->type == NT::FunctionDeclaration || enclosing->type == NT::FunctionExpression) &&
            Is(following, NT::BlockStatement) && enclosing->body() == following) {
            const int64_t idx = p.GetNextNonSpaceNonCommentCharacterIndex(comment->end);
            if (idx == static_cast<int64_t>(following->start)) {
                AddBlockStatementFirstComment(following);
                return true;
            }
        }
        return false;
    }

    bool LabeledStatement() {
        if (Is(enclosing, NT::LabeledStatement)) {
            p.AddLeading(enclosing, comment);
            return true;
        }
        return false;
    }

    bool BreakAndContinue() {
        if ((Is(enclosing, NT::ContinueStatement) || Is(enclosing, NT::BreakStatement)) && !enclosing->label()) {
            p.AddTrailing(enclosing, comment);
            return true;
        }
        return false;
    }

    bool CallExpression() {
        if (p.IsCallExpression(enclosing) && preceding && enclosing->callee() == preceding &&
            !enclosing->arguments().empty()) {
            p.AddLeading(enclosing->arguments()[0], comment);
            return true;
        }
        return false;
    }

    bool PropertyComments() {
        if (IsObjectProperty(enclosing)) {
            p.AddLeading(enclosing, comment);
            return true;
        }
        return false;
    }

    bool OnlyComments() {
        if (ast->body_list().empty()) {
            if (is_last) {
                p.AddDangling(ast, comment);
            } else {
                p.AddLeading(ast, comment);
            }
            return true;
        }
        return false;
    }

    bool ForComments() {
        if (Is(enclosing, NT::ForInStatement) || Is(enclosing, NT::ForOfStatement)) {
            p.AddLeading(enclosing, comment);
            return true;
        }
        return false;
    }

    bool ModuleSpecifiers() {
        if (Is(enclosing, NT::ImportSpecifier) || Is(enclosing, NT::ExportSpecifier)) {
            p.AddLeading(enclosing, comment);
            return true;
        }
        const bool is_import = Is(preceding, NT::ImportSpecifier) && Is(enclosing, NT::ImportDeclaration);
        const bool is_export = Is(preceding, NT::ExportSpecifier) && Is(enclosing, NT::ExportNamedDeclaration);
        if ((is_import || is_export) && p.HasNewline(comment->end)) {
            p.AddTrailing(preceding, comment);
            return true;
        }
        return false;
    }

    bool AssignmentPattern() {
        if (Is(enclosing, NT::AssignmentPattern)) {
            p.AddLeading(enclosing, comment);
            return true;
        }
        return false;
    }

    bool VariableDeclarator() {
        if (enclosing &&
            (enclosing->type == NT::VariableDeclarator || enclosing->type == NT::AssignmentExpression ||
             enclosing->type == NT::TSTypeAliasDeclaration) &&
            following &&
            (following->type == NT::ObjectExpression || following->type == NT::ArrayExpression ||
             following->type == NT::TemplateLiteral || following->type == NT::TaggedTemplateExpression ||
             following->type == NT::TSTypeLiteral || IsBlockComment(comment))) {
            p.AddLeading(following, comment);
            return true;
        }
        return false;
    }

    bool SwitchDefaultCase() {
        if (!enclosing || enclosing->type != NT::SwitchCase || enclosing->test() || !following ||
            enclosing->consequent_list().empty() || following != enclosing->consequent_list()[0]) {
            return false;
        }
        if (following->type == NT::BlockStatement && IsLineComment(comment)) {
            AddBlockStatementFirstComment(following);
        } else {
            p.AddDangling(enclosing, comment);
        }
        return true;
    }

    bool LastBinaryOperatorOperand() {
        if (!following && Is(enclosing, NT::UnaryExpression) &&
            (Is(preceding, NT::LogicalExpression) || Is(preceding, NT::BinaryExpression))) {
            if (p.HasNewlineInRange(enclosing->argument()->start, preceding->right()->start) && IsSingleLineComment() &&
                !p.HasNewlineInRange(preceding->right()->start, comment->start)) {
                p.AddTrailing(preceding->right(), comment);
                return true;
            }
        }
        return false;
    }
};

bool Printer::HandleOwnLine(CommentRec* c, Node* ast, bool is_last) {
    Handlers h{*this, c, c->preceding, c->enclosing, c->following, ast, is_last};
    return h.IgnoreComments() || h.ConditionalExpression() || h.LastFunctionArg() || h.MemberExpression() ||
           h.IfStatement() || h.While() || h.TryStatement() || h.ClassComments() || h.ForComments() ||
           h.UnionTypeComments() || h.OnlyComments() || h.ModuleSpecifiers() || h.AssignmentPattern() ||
           h.MethodNameComments() || h.LabeledStatement() || h.BreakAndContinue() ||
           h.CommentsInDestructuringPattern() || h.TSMappedTypeComments();
}

bool Printer::HandleEndOfLine(CommentRec* c, Node* ast, bool is_last) {
    Handlers h{*this, c, c->preceding, c->enclosing, c->following, ast, is_last};
    return h.ClosureTypeCast() || h.LastFunctionArg() || h.ConditionalExpression() || h.ModuleSpecifiers() ||
           h.IfStatement() || h.While() || h.TryStatement() || h.ClassComments() || h.LabeledStatement() ||
           h.CallExpression() || h.PropertyComments() || h.OnlyComments() || h.VariableDeclarator() ||
           h.BreakAndContinue() || h.SwitchDefaultCase() || h.LastUnionElementInExpression() ||
           h.LastBinaryOperatorOperand() || h.TSMappedTypeComments();
}

bool Printer::HandleRemaining(CommentRec* c, Node* ast, bool is_last) {
    Handlers h{*this, c, c->preceding, c->enclosing, c->following, ast, is_last};
    return h.IgnoreComments() || h.IfStatement() || h.While() || h.ObjectPropertyAssignment() ||
           h.CommentInEmptyParens() || h.MethodNameComments() || h.OnlyComments() || h.CommentAfterArrowParams() ||
           h.FunctionNameComments() || h.BreakAndContinue() || h.TSFunctionTrailingComments();
}

// ---------------------------------------------------------------- print.js

static std::string_view TrimEndJs(std::string_view s) {
    const std::string_view t = TrimJs(s);
    if (t.empty()) return s.substr(0, 0);
    return s.substr(0, static_cast<size_t>(t.data() - s.data()) + t.size());
}
static std::string_view TrimStartJs2(std::string_view s) {
    const std::string_view t = TrimJs(s);
    if (t.empty()) return s.substr(s.size());
    return s.substr(static_cast<size_t>(t.data() - s.data()));
}

Doc* Printer::PrintComment(CommentRec* c) {
    c->printed = true;
    if (IsLineComment(c)) return S(TrimEndJs(text_.substr(c->start, c->end - c->start)));
    if (IsIndentableBlockComment(c)) {
        std::vector<std::string_view> lines;
        std::string_view v = c->value;
        while (true) {
            const size_t nl = v.find('\n');
            lines.push_back(v.substr(0, nl));
            if (nl == std::string_view::npos) break;
            v.remove_prefix(nl + 1);
        }
        Parts ps;
        for (size_t i = 0; i < lines.size(); i++) {
            if (i == 0) {
                ps.push_back(S(TrimEndJs(lines[i])));
            } else {
                ps.push_back(a_.Own(" " + std::string(i + 1 < lines.size() ? TrimJs(lines[i]) : TrimStartJs2(lines[i]))));
            }
        }
        return Cat({S("/*"), a_.Join(hardline(), ps), S("*/")});
    }
    return Cat({S("/*"), a_.ReplaceEndOfLine(c->value), S("*/")});
}

Doc* Printer::PrintLeadingComment(CommentRec* c) {
    Parts parts{PrintComment(c)};
    if (IsBlockComment(c)) {
        parts.push_back(HasNewline(c->end) ? (HasNewline(c->start, true) ? hardline() : line()) : S(" "));
    } else {
        parts.push_back(hardline());
    }
    const int64_t index = SkipNewline(SkipSpaces(c->end));
    if (index != kNoIndex && HasNewline(index)) parts.push_back(hardline());
    return Cat(parts);
}

Printer::TrailingPrinted Printer::PrintTrailingComment(CommentRec* c, const TrailingPrinted* prev) {
    Doc* printed = PrintComment(c);
    const bool is_block = IsBlockComment(c);
    if ((prev && prev->has_line_suffix && !prev->is_block) || HasNewline(c->start, true)) {
        const bool line_before_empty = IsPreviousLineEmpty(c->start);
        return {a_.LineSuffix(Cat({hardline(), line_before_empty ? hardline() : nullptr, printed})), is_block, true};
    }
    if (!is_block || (prev && prev->has_line_suffix)) {
        return {Cat({a_.LineSuffix(Cat({S(" "), printed})), a_.break_parent}), is_block, true};
    }
    return {Cat({S(" "), printed}), is_block, false};
}

Doc* Printer::PrintDanglingComments(bool indent, const std::function<bool(CommentRec*)>& filter, Key marker) {
    auto* cs = CommentsOf(node());
    if (!cs || cs->empty()) return nullptr;
    Parts parts;
    for (CommentRec* c : *cs) {
        if (c->leading || c->trailing || c->marker != marker || (filter && !filter(c))) continue;
        parts.push_back(PrintComment(c));
    }
    if (parts.empty()) return nullptr;
    Doc* doc = a_.Join(hardline(), parts);
    return indent ? Indent(Cat({hardline(), doc})) : doc;
}

Printer::Separate Printer::PrintCommentsSeparately() {
    Separate out{nullptr, nullptr, false};
    auto* cs = CommentsOf(node());
    if (!cs) return out;
    Parts leading, trailing;
    TrailingPrinted prev{};
    bool have_prev = false;
    bool any = false;
    for (CommentRec* c : *cs) {
        if (c->ignored) continue;
        any = true;
        if (c->leading) {
            leading.push_back(PrintLeadingComment(c));
        } else if (c->trailing) {
            prev = PrintTrailingComment(c, have_prev ? &prev : nullptr);
            have_prev = true;
            trailing.push_back(prev.doc);
        }
    }
    if (!any) return out;
    out.any = true;
    out.leading = Cat(leading);
    out.trailing = Cat(trailing);
    return out;
}

Doc* Printer::PrintComments(Doc* doc) {
    Separate s = PrintCommentsSeparately();
    if (!s.any) return doc;
    if (doc && doc->kind == DocKind::Label) {
        return a_.Label(doc->label, Cat({s.leading, doc->contents, s.trailing}));
    }
    return Cat({s.leading, doc, s.trailing});
}

const CommentRec* Printer::UnprintedComment() const {
    for (const CommentRec* c : all_comments_) {
        if (!c->printed && !c->ignored) return c;
    }
    return nullptr;
}

Doc* Printer::PrintIgnored() {
    Node* n = node();
    for (CommentRec* c : all_comments_) {
        if (c->start >= n->start && c->end <= n->end) c->ignored = true;
    }
    return S(text_.substr(n->start, n->end - n->start));
}

}  // namespace jsfmt
