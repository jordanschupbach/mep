// rust_format_pairs.cpp -- rustfmt's pairs.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

#include <algorithm>

namespace rustfmt {

namespace {

// PairList<'_, '_, ast::Expr>: the flattened binop chain.
struct PairList {
    std::vector<std::pair<const rust::Expr*, RewriteResult>> list;
    std::vector<std::string_view> separators;
    Span span;
};

bool is_ident_or_bool_lit(const rust::Expr* expr) {
    switch (expr->kind) {
    case rust::ExprKind::Path: return expr->qself == nullptr && expr->path->segments.size() == 1;
    case rust::ExprKind::Lit: return expr->lit.kind == rust::LitKind::Bool;
    case rust::ExprKind::Unary:
    case rust::ExprKind::AddrOf:
    case rust::ExprKind::Paren:
    case rust::ExprKind::Try: return is_ident_or_bool_lit(expr->lhs);
    default: return false;
    }
}

size_t let_chain_count(const PairList& pl) {
    size_t n = 0;
    for (const auto& e : pl.list) {
        if (e.first->kind == rust::ExprKind::Let) n++;
    }
    return n;
}

bool can_rewrite_let_chain_single_line(const PairList& pl) {
    if (pl.list.size() != 2) return false;

    bool fist_item_is_ident_or_bool_lit = is_ident_or_bool_lit(pl.list[0].first);
    bool second_item_is_let_chain = pl.list[1].first->kind == rust::ExprKind::Let;

    return fist_item_is_ident_or_bool_lit && second_item_is_let_chain;
}

// impl FlattenPair for ast::Expr: flatten
std::optional<PairList> flatten(const rust::Expr* self, const RewriteContext& ctx, Shape shape) {
    if (self->kind != rust::ExprKind::Binary) return std::nullopt;
    rust::BinOpKind top_op = self->binop;

    auto default_rewrite = [&](const rust::Expr* node, size_t sep, bool is_first) -> RewriteResult {
        if (is_first) return rewrite_result(ctx, node, shape);
        size_t nested_overhead = sep + 1;
        size_t rhs_offset = shape.rhs_overhead(*ctx.config);
        // non-default config: indent_style = Visual
        Shape ns0 = shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);
        std::optional<Shape> nested_shape = ns0.sub_width(rhs_offset);
        if (!nested_shape) return Err(RewriteError::exceeds(ns0.exceeds(node->span)));
        // binop_separator = Front (default)
        std::optional<Shape> default_shape = nested_shape->offset_left(nested_overhead);
        if (!default_shape) return Err(RewriteError::exceeds(nested_shape->exceeds(node->span)));
        return rewrite_result(ctx, node, *default_shape);
    };

    // Turn a tree of binop expressions into a list using a depth-first,
    // in-order traversal.
    std::vector<const rust::Expr*> stack;
    PairList pl;
    const rust::Expr* node = self;
    pl.span = spanned(self);
    for (;;) {
        if (node->kind == rust::ExprKind::Binary && node->binop == top_op) {
            stack.push_back(node);
            node = node->lhs;
        } else {
            size_t op_len = pl.separators.empty() ? 0 : pl.separators.back().size();
            RewriteResult rw = default_rewrite(node, op_len, pl.list.empty());
            pl.list.emplace_back(node, std::move(rw));
            if (!stack.empty()) {
                const rust::Expr* pop = stack.back();
                stack.pop_back();
                if (pop->kind != rust::ExprKind::Binary) panic("flatten: unreachable");
                pl.separators.push_back(rust::binop_str(pop->binop));
                node = pop->rhs;
            } else {
                break;
            }
        }
    }

    if (pl.list.size() - 1 != pl.separators.size()) panic("flatten: separator count mismatch");
    return pl;
}

// This may return a multi-line result since we allow the last expression to go
// multiline in a 'single line' formatting.
Opt rewrite_pairs_one_line(const PairList& list, Shape shape, const RewriteContext& ctx) {
    if (list.list.size() < 2) panic("Not a pair?");

    std::string result;
    Shape base_shape = shape.block();

    size_t n = std::min(list.list.size(), list.separators.size());
    for (size_t k = 0; k < n; ++k) {
        const RewriteResult& rewrite = list.list[k].second;
        const std::string_view s = list.separators[k];
        if (rewrite.ok()) {
            if (!is_single_line(*rewrite) || result.size() > shape.width) return std::nullopt;

            result += *rewrite;
            result.push_back(' ');
            result += s;
            result.push_back(' ');
        } else {
            return std::nullopt;
        }
    }

    size_t prefix_len = result.size();
    const rust::Expr* last = list.list.back().first;
    std::optional<Shape> cur_shape = base_shape.offset_left_opt(last_line_width(result));
    if (!cur_shape) return std::nullopt;
    Opt last_rewrite = rewrite_result(ctx, last, *cur_shape).opt();
    if (!last_rewrite) return std::nullopt;
    result += *last_rewrite;

    if (first_line_width(result) > shape.width) return std::nullopt;

    // Check the last expression in the list. We sometimes let this expression
    // go over multiple lines, but we check for some ugly conditions.
    if (!(is_single_line(result) || starts_with(*last_rewrite, "{")) &&
        (starts_with(*last_rewrite, "(") || prefix_len > ctx.config->tab_spaces())) {
        return std::nullopt;
    }

    return wrap_str(std::move(result), ctx.config->max_width(), shape);
}

RewriteResult rewrite_pairs_multiline(const PairList& list, Shape shape, const RewriteContext& ctx) {
    size_t rhs_offset = shape.rhs_overhead(*ctx.config);
    // non-default config: indent_style = Visual
    Shape ns0 = shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);
    std::optional<Shape> nested_shape_o = ns0.sub_width(rhs_offset);
    if (!nested_shape_o) return Err(RewriteError::exceeds(ns0.exceeds(list.span)));
    Shape nested_shape = *nested_shape_o;

    std::string indent_str = nested_shape.indent.to_string_with_newline(*ctx.config);
    std::string result;

    if (!list.list[0].second.ok()) return Err(list.list[0].second.error());
    result += *list.list[0].second;

    size_t n = std::min(list.list.size() - 1, list.separators.size());
    for (size_t k = 0; k < n; ++k) {
        const rust::Expr* e = list.list[k + 1].first;
        const RewriteResult& default_rw = list.list[k + 1].second;
        std::string_view s = list.separators[k];
        // The following test checks if we should keep two subexprs on the same
        // line. We do this if not doing so would create an orphan and there is
        // enough space to do so.
        size_t offset = contains(result, '\n') ? 0 : shape.used_width();
        if (last_line_width(result) + offset <= nested_shape.used_width()) {
            // We must snuggle the next line onto the previous line to avoid an orphan.
            if (std::optional<Shape> line_shape = shape.offset_left_opt(s.size() + 2 + trimmed_last_line_width(result))) {
                RewriteResult rewrite = rewrite_result(ctx, e, *line_shape);
                if (rewrite.ok()) {
                    result.push_back(' ');
                    result += s;
                    result.push_back(' ');
                    result += *rewrite;
                    continue;
                }
            }
        }

        // binop_separator = Front (default)
        result += indent_str;
        result += s;
        result.push_back(' ');

        if (!default_rw.ok()) return Err(default_rw.error());
        result += *default_rw;
    }
    return RewriteResult(std::move(result));
}

}  // namespace

// Flattens a tree of pairs into a list and tries to rewrite them all at once.
// FIXME would be nice to reuse the lists API for this, but because each separator
// can be different, we can't.
RewriteResult rewrite_all_pairs(const rust::Expr* expr, Shape shape, const RewriteContext& ctx) {
    std::optional<PairList> list = flatten(expr, ctx, shape);
    if (!list) return Err(RewriteError::unknown());
    if (let_chain_count(*list) > 0 && !can_rewrite_let_chain_single_line(*list)) {
        return rewrite_pairs_multiline(*list, shape, ctx);
    }
    // First we try formatting on one line.
    Opt one = rewrite_pairs_one_line(*list, shape, ctx);
    if (one) return RewriteResult(std::move(*one));
    return rewrite_pairs_multiline(*list, shape, ctx);
}

// Rewrites a single pair.
RewriteResult rewrite_pair(const RwNode& lhs, const RwNode& rhs, PairParts pp, const RewriteContext& ctx,
                           Shape shape, SeparatorPlace separator_place) {
    size_t tab_spaces = ctx.config->tab_spaces();
    size_t lhs_overhead = 0;
    switch (separator_place) {
    case SeparatorPlace::Back: lhs_overhead = shape.used_width() + pp.prefix.size() + trim_end(pp.infix).size(); break;
    case SeparatorPlace::Front: lhs_overhead = shape.used_width(); break;
    }
    Shape lhs_shape = shape;
    lhs_shape.width = ctx.budget(lhs_overhead);
    RewriteResult lhs_rw = rewrite_result(ctx, lhs, lhs_shape);
    if (!lhs_rw) return lhs_rw;
    std::string lhs_result = std::string(pp.prefix) + *lhs_rw;

    // Try to put both lhs and rhs on the same line.
    Opt rhs_orig_result;
    if (std::optional<Shape> s = shape.offset_left_opt(last_line_width(lhs_result) + pp.infix.size())) {
        if (std::optional<Shape> rhs_shape = s->sub_width_opt(pp.suffix.size())) {
            rhs_orig_result = rewrite_result(ctx, rhs, *rhs_shape).opt();
        }
    }

    if (rhs_orig_result) {
        const std::string& rhs_result = *rhs_orig_result;
        // If the length of the lhs is equal to or shorter than the tab width or
        // the rhs looks like block expression, we put the rhs on the same
        // line with the lhs even if the rhs is multi-lined.
        bool allow_same_line = lhs_result.size() <= tab_spaces;
        if (!allow_same_line) {
            std::vector<std::string_view> ls = lines(rhs_result);
            allow_same_line = !ls.empty() && ends_with(ls.front(), "{");
        }
        if (!contains(rhs_result, '\n') || allow_same_line) {
            size_t one_line_width =
                last_line_width(lhs_result) + pp.infix.size() + first_line_width(rhs_result) + pp.suffix.size();
            if (one_line_width <= shape.width) {
                std::string out = lhs_result;
                out += pp.infix;
                out += rhs_result;
                out += pp.suffix;
                return RewriteResult(std::move(out));
            }
        }
    }

    // We have to use multiple lines.
    // Re-evaluate the rhs because we have more space now:
    // non-default config: indent_style = Visual
    // Try to calculate the initial constraint on the right hand side.
    size_t rhs_overhead = shape.rhs_overhead(*ctx.config);
    Shape rs0 = Shape::indented(shape.indent.block_indent_(*ctx.config), *ctx.config);
    std::optional<Shape> rhs_shape_o = rs0.sub_width(rhs_overhead);
    if (!rhs_shape_o) return Err(RewriteError::exceeds(rs0.exceeds(spanned(rhs))));
    Shape rhs_shape = *rhs_shape_o;
    std::string_view infix =
        separator_place == SeparatorPlace::Back ? trim_end(pp.infix) : trim_start(pp.infix);
    if (separator_place == SeparatorPlace::Front) {
        std::optional<Shape> o = rhs_shape.offset_left(infix.size());
        if (!o) return Err(RewriteError::exceeds(rhs_shape.exceeds(spanned(rhs))));
        rhs_shape = *o;
    }
    RewriteResult rhs_result = rewrite_result(ctx, rhs, rhs_shape);
    if (!rhs_result) return rhs_result;
    std::string indent_str = rhs_shape.indent.to_string_with_newline(*ctx.config);
    std::string infix_with_sep;
    if (separator_place == SeparatorPlace::Back) {
        infix_with_sep = std::string(infix) + indent_str;
    } else {
        infix_with_sep = indent_str + std::string(infix);
    }
    std::string out = lhs_result;
    out += infix_with_sep;
    out += *rhs_result;
    out += pp.suffix;
    return RewriteResult(std::move(out));
}

}  // namespace rustfmt
