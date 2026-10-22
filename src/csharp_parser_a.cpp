// LanguageParser, unit A: a translation of Roslyn's LanguageParser.cs lines 1-2380 (constructor,
// IsTerminator, compilation unit, namespaces, extern alias / using directives, attributes, modifiers, type
// declarations, base lists, constraint clauses ... IsPossibleMemberStart). See csharp_parser_internal.h for
// the translation conventions and csharp_syntax.h for the Roslyn commit and license.

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief A C# `finally` block: runs the callable when the scope is left (returns and exceptions alike). */
template <class F>
struct Finally {
    F f;
    ~Finally() { f(); }
};
template <class F>
Finally(F) -> Finally<F>;

}  // namespace

// The C# constructor also takes oldTree / changes / cancellationToken (incremental parsing, not ported) and
// passes preLexIfNotIncremental: true to the base (the base's business here).
LanguageParser::LanguageParser(Lexer *lexer_, LexerMode lexerMode) : SyntaxParser(lexer_, lexerMode, false) {}

LanguageParser::~LanguageParser() = default;

bool LanguageParser::IsSomeWord(SyntaxKind kind) {
    return kind == SyntaxKind::IdentifierToken || SyntaxFacts::IsKeywordKind(kind);
}

// Parsing rule terminating conditions.  This is how we know if it is
// okay to abort the current parsing rule when unexpected tokens occur.

bool LanguageParser::IsTerminator() {
    if (CurrentToken()->kind == SyntaxKind::EndOfFileToken) {
        return true;
    }

    for (uint32_t i = 1; i <= LastTerminatorState; i <<= 1) {
        switch (_termState & static_cast<TerminatorState>(i)) {
            case TerminatorState::IsNamespaceMemberStartOrStop:
                if (IsNamespaceMemberStartOrStop()) return true;
                break;
            case TerminatorState::IsAttributeDeclarationTerminator:
                if (IsAttributeDeclarationTerminator()) return true;
                break;
            case TerminatorState::IsPossibleAggregateClauseStartOrStop:
                if (IsPossibleAggregateClauseStartOrStop()) return true;
                break;
            case TerminatorState::IsPossibleMemberStartOrStop:
                if (IsPossibleMemberStartOrStop()) return true;
                break;
            case TerminatorState::IsEndOfReturnType:
                if (IsEndOfReturnType()) return true;
                break;
            case TerminatorState::IsEndOfParameterList:
                if (IsEndOfParameterList()) return true;
                break;
            case TerminatorState::IsEndOfFieldDeclaration:
                if (IsEndOfFieldDeclaration()) return true;
                break;
            case TerminatorState::IsPossibleEndOfVariableDeclaration:
                if (IsPossibleEndOfVariableDeclaration()) return true;
                break;
            case TerminatorState::IsEndOfTypeArgumentList:
                if (IsEndOfTypeArgumentList()) return true;
                break;
            case TerminatorState::IsPossibleStatementStartOrStop:
                if (IsPossibleStatementStartOrStop()) return true;
                break;
            case TerminatorState::IsEndOfFixedStatement:
                if (IsEndOfFixedStatement()) return true;
                break;
            case TerminatorState::IsEndOfTryBlock:
                if (IsEndOfTryBlock()) return true;
                break;
            case TerminatorState::IsEndOfCatchClause:
                if (IsEndOfCatchClause()) return true;
                break;
            case TerminatorState::IsEndOfFilterClause:
                if (IsEndOfFilterClause()) return true;
                break;
            case TerminatorState::IsEndOfCatchBlock:
                if (IsEndOfCatchBlock()) return true;
                break;
            case TerminatorState::IsEndOfDoWhileExpression:
                if (IsEndOfDoWhileExpression()) return true;
                break;
            case TerminatorState::IsEndOfForStatementArgument:
                if (IsEndOfForStatementArgument()) return true;
                break;
            case TerminatorState::IsEndOfDeclarationClause:
                if (IsEndOfDeclarationClause()) return true;
                break;
            case TerminatorState::IsEndOfArgumentList:
                if (IsEndOfArgumentList()) return true;
                break;
            case TerminatorState::IsSwitchSectionStart:
                if (IsPossibleSwitchSection()) return true;
                break;
            case TerminatorState::IsEndOfTypeParameterList:
                if (IsEndOfTypeParameterList()) return true;
                break;
            case TerminatorState::IsEndOfMethodSignature:
                if (IsEndOfMethodSignature()) return true;
                break;
            case TerminatorState::IsEndOfNameInExplicitInterface:
                if (IsEndOfNameInExplicitInterface()) return true;
                break;
            case TerminatorState::IsEndOfFunctionPointerParameterList:
                if (IsEndOfFunctionPointerParameterList(/*errored:*/ false)) return true;
                break;
            case TerminatorState::IsEndOfFunctionPointerParameterListErrored:
                if (IsEndOfFunctionPointerParameterList(/*errored:*/ true)) return true;
                break;
            case TerminatorState::IsEndOfFunctionPointerCallingConvention:
                if (IsEndOfFunctionPointerCallingConvention()) return true;
                break;
            case TerminatorState::IsEndOfTypeSignature:
                if (IsEndOfTypeSignature()) return true;
                break;
            default:
                break;
        }
    }

    return false;
}

// GetOldParent (incremental parsing only): not ported. NamespaceBodyBuilder: csharp_parser_types.inc.

GreenNode *LanguageParser::ParseCompilationUnit() {
    return ParseWithStackGuard(
        [](LanguageParser *p) { return p->ParseCompilationUnitCore(); },
        [](LanguageParser *) {
            return SyntaxFactory::CompilationUnit(nullptr, nullptr, nullptr, nullptr,
                                                  SyntaxFactory::Token(SyntaxKind::EndOfFileToken));
        });
}

GreenNode *LanguageParser::ParseCompilationUnitCore() {
    GreenNode *tmp = nullptr;
    SyntaxListBuilder initialBadNodes;  // null
    NamespaceBodyBuilder body(_pool);
    Finally finally_{[&] { body.Free(_pool); }};

    ParseNamespaceBody(tmp, body, initialBadNodes, SyntaxKind::CompilationUnit);

    GreenNode *eof = EatToken(SyntaxKind::EndOfFileToken);
    GreenNode *result = SyntaxFactory::CompilationUnit(body.Externs, body.Usings, body.Attributes, body.Members, eof);

    if (!initialBadNodes.IsNull()) {
        // attach initial bad nodes as leading trivia on first token
        result = AddLeadingSkippedSyntax(result, initialBadNodes.ToListNode());
        _pool.Free(initialBadNodes);
    }

    return result;
}

GreenNode *LanguageParser::ParseWithStackGuard(const ParseElementFn &parseFunc,
                                               const ParseElementFn &createEmptyNodeFunc) {
    // If this value is non-zero then we are nesting calls to ParseWithStackGuard which should not be
    // happening.  It's not a bug but it's inefficient and should be changed.
    CS_ASSERT(_recursionDepth == 0);

    try {
        return parseFunc(this);
    } catch (const InsufficientExecutionStack &) {
        return CreateForGlobalFailure(lexer->TextWindow.Position(), createEmptyNodeFunc(this));
    }
}

GreenNode *LanguageParser::CreateForGlobalFailure(int position, GreenNode *node) {
    // Turn the complete input into a single skipped token. This avoids running the lexer, and therefore
    // the preprocessor directive parser, which may itself run into the same problem that caused the
    // original failure.
    // (A one-element list is the element itself.)
    GreenNode *badToken = SyntaxFactory::BadToken(nullptr, lexer->TextWindow.text, nullptr);
    GreenNode *fileAsTrivia = SyntaxFactory::SkippedTokensTrivia(badToken);
    node = AddLeadingSkippedSyntax(node, fileAsTrivia);
    ForceEndOfFile();  // force the scanner to report that it is at the end of the input.
    return AddError(node, position, 0, ErrorCode::ERR_InsufficientStack);
}

GreenNode *LanguageParser::ParseNamespaceDeclaration(SyntaxList attributeLists, SyntaxListBuilder modifiers) {
    _recursionDepth++;
    EnsureSufficientExecutionStack(_recursionDepth);
    GreenNode *result = ParseNamespaceDeclarationCore(attributeLists, modifiers);
    _recursionDepth--;
    return result;
}

GreenNode *LanguageParser::ParseNamespaceDeclarationCore(SyntaxList attributeLists, SyntaxListBuilder modifiers) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::NamespaceKeyword);
    GreenNode *namespaceToken = EatToken(SyntaxKind::NamespaceKeyword);

    if (IsScript()) {
        namespaceToken = AddError(namespaceToken, ErrorCode::ERR_NamespaceNotAllowedInScript);
    }

    GreenNode *name = ParseQualifiedName();

    GreenNode *openBrace = nullptr;
    GreenNode *semicolon = nullptr;

    if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        semicolon = EatToken(SyntaxKind::SemicolonToken);
    } else if (CurrentToken()->kind == SyntaxKind::OpenBraceToken || IsPossibleNamespaceMemberDeclaration()) {
        // either we see the brace we expect here or we see something that could come after a brace
        // so we insert a missing one
        openBrace = EatToken(SyntaxKind::OpenBraceToken);
    } else {
        // the next character is neither the brace we expect, nor a token that could follow the expected
        // brace so we assume it's a mistake and replace it with a missing brace
        openBrace = ConvertToMissingWithTrailingTrivia(EatTokenEvenWithIncorrectKind(SyntaxKind::OpenBraceToken),
                                                       SyntaxKind::OpenBraceToken);
    }

    CS_ASSERT(semicolon != nullptr || openBrace != nullptr);

    NamespaceBodyBuilder body(_pool);
    Finally finally_{[&] { body.Free(_pool); }};

    if (openBrace == nullptr) {
        CS_ASSERT(semicolon != nullptr);

        SyntaxListBuilder initialBadNodes;  // null
        ParseNamespaceBody(semicolon, body, initialBadNodes, SyntaxKind::FileScopedNamespaceDeclaration);
        CS_ASSERT(initialBadNodes.IsNull());  // init bad nodes should have been attached to semicolon...

        return SyntaxFactory::FileScopedNamespaceDeclaration(attributeLists, modifiers.ToList(), namespaceToken, name,
                                                             semicolon, body.Externs, body.Usings, body.Members);
    } else {
        SyntaxListBuilder initialBadNodes;  // null
        ParseNamespaceBody(openBrace, body, initialBadNodes, SyntaxKind::NamespaceDeclaration);
        CS_ASSERT(initialBadNodes.IsNull());  // init bad nodes should have been attached to open brace...

        // (C# evaluates the arguments left to right: close brace, then the optional semicolon.)
        GreenNode *closeBrace = EatToken(SyntaxKind::CloseBraceToken);
        GreenNode *trailingSemicolon = TryEatToken(SyntaxKind::SemicolonToken);
        return SyntaxFactory::NamespaceDeclaration(attributeLists, modifiers.ToList(), namespaceToken, name,
                                                   openBrace, body.Externs, body.Usings, body.Members, closeBrace,
                                                   trailingSemicolon);
    }
}

/// Are we possibly at the start of an attribute list, or at a modifier which is valid on a type, or on a keyword of a type declaration?
bool LanguageParser::IsPossibleStartOfTypeDeclaration(SyntaxKind kind) {
    return IsTypeModifierOrTypeKeyword(kind) || kind == SyntaxKind::OpenBracketToken;
}

/// Are we at a modifier which is valid on a type declaration or at a type keyword?
bool LanguageParser::IsTypeModifierOrTypeKeyword(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::EnumKeyword:
        case SyntaxKind::DelegateKeyword:
        case SyntaxKind::ClassKeyword:
        case SyntaxKind::InterfaceKeyword:
        case SyntaxKind::StructKeyword:
        case SyntaxKind::AbstractKeyword:
        case SyntaxKind::InternalKeyword:
        case SyntaxKind::NewKeyword:
        case SyntaxKind::PrivateKeyword:
        case SyntaxKind::ProtectedKeyword:
        case SyntaxKind::PublicKeyword:
        case SyntaxKind::SealedKeyword:
        case SyntaxKind::StaticKeyword:
        case SyntaxKind::UnsafeKeyword:
            return true;
        default:
            return false;
    }
}

void LanguageParser::AddSkippedNamespaceText(GreenNode *&openBraceOrSemicolon, NamespaceBodyBuilder &body,
                                             SyntaxListBuilder &initialBadNodes, GreenNode *skippedSyntax) {
    if (body.Members.Count() > 0) {
        AddTrailingSkippedSyntax(body.Members, skippedSyntax);
    } else if (body.Attributes.Count() > 0) {
        AddTrailingSkippedSyntax(body.Attributes, skippedSyntax);
    } else if (body.Usings.Count() > 0) {
        AddTrailingSkippedSyntax(body.Usings, skippedSyntax);
    } else if (body.Externs.Count() > 0) {
        AddTrailingSkippedSyntax(body.Externs, skippedSyntax);
    } else if (openBraceOrSemicolon != nullptr) {
        openBraceOrSemicolon = AddTrailingSkippedSyntax(openBraceOrSemicolon, skippedSyntax);
    } else {
        if (initialBadNodes.IsNull()) {
            initialBadNodes = _pool.Allocate();
        }

        initialBadNodes.AddRange(SyntaxList(skippedSyntax));
    }
}

// NamespaceParts: csharp_parser_types.inc.

void LanguageParser::ParseNamespaceBody(GreenNode *&openBraceOrSemicolon, NamespaceBodyBuilder &body,
                                        SyntaxListBuilder &initialBadNodes, SyntaxKind parentKind) {
    bool sawMemberDeclarationOnlyValidWithinTypeDeclaration = false;
    ParseNamespaceBodyWorker(openBraceOrSemicolon, body, initialBadNodes, parentKind,
                             sawMemberDeclarationOnlyValidWithinTypeDeclaration);

    // In the common case, we will not see errant type-only member declarations in the namespace itself.  In
    // that case, we have no extra work to do and can return immediately.
    //
    // If we do see errant type-only members (like a method/property/constructor/etc.), then see if they follow
    // some normal type declaration.  If so, it's likely there was a misplaced close curly that preemptively
    // ended the type declaration, and the member declaration was supposed to go in it instead.
    if (!sawMemberDeclarationOnlyValidWithinTypeDeclaration) return;

    // In a script file, it can be ok to have these members at the top level.  For example, a field is actually
    // ok to parse out at the top level as it will become a field on the script global object.
    if (IsScript() && parentKind == SyntaxKind::CompilationUnit) return;

    // (local functions)
    struct SiblingRange {
        int firstSiblingToMoveInclusive;
        int lastSiblingToMoveExclusive;
    };
    auto determineSiblingsToMoveIntoType = [](int typeDeclarationIndex,
                                              const NamespaceBodyBuilder &body_) -> std::optional<SiblingRange> {
        int startInclusive = typeDeclarationIndex + 1;
        if (startInclusive < body_.Members.Count() &&
            IsMemberDeclarationOnlyValidWithinTypeDeclaration(body_.Members[startInclusive])) {
            int endExclusive = startInclusive + 1;

            while (endExclusive < body_.Members.Count() &&
                   IsMemberDeclarationOnlyValidWithinTypeDeclaration(body_.Members[endExclusive])) {
                endExclusive++;
            }

            return SiblingRange{startInclusive, endExclusive};
        }

        return std::nullopt;
    };

    auto moveSiblingMembersIntoPrecedingType = [this](GreenNode *typeDeclaration, const NamespaceBodyBuilder &body_,
                                                      int firstSiblingToMoveInclusive,
                                                      int lastSiblingToMoveExclusive) -> GreenNode * {
        SyntaxListBuilder finalTypeDeclarationMembers = _pool.Allocate();
        finalTypeDeclarationMembers.AddRange(SyntaxList(GetField(typeDeclaration, Field::Members)));

        for (int memberToMoveIndex = firstSiblingToMoveInclusive; memberToMoveIndex < lastSiblingToMoveExclusive;
             memberToMoveIndex++) {
            GreenNode *currentSibling = body_.Members[memberToMoveIndex];
            if (memberToMoveIndex == firstSiblingToMoveInclusive) {
                // Move the existing close brace token to the first member as a skipped token, with a
                // diagnostic saying that it was unexpected.
                currentSibling = AddLeadingSkippedSyntax(
                    currentSibling,
                    AddError(GetField(typeDeclaration, Field::CloseBraceToken), ErrorCode::ERR_InvalidMemberDecl, {u"}"}));
            }

            finalTypeDeclarationMembers.Add(currentSibling);
        }

        bool isLast = lastSiblingToMoveExclusive == body_.Members.Count();

        // The existing close brace token is moved to the first member as a skipped token, with a diagnostic saying
        // it was unexpected.  The type decl will then get a missing close brace token if there are still members
        // following.  If not, we'll try to eat an actual close brace token.
        GreenNode *finalCloseBraceToken =
            isLast ? EatToken(SyntaxKind::CloseBraceToken)
                   : AddError(SyntaxFactory::MissingToken(SyntaxKind::CloseBraceToken), ErrorCode::ERR_RbraceExpected);
        GreenNode *newMembers = _pool.ToListAndFree(finalTypeDeclarationMembers);

        // typeDeclaration.UpdateCore(...) with new Members and CloseBraceToken (the node's diagnostics are kept,
        // as the green Update does).
        GreenNode *updated =
            WithSlot(typeDeclaration, FieldSlot(typeDeclaration->kind, Field::Members), newMembers);
        return WithSlot(updated, FieldSlot(typeDeclaration->kind, Field::CloseBraceToken), finalCloseBraceToken);
    };

    SyntaxListBuilder finalMembers = _pool.Allocate();

    // Do a single linear sweep, examining each type declaration we run into within the namespace.
    for (int currentBodyMemberIndex = 0; currentBodyMemberIndex < body.Members.Count();) {
        GreenNode *currentMember = body.Members[currentBodyMemberIndex];

        // If we have a suitable type declaration that ended without problem (has a real close curly and no
        // trailing semicolon),  then see if there are any type-only members following it that should be moved
        // into it.
        if (KindDerivesFrom(currentMember->kind, "TypeDeclarationSyntax") &&
            GetField(currentMember, Field::SemicolonToken) == nullptr &&
            GetField(currentMember, Field::CloseBraceToken) != nullptr &&
            !GetField(currentMember, Field::CloseBraceToken)->IsMissing() &&
            !GetField(currentMember, Field::CloseBraceToken)->ContainsDiagnostics()) {
            GreenNode *currentTypeDeclaration = currentMember;
            std::optional<SiblingRange> siblingsToMoveIntoType =
                determineSiblingsToMoveIntoType(currentBodyMemberIndex, body);
            if (siblingsToMoveIntoType) {
                int firstSiblingToMoveInclusive = siblingsToMoveIntoType->firstSiblingToMoveInclusive;
                int lastSiblingToMoveExclusive = siblingsToMoveIntoType->lastSiblingToMoveExclusive;
                // We found sibling type-only members.  Move them into the preceding type declaration.
                GreenNode *finalTypeDeclaration = moveSiblingMembersIntoPrecedingType(
                    currentTypeDeclaration, body, firstSiblingToMoveInclusive, lastSiblingToMoveExclusive);
                finalMembers.Add(finalTypeDeclaration);

                // We moved a sequence of type-only-members into the preceding type declaration.  We need to
                // continue processing from the end of that sequence.
                currentBodyMemberIndex = lastSiblingToMoveExclusive;
                continue;
            }
        }

        // Simple case.  A normal namespace member we don't need to do anything with.
        finalMembers.Add(currentMember);
        currentBodyMemberIndex++;
    }

    _pool.Free(body.Members);
    body.Members = finalMembers;
}

bool LanguageParser::IsMemberDeclarationOnlyValidWithinTypeDeclaration(GreenNode *memberDeclaration) {
    if (memberDeclaration == nullptr) return false;
    switch (memberDeclaration->kind) {
        case SyntaxKind::ConstructorDeclaration:
        case SyntaxKind::ConversionOperatorDeclaration:
        case SyntaxKind::DestructorDeclaration:
        case SyntaxKind::EventDeclaration:
        case SyntaxKind::EventFieldDeclaration:
        case SyntaxKind::FieldDeclaration:
        case SyntaxKind::IndexerDeclaration:
        case SyntaxKind::MethodDeclaration:
        case SyntaxKind::OperatorDeclaration:
        case SyntaxKind::PropertyDeclaration:
            return true;
        default:
            return false;
    }
}

void LanguageParser::ParseNamespaceBodyWorker(GreenNode *&openBraceOrSemicolon, NamespaceBodyBuilder &body,
                                              SyntaxListBuilder &initialBadNodes, SyntaxKind parentKind,
                                              bool &sawMemberDeclarationOnlyValidWithinTypeDeclaration) {
    // "top-level" expressions and statements should never occur inside an asynchronous context
    CS_ASSERT(!IsInAsync());

    bool isGlobal = openBraceOrSemicolon == nullptr;

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsNamespaceMemberStartOrStop;
    NamespaceParts seen = NamespaceParts::None;
    SyntaxListBuilder pendingIncompleteMembers = _pool.Allocate();
    bool reportUnexpectedToken = true;

    sawMemberDeclarationOnlyValidWithinTypeDeclaration = false;

    // (local functions)
    auto adjustStateAndReportStatementOutOfOrder = [this](NamespaceParts &seen_,
                                                          GreenNode *memberOrStatement) -> GreenNode * {
        switch (memberOrStatement->kind) {
            case SyntaxKind::GlobalStatement:
                if (seen_ < NamespaceParts::MembersAndStatements) {
                    seen_ = NamespaceParts::MembersAndStatements;
                } else if (seen_ == NamespaceParts::TypesAndNamespaces) {
                    seen_ = NamespaceParts::TopLevelStatementsAfterTypesAndNamespaces;

                    if (!IsScript()) {
                        memberOrStatement = AddError(memberOrStatement, ErrorCode::ERR_TopLevelStatementAfterNamespaceOrType);
                    }
                }

                break;

            case SyntaxKind::NamespaceDeclaration:
            case SyntaxKind::FileScopedNamespaceDeclaration:
            case SyntaxKind::EnumDeclaration:
            case SyntaxKind::StructDeclaration:
            case SyntaxKind::UnionDeclaration:
            case SyntaxKind::ClassDeclaration:
            case SyntaxKind::InterfaceDeclaration:
            case SyntaxKind::DelegateDeclaration:
            case SyntaxKind::RecordDeclaration:
            case SyntaxKind::RecordStructDeclaration:
                if (seen_ < NamespaceParts::TypesAndNamespaces) {
                    seen_ = NamespaceParts::TypesAndNamespaces;
                }
                break;

            default:
                if (seen_ < NamespaceParts::MembersAndStatements) {
                    seen_ = NamespaceParts::MembersAndStatements;
                }
                break;
        }

        return memberOrStatement;
    };

    auto parseUsingDirective = [this](GreenNode *&openBrace, NamespaceBodyBuilder &body_,
                                      SyntaxListBuilder &initialBadNodes_, NamespaceParts &seen_,
                                      SyntaxListBuilder &pendingIncompleteMembers_) {
        // incomplete members must be processed before we add any nodes to the body:
        ReduceIncompleteMembers(pendingIncompleteMembers_, openBrace, body_, initialBadNodes_);

        GreenNode *using_ = ParseUsingDirective();
        if (seen_ > NamespaceParts::Usings) {
            using_ = AddError(using_, ErrorCode::ERR_UsingAfterElements);
            AddSkippedNamespaceText(openBrace, body_, initialBadNodes_, using_);
        } else {
            body_.Usings.Add(using_);
            seen_ = NamespaceParts::Usings;
        }
    };

    Finally finally_{[&] {
        _termState = saveTerm;

        // adds pending incomplete nodes:
        AddIncompleteMembers(pendingIncompleteMembers, body);
        _pool.Free(pendingIncompleteMembers);
    }};

    while (true) {
        bool gotoDefault = false;  // `goto default`
        switch (CurrentToken()->kind) {
            case SyntaxKind::NamespaceKeyword: {
                // incomplete members must be processed before we add any nodes to the body:
                AddIncompleteMembers(pendingIncompleteMembers, body);

                SyntaxListBuilder attributeLists = _pool.Allocate();
                SyntaxListBuilder modifiers = _pool.Allocate();

                body.Members.Add(
                    adjustStateAndReportStatementOutOfOrder(seen, ParseNamespaceDeclaration(attributeLists, modifiers)));

                _pool.Free(attributeLists);
                _pool.Free(modifiers);

                reportUnexpectedToken = true;
                break;
            }

            case SyntaxKind::CloseBraceToken:
                // A very common user error is to type an additional }
                // somewhere in the file.  This will cause us to stop parsing
                // the root (global) namespace too early and will make the
                // rest of the file unparseable and unusable by intellisense.
                // We detect that case here and we skip the close curly and
                // continue parsing as if we did not see the }
                if (isGlobal) {
                    // incomplete members must be processed before we add any nodes to the body:
                    ReduceIncompleteMembers(pendingIncompleteMembers, openBraceOrSemicolon, body, initialBadNodes);

                    GreenNode *token = EatToken();
                    token = AddError(token, IsScript() ? ErrorCode::ERR_GlobalDefinitionOrStatementExpected
                                                       : ErrorCode::ERR_EOFExpected);

                    AddSkippedNamespaceText(openBraceOrSemicolon, body, initialBadNodes, token);
                    reportUnexpectedToken = true;
                    break;
                } else {
                    // This token marks the end of a namespace body
                    return;
                }

            case SyntaxKind::EndOfFileToken:
                // This token marks the end of a namespace body
                return;

            case SyntaxKind::ExternKeyword:
                if (isGlobal && !ScanExternAliasDirective()) {
                    // extern member or a local function
                    gotoDefault = true;
                    break;
                } else {
                    // incomplete members must be processed before we add any nodes to the body:
                    ReduceIncompleteMembers(pendingIncompleteMembers, openBraceOrSemicolon, body, initialBadNodes);

                    GreenNode *extern_ = ParseExternAliasDirective();
                    if (seen > NamespaceParts::ExternAliases) {
                        extern_ = AddErrorToFirstToken(extern_, ErrorCode::ERR_ExternAfterElements);
                        AddSkippedNamespaceText(openBraceOrSemicolon, body, initialBadNodes, extern_);
                    } else {
                        body.Externs.Add(extern_);
                        seen = NamespaceParts::ExternAliases;
                    }

                    reportUnexpectedToken = true;
                    break;
                }

            case SyntaxKind::UsingKeyword:
                if (isGlobal && (PeekToken(1)->kind == SyntaxKind::OpenParenToken ||
                                 (!IsScript() && IsPossibleTopLevelUsingLocalDeclarationStatement()))) {
                    // Top-level using statement or using local declaration
                    gotoDefault = true;
                    break;
                } else {
                    parseUsingDirective(openBraceOrSemicolon, body, initialBadNodes, seen, pendingIncompleteMembers);
                }

                reportUnexpectedToken = true;
                break;

            case SyntaxKind::IdentifierToken:
                if (CurrentToken()->contextual_kind != SyntaxKind::GlobalKeyword ||
                    PeekToken(1)->kind != SyntaxKind::UsingKeyword) {
                    gotoDefault = true;
                    break;
                } else {
                    parseUsingDirective(openBraceOrSemicolon, body, initialBadNodes, seen, pendingIncompleteMembers);
                }

                reportUnexpectedToken = true;
                break;

            case SyntaxKind::OpenBracketToken:
                if (IsPossibleGlobalAttributeDeclaration()) {
                    // Could be an attribute, or it could be a collection expression at the top level.  e.g.
                    // `[assembly: 1].XYZ();`. While this is definitely odd code, it is totally legal (as
                    // `assembly` is just an identifier).
                    GreenNode *attribute =
                        TryParseAttributeDeclaration(/*inExpressionContext:*/ parentKind == SyntaxKind::CompilationUnit);
                    if (attribute != nullptr) {
                        // incomplete members must be processed before we add any nodes to the body:
                        ReduceIncompleteMembers(pendingIncompleteMembers, openBraceOrSemicolon, body, initialBadNodes);

                        if (!isGlobal || seen > NamespaceParts::GlobalAttributes) {
                            // attribute.Update(open, attribute.Target.Update(AddError(Target.Identifier, ...),
                            // Target.ColonToken), attributes, close): the nodes' own diagnostics are kept.
                            GreenNode *target = GetField(attribute, Field::Target);
                            GreenNode *newTarget =
                                WithSlot(target, FieldSlot(target->kind, Field::Identifier),
                                         AddError(GetField(target, Field::Identifier),
                                                  ErrorCode::ERR_GlobalAttributesNotFirst));
                            attribute = WithSlot(attribute, FieldSlot(attribute->kind, Field::Target), newTarget);

                            AddSkippedNamespaceText(openBraceOrSemicolon, body, initialBadNodes, attribute);
                        } else {
                            body.Attributes.Add(attribute);
                            seen = NamespaceParts::GlobalAttributes;
                        }

                        reportUnexpectedToken = true;
                        break;
                    }
                }

                gotoDefault = true;
                break;

            default:
                gotoDefault = true;
                break;
        }

        if (!gotoDefault) continue;

        // default:
        GreenNode *memberOrStatement =
            isGlobal ? ParseMemberDeclarationOrStatement(parentKind) : ParseMemberDeclaration(parentKind);

        sawMemberDeclarationOnlyValidWithinTypeDeclaration |=
            IsMemberDeclarationOnlyValidWithinTypeDeclaration(memberOrStatement);
        if (memberOrStatement == nullptr) {
            // incomplete members must be processed before we add any nodes to the body:
            ReduceIncompleteMembers(pendingIncompleteMembers, openBraceOrSemicolon, body, initialBadNodes);

            // eat one token and try to parse declaration or statement again:
            GreenNode *skippedToken = EatToken();
            if (reportUnexpectedToken && !skippedToken->ContainsDiagnostics()) {
                skippedToken = AddError(skippedToken, IsScript() ? ErrorCode::ERR_GlobalDefinitionOrStatementExpected
                                                                 : ErrorCode::ERR_EOFExpected);

                // do not report the error multiple times for subsequent tokens:
                reportUnexpectedToken = false;
            }

            AddSkippedNamespaceText(openBraceOrSemicolon, body, initialBadNodes, skippedToken);
        } else if (memberOrStatement->kind == SyntaxKind::IncompleteMember &&
                   seen < NamespaceParts::MembersAndStatements) {
            pendingIncompleteMembers.Add(memberOrStatement);
            reportUnexpectedToken = true;
        } else {
            // incomplete members must be processed before we add any nodes to the body:
            AddIncompleteMembers(pendingIncompleteMembers, body);

            body.Members.Add(adjustStateAndReportStatementOutOfOrder(seen, memberOrStatement));
            reportUnexpectedToken = true;
        }
    }
}

void LanguageParser::AddIncompleteMembers(SyntaxListBuilder &incompleteMembers, NamespaceBodyBuilder &body) {
    if (incompleteMembers.Count() > 0) {
        body.Members.AddRange(incompleteMembers);
        incompleteMembers.Clear();
    }
}

void LanguageParser::ReduceIncompleteMembers(SyntaxListBuilder &incompleteMembers, GreenNode *&openBraceOrSemicolon,
                                             NamespaceBodyBuilder &body, SyntaxListBuilder &initialBadNodes) {
    for (int i = 0; i < incompleteMembers.Count(); i++)
        AddSkippedNamespaceText(openBraceOrSemicolon, body, initialBadNodes, incompleteMembers[i]);

    incompleteMembers.Clear();
}

bool LanguageParser::IsPossibleNamespaceMemberDeclaration() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::ExternKeyword:
        case SyntaxKind::UsingKeyword:
        case SyntaxKind::NamespaceKeyword:
            return true;
        case SyntaxKind::IdentifierToken:
            return IsPartialInNamespaceMemberDeclaration();
        default:
            return IsPossibleStartOfTypeDeclaration(CurrentToken()->kind);
    }
}

bool LanguageParser::IsPartialInNamespaceMemberDeclaration() {
    if (CurrentToken()->contextual_kind == SyntaxKind::PartialKeyword) {
        if (IsPartialType()) {
            return true;
        } else if (PeekToken(1)->kind == SyntaxKind::NamespaceKeyword) {
            return true;
        }
    }

    return false;
}

bool LanguageParser::IsEndOfNamespace() { return CurrentToken()->kind == SyntaxKind::CloseBraceToken; }

bool LanguageParser::IsGobalAttributesTerminator() {
    return IsEndOfNamespace() || IsPossibleNamespaceMemberDeclaration();
}

bool LanguageParser::IsNamespaceMemberStartOrStop() {
    return IsEndOfNamespace() || IsPossibleNamespaceMemberDeclaration();
}

/// Returns true if the lookahead tokens compose extern alias directive.
bool LanguageParser::ScanExternAliasDirective() {
    // The check also includes the ending semicolon so that we can disambiguate among:
    //   extern alias goo;
    //   extern alias goo();
    //   extern alias goo { get; }

    return CurrentToken()->kind == SyntaxKind::ExternKeyword &&
           (PeekToken(1)->kind == SyntaxKind::IdentifierToken &&
            PeekToken(1)->contextual_kind == SyntaxKind::AliasKeyword) &&
           PeekToken(2)->kind == SyntaxKind::IdentifierToken && PeekToken(3)->kind == SyntaxKind::SemicolonToken;
}

GreenNode *LanguageParser::ParseExternAliasDirective() {
    // (IsIncrementalAndFactoryContextMatches: never true -- node reuse not ported.)
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ExternKeyword);

    GreenNode *externKeyword = EatToken(SyntaxKind::ExternKeyword);
    GreenNode *aliasKeyword = EatContextualToken(SyntaxKind::AliasKeyword);
    GreenNode *identifier = ParseIdentifierToken();
    GreenNode *semicolon = EatToken(SyntaxKind::SemicolonToken);
    return SyntaxFactory::ExternAliasDirective(externKeyword, aliasKeyword, identifier, semicolon);
}

GreenNode *LanguageParser::ParseNameEquals() {
    CS_ASSERT(IsNamedAssignment());
    GreenNode *name = SyntaxFactory::IdentifierName(ParseIdentifierToken());
    GreenNode *equals = EatToken(SyntaxKind::EqualsToken);
    return SyntaxFactory::NameEquals(name, equals);
}

GreenNode *LanguageParser::ParseUsingDirective() {
    // (IsIncrementalAndFactoryContextMatches: never true -- node reuse not ported.)
    GreenNode *globalToken =
        CurrentToken()->contextual_kind == SyntaxKind::GlobalKeyword ? ConvertToKeyword(EatToken()) : nullptr;

    CS_ASSERT(CurrentToken()->kind == SyntaxKind::UsingKeyword);

    GreenNode *usingToken = EatToken(SyntaxKind::UsingKeyword);
    GreenNode *staticToken = TryEatToken(SyntaxKind::StaticKeyword);
    GreenNode *unsafeToken = TryEatToken(SyntaxKind::UnsafeKeyword);

    // if the user wrote `using unsafe static` skip the `static` and tell them it needs to be `using static unsafe`.
    if (staticToken == nullptr && unsafeToken != nullptr && CurrentToken()->kind == SyntaxKind::StaticKeyword) {
        // create a missing 'static' token so that later binding does recognize what the user wanted.
        staticToken = SyntaxFactory::MissingToken(SyntaxKind::StaticKeyword);
        unsafeToken = AddTrailingSkippedSyntax(unsafeToken, AddError(EatToken(), ErrorCode::ERR_BadStaticAfterUnsafe));
    }

    GreenNode *alias = IsNamedAssignment() ? ParseNameEquals() : nullptr;

    GreenNode *type;
    GreenNode *semicolon;

    bool isAliasToFunctionPointer = alias != nullptr && CurrentToken()->kind == SyntaxKind::DelegateKeyword;
    if (!isAliasToFunctionPointer && IsPossibleNamespaceMemberDeclaration()) {
        // We're worried about the case where someone already has a correct program
        // and they've gone back to add a using directive, but have not finished the
        // new directive.  e.g.
        //
        //    using
        //    namespace Goo {
        //        //...
        //    }
        //
        // If the token we see after "using" could be its own top-level construct, then
        // we just want to insert a missing identifier and semicolon and then return to
        // parsing at the top-level.
        //
        // NB: there's no way this could be true for a set of tokens that form a valid
        // using directive, so there's no danger in checking the error case first.

        type = SyntaxFactory::IdentifierName(CreateMissingToken(SyntaxKind::IdentifierToken, CurrentToken()->kind));
        semicolon = SyntaxFactory::MissingToken(SyntaxKind::SemicolonToken);
    } else {
        // In the case where we don't have an alias, only parse out a name for this using-directive.  This is
        // worse for error recovery, but it means all code that consumes a using-directive can keep on assuming
        // it has a name when there is no alias.  Only code that specifically has to process aliases then has to
        // deal with getting arbitrary types back.
        type = alias == nullptr ? ParseQualifiedName() : ParseType();

        // If we can see a semicolon ahead, then the current token was probably supposed to be an identifier
        if (type->IsMissing() && PeekToken(1)->kind == SyntaxKind::SemicolonToken)
            type = AddTrailingSkippedSyntax(type, EatToken());

        semicolon = EatToken(SyntaxKind::SemicolonToken);
    }

    return SyntaxFactory::UsingDirective(globalToken, usingToken, staticToken, unsafeToken, alias, type, semicolon);
}

bool LanguageParser::IsPossibleGlobalAttributeDeclaration() {
    return CurrentToken()->kind == SyntaxKind::OpenBracketToken && IsGlobalAttributeTarget(PeekToken(1)) &&
           PeekToken(2)->kind == SyntaxKind::ColonToken;
}

bool LanguageParser::IsGlobalAttributeTarget(GreenNode *token) {
    // token.ToAttributeLocation(): AttributeLocation.Assembly / Module. Roslyn compares the token's Text
    // (not its ValueText, "to match dev10").
    return token->text == u"assembly" || token->text == u"module";
}

bool LanguageParser::IsPossibleAttributeDeclaration() {
    // Have to at least start with `[` to be an attribute
    if (CurrentToken()->kind != SyntaxKind::OpenBracketToken) return false;

    DisposableResetPoint resetPoint_(this, /*resetOnDispose:*/ true);

    // Eat the `[`
    EatToken();

    // `[ id` could definitely begin an attribute.
    if (IsTrueIdentifier()) return true;

    // `[ word: ...` could definitely start an attribute.
    if (IsAttributeTarget()) return true;

    // If we see `[lit` (like `[0`) then this is def not an attribute, and should be parsed as a collection
    // expr.  Note: this heuristic can be added to in the future.
    if (SyntaxFacts::IsLiteralExpression(CurrentToken()->kind)) return false;

    return true;
}

SyntaxList LanguageParser::ParseAttributeDeclarations(bool inExpressionContext) {
    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsAttributeDeclarationTerminator;

    // An attribute can never appear *inside* an attribute argument (since a lambda expression cannot be used as
    // a constant argument value).  However, during parsing we can end up in a state where we're trying to
    // exactly that, through a path of Attribute->Argument->Expression->Attribute (since attributes can not be
    // on lambda expressions).
    //
    // Worse, when we are in a deeply ambiguous (or gibberish) scenario, where we see lots of code with `... [
    // ... [ ... ] ... ] ...`, we can get into exponential speculative parsing where we try `[ ... ]` both as an
    // attribute *and* a collection expression.
    //
    // Since we cannot ever legally have an attribute within an attribute, we bail out here immediately
    // syntactically.  This does mean we won't parse something like: `[X([Y]() => {})]` without errors, but that
    // is not semantically legal anyway.
    if (saveTerm == _termState) return SyntaxList();

    SyntaxListBuilder attributes = _pool.Allocate();
    while (IsPossibleAttributeDeclaration()) {
        GreenNode *attributeDeclaration = TryParseAttributeDeclaration(inExpressionContext);
        if (attributeDeclaration == nullptr) break;

        attributes.Add(attributeDeclaration);
    }

    _termState = saveTerm;

    return _pool.ToListAndFree(attributes);
}

bool LanguageParser::IsAttributeDeclarationTerminator() {
    return CurrentToken()->kind == SyntaxKind::CloseBracketToken || IsPossibleAttributeDeclaration();  // start of a new one...
}

bool LanguageParser::IsAttributeTarget() {
    return IsSomeWord(CurrentToken()->kind) && PeekToken(1)->kind == SyntaxKind::ColonToken;
}

GreenNode *LanguageParser::TryParseAttributeDeclaration(bool inExpressionContext) {
    // (IsIncrementalAndFactoryContextMatches: never true -- node reuse not ported.)

    // May have to reset if we discover this is not an attribute but is instead a collection expression.
    DisposableResetPoint resetPoint(this, /*resetOnDispose:*/ false);

    GreenNode *openBracket = EatToken(SyntaxKind::OpenBracketToken);

    // Check for optional location :
    GreenNode *location = nullptr;
    if (IsAttributeTarget()) {
        GreenNode *identifier = ConvertToKeyword(EatToken());
        GreenNode *colon = EatToken(SyntaxKind::ColonToken);
        location = SyntaxFactory::AttributeTargetSpecifier(identifier, colon);
    }

    // (local functions)
    SkipBadTokensFn skipBadAttributeListTokens = [](LanguageParser *this_, GreenNode *&openBracket_,
                                                    SyntaxListBuilder list, SyntaxKind expectedKind,
                                                    SyntaxKind closeKind) -> PostSkipAction {
        return this_->SkipBadSeparatedListTokensWithExpectedKind(
            openBracket_, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleAttribute();
            },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    SeparatedSyntaxList attributes = ParseCommaSeparatedSyntaxList(
        openBracket, SyntaxKind::CloseBracketToken,
        IsPossibleElementFn([](LanguageParser *p) { return p->IsPossibleAttribute(); }),
        ParseElementFn([](LanguageParser *p) { return p->ParseAttribute(); }), skipBadAttributeListTokens,
        /*allowTrailingSeparator:*/ true,
        /*requireOneElement:*/ true,
        /*allowSemicolonAsSeparator:*/ false);

    auto shouldParseAsCollectionExpression = [this]() -> bool {
        // `[A, B].` is a member access off of a collection expression.
        if (CurrentToken()->kind == SyntaxKind::DotToken) return true;

        // `[A, B]->` is a member access off of a collection expression. Note: this will always be illegal
        // semantically (as a collection expression has the natural type List<> which is not a pointer type).  But
        // we leave that check to binding.
        if (CurrentToken()->kind == SyntaxKind::MinusGreaterThanToken) return true;

        // `[A, B]?.`  The `?` is unnecessary (as a collection expression is always non-null), but is still
        // syntactically legal.
        if (CurrentToken()->kind == SyntaxKind::QuestionToken && PeekToken(1)->kind == SyntaxKind::DotToken) {
            return true;
        }

        return false;
    };

    GreenNode *closeBracket = EatToken(SyntaxKind::CloseBracketToken);
    if (inExpressionContext && shouldParseAsCollectionExpression()) {
        // we're in an expression and we've seen `[A, B].`  This is actually the start of a collection expression
        // that someone is explicitly accessing a member off of.
        resetPoint.Reset();
        return nullptr;
    }

    return SyntaxFactory::AttributeList(openBracket, location, attributes, closeBracket);
}

bool LanguageParser::IsPossibleAttribute() { return IsTrueIdentifier(); }

GreenNode *LanguageParser::ParseAttribute() {
    // (IsIncrementalAndFactoryContextMatches: never true -- node reuse not ported.)
    GreenNode *name = ParseQualifiedName();
    GreenNode *argumentList = ParseAttributeArgumentList();
    return SyntaxFactory::Attribute(name, argumentList);
}

GreenNode *LanguageParser::ParseAttributeArgumentList() {
    // (IsIncrementalAndFactoryContextMatches: never true -- node reuse not ported.)
    if (CurrentToken()->kind != SyntaxKind::OpenParenToken) return nullptr;

    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);

    // (local functions)
    SkipBadTokensFn skipBadAttributeArgumentTokens = [](LanguageParser *this_, GreenNode *&openParen_,
                                                        SyntaxListBuilder list, SyntaxKind expectedKind,
                                                        SyntaxKind closeKind) -> PostSkipAction {
        return this_->SkipBadSeparatedListTokensWithExpectedKind(
            openParen_, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleAttributeArgument();
            },
            [](LanguageParser *p, SyntaxKind closeKind_) { return p->CurrentToken()->kind == closeKind_; },
            expectedKind, closeKind);
    };

    ImmediatelyAbortFn immediatelyAbort = [](GreenNode *argument) -> bool {
        // We can be very thrown off by incomplete strings in an attribute argument (especially as the lexer
        // will restart on the next line with the contents of the string then being interpreted as more
        // arguments).  Bail out in this case to prevent going off the rails.
        GreenNode *expression = GetField(argument, Field::Expression);
        if (expression != nullptr && expression->kind == SyntaxKind::StringLiteralExpression) {
            GreenNode *literalToken = GetField(expression, Field::Token);
            for (const SyntaxDiagnosticInfo *d = DiagsBegin(literalToken); d != DiagsEnd(literalToken); ++d) {
                if (d->code == ErrorCode::ERR_NewlineInConst) return true;
            }
        }

        if (expression != nullptr && expression->kind == SyntaxKind::InterpolatedStringExpression &&
            GetField(expression, Field::StringStartToken)->kind == SyntaxKind::InterpolatedStringStartToken &&
            GetField(expression, Field::StringEndToken)->IsMissing())
            return true;

        return false;
    };

    SeparatedSyntaxList argNodes = ParseCommaSeparatedSyntaxList(
        openParen, SyntaxKind::CloseParenToken,
        IsPossibleElementFn([](LanguageParser *p) { return p->IsPossibleAttributeArgument(); }),
        ParseElementFn([](LanguageParser *p) { return p->ParseAttributeArgument(); }), immediatelyAbort,
        skipBadAttributeArgumentTokens,
        /*allowTrailingSeparator:*/ false,
        /*requireOneElement:*/ false,
        /*allowSemicolonAsSeparator:*/ false);

    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
    return SyntaxFactory::AttributeArgumentList(openParen, argNodes, closeParen);
}

bool LanguageParser::IsPossibleAttributeArgument() { return IsPossibleExpression(); }

GreenNode *LanguageParser::ParseAttributeArgument() {
    // Need to parse both "real" named arguments and attribute-style named arguments.
    // We track attribute-style named arguments only with fShouldHaveName.

    GreenNode *nameEquals = nullptr;
    GreenNode *nameColon = nullptr;
    if (CurrentToken()->kind == SyntaxKind::IdentifierToken) {
        switch (PeekToken(1)->kind) {
            case SyntaxKind::EqualsToken: {
                GreenNode *name = SyntaxFactory::IdentifierName(ParseIdentifierToken());
                GreenNode *equals = EatToken(SyntaxKind::EqualsToken);
                nameEquals = SyntaxFactory::NameEquals(name, equals);

                break;
            }
            case SyntaxKind::ColonToken: {
                GreenNode *name = ParseIdentifierName();
                GreenNode *colon = EatToken(SyntaxKind::ColonToken);
                nameColon = SyntaxFactory::NameColon(name, colon);

                break;
            }
            default:
                break;
        }
    }

    GreenNode *expression = ParseExpressionCore();
    return SyntaxFactory::AttributeArgument(nameEquals, nameColon, expression);
}

DeclarationModifiers LanguageParser::GetModifierExcludingScoped(GreenNode *token) {
    return GetModifierExcludingScoped(token->kind, token->contextual_kind);
}

DeclarationModifiers LanguageParser::GetModifierExcludingScoped(SyntaxKind kind, SyntaxKind contextualKind) {
    switch (kind) {
        case SyntaxKind::PublicKeyword:
            return DeclarationModifiers::Public;
        case SyntaxKind::InternalKeyword:
            return DeclarationModifiers::Internal;
        case SyntaxKind::ProtectedKeyword:
            return DeclarationModifiers::Protected;
        case SyntaxKind::PrivateKeyword:
            return DeclarationModifiers::Private;
        case SyntaxKind::SealedKeyword:
            return DeclarationModifiers::Sealed;
        case SyntaxKind::AbstractKeyword:
            return DeclarationModifiers::Abstract;
        case SyntaxKind::StaticKeyword:
            return DeclarationModifiers::Static;
        case SyntaxKind::VirtualKeyword:
            return DeclarationModifiers::Virtual;
        case SyntaxKind::ExternKeyword:
            return DeclarationModifiers::Extern;
        case SyntaxKind::NewKeyword:
            return DeclarationModifiers::New;
        case SyntaxKind::OverrideKeyword:
            return DeclarationModifiers::Override;
        case SyntaxKind::ReadOnlyKeyword:
            return DeclarationModifiers::ReadOnly;
        case SyntaxKind::VolatileKeyword:
            return DeclarationModifiers::Volatile;
        case SyntaxKind::UnsafeKeyword:
            return DeclarationModifiers::Unsafe;
        case SyntaxKind::PartialKeyword:
            return DeclarationModifiers::Partial;
        case SyntaxKind::AsyncKeyword:
            return DeclarationModifiers::Async;
        case SyntaxKind::RefKeyword:
            return DeclarationModifiers::Ref;
        case SyntaxKind::IdentifierToken:
            switch (contextualKind) {
                case SyntaxKind::PartialKeyword:
                    return DeclarationModifiers::Partial;
                case SyntaxKind::AsyncKeyword:
                    return DeclarationModifiers::Async;
                case SyntaxKind::RequiredKeyword:
                    return DeclarationModifiers::Required;
                case SyntaxKind::FileKeyword:
                    return DeclarationModifiers::File;
                default:
                    break;
            }

            return DeclarationModifiers::None;  // goto default
        default:
            return DeclarationModifiers::None;
    }
}

void LanguageParser::ParseModifiers(SyntaxListBuilder tokens, bool forAccessors, bool forTopLevelStatements,
                                    bool &isPossibleTypeDeclaration) {
    CS_ASSERT(!(forAccessors && forTopLevelStatements));

    // (local function)
    auto isStructOrRecordOrUnionKeyword = [](GreenNode *token) -> bool {
        if (token->kind == SyntaxKind::StructKeyword) {
            return true;
        }

        switch (token->contextual_kind) {
            case SyntaxKind::RecordKeyword:
                // This is an unusual use of LangVersion. Normally we only produce errors when the langversion
                // does not support a feature, but in this case we are effectively making a language breaking
                // change to consider "record" a type declaration in all ambiguous cases. To avoid breaking
                // older code that is not using C# 9 we conditionally parse based on langversion
                return IsFeatureEnabled(/*MessageID.IDS_FeatureRecords*/);

            case SyntaxKind::UnionKeyword:
                // (as above, C# 15)
                return IsFeatureEnabled(/*MessageID.IDS_FeatureUnions*/);

            default:
                break;
        }

        return false;
    };

    isPossibleTypeDeclaration = true;

    while (true) {
        DeclarationModifiers newMod = GetModifierExcludingScoped(CurrentToken());

        CS_ASSERT(newMod != DeclarationModifiers::Scoped);
        if (newMod == DeclarationModifiers::None) {
            if (!forAccessors) {
                GreenNode *scopedKeyword =
                    ParsePossibleScopedKeyword(/*isFunctionPointerParameter:*/ false, /*isLambdaParameter:*/ false);

                if (scopedKeyword != nullptr) {
                    isPossibleTypeDeclaration = false;
                    tokens.Add(scopedKeyword);
                }
            }

            break;
        }

        GreenNode *modTok;
        switch (newMod) {
            case DeclarationModifiers::Partial: {
                GreenNode *nextToken = PeekToken(1);
                if (IsPartialType() || IsPartialMember()) {
                    // Standard legal cases.
                    modTok = ConvertToKeyword(EatToken());
                } else if (nextToken->kind == SyntaxKind::NamespaceKeyword) {
                    // Error reported in binding
                    modTok = ConvertToKeyword(EatToken());
                } else if (nextToken->kind == SyntaxKind::EnumKeyword || nextToken->kind == SyntaxKind::DelegateKeyword ||
                           (IsPossibleStartOfTypeDeclaration(nextToken->kind) &&
                            GetModifierExcludingScoped(nextToken) != DeclarationModifiers::None)) {
                    // Error reported in ModifierUtils.
                    modTok = ConvertToKeyword(EatToken());
                } else {
                    return;
                }

                break;
            }

            case DeclarationModifiers::Ref: {
                // 'ref' is only a modifier if used on a ref struct
                // it must be either immediately before the 'struct'
                // keyword, or immediately before 'partial struct' if
                // this is a partial ref struct declaration
                GreenNode *next = PeekToken(1);
                if (isStructOrRecordOrUnionKeyword(next) ||
                    (next->contextual_kind == SyntaxKind::PartialKeyword && isStructOrRecordOrUnionKeyword(PeekToken(2)))) {
                    modTok = EatToken();
                } else if (forAccessors && IsPossibleAccessorModifier()) {
                    // Accept ref as a modifier for properties and event accessors, to produce an error later during binding.
                    modTok = EatToken();
                } else {
                    return;
                }
                break;
            }

            case DeclarationModifiers::File:
                if ((!IsFeatureEnabled(/*MessageID.IDS_FeatureFileTypes*/) || forTopLevelStatements) &&
                    !ShouldContextualKeywordBeTreatedAsModifier(/*parsingStatementNotDeclaration:*/ false)) {
                    return;
                }

                // LangVersion errors for 'file' modifier are given during binding.
                modTok = ConvertToKeyword(EatToken());
                break;

            case DeclarationModifiers::Async:
                if (!ShouldContextualKeywordBeTreatedAsModifier(/*parsingStatementNotDeclaration:*/ false)) {
                    return;
                }

                modTok = ConvertToKeyword(EatToken());
                break;

            case DeclarationModifiers::Required:
                // In C# 11, required in a modifier position is always a keyword if not escaped. Otherwise, we reuse the async detection
                // machinery to make a conservative guess as to whether the user meant required to be a keyword, so that they get a good langver
                // diagnostic and all the machinery to upgrade their project kicks in. The only exception to this rule is top level statements,
                // where the user could conceivably have a local named required. For these locations, we need to disambiguate as well.
                if ((!IsFeatureEnabled(/*MessageID.IDS_FeatureRequiredMembers*/) || forTopLevelStatements) &&
                    !ShouldContextualKeywordBeTreatedAsModifier(/*parsingStatementNotDeclaration:*/ false)) {
                    return;
                }

                modTok = ConvertToKeyword(EatToken());

                break;

            default:
                modTok = EatToken();
                break;
        }

        CS_ASSERT(!(modTok->kind == SyntaxKind::OutKeyword || modTok->kind == SyntaxKind::InKeyword));
        tokens.Add(modTok);
    }
}

bool LanguageParser::ShouldContextualKeywordBeTreatedAsModifier(bool parsingStatementNotDeclaration) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::IdentifierToken &&
              GetModifierExcludingScoped(CurrentToken()) != DeclarationModifiers::None);

    // Adapted from CParser::IsAsyncMethod.

    if (IsNonContextualModifier(PeekToken(1))) {
        // If the next token is a (non-contextual) modifier keyword, then this token is
        // definitely a modifier
        return true;
    }

    // Some of our helpers start at the current token, so we'll have to advance for their
    // sake and then backtrack when we're done.  Don't leave this block without releasing
    // the reset point.
    DisposableResetPoint resetPoint_(this, /*resetOnDispose:*/ true);

    EatToken();  // move past contextual token

    if (!parsingStatementNotDeclaration && (CurrentToken()->contextual_kind == SyntaxKind::PartialKeyword)) {
        EatToken();  // "partial" doesn't affect our decision, so look past it.
    }

    // ... 'TOKEN' [partial] <typedecl> ...
    // ... 'TOKEN' [partial] <event> ...
    // ... 'TOKEN' [partial] <implicit> <operator> ...
    // ... 'TOKEN' [partial] <explicit> <operator> ...
    // ... 'TOKEN' [partial] <typename> <operator> ...
    // ... 'TOKEN' [partial] <typename> <membername> ...
    // DEVNOTE: Although we parse async user defined conversions, operators, etc. here,
    // anything other than async methods are detected as erroneous later, during the define phase
    // Generally wherever we refer to 'async' here, it can also be 'required' or 'file'.

    if (!parsingStatementNotDeclaration) {
        SyntaxKind currentTokenKind = CurrentToken()->kind;
        if (IsTypeModifierOrTypeKeyword(currentTokenKind) || currentTokenKind == SyntaxKind::EventKeyword ||
            ((currentTokenKind == SyntaxKind::ExplicitKeyword || currentTokenKind == SyntaxKind::ImplicitKeyword) &&
             PeekToken(1)->kind == SyntaxKind::OperatorKeyword)) {
            return true;
        }
    }

    if (ScanType() != ScanTypeFlags::NotType) {
        // We've seen "TOKEN TypeName".  Now we have to determine if we should we treat
        // 'TOKEN' as a modifier.  Or is the user actually writing something like
        // "public TOKEN Goo" where 'TOKEN' is actually the return type.

        if (IsPossibleMemberName()) {
            // we have: "TOKEN Type X" or "TOKEN Type this", 'TOKEN' is definitely a
            // modifier here.
            return true;
        }

        SyntaxKind currentTokenKind = CurrentToken()->kind;

        // The file ends with "TOKEN TypeName", it's not legal code, and it's much
        // more likely that this is meant to be a modifier.
        if (currentTokenKind == SyntaxKind::EndOfFileToken) {
            return true;
        }

        // "TOKEN TypeName }".  In this case, we just have an incomplete member, and
        // we should definitely default to 'TOKEN' being considered a return type here.
        if (currentTokenKind == SyntaxKind::CloseBraceToken) {
            return true;
        }

        // "TOKEN TypeName void". In this case, we just have an incomplete member before
        // an existing member.  Treat this 'TOKEN' as a keyword.
        if (SyntaxFacts::IsPredefinedType(CurrentToken()->kind)) {
            return true;
        }

        // "TOKEN TypeName public".  In this case, we just have an incomplete member before
        // an existing member.  Treat this 'TOKEN' as a keyword.
        if (IsNonContextualModifier(CurrentToken())) {
            return true;
        }

        // "TOKEN TypeName class". In this case, we just have an incomplete member before
        // an existing type declaration.  Treat this 'TOKEN' as a keyword.
        if (IsTypeDeclarationStart()) {
            return true;
        }

        // "TOKEN TypeName namespace". In this case, we just have an incomplete member before
        // an existing namespace declaration.  Treat this 'TOKEN' as a keyword.
        if (currentTokenKind == SyntaxKind::NamespaceKeyword) {
            return true;
        }

        if (!parsingStatementNotDeclaration && currentTokenKind == SyntaxKind::OperatorKeyword) {
            return true;
        }
    }

    return false;
}

bool LanguageParser::IsNonContextualModifier(GreenNode *nextToken) {
    return !SyntaxFacts::IsContextualKeyword(nextToken->contextual_kind) &&
           GetModifierExcludingScoped(nextToken) != DeclarationModifiers::None;
}

bool LanguageParser::IsPartialType() {
    CS_ASSERT(CurrentToken()->contextual_kind == SyntaxKind::PartialKeyword);
    GreenNode *nextToken = PeekToken(1);
    switch (nextToken->kind) {
        case SyntaxKind::StructKeyword:
        case SyntaxKind::ClassKeyword:
        case SyntaxKind::InterfaceKeyword:
            return true;
        default:
            break;
    }

    switch (nextToken->contextual_kind) {
        case SyntaxKind::RecordKeyword:
            // This is an unusual use of LangVersion. Normally we only produce errors when the langversion
            // does not support a feature, but in this case we are effectively making a language breaking
            // change to consider "record" a type declaration in all ambiguous cases. To avoid breaking
            // older code that is not using C# 9 we conditionally parse based on langversion
            return IsFeatureEnabled(/*MessageID.IDS_FeatureRecords*/);

        case SyntaxKind::UnionKeyword:
            // (as above, C# 15)
            return IsFeatureEnabled(/*MessageID.IDS_FeatureUnions*/);

        default:
            break;
    }

    return false;
}

bool LanguageParser::IsPartialMember() {
    CS_ASSERT(CurrentToken()->contextual_kind == SyntaxKind::PartialKeyword);

    // Check for:
    //   partial event
    if (PeekToken(1)->kind == SyntaxKind::EventKeyword) {
        return true;
    }

    // Check for constructor:
    //   partial Identifier(
    if (PeekToken(1)->kind == SyntaxKind::IdentifierToken && PeekToken(2)->kind == SyntaxKind::OpenParenToken) {
        return IsFeatureEnabled(/*MessageID.IDS_FeaturePartialEventsAndConstructors*/);
    }

    // Check for method/property:
    //   partial ReturnType MemberName
    DisposableResetPoint resetPoint_(this, /*resetOnDispose:*/ true);

    EatToken();  // partial

    if (ScanType() == ScanTypeFlags::NotType) {
        return false;
    }

    return IsPossibleMemberName();
}

bool LanguageParser::IsPossibleMemberName() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::IdentifierToken:
            if (CurrentToken()->contextual_kind == SyntaxKind::GlobalKeyword &&
                PeekToken(1)->kind == SyntaxKind::UsingKeyword) {
                return false;
            }

            return true;
        case SyntaxKind::ThisKeyword:
            return true;
        default:
            return false;
    }
}

GreenNode *LanguageParser::ParseTypeDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    // "top-level" expressions and statements should never occur inside an asynchronous context
    CS_ASSERT(!IsInAsync());

    // (cancellationToken.ThrowIfCancellationRequested(): no cancellation here.)

    switch (CurrentToken()->kind) {
        case SyntaxKind::ClassKeyword:
            return ParseMainTypeDeclaration(attributes, modifiers);

        case SyntaxKind::StructKeyword:
            return ParseMainTypeDeclaration(attributes, modifiers);

        case SyntaxKind::InterfaceKeyword:
            return ParseMainTypeDeclaration(attributes, modifiers);

        case SyntaxKind::DelegateKeyword:
            return ParseDelegateDeclaration(attributes, modifiers);

        case SyntaxKind::EnumKeyword:
            return ParseEnumDeclaration(attributes, modifiers);

        case SyntaxKind::IdentifierToken:
            CS_ASSERT(CurrentToken()->contextual_kind == SyntaxKind::RecordKeyword ||
                      CurrentToken()->contextual_kind == SyntaxKind::ExtensionKeyword ||
                      CurrentToken()->contextual_kind == SyntaxKind::UnionKeyword);
            return ParseMainTypeDeclaration(attributes, modifiers);

        default:
            // throw ExceptionUtilities.UnexpectedValue(this.CurrentToken.Kind): unreachable.
            CS_ASSERT(false);
            return nullptr;
    }
}

GreenNode *LanguageParser::ParseMainTypeDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ClassKeyword || CurrentToken()->kind == SyntaxKind::StructKeyword ||
              CurrentToken()->kind == SyntaxKind::InterfaceKeyword ||
              CurrentToken()->contextual_kind == SyntaxKind::RecordKeyword ||
              CurrentToken()->contextual_kind == SyntaxKind::ExtensionKeyword ||
              CurrentToken()->contextual_kind == SyntaxKind::UnionKeyword);

    // "top-level" expressions and statements should never occur inside an asynchronous context
    CS_ASSERT(!IsInAsync());

    // (local functions)
    auto tryScanRecordStart = [this](GreenNode *&keyword_, GreenNode *&recordModifier_) -> bool {
        if (CurrentToken()->contextual_kind == SyntaxKind::RecordKeyword) {
            keyword_ = ConvertToKeyword(EatToken());
            recordModifier_ = CurrentToken()->kind == SyntaxKind::ClassKeyword ||
                                      CurrentToken()->kind == SyntaxKind::StructKeyword
                                  ? EatToken()
                                  : nullptr;

            return true;
        }

        if ((CurrentToken()->kind == SyntaxKind::StructKeyword || CurrentToken()->kind == SyntaxKind::ClassKeyword) &&
            PeekToken(1)->contextual_kind == SyntaxKind::RecordKeyword &&
            PeekToken(2)->kind == SyntaxKind::IdentifierToken) {
            // Provide a specific diagnostic on `struct record S` or `class record C`
            GreenNode *misplacedToken = EatToken();

            // Parse out 'record' but place 'struct/class' as leading skipped trivia on it.
            keyword_ = AddLeadingSkippedSyntax(AddError(ConvertToKeyword(EatToken()), ErrorCode::ERR_MisplacedRecord),
                                               misplacedToken);

            // Treat `struct record` as a RecordStructDeclaration, and `class record` as a RecordDeclaration.
            recordModifier_ = SyntaxFactory::MissingToken(misplacedToken->kind);
            return true;
        }

        keyword_ = nullptr;
        recordModifier_ = nullptr;
        return false;
    };

    auto constructTypeDeclaration = [](SyntaxList attributes_, SyntaxListBuilder modifiers_, GreenNode *keyword_,
                                       GreenNode *recordModifier_, GreenNode *name_, GreenNode *typeParameters_,
                                       GreenNode *paramList_, GreenNode *baseList_, SyntaxListBuilder constraints_,
                                       GreenNode *openBrace_, SyntaxListBuilder members_, GreenNode *closeBrace_,
                                       GreenNode *semicolon_) -> GreenNode * {
        GreenNode *modifiersList = modifiers_.ToList();
        GreenNode *membersList = members_.ToList();
        GreenNode *constraintsList = constraints_.ToList();
        switch (keyword_->kind) {
            case SyntaxKind::ClassKeyword:
                CS_ASSERT(name_ != nullptr);
                return SyntaxFactory::ClassDeclaration(attributes_, modifiersList, keyword_, name_, typeParameters_,
                                                       paramList_, baseList_, constraintsList, openBrace_, membersList,
                                                       closeBrace_, semicolon_);

            case SyntaxKind::StructKeyword:
            case SyntaxKind::UnionKeyword:
                CS_ASSERT(name_ != nullptr);
                return SyntaxFactory::StructDeclaration(
                    keyword_->kind == SyntaxKind::UnionKeyword ? SyntaxKind::UnionDeclaration
                                                               : SyntaxKind::StructDeclaration,
                    attributes_, modifiersList, keyword_, name_, typeParameters_, paramList_, baseList_,
                    constraintsList, openBrace_, membersList, closeBrace_, semicolon_);

            case SyntaxKind::InterfaceKeyword:
                CS_ASSERT(name_ != nullptr);
                return SyntaxFactory::InterfaceDeclaration(attributes_, modifiersList, keyword_, name_,
                                                           typeParameters_, paramList_, baseList_, constraintsList,
                                                           openBrace_, membersList, closeBrace_, semicolon_);

            case SyntaxKind::RecordKeyword: {
                // record struct ...
                // record ...
                // record class ...
                CS_ASSERT(name_ != nullptr);
                SyntaxKind declarationKind =
                    recordModifier_ != nullptr && recordModifier_->kind == SyntaxKind::StructKeyword
                        ? SyntaxKind::RecordStructDeclaration
                        : SyntaxKind::RecordDeclaration;
                return SyntaxFactory::RecordDeclaration(declarationKind, attributes_, modifiers_.ToList(), keyword_,
                                                        /*classOrStructKeyword:*/ recordModifier_, name_,
                                                        typeParameters_, paramList_, baseList_, constraints_,
                                                        openBrace_, members_, closeBrace_, semicolon_);
            }

            case SyntaxKind::ExtensionKeyword:
                CS_ASSERT(name_ == nullptr);
                CS_ASSERT(baseList_ == nullptr);
                return SyntaxFactory::ExtensionBlockDeclaration(attributes_, modifiers_.ToList(), keyword_,
                                                                typeParameters_, paramList_, constraints_, openBrace_,
                                                                members_, closeBrace_, semicolon_);

            default:
                // throw ExceptionUtilities.UnexpectedValue(keyword.Kind): unreachable.
                CS_ASSERT(false);
                return nullptr;
        }
    };

    GreenNode *keyword = nullptr;
    GreenNode *recordModifier = nullptr;
    if (!tryScanRecordStart(keyword, recordModifier)) {
        keyword = ConvertToKeyword(EatToken());
    }

    bool isExtension = keyword->kind == SyntaxKind::ExtensionKeyword;
    bool isUnion = keyword->kind == SyntaxKind::UnionKeyword;
    TerminatorState outerSaveTerm = _termState;
    _termState |= TerminatorState::IsEndOfTypeSignature;

    TerminatorState saveTerm = _termState;
    _termState |= TerminatorState::IsPossibleAggregateClauseStartOrStop;

    GreenNode *name;
    if (isExtension) {
        name = nullptr;
        if (CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            keyword = AddTrailingSkippedSyntax(keyword, AddError(EatToken(), ErrorCode::ERR_ExtensionDisallowsName));
        }
    } else {
        name = ParseIdentifierToken();
    }

    GreenNode *typeParameters = ParseTypeParameterList();

    // For extension declarations, there must be a parameter list
    GreenNode *paramList = CurrentToken()->kind == SyntaxKind::OpenParenToken || isExtension
                               ? ParseParenthesizedParameterList(/*forExtensionOrUnion:*/ isExtension || isUnion)
                               : nullptr;

    GreenNode *baseList = isExtension ? nullptr : ParseBaseList();
    _termState = saveTerm;

    // Parse class body
    bool parseMembers = true;
    SyntaxListBuilder members;      // default (null)
    SyntaxListBuilder constraints;  // default (null)
    Finally finally_{[&] {
        if (!members.IsNull()) {
            _pool.Free(members);
        }

        if (!constraints.IsNull()) {
            _pool.Free(constraints);
        }
    }};

    if (CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
        constraints = _pool.Allocate();
        ParseTypeParameterConstraintClauses(constraints);
    }

    _termState = outerSaveTerm;

    GreenNode *semicolon;
    GreenNode *openBrace;
    GreenNode *closeBrace;
    if (CurrentToken()->kind == SyntaxKind::SemicolonToken) {
        semicolon = EatToken(SyntaxKind::SemicolonToken);
        openBrace = nullptr;
        closeBrace = nullptr;
    } else {
        openBrace = EatToken(SyntaxKind::OpenBraceToken);

        // ignore members if missing open curly
        if (openBrace->IsMissing()) {
            parseMembers = false;
        }

        // even if we saw a { or think we should parse members bail out early since
        // we know namespaces can't be nested inside types
        if (parseMembers) {
            members = _pool.Allocate();

            while (true) {
                SyntaxKind kind = CurrentToken()->kind;

                if (CanStartMember(kind)) {
                    // This token can start a member -- go parse it
                    TerminatorState saveTerm2 = _termState;
                    _termState |= TerminatorState::IsPossibleMemberStartOrStop;

                    GreenNode *member = ParseMemberDeclaration(keyword->kind);
                    if (member != nullptr) {
                        // statements are accepted here, a semantic error will be reported later
                        members.Add(member);
                    } else {
                        // we get here if we couldn't parse the lookahead as a statement or a declaration (we haven't consumed any tokens):
                        SkipBadMemberListTokens(openBrace, members);
                    }

                    _termState = saveTerm2;
                } else if (kind == SyntaxKind::CloseBraceToken || kind == SyntaxKind::EndOfFileToken || IsTerminator()) {
                    // This marks the end of members of this class
                    break;
                } else {
                    // Error -- try to sync up with intended reality
                    SkipBadMemberListTokens(openBrace, members);
                }
            }
        }

        closeBrace = openBrace->IsMissing() ? CreateMissingToken(SyntaxKind::CloseBraceToken, CurrentToken()->kind)
                                            : EatToken(SyntaxKind::CloseBraceToken);

        semicolon = TryEatToken(SyntaxKind::SemicolonToken);
    }

    return constructTypeDeclaration(attributes, modifiers, keyword, recordModifier, name, typeParameters, paramList,
                                    baseList, constraints, openBrace, members, closeBrace, semicolon);
}

void LanguageParser::SkipBadMemberListTokens(GreenNode *&openBrace, SyntaxListBuilder members) {
    if (members.Count() > 0) {
        GreenNode *tmp = members[members.Count() - 1];
        SkipBadMemberListTokens(tmp);
        members[members.Count() - 1] = tmp;
    } else {
        GreenNode *tmp = openBrace;
        SkipBadMemberListTokens(tmp);
        openBrace = tmp;
    }
}

void LanguageParser::SkipBadMemberListTokens(GreenNode *&previousNode) {
    int curlyCount = 0;
    SyntaxListBuilder tokens = _pool.Allocate();

    bool done = false;

    // always consume at least one token.
    GreenNode *token = EatToken();
    token = AddError(token, ErrorCode::ERR_InvalidMemberDecl, {token->text});
    tokens.Add(token);

    while (!done) {
        SyntaxKind kind = CurrentToken()->kind;

        // If this token can start a member, we're done
        if (CanStartMember(kind) &&
            !(kind == SyntaxKind::DelegateKeyword && (PeekToken(1)->kind == SyntaxKind::OpenBraceToken ||
                                                     PeekToken(1)->kind == SyntaxKind::OpenParenToken))) {
            done = true;
            continue;
        }

        // <UNDONE>  UNDONE: Seems like this makes sense,
        // but if this token can start a namespace element, but not a member, then
        // perhaps we should bail back up to parsing a namespace body somehow...</UNDONE>

        // Watch curlies and look for end of file/close curly
        switch (kind) {
            case SyntaxKind::OpenBraceToken:
                curlyCount++;
                break;

            case SyntaxKind::CloseBraceToken:
                if (curlyCount-- == 0) {
                    done = true;
                    continue;
                }

                break;

            case SyntaxKind::EndOfFileToken:
                done = true;
                continue;

            default:
                break;
        }

        tokens.Add(EatToken());
    }

    previousNode = AddTrailingSkippedSyntax(previousNode, _pool.ToListAndFree(tokens));  // ToTokenListAndFree(tokens).Node
}

bool LanguageParser::IsPossibleMemberStartOrStop() {
    return IsPossibleMemberStart() || CurrentToken()->kind == SyntaxKind::CloseBraceToken;
}

bool LanguageParser::IsPossibleAggregateClauseStartOrStop() {
    return CurrentToken()->kind == SyntaxKind::ColonToken || CurrentToken()->kind == SyntaxKind::OpenBraceToken ||
           IsCurrentTokenWhereOfConstraintClause();
}

GreenNode *LanguageParser::ParseBaseList() {
    // We are only called from ParseMainTypeDeclaration which unilaterally sets this.
    CS_ASSERT(Any(_termState & TerminatorState::IsEndOfTypeSignature));

    GreenNode *colon = TryEatToken(SyntaxKind::ColonToken);
    if (colon == nullptr) return nullptr;

    // (local function)
    auto skipBadBaseListTokens = [this](GreenNode *&colon_, SyntaxListBuilder list_,
                                        SyntaxKind expected) -> PostSkipAction {
        return SkipBadSeparatedListTokensWithExpectedKind(
            colon_, list_,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleAttribute();
            },
            [](LanguageParser *p, SyntaxKind) {
                return p->CurrentToken()->kind == SyntaxKind::OpenBraceToken || p->IsCurrentTokenWhereOfConstraintClause();
            },
            expected);
    };

    SyntaxListBuilder list = _pool.AllocateSeparated();

    // Grammar requires at least one base type follow the colon.
    GreenNode *firstType = ParseType();
    list.Add(CurrentToken()->kind == SyntaxKind::OpenParenToken
                 ? SyntaxFactory::PrimaryConstructorBaseType(firstType, ParseParenthesizedArgumentList())
                 : SyntaxFactory::SimpleBaseType(firstType));

    // Parse any optional base types that follow.
    while (true) {
        if (CurrentToken()->kind == SyntaxKind::OpenBraceToken || CurrentToken()->kind == SyntaxKind::SemicolonToken ||
            IsCurrentTokenWhereOfConstraintClause()) {
            break;
        }

        if (CurrentToken()->kind == SyntaxKind::CommaToken) {
            list.AddSeparator(EatToken(SyntaxKind::CommaToken));
            list.Add(SyntaxFactory::SimpleBaseType(ParseType()));
            continue;
        }

        // Error recovery.  Code had an element in the base list, but wasn't followed by a comma or the start of
        // any production that normally follows in a type declaration.  See if this is just a case of a missing
        // comma between types in the base list.
        //
        // Note: if we see something that looks more like a modifier than a type (like 'file') do not try to
        // consume it as a type here, as we want to use that to better determine what member is actually following
        // this incomplete type declaration.
        if (GetModifierExcludingScoped(CurrentToken()) != DeclarationModifiers::None) {
            break;
        }

        if (IsPossibleType()) {
            list.AddSeparator(EatToken(SyntaxKind::CommaToken));
            list.Add(SyntaxFactory::SimpleBaseType(ParseType()));
            continue;
        }

        if (skipBadBaseListTokens(colon, list, SyntaxKind::CommaToken) == PostSkipAction::Abort) {
            break;
        }
    }

    return SyntaxFactory::BaseList(colon, _pool.ToListAndFree(list));
}

bool LanguageParser::IsCurrentTokenWhereOfConstraintClause() {
    return CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword &&
           PeekToken(1)->kind == SyntaxKind::IdentifierToken && PeekToken(2)->kind == SyntaxKind::ColonToken;
}

void LanguageParser::ParseTypeParameterConstraintClauses(SyntaxListBuilder list) {
    while (CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
        list.Add(ParseTypeParameterConstraintClause());
    }
}

GreenNode *LanguageParser::ParseTypeParameterConstraintClause() {
    GreenNode *where = EatContextualToken(SyntaxKind::WhereKeyword);
    GreenNode *name = !IsTrueIdentifier() ? AddError(CreateMissingIdentifierName(), ErrorCode::ERR_IdentifierExpected)
                                          : ParseIdentifierName();

    GreenNode *colon = EatToken(SyntaxKind::ColonToken);

    // (local function)
    auto skipBadTypeParameterConstraintTokens = [this](SyntaxListBuilder list,
                                                       SyntaxKind expected) -> PostSkipAction {
        GreenNode *tmp = nullptr;
        CS_ASSERT(list.Count() > 0);
        return SkipBadSeparatedListTokensWithExpectedKind(
            tmp, list,
            [](LanguageParser *p) {
                return p->CurrentToken()->kind != SyntaxKind::CommaToken && !p->IsPossibleTypeParameterConstraint();
            },
            [](LanguageParser *p, SyntaxKind) {
                return p->CurrentToken()->kind == SyntaxKind::OpenBraceToken || p->IsCurrentTokenWhereOfConstraintClause();
            },
            expected);
    };

    SyntaxListBuilder bounds = _pool.AllocateSeparated();

    // first bound
    if (CurrentToken()->kind == SyntaxKind::OpenBraceToken || IsCurrentTokenWhereOfConstraintClause()) {
        bounds.Add(SyntaxFactory::TypeConstraint(AddError(CreateMissingIdentifierName(), ErrorCode::ERR_TypeExpected)));
    } else {
        GreenNode *constraint = ParseTypeParameterConstraint();
        bounds.Add(constraint);

        // remaining bounds
        while (true) {
            bool haveComma;

            if (CurrentToken()->kind == SyntaxKind::OpenBraceToken ||
                (Any(_termState & TerminatorState::IsEndOfTypeSignature) &&
                 CurrentToken()->kind == SyntaxKind::SemicolonToken) ||
                CurrentToken()->kind == SyntaxKind::EqualsGreaterThanToken ||
                CurrentToken()->contextual_kind == SyntaxKind::WhereKeyword) {
                break;
            }
            // (C#: `haveComma = (Kind == CommaToken) || IsPossibleTypeParameterConstraint()` -- the assignment takes
            // the whole disjunction.)
            else if ((haveComma = (CurrentToken()->kind == SyntaxKind::CommaToken) || IsPossibleTypeParameterConstraint())) {
                GreenNode *separatorToken = EatToken(SyntaxKind::CommaToken);

                if (constraint->kind == SyntaxKind::AllowsConstraintClause && haveComma &&
                    !IsPossibleTypeParameterConstraint()) {
                    AddTrailingSkippedSyntax(bounds, AddError(separatorToken, ErrorCode::ERR_UnexpectedToken,
                                                              {SyntaxFacts::GetText(SyntaxKind::CommaToken)}));
                    break;
                }

                bounds.AddSeparator(separatorToken);
                if (IsCurrentTokenWhereOfConstraintClause()) {
                    bounds.Add(
                        SyntaxFactory::TypeConstraint(AddError(CreateMissingIdentifierName(), ErrorCode::ERR_TypeExpected)));
                    break;
                } else {
                    constraint = ParseTypeParameterConstraint();
                    bounds.Add(constraint);
                }
            } else if (skipBadTypeParameterConstraintTokens(bounds, SyntaxKind::CommaToken) == PostSkipAction::Abort) {
                break;
            }
        }
    }

    return SyntaxFactory::TypeParameterConstraintClause(where, name, colon, _pool.ToListAndFree(bounds));
}

bool LanguageParser::IsPossibleTypeParameterConstraint() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::NewKeyword:
        case SyntaxKind::ClassKeyword:
        case SyntaxKind::StructKeyword:
        case SyntaxKind::DefaultKeyword:
            return true;
        case SyntaxKind::IdentifierToken:

            return (CurrentToken()->contextual_kind == SyntaxKind::AllowsKeyword &&
                    PeekToken(1)->kind == SyntaxKind::RefKeyword) ||
                   IsTrueIdentifier();
        default:
            return IsPredefinedType(CurrentToken()->kind);
    }
}

GreenNode *LanguageParser::ParseTypeParameterConstraint() {
    // (local function)
    auto parseTypeOrAllowsConstraint = [this]() -> GreenNode * {
        if (CurrentToken()->contextual_kind == SyntaxKind::AllowsKeyword &&
            PeekToken(1)->kind == SyntaxKind::RefKeyword) {
            GreenNode *allows = EatContextualToken(SyntaxKind::AllowsKeyword);

            SyntaxListBuilder bounds = _pool.AllocateSeparated();

            while (true) {
                GreenNode *refKeyword = EatToken(SyntaxKind::RefKeyword);
                GreenNode *structKeyword = EatToken(SyntaxKind::StructKeyword);
                bounds.Add(SyntaxFactory::RefStructConstraint(refKeyword, structKeyword));

                if (CurrentToken()->kind == SyntaxKind::CommaToken && PeekToken(1)->kind == SyntaxKind::RefKeyword) {
                    bounds.AddSeparator(EatToken(SyntaxKind::CommaToken));
                    continue;
                }

                break;
            }

            return SyntaxFactory::AllowsConstraintClause(allows, _pool.ToListAndFree(bounds));
        }

        return SyntaxFactory::TypeConstraint(ParseType());
    };

    // (C# switch expression; the arguments are evaluated left to right, hence the locals.)
    switch (CurrentToken()->kind) {
        case SyntaxKind::NewKeyword: {
            GreenNode *newKeyword = EatToken();
            GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken);
            GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken);
            return SyntaxFactory::ConstructorConstraint(newKeyword, openParen, closeParen);
        }

        case SyntaxKind::StructKeyword: {
            GreenNode *classOrStructKeyword = EatToken();
            GreenNode *questionToken =
                CurrentToken()->kind == SyntaxKind::QuestionToken
                    ? AddError(EatToken(), ErrorCode::ERR_UnexpectedToken, {SyntaxFacts::GetText(SyntaxKind::QuestionToken)})
                    : nullptr;
            return SyntaxFactory::ClassOrStructConstraint(SyntaxKind::StructConstraint, classOrStructKeyword,
                                                          questionToken);
        }

        case SyntaxKind::ClassKeyword: {
            GreenNode *classOrStructKeyword = EatToken();
            GreenNode *questionToken = TryEatToken(SyntaxKind::QuestionToken);
            return SyntaxFactory::ClassOrStructConstraint(SyntaxKind::ClassConstraint, classOrStructKeyword,
                                                          questionToken);
        }

        case SyntaxKind::DefaultKeyword:
            return SyntaxFactory::DefaultConstraint(/*defaultKeyword:*/ EatToken());

        case SyntaxKind::EnumKeyword: {
            GreenNode *missing = AddError(CreateMissingIdentifierName(), ErrorCode::ERR_NoEnumConstraint);
            return SyntaxFactory::TypeConstraint(AddTrailingSkippedSyntax(missing, EatToken()));
        }

        // Produce a specific diagnostic for `where T : delegate`
        // but not `where T : delegate*<...>
        case SyntaxKind::DelegateKeyword: {
            if (PeekToken(1)->kind == SyntaxKind::AsteriskToken) {
                return SyntaxFactory::TypeConstraint(ParseType());
            }
            GreenNode *missing = AddError(CreateMissingIdentifierName(), ErrorCode::ERR_NoDelegateConstraint);
            return SyntaxFactory::TypeConstraint(AddTrailingSkippedSyntax(missing, EatToken()));
        }

        default:
            return parseTypeOrAllowsConstraint();
    }
}

bool LanguageParser::IsPossibleMemberStart() { return CanStartMember(CurrentToken()->kind); }

}  // namespace csharp
