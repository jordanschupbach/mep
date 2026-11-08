// rust_format_expr.cpp -- rustfmt's expr.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

#include <algorithm>

namespace rustfmt {

// `let x = e?;` for RewriteResult / Expected<T>.
#define EXPR_TRY(var, e)                                  \
    auto var##_res_ = (e);                                \
    if (!var##_res_) return Err(var##_res_.error());      \
    auto var = std::move(*var##_res_)
// `let s = shape.offset_left(n, span)?;` for the optional<Shape> forms.
#define EXPR_SHAPE(var, e, width_, span_)                                  \
    std::optional<Shape> var##_opt_ = (e);                                 \
    if (!var##_opt_) return Err(RewriteError::exceeds((width_), (span_))); \
    Shape var = *var##_opt_

namespace {

// char::is_whitespace (Unicode White_Space).
bool is_ws_char(uint32_t c) {
    return (c >= 0x9 && c <= 0xD) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

const rust::Label* label_of(const rust::Expr* e) { return e->has_label ? &e->label : nullptr; }

std::vector<OverflowableItem> items_of(const std::vector<rust::Expr*>& exprs) {
    std::vector<OverflowableItem> v;
    v.reserve(exprs.size());
    for (const rust::Expr* e : exprs) v.push_back(RwNode::of(e));
    return v;
}

const char* unop_as_str(rust::UnOp op) {
    switch (op) {
    case rust::UnOp::Deref: return "*";
    case rust::UnOp::Not: return "!";
    case rust::UnOp::Neg: return "-";
    }
    return "";
}

// impl Display for GenBlockKind
const char* gen_block_kind_str(rust::GenBlockKind k) {
    switch (k) {
    case rust::GenBlockKind::Async: return "async";
    case rust::GenBlockKind::Gen: return "gen";
    case rust::GenBlockKind::AsyncGen: return "async gen";
    }
    return "";
}

}  // namespace

// Forward declarations of file-local functions.
static Opt rewrite_empty_block(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs,
                               const rust::Label* label, std::string_view prefix, Shape shape);
static RewriteResult block_prefix(const RewriteContext& ctx, const rust::Block* block, Shape shape);
static RewriteResult rewrite_single_line_block(const RewriteContext& ctx, std::string_view prefix,
                                               const rust::Block* block, const rust::AttrVec* attrs,
                                               const rust::Label* label, Shape shape);
static RewriteResult rewrite_block(const rust::Block* block, const rust::AttrVec* attrs, const rust::Label* label,
                                   const RewriteContext& ctx, Shape shape);
static RewriteResult rewrite_block_inner(const rust::Block* block, const rust::AttrVec* attrs,
                                         const rust::Label* label, bool allow_single_line,
                                         const RewriteContext& ctx, Shape shape);
static std::string rewrite_label(const RewriteContext& ctx, const rust::Label* opt_label);
static Opt extract_comment(Span span, const RewriteContext& ctx, Shape shape);
static bool block_has_statements(const rust::Block* block);
static std::optional<SeparatorTactic> choose_separator_tactic(const RewriteContext& ctx, Span span);
static RewriteResult rewrite_paren_in_multi_line(const RewriteContext& ctx, const rust::Expr* subexpr, Shape shape,
                                                 Span pre_span, Span post_span);
static RewriteResult rewrite_index(const rust::Expr* expr, const rust::Expr* index, const RewriteContext& ctx,
                                   Shape shape);
static RewriteResult rewrite_struct_lit(const RewriteContext& ctx, const rust::Expr* expr, Shape shape);
static RewriteResult rewrite_let(const RewriteContext& ctx, Shape shape, const rust::Pat* pat,
                                 const rust::Expr* expr);
static RewriteResult rewrite_assignment(const RewriteContext& ctx, const rust::Expr* lhs, const rust::Expr* rhs,
                                        const Span* op_span, Shape shape);
static RewriteResult choose_rhs(const RewriteContext& ctx, const RwNode& expr, Shape shape,
                                const RewriteResult& orig_rhs, const RhsAssignKind& rhs_kind,
                                RhsTactics rhs_tactics, bool has_rhs_comment);
static std::optional<Shape> shape_from_rhs_tactic(const RewriteContext& ctx, Shape shape, RhsTactics rhs_tactic);
static RewriteResult rewrite_expr_addrof(const RewriteContext& ctx, rust::BorrowKind borrow_kind,
                                         rust::Mutability mutability, const rust::Expr* expr, Shape shape);

// ===========================================================================
// impl Rewrite for ast::Expr

RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Expr* x, Shape shape) {
    return format_expr(x, ExprType::SubExpression, ctx, shape);
}

bool lit_ends_in_dot(const rust::Lit& lit, const RewriteContext& ctx) {
    (void)ctx;
    switch (lit.kind) {
    case rust::LitKind::Float: return float_lit_ends_in_dot(lit.symbol, lit.suffix);
    default: return false;
    }
}

// float_literal_trailing_zero = Preserve (the default); `suffix` empty = None.
bool float_lit_ends_in_dot(std::string_view symbol, std::string_view suffix) {
    // non-default config: float_literal_trailing_zero
    return ends_with(symbol, ".") && suffix.empty();
}

// ---------------------------------------------------------------------------
// ControlFlow (internal)

namespace {

// Abstraction over control flow expressions
struct ControlFlow {
    const rust::Expr* cond = nullptr;
    const rust::Block* block = nullptr;
    const rust::Expr* else_block = nullptr;
    const rust::Label* label = nullptr;
    const rust::Pat* pat = nullptr;
    std::string_view keyword;
    std::string_view matcher;
    std::string_view connector;
    bool allow_single_line = false;
    // HACK: `true` if this is an `if` expression in an `else if`.
    bool nested_if = false;
    bool is_loop = false;
    Span span;

    static ControlFlow new_if(const rust::Expr* cond, const rust::Pat* pat, const rust::Block* block,
                              const rust::Expr* else_block, bool allow_single_line, bool nested_if, Span span);
    static ControlFlow new_loop(const rust::Block* block, const rust::Label* label, Span span);
    static ControlFlow new_while(const rust::Pat* pat, const rust::Expr* cond, const rust::Block* block,
                                 const rust::Label* label, Span span);
    static ControlFlow new_for(const rust::Pat* pat, const rust::Expr* cond, const rust::Block* block,
                               const rust::Label* label, Span span, rust::ForLoopKind kind);

    Opt rewrite_single_line(std::string_view pat_expr_str, const RewriteContext& ctx, size_t width) const;
    RewriteResult rewrite_pat_expr(const RewriteContext& ctx, const rust::Expr* expr, Shape shape,
                                   size_t offset) const;
    Expected<std::pair<std::string, size_t>> rewrite_cond(const RewriteContext& ctx, Shape shape,
                                                          std::string_view alt_block_sep) const;
    RewriteResult rewrite_result(const RewriteContext& ctx, Shape shape) const;
};

std::pair<const rust::Pat*, const rust::Expr*> extract_pats_and_cond(const rust::Expr* expr) {
    if (expr->kind == rust::ExprKind::Let) return {expr->pat, expr->lhs};
    return {nullptr, expr};
}

// FIXME: Refactor this.
std::optional<ControlFlow> to_control_flow(const rust::Expr* expr, ExprType expr_type) {
    switch (expr->kind) {
    case rust::ExprKind::If: {
        auto [pat, cond] = extract_pats_and_cond(expr->lhs);
        return ControlFlow::new_if(cond, pat, expr->block, expr->rhs, expr_type == ExprType::SubExpression, false,
                                   expr->span);
    }
    case rust::ExprKind::ForLoop:
        return ControlFlow::new_for(expr->pat, expr->lhs, expr->block, label_of(expr), expr->span, expr->for_kind);
    case rust::ExprKind::Loop: return ControlFlow::new_loop(expr->block, label_of(expr), expr->span);
    case rust::ExprKind::While: {
        auto [pat, cond] = extract_pats_and_cond(expr->lhs);
        return ControlFlow::new_while(pat, cond, expr->block, label_of(expr), expr->span);
    }
    default: return std::nullopt;
    }
}

std::string_view choose_matcher(const rust::Pat* pat) { return pat ? "let" : ""; }

ControlFlow ControlFlow::new_if(const rust::Expr* cond, const rust::Pat* pat, const rust::Block* block,
                                const rust::Expr* else_block, bool allow_single_line, bool nested_if, Span span) {
    ControlFlow cf;
    cf.cond = cond;
    cf.block = block;
    cf.else_block = else_block;
    cf.label = nullptr;
    cf.pat = pat;
    cf.keyword = "if";
    cf.matcher = choose_matcher(pat);
    cf.connector = " =";
    cf.allow_single_line = allow_single_line;
    cf.nested_if = nested_if;
    cf.is_loop = false;
    cf.span = span;
    return cf;
}

ControlFlow ControlFlow::new_loop(const rust::Block* block, const rust::Label* label, Span span) {
    ControlFlow cf;
    cf.cond = nullptr;
    cf.block = block;
    cf.else_block = nullptr;
    cf.label = label;
    cf.pat = nullptr;
    cf.keyword = "loop";
    cf.matcher = "";
    cf.connector = "";
    cf.allow_single_line = false;
    cf.nested_if = false;
    cf.is_loop = true;
    cf.span = span;
    return cf;
}

ControlFlow ControlFlow::new_while(const rust::Pat* pat, const rust::Expr* cond, const rust::Block* block,
                                   const rust::Label* label, Span span) {
    ControlFlow cf;
    cf.cond = cond;
    cf.block = block;
    cf.else_block = nullptr;
    cf.label = label;
    cf.pat = pat;
    cf.keyword = "while";
    cf.matcher = choose_matcher(pat);
    cf.connector = " =";
    cf.allow_single_line = false;
    cf.nested_if = false;
    cf.is_loop = true;
    cf.span = span;
    return cf;
}

ControlFlow ControlFlow::new_for(const rust::Pat* pat, const rust::Expr* cond, const rust::Block* block,
                                 const rust::Label* label, Span span, rust::ForLoopKind kind) {
    ControlFlow cf;
    cf.cond = cond;
    cf.block = block;
    cf.else_block = nullptr;
    cf.label = label;
    cf.pat = pat;
    cf.keyword = kind == rust::ForLoopKind::For ? "for" : "for await";
    cf.matcher = "";
    cf.connector = " in";
    cf.allow_single_line = false;
    cf.nested_if = false;
    cf.is_loop = true;
    cf.span = span;
    return cf;
}

Opt ControlFlow::rewrite_single_line(std::string_view pat_expr_str, const RewriteContext& ctx, size_t width) const {
    if (!allow_single_line) panic("assertion failed: self.allow_single_line");
    if (!else_block) return std::nullopt;
    const rust::Expr* else_blk = else_block;
    size_t fixed_cost = keyword.size() + std::string_view("  {  } else {  }").size();

    if (else_blk->kind == rust::ExprKind::Block) {
        const rust::Block* else_node = else_blk->block;
        std::optional<StmtW> if_expr = StmtW::from_simple_block(ctx, block, nullptr);
        std::optional<StmtW> else_expr = StmtW::from_simple_block(ctx, else_node, nullptr);
        if (!(if_expr && else_expr && !contains(pat_expr_str, '\n'))) return std::nullopt;

        std::optional<size_t> new_width = checked_sub(width, pat_expr_str.size() + fixed_cost);
        if (!new_width) return std::nullopt;
        Opt if_str = rustfmt::rewrite_result(ctx, *if_expr, Shape::legacy(*new_width, Indent::empty())).opt();
        if (!if_str) return std::nullopt;

        std::optional<size_t> new_width2 = checked_sub(*new_width, if_str->size());
        if (!new_width2) return std::nullopt;
        Opt else_str = rustfmt::rewrite_result(ctx, *else_expr, Shape::legacy(*new_width2, Indent::empty())).opt();
        if (!else_str) return std::nullopt;

        if (contains(*if_str, '\n') || contains(*else_str, '\n')) return std::nullopt;

        std::string result = std::string(keyword) + " " + std::string(pat_expr_str) + " { " + *if_str +
                             " } else { " + *else_str + " }";

        if (result.size() <= width) return result;
    }

    return std::nullopt;
}

/// Returns `true` if the last line of pat_str has leading whitespace and it is wider than the
/// shape's indent.
bool last_line_offsetted(size_t start_column, std::string_view pat_str) {
    size_t leading_whitespaces = 0;
    // Walking the chars backwards up to the last '\n': the count that survives is the
    // run of whitespace at the start of the last line.
    size_t nl = pat_str.rfind('\n');
    std::string_view last = nl == std::string_view::npos ? pat_str : pat_str.substr(nl + 1);
    size_t i = 0;
    while (i < last.size()) {
        size_t len = 1;
        uint32_t c = rust::decode_utf8(last, i, len);
        if (len == 0) len = 1;
        if (!is_ws_char(c)) break;
        leading_whitespaces++;
        i += len;
    }
    return leading_whitespaces > start_column;
}

RewriteResult ControlFlow::rewrite_pat_expr(const RewriteContext& ctx, const rust::Expr* expr, Shape shape,
                                            size_t offset) const {
    EXPR_SHAPE(cond_shape, shape.offset_left(offset), shape.width, expr->span);
    if (pat) {
        std::string matcher_s = matcher.empty() ? std::string(matcher) : std::string(matcher) + " ";
        std::optional<Shape> ps = cond_shape.offset_left(matcher_s.size());
        if (ps) ps = ps->sub_width(connector.size());
        if (!ps) return Err(RewriteError::exceeds(cond_shape.width, pat->span));
        EXPR_TRY(pat_string, rustfmt::rewrite_result(ctx, pat, *ps));
        uint32_t comments_lo = ctx.snippet_provider->span_after(span.with_lo(pat->span.hi), trim(connector));
        Span comments_span = mk_sp(comments_lo, expr->span.lo);
        return rewrite_assign_rhs_with_comments(ctx, matcher_s + pat_string + std::string(connector),
                                                RwNode::of(expr), cond_shape, RhsAssignKind::of_expr(expr),
                                                RhsTactics::Default, comments_span, true);
    }

    RewriteResult expr_rw = rustfmt::rewrite_result(ctx, expr, cond_shape);
    // The expression may (partially) fit on the current line.
    // We do not allow splitting between `if` and condition.
    if (keyword == "if" || expr_rw.ok()) return expr_rw;

    // The expression won't fit on the current line, jump to next.
    Shape nested_shape = shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);
    std::string nested_indent_str = nested_shape.indent.to_string_with_newline(*ctx.config);
    EXPR_TRY(r, rustfmt::rewrite_result(ctx, expr, nested_shape));
    return nested_indent_str + r;
}

Expected<std::pair<std::string, size_t>> ControlFlow::rewrite_cond(const RewriteContext& ctx, Shape shape,
                                                                   std::string_view alt_block_sep) const {
    // Do not take the rhs overhead from the upper expressions into account
    // when rewriting pattern.
    size_t new_width = ctx.budget(shape.used_width());
    Shape fresh_shape = shape;
    fresh_shape.width = new_width;
    Shape constr_shape = fresh_shape;
    if (nested_if) {
        // We are part of an if-elseif-else chain. Our constraints are tightened.
        // 7 = "} else " .len()
        std::optional<Shape> s = fresh_shape.offset_left(7);
        if (!s) return RewriteError::exceeds(fresh_shape.width, span);
        constr_shape = *s;
    }

    std::string label_string = rewrite_label(ctx, label);
    // 1 = space after keyword.
    size_t offset = keyword.size() + label_string.size() + 1;

    std::string pat_expr_string;
    if (cond) {
        RewriteResult r = rewrite_pat_expr(ctx, cond, constr_shape, offset);
        if (!r) return r.error();
        pat_expr_string = std::move(*r);
    }

    // control_brace_style = AlwaysSameLine: 2 = ` {`
    size_t brace_overhead = 2;
    size_t one_line_budget = sat_sub(ctx.config->max_width(), constr_shape.used_width() + offset + brace_overhead);
    bool force_newline_brace =
        (contains(pat_expr_string, '\n') || pat_expr_string.size() > one_line_budget) &&
        (!last_line_extendable(pat_expr_string) || last_line_offsetted(shape.used_width(), pat_expr_string));

    // Try to format if-else on single line.
    if (allow_single_line && ctx.config->single_line_if_else_max_width() > 0) {
        Opt trial = rewrite_single_line(pat_expr_string, ctx, shape.width);

        if (trial) {
            if (trial->size() <= ctx.config->single_line_if_else_max_width()) {
                return std::make_pair(std::move(*trial), size_t(0));
            }
        }
    }

    Span cond_span = cond ? cond->span : mk_sp(block->span.lo, block->span.lo);

    // `for event in event`
    // Do not include label in the span.
    uint32_t lo = label ? label->ident.span.hi : span.lo;
    uint32_t between_hi;
    if (!pat) {
        between_hi = cond_span.lo;
    } else if (matcher.empty()) {
        between_hi = pat->span.lo;
    } else {
        between_hi = ctx.snippet_provider->span_before(span, trim(matcher));
    }
    Span between_kwd_cond = mk_sp(ctx.snippet_provider->span_after(mk_sp(lo, span.hi), trim(keyword)), between_hi);

    Opt between_kwd_cond_comment = extract_comment(between_kwd_cond, ctx, shape);

    Opt after_cond_comment = extract_comment(mk_sp(cond_span.hi, block->span.lo), ctx, shape);

    std::string_view block_sep;
    if (!cond && between_kwd_cond_comment) {
        block_sep = "";
    } else if (force_newline_brace) {  // control_brace_style = AlwaysSameLine
        block_sep = alt_block_sep;
    } else {
        block_sep = " ";
    }

    size_t used_width;
    if (contains(pat_expr_string, '\n')) {
        used_width = last_line_width(pat_expr_string);
    } else {
        // 2 = spaces after keyword and condition.
        used_width = label_string.size() + keyword.size() + pat_expr_string.size() + 2;
    }

    std::string result = label_string;
    result += keyword;
    if (between_kwd_cond_comment) {
        result += *between_kwd_cond_comment;
    } else if (!(pat_expr_string.empty() || starts_with(pat_expr_string, "\n"))) {
        result += " ";
    }
    result += pat_expr_string;
    if (after_cond_comment) {
        result += *after_cond_comment;
    } else {
        result += block_sep;
    }
    return std::make_pair(std::move(result), used_width);
}

// impl Rewrite for ControlFlow
RewriteResult ControlFlow::rewrite_result(const RewriteContext& ctx, Shape shape) const {
    std::string alt_block_sep = shape.indent.to_string_with_newline(*ctx.config);
    Expected<std::pair<std::string, size_t>> cond_r = rewrite_cond(ctx, shape, alt_block_sep);
    if (!cond_r) return Err(cond_r.error());
    std::string cond_str = std::move(cond_r->first);
    size_t used_width = cond_r->second;
    // If `used_width` is 0, it indicates that whole control flow is written in a single line.
    if (used_width == 0) return cond_str;

    size_t block_width = sat_sub(shape.width, used_width);
    // This is used only for the empty block case: `{}`. So, we use 1 if we know
    // we should avoid the single line case.
    if (else_block || nested_if) block_width = std::min<size_t>(1, block_width);
    Shape block_shape = shape;
    block_shape.width = block_width;
    RewriteResult block_r(std::string{});
    {
        bool old_val = ctx.is_if_else_block_;
        ctx.is_if_else_block_ = else_block != nullptr;
        bool old_is_loop = ctx.is_loop_block_;
        ctx.is_loop_block_ = is_loop;
        block_r = rewrite_block_with_visitor(ctx, "", block, nullptr, nullptr, block_shape, true);
        ctx.is_loop_block_ = old_is_loop;
        ctx.is_if_else_block_ = old_val;
    }
    if (!block_r) return block_r;

    std::string result = cond_str + *block_r;

    if (else_block) {
        Shape shape2 = Shape::indented(shape.indent, *ctx.config);
        bool last_in_chain = false;
        RewriteResult rw(std::string{});
        if (else_block->kind == rust::ExprKind::If) {
            // If the else expression is another if-else expression, prevent it
            // from being formatted on a single line.
            // Note how we're passing the original shape, as the
            // cost of "else" should not cascade.
            auto [pats, cond2] = extract_pats_and_cond(else_block->lhs);
            rw = ControlFlow::new_if(cond2, pats, else_block->block, else_block->rhs, false, true,
                                     mk_sp(else_block->span.lo, span.hi))
                     .rewrite_result(ctx, shape2);
        } else {
            last_in_chain = true;
            // When rewriting a block, the width is only used for single line
            // blocks, passing 1 lets us avoid that.
            Shape else_shape = shape2;
            else_shape.width = std::min<size_t>(1, shape2.width);
            rw = format_expr(else_block, ExprType::Statement, ctx, else_shape);
        }

        std::string else_kw = rewrite_else_kw_with_comments(false, last_in_chain, ctx,
                                                             block->span.between(else_block->span), shape2);
        result += else_kw;
        if (!rw) return rw;
        result += *rw;
    }

    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// format_expr

static RewriteResult format_expr_kind(const rust::Expr* expr, ExprType expr_type, const RewriteContext& ctx,
                                      Shape shape) {
    using K = rust::ExprKind;
    const Config& config = *ctx.config;
    switch (expr->kind) {
    case K::Array:
        return rewrite_array("", items_of(expr->exprs), expr->span, ctx, shape,
                             choose_separator_tactic(ctx, expr->span), std::nullopt);
    case K::Lit: {
        RewriteResult rw = rewrite_literal(ctx, expr->lit, expr->span, shape);
        if (rw) return rw;
        if (expr->lit.kind == rust::LitKind::StrRaw) return std::string(trim(ctx.snippet(expr->span)));
        return Err(RewriteError::unknown());
    }
    case K::Call: {
        const rust::Expr* callee = expr->lhs;
        Span inner_span = mk_sp(callee->span.hi, expr->span.hi);
        EXPR_TRY(callee_str, rustfmt::rewrite_result(ctx, callee, shape));
        return rewrite_call(ctx, callee_str, expr->exprs, inner_span, shape);
    }
    case K::Move: {
        Span inner_span = mk_sp(expr->kw_span.hi, expr->span.hi);
        std::vector<rust::Expr*> args{expr->lhs};
        return rewrite_call(ctx, "move", args, inner_span, shape);
    }
    case K::Paren: return rewrite_paren(ctx, expr->lhs, shape, expr->span);
    case K::Binary: {
        // FIXME: format comments between operands and operator
        RewriteResult rw = rewrite_all_pairs(expr, shape, ctx);
        if (rw) return rw;
        std::string infix = " " + std::string(ctx.snippet(expr->op_span)) + " ";
        // binop_separator = Front
        return rewrite_pair(RwNode::of(expr->lhs), RwNode::of(expr->rhs), PairParts::infix_(infix), ctx, shape,
                            SeparatorPlace::Front);
    }
    case K::Unary:
        // rewrite_unary_op: for some reason, an UnOp is not spanned like BinOp!
        return rewrite_unary_prefix(ctx, unop_as_str(expr->unop), RwNode::of(expr->lhs), shape);
    case K::Struct: return rewrite_struct_lit(ctx, expr, shape);
    case K::Tup: return rewrite_tuple(ctx, items_of(expr->exprs), expr->span, shape, expr->exprs.size() == 1);
    case K::Let: return rewrite_let(ctx, shape, expr->pat, expr->lhs);
    case K::If:
    case K::ForLoop:
    case K::Loop:
    case K::While: {
        std::optional<ControlFlow> cf = to_control_flow(expr, expr_type);
        if (!cf) return Err(RewriteError::unknown());
        return cf->rewrite_result(ctx, shape);
    }
    case K::ConstBlock: {
        const rust::Expr* value = expr->anon_const.value;
        std::string rw;
        if (value && value->kind == K::Block) {
            // Inner attributes are associated with the `ast::ExprKind::ConstBlock` node,
            // not the `ast::Block` node we're about to rewrite. To prevent dropping inner
            // attributes call `rewrite_block` directly.
            // See https://github.com/rust-lang/rustfmt/issues/6158
            EXPR_TRY(r, rewrite_block(value->block, &expr->attrs, label_of(value), ctx, shape));
            rw = std::move(r);
        } else {
            EXPR_TRY(r, rustfmt::rewrite_result(ctx, &expr->anon_const, shape));
            rw = std::move(r);
        }
        return "const " + rw;
    }
    case K::Block: {
        const rust::Block* block = expr->block;
        const rust::Label* opt_label = label_of(expr);
        switch (expr_type) {
        case ExprType::Statement:
            if (is_unsafe_block(block)) {
                return rewrite_block(block, &expr->attrs, opt_label, ctx, shape);
            } else if (Opt rw = rewrite_empty_block(ctx, block, &expr->attrs, opt_label, "", shape)) {
                // Rewrite block without trying to put it in a single line.
                return std::move(*rw);
            } else {
                EXPR_TRY(prefix, block_prefix(ctx, block, shape));
                return rewrite_block_with_visitor(ctx, prefix, block, &expr->attrs, opt_label, shape, true);
            }
        case ExprType::SubExpression: return rewrite_block(block, &expr->attrs, opt_label, ctx, shape);
        }
        return Err(RewriteError::unknown());
    }
    case K::Match: return rewrite_match(ctx, expr->lhs, expr->arms, shape, expr->span, expr->attrs, expr->match_kind);
    case K::Path: return rewrite_path(ctx, PathContext::Expr, expr->qself, expr->path, shape);
    case K::Assign: return rewrite_assignment(ctx, expr->lhs, expr->rhs, nullptr, shape);
    case K::AssignOp: return rewrite_assignment(ctx, expr->lhs, expr->rhs, &expr->op_span, shape);
    case K::Continue: {
        std::string id_str;
        if (expr->has_label) {
            // Ident lose the `r#` prefix in raw labels, so use the original snippet
            id_str = " " + std::string(ctx.snippet(expr->label.ident.span));
        }
        return "continue" + id_str;
    }
    case K::Break: {
        std::string id_str;
        if (expr->has_label) {
            // Ident lose the `r#` prefix in raw labels, so use the original snippet
            id_str = " " + std::string(ctx.snippet(expr->label.ident.span));
        }
        if (expr->lhs) return rewrite_unary_prefix(ctx, "break" + id_str + " ", RwNode::of(expr->lhs), shape);
        return "break" + id_str;
    }
    case K::Yield:
        if (expr->yield_kind == rust::YieldKind::Postfix) return rewrite_chain(expr, ctx, shape);
        if (expr->lhs) return rewrite_unary_prefix(ctx, "yield ", RwNode::of(expr->lhs), shape);
        return std::string("yield");
    case K::Closure: return rewrite_closure(expr->closure, expr->span, ctx, shape);
    case K::Try:
    case K::Field:
    case K::MethodCall:
    case K::Await:
    case K::Use: return rewrite_chain(expr, ctx, shape);
    case K::MacCall: {
        RewriteResult rw = rewrite_macro(expr->mac, ctx, shape, MacroPosition::Expression, expr->span);
        if (rw) return rw;
        return max_width_error(wrap_str(std::string(ctx.snippet(expr->span)), config.max_width(), shape),
                               shape.width, expr->span);
    }
    case K::Ret:
        if (!expr->lhs) return std::string("return");
        return rewrite_unary_prefix(ctx, "return ", RwNode::of(expr->lhs), shape);
    case K::Become: return rewrite_unary_prefix(ctx, "become ", RwNode::of(expr->lhs), shape);
    case K::Yeet:
        if (!expr->lhs) return std::string("do yeet");
        return rewrite_unary_prefix(ctx, "do yeet ", RwNode::of(expr->lhs), shape);
    case K::AddrOf: return rewrite_expr_addrof(ctx, expr->borrow, expr->mutbl, expr->lhs, shape);
    case K::Cast:
        return rewrite_pair(RwNode::of(expr->lhs), RwNode::of(expr->ty), PairParts::infix_(" as "), ctx, shape,
                            SeparatorPlace::Front);
    case K::Index: return rewrite_index(expr->lhs, expr->rhs, ctx, shape);
    case K::Repeat:
        return rewrite_pair(RwNode::of(expr->lhs), RwNode::of(expr->anon_const.value), PairParts{"[", "; ", "]"},
                            ctx, shape, SeparatorPlace::Back);
    case K::Range: {
        const char* delim = expr->limits == rust::RangeLimits::HalfOpen ? ".." : "..=";

        struct RangeHelpers {
            static bool needs_space_before_range(const RewriteContext& c, const rust::Expr* lhs) {
                switch (lhs->kind) {
                case rust::ExprKind::Lit: return lit_ends_in_dot(lhs->lit, c);
                case rust::ExprKind::Unary: return needs_space_before_range(c, lhs->lhs);
                case rust::ExprKind::Binary: return needs_space_before_range(c, lhs->rhs);
                default: return false;
                }
            }
            // Don't format `.. ..` into `....`, which is invalid.
            //
            // This check is unnecessary for `lhs`, because a range
            // starting from another range needs parentheses as `(x ..) ..`
            // (`x .. ..` is a range from `x` to `..`).
            static bool needs_space_after_range(const rust::Expr* rhs) {
                return rhs->kind == rust::ExprKind::Range && rhs->lhs == nullptr;
            }
        };

        auto default_sp_delim = [&](const rust::Expr* lhs, const rust::Expr* rhs) {
            std::string s;
            if (lhs && RangeHelpers::needs_space_before_range(ctx, lhs)) s += " ";
            s += delim;
            if (rhs && RangeHelpers::needs_space_after_range(rhs)) s += " ";
            return s;
        };

        const rust::Expr* lhs = expr->lhs;
        const rust::Expr* rhs = expr->rhs;
        // non-default config: spaces_around_ranges
        if (lhs && rhs) {
            std::string sp_delim = default_sp_delim(lhs, rhs);
            return rewrite_pair(RwNode::of(lhs), RwNode::of(rhs), PairParts::infix_(sp_delim), ctx, shape,
                                SeparatorPlace::Front);
        }
        if (!lhs && rhs) {
            std::string sp_delim = default_sp_delim(nullptr, rhs);
            return rewrite_unary_prefix(ctx, sp_delim, RwNode::of(rhs), shape);
        }
        if (lhs && !rhs) {
            std::string sp_delim = default_sp_delim(lhs, nullptr);
            return rewrite_unary_suffix(ctx, sp_delim, RwNode::of(lhs), shape);
        }
        return std::string(delim);
    }
    // We do not format these expressions yet, but they should still
    // satisfy our width restrictions.
    // Style Guide RFC for InlineAsm variant pending
    // https://github.com/rust-dev-tools/fmt-rfcs/issues/152
    case K::InlineAsm: return std::string(ctx.snippet(expr->span));
    case K::TryBlock: {
        const rust::Block* block = expr->block;
        if (!expr->ty) {
            RewriteResult rw = rewrite_single_line_block(ctx, "try ", block, &expr->attrs, nullptr, shape);
            if (rw) return rw;
            // FIXME: 9 sounds like `"do catch ".len()`, so may predate the rename
            // 9 = `try `
            size_t budget = sat_sub(shape.width, 9);
            EXPR_TRY(b, rewrite_block(block, &expr->attrs, nullptr, ctx, Shape::legacy(budget, shape.indent)));
            return "try " + b;
        }
        std::string_view keyword = "try bikeshed ";
        // 2 = " {".len()
        std::optional<Shape> ty_shape_o = shape.shrink_left(keyword.size());
        if (ty_shape_o) ty_shape_o = ty_shape_o->sub_width(2);
        if (!ty_shape_o) return Err(RewriteError::exceeds(shape.width, expr->span));
        EXPR_TRY(ty_str, rustfmt::rewrite_result(ctx, expr->ty, *ty_shape_o));
        std::string prefix = std::string(keyword) + ty_str + " ";
        RewriteResult rw = rewrite_single_line_block(ctx, prefix, block, &expr->attrs, nullptr, shape);
        if (rw) return rw;
        size_t budget = sat_sub(shape.width, prefix.size());
        EXPR_TRY(b, rewrite_block(block, &expr->attrs, nullptr, ctx, Shape::legacy(budget, shape.indent)));
        return prefix + b;
    }
    case K::Gen: {
        const char* mover = expr->capture == rust::CaptureBy::Value ? "move " : "";
        std::string kind_prefix = std::string(gen_block_kind_str(expr->gen_kind)) + " " + mover;
        RewriteResult rw = rewrite_single_line_block(ctx, kind_prefix, expr->block, &expr->attrs, nullptr, shape);
        if (rw) return rw;
        // 6 = `async `
        size_t budget = sat_sub(shape.width, 6);
        EXPR_TRY(b, rewrite_block(expr->block, &expr->attrs, nullptr, ctx, Shape::legacy(budget, shape.indent)));
        return kind_prefix + b;
    }
    case K::Underscore: return std::string("_");
    case K::FormatArgs:
    case K::Type:
    case K::IncludedBytes:
    case K::OffsetOf:
    case K::UnsafeBinderCast:
        // These don't normally occur in the AST because macros aren't expanded. However,
        // rustfmt tries to parse macro arguments when formatting macros, so it's not totally
        // impossible for rustfmt to come across one of these nodes when formatting a file.
        // Also, rustfmt might get passed the output from `-Zunpretty=expanded`.
        return Err(RewriteError::unknown());
    case K::Err:
    case K::Dummy: return Err(RewriteError::unknown());
    }
    return Err(RewriteError::unknown());
}

RewriteResult format_expr(const rust::Expr* expr, ExprType expr_type, const RewriteContext& ctx, Shape shape) {
    DepthGuard g(*ctx.state);
    // skip_out_of_file_lines_range_err!: file_lines is the default (everything), never skips.

    if (contains_skip(ctx, expr->attrs)) return std::string(ctx.snippet(spanned(expr)));
    if (expr_type == ExprType::Statement && semicolon_for_expr(ctx, expr)) {
        EXPR_SHAPE(s, shape.sub_width(1), shape.width, expr->span);
        shape = s;
    }

    RewriteResult expr_rw = format_expr_kind(expr, expr_type, ctx, shape);
    if (!expr_rw) return expr_rw;
    std::string expr_str = recover_comment_removed(std::move(*expr_rw), expr->span, ctx);
    rust::AttrVec attrs = outer_attributes(expr->attrs);
    EXPR_TRY(attrs_str, rewrite_attrs(ctx, attrs, shape));
    Span span = mk_sp(attrs.empty() ? expr->span.lo : attrs.back().span.hi, expr->span.lo);
    return combine_strs_with_missing_comments(ctx, attrs_str, expr_str, span, shape, false);
}

RewriteResult rewrite_array(std::string_view name, const std::vector<OverflowableItem>& exprs, Span span,
                            const RewriteContext& ctx, Shape shape,
                            std::optional<SeparatorTactic> force_separator_tactic,
                            std::optional<Delimiter> delim_token) {
    return rewrite_with_square_brackets(ctx, name, exprs, shape, span, force_separator_tactic, delim_token);
}

static Opt rewrite_empty_block(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs,
                               const rust::Label* label, std::string_view prefix, Shape shape) {
    if (block_has_statements(block)) return std::nullopt;

    std::string label_str = rewrite_label(ctx, label);
    if (attrs && !inner_attributes(*attrs).empty()) return std::nullopt;

    if (!block_contains_comment(ctx, block) && shape.width >= 2) {
        return std::string(prefix) + label_str + "{}";
    }

    // If a block contains only a single-line comment, then leave it on one line.
    std::string_view user_str = trim(ctx.snippet(block->span));
    if (starts_with(user_str, "{") && ends_with(user_str, "}")) {
        std::string_view comment_str = trim(user_str.substr(1, user_str.size() - 2));
        if (block->stmts.empty() && !contains(comment_str, '\n') && !starts_with(comment_str, "//") &&
            comment_str.size() + 4 <= shape.width) {
            return std::string(prefix) + label_str + "{ " + std::string(comment_str) + " }";
        }
    }

    return std::nullopt;
}

static RewriteResult block_prefix(const RewriteContext& ctx, const rust::Block* block, Shape shape) {
    if (block->rules == rust::BlockCheckMode::Unsafe) {
        std::string_view snippet = ctx.snippet(block->span);
        std::optional<size_t> open_pos = find_uncommented(snippet, "{");
        if (!open_pos) return Err(RewriteError::unknown());
        if (*open_pos < 6) panic("block_prefix: slice index starts at 6 but ends before it", block->span.lo);
        // Extract comment between unsafe and block start.
        std::string_view trimmed = trim(snippet.substr(6, *open_pos - 6));

        if (!trimmed.empty()) {
            // 9 = "unsafe  {".len(), 7 = "unsafe ".len()
            std::optional<size_t> budget = checked_sub(shape.width, 9);
            if (!budget) return Err(RewriteError::exceeds(shape.width, block->span));
            EXPR_TRY(c, rewrite_comment(trimmed, true, Shape::legacy(*budget, shape.indent + 7), *ctx.config));
            return "unsafe " + c + " ";
        }
        return std::string("unsafe ");
    }
    return std::string();
}

static RewriteResult rewrite_single_line_block(const RewriteContext& ctx, std::string_view prefix,
                                               const rust::Block* block, const rust::AttrVec* attrs,
                                               const rust::Label* label, Shape shape) {
    if (std::optional<StmtW> block_expr = StmtW::from_simple_block(ctx, block, attrs)) {
        EXPR_SHAPE(expr_shape, shape.offset_left(last_line_width(prefix)), shape.width, spanned(*block_expr));
        EXPR_TRY(expr_str, rustfmt::rewrite_result(ctx, *block_expr, expr_shape));
        std::string label_str = rewrite_label(ctx, label);
        std::string result = std::string(prefix) + label_str + "{ " + expr_str + " }";
        if (result.size() <= shape.width && !contains(result, '\n')) return result;
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_block_with_visitor(const RewriteContext& ctx, std::string_view prefix, const rust::Block* block,
                                         const rust::AttrVec* attrs, const rust::Label* label, Shape shape,
                                         bool has_braces) {
    if (Opt rw_str = rewrite_empty_block(ctx, block, attrs, label, prefix, shape)) return std::move(*rw_str);

    FmtVisitor visitor = FmtVisitor::from_context(ctx);
    visitor.block_indent = shape.indent;
    visitor.is_if_else_block = ctx.is_if_else_block();
    visitor.is_loop_block = ctx.is_loop_block();
    if (block->rules == rust::BlockCheckMode::Unsafe || label != nullptr) {
        std::string_view snippet = ctx.snippet(block->span);
        std::optional<size_t> open_pos = find_uncommented(snippet, "{");
        if (!open_pos) return Err(RewriteError::unknown());
        visitor.last_pos = block->span.lo + static_cast<uint32_t>(*open_pos);
    } else {
        visitor.last_pos = block->span.lo;
    }

    std::optional<rust::AttrVec> inner_attrs;
    if (attrs) inner_attrs = inner_attributes(*attrs);
    std::string label_str = rewrite_label(ctx, label);
    visitor.visit_block(block, inner_attrs ? &*inner_attrs : nullptr, has_braces);
    // context.skipped_range.append(visitor.get_context().skipped_range)
    if (visitor.skipped_range && ctx.skipped_range && visitor.skipped_range != ctx.skipped_range) {
        ctx.skipped_range->insert(ctx.skipped_range->end(), visitor.skipped_range->begin(),
                                  visitor.skipped_range->end());
        visitor.skipped_range->clear();
    }
    return std::string(prefix) + label_str + visitor.buffer;
}

// impl Rewrite for ast::Block
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Block* x, Shape shape) {
    return rewrite_block(x, nullptr, nullptr, ctx, shape);
}

static RewriteResult rewrite_block(const rust::Block* block, const rust::AttrVec* attrs, const rust::Label* label,
                                   const RewriteContext& ctx, Shape shape) {
    return rewrite_block_inner(block, attrs, label, true, ctx, shape);
}

static RewriteResult rewrite_block_inner(const rust::Block* block, const rust::AttrVec* attrs,
                                         const rust::Label* label, bool allow_single_line,
                                         const RewriteContext& ctx, Shape shape) {
    EXPR_TRY(prefix, block_prefix(ctx, block, shape));

    // shape.width is used only for the single line case: either the empty block `{}`,
    // or an unsafe expression `unsafe { e }`.
    if (Opt rw_str = rewrite_empty_block(ctx, block, attrs, label, prefix, shape)) return std::move(*rw_str);

    EXPR_TRY(result_str, rewrite_block_with_visitor(ctx, prefix, block, attrs, label, shape, true));
    if (allow_single_line && lines(result_str).size() <= 3) {
        RewriteResult rw = rewrite_single_line_block(ctx, prefix, block, attrs, label, shape);
        if (rw) return rw;
    }
    return result_str;
}

/// Rewrite the divergent block of a `let-else` statement.
RewriteResult rewrite_let_else_block(const rust::Block* block, bool allow_single_line, const RewriteContext& ctx,
                                     Shape shape) {
    return rewrite_block_inner(block, nullptr, nullptr, allow_single_line, ctx, shape);
}

// Rewrite condition if the given expression has one.
Opt rewrite_cond(const RewriteContext& ctx, const rust::Expr* expr, Shape shape) {
    if (expr->kind == rust::ExprKind::Match && expr->match_kind == rust::MatchKind::Prefix) {
        // `match `cond` {`
        std::optional<Shape> cond_shape = shape.offset_left_opt(8);
        if (!cond_shape) return std::nullopt;
        return rustfmt::rewrite_result(ctx, expr->lhs, *cond_shape).opt();
    }
    std::optional<ControlFlow> control_flow = to_control_flow(expr, ExprType::SubExpression);
    if (!control_flow) return std::nullopt;
    std::string alt_block_sep = "\n" + shape.indent.block_only().to_string(*ctx.config);
    Expected<std::pair<std::string, size_t>> r = control_flow->rewrite_cond(ctx, shape, alt_block_sep);
    if (!r) return std::nullopt;
    return std::move(r->first);
}

/// Rewrite the `else` keyword with surrounding comments.
///
/// force_newline_else: whether or not to rewrite the `else` keyword on a newline.
/// is_last: true if this is an `else` and `false` if this is an `else if` block.
/// span: Span between the end of the last expression and the start of the else block,
///       which contains the `else` keyword
std::string rewrite_else_kw_with_comments(bool force_newline_else, bool is_last, const RewriteContext& ctx,
                                          Span span, Shape shape) {
    (void)is_last;
    uint32_t else_kw_lo = ctx.snippet_provider->span_before(span, "else");
    Span before_else_kw = mk_sp(span.lo, else_kw_lo);
    Opt before_else_kw_comment = extract_comment(before_else_kw, ctx, shape);

    uint32_t else_kw_hi = ctx.snippet_provider->span_after(span, "else");
    Span after_else_kw = mk_sp(else_kw_hi, span.hi);
    Opt after_else_kw_comment = extract_comment(after_else_kw, ctx, shape);

    std::string newline_sep = shape.indent.to_string_with_newline(*ctx.config);
    // control_brace_style = AlwaysSameLine
    std::string before_sep = force_newline_else ? newline_sep : std::string(" ");
    std::string after_sep = " ";

    return (before_else_kw_comment ? *before_else_kw_comment : before_sep) + "else" +
           (after_else_kw_comment ? *after_else_kw_comment : after_sep);
}

static std::string rewrite_label(const RewriteContext& ctx, const rust::Label* opt_label) {
    if (opt_label) return std::string(ctx.snippet(opt_label->ident.span)) + ": ";
    return std::string();
}

static Opt extract_comment(Span span, const RewriteContext& ctx, Shape shape) {
    RewriteResult r = rewrite_missing_comment(span, shape, ctx);
    if (r && !r->empty()) {
        std::string indent = shape.indent.to_string_with_newline(*ctx.config);
        return indent + *r + indent;
    }
    return std::nullopt;
}

bool block_contains_comment(const RewriteContext& ctx, const rust::Block* block) {
    return contains_comment(ctx.snippet(block->span));
}

// Checks that a block contains no statements, an expression and no comments or
// attributes.
// FIXME: incorrectly returns false when comment is contained completely within
// the expression.
bool is_simple_block(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs) {
    return block->stmts.size() == 1 && stmt_is_expr(block->stmts[0]) && !block_contains_comment(ctx, block) &&
           (!attrs || attrs->empty());
}

/// Checks whether a block contains at most one statement or expression, and no
/// comments or attributes.
bool is_simple_block_stmt(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs) {
    return block->stmts.size() <= 1 && !block_contains_comment(ctx, block) && (!attrs || attrs->empty());
}

static bool block_has_statements(const rust::Block* block) {
    for (const rust::Stmt* stmt : block->stmts)
        if (stmt->kind != rust::StmtKind::Empty) return true;
    return false;
}

/// Checks whether a block contains no statements, expressions, comments, or
/// inner attributes.
bool is_empty_block(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs) {
    return !block_has_statements(block) && !block_contains_comment(ctx, block) &&
           (!attrs || inner_attributes(*attrs).empty());
}

bool stmt_is_expr(const rust::Stmt* stmt) { return stmt->kind == rust::StmtKind::Expr; }

bool is_unsafe_block(const rust::Block* block) { return block->rules == rust::BlockCheckMode::Unsafe; }

static RewriteResult rewrite_string_lit(const RewriteContext& ctx, Span span, Shape shape) {
    std::string_view string_lit = ctx.snippet(span);

    // non-default config: format_strings
    std::vector<std::string_view> ls = lines(string_lit);
    bool all_continued = true;
    for (size_t i = 0; i + 1 < ls.size(); i++) {
        if (!ends_with(ls[i], "\\")) {
            all_continued = false;
            break;
        }
    }
    if (all_continued && ctx.config->style_edition() >= StyleEdition::E2024) return std::string(string_lit);
    return max_width_error(wrap_str(std::string(string_lit), ctx.config->max_width(), shape), shape.width, span);
}

static bool is_semantic_float(const rust::Lit& lit) {
    switch (lit.kind) {
    case rust::LitKind::Float: return true;
    case rust::LitKind::Integer: return lit.suffix == "f32" || lit.suffix == "f64";
    default: return false;
    }
}

static RewriteResult rewrite_float_lit(const RewriteContext& ctx, const rust::Lit& lit, Span span, Shape shape) {
    (void)lit;
    // float_literal_trailing_zero = Preserve
    // non-default config: float_literal_trailing_zero
    return max_width_error(wrap_str(std::string(ctx.snippet(span)), ctx.config->max_width(), shape), shape.width,
                           span);
}

static RewriteResult rewrite_int_lit(const RewriteContext& ctx, const rust::Lit& lit, Span span, Shape shape) {
    if (is_semantic_float(lit)) return rewrite_float_lit(ctx, lit, span, shape);
    // hex_literal_case = Preserve: no rewrite of `0x` literals.
    // non-default config: hex_literal_case
    return max_width_error(wrap_str(std::string(ctx.snippet(span)), ctx.config->max_width(), shape), shape.width,
                           span);
}

RewriteResult rewrite_literal(const RewriteContext& ctx, const rust::Lit& lit, Span span, Shape shape) {
    switch (lit.kind) {
    case rust::LitKind::Str: return rewrite_string_lit(ctx, span, shape);
    case rust::LitKind::Integer: return rewrite_int_lit(ctx, lit, span, shape);
    case rust::LitKind::Float: return rewrite_float_lit(ctx, lit, span, shape);
    default:
        return max_width_error(wrap_str(std::string(ctx.snippet(span)), ctx.config->max_width(), shape),
                               shape.width, span);
    }
}

static std::optional<SeparatorTactic> choose_separator_tactic(const RewriteContext& ctx, Span span) {
    if (ctx.inside_macro()) {
        if (span_ends_with_comma(ctx, span)) return SeparatorTactic::Always;
        return SeparatorTactic::Never;
    }
    return std::nullopt;
}

RewriteResult rewrite_call(const RewriteContext& ctx, std::string_view callee, const std::vector<rust::Expr*>& args,
                           Span span, Shape shape) {
    return rewrite_with_parens(ctx, callee, items_of(args), shape, span, ctx.config->fn_call_width(),
                               choose_separator_tactic(ctx, span));
}

bool is_simple_expr(const rust::Expr* expr) {
    using K = rust::ExprKind;
    switch (expr->kind) {
    case K::Lit: return true;
    case K::Path: return expr->qself == nullptr && expr->path->segments.size() <= 1;
    case K::AddrOf:
    case K::Cast:
    case K::Field:
    case K::Try:
    case K::Unary: return is_simple_expr(expr->lhs);
    case K::Index: return is_simple_expr(expr->lhs) && is_simple_expr(expr->rhs);
    case K::Repeat: return is_simple_expr(expr->lhs) && is_simple_expr(expr->anon_const.value);
    default: return false;
    }
}

bool is_every_expr_simple(const std::vector<OverflowableItem>& lists) {
    for (const OverflowableItem& it : lists)
        if (!overflowable_is_simple(it)) return false;
    return true;
}

bool can_be_overflowed_expr(const RewriteContext& ctx, const rust::Expr* expr, size_t args_len) {
    using K = rust::ExprKind;
    if (!expr->attrs.empty()) return false;
    switch (expr->kind) {
    case K::Match:
        // non-default config: indent_style = Visual, overflow_delimited_expr
        return ctx.use_block_indent() && args_len == 1;
    case K::If:
    case K::ForLoop:
    case K::Loop:
    case K::While: return ctx.config->combine_control_expr() && ctx.use_block_indent() && args_len == 1;

    // Handle always block-like expressions
    case K::Gen:
    case K::Block:
    case K::Closure:
    case K::TryBlock: return true;

    // Handle `[]` and `{}`-like expressions
    case K::Array:
    case K::Struct: return ctx.config->overflow_delimited_expr() || (ctx.use_block_indent() && args_len == 1);
    case K::MacCall:
        // non-default config: overflow_delimited_expr (Bracket / Brace delimiters)
        return ctx.use_block_indent() && args_len == 1;

    // Handle parenthetical expressions
    case K::Call:
    case K::MethodCall:
    case K::Tup: return ctx.use_block_indent() && args_len == 1;

    // Handle unary-like expressions
    case K::AddrOf:
    case K::Try:
    case K::Unary:
    case K::Cast: return can_be_overflowed_expr(ctx, expr->lhs, args_len);
    default: return false;
    }
}

bool is_nested_call(const rust::Expr* expr) {
    using K = rust::ExprKind;
    switch (expr->kind) {
    case K::Call:
    case K::MacCall: return true;
    case K::AddrOf:
    case K::Try:
    case K::Unary:
    case K::Cast: return is_nested_call(expr->lhs);
    default: return false;
    }
}

/// Returns `true` if a function call or a method call represented by the given span ends with a
/// trailing comma. This function is used when rewriting macro, as adding or removing a trailing
/// comma from macro can potentially break the code.
bool span_ends_with_comma(const RewriteContext& ctx, Span span) {
    bool result = false;
    uint32_t prev_char = 0;
    auto is_closing = [](uint32_t c) { return c == ')' || c == '}' || c == ']'; };

    for (const ClassifiedChar& cc : char_classes(ctx.snippet(span))) {
        uint32_t c = cc.ch;
        if (fcck_is_comment(cc.kind) || is_ws_char(c)) continue;
        if (is_closing(c)) {
            result = result && !is_closing(prev_char);
        } else if (c == ',') {
            result = true;
        } else {
            result = false;
        }
        prev_char = c;
    }

    return result;
}

RewriteResult rewrite_paren(const RewriteContext& ctx, const rust::Expr* subexpr, Shape shape, Span span) {
    // Extract comments within parens.
    Span pre_span;
    Span post_span;
    std::string pre_comment;
    std::string post_comment;
    bool remove_nested_parens = ctx.config->remove_nested_parens();
    for (;;) {
        // 1 = "(" or ")"
        pre_span = mk_sp(span.lo + 1, spanned(subexpr).lo);
        post_span = mk_sp(subexpr->span.hi, span.hi - 1);
        {
            EXPR_TRY(pc, rewrite_missing_comment(pre_span, shape, ctx));
            pre_comment = std::move(pc);
        }
        {
            EXPR_TRY(pc, rewrite_missing_comment(post_span, shape, ctx));
            post_comment = std::move(pc);
        }

        // Remove nested parens if there are no comments.
        if (subexpr->kind == rust::ExprKind::Paren) {
            if (remove_nested_parens && pre_comment.empty() && post_comment.empty()) {
                span = subexpr->span;
                subexpr = subexpr->lhs;
                continue;
            }
        }

        break;
    }

    // 1 = `(` and `)`
    std::optional<Shape> sub_shape = shape.offset_left(1);
    if (sub_shape) sub_shape = sub_shape->sub_width(1);
    if (!sub_shape) return Err(RewriteError::exceeds(shape.width, span));
    EXPR_TRY(subexpr_str, rustfmt::rewrite_result(ctx, subexpr, *sub_shape));
    bool fits_single_line = !contains(pre_comment, "//") && !contains(post_comment, "//");
    if (fits_single_line) return "(" + pre_comment + subexpr_str + post_comment + ")";
    return rewrite_paren_in_multi_line(ctx, subexpr, shape, pre_span, post_span);
}

static RewriteResult rewrite_paren_in_multi_line(const RewriteContext& ctx, const rust::Expr* subexpr, Shape shape,
                                                 Span pre_span, Span post_span) {
    const Config& config = *ctx.config;
    Indent nested_indent = shape.indent.block_indent_(config);
    Shape nested_shape = Shape::indented(nested_indent, config);
    EXPR_TRY(pre_comment, rewrite_missing_comment(pre_span, nested_shape, ctx));
    EXPR_TRY(post_comment, rewrite_missing_comment(post_span, nested_shape, ctx));
    EXPR_TRY(subexpr_str, rustfmt::rewrite_result(ctx, subexpr, nested_shape));

    std::string result;
    result.reserve(subexpr_str.size() * 2);
    result.push_back('(');
    if (!pre_comment.empty()) {
        result += nested_indent.to_string_with_newline(config);
        result += pre_comment;
    }
    result += nested_indent.to_string_with_newline(config);
    result += subexpr_str;
    if (!post_comment.empty()) {
        result += nested_indent.to_string_with_newline(config);
        result += post_comment;
    }
    result += shape.indent.to_string_with_newline(config);
    result.push_back(')');

    return result;
}

static RewriteResult rewrite_index(const rust::Expr* expr, const rust::Expr* index, const RewriteContext& ctx,
                                   Shape shape) {
    const Config& config = *ctx.config;
    EXPR_TRY(expr_str, rustfmt::rewrite_result(ctx, expr, shape));

    size_t offset = last_line_width(expr_str) + 1;
    size_t rhs_overhead = shape.rhs_overhead(config);
    std::optional<Shape> index_shape;
    if (contains(expr_str, '\n')) {
        index_shape = Shape::legacy(config.max_width(), shape.indent).offset_left(offset);
        if (index_shape) index_shape = index_shape->sub_width(1 + rhs_overhead);
    } else {
        // indent_style = Block
        index_shape = shape.offset_left(offset);
        if (index_shape) index_shape = index_shape->sub_width(1);
    }
    RewriteResult orig_index_rw = index_shape ? rustfmt::rewrite_result(ctx, index, *index_shape)
                                              : RewriteResult(RewriteError::exceeds(shape.width, spanned(index)));

    // Return if index fits in a single line.
    if (orig_index_rw && !contains(*orig_index_rw, '\n')) return expr_str + "[" + *orig_index_rw + "]";

    // Try putting index on the next line and see if it fits in a single line.
    Indent indent = shape.indent.block_indent_(config);
    std::optional<Shape> index_shape2 = Shape::indented(indent, config).offset_left(1);
    if (index_shape2) index_shape2 = index_shape2->sub_width(1 + rhs_overhead);
    if (!index_shape2) return Err(RewriteError::exceeds(shape.width, spanned(index)));
    RewriteResult new_index_rw = rustfmt::rewrite_result(ctx, index, *index_shape2);
    if (new_index_rw && !contains(*new_index_rw, '\n'))
        return expr_str + indent.to_string_with_newline(config) + "[" + *new_index_rw + "]";
    if (!orig_index_rw && new_index_rw)
        return expr_str + indent.to_string_with_newline(config) + "[" + *new_index_rw + "]";
    if (orig_index_rw) return expr_str + "[" + *orig_index_rw + "]";
    // When both orig_index_rw and new_index_rw result in errors, we currently propagate the
    // error from the second attempt since it is more generous with width constraints.
    // This decision is somewhat arbitrary and is open to change.
    return new_index_rw;
}

namespace {
struct StructLitField {
    enum Kind : uint8_t { Regular, Base, Rest } kind = Regular;
    const rust::ExprField* field = nullptr;  // Regular
    const rust::Expr* base = nullptr;        // Base
    Span rest_span;                          // Rest
};
}  // namespace

static RewriteResult rewrite_struct_lit(const RewriteContext& ctx, const rust::Expr* expr, Shape shape) {
    const rust::Path* path = expr->path;
    const rust::QSelf* qself = expr->qself;
    const std::vector<rust::ExprField>& fields = expr->struct_fields;
    const rust::AttrVec& attrs = expr->attrs;
    Span span = expr->span;

    // 2 = " {".len()
    EXPR_SHAPE(path_shape, shape.sub_width(2), shape.width, span);
    EXPR_TRY(path_str, rewrite_path(ctx, PathContext::Expr, qself, path, path_shape));

    bool has_base_or_rest;
    switch (expr->rest) {
    case rust::StructRestKind::None:
        if (fields.empty()) return path_str + " {}";
        has_base_or_rest = false;
        break;
    case rust::StructRestKind::Rest:
        if (fields.empty()) return path_str + " { .. }";
        has_base_or_rest = true;
        break;
    case rust::StructRestKind::Base: has_base_or_rest = true; break;
    default: has_base_or_rest = false; break;
    }

    // Foo { a: Foo } - indent is +3, width is -5.
    auto shapes = struct_lit_shape(shape, ctx, path_str.size() + 3, 2);
    if (!shapes) return Err(RewriteError::exceeds(shape.width, span));
    std::optional<Shape> h_shape = shapes->first;
    Shape v_shape = shapes->second;

    size_t one_line_width = h_shape ? h_shape->width : 0;
    uint32_t body_lo = ctx.snippet_provider->span_after(span, "{");
    // non-default config: struct_field_align_threshold > 0 (rewrite_with_alignment)
    std::vector<StructLitField> field_iter;
    for (const rust::ExprField& f : fields) {
        StructLitField it;
        it.kind = StructLitField::Regular;
        it.field = &f;
        field_iter.push_back(it);
    }
    if (expr->rest == rust::StructRestKind::Base) {
        StructLitField it;
        it.kind = StructLitField::Base;
        it.base = expr->rhs;
        field_iter.push_back(it);
    } else if (expr->rest == rust::StructRestKind::Rest) {
        StructLitField it;
        it.kind = StructLitField::Rest;
        it.rest_span = expr->rest_span;
        field_iter.push_back(it);
    }

    auto span_lo = [&](size_t i) -> uint32_t {
        const StructLitField& item = field_iter[i];
        switch (item.kind) {
        case StructLitField::Regular: return spanned(item.field).lo;
        case StructLitField::Base: {
            uint32_t last_field_hi = fields.empty() ? span.lo : fields.back().span.hi;
            std::string_view snippet = ctx.snippet(mk_sp(last_field_hi, item.base->span.lo));
            std::optional<size_t> pos = find_uncommented(snippet, "..");
            if (!pos) panic("called `Option::unwrap()` on a `None` value (struct base `..`)", last_field_hi);
            return last_field_hi + static_cast<uint32_t>(*pos);
        }
        case StructLitField::Rest: return item.rest_span.lo;
        }
        return 0;
    };
    auto span_hi = [&](size_t i) -> uint32_t {
        const StructLitField& item = field_iter[i];
        switch (item.kind) {
        case StructLitField::Regular: return spanned(item.field).hi;
        case StructLitField::Base: return item.base->span.hi;
        case StructLitField::Rest: return item.rest_span.hi;
        }
        return 0;
    };
    auto rewrite = [&](size_t i) -> RewriteResult {
        const StructLitField& item = field_iter[i];
        switch (item.kind) {
        case StructLitField::Regular: {
            // The 1 taken from the v_budget is for the comma.
            EXPR_SHAPE(s, v_shape.sub_width(1), v_shape.width, span);
            return rewrite_field(ctx, item.field, s, 0);
        }
        case StructLitField::Base: {
            // 2 = ..
            EXPR_SHAPE(s, v_shape.offset_left(2), v_shape.width, span);
            EXPR_TRY(r, rustfmt::rewrite_result(ctx, item.base, s));
            return ".." + r;
        }
        case StructLitField::Rest: return std::string("..");
        }
        return Err(RewriteError::unknown());
    };

    std::vector<ListItem> item_vec =
        itemize_list(*ctx.snippet_provider, field_iter.size(), "}", ",", span_lo, span_hi, rewrite, body_lo, span.hi,
                     false);

    DefinitiveListTactic tactic = struct_lit_tactic(h_shape, ctx, item_vec);
    Shape nested_shape = shape_for_tactic(tactic, h_shape, v_shape);

    bool ends_with_comma = span_ends_with_comma(ctx, span);
    bool force_no_trailing_comma = ctx.inside_macro() && !ends_with_comma;

    ListFormatting fmt = struct_lit_formatting(nested_shape, tactic, ctx,
                                               force_no_trailing_comma || has_base_or_rest || !ctx.use_block_indent());

    EXPR_TRY(fields_str, write_list(item_vec, fmt));

    EXPR_TRY(wrapped, wrap_struct_field(ctx, attrs, fields_str, shape, v_shape, one_line_width));
    return path_str + " {" + wrapped + "}";

    // FIXME if context.config.indent_style() == Visual, but we run out
    // of space, we should fall back to BlockIndent.
}

RewriteResult wrap_struct_field(const RewriteContext& ctx, const rust::AttrVec& attrs, std::string_view fields_str,
                                Shape shape, Shape nested_shape, size_t one_line_width) {
    const Config& config = *ctx.config;
    // indent_style = Block
    bool should_vertical =
        contains(fields_str, '\n') || !config.struct_lit_single_line() || fields_str.size() > one_line_width;

    rust::AttrVec inner_attrs = inner_attributes(attrs);
    if (inner_attrs.empty()) {
        if (should_vertical) {
            return nested_shape.indent.to_string_with_newline(config) + std::string(fields_str) +
                   shape.indent.to_string_with_newline(config);
        }
        // One liner or visual indent.
        return " " + std::string(fields_str) + " ";
    }
    EXPR_TRY(ia, rewrite_attrs(ctx, inner_attrs, shape));
    return nested_shape.indent.to_string_with_newline(config) + ia +
           nested_shape.indent.to_string_with_newline(config) + std::string(fields_str) +
           shape.indent.to_string_with_newline(config);
}

const char* struct_lit_field_separator(const Config& config) { return colon_spaces(config); }

RewriteResult rewrite_field(const RewriteContext& ctx, const rust::ExprField* field, Shape shape,
                            size_t prefix_max_width) {
    const Config& config = *ctx.config;
    if (contains_skip(ctx, field->attrs)) return std::string(ctx.snippet(spanned(field)));
    EXPR_TRY(attrs_str, rewrite_attrs(ctx, field->attrs, shape));
    if (!attrs_str.empty()) attrs_str += shape.indent.to_string_with_newline(config);
    std::string_view name = ctx.snippet(field->ident.span);
    if (field->is_shorthand) return attrs_str + std::string(name);

    std::string separator = struct_lit_field_separator(config);
    for (size_t i = 0; i < sat_sub(prefix_max_width, name.size()); i++) separator.push_back(' ');
    size_t overhead = name.size() + separator.size();
    EXPR_SHAPE(expr_shape, shape.offset_left(overhead), shape.width, field->span);
    RewriteResult e = rustfmt::rewrite_result(ctx, field->expr, expr_shape);
    // non-default config: use_field_init_shorthand
    if (e) return attrs_str + std::string(name) + separator + *e;
    Indent expr_offset = shape.indent.block_indent_(config);
    EXPR_TRY(s, rustfmt::rewrite_result(ctx, field->expr, Shape::indented(expr_offset, config)));
    return attrs_str + std::string(name) + ":\n" + expr_offset.to_string(config) + s;
}

// impl Rewrite for ast::ExprField (vertical.rs's AlignedItem::rewrite_aligned_item with no alignment).
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::ExprField* x, Shape shape) {
    return rewrite_field(ctx, x, shape, 0);
}

static RewriteResult rewrite_let(const RewriteContext& ctx, Shape shape, const rust::Pat* pat,
                                 const rust::Expr* expr) {
    std::string result = "let ";

    // TODO(ytmimi) comments could appear between `let` and the `pat`

    // 4 = "let ".len()
    EXPR_SHAPE(pat_shape, shape.offset_left(4), shape.width, pat->span);
    // style_edition 2027: pat_shape.sub_width(2) -- never taken.
    EXPR_TRY(pat_str, rustfmt::rewrite_result(ctx, pat, pat_shape));
    result += pat_str;

    // TODO(ytmimi) comments could appear between `pat` and `=`
    result += " =";

    uint32_t comments_lo = ctx.snippet_provider->span_after(expr->span.with_lo(pat->span.hi), "=");
    Span comments_span = mk_sp(comments_lo, expr->span.lo);
    return rewrite_assign_rhs_with_comments(ctx, std::move(result), RwNode::of(expr), shape,
                                            RhsAssignKind::of_expr(expr), RhsTactics::Default, comments_span, true);
}

RewriteResult rewrite_tuple(const RewriteContext& ctx, const std::vector<OverflowableItem>& items, Span span,
                            Shape shape, bool is_singleton_tuple) {
    // use_block_indent() is always true (indent_style = Block):
    // non-default config: rewrite_tuple_in_visual_indent_style
    // We use the same rule as function calls for rewriting tuples.
    std::optional<SeparatorTactic> force_tactic;
    if (ctx.inside_macro()) {
        force_tactic = span_ends_with_comma(ctx, span) ? SeparatorTactic::Always : SeparatorTactic::Never;
    } else if (is_singleton_tuple) {
        force_tactic = SeparatorTactic::Always;
    }
    return rewrite_with_parens(ctx, "", items, shape, span, ctx.config->fn_call_width(), force_tactic);
}

RewriteResult rewrite_unary_prefix(const RewriteContext& ctx, std::string_view prefix, const RwNode& rewrite,
                                   Shape shape) {
    EXPR_SHAPE(s, shape.offset_left(prefix.size()), shape.width, spanned(rewrite));
    EXPR_TRY(r, rustfmt::rewrite_result(ctx, rewrite, s));
    return std::string(prefix) + r;
}

// FIXME: this is probably not correct for multi-line Rewrites. we should
// subtract suffix.len() from the last line budget, not the first!
RewriteResult rewrite_unary_suffix(const RewriteContext& ctx, std::string_view suffix, const RwNode& rewrite,
                                   Shape shape) {
    EXPR_SHAPE(s, shape.sub_width(suffix.size()), shape.width, spanned(rewrite));
    EXPR_TRY(r, rustfmt::rewrite_result(ctx, rewrite, s));
    r += suffix;
    return r;
}

static RewriteResult rewrite_assignment(const RewriteContext& ctx, const rust::Expr* lhs, const rust::Expr* rhs,
                                        const Span* op_span, Shape shape) {
    std::string_view operator_str = op_span ? ctx.snippet(*op_span) : std::string_view("=");

    // 1 = space between lhs and operator.
    EXPR_SHAPE(lhs_shape, shape.sub_width(operator_str.size() + 1), shape.width, spanned(lhs));
    EXPR_TRY(l, rustfmt::rewrite_result(ctx, lhs, lhs_shape));
    std::string lhs_str = l + " " + std::string(operator_str);

    return rewrite_assign_rhs(ctx, std::move(lhs_str), RwNode::of(rhs), RhsAssignKind::of_expr(rhs), shape);
}

// The left hand side must contain everything up to, and including, the
// assignment operator.
RewriteResult rewrite_assign_rhs(const RewriteContext& ctx, std::string lhs, const RwNode& ex,
                                 const RhsAssignKind& rhs_kind, Shape shape) {
    return rewrite_assign_rhs_with(ctx, std::move(lhs), ex, shape, rhs_kind, RhsTactics::Default);
}

RewriteResult rewrite_assign_rhs_expr(const RewriteContext& ctx, std::string_view lhs, const RwNode& ex, Shape shape,
                                      const RhsAssignKind& rhs_kind, RhsTactics rhs_tactics) {
    size_t llw = sat_sub(last_line_width(lhs), contains(lhs, '\n') ? shape.indent.width() : 0);
    // 1 = space between operator and rhs.
    std::optional<Shape> os = shape.offset_left_opt(llw + 1);
    Shape orig_shape = os ? *os : Shape{0, shape.indent, shape.offset + llw + 1};
    bool has_rhs_comment = false;
    if (std::optional<size_t> offset = find_last_uncommented(lhs, "=")) {
        has_rhs_comment = trim_end(lhs).size() > *offset + 1;
    }

    RewriteResult orig_rhs = rustfmt::rewrite_result(ctx, ex, orig_shape);
    return choose_rhs(ctx, ex, orig_shape, orig_rhs, rhs_kind, rhs_tactics, has_rhs_comment);
}

RewriteResult rewrite_assign_rhs_with(const RewriteContext& ctx, std::string lhs, const RwNode& ex, Shape shape,
                                      const RhsAssignKind& rhs_kind, RhsTactics rhs_tactics) {
    EXPR_TRY(rhs, rewrite_assign_rhs_expr(ctx, lhs, ex, shape, rhs_kind, rhs_tactics));
    return lhs + rhs;
}

RewriteResult rewrite_assign_rhs_with_comments(const RewriteContext& ctx, std::string lhs, const RwNode& ex,
                                               Shape shape, const RhsAssignKind& rhs_kind, RhsTactics rhs_tactics,
                                               Span between_span, bool allow_extend) {
    bool has_comment = contains_comment(ctx.snippet(between_span));
    if (has_comment) {
        EXPR_SHAPE(s, shape.block_left(ctx.config->tab_spaces()), shape.width,
                   between_span.with_hi(spanned(ex).hi));
        shape = s;
    }
    EXPR_TRY(rhs, rewrite_assign_rhs_expr(ctx, lhs, ex, shape, rhs_kind, rhs_tactics));
    if (has_comment) {
        std::string_view r = trim_start(rhs);
        return combine_strs_with_missing_comments(ctx, lhs, r, between_span, shape, allow_extend);
    }
    return lhs + rhs;
}

static RewriteResult choose_rhs(const RewriteContext& ctx, const RwNode& expr, Shape shape,
                                const RewriteResult& orig_rhs, const RhsAssignKind& rhs_kind,
                                RhsTactics rhs_tactics, bool has_rhs_comment) {
    (void)rhs_kind;
    const Config& config = *ctx.config;
    if (orig_rhs && orig_rhs->empty()) return std::string();
    if (orig_rhs && !contains(*orig_rhs, '\n') && unicode_str_width(*orig_rhs) <= shape.width)
        return " " + *orig_rhs;

    // Expression did not fit on the same line as the identifier.
    // Try splitting the line and see if that works better.
    std::optional<Shape> new_shape = shape_from_rhs_tactic(ctx, shape, rhs_tactics);
    if (!new_shape) return Err(RewriteError::unknown());
    RewriteResult new_rhs = rustfmt::rewrite_result(ctx, expr, *new_shape);
    std::string new_indent_str = shape.indent.block_indent_(config).to_string_with_newline(config);
    std::string before_space_str = has_rhs_comment ? "" : " ";

    if (orig_rhs && new_rhs && !filtered_str_fits(*new_rhs, config.max_width(), *new_shape))
        return before_space_str + *orig_rhs;
    if (orig_rhs && new_rhs && prefer_next_line(*orig_rhs, *new_rhs, rhs_tactics)) return new_indent_str + *new_rhs;
    if (!orig_rhs && new_rhs) return new_indent_str + *new_rhs;
    if (!orig_rhs && !new_rhs && rhs_tactics == RhsTactics::AllowOverflow) {
        Shape inf = shape.infinite_width();
        EXPR_TRY(s, rustfmt::rewrite_result(ctx, expr, inf));
        return before_space_str + s;
    }
    // When both orig_rhs and new_rhs result in errors, we currently propagate
    // the error from the second attempt since it is more generous with
    // width constraints. This decision is somewhat arbitrary and is open to change.
    if (!orig_rhs && !new_rhs) return new_rhs;
    return before_space_str + *orig_rhs;
}

static std::optional<Shape> shape_from_rhs_tactic(const RewriteContext& ctx, Shape shape, RhsTactics rhs_tactic) {
    const Config& config = *ctx.config;
    switch (rhs_tactic) {
    case RhsTactics::ForceNextLineWithoutIndent:
        return shape.with_max_width(config).sub_width_opt(shape.indent.width());
    case RhsTactics::Default:
    case RhsTactics::AllowOverflow:
        return Shape::indented(shape.indent.block_indent_(config), config).sub_width_opt(shape.rhs_overhead(config));
    }
    return std::nullopt;
}

/// Returns true if formatting next_line_rhs is better on a new line when compared to the
/// original's line formatting.
///
/// It is considered better if:
/// 1. the tactic is ForceNextLineWithoutIndent
/// 2. next_line_rhs doesn't have newlines
/// 3. the original line has more newlines than next_line_rhs
/// 4. the original formatting of the first line ends with `(`, `{`, or `[` and next_line_rhs
///    doesn't
bool prefer_next_line(std::string_view orig_rhs, std::string_view next_line_rhs, RhsTactics rhs_tactics) {
    return rhs_tactics == RhsTactics::ForceNextLineWithoutIndent || !contains(next_line_rhs, '\n') ||
           count_newlines(orig_rhs) > count_newlines(next_line_rhs) + 1 ||
           (first_line_ends_with(orig_rhs, '(') && !first_line_ends_with(next_line_rhs, '(')) ||
           (first_line_ends_with(orig_rhs, '{') && !first_line_ends_with(next_line_rhs, '{')) ||
           (first_line_ends_with(orig_rhs, '[') && !first_line_ends_with(next_line_rhs, '['));
}

static RewriteResult rewrite_expr_addrof(const RewriteContext& ctx, rust::BorrowKind borrow_kind,
                                         rust::Mutability mutability, const rust::Expr* expr, Shape shape) {
    const char* operator_str = "&";
    bool m = mutability == rust::Mutability::Mut;
    switch (borrow_kind) {
    case rust::BorrowKind::Ref: operator_str = m ? "&mut " : "&"; break;
    case rust::BorrowKind::Pin: operator_str = m ? "&pin mut " : "&pin const "; break;
    case rust::BorrowKind::Raw: operator_str = m ? "&raw mut " : "&raw const "; break;
    }
    return rewrite_unary_prefix(ctx, operator_str, RwNode::of(expr), shape);
}

bool is_method_call(const rust::Expr* expr) {
    using K = rust::ExprKind;
    switch (expr->kind) {
    case K::MethodCall: return true;
    case K::AddrOf:
    case K::Cast:
    case K::Try:
    case K::Unary: return is_method_call(expr->lhs);
    default: return false;
    }
}

#undef EXPR_TRY
#undef EXPR_SHAPE

}  // namespace rustfmt
