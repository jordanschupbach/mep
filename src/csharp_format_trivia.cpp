// mep's C# formatter: trivia. A port of Roslyn's TriviaDataFactory (whitespace,
// complex trivia, the analyzers), AbstractTriviaFormatter and
// CSharpTriviaFormatter (comments, doc comments, directives). See
// csharp_format_internal.h (Roslyn is MIT-licensed, Copyright (c) .NET
// Foundation and Contributors).

#include <algorithm>

#include "csharp_format_internal.h"

namespace csfmt {

namespace {

bool IsNullOrWhitespace(U16View text) {
    // AbstractTriviaFormatter.IsNullOrWhitespace: its test (`!IsWhitespace(c) || !IsNewLine(c)`) fails for
    // every character, so only the empty string passes. Kept as Roslyn has it.
    return text.empty();
}

int TriviaSpanStart(Trivia t) {
    return t->position + (t->structure ? t->green->GetLeadingTriviaWidth() : 0);
}

// --- LineColumn / LineColumnDelta / LineColumnRule -------------------------------------------------

struct LineColumnDelta {
    int lines = 0;
    int spaces = 0;
    bool whitespace_only = true;
    bool force_update = false;
    LineColumnDelta With(LineColumnDelta d) const {
        if (d.lines <= 0) return {lines, spaces + d.spaces, whitespace_only && d.whitespace_only, force_update || d.force_update};
        return {lines + d.lines, d.spaces, d.whitespace_only, force_update || d.force_update || spaces > 0};
    }
};

struct LineColumn {
    int line = 0;
    int column = 0;
    bool whitespace_only = true;
    LineColumn With(LineColumnDelta d) const {
        if (d.lines <= 0) return {line, column + d.spaces, whitespace_only && d.whitespace_only};
        return {line + d.lines, d.spaces, d.whitespace_only};
    }
};

struct LineColumnRule {
    enum class SpaceOp { Preserve, Force };
    enum class LineOp { Preserve, Force };
    enum class IndentOp { Absolute, Default, Given, Follow, Preserve };
    SpaceOp space_op = SpaceOp::Preserve;
    LineOp line_op = LineOp::Preserve;
    IndentOp indent_op = IndentOp::Preserve;
    int lines = 0;
    int spaces = 0;
    int indentation = 0;

    static LineColumnRule Preserve() { return {}; }
    static LineColumnRule PreserveWithGivenSpaces(int spaces) {
        return {SpaceOp::Preserve, LineOp::Preserve, IndentOp::Given, 0, spaces, 0};
    }
    static LineColumnRule PreserveLinesWithDefaultIndentation(int lines) {
        return {SpaceOp::Preserve, LineOp::Preserve, IndentOp::Default, lines, 0, -1};
    }
    static LineColumnRule PreserveLinesWithGivenIndentation(int lines) {
        return {SpaceOp::Preserve, LineOp::Preserve, IndentOp::Given, lines, 0, -1};
    }
    static LineColumnRule PreserveLinesWithAbsoluteIndentation(int lines, int indentation) {
        return {SpaceOp::Preserve, LineOp::Preserve, IndentOp::Absolute, lines, 0, indentation};
    }
    static LineColumnRule PreserveLinesWithFollowingPrecedingIndentation() {
        return {SpaceOp::Preserve, LineOp::Preserve, IndentOp::Follow, -1, 0, -1};
    }
};

// --- Analyzer --------------------------------------------------------------------------------------

struct AnalysisResult {
    int line_breaks = 0;
    int space = 0;
    int tab = 0;
    bool has_tab_after_space = false;
    bool has_unknown_whitespace = false;
    bool has_trailing_space = false;
    bool has_skipped_tokens = false;
    bool has_skipped_or_disabled_text = false;
    bool has_conflict_marker = false;
    bool has_comments = false;
    bool has_preprocessor = false;
};

void AnalyzeList(const Tree &tree, const std::vector<csharp::SyntaxTrivia> &list, AnalysisResult &r) {
    for (const csharp::SyntaxTrivia &t : list) {
        SyntaxKind k = t.Kind();
        if (k == SyntaxKind::WhitespaceTrivia) {
            int space = 0, tab = 0, unknown = 0;
            for (char16_t c : TriviaText(tree, &t)) {
                if (c == u' ') {
                    space++;
                } else if (c == u'\t') {
                    if (r.space > 0) r.has_tab_after_space = true;
                    tab++;
                } else {
                    unknown++;
                }
            }
            r.space += space;
            r.tab += tab;
            r.has_unknown_whitespace |= unknown > 0;
        } else if (k == SyntaxKind::EndOfLineTrivia) {
            if (r.space > 0 || r.tab > 0) r.has_trailing_space = true;
            r.line_breaks++;
            r.has_tab_after_space = false;
            r.space = 0;
            r.tab = 0;
        } else if (IsRegularOrDocComment(k)) {
            r.has_comments = true;
        } else if (k == SyntaxKind::SkippedTokensTrivia) {
            r.has_skipped_tokens = true;
        } else if (k == SyntaxKind::DisabledTextTrivia || k == SyntaxKind::PreprocessingMessageTrivia) {
            r.has_skipped_or_disabled_text = true;
        } else if (k == SyntaxKind::ConflictMarkerTrivia) {
            r.has_conflict_marker = true;
        } else {
            r.has_preprocessor = true;
        }
    }
}

bool ContainsOnlyWhitespace(const AnalysisResult &r) {
    return !r.has_comments && !r.has_preprocessor && !r.has_skipped_tokens && !r.has_skipped_or_disabled_text &&
           !r.has_conflict_marker;
}

bool ContainsSkippedTokensOrText(const std::vector<Trivia> &list) {
    for (Trivia t : list) {
        if (t->Kind() == SyntaxKind::SkippedTokensTrivia || t->Kind() == SyntaxKind::PreprocessingMessageTrivia) return true;
    }
    return false;
}

// --- Whitespace data -------------------------------------------------------------------------------

struct Whitespace : TriviaData {
    Whitespace(int lines, int indentation, Type t = Type::Whitespace) : TriviaData(t) {
        line_breaks = lines;
        spaces = indentation;
    }
    TriviaData *WithSpace(int space, FormattingContext &context) override;
    TriviaData *WithLine(int line, int indentation, FormattingContext &context) override;
    TriviaData *WithIndentation(int indentation, FormattingContext &context) override;
    void Format(FormattingContext &, int) override {}
};

struct ModifiedWhitespace : Whitespace {
    ModifiedWhitespace(Whitespace *orig, int lines, int indentation)
        : Whitespace(lines, indentation, Type::ModifiedWhitespace), original(orig) {}
    Whitespace *original;
    TriviaData *WithSpace(int space, FormattingContext &context) override {
        if (!original) return Whitespace::WithSpace(space, context);
        if (line_breaks == original->line_breaks && original->spaces == space) return original;
        return Whitespace::WithSpace(space, context);
    }
    TriviaData *WithLine(int line, int indentation, FormattingContext &context) override {
        if (!original) return Whitespace::WithLine(line, indentation, context);
        if (original->line_breaks == line && original->spaces == indentation) return original;
        return Whitespace::WithLine(line, indentation, context);
    }
    TriviaData *WithIndentation(int indentation, FormattingContext &context) override {
        if (!original) return Whitespace::WithIndentation(indentation, context);
        if (line_breaks == original->line_breaks && original->spaces == indentation) return original;
        return Whitespace::WithIndentation(indentation, context);
    }
    void Format(FormattingContext &context, int pair_index) override;
};

struct FormattedWhitespace : TriviaData {
    FormattedWhitespace(const LineOptions &o, int lines, int indentation) : TriviaData(Type::FormattedWhitespace) {
        line_breaks = std::max(0, lines);
        spaces = std::max(0, indentation);
        if (line_breaks > 0) {
            for (int i = 0; i < line_breaks; i++) text += o.new_line;
            text += CreateIndentationString(spaces, o.use_tabs, o.tab_size);
        } else {
            text = U16(static_cast<size_t>(spaces), u' ');
        }
    }
    U16 text;
    bool ContainsChanges() const override { return true; }
    U16 NewText() const override { return text; }
    TriviaData *WithSpace(int, FormattingContext &) override { return this; }
    TriviaData *WithLine(int, int, FormattingContext &) override { return this; }
    TriviaData *WithIndentation(int, FormattingContext &) override { return this; }
    void Format(FormattingContext &, int) override {}
};

TriviaData *Whitespace::WithSpace(int space, FormattingContext &context) {
    if (line_breaks == 0 && spaces == space) return this;
    return context.engine.New<ModifiedWhitespace>(this, 0, space);
}

TriviaData *Whitespace::WithLine(int line, int indentation, FormattingContext &context) {
    if (line_breaks == line && spaces == indentation) return this;
    return context.engine.New<ModifiedWhitespace>(this, line, indentation);
}

TriviaData *Whitespace::WithIndentation(int indentation, FormattingContext &context) {
    if (spaces == indentation) return this;
    return context.engine.New<ModifiedWhitespace>(this, line_breaks, indentation);
}

void ModifiedWhitespace::Format(FormattingContext &context, int pair_index) {
    context.Apply(pair_index, context.engine.New<FormattedWhitespace>(context.Options(), line_breaks, spaces));
}

// --- The trivia formatter (AbstractTriviaFormatter + CSharpTriviaFormatter) --------------------------

class TriviaFormatter {
public:
    TriviaFormatter(FormattingContext &context, Token token1, Token token2, U16View original_string, int line_breaks,
                    int spaces)
        : context_(context), tree_(context.engine.tree), rules_(context.engine.rules), opt_(context.Options()),
          original_(original_string), line_breaks_(line_breaks), spaces_(spaces), token1_(token1), token2_(token2) {
        initial_ = GetInitialLineColumn();
        indentation_ = line_breaks_ > 0 ? GetIndentation() : -1;
        first_line_blank_ = FirstLineBlank();
    }

    /** @brief FormatToSyntaxTrivia: the formatted trivia list (the original one when formatting failed). */
    std::vector<OutTrivia> FormatToSyntaxTrivia() {
        std::vector<OutTrivia> list;
        LineColumn lc = FormatTrivia(list);
        AddExtraLines(lc.line, list);
        if (succeeded_) return list;
        list.clear();
        for (Trivia t : TriviaListBetween(token1_, token2_)) list.push_back({t->Kind(), U16(TriviaText(tree_, t))});
        return list;
    }

    bool Succeeded() const { return succeeded_; }

private:
    int StartPosition() const { return token1_ ? SpanOf(token1_).end : context_.engine.start_position; }
    int EndPosition() const { return token2_ ? SpanStart(token2_) : context_.engine.end_position; }

    LineColumnDelta GetLineColumnDelta(int initial_column, U16View text) const {
        U16View line = GetLastLineText(text);
        if (line.size() != text.size()) {
            return {GetNumberOfLineBreaks(text), GetColumnFromLineOffset(line, static_cast<int>(line.size()), opt_.tab_size),
                    IsNullOrWhitespace(line), false};
        }
        return {0, ConvertTabToSpace(text, opt_.tab_size, initial_column, static_cast<int>(text.size())),
                IsNullOrWhitespace(line), false};
    }

    LineColumn GetInitialLineColumn() const {
        U16View text = TokenText(token1_);
        int initial = token1_ ? context_.Stream().GetCurrentColumn(token1_) : 0;
        LineColumnDelta d = GetLineColumnDelta(initial, text);
        return {0, initial + d.spaces, d.whitespace_only};
    }

    int GetIndentation() const {
        U16View last = GetLastLineText(original_);
        int initial = last.size() == original_.size() ? initial_.column : 0;
        int index = GetFirstNonWhitespaceIndexInString(last);
        if (index < 0) return spaces_;
        int position = ConvertTabToSpace(last, opt_.tab_size, initial, index);
        int token_position = ConvertTabToSpace(last, opt_.tab_size, initial, static_cast<int>(last.size()));
        return spaces_ - (token_position - position);
    }

    bool FirstLineBlank() const {
        size_t index = U16View::npos;
        for (size_t i = 0; i < original_.size(); i++) {
            if (IsCSharpNewLine(original_[i])) {
                index = i;
                break;
            }
        }
        if (index == U16View::npos) return IsNullOrWhitespace(original_);
        for (size_t i = 0; i < index; i++) {
            if (!IsCSharpWhitespace(original_[i])) return false;
        }
        return true;
    }

    int GetExistingIndentation(Trivia trivia) const {
        int offset = trivia->position - StartPosition();
        U16View text = original_.substr(0, static_cast<size_t>(std::max(0, offset)));
        return initial_.With(GetLineColumnDelta(initial_.column, text)).column;
    }

    LineColumn FormatTrivia(std::vector<OutTrivia> &changes) {
        LineColumn lc = initial_;
        LineColumnDelta existing;
        Trivia previous = nullptr;
        bool implicit_line_break = false;
        for (Trivia trivia : TriviaListBetween(token1_, token2_)) {
            SyntaxKind k = trivia->Kind();
            if (k == SyntaxKind::WhitespaceTrivia || k == SyntaxKind::EndOfLineTrivia) {
                LineColumnDelta d;
                if (k == SyntaxKind::EndOfLineTrivia) {
                    d = {1, 0, true, false};
                    implicit_line_break = false;
                } else {
                    U16View text = TriviaText(tree_, trivia);
                    d = {0, ConvertTabToSpace(text, opt_.tab_size, lc.With(existing).column, static_cast<int>(text.size())),
                         true, false};
                }
                existing = existing.With(d);
                continue;
            }
            lc = FormatFirstTriviaAndWhitespaceAfter(lc, previous, existing, trivia, changes, implicit_line_break);
            implicit_line_break = implicit_line_break || ContainsImplicitLineBreak(trivia);
            existing = LineColumnDelta();
            previous = trivia;
        }
        return FormatFirstTriviaAndWhitespaceAfter(lc, previous, existing, nullptr, changes, implicit_line_break);
    }

    static bool ContainsImplicitLineBreak(Trivia trivia) {
        if (!trivia->structure) return false;
        Token last = LastToken(trivia->structure, true);
        if (!last) return false;
        for (const csharp::SyntaxTrivia &t : last->trailing) {
            if (t.Kind() == SyntaxKind::EndOfLineTrivia) return true;
        }
        return false;
    }

    LineColumn FormatFirstTriviaAndWhitespaceAfter(LineColumn before, Trivia trivia1, LineColumnDelta existing,
                                                   Trivia trivia2, std::vector<OutTrivia> &changes,
                                                   bool implicit_line_break) {
        LineColumn after1 = trivia1 ? before.With(Format(before, trivia1, changes)) : before;
        LineColumnRule rule = GetOverallLineColumnRuleBetween(trivia1, existing, implicit_line_break, trivia2);
        LineColumnDelta delta = Apply(before, trivia1, after1, existing, trivia2, rule);
        AddWhitespaceTrivia(after1, delta, changes);
        return after1.With(delta);
    }

    LineColumnRule GetOverallLineColumnRuleBetween(Trivia trivia1, LineColumnDelta existing, bool implicit_line_break,
                                                   Trivia trivia2) {
        LineColumnRule rule = GetLineColumnRuleBetween(trivia1, existing, implicit_line_break, trivia2);
        Token t1 = nullptr;
        Token t2 = nullptr;
        if (!trivia1) {
            t1 = token1_;
        } else if (trivia1->structure) {
            Token last = LastToken(trivia1->structure, true);
            if (last && ContainsOnlyWhitespace(SpanOf(last).end, FullSpanOf(last).end)) t1 = last;
        }
        if (!trivia2) {
            t2 = token2_;
        } else if (trivia2->structure) {
            Token first = FirstToken(trivia2->structure, true);
            if (first && ContainsOnlyWhitespace(FullSpanOf(first).start, SpanStart(first))) t2 = first;
        }
        if (!t1 || !t2) return rule;

        NewLinesOp line_op = rules_.GetAdjustNewLinesOperation(t1, t2);
        if (existing.lines != 0 && !line_op.valid) return rule;
        if (line_op.valid) {
            switch (line_op.option) {
            case NewLinesOption::PreserveLines:
                if (existing.lines != 0) {
                    rule.lines = line_op.line;
                    rule.line_op = LineColumnRule::LineOp::Preserve;
                    return rule;
                }
                break;
            case NewLinesOption::ForceLines:
                rule.lines = line_op.line;
                rule.line_op = LineColumnRule::LineOp::Force;
                return rule;
            case NewLinesOption::ForceLinesIfOnSingleLine:
                if (context_.Stream().TwoTokensOnSameLine(t1, t2)) {
                    rule.lines = line_op.line;
                    rule.line_op = LineColumnRule::LineOp::Force;
                    return rule;
                }
                break;
            }
        }
        SpacesOp space_op = rules_.GetAdjustSpacesOperation(t1, t2);
        if (!space_op.valid) return rule;
        if (space_op.option == SpacesOption::DefaultSpacesIfOnSingleLine && space_op.space == 1) return rule;
        rule.spaces = space_op.space;
        return rule;
    }

    bool ContainsOnlyWhitespace(int start, int end) const {
        int t1_end = token1_ ? SpanOf(token1_).end : 0;
        int length = end - start;
        for (int i = start - t1_end; i < length; i++) {
            if (i < 0 || i >= static_cast<int>(original_.size())) break;
            if (!IsDotNetWhiteSpace(original_[static_cast<size_t>(i)])) return false;
        }
        return true;
    }

    bool IsStartOrEndOfFile(Trivia trivia1, Trivia trivia2) const {
        bool start_or_end = (!token1_ || !token2_) && (!trivia1 || !trivia2);
        bool at_eof = Kind(token2_) == SyntaxKind::EndOfFileToken && !trivia2;
        return start_or_end || at_eof;
    }

    LineColumnRule GetLineColumnRuleBetween(Trivia trivia1, LineColumnDelta existing, bool implicit_line_break,
                                            Trivia trivia2) {
        if (IsStartOrEndOfFile(trivia1, trivia2)) return LineColumnRule::PreserveLinesWithAbsoluteIndentation(0, 0);

        SyntaxKind k1 = trivia1 ? trivia1->Kind() : SyntaxKind::None;
        if (!trivia2) {
            if (k1 == SyntaxKind::MultiLineCommentTrivia || k1 == SyntaxKind::MultiLineDocumentationCommentTrivia) {
                bool insert = rules_.GetAdjustNewLinesOperation(token1_, token2_).valid;
                return LineColumnRule::PreserveLinesWithGivenIndentation(insert ? 1 : 0);
            }
            if (existing.spaces != spaces_) return LineColumnRule::PreserveWithGivenSpaces(spaces_);
            return LineColumnRule::Preserve();
        }

        SyntaxKind k2 = trivia2->Kind();
        if (csharp::SyntaxFacts::IsPreprocessorDirective(k2)) {
            if (k2 == SyntaxKind::BadDirectiveTrivia && existing.lines == 0 && !implicit_line_break) {
                succeeded_ = false;
                return LineColumnRule::Preserve();
            }
            int lines = (!trivia1 && !token1_) ? 0 : 1;
            if (k2 == SyntaxKind::RegionDirectiveTrivia || k2 == SyntaxKind::EndRegionDirectiveTrivia) {
                Trivia previous = trivia2;
                while (true) {
                    int start = previous->position;
                    if (start == 0) break;
                    previous = tree_.FindTrivia(start - 1);
                    if (!previous) break;
                    if (previous->Kind() == SyntaxKind::DisabledTextTrivia) return LineColumnRule::Preserve();
                    if (previous->Kind() == SyntaxKind::EndIfDirectiveTrivia) break;
                    if (previous->structure && Is(previous->structure, kBranchingDirective)) {
                        if (!csharp::BranchTaken(previous->structure->green)) return LineColumnRule::Preserve();
                        break;
                    }
                }
                if (k2 == SyntaxKind::EndRegionDirectiveTrivia && trivia2->structure) {
                    const Node *region = tree_.MatchingRegion(trivia2->structure);
                    if (region) {
                        int ind = context_.GetBaseIndentation(region->Span().start);
                        return LineColumnRule::PreserveLinesWithAbsoluteIndentation(lines, ind);
                    }
                }
                return LineColumnRule::PreserveLinesWithDefaultIndentation(lines);
            }
            return LineColumnRule::PreserveLinesWithAbsoluteIndentation(lines, 0);
        }

        if (IsRegularOrDocComment(k2)) {
            if (k1 != k2 || k2 == SyntaxKind::MultiLineCommentTrivia || k2 == SyntaxKind::MultiLineDocumentationCommentTrivia ||
                existing.lines > 1) {
                if (rules_.GetAdjustNewLinesOperation(token1_, token2_).valid) {
                    return LineColumnRule::PreserveLinesWithDefaultIndentation(0);
                }
                return LineColumnRule::PreserveLinesWithGivenIndentation(0);
            }
            if (existing.lines == 0) return LineColumnRule::PreserveLinesWithGivenIndentation(0);
            return LineColumnRule::PreserveLinesWithFollowingPrecedingIndentation();
        }

        if (k2 == SyntaxKind::SkippedTokensTrivia) succeeded_ = false;
        return LineColumnRule::Preserve();
    }

    LineColumnDelta Apply(LineColumn before, Trivia trivia1, LineColumn after1, LineColumnDelta existing, Trivia trivia2,
                          const LineColumnRule &rule) {
        if ((IsMissing(token1_) && !trivia1) || (!trivia2 && IsMissing(token2_))) return existing;
        int adjusted = std::max(0, rule.lines - ((after1.column == 0 && after1.line > 0) ? 1 : 0));
        int lines = rule.line_op == LineColumnRule::LineOp::Preserve ? std::max(adjusted, existing.lines) : adjusted;

        int spaces = 0;
        LineColumn after_existing = after1.With(existing);
        if (rule.lines > 0 || after_existing.whitespace_only) {
            switch (rule.indent_op) {
            case LineColumnRule::IndentOp::Absolute: spaces = std::max(0, rule.indentation); break;
            case LineColumnRule::IndentOp::Default:
                spaces = context_.GetBaseIndentation(trivia2 ? TriviaSpanStart(trivia2) : EndPosition());
                break;
            case LineColumnRule::IndentOp::Given: spaces = !trivia2 ? spaces_ : std::max(0, indentation_); break;
            case LineColumnRule::IndentOp::Follow: spaces = std::max(0, before.column); break;
            case LineColumnRule::IndentOp::Preserve: spaces = existing.spaces; break;
            }
        } else {
            spaces = rule.space_op == LineColumnRule::SpaceOp::Preserve ? std::max(rule.spaces, existing.spaces)
                                                                         : std::max(rule.spaces, 0);
        }
        return {lines, spaces, true, existing.force_update};
    }

    void AddWhitespaceTrivia(LineColumn lc, LineColumnDelta delta, std::vector<OutTrivia> &changes) const {
        if (delta.lines == 0 && delta.spaces == 0) return;
        for (int i = 0; i < delta.lines; i++) changes.push_back({SyntaxKind::EndOfLineTrivia, opt_.new_line});
        if (delta.spaces == 0) return;
        if (delta.lines > 0 || lc.column == 0) {
            changes.push_back({SyntaxKind::WhitespaceTrivia, CreateIndentationString(delta.spaces, opt_.use_tabs, opt_.tab_size)});
            return;
        }
        changes.push_back({SyntaxKind::WhitespaceTrivia, U16(static_cast<size_t>(std::max(0, delta.spaces)), u' ')});
    }

    void AddExtraLines(int lines_between_tokens, std::vector<OutTrivia> &changes) const {
        if (lines_between_tokens >= line_breaks_) return;
        std::vector<OutTrivia> breaks;
        AddWhitespaceTrivia(LineColumn{}, LineColumnDelta{line_breaks_ - lines_between_tokens, 0, true, false}, breaks);
        size_t at = 0;
        if (!first_line_blank_ && !changes.empty()) {
            bool found = false;
            for (size_t i = changes.size(); i-- > 0;) {
                if (changes[i].kind == SyntaxKind::EndOfLineTrivia) {
                    at = i + 1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (size_t i = changes.size(); i-- > 0;) {
                    if (ContainsLineBreak(changes[i].text)) {
                        at = i + 1;
                        break;
                    }
                }
            }
        }
        changes.insert(changes.begin() + static_cast<long>(at), breaks.begin(), breaks.end());
    }

    LineColumnDelta Format(LineColumn lc, Trivia trivia, std::vector<OutTrivia> &changes) {
        if (trivia->structure) return FormatStructured(lc, trivia, changes);
        U16View text = TriviaText(tree_, trivia);
        if (trivia->Kind() == SyntaxKind::MultiLineCommentTrivia) {
            int indentation = lc.column;
            int delta = indentation - GetExistingIndentation(trivia);
            if (delta != 0) {
                U16 comment = ReindentStartOfComment(text, indentation, delta);
                LineColumnDelta d = GetLineColumnDelta(lc.column, comment);
                changes.push_back({trivia->Kind(), std::move(comment)});
                return d;
            }
        }
        changes.push_back({trivia->Kind(), U16(text)});
        return GetLineColumnDelta(lc.column, text);
    }

    LineColumnDelta FormatStructured(LineColumn lc, Trivia trivia, std::vector<OutTrivia> &changes) {
        U16View text = TriviaText(tree_, trivia);
        if (trivia->Kind() == SyntaxKind::SkippedTokensTrivia) {
            succeeded_ = false;
            changes.push_back({trivia->Kind(), U16(text)});
            return GetLineColumnDelta(lc.column, text);
        }
        if (!IsDocComment(trivia->Kind())) {
            bool changed = false;
            U16 formatted = FormatStructuredTrivia(tree_, rules_, opt_, trivia, initial_.column, &changed);
            LineColumnDelta d = GetLineColumnDelta(lc.column, formatted);
            changes.push_back({trivia->Kind(), std::move(formatted)});
            return d;
        }
        U16 doc = FormatDocumentComment(lc, trivia);
        LineColumnDelta d = GetLineColumnDelta(lc.column, doc);
        changes.push_back({trivia->Kind(), std::move(doc)});
        return d;
    }

    /** @brief FormattingExtensions.ReindentStartOfXmlDocumentationComment with forceIndentation false. */
    U16 ReindentStartOfComment(U16View text, int indentation, int delta) const {
        (void)indentation;
        std::vector<U16View> lines;
        size_t start = 0;
        while (true) {
            size_t nl = text.find(u'\n', start);
            if (nl == U16View::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, nl - start));
            start = nl + 1;
        }
        auto trim_crlf_end = [](U16View s) {
            while (!s.empty() && (s.back() == u'\r' || s.back() == u'\n')) s.remove_suffix(1);
            return s;
        };
        U16 out;
        U16View first = trim_crlf_end(lines[0]);
        while (!first.empty() && (first.front() == u'\r' || first.front() == u'\n')) first.remove_prefix(1);
        out.append(first);
        if (lines.size() > 1) out += opt_.new_line;
        for (size_t i = 1; i < lines.size(); i++) {
            U16View line = trim_crlf_end(lines[i]);
            int nonws = GetFirstNonWhitespaceIndexInString(line);
            if (nonws >= 0) {
                int current = GetColumnFromLineOffset(line, nonws, opt_.tab_size);
                int new_indentation = std::max(current + delta, 0);
                out += CreateIndentationString(new_indentation, opt_.use_tabs, opt_.tab_size);
                out.append(line.substr(static_cast<size_t>(nonws)));
            }
            if (i < lines.size() - 1) out += opt_.new_line;
        }
        return out;
    }

    /** @brief FormattingExtensions.AdjustIndentForXmlDocExteriorTrivia. */
    U16 AdjustIndentForXmlDocExteriorTrivia(U16View text, bool force, int indentation, int delta) const {
        bool empty = false;
        int nonws = GetFirstNonWhitespaceIndexInString(text);
        if (nonws == -1) {
            empty = true;
            nonws = static_cast<int>(text.size());
        }
        int new_indentation =
            force ? indentation : std::max(GetColumnFromLineOffset(text, nonws, opt_.tab_size) + delta, 0);
        U16 out = CreateIndentationString(new_indentation, opt_.use_tabs, opt_.tab_size);
        if (!empty) out.append(text.substr(static_cast<size_t>(nonws)));
        return out;
    }

    /** @brief DocumentationCommentExteriorCommentRewriter over a doc comment: its text with exteriors re-indented. */
    U16 RewriteDocComment(Trivia trivia, bool force, int indentation, int delta) const {
        const Node *doc = trivia->structure;
        TextSpan doc_span = doc->Span();
        U16 out;
        int pos = trivia->position;
        int end = trivia->position + trivia->green->full_width;
        std::vector<Token> tokens;
        std::vector<const Node *> stack{doc};
        // Tokens of the structure, in order.
        struct Walk {
            static void Collect(const Node *n, std::vector<Token> &out) {
                if (n->IsToken()) {
                    out.push_back(n);
                    return;
                }
                for (const Node *c : n->children) Collect(c, out);
            }
        };
        Walk::Collect(doc, tokens);
        auto visit = [&](const csharp::SyntaxTrivia &t) {
            if (t.Kind() != SyntaxKind::DocumentationCommentExteriorTrivia) return;
            int span_end = t.position + t.green->full_width;
            if (span_end == doc_span.start || span_end == doc_span.end) return;
            U16View text = TriviaText(tree_, &t);
            U16 replaced = AdjustIndentForXmlDocExteriorTrivia(text, force, indentation, delta);
            if (replaced == text) return;
            out.append(tree_.TextOf(FromBounds(pos, t.position)));
            out += replaced;
            pos = span_end;
        };
        for (Token tok : tokens) {
            for (const csharp::SyntaxTrivia &t : tok->leading) visit(t);
            for (const csharp::SyntaxTrivia &t : tok->trailing) visit(t);
        }
        out.append(tree_.TextOf(FromBounds(pos, end)));
        return out;
    }

    U16 FormatDocumentComment(LineColumn lc, Trivia trivia) const {
        int indentation = lc.column;
        U16View text = TriviaText(tree_, trivia);
        if (trivia->Kind() == SyntaxKind::SingleLineDocumentationCommentTrivia) {
            U16View trimmed = text;
            while (!trimmed.empty() && IsDotNetWhiteSpace(trimmed.back())) trimmed.remove_suffix(1);
            if (!ContainsLineBreak(trimmed)) return U16(text);
            return RewriteDocComment(trivia, true, indentation, 0);
        }
        int delta = indentation - GetExistingIndentation(trivia);
        if (delta == 0) return U16(text);
        return RewriteDocComment(trivia, false, indentation, delta);
    }

    FormattingContext &context_;
    const Tree &tree_;
    const Rules &rules_;
    const LineOptions &opt_;
    U16View original_;
    int line_breaks_;
    int spaces_;
    Token token1_;
    Token token2_;
    LineColumn initial_;
    int indentation_ = -1;
    bool first_line_blank_ = false;
    bool succeeded_ = true;
};

// --- Complex trivia --------------------------------------------------------------------------------

struct ComplexTrivia;

struct FormattedComplexTrivia : TriviaData {
    FormattedComplexTrivia(FormattingContext &context, Token t1, Token t2, int lines, int spaces_in, U16View original)
        : TriviaData(Type::FormattedComplex) {
        line_breaks = std::max(0, lines);
        spaces = std::max(0, spaces_in);
        TriviaFormatter formatter(context, t1, t2, original, line_breaks, spaces);
        std::vector<OutTrivia> list = formatter.FormatToSyntaxTrivia();
        succeeded = formatter.Succeeded();
        for (const OutTrivia &t : list) text += t.text;
    }
    bool succeeded = true;
    U16 text;
    bool ContainsChanges() const override { return succeeded; }
    U16 NewText() const override { return text; }
    TriviaData *WithSpace(int, FormattingContext &) override { return this; }
    TriviaData *WithLine(int, int, FormattingContext &) override { return this; }
    TriviaData *WithIndentation(int, FormattingContext &) override { return this; }
    void Format(FormattingContext &, int) override {}
};

/** @brief ProcessTextBetweenTokens (ComplexTrivia.ExtractLineAndSpace). */
void ExtractLineAndSpace(const Engine &engine, U16View text, Token token1, int *lines, int *spaces) {
    int tab = engine.options.tab_size;
    *lines = GetNumberOfLineBreaks(text);
    if (*lines > 0) {
        U16View line = GetLastLineText(text);
        *spaces = GetColumnFromLineOffset(line, static_cast<int>(line.size()), tab);
        return;
    }
    int initial = token1 ? engine.GetOriginalColumn(token1) : 0;
    *spaces = ConvertTabToSpace(text, tab, GetTextColumn(TokenText(token1), tab, initial), static_cast<int>(text.size()));
}

struct ModifiedComplexTrivia : TriviaData {
    ModifiedComplexTrivia(ComplexTrivia *orig, int lines, int sp) : TriviaData(Type::ModifiedComplex), original(orig) {
        line_breaks = lines;
        spaces = sp;
    }
    ComplexTrivia *original;
    TriviaData *WithSpace(int space, FormattingContext &context) override;
    TriviaData *WithLine(int line, int indentation, FormattingContext &context) override;
    TriviaData *WithIndentation(int indentation, FormattingContext &context) override;
    void Format(FormattingContext &context, int pair_index) override;
};

/** @brief The CodeShapeAnalyzer: does a multi-line complex trivia need formatting? */
class CodeShapeAnalyzer {
public:
    CodeShapeAnalyzer(FormattingContext &context, bool first_trivia_in_tree, const std::vector<Trivia> &list)
        : context_(context), list_(list), last_line_break_index_(first_trivia_in_tree ? 0 : -1) {}

    bool ShouldFormat() {
        int index = -1;
        for (Trivia t : list_) {
            index++;
            if (OnWhitespace(t) || OnEndOfLine(t, index) || OnTouchedNoisyCharacter(t) || OnComment(t) ||
                OnRegion(t, index) || OnPreprocessor(t, index) || OnDisabledTextTrivia(t, index)) {
                return true;
            }
        }
        return false;
    }

private:
    bool UseIndentation() const { return last_line_break_index_ >= 0; }
    const Tree &tree() const { return context_.engine.tree; }

    bool OnWhitespace(Trivia t) {
        if (t->Kind() != SyntaxKind::WhitespaceTrivia) return false;
        if (!UseIndentation() || touched_noisy_) {
            has_trailing_space_ = true;
            return false;
        }
        U16View text = TriviaText(tree(), t);
        if (text.find(u'\t') != U16View::npos) return true;
        indentation_ += ConvertTabToSpace(text, context_.Options().tab_size, indentation_, static_cast<int>(text.size()));
        return false;
    }

    bool OnEndOfLine(Trivia t, int index) {
        if (t->Kind() != SyntaxKind::EndOfLineTrivia) return false;
        if (has_trailing_space_) return true;
        if (indentation_ > 0 && !touched_noisy_) return true;
        Reset(index);
        return false;
    }

    void Reset(int index) {
        indentation_ = 0;
        touched_noisy_ = false;
        has_trailing_space_ = false;
        last_line_break_index_ = index;
    }

    bool OnComment(Trivia t) {
        if (!IsRegularOrDocComment(t->Kind())) return false;
        if (UseIndentation() && indentation_ != context_.GetBaseIndentation(TriviaSpanStart(t))) return true;
        if (t->Kind() == SyntaxKind::SingleLineDocumentationCommentTrivia && ShouldFormatSingleLineDoc(t)) return true;
        return false;
    }

    bool ShouldFormatSingleLineDoc(Trivia t) const {
        bool saw_first = false;
        std::vector<Token> tokens;
        struct Walk {
            static void Collect(const Node *n, std::vector<Token> &out) {
                if (n->IsToken()) {
                    out.push_back(n);
                    return;
                }
                for (const Node *c : n->children) Collect(c, out);
            }
        };
        Walk::Collect(t->structure, tokens);
        for (Token tok : tokens) {
            for (const csharp::SyntaxTrivia &x : tok->leading) {
                if (x.Kind() == SyntaxKind::DocumentationCommentExteriorTrivia) {
                    if (!saw_first) {
                        saw_first = true;
                        break;
                    }
                    U16View text = TriviaText(tree(), &x);
                    int offset = static_cast<int>(text.size()) - 3;
                    if (offset < 0) offset = 0;
                    if (GetColumnFromLineOffset(text, offset, context_.Options().tab_size) != indentation_) return true;
                    break;
                }
            }
        }
        return false;
    }

    bool OnRegion(Trivia t, int index) {
        if (t->Kind() != SyntaxKind::RegionDirectiveTrivia && t->Kind() != SyntaxKind::EndRegionDirectiveTrivia) return false;
        if (!UseIndentation()) return true;
        if (indentation_ != context_.GetBaseIndentation(TriviaSpanStart(t))) return true;
        Reset(index);
        return false;
    }

    bool OnPreprocessor(Trivia t, int index) {
        if (!csharp::SyntaxFacts::IsPreprocessorDirective(t->Kind())) return false;
        if (!UseIndentation()) return true;
        if (indentation_ != 0) return true;
        Reset(index);
        return false;
    }

    bool OnTouchedNoisyCharacter(Trivia t) {
        if (t->Kind() == SyntaxKind::WhitespaceTrivia || t->Kind() == SyntaxKind::EndOfLineTrivia) return false;
        touched_noisy_ = true;
        has_trailing_space_ = false;
        return false;
    }

    bool OnDisabledTextTrivia(Trivia t, int index) {
        if (t->Kind() == SyntaxKind::DisabledTextTrivia) {
            U16View text = TriviaText(tree(), t);
            if (!text.empty() && IsCSharpNewLine(text.back())) Reset(index);
        }
        return false;
    }

    FormattingContext &context_;
    const std::vector<Trivia> &list_;
    int indentation_ = 0;
    bool has_trailing_space_ = false;
    int last_line_break_index_;
    bool touched_noisy_ = false;
};

bool ShouldFormatSingleLine(const Tree &tree, const std::vector<Trivia> &list) {
    for (Trivia t : list) {
        SyntaxKind k = t->Kind();
        if (k == SyntaxKind::WhitespaceTrivia) {
            if (TriviaText(tree, t).find(u'\t') != U16View::npos) return true;
        }
        if (IsRegularOrDocComment(k)) return false;
        if (csharp::SyntaxFacts::IsPreprocessorDirective(k)) return false;
    }
    return true;
}

struct ComplexTrivia : TriviaData {
    ComplexTrivia(const Engine &engine, Token t1, Token t2) : TriviaData(Type::Complex), token1(t1), token2(t2) {
        original = engine.GetTextBetween(t1, t2);
        ExtractLineAndSpace(engine, original, token1, &line_breaks, &spaces);
    }
    Token token1;
    Token token2;
    U16View original;

    TriviaData *WithSpace(int, FormattingContext &) override { return this; }

    TriviaData *WithLine(int line, int indentation, FormattingContext &context) override {
        if (!SecondTokenIsFirstTokenOnLine()) return context.engine.New<ModifiedComplexTrivia>(this, line, indentation);
        if (line_breaks < line) return context.engine.New<ModifiedComplexTrivia>(this, line, indentation);
        if (line_breaks == line) return WithIndentation(indentation, context);
        return this;
    }

    TriviaData *WithIndentation(int indentation, FormattingContext &context) override {
        if (!SecondTokenIsFirstTokenOnLine()) return this;
        if (spaces == indentation) return this;
        std::vector<Trivia> list = TriviaListBetween(token1, token2);
        if (ContainsSkippedTokensOrText(list)) return this;
        FormattedComplexTrivia formatted(context, token1, token2, line_breaks, indentation, original);
        int lines = 0, sp = 0;
        ExtractLineAndSpace(context.engine, formatted.text, token1, &lines, &sp);
        if (!formatted.succeeded) {
            // FormatToSyntaxTrivia hands back the original trivia then.
            ExtractLineAndSpace(context.engine, original, token1, &lines, &sp);
        }
        return context.engine.New<ModifiedComplexTrivia>(this, lines, sp);
    }

    bool ShouldFormat(FormattingContext &context) const {
        int end = token2 ? SpanStart(token2) : SpanOf(token1).end;
        TextSpan span = FromBounds(token1 ? SpanOf(token1).end : 0, end);
        if (context.IsSpacingSuppressed(span)) return false;
        std::vector<Trivia> list = TriviaListBetween(token1, token2);
        if (ContainsSkippedTokensOrText(list)) return false;
        if (!SecondTokenIsFirstTokenOnLine()) return ShouldFormatSingleLine(context.engine.tree, list);
        if (context.Options().use_tabs) return true;
        CodeShapeAnalyzer analyzer(context, token1 == nullptr, list);
        return analyzer.ShouldFormat();
    }

    void Format(FormattingContext &context, int pair_index) override {
        if (!ShouldFormat(context)) return;
        context.Apply(pair_index,
                      context.engine.New<FormattedComplexTrivia>(context, token1, token2, line_breaks, spaces, original));
    }
};

TriviaData *ModifiedComplexTrivia::WithSpace(int space, FormattingContext &context) {
    return original->WithSpace(space, context);
}
TriviaData *ModifiedComplexTrivia::WithLine(int line, int indentation, FormattingContext &context) {
    return original->WithLine(line, indentation, context);
}
TriviaData *ModifiedComplexTrivia::WithIndentation(int indentation, FormattingContext &context) {
    return original->WithIndentation(indentation, context);
}
void ModifiedComplexTrivia::Format(FormattingContext &context, int pair_index) {
    std::vector<Trivia> list = TriviaListBetween(original->token1, original->token2);
    if (ContainsSkippedTokensOrText(list)) return;
    context.Apply(pair_index, context.engine.New<FormattedComplexTrivia>(context, original->token1, original->token2,
                                                                          line_breaks, spaces, original->original));
}

}  // namespace

// --- TriviaFactory ---------------------------------------------------------------------------------

TriviaFactory::TriviaFactory(Engine &e) : engine(e) {
    for (int i = 0; i < 10; i++) spaces_.push_back(engine.New<Whitespace>(0, i));
    for (int line = 0; line < 5; line++) {
        for (int level = 0; level < 20; level++) {
            whitespaces_.push_back(engine.New<Whitespace>(line + 1, level * engine.options.indentation_size));
        }
    }
}

TriviaData *TriviaFactory::GetSpaceTriviaData(int space) {
    if (space < 10) return spaces_[static_cast<size_t>(space)];
    return engine.New<Whitespace>(0, space);
}

TriviaData *TriviaFactory::GetWhitespaceTriviaData(int line_breaks, int indentation, bool use_as_is) {
    int size = engine.options.indentation_size;
    bool can_use_cache = use_as_is && line_breaks > 0 && line_breaks <= 5 && indentation % size == 0;
    if (can_use_cache) {
        int level = indentation / size;
        if (level < 20) return whitespaces_[static_cast<size_t>((line_breaks - 1) * 20 + level)];
    }
    if (use_as_is) return engine.New<Whitespace>(line_breaks, indentation);
    return engine.New<ModifiedWhitespace>(nullptr, line_breaks, indentation);
}

namespace {

TriviaData *WhitespaceOnlyInfo(TriviaFactory &f, Token t1, Token t2, const AnalysisResult &r) {
    if (!ContainsOnlyWhitespace(r)) return nullptr;
    const LineOptions &o = f.engine.options;
    int space = (r.has_trailing_space || r.has_unknown_whitespace || r.line_breaks > 0 || r.tab > 0) ? -1 : r.space;
    if (space >= 0) return f.GetSpaceTriviaData(space);
    if (r.line_breaks == 0 && r.tab > 0) {
        int initial = t1 ? f.engine.GetOriginalColumn(t1) + SpanOf(t1).Length() : 0;
        U16View snippet = f.engine.GetTextBetween(t1, t2);
        int spaces = ConvertTabToSpace(snippet, o.tab_size, initial, static_cast<int>(snippet.size()));
        return f.engine.New<ModifiedWhitespace>(nullptr, r.line_breaks, spaces);
    }
    int indentation = r.tab * o.tab_size + r.space;
    bool as_is;
    if (r.has_trailing_space || r.has_unknown_whitespace) {
        if (r.has_unknown_whitespace && r.line_breaks == 0 && indentation == 0) indentation = 1;
        as_is = false;
    } else if (!o.use_tabs) {
        as_is = r.tab <= 0;
    } else if (r.has_tab_after_space) {
        as_is = false;
    } else if (r.space >= o.tab_size) {
        as_is = false;
    } else {
        as_is = true;
    }
    return f.GetWhitespaceTriviaData(r.line_breaks, indentation, as_is);
}

}  // namespace

TriviaData *TriviaFactory::CreateLeadingTrivia(Token token) {
    if (!HasLeadingTrivia(token)) return GetSpaceTriviaData(0);
    AnalysisResult r;
    AnalyzeList(engine.tree, token->leading, r);
    if (TriviaData *info = WhitespaceOnlyInfo(*this, nullptr, token, r)) return info;
    return engine.New<ComplexTrivia>(engine, nullptr, token);
}

TriviaData *TriviaFactory::CreateTrailingTrivia(Token token) {
    if (!HasTrailingTrivia(token)) return GetSpaceTriviaData(0);
    AnalysisResult r;
    AnalyzeList(engine.tree, token->trailing, r);
    if (TriviaData *info = WhitespaceOnlyInfo(*this, token, nullptr, r)) return info;
    return engine.New<ComplexTrivia>(engine, token, nullptr);
}

TriviaData *TriviaFactory::Create(Token token1, Token token2) {
    if (!HasTrailingTrivia(token1) && !HasLeadingTrivia(token2)) return GetSpaceTriviaData(0);
    AnalysisResult r;
    if (IsMissing(token1) && FullWidth(token1) == 0) {
        for (Token current = token1; current;) {
            Token previous = engine.tree.PreviousToken(current, true);
            if (FullWidth(previous) == 0) {
                current = previous;
                continue;
            }
            if (!previous->trailing.empty() && previous->trailing.back().Kind() == SyntaxKind::EndOfLineTrivia) {
                r.line_breaks = 1;
            }
            break;
        }
    } else if (token1) {
        AnalyzeList(engine.tree, token1->trailing, r);
    }
    if (token2) AnalyzeList(engine.tree, token2->leading, r);
    if (TriviaData *info = WhitespaceOnlyInfo(*this, token1, token2, r)) return info;
    return engine.New<ComplexTrivia>(engine, token1, token2);
}

}  // namespace csfmt
