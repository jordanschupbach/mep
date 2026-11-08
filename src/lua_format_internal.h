#pragma once

// Internals shared by the Lua formatter's translation units
// (lua_format.cpp: statements, blocks, assignments, trivia; lua_format_expr.cpp:
// expressions, calls, functions, tables). The layout rules follow StyLua
// 2.3.1's formatters module; names mirror its functions so the two can be
// read side by side.

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lua_format_cst.h"
#include "lua_format_walk.h"

namespace luafmt::detail {

using namespace cst;  // NOLINT

struct Config {
    size_t column_width = 120;
    size_t indent_width = 4;
    bool tabs = true;
};

struct Ctx {
    Config cfg;
    bool formatting_disabled = false;
};

// StyLua's tactics re-format sub-expressions to compare layouts, which goes
// exponential on pathological nesting (f(f(f(...))) hundreds deep). Every
// formatting step spends from a budget proportional to the input's size; an
// exhausted budget abandons the format (the input comes back unchanged).
struct WorkExhausted {};
void SpendWork();

struct Indent {
    size_t width = 4;
    size_t block = 0;
    size_t additional = 0;
    size_t IndentWidth() const { return (block + additional) * width; }
};

std::vector<std::string_view> Lines(std::string_view s);

struct Shape {
    Indent indent;
    size_t offset = 0;
    size_t column_width = 120;
    bool simple_heuristics = false;

    static Shape New(const Ctx &ctx) {
        Shape s;
        s.indent.width = ctx.cfg.indent_width;
        s.column_width = ctx.cfg.column_width;
        return s;
    }
    Shape WithInfiniteWidth() const {
        Shape s = *this;
        s.column_width = std::numeric_limits<size_t>::max();
        return s;
    }
    Shape WithIndent(Indent i) const {
        Shape s = *this;
        s.indent = i;
        return s;
    }
    Shape IncrementBlockIndent() const {
        Shape s = *this;
        s.indent.block++;
        return s;
    }
    Shape IncrementAdditionalIndent() const {
        Shape s = *this;
        s.indent.additional++;
        return s;
    }
    Shape AddIndentLevel(size_t n) const {
        Shape s = *this;
        s.indent.additional += n;
        return s;
    }
    Shape WithAdditionalIndent(size_t n) const {
        Shape s = *this;
        s.indent.additional = n;
        return s;
    }
    size_t UsedWidth() const { return indent.IndentWidth() + offset; }
    bool OverBudget() const { return UsedWidth() > column_width; }
    Shape Add(size_t w) const {
        Shape s = *this;
        s.offset += w;
        return s;
    }
    Shape operator+(size_t w) const { return Add(w); }
    Shape WithSimpleHeuristics() const {
        Shape s = *this;
        s.simple_heuristics = true;
        return s;
    }
    Shape Reset() const {
        Shape s = *this;
        s.offset = 0;
        return s;
    }
    Shape TakeFirstLine(std::string_view str) const {
        const auto ls = Lines(str);
        return Add(ls.empty() ? 0 : ls.front().size());
    }
    Shape TakeLastLine(std::string_view str) const {
        const auto ls = Lines(str);
        if (ls.empty()) return Add(0);
        if (ls.size() > 1) return Reset().Add(ls.back().size());
        return Add(ls.back().size());
    }
};

// ---- trivia updates -----------------------------------------------------------

inline const std::optional<size_t> kHang1{size_t{1}};

enum class FTMode { Append, Replace, NoChange };
struct FT {
    FTMode mode = FTMode::NoChange;
    std::vector<Token> v;
    static FT Append(std::vector<Token> t) { return FT{FTMode::Append, std::move(t)}; }
    static FT Replace(std::vector<Token> t) { return FT{FTMode::Replace, std::move(t)}; }
    static FT NoChange() { return FT{}; }
};

inline void ApplyFT(std::vector<Token> &dst, const FT &ft) {
    if (ft.mode == FTMode::Append) {
        dst.insert(dst.end(), ft.v.begin(), ft.v.end());
    } else if (ft.mode == FTMode::Replace) {
        dst = ft.v;
    }
}

template <class T>
T UpdLead(const T &n, const FT &ft) {
    T c = n;
    if (TokenRef *t = FirstTok(c)) ApplyFT(t->leading, ft);
    return c;
}
template <class T>
T UpdTrail(const T &n, const FT &ft) {
    T c = n;
    if (TokenRef *t = LastTok(c)) ApplyFT(t->trailing, ft);
    return c;
}
template <class T>
T UpdTrivia(const T &n, const FT &lead, const FT &trail) {
    T c = n;
    if (TokenRef *t = FirstTok(c)) ApplyFT(t->leading, lead);
    if (TokenRef *t = LastTok(c)) ApplyFT(t->trailing, trail);
    return c;
}
template <class T>
std::vector<Token> LeadTrivia(const T &n) {
    TokenRef *t = FirstTok(const_cast<T &>(n));
    return t ? t->leading : std::vector<Token>{};
}
template <class T>
std::vector<Token> TrailTrivia(const T &n) {
    TokenRef *t = LastTok(const_cast<T &>(n));
    return t ? t->trailing : std::vector<Token>{};
}

template <class T>
T StripTrivia(const T &n) {
    return UpdTrivia(n, FT::Replace({}), FT::Replace({}));
}
template <class T>
T StripLead(const T &n) {
    return UpdLead(n, FT::Replace({}));
}
template <class T>
T StripTrail(const T &n) {
    return UpdTrail(n, FT::Replace({}));
}

enum class CS { Single, Multiline, All };

inline bool IsCommentSearch(const Token &t, CS s) {
    switch (s) {
    case CS::Single:
        return t.type == TT::SingleLineComment;
    case CS::Multiline:
        return t.type == TT::MultiLineComment;
    case CS::All:
        return t.IsComment();
    }
    return false;
}
inline bool AnyComment(const std::vector<Token> &v, CS s) {
    for (const auto &t : v)
        if (IsCommentSearch(t, s)) return true;
    return false;
}
inline bool TriviaIsNewline(const Token &t) {
    return t.type == TT::Whitespace && t.text.find('\n') != std::string::npos;
}
inline bool TriviaIsWhitespace(const Token &t) { return t.type == TT::Whitespace; }

template <class T>
bool HasLeadingComments(const T &n, CS s) {
    return AnyComment(LeadTrivia(n), s);
}
template <class T>
bool HasTrailingComments(const T &n, CS s) {
    return AnyComment(TrailTrivia(n), s);
}
template <class T>
std::vector<Token> LeadingComments(const T &n) {
    std::vector<Token> out;
    for (auto &t : LeadTrivia(n))
        if (t.IsComment()) out.push_back(t);
    return out;
}
Token Spaces(size_t n);
template <class T>
std::vector<Token> TrailingCommentsSearch(const T &n, CS s) {
    std::vector<Token> out;
    for (auto &t : TrailTrivia(n))
        if (IsCommentSearch(t, s)) {
            out.push_back(Spaces(1));
            out.push_back(t);
        }
    return out;
}
template <class T>
std::vector<Token> TrailingComments(const T &n) {
    return TrailingCommentsSearch(n, CS::All);
}
template <class T>
std::pair<T, std::vector<Token>> TakeLeadingComments(const T &n) {
    return {UpdLead(n, FT::Replace({})), LeadingComments(n)};
}
template <class T>
std::pair<T, std::vector<Token>> TakeTrailingComments(const T &n) {
    return {UpdTrail(n, FT::Replace({})), TrailingComments(n)};
}
template <class T>
std::pair<T, std::vector<Token>> TakeTrailingTrivia(const T &n) {
    return {UpdTrail(n, FT::Replace({})), TrailTrivia(n)};
}

inline bool TokenContainsComments(const TokenRef &t) {
    return AnyComment(t.leading, CS::All) || AnyComment(t.trailing, CS::All);
}
template <class T>
bool ContainsComments(const T &n) {
    auto f = [](const TokenRef &t) { return TokenContainsComments(t); };
    return Walk(n, f, true);
}

inline bool HasInlineComments(const TokenRef &) { return false; }
inline bool HasInlineComments(const Var &) { return false; }
bool HasInlineComments(const Expr &e);

template <class T>
bool PunctuatedInlineComments(const Punctuated<T> &p, bool include_leading) {
    for (size_t i = 0; i < p.pairs.size(); i++) {
        const auto &pr = p.pairs[i];
        if (i + 1 < p.pairs.size() && !TrailingComments(pr.value).empty()) return true;
        if ((pr.punct && TokenContainsComments(*pr.punct)) || (include_leading && !LeadingComments(pr.value).empty()) ||
            HasInlineComments(pr.value))
            return true;
    }
    return false;
}

template <class T>
bool SpansMultipleLines(const T &n) {
    return Lines(Print(n)).size() > 1;
}
inline bool SpansMultipleLines(const std::string &s) { return Lines(s).size() > 1; }

template <class T>
std::pair<size_t, size_t> Range(const T &n) {
    TokenRef *a = FirstTok(const_cast<T &>(n));
    TokenRef *b = LastTok(const_cast<T &>(n));
    return {a ? a->tok.start : 0, b ? b->tok.end : 0};
}

// test_over_budget: the node's tokens in node order with comments dropped.
template <class T>
bool TestOverBudget(Shape shape, const T &n) {
    std::string s;
    auto f = [&s](const TokenRef &t) {
        for (const auto &x : t.leading)
            if (!x.IsComment()) AppendTokenText(s, x);
        AppendTokenText(s, t.tok);
        for (const auto &x : t.trailing)
            if (!x.IsComment()) AppendTokenText(s, x);
        return false;
    };
    Walk(n, f, true);
    const auto ls = Lines(s);
    for (size_t i = 0; i < ls.size(); i++) {
        const Shape sh = i == 0 ? shape : shape.Reset();
        if (sh.Add(ls[i].size()).OverBudget()) return true;
    }
    return false;
}

// ---- tokens the formatter makes ----------------------------------------------------

Token Tabs(size_t n);
Token NewlineTok(const Ctx &ctx);
Token IndentTrivia(const Ctx &ctx, Shape shape);
TokenRef Sym(std::string_view text);  // TokenReference::symbol: "  x " with spaces kept as trivia

// ---- general.rs ---------------------------------------------------------------------

enum class FTT { Token, Leading, Trailing };
enum class EndTokenType { IndentComments, InlineComments };

struct FormattedToken {
    Token tok;
    std::vector<Token> leading, trailing;
    bool has_leading = false, has_trailing = false;
};
FormattedToken FormatToken(const Ctx &ctx, const Token &t, FTT type, Shape shape);
std::vector<Token> TriviaToVec(const FormattedToken &ft);
std::vector<Token> LoadTokenTrivia(const Ctx &ctx, const std::vector<Token> &trivia, FTT type, Shape shape);
TokenRef FormatTokenRef(const Ctx &ctx, const TokenRef &t, Shape shape);
TokenRef FormatSymbol(const Ctx &ctx, const TokenRef &cur, const TokenRef &wanted, Shape shape);
inline TokenRef FmtSymbol(const Ctx &ctx, const TokenRef &cur, std::string_view text, Shape shape) {
    return FormatSymbol(ctx, cur, Sym(text), shape);
}
TokenRef FormatEndToken(const Ctx &ctx, const TokenRef &t, EndTokenType type, Shape shape);
ContainedSpan FormatContainedSpan(const Ctx &ctx, const ContainedSpan &c, Shape shape);
TokenRef FormatEof(const Ctx &ctx, const TokenRef &eof, Shape shape);
TokenRef PrependNewlineIndentTok(const Ctx &ctx, const TokenRef &t, Shape shape);

std::vector<Token> PrependNewlineIndentTrivia(const Ctx &ctx, const std::vector<Token> &leading, Shape shape);
template <class T>
T PrependNewlineIndent(const Ctx &ctx, const T &n, Shape shape) {
    return UpdLead(n, FT::Replace(PrependNewlineIndentTrivia(ctx, LeadTrivia(n), shape)));
}

template <class T, class F>
Punctuated<T> FormatPunctuated(const Ctx &ctx, const Punctuated<T> &old, Shape shape, F fmt) {
    Punctuated<T> list;
    for (const auto &pr : old.pairs) {
        if (pr.punct) {
            T v = fmt(ctx, pr.value, shape);
            TokenRef p = FmtSymbol(ctx, *pr.punct, ",", shape);
            p.trailing.push_back(Spaces(1));
            shape = shape.TakeLastLine(Print(v)) + 2;
            list.push(std::move(v), std::move(p));
        } else {
            list.push(fmt(ctx, pr.value, shape));
        }
    }
    return list;
}

template <class T, class F>
Punctuated<T> FormatPunctuatedMultiline(const Ctx &ctx, const Punctuated<T> &old, Shape shape, F fmt,
                                        std::optional<size_t> hang_level) {
    Punctuated<T> out;
    const Shape hanging = hang_level ? shape.WithIndent(shape.AddIndentLevel(*hang_level).indent) : shape;
    for (size_t idx = 0; idx < old.pairs.size(); idx++) {
        const auto &pr = old.pairs[idx];
        const Shape sh = idx == 0 ? shape : hanging.Reset();
        T v = fmt(ctx, pr.value, sh);
        if (idx != 0) v = PrependNewlineIndent(ctx, v, hanging);
        if (pr.punct) {
            std::vector<Token> tc = TrailingComments(v);
            v = UpdTrail(v, FT::Replace({}));
            TokenRef p = FmtSymbol(ctx, *pr.punct, ",", sh);
            tc.insert(tc.end(), p.trailing.begin(), p.trailing.end());
            p.trailing = std::move(tc);
            out.push(std::move(v), std::move(p));
        } else {
            out.push(std::move(v));
        }
    }
    return out;
}

template <class T, class F>
Punctuated<T> TryFormatPunctuated(const Ctx &ctx, const Punctuated<T> &old, Shape shape, F fmt,
                                  std::optional<size_t> hang_level) {
    if (PunctuatedInlineComments(old, false)) return FormatPunctuatedMultiline(ctx, old, shape, fmt, hang_level);
    return FormatPunctuated(ctx, old, shape, fmt);
}

template <class T, class F>
std::pair<ContainedSpan, Punctuated<T>> FormatContainedPunctuatedMultiline(const Ctx &ctx, const ContainedSpan &parens,
                                                                         const Punctuated<T> &args, F fmt,
                                                                         Shape shape) {
    TokenRef open = FormatTokenRef(ctx, parens.open, shape);
    open.trailing.push_back(NewlineTok(ctx));
    TokenRef close = FormatEndToken(ctx, parens.close, EndTokenType::IndentComments, shape);
    close.leading.push_back(IndentTrivia(ctx, shape));
    Punctuated<T> out;
    const Shape inner = shape.IncrementAdditionalIndent();
    for (const auto &pr : args.pairs) {
        const Shape sh = inner.Reset();
        T a = UpdLead(fmt(ctx, pr.value, sh), FT::Append({IndentTrivia(ctx, sh)}));
        std::vector<Token> multi = TrailingCommentsSearch(a, CS::Multiline);
        std::vector<Token> single = TrailingCommentsSearch(a, CS::Single);
        a = UpdTrail(a, FT::Replace(multi));
        TokenRef p;
        if (pr.punct) {
            TokenRef sym = FmtSymbol(ctx, *pr.punct, ",", sh);
            std::vector<Token> tt;
            for (const auto &x : sym.leading)
                if (x.IsComment()) {
                    tt.push_back(NewlineTok(ctx));
                    tt.push_back(IndentTrivia(ctx, sh));
                    tt.push_back(x);
                }
            tt.insert(tt.end(), single.begin(), single.end());
            tt.push_back(NewlineTok(ctx));
            sym.leading.clear();
            sym.trailing.insert(sym.trailing.end(), tt.begin(), tt.end());
            p = std::move(sym);
        } else {
            // StyLua's phantom comma: just the newline (with any moved comments before it).
            p.leading = single;
            p.tok = NewlineTok(ctx);
        }
        out.push(std::move(a), std::move(p));
    }
    return {ContainedSpan{std::move(open), std::move(close)}, std::move(out)};
}

// ---- formatters (lua_format.cpp) ----------------------------------------------------------

Block FormatBlock(const Ctx &ctx, const Block &b, Shape shape);
LastStmt FormatLastStmtNoTrivia(const Ctx &ctx, const LastStmt &s, Shape shape);
Stmt FormatStmtNoTrivia(const Ctx &ctx, const Stmt &s, Shape shape);
TokenRef HangEqualToken(const Ctx &ctx, const TokenRef &eq, Shape shape, bool indent_first_item);
std::pair<TokenRef, Expr> HangAtEqualsDueToComments(const Ctx &ctx, const TokenRef &eq, const Expr &e, Shape shape);
Punctuated<Expr> HangPunctuatedList(const Ctx &ctx, const Punctuated<Expr> &p, Shape shape);
std::optional<size_t> CalculateHangLevel(const Expr &e);
Expr RemoveConditionParentheses(const Expr &e);

// ---- formatters (lua_format_expr.cpp) -------------------------------------------------------

Expr FormatExpression(const Ctx &ctx, const Expr &e, Shape shape);
Expr HangExpression(const Ctx &ctx, const Expr &e, Shape shape, std::optional<size_t> hang_level);
Expr HangExpressionTrailingNewline(const Ctx &ctx, const Expr &e, Shape shape, std::optional<size_t> hang_level);
Var FormatVar(const Ctx &ctx, const Var &v, Shape shape);
FunctionCall FormatFunctionCall(const Ctx &ctx, const FunctionCall &c, Shape shape);
TableConstructor FormatTableConstructor(const Ctx &ctx, const TableConstructor &t, Shape shape);
FunctionBody FormatFunctionBody(const Ctx &ctx, const FunctionBody &b, Shape shape);
bool CanHangExpression(const Expr &e);
bool IsBlockEmpty(const Block &b);
bool IsBlockSimple(const Block &b);
bool ShouldFormatNodeSkip(const Ctx &ctx, const std::vector<Token> &leading);
Ctx CheckToggleFormatting(const Ctx &ctx, const std::vector<Token> &leading);

}  // namespace luafmt::detail
