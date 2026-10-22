// The driver of mep's C formatter (c_format.h): lex, parse into one run of
// unwrapped lines per #if branch combination, annotate and format each run,
// merge the whitespace replacements the runs produce (the first run to
// format a token wins, as in clang-format), apply them, and proof-read the
// result against the input before returning it.

#include "c_format.h"

#include "c_format_internal.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>

namespace cfmt {

namespace {

using namespace detail;

bool inputUsesCRLF(std::string_view Text) {
    size_t LF = static_cast<size_t>(std::count(Text.begin(), Text.end(), '\n'));
    size_t CR = static_cast<size_t>(std::count(Text.begin(), Text.end(), '\r')) * 2;
    return LF == CR ? false : CR > LF;
}

// Adds R to the merged set; false on a conflict clang-format would refuse.
bool addReplacement(std::map<size_t, Replacement> &Merged, Replacement R) {
    auto It = Merged.lower_bound(R.Offset);
    // Same range, same text: nothing new.
    if (It != Merged.end() && It->second.Offset == R.Offset && It->second.Length == R.Length &&
        It->second.Text == R.Text) {
        return true;
    }
    auto overlaps = [](const Replacement &A, const Replacement &B) {
        if (A.Length == 0 && B.Length == 0) return A.Offset == B.Offset;
        if (A.Length == 0) return B.Offset < A.Offset && A.Offset < B.Offset + B.Length;
        if (B.Length == 0) return A.Offset < B.Offset && B.Offset < A.Offset + A.Length;
        return A.Offset < B.Offset + B.Length && B.Offset < A.Offset + A.Length;
    };
    if (It != Merged.end() && overlaps(It->second, R)) {
        if (It->second.Length == 0 && R.Length == 0 && R.Text + It->second.Text == It->second.Text + R.Text) {
            It->second.Text += R.Text;
            return true;
        }
        return false;
    }
    if (It != Merged.begin()) {
        auto Prev = std::prev(It);
        if (overlaps(Prev->second, R)) return false;
    }
    Merged.emplace(R.Offset, std::move(R));
    return true;
}

// --- Proof-reading ------------------------------------------------------------------

struct ProofToken {
    tok Kind;
    std::string Text;  // normalized
    bool InDirective;
    bool DefineParenAdjacent;  // for the token after a #define's name
    size_t Offset;             // where it starts in its text
};

std::vector<std::string> commentWords(std::string_view Text) {
    std::vector<std::string> Words;
    bool Block = Text.substr(0, 2) == "/*";
    if (Block) {
        Text.remove_prefix(2);
        if (Text.size() >= 2 && Text.substr(Text.size() - 2) == "*/") Text.remove_suffix(2);
    }
    size_t Pos = 0;
    bool FirstLine = true;
    while (Pos <= Text.size()) {
        size_t NL = Text.find('\n', Pos);
        std::string_view Line = Text.substr(Pos, NL == std::string_view::npos ? std::string_view::npos : NL - Pos);
        // Strip leading whitespace, `//` markers and block-comment decorations.
        size_t B = Line.find_first_not_of(" \t\v\f\r");
        Line = B == std::string_view::npos ? std::string_view() : Line.substr(B);
        if (!Block) {
            while (Line.substr(0, 2) == "//") {
                Line.remove_prefix(2);
                while (!Line.empty() && (Line[0] == '/' || Line[0] == '!' || Line[0] == '<' || Line[0] == ':'))
                    Line.remove_prefix(1);
            }
        } else if (!FirstLine) {
            while (!Line.empty() && Line[0] == '*') Line.remove_prefix(1);
        }
        size_t I = 0;
        while (I < Line.size()) {
            while (I < Line.size() && (Line[I] == ' ' || Line[I] == '\t' || Line[I] == '\v' || Line[I] == '\f' ||
                                       Line[I] == '\r' || Line[I] == '\\'))
                ++I;
            size_t S = I;
            while (I < Line.size() && !(Line[I] == ' ' || Line[I] == '\t' || Line[I] == '\v' || Line[I] == '\f' ||
                                        Line[I] == '\r'))
                ++I;
            // Reflowing can bring a `*` of the text (`PyObject **p`) to the
            // start of a line, where it reads as decoration: ignore every `*`.
            std::string W;
            for (size_t J = S; J < I; ++J)
                if (Line[J] != '*') W += Line[J];
            if (!W.empty()) Words.push_back(std::move(W));
        }
        FirstLine = false;
        if (NL == std::string_view::npos) break;
        Pos = NL + 1;
    }
    return Words;
}

bool isStringKind(tok K) {
    return K == tok::string_literal || K == tok::wide_string_literal || K == tok::utf8_string_literal ||
           K == tok::utf16_string_literal || K == tok::utf32_string_literal;
}

std::vector<ProofToken> proofTokens(std::string_view Text) {
    Style S;
    Source Src;
    Src.Text = Text;
    Lex(Src, S);
    std::vector<ProofToken> Out;
    bool InDirective = false;
    const FormatToken *DefineName = nullptr;
    std::vector<std::string> PendingComment;
    size_t CommentOffset = 0;
    bool CommentDir = false;
    auto flushComment = [&]() {
        if (PendingComment.empty()) return;
        std::string Joined;
        for (const std::string &W : PendingComment) {
            Joined += W;
            Joined += ' ';
        }
        Out.push_back({tok::comment, Joined, CommentDir, false, CommentOffset});
        PendingComment.clear();
    };
    for (size_t i = 0; i < Src.Tokens.size(); ++i) {
        const FormatToken *T = Src.Tokens[i];
        if (T->HasUnescapedNewline || T->IsFirst) InDirective = false;
        if (T->is(tok::hash) && (T->HasUnescapedNewline || T->IsFirst)) InDirective = true;
        if (T->is(tok::comment)) {
            if (PendingComment.empty()) {
                CommentOffset = static_cast<size_t>(T->TokenText.data() - Text.data());
                CommentDir = InDirective;
            }
            for (std::string &W : commentWords(T->TokenText)) PendingComment.push_back(std::move(W));
            continue;
        }
        flushComment();
        if (T->is(tok::eof)) break;
        ProofToken P{T->Kind, std::string(T->TokenText), InDirective, false,
                     static_cast<size_t>(T->TokenText.data() - Text.data())};
        if (DefineName && i > 0 && Src.Tokens[i - 1] == DefineName) {
            P.DefineParenAdjacent = T->is(tok::l_paren) && !T->hasWhitespaceBefore();
            DefineName = nullptr;
        }
        if (T->HasIdentifierInfo && T->PPKind == pp::define && i > 0 && Src.Tokens[i - 1]->is(tok::hash) &&
            i + 1 < Src.Tokens.size()) {
            DefineName = Src.Tokens[i + 1];
        }
        // Adjacent string literals concatenate: compare their joined content.
        if (isStringKind(T->Kind) && !Out.empty() && isStringKind(Out.back().Kind) &&
            Out.back().InDirective == InDirective) {
            std::string_view Body = T->TokenText;
            size_t Q = Body.find('"');
            if (Q != std::string_view::npos && Body.size() >= Q + 2) {
                std::string &Prev = Out.back().Text;
                if (!Prev.empty() && Prev.back() == '"') {
                    Prev.pop_back();
                    Prev += Body.substr(Q + 1);
                    continue;
                }
            }
        }
        Out.push_back(std::move(P));
    }
    flushComment();
    return Out;
}

int lineOf(std::string_view Text, size_t Offset) {
    return 1 + static_cast<int>(std::count(Text.begin(), Text.begin() + std::min(Offset, Text.size()), '\n'));
}

std::string shortened(const std::string &S) { return S.size() > 40 ? S.substr(0, 37) + "..." : S; }

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result R;
    Style S;
    S.ColumnLimit = opts.column_limit;
    S.IndentWidth = opts.indent_width;
    S.ContinuationIndentWidth = opts.continuation_indent_width;
    S.TabWidth = opts.tab_width;
    S.UseTab = opts.use_tabs ? Style::UT_ForIndentation : Style::UT_Never;
    S.IndentCaseLabels = opts.indent_case_labels;

    Source Src;
    Src.Text = src;
    Lex(Src, S);
    const bool UseCRLF = inputUsesCRLF(src);

    // Each run formats the whole file once, so a file with many #if
    // combinations costs that many passes (clang-format's own cost: on
    // sqlite3.c's 252 it takes minutes and tens of gigabytes). The first
    // run knows how many there will be; past the budget, refuse up front.
    std::map<size_t, Replacement> Merged;
    std::string Failure;
    const size_t TokenCount = std::max<size_t>(Src.Tokens.size(), 1);
    ParseUnwrappedLines(Src, S, [&](std::vector<UnwrappedLine> &Run, size_t PredictedRuns) {
        if (opts.max_work > 0 && PredictedRuns * TokenCount > opts.max_work) {
            Failure = "too many #if branch combinations (" + std::to_string(PredictedRuns) +
                      ") to lay out a file this size; left unchanged";
            return false;
        }
        if (Run.empty()) return true;
        TokenAnnotator Annotator(S);
        std::vector<std::unique_ptr<AnnotatedLine>> Owned;
        std::vector<AnnotatedLine *> Lines;
        Owned.reserve(Run.size());
        Lines.reserve(Run.size());
        for (const UnwrappedLine &L : Run) {
            Owned.push_back(std::make_unique<AnnotatedLine>(L));
            Lines.push_back(Owned.back().get());
            Annotator.annotate(*Lines.back());
        }
        for (AnnotatedLine *L : Lines) Annotator.calculateFormattingInformation(*L);
        Annotator.setCommentLineLevels(Lines);
        WhitespaceManager Whitespaces(Src, S, UseCRLF);
        FormatLines(Lines, Src, S, Whitespaces);
        for (Replacement &Rep : Whitespaces.generateReplacements()) {
            if (!addReplacement(Merged, std::move(Rep))) {
                Failure = "conflicting layouts for different #if branches; left unchanged";
                return false;
            }
        }
        return true;
    });
    if (!Failure.empty()) {
        R.ok = false;
        R.text = std::string(src);
        R.error = std::move(Failure);
        return R;
    }

    std::string Out;
    Out.reserve(src.size() + src.size() / 8);
    size_t Pos = 0;
    for (const auto &[Offset, Rep] : Merged) {
        Out.append(src.substr(Pos, Rep.Offset - Pos));
        Out.append(Rep.Text);
        Pos = Rep.Offset + Rep.Length;
    }
    Out.append(src.substr(Pos));

    if (opts.verify) {
        std::vector<ProofToken> Before = proofTokens(src);
        std::vector<ProofToken> After = proofTokens(Out);
        bool Same = true;
        size_t Mismatch = std::min(Before.size(), After.size());
        for (size_t i = 0; Same && i < std::min(Before.size(), After.size()); ++i) {
            const ProofToken &A = Before[i];
            const ProofToken &B = After[i];
            if (A.Kind != B.Kind || A.Text != B.Text || A.InDirective != B.InDirective ||
                A.DefineParenAdjacent != B.DefineParenAdjacent) {
                Same = false;
                Mismatch = i;
            }
        }
        if (Same && Before.size() != After.size()) Same = false;
        if (!Same) {
            R.ok = false;
            R.text = std::string(src);
            R.error = "the formatted text did not read back the same (a formatter bug); left unchanged";
            if (Mismatch < Before.size()) {
                R.error_line = lineOf(src, Before[Mismatch].Offset);
                if (std::getenv("MEP_CFMT_DEBUG") && Mismatch < After.size())
                    R.error += ": `" + shortened(Before[Mismatch].Text) + "` became `" +
                               shortened(After[Mismatch].Text) + "`";
            }
            return R;
        }
    }
    R.ok = true;
    R.text = std::move(Out);
    return R;
}

}  // namespace cfmt
