// LanguageParser, part D: statements, blocks, local declarations and local functions -- a translation of
// Roslyn's LanguageParser.cs lines 8197-11027 (ParseStatement .. TryParseLocalFunctionStatementBody), in the
// C# order. See csharp_parser_internal.h for the translation conventions.
//
// C# evaluates call arguments left to right; C++ does not, so wherever a factory call's arguments consume
// tokens (`_syntaxFactory.X(this.EatToken(..), this.ParseY())`) they are first evaluated into locals, in the
// C# order.

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief Runs a callable at scope exit (a C# `finally`). */
template <class F>
struct Finally {
    F f;
    explicit Finally(F fn) : f(std::move(fn)) {}
    ~Finally() { f(); }
    Finally(const Finally &) = delete;
    Finally &operator=(const Finally &) = delete;
};
template <class F>
Finally(F) -> Finally<F>;

}  // namespace

GreenNode *LanguageParser::ParseStatement() {
    return ParseWithStackGuard(
        [](LanguageParser *self) -> GreenNode * {
            GreenNode *statement = self->ParsePossiblyAttributedStatement();
            return statement != nullptr ? statement : self->ParseExpressionStatement(nullptr);
        },
        [](LanguageParser *) -> GreenNode * {
            return SyntaxFactory::EmptyStatement(nullptr, SyntaxFactory::MissingToken(SyntaxKind::SemicolonToken));
        });
}

GreenNode *LanguageParser::ParsePossiblyAttributedStatement() {
    return ParseStatementCore(ParseStatementAttributeDeclarations(), /*isGlobal:*/ false);
}

SyntaxList LanguageParser::ParseStatementAttributeDeclarations() {
    if (CurrentToken()->kind != SyntaxKind::OpenBracketToken) return nullptr;

    // See if we should treat this as a collection expression.  At the top-level or statement-level, this should
    // only be considered a collection if followed by a `.`, `?` or `!` (indicating it's a value, not an
    // attribute).
    ResetPoint resetPoint = GetResetPoint();

    // Grab the first part as a collection expression.
    ParseCollectionExpression();

    // Continue consuming element access expressions for `[x][y]...`.
    bool hadBracketArgumentList = false;
    while (CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
        ParseBracketedArgumentList();
        hadBracketArgumentList = true;
    }

    // Check the next token to see if it indicates the `[...]` sequence we have is a term or not.
    SyntaxKind k = CurrentToken()->kind;
    bool isCollectionExpression = k == SyntaxKind::DotToken || k == SyntaxKind::QuestionToken ||
                                  k == SyntaxKind::ExclamationToken || k == SyntaxKind::PlusPlusToken ||
                                  k == SyntaxKind::MinusMinusToken || k == SyntaxKind::MinusGreaterThanToken;

    // Now look for another set of items that indicate that we're not an attribute, but instead are a collection
    // expression misplaced in an invalid top level expression-statement. (like `[] + b`).
    isCollectionExpression =
        isCollectionExpression || IsExpectedBinaryOperator(CurrentToken()->kind) ||
        IsExpectedAssignmentOperator(CurrentToken()->kind) ||
        ((CurrentToken()->contextual_kind == SyntaxKind::SwitchKeyword ||
          CurrentToken()->contextual_kind == SyntaxKind::WithKeyword) &&
         PeekToken(1)->kind == SyntaxKind::OpenBraceToken);

    if (!isCollectionExpression && hadBracketArgumentList && CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        // `[() => {}][rand.NextInt() % x]();` vs. `[Attr] (A, B) LocalFunc() { }`: parse out a type and look for
        // an identifier next.
        GreenNode *returnType = ParseReturnType();
        isCollectionExpression = ContainsErrorDiagnostic(returnType) || !IsTrueIdentifier();
    }

    // If this was a collection expression, not an attribute declaration, return no attributes so that the
    // caller will parse this out as a collection expression. Otherwise re-parse the code as the actual
    // attribute declarations.
    Reset(resetPoint);
    SyntaxList attributes =
        isCollectionExpression ? SyntaxList() : ParseAttributeDeclarations(/*inExpressionContext:*/ true);
    Release(resetPoint);

    return attributes;
}

GreenNode *LanguageParser::ParseStatementCore(SyntaxList attributes, bool isGlobal) {
    if (GreenNode *reused = TryReuseStatement(attributes, isGlobal)) {
        return reused;
    }

    ResetPoint resetPointBeforeStatement = GetResetPoint();
    Finally finally_([this, &resetPointBeforeStatement] {
        _recursionDepth--;
        Release(resetPointBeforeStatement);
    });
    _recursionDepth++;
    EnsureSufficientExecutionStack(_recursionDepth);

    GreenNode *result;

    // Main switch to handle processing almost any statement.
    switch (CurrentToken()->kind) {
    case SyntaxKind::FixedKeyword:
        return ParseFixedStatement(attributes);
    case SyntaxKind::BreakKeyword:
        return ParseBreakStatement(attributes);
    case SyntaxKind::ContinueKeyword:
        return ParseContinueStatement(attributes);
    case SyntaxKind::TryKeyword:
    case SyntaxKind::CatchKeyword:
    case SyntaxKind::FinallyKeyword:
        return ParseTryStatement(attributes);
    case SyntaxKind::CheckedKeyword:
    case SyntaxKind::UncheckedKeyword:
        return ParseCheckedStatement(attributes);
    case SyntaxKind::DoKeyword:
        return ParseDoStatement(attributes);
    case SyntaxKind::ForKeyword:
        return ParseForOrForEachStatement(attributes);
    case SyntaxKind::ForEachKeyword:
        return ParseForEachStatement(attributes, /*awaitTokenOpt:*/ nullptr);
    case SyntaxKind::GotoKeyword:
        return ParseGotoStatement(attributes);
    case SyntaxKind::IfKeyword:
        return ParseIfStatement(attributes);
    case SyntaxKind::ElseKeyword:
        // Including 'else' keyword to handle 'else without if' error cases
        return ParseMisplacedElse(attributes);
    case SyntaxKind::LockKeyword:
        return ParseLockStatement(attributes);
    case SyntaxKind::ReturnKeyword:
        return ParseReturnStatement(attributes);
    case SyntaxKind::SwitchKeyword:
    case SyntaxKind::CaseKeyword:  // error recovery case.
        return ParseSwitchStatement(attributes);
    case SyntaxKind::ThrowKeyword:
        return ParseThrowStatement(attributes);
    case SyntaxKind::UnsafeKeyword:
        result = TryParseStatementStartingWithUnsafe(attributes);
        if (result != nullptr) return result;
        break;
    case SyntaxKind::UsingKeyword:
        return ParseStatementStartingWithUsing(attributes);
    case SyntaxKind::WhileKeyword:
        return ParseWhileStatement(attributes);
    case SyntaxKind::OpenBraceToken:
        return ParseBlock(attributes);
    case SyntaxKind::SemicolonToken:
        return SyntaxFactory::EmptyStatement(attributes, EatToken());
    case SyntaxKind::IdentifierToken:
        result = TryParseStatementStartingWithIdentifier(attributes, isGlobal);
        if (result != nullptr) return result;
        break;
    default:
        break;
    }

    return ParseStatementCoreRest(attributes, isGlobal, resetPointBeforeStatement);
}

GreenNode *LanguageParser::TryReuseStatement(SyntaxList /*attributes*/, bool /*isGlobal*/) {
    // Incremental reuse only (IsIncrementalAndFactoryContextMatches is always false).
    return nullptr;
}

template <class RP>
GreenNode *LanguageParser::ParseStatementCoreRest(SyntaxList attributes, bool isGlobal,
                                                  RP &resetPointBeforeStatement) {
    isGlobal = isGlobal && IsScript();

    if (!IsPossibleLocalDeclarationStatement(isGlobal)) {
        return ParseExpressionStatement(attributes);
    }

    if (isGlobal) {
        // if we're at the global script level, then we don't support local-decls or
        // local-funcs. The caller instead will look for those and parse them as
        // fields/methods in the global script scope.
        return nullptr;
    }

    bool beginsWithAwait = CurrentToken()->contextual_kind == SyntaxKind::AwaitKeyword;
    GreenNode *result = ParseLocalDeclarationStatement(attributes);

    // didn't get any sort of statement.  This was something else entirely
    // (like just a `}`).  No need to retry anything here.  Just reset back
    // to where we started from and bail entirely from parsing a statement.
    if (result == nullptr) {
        Reset(resetPointBeforeStatement);
        return nullptr;
    }

    if (result->ContainsDiagnostics() && beginsWithAwait && !IsInAsync()) {
        // Local decl had issues.  We were also starting with 'await' in a non-async
        // context. Retry parsing this as if we were in an 'async' context as it's much
        // more likely that this was a misplace await-expr' than a local decl.
        Reset(resetPointBeforeStatement);

        ParserSyntaxContextResetter _(this, /*isInAsyncContext:*/ true);
        result = ParseExpressionStatement(attributes);
    }

    // Didn't want to retry as an `await expr`.  Just return what we actually
    // produced.
    return result;
}

GreenNode *LanguageParser::TryParseStatementStartingWithIdentifier(SyntaxList attributes, bool isGlobal) {
    if (CurrentToken()->contextual_kind == SyntaxKind::AwaitKeyword &&
        PeekToken(1)->kind == SyntaxKind::ForEachKeyword) {
        GreenNode *awaitToken = EatContextualToken(SyntaxKind::AwaitKeyword);
        return ParseForEachStatement(attributes, awaitToken);
    } else if (IsPossibleAwaitUsing()) {
        if (PeekToken(2)->kind == SyntaxKind::OpenParenToken) {
            // `await using Type ...` is handled below in ParseLocalDeclarationStatement
            GreenNode *awaitToken = EatContextualToken(SyntaxKind::AwaitKeyword);
            return ParseUsingStatement(attributes, awaitToken);
        }
    } else if (IsPossibleLabeledStatement()) {
        return ParseLabeledStatement(attributes);
    } else if (IsPossibleYieldStatement()) {
        return ParseYieldStatement(attributes);
    } else if (IsPossibleAwaitExpressionStatement()) {
        return ParseExpressionStatement(attributes);
    } else if (IsQueryExpression(/*mayBeVariableDeclaration:*/ true,
                                 /*mayBeMemberDeclaration:*/ isGlobal && IsScript())) {
        GreenNode *query = ParseQueryExpression(static_cast<Precedence>(0));
        return ParseExpressionStatement(attributes, query);
    }

    return nullptr;
}

GreenNode *LanguageParser::ParseStatementStartingWithUsing(SyntaxList attributes) {
    return PeekToken(1)->kind == SyntaxKind::OpenParenToken ? ParseUsingStatement(attributes)
                                                            : ParseLocalDeclarationStatement(attributes);
}

// Checking for brace to disambiguate between unsafe statement and unsafe local function
GreenNode *LanguageParser::TryParseStatementStartingWithUnsafe(SyntaxList attributes) {
    return IsPossibleUnsafeStatement() ? ParseUnsafeStatement(attributes) : nullptr;
}

bool LanguageParser::IsPossibleAwaitUsing() {
    return CurrentToken()->contextual_kind == SyntaxKind::AwaitKeyword &&
           PeekToken(1)->kind == SyntaxKind::UsingKeyword;
}

bool LanguageParser::IsPossibleLabeledStatement() {
    return PeekToken(1)->kind == SyntaxKind::ColonToken && IsTrueIdentifier();
}

bool LanguageParser::IsPossibleUnsafeStatement() { return PeekToken(1)->kind == SyntaxKind::OpenBraceToken; }

bool LanguageParser::IsPossibleYieldStatement() {
    return CurrentToken()->contextual_kind == SyntaxKind::YieldKeyword &&
           (PeekToken(1)->kind == SyntaxKind::ReturnKeyword || PeekToken(1)->kind == SyntaxKind::BreakKeyword);
}

bool LanguageParser::IsPossibleLocalDeclarationStatement(bool isGlobalScriptLevel) {
    // This method decides whether to parse a statement as a
    // declaration or as an expression statement. In the old
    // compiler it would simply call IsLocalDeclaration.

    SyntaxKind tk = CurrentToken()->kind;
    if (tk == SyntaxKind::RefKeyword ||
        IsDeclarationModifier(tk) ||  // treat `static int x = 2;` as a local variable declaration
        (SyntaxFacts::IsPredefinedType(tk) &&
         PeekToken(1)->kind != SyntaxKind::DotToken &&       // e.g. `int.Parse()` is an expression
         PeekToken(1)->kind != SyntaxKind::OpenParenToken)) {  // e.g. `int (x, y)` is an error decl expression
        return true;
    }

    // note: `using (` and `await using (` are already handled in ParseStatementCore.
    if (tk == SyntaxKind::UsingKeyword) {
        CS_ASSERT(PeekToken(1)->kind != SyntaxKind::OpenParenToken);
        return true;
    }

    if (IsPossibleAwaitUsing()) {
        CS_ASSERT(PeekToken(2)->kind != SyntaxKind::OpenParenToken);
        return true;
    }

    if (IsDefiniteScopedModifier(/*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ false)) {
        return true;
    }

    tk = CurrentToken()->contextual_kind;

    bool isPossibleModifier =
        IsAdditionalLocalFunctionModifier(tk) &&
        (!(tk == SyntaxKind::AsyncKeyword || tk == SyntaxKind::ScopedKeyword) ||
         ShouldContextualKeywordBeTreatedAsModifier(/*parsingStatementNotDeclaration:*/ true));
    if (isPossibleModifier) {
        return true;
    }

    return IsPossibleFirstTypedIdentifierInLocalDeclarationStatement(isGlobalScriptLevel);
}

bool LanguageParser::IsPossibleFirstTypedIdentifierInLocalDeclarationStatement(bool isGlobalScriptLevel) {
    std::optional<bool> typedIdentifier =
        IsPossibleTypedIdentifierStart(CurrentToken(), PeekToken(1), /*allowThisKeyword:*/ false);
    if (typedIdentifier.has_value()) {
        return *typedIdentifier;
    }

    // It's common to have code like the following:
    //
    //      Task.
    //      await Task.Delay()
    //
    // In this case we don't want to parse this as a local declaration like:
    //
    //      Task.await Task
    //
    // Note that we explicitly do this check when we see that the code spreads over multiple
    // lines.  We don't want this if the user has actually written "X.Y z"
    SyntaxKind tk = CurrentToken()->contextual_kind;

    if (tk == SyntaxKind::IdentifierToken) {
        GreenNode *token1 = PeekToken(1);
        if (token1->kind == SyntaxKind::DotToken &&
            SyntaxList(token1->GetTrailingTrivia()).Any(SyntaxKind::EndOfLineTrivia)) {
            if (PeekToken(2)->kind == SyntaxKind::IdentifierToken &&
                PeekToken(3)->kind == SyntaxKind::IdentifierToken) {
                // We have something like:
                //
                //      X.
                //      Y z
                //
                // This is only a local declaration if we have:
                //
                //      X.Y z;
                //      X.Y z = ...
                //      X.Y z, ...
                //      X.Y z( ...      (local function)
                //      X.Y z<W...      (local function)
                //
                SyntaxKind token4Kind = PeekToken(4)->kind;
                if (token4Kind != SyntaxKind::SemicolonToken && token4Kind != SyntaxKind::EqualsToken &&
                    token4Kind != SyntaxKind::CommaToken && token4Kind != SyntaxKind::OpenParenToken &&
                    token4Kind != SyntaxKind::LessThanToken) {
                    return false;
                }
            }
        }
    }

    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

    ScanTypeFlags st = ScanType();

    // We could always return true for st == AliasQualName in addition to MustBeType on the first line, however, we
    // want it to return false in the case where CurrentToken.Kind != SyntaxKind.Identifier so that error cases,
    // like: A::N(), are not parsed as variable declarations and instead are parsed as A.N() where we can give a
    // better error message saying "did you meant to use a '.'?"
    if (st == ScanTypeFlags::MustBeType && CurrentToken()->kind != SyntaxKind::DotToken &&
        CurrentToken()->kind != SyntaxKind::OpenParenToken) {
        return true;
    }

    if (st == ScanTypeFlags::NotType) {
        return false;
    }

    if (CurrentToken()->kind != SyntaxKind::IdentifierToken) {
        // In the case of something like:
        //     List<SomeType>
        //     if
        // we know that we're in an error case, as the following keyword must be the start of a new statement.
        // We'd prefer to assume that this is an incomplete local declaration over an expression, as it's more likely
        // the user is just in the middle of writing a local declaration, and not an expression.
        return st == ScanTypeFlags::GenericTypeOrExpression &&
               (IsDefiniteStatement() || IsTypeDeclarationStart() || IsAccessibilityModifier(CurrentToken()->kind));
    }

    // T? and T* might start an expression, we need to parse further to disambiguate:
    if (isGlobalScriptLevel) {
        if (st == ScanTypeFlags::PointerOrMultiplication) {
            return false;
        } else if (st == ScanTypeFlags::NullableType) {
            return IsPossibleDeclarationStatementFollowingNullableType(isGlobalScriptLevel);
        }
    }

    return true;
}

bool LanguageParser::IsPossibleTopLevelUsingLocalDeclarationStatement() {
    if (CurrentToken()->kind != SyntaxKind::UsingKeyword) {
        return false;
    }

    SyntaxKind tk = PeekToken(1)->kind;

    if (tk == SyntaxKind::RefKeyword) {
        return true;
    }

    if (IsDeclarationModifier(tk)) {  // treat `const int x = 2;` as a local variable declaration
        if (tk != SyntaxKind::StaticKeyword) {  // For `static` we still need to make sure we have a typed identifier
                                                // after it, because `using static type;` is a valid using directive.
            return true;
        }
    } else if (SyntaxFacts::IsPredefinedType(tk)) {
        return true;
    }

    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

    // Skip 'using' keyword
    EatToken();

    if (IsDefiniteScopedModifier(/*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ false)) {
        return true;
    }

    if (tk == SyntaxKind::StaticKeyword) {
        // Skip 'static' keyword
        EatToken();
    }

    return IsPossibleFirstTypedIdentifierInLocalDeclarationStatement(/*isGlobalScriptLevel:*/ false);
}

// Looks ahead for a declaration of a field, property or method declaration following a nullable type T?.
bool LanguageParser::IsPossibleDeclarationStatementFollowingNullableType(bool isGlobalScriptLevel) {
    if (IsFieldDeclaration(/*isEvent:*/ false, isGlobalScriptLevel)) {
        return IsPossibleFieldDeclarationFollowingNullableType();
    }

    GreenNode *explicitInterfaceOpt = nullptr;
    GreenNode *identifierOrThisOpt = nullptr;
    GreenNode *typeParameterListOpt = nullptr;
    ParseMemberName(explicitInterfaceOpt, identifierOrThisOpt, typeParameterListOpt, /*isEvent:*/ false);

    if (explicitInterfaceOpt == nullptr && identifierOrThisOpt == nullptr && typeParameterListOpt == nullptr) {
        return false;
    }

    // looks like a property:
    //      T? Goo {
    //
    // Importantly, we don't consider `T? Goo =>` to be the start of a property.  This is because it's legal to
    // write:
    //      T ? Goo => Goo : Bar => Bar
    if (CurrentToken()->kind == SyntaxKind::OpenBraceToken) {
        return true;
    }

    // don't accept indexers:
    // (C# dereferences identifierOrThisOpt here even when it is null: a NullReferenceException in Roslyn. Never
    // reached -- IsScript is false.)
    if (identifierOrThisOpt != nullptr && identifierOrThisOpt->kind == SyntaxKind::ThisKeyword) {
        return false;
    }

    return IsPossibleMethodDeclarationFollowingNullableType();
}

// At least one variable declaration terminated by a semicolon or a comma.
//   idf;
//   idf,
//   idf = <expr>;
//   idf = <expr>,
bool LanguageParser::IsPossibleFieldDeclarationFollowingNullableType() {
    if (CurrentToken()->kind != SyntaxKind::IdentifierToken) {
        return false;
    }

    EatToken();

    if (CurrentToken()->kind == SyntaxKind::EqualsToken) {
        TerminatorState saveTerm = _termState;
        _termState |= TerminatorState::IsEndOfFieldDeclaration;
        EatToken();
        ParseVariableInitializer();
        _termState = saveTerm;
    }

    return CurrentToken()->kind == SyntaxKind::CommaToken || CurrentToken()->kind == SyntaxKind::SemicolonToken;
}

bool LanguageParser::IsPossibleMethodDeclarationFollowingNullableType() {
    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfMethodSignature;

    GreenNode *paramList = ParseParenthesizedParameterList(/*forExtensionOrUnion:*/ false);

    _termState = saveTerm;
    SyntaxList separatedParameters = SeparatedSyntaxList(GetField(paramList, Field::Parameters)).GetWithSeparators();
    GreenNode *closeParenToken = GetField(paramList, Field::CloseParenToken);

    // parsed full signature:
    if (!closeParenToken->IsMissing()) {
        // (...) {
        // (...) where
        if (CurrentToken()->kind == SyntaxKind::OpenBraceToken ||
            CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
            return true;
        }

        // disambiguates conditional expressions
        // (...) :
        if (CurrentToken()->kind == SyntaxKind::ColonToken) {
            return false;
        }
    }

    // no parameters, just an open paren followed by a token that doesn't belong to a parameter definition:
    if (separatedParameters.Count() == 0) {
        return false;
    }

    GreenNode *parameter = separatedParameters[0];
    SyntaxList parameterModifiers = GetField(parameter, Field::Modifiers);
    GreenNode *parameterType = GetField(parameter, Field::Type);
    GreenNode *parameterIdentifier = GetField(parameter, Field::Identifier);

    // has an attribute:
    //   ([Attr]
    if (SyntaxList(GetField(parameter, Field::AttributeLists)).Count() > 0) {
        return true;
    }

    // has params modifier:
    //   (params
    for (int i = 0; i < parameterModifiers.Count(); i++) {
        if (parameterModifiers[i]->kind == SyntaxKind::ParamsKeyword) {
            return true;
        }
    }

    if (parameterType == nullptr) {
        // has arglist:
        //   (__arglist
        if (parameterIdentifier->kind == SyntaxKind::ArgListKeyword) {
            return true;
        }
    } else if (parameterType->kind == SyntaxKind::NullableType) {
        // nullable type with modifiers
        //   (ref T?
        //   (out T?
        if (parameterModifiers.Count() > 0) {
            return true;
        }

        // nullable type, identifier, and separator or closing parent
        //   (T ? idf,
        //   (T ? idf)
        if (!parameterIdentifier->IsMissing() &&
            ((separatedParameters.Count() >= 2 && !separatedParameters[1]->IsMissing()) ||
             (separatedParameters.Count() == 1 && !closeParenToken->IsMissing()))) {
            return true;
        }
    } else if (parameterType->kind == SyntaxKind::IdentifierName &&
               GetField(parameterType, Field::Identifier)->contextual_kind == SyntaxKind::FromKeyword) {
        // assume that "from" is meant to be a query start ("from" bound to a type is rare):
        // (from
        return false;
    } else {
        // has a name and a non-nullable type:
        //   (T idf
        //   (ref T idf
        //   (out T idf
        if (!parameterIdentifier->IsMissing()) {
            return true;
        }
    }

    return false;
}

/**
 * Determines if the current 'delegate' keyword starts an anonymous delegate expression rather than a delegate
 * type declaration: true for `delegate { }` / `delegate (params) { }`, false for `delegate Type Name(params);`.
 */
bool LanguageParser::IsAnonymousDelegateExpression() {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::DelegateKeyword);

    GreenNode *nextToken = PeekToken(1);

    // delegate { } is definitely an anonymous delegate
    if (nextToken->kind == SyntaxKind::OpenBraceToken) {
        return true;
    }

    // If not followed by '(', it's a delegate type declaration
    if (nextToken->kind != SyntaxKind::OpenParenToken) {
        return false;
    }

    // Now we have 'delegate (' - need to distinguish:
    // - Anonymous delegate: delegate (params) { }
    // - Delegate declaration: delegate (TupleType) Name(params);
    DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ true);

    // Skip 'delegate'
    EatToken();

    CS_ASSERT(CurrentToken()->kind == SyntaxKind::OpenParenToken);
    // Skip '('
    EatToken();

    // Try to scan as a tuple type
    GreenNode *unused = nullptr;
    ScanTypeFlags scanResult = ScanTupleType(unused);

    // If it successfully scanned as a tuple type and is followed by an identifier,
    // it's a delegate type declaration with a tuple return type.
    if (scanResult == ScanTypeFlags::TupleType && CurrentToken()->kind == SyntaxKind::IdentifierToken) {
        return false;
    }

    // Otherwise, assume it's an anonymous delegate expression.
    return true;
}

bool LanguageParser::IsPossibleNewExpression() {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::NewKeyword);

    // skip new
    GreenNode *nextToken = PeekToken(1);

    // new { }
    // new [ ]
    switch (nextToken->kind) {
    case SyntaxKind::OpenBraceToken:
    case SyntaxKind::OpenBracketToken:
        return true;
    default:
        break;
    }

    //
    // Declaration with new modifier vs. new expression
    // Parse it as an expression if the type is not followed by an identifier or this keyword.
    //
    // Member declarations:
    //   new T Idf ...
    //   new T this ...
    //   new partial Idf    ("partial" as a type name)
    //   new partial this   ("partial" as a type name)
    //   new partial T Idf
    //   new partial T this
    //   new <modifier>
    //   new <class|interface|struct|enum>
    //   new partial <class|interface|struct|enum>
    //
    // New expressions:
    //   new T []
    //   new T { }
    //   new <non-type>
    //   new partial []
    //
    if (SyntaxFacts::GetBaseTypeDeclarationKind(nextToken->kind) != SyntaxKind::None) {
        return false;
    }

    DeclarationModifiers modifier = GetModifierExcludingScoped(nextToken);
    if (modifier == DeclarationModifiers::Partial) {
        if (SyntaxFacts::IsPredefinedType(PeekToken(2)->kind)) {
            return false;
        }

        // class, struct, enum, interface keywords, but also other modifiers that are not allowed after
        // partial keyword but start class declaration, so we can assume the user just swapped them.
        if (IsTypeModifierOrTypeKeyword(PeekToken(2)->kind)) {
            return false;
        }
    } else if (modifier != DeclarationModifiers::None) {
        return false;
    }

    std::optional<bool> typedIdentifier =
        IsPossibleTypedIdentifierStart(nextToken, PeekToken(2), /*allowThisKeyword:*/ true);
    if (typedIdentifier.has_value()) {
        // new Idf Idf
        // new Idf .
        // new partial T
        // new partial .
        return !*typedIdentifier;
    }

    DisposableResetPoint _(this, /*resetOnDispose:*/ true);

    // skips new keyword
    EatToken();
    ScanTypeFlags st = ScanType();

    return !IsPossibleMemberName() || st == ScanTypeFlags::NotType;
}

/**
 * @return true if the current token can be the first token of a typed identifier (a type name followed by an
 *         identifier), false if it definitely can't be, nullopt if we need to scan further to find out.
 */
std::optional<bool> LanguageParser::IsPossibleTypedIdentifierStart(GreenNode *current, GreenNode *next,
                                                                   bool allowThisKeyword) {
    if (IsTrueIdentifier(current)) {
        switch (next->kind) {
        // tokens that can be in type names...
        case SyntaxKind::DotToken:
        case SyntaxKind::AsteriskToken:
        case SyntaxKind::QuestionToken:
        case SyntaxKind::OpenBracketToken:
        case SyntaxKind::LessThanToken:
        case SyntaxKind::ColonColonToken:
            return std::nullopt;

        case SyntaxKind::OpenParenToken:
            if (SyntaxFacts::IsIdentifierVar(current)) {
                // potentially either a tuple type in a local declaration (true), or
                // a tuple lvalue in a deconstruction assignment (false).
                return std::nullopt;
            } else {
                return false;
            }

        case SyntaxKind::IdentifierToken:
            return IsTrueIdentifier(next);

        case SyntaxKind::ThisKeyword:
            return allowThisKeyword;

        default:
            return false;
        }
    }

    return std::nullopt;
}

GreenNode *LanguageParser::ParsePossiblyAttributedBlock() {
    return ParseBlock(ParseAttributeDeclarations(/*inExpressionContext:*/ false));
}

/// Used to parse the block-body for a method or accessor.  For blocks that appear *inside*
/// method bodies, call ParseBlock.
/// isAccessorBody: if true, then we produce a special diagnostic if the open brace is missing.
GreenNode *LanguageParser::ParseMethodOrAccessorBodyBlock(SyntaxList attributes, bool isAccessorBody) {
    // (Incremental re-use of a Block: not ported, IsIncrementalAndFactoryContextMatches is always false.)

    // There's a special error code for a missing token after an accessor keyword
    GreenNode *openBrace =
        isAccessorBody && CurrentToken()->kind != SyntaxKind::OpenBraceToken
            ? AddError(SyntaxFactory::MissingToken(SyntaxKind::OpenBraceToken),
                       // IsFeatureEnabled(MessageID.IDS_FeatureExpressionBodiedAccessor) is always true.
                       ErrorCode::ERR_SemiOrLBraceOrArrowExpected)
            : EatToken(SyntaxKind::OpenBraceToken);

    SyntaxListBuilder statements = _pool.Allocate();
    ParseStatements(openBrace, statements, /*stopOnSwitchSections:*/ false);

    // Force creation a many-children list, even if only 1, 2, or 3 elements in the statement list (so a single
    // statement longer than 60 characters is wrapped in a one-item list node, as Roslyn's SyntaxList.List(array)
    // does).
    GreenNode *statementList =
        IsLargeEnoughNonEmptyStatementList(statements)
            ? SyntaxFactory::MakeList(statements.Storage()->data(), statements.Count())
            : statements.ToList();
    GreenNode *closeBrace = EatToken(SyntaxKind::CloseBraceToken);
    GreenNode *block = SyntaxFactory::Block(attributes, openBrace, statementList, closeBrace);

    _pool.Free(statements);
    return block;
}

/// Used to parse normal blocks that appear inside method bodies.  For the top level block
/// of a method/accessor use ParseMethodOrAccessorBodyBlock.
GreenNode *LanguageParser::ParseBlock(SyntaxList attributes) {
    // (Incremental re-use of a Block: not ported, IsIncrementalAndFactoryContextMatches is always false.)

    GreenNode *openBrace = EatToken(SyntaxKind::OpenBraceToken);

    SyntaxListBuilder statements = _pool.Allocate();
    ParseStatements(openBrace, statements, /*stopOnSwitchSections:*/ false);

    GreenNode *statementList = _pool.ToListAndFree(statements);
    GreenNode *closeBrace = EatToken(SyntaxKind::CloseBraceToken);
    return SyntaxFactory::Block(attributes, openBrace, statementList, closeBrace);
}

// Is this statement list non-empty, and large enough to make using weak children beneficial?
bool LanguageParser::IsLargeEnoughNonEmptyStatementList(SyntaxListBuilder statements) {
    if (statements.Count() == 0) {
        return false;
    } else if (statements.Count() == 1) {
        // If we have a single statement, it might be small, like "return null", or large,
        // like a loop or if or switch with many statements inside. Use the width as a proxy for
        // how big it is. If it's small, its better to forgo a many children list anyway, since the
        // weak reference would consume as much memory as is saved.
        return statements[0]->Width() > 60;
    } else {
        // For 2 or more statements, go ahead and create a many-children lists.
        return true;
    }
}

void LanguageParser::ParseStatements(GreenNode *&previousNode, SyntaxListBuilder statements,
                                     bool stopOnSwitchSections) {
    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsPossibleStatementStartOrStop;  // partial statements can abort if a new
                                                                    // statement starts
    if (stopOnSwitchSections) {
        _termState |= TerminatorState::IsSwitchSectionStart;
    }

    int lastTokenPosition = -1;
    while (CurrentToken()->kind != SyntaxKind::CloseBraceToken && CurrentToken()->kind != SyntaxKind::EndOfFileToken &&
           !(stopOnSwitchSections && IsPossibleSwitchSection()) && IsMakingProgress(lastTokenPosition)) {
        if (IsPossibleStatement()) {
            GreenNode *statement = ParsePossiblyAttributedStatement();
            if (statement != nullptr) {
                statements.Add(statement);
                continue;
            }
        }

        GreenNode *trailingTrivia = nullptr;
        PostSkipAction action = SkipBadStatementListTokens(statements, SyntaxKind::CloseBraceToken, trailingTrivia);
        if (trailingTrivia != nullptr) {
            previousNode = AddTrailingSkippedSyntax(previousNode, trailingTrivia);
        }

        if (action == PostSkipAction::Abort) {
            break;
        }
    }

    _termState = saveTerm;
}

bool LanguageParser::IsPossibleStatementStartOrStop() {
    return CurrentToken()->kind == SyntaxKind::SemicolonToken || IsPossibleStatement();
}

LanguageParser::PostSkipAction LanguageParser::SkipBadStatementListTokens(SyntaxListBuilder statements, SyntaxKind expected,
                                                          GreenNode *&trailingTrivia) {
    return SkipBadListTokensWithExpectedKindHelper(
        statements,
        // We know we have a bad statement, so it can't be a local
        // function, meaning we shouldn't consider accessibility
        // modifiers to be the start of a statement
        [](LanguageParser *p) { return !p->IsPossibleStatement(); },
        [](LanguageParser *p, SyntaxKind) { return p->CurrentToken()->kind == SyntaxKind::CloseBraceToken; },
        expected,
        /*closeKind:*/ SyntaxKind::None, trailingTrivia);
}

bool LanguageParser::IsDefiniteStatement() {
    SyntaxKind tk = CurrentToken()->kind;
    // Only those cases that can be certain start a new statement, regardless of context
    switch (tk) {
    case SyntaxKind::FixedKeyword:
    case SyntaxKind::BreakKeyword:
    case SyntaxKind::ContinueKeyword:
    case SyntaxKind::TryKeyword:
    case SyntaxKind::ConstKeyword:
    case SyntaxKind::DoKeyword:
    case SyntaxKind::ForKeyword:
    case SyntaxKind::ForEachKeyword:
    case SyntaxKind::GotoKeyword:
    case SyntaxKind::IfKeyword:
    case SyntaxKind::ElseKeyword:
    case SyntaxKind::LockKeyword:
    case SyntaxKind::ReturnKeyword:
    case SyntaxKind::UnsafeKeyword:
    case SyntaxKind::UsingKeyword:
    case SyntaxKind::WhileKeyword:
    case SyntaxKind::VolatileKeyword:
    case SyntaxKind::ExternKeyword:
    case SyntaxKind::CaseKeyword:  // for parsing an errant case without a switch.
        return true;

    default:
        return false;
    }
}

bool LanguageParser::IsPossibleStatement() {
    if (IsDefiniteStatement()) {
        return true;
    }

    SyntaxKind tk = CurrentToken()->kind;
    switch (tk) {
    case SyntaxKind::CheckedKeyword:
    case SyntaxKind::UncheckedKeyword:
    case SyntaxKind::ThrowKeyword:
    case SyntaxKind::SwitchKeyword:
    case SyntaxKind::OpenBraceToken:
    case SyntaxKind::SemicolonToken:
    case SyntaxKind::StaticKeyword:
    case SyntaxKind::ReadOnlyKeyword:
    case SyntaxKind::RefKeyword:
    case SyntaxKind::OpenBracketToken:
        return true;

    case SyntaxKind::IdentifierToken:
        return IsTrueIdentifier();

    default:
        return IsPredefinedType(tk) || IsPossibleExpression();
    }
}

GreenNode *LanguageParser::ParseFixedStatement(SyntaxList attributes) {
    GreenNode *fixed_ = EatToken(SyntaxKind::FixedKeyword);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfFixedStatement;
    GreenNode *decl = ParseParenthesizedVariableDeclaration(VariableFlags::None, /*scopedKeyword:*/ nullptr);
    _termState = saveTerm;

    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *statement = ParseEmbeddedStatement();
    return SyntaxFactory::FixedStatement(attributes, fixed_, openParen, decl, closeParen, statement);
}

bool LanguageParser::IsEndOfFixedStatement() {
    SyntaxKind k = CurrentToken()->kind;
    return k == SyntaxKind::CloseParenToken || k == SyntaxKind::OpenBraceToken || k == SyntaxKind::SemicolonToken;
}

GreenNode *LanguageParser::ParseEmbeddedStatement() {
    // parseEmbeddedStatementRest, inlined.
    GreenNode *statement = ParsePossiblyAttributedStatement();
    if (statement == nullptr) {
        // The consumers of embedded statements are expecting to receive a non-null statement
        // yet there are several error conditions that can lead ParseStatementCore to return
        // null.  When that occurs create an error empty Statement and return it to the caller.
        return SyntaxFactory::EmptyStatement(nullptr, EatToken(SyntaxKind::SemicolonToken));
    }

    // In scripts, stand-alone expression statements may not be followed by semicolons; the C# re-reports a
    // missing semicolon on embedded expression statements there. Dead code: IsScript is false.

    return statement;
}

GreenNode *LanguageParser::ParseBreakStatement(SyntaxList attributes) {
    GreenNode *breakKeyword = EatToken(SyntaxKind::BreakKeyword);
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::BreakStatement(attributes, breakKeyword, semicolon);
}

GreenNode *LanguageParser::ParseContinueStatement(SyntaxList attributes) {
    GreenNode *continueKeyword = EatToken(SyntaxKind::ContinueKeyword);
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::ContinueStatement(attributes, continueKeyword, semicolon);
}

GreenNode *LanguageParser::ParseTryStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::TryKeyword || CurrentToken()->kind == SyntaxKind::CatchKeyword ||
              CurrentToken()->kind == SyntaxKind::FinallyKeyword);

    auto missingBlock = []() -> GreenNode * {
        return SyntaxFactory::Block(nullptr, SyntaxFactory::MissingToken(SyntaxKind::OpenBraceToken), nullptr,
                                    SyntaxFactory::MissingToken(SyntaxKind::CloseBraceToken));
    };

    // We are called into on try/catch/finally, so eating the try may actually fail.
    GreenNode *try_ = EatToken(SyntaxKind::TryKeyword);

    GreenNode *tryBlock;
    if (try_->IsMissing()) {
        // If there was no actual `try`, then we got here because of a misplaced `catch`/`finally`.  In that
        // case just synthesize a fully missing try-block.  We will already have issued a diagnostic on the
        // `try` keyword, so we don't need to issue any more.
        CS_ASSERT(try_->ContainsDiagnostics());
        tryBlock = missingBlock();
    } else {
        TerminatorState saveTerm = _termState;
        _termState |= TerminatorState::IsEndOfTryBlock;
        tryBlock = ParsePossiblyAttributedBlock();
        _termState = saveTerm;
    }

    SyntaxListBuilder catchClauses;
    GreenNode *finallyClause = nullptr;
    if (CurrentToken()->kind == SyntaxKind::CatchKeyword) {
        catchClauses = _pool.Allocate();
        while (CurrentToken()->kind == SyntaxKind::CatchKeyword) {
            catchClauses.Add(ParseCatchClause());
        }
    }

    if (CurrentToken()->kind == SyntaxKind::FinallyKeyword) {
        GreenNode *finallyKeyword = EatToken();
        GreenNode *finallyBlock = ParsePossiblyAttributedBlock();
        finallyClause = SyntaxFactory::FinallyClause(finallyKeyword, finallyBlock);
    }

    if (catchClauses.IsNull() && finallyClause == nullptr) {
        if (!ContainsErrorDiagnostic(tryBlock)) tryBlock = AddErrorToLastToken(tryBlock, ErrorCode::ERR_ExpectedEndTry);

        // synthesize missing tokens for "finally { }":
        finallyClause =
            SyntaxFactory::FinallyClause(SyntaxFactory::MissingToken(SyntaxKind::FinallyKeyword), missingBlock());
    }

    // (_pool.ToListAndFree of a null builder is null.)
    GreenNode *catches = catchClauses.IsNull() ? nullptr : _pool.ToListAndFree(catchClauses);
    return SyntaxFactory::TryStatement(attributes, try_, tryBlock, catches, finallyClause);
}

bool LanguageParser::IsEndOfTryBlock() {
    SyntaxKind k = CurrentToken()->kind;
    return k == SyntaxKind::CloseBraceToken || k == SyntaxKind::CatchKeyword || k == SyntaxKind::FinallyKeyword;
}

GreenNode *LanguageParser::ParseCatchClause() {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::CatchKeyword);

    GreenNode *catch_ = EatToken();

    GreenNode *decl = nullptr;
    TerminatorState saveTerm = _termState;

    if (CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        GreenNode *openParen = EatToken();

        _termState |= TerminatorState::IsEndOfCatchClause;
        GreenNode *type = ParseType();
        GreenNode *name = nullptr;
        if (IsTrueIdentifier()) {
            name = ParseIdentifierToken();
        }

        _termState = saveTerm;

        GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
        decl = SyntaxFactory::CatchDeclaration(openParen, type, name, closeParen);
    }

    GreenNode *filter = nullptr;

    SyntaxKind keywordKind = CurrentToken()->contextual_kind;
    if (keywordKind == SyntaxKind::WhenKeyword || keywordKind == SyntaxKind::IfKeyword) {
        GreenNode *whenKeyword = EatContextualToken(SyntaxKind::WhenKeyword);
        if (keywordKind == SyntaxKind::IfKeyword) {
            // The initial design of C# exception filters called for the use of the
            // "if" keyword in this position.  We've since changed to "when", but
            // the error recovery experience for early adopters (and for old source
            // stored in the symbol server) will be better if we consume "if" as
            // though it were "when".
            whenKeyword = AddTrailingSkippedSyntax(whenKeyword, EatToken());
        }

        _termState |= TerminatorState::IsEndOfFilterClause;
        GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
        GreenNode *filterExpression = ParseExpressionForParenthesizedConstruct();

        _termState = saveTerm;
        GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
        filter = SyntaxFactory::CatchFilterClause(whenKeyword, openParen, filterExpression, closeParen);
    }

    _termState |= TerminatorState::IsEndOfCatchBlock;
    GreenNode *block = ParsePossiblyAttributedBlock();
    _termState = saveTerm;

    return SyntaxFactory::CatchClause(catch_, decl, filter, block);
}

bool LanguageParser::IsEndOfCatchClause() {
    SyntaxKind k = CurrentToken()->kind;
    return k == SyntaxKind::CloseParenToken || k == SyntaxKind::OpenBraceToken || k == SyntaxKind::CloseBraceToken ||
           k == SyntaxKind::CatchKeyword || k == SyntaxKind::FinallyKeyword;
}

bool LanguageParser::IsEndOfFilterClause() {
    SyntaxKind k = CurrentToken()->kind;
    return k == SyntaxKind::CloseParenToken || k == SyntaxKind::OpenBraceToken || k == SyntaxKind::CloseBraceToken ||
           k == SyntaxKind::CatchKeyword || k == SyntaxKind::FinallyKeyword;
}

bool LanguageParser::IsEndOfCatchBlock() {
    SyntaxKind k = CurrentToken()->kind;
    return k == SyntaxKind::CloseBraceToken || k == SyntaxKind::CatchKeyword || k == SyntaxKind::FinallyKeyword;
}

GreenNode *LanguageParser::ParseCheckedStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::CheckedKeyword ||
              CurrentToken()->kind == SyntaxKind::UncheckedKeyword);

    if (PeekToken(1)->kind == SyntaxKind::OpenParenToken) {
        return ParseExpressionStatement(attributes);
    }

    GreenNode *keyword = EatToken();
    GreenNode *block = ParsePossiblyAttributedBlock();
    return SyntaxFactory::CheckedStatement(SyntaxFacts::GetCheckStatement(keyword->kind), attributes, keyword, block);
}

GreenNode *LanguageParser::ParseDoStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::DoKeyword);
    GreenNode *do_ = EatToken(SyntaxKind::DoKeyword);
    GreenNode *statement = ParseEmbeddedStatement();
    GreenNode *while_ = EatToken(SyntaxKind::WhileKeyword);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfDoWhileExpression;
    GreenNode *expression = ParseExpressionForParenthesizedConstruct();
    _termState = saveTerm;

    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::DoStatement(attributes, do_, statement, while_, openParen, expression, closeParen,
                                      semicolon);
}

bool LanguageParser::IsEndOfDoWhileExpression() {
    return CurrentToken()->kind == SyntaxKind::CloseParenToken || CurrentToken()->kind == SyntaxKind::SemicolonToken;
}

GreenNode *LanguageParser::ParseForOrForEachStatement(SyntaxList attributes) {
    // Check if the user wrote the following accidentally:
    //
    // for (SomeType t in
    //
    // instead of
    //
    // foreach (SomeType t in
    //
    // In that case, parse it as a foreach, but given the appropriate message that a
    // 'foreach' keyword was expected.
    DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ false);

    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ForKeyword);
    EatToken();
    if (EatToken()->kind == SyntaxKind::OpenParenToken && ScanType() != ScanTypeFlags::NotType &&
        EatToken()->kind == SyntaxKind::IdentifierToken && EatToken()->kind == SyntaxKind::InKeyword) {
        // Looks like a foreach statement.  Parse it that way instead
        resetPoint.Reset();
        return ParseForEachStatement(attributes, /*awaitTokenOpt:*/ nullptr);
    } else {
        // Normal for statement.
        resetPoint.Reset();
        return ParseForStatement(attributes);
    }
}

GreenNode *LanguageParser::ParseForStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ForKeyword);

    // static PostSkipAction skipBadForStatementExpressionListTokens(...)
    SkipBadTokensFn skipBadForStatementExpressionListTokens =
        [](LanguageParser *self, GreenNode *&startToken, SyntaxListBuilder list, SyntaxKind expectedKind,
           SyntaxKind closeKind) -> PostSkipAction {
        if (self->CurrentToken()->kind == SyntaxKind::CloseParenToken ||
            self->CurrentToken()->kind == SyntaxKind::SemicolonToken)
            return PostSkipAction::Abort;

        return self->SkipBadSeparatedListTokensWithExpectedKind(
            startToken, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleExpression();
            },
            [](LanguageParser *p, SyntaxKind closeKind2) {
                return p->CurrentToken()->kind == closeKind2 || p->CurrentToken()->kind == SyntaxKind::SemicolonToken;
            },
            expectedKind, closeKind);
    };

    // Parses out a sequence of expressions.  Both for the initializer section (the `for (initializer1,
    // initializer2, ...` section), as well as the incrementor section (the `for (;; incrementor1, incrementor2,
    // ...` section).
    auto parseForStatementExpressionList = [&](GreenNode *&startToken,
                                               bool allowSemicolonAsSeparator) -> SeparatedSyntaxList {
        return ParseCommaSeparatedSyntaxList(
            startToken, SyntaxKind::CloseParenToken, [](LanguageParser *self) { return self->IsPossibleExpression(); },
            [](LanguageParser *self) { return self->ParseExpressionCore(); }, skipBadForStatementExpressionListTokens,
            /*allowTrailingSeparator:*/ false,
            /*requireOneElement:*/ false, allowSemicolonAsSeparator);
    };

    auto eatCommaOrSemicolon = [&]() -> GreenNode * {
        return CurrentToken()->kind == SyntaxKind::CommaToken ? EatTokenAsKind(SyntaxKind::SemicolonToken)
                                                              : EatToken(SyntaxKind::SemicolonToken);
    };

    auto eatUnexpectedTokensAndCloseParenToken = [&]() -> GreenNode * {
        SyntaxListBuilder skippedTokens = _pool.Allocate();

        while (CurrentToken()->kind == SyntaxKind::SemicolonToken || CurrentToken()->kind == SyntaxKind::CommaToken)
            skippedTokens.Add(EatTokenEvenWithIncorrectKind(SyntaxKind::CloseParenToken));

        GreenNode *result = EatToken(SyntaxKind::CloseParenToken);
        return AddLeadingSkippedSyntax(result, _pool.ToListAndFree(skippedTokens));
    };

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfForStatementArgument;

    GreenNode *forToken = EatToken(SyntaxKind::ForKeyword);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);

    // (VariableDeclarationSyntax variableDeclaration, SeparatedSyntaxList<ExpressionSyntax> initializers)
    //     eatVariableDeclarationOrInitializers()
    auto eatVariableDeclarationOrInitializers = [&]() -> std::pair<GreenNode *, GreenNode *> {
        DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ false);

        // Here can be either a declaration or an expression statement list.  Scan
        // for a declaration first.
        bool isDeclaration = false;

        if (CurrentToken()->contextual_kind == SyntaxKind::ScopedKeyword) {
            if (PeekToken(1)->kind == SyntaxKind::RefKeyword) {
                isDeclaration = true;
            } else {
                EatToken();
                isDeclaration = ScanType() != ScanTypeFlags::NotType &&
                                CurrentToken()->kind == SyntaxKind::IdentifierToken;
                resetPoint.Reset();
            }
        } else if (CurrentToken()->kind == SyntaxKind::RefKeyword) {
            isDeclaration = true;
        }

        if (!isDeclaration) {
            isDeclaration = !IsQueryExpression(/*mayBeVariableDeclaration:*/ true,
                                               /*mayBeMemberDeclaration:*/ false) &&
                            ScanType() != ScanTypeFlags::NotType && IsTrueIdentifier();
            resetPoint.Reset();
        }

        if (isDeclaration) {
            GreenNode *scopedKeyword =
                ParsePossibleScopedKeyword(/*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ false);
            return {ParseParenthesizedVariableDeclaration(VariableFlags::ForStatement, scopedKeyword), nullptr};
        } else if (CurrentToken()->kind != SyntaxKind::SemicolonToken) {
            // Not a type followed by an identifier, so it must be the initializer expression list.
            //
            // Do not consume semicolons here as they are used to separate the initializers out from the
            // condition of the for loop.
            return {nullptr, parseForStatementExpressionList(openParen, /*allowSemicolonAsSeparator:*/ false)};
        } else {
            return {nullptr, nullptr};
        }
    };

    auto [variableDeclaration, initializers] = eatVariableDeclarationOrInitializers();

    GreenNode *firstSemicolonToken = eatCommaOrSemicolon();
    GreenNode *condition =
        CurrentToken()->kind != SyntaxKind::SemicolonToken && CurrentToken()->kind != SyntaxKind::CommaToken
            ? ParseExpressionCore()
            : nullptr;

    // Used to place skipped tokens we run into when parsing the incrementors list.
    GreenNode *secondSemicolonToken = eatCommaOrSemicolon();

    // Do allow semicolons (with diagnostics) in the incrementors list.  This allows us to consume
    // accidental extra incrementors that should have been separated by commas.
    GreenNode *incrementors =
        CurrentToken()->kind != SyntaxKind::CloseParenToken
            ? parseForStatementExpressionList(secondSemicolonToken, /*allowSemicolonAsSeparator:*/ true).Node()
            : nullptr;

    GreenNode *closeParen = eatUnexpectedTokensAndCloseParenToken();
    GreenNode *statement = ParseEmbeddedStatement();
    GreenNode *forStatement =
        SyntaxFactory::ForStatement(attributes, forToken, openParen, variableDeclaration, initializers,
                                    firstSemicolonToken, condition, secondSemicolonToken, incrementors, closeParen,
                                    statement);

    _termState = saveTerm;

    return forStatement;
}

bool LanguageParser::IsEndOfForStatementArgument() {
    SyntaxKind k = CurrentToken()->kind;
    return k == SyntaxKind::SemicolonToken || k == SyntaxKind::CloseParenToken || k == SyntaxKind::OpenBraceToken;
}

GreenNode *LanguageParser::ParseForEachStatement(SyntaxList attributes, GreenNode *awaitTokenOpt) {
    // Can be a 'for' keyword if the user typed: 'for (SomeType t in'
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ForEachKeyword || CurrentToken()->kind == SyntaxKind::ForKeyword);

    // Syntax for foreach is either:
    //  foreach [await] ( <type> <identifier> in <expr> ) <embedded-statement>
    // or
    //  foreach [await] ( <deconstruction-declaration> in <expr> ) <embedded-statement>

    GreenNode *foreach_;

    // If we're at a 'for', then consume it and attach
    // it as skipped text to the missing 'foreach' token.
    if (CurrentToken()->kind == SyntaxKind::ForKeyword) {
        GreenNode *skippedForToken = EatToken();
        skippedForToken = AddError(skippedForToken, ErrorCode::ERR_SyntaxError,
                                   {SyntaxFacts::GetText(SyntaxKind::ForEachKeyword)});
        foreach_ = ConvertToMissingWithTrailingTrivia(skippedForToken, SyntaxKind::ForEachKeyword);
    } else {
        foreach_ = EatToken(SyntaxKind::ForEachKeyword);
    }

    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);

    GreenNode *variable = ParseExpressionOrDeclaration(ParseTypeMode::Normal, /*permitTupleDesignation:*/ true);
    GreenNode *in_ = EatToken(SyntaxKind::InKeyword, ErrorCode::ERR_InExpected);
    if (!IsValidForeachVariable(variable)) {
        in_ = AddError(in_, ErrorCode::ERR_BadForeachDecl);
    }

    GreenNode *expression = ParseExpressionCore();
    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *statement = ParseEmbeddedStatement();

    if (variable->kind == SyntaxKind::DeclarationExpression) {
        GreenNode *decl = variable;
        GreenNode *designation = GetField(decl, Field::Designation);
        if (designation->kind != SyntaxKind::ParenthesizedVariableDesignation) {
            // if we see a foreach declaration that isn't a deconstruction, we use the old form of foreach syntax
            // node.
            GreenNode *identifier = nullptr;
            switch (designation->kind) {
            case SyntaxKind::SingleVariableDesignation:
                identifier = GetField(designation, Field::Identifier);
                break;
            case SyntaxKind::DiscardDesignation: {
                // revert the identifier from its contextual underscore back to an identifier.
                GreenNode *discard = GetField(designation, Field::UnderscoreToken);
                CS_ASSERT(discard->kind == SyntaxKind::UnderscoreToken);
                identifier =
                    SyntaxFactory::TokenWithValue(SyntaxKind::IdentifierToken, discard->GetLeadingTrivia(),
                                                  discard->text, discard->value_text, nullptr,
                                                  discard->GetTrailingTrivia());
                break;
            }
            default:
                // throw ExceptionUtilities.UnexpectedValue(decl.designation.Kind) -- unreachable.
                CS_ASSERT(false);
                break;
            }

            return SyntaxFactory::ForEachStatement(attributes, awaitTokenOpt, foreach_, openParen,
                                                   GetField(decl, Field::Type), identifier, in_, expression,
                                                   closeParen, statement);
        }
    }

    return SyntaxFactory::ForEachVariableStatement(attributes, awaitTokenOpt, foreach_, openParen, variable, in_,
                                                   expression, closeParen, statement);
}

//
// Parse an expression where a declaration expression would be permitted. This is suitable for use after
// the `out` keyword in an argument list, or in the elements of a tuple literal (because they may
// be on the left-hand-side of a positional subpattern). The first element of a tuple is handled slightly
// differently, as we check for the comma before concluding that the identifier should cause a
// disambiguation. (See the C# for the full comment, and ScanTypeArgumentList where these disambiguation rules
// are encoded.)
//
GreenNode *LanguageParser::ParseExpressionOrDeclaration(ParseTypeMode mode, bool permitTupleDesignation) {
    bool isScoped = false;
    return IsPossibleDeclarationExpression(mode, permitTupleDesignation, isScoped)
               ? ParseDeclarationExpression(mode, isScoped)
               : ParseSubExpression(Precedence::Expression);
}

bool LanguageParser::IsPossibleDeclarationExpression(ParseTypeMode mode, bool permitTupleDesignation,
                                                     bool &isScoped) {
    CS_ASSERT(mode == ParseTypeMode::Normal || mode == ParseTypeMode::FirstElementOfPossibleTupleLiteral ||
              mode == ParseTypeMode::AfterTupleComma);
    isScoped = false;

    if (IsInAsync() && CurrentToken()->contextual_kind == SyntaxKind::AwaitKeyword) {
        // can't be a declaration expression.
        return false;
    }

    DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ true);

    if (CurrentToken()->contextual_kind == SyntaxKind::ScopedKeyword) {
        EatToken();
        if (ScanType() != ScanTypeFlags::NotType && CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            switch (mode) {
            case ParseTypeMode::FirstElementOfPossibleTupleLiteral:
                if (PeekToken(1)->kind == SyntaxKind::CommaToken) {
                    isScoped = true;
                    return true;
                }
                break;

            case ParseTypeMode::AfterTupleComma:
                if (PeekToken(1)->kind == SyntaxKind::CommaToken || PeekToken(1)->kind == SyntaxKind::CloseParenToken) {
                    isScoped = true;
                    return true;
                }
                break;

            default:
                // The other case where we disambiguate between a declaration and expression is before the `in` of
                // a foreach loop. There we err on the side of accepting a declaration.
                isScoped = true;
                return true;
            }
        }

        resetPoint.Reset();
    }

    bool typeIsVar = IsVarType();
    GreenNode *lastTokenOfType = nullptr;
    if (ScanType(mode, lastTokenOfType) == ScanTypeFlags::NotType) {
        return false;
    }

    // check for a designation
    if (!ScanDesignation(permitTupleDesignation && (typeIsVar || IsPredefinedType(lastTokenOfType->kind)))) {
        return false;
    }

    switch (mode) {
    case ParseTypeMode::FirstElementOfPossibleTupleLiteral:
        return CurrentToken()->kind == SyntaxKind::CommaToken;
    case ParseTypeMode::AfterTupleComma:
        return CurrentToken()->kind == SyntaxKind::CommaToken || CurrentToken()->kind == SyntaxKind::CloseParenToken;
    default:
        // The other case where we disambiguate between a declaration and expression is before the `in` of a
        // foreach loop. There we err on the side of accepting a declaration.
        return true;
    }
}

/// Is the following set of tokens, interpreted as a type, the type `var`?
bool LanguageParser::IsVarType() {
    if (!SyntaxFacts::IsIdentifierVar(CurrentToken())) {
        return false;
    }

    switch (PeekToken(1)->kind) {
    case SyntaxKind::DotToken:
    case SyntaxKind::ColonColonToken:
    case SyntaxKind::OpenBracketToken:
    case SyntaxKind::AsteriskToken:
    case SyntaxKind::QuestionToken:
    case SyntaxKind::LessThanToken:
        return false;
    default:
        return true;
    }
}

bool LanguageParser::IsValidForeachVariable(GreenNode *variable) {
    switch (variable->kind) {
    case SyntaxKind::DeclarationExpression:
        // e.g. `foreach (var (x, y) in e)`
        return true;
    case SyntaxKind::TupleExpression:
        // e.g. `foreach ((var x, var y) in e)`
        return true;
    case SyntaxKind::IdentifierName:
        // e.g. `foreach (_ in e)`
        return GetField(variable, Field::Identifier)->contextual_kind == SyntaxKind::UnderscoreToken;
    default:
        return false;
    }
}

GreenNode *LanguageParser::ParseGotoStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::GotoKeyword);

    GreenNode *goto_ = EatToken(SyntaxKind::GotoKeyword);

    GreenNode *caseOrDefault = nullptr;
    GreenNode *arg = nullptr;
    SyntaxKind kind;

    if (CurrentToken()->kind == SyntaxKind::CaseKeyword || CurrentToken()->kind == SyntaxKind::DefaultKeyword) {
        caseOrDefault = EatToken();
        if (caseOrDefault->kind == SyntaxKind::CaseKeyword) {
            kind = SyntaxKind::GotoCaseStatement;
            arg = ParseExpressionCore();
        } else {
            kind = SyntaxKind::GotoDefaultStatement;
        }
    } else {
        kind = SyntaxKind::GotoStatement;
        arg = ParseIdentifierName();
    }

    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::GotoStatement(kind, attributes, goto_, caseOrDefault, arg, semicolon);
}

GreenNode *LanguageParser::ParseExpressionForParenthesizedConstruct() {
    return ParseErrantExpressionWhenNoCloseParenToken(ParseExpressionCore());
}

GreenNode *LanguageParser::ParseErrantExpressionWhenNoCloseParenToken(GreenNode *expression) {
    if (CurrentToken()->kind != SyntaxKind::CloseParenToken) {
        // Look for common case of `(a b)` where the user is in the middle of updating the expression to
        // something like `(a && b)`.  We want to recover well here as otherwise the subsequent close paren will
        // cause problems for higher level constructs.
        //
        // Note if we see `(a b) =>` we don't do this as it's much more likely this is some incomplete lambda
        // expression.
        DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ false);
        GreenNode *nextExpression = ParseExpressionCore();

        if (CurrentToken()->kind == SyntaxKind::CloseParenToken &&
            PeekToken(1)->kind != SyntaxKind::EqualsGreaterThanToken &&
            !nextExpression->GetLastToken()->IsMissing()) {
            expression = AddTrailingSkippedSyntax(
                expression, AddErrorToFirstToken(nextExpression, ErrorCode::ERR_UnexpectedToken,
                                                 {nextExpression->GetFirstToken()->text}));
        } else {
            resetPoint.Reset();
        }
    }

    return expression;
}

GreenNode *LanguageParser::ParseIfStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::IfKeyword);

    struct IfParts {
        GreenNode *ifKeyword;
        GreenNode *openParen;
        GreenNode *condition;
        GreenNode *closeParen;
        GreenNode *consequence;
        GreenNode *elseKeyword;
    };
    std::vector<IfParts> stack;

    GreenNode *alternative = nullptr;
    while (true) {
        GreenNode *ifKeyword = EatToken(SyntaxKind::IfKeyword);
        GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
        GreenNode *condition = ParseExpressionForParenthesizedConstruct();
        GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
        GreenNode *consequence = ParseEmbeddedStatement();

        GreenNode *elseKeyword =
            CurrentToken()->kind != SyntaxKind::ElseKeyword ? nullptr : EatToken(SyntaxKind::ElseKeyword);
        stack.push_back({ifKeyword, openParen, condition, closeParen, consequence, elseKeyword});

        if (elseKeyword == nullptr) {
            alternative = nullptr;
            break;
        }

        if (CurrentToken()->kind != SyntaxKind::IfKeyword) {
            alternative = ParseEmbeddedStatement();
            break;
        }

        alternative = TryReuseStatement(/*attributes:*/ nullptr, /*isGlobal:*/ false);
        if (alternative != nullptr) {
            break;
        }
    }

    GreenNode *ifStatement;
    do {
        IfParts p = stack.back();
        stack.pop_back();
        GreenNode *elseClause = alternative == nullptr ? nullptr : SyntaxFactory::ElseClause(p.elseKeyword, alternative);
        ifStatement = SyntaxFactory::IfStatement(!stack.empty() ? SyntaxList() : attributes, p.ifKeyword,
                                                 p.openParen, p.condition, p.closeParen, p.consequence, elseClause);
        alternative = ifStatement;
    } while (!stack.empty());

    return ifStatement;
}

GreenNode *LanguageParser::ParseMisplacedElse(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ElseKeyword);

    GreenNode *ifKeyword = EatToken(SyntaxKind::IfKeyword, ErrorCode::ERR_ElseCannotStartStatement);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
    GreenNode *condition = ParseExpressionCore();
    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *statement = ParseExpressionStatement(nullptr);
    GreenNode *else_ = ParseElseClauseOpt();
    return SyntaxFactory::IfStatement(attributes, ifKeyword, openParen, condition, closeParen, statement, else_);
}

GreenNode *LanguageParser::ParseElseClauseOpt() {
    if (CurrentToken()->kind != SyntaxKind::ElseKeyword) return nullptr;
    GreenNode *elseKeyword = EatToken(SyntaxKind::ElseKeyword);
    GreenNode *statement = ParseEmbeddedStatement();
    return SyntaxFactory::ElseClause(elseKeyword, statement);
}

GreenNode *LanguageParser::ParseLockStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::LockKeyword);
    GreenNode *lockKeyword = EatToken(SyntaxKind::LockKeyword);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
    GreenNode *expression = ParseExpressionForParenthesizedConstruct();
    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *statement = ParseEmbeddedStatement();
    return SyntaxFactory::LockStatement(attributes, lockKeyword, openParen, expression, closeParen, statement);
}

GreenNode *LanguageParser::ParseReturnStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ReturnKeyword);
    GreenNode *returnKeyword = EatToken(SyntaxKind::ReturnKeyword);
    GreenNode *expression =
        CurrentToken()->kind != SyntaxKind::SemicolonToken ? ParsePossibleRefExpression() : nullptr;
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::ReturnStatement(attributes, returnKeyword, expression, semicolon);
}

GreenNode *LanguageParser::ParseYieldStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->contextual_kind == SyntaxKind::YieldKeyword);

    GreenNode *yieldToken = ConvertToKeyword(EatToken());
    GreenNode *returnOrBreak;
    GreenNode *arg = nullptr;
    SyntaxKind kind;

    if (CurrentToken()->kind == SyntaxKind::BreakKeyword) {
        kind = SyntaxKind::YieldBreakStatement;
        returnOrBreak = EatToken();
    } else {
        kind = SyntaxKind::YieldReturnStatement;
        returnOrBreak = EatToken(SyntaxKind::ReturnKeyword);
        if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
            returnOrBreak = AddError(returnOrBreak, ErrorCode::ERR_EmptyYield);
        } else {
            arg = ParseExpressionCore();
        }
    }

    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::YieldStatement(kind, attributes, yieldToken, returnOrBreak, arg, semicolon);
}

GreenNode *LanguageParser::ParseSwitchStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::SwitchKeyword || CurrentToken()->kind == SyntaxKind::CaseKeyword);

    GreenNode *switchKeyword;
    GreenNode *openParen;
    GreenNode *expression;
    GreenNode *closeParen;
    GreenNode *openBrace;

    // parseSwitchHeader(out switchKeyword, out openParen, out expression, out closeParen, out openBrace)
    if (CurrentToken()->kind == SyntaxKind::CaseKeyword) {
        // try to eat a 'switch' so the user gets a good error message about what's wrong. then directly
        // creating missing tokens for the rest so they don't get cascading errors.
        switchKeyword = EatToken(SyntaxKind::SwitchKeyword);
        openParen = SyntaxFactory::MissingToken(SyntaxKind::OpenParenToken);
        expression = CreateMissingIdentifierName();
        closeParen = SyntaxFactory::MissingToken(SyntaxKind::CloseParenToken);
        openBrace = SyntaxFactory::MissingToken(SyntaxKind::OpenBraceToken);
    } else {
        switchKeyword = EatToken(SyntaxKind::SwitchKeyword);
        expression = ParseExpressionCore();
        if (expression->kind == SyntaxKind::ParenthesizedExpression) {
            GreenNode *parenExpression = expression;
            openParen = GetField(parenExpression, Field::OpenParenToken);
            expression = GetField(parenExpression, Field::Expression);
            closeParen = GetField(parenExpression, Field::CloseParenToken);

            CS_ASSERT(parenExpression->diag_count == 0);
        } else if (expression->kind == SyntaxKind::TupleExpression) {
            // As a special case, when a tuple literal is the governing expression of
            // a switch statement we permit the switch statement's own parentheses to be omitted.
            // LDM 2018-04-04.
            openParen = closeParen = nullptr;
        } else {
            // Some other expression has appeared without parens. Give a syntax error.
            openParen = SyntaxFactory::MissingToken(SyntaxKind::OpenParenToken);
            expression = AddError(expression, ErrorCode::ERR_SwitchGoverningExpressionRequiresParens);
            closeParen = SyntaxFactory::MissingToken(SyntaxKind::CloseParenToken);
        }

        openBrace = EatToken(SyntaxKind::OpenBraceToken);
    }

    SyntaxListBuilder sections = _pool.Allocate();

    while (IsPossibleSwitchSection()) sections.Add(ParseSwitchSection());

    GreenNode *sectionList = _pool.ToListAndFree(sections);
    GreenNode *closeBrace = EatToken(SyntaxKind::CloseBraceToken);
    return SyntaxFactory::SwitchStatement(attributes, switchKeyword, openParen, expression, closeParen, openBrace,
                                          sectionList, closeBrace);
}

bool LanguageParser::IsPossibleSwitchSection() {
    return CurrentToken()->kind == SyntaxKind::CaseKeyword ||
           (CurrentToken()->kind == SyntaxKind::DefaultKeyword && PeekToken(1)->kind != SyntaxKind::OpenParenToken);
}

GreenNode *LanguageParser::ParseSwitchSection() {
    CS_ASSERT(IsPossibleSwitchSection());

    // First, parse case label(s)
    SyntaxListBuilder labels = _pool.Allocate();
    SyntaxListBuilder statements = _pool.Allocate();

    do {
        GreenNode *label;
        if (CurrentToken()->kind == SyntaxKind::CaseKeyword) {
            GreenNode *caseKeyword = EatToken();

            if (CurrentToken()->kind == SyntaxKind::ColonToken) {
                GreenNode *value = ParseIdentifierName(ErrorCode::ERR_ConstantExpected);
                GreenNode *colon = EatToken(SyntaxKind::ColonToken);
                label = SyntaxFactory::CaseSwitchLabel(caseKeyword, value, colon);
            } else {
                GreenNode *node = ParseExpressionOrPatternForSwitchStatement();

                // if there is a 'when' token, we treat a case expression as a constant pattern.
                if (CurrentToken()->contextual_kind == SyntaxKind::WhenKeyword &&
                    KindDerivesFrom(node->kind, "ExpressionSyntax"))
                    node = SyntaxFactory::ConstantPattern(node);

                if (node->kind == SyntaxKind::DiscardPattern)
                    node = AddError(node, ErrorCode::ERR_DiscardPatternInSwitchStatement);

                if (KindDerivesFrom(node->kind, "PatternSyntax")) {
                    GreenNode *whenClause = ParseWhenClause(Precedence::Expression);
                    GreenNode *colon = EatToken(SyntaxKind::ColonToken);
                    label = SyntaxFactory::CasePatternSwitchLabel(caseKeyword, node, whenClause, colon);
                } else {
                    GreenNode *colon = EatToken(SyntaxKind::ColonToken);
                    label = SyntaxFactory::CaseSwitchLabel(caseKeyword, node, colon);
                }
            }
        } else {
            CS_ASSERT(CurrentToken()->kind == SyntaxKind::DefaultKeyword);
            GreenNode *defaultKeyword = EatToken(SyntaxKind::DefaultKeyword);
            GreenNode *colon = EatToken(SyntaxKind::ColonToken);
            label = SyntaxFactory::DefaultSwitchLabel(defaultKeyword, colon);
        }

        labels.Add(label);
    } while (IsPossibleSwitchSection());

    // Next, parse statement list stopping for new sections
    GreenNode *tmp = labels[labels.Count() - 1];
    ParseStatements(tmp, statements, /*stopOnSwitchSections:*/ true);
    labels[labels.Count() - 1] = tmp;

    GreenNode *labelList = _pool.ToListAndFree(labels);
    GreenNode *statementList = _pool.ToListAndFree(statements);
    return SyntaxFactory::SwitchSection(labelList, statementList);
}

GreenNode *LanguageParser::ParseThrowStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ThrowKeyword);
    GreenNode *throwKeyword = EatToken(SyntaxKind::ThrowKeyword);
    GreenNode *expression = CurrentToken()->kind != SyntaxKind::SemicolonToken ? ParseExpressionCore() : nullptr;
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::ThrowStatement(attributes, throwKeyword, expression, semicolon);
}

GreenNode *LanguageParser::ParseUnsafeStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::UnsafeKeyword);
    GreenNode *unsafeKeyword = EatToken(SyntaxKind::UnsafeKeyword);
    GreenNode *block = ParsePossiblyAttributedBlock();
    return SyntaxFactory::UnsafeStatement(attributes, unsafeKeyword, block);
}

GreenNode *LanguageParser::ParseUsingStatement(SyntaxList attributes, GreenNode *awaitTokenOpt) {
    GreenNode *using_ = EatToken(SyntaxKind::UsingKeyword);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);

    GreenNode *declaration = nullptr;
    GreenNode *expression = nullptr;

    ResetPoint resetPoint = GetResetPoint();
    ParseUsingExpression(declaration, expression, resetPoint);
    Release(resetPoint);

    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *statement = ParseEmbeddedStatement();
    return SyntaxFactory::UsingStatement(attributes, awaitTokenOpt, using_, openParen, declaration, expression,
                                         closeParen, statement);
}

template <class RP>
void LanguageParser::ParseUsingExpression(GreenNode *&declaration, GreenNode *&expression, RP &resetPoint) {
    if (IsAwaitExpression()) {
        expression = ParseExpressionCore();
        return;
    }

    // Now, this can be either an expression or a decl list

    ScanTypeFlags st;

    if (IsQueryExpression(/*mayBeVariableDeclaration:*/ true, /*mayBeMemberDeclaration:*/ false)) {
        st = ScanTypeFlags::NotType;
    } else {
        GreenNode *scopedKeyword =
            ParsePossibleScopedKeyword(/*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ false);

        if (scopedKeyword != nullptr) {
            declaration = ParseParenthesizedVariableDeclaration(VariableFlags::None, scopedKeyword);
            return;
        } else {
            st = ScanType();
        }
    }

    if (st == ScanTypeFlags::NullableType) {
        // We need to handle:
        // * using (f ? x = a : x = b)
        // * using (f ? x = a)
        // * using (f ? x, y)

        if (CurrentToken()->kind != SyntaxKind::IdentifierToken) {
            Reset(resetPoint);
            expression = ParseExpressionCore();
        } else {
            switch (PeekToken(1)->kind) {
            default:
                Reset(resetPoint);
                expression = ParseExpressionCore();
                break;

            case SyntaxKind::CommaToken:
            case SyntaxKind::CloseParenToken:
                Reset(resetPoint);
                declaration = ParseParenthesizedVariableDeclaration(VariableFlags::None, /*scopedKeyword:*/ nullptr);
                break;

            case SyntaxKind::EqualsToken:
                // Parse it as a decl. If the next token is a : and only one variable was parsed,
                // convert the whole thing to ?: expression.
                Reset(resetPoint);
                declaration = ParseParenthesizedVariableDeclaration(VariableFlags::None, /*scopedKeyword:*/ nullptr);

                // We may have non-nullable types in error scenarios.
                {
                    GreenNode *declType = GetField(declaration, Field::Type);
                    if (CurrentToken()->kind == SyntaxKind::ColonToken && declType->kind == SyntaxKind::NullableType &&
                        SyntaxFacts::IsName(GetField(declType, Field::ElementType)->kind) &&
                        SeparatedSyntaxList(GetField(declaration, Field::Variables)).Count() == 1) {
                        // We have "name? id = expr :" so need to convert to a ?: expression.
                        Reset(resetPoint);
                        declaration = nullptr;
                        expression = ParseExpressionCore();
                    }
                }

                break;
            }
        }
    } else if (IsUsingStatementVariableDeclaration(st)) {
        Reset(resetPoint);
        declaration = ParseParenthesizedVariableDeclaration(VariableFlags::None, /*scopedKeyword:*/ nullptr);
    } else {
        // Must be an expression statement
        Reset(resetPoint);
        expression = ParseExpressionCore();
    }
}

bool LanguageParser::IsUsingStatementVariableDeclaration(ScanTypeFlags st) {
    CS_ASSERT(st != ScanTypeFlags::NullableType);

    bool condition1 = st == ScanTypeFlags::MustBeType && CurrentToken()->kind != SyntaxKind::DotToken;
    bool condition2 = st != ScanTypeFlags::NotType && CurrentToken()->kind == SyntaxKind::IdentifierToken;
    bool condition3 = st == ScanTypeFlags::NonGenericTypeOrExpression || PeekToken(1)->kind == SyntaxKind::EqualsToken;

    return condition1 || (condition2 && condition3);
}

GreenNode *LanguageParser::ParseWhileStatement(SyntaxList attributes) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::WhileKeyword);
    GreenNode *whileKeyword = EatToken(SyntaxKind::WhileKeyword);
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
    GreenNode *condition = ParseExpressionForParenthesizedConstruct();
    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *statement = ParseEmbeddedStatement();
    return SyntaxFactory::WhileStatement(attributes, whileKeyword, openParen, condition, closeParen, statement);
}

GreenNode *LanguageParser::ParseLabeledStatement(SyntaxList attributes) {
    // We have an identifier followed by a colon. But if the identifier is a contextual keyword in a query context,
    // ParseIdentifier will result in a missing name and Eat(Colon) will fail. We won't make forward progress.
    CS_ASSERT(IsTrueIdentifier() && PeekToken(1)->kind == SyntaxKind::ColonToken);

    GreenNode *identifier = ParseIdentifierToken();
    GreenNode *colon = EatToken(SyntaxKind::ColonToken);
    GreenNode *statement = ParsePossiblyAttributedStatement();
    if (statement == nullptr) statement = SyntaxFactory::EmptyStatement(nullptr, EatToken(SyntaxKind::SemicolonToken));
    return SyntaxFactory::LabeledStatement(attributes, identifier, colon, statement);
}

/// Parses any kind of local declaration statement: local variable or local function.
GreenNode *LanguageParser::ParseLocalDeclarationStatement(SyntaxList attributes) {
    GreenNode *awaitKeyword;
    GreenNode *usingKeyword;
    bool canParseAsLocalFunction = false;
    if (IsPossibleAwaitUsing()) {
        awaitKeyword = EatContextualToken(SyntaxKind::AwaitKeyword);
        usingKeyword = EatToken();
    } else if (CurrentToken()->kind == SyntaxKind::UsingKeyword) {
        awaitKeyword = nullptr;
        usingKeyword = EatToken();
    } else {
        awaitKeyword = nullptr;
        usingKeyword = nullptr;
        canParseAsLocalFunction = true;
    }

    SyntaxListBuilder mods = _pool.Allocate();
    ParseLocalDeclarationStatementModifiers(mods, /*isUsingDeclaration:*/ usingKeyword != nullptr);

    SyntaxListBuilder variables = _pool.AllocateSeparated();
    Finally finally_([this, variables, mods] {
        _pool.Free(variables);
        _pool.Free(mods);
    });

    GreenNode *scopedKeyword =
        ParsePossibleScopedKeyword(/*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ false);

    // For local functions, 'scoped' is a modifier in LocalFunctionStatementSyntax
    if (scopedKeyword != nullptr) {
        mods.Add(scopedKeyword);
    }

    GreenNode *type = nullptr;
    GreenNode *localFunction = nullptr;
    ParseLocalDeclaration(variables,
                          /*allowLocalFunctions:*/ canParseAsLocalFunction,
                          // A local declaration doesn't have a `(...)` construct.  So no need to stop if we hit a
                          // close paren after a declarator.  Let normal error recovery kick in.
                          /*stopOnCloseParen:*/ false, attributes, mods.ToList(),
                          /*scopedKeyword:*/ nullptr,
                          /*initialFlags:*/ VariableFlags::None, type, localFunction);

    if (localFunction != nullptr) {
        CS_ASSERT(variables.Count() == 0);
        return localFunction;
    }

    if (canParseAsLocalFunction) {
        // If we find an accessibility modifier but no local function it's likely
        // the user forgot a closing brace. Let's back out of statement parsing.
        // We check just for a leading accessibility modifier in the syntax because
        // SkipBadStatementListTokens will not skip attribute lists.
        if (attributes.Count() == 0 && mods.Count() > 0 && IsAccessibilityModifier(mods[0]->contextual_kind)) {
            return nullptr;
        }
    }

    // For locals, 'scoped' is part of ScopedTypeSyntax.
    if (scopedKeyword != nullptr) {
        mods.RemoveLast();
        type = SyntaxFactory::ScopedType(scopedKeyword, type);
    }

    // We've already reported all modifiers for local_using_declaration as errors
    if (usingKeyword == nullptr) {
        for (int i = 0; i < mods.Count(); i++) {
            GreenNode *mod = mods[i];

            if (IsAdditionalLocalFunctionModifier(mod->contextual_kind)) {
                mods[i] = AddError(mod, ErrorCode::ERR_BadMemberFlag, {mod->text});
            }
        }
    }

    GreenNode *modifierList = mods.ToList();
    GreenNode *declaration = SyntaxFactory::VariableDeclaration(type, variables.ToList());
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::LocalDeclarationStatement(attributes, awaitKeyword, usingKeyword, modifierList,
                                                    declaration, semicolon);
}

bool LanguageParser::IsDefiniteScopedModifier(bool isFunctionPointerParameter, bool isLambdaParameter) {
    if (CurrentToken()->contextual_kind != SyntaxKind::ScopedKeyword) return false;

    // In C# 14 we decided that within a lambda 'scoped' would *always* be a modifier, not a type.
    // so `scoped scoped` is `modifier-scoped identifier-scoped` not `type-scoped identifier-scoped`.
    // Note: this only applies the modifier/type portion.  We still allow the identifier of a lambda
    // to be named 'scoped'.  (IDS_FeatureSimpleLambdaParameterModifiers is enabled.)
    if (isLambdaParameter) return true;

    DisposableResetPoint beforeScopedResetPoint(this, /*resetOnDispose:*/ true);

    EatContextualToken(SyntaxKind::ScopedKeyword);

    // trivial case.  scoped ref/out/in/this  is definitely the scoped keyword. Note: the only actual legal
    // cases are `scoped ref`, `scoped out`, and `scoped in`.  But we detect and allow `scoped this`, `scoped
    // params` and `scoped readonly` as well.  These will be reported as errors later in binding.
    if (IsParameterModifierExcludingScoped(CurrentToken())) return true;

    // Had `scoped Type ...`
    //
    // 1. This is a function pointer `delegate<T1, scoped T2>`
    // 2. this is a parameter `scoped T x`.
    auto isValidScopedTypeCase = [&]() -> bool {
        if (isFunctionPointerParameter) {
            return CurrentToken()->kind == SyntaxKind::CommaToken || CurrentToken()->kind == SyntaxKind::GreaterThanToken;
        } else if (CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            return true;
        }

        return false;
    };

    // More complex cases.  We have to check for `scoped Type ...` now.
    //
    // Note that `scoped scoped` can be valid here as a type called scoped and a variable called scoped.
    return ScanType() != ScanTypeFlags::NotType && isValidScopedTypeCase();
}

GreenNode *LanguageParser::ParsePossibleScopedKeyword(bool isFunctionPointerParameter, bool isLambdaParameter) {
    return IsDefiniteScopedModifier(isFunctionPointerParameter, isLambdaParameter)
               ? EatContextualToken(SyntaxKind::ScopedKeyword)
               : nullptr;
}

GreenNode *LanguageParser::ParseDesignation(bool forPattern) {
    // the two forms of designation are
    // (1) identifier
    // (2) ( designation ... )
    // for pattern-matching, we permit the designation list to be empty
    GreenNode *result;
    if (CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
        SyntaxListBuilder listOfDesignations = _pool.AllocateSeparated();

        bool done = false;
        if (forPattern) {
            done = (CurrentToken()->kind == SyntaxKind::CloseParenToken);
        } else {
            listOfDesignations.Add(ParseDesignation(forPattern));
            listOfDesignations.AddSeparator(EatToken(SyntaxKind::CommaToken));
        }

        if (!done) {
            while (true) {
                listOfDesignations.Add(ParseDesignation(forPattern));
                if (CurrentToken()->kind == SyntaxKind::CommaToken) {
                    listOfDesignations.AddSeparator(EatToken(SyntaxKind::CommaToken));
                } else {
                    break;
                }
            }
        }

        GreenNode *designations = _pool.ToListAndFree(listOfDesignations);
        GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
        result = SyntaxFactory::ParenthesizedVariableDesignation(openParen, designations, closeParen);
    } else {
        result = ParseSimpleDesignation();
    }

    return result;
}

/// Parse a single variable designation (e.g. `x`) or a wildcard designation (e.g. `_`)
GreenNode *LanguageParser::ParseSimpleDesignation() {
    return CurrentToken()->contextual_kind == SyntaxKind::UnderscoreToken
               ? SyntaxFactory::DiscardDesignation(EatContextualToken(SyntaxKind::UnderscoreToken))
               : SyntaxFactory::SingleVariableDesignation(EatToken(SyntaxKind::IdentifierToken));
}

GreenNode *LanguageParser::ParseWhenClause(Precedence precedence) {
    if (CurrentToken()->contextual_kind != SyntaxKind::WhenKeyword) {
        return nullptr;
    }

    GreenNode *whenKeyword = EatContextualToken(SyntaxKind::WhenKeyword);
    GreenNode *condition = ParseSubExpression(precedence);
    return SyntaxFactory::WhenClause(whenKeyword, condition);
}

/// Parse a local variable declaration for constructs where the variable declaration is enclosed in parentheses.
/// Specifically, only for the `fixed (...)` `for(...)` or `using (...)` statements.
GreenNode *LanguageParser::ParseParenthesizedVariableDeclaration(VariableFlags initialFlags,
                                                                 GreenNode *scopedKeyword) {
    SyntaxListBuilder variables = _pool.AllocateSeparated();
    GreenNode *type = nullptr;
    GreenNode *localFunction = nullptr;
    ParseLocalDeclaration(variables,
                          /*allowLocalFunctions:*/ false,
                          // Always stop on a close paren as the parent `fixed(...)/for(...)/using(...)` statement
                          // wants to consume it.
                          /*stopOnCloseParen:*/ true,
                          /*attributes:*/ nullptr,
                          /*mods:*/ nullptr, scopedKeyword, initialFlags, type, localFunction);
    CS_ASSERT(localFunction == nullptr);
    return SyntaxFactory::VariableDeclaration(type, _pool.ToListAndFree(variables));
}

void LanguageParser::ParseLocalDeclaration(SyntaxListBuilder variables, bool allowLocalFunctions,
                                           bool stopOnCloseParen, SyntaxList attributes, SyntaxList mods,
                                           GreenNode *scopedKeyword, VariableFlags initialFlags, GreenNode *&type,
                                           GreenNode *&localFunction) {
    type = allowLocalFunctions ? ParseReturnType() : ParseType();

    if (scopedKeyword != nullptr) type = SyntaxFactory::ScopedType(scopedKeyword, type);

    VariableFlags flags = initialFlags | VariableFlags::LocalOrField;
    if (mods.Any(SyntaxKind::ConstKeyword)) {
        flags = flags | VariableFlags::Const;
    }

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfDeclarationClause;
    ParseVariableDeclarators(type, flags, variables,
                             /*variableDeclarationsExpected:*/ true, allowLocalFunctions, stopOnCloseParen,
                             attributes, mods, localFunction);
    _termState = saveTerm;

    if (allowLocalFunctions && localFunction == nullptr && type->kind == SyntaxKind::PredefinedType &&
        GetField(type, Field::Keyword)->kind == SyntaxKind::VoidKeyword) {
        type = AddError(type, ErrorCode::ERR_NoVoidHere);
    }
}

bool LanguageParser::IsEndOfDeclarationClause() {
    switch (CurrentToken()->kind) {
    case SyntaxKind::SemicolonToken:
    case SyntaxKind::ColonToken:
        return true;
    default:
        return false;
    }
}

void LanguageParser::ParseLocalDeclarationStatementModifiers(SyntaxListBuilder list, bool isUsingDeclaration) {
    auto shouldTreatAsModifier = [&]() -> bool {
        DisposableResetPoint _(this, /*resetOnDispose:*/ true);

        CS_ASSERT(CurrentToken()->kind == SyntaxKind::IdentifierToken);

        do {
            EatToken();

            if (IsDeclarationModifier(CurrentToken()->kind) || IsAdditionalLocalFunctionModifier(CurrentToken()->kind)) {
                return true;
            }

            DisposableResetPoint _2(this, /*resetOnDispose:*/ true);

            if (ScanType() != ScanTypeFlags::NotType && CurrentToken()->kind == SyntaxKind::IdentifierToken) {
                return true;
            }
        }
        // If current token might be a contextual modifier we need to check ahead the next token after it
        // If the next token appears to be a modifier, we treat current token as a modifier as well
        // This allows to correctly parse things like local functions with several `async` modifiers
        while (IsAdditionalLocalFunctionModifier(CurrentToken()->contextual_kind));

        return false;
    };

    SyntaxKind k;
    while (IsDeclarationModifier(k = CurrentToken()->contextual_kind) || IsAdditionalLocalFunctionModifier(k)) {
        GreenNode *mod;
        if (k == SyntaxKind::AsyncKeyword) {
            // check for things like "async async()" where async is the type and/or the function name
            if (!shouldTreatAsModifier()) {
                break;
            }

            mod = EatContextualToken(k);
        } else {
            mod = EatToken();
        }

        if (isUsingDeclaration) {
            mod = AddError(mod, ErrorCode::ERR_NoModifiersOnUsing);
        } else if (k == SyntaxKind::ReadOnlyKeyword || k == SyntaxKind::VolatileKeyword) {
            mod = AddError(mod, ErrorCode::ERR_BadMemberFlag, {mod->text});
        }

        // Note: Duplicate modifiers are not reported here during parsing.
        // They will be reported during binding (see Binder_Statements.BindDeclarationStatementParts).

        list.Add(mod);
    }
}

bool LanguageParser::IsDeclarationModifier(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::ConstKeyword:
    case SyntaxKind::StaticKeyword:
    case SyntaxKind::ReadOnlyKeyword:
    case SyntaxKind::VolatileKeyword:
        return true;
    default:
        return false;
    }
}

bool LanguageParser::IsAdditionalLocalFunctionModifier(SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::StaticKeyword:
    case SyntaxKind::AsyncKeyword:
    case SyntaxKind::UnsafeKeyword:
    case SyntaxKind::ExternKeyword:
    // Not a valid modifier, but we should parse to give a good
    // error message
    case SyntaxKind::PublicKeyword:
    case SyntaxKind::InternalKeyword:
    case SyntaxKind::ProtectedKeyword:
    case SyntaxKind::PrivateKeyword:
        return true;

    default:
        return false;
    }
}

bool LanguageParser::IsAccessibilityModifier(SyntaxKind kind) {
    switch (kind) {
    // Accessibility modifiers aren't legal in a local function,
    // but a common mistake. Parse to give a better error message.
    case SyntaxKind::PublicKeyword:
    case SyntaxKind::InternalKeyword:
    case SyntaxKind::ProtectedKeyword:
    case SyntaxKind::PrivateKeyword:
        return true;

    default:
        return false;
    }
}

GreenNode *LanguageParser::TryParseLocalFunctionStatementBody(SyntaxList attributes, SyntaxList modifiers,
                                                              GreenNode *type, GreenNode *identifier) {
    // This may potentially be an ambiguous parse until very far into the token stream, so we may have to
    // backtrack. For example, "await x()" is ambiguous at the current point of parsing (right now we're right
    // after the x). The point at which it becomes unambiguous is after the argument list. A "=>" or "{" means its
    // a local function (with return type @await), a ";" or other expression-y token means its an await of a
    // function call.

    // Note that we could just check if we're in an async context, but that breaks some analyzers, because
    // "await f();" would be parsed as a local function statement when really we want a parse error so we can say
    // "did you mean to make this method be an async method?" (it's invalid either way, so the spec doesn't care)
    DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ false);

    // Indicates this must be parsed as a local function, even if there's no body
    bool forceLocalFunc = true;
    if (type->kind == SyntaxKind::IdentifierName) {
        GreenNode *id = GetField(type, Field::Identifier);
        forceLocalFunc = id->contextual_kind != SyntaxKind::AwaitKeyword;
    }

    SyntaxListBuilder badBuilder;
    for (int i = 0; i < modifiers.Count(); i++) {
        GreenNode *modifier = modifiers[i];
        switch (modifier->contextual_kind) {
        case SyntaxKind::AsyncKeyword:
            forceLocalFunc = true;
            continue;
        case SyntaxKind::UnsafeKeyword:
            forceLocalFunc = true;
            continue;
        case SyntaxKind::ReadOnlyKeyword:
        case SyntaxKind::VolatileKeyword:
            continue;  // already reported earlier, no need to report again
        case SyntaxKind::StaticKeyword:
            continue;
        case SyntaxKind::ExternKeyword:
            continue;
        default:
            modifier = AddError(modifier, ErrorCode::ERR_BadMemberFlag, {modifier->text});
            break;
        }

        if (badBuilder.IsNull()) {
            badBuilder = _pool.Allocate();
            badBuilder.AddRange(modifiers);
        }

        badBuilder[i] = modifier;
    }

    if (!badBuilder.IsNull()) {
        modifiers = badBuilder.ToList();
        _pool.Free(badBuilder);
    }

    ParserSyntaxContextResetter _(this, /*isInAsyncContext:*/ modifiers.Any(SyntaxKind::AsyncKeyword));

    GreenNode *typeParameterListOpt = ParseTypeParameterList();
    // "await f<T>()" still makes sense, so don't force accept a local function if there's a type parameter list.
    GreenNode *paramList = ParseParenthesizedParameterList(/*forExtensionOrUnion:*/ false);
    // "await x()" is ambiguous (see note at start of this method), but we assume "await x(await y)" is meant to be
    // a function if it's in a non-async context.
    if (!forceLocalFunc) {
        SeparatedSyntaxList paramListSyntax = GetField(paramList, Field::Parameters);
        for (int i = 0; i < paramListSyntax.Count(); i++) {
            // "await x(y)" still parses as a parameter list, so check to see if it's a valid parameter (like
            // "x(t y)")
            forceLocalFunc |= !paramListSyntax[i]->ContainsDiagnostics();
            if (forceLocalFunc) break;
        }
    }

    SyntaxListBuilder constraints;
    if (CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
        constraints = _pool.Allocate();
        ParseTypeParameterConstraintClauses(constraints);
        forceLocalFunc = true;
    }

    GreenNode *blockBody = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    ParseBlockAndExpressionBodiesWithSemicolon(blockBody, expressionBody, semicolon,
                                               /*parseSemicolonAfterBlock:*/ false);

    if (!forceLocalFunc && blockBody == nullptr && expressionBody == nullptr) {
        resetPoint.Reset();
        return nullptr;
    }

    return SyntaxFactory::LocalFunctionStatement(attributes, modifiers, type, identifier, typeParameterListOpt,
                                                 paramList, constraints, blockBody, expressionBody, semicolon);
}

}  // namespace csharp
