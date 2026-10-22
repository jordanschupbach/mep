// Unit F of LanguageParser (included inside class LanguageParser):
//   csharp_parser_f.cpp  -- LanguageParser.cs from ScanCast to the end
//   csharp_parser_f2.cpp -- LanguageParser_Patterns.cs, LanguageParser_InterpolatedString.cs
// (GetResetPoint / Reset / Release are declared in csharp_parser_internal.h and defined in csharp_parser_f.cpp.)

// --- LanguageParser.cs: casts, new, collection expressions, initializers ------------------------
bool ScanCast(bool forPattern = false, bool inSwitchArmPattern = false);
bool IsPossibleLambdaExpression(Precedence precedence);
static bool CanFollowCast(SyntaxKind kind);
GreenNode *ParseNewExpression();
GreenNode *ParseCollectionExpression();
bool IsPossibleCollectionElement();
GreenNode *ParseCollectionElement();
bool IsAnonymousType();
GreenNode *ParseAnonymousTypeExpression();
GreenNode *ParseAnonymousTypeMemberInitializer();
bool IsInitializerMember();
bool IsComplexElementInitializer();
bool IsNamedAssignment();
bool IsNamedMemberInitializer();
bool IsDictionaryInitializer();
GreenNode *ParseArrayOrObjectCreationExpression();
bool IsImplicitObjectCreation();
GreenNode *ParseWithExpression(GreenNode *receiverExpression, GreenNode *withKeyword);
GreenNode *ParseObjectOrCollectionInitializer();
GreenNode *ParseObjectOrCollectionInitializerMember();
static PostSkipAction SkipBadInitializerListTokens(LanguageParser *self, GreenNode *&startToken, SyntaxListBuilder list,
                                                   SyntaxKind expectedKind, SyntaxKind closeKind);
GreenNode *ParseObjectInitializerNamedAssignment();
GreenNode *ParseDictionaryInitializer();
GreenNode *ParseComplexElementInitializer();
bool IsImplicitlyTypedArray();
GreenNode *ParseImplicitlyTypedArrayCreation();
GreenNode *ParseArrayInitializer();
GreenNode *ParseStackAllocExpression();
GreenNode *ParseImplicitlyTypedStackAllocExpression();
GreenNode *ParseRegularStackAllocExpression();

// --- anonymous methods, lambdas ------------------------------------------------------------------
GreenNode *ParseAnonymousMethodExpression();
SyntaxList ParseAnonymousFunctionModifiers();
bool IsAnonymousFunctionAsyncModifier();
GreenNode *TryParseLambdaExpression();
GreenNode *ParseLambdaExpression();
/** @brief (block, expressionBody). */
std::pair<GreenNode *, GreenNode *> ParseLambdaBody();
GreenNode *ParseLambdaParameterList();
bool IsPossibleLambdaParameter();
GreenNode *ParseLambdaParameter();
bool ShouldParseLambdaParameterType();

// --- queries -----------------------------------------------------------------------------------------
bool IsCurrentTokenQueryContextualKeyword();
static bool IsTokenQueryContextualKeyword(GreenNode *token);
static bool IsTokenStartOfNewQueryClause(GreenNode *token);
bool IsQueryExpression(bool mayBeVariableDeclaration, bool mayBeMemberDeclaration);
bool IsQueryExpressionAfterFrom(bool mayBeVariableDeclaration, bool mayBeMemberDeclaration);
GreenNode *ParseQueryExpression(Precedence precedence);
GreenNode *ParseQueryBody();
GreenNode *ParseFromClause();
GreenNode *ParseJoinClause();
GreenNode *ParseLetClause();
GreenNode *ParseWhereClause();
GreenNode *ParseOrderByClause();
GreenNode *ParseOrdering();
GreenNode *ParseSelectClause();
GreenNode *ParseGroupClause();
GreenNode *ParseQueryContinuation();

// --- lists, reset points, misc ----------------------------------------------------------------------
/** @brief Always false: there is no incremental parsing (code under it is dead). */
bool IsIncrementalAndFactoryContextMatches() const { return false; }
/** @brief ParseCommaSeparatedSyntaxList without immediatelyAbort. */
SeparatedSyntaxList ParseCommaSeparatedSyntaxList(GreenNode *&openToken, SyntaxKind closeTokenKind,
                                                  const IsPossibleElementFn &isPossibleElement,
                                                  const ParseElementFn &parseElement,
                                                  const SkipBadTokensFn &skipBadTokens, bool allowTrailingSeparator,
                                                  bool requireOneElement, bool allowSemicolonAsSeparator);
/** @brief immediatelyAbort may be nullptr (an empty std::function). */
SeparatedSyntaxList ParseCommaSeparatedSyntaxList(GreenNode *&openToken, SyntaxKind closeTokenKind,
                                                  const IsPossibleElementFn &isPossibleElement,
                                                  const ParseElementFn &parseElement,
                                                  const ImmediatelyAbortFn &immediatelyAbort,
                                                  const SkipBadTokensFn &skipBadTokens, bool allowTrailingSeparator,
                                                  bool requireOneElement, bool allowSemicolonAsSeparator);
GreenNode *ConsumeUnexpectedTokens(GreenNode *node);
static bool ContainsErrorDiagnostic(GreenNode *node);

// --- LanguageParser_Patterns.cs (csharp_parser_f2.cpp) ----------------------------------------------
GreenNode *ParseTypeOrPatternForIsOperator();
bool ConvertExpressionToType(GreenNode *expression, GreenNode *&type);
GreenNode *ParsePattern(Precedence precedence, bool afterIs = false, bool inSwitchArmPattern = false);
GreenNode *ParseDisjunctivePattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern);
bool LooksLikeTypeOfPattern();
GreenNode *ParseConjunctivePattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern);
bool ScanDesignation(bool permitTuple);
GreenNode *ParseNegatedPattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern);
GreenNode *ParsePrimaryPattern(Precedence precedence, bool afterIs, bool inSwitchArmPattern);
bool CanTokenFollowTypeInPattern(Precedence precedence);
GreenNode *ParsePatternContinued(GreenNode *type, Precedence precedence, bool inSwitchArmPattern);
GreenNode *TryParseSimpleDesignation(bool whenIsKeyword);
bool IsValidPatternDesignation(bool inSwitchArmPattern);
GreenNode *ParseExpressionOrPatternForSwitchStatement();
GreenNode *ConvertPatternToExpressionIfPossible(GreenNode *pattern, bool permitTypeArguments = false);
bool ConvertTypeToExpression(GreenNode *type, GreenNode *&expr, bool permitTypeArguments = false);
bool LooksLikeTupleArrayType();
GreenNode *ParsePropertyPatternClause();
GreenNode *ParseSubpatternElement();
bool IsPossibleSubpatternElement();
static PostSkipAction SkipBadPatternListTokens(LanguageParser *self, GreenNode *&open, SyntaxListBuilder list,
                                               SyntaxKind expectedKind, SyntaxKind closeKind);
GreenNode *ParseSwitchExpression(GreenNode *governingExpression, GreenNode *switchKeyword);
GreenNode *ParseListPattern(bool inSwitchArmPattern);

// --- LanguageParser_InterpolatedString.cs (csharp_parser_f2.cpp) -----------------------------------
GreenNode *ParseRawStringToken();
GreenNode *ParseInterpolatedStringToken();
GreenNode *ParseInterpolatedOrRawStringToken(GreenNode *originalToken, bool isInterpolatedString);
static std::u16string CharToString(char16_t ch);
static bool CheckForSpaceDifference(Text currentLineWhitespace, Text indentationLineWhitespace,
                                    std::u16string &currentLineMessage, std::u16string &indentationLineMessage);
static GreenNode *TokenOrMissingToken(GreenNode *leading, SyntaxKind kind, Text text, GreenNode *trailing);
static int SkipWhitespace(Text text, int currentIndex);
static int ConsumeRemainingContentThroughNewLine(std::u16string &content, Text text, int currentIndex);
/** @brief The static overload (C#'s options parameter dropped: the options are fixed). */
static GreenNode *ParseInterpolation(Text text, Lexer::Interpolation interpolation, Lexer::InterpolatedStringKind kind,
                                     bool isInAsyncContext, bool isInFieldKeywordContext);
GreenNode *ParseInterpolation(Text text, Lexer::Interpolation interpolation, Lexer::InterpolatedStringKind kind,
                              GreenNode *openBraceToken);
GreenNode *MakeInterpolatedStringTextToken(Lexer::InterpolatedStringKind kind, Text text);
/** @brief The diagnostics of `infos` shifted by `offset` (empty in, empty out). */
static std::vector<SyntaxDiagnosticInfo> MoveDiagnostics(const SyntaxDiagnosticInfo *infos, int count, int offset);
