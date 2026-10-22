// LanguageParser.cs lines 5248-8196 (unit C: fields, variables, enums, names and types), included inside
// class LanguageParser (csharp_parser_internal.h). Defined in csharp_parser_c.cpp, in the C# order.
// GetOriginalModifiers / WasFirstVariable / GetOriginalVariableFlags / CanReuseVariableDeclarator are
// incremental-parsing helpers over red nodes and are not ported.

bool IsEndOfFieldDeclaration();
SeparatedSyntaxList ParseFieldDeclarationVariableDeclarators(GreenNode *type, VariableFlags flags, SyntaxKind parentKind);
void ParseVariableDeclarators(GreenNode *type, VariableFlags flags, SeparatedSyntaxListBuilder variables,
                              bool variableDeclarationsExpected, bool allowLocalFunctions, bool stopOnCloseParen,
                              SyntaxList attributes, SyntaxList mods, GreenNode *&localFunction);
PostSkipAction SkipBadVariableListTokens(SeparatedSyntaxListBuilder list, SyntaxKind expected);
GreenNode *ParseVariableDeclarator(GreenNode *parentType, VariableFlags flags, bool isFirst, bool allowLocalFunctions,
                                   SyntaxList attributes, SyntaxList mods, GreenNode *&localFunction,
                                   bool isExpressionContext = false);
bool IsLocalFunctionAfterIdentifier();
bool IsPossibleEndOfVariableDeclaration();
GreenNode *ParseVariableInitializer();
bool IsPossibleVariableInitializer();
GreenNode *ParseConstantFieldDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers, SyntaxKind parentKind);
GreenNode *ParseDelegateDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
GreenNode *ParseEnumDeclaration(SyntaxList attributes, SyntaxListBuilder modifiers);
GreenNode *ParseEnumMemberDeclaration();
bool IsPossibleEnumMemberDeclaration();
bool IsDotOrColonColon();
GreenNode *ParseName();
GreenNode *CreateMissingIdentifierName();
static GreenNode *CreateMissingIdentifierToken();
bool IsTrueIdentifier();
bool IsTrueIdentifier(GreenNode *token);
GreenNode *ParseIdentifierName(ErrorCode code = ErrorCode::ERR_IdentifierExpected);
GreenNode *ParseIdentifierToken(ErrorCode code = ErrorCode::ERR_IdentifierExpected);
bool IsCurrentTokenQueryKeywordInQuery();
bool IsCurrentTokenPartialKeywordOfPartialMemberOrType();
bool IsCurrentTokenFieldInKeywordContext();
GreenNode *ParseTypeParameterList();
bool IsStartOfTypeParameter();
GreenNode *ParseTypeParameter();
GreenNode *ParseSimpleName(NameOptions options = NameOptions::None);
ScanTypeArgumentListKind ScanTypeArgumentList(NameOptions options);
ScanTypeFlags ScanPossibleTypeArgumentList(GreenNode *&greaterThanToken, bool &isDefinitelyTypeArgumentList);
void ParseTypeArgumentList(GreenNode *&open, SeparatedSyntaxListBuilder types, GreenNode *&close);
PostSkipAction SkipBadTypeArgumentListTokens(SeparatedSyntaxListBuilder list, SyntaxKind expected);
GreenNode *ParseTypeArgument();
bool IsEndOfTypeArgumentList();
bool IsOpenName();
void ParseMemberName(GreenNode *&explicitInterfaceOpt, GreenNode *&identifierOrThisOpt,
                     GreenNode *&typeParameterListOpt, bool isEvent);
void AccumulateExplicitInterfaceName(GreenNode *&explicitInterfaceName, GreenNode *&separator);
bool IsOperatorStart(GreenNode *&explicitInterfaceOpt, bool advanceParser = true);
GreenNode *ParseAliasQualifiedName(NameOptions allowedParts = NameOptions::None);
GreenNode *ParseQualifiedName(NameOptions options = NameOptions::None);
GreenNode *ParseQualifiedNameRight(NameOptions options, GreenNode *left, GreenNode *separator);
GreenNode *ConvertToMissingWithTrailingTrivia(GreenNode *token, SyntaxKind expectedKind);
bool IsPossibleType();
ScanTypeFlags ScanType(bool forPattern = false);
ScanTypeFlags ScanType(GreenNode *&lastTokenOfType, bool forPattern = false);
void ScanNamedTypePart();
ScanTypeFlags ScanNamedTypePart(GreenNode *&lastTokenOfType);
ScanTypeFlags ScanType(ParseTypeMode mode, GreenNode *&lastTokenOfType);
ScanTypeFlags ScanTupleType(GreenNode *&lastTokenOfType);
ScanTypeFlags ScanFunctionPointerType(GreenNode *&lastTokenOfType);
static bool IsPredefinedType(SyntaxKind keyword);
GreenNode *ParseTypeName();
GreenNode *ParseTypeOrVoid();
GreenNode *ParseType(ParseTypeMode mode = ParseTypeMode::Normal);
GreenNode *ParseTypeCore(ParseTypeMode mode);
GreenNode *TryEatNullableQualifierIfApplicable(GreenNode *typeParsedSoFar, ParseTypeMode mode);
bool PointerTypeModsFollowedByRankAndDimensionSpecifier();
GreenNode *ParseArrayRankSpecifier(bool &sawNonOmittedSize);
GreenNode *ParseTupleType();
GreenNode *ParseTupleElement();
PostSkipAction SkipBadArrayRankSpecifierTokens(GreenNode *&openBracket, SeparatedSyntaxListBuilder list,
                                               SyntaxKind expected);
GreenNode *ParseUnderlyingType(ParseTypeMode mode, NameOptions options = NameOptions::None);
GreenNode *ParseFunctionPointerTypeSyntax();
bool IsFunctionPointerStart();
static bool IsPossibleFunctionPointerParameterListStart(GreenNode *token);
GreenNode *ParsePointerTypeMods(GreenNode *type);
