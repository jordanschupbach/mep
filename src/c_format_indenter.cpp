// The continuation indenter of mep's C formatter: the port of clang-format's
// ContinuationIndenter, which decides, token by token, where a line that does
// not fit is broken and at which column each continuation starts; plus the
// parts of BreakableToken that reflow comments and split long string
// literals, and the CommaSeparatedList role that lays a long braced list out
// in columns.

#include "c_format_internal.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <limits>
#include <optional>

namespace cfmt::detail {

bool ParenState::operator<(const ParenState &Other) const {
    if (Indent != Other.Indent) return Indent < Other.Indent;
    if (LastSpace != Other.LastSpace) return LastSpace < Other.LastSpace;
    if (NestedBlockIndent != Other.NestedBlockIndent) return NestedBlockIndent < Other.NestedBlockIndent;
    if (FirstLessLess != Other.FirstLessLess) return FirstLessLess < Other.FirstLessLess;
    if (IsAligned != Other.IsAligned) return IsAligned;
    if (BreakBeforeClosingBrace != Other.BreakBeforeClosingBrace) return BreakBeforeClosingBrace;
    if (QuestionColumn != Other.QuestionColumn) return QuestionColumn < Other.QuestionColumn;
    if (AvoidBinPacking != Other.AvoidBinPacking) return AvoidBinPacking;
    if (BreakBeforeParameter != Other.BreakBeforeParameter) return BreakBeforeParameter;
    if (NoLineBreak != Other.NoLineBreak) return NoLineBreak;
    if (LastOperatorWrapped != Other.LastOperatorWrapped) return LastOperatorWrapped;
    if (ColonPos != Other.ColonPos) return ColonPos < Other.ColonPos;
    if (StartOfFunctionCall != Other.StartOfFunctionCall) return StartOfFunctionCall < Other.StartOfFunctionCall;
    if (StartOfArraySubscripts != Other.StartOfArraySubscripts)
        return StartOfArraySubscripts < Other.StartOfArraySubscripts;
    if (CallContinuation != Other.CallContinuation) return CallContinuation < Other.CallContinuation;
    if (VariablePos != Other.VariablePos) return VariablePos < Other.VariablePos;
    if (ContainsLineBreak != Other.ContainsLineBreak) return ContainsLineBreak;
    if (ContainsUnwrappedBuilder != Other.ContainsUnwrappedBuilder) return ContainsUnwrappedBuilder;
    if (NestedBlockInlined != Other.NestedBlockInlined) return NestedBlockInlined;
    if (IsChainedConditional != Other.IsChainedConditional) return IsChainedConditional;
    if (IsWrappedConditional != Other.IsWrappedConditional) return IsWrappedConditional;
    if (UnindentOperator != Other.UnindentOperator) return UnindentOperator;
    return false;
}

bool LineState::operator<(const LineState &Other) const {
    if (NextToken != Other.NextToken) return NextToken < Other.NextToken;
    if (Column != Other.Column) return Column < Other.Column;
    if (NoContinuation != Other.NoContinuation) return NoContinuation;
    if (StartOfLineLevel != Other.StartOfLineLevel) return StartOfLineLevel < Other.StartOfLineLevel;
    if (LowestLevelOnLine != Other.LowestLevelOnLine) return LowestLevelOnLine < Other.LowestLevelOnLine;
    if (StartOfStringLiteral != Other.StartOfStringLiteral) return StartOfStringLiteral < Other.StartOfStringLiteral;
    if (IgnoreStackForComparison || Other.IgnoreStackForComparison) return false;
    return Stack < Other.Stack;
}

namespace {

constexpr std::string_view Blanks = " \t\v\f\r";

// LLVM's StringRef::find_last_of / find_last_not_of take `From` as an
// exclusive bound (they search [0, From)); std::string_view's is inclusive.
size_t llvmFindLastOf(std::string_view S, std::string_view Chars, size_t From) {
    if (From == 0) return std::string_view::npos;
    return S.find_last_of(Chars, From - 1);
}

size_t llvmFindLastNotOf(std::string_view S, std::string_view Chars, size_t From) {
    if (From == 0) return std::string_view::npos;
    return S.find_last_not_of(Chars, From - 1);
}

bool isBlank(char C) { return Blanks.find(C) != std::string_view::npos; }

std::string_view rtrimBlanks(std::string_view S) {
    size_t E = S.find_last_not_of(Blanks);
    return E == std::string_view::npos ? S.substr(0, 0) : S.substr(0, E + 1);
}

std::string_view ltrimBlanks(std::string_view S) {
    size_t B = S.find_first_not_of(Blanks);
    return B == std::string_view::npos ? S.substr(S.size()) : S.substr(B);
}

std::string_view trimBlanks(std::string_view S) { return rtrimBlanks(ltrimBlanks(S)); }

bool isPunctuation(char C) {
    unsigned char U = static_cast<unsigned char>(C);
    return U < 0x80 && std::ispunct(U);
}

std::vector<std::string_view> splitLines(std::string_view Text, std::string_view Sep) {
    std::vector<std::string_view> Out;
    size_t Start = 0;
    for (;;) {
        size_t Pos = Text.find(Sep, Start);
        if (Pos == std::string_view::npos) {
            Out.push_back(Text.substr(Start));
            return Out;
        }
        Out.push_back(Text.substr(Start, Pos - Start));
        Start = Pos + Sep.size();
    }
}

unsigned getLengthToMatchingParen(const FormatToken &Tok, const std::vector<ParenState> &Stack) {
    if (!Tok.MatchingParen) return 0;
    FormatToken *End = Tok.MatchingParen;
    int MatchingStackIndex = static_cast<int>(Stack.size()) - 1;
    auto FindParenState = [&](const FormatToken *LBrace) -> const ParenState * {
        while (MatchingStackIndex >= 0 && Stack[static_cast<size_t>(MatchingStackIndex)].Tok != LBrace) --MatchingStackIndex;
        return MatchingStackIndex >= 0 ? &Stack[static_cast<size_t>(MatchingStackIndex)] : nullptr;
    };
    for (; End->Next; End = End->Next) {
        if (End->Next->CanBreakBefore) break;
        if (!End->Next->closesScope()) continue;
        if (End->Next->MatchingParen &&
            End->Next->MatchingParen->isOneOf(tok::l_brace, TT::ArrayInitializerLSquare, tok::less)) {
            const ParenState *State = FindParenState(End->Next->MatchingParen);
            if (State && State->BreakBeforeClosingBrace) break;
        }
    }
    return End->TotalLength - Tok.TotalLength + 1;
}

unsigned getLengthToNextOperator(const FormatToken &Tok) {
    if (!Tok.NextOperator) return 0;
    return Tok.NextOperator->TotalLength - Tok.TotalLength;
}

bool startsSegmentOfBuilderTypeCall(const FormatToken &Tok) {
    return Tok.isMemberAccess() && Tok.Previous && Tok.Previous->closesScope();
}

bool shouldUnindentNextOperator(const FormatToken &Tok) {
    const FormatToken *Previous = Tok.getPreviousNonComment();
    return Previous && (Previous->getPrecedence() == prec::Assignment || Previous->is(tok::kw_return));
}

bool hasNestedBlockInlined(const FormatToken *Previous) {
    if (Previous->isNot(tok::l_paren)) return true;
    if (Previous->ParameterCount > 1) return true;
    return false;
}

// --- Breakable tokens -------------------------------------------------------------------

using Split = std::pair<size_t, unsigned>;
constexpr size_t npos = std::string_view::npos;

std::string_view getLineCommentIndentPrefix(std::string_view Comment) {
    static constexpr std::string_view KnownPrefixes[] = {"///<", "//!<", "///", "//!", "//:", "//"};
    for (std::string_view KnownPrefix : KnownPrefixes) {
        if (Comment.substr(0, KnownPrefix.size()) == KnownPrefix) {
            size_t PrefixLength = Comment.find_first_not_of(' ', KnownPrefix.size());
            return Comment.substr(0, PrefixLength == npos ? Comment.size() : PrefixLength);
        }
    }
    return {};
}

bool matchesNumberedList(std::string_view S, bool RequireSpace) {
    // ^[1-9][0-9]?\.( )?
    if (S.empty() || S[0] < '1' || S[0] > '9') return false;
    size_t I = 1;
    if (I < S.size() && S[I] >= '0' && S[I] <= '9') ++I;
    if (I >= S.size() || S[I] != '.') return false;
    ++I;
    if (RequireSpace) return I < S.size() && S[I] == ' ';
    return true;
}

Split getCommentSplit(std::string_view Text, unsigned ContentStartColumn, unsigned ColumnLimit, unsigned TabWidth,
                      bool DecorationEndsWithStar = false) {
    if (ColumnLimit <= ContentStartColumn + 1) return Split(npos, 0);
    unsigned MaxSplit = ColumnLimit - ContentStartColumn + 1;
    size_t MaxSplitBytes = 0;
    for (unsigned NumChars = 0; NumChars < MaxSplit && MaxSplitBytes < Text.size();) {
        unsigned BytesInChar = getCodePointNumBytes(Text[MaxSplitBytes]);
        NumChars += columnWidthWithTabs(Text.substr(MaxSplitBytes, BytesInChar), ContentStartColumn + NumChars, TabWidth);
        MaxSplitBytes += BytesInChar;
    }
    size_t SpaceOffset = llvmFindLastOf(Text, Blanks, MaxSplitBytes);
    while (SpaceOffset != npos) {
        size_t LastNonBlank = llvmFindLastNotOf(Text, Blanks, SpaceOffset);
        if (LastNonBlank != npos && Text[LastNonBlank] == '\\') {
            SpaceOffset = llvmFindLastOf(Text, Blanks, LastNonBlank);
            continue;
        }
        if (matchesNumberedList(ltrimBlanks(Text.substr(SpaceOffset)), false)) {
            SpaceOffset = llvmFindLastOf(Text, Blanks, SpaceOffset);
            continue;
        }
        break;
    }
    if (SpaceOffset == npos || llvmFindLastNotOf(Text, Blanks, SpaceOffset) == npos) {
        size_t FirstNonWhitespace = Text.find_first_not_of(Blanks);
        if (FirstNonWhitespace == npos) return Split(npos, 0);
        SpaceOffset = Text.find_first_of(Blanks, std::max<size_t>(MaxSplitBytes, FirstNonWhitespace));
    }
    if (SpaceOffset != npos && SpaceOffset != 0) {
        if (SpaceOffset == 1 && Text[SpaceOffset - 1] == '*') return Split(npos, 0);
        std::string_view BeforeCut = rtrimBlanks(Text.substr(0, SpaceOffset));
        std::string_view AfterCut = Text.substr(SpaceOffset);
        if (!DecorationEndsWithStar) AfterCut = ltrimBlanks(AfterCut);
        return Split(BeforeCut.size(), static_cast<unsigned>(AfterCut.data() - (BeforeCut.data() + BeforeCut.size())));
    }
    return Split(npos, 0);
}

Split getStringSplit(std::string_view Text, unsigned UsedColumns, unsigned ColumnLimit, unsigned TabWidth) {
    if (Text.empty()) return Split(npos, 0);
    if (ColumnLimit <= UsedColumns) return Split(npos, 0);
    unsigned MaxSplit = ColumnLimit - UsedColumns;
    size_t SpaceOffset = 0, SlashOffset = 0, WordStartOffset = 0, SplitPoint = 0;
    for (unsigned Chars = 0;;) {
        unsigned Advance;
        if (Text[0] == '\\') {
            Advance = getEscapeSequenceLength(Text);
            Chars += Advance;
        } else {
            Advance = getCodePointNumBytes(Text[0]);
            Chars += columnWidthWithTabs(Text.substr(0, Advance), UsedColumns + Chars, TabWidth);
        }
        if (Chars > MaxSplit || Text.size() <= Advance) break;
        if (isBlank(Text[0])) SpaceOffset = SplitPoint;
        if (Text[0] == '/') SlashOffset = SplitPoint;
        if (Advance == 1 && !std::isalnum(static_cast<unsigned char>(Text[0]))) WordStartOffset = SplitPoint;
        SplitPoint += Advance;
        Text = Text.substr(Advance);
    }
    if (SpaceOffset != 0) return Split(SpaceOffset + 1, 0);
    if (SlashOffset != 0) return Split(SlashOffset + 1, 0);
    if (WordStartOffset != 0) return Split(WordStartOffset + 1, 0);
    if (SplitPoint != 0) return Split(SplitPoint, 0);
    return Split(npos, 0);
}

bool mayReflowContent(std::string_view Content) {
    Content = trimBlanks(Content);
    bool hasSpecialMeaningPrefix = false;
    for (std::string_view Prefix : {"@", "\\", "TODO", "FIXME", "XXX", "-# ", "- ", "+ ", "* "}) {
        if (Content.substr(0, Prefix.size()) == Prefix) {
            hasSpecialMeaningPrefix = true;
            break;
        }
    }
    hasSpecialMeaningPrefix = hasSpecialMeaningPrefix || matchesNumberedList(Content, true);
    return Content.size() >= 2 && !hasSpecialMeaningPrefix && Content.back() != '\\' &&
           (!isPunctuation(Content[0]) || !isPunctuation(Content[1]));
}

bool matchesCommentPragma(std::string_view Content) { return Content.substr(0, 13) == " IWYU pragma:"; }

class BreakableToken {
public:
    virtual ~BreakableToken() = default;
    virtual unsigned getLineCount() const = 0;
    virtual unsigned getRangeLength(unsigned LineIndex, unsigned Offset, size_t Length, unsigned StartColumn) const = 0;
    virtual unsigned getRemainingLength(unsigned LineIndex, unsigned Offset, unsigned StartColumn) const = 0;
    virtual unsigned getContentStartColumn(unsigned LineIndex, bool Break) const = 0;
    virtual unsigned getContentIndent(unsigned) const { return 0; }
    virtual Split getSplit(unsigned LineIndex, unsigned TailOffset, unsigned ColumnLimit,
                           unsigned ContentStartColumn) const = 0;
    virtual void insertBreak(unsigned LineIndex, unsigned TailOffset, Split S, unsigned ContentIndent,
                             WhitespaceManager &Whitespaces) const = 0;
    unsigned getLengthAfterCompression(unsigned RemainingTokenColumns, Split Sp) const {
        return RemainingTokenColumns + 1 - Sp.second;
    }
    virtual void compressWhitespace(unsigned, unsigned, Split, WhitespaceManager &) const {}
    virtual bool supportsReflow() const { return false; }
    virtual Split getReflowSplit(unsigned) const { return Split(npos, 0); }
    virtual void reflow(unsigned, WhitespaceManager &) const {}
    virtual bool introducesBreakBeforeToken() const { return false; }
    virtual void adaptStartOfLine(unsigned, WhitespaceManager &) const {}
    virtual Split getSplitAfterLastLine(unsigned) const { return Split(npos, 0); }
    virtual void replaceWhitespaceAfterLastLine(unsigned, Split, WhitespaceManager &) const {}
    virtual void updateNextToken(LineState &) const {}
    virtual void updateAfterBroken(WhitespaceManager &) const {}

protected:
    BreakableToken(const FormatToken &Tok, bool InPPDirective, const Style &S)
        : Tok(Tok), InPPDirective(InPPDirective), S(S) {}
    const FormatToken &Tok;
    const bool InPPDirective;
    const Style &S;
};

class BreakableStringLiteral : public BreakableToken {
public:
    BreakableStringLiteral(const FormatToken &Tok, unsigned StartColumn, std::string_view Prefix,
                           std::string_view Postfix, unsigned UnbreakableTailLength, bool InPPDirective,
                           const Style &S)
        : BreakableToken(Tok, InPPDirective, S), StartColumn(StartColumn), Prefix(Prefix), Postfix(Postfix),
          UnbreakableTailLength(UnbreakableTailLength) {
        Line = Tok.TokenText.substr(Prefix.size(), Tok.TokenText.size() - Prefix.size() - Postfix.size());
    }
    unsigned getLineCount() const override { return 1; }
    unsigned getRangeLength(unsigned, unsigned, size_t, unsigned) const override { return 0; }
    unsigned getRemainingLength(unsigned, unsigned Offset, unsigned StartCol) const override {
        return UnbreakableTailLength + static_cast<unsigned>(Postfix.size()) +
               columnWidthWithTabs(Line.substr(Offset), StartCol, S.TabWidth);
    }
    unsigned getContentStartColumn(unsigned, bool) const override {
        return StartColumn + static_cast<unsigned>(Prefix.size());
    }
    Split getSplit(unsigned, unsigned TailOffset, unsigned ColumnLimit, unsigned ContentStartColumn) const override {
        return getStringSplit(Line.substr(TailOffset), ContentStartColumn,
                              ColumnLimit - static_cast<unsigned>(Postfix.size()), S.TabWidth);
    }
    void insertBreak(unsigned, unsigned TailOffset, Split Sp, unsigned, WhitespaceManager &Whitespaces) const override {
        Whitespaces.replaceWhitespaceInToken(Tok, static_cast<unsigned>(Prefix.size() + TailOffset + Sp.first),
                                             Sp.second, Postfix, Prefix, InPPDirective, 1, static_cast<int>(StartColumn));
    }

private:
    unsigned StartColumn;
    std::string_view Prefix;
    std::string_view Postfix;
    std::string_view Line;
    unsigned UnbreakableTailLength;
};

class BreakableComment : public BreakableToken {
public:
    unsigned getLineCount() const override { return static_cast<unsigned>(Lines.size()); }
    Split getSplit(unsigned LineIndex, unsigned TailOffset, unsigned ColumnLimit,
                   unsigned ContentStartColumn) const override {
        if (!S.ReflowComments || matchesCommentPragma(Content[LineIndex])) return Split(npos, 0);
        return getCommentSplit(Content[LineIndex].substr(TailOffset), ContentStartColumn, ColumnLimit, S.TabWidth);
    }
    void compressWhitespace(unsigned LineIndex, unsigned TailOffset, Split Sp,
                            WhitespaceManager &Whitespaces) const override {
        std::string_view Text = Content[LineIndex].substr(TailOffset);
        unsigned BreakOffsetInToken =
            static_cast<unsigned>(static_cast<size_t>(Text.data() - tokenAt(LineIndex).TokenText.data()) + Sp.first);
        Whitespaces.replaceWhitespaceInToken(tokenAt(LineIndex), BreakOffsetInToken, Sp.second, "", "",
                                             /*InPPDirective=*/false, 0, 1);
    }
    bool supportsReflow() const override { return true; }

protected:
    BreakableComment(const FormatToken &Token, unsigned StartColumn, bool InPPDirective, const Style &S)
        : BreakableToken(Token, InPPDirective, S), StartColumn(StartColumn) {}
    const FormatToken &tokenAt(unsigned LineIndex) const { return Tokens[LineIndex] ? *Tokens[LineIndex] : Tok; }

    std::vector<std::string_view> Lines;
    std::vector<std::string_view> Content;
    std::vector<const FormatToken *> Tokens;
    std::vector<int> ContentColumn;
    unsigned StartColumn;
    std::string_view ReflowPrefix = " ";
};

class BreakableBlockComment : public BreakableComment {
public:
    BreakableBlockComment(const FormatToken &Token, unsigned StartCol, unsigned OriginalStartColumn, bool FirstInLine,
                          bool InPPDirective, const Style &S, bool UseCRLF)
        : BreakableComment(Token, StartCol, InPPDirective, S), UnbreakableTailLength(Token.UnbreakableTailLength) {
        std::string_view TokenText(Tok.TokenText);
        Lines = splitLines(TokenText.substr(2, TokenText.size() - 4), UseCRLF ? "\r\n" : "\n");
        int IndentDelta = static_cast<int>(StartColumn) - static_cast<int>(OriginalStartColumn);
        Content.resize(Lines.size());
        Content[0] = Lines[0];
        ContentColumn.resize(Lines.size());
        ContentColumn[0] = static_cast<int>(StartColumn) + 2;
        Tokens.resize(Lines.size());
        for (size_t i = 1; i < Lines.size(); ++i) adjustWhitespace(static_cast<unsigned>(i), IndentDelta);
        DecorationColumn = StartColumn + 1;
        if (Lines.size() >= 2 && Content[1].substr(0, 2) == "**" &&
            static_cast<unsigned>(ContentColumn[1]) == StartColumn) {
            DecorationColumn = StartColumn;
        }
        Decoration = "* ";
        if (Lines.size() == 1 && !FirstInLine) Decoration = "";
        for (size_t i = 1, e = Content.size(); i < e && !Decoration.empty(); ++i) {
            const std::string_view &Text = Content[i];
            if (i + 1 == e) {
                if (Text.empty()) break;
            } else if (!Text.empty() && Decoration.substr(0, Text.size()) == Text) {
                continue;
            }
            while (Text.substr(0, Decoration.size()) != Decoration) Decoration.remove_suffix(1);
        }
        LastLineNeedsDecoration = true;
        IndentAtLineBreak = ContentColumn[0] + 1;
        for (size_t i = 1, e = Lines.size(); i < e; ++i) {
            if (Content[i].empty()) {
                if (i + 1 == e) {
                    LastLineNeedsDecoration = false;
                    if (e >= 2 && !Decoration.empty()) ContentColumn[i] = static_cast<int>(DecorationColumn);
                } else if (Decoration.empty()) {
                    ContentColumn[i] = 0;
                }
                continue;
            }
            size_t DecorationSize =
                Decoration.substr(0, Content[i].size()) == Content[i] ? Content[i].size() : Decoration.size();
            if (DecorationSize) ContentColumn[i] = static_cast<int>(DecorationColumn + DecorationSize);
            Content[i] = Content[i].substr(DecorationSize);
            if (Decoration.substr(0, Content[i].size()) != Content[i])
                IndentAtLineBreak = std::min<int>(IndentAtLineBreak, std::max(0, ContentColumn[i]));
        }
        IndentAtLineBreak = std::max<int>(IndentAtLineBreak, static_cast<int>(Decoration.size()));
    }

    Split getSplit(unsigned LineIndex, unsigned TailOffset, unsigned ColumnLimit,
                   unsigned ContentStartColumn) const override {
        if (!S.ReflowComments || matchesCommentPragma(Content[LineIndex])) return Split(npos, 0);
        return getCommentSplit(Content[LineIndex].substr(TailOffset), ContentStartColumn, ColumnLimit, S.TabWidth,
                               !Decoration.empty() && Decoration.back() == '*');
    }
    unsigned getRangeLength(unsigned LineIndex, unsigned Offset, size_t Length, unsigned StartCol) const override {
        return columnWidthWithTabs(Content[LineIndex].substr(Offset, Length), StartCol, S.TabWidth);
    }
    unsigned getRemainingLength(unsigned LineIndex, unsigned Offset, unsigned StartCol) const override {
        unsigned LineLength = UnbreakableTailLength + getRangeLength(LineIndex, Offset, npos, StartCol);
        if (LineIndex + 1 == Lines.size()) {
            LineLength += 2;
            bool HasRemainingText = Offset < Content[LineIndex].size();
            if (!HasRemainingText) {
                bool HasDecoration = ltrimWS(Lines[LineIndex]).substr(0, Decoration.size()) == Decoration;
                if (HasDecoration) LineLength -= static_cast<unsigned>(Decoration.size());
            }
        }
        return LineLength;
    }
    unsigned getContentStartColumn(unsigned LineIndex, bool Break) const override {
        if (Break) return static_cast<unsigned>(IndentAtLineBreak);
        return static_cast<unsigned>(std::max(0, ContentColumn[LineIndex]));
    }
    void insertBreak(unsigned LineIndex, unsigned TailOffset, Split Sp, unsigned ContentIndent,
                     WhitespaceManager &Whitespaces) const override {
        std::string_view Text = Content[LineIndex].substr(TailOffset);
        std::string_view Prefix = Decoration;
        unsigned LocalIndentAtLineBreak = static_cast<unsigned>(IndentAtLineBreak);
        if (LineIndex + 1 == Lines.size() && Text.size() == Sp.first + Sp.second) {
            Prefix = "";
            if (LocalIndentAtLineBreak >= 2) LocalIndentAtLineBreak -= 2;
        }
        unsigned BreakOffsetInToken =
            static_cast<unsigned>(static_cast<size_t>(Text.data() - tokenAt(LineIndex).TokenText.data()) + Sp.first);
        std::string PrefixWithTrailingIndent(Prefix);
        PrefixWithTrailingIndent.append(ContentIndent, ' ');
        Whitespaces.replaceWhitespaceInToken(
            tokenAt(LineIndex), BreakOffsetInToken, Sp.second, "", PrefixWithTrailingIndent, InPPDirective, 1,
            static_cast<int>(LocalIndentAtLineBreak + ContentIndent - PrefixWithTrailingIndent.size()));
    }
    Split getReflowSplit(unsigned LineIndex) const override {
        if (!mayReflow(LineIndex)) return Split(npos, 0);
        size_t Trimmed = Content[LineIndex].find_first_not_of(Blanks);
        return Split(0, Trimmed != npos ? static_cast<unsigned>(Trimmed) : 0);
    }
    void reflow(unsigned LineIndex, WhitespaceManager &Whitespaces) const override {
        std::string_view TrimmedContent = ltrimBlanks(Content[LineIndex]);
        unsigned WhitespaceOffsetInToken = static_cast<unsigned>(
            Content[LineIndex - 1].data() + Content[LineIndex - 1].size() - tokenAt(LineIndex).TokenText.data());
        unsigned WhitespaceLength = static_cast<unsigned>(TrimmedContent.data() - tokenAt(LineIndex).TokenText.data() -
                                                          WhitespaceOffsetInToken);
        Whitespaces.replaceWhitespaceInToken(tokenAt(LineIndex), WhitespaceOffsetInToken, WhitespaceLength, "",
                                             ReflowPrefix, InPPDirective, 0, 0);
    }
    void adaptStartOfLine(unsigned LineIndex, WhitespaceManager &Whitespaces) const override {
        if (LineIndex == 0) return;  // DelimitersOnNewline is JS/Java only
        std::string_view Prefix = Decoration;
        if (Content[LineIndex].empty()) {
            if (LineIndex + 1 == Lines.size()) {
                if (!LastLineNeedsDecoration) Prefix = "";
            } else if (!Decoration.empty()) {
                Prefix = Prefix.substr(0, 1);
            }
        } else if (ContentColumn[LineIndex] == 1) {
            Prefix = Prefix.substr(0, 1);
        }
        unsigned WhitespaceOffsetInToken = static_cast<unsigned>(
            Content[LineIndex - 1].data() + Content[LineIndex - 1].size() - tokenAt(LineIndex).TokenText.data());
        unsigned WhitespaceLength = static_cast<unsigned>(Content[LineIndex].data() -
                                                          tokenAt(LineIndex).TokenText.data() - WhitespaceOffsetInToken);
        Whitespaces.replaceWhitespaceInToken(tokenAt(LineIndex), WhitespaceOffsetInToken, WhitespaceLength, "", Prefix,
                                             InPPDirective, 1,
                                             ContentColumn[LineIndex] - static_cast<int>(Prefix.size()));
    }

private:
    static std::string_view ltrimWS(std::string_view S) {
        size_t B = S.find_first_not_of(" \t\n\v\f\r");
        return B == npos ? S.substr(S.size()) : S.substr(B);
    }
    void adjustWhitespace(unsigned LineIndex, int IndentDelta) {
        size_t EndOfPreviousLine = Lines[LineIndex - 1].size();
        if (InPPDirective && !Lines[LineIndex - 1].empty() && Lines[LineIndex - 1].back() == '\\')
            --EndOfPreviousLine;
        EndOfPreviousLine = llvmFindLastNotOf(Lines[LineIndex - 1], Blanks, EndOfPreviousLine);
        if (EndOfPreviousLine == npos)
            EndOfPreviousLine = 0;
        else
            ++EndOfPreviousLine;
        size_t StartOfLine = Lines[LineIndex].find_first_not_of(Blanks);
        if (StartOfLine == npos) StartOfLine = Lines[LineIndex].size();
        std::string_view Whitespace = Lines[LineIndex].substr(0, StartOfLine);
        size_t PreviousContentOffset = static_cast<size_t>(Content[LineIndex - 1].data() - Lines[LineIndex - 1].data());
        Content[LineIndex - 1] =
            Lines[LineIndex - 1].substr(PreviousContentOffset, EndOfPreviousLine - PreviousContentOffset);
        Content[LineIndex] = Lines[LineIndex].substr(StartOfLine);
        ContentColumn[LineIndex] = static_cast<int>(columnWidthWithTabs(Whitespace, 0, S.TabWidth)) + IndentDelta;
    }
    bool mayReflow(unsigned LineIndex) const {
        std::string_view IndentContent = Content[LineIndex];
        if (ltrimBlanks(Lines[LineIndex]).substr(0, 1) == "*") IndentContent = ltrimBlanks(Lines[LineIndex]).substr(1);
        return LineIndex > 0 && S.ReflowComments && !matchesCommentPragma(IndentContent) &&
               mayReflowContent(Content[LineIndex]) && !Tok.Finalized && !switchesFormatting(tokenAt(LineIndex));
    }

    unsigned DecorationColumn = 0;
    std::string_view Decoration;
    bool LastLineNeedsDecoration = true;
    int IndentAtLineBreak = 0;
    unsigned UnbreakableTailLength;
};

class BreakableLineCommentSection : public BreakableComment {
public:
    BreakableLineCommentSection(const FormatToken &Token, unsigned StartCol, bool InPPDirective, const Style &S)
        : BreakableComment(Token, StartCol, InPPDirective, S) {
        const FormatToken *LineTok = nullptr;
        const int Minimum = 1;  // SpacesInLineCommentPrefix
        int FirstLineSpaceChange = 0;
        for (const FormatToken *CurrentTok = &Tok; CurrentTok && CurrentTok->is(TT::LineComment);
             CurrentTok = CurrentTok->Next) {
            LastLineTok = LineTok;
            std::string_view TokenText(CurrentTok->TokenText);
            size_t FirstLineIndex = Lines.size();
            for (std::string_view L : splitLines(TokenText, "\n")) Lines.push_back(L);
            Content.resize(Lines.size());
            ContentColumn.resize(Lines.size());
            PrefixSpaceChange.resize(Lines.size());
            Tokens.resize(Lines.size());
            Prefix.resize(Lines.size());
            OriginalPrefix.resize(Lines.size());
            for (size_t i = FirstLineIndex, e = Lines.size(); i < e; ++i) {
                Lines[i] = ltrimBlanks(Lines[i]);
                std::string_view IndentPrefix = getLineCommentIndentPrefix(Lines[i]);
                OriginalPrefix[i] = IndentPrefix;
                const int SpacesInPrefix = static_cast<int>(std::count(IndentPrefix.begin(), IndentPrefix.end(), ' '));
                const auto NoSpaceBeforeFirstCommentChar = [&]() {
                    const char FirstCommentChar = Lines[i][IndentPrefix.size()];
                    const unsigned FirstCharByteSize = getCodePointNumBytes(FirstCommentChar);
                    if (columnWidth(Lines[i].substr(IndentPrefix.size(), FirstCharByteSize)) != 1) return false;
                    if (FirstCommentChar == '#' && TokenText.substr(0, 1) != "#") return false;
                    return FirstCommentChar == '\\' || isPunctuation(FirstCommentChar) ||
                           FirstCommentChar == ' ' || FirstCommentChar == '\t';
                };
                if (i == 0 || rtrimBlanks(OriginalPrefix[i]) != rtrimBlanks(OriginalPrefix[i - 1])) {
                    if (SpacesInPrefix < Minimum && Lines[i].size() > IndentPrefix.size() &&
                        !NoSpaceBeforeFirstCommentChar()) {
                        FirstLineSpaceChange = Minimum - SpacesInPrefix;
                    } else {
                        FirstLineSpaceChange = 0;
                    }
                }
                if (Lines[i].size() != IndentPrefix.size()) {
                    PrefixSpaceChange[i] = SpacesInPrefix + FirstLineSpaceChange < Minimum ? Minimum - SpacesInPrefix
                                                                                           : FirstLineSpaceChange;
                    const char FirstNonSpace = Lines[i][IndentPrefix.size()];
                    const bool IsFormatComment = LineTok && switchesFormatting(*LineTok);
                    const bool LineRequiresLeadingSpace =
                        !NoSpaceBeforeFirstCommentChar() || (FirstNonSpace == '}' && FirstLineSpaceChange != 0);
                    const bool AllowsSpaceChange = !IsFormatComment && (SpacesInPrefix != 0 || LineRequiresLeadingSpace);
                    if (PrefixSpaceChange[i] > 0 && AllowsSpaceChange) {
                        Prefix[i] = std::string(IndentPrefix);
                        Prefix[i].append(static_cast<size_t>(PrefixSpaceChange[i]), ' ');
                    } else if (PrefixSpaceChange[i] < 0 && AllowsSpaceChange) {
                        size_t Drop = std::min<size_t>(static_cast<size_t>(-PrefixSpaceChange[i]),
                                                       static_cast<size_t>(SpacesInPrefix));
                        Prefix[i] = std::string(IndentPrefix.substr(0, IndentPrefix.size() - Drop));
                    } else {
                        Prefix[i] = std::string(IndentPrefix);
                    }
                } else {
                    Prefix[i] = std::string(IndentPrefix.substr(0, IndentPrefix.size() - static_cast<size_t>(SpacesInPrefix)));
                }
                Tokens[i] = LineTok;
                Content[i] = Lines[i].substr(IndentPrefix.size());
                ContentColumn[i] = static_cast<int>(StartColumn + columnWidthWithTabs(Prefix[i], StartColumn, S.TabWidth));
                size_t EndOfLine = Content[i].find_last_not_of(Blanks);
                if (EndOfLine == npos)
                    EndOfLine = Content[i].size();
                else
                    ++EndOfLine;
                Content[i] = Content[i].substr(0, EndOfLine);
            }
            LineTok = CurrentTok->Next;
            if (CurrentTok->Next && !CurrentTok->Next->ContinuesLineCommentSection) break;
        }
    }

    unsigned getRangeLength(unsigned LineIndex, unsigned Offset, size_t Length, unsigned StartCol) const override {
        return columnWidthWithTabs(Content[LineIndex].substr(Offset, Length), StartCol, S.TabWidth);
    }
    unsigned getRemainingLength(unsigned LineIndex, unsigned Offset, unsigned StartCol) const override {
        return getRangeLength(LineIndex, Offset, npos, StartCol);
    }
    unsigned getContentStartColumn(unsigned LineIndex, bool) const override {
        return static_cast<unsigned>(ContentColumn[LineIndex]);
    }
    void insertBreak(unsigned LineIndex, unsigned TailOffset, Split Sp, unsigned,
                     WhitespaceManager &Whitespaces) const override {
        std::string_view Text = Content[LineIndex].substr(TailOffset);
        unsigned BreakOffsetInToken =
            static_cast<unsigned>(static_cast<size_t>(Text.data() - tokenAt(LineIndex).TokenText.data()) + Sp.first);
        Whitespaces.replaceWhitespaceInToken(tokenAt(LineIndex), BreakOffsetInToken, Sp.second, "", Prefix[LineIndex],
                                             InPPDirective, 1,
                                             ContentColumn[LineIndex] - static_cast<int>(Prefix[LineIndex].size()));
    }
    Split getReflowSplit(unsigned LineIndex) const override {
        if (!mayReflow(LineIndex)) return Split(npos, 0);
        size_t Trimmed = Content[LineIndex].find_first_not_of(Blanks);
        return Split(0, Trimmed != npos ? static_cast<unsigned>(Trimmed) : 0);
    }
    void reflow(unsigned LineIndex, WhitespaceManager &Whitespaces) const override {
        if (LineIndex > 0 && Tokens[LineIndex] != Tokens[LineIndex - 1]) {
            Whitespaces.replaceWhitespace(*const_cast<FormatToken *>(Tokens[LineIndex]), 0, 0, StartColumn,
                                          /*IsAligned=*/true, /*InPPDirective=*/false);
        } else if (LineIndex > 0) {
            unsigned Offset = static_cast<unsigned>(Lines[LineIndex - 1].data() + Lines[LineIndex - 1].size() -
                                                    tokenAt(LineIndex - 1).TokenText.data());
            unsigned WhitespaceLength =
                static_cast<unsigned>(Lines[LineIndex].data() - tokenAt(LineIndex).TokenText.data() - Offset);
            Whitespaces.replaceWhitespaceInToken(*Tokens[LineIndex], Offset, WhitespaceLength, "", "", false, 0, 0);
        }
        unsigned Offset = static_cast<unsigned>(Lines[LineIndex].data() - tokenAt(LineIndex).TokenText.data());
        unsigned WhitespaceLength = static_cast<unsigned>(Content[LineIndex].data() - Lines[LineIndex].data());
        Whitespaces.replaceWhitespaceInToken(*Tokens[LineIndex], Offset, WhitespaceLength, "", ReflowPrefix, false, 0,
                                             0);
    }
    void adaptStartOfLine(unsigned LineIndex, WhitespaceManager &Whitespaces) const override {
        if (LineIndex > 0 && Tokens[LineIndex] != Tokens[LineIndex - 1]) {
            unsigned LineColumn = static_cast<unsigned>(
                ContentColumn[LineIndex] - (Content[LineIndex].data() - Lines[LineIndex].data()) +
                (static_cast<int>(OriginalPrefix[LineIndex].size()) - static_cast<int>(Prefix[LineIndex].size())));
            Whitespaces.replaceWhitespace(*const_cast<FormatToken *>(Tokens[LineIndex]), 1, LineColumn, LineColumn,
                                          /*IsAligned=*/true, /*InPPDirective=*/false);
        }
        if (OriginalPrefix[LineIndex] != std::string_view(Prefix[LineIndex])) {
            const int SpacesToRemove = -std::min(PrefixSpaceChange[LineIndex], 0);
            const int SpacesToAdd = std::max(PrefixSpaceChange[LineIndex], 0);
            Whitespaces.replaceWhitespaceInToken(
                tokenAt(LineIndex), static_cast<unsigned>(OriginalPrefix[LineIndex].size() - static_cast<size_t>(SpacesToRemove)),
                static_cast<unsigned>(SpacesToRemove), "", "", false, 0, SpacesToAdd);
        }
    }
    void updateNextToken(LineState &State) const override {
        if (LastLineTok) State.NextToken = LastLineTok->Next;
    }

private:
    bool mayReflow(unsigned LineIndex) const {
        std::string_view IndentContent = Content[LineIndex];
        if (Lines[LineIndex].substr(0, 2) == "//") IndentContent = Lines[LineIndex].substr(2);
        return LineIndex > 0 && S.ReflowComments && !matchesCommentPragma(IndentContent) &&
               mayReflowContent(Content[LineIndex]) && !Tok.Finalized && !switchesFormatting(tokenAt(LineIndex)) &&
               OriginalPrefix[LineIndex] == OriginalPrefix[LineIndex - 1];
    }

    std::vector<std::string> Prefix;
    std::vector<std::string_view> OriginalPrefix;
    std::vector<int> PrefixSpaceChange;
    const FormatToken *LastLineTok = nullptr;
};

}  // namespace

bool switchesFormatting(const FormatToken &Token) {
    std::string_view Content = Token.TokenText.substr(2);
    size_t B = Content.find_first_not_of(" \t\n\v\f\r");
    Content = B == npos ? Content.substr(Content.size()) : Content.substr(B);
    return Content.substr(0, 15) == "clang-format on" || Content.substr(0, 16) == "clang-format off";
}

// --- ContinuationIndenter ---------------------------------------------------------------

unsigned ContinuationIndenter::spellingColumn(size_t Offset) const {
    size_t LineStart = Src.Text.rfind('\n', Offset == 0 ? 0 : Offset - 1);
    LineStart = (LineStart == npos || Offset == 0) ? 0 : LineStart + 1;
    if (Offset > 0 && Src.Text[Offset - 1] == '\n') LineStart = Offset;
    return static_cast<unsigned>(Offset - LineStart + 1);
}

LineState ContinuationIndenter::getInitialState(unsigned FirstIndent, unsigned FirstStartColumn,
                                                const AnnotatedLine *Line, bool DryRun) {
    LineState State;
    State.FirstIndent = FirstIndent;
    if (FirstStartColumn && Line->First->NewlinesBefore == 0)
        State.Column = FirstStartColumn;
    else
        State.Column = FirstIndent;
    State.Line = Line;
    State.NextToken = Line->First;
    State.Stack.push_back(ParenState(nullptr, FirstIndent, FirstIndent, false, false));
    State.NoContinuation = false;
    State.StartOfStringLiteral = 0;
    State.NoLineBreak = false;
    State.StartOfLineLevel = 0;
    State.LowestLevelOnLine = 0;
    State.IgnoreStackForComparison = false;
    moveStateToNextToken(State, DryRun, /*Newline=*/false);
    return State;
}

bool ContinuationIndenter::canBreak(const LineState &State) {
    const FormatToken &Current = *State.NextToken;
    const FormatToken &Previous = *Current.Previous;
    const ParenState &CurrentState = State.Stack.back();
    if (!Current.CanBreakBefore && !(CurrentState.BreakBeforeClosingBrace && Current.closesBlockOrBlockTypeList()))
        return false;
    if (!Current.MustBreakBefore && Previous.is(tok::l_brace) && Previous.isNot(TT::DictLiteral) &&
        Previous.is(BK_BracedInit) && Previous.Previous &&
        Previous.Previous->isOneOf(tok::l_brace, tok::l_paren, tok::comma)) {
        return false;
    }
    if (Previous.opensScope() && Previous.isNot(tok::l_brace) && State.LowestLevelOnLine < State.StartOfLineLevel &&
        State.LowestLevelOnLine < Current.NestingLevel) {
        return false;
    }
    if (Current.isMemberAccess() && CurrentState.ContainsUnwrappedBuilder) return false;
    if (Previous.is(tok::l_brace) && State.Stack.size() > 1 && State.Stack[State.Stack.size() - 2].NestedBlockInlined &&
        State.Stack[State.Stack.size() - 2].HasMultipleNestedBlocks) {
        return false;  // isCpp() && LBI_OuterScope: Signature in LLVM style
    }
    if (Current.is(TT::FunctionDeclarationName) && State.Column < 6) return false;
    if (!Current.isOneOf(TT::BinaryOperator, tok::comma) && CurrentState.NoLineBreakInOperand) return false;
    if (Previous.is(tok::l_square) && Previous.is(TT::ObjCMethodExpr)) return false;
    return !State.NoLineBreak && !CurrentState.NoLineBreak;
}

bool ContinuationIndenter::mustBreak(const LineState &State) {
    const FormatToken &Current = *State.NextToken;
    const FormatToken &Previous = *Current.Previous;
    const ParenState &CurrentState = State.Stack.back();
    if (Current.MustBreakBefore) return true;
    if (Current.is(TT::InlineASMColon)) return true;  // BBIAS_OnlyMultiline with a column limit
    if (CurrentState.BreakBeforeClosingBrace && Current.closesBlockOrBlockTypeList()) return true;
    if ((startsNextParameter(Current) || Previous.is(tok::semi) ||
         (Previous.is(TT::TemplateCloser) && Current.is(TT::StartOfName) &&
          State.Line->First->isNot(TT::AttributeSquare)) ||
         (S.BreakBeforeTernaryOperators && Current.is(TT::ConditionalExpr) && Previous.isNot(tok::question)) ||
         (!S.BreakBeforeTernaryOperators && Previous.is(TT::ConditionalExpr))) &&
        CurrentState.BreakBeforeParameter && !Current.isTrailingComment() &&
        !Current.isOneOf(tok::r_paren, tok::r_brace)) {
        return true;
    }
    if (CurrentState.IsChainedConditional &&
        ((S.BreakBeforeTernaryOperators && Current.is(TT::ConditionalExpr) && Current.is(tok::colon)) ||
         (!S.BreakBeforeTernaryOperators && Previous.is(TT::ConditionalExpr) && Previous.is(tok::colon)))) {
        return true;
    }
    if (((Previous.is(TT::DictLiteral) && Previous.is(tok::l_brace)) ||
         (Previous.is(TT::ArrayInitializerLSquare) && Previous.ParameterCount > 1)) &&
        S.ColumnLimit > 0 && getLengthToMatchingParen(Previous, State.Stack) + State.Column - 1 > getColumnLimit(State)) {
        return true;
    }
    if (Current.is(TT::CtorInitializerColon) &&
        (State.Column + State.Line->Last->TotalLength - Previous.TotalLength > getColumnLimit(State) ||
         CurrentState.BreakBeforeParameter) &&
        ((!Current.isTrailingComment() && S.ColumnLimit > 0) || Current.NewlinesBefore > 0)) {
        return true;
    }
    if (Current.is(TT::ObjCMethodExpr) && Previous.isNot(TT::SelectorName) &&
        State.Line->startsWith(TT::ObjCMethodSpecifier)) {
        return true;
    }
    if (Current.is(TT::SelectorName) && Previous.isNot(tok::at) && CurrentState.ObjCSelectorNameFound &&
        CurrentState.BreakBeforeParameter && !Current.startsSequence(TT::SelectorName, tok::colon, tok::caret)) {
        return true;
    }
    unsigned NewLineColumn = getNewLineColumn(State);
    if (Current.isMemberAccess() && S.ColumnLimit != 0 &&
        State.Column + getLengthToNextOperator(Current) > S.ColumnLimit &&
        (State.Column > NewLineColumn || Current.NestingLevel < State.StartOfLineLevel)) {
        return true;
    }
    if (startsSegmentOfBuilderTypeCall(Current) &&
        (CurrentState.CallContinuation != 0 || CurrentState.BreakBeforeParameter) &&
        !(Previous.closesScopeAfterBlock() && State.Column <= NewLineColumn)) {
        return true;
    }
    if (Previous.ClosesTemplateDeclaration && CurrentState.BreakBeforeParameter && Current.CanBreakBefore) return true;
    if (State.Line->First->isNot(tok::kw_enum) && State.Column <= NewLineColumn) return false;
    if (Previous.is(TT::BinaryOperator) && Current.CanBreakBefore) {
        const prec::Level PreviousPrecedence = Previous.getPrecedence();
        if (PreviousPrecedence != prec::Assignment && CurrentState.BreakBeforeParameter &&
            !Current.isTrailingComment()) {
            const bool LHSIsBinaryExpr = Previous.Previous && Previous.Previous->EndsBinaryExpression;
            if (LHSIsBinaryExpr) return true;
            const bool IsComparison =
                (PreviousPrecedence == prec::Relational || PreviousPrecedence == prec::Equality ||
                 PreviousPrecedence == prec::Spaceship) &&
                Previous.Previous && Previous.Previous->isNot(TT::BinaryOperator);
            if (!IsComparison) return true;
        }
    } else if (Current.is(TT::BinaryOperator) && Current.CanBreakBefore && CurrentState.BreakBeforeParameter) {
        return true;
    }
    if (Current.is(tok::lessless) && CurrentState.BreakBeforeParameter && CurrentState.FirstLessLess == 0) return true;
    if (Current.NestingLevel == 0 && !Current.isTrailingComment()) {
        if (Previous.ClosesTemplateDeclaration) return true;
        if (Previous.is(TT::FunctionAnnotationRParen) && State.Line->Type != LT_PreprocessorDirective) return true;
    }
    if (Current.is(TT::FunctionDeclarationName) && !State.Line->ReturnTypeWrapped &&
        CurrentState.BreakBeforeParameter) {
        for (const FormatToken *Tok = &Previous; Tok; Tok = Tok->Previous) {
            if (Tok->is(TT::LineComment)) return false;
            if (Tok->is(TT::TemplateCloser)) Tok = Tok->MatchingParen;
            if (Tok->FirstAfterPPLine) return false;
        }
        return true;
    }
    if (S.ColumnLimit != 0 && Previous.is(BK_Block) && Previous.is(tok::l_brace) &&
        !Current.isOneOf(tok::r_brace, tok::comment)) {
        return true;
    }
    if (Current.is(tok::lessless) &&
        ((Previous.is(tok::identifier) && Previous.TokenText == "endl") ||
         (Previous.isLiteral() && ((Previous.TokenText.size() >= 3 &&
                                    Previous.TokenText.substr(Previous.TokenText.size() - 3) == "\\n\"") ||
                                   Previous.TokenText == "'\\n'")))) {
        return true;
    }
    if (Previous.is(TT::BlockComment) && Previous.IsMultiline) return true;
    if (State.NoContinuation) return true;
    return false;
}

unsigned ContinuationIndenter::addTokenToState(LineState &State, bool Newline, bool DryRun, unsigned ExtraSpaces) {
    const FormatToken &Current = *State.NextToken;
    const FormatToken &Previous = *State.NextToken->Previous;
    State.NoContinuation = false;
    if (Current.is(TT::ImplicitStringLiteral) && (!Previous.HasIdentifierInfo || Previous.PPKind == pp::none)) {
        unsigned EndColumn = spellingColumn(Current.WhitespaceEnd);
        if (Current.LastNewlineOffset != 0) {
            State.Column = EndColumn;
        } else {
            unsigned StartColumn = spellingColumn(Current.WhitespaceStart);
            State.Column += EndColumn - StartColumn;
        }
        moveStateToNextToken(State, DryRun, /*Newline=*/false);
        return 0;
    }
    unsigned Penalty = 0;
    if (Newline)
        Penalty = addTokenOnNewLine(State, DryRun);
    else
        addTokenOnCurrentLine(State, DryRun, ExtraSpaces);
    return moveStateToNextToken(State, DryRun, Newline) + Penalty;
}

void ContinuationIndenter::addTokenOnCurrentLine(LineState &State, bool DryRun, unsigned ExtraSpaces) {
    FormatToken &Current = *State.NextToken;
    const FormatToken &Previous = *State.NextToken->Previous;
    ParenState &CurrentState = State.Stack.back();
    if (Current.is(tok::equal) && (State.Line->First->is(tok::kw_for) || Current.NestingLevel == 0) &&
        CurrentState.VariablePos == 0 &&
        (!Previous.Previous || Previous.Previous->isNot(TT::DesignatedInitializerPeriod))) {
        CurrentState.VariablePos = State.Column;
        const FormatToken *Tok = &Previous;
        while (Tok && CurrentState.VariablePos >= Tok->ColumnWidth) {
            CurrentState.VariablePos -= Tok->ColumnWidth;
            if (Tok->SpacesRequiredBefore != 0) break;
            Tok = Tok->Previous;
        }
        if (Previous.PartOfMultiVariableDeclStmt) CurrentState.LastSpace = CurrentState.VariablePos;
    }
    unsigned Spaces = Current.SpacesRequiredBefore + ExtraSpaces;
    if (!DryRun) {
        Whitespaces.replaceWhitespace(Current, /*Newlines=*/0, Spaces, State.Column + Spaces, /*IsAligned=*/false,
                                      State.Line->InMacroBody);
    }
    if (Current.is(TT::SelectorName) && !CurrentState.ObjCSelectorNameFound) {
        unsigned MinIndent = std::max(State.FirstIndent + S.ContinuationIndentWidth, CurrentState.Indent);
        unsigned FirstColonPos = State.Column + Spaces + Current.ColumnWidth;
        if (Current.LongestObjCSelectorName == 0)
            CurrentState.AlignColons = false;
        else if (MinIndent + Current.LongestObjCSelectorName > FirstColonPos)
            CurrentState.ColonPos = MinIndent + Current.LongestObjCSelectorName;
        else
            CurrentState.ColonPos = FirstColonPos;
    }
    // AlignAfterOpenBracket: Align.
    if (Previous.opensScope() && Previous.isNot(TT::ObjCMethodExpr) &&
        (Current.isNot(TT::LineComment) || Previous.is(BK_BracedInit))) {
        CurrentState.Indent = State.Column + Spaces;
        CurrentState.IsAligned = true;
    }
    if (CurrentState.AvoidBinPacking && startsNextParameter(Current)) CurrentState.NoLineBreak = true;
    if (startsSegmentOfBuilderTypeCall(Current) && State.Column > getNewLineColumn(State))
        CurrentState.ContainsUnwrappedBuilder = true;
    if (Current.isMemberAccess() && Previous.is(tok::r_paren) &&
        (Previous.MatchingParen && (Previous.TotalLength - Previous.MatchingParen->TotalLength > 10))) {
        CurrentState.NoLineBreak = true;
    }
    const FormatToken *P = Current.getPreviousNonComment();
    if (Current.isNot(tok::comment) && P &&
        (P->isOneOf(TT::BinaryOperator, tok::comma) || (P->is(TT::ConditionalExpr) && P->is(tok::colon))) &&
        !P->isOneOf(TT::CtorInitializerComma) && P->getPrecedence() != prec::Assignment &&
        P->getPrecedence() != prec::Relational && P->getPrecedence() != prec::Spaceship) {
        bool BreakBeforeOperator = P->MustBreakBefore || P->is(tok::lessless) ||
                                   (P->is(TT::ConditionalExpr) && S.BreakBeforeTernaryOperators);
        bool HasTwoOperands = P->OperatorIndex == 0 && !P->NextOperator && P->isNot(TT::ConditionalExpr);
        if ((!BreakBeforeOperator && !(HasTwoOperands && S.AlignOperands)) ||
            (!CurrentState.LastOperatorWrapped && BreakBeforeOperator)) {
            CurrentState.NoLineBreakInOperand = true;
        }
    }
    State.Column += Spaces;
    if (Current.isNot(tok::comment) && Previous.is(tok::l_paren) && Previous.Previous &&
        (Previous.Previous->is(tok::kw_for) || Previous.Previous->isIf())) {
        CurrentState.LastSpace = State.Column;
        CurrentState.NestedBlockIndent = State.Column;
    } else if (!Current.isOneOf(tok::comment, tok::caret) &&
               (Previous.is(tok::comma) || (Previous.is(tok::colon) && Previous.is(TT::ObjCMethodExpr)))) {
        CurrentState.LastSpace = State.Column;
    } else if (Previous.isOneOf(TT::ConditionalExpr, TT::CtorInitializerColon)) {
        CurrentState.LastSpace = State.Column;
    } else if (Previous.is(TT::BinaryOperator) &&
               ((Previous.getPrecedence() != prec::Assignment &&
                 (Previous.isNot(tok::lessless) || Previous.OperatorIndex != 0 || Previous.NextOperator)) ||
                Current.StartsBinaryExpression)) {
        CurrentState.LastSpace = State.Column;
    } else if (Previous.is(TT::InheritanceColon)) {
        CurrentState.Indent = State.Column;
        CurrentState.LastSpace = State.Column;
    } else if (Previous.opensScope()) {
        if (Previous.MatchingParen) {
            const FormatToken *Next = Previous.MatchingParen->getNextNonComment();
            if (Next && Next->isMemberAccess() && State.Stack.size() > 1 &&
                State.Stack[State.Stack.size() - 2].CallContinuation == 0) {
                CurrentState.LastSpace = State.Column;
            }
        }
    }
}

unsigned ContinuationIndenter::addTokenOnNewLine(LineState &State, bool DryRun) {
    FormatToken &Current = *State.NextToken;
    const FormatToken &Previous = *State.NextToken->Previous;
    ParenState &CurrentState = State.Stack.back();
    unsigned Penalty = 0;
    const FormatToken *PreviousNonComment = Current.getPreviousNonComment();
    const FormatToken *NextNonComment = Previous.getNextNonComment();
    if (!NextNonComment) NextNonComment = &Current;
    if (!CurrentState.ContainsLineBreak) Penalty += 15;
    CurrentState.ContainsLineBreak = true;
    Penalty += State.NextToken->SplitPenalty;
    if (NextNonComment->is(tok::lessless) && CurrentState.FirstLessLess == 0 &&
        (State.Column <= S.ColumnLimit / 3 || CurrentState.BreakBeforeParameter)) {
        Penalty += S.PenaltyBreakFirstLessLess;
    }
    State.Column = getNewLineColumn(State);
    if (State.Column > State.FirstIndent) Penalty += S.PenaltyIndentedWhitespace * (State.Column - State.FirstIndent);
    if (Current.isNot(TT::LambdaArrow)) CurrentState.NestedBlockIndent = State.Column;
    if (NextNonComment->isMemberAccess()) {
        if (CurrentState.CallContinuation == 0) CurrentState.CallContinuation = State.Column;
    } else if (NextNonComment->is(TT::SelectorName)) {
        if (!CurrentState.ObjCSelectorNameFound) {
            if (NextNonComment->LongestObjCSelectorName == 0)
                CurrentState.AlignColons = false;
            else
                CurrentState.ColonPos =
                    CurrentState.Indent + std::max(NextNonComment->LongestObjCSelectorName, NextNonComment->ColumnWidth);
        } else if (CurrentState.AlignColons && CurrentState.ColonPos <= NextNonComment->ColumnWidth) {
            CurrentState.ColonPos = State.Column + NextNonComment->ColumnWidth;
        }
    } else if (PreviousNonComment && PreviousNonComment->is(tok::colon) &&
               PreviousNonComment->isOneOf(TT::ObjCMethodExpr, TT::DictLiteral)) {
        if (State.Stack.size() > 1) {
            State.Stack[State.Stack.size() - 2].LastSpace =
                std::max(CurrentState.LastSpace, CurrentState.Indent) + S.ContinuationIndentWidth;
        }
    }
    if ((PreviousNonComment && PreviousNonComment->isOneOf(tok::comma, tok::semi) && !CurrentState.AvoidBinPacking) ||
        Previous.is(TT::BinaryOperator)) {
        CurrentState.BreakBeforeParameter = false;
    }
    if (PreviousNonComment && PreviousNonComment->is(TT::TemplateCloser) && Current.NestingLevel == 0)
        CurrentState.BreakBeforeParameter = false;
    if (NextNonComment->is(tok::question) || (PreviousNonComment && PreviousNonComment->is(tok::question)))
        CurrentState.BreakBeforeParameter = true;
    if (Current.is(TT::BinaryOperator) && Current.CanBreakBefore) CurrentState.BreakBeforeParameter = false;
    if (!DryRun) {
        unsigned MaxEmptyLinesToKeep = S.MaxEmptyLinesToKeep + 1;
        if (Current.is(tok::r_brace) && Current.MatchingParen && !Current.MatchingParen->Children.empty())
            MaxEmptyLinesToKeep = 1;
        unsigned Newlines = std::max(1u, std::min(Current.NewlinesBefore, MaxEmptyLinesToKeep));
        bool ContinuePPDirective = State.Line->InPPDirective && State.Line->Type != LT_ImportStatement;
        Whitespaces.replaceWhitespace(Current, Newlines, State.Column, State.Column, CurrentState.IsAligned,
                                      ContinuePPDirective);
    }
    if (!Current.isTrailingComment()) CurrentState.LastSpace = State.Column;
    if (Current.is(tok::lessless)) CurrentState.LastSpace += 3;
    State.StartOfLineLevel = Current.NestingLevel;
    State.LowestLevelOnLine = Current.NestingLevel;
    {
        // NestedBlockSpecialCase is false for C (no lambdas, ObjC or requires).
        for (size_t I = State.Stack.size() - 1; I-- > 0;) State.Stack[I].BreakBeforeParameter = true;
    }
    if (PreviousNonComment && !PreviousNonComment->isOneOf(tok::comma, tok::colon, tok::semi) &&
        (PreviousNonComment->isNot(TT::TemplateCloser) || Current.NestingLevel != 0) &&
        !PreviousNonComment->isOneOf(TT::BinaryOperator, TT::FunctionAnnotationRParen) &&
        Current.isNot(TT::BinaryOperator) && !PreviousNonComment->opensScope()) {
        CurrentState.BreakBeforeParameter = true;
    }
    if (PreviousNonComment && PreviousNonComment->isOneOf(tok::l_brace, TT::ArrayInitializerLSquare))
        CurrentState.BreakBeforeClosingBrace = true;
    if (CurrentState.AvoidBinPacking) {
        if (!Previous.isOneOf(tok::l_paren, tok::l_brace, TT::BinaryOperator) ||
            (!S.AllowAllParametersOfDeclarationOnNextLine && State.Line->MustBeDeclaration) ||
            (!S.AllowAllArgumentsOnNextLine && !State.Line->MustBeDeclaration) || Previous.is(TT::DictLiteral)) {
            CurrentState.BreakBeforeParameter = true;
        }
    }
    return Penalty;
}

unsigned ContinuationIndenter::getNewLineColumn(const LineState &State) {
    if (!State.NextToken || !State.NextToken->Previous) return 0;
    FormatToken &Current = *State.NextToken;
    const ParenState &CurrentState = State.Stack.back();
    const FormatToken &Previous = *Current.Previous;
    unsigned ContinuationIndent = std::max(CurrentState.LastSpace, CurrentState.Indent) + S.ContinuationIndentWidth;
    const FormatToken *PreviousNonComment = Current.getPreviousNonComment();
    const FormatToken *NextNonComment = Previous.getNextNonComment();
    if (!NextNonComment) NextNonComment = &Current;
    if (NextNonComment->is(tok::l_brace) && NextNonComment->is(BK_Block)) {
        if (Current.NestingLevel == 0) return State.FirstIndent;
        return CurrentState.Indent;
    }
    if (Current.isOneOf(tok::r_brace, tok::r_square) && State.Stack.size() > 1) {
        if (Current.closesBlockOrBlockTypeList()) return State.Stack[State.Stack.size() - 2].NestedBlockIndent;
        if (Current.MatchingParen && Current.MatchingParen->is(BK_BracedInit))
            return State.Stack[State.Stack.size() - 2].LastSpace;
        return State.FirstIndent;
    }
    if (Current.is(tok::r_paren) && State.Stack.size() > 1 &&
        (!Current.Next || Current.Next->isOneOf(tok::semi, tok::kw_const, tok::l_brace))) {
        return State.Stack[State.Stack.size() - 2].LastSpace;
    }
    if (Current.is(tok::identifier) && Current.Next && Current.Next->is(TT::DictLiteral)) return CurrentState.Indent;
    if (NextNonComment->isStringLiteral() && State.StartOfStringLiteral != 0) return State.StartOfStringLiteral;
    if (NextNonComment->is(tok::lessless) && CurrentState.FirstLessLess != 0) return CurrentState.FirstLessLess;
    if (NextNonComment->isMemberAccess()) {
        if (CurrentState.CallContinuation == 0) return ContinuationIndent;
        return CurrentState.CallContinuation;
    }
    if (CurrentState.QuestionColumn != 0 &&
        ((NextNonComment->is(tok::colon) && NextNonComment->is(TT::ConditionalExpr)) ||
         Previous.is(TT::ConditionalExpr))) {
        if (((NextNonComment->is(tok::colon) && NextNonComment->Next && !NextNonComment->Next->FakeLParens.empty() &&
              NextNonComment->Next->FakeLParens.back() == prec::Conditional) ||
             (Previous.is(tok::colon) && !Current.FakeLParens.empty() &&
              Current.FakeLParens.back() == prec::Conditional)) &&
            !CurrentState.IsWrappedConditional) {
            unsigned Indent = CurrentState.Indent;
            if (S.AlignOperands) Indent -= S.ContinuationIndentWidth;
            // UnindentOperator is only set with OAS_AlignAfterOperator.
            return Indent;
        }
        return CurrentState.QuestionColumn;
    }
    if (Previous.is(tok::comma) && CurrentState.VariablePos != 0) return CurrentState.VariablePos;
    if (NextNonComment->isOneOf(TT::CtorInitializerColon, TT::InheritanceColon, TT::InheritanceComma))
        return State.FirstIndent + 4;  // ConstructorInitializerIndentWidth
    if ((PreviousNonComment &&
         (PreviousNonComment->ClosesTemplateDeclaration ||
          (PreviousNonComment->is(TT::AttributeMacro) && Current.isNot(tok::l_paren) &&
           !Current.endsSequence(TT::StartOfName, TT::AttributeMacro, TT::PointerOrReference)) ||
          PreviousNonComment->isOneOf(TT::AttributeRParen, TT::AttributeSquare, TT::FunctionAnnotationRParen))) ||
        NextNonComment->is(TT::FunctionDeclarationName)) {
        return std::max(CurrentState.LastSpace, CurrentState.Indent);
    }
    if (NextNonComment->is(TT::SelectorName)) {
        if (!CurrentState.ObjCSelectorNameFound) {
            unsigned MinIndent = CurrentState.Indent;
            return MinIndent + std::max(NextNonComment->LongestObjCSelectorName, NextNonComment->ColumnWidth) -
                   NextNonComment->ColumnWidth;
        }
        if (!CurrentState.AlignColons) return CurrentState.Indent;
        if (CurrentState.ColonPos > NextNonComment->ColumnWidth) return CurrentState.ColonPos - NextNonComment->ColumnWidth;
        return CurrentState.Indent;
    }
    if (NextNonComment->is(tok::colon) && NextNonComment->is(TT::ObjCMethodExpr)) return CurrentState.ColonPos;
    if (NextNonComment->is(TT::ArraySubscriptLSquare)) {
        if (CurrentState.StartOfArraySubscripts != 0) return CurrentState.StartOfArraySubscripts;
        return ContinuationIndent;
    }
    if (State.Line->InPragmaDirective) {
        const FormatToken *PragmaType = State.Line->First->Next ? State.Line->First->Next->Next : nullptr;
        if (PragmaType && PragmaType->TokenText == "omp") return CurrentState.Indent + S.ContinuationIndentWidth;
    }
    if (NextNonComment->is(tok::identifier) && NextNonComment->FakeRParens == 0 && NextNonComment->Next &&
        NextNonComment->Next->is(TT::ObjCMethodExpr)) {
        return CurrentState.Indent;
    }
    if (NextNonComment->isOneOf(TT::StartOfName, TT::PointerOrReference) || Previous.isOneOf(tok::coloncolon, tok::equal))
        return ContinuationIndent;
    if (PreviousNonComment && PreviousNonComment->is(tok::colon) &&
        PreviousNonComment->isOneOf(TT::ObjCMethodExpr, TT::DictLiteral)) {
        return ContinuationIndent;
    }
    if (NextNonComment->is(TT::CtorInitializerComma)) return CurrentState.Indent;
    if (Previous.is(tok::r_paren) && !Current.isBinaryOperator() && !Current.isOneOf(tok::colon, tok::comment))
        return ContinuationIndent;
    if (CurrentState.Indent == State.FirstIndent && PreviousNonComment &&
        !PreviousNonComment->isOneOf(tok::r_brace, TT::CtorInitializerComma)) {
        return CurrentState.Indent + S.ContinuationIndentWidth;
    }
    return CurrentState.Indent;
}

unsigned ContinuationIndenter::moveStateToNextToken(LineState &State, bool DryRun, bool Newline) {
    const FormatToken &Current = *State.NextToken;
    ParenState &CurrentState = State.Stack.back();
    if (Current.isOneOf(tok::comma, TT::BinaryOperator)) CurrentState.NoLineBreakInOperand = false;
    if (Current.is(TT::InheritanceColon)) CurrentState.AvoidBinPacking = true;
    if (Current.is(tok::lessless)) {
        if (CurrentState.FirstLessLess == 0)
            CurrentState.FirstLessLess = State.Column;
        else
            CurrentState.LastOperatorWrapped = Newline;
    }
    if (Current.is(TT::BinaryOperator) && Current.isNot(tok::lessless)) CurrentState.LastOperatorWrapped = Newline;
    if (Current.is(TT::ConditionalExpr) && Current.Previous && Current.Previous->isNot(TT::ConditionalExpr))
        CurrentState.LastOperatorWrapped = Newline;
    if (Current.is(TT::ArraySubscriptLSquare) && CurrentState.StartOfArraySubscripts == 0)
        CurrentState.StartOfArraySubscripts = State.Column;
    auto IsWrappedConditional = [](const FormatToken &Tok) {
        if (!(Tok.is(TT::ConditionalExpr) && Tok.is(tok::question))) return false;
        if (Tok.MustBreakBefore) return true;
        const FormatToken *Next = Tok.getNextNonComment();
        return Next && Next->MustBreakBefore;
    };
    if (IsWrappedConditional(Current)) CurrentState.IsWrappedConditional = true;
    if (S.BreakBeforeTernaryOperators && Current.is(tok::question)) CurrentState.QuestionColumn = State.Column;
    if (!S.BreakBeforeTernaryOperators && Current.isNot(tok::colon)) {
        const FormatToken *Previous = Current.Previous;
        while (Previous && Previous->isTrailingComment()) Previous = Previous->Previous;
        if (Previous && Previous->is(tok::question)) CurrentState.QuestionColumn = State.Column;
    }
    if (!Current.opensScope() && !Current.closesScope() && Current.isNot(TT::PointerOrReference))
        State.LowestLevelOnLine = std::min(State.LowestLevelOnLine, Current.NestingLevel);
    if (Current.isMemberAccess()) CurrentState.StartOfFunctionCall = !Current.NextOperator ? 0 : State.Column;
    if (Current.is(TT::SelectorName)) CurrentState.ObjCSelectorNameFound = true;
    if (Current.is(TT::CtorInitializerColon)) {
        CurrentState.Indent = State.Column + 2;
        CurrentState.NestedBlockIndent = CurrentState.Indent;
        CurrentState.BreakBeforeParameter = false;
    }
    if (Current.is(TT::InheritanceColon)) CurrentState.Indent = State.FirstIndent + 4;
    if (Current.isOneOf(TT::BinaryOperator, TT::ConditionalExpr) && Newline)
        CurrentState.NestedBlockIndent = State.Column + Current.ColumnWidth + 1;
    if (Current.isOneOf(TT::LambdaLSquare, TT::LambdaArrow)) CurrentState.LastSpace = State.Column;

    const FormatToken *Previous = Current.getPreviousNonComment();
    if (Current.isNot(tok::comment) && Previous && Previous->isOneOf(tok::l_brace, TT::ArrayInitializerLSquare) &&
        Previous->isNot(TT::DictLiteral) && State.Stack.size() > 1 && !CurrentState.HasMultipleNestedBlocks) {
        if (State.Stack[State.Stack.size() - 2].NestedBlockInlined && Newline)
            for (size_t I = 0; I + 1 < State.Stack.size(); ++I) State.Stack[I].NoLineBreak = true;
        State.Stack[State.Stack.size() - 2].NestedBlockInlined = false;
    }
    if (Previous && (Previous->isOneOf(TT::BinaryOperator, TT::ConditionalExpr) ||
                     (Previous->isOneOf(tok::l_paren, tok::comma, tok::colon) &&
                      !Previous->isOneOf(TT::DictLiteral, TT::ObjCMethodExpr)))) {
        State.Stack.back().NestedBlockInlined = !Newline && hasNestedBlockInlined(Previous);
    }
    moveStatePastFakeLParens(State, Newline);
    moveStatePastScopeCloser(State);
    bool AllowBreak = !State.Stack.back().NoLineBreak && !State.Stack.back().NoLineBreakInOperand;
    moveStatePastScopeOpener(State, Newline);
    moveStatePastFakeRParens(State);
    if (Current.isStringLiteral() && State.StartOfStringLiteral == 0) {
        State.StartOfStringLiteral = State.Column;
    } else if (!Current.isOneOf(tok::comment, tok::identifier, tok::hash) && !Current.isStringLiteral()) {
        State.StartOfStringLiteral = 0;
    }
    State.Column += Current.ColumnWidth;
    State.NextToken = State.NextToken->Next;
    unsigned Penalty = handleEndOfLine(Current, State, DryRun, AllowBreak, Newline);
    if (Current.Role) Current.Role->formatFromToken(State, this, DryRun);
    if (Previous && Previous->Role) Penalty += Previous->Role->formatAfterToken(State, this, DryRun);
    return Penalty;
}

void ContinuationIndenter::moveStatePastFakeLParens(LineState &State, bool Newline) {
    const FormatToken &Current = *State.NextToken;
    if (Current.FakeLParens.empty()) return;
    const FormatToken *Previous = Current.getPreviousNonComment();
    bool SkipFirstExtraIndent =
        Previous && (Previous->opensScope() || Previous->isOneOf(tok::semi, tok::kw_return) ||
                     (Previous->getPrecedence() == prec::Assignment && S.AlignOperands) ||
                     Previous->is(TT::ObjCMethodExpr));
    for (auto It = Current.FakeLParens.rbegin(); It != Current.FakeLParens.rend(); ++It) {
        const prec::Level PrecedenceLevel = *It;
        const ParenState &CurrentState = State.Stack.back();
        ParenState NewParenState = CurrentState;
        NewParenState.Tok = nullptr;
        NewParenState.ContainsLineBreak = false;
        NewParenState.LastOperatorWrapped = true;
        NewParenState.IsChainedConditional = false;
        NewParenState.IsWrappedConditional = false;
        NewParenState.UnindentOperator = false;
        NewParenState.NoLineBreak = NewParenState.NoLineBreak || CurrentState.NoLineBreakInOperand;
        if (PrecedenceLevel > prec::Comma) NewParenState.AvoidBinPacking = false;
        if (!Current.isTrailingComment() && (S.AlignOperands || PrecedenceLevel < prec::Assignment) &&
            (!Previous || Previous->isNot(tok::kw_return) || PrecedenceLevel > 0)) {
            NewParenState.Indent = std::max(std::max(State.Column, NewParenState.Indent), CurrentState.LastSpace);
        }
        if (Previous && Previous->endsSequence(tok::l_paren, tok::kw__Generic) && State.Stack.size() > 1) {
            NewParenState.Indent = State.Stack[State.Stack.size() - 2].Indent + S.ContinuationIndentWidth;
        }
        if ((shouldUnindentNextOperator(Current) ||
             (Previous && (PrecedenceLevel == prec::Conditional && Previous->is(tok::question) &&
                           Previous->is(TT::ConditionalExpr)))) &&
            !Newline) {
            if (S.AlignOperands) NewParenState.IsAligned = true;
        }
        if (PrecedenceLevel > prec::Unknown) NewParenState.LastSpace = std::max(NewParenState.LastSpace, State.Column);
        if (PrecedenceLevel != prec::Conditional && Current.isNot(TT::UnaryOperator))
            NewParenState.StartOfFunctionCall = State.Column;
        if (PrecedenceLevel == prec::Conditional && Previous && Previous->is(tok::colon) &&
            Previous->is(TT::ConditionalExpr) && &*It == &Current.FakeLParens.back() &&
            !CurrentState.IsWrappedConditional) {
            NewParenState.IsChainedConditional = true;
            NewParenState.UnindentOperator = State.Stack.back().UnindentOperator;
        } else if (PrecedenceLevel == prec::Conditional ||
                   (!SkipFirstExtraIndent && PrecedenceLevel > prec::Assignment && !Current.isTrailingComment())) {
            NewParenState.Indent += S.ContinuationIndentWidth;
        }
        if ((Previous && !Previous->opensScope()) || PrecedenceLevel != prec::Comma)
            NewParenState.BreakBeforeParameter = false;
        State.Stack.push_back(NewParenState);
        SkipFirstExtraIndent = false;
    }
}

void ContinuationIndenter::moveStatePastFakeRParens(LineState &State) {
    for (unsigned i = 0, e = State.NextToken->FakeRParens; i != e; ++i) {
        unsigned VariablePos = State.Stack.back().VariablePos;
        if (State.Stack.size() == 1) break;
        State.Stack.pop_back();
        State.Stack.back().VariablePos = VariablePos;
    }
}

void ContinuationIndenter::moveStatePastScopeOpener(LineState &State, bool Newline) {
    const FormatToken &Current = *State.NextToken;
    if (!Current.opensScope()) return;
    const ParenState &CurrentState = State.Stack.back();
    if (Current.MatchingParen && Current.is(BK_Block)) {
        moveStateToNewBlock(State, Newline);
        return;
    }
    const bool EndsInComma = [](const FormatToken *Tok) {
        if (!Tok) return false;
        const FormatToken *Prev = Tok->getPreviousNonComment();
        if (!Prev) return false;
        return Prev->is(tok::comma);
    }(Current.MatchingParen);
    unsigned NewIndent;
    unsigned LastSpace = CurrentState.LastSpace;
    bool AvoidBinPacking;
    bool BreakBeforeParameter = false;
    unsigned NestedBlockIndent = std::max(CurrentState.StartOfFunctionCall, CurrentState.NestedBlockIndent);
    if (Current.isOneOf(tok::l_brace, TT::ArrayInitializerLSquare)) {
        if (Current.opensBlockOrBlockTypeList())
            NewIndent = S.IndentWidth + std::min(State.Column, CurrentState.NestedBlockIndent);
        else if (Current.is(tok::l_brace))
            NewIndent = CurrentState.LastSpace + S.ContinuationIndentWidth;  // BracedInitializerIndentWidth: -1
        else
            NewIndent = CurrentState.LastSpace + S.ContinuationIndentWidth;
        const FormatToken *NextNonComment = Current.getNextNonComment();
        AvoidBinPacking = EndsInComma || Current.is(TT::DictLiteral) || !S.BinPackArguments ||
                          (NextNonComment &&
                           NextNonComment->isOneOf(TT::DesignatedInitializerPeriod, TT::DesignatedInitializerLSquare));
        BreakBeforeParameter = EndsInComma;
        if (Current.ParameterCount > 1) NestedBlockIndent = std::max(NestedBlockIndent, State.Column + 1);
    } else {
        NewIndent = S.ContinuationIndentWidth + std::max(CurrentState.LastSpace, CurrentState.StartOfFunctionCall);
        if (Current.is(tok::less) && Current.ParentBracket == tok::l_paren) {
            NewIndent = std::max(NewIndent, CurrentState.Indent);
            LastSpace = std::max(LastSpace, CurrentState.Indent);
        }
        bool BinPackDeclaration = S.BinPackParameters;
        bool GenericSelection =
            Current.getPreviousNonComment() && Current.getPreviousNonComment()->is(tok::kw__Generic);
        AvoidBinPacking = GenericSelection || (State.Line->MustBeDeclaration && !BinPackDeclaration) ||
                          (!State.Line->MustBeDeclaration && !S.BinPackArguments);
        if (Current.is(TT::ObjCMethodExpr) && Current.MatchingParen) {
            if (getLengthToMatchingParen(Current, State.Stack) + State.Column > getColumnLimit(State))
                BreakBeforeParameter = true;
        }
    }
    bool NoLineBreak = Current.Children.empty() && !Current.isOneOf(TT::DictLiteral, TT::ArrayInitializerLSquare) &&
                       (CurrentState.NoLineBreak || CurrentState.NoLineBreakInOperand ||
                        (Current.is(TT::TemplateOpener) && CurrentState.ContainsUnwrappedBuilder));
    State.Stack.push_back(ParenState(&Current, NewIndent, LastSpace, AvoidBinPacking, NoLineBreak));
    ParenState &NewState = State.Stack.back();
    NewState.NestedBlockIndent = NestedBlockIndent;
    NewState.BreakBeforeParameter = BreakBeforeParameter;
    NewState.HasMultipleNestedBlocks = (Current.BlockParameterCount > 1);
}

void ContinuationIndenter::moveStatePastScopeCloser(LineState &State) {
    const FormatToken &Current = *State.NextToken;
    if (!Current.closesScope()) return;
    if (State.Stack.size() > 1 &&
        (Current.isOneOf(tok::r_paren, tok::r_square) ||
         (Current.is(tok::r_brace) && State.NextToken != State.Line->First) ||
         State.NextToken->is(TT::TemplateCloser))) {
        State.Stack.pop_back();
    }
    ParenState &CurrentState = State.Stack.back();
    if (CurrentState.BreakBeforeParameter && Current.MatchingParen && Current.MatchingParen->Previous) {
        const FormatToken &CurrentScopeOpener = *Current.MatchingParen->Previous;
        if (CurrentScopeOpener.is(TT::ObjCMethodExpr) && CurrentScopeOpener.MatchingParen) {
            int NecessarySpaceInLine = static_cast<int>(getLengthToMatchingParen(CurrentScopeOpener, State.Stack)) +
                                       static_cast<int>(CurrentScopeOpener.TotalLength) -
                                       static_cast<int>(Current.TotalLength) - 1;
            if (static_cast<int>(State.Column + Current.ColumnWidth) + NecessarySpaceInLine <=
                static_cast<int>(S.ColumnLimit)) {
                CurrentState.BreakBeforeParameter = false;
            }
        }
    }
    if (Current.is(tok::r_square)) {
        const FormatToken *NextNonComment = Current.getNextNonComment();
        if (NextNonComment && NextNonComment->isNot(tok::l_square)) CurrentState.StartOfArraySubscripts = 0;
    }
}

void ContinuationIndenter::moveStateToNewBlock(LineState &State, bool) {
    unsigned NestedBlockIndent = State.Stack.back().NestedBlockIndent;
    unsigned NewIndent =
        NestedBlockIndent + (State.NextToken->is(TT::ObjCBlockLBrace) ? 2u /*ObjCBlockIndentWidth*/ : S.IndentWidth);
    State.Stack.push_back(ParenState(State.NextToken, NewIndent, State.Stack.back().LastSpace,
                                     /*AvoidBinPacking=*/true, /*NoLineBreak=*/false));
    State.Stack.back().NestedBlockIndent = NestedBlockIndent;
    State.Stack.back().BreakBeforeParameter = true;
}

unsigned ContinuationIndenter::addMultilineToken(const FormatToken &Current, LineState &State) {
    for (ParenState &Paren : State.Stack) Paren.BreakBeforeParameter = true;
    unsigned ColumnsUsed = State.Column;
    State.Column = Current.LastLineColumnWidth;
    if (ColumnsUsed > getColumnLimit(State)) return S.PenaltyExcessCharacter * (ColumnsUsed - getColumnLimit(State));
    return 0;
}

unsigned ContinuationIndenter::handleEndOfLine(const FormatToken &Current, LineState &State, bool DryRun,
                                               bool AllowBreak, bool) {
    unsigned Penalty = 0;
    if (Current.IsMultiline && Current.isNot(TT::BlockComment)) {
        Penalty = addMultilineToken(Current, State);
    } else if (State.Line->Type != LT_ImportStatement) {
        LineState OriginalState = State;
        bool Strict = false;
        bool Exceeded = false;
        std::tie(Penalty, Exceeded) = breakProtrudingToken(Current, State, AllowBreak, /*DryRun=*/true, Strict);
        if (Exceeded) {
            LineState StrictState = OriginalState;
            unsigned StrictPenalty =
                breakProtrudingToken(Current, StrictState, AllowBreak, /*DryRun=*/true, /*Strict=*/true).first;
            Strict = StrictPenalty <= Penalty;
            if (Strict) {
                Penalty = StrictPenalty;
                State = StrictState;
            }
        }
        if (!DryRun) breakProtrudingToken(Current, OriginalState, AllowBreak, /*DryRun=*/false, Strict);
    }
    if (State.Column > getColumnLimit(State)) {
        unsigned ExcessCharacters = State.Column - getColumnLimit(State);
        Penalty += S.PenaltyExcessCharacter * ExcessCharacters;
    }
    return Penalty;
}

std::pair<unsigned, bool> ContinuationIndenter::breakProtrudingToken(const FormatToken &Current, LineState &State,
                                                                     bool AllowBreak, bool DryRun, bool Strict) {
    std::unique_ptr<const BreakableToken> Token;
    {
        unsigned StartColumn = State.Column - Current.ColumnWidth;
        if (Current.isStringLiteral()) {
            if (!S.BreakStringLiterals || !AllowBreak) return {0, false};
            if (State.Line->Type == LT_PreprocessorDirective) return {0, false};
            if (Current.IsUnterminatedLiteral) return {0, false};
            std::string_view Text = Current.TokenText;
            unsigned UnbreakableTailLength = (State.NextToken && canBreak(State)) ? 0 : Current.UnbreakableTailLength;
            std::string_view Prefix, Postfix;
            auto EndsWith = [&](std::string_view P) { return Text.size() >= P.size() && Text.substr(Text.size() - P.size()) == P; };
            auto StartsWith = [&](std::string_view P) { return Text.substr(0, P.size()) == P; };
            if (EndsWith(Postfix = "\"") &&
                (StartsWith(Prefix = "@\"") || StartsWith(Prefix = "\"") || StartsWith(Prefix = "u\"") ||
                 StartsWith(Prefix = "U\"") || StartsWith(Prefix = "u8\"") || StartsWith(Prefix = "L\""))) {
                Token = std::make_unique<BreakableStringLiteral>(Current, StartColumn, Prefix, Postfix,
                                                                 UnbreakableTailLength, State.Line->InPPDirective, S);
            } else if (StartsWith(Prefix = "_T(\"") && EndsWith(Postfix = "\")")) {
                Token = std::make_unique<BreakableStringLiteral>(Current, StartColumn, Prefix, Postfix,
                                                                 UnbreakableTailLength, State.Line->InPPDirective, S);
            } else {
                return {0, false};
            }
        } else if (Current.is(TT::BlockComment)) {
            if (!S.ReflowComments || switchesFormatting(Current)) return {0, false};
            Token = std::make_unique<BreakableBlockComment>(Current, StartColumn, Current.OriginalColumn,
                                                            !Current.Previous, State.Line->InPPDirective, S,
                                                            Whitespaces.useCRLF());
        } else if (Current.is(TT::LineComment) && (!Current.Previous || Current.Previous->isNot(TT::ImplicitStringLiteral))) {
            bool RegularComments = true;
            for (const FormatToken *T = &Current; T && T->is(TT::LineComment); T = T->Next) {
                if (!(T->TokenText.substr(0, 2) == "//" || T->TokenText.substr(0, 1) == "#")) {
                    RegularComments = false;
                    break;
                }
            }
            if (!S.ReflowComments || matchesCommentPragma(Current.TokenText.substr(2)) || switchesFormatting(Current) ||
                !RegularComments) {
                return {0, false};
            }
            Token = std::make_unique<BreakableLineCommentSection>(Current, StartColumn, /*InPPDirective=*/false, S);
        } else {
            return {0, false};
        }
    }
    unsigned ColumnLimit = getColumnLimit(State);
    if (Current.is(TT::LineComment)) ColumnLimit = S.ColumnLimit;
    if (ColumnLimit == 0) ColumnLimit = std::numeric_limits<unsigned>::max();
    if (Current.UnbreakableTailLength >= ColumnLimit) return {0, false};
    unsigned StartColumn = State.Column - Current.ColumnWidth;
    unsigned NewBreakPenalty = Current.isStringLiteral() ? S.PenaltyBreakString : S.PenaltyBreakComment;
    bool Exceeded = false;
    bool BreakInserted = Token->introducesBreakBeforeToken();
    bool NewBreakBefore = false;
    bool Reflow = false;
    unsigned TailOffset = 0;
    unsigned ContentStartColumn = Token->getContentStartColumn(0, /*Break=*/false);
    unsigned RemainingTokenColumns = Token->getRemainingLength(0, TailOffset, ContentStartColumn);
    if (!DryRun) Token->adaptStartOfLine(0, Whitespaces);
    unsigned ContentIndent = 0;
    unsigned Penalty = 0;
    for (unsigned LineIndex = 0, EndIndex = Token->getLineCount(); LineIndex != EndIndex; ++LineIndex) {
        NewBreakBefore = false;
        bool TryReflow = Reflow;
        while (ContentStartColumn + RemainingTokenColumns > ColumnLimit) {
            Split Sp = Token->getSplit(LineIndex, TailOffset, ColumnLimit, ContentStartColumn);
            if (Sp.first == npos) {
                if (LineIndex < EndIndex - 1)
                    Penalty += S.PenaltyExcessCharacter * (ContentStartColumn + RemainingTokenColumns - ColumnLimit);
                break;
            }
            if (Token->supportsReflow()) {
                unsigned ToSplitColumns = Token->getRangeLength(LineIndex, TailOffset, Sp.first, ContentStartColumn);
                Split NextSplit = Token->getSplit(LineIndex, static_cast<unsigned>(TailOffset + Sp.first + Sp.second),
                                                  ColumnLimit, ContentStartColumn + ToSplitColumns + 1);
                unsigned ToNextSplitColumns = 0;
                if (NextSplit.first == npos) {
                    ToNextSplitColumns = Token->getRemainingLength(LineIndex, TailOffset, ContentStartColumn);
                } else {
                    ToNextSplitColumns = Token->getRangeLength(LineIndex, TailOffset,
                                                               Sp.first + Sp.second + NextSplit.first, ContentStartColumn);
                }
                ToNextSplitColumns = Token->getLengthAfterCompression(ToNextSplitColumns, Sp);
                bool ContinueOnLine = ContentStartColumn + ToNextSplitColumns <= ColumnLimit;
                unsigned ExcessCharactersPenalty = 0;
                if (!ContinueOnLine && !Strict) {
                    ExcessCharactersPenalty =
                        (ContentStartColumn + ToNextSplitColumns - ColumnLimit) * S.PenaltyExcessCharacter;
                    if (ExcessCharactersPenalty < NewBreakPenalty) {
                        Exceeded = true;
                        ContinueOnLine = true;
                    }
                }
                if (ContinueOnLine) {
                    TryReflow = true;
                    if (!DryRun) Token->compressWhitespace(LineIndex, TailOffset, Sp, Whitespaces);
                    ContentStartColumn += ToSplitColumns + 1;
                    Penalty += ExcessCharactersPenalty;
                    TailOffset += static_cast<unsigned>(Sp.first + Sp.second);
                    RemainingTokenColumns = Token->getRemainingLength(LineIndex, TailOffset, ContentStartColumn);
                    continue;
                }
            }
            if (!Reflow) ContentIndent = Token->getContentIndent(LineIndex);
            ContentStartColumn = ContentIndent + Token->getContentStartColumn(LineIndex, /*Break=*/true);
            unsigned NewRemainingTokenColumns = Token->getRemainingLength(
                LineIndex, static_cast<unsigned>(TailOffset + Sp.first + Sp.second), ContentStartColumn);
            if (NewRemainingTokenColumns == 0) {
                ContentIndent = 0;
                ContentStartColumn = Token->getContentStartColumn(LineIndex, /*Break=*/true);
                NewRemainingTokenColumns = Token->getRemainingLength(
                    LineIndex, static_cast<unsigned>(TailOffset + Sp.first + Sp.second), ContentStartColumn);
            }
            if (NewRemainingTokenColumns >= RemainingTokenColumns) break;
            if (!DryRun) Token->insertBreak(LineIndex, TailOffset, Sp, ContentIndent, Whitespaces);
            Penalty += NewBreakPenalty;
            TailOffset += static_cast<unsigned>(Sp.first + Sp.second);
            RemainingTokenColumns = NewRemainingTokenColumns;
            BreakInserted = true;
            NewBreakBefore = true;
        }
        if (LineIndex + 1 != EndIndex) {
            unsigned NextLineIndex = LineIndex + 1;
            if (NewBreakBefore) TryReflow = true;
            if (TryReflow) {
                Reflow = false;
                ContentStartColumn += RemainingTokenColumns + 1;
                Split SplitBeforeNext = Token->getReflowSplit(NextLineIndex);
                if (SplitBeforeNext.first != npos) {
                    TailOffset = static_cast<unsigned>(SplitBeforeNext.first + SplitBeforeNext.second);
                    RemainingTokenColumns = Token->getRemainingLength(NextLineIndex, TailOffset, ContentStartColumn);
                    Reflow = true;
                    if (ContentStartColumn + RemainingTokenColumns > ColumnLimit) {
                        Split Sp = Token->getSplit(NextLineIndex, TailOffset, ColumnLimit, ContentStartColumn);
                        if (Sp.first == npos) {
                            Reflow = false;
                        } else {
                            unsigned ToSplitColumns =
                                Token->getRangeLength(NextLineIndex, TailOffset, Sp.first, ContentStartColumn);
                            if (ContentStartColumn + ToSplitColumns > ColumnLimit) {
                                unsigned ExcessCharactersPenalty =
                                    (ContentStartColumn + ToSplitColumns - ColumnLimit) * S.PenaltyExcessCharacter;
                                if (NewBreakPenalty < ExcessCharactersPenalty) Reflow = false;
                            }
                        }
                    }
                }
            }
            if (!Reflow) {
                TailOffset = 0;
                ContentStartColumn = Token->getContentStartColumn(NextLineIndex, /*Break=*/false);
                RemainingTokenColumns = Token->getRemainingLength(NextLineIndex, TailOffset, ContentStartColumn);
                if (!DryRun) Token->adaptStartOfLine(NextLineIndex, Whitespaces);
            } else {
                if (NewBreakBefore) Penalty -= NewBreakPenalty;
                if (!DryRun) Token->reflow(NextLineIndex, Whitespaces);
            }
        }
    }
    Split SplitAfterLastLine = Token->getSplitAfterLastLine(TailOffset);
    if (SplitAfterLastLine.first != npos) {
        Penalty += S.PenaltyExcessCharacter * (ContentStartColumn + RemainingTokenColumns - ColumnLimit);
        if (!DryRun) Token->replaceWhitespaceAfterLastLine(TailOffset, SplitAfterLastLine, Whitespaces);
        ContentStartColumn = Token->getContentStartColumn(Token->getLineCount() - 1, /*Break=*/true);
        RemainingTokenColumns = Token->getRemainingLength(
            Token->getLineCount() - 1, static_cast<unsigned>(TailOffset + SplitAfterLastLine.first + SplitAfterLastLine.second),
            ContentStartColumn);
    }
    State.Column = ContentStartColumn + RemainingTokenColumns - Current.UnbreakableTailLength;
    if (BreakInserted) {
        if (!DryRun) Token->updateAfterBroken(Whitespaces);
        if (Current.isNot(TT::LineComment))
            for (ParenState &Paren : State.Stack) Paren.BreakBeforeParameter = true;
        if (Current.is(TT::BlockComment)) State.NoContinuation = true;
        State.Stack.back().LastSpace = StartColumn;
    }
    Token->updateNextToken(State);
    return {Penalty, Exceeded};
}

unsigned ContinuationIndenter::getColumnLimit(const LineState &State) const {
    return S.ColumnLimit - (State.Line->InPPDirective ? 2 : 0);
}

// --- CommaSeparatedList -----------------------------------------------------------------

namespace {

unsigned CodePointsBetween(const FormatToken *Begin, const FormatToken *End) {
    return End->TotalLength - Begin->TotalLength + Begin->ColumnWidth;
}

class CommaSeparatedList : public TokenRole {
public:
    explicit CommaSeparatedList(const Style &S) : S(S) {}

    void CommaFound(const FormatToken *Token) override { Commas.push_back(Token); }
    const FormatToken *lastComma() override { return Commas.empty() ? nullptr : Commas.back(); }

    unsigned formatFromToken(LineState &State, ContinuationIndenter *, bool) override {
        if (Formats.size() == 1 || HasNestedBracedList) State.Stack.back().AvoidBinPacking = true;
        return 0;
    }

    unsigned formatAfterToken(LineState &State, ContinuationIndenter *Indenter, bool DryRun) override {
        if (!State.NextToken || !State.NextToken->Previous) return 0;
        if (Formats.size() <= 1) return 0;
        const FormatToken *LBrace = State.NextToken->Previous->getPreviousNonComment();
        if (!LBrace || !LBrace->isOneOf(tok::l_brace, TT::ArrayInitializerLSquare) || LBrace->is(BK_Block) ||
            LBrace->is(TT::DictLiteral) || LBrace->Next->is(TT::DesignatedInitializerPeriod)) {
            return 0;
        }
        unsigned RemainingCodePoints = S.ColumnLimit - State.Column + State.NextToken->Previous->ColumnWidth;
        const ColumnFormat *Format = getColumnFormat(RemainingCodePoints);
        if (!Format) return 10000;
        unsigned Penalty = 0;
        unsigned Column = 0;
        unsigned Item = 0;
        while (State.NextToken != LBrace->MatchingParen) {
            bool NewLine = false;
            unsigned ExtraSpaces = 0;
            if (Item < Commas.size() && State.NextToken->Previous == Commas[Item]) {
                if (!State.NextToken->isTrailingComment()) {
                    ExtraSpaces += Format->ColumnSizes[Column] - ItemLengths[Item];
                    ++Column;
                }
                ++Item;
            }
            if (Column == Format->Columns || State.NextToken->MustBreakBefore) {
                Column = 0;
                NewLine = true;
            }
            Penalty += Indenter->addTokenToState(State, NewLine, DryRun, ExtraSpaces);
        }
        return Penalty;
    }

    void precomputeFormattingInfos(const FormatToken *Token) override {
        if (!Token->MatchingParen || !Token->isOneOf(tok::l_brace, TT::ArrayInitializerLSquare)) return;
        // Cpp11BracedListStyle with BinPackArguments: no early return.
        if (Token->is(TT::ArrayInitializerLSquare) && Commas.size() < 19) return;
        const FormatToken *ItemBegin = Token->Next;
        while (ItemBegin->isTrailingComment()) ItemBegin = ItemBegin->Next;
        std::vector<bool> MustBreakBeforeItem;
        std::vector<unsigned> EndOfLineItemLength;
        bool HasSeparatingComment = false;
        for (unsigned i = 0, e = static_cast<unsigned>(Commas.size()) + 1; i != e; ++i) {
            while (ItemBegin->HasUnescapedNewline && ItemBegin->isTrailingComment()) {
                ItemBegin = ItemBegin->Next;
                HasSeparatingComment = i > 0;
            }
            MustBreakBeforeItem.push_back(ItemBegin->MustBreakBefore);
            if (ItemBegin->is(tok::l_brace)) HasNestedBracedList = true;
            const FormatToken *ItemEnd = nullptr;
            if (i == Commas.size()) {
                ItemEnd = Token->MatchingParen;
                const FormatToken *NonCommentEnd = ItemEnd->getPreviousNonComment();
                ItemLengths.push_back(CodePointsBetween(ItemBegin, NonCommentEnd));
                if (!ItemEnd->Previous->isTrailingComment()) {
                    while (ItemEnd->Next && !ItemEnd->Next->CanBreakBefore) ItemEnd = ItemEnd->Next;
                } else {
                    ItemEnd = Token->MatchingParen->Previous;
                }
            } else {
                ItemEnd = Commas[i];
                ItemLengths.push_back(CodePointsBetween(ItemBegin, ItemEnd));
                if (ItemEnd->Next && !ItemEnd->Next->HasUnescapedNewline && ItemEnd->Next->isTrailingComment())
                    ItemEnd = ItemEnd->Next;
            }
            EndOfLineItemLength.push_back(CodePointsBetween(ItemBegin, ItemEnd));
            if (ItemEnd->getNextNonComment() == Token->MatchingParen) break;
            ItemBegin = ItemEnd->Next;
        }
        if (Commas.size() < 5 || HasSeparatingComment) return;
        if (Token->NestingLevel != 0 && Token->is(tok::l_brace) && Commas.size() < 19) return;
        unsigned MaxItems = S.ColumnLimit / 3;
        std::vector<unsigned> MinSizeInColumn;
        for (unsigned Columns = 1; Columns <= MaxItems; ++Columns) {
            ColumnFormat Format;
            Format.Columns = Columns;
            Format.ColumnSizes.resize(Columns);
            MinSizeInColumn.assign(Columns, UINT_MAX);
            Format.LineCount = 1;
            bool HasRowWithSufficientColumns = false;
            unsigned Column = 0;
            for (unsigned i = 0, e = static_cast<unsigned>(ItemLengths.size()); i != e; ++i) {
                if (MustBreakBeforeItem[i] || Column == Columns) {
                    ++Format.LineCount;
                    Column = 0;
                }
                if (Column == Columns - 1) HasRowWithSufficientColumns = true;
                unsigned Length = (Column == Columns - 1) ? EndOfLineItemLength[i] : ItemLengths[i];
                Format.ColumnSizes[Column] = std::max(Format.ColumnSizes[Column], Length);
                MinSizeInColumn[Column] = std::min(MinSizeInColumn[Column], Length);
                ++Column;
            }
            if (!HasRowWithSufficientColumns) break;
            Format.TotalWidth = Columns - 1;
            for (unsigned i = 0; i < Columns; ++i) Format.TotalWidth += Format.ColumnSizes[i];
            bool TooWide = false;
            for (unsigned i = 0; i + 1 < Columns; ++i)
                if (Format.ColumnSizes[i] - MinSizeInColumn[i] > 10) TooWide = true;
            if (TooWide) continue;
            if (Format.TotalWidth > S.ColumnLimit && Columns > 1) continue;
            Formats.push_back(Format);
        }
    }

private:
    struct ColumnFormat {
        unsigned Columns = 0;
        unsigned TotalWidth = 0;
        unsigned LineCount = 0;
        std::vector<unsigned> ColumnSizes;
    };

    const ColumnFormat *getColumnFormat(unsigned RemainingCharacters) const {
        const ColumnFormat *BestFormat = nullptr;
        for (auto It = Formats.rbegin(); It != Formats.rend(); ++It) {
            const ColumnFormat &Format = *It;
            if (Format.TotalWidth <= RemainingCharacters || Format.Columns == 1) {
                if (BestFormat && Format.LineCount > BestFormat->LineCount) break;
                BestFormat = &Format;
            }
        }
        return BestFormat;
    }

    const Style &S;
    std::vector<const FormatToken *> Commas;
    std::vector<unsigned> ItemLengths;
    std::vector<ColumnFormat> Formats;
    bool HasNestedBracedList = false;
};

}  // namespace

std::shared_ptr<TokenRole> makeCommaSeparatedList(const Style &S) { return std::make_shared<CommaSeparatedList>(S); }

}  // namespace cfmt::detail
