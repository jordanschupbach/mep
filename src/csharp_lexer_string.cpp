// mep's C# lexer, part 3: string and character literals, interpolated and raw string literals.
// A translation of Roslyn's src/Compilers/CSharp/Portable/Parser/Lexer_StringLiteral.cs, Lexer_RawStringLiteral.cs
// and Lexer.Interpolation.cs (MIT, (c) .NET Foundation and Contributors; see csharp_syntax.h for the commit).
// Same method order as the C#.

#include <string>

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief A one-character string in the arena (C# `ch.ToString()` as a diagnostic argument). */
Text CharText(char16_t ch) { return CurrentArena()->CopyText(std::u16string(1, ch)); }

/** @brief Appends a green node's full text (trivia included) to `out`. */
void AppendFullText(const GreenNode *node, std::u16string &out) {
    if (node == nullptr) return;
    if (node->IsToken()) {
        AppendFullText(node->leading, out);
        out.append(node->text);
        AppendFullText(node->trailing, out);
        return;
    }
    if (node->IsTrivia()) {
        out.append(node->text);
        return;
    }
    for (int i = 0; i < node->slot_count; i++) AppendFullText(node->slots[i], out);
}

}  // namespace

// --- Lexer_StringLiteral.cs ---------------------------------------------------------------------------------

void Lexer::ScanStringLiteral(TokenInfo &info, bool inDirective) {
    char16_t quoteCharacter = TextWindow.PeekChar();
    CS_ASSERT(quoteCharacter == u'\'' || quoteCharacter == u'"');

    if (TextWindow.PeekChar() == u'"' && TextWindow.PeekChar(1) == u'"' && TextWindow.PeekChar(2) == u'"') {
        ScanRawStringLiteral(info, inDirective);
        if (inDirective) {
            // Reinterpret this as just a string literal so that the directive parser can consume this.
            // But report this is illegal so that the user knows to fix this up to be a normal string.
            info.Kind = SyntaxKind::StringLiteralToken;
            info.StringValue = u"";
            info.HasStringValue = true;
            AddError(ErrorCode::ERR_RawStringNotInDirectives);
        }
        return;
    }

    TextWindow.AdvanceChar();
    _builder.clear();

    while (true) {
        char16_t ch = TextWindow.PeekChar();

        // Normal string & char constants can have escapes. Strings in directives cannot.
        if (ch == u'\\' && !inDirective) {
            char16_t c2;
            ch = ScanEscapeSequence(c2);
            _builder.push_back(ch);
            if (c2 != SlidingTextWindow::InvalidCharacter) {
                _builder.push_back(c2);
            }
        } else if (ch == quoteCharacter) {
            TextWindow.AdvanceChar();
            break;
        } else if (SyntaxFacts::IsNewLine(ch) ||
                   (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd())) {
            // String and character literals can contain any Unicode character. They are not limited
            // to valid UTF-16 characters. So if we get the SlidingTextWindow's sentinel value,
            // double check that it was not real user-code contents. This will be rare.
            CS_ASSERT(CurrentLexemeWidth() > 0);
            AddError(ErrorCode::ERR_NewlineInConst);
            break;
        } else {
            TextWindow.AdvanceChar();
            _builder.push_back(ch);
        }
    }

    if (quoteCharacter == u'\'') {
        info.Text_ = GetInternedLexemeText();
        info.HasText = true;
        info.Kind = SyntaxKind::CharacterLiteralToken;
        if (_builder.size() != 1) {
            AddError(!_builder.empty() ? ErrorCode::ERR_TooManyCharsInConst : ErrorCode::ERR_EmptyCharConst);
        }

        if (!_builder.empty()) {
            info.StringValue = Intern(_builder);
            info.HasStringValue = true;
            info.CharValue = info.StringValue[0];
        } else {
            info.StringValue = u"";
            info.HasStringValue = true;
            info.CharValue = SlidingTextWindow::InvalidCharacter;
        }
    } else {
        if (!inDirective && ScanUtf8Suffix()) {
            info.Kind = SyntaxKind::Utf8StringLiteralToken;
        } else {
            info.Kind = SyntaxKind::StringLiteralToken;
        }

        info.Text_ = GetInternedLexemeText();
        info.HasText = true;

        if (!_builder.empty()) {
            info.StringValue = Intern(_builder);
        } else {
            info.StringValue = u"";
        }
        info.HasStringValue = true;
    }
}

bool Lexer::ScanUtf8Suffix() {
    char16_t ch = TextWindow.PeekChar();
    if ((ch == u'u' || ch == u'U') && TextWindow.PeekChar(1) == u'8') {
        TextWindow.AdvanceChar(2);
        return true;
    }

    return false;
}

char16_t Lexer::ScanEscapeSequence(char16_t &surrogateCharacter) {
    int start = TextWindow.Position();
    surrogateCharacter = SlidingTextWindow::InvalidCharacter;
    char16_t ch = TextWindow.NextChar();
    CS_ASSERT(ch == u'\\');

    ch = TextWindow.NextChar();
    switch (ch) {
        // escaped characters that translate to themselves
        case u'\'':
        case u'"':
        case u'\\':
            break;
        // translate escapes as per C# spec 2.4.4.4
        case u'0':
            ch = u'\u0000';
            break;
        case u'a':
            ch = u'\u0007';
            break;
        case u'b':
            ch = u'\u0008';
            break;
        case u'e':
            // MessageID.IDS_FeatureStringEscapeCharacter: always available (LanguageVersion.Preview), no diagnostic.
            ch = u'\u001b';
            break;
        case u'f':
            ch = u'\u000c';
            break;
        case u'n':
            ch = u'\u000a';
            break;
        case u'r':
            ch = u'\u000d';
            break;
        case u't':
            ch = u'\u0009';
            break;
        case u'v':
            ch = u'\u000b';
            break;
        case u'x':
        case u'u':
        case u'U': {
            TextWindow.Reset(start);
            std::optional<SyntaxDiagnosticInfo> error;
            ch = NextUnicodeEscape(surrogateCharacter, error);
            AddError(error);
            break;
        }
        default:
            AddError(start, TextWindow.Position() - start, ErrorCode::ERR_IllegalEscape);
            break;
    }

    return ch;
}

void Lexer::ScanVerbatimStringLiteral(TokenInfo &info) {
    CS_ASSERT(TextWindow.PeekChar() == u'@');
    _builder.clear();

    int start = TextWindow.Position();
    while (TextWindow.PeekChar() == u'@') {
        TextWindow.AdvanceChar();
    }

    if (TextWindow.Position() - start >= 2) {
        AddError(start, /*width:*/ TextWindow.Position() - start, ErrorCode::ERR_IllegalAtSequence);
    }

    CS_ASSERT(TextWindow.PeekChar() == u'"');
    TextWindow.AdvanceChar();

    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (ch == u'"') {
            TextWindow.AdvanceChar();
            if (TextWindow.PeekChar() == u'"') {
                // Doubled quote -- skip & put the single quote in the string and keep going.
                TextWindow.AdvanceChar();
                _builder.push_back(ch);
                continue;
            }

            // otherwise, the string is finished.
            break;
        }

        if (ch == SlidingTextWindow::InvalidCharacter && TextWindow.IsReallyAtEnd()) {
            // Reached the end of the source without finding the end-quote.  Give an error back at the
            // starting point. And finish lexing this string.
            AddError(ErrorCode::ERR_UnterminatedStringLit);
            break;
        }

        TextWindow.AdvanceChar();
        _builder.push_back(ch);
    }

    if (ScanUtf8Suffix()) {
        info.Kind = SyntaxKind::Utf8StringLiteralToken;
    } else {
        info.Kind = SyntaxKind::StringLiteralToken;
    }

    info.Text_ = GetNonInternedLexemeText();
    info.HasText = true;
    info.StringValue = Intern(_builder);
    info.HasStringValue = true;
}

void Lexer::ScanInterpolatedStringLiteral(TokenInfo &info) {
    // We have a string of one of the forms
    //                $" ... "
    //                $@" ... "
    //                @$" ... "
    // Where the contents contains zero or more sequences
    //                { STUFF }
    // where these curly braces delimit STUFF in expression "holes".
    // In order to properly find the closing quote of the whole string,
    // we need to locate the closing brace of each hole, as strings
    // may appear in expressions in the holes. So we
    // need to match up any braces that appear between them.
    // But in order to do that, we also need to match up any
    // /**/ comments, ' characters quotes, () parens
    // [] brackets, and "" strings, including interpolated holes in the latter.

    std::optional<SyntaxDiagnosticInfo> error;
    InterpolatedStringKind kind;
    TextRange openQuoteRange;
    TextRange closeQuoteRange;
    ScanInterpolatedOrRawStringLiteralTop(info, /*isInterpolatedString:*/ true, error, kind, openQuoteRange,
                                          /*interpolations:*/ nullptr, closeQuoteRange);
    AddError(error);
}

void Lexer::ScanInterpolatedOrRawStringLiteralTop(TokenInfo &info, bool isInterpolatedString,
                                                  std::optional<SyntaxDiagnosticInfo> &error,
                                                  InterpolatedStringKind &kind, TextRange &openQuoteRange,
                                                  std::vector<Interpolation> *interpolations,
                                                  TextRange &closeQuoteRange) {
    InterpolatedOrRawStringScanner subScanner(this, isInterpolatedString);
    subScanner.ScanStringLiteralTop(kind, openQuoteRange, interpolations, closeQuoteRange);
    error = subScanner.Error;
    info.Kind = SyntaxKind::InterpolatedStringToken;
    info.Text_ = GetNonInternedLexemeText();
    info.HasText = true;
}

/// Turn a (parsed) interpolated string nonterminal into an interpolated string token.
/// (Only the Blender calls this, i.e. incremental parsing, which mep does not do.)
GreenNode *Lexer::RescanInterpolatedString(GreenNode *interpolatedString) {
    // interpolatedString.ToString(): the text without the first token's leading and the last token's trailing trivia.
    std::u16string full;
    AppendFullText(interpolatedString, full);
    int leadingWidth = interpolatedString->GetLeadingTriviaWidth();
    Text text = CurrentArena()->CopyText(
        Text(full).substr(static_cast<size_t>(leadingWidth), static_cast<size_t>(interpolatedString->Width())));
    SyntaxKind kind = SyntaxKind::InterpolatedStringToken;
    // TODO: scan the contents (perhaps using ScanInterpolatedStringLiteralContents) to reconstruct any lexical
    // errors such as // inside an expression hole
    GreenNode *firstToken = interpolatedString->GetFirstToken();
    GreenNode *lastToken = interpolatedString->GetLastToken();
    return SyntaxFactory::Literal(firstToken ? firstToken->GetLeadingTrivia() : nullptr, text, kind, text,
                                  lastToken ? lastToken->GetTrailingTrivia() : nullptr);
}

// --- InterpolatedOrRawStringScanner ---------------------------------------------------------------------------

bool Lexer::InterpolatedOrRawStringScanner::IsAtEnd(InterpolatedStringKind kind) {
    return IsAtEnd(/*allowNewline:*/ kind == InterpolatedStringKind::Verbatim ||
                   kind == InterpolatedStringKind::MultiLineRaw);
}

bool Lexer::InterpolatedOrRawStringScanner::IsAtEnd(bool allowNewline) {
    char16_t ch = _lexer->TextWindow.PeekChar();
    return (!allowNewline && SyntaxFacts::IsNewLine(ch)) ||
           (ch == SlidingTextWindow::InvalidCharacter && _lexer->TextWindow.IsReallyAtEnd());
}

void Lexer::InterpolatedOrRawStringScanner::TrySetError(const SyntaxDiagnosticInfo &error) {
    // only need to record the first error we hit
    if (!Error) Error = error;
}

void Lexer::InterpolatedOrRawStringScanner::ScanStringLiteralTop(InterpolatedStringKind &kind,
                                                                 TextRange &openQuoteRange,
                                                                 std::vector<Interpolation> *interpolations,
                                                                 TextRange &closeQuoteRange) {
    // Scan through the open-quote portion of this literal, determining important information the rest of
    // the scanning needs.
    int start = _lexer->TextWindow.Position();
    int startingDollarSignCount = 0;
    int startingQuoteCount = 0;
    bool succeeded = ScanOpenQuote(kind, startingDollarSignCount, startingQuoteCount);
    CS_ASSERT(_lexer->TextWindow.Position() != start);

    openQuoteRange = TextRange{start, _lexer->TextWindow.Position()};

    if (!succeeded) {
        // Processing the start of this literal didn't give us enough information to proceed.  Stop now,
        // terminating the string to the furthest point we reached.
        closeQuoteRange = TextRange{_lexer->TextWindow.Position(), _lexer->TextWindow.Position()};
        return;
    }

    ScanInterpolatedStringLiteralContents(kind, startingDollarSignCount, startingQuoteCount, interpolations);
    ScanInterpolatedStringLiteralEnd(kind, startingQuoteCount, closeQuoteRange);
}

/// startingDollarSignCount: Number of '$' characters this interpolated string started with.  We'll need to see that
/// many '{' in a row to start an interpolation.  Any less and we'll treat that as just text.  Note if this count is
/// '1' then this is a normal (non-raw) interpolation and `{{` is treated as an escape.
/// startingQuoteCount: Number of '"' characters this interpolated string started with.
/// Returns true if we successfully processed the open quote range and can proceed to the rest of the literal. false
/// if we were not successful and should stop processing.
bool Lexer::InterpolatedOrRawStringScanner::ScanOpenQuote(InterpolatedStringKind &kind, int &startingDollarSignCount,
                                                          int &startingQuoteCount) {
    // Handles reading the start of the interpolated string literal (up to where the content begins)
    SlidingTextWindow &window = _lexer->TextWindow;
    int start = window.Position();

    char16_t c0 = window.PeekChar(0);
    char16_t c1 = window.PeekChar(1);
    char16_t c2 = window.PeekChar(2);
    if ((c0 == u'$' && c1 == u'@' && c2 == u'"') || (c0 == u'@' && c1 == u'$' && c2 == u'"')) {
        // $@" or @$"
        //
        // Note: we do not consider $@""" as the start of raw-string (in error conditions) as that's a legal
        // verbatim string beginning already.

        kind = InterpolatedStringKind::Verbatim;
        startingDollarSignCount = 1;
        startingQuoteCount = 1;
        window.AdvanceChar(3);
        return true;
    }

    char16_t c3 = window.PeekChar(3);
    if ((c0 == u'$' && c1 == u'"' && c2 != u'"') || (c0 == u'$' && c1 == u'"' && c2 == u'"' && c3 != u'"')) {
        // $"...
        // $""
        // not $"""
        kind = InterpolatedStringKind::Normal;
        startingDollarSignCount = 1;
        startingQuoteCount = 1;
        window.AdvanceChar(2);
        return true;
    }

    // From this point we have either a complete error case that we cannot process further, or a raw literal
    // of some sort.
    int prefixAtCount = _lexer->ConsumeAtSignSequence();
    startingDollarSignCount = _lexer->ConsumeDollarSignSequence();

    int suffixAtCount = _lexer->ConsumeAtSignSequence();
    startingQuoteCount = _lexer->ConsumeQuoteSequence();

    int totalAtCount = prefixAtCount + suffixAtCount;

    if (_isInterpolatedString) {
        CS_ASSERT(startingDollarSignCount > 0);
    } else {
        CS_ASSERT(startingDollarSignCount == 0);
        CS_ASSERT(totalAtCount == 0);
    }

    // We should only have gotten here if we had at least two characters that made us think we had an
    // interpolated string. Note that we may enter here on just `@@` or `$$` (without seeing anything else),
    // so we can't put a stricter bound on this here.
    CS_ASSERT(totalAtCount + startingDollarSignCount + startingQuoteCount >= 2);

    if (startingQuoteCount == 0) {
        // We have no quotes at all.  We cannot continue on as we have no quotes, and thus can't even find
        // where the string starts or ends.
        TrySetError(
            _lexer->MakeError(start, window.Position() - start, ErrorCode::ERR_StringMustStartWithQuoteCharacter));
        kind = totalAtCount == 1 && startingDollarSignCount == 1 ? InterpolatedStringKind::Verbatim
                                                                 : InterpolatedStringKind::SingleLineRaw;
        return false;
    }

    // @-signs with interpolations are always illegal.  Detect these and give a reasonable error message.
    // Continue on if we can.
    if (totalAtCount > 0) {
        TrySetError(_lexer->MakeError(start, window.Position() - start, ErrorCode::ERR_IllegalAtSequence));
    }

    if (startingQuoteCount < 3) {
        // 1-2 quotes present.  Not legal.  But we can give a good error message and still proceed.
        TrySetError(_lexer->MakeError(window.Position() - startingQuoteCount, startingQuoteCount,
                                      ErrorCode::ERR_NotEnoughQuotesForRawString));
    }

    // Now see if this was a single-line or multi-line raw literal.

    int afterQuotePosition = window.Position();
    _lexer->ConsumeWhitespace();
    if (SyntaxFacts::IsNewLine(window.PeekChar())) {
        // We had whitespace followed by a newline.  That section is considered the open-quote section of
        // the literal.
        window.AdvancePastNewLine();
        kind = InterpolatedStringKind::MultiLineRaw;
    } else {
        // wasn't multi-line, jump back to right after the quotes as what follows is content and not
        // considered part of the open quote.
        window.Reset(afterQuotePosition);
        kind = InterpolatedStringKind::SingleLineRaw;
    }

    return true;
}

void Lexer::InterpolatedOrRawStringScanner::ScanInterpolatedStringLiteralEnd(InterpolatedStringKind kind,
                                                                             int startingQuoteCount,
                                                                             TextRange &closeQuoteRange) {
    // Handles reading the end of the interpolated string literal (after where the content ends)

    int closeQuotePosition = _lexer->TextWindow.Position();

    if (kind == InterpolatedStringKind::Normal || kind == InterpolatedStringKind::Verbatim) {
        ScanNormalOrVerbatimInterpolatedStringLiteralEnd(kind);
    } else {
        CS_ASSERT(kind == InterpolatedStringKind::SingleLineRaw || kind == InterpolatedStringKind::MultiLineRaw);
        ScanRawInterpolatedStringLiteralEnd(kind, startingQuoteCount);

        if (!_isInterpolatedString) _lexer->ScanUtf8Suffix();
    }

    // Note: this range may be empty.  For example, if we hit the end of a line for a single-line construct,
    // or we hit the end of a file for a multi-line construct.
    closeQuoteRange = TextRange{closeQuotePosition, _lexer->TextWindow.Position()};
}

void Lexer::InterpolatedOrRawStringScanner::ScanNormalOrVerbatimInterpolatedStringLiteralEnd(
    InterpolatedStringKind kind) {
    CS_ASSERT(kind == InterpolatedStringKind::Normal || kind == InterpolatedStringKind::Verbatim);
    (void)kind;

    if (_lexer->TextWindow.PeekChar() != u'"') {
        // Didn't find a closing quote.  We hit the end of a line (in the normal case) or the end of the
        // file in the normal/verbatim case.
        CS_ASSERT(IsAtEnd(kind));

        int position = IsAtEnd(/*allowNewline:*/ true) ? _lexer->TextWindow.Position() - 1
                                                        : _lexer->TextWindow.Position();
        TrySetError(_lexer->MakeError(position, /*width:*/ 1, ErrorCode::ERR_UnterminatedStringLit));
    } else {
        // found the closing quote
        _lexer->TextWindow.AdvanceChar();  // "
    }
}

void Lexer::InterpolatedOrRawStringScanner::ScanRawInterpolatedStringLiteralEnd(InterpolatedStringKind kind,
                                                                                int startingQuoteCount) {
    CS_ASSERT(kind == InterpolatedStringKind::SingleLineRaw || kind == InterpolatedStringKind::MultiLineRaw);

    if (kind == InterpolatedStringKind::SingleLineRaw) {
        if (_lexer->TextWindow.PeekChar() != u'"') {
            // Didn't find a closing quote.  We hit the end of a line (in the normal case) or the end of the
            // file in the normal/verbatim case.
            CS_ASSERT(IsAtEnd(kind));

            TrySetError(_lexer->MakeError(_lexer->TextWindow.Position(),
                                          /*width:*/ SyntaxFacts::IsNewLine(_lexer->TextWindow.PeekChar()) ? 1 : 0,
                                          ErrorCode::ERR_UnterminatedRawString));
        } else {
            int closeQuoteCount = _lexer->ConsumeQuoteSequence();

            // We should only hit here if we had enough close quotes to end the string.  If we didn't have
            // enough they should have just have been consumed as content, and we'd hit the 'true' case in
            // this 'if' instead.
            //
            // If we have too many close quotes for this string, report an error on the excess quotes so the
            // user knows how many they need to delete.
            CS_ASSERT(closeQuoteCount >= startingQuoteCount);
            if (closeQuoteCount > startingQuoteCount) {
                int excessQuoteCount = closeQuoteCount - startingQuoteCount;
                TrySetError(_lexer->MakeError(/*position:*/ _lexer->TextWindow.Position() - excessQuoteCount,
                                              /*width:*/ excessQuoteCount, ErrorCode::ERR_TooManyQuotesForRawString));
            }
        }
    } else {
        // A multiline literal might end either because:
        //
        // 1. we hit the end of the file.
        // 2. we hit quotes *after* content on a line.
        // 3. we found the legitimate end to the literal.

        if (IsAtEnd(kind)) {
            TrySetError(
                _lexer->MakeError(_lexer->TextWindow.Position(), /*width:*/ 0, ErrorCode::ERR_UnterminatedRawString));
        } else if (_lexer->TextWindow.PeekChar() == u'"') {
            // Don't allow a content line to contain a quote sequence that looks like a delimiter (or longer)
            int closeQuoteCount = _lexer->ConsumeQuoteSequence();

            // We must have too many close quotes.  If we had less, they would have just been consumed as content.
            CS_ASSERT(closeQuoteCount >= startingQuoteCount);

            TrySetError(_lexer->MakeError(/*position:*/ _lexer->TextWindow.Position() - closeQuoteCount,
                                          /*width:*/ closeQuoteCount, ErrorCode::ERR_RawStringDelimiterOnOwnLine));
        } else {
            _lexer->TextWindow.AdvancePastNewLine();
            _lexer->ConsumeWhitespace();

            int closeQuoteCount = _lexer->ConsumeQuoteSequence();

            // We should only hit here if we had enough close quotes to end the string.  If we didn't have
            // enough they should have just have been consumed as content, and we'd hit one of the above cases
            // instead.
            CS_ASSERT(closeQuoteCount >= startingQuoteCount);
            if (closeQuoteCount > startingQuoteCount) {
                int excessQuoteCount = closeQuoteCount - startingQuoteCount;
                TrySetError(_lexer->MakeError(/*position:*/ _lexer->TextWindow.Position() - excessQuoteCount,
                                              /*width:*/ excessQuoteCount, ErrorCode::ERR_TooManyQuotesForRawString));
            }
        }
    }
}

void Lexer::InterpolatedOrRawStringScanner::ScanInterpolatedStringLiteralContents(
    InterpolatedStringKind kind, int startingDollarSignCount, int startingQuoteCount,
    std::vector<Interpolation> *interpolations) {
    // Check for the trivial multi-line raw string literal of the form:
    //
    // $"""
    //  """
    //
    // And give the special message that a content line is required in the literal.
    if (CheckForIllegalEmptyMultiLineRawStringLiteral(kind, startingQuoteCount)) return;

    while (true) {
        if (IsAtEnd(kind)) {
            // error: end of line/file before end of string pop out. Error will be reported in
            // ScanInterpolatedStringLiteralEnd
            return;
        }

        if (IsAtEndOfMultiLineRawLiteral(kind, startingQuoteCount)) return;

        char16_t peek = _lexer->TextWindow.PeekChar();
        if (peek == u'"') {
            // Depending on the type of string or the escapes involved, this may be the end of the
            // string literal, or it may just be content.
            if (IsEndDelimiterOtherwiseConsume(kind, startingQuoteCount)) return;

            continue;
        } else if (peek == u'}' && _isInterpolatedString) {
            HandleCloseBraceInContent(kind, startingDollarSignCount);
            continue;
        } else if (peek == u'{' && _isInterpolatedString) {
            HandleOpenBraceInContent(kind, startingDollarSignCount, interpolations);
            continue;
        } else if (peek == u'\\') {
            // In a normal interpolated string a backslash starts an escape. In all other interpolated
            // strings it's just a backslash.
            if (kind == InterpolatedStringKind::Normal) {
                int escapeStart = _lexer->TextWindow.Position();
                char16_t discarded;
                char16_t ch = _lexer->ScanEscapeSequence(/*surrogateCharacter:*/ discarded);
                if (ch == u'{' || ch == u'}') {
                    TrySetError(_lexer->MakeError(escapeStart, _lexer->TextWindow.Position() - escapeStart,
                                                  ErrorCode::ERR_EscapedCurly, {CharText(ch)}));
                }
            } else {
                _lexer->TextWindow.AdvanceChar();
            }

            continue;
        } else {
            // found some other character in the string portion.  Just consume it as content and continue.
            _lexer->TextWindow.AdvanceChar();
            continue;
        }
    }
}

bool Lexer::InterpolatedOrRawStringScanner::CheckForIllegalEmptyMultiLineRawStringLiteral(InterpolatedStringKind kind,
                                                                                         int startingQuoteCount) {
    if (kind == InterpolatedStringKind::MultiLineRaw) {
        _lexer->ConsumeWhitespace();
        int beforeQuotesPosition = _lexer->TextWindow.Position();
        int closeQuoteCount = _lexer->ConsumeQuoteSequence();

        if (closeQuoteCount >= startingQuoteCount) {
            // Found the end of the string.  reset our position so that ScanInterpolatedStringLiteralEnd
            // can consume it.
            TrySetError(_lexer->MakeError(_lexer->TextWindow.Position() - closeQuoteCount, closeQuoteCount,
                                          ErrorCode::ERR_RawStringMustContainContent));
            _lexer->TextWindow.Reset(beforeQuotesPosition);
            return true;
        }
    }

    return false;
}

bool Lexer::InterpolatedOrRawStringScanner::IsAtEndOfMultiLineRawLiteral(InterpolatedStringKind kind,
                                                                         int startingQuoteCount) {
    if (kind == InterpolatedStringKind::MultiLineRaw) {
        // A multiline string ends with a newline, whitespace and at least as many quotes as we started with.

        int startPosition = _lexer->TextWindow.Position();
        if (SyntaxFacts::IsNewLine(_lexer->TextWindow.PeekChar())) {
            _lexer->TextWindow.AdvancePastNewLine();
            _lexer->ConsumeWhitespace();
            int closeQuoteCount = _lexer->ConsumeQuoteSequence();

            _lexer->TextWindow.Reset(startPosition);

            if (closeQuoteCount >= startingQuoteCount) return true;
        }
    }

    return false;
}

/// Returns true if the quote was an end delimiter and lexing of the contents of the
/// interpolated string literal should stop.  If it was an end delimiter it will not be consumed.  If it is
/// content and should not terminate the string then it will be consumed by this method.
bool Lexer::InterpolatedOrRawStringScanner::IsEndDelimiterOtherwiseConsume(InterpolatedStringKind kind,
                                                                           int startingQuoteCount) {
    if (kind == InterpolatedStringKind::Normal || kind == InterpolatedStringKind::Verbatim) {
        // When recovering from mismatched delimiters, we consume the next sequence of quote
        // characters as the close quote for the interpolated string. In practice this gets us
        // out of trouble in scenarios we've encountered. See, for example,
        // https://github.com/dotnet/roslyn/issues/44789
        if (RecoveringFromRunawayLexing()) {
            return true;
        }

        if (kind == InterpolatedStringKind::Normal) {
            // Was in a normal $"  string, the next " closes us.
            return true;
        }

        CS_ASSERT(kind == InterpolatedStringKind::Verbatim);
        // In a verbatim string a "" sequence is an escape. Otherwise this terminates us.
        if (_lexer->TextWindow.PeekChar(1) != u'"') {
            return true;
        }

        // Was just escaped content.  Consume it.
        _lexer->TextWindow.AdvanceChar(2);  // ""
    } else {
        CS_ASSERT(kind == InterpolatedStringKind::SingleLineRaw || kind == InterpolatedStringKind::MultiLineRaw);

        int beforeQuotePosition = _lexer->TextWindow.Position();
        int currentQuoteCount = _lexer->ConsumeQuoteSequence();
        if (currentQuoteCount >= startingQuoteCount) {
            // we saw a long enough sequence of close quotes to finish us.  Move back to before the close quotes
            // and let the caller handle this (including error-ing if there are too many close quotes, or if the
            // close quotes are in the wrong location).
            _lexer->TextWindow.Reset(beforeQuotePosition);
            return true;
        }
    }

    // otherwise, these were just quotes that we should treat as raw content.
    return false;
}

void Lexer::InterpolatedOrRawStringScanner::HandleCloseBraceInContent(InterpolatedStringKind kind,
                                                                      int startingDollarSignCount) {
    if (kind == InterpolatedStringKind::Normal || kind == InterpolatedStringKind::Verbatim) {
        int pos = _lexer->TextWindow.Position();
        _lexer->TextWindow.AdvanceChar();  // }

        // ensure any } characters are doubled up
        if (_lexer->TextWindow.PeekChar() == u'}') {
            _lexer->TextWindow.AdvanceChar();  // }
        } else {
            TrySetError(_lexer->MakeError(pos, 1, ErrorCode::ERR_UnescapedCurly, {u"}"}));
        }
    } else {
        CS_ASSERT(kind == InterpolatedStringKind::MultiLineRaw || kind == InterpolatedStringKind::SingleLineRaw);

        // A close quote is normally fine as content in a raw interpolated string literal. However, similar
        // to the rules around quotes, we do not allow a subsequence of braces to be longer than the number
        // of `$`s the literal starts with.  Note: this restriction is only on *content*.  It acceptable to
        // have a sequence of braces be longer, as long as it is part content and also part of an
        // interpolation.  In that case, the content portion must abide by this rule.
        int closeBraceCount = _lexer->ConsumeCloseBraceSequence();
        if (closeBraceCount >= startingDollarSignCount) {
            TrySetError(_lexer->MakeError(/*position:*/ _lexer->TextWindow.Position() - closeBraceCount,
                                          /*width:*/ closeBraceCount, ErrorCode::ERR_TooManyCloseBracesForRawString));
        }
    }
}

void Lexer::InterpolatedOrRawStringScanner::HandleOpenBraceInContent(InterpolatedStringKind kind,
                                                                     int startingDollarSignCount,
                                                                     std::vector<Interpolation> *interpolations) {
    if (kind == InterpolatedStringKind::Normal || kind == InterpolatedStringKind::Verbatim) {
        HandleOpenBraceInNormalOrVerbatimContent(kind, interpolations);
    } else {
        HandleOpenBraceInRawContent(kind, startingDollarSignCount, interpolations);
    }
}

void Lexer::InterpolatedOrRawStringScanner::HandleOpenBraceInNormalOrVerbatimContent(
    InterpolatedStringKind kind, std::vector<Interpolation> *interpolations) {
    CS_ASSERT(kind == InterpolatedStringKind::Normal || kind == InterpolatedStringKind::Verbatim);
    if (_lexer->TextWindow.PeekChar(1) == u'{') {
        _lexer->TextWindow.AdvanceChar(2);  // {{
    } else {
        int openBracePosition = _lexer->TextWindow.Position();
        _lexer->TextWindow.AdvanceChar();
        TextRange colonRange;
        ScanInterpolatedStringLiteralHoleBalancedText(kind, u'}', /*isHole:*/ true, colonRange);
        int closeBracePosition = _lexer->TextWindow.Position();
        if (_lexer->TextWindow.PeekChar() == u'}') {
            _lexer->TextWindow.AdvanceChar();
        } else {
            TrySetError(_lexer->MakeError(openBracePosition - 1, 2, ErrorCode::ERR_UnclosedExpressionHole));
        }

        if (interpolations != nullptr) {
            interpolations->push_back(Interpolation{TextRange{openBracePosition, openBracePosition + 1}, colonRange,
                                                    TextRange{closeBracePosition, _lexer->TextWindow.Position()}});
        }
    }
}

void Lexer::InterpolatedOrRawStringScanner::HandleOpenBraceInRawContent(InterpolatedStringKind kind,
                                                                        int startingDollarSignCount,
                                                                        std::vector<Interpolation> *interpolations) {
    CS_ASSERT(kind == InterpolatedStringKind::SingleLineRaw || kind == InterpolatedStringKind::MultiLineRaw);

    // In raw content we are allowed to see up to 2*N-1 open (or close) braces.  For example, if the string
    // literal starts with `$$$"""` then we can see up to `2*3-1 = 5` braces like so `$$$""" {{{{{`.  The
    // inner three braces start the interpolation.  The outer two braces are just content.  This ensures the
    // rule that the content cannot contain a sequence of open or close braces equal to (or longer than) the
    // dollar sequence.
    int beforeOpenBracesPosition = _lexer->TextWindow.Position();
    int openBraceCount = _lexer->ConsumeOpenBraceSequence();
    if (openBraceCount < startingDollarSignCount) {
        // not enough open braces to matter.  Just treat as content.
        return;
    }

    int afterOpenBracePosition = _lexer->TextWindow.Position();
    if (openBraceCount >= 2 * startingDollarSignCount) {
        // Too many open braces.  Report an error on the portion up before the section that counts as the
        // start of the interpolation.
        TrySetError(_lexer->MakeError(beforeOpenBracesPosition, /*width:*/ openBraceCount - startingDollarSignCount,
                                      ErrorCode::ERR_TooManyOpenBracesForRawString));
    }

    // Now, try to scan the contents of the interpolation.  Ending when we hit a close brace.
    TextRange colonRange;
    ScanInterpolatedStringLiteralHoleBalancedText(kind, u'}', /*isHole:*/ true, colonRange);

    int beforeCloseBracePosition = _lexer->TextWindow.Position();
    int closeBraceCount = _lexer->ConsumeCloseBraceSequence();

    if (closeBraceCount == 0) {
        // Didn't find any close braces.  Report a particular error on the open braces that they are unclosed.
        TrySetError(_lexer->MakeError(/*position:*/ afterOpenBracePosition - startingDollarSignCount,
                                      /*width:*/ startingDollarSignCount, ErrorCode::ERR_UnclosedExpressionHole));
    } else if (closeBraceCount < startingDollarSignCount) {
        // not enough close braces to end the interpolation.  Report here.
        TrySetError(_lexer->MakeError(beforeOpenBracesPosition, /*width:*/ openBraceCount - startingDollarSignCount,
                                      ErrorCode::ERR_NotEnoughCloseBracesForRawString));
    } else {
        // Only consume up to the minimum number of close braces we need to end the interpolation. Any
        // excess will be consumed in the content consumption pass in ScanInterpolatedStringLiteralContents.
        _lexer->TextWindow.Reset(beforeCloseBracePosition + startingDollarSignCount);
    }

    if (interpolations != nullptr) {
        interpolations->push_back(Interpolation{TextRange{afterOpenBracePosition - startingDollarSignCount,
                                                          afterOpenBracePosition},
                                                colonRange,
                                                TextRange{beforeCloseBracePosition, _lexer->TextWindow.Position()}});
    }
}

void Lexer::InterpolatedOrRawStringScanner::ScanFormatSpecifier(InterpolatedStringKind kind) {
    /*
    ## Grammar from spec:

    interpolation_format
        : ':' interpolation_format_character+
    ;
    interpolation_format_character
        : '<Any character except \" (U+0022), : (U+003A), { (U+007B) and } (U+007D)>'
    ;
     */

    CS_ASSERT(_lexer->TextWindow.PeekChar() == u':');
    _lexer->TextWindow.AdvanceChar();
    while (true) {
        char16_t ch = _lexer->TextWindow.PeekChar();
        if (ch == u'\\' && kind == InterpolatedStringKind::Normal) {
            // normal string & char constants can have escapes
            int pos = _lexer->TextWindow.Position();
            char16_t discarded;
            ch = _lexer->ScanEscapeSequence(/*surrogateCharacter:*/ discarded);
            if (ch == u'{' || ch == u'}') {
                TrySetError(_lexer->MakeError(pos, 1, ErrorCode::ERR_EscapedCurly, {CharText(ch)}));
            }
        } else if (ch == u'"') {
            if (kind == InterpolatedStringKind::Verbatim && _lexer->TextWindow.PeekChar(1) == u'"') {
                _lexer->TextWindow.AdvanceChar(2);  // ""
            } else {
                return;  // premature end of string! let caller complain about unclosed interpolation
            }
        } else if (ch == u'{') {
            TrySetError(_lexer->MakeError(_lexer->TextWindow.Position(), 1, ErrorCode::ERR_UnexpectedCharacter,
                                          {CharText(ch)}));
            _lexer->TextWindow.AdvanceChar();
        } else if (ch == u'}') {
            return;  // end of interpolation
        } else if (IsAtEnd(/*allowNewline:*/ true)) {
            return;  // premature end; let caller complain
        } else {
            _lexer->TextWindow.AdvanceChar();
        }
    }
}

/// Scan past the hole inside an interpolated string literal, leaving the current character on the '}' (if any)
void Lexer::InterpolatedOrRawStringScanner::ScanInterpolatedStringLiteralHoleBalancedText(InterpolatedStringKind kind,
                                                                                         char16_t endingChar,
                                                                                         bool isHole,
                                                                                         TextRange &colonRange) {
    colonRange = TextRange{};
    while (true) {
        char16_t ch = _lexer->TextWindow.PeekChar();

        // Note: within a hole newlines are always allowed.  The restriction on if newlines are allowed or not
        // is only within a text-portion of the interpolated string.
        if (IsAtEnd(/*allowNewline:*/ true)) {
            // the caller will complain
            return;
        }

        switch (ch) {
            case u'#':
                // preprocessor directives not allowed.
                TrySetError(_lexer->MakeError(_lexer->TextWindow.Position(), 1, ErrorCode::ERR_SyntaxError,
                                              {CharText(endingChar)}));
                _lexer->TextWindow.AdvanceChar();
                continue;
            case u'$': {
                TokenInfo discarded{};
                if (_lexer->TryScanInterpolatedString(discarded)) {
                    continue;
                }

                goto default_;
            }
            case u':':
                // the first colon not nested within matching delimiters is the start of the format string
                if (isHole) {
                    CS_ASSERT(colonRange.start == 0 && colonRange.end == 0);
                    colonRange = TextRange{_lexer->TextWindow.Position(), _lexer->TextWindow.Position() + 1};
                    ScanFormatSpecifier(kind);
                    return;
                }

                goto default_;
            case u'}':
            case u')':
            case u']':
                if (ch == endingChar) {
                    return;
                }

                TrySetError(_lexer->MakeError(_lexer->TextWindow.Position(), 1, ErrorCode::ERR_SyntaxError,
                                              {CharText(endingChar)}));
                goto default_;
            case u'"':
                if (RecoveringFromRunawayLexing()) {
                    // When recovering from mismatched delimiters, we consume the next
                    // quote character as the close quote for the interpolated string. In
                    // practice this gets us out of trouble in scenarios we've encountered.
                    // See, for example, https://github.com/dotnet/roslyn/issues/44789
                    return;
                }

                // handle string literal inside an expression hole.
                ScanInterpolatedStringLiteralNestedString();
                continue;

            case u'\'':
                // handle character literal inside an expression hole.
                ScanInterpolatedStringLiteralNestedString();
                continue;
            case u'@': {
                TokenInfo discarded{};
                if (_lexer->TryScanAtStringToken(discarded)) continue;

                if (_lexer->TextWindow.PeekChar(1) == u'*') {
                    // Razor comment. Handle for error recovery purposes. The parser will come along later to retokenize the inside of the string
                    // and report a proper error.
                    bool isTerminated;
                    _lexer->ScanMultiLineComment(isTerminated, /*delimiter:*/ u'@');
                    continue;
                }

                // Wasn't an @"" or @$"" string.  Just consume this as normal code.
                goto default_;
            }
            case u'/':
                switch (_lexer->TextWindow.PeekChar(1)) {
                    case u'/':
                        _lexer->ScanToEndOfLine();
                        continue;
                    case u'*': {
                        bool isTerminated;
                        _lexer->ScanMultiLineComment(isTerminated, u'/');
                        continue;
                    }
                    default:
                        _lexer->TextWindow.AdvanceChar();
                        continue;
                }
            case u'{':
                // TODO: after the colon this has no special meaning.
                ScanInterpolatedStringLiteralHoleBracketed(kind, u'{', u'}');
                continue;
            case u'(':
                // TODO: after the colon this has no special meaning.
                ScanInterpolatedStringLiteralHoleBracketed(kind, u'(', u')');
                continue;
            case u'[':
                // TODO: after the colon this has no special meaning.
                ScanInterpolatedStringLiteralHoleBracketed(kind, u'[', u']');
                continue;
            default:
                goto default_;
        }

    default_:
        // part of code in the expression hole
        _lexer->TextWindow.AdvanceChar();
        continue;
    }
}

void Lexer::InterpolatedOrRawStringScanner::ScanInterpolatedStringLiteralNestedString() {
    TokenInfo info{};
    _lexer->ScanStringLiteral(info, /*inDirective:*/ false);
}

void Lexer::InterpolatedOrRawStringScanner::ScanInterpolatedStringLiteralHoleBracketed(InterpolatedStringKind kind,
                                                                                      char16_t start, char16_t end) {
    CS_ASSERT(start == _lexer->TextWindow.PeekChar());
    (void)start;
    _lexer->TextWindow.AdvanceChar();
    TextRange colonRange;
    ScanInterpolatedStringLiteralHoleBalancedText(kind, end, /*isHole:*/ false, /*colonRange:*/ colonRange);
    if (_lexer->TextWindow.PeekChar() == end) {
        _lexer->TextWindow.AdvanceChar();
    } else {
        // an error was given by the caller
    }
}

// --- Lexer_RawStringLiteral.cs ------------------------------------------------------------------------------------

/// Returns the number of characters that were consumed.
int Lexer::ConsumeCharSequence(char16_t ch) {
    int start = TextWindow.Position();
    while (TextWindow.PeekChar() == ch) TextWindow.AdvanceChar();

    return TextWindow.Position() - start;
}

void Lexer::ConsumeWhitespace() {
    while (true) {
        char16_t ch = TextWindow.PeekChar();
        if (!SyntaxFacts::IsWhitespace(ch)) break;

        TextWindow.AdvanceChar();
    }
}

void Lexer::ScanRawStringLiteral(TokenInfo &info, bool inDirective) {
    _builder.clear();

    int startingQuoteCount = ConsumeQuoteSequence();

    CS_ASSERT(startingQuoteCount >= 3);

    // Keep consuming whitespace after the initial quote sequence.
    ConsumeWhitespace();

    if (SyntaxFacts::IsNewLine(TextWindow.PeekChar())) {
        // Past the initial whitespace, and we hit a newline, this is a multi line raw string literal.
        ScanMultiLineRawStringLiteral(info, startingQuoteCount);
    } else {
        // Past the initial whitespace, and we hit anything else, this is a single line raw string literal.
        ScanSingleLineRawStringLiteral(info, startingQuoteCount);
    }

    CS_ASSERT(info.Kind == SyntaxKind::SingleLineRawStringLiteralToken ||
              info.Kind == SyntaxKind::MultiLineRawStringLiteralToken);

    if (!inDirective && ScanUtf8Suffix()) {
        switch (info.Kind) {
            case SyntaxKind::SingleLineRawStringLiteralToken:
                info.Kind = SyntaxKind::Utf8SingleLineRawStringLiteralToken;
                break;

            case SyntaxKind::MultiLineRawStringLiteralToken:
                info.Kind = SyntaxKind::Utf8MultiLineRawStringLiteralToken;
                break;

            default:
                CS_ASSERT(false);  // throw ExceptionUtilities.UnexpectedValue(info.Kind);
                break;
        }
    }

    // Note: we intentionally are not setting .StringValue for raw string literals.  That will be determined in
    // the parser later on in LanguageParser.ParseRawStringToken
    CS_ASSERT(!info.HasStringValue);

    // We should not have reported any errors for this raw string literal.  Any errors should be deferred to the parser.
    CS_ASSERT(!HasErrors());
    info.Text_ = GetInternedLexemeText();
    info.HasText = true;
}

void Lexer::ScanSingleLineRawStringLiteral(TokenInfo &info, int startingQuoteCount) {
    info.Kind = SyntaxKind::SingleLineRawStringLiteralToken;

    while (true) {
        char16_t currentChar = TextWindow.PeekChar();

        // See if we reached the end of the line or file before hitting the end. Errors about this will be
        // reported by the parser.
        if (SyntaxFacts::IsNewLine(currentChar) || IsAtEndOfText(currentChar)) return;

        if (currentChar != u'"') {
            // anything not a quote sequence just moves it forward.
            TextWindow.AdvanceChar();
            continue;
        }

        int currentQuoteCount = ConsumeQuoteSequence();

        // A raw string literal starting with some number of quotes can contain a quote sequence with fewer quotes.
        if (currentQuoteCount < startingQuoteCount) continue;

        // We have enough quotes to finish this string at this point.  Errors about excess quotes will be
        // reported in the parser.
        return;
    }
}

void Lexer::ScanMultiLineRawStringLiteral(TokenInfo &info, int startingQuoteCount) {
    info.Kind = SyntaxKind::MultiLineRawStringLiteralToken;

    auto scanMultiLineRawStringLiteralLine = [this](int startingQuoteCount_) -> bool {
        TextWindow.AdvancePastNewLine();

        ConsumeWhitespace();

        // After the whitespace see if this is the line that ends the multiline literal.  If so we're done scanning
        // lines.  Errors about this will be reported by the parser.
        if (ConsumeQuoteSequence() >= startingQuoteCount_) return false;

        // We're not on the terminating line. Consume a normal content line.  Eat to the end of line (or file in the
        // case of errors).
        while (true) {
            char16_t currentChar = TextWindow.PeekChar();

            // Check if we have an unterminated raw string. Errors about this will be reported by the parser.
            if (IsAtEndOfText(currentChar)) return false;

            if (SyntaxFacts::IsNewLine(currentChar)) return true;

            if (currentChar == u'"') {
                // Don't allow a content line to contain a quote sequence that looks like a delimiter (or longer).
                // Errors about this will be reported by the parser.
                if (ConsumeQuoteSequence() >= startingQuoteCount_) return false;
            } else {
                TextWindow.AdvanceChar();
            }
        }
    };

    CS_ASSERT(SyntaxFacts::IsNewLine(TextWindow.PeekChar()));
    while (scanMultiLineRawStringLiteralLine(startingQuoteCount)) {
    }
}

}  // namespace csharp
