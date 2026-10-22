// LanguageParser, translation unit E: LanguageParser.cs lines 11028-12893 (ParseExpressionStatement ..
// ParseTupleExpressionTail) -- the expression core: precedence parsing, assignment / conditional expressions,
// primary and postfix expressions, conditional access, argument lists, typeof/default/sizeof/..., the
// parenthesized-lambda scans and parenthesized expressions / tuples.
//
// Translated from Roslyn (MIT, (c) .NET Foundation and Contributors); see csharp_syntax.h for the commit.
//
// C# evaluates call arguments left to right and C++ does not, so every factory call whose arguments consume
// tokens is written with the arguments in locals, in the C# order.

#include <vector>

#include "csharp_parser_internal.h"

namespace csharp {

GreenNode *LanguageParser::ParseExpressionStatement(SyntaxList attributes) {
    GreenNode *expression = this->ParseExpressionCore();
    return ParseExpressionStatement(attributes, expression);
}

GreenNode *LanguageParser::ParseExpressionStatement(SyntaxList attributes, GreenNode *expression) {
    GreenNode *semicolon;
    if (IsScript() && this->CurrentToken()->kind == SyntaxKind::EndOfFileToken) {
        semicolon = SyntaxFactory::MissingToken(SyntaxKind::SemicolonToken);
    } else {
        // Do not report an error if the expression is not a statement expression.
        // The error is reported in semantic analysis.
        semicolon = this->EatToken(SyntaxKind::SemicolonToken);
    }

    return SyntaxFactory::ExpressionStatement(attributes, expression, semicolon);
}

GreenNode *LanguageParser::ParseExpression() {
    return ParseWithStackGuard([](LanguageParser *p) { return p->ParseExpressionCore(); },
                               [](LanguageParser *p) { return p->CreateMissingIdentifierName(); });
}

GreenNode *LanguageParser::ParseExpressionCore() { return this->ParseSubExpression(Precedence::Expression); }

/// Is the current token one that could start an expression?
bool LanguageParser::CanStartExpression() {
    return IsPossibleExpression(/*allowBinaryExpressions:*/ false, /*allowAssignmentExpressions:*/ false);
}

/// Is the current token one that could be in an expression?
bool LanguageParser::IsPossibleExpression() {
    return IsPossibleExpression(/*allowBinaryExpressions:*/ true, /*allowAssignmentExpressions:*/ true);
}

bool LanguageParser::IsPossibleExpression(bool allowBinaryExpressions, bool allowAssignmentExpressions) {
    SyntaxKind tk = this->CurrentToken()->kind;
    // case SyntaxKind.DotToken when IsAtDotDotToken(): (otherwise a dot goes to the default section)
    if (tk == SyntaxKind::DotToken && IsAtDotDotToken()) return true;
    switch (tk) {
        case SyntaxKind::TypeOfKeyword:
        case SyntaxKind::DefaultKeyword:
        case SyntaxKind::SizeOfKeyword:
        case SyntaxKind::MakeRefKeyword:
        case SyntaxKind::RefTypeKeyword:
        case SyntaxKind::CheckedKeyword:
        case SyntaxKind::UncheckedKeyword:
        case SyntaxKind::RefValueKeyword:
        case SyntaxKind::ArgListKeyword:
        case SyntaxKind::BaseKeyword:
        case SyntaxKind::FalseKeyword:
        case SyntaxKind::ThisKeyword:
        case SyntaxKind::TrueKeyword:
        case SyntaxKind::NullKeyword:
        case SyntaxKind::OpenParenToken:
        case SyntaxKind::NumericLiteralToken:
        case SyntaxKind::StringLiteralToken:
        case SyntaxKind::Utf8StringLiteralToken:
        case SyntaxKind::SingleLineRawStringLiteralToken:
        case SyntaxKind::Utf8SingleLineRawStringLiteralToken:
        case SyntaxKind::MultiLineRawStringLiteralToken:
        case SyntaxKind::Utf8MultiLineRawStringLiteralToken:
        case SyntaxKind::InterpolatedStringToken:
        case SyntaxKind::InterpolatedStringStartToken:
        case SyntaxKind::InterpolatedVerbatimStringStartToken:
        case SyntaxKind::InterpolatedSingleLineRawStringStartToken:
        case SyntaxKind::InterpolatedMultiLineRawStringStartToken:
        case SyntaxKind::CharacterLiteralToken:
        case SyntaxKind::NewKeyword:
        case SyntaxKind::DelegateKeyword:
        case SyntaxKind::ColonColonToken:  // bad aliased name
        case SyntaxKind::ThrowKeyword:
        case SyntaxKind::StackAllocKeyword:
        case SyntaxKind::RefKeyword:
        case SyntaxKind::OpenBracketToken:  // attributes on a lambda, or a collection expression.
            return true;
        case SyntaxKind::StaticKeyword:
            return IsPossibleAnonymousMethodExpression() || IsPossibleLambdaExpression(Precedence::Expression);
        case SyntaxKind::IdentifierToken:
            // Specifically allow the from contextual keyword, because it can always be the start of an
            // expression (whether it is used as an identifier or a keyword).
            return this->IsTrueIdentifier() || this->CurrentToken()->contextual_kind == SyntaxKind::FromKeyword;
        default:
            return IsPredefinedType(tk) || SyntaxFacts::IsAnyUnaryExpression(tk) ||
                   (allowBinaryExpressions && SyntaxFacts::IsBinaryExpression(tk)) ||
                   (allowAssignmentExpressions && SyntaxFacts::IsAssignmentExpressionOperatorToken(tk));
    }
}

bool LanguageParser::IsInvalidSubExpression(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::BreakKeyword:
        case SyntaxKind::CaseKeyword:
        case SyntaxKind::CatchKeyword:
        case SyntaxKind::ConstKeyword:
        case SyntaxKind::ContinueKeyword:
        case SyntaxKind::DoKeyword:
        case SyntaxKind::FinallyKeyword:
        case SyntaxKind::ForKeyword:
        case SyntaxKind::ForEachKeyword:
        case SyntaxKind::GotoKeyword:
        case SyntaxKind::IfKeyword:
        case SyntaxKind::ElseKeyword:
        case SyntaxKind::LockKeyword:
        case SyntaxKind::ReturnKeyword:
        case SyntaxKind::SwitchKeyword:
        case SyntaxKind::TryKeyword:
        case SyntaxKind::UsingKeyword:
        case SyntaxKind::WhileKeyword:
            return true;
        default:
            return false;
    }
}

bool LanguageParser::IsRightAssociative(SyntaxKind op) {
    switch (op) {
        case SyntaxKind::SimpleAssignmentExpression:
        case SyntaxKind::AddAssignmentExpression:
        case SyntaxKind::SubtractAssignmentExpression:
        case SyntaxKind::MultiplyAssignmentExpression:
        case SyntaxKind::DivideAssignmentExpression:
        case SyntaxKind::ModuloAssignmentExpression:
        case SyntaxKind::AndAssignmentExpression:
        case SyntaxKind::ExclusiveOrAssignmentExpression:
        case SyntaxKind::OrAssignmentExpression:
        case SyntaxKind::LeftShiftAssignmentExpression:
        case SyntaxKind::RightShiftAssignmentExpression:
        case SyntaxKind::UnsignedRightShiftAssignmentExpression:
        case SyntaxKind::CoalesceAssignmentExpression:
        case SyntaxKind::CoalesceExpression:
            return true;
        default:
            return false;
    }
}

// (enum Precedence: csharp_parser_types.inc)

LanguageParser::Precedence LanguageParser::GetPrecedence(SyntaxKind op) {
    switch (op) {
        case SyntaxKind::QueryExpression:
            return Precedence::Expression;
        case SyntaxKind::ParenthesizedLambdaExpression:
        case SyntaxKind::SimpleLambdaExpression:
        case SyntaxKind::AnonymousMethodExpression:
            return Precedence::Lambda;
        case SyntaxKind::SimpleAssignmentExpression:
        case SyntaxKind::AddAssignmentExpression:
        case SyntaxKind::SubtractAssignmentExpression:
        case SyntaxKind::MultiplyAssignmentExpression:
        case SyntaxKind::DivideAssignmentExpression:
        case SyntaxKind::ModuloAssignmentExpression:
        case SyntaxKind::AndAssignmentExpression:
        case SyntaxKind::ExclusiveOrAssignmentExpression:
        case SyntaxKind::OrAssignmentExpression:
        case SyntaxKind::LeftShiftAssignmentExpression:
        case SyntaxKind::RightShiftAssignmentExpression:
        case SyntaxKind::UnsignedRightShiftAssignmentExpression:
        case SyntaxKind::CoalesceAssignmentExpression:
            return Precedence::Assignment;
        case SyntaxKind::CoalesceExpression:
        case SyntaxKind::ThrowExpression:
            return Precedence::Coalescing;
        case SyntaxKind::LogicalOrExpression:
            return Precedence::ConditionalOr;
        case SyntaxKind::LogicalAndExpression:
            return Precedence::ConditionalAnd;
        case SyntaxKind::BitwiseOrExpression:
            return Precedence::LogicalOr;
        case SyntaxKind::ExclusiveOrExpression:
            return Precedence::LogicalXor;
        case SyntaxKind::BitwiseAndExpression:
            return Precedence::LogicalAnd;
        case SyntaxKind::EqualsExpression:
        case SyntaxKind::NotEqualsExpression:
            return Precedence::Equality;
        case SyntaxKind::LessThanExpression:
        case SyntaxKind::LessThanOrEqualExpression:
        case SyntaxKind::GreaterThanExpression:
        case SyntaxKind::GreaterThanOrEqualExpression:
        case SyntaxKind::IsExpression:
        case SyntaxKind::AsExpression:
        case SyntaxKind::IsPatternExpression:
            return Precedence::Relational;
        case SyntaxKind::SwitchExpression:
        case SyntaxKind::WithExpression:
            return Precedence::Switch;
        case SyntaxKind::LeftShiftExpression:
        case SyntaxKind::RightShiftExpression:
        case SyntaxKind::UnsignedRightShiftExpression:
            return Precedence::Shift;
        case SyntaxKind::AddExpression:
        case SyntaxKind::SubtractExpression:
            return Precedence::Additive;
        case SyntaxKind::MultiplyExpression:
        case SyntaxKind::DivideExpression:
        case SyntaxKind::ModuloExpression:
            return Precedence::Multiplicative;
        case SyntaxKind::UnaryPlusExpression:
        case SyntaxKind::UnaryMinusExpression:
        case SyntaxKind::BitwiseNotExpression:
        case SyntaxKind::LogicalNotExpression:
        case SyntaxKind::PreIncrementExpression:
        case SyntaxKind::PreDecrementExpression:
        case SyntaxKind::TypeOfExpression:
        case SyntaxKind::SizeOfExpression:
        case SyntaxKind::CheckedExpression:
        case SyntaxKind::UncheckedExpression:
        case SyntaxKind::MakeRefExpression:
        case SyntaxKind::RefValueExpression:
        case SyntaxKind::RefTypeExpression:
        case SyntaxKind::AwaitExpression:
        case SyntaxKind::IndexExpression:
            return Precedence::Unary;
        case SyntaxKind::CastExpression:
            return Precedence::Cast;
        case SyntaxKind::PointerIndirectionExpression:
            return Precedence::PointerIndirection;
        case SyntaxKind::AddressOfExpression:
            return Precedence::AddressOf;
        case SyntaxKind::RangeExpression:
            return Precedence::Range;
        case SyntaxKind::ConditionalExpression:
            return Precedence::Expression;
        case SyntaxKind::AliasQualifiedName:
        case SyntaxKind::AnonymousObjectCreationExpression:
        case SyntaxKind::ArgListExpression:
        case SyntaxKind::ArrayCreationExpression:
        case SyntaxKind::BaseExpression:
        case SyntaxKind::CharacterLiteralExpression:
        case SyntaxKind::CollectionExpression:
        case SyntaxKind::ConditionalAccessExpression:
        case SyntaxKind::DeclarationExpression:
        case SyntaxKind::DefaultExpression:
        case SyntaxKind::DefaultLiteralExpression:
        case SyntaxKind::ElementAccessExpression:
        case SyntaxKind::FalseLiteralExpression:
        case SyntaxKind::FieldExpression:
        case SyntaxKind::GenericName:
        case SyntaxKind::IdentifierName:
        case SyntaxKind::ImplicitArrayCreationExpression:
        case SyntaxKind::ImplicitStackAllocArrayCreationExpression:
        case SyntaxKind::ImplicitObjectCreationExpression:
        case SyntaxKind::InterpolatedStringExpression:
        case SyntaxKind::InvocationExpression:
        case SyntaxKind::NullLiteralExpression:
        case SyntaxKind::NumericLiteralExpression:
        case SyntaxKind::ObjectCreationExpression:
        case SyntaxKind::ParenthesizedExpression:
        case SyntaxKind::PointerMemberAccessExpression:
        case SyntaxKind::PostDecrementExpression:
        case SyntaxKind::PostIncrementExpression:
        case SyntaxKind::PredefinedType:
        case SyntaxKind::RefExpression:
        case SyntaxKind::SimpleMemberAccessExpression:
        case SyntaxKind::StackAllocArrayCreationExpression:
        case SyntaxKind::StringLiteralExpression:
        case SyntaxKind::Utf8StringLiteralExpression:
        case SyntaxKind::SuppressNullableWarningExpression:
        case SyntaxKind::ThisExpression:
        case SyntaxKind::TrueLiteralExpression:
        case SyntaxKind::TupleExpression:
            return Precedence::Primary;
        default:
            // C#: throw ExceptionUtilities.UnexpectedValue(op). Every expression kind the parser builds is
            // listed above (Roslyn checks that in DEBUG builds); rather than abort the editor on a kind that
            // cannot occur, treat it as primary.
            CS_ASSERT(false);
            return Precedence::Primary;
    }
}

bool LanguageParser::IsExpectedPrefixUnaryOperator(SyntaxKind kind) {
    return SyntaxFacts::IsPrefixUnaryExpression(kind) && kind != SyntaxKind::RefKeyword &&
           kind != SyntaxKind::OutKeyword;
}

bool LanguageParser::IsExpectedBinaryOperator(SyntaxKind kind) { return SyntaxFacts::IsBinaryExpression(kind); }

bool LanguageParser::IsExpectedAssignmentOperator(SyntaxKind kind) {
    return SyntaxFacts::IsAssignmentExpressionOperatorToken(kind);
}

bool LanguageParser::IsPossibleAwaitExpressionStatement() {
    return (this->IsScript() || this->IsInAsync()) && this->CurrentToken()->contextual_kind == SyntaxKind::AwaitKeyword;
}

bool LanguageParser::IsAwaitExpression() {
    if (this->CurrentToken()->contextual_kind == SyntaxKind::AwaitKeyword) {
        if (this->IsInAsync()) {
            // If we see an await in an async function, parse it as an unop.
            return true;
        }

        // If we see an await followed by a token that cannot follow an identifier, parse await as a unop.
        // BindAwait() catches the cases where await successfully parses as a unop but is not in an async
        // function, and reports an appropriate ERR_BadAwaitWithoutAsync* error.
        GreenNode *next = PeekToken(1);
        switch (next->kind) {
            case SyntaxKind::IdentifierToken:
                return next->contextual_kind != SyntaxKind::WithKeyword;

            // Keywords
            case SyntaxKind::NewKeyword:
            case SyntaxKind::ThisKeyword:
            case SyntaxKind::BaseKeyword:
            case SyntaxKind::DelegateKeyword:
            case SyntaxKind::TypeOfKeyword:
            case SyntaxKind::CheckedKeyword:
            case SyntaxKind::UncheckedKeyword:
            case SyntaxKind::DefaultKeyword:

            // Literals
            case SyntaxKind::TrueKeyword:
            case SyntaxKind::FalseKeyword:
            case SyntaxKind::StringLiteralToken:
            case SyntaxKind::SingleLineRawStringLiteralToken:
            case SyntaxKind::Utf8SingleLineRawStringLiteralToken:
            case SyntaxKind::MultiLineRawStringLiteralToken:
            case SyntaxKind::Utf8MultiLineRawStringLiteralToken:
            case SyntaxKind::InterpolatedStringToken:
            case SyntaxKind::Utf8StringLiteralToken:
            case SyntaxKind::InterpolatedStringStartToken:
            case SyntaxKind::InterpolatedVerbatimStringStartToken:
            case SyntaxKind::InterpolatedSingleLineRawStringStartToken:
            case SyntaxKind::InterpolatedMultiLineRawStringStartToken:
            case SyntaxKind::NumericLiteralToken:
            case SyntaxKind::NullKeyword:
            case SyntaxKind::CharacterLiteralToken:
                return true;
            default:
                break;
        }
    }

    return false;
}

/// Parse a subexpression of the enclosing operator of the given precedence.
GreenNode *LanguageParser::ParseSubExpression(Precedence precedence) {
    _recursionDepth++;

    EnsureSufficientExecutionStack(_recursionDepth);

    GreenNode *result = ParseSubExpressionCore(precedence);
    _recursionDepth--;
    return result;
}

GreenNode *LanguageParser::ParseSubExpressionCore(Precedence precedence) {
    // all of these are tokens that start statements and are invalid
    // to start a expression with. if we see one, then we must have
    // something like:
    //
    // return
    // if (...
    // parse out a missing name node for the expression, and keep on going
    SyntaxKind tk = this->CurrentToken()->kind;
    if (IsInvalidSubExpression(tk)) {
        return this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_InvalidExprTerm,
                              {SyntaxFacts::GetText(tk)});
    }

    // Parses out a unary expression, or something with higher precedence (cast, addressof, primary).
    // (The C# local function takes `precedence` as a parameter; it is only ever called with this method's.)
    auto parseUnaryOrPrimaryExpression = [&]() -> GreenNode * {
        // Parse a left operand -- possibly preceded by a unary operator.
        if (IsExpectedPrefixUnaryOperator(tk)) {
            SyntaxKind opKind = SyntaxFacts::GetPrefixUnaryExpression(tk);
            GreenNode *operatorToken = this->EatToken();
            GreenNode *operand = this->ParseSubExpression(GetPrecedence(opKind));
            return SyntaxFactory::PrefixUnaryExpression(opKind, operatorToken, operand);
        }

        // Check *explicitly* for `..` starting an expression.  This *is* the initial term we want to parse out.
        // If we have `expr..` though we don't do that here.  Instead, we'll parse out 'expr', and the `..`
        // portion will be handled in ParseExpressionContinued.
        if (IsAtDotDotToken()) {
            GreenNode *dotDot = this->EatDotDotToken();
            GreenNode *right = CanStartExpression() ? this->ParseSubExpression(Precedence::Range) : nullptr;
            return SyntaxFactory::RangeExpression(/*leftOperand:*/ nullptr, dotDot, right);
        }

        if (IsAwaitExpression()) {
            GreenNode *awaitKeyword = this->EatContextualToken(SyntaxKind::AwaitKeyword);
            GreenNode *operand = this->ParseSubExpression(GetPrecedence(SyntaxKind::AwaitExpression));
            return SyntaxFactory::AwaitExpression(awaitKeyword, operand);
        }

        if (this->IsQueryExpression(/*mayBeVariableDeclaration:*/ false, /*mayBeMemberDeclaration:*/ false))
            return this->ParseQueryExpression(precedence);

        if (this->CurrentToken()->contextual_kind == SyntaxKind::FromKeyword && IsInQuery()) {
            // If this "from" token wasn't the start of a query then it's not really an expression.
            // Consume it so that we don't try to parse it again as the next argument in an
            // argument list.
            // (C# argument order: the token is eaten before CurrentToken.Text is read, so the message names the
            // token *after* `from`.)
            GreenNode *missing = this->CreateMissingIdentifierName();
            GreenNode *fromToken = this->EatToken();
            Text nextText = this->CurrentToken()->text;
            return AddTrailingSkippedSyntax(missing, this->AddError(fromToken, ErrorCode::ERR_InvalidExprTerm, {nextText}));
        }

        if (tk == SyntaxKind::ThrowKeyword) {
            GreenNode *result = ParseThrowExpression();
            // we parse a throw expression even at the wrong precedence for better recovery
            return precedence <= Precedence::Coalescing
                       ? result
                       : this->AddError(result, ErrorCode::ERR_InvalidExprTerm, {SyntaxFacts::GetText(tk)});
        }

        if (this->IsPossibleDeconstructionLeft(precedence))
            return ParseDeclarationExpression(ParseTypeMode::Normal, /*isScoped:*/ false);

        // Not a unary operator - get a primary expression.
        return this->ParsePrimaryExpression(precedence);
    };

    GreenNode *unaryOrPrimary = parseUnaryOrPrimaryExpression();
    return ParseExpressionContinued(unaryOrPrimary, precedence);
}

/// The ConditionalExpression check of ParseExpressionContinued (its static local function
/// containsTernaryCollectionToReinterpret): is there, anywhere in `expression`, a conditional expression whose
/// when-true part starts with `[`?
static bool containsTernaryCollectionToReinterpret(GreenNode *expression) {
    std::vector<GreenNode *> stack;
    stack.push_back(expression);

    while (!stack.empty()) {
        GreenNode *current = stack.back();
        stack.pop_back();
        if (current->kind == SyntaxKind::ConditionalExpression) {
            GreenNode *whenTrue = GetField(current, Field::WhenTrue);
            GreenNode *first = whenTrue != nullptr ? whenTrue->GetFirstToken() : nullptr;
            if (first != nullptr && first->kind == SyntaxKind::OpenBracketToken) return true;
        }

        // Note: we could consider not recursing into anonymous-methods/lambdas (since we reset the
        // ForceConditionalAccessExpression flag when we go into that).  However, that adds a bit of
        // fragile coupling between these different code blocks that i'd prefer to avoid.  In practice
        // the extra cost here will almost never occur, so the simplicity is worth it.
        // (ChildNodesAndTokens: the slots, lists entered; the visiting order does not affect the answer.)
        if (current->IsToken() || current->IsTrivia()) continue;
        for (int i = 0; i < current->SlotCount(); i++) {
            GreenNode *child = current->GetSlot(i);
            if (child != nullptr) stack.push_back(child);
        }
    }

    return false;
}

/// Takes in an initial unary expression or primary expression, and then consumes what follows as long as its
/// precedence is either lower than the <paramref name="precedence"/> we're parsing currently, or equal to that
/// precedence if we have something right-associative <see cref="IsRightAssociative"/>.
GreenNode *LanguageParser::ParseExpressionContinued(GreenNode *unaryOrPrimaryExpression, Precedence precedence) {
    // (The C# local function tryExpandExpression takes `precedence` as a parameter; it is only ever called
    // with this method's.)
    auto tryExpandExpression = [&](GreenNode *leftOperand) -> GreenNode * {
        // Look for operators that can follow what we've seen so far, and which are acceptable at this
        // precedence level.  Examples include binary operator, assignment operators, range operators `..`, as
        // well as `switch` and `with` clauses.

        auto [operatorTokenKind, operatorExpressionKind] = GetExpressionOperatorTokenKindAndExpressionKind();

        if (operatorTokenKind == SyntaxKind::None) return nullptr;

        Precedence newPrecedence = GetPrecedence(operatorExpressionKind);

        // Check the precedence to see if we should "take" this operator.  A lower precedence means what's
        // coming isn't a child of us, but rather we will be a child of it.  So we bail out and let any higher
        // up expression parsing consume it with us as the left side.
        if (newPrecedence < precedence) return nullptr;

        // Same precedence, but not right-associative -- deal with this "later"
        if ((newPrecedence == precedence) && !IsRightAssociative(operatorExpressionKind)) return nullptr;

        // Now consume the operator (including consuming multiple tokens in the case of merged operator tokens)
        GreenNode *operatorToken = EatExpressionOperatorToken(operatorTokenKind);

        if (newPrecedence > GetPrecedence(leftOperand->kind)) {
            // Normally, a left operand with a looser precedence will consume all right operands that
            // have a tighter precedence.  For example, in the expression `a + b * c`, the `* c` part
            // will be consumed as part of the right operand of the addition.  However, there are a
            // few circumstances in which a tighter precedence is not consumed: that occurs when the
            // left hand operator does not have an expression as its right operand.  This occurs for
            // the is-type operator and the is-pattern operator.  Source text such as
            // `a is {} + b` should produce a syntax error, as parsing the `+` with an `is`
            // expression as its left operand would be a precedence inversion.  Similarly, it occurs
            // with an anonymous method expression or a lambda expression with a block body.  No
            // further parsing will find a way to fix things up, so we accept the operator but issue
            // a diagnostic.
            operatorToken = this->AddError(operatorToken,
                                           leftOperand->kind == SyntaxKind::IsPatternExpression
                                               ? ErrorCode::ERR_UnexpectedToken
                                               : ErrorCode::WRN_PrecedenceInversion,
                                           {operatorToken->text});
        }

        if (operatorExpressionKind == SyntaxKind::AsExpression) {
            GreenNode *type = this->ParseType(ParseTypeMode::AsExpression);
            return SyntaxFactory::BinaryExpression(operatorExpressionKind, leftOperand, operatorToken, type);
        }

        if (operatorExpressionKind == SyntaxKind::IsExpression) return ParseIsExpression(leftOperand, operatorToken);

        if (operatorExpressionKind == SyntaxKind::SwitchExpression)
            return ParseSwitchExpression(leftOperand, operatorToken);

        if (operatorExpressionKind == SyntaxKind::WithExpression) return ParseWithExpression(leftOperand, operatorToken);

        if (operatorExpressionKind == SyntaxKind::RangeExpression) {
            GreenNode *right = CanStartExpression() ? this->ParseSubExpression(Precedence::Range) : nullptr;
            return SyntaxFactory::RangeExpression(leftOperand, operatorToken, right);
        }

        if (IsExpectedAssignmentOperator(operatorToken->kind))
            return ParseAssignmentExpression(operatorExpressionKind, leftOperand, operatorToken);

        if (IsExpectedBinaryOperator(operatorToken->kind)) {
            GreenNode *right = this->ParseSubExpression(newPrecedence);
            return SyntaxFactory::BinaryExpression(operatorExpressionKind, leftOperand, operatorToken, right);
        }

        // throw ExceptionUtilities.Unreachable();
        CS_ASSERT(false);
        return nullptr;
    };

    auto consumeConditionalExpression = [&](GreenNode *leftOperand) -> GreenNode * {
        // Complex ambiguity with `?` and collection-expressions.  Specifically: b?[c]:d
        //
        // On its own, we want that to be a conditional expression with a collection expression in it.  However, for
        // back compat, we need to make sure that `a ? b?[c] : d` sees the inner `b?[c]` as a
        // conditional-access-expression.  So, if after consuming the portion after the initial `?` if we do not
        // have the `:` we need, and we can see a `?[` in that portion of the parse, then we retry consuming the
        // when-true portion, but this time forcing the prior way of handling `?[`.
        GreenNode *questionToken = this->EatToken();

        DisposableResetPoint afterQuestionToken(this, /*resetOnDispose:*/ false);
        GreenNode *whenTrue = this->ParsePossibleRefExpression();

        if (this->CurrentToken()->kind != SyntaxKind::ColonToken && !this->ForceConditionalAccessExpression() &&
            containsTernaryCollectionToReinterpret(whenTrue)) {
            // Keep track of where we are right now in case the new parse doesn't make things better.
            DisposableResetPoint originalAfterWhenTrue(this, /*resetOnDispose:*/ false);

            // Go back to right after the `?`
            afterQuestionToken.Reset();

            // try reparsing with `?[` as a conditional access, not a ternary+collection
            GreenNode *newWhenTrue;
            {
                ParserSyntaxContextResetter _(this, std::nullopt, std::nullopt, std::nullopt,
                                              /*forceConditionalAccessExpression:*/ true);
                // reparse the when-true portion
                newWhenTrue = this->ParsePossibleRefExpression();
            }

            if (this->CurrentToken()->kind == SyntaxKind::ColonToken) {
                // if we now are at a colon, this was preferred parse.
                whenTrue = newWhenTrue;
            } else {
                // retrying the parse didn't help.  Use the original interpretation.
                originalAfterWhenTrue.Reset();
            }
        }

        if (this->CurrentToken()->kind == SyntaxKind::EndOfFileToken && this->lexer->InterpolationFollowedByColon()) {
            // We have an interpolated string with an interpolation that contains a conditional expression.
            // Unfortunately, the precedence demands that the colon is considered to signal the start of the
            // format string. Without this code, the compiler would complain about a missing colon, and point
            // to the colon that is present, which would be confusing. We aim to give a better error message.
            GreenNode *conditionalExpression = SyntaxFactory::ConditionalExpression(
                leftOperand, questionToken, whenTrue, SyntaxFactory::MissingToken(SyntaxKind::ColonToken),
                SyntaxFactory::IdentifierName(SyntaxFactory::MissingToken(SyntaxKind::IdentifierToken)));
            return this->AddError(conditionalExpression, ErrorCode::ERR_ConditionalInInterpolation);
        } else {
            GreenNode *colonToken = this->EatToken(SyntaxKind::ColonToken);
            GreenNode *whenFalse = this->ParsePossibleRefExpression();
            return SyntaxFactory::ConditionalExpression(leftOperand, questionToken, whenTrue, colonToken, whenFalse);
        }
    };

    GreenNode *currentExpression = unaryOrPrimaryExpression;

    // Keep on expanding the left operand as long as what we see fits the precedence we're under.
    while (GreenNode *expandedExpression = tryExpandExpression(currentExpression))
        currentExpression = expandedExpression;

    // Finally, consume a conditional expression if precedence allows it.
    //
    // https://github.com/dotnet/csharpstandard/blob/standard-v6/standard/expressions.md#1115-conditional-operator:
    //
    // conditional_expression
    //     : null_coalescing_expression
    //     | null_coalescing_expression '?' expression ':' expression
    //     ;
    //
    // 1. Only take the conditional part of the expression if we're at or below its precedence.
    // 2. When parsing the branches of the expression, parse at the highest precedence again ('expression').
    //    This allows for things like assignments/lambdas in the branches of the conditional.
    if (this->CurrentToken()->kind == SyntaxKind::QuestionToken && precedence <= Precedence::Conditional)
        return consumeConditionalExpression(currentExpression);

    return currentExpression;
}

std::pair<SyntaxKind, SyntaxKind> LanguageParser::GetExpressionOperatorTokenKindAndExpressionKind() {
    // If the set of expression continuations is updated here, please review ParseStatementAttributeDeclarations
    // to see if it may need a similar look-ahead check to determine if something is a collection expression versus
    // an attribute.

    GreenNode *token1 = this->CurrentToken();
    SyntaxKind token1Kind = token1->contextual_kind;

    // Merge two consecutive dots into a DotDotToken
    if (IsAtDotDotToken()) return {SyntaxKind::DotDotToken, SyntaxKind::RangeExpression};

    // check for >>, >>=, >>> or >>>=
    //
    // In all those cases, update token1Kind to be the merged token kind.  It will then be handled by the code below.
    if (token1Kind == SyntaxKind::GreaterThanToken) {
        GreenNode *token2 = this->PeekToken(1);
        if ((token2->kind == SyntaxKind::GreaterThanToken || token2->kind == SyntaxKind::GreaterThanEqualsToken) &&
            NoTriviaBetween(token1, token2))  // check to see if they really are adjacent
        {
            if (token2->kind == SyntaxKind::GreaterThanToken) {
                GreenNode *token3 = this->PeekToken(2);
                if ((token3->kind == SyntaxKind::GreaterThanToken || token3->kind == SyntaxKind::GreaterThanEqualsToken) &&
                    NoTriviaBetween(token2, token3))  // check to see if they really are adjacent
                {
                    // >>>  or  >>>=
                    token1Kind = token3->kind == SyntaxKind::GreaterThanToken
                                     ? SyntaxKind::GreaterThanGreaterThanGreaterThanToken
                                     : SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken;
                } else {
                    // >>
                    token1Kind = SyntaxKind::GreaterThanGreaterThanToken;
                }
            } else {
                // >>=
                token1Kind = SyntaxKind::GreaterThanGreaterThanEqualsToken;
            }
        }
    }

    if (IsExpectedBinaryOperator(token1Kind)) return {token1Kind, SyntaxFacts::GetBinaryExpression(token1Kind)};

    if (IsExpectedAssignmentOperator(token1Kind)) return {token1Kind, SyntaxFacts::GetAssignmentExpression(token1Kind)};

    if (token1Kind == SyntaxKind::SwitchKeyword && this->PeekToken(1)->kind == SyntaxKind::OpenBraceToken)
        return {token1Kind, SyntaxKind::SwitchExpression};

    if (token1Kind == SyntaxKind::WithKeyword && this->PeekToken(1)->kind == SyntaxKind::OpenBraceToken)
        return {token1Kind, SyntaxKind::WithExpression};

    // Something that doesn't expand the current expression we're looking at.  Bail out and see if we
    // can end with a conditional expression.
    return {SyntaxKind::None, SyntaxKind::None};
}

GreenNode *LanguageParser::EatExpressionOperatorToken(SyntaxKind operatorTokenKind) {
    // Combine tokens into a single token if needed

    if (operatorTokenKind == SyntaxKind::DotDotToken) return EatDotDotToken();

    if (operatorTokenKind == SyntaxKind::GreaterThanGreaterThanToken ||
        operatorTokenKind == SyntaxKind::GreaterThanGreaterThanEqualsToken) {
        // >> and >>=
        // Two tokens need to be consumed here.

        GreenNode *token1 = EatToken();
        GreenNode *token2 = EatToken();

        return SyntaxFactory::Token(token1->GetLeadingTrivia(), operatorTokenKind, token2->GetTrailingTrivia());
    } else if (operatorTokenKind == SyntaxKind::GreaterThanGreaterThanGreaterThanToken ||
               operatorTokenKind == SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken) {
        // >>> and >>>=
        // Three tokens need to be consumed here.

        GreenNode *token1 = EatToken();
        (void)EatToken();
        GreenNode *token3 = EatToken();

        return SyntaxFactory::Token(token1->GetLeadingTrivia(), operatorTokenKind, token3->GetTrailingTrivia());
    } else {
        // Normal operator.  Eat as a single token, converting contextual words cases (like 'with') to a keyword.
        return this->EatContextualToken(operatorTokenKind);
    }
}

GreenNode *LanguageParser::ParseAssignmentExpression(SyntaxKind operatorExpressionKind, GreenNode *leftOperand,
                                                     GreenNode *operatorToken) {
    CS_ASSERT(IsExpectedAssignmentOperator(operatorToken->kind));
    CS_ASSERT(GetPrecedence(operatorExpressionKind) == Precedence::Assignment);

    GreenNode *rhs;

    if (operatorExpressionKind == SyntaxKind::SimpleAssignmentExpression &&
        CurrentToken()->kind == SyntaxKind::RefKeyword &&
        // check for lambda expression with explicit ref return type: `ref int () => { ... }`
        !this->IsPossibleLambdaExpression(Precedence::Assignment)) {
        GreenNode *refKeyword = this->EatToken();
        GreenNode *expression = this->ParseExpressionCore();
        rhs = SyntaxFactory::RefExpression(refKeyword, expression);
    } else {
        rhs = this->ParseSubExpression(Precedence::Assignment);
    }

    return SyntaxFactory::AssignmentExpression(operatorExpressionKind, leftOperand, operatorToken, rhs);
}

/// Check if we're currently at a .. sequence that can then be parsed out as a DotDotToken.
bool LanguageParser::IsAtDotDotToken() {
    if (this->CurrentToken()->kind != SyntaxKind::DotToken) return false;

    GreenNode *nextToken = this->PeekToken(1);
    return nextToken->kind == SyntaxKind::DotToken && NoTriviaBetween(this->CurrentToken(), nextToken);
}

bool LanguageParser::IsAtDotDotToken(GreenNode *token1, GreenNode *token2) {
    return token1->kind == SyntaxKind::DotToken && token2->kind == SyntaxKind::DotToken &&
           NoTriviaBetween(token1, token2);
}

/// Consume the next two tokens as a DotDotToken.  Note: if three dot tokens are in a row, an error will be
/// placed on the `..` token to say that is illegal, and single DotDot token will be returned.
GreenNode *LanguageParser::EatDotDotToken() {
    CS_ASSERT(IsAtDotDotToken());
    GreenNode *token1 = this->EatToken();
    GreenNode *token2 = this->EatToken();

    GreenNode *dotDotToken =
        SyntaxFactory::Token(token1->GetLeadingTrivia(), SyntaxKind::DotDotToken, token2->GetTrailingTrivia());
    GreenNode *token3 = this->CurrentToken();
    if (token3->kind == SyntaxKind::DotToken && NoTriviaBetween(token2, token3)) {
        // At least three dots directly in a row.  Definitely mark that this is always illegal.  We do not allow
        // `...` at all in case we want to use that syntax in the future.
        dotDotToken = AddError(dotDotToken, /*offset:*/ dotDotToken->GetLeadingTriviaWidth(), /*length:*/ 0,
                               ErrorCode::ERR_TripleDotNotAllowed);

        // If we have exactly 3 dots in a row, then make the third dot into skipped trivia on the `..` as this
        // is likely just a mistyped range/slice and we'll recover better if we don't try to process the 3rd dot
        // as a member access or anything like that.
        //
        // If we have 4 dots in a row (`....`), then don't skip any of them.  We'll let the caller handle the
        // next two dots as a range/slice/whatever.
        GreenNode *token4 = this->PeekToken(1);
        if (token4->kind != SyntaxKind::DotToken || !NoTriviaBetween(token3, token4)) {
            dotDotToken = AddSkippedSyntax(dotDotToken, this->EatToken(), /*trailing:*/ true);
        }
    }

    return dotDotToken;
}

GreenNode *LanguageParser::ParseDeclarationExpression(ParseTypeMode mode, bool isScoped) {
    GreenNode *scopedKeyword = isScoped ? EatContextualToken(SyntaxKind::ScopedKeyword) : nullptr;

    GreenNode *type = this->ParseType(mode);
    GreenNode *declType = scopedKeyword == nullptr ? type : SyntaxFactory::ScopedType(scopedKeyword, type);
    GreenNode *designation = ParseDesignation(/*forPattern:*/ false);
    return SyntaxFactory::DeclarationExpression(declType, designation);
}

GreenNode *LanguageParser::ParseThrowExpression() {
    GreenNode *throwKeyword = this->EatToken(SyntaxKind::ThrowKeyword);
    GreenNode *expression = this->ParseSubExpression(Precedence::Coalescing);
    return SyntaxFactory::ThrowExpression(throwKeyword, expression);
}

GreenNode *LanguageParser::ParseIsExpression(GreenNode *leftOperand, GreenNode *opToken) {
    GreenNode *node = this->ParseTypeOrPatternForIsOperator();
    if (KindDerivesFrom(node->kind, "PatternSyntax"))
        return SyntaxFactory::IsPatternExpression(leftOperand, opToken, node);
    // TypeSyntax type (anything else: C# throws ExceptionUtilities.UnexpectedValue(node))
    CS_ASSERT(KindDerivesFrom(node->kind, "TypeSyntax"));
    return SyntaxFactory::BinaryExpression(SyntaxKind::IsExpression, leftOperand, opToken, node);
}

GreenNode *LanguageParser::ParsePrimaryExpression(Precedence precedence) {
    // Primary expressions:
    // x.y, f(x), a[i], x?.y, x?[y], x++, x--, x!, new, typeof, checked, unchecked, default, nameof, delegate, sizeof, stackalloc, x->y
    //
    // Note that postfix operators (like ++) are still primary expressions, even though their prefix equivalents (`++x`) are unary.

    // (The C# local function takes `precedence` as a parameter; it is only ever called with this method's.)
    auto parsePrimaryExpressionWithoutPostfix = [&]() -> GreenNode * {
        SyntaxKind tk = this->CurrentToken()->kind;
        switch (tk) {
            case SyntaxKind::TypeOfKeyword:
                return this->ParseTypeOfExpression();
            case SyntaxKind::DefaultKeyword:
                return this->ParseDefaultExpression();
            case SyntaxKind::SizeOfKeyword:
                return this->ParseSizeOfExpression();
            case SyntaxKind::MakeRefKeyword:
                return this->ParseMakeRefExpression();
            case SyntaxKind::RefTypeKeyword:
                return this->ParseRefTypeExpression();
            case SyntaxKind::CheckedKeyword:
            case SyntaxKind::UncheckedKeyword:
                return this->ParseCheckedOrUncheckedExpression();
            case SyntaxKind::RefValueKeyword:
                return this->ParseRefValueExpression();
            case SyntaxKind::ColonColonToken:
                // misplaced ::
                // Calling ParseAliasQualifiedName will cause us to create a missing identifier node that then
                // properly consumes the :: and the reset of the alias name afterwards.
                return this->ParseAliasQualifiedName(NameOptions::InExpression);
            case SyntaxKind::EqualsGreaterThanToken:
                return this->ParseLambdaExpression();
            case SyntaxKind::StaticKeyword:
                if (this->IsPossibleAnonymousMethodExpression()) {
                    return this->ParseAnonymousMethodExpression();
                } else if (this->IsPossibleLambdaExpression(precedence)) {
                    return this->ParseLambdaExpression();
                } else {
                    return this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_InvalidExprTerm,
                                          {this->CurrentToken()->text});
                }
            case SyntaxKind::IdentifierToken: {
                if (this->IsTrueIdentifier()) {
                    if (this->IsPossibleAnonymousMethodExpression()) {
                        return this->ParseAnonymousMethodExpression();
                    } else if (GreenNode *lambda = nullptr;
                               this->IsPossibleLambdaExpression(precedence) &&
                               (lambda = this->TryParseLambdaExpression()) != nullptr) {
                        return lambda;
                    } else if (this->IsPossibleDeconstructionLeft(precedence)) {
                        return ParseDeclarationExpression(ParseTypeMode::Normal, /*isScoped:*/ false);
                    } else if (IsCurrentTokenFieldInKeywordContext() &&
                               PeekToken(1)->kind != SyntaxKind::ColonColonToken) {
                        return SyntaxFactory::FieldExpression(this->EatContextualToken(SyntaxKind::FieldKeyword));
                    } else {
                        return this->ParseAliasQualifiedName(NameOptions::InExpression);
                    }
                } else {
                    return this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_InvalidExprTerm,
                                          {this->CurrentToken()->text});
                }
            }
            case SyntaxKind::OpenBracketToken:
                return this->IsPossibleLambdaExpression(precedence) ? this->ParseLambdaExpression()
                                                                     : this->ParseCollectionExpression();
            case SyntaxKind::ThisKeyword:
                return SyntaxFactory::ThisExpression(this->EatToken());
            case SyntaxKind::BaseKeyword:
                return ParseBaseExpression();

            case SyntaxKind::ArgListKeyword:
            case SyntaxKind::FalseKeyword:
            case SyntaxKind::TrueKeyword:
            case SyntaxKind::NullKeyword:
            case SyntaxKind::NumericLiteralToken:
            case SyntaxKind::StringLiteralToken:
            case SyntaxKind::Utf8StringLiteralToken:
            case SyntaxKind::CharacterLiteralToken:
                return SyntaxFactory::LiteralExpression(SyntaxFacts::GetLiteralExpression(tk), this->EatToken());
            case SyntaxKind::InterpolatedStringStartToken:
            case SyntaxKind::InterpolatedVerbatimStringStartToken:
            case SyntaxKind::InterpolatedSingleLineRawStringStartToken:
            case SyntaxKind::InterpolatedMultiLineRawStringStartToken:
                // C#: throw new NotImplementedException(); -- this should not occur because these tokens are
                // produced and parsed immediately. Not reachable; don't abort the editor if it ever is.
                CS_ASSERT(false);
                return this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_InvalidExprTerm,
                                      {this->CurrentToken()->text});
            case SyntaxKind::InterpolatedStringToken:
                return this->ParseInterpolatedStringToken();
            case SyntaxKind::SingleLineRawStringLiteralToken:
            case SyntaxKind::Utf8SingleLineRawStringLiteralToken:
            case SyntaxKind::MultiLineRawStringLiteralToken:
            case SyntaxKind::Utf8MultiLineRawStringLiteralToken:
                return this->ParseRawStringToken();
            case SyntaxKind::OpenParenToken: {
                GreenNode *lambda = nullptr;
                return IsPossibleLambdaExpression(precedence) && (lambda = this->TryParseLambdaExpression()) != nullptr
                           ? lambda
                           : this->ParseCastOrParenExpressionOrTuple();
            }
            case SyntaxKind::NewKeyword:
                return this->ParseNewExpression();
            case SyntaxKind::StackAllocKeyword:
                return this->ParseStackAllocExpression();
            case SyntaxKind::DelegateKeyword:
                // check for lambda expression with explicit function pointer return type
                return this->IsPossibleLambdaExpression(precedence) ? this->ParseLambdaExpression()
                                                                     : this->ParseAnonymousMethodExpression();
            case SyntaxKind::RefKeyword: {
                // check for lambda expression with explicit ref return type: `ref int () => { ... }`
                if (this->IsPossibleLambdaExpression(precedence)) {
                    return this->ParseLambdaExpression();
                }
                // ref is not expected to appear in this position.
                GreenNode *refKeyword = this->EatToken();
                GreenNode *expression = this->ParseExpressionCore();
                return this->AddError(SyntaxFactory::RefExpression(refKeyword, expression), ErrorCode::ERR_InvalidExprTerm,
                                      {SyntaxFacts::GetText(tk)});
            }
            default:
                if (IsPredefinedType(tk)) {
                    if (this->IsPossibleLambdaExpression(precedence)) {
                        return this->ParseLambdaExpression();
                    }

                    // check for intrinsic type followed by '.'
                    GreenNode *expr = SyntaxFactory::PredefinedType(this->EatToken());

                    if (this->CurrentToken()->kind != SyntaxKind::DotToken || tk == SyntaxKind::VoidKeyword) {
                        expr = this->AddError(expr, ErrorCode::ERR_InvalidExprTerm, {SyntaxFacts::GetText(tk)});
                    }

                    return expr;
                } else {
                    GreenNode *expr = this->CreateMissingIdentifierName();

                    if (tk == SyntaxKind::EndOfFileToken) {
                        expr = this->AddError(expr, ErrorCode::ERR_ExpressionExpected);
                    } else if (SyntaxFacts::IsBinaryExpression(tk) || SyntaxFacts::IsAssignmentExpressionOperatorToken(tk)) {
                        // We got into the expression parsing path because we saw an error operator (see the
                        // default case in IsPossibleExpression), knowing we'd create a missing expr which would
                        // then allow the binary/assignment expr parsing to proceed.  In this case, we want to
                        // report the invalid expr, but place it next to the operator, not whatever might have
                        // come arbitrarily far before us.
                        return WithAdditionalDiagnostics(
                            expr, MakeError(/*offset:*/ this->CurrentToken()->GetLeadingTriviaWidth(),
                                            /*width:*/ this->CurrentToken()->Width(), ErrorCode::ERR_InvalidExprTerm,
                                            {SyntaxFacts::GetText(tk)}));
                    } else {
                        expr = this->AddError(expr, ErrorCode::ERR_InvalidExprTerm, {SyntaxFacts::GetText(tk)});
                    }

                    return expr;
                }
        }
    };

    auto parsePostFixExpression = [&](GreenNode *expr) -> GreenNode * {
        CS_ASSERT(expr != nullptr);

        while (true) {
            // If the set of postfix expressions is updated here, please review ParseStatementAttributeDeclarations
            // to see if it may need a similar look-ahead check to determine if something is a collection expression
            // versus an attribute.

            SyntaxKind kind = this->CurrentToken()->kind;
            // case SyntaxKind.DotToken when !IsAtDotDotToken(): (a `..` goes to the default section)
            if (kind == SyntaxKind::DotToken && IsAtDotDotToken()) return expr;
            switch (kind) {
                case SyntaxKind::OpenParenToken:
                    expr = SyntaxFactory::InvocationExpression(expr, this->ParseParenthesizedArgumentList());
                    continue;

                case SyntaxKind::OpenBracketToken:
                    expr = SyntaxFactory::ElementAccessExpression(expr, this->ParseBracketedArgumentList());
                    continue;

                case SyntaxKind::PlusPlusToken:
                case SyntaxKind::MinusMinusToken:
                    expr = SyntaxFactory::PostfixUnaryExpression(
                        SyntaxFacts::GetPostfixUnaryExpression(this->CurrentToken()->kind), expr, this->EatToken());
                    continue;

                case SyntaxKind::ColonColonToken:
                    if (this->PeekToken(1)->kind == SyntaxKind::IdentifierToken) {
                        // replace :: with missing dot and annotate with skipped text "::" and error
                        GreenNode *dot = this->ConvertToMissingWithTrailingTrivia(
                            this->AddError(this->EatToken(), ErrorCode::ERR_UnexpectedAliasedName), SyntaxKind::DotToken);
                        GreenNode *name = this->ParseSimpleName(NameOptions::InExpression);
                        expr = SyntaxFactory::MemberAccessExpression(SyntaxKind::SimpleMemberAccessExpression, expr, dot,
                                                                     name);
                    } else {
                        // just some random trailing :: ?
                        expr = AddTrailingSkippedSyntax(expr, this->EatTokenEvenWithIncorrectKind(SyntaxKind::DotToken));
                    }

                    continue;

                case SyntaxKind::MinusGreaterThanToken: {
                    GreenNode *arrow = this->EatToken();
                    GreenNode *name = this->ParseSimpleName(NameOptions::InExpression);
                    expr = SyntaxFactory::MemberAccessExpression(SyntaxKind::PointerMemberAccessExpression, expr, arrow,
                                                                 name);
                    continue;
                }

                case SyntaxKind::DotToken: {
                    // if we have the error situation:
                    //
                    //      expr.
                    //      X Y
                    //
                    // Then we don't want to parse this out as "Expr.X"
                    //
                    // It's far more likely the member access expression is simply incomplete and
                    // there is a new declaration on the next line.
                    if (SyntaxList(this->CurrentToken()->GetTrailingTrivia()).Any(SyntaxKind::EndOfLineTrivia) &&
                        this->PeekToken(1)->kind == SyntaxKind::IdentifierToken &&
                        this->PeekToken(2)->contextual_kind == SyntaxKind::IdentifierToken) {
                        GreenNode *dot = this->EatToken();
                        return SyntaxFactory::MemberAccessExpression(
                            SyntaxKind::SimpleMemberAccessExpression, expr, dot,
                            this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_IdentifierExpected));
                    }

                    GreenNode *dot = this->EatToken();
                    GreenNode *name = this->ParseSimpleName(NameOptions::InExpression);
                    expr = SyntaxFactory::MemberAccessExpression(SyntaxKind::SimpleMemberAccessExpression, expr, dot, name);
                    continue;
                }

                case SyntaxKind::QuestionToken: {
                    GreenNode *conditionalAccess = nullptr;
                    if (TryParseConditionalAccessExpression(expr, conditionalAccess)) {
                        expr = conditionalAccess;
                        continue;
                    }

                    return expr;
                }

                case SyntaxKind::ExclamationToken:
                    expr = SyntaxFactory::PostfixUnaryExpression(SyntaxKind::SuppressNullableWarningExpression, expr,
                                                                 this->EatToken());
                    continue;

                default:
                    return expr;
            }
        }
    };

    return parsePostFixExpression(parsePrimaryExpressionWithoutPostfix());
}

GreenNode *LanguageParser::ParseBaseExpression() {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::BaseKeyword);
    return SyntaxFactory::BaseExpression(this->EatToken());
}

/// Returns true if...
/// 1. The precedence is less than or equal to Assignment, and
/// 2. The current token is the identifier var or a predefined type, and
/// 3. it is followed by (, and
/// 4. that ( begins a valid parenthesized designation, and
/// 5. the token following that designation is =
bool LanguageParser::IsPossibleDeconstructionLeft(Precedence precedence) {
    if (precedence > Precedence::Assignment ||
        !(SyntaxFacts::IsIdentifierVar(this->CurrentToken()) || IsPredefinedType(this->CurrentToken()->kind))) {
        return false;
    }

    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

    this->EatToken();  // `var`
    return this->CurrentToken()->kind == SyntaxKind::OpenParenToken && ScanDesignator() &&
           this->CurrentToken()->kind == SyntaxKind::EqualsToken;
}

bool LanguageParser::ScanDesignator() {
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::IdentifierToken:
            if (!IsTrueIdentifier()) {
                return false;  // goto default
            }

            this->EatToken();  // eat the identifier
            return true;
        case SyntaxKind::OpenParenToken:
            while (true) {
                this->EatToken();  // eat the open paren or comma
                if (!ScanDesignator()) {
                    return false;
                }

                switch (this->CurrentToken()->kind) {
                    case SyntaxKind::CommaToken:
                        continue;
                    case SyntaxKind::CloseParenToken:
                        this->EatToken();  // eat the close paren
                        return true;
                    default:
                        return false;
                }
            }
        default:
            return false;
    }
}

bool LanguageParser::IsPossibleAnonymousMethodExpression() {
    // Skip past any static/async keywords.
    int tokenIndex = 0;
    while (this->PeekToken(tokenIndex)->kind == SyntaxKind::StaticKeyword ||
           this->PeekToken(tokenIndex)->contextual_kind == SyntaxKind::AsyncKeyword) {
        tokenIndex++;
    }

    return this->PeekToken(tokenIndex)->kind == SyntaxKind::DelegateKeyword &&
           this->PeekToken(tokenIndex + 1)->kind != SyntaxKind::AsteriskToken;
}

/// Called when we could be at a `?` that could start a ConditionalAccessExpressionSyntax or a
/// ConditionalExpressionSyntax.  Returns true if this succeeds at parsing the former, and false if we're not at
/// the start of a conditional access expression.
bool LanguageParser::TryParseConditionalAccessExpression(GreenNode *primaryExpression,
                                                         GreenNode *&conditionalAccessExpression) {
    // From
    // https://github.com/dotnet/csharpstandard/blob/standard-v7/standard/expressions.md#1288-null-conditional-member-access
    // https://github.com/dotnet/csharpstandard/blob/standard-v7/standard/expressions.md#12812-null-conditional-element-access

    // null_conditional_member_access
    //      : primary_expression '?' '.' identifier type_argument_list? dependent_access*
    //      ;
    //
    // null_conditional_element_access
    //      : primary_no_array_creation_expression '?' '[' argument_list ']' dependent_access*
    //      ;
    //
    // dependent_access
    //      : '.' identifier type_argument_list?    // member access
    //      | '[' argument_list ']'                 // element access
    //      | '(' argument_list ? ')'               // invocation
    //      ;

    // We get in here after parsing out the initial primary expression and seeing a `?` follow.

    auto isStartOfElementBindingExpression = [&](SyntaxKind nextTokenKind) -> bool {
        if (nextTokenKind != SyntaxKind::OpenBracketToken) return false;

        // could simply be `x?[0]`, or could be `x ? [0] : [1]`.

        // Caller only wants us to parse ?[ how it was originally parsed before collection expressions.
        if (this->ForceConditionalAccessExpression()) return true;

        DisposableResetPoint _(this, /*resetOnDispose:*/ true);

        // Move past the '?'. Parse what comes next the same way that conditional expressions are parsed.
        this->EatToken();
        this->ParsePossibleRefExpression();

        // If we see a colon, then do not parse this as a conditional-access-expression, pop up to the caller
        // and have it reparse this as a conditional-expression instead.
        return this->CurrentToken()->kind != SyntaxKind::ColonToken;
    };

    // Returns (questionToken, bindingExpression); both null when not at a conditional access.
    auto tryEatQuestionAndBindingExpression = [&]() -> std::pair<GreenNode *, GreenNode *> {
        if (this->CurrentToken()->kind == SyntaxKind::QuestionToken) {
            GreenNode *nextToken = this->PeekToken(1);
            SyntaxKind nextTokenKind = nextToken->kind;

            // ?.   is always the start of of a consequence expression.
            //
            // ?..  is a ternary with a range expression as it's 'whenTrue' clause.
            if (nextTokenKind == SyntaxKind::DotToken && !IsAtDotDotToken(nextToken, this->PeekToken(2))) {
                GreenNode *questionToken = EatToken();
                GreenNode *dotToken = this->EatToken();
                GreenNode *name = this->ParseSimpleName(NameOptions::InExpression);
                return {questionToken, SyntaxFactory::MemberBindingExpression(dotToken, name)};
            }

            if (isStartOfElementBindingExpression(nextTokenKind)) {
                GreenNode *questionToken = EatToken();
                return {questionToken, SyntaxFactory::ElementBindingExpression(this->ParseBracketedArgumentList())};
            }
        }

        // Anything else is either not a `?` at all, or is just a `?` that starts a conditional expression (not
        // a conditional access expression).
        return {nullptr, nullptr};
    };

    auto tryParseDependentAccess = [&](GreenNode *expr) -> GreenNode * {
        switch (this->CurrentToken()->kind) {
            case SyntaxKind::OpenParenToken:
                return SyntaxFactory::InvocationExpression(expr, this->ParseParenthesizedArgumentList());
            case SyntaxKind::OpenBracketToken:
                return SyntaxFactory::ElementAccessExpression(expr, this->ParseBracketedArgumentList());
            case SyntaxKind::DotToken: {
                GreenNode *dot = this->EatToken();
                GreenNode *name = this->ParseSimpleName(NameOptions::InExpression);
                return SyntaxFactory::MemberAccessExpression(SyntaxKind::SimpleMemberAccessExpression, expr, dot, name);
            }
            default:
                return nullptr;
        }
    };

    auto parseWhenNotNull = [&](GreenNode *expr) -> GreenNode * {
        while (true) {
            // We should consume suppression '!'s which are in the middle of the 'whenNotNull', but not at the end.
            // For example, 'a?.b!.c' should be a cond-access whose RHS is '.b!.c',
            // while 'a?.b!' should be a suppression-expr containing a cond-access 'a?.b'.
            DisposableResetPoint beforeSuppressionsResetPoint(this, /*resetOnDispose:*/ false);
            GreenNode *expressionBeforeSuppressions = expr;

            while (this->CurrentToken()->kind == SyntaxKind::ExclamationToken)
                expr = SyntaxFactory::PostfixUnaryExpression(SyntaxKind::SuppressNullableWarningExpression, expr,
                                                             EatToken());

            // Expand to consume the `dependent_access*` continuations.
            if (GreenNode *expandedExpression = tryParseDependentAccess(expr)) {
                expr = expandedExpression;
                continue;
            }

            // A trailing cond-access or assignment is effectively the "end" of the current cond-access node.
            // Due to right-associativity, everything that follows will be included in the child node.
            // e.g. 'a?.b?.c' parses as '(a) ? (.b?.c)'
            // e.g. 'a?.b = c?.d = e?.f' parses as 'a?.b = (c?.d = e?.f)'

            // a?.b?.c
            // a?.b!?.c
            GreenNode *conditionalAccess = nullptr;
            if (TryParseConditionalAccessExpression(expr, conditionalAccess)) return conditionalAccess;

            // a?.b = c
            // a?.b! = c
            auto [operatorTokenKind, operatorExpressionKind] = GetExpressionOperatorTokenKindAndExpressionKind();
            if (IsExpectedAssignmentOperator(operatorTokenKind)) {
                GreenNode *operatorToken = EatExpressionOperatorToken(operatorTokenKind);
                return ParseAssignmentExpression(operatorExpressionKind, expr, operatorToken);
            }

            // End of the cond-access.
            // Any '!' suppressions which followed this are a parent of the cond-access, not a child of it.
            beforeSuppressionsResetPoint.Reset();
            return expressionBeforeSuppressions;
        }
    };

    auto [questionToken, bindingExpression] = tryEatQuestionAndBindingExpression();
    if (questionToken == nullptr || bindingExpression == nullptr) {
        conditionalAccessExpression = nullptr;
        return false;
    }

    GreenNode *whenNotNull = parseWhenNotNull(bindingExpression);
    conditionalAccessExpression = SyntaxFactory::ConditionalAccessExpression(primaryExpression, questionToken, whenNotNull);
    return true;
}

GreenNode *LanguageParser::ParseParenthesizedArgumentList() {
    // (IsIncrementalAndFactoryContextMatches / EatNode: no incremental parsing)

    GreenNode *openToken;
    SeparatedSyntaxList arguments;
    GreenNode *closeToken;
    ParseArgumentList(/*openToken:*/ openToken, /*arguments:*/ arguments, /*closeToken:*/ closeToken,
                      /*openKind:*/ SyntaxKind::OpenParenToken, /*closeKind:*/ SyntaxKind::CloseParenToken);
    return SyntaxFactory::ArgumentList(openToken, arguments, closeToken);
}

GreenNode *LanguageParser::ParseBracketedArgumentList() {
    // (IsIncrementalAndFactoryContextMatches / EatNode: no incremental parsing)

    GreenNode *openToken;
    SeparatedSyntaxList arguments;
    GreenNode *closeToken;
    ParseArgumentList(/*openToken:*/ openToken, /*arguments:*/ arguments, /*closeToken:*/ closeToken,
                      /*openKind:*/ SyntaxKind::OpenBracketToken, /*closeKind:*/ SyntaxKind::CloseBracketToken);
    return SyntaxFactory::BracketedArgumentList(openToken, arguments, closeToken);
}

void LanguageParser::ParseArgumentList(GreenNode *&openToken, SeparatedSyntaxList &arguments, GreenNode *&closeToken,
                                       SyntaxKind openKind, SyntaxKind closeKind) {
    CS_ASSERT(openKind == SyntaxKind::OpenParenToken || openKind == SyntaxKind::OpenBracketToken);
    CS_ASSERT(closeKind == SyntaxKind::CloseParenToken || closeKind == SyntaxKind::CloseBracketToken);
    CS_ASSERT((openKind == SyntaxKind::OpenParenToken) == (closeKind == SyntaxKind::CloseParenToken));
    bool isIndexer = openKind == SyntaxKind::OpenBracketToken;

    static const SkipBadTokensFn skipBadArgumentListTokens =
        [](LanguageParser *self, GreenNode *&open, SyntaxListBuilder list, SyntaxKind expectedKind,
           SyntaxKind closeKind_) -> PostSkipAction {
        if (self->CurrentToken()->kind == SyntaxKind::CloseParenToken ||
            self->CurrentToken()->kind == SyntaxKind::CloseBracketToken ||
            self->CurrentToken()->kind == SyntaxKind::SemicolonToken)
            return PostSkipAction::Abort;

        return self->SkipBadSeparatedListTokensWithExpectedKind(
            open, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleArgumentExpression();
            },
            [](LanguageParser *p, SyntaxKind closeKind__) {
                return p->CurrentToken()->kind == closeKind__ || p->CurrentToken()->kind == SyntaxKind::SemicolonToken;
            },
            expectedKind, closeKind_);
    };

    // convert `[` into `(` or vice versa for error recovery
    openToken = this->CurrentToken()->kind == SyntaxKind::OpenParenToken ||
                        this->CurrentToken()->kind == SyntaxKind::OpenBracketToken
                    ? this->EatTokenAsKind(openKind)
                    : this->EatToken(openKind);

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfArgumentList;

    if (this->CurrentToken()->kind != closeKind && this->CurrentToken()->kind != SyntaxKind::SemicolonToken) {
        if (isIndexer) {
            // An indexer always expects at least one value.
            arguments = ParseCommaSeparatedSyntaxList(
                openToken, SyntaxKind::CloseBracketToken,
                [](LanguageParser *p) { return p->IsPossibleArgumentExpression(); },
                [](LanguageParser *p) { return p->ParseArgumentExpression(/*isIndexer:*/ true); },
                skipBadArgumentListTokens,
                /*allowTrailingSeparator:*/ false,
                /*requireOneElement:*/ false,
                /*allowSemicolonAsSeparator:*/ false);
        } else {
            arguments = ParseCommaSeparatedSyntaxList(
                openToken, SyntaxKind::CloseParenToken,
                [](LanguageParser *p) { return p->IsPossibleArgumentExpression(); },
                [](LanguageParser *p) { return p->ParseArgumentExpression(/*isIndexer:*/ false); },
                skipBadArgumentListTokens,
                /*allowTrailingSeparator:*/ false,
                /*requireOneElement:*/ false,
                /*allowSemicolonAsSeparator:*/ false);
        }
    } else if (isIndexer && this->CurrentToken()->kind == closeKind) {
        // An indexer always expects at least one value. And so we need to give an error
        // for the case where we see only "[]". ParseArgumentExpression gives it.
        SyntaxListBuilder list = _pool.AllocateSeparated();
        list.Add(this->ParseArgumentExpression(isIndexer));
        arguments = _pool.ToListAndFree(list);
    } else {
        arguments = SeparatedSyntaxList();
    }

    _termState = saveTerm;

    // convert `]` into `)` or vice versa for error recovery
    closeToken = this->CurrentToken()->kind == SyntaxKind::CloseParenToken ||
                         this->CurrentToken()->kind == SyntaxKind::CloseBracketToken
                     ? this->EatTokenAsKind(closeKind)
                     : this->EatToken(closeKind);
}

bool LanguageParser::IsEndOfArgumentList() {
    return this->CurrentToken()->kind == SyntaxKind::CloseParenToken ||
           this->CurrentToken()->kind == SyntaxKind::CloseBracketToken;
}

bool LanguageParser::IsPossibleArgumentExpression() {
    return IsValidArgumentRefKindKeyword(this->CurrentToken()->kind) || this->IsPossibleExpression();
}

bool LanguageParser::IsValidArgumentRefKindKeyword(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::RefKeyword:
        case SyntaxKind::OutKeyword:
        case SyntaxKind::InKeyword:
            return true;
        default:
            return false;
    }
}

GreenNode *LanguageParser::ParseArgumentExpression(bool isIndexer) {
    GreenNode *nameColon = nullptr;
    if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken &&
        this->PeekToken(1)->kind == SyntaxKind::ColonToken) {
        GreenNode *name = this->ParseIdentifierName();
        GreenNode *colon = this->EatToken(SyntaxKind::ColonToken);
        nameColon = SyntaxFactory::NameColon(name, colon);
    }

    GreenNode *refKindKeyword = nullptr;
    if (IsValidArgumentRefKindKeyword(this->CurrentToken()->kind) &&
        // check for lambda expression with explicit ref return type: `ref int () => { ... }`
        !(this->CurrentToken()->kind == SyntaxKind::RefKeyword &&
          this->IsPossibleLambdaExpression(Precedence::Expression))) {
        refKindKeyword = this->EatToken();
    }

    GreenNode *expression;

    if (isIndexer && (this->CurrentToken()->kind == SyntaxKind::CommaToken ||
                      this->CurrentToken()->kind == SyntaxKind::CloseBracketToken)) {
        expression = this->ParseIdentifierName(ErrorCode::ERR_ValueExpected);
    } else if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
        expression = this->ParseIdentifierName(ErrorCode::ERR_MissingArgument);
    } else {
        // According to Language Specification, section 7.6.7 Element access
        //      The argument-list of an element-access is not allowed to contain ref or out arguments.
        // However, we actually do support ref indexing of indexed properties in COM interop
        // scenarios, and when indexing an object of static type "dynamic". So we enforce
        // that the ref/out of the argument must match the parameter when binding the argument list.

        expression = refKindKeyword != nullptr && refKindKeyword->kind == SyntaxKind::OutKeyword
                         ? ParseExpressionOrDeclaration(ParseTypeMode::Normal, /*permitTupleDesignation:*/ false)
                         : ParseSubExpression(Precedence::Expression);
    }

    return SyntaxFactory::Argument(nameColon, refKindKeyword, expression);
}

GreenNode *LanguageParser::ParseTypeOfExpression() {
    GreenNode *keyword = this->EatToken();
    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *type = this->ParseTypeOrVoid();
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::TypeOfExpression(keyword, openParen, type, closeParen);
}

GreenNode *LanguageParser::ParseDefaultExpression() {
    GreenNode *keyword = this->EatToken();
    if (this->CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
        GreenNode *type = this->ParseType();
        GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
        return SyntaxFactory::DefaultExpression(keyword, openParen, type, closeParen);
    } else {
        return SyntaxFactory::LiteralExpression(SyntaxKind::DefaultLiteralExpression, keyword);
    }
}

GreenNode *LanguageParser::ParseSizeOfExpression() {
    GreenNode *keyword = this->EatToken();
    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *type = this->ParseType();
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::SizeOfExpression(keyword, openParen, type, closeParen);
}

GreenNode *LanguageParser::ParseMakeRefExpression() {
    GreenNode *keyword = this->EatToken();
    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *expression = this->ParseExpressionForParenthesizedConstruct();
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::MakeRefExpression(keyword, openParen, expression, closeParen);
}

GreenNode *LanguageParser::ParseRefTypeExpression() {
    GreenNode *keyword = this->EatToken();
    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *expression = this->ParseExpressionForParenthesizedConstruct();
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::RefTypeExpression(keyword, openParen, expression, closeParen);
}

GreenNode *LanguageParser::ParseCheckedOrUncheckedExpression() {
    GreenNode *checkedOrUnchecked = this->EatToken();
    CS_ASSERT(checkedOrUnchecked->kind == SyntaxKind::CheckedKeyword ||
              checkedOrUnchecked->kind == SyntaxKind::UncheckedKeyword);
    SyntaxKind kind = checkedOrUnchecked->kind == SyntaxKind::CheckedKeyword ? SyntaxKind::CheckedExpression
                                                                             : SyntaxKind::UncheckedExpression;

    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *expression = this->ParseExpressionForParenthesizedConstruct();
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::CheckedExpression(kind, checkedOrUnchecked, openParen, expression, closeParen);
}

GreenNode *LanguageParser::ParseRefValueExpression() {
    GreenNode *keyword = this->EatToken(SyntaxKind::RefValueKeyword);
    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *expression = this->ParseSubExpression(Precedence::Expression);
    GreenNode *comma = this->EatToken(SyntaxKind::CommaToken);
    GreenNode *type = this->ParseType();
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::RefValueExpression(keyword, openParen, expression, comma, type, closeParen);
}

bool LanguageParser::ScanParenthesizedLambda(Precedence precedence) {
    return ScanImplicitlyTypedLambdaOrSimpleExplicitlyTypedParenthesizedLambda(precedence) ||
           ScanExplicitlyTypedLambda(precedence);
}

/// Scans implicitly typed  lambdas (like `(a, b) =>`) as well as basic explicitly typed lambdas (like
/// `(A a, B b) =>`).  More complex scanning of parenthesized lambdas happens in ScanExplicitlyTypedLambda.
bool LanguageParser::ScanImplicitlyTypedLambdaOrSimpleExplicitlyTypedParenthesizedLambda(Precedence precedence) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::OpenParenToken);

    if (precedence > Precedence::Lambda) return false;

    //  (   ) =>
    //  ( x ) =>           or       ( ref x ) =>
    //  ( x , ... ) =>     or       ( ref x , ...) =>
    int index = 1;

    while (true) {
        GreenNode *token = this->PeekToken(index++);

        // Keep skipping modifiers, commas, and identifiers to consume the rest of the lambda arguments. Note:
        // this *will* grab explicitly typed lambdas like `(A b) =>`.  However, that's ok.  The only caller of
        // this is ScanParenthesizedLambda, which just wants to know if it's on some form of lambda.
        if (this->IsTrueIdentifier(token) || token->kind == SyntaxKind::CommaToken ||
            IsParameterModifierIncludingScoped(token)) {
            continue;
        }

        return token->kind == SyntaxKind::CloseParenToken &&
               this->PeekToken(index)->kind == SyntaxKind::EqualsGreaterThanToken;
    }
}

bool LanguageParser::ScanExplicitlyTypedLambda(Precedence precedence) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::OpenParenToken);

    if (precedence > Precedence::Lambda) {
        return false;
    }

    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

    // Do we have the following, where the attributes, modifier, and type are
    // optional? If so then parse it as a lambda.
    //   (attributes modifier T x [, ...]) =>
    //
    // It's not sufficient to assume this is a lambda expression if we see a
    // modifier such as `(ref x,` because the caller of this method may have
    // scanned past a preceding identifier, and `F (ref x,` might be a call to
    // method F rather than a lambda expression with return type F.
    // Instead, we need to scan to `=>`.

    while (true) {
        // Advance past the open paren or comma.
        this->EatToken();

        ParseAttributeDeclarations(/*inExpressionContext:*/ true);

        if (IsParameterModifierIncludingScoped(this->CurrentToken())) {
            SyntaxListBuilder modifiers = _pool.Allocate();
            ParseParameterModifiers(modifiers, /*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ true);
            _pool.Free(modifiers);
        }

        if (ShouldParseLambdaParameterType() && this->ScanType() == ScanTypeFlags::NotType) {
            return false;
        }

        // eat the parameter name.
        GreenNode *identifier = this->IsTrueIdentifier() ? this->EatToken() : CreateMissingIdentifierToken();
        (void)identifier;

        GreenNode *equalsToken = TryEatToken(SyntaxKind::EqualsToken);

        // If we have an `=` then parse out a default value.  Note: this is not legal, but this allows us to
        // to be resilient to the user writing this so we don't go completely off the rails.
        if (equalsToken != nullptr) {
            // Note: we don't do this if we have `=[`.  Realistically, this is never going to be a lambda
            // expression as a `[` can only start an attribute declaration or collection expression, neither of
            // which can be a default arg.  Checking for this helps us from going off the rails in pathological
            // cases with lots of nested tokens that look like the could be anything.
            if (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
                return false;
            }

            this->ParseExpressionCore();
        }

        switch (this->CurrentToken()->kind) {
            case SyntaxKind::CommaToken:
                continue;

            case SyntaxKind::CloseParenToken:
                return this->PeekToken(1)->kind == SyntaxKind::EqualsGreaterThanToken;

            default:
                return false;
        }
    }
}

GreenNode *LanguageParser::ParseCastOrParenExpressionOrTuple() {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::OpenParenToken);

    DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ false);

    // We have a decision to make -- is this a cast, or is it a parenthesized
    // expression?  Because look-ahead is cheap with our token stream, we check
    // to see if this "looks like" a cast (without constructing any parse trees)
    // to help us make the decision.
    if (this->ScanCast(/*forPattern:*/ false, /*inSwitchArmPattern:*/ false)) {
        if (!IsCurrentTokenQueryKeywordInQuery()) {
            // Looks like a cast, so parse it as one.
            resetPoint.Reset();
            GreenNode *openParenToken = this->EatToken(SyntaxKind::OpenParenToken);
            GreenNode *type = this->ParseType();
            GreenNode *closeParenToken = this->EatToken(SyntaxKind::CloseParenToken);
            GreenNode *operand = this->ParseSubExpression(Precedence::Cast);
            return SyntaxFactory::CastExpression(openParenToken, type, closeParenToken, operand);
        }
    }

    // Doesn't look like a cast, so parse this as a parenthesized expression or tuple.
    resetPoint.Reset();
    GreenNode *openParen = this->EatToken(SyntaxKind::OpenParenToken);
    GreenNode *expression = this->ParseExpressionOrDeclaration(ParseTypeMode::FirstElementOfPossibleTupleLiteral,
                                                               /*permitTupleDesignation:*/ true);

    //  ( <expr>,    must be a tuple
    if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
        return ParseTupleExpressionTail(
            openParen, SyntaxFactory::Argument(/*nameColon:*/ nullptr, /*refKindKeyword:*/ nullptr, expression));
    }

    // ( name:
    if (expression->kind == SyntaxKind::IdentifierName && this->CurrentToken()->kind == SyntaxKind::ColonToken) {
        GreenNode *nameColon = SyntaxFactory::NameColon(expression, EatToken());
        GreenNode *argumentExpression = this->ParseExpressionOrDeclaration(
            ParseTypeMode::FirstElementOfPossibleTupleLiteral, /*permitTupleDesignation:*/ true);
        return ParseTupleExpressionTail(
            openParen, SyntaxFactory::Argument(nameColon, /*refKindKeyword:*/ nullptr, argumentExpression));
    }

    GreenNode *innerExpression = this->ParseErrantExpressionWhenNoCloseParenToken(expression);
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::ParenthesizedExpression(openParen, innerExpression, closeParen);
}

GreenNode *LanguageParser::ParseTupleExpressionTail(GreenNode *openParen, GreenNode *firstArg) {
    SyntaxListBuilder list = _pool.AllocateSeparated();
    list.Add(firstArg);

    while (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
        list.AddSeparator(this->EatToken(SyntaxKind::CommaToken));

        GreenNode *expression =
            ParseExpressionOrDeclaration(ParseTypeMode::AfterTupleComma, /*permitTupleDesignation:*/ true);
        GreenNode *argument;
        if (expression->kind != SyntaxKind::IdentifierName || this->CurrentToken()->kind != SyntaxKind::ColonToken) {
            argument = SyntaxFactory::Argument(/*nameColon:*/ nullptr, /*refKindKeyword:*/ nullptr,
                                               /*expression:*/ expression);
        } else {
            GreenNode *nameColon = SyntaxFactory::NameColon(expression, EatToken());
            GreenNode *argumentExpression =
                ParseExpressionOrDeclaration(ParseTypeMode::AfterTupleComma, /*permitTupleDesignation:*/ true);
            argument = SyntaxFactory::Argument(nameColon, /*refKindKeyword:*/ nullptr, argumentExpression);
        }

        list.Add(argument);
    }

    if (list.Count() < 2) {
        list.AddSeparator(SyntaxFactory::MissingToken(SyntaxKind::CommaToken));
        list.Add(SyntaxFactory::Argument(
            /*nameColon:*/ nullptr, /*refKindKeyword:*/ nullptr,
            this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_TupleTooFewElements)));
    }

    GreenNode *arguments = _pool.ToListAndFree(list);
    GreenNode *closeParen = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::TupleExpression(openParen, arguments, closeParen);
}

}  // namespace csharp
