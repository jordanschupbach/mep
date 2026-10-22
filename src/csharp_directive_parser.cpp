// DirectiveParser.cs (Roslyn, MIT, (c) .NET Foundation and Contributors): the preprocessor directive parser.
// Method for method, in the C# file's order. The options are fixed (csharp_parser_internal.h): Regular source
// (so #r / #load always report "only allowed in scripts"), no FileBasedProgram feature (#! and #: always
// report), LanguageVersion.Preview (#nullable / #pragma feature checks pass), no predefined symbols.

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief GreenNode.WriteTo(writer, leading, trailing): the text of a node, optionally without the first
 *         token's leading / the last token's trailing trivia. */
void WriteTo(std::u16string &out, const GreenNode *node, bool leading, bool trailing) {
    if (node == nullptr) return;
    if (node->IsTrivia()) {
        out.append(node->text);
        return;
    }
    if (node->IsToken()) {
        if (leading && node->leading) WriteTo(out, node->leading, true, true);
        out.append(node->text);
        if (trailing && node->trailing) WriteTo(out, node->trailing, true, true);
        return;
    }
    int firstIndex = -1, lastIndex = -1;
    for (int i = 0; i < node->SlotCount(); i++) {
        if (node->GetSlot(i) != nullptr) {
            if (firstIndex < 0) firstIndex = i;
            lastIndex = i;
        }
    }
    for (int i = firstIndex; i >= 0 && i <= lastIndex; i++) {
        GreenNode *child = node->GetSlot(i);
        if (child != nullptr) {
            WriteTo(out, child, leading || i != firstIndex, trailing || i != lastIndex);
        }
    }
}

std::u16string ToFullString(const GreenNode *node) {
    std::u16string s;
    WriteTo(s, node, true, true);
    return s;
}

/** @brief SyntaxList.Concat(left, right) (the internal green-list concatenation). */
GreenNode *Concat(GreenNode *left, GreenNode *right) {
    if (left == nullptr) return right;
    if (right == nullptr) return left;
    SyntaxListBuilder b = SyntaxListBuilder::Create();
    b.Add(left);   // a list is added item by item
    b.Add(right);
    return b.ToListNode();
}

/** @brief LanguageParser.NoTriviaBetween. */
bool NoTriviaBetweenTokens(const GreenNode *token1, const GreenNode *token2) {
    return token1->GetTrailingTriviaWidth() == 0 && token2->GetLeadingTriviaWidth() == 0;
}

/** @brief `(int)token.Value` of a directive numeric literal (always lexed as System.Int32 in directives). */
int Int32Value(const GreenNode *token) {
    if (token->value == nullptr) return 0;
    return static_cast<int32_t>(static_cast<uint32_t>(token->value->bits));
}

/** @brief .NET char.IsWhiteSpace. */
bool IsDotNetWhiteSpace(char16_t c) {
    if (c == u' ' || (c >= u'\t' && c <= u'\r') || c == u'\x85' || c == u'\xA0') return true;
    if (c < 0x100) return false;
    int cat = GetUnicodeCategory(c);
    return cat == 11 || cat == 12 || cat == 13;  // SpaceSeparator, LineSeparator, ParagraphSeparator
}

bool EqualsAsciiIgnoreCase(Text s, const char16_t *lower) {
    size_t n = std::char_traits<char16_t>::length(lower);
    if (s.size() != n) return false;
    for (size_t i = 0; i < n; i++) {
        char16_t c = s[i];
        if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
        if (c != lower[i]) return false;
    }
    return true;
}

/** @brief .NET bool.TryParse: "true"/"false" in any ASCII case, ignoring surrounding whitespace and NULs. */
bool BoolTryParse(Text value, bool &result) {
    size_t b = 0, e = value.size();
    while (b < e && (IsDotNetWhiteSpace(value[b]) || value[b] == u'\0')) b++;
    while (e > b && (IsDotNetWhiteSpace(value[e - 1]) || value[e - 1] == u'\0')) e--;
    Text v = value.substr(b, e - b);
    if (EqualsAsciiIgnoreCase(v, u"true")) {
        result = true;
        return true;
    }
    if (EqualsAsciiIgnoreCase(v, u"false")) {
        result = false;
        return true;
    }
    result = false;
    return false;
}

bool IsHex(char16_t c) { return SyntaxFacts::IsHexDigit(c); }

bool AllHex(Text s) {
    for (char16_t c : s) {
        if (!IsHex(c)) return false;
    }
    return true;
}

/** @brief Guid's compat TryParseHex: optional '+', optional "0x"/"0X", then hex digits; the significant digits
 *         (after leading zeros) must fit `maxDigits`. */
bool GuidCompatHex(Text s, int maxDigits) {
    if (!s.empty() && s[0] == u'+') s = s.substr(1);
    if (s.size() > 1 && s[0] == u'0' && (s[1] | 0x20) == u'x') s = s.substr(2);
    size_t i = 0;
    while (i < s.size() && s[i] == u'0') i++;
    int digits = 0;
    for (; i < s.size(); i++) {
        if (!IsHex(s[i])) return false;
        digits++;
    }
    return digits <= maxDigits;
}

/** @brief Guid "D" format (8-4-4-4-12), with .NET's compat fallback for "+" / "0x" component prefixes. */
bool GuidTryParseExactD(Text g) {
    if (g.size() != 36 || g[8] != u'-' || g[13] != u'-' || g[18] != u'-' || g[23] != u'-') return false;
    if (AllHex(g.substr(0, 8)) && AllHex(g.substr(9, 4)) && AllHex(g.substr(14, 4)) && AllHex(g.substr(19, 4)) &&
        AllHex(g.substr(24, 12))) {
        return true;
    }
    if (g.find_first_of(u"Xx+") == Text::npos) return false;
    return GuidCompatHex(g.substr(0, 8), 8) && GuidCompatHex(g.substr(9, 4), 4) &&
           GuidCompatHex(g.substr(14, 4), 4) && GuidCompatHex(g.substr(19, 4), 4) && AllHex(g.substr(24, 12));
}

/** @brief Guid "X" format: {0xa,0xb,0xc,{0xd,0xe,0xf,0xg,0xh,0xi,0xj,0xk}} (whitespace anywhere ignored). */
bool GuidTryParseExactX(Text input) {
    std::u16string g;
    for (char16_t c : input) {
        if (!IsDotNetWhiteSpace(c)) g.push_back(c);
    }
    size_t p = 0;
    auto eat = [&](char16_t c) {
        if (p < g.size() && g[p] == c) {
            p++;
            return true;
        }
        return false;
    };
    auto hexComponent = [&](int maxDigits) {
        if (!(p + 1 < g.size() && g[p] == u'0' && (g[p + 1] | 0x20) == u'x')) return false;
        p += 2;
        size_t start = p;
        while (p < g.size() && IsHex(g[p])) p++;
        if (p == start) return false;
        size_t q = start;
        while (q < p && g[q] == u'0') q++;
        return static_cast<int>(p - q) <= maxDigits;
    };
    if (!eat(u'{') || !hexComponent(8) || !eat(u',') || !hexComponent(4) || !eat(u',') || !hexComponent(4) ||
        !eat(u',') || !eat(u'{')) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        if (i > 0 && !eat(u',')) return false;
        if (!hexComponent(2)) return false;
    }
    return eat(u'}') && eat(u'}') && p == g.size();
}

/** @brief .NET Guid.TryParse (formats N, D, B, P, X). */
bool GuidTryParse(Text input) {
    size_t b = 0, e = input.size();
    while (b < e && IsDotNetWhiteSpace(input[b])) b++;
    while (e > b && IsDotNetWhiteSpace(input[e - 1])) e--;
    Text g = input.substr(b, e - b);
    if (g.empty()) return false;
    switch (g[0]) {
        case u'(':
            return g.size() == 38 && g[37] == u')' && GuidTryParseExactD(g.substr(1, 36));
        case u'{':
            if (g.find(u',') != Text::npos) return GuidTryParseExactX(g);
            return g.size() == 38 && g[37] == u'}' && GuidTryParseExactD(g.substr(1, 36));
        default:
            if (g.find(u'-') != Text::npos) return GuidTryParseExactD(g);
            return g.size() == 32 && AllHex(g);
    }
}

GreenNode *WithoutDiagnosticsGreen(GreenNode *node) { return WithDiagnosticsGreen(node, nullptr, 0); }

}  // namespace

DirectiveParser::DirectiveParser(Lexer *lexer_) : SyntaxParser(lexer_, LexerMode::Directive, /*allowModeReset*/ false) {}

void DirectiveParser::ReInitialize(DirectiveStack context) {
    SyntaxParser::ReInitialize();
    _context = std::move(context);
}

GreenNode *DirectiveParser::ParseDirective(bool isActive, bool endIsActive, bool isFollowingToken) {
    int hashPosition = lexer->TextWindow.Position();
    GreenNode *hash = this->EatToken(SyntaxKind::HashToken, false);

    // The behavior of these directives when isActive is false is somewhat complicated.
    // The key functions in the native compiler are ScanPreprocessorIfSection and
    // ScanAndIgnoreDirective in CSourceData::CPreprocessor.
    // Key points:
    //   1) #error, #warning, #line, and #pragma have no effect and produce no diagnostics.
    //   2) #if, #else, #elif, #endif, #region, and #endregion must still nest correctly.
    //   3) #define and #undef produce diagnostics but have no effect.
    // #reference, #load and #! are new, but they do not require nesting behavior, so we'll
    // ignore their diagnostics (as in (1) above).

    GreenNode *result;
    SyntaxKind contextualKind = this->CurrentToken()->contextual_kind;
    switch (contextualKind) {
        case SyntaxKind::IfKeyword:
            result = this->ParseIfDirective(hash, this->EatContextualToken(contextualKind), isActive);
            break;

        case SyntaxKind::ElifKeyword:
            result = this->ParseElifDirective(hash, this->EatContextualToken(contextualKind), isActive, endIsActive);
            break;

        case SyntaxKind::ElseKeyword:
            result = this->ParseElseDirective(hash, this->EatContextualToken(contextualKind), isActive, endIsActive);
            break;

        case SyntaxKind::EndIfKeyword:
            result = this->ParseEndIfDirective(hash, this->EatContextualToken(contextualKind), isActive, endIsActive);
            break;

        case SyntaxKind::RegionKeyword:
            result = this->ParseRegionDirective(hash, this->EatContextualToken(contextualKind), isActive);
            break;

        case SyntaxKind::EndRegionKeyword:
            result = this->ParseEndRegionDirective(hash, this->EatContextualToken(contextualKind), isActive);
            break;

        case SyntaxKind::DefineKeyword:
        case SyntaxKind::UndefKeyword:
            result = this->ParseDefineOrUndefDirective(hash, this->EatContextualToken(contextualKind), isActive,
                                                       isFollowingToken);
            break;

        case SyntaxKind::ErrorKeyword:
        case SyntaxKind::WarningKeyword:
            result = this->ParseErrorOrWarningDirective(hash, this->EatContextualToken(contextualKind), isActive);
            break;

        case SyntaxKind::LineKeyword: {
            GreenNode *lineKeyword = this->EatContextualToken(contextualKind);
            result = (this->CurrentToken()->kind == SyntaxKind::OpenParenToken)
                         ? this->ParseLineSpanDirective(hash, lineKeyword, isActive)
                         : this->ParseLineDirective(hash, lineKeyword, isActive);
            break;
        }

        case SyntaxKind::PragmaKeyword:
            result = this->ParsePragmaDirective(hash, this->EatContextualToken(contextualKind), isActive);
            break;

        case SyntaxKind::ReferenceKeyword:
            result = this->ParseReferenceDirective(hash, this->EatContextualToken(contextualKind), isActive,
                                                   isFollowingToken);
            break;

        case SyntaxKind::LoadKeyword:
            result = this->ParseLoadDirective(hash, this->EatContextualToken(contextualKind), isActive,
                                              isFollowingToken);
            break;

        case SyntaxKind::NullableKeyword:
            result = this->ParseNullableDirective(hash, this->EatContextualToken(contextualKind), isActive);
            break;

        default:
            if (contextualKind == SyntaxKind::ExclamationToken) {
                // Always parse as a shebang directive, but report an error if not at position 0
                if (hashPosition != 0 || hash->GetTrailingTrivia() != nullptr) {
                    hash = this->AddError(hash, ErrorCode::ERR_BadDirectivePlacement);
                }

                result = this->ParseShebangDirective(hash, this->EatToken(SyntaxKind::ExclamationToken), isActive);
            } else if (contextualKind == SyntaxKind::ColonToken && hash->GetTrailingTrivia() == nullptr) {
                result = this->ParseIgnoredDirective(hash, this->EatToken(SyntaxKind::ColonToken), isActive,
                                                     isFollowingToken);
            } else {
                GreenNode *id = this->EatToken(SyntaxKind::IdentifierToken, false);
                GreenNode *end = this->ParseEndOfDirective(/*ignoreErrors*/ true);
                if (!id->IsMissing()) {
                    id = this->AddError(id, ErrorCode::ERR_PPDirectiveExpected);
                } else {
                    hash = this->AddError(hash, ErrorCode::ERR_PPDirectiveExpected);
                }

                result = SyntaxFactory::BadDirectiveTrivia(hash, id, end, isActive);
            }

            break;
    }

    return result;
}

GreenNode *DirectiveParser::ParseIfDirective(GreenNode *hash, GreenNode *keyword, bool isActive) {
    GreenNode *expr = this->ParseExpression();
    GreenNode *eod = this->ParseEndOfDirective(/*ignoreErrors*/ false);
    bool isTrue = this->EvaluateBool(expr);
    bool branchTaken = isActive && isTrue;
    return SyntaxFactory::IfDirectiveTrivia(hash, keyword, expr, eod, isActive, branchTaken, isTrue);
}

GreenNode *DirectiveParser::ParseElifDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool endIsActive) {
    GreenNode *expr = this->ParseExpression();
    GreenNode *eod = this->ParseEndOfDirective(/*ignoreErrors*/ false);
    if (_context.HasPreviousIfOrElif()) {
        bool isTrue = this->EvaluateBool(expr);
        bool branchTaken = endIsActive && isTrue && !_context.PreviousBranchTaken();
        return SyntaxFactory::ElifDirectiveTrivia(hash, keyword, expr, eod, endIsActive, branchTaken, isTrue);
    } else {
        eod = TokenWithLeadingTrivia(
            eod, Concat(SyntaxFactory::DisabledText(CurrentArena()->CopyText(ToFullString(expr))),
                        eod->GetLeadingTrivia()));
        if (_context.HasUnfinishedRegion()) {
            return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                                  ErrorCode::ERR_EndRegionDirectiveExpected);
        } else if (_context.HasUnfinishedIf()) {
            return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                                  ErrorCode::ERR_EndifDirectiveExpected);
        } else {
            return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                                  ErrorCode::ERR_UnexpectedDirective);
        }
    }
}

GreenNode *DirectiveParser::ParseElseDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool endIsActive) {
    GreenNode *eod = this->ParseEndOfDirective(/*ignoreErrors*/ false);
    if (_context.HasPreviousIfOrElif()) {
        bool branchTaken = endIsActive && !_context.PreviousBranchTaken();
        return SyntaxFactory::ElseDirectiveTrivia(hash, keyword, eod, endIsActive, branchTaken);
    } else if (_context.HasUnfinishedRegion()) {
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_EndRegionDirectiveExpected);
    } else if (_context.HasUnfinishedIf()) {
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_EndifDirectiveExpected);
    } else {
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_UnexpectedDirective);
    }
}

GreenNode *DirectiveParser::ParseEndIfDirective(GreenNode *hash, GreenNode *keyword, bool isActive, bool endIsActive) {
    GreenNode *eod = this->ParseEndOfDirective(/*ignoreErrors*/ false);
    if (_context.HasUnfinishedIf()) {
        return SyntaxFactory::EndIfDirectiveTrivia(hash, keyword, eod, endIsActive);
    } else if (_context.HasUnfinishedRegion()) {
        // CONSIDER: dev10 actually pops the region off the directive stack here.
        // See if (tok == PPT_ENDIF) in CSourceData::CPreprocessor::ScanPreprocessorIfSection.
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_EndRegionDirectiveExpected);
    } else {
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_UnexpectedDirective);
    }
}

GreenNode *DirectiveParser::ParseRegionDirective(GreenNode *hash, GreenNode *keyword, bool isActive) {
    return SyntaxFactory::RegionDirectiveTrivia(hash, keyword, this->ParseEndOfDirectiveWithOptionalPreprocessingMessage(),
                                                isActive);
}

GreenNode *DirectiveParser::ParseEndRegionDirective(GreenNode *hash, GreenNode *keyword, bool isActive) {
    GreenNode *eod = this->ParseEndOfDirectiveWithOptionalPreprocessingMessage();
    if (_context.HasUnfinishedRegion()) {
        return SyntaxFactory::EndRegionDirectiveTrivia(hash, keyword, eod, isActive);
    } else if (_context.HasUnfinishedIf()) {
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_EndifDirectiveExpected);
    } else {
        return this->AddError(SyntaxFactory::BadDirectiveTrivia(hash, keyword, eod, isActive),
                              ErrorCode::ERR_UnexpectedDirective);
    }
}

GreenNode *DirectiveParser::ParseDefineOrUndefDirective(GreenNode *hash, GreenNode *keyword, bool isActive,
                                                        bool isFollowingToken) {
    if (isFollowingToken) {
        keyword = this->AddError(keyword, ErrorCode::ERR_PPDefFollowsToken);
    }

    GreenNode *name = this->EatToken(SyntaxKind::IdentifierToken, ErrorCode::ERR_IdentifierExpected);
    name = TruncateIdentifier(name);
    GreenNode *end = this->ParseEndOfDirective(/*ignoreErrors*/ name->IsMissing());
    if (keyword->kind == SyntaxKind::DefineKeyword) {
        return SyntaxFactory::DefineDirectiveTrivia(hash, keyword, name, end, isActive);
    } else {
        return SyntaxFactory::UndefDirectiveTrivia(hash, keyword, name, end, isActive);
    }
}

/// An error/warning directive tells the compiler to indicate a syntactic error/warning
/// at the current location.
///
/// Format: #error Error message string
/// Resulting message: from the first non-whitespace character after the directive
/// keyword until the end of the directive (aka EOD) at the line break or EOF.
/// Resulting span: [first non-whitespace char, EOD)
GreenNode *DirectiveParser::ParseErrorOrWarningDirective(GreenNode *hash, GreenNode *keyword, bool isActive) {
    GreenNode *eod = this->ParseEndOfDirectiveWithOptionalPreprocessingMessage();
    bool isError = keyword->kind == SyntaxKind::ErrorKeyword;
    if (isActive) {
        std::u16string triviaBuilder;
        int triviaWidth = 0;

        // whitespace and single line comments are trailing trivia on the keyword, the rest
        // of the error message is leading trivia on the eod.
        //
        bool skipping = true;
        SyntaxList keywordTrailing(keyword->GetTrailingTrivia());
        for (int i = 0; i < keywordTrailing.Count(); i++) {
            GreenNode *t = keywordTrailing[i];
            if (skipping) {
                if (t->kind == SyntaxKind::WhitespaceTrivia) {
                    continue;
                }

                skipping = false;
            }

            WriteTo(triviaBuilder, t, true, true);
            triviaWidth += t->full_width;
        }

        SyntaxList eodLeading(eod->GetLeadingTrivia());
        for (int i = 0; i < eodLeading.Count(); i++) {
            GreenNode *node = eodLeading[i];
            WriteTo(triviaBuilder, node, true, true);
            triviaWidth += node->full_width;
        }

        // Relative to Start (not FullStart) of eod. Can be negative if part of the error text comes from the
        // trailing trivia of the keyword token.
        int triviaOffset = -triviaWidth;

        const std::u16string &errorText = triviaBuilder;
        eod = this->AddError(eod, triviaOffset, triviaWidth,
                             isError ? ErrorCode::ERR_ErrorDirective : ErrorCode::WRN_WarningDirective,
                             {ArgText(errorText)});

        if (isError) {
            if (errorText == u"version") {
                // DEVIATION: Roslyn reports its own product version and assembly path here
                // (CommonCompiler.GetProductVersion / GetAssemblyLocation). We report the version of the compiler
                // this port follows and no path. SpecifiedLanguageVersion == effective == Preview -> "preview".
                eod = this->AddError(eod, triviaOffset, triviaWidth, ErrorCode::ERR_CompilerAndLanguageVersion,
                                     {u"5.6.0-2.26329.109 (35b593be)", u"preview", u""});
            } else {
                // `this.Options.LanguageVersion != LanguageVersion.Preview && "version:..."`: never (Preview).
            }
        }
    }

    if (isError) {
        return SyntaxFactory::ErrorDirectiveTrivia(hash, keyword, eod, isActive);
    } else {
        return SyntaxFactory::WarningDirectiveTrivia(hash, keyword, eod, isActive);
    }
}

GreenNode *DirectiveParser::ParseLineDirective(GreenNode *hash, GreenNode *id, bool isActive) {
    GreenNode *line;
    GreenNode *file = nullptr;
    bool sawLineButNotFile = false;
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::DefaultKeyword:
        case SyntaxKind::HiddenKeyword:
            line = this->EatToken();
            break;
        default:
            line = this->EatToken(SyntaxKind::NumericLiteralToken, ErrorCode::ERR_InvalidLineNumber,
                                  /*reportError*/ isActive);
            sawLineButNotFile = true;  // assume this is the case until we (potentially) see the file name below
            if (isActive && !line->IsMissing() && line->kind == SyntaxKind::NumericLiteralToken) {
                if (Int32Value(line) < 1) {
                    line = this->AddError(line, ErrorCode::ERR_InvalidLineNumber);
                } else if (Int32Value(line) > MaxLineValue) {
                    line = this->AddError(line, ErrorCode::WRN_TooManyLinesForDebugger);
                }
            }

            if (this->CurrentToken()->kind == SyntaxKind::StringLiteralToken &&
                (line->IsMissing() || line->GetTrailingTriviaWidth() > 0 ||
                 this->CurrentToken()->GetLeadingTriviaWidth() > 0))  // require separation between line number and file name
            {
                file = this->EatToken();
                sawLineButNotFile = false;
            }

            break;
    }

    GreenNode *end = this->ParseEndOfDirective(/*ignoreErrors*/ line->IsMissing() || !isActive, /*afterPragma*/ false,
                                               /*afterLineNumber*/ sawLineButNotFile);
    return SyntaxFactory::LineDirectiveTrivia(hash, id, line, file, end, isActive);
}

GreenNode *DirectiveParser::ParseLineSpanDirective(GreenNode *hash, GreenNode *lineKeyword, bool isActive) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::OpenParenToken);

    // static bool noTriviaBetween(SyntaxToken token1, SyntaxToken token2)
    auto noTriviaBetween = [](const GreenNode *token1, const GreenNode *token2) {
        return token1 != nullptr && !token1->IsMissing() && token2 != nullptr && !token2->IsMissing() &&
               NoTriviaBetweenTokens(token1, token2);
    };

    bool reportError = isActive;
    int startLine = 0, startCharacter = 0;
    GreenNode *start = ParseLineDirectivePosition(reportError, startLine, startCharacter);
    if (noTriviaBetween(lineKeyword, start->GetFirstToken())) {
        start = this->AddError(start, ErrorCode::ERR_LineSpanDirectiveRequiresSpace);
    }

    GreenNode *minus = EatToken(SyntaxKind::MinusToken, /*reportError*/ reportError);
    if (minus->IsMissing()) reportError = false;

    int endLine = 0, endCharacter = 0;
    GreenNode *end = ParseLineDirectivePosition(reportError, endLine, endCharacter);
    if (reportError && (endLine < startLine || (endLine == startLine && endCharacter < startCharacter))) {
        end = this->AddError(end, ErrorCode::ERR_LineSpanDirectiveEndLessThanStart);
    }

    GreenNode *characterOffset = nullptr;
    if (CurrentToken()->kind == SyntaxKind::NumericLiteralToken) {
        int discard = 0;
        characterOffset = ParseLineDirectiveNumericLiteral(reportError, /*minValue*/ 1, /*maxValue*/ MaxCharacterValue,
                                                           discard);
    }

    if (noTriviaBetween(end->GetLastToken(), characterOffset)) {
        characterOffset = this->AddError(characterOffset, ErrorCode::ERR_LineSpanDirectiveRequiresSpace);
    }

    GreenNode *file = EatToken(SyntaxKind::StringLiteralToken, ErrorCode::ERR_MissingPPFile, /*reportError*/ reportError);
    if (file->IsMissing()) reportError = false;

    if (noTriviaBetween(characterOffset != nullptr ? characterOffset : end->GetLastToken(), file)) {
        file = this->AddError(file, ErrorCode::ERR_LineSpanDirectiveRequiresSpace);
    }

    GreenNode *endOfDirective = this->ParseEndOfDirective(/*ignoreErrors*/ !reportError);
    return SyntaxFactory::LineSpanDirectiveTrivia(hash, lineKeyword, start, minus, end, characterOffset, file,
                                                  endOfDirective, isActive);
}

GreenNode *DirectiveParser::ParseLineDirectivePosition(bool &reportError, int &line, int &character) {
    GreenNode *openParen = EatToken(SyntaxKind::OpenParenToken, reportError);
    if (openParen->IsMissing()) reportError = false;

    GreenNode *lineToken = ParseLineDirectiveNumericLiteral(reportError, /*minValue*/ 1, /*maxValue*/ MaxLineValue, line);

    GreenNode *comma = EatToken(SyntaxKind::CommaToken, reportError);
    if (comma->IsMissing()) reportError = false;

    GreenNode *characterToken =
        ParseLineDirectiveNumericLiteral(reportError, /*minValue*/ 1, /*maxValue*/ MaxCharacterValue, character);

    GreenNode *closeParen = EatToken(SyntaxKind::CloseParenToken, reportError);
    if (closeParen->IsMissing()) reportError = false;

    return SyntaxFactory::LineDirectivePosition(openParen, lineToken, comma, characterToken, closeParen);
}

GreenNode *DirectiveParser::ParseLineDirectiveNumericLiteral(bool &reportError, int minValue, int maxValue, int &value) {
    GreenNode *token = this->EatToken(SyntaxKind::NumericLiteralToken, ErrorCode::ERR_LineSpanDirectiveInvalidValue,
                                      /*reportError*/ reportError);
    value = 0;
    if (token->IsMissing()) {
        reportError = false;
    } else if (token->kind == SyntaxKind::NumericLiteralToken) {
        value = Int32Value(token);
        if (value < minValue || value > maxValue) {
            token = this->AddError(token, ErrorCode::ERR_LineSpanDirectiveInvalidValue);
            reportError = false;
        }
    }
    return token;
}

GreenNode *DirectiveParser::ParseReferenceDirective(GreenNode *hash, GreenNode *keyword, bool isActive,
                                                    bool isFollowingToken) {
    (void)isFollowingToken;  // only consulted for scripts
    if (isActive) {
        // Options.Kind == SourceCodeKind.Regular (always)
        keyword = this->AddError(keyword, ErrorCode::ERR_ReferenceDirectiveOnlyAllowedInScripts);
    }

    GreenNode *file = this->EatToken(SyntaxKind::StringLiteralToken, ErrorCode::ERR_ExpectedPPFile,
                                     /*reportError*/ isActive);

    GreenNode *end = this->ParseEndOfDirective(/*ignoreErrors*/ file->IsMissing() || !isActive);
    return SyntaxFactory::ReferenceDirectiveTrivia(hash, keyword, file, end, isActive);
}

GreenNode *DirectiveParser::ParseLoadDirective(GreenNode *hash, GreenNode *keyword, bool isActive,
                                               bool isFollowingToken) {
    (void)isFollowingToken;  // only consulted for scripts
    if (isActive) {
        // Options.Kind == SourceCodeKind.Regular (always)
        keyword = this->AddError(keyword, ErrorCode::ERR_LoadDirectiveOnlyAllowedInScripts);
    }

    GreenNode *file = this->EatToken(SyntaxKind::StringLiteralToken, ErrorCode::ERR_ExpectedPPFile,
                                     /*reportError*/ isActive);

    GreenNode *end = this->ParseEndOfDirective(/*ignoreErrors*/ file->IsMissing() || !isActive);
    return SyntaxFactory::LoadDirectiveTrivia(hash, keyword, file, end, isActive);
}

GreenNode *DirectiveParser::ParseNullableDirective(GreenNode *hash, GreenNode *token, bool isActive) {
    if (isActive) {
        token = CheckFeatureAvailability(token);  // MessageID.IDS_FeatureNullableReferenceTypes
    }

    GreenNode *setting;
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::EnableKeyword:
        case SyntaxKind::DisableKeyword:
        case SyntaxKind::RestoreKeyword:
            setting = EatToken();
            break;
        default:
            setting = EatToken(SyntaxKind::DisableKeyword, ErrorCode::ERR_NullableDirectiveQualifierExpected,
                               /*reportError*/ isActive);
            break;
    }

    GreenNode *target;
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::WarningsKeyword:
        case SyntaxKind::AnnotationsKeyword:
            target = EatToken();
            break;
        case SyntaxKind::EndOfDirectiveToken:
        case SyntaxKind::EndOfFileToken:
            target = nullptr;
            break;
        default:
            target = EatToken(SyntaxKind::WarningsKeyword, ErrorCode::ERR_NullableDirectiveTargetExpected,
                              /*reportError*/ !setting->IsMissing() && isActive);
            break;
    }

    GreenNode *end = this->ParseEndOfDirective(
        /*ignoreErrors*/ setting->IsMissing() || (target != nullptr && target->IsMissing()) || !isActive);
    return SyntaxFactory::NullableDirectiveTrivia(hash, token, setting, target, end, isActive);
}

GreenNode *DirectiveParser::ParsePragmaDirective(GreenNode *hash, GreenNode *pragma, bool isActive) {
    if (isActive) {
        pragma = CheckFeatureAvailability(pragma);  // MessageID.IDS_FeaturePragma
    }

    bool hasError = false;
    if (this->CurrentToken()->contextual_kind == SyntaxKind::WarningKeyword) {
        GreenNode *warning = this->EatContextualToken(SyntaxKind::WarningKeyword);
        GreenNode *style;
        if (this->CurrentToken()->kind == SyntaxKind::DisableKeyword ||
            this->CurrentToken()->kind == SyntaxKind::RestoreKeyword) {
            style = this->EatToken();

            SeparatedSyntaxListBuilder ids = SyntaxListBuilder::Create();
            while (this->CurrentToken()->kind != SyntaxKind::EndOfDirectiveToken) {
                GreenNode *id;
                GreenNode *idExpression;

                if (this->CurrentToken()->kind == SyntaxKind::NumericLiteralToken) {
                    // Previous versions of the compiler used to report a warning (CS1691)
                    // whenever an unrecognized warning code was supplied in a #pragma directive
                    // (or via /nowarn /warnaserror flags on the command line).
                    // Going forward, we won't generate any warning in such cases. This will make
                    // maintenance of backwards compatibility easier (we no longer need to worry
                    // about breaking existing projects / command lines if we deprecate / remove
                    // an old warning code).
                    id = this->EatToken();
                    idExpression = SyntaxFactory::LiteralExpression(SyntaxKind::NumericLiteralExpression, id);
                } else if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken) {
                    // Lexing / parsing of identifiers inside #pragma warning directives is identical
                    // to that inside #define directives except that very long identifiers inside #define
                    // are truncated to 128 characters to maintain backwards compatibility with previous
                    // versions of the compiler. (See TruncateIdentifier() below.)
                    // Since support for identifiers inside #pragma warning directives is new,
                    // we don't have any backwards compatibility constraints. So we can preserve the
                    // identifier exactly as it appears in source.
                    id = this->EatToken();
                    idExpression = SyntaxFactory::IdentifierName(id);
                } else {
                    id = this->EatToken(SyntaxKind::NumericLiteralToken, ErrorCode::WRN_IdentifierOrNumericLiteralExpected,
                                        /*reportError*/ isActive);
                    idExpression = SyntaxFactory::LiteralExpression(SyntaxKind::NumericLiteralExpression, id);
                }

                hasError = hasError || id->ContainsDiagnostics();
                ids.Add(idExpression);

                if (this->CurrentToken()->kind != SyntaxKind::CommaToken) {
                    break;
                }

                ids.AddSeparator(this->EatToken());
            }

            GreenNode *end = this->ParseEndOfDirective(hasError || !isActive, /*afterPragma*/ true);
            return SyntaxFactory::PragmaWarningDirectiveTrivia(hash, pragma, warning, style, ids.ToList(), end, isActive);
        } else {
            style = this->EatToken(SyntaxKind::DisableKeyword, ErrorCode::WRN_IllegalPPWarning, /*reportError*/ isActive);
            GreenNode *end = this->ParseEndOfDirective(/*ignoreErrors*/ true, /*afterPragma*/ true);
            return SyntaxFactory::PragmaWarningDirectiveTrivia(hash, pragma, warning, style, nullptr, end, isActive);
        }
    } else if (this->CurrentToken()->kind == SyntaxKind::ChecksumKeyword) {
        GreenNode *checksum = this->EatToken();
        GreenNode *file = this->EatToken(SyntaxKind::StringLiteralToken, ErrorCode::WRN_IllegalPPChecksum,
                                         /*reportError*/ isActive);
        GreenNode *guid = this->EatToken(SyntaxKind::StringLiteralToken, ErrorCode::WRN_IllegalPPChecksum,
                                         /*reportError*/ isActive && !file->IsMissing());
        if (isActive && !guid->IsMissing()) {
            if (!GuidTryParse(guid->value_text)) {
                guid = this->AddError(guid, ErrorCode::WRN_IllegalPPChecksum);
            }
        }

        GreenNode *bytes = this->EatToken(SyntaxKind::StringLiteralToken, ErrorCode::WRN_IllegalPPChecksum,
                                          /*reportError*/ isActive && !guid->IsMissing());
        if (isActive && !bytes->IsMissing()) {
            if (bytes->value_text.size() % 2 != 0) {
                bytes = this->AddError(bytes, ErrorCode::WRN_IllegalPPChecksum);
            } else {
                for (char16_t c : bytes->value_text) {
                    if (!SyntaxFacts::IsHexDigit(c)) {
                        bytes = this->AddError(bytes, ErrorCode::WRN_IllegalPPChecksum);
                        break;
                    }
                }
            }
        }

        hasError = file->ContainsDiagnostics() || guid->ContainsDiagnostics() || bytes->ContainsDiagnostics();  // (C# `|`; no side effects)
        GreenNode *eod = this->ParseEndOfDirective(/*ignoreErrors*/ hasError, /*afterPragma*/ true);
        return SyntaxFactory::PragmaChecksumDirectiveTrivia(hash, pragma, checksum, file, guid, bytes, eod, isActive);
    } else {
        GreenNode *warning = this->EatToken(SyntaxKind::WarningKeyword, ErrorCode::WRN_IllegalPragma,
                                            /*reportError*/ isActive);
        GreenNode *style = this->EatToken(SyntaxKind::DisableKeyword, /*reportError*/ false);
        GreenNode *eod = this->ParseEndOfDirective(/*ignoreErrors*/ true, /*afterPragma*/ true);
        return SyntaxFactory::PragmaWarningDirectiveTrivia(hash, pragma, warning, style, nullptr, eod, isActive);
    }
}

GreenNode *DirectiveParser::ParseShebangDirective(GreenNode *hash, GreenNode *exclamation, bool isActive) {
    // lexer.Options.Kind != SourceCodeKind.Script && !lexer.Options.FileBasedProgram: always.
    exclamation = this->AddError(exclamation, ErrorCode::ERR_PPShebangInProjectBasedProgram);

    return SyntaxFactory::ShebangDirectiveTrivia(hash, exclamation, this->ParseEndOfDirectiveWithOptionalPreprocessingMessage(),
                                                 isActive);
}

GreenNode *DirectiveParser::ParseIgnoredDirective(GreenNode *hash, GreenNode *colon, bool isActive,
                                                  bool isFollowingToken) {
    if (isActive) {
        // !lexer.Options.FileBasedProgram: always.
        colon = this->AddError(colon, ErrorCode::ERR_PPIgnoredNeedsFileBasedProgram);

        if (isFollowingToken) {
            colon = this->AddError(colon, ErrorCode::ERR_PPIgnoredFollowsToken);
        }

        if (_context.SeenAnyIfDirectives()) {
            colon = this->AddError(colon, ErrorCode::ERR_PPIgnoredFollowsIf);
        }
    }

    GreenNode *content = nullptr;
    GreenNode *endOfDirective = this->lexer->LexEndOfDirectiveWithOptionalContent(content);
    return SyntaxFactory::IgnoredDirectiveTrivia(hash, colon, content, endOfDirective, isActive);
}

GreenNode *DirectiveParser::ParseEndOfDirectiveWithOptionalPreprocessingMessage() {
    return this->lexer->LexEndOfDirectiveWithOptionalPreprocessingMessage();
}

GreenNode *DirectiveParser::ParseEndOfDirective(bool ignoreErrors, bool afterPragma, bool afterLineNumber) {
    SyntaxListBuilder skippedTokens;  // null

    // Consume all extraneous tokens as leading SkippedTokens trivia.
    if (this->CurrentToken()->kind != SyntaxKind::EndOfDirectiveToken &&
        this->CurrentToken()->kind != SyntaxKind::EndOfFileToken) {
        skippedTokens = SyntaxListBuilder::Create();

        if (!ignoreErrors) {
            ErrorCode errorCode = ErrorCode::ERR_EndOfPPLineExpected;
            if (afterPragma) {
                errorCode = ErrorCode::WRN_EndOfPPLineExpected;
            } else if (afterLineNumber) {
                errorCode = ErrorCode::ERR_MissingPPFile;
            }

            skippedTokens.Add(this->AddError(WithoutDiagnosticsGreen(this->EatToken()), errorCode));
        }

        while (this->CurrentToken()->kind != SyntaxKind::EndOfDirectiveToken &&
               this->CurrentToken()->kind != SyntaxKind::EndOfFileToken) {
            skippedTokens.Add(WithoutDiagnosticsGreen(this->EatToken()));
        }
    }

    // attach text from extraneous tokens as trivia to EndOfDirective token
    GreenNode *endOfDirective = this->CurrentToken()->kind == SyntaxKind::EndOfDirectiveToken
                                    ? this->EatToken()
                                    : SyntaxFactory::Token(SyntaxKind::EndOfDirectiveToken);

    if (!skippedTokens.IsNull()) {
        endOfDirective = TokenWithLeadingTrivia(endOfDirective, SyntaxFactory::SkippedTokensTrivia(skippedTokens.ToList()));
    }

    return endOfDirective;
}

GreenNode *DirectiveParser::ParseExpression() { return this->ParseLogicalOr(); }

GreenNode *DirectiveParser::ParseLogicalOr() {
    GreenNode *left = this->ParseLogicalAnd();
    while (this->CurrentToken()->kind == SyntaxKind::BarBarToken) {
        GreenNode *op = this->EatToken();
        GreenNode *right = this->ParseLogicalAnd();
        left = SyntaxFactory::BinaryExpression(SyntaxKind::LogicalOrExpression, left, op, right);
    }

    return left;
}

GreenNode *DirectiveParser::ParseLogicalAnd() {
    GreenNode *left = this->ParseEquality();
    while (this->CurrentToken()->kind == SyntaxKind::AmpersandAmpersandToken) {
        GreenNode *op = this->EatToken();
        GreenNode *right = this->ParseEquality();
        left = SyntaxFactory::BinaryExpression(SyntaxKind::LogicalAndExpression, left, op, right);
    }

    return left;
}

GreenNode *DirectiveParser::ParseEquality() {
    GreenNode *left = this->ParseLogicalNot();
    while (this->CurrentToken()->kind == SyntaxKind::EqualsEqualsToken ||
           this->CurrentToken()->kind == SyntaxKind::ExclamationEqualsToken) {
        GreenNode *op = this->EatToken();
        GreenNode *right = this->ParseEquality();  // (right-recursive, as in Roslyn)
        left = SyntaxFactory::BinaryExpression(SyntaxFacts::GetBinaryExpression(op->kind), left, op, right);
    }

    return left;
}

GreenNode *DirectiveParser::ParseLogicalNot() {
    if (this->CurrentToken()->kind == SyntaxKind::ExclamationToken) {
        GreenNode *op = this->EatToken();
        return SyntaxFactory::PrefixUnaryExpression(SyntaxKind::LogicalNotExpression, op, this->ParseLogicalNot());
    }

    return this->ParsePrimary();
}

GreenNode *DirectiveParser::ParsePrimary() {
    SyntaxKind k = this->CurrentToken()->kind;
    switch (k) {
        case SyntaxKind::OpenParenToken: {
            GreenNode *open = this->EatToken();
            GreenNode *expr = this->ParseExpression();
            GreenNode *close = this->EatToken(SyntaxKind::CloseParenToken);
            return SyntaxFactory::ParenthesizedExpression(open, expr, close);
        }
        case SyntaxKind::IdentifierToken: {
            GreenNode *identifier = TruncateIdentifier(this->EatToken());
            return SyntaxFactory::IdentifierName(identifier);
        }
        case SyntaxKind::TrueKeyword:
        case SyntaxKind::FalseKeyword:
            return SyntaxFactory::LiteralExpression(SyntaxFacts::GetLiteralExpression(k), this->EatToken());
        default:
            return SyntaxFactory::IdentifierName(this->EatToken(SyntaxKind::IdentifierToken, ErrorCode::ERR_InvalidPreprocExpr));
    }
}

// Ignore everything after the 128th character by setting the value text of the token
// from the prefix.  This is for backwards compatibility with Dev 10.
GreenNode *DirectiveParser::TruncateIdentifier(GreenNode *identifier) {
    if (identifier->Width() > MAX_DIRECTIVE_IDENTIFIER_WIDTH) {
        GreenNode *leading = identifier->GetLeadingTrivia();
        GreenNode *trailing = identifier->GetTrailingTrivia();

        Text text = identifier->text;
        Text identifierPart = text.substr(0, MAX_DIRECTIVE_IDENTIFIER_WIDTH);

        identifier = SyntaxFactory::Identifier(SyntaxKind::IdentifierToken, leading, text, identifierPart, trailing);
    }
    return identifier;
}

bool DirectiveParser::EvaluateBool(GreenNode *expr) {
    // `result is bool ? (bool)result : false` -- Evaluate only ever yields bools.
    return Evaluate(expr);
}

bool DirectiveParser::Evaluate(GreenNode *expr) {
    switch (expr->kind) {
        case SyntaxKind::ParenthesizedExpression:
            return Evaluate(GetField(expr, Field::Expression));
        case SyntaxKind::TrueLiteralExpression:
        case SyntaxKind::FalseLiteralExpression:
            // ((LiteralExpressionSyntax)expr).Token.Value: true / false for the keyword tokens.
            return GetField(expr, Field::Token)->kind == SyntaxKind::TrueKeyword;
        case SyntaxKind::LogicalAndExpression:
        case SyntaxKind::BitwiseAndExpression:
            return EvaluateBool(GetField(expr, Field::Left)) && EvaluateBool(GetField(expr, Field::Right));
        case SyntaxKind::LogicalOrExpression:
        case SyntaxKind::BitwiseOrExpression:
            return EvaluateBool(GetField(expr, Field::Left)) || EvaluateBool(GetField(expr, Field::Right));
        case SyntaxKind::EqualsExpression:
            return Evaluate(GetField(expr, Field::Left)) == Evaluate(GetField(expr, Field::Right));
        case SyntaxKind::NotEqualsExpression:
            return Evaluate(GetField(expr, Field::Left)) != Evaluate(GetField(expr, Field::Right));
        case SyntaxKind::LogicalNotExpression:
            return !EvaluateBool(GetField(expr, Field::Operand));
        case SyntaxKind::IdentifierName: {
            // For backwards compatibility, we want to evaluate any unicode escape sequences in
            // the identifier name and then check again for boolean literals.  (This actually
            // seems like a bug that we're retaining for back-compat, because section 2.4.1 of
            // the spec says that escape sequences can only appear in identifiers, character
            // literals, and regular string literals - not boolean literals.  In (non-directive)
            // C#, true is equivalent to the identifier @true, not the boolean literal true.)
            Text id = GetField(expr, Field::Identifier)->value_text;
            bool constantValue = false;
            if (BoolTryParse(id, constantValue)) {
                return constantValue;
            }
            return IsDefined(id);
        }
        default:
            break;
    }

    return false;
}

bool DirectiveParser::IsDefined(Text id) {
    DefineState defState = _context.IsDefined(id);
    switch (defState) {
        default:
        case DefineState::Unspecified:
            return false;  // this.Options.PreprocessorSymbols.Contains(id): no symbols are defined
        case DefineState::Defined:
            return true;
        case DefineState::Undefined:
            return false;
    }
}

}  // namespace csharp
