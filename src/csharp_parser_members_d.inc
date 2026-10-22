// LanguageParser.cs lines 8197-11027 (unit D, csharp_parser_d.cpp): statements, blocks, local declarations,
// local functions. Included inside class LanguageParser.

GreenNode *ParseStatement();
GreenNode *ParsePossiblyAttributedStatement();
SyntaxList ParseStatementAttributeDeclarations();
GreenNode *ParseStatementCore(SyntaxList attributes, bool isGlobal);
GreenNode *TryReuseStatement(SyntaxList attributes, bool isGlobal);
// RP is LanguageParser::ResetPoint, which is declared after this .inc (a plain `ResetPoint` here would name
// SyntaxParser::ResetPoint); only csharp_parser_d.cpp calls these two, so the templates are instantiated there.
template <class RP>
GreenNode *ParseStatementCoreRest(SyntaxList attributes, bool isGlobal, RP &resetPointBeforeStatement);
GreenNode *TryParseStatementStartingWithIdentifier(SyntaxList attributes, bool isGlobal);
GreenNode *ParseStatementStartingWithUsing(SyntaxList attributes);
GreenNode *TryParseStatementStartingWithUnsafe(SyntaxList attributes);
bool IsPossibleAwaitUsing();
bool IsPossibleLabeledStatement();
bool IsPossibleUnsafeStatement();
bool IsPossibleYieldStatement();
bool IsPossibleLocalDeclarationStatement(bool isGlobalScriptLevel);
bool IsPossibleFirstTypedIdentifierInLocalDeclarationStatement(bool isGlobalScriptLevel);
bool IsPossibleTopLevelUsingLocalDeclarationStatement();
bool IsPossibleDeclarationStatementFollowingNullableType(bool isGlobalScriptLevel);
bool IsPossibleFieldDeclarationFollowingNullableType();
bool IsPossibleMethodDeclarationFollowingNullableType();
bool IsAnonymousDelegateExpression();
bool IsPossibleNewExpression();
std::optional<bool> IsPossibleTypedIdentifierStart(GreenNode *current, GreenNode *next, bool allowThisKeyword);
GreenNode *ParsePossiblyAttributedBlock();
GreenNode *ParseMethodOrAccessorBodyBlock(SyntaxList attributes, bool isAccessorBody);
GreenNode *ParseBlock(SyntaxList attributes);
static bool IsLargeEnoughNonEmptyStatementList(SyntaxListBuilder statements);
void ParseStatements(GreenNode *&previousNode, SyntaxListBuilder statements, bool stopOnSwitchSections);
bool IsPossibleStatementStartOrStop();
PostSkipAction SkipBadStatementListTokens(SyntaxListBuilder statements, SyntaxKind expected, GreenNode *&trailingTrivia);
bool IsDefiniteStatement();
bool IsPossibleStatement();
GreenNode *ParseFixedStatement(SyntaxList attributes);
bool IsEndOfFixedStatement();
GreenNode *ParseEmbeddedStatement();
GreenNode *ParseBreakStatement(SyntaxList attributes);
GreenNode *ParseContinueStatement(SyntaxList attributes);
GreenNode *ParseTryStatement(SyntaxList attributes);
bool IsEndOfTryBlock();
GreenNode *ParseCatchClause();
bool IsEndOfCatchClause();
bool IsEndOfFilterClause();
bool IsEndOfCatchBlock();
GreenNode *ParseCheckedStatement(SyntaxList attributes);
GreenNode *ParseDoStatement(SyntaxList attributes);
bool IsEndOfDoWhileExpression();
GreenNode *ParseForOrForEachStatement(SyntaxList attributes);
GreenNode *ParseForStatement(SyntaxList attributes);
bool IsEndOfForStatementArgument();
GreenNode *ParseForEachStatement(SyntaxList attributes, GreenNode *awaitTokenOpt);
GreenNode *ParseExpressionOrDeclaration(ParseTypeMode mode, bool permitTupleDesignation);
bool IsPossibleDeclarationExpression(ParseTypeMode mode, bool permitTupleDesignation, bool &isScoped);
bool IsVarType();
static bool IsValidForeachVariable(GreenNode *variable);
GreenNode *ParseGotoStatement(SyntaxList attributes);
GreenNode *ParseExpressionForParenthesizedConstruct();
GreenNode *ParseErrantExpressionWhenNoCloseParenToken(GreenNode *expression);
GreenNode *ParseIfStatement(SyntaxList attributes);
GreenNode *ParseMisplacedElse(SyntaxList attributes);
GreenNode *ParseElseClauseOpt();
GreenNode *ParseLockStatement(SyntaxList attributes);
GreenNode *ParseReturnStatement(SyntaxList attributes);
GreenNode *ParseYieldStatement(SyntaxList attributes);
GreenNode *ParseSwitchStatement(SyntaxList attributes);
bool IsPossibleSwitchSection();
GreenNode *ParseSwitchSection();
GreenNode *ParseThrowStatement(SyntaxList attributes);
GreenNode *ParseUnsafeStatement(SyntaxList attributes);
GreenNode *ParseUsingStatement(SyntaxList attributes, GreenNode *awaitTokenOpt = nullptr);
template <class RP>  // RP is LanguageParser::ResetPoint (see ParseStatementCoreRest)
void ParseUsingExpression(GreenNode *&declaration, GreenNode *&expression, RP &resetPoint);
bool IsUsingStatementVariableDeclaration(ScanTypeFlags st);
GreenNode *ParseWhileStatement(SyntaxList attributes);
GreenNode *ParseLabeledStatement(SyntaxList attributes);
GreenNode *ParseLocalDeclarationStatement(SyntaxList attributes);
bool IsDefiniteScopedModifier(bool isFunctionPointerParameter, bool isLambdaParameter);
GreenNode *ParsePossibleScopedKeyword(bool isFunctionPointerParameter, bool isLambdaParameter);
GreenNode *ParseDesignation(bool forPattern);
GreenNode *ParseSimpleDesignation();
GreenNode *ParseWhenClause(Precedence precedence);
GreenNode *ParseParenthesizedVariableDeclaration(VariableFlags initialFlags, GreenNode *scopedKeyword);
void ParseLocalDeclaration(SyntaxListBuilder variables, bool allowLocalFunctions, bool stopOnCloseParen,
                           SyntaxList attributes, SyntaxList mods, GreenNode *scopedKeyword, VariableFlags initialFlags,
                           GreenNode *&type, GreenNode *&localFunction);
bool IsEndOfDeclarationClause();
void ParseLocalDeclarationStatementModifiers(SyntaxListBuilder list, bool isUsingDeclaration);
static bool IsDeclarationModifier(SyntaxKind kind);
static bool IsAdditionalLocalFunctionModifier(SyntaxKind kind);
static bool IsAccessibilityModifier(SyntaxKind kind);
GreenNode *TryParseLocalFunctionStatementBody(SyntaxList attributes, SyntaxList modifiers, GreenNode *type,
                                              GreenNode *identifier);
