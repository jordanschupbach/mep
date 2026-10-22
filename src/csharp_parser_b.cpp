// LanguageParser, unit B: LanguageParser.cs lines 2381-5247 (CanStartMember .. ParseEventFieldDeclaration):
// member declarations, constructors/destructors, methods, operators, properties, indexers, accessors,
// parameters, events and field declarations. A method-for-method translation of Roslyn's
// src/Compilers/CSharp/Portable/Parser/LanguageParser.cs (MIT, (c) .NET Foundation and Contributors; see
// csharp_syntax.h for the commit). Declarations are in csharp_parser_members_b.inc.
//
// C# evaluates call arguments left to right and C++ does not, so every factory call whose arguments consume
// tokens has them hoisted into locals in the C# order.

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief Runs `f` at scope exit: the C# `finally` blocks. */
template <class F>
class Finally {
public:
    explicit Finally(F f) : f_(std::move(f)) {}
    ~Finally() { f_(); }
    Finally(const Finally &) = delete;
    Finally &operator=(const Finally &) = delete;

private:
    F f_;
};

}  // namespace

bool LanguageParser::CanStartMember(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::AbstractKeyword:
        case SyntaxKind::BoolKeyword:
        case SyntaxKind::ByteKeyword:
        case SyntaxKind::CharKeyword:
        case SyntaxKind::ClassKeyword:
        case SyntaxKind::ConstKeyword:
        case SyntaxKind::DecimalKeyword:
        case SyntaxKind::DelegateKeyword:
        case SyntaxKind::DoubleKeyword:
        case SyntaxKind::EnumKeyword:
        case SyntaxKind::EventKeyword:
        case SyntaxKind::ExternKeyword:
        case SyntaxKind::FixedKeyword:
        case SyntaxKind::FloatKeyword:
        case SyntaxKind::IntKeyword:
        case SyntaxKind::InterfaceKeyword:
        case SyntaxKind::InternalKeyword:
        case SyntaxKind::LongKeyword:
        case SyntaxKind::NewKeyword:
        case SyntaxKind::ObjectKeyword:
        case SyntaxKind::OverrideKeyword:
        case SyntaxKind::PrivateKeyword:
        case SyntaxKind::ProtectedKeyword:
        case SyntaxKind::PublicKeyword:
        case SyntaxKind::ReadOnlyKeyword:
        case SyntaxKind::SByteKeyword:
        case SyntaxKind::SealedKeyword:
        case SyntaxKind::ShortKeyword:
        case SyntaxKind::StaticKeyword:
        case SyntaxKind::StringKeyword:
        case SyntaxKind::StructKeyword:
        case SyntaxKind::UIntKeyword:
        case SyntaxKind::ULongKeyword:
        case SyntaxKind::UnsafeKeyword:
        case SyntaxKind::UShortKeyword:
        case SyntaxKind::VirtualKeyword:
        case SyntaxKind::VoidKeyword:
        case SyntaxKind::VolatileKeyword:
        case SyntaxKind::IdentifierToken:
        case SyntaxKind::TildeToken:
        case SyntaxKind::OpenBracketToken:
        case SyntaxKind::ImplicitKeyword:
        case SyntaxKind::ExplicitKeyword:
        case SyntaxKind::OpenParenToken:  // tuple
        case SyntaxKind::RefKeyword:
            return true;

        default:
            return false;
    }
}

bool LanguageParser::IsTypeDeclarationStart() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::ClassKeyword:
        case SyntaxKind::EnumKeyword:
        case SyntaxKind::InterfaceKeyword:
        case SyntaxKind::StructKeyword:
            return true;

        case SyntaxKind::DelegateKeyword:
            // case SyntaxKind.DelegateKeyword when !IsFunctionPointerStart(): (otherwise: default)
            return !IsFunctionPointerStart();

        case SyntaxKind::IdentifierToken:
            switch (CurrentToken()->contextual_kind) {
                case SyntaxKind::RecordKeyword:
                    // IsFeatureEnabled(MessageID.IDS_FeatureRecords)
                    return IsFeatureEnabled();

                case SyntaxKind::UnionKeyword:
                    // IsFeatureEnabled(MessageID.IDS_FeatureUnions)
                    return IsFeatureEnabled();

                default:
                    break;
            }

            if (IsExtensionContainerStart()) {
                return true;
            }

            return false;

        default:
            return false;
    }
}

// CanReuseMemberDeclaration: incremental parsing only, not ported.

GreenNode *LanguageParser::ParseMemberDeclaration() {
    // Use a parent kind that causes inclusion of only member declarations that could appear in a struct
    // e.g. including fixed member declarations, but not statements.
    static constexpr SyntaxKind parentKind = SyntaxKind::StructDeclaration;
    return ParseWithStackGuard(
        [](LanguageParser *self) { return self->ParseMemberDeclaration(parentKind); },
        // Creates a dummy declaration node to which we can attach a stack overflow message
        [](LanguageParser *self) {
            return SyntaxFactory::IncompleteMember(nullptr, nullptr, self->CreateMissingIdentifierName());
        });
}

// Returns null if we can't parse anything (even partially).
GreenNode *LanguageParser::ParseMemberDeclarationOrStatement(SyntaxKind parentKind) {
    _recursionDepth++;
    EnsureSufficientExecutionStack(_recursionDepth);
    GreenNode *result = ParseMemberDeclarationOrStatementCore(parentKind);
    _recursionDepth--;
    return result;
}

/// Changes in this function around member parsing should be mirrored in ParseMemberDeclarationCore.
/// Returns null if we can't parse anything (even partially).
GreenNode *LanguageParser::ParseMemberDeclarationOrStatementCore(SyntaxKind parentKind) {
    // "top-level" expressions and statements should never occur inside an asynchronous context
    CS_ASSERT(!IsInAsync());
    CS_ASSERT(parentKind == SyntaxKind::CompilationUnit);

    // (IsIncrementalAndFactoryContextMatches && CanReuseMemberDeclaration(...): no incremental parsing.)

    TerminatorState saveTermState = _termState;

    SyntaxList attributes = ParseStatementAttributeDeclarations();
    bool haveAttributes = attributes.Count() > 0;

    ResetPoint afterAttributesPoint = GetResetPoint();

    SyntaxListBuilder modifiers = _pool.Allocate();

    // Local functions of the C# method. They are always called with this method's `attributes` and
    // `afterAttributesPoint`, which they use directly. Each returns the `out result` (nullptr for false).

    // bool tryParseLocalDeclarationStatement<DeclarationSyntax>(attributes, out result)
    auto tryParseLocalDeclarationStatement = [&](SyntaxKind declarationKind) -> GreenNode * {
        // We are implicitly in an async context
        ParserSyntaxContextResetter _(this, true);
        int lastTokenPosition = -1;
        IsMakingProgress(lastTokenPosition);

        GreenNode *topLevelStatement = ParseLocalDeclarationStatement(attributes);
        if (topLevelStatement != nullptr && topLevelStatement->kind == declarationKind &&
            IsMakingProgress(lastTokenPosition, false)) {
            return SyntaxFactory::GlobalStatement(nullptr, nullptr, topLevelStatement);
        }

        return nullptr;
    };

    // bool tryParseStatement(attributes, ref afterAttributesPoint, out result)
    auto tryParseStatement = [&]() -> GreenNode * {
        DisposableResetPoint resetOnFailurePoint(this, false);

        Reset(afterAttributesPoint);

        if (IsPossibleStatement()) {
            TerminatorState saveTerm = _termState;
            _termState |= TerminatorState::IsPossibleStatementStartOrStop;  // partial statements can abort if a new statement starts

            // We are implicitly in an async context
            ParserSyntaxContextResetter _(this, true);
            GreenNode *statement = ParseStatementCore(attributes, true);

            _termState = saveTerm;

            if (statement != nullptr) {
                return SyntaxFactory::GlobalStatement(nullptr, nullptr, statement);
            }
        }

        resetOnFailurePoint.Reset();

        return nullptr;
    };

    // bool tryParseLocalDeclarationStatementFromStartPoint<DeclarationSyntax>(attributes, ref startPoint, out result)
    auto tryParseLocalDeclarationStatementFromStartPoint = [&](SyntaxKind declarationKind,
                                                               ResetPoint &startPoint) -> GreenNode * {
        DisposableResetPoint resetOnFailurePoint(this, false);

        Reset(startPoint);

        if (GreenNode *res = tryParseLocalDeclarationStatement(declarationKind)) {
            return res;
        }

        resetOnFailurePoint.Reset();
        return nullptr;
    };

    auto isAcceptableNonDeclarationStatement = [](GreenNode *statement, bool isScript) -> bool {
        if (statement == nullptr) return false;
        switch (statement->kind) {
            case SyntaxKind::LocalFunctionStatement:
                return false;

            case SyntaxKind::ExpressionStatement:
                // Do not parse a single identifier as an expression statement in a Simple Program, this could be a
                // beginning of a keyword and we want completion to offer it.
                if (!isScript && GetField(statement, Field::Expression)->kind == SyntaxKind::IdentifierName &&
                    GetField(statement, Field::SemicolonToken)->IsMissing()) {
                    return false;
                }
                return true;  // the `when` failed: default

            case SyntaxKind::LocalDeclarationStatement:
                return !isScript && GetField(statement, Field::UsingKeyword) != nullptr;

            default:
                return true;
        }
    };

    auto isFollowedByPossibleUsingDirective = [&]() -> bool {
        if (CurrentToken()->kind == SyntaxKind::UsingKeyword) {
            return !IsPossibleTopLevelUsingLocalDeclarationStatement();
        }

        if (CurrentToken()->contextual_kind == SyntaxKind::GlobalKeyword &&
            PeekToken(1)->kind == SyntaxKind::UsingKeyword) {
            DisposableResetPoint _(this, true);

            // Skip 'global' keyword
            EatToken();
            return !IsPossibleTopLevelUsingLocalDeclarationStatement();
        }

        return false;
    };

    // try { ... } finally { _pool.Free(modifiers); _termState = saveTermState; Release(ref afterAttributesPoint); }
    Finally outerFinally([&] {
        _pool.Free(modifiers);
        _termState = saveTermState;
        Release(afterAttributesPoint);
    });

    //
    // Check for the following cases to disambiguate between member declarations and expressions.
    // Doing this before parsing modifiers simplifies further analysis since some of these keywords can act as
    // modifiers as well.
    //
    // unsafe { ... }
    // fixed (...) { ... }
    // delegate (...) { ... }
    // delegate { ... }
    // new { ... }
    // new[] { ... }
    // new T (...)
    // new T [...]
    //
    if (!haveAttributes || !IsScript()) {
        // We are implicitly in an async context in a non-script environment.
        ParserSyntaxContextResetter _(this, IsInAsync() || !IsScript());

        switch (CurrentToken()->kind) {
            case SyntaxKind::UnsafeKeyword:
                if (PeekToken(1)->kind == SyntaxKind::OpenBraceToken) {
                    return SyntaxFactory::GlobalStatement(nullptr, nullptr, ParseUnsafeStatement(attributes));
                }
                break;

            case SyntaxKind::FixedKeyword:
                if (PeekToken(1)->kind == SyntaxKind::OpenParenToken) {
                    return SyntaxFactory::GlobalStatement(nullptr, nullptr, ParseFixedStatement(attributes));
                }
                break;

            case SyntaxKind::DelegateKeyword:
                // Check if this is an anonymous delegate expression or a delegate type declaration.
                // Anonymous delegate: delegate { } or delegate (params) { }
                // Delegate declaration: delegate Type Name(params);
                if (IsAnonymousDelegateExpression()) {
                    return SyntaxFactory::GlobalStatement(nullptr, nullptr, ParseExpressionStatement(attributes));
                }
                break;

            case SyntaxKind::NewKeyword:
                if (IsPossibleNewExpression()) {
                    return SyntaxFactory::GlobalStatement(nullptr, nullptr, ParseExpressionStatement(attributes));
                }
                break;

            default:
                break;
        }
    }

    // All modifiers that might start an expression are processed above.
    bool isPossibleTypeDeclaration = false;
    ParseModifiers(modifiers, false, true, isPossibleTypeDeclaration);
    bool haveModifiers = (modifiers.Count() > 0);
    GreenNode *result = nullptr;

    // Check for constructor form
    if (CurrentToken()->kind == SyntaxKind::IdentifierToken && PeekToken(1)->kind == SyntaxKind::OpenParenToken) {
        // Script:
        // Constructor definitions are not allowed. We parse them as method calls with semicolon missing error:
        //
        // Script(...) { ... }
        //            ^
        //            missing ';'
        //
        // Unless modifiers or attributes are present this is more likely to be a method call than a method
        // definition.
        if (haveAttributes || haveModifiers) {
            GreenNode *voidType = SyntaxFactory::PredefinedType(
                AddError(SyntaxFactory::MissingToken(SyntaxKind::VoidKeyword), ErrorCode::ERR_MemberNeedsType));

            if (!IsScript()) {
                if ((result = tryParseLocalDeclarationStatementFromStartPoint(SyntaxKind::LocalFunctionStatement,
                                                                              afterAttributesPoint)) != nullptr) {
                    return result;
                }
            } else {
                GreenNode *identifier = EatToken();
                return ParseMethodDeclaration(attributes, modifiers, voidType, nullptr, identifier, nullptr);
            }
        }
    }

    // Destructors are disallowed in global code, skipping check for them.
    // TODO: better error messages for script

    // Check for constant
    if (CurrentToken()->kind == SyntaxKind::ConstKeyword) {
        if (!IsScript() && (result = tryParseLocalDeclarationStatementFromStartPoint(
                                SyntaxKind::LocalDeclarationStatement, afterAttributesPoint)) != nullptr) {
            return result;
        }

        // Prefers const field over const local variable decl
        return ParseConstantFieldDeclaration(attributes, modifiers, parentKind);
    }

    // Check for event.
    if (CurrentToken()->kind == SyntaxKind::EventKeyword) {
        return ParseEventDeclaration(attributes, modifiers, parentKind);
    }

    // check for fixed size buffers.
    if (CurrentToken()->kind == SyntaxKind::FixedKeyword) {
        return ParseFixedSizeBufferDeclaration(attributes, modifiers, parentKind);
    }

    // Check for conversion operators (implicit/explicit)
    result = TryParseConversionOperatorDeclaration(attributes, modifiers);
    if (result != nullptr) {
        return result;
    }

    if (CurrentToken()->kind == SyntaxKind::NamespaceKeyword) {
        return ParseNamespaceDeclaration(attributes, modifiers);
    }

    // It's valid to have a type declaration here -- check for those
    if (isPossibleTypeDeclaration && IsTypeDeclarationStart()) {
        return ParseTypeDeclaration(attributes, modifiers);
    }

    GreenNode *type = ParseReturnType();

    ResetPoint afterTypeResetPoint = GetResetPoint();

    // try { ... } finally { Release(ref afterTypeResetPoint); }
    Finally innerFinally([&] { Release(afterTypeResetPoint); });

    // Try as a regular statement rather than a member declaration, if appropriate.
    {
        GreenNode *ignoredExplicitInterface = nullptr;
        if ((!haveAttributes || !IsScript()) && !haveModifiers &&
            (type->kind == SyntaxKind::RefType || !IsOperatorStart(ignoredExplicitInterface, false))) {
            Reset(afterAttributesPoint);

            if (CurrentToken()->kind != SyntaxKind::CloseBraceToken &&
                CurrentToken()->kind != SyntaxKind::EndOfFileToken && IsPossibleStatement()) {
                TerminatorState saveTerm = _termState;
                _termState |= TerminatorState::IsPossibleStatementStartOrStop;  // partial statements can abort if a new statement starts

                // We are implicitly in an async context in a non-script environment.
                ParserSyntaxContextResetter _(this, IsInAsync() || !IsScript());

                // In Script we don't allow local declaration statements at the top level.  We want
                // to fall out below and parse them instead as fields. For top-level statements, we allow
                // them, but want to try properties , etc. first.
                GreenNode *statement = ParseStatementCore(attributes, true);

                _termState = saveTerm;

                if (isAcceptableNonDeclarationStatement(statement, IsScript())) {
                    return SyntaxFactory::GlobalStatement(nullptr, nullptr, statement);
                }
            }

            Reset(afterTypeResetPoint);
        }
    }

    // Everything that's left -- methods, fields, properties, locals,
    // indexers, and non-conversion operators -- starts with a type
    // (possibly void).

    // Check for misplaced modifiers.  if we see any, then consider this member
    // terminated and restart parsing.
    if (IsMisplacedModifier(modifiers, attributes, type, result)) {
        return result;
    }

parse_member_name:;
    {
        // If we've seen the ref keyword, we know we must have an indexer, method, property, or local.
        bool typeIsRef = type->kind == SyntaxKind::RefType;  // type.IsRef
        GreenNode *explicitInterfaceOpt = nullptr;

        // Check here for operators
        // Allow old-style implicit/explicit casting operator syntax, just so we can give a better error
        if (!typeIsRef && IsOperatorStart(explicitInterfaceOpt)) {
            return ParseOperatorDeclaration(attributes, modifiers, type, explicitInterfaceOpt);
        }

        if ((!typeIsRef || !IsScript()) && IsFieldDeclaration(false, true)) {
            TerminatorState saveTerm = _termState;

            if ((!haveAttributes && !haveModifiers) || !IsScript()) {
                // if we are at top-level then statements can occur
                _termState |= TerminatorState::IsPossibleStatementStartOrStop;

                if (!IsScript()) {
                    Reset(afterAttributesPoint);
                    if ((result = tryParseLocalDeclarationStatement(SyntaxKind::LocalDeclarationStatement)) !=
                        nullptr) {
                        return result;
                    }

                    Reset(afterTypeResetPoint);
                }
            }

            if (!typeIsRef) {
                return ParseNormalFieldDeclaration(attributes, modifiers, type, parentKind);
            } else {
                _termState = saveTerm;
            }
        }

        // At this point we can either have indexers, methods, or
        // properties (or something unknown).  Try to break apart
        // the following name and determine what to do from there.
        GreenNode *identifierOrThisOpt = nullptr;
        GreenNode *typeParameterListOpt = nullptr;
        ParseMemberName(explicitInterfaceOpt, identifierOrThisOpt, typeParameterListOpt, false);

        if (!haveModifiers && !haveAttributes && !IsScript() && explicitInterfaceOpt == nullptr &&
            identifierOrThisOpt == nullptr && typeParameterListOpt == nullptr && !type->IsMissing() &&
            type->kind != SyntaxKind::RefType && !isFollowedByPossibleUsingDirective() &&
            (result = tryParseLocalDeclarationStatementFromStartPoint(SyntaxKind::LocalDeclarationStatement,
                                                                      afterAttributesPoint)) != nullptr) {
            return result;
        }

        // First, check if we got absolutely nothing.  If so, then
        // We need to consume a bad member and try again.
        if (IsNoneOrIncompleteMember(parentKind, attributes, modifiers, type, explicitInterfaceOpt,
                                     identifierOrThisOpt, typeParameterListOpt, result)) {
            return result;
        }

        // If the modifiers did not include "async", and the type we got was "async", and there was an
        // error in the identifier or its type parameters, then the user is probably in the midst of typing
        // an async method.  In that case we reconsider "async" to be a modifier, and treat the identifier
        // (with the type parameters) as the type (with type arguments).  Then we go back to looking for
        // the member name again.
        // For example, if we get
        //     async Task<
        // then we want async to be a modifier and Task<MISSING> to be a type.
        if (ReconsideredTypeAsAsyncModifier(modifiers, type, afterTypeResetPoint, explicitInterfaceOpt,
                                            identifierOrThisOpt, typeParameterListOpt)) {
            goto parse_member_name;
        }

        // identifierOrThisOpt may be null if we're doing error recovery for a property with missing identifier
        CS_ASSERT(identifierOrThisOpt != nullptr || IsStartOfPropertyBody(0));

        if (TryParseIndexerOrPropertyDeclaration(attributes, modifiers, type, explicitInterfaceOpt,
                                                 identifierOrThisOpt, typeParameterListOpt, result)) {
            return result;
        }

        if (!IsScript()) {
            if (explicitInterfaceOpt == nullptr &&
                (result = tryParseLocalDeclarationStatementFromStartPoint(SyntaxKind::LocalFunctionStatement,
                                                                          afterAttributesPoint)) != nullptr) {
                return result;
            }

            if (!haveModifiers && (result = tryParseStatement()) != nullptr) {
                return result;
            }
        }

        // treat anything else as a method.

        return ParseMethodDeclaration(attributes, modifiers, type, explicitInterfaceOpt, identifierOrThisOpt,
                                      typeParameterListOpt);
    }
}

bool LanguageParser::IsMisplacedModifier(SyntaxListBuilder modifiers, SyntaxList attributes, GreenNode *type,
                                         GreenNode *&result) {
    SyntaxKind contextualKind = CurrentToken()->contextual_kind;
    if (GetModifierExcludingScoped(CurrentToken()) != DeclarationModifiers::None &&
        !(contextualKind == SyntaxKind::PartialKeyword || contextualKind == SyntaxKind::AsyncKeyword ||
          contextualKind == SyntaxKind::RequiredKeyword || contextualKind == SyntaxKind::FileKeyword) &&
        IsComplete(type)) {
        GreenNode *misplacedModifier = CurrentToken();
        type = AddError(type,
                        // We're attaching a diagnostic for the misplaced modifier on the 'type' node.  So the offset
                        // will be *relative* to the *start* (not *full start*) of 'type'. That offset will then be
                        // the width of type itself, plus any trailing trivia it has, plus the leading trivia of the
                        // modifier itself.
                        type->Width() + type->GetTrailingTriviaWidth() + misplacedModifier->GetLeadingTriviaWidth(),
                        misplacedModifier->Width(), ErrorCode::ERR_BadModifierLocation, {misplacedModifier->text});

        result = SyntaxFactory::IncompleteMember(attributes, modifiers.ToList(), type);
        return true;
    }

    result = nullptr;
    return false;
}

bool LanguageParser::IsNoneOrIncompleteMember(SyntaxKind parentKind, SyntaxList attributes,
                                              SyntaxListBuilder modifiers, GreenNode *type,
                                              GreenNode *explicitInterfaceOpt, GreenNode *identifierOrThisOpt,
                                              GreenNode *typeParameterListOpt, GreenNode *&result) {
    auto looksLikePropertyType = [&]() -> bool {
        GreenNode *propertyType = type;

        // We really want to see at least `Type {` or `Type =>` to think of this as a property. And we want the
        // parser to be confident it got a good return type for the property, not something it was confused
        // about and already reported errors on.  Otherwise, we can too easily end up in cases where an error is
        // next to a `{` and we don't want that to be a property.
        if (propertyType->IsMissing() || ContainsErrorDiagnostic(propertyType)) return false;

        if (propertyType->kind == SyntaxKind::RefType) propertyType = GetField(propertyType, Field::Type);

        // Unlikely cases to actually be a property.  Likely some runaway error recovery case.  For example:
        //
        // with { }   (likely a with expression)
        // extension { }  (likely an extension block being written)
        // get { } (likely an accessor, not a property called 'get').  Same for set/add/remove/init.
        //
        // Properties are virtually always going to have a pascal cased identifier name.  So if we see one that
        // is actually a contextual keyword, then it's unlikely to be a property.
        if (propertyType->kind == SyntaxKind::IdentifierName &&
            SyntaxFacts::IsKeywordKind(GetField(propertyType, Field::Identifier)->contextual_kind)) {
            return false;
        }

        // Can add more cases here if we find ourselves being too eager.

        return true;
    };

    auto looksLikeStartOfPropertyBody = [&]() -> bool {
        // Need to see at least some modifiers, or something that strongly looks like a property type in order
        // for us to at least consider this a property.  Otherwise, we end up in cases where errant `{` and `=>`
        // tokens would lead us to think we're a property in inappropriate places.
        if (modifiers.Count() == 0 && !looksLikePropertyType()) return false;

        if (!IsStartOfPropertyBody(0)) return false;

        return true;
    };

    if (explicitInterfaceOpt == nullptr && identifierOrThisOpt == nullptr && typeParameterListOpt == nullptr) {
        if (looksLikeStartOfPropertyBody()) {
            result = nullptr;
            return false;
        }

        if (attributes.Count() == 0 && modifiers.Count() == 0 && type->IsMissing() &&
            type->kind != SyntaxKind::RefType) {
            // we haven't advanced, the caller needs to consume the tokens ahead
            result = nullptr;
            return true;
        }

        GreenNode *incompleteMember =
            SyntaxFactory::IncompleteMember(attributes, modifiers.ToList(), type->IsMissing() ? nullptr : type);
        if (ContainsErrorDiagnostic(incompleteMember)) {
            result = incompleteMember;
        } else if (parentKind == SyntaxKind::NamespaceDeclaration ||
                   parentKind == SyntaxKind::FileScopedNamespaceDeclaration ||
                   (parentKind == SyntaxKind::CompilationUnit && !IsScript())) {
            result = AddErrorToLastToken(incompleteMember, ErrorCode::ERR_NamespaceUnexpected);
        } else {
            // the error position should indicate CurrentToken
            result = AddError(
                incompleteMember,
                // We're attaching a diagnostic for the current token on the 'incompleteMember' node.  So the
                // offset will be *relative* to the *start* (not *full start*) of 'incompleteMember'. That
                // offset will then be the width of incompleteMember itself, plus any trailing trivia it has,
                // plus the leading trivia of the modifier itself.
                incompleteMember->Width() + incompleteMember->GetTrailingTriviaWidth() +
                    CurrentToken()->GetLeadingTriviaWidth(),
                CurrentToken()->Width(), ErrorCode::ERR_InvalidMemberDecl, {CurrentToken()->text});
        }

        return true;
    }

    result = nullptr;
    return false;
}

template <class RP>
bool LanguageParser::ReconsideredTypeAsAsyncModifier(SyntaxListBuilder &modifiers, GreenNode *&type,
                                                     RP &afterTypeResetPoint, GreenNode *&explicitInterfaceOpt,
                                                     GreenNode *&identifierOrThisOpt,
                                                     GreenNode *&typeParameterListOpt) {
    SyntaxKind currentKind = CurrentToken()->kind;
    if (type->kind != SyntaxKind::RefType && identifierOrThisOpt != nullptr &&
        ((typeParameterListOpt != nullptr && typeParameterListOpt->ContainsDiagnostics()) ||
         (currentKind != SyntaxKind::OpenParenToken && currentKind != SyntaxKind::OpenBraceToken &&
          currentKind != SyntaxKind::EqualsGreaterThanToken)) &&
        ReconsiderTypeAsAsyncModifier(modifiers, type, identifierOrThisOpt)) {
        Reset(afterTypeResetPoint);
        explicitInterfaceOpt = nullptr;
        identifierOrThisOpt = nullptr;
        typeParameterListOpt = nullptr;
        Release(afterTypeResetPoint);
        type = ParseReturnType();
        afterTypeResetPoint = GetResetPoint();
        return true;
    }

    return false;
}

bool LanguageParser::TryParseIndexerOrPropertyDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                          GreenNode *type, GreenNode *explicitInterfaceOpt,
                                                          GreenNode *identifierOrThisOpt,
                                                          GreenNode *typeParameterListOpt, GreenNode *&result) {
    if (identifierOrThisOpt != nullptr && identifierOrThisOpt->kind == SyntaxKind::ThisKeyword) {
        result = ParseIndexerDeclaration(attributes, modifiers, type, explicitInterfaceOpt, identifierOrThisOpt,
                                         typeParameterListOpt);
        return true;
    }

    if (IsStartOfPropertyBody(0)) {
        // Error recovery: if we see a property body but no identifier, synthesize a missing identifier
        if (identifierOrThisOpt == nullptr) {
            identifierOrThisOpt = AddError(CreateMissingIdentifierToken(), ErrorCode::ERR_IdentifierExpected);
        }

        result = ParsePropertyDeclaration(attributes, modifiers, type, explicitInterfaceOpt, identifierOrThisOpt,
                                          typeParameterListOpt);
        return true;
    }

    result = nullptr;
    return false;
}

bool LanguageParser::IsStartOfPropertyBody(int index) {
    SyntaxKind kind = PeekToken(index)->kind;

    // If we have `{` or `=>`, definitely a property body.
    if (kind == SyntaxKind::OpenBraceToken || kind == SyntaxKind::EqualsGreaterThanToken) return true;

    // For error recovery, also consider `; {` and `; =>` as starting a property body.
    if (kind == SyntaxKind::SemicolonToken) {
        kind = PeekToken(index + 1)->kind;
        if (kind == SyntaxKind::OpenBraceToken || kind == SyntaxKind::EqualsGreaterThanToken) return true;
    }

    return false;
}

// Returns null if we can't parse anything (even partially).
GreenNode *LanguageParser::ParseMemberDeclaration(SyntaxKind parentKind) {
    _recursionDepth++;
    EnsureSufficientExecutionStack(_recursionDepth);
    GreenNode *result = ParseMemberDeclarationCore(parentKind);
    _recursionDepth--;
    return result;
}

/// Changes in this function should be mirrored in ParseMemberDeclarationOrStatementCore.
/// Returns null if we can't parse anything (even partially).
GreenNode *LanguageParser::ParseMemberDeclarationCore(SyntaxKind parentKind) {
    // "top-level" expressions and statements should never occur inside an asynchronous context
    CS_ASSERT(!IsInAsync());
    CS_ASSERT(parentKind != SyntaxKind::CompilationUnit);

    // (IsIncrementalAndFactoryContextMatches && CanReuseMemberDeclaration(...): no incremental parsing.)

    SyntaxListBuilder modifiers = _pool.Allocate();

    TerminatorState saveTermState = _termState;

    // try { ... } finally { _pool.Free(modifiers); _termState = saveTermState; }
    Finally outerFinally([&] {
        _pool.Free(modifiers);
        _termState = saveTermState;
    });

    SyntaxList attributes = ParseAttributeDeclarations(false);

    bool isPossibleTypeDeclaration = false;
    ParseModifiers(modifiers, false, false, isPossibleTypeDeclaration);

    if (IsExtensionContainerStart()) {
        return ParseMainTypeDeclaration(attributes, modifiers);
    }

    // Check for constructor form
    if (CurrentToken()->kind == SyntaxKind::IdentifierToken && PeekToken(1)->kind == SyntaxKind::OpenParenToken) {
        return ParseConstructorDeclaration(attributes, modifiers);
    }

    // Check for destructor form
    if (CurrentToken()->kind == SyntaxKind::TildeToken) {
        return ParseDestructorDeclaration(attributes, modifiers);
    }

    // Check for constant
    if (CurrentToken()->kind == SyntaxKind::ConstKeyword) {
        return ParseConstantFieldDeclaration(attributes, modifiers, parentKind);
    }

    // Check for event.
    if (CurrentToken()->kind == SyntaxKind::EventKeyword) {
        return ParseEventDeclaration(attributes, modifiers, parentKind);
    }

    // check for fixed size buffers.
    if (CurrentToken()->kind == SyntaxKind::FixedKeyword) {
        return ParseFixedSizeBufferDeclaration(attributes, modifiers, parentKind);
    }

    // Check for conversion operators (implicit/explicit)
    GreenNode *result = TryParseConversionOperatorDeclaration(attributes, modifiers);
    if (result != nullptr) {
        return result;
    }

    // Namespaces should be handled by the caller, not checking for them

    // It's valid to have a type declaration here -- check for those
    if (isPossibleTypeDeclaration && IsTypeDeclarationStart()) {
        return ParseTypeDeclaration(attributes, modifiers);
    }

    // Everything that's left -- methods, fields, properties,
    // indexers, and non-conversion operators -- starts with a type
    // (possibly void).
    GreenNode *type = ParseReturnType();

    ResetPoint afterTypeResetPoint = GetResetPoint();

    // try { ... } finally { Release(ref afterTypeResetPoint); }
    Finally innerFinally([&] { Release(afterTypeResetPoint); });

    // Check for misplaced modifiers.  if we see any, then consider this member
    // terminated and restart parsing.
    if (IsMisplacedModifier(modifiers, attributes, type, result)) {
        return result;
    }

parse_member_name:;
    {
        GreenNode *explicitInterfaceOpt = nullptr;

        // If we've seen the ref keyword, we know we must have an indexer, method, field, or property.
        if (type->kind != SyntaxKind::RefType) {
            // Check here for operators
            // Allow old-style implicit/explicit casting operator syntax, just so we can give a better error
            if (IsOperatorStart(explicitInterfaceOpt)) {
                return ParseOperatorDeclaration(attributes, modifiers, type, explicitInterfaceOpt);
            }
        }

        if (IsFieldDeclaration(false, false)) {
            return ParseNormalFieldDeclaration(attributes, modifiers, type, parentKind);
        }

        // At this point we can either have indexers, methods, or
        // properties (or something unknown).  Try to break apart
        // the following name and determine what to do from there.
        GreenNode *identifierOrThisOpt = nullptr;
        GreenNode *typeParameterListOpt = nullptr;
        ParseMemberName(explicitInterfaceOpt, identifierOrThisOpt, typeParameterListOpt, false);

        // First, check if we got absolutely nothing.  If so, then
        // We need to consume a bad member and try again.
        if (IsNoneOrIncompleteMember(parentKind, attributes, modifiers, type, explicitInterfaceOpt,
                                     identifierOrThisOpt, typeParameterListOpt, result)) {
            return result;
        }

        // If the modifiers did not include "async", and the type we got was "async", and there was an
        // error in the identifier or its type parameters, then the user is probably in the midst of typing
        // an async method.  In that case we reconsider "async" to be a modifier, and treat the identifier
        // (with the type parameters) as the type (with type arguments).  Then we go back to looking for
        // the member name again.
        // For example, if we get
        //     async Task<
        // then we want async to be a modifier and Task<MISSING> to be a type.
        if (ReconsideredTypeAsAsyncModifier(modifiers, type, afterTypeResetPoint, explicitInterfaceOpt,
                                            identifierOrThisOpt, typeParameterListOpt)) {
            goto parse_member_name;
        }

        // identifierOrThisOpt may be null if we're doing error recovery for a property with missing identifier
        CS_ASSERT(identifierOrThisOpt != nullptr || IsStartOfPropertyBody(0));

        if (TryParseIndexerOrPropertyDeclaration(attributes, modifiers, type, explicitInterfaceOpt,
                                                 identifierOrThisOpt, typeParameterListOpt, result)) {
            return result;
        }

        // treat anything else as a method.
        return ParseMethodDeclaration(attributes, modifiers, type, explicitInterfaceOpt, identifierOrThisOpt,
                                      typeParameterListOpt);
    }
}

bool LanguageParser::IsExtensionContainerStart() {
    // For error recovery, we recognize `extension` followed by `<` even in older language versions
    return CurrentToken()->contextual_kind == SyntaxKind::ExtensionKeyword &&
           (IsFeatureEnabled() /* IDS_FeatureExtensions */ || PeekToken(1)->kind == SyntaxKind::LessThanToken);
}

// if the modifiers do not contain async or replace and the type is the identifier "async" or "replace", then
// add that identifier to the modifiers and assign a new type from the identifierOrThisOpt and the
// type parameter list
bool LanguageParser::ReconsiderTypeAsAsyncModifier(SyntaxListBuilder &modifiers, GreenNode *type,
                                                   GreenNode *identifierOrThisOpt) {
    if (type->kind != SyntaxKind::IdentifierName) return false;

    if (identifierOrThisOpt->kind != SyntaxKind::IdentifierToken) return false;

    GreenNode *identifier = GetField(type, Field::Identifier);
    SyntaxKind contextualKind = identifier->contextual_kind;
    if (contextualKind != SyntaxKind::AsyncKeyword || modifiers.Any(contextualKind)) {
        return false;
    }

    modifiers.Add(ConvertToKeyword(identifier));
    return true;
}

bool LanguageParser::IsFieldDeclaration(bool isEvent, bool isGlobalScriptLevel) {
    if (CurrentToken()->kind != SyntaxKind::IdentifierToken) {
        return false;
    }

    if (CurrentToken()->contextual_kind == SyntaxKind::GlobalKeyword &&
        PeekToken(1)->kind == SyntaxKind::UsingKeyword) {
        return false;
    }

    // Treat this as a field, unless we have anything following that
    // makes us:
    //   a) explicit
    //   b) generic
    //   c) a property
    //   d) a method (unless we already know we're parsing an event)

    // Error recovery, don't allow a misplaced semicolon after the name in a property to throw off the entire parse.
    //
    // e.g. `public int MyProperty; { get; set; }` should still be parsed as a property with a skipped token.
    if (!isGlobalScriptLevel && IsStartOfPropertyBody(1)) {
        return false;
    }

    switch (PeekToken(1)->kind) {
        case SyntaxKind::DotToken:                // Goo.     explicit
        case SyntaxKind::ColonColonToken:         // Goo::    explicit
        case SyntaxKind::LessThanToken:           // Goo<     explicit or generic method
        case SyntaxKind::OpenBraceToken:          // Goo {    property
        case SyntaxKind::EqualsGreaterThanToken:  // Goo =>   property
            return false;
        case SyntaxKind::OpenParenToken:  // Goo(     method
            return isEvent;
        default:
            return true;
    }
}

bool LanguageParser::IsOperatorKeyword() {
    SyntaxKind kind = CurrentToken()->kind;
    return kind == SyntaxKind::ImplicitKeyword || kind == SyntaxKind::ExplicitKeyword ||
           kind == SyntaxKind::OperatorKeyword;
}

bool LanguageParser::IsComplete(GreenNode *node) {
    if (node == nullptr) {
        return false;
    }

    // foreach (var child in node.ChildNodesAndTokens().Reverse()): the slots from last to first, a list slot's
    // items from last to first, null slots skipped. Returns nullopt to go on to the next child.
    auto visit = [](GreenNode *child) -> std::optional<bool> {
        if (!child->IsToken()) {
            return IsComplete(child);
        }

        if (child->IsMissing()) {
            return false;
        }

        if (child->kind != SyntaxKind::None) {
            return true;
        }

        // if token was optional, consider the next one..
        return std::nullopt;
    };

    for (int i = node->SlotCount() - 1; i >= 0; i--) {
        GreenNode *slot = node->GetSlot(i);
        if (slot == nullptr) continue;
        if (slot->IsList()) {
            for (int j = slot->SlotCount() - 1; j >= 0; j--) {
                if (std::optional<bool> r = visit(slot->GetSlot(j))) return *r;
            }
        } else {
            if (std::optional<bool> r = visit(slot)) return *r;
        }
    }

    return true;
}

GreenNode *LanguageParser::ParseConstructorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    GreenNode *name = ParseIdentifierToken();
    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfMethodSignature;
    // try { ... } finally { _termState = saveTerm; }
    Finally restoreTerm([&] { _termState = saveTerm; });

    GreenNode *paramList = ParseParenthesizedParameterList(false);
    GreenNode *initializer = TryParseConstructorInitializer();

    GreenNode *body = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    ParseBlockAndExpressionBodiesWithSemicolon(body, expressionBody, semicolon);

    return SyntaxFactory::ConstructorDeclaration(attributes, modifiers.ToList(), name, paramList, initializer, body,
                                                 expressionBody, semicolon);
}

GreenNode *LanguageParser::TryParseConstructorInitializer() {
    SyntaxKind currentTokenKind = CurrentToken()->kind;
    bool shouldParse = currentTokenKind == SyntaxKind::ColonToken ||
                       (currentTokenKind == SyntaxKind::EqualsGreaterThanToken &&
                        (PeekToken(1)->kind == SyntaxKind::ThisKeyword ||
                         PeekToken(1)->kind == SyntaxKind::BaseKeyword) &&
                        PeekToken(2)->kind == SyntaxKind::OpenParenToken);

    if (!shouldParse) return nullptr;

    return ParseConstructorInitializer();
}

GreenNode *LanguageParser::ParseConstructorInitializer() {
    // Normally called for `:` but also in some error recovery circumstances for `=>`. EatTokenAsKind handles
    // both cases properly, producing the right errors we need in the latter case, and always consuming
    // whichever token we're coming into this method on.
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ColonToken ||
              CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken);
    GreenNode *colon = EatTokenAsKind(SyntaxKind::ColonToken);

    GreenNode *token = CurrentToken()->kind == SyntaxKind::BaseKeyword || CurrentToken()->kind == SyntaxKind::ThisKeyword
                           ? EatToken()
                           : EatToken(SyntaxKind::ThisKeyword, ErrorCode::ERR_ThisOrBaseExpected);

    GreenNode *argumentList;
    if (CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        argumentList = ParseParenthesizedArgumentList();
    } else {
        GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken, !token->ContainsDiagnostics());
        GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken, !token->ContainsDiagnostics());
        argumentList = SyntaxFactory::ArgumentList(openParen, nullptr, closeParen);
    }

    return SyntaxFactory::ConstructorInitializer(token->kind == SyntaxKind::BaseKeyword
                                                     ? SyntaxKind::BaseConstructorInitializer
                                                     : SyntaxKind::ThisConstructorInitializer,
                                                 colon, token, argumentList);
}

GreenNode *LanguageParser::ParseDestructorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::TildeToken);
    GreenNode *tilde = EatToken(SyntaxKind::TildeToken);

    GreenNode *name = ParseIdentifierToken();
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    GreenNode *parameterList = SyntaxFactory::ParameterList(openParen, nullptr, closeParen);

    GreenNode *body = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    ParseBlockAndExpressionBodiesWithSemicolon(body, expressionBody, semicolon);

    return SyntaxFactory::DestructorDeclaration(attributes, modifiers.ToList(), tilde, name, parameterList, body,
                                                expressionBody, semicolon);
}

/// Parses any block or expression bodies that are present. Also parses
/// the trailing semicolon if one is present.
void LanguageParser::ParseBlockAndExpressionBodiesWithSemicolon(GreenNode *&blockBody, GreenNode *&expressionBody,
                                                                GreenNode *&semicolon,
                                                                bool parseSemicolonAfterBlock) {
    // Check for 'forward' declarations with no block of any kind
    if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        blockBody = nullptr;
        expressionBody = nullptr;
        semicolon = EatToken(SyntaxKind::SemicolonToken);
        return;
    }

    blockBody = CurrentToken()->kind == SyntaxKind::OpenBraceToken ? ParseMethodOrAccessorBodyBlock(nullptr, false)
                                                                    : nullptr;

    expressionBody = CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken ? ParseArrowExpressionClause()
                                                                                 : nullptr;

    // Expression-bodies need semicolons and native behavior
    // expects a semicolon if there is no body
    if (expressionBody != nullptr || blockBody == nullptr) {
        semicolon = EatToken(SyntaxKind::SemicolonToken);
    }
    // Check for bad semicolon after block body
    else if (parseSemicolonAfterBlock && CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        semicolon = EatTokenWithPrejudice(ErrorCode::ERR_UnexpectedSemicolon);
    } else {
        semicolon = nullptr;
    }
}

bool LanguageParser::IsEndOfTypeParameterList() {
    if (CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        // void Goo<T (
        return true;
    }

    if (CurrentToken()->kind == SyntaxKind::ColonToken) {
        // class C<T :
        return true;
    }

    if (CurrentToken()->kind == SyntaxKind::OpenBraceToken) {
        // class C<T {
        return true;
    }

    if (IsCurrentTokenWhereOfConstraintClause()) {
        // class C<T where T :
        return true;
    }

    return false;
}

bool LanguageParser::IsEndOfMethodSignature() {
    return CurrentToken()->kind == SyntaxKind::SemicolonToken || CurrentToken()->kind == SyntaxKind::OpenBraceToken;
}

bool LanguageParser::IsEndOfTypeSignature() {
    return CurrentToken()->kind == SyntaxKind::SemicolonToken || CurrentToken()->kind == SyntaxKind::OpenBraceToken;
}

bool LanguageParser::IsEndOfNameInExplicitInterface() {
    return CurrentToken()->kind == SyntaxKind::DotToken || CurrentToken()->kind == SyntaxKind::ColonColonToken;
}

bool LanguageParser::IsEndOfFunctionPointerParameterList(bool errored) {
    return CurrentToken()->kind == (errored ? SyntaxKind::CloseParenToken : SyntaxKind::GreaterThanToken);
}

bool LanguageParser::IsEndOfFunctionPointerCallingConvention() {
    return CurrentToken()->kind == SyntaxKind::CloseBracketToken;
}

GreenNode *LanguageParser::ParseMethodDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                                  GreenNode *explicitInterfaceOpt, GreenNode *identifier,
                                                  GreenNode *typeParameterList) {
    // Parse the name (it could be qualified)
    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfMethodSignature;

    GreenNode *paramList = ParseParenthesizedParameterList(false);

    SyntaxListBuilder constraints;  // default(SyntaxListBuilder<TypeParameterConstraintClauseSyntax>)
    if (CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
        constraints = _pool.Allocate();
        ParseTypeParameterConstraintClauses(constraints);
    } else if (CurrentToken()->kind == SyntaxKind::ColonToken) {
        // Use else if, rather than if, because if we see both a constructor initializer and a constraint clause,
        // we're too lost to recover.
        GreenNode *colonToken = CurrentToken();

        GreenNode *initializer = ParseConstructorInitializer();
        initializer = AddErrorToFirstToken(initializer, ErrorCode::ERR_UnexpectedToken, {colonToken->text});
        paramList = AddTrailingSkippedSyntax(paramList, initializer);

        // CONSIDER: Parsing an invalid constructor initializer could, conceivably, get us way
        // off track.  If this becomes a problem, an alternative approach would be to generalize
        // EatTokenWithPrejudice in such a way that we can just skip everything until we recognize
        // our context again (perhaps an open brace).
    }

    _termState = saveTerm;

    // Method declarations cannot be nested or placed inside async lambdas, and so cannot occur in an
    // asynchronous context. Therefore the IsInAsync state of the parent scope is not saved and
    // restored, just assumed to be false and reset accordingly after parsing the method body.
    CS_ASSERT(!IsInAsync());

    ParserSyntaxContextResetter _(this, modifiers.Any(SyntaxKind::AsyncKeyword));
    GreenNode *blockBody = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    ParseBlockAndExpressionBodiesWithSemicolon(blockBody, expressionBody, semicolon);

    GreenNode *modifierList = modifiers.ToList();
    GreenNode *constraintList = _pool.ToListAndFree(constraints);
    return SyntaxFactory::MethodDeclaration(attributes, modifierList, type, explicitInterfaceOpt, identifier,
                                            typeParameterList, paramList, constraintList, blockBody, expressionBody,
                                            semicolon);
}

GreenNode *LanguageParser::ParseReturnType() {
    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfReturnType;
    GreenNode *type = ParseTypeOrVoid();
    _termState = saveTerm;
    return type;
}

bool LanguageParser::IsEndOfReturnType() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::OpenParenToken:
        case SyntaxKind::OpenBraceToken:
        case SyntaxKind::SemicolonToken:
            return true;
        default:
            return false;
    }
}

GreenNode *LanguageParser::TryParseConversionOperatorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    ResetPoint point = GetResetPoint();

    // try { ... } finally { Release(ref point); }
    Finally releasePoint([&] { Release(point); });

    auto tryParseExplicitInterfaceSpecifier = [&]() -> GreenNode * {
        if (CurrentToken()->kind != SyntaxKind::IdentifierToken) {
            return nullptr;
        }

        GreenNode *explicitInterfaceName = nullptr;
        GreenNode *separator = nullptr;

        while (true) {
            // now, scan past the next name.  if it's followed by a dot then
            // it's part of the explicit name we're building up.  Otherwise,
            // it should be an operator token

            bool isPartOfInterfaceName;
            {
                DisposableResetPoint scanPoint(this, true);
                if (CurrentToken()->kind == SyntaxKind::OperatorKeyword) {
                    isPartOfInterfaceName = false;
                } else {
                    int lastTokenPosition = -1;
                    IsMakingProgress(lastTokenPosition, true);
                    ScanNamedTypePart();
                    isPartOfInterfaceName = IsDotOrColonColon() || (IsMakingProgress(lastTokenPosition, false) &&
                                                                     CurrentToken()->kind != SyntaxKind::OpenParenToken);
                }
            }

            if (!isPartOfInterfaceName) {
                // We're past any explicit interface portion
                if (separator != nullptr && separator->kind == SyntaxKind::ColonColonToken) {
                    separator = AddError(separator, ErrorCode::ERR_AliasQualAsExpression);
                    separator = ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
                }

                break;
            } else {
                // If we saw a . or :: then we must have something explicit.
                AccumulateExplicitInterfaceName(explicitInterfaceName, separator);
            }
        }

        if (explicitInterfaceName == nullptr) {
            return nullptr;
        }

        if (separator->kind != SyntaxKind::DotToken) {
            separator = WithAdditionalDiagnostics(
                separator, GetExpectedTokenError(SyntaxKind::DotToken, separator->kind,
                                                 separator->GetLeadingTriviaWidth(), separator->Width()));
            separator = ConvertToMissingWithTrailingTrivia(separator, SyntaxKind::DotToken);
        }

        return SyntaxFactory::ExplicitInterfaceSpecifier(explicitInterfaceName, separator);
    };

    bool haveExplicitInterfaceName = false;

    if (!(CurrentToken()->kind == SyntaxKind::ImplicitKeyword || CurrentToken()->kind == SyntaxKind::ExplicitKeyword)) {
        SyntaxKind separatorKind = SyntaxKind::None;

        if (CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            // Scan possible ExplicitInterfaceSpecifier

            while (true) {
                // now, scan past the next name.  if it's followed by a dot then
                // it's part of the explicit name we're building up.  Otherwise,
                // it should be an operator token

                if (CurrentToken()->kind == SyntaxKind::OperatorKeyword) {
                    // We're past any explicit interface portion
                    break;
                } else {
                    DisposableResetPoint scanNamePartPoint(this, false);

                    int lastTokenPosition = -1;
                    IsMakingProgress(lastTokenPosition, true);
                    ScanNamedTypePart();

                    if (IsDotOrColonColon() || (IsMakingProgress(lastTokenPosition, false) &&
                                                CurrentToken()->kind != SyntaxKind::OpenParenToken)) {
                        haveExplicitInterfaceName = true;

                        if (IsDotOrColonColon()) {
                            separatorKind = CurrentToken()->kind;
                            EatToken();
                        } else {
                            separatorKind = SyntaxKind::None;
                        }

                    } else {
                        scanNamePartPoint.Reset();

                        // We're past any explicit interface portion
                        break;
                    }
                }
            }
        }

        bool possibleConversion;

        if (CurrentToken()->kind != SyntaxKind::OperatorKeyword ||
            (haveExplicitInterfaceName && separatorKind != SyntaxKind::DotToken)) {
            possibleConversion = false;
        } else if (PeekToken(1)->kind == SyntaxKind::CheckedKeyword ||
                   PeekToken(1)->kind == SyntaxKind::UncheckedKeyword) {
            possibleConversion = !SyntaxFacts::IsAnyOverloadableOperator(PeekToken(2)->kind);
        } else {
            possibleConversion = !SyntaxFacts::IsAnyOverloadableOperator(PeekToken(1)->kind);
        }

        Reset(point);

        if (!possibleConversion) {
            return nullptr;
        }
    }

    GreenNode *style = CurrentToken()->kind == SyntaxKind::ImplicitKeyword ||
                               CurrentToken()->kind == SyntaxKind::ExplicitKeyword
                           ? EatToken()
                           : EatToken(SyntaxKind::ExplicitKeyword);

    GreenNode *explicitInterfaceOpt = tryParseExplicitInterfaceSpecifier();
    CS_ASSERT(!style->IsMissing() || haveExplicitInterfaceName == (explicitInterfaceOpt != nullptr));

    GreenNode *opKeyword;
    GreenNode *type;

    if (!style->IsMissing() && explicitInterfaceOpt != nullptr && CurrentToken()->kind != SyntaxKind::OperatorKeyword &&
        SyntaxList(style->GetTrailingTrivia()).Any(SyntaxKind::EndOfLineTrivia)) {
        // Not likely an explicit interface implementation. Likely a beginning of the next member on the next line.
        Reset(point);
        style = EatToken();
        explicitInterfaceOpt = nullptr;
        opKeyword = EatToken(SyntaxKind::OperatorKeyword);
        type = AddError(CreateMissingIdentifierName(), ErrorCode::ERR_IdentifierExpected);

        return SyntaxFactory::ConversionOperatorDeclaration(
            attributes, modifiers.ToList(), style, explicitInterfaceOpt, opKeyword, /*checkedKeyword:*/ nullptr, type,
            SyntaxFactory::ParameterList(SyntaxFactory::MissingToken(SyntaxKind::OpenParenToken), nullptr,
                                         SyntaxFactory::MissingToken(SyntaxKind::CloseParenToken)),
            /*body:*/ nullptr,
            /*expressionBody:*/ nullptr,
            /*semicolonToken:*/ SyntaxFactory::MissingToken(SyntaxKind::SemicolonToken));
    }

    opKeyword = EatToken(SyntaxKind::OperatorKeyword);
    GreenNode *checkedKeyword = TryEatCheckedOrHandleUnchecked(opKeyword);

    Release(point);
    point = GetResetPoint();

    bool couldBeParameterList = CurrentToken()->kind == SyntaxKind::OpenParenToken;
    type = ParseType();

    if (couldBeParameterList && type->kind == SyntaxKind::TupleType) {
        SeparatedSyntaxList elements(GetField(type, Field::Elements));
        if (elements.Count() == 2 && elements.SeparatorCount() == 1 && elements.GetSeparator(0)->IsMissing() &&
            elements[1]->IsMissing() && CurrentToken()->kind != SyntaxKind::OpenParenToken) {
            // It looks like the type is missing and we parsed parameter list as the type. Recover.
            Reset(point);
            type = ParseIdentifierName();
        }
    }

    GreenNode *paramList = ParseParenthesizedParameterList(false);

    GreenNode *blockBody = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    ParseBlockAndExpressionBodiesWithSemicolon(blockBody, expressionBody, semicolon);

    return SyntaxFactory::ConversionOperatorDeclaration(attributes, modifiers.ToList(), style, explicitInterfaceOpt,
                                                        opKeyword, checkedKeyword, type, paramList, blockBody,
                                                        expressionBody, semicolon);
}

GreenNode *LanguageParser::TryEatCheckedOrHandleUnchecked(GreenNode *&operatorKeyword) {
    if (CurrentToken()->kind == SyntaxKind::UncheckedKeyword) {
        // if we encounter `operator unchecked`, we place the `unchecked` as skipped trivia on `operator`
        GreenNode *misplacedToken = AddError(EatToken(), ErrorCode::ERR_MisplacedUnchecked);
        operatorKeyword = AddTrailingSkippedSyntax(operatorKeyword, misplacedToken);
        return nullptr;
    }

    return TryEatToken(SyntaxKind::CheckedKeyword);
}

GreenNode *LanguageParser::ParseOperatorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                    GreenNode *type, GreenNode *explicitInterfaceOpt) {
    // We can get here after seeing `explicit` or `implicit` or `operator`.  `ret-type explicit op ...` is not
    // legal though.
    GreenNode *firstToken = CurrentToken();
    if ((firstToken->kind == SyntaxKind::ExplicitKeyword || firstToken->kind == SyntaxKind::ImplicitKeyword) &&
        PeekToken(1)->kind == SyntaxKind::OperatorKeyword) {
        GreenNode *conversionOperator = TryParseConversionOperatorDeclaration(attributes, modifiers);
        if (conversionOperator != nullptr) {
            // We need to ensure the type syntax the user provided gets an appropriate error and is placed as
            // leading skipped trivia for the explicit/implicit keyword.
            GreenNode *newImplicitOrExplicitKeyword =
                AddLeadingSkippedSyntax(GetField(conversionOperator, Field::ImplicitOrExplicitKeyword),
                                        AddError(type, ErrorCode::ERR_BadOperatorSyntax, {firstToken->text}));
            // conversionOperator.Update(...): a new node with the new keyword; the old node's own diagnostics kept.
            GreenNode *updated = SyntaxFactory::ConversionOperatorDeclaration(
                GetField(conversionOperator, Field::AttributeLists), GetField(conversionOperator, Field::Modifiers),
                newImplicitOrExplicitKeyword, GetField(conversionOperator, Field::ExplicitInterfaceSpecifier),
                GetField(conversionOperator, Field::OperatorKeyword), GetField(conversionOperator, Field::CheckedKeyword),
                GetField(conversionOperator, Field::Type), GetField(conversionOperator, Field::ParameterList),
                GetField(conversionOperator, Field::Body), GetField(conversionOperator, Field::ExpressionBody),
                GetField(conversionOperator, Field::SemicolonToken));
            if (conversionOperator->diag_count > 0) {
                updated = WithDiagnosticsGreen(updated, conversionOperator->diags, conversionOperator->diag_count);
            }
            return updated;
        }
    }

    GreenNode *opKeyword = EatToken(SyntaxKind::OperatorKeyword);
    GreenNode *checkedKeyword = TryEatCheckedOrHandleUnchecked(opKeyword);
    GreenNode *opToken;
    int opTokenErrorOffset;
    int opTokenErrorWidth;

    if (SyntaxFacts::IsAnyOverloadableOperator(CurrentToken()->kind)) {
        opToken = EatToken();
        CS_ASSERT(!opToken->IsMissing());
        opTokenErrorOffset = opToken->GetLeadingTriviaWidth();
        opTokenErrorWidth = opToken->Width();
    } else {
        if (CurrentToken()->kind == SyntaxKind::ImplicitKeyword || CurrentToken()->kind == SyntaxKind::ExplicitKeyword) {
            // Grab the offset and width before we consume the invalid keyword and change our position.
            opTokenErrorOffset = 0;
            opTokenErrorWidth = CurrentToken()->Width();
            opToken = ConvertToMissingWithTrailingTrivia(EatToken(), SyntaxKind::PlusToken);
            CS_ASSERT(opToken->IsMissing());  // ConvertToMissingWithTrailingTrivia should have converted to a missing token.

            CS_ASSERT(type != nullptr);  // How could it be?  The only caller got it from ParseReturnType.

            if (type->IsMissing()) {
                SyntaxDiagnosticInfo diagInfo = MakeError(opTokenErrorOffset, opTokenErrorWidth,
                                                          ErrorCode::ERR_BadOperatorSyntax,
                                                          {SyntaxFacts::GetText(SyntaxKind::PlusToken)});
                opToken = WithAdditionalDiagnostics(opToken, diagInfo);
            } else {
                // Dev10 puts this error on the type (if there is one).
                type = AddError(type, ErrorCode::ERR_BadOperatorSyntax, {SyntaxFacts::GetText(SyntaxKind::PlusToken)});
            }
        } else {
            // Consume whatever follows the operator keyword as the operator token.  If it is not we'll add an
            // error below (when we can guess the arity). Handle .. as well so we can give the user a good
            // message if they do `operator ..`
            opToken = IsAtDotDotToken() ? EatDotDotToken() : EatToken();
            CS_ASSERT(!opToken->IsMissing());
            opTokenErrorOffset = opToken->GetLeadingTriviaWidth();
            opTokenErrorWidth = opToken->Width();
        }
    }

    // check for >> and >>>
    if (opToken->kind == SyntaxKind::GreaterThanToken) {
        GreenNode *tk = CurrentToken();

        if (tk->kind == SyntaxKind::GreaterThanToken) {
            if (NoTriviaBetween(opToken, tk))  // no trailing trivia and no leading trivia
            {
                GreenNode *opToken2 = EatToken();
                tk = CurrentToken();

                if (tk->kind == SyntaxKind::GreaterThanToken &&
                    NoTriviaBetween(opToken2, tk))  // no trailing trivia and no leading trivia
                {
                    opToken2 = EatToken();
                    opToken = SyntaxFactory::Token(opToken->GetLeadingTrivia(),
                                                   SyntaxKind::GreaterThanGreaterThanGreaterThanToken,
                                                   opToken2->GetTrailingTrivia());
                    opTokenErrorWidth = opToken->Width();
                } else if (tk->kind == SyntaxKind::GreaterThanEqualsToken &&
                           NoTriviaBetween(opToken2, tk))  // no trailing trivia and no leading trivia
                {
                    opToken2 = EatToken();
                    opToken = SyntaxFactory::Token(opToken->GetLeadingTrivia(),
                                                   SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken,
                                                   opToken2->GetTrailingTrivia());
                    opTokenErrorWidth = opToken->Width();
                } else {
                    opToken = SyntaxFactory::Token(opToken->GetLeadingTrivia(), SyntaxKind::GreaterThanGreaterThanToken,
                                                   opToken2->GetTrailingTrivia());
                    opTokenErrorWidth = opToken->Width();
                }
            }
        } else if (tk->kind == SyntaxKind::GreaterThanEqualsToken &&
                   NoTriviaBetween(opToken, tk))  // no trailing trivia and no leading trivia
        {
            GreenNode *opToken2 = EatToken();
            opToken = SyntaxFactory::Token(opToken->GetLeadingTrivia(), SyntaxKind::GreaterThanGreaterThanEqualsToken,
                                           opToken2->GetTrailingTrivia());
            opTokenErrorWidth = opToken->Width();
        }
    }

    SyntaxKind opKind = opToken->kind;
    GreenNode *paramList = ParseParenthesizedParameterList(false);
    int parameterCount = SeparatedSyntaxList(GetField(paramList, Field::Parameters)).Count();

    switch (parameterCount) {
        case 1:
            if (opToken->IsMissing() || !(SyntaxFacts::IsOverloadableUnaryOperator(opKind) ||
                                          SyntaxFacts::IsOverloadableCompoundAssignmentOperator(opKind))) {
                SyntaxDiagnosticInfo diagInfo =
                    MakeError(opTokenErrorOffset, opTokenErrorWidth, ErrorCode::ERR_OvlUnaryOperatorExpected);
                opToken = WithAdditionalDiagnostics(opToken, diagInfo);
            }

            break;
        case 2:
            if (opToken->IsMissing() || !SyntaxFacts::IsOverloadableBinaryOperator(opKind)) {
                SyntaxDiagnosticInfo diagInfo =
                    MakeError(opTokenErrorOffset, opTokenErrorWidth, ErrorCode::ERR_OvlBinaryOperatorExpected);
                opToken = WithAdditionalDiagnostics(opToken, diagInfo);
            }

            break;
        default:
            if (opToken->IsMissing()) {
                SyntaxDiagnosticInfo diagInfo =
                    MakeError(opTokenErrorOffset, opTokenErrorWidth, ErrorCode::ERR_OvlOperatorExpected);
                opToken = WithAdditionalDiagnostics(opToken, diagInfo);
            } else if (SyntaxFacts::IsOverloadableBinaryOperator(opKind)) {
                opToken = AddError(opToken, ErrorCode::ERR_BadBinOpArgs, {SyntaxFacts::GetText(opKind)});
            } else if (SyntaxFacts::IsOverloadableUnaryOperator(opKind)) {
                if (!(opKind == SyntaxKind::PlusPlusToken || opKind == SyntaxKind::MinusMinusToken) ||
                    parameterCount != 0) {
                    opToken = AddError(opToken, ErrorCode::ERR_BadUnOpArgs, {SyntaxFacts::GetText(opKind)});
                }
            } else if (SyntaxFacts::IsOverloadableCompoundAssignmentOperator(opKind)) {
                opToken = AddError(opToken, ErrorCode::ERR_BadCompoundAssignmentOpArgs, {SyntaxFacts::GetText(opKind)});
            } else {
                opToken = AddError(opToken, ErrorCode::ERR_OvlOperatorExpected);
            }

            break;
    }

    GreenNode *blockBody = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    ParseBlockAndExpressionBodiesWithSemicolon(blockBody, expressionBody, semicolon);

    // if the operator is invalid, then switch it to plus (which will work either way) so that
    // we can finish building the tree
    if (!(opKind == SyntaxKind::IsKeyword || SyntaxFacts::IsOverloadableUnaryOperator(opKind) ||
          SyntaxFacts::IsOverloadableBinaryOperator(opKind) ||
          SyntaxFacts::IsOverloadableCompoundAssignmentOperator(opKind))) {
        opToken = ConvertToMissingWithTrailingTrivia(opToken, SyntaxKind::PlusToken);
    }

    return SyntaxFactory::OperatorDeclaration(attributes, modifiers.ToList(), type, explicitInterfaceOpt, opKeyword,
                                              checkedKeyword, opToken, paramList, blockBody, expressionBody,
                                              semicolon);
}

GreenNode *LanguageParser::ParseIndexerDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                   GreenNode *type, GreenNode *explicitInterfaceOpt,
                                                   GreenNode *thisKeyword, GreenNode *typeParameterList) {
    CS_ASSERT(thisKeyword->kind == SyntaxKind::ThisKeyword);

    // check to see if the user tried to create a generic indexer.
    if (typeParameterList != nullptr) {
        thisKeyword = AddTrailingSkippedSyntax(thisKeyword, typeParameterList);
        thisKeyword = AddError(thisKeyword, ErrorCode::ERR_UnexpectedGenericName);
    }

    GreenNode *parameterList = ParseBracketedParameterList();

    GreenNode *accessorList = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;
    // Try to parse accessor list unless there is an expression
    // body and no accessor list
    if (CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken) {
        expressionBody = ParseArrowExpressionClause();
        semicolon = EatToken(SyntaxKind::SemicolonToken);
    } else {
        accessorList = ParseAccessorList(AccessorDeclaringKind::Indexer);
        if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
            semicolon = EatTokenWithPrejudice(ErrorCode::ERR_UnexpectedSemicolon);
        }
    }

    // If the user has erroneously provided both an accessor list
    // and an expression body, but no semicolon, we want to parse
    // the expression body and report the error (which is done later)
    if (CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken && semicolon == nullptr) {
        expressionBody = ParseArrowExpressionClause();
        semicolon = EatToken(SyntaxKind::SemicolonToken);
    }

    return SyntaxFactory::IndexerDeclaration(attributes, modifiers.ToList(), type, explicitInterfaceOpt, thisKeyword,
                                             parameterList, accessorList, expressionBody, semicolon);
}

GreenNode *LanguageParser::ParsePropertyDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                    GreenNode *type, GreenNode *explicitInterfaceOpt,
                                                    GreenNode *identifier, GreenNode *typeParameterList) {
    // check to see if the user tried to create a generic property.
    if (typeParameterList != nullptr) {
        identifier = AddTrailingSkippedSyntax(identifier, typeParameterList);
        identifier = AddError(identifier, ErrorCode::ERR_UnexpectedGenericName);
    }

    // Error recovery: add an errant semicolon to the identifier token and keep going.
    if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        identifier = AddTrailingSkippedSyntax(identifier, EatTokenEvenWithIncorrectKind(SyntaxKind::OpenBraceToken));
    }

    // We know we are parsing a property because we have seen either an open brace or an arrow token
    CS_ASSERT(IsStartOfPropertyBody(0));

    GreenNode *accessorList =
        CurrentToken()->kind == SyntaxKind::OpenBraceToken ? ParseAccessorList(AccessorDeclaringKind::Property) : nullptr;

    GreenNode *expressionBody = nullptr;
    GreenNode *initializer = nullptr;

    // Check for expression body
    if (CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken) {
        ParserSyntaxContextResetter _(this, std::nullopt, std::nullopt, /*isInFieldKeywordContext:*/ true);
        expressionBody = ParseArrowExpressionClause();
    }
    // Check if we have an initializer
    else if (CurrentToken()->kind == SyntaxKind::EqualsToken) {
        GreenNode *equals = EatToken(SyntaxKind::EqualsToken);
        GreenNode *value = ParseVariableInitializer();
        initializer = SyntaxFactory::EqualsValueClause(equals, value);
    }

    GreenNode *semicolon = nullptr;
    if (expressionBody != nullptr || initializer != nullptr) {
        semicolon = EatToken(SyntaxKind::SemicolonToken);
    } else if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        semicolon = EatTokenWithPrejudice(ErrorCode::ERR_UnexpectedSemicolon);
    }

    return SyntaxFactory::PropertyDeclaration(attributes, modifiers.ToList(), type, explicitInterfaceOpt, identifier,
                                              accessorList, expressionBody, initializer, semicolon);
}

// (ParserSyntaxContextResetter, AccessorDeclaringKind: csharp_parser_internal.h / csharp_parser_types.inc.)

GreenNode *LanguageParser::ParseAccessorList(AccessorDeclaringKind declaringKind) {
    GreenNode *openBrace = EatToken(SyntaxKind::OpenBraceToken);
    GreenNode *accessors = nullptr;  // default(SyntaxList<AccessorDeclarationSyntax>)

    if (!openBrace->IsMissing() || !IsTerminator()) {
        // parse property accessors
        SyntaxListBuilder builder = _pool.Allocate();

        while (true) {
            if (CurrentToken()->kind == SyntaxKind::CloseBraceToken) {
                break;
            } else if (IsPossibleAccessor()) {
                GreenNode *acc = ParseAccessorDeclaration(declaringKind);
                builder.Add(acc);
            } else if (SkipBadAccessorListTokens(openBrace, builder,
                                                 declaringKind == AccessorDeclaringKind::Event
                                                     ? ErrorCode::ERR_AddOrRemoveExpected
                                                     : ErrorCode::ERR_GetOrSetExpected) == PostSkipAction::Abort) {
                break;
            }
        }

        accessors = _pool.ToListAndFree(builder);
    }

    GreenNode *closeBrace = EatToken(SyntaxKind::CloseBraceToken);
    return SyntaxFactory::AccessorList(openBrace, accessors, closeBrace);
}

GreenNode *LanguageParser::ParseArrowExpressionClause() {
    GreenNode *arrow = EatToken(SyntaxKind::EqualsGreaterThanToken);
    GreenNode *expression = ParsePossibleRefExpression();
    return SyntaxFactory::ArrowExpressionClause(arrow, expression);
}

GreenNode *LanguageParser::ParsePossibleRefExpression() {
    // check for lambda expression with explicit ref return type: `ref int () => { ... }`
    GreenNode *refKeyword =
        CurrentToken()->kind == SyntaxKind::RefKeyword && !IsPossibleLambdaExpression(Precedence::Expression)
            ? EatToken()
            : nullptr;

    GreenNode *expression = ParseExpressionCore();
    return refKeyword == nullptr ? expression : SyntaxFactory::RefExpression(refKeyword, expression);
}

LanguageParser::PostSkipAction LanguageParser::SkipBadAccessorListTokens(GreenNode *&openBrace, SyntaxListBuilder list,
                                                         ErrorCode error) {
    return SkipBadListTokensWithErrorCode(
        openBrace, list,
        [](LanguageParser *p) { return p->CurrentToken()->kind != SyntaxKind::CloseBraceToken && !p->IsPossibleAccessor(); },
        [](LanguageParser *p) { return p->IsTerminator(); }, error);
}

bool LanguageParser::IsPossibleAccessor() {
    return CurrentToken()->kind == SyntaxKind::IdentifierToken || IsPossibleAttributeDeclaration() ||
           SyntaxFacts::GetAccessorDeclarationKind(CurrentToken()->contextual_kind) != SyntaxKind::None ||
           CurrentToken()->kind == SyntaxKind::OpenBraceToken  // for accessor blocks w/ missing keyword
           || CurrentToken()->kind == SyntaxKind::SemicolonToken  // for empty body accessors w/ missing keyword
           || IsPossibleAccessorModifier();
}

bool LanguageParser::IsPossibleAccessorModifier() {
    // We only want to accept a modifier as the start of an accessor if the modifiers are
    // actually followed by "get/set/add/remove".  Otherwise, we might thing think we're
    // starting an accessor when we're actually starting a normal class member.  For example:
    //
    //      class C {
    //          public int Prop { get { this.
    //          private DateTime x;
    //
    // We don't want to think of the "private" in "private DateTime x" as starting an accessor
    // here.  If we do, we'll get totally thrown off in parsing the remainder and that will
    // throw off the rest of the features that depend on a good syntax tree.
    //
    // Note: we allow all modifiers here.  That's because we want to parse things like
    // "abstract get" as an accessor.  This way we can provide a good error message
    // to the user that this is not allowed.

    if (GetModifierExcludingScoped(CurrentToken()) == DeclarationModifiers::None) {
        return false;
    }

    int peekIndex = 1;
    while (GetModifierExcludingScoped(PeekToken(peekIndex)) != DeclarationModifiers::None) {
        peekIndex++;
    }

    GreenNode *token = PeekToken(peekIndex);
    if (token->kind == SyntaxKind::CloseBraceToken || token->kind == SyntaxKind::EndOfFileToken) {
        // If we see "{ get { } public }
        // then we will think that "public" likely starts an accessor.
        return true;
    }

    switch (token->contextual_kind) {
        case SyntaxKind::GetKeyword:
        case SyntaxKind::SetKeyword:
        case SyntaxKind::InitKeyword:
        case SyntaxKind::AddKeyword:
        case SyntaxKind::RemoveKeyword:
            return true;
        default:
            return false;
    }
}

// (PostSkipAction: csharp_parser_types.inc.)

LanguageParser::PostSkipAction LanguageParser::SkipBadSeparatedListTokensWithExpectedKind(
    GreenNode *&startToken, SyntaxListBuilder list, const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *, SyntaxKind)> &abortFunction, SyntaxKind expected, SyntaxKind closeKind) {
    // We're going to cheat here and pass the underlying SyntaxListBuilder of "list" to the helper method so that
    // it can append skipped trivia to the last element, regardless of whether that element is a node or a token.
    GreenNode *trailingTrivia = nullptr;
    PostSkipAction action = SkipBadListTokensWithExpectedKindHelper(list, isNotExpectedFunction, abortFunction,
                                                                    expected, closeKind, trailingTrivia);
    if (trailingTrivia != nullptr) {
        startToken = AddTrailingSkippedSyntax(startToken, trailingTrivia);
    }
    return action;
}

LanguageParser::PostSkipAction LanguageParser::SkipBadListTokensWithErrorCode(
    GreenNode *&startToken, SyntaxListBuilder list, const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *)> &abortFunction, ErrorCode error) {
    GreenNode *trailingTrivia = nullptr;
    PostSkipAction action =
        SkipBadListTokensWithErrorCodeHelper(list, isNotExpectedFunction, abortFunction, error, trailingTrivia);
    if (trailingTrivia != nullptr) {
        startToken = AddTrailingSkippedSyntax(startToken, trailingTrivia);
    }
    return action;
}

/// WARNING: it is possible that "list" is really the underlying builder of a SeparateSyntaxListBuilder,
/// so it is important that we not add anything to the list.
LanguageParser::PostSkipAction LanguageParser::SkipBadListTokensWithExpectedKindHelper(
    SyntaxListBuilder list, const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *, SyntaxKind)> &abortFunction, SyntaxKind expected, SyntaxKind closeKind,
    GreenNode *&trailingTrivia) {
    if (list.Count() == 0) {
        return SkipBadTokensWithExpectedKind(isNotExpectedFunction, abortFunction, expected, closeKind,
                                             trailingTrivia);
    } else {
        GreenNode *lastItemTrailingTrivia = nullptr;
        PostSkipAction action = SkipBadTokensWithExpectedKind(isNotExpectedFunction, abortFunction, expected,
                                                              closeKind, lastItemTrailingTrivia);
        if (lastItemTrailingTrivia != nullptr) {
            AddTrailingSkippedSyntax(list, lastItemTrailingTrivia);
        }
        trailingTrivia = nullptr;
        return action;
    }
}

LanguageParser::PostSkipAction LanguageParser::SkipBadListTokensWithErrorCodeHelper(
    SyntaxListBuilder list, const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *)> &abortFunction, ErrorCode error, GreenNode *&trailingTrivia) {
    if (list.Count() == 0) {
        return SkipBadTokensWithErrorCode(isNotExpectedFunction, abortFunction, error, trailingTrivia);
    } else {
        GreenNode *lastItemTrailingTrivia = nullptr;
        PostSkipAction action =
            SkipBadTokensWithErrorCode(isNotExpectedFunction, abortFunction, error, lastItemTrailingTrivia);
        if (lastItemTrailingTrivia != nullptr) {
            AddTrailingSkippedSyntax(list, lastItemTrailingTrivia);
        }
        trailingTrivia = nullptr;
        return action;
    }
}

LanguageParser::PostSkipAction LanguageParser::SkipBadTokensWithExpectedKind(
    const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *, SyntaxKind)> &abortFunction, SyntaxKind expected, SyntaxKind closeKind,
    GreenNode *&trailingTrivia) {
    SyntaxListBuilder nodes = _pool.Allocate();
    bool first = true;
    PostSkipAction action = PostSkipAction::Continue;
    while (isNotExpectedFunction(this)) {
        if (abortFunction(this, closeKind) || IsTerminator()) {
            action = PostSkipAction::Abort;
            break;
        }

        GreenNode *token =
            (first && !CurrentToken()->ContainsDiagnostics()) ? EatTokenEvenWithIncorrectKind(expected) : EatToken();
        first = false;
        nodes.Add(token);
    }

    trailingTrivia = _pool.ToListAndFree(nodes);  // _pool.ToTokenListAndFree(nodes).Node
    return action;
}

LanguageParser::PostSkipAction LanguageParser::SkipBadTokensWithErrorCode(
    const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *)> &abortFunction, ErrorCode errorCode, GreenNode *&trailingTrivia) {
    SyntaxListBuilder nodes = _pool.Allocate();
    bool first = true;
    PostSkipAction action = PostSkipAction::Continue;
    while (isNotExpectedFunction(this)) {
        if (abortFunction(this)) {
            action = PostSkipAction::Abort;
            break;
        }

        GreenNode *token =
            (first && !CurrentToken()->ContainsDiagnostics()) ? EatTokenWithPrejudice(errorCode) : EatToken();
        first = false;
        nodes.Add(token);
    }

    trailingTrivia = _pool.ToListAndFree(nodes);  // _pool.ToTokenListAndFree(nodes).Node
    return action;
}

GreenNode *LanguageParser::ParseAccessorDeclaration(AccessorDeclaringKind declaringKind) {
    // (IsIncrementalAndFactoryContextMatches && IsAccessorDeclaration(CurrentNodeKind): no incremental parsing.)

    ParserSyntaxContextResetter _1(this, std::nullopt, std::nullopt,
                                   /*isInFieldKeywordContext:*/ declaringKind == AccessorDeclaringKind::Property);

    SyntaxListBuilder accMods = _pool.Allocate();

    SyntaxList accAttrs = ParseAttributeDeclarations(false);
    bool ignoredIsPossibleTypeDeclaration = false;
    ParseModifiers(accMods, true, false, ignoredIsPossibleTypeDeclaration);

    GreenNode *accessorName =
        EatToken(SyntaxKind::IdentifierToken, declaringKind == AccessorDeclaringKind::Event
                                                  ? ErrorCode::ERR_AddOrRemoveExpected
                                                  : ErrorCode::ERR_GetOrSetExpected);
    SyntaxKind accessorKind = GetAccessorKind(accessorName);

    // Only convert the identifier to a keyword if it's a valid one.  Otherwise any
    // other contextual keyword (like 'partial') will be converted into a keyword
    // and will be invalid.
    if (accessorKind == SyntaxKind::UnknownAccessorDeclaration) {
        // We'll have an UnknownAccessorDeclaration either because we didn't have
        // an IdentifierToken or because we have an IdentifierToken which is not
        // add/remove/get/set.  In the former case, we'll already have reported
        // an error and will have a missing token.  But in the latter case we need
        // to report that the identifier is incorrect.
        if (!accessorName->IsMissing()) {
            accessorName = AddError(accessorName, declaringKind == AccessorDeclaringKind::Event
                                                      ? ErrorCode::ERR_AddOrRemoveExpected
                                                      : ErrorCode::ERR_GetOrSetExpected);
        } else {
            CS_ASSERT(accessorName->ContainsDiagnostics());
        }
    } else {
        accessorName = ConvertToKeyword(accessorName);
    }

    GreenNode *blockBody = nullptr;
    GreenNode *expressionBody = nullptr;
    GreenNode *semicolon = nullptr;

    bool currentTokenIsSemicolon = CurrentToken()->kind == SyntaxKind::SemicolonToken;
    bool currentTokenIsArrow = CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken;
    bool currentTokenIsOpenBraceToken = CurrentToken()->kind == SyntaxKind::OpenBraceToken;

    if (currentTokenIsOpenBraceToken || currentTokenIsArrow) {
        ParseBlockAndExpressionBodiesWithSemicolon(blockBody, expressionBody, semicolon);
    } else if (currentTokenIsSemicolon) {
        semicolon = EatAccessorSemicolon();
    } else {
        // We didn't get something we recognized.  If we got an accessor type we
        // recognized (i.e. get/set/init/add/remove) then try to parse out a block.
        // Only do this if it doesn't seem like we're at the end of the accessor/property.
        // for example, if we have "get set", don't actually try to parse out the
        // block.  Otherwise we'll consume the 'set'.  In that case, just end the
        // current accessor with a semicolon so we can properly consume the next
        // in the calling method's loop.
        if (accessorKind != SyntaxKind::UnknownAccessorDeclaration) {
            if (!IsTerminator()) {
                blockBody = ParseMethodOrAccessorBodyBlock(nullptr, true);
            } else {
                semicolon = EatAccessorSemicolon();
            }
        } else {
            // Don't bother eating anything if we didn't even have a valid accessor.
            // It will just lead to more errors.  Note: we will have already produced
            // a good error by now.
            CS_ASSERT(accessorName->ContainsDiagnostics());
        }
    }

    GreenNode *modifierList = _pool.ToListAndFree(accMods);  // _pool.ToTokenListAndFree(accMods)
    return SyntaxFactory::AccessorDeclaration(accessorKind, accAttrs, modifierList, accessorName, blockBody,
                                              expressionBody, semicolon);
}

GreenNode *LanguageParser::EatAccessorSemicolon() {
    return EatToken(SyntaxKind::SemicolonToken, IsFeatureEnabled() /* IDS_FeatureExpressionBodiedAccessor */
                                                    ? ErrorCode::ERR_SemiOrLBraceOrArrowExpected
                                                    : ErrorCode::ERR_SemiOrLBraceExpected);
}

SyntaxKind LanguageParser::GetAccessorKind(GreenNode *accessorName) {
    switch (accessorName->contextual_kind) {
        case SyntaxKind::GetKeyword:
            return SyntaxKind::GetAccessorDeclaration;
        case SyntaxKind::SetKeyword:
            return SyntaxKind::SetAccessorDeclaration;
        case SyntaxKind::InitKeyword:
            return SyntaxKind::InitAccessorDeclaration;
        case SyntaxKind::AddKeyword:
            return SyntaxKind::AddAccessorDeclaration;
        case SyntaxKind::RemoveKeyword:
            return SyntaxKind::RemoveAccessorDeclaration;
        default:
            return SyntaxKind::UnknownAccessorDeclaration;
    }
}

GreenNode *LanguageParser::ParseParenthesizedParameterList(bool forExtensionOrUnion) {
    // (IsIncrementalAndFactoryContextMatches && CanReuseParameterList(...): no incremental parsing.)

    GreenNode *open = nullptr;
    GreenNode *close = nullptr;
    SeparatedSyntaxList parameters =
        ParseParameterList(open, close, SyntaxKind::OpenParenToken, SyntaxKind::CloseParenToken, forExtensionOrUnion);
    return SyntaxFactory::ParameterList(open, parameters, close);
}

GreenNode *LanguageParser::ParseBracketedParameterList() {
    // (IsIncrementalAndFactoryContextMatches && CanReuseBracketedParameterList(...): no incremental parsing.)

    GreenNode *open = nullptr;
    GreenNode *close = nullptr;
    SeparatedSyntaxList parameters =
        ParseParameterList(open, close, SyntaxKind::OpenBracketToken, SyntaxKind::CloseBracketToken, false);
    return SyntaxFactory::BracketedParameterList(open, parameters, close);
}

// CanReuseParameterList, CanReuseBracketedParameterList: incremental parsing only, not ported.

SeparatedSyntaxList LanguageParser::ParseParameterList(GreenNode *&open, GreenNode *&close, SyntaxKind openKind,
                                                       SyntaxKind closeKind, bool forExtensionOrUnion) {
    open = EatToken(openKind);

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsEndOfParameterList;

    ParseElementFn parseElement;
    if (forExtensionOrUnion) {
        parseElement = [](LanguageParser *self) { return self->ParseParameter(true); };
    } else {
        parseElement = [](LanguageParser *self) { return self->ParseParameter(false); };
    }

    auto skipBadParameterListTokens = [](LanguageParser *self, GreenNode *&openToken, SyntaxListBuilder list,
                                         SyntaxKind expectedKind, SyntaxKind closeTokenKind) -> PostSkipAction {
        return self->SkipBadSeparatedListTokensWithExpectedKind(
            openToken, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleParameter();
            },
            [](LanguageParser *p, SyntaxKind ck) { return p->CurrentToken()->kind == ck; }, expectedKind,
            closeTokenKind);
    };

    SeparatedSyntaxList parameters = ParseCommaSeparatedSyntaxList(
        open, closeKind, [](LanguageParser *self) { return self->IsPossibleParameter(); }, parseElement,
        skipBadParameterListTokens, /*allowTrailingSeparator:*/ false,
        /*requireOneElement:*/ forExtensionOrUnion,  // For extension/union declarations, we require at least one parameter
        /*allowSemicolonAsSeparator:*/ false);

    _termState = saveTerm;
    close = EatToken(closeKind);

    return parameters;
}

bool LanguageParser::IsEndOfParameterList() {
    SyntaxKind kind = CurrentToken()->kind;
    return kind == SyntaxKind::CloseParenToken || kind == SyntaxKind::CloseBracketToken ||
           kind == SyntaxKind::SemicolonToken;
}

bool LanguageParser::IsPossibleParameter() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::OpenBracketToken:  // attribute
        case SyntaxKind::ArgListKeyword:
        case SyntaxKind::OpenParenToken:  // tuple
            return true;

        case SyntaxKind::IdentifierToken:
            return IsTrueIdentifier();

        default:
            // case SyntaxKind.DelegateKeyword when IsFunctionPointerStart(): // Function pointer type
            if (CurrentToken()->kind == SyntaxKind::DelegateKeyword && IsFunctionPointerStart()) {
                return true;
            }
            return IsParameterModifierExcludingScoped(CurrentToken()) || IsDefiniteScopedModifier(false, false) ||
                   IsPredefinedType(CurrentToken()->kind);
    }
}

// CanReuseParameter: incremental parsing only, not ported.

GreenNode *LanguageParser::ParseParameter(bool identifierIsOptional) {
    // (IsIncrementalAndFactoryContextMatches && CanReuseParameter(...): no incremental parsing.)

    SyntaxList attributes = ParseAttributeDeclarations(false);

    SyntaxListBuilder modifiers = _pool.Allocate();
    ParseParameterModifiers(modifiers, false, false);

    if (CurrentToken()->kind == SyntaxKind::ArgListKeyword) {
        // We store an __arglist parameter as a parameter with null type and whose
        // .Identifier has the kind ArgListKeyword.
        GreenNode *modifierList = modifiers.ToList();
        GreenNode *argList = EatToken(SyntaxKind::ArgListKeyword);
        return SyntaxFactory::Parameter(attributes, modifierList, /*type:*/ nullptr, argList, /*@default:*/ nullptr);
    }

    GreenNode *type = ParseType(ParseTypeMode::Parameter);

    GreenNode *identifier;
    if (CurrentToken()->kind == SyntaxKind::IdentifierToken && IsCurrentTokenWhereOfConstraintClause()) {
        identifier = identifierIsOptional
                         ? nullptr
                         : AddError(CreateMissingIdentifierToken(), ErrorCode::ERR_IdentifierExpected);
    } else {
        // The receiver parameter on an extension declaration may have a name or not
        identifier = identifierIsOptional && CurrentToken()->kind != SyntaxKind::IdentifierToken
                         ? nullptr
                         : ParseIdentifierToken();
    }

    // When the user type "int goo[]", give them a useful error
    if (identifier != nullptr && CurrentToken()->kind == SyntaxKind::OpenBracketToken &&
        PeekToken(1)->kind == SyntaxKind::CloseBracketToken) {
        GreenNode *openBracket = AddError(EatToken(), ErrorCode::ERR_BadArraySyntax);
        GreenNode *closeBracket = EatToken();
        identifier = AddTrailingSkippedSyntax(identifier, SyntaxFactory::List(openBracket, closeBracket));
    }

    GreenNode *equalsToken = TryEatToken(SyntaxKind::EqualsToken);

    GreenNode *modifierList = _pool.ToListAndFree(modifiers);  // _pool.ToTokenListAndFree(modifiers)
    GreenNode *defaultValue = nullptr;
    if (equalsToken != nullptr) {
        GreenNode *value = ParseExpressionCore();
        defaultValue = SyntaxFactory::EqualsValueClause(equalsToken, value);
    }
    return SyntaxFactory::Parameter(attributes, modifierList, type, identifier, defaultValue);
}

bool LanguageParser::NoTriviaBetween(GreenNode *token1, GreenNode *token2) {
    return token1->GetTrailingTriviaWidth() == 0 && token2->GetLeadingTriviaWidth() == 0;
}

bool LanguageParser::IsParameterModifierIncludingScoped(GreenNode *token) {
    return IsParameterModifierExcludingScoped(token) || token->contextual_kind == SyntaxKind::ScopedKeyword;
}

bool LanguageParser::IsParameterModifierExcludingScoped(GreenNode *token) {
    switch (token->kind) {
        case SyntaxKind::ThisKeyword:
        case SyntaxKind::RefKeyword:
        case SyntaxKind::OutKeyword:
        case SyntaxKind::InKeyword:
        case SyntaxKind::ParamsKeyword:
        case SyntaxKind::ReadOnlyKeyword:
            return true;
        default:
            break;
    }

    return false;
}

void LanguageParser::ParseParameterModifiers(SyntaxListBuilder modifiers, bool isFunctionPointerParameter,
                                             bool isLambdaParameter) {
    CS_ASSERT(!(isFunctionPointerParameter && isLambdaParameter));

    bool seenScoped = false;
    while (true) {
        // Normal keyword-modifier (in/out/ref/readonly/params/this).  Always safe to consume.
        if (IsParameterModifierExcludingScoped(CurrentToken())) {
            modifiers.Add(EatToken());
            continue;
        }

        // 'scoped' modifier.  May be ambiguous with a type/identifier.  And has changed parsing rules between
        // C#13/14 inside a lambda parameter list.
        if (IsDefiniteScopedModifier(isFunctionPointerParameter, isLambdaParameter)) {
            // First scoped-modifier is always considered the modifier.
            if (!seenScoped) {
                seenScoped = true;
                modifiers.Add(EatContextualToken(SyntaxKind::ScopedKeyword));
                continue;
            } else {
                // If we've already seen `scoped` then we may have a situation like `scoped scoped`. This could
                // be duplicated modifier, or it could be that the second `scoped` is actually the identifier of
                // a parameter.
                //
                // Places where it is an identifier are:
                //
                //      `(scoped scoped) =>`
                //      `(scoped scoped, ...) =>`
                //      `(scoped scoped = ...) =>`
                SyntaxKind next = PeekToken(1)->kind;
                if (!(next == SyntaxKind::CloseParenToken || next == SyntaxKind::CommaToken ||
                      next == SyntaxKind::EqualsToken)) {
                    modifiers.Add(EatContextualToken(SyntaxKind::ScopedKeyword));
                    continue;
                }
            }
        }

        // Not a modifier.  We're done.
        return;
    }
}

GreenNode *LanguageParser::ParseFixedSizeBufferDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                           SyntaxKind parentKind) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::FixedKeyword);

    modifiers.Add(EatToken());

    GreenNode *type = ParseType();

    GreenNode *modifierList = modifiers.ToList();
    SeparatedSyntaxList variables = ParseFieldDeclarationVariableDeclarators(type, VariableFlags::Fixed, parentKind);
    GreenNode *declaration = SyntaxFactory::VariableDeclaration(type, variables);
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::FieldDeclaration(attributes, modifierList, declaration, semicolon);
}

GreenNode *LanguageParser::ParseEventDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                 SyntaxKind parentKind) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::EventKeyword);

    GreenNode *eventToken = EatToken();
    GreenNode *type = ParseType();

    return IsFieldDeclaration(true, parentKind == SyntaxKind::CompilationUnit)
               ? ParseEventFieldDeclaration(attributes, modifiers, eventToken, type, parentKind)
               : ParseEventDeclarationWithAccessors(attributes, modifiers, eventToken, type);
}

GreenNode *LanguageParser::ParseEventDeclarationWithAccessors(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                              GreenNode *eventToken, GreenNode *type) {
    GreenNode *explicitInterfaceOpt = nullptr;
    GreenNode *identifierOrThisOpt = nullptr;
    GreenNode *typeParameterList = nullptr;

    ParseMemberName(explicitInterfaceOpt, identifierOrThisOpt, typeParameterList, true);

    // If we got an explicitInterfaceOpt but not an identifier, then we're in the special
    // case for ERR_ExplicitEventFieldImpl (see ParseMemberName for details).
    if (explicitInterfaceOpt != nullptr && CurrentToken()->kind != SyntaxKind::OpenBraceToken &&
        CurrentToken()->kind != SyntaxKind::SemicolonToken) {
        CS_ASSERT(typeParameterList == nullptr);
        return SyntaxFactory::EventDeclaration(
            attributes, modifiers.ToList(), eventToken, type,
            // already has an appropriate error attached
            explicitInterfaceOpt,
            // No need for a diagnostic, ParseMemberName has already added one.
            identifierOrThisOpt == nullptr ? CreateMissingIdentifierToken() : identifierOrThisOpt,
            SyntaxFactory::AccessorList(SyntaxFactory::MissingToken(SyntaxKind::OpenBraceToken), nullptr,
                                        SyntaxFactory::MissingToken(SyntaxKind::CloseBraceToken)),
            /*semicolonToken:*/ nullptr);
    }

    GreenNode *identifier;

    if (identifierOrThisOpt == nullptr) {
        identifier = CreateMissingIdentifierToken();
    } else if (identifierOrThisOpt->kind != SyntaxKind::IdentifierToken) {
        CS_ASSERT(identifierOrThisOpt->kind == SyntaxKind::ThisKeyword);
        identifier = ConvertToMissingWithTrailingTrivia(identifierOrThisOpt, SyntaxKind::IdentifierToken);
    } else {
        identifier = identifierOrThisOpt;
    }

    CS_ASSERT(identifier != nullptr);
    CS_ASSERT(identifier->kind == SyntaxKind::IdentifierToken);

    if (identifier->IsMissing() && !type->IsMissing()) {
        identifier = AddError(identifier, ErrorCode::ERR_IdentifierExpected);
    }

    if (typeParameterList != nullptr)  // check to see if the user tried to create a generic event.
    {
        identifier = AddTrailingSkippedSyntax(identifier, typeParameterList);
        identifier = AddError(identifier, ErrorCode::ERR_UnexpectedGenericName);
    }

    GreenNode *accessorList = nullptr;
    GreenNode *semicolon = nullptr;

    if (explicitInterfaceOpt != nullptr && CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        semicolon = EatToken(SyntaxKind::SemicolonToken);
    } else {
        accessorList = ParseAccessorList(AccessorDeclaringKind::Event);
    }

    GreenNode *decl = SyntaxFactory::EventDeclaration(attributes, modifiers.ToList(), eventToken, type,
                                                      explicitInterfaceOpt, identifier, accessorList, semicolon);

    decl = EatUnexpectedTrailingSemicolon(decl);

    return decl;
}

GreenNode *LanguageParser::EatUnexpectedTrailingSemicolon(GreenNode *decl) {
    // allow for case of one unexpected semicolon...
    if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        GreenNode *semi = EatToken();
        semi = AddError(semi, ErrorCode::ERR_UnexpectedSemicolon);
        decl = AddTrailingSkippedSyntax(decl, semi);
    }

    return decl;
}

GreenNode *LanguageParser::ParseNormalFieldDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                       GreenNode *type, SyntaxKind parentKind) {
    SeparatedSyntaxList variables =
        ParseFieldDeclarationVariableDeclarators(type, VariableFlags::LocalOrField, parentKind);

    // Make 'scoped' part of the type when it is the last token in the modifiers list
    if (modifiers.Count() > 0 && modifiers[modifiers.Count() - 1]->IsToken() &&
        modifiers[modifiers.Count() - 1]->kind == SyntaxKind::ScopedKeyword) {
        GreenNode *scopedKeyword = modifiers[modifiers.Count() - 1];
        type = SyntaxFactory::ScopedType(scopedKeyword, type);
        modifiers.RemoveLast();
    }

    GreenNode *modifierList = modifiers.ToList();
    GreenNode *declaration = SyntaxFactory::VariableDeclaration(type, variables);
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::FieldDeclaration(attributes, modifierList, declaration, semicolon);
}

GreenNode *LanguageParser::ParseEventFieldDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers,
                                                      GreenNode *eventToken, GreenNode *type, SyntaxKind parentKind) {
    // An attribute specified on an event declaration that omits event accessors can apply
    // to the event being declared, to the associated field (if the event is not abstract),
    // or to the associated add and remove methods. In the absence of an
    // attribute-target-specifier, the attribute applies to the event. The presence of the
    // event attribute-target-specifier indicates that the attribute applies to the event;
    // the presence of the field attribute-target-specifier indicates that the attribute
    // applies to the field; and the presence of the method attribute-target-specifier
    // indicates that the attribute applies to the methods.
    //
    // NOTE(cyrusn): We allow more than the above here.  Specifically, even if the event is
    // abstract, we allow the attribute to specify that it belongs to a field.  Later, in the
    // semantic pass, we will disallow this.

    SeparatedSyntaxList variables = ParseFieldDeclarationVariableDeclarators(type, VariableFlags::None, parentKind);
    if (CurrentToken()->kind == SyntaxKind::DotToken) {
        // Better error message for confusing event situation.
        eventToken = AddError(eventToken, ErrorCode::ERR_ExplicitEventFieldImpl);
    }

    GreenNode *modifierList = modifiers.ToList();
    GreenNode *declaration = SyntaxFactory::VariableDeclaration(type, variables);
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::EventFieldDeclaration(attributes, modifierList, eventToken, declaration, semicolon);
}

}  // namespace csharp
