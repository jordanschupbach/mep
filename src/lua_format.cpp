// mep's Lua formatter: a C++ port of the layout rules of StyLua 2.3.1
// (https://github.com/JohnnyMorganz/StyLua, MPL-2.0) -- its behaviour, not
// its text: the same shapes, hanging decisions, comment placement and
// blank-line rules, re-expressed over mep's own CST (lua_format_cst.h), so
// that with StyLua's default configuration the output matches `stylua -`
// byte for byte. This file holds the token/trivia layer (StyLua's
// general.rs, trivia_util.rs), blocks, statements and assignments;
// lua_format_expr.cpp holds expressions, calls, functions and tables.
#include "lua_format.h"

#include <algorithm>

#include "lua_format_internal.h"

namespace luafmt::detail {

namespace {
thread_local size_t g_work_left = 0;
}  // namespace

void SpendWork() {
    if (g_work_left == 0) throw WorkExhausted{};
    g_work_left--;
}

std::vector<std::string_view> Lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string_view::npos) {
            out.push_back(s.substr(i));
            break;
        }
        std::string_view line = s.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.push_back(line);
        i = e + 1;
    }
    return out;
}

Token Spaces(size_t n) {
    Token t;
    t.type = TT::Whitespace;
    t.text = std::string(n, ' ');
    return t;
}
Token Tabs(size_t n) {
    Token t;
    t.type = TT::Whitespace;
    t.text = std::string(n, '\t');
    return t;
}
Token NewlineTok(const Ctx &) {
    Token t;
    t.type = TT::Whitespace;
    t.text = "\n";
    return t;
}
Token IndentTrivia(const Ctx &ctx, Shape shape) {
    const size_t level = shape.indent.block + shape.indent.additional;
    return ctx.cfg.tabs ? Tabs(level) : Spaces(level * ctx.cfg.indent_width);
}

TokenRef Sym(std::string_view text) {
    TokenRef r;
    size_t a = 0;
    while (a < text.size() && text[a] == ' ') a++;
    size_t b = text.size();
    while (b > a && text[b - 1] == ' ') b--;
    if (a > 0) r.leading.push_back(Spaces(a));
    r.tok.type = TT::Symbol;
    r.tok.text = std::string(text.substr(a, b - a));
    r.tok.start = a;
    r.tok.end = b;
    if (b < text.size()) r.trailing.push_back(Spaces(text.size() - b));
    return r;
}

// ---- general.rs -------------------------------------------------------------------

namespace {

char QuoteToUse(const std::string &lit) {
    size_t singles = 0, doubles = 0;
    for (char c : lit) {
        if (c == '\'') singles++;
        if (c == '"') doubles++;
    }
    if (singles == 0 && doubles == 0) return '"';
    if (singles == doubles) return '"';
    return singles > doubles ? '"' : '\'';
}

size_t Utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

bool NecessaryEscape(std::string_view ch) {
    if (ch.size() != 1) return false;  // any multi-byte character is unnecessary to escape
    const char c = ch[0];
    // UNNECESSARY_ESCAPES = ^[^\n\r"'0-9\\abfnrtuvxz]$
    if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '\\') return true;
    if (c >= '0' && c <= '9') return true;
    return std::string_view("abfnrtuvxz").find(c) != std::string_view::npos;
}

std::string FormatStringLiteral(const std::string &lit, char quote) {
    std::string out;
    out.reserve(lit.size());
    size_t i = 0;
    while (i < lit.size()) {
        const char c = lit[i];
        if (c == '\\' && i + 1 < lit.size() && (lit[i + 1] == '"' || lit[i + 1] == '\'')) {
            const char q = lit[i + 1];
            if (q == quote) out += '\\';
            out += q;
            i += 2;
        } else if (c == '"' || c == '\'') {
            if (c == quote) out += '\\';
            out += c;
            i++;
        } else if (c == '\\' && i + 1 < lit.size()) {
            const size_t n = std::min(Utf8Len(static_cast<unsigned char>(lit[i + 1])), lit.size() - i - 1);
            const std::string_view ch(lit.data() + i + 1, n);
            if (NecessaryEscape(ch)) out += '\\';
            out += ch;
            i += 1 + n;
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

std::string TrimEnd(const std::string &s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' ||
                     s[e - 1] == '\f' || s[e - 1] == '\v'))
        e--;
    return s.substr(0, e);
}

std::string NormalizeNewlines(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out += s[i];
    }
    return out;
}

}  // namespace

FormattedToken FormatToken(const Ctx &ctx, const Token &t, FTT type, Shape shape) {
    FormattedToken r;
    r.tok.type = t.type;
    r.tok.blocks = t.blocks;
    r.tok.quote = t.quote;
    switch (t.type) {
    case TT::Number:
        r.tok.text = (!t.text.empty() && t.text[0] == '.') ? "0" + t.text : t.text;
        break;
    case TT::String:
        if (t.quote == '[') {
            r.tok.text = NormalizeNewlines(t.text);
        } else {
            const char q = QuoteToUse(t.text);
            r.tok.text = FormatStringLiteral(t.text, q);
            r.tok.quote = q;
        }
        break;
    case TT::Shebang:
        r.tok.text = TrimEnd(t.text);
        r.has_trailing = true;
        r.trailing = {NewlineTok(ctx)};
        break;
    case TT::SingleLineComment:
        r.tok.text = TrimEnd(t.text);
        if (type == FTT::Leading) {
            r.has_leading = r.has_trailing = true;
            r.leading = {IndentTrivia(ctx, shape)};
            r.trailing = {NewlineTok(ctx)};
        } else if (type == FTT::Trailing) {
            r.has_leading = true;
            r.leading = {Spaces(1)};
        }
        break;
    case TT::MultiLineComment:
        if (type == FTT::Leading) {
            r.has_leading = r.has_trailing = true;
            r.leading = {IndentTrivia(ctx, shape)};
            r.trailing = {NewlineTok(ctx)};
        }
        r.tok.text = NormalizeNewlines(t.text);
        break;
    default:
        r.tok.text = t.text;
        break;
    }
    return r;
}

std::vector<Token> TriviaToVec(const FormattedToken &ft) {
    std::vector<Token> out = ft.leading;
    out.push_back(ft.tok);
    out.insert(out.end(), ft.trailing.begin(), ft.trailing.end());
    return out;
}

std::vector<Token> LoadTokenTrivia(const Ctx &ctx, const std::vector<Token> &trivia, FTT type, Shape shape) {
    std::vector<Token> out;
    int newlines = 0;
    for (size_t i = 0; i < trivia.size(); i++) {
        const Token &t = trivia[i];
        if (t.type == TT::Whitespace) {
            if (type == FTT::Leading) {
                if (t.text.find('\n') != std::string::npos) {
                    newlines++;
                    if (newlines == 1) out.push_back(NewlineTok(ctx));
                }
            } else if (type == FTT::Trailing) {
                if (i + 1 < trivia.size() && trivia[i + 1].type == TT::MultiLineComment &&
                    t.text.find('\n') == std::string::npos)
                    out.push_back(Spaces(1));
            }
            continue;
        }
        if (t.type == TT::Shebang || t.IsComment()) {
            if (type == FTT::Leading && i + 1 < trivia.size() && trivia[i + 1].type == TT::Whitespace &&
                trivia[i + 1].text.find('\n') != std::string::npos)
                i++;
            newlines = 0;
        } else {
            newlines = 0;
        }
        FormattedToken ft = FormatToken(ctx, t, type, shape);
        out.insert(out.end(), ft.leading.begin(), ft.leading.end());
        out.push_back(ft.tok);
        out.insert(out.end(), ft.trailing.begin(), ft.trailing.end());
    }
    return out;
}

TokenRef FormatTokenRef(const Ctx &ctx, const TokenRef &t, Shape shape) {
    TokenRef r;
    r.leading = LoadTokenTrivia(ctx, t.leading, FTT::Leading, shape);
    r.trailing = LoadTokenTrivia(ctx, t.trailing, FTT::Trailing, shape);
    r.tok = FormatToken(ctx, t.tok, FTT::Token, shape).tok;
    return r;
}

TokenRef FormatSymbol(const Ctx &ctx, const TokenRef &cur, const TokenRef &wanted, Shape shape) {
    TokenRef r;
    r.leading = LoadTokenTrivia(ctx, cur.leading, FTT::Leading, shape);
    std::vector<Token> trail = LoadTokenTrivia(ctx, cur.trailing, FTT::Trailing, shape);
    r.leading.insert(r.leading.end(), wanted.leading.begin(), wanted.leading.end());
    r.trailing = wanted.trailing;
    r.trailing.insert(r.trailing.end(), trail.begin(), trail.end());
    r.tok = wanted.tok;
    return r;
}

TokenRef FormatEndToken(const Ctx &ctx, const TokenRef &t, EndTokenType type, Shape shape) {
    std::vector<Token> lead = LoadTokenTrivia(
        ctx, t.leading, FTT::Leading, type == EndTokenType::IndentComments ? shape.IncrementAdditionalIndent() : shape);
    std::vector<Token> trail = LoadTokenTrivia(ctx, t.trailing, FTT::Trailing, shape);
    // Drop the newlines at the end of the block (walking backwards), until a comment is seen.
    std::vector<Token> kept;
    bool stop = false;
    for (size_t k = lead.size(); k-- > 0;) {
        const Token &x = lead[k];
        if (x.type == TT::Whitespace) {
            const bool next_is_comment = k > 0 && lead[k - 1].IsComment();
            if (!stop && x.text.find('\n') != std::string::npos && !next_is_comment) continue;
            kept.push_back(x);
        } else {
            kept.push_back(x);
            stop = true;
        }
    }
    std::reverse(kept.begin(), kept.end());
    TokenRef r;
    r.leading = std::move(kept);
    r.tok = t.tok;
    r.tok.start = r.tok.end = 0;
    r.trailing = std::move(trail);
    return r;
}

ContainedSpan FormatContainedSpan(const Ctx &ctx, const ContainedSpan &c, Shape shape) {
    return ContainedSpan{FormatTokenRef(ctx, c.open, shape), FormatTokenRef(ctx, c.close, shape)};
}

TokenRef FormatEof(const Ctx &ctx, const TokenRef &eof, Shape shape) {
    if (ShouldFormatNodeSkip(ctx, eof.leading)) return eof;
    std::vector<Token> lead = LoadTokenTrivia(ctx, eof.leading, FTT::Leading, shape);
    const bool only_ws = std::all_of(lead.begin(), lead.end(), [](const Token &t) { return t.type == TT::Whitespace; });
    TokenRef r;
    r.tok.type = TT::Eof;
    if (only_ws) return r;
    while (!lead.empty() && lead.back().type == TT::Whitespace) lead.pop_back();
    lead.push_back(NewlineTok(ctx));
    r.leading = std::move(lead);
    return r;
}

std::vector<Token> PrependNewlineIndentTrivia(const Ctx &ctx, const std::vector<Token> &leading, Shape shape) {
    std::vector<Token> out;
    for (const auto &t : leading)
        if (t.IsComment()) {
            out.push_back(NewlineTok(ctx));
            out.push_back(IndentTrivia(ctx, shape));
            out.push_back(t);
        }
    out.push_back(NewlineTok(ctx));
    out.push_back(IndentTrivia(ctx, shape));
    return out;
}

TokenRef PrependNewlineIndentTok(const Ctx &ctx, const TokenRef &t, Shape shape) {
    return PrependNewlineIndent(ctx, t, shape);
}

// ---- context.rs: stylua: ignore -----------------------------------------------------------

namespace {

std::vector<std::string> CommentLines(const std::vector<Token> &leading) {
    std::vector<std::string> out;
    for (const auto &t : leading) {
        if (!t.IsComment()) continue;
        for (auto l : Lines(t.text)) {
            size_t a = 0, b = l.size();
            while (a < b && (l[a] == ' ' || l[a] == '\t' || l[a] == '\r')) a++;
            while (b > a && (l[b - 1] == ' ' || l[b - 1] == '\t' || l[b - 1] == '\r')) b--;
            out.emplace_back(l.substr(a, b - a));
        }
    }
    return out;
}

}  // namespace

Ctx CheckToggleFormatting(const Ctx &ctx, const std::vector<Token> &leading) {
    Ctx c = ctx;
    for (const auto &l : CommentLines(leading)) {
        if (l == "stylua: ignore start") c.formatting_disabled = true;
        if (l == "stylua: ignore end") c.formatting_disabled = false;
    }
    return c;
}

bool ShouldFormatNodeSkip(const Ctx &ctx, const std::vector<Token> &leading) {
    if (ctx.formatting_disabled) return true;
    for (const auto &l : CommentLines(leading))
        if (l == "stylua: ignore") return true;
    return false;
}

// ---- assignment.rs ---------------------------------------------------------------------

std::optional<size_t> CalculateHangLevel(const Expr &e) {
    switch (e.kind) {
    case EK::Paren:
        return std::nullopt;
    case EK::Unary:
        return CalculateHangLevel(*e.rhs);
    default:
        return 1;
    }
}

Punctuated<Expr> HangPunctuatedList(const Ctx &ctx, const Punctuated<Expr> &p, Shape shape) {
    Punctuated<Expr> out;
    for (size_t idx = 0; idx < p.pairs.size(); idx++) {
        const auto &pr = p.pairs[idx];
        Expr v = HangExpression(ctx, pr.value, shape, CalculateHangLevel(pr.value));
        if (idx != 0) v = UpdLead(v, FT::Append({IndentTrivia(ctx, shape)}));
        std::optional<TokenRef> punct;
        if (pr.punct) {
            TokenRef s = FmtSymbol(ctx, *pr.punct, ",", shape);
            s.trailing.push_back(NewlineTok(ctx));
            punct = std::move(s);
        }
        out.push(std::move(v), std::move(punct));
    }
    return out;
}

TokenRef HangEqualToken(const Ctx &ctx, const TokenRef &eq, Shape shape, bool indent_first_item) {
    std::vector<Token> extra{NewlineTok(ctx)};
    if (indent_first_item) extra.push_back(IndentTrivia(ctx, shape.IncrementAdditionalIndent()));
    std::vector<Token> trail;
    for (const auto &t : eq.trailing)
        if (t.IsComment()) {
            trail.push_back(Spaces(1));
            trail.push_back(t);
        }
    trail.insert(trail.end(), extra.begin(), extra.end());
    TokenRef r = eq;
    r.trailing = std::move(trail);
    return r;
}

namespace {

bool IsComplexFunctionCall(const FunctionCall &c) {
    auto test = [](const FunctionArgs &a) {
        if (a.kind != FunctionArgs::Parens) return false;
        int count = 0;
        for (const auto &pr : a.args->pairs) {
            if (pr.value.kind == EK::Function) return true;
            if (pr.value.kind == EK::Table) count++;
        }
        return count > 1;
    };
    for (const auto &s : c.suffixes)
        if (s.IsCall() && test(s.args)) return true;
    return false;
}

bool PreventEqualsHanging(const Expr &e) {
    if (e.kind == EK::Function) return true;
    if (e.kind == EK::Call) return IsComplexFunctionCall(*e.call);
    return false;
}

std::vector<Token> IndentedComments(const Ctx &ctx, const std::vector<Token> &comments, Shape shape) {
    std::vector<Token> out;
    for (const auto &c : comments) {
        out.push_back(IndentTrivia(ctx, shape));
        out.push_back(c);
        out.push_back(NewlineTok(ctx));
    }
    out.push_back(IndentTrivia(ctx, shape));
    return out;
}

}  // namespace

std::pair<TokenRef, Expr> HangAtEqualsDueToComments(const Ctx &ctx, const TokenRef &eq, const Expr &e, Shape shape) {
    TokenRef equal = HangEqualToken(ctx, eq, shape, false);
    const Shape sh = shape.Reset().IncrementAdditionalIndent();
    Expr ex = HasInlineComments(StripTrivia(e)) ? HangExpression(ctx, e, sh, std::nullopt) : FormatExpression(ctx, e, sh);
    auto [stripped, comments] = TakeLeadingComments(ex);
    ex = UpdLead(stripped, FT::Replace(IndentedComments(ctx, comments, sh)));
    return {std::move(equal), std::move(ex)};
}

namespace {

std::pair<Punctuated<Expr>, TokenRef> AttemptAssignmentTactics(const Ctx &ctx, const Punctuated<Expr> &exprs,
                                                               Shape shape, const TokenRef &equal_token) {
    auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatExpression(c, e, s); };
    if (exprs.size() > 1) {
        TokenRef hanging_equal = HangEqualToken(ctx, equal_token, shape, true);
        const Shape hanging_shape = shape.Reset().IncrementAdditionalIndent();
        Punctuated<Expr> expr_list = FormatPunctuated(ctx, exprs, hanging_shape.WithInfiniteWidth(), fmt);
        if (PunctuatedInlineComments(exprs, true) ||
            hanging_shape.TakeFirstLine(Print(StripTrivia(expr_list))).OverBudget()) {
            Punctuated<Expr> multi = FormatPunctuatedMultiline(ctx, exprs, hanging_shape, fmt, std::nullopt);
            Punctuated<Expr> out;
            for (size_t idx = 0; idx < multi.pairs.size(); idx++) {
                auto &formatted = multi.pairs[idx];
                const Expr &original = exprs.pairs[idx].value;
                const Shape sh = hanging_shape.Reset();
                if (HasInlineComments(formatted.value) ||
                    sh.TakeFirstLine(Print(StripLead(formatted.value))).OverBudget()) {
                    Expr e = HangExpression(ctx, original, sh, CalculateHangLevel(original));
                    if (idx != 0) e = PrependNewlineIndent(ctx, e, sh);
                    out.push(std::move(e), std::move(formatted.punct));
                } else {
                    out.pairs.push_back(std::move(formatted));
                }
            }
            return {std::move(out), std::move(hanging_equal)};
        }
        return {std::move(expr_list), std::move(hanging_equal)};
    }
    const Expr &expression = exprs.pairs.front().value;
    if (TokenContainsComments(equal_token) || HasLeadingComments(expression, CS::Single)) {
        auto [eq, ex] = HangAtEqualsDueToComments(ctx, equal_token, expression, shape);
        Punctuated<Expr> l;
        l.push(std::move(ex));
        return {std::move(l), std::move(eq)};
    }
    Punctuated<Expr> expr_list = FormatPunctuated(ctx, exprs, shape, fmt);
    const Shape formatting_shape = shape.TakeFirstLine(Print(StripTrail(expr_list)));
    if (CanHangExpression(expression)) {
        Punctuated<Expr> hanging = HangPunctuatedList(ctx, exprs, shape);
        const Shape hanging_shape = shape.TakeFirstLine(Print(StripTrivia(hanging)));
        if (HasInlineComments(expression) || hanging_shape.UsedWidth() < formatting_shape.UsedWidth())
            return {std::move(hanging), equal_token};
        return {std::move(expr_list), equal_token};
    }
    if (PreventEqualsHanging(expression)) return {std::move(expr_list), equal_token};
    TokenRef hanging_equal = HangEqualToken(ctx, equal_token, shape, true);
    Shape eq_shape = shape.Reset().IncrementAdditionalIndent();
    Punctuated<Expr> hanging_list = FormatPunctuated(ctx, exprs, eq_shape, fmt);
    eq_shape = eq_shape.TakeFirstLine(Print(StripTrail(hanging_list)));
    if ((!eq_shape.OverBudget() && Lines(Print(hanging_list)).size() + 1 < Lines(Print(expr_list)).size()) ||
        formatting_shape.OverBudget())
        return {std::move(hanging_list), std::move(hanging_equal)};
    return {std::move(expr_list), equal_token};
}

Stmt FormatAssignmentNoTrivia(const Ctx &ctx, const Stmt &a, Shape shape) {
    auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatExpression(c, e, s); };
    auto fmt_var = [](const Ctx &c, const Var &v, Shape s) { return FormatVar(c, v, s); };
    const bool contains_comments = TokenContainsComments(*a.equal) || PunctuatedInlineComments(a.exprs, true);
    Punctuated<Var> var_list = TryFormatPunctuated(ctx, a.vars, shape.WithInfiniteWidth(), fmt_var, kHang1);
    TokenRef equal = FmtSymbol(ctx, *a.equal, " = ", shape);
    Punctuated<Expr> expr_list = FormatPunctuated(ctx, a.exprs, shape.WithInfiniteWidth(), fmt);
    if (HasTrailingComments(var_list, CS::Single)) {
        shape = shape.Reset().IncrementAdditionalIndent().Add(2);
        equal = PrependNewlineIndent(ctx, equal, shape);
    }
    const Shape singleline =
        shape + (Print(StripLead(var_list)).size() + 3 + Print(StripTrail(expr_list)).size());
    if (contains_comments || singleline.OverBudget()) {
        var_list = TryFormatPunctuated(ctx, a.vars, shape, fmt_var, kHang1);
        const Shape sh = shape + (Print(StripLead(var_list)).size() + 3);
        auto [el, eq] = AttemptAssignmentTactics(ctx, a.exprs, sh, equal);
        expr_list = std::move(el);
        equal = std::move(eq);
    }
    Stmt s;
    s.kind = SK::Assignment;
    s.vars = std::move(var_list);
    s.equal = std::move(equal);
    s.exprs = std::move(expr_list);
    return s;
}

Attribute FormatAttribute(const Ctx &ctx, const Attribute &a, Shape shape) {
    Attribute r;
    r.brackets = FormatContainedSpan(ctx, a.brackets, shape);
    r.brackets.open.leading.push_back(Spaces(1));
    r.name = FormatTokenRef(ctx, a.name, shape);
    return r;
}

Stmt FormatLocalAssignmentNoTrivia(const Ctx &ctx, const Stmt &a, Shape shape) {
    auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatExpression(c, e, s); };
    auto fmt_tok = [](const Ctx &c, const TokenRef &t, Shape s) { return FormatTokenRef(c, t, s); };
    Stmt s;
    s.kind = SK::Local;
    if (a.exprs.empty()) {
        s.local_token = FmtSymbol(ctx, a.local_token, "local ", shape);
        const Shape sh = shape + 6;
        s.names = TryFormatPunctuated(ctx, a.names, sh, fmt_tok, kHang1);
        for (const auto &at : a.attributes)
            s.attributes.push_back(at ? std::optional<Attribute>(FormatAttribute(ctx, *at, sh)) : std::nullopt);
        return s;
    }
    const bool contains_comments =
        (a.equal && TokenContainsComments(*a.equal)) || PunctuatedInlineComments(a.exprs, true);
    s.local_token = FmtSymbol(ctx, a.local_token, "local ", shape);
    Punctuated<TokenRef> names = TryFormatPunctuated(ctx, a.names, shape.WithInfiniteWidth(), fmt_tok, kHang1);
    TokenRef equal = FmtSymbol(ctx, *a.equal, " = ", shape);
    Punctuated<Expr> expr_list = FormatPunctuated(ctx, a.exprs, shape.WithInfiniteWidth(), fmt);
    size_t attr_len = 0;
    for (const auto &at : a.attributes) {
        s.attributes.push_back(at ? std::optional<Attribute>(FormatAttribute(ctx, *at, shape)) : std::nullopt);
        if (s.attributes.back()) attr_len += Print(*s.attributes.back()).size();
    }
    if (HasTrailingComments(names, CS::Single)) {
        shape = shape.Reset().IncrementAdditionalIndent().Add(2);
        equal = PrependNewlineIndent(ctx, equal, shape);
    }
    const Shape singleline =
        shape + (Print(StripLead(names)).size() + 6 + 3 + attr_len + Print(StripTrail(expr_list)).size());
    if (contains_comments || singleline.OverBudget()) {
        names = TryFormatPunctuated(ctx, a.names, shape, fmt_tok, kHang1);
        const Shape sh = shape + (Print(StripLead(names)).size() + 6 + 3 + attr_len);
        auto [el, eq] = AttemptAssignmentTactics(ctx, a.exprs, sh, equal);
        expr_list = std::move(el);
        equal = std::move(eq);
    }
    s.names = std::move(names);
    s.equal = std::move(equal);
    s.exprs = std::move(expr_list);
    return s;
}

}  // namespace

// ---- stmt.rs --------------------------------------------------------------------

Expr RemoveConditionParentheses(const Expr &e) {
    if (e.kind != EK::Paren) return e;
    std::vector<Token> comments = TrailingComments(e);
    return UpdTrail(*e.rhs, FT::Append(std::move(comments)));
}

namespace {

std::vector<Token> One(Token t) { return std::vector<Token>{std::move(t)}; }

Stmt FormatDoBlock(const Ctx &ctx, const Stmt &d, Shape shape) {
    Stmt s;
    s.kind = SK::Do;
    s.first = FmtSymbol(ctx, d.first, "do", shape);
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    s.first.trailing.push_back(NewlineTok(ctx));
    s.block = FormatBlock(ctx, *d.block, shape.Reset().IncrementBlockIndent());
    s.end_token = FormatEndToken(ctx, d.end_token, EndTokenType::IndentComments, shape);
    s.end_token.leading.push_back(IndentTrivia(ctx, shape));
    s.end_token.trailing.push_back(NewlineTok(ctx));
    return s;
}

bool HugGenericFor(const Punctuated<Expr> &exprs) {
    if (exprs.size() != 1) return false;
    const Expr &e = exprs.pairs.front().value;
    if (HasLeadingComments(e, CS::All) || HasTrailingComments(e, CS::All)) return false;
    if (e.kind != EK::Call) return false;
    const auto &sfx = e.call->suffixes;
    if (sfx.size() != 1) return false;
    const Suffix &s = sfx.front();
    if (s.kind != Suffix::kAnon) return false;
    if (s.args.kind == FunctionArgs::Table) return true;
    if (s.args.kind == FunctionArgs::Parens)
        return s.args.args->size() == 1 && s.args.args->pairs.front().value.kind == EK::Table;
    return false;
}

Stmt FormatGenericFor(const Ctx &ctx, const Stmt &g, Shape shape) {
    auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatExpression(c, e, s); };
    auto fmt_tok = [](const Ctx &c, const TokenRef &t, Shape s) { return FormatTokenRef(c, t, s); };
    const std::vector<Token> leading{IndentTrivia(ctx, shape)};
    TokenRef for_token = FmtSymbol(ctx, g.first, "for ", shape);
    for_token.leading.insert(for_token.leading.end(), leading.begin(), leading.end());
    shape = shape + 4;
    Punctuated<TokenRef> singleline_names = FormatPunctuated(ctx, g.names, shape, fmt_tok);
    const Shape singleline_shape = shape.TakeFirstLine(Print(singleline_names));
    const bool names_multiline =
        ContainsComments(g.names) || SpansMultipleLines(singleline_names) || singleline_shape.OverBudget();
    if (names_multiline) {
        for_token = FmtSymbol(ctx, g.first, "for", shape);
        for_token.leading.insert(for_token.leading.end(), leading.begin(), leading.end());
    }
    Punctuated<TokenRef> names;
    if (names_multiline) {
        const Shape sh = shape.Reset().IncrementAdditionalIndent();
        names = FormatPunctuatedMultiline(ctx, g.names, sh, fmt_tok, std::nullopt);
        names = UpdLead(names, FT::Append({NewlineTok(ctx), IndentTrivia(ctx, sh)}));
    } else {
        names = std::move(singleline_names);
    }
    shape = names_multiline ? shape.Reset() + 3 : singleline_shape + 4;
    Punctuated<Expr> singleline_expr = FormatPunctuated(ctx, g.exprs, shape, fmt);
    const Shape singleline_expr_shape = shape.TakeFirstLine(Print(singleline_expr));
    const bool expr_multiline = (HasTrailingComments(g.in_token, CS::All) || ContainsComments(g.exprs) ||
                                 SpansMultipleLines(singleline_expr) || singleline_expr_shape.OverBudget()) &&
                                !HugGenericFor(g.exprs);
    TokenRef in_token;
    if (names_multiline) {
        in_token = FmtSymbol(ctx, g.in_token, expr_multiline ? "in" : "in ", shape);
        in_token.leading.push_back(NewlineTok(ctx));
        in_token.leading.push_back(IndentTrivia(ctx, shape));
    } else {
        in_token = FmtSymbol(ctx, g.in_token, expr_multiline ? " in" : " in ", shape);
    }
    Punctuated<Expr> expr_list;
    if (expr_multiline) {
        const Shape sh = shape.Reset().IncrementAdditionalIndent();
        expr_list = FormatPunctuatedMultiline(ctx, g.exprs, sh, fmt, std::nullopt);
        expr_list = PrependNewlineIndent(ctx, expr_list, sh);
    } else {
        expr_list = std::move(singleline_expr);
    }
    TokenRef do_token;
    if (expr_multiline) {
        do_token = FmtSymbol(ctx, g.do_token, "do", shape);
        do_token.leading.push_back(NewlineTok(ctx));
        do_token.leading.push_back(IndentTrivia(ctx, shape));
    } else {
        do_token = FmtSymbol(ctx, g.do_token, " do", shape);
    }
    do_token.trailing.push_back(NewlineTok(ctx));
    Stmt s;
    s.kind = SK::GenericFor;
    s.first = std::move(for_token);
    s.names = std::move(names);
    s.in_token = std::move(in_token);
    s.exprs = std::move(expr_list);
    s.do_token = std::move(do_token);
    s.block = FormatBlock(ctx, *g.block, shape.Reset().IncrementBlockIndent());
    s.end_token = FormatEndToken(ctx, g.end_token, EndTokenType::IndentComments, shape);
    s.end_token.leading.insert(s.end_token.leading.end(), leading.begin(), leading.end());
    s.end_token.trailing.push_back(NewlineTok(ctx));
    return s;
}

bool ShouldIndentFurther(const std::vector<Token> &trivia, Shape shape) {
    const size_t current = shape.indent.block + shape.indent.additional;
    for (size_t i = 0; i + 1 < trivia.size(); i++) {
        const Token &t = trivia[i];
        if (t.type != TT::Whitespace || !trivia[i + 1].IsComment()) continue;
        std::string last;
        for (size_t k = t.text.size(); k-- > 0;) {
            if (t.text[k] == '\n' || t.text[k] == '\r') break;
            last += t.text[k];
        }
        size_t level = 0;
        if (last.find('\t') != std::string::npos) {
            level = static_cast<size_t>(std::count(last.begin(), last.end(), '\t'));
        } else {
            level = static_cast<size_t>(std::count(last.begin(), last.end(), ' ')) / shape.indent.width;
        }
        if (level > current) return true;
    }
    return false;
}

ElseIf FormatElseIf(const Ctx &ctx, const ElseIf &ei, Shape shape) {
    shape = shape.Reset();
    const Expr condition = RemoveConditionParentheses(ei.cond);
    const EndTokenType ett = ShouldIndentFurther(ei.elseif_token.leading, shape) ? EndTokenType::IndentComments
                                                                                   : EndTokenType::InlineComments;
    TokenRef elseif_token = FormatEndToken(ctx, ei.elseif_token, ett, shape);
    Expr singleline_condition = FormatExpression(ctx, condition, shape + 7);
    TokenRef singleline_then = FmtSymbol(ctx, ei.then_token, " then", shape);
    const Shape singleline_shape = shape + (7 + 5 + Print(StripTrivia(singleline_condition)).size());
    const bool multiline = singleline_shape.OverBudget() || HasTrailingComments(ei.elseif_token, CS::All) ||
                           HasLeadingComments(ei.then_token, CS::All) || ContainsComments(condition);
    elseif_token.trailing.push_back(multiline ? NewlineTok(ctx) : Spaces(1));
    elseif_token.leading.push_back(IndentTrivia(ctx, shape));
    ElseIf r;
    r.elseif_token = std::move(elseif_token);
    if (multiline) {
        const Shape sh = shape.Reset().IncrementAdditionalIndent();
        r.cond = UpdLead(HangExpressionTrailingNewline(ctx, condition, sh, std::nullopt),
                         FT::Append({IndentTrivia(ctx, sh)}));
        r.then_token = FormatEndToken(ctx, ei.then_token, EndTokenType::IndentComments, shape);
        r.then_token.leading.push_back(IndentTrivia(ctx, shape));
    } else {
        r.cond = std::move(singleline_condition);
        r.then_token = std::move(singleline_then);
    }
    r.then_token.trailing.push_back(NewlineTok(ctx));
    r.block = FormatBlock(ctx, ei.block, shape.Reset().IncrementBlockIndent());
    return r;
}

Stmt FormatIf(const Ctx &ctx, const Stmt &n, Shape shape) {
    const Expr condition = RemoveConditionParentheses(*n.cond);
    TokenRef singleline_if = FmtSymbol(ctx, n.first, "if ", shape);
    Expr singleline_condition = FormatExpression(ctx, condition, shape + 3 + 5);
    TokenRef singleline_then = FmtSymbol(ctx, n.do_token, " then", shape);
    const Shape singleline_shape = shape + (3 + 5 + Print(StripTrivia(singleline_condition)).size());
    const bool multiline = singleline_shape.OverBudget() || HasTrailingComments(n.first, CS::All) ||
                           HasLeadingComments(n.do_token, CS::All) || ContainsComments(condition);
    Stmt s;
    s.kind = SK::If;
    if (multiline) {
        s.first = FmtSymbol(ctx, n.first, "if", shape);
        s.first.trailing.push_back(NewlineTok(ctx));
    } else {
        s.first = std::move(singleline_if);
    }
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    if (multiline) {
        const Shape sh = shape.Reset().IncrementAdditionalIndent();
        s.cond = UpdLead(HangExpressionTrailingNewline(ctx, condition, sh, std::nullopt),
                         FT::Append({IndentTrivia(ctx, sh)}));
        s.do_token = FormatEndToken(ctx, n.do_token, EndTokenType::IndentComments, shape);
        s.do_token.leading.push_back(IndentTrivia(ctx, shape));
    } else {
        s.cond = std::move(singleline_condition);
        s.do_token = std::move(singleline_then);
    }
    s.do_token.trailing.push_back(NewlineTok(ctx));
    s.block = FormatBlock(ctx, *n.block, shape.Reset().IncrementBlockIndent());
    s.end_token = FormatEndToken(ctx, n.end_token, EndTokenType::IndentComments, shape);
    s.end_token.leading.push_back(IndentTrivia(ctx, shape));
    s.end_token.trailing.push_back(NewlineTok(ctx));
    for (const auto &ei : n.elseifs) s.elseifs.push_back(FormatElseIf(ctx, ei, shape));
    if (n.else_token) {
        const EndTokenType ett = ShouldIndentFurther(n.else_token->leading, shape) ? EndTokenType::IndentComments
                                                                                    : EndTokenType::InlineComments;
        TokenRef et = FormatEndToken(ctx, *n.else_token, ett, shape);
        et.leading.push_back(IndentTrivia(ctx, shape));
        et.trailing.push_back(NewlineTok(ctx));
        s.else_token = std::move(et);
        s.else_block = FormatBlock(ctx, *n.else_block, shape.Reset().IncrementBlockIndent());
    }
    return s;
}

Stmt FormatNumericFor(const Ctx &ctx, const Stmt &n, Shape shape) {
    Stmt s;
    s.kind = SK::NumericFor;
    s.first = FmtSymbol(ctx, n.first, "for ", shape);
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    s.index_var = FormatTokenRef(ctx, n.index_var, shape);
    s.equal = FmtSymbol(ctx, *n.equal, " = ", shape);
    s.cond = FormatExpression(ctx, *n.cond, shape);
    s.comma1 = FmtSymbol(ctx, n.comma1, ", ", shape);
    s.limit = FormatExpression(ctx, *n.limit, shape);
    if (n.comma2) {
        s.comma2 = FmtSymbol(ctx, *n.comma2, ", ", shape);
        s.step = FormatExpression(ctx, *n.step, shape);
    }
    s.do_token = FmtSymbol(ctx, n.do_token, " do", shape);
    s.do_token.trailing.push_back(NewlineTok(ctx));
    s.block = FormatBlock(ctx, *n.block, shape.Reset().IncrementBlockIndent());
    s.end_token = FormatEndToken(ctx, n.end_token, EndTokenType::IndentComments, shape);
    s.end_token.leading.push_back(IndentTrivia(ctx, shape));
    s.end_token.trailing.push_back(NewlineTok(ctx));
    return s;
}

Stmt FormatRepeat(const Ctx &ctx, const Stmt &n, Shape shape) {
    Stmt s;
    s.kind = SK::Repeat;
    s.first = FmtSymbol(ctx, n.first, "repeat", shape);
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    s.first.trailing.push_back(NewlineTok(ctx));
    s.block = FormatBlock(ctx, *n.block, shape.Reset().IncrementBlockIndent());
    s.end_token = FmtSymbol(ctx, n.end_token, "until ", shape);
    s.end_token.leading.push_back(IndentTrivia(ctx, shape));
    const Expr condition = RemoveConditionParentheses(*n.cond);
    const Shape singleline_shape = shape + (6 + Print(StripTrivia(condition)).size());
    const bool multiline = singleline_shape.OverBudget() || HasInlineComments(condition);
    shape = shape + 6;
    if (multiline) {
        s.cond = HangExpressionTrailingNewline(ctx, condition, shape.IncrementAdditionalIndent(), std::nullopt);
    } else {
        s.cond = UpdTrail(FormatExpression(ctx, condition, shape), FT::Append(One(NewlineTok(ctx))));
    }
    return s;
}

Stmt FormatWhile(const Ctx &ctx, const Stmt &n, Shape shape) {
    const Expr condition = RemoveConditionParentheses(*n.cond);
    TokenRef singleline_while = FmtSymbol(ctx, n.first, "while ", shape);
    Expr singleline_condition = FormatExpression(ctx, condition, shape + 6);
    TokenRef singleline_do = FmtSymbol(ctx, n.do_token, " do", shape);
    const Shape singleline_shape = shape + (6 + 3 + Print(StripTrivia(singleline_condition)).size());
    const bool multiline = singleline_shape.OverBudget() || HasTrailingComments(n.first, CS::All) ||
                           HasLeadingComments(n.do_token, CS::All) || ContainsComments(condition);
    Stmt s;
    s.kind = SK::While;
    if (multiline) {
        s.first = FmtSymbol(ctx, n.first, "while", shape);
        s.first.trailing.push_back(NewlineTok(ctx));
    } else {
        s.first = std::move(singleline_while);
    }
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    if (multiline) {
        const Shape sh = shape.Reset().IncrementAdditionalIndent();
        s.cond = UpdLead(HangExpressionTrailingNewline(ctx, condition, sh, std::nullopt),
                         FT::Append({IndentTrivia(ctx, sh)}));
        s.do_token = FormatEndToken(ctx, n.do_token, EndTokenType::IndentComments, shape);
        s.do_token.leading.push_back(IndentTrivia(ctx, shape));
    } else {
        s.cond = std::move(singleline_condition);
        s.do_token = std::move(singleline_do);
    }
    s.do_token.trailing.push_back(NewlineTok(ctx));
    s.block = FormatBlock(ctx, *n.block, shape.Reset().IncrementBlockIndent());
    s.end_token = FormatEndToken(ctx, n.end_token, EndTokenType::IndentComments, shape);
    s.end_token.leading.push_back(IndentTrivia(ctx, shape));
    s.end_token.trailing.push_back(NewlineTok(ctx));
    return s;
}

FunctionName FormatFunctionName(const Ctx &ctx, const FunctionName &n, Shape shape) {
    FunctionName r;
    for (const auto &pr : n.names.pairs) {
        if (pr.punct)
            r.names.push(FormatTokenRef(ctx, pr.value, shape), FmtSymbol(ctx, *pr.punct, ".", shape));
        else
            r.names.push(FormatTokenRef(ctx, pr.value, shape));
    }
    if (n.method)
        r.method = std::make_pair(FmtSymbol(ctx, n.method->first, ":", shape), FormatTokenRef(ctx, n.method->second, shape));
    return r;
}

Stmt FormatFunctionDeclaration(const Ctx &ctx, const Stmt &n, Shape shape) {
    Stmt s;
    s.kind = SK::FunctionDecl;
    s.first = FmtSymbol(ctx, n.first, "function ", shape);
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    // SpaceAfterFunctionNames::Never: an empty whitespace token after the name.
    s.fname = UpdTrail(FormatFunctionName(ctx, n.fname, shape), FT::Append(One(Spaces(0))));
    const Shape sh = shape + (9 + Print(StripTrivia(s.fname)).size());
    s.body = UpdTrail(FormatFunctionBody(ctx, *n.body, sh), FT::Append(One(NewlineTok(ctx))));
    return s;
}

Stmt FormatLocalFunction(const Ctx &ctx, const Stmt &n, Shape shape) {
    Stmt s;
    s.kind = SK::LocalFunction;
    s.local_token = FmtSymbol(ctx, n.local_token, "local ", shape);
    s.local_token.leading.push_back(IndentTrivia(ctx, shape));
    s.function_token = FmtSymbol(ctx, n.function_token, "function ", shape);
    s.name = FormatTokenRef(ctx, n.name, shape);
    s.name.trailing.push_back(Spaces(0));
    const Shape sh = shape + (6 + 9 + Print(StripTrivia(s.name)).size());
    s.body = UpdTrail(FormatFunctionBody(ctx, *n.body, sh), FT::Append(One(NewlineTok(ctx))));
    return s;
}

Stmt FormatGoto(const Ctx &ctx, const Stmt &n, Shape shape) {
    Stmt s;
    s.kind = SK::Goto;
    s.first = FmtSymbol(ctx, n.first, "goto ", shape);
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    s.name = FormatTokenRef(ctx, n.name, shape);
    s.name.trailing.push_back(NewlineTok(ctx));
    return s;
}

Stmt FormatLabel(const Ctx &ctx, const Stmt &n, Shape shape) {
    Stmt s;
    s.kind = SK::Label;
    s.first = FmtSymbol(ctx, n.first, "::", shape);
    s.first.leading.push_back(IndentTrivia(ctx, shape));
    s.name = FormatTokenRef(ctx, n.name, shape);
    s.end_token = FmtSymbol(ctx, n.end_token, "::", shape);
    s.end_token.trailing.push_back(NewlineTok(ctx));
    return s;
}

Stmt FormatStmt(const Ctx &ctx, const Stmt &st, Shape shape) {
    if (ShouldFormatNodeSkip(ctx, LeadTrivia(st))) return st;
    const FT lead = FT::Append({IndentTrivia(ctx, shape)});
    const FT trail = FT::Append({NewlineTok(ctx)});
    switch (st.kind) {
    case SK::Assignment:
        return UpdTrivia(FormatAssignmentNoTrivia(ctx, st, shape), lead, trail);
    case SK::Local:
        return UpdTrivia(FormatLocalAssignmentNoTrivia(ctx, st, shape), lead, trail);
    case SK::Call: {
        Stmt s;
        s.kind = SK::Call;
        s.call = UpdTrivia(FormatFunctionCall(ctx, *st.call, shape), lead, trail);
        return s;
    }
    case SK::Do:
        return FormatDoBlock(ctx, st, shape);
    case SK::GenericFor:
        return FormatGenericFor(ctx, st, shape);
    case SK::If:
        return FormatIf(ctx, st, shape);
    case SK::NumericFor:
        return FormatNumericFor(ctx, st, shape);
    case SK::Repeat:
        return FormatRepeat(ctx, st, shape);
    case SK::While:
        return FormatWhile(ctx, st, shape);
    case SK::FunctionDecl:
        return FormatFunctionDeclaration(ctx, st, shape);
    case SK::LocalFunction:
        return FormatLocalFunction(ctx, st, shape);
    case SK::Goto:
        return FormatGoto(ctx, st, shape);
    case SK::Label:
        return FormatLabel(ctx, st, shape);
    }
    return st;
}

// ---- block.rs ----------------------------------------------------------------------

bool IsFunctionOrTable(const Expr &e) { return e.kind == EK::Table || e.kind == EK::Function; }

LastStmt FormatReturn(const Ctx &ctx, const LastStmt &r, Shape shape) {
    auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatExpression(c, e, s); };
    LastStmt out;
    if (r.returns.empty()) {
        out.token = FmtSymbol(ctx, r.token, "return", shape);
        return out;
    }
    TokenRef token = FmtSymbol(ctx, r.token, "return ", shape);
    shape = shape + 7;
    const Punctuated<Expr> &returns = r.returns;
    const bool token_trailing_comments = HasTrailingComments(r.token, CS::Single);
    const bool contains_comments = token_trailing_comments || PunctuatedInlineComments(returns, true);
    bool is_function_or_table = true;
    for (const auto &pr : returns.pairs) is_function_or_table = is_function_or_table && IsFunctionOrTable(pr.value);
    bool should_multiline = false;
    Punctuated<Expr> singleline_returns;
    if (contains_comments) {
        should_multiline = true;
    } else if (is_function_or_table) {
        singleline_returns = FormatPunctuated(ctx, returns, shape, fmt);
    } else {
        singleline_returns = FormatPunctuated(ctx, returns, shape.WithInfiniteWidth(), fmt);
        should_multiline = (shape + Print(StripTrail(singleline_returns)).size()).OverBudget();
    }
    const bool comment_between = token_trailing_comments || HasLeadingComments(returns.pairs.front().value, CS::Single);
    if (comment_between) token = HangEqualToken(ctx, FmtSymbol(ctx, r.token, "return", shape), shape, false);
    Punctuated<Expr> formatted;
    if (should_multiline) {
        if (returns.size() > 1 || comment_between) {
            const std::optional<size_t> hang_level = is_function_or_table ? std::nullopt : kHang1;
            Punctuated<Expr> multi = FormatPunctuatedMultiline(ctx, returns, shape, fmt, hang_level);
            for (size_t idx = 0; idx < multi.pairs.size(); idx++) {
                auto pr = std::move(multi.pairs[idx]);
                const Expr &original = returns.pairs[idx].value;
                Shape sh = shape;
                if (!(idx == 0 && !comment_between)) {
                    sh = shape.Reset();
                    if (hang_level) sh = sh.WithIndent(sh.AddIndentLevel(*hang_level).indent);
                }
                if (HasInlineComments(pr.value) || sh.TakeFirstLine(Print(StripLead(pr.value))).OverBudget()) {
                    Expr e = HangExpression(ctx, original, sh, kHang1);
                    if (idx != 0) e = PrependNewlineIndent(ctx, e, sh);
                    pr.value = std::move(e);
                }
                if (comment_between && idx == 0) {
                    auto [first, comments] = TakeLeadingComments(pr.value);
                    pr.value = UpdLead(first, FT::Replace(IndentedComments(ctx, comments, sh)));
                }
                formatted.pairs.push_back(std::move(pr));
            }
        } else {
            Punctuated<Expr> hanging = HangPunctuatedList(ctx, returns, shape);
            const Shape hanging_shape = shape.TakeFirstLine(Print(StripTrivia(hanging)));
            Punctuated<Expr> normal = FormatPunctuated(ctx, returns, shape, fmt);
            const Shape normal_shape = shape.TakeFirstLine(Print(StripTrail(normal)));
            formatted = hanging_shape.UsedWidth() < normal_shape.UsedWidth() ? std::move(hanging) : std::move(normal);
        }
    } else {
        formatted = std::move(singleline_returns);
    }
    out.token = std::move(token);
    out.returns = std::move(formatted);
    return out;
}

bool ExprEndsWithIdentOrParens(const Expr &e) {
    switch (e.kind) {
    case EK::Paren:
    case EK::Call:
    case EK::Var:
        return true;
    case EK::Binary:
    case EK::Unary:
        return ExprEndsWithIdentOrParens(*e.rhs);
    default:
        return false;
    }
}

bool StmtEndsWithIdentOrParens(const Stmt &s) {
    switch (s.kind) {
    case SK::Assignment:
    case SK::Local:
        return !s.exprs.empty() && ExprEndsWithIdentOrParens(s.exprs.pairs.back().value);
    case SK::Call:
        return true;
    case SK::Repeat:
        return ExprEndsWithIdentOrParens(*s.cond);
    default:
        return false;
    }
}

bool VarHasParens(const Var &v) { return !v.is_name && !v.expr->prefix.is_name; }

bool RequiresSemicolon(const Stmt &s, const Stmt *next) {
    if (!StmtEndsWithIdentOrParens(s) || !next) return false;
    if (next->kind == SK::Call) return !next->call->prefix.is_name;
    if (next->kind == SK::Assignment) return !next->vars.empty() && VarHasParens(next->vars.pairs.front().value);
    return false;
}

void RemoveLeadingNewlines(std::vector<Token> &lead) {
    size_t k = 0;
    while (k < lead.size() && TriviaIsNewline(lead[k])) k++;
    lead.erase(lead.begin(), lead.begin() + static_cast<std::ptrdiff_t>(k));
}

// The trivia after a statement's last token, without the final newline, then
// the semicolon's comments, then a newline.
std::vector<Token> SemicolonTrivia(const Ctx &ctx, std::vector<Token> trailing, const TokenRef &semi) {
    if (!trailing.empty()) trailing.pop_back();
    for (const auto *v : {&semi.leading, &semi.trailing})
        for (const auto &t : *v)
            if (t.IsComment()) {
                trailing.push_back(Spaces(1));
                trailing.push_back(t);
            }
    trailing.push_back(NewlineTok(ctx));
    return trailing;
}

}  // namespace

LastStmt FormatLastStmtNoTrivia(const Ctx &ctx, const LastStmt &s, Shape shape) {
    if (s.is_break) {
        LastStmt r;
        r.is_break = true;
        r.token = FmtSymbol(ctx, s.token, "break", shape);
        return r;
    }
    return FormatReturn(ctx, s, shape);
}

Block FormatBlock(const Ctx &ctx_in, const Block &block, Shape shape_in) {
    SpendWork();
    Ctx ctx = ctx_in;
    Block out;
    bool remove_newlines = true;
    for (size_t i = 0; i < block.stmts.size(); i++) {
        const StmtSemi &ss = block.stmts[i];
        ctx = CheckToggleFormatting(ctx, LeadTrivia(ss.stmt));
        const Shape shape = shape_in.Reset();
        Stmt stmt = FormatStmt(ctx, ss.stmt, shape);
        if (remove_newlines) {
            if (!ShouldFormatNodeSkip(ctx, LeadTrivia(stmt))) {
                if (TokenRef *t = FirstTok(stmt)) RemoveLeadingNewlines(t->leading);
            }
            remove_newlines = false;
        }
        const Stmt *next = i + 1 < block.stmts.size() ? &block.stmts[i + 1].stmt : nullptr;
        std::optional<TokenRef> semicolon;
        if (RequiresSemicolon(stmt, next)) {
            auto [updated, trivia] = TakeTrailingTrivia(stmt);
            stmt = std::move(updated);
            TokenRef semi = ss.semi ? FmtSymbol(ctx, *ss.semi, ";", shape) : Sym(";");
            semi.trailing.insert(semi.trailing.end(), trivia.begin(), trivia.end());
            semicolon = std::move(semi);
        } else if (ss.semi) {
            stmt = UpdTrail(stmt, FT::Replace(SemicolonTrivia(ctx, TrailTrivia(stmt), *ss.semi)));
        }
        out.stmts.push_back(StmtSemi{std::move(stmt), std::move(semicolon)});
    }
    if (block.last) {
        const auto &[ls, semi] = *block.last;
        ctx = CheckToggleFormatting(ctx, LeadTrivia(ls));
        const Shape shape = shape_in.Reset();
        LastStmt last;
        if (ShouldFormatNodeSkip(ctx, LeadTrivia(ls))) {
            last = ls;
        } else {
            last = UpdTrivia(FormatLastStmtNoTrivia(ctx, ls, shape), FT::Append({IndentTrivia(ctx, shape)}),
                             FT::Append({NewlineTok(ctx)}));
            if (remove_newlines && !ShouldFormatNodeSkip(ctx, LeadTrivia(last))) RemoveLeadingNewlines(last.token.leading);
        }
        if (semi) last = UpdTrail(last, FT::Replace(SemicolonTrivia(ctx, TrailTrivia(last), *semi)));
        out.last = std::pair<LastStmt, std::optional<TokenRef>>(std::move(last), std::nullopt);
    }
    return out;
}

Stmt FormatStmtNoTrivia(const Ctx &ctx, const Stmt &s, Shape shape) { return FormatStmt(ctx, s, shape); }

// ---- trivia_util.rs helpers -------------------------------------------------------------

bool HasInlineComments(const Expr &e) {
    switch (e.kind) {
    case EK::Binary:
        return TokenContainsComments(e.tok) || ContainsComments(*e.lhs) || HasInlineComments(*e.rhs);
    case EK::Unary:
        return TokenContainsComments(e.tok) || HasInlineComments(*e.rhs);
    case EK::Paren:
        return AnyComment(e.contained.open.trailing, CS::All) || AnyComment(e.contained.close.leading, CS::All) ||
               ContainsComments(*e.rhs);
    default:
        return false;
    }
}

bool CanHangExpression(const Expr &e) {
    switch (e.kind) {
    case EK::Paren:
    case EK::Binary:
        return true;
    case EK::Unary:
        return CanHangExpression(*e.rhs);
    case EK::Call:
        return !e.call->prefix.is_name && CanHangExpression(*e.call->prefix.expr);
    case EK::Var:
        return !e.var->is_name && !e.var->expr->prefix.is_name && CanHangExpression(*e.var->expr->prefix.expr);
    default:
        return false;
    }
}

bool IsBlockEmpty(const Block &b) { return b.stmts.empty() && !b.last; }

bool IsBlockSimple(const Block &) { return false; }  // only used by the collapse_simple_statement options

}  // namespace luafmt::detail

// ---- entry point -----------------------------------------------------------------------

namespace luafmt {

namespace {

using namespace detail;  // NOLINT

// The string's value with escapes resolved enough to compare across quote styles.
std::string StringKey(const Token &t) {
    if (t.quote == '[') {
        std::string body;
        for (size_t i = 0; i < t.text.size(); i++)
            if (!(t.text[i] == '\r' && i + 1 < t.text.size() && t.text[i + 1] == '\n')) body += t.text[i];
        return "[" + body;
    }
    std::string out;
    for (size_t i = 0; i < t.text.size(); i++) {
        const char c = t.text[i];
        if (c == '\\' && i + 1 < t.text.size()) {
            const char n = t.text[i + 1];
            if (n == '"' || n == '\'' || std::string_view("abfnrtuvxz0123456789\\\n\r").find(n) == std::string_view::npos) {
                out += n;
                i++;
                continue;
            }
            out += c;
            out += n;
            i++;
            continue;
        }
        out += c;
    }
    return "q" + out;
}

// Non-trivia tokens with the changes StyLua is allowed to make taken out:
// parentheses (it drops redundant ones and adds call parentheses), separators
// (semicolons go, tables gain or lose a trailing comma),
// string quoting, and leading-dot numbers.
bool Signature(std::string_view src, std::vector<std::string> &out) {
    std::vector<cst::Token> toks;
    cst::ParseError err;
    if (!cst::Lex(src, toks, err)) return false;
    for (const auto &t : toks) {
        if (t.IsTrivia() || t.type == cst::TT::Eof) continue;
        if (t.type == cst::TT::Symbol && (t.text == "(" || t.text == ")" || t.text == ";" || t.text == ",")) continue;
        if (t.type == cst::TT::String) {
            out.push_back(StringKey(t));
        } else if (t.type == cst::TT::Number && !t.text.empty() && t.text[0] == '.') {
            out.push_back("0" + t.text);
        } else {
            out.push_back(t.text);
        }
    }
    return true;
}

// Every comment's text, in order (trailing whitespace trimmed: StyLua trims it).
std::vector<std::string> Comments(std::string_view src) {
    std::vector<std::string> out;
    std::vector<cst::Token> toks;
    cst::ParseError err;
    if (!cst::Lex(src, toks, err)) return out;
    for (const auto &t : toks) {
        if (t.type == cst::TT::SingleLineComment) {
            std::string s = t.text;
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
            out.push_back(s);
        } else if (t.type == cst::TT::MultiLineComment) {
            std::string s;
            for (char c : t.text)
                if (c != '\r') s += c;
            out.push_back(s);
        }
    }
    return out;
}

}  // namespace

constexpr size_t kWorkPerByte = 100;
constexpr size_t kWorkBase = 200000;

Result Format(std::string_view src, const Options &opts) {
    Result r;
    r.text = std::string(src);
    cst::Ast ast;
    cst::ParseError err;
    if (!cst::Parse(src, ast, err)) {
        r.ok = false;
        r.error = err.message;
        r.error_line = err.line;
        return r;
    }
    Ctx ctx;
    ctx.cfg.column_width = opts.width > 0 ? static_cast<size_t>(opts.width) : 120;
    ctx.cfg.indent_width = opts.indent > 0 ? static_cast<size_t>(opts.indent) : 4;
    ctx.cfg.tabs = opts.use_tabs;
    const Shape shape = Shape::New(ctx);
    cst::Ast out;
    g_work_left = kWorkPerByte * src.size() + kWorkBase;
    try {
        out.block = FormatBlock(ctx, ast.block, shape);
        out.eof = FormatEof(ctx, ast.eof, shape);
    } catch (const WorkExhausted &) {
        r.ok = false;
        r.error = "too deeply nested to format";
        return r;
    }
    std::string text = cst::Print(out);
    // Self-check: the output must lex to the same program and keep every comment.
    std::vector<std::string> a, b;
    if (!opts.verify) {
        r.ok = true;
        r.text = std::move(text);
        return r;
    }
    if (!Signature(src, a) || !Signature(text, b) || a != b) {
        r.ok = false;
        r.error = "internal error: formatting changed the program; left unformatted";
        for (size_t k = 0; k < std::min(a.size(), b.size()); k++)
            if (a[k] != b[k]) {
                r.error += " (`" + a[k] + "` became `" + b[k] + "`)";
                break;
            }
        return r;
    }
    if (Comments(src) != Comments(text)) {
        r.ok = false;
        r.error = "internal error: formatting lost a comment; left unformatted";
        return r;
    }
    cst::Ast check;
    if (!cst::Parse(text, check, err)) {
        r.ok = false;
        r.error = "internal error: formatted output does not parse; left unformatted";
        return r;
    }
    r.ok = true;
    r.text = std::move(text);
    return r;
}

}  // namespace luafmt
