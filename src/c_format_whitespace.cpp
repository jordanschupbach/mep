// The whitespace pass of mep's C formatter: the port of clang-format's
// WhitespaceManager. The line formatter records, for each token, the
// newlines and spaces it wants before it (and, inside comments and strings,
// the breaks it inserts); this pass then lines up runs of trailing comments,
// the `?`/`:` of chained conditionals and the backslashes that continue a
// macro, and turns everything into text replacements.

#include "c_format_internal.h"

#include <algorithm>
#include <climits>
#include <tuple>

namespace cfmt::detail {

namespace {

size_t tokenStart(const Source &Src, const FormatToken &Tok) {
    return static_cast<size_t>(Tok.TokenText.data() - Src.Text.data());
}

std::tuple<unsigned, unsigned, int> indentAndNestingLevel(const Change &C) {
    return std::make_tuple(C.Tok->IndentLevel, C.Tok->NestingLevel, C.ConditionalsLevel);
}

template <typename F>
void AlignTokenSequence(const Style &S, unsigned Start, unsigned End, unsigned Column, bool RightJustify, F &&Matches,
                        std::vector<Change> &Changes) {
    bool FoundMatchOnLine = false;
    int Shift = 0;
    std::vector<unsigned> ScopeStack;
    for (unsigned i = Start; i != End; ++i) {
        Change &CurrentChange = Changes[i];
        if (!ScopeStack.empty() &&
            indentAndNestingLevel(CurrentChange) < indentAndNestingLevel(Changes[ScopeStack.back()])) {
            ScopeStack.pop_back();
        }
        unsigned PreviousNonComment = i - 1;
        while (PreviousNonComment > Start && Changes[PreviousNonComment].Tok->is(tok::comment)) --PreviousNonComment;
        if (i != Start && indentAndNestingLevel(CurrentChange) > indentAndNestingLevel(Changes[PreviousNonComment]))
            ScopeStack.push_back(i);
        bool InsideNestedScope = !ScopeStack.empty();
        bool ContinuedStringLiteral =
            i > Start && CurrentChange.Tok->is(tok::string_literal) && Changes[i - 1].Tok->is(tok::string_literal);
        bool SkipMatchCheck = InsideNestedScope || ContinuedStringLiteral;
        if (CurrentChange.NewlinesBefore > 0 && !SkipMatchCheck) {
            Shift = 0;
            FoundMatchOnLine = false;
        }
        if (!FoundMatchOnLine && !SkipMatchCheck && Matches(CurrentChange)) {
            FoundMatchOnLine = true;
            Shift = static_cast<int>(Column) - static_cast<int>(RightJustify ? CurrentChange.TokenLength : 0) -
                    static_cast<int>(CurrentChange.StartOfTokenColumn);
            CurrentChange.Spaces += Shift;
            if (CurrentChange.NewlinesBefore == 0)
                CurrentChange.Spaces =
                    std::max(CurrentChange.Spaces, static_cast<int>(CurrentChange.Tok->SpacesRequiredBefore));
        }
        if (Shift == 0) continue;
        if (InsideNestedScope && CurrentChange.NewlinesBefore > 0) {
            unsigned ScopeStart = ScopeStack.back();
            auto ShouldShiftBeAdded = [&] {
                if (Changes[ScopeStart - 1].Tok->is(TT::FunctionDeclarationName)) return true;
                if (Changes[ScopeStart - 1].Tok->is(TT::LambdaLBrace)) return false;
                if (ScopeStart > Start + 1 && Changes[ScopeStart - 2].Tok->is(TT::FunctionDeclarationName)) return true;
                if (ScopeStart > Start + 1 && Changes[ScopeStart - 2].Tok->isOneOf(tok::identifier, TT::TemplateCloser) &&
                    Changes[ScopeStart - 1].Tok->is(tok::l_paren) && Changes[ScopeStart].Tok->isNot(TT::LambdaLSquare)) {
                    if (CurrentChange.Tok->MatchingParen && CurrentChange.Tok->MatchingParen->is(TT::LambdaLBrace))
                        return false;
                    if (Changes[ScopeStart].NewlinesBefore > 0) return false;
                    if (CurrentChange.Tok->is(tok::l_brace) && CurrentChange.Tok->is(BK_BracedInit)) return true;
                    return S.BinPackArguments;
                }
                if (CurrentChange.Tok->is(TT::ConditionalExpr)) return true;
                if (CurrentChange.Tok->is(TT::DesignatedInitializerPeriod)) return true;
                if (CurrentChange.Tok->Previous && CurrentChange.Tok->Previous->is(TT::ConditionalExpr)) return true;
                if (ScopeStart > Start + 1 && Changes[ScopeStart - 2].Tok->is(tok::identifier) &&
                    Changes[ScopeStart - 1].Tok->is(tok::l_brace) && CurrentChange.Tok->is(tok::l_brace) &&
                    CurrentChange.Tok->is(BK_BracedInit)) {
                    return true;
                }
                if (ScopeStart > Start + 1 && Changes[ScopeStart - 2].Tok->isNot(tok::identifier) &&
                    Changes[ScopeStart - 1].Tok->is(tok::l_brace) && CurrentChange.Tok->isNot(tok::r_brace)) {
                    for (auto It = ScopeStack.rbegin(); It != ScopeStack.rend(); ++It) {
                        unsigned OuterScopeStart = *It;
                        if (OuterScopeStart > Start && Changes[OuterScopeStart - 1].Tok->is(TT::LambdaLBrace))
                            return false;
                    }
                    if (Changes[ScopeStart].NewlinesBefore > 0) return false;
                    return true;
                }
                if (Changes[ScopeStart - 1].Tok->is(TT::TemplateOpener)) return true;
                return false;
            };
            if (ShouldShiftBeAdded()) CurrentChange.Spaces += Shift;
        }
        if (ContinuedStringLiteral) CurrentChange.Spaces += Shift;
        CurrentChange.StartOfTokenColumn = static_cast<unsigned>(static_cast<int>(CurrentChange.StartOfTokenColumn) + Shift);
        if (i + 1 != Changes.size())
            Changes[i + 1].PreviousEndOfTokenColumn =
                static_cast<unsigned>(static_cast<int>(Changes[i + 1].PreviousEndOfTokenColumn) + Shift);
        if (S.PointerAlignment == Style::PAS_Right && CurrentChange.Spaces != 0 &&
            !CurrentChange.Tok->isOneOf(tok::equal, tok::r_paren, TT::TemplateCloser)) {
            for (int Previous = static_cast<int>(i) - 1;
                 Previous >= 0 && Changes[static_cast<size_t>(Previous)].Tok->is(TT::PointerOrReference); --Previous) {
                Changes[static_cast<size_t>(Previous + 1)].Spaces -= Shift;
                Changes[static_cast<size_t>(Previous)].Spaces += Shift;
                Changes[static_cast<size_t>(Previous)].StartOfTokenColumn =
                    static_cast<unsigned>(static_cast<int>(Changes[static_cast<size_t>(Previous)].StartOfTokenColumn) + Shift);
            }
        }
    }
}

template <typename F>
unsigned AlignTokens(const Style &S, F &&Matches, std::vector<Change> &Changes, unsigned StartAt) {
    unsigned WidthLeft = 0;
    unsigned WidthAnchor = 0;
    unsigned WidthRight = 0;
    unsigned StartOfSequence = 0;
    unsigned EndOfSequence = 0;
    auto IndentAndNestingLevel =
        StartAt < Changes.size() ? indentAndNestingLevel(Changes[StartAt]) : std::tuple<unsigned, unsigned, int>();
    unsigned CommasBeforeLastMatch = 0;
    unsigned CommasBeforeMatch = 0;
    bool FoundMatchOnLine = false;
    bool LineIsComment = true;
    auto AlignCurrentSequence = [&] {
        if (StartOfSequence > 0 && StartOfSequence < EndOfSequence)
            AlignTokenSequence(S, StartOfSequence, EndOfSequence, WidthLeft + WidthAnchor, false, Matches, Changes);
        WidthLeft = 0;
        WidthAnchor = 0;
        WidthRight = 0;
        StartOfSequence = 0;
        EndOfSequence = 0;
    };
    unsigned i = StartAt;
    for (unsigned e = static_cast<unsigned>(Changes.size()); i != e; ++i) {
        Change &CurrentChange = Changes[i];
        if (indentAndNestingLevel(CurrentChange) < IndentAndNestingLevel) break;
        if (CurrentChange.NewlinesBefore != 0) {
            CommasBeforeMatch = 0;
            EndOfSequence = i;
            bool EmptyLineBreak = CurrentChange.NewlinesBefore > 1;
            bool NoMatchBreak = !FoundMatchOnLine;
            if (EmptyLineBreak || NoMatchBreak) AlignCurrentSequence();
            if (i == 0 || CurrentChange.Tok->isNot(tok::string_literal) || Changes[i - 1].Tok->isNot(tok::string_literal))
                FoundMatchOnLine = false;
            LineIsComment = true;
        }
        if (CurrentChange.Tok->isNot(tok::comment)) LineIsComment = false;
        if (CurrentChange.Tok->is(tok::comma)) {
            ++CommasBeforeMatch;
        } else if (indentAndNestingLevel(CurrentChange) > IndentAndNestingLevel) {
            unsigned StoppedAt = AlignTokens(S, Matches, Changes, i);
            i = StoppedAt - 1;
            continue;
        }
        if (!Matches(CurrentChange)) continue;
        if (FoundMatchOnLine || CommasBeforeMatch != CommasBeforeLastMatch) AlignCurrentSequence();
        CommasBeforeLastMatch = CommasBeforeMatch;
        FoundMatchOnLine = true;
        if (StartOfSequence == 0) StartOfSequence = i;
        unsigned ChangeWidthLeft = CurrentChange.StartOfTokenColumn;
        unsigned ChangeWidthAnchor = 0;
        unsigned ChangeWidthRight = CurrentChange.TokenLength;
        for (unsigned j = i + 1; j != e && Changes[j].NewlinesBefore == 0; ++j) {
            ChangeWidthRight += static_cast<unsigned>(Changes[j].Spaces);
            if (!Changes[j].IsInsideToken) ChangeWidthRight += Changes[j].TokenLength;
        }
        unsigned NewLeft = std::max(ChangeWidthLeft, WidthLeft);
        unsigned NewAnchor = std::max(ChangeWidthAnchor, WidthAnchor);
        unsigned NewRight = std::max(ChangeWidthRight, WidthRight);
        if (S.ColumnLimit != 0 && S.ColumnLimit < NewLeft + NewAnchor + NewRight) {
            AlignCurrentSequence();
            StartOfSequence = i;
            WidthLeft = ChangeWidthLeft;
            WidthAnchor = ChangeWidthAnchor;
            WidthRight = ChangeWidthRight;
        } else {
            WidthLeft = NewLeft;
            WidthAnchor = NewAnchor;
            WidthRight = NewRight;
        }
    }
    (void)LineIsComment;
    EndOfSequence = i;
    AlignCurrentSequence();
    return i;
}

}  // namespace

void WhitespaceManager::replaceWhitespace(FormatToken &Tok, unsigned Newlines, unsigned Spaces,
                                          unsigned StartOfTokenColumn, bool IsAligned, bool InPPDirective) {
    if (Tok.Finalized) return;
    Tok.Decision = Newlines > 0 ? FD_Break : FD_Continue;
    Change C;
    C.Tok = &Tok;
    C.CreateReplacement = true;
    C.OriginalStart = Tok.WhitespaceStart;
    C.OriginalEnd = Tok.WhitespaceEnd;
    C.Spaces = static_cast<int>(Spaces);
    C.StartOfTokenColumn = StartOfTokenColumn;
    C.NewlinesBefore = Newlines;
    C.IsAligned = IsAligned;
    C.ContinuesPPDirective = InPPDirective && !Tok.IsFirst;
    Changes.push_back(std::move(C));
}

void WhitespaceManager::addUntouchableToken(const FormatToken &Tok, bool InPPDirective) {
    if (Tok.Finalized) return;
    Change C;
    C.Tok = &Tok;
    C.CreateReplacement = false;
    C.OriginalStart = Tok.WhitespaceStart;
    C.OriginalEnd = Tok.WhitespaceEnd;
    C.Spaces = 0;
    C.StartOfTokenColumn = Tok.OriginalColumn;
    C.NewlinesBefore = Tok.NewlinesBefore;
    C.ContinuesPPDirective = InPPDirective && !Tok.IsFirst;
    Changes.push_back(std::move(C));
}

void WhitespaceManager::replaceWhitespaceInToken(const FormatToken &Tok, unsigned Offset, unsigned ReplaceChars,
                                                 std::string_view PreviousPostfix, std::string_view CurrentPrefix,
                                                 bool InPPDirective, unsigned Newlines, int Spaces) {
    if (Tok.Finalized) return;
    size_t Start = tokenStart(Src_, Tok) + Offset;
    Change C;
    C.Tok = &Tok;
    C.CreateReplacement = true;
    C.OriginalStart = Start;
    C.OriginalEnd = Start + ReplaceChars;
    C.Spaces = Spaces;
    C.StartOfTokenColumn = static_cast<unsigned>(std::max(0, Spaces));
    C.NewlinesBefore = Newlines;
    C.PreviousLinePostfix = std::string(PreviousPostfix);
    C.CurrentLinePrefix = std::string(CurrentPrefix);
    C.IsAligned = true;
    C.ContinuesPPDirective = InPPDirective && !Tok.IsFirst;
    C.IsInsideToken = true;
    Changes.push_back(std::move(C));
}

std::vector<Replacement> WhitespaceManager::generateReplacements() {
    std::vector<Replacement> Out;
    if (Changes.empty()) return Out;
    std::stable_sort(Changes.begin(), Changes.end(), [](const Change &C1, const Change &C2) {
        return C1.OriginalStart < C2.OriginalStart ||
               (C1.OriginalStart == C2.OriginalStart && C1.OriginalEnd < C2.OriginalEnd);
    });
    calculateLineBreakInformation();
    alignChainedConditionals();
    alignTrailingComments();
    alignEscapedNewlines();
    generateChanges(Out);
    return Out;
}

void WhitespaceManager::calculateLineBreakInformation() {
    Changes[0].PreviousEndOfTokenColumn = 0;
    Change *LastOutsideTokenChange = &Changes[0];
    for (size_t I = 1, e = Changes.size(); I != e; ++I) {
        Change &C = Changes[I];
        Change &P = Changes[I - 1];
        unsigned &PrevTokLength = P.TokenLength;
        size_t OriginalWhitespaceStartOffset = C.OriginalStart;
        size_t PreviousOriginalWhitespaceEndOffset = P.OriginalEnd;
        std::string_view Text = Src_.Text.substr(PreviousOriginalWhitespaceEndOffset,
                                                 OriginalWhitespaceStartOffset - PreviousOriginalWhitespaceEndOffset);
        size_t NewlinePos = Text.find_first_of('\n');
        if (NewlinePos == std::string_view::npos) {
            PrevTokLength = static_cast<unsigned>(OriginalWhitespaceStartOffset - PreviousOriginalWhitespaceEndOffset +
                                                  C.PreviousLinePostfix.size() + P.CurrentLinePrefix.size());
            if (!P.IsInsideToken) PrevTokLength = std::min(PrevTokLength, P.Tok->ColumnWidth);
        } else {
            PrevTokLength = static_cast<unsigned>(NewlinePos + P.CurrentLinePrefix.size());
        }
        if (P.IsInsideToken && P.NewlinesBefore == 0)
            LastOutsideTokenChange->TokenLength += PrevTokLength + static_cast<unsigned>(P.Spaces);
        else
            LastOutsideTokenChange = &P;
        C.PreviousEndOfTokenColumn = P.StartOfTokenColumn + PrevTokLength;
        P.IsTrailingComment = (C.NewlinesBefore > 0 || C.Tok->is(tok::eof) ||
                               (C.IsInsideToken && C.Tok->is(tok::comment))) &&
                              P.Tok->is(tok::comment) && OriginalWhitespaceStartOffset != PreviousOriginalWhitespaceEndOffset;
    }
    Changes.back().TokenLength = 0;
    Changes.back().IsTrailingComment = Changes.back().Tok->is(tok::comment);
    const Change *LastBlockComment = nullptr;
    for (Change &C : Changes) {
        if (C.IsInsideToken && C.NewlinesBefore == 0) C.IsTrailingComment = false;
        C.StartOfBlockComment = nullptr;
        C.IndentationOffset = 0;
        if (C.Tok->is(tok::comment)) {
            if (C.Tok->is(TT::LineComment) || !C.IsInsideToken) {
                LastBlockComment = &C;
            } else if ((C.StartOfBlockComment = LastBlockComment)) {
                C.IndentationOffset =
                    static_cast<int>(C.StartOfTokenColumn) - static_cast<int>(C.StartOfBlockComment->StartOfTokenColumn);
            }
        } else {
            LastBlockComment = nullptr;
        }
    }
    std::vector<bool> ScopeStack;
    int ConditionalsLevel = 0;
    for (Change &C : Changes) {
        for (size_t i = 0, e = C.Tok->FakeLParens.size(); i != e; ++i) {
            bool isNestedConditional = C.Tok->FakeLParens[e - 1 - i] == prec::Conditional &&
                                       !(i == 0 && C.Tok->Previous && C.Tok->Previous->is(TT::ConditionalExpr) &&
                                         C.Tok->Previous->is(tok::colon));
            if (isNestedConditional) ++ConditionalsLevel;
            ScopeStack.push_back(isNestedConditional);
        }
        C.ConditionalsLevel = ConditionalsLevel;
        for (unsigned i = C.Tok->FakeRParens; i > 0 && !ScopeStack.empty(); --i) {
            bool Back = ScopeStack.back();
            ScopeStack.pop_back();
            if (Back) --ConditionalsLevel;
        }
    }
}

void WhitespaceManager::alignChainedConditionals() {
    if (Style_.BreakBeforeTernaryOperators) {
        AlignTokens(
            Style_,
            [](const Change &C) {
                return C.Tok->is(TT::ConditionalExpr) &&
                       ((C.Tok->is(tok::question) && !C.NewlinesBefore) ||
                        (C.Tok->is(tok::colon) && C.Tok->Next &&
                         (C.Tok->Next->FakeLParens.empty() || C.Tok->Next->FakeLParens.back() != prec::Conditional)));
            },
            Changes, 0);
    }
}

void WhitespaceManager::alignTrailingComments() {
    if (!Style_.AlignTrailingComments) return;
    const int Size = static_cast<int>(Changes.size());
    int MinColumn = 0;
    int StartOfSequence = 0;
    bool BreakBeforeNext = false;
    int NewLineThreshold = 1;  // OverEmptyLines: 0
    auto spellingColumn = [&](size_t Offset) {
        size_t LineStart = Src_.Text.rfind('\n', Offset == 0 ? 0 : Offset - 1);
        if (Offset == 0 || LineStart == std::string_view::npos) LineStart = 0;
        else ++LineStart;
        return static_cast<int>(Offset - LineStart + 1);
    };
    for (int I = 0, MaxColumn = INT_MAX, Newlines = 0; I < Size; ++I) {
        Change &C = Changes[static_cast<size_t>(I)];
        if (C.StartOfBlockComment) continue;
        Newlines += static_cast<int>(C.NewlinesBefore);
        if (!C.IsTrailingComment) continue;
        const int ChangeMinColumn = static_cast<int>(C.StartOfTokenColumn);
        int ChangeMaxColumn;
        if (!C.CreateReplacement)
            ChangeMaxColumn = ChangeMinColumn;
        else if (Style_.ColumnLimit >= C.TokenLength)
            ChangeMaxColumn = static_cast<int>(Style_.ColumnLimit - C.TokenLength);
        else
            ChangeMaxColumn = ChangeMinColumn;
        if (I + 1 < Size && Changes[static_cast<size_t>(I + 1)].ContinuesPPDirective && ChangeMaxColumn >= 2) ChangeMaxColumn -= 2;
        bool WasAlignedWithStartOfNextLine = false;
        if (C.NewlinesBefore >= 1) {
            const int CommentColumn = spellingColumn(C.OriginalEnd);
            for (int J = I + 1; J < Size; ++J) {
                if (Changes[static_cast<size_t>(J)].Tok->is(tok::comment)) continue;
                const int NextColumn = spellingColumn(Changes[static_cast<size_t>(J)].OriginalEnd);
                WasAlignedWithStartOfNextLine =
                    CommentColumn == NextColumn || CommentColumn == NextColumn + static_cast<int>(Style_.IndentWidth);
                break;
            }
        }
        auto DontAlignThisComment = [](const FormatToken *Tok) {
            if (Tok->is(tok::semi)) {
                Tok = Tok->getPreviousNonComment();
                if (!Tok) return false;
            }
            if (Tok->is(tok::r_paren)) {
                Tok = Tok->MatchingParen;
                if (!Tok) return false;
                Tok = Tok->getPreviousNonComment();
                if (!Tok) return false;
                if (Tok->is(TT::DoWhile)) {
                    const FormatToken *Prev = Tok->getPreviousNonComment();
                    if (!Prev) return true;
                    Tok = Prev;
                }
            }
            if (Tok->isNot(tok::r_brace)) return false;
            while (Tok->Previous && Tok->Previous->is(tok::r_brace)) Tok = Tok->Previous;
            return Tok->NewlinesBefore > 0;
        };
        if (I > 0 && C.NewlinesBefore == 0 && DontAlignThisComment(Changes[static_cast<size_t>(I - 1)].Tok)) {
            alignTrailingComments(static_cast<unsigned>(StartOfSequence), static_cast<unsigned>(I),
                                  static_cast<unsigned>(MinColumn));
            MinColumn = 0;
            MaxColumn = INT_MAX;
            StartOfSequence = I + 1;
        } else if (BreakBeforeNext || Newlines > NewLineThreshold ||
                   (ChangeMinColumn > MaxColumn || ChangeMaxColumn < MinColumn) ||
                   (C.NewlinesBefore == 1 && I > 0 && !Changes[static_cast<size_t>(I - 1)].IsTrailingComment) ||
                   WasAlignedWithStartOfNextLine) {
            alignTrailingComments(static_cast<unsigned>(StartOfSequence), static_cast<unsigned>(I),
                                  static_cast<unsigned>(MinColumn));
            MinColumn = ChangeMinColumn;
            MaxColumn = ChangeMaxColumn;
            StartOfSequence = I;
        } else {
            MinColumn = std::max(MinColumn, ChangeMinColumn);
            MaxColumn = std::min(MaxColumn, ChangeMaxColumn);
        }
        BreakBeforeNext = (I == 0) || (C.NewlinesBefore > 1) || (C.NewlinesBefore == 1 && StartOfSequence == I);
        Newlines = 0;
    }
    alignTrailingComments(static_cast<unsigned>(StartOfSequence), static_cast<unsigned>(Size),
                          static_cast<unsigned>(MinColumn));
}

void WhitespaceManager::alignTrailingComments(unsigned Start, unsigned End, unsigned Column) {
    for (unsigned i = Start; i != End; ++i) {
        int Shift = 0;
        if (Changes[i].IsTrailingComment) Shift = static_cast<int>(Column) - static_cast<int>(Changes[i].StartOfTokenColumn);
        if (Changes[i].StartOfBlockComment) {
            Shift = Changes[i].IndentationOffset + static_cast<int>(Changes[i].StartOfBlockComment->StartOfTokenColumn) -
                    static_cast<int>(Changes[i].StartOfTokenColumn);
        }
        if (Shift <= 0) continue;
        Changes[i].Spaces += Shift;
        if (i + 1 != Changes.size()) Changes[i + 1].PreviousEndOfTokenColumn += static_cast<unsigned>(Shift);
        Changes[i].StartOfTokenColumn += static_cast<unsigned>(Shift);
    }
}

void WhitespaceManager::alignEscapedNewlines() {
    const bool AlignLeft = !Style_.AlignEscapedNewlinesRight;
    const unsigned MaxColumn = Style_.ColumnLimit;
    unsigned MaxEndOfLine = AlignLeft ? 0 : MaxColumn;
    unsigned StartOfMacro = 0;
    for (unsigned i = 1, e = static_cast<unsigned>(Changes.size()); i < e; ++i) {
        Change &C = Changes[i];
        if (C.NewlinesBefore == 0) continue;
        const bool InPPDirective = C.ContinuesPPDirective;
        const unsigned BackslashColumn = C.PreviousEndOfTokenColumn + 2;
        if (InPPDirective) MaxEndOfLine = std::max(BackslashColumn, MaxEndOfLine);
        if (!InPPDirective) {
            alignEscapedNewlines(StartOfMacro + 1, i, MaxEndOfLine);
            MaxEndOfLine = AlignLeft ? 0 : MaxColumn;
            StartOfMacro = i;
        }
    }
    alignEscapedNewlines(StartOfMacro + 1, static_cast<unsigned>(Changes.size()), MaxEndOfLine);
}

void WhitespaceManager::alignEscapedNewlines(unsigned Start, unsigned End, unsigned Column) {
    for (unsigned i = Start; i < End; ++i) {
        Change &C = Changes[i];
        if (C.NewlinesBefore > 0) {
            if (C.PreviousEndOfTokenColumn + 1 > Column)
                C.EscapedNewlineColumn = 0;
            else
                C.EscapedNewlineColumn = Column;
        }
    }
}

void WhitespaceManager::generateChanges(std::vector<Replacement> &Out) {
    for (size_t i = 0, e = Changes.size(); i != e; ++i) {
        const Change &C = Changes[i];
        if (i > 0) {
            const Change &Last = Changes[i - 1];
            if (Last.OriginalStart == C.OriginalStart &&
                (Last.OriginalEnd != Last.OriginalStart || C.OriginalEnd == C.OriginalStart)) {
                continue;
            }
        }
        if (C.CreateReplacement) {
            std::string ReplacementText = C.PreviousLinePostfix;
            if (C.ContinuesPPDirective)
                appendEscapedNewlineText(ReplacementText, C.NewlinesBefore, C.PreviousEndOfTokenColumn,
                                         C.EscapedNewlineColumn);
            else
                appendNewlineText(ReplacementText, C);
            appendIndentText(ReplacementText, C.Tok->IndentLevel, static_cast<unsigned>(std::max(0, C.Spaces)),
                             static_cast<unsigned>(std::max(static_cast<int>(C.StartOfTokenColumn), C.Spaces) -
                                                   std::max(0, C.Spaces)),
                             C.IsAligned);
            ReplacementText.append(C.CurrentLinePrefix);
            std::string_view Original = Src_.Text.substr(C.OriginalStart, C.OriginalEnd - C.OriginalStart);
            if (Original == ReplacementText) continue;
            Out.push_back(Replacement{C.OriginalStart, C.OriginalEnd - C.OriginalStart, std::move(ReplacementText)});
        }
    }
}

void WhitespaceManager::appendNewlineText(std::string &Text, const Change &C) const {
    if (C.NewlinesBefore == 0) return;
    const char *Newline = UseCRLF_ ? "\r\n" : "\n";
    for (unsigned I = 0; I < C.NewlinesBefore; ++I) Text.append(Newline);
}

void WhitespaceManager::appendEscapedNewlineText(std::string &Text, unsigned Newlines,
                                                 unsigned PreviousEndOfTokenColumn,
                                                 unsigned EscapedNewlineColumn) const {
    if (Newlines > 0) {
        int SpacesI = std::max<int>(1, static_cast<int>(EscapedNewlineColumn) - static_cast<int>(PreviousEndOfTokenColumn) - 1);
        for (unsigned i = 0; i < Newlines; ++i) {
            Text.append(static_cast<size_t>(SpacesI), ' ');
            Text.append(UseCRLF_ ? "\\\r\n" : "\\\n");
            SpacesI = std::max<int>(0, static_cast<int>(EscapedNewlineColumn) - 1);
        }
    }
}

void WhitespaceManager::appendIndentText(std::string &Text, unsigned IndentLevel, unsigned Spaces,
                                         unsigned WhitespaceStartColumn, bool IsAligned) const {
    switch (Style_.UseTab) {
    case Style::UT_Never:
        Text.append(Spaces, ' ');
        break;
    case Style::UT_Always: {
        if (Style_.TabWidth) {
            unsigned FirstTabWidth = Style_.TabWidth - WhitespaceStartColumn % Style_.TabWidth;
            if (Spaces < FirstTabWidth || Spaces == 1) {
                Text.append(Spaces, ' ');
                break;
            }
            Spaces -= FirstTabWidth;
            Text.append("\t");
            Text.append(Spaces / Style_.TabWidth, '\t');
            Text.append(Spaces % Style_.TabWidth, ' ');
        } else if (Spaces == 1) {
            Text.append(Spaces, ' ');
        }
        break;
    }
    case Style::UT_ForIndentation:
        if (WhitespaceStartColumn == 0) {
            unsigned Indentation = IndentLevel * Style_.IndentWidth;
            Spaces = appendTabIndent(Text, Spaces, Indentation);
        }
        Text.append(Spaces, ' ');
        break;
    case Style::UT_ForContinuationAndIndentation:
        if (WhitespaceStartColumn == 0) Spaces = appendTabIndent(Text, Spaces, Spaces);
        Text.append(Spaces, ' ');
        break;
    case Style::UT_AlignWithSpaces:
        if (WhitespaceStartColumn == 0) {
            unsigned Indentation = IsAligned ? IndentLevel * Style_.IndentWidth : Spaces;
            Spaces = appendTabIndent(Text, Spaces, Indentation);
        }
        Text.append(Spaces, ' ');
        break;
    }
}

unsigned WhitespaceManager::appendTabIndent(std::string &Text, unsigned Spaces, unsigned Indentation) const {
    if (Indentation > Spaces) Indentation = Spaces;
    if (Style_.TabWidth) {
        unsigned Tabs = Indentation / Style_.TabWidth;
        Text.append(Tabs, '\t');
        Spaces -= Tabs * Style_.TabWidth;
    }
    return Spaces;
}

}  // namespace cfmt::detail
