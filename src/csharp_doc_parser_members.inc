// DocumentationCommentParser's members (DocumentationCommentParser.cs, DocumentationCommentXmlTokens.cs),
// defined in csharp_doc_parser.cpp. Included inside `class DocumentationCommentParser`
// (csharp_parser_internal.h).

    void ParseRemainder(SyntaxListBuilder nodes);

    /**
     * @brief DocumentationCommentXmlTokens (DocumentationCommentXmlTokens.cs): the shared well-known XML tag and
     *        attribute-name tokens the lexer hands out in XmlElementTag mode (Lexer.LexXmlElementTagToken).
     *        `leading` is the lexer's leading-trivia builder (a null builder for C# null).
     */
    struct DocumentationCommentXmlTokens {
        static GreenNode *LookupToken(Text text, const SyntaxListBuilder &leading);
    };

    /** @brief Roslyn's XmlParseErrorCode (Errors/XmlParseErrorCode.cs). */
    enum class XmlParseErrorCode {
        XML_RefUndefinedEntity_1,
        XML_InvalidCharEntity,
        XML_InvalidUnicodeChar,
        XML_InvalidWhitespace,
        XML_MissingEqualsAttribute,
        XML_StringLiteralNoStartQuote,
        XML_StringLiteralNoEndQuote,
        XML_StringLiteralNonAsciiQuote,
        XML_LessThanInAttributeValue,
        XML_IncorrectComment,
        XML_ElementTypeMatch,
        XML_DuplicateAttribute,
        XML_WhitespaceMissing,
        XML_EndTagNotExpected,
        XML_CDataEndTagNotAllowed,
        XML_ExpectedIdentifier,
        XML_ExpectedEndOfTag,
        XML_InvalidToken,
        XML_ExpectedEndOfXml,
        XML_EndTagExpected,
    };

protected:
    SyntaxDiagnosticInfo GetExpectedTokenError(SyntaxKind expected, SyntaxKind actual, int offset, int width) override;
    SyntaxDiagnosticInfo GetExpectedMissingNodeOrTokenError(GreenNode *missingNodeOrToken, SyntaxKind expected,
                                                           SyntaxKind actual) override;
    GreenNode *WithAdditionalDiagnostics(GreenNode *node, const SyntaxDiagnosticInfo &diagnostic) override;

private:
    enum class SkipResult { Continue, Abort };
    using ParserPredicate = bool (*)(DocumentationCommentParser *);

    // The C# `private LexerMode SetMode(LexerMode mode)` (sets the mode keeping the doc-comment location and
    // style bits, returns the old mode). Renamed: the base's C# `Mode` setter is SyntaxParser::SetMode here.
    LexerMode SwapMode(LexerMode mode);
    void ResetMode(LexerMode mode);

    void ParseXmlNodes(SyntaxListBuilder nodes);
    GreenNode *ParseXmlNode();
    bool IsXmlNodeStartOrStop();
    GreenNode *ParseXmlText();
    GreenNode *ParseXmlElement();
    static bool MatchingXmlNames(GreenNode *name, GreenNode *endName);
    void ParseXmlAttributes(GreenNode *&elementName, SyntaxListBuilder attrs);
    SkipResult SkipBadTokens(GreenNode *&startNode, SyntaxListBuilder list, ParserPredicate isNotExpectedFunction,
                             ParserPredicate abortFunction, XmlParseErrorCode error);
    GreenNode *ParseXmlAttribute(GreenNode *elementName);
    static bool XmlElementSupportsNameAttribute(GreenNode *elementName);
    bool IsVerbatimCref();
    void ParseCrefAttribute(GreenNode *&startQuote, GreenNode *&cref, GreenNode *&endQuote);
    void ParseNameAttribute(GreenNode *&startQuote, GreenNode *&identifier, GreenNode *&endQuote);
    void ParseXmlAttributeText(GreenNode *&startQuote, SyntaxListBuilder textTokens, GreenNode *&endQuote);
    GreenNode *ParseXmlAttributeStartQuote();
    GreenNode *ParseXmlAttributeEndQuote(SyntaxKind quoteKind);
    GreenNode *SkipNonAsciiQuotationMark();
    static bool IsNonAsciiQuotationMark(GreenNode *token);
    GreenNode *ParseXmlName();
    GreenNode *ParseXmlComment();
    GreenNode *ParseXmlCDataSection();
    GreenNode *ParseXmlProcessingInstruction();
    GreenNode *WithXmlParseError(GreenNode *node, XmlParseErrorCode code, std::initializer_list<Text> args = {});

    // Cref.
    GreenNode *ParseCrefAttributeValue();
    GreenNode *ParseMemberCref();
    GreenNode *ParseNameMemberCref();
    GreenNode *ParseIndexerMemberCref();
    GreenNode *ParsePossibleExtensionMemberCref();
    GreenNode *ParseOperatorMemberCref();
    GreenNode *TryEatCheckedKeyword(bool isConversion, GreenNode *&operatorKeyword);
    GreenNode *ParseConversionOperatorMemberCref();
    GreenNode *ParseCrefParameterList();
    GreenNode *ParseBracketedCrefParameterList();
    GreenNode *ParseBaseCrefParameterList(bool useSquareBrackets);
    bool IsPossibleCrefParameter();
    GreenNode *ParseCrefParameter();
    GreenNode *ParseCrefName(bool typeArgumentsMustBeIdentifiers);
    GreenNode *ParseTypeArguments(bool typeArgumentsMustBeIdentifiers);
    GreenNode *ParseCrefType(bool typeArgumentsMustBeIdentifiers, bool checkForMember = false);
    GreenNode *ParseCrefTypeHelper(bool typeArgumentsMustBeIdentifiers, bool checkForMember = false);
    GreenNode *ParseCrefTypeSuffix(GreenNode *type);
    bool IsEndOfCrefAttribute();
    bool InCref() const;

    // Name attribute values.
    GreenNode *ParseNameAttributeValue();
    bool IsEndOfNameAttribute();

    // assuming this is not used concurrently (C# HashSet<string>; attribute lists are short)
    std::vector<std::u16string> _attributesSeen;

public:
