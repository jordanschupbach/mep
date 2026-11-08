// mep's Lua formatter, continued (see lua_format.cpp): expressions, function
// calls and bodies, and table constructors -- StyLua 2.3.1's expression.rs,
// functions.rs and table.rs layout rules, ported to mep's CST.
#include <algorithm>
#include <cctype>

#include "lua_format_internal.h"

namespace luafmt::detail {

namespace {

enum class ECtx { Standard, Prefix, BinaryLHS, BinaryLHSExponent, UnaryOrBinary };
enum class NextNode { Obscure, None };

std::vector<Token> One(Token t) { return std::vector<Token>{std::move(t)}; }
Token EmptyWs() { return Spaces(0); }  // StyLua's spaces(0) for SpaceAfterFunctionNames::Never

int Prec(const TokenRef &op) {
    const std::string &s = op.tok.text;
    if (s == "^") return 12;
    if (s == "%" || s == "/" || s == "*" || s == "//") return 10;
    if (s == "-" || s == "+") return 9;
    if (s == "..") return 8;
    if (s == "<<" || s == ">>") return 7;
    if (s == "&") return 6;
    if (s == "~") return 5;
    if (s == "|") return 4;
    if (s == ">" || s == ">=" || s == "<" || s == "<=" || s == "~=" || s == "==") return 3;
    if (s == "and") return 2;
    if (s == "or") return 1;
    return 0;
}
bool RightAssoc(const TokenRef &op) { return op.tok.text == "^" || op.tok.text == ".."; }

Expr FormatExprInternal(const Ctx &ctx, const Expr &e, ECtx ec, Shape shape);
Expr FormatHangingExpr(const Ctx &ctx, const Expr &e, Shape shape, ECtx ec, std::optional<std::pair<size_t, size_t>> lhs_range,
                       size_t lhs_indent);
Suffix FormatSuffix(const Ctx &ctx, const Suffix &s, Shape shape, NextNode next);
FunctionArgs FormatFunctionArgs(const Ctx &ctx, const FunctionArgs &a, Shape shape, NextNode next);
Box<AnonFn> FormatAnonymousFunction(const Ctx &ctx, const AnonFn &f, Shape shape);

TokenRef FormatBinop(const Ctx &ctx, const TokenRef &op, Shape shape) {
    if (op.tok.text == ">>") {
        TokenRef r = FormatTokenRef(ctx, op, shape);
        r.leading.push_back(Spaces(1));
        r.trailing.push_back(Spaces(1));
        return r;
    }
    return FmtSymbol(ctx, op, " " + op.tok.text + " ", shape);
}

TokenRef FormatUnop(const Ctx &ctx, const TokenRef &op, Shape shape) {
    return FmtSymbol(ctx, op, op.tok.text == "not" ? "not " : op.tok.text, shape);
}

bool CheckExcessParentheses(const Expr &inner, ECtx ec) {
    switch (inner.kind) {
    case EK::Paren:
        return true;
    case EK::Unary:
        if (ec == ECtx::BinaryLHSExponent) return false;
        if (ec == ECtx::BinaryLHS && inner.tok.tok.text == "not") return false;
        return CheckExcessParentheses(*inner.rhs, ec);
    case EK::Binary:
    case EK::Call:
        return false;
    case EK::Symbol:
        return inner.tok.tok.text != "...";
    default:
        return true;
    }
}

std::pair<std::vector<Token>, std::vector<Token>> ContainedSpanComments(const Ctx &ctx, const ContainedSpan &c,
                                                                         Shape shape) {
    std::vector<Token> lead, trail;
    for (const auto &t : c.open.leading)
        if (t.IsComment()) {
            lead.push_back(IndentTrivia(ctx, shape));
            lead.push_back(t);
            lead.push_back(NewlineTok(ctx));
        }
    for (const auto &t : c.close.trailing)
        if (t.IsComment()) {
            trail.push_back(Spaces(1));
            trail.push_back(t);
        }
    return {std::move(lead), std::move(trail)};
}

bool IsBracketsString(const Expr &e) {
    if (e.kind == EK::String) return e.tok.tok.quote == '[';
    if (e.kind == EK::Paren) return IsBracketsString(*e.rhs);
    return false;
}

bool IsString(const Expr &e) {
    if (e.kind == EK::String) return true;
    if (e.kind == EK::Paren) return IsString(*e.rhs);
    return false;
}

Expr FormatExprInternal(const Ctx &ctx, const Expr &e, ECtx ec, Shape shape) {
    SpendWork();
    Expr r;
    r.kind = e.kind;
    switch (e.kind) {
    case EK::Function:
        r.func = FormatAnonymousFunction(ctx, *e.func, shape);
        return r;
    case EK::Call:
        r.call = FormatFunctionCall(ctx, *e.call, shape);
        return r;
    case EK::Number:
    case EK::String:
    case EK::Symbol:
        r.tok = FormatTokenRef(ctx, e.tok, shape);
        return r;
    case EK::Table:
        r.table = FormatTableConstructor(ctx, *e.table, shape);
        return r;
    case EK::Var:
        r.var = FormatVar(ctx, *e.var, shape);
        return r;
    case EK::Paren: {
        const bool keep = ec == ECtx::Prefix;
        const bool use_inner = CheckExcessParentheses(*e.rhs, ec);
        if (use_inner && !keep) {
            auto [lead, trail] = ContainedSpanComments(ctx, e.contained, shape);
            return UpdTrail(UpdLead(FormatExpression(ctx, *e.rhs, shape), FT::Append(std::move(lead))),
                            FT::Append(std::move(trail)));
        }
        r.contained = FormatContainedSpan(ctx, e.contained, shape);
        r.rhs = FormatExpression(ctx, *e.rhs, shape + 1);
        return r;
    }
    case EK::Unary: {
        r.tok = FormatUnop(ctx, e.tok, shape);
        const Shape sh = shape + Print(StripLead(r.tok)).size();
        Expr inner = FormatExprInternal(ctx, *e.rhs, ECtx::UnaryOrBinary, sh);
        if (r.tok.tok.text == "-") {
            bool need = false;
            if (inner.kind == EK::Unary && inner.tok.tok.text == "-") need = true;
            if (inner.kind == EK::Paren && inner.rhs->kind == EK::Unary && inner.rhs->tok.tok.text == "-") need = true;
            if (need) {
                auto [ne, comments] = TakeTrailingComments(inner);
                Expr p;
                p.kind = EK::Paren;
                p.contained = ContainedSpan{Sym("("), Sym(")")};
                p.contained.close.trailing.insert(p.contained.close.trailing.end(), comments.begin(), comments.end());
                p.rhs = std::move(ne);
                inner = std::move(p);
            }
        }
        r.rhs = std::move(inner);
        return r;
    }
    case EK::Binary: {
        const ECtx lc = e.tok.tok.text == "^" ? ECtx::BinaryLHSExponent : ECtx::BinaryLHS;
        r.lhs = FormatExprInternal(ctx, *e.lhs, lc, shape);
        r.tok = FormatBinop(ctx, e.tok, shape);
        const Shape sh = shape.TakeLastLine(Print(*r.lhs)) + Print(r.tok).size();
        r.rhs = FormatExprInternal(ctx, *e.rhs, ECtx::UnaryOrBinary, sh);
        return r;
    }
    }
    return r;
}

std::pair<TokenRef, TokenRef> ProcessDotName(const Ctx &ctx, const TokenRef &dot, const TokenRef &name, Shape shape) {
    auto [d, dot_comments] = TakeTrailingComments(FormatTokenRef(ctx, dot, shape));
    auto [n, name_comments] = TakeLeadingComments(FormatTokenRef(ctx, name, shape));
    dot_comments.insert(dot_comments.end(), name_comments.begin(), name_comments.end());
    if (!dot_comments.empty()) {
        d.leading.insert(d.leading.end(), dot_comments.begin(), dot_comments.end());
        d = PrependNewlineIndent(ctx, d, shape);
    }
    return {std::move(d), std::move(n)};
}

Index FormatIndex(const Ctx &ctx, const Index &i, Shape shape) {
    Index r;
    r.is_dot = i.is_dot;
    if (i.is_dot) {
        auto [d, n] = ProcessDotName(ctx, i.dot, i.name, shape);
        r.dot = std::move(d);
        r.name = std::move(n);
        return r;
    }
    if (HasTrailingComments(i.brackets.open, CS::All) || ContainsComments(*i.expr) ||
        HasLeadingComments(i.brackets.close, CS::All)) {
        const Shape indent_shape = shape.Reset().IncrementAdditionalIndent();
        r.brackets.open = FmtSymbol(ctx, i.brackets.open, "[", shape);
        r.brackets.open.trailing.push_back(NewlineTok(ctx));
        r.brackets.open.trailing.push_back(IndentTrivia(ctx, indent_shape));
        r.brackets.close = FormatEndToken(ctx, i.brackets.close, EndTokenType::IndentComments, shape);
        r.brackets.close.leading.push_back(IndentTrivia(ctx, shape));
        r.expr = UpdTrail(FormatExpression(ctx, *i.expr, indent_shape), FT::Append(One(NewlineTok(ctx))));
    } else if (IsBracketsString(*i.expr)) {
        r.brackets = FormatContainedSpan(ctx, i.brackets, shape);
        r.expr = UpdTrail(UpdLead(FormatExpression(ctx, *i.expr, shape + 2), FT::Append(One(Spaces(1)))),
                          FT::Append(One(Spaces(1))));
    } else {
        r.brackets = FormatContainedSpan(ctx, i.brackets, shape);
        r.expr = FormatExpression(ctx, *i.expr, shape + 1);
    }
    return r;
}

Prefix FormatPrefix(const Ctx &ctx, const Prefix &p, Shape shape) {
    Prefix r;
    r.is_name = p.is_name;
    if (p.is_name) {
        r.name = FormatTokenRef(ctx, p.name, shape);
        return r;
    }
    Expr single = FormatExprInternal(ctx, *p.expr, ECtx::Prefix, shape);
    if (shape.TakeFirstLine(Print(StripTrivia(single))).OverBudget() && !IsString(*p.expr)) {
        r.expr = FormatHangingExpr(ctx, *p.expr, shape, ECtx::Prefix, std::nullopt, 0);
    } else {
        r.expr = std::move(single);
    }
    return r;
}

// ---- hanging -------------------------------------------------------------------

TokenRef HangBinop(const Ctx &ctx, const TokenRef &binop, Shape shape, const Expr &rhs) {
    std::vector<Token> lead;
    for (const auto &t : binop.leading)
        if (t.IsComment()) {
            lead.push_back(NewlineTok(ctx));
            lead.push_back(IndentTrivia(ctx, shape));
            lead.push_back(t);
        }
    for (const auto &t : binop.trailing)
        if (t.IsComment()) {
            lead.push_back(Spaces(1));
            lead.push_back(t);
        }
    for (const auto &t : LeadingComments(rhs)) {
        lead.push_back(NewlineTok(ctx));
        lead.push_back(IndentTrivia(ctx, shape));
        lead.push_back(t);
    }
    lead.push_back(NewlineTok(ctx));
    lead.push_back(IndentTrivia(ctx, shape));
    TokenRef r = binop;
    r.leading = std::move(lead);
    r.trailing = One(Spaces(1));
    return r;
}

size_t BinopExpressionLength(const Expr &e, const TokenRef &top) {
    if (e.kind == EK::Binary) {
        if (Prec(e.tok) >= Prec(top) && RightAssoc(e.tok) == RightAssoc(top)) {
            if (RightAssoc(e.tok))
                return BinopExpressionLength(*e.rhs, top) + Print(StripTrivia(e.tok)).size() + 2 +
                       Print(StripTrivia(*e.lhs)).size();
            return BinopExpressionLength(*e.lhs, top) + Print(StripTrivia(e.tok)).size() + 2 +
                   Print(StripTrivia(*e.rhs)).size();
        }
        return 0;
    }
    return Print(StripTrivia(e)).size();
}

bool BinopExpressionContainsComments(const Expr &e, const TokenRef &top) {
    if (e.kind != EK::Binary || Prec(e.tok) != Prec(top)) return false;
    return TokenContainsComments(e.tok) || HasLeadingComments(*e.rhs, CS::All) || HasTrailingComments(*e.lhs, CS::All) ||
           BinopExpressionContainsComments(*e.lhs, top) || BinopExpressionContainsComments(*e.rhs, top);
}

struct LeftmostRangeHang {
    std::pair<size_t, size_t> range;
    size_t original_additional_indent_level = 0;

    static LeftmostRangeHang Find(const Expr &e, size_t level) {
        if (e.kind == EK::Binary) return Find(*e.lhs, level);
        return LeftmostRangeHang{Range(e), level};
    }
    Shape RequiredShape(Shape shape, std::pair<size_t, size_t> item) const {
        if (range.first >= item.first && range.second <= item.second)
            return shape.WithIndent(shape.WithAdditionalIndent(original_additional_indent_level).indent);
        return shape;
    }
};

using LhsRange = std::optional<LeftmostRangeHang>;

bool IsHangBinopOverWidth(Shape shape, const Expr &e, const TokenRef &top, const LhsRange &lhs_range) {
    if (lhs_range) shape = lhs_range->RequiredShape(shape, Range(e));
    return shape.Add(BinopExpressionLength(e, top)).OverBudget();
}

int BinopPrecedenceLevel(const Expr &e) { return e.kind == EK::Binary ? Prec(e.tok) : 0; }

bool DidHangExpression(const Expr &e) {
    if (e.kind != EK::Binary) return false;
    for (const auto &t : e.tok.leading)
        if (TriviaIsNewline(t)) return true;
    return false;
}

Expr FormatHangingExprR(const Ctx &ctx, const Expr &e, Shape shape, ECtx ec, const LhsRange &lhs_range);

Expr HangBinopExpression(const Ctx &ctx, const Expr &e, const TokenRef &top_binop, Shape shape, const LhsRange &lhs_range,
                         ECtx ec) {
    if (e.kind != EK::Binary) return FormatHangingExprR(ctx, e, shape, ec, lhs_range);
    const TokenRef &binop = e.tok;
    const bool same_op_level = Prec(binop) == Prec(top_binop) && RightAssoc(binop) == RightAssoc(top_binop);
    const bool right = RightAssoc(binop);
    const Shape test_shape = same_op_level ? shape : shape.IncrementAdditionalIndent();
    const bool over = IsHangBinopOverWidth(test_shape, e, binop, lhs_range);
    const bool should_hang = same_op_level || over || BinopExpressionContainsComments(e, binop);
    if (should_hang) shape = test_shape;
    TokenRef new_binop = FormatBinop(ctx, binop, shape);
    if (should_hang) new_binop = HangBinop(ctx, binop, shape, *e.rhs);
    Expr r;
    r.kind = EK::Binary;
    if (should_hang) {
        const Shape lhs_shape = shape;
        const Shape rhs_shape = shape.Reset() + Print(StripTrivia(new_binop)).size() + 1;
        Expr lhs, rhs;
        if (!right) {
            lhs = HangBinopExpression(ctx, *e.lhs, same_op_level ? top_binop : binop, lhs_shape, lhs_range, ec);
            rhs = ContainsComments(*e.rhs) ? HangBinopExpression(ctx, *e.rhs, binop, shape, lhs_range, ec)
                                           : FormatExprInternal(ctx, *e.rhs, ECtx::UnaryOrBinary, rhs_shape);
        } else {
            if (ContainsComments(*e.lhs)) {
                lhs = HangBinopExpression(ctx, *e.lhs, binop, shape, lhs_range, ec);
            } else {
                const ECtx c = binop.tok.text == "^" ? ECtx::BinaryLHSExponent : ECtx::BinaryLHS;
                lhs = FormatExprInternal(ctx, *e.lhs, c, lhs_shape);
            }
            rhs = HangBinopExpression(ctx, *e.rhs, same_op_level ? top_binop : binop, rhs_shape, lhs_range, ec);
        }
        r.lhs = std::move(lhs);
        r.rhs = UpdLead(rhs, FT::Replace({}));
    } else {
        if (ContainsComments(*e.lhs)) {
            r.lhs = HangBinopExpression(ctx, *e.lhs, binop, shape, lhs_range, ec);
        } else {
            const ECtx c = binop.tok.text == "^" ? ECtx::BinaryLHSExponent : ECtx::BinaryLHS;
            r.lhs = FormatExprInternal(ctx, *e.lhs, c, shape);
        }
        if (ContainsComments(*e.rhs)) {
            r.rhs = HangBinopExpression(ctx, *e.rhs, binop, shape, lhs_range, ec);
        } else {
            r.rhs = FormatExprInternal(ctx, *e.rhs, ECtx::UnaryOrBinary, shape);
        }
    }
    r.tok = std::move(new_binop);
    return r;
}

Expr FormatHangingExprR(const Ctx &ctx, const Expr &e, Shape shape, ECtx ec, const LhsRange &lhs_range) {
    SpendWork();
    const auto expression_range = Range(e);
    switch (e.kind) {
    case EK::Paren: {
        const Shape lhs_shape = lhs_range ? lhs_range->RequiredShape(shape, expression_range) : shape;
        const bool keep = ec == ECtx::Prefix;
        const bool use_inner = CheckExcessParentheses(*e.rhs, ec);
        if (use_inner && !keep) {
            auto [lead, trail] = ContainedSpanComments(ctx, e.contained, shape);
            return UpdTrail(UpdLead(FormatHangingExprR(ctx, *e.rhs, lhs_shape, ec, lhs_range), FT::Append(std::move(lead))),
                            FT::Append(std::move(trail)));
        }
        ContainedSpan contained = FormatContainedSpan(ctx, e.contained, lhs_shape);
        Expr formatted = FormatExpression(ctx, *e.rhs, lhs_shape + 1);
        const std::string str = Print(formatted);
        if (!ContainsComments(*e.rhs) && !lhs_shape.Add(2 + str.size()).OverBudget()) {
            Expr r;
            r.kind = EK::Paren;
            r.contained = std::move(contained);
            r.rhs = std::move(formatted);
            return r;
        }
        const Shape expression_shape = lhs_shape.Reset().IncrementAdditionalIndent();
        contained.open.trailing.push_back(NewlineTok(ctx));
        contained.open.trailing.push_back(IndentTrivia(ctx, expression_shape));
        contained.close.leading.push_back(NewlineTok(ctx));
        contained.close.leading.push_back(IndentTrivia(ctx, lhs_shape));
        Expr r;
        r.kind = EK::Paren;
        r.contained = std::move(contained);
        r.rhs = FormatHangingExprR(ctx, *e.rhs, expression_shape, ECtx::Standard, std::nullopt);
        return r;
    }
    case EK::Unary: {
        Expr r;
        r.kind = EK::Unary;
        r.tok = FormatUnop(ctx, e.tok, shape);
        const Shape sh = shape + Print(StripLead(r.tok)).size();
        r.rhs = FormatHangingExprR(ctx, *e.rhs, sh, ECtx::UnaryOrBinary, lhs_range);
        return r;
    }
    case EK::Binary: {
        Expr lhs = HangBinopExpression(ctx, *e.lhs, e.tok, shape, lhs_range, ECtx::UnaryOrBinary);
        const Shape current_shape = shape.TakeLastLine(Print(lhs)) + 1;
        TokenRef new_binop = FormatBinop(ctx, e.tok, current_shape);
        const Shape singleline_shape = current_shape + Print(StripTrivia(e.tok)).size() + 1;
        Expr new_rhs = HangBinopExpression(ctx, *e.rhs, e.tok, singleline_shape, std::nullopt, ECtx::Standard);
        if ((DidHangExpression(lhs) && BinopPrecedenceLevel(lhs) >= Prec(e.tok)) ||
            (DidHangExpression(new_rhs) && BinopPrecedenceLevel(new_rhs) >= Prec(e.tok)) ||
            TokenContainsComments(e.tok) || HasTrailingComments(lhs, CS::All) ||
            (shape.TakeLastLine(Print(lhs)) + (Print(e.tok) + Print(*e.rhs)).size()).OverBudget()) {
            const Shape hanging_shape = shape.Reset() + Print(StripTrivia(e.tok)).size() + 1;
            new_binop = HangBinop(ctx, e.tok, shape, *e.rhs);
            new_rhs = UpdLead(HangBinopExpression(ctx, *e.rhs, e.tok, hanging_shape, std::nullopt, ECtx::Standard),
                              FT::Replace({}));
        }
        Expr r;
        r.kind = EK::Binary;
        r.lhs = std::move(lhs);
        r.tok = std::move(new_binop);
        r.rhs = std::move(new_rhs);
        return r;
    }
    default: {
        const Shape value_shape = lhs_range ? lhs_range->RequiredShape(shape, expression_range) : shape;
        return FormatExprInternal(ctx, e, ec, value_shape);
    }
    }
}

Expr FormatHangingExpr(const Ctx &ctx, const Expr &e, Shape shape, ECtx ec, std::optional<std::pair<size_t, size_t>>, size_t) {
    return FormatHangingExprR(ctx, e, shape, ec, std::nullopt);
}

}  // namespace

Expr FormatExpression(const Ctx &ctx, const Expr &e, Shape shape) {
    return FormatExprInternal(ctx, e, ECtx::Standard, shape);
}

Expr HangExpression(const Ctx &ctx, const Expr &e, Shape shape, std::optional<size_t> hang_level) {
    const size_t original = shape.indent.additional;
    if (hang_level) shape = shape.WithIndent(shape.AddIndentLevel(*hang_level).indent);
    LhsRange lhs_range;
    if (hang_level) lhs_range = LeftmostRangeHang::Find(e, original);
    return FormatHangingExprR(ctx, e, shape, ECtx::Standard, lhs_range);
}

Expr HangExpressionTrailingNewline(const Ctx &ctx, const Expr &e, Shape shape, std::optional<size_t> hang_level) {
    return UpdTrail(HangExpression(ctx, e, shape, hang_level), FT::Append(One(NewlineTok(ctx))));
}

Var FormatVar(const Ctx &ctx, const Var &v, Shape shape) {
    Var r;
    r.is_name = v.is_name;
    if (v.is_name) {
        r.name = FormatTokenRef(ctx, v.name, shape);
    } else {
        r.expr = FormatFunctionCall(ctx, *v.expr, shape);
    }
    return r;
}

// ---- functions.rs -------------------------------------------------------------------

namespace {

Box<AnonFn> FormatAnonymousFunction(const Ctx &ctx, const AnonFn &f, Shape shape) {
    AnonFn r;
    r.function_token = FmtSymbol(ctx, f.function_token, "function", shape);
    r.function_token.trailing.push_back(EmptyWs());
    r.body = FormatFunctionBody(ctx, *f.body, shape.Add(8));
    return r;
}

Suffix FormatCall(const Ctx &ctx, const Suffix &s, Shape shape, NextNode next) {
    Suffix r;
    r.kind = s.kind;
    if (s.kind == Suffix::kAnon) {
        r.args = UpdLead(FormatFunctionArgs(ctx, s.args, shape, next), FT::Append(One(EmptyWs())));
        return r;
    }
    auto [colon, name] = ProcessDotName(ctx, s.colon, s.method_name, shape);
    const Shape sh = shape + (Print(colon).size() + Print(name).size());
    r.colon = std::move(colon);
    r.method_name = std::move(name);
    r.args = UpdLead(FormatFunctionArgs(ctx, s.args, sh, next), FT::Append(One(EmptyWs())));
    return r;
}

Suffix FormatSuffix(const Ctx &ctx, const Suffix &s, Shape shape, NextNode next) {
    if (s.IsCall()) return FormatCall(ctx, s, shape, next);
    Suffix r;
    r.kind = Suffix::kIndex;
    r.index = FormatIndex(ctx, s.index, shape);
    return r;
}

bool IsComplexArg(const Expr &e) {
    const std::string s = Print(e);
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a).find('\n') != std::string::npos;
}

bool FunctionTriviaContainsComments(const Token &t) {
    return t.type == TT::SingleLineComment || (t.type == TT::MultiLineComment && Lines(t.text).size() > 1);
}

bool FunctionArgsContainsComments(const ContainedSpan &parens, const Punctuated<Expr> &args) {
    if (HasTrailingComments(parens.open, CS::Single) || HasLeadingComments(parens.close, CS::Single)) return true;
    for (const auto &pr : args.pairs) {
        for (const auto &t : LeadTrivia(pr.value))
            if (FunctionTriviaContainsComments(t)) return true;
        for (const auto &t : TrailTrivia(pr.value))
            if (FunctionTriviaContainsComments(t)) return true;
        if (pr.punct) {
            for (const auto &t : pr.punct->leading)
                if (FunctionTriviaContainsComments(t)) return true;
            for (const auto &t : pr.punct->trailing)
                if (FunctionTriviaContainsComments(t)) return true;
        }
    }
    return false;
}

bool ShouldCollapseFunctionBody(const FunctionBody &b) {
    bool require_multiline = false;
    for (const auto &t : b.parens.close.trailing) require_multiline = require_multiline || t.IsComment();
    for (const auto &t : b.end_token.leading) require_multiline = require_multiline || t.IsComment();
    require_multiline = require_multiline || ContainsComments(b.block);
    return !require_multiline && IsBlockEmpty(b.block);
}

enum class ArgState { None, SeenMultiline, SeenNonMultilineAfterMultiline };

bool FunctionArgsMultilineHeuristic(const Ctx &ctx, const Punctuated<Expr> &args, Shape shape) {
    if (args.empty()) return false;
    if (shape.simple_heuristics) return false;
    Shape singleline = shape + 1;
    ArgState state = ArgState::None;
    for (const auto &pr : args.pairs) {
        const Expr argument = FormatExpression(ctx, pr.value, shape.WithSimpleHeuristics().WithInfiniteWidth());
        if (argument.kind == EK::Function) {
            const bool expanded = !ShouldCollapseFunctionBody(*argument.func->body);
            if (expanded) {
                if (state == ArgState::SeenNonMultilineAfterMultiline) return true;
                if (state == ArgState::None) state = ArgState::SeenMultiline;
                singleline = singleline.TakeFirstLine(Print(argument));
                if (singleline.OverBudget()) return true;
                singleline = singleline.Reset() + 3;
            } else {
                singleline = singleline + Print(argument).size();
            }
        } else if (argument.kind == EK::Table) {
            bool expanded = false;
            for (const auto &t : argument.table->braces.open.trailing) expanded = expanded || TriviaIsNewline(t);
            if (expanded) {
                if (state == ArgState::SeenNonMultilineAfterMultiline) return true;
                singleline = singleline.TakeFirstLine(Print(*argument.table));
                if (singleline.OverBudget()) return true;
                if (state == ArgState::None) state = ArgState::SeenMultiline;
                singleline = singleline.Reset() + 1;
            } else {
                singleline = singleline + Print(argument).size();
            }
        } else {
            if (state == ArgState::SeenMultiline) state = ArgState::SeenNonMultilineAfterMultiline;
            if (argument.kind != EK::Paren && argument.kind != EK::Unary && argument.kind != EK::Binary) {
                if (IsComplexArg(argument) && args.size() > 1) return true;
            }
            const std::string s = Print(argument);
            if (singleline.TakeFirstLine(s).OverBudget()) return true;
            singleline = singleline.TakeLastLine(s);
        }
        if (singleline.OverBudget()) return true;
        if (pr.punct) {
            size_t extra = 0;
            for (const auto &t : pr.punct->trailing)
                if (t.IsComment()) extra += TokenText(t).size();
            singleline = singleline + 2 + extra;
        }
    }
    return singleline.Add(1).OverBudget();
}

Expr FormatArgumentMultiline(const Ctx &ctx, const Expr &arg, Shape shape) {
    Expr inf = FormatExpression(ctx, arg, shape.WithInfiniteWidth());
    if (HasInlineComments(arg) || shape.Add(Print(StripTrivia(inf)).size()).OverBudget()) {
        if (CanHangExpression(arg)) return HangExpression(ctx, arg, shape, kHang1);
        return FormatExpression(ctx, arg, shape);
    }
    return inf;
}

FunctionArgs WrapInParens(const Ctx &ctx, const Expr &e, Shape shape) {
    Expr ne = FormatExpression(ctx, e, shape + 1);
    auto [stripped, comments] = TakeTrailingComments(ne);
    FunctionArgs r;
    r.kind = FunctionArgs::Parens;
    r.parens = ContainedSpan{Sym("("), Sym(")")};
    r.parens.close.trailing.insert(r.parens.close.trailing.end(), comments.begin(), comments.end());
    Punctuated<Expr> args;
    args.push(std::move(stripped));
    r.args = std::move(args);
    return r;
}

FunctionArgs FormatFunctionArgs(const Ctx &ctx, const FunctionArgs &a, Shape shape, NextNode) {
    if (a.kind == FunctionArgs::String) {
        Expr e;
        e.kind = EK::String;
        e.tok = a.str;
        return WrapInParens(ctx, e, shape);
    }
    if (a.kind == FunctionArgs::Table) {
        Expr e;
        e.kind = EK::Table;
        e.table = *a.table;
        return WrapInParens(ctx, e, shape);
    }
    const Punctuated<Expr> &args = *a.args;
    const bool force = FunctionArgsContainsComments(a.parens, args);
    const bool multiline = force || FunctionArgsMultilineHeuristic(ctx, args, shape);
    const bool hug = multiline && !force && args.size() == 1 && args.pairs.front().value.kind == EK::Table;
    FunctionArgs r;
    r.kind = FunctionArgs::Parens;
    if (multiline && !hug) {
        auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatArgumentMultiline(c, e, s); };
        auto [parens, out] = FormatContainedPunctuatedMultiline(ctx, a.parens, args, fmt, shape);
        r.parens = std::move(parens);
        r.args = std::move(out);
        return r;
    }
    const size_t inc = hug ? 2 : 1;
    TokenRef open = FormatTokenRef(ctx, a.parens.open, shape);
    if (HasTrailingComments(open, CS::All) && !args.empty()) open.trailing.push_back(Spaces(1));
    TokenRef close = FormatTokenRef(ctx, a.parens.close, shape);
    auto fmt = [](const Ctx &c, const Expr &e, Shape s) { return FormatExpression(c, e, s); };
    Punctuated<Expr> out = FormatPunctuated(ctx, args, shape + inc, fmt);
    for (auto &pr : out.pairs) {
        if (TokenRef *t = FirstTok(pr.value)) {
            size_t k = 0;
            while (k < t->leading.size() && TriviaIsWhitespace(t->leading[k])) k++;
            t->leading.erase(t->leading.begin(), t->leading.begin() + static_cast<std::ptrdiff_t>(k));
        }
    }
    r.parens = ContainedSpan{std::move(open), std::move(close)};
    r.args = std::move(out);
    return r;
}

TokenRef FormatParameter(const Ctx &ctx, const TokenRef &p, Shape shape) {
    if (p.tok.IsSymbol("...")) return FmtSymbol(ctx, p, "...", shape);
    return FormatTokenRef(ctx, p, shape);
}

Punctuated<TokenRef> FormatSinglelineParameters(const Ctx &ctx, const FunctionBody &b, Shape shape) {
    Punctuated<TokenRef> out;
    for (const auto &pr : b.params.pairs) {
        TokenRef p = FormatParameter(ctx, pr.value, shape);
        std::optional<TokenRef> punct;
        if (pr.punct) punct = FmtSymbol(ctx, *pr.punct, ", ", shape);
        out.push(std::move(p), std::move(punct));
    }
    return out;
}

bool ShouldParametersFormatMultiline(const Ctx &ctx, const FunctionBody &b, Shape shape, bool should_collapse) {
    if (b.params.empty()) return false;
    size_t len = Print(FormatSinglelineParameters(ctx, b, shape)).size() + 2;
    if (should_collapse) len += 4;
    return (shape + len).OverBudget();
}

bool ShouldInlinePrefix(const Ctx &ctx, const Prefix &p) {
    const std::string s = Print(StripTrivia(p));
    return (!s.empty() && std::isupper(static_cast<unsigned char>(s[0]))) || s.size() <= ctx.cfg.indent_width;
}

bool SuffixContainsComments(const Suffix &s) {
    if (s.kind == Suffix::kIndex && s.index.is_dot)
        return HasTrailingComments(s.index.dot, CS::All) || HasLeadingComments(s.index.name, CS::All);
    if (s.kind == Suffix::kMethod) return HasTrailingComments(s.colon, CS::Single);
    return false;
}

}  // namespace

FunctionBody FormatFunctionBody(const Ctx &ctx, const FunctionBody &b, Shape shape) {
    const bool should_collapse = ShouldCollapseFunctionBody(b);
    bool contains_comments = false;
    for (const auto &pr : b.params.pairs)
        contains_comments = contains_comments || (pr.punct && TokenContainsComments(*pr.punct)) ||
                            TokenContainsComments(pr.value);
    const bool multiline_params = contains_comments || ShouldParametersFormatMultiline(ctx, b, shape, should_collapse);
    const bool singleline_function = !multiline_params && should_collapse;
    FunctionBody r;
    if (multiline_params) {
        auto fmt = [](const Ctx &c, const TokenRef &t, Shape s) { return FormatParameter(c, t, s); };
        auto [parens, params] = FormatContainedPunctuatedMultiline(ctx, b.parens, b.params, fmt, shape);
        r.parens = std::move(parens);
        r.params = std::move(params);
    } else {
        r.parens = FormatContainedSpan(ctx, b.parens, shape);
        r.params = FormatSinglelineParameters(ctx, b, shape);
    }
    if (!singleline_function) r.block = FormatBlock(ctx, b.block, shape.Reset().IncrementBlockIndent());
    r.parens.close.trailing.push_back(singleline_function ? Spaces(1) : NewlineTok(ctx));
    r.end_token = FormatEndToken(ctx, b.end_token, EndTokenType::IndentComments, shape);
    if (!singleline_function) r.end_token.leading.push_back(IndentTrivia(ctx, shape));
    return r;
}

FunctionCall FormatFunctionCall(const Ctx &ctx, const FunctionCall &c, Shape shape) {
    Prefix formatted_prefix = FormatPrefix(ctx, c.prefix, shape);
    bool must_hang = HasTrailingComments(c.prefix, CS::Single);
    if (!must_hang) {
        for (size_t i = 0; i < c.suffixes.size(); i++) {
            const Suffix &s = c.suffixes[i];
            must_hang = HasLeadingComments(s, CS::All) || SuffixContainsComments(s) ||
                        (i + 1 < c.suffixes.size() && HasTrailingComments(s, CS::All));
            if (must_hang) break;
        }
    }
    auto is_call_like = [&](size_t i) {
        const Suffix &s = c.suffixes[i];
        return s.kind == Suffix::kMethod ||
               (s.kind == Suffix::kIndex && i + 1 < c.suffixes.size() && c.suffixes[i + 1].kind == Suffix::kAnon);
    };
    bool keep_first_call_inlined = false;
    bool should_hang = false;
    {
        int call_count = 0;
        for (size_t i = 0; i < c.suffixes.size(); i++) {
            if (is_call_like(i)) {
                call_count++;
                if (call_count > 1) break;
            }
        }
        if (call_count > 1) {
            std::vector<Suffix> fs;
            for (const auto &s : c.suffixes) fs.push_back(FormatSuffix(ctx, s, shape, NextNode::None));
            FunctionCall prelim;
            prelim.prefix = formatted_prefix;
            prelim.suffixes = fs;
            keep_first_call_inlined = ShouldInlinePrefix(ctx, c.prefix) &&
                                      !TestOverBudget(shape.TakeLastLine(Print(StripLead(formatted_prefix))), fs.front());
            if (shape.TakeFirstLine(Print(StripTrivia(prelim))).OverBudget()) {
                should_hang = true;
            } else {
                size_t idx_or_method = 0;
                for (const auto &s : c.suffixes)
                    if (s.kind == Suffix::kIndex || s.kind == Suffix::kMethod) idx_or_method++;
                if (keep_first_call_inlined && idx_or_method == 2) {
                    should_hang = false;
                } else {
                    bool contains_newline = false;
                    for (size_t idx = 0; idx < prelim.suffixes.size(); idx++) {
                        bool remaining_call = false;
                        for (size_t k = idx + 1; k < prelim.suffixes.size(); k++)
                            remaining_call = remaining_call || prelim.suffixes[k].IsCall();
                        if (remaining_call && prelim.suffixes[idx].IsCall() &&
                            Print(StripTrivia(prelim.suffixes[idx])).find('\n') != std::string::npos) {
                            contains_newline = true;
                            break;
                        }
                    }
                    should_hang = contains_newline;
                }
            }
        }
    }
    Shape sh = shape.TakeLastLine(Print(StripLead(formatted_prefix)));
    FunctionCall out;
    out.prefix = std::move(formatted_prefix);
    bool previous_was_index = true;
    for (size_t idx = 0; idx < c.suffixes.size(); idx++) {
        const Suffix &s = c.suffixes[idx];
        const bool will_hang = must_hang || (should_hang && is_call_like(idx) && !(keep_first_call_inlined && idx == 0));
        Shape current = sh;
        if (will_hang || (should_hang && previous_was_index && s.kind == Suffix::kAnon &&
                          !(keep_first_call_inlined && idx == 1))) {
            sh = sh.Reset();
            current = sh.IncrementAdditionalIndent();
        }
        const bool ambiguous = idx + 1 < c.suffixes.size() &&
                               (c.suffixes[idx + 1].kind == Suffix::kIndex || c.suffixes[idx + 1].kind == Suffix::kMethod);
        Suffix f = FormatSuffix(ctx, s, current, ambiguous ? NextNode::Obscure : NextNode::None);
        if (will_hang && !(previous_was_index && f.kind == Suffix::kAnon)) f = PrependNewlineIndent(ctx, f, current);
        previous_was_index = f.kind == Suffix::kIndex;
        sh = sh.TakeLastLine(Print(f));
        out.suffixes.push_back(std::move(f));
    }
    return out;
}

// ---- table.rs -----------------------------------------------------------------------

namespace {

enum class TableType { MultiLine, SingleLine, Empty };

Expr FormatFieldExpressionValue(const Ctx &ctx, const Expr &e, Shape shape) {
    const FT trailing = FT::Replace(TrailingCommentsSearch(e, CS::Multiline));
    if (CanHangExpression(e)) {
        if (HasInlineComments(e)) return UpdTrail(HangExpression(ctx, e, shape, kHang1), trailing);
        Expr single = UpdTrail(FormatExpression(ctx, e, shape), trailing);
        Expr hanging = UpdTrail(HangExpression(ctx, e, shape, kHang1), trailing);
        if (TestOverBudget(shape, single) || Lines(Print(hanging)).size() < Lines(Print(single)).size()) return hanging;
        return single;
    }
    return UpdTrail(FormatExpression(ctx, e, shape), trailing);
}

// handle_field_key_equals_comments: the key's new leading trivia, and a plain ` = `.
std::pair<std::vector<Token>, TokenRef> FieldKeyEqualsComments(const Ctx &ctx, const std::vector<Token> &key_leading,
                                                               const std::vector<Token> &key_trailing,
                                                               const TokenRef &equal, Shape shape) {
    std::vector<Token> out = key_leading;
    auto add = [&](const Token &t) {
        out.push_back(IndentTrivia(ctx, shape));
        out.push_back(t);
        out.push_back(NewlineTok(ctx));
    };
    for (const auto &t : key_trailing)
        if (t.IsComment()) add(t);
    for (const auto &t : equal.leading)
        if (t.IsComment()) add(t);
    for (const auto &t : equal.trailing)
        if (t.IsComment()) add(t);
    TokenRef eq = Sym("=");
    eq.leading = One(Spaces(1));
    eq.trailing = One(Spaces(1));
    return {std::move(out), std::move(eq)};
}

std::pair<Field, std::vector<Token>> FormatField(const Ctx &ctx, const Field &field, TableType tt, Shape shape) {
    if (ShouldFormatNodeSkip(ctx, LeadTrivia(field))) return {field, {}};
    const FT leading = tt == TableType::MultiLine ? FT::Append({IndentTrivia(ctx, shape)}) : FT::NoChange();
    Field r;
    r.kind = field.kind;
    std::vector<Token> trailing_trivia;
    switch (field.kind) {
    case Field::ExprKey: {
        trailing_trivia = TrailingCommentsSearch(*field.value, CS::Single);
        ContainedSpan brackets = FormatContainedSpan(ctx, field.brackets, shape);
        const bool space_brackets = IsBracketsString(*field.key);
        Expr key;
        if (space_brackets) {
            key = UpdTrail(UpdLead(FormatExpression(ctx, *field.key, shape + 2), FT::Append(One(Spaces(1)))),
                           FT::Append(One(Spaces(1))));
        } else {
            key = FormatExpression(ctx, *field.key, shape + 1);
        }
        auto [klead, equal] = FieldKeyEqualsComments(ctx, brackets.open.leading, brackets.close.trailing, field.equal, shape);
        brackets.open.leading = std::move(klead);
        brackets.close.trailing.clear();
        ApplyFT(brackets.open.leading, leading);
        Expr value;
        if (HasLeadingComments(*field.value, CS::Single)) {
            auto [eq, v] = HangAtEqualsDueToComments(ctx, equal, *field.value, shape);
            equal = std::move(eq);
            value = UpdTrail(v, FT::Replace(TrailingCommentsSearch(*field.value, CS::Multiline)));
        } else {
            const Shape sh = shape.TakeLastLine(Print(key)) + (2 + 3 + (space_brackets ? 2 : 0));
            value = FormatFieldExpressionValue(ctx, *field.value, sh);
        }
        r.brackets = std::move(brackets);
        r.key = std::move(key);
        r.equal = std::move(equal);
        r.value = std::move(value);
        break;
    }
    case Field::NameKey: {
        trailing_trivia = TrailingCommentsSearch(*field.value, CS::Single);
        TokenRef key = FormatTokenRef(ctx, field.name, shape);
        auto [klead, equal] = FieldKeyEqualsComments(ctx, key.leading, key.trailing, field.equal, shape);
        key.leading = std::move(klead);
        key.trailing.clear();
        ApplyFT(key.leading, leading);
        Expr value;
        if (HasLeadingComments(*field.value, CS::Single)) {
            auto [eq, v] = HangAtEqualsDueToComments(ctx, equal, *field.value, shape);
            equal = std::move(eq);
            value = UpdTrail(v, FT::Replace(TrailingCommentsSearch(*field.value, CS::Multiline)));
        } else {
            const Shape sh = shape + (Print(StripTrivia(key)).size() + 3);
            value = FormatFieldExpressionValue(ctx, *field.value, sh);
        }
        r.name = std::move(key);
        r.equal = std::move(equal);
        r.value = std::move(value);
        break;
    }
    case Field::NoKey: {
        trailing_trivia = TrailingCommentsSearch(*field.value, CS::Single);
        if (tt == TableType::MultiLine) {
            r.value = UpdLead(FormatFieldExpressionValue(ctx, *field.value, shape), leading);
        } else {
            r.value = FormatExpression(ctx, *field.value, shape);
        }
        break;
    }
    }
    return {std::move(r), std::move(trailing_trivia)};
}

ContainedSpan CreateTableBraces(const Ctx &ctx, const TokenRef &open, const TokenRef &close, TableType tt, Shape shape) {
    ContainedSpan r;
    switch (tt) {
    case TableType::MultiLine:
        r.open = FmtSymbol(ctx, open, "{", shape);
        r.open.trailing.push_back(NewlineTok(ctx));
        r.close = FormatEndToken(ctx, close, EndTokenType::IndentComments, shape);
        r.close.leading.push_back(IndentTrivia(ctx, shape));
        return r;
    case TableType::SingleLine:
        r.open = FmtSymbol(ctx, open, "{ ", shape);
        r.close = FmtSymbol(ctx, close, " }", shape);
        return r;
    case TableType::Empty: {
        r.open = FmtSymbol(ctx, open, "{", shape);
        r.close = FmtSymbol(ctx, close, "}", shape);
        std::vector<Token> a, b;
        for (const auto &t : r.open.trailing)
            if (!TriviaIsNewline(t)) a.push_back(t);
        for (const auto &t : r.close.leading)
            if (!TriviaIsNewline(t)) b.push_back(t);
        r.open.trailing = std::move(a);
        r.close.leading = std::move(b);
        return r;
    }
    }
    return r;
}

bool ExpressionIsMultilineFunction(const Expr &e) {
    return e.kind == EK::Function && !ShouldCollapseFunctionBody(*e.func->body);
}

bool TableFieldsContainComments(const TableConstructor &t) {
    for (const auto &pr : t.fields.pairs) {
        if (ContainsComments(pr.value)) return true;
        if (pr.punct && TokenContainsComments(*pr.punct)) return true;
    }
    return false;
}

bool ShouldExpand(const TableConstructor &t) {
    bool comments = false;
    for (const auto &x : t.braces.open.trailing) comments = comments || x.IsComment();
    for (const auto &x : t.braces.close.leading) comments = comments || x.IsComment();
    if (comments || TableFieldsContainComments(t)) return true;
    for (const auto &pr : t.fields.pairs) {
        const Field &f = pr.value;
        if (f.kind == Field::ExprKey && (ExpressionIsMultilineFunction(*f.key) || ExpressionIsMultilineFunction(*f.value)))
            return true;
        if (f.kind != Field::ExprKey && ExpressionIsMultilineFunction(*f.value)) return true;
    }
    return false;
}

}  // namespace

TableConstructor FormatTableConstructor(const Ctx &ctx_in, const TableConstructor &t, Shape shape) {
    const TokenRef &open = t.braces.open;
    const TokenRef &close = t.braces.close;
    TableType tt;
    if (!t.fields.empty()) {
        bool nl = false;
        for (const auto &x : open.trailing) nl = nl || TriviaIsNewline(x);
        if (nl) {
            tt = TableType::MultiLine;
        } else {
            const size_t r0 = !open.leading.empty() ? open.leading.back().end : open.tok.end;
            const size_t r1 = close.tok.start;
            const auto &last = t.fields.pairs.back();
            bool a = false, b = false;
            for (const auto &x : open.trailing) a = a || TriviaIsWhitespace(x);
            if (last.punct) {
                for (const auto &x : last.punct->trailing) b = b || TriviaIsWhitespace(x);
            } else {
                for (const auto &x : TrailTrivia(last.value)) b = b || TriviaIsWhitespace(x);
            }
            const size_t additional = (a && b) ? 0 : (a || b) ? 1 : 2;
            const Shape singleline = shape + (r1 - r0) + additional + 1;
            tt = singleline.OverBudget() ? TableType::MultiLine
                                         : (ShouldExpand(t) ? TableType::MultiLine : TableType::SingleLine);
        }
    } else {
        tt = ShouldExpand(t) ? TableType::MultiLine : TableType::Empty;
    }
    TableConstructor r;
    r.braces = CreateTableBraces(ctx_in, open, close, tt, shape);
    if (tt == TableType::Empty) return r;
    if (tt == TableType::SingleLine) {
        Shape sh = shape + 2;
        for (size_t i = 0; i < t.fields.pairs.size(); i++) {
            const auto &pr = t.fields.pairs[i];
            auto [f, trailing] = FormatField(ctx_in, pr.value, tt, sh);
            std::optional<TokenRef> punct;
            if (i + 1 < t.fields.pairs.size()) {
                sh = sh + (Print(f).size() + 2);
                punct = pr.punct ? FmtSymbol(ctx_in, *pr.punct, ", ", sh) : Sym(", ");
            }
            r.fields.push(std::move(f), std::move(punct));
        }
        return r;
    }
    Shape sh = shape.Reset().IncrementAdditionalIndent();
    Ctx ctx = ctx_in;
    for (const auto &pr : t.fields.pairs) {
        ctx = CheckToggleFormatting(ctx, LeadTrivia(pr.value));
        sh = sh.Reset().Add(1);
        auto [f, trailing] = FormatField(ctx, pr.value, tt, sh);
        if (std::all_of(trailing.begin(), trailing.end(), TriviaIsWhitespace)) {
            trailing.clear();
        } else {
            std::vector<Token> tr;
            for (const auto &x : trailing) {
                if (TriviaIsWhitespace(x)) continue;
                const auto v = TriviaToVec(FormatToken(ctx, x, FTT::Trailing, sh));
                tr.insert(tr.end(), v.begin(), v.end());
            }
            trailing = std::move(tr);
        }
        trailing.push_back(NewlineTok(ctx));
        TokenRef sym = pr.punct ? FmtSymbol(ctx, *pr.punct, ",", sh) : Sym(",");
        sym.trailing.insert(sym.trailing.end(), trailing.begin(), trailing.end());
        r.fields.push(std::move(f), std::move(sym));
    }
    return r;
}

}  // namespace luafmt::detail
