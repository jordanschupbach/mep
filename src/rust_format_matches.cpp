// rust_format_matches.cpp -- rustfmt's matches.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//! Format match expression.
#include "rust_format_internal.h"

#include <algorithm>

namespace rustfmt {

#define MATCH_TRY(var, e)                             \
    auto var##_res_ = (e);                            \
    if (!var##_res_) return Err(var##_res_.error());  \
    auto var = std::move(*var##_res_)
#define MATCH_SHAPE(var, e, width_, span_)                                 \
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

/// A simple wrapper type against `ast::Arm`. Used inside `write_list()`.
struct ArmWrapper {
    const rust::Arm* arm = nullptr;
    /// `true` if the arm is the last one in match expression. Used to decide on whether we should
    /// add trailing comma to the match arm when `config.trailing_comma() == Never`.
    bool is_last = false;
    /// Holds a byte position of `|` at the beginning of the arm pattern, if available.
    std::optional<uint32_t> beginning_vert;
};

// impl Spanned for ArmWrapper
Span spanned_arm(const ArmWrapper& a) {
    Span arm_span = spanned(a.arm);
    if (a.beginning_vert) {
        uint32_t lo = std::min(*a.beginning_vert, arm_span.lo);
        return mk_sp(lo, arm_span.hi);
    }
    return arm_span;
}

RewriteResult rewrite_match_arms(const RewriteContext& ctx, const std::vector<rust::Arm>& arms, Shape shape,
                                 Span span, uint32_t open_brace_pos);
RewriteResult rewrite_match_arm(const RewriteContext& ctx, const rust::Arm* arm, Shape shape, bool is_last,
                                bool has_leading_pipe);
RewriteResult rewrite_match_body(const RewriteContext& ctx, const rust::Expr* body, std::string_view pats_str,
                                 Shape shape, bool has_guard, Span arrow_span, bool is_last);
RewriteResult rewrite_guard(const RewriteContext& ctx, const rust::Guard* guard, Shape shape, size_t pattern_width,
                            bool multiline_pattern);
RewriteResult nop_block_collapse(RewriteResult block_str, size_t budget);
bool can_flatten_block_around_this(const rust::Expr* body);

// impl Rewrite for ArmWrapper
RewriteResult rewrite_arm_wrapper(const RewriteContext& ctx, const ArmWrapper& a, Shape shape) {
    return rewrite_match_arm(ctx, a.arm, shape, a.is_last, a.beginning_vert.has_value());
}

}  // namespace

RewriteResult rewrite_match(const RewriteContext& ctx, const rust::Expr* cond, const std::vector<rust::Arm>& arms,
                            Shape shape, Span span, const rust::AttrVec& attrs, rust::MatchKind match_kind) {
    const Config& config = *ctx.config;
    // Do not take the rhs overhead from the upper expressions into account
    // when rewriting match condition.
    Shape cond_shape0 = shape;
    cond_shape0.width = ctx.budget(shape.used_width());
    // 6 = `match `
    // indent_style = Block
    MATCH_SHAPE(cond_shape, cond_shape0.offset_left(6), cond_shape0.width, span);
    MATCH_TRY(cond_str, rewrite_result(ctx, cond, cond_shape));
    std::string alt_block_sep = shape.indent.to_string_with_newline(config);
    std::string block_sep;
    // control_brace_style = AlwaysSameLine
    if (last_line_extendable(cond_str)) {
        block_sep = " ";
    } else if (contains(cond_str, '\n') || cond_str.size() + 2 > cond_shape.width) {
        // 2 = ` {`
        block_sep = alt_block_sep;
    } else {
        block_sep = " ";
    }

    // match_arm_indent = true
    Indent nested_indent = shape.indent.block_indent_(config);
    std::string nested_indent_str = nested_indent.to_string(config);

    // Inner attributes.
    rust::AttrVec inner_attrs = inner_attributes(attrs);
    std::string inner_attrs_str;
    if (!inner_attrs.empty()) {
        Shape s = config.style_edition() <= StyleEdition::E2021 ? shape : shape.block_indent(config.tab_spaces());
        MATCH_TRY(r, rewrite_attrs(ctx, inner_attrs, s));
        inner_attrs_str = nested_indent_str + r + "\n";
    }

    uint32_t open_brace_pos;
    if (inner_attrs.empty()) {
        uint32_t hi = arms.empty() ? span.hi : spanned(&arms[0]).lo;
        open_brace_pos = ctx.snippet_provider->span_after(mk_sp(cond->span.hi, hi), "{");
    } else {
        open_brace_pos = inner_attrs.back().span.hi;
    }

    if (arms.empty()) {
        std::string_view snippet = ctx.snippet(mk_sp(open_brace_pos, span.hi - 1));
        if (trim(snippet).empty()) return "match " + cond_str + " {}";
        // Empty match with comments or inner attributes? We are not going to bother, sorry ;)
        return std::string(ctx.snippet(span));
    }

    Span span_after_cond = mk_sp(cond->span.hi, span.hi);

    MATCH_TRY(arms_str, rewrite_match_arms(ctx, arms, shape, span_after_cond, open_brace_pos));
    std::string tail = "{\n" + inner_attrs_str + nested_indent_str + arms_str + "\n" + shape.indent.to_string(config) + "}";
    switch (match_kind) {
    case rust::MatchKind::Prefix: return "match " + cond_str + block_sep + tail;
    case rust::MatchKind::Postfix: return cond_str + ".match" + block_sep + tail;
    }
    return Err(RewriteError::unknown());
}

namespace {

const char* arm_comma(const Config& config, const rust::Expr* body, bool is_last) {
    (void)is_last;
    // trailing_comma = Vertical: the `is_last && trailing_comma == Never` case never applies.
    if (config.match_block_trailing_comma()) return ",";
    if (body->kind == rust::ExprKind::Block) {
        if (body->block->rules == rust::BlockCheckMode::Default) return "";
        return ",";
    }
    return ",";
}

/// Collect a byte position of the beginning `|` for each arm, if available.
std::vector<std::optional<uint32_t>> collect_beginning_verts(const RewriteContext& ctx,
                                                             const std::vector<rust::Arm>& arms) {
    std::vector<std::optional<uint32_t>> v;
    v.reserve(arms.size());
    for (const rust::Arm& a : arms) {
        if (starts_with(ctx.snippet(a.pat->span), "|"))
            v.push_back(spanned(a.pat).lo);
        else
            v.push_back(std::nullopt);
    }
    return v;
}

RewriteResult rewrite_match_arms(const RewriteContext& ctx, const std::vector<rust::Arm>& arms, Shape shape,
                                 Span span, uint32_t open_brace_pos) {
    // match_arm_indent = true
    Shape arm_shape = shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);

    size_t arm_len = arms.size();
    std::vector<std::optional<uint32_t>> beginning_verts = collect_beginning_verts(ctx, arms);
    std::vector<ArmWrapper> wrappers;
    wrappers.reserve(arm_len);
    for (size_t i = 0; i < arm_len; i++) {
        bool is_last = i >= sat_sub(arm_len, 1);
        wrappers.push_back(ArmWrapper{&arms[i], is_last, beginning_verts[i]});
    }
    std::vector<ListItem> arms_vec = itemize_list(
        *ctx.snippet_provider, wrappers.size(), "}", "|", [&](size_t i) { return spanned_arm(wrappers[i]).lo; },
        [&](size_t i) { return spanned_arm(wrappers[i]).hi; },
        [&](size_t i) { return rewrite_arm_wrapper(ctx, wrappers[i], arm_shape); }, open_brace_pos, span.hi,
        false);
    // We will add/remove commas inside `arm.rewrite()`, and hence no separator here.
    ListFormatting fmt = ListFormatting(arm_shape, *ctx.config).separator("").preserve_newline(true);

    return write_list(arms_vec, fmt);
}

std::pair<bool, const rust::Expr*> flatten_arm_body(const RewriteContext& ctx, const rust::Expr* body,
                                                    std::optional<Shape> opt_shape);

RewriteResult rewrite_match_arm(const RewriteContext& ctx, const rust::Arm* arm, Shape shape, bool is_last,
                                bool has_leading_pipe) {
    (void)has_leading_pipe;
    const Config& config = *ctx.config;
    Span missing_span;
    std::string attrs_str;
    if (!arm->attrs.empty()) {
        if (contains_skip(ctx, arm->attrs)) {
            if (!arm->body) return Err(RewriteError::unknown());
            const rust::Expr* body = flatten_arm_body(ctx, arm->body, std::nullopt).second;
            // `arm.span()` does not include trailing comma, add it manually.
            return std::string(ctx.snippet(spanned(arm))) + arm_comma(config, body, is_last);
        }
        missing_span = mk_sp(arm->attrs.back().span.hi, arm->pat->span.lo);
        MATCH_TRY(a, rewrite_attrs(ctx, arm->attrs, shape));
        attrs_str = std::move(a);
    } else {
        Span s = spanned(arm);
        missing_span = mk_sp(s.lo, s.lo);
    }

    // Leading pipe offset
    // 2 = `| `
    // match_arm_leading_pipes = Never
    // non-default config: match_arm_leading_pipes
    size_t pipe_offset = 0;
    std::string_view pipe_str = "";

    // Patterns
    if (!arm->body) return Err(RewriteError::unknown());
    const rust::Expr* body = arm->body;
    std::optional<Shape> pat_shape_o;
    if (body->kind == rust::ExprKind::Block && body->has_label) {
        // Some block with a label ` => 'label: {`
        // 7 = ` => : {`
        size_t label_len = body->label.ident.name.size();
        pat_shape_o = shape.sub_width(7 + label_len);
        if (pat_shape_o) pat_shape_o = pat_shape_o->offset_left(pipe_offset);
    } else {
        // 5 = ` => {`
        pat_shape_o = shape.sub_width(5);
        if (pat_shape_o) pat_shape_o = pat_shape_o->offset_left(pipe_offset);
    }
    if (!pat_shape_o) return Err(RewriteError::exceeds(shape.width, arm->span));
    MATCH_TRY(pats_str, rewrite_result(ctx, arm->pat, *pat_shape_o));

    // Guard
    bool block_like_pat = trimmed_last_line_width(pats_str) <= config.tab_spaces();
    bool new_line_guard = contains(pats_str, '\n') && !block_like_pat;
    MATCH_TRY(guard_str, rewrite_guard(ctx, arm->guard, shape, trimmed_last_line_width(pats_str), new_line_guard));

    std::string rhs = std::string(pipe_str) + pats_str + guard_str;
    MATCH_TRY(lhs_str, combine_strs_with_missing_comments(ctx, attrs_str, rhs, missing_span, shape, false));

    Span arrow_span = mk_sp(arm->pat->span.hi, spanned(body).lo);
    return rewrite_match_body(ctx, body, lhs_str, shape, contains(guard_str, '\n'), arrow_span, is_last);
}

bool stmt_is_expr_mac(const rust::Stmt* stmt) {
    if (stmt->kind == rust::StmtKind::Expr) {
        if (stmt->expr->kind == rust::ExprKind::MacCall) return true;
    }
    return false;
}

const rust::Block* block_can_be_flattened(const RewriteContext& ctx, const rust::Expr* expr) {
    if (expr->kind == rust::ExprKind::Block) {
        const rust::Block* block = expr->block;
        if (!expr->has_label && !is_unsafe_block(block) && !ctx.inside_macro() &&
            is_simple_block(ctx, block, &expr->attrs) && !stmt_is_expr_mac(block->stmts[0])) {
            return block;
        }
    }
    return nullptr;
}

// (extend, body)
// @extend: true if the arm body can be put next to `=>`
// @body: flattened body, if the body is block with a single expression
std::pair<bool, const rust::Expr*> flatten_arm_body(const RewriteContext& ctx, const rust::Expr* body,
                                                    std::optional<Shape> opt_shape) {
    auto can_extend = [&](const rust::Expr* expr) {
        return !ctx.config->force_multiline_blocks() && can_flatten_block_around_this(expr);
    };

    if (const rust::Block* block = block_can_be_flattened(ctx, body)) {
        if (block->stmts[0]->kind == rust::StmtKind::Expr) {
            const rust::Expr* expr = block->stmts[0]->expr;
            if (expr->kind == rust::ExprKind::Block) {
                if (expr->attrs.empty()) return flatten_arm_body(ctx, expr, std::nullopt);
                return {true, body};
            }
            bool cond_becomes_multi_line = false;
            if (opt_shape) {
                Opt cond = rewrite_cond(ctx, expr, *opt_shape);
                cond_becomes_multi_line = cond && contains(*cond, '\n');
            }
            if (cond_becomes_multi_line) return {false, body};
            return {can_extend(expr), expr};
        }
        return {false, body};
    }
    return {can_extend(body), body};
}

RewriteResult rewrite_match_body(const RewriteContext& ctx, const rust::Expr* body_in, std::string_view pats_str,
                                 Shape shape, bool has_guard, Span arrow_span, bool is_last) {
    const Config& config = *ctx.config;
    auto [extend, body] = flatten_arm_body(ctx, body_in, shape.offset_left_opt(extra_offset(pats_str, shape) + 4));
    bool is_block = false;
    bool is_empty_blk = false;
    if (body->kind == rust::ExprKind::Block) {
        is_block = true;
        is_empty_blk = is_empty_block(ctx, body->block, &body->attrs);
    }

    std::string_view comma = arm_comma(config, body, is_last);
    std::string alt_block_sep = shape.indent.to_string_with_newline(config);

    auto combine_orig_body = [&](std::string_view body_str) -> RewriteResult {
        // control_brace_style = AlwaysSameLine
        std::string_view block_sep = " ";
        return std::string(pats_str) + " =>" + std::string(block_sep) + std::string(body_str) + std::string(comma);
    };

    Indent next_line_indent = (!is_block || is_empty_blk) ? shape.indent.block_indent_(config) : shape.indent;

    bool forbid_same_line = (has_guard && contains(pats_str, '\n') && !is_empty_blk) || !body->attrs.empty();

    // Look for comments between `=>` and the start of the body.
    std::string arrow_comment;
    {
        std::string_view arrow_snippet = trim(ctx.snippet(arrow_span));
        // search for the arrow starting from the end of the snippet since there may be a match
        // expression within the guard
        std::optional<size_t> arrow_index;
        if (config.style_edition() <= StyleEdition::E2021) {
            size_t p = arrow_snippet.rfind("=>");
            if (p != std::string_view::npos) arrow_index = p;
        } else {
            arrow_index = find_last_uncommented(arrow_snippet, "=>");
        }
        if (!arrow_index) panic("called `Option::unwrap()` on a `None` value (match arm `=>`)", arrow_span.lo);
        // 2 = `=>`
        std::string_view comment_str = trim(arrow_snippet.substr(*arrow_index + 2));
        if (!comment_str.empty()) {
            MATCH_TRY(c, rewrite_comment(comment_str, false, shape, config));
            arrow_comment = std::move(c);
        }
    }

    auto combine_next_line_body = [&](std::string_view body_str) -> RewriteResult {
        std::string nested_indent_str = next_line_indent.to_string_with_newline(config);

        if (is_block) {
            std::string result(pats_str);
            result += " =>";
            if (!arrow_comment.empty()) {
                result += nested_indent_str;
                result += arrow_comment;
            }
            result += nested_indent_str;
            result += body_str;
            result += comma;
            return result;
        }

        std::string indent_str = shape.indent.to_string_with_newline(config);
        std::string_view body_prefix;
        std::string body_suffix;
        if (config.match_arm_blocks() && !ctx.inside_macro()) {
            // match_block_trailing_comma = false
            const char* comma2 = config.match_block_trailing_comma() ? "," : "";
            const char* semicolon = "";
            if (config.style_edition() <= StyleEdition::E2021) {
                semicolon = "";
            } else {
                semicolon = semicolon_for_expr(ctx, body) ? ";" : "";
            }
            body_prefix = "{";
            body_suffix = std::string(semicolon) + indent_str + "}" + comma2;
        } else {
            body_prefix = "";
            body_suffix = ",";
        }

        std::string block_sep;
        // control_brace_style = AlwaysSameLine
        if (body_prefix.empty()) {
            block_sep = "";
        } else if (forbid_same_line || !arrow_comment.empty()) {
            block_sep = alt_block_sep + std::string(body_prefix);
        } else {
            block_sep = " " + std::string(body_prefix);
        }
        block_sep += nested_indent_str;

        std::string result(pats_str);
        result += " =>";
        if (!arrow_comment.empty()) {
            result += indent_str;
            result += arrow_comment;
        }
        result += block_sep;
        result += body_str;
        result += body_suffix;
        return result;
    };

    // Let's try and get the arm body on the same line as the condition.
    // 4 = ` => `.len()
    std::optional<Shape> orig_body_shape = shape.offset_left_opt(extra_offset(pats_str, shape) + 4);
    if (orig_body_shape) orig_body_shape = orig_body_shape->sub_width_opt(comma.size());
    RewriteResult orig_body(RewriteError::unknown());
    if (forbid_same_line || !arrow_comment.empty()) {
        orig_body = Err(RewriteError::unknown());
    } else if (orig_body_shape) {
        Shape body_shape = *orig_body_shape;
        RewriteResult rewrite =
            nop_block_collapse(format_expr(body, ExprType::Statement, ctx, body_shape), body_shape.width);

        if (rewrite &&
            (is_block || (!contains(*rewrite, '\n') && unicode_str_width(*rewrite) <= body_shape.width))) {
            return combine_orig_body(*rewrite);
        }
        orig_body = rewrite;
    } else {
        orig_body = Err(RewriteError::unknown());
    }
    size_t orig_budget = orig_body_shape ? orig_body_shape->width : 0;

    // Try putting body on the next line and see if it looks better.
    Shape next_line_body_shape = Shape::indented(next_line_indent, config);
    RewriteResult next_line_body = nop_block_collapse(
        format_expr(body, ExprType::Statement, ctx, next_line_body_shape), next_line_body_shape.width);
    if (orig_body && next_line_body && prefer_next_line(*orig_body, *next_line_body, RhsTactics::Default))
        return combine_next_line_body(*next_line_body);
    if (orig_body && extend && first_line_width(*orig_body) <= orig_budget) return combine_orig_body(*orig_body);
    if (orig_body && next_line_body && contains(*orig_body, '\n')) return combine_next_line_body(*next_line_body);
    if (!orig_body && next_line_body) return combine_next_line_body(*next_line_body);
    // When both orig_body and next_line_body result in errors, we currently propagate the
    // error from the second attempt since it is more generous with width constraints.
    // This decision is somewhat arbitrary and is open to change.
    if (!orig_body && !next_line_body) return next_line_body;
    return combine_orig_body(*orig_body);
}

// The `if ...` guard on a match arm.
RewriteResult rewrite_guard(const RewriteContext& ctx, const rust::Guard* guard, Shape shape,
                            // The amount of space used up on this line for the pattern in
                            // the arm (excludes offset).
                            size_t pattern_width, bool multiline_pattern) {
    const Config& config = *ctx.config;
    if (!guard) return std::string();
    // First try to fit the guard string on the same line as the pattern.
    // 4 = ` if `, 5 = ` => {`
    std::optional<Shape> cond_shape = shape.offset_left_opt(pattern_width + 4);
    if (cond_shape) cond_shape = cond_shape->sub_width_opt(5);
    if (!multiline_pattern) {
        if (cond_shape) {
            RewriteResult cond_str = rewrite_result(ctx, guard->cond, *cond_shape);
            if (cond_str) {
                if (!contains(*cond_str, '\n') || pattern_width <= config.tab_spaces()) return " if " + *cond_str;
            }
        }
    }

    // Not enough space to put the guard after the pattern, try a newline.
    // 3 = `if `, 5 = ` => {`
    Span guard_span = guard->cond->span;  // Guard::span() is the condition's span
    std::optional<Shape> cs = Shape::indented(shape.indent.block_indent_(config), config).offset_left(3);
    if (cs) cs = cs->sub_width(5);
    if (!cs) return Err(RewriteError::exceeds(shape.width, guard_span));
    MATCH_TRY(cond_str, rewrite_result(ctx, guard->cond, *cs));
    return cs->indent.to_string_with_newline(config) + "if " + cond_str;
}

RewriteResult nop_block_collapse(RewriteResult block_str, size_t budget) {
    if (!block_str) return block_str;
    const std::string& s = *block_str;
    if (starts_with(s, "{") && budget >= 2) {
        // block_str[1..].find(|c: char| !c.is_whitespace()).unwrap()
        std::string_view rest = std::string_view(s).substr(1);
        std::optional<size_t> found;
        size_t i = 0;
        while (i < rest.size()) {
            size_t len = 1;
            uint32_t c = rust::decode_utf8(rest, i, len);
            if (len == 0) len = 1;
            if (!is_ws_char(c)) {
                found = i;
                break;
            }
            i += len;
        }
        if (!found) panic("called `Option::unwrap()` on a `None` value (nop_block_collapse)");
        if (*found == s.size() - 2) return std::string("{}");
    }
    return block_str;
}

bool can_flatten_block_around_this(const rust::Expr* body) {
    using K = rust::ExprKind;
    switch (body->kind) {
    // We do not allow `if` to stay on the same line, since we could easily mistake
    // `pat => if cond { ... }` and `pat if cond => { ... }`.
    case K::If: return false;
    // We do not allow collapsing a block around expression with condition
    // to avoid it being cluttered with match arm.
    case K::ForLoop:
    case K::While: return false;
    case K::Loop:
    case K::Match:
    case K::Block:
    case K::Closure:
    case K::Array:
    case K::Call:
    case K::MethodCall:
    case K::MacCall:
    case K::Struct:
    case K::Tup: return true;
    case K::AddrOf:
    case K::Try:
    case K::Unary:
    case K::Index:
    case K::Cast: return can_flatten_block_around_this(body->lhs);
    default: return false;
    }
}

}  // namespace

#undef MATCH_TRY
#undef MATCH_SHAPE

}  // namespace rustfmt
