// mep's C# formatter: the rules. A port of Roslyn's C# formatting rules
// (WrappingFormattingRule, SpacingFormattingRule, NewLineUserSettingFormattingRule,
// EndOfFileTokenFormattingRule, StructuredTriviaFormattingRule,
// IndentBlockFormattingRule, SuppressFormattingRule, AnchorIndentationFormattingRule,
// QueryExpressionFormattingRule, TokenBasedFormattingRule, DefaultOperationProvider)
// with the default options, plus FormattingHelpers and the syntax extensions
// they use. ElasticTriviaFormattingRule and IndentUserSettingsFormattingRule do
// nothing on parsed text with default options and are left out. See
// csharp_format_internal.h (Roslyn is MIT-licensed, Copyright (c) .NET
// Foundation and Contributors).

#include "csharp_format_internal.h"

namespace csfmt {

using csharp::Field;
using K = SyntaxKind;

namespace {

// --- Navigation -----------------------------------------------------------------------------------

/** @brief Parent, ascending out of structured trivia (SyntaxNode.GetParent(ascendOutOfTrivia: true)). */
const Node *ParentOut(const Node *n) {
    if (!n) return nullptr;
    if (n->parent) return n->parent;
    if (n->parent_trivia && n->parent_trivia->token) return n->parent_trivia->token->parent;
    return nullptr;
}

/** @brief token.GetAncestor<T>(): the first ancestor (from the token's parent) of a class. */
const Node *GetAncestor(Token t, ClassBit b) {
    for (const Node *n = t ? t->parent : nullptr; n; n = ParentOut(n)) {
        if (Is(n, b)) return n;
    }
    return nullptr;
}

const Node *GetAncestorKind(Token t, K k) {
    for (const Node *n = t ? t->parent : nullptr; n; n = ParentOut(n)) {
        if (n->Kind() == k) return n;
    }
    return nullptr;
}

bool IsLastTokenOfNode(Token t, ClassBit b) {
    const Node *a = GetAncestor(t, b);
    return a && LastToken(a, true) == t;
}

struct Pair {
    Token open = nullptr;
    Token close = nullptr;
};

bool IsValidPair(Pair p) {
    if (!p.open || IsMissing(p.open) || !p.close) return false;
    if (Kind(p.open) == K::OpenBraceToken) return Kind(p.close) == K::CloseBraceToken;
    if (Kind(p.open) == K::OpenBracketToken) return Kind(p.close) == K::CloseBracketToken;
    return false;
}

Pair Braces(const Node *n, Field open = Field::OpenBraceToken, Field close = Field::CloseBraceToken) {
    return {F(n, open), F(n, close)};
}

/** @brief SyntaxNodeExtensions.GetBraces. */
Pair GetBracePair(const Node *n) {
    if (!n) return {};
    K k = n->Kind();
    if (k == K::NamespaceDeclaration || Is(n, kBaseTypeDeclaration) || k == K::AccessorList || k == K::Block ||
        k == K::SwitchStatement || k == K::AnonymousObjectCreationExpression || Is(n, kInitializerExpression) ||
        k == K::SwitchExpression || k == K::PropertyPatternClause) {
        return Braces(n);
    }
    if (k == K::WithExpression) return Braces(F(n, Field::Initializer));
    if (k == K::ImplicitObjectCreationExpression) {
        const Node *init = F(n, Field::Initializer);
        if (init) return Braces(init);
    }
    return {};
}

/** @brief SyntaxNodeExtensions.GetBrackets. */
Pair GetBracketPair(const Node *n) {
    if (!n) return {};
    switch (n->Kind()) {
    case K::ArrayRankSpecifier:
    case K::BracketedArgumentList:
    case K::ImplicitArrayCreationExpression:
    case K::AttributeList:
    case K::BracketedParameterList:
    case K::ListPattern:
    case K::CollectionExpression: return Braces(n, Field::OpenBracketToken, Field::CloseBracketToken);
    default: return {};
    }
}

bool IsEmbeddedStatementOwner(const Node *n) {
    if (!n) return false;
    switch (n->Kind()) {
    case K::DoStatement:
    case K::ElseClause:
    case K::FixedStatement:
    case K::ForEachStatement:
    case K::ForEachVariableStatement:
    case K::ForStatement:
    case K::IfStatement:
    case K::LabeledStatement:
    case K::LockStatement:
    case K::UsingStatement:
    case K::WhileStatement: return true;
    default: return false;
    }
}

const Node *GetEmbeddedStatement(const Node *n) {
    return IsEmbeddedStatementOwner(n) ? F(n, Field::Statement) : nullptr;
}

bool IsEmbeddedStatementOwnerWithCloseParen(const Node *n) {
    if (!n) return false;
    switch (n->Kind()) {
    case K::IfStatement:
    case K::WhileStatement:
    case K::ForStatement:
    case K::ForEachStatement:
    case K::ForEachVariableStatement:
    case K::UsingStatement:
    case K::FixedStatement:
    case K::LockStatement: return true;
    default: return false;
    }
}

bool IsLambdaBodyBlock(const Node *n) {
    return n && n->Kind() == K::Block &&
           (IsParentKind(n, K::SimpleLambdaExpression) || IsParentKind(n, K::ParenthesizedLambdaExpression));
}

bool IsAnonymousMethodBlock(const Node *n) {
    return n && n->Kind() == K::Block && IsParentKind(n, K::AnonymousMethodExpression);
}

bool IsAnyArgumentList(const Node *n) {
    K k = KindOf(n);
    return k == K::ArgumentList || k == K::AttributeArgumentList || k == K::BracketedArgumentList ||
           k == K::TypeArgumentList;
}

bool AreOnSameLine(Token prev, Token cur) {
    for (const csharp::SyntaxTrivia &t : prev->trailing) {
        if (t.Kind() == K::EndOfLineTrivia) return false;
    }
    for (const csharp::SyntaxTrivia &t : cur->leading) {
        if (t.Kind() == K::EndOfLineTrivia) return false;
    }
    return true;
}

// --- FormattingHelpers -----------------------------------------------------------------------------

bool IsOpenParenInParameterList(Token t) { return Kind(t) == K::OpenParenToken && IsKind(Parent(t), K::ParameterList); }
bool IsCloseParenInParameterList(Token t) { return Kind(t) == K::CloseParenToken && IsKind(Parent(t), K::ParameterList); }
bool IsOpenParenInParameterListOfAConversionOperatorDeclaration(Token t) {
    return IsOpenParenInParameterList(t) && IsParentKind(Parent(t), K::ConversionOperatorDeclaration);
}
bool IsOpenParenInParameterListOfAOperationDeclaration(Token t) {
    return IsOpenParenInParameterList(t) && IsParentKind(Parent(t), K::OperatorDeclaration);
}
bool IsOpenParenInParameterListOfParenthesizedLambdaExpression(Token t) {
    return Kind(t) == K::OpenParenToken && IsKind(Parent(t), K::ParameterList) &&
           IsParentKind(Parent(t), K::ParenthesizedLambdaExpression);
}

bool IsTokenInArgumentListOrPositionalPattern(Token t) {
    K pk = KindOf(Parent(t));
    if (pk == K::ArgumentList || pk == K::AttributeArgumentList) return true;
    if (pk == K::PositionalPatternClause && IsParentKind(Parent(t), K::RecursivePattern)) {
        return FirstToken(Parent(Parent(t))) != t;
    }
    return false;
}
bool IsOpenParenInArgumentListOrPositionalPattern(Token t) {
    return Kind(t) == K::OpenParenToken && IsTokenInArgumentListOrPositionalPattern(t);
}
bool IsCloseParenInArgumentListOrPositionalPattern(Token t) {
    return Kind(t) == K::CloseParenToken && IsTokenInArgumentListOrPositionalPattern(t);
}

bool IsColonInTypeBaseList(Token t) { return Kind(t) == K::ColonToken && IsKind(Parent(t), K::BaseList); }
bool IsCommaInArgumentOrParameterList(Token t) {
    K pk = KindOf(Parent(t));
    return Kind(t) == K::CommaToken &&
           (IsAnyArgumentList(Parent(t)) || pk == K::ParameterList || pk == K::FunctionPointerParameterList);
}

bool IsSemicolonInForStatement(Token t) {
    if (Kind(t) != K::SemicolonToken || !IsKind(Parent(t), K::ForStatement)) return false;
    return F(Parent(t), Field::FirstSemicolonToken) == t || F(Parent(t), Field::SecondSemicolonToken) == t;
}

bool IsCloseBraceOfExpression(Token t) {
    if (Kind(t) != K::CloseBraceToken) return false;
    return Is(Parent(t), kExpression) || IsKind(Parent(t), K::PropertyPatternClause);
}

bool IsCommaInEnumDeclaration(Token t) { return Kind(t) == K::CommaToken && IsKind(Parent(t), K::EnumDeclaration); }
bool IsCommaInAnyArgumentsList(Token t) { return Kind(t) == K::CommaToken && IsAnyArgumentList(Parent(t)); }
bool IsCommaInVariableDeclaration(Token t) {
    return Kind(t) == K::CommaToken && IsKind(Parent(t), K::VariableDeclaration);
}
bool IsOpenParenOfParenthesizedExpression(Token t) {
    return IsKind(Parent(t), K::ParenthesizedExpression) && F(Parent(t), Field::OpenParenToken) == t;
}
bool IsCloseParenOfParenthesizedExpression(Token t) {
    return IsKind(Parent(t), K::ParenthesizedExpression) && F(Parent(t), Field::CloseParenToken) == t;
}

bool IsParenInArgumentList(Token t) {
    const Node *p = Parent(t);
    switch (KindOf(p)) {
    case K::SizeOfExpression:
    case K::TypeOfExpression:
    case K::CheckedExpression:
    case K::UncheckedExpression:
    case K::DefaultExpression:
    case K::MakeRefExpression:
    case K::RefTypeExpression:
    case K::RefValueExpression:
    case K::ArgumentList:
    case K::AttributeArgumentList: return F(p, Field::OpenParenToken) == t || F(p, Field::CloseParenToken) == t;
    default: return false;
    }
}

bool IsEqualsTokenInAutoPropertyInitializers(Token t) {
    return Kind(t) == K::EqualsToken && IsKind(Parent(t), K::EqualsValueClause) &&
           IsParentKind(Parent(t), K::PropertyDeclaration);
}

bool IsCloseParenInStatement(Token t) {
    const Node *s = Parent(t);
    if (!Is(s, kStatement)) return false;
    switch (s->Kind()) {
    case K::IfStatement:
    case K::SwitchStatement:
    case K::WhileStatement:
    case K::DoStatement:
    case K::ForStatement:
    case K::ForEachStatement:
    case K::ForEachVariableStatement:
    case K::LockStatement:
    case K::UsingStatement:
    case K::FixedStatement: return F(s, Field::CloseParenToken) == t;
    default: return false;
    }
}

bool IsDotInMemberAccess(Token t) {
    const Node *p = Parent(t);
    if (!p || (p->Kind() != K::SimpleMemberAccessExpression && p->Kind() != K::PointerMemberAccessExpression)) {
        return false;
    }
    return Kind(t) == K::DotToken && F(p, Field::OperatorToken) == t;
}

bool IsDotInMemberAccessOrQualifiedName(Token t) {
    return IsDotInMemberAccess(t) || (Kind(t) == K::DotToken && IsKind(Parent(t), K::QualifiedName));
}

bool IsGenericGreaterThanToken(Token t) {
    if (Kind(t) != K::GreaterThanToken) return false;
    K pk = KindOf(Parent(t));
    return pk == K::TypeParameterList || pk == K::TypeArgumentList;
}

bool IsCommaInInitializerExpression(Token t) {
    return Kind(t) == K::CommaToken &&
           (Is(Parent(t), kInitializerExpression) || IsKind(Parent(t), K::AnonymousObjectCreationExpression));
}
bool IsColonInCasePatternSwitchLabel(Token t) {
    return Kind(t) == K::ColonToken && IsKind(Parent(t), K::CasePatternSwitchLabel);
}
bool IsColonInSwitchExpressionArm(Token t) {
    return Kind(t) == K::ColonToken && IsKind(Parent(t), K::SwitchExpressionArm);
}
bool IsCommaInSwitchExpression(Token t) { return Kind(t) == K::CommaToken && IsKind(Parent(t), K::SwitchExpression); }
bool IsCommaInPropertyPatternClause(Token t) {
    return Kind(t) == K::CommaToken && IsKind(Parent(t), K::PropertyPatternClause);
}
bool IsColonInSwitchLabel(Token t) {
    return Kind(t) == K::ColonToken && Is(Parent(t), kSwitchLabel) && F(Parent(t), Field::ColonToken) == t;
}
bool IsColonInLabeledStatement(Token t) {
    return Kind(t) == K::ColonToken && IsKind(Parent(t), K::LabeledStatement) && F(Parent(t), Field::ColonToken) == t;
}
bool IsNestedQueryExpression(Token t) {
    return Kind(t) == K::InKeyword && IsKind(Parent(t), K::FromClause) &&
           IsKind(F(Parent(t), Field::Expression), K::QueryExpression);
}
bool IsFirstFromKeywordInExpression(Token t) {
    if (Kind(t) != K::FromKeyword) return false;
    const Node *q = Parent(Parent(t));
    return IsKind(q, K::QueryExpression) && FirstToken(q) == t;
}

bool IsInitializerForObjectOrAnonymousObjectCreationExpression(const Node *n) {
    if (Is(n, kInitializerExpression)) {
        const Node *parent = Parent(n);
        if (IsKind(parent, K::AnonymousObjectCreationExpression)) return true;
        if (Is(parent, kBaseObjectCreationExpression)) {
            std::vector<const Node *> exprs = Items(n, Field::Expressions);
            if (exprs.empty()) return true;
            if (exprs[0]->Kind() == K::SimpleAssignmentExpression) return true;
        }
        return false;
    }
    if (IsKind(n, K::AnonymousObjectMemberDeclarator)) return IsParentKind(n, K::AnonymousObjectCreationExpression);
    return false;
}

bool IsInitializerForArrayOrCollectionCreationExpression(const Node *n) {
    if (Is(n, kInitializerExpression)) {
        const Node *parent = Parent(n);
        K pk = KindOf(parent);
        if (pk == K::ArrayCreationExpression || pk == K::ImplicitArrayCreationExpression ||
            pk == K::StackAllocArrayCreationExpression || pk == K::ImplicitStackAllocArrayCreationExpression ||
            pk == K::EqualsValueClause || pk == K::SimpleAssignmentExpression) {
            return true;
        }
        if (Is(parent, kBaseObjectCreationExpression)) return !IsInitializerForObjectOrAnonymousObjectCreationExpression(n);
        return false;
    }
    if (IsKind(n, K::AnonymousObjectMemberDeclarator)) {
        const Node *parent = Parent(n);
        K pk = KindOf(parent);
        return pk == K::ArrayCreationExpression || pk == K::ImplicitArrayCreationExpression ||
               pk == K::EqualsValueClause || Is(parent, kBaseObjectCreationExpression) ||
               pk == K::SimpleAssignmentExpression;
    }
    return false;
}

bool ParenOrBracketContainsNothing(Token t1, Token t2) {
    return (Kind(t1) == K::OpenParenToken && Kind(t2) == K::CloseParenToken) ||
           (Kind(t1) == K::OpenBracketToken && Kind(t2) == K::CloseBracketToken);
}

std::vector<const Node *> GetAttributes(const Node *member) { return Items(member, Field::AttributeLists); }

/** @brief GetFirstAndLastMemberDeclarationTokensAfterAttributes. */
Pair FirstAndLastMemberTokensAfterAttributes(const Tree &tree, const Node *member) {
    std::vector<const Node *> attrs = GetAttributes(member);
    if (attrs.empty()) return {FirstToken(member, true), LastToken(member, true)};
    Token last = LastToken(member, true);
    Token last_attr = LastToken(attrs.back(), true);
    if (last_attr == last) return {};
    return {tree.NextToken(last_attr, true), last};
}

bool IsPlusOrMinusExpression(Token t) {
    if (Kind(t) != K::PlusToken && Kind(t) != K::MinusToken) return false;
    return Is(Parent(t), kPrefixUnaryExpression);
}

bool IsInterpolation(Token t) { return IsKind(Parent(t), K::Interpolation); }

bool IsOpenParenInVarDeconstructionDeclaration(Token t) {
    return Kind(t) == K::OpenParenToken && IsKind(Parent(t), K::ParenthesizedVariableDesignation) &&
           IsParentKind(Parent(t), K::DeclarationExpression);
}

bool IsCommaInTupleExpression(Token t) { return Kind(t) == K::CommaToken && IsKind(Parent(t), K::TupleExpression); }
bool IsCommaInTupleType(Token t) { return Kind(t) == K::CommaToken && IsKind(Parent(t), K::TupleType); }
bool IsCommaInParenthesizedVariableDesignation(Token t) {
    return Kind(t) == K::CommaToken && IsKind(Parent(t), K::ParenthesizedVariableDesignation);
}
bool IsCommaInCollectionExpression(Token t) {
    return Kind(t) == K::CommaToken && IsKind(Parent(t), K::CollectionExpression);
}
bool IsOpenBraceOrCommaOfObjectInitializer(Token t) {
    return (Kind(t) == K::OpenBraceToken || Kind(t) == K::CommaToken) && IsKind(Parent(t), K::ObjectInitializerExpression);
}
bool IsOpenBraceOfAccessorList(Token t) { return Kind(t) == K::OpenBraceToken && IsKind(Parent(t), K::AccessorList); }

bool IsKeyword(Token t) { return csharp::SyntaxFacts::IsKeywordKind(Kind(t)); }

// --- Operation factories -------------------------------------------------------------------------

/** @brief CommonFormattingHelpers.GetSpanIncludingTrailingAndLeadingTriviaOfAdjacentTokens. */
TextSpan AdjacentSpan(const Tree &tree, Token start, Token end) {
    int s;
    Token prev = tree.PreviousToken(start, false);
    if (prev) {
        s = SpanOf(prev).end;
    } else {
        int forward = FullSpanOf(start).start;
        if (forward <= 0) {
            s = 0;
        } else {
            Token t = tree.syntax()->FindToken(forward + 1);
            s = t ? SpanOf(t).end : forward;
        }
    }
    int e;
    Token next = tree.NextToken(end, false);
    e = next ? SpanStart(next) : FullSpanOf(end).end;
    return FromBounds(s, e);
}

void AddIndent(const Tree &tree, std::vector<IndentBlockOperation> &list, Token start, Token end, int delta = 1,
               int option = kRelativePosition) {
    if (!start || !end) return;
    IndentBlockOperation op;
    op.span = AdjacentSpan(tree, start, end);
    op.option = option;
    op.start_token = start;
    op.end_token = end;
    op.delta = delta;
    list.push_back(op);
}

void AddIndentSpan(std::vector<IndentBlockOperation> &list, Token start, Token end, TextSpan span, int delta = 1,
                   int option = kRelativePosition) {
    if (!start || !end) return;
    IndentBlockOperation op;
    op.span = span;
    op.option = option;
    op.start_token = start;
    op.end_token = end;
    op.delta = delta;
    list.push_back(op);
}

void AddRelative(const Tree &tree, std::vector<IndentBlockOperation> &list, Token base, Token start, Token end,
                 int delta, int option) {
    if (!start || !end || !base) return;
    IndentBlockOperation op;
    op.base_token = base;
    op.span = AdjacentSpan(tree, start, end);
    op.option = option;
    op.start_token = start;
    op.end_token = end;
    op.is_relative = true;
    op.delta = delta;
    if (SpanOf(base).end > op.span.start) return;  // Roslyn's contract would throw
    list.push_back(op);
}

void AddSuppress(std::vector<SuppressOperation> &list, Token start, Token end, int option) {
    if (!start || !end) return;
    list.push_back({FromBounds(SpanStart(start), SpanOf(end).end), option, start, end});
}

void AddSuppressWrappingIfOnSingleLine(std::vector<SuppressOperation> &list, Token start, Token end, int extra = 0) {
    AddSuppress(list, start, end, kNoWrappingIfOnSingleLine | extra);
}

void AddSuppressAllIfOnMultipleLine(std::vector<SuppressOperation> &list, Token start, Token end) {
    AddSuppress(list, start, end, kNoSpacingIfOnMultipleLine | kNoWrapping);
}

void AddAnchor(std::vector<AnchorIndentationOperation> &list, Token anchor, Token end) {
    if (!anchor || !end) return;
    list.push_back({anchor, FromBounds(SpanOf(anchor).end, SpanOf(end).end), end});
}

void AddAnchorNode(std::vector<AnchorIndentationOperation> &list, const Node *n) {
    AddAnchor(list, FirstToken(n, true), LastToken(n, true));
}

/** @brief BaseFormattingRule.AddBraceSuppressOperations. */
void AddBraceSuppressOperations(const Tree &tree, std::vector<SuppressOperation> &list, const Node *node) {
    Pair braces = GetBracePair(node);
    if (!IsValidPair(braces)) return;
    Token first = FirstToken(node, true);
    if (Is(node, kMemberDeclaration)) first = FirstAndLastMemberTokensAfterAttributes(tree, node).open;
    if (IsLambdaBodyBlock(node)) {
        first = FirstToken(Parent(node), true);
    } else if (IsKind(node, K::PropertyPatternClause)) {
        first = tree.PreviousToken(first, false);
    }
    AddSuppressWrappingIfOnSingleLine(list, first, braces.close);
    AddSuppressWrappingIfOnSingleLine(list, braces.open, braces.close);
}

void AddStatementExceptBlockSuppressOperations(std::vector<SuppressOperation> &list, const Node *node) {
    if (!Is(node, kStatement) || node->Kind() == K::Block) return;
    AddSuppressWrappingIfOnSingleLine(list, FirstToken(node, true), LastToken(node, true));
}

// --- QueryExpressionFormattingRule ---------------------------------------------------------------

void QuerySuppress(std::vector<SuppressOperation> &list, const Node *node) {
    if (IsKind(node, K::QueryExpression)) {
        AddSuppressWrappingIfOnSingleLine(list, FirstToken(node, true), LastToken(node, true));
    }
}

void QueryFromClauseIndent(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *from) {
    if (IsMissing(F(from, Field::InKeyword))) return;
    const Node *expr = F(from, Field::Expression);
    AddRelative(tree, list, F(from, Field::FromKeyword), FirstToken(expr, true), LastToken(expr, true), 1,
                kRelativePosition);
}

void QueryIndent(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *node) {
    if (!IsKind(node, K::QueryExpression)) return;
    const Node *from = F(node, Field::FromClause);
    QueryFromClauseIndent(tree, list, from);
    const Node *body = F(node, Field::Body);
    for (const Node *clause : Items(body, Field::Clauses)) {
        if (clause->Kind() == K::FromClause) QueryFromClauseIndent(tree, list, clause);
    }
    Token base = FirstToken(node, true);
    Token end = LastToken(node, true);
    if (!IsMissing(base) && base != end) {
        Token last = LastToken(F(from, Field::Expression), true);
        Token start = IsMissing(last) ? tree.NextToken(base, true) : FirstToken(body, true);
        AddRelative(tree, list, base, start, end, 0, kRelativePosition);
    }
}

void QueryAnchor(std::vector<AnchorIndentationOperation> &list, const Node *node) {
    if (Is(node, kQueryClause) || Is(node, kSelectOrGroupClause)) {
        AddAnchor(list, FirstToken(node, true), LastToken(node, true));
    } else if (IsKind(node, K::QueryContinuation)) {
        AddAnchor(list, F(node, Field::IntoKeyword), LastToken(node, true));
    }
}

// --- SuppressFormattingRule ----------------------------------------------------------------------

bool IsFormatDirective(const Tree &tree, const Node *d, K disable_or_restore) {
    if (!IsKind(d, K::PragmaWarningDirectiveTrivia)) return false;
    if (Kind(F(d, Field::DisableOrRestoreKeyword)) != disable_or_restore) return false;
    for (const Node *code : Items(d, Field::ErrorCodes)) {
        if (code->Kind() != K::IdentifierName) continue;
        Token id = F(code, Field::Identifier);
        U16View v = id ? U16View(id->green->value_text) : U16View();
        (void)tree;
        if (v == u"format" || v == u"IDE0055") return true;
    }
    return false;
}

void AddFormatSuppressOperations(const Tree &tree, std::vector<SuppressOperation> &list, const Node *node) {
    if (!node->green->ContainsDirectives()) return;
    for (const Node *child : node->children) {
        if (!child->IsToken()) continue;
        auto process = [&](const std::vector<csharp::SyntaxTrivia> &trivia) {
            for (const csharp::SyntaxTrivia &t : trivia) {
                if (!t.structure || !IsFormatDirective(tree, t.structure, K::DisableKeyword)) continue;
                Token start = LastToken(t.structure, true);
                // GetNextDirective: the next directive in the file that restores formatting.
                Token end = nullptr;
                bool after = false;
                for (Token tok : tree.Tokens()) {
                    for (const csharp::SyntaxTrivia &x : tok->leading) {
                        if (&x == &t) {
                            after = true;
                            continue;
                        }
                        if (after && x.structure && csharp::IsDirectiveTriviaKind(x.Kind()) &&
                            IsFormatDirective(tree, x.structure, K::RestoreKeyword)) {
                            end = FirstToken(x.structure, true);
                            break;
                        }
                    }
                    if (end) break;
                    for (const csharp::SyntaxTrivia &x : tok->trailing) {
                        if (&x == &t) after = true;
                    }
                }
                if (!end) end = tree.Tokens().back();
                list.push_back({FromBounds(SpanOf(start).end, SpanStart(end)), kDisableFormatting, start, end});
            }
        };
        process(child->leading);
        process(child->trailing);
    }
}

void AddInitializerSuppressItems(std::vector<SuppressOperation> &list, const Node *parent,
                                 const std::vector<const Node *> &items) {
    AddSuppressWrappingIfOnSingleLine(list, FirstToken(parent, true), LastToken(parent, true));
    for (const Node *item : items) {
        Token first = FirstToken(item, true);
        Token last = LastToken(item, true);
        if (first != last) AddSuppressWrappingIfOnSingleLine(list, first, last);
    }
}

void AddInitializerSuppressOperations(const Tree &tree, std::vector<SuppressOperation> &list, const Node *node) {
    if (IsInitializerForArrayOrCollectionCreationExpression(node)) {
        AddSuppressAllIfOnMultipleLine(list, tree.PreviousToken(F(node, Field::OpenBraceToken), true),
                                       F(node, Field::CloseBraceToken));
        return;
    }
    if (IsKind(node, K::CollectionExpression)) {
        AddSuppressAllIfOnMultipleLine(list, F(node, Field::OpenBracketToken), F(node, Field::CloseBracketToken));
        return;
    }
    const Node *initializer = nullptr;
    K k = node->Kind();
    if (k == K::ObjectCreationExpression || k == K::ArrayCreationExpression || k == K::ImplicitArrayCreationExpression) {
        initializer = F(node, Field::Initializer);
    }
    if (initializer && Parent(initializer)) {
        AddInitializerSuppressItems(list, Parent(initializer), Items(initializer, Field::Expressions));
        return;
    }
    if (k == K::AnonymousObjectCreationExpression) {
        AddInitializerSuppressItems(list, node, Items(node, Field::Initializers));
    }
}

void AddSpecificNodesSuppressOperations(const Tree &tree, std::vector<SuppressOperation> &list, const Node *node) {
    K k = node->Kind();
    if (k == K::IfStatement) {
        AddSuppressWrappingIfOnSingleLine(list, F(node, Field::IfKeyword), LastToken(F(node, Field::Statement), true));
        const Node *els = F(node, Field::Else);
        if (els) {
            AddSuppressWrappingIfOnSingleLine(list, F(els, Field::ElseKeyword), LastToken(F(els, Field::Statement), true));
        }
        return;
    }
    if (k == K::RecursivePattern) {
        const Node *positional = F(node, Field::PositionalPatternClause);
        const Node *property = F(node, Field::PropertyPatternClause);
        if (positional) {
            Token open = F(positional, Field::OpenParenToken);
            Token close = F(positional, Field::CloseParenToken);
            AddSuppressWrappingIfOnSingleLine(list, open, close);
            if (property) AddSuppressWrappingIfOnSingleLine(list, open, LastToken(property));
        } else if (property) {
            AddSuppressWrappingIfOnSingleLine(list, F(property, Field::OpenBraceToken), F(property, Field::CloseBraceToken));
        }
        return;
    }
    if (k == K::SwitchExpressionArm || k == K::SwitchExpression || k == K::CasePatternSwitchLabel) {
        AddSuppressWrappingIfOnSingleLine(list, FirstToken(node), LastToken(node));
        return;
    }
    if (k == K::IsPatternExpression) {
        AddSuppressWrappingIfOnSingleLine(list, FirstToken(node), LastToken(node));
        const Node *pattern = F(node, Field::Pattern);
        if (IsKind(pattern, K::RecursivePattern)) {
            const Node *ppc = F(pattern, Field::PropertyPatternClause);
            if (ppc) AddSuppressWrappingIfOnSingleLine(list, F(node, Field::IsKeyword), LastToken(ppc));
        }
        return;
    }
    if (k == K::BaseConstructorInitializer || k == K::ThisConstructorInitializer) {
        const Node *ctor = Parent(node);
        if (IsKind(ctor, K::ConstructorDeclaration)) {
            const Node *body = F(ctor, Field::Body);
            if (body) AddSuppressWrappingIfOnSingleLine(list, F(node, Field::ColonToken), F(body, Field::CloseBraceToken));
        }
        return;
    }
    if (k == K::DoStatement) {
        AddSuppressWrappingIfOnSingleLine(list, FirstToken(node, true), LastToken(F(node, Field::Statement), true));
        return;
    }
    if (Is(node, kMemberDeclaration)) {
        Pair p = FirstAndLastMemberTokensAfterAttributes(tree, node);
        AddSuppressWrappingIfOnSingleLine(list, p.open, p.close);
        std::vector<const Node *> attributes = GetAttributes(node);
        Token end = LastToken(node, true);
        for (const Node *a : attributes) AddSuppressWrappingIfOnSingleLine(list, FirstToken(a, true), end);
        if (k == K::PropertyDeclaration && F(node, Field::Initializer) && F(node, Field::AccessorList)) {
            AddSuppressWrappingIfOnSingleLine(list, p.open, LastToken(F(node, Field::AccessorList)));
        }
        return;
    }
    if (Is(node, kAccessorDeclaration)) {
        AddSuppressWrappingIfOnSingleLine(list, F(node, Field::Keyword), LastToken(node, true));
        return;
    }
    if (k == K::SwitchSection) {
        std::vector<const Node *> labels = Items(node, Field::Labels);
        if (labels.size() < 2) {
            AddSuppressWrappingIfOnSingleLine(list, FirstToken(node, true), LastToken(node));
        } else {
            for (size_t i = 0; i + 1 < labels.size(); i++) {
                AddSuppressWrappingIfOnSingleLine(list, FirstToken(labels[i], true), LastToken(labels[i]));
            }
            AddSuppressWrappingIfOnSingleLine(list, FirstToken(labels.back(), true), LastToken(node));
        }
        return;
    }
    if (Is(node, kAnonymousFunctionExpression) || k == K::LocalFunctionStatement) {
        AddSuppressWrappingIfOnSingleLine(list, FirstToken(node, true), LastToken(node, true), kIgnoreElasticWrapping);
        return;
    }
    if (k == K::Parameter) {
        std::vector<const Node *> attrs = Items(node, Field::AttributeLists);
        if (!attrs.empty()) AddSuppressAllIfOnMultipleLine(list, F(attrs[0], Field::OpenBracketToken), LastToken(node));
    }
    if (k == K::TryStatement) {
        Token try_kw = F(node, Field::TryKeyword);
        const Node *block = F(node, Field::Block);
        if (!IsMissing(try_kw) && block && !IsMissing(F(block, Field::CloseBraceToken))) {
            AddSuppressWrappingIfOnSingleLine(list, try_kw, F(block, Field::CloseBraceToken));
        }
    }
    if (k == K::CatchClause) {
        Token kw = F(node, Field::CatchKeyword);
        const Node *block = F(node, Field::Block);
        if (!IsMissing(kw) && block && !IsMissing(F(block, Field::CloseBraceToken))) {
            AddSuppressWrappingIfOnSingleLine(list, kw, F(block, Field::CloseBraceToken));
        }
    }
    if (k == K::FinallyClause) {
        Token kw = F(node, Field::FinallyKeyword);
        const Node *block = F(node, Field::Block);
        if (!IsMissing(kw) && block && !IsMissing(F(block, Field::CloseBraceToken))) {
            AddSuppressWrappingIfOnSingleLine(list, kw, F(block, Field::CloseBraceToken));
        }
    }
    if (k == K::InterpolatedStringExpression) {
        AddSuppressWrappingIfOnSingleLine(list, F(node, Field::StringStartToken), F(node, Field::StringEndToken));
    }
}

void SuppressFormattingRuleOps(const Tree &tree, std::vector<SuppressOperation> &list, const Node *node) {
    AddFormatSuppressOperations(tree, list, node);
    AddInitializerSuppressOperations(tree, list, node);
    AddBraceSuppressOperations(tree, list, node);
    AddStatementExceptBlockSuppressOperations(list, node);
    AddSpecificNodesSuppressOperations(tree, list, node);
}

// --- WrappingFormattingRule ----------------------------------------------------------------------

void WrappingRuleOps(const Tree &tree, std::vector<SuppressOperation> &list, const Node *node) {
    AddBraceSuppressOperations(tree, list, node);
    AddStatementExceptBlockSuppressOperations(list, node);
    // AddSpecificNodesSuppressOperations / GetSpecificNodeSuppressionTokenRange.
    Token first = nullptr;
    Token last = nullptr;
    const Node *embedded = GetEmbeddedStatement(node);
    if (embedded) {
        Token first_of_embedded = FirstToken(embedded, true);
        first = tree.PreviousToken(first_of_embedded, true);
        last = embedded->Kind() == K::Block ? LastToken(embedded, true) : first_of_embedded;
    } else if (IsKind(node, K::SwitchSection)) {
        first = FirstToken(node, true);
        last = LastToken(node, true);
    } else if (IsKind(node, K::AnonymousMethodExpression)) {
        first = F(node, Field::DelegateKeyword);
        last = LastToken(node, true);
    }
    if (first || last) AddSuppressWrappingIfOnSingleLine(list, first, last);
}

// --- AnchorIndentationFormattingRule -------------------------------------------------------------

void AnchorRuleOps(std::vector<AnchorIndentationOperation> &list, const Node *node) {
    K k = node->Kind();
    if (k == K::SimpleLambdaExpression || k == K::ParenthesizedLambdaExpression || k == K::AnonymousMethodExpression) {
        AddAnchorNode(list, node);
        return;
    }
    if (k == K::Block) {
        const Node *parent = Parent(node);
        if (!parent || parent->Kind() == K::Block) {
            AddAnchorNode(list, node);
        } else {
            AddAnchor(list, FirstToken(parent, true), LastToken(node, true));
        }
        return;
    }
    if (Is(node, kStatement) || k == K::UsingDirective || k == K::NamespaceDeclaration ||
        Is(node, kBaseTypeDeclaration) || Is(node, kMemberDeclaration) || Is(node, kAccessorDeclaration) ||
        k == K::SwitchExpressionArm) {
        AddAnchorNode(list, node);
    }
}

// --- IndentBlockFormattingRule -------------------------------------------------------------------

void SetAlignmentNode(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *base_node,
                      const Node *body) {
    if (!base_node || !body) return;
    AddRelative(tree, list, FirstToken(base_node, true), FirstToken(body, true), LastToken(body, true), 0,
                kRelativeToFirstTokenOnBaseTokenLine);
}

void AddAlignmentBlockOperation(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *node) {
    K k = node->Kind();
    auto set = [&](Token base, const Node *init) {
        if (!init) return;
        AddRelative(tree, list, base, F(init, Field::OpenBraceToken), F(init, Field::CloseBraceToken), 0,
                    kRelativeToFirstTokenOnBaseTokenLine);
    };
    switch (k) {
    case K::SimpleLambdaExpression:
    case K::ParenthesizedLambdaExpression: {
        // LambdaExpressionSyntax.Body: the block, else the expression body.
        const Node *body = F(node, Field::Block);
        SetAlignmentNode(tree, list, node, body ? body : F(node, Field::ExpressionBody));
        return;
    }
    case K::AnonymousMethodExpression: SetAlignmentNode(tree, list, node, F(node, Field::Block)); return;
    default: break;
    }
    if (Is(node, kBaseObjectCreationExpression)) {
        const Node *init = F(node, Field::Initializer);
        if (init) SetAlignmentNode(tree, list, node, init);
        return;
    }
    switch (k) {
    case K::AnonymousObjectCreationExpression:
        AddRelative(tree, list, F(node, Field::NewKeyword), F(node, Field::OpenBraceToken), F(node, Field::CloseBraceToken),
                    0, kRelativeToFirstTokenOnBaseTokenLine);
        return;
    case K::ArrayCreationExpression:
    case K::ImplicitArrayCreationExpression: set(F(node, Field::NewKeyword), F(node, Field::Initializer)); return;
    case K::StackAllocArrayCreationExpression:
    case K::ImplicitStackAllocArrayCreationExpression:
        set(F(node, Field::StackAllocKeyword), F(node, Field::Initializer));
        return;
    case K::SwitchExpression:
        AddRelative(tree, list, FirstToken(node), F(node, Field::OpenBraceToken), F(node, Field::CloseBraceToken), 0,
                    kRelativeToFirstTokenOnBaseTokenLine);
        return;
    case K::WithExpression: set(FirstToken(node), F(node, Field::Initializer)); return;
    case K::PropertyPatternClause: {
        const Node *rp = Parent(node);
        if (IsKind(rp, K::RecursivePattern) && Parent(rp)) {
            Token base = FirstToken(Parent(rp));
            if (base == F(node, Field::OpenBraceToken)) return;
            AddRelative(tree, list, base, F(node, Field::OpenBraceToken), F(node, Field::CloseBraceToken), 0,
                        kRelativeToFirstTokenOnBaseTokenLine | kIndentIfConditionOfAnchorToken);
        }
        return;
    }
    default: return;
    }
}

void AddBlockIndentationOperation(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *node) {
    Pair braces = GetBracePair(node);
    if (!IsValidPair(braces)) return;
    auto relative_to_first = [&]() {
        AddRelative(tree, list, braces.open, tree.NextToken(braces.open, true), braces.close, 0,
                    kRelativeToFirstTokenOnBaseTokenLine);
    };
    K k = node->Kind();
    if (IsLambdaBodyBlock(node) || IsAnonymousMethodBlock(node) || k == K::PropertyPatternClause ||
        k == K::SwitchExpression) {
        relative_to_first();
    }
    if (k == K::ArrayInitializerExpression && IsParentKind(node, K::ArrayCreationExpression)) relative_to_first();
    AddIndent(tree, list, tree.NextToken(braces.open, true), tree.PreviousToken(braces.close, true));
}

bool IsWithinSwitchExpressionArmPattern(const Node *node) {
    for (const Node *cur = node; cur; cur = cur->parent) {
        if (IsKind(cur->parent, K::SwitchExpressionArm) && F(cur->parent, Field::Pattern) == cur) return true;
        if (cur->Kind() == K::SwitchExpressionArm) return false;
    }
    return false;
}

void AddBracketIndentationOperation(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *node) {
    // (Roslyn checks the switch-arm pattern first; both checks only return early, and this order keeps a
    // deep tree from walking its ancestors at every node.)
    Pair brackets = GetBracketPair(node);
    if (!IsValidPair(brackets)) return;
    if (IsWithinSwitchExpressionArmPattern(node)) return;
    if (node->parent && (node->Kind() == K::ListPattern || node->Kind() == K::CollectionExpression)) {
        AddIndent(tree, list, tree.NextToken(brackets.open, true), tree.PreviousToken(brackets.close, true));
        Token first = FirstToken(node, true);
        Token last = LastToken(node, true);
        Token base = tree.PreviousToken(first, true);
        AddRelative(tree, list, base, first, last, 0, kRelativeToFirstTokenOnBaseTokenLine);
    }
}

void AddParenthesizedPatternIndentationOperation(const Tree &tree, std::vector<IndentBlockOperation> &list,
                                                 const Node *node) {
    K k = node->Kind();
    if (k != K::ParenthesizedPattern && k != K::PositionalPatternClause) return;
    Token open = F(node, Field::OpenParenToken);
    Token close = F(node, Field::CloseParenToken);
    if (!open || !close) return;
    AddIndentSpan(list, tree.NextToken(open, true), tree.PreviousToken(close, true),
                  FromBounds(SpanOf(open).end, SpanStart(close)));
}

void AddLabelIndentationOperation(std::vector<IndentBlockOperation> &list, const Node *node) {
    if (!IsKind(node, K::LabeledStatement)) return;
    Token id = F(node, Field::Identifier);
    Token colon = F(node, Field::ColonToken);
    if (!id || !colon) return;
    AddIndentSpan(list, id, colon, FromBounds(SpanStart(id), SpanOf(colon).end), -1);
}

void AddSwitchIndentationOperation(const Tree &tree, std::vector<IndentBlockOperation> &list, const Node *node) {
    if (!IsKind(node, K::SwitchSection)) return;
    std::vector<const Node *> labels = Items(node, Field::Labels);
    std::vector<const Node *> statements = Items(node, Field::Statements);
    if (labels.empty() && statements.empty()) return;
    const Node *sw = Parent(node);
    std::vector<const Node *> sections = Items(sw, Field::Sections);
    bool last_section = !sections.empty() && sections.back() == node;
    if (statements.empty()) {
        Token last_label = LastToken(labels.back(), true);
        Token next = tree.NextToken(last_label, true);
        TextSpan span = last_section ? FromBounds(FullSpanOf(last_label).end, SpanStart(next))
                                     : FromBounds(FullSpanOf(last_label).end, FullSpanOf(last_label).end);
        AddIndentSpan(list, last_label, last_label, span);
        return;
    }
    Token start = FirstToken(statements.front(), true);
    Token end = LastToken(statements.back(), true);
    TextSpan span = AdjacentSpan(tree, start, end);
    if (!last_section) span = FromBounds(span.start, FullSpanOf(end).end);
    AddIndentSpan(list, start, end, span);
}

void AddEmbeddedStatementsIndentationOperation(const Tree &tree, std::vector<IndentBlockOperation> &list,
                                               const Node *node) {
    K k = node->Kind();
    if (k == K::ElseClause) {
        const Node *stmt = F(node, Field::Statement);
        if (IsKind(stmt, K::IfStatement) && AreOnSameLine(F(node, Field::ElseKeyword), F(stmt, Field::IfKeyword))) {
            return;
        }
    }
    if ((k == K::UsingStatement && IsKind(F(node, Field::Statement), K::UsingStatement)) ||
        (k == K::FixedStatement && IsKind(F(node, Field::Statement), K::FixedStatement))) {
        return;
    }
    if (k == K::LabeledStatement) return;
    const Node *embedded = GetEmbeddedStatement(node);
    if (!embedded || embedded->Kind() == K::Block) return;
    Token first = FirstToken(embedded, true);
    Token last = LastToken(embedded, true);
    if (IsEmbeddedStatementOwner(embedded)) {
        Token before = tree.PreviousToken(first, true);
        if (before && AreOnSameLine(before, first)) return;
    }
    if (IsMissing(last)) {
        AddIndent(tree, list, first, last);
    } else {
        AddIndentSpan(list, first, last, FromBounds(FullSpanOf(first).start, FullSpanOf(last).end));
    }
}

void AddTypeParameterConstraintClauseOperation(const Tree &tree, std::vector<IndentBlockOperation> &list,
                                               const Node *node) {
    if (!IsKind(node, K::TypeParameterConstraintClause) || !Parent(node)) return;
    AddRelative(tree, list, FirstToken(Parent(node)), FirstToken(node), LastToken(node), 1, kRelativePosition);
}

// --- Token rules -------------------------------------------------------------------------------------

/** @brief TokenBasedFormattingRule.GetAdjustNewLinesOperation (its `next` is the default: none). */
NewLinesOp TokenBasedNewLines(const Tree &tree, Token prev, Token cur) {
    using O = NewLinesOption;
    K ck = Kind(cur);
    K pk = Kind(prev);
    if (ck == K::OpenBraceToken) {
        if (IsInterpolation(cur)) return {};
        if (!IsOpenParenOfParenthesizedExpression(prev)) return Lines(1, O::PreserveLines);
    } else if (ck == K::CloseBraceToken) {
        if (IsInterpolation(cur)) return {};
        return Lines(1, O::PreserveLines);
    }
    if (pk == K::CloseBraceToken && ck == K::WhileKeyword) return Lines(0, O::PreserveLines);
    if (pk == K::CloseBraceToken) {
        if (IsInterpolation(prev)) return {};
        if (!IsCloseBraceOfExpression(prev)) {
            if (ck != K::SemicolonToken && !IsCloseParenOfParenthesizedExpression(cur) &&
                !IsCommaInInitializerExpression(cur) && !IsCommaInAnyArgumentsList(cur) &&
                !IsCommaInVariableDeclaration(cur) && !IsCommaInTupleExpression(cur) &&
                !IsCommaInCollectionExpression(cur) && !IsParenInArgumentList(cur) && !IsDotInMemberAccess(cur) &&
                !IsCloseParenInStatement(cur) && !IsEqualsTokenInAutoPropertyInitializers(cur) &&
                !IsColonInCasePatternSwitchLabel(cur) && !IsColonInSwitchExpressionArm(cur)) {
                return Lines(1, O::PreserveLines);
            }
        }
    } else if (pk == K::OpenBraceToken) {
        if (IsInterpolation(prev)) return {};
        return Lines(1, O::PreserveLines);
    }
    if (IsCommaInInitializerExpression(prev)) return Lines(1, O::PreserveLines);
    if (IsCommaInCollectionExpression(prev)) return Lines(1, O::PreserveLines);
    if (IsCommaInSwitchExpression(prev)) return Lines(1, O::PreserveLines);
    if (IsCommaInPropertyPatternClause(prev)) return Lines(0, O::PreserveLines);
    if (pk == K::ElseKeyword) {
        bool else_if_same_line = ck == K::IfKeyword && AreOnSameLine(prev, cur);
        if (!else_if_same_line) return Lines(1, O::PreserveLines);
    }
    if (IsCommaInEnumDeclaration(prev)) return Lines(0, O::PreserveLines);
    if (IsColonInSwitchLabel(prev) || IsColonInLabeledStatement(prev)) return Lines(1, O::PreserveLines);
    if (pk == K::CloseParenToken && IsEmbeddedStatementOwnerWithCloseParen(Parent(prev))) {
        return Lines(1, O::PreserveLines);
    }
    if (pk == K::DoKeyword && IsKind(Parent(prev), K::DoStatement)) return Lines(1, O::PreserveLines);
    if (IsSemicolonInForStatement(prev)) return {};
    if (pk == K::SemicolonToken && (ck == K::CaseKeyword || ck == K::DefaultKeyword || ck == K::ElseKeyword)) {
        return Lines(1, O::PreserveLines);
    }
    if (pk == K::SemicolonToken) {
        if (IsKind(Parent(prev), K::UsingDirective)) return Lines(1, O::PreserveLines);
        if (IsKind(Parent(prev), K::FileScopedNamespaceDeclaration)) return Lines(2, O::PreserveLines);
        return Lines(0, O::PreserveLines);
    }
    if (ck == K::SemicolonToken && IsKind(Parent(cur), K::EmptyStatement) &&
        IsLastTokenOfNode(tree.PreviousToken(cur, false), kStatement)) {
        return Lines(1, O::ForceLinesIfOnSingleLine);
    }
    if (pk == K::CloseBracketToken && Is(Parent(prev), kAttributeList)) {
        const Node *owner = Parent(Parent(prev));
        if (IsKind(owner, K::CompilationUnit) || Is(owner, kMemberDeclaration) || Is(owner, kAccessorDeclaration)) {
            return Lines(1, O::PreserveLines);
        }
        return Lines(0, O::PreserveLines);
    }
    return {};
}

/** @brief TokenBasedFormattingRule.GetAdjustSpacesOperation, then DefaultOperationProvider. */
SpacesOp TokenBasedSpaces(Token prev, Token cur) {
    using O = SpacesOption;
    K ck = Kind(cur);
    K pk = Kind(prev);
    const Node *pp = Parent(prev);
    const Node *cp = Parent(cur);
    if (ck == K::SemicolonToken) {
        if (pk == K::SemicolonToken) return Spaces(1, O::ForceSpacesIfOnSingleLine);
        if (pk == K::CloseParenToken && IsEmbeddedStatementOwnerWithCloseParen(pp)) {
            return Spaces(1, O::ForceSpacesIfOnSingleLine);
        }
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (pk == K::OmittedArraySizeExpressionToken || pk == K::OmittedTypeArgumentToken ||
        ck == K::OmittedArraySizeExpressionToken || ck == K::OmittedTypeArgumentToken) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (pk == K::CloseBracketToken && IsKind(pp, K::AttributeList) && IsParentKind(pp, K::Parameter)) {
        return Spaces(ck == K::OpenBracketToken ? 0 : 1, O::ForceSpacesIfOnSingleLine);
    }
    if (ck == K::OpenParenToken && pk == K::ThisKeyword) return Spaces(1, O::ForceSpacesIfOnSingleLine);
    if (pk == K::NewKeyword) {
        K ppk = KindOf(pp);
        int s = (ppk == K::ConstructorConstraint || ppk == K::ImplicitObjectCreationExpression ||
                 ppk == K::ImplicitArrayCreationExpression)
                    ? 0
                    : 1;
        return Spaces(s, O::ForceSpacesIfOnSingleLine);
    }
    if (ck == K::OpenParenToken) {
        if (pk == K::IdentifierToken || pk == K::DefaultKeyword || pk == K::BaseKeyword || pk == K::ThisKeyword ||
            pk == K::ExtensionKeyword || IsGenericGreaterThanToken(prev) || IsParenInArgumentList(cur)) {
            return Spaces(0, O::ForceSpacesIfOnSingleLine);
        }
    }
    if (ParenOrBracketContainsNothing(prev, cur)) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    if (pk == K::CommaToken && ck == K::OpenBracketToken && Is(cp, kAttributeList)) {
        return Spaces(1, O::ForceSpacesIfOnSingleLine);
    }
    if (pk == K::CloseBracketToken && Is(pp, kAttributeList)) return Spaces(0, O::PreserveSpaces);
    switch (ck) {
    case K::CloseParenToken:
    case K::CloseBracketToken:
    case K::CommaToken:
    case K::DotToken:
    case K::MinusGreaterThanToken: return Spaces(0, O::ForceSpacesIfOnSingleLine);
    default: break;
    }
    if (ck == K::OpenBracketToken && KindOf(cp) != K::CollectionExpression && KindOf(cp) != K::AttributeList &&
        !IsOpenBraceOrCommaOfObjectInitializer(prev)) {
        if (IsOpenBraceOfAccessorList(prev) || IsLastTokenOfNode(prev, kAccessorDeclaration)) {
            return Spaces(1, O::ForceSpacesIfOnSingleLine);
        }
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (ck == K::ColonToken) {
        K cpk = KindOf(cp);
        if (cpk == K::AttributeTargetSpecifier || cpk == K::CaseSwitchLabel || cpk == K::CasePatternSwitchLabel ||
            cpk == K::DefaultSwitchLabel || cpk == K::ExpressionColon || cpk == K::LabeledStatement ||
            cpk == K::NameColon || cpk == K::SwitchExpressionArm) {
            return Spaces(0, O::ForceSpacesIfOnSingleLine);
        }
    }
    if (IsKind(pp, K::CastExpression) && pk == K::CloseParenToken) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    {
        K ppk = KindOf(pp);
        if (ppk == K::TypeArgumentList || ppk == K::TypeParameterList || ppk == K::FunctionPointerType) {
            if (pk == K::LessThanToken) return Spaces(0, O::ForceSpacesIfOnSingleLine);
            if (pk == K::GreaterThanToken && ck == K::GreaterThanToken) return Spaces(0, O::ForceSpacesIfOnSingleLine);
        }
    }
    if ((ck == K::LessThanToken || ck == K::GreaterThanToken) &&
        (KindOf(cp) == K::TypeArgumentList || KindOf(cp) == K::TypeParameterList)) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if ((pk == K::PlusPlusToken || pk == K::MinusMinusToken) && Is(pp, kPrefixUnaryExpression)) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if ((ck == K::PlusPlusToken || ck == K::MinusMinusToken) && Is(cp, kPostfixUnaryExpression)) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (ck == K::QuestionToken && IsKind(cp, K::ConditionalAccessExpression)) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    if (ck == K::QuestionToken && (KindOf(cp) == K::NullableType || KindOf(cp) == K::ClassConstraint)) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (ck == K::QuestionToken && pp && IsParentKind(pp, K::ArrayType)) return Spaces(0, O::ForceSpaces);
    if (ck == K::ExclamationToken && IsKind(cp, K::SuppressNullableWarningExpression)) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (ck == K::AsteriskToken && Is(cp, kPointerType)) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    if (pk == K::AsteriskToken && Is(pp, kPrefixUnaryExpression)) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    switch (pk) {
    case K::OpenParenToken:
    case K::OpenBracketToken:
    case K::DotToken:
    case K::MinusGreaterThanToken: return Spaces(0, O::ForceSpacesIfOnSingleLine);
    case K::CloseParenToken:
    case K::CloseBracketToken: return Spaces(pk == ck ? 0 : 1, O::ForceSpacesIfOnSingleLine);
    default: break;
    }
    if (IsPlusOrMinusExpression(prev) && !IsPlusOrMinusExpression(cur)) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    if (IsPlusOrMinusExpression(prev) && IsPlusOrMinusExpression(cur) && pk != ck) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (pk == K::ExclamationToken && !IsKind(pp, K::SuppressNullableWarningExpression)) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (pk == K::TildeToken && (Is(pp, kPrefixUnaryExpression) || Is(pp, kDestructorDeclaration))) {
        return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    if (pk == K::AmpersandToken && Is(pp, kPrefixUnaryExpression)) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    if (pk == K::ColonColonToken || ck == K::ColonColonToken) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    // DefaultOperationProvider.
    return Spaces(ck == K::EndOfFileToken ? 0 : 1, O::DefaultSpacesIfOnSingleLine);
}

bool IsFunctionLikeKeywordExpressionKind(K k) {
    return k == K::TypeOfExpression || k == K::DefaultExpression || k == K::SizeOfExpression;
}

bool IsControlFlowLikeKeywordStatementKind(K k) {
    switch (k) {
    case K::IfStatement:
    case K::WhileStatement:
    case K::SwitchStatement:
    case K::ForStatement:
    case K::ForEachStatement:
    case K::ForEachVariableStatement:
    case K::DoStatement:
    case K::CatchDeclaration:
    case K::UsingStatement:
    case K::LockStatement:
    case K::FixedStatement:
    case K::CatchFilterClause: return true;
    default: return false;
    }
}

bool HasFormattableBracketParent(Token t) {
    switch (KindOf(Parent(t))) {
    case K::ArrayRankSpecifier:
    case K::BracketedArgumentList:
    case K::BracketedParameterList:
    case K::ImplicitArrayCreationExpression:
    case K::ListPattern:
    case K::CollectionExpression: return true;
    default: return false;
    }
}

bool IsEmptyForStatement(const Node *f) {
    return Items(f, Field::Initializers).empty() && !F(f, Field::Declaration) && !F(f, Field::Condition) &&
           Items(f, Field::Incrementors).empty();
}

/** @brief The space chain after SpacingFormattingRule: EOF, structured trivia, token based, default. */
SpacesOp SpacesAfterSpacingRule(Token prev, Token cur) {
    using O = SpacesOption;
    // EndOfFileTokenFormattingRule.
    if (Kind(cur) == K::EndOfFileToken) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    // StructuredTriviaFormattingRule.
    if (Is(Parent(prev), kStructuredTrivia) || Is(Parent(cur), kStructuredTrivia)) {
        if (Kind(prev) == K::HashToken && csharp::SyntaxFacts::IsPreprocessorKeyword(Kind(cur))) {
            return Spaces(0, O::ForceSpacesIfOnSingleLine);
        }
        if (Kind(prev) == K::RegionKeyword && Kind(cur) == K::EndOfDirectiveToken) return Spaces(0, O::PreserveSpaces);
        if (Kind(cur) == K::EndOfDirectiveToken) return Spaces(0, O::ForceSpacesIfOnSingleLine);
    }
    return TokenBasedSpaces(prev, cur);
}

/** @brief SpacingFormattingRule.GetAdjustSpacesOperation (default options). */
SpacesOp SpacingRule(Token prev, Token cur) {
    using O = SpacesOption;
    const SpacesOp zero = Spaces(0, O::ForceSpacesIfOnSingleLine);
    const SpacesOp one = Spaces(1, O::ForceSpacesIfOnSingleLine);
    K pk = Kind(prev);
    K ck = Kind(cur);
    const Node *pp = Parent(prev);
    const Node *cp = Parent(cur);
    K ppk = KindOf(pp);
    K cpk = KindOf(cp);

    if (IsOpenParenInParameterList(cur) && pk == K::IdentifierToken) {
        if (IsOpenParenInParameterListOfParenthesizedLambdaExpression(cur)) return one;
        return zero;
    }
    if (IsOpenParenInParameterList(cur) && pk == K::GreaterThanToken) {
        if (IsOpenParenInParameterListOfParenthesizedLambdaExpression(cur) && ppk == K::TypeArgumentList) return one;
        if (ppk == K::TypeParameterList) return zero;
    }
    if ((IsKeyword(prev) || pk == K::QuestionToken || pk == K::AsteriskToken || pk == K::CloseBracketToken ||
         pk == K::CloseParenToken || pk == K::GreaterThanToken) &&
        IsOpenParenInParameterListOfAConversionOperatorDeclaration(cur)) {
        return zero;
    }
    if (ppk == K::OperatorDeclaration && IsOpenParenInParameterListOfAOperationDeclaration(cur)) return zero;
    if (IsOpenParenInParameterList(prev) && IsCloseParenInParameterList(cur)) return zero;
    if (IsOpenParenInParameterList(prev)) return zero;
    if (IsCloseParenInParameterList(cur)) return zero;
    if (IsOpenParenInArgumentListOrPositionalPattern(cur)) return zero;
    if (IsOpenParenInArgumentListOrPositionalPattern(prev) && IsCloseParenInArgumentListOrPositionalPattern(cur)) {
        return zero;
    }
    if (IsOpenParenInArgumentListOrPositionalPattern(prev)) return zero;
    if (IsCloseParenInArgumentListOrPositionalPattern(cur)) return zero;
    if (ck == K::OpenParenToken && IsFunctionLikeKeywordExpressionKind(cpk)) return zero;
    if (pk == K::OpenParenToken && IsFunctionLikeKeywordExpressionKind(ppk)) return zero;
    if (ck == K::CloseParenToken && IsFunctionLikeKeywordExpressionKind(cpk)) return zero;
    if (ck == K::OpenParenToken &&
        (pk == K::IfKeyword || pk == K::WhileKeyword || pk == K::SwitchKeyword || pk == K::ForKeyword ||
         pk == K::ForEachKeyword || pk == K::CatchKeyword || pk == K::UsingKeyword || pk == K::WhenKeyword ||
         pk == K::LockKeyword || pk == K::FixedKeyword)) {
        return one;
    }
    if (((ppk == K::ParenthesizedExpression || ppk == K::ParenthesizedPattern) && pk == K::OpenParenToken) ||
        ((cpk == K::ParenthesizedExpression || cpk == K::ParenthesizedPattern) && ck == K::CloseParenToken)) {
        return zero;
    }
    if ((ppk == K::CastExpression && pk == K::OpenParenToken) || (cpk == K::CastExpression && ck == K::CloseParenToken)) {
        return zero;
    }
    if (ppk == K::ForStatement && IsEmptyForStatement(pp)) {
        if (ck == K::SemicolonToken && pk != K::SemicolonToken) return zero;
        return one;
    }
    if (pk == K::OpenParenToken && IsControlFlowLikeKeywordStatementKind(ppk)) return zero;
    if (ck == K::CloseParenToken && IsControlFlowLikeKeywordStatementKind(cpk)) return zero;
    if (ppk == K::CastExpression && pk == K::CloseParenToken) return zero;
    if (ck == K::OpenBracketToken && (cpk == K::ListPattern || cpk == K::CollectionExpression)) {
        if (pk == K::CommaToken) return one;
        if (pk != K::OpenParenToken && pk != K::OpenBracketToken) return one;
    }
    if (ck == K::OpenBracketToken && HasFormattableBracketParent(cur) && !IsOpenBraceOrCommaOfObjectInitializer(prev)) {
        return zero;
    }
    if (((ck == K::CloseBracketToken && pk == K::OpenBracketToken) || ck == K::OmittedArraySizeExpressionToken) &&
        HasFormattableBracketParent(prev)) {
        return zero;
    }
    if (pk == K::OpenBracketToken && HasFormattableBracketParent(prev)) return zero;
    if (ck == K::CloseBracketToken && pk != K::OmittedArraySizeExpressionToken && HasFormattableBracketParent(cur)) {
        return zero;
    }
    if (pk == K::CloseBracketToken && ppk == K::AttributeList) {
        if (ck == K::OpenBracketToken && cpk == K::AttributeList) return zero;
        if (ck == K::EndOfFileToken) return zero;
        return one;
    }
    if (IsColonInTypeBaseList(prev)) return one;
    if (IsColonInTypeBaseList(cur)) return one;
    auto comma_custom = [&](bool check_previous) {
        Token token = check_previous ? prev : cur;
        K token_kind = check_previous ? pk : ck;
        K other = check_previous ? ck : pk;
        return (IsCommaInArgumentOrParameterList(token) && other != K::OmittedTypeArgumentToken) ||
               IsCommaInInitializerExpression(token) || IsCommaInCollectionExpression(token) ||
               IsCommaInTupleExpression(token) || IsCommaInTupleType(token) ||
               IsCommaInParenthesizedVariableDesignation(token) ||
               (token_kind == K::CommaToken && other != K::OmittedArraySizeExpressionToken &&
                HasFormattableBracketParent(token));
    };
    if (comma_custom(true)) return one;
    if (comma_custom(false)) return zero;
    if (IsDotInMemberAccessOrQualifiedName(prev)) return zero;
    if (IsDotInMemberAccessOrQualifiedName(cur)) return zero;
    if (IsSemicolonInForStatement(prev) && ck != K::CloseParenToken) return one;
    if (IsSemicolonInForStatement(cur)) return zero;
    if (Is(cp, kBinaryExpression) || Is(pp, kBinaryExpression) || Is(cp, kAssignmentExpression) ||
        Is(pp, kAssignmentExpression) || cpk == K::AndPattern || cpk == K::OrPattern || cpk == K::RelationalPattern ||
        ppk == K::AndPattern || ppk == K::OrPattern || ppk == K::RelationalPattern) {
        return one;
    }
    if (ppk == K::FunctionPointerType) {
        if (ck == K::AsteriskToken && pk == K::DelegateKeyword) return zero;
        if (pk == K::AsteriskToken && cpk == K::FunctionPointerCallingConvention) {
            if (ck == K::IdentifierToken || ck == K::ManagedKeyword || ck == K::UnmanagedKeyword) return one;
        }
    }
    if (cpk == K::FunctionPointerParameterList && ck == K::LessThanToken) {
        if (pk == K::AsteriskToken || pk == K::ManagedKeyword || pk == K::UnmanagedKeyword ||
            (pk == K::CloseBracketToken && ppk == K::FunctionPointerUnmanagedCallingConventionList)) {
            return zero;
        }
    }
    if (ppk == K::FunctionPointerCallingConvention && cpk == K::FunctionPointerUnmanagedCallingConventionList &&
        ck == K::OpenBracketToken) {
        return zero;
    }
    if (cpk == K::FunctionPointerUnmanagedCallingConventionList && ppk == K::FunctionPointerUnmanagedCallingConventionList) {
        if (ck == K::IdentifierToken) {
            if (pk == K::OpenBracketToken) return zero;
            if (pk == K::CommaToken) return one;
        }
        if (ck == K::CommaToken) return zero;
        if (ck == K::CloseBracketToken) return zero;
    }
    if (pk == K::LessThanToken && ppk == K::FunctionPointerParameterList) return zero;
    if (ck == K::GreaterThanToken && cpk == K::FunctionPointerParameterList) return zero;
    if (ppk == K::NotPattern) return one;
    if (pk == K::DotDotToken && ppk == K::SlicePattern) return one;
    if (pk == K::InterpolatedStringStartToken || pk == K::InterpolatedVerbatimStringStartToken ||
        pk == K::InterpolatedSingleLineRawStringStartToken || pk == K::InterpolatedMultiLineRawStringStartToken) {
        return Spaces(0, O::ForceSpaces);
    }
    if (ck == K::InterpolatedStringEndToken || ck == K::InterpolatedRawStringEndToken) return Spaces(0, O::ForceSpaces);
    if ((ck == K::OpenBraceToken && cpk == K::Interpolation) || (pk == K::CloseBraceToken && ppk == K::Interpolation)) {
        return Spaces(0, O::ForceSpaces);
    }
    if (pk == K::OpenBraceToken && ppk == K::Interpolation) return Spaces(0, O::ForceSpaces);
    if (ck == K::CloseBraceToken && cpk == K::Interpolation) {
        if (!F(cp, Field::FormatClause)) return Spaces(0, O::ForceSpaces);
        return Spaces(0, O::PreserveSpaces);
    }
    if ((pk == K::CommaToken && ppk == K::InterpolationAlignmentClause) ||
        (ck == K::CommaToken && cpk == K::InterpolationAlignmentClause)) {
        return Spaces(0, O::ForceSpaces);
    }
    if ((pk == K::ColonToken && ppk == K::InterpolationFormatClause) ||
        (ck == K::ColonToken && cpk == K::InterpolationFormatClause)) {
        return Spaces(0, O::ForceSpaces);
    }
    if (IsOpenParenInVarDeconstructionDeclaration(cur)) return Spaces(1, O::ForceSpaces);
    if (pk == K::CaretToken && ppk == K::IndexExpression) return Spaces(0, O::ForceSpaces);
    if (pk == K::DotDotToken && ppk == K::RangeExpression) {
        if (F(pp, Field::RightOperand)) return Spaces(0, O::ForceSpaces);
    }
    if (ck == K::DotDotToken && cpk == K::RangeExpression) {
        if (F(cp, Field::LeftOperand)) return Spaces(0, O::ForceSpaces);
    }
    return SpacesAfterSpacingRule(prev, cur);
}

/** @brief The new-line chain under NewLineUserSettingFormattingRule: EOF, structured trivia, query, token based. */
NewLinesOp NewLinesBelowUserSettings(const Tree &tree, Token prev, Token cur) {
    using O = NewLinesOption;
    if (Kind(cur) == K::EndOfFileToken) return Lines(0, O::PreserveLines);
    if (Is(Parent(prev), kStructuredTrivia) || Is(Parent(cur), kStructuredTrivia)) return {};
    // QueryExpressionFormattingRule.
    if (IsNestedQueryExpression(prev)) return Lines(0, O::PreserveLines);
    if (!IsFirstFromKeywordInExpression(cur)) {
        switch (Kind(cur)) {
        case K::FromKeyword:
        case K::WhereKeyword:
        case K::LetKeyword:
        case K::JoinKeyword:
        case K::OrderByKeyword:
        case K::GroupKeyword:
        case K::SelectKeyword:
            if (GetAncestorKind(cur, K::QueryExpression)) return Lines(1, O::PreserveLines);
            break;
        default: break;
        }
    }
    return TokenBasedNewLines(tree, prev, cur);
}

bool IsControlBlock(const Node *n) {
    if (IsKind(n, K::SwitchStatement)) return true;
    switch (KindOf(Parent(n))) {
    case K::IfStatement:
    case K::ElseClause:
    case K::WhileStatement:
    case K::DoStatement:
    case K::ForEachStatement:
    case K::ForEachVariableStatement:
    case K::UsingStatement:
    case K::ForStatement:
    case K::TryStatement:
    case K::CatchClause:
    case K::FinallyClause:
    case K::LockStatement:
    case K::CheckedStatement:
    case K::UncheckedStatement:
    case K::SwitchSection:
    case K::FixedStatement:
    case K::UnsafeStatement: return true;
    default: return false;
    }
}

}  // namespace

// --- Rules -------------------------------------------------------------------------------------------

void Rules::AddSuppressOperations(std::vector<SuppressOperation> &list, const Node *node) const {
    // Chain order: each rule runs the rest first, then adds its own.
    QuerySuppress(list, node);
    SuppressFormattingRuleOps(tree_, list, node);
    WrappingRuleOps(tree_, list, node);
}

void Rules::AddAnchorIndentationOperations(std::vector<AnchorIndentationOperation> &list, const Node *node) const {
    QueryAnchor(list, node);
    AnchorRuleOps(list, node);
}

void Rules::AddIndentBlockOperations(std::vector<IndentBlockOperation> &list, const Node *node) const {
    QueryIndent(tree_, list, node);
    AddAlignmentBlockOperation(tree_, list, node);
    AddBlockIndentationOperation(tree_, list, node);
    AddBracketIndentationOperation(tree_, list, node);
    AddParenthesizedPatternIndentationOperation(tree_, list, node);
    AddLabelIndentationOperation(list, node);
    AddSwitchIndentationOperation(tree_, list, node);
    AddEmbeddedStatementsIndentationOperation(tree_, list, node);
    AddTypeParameterConstraintClauseOperation(tree_, list, node);
}

NewLinesOp Rules::GetAdjustNewLinesOperation(Token prev, Token cur) const {
    using O = NewLinesOption;
    // NewLineUserSettingFormattingRule (default options).
    NewLinesOp op = NewLinesBelowUserSettings(tree_, prev, cur);
    const Node *cp = Parent(cur);
    K ck = Kind(cur);
    if (Kind(prev) == K::CommaToken && IsKind(Parent(prev), K::ObjectInitializerExpression)) return Lines(1, O::PreserveLines);
    if (Kind(prev) == K::CommaToken && IsKind(Parent(prev), K::AnonymousObjectCreationExpression)) {
        return Lines(1, O::PreserveLines);
    }
    if (Kind(prev) == K::CloseBraceToken && ck == K::ElseKeyword) return Lines(1, O::PreserveLines);
    if (ck == K::CatchKeyword || ck == K::FinallyKeyword) return Lines(1, O::PreserveLines);
    if (ck == K::OpenBraceToken) {
        K cpk = KindOf(cp);
        if (Is(cp, kBaseTypeDeclaration) || cpk == K::NamespaceDeclaration) return Lines(1, O::PreserveLines);
        if (cpk == K::AnonymousObjectCreationExpression) return Lines(1, O::PreserveLines);
        if (cpk == K::ObjectInitializerExpression || cpk == K::CollectionInitializerExpression ||
            cpk == K::WithInitializerExpression || cpk == K::PropertyPatternClause) {
            return Lines(1, O::PreserveLines);
        }
        if (cpk == K::ArrayInitializerExpression || cpk == K::ImplicitArrayCreationExpression) return {};
        const Node *cpp = Parent(cp);
        if (Is(cpp, kMemberDeclaration)) return Lines(1, O::PreserveLines);
        if (Is(cpp, kAccessorDeclaration)) return Lines(1, O::PreserveLines);
        if (IsKind(cpp, K::AnonymousMethodExpression)) return Lines(1, O::ForceLinesIfOnSingleLine);
        if (IsKind(cpp, K::LocalFunctionStatement)) return Lines(1, O::PreserveLines);
        if (IsKind(cpp, K::SimpleLambdaExpression) || IsKind(cpp, K::ParenthesizedLambdaExpression)) {
            return Lines(1, O::ForceLinesIfOnSingleLine);
        }
        if (cpk == K::SwitchExpression) return Lines(1, O::PreserveLines);
        if (IsControlBlock(cp)) return Lines(1, O::PreserveLines);
    }
    return op;
}

SpacesOp Rules::GetAdjustSpacesOperation(Token prev, Token cur) const { return SpacingRule(prev, cur); }

// --- CSharpHeaderFacts -----------------------------------------------------------------------------

const Node *ConditionStatementForHeader(const Tree &tree, int position) {
    // IsOnIfStatementHeader, then IsOnWhileStatementHeader (TryGetAncestorForLocation + IsOnHeader).
    for (K kind : {K::IfStatement, K::WhileStatement}) {
        Token right = tree.syntax()->FindToken(position);
        const Node *node = GetAncestorKind(right, kind);
        if (!node) {
            if (right && (FullSpanOf(right).start == position || Kind(right) == K::EndOfFileToken)) {
                node = GetAncestorKind(tree.PreviousToken(right, false), kind);
            }
        }
        if (!node) continue;
        // GetStartOfNodeExcludingAttributes.
        std::vector<const Node *> attrs = Items(node, Field::AttributeLists);
        int start = node->Span().start;
        if (!attrs.empty()) {
            Token after = tree.NextToken(LastToken(attrs.back(), true), false);
            start = after ? SpanStart(after) : start;
        }
        Token close = F(node, Field::CloseParenToken);
        int end = FullSpanOf(close).end;
        if (start > end) continue;
        if (position >= start && position <= end) return node;
    }
    return nullptr;
}

}  // namespace csfmt
