// LanguageParser, unit C: a translation of Roslyn's
// src/Compilers/CSharp/Portable/Parser/LanguageParser.cs (MIT, (c) .NET
// Foundation and Contributors), lines 5248-8196 -- field and variable
// declarators, delegates, enums, identifiers, type parameter lists, type
// argument lists, names, and types (the speculative ScanType family, ParseType,
// array rank specifiers, tuple types, function pointer types, pointer type
// modifiers). See csharp_parser_internal.h for the translation conventions;
// the methods are in the C# order.
#include <utility>

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief Runs a callable at scope exit (the C# `finally` blocks of this file). */
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() { f_(); }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    F f_;
};

}  // namespace

bool LanguageParser::IsEndOfFieldDeclaration() { return this->CurrentToken()->kind == SyntaxKind::SemicolonToken; }

SeparatedSyntaxList LanguageParser::ParseFieldDeclarationVariableDeclarators(GreenNode *type, VariableFlags flags,
                                                                             SyntaxKind parentKind) {
    // Although we try parse variable declarations in contexts where they are not allowed (non-interactive top-level or a namespace)
    // the reported errors should take into consideration whether or not one expects them in the current context.
    bool variableDeclarationsExpected = parentKind != SyntaxKind::NamespaceDeclaration &&
                                        parentKind != SyntaxKind::FileScopedNamespaceDeclaration &&
                                        (parentKind != SyntaxKind::CompilationUnit || IsScript());

    auto variables = _pool.AllocateSeparated();

    auto saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfFieldDeclaration;

    GreenNode *localFunction = nullptr;
    ParseVariableDeclarators(type, flags, variables, variableDeclarationsExpected,
                             /*allowLocalFunctions:*/ false,
                             // A field declaration doesn't have a `(...)` construct.  So no need to stop if we hit a close paren
                             // after a declarator.  Let normal error recovery kick in.
                             /*stopOnCloseParen:*/ false,
                             /*attributes:*/ nullptr,
                             /*mods:*/ nullptr, localFunction);
    CS_ASSERT(localFunction == nullptr);

    _termState = saveTerm;
    return _pool.ToListAndFree(variables);
}

void LanguageParser::ParseVariableDeclarators(GreenNode *type, VariableFlags flags, SeparatedSyntaxListBuilder variables,
                                              bool variableDeclarationsExpected, bool allowLocalFunctions,
                                              bool stopOnCloseParen, SyntaxList attributes, SyntaxList mods,
                                              GreenNode *&localFunction) {
    variables.Add(this->ParseVariableDeclarator(type, flags, /*isFirst:*/ true, allowLocalFunctions, attributes, mods,
                                                localFunction));

    if (localFunction != nullptr) {
        // ParseVariableDeclarator returns null, so it is not added to variables
        CS_ASSERT(variables.Count() == 0);
        return;
    }

    while (true) {
        if (this->CurrentToken()->kind == SyntaxKind::SemicolonToken) {
            break;
        } else if (stopOnCloseParen && this->CurrentToken()->kind == SyntaxKind::CloseParenToken) {
            break;
        } else if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
            // If we see `for (int i = 0, i < ...` then we do not want to consume the second 'i' as the next declarator as it
            // is more likely that the user meant to write `for (int i = 0; i < ...` instead and accidentally
            // used a comma instead of a semicolon.
            //
            // Note: the legal forms that we must keep parsing as a variable declarator are:
            //
            //      for (int i = 0, j, k; ...       // identifier comma
            //      for (int i = 0, j = ...         // identifier equals
            //      for (int i = 0, j; ...          // identifier semicolon
            //
            // We also accept: `for (int i = 0, ;` as that's likely an intermediary state prior to writing the
            // next variable. Anything else we'll treat as as more likely to be the following conditional.

            if (Any(flags & VariableFlags::ForStatement) && this->PeekToken(1)->kind != SyntaxKind::SemicolonToken) {
                SyntaxKind peek2 = this->PeekToken(2)->kind;
                bool isLegalVariableDeclaratorStart =
                    IsTrueIdentifier(this->PeekToken(1)) &&
                    (peek2 == SyntaxKind::CommaToken || peek2 == SyntaxKind::EqualsToken ||
                     peek2 == SyntaxKind::SemicolonToken);

                if (!isLegalVariableDeclaratorStart) break;
            }

            variables.AddSeparator(this->EatToken(SyntaxKind::CommaToken));
            variables.Add(this->ParseVariableDeclarator(type, flags, /*isFirst:*/ false, /*allowLocalFunctions:*/ false,
                                                        attributes, mods, localFunction));

            CS_ASSERT(localFunction == nullptr);
        } else if (!variableDeclarationsExpected ||
                   this->SkipBadVariableListTokens(variables, SyntaxKind::CommaToken) == PostSkipAction::Abort) {
            break;
        }
    }
}

LanguageParser::PostSkipAction LanguageParser::SkipBadVariableListTokens(SeparatedSyntaxListBuilder list, SyntaxKind expected) {
    GreenNode *tmp = nullptr;
    CS_ASSERT(list.Count() > 0);
    return this->SkipBadSeparatedListTokensWithExpectedKind(
        tmp, list, [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CommaToken; },
        [](LanguageParser *p, SyntaxKind) { return p->CurrentToken()->kind == SyntaxKind::SemicolonToken; }, expected);
}

// GetOriginalModifiers, WasFirstVariable, GetOriginalVariableFlags, CanReuseVariableDeclarator: incremental
// parsing only (they inspect old red nodes), not ported.

GreenNode *LanguageParser::ParseVariableDeclarator(GreenNode *parentType, VariableFlags flags, bool isFirst,
                                                   bool allowLocalFunctions, SyntaxList attributes, SyntaxList mods,
                                                   GreenNode *&localFunction, bool isExpressionContext) {
    // (IsIncrementalAndFactoryContextMatches && CanReuseVariableDeclarator(...): incremental parsing only.)

    if (!isExpressionContext) {
        // Check for the common pattern of:
        //
        // C                    //<-- here
        // Console.WriteLine();
        //
        // Standard greedy parsing will assume that this should be parsed as a variable
        // declaration: "C Console".  We want to avoid that as it can confused parts of the
        // system further up.  So, if we see certain things following the identifier, then we can
        // assume it's not the actual name.
        //
        // So, if we're after a newline and we see a name followed by the list below, then we
        // assume that we're accidentally consuming too far into the next statement.
        //
        // <dot>, <arrow>, any binary operator (except =), <question>.  None of these characters
        // are allowed in a normal variable declaration.  This also provides a more useful error
        // message to the user.  Instead of telling them that a semicolon is expected after the
        // following token, then instead get a useful message about an identifier being missing.
        DisposableResetPoint _(this, /*resetOnDispose:*/ true);

        auto currentTokenKind = this->CurrentToken()->kind;
        if (currentTokenKind == SyntaxKind::IdentifierToken && !parentType->IsMissing()) {
            auto isAfterNewLine =
                SyntaxList(parentType->GetLastToken()->GetTrailingTrivia()).Any(SyntaxKind::EndOfLineTrivia);
            if (isAfterNewLine) {
                // Note: this token is free to get.  There is a cached singleton in SyntaxFactory for it.
                auto missingIdentifier = CreateMissingIdentifierToken();
                auto [offset, width] = this->GetDiagnosticSpanForMissingNodeOrToken(missingIdentifier);

                this->EatToken();
                currentTokenKind = this->CurrentToken()->kind;

                auto isNonEqualsBinaryToken = currentTokenKind != SyntaxKind::EqualsToken &&
                                              SyntaxFacts::IsBinaryExpressionOperatorToken(currentTokenKind);

                if (currentTokenKind == SyntaxKind::DotToken || currentTokenKind == SyntaxKind::OpenParenToken ||
                    currentTokenKind == SyntaxKind::MinusGreaterThanToken || isNonEqualsBinaryToken) {
                    auto isPossibleLocalFunctionToken =
                        currentTokenKind == SyntaxKind::OpenParenToken || currentTokenKind == SyntaxKind::LessThanToken;

                    // Make sure this isn't a local function
                    if (!isPossibleLocalFunctionToken || !IsLocalFunctionAfterIdentifier()) {
                        missingIdentifier =
                            this->AddError(missingIdentifier, offset, width, ErrorCode::ERR_IdentifierExpected);

                        localFunction = nullptr;
                        // (The reset point resets the parser after the declarator is built, as the C# `using` does.)
                        return SyntaxFactory::VariableDeclarator(missingIdentifier, nullptr, nullptr);
                    }
                }
            }
        }
    }

    // NOTE: Diverges from Dev10.
    //
    // When we see parse an identifier and we see the partial contextual keyword, we check
    // to see whether it is already attached to a partial class or partial method
    // declaration.  However, in the specific case of variable declarators, Dev10
    // specifically treats it as a variable name, even if it could be interpreted as a
    // keyword.
    auto name = this->ParseIdentifierToken();
    GreenNode *argumentList = nullptr;
    GreenNode *initializer = nullptr;
    TerminatorState saveTerm = _termState;
    bool isFixed = Any(flags & VariableFlags::Fixed);
    bool isConst = Any(flags & VariableFlags::Const);
    bool isLocalOrField = Any(flags & VariableFlags::LocalOrField);

    // Give better error message in the case where the user did something like:
    //
    // X x = 1, Y y = 2;
    // using (X x = expr1, Y y = expr2) ...
    //
    // The superfluous type name is treated as variable (it is an identifier) and a missing ',' is injected after it.
    if (!isFirst && this->IsTrueIdentifier()) {
        name = this->AddError(name, ErrorCode::ERR_MultiTypeInDeclaration);
    }

    auto looksLikeVariableInitializer = [&]() -> bool {
        // Note: this check is redundant, as CanStartExpression will return false for an equals-token. However,
        // we want to guarantee that this always holds true, and thus the caller will *always* report an error
        // when trying to consume the equals token.  That ensures that we it's then ok to skip other syntax
        // errors that are reported with variable declarators.
        if (this->CurrentToken()->kind == SyntaxKind::EqualsToken) return false;

        // If we see a token that can start an expression after the identifier (e.g., "int value 5;"),
        // treat it as a missing '=' and parse the initializer.
        //
        // Do this except for cases that are better served by saying we have a missing comma.  Specifically:
        //
        //      Type t1 t2 t3
        //      Type t1 t2,
        //      Type t1 t2 = ...
        //      Type t1 t2;
        //      Type t1 t2)    // likely an incorrect tuple.
        SyntaxKind peek1 = this->PeekToken(1)->kind;
        auto shouldParseAsNextDeclarator =
            this->CurrentToken()->kind == SyntaxKind::IdentifierToken &&
            (peek1 == SyntaxKind::IdentifierToken || peek1 == SyntaxKind::CommaToken ||
             peek1 == SyntaxKind::EqualsToken || peek1 == SyntaxKind::SemicolonToken ||
             peek1 == SyntaxKind::CloseParenToken || peek1 == SyntaxKind::EndOfFileToken);
        if (shouldParseAsNextDeclarator) return false;

        if (ContainsErrorDiagnostic(name)) return false;

        if (!CanStartExpression()) return false;

        DisposableResetPoint _(this, /*resetOnDispose:*/ true);
        // (C# `initializer`; renamed: it would shadow the enclosing method's local.)
        auto initializerExpression = this->ParseExpressionCore();

        // If we see a type following, then prefer to view this as a declarator for the next variable.
        if (KindDerivesFrom(initializerExpression->kind, "TypeSyntax")) return false;

        return !ContainsErrorDiagnostic(initializerExpression);
    };

    // The C# switch, with its `goto case` / `goto default` jumps as labels.
    {
        SyntaxKind kind = this->CurrentToken()->kind;
        if (kind == SyntaxKind::EqualsToken) {
            goto case_EqualsToken;
        } else if (kind == SyntaxKind::LessThanToken) {
            if (allowLocalFunctions && isFirst) {
                localFunction = TryParseLocalFunctionStatementBody(attributes, mods, parentType, name);
                if (localFunction != nullptr) {
                    return nullptr;
                }
            }
            goto case_default;
        } else if (kind == SyntaxKind::OpenParenToken) {
            if (allowLocalFunctions && isFirst) {
                localFunction = TryParseLocalFunctionStatementBody(attributes, mods, parentType, name);
                if (localFunction != nullptr) {
                    return nullptr;
                }
            }

            // Special case for accidental use of C-style constructors
            // Fake up something to hold the arguments.
            _termState |= TerminatorState::IsPossibleEndOfVariableDeclaration;
            argumentList = this->ParseBracketedArgumentList();
            _termState = saveTerm;
            argumentList = this->AddError(argumentList, ErrorCode::ERR_BadVarDecl);
            goto switch_end;
        } else if (kind == SyntaxKind::OpenBracketToken) {
            goto case_OpenBracketToken;
        } else {
            goto case_default;
        }
    }

case_EqualsToken: {
    if (isFixed) {
        goto case_default;
    }

    auto equals = this->EatToken();

    // check for lambda expression with explicit ref return type: `ref int () => { ... }`
    auto refKeyword = isLocalOrField && !isConst && this->CurrentToken()->kind == SyntaxKind::RefKeyword &&
                              !this->IsPossibleLambdaExpression(Precedence::Expression)
                          ? this->EatToken()
                          : nullptr;

    auto init = this->ParseVariableInitializer();
    initializer =
        SyntaxFactory::EqualsValueClause(equals, refKeyword == nullptr ? init : SyntaxFactory::RefExpression(refKeyword, init));
    goto switch_end;
}

case_OpenBracketToken: {
    bool sawNonOmittedSize;
    _termState |= TerminatorState::IsPossibleEndOfVariableDeclaration;
    auto specifier = this->ParseArrayRankSpecifier(sawNonOmittedSize);
    _termState = saveTerm;
    auto open = GetField(specifier, Field::OpenBracketToken);
    SeparatedSyntaxList sizes = GetField(specifier, Field::Sizes);
    auto close = GetField(specifier, Field::CloseBracketToken);
    if (isFixed && !sawNonOmittedSize) {
        close = this->AddError(close, ErrorCode::ERR_ValueExpected);
    }

    auto args = _pool.AllocateSeparated();
    auto withSeps = sizes.GetWithSeparators();
    for (int i = 0; i < withSeps.Count(); i++) {
        GreenNode *item = withSeps[i];
        if (!item->IsToken()) {  // item is ExpressionSyntax
            GreenNode *expression = item;
            bool isOmitted = expression->kind == SyntaxKind::OmittedArraySizeExpression;
            if (!isFixed && !isOmitted) {
                expression = this->AddError(expression, ErrorCode::ERR_ArraySizeInDeclaration);
            }

            args.Add(SyntaxFactory::Argument(nullptr, /*refKindKeyword:*/ nullptr, expression));
        } else {
            args.AddSeparator(item);
        }
    }

    argumentList = SyntaxFactory::BracketedArgumentList(open, _pool.ToListAndFree(args), close);
    if (!isFixed) {
        argumentList = this->AddError(argumentList, ErrorCode::ERR_CStyleArray);
        // If we have "int x[] = new int[10];" then parse the initializer.
        if (this->CurrentToken()->kind == SyntaxKind::EqualsToken) {
            goto case_EqualsToken;
        }
    }

    goto switch_end;
}

case_default: {
    CS_ASSERT(argumentList == nullptr);
    CS_ASSERT(initializer == nullptr);

    // Note: it is ok that we do this work prior to the isConst/isFixed checks below.  If it looks like
    // a variable initializer, that means we're missing at least an equals and we'll report that error
    // here.  So it's fine to not report the other errors related to const/fixed as they can be fixed up
    // once the user adds the '='.
    if (looksLikeVariableInitializer()) {
        CS_ASSERT(this->CurrentToken()->kind != SyntaxKind::EqualsToken);

        localFunction = nullptr;
        auto equalsToken = this->EatToken(SyntaxKind::EqualsToken);
        return SyntaxFactory::VariableDeclarator(name, /*argumentList:*/ nullptr,
                                                 SyntaxFactory::EqualsValueClause(equalsToken, this->ParseVariableInitializer()));
    }

    if (isConst) {
        name = this->AddError(name, ErrorCode::ERR_ConstValueRequired);  // Error here for missing constant initializers
    } else if (isFixed) {
        if (parentType->kind == SyntaxKind::ArrayType) {
            // They accidentally put the array before the identifier
            name = this->AddError(name, ErrorCode::ERR_FixedDimsRequired);
        } else {
            goto case_OpenBracketToken;
        }
    }

    goto switch_end;
}

switch_end:
    localFunction = nullptr;
    return SyntaxFactory::VariableDeclarator(name, argumentList, initializer);
}

// Is there a local function after an eaten identifier?
bool LanguageParser::IsLocalFunctionAfterIdentifier() {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::OpenParenToken ||
              this->CurrentToken()->kind == SyntaxKind::LessThanToken);

    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

    auto typeParameterListOpt = this->ParseTypeParameterList();
    (void)typeParameterListOpt;
    auto paramList = ParseParenthesizedParameterList(/*forExtensionOrUnion:*/ false);

    if (!paramList->IsMissing() &&
        (this->CurrentToken()->kind == SyntaxKind::OpenBraceToken ||
         this->CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken ||
         this->CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword)) {
        return true;
    }

    return false;
}

bool LanguageParser::IsPossibleEndOfVariableDeclaration() {
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::CommaToken:
        case SyntaxKind::SemicolonToken:
            return true;
        default:
            return false;
    }
}

GreenNode *LanguageParser::ParseVariableInitializer() {
    return this->CurrentToken()->kind == SyntaxKind::OpenBraceToken ? this->ParseArrayInitializer()
                                                                    : this->ParseExpressionCore();
}

bool LanguageParser::IsPossibleVariableInitializer() {
    return this->CurrentToken()->kind == SyntaxKind::OpenBraceToken || this->IsPossibleExpression();
}

GreenNode *LanguageParser::ParseConstantFieldDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                         SyntaxKind parentKind) {
    modifiers.Add(this->EatToken(SyntaxKind::ConstKeyword));

    auto type = this->ParseType();
    // (C# evaluates the arguments left to right; spelled out so C++ does the same.)
    GreenNode *mods = modifiers.ToList();
    GreenNode *declaration = SyntaxFactory::VariableDeclaration(
        type, this->ParseFieldDeclarationVariableDeclarators(type, VariableFlags::Const, parentKind));
    GreenNode *semicolon = this->EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::FieldDeclaration(attributes, mods, declaration, semicolon);
}

GreenNode *LanguageParser::ParseDelegateDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::DelegateKeyword);

    auto delegateToken = this->EatToken(SyntaxKind::DelegateKeyword);
    auto type = this->ParseReturnType();
    auto saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfMethodSignature;
    auto name = this->ParseIdentifierToken();
    auto typeParameters = this->ParseTypeParameterList();
    auto parameterList = this->ParseParenthesizedParameterList(/*forExtensionOrUnion:*/ false);
    SyntaxListBuilder constraints;  // default(SyntaxListBuilder<TypeParameterConstraintClauseSyntax>)

    if (this->CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
        constraints = _pool.Allocate();
        this->ParseTypeParameterConstraintClauses(constraints);
    }

    _termState = saveTerm;

    GreenNode *constraintList = _pool.ToListAndFree(constraints);
    GreenNode *semicolon = this->EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::DelegateDeclaration(attributes, modifiers.ToList(), delegateToken, type, name, typeParameters,
                                              parameterList, constraintList, semicolon);
}

GreenNode *LanguageParser::ParseEnumDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::EnumKeyword);

    auto enumToken = this->EatToken(SyntaxKind::EnumKeyword);
    auto name = this->ParseIdentifierToken();

    // check to see if the user tried to create a generic enum.
    auto typeParameters = this->ParseTypeParameterList();

    if (typeParameters != nullptr) {
        name = AddTrailingSkippedSyntax(name, typeParameters);
        name = this->AddError(name, ErrorCode::ERR_UnexpectedGenericName);
    }

    GreenNode *baseList = nullptr;
    if (this->CurrentToken()->kind == SyntaxKind::ColonToken) {
        auto colon = this->EatToken(SyntaxKind::ColonToken);
        auto type = this->ParseType();
        auto tmpList = _pool.AllocateSeparated();
        tmpList.Add(SyntaxFactory::SimpleBaseType(type));
        baseList = SyntaxFactory::BaseList(colon, _pool.ToListAndFree(tmpList));
    }

    auto skipBadEnumMemberListTokens = [](LanguageParser *self, GreenNode *&openBrace, SyntaxListBuilder list,
                                          SyntaxKind expectedKind, SyntaxKind closeKind) -> PostSkipAction {
        return self->SkipBadSeparatedListTokensWithExpectedKind(
            openBrace, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken &&
                       p->CurrentToken()->kind != SyntaxKind::SemicolonToken && !p->IsPossibleEnumMemberDeclaration();
            },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    SeparatedSyntaxList members;  // default(SeparatedSyntaxList<EnumMemberDeclarationSyntax>)
    GreenNode *semicolon;
    GreenNode *openBrace;
    GreenNode *closeBrace;

    if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        semicolon = EatToken(SyntaxKind::SemicolonToken);
        openBrace = nullptr;
        closeBrace = nullptr;
    } else {
        openBrace = this->EatToken(SyntaxKind::OpenBraceToken);

        if (!openBrace->IsMissing()) {
            // It's not uncommon for people to use semicolons to separate out enum members.  So be resilient to
            // that, successfully consuming them as separators, while telling the user it needs to be a comma
            // instead.
            members = this->ParseCommaSeparatedSyntaxList(
                openBrace, SyntaxKind::CloseBraceToken,
                [](LanguageParser *self) { return self->IsPossibleEnumMemberDeclaration(); },
                [](LanguageParser *self) { return self->ParseEnumMemberDeclaration(); }, skipBadEnumMemberListTokens,
                /*allowTrailingSeparator:*/ true,
                /*requireOneElement:*/ false,
                /*allowSemicolonAsSeparator:*/ true);
        }

        closeBrace = this->EatToken(SyntaxKind::CloseBraceToken);
        semicolon = TryEatToken(SyntaxKind::SemicolonToken);
    }

    return SyntaxFactory::EnumDeclaration(attributes, modifiers.ToList(), enumToken, name, baseList, openBrace, members,
                                          closeBrace, semicolon);
}

GreenNode *LanguageParser::ParseEnumMemberDeclaration() {
    // (IsIncrementalAndFactoryContextMatches && CurrentNodeKind == EnumMemberDeclaration: incremental parsing only.)

    auto memberAttrs = this->ParseAttributeDeclarations(/*inExpressionContext:*/ false);
    auto memberName = this->ParseIdentifierToken();
    GreenNode *equalsValue = nullptr;
    if (this->CurrentToken()->kind == SyntaxKind::EqualsToken) {
        // an identifier is a valid expression
        auto equalsToken = this->EatToken(SyntaxKind::EqualsToken);
        equalsValue = SyntaxFactory::EqualsValueClause(
            equalsToken, this->CurrentToken()->kind == SyntaxKind::CommaToken ||
                                 this->CurrentToken()->kind == SyntaxKind::CloseBraceToken
                             ? this->ParseIdentifierName(ErrorCode::ERR_ConstantExpected)
                             : this->ParseExpressionCore());
    }

    return SyntaxFactory::EnumMemberDeclaration(memberAttrs, /*modifiers:*/ nullptr, memberName, equalsValue);
}

bool LanguageParser::IsPossibleEnumMemberDeclaration() {
    return this->CurrentToken()->kind == SyntaxKind::OpenBracketToken || this->IsTrueIdentifier();
}

bool LanguageParser::IsDotOrColonColon() {
    return this->CurrentToken()->kind == SyntaxKind::DotToken || this->CurrentToken()->kind == SyntaxKind::ColonColonToken;
}

// This is public and parses open types. You probably don't want to use it.
GreenNode *LanguageParser::ParseName() { return this->ParseQualifiedName(); }

GreenNode *LanguageParser::CreateMissingIdentifierName() {
    return SyntaxFactory::IdentifierName(CreateMissingIdentifierToken());
}

GreenNode *LanguageParser::CreateMissingIdentifierToken() { return SyntaxFactory::MissingToken(SyntaxKind::IdentifierToken); }

/// True if current identifier token is not really some contextual keyword
bool LanguageParser::IsTrueIdentifier() {
    if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken) {
        if (!IsCurrentTokenPartialKeywordOfPartialMemberOrType() && !IsCurrentTokenQueryKeywordInQuery() &&
            !IsCurrentTokenWhereOfConstraintClause()) {
            return true;
        }
    }

    return false;
}

/// True if the given token is not really some contextual keyword.
/// This method is for use in executable code, as it treats <c>partial</c> as an identifier.
bool LanguageParser::IsTrueIdentifier(GreenNode *token) {
    return token->kind == SyntaxKind::IdentifierToken && !(this->IsInQuery() && IsTokenQueryContextualKeyword(token));
}

GreenNode *LanguageParser::ParseIdentifierName(ErrorCode code) {
    // (IsIncrementalAndFactoryContextMatches && CurrentNodeKind == IdentifierName: incremental parsing only.)

    return SyntaxFactory::IdentifierName(ParseIdentifierToken(code));
}

GreenNode *LanguageParser::ParseIdentifierToken(ErrorCode code) {
    auto ctk = this->CurrentToken()->kind;
    if (ctk == SyntaxKind::IdentifierToken) {
        // Error tolerance for IntelliSense. Consider the following case: [EditorBrowsable( partial class Goo {
        // } Because we're parsing an attribute argument we'll end up consuming the "partial" identifier and
        // we'll eventually end up in a pretty confused state.  Because of that it becomes very difficult to
        // show the correct parameter help in this case.  So, when we see "partial" we check if it's being used
        // as an identifier or as a contextual keyword.  If it's the latter then we bail out.  See
        // Bug: vswhidbey/542125
        if (IsCurrentTokenPartialKeywordOfPartialMemberOrType() || IsCurrentTokenQueryKeywordInQuery()) {
            auto result = CreateMissingIdentifierToken();
            result = this->AddError(result, ErrorCode::ERR_InvalidExprTerm, {this->CurrentToken()->text});
            return result;
        }

        GreenNode *identifierToken = this->EatToken();

        if (this->IsInAsync() && identifierToken->contextual_kind == SyntaxKind::AwaitKeyword) {
            identifierToken = this->AddError(identifierToken, ErrorCode::ERR_BadAwaitAsIdentifier);
        }

        return identifierToken;
    } else {
        return this->AddError(CreateMissingIdentifierToken(), code);
    }
}

bool LanguageParser::IsCurrentTokenQueryKeywordInQuery() {
    return this->IsInQuery() && this->IsCurrentTokenQueryContextualKeyword();
}

bool LanguageParser::IsCurrentTokenPartialKeywordOfPartialMemberOrType() {
    if (this->CurrentToken()->contextual_kind == SyntaxKind::PartialKeyword) {
        if (this->IsPartialType() || this->IsPartialMember()) {
            return true;
        }
    }

    return false;
}

bool LanguageParser::IsCurrentTokenFieldInKeywordContext() {
    return CurrentToken()->contextual_kind == SyntaxKind::FieldKeyword && IsInFieldKeywordContext() &&
           IsFeatureEnabled(/*MessageID.IDS_FeatureFieldKeyword*/);
}

GreenNode *LanguageParser::ParseTypeParameterList() {
    if (this->CurrentToken()->kind != SyntaxKind::LessThanToken) {
        return nullptr;
    }

    auto saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfTypeParameterList;

    auto skipBadTypeParameterListTokens = [](LanguageParser *self, GreenNode *&open, SyntaxListBuilder list,
                                             SyntaxKind expectedKind, SyntaxKind closeKind) -> PostSkipAction {
        return self->SkipBadSeparatedListTokensWithExpectedKind(
            open, list, [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CommaToken; },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    auto open = this->EatToken(SyntaxKind::LessThanToken);
    GreenNode *parameters = this->ParseCommaSeparatedSyntaxList(
        open, SyntaxKind::GreaterThanToken, [](LanguageParser *self) { return self->IsStartOfTypeParameter(); },
        [](LanguageParser *self) { return self->ParseTypeParameter(); }, skipBadTypeParameterListTokens,
        /*allowTrailingSeparator:*/ false,
        /*requireOneElement:*/ true,
        /*allowSemicolonAsSeparator:*/ false);

    _termState = saveTerm;

    GreenNode *close = this->EatToken(SyntaxKind::GreaterThanToken);
    return SyntaxFactory::TypeParameterList(open, parameters, close);
}

bool LanguageParser::IsStartOfTypeParameter() {
    if (this->IsCurrentTokenWhereOfConstraintClause()) return false;

    // possible attributes.
    // Variance.
    if (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken || this->CurrentToken()->kind == SyntaxKind::InKeyword ||
        this->CurrentToken()->kind == SyntaxKind::OutKeyword)
        return true;

    return IsTrueIdentifier();
}

GreenNode *LanguageParser::ParseTypeParameter() {
    SyntaxList attrs;  // default(SyntaxList<AttributeListSyntax>)
    if (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
        auto saveTerm = _termState;
        _termState = TerminatorState::IsEndOfTypeArgumentList;
        attrs = this->ParseAttributeDeclarations(/*inExpressionContext:*/ false);
        _termState = saveTerm;
    }

    if (this->IsCurrentTokenWhereOfConstraintClause() || this->IsCurrentTokenPartialKeywordOfPartialMemberOrType()) {
        return SyntaxFactory::TypeParameter(attrs, /*varianceKeyword:*/ nullptr,
                                            this->AddError(CreateMissingIdentifierToken(), ErrorCode::ERR_IdentifierExpected));
    }

    GreenNode *varianceKeyword =
        this->CurrentToken()->kind == SyntaxKind::InKeyword || this->CurrentToken()->kind == SyntaxKind::OutKeyword
            ? EatToken()
            : nullptr;
    GreenNode *identifier = this->ParseIdentifierToken();
    return SyntaxFactory::TypeParameter(attrs, varianceKeyword, identifier);
}

// Parses the parts of the names between Dots and ColonColons.
GreenNode *LanguageParser::ParseSimpleName(NameOptions options) {
    auto id = this->ParseIdentifierName();
    if (GetField(id, Field::Identifier)->IsMissing()) {
        return id;
    }

    // You can pass ignore generics if you don't even want the parser to consider generics at all.
    // The name parsing will then stop at the first "<". It doesn't make sense to pass both Generic and IgnoreGeneric.

    GreenNode *name = id;
    if (this->CurrentToken()->kind == SyntaxKind::LessThanToken) {
        ScanTypeArgumentListKind kind;
        {
            DisposableResetPoint _(this, /*resetOnDispose:*/ true);
            kind = this->ScanTypeArgumentList(options);
        }

        if (kind == ScanTypeArgumentListKind::DefiniteTypeArgumentList ||
            (kind == ScanTypeArgumentListKind::PossibleTypeArgumentList && Any(options & NameOptions::InTypeList))) {
            CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::LessThanToken);

            auto types = _pool.AllocateSeparated();
            GreenNode *open;
            GreenNode *close;
            this->ParseTypeArgumentList(open, types, close);
            name = SyntaxFactory::GenericName(GetField(id, Field::Identifier),
                                              SyntaxFactory::TypeArgumentList(open, _pool.ToListAndFree(types), close));
        }
    }

    return name;
}

LanguageParser::ScanTypeArgumentListKind LanguageParser::ScanTypeArgumentList(NameOptions options) {
    if (this->CurrentToken()->kind != SyntaxKind::LessThanToken) {
        return ScanTypeArgumentListKind::NotTypeArgumentList;
    }

    if (!Any(options & NameOptions::InExpression)) {
        return ScanTypeArgumentListKind::DefiniteTypeArgumentList;
    }

    // We're in an expression context, and we have a < token.  This could be a
    // type argument list, or it could just be a relational expression.
    //
    // Scan just the type argument list portion (i.e. the part from < to > ) to
    // see what we think it could be.  This will give us one of three possibilities:
    //
    //      result == ScanTypeFlags.NotType.
    //
    // This is absolutely not a type-argument-list.  Just return that result immediately.
    //
    //      result != ScanTypeFlags.NotType && isDefinitelyTypeArgumentList.
    //
    // This is absolutely a type-argument-list.  Just return that result immediately
    //
    //      result != ScanTypeFlags.NotType && !isDefinitelyTypeArgumentList.
    //
    // This could be a type-argument list, or it could be an expression.  Need to see
    // what came after the last '>' to find out which it is.

    // Scan for a type argument list. If we think it's a type argument list
    // then assume it is unless we see specific tokens following it.
    GreenNode *greaterThanToken;
    bool isDefinitelyTypeArgumentList;
    ScanTypeFlags possibleTypeArgumentFlags = ScanPossibleTypeArgumentList(greaterThanToken, isDefinitelyTypeArgumentList);

    if (possibleTypeArgumentFlags == ScanTypeFlags::NotType) {
        return ScanTypeArgumentListKind::NotTypeArgumentList;
    }

    if (isDefinitelyTypeArgumentList) {
        return ScanTypeArgumentListKind::DefiniteTypeArgumentList;
    }

    // If we did not definitively determine from immediate syntax that it was or
    // was not a type argument list, we must have scanned the entire thing up through
    // the closing greater-than token. In that case we will disambiguate based on the
    // token that follows it.
    CS_ASSERT(greaterThanToken->kind == SyntaxKind::GreaterThanToken);

    switch (this->CurrentToken()->kind) {
        case SyntaxKind::OpenParenToken:
        case SyntaxKind::CloseParenToken:
        case SyntaxKind::CloseBracketToken:
        case SyntaxKind::CloseBraceToken:
        case SyntaxKind::ColonToken:
        case SyntaxKind::SemicolonToken:
        case SyntaxKind::CommaToken:
        case SyntaxKind::DotToken:
        case SyntaxKind::QuestionToken:
        case SyntaxKind::EqualsEqualsToken:
        case SyntaxKind::ExclamationEqualsToken:
        case SyntaxKind::BarToken:
        case SyntaxKind::CaretToken:
            // These tokens are from 7.5.4.2 Grammar Ambiguities
            return ScanTypeArgumentListKind::DefiniteTypeArgumentList;

        case SyntaxKind::AmpersandAmpersandToken:  // e.g. `e is A<B> && e`
        case SyntaxKind::BarBarToken:              // e.g. `e is A<B> || e`
        case SyntaxKind::AmpersandToken:           // e.g. `e is A<B> & e`
        case SyntaxKind::OpenBracketToken:         // e.g. `e is A<B>[]`
        case SyntaxKind::LessThanToken:            // e.g. `e is A<B> < C`
        case SyntaxKind::LessThanEqualsToken:      // e.g. `e is A<B> <= C`
        case SyntaxKind::GreaterThanEqualsToken:   // e.g. `e is A<B> >= C`
        case SyntaxKind::IsKeyword:                // e.g. `e is A<B> is bool`
        case SyntaxKind::AsKeyword:                // e.g. `e is A<B> as bool`
            // These tokens were added to 7.5.4.2 Grammar Ambiguities in C# 7.0
            return ScanTypeArgumentListKind::DefiniteTypeArgumentList;

        case SyntaxKind::OpenBraceToken:  // e.g. `e is A<B> {}`
            // This token was added to 7.5.4.2 Grammar Ambiguities in C# 8.0
            return ScanTypeArgumentListKind::DefiniteTypeArgumentList;

        case SyntaxKind::GreaterThanToken:
            if (Any(options & NameOptions::AfterIs) && this->PeekToken(1)->kind != SyntaxKind::GreaterThanToken) {
                // This token is added to 7.5.4.2 Grammar Ambiguities in C#7 for the special case in which
                // the possible generic is following an `is` keyword, e.g. `e is A<B> > C`.
                // We test one further token ahead because a right-shift operator `>>` looks like a pair of greater-than
                // tokens at this stage, but we don't intend to be handling the right-shift operator.
                // The upshot is that we retain compatibility with the two previous behaviors:
                // `(x is A<B>>C)` is parsed as `(x is A<B>) > C`
                // `A<B>>C` elsewhere is parsed as `A < (B >> C)`
                return ScanTypeArgumentListKind::DefiniteTypeArgumentList;
            }
            // (the `when` failed: the default section)
            return ScanTypeArgumentListKind::PossibleTypeArgumentList;

        case SyntaxKind::IdentifierToken: {
            // C#7: In certain contexts, we treat *identifier* as a disambiguating token. Those
            // contexts are where the sequence of tokens being disambiguated is immediately preceded by one
            // of the keywords is, case, or out, or arises while parsing the first element of a tuple literal
            // (in which case the tokens are preceded by `(` and the identifier is followed by a `,`) or a
            // subsequent element of a tuple literal (in which case the tokens are preceded by `,` and the
            // identifier is followed by a `,` or `)`).
            // In C#8 (or whenever recursive patterns are introduced) we also treat an identifier as a
            // disambiguating token if we're parsing the type of a pattern.
            // Note that we treat query contextual keywords (which appear here as identifiers) as disambiguating tokens as well.
            SyntaxKind peek1 = this->PeekToken(1)->kind;
            if (Any(options & (NameOptions::AfterIs | NameOptions::DefinitePattern | NameOptions::AfterOut)) ||
                (Any(options & NameOptions::AfterTupleComma) &&
                 (peek1 == SyntaxKind::CommaToken || peek1 == SyntaxKind::CloseParenToken)) ||
                (Any(options & NameOptions::FirstElementOfPossibleTupleLiteral) && peek1 == SyntaxKind::CommaToken)) {
                // we allow 'G<T,U> x' as a pattern-matching operation and a declaration expression in a tuple.
                return ScanTypeArgumentListKind::DefiniteTypeArgumentList;
            }

            return ScanTypeArgumentListKind::PossibleTypeArgumentList;
        }

        case SyntaxKind::EndOfFileToken:  // e.g. `e is A<B>`
            // This is useful for parsing expressions in isolation
            return ScanTypeArgumentListKind::DefiniteTypeArgumentList;

        case SyntaxKind::EqualsGreaterThanToken:  // e.g. `e switch { A<B> => 1 }`
            // This token was added to 7.5.4.2 Grammar Ambiguities in C# 9.0
            return ScanTypeArgumentListKind::DefiniteTypeArgumentList;

        default:
            return ScanTypeArgumentListKind::PossibleTypeArgumentList;
    }
}

LanguageParser::ScanTypeFlags LanguageParser::ScanPossibleTypeArgumentList(GreenNode *&greaterThanToken,
                                                                          bool &isDefinitelyTypeArgumentList) {
    isDefinitelyTypeArgumentList = false;
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::LessThanToken);

    // If we have `X<>`, or `X<,>` or `X<,,,,,,>` then none of these are legal expression or types. However,
    // it seems likelier that they are invalid open-types in an expression context, versus expressions
    // missing values (note that we only support this when the open name does have the final `>` token).
    if (IsOpenName()) {
        isDefinitelyTypeArgumentList = true;

        auto start = this->EatToken();
        (void)start;
        while (this->CurrentToken()->kind == SyntaxKind::CommaToken) this->EatToken();
        greaterThanToken = this->EatToken();
        CS_ASSERT(start->kind == SyntaxKind::LessThanToken);
        CS_ASSERT(greaterThanToken->kind == SyntaxKind::GreaterThanToken);

        return ScanTypeFlags::GenericTypeOrMethod;
    }

    ScanTypeFlags result = ScanTypeFlags::GenericTypeOrExpression;
    ScanTypeFlags lastScannedType = ScanTypeFlags::NotType;

    do {
        this->EatToken();

        // Type arguments cannot contain attributes, so if this is an open square, we early out and assume it is not a type argument
        if (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
            greaterThanToken = nullptr;
            return ScanTypeFlags::NotType;
        }

        if (this->CurrentToken()->kind == SyntaxKind::GreaterThanToken) {
            greaterThanToken = EatToken();
            return result;
        }

        // Allow for any chain of errant commas in the generic name.  like `Dictionary<,int>` or
        // `Dictionary<int,,>` We still want to think of these as generics, just with missing type-arguments, vs
        // some invalid tree-expression that we would otherwise form.
        if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
            lastScannedType = ScanTypeFlags::NotType;  // default
            continue;
        }

        {
            GreenNode *ignored;
            lastScannedType = this->ScanType(ignored);
        }
        switch (lastScannedType) {
            case ScanTypeFlags::NotType:
                greaterThanToken = nullptr;
                return ScanTypeFlags::NotType;

            case ScanTypeFlags::MustBeType:
                // We're currently scanning a possible type-argument list.  But we're
                // not sure if this is actually a type argument list, or is maybe some
                // complex relational expression with <'s and >'s.  One thing we can
                // tell though is that if we have a predefined type (like 'int' or 'string')
                // before a comma or > then this is definitely a type argument list. i.e.
                // if you have:
                //
                //      var v = ImmutableDictionary<int,
                //
                // then there's no legal interpretation of this as an expression (since a
                // standalone predefined type is not a valid simple term.  Contrast that
                // with :
                //
                //  var v = ImmutableDictionary<Int32,
                //
                // Here this might actually be a relational expression and the comma is meant
                // to separate out the variable declarator 'v' from the next variable.
                //
                // Note: we check if we got 'MustBeType' which triggers for predefined types,
                // (int, string, etc.), or array types (Goo[], A<T>[][] etc.), or pointer types
                // of things that must be types (int*, void**, etc.).
                isDefinitelyTypeArgumentList = isDefinitelyTypeArgumentList ||
                                               this->CurrentToken()->kind == SyntaxKind::CommaToken ||
                                               this->CurrentToken()->kind == SyntaxKind::GreaterThanToken;
                result = ScanTypeFlags::GenericTypeOrMethod;
                break;

                // case ScanTypeFlags.TupleType:
                // It would be nice if we saw a tuple to state that we definitely had a
                // type argument list.  However, there are cases where this would not be
                // true.  For example:
                //
                // public class C
                // {
                //     public static void Main()
                //     {
                //         XX X = default;
                //         int a = 1, b = 2;
                //         bool z = X < (a, b), w = false;
                //     }
                // }
                //
                // struct XX
                // {
                //     public static bool operator <(XX x, (int a, int b) arg) => true;
                //     public static bool operator >(XX x, (int a, int b) arg) => false;
                // }

            case ScanTypeFlags::NullableType:
                // See above.  If we have `X<Y?,` or `X<Y?>` then this is definitely a type argument list.
                isDefinitelyTypeArgumentList = isDefinitelyTypeArgumentList ||
                                               this->CurrentToken()->kind == SyntaxKind::CommaToken ||
                                               this->CurrentToken()->kind == SyntaxKind::GreaterThanToken;
                if (isDefinitelyTypeArgumentList) {
                    result = ScanTypeFlags::GenericTypeOrMethod;
                }

                // Note: we intentionally fall out without setting 'result'.
                // Seeing a nullable type (not followed by a , or > ) is not enough
                // information for us to determine what this is yet.  i.e. the user may have:
                //
                //      X < Y ? Z : W
                //
                // We'd see a nullable type here, but this is definitely not a type arg list.

                break;

            case ScanTypeFlags::GenericTypeOrExpression:
                // See above.  If we have  X<Y<Z>,  then this would definitely be a type argument list.
                // However, if we have  X<Y<Z>> then this might not be type argument list.  This could just
                // be some sort of expression where we're comparing, and then shifting values.
                if (!isDefinitelyTypeArgumentList) {
                    isDefinitelyTypeArgumentList = this->CurrentToken()->kind == SyntaxKind::CommaToken;
                    result = ScanTypeFlags::GenericTypeOrMethod;
                }
                break;

            case ScanTypeFlags::GenericTypeOrMethod:
                result = ScanTypeFlags::GenericTypeOrMethod;
                break;

            case ScanTypeFlags::NonGenericTypeOrExpression:
                // Explicitly keeping this case in the switch for clarity.  We parsed out another portion of the
                // type argument list that looks like it's a non-generic-type-or-expr (the simplest case just
                // being "X").  That changes nothing here wrt determining what type of entity we have here, so
                // just fall through and see if we're followed by a "," (in which case keep going), or a ">", in
                // which case we're done.
                break;

            default:
                break;
        }
    } while (this->CurrentToken()->kind == SyntaxKind::CommaToken);

    if (this->CurrentToken()->kind != SyntaxKind::GreaterThanToken) {
        // Error recovery after missing > token:

        // In the case of an identifier, we assume that there could be a missing > token
        // For example, we have reached C in X<A, B C
        if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            greaterThanToken = this->EatToken(SyntaxKind::GreaterThanToken);
            return result;
        }

        // As for tuples, we do not expect direct invocation right after the parenthesis
        // EXAMPLE: X<(string, string)(), where we imply a missing > token between )(
        // as the user probably wants to invoke X by X<(string, string)>()
        if (lastScannedType == ScanTypeFlags::TupleType && this->CurrentToken()->kind == SyntaxKind::OpenParenToken) {
            greaterThanToken = this->EatToken(SyntaxKind::GreaterThanToken);
            return result;
        }

        greaterThanToken = nullptr;
        return ScanTypeFlags::NotType;
    }

    greaterThanToken = this->EatToken();

    // If we have `X<Y>)` then this would definitely be a type argument list.
    isDefinitelyTypeArgumentList =
        isDefinitelyTypeArgumentList || this->CurrentToken()->kind == SyntaxKind::CloseParenToken;
    if (isDefinitelyTypeArgumentList) {
        result = ScanTypeFlags::GenericTypeOrMethod;
    }

    return result;
}

// ParseInstantiation: Parses the generic argument/parameter parts of the name.
void LanguageParser::ParseTypeArgumentList(GreenNode *&open, SeparatedSyntaxListBuilder types, GreenNode *&close) {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::LessThanToken);

    auto tokenBreaksTypeArgumentList = [](GreenNode *token) -> bool {
        auto contextualKind = SyntaxFacts::GetContextualKeywordKind(token->value_text);
        switch (contextualKind) {
            // Example: x is IEnumerable<string or IList<int>
            case SyntaxKind::OrKeyword:
            // Example: x is IEnumerable<string and IDisposable
            case SyntaxKind::AndKeyword:
                return true;
            default:
                break;
        }

        switch (token->kind) {
            // Example: Method<string(argument)
            // Note: We would do a bad job handling a tuple argument with a missing comma,
            //       like: Method<string (int x, int y)>
            //       but since we do not look as far as possible to determine whether it is
            //       a tuple type or an argument list, we resort to considering it as an
            //       argument list
            case SyntaxKind::OpenParenToken:

            // Example: IEnumerable<string Method<T>() --- (< in <T>)
            case SyntaxKind::LessThanToken:
            // Example: Method(IEnumerable<string parameter)
            case SyntaxKind::CloseParenToken:
            // Example: IEnumerable<string field;
            case SyntaxKind::SemicolonToken:
            // Example: IEnumerable<string Property { get; set; }
            case SyntaxKind::OpenBraceToken:
            // Example:
            // {
            //     IEnumerable<string field
            // }
            case SyntaxKind::CloseBraceToken:
            // Examples:
            // - IEnumerable<string field = null;
            // - Method(IEnumerable<string parameter = null)
            case SyntaxKind::EqualsToken:
            // Example: IEnumerable<string Property => null;
            case SyntaxKind::EqualsGreaterThanToken:
            // Example: IEnumerable<string this[string key] { get; set; }
            case SyntaxKind::ThisKeyword:
            // Example: static IEnumerable<string operator +(A left, A right);
            case SyntaxKind::OperatorKeyword:
                return true;
            default:
                break;
        }

        return false;
    };

    auto isOpenName = this->IsOpenName();
    open = this->EatToken(SyntaxKind::LessThanToken);
    open = CheckFeatureAvailability(open /*, MessageID.IDS_FeatureGenerics*/);

    if (isOpenName) {
        // NOTE: trivia will be attached to comma, not omitted type argument
        auto omittedTypeArgumentInstance =
            SyntaxFactory::OmittedTypeArgument(SyntaxFactory::Token(SyntaxKind::OmittedTypeArgumentToken));
        types.Add(omittedTypeArgumentInstance);
        while (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
            types.AddSeparator(this->EatToken(SyntaxKind::CommaToken));
            types.Add(omittedTypeArgumentInstance);
        }

        close = this->EatToken(SyntaxKind::GreaterThanToken);

        return;
    }

    // first type
    types.Add(this->ParseTypeArgument());

    // remaining types & commas
    while (true) {
        if (this->CurrentToken()->kind == SyntaxKind::GreaterThanToken) {
            break;
        }

        // We prefer early terminating the argument list over parsing until exhaustion
        // for better error recovery
        if (tokenBreaksTypeArgumentList(this->CurrentToken())) {
            break;
        }

        // We are currently past parsing a type and we encounter an unexpected identifier token
        // followed by tokens that are not part of a type argument list
        // Example: List<(string a, string b) Method() { }
        //                 current token:     ^^^^^^
        if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken && tokenBreaksTypeArgumentList(this->PeekToken(1))) {
            break;
        }

        // This is for the case where we are in a this[] accessor, and the last one of the parameters in the parameter list
        // is missing a > on its type
        // Example: X this[IEnumerable<string parameter] =>
        //                 current token:     ^^^^^^^^^
        if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken &&
            this->PeekToken(1)->kind == SyntaxKind::CloseBracketToken) {
            break;
        }

        if (this->CurrentToken()->kind == SyntaxKind::CommaToken || this->IsPossibleType()) {
            types.AddSeparator(this->EatToken(SyntaxKind::CommaToken));
            types.Add(this->ParseTypeArgument());
        } else if (this->SkipBadTypeArgumentListTokens(types, SyntaxKind::CommaToken) == PostSkipAction::Abort) {
            break;
        }
    }

    close = this->EatToken(SyntaxKind::GreaterThanToken);
}

LanguageParser::PostSkipAction LanguageParser::SkipBadTypeArgumentListTokens(SeparatedSyntaxListBuilder list, SyntaxKind expected) {
    GreenNode *tmp = nullptr;
    CS_ASSERT(list.Count() > 0);
    return this->SkipBadSeparatedListTokensWithExpectedKind(
        tmp, list,
        [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleType(); },
        [](LanguageParser *p, SyntaxKind) { return p->CurrentToken()->kind == SyntaxKind::GreaterThanToken; }, expected);
}

// Parses the individual generic parameter/arguments in a name.
GreenNode *LanguageParser::ParseTypeArgument() {
    SyntaxList attrs;  // default(SyntaxList<AttributeListSyntax>)
    if (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken &&
        this->PeekToken(1)->kind != SyntaxKind::CloseBracketToken) {
        // Here, if we see a "[" that looks like it has something in it, we parse
        // it as an attribute and then later put an error on the whole type if
        // it turns out that attributes are not allowed.
        // TODO: should there be another flag that controls this behavior? we have
        // "allowAttrs" but should there also be a "recognizeAttrs" that we can
        // set to false in an expression context?

        auto saveTerm = _termState;
        _termState = TerminatorState::IsEndOfTypeArgumentList;
        attrs = this->ParseAttributeDeclarations(/*inExpressionContext:*/ false);
        _termState = saveTerm;
    }

    // Recognize the variance syntax, but give an error as it's only appropriate in a type parameter list.
    auto varianceToken =
        this->CurrentToken()->kind == SyntaxKind::InKeyword || this->CurrentToken()->kind == SyntaxKind::OutKeyword
            ? this->AddError(this->EatToken(), ErrorCode::ERR_IllegalVarianceSyntax)
            : nullptr;

    auto result = this->ParseType();

    // Consider the case where someone supplies an invalid type argument
    // Such as Action<0> or Action<static>.  In this case we generate a missing
    // identifier in ParseType, but if we continue as is we'll immediately start to
    // interpret 0 as the start of a new expression when we can tell it's most likely
    // meant to be part of the type list.
    //
    // To solve this we check if the current token is not comma or greater than and
    // the next token is a comma or greater than. If so we assume that the found
    // token is part of this expression and we attempt to recover. This does open
    // the door for cases where we have an  incomplete line to be interpretted as
    // a single expression.  For example:
    //
    // Action< // Incomplete line
    // a>b;
    //
    // However, this only happens when the following expression is of the form a>...
    // or a,... which  means this case should happen less frequently than what we're
    // trying to solve here so we err on the side of better error messages
    // for the majority of cases.

    if (result->IsMissing() && this->CurrentToken()->kind != SyntaxKind::CommaToken &&
        this->CurrentToken()->kind != SyntaxKind::GreaterThanToken &&
        (this->PeekToken(1)->kind == SyntaxKind::CommaToken || this->PeekToken(1)->kind == SyntaxKind::GreaterThanToken)) {
        // Eat the current token and add it as skipped so we recover
        result = AddTrailingSkippedSyntax(result, this->EatToken());
    }

    if (varianceToken != nullptr) {
        result = AddLeadingSkippedSyntax(result, varianceToken);
    }

    if (attrs.Count() > 0) {
        result = AddLeadingSkippedSyntax(result, attrs.Node());
        result = this->AddError(result, ErrorCode::ERR_TypeExpected);
    }

    return result;
}

bool LanguageParser::IsEndOfTypeArgumentList() { return this->CurrentToken()->kind == SyntaxKind::GreaterThanToken; }

bool LanguageParser::IsOpenName() {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::LessThanToken);
    auto n = 1;
    while (this->PeekToken(n)->kind == SyntaxKind::CommaToken) n++;

    return this->PeekToken(n)->kind == SyntaxKind::GreaterThanToken;
}

void LanguageParser::ParseMemberName(GreenNode *&explicitInterfaceOpt, GreenNode *&identifierOrThisOpt,
                                     GreenNode *&typeParameterListOpt, bool isEvent) {
    identifierOrThisOpt = nullptr;
    explicitInterfaceOpt = nullptr;
    typeParameterListOpt = nullptr;

    if (!IsPossibleMemberName()) {
        // No clue what this is.  Just bail.  Our caller will have to
        // move forward and try again.
        return;
    }

    GreenNode *explicitInterfaceName = nullptr;
    GreenNode *separator = nullptr;

    ResetPoint beforeIdentifierPoint{};
    bool beforeIdentifierPointSet = false;

    // try { ... } finally { if (beforeIdentifierPointSet) Release(ref beforeIdentifierPoint); }
    ScopeExit finally([&] {
        if (beforeIdentifierPointSet) {
            Release(beforeIdentifierPoint);
        }
    });

    while (true) {
        // Check if we got 'this'.  If so, then we have an indexer.
        // Note: we parse out type parameters here as well so that
        // we can give a useful error about illegal generic indexers.
        if (this->CurrentToken()->kind == SyntaxKind::ThisKeyword) {
            beforeIdentifierPoint = GetResetPoint();
            beforeIdentifierPointSet = true;
            identifierOrThisOpt = this->EatToken();
            typeParameterListOpt = this->ParseTypeParameterList();
            break;
        }

        // now, scan past the next name.  if it's followed by a dot then
        // it's part of the explicit name we're building up.  Otherwise,
        // it's the name of the member.
        bool isMemberName;
        {
            DisposableResetPoint _(this, /*resetOnDispose:*/ true);
            ScanNamedTypePart();
            isMemberName = !IsDotOrColonColon();
        }

        if (isMemberName) {
            // We're past any explicit interface portion and We've
            // gotten to the member name.
            beforeIdentifierPoint = GetResetPoint();
            beforeIdentifierPointSet = true;

            if (separator != nullptr && separator->kind == SyntaxKind::ColonColonToken) {
                separator = this->AddError(separator, ErrorCode::ERR_AliasQualAsExpression);
                separator = this->ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
            }

            identifierOrThisOpt = this->ParseIdentifierToken();
            typeParameterListOpt = this->ParseTypeParameterList();
            break;
        } else {
            // If we saw a . or :: then we must have something explicit.
            AccumulateExplicitInterfaceName(explicitInterfaceName, separator);
        }
    }

    if (explicitInterfaceName != nullptr) {
        if (separator->kind != SyntaxKind::DotToken) {
            separator = WithAdditionalDiagnostics(
                separator, GetExpectedTokenError(SyntaxKind::DotToken, separator->kind, separator->GetLeadingTriviaWidth(),
                                                 separator->Width()));
            separator = ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
        }

        if (isEvent && this->CurrentToken()->kind != SyntaxKind::OpenBraceToken &&
            this->CurrentToken()->kind != SyntaxKind::SemicolonToken) {
            // CS0071: If you're explicitly implementing an event field, you have to use the accessor form
            //
            // Good:
            //   event EventDelegate Parent.E
            //   {
            //      add { ... }
            //      remove { ... }
            //   }
            //
            // Bad:
            //   event EventDelegate Parent.
            //   E( //(or anything that is not the semicolon
            //
            // To recover: rollback to before the name of the field was parsed (just the part after the last
            // dot), insert a missing identifier for the field name, insert missing accessors, and then treat
            // the event name that's actually there as the beginning of a new member. e.g.
            //
            //   event EventDelegate Parent./*Missing nodes here*/
            //
            //   E(
            //
            // Rationale: The identifier could be the name of a type at the beginning of an existing member
            // declaration (above which someone has started to type an explicit event implementation).
            //
            // In case the dot doesn't follow with an end line or E ends with a semicolon, the error recovery
            // is skipped. In that case the rationale above does not fit very well.

            explicitInterfaceOpt = SyntaxFactory::ExplicitInterfaceSpecifier(
                explicitInterfaceName, AddError(separator, ErrorCode::ERR_ExplicitEventFieldImpl));

            if (SyntaxList(separator->GetTrailingTrivia()).Any(SyntaxKind::EndOfLineTrivia)) {
                CS_ASSERT(beforeIdentifierPointSet);
                Reset(beforeIdentifierPoint);
                // clear fields that were populated after the reset point
                identifierOrThisOpt = nullptr;
                typeParameterListOpt = nullptr;
            }
        } else {
            explicitInterfaceOpt = SyntaxFactory::ExplicitInterfaceSpecifier(explicitInterfaceName, separator);
        }
    }
}

void LanguageParser::AccumulateExplicitInterfaceName(GreenNode *&explicitInterfaceName, GreenNode *&separator) {
    // first parse the upcoming name portion.

    auto saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfNameInExplicitInterface;

    if (explicitInterfaceName == nullptr) {
        // If this is the first time, then just get the next simple
        // name and store it as the explicit interface name.
        explicitInterfaceName = this->ParseSimpleName(NameOptions::InTypeList);

        // Now, get the next separator.
        separator = this->CurrentToken()->kind == SyntaxKind::ColonColonToken
                        ? this->EatToken()  // fine after the first identifier
                        : this->EatToken(SyntaxKind::DotToken);
    } else {
        // Parse out the next part and combine it with the
        // current explicit name to form the new explicit name.
        auto tmp = this->ParseQualifiedNameRight(NameOptions::InTypeList, explicitInterfaceName, separator);
        CS_ASSERT(tmp != explicitInterfaceName);
        explicitInterfaceName = tmp;

        // Now, get the next separator.
        if (this->CurrentToken()->kind == SyntaxKind::ColonColonToken) {
            separator = this->EatToken();
            separator = this->AddError(separator, ErrorCode::ERR_UnexpectedAliasedName);
            separator = this->ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
        } else {
            separator = this->EatToken(SyntaxKind::DotToken);
        }
    }

    _termState = saveTerm;
}

/// This is an adjusted version of ParseMemberName.
/// When it returns true, it stops at operator keyword (IsOperatorKeyword).
/// When it returns false, it does not advance in the token stream.
bool LanguageParser::IsOperatorStart(GreenNode *&explicitInterfaceOpt, bool advanceParser) {
    explicitInterfaceOpt = nullptr;

    if (IsOperatorKeyword()) {
        return true;
    }

    if (this->CurrentToken()->kind != SyntaxKind::IdentifierToken) {
        return false;
    }

    GreenNode *explicitInterfaceName = nullptr;
    GreenNode *separator = nullptr;

    DisposableResetPoint beforeIdentifierPoint(this, /*resetOnDispose:*/ false);

    while (true) {
        // now, scan past the next name.  if it's followed by a dot then
        // it's part of the explicit name we're building up.  Otherwise,
        // it should be an operator token
        bool isPartOfInterfaceName;
        {
            DisposableResetPoint _(this, /*resetOnDispose:*/ true);
            if (IsOperatorKeyword()) {
                isPartOfInterfaceName = false;
            } else {
                ScanNamedTypePart();

                // If we have part of the interface name, but no dot before the operator token, then
                // for the purpose of error recovery, treat this as an operator start with a
                // missing dot token.
                isPartOfInterfaceName = IsDotOrColonColon() || IsOperatorKeyword();
            }
        }

        if (!isPartOfInterfaceName) {
            // We're past any explicit interface portion
            if (separator != nullptr && separator->kind == SyntaxKind::ColonColonToken) {
                separator = this->AddError(separator, ErrorCode::ERR_AliasQualAsExpression);
                separator = this->ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
            }

            break;
        } else {
            // If we saw a . or :: then we must have something explicit.
            AccumulateExplicitInterfaceName(explicitInterfaceName, separator);
        }
    }

    if (!IsOperatorKeyword() || explicitInterfaceName == nullptr) {
        beforeIdentifierPoint.Reset();
        return false;
    }

    if (!advanceParser) {
        beforeIdentifierPoint.Reset();
        return true;
    }

    if (separator->kind != SyntaxKind::DotToken) {
        separator = WithAdditionalDiagnostics(
            separator,
            GetExpectedTokenError(SyntaxKind::DotToken, separator->kind, separator->GetLeadingTriviaWidth(), separator->Width()));
        separator = ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
    }

    explicitInterfaceOpt = SyntaxFactory::ExplicitInterfaceSpecifier(explicitInterfaceName, separator);
    return true;
}

GreenNode *LanguageParser::ParseAliasQualifiedName(NameOptions allowedParts) {
    auto name = this->ParseSimpleName(allowedParts);
    if (this->CurrentToken()->kind == SyntaxKind::ColonColonToken) {
        auto separator = this->EatToken();
        return ParseQualifiedNameRight(allowedParts, name, separator);
    }
    return name;
}

GreenNode *LanguageParser::ParseQualifiedName(NameOptions options) {
    GreenNode *name = this->ParseAliasQualifiedName(options);

    // Handle .. tokens for error recovery purposes.
    while (IsDotOrColonColon()) {
        if (this->PeekToken(1)->kind == SyntaxKind::ThisKeyword) {
            break;
        }

        auto separator = this->EatToken();
        name = ParseQualifiedNameRight(options, name, separator);
    }

    return name;
}

GreenNode *LanguageParser::ParseQualifiedNameRight(NameOptions options, GreenNode *left, GreenNode *separator) {
    CS_ASSERT(separator->kind == SyntaxKind::DotToken || separator->kind == SyntaxKind::ColonColonToken);
    auto right = this->ParseSimpleName(options);

    switch (separator->kind) {
        case SyntaxKind::DotToken:
            return SyntaxFactory::QualifiedName(left, separator, right);

        case SyntaxKind::ColonColonToken: {
            if (left->kind != SyntaxKind::IdentifierName) {
                separator = this->AddError(separator, ErrorCode::ERR_UnexpectedAliasedName);
            }

            // If the left hand side is not an identifier name then the user has done
            // something like Goo.Bar::Blah. We've already made an error node for the
            // ::, so just pretend that they typed Goo.Bar.Blah and continue on.

            if (left->kind != SyntaxKind::IdentifierName) {
                separator = this->ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
                return SyntaxFactory::QualifiedName(left, separator, right);
            } else {
                GreenNode *identifierLeft = left;
                if (GetField(identifierLeft, Field::Identifier)->contextual_kind == SyntaxKind::GlobalKeyword) {
                    identifierLeft = SyntaxFactory::IdentifierName(ConvertToKeyword(GetField(identifierLeft, Field::Identifier)));
                }

                // If the name on the right had errors or warnings then we need to preserve
                // them in the tree.
                // (WithAdditionalDiagnostics(node, left.GetDiagnostics()): left's own diagnostics, appended in order.)
                GreenNode *result = SyntaxFactory::AliasQualifiedName(identifierLeft, separator, right);
                for (const SyntaxDiagnosticInfo *d = DiagsBegin(left); d != DiagsEnd(left); ++d) {
                    result = WithAdditionalDiagnostics(result, *d);
                }
                return result;
            }
        }

        default:
            CS_ASSERT(false);  // throw ExceptionUtilities.Unreachable();
            return nullptr;
    }
}

GreenNode *LanguageParser::ConvertToMissingWithTrailingTrivia(GreenNode *token, SyntaxKind expectedKind) {
    auto newToken = SyntaxFactory::MissingToken(expectedKind);
    newToken = AddTrailingSkippedSyntax(newToken, token);
    return newToken;
}

bool LanguageParser::IsPossibleType() {
    auto tk = this->CurrentToken()->kind;
    return IsPredefinedType(tk) || this->IsTrueIdentifier();
}

LanguageParser::ScanTypeFlags LanguageParser::ScanType(bool forPattern) {
    GreenNode *ignored;
    return ScanType(ignored, forPattern);
}

LanguageParser::ScanTypeFlags LanguageParser::ScanType(GreenNode *&lastTokenOfType, bool forPattern) {
    return ScanType(forPattern ? ParseTypeMode::DefinitePattern : ParseTypeMode::Normal, lastTokenOfType);
}

void LanguageParser::ScanNamedTypePart() {
    GreenNode *ignored;
    ScanNamedTypePart(ignored);
}

LanguageParser::ScanTypeFlags LanguageParser::ScanNamedTypePart(GreenNode *&lastTokenOfType) {
    if (this->CurrentToken()->kind != SyntaxKind::IdentifierToken || !this->IsTrueIdentifier()) {
        lastTokenOfType = nullptr;
        return ScanTypeFlags::NotType;
    }

    lastTokenOfType = this->EatToken();
    if (this->CurrentToken()->kind == SyntaxKind::LessThanToken) {
        bool ignored;
        return this->ScanPossibleTypeArgumentList(lastTokenOfType, ignored);
    } else {
        return ScanTypeFlags::NonGenericTypeOrExpression;
    }
}

LanguageParser::ScanTypeFlags LanguageParser::ScanType(ParseTypeMode mode, GreenNode *&lastTokenOfType) {
    CS_ASSERT(mode != ParseTypeMode::NewExpression);
    ScanTypeFlags result;

    if (this->CurrentToken()->kind == SyntaxKind::RefKeyword) {
        // in a ref local or ref return, we treat "ref" and "ref readonly" as part of the type
        this->EatToken();

        if (this->CurrentToken()->kind == SyntaxKind::ReadOnlyKeyword) {
            this->EatToken();
        }
    }

    // Handle :: as well for error case of an alias used without a preceding identifier.
    if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken ||
        this->CurrentToken()->kind == SyntaxKind::ColonColonToken) {
        bool isAlias;
        if (this->CurrentToken()->kind == SyntaxKind::ColonColonToken) {
            result = ScanTypeFlags::NonGenericTypeOrExpression;

            // Definitely seems like an alias if we're starting with a ::
            isAlias = true;

            // We set this to null to appease the flow checker.  It will always be the case that this will be
            // set to an appropriate value inside the `for` loop below.  We'll consume the :: there and then
            // call ScanNamedTypePart which will always set this to a valid value.
            lastTokenOfType = nullptr;
        } else {
            CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::IdentifierToken);

            // We're an alias if we start with an: id::
            isAlias = this->PeekToken(1)->kind == SyntaxKind::ColonColonToken;

            result = this->ScanNamedTypePart(lastTokenOfType);
            if (result == ScanTypeFlags::NotType) {
                return ScanTypeFlags::NotType;
            }

            CS_ASSERT(result == ScanTypeFlags::GenericTypeOrExpression || result == ScanTypeFlags::GenericTypeOrMethod ||
                      result == ScanTypeFlags::NonGenericTypeOrExpression);
        }

        // Scan a name
        for (bool firstLoop = true; IsDotOrColonColon(); firstLoop = false) {
            // If we consume any more dots or colons, don't consider us an alias anymore.  For dots, we now have
            // x::y.z (which is now back to a normal expr/type, not an alias), and for colons that means we have
            // x::y::z or x.y::z both of which are effectively gibberish.
            if (!firstLoop) {
                isAlias = false;
            }

            this->EatToken();
            result = this->ScanNamedTypePart(lastTokenOfType);
            if (result == ScanTypeFlags::NotType) {
                return ScanTypeFlags::NotType;
            }

            CS_ASSERT(result == ScanTypeFlags::GenericTypeOrExpression || result == ScanTypeFlags::GenericTypeOrMethod ||
                      result == ScanTypeFlags::NonGenericTypeOrExpression);
        }

        if (isAlias) {
            result = ScanTypeFlags::AliasQualifiedName;
        }
    } else if (IsPredefinedType(this->CurrentToken()->kind)) {
        // Simple type...
        lastTokenOfType = this->EatToken();
        result = ScanTypeFlags::MustBeType;
    } else if (this->CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        lastTokenOfType = this->EatToken();

        result = this->ScanTupleType(lastTokenOfType);
        if (result == ScanTypeFlags::NotType ||
            (mode == ParseTypeMode::DefinitePattern && this->CurrentToken()->kind != SyntaxKind::OpenBracketToken)) {
            // A tuple type can appear in a pattern only if it is the element type of an array type.
            return ScanTypeFlags::NotType;
        }
    } else if (IsFunctionPointerStart()) {
        result = ScanFunctionPointerType(lastTokenOfType);
    } else {
        // Can't be a type!
        lastTokenOfType = nullptr;
        return ScanTypeFlags::NotType;
    }

    int lastTokenPosition = -1;
    while (IsMakingProgress(lastTokenPosition)) {
        SyntaxKind kind = this->CurrentToken()->kind;
        if (kind == SyntaxKind::QuestionToken && lastTokenOfType->kind != SyntaxKind::QuestionToken  // don't allow `Type??`
            && lastTokenOfType->kind != SyntaxKind::AsteriskToken) {                                  // don't allow `Type*?`
            lastTokenOfType = this->EatToken();
            result = ScanTypeFlags::NullableType;
        } else if (kind == SyntaxKind::AsteriskToken) {
            // Check for pointer type(s)
            switch (mode) {
                case ParseTypeMode::FirstElementOfPossibleTupleLiteral:
                case ParseTypeMode::AfterTupleComma:
                    // We are parsing the type for a declaration expression in a tuple, which does
                    // not permit pointer types except as an element type of an array type.
                    // In that context a `*` is parsed as a multiplication.
                    if (PointerTypeModsFollowedByRankAndDimensionSpecifier()) {
                        goto asterisk_default;
                    }
                    goto done;
                case ParseTypeMode::DefinitePattern:
                    // pointer type syntax is not supported in patterns.
                    goto done;
                default:
                asterisk_default:
                    lastTokenOfType = this->EatToken();
                    if (result == ScanTypeFlags::GenericTypeOrExpression ||
                        result == ScanTypeFlags::NonGenericTypeOrExpression) {
                        result = ScanTypeFlags::PointerOrMultiplication;
                    } else if (result == ScanTypeFlags::GenericTypeOrMethod) {
                        result = ScanTypeFlags::MustBeType;
                    }
                    break;
            }
        } else if (kind == SyntaxKind::OpenBracketToken) {
            // Check for array types.
            this->EatToken();
            while (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
                this->EatToken();
            }

            if (this->CurrentToken()->kind != SyntaxKind::CloseBracketToken) {
                lastTokenOfType = nullptr;
                return ScanTypeFlags::NotType;
            }

            lastTokenOfType = this->EatToken();
            result = ScanTypeFlags::MustBeType;
        } else {
            goto done;
        }
    }

done:
    return result;
}

/// Returns TupleType when a possible tuple type is found.
/// Note that this is not MustBeType, so that the caller can consider deconstruction syntaxes.
/// The caller is expected to have consumed the opening paren.
LanguageParser::ScanTypeFlags LanguageParser::ScanTupleType(GreenNode *&lastTokenOfType) {
    auto tupleElementType = ScanType(lastTokenOfType);
    if (tupleElementType != ScanTypeFlags::NotType) {
        if (IsTrueIdentifier()) {
            lastTokenOfType = this->EatToken();
        }

        if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
            do {
                lastTokenOfType = this->EatToken();
                tupleElementType = ScanType(lastTokenOfType);

                if (tupleElementType == ScanTypeFlags::NotType) {
                    lastTokenOfType = this->EatToken();
                    return ScanTypeFlags::NotType;
                }

                if (IsTrueIdentifier()) {
                    lastTokenOfType = this->EatToken();
                }
            } while (this->CurrentToken()->kind == SyntaxKind::CommaToken);

            if (this->CurrentToken()->kind == SyntaxKind::CloseParenToken) {
                lastTokenOfType = this->EatToken();
                return ScanTypeFlags::TupleType;
            }
        }
    }

    // Can't be a type!
    lastTokenOfType = nullptr;
    return ScanTypeFlags::NotType;
}

LanguageParser::ScanTypeFlags LanguageParser::ScanFunctionPointerType(GreenNode *&lastTokenOfType) {
    CS_ASSERT(IsFunctionPointerStart());

    auto skipBadFunctionPointerTokens = [this]() -> PostSkipAction {
        GreenNode *ignoredTrailingTrivia = nullptr;
        return SkipBadTokensWithExpectedKind(
            /*isNotExpectedFunction:*/ [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CommaToken; },
            /*abortFunction:*/ [](LanguageParser *p, SyntaxKind) { return p->IsTerminator(); },
            /*expected:*/ SyntaxKind::CommaToken,
            /*closeKind:*/ SyntaxKind::None,
            /*trailingTrivia:*/ ignoredTrailingTrivia);
    };

    (void)EatToken(SyntaxKind::DelegateKeyword);
    lastTokenOfType = EatToken(SyntaxKind::AsteriskToken);

    TerminatorState saveTerm;

    if (CurrentToken()->kind == SyntaxKind::IdentifierToken) {
        auto peek1 = PeekToken(1);
        if (CurrentToken()->contextual_kind == SyntaxKind::ManagedKeyword ||
            CurrentToken()->contextual_kind == SyntaxKind::UnmanagedKeyword ||
            IsPossibleFunctionPointerParameterListStart(peek1) || peek1->kind == SyntaxKind::OpenBracketToken) {
            lastTokenOfType = EatToken();
        } else {
            // Whatever is next, it's probably not part of the type. We know that delegate* must be
            // a function pointer start, however, so say the asterisk is the last element and bail
            return ScanTypeFlags::MustBeType;
        }

        if (CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
            lastTokenOfType = EatToken(SyntaxKind::OpenBracketToken);
            saveTerm = _termState;
            _termState |= TerminatorState::IsEndOfFunctionPointerCallingConvention;

            {
                ScopeExit finally([&] { _termState = saveTerm; });
                while (true) {
                    GreenNode *identifier = TryEatToken(SyntaxKind::IdentifierToken);
                    lastTokenOfType = identifier != nullptr ? identifier : lastTokenOfType;

                    if (skipBadFunctionPointerTokens() == PostSkipAction::Abort) {
                        break;
                    }

                    CS_ASSERT(CurrentToken()->kind == SyntaxKind::CommaToken);
                    lastTokenOfType = EatToken();
                }

                GreenNode *closeBracket = TryEatToken(SyntaxKind::CloseBracketToken);
                lastTokenOfType = closeBracket != nullptr ? closeBracket : lastTokenOfType;
            }
        }
    }

    if (!IsPossibleFunctionPointerParameterListStart(CurrentToken())) {
        // Even though this function pointer type is incomplete, we know that it
        // must be the start of a type, as there is no other possible interpretation
        // of delegate*. By always treating it as a type, we ensure that any disambiguation
        // done in later parsing treats this as a type, which will produce better
        // errors at later stages.
        return ScanTypeFlags::MustBeType;
    }

    auto validStartingToken = EatToken()->kind == SyntaxKind::LessThanToken;

    saveTerm = _termState;
    _termState |= validStartingToken ? TerminatorState::IsEndOfFunctionPointerParameterList
                                     : TerminatorState::IsEndOfFunctionPointerParameterListErrored;
    auto ignoredModifiers = _pool.Allocate();

    {
        ScopeExit finally([&] {
            _termState = saveTerm;
            _pool.Free(ignoredModifiers);
        });
        do {
            ParseParameterModifiers(ignoredModifiers, /*isFunctionPointerParameter:*/ true, /*isLambdaParameter:*/ false);
            ignoredModifiers.Clear();

            {
                GreenNode *ignored;
                (void)ScanType(ignored);
            }

            if (skipBadFunctionPointerTokens() == PostSkipAction::Abort) {
                break;
            }

            (void)EatToken(SyntaxKind::CommaToken);
        } while (true);
    }

    if (!validStartingToken && CurrentToken()->kind == SyntaxKind::CloseParenToken) {
        lastTokenOfType = EatTokenAsKind(SyntaxKind::GreaterThanToken);
    } else {
        lastTokenOfType = EatToken(SyntaxKind::GreaterThanToken);
    }

    return ScanTypeFlags::MustBeType;
}

bool LanguageParser::IsPredefinedType(SyntaxKind keyword) { return SyntaxFacts::IsPredefinedType(keyword); }

GreenNode *LanguageParser::ParseTypeName() { return ParseType(); }

GreenNode *LanguageParser::ParseTypeOrVoid() {
    if (this->CurrentToken()->kind == SyntaxKind::VoidKeyword && this->PeekToken(1)->kind != SyntaxKind::AsteriskToken) {
        // Must be 'void' type, so create such a type node and return it.
        return SyntaxFactory::PredefinedType(this->EatToken());
    }

    return this->ParseType();
}

GreenNode *LanguageParser::ParseType(ParseTypeMode mode) {
    if (this->CurrentToken()->kind == SyntaxKind::RefKeyword) {
        GreenNode *refKeyword = this->EatToken();
        GreenNode *readOnlyKeyword =
            this->CurrentToken()->kind == SyntaxKind::ReadOnlyKeyword ? this->EatToken() : nullptr;
        GreenNode *type = ParseTypeCore(ParseTypeMode::AfterRef);
        return SyntaxFactory::RefType(refKeyword, readOnlyKeyword, type);
    }

    return ParseTypeCore(mode);
}

GreenNode *LanguageParser::ParseTypeCore(ParseTypeMode mode) {
    NameOptions nameOptions;
    switch (mode) {
        case ParseTypeMode::AfterIs:
            nameOptions = NameOptions::InExpression | NameOptions::AfterIs | NameOptions::PossiblePattern;
            break;
        case ParseTypeMode::DefinitePattern:
            nameOptions = NameOptions::InExpression | NameOptions::DefinitePattern | NameOptions::PossiblePattern;
            break;
        case ParseTypeMode::AfterOut:
            nameOptions = NameOptions::InExpression | NameOptions::AfterOut;
            break;
        case ParseTypeMode::AfterTupleComma:
            nameOptions = NameOptions::InExpression | NameOptions::AfterTupleComma;
            break;
        case ParseTypeMode::FirstElementOfPossibleTupleLiteral:
            nameOptions = NameOptions::InExpression | NameOptions::FirstElementOfPossibleTupleLiteral;
            break;
        case ParseTypeMode::NewExpression:
        case ParseTypeMode::AsExpression:
        case ParseTypeMode::Normal:
        case ParseTypeMode::Parameter:
        case ParseTypeMode::AfterRef:
            nameOptions = NameOptions::None;
            break;
        default:
            CS_ASSERT(false);  // throw ExceptionUtilities.UnexpectedValue(mode);
            nameOptions = NameOptions::None;
            break;
    }

    auto type = this->ParseUnderlyingType(mode, /*options:*/ nameOptions);
    CS_ASSERT(type != nullptr);

    int lastTokenPosition = -1;
    while (IsMakingProgress(lastTokenPosition)) {
        switch (this->CurrentToken()->kind) {
            case SyntaxKind::QuestionToken: {
                auto question = TryEatNullableQualifierIfApplicable(type, mode);
                if (question != nullptr) {
                    type = SyntaxFactory::NullableType(type, question);
                    continue;
                }

                // token not consumed
                break;
            }
            case SyntaxKind::AsteriskToken:
                switch (mode) {
                    case ParseTypeMode::AfterIs:
                    case ParseTypeMode::DefinitePattern:
                    case ParseTypeMode::AfterTupleComma:
                    case ParseTypeMode::FirstElementOfPossibleTupleLiteral:
                        // these contexts do not permit a pointer type except as an element type of an array.
                        if (PointerTypeModsFollowedByRankAndDimensionSpecifier()) {
                            type = this->ParsePointerTypeMods(type);
                            continue;
                        }
                        break;
                    case ParseTypeMode::Normal:
                    case ParseTypeMode::Parameter:
                    case ParseTypeMode::AfterOut:
                    case ParseTypeMode::AfterRef:
                    case ParseTypeMode::AsExpression:
                    case ParseTypeMode::NewExpression:
                        type = this->ParsePointerTypeMods(type);
                        continue;
                }

                // token not consumed
                break;
            case SyntaxKind::OpenBracketToken:
                // Now check for arrays.
                {
                    auto ranks = _pool.Allocate();
                    do {
                        bool ignored;
                        ranks.Add(this->ParseArrayRankSpecifier(ignored));
                    } while (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken);

                    type = SyntaxFactory::ArrayType(type, _pool.ToListAndFree(ranks));
                    continue;
                }
            default:
                // token not consumed
                break;
        }

        // token not consumed
        break;
    }

    CS_ASSERT(type != nullptr);
    return type;
}

GreenNode *LanguageParser::TryEatNullableQualifierIfApplicable(GreenNode *typeParsedSoFar, ParseTypeMode mode) {
    CS_ASSERT(this->CurrentToken()->kind == SyntaxKind::QuestionToken);

    // These are the fast tests for (in)applicability. More expensive tests are follow.
    //
    // If we already have `x?` or `x*` then do not parse out a nullable type if we see `x??` or `x*?`.  These
    // are never legal as types in the language, so we can fast bail out.
    if (typeParsedSoFar->kind == SyntaxKind::NullableType || typeParsedSoFar->kind == SyntaxKind::PointerType)
        return nullptr;

    auto canFollowNullableType = [&]() -> bool {
        if (mode == ParseTypeMode::AfterIs && this->CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
            // T?[
            //
            // This could be a array of nullable types (e.g. `is T?[]` or `is T?[,]`) or it's a
            // conditional with a collection expression or lambda (e.g. `is T ? [...] :` or `is T ? [Attr]() => ...`)
            //
            // Note: `is T?[]` could be the start of either.  So we have to look to see if we have a
            // `:` to know which case we're in.

            switch (this->PeekToken(1)->kind) {
                // `is T?[,]`.  Definitely an array of nullable type.
                case SyntaxKind::CommaToken:
                    return true;

                // `is T?[]`.  Could be an array of a nullable type, or a conditional.  Have to
                // see if it is followed by `:` to find out.  If there is a colon, it's a
                // conditional.
                case SyntaxKind::CloseBracketToken: {
                    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

                    // Consume the expression after the `?`.
                    auto whenTrue = this->ParsePossibleRefExpression();
                    (void)whenTrue;

                    // Now see if we have a ':' following.  If so, this is a conditional.  If not, it's a nullable type.
                    return this->CurrentToken()->kind != SyntaxKind::ColonToken;
                }

                // `is T ? [...`.  Not an array.  This is a conditional with a collection expr
                // or attributed lambda.
                default:
                    return false;
            }
        }

        switch (mode) {
            case ParseTypeMode::AfterIs:
            case ParseTypeMode::DefinitePattern:
            case ParseTypeMode::AsExpression: {
                // We are currently after `?` token after a nullable type pattern and need to decide how to
                // parse what we see next.  In the case of an identifier (e.g. `x ? a` there are two ways we can
                // see things
                //
                // 1. As a start of conditional expression, e.g. `var a = obj is string ? a : b`
                // 2. As a designation of a nullable-typed pattern, e.g. `if (obj is string? str)`
                //
                // Since nullable types (no matter reference or value types) are not valid in patterns by
                // default we are biased towards the first option and consider case 2 only for error recovery
                // purposes (if we parse here as nullable type pattern an error will be reported during
                // binding). This condition checks for simple cases, where we better use option 2 and parse a
                // nullable-typed pattern
                if (IsTrueIdentifier(this->CurrentToken())) {
                    // 1. `async` can start a simple lambda in a conditional expression
                    // (e.g. `x is Y ? async a => ...`). The correct behavior is to treat `async` as a keyword
                    // 2. In a non-async method, `await` is a simple identifier.  However, if we see `x ? await`
                    // it's almost certainly the start of an `await expression` in a conditional expression
                    // (e.g. `x is Y ? await ...`), not a nullable type pattern (since users would not use
                    // 'await' as the name of a variable).  So just treat this as a conditional expression.
                    // 3. `from` most likely starts a linq query: `x is Y ? from item in collection select item : ...`
                    SyntaxKind ck = this->CurrentToken()->contextual_kind;
                    if (ck == SyntaxKind::AsyncKeyword || ck == SyntaxKind::AwaitKeyword || ck == SyntaxKind::FromKeyword)
                        return false;

                    auto nextToken = PeekToken(1);

                    // Cases like `x is Y ? someRecord with { } : ...`
                    if (nextToken->contextual_kind == SyntaxKind::WithKeyword) return false;

                    auto nextTokenKind = nextToken->kind;

                    // These token either 100% end a pattern or start a new one:

                    // A literal token starts a new pattern. Can occur in list pattern with missing separation
                    // `,`.  For example, in `x is [int[]? arr 5]` we'd prefer to parse this as a missing `,`
                    // after `arr`
                    if (SyntaxFacts::IsLiteral(nextTokenKind)) return true;

                    // A predefined type is basically the same case: `x is [string[]? slice char ch]`. We'd
                    // prefer to parse this as a missing `,` after `slice`.
                    if (SyntaxFacts::IsPredefinedType(nextTokenKind)) return true;

                    // `)`, `]` and `}` obviously end a pattern.  For example:
                    // `if (x is int? i)`, `indexable[x is string? s]`, `x is { Prop: Type? typeVar }`
                    if (nextTokenKind == SyntaxKind::CloseParenToken || nextTokenKind == SyntaxKind::CloseBracketToken ||
                        nextTokenKind == SyntaxKind::CloseBraceToken)
                        return true;

                    // `{` starts a new pattern.  For example: `x is A? { ...`. Note, that `[` and `(` are not
                    // in the list because they can start an invocation/indexer
                    if (nextTokenKind == SyntaxKind::OpenBraceToken) return true;

                    // `,` ends a pattern in list/property pattern.  For example `x is { Prop1: Type1? type, Prop2: Type2 }` or
                    // `x is [Type1? type, ...]`
                    if (nextTokenKind == SyntaxKind::CommaToken) return true;

                    // `;` ends a pattern if it finishes an expression statement: var y = x is bool? b;
                    if (nextTokenKind == SyntaxKind::SemicolonToken) return true;

                    // EndOfFileToken is obviously the end of parsing. We are better parsing a pattern rather
                    // than an unfinished conditional expression
                    if (nextTokenKind == SyntaxKind::EndOfFileToken) return true;

                    return false;
                }

                // If nothing from above worked permit the nullable qualifier if it is followed by a token that
                // could not start an expression. If we have `T?[]` we do want to treat that as an array of
                // nullables (following existing parsing), not a conditional that returns a list.
                if (this->CurrentToken()->kind == SyntaxKind::OpenBracketToken) return true;

                return !CanStartExpression();
            }
            case ParseTypeMode::NewExpression:
                // A nullable qualifier is permitted as part of the type in a `new` expression. e.g. `new
                // int?()` is allowed.  It creates a null value of type `Nullable<int>`. Similarly `new int? {}`
                // is allowed.
                return this->CurrentToken()->kind == SyntaxKind::OpenParenToken ||    // ctor parameters
                       this->CurrentToken()->kind == SyntaxKind::OpenBracketToken ||  // array type
                       this->CurrentToken()->kind == SyntaxKind::OpenBraceToken;      // object initializer
            default:
                return true;
        }
    };

    DisposableResetPoint outerResetPoint(this, /*resetOnDispose:*/ false);

    auto questionToken = this->EatToken();
    if (!canFollowNullableType()) {
        // Restore current token index
        outerResetPoint.Reset();
        return nullptr;
    }

    return questionToken;
}

bool LanguageParser::PointerTypeModsFollowedByRankAndDimensionSpecifier() {
    // Are pointer specifiers (if any) followed by an array specifier?
    for (int i = 0;; i++) {
        switch (this->PeekToken(i)->kind) {
            case SyntaxKind::AsteriskToken:
                continue;
            case SyntaxKind::OpenBracketToken:
                return true;
            default:
                return false;
        }
    }
}

GreenNode *LanguageParser::ParseArrayRankSpecifier(bool &sawNonOmittedSize) {
    sawNonOmittedSize = false;
    bool sawOmittedSize = false;
    auto open = this->EatToken(SyntaxKind::OpenBracketToken);
    auto list = _pool.AllocateSeparated();

    auto omittedArraySizeExpressionInstance =
        SyntaxFactory::OmittedArraySizeExpression(SyntaxFactory::Token(SyntaxKind::OmittedArraySizeExpressionToken));
    int lastTokenPosition = -1;
    while (IsMakingProgress(lastTokenPosition) && this->CurrentToken()->kind != SyntaxKind::CloseBracketToken) {
        if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
            // NOTE: trivia will be attached to comma, not omitted array size
            sawOmittedSize = true;
            list.Add(omittedArraySizeExpressionInstance);
            list.AddSeparator(this->EatToken());
        } else if (this->IsPossibleExpression()) {
            auto size = this->ParseExpressionCore();
            sawNonOmittedSize = true;
            list.Add(size);

            if (this->CurrentToken()->kind != SyntaxKind::CloseBracketToken) {
                list.AddSeparator(this->EatToken(SyntaxKind::CommaToken));
            }
        } else if (this->SkipBadArrayRankSpecifierTokens(open, list, SyntaxKind::CommaToken) == PostSkipAction::Abort) {
            break;
        }
    }

    // Don't end on a comma.
    // If the omitted size would be the only element, then skip it unless sizes were expected.
    if (((list.Count() & 1) == 0)) {
        sawOmittedSize = true;
        list.Add(omittedArraySizeExpressionInstance);
    }

    // Never mix omitted and non-omitted array sizes.  If there were non-omitted array sizes,
    // then convert all of the omitted array sizes to missing identifiers.
    if (sawOmittedSize && sawNonOmittedSize) {
        for (int i = 0; i < list.Count(); i++) {
            if (list[i]->kind == SyntaxKind::OmittedArraySizeExpression)
                list[i] = this->AddError(this->CreateMissingIdentifierName(), /*offset:*/ 0, list[i]->Width(),
                                         ErrorCode::ERR_ValueExpected);
        }
    }

    GreenNode *sizes = _pool.ToListAndFree(list);
    GreenNode *close = this->EatToken(SyntaxKind::CloseBracketToken);
    return SyntaxFactory::ArrayRankSpecifier(open, sizes, close);
}

GreenNode *LanguageParser::ParseTupleType() {
    auto open = this->EatToken(SyntaxKind::OpenParenToken);
    auto list = _pool.AllocateSeparated();

    if (this->CurrentToken()->kind != SyntaxKind::CloseParenToken) {
        list.Add(ParseTupleElement());

        while (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
            list.AddSeparator(this->EatToken(SyntaxKind::CommaToken));
            list.Add(ParseTupleElement());
        }
    }

    if (list.Count() < 2) {
        if (list.Count() < 1) {
            list.Add(SyntaxFactory::TupleElement(this->CreateMissingIdentifierName(), /*identifier:*/ nullptr));
        }

        list.AddSeparator(SyntaxFactory::MissingToken(SyntaxKind::CommaToken));
        auto missing = this->AddError(this->CreateMissingIdentifierName(), ErrorCode::ERR_TupleTooFewElements);
        list.Add(SyntaxFactory::TupleElement(missing, /*identifier:*/ nullptr));
    }

    GreenNode *elements = _pool.ToListAndFree(list);
    GreenNode *close = this->EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::TupleType(open, elements, close);
}

GreenNode *LanguageParser::ParseTupleElement() {
    GreenNode *type = ParseType();
    GreenNode *identifier = IsTrueIdentifier() ? this->ParseIdentifierToken() : nullptr;
    return SyntaxFactory::TupleElement(type, identifier);
}

LanguageParser::PostSkipAction LanguageParser::SkipBadArrayRankSpecifierTokens(GreenNode *&openBracket, SeparatedSyntaxListBuilder list,
                                                               SyntaxKind expected) {
    return this->SkipBadSeparatedListTokensWithExpectedKind(
        openBracket, list,
        [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleExpression(); },
        [](LanguageParser *p, SyntaxKind) { return p->CurrentToken()->kind == SyntaxKind::CloseBracketToken; }, expected);
}

GreenNode *LanguageParser::ParseUnderlyingType(ParseTypeMode mode, NameOptions options) {
    if (IsPredefinedType(this->CurrentToken()->kind)) {
        // This is a predefined type
        auto token = this->EatToken();
        if (token->kind == SyntaxKind::VoidKeyword && this->CurrentToken()->kind != SyntaxKind::AsteriskToken) {
            token = this->AddError(token, mode == ParseTypeMode::Parameter ? ErrorCode::ERR_NoVoidParameter
                                                                           : ErrorCode::ERR_NoVoidHere);
        }

        return SyntaxFactory::PredefinedType(token);
    }

    // The :: case is for error recovery.
    if (IsTrueIdentifier() || this->CurrentToken()->kind == SyntaxKind::ColonColonToken) {
        return this->ParseQualifiedName(options);
    }

    if (this->CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        return this->ParseTupleType();
    } else if (IsFunctionPointerStart()) {
        return ParseFunctionPointerTypeSyntax();
    }

    return this->AddError(this->CreateMissingIdentifierName(),
                          mode == ParseTypeMode::NewExpression ? ErrorCode::ERR_BadNewExpr : ErrorCode::ERR_TypeExpected);
}

GreenNode *LanguageParser::ParseFunctionPointerTypeSyntax() {
    CS_ASSERT(IsFunctionPointerStart());

    auto skipBadFunctionPointerTokens = [this](SeparatedSyntaxListBuilder list) -> PostSkipAction {
        GreenNode *tmp = nullptr;
        CS_ASSERT(list.Count() > 0);
        return SkipBadSeparatedListTokensWithExpectedKind(
            tmp, list,
            /*isNotExpectedFunction:*/ [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CommaToken; },
            // this.IsTerminator() (called by our caller) is the only thing that aborts parsing.
            /*abortFunction:*/ [](LanguageParser *, SyntaxKind) { return false; },
            /*expected:*/ SyntaxKind::CommaToken);
    };

    auto parseCallingConvention = [&]() -> GreenNode * {
        if (CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            GreenNode *managedSpecifier;
            GreenNode *peek1 = PeekToken(1);
            if (CurrentToken()->contextual_kind == SyntaxKind::ManagedKeyword ||
                CurrentToken()->contextual_kind == SyntaxKind::UnmanagedKeyword) {
                managedSpecifier = EatContextualToken(CurrentToken()->contextual_kind);
            } else if (IsPossibleFunctionPointerParameterListStart(peek1)) {
                // If there's a possible parameter list next, treat this as a bad identifier that should have been managed or unmanaged
                managedSpecifier = EatTokenAsKind(SyntaxKind::ManagedKeyword);
            } else if (peek1->kind == SyntaxKind::OpenBracketToken) {
                // If there's an open brace next, treat this as a bad identifier that should have been unmanaged
                managedSpecifier = EatTokenAsKind(SyntaxKind::UnmanagedKeyword);
            } else {
                // Whatever is next, it's probably not a calling convention or a function pointer type.
                // Bail out
                return nullptr;
            }

            GreenNode *unmanagedCallingConventions = nullptr;
            if (CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
                auto openBracket = EatToken(SyntaxKind::OpenBracketToken);
                auto callingConventionModifiers = _pool.AllocateSeparated();
                auto saveTerm = _termState;
                _termState |= TerminatorState::IsEndOfFunctionPointerCallingConvention;

                {
                    ScopeExit finally([&] { _termState = saveTerm; });
                    while (true) {
                        callingConventionModifiers.Add(
                            SyntaxFactory::FunctionPointerUnmanagedCallingConvention(EatToken(SyntaxKind::IdentifierToken)));

                        if (skipBadFunctionPointerTokens(callingConventionModifiers) == PostSkipAction::Abort) {
                            break;
                        }

                        CS_ASSERT(CurrentToken()->kind == SyntaxKind::CommaToken);
                        callingConventionModifiers.AddSeparator(EatToken(SyntaxKind::CommaToken));
                    }

                    auto closeBracket = EatToken(SyntaxKind::CloseBracketToken);

                    unmanagedCallingConventions = SyntaxFactory::FunctionPointerUnmanagedCallingConventionList(
                        openBracket, _pool.ToListAndFree(callingConventionModifiers), closeBracket);
                }
            }

            if (managedSpecifier->kind == SyntaxKind::ManagedKeyword && unmanagedCallingConventions != nullptr) {
                // 'managed' calling convention cannot be combined with unmanaged calling convention specifiers.
                unmanagedCallingConventions =
                    AddError(unmanagedCallingConventions, ErrorCode::ERR_CannotSpecifyManagedWithUnmanagedSpecifiers);
            }

            return SyntaxFactory::FunctionPointerCallingConvention(managedSpecifier, unmanagedCallingConventions);
        }

        return nullptr;
    };

    auto delegate_ = EatToken(SyntaxKind::DelegateKeyword);
    auto asterisk = EatToken(SyntaxKind::AsteriskToken);

    GreenNode *callingConvention = parseCallingConvention();

    if (!IsPossibleFunctionPointerParameterListStart(CurrentToken())) {
        auto lessThanTokenError = CreateMissingToken(SyntaxKind::LessThanToken, SyntaxKind::None);

        auto missingTypes = _pool.AllocateSeparated();
        auto missingType = SyntaxFactory::FunctionPointerParameter(/*attributeLists:*/ nullptr, /*modifiers:*/ nullptr,
                                                                   CreateMissingIdentifierName());
        missingTypes.Add(missingType);

        // Handle the simple case of delegate*>. We don't try to deal with any variation of delegate*invalid>, as
        // we don't know for sure that the expression isn't a relational with something else.
        GreenNode *parameters = _pool.ToListAndFree(missingTypes);
        GreenNode *greaterThan = TryEatToken(SyntaxKind::GreaterThanToken);
        if (greaterThan == nullptr) greaterThan = SyntaxFactory::MissingToken(SyntaxKind::GreaterThanToken);
        return SyntaxFactory::FunctionPointerType(
            delegate_, asterisk, callingConvention,
            SyntaxFactory::FunctionPointerParameterList(lessThanTokenError, parameters, greaterThan));
    }

    auto lessThanToken = EatTokenAsKind(SyntaxKind::LessThanToken);
    auto saveTerm = _termState;
    _termState |= (lessThanToken->IsMissing() ? TerminatorState::IsEndOfFunctionPointerParameterListErrored
                                              : TerminatorState::IsEndOfFunctionPointerParameterList);
    auto types = _pool.AllocateSeparated();

    ScopeExit finally([&] { _termState = saveTerm; });

    while (true) {
        auto modifiers = _pool.Allocate();

        ParseParameterModifiers(modifiers, /*isFunctionPointerParameter:*/ true, /*isLambdaParameter:*/ false);

        GreenNode *modifierList = _pool.ToListAndFree(modifiers);  // ToTokenListAndFree
        GreenNode *parameterType = ParseTypeOrVoid();
        types.Add(SyntaxFactory::FunctionPointerParameter(/*attributeLists:*/ nullptr, modifierList, parameterType));

        if (skipBadFunctionPointerTokens(types) == PostSkipAction::Abort) {
            break;
        }

        CS_ASSERT(CurrentToken()->kind == SyntaxKind::CommaToken);
        types.AddSeparator(EatToken(SyntaxKind::CommaToken));
    }

    GreenNode *parameters = _pool.ToListAndFree(types);
    GreenNode *greaterThan = lessThanToken->IsMissing() && CurrentToken()->kind == SyntaxKind::CloseParenToken
                                 ? EatTokenAsKind(SyntaxKind::GreaterThanToken)
                                 : EatToken(SyntaxKind::GreaterThanToken);
    return SyntaxFactory::FunctionPointerType(delegate_, asterisk, callingConvention,
                                              SyntaxFactory::FunctionPointerParameterList(lessThanToken, parameters, greaterThan));
}

bool LanguageParser::IsFunctionPointerStart() {
    return CurrentToken()->kind == SyntaxKind::DelegateKeyword && PeekToken(1)->kind == SyntaxKind::AsteriskToken;
}

// We consider both ( and < to be possible starts, in order to make error recovery more graceful
// in the scenario where a user accidentally surrounds their function pointer type list with parens.
bool LanguageParser::IsPossibleFunctionPointerParameterListStart(GreenNode *token) {
    return token->kind == SyntaxKind::LessThanToken || token->kind == SyntaxKind::OpenParenToken;
}

GreenNode *LanguageParser::ParsePointerTypeMods(GreenNode *type) {
    // Check for pointer types
    while (this->CurrentToken()->kind == SyntaxKind::AsteriskToken) {
        type = SyntaxFactory::PointerType(type, this->EatToken());
    }

    return type;
}

}  // namespace csharp
