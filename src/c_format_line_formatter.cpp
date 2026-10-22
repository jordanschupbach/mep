// Formatting unwrapped lines for mep's C formatter: the port of
// clang-format's UnwrappedLineFormatter. It joins lines that LLVM style
// keeps on one line (a short function body, an empty block, a #define's
// body), computes each line's indentation and blank lines, and lays out a
// line that does not fit by a shortest-path search over break decisions,
// scored with the penalties the annotator and indenter assign.

#include "c_format_internal.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <deque>
#include <map>
#include <queue>
#include <set>

namespace cfmt::detail {

namespace {

bool startsExternCBlock(const AnnotatedLine &Line) {
    const FormatToken *Next = Line.First->getNextNonComment();
    const FormatToken *NextNext = Next ? Next->getNextNonComment() : nullptr;
    return Line.startsWith(tok::kw_extern) && Next && Next->isStringLiteral() && NextNext &&
           NextNext->is(tok::l_brace);
}

bool isRecordLBrace(const FormatToken &Tok) {
    return Tok.isOneOf(TT::EnumLBrace, TT::RecordLBrace, TT::StructLBrace, TT::UnionLBrace);
}

class LevelIndentTracker {
public:
    LevelIndentTracker(const Style &S, unsigned StartLevel, int AdditionalIndent)
        : S(S) {
        for (unsigned i = 0; i != StartLevel; ++i)
            IndentForLevel.push_back(static_cast<int>(S.IndentWidth * i) + AdditionalIndent);
    }
    unsigned getIndent() const { return Indent; }
    void nextLine(const AnnotatedLine &Line) {
        Offset = 0;
        if (Line.Level >= IndentForLevel.size()) IndentForLevel.resize(Line.Level + 1, -1);
        if (!Line.InPPDirective) IndentForLevel.resize(Line.Level + 1);
        Indent = getIndent(Line.Level);
        if (static_cast<int>(Indent) + Offset >= 0) Indent = static_cast<unsigned>(static_cast<int>(Indent) + Offset);
        if (Line.IsContinuation) Indent = Line.Level * S.IndentWidth + S.ContinuationIndentWidth;
    }
    void adjustToUnmodifiedLine(const AnnotatedLine &Line) {
        if (Line.InPPDirective || Line.IsContinuation) return;
        if (Line.First->is(tok::comment) && IndentForLevel[Line.Level] != -1) return;
        unsigned LevelIndent = Line.First->OriginalColumn;
        if (static_cast<int>(LevelIndent) - Offset >= 0)
            LevelIndent = static_cast<unsigned>(static_cast<int>(LevelIndent) - Offset);
        IndentForLevel[Line.Level] = static_cast<int>(LevelIndent);
    }

private:
    unsigned getIndent(unsigned Level) const {
        if (IndentForLevel[Level] != -1) return static_cast<unsigned>(IndentForLevel[Level]);
        if (Level == 0) return 0;
        return getIndent(Level - 1) + S.IndentWidth;
    }
    const Style &S;
    std::vector<int> IndentForLevel;
    int Offset = 0;
    unsigned Indent = 0;
};

class LineJoiner {
public:
    LineJoiner(const Style &S, const std::vector<AnnotatedLine *> &Lines)
        : S(S), Lines(Lines), Next(0), End(Lines.size()) {}

    const AnnotatedLine *getNextMergedLine(bool DryRun, LevelIndentTracker &IndentTracker) {
        if (Next == End) return nullptr;
        const AnnotatedLine *Current = Lines[Next];
        IndentTracker.nextLine(*Current);
        unsigned MergedLines = tryFitMultipleLinesInOne(IndentTracker, Next);
        if (!DryRun)
            for (unsigned i = 0; i < MergedLines; ++i) join(*Lines[Next], *Lines[Next + i + 1]);
        Next = Next + MergedLines + 1;
        return Current;
    }

private:
    AnnotatedLine *at(size_t I) const { return Lines[I]; }

    unsigned tryFitMultipleLinesInOne(LevelIndentTracker &IndentTracker, size_t I) {
        if (I + 1 == End) return 0;
        const AnnotatedLine *TheLine = at(I);
        if (TheLine->Last->is(TT::LineComment)) return 0;
        const AnnotatedLine &NextLine = *at(I + 1);
        if (NextLine.Type == LT_Invalid || NextLine.First->MustBreakBefore) return 0;
        if (TheLine->InPPDirective && (!NextLine.InPPDirective || NextLine.First->HasUnescapedNewline)) return 0;
        const unsigned Indent = IndentTracker.getIndent();
        if (S.ColumnLimit > 0 && Indent > S.ColumnLimit) return 0;
        unsigned Limit = S.ColumnLimit - Indent;
        Limit = TheLine->Last->TotalLength > Limit ? 0 : Limit - TheLine->Last->TotalLength;
        const AnnotatedLine *PreviousLine = I != 0 ? at(I - 1) : nullptr;
        if (PreviousLine && TheLine->Last->is(tok::l_brace) && TheLine->First == TheLine->Last) {
            bool EmptyBlock = NextLine.First->is(tok::r_brace);
            const FormatToken *Tok = PreviousLine->First;
            if (Tok && Tok->is(tok::comment)) Tok = Tok->getNextNonComment();
            if (Tok && Tok->is(tok::kw_typedef)) Tok = Tok->getNextNonComment();
            if (Tok && Tok->isOneOf(tok::kw_struct, tok::kw_union, tok::kw_extern))
                return !S.BraceWrappingSplitEmptyRecord && EmptyBlock ? tryMergeSimpleBlock(I, Limit) : 0;
        }
        const bool MergeShortFunctions = S.AllowShortFunctionsOnASingleLine == Style::SFS_All ||
                                         (S.AllowShortFunctionsOnASingleLine >= Style::SFS_Empty &&
                                          NextLine.First->is(tok::r_brace));
        const FormatToken *FirstNonComment = TheLine->getFirstNonComment();
        if (!FirstNonComment) return 0;
        const FormatToken *LastNonComment = TheLine->getLastNonComment();
        if (LastNonComment->is(TT::FunctionLBrace) && TheLine->First != LastNonComment)
            return MergeShortFunctions ? tryMergeSimpleBlock(I, Limit) : 0;
        if (TheLine->Last->is(tok::l_brace) && FirstNonComment != TheLine->Last &&
            FirstNonComment->isOneOf(tok::kw_if, tok::kw_while, tok::kw_for, TT::ForEachMacro)) {
            return S.AllowShortBlocksOnASingleLine != Style::SBS_Never ? tryMergeSimpleBlock(I, Limit) : 0;
        }
        if (NextLine.First->is(TT::ControlStatementLBrace)) return 0;  // BWACS_Never
        if (PreviousLine && TheLine->First->is(tok::l_brace)) {
            if (PreviousLine->First->isOneOf(tok::kw_case, tok::kw_default)) return 0;
        }
        if (PreviousLine && S.BraceWrappingSplitEmptyRecord && TheLine->Last->is(tok::l_brace) && PreviousLine->Last) {
            const FormatToken *Previous = PreviousLine->Last;
            if (Previous) {
                if (Previous->is(tok::comment)) Previous = Previous->getPreviousNonComment();
                if (Previous) {
                    if (Previous->is(tok::greater) && !PreviousLine->InPPDirective) return 0;
                    if (Previous->is(tok::identifier)) {
                        const FormatToken *PreviousPrevious = Previous->getPreviousNonComment();
                        if (PreviousPrevious && PreviousPrevious->is(tok::kw_struct)) return 0;
                    }
                }
            }
        }
        if (TheLine->Last->is(tok::l_brace)) {
            bool ShouldMerge = false;
            if (TheLine->Last->is(TT::EnumLBrace)) {
                ShouldMerge = S.AllowShortEnumsOnASingleLine;
            } else if (TheLine->Last->is(TT::StructLBrace)) {
                ShouldMerge = true;  // !AfterClass
            } else if (TheLine->InPPDirective || !TheLine->First->isOneOf(tok::kw_enum, tok::kw_struct)) {
                ShouldMerge = !S.BraceWrappingAfterFunction ||
                              (NextLine.First->is(tok::r_brace) && !S.BraceWrappingSplitEmptyFunction);
            }
            return ShouldMerge ? tryMergeSimpleBlock(I, Limit) : 0;
        }
        if (NextLine.First->is(TT::FunctionLBrace) && S.BraceWrappingAfterFunction) {
            if (NextLine.Last->is(TT::LineComment)) return 0;
            if (Limit <= 2) return 0;
            Limit -= 2;
            unsigned MergedLines = 0;
            if (MergeShortFunctions ||
                (S.AllowShortFunctionsOnASingleLine >= Style::SFS_Empty && NextLine.First == NextLine.Last &&
                 I + 2 != End && at(I + 2)->First->is(tok::r_brace))) {
                MergedLines = tryMergeSimpleBlock(I + 1, Limit);
                if (MergedLines > 0) ++MergedLines;
            }
            return MergedLines;
        }
        if (TheLine->First->is(tok::kw_if))
            return S.AllowShortIfStatementsOnASingleLine ? tryMergeSimpleControlStatement(I, Limit) : 0;
        if (TheLine->First->isOneOf(tok::kw_for, tok::kw_while, tok::kw_do, TT::ForEachMacro))
            return S.AllowShortLoopsOnASingleLine ? tryMergeSimpleControlStatement(I, Limit) : 0;
        if (TheLine->First->isOneOf(tok::kw_case, tok::kw_default))
            return S.AllowShortCaseLabelsOnASingleLine ? tryMergeShortCaseLabels(I, Limit) : 0;
        if (TheLine->InPPDirective && (TheLine->First->HasUnescapedNewline || TheLine->First->IsFirst))
            return tryMergeSimplePPDirective(I, Limit);
        return 0;
    }

    unsigned tryMergeSimplePPDirective(size_t I, unsigned Limit) {
        if (Limit == 0) return 0;
        if (I + 2 != End && at(I + 2)->InPPDirective && !at(I + 2)->First->HasUnescapedNewline) return 0;
        if (1 + at(I + 1)->Last->TotalLength > Limit) return 0;
        return 1;
    }

    unsigned tryMergeSimpleControlStatement(size_t I, unsigned Limit) {
        if (Limit == 0) return 0;
        if (at(I + 1)->InPPDirective != at(I)->InPPDirective ||
            (at(I + 1)->InPPDirective && at(I + 1)->First->HasUnescapedNewline)) {
            return 0;
        }
        Limit = limitConsideringMacros(I + 1, Limit);
        const AnnotatedLine &Line = *at(I);
        if (Line.First->isNot(tok::kw_do) && Line.First->isNot(tok::kw_else) && Line.Last->isNot(tok::kw_else) &&
            Line.Last->isNot(tok::r_paren)) {
            return 0;
        }
        if (Line.First->is(tok::kw_do) && Line.Last->isNot(tok::kw_do)) return 0;
        if (1 + at(I + 1)->Last->TotalLength > Limit) return 0;
        if (at(I + 1)->First->isOneOf(tok::semi, tok::kw_if, tok::kw_for, tok::kw_while, TT::ForEachMacro,
                                      TT::LineComment)) {
            return 0;
        }
        return 1;
    }

    unsigned tryMergeShortCaseLabels(size_t I, unsigned Limit) {
        if (Limit == 0 || I + 1 == End || at(I + 1)->First->isOneOf(tok::kw_case, tok::kw_default)) return 0;
        if (at(I)->Last->is(tok::l_brace) || at(I + 1)->First->is(tok::l_brace)) return 0;
        unsigned NumStmts = 0;
        unsigned Length = 0;
        bool EndsWithComment = false;
        bool InPPDirective = at(I)->InPPDirective;
        bool InMacroBody = at(I)->InMacroBody;
        const unsigned Level = at(I)->Level;
        for (; NumStmts < 3; ++NumStmts) {
            if (I + 1 + NumStmts == End) break;
            const AnnotatedLine *Line = at(I + 1 + NumStmts);
            if (Line->InPPDirective != InPPDirective) break;
            if (Line->InMacroBody != InMacroBody) break;
            if (Line->First->isOneOf(tok::kw_case, tok::kw_default, tok::r_brace)) break;
            if (Line->First->isOneOf(tok::kw_if, tok::kw_for, tok::kw_switch, tok::kw_while) || EndsWithComment)
                return 0;
            if (Line->First->is(tok::comment)) {
                if (Level != Line->Level) return 0;
                for (size_t J = I + 2 + NumStmts; J != End; ++J) {
                    Line = at(J);
                    if (Line->InPPDirective != InPPDirective) break;
                    if (Line->First->isOneOf(tok::kw_case, tok::kw_default, tok::r_brace)) break;
                    if (Line->First->isNot(tok::comment) || Level != Line->Level) return 0;
                }
                break;
            }
            if (Line->Last->is(tok::comment)) EndsWithComment = true;
            Length += at(I + 1 + NumStmts)->Last->TotalLength + 1;
        }
        if (NumStmts == 0 || NumStmts == 3 || Length > Limit) return 0;
        return NumStmts;
    }

    unsigned tryMergeSimpleBlock(size_t I, unsigned Limit) {
        if (at(I + 1)->Type == LT_PreprocessorDirective) return 0;
        AnnotatedLine &Line = *at(I);
        if (Line.First->isOneOf(tok::at, tok::minus, tok::plus)) return 0;
        if (Line.First->is(tok::kw_case) || (Line.First->Next && Line.First->Next->is(tok::kw_else))) return 0;
        if (Line.First->is(tok::kw_default)) {
            const FormatToken *Tok = Line.First->getNextNonComment();
            if (Tok && Tok->is(tok::colon)) return 0;
        }
        auto IsCtrlStmt = [](const AnnotatedLine &L) {
            return L.First->isOneOf(tok::kw_if, tok::kw_else, tok::kw_while, tok::kw_do, tok::kw_for, TT::ForEachMacro);
        };
        const bool IsSplitBlock = S.AllowShortBlocksOnASingleLine == Style::SBS_Never ||
                                  (S.AllowShortBlocksOnASingleLine == Style::SBS_Empty &&
                                   at(I + 1)->First->isNot(tok::r_brace));
        if (IsCtrlStmt(Line) || Line.First->isOneOf(tok::kw___try, tok::kw___finally, tok::r_brace) ||
            (Line.First->is(tok::identifier) && Line.First->TokenText == "__except")) {
            if (IsSplitBlock) return 0;
            if (Line.First->isOneOf(tok::kw___try, tok::kw___finally)) return 0;
        }
        if (Line.endsWith(tok::l_brace)) {
            if (S.AllowShortBlocksOnASingleLine == Style::SBS_Never && Line.First->is(TT::BlockLBrace)) return 0;
            if (IsSplitBlock && Line.First == Line.Last && I > 0 &&
                (at(I - 1)->endsWith(tok::kw_else) || IsCtrlStmt(*at(I - 1)))) {
                return 0;
            }
            FormatToken *Tok = at(I + 1)->First;
            auto ShouldMerge = [Tok]() {
                if (Tok->isNot(tok::r_brace) || Tok->MustBreakBefore) return false;
                const FormatToken *Next = Tok->getNextNonComment();
                return !Next || Next->is(tok::semi);
            };
            if (ShouldMerge()) {
                Tok->SpacesRequiredBefore = Line.Last->is(tok::comment) ? 1 : 0;  // SpaceInEmptyBlock: false
                Tok->CanBreakBefore = true;
                return 1;
            } else if (Limit != 0 && !startsExternCBlock(Line)) {
                if (isRecordLBrace(*Line.Last)) return 0;
                if (I + 2 == End || at(I + 2)->Type == LT_Invalid) return 0;
                Limit = limitConsideringMacros(I + 2, Limit);
                if (!nextTwoLinesFitInto(I, Limit)) return 0;
                if (at(I + 1)->Last->is(TT::LineComment)) return 0;
                do {
                    if (Tok->is(tok::l_brace) && Tok->isNot(BK_BracedInit)) return 0;
                    Tok = Tok->Next;
                } while (Tok);
                Tok = at(I + 2)->First;
                if (Tok->isNot(tok::r_brace)) return 0;
                if (Tok->Next && Tok->Next->is(tok::kw_else)) return 0;
                return 2;
            }
        } else if (at(I + 1)->First->is(tok::l_brace)) {
            if (at(I + 1)->Last->is(TT::LineComment)) return 0;
            if (Limit <= 2) return 0;
            Limit -= 2;
            unsigned MergedLines = 0;
            if (S.AllowShortBlocksOnASingleLine != Style::SBS_Never ||
                (at(I + 1)->First == at(I + 1)->Last && I + 2 != End && at(I + 2)->First->is(tok::r_brace))) {
                MergedLines = tryMergeSimpleBlock(I + 1, Limit);
                if (MergedLines > 0) ++MergedLines;
            }
            return MergedLines;
        }
        return 0;
    }

    unsigned limitConsideringMacros(size_t I, unsigned Limit) {
        if (at(I)->InPPDirective && I + 1 != End && !at(I + 1)->First->HasUnescapedNewline &&
            at(I + 1)->First->isNot(tok::eof)) {
            return Limit < 2 ? 0 : Limit - 2;
        }
        return Limit;
    }

    bool nextTwoLinesFitInto(size_t I, unsigned Limit) {
        if (at(I + 1)->First->MustBreakBefore || at(I + 2)->First->MustBreakBefore) return false;
        return 1 + at(I + 1)->Last->TotalLength + 1 + at(I + 2)->Last->TotalLength <= Limit;
    }

    void join(AnnotatedLine &A, const AnnotatedLine &B) {
        A.Last->Next = B.First;
        B.First->Previous = A.Last;
        B.First->CanBreakBefore = true;
        unsigned LengthA = A.Last->TotalLength + B.First->SpacesRequiredBefore;
        for (FormatToken *Tok = B.First; Tok; Tok = Tok->Next) {
            Tok->TotalLength += LengthA;
            A.Last = Tok;
        }
    }

    const Style &S;
    const std::vector<AnnotatedLine *> &Lines;
    size_t Next;
    size_t End;
};

void markFinalized(FormatToken *Tok) {
    if (Tok->is(tok::hash) && !Tok->Previous && Tok->Next &&
        Tok->Next->isOneOf(pp::if_, pp::ifdef, pp::ifndef, pp::elif, pp::elifdef, pp::elifndef, pp::else_,
                           pp::endif)) {
        Tok = Tok->Next;
    }
    for (; Tok; Tok = Tok->Next) Tok->Finalized = true;
}

class UnwrappedLineFormatter {
public:
    UnwrappedLineFormatter(ContinuationIndenter &Indenter, WhitespaceManager &Whitespaces, const Style &S)
        : Indenter(Indenter), Whitespaces(Whitespaces), S(S) {}

    unsigned format(const std::vector<AnnotatedLine *> &Lines, bool DryRun = false, int AdditionalIndent = 0,
                    bool FixBadIndentation = false);

private:
    void formatFirstToken(const AnnotatedLine &Line, const AnnotatedLine *PreviousLine,
                          const AnnotatedLine *PrevPrevLine, const std::vector<AnnotatedLine *> &Lines,
                          unsigned Indent, unsigned NewlineIndent);
    unsigned getColumnLimit(bool InPPDirective, const AnnotatedLine *NextLine) const;

    ContinuationIndenter &Indenter;
    WhitespaceManager &Whitespaces;
    const Style &S;
    std::map<std::pair<const std::vector<AnnotatedLine *> *, int>, unsigned> PenaltyCache;
};

class LineFormatter {
public:
    LineFormatter(ContinuationIndenter &Indenter, WhitespaceManager &Whitespaces, const Style &S,
                  UnwrappedLineFormatter &BlockFormatter)
        : Indenter(Indenter), Whitespaces(Whitespaces), S(S), BlockFormatter(BlockFormatter) {}
    virtual ~LineFormatter() = default;
    virtual unsigned formatLine(const AnnotatedLine &Line, unsigned FirstIndent, unsigned FirstStartColumn,
                                bool DryRun) = 0;

protected:
    bool formatChildren(LineState &State, bool NewLine, bool DryRun, unsigned &Penalty) {
        const FormatToken *LBrace = State.NextToken->getPreviousNonComment();
        bool HasLBrace = LBrace && LBrace->is(tok::l_brace) && LBrace->is(BK_Block);
        FormatToken &Previous = *State.NextToken->Previous;
        if (Previous.Children.empty() || !HasLBrace) return true;
        if (NewLine) {
            const ParenState &P = State.Stack.back();
            int AdditionalIndent =
                static_cast<int>(P.Indent) - static_cast<int>(Previous.Children[0]->Level * S.IndentWidth);
            Penalty += BlockFormatter.format(Previous.Children, DryRun, AdditionalIndent, /*FixBadIndentation=*/true);
            return true;
        }
        if (Previous.Children[0]->First->MustBreakBefore) return false;
        if (Previous.is(tok::comment)) return false;
        if (Previous.Children.size() > 1) return false;
        const AnnotatedLine *Child = Previous.Children[0];
        if (Child->Last->isTrailingComment()) return false;
        if (S.ColumnLimit > 0 && Child->Last->TotalLength + State.Column + 2 > S.ColumnLimit) return false;
        if (!DryRun) {
            Whitespaces.replaceWhitespace(*Child->First, /*Newlines=*/0, /*Spaces=*/1, State.Column,
                                          /*IsAligned=*/false, State.Line->InPPDirective);
        }
        Penalty += formatLine(*Child, State.Column + 1, /*FirstStartColumn=*/0, DryRun);
        if (!DryRun) markFinalized(Child->First);
        State.Column += 1 + Child->Last->TotalLength;
        return true;
    }

    ContinuationIndenter &Indenter;
    WhitespaceManager &Whitespaces;
    const Style &S;
    UnwrappedLineFormatter &BlockFormatter;
};

class NoLineBreakFormatter : public LineFormatter {
public:
    using LineFormatter::LineFormatter;
    unsigned formatLine(const AnnotatedLine &Line, unsigned FirstIndent, unsigned FirstStartColumn,
                        bool DryRun) override {
        unsigned Penalty = 0;
        LineState State = Indenter.getInitialState(FirstIndent, FirstStartColumn, &Line, DryRun);
        while (State.NextToken) {
            formatChildren(State, /*NewLine=*/false, DryRun, Penalty);
            Indenter.addTokenToState(State, /*Newline=*/State.NextToken->MustBreakBefore, DryRun);
        }
        return Penalty;
    }
};

class OptimizingLineFormatter : public LineFormatter {
public:
    using LineFormatter::LineFormatter;
    unsigned formatLine(const AnnotatedLine &Line, unsigned FirstIndent, unsigned FirstStartColumn,
                        bool DryRun) override {
        LineState State = Indenter.getInitialState(FirstIndent, FirstStartColumn, &Line, DryRun);
        if (State.Line->Type == LT_ObjCMethodDecl) State.Stack.back().BreakBeforeParameter = true;
        return analyzeSolutionSpace(State, DryRun);
    }

private:
    struct StateNode {
        StateNode(const LineState &State, bool NewLine, StateNode *Previous)
            : State(State), NewLine(NewLine), Previous(Previous) {}
        LineState State;
        bool NewLine;
        StateNode *Previous;
    };
    using OrderedPenalty = std::pair<unsigned, unsigned>;
    using QueueItem = std::pair<OrderedPenalty, StateNode *>;
    struct QueueGreater {
        bool operator()(const QueueItem &A, const QueueItem &B) const { return A.first > B.first; }
    };
    using QueueType = std::priority_queue<QueueItem, std::vector<QueueItem>, QueueGreater>;
    struct CompareLineStatePointers {
        bool operator()(LineState *obj1, LineState *obj2) const { return *obj1 < *obj2; }
    };

    unsigned analyzeSolutionSpace(LineState &InitialState, bool DryRun) {
        std::set<LineState *, CompareLineStatePointers> Seen;
        unsigned Count = 0;
        QueueType Queue;
        Nodes.emplace_back(InitialState, false, nullptr);
        Queue.push(QueueItem(OrderedPenalty(0, Count), &Nodes.back()));
        ++Count;
        unsigned Penalty = 0;
        while (!Queue.empty()) {
            if (Count > 25000000) return 0;
            Penalty = Queue.top().first.first;
            StateNode *Node = Queue.top().second;
            if (!Node->State.NextToken) break;
            Queue.pop();
            if (Count > 50000) Node->State.IgnoreStackForComparison = true;
            if (!Seen.insert(&Node->State).second) continue;
            FormatDecision LastFormat = Node->State.NextToken->Decision;
            if (LastFormat == FD_Unformatted || LastFormat == FD_Continue)
                addNextStateToQueue(Penalty, Node, /*NewLine=*/false, &Count, &Queue);
            if (LastFormat == FD_Unformatted || LastFormat == FD_Break)
                addNextStateToQueue(Penalty, Node, /*NewLine=*/true, &Count, &Queue);
        }
        if (Queue.empty()) return 0;
        if (!DryRun) reconstructPath(InitialState, Queue.top().second);
        return Penalty;
    }

    void addNextStateToQueue(unsigned Penalty, StateNode *PreviousNode, bool NewLine, unsigned *Count,
                             QueueType *Queue) {
        if (NewLine && !Indenter.canBreak(PreviousNode->State)) return;
        if (!NewLine && Indenter.mustBreak(PreviousNode->State)) return;
        Nodes.emplace_back(PreviousNode->State, NewLine, PreviousNode);
        StateNode *Node = &Nodes.back();
        if (!formatChildren(Node->State, NewLine, /*DryRun=*/true, Penalty)) return;
        Penalty += Indenter.addTokenToState(Node->State, NewLine, true);
        Queue->push(QueueItem(OrderedPenalty(Penalty, *Count), Node));
        ++(*Count);
    }

    void reconstructPath(LineState &State, StateNode *Best) {
        std::vector<StateNode *> Path;
        while (Best->Previous) {
            Path.push_back(Best);
            Best = Best->Previous;
        }
        for (auto It = Path.rbegin(); It != Path.rend(); ++It) {
            unsigned Penalty = 0;
            formatChildren(State, (*It)->NewLine, /*DryRun=*/false, Penalty);
            Penalty += Indenter.addTokenToState(State, (*It)->NewLine, false);
        }
    }

    std::deque<StateNode> Nodes;
};

unsigned computeNewlines(const AnnotatedLine &Line, const AnnotatedLine *PreviousLine, const Style &S) {
    const FormatToken &RootToken = *Line.First;
    unsigned Newlines = std::min(RootToken.NewlinesBefore, S.MaxEmptyLinesToKeep + 1);
    if (RootToken.is(tok::r_brace) && (!RootToken.Next || (RootToken.Next->is(tok::semi) && !RootToken.Next->Next)))
        Newlines = std::min(Newlines, 1u);
    if (!PreviousLine && Line.Level > 0) Newlines = std::min(Newlines, 1u);
    if (Newlines == 0 && !RootToken.IsFirst) Newlines = 1;
    if (RootToken.IsFirst && !RootToken.HasUnescapedNewline) Newlines = 0;
    if (!S.KeepEmptyLinesAtStartOfBlock && PreviousLine && PreviousLine->Last->is(tok::l_brace) &&
        !startsExternCBlock(*PreviousLine)) {
        Newlines = 1;
    }
    return Newlines;
}

}  // namespace

void UnwrappedLineFormatter::formatFirstToken(const AnnotatedLine &Line, const AnnotatedLine *PreviousLine,
                                              const AnnotatedLine *, const std::vector<AnnotatedLine *> &,
                                              unsigned Indent, unsigned NewlineIndent) {
    FormatToken &RootToken = *Line.First;
    if (RootToken.is(tok::eof)) {
        unsigned Newlines = std::min(RootToken.NewlinesBefore, 1u);  // KeepEmptyLines.AtEndOfFile: false
        unsigned TokenIndent = Newlines ? NewlineIndent : 0;
        Whitespaces.replaceWhitespace(RootToken, Newlines, TokenIndent, TokenIndent);
        return;
    }
    if (RootToken.Newlines < 0) RootToken.Newlines = static_cast<int>(computeNewlines(Line, PreviousLine, S));
    if (RootToken.Newlines > 0) Indent = NewlineIndent;
    if (Line.Type == LT_PreprocessorDirective || Line.Type == LT_ImportStatement) Indent = 0;
    Whitespaces.replaceWhitespace(RootToken, static_cast<unsigned>(RootToken.Newlines), Indent, Indent,
                                  /*IsAligned=*/false, Line.InPPDirective && !RootToken.HasUnescapedNewline);
}

unsigned UnwrappedLineFormatter::getColumnLimit(bool InPPDirective, const AnnotatedLine *NextLine) const {
    bool ContinuesPPDirective =
        InPPDirective && (!NextLine || (NextLine->InPPDirective && !NextLine->First->HasUnescapedNewline));
    return S.ColumnLimit - (ContinuesPPDirective ? 2 : 0);
}

unsigned UnwrappedLineFormatter::format(const std::vector<AnnotatedLine *> &Lines, bool DryRun, int AdditionalIndent,
                                        bool) {
    LineJoiner Joiner(S, Lines);
    std::pair<const std::vector<AnnotatedLine *> *, int> CacheKey(&Lines, AdditionalIndent);
    auto CacheIt = PenaltyCache.find(CacheKey);
    if (DryRun && CacheIt != PenaltyCache.end()) return CacheIt->second;
    unsigned Penalty = 0;
    LevelIndentTracker IndentTracker(S, Lines[0]->Level, AdditionalIndent);
    const AnnotatedLine *PrevPrevLine = nullptr;
    const AnnotatedLine *PreviousLine = nullptr;
    const AnnotatedLine *NextLine = nullptr;
    for (const AnnotatedLine *Line = Joiner.getNextMergedLine(DryRun, IndentTracker); Line;
         PrevPrevLine = PreviousLine, PreviousLine = Line, Line = NextLine) {
        const AnnotatedLine &TheLine = *Line;
        unsigned Indent = IndentTracker.getIndent();
        if (TheLine.Type != LT_Invalid) {
            if (!DryRun) {
                bool LastLine = TheLine.First->is(tok::eof);
                formatFirstToken(TheLine, PreviousLine, PrevPrevLine, Lines, Indent, LastLine ? 0 : Indent);
            }
            NextLine = Joiner.getNextMergedLine(DryRun, IndentTracker);
            unsigned ColumnLimit = getColumnLimit(TheLine.InPPDirective, NextLine);
            bool FitsIntoOneLine =
                TheLine.Last->TotalLength + Indent <= ColumnLimit || TheLine.Type == LT_ImportStatement;
            if (FitsIntoOneLine) {
                Penalty += NoLineBreakFormatter(Indenter, Whitespaces, S, *this).formatLine(TheLine, Indent, 0, DryRun);
            } else {
                Penalty +=
                    OptimizingLineFormatter(Indenter, Whitespaces, S, *this).formatLine(TheLine, Indent, 0, DryRun);
            }
        } else {
            bool StartsNewLine = TheLine.First->NewlinesBefore > 0 || TheLine.First->IsFirst;
            if (StartsNewLine) IndentTracker.adjustToUnmodifiedLine(TheLine);
            if (!DryRun) {
                if (StartsNewLine) {
                    formatFirstToken(TheLine, PreviousLine, PrevPrevLine, Lines, TheLine.First->OriginalColumn,
                                     TheLine.First->OriginalColumn);
                } else {
                    Whitespaces.addUntouchableToken(*TheLine.First, TheLine.InPPDirective);
                }
                for (FormatToken *Tok = TheLine.First->Next; Tok; Tok = Tok->Next)
                    Whitespaces.addUntouchableToken(*Tok, TheLine.InPPDirective);
            }
            NextLine = Joiner.getNextMergedLine(DryRun, IndentTracker);
        }
        if (!DryRun) markFinalized(TheLine.First);
    }
    PenaltyCache[CacheKey] = Penalty;
    return Penalty;
}

unsigned FormatLines(const std::vector<AnnotatedLine *> &Lines, const Source &Src, const Style &S,
                     WhitespaceManager &Whitespaces) {
    ContinuationIndenter Indenter(Src, S, Whitespaces);
    UnwrappedLineFormatter Formatter(Indenter, Whitespaces, S);
    return Formatter.format(Lines);
}

}  // namespace cfmt::detail
