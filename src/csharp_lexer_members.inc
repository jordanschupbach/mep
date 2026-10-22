// Members of class Lexer defined in csharp_lexer.cpp: Lexer.cs from the top through LexDirectiveTrivia (everything
// before LexXmlDocComment), AbstractLexer.GetErrors, and LexerCache's keyword lookup. Included inside class Lexer.

    // LexerCache (only its keyword lookup matters; the token / trivia caches only share identical nodes).
    struct LexerCache {
        static constexpr int MaxKeywordLength = 10;
        /** @brief LexerCache.TryGetKeywordKind: a keyword's or contextual keyword's kind by its text. */
        static bool TryGetKeywordKind(Text key, SyntaxKind &kind);
    };
    LexerCache _cache;

    // Lexer.cs fields not in the shared part of the class.
    int _identLen = 0;
    // The ValueText of the last decimal literal GetValueDecimal parsed (decimal.ToString() of its value: the
    // exact digits and scale), for Create; TokenInfo only carries an approximate double.
    Text _decimalValueText;

    // AbstractLexer.GetErrors: an empty vector stands for null (AddError never leaves an empty list).
    std::vector<SyntaxDiagnosticInfo> GetErrors() const { return _errors; }

    GreenNode *LexSyntaxToken();
    GreenNode *Create(const TokenInfo &info, GreenNode *leading, GreenNode *trailing,
                      const std::vector<SyntaxDiagnosticInfo> &errors);
    void ScanSyntaxToken(TokenInfo &info);
    bool TryScanAtStringToken(TokenInfo &info);
    bool TryScanInterpolatedString(TokenInfo &info);
    bool ScanInteger();
    void ScanNumericLiteralSingleInteger(bool &underscoreInWrongPlace, bool &usedUnderscore,
                                         bool &firstCharWasUnderscore, bool isHex, bool isBinary);
    bool ScanNumericLiteral(TokenInfo &info);
    static bool TryParseBinaryUInt64(const std::u16string &text, uint64_t &value);
    int32_t GetValueInt32(Text text, bool isHex);
    uint64_t GetValueUInt64(Text text, bool isHex, bool isBinary);
    double GetValueDouble(const std::u16string &text);
    float GetValueSingle(const std::u16string &text);
    double GetValueDecimal(const std::u16string &text, int start, int end);
    void ResetIdentBuffer();
    void AddIdentChar(char16_t ch);
    void GrowIdentBuffer();
    bool ScanIdentifier(TokenInfo &info);
    bool ScanIdentifier_FastPath(TokenInfo &info);
    bool ScanIdentifier_SlowPath(TokenInfo &info);
    bool ScanIdentifier_CrefSlowPath(TokenInfo &info);
    bool ScanIdentifierOrKeyword(TokenInfo &info);
    void LexSyntaxTrivia(bool isFollowingToken, bool isTrailing, SyntaxListBuilder &triviaList);
    static constexpr int s_conflictMarkerLength = 7;  // "<<<<<<<".Length
    bool IsConflictMarkerTrivia();
    void LexConflictMarkerTrivia(SyntaxListBuilder &triviaList);
    SyntaxListBuilder LexConflictMarkerDisabledText(bool atSecondMiddleMarker, SyntaxListBuilder &triviaList);
    void LexConflictMarkerEndOfLine(SyntaxListBuilder &triviaList);
    void LexConflictMarkerHeader(SyntaxListBuilder &triviaList);
    /** @brief `ref SyntaxListBuilder? list`: a null builder is created on first use. */
    void AddTrivia(GreenNode *trivia, SyntaxListBuilder &list);
    void ScanMultiLineComment(bool &isTerminated, char16_t delimiter);
    void ScanToEndOfLine();
    GreenNode *ScanEndOfLine();
    GreenNode *ScanWhitespace();
    void LexDirectiveAndExcludedTrivia(bool isFollowingToken, SyntaxListBuilder &triviaList);
    void LexExcludedDirectivesAndTrivia(bool endIsActive, SyntaxListBuilder &triviaList);
    GreenNode *LexSingleDirective(bool isActive, bool endIsActive, bool isFollowingToken,
                                  SyntaxListBuilder &triviaList);
    GreenNode *ParseDirective(bool isActive, bool endIsActive, bool isFollowingToken);
    GreenNode *LexDisabledText(bool &followedByDirective);
    GreenNode *LexDirectiveToken();
    /** @brief The message's text (a slice of the source: the builder copies it char by char), or none. */
    std::optional<Text> LexOptionalPreprocessingMessage();
    GreenNode *LexEndOfDirectiveAfterOptionalPreprocessingMessage(GreenNode *leading);
    bool ScanDirectiveToken(TokenInfo &info);
    void LexDirectiveTrailingTrivia(bool includeEndOfLine, SyntaxListBuilder &trivia);
    GreenNode *LexDirectiveTrivia();
