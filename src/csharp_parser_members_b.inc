// LanguageParser members of unit B (LanguageParser.cs 2381-5247: CanStartMember .. ParseEventFieldDeclaration),
// defined in csharp_parser_b.cpp. Included inside class LanguageParser. Roslyn's names, order and parameters.
// Not ported (incremental parsing only): CanReuseMemberDeclaration, CanReuseParameterList,
// CanReuseBracketedParameterList, CanReuseParameter.

static bool CanStartMember(SyntaxKind kind);
bool IsTypeDeclarationStart();
GreenNode *ParseMemberDeclaration();
GreenNode *ParseMemberDeclarationOrStatement(SyntaxKind parentKind);
GreenNode *ParseMemberDeclarationOrStatementCore(SyntaxKind parentKind);
bool IsMisplacedModifier(SyntaxListBuilder modifiers, SyntaxList attributes, GreenNode *type, GreenNode *&result);
bool IsNoneOrIncompleteMember(SyntaxKind parentKind, SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                              GreenNode *explicitInterfaceOpt, GreenNode *identifierOrThisOpt,
                              GreenNode *typeParameterListOpt, GreenNode *&result);
// RP is LanguageParser::ResetPoint, which is declared after this .inc (a plain `ResetPoint` here would name
// SyntaxParser::ResetPoint); only csharp_parser_b.cpp calls it, so the template is instantiated there.
template <class RP>
bool ReconsideredTypeAsAsyncModifier(SyntaxListBuilder &modifiers, GreenNode *&type, RP &afterTypeResetPoint,
                                     GreenNode *&explicitInterfaceOpt, GreenNode *&identifierOrThisOpt,
                                     GreenNode *&typeParameterListOpt);
bool TryParseIndexerOrPropertyDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                          GreenNode *explicitInterfaceOpt, GreenNode *identifierOrThisOpt,
                                          GreenNode *typeParameterListOpt, GreenNode *&result);
bool IsStartOfPropertyBody(int index);
GreenNode *ParseMemberDeclaration(SyntaxKind parentKind);
GreenNode *ParseMemberDeclarationCore(SyntaxKind parentKind);
bool IsExtensionContainerStart();
static bool ReconsiderTypeAsAsyncModifier(SyntaxListBuilder &modifiers, GreenNode *type, GreenNode *identifierOrThisOpt);
bool IsFieldDeclaration(bool isEvent, bool isGlobalScriptLevel);
bool IsOperatorKeyword();
static bool IsComplete(GreenNode *node);
GreenNode *ParseConstructorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
GreenNode *TryParseConstructorInitializer();
GreenNode *ParseConstructorInitializer();
GreenNode *ParseDestructorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
void ParseBlockAndExpressionBodiesWithSemicolon(GreenNode *&blockBody, GreenNode *&expressionBody,
                                                GreenNode *&semicolon, bool parseSemicolonAfterBlock = true);
bool IsEndOfTypeParameterList();
bool IsEndOfMethodSignature();
bool IsEndOfTypeSignature();
bool IsEndOfNameInExplicitInterface();
bool IsEndOfFunctionPointerParameterList(bool errored);
bool IsEndOfFunctionPointerCallingConvention();
GreenNode *ParseMethodDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                  GreenNode *explicitInterfaceOpt, GreenNode *identifier, GreenNode *typeParameterList);
GreenNode *ParseReturnType();
bool IsEndOfReturnType();
GreenNode *TryParseConversionOperatorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
GreenNode *TryEatCheckedOrHandleUnchecked(GreenNode *&operatorKeyword);
GreenNode *ParseOperatorDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                    GreenNode *explicitInterfaceOpt);
GreenNode *ParseIndexerDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                   GreenNode *explicitInterfaceOpt, GreenNode *thisKeyword,
                                   GreenNode *typeParameterList);
GreenNode *ParsePropertyDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                    GreenNode *explicitInterfaceOpt, GreenNode *identifier,
                                    GreenNode *typeParameterList);
GreenNode *ParseAccessorList(AccessorDeclaringKind declaringKind);
GreenNode *ParseArrowExpressionClause();
GreenNode *ParsePossibleRefExpression();
PostSkipAction SkipBadAccessorListTokens(GreenNode *&openBrace, SyntaxListBuilder list, ErrorCode error);
bool IsPossibleAccessor();
bool IsPossibleAccessorModifier();
/** @brief SkipBadSeparatedListTokensWithExpectedKind<T, TNode> (T and TNode are node types: GreenNode). */
PostSkipAction SkipBadSeparatedListTokensWithExpectedKind(
    GreenNode *&startToken, SyntaxListBuilder list, const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *, SyntaxKind)> &abortFunction, SyntaxKind expected,
    SyntaxKind closeKind = SyntaxKind::None);
/** @brief SkipBadListTokensWithErrorCode<T, TNode>. */
PostSkipAction SkipBadListTokensWithErrorCode(GreenNode *&startToken, SyntaxListBuilder list,
                                              const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
                                              const std::function<bool(LanguageParser *)> &abortFunction,
                                              ErrorCode error);
PostSkipAction SkipBadListTokensWithExpectedKindHelper(
    SyntaxListBuilder list, const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
    const std::function<bool(LanguageParser *, SyntaxKind)> &abortFunction, SyntaxKind expected, SyntaxKind closeKind,
    GreenNode *&trailingTrivia);
/** @brief SkipBadListTokensWithErrorCodeHelper<TNode>. */
PostSkipAction SkipBadListTokensWithErrorCodeHelper(SyntaxListBuilder list,
                                                    const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
                                                    const std::function<bool(LanguageParser *)> &abortFunction,
                                                    ErrorCode error, GreenNode *&trailingTrivia);
PostSkipAction SkipBadTokensWithExpectedKind(const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
                                             const std::function<bool(LanguageParser *, SyntaxKind)> &abortFunction,
                                             SyntaxKind expected, SyntaxKind closeKind, GreenNode *&trailingTrivia);
PostSkipAction SkipBadTokensWithErrorCode(const std::function<bool(LanguageParser *)> &isNotExpectedFunction,
                                          const std::function<bool(LanguageParser *)> &abortFunction,
                                          ErrorCode errorCode, GreenNode *&trailingTrivia);
GreenNode *ParseAccessorDeclaration(AccessorDeclaringKind declaringKind);
GreenNode *EatAccessorSemicolon();
static SyntaxKind GetAccessorKind(GreenNode *accessorName);
GreenNode *ParseParenthesizedParameterList(bool forExtensionOrUnion);
GreenNode *ParseBracketedParameterList();
SeparatedSyntaxList ParseParameterList(GreenNode *&open, GreenNode *&close, SyntaxKind openKind, SyntaxKind closeKind,
                                       bool forExtensionOrUnion);
bool IsEndOfParameterList();
bool IsPossibleParameter();
GreenNode *ParseParameter(bool identifierIsOptional);
static bool NoTriviaBetween(GreenNode *token1, GreenNode *token2);
static bool IsParameterModifierIncludingScoped(GreenNode *token);
static bool IsParameterModifierExcludingScoped(GreenNode *token);
void ParseParameterModifiers(SyntaxListBuilder modifiers, bool isFunctionPointerParameter, bool isLambdaParameter);
GreenNode *ParseFixedSizeBufferDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, SyntaxKind parentKind);
GreenNode *ParseEventDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, SyntaxKind parentKind);
GreenNode *ParseEventDeclarationWithAccessors(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *eventToken,
                                              GreenNode *type);
/** @brief EatUnexpectedTrailingSemicolon<TNode>. */
GreenNode *EatUnexpectedTrailingSemicolon(GreenNode *decl);
GreenNode *ParseNormalFieldDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *type,
                                       SyntaxKind parentKind);
GreenNode *ParseEventFieldDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, GreenNode *eventToken,
                                      GreenNode *type, SyntaxKind parentKind);
