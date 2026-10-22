// LanguageParser members of unit A (LanguageParser.cs lines 1-2380), defined in csharp_parser_a.cpp.
// Included inside class LanguageParser (csharp_parser_internal.h). The constructor, destructor and
// ParseCompilationUnit() are declared in the header itself.

static bool IsSomeWord(SyntaxKind kind);
bool IsTerminator();
GreenNode *ParseCompilationUnitCore();
/** @brief ParseWithStackGuard<TNode>: runs parseFunc; on InsufficientExecutionStack, createEmptyNodeFunc's node
 *         carrying the whole text as skipped text plus ERR_InsufficientStack. */
GreenNode *ParseWithStackGuard(const ParseElementFn &parseFunc, const ParseElementFn &createEmptyNodeFunc);
GreenNode *CreateForGlobalFailure(int position, GreenNode *node);
GreenNode *ParseNamespaceDeclaration(SyntaxList attributeLists, SyntaxListBuilder modifiers);
GreenNode *ParseNamespaceDeclarationCore(SyntaxList attributeLists, SyntaxListBuilder modifiers);
static bool IsPossibleStartOfTypeDeclaration(SyntaxKind kind);
static bool IsTypeModifierOrTypeKeyword(SyntaxKind kind);
void AddSkippedNamespaceText(GreenNode *&openBraceOrSemicolon, NamespaceBodyBuilder &body,
                             SyntaxListBuilder &initialBadNodes, GreenNode *skippedSyntax);
void ParseNamespaceBody(GreenNode *&openBraceOrSemicolon, NamespaceBodyBuilder &body,
                        SyntaxListBuilder &initialBadNodes, SyntaxKind parentKind);
static bool IsMemberDeclarationOnlyValidWithinTypeDeclaration(GreenNode *memberDeclaration);
void ParseNamespaceBodyWorker(GreenNode *&openBraceOrSemicolon, NamespaceBodyBuilder &body,
                              SyntaxListBuilder &initialBadNodes, SyntaxKind parentKind,
                              bool &sawMemberDeclarationOnlyValidWithinTypeDeclaration);
static void AddIncompleteMembers(SyntaxListBuilder &incompleteMembers, NamespaceBodyBuilder &body);
void ReduceIncompleteMembers(SyntaxListBuilder &incompleteMembers, GreenNode *&openBraceOrSemicolon,
                             NamespaceBodyBuilder &body, SyntaxListBuilder &initialBadNodes);
bool IsPossibleNamespaceMemberDeclaration();
bool IsPartialInNamespaceMemberDeclaration();
bool IsEndOfNamespace();
bool IsGobalAttributesTerminator();
bool IsNamespaceMemberStartOrStop();
bool ScanExternAliasDirective();
GreenNode *ParseExternAliasDirective();
GreenNode *ParseNameEquals();
GreenNode *ParseUsingDirective();
bool IsPossibleGlobalAttributeDeclaration();
static bool IsGlobalAttributeTarget(GreenNode *token);
bool IsPossibleAttributeDeclaration();
SyntaxList ParseAttributeDeclarations(bool inExpressionContext);
bool IsAttributeDeclarationTerminator();
bool IsAttributeTarget();
GreenNode *TryParseAttributeDeclaration(bool inExpressionContext);
bool IsPossibleAttribute();
GreenNode *ParseAttribute();
GreenNode *ParseAttributeArgumentList();
bool IsPossibleAttributeArgument();
GreenNode *ParseAttributeArgument();
static DeclarationModifiers GetModifierExcludingScoped(GreenNode *token);
static DeclarationModifiers GetModifierExcludingScoped(SyntaxKind kind, SyntaxKind contextualKind);
void ParseModifiers(SyntaxListBuilder tokens, bool forAccessors, bool forTopLevelStatements,
                    bool &isPossibleTypeDeclaration);
bool ShouldContextualKeywordBeTreatedAsModifier(bool parsingStatementNotDeclaration);
static bool IsNonContextualModifier(GreenNode *nextToken);
bool IsPartialType();
bool IsPartialMember();
bool IsPossibleMemberName();
GreenNode *ParseTypeDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
GreenNode *ParseMainTypeDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
void SkipBadMemberListTokens(GreenNode *&openBrace, SyntaxListBuilder members);
void SkipBadMemberListTokens(GreenNode *&previousNode);
bool IsPossibleMemberStartOrStop();
bool IsPossibleAggregateClauseStartOrStop();
GreenNode *ParseBaseList();
bool IsCurrentTokenWhereOfConstraintClause();
void ParseTypeParameterConstraintClauses(SyntaxListBuilder list);
GreenNode *ParseTypeParameterConstraintClause();
bool IsPossibleTypeParameterConstraint();
GreenNode *ParseTypeParameterConstraint();
bool IsPossibleMemberStart();
