// LanguageParser, unit F (part 1): a translation of Roslyn's
// src/Compilers/CSharp/Portable/Parser/LanguageParser.cs (MIT, (c) .NET
// Foundation and Contributors) from ScanCast to the end of the file: casts,
// `new` expressions, collection expressions, initializers, stackalloc,
// anonymous methods, lambdas, query expressions, ParseCommaSeparatedSyntaxList,
// the reset points, ConsumeUnexpectedTokens and ContainsErrorDiagnostic.
// See csharp_parser_internal.h for the translation conventions.
//
// C# evaluates call arguments left to right and C++ does not, so every
// factory call whose arguments consume tokens takes them from locals computed
// in the C# order.
#include <utility>
#include <vector>

#include "csharp_parser_internal.h"

namespace csharp {

using K = SyntaxKind;

bool LanguageParser::ScanCast(bool forPattern, bool inSwitchArmPattern) {
    CS_ASSERT(!inSwitchArmPattern || forPattern);

    if (this->CurrentToken()->kind != K::OpenParenToken) {
        return false;
    }

    this->EatToken();

    auto type = this->ScanType(forPattern);
    if (type == ScanTypeFlags::NotType) {
        return false;
    }

    if (this->CurrentToken()->kind != K::CloseParenToken) {
        return false;
    }

    this->EatToken();

    auto isBinaryPatternKeyword = [&]() -> bool {
        return this->CurrentToken()->contextual_kind == K::OrKeyword ||
               this->CurrentToken()->contextual_kind == K::AndKeyword;
    };

    auto isBinaryPattern = [&]() -> bool {
        if (!isBinaryPatternKeyword()) {
            return false;
        }

        bool lastTokenIsBinaryOperator = true;

        EatToken();
        while (isBinaryPatternKeyword()) {
            // If we see a subsequent binary pattern token, it can't be an operator.
            // Later, it will be parsed as an identifier.
            lastTokenIsBinaryOperator = !lastTokenIsBinaryOperator;
            EatToken();
        }

        // In case a combinator token is used as a constant, we explicitly check that a pattern is NOT followed.
        // Such as `(e is (int)or or >= 0)` versus `(e is (int) or or)`
        return lastTokenIsBinaryOperator == IsPossibleSubpatternElement();
    };

    if (forPattern && this->CurrentToken()->kind == K::IdentifierToken) {
        // In a pattern we might have a cast of a constant, or the start of a legal pattern form.
        //
        // For example: `(A.B) and ...` should be treated not as a 'cast' of some variable 'and', but instead as
        // a conjunctive pattern.
        if (isBinaryPattern()) return false;

        // Similarly `(A.B) when` directly in a switch arm should be treated as the start of a `when` clause not
        // a cast of a `when` variable when in a switch expression arm.  This matches the exact checking logic
        // in IsValidPatternDesignation.
        if (inSwitchArmPattern && this->CurrentToken()->contextual_kind == K::WhenKeyword) return false;

        return true;
    }

    switch (type) {
        // If we have any of the following, we know it must be a cast:
        // 1) (Goo*)bar;
        // 2) (Goo?)bar;
        // 3) "(int)bar" or "(int[])bar"
        // 4) (G::Goo)bar
        case ScanTypeFlags::PointerOrMultiplication:
        case ScanTypeFlags::NullableType:
        case ScanTypeFlags::MustBeType:
        case ScanTypeFlags::AliasQualifiedName: {
            // The thing between parens is unambiguously a type.
            // In a pattern, we need more lookahead to confirm it is a cast and not
            // a parenthesized type pattern.  In this case the tokens that
            // have both unary and binary operator forms may appear in their unary form
            // following a cast.
            if (!forPattern) return true;
            auto tk = this->CurrentToken()->kind;
            switch (tk) {
                case K::PlusToken:
                case K::MinusToken:
                case K::AmpersandToken:
                case K::AsteriskToken:
                    return true;
                default:
                    break;
            }
            // `(X)..` must be a cast of a range expression, not a member access of some arbitrary expression.
            if (tk == K::DotToken && IsAtDotDotToken()) return true;
            return CanFollowCast(tk);
        }

        case ScanTypeFlags::GenericTypeOrMethod:
        case ScanTypeFlags::TupleType:
            // If we have `(X<Y>)[...` then we know this must be a cast of a collection expression, not an index
            // into some expr. As most collections are generic, the common case is not ambiguous.
            //
            // Things are still ambiguous if you have `(X)[...` and for back compat we still parse that as
            // indexing into an expression.  The user can still write `(X)([...` in this case though to get cast
            // parsing. As non-generic casts are the rare case for collection expressions, this gives a good
            // balance of back compat and user ease for the normal case.
            return this->CurrentToken()->kind == K::OpenBracketToken || CanFollowCast(this->CurrentToken()->kind);

        case ScanTypeFlags::GenericTypeOrExpression:
        case ScanTypeFlags::NonGenericTypeOrExpression:
            // if we have `(A)[]` then treat that always as a cast of an empty collection expression.  `[]` is not
            // legal on the RHS in any other circumstances for a parenthesized expr.
            if (this->CurrentToken()->kind == K::OpenBracketToken && this->PeekToken(1)->kind == K::CloseBracketToken) {
                return true;
            }

            // check for ambiguous type or expression followed by disambiguating token.  i.e.
            //
            // "(A)b" is a cast.  But "(A)+b" is not a cast.
            return CanFollowCast(this->CurrentToken()->kind);

        default:
            // throw ExceptionUtilities.UnexpectedValue(type) -- unreachable (NotType returned above).
            CS_ASSERT(false);
            return false;
    }
}

/// Tokens that match the following are considered a possible lambda expression:
/// <code>attribute-list* ('async' | 'static')* type? ('(' | identifier) ...</code>
/// For better error recovery 'static =>' is also considered a possible lambda expression.
bool LanguageParser::IsPossibleLambdaExpression(Precedence precedence) {
    if (precedence > Precedence::Lambda) {
        return false;
    }

    auto token1 = this->PeekToken(1);

    // x =>
    //
    // Def a lambda.
    if (token1->kind == K::EqualsGreaterThanToken) {
        return true;
    }

    DisposableResetPoint _(this, /*resetOnDispose*/ true);

    // A lambda could be starting with attributes, attempt to skip past them and check after that point.
    if (CurrentToken()->kind == K::OpenBracketToken) {
        // Subtle case to deal with.  Consider:
        //
        // [X, () => {}       vs:
        // [X] () => {}
        //
        // The former is a collection expression, the latter an attributed-lambda.  However, we will likely
        // successfully parse out `[X,` as an incomplete attribute, and thus think the former is the latter. So,
        // to ensure proper parsing of the collection expressions, bail out if the attribute is not complete.
        SyntaxList attributeDeclarations = ParseAttributeDeclarations(/*inExpressionContext*/ true);
        if (attributeDeclarations.Count() > 0) {
            GreenNode *closeBracket = GetField(attributeDeclarations.Last(), Field::CloseBracketToken);
            if (closeBracket != nullptr && closeBracket->IsMissing()) return false;
        }
    }

    bool seenStatic;
    if (this->CurrentToken()->kind == K::StaticKeyword) {
        EatToken();
        seenStatic = true;
    } else if (this->CurrentToken()->contextual_kind == K::AsyncKeyword && this->PeekToken(1)->kind == K::StaticKeyword) {
        EatToken();
        EatToken();
        seenStatic = true;
    } else {
        seenStatic = false;
    }

    if (seenStatic) {
        if (this->CurrentToken()->kind == K::EqualsGreaterThanToken) {
            // 1. `static =>`
            // 2. `async static =>`

            // This is an error case, but we have enough code in front of us to be certain
            // the user was trying to write a static lambda.
            return true;
        }

        if (this->CurrentToken()->kind == K::OpenParenToken) {
            // 1. `static (...
            // 2. `async static (...
            return true;
        }
    }

    if (this->CurrentToken()->kind == K::IdentifierToken && this->PeekToken(1)->kind == K::EqualsGreaterThanToken) {
        // 1. `a => ...`
        // 1. `static a => ...`
        // 2. `async static a => ...`
        return true;
    }

    // Have checked all the static forms.  And have checked for the basic `a => a` form.
    // At this point we have must be on 'async' or an explicit return type for this to still be a lambda.
    if (this->CurrentToken()->contextual_kind == K::AsyncKeyword && IsAnonymousFunctionAsyncModifier()) {
        EatToken();
    }

    {
        DisposableResetPoint nestedResetPoint(this, /*resetOnDispose*/ false);
        auto st = ScanType();
        if (st == ScanTypeFlags::NotType || this->CurrentToken()->kind != K::OpenParenToken) {
            nestedResetPoint.Reset();
        }
    }

    // However, just because we're on `async` doesn't mean we're a lambda.  We might have
    // something lambda-like like:
    //
    //      async a => ...  // or
    //      async (a) => ...
    //
    // Or we could have something that isn't a lambda like:
    //
    //      async ();

    // 'async <identifier> => ...' looks like an async simple lambda
    if (this->CurrentToken()->kind == K::IdentifierToken && this->PeekToken(1)->kind == K::EqualsGreaterThanToken) {
        // async a => ...
        return true;
    }

    // Non-simple async lambda must be of the form 'async (...'
    if (this->CurrentToken()->kind != K::OpenParenToken) {
        return false;
    }

    // Check whether looks like implicitly or explicitly typed lambda
    return ScanParenthesizedLambda(precedence);
}

bool LanguageParser::CanFollowCast(SyntaxKind kind) {
    switch (kind) {
        case K::AsKeyword:
        case K::IsKeyword:
        case K::SemicolonToken:
        case K::CloseParenToken:
        case K::CloseBracketToken:
        case K::OpenBraceToken:
        case K::CloseBraceToken:
        case K::CommaToken:
        case K::EqualsToken:
        case K::PlusEqualsToken:
        case K::MinusEqualsToken:
        case K::AsteriskEqualsToken:
        case K::SlashEqualsToken:
        case K::PercentEqualsToken:
        case K::AmpersandEqualsToken:
        case K::CaretEqualsToken:
        case K::BarEqualsToken:
        case K::LessThanLessThanEqualsToken:
        case K::GreaterThanGreaterThanEqualsToken:
        case K::GreaterThanGreaterThanGreaterThanEqualsToken:
        case K::QuestionToken:
        case K::ColonToken:
        case K::BarBarToken:
        case K::AmpersandAmpersandToken:
        case K::BarToken:
        case K::CaretToken:
        case K::AmpersandToken:
        case K::EqualsEqualsToken:
        case K::ExclamationEqualsToken:
        case K::LessThanToken:
        case K::LessThanEqualsToken:
        case K::GreaterThanToken:
        case K::GreaterThanEqualsToken:
        case K::QuestionQuestionEqualsToken:
        case K::LessThanLessThanToken:
        case K::GreaterThanGreaterThanToken:
        case K::GreaterThanGreaterThanGreaterThanToken:
        case K::PlusToken:
        case K::MinusToken:
        case K::AsteriskToken:
        case K::SlashToken:
        case K::PercentToken:
        case K::PlusPlusToken:
        case K::MinusMinusToken:
        case K::OpenBracketToken:
        case K::DotToken:
        case K::MinusGreaterThanToken:
        case K::QuestionQuestionToken:
        case K::EndOfFileToken:
        case K::SwitchKeyword:
        case K::EqualsGreaterThanToken:
            return false;
        default:
            return true;
    }
}

GreenNode *LanguageParser::ParseNewExpression() {
    CS_ASSERT(this->CurrentToken()->kind == K::NewKeyword);

    if (this->IsAnonymousType()) {
        return this->ParseAnonymousTypeExpression();
    } else if (this->IsImplicitlyTypedArray()) {
        return this->ParseImplicitlyTypedArrayCreation();
    } else {
        // assume object creation as default case
        return this->ParseArrayOrObjectCreationExpression();
    }
}

GreenNode *LanguageParser::ParseCollectionExpression() {
    CS_ASSERT(this->CurrentToken()->kind == K::OpenBracketToken);
    auto skipBadCollectionElementTokens = [](LanguageParser *self, GreenNode *&openBracket, SyntaxListBuilder list,
                                             SyntaxKind expectedKind, SyntaxKind closeKind) -> PostSkipAction {
        return self->SkipBadSeparatedListTokensWithExpectedKind(
            openBracket, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != K::CommaToken && !p->IsPossibleCollectionElement();
            },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    auto openBracket = this->EatToken(K::OpenBracketToken);
    auto list = this->ParseCommaSeparatedSyntaxList(
        openBracket, K::CloseBracketToken, [](LanguageParser *self) { return self->IsPossibleCollectionElement(); },
        [](LanguageParser *self) { return self->ParseCollectionElement(); }, skipBadCollectionElementTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBracket = this->EatToken(K::CloseBracketToken);
    return SyntaxFactory::CollectionExpression(openBracket, list, closeBracket);
}

bool LanguageParser::IsPossibleCollectionElement() { return this->IsPossibleExpression(); }

GreenNode *LanguageParser::ParseCollectionElement() {
    // Even though `with(` could start a legal expression (like `with(x) + y`), spec mandates that if we see
    // `with(` at the start of a collection element, we only parse it as a with-element.
    if (this->CurrentToken()->contextual_kind == K::WithKeyword && this->PeekToken(1)->kind == K::OpenParenToken) {
        auto withKeyword = this->EatContextualToken(K::WithKeyword);
        auto argumentList = this->ParseParenthesizedArgumentList();
        return SyntaxFactory::WithElement(withKeyword, argumentList);
    }

    // Like above, even though `..` could start a legal expression (like `..` (a naked-range)), the spec
    // mandates that if we see `..` at the start of a collection element, we only parse it as a spread-element.
    if (this->IsAtDotDotToken()) {
        auto dotDot = this->EatDotDotToken();
        auto expression = this->ParseExpressionCore();
        return SyntaxFactory::SpreadElement(dotDot, expression);
    }

    return SyntaxFactory::ExpressionElement(this->ParseExpressionCore());
}

bool LanguageParser::IsAnonymousType() {
    return this->CurrentToken()->kind == K::NewKeyword && this->PeekToken(1)->kind == K::OpenBraceToken;
}

GreenNode *LanguageParser::ParseAnonymousTypeExpression() {
    CS_ASSERT(IsAnonymousType());
    auto new_ = this->EatToken(K::NewKeyword);

    CS_ASSERT(this->CurrentToken()->kind == K::OpenBraceToken);

    auto openBrace = this->EatToken(K::OpenBraceToken);
    auto expressions = ParseCommaSeparatedSyntaxList(
        openBrace, K::CloseBraceToken, [](LanguageParser *self) { return self->IsPossibleExpression(); },
        [](LanguageParser *self) { return self->ParseAnonymousTypeMemberInitializer(); }, SkipBadInitializerListTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBrace = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::AnonymousObjectCreationExpression(new_, openBrace, expressions, closeBrace);
}

GreenNode *LanguageParser::ParseAnonymousTypeMemberInitializer() {
    GreenNode *nameEquals = this->IsNamedAssignment() ? ParseNameEquals() : nullptr;
    auto expression = this->ParseExpressionCore();
    return SyntaxFactory::AnonymousObjectMemberDeclarator(nameEquals, expression);
}

bool LanguageParser::IsInitializerMember() {
    return this->IsComplexElementInitializer() || this->IsNamedAssignment() || this->IsDictionaryInitializer() ||
           this->IsPossibleExpression();
}

bool LanguageParser::IsComplexElementInitializer() { return this->CurrentToken()->kind == K::OpenBraceToken; }

bool LanguageParser::IsNamedAssignment() { return IsTrueIdentifier() && this->PeekToken(1)->kind == K::EqualsToken; }

bool LanguageParser::IsNamedMemberInitializer() {
    return IsTrueIdentifier() && (this->PeekToken(1)->kind == K::EqualsToken || this->PeekToken(1)->kind == K::ColonToken);
}

bool LanguageParser::IsDictionaryInitializer() { return this->CurrentToken()->kind == K::OpenBracketToken; }

GreenNode *LanguageParser::ParseArrayOrObjectCreationExpression() {
    GreenNode *new_ = this->EatToken(K::NewKeyword);

    GreenNode *type = nullptr;
    GreenNode *initializer = nullptr;

    if (!IsImplicitObjectCreation()) {
        type = this->ParseType(ParseTypeMode::NewExpression);
        if (type->kind == K::ArrayType) {
            // Check for an initializer.
            if (this->CurrentToken()->kind == K::OpenBraceToken) {
                initializer = this->ParseArrayInitializer();
            }

            return SyntaxFactory::ArrayCreationExpression(new_, type, initializer);
        }
    }

    GreenNode *argumentList = nullptr;
    if (this->CurrentToken()->kind == K::OpenParenToken) {
        argumentList = this->ParseParenthesizedArgumentList();
    }

    if (this->CurrentToken()->kind == K::OpenBraceToken) {
        initializer = this->ParseObjectOrCollectionInitializer();
    }

    // we need one or the other.  also, don't bother reporting this if we already complained about the new type.
    if (argumentList == nullptr && initializer == nullptr) {
        auto openParen = this->EatToken(K::OpenParenToken, ErrorCode::ERR_BadNewExpr,
                                        /*reportError*/ type != nullptr && !type->ContainsDiagnostics());
        argumentList = SyntaxFactory::ArgumentList(openParen, nullptr, SyntaxFactory::MissingToken(K::CloseParenToken));
    }

    return type == nullptr ? SyntaxFactory::ImplicitObjectCreationExpression(new_, argumentList, initializer)
                           : SyntaxFactory::ObjectCreationExpression(new_, type, argumentList, initializer);
}

bool LanguageParser::IsImplicitObjectCreation() {
    // The caller is expected to have consumed the new keyword.
    if (this->CurrentToken()->kind != K::OpenParenToken) {
        return false;
    }

    DisposableResetPoint _1(this, /*resetOnDispose*/ true);

    this->EatToken();  // open paren
    GreenNode *unused = nullptr;
    ScanTypeFlags scanTypeFlags = ScanTupleType(unused);
    if (scanTypeFlags != ScanTypeFlags::NotType) {
        switch (this->CurrentToken()->kind) {
            case K::QuestionToken:     // e.g. `new(a, b)?()`
            case K::OpenBracketToken:  // e.g. `new(a, b)[]`
            case K::OpenParenToken:    // e.g. `new(a, b)()` for better error recovery
                return false;
            default:
                break;
        }
    }

    return true;
}

GreenNode *LanguageParser::ParseWithExpression(GreenNode *receiverExpression, GreenNode *withKeyword) {
    auto openBrace = this->EatToken(K::OpenBraceToken);
    auto list = this->ParseCommaSeparatedSyntaxList(
        openBrace, K::CloseBraceToken, [](LanguageParser *self) { return self->IsPossibleExpression(); },
        [](LanguageParser *self) { return self->ParseExpressionCore(); }, SkipBadInitializerListTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBrace = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::WithExpression(
        receiverExpression, withKeyword,
        SyntaxFactory::InitializerExpression(K::WithInitializerExpression, openBrace, list, closeBrace));
}

GreenNode *LanguageParser::ParseObjectOrCollectionInitializer() {
    auto isObjectInitializer = [](SeparatedSyntaxList initializers) -> bool {
        // Empty initializer list must be parsed as an object initializer.
        if (initializers.Count() == 0) return true;

        // We have at least one initializer expression. If at least one initializer expression is a named
        // assignment, this is an object initializer. Otherwise, this is a collection initializer.
        for (int i = 0, n = initializers.Count(); i < n; i++) {
            GreenNode *init = initializers[i];
            if (init != nullptr && init->kind == K::SimpleAssignmentExpression) {
                GreenNode *left = GetField(init, Field::Left);
                if (left != nullptr && (left->kind == K::IdentifierName || left->kind == K::ImplicitElementAccess)) {
                    return true;
                }
            }
        }

        return false;
    };

    auto openBrace = this->EatToken(K::OpenBraceToken);

    auto initializers = this->ParseCommaSeparatedSyntaxList(
        openBrace, K::CloseBraceToken, [](LanguageParser *self) { return self->IsInitializerMember(); },
        [](LanguageParser *self) { return self->ParseObjectOrCollectionInitializerMember(); },
        SkipBadInitializerListTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ true);

    auto kind = isObjectInitializer(initializers) ? K::ObjectInitializerExpression : K::CollectionInitializerExpression;

    auto closeBrace = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::InitializerExpression(kind, openBrace, initializers, closeBrace);
}

GreenNode *LanguageParser::ParseObjectOrCollectionInitializerMember() {
    if (this->IsComplexElementInitializer()) {
        // { ... }
        return this->ParseComplexElementInitializer();
    } else if (IsDictionaryInitializer()) {
        // [...] = { ... }
        // [...] = ref <expr>
        // [...] = <expr>
        return this->ParseDictionaryInitializer();
    } else if (this->IsNamedMemberInitializer()) {
        // Name = { ... }
        // Name = ref <expr>
        // Name =  <expr>
        return this->ParseObjectInitializerNamedAssignment();
    } else {
        // <expr>
        // ref <expr>
        return this->ParsePossibleRefExpression();
    }
}

LanguageParser::PostSkipAction LanguageParser::SkipBadInitializerListTokens(LanguageParser *self, GreenNode *&startToken,
                                                            SyntaxListBuilder list, SyntaxKind expectedKind,
                                                            SyntaxKind closeKind) {
    return self->SkipBadSeparatedListTokensWithExpectedKind(
        startToken, list,
        [](LanguageParser *p) { return p->CurrentToken()->kind != K::CommaToken && !p->IsPossibleExpression(); },
        [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; }, expectedKind,
        closeKind);
}

GreenNode *LanguageParser::ParseObjectInitializerNamedAssignment() {
    auto left = this->ParseIdentifierName();
    auto equals = this->CurrentToken()->kind == K::ColonToken ? this->EatTokenAsKind(K::EqualsToken)
                                                              : this->EatToken(K::EqualsToken);
    auto right = this->CurrentToken()->kind == K::OpenBraceToken ? this->ParseObjectOrCollectionInitializer()
                                                                 : this->ParsePossibleRefExpression();
    return SyntaxFactory::AssignmentExpression(K::SimpleAssignmentExpression, left, equals, right);
}

GreenNode *LanguageParser::ParseDictionaryInitializer() {
    auto left = SyntaxFactory::ImplicitElementAccess(this->ParseBracketedArgumentList());
    auto equals = this->EatToken(K::EqualsToken);
    auto right = this->CurrentToken()->kind == K::OpenBraceToken ? this->ParseObjectOrCollectionInitializer()
                                                                 : this->ParsePossibleRefExpression();
    return SyntaxFactory::AssignmentExpression(K::SimpleAssignmentExpression, left, equals, right);
}

GreenNode *LanguageParser::ParseComplexElementInitializer() {
    auto openBrace = this->EatToken(K::OpenBraceToken);

    auto initializers = this->ParseCommaSeparatedSyntaxList(
        openBrace, K::CloseBraceToken, [](LanguageParser *self) { return self->IsPossibleExpression(); },
        [](LanguageParser *self) { return self->ParseExpressionCore(); }, SkipBadInitializerListTokens,
        /*allowTrailingSeparator*/ false,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBrace = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::InitializerExpression(K::ComplexElementInitializerExpression, openBrace, initializers,
                                                closeBrace);
}

bool LanguageParser::IsImplicitlyTypedArray() {
    CS_ASSERT(this->CurrentToken()->kind == K::NewKeyword || this->CurrentToken()->kind == K::StackAllocKeyword);
    return this->PeekToken(1)->kind == K::OpenBracketToken;
}

GreenNode *LanguageParser::ParseImplicitlyTypedArrayCreation() {
    auto new_ = this->EatToken(K::NewKeyword);
    auto openBracket = this->EatToken(K::OpenBracketToken);

    auto commas = _pool.Allocate();

    int lastTokenPosition = -1;
    while (IsMakingProgress(lastTokenPosition)) {
        if (this->IsPossibleExpression()) {
            auto size = this->AddError(this->ParseExpressionCore(), ErrorCode::ERR_InvalidArray);
            if (commas.Count() == 0) {
                openBracket = AddTrailingSkippedSyntax(openBracket, size);
            } else {
                AddTrailingSkippedSyntax(commas, size);
            }
        }

        if (this->CurrentToken()->kind == K::CommaToken) {
            commas.Add(this->EatToken());
            continue;
        }

        break;
    }

    GreenNode *commaList = _pool.ToListAndFree(commas);
    auto closeBracket = this->EatToken(K::CloseBracketToken);
    auto initializer = this->ParseArrayInitializer();
    return SyntaxFactory::ImplicitArrayCreationExpression(new_, openBracket, commaList, closeBracket, initializer);
}

GreenNode *LanguageParser::ParseArrayInitializer() {
    auto skipBadArrayInitializerTokens = [](LanguageParser *self, GreenNode *&openBrace, SyntaxListBuilder list,
                                            SyntaxKind expectedKind, SyntaxKind closeKind) -> PostSkipAction {
        return self->SkipBadSeparatedListTokensWithExpectedKind(
            openBrace, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != K::CommaToken && !p->IsPossibleVariableInitializer();
            },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    auto openBrace = this->EatToken(K::OpenBraceToken);
    auto list = this->ParseCommaSeparatedSyntaxList(
        openBrace, K::CloseBraceToken, [](LanguageParser *self) { return self->IsPossibleVariableInitializer(); },
        [](LanguageParser *self) { return self->ParseVariableInitializer(); }, skipBadArrayInitializerTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBrace = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::InitializerExpression(K::ArrayInitializerExpression, openBrace, list, closeBrace);
}

GreenNode *LanguageParser::ParseStackAllocExpression() {
    return this->IsImplicitlyTypedArray() ? ParseImplicitlyTypedStackAllocExpression()
                                          : ParseRegularStackAllocExpression();
}

GreenNode *LanguageParser::ParseImplicitlyTypedStackAllocExpression() {
    auto stackalloc = this->EatToken(K::StackAllocKeyword);
    auto openBracket = this->EatToken(K::OpenBracketToken);

    int lastTokenPosition = -1;
    while (IsMakingProgress(lastTokenPosition)) {
        if (this->IsPossibleExpression()) {
            auto size = this->AddError(this->ParseExpressionCore(), ErrorCode::ERR_InvalidStackAllocArray);
            openBracket = AddTrailingSkippedSyntax(openBracket, size);
        }

        if (this->CurrentToken()->kind == K::CommaToken) {
            auto comma = this->AddError(this->EatToken(), ErrorCode::ERR_InvalidStackAllocArray);
            openBracket = AddTrailingSkippedSyntax(openBracket, comma);
            continue;
        }

        break;
    }

    auto closeBracket = this->EatToken(K::CloseBracketToken);
    auto initializer = this->ParseArrayInitializer();
    return SyntaxFactory::ImplicitStackAllocArrayCreationExpression(stackalloc, openBracket, closeBracket, initializer);
}

GreenNode *LanguageParser::ParseRegularStackAllocExpression() {
    auto stackalloc = this->EatToken(K::StackAllocKeyword);
    auto type = this->ParseType();
    GreenNode *initializer = this->CurrentToken()->kind == K::OpenBraceToken ? this->ParseArrayInitializer() : nullptr;
    return SyntaxFactory::StackAllocArrayCreationExpression(stackalloc, type, initializer);
}

GreenNode *LanguageParser::ParseAnonymousMethodExpression() {
    auto modifiers = ParseAnonymousFunctionModifiers();
    ParserSyntaxContextResetter _(this, /*isInAsyncContext*/ this->IsInAsync() || modifiers.Any(K::AsyncKeyword),
                                  /*isInQueryContext*/ std::nullopt, /*isInFieldKeywordContext*/ std::nullopt,
                                  /*forceConditionalAccessExpression*/ false);

    // parseAnonymousMethodExpressionWorker
    auto delegate = this->EatToken(K::DelegateKeyword);

    GreenNode *parameterList = nullptr;
    if (this->CurrentToken()->kind == K::OpenParenToken) {
        parameterList = this->ParseParenthesizedParameterList(/*forExtensionOrUnion*/ false);
    }

    // In mismatched braces cases (missing a }) it is possible for delegate declarations to be
    // parsed as delegate statement expressions.  When this situation occurs all subsequent
    // delegate declarations will also be parsed as delegate statement expressions.  In a file with
    // a sufficient number of delegates, common in generated code, it will put considerable
    // stack pressure on the parser.
    //
    // To help avoid this problem we don't recursively descend into a delegate expression unless
    // { } are actually present.  This keeps the stack pressure lower in bad code scenarios.
    if (this->CurrentToken()->kind != K::OpenBraceToken) {
        // There's a special error code for a missing token after an accessor keyword
        auto openBrace = this->EatToken(K::OpenBraceToken);
        return SyntaxFactory::AnonymousMethodExpression(
            modifiers, delegate, parameterList,
            SyntaxFactory::Block(/*attributeLists*/ nullptr, openBrace, /*statements*/ nullptr,
                                 SyntaxFactory::MissingToken(K::CloseBraceToken)),
            /*expressionBody*/ nullptr);
    }

    auto block = this->ParseBlock(/*attributes*/ nullptr);
    return SyntaxFactory::AnonymousMethodExpression(modifiers, delegate, parameterList, block,
                                                    /*expressionBody*/ nullptr);
}

SyntaxList LanguageParser::ParseAnonymousFunctionModifiers() {
    auto modifiers = _pool.Allocate();

    while (true) {
        if (this->CurrentToken()->kind == K::StaticKeyword) {
            modifiers.Add(this->EatToken(K::StaticKeyword));
            continue;
        }

        if (this->CurrentToken()->contextual_kind == K::AsyncKeyword && IsAnonymousFunctionAsyncModifier()) {
            modifiers.Add(this->EatContextualToken(K::AsyncKeyword));
            continue;
        }

        break;
    }

    return _pool.ToListAndFree(modifiers);
}

bool LanguageParser::IsAnonymousFunctionAsyncModifier() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::AsyncKeyword);

    auto kind = this->PeekToken(1)->kind;
    switch (kind) {
        case K::OpenParenToken:
        case K::IdentifierToken:
        case K::StaticKeyword:
        case K::RefKeyword:
        case K::DelegateKeyword:
            return true;
        default:
            return IsPredefinedType(kind);
    }
}

/// Parse expected lambda expression but assume `x ? () => y :` is a conditional
/// expression rather than a lambda expression with an explicit return type and
/// return null in that case only.
GreenNode *LanguageParser::TryParseLambdaExpression() {
    DisposableResetPoint resetPoint(this, /*resetOnDispose*/ false);
    auto result = ParseLambdaExpression();

    if (this->CurrentToken()->kind == K::ColonToken && result->kind == K::ParenthesizedLambdaExpression) {
        GreenNode *returnType = GetField(result, Field::ReturnType);
        if (returnType != nullptr && returnType->kind == K::NullableType) {
            resetPoint.Reset();
            return nullptr;
        }
    }

    return result;
}

GreenNode *LanguageParser::ParseLambdaExpression() {
    SyntaxList attributes = ParseAttributeDeclarations(/*inExpressionContext*/ true);

    auto modifiers = ParseAnonymousFunctionModifiers();

    ParserSyntaxContextResetter _(this, /*isInAsyncContext*/ this->IsInAsync() || modifiers.Any(K::AsyncKeyword),
                                  /*isInQueryContext*/ std::nullopt, /*isInFieldKeywordContext*/ std::nullopt,
                                  /*forceConditionalAccessExpression*/ false);

    // parseLambdaExpressionWorker
    GreenNode *returnType;
    {
        DisposableResetPoint resetPoint(this, /*resetOnDispose*/ false);
        returnType = ParseReturnType();
        if (CurrentToken()->kind != K::OpenParenToken) {
            resetPoint.Reset();
            returnType = nullptr;
        }
    }

    if (this->CurrentToken()->kind == K::OpenParenToken) {
        auto paramList = this->ParseLambdaParameterList();
        auto arrow = this->EatToken(K::EqualsGreaterThanToken);
        auto [block, expression] = ParseLambdaBody();

        return SyntaxFactory::ParenthesizedLambdaExpression(attributes, modifiers, returnType, paramList, arrow, block,
                                                            expression);
    } else {
        // Unparenthesized lambda case
        // x => ...
        auto identifier =
            (this->CurrentToken()->kind != K::IdentifierToken && this->PeekToken(1)->kind == K::EqualsGreaterThanToken)
                ? this->EatTokenAsKind(K::IdentifierToken)
                : this->ParseIdentifierToken();

        // Case x=>, x =>
        auto arrow = this->EatToken(K::EqualsGreaterThanToken);

        auto parameter = SyntaxFactory::Parameter(/*attributeLists*/ nullptr, /*modifiers*/ nullptr, /*type*/ nullptr,
                                                  identifier, /*default*/ nullptr);
        auto [block, expression] = ParseLambdaBody();
        return SyntaxFactory::SimpleLambdaExpression(attributes, modifiers, parameter, arrow, block, expression);
    }
}

std::pair<GreenNode *, GreenNode *> LanguageParser::ParseLambdaBody() {
    if (CurrentToken()->kind == K::OpenBraceToken) {
        return {ParseBlock(/*attributes*/ nullptr), nullptr};
    }
    return {nullptr, ParsePossibleRefExpression()};
}

GreenNode *LanguageParser::ParseLambdaParameterList() {
    auto skipBadLambdaParameterListTokens = [](LanguageParser *self, GreenNode *&openParen, SyntaxListBuilder list,
                                               SyntaxKind expectedKind, SyntaxKind closeKind) -> PostSkipAction {
        return self->SkipBadSeparatedListTokensWithExpectedKind(
            openParen, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != K::CommaToken && !p->IsPossibleLambdaParameter();
            },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    auto openParen = this->EatToken(K::OpenParenToken);
    auto saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfParameterList;

    auto nodes = ParseCommaSeparatedSyntaxList(
        openParen, K::CloseParenToken, [](LanguageParser *self) { return self->IsPossibleLambdaParameter(); },
        [](LanguageParser *self) { return self->ParseLambdaParameter(); }, skipBadLambdaParameterListTokens,
        /*allowTrailingSeparator*/ false,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    _termState = saveTerm;

    auto closeParen = this->EatToken(K::CloseParenToken);
    return SyntaxFactory::ParameterList(openParen, nodes, closeParen);
}

bool LanguageParser::IsPossibleLambdaParameter() {
    switch (this->CurrentToken()->kind) {
        case K::ParamsKeyword:
        case K::ReadOnlyKeyword:
        case K::RefKeyword:
        case K::OutKeyword:
        case K::InKeyword:
        case K::OpenParenToken:    // tuple
        case K::OpenBracketToken:  // attribute
            return true;

        case K::IdentifierToken:
            return this->IsTrueIdentifier();

        case K::DelegateKeyword:
            return this->IsFunctionPointerStart();

        default:
            return IsPredefinedType(this->CurrentToken()->kind);
    }
}

GreenNode *LanguageParser::ParseLambdaParameter() {
    SyntaxList attributes = ParseAttributeDeclarations(/*inExpressionContext*/ false);

    // Params are actually illegal in a lambda, but we'll allow it for error recovery purposes and
    // give the "params unexpected" error at semantic analysis time.
    SyntaxListBuilder modifiers = _pool.Allocate();
    if (IsParameterModifierIncludingScoped(this->CurrentToken())) {
        ParseParameterModifiers(modifiers, /*isFunctionPointerParameter*/ false, /*isLambdaParameter*/ true);
    }

    GreenNode *paramType = ShouldParseLambdaParameterType() ? ParseType(ParseTypeMode::Parameter) : nullptr;

    auto identifier = this->ParseIdentifierToken();

    // Parse default value if any
    auto equalsToken = TryEatToken(K::EqualsToken);

    GreenNode *modifierList = _pool.ToListAndFree(modifiers);
    GreenNode *default_ = equalsToken != nullptr
                              ? SyntaxFactory::EqualsValueClause(equalsToken, this->ParseExpressionCore())
                              : nullptr;
    return SyntaxFactory::Parameter(attributes, modifierList, paramType, identifier, default_);
}

bool LanguageParser::ShouldParseLambdaParameterType() {
    // If we have "int/string/etc." always parse out a type.
    if (IsPredefinedType(this->CurrentToken()->kind)) {
        return true;
    }

    // if we have a tuple type in a lambda.
    if (this->CurrentToken()->kind == K::OpenParenToken) {
        return true;
    }

    if (this->IsFunctionPointerStart()) {
        return true;
    }

    if (this->IsTrueIdentifier(this->CurrentToken())) {
        // Don't parse out a type if we see:
        //
        //      (a,
        //      (a)
        //      (a =>
        //      (a {
        //      (a =
        //
        // In all other cases, parse out a type.
        auto peek1 = this->PeekToken(1);
        if (peek1->kind != K::CommaToken && peek1->kind != K::CloseParenToken &&
            peek1->kind != K::EqualsGreaterThanToken && peek1->kind != K::OpenBraceToken &&
            peek1->kind != K::EqualsToken) {
            return true;
        }
    }

    return false;
}

bool LanguageParser::IsCurrentTokenQueryContextualKeyword() { return IsTokenQueryContextualKeyword(this->CurrentToken()); }

bool LanguageParser::IsTokenQueryContextualKeyword(GreenNode *token) {
    if (IsTokenStartOfNewQueryClause(token)) {
        return true;
    }

    switch (token->contextual_kind) {
        case K::OnKeyword:
        case K::EqualsKeyword:
        case K::AscendingKeyword:
        case K::DescendingKeyword:
        case K::ByKeyword:
            return true;
        default:
            break;
    }

    return false;
}

bool LanguageParser::IsTokenStartOfNewQueryClause(GreenNode *token) {
    switch (token->contextual_kind) {
        case K::FromKeyword:
        case K::JoinKeyword:
        case K::IntoKeyword:
        case K::WhereKeyword:
        case K::OrderByKeyword:
        case K::GroupKeyword:
        case K::SelectKeyword:
        case K::LetKeyword:
            return true;
        default:
            return false;
    }
}

bool LanguageParser::IsQueryExpression(bool mayBeVariableDeclaration, bool mayBeMemberDeclaration) {
    return this->CurrentToken()->contextual_kind == K::FromKeyword &&
           this->IsQueryExpressionAfterFrom(mayBeVariableDeclaration, mayBeMemberDeclaration);
}

// from_clause ::= from <type>? <identifier> in expression
bool LanguageParser::IsQueryExpressionAfterFrom(bool mayBeVariableDeclaration, bool mayBeMemberDeclaration) {
    // from x ...
    auto pk1 = this->PeekToken(1)->kind;
    if (IsPredefinedType(pk1)) {
        return true;
    }

    if (pk1 == K::IdentifierToken) {
        auto pk2 = this->PeekToken(2)->kind;
        if (pk2 == K::InKeyword) {
            return true;
        }

        if (mayBeVariableDeclaration) {
            if (pk2 == K::SemicolonToken ||  // from x;
                pk2 == K::CommaToken ||      // from x, y;
                pk2 == K::EqualsToken)       // from x = null;
            {
                return false;
            }
        }

        if (mayBeMemberDeclaration) {
            // from idf { ...   property decl
            // from idf(...     method decl
            if (pk2 == K::OpenParenToken || pk2 == K::OpenBraceToken) {
                return false;
            }

            // otherwise we need to scan a type
        } else {
            return true;
        }
    }

    // from T x ...
    DisposableResetPoint _(this, /*resetOnDispose*/ true);

    this->EatToken();
    return this->ScanType() != ScanTypeFlags::NotType &&
           (this->CurrentToken()->kind == K::IdentifierToken || this->CurrentToken()->kind == K::InKeyword);
}

GreenNode *LanguageParser::ParseQueryExpression(Precedence precedence) {
    ParserSyntaxContextResetter _(this, /*isInAsyncContext*/ std::nullopt, /*isInQueryContext*/ true);
    auto fromClause = this->ParseFromClause();

    GreenNode *from = precedence <= Precedence::Assignment
                          ? fromClause
                          : this->AddError(fromClause, ErrorCode::WRN_PrecedenceInversion,
                                           {SyntaxFacts::GetText(K::FromKeyword)});
    auto body = this->ParseQueryBody();
    return SyntaxFactory::QueryExpression(from, body);
}

GreenNode *LanguageParser::ParseQueryBody() {
    auto clauses = _pool.Allocate();

    // from, join, let, where and orderby
    while (true) {
        switch (this->CurrentToken()->contextual_kind) {
            case K::FromKeyword:
                clauses.Add(this->ParseFromClause());
                continue;
            case K::JoinKeyword:
                clauses.Add(this->ParseJoinClause());
                continue;
            case K::LetKeyword:
                clauses.Add(this->ParseLetClause());
                continue;
            case K::WhereKeyword:
                clauses.Add(this->ParseWhereClause());
                continue;
            case K::OrderByKeyword:
                clauses.Add(this->ParseOrderByClause());
                continue;
            default:
                break;
        }

        break;
    }

    // select or group clause
    GreenNode *selectOrGroupBy;
    switch (this->CurrentToken()->contextual_kind) {
        case K::SelectKeyword:
            selectOrGroupBy = this->ParseSelectClause();
            break;
        case K::GroupKeyword:
            selectOrGroupBy = this->ParseGroupClause();
            break;
        default: {
            auto selectKeyword = this->EatToken(K::SelectKeyword, ErrorCode::ERR_ExpectedSelectOrGroup);
            selectOrGroupBy = SyntaxFactory::SelectClause(selectKeyword, this->CreateMissingIdentifierName());
            break;
        }
    }

    GreenNode *clauseList = _pool.ToListAndFree(clauses);
    GreenNode *continuation =
        this->CurrentToken()->contextual_kind == K::IntoKeyword ? this->ParseQueryContinuation() : nullptr;
    return SyntaxFactory::QueryBody(clauseList, selectOrGroupBy, continuation);
}

GreenNode *LanguageParser::ParseFromClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::FromKeyword);
    auto from = this->EatContextualToken(K::FromKeyword);

    GreenNode *type = this->PeekToken(1)->kind != K::InKeyword ? this->ParseType() : nullptr;

    GreenNode *name;
    if (this->PeekToken(1)->contextual_kind == K::InKeyword &&
        (this->CurrentToken()->kind != K::IdentifierToken ||
         SyntaxFacts::IsQueryContextualKeyword(this->CurrentToken()->contextual_kind))) {
        // if this token is a something other than an identifier (someone accidentally used a contextual
        // keyword or a literal, for example), but we can see that the "in" is in the right place, then
        // just replace whatever is here with a missing identifier
        name = this->EatToken();
        name = WithAdditionalDiagnostics(name, this->GetExpectedTokenError(K::IdentifierToken, name->contextual_kind,
                                                                           name->GetLeadingTriviaWidth(), name->Width()));
        name = this->ConvertToMissingWithTrailingTrivia(name, K::IdentifierToken);
    } else {
        name = this->ParseIdentifierToken();
    }

    auto inKeyword = this->EatToken(K::InKeyword);
    auto expression = this->ParseExpressionCore();
    return SyntaxFactory::FromClause(from, type, name, inKeyword, expression);
}

GreenNode *LanguageParser::ParseJoinClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::JoinKeyword);
    auto joinKeyword = this->EatContextualToken(K::JoinKeyword);
    GreenNode *type = this->PeekToken(1)->kind != K::InKeyword ? this->ParseType() : nullptr;
    auto identifier = this->ParseIdentifierToken();
    auto inKeyword = this->EatToken(K::InKeyword);
    auto inExpression = this->ParseExpressionCore();
    auto onKeyword = this->EatContextualToken(K::OnKeyword, ErrorCode::ERR_ExpectedContextualKeywordOn);
    auto leftExpression = this->ParseExpressionCore();
    auto equalsKeyword = this->EatContextualToken(K::EqualsKeyword, ErrorCode::ERR_ExpectedContextualKeywordEquals);
    auto rightExpression = this->ParseExpressionCore();
    GreenNode *into = nullptr;
    if (this->CurrentToken()->contextual_kind == K::IntoKeyword) {
        auto intoKeyword = ConvertToKeyword(this->EatToken());
        into = SyntaxFactory::JoinIntoClause(intoKeyword, this->ParseIdentifierToken());
    }
    return SyntaxFactory::JoinClause(joinKeyword, type, identifier, inKeyword, inExpression, onKeyword, leftExpression,
                                     equalsKeyword, rightExpression, into);
}

GreenNode *LanguageParser::ParseLetClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::LetKeyword);
    auto letKeyword = this->EatContextualToken(K::LetKeyword);
    // If we see a keyword followed by '=', use EatTokenAsKind to produce a better error message and recover well.
    auto identifier =
        SyntaxFacts::IsReservedKeyword(this->CurrentToken()->kind) && this->PeekToken(1)->kind == K::EqualsToken
            ? this->EatTokenAsKind(K::IdentifierToken)
            : this->ParseIdentifierToken();
    auto equalsToken = this->EatToken(K::EqualsToken);
    auto expression = this->ParseExpressionCore();
    return SyntaxFactory::LetClause(letKeyword, identifier, equalsToken, expression);
}

GreenNode *LanguageParser::ParseWhereClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::WhereKeyword);
    auto whereKeyword = this->EatContextualToken(K::WhereKeyword);
    return SyntaxFactory::WhereClause(whereKeyword, this->ParseExpressionCore());
}

GreenNode *LanguageParser::ParseOrderByClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::OrderByKeyword);
    auto orderby = this->EatContextualToken(K::OrderByKeyword);

    auto list = _pool.AllocateSeparated();

    auto skipBadOrderingListTokens = [this](SyntaxListBuilder list_, SyntaxKind expected) -> PostSkipAction {
        GreenNode *tmp = nullptr;
        CS_ASSERT(list_.Count() > 0);
        return this->SkipBadSeparatedListTokensWithExpectedKind(
            tmp, list_, [](LanguageParser *p) { return p->CurrentToken()->kind != K::CommaToken; },
            [](LanguageParser *p, SyntaxKind) {
                return p->CurrentToken()->kind == K::CloseParenToken || p->CurrentToken()->kind == K::SemicolonToken ||
                       p->IsCurrentTokenQueryContextualKeyword();
            },
            expected);
    };

    // first argument
    list.Add(this->ParseOrdering());

    // additional arguments
    while (this->CurrentToken()->kind == K::CommaToken) {
        if (this->CurrentToken()->kind == K::CloseParenToken || this->CurrentToken()->kind == K::SemicolonToken) {
            break;
        } else if (this->CurrentToken()->kind == K::CommaToken) {
            list.AddSeparator(this->EatToken(K::CommaToken));
            list.Add(this->ParseOrdering());
            continue;
        } else if (skipBadOrderingListTokens(list, K::CommaToken) == PostSkipAction::Abort) {
            break;
        }
    }

    return SyntaxFactory::OrderByClause(orderby, _pool.ToListAndFree(list));
}

GreenNode *LanguageParser::ParseOrdering() {
    auto expression = this->ParseExpressionCore();
    GreenNode *direction = nullptr;
    SyntaxKind kind = K::AscendingOrdering;

    if (this->CurrentToken()->contextual_kind == K::AscendingKeyword ||
        this->CurrentToken()->contextual_kind == K::DescendingKeyword) {
        direction = ConvertToKeyword(this->EatToken());
        if (direction->kind == K::DescendingKeyword) {
            kind = K::DescendingOrdering;
        }
    }

    return SyntaxFactory::Ordering(kind, expression, direction);
}

GreenNode *LanguageParser::ParseSelectClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::SelectKeyword);
    auto selectKeyword = this->EatContextualToken(K::SelectKeyword);
    return SyntaxFactory::SelectClause(selectKeyword, this->ParseExpressionCore());
}

GreenNode *LanguageParser::ParseGroupClause() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::GroupKeyword);
    auto groupKeyword = this->EatContextualToken(K::GroupKeyword);
    auto groupExpression = this->ParseExpressionCore();
    auto byKeyword = this->EatContextualToken(K::ByKeyword, ErrorCode::ERR_ExpectedContextualKeywordBy);
    auto byExpression = this->ParseExpressionCore();
    return SyntaxFactory::GroupClause(groupKeyword, groupExpression, byKeyword, byExpression);
}

GreenNode *LanguageParser::ParseQueryContinuation() {
    CS_ASSERT(this->CurrentToken()->contextual_kind == K::IntoKeyword);
    auto intoKeyword = this->EatContextualToken(K::IntoKeyword);
    auto identifier = this->ParseIdentifierToken();
    auto body = this->ParseQueryBody();
    return SyntaxFactory::QueryContinuation(intoKeyword, identifier, body);
}

// IsIncremental / IsIncrementalAndFactoryContextMatches / MatchesFactoryContext: incremental parsing does not
// exist here (IsIncrementalAndFactoryContextMatches is always false, see csharp_parser_members_f.inc).
// The IsInAsync / ForceConditionalAccessExpression / IsInQuery / IsInFieldKeywordContext properties are in
// csharp_parser_internal.h.

/// Parses a comma separated list of nodes. (See the C# for the parameter documentation.)
SeparatedSyntaxList LanguageParser::ParseCommaSeparatedSyntaxList(GreenNode *&openToken, SyntaxKind closeTokenKind,
                                                                  const IsPossibleElementFn &isPossibleElement,
                                                                  const ParseElementFn &parseElement,
                                                                  const SkipBadTokensFn &skipBadTokens,
                                                                  bool allowTrailingSeparator, bool requireOneElement,
                                                                  bool allowSemicolonAsSeparator) {
    return ParseCommaSeparatedSyntaxList(openToken, closeTokenKind, isPossibleElement, parseElement,
                                         /*immediatelyAbort*/ nullptr, skipBadTokens, allowTrailingSeparator,
                                         requireOneElement, allowSemicolonAsSeparator);
}

SeparatedSyntaxList LanguageParser::ParseCommaSeparatedSyntaxList(GreenNode *&openToken, SyntaxKind closeTokenKind,
                                                                  const IsPossibleElementFn &isPossibleElement,
                                                                  const ParseElementFn &parseElement,
                                                                  const ImmediatelyAbortFn &immediatelyAbort,
                                                                  const SkipBadTokensFn &skipBadTokens,
                                                                  bool allowTrailingSeparator, bool requireOneElement,
                                                                  bool allowSemicolonAsSeparator) {
    // If we ever want this function to parse out separated lists with a different separator, we can
    // parameterize this method on this value.
    auto separatorTokenKind = K::CommaToken;
    auto nodes = _pool.AllocateSeparated();

    auto shouldParseSeparatorOrElement = [&]() -> bool {
        // if we're on a separator, we def should parse it out as such.
        if (this->CurrentToken()->kind == separatorTokenKind) return true;

        // We're not on a valid separator, but we want to be resilient for the user accidentally using the wrong
        // one in common cases.
        if (allowSemicolonAsSeparator && this->CurrentToken()->kind == K::SemicolonToken) return true;

        if (isPossibleElement(this)) return true;

        return false;
    };

tryAgain:
    if (requireOneElement || this->CurrentToken()->kind != closeTokenKind) {
        if (requireOneElement || shouldParseSeparatorOrElement()) {
            // first argument
            GreenNode *node = parseElement(this);
            nodes.Add(node);

            // now that we've gotten one element, we don't require any more.
            requireOneElement = false;

            // Ensure that if parsing separators/elements doesn't move us forward, that we always bail out from
            // parsing this list.
            int lastTokenPosition = -1;

            while (!(immediatelyAbort && immediatelyAbort(node)) && IsMakingProgress(lastTokenPosition)) {
                if (this->CurrentToken()->kind == closeTokenKind) break;

                if (shouldParseSeparatorOrElement()) {
                    // If we got a semicolon instead of comma, consume it with error and act as if it were a
                    // comma. Note: we do not change the kind of the token, so we can end up with a separated
                    // syntax list whose separators are a mix of commas and semicolons.  That is ok and is part
                    // of the expected contract of separated lists.  There will still be a diagnostic on the
                    // token letting the user know there is an error.  This allows us to recover gracefully,
                    // especially for higher levels like the IDE.
                    nodes.AddSeparator(this->CurrentToken()->kind == K::SemicolonToken
                                           ? this->EatTokenEvenWithIncorrectKind(separatorTokenKind)
                                           : this->EatToken(separatorTokenKind));

                    if (allowTrailingSeparator) {
                        // check for exit case after legal trailing comma
                        if (this->CurrentToken()->kind == closeTokenKind) {
                            break;
                        } else if (!isPossibleElement(this)) {
                            goto tryAgain;
                        }
                    }

                    node = parseElement(this);
                    nodes.Add(node);
                    continue;
                }

                // Something we didn't recognize, try to skip tokens, reporting that we expected a separator here.
                if (skipBadTokens(this, openToken, nodes, separatorTokenKind, closeTokenKind) == PostSkipAction::Abort)
                    break;
            }
        } else if (skipBadTokens(this, openToken, nodes, K::IdentifierToken, closeTokenKind) ==
                   PostSkipAction::Continue) {
            // Something we didn't recognize, try to skip tokens, reporting that we expected an identifier here.
            // While 'identifier' may not be completely accurate in terms of what the list needs, it's a
            // generally good 'catch all' indicating that some name/expr was needed, where something else
            // invalid was found.
            goto tryAgain;
        }
    }

    return _pool.ToListAndFree(nodes);
}

// GetDisposableResetPoint(resetOnDispose): `DisposableResetPoint x(this, resetOnDispose)` (csharp_parser_internal.h).

LanguageParser::ResetPoint LanguageParser::GetResetPoint() {
    ResetPoint point;
    point.BaseResetPoint = SyntaxParser::GetResetPoint();
    point.TerminatorState_ = _termState;
    point.IsInAsync = IsInAsync();
    point.IsInQuery = IsInQuery();
    point.IsInFieldKeywordContext = IsInFieldKeywordContext();
    return point;
}

void LanguageParser::Reset(ResetPoint &state) {
    _termState = state.TerminatorState_;
    SetIsInAsync(state.IsInAsync);
    SetIsInQuery(state.IsInQuery);
    SetIsInFieldKeywordContext(state.IsInFieldKeywordContext);
    SyntaxParser::Reset(state.BaseResetPoint);
}

void LanguageParser::Release(ResetPoint &state) { SyntaxParser::Release(state.BaseResetPoint); }

// DisposableResetPoint and ResetPoint: csharp_parser_internal.h.

GreenNode *LanguageParser::ConsumeUnexpectedTokens(GreenNode *node) {
    if (this->CurrentToken()->kind == K::EndOfFileToken) return node;
    SyntaxListBuilder b = _pool.Allocate();
    while (this->CurrentToken()->kind != K::EndOfFileToken) {
        b.Add(this->EatToken());
    }

    SyntaxList trailingTrash = b.ToList();
    _pool.Free(b);

    node = this->AddError(node, ErrorCode::ERR_UnexpectedToken, {trailingTrash[0]->text});
    node = this->AddTrailingSkippedSyntax(node, trailingTrash.Node());
    return node;
}

bool LanguageParser::ContainsErrorDiagnostic(GreenNode *node) {
    // ContainsDiagnostics returns true if this node (or any descendants) contain any sort of error.  However,
    // GetDiagnostics() only returns diagnostics at that node itself.  So we have to explicitly walk down the
    // tree to find out if the diagnostics are error or not.

    // Quick check to avoid any unnecessary work.
    if (node->ContainsDiagnostics()) {
        std::vector<GreenNode *> stack;
        stack.push_back(node);

        while (!stack.empty()) {
            auto current = stack.back();
            stack.pop_back();
            if (!current->ContainsDiagnostics()) continue;

            for (const SyntaxDiagnosticInfo *d = DiagsBegin(current); d != DiagsEnd(current); ++d) {
                if (ErrorSeverity(d->code) == DiagnosticSeverity::Error) return true;
            }

            // ChildNodesAndTokens: the slots, with lists flattened into their items (trivia not included).
            if (current->IsToken()) continue;
            for (int i = 0; i < current->SlotCount(); i++) {
                GreenNode *child = current->GetSlot(i);
                if (child == nullptr) continue;
                if (child->IsList()) {
                    for (int j = 0; j < child->SlotCount(); j++) {
                        if (child->GetSlot(j) != nullptr) stack.push_back(child->GetSlot(j));
                    }
                } else {
                    stack.push_back(child);
                }
            }
        }
    }

    return false;
}

}  // namespace csharp
