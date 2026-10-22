// Members of class Lexer defined in csharp_lexer_xml.cpp (Lexer.cs from LexXmlDocComment to the end: XML doc-comment
// and cref lexing, unicode escapes) and csharp_lexer_string.cpp (Lexer_StringLiteral.cs, Lexer_RawStringLiteral.cs,
// Lexer.Interpolation.cs). Included inside class Lexer (csharp_parser_internal.h).

    // --- Lexer.cs: XML doc comments (csharp_lexer_xml.cpp) ---
    GreenNode *LexXmlDocComment(XmlDocCommentStyle style);
    GreenNode *LexXmlToken();
    bool ScanXmlToken(TokenInfo &info);
    void ScanXmlTextLiteralNewLineToken(TokenInfo &info);
    void ScanXmlTagStart(TokenInfo &info);
    void ScanXmlEntity(TokenInfo &info);
    static bool MatchesProductionForXmlChar(uint32_t charValue);
    void ScanXmlText(TokenInfo &info);
    GreenNode *LexXmlElementTagToken();
    bool ScanXmlElementTagToken(TokenInfo &info);
    void ScanXmlName(TokenInfo &info);
    static bool IsXmlNameStartChar(char16_t ch);
    static bool IsXmlNameChar(char16_t ch);
    GreenNode *LexXmlAttributeTextToken();
    bool ScanXmlAttributeTextToken(TokenInfo &info);
    void ScanXmlAttributeText(TokenInfo &info);
    GreenNode *LexXmlCharacter();
    bool ScanXmlCharacter(TokenInfo &info);
    GreenNode *LexXmlCrefOrNameToken();
    bool ScanXmlCrefToken(TokenInfo &info);
    bool AdvanceIfMatches(char16_t ch);
    bool InXmlCrefOrNameAttributeValue() const;
    bool InXmlNameAttributeValue() const;
    void AddCrefError(ErrorCode code, std::initializer_list<Text> args = {});
    void AddCrefError(const std::optional<SyntaxDiagnosticInfo> &info);
    void AddCrefError(const SyntaxDiagnosticInfo &info) { AddCrefError(std::optional<SyntaxDiagnosticInfo>(info)); }
    GreenNode *LexXmlCDataSectionTextToken();
    bool ScanXmlCDataSectionTextToken(TokenInfo &info);
    void ScanXmlCDataSectionText(TokenInfo &info);
    GreenNode *LexXmlCommentTextToken();
    bool ScanXmlCommentTextToken(TokenInfo &info);
    void ScanXmlCommentText(TokenInfo &info);
    GreenNode *LexXmlProcessingInstructionTextToken();
    bool ScanXmlProcessingInstructionTextToken(TokenInfo &info);
    void ScanXmlProcessingInstructionText(TokenInfo &info);
    void LexXmlDocCommentLeadingTrivia(SyntaxListBuilder &trivia);
    void LexXmlDocCommentLeadingTriviaWithWhitespace(SyntaxListBuilder &trivia);
    void LexXmlWhitespaceAndNewLineTrivia(SyntaxListBuilder &trivia);
    bool IsUnicodeEscape();
    char16_t PeekCharOrUnicodeEscape(char16_t &surrogateCharacter);
    char16_t PeekUnicodeEscape(char16_t &surrogateCharacter);
    char16_t NextCharOrUnicodeEscape(char16_t &surrogateCharacter, std::optional<SyntaxDiagnosticInfo> &info);
    char16_t NextUnicodeEscape(char16_t &surrogateCharacter, std::optional<SyntaxDiagnosticInfo> &info);
    char16_t ScanUnicodeEscape(bool peek, char16_t &surrogateCharacter, std::optional<SyntaxDiagnosticInfo> &info);
    // TryScanXmlEntity is public (csharp_parser_internal.h).
    SyntaxDiagnosticInfo CreateIllegalEscapeDiagnostic(int start) const;
    static char16_t GetCharsFromUtf32(uint32_t codepoint, char16_t &lowSurrogate);
    /** @brief AbstractLexer.AddError(XmlParseErrorCode, args): see csharp_lexer_xml.cpp for the representation. */
    void AddXmlError(int xmlParseErrorCode, std::initializer_list<Text> args = {});

    // --- Lexer_StringLiteral.cs (csharp_lexer_string.cpp) ---
    void ScanStringLiteral(TokenInfo &info, bool inDirective);
    bool ScanUtf8Suffix();
    char16_t ScanEscapeSequence(char16_t &surrogateCharacter);
    void ScanVerbatimStringLiteral(TokenInfo &info);
    void ScanInterpolatedStringLiteral(TokenInfo &info);
    // ScanInterpolatedOrRawStringLiteralTop is public (csharp_parser_internal.h).
    /** @brief Turn a (parsed) interpolated string nonterminal into an interpolated string token (Blender only). */
    static GreenNode *RescanInterpolatedString(GreenNode *interpolatedString);

    /** @brief Lexer_StringLiteral.cs's InterpolatedOrRawStringScanner (a ref struct living on the stack). */
    class InterpolatedOrRawStringScanner {
    public:
        InterpolatedOrRawStringScanner(Lexer *lexer, bool isInterpolatedString)
            : _lexer(lexer), _isInterpolatedString(isInterpolatedString) {}
        InterpolatedOrRawStringScanner(const InterpolatedOrRawStringScanner &) = delete;
        InterpolatedOrRawStringScanner &operator=(const InterpolatedOrRawStringScanner &) = delete;

        /** @brief The first error encountered while scanning (null if none). */
        std::optional<SyntaxDiagnosticInfo> Error;

        void ScanStringLiteralTop(InterpolatedStringKind &kind, TextRange &openQuoteRange,
                                  std::vector<Interpolation> *interpolations, TextRange &closeQuoteRange);

    private:
        Lexer *const _lexer;
        const bool _isInterpolatedString;

        bool IsAtEnd(InterpolatedStringKind kind);
        bool IsAtEnd(bool allowNewline);
        void TrySetError(const SyntaxDiagnosticInfo &error);
        bool ScanOpenQuote(InterpolatedStringKind &kind, int &startingDollarSignCount, int &startingQuoteCount);
        void ScanInterpolatedStringLiteralEnd(InterpolatedStringKind kind, int startingQuoteCount,
                                              TextRange &closeQuoteRange);
        void ScanNormalOrVerbatimInterpolatedStringLiteralEnd(InterpolatedStringKind kind);
        void ScanRawInterpolatedStringLiteralEnd(InterpolatedStringKind kind, int startingQuoteCount);
        void ScanInterpolatedStringLiteralContents(InterpolatedStringKind kind, int startingDollarSignCount,
                                                   int startingQuoteCount, std::vector<Interpolation> *interpolations);
        bool CheckForIllegalEmptyMultiLineRawStringLiteral(InterpolatedStringKind kind, int startingQuoteCount);
        bool IsAtEndOfMultiLineRawLiteral(InterpolatedStringKind kind, int startingQuoteCount);
        bool IsEndDelimiterOtherwiseConsume(InterpolatedStringKind kind, int startingQuoteCount);
        void HandleCloseBraceInContent(InterpolatedStringKind kind, int startingDollarSignCount);
        void HandleOpenBraceInContent(InterpolatedStringKind kind, int startingDollarSignCount,
                                      std::vector<Interpolation> *interpolations);
        void HandleOpenBraceInNormalOrVerbatimContent(InterpolatedStringKind kind,
                                                      std::vector<Interpolation> *interpolations);
        void HandleOpenBraceInRawContent(InterpolatedStringKind kind, int startingDollarSignCount,
                                         std::vector<Interpolation> *interpolations);
        void ScanFormatSpecifier(InterpolatedStringKind kind);
        void ScanInterpolatedStringLiteralHoleBalancedText(InterpolatedStringKind kind, char16_t endingChar, bool isHole,
                                                           TextRange &colonRange);
        bool RecoveringFromRunawayLexing() const { return Error.has_value(); }
        void ScanInterpolatedStringLiteralNestedString();
        void ScanInterpolatedStringLiteralHoleBracketed(InterpolatedStringKind kind, char16_t start, char16_t end);
    };
    friend class InterpolatedOrRawStringScanner;

    // --- Lexer_RawStringLiteral.cs (csharp_lexer_string.cpp) ---
    int ConsumeCharSequence(char16_t ch);
    int ConsumeQuoteSequence() { return ConsumeCharSequence(u'"'); }
    int ConsumeDollarSignSequence() { return ConsumeCharSequence(u'$'); }
    int ConsumeAtSignSequence() { return ConsumeCharSequence(u'@'); }
    int ConsumeOpenBraceSequence() { return ConsumeCharSequence(u'{'); }
    int ConsumeCloseBraceSequence() { return ConsumeCharSequence(u'}'); }
    void ConsumeWhitespace();
    bool IsAtEndOfText(char16_t currentChar) const {
        return currentChar == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd();
    }
    void ScanRawStringLiteral(TokenInfo &info, bool inDirective);
    void ScanSingleLineRawStringLiteral(TokenInfo &info, int startingQuoteCount);
    void ScanMultiLineRawStringLiteral(TokenInfo &info, int startingQuoteCount);
