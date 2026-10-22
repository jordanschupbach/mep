// LanguageParser, unit F (part 2): a translation of Roslyn's
// src/Compilers/CSharp/Portable/Parser/LanguageParser_Patterns.cs and
// LanguageParser_InterpolatedString.cs (MIT, (c) .NET Foundation and
// Contributors). See csharp_parser_internal.h for the translation conventions.
//
// C# evaluates call arguments left to right and C++ does not, so every
// factory call whose arguments consume tokens takes them from locals computed
// in the C# order.
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "csharp_parser_internal.h"

namespace csharp {

using K = SyntaxKind;

namespace {

/** @brief text[range] (System.Range over a string). */
Text Slice(Text text, TextRange range) {
    return text.substr(static_cast<size_t>(range.start), static_cast<size_t>(range.Length()));
}

/** @brief text[start..end]. */
Text Slice(Text text, int start, int end) {
    return text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
}

}  // namespace

// ==== LanguageParser_Patterns.cs ===================================================================

/// Parses the type, or pattern, right-hand operand of an is expression.
/// Priority is the TypeSyntax. It may return a TypeSyntax which turns out in binding to
/// be a constant pattern such as enum 'Days.Sunday'. We handle such cases in the binder of the is operator.
/// Note that the syntax `_` will be parsed as a type.
GreenNode *LanguageParser::ParseTypeOrPatternForIsOperator() {
    auto pattern = ParsePattern(GetPrecedence(K::IsPatternExpression), /*afterIs*/ true);
    switch (pattern->kind) {
        case K::ConstantPattern: {
            GreenNode *type = nullptr;
            if (ConvertExpressionToType(GetField(pattern, Field::Expression), type)) return type;
            return pattern;
        }
        case K::TypePattern:
            return GetField(pattern, Field::Type);
        case K::DiscardPattern:
            return SyntaxFactory::IdentifierName(ConvertToIdentifier(GetField(pattern, Field::UnderscoreToken)));
        default:
            return pattern;
    }
}

bool LanguageParser::ConvertExpressionToType(GreenNode *expression, GreenNode *&type) {
    switch (expression->kind) {
        // SimpleNameSyntax
        case K::IdentifierName:
        case K::GenericName:
            type = expression;
            return true;
        case K::SimpleMemberAccessExpression:
        case K::PointerMemberAccessExpression: {
            // MemberAccessExpressionSyntax { Expression: var expr, OperatorToken: { Kind: DotToken } dotToken,
            //                                Name: var simpleName } when ConvertExpressionToType(expr, out var leftType)
            GreenNode *dotToken = GetField(expression, Field::OperatorToken);
            GreenNode *leftType = nullptr;
            if (dotToken->kind == K::DotToken && ConvertExpressionToType(GetField(expression, Field::Expression), leftType)) {
                type = SyntaxFactory::QualifiedName(leftType, dotToken, GetField(expression, Field::Name));
                return true;
            }
            type = nullptr;
            return false;
        }
        case K::AliasQualifiedName:
            type = expression;
            return true;
        default:
            type = nullptr;
            return false;
    }
}

GreenNode *LanguageParser::ParsePattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern) {
    return ParseDisjunctivePattern(precedence, afterIs, inSwitchArmPattern);
}

GreenNode *LanguageParser::ParseDisjunctivePattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern) {
    GreenNode *result = ParseConjunctivePattern(precedence, afterIs, inSwitchArmPattern);
    while (this->CurrentToken()->contextual_kind == K::OrKeyword) {
        auto orKeyword = ConvertToKeyword(this->EatToken());
        auto right = ParseConjunctivePattern(precedence, afterIs, inSwitchArmPattern);
        result = SyntaxFactory::BinaryPattern(K::OrPattern, result, orKeyword, right);
    }

    return result;
}

/// Given tk, the type of the current token, does this look like the type of a pattern?
bool LanguageParser::LooksLikeTypeOfPattern() {
    auto tk = CurrentToken()->kind;
    if (SyntaxFacts::IsPredefinedType(tk)) {
        return true;
    }

    if (tk == K::IdentifierToken && this->CurrentToken()->contextual_kind != K::UnderscoreToken &&
        (this->CurrentToken()->contextual_kind != K::NameOfKeyword || this->PeekToken(1)->kind != K::OpenParenToken)) {
        return true;
    }

    if (LooksLikeTupleArrayType()) {
        return true;
    }

    // We'll parse the function pointer, but issue an error in semantic analysis
    if (IsFunctionPointerStart()) {
        return true;
    }

    return false;
}

GreenNode *LanguageParser::ParseConjunctivePattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern) {
    GreenNode *result = ParseNegatedPattern(precedence, afterIs, inSwitchArmPattern);
    while (this->CurrentToken()->contextual_kind == K::AndKeyword) {
        auto andKeyword = ConvertToKeyword(this->EatToken());
        auto right = ParseNegatedPattern(precedence, afterIs, inSwitchArmPattern);
        result = SyntaxFactory::BinaryPattern(K::AndPattern, result, andKeyword, right);
    }

    return result;
}

bool LanguageParser::ScanDesignation(bool permitTuple) {
    switch (this->CurrentToken()->kind) {
        default:
            return false;
        case K::IdentifierToken: {
            bool result = this->IsTrueIdentifier();
            this->EatToken();
            return result;
        }
        case K::OpenParenToken: {
            if (!permitTuple) {
                return false;
            }

            bool sawComma = false;
            while (true) {
                this->EatToken();  // consume the `(` or `,`
                if (!ScanDesignation(/*permitTuple*/ true)) {
                    return false;
                }
                switch (this->CurrentToken()->kind) {
                    case K::CloseParenToken:
                        this->EatToken();
                        return sawComma;
                    case K::CommaToken:
                        sawComma = true;
                        continue;
                    default:
                        return false;
                }
            }
        }
    }
}

GreenNode *LanguageParser::ParseNegatedPattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern) {
    if (this->CurrentToken()->contextual_kind == K::NotKeyword) {
        auto notKeyword = ConvertToKeyword(this->EatToken());
        auto pattern = ParseNegatedPattern(precedence, afterIs, inSwitchArmPattern);
        return SyntaxFactory::UnaryPattern(notKeyword, pattern);
    } else if (this->CurrentToken()->kind == K::EqualsEqualsToken) {
        auto skippedSyntax = AddError(this->EatToken(), ErrorCode::ERR_EqualityOperatorInPatternNotSupported);
        auto node = ParseNegatedPattern(precedence, afterIs, inSwitchArmPattern);
        return AddLeadingSkippedSyntax(node, skippedSyntax);
    } else if (this->CurrentToken()->kind == K::ExclamationEqualsToken) {
        // Handle != in patterns - suggest using 'not' instead
        auto operatorToken = this->AddTrailingSkippedSyntax(
            SyntaxFactory::MissingToken(K::NotKeyword),
            this->AddError(this->EatToken(), ErrorCode::ERR_InequalityOperatorInPatternNotSupported));
        auto pattern = ParseNegatedPattern(precedence, afterIs, inSwitchArmPattern);
        return SyntaxFactory::UnaryPattern(operatorToken, pattern);
    } else {
        return ParsePrimaryPattern(precedence, afterIs, inSwitchArmPattern);
    }
}

GreenNode *LanguageParser::ParsePrimaryPattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern) {
    // handle common error recovery situations during typing
    auto tk = this->CurrentToken()->kind;
    switch (tk) {
        case K::CommaToken:
        case K::SemicolonToken:
        case K::CloseBraceToken:
        case K::CloseParenToken:
        case K::CloseBracketToken:
        case K::EqualsGreaterThanToken:
            return SyntaxFactory::ConstantPattern(this->ParseIdentifierName(ErrorCode::ERR_MissingPattern));
        default:
            break;
    }

    if (CurrentToken()->contextual_kind == K::UnderscoreToken) {
        return SyntaxFactory::DiscardPattern(this->EatContextualToken(K::UnderscoreToken));
    }

    switch (CurrentToken()->kind) {
        case K::OpenBracketToken:
            return this->ParseListPattern(inSwitchArmPattern);
        case K::DotToken:
            if (IsAtDotDotToken()) {
                auto dotDot = EatDotDotToken();
                GreenNode *pattern = IsPossibleSubpatternElement()
                                         ? ParsePattern(precedence, /*afterIs*/ false, inSwitchArmPattern)
                                         : nullptr;
                return SyntaxFactory::SlicePattern(dotDot, pattern);
            }
            break;
        case K::LessThanToken:
        case K::LessThanEqualsToken:
        case K::GreaterThanToken:
        case K::GreaterThanEqualsToken:
        case K::EqualsEqualsToken:
        case K::ExclamationEqualsToken: {
            // this is a relational pattern.
            CS_ASSERT(precedence < Precedence::Shift);
            auto operatorToken = this->EatToken();
            auto expression = this->ParseSubExpression(Precedence::Relational);
            return SyntaxFactory::RelationalPattern(operatorToken, expression);
        }
        default:
            break;
    }

    DisposableResetPoint resetPoint(this, /*resetOnDispose*/ false);

    GreenNode *type = nullptr;
    if (LooksLikeTypeOfPattern()) {
        type = this->ParseType(afterIs ? ParseTypeMode::AfterIs : ParseTypeMode::DefinitePattern);
        if (type->IsMissing() || !CanTokenFollowTypeInPattern(precedence)) {
            // either it is not shaped like a type, or it is a constant expression.
            resetPoint.Reset();
            type = nullptr;
        }
    }

    auto pattern = ParsePatternContinued(type, precedence, inSwitchArmPattern);
    if (pattern != nullptr) return pattern;

    resetPoint.Reset();
    auto value = this->ParseSubExpression(precedence);
    return SyntaxFactory::ConstantPattern(value);
}

/// Is the current token something that could follow a type in a pattern?
bool LanguageParser::CanTokenFollowTypeInPattern(Precedence precedence) {
    auto kind = this->CurrentToken()->kind;
    switch (kind) {
        case K::OpenParenToken:
        case K::OpenBraceToken:
        case K::IdentifierToken:
        case K::CloseBraceToken:  // for efficiency, test some tokens that can follow a type pattern
        case K::CloseBracketToken:
        case K::CloseParenToken:
        case K::CommaToken:
        case K::SemicolonToken:
            return true;
        case K::DotToken:
            // int.MaxValue is an expression, not a type.
            return false;
        case K::MinusGreaterThanToken:
        case K::ExclamationToken:
            // parse as an expression for error recovery
            return false;
        default:
            // If we find what looks like a continuation of an expression, it is not a type.
            return !SyntaxFacts::IsBinaryExpressionOperatorToken(kind) ||
                   GetPrecedence(SyntaxFacts::GetBinaryExpression(kind)) <= precedence;
    }
}

GreenNode *LanguageParser::ParsePatternContinued(GreenNode *type, Precedence precedence, bool inSwitchArmPattern) {
    auto parsePropertyPatternClause = [&](GreenNode *&propertyPatternClauseResult) -> bool {
        // Check for the `id {` and report that the designator has to come after the property pattern.
        GreenNode *misplacedIdentifier = this->IsTrueIdentifier() && this->IsValidPatternDesignation(inSwitchArmPattern) &&
                                                 this->PeekToken(1)->kind == K::OpenBraceToken
                                             ? AddError(this->EatToken(), ErrorCode::ERR_DesignatorBeforePropertyPattern)
                                             : nullptr;

        if (this->CurrentToken()->kind == K::OpenBraceToken) {
            propertyPatternClauseResult = AddLeadingSkippedSyntax(ParsePropertyPatternClause(), misplacedIdentifier);
            return true;
        }

        propertyPatternClauseResult = nullptr;
        return false;
    };

    auto looksLikeCast = [&]() -> bool {
        DisposableResetPoint _(this, /*resetOnDispose*/ true);
        return this->ScanCast(/*forPattern*/ true, inSwitchArmPattern);
    };

    if (type != nullptr && type->kind == K::IdentifierName) {
        auto typeIdentifierToken = GetField(type, Field::Identifier);
        if (typeIdentifierToken->contextual_kind == K::VarKeyword &&
            (this->CurrentToken()->kind == K::OpenParenToken || this->IsValidPatternDesignation(inSwitchArmPattern))) {
            // we have a "var" pattern; "var" is not permitted to be a stand-in for a type (or a constant) in a pattern.
            auto varToken = ConvertToKeyword(typeIdentifierToken);
            auto varDesignation = ParseDesignation(/*forPattern*/ true);
            return SyntaxFactory::VarPattern(varToken, varDesignation);
        }
    }

    if (this->CurrentToken()->kind == K::OpenParenToken && (type != nullptr || !looksLikeCast())) {
        // It is possible this is a parenthesized (constant) expression.
        // We normalize later.
        auto openParenToken = this->EatToken(K::OpenParenToken);
        auto subPatterns = this->ParseCommaSeparatedSyntaxList(
            openParenToken, K::CloseParenToken, [](LanguageParser *self) { return self->IsPossibleSubpatternElement(); },
            [](LanguageParser *self) { return self->ParseSubpatternElement(); }, SkipBadPatternListTokens,
            /*allowTrailingSeparator*/ false,
            /*requireOneElement*/ false,
            /*allowSemicolonAsSeparator*/ false);
        auto closeParenToken = this->EatToken(K::CloseParenToken);

        GreenNode *propertyPatternClause0 = nullptr;
        parsePropertyPatternClause(propertyPatternClause0);
        auto designation0 = TryParseSimpleDesignation(inSwitchArmPattern);

        if (type == nullptr && propertyPatternClause0 == nullptr && designation0 == nullptr && subPatterns.Count() == 1 &&
            subPatterns.SeparatorCount() == 0) {
            auto firstSubPattern = subPatterns[0];
            CS_ASSERT(firstSubPattern != nullptr);

            if (GetField(firstSubPattern, Field::ExpressionColon) == nullptr) {
                auto subpattern = GetField(firstSubPattern, Field::Pattern);
                switch (subpattern->kind) {
                    case K::ConstantPattern: {
                        // There is an ambiguity between a positional pattern `(` pattern `)`
                        // and a constant expression pattern that happens to be parenthesized.
                        // Per 2017-11-20 LDM we treat such syntax as a parenthesized expression always.
                        GreenNode *expression = SyntaxFactory::ParenthesizedExpression(
                            openParenToken, GetField(subpattern, Field::Expression), closeParenToken);
                        expression = ParseExpressionContinued(expression, precedence);
                        return SyntaxFactory::ConstantPattern(expression);
                    }
                    default:
                        return SyntaxFactory::ParenthesizedPattern(openParenToken, subpattern, closeParenToken);
                }
            }
        }

        auto positionalPatternClause = SyntaxFactory::PositionalPatternClause(openParenToken, subPatterns, closeParenToken);
        auto result = SyntaxFactory::RecursivePattern(type, positionalPatternClause, propertyPatternClause0, designation0);
        return result;
    }

    GreenNode *propertyPatternClause = nullptr;
    if (parsePropertyPatternClause(propertyPatternClause)) {
        auto designation = TryParseSimpleDesignation(inSwitchArmPattern);
        return SyntaxFactory::RecursivePattern(type, /*positionalPatternClause*/ nullptr, propertyPatternClause,
                                               designation);
    }

    if (type != nullptr) {
        auto designation = TryParseSimpleDesignation(inSwitchArmPattern);
        if (designation != nullptr) return SyntaxFactory::DeclarationPattern(type, designation);

        // We normally prefer an expression rather than a type in a pattern.
        GreenNode *expression = nullptr;
        return ConvertTypeToExpression(type, expression)
                   ? SyntaxFactory::ConstantPattern(ParseExpressionContinued(expression, precedence))
                   : SyntaxFactory::TypePattern(type);
    }

    // let the caller fall back to parsing an expression
    return nullptr;
}

GreenNode *LanguageParser::TryParseSimpleDesignation(bool whenIsKeyword) {
    return this->IsTrueIdentifier() && this->IsValidPatternDesignation(whenIsKeyword) ? ParseSimpleDesignation()
                                                                                       : nullptr;
}

bool LanguageParser::IsValidPatternDesignation(bool inSwitchArmPattern) {
    if (CurrentToken()->kind == K::IdentifierToken) {
        switch (CurrentToken()->contextual_kind) {
            case K::WhenKeyword:
                // When directly in a switch arm, we *always* treat 'when' as a keyword starting the 'when
                // clause'.  In other patterns, we allow 'when' to be a normal designator.
                return !inSwitchArmPattern;
            case K::AndKeyword:
            case K::OrKeyword: {
                auto tk = PeekToken(1)->kind;
                switch (tk) {
                    case K::CloseBraceToken:
                    case K::CloseBracketToken:
                    case K::CloseParenToken:
                    case K::CommaToken:
                    case K::SemicolonToken:
                    case K::QuestionToken:
                    case K::ColonToken:
                        return true;
                    case K::LessThanEqualsToken:
                    case K::LessThanToken:
                    case K::GreaterThanEqualsToken:
                    case K::GreaterThanToken:
                    case K::IdentifierToken:
                    case K::OpenBraceToken:
                    case K::OpenParenToken:
                    case K::OpenBracketToken:
                        // these all can start a pattern
                        return false;
                    default: {
                        if (SyntaxFacts::IsBinaryExpression(tk))
                            return true;  // `e is int and && true` is valid C# 7.0 code with `and` being a designator

                        // If the following token could start an expression, it may be a constant pattern after a combinator.
                        DisposableResetPoint _(this, /*resetOnDispose*/ true);
                        this->EatToken();
                        return !CanStartExpression();
                    }
                }
            }
            case K::UnderscoreToken:  // discard is a valid pattern designation
            default:
                return true;
        }
    }

    return false;
}

GreenNode *LanguageParser::ParseExpressionOrPatternForSwitchStatement() {
    auto savedState = _termState;
    _termState |= TerminatorState::IsExpressionOrPatternInCaseLabelOfSwitchStatement;
    auto pattern = ParsePattern(Precedence::Conditional, /*afterIs*/ false, /*inSwitchArmPattern*/ true);
    _termState = savedState;
    return ConvertPatternToExpressionIfPossible(pattern);
}

GreenNode *LanguageParser::ConvertPatternToExpressionIfPossible(GreenNode *pattern, bool permitTypeArguments) {
    switch (pattern->kind) {
        case K::ConstantPattern:
            return GetField(pattern, Field::Expression);
        case K::TypePattern: {
            GreenNode *expr = nullptr;
            if (ConvertTypeToExpression(GetField(pattern, Field::Type), expr, permitTypeArguments)) return expr;
            return pattern;
        }
        case K::DiscardPattern:
            return SyntaxFactory::IdentifierName(ConvertToIdentifier(GetField(pattern, Field::UnderscoreToken)));
        default:
            return pattern;
    }
}

bool LanguageParser::ConvertTypeToExpression(GreenNode *type, GreenNode *&expr, bool permitTypeArguments) {
    switch (type->kind) {
        case K::GenericName:
            expr = type;
            return permitTypeArguments;
        case K::IdentifierName:  // SimpleNameSyntax
            expr = type;
            return true;
        case K::QualifiedName: {
            GreenNode *left = GetField(type, Field::Left);
            GreenNode *dotToken = GetField(type, Field::DotToken);
            GreenNode *right = GetField(type, Field::Right);
            if (permitTypeArguments || right->kind != K::GenericName) {
                GreenNode *leftExpr = nullptr;
                auto newLeft = ConvertTypeToExpression(left, leftExpr, /*permitTypeArguments*/ true) ? leftExpr : left;
                expr = SyntaxFactory::MemberAccessExpression(K::SimpleMemberAccessExpression, newLeft, dotToken, right);
                return true;
            }
            expr = nullptr;
            return false;
        }
        default:
            expr = nullptr;
            return false;
    }
}

bool LanguageParser::LooksLikeTupleArrayType() {
    if (this->CurrentToken()->kind != K::OpenParenToken) return false;

    DisposableResetPoint _(this, /*resetOnDispose*/ true);
    return ScanType(/*forPattern*/ true) != ScanTypeFlags::NotType;
}

GreenNode *LanguageParser::ParsePropertyPatternClause() {
    auto openBraceToken = this->EatToken(K::OpenBraceToken);
    auto subPatterns = this->ParseCommaSeparatedSyntaxList(
        openBraceToken, K::CloseBraceToken, [](LanguageParser *self) { return self->IsPossibleSubpatternElement(); },
        [](LanguageParser *self) { return self->ParseSubpatternElement(); }, SkipBadPatternListTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBraceToken = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::PropertyPatternClause(openBraceToken, subPatterns, closeBraceToken);
}

GreenNode *LanguageParser::ParseSubpatternElement() {
    GreenNode *exprColon = nullptr;

    GreenNode *pattern = ParsePattern(Precedence::Conditional);
    // If there is a colon but it's not preceded by a valid expression, leave it out to parse it as a missing comma, preserving C# 9.0 behavior.
    if (this->CurrentToken()->kind == K::ColonToken) {
        GreenNode *expr = ConvertPatternToExpressionIfPossible(pattern, /*permitTypeArguments*/ true);
        if (KindDerivesFrom(expr->kind, "ExpressionSyntax")) {
            auto colon = EatToken();
            exprColon = expr->kind == K::IdentifierName ? SyntaxFactory::NameColon(expr, colon)
                                                        : SyntaxFactory::ExpressionColon(expr, colon);

            pattern = ParsePattern(Precedence::Conditional);
        }
    }

    return SyntaxFactory::Subpattern(exprColon, pattern);
}

/// Check the next token to see if it is valid as the first token of a subpattern element.
/// Used to assist in error recovery for subpattern lists (e.g. determining which tokens to skip)
/// to ensure we make forward progress during recovery.
bool LanguageParser::IsPossibleSubpatternElement() {
    if (this->CanStartExpression()) return true;
    switch (this->CurrentToken()->kind) {
        case K::OpenBraceToken:
        case K::OpenBracketToken:
        case K::LessThanToken:
        case K::LessThanEqualsToken:
        case K::GreaterThanToken:
        case K::GreaterThanEqualsToken:
            return true;
        default:
            return false;
    }
}

LanguageParser::PostSkipAction LanguageParser::SkipBadPatternListTokens(LanguageParser *self, GreenNode *&open,
                                                                        SyntaxListBuilder list, SyntaxKind expectedKind,
                                                                        SyntaxKind closeKind) {
    auto ck = self->CurrentToken()->kind;
    if (ck == K::CloseParenToken || ck == K::CloseBraceToken || ck == K::CloseBracketToken || ck == K::SemicolonToken)
        return PostSkipAction::Abort;

    // `:` is usually treated as incorrect separation token. This helps for error recovery in basic typing scenarios like `{ Prop:$$ Prop1: { ... } }`.
    // However, such behavior isn't much desirable when parsing pattern of a case label in a switch statement. For instance, consider the following example: `case { Prop: { }: case ...`.
    // Normally we would skip second `:` and `case` keyword after it as bad tokens and continue parsing pattern, which produces a lot of noise errors.
    // In order to avoid that and produce single error of missing `}` we exit on unexpected `:` in such cases.
    if (Any(self->_termState & TerminatorState::IsExpressionOrPatternInCaseLabelOfSwitchStatement) &&
        self->CurrentToken()->kind == K::ColonToken)
        return PostSkipAction::Abort;

    // This is pretty much the same as above, but for switch expressions and `=>` and `:` tokens.
    // The reason why we cannot use single flag for both cases is because we want `=>` to be the "exit" token only for switch expressions.
    // Consider the following example: `case (() => 0):`. Normally `=>` is treated as bad separator, so we parse this basically the same as `case ((), 1):`, which is syntactically valid.
    // However, if we treated `=>` as "exit" token, parsing wouldn't consume full case label properly and would produce a lot of noise errors.
    // We can afford `:` to be the exit token for switch expressions because error recovery is already good enough and treats `:` as bad `=>`,
    // meaning that switch expression arm `{ : 1` can be recovered to `{ } => 1` where the closing `}` is missing and instead of `=>` we have `:`.
    if (Any(self->_termState & TerminatorState::IsPatternInSwitchExpressionArm) &&
        (self->CurrentToken()->kind == K::EqualsGreaterThanToken || self->CurrentToken()->kind == K::ColonToken))
        return PostSkipAction::Abort;

    return self->SkipBadSeparatedListTokensWithExpectedKind(
        open, list,
        [](LanguageParser *p) {
            return p->CurrentToken()->kind != K::CommaToken && !p->IsPossibleSubpatternElement();
        },
        [](LanguageParser *p, SyntaxKind closeKind_) {
            return p->CurrentToken()->kind == closeKind_ || p->CurrentToken()->kind == K::SemicolonToken;
        },
        expectedKind, closeKind);
}

GreenNode *LanguageParser::ParseSwitchExpression(GreenNode *governingExpression, GreenNode *switchKeyword) {
    auto parseSwitchExpressionArms = [&]() -> GreenNode * {
        auto arms = _pool.AllocateSeparated();

        while (this->CurrentToken()->kind != K::CloseBraceToken) {
            // Help out in the case where a user is converting a switch statement to a switch expression. Note:
            // `default(...)` and `default` will also be consumed as a legal syntactic patterns (though the
            // latter will fail during binding).  So if the user has `default:` we will recover fine as we
            // handle the errant colon below.
            GreenNode *errantCase = this->CurrentToken()->kind == K::CaseKeyword
                                        ? AddError(this->EatToken(), ErrorCode::ERR_BadCaseInSwitchArm)
                                        : nullptr;

            auto savedState = _termState;
            _termState |= TerminatorState::IsPatternInSwitchExpressionArm;
            auto pattern = ParsePattern(Precedence::Coalescing, /*afterIs*/ false, /*inSwitchArmPattern*/ true);
            _termState = savedState;

            // We use a precedence that excludes lambdas, assignments, and a conditional which could have a
            // lambda on the right, because we need the parser to leave the EqualsGreaterThanToken to be
            // consumed by the switch arm. The strange side-effect of that is that the conditional expression is
            // not permitted as a constant expression here; it would have to be parenthesized.

            auto whenClause = ParseWhenClause(Precedence::Coalescing);
            // Help out in the case where a user is converting a switch statement to a switch expression.
            // Consume the `:` as a `=>` and report an error.
            auto arrow = this->CurrentToken()->kind == K::ColonToken ? this->EatTokenAsKind(K::EqualsGreaterThanToken)
                                                                     : this->EatToken(K::EqualsGreaterThanToken);
            auto expression = ParseExpressionCore();
            auto switchExpressionCase = SyntaxFactory::SwitchExpressionArm(pattern, whenClause, arrow, expression);

            // If we're not making progress, abort
            if (errantCase == nullptr && switchExpressionCase->full_width == 0 &&
                this->CurrentToken()->kind != K::CommaToken)
                break;

            if (errantCase != nullptr) switchExpressionCase = AddLeadingSkippedSyntax(switchExpressionCase, errantCase);

            arms.Add(switchExpressionCase);
            if (this->CurrentToken()->kind != K::CloseBraceToken) {
                auto commaToken = this->CurrentToken()->kind == K::SemicolonToken ? this->EatTokenAsKind(K::CommaToken)
                                                                                  : this->EatToken(K::CommaToken);
                arms.AddSeparator(commaToken);
            }
        }

        return _pool.ToListAndFree(arms);
    };

    // For better error recovery when an expression is typed on a line before a switch statement,
    // the caller checks if the switch keyword is followed by an open curly brace. Only if it is
    // would we attempt to parse it as a switch expression here.
    auto openBrace = this->EatToken(K::OpenBraceToken);
    auto arms = parseSwitchExpressionArms();
    auto closeBrace = this->EatToken(K::CloseBraceToken);
    return SyntaxFactory::SwitchExpression(governingExpression, switchKeyword, openBrace, arms, closeBrace);
}

GreenNode *LanguageParser::ParseListPattern(bool inSwitchArmPattern) {
    auto openBracket = this->EatToken(K::OpenBracketToken);
    auto list = this->ParseCommaSeparatedSyntaxList(
        openBracket, K::CloseBracketToken, [](LanguageParser *self) { return self->IsPossibleSubpatternElement(); },
        [](LanguageParser *self) { return self->ParsePattern(Precedence::Conditional); }, SkipBadPatternListTokens,
        /*allowTrailingSeparator*/ true,
        /*requireOneElement*/ false,
        /*allowSemicolonAsSeparator*/ false);

    auto closeBracket = this->EatToken(K::CloseBracketToken);
    auto designation = TryParseSimpleDesignation(inSwitchArmPattern);
    return SyntaxFactory::ListPattern(openBracket, list, closeBracket, designation);
}

// ==== LanguageParser_InterpolatedString.cs =========================================================

GreenNode *LanguageParser::ParseRawStringToken() {
    auto originalToken = this->EatToken();

    auto expressionKind = SyntaxFacts::GetLiteralExpression(originalToken->kind);
    CS_ASSERT(expressionKind != K::None);

    // We want to share as much code as possible with raw-interpolated-strings.  Especially the code for dealing
    // with indentation removal and determining the 'value' of the string.  As such, we will reinterpret this
    // raw string as an interpolated string with no $'s and no holes, and then extract out the content token
    // from that.

    auto interpolatedString = ParseInterpolatedOrRawStringToken(originalToken, /*isInterpolatedString*/ false);

    // Because there are no actual interpolations, we expect to only see a single text content node containing
    // the interpreted value of the raw string.
    auto interpolatedText = SyntaxList(GetField(interpolatedString, Field::Contents))[0];

    // getDiagnostics
    std::vector<SyntaxDiagnosticInfo> diagnostics;
    {
        // And any diagnostics from the interpolated string as a whole.
        diagnostics.insert(diagnostics.end(), DiagsBegin(interpolatedString), DiagsEnd(interpolatedString));

        // We may have diagnostics on the InterpolatedStringText node itself, but not on the text token inside it
        // (since we create it, and immediately add it to the InterpolatedStringText node).  If so, move those over.
        // However, move them as they are relative to the text token, and now need to be relative to the start of
        // the token as a whole.
        auto textTokenDiagnostics =
            MoveDiagnostics(interpolatedText->diags, interpolatedText->diag_count,
                            GetField(interpolatedString, Field::StringStartToken)->Width());
        diagnostics.insert(diagnostics.end(), textTokenDiagnostics.begin(), textTokenDiagnostics.end());
    }

    // getTokenValue
    Text tokenValue;
    if (diagnostics.empty()) {
        tokenValue = GetField(interpolatedText, Field::TextToken)->value_text;
    } else {
        // Preserve what the lexer used to do here.  In the presence of any diagnostics, the text of the raw
        // string minus the starting quotes is used as the value.
        size_t startIndex = 0;
        Text originalText = originalToken->text;
        while (startIndex < originalText.size() && originalText[startIndex] == u'"') startIndex++;

        tokenValue = originalText.substr(startIndex);
    }

    // We preserve everything from the original raw token.  Except we use the computed value text from the
    // interpolated text token instead as long as we got no diagnostics for this raw string.
    auto finalToken = WithDiagnosticsGreen(
        SyntaxFactory::Literal(originalToken->GetLeadingTrivia(), originalToken->text, originalToken->kind, tokenValue,
                               originalToken->GetTrailingTrivia()),
        diagnostics.data(), static_cast<int>(diagnostics.size()));

    return SyntaxFactory::LiteralExpression(expressionKind, finalToken);
}

GreenNode *LanguageParser::ParseInterpolatedStringToken() {
    // We don't want to make the scanner stateful (between tokens) if we can possibly avoid it.
    // The approach implemented here is
    //
    // (1) Scan the whole interpolated string literal as a single token. Now the statefulness of
    // the scanner (to match { }'s) is limited to its behavior while scanning a single token.
    //
    // (2) When the parser gets such a token, here, it spins up another scanner / parser on each of
    // the holes and builds a tree for the whole thing (resulting in an InterpolatedStringExpressionSyntax).
    //
    // (3) The parser discards the original token and replaces it with this tree. (In other words,
    // it replaces one token with a different set of tokens that have already been parsed)
    //
    // (4) On an incremental change, we widen the invalidated region to include any enclosing interpolated
    // string nonterminal so that we never reuse tokens inside a changed interpolated string.
    //
    // This has the secondary advantage that it can reasonably be specified.
    //
    // The substitution will end up being invisible to external APIs and clients such as the IDE, as
    // they have no way to ask for the stream of tokens before parsing.

    CS_ASSERT(this->CurrentToken()->kind == K::InterpolatedStringToken);
    auto originalToken = this->EatToken();

    return ParseInterpolatedOrRawStringToken(originalToken, /*isInterpolatedString*/ true);
}

/// Takes the token produced by the lexer for an (raw or regular) interpolated string or non-interpolated raw
/// string literal and creates an actual parsed InterpolatedStringExpressionSyntax for the syntax
/// tree.  For an interpolated string, this will now contain all the holes parsed out as well.  For a raw string
/// this will contain a single InterpolatedStringTextSyntax for the contents of the raw string.
GreenNode *LanguageParser::ParseInterpolatedOrRawStringToken(GreenNode *originalToken, bool isInterpolatedString) {
    // The token's text is a slice of the source (or of arena text): every substring of it outlives the parse.
    Text originalText = originalToken->text;
    Text originalTextSpan = originalText;

    // compute the positions of the interpolations in the original string literal, if there was an error or not,
    // and where the open and close quotes can be found.
    std::vector<Lexer::Interpolation> interpolations;

    Lexer::InterpolatedStringKind kind = Lexer::InterpolatedStringKind::Normal;
    std::optional<SyntaxDiagnosticInfo> error;
    TextRange openQuoteRange;
    TextRange closeQuoteRange;
    // rescanInterpolation
    {
        Lexer tempLexer(originalText, /*allowPreprocessorDirectives*/ false);
        Lexer::TokenInfo info;
        tempLexer.ScanInterpolatedOrRawStringLiteralTop(info, isInterpolatedString, error, kind, openQuoteRange,
                                                        &interpolations, closeQuoteRange);

        CS_ASSERT(isInterpolatedString || interpolations.empty());
    }

    // Only bother trying to do dedentation if we have a multiline literal without errors.  There's no point
    // trying in the presence of errors as we may not even be able to determine what the dedentation should be.
    auto needsDedentation = kind == Lexer::InterpolatedStringKind::MultiLineRaw && !error.has_value();

    auto getOpenQuote = [&]() -> GreenNode * {
        SyntaxKind startKind = K::InterpolatedStringStartToken;
        switch (kind) {
            case Lexer::InterpolatedStringKind::Normal:
                startKind = K::InterpolatedStringStartToken;
                break;
            case Lexer::InterpolatedStringKind::Verbatim:
                startKind = K::InterpolatedVerbatimStringStartToken;
                break;
            case Lexer::InterpolatedStringKind::SingleLineRaw:
                startKind = K::InterpolatedSingleLineRawStringStartToken;
                break;
            case Lexer::InterpolatedStringKind::MultiLineRaw:
                startKind = K::InterpolatedMultiLineRawStringStartToken;
                break;
        }
        return SyntaxFactory::Token(originalToken->GetLeadingTrivia(), startKind, Slice(originalText, openQuoteRange),
                                    /*trailing*/ nullptr);
    };

    // Gets the indentation whitespace from the last line of a multi-line raw literal.
    auto getIndentationWhitespace = [&](Text span) -> Text {
        // The content we want to create text token out of.  Effectively, what is in the text sections
        // minus leading whitespace.
        auto closeQuoteText = Slice(span, closeQuoteRange);

        // A multi-line raw interpolation without errors always ends with a new-line, some number of spaces, and
        // the quotes. So it's safe to just pull off the first two characters here to find where the
        // newline-ends.
        auto afterNewLine = SlidingTextWindow::GetNewLineWidth(closeQuoteText[0], closeQuoteText[1]);
        auto afterWhitespace = SkipWhitespace(closeQuoteText, afterNewLine);

        CS_ASSERT(closeQuoteText[afterWhitespace] == u'"');
        return Slice(closeQuoteText, afterNewLine, afterWhitespace);
    };

    auto makeContent = [&](Text indentationWhitespace, std::u16string &content, bool isFirst, bool isLast,
                           Text text) -> GreenNode * {
        if (text.empty()) {
            // For the raw string case, always include an InterpolatedStringText token, even if empty. This
            // allows the caller to uniformly assume there is always at least one text token that it can
            // extract data from.
            return isInterpolatedString
                       ? nullptr
                       : SyntaxFactory::InterpolatedStringText(SyntaxFactory::Literal(
                             /*leading*/ nullptr, u"", K::InterpolatedStringTextToken, u"", /*trailing*/ nullptr));
        }

        // If we're not dedenting then just make a standard interpolated text token.  Also, we can short-circuit
        // if the indentation whitespace is empty (nothing to dedent in that case).
        if (!needsDedentation || indentationWhitespace.empty())
            return SyntaxFactory::InterpolatedStringText(MakeInterpolatedStringTextToken(kind, text));

        content.clear();
        int currentIndex = 0;
        int textLength = static_cast<int>(text.size());

        // If we're not processing the first content chunk, then we must be processing a chunk that came after
        // an interpolation.  In that case, we need to consume up through the next newline of that chunk as
        // content that is not subject to dedentation.
        if (!isFirst) currentIndex = ConsumeRemainingContentThroughNewLine(content, text, currentIndex);

        // We're either the first item, or we consumed up through a newline from the previous line. We're
        // definitely at the start of a new line (or at the end).  Regardless, we want to consume each
        // successive line, making sure its indentation is correct.

        // Consume one line at a time.
        std::optional<SyntaxDiagnosticInfo> indentationError;
        while (currentIndex < textLength) {
            auto lineStartPosition = currentIndex;

            // Only bother reporting a single indentation error on a text chunk.
            if (!indentationError.has_value()) {
                currentIndex = SkipWhitespace(text, currentIndex);
                auto currentLineWhitespace = Slice(text, lineStartPosition, currentIndex);

                if (!currentLineWhitespace.starts_with(indentationWhitespace)) {
                    // We have a line where the indentation of that line isn't a prefix of indentation
                    // whitespace.
                    //
                    // If we're not on a blank line then this is bad.  That's a content line that doesn't start
                    // with the indentation whitespace.  If we are on a blank line then it's ok if the whitespace
                    // we do have is a prefix of the indentation whitespace.
                    auto isBlankLine = (currentIndex == textLength && isLast) ||
                                       (currentIndex < textLength &&
                                        SyntaxFacts::IsNewLine(text[static_cast<size_t>(currentIndex)]));
                    auto isLegalBlankLine = isBlankLine && indentationWhitespace.starts_with(currentLineWhitespace);
                    if (!isLegalBlankLine) {
                        // Specialized error message if this is a spacing difference.
                        std::u16string currentLineWhitespaceChar;
                        std::u16string indentationWhitespaceChar;
                        if (CheckForSpaceDifference(currentLineWhitespace, indentationWhitespace,
                                                    currentLineWhitespaceChar, indentationWhitespaceChar)) {
                            if (!indentationError.has_value()) {
                                indentationError = MakeError(lineStartPosition,
                                                             /*width*/ currentIndex - lineStartPosition,
                                                             ErrorCode::ERR_LineContainsDifferentWhitespace,
                                                             {ArgText(currentLineWhitespaceChar),
                                                              ArgText(indentationWhitespaceChar)});
                            }
                        } else {
                            if (!indentationError.has_value()) {
                                indentationError = MakeError(lineStartPosition,
                                                             /*width*/ currentIndex - lineStartPosition,
                                                             ErrorCode::ERR_LineDoesNotStartWithSameWhitespace);
                            }
                        }
                    }
                }
            }

            // Skip the leading whitespace that matches the terminator line and add any text after that to our content.
            currentIndex = std::min(currentIndex, lineStartPosition + static_cast<int>(indentationWhitespace.size()));
            currentIndex = ConsumeRemainingContentThroughNewLine(content, text, currentIndex);
        }

        // if we ran into any errors, don't give this item any special value.  It just has the value of our actual text.
        Text textString = text;
        Text valueString = indentationError.has_value() ? textString : CurrentArena()->CopyText(content);

        // Note: we place errors on the InterpolatedStringText node itself, not on the token.  This is an
        // invariant that higher up callers can depend on.
        auto node = SyntaxFactory::InterpolatedStringText(SyntaxFactory::Literal(
            /*leading*/ nullptr, textString, K::InterpolatedStringTextToken, valueString, /*trailing*/ nullptr));

        return indentationError.has_value() ? WithDiagnosticsGreen(node, &*indentationError, 1) : node;
    };

    // if the interpolation starts on its own line, then it has to have correct indentation whitespace
    // before it.  e.g.:
    //
    //      var x = """
    //          {1 + 1}
    //          """
    //
    // Not:
    //
    //      var x = """
    // {1 + 1}
    //          """
    //
    // Note: We don't need to check
    //
    //      var x = """
    // <space>{1 + 1}
    //          """
    //
    // as initial whitespace in text will already be checked in makeContent.  This is only for the case where
    // the interpolation is at the start of a line.
    auto getInterpolationIndentationError = [&](Text indentationWhitespace,
                                                const Lexer::Interpolation &interpolation)
        -> std::optional<SyntaxDiagnosticInfo> {
        if (needsDedentation && !indentationWhitespace.empty()) {
            auto openBracePosition = interpolation.OpenBraceRange.start;
            if (openBracePosition > 0 &&
                SyntaxFacts::IsNewLine(originalText[static_cast<size_t>(openBracePosition - 1)]))
                // Pass 0 as the offset to give the error on the interpolation brace.
                return MakeError(/*offset*/ 0, /*width*/ 1, ErrorCode::ERR_LineDoesNotStartWithSameWhitespace);
        }

        return std::nullopt;
    };

    auto getContent = [&](Text span) -> GreenNode * {
        std::u16string content;
        auto builder = _pool.Allocate();

        Text indentationWhitespace = needsDedentation ? getIndentationWhitespace(span) : Text();

        auto currentContentStart = openQuoteRange.end;
        for (size_t i = 0; i < interpolations.size(); i++) {
            auto interpolation = interpolations[i];

            // Add a token for text preceding the interpolation
            builder.Add(makeContent(indentationWhitespace, content, /*isFirst*/ i == 0, /*isLast*/ false,
                                    Slice(span, currentContentStart, interpolation.OpenBraceRange.start)));

            // Now parse the interpolation itself.
            auto interpolationNode =
                ParseInterpolation(originalText, interpolation, kind, IsInAsync(), IsInFieldKeywordContext());

            // Make sure the interpolation starts at the right location.
            auto indentationError = getInterpolationIndentationError(indentationWhitespace, interpolation);
            if (indentationError.has_value())
                interpolationNode = WithDiagnosticsGreen(interpolationNode, &*indentationError, 1);

            builder.Add(interpolationNode);
            currentContentStart = interpolation.CloseBraceRange.end;
        }

        // Add a token for text following the last interpolation
        builder.Add(makeContent(indentationWhitespace, content, /*isFirst*/ interpolations.empty(), /*isLast*/ true,
                                Slice(span, currentContentStart, closeQuoteRange.start)));

        GreenNode *result = builder.ToList();
        _pool.Free(builder);
        return result;
    };

    auto getCloseQuote = [&]() -> GreenNode * {
        // Make a token for the close quote " (even if it was missing)
        SyntaxKind endKind = K::InterpolatedStringEndToken;
        switch (kind) {
            case Lexer::InterpolatedStringKind::Normal:
            case Lexer::InterpolatedStringKind::Verbatim:
                endKind = K::InterpolatedStringEndToken;
                break;
            case Lexer::InterpolatedStringKind::SingleLineRaw:
            case Lexer::InterpolatedStringKind::MultiLineRaw:
                endKind = K::InterpolatedRawStringEndToken;
                break;
        }
        return TokenOrMissingToken(/*leading*/ nullptr, endKind, Slice(originalText, closeQuoteRange),
                                   originalToken->GetTrailingTrivia());
    };

    auto openQuote = getOpenQuote();
    auto contents = getContent(originalTextSpan);
    auto closeQuote = getCloseQuote();
    GreenNode *result = SyntaxFactory::InterpolatedStringExpression(openQuote, contents, closeQuote);

    if (error.has_value()) result = WithDiagnosticsGreen(result, &*error, 1);

    return result;
}

/// Converts a whitespace character to its string representation for error messages.
std::u16string LanguageParser::CharToString(char16_t ch) {
    switch (ch) {
        case u'\t':
            return u"\\t";
        case u'\v':
            return u"\\v";
        case u'\f':
            return u"\\f";
        default: {
            // @$"\u{(int)ch:x4}"
            static const char16_t kHex[] = u"0123456789abcdef";
            std::u16string s = u"\\u";
            for (int shift = 12; shift >= 0; shift -= 4) s += kHex[(ch >> shift) & 0xF];
            return s;
        }
    }
}

/// Checks if two whitespace sequences differ at a specific character position where both
/// characters are whitespace but different types (e.g., tab vs space).
bool LanguageParser::CheckForSpaceDifference(Text currentLineWhitespace, Text indentationLineWhitespace,
                                             std::u16string &currentLineMessage,
                                             std::u16string &indentationLineMessage) {
    for (size_t i = 0, n = std::min(currentLineWhitespace.size(), indentationLineWhitespace.size()); i < n; i++) {
        auto currentLineChar = currentLineWhitespace[i];
        auto indentationLineChar = indentationLineWhitespace[i];

        if (currentLineChar != indentationLineChar && SyntaxFacts::IsWhitespace(currentLineChar) &&
            SyntaxFacts::IsWhitespace(indentationLineChar)) {
            currentLineMessage = CharToString(currentLineChar);
            indentationLineMessage = CharToString(indentationLineChar);
            return true;
        }
    }

    currentLineMessage.clear();
    indentationLineMessage.clear();
    return false;
}

GreenNode *LanguageParser::TokenOrMissingToken(GreenNode *leading, SyntaxKind kind, Text text, GreenNode *trailing) {
    return text.empty() ? SyntaxFactory::MissingToken(leading, kind, trailing)
                        : SyntaxFactory::Token(leading, kind, text, trailing);
}

int LanguageParser::SkipWhitespace(Text text, int currentIndex) {
    while (currentIndex < static_cast<int>(text.size()) &&
           SyntaxFacts::IsWhitespace(text[static_cast<size_t>(currentIndex)]))
        currentIndex++;
    return currentIndex;
}

int LanguageParser::ConsumeRemainingContentThroughNewLine(std::u16string &content, Text text, int currentIndex) {
    auto start = currentIndex;
    int length = static_cast<int>(text.size());
    while (currentIndex < length) {
        auto ch = text[static_cast<size_t>(currentIndex)];
        if (!SyntaxFacts::IsNewLine(ch)) {
            currentIndex++;
            continue;
        }

        currentIndex += SlidingTextWindow::GetNewLineWidth(
            ch, currentIndex + 1 < length ? text[static_cast<size_t>(currentIndex + 1)] : u'\0');
        break;
    }

    content.append(Slice(text, start, currentIndex));
    return currentIndex;
}

GreenNode *LanguageParser::ParseInterpolation(Text text, Lexer::Interpolation interpolation,
                                              Lexer::InterpolatedStringKind kind, bool isInAsyncContext,
                                              bool isInFieldKeywordContext) {
    // Grab the text from after the { all the way to the start of the } (or the start of the : if present). This
    // will be used to parse out the expression of the interpolation.
    //
    // The parsing of the open brace, close brace and colon is specially handled in ParseInterpolation below.
    auto followingRange = interpolation.HasColon() ? interpolation.ColonRange : interpolation.CloseBraceRange;
    // A slice of `text` (itself a slice of the source): stable for the tokens the temporary lexer makes.
    auto expressionText = Slice(text, interpolation.OpenBraceRange.end, followingRange.start);

    Lexer tempLexer(expressionText, /*allowPreprocessorDirectives*/ false,
                    /*interpolationFollowedByColon*/ interpolation.HasColon());

    // First grab any trivia right after the {, it will be trailing trivia for the { token.
    auto openTokenTrailingTrivia = tempLexer.LexSyntaxTrailingTrivia();

    // Now create a parser to actually handle the expression portion of the interpolation
    LanguageParser tempParser(&tempLexer);
    ParserSyntaxContextResetter _(&tempParser, /*isInAsyncContext*/ isInAsyncContext, /*isInQueryContext*/ std::nullopt,
                                  /*isInFieldKeywordContext*/ isInFieldKeywordContext);

    auto result = tempParser.ParseInterpolation(
        text, interpolation, kind,
        SyntaxFactory::Token(/*leading*/ nullptr, K::OpenBraceToken, Slice(text, interpolation.OpenBraceRange),
                             openTokenTrailingTrivia));

    return result;
}

GreenNode *LanguageParser::ParseInterpolation(Text text, Lexer::Interpolation interpolation,
                                              Lexer::InterpolatedStringKind kind, GreenNode *openBraceToken) {
    auto getInterpolationCloseToken = [&](GreenNode *leading) -> GreenNode * {
        return TokenOrMissingToken(leading, K::CloseBraceToken, Slice(text, interpolation.CloseBraceRange),
                                   /*trailing*/ nullptr);
    };

    // getExpressionAndAlignment
    GreenNode *expression;
    GreenNode *alignment;
    {
        auto expr = this->ParseExpressionCore();

        if (this->CurrentToken()->kind != K::CommaToken) {
            expression = this->ConsumeUnexpectedTokens(expr);
            alignment = nullptr;
        } else {
            auto commaToken = this->EatToken(K::CommaToken);
            auto value = this->ConsumeUnexpectedTokens(this->ParseExpressionCore());
            alignment = SyntaxFactory::InterpolationAlignmentClause(commaToken, value);
            expression = expr;
        }
    }

    // getFormatAndCloseBrace
    GreenNode *format;
    GreenNode *closeBraceToken;
    {
        auto leading = this->CurrentToken()->GetLeadingTrivia();
        if (interpolation.HasColon()) {
            auto colonToken =
                SyntaxFactory::Token(leading, K::ColonToken, Slice(text, interpolation.ColonRange), /*trailing*/ nullptr);
            auto formatStringToken = MakeInterpolatedStringTextToken(
                kind, Slice(text, interpolation.ColonRange.end, interpolation.CloseBraceRange.start));
            format = SyntaxFactory::InterpolationFormatClause(colonToken, formatStringToken);
            closeBraceToken = getInterpolationCloseToken(/*leading*/ nullptr);
        } else {
            format = nullptr;
            closeBraceToken = getInterpolationCloseToken(leading);
        }
    }

    auto result = SyntaxFactory::Interpolation(openBraceToken, expression, alignment, format, closeBraceToken);
    return result;
}

/// Interpret the given raw text from source as an InterpolatedStringTextToken.
/// `text`: the text for the full string literal, including the quotes and contents
/// `kind`: the kind of the interpolated string we were processing
GreenNode *LanguageParser::MakeInterpolatedStringTextToken(Lexer::InterpolatedStringKind kind, Text text) {
    // with a raw string, we don't do any interpretation of the content.  Note: removal of indentation is
    // handled already in splitContent
    if (kind == Lexer::InterpolatedStringKind::SingleLineRaw || kind == Lexer::InterpolatedStringKind::MultiLineRaw)
        return SyntaxFactory::Literal(/*leading*/ nullptr, text, K::InterpolatedStringTextToken, text,
                                      /*trailing*/ nullptr);

    CS_ASSERT(kind == Lexer::InterpolatedStringKind::Normal || kind == Lexer::InterpolatedStringKind::Verbatim);

    // For a normal/verbatim piece of content, process the inner content as if it was in a corresponding
    // *non*-interpolated string to get the correct meaning of all the escapes/diagnostics within.
    std::u16string prefix = kind == Lexer::InterpolatedStringKind::Verbatim ? u"@\"" : u"\"";
    std::u16string fakeString = prefix;
    fakeString.append(text);
    fakeString += u'"';
    // The temporary lexer's tokens may point into its text: give it arena-owned text.
    Text fakeText = CurrentArena()->CopyText(fakeString);
    Lexer tempLexer(fakeText, /*allowPreprocessorDirectives*/ false);
    auto mode = LexerMode::Syntax;
    auto token = tempLexer.LexRef(mode);
    CS_ASSERT(token->kind == K::StringLiteralToken);
    auto result = SyntaxFactory::Literal(/*leading*/ nullptr, text, K::InterpolatedStringTextToken, token->value_text,
                                         /*trailing*/ nullptr);
    if (token->ContainsDiagnostics()) {
        auto moved = MoveDiagnostics(token->diags, token->diag_count, -static_cast<int>(prefix.size()));
        result = WithDiagnosticsGreen(result, moved.data(), static_cast<int>(moved.size()));
    }

    return result;
}

std::vector<SyntaxDiagnosticInfo> LanguageParser::MoveDiagnostics(const SyntaxDiagnosticInfo *infos, int count,
                                                                  int offset) {
    std::vector<SyntaxDiagnosticInfo> builder;
    if (infos == nullptr || count == 0) return builder;

    builder.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        // This cast should always be safe.  We are only moving diagnostics produced on syntax nodes and tokens.
        SyntaxDiagnosticInfo sd = infos[i];
        sd.offset = sd.offset + offset;  // WithOffset
        builder.push_back(sd);
    }

    return builder;
}

}  // namespace csharp
