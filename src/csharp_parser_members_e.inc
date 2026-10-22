// LanguageParser members of translation unit E (LanguageParser.cs 11028-12893: ParseExpressionStatement ..
// ParseTupleExpressionTail), defined in csharp_parser_e.cpp. Included inside class LanguageParser.

GreenNode *ParseExpressionStatement(SyntaxList attributes);
GreenNode *ParseExpressionStatement(SyntaxList attributes, GreenNode *expression);
GreenNode *ParseExpression();
GreenNode *ParseExpressionCore();
bool CanStartExpression();
bool IsPossibleExpression();
bool IsPossibleExpression(bool allowBinaryExpressions, bool allowAssignmentExpressions);
static bool IsInvalidSubExpression(SyntaxKind kind);
static bool IsRightAssociative(SyntaxKind op);
static Precedence GetPrecedence(SyntaxKind op);
static bool IsExpectedPrefixUnaryOperator(SyntaxKind kind);
static bool IsExpectedBinaryOperator(SyntaxKind kind);
static bool IsExpectedAssignmentOperator(SyntaxKind kind);
bool IsPossibleAwaitExpressionStatement();
bool IsAwaitExpression();
GreenNode *ParseSubExpression(Precedence precedence);
GreenNode *ParseSubExpressionCore(Precedence precedence);
GreenNode *ParseExpressionContinued(GreenNode *unaryOrPrimaryExpression, Precedence precedence);
/** @brief Returns (operatorTokenKind, operatorExpressionKind). */
std::pair<SyntaxKind, SyntaxKind> GetExpressionOperatorTokenKindAndExpressionKind();
GreenNode *EatExpressionOperatorToken(SyntaxKind operatorTokenKind);
GreenNode *ParseAssignmentExpression(SyntaxKind operatorExpressionKind, GreenNode *leftOperand, GreenNode *operatorToken);
bool IsAtDotDotToken();
static bool IsAtDotDotToken(GreenNode *token1, GreenNode *token2);
GreenNode *EatDotDotToken();
GreenNode *ParseDeclarationExpression(ParseTypeMode mode, bool isScoped);
GreenNode *ParseThrowExpression();
GreenNode *ParseIsExpression(GreenNode *leftOperand, GreenNode *opToken);
GreenNode *ParsePrimaryExpression(Precedence precedence);
GreenNode *ParseBaseExpression();
bool IsPossibleDeconstructionLeft(Precedence precedence);
bool ScanDesignator();
bool IsPossibleAnonymousMethodExpression();
bool TryParseConditionalAccessExpression(GreenNode *primaryExpression, GreenNode *&conditionalAccessExpression);
GreenNode *ParseParenthesizedArgumentList();
GreenNode *ParseBracketedArgumentList();
void ParseArgumentList(GreenNode *&openToken, SeparatedSyntaxList &arguments, GreenNode *&closeToken,
                       SyntaxKind openKind, SyntaxKind closeKind);
bool IsEndOfArgumentList();
bool IsPossibleArgumentExpression();
static bool IsValidArgumentRefKindKeyword(SyntaxKind kind);
GreenNode *ParseArgumentExpression(bool isIndexer);
GreenNode *ParseTypeOfExpression();
GreenNode *ParseDefaultExpression();
GreenNode *ParseSizeOfExpression();
GreenNode *ParseMakeRefExpression();
GreenNode *ParseRefTypeExpression();
GreenNode *ParseCheckedOrUncheckedExpression();
GreenNode *ParseRefValueExpression();
bool ScanParenthesizedLambda(Precedence precedence);
bool ScanImplicitlyTypedLambdaOrSimpleExplicitlyTypedParenthesizedLambda(Precedence precedence);
bool ScanExplicitlyTypedLambda(Precedence precedence);
GreenNode *ParseCastOrParenExpressionOrTuple();
GreenNode *ParseTupleExpressionTail(GreenNode *openParen, GreenNode *firstArg);
