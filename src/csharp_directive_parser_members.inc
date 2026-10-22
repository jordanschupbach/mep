// DirectiveParser's members (DirectiveParser.cs), defined in csharp_directive_parser.cpp.
// Included inside `class DirectiveParser` (csharp_parser_internal.h).
private:
    static constexpr int MAX_DIRECTIVE_IDENTIFIER_WIDTH = 128;
    static constexpr int MaxLineValue = 0xfeefed;
    static constexpr int MaxCharacterValue = 0x10000;

    GreenNode *ParseIfDirective(GreenNode *hash, GreenNode *keyword, bool isActive);
    GreenNode *ParseElifDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool endIsActive);
    GreenNode *ParseElseDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool endIsActive);
    GreenNode *ParseEndIfDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool endIsActive);
    GreenNode *ParseRegionDirective(GreenNode *hash, GreenNode *keyword, bool isActive);
    GreenNode *ParseEndRegionDirective(GreenNode *hash, GreenNode *keyword, bool isActive);
    GreenNode *ParseDefineOrUndefDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool isFollowingToken);
    GreenNode *ParseErrorOrWarningDirective(GreenNode *hash, GreenNode *keyword, bool isActive);
    GreenNode *ParseLineDirective(GreenNode *hash, GreenNode *id, bool isActive);
    GreenNode *ParseLineSpanDirective(GreenNode *hash, GreenNode *lineKeyword, bool isActive);
    GreenNode *ParseLineDirectivePosition(bool &reportError, int &line, int &character);
    GreenNode *ParseLineDirectiveNumericLiteral(bool &reportError, int minValue, int maxValue, int &value);
    GreenNode *ParseReferenceDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool isFollowingToken);
    GreenNode *ParseLoadDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool isFollowingToken);
    GreenNode *ParseNullableDirective(GreenNode *hash, GreenNode *token, bool isActive);
    GreenNode *ParsePragmaDirective(GreenNode *hash, GreenNode *pragma, bool isActive);
    GreenNode *ParseShebangDirective(GreenNode *hash, GreenNode *exclamation, bool isActive);
    GreenNode *ParseIgnoredDirective(GreenNode *hash, GreenNode *colon, bool isActive, bool isFollowingToken);
    GreenNode *ParseEndOfDirectiveWithOptionalPreprocessingMessage();
    GreenNode *ParseEndOfDirective(bool ignoreErrors, bool afterPragma = false, bool afterLineNumber = false);
    GreenNode *ParseExpression();
    GreenNode *ParseLogicalOr();
    GreenNode *ParseLogicalAnd();
    GreenNode *ParseEquality();
    GreenNode *ParseLogicalNot();
    GreenNode *ParsePrimary();
    static GreenNode *TruncateIdentifier(GreenNode *identifier);
    bool EvaluateBool(GreenNode *expr);
    // Evaluate returns `object` in C#, but every value it can produce is a bool (bool literals' Token.Value,
    // IsDefined, comparisons), so `object.Equals` of two results is bool equality.
    bool Evaluate(GreenNode *expr);
    bool IsDefined(Text id);

public:
