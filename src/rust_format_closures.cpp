// rust_format_closures.cpp -- rustfmt's closures.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// This module is pretty messy because of the rules around closures and blocks:
//   * if there is a return type, then there must be braces,
//   * given a closure with braces, whether that is parsed to give an inner block
//     or not depends on if there is a return type and if there are statements
//     in that block,
//   * if the first expression in the body ends with a block (i.e., is a
//     statement without needing a semi-colon), then adding or removing braces
//     can change whether it is treated as an expression or statement.
#include "rust_format_internal.h"

namespace rustfmt {

namespace {

RewriteResult rewrite_closure_with_block(const rust::Expr* body, std::string_view prefix,
                                         const RewriteContext& context, Shape shape);
RewriteResult rewrite_closure_expr(const rust::Expr* expr, std::string_view prefix, const RewriteContext& context,
                                   Shape shape);
RewriteResult rewrite_closure_block(const rust::Expr* block, std::string_view prefix, const RewriteContext& context,
                                    Shape shape);
bool is_block_closure_forced(const RewriteContext& context, const rust::Expr* expr);
bool expr_requires_semi_to_be_stmt(const rust::Expr* e);

std::string concat_prefix(std::string_view prefix, std::string_view rest) {
    std::string s(prefix);
    s.push_back(' ');
    s += rest;
    return s;
}

// Figure out if a block is necessary.
bool needs_block(const rust::Block* block, bool has_label, std::string_view prefix, const RewriteContext& context) {
    bool has_attributes = false;
    if (!block->stmts.empty()) {
        const rust::AttrVec* attrs = get_attrs_from_stmt(block->stmts.front());
        has_attributes = attrs && !attrs->empty();
    }

    return is_unsafe_block(block) || block->stmts.size() > 1 || has_attributes ||
           block_contains_comment(context, block) || contains(prefix, '\n') || has_label;
}

const rust::Expr* get_inner_expr(const rust::Expr* expr, std::string_view prefix, const RewriteContext& context) {
    if (expr->kind == rust::ExprKind::Block) {
        const rust::Block* block = expr->block;
        if (!needs_block(block, expr->has_label, prefix, context)) {
            // block.stmts.len() == 1 except with `|| {{}}`;
            // https://github.com/rust-lang/rustfmt/issues/3844
            if (!block->stmts.empty()) {
                if (const rust::Expr* e = stmt_expr(block->stmts.front())) return get_inner_expr(e, prefix, context);
            }
        }
    }
    return expr;
}

RewriteResult try_rewrite_without_block(const rust::Expr* expr_in, std::string_view prefix,
                                        const RewriteContext& context, Shape shape, Shape body_shape) {
    const rust::Expr* expr = get_inner_expr(expr_in, prefix, context);

    if (is_block_closure_forced(context, expr)) return rewrite_closure_with_block(expr, prefix, context, shape);
    return rewrite_closure_expr(expr, prefix, context, body_shape);
}

bool veto_block(const rust::Expr* e) {
    switch (e->kind) {
    case rust::ExprKind::Call:
    case rust::ExprKind::Binary:
    case rust::ExprKind::Cast:
    case rust::ExprKind::Type:
    case rust::ExprKind::Assign:
    case rust::ExprKind::AssignOp:
    case rust::ExprKind::Field:
    case rust::ExprKind::Index:
    case rust::ExprKind::Range:
    case rust::ExprKind::Try: return true;
    default: return false;
    }
}

// Rewrite closure with a single expression wrapping its body with block.
// || { #[attr] foo() } -> Block { #[attr] foo() }
RewriteResult rewrite_closure_with_block(const rust::Expr* body, std::string_view prefix,
                                         const RewriteContext& context, Shape shape) {
    const rust::Expr* left_most = left_most_sub_expr(body);
    bool veto_block_ = veto_block(body) && !expr_requires_semi_to_be_stmt(left_most);
    if (veto_block_) return Err(RewriteError::unknown());

    rust::Stmt* stmt = context.arena().make<rust::Stmt>();
    stmt->kind = rust::StmtKind::Expr;
    stmt->expr = const_cast<rust::Expr*>(body);  // NOLINT: the synthetic block only reads it
    stmt->span = body->span;
    rust::Block* block = context.arena().make<rust::Block>();
    block->stmts.push_back(stmt);
    block->rules = rust::BlockCheckMode::Default;
    block->span = body->attrs.empty() ? body->span : body->attrs.front().span.to(body->span);

    RewriteResult block_str =
        rewrite_block_with_visitor(context, "", block, &body->attrs, nullptr, shape, false);
    if (!block_str) return block_str;
    return concat_prefix(prefix, *block_str);
}

// allow_multi_line (a nested fn of rewrite_closure_expr)
bool allow_multi_line(const rust::Expr* expr) {
    switch (expr->kind) {
    case rust::ExprKind::Match:
    case rust::ExprKind::Gen:
    case rust::ExprKind::Block:
    case rust::ExprKind::TryBlock:
    case rust::ExprKind::Loop:
    case rust::ExprKind::Struct: return true;

    case rust::ExprKind::AddrOf:
    case rust::ExprKind::Try:
    case rust::ExprKind::Unary:
    case rust::ExprKind::Cast: return allow_multi_line(expr->lhs);

    default: return false;
    }
}

// Rewrite closure with a single expression without wrapping its body with block.
RewriteResult rewrite_closure_expr(const rust::Expr* expr, std::string_view prefix, const RewriteContext& context,
                                   Shape shape) {
    // When rewriting closure's body without block, we require it to fit in a single line
    // unless it is a block-like expression or we are inside macro call.
    // (force_multiline_blocks is false: non-default config: force_multiline_blocks)
    bool veto_multiline = !allow_multi_line(expr) && !context.inside_macro();
    RewriteResult rw = rewrite_result(context, expr, shape);
    if (!rw) return rw;
    if (veto_multiline && contains(*rw, '\n')) return Err(RewriteError::unknown());
    return concat_prefix(prefix, *rw);
}

// Rewrite closure whose body is block.
RewriteResult rewrite_closure_block(const rust::Expr* block, std::string_view prefix, const RewriteContext& context,
                                    Shape shape) {
    RewriteResult rw = rewrite_result(context, block, shape);
    if (!rw) return rw;
    return concat_prefix(prefix, *rw);
}

// Return type is (prefix, extra_offset)
Expected<std::pair<std::string, size_t>> rewrite_closure_fn_decl(const rust::Closure* closure,
                                                                 const rust::Expr* body, Span span,
                                                                 const RewriteContext& context, Shape shape) {
    using R = Expected<std::pair<std::string, size_t>>;
    std::string binder;
    if (closure->binder.present) {
        if (closure->binder.generic_params.empty()) {
            binder = "for<> ";
        } else {
            Opt lifetime_str = rewrite_bound_params(context, shape, closure->binder.generic_params);
            if (!lifetime_str) return R(RewriteError::unknown());
            binder = "for<" + *lifetime_str + "> ";
        }
    }

    std::string_view const_ = closure->constness.yes ? "const " : "";
    std::string_view immovable = closure->movability == rust::Movability::Static ? "static " : "";
    std::string_view coro;
    switch (closure->coroutine_kind.kind) {
    case rust::CoroutineKind::Async: coro = "async "; break;
    case rust::CoroutineKind::Gen: coro = "gen "; break;
    case rust::CoroutineKind::AsyncGen: coro = "async gen "; break;
    case rust::CoroutineKind::None: coro = ""; break;
    }
    std::string_view capture_str;
    switch (closure->capture) {
    case rust::CaptureBy::Value: capture_str = "move "; break;
    case rust::CaptureBy::Use: capture_str = "use "; break;
    case rust::CaptureBy::Ref: capture_str = ""; break;
    }
    // 4 = "|| {".len(), which is overconservative when the closure consists of
    // a single expression.
    size_t offset = binder.size() + const_.size() + immovable.size() + coro.size() + capture_str.size();
    std::optional<Shape> shrunk = shape.shrink_left(offset);
    if (!shrunk) return R(RewriteError::exceeds(shape.exceeds(span)));
    std::optional<Shape> nested_shape_o = shrunk->sub_width(4);
    if (!nested_shape_o) return R(RewriteError::exceeds(shrunk->exceeds(span)));
    Shape nested_shape = *nested_shape_o;

    // 1 = |
    Indent param_offset = nested_shape.indent + 1;
    std::optional<Shape> param_shape_o = nested_shape.offset_left(1);
    if (!param_shape_o) return R(RewriteError::exceeds(nested_shape.exceeds(span)));
    Shape param_shape = param_shape_o->visual_indent(0);
    RewriteResult ret_str = rewrite_result(context, &closure->fn_decl->output, param_shape);
    if (!ret_str) return R(ret_str.error());

    const std::vector<rust::Param*>& inputs = closure->fn_decl->inputs;
    std::vector<ListItem> item_vec = itemize_list(
        *context.snippet_provider, inputs.size(), "|", ",",
        [&](size_t i) { return span_lo_for_param(inputs[i]); },
        [&](size_t i) { return span_hi_for_param(context, inputs[i]); },
        [&](size_t i) { return rewrite_result(context, inputs[i], param_shape); },
        context.snippet_provider->span_after(span, "|"), body->span.lo, false);
    // 1 = space between parameters and return type.
    size_t horizontal_budget = sat_sub(nested_shape.width, ret_str->size() + 1);
    DefinitiveListTactic tactic =
        definitive_tactic(item_vec, ListTactic::HorizontalVertical(), Separator::Comma, horizontal_budget);
    if (tactic == DefinitiveListTactic::Horizontal()) {
        std::optional<Shape> s = param_shape.sub_width(ret_str->size() + 1);
        if (!s) return R(RewriteError::exceeds(param_shape.exceeds(span)));
        param_shape = *s;
    }

    ListFormatting fmt(param_shape, *context.config);
    fmt.tactic(tactic).preserve_newline(true);
    RewriteResult list_str = write_list(item_vec, fmt);
    if (!list_str) return R(list_str.error());
    std::string prefix = binder;
    prefix += const_;
    prefix += immovable;
    prefix += coro;
    prefix += capture_str;
    prefix += "|";
    prefix += *list_str;
    prefix += "|";

    if (!ret_str->empty()) {
        if (contains(prefix, '\n')) {
            prefix.push_back('\n');
            prefix += param_offset.to_string(*context.config);
        } else {
            prefix.push_back(' ');
        }
        prefix += *ret_str;
    }
    // 1 = space between `|...|` and body.
    size_t extra_offset = last_line_width(prefix) + 1;

    return R(std::make_pair(std::move(prefix), extra_offset));
}

bool is_block_closure_forced_inner(const rust::Expr* expr, StyleEdition style_edition) {
    switch (expr->kind) {
    case rust::ExprKind::If:
    case rust::ExprKind::While:
    case rust::ExprKind::ForLoop: return true;
    case rust::ExprKind::Loop: return style_edition >= StyleEdition::E2024;
    case rust::ExprKind::AddrOf:
    case rust::ExprKind::Try:
    case rust::ExprKind::Unary:
    case rust::ExprKind::Cast: return is_block_closure_forced_inner(expr->lhs, style_edition);
    default: return false;
    }
}

bool is_block_closure_forced(const RewriteContext& context, const rust::Expr* expr) {
    // If we are inside macro, we do not want to add or remove block from closure body.
    if (context.inside_macro()) return false;
    return is_block_closure_forced_inner(expr, context.config->style_edition());
}

/// Does this expression require a semicolon to be treated as a statement?
/// (From rustc's classify.rs.)
bool expr_requires_semi_to_be_stmt(const rust::Expr* e) {
    switch (e->kind) {
    case rust::ExprKind::If:
    case rust::ExprKind::Match:
    case rust::ExprKind::Block:
    case rust::ExprKind::While:
    case rust::ExprKind::Loop:
    case rust::ExprKind::ForLoop:
    case rust::ExprKind::TryBlock: return false;
    default: return true;
    }
}

}  // namespace

// rustfmt passes the closure's fields one by one (binder, constness, capture,
// coroutine_kind, movability, fn_decl, body); here they come as the Closure.
RewriteResult rewrite_closure(const rust::Closure* closure, Span span, const RewriteContext& context, Shape shape) {
    const rust::Expr* body = closure->body;
    auto decl = rewrite_closure_fn_decl(closure, body, span, context, shape);
    if (!decl) return Err(decl.error());
    const std::string& prefix = decl->first;
    size_t extra_offset = decl->second;
    // 1 = space between `|...|` and body.
    std::optional<Shape> body_shape_o = shape.offset_left(extra_offset);
    if (!body_shape_o) return Err(RewriteError::exceeds(shape.exceeds(span)));
    Shape body_shape = *body_shape_o;

    if (body->kind == rust::ExprKind::Block) {
        const rust::Block* block = body->block;
        // The body of the closure is an empty block.
        if (block->stmts.empty() && !block_contains_comment(context, block)) {
            RewriteResult rw = rewrite_result(context, body, shape);
            if (!rw) return rw;
            return concat_prefix(prefix, *rw);
        }

        RewriteResult result = (closure->fn_decl->output.is_default() && !context.inside_macro())
                                   ? try_rewrite_without_block(body, prefix, context, shape, body_shape)
                                   : Err(RewriteError::unknown());

        if (result) return result;
        // Either we require a block, or tried without and failed.
        return rewrite_closure_block(body, prefix, context, body_shape);
    }
    RewriteResult r = rewrite_closure_expr(body, prefix, context, body_shape);
    if (r) return r;
    // The closure originally had a non-block expression, but we can't fit on
    // one line, so we'll insert a block.
    return rewrite_closure_with_block(body, prefix, context, body_shape);
}

// Rewriting closure which is placed at the end of the function call's arg.
// Returns an error if the reformatted closure 'looks bad'.
RewriteResult rewrite_last_closure(const RewriteContext& context, const rust::Expr* expr, Shape shape) {
    if (expr->kind == rust::ExprKind::Closure) {
        const rust::Closure* closure = expr->closure;
        const rust::Expr* body = closure->body;
        if (body->kind == rust::ExprKind::Block) {
            const rust::Block* block = body->block;
            if (!is_unsafe_block(block) && !context.inside_macro() && is_simple_block(context, block, &body->attrs) &&
                !body->has_label) {
                if (block->stmts.empty()) panic("rewrite_last_closure: simple block without statements");
                if (const rust::Expr* e = stmt_expr(block->stmts[0])) body = e;
            }
        }
        auto decl = rewrite_closure_fn_decl(closure, body, expr->span, context, shape);
        if (!decl) return Err(decl.error());
        const std::string& prefix = decl->first;
        size_t extra_offset = decl->second;
        // If the closure goes multi line before its body, do not overflow the closure.
        if (contains(prefix, '\n')) return Err(RewriteError::unknown());

        std::optional<Shape> body_shape_o = shape.offset_left(extra_offset);
        if (!body_shape_o) return Err(RewriteError::exceeds(shape.exceeds(expr->span)));
        Shape body_shape = *body_shape_o;

        // We force to use block for the body of the closure for certain kinds of expressions.
        if (is_block_closure_forced(context, body)) {
            RewriteResult body_str = rewrite_closure_with_block(body, prefix, context, body_shape);
            if (!body_str) return body_str;
            if (closure->fn_decl->output.is_default() && lines(*body_str).size() <= 7) {
                // If the expression can fit in a single line, we need not force block
                // closure.  However, if the closure has a return type, then we must
                // keep the blocks.
                RewriteResult single_line_body_str = rewrite_closure_expr(body, prefix, context, shape);
                if (single_line_body_str && !contains(*single_line_body_str, '\n')) return single_line_body_str;
            }
            return body_str;
        }

        // When overflowing the closure which consists of a single control flow expression,
        // force to use block if its condition uses multi line.
        Opt cond = rewrite_cond(context, body, body_shape);
        bool is_multi_lined_cond = cond && (contains(*cond, '\n') || cond->size() > body_shape.width);
        if (is_multi_lined_cond) return rewrite_closure_with_block(body, prefix, context, body_shape);

        // Seems fine, just format the closure in usual manner.
        return rewrite_result(context, expr, shape);
    }
    return Err(RewriteError::unknown());
}

/// Returns `true` if the given vector of arguments has more than one `ast::ExprKind::Closure`.
bool args_have_many_closure(const std::vector<OverflowableItem>& args) {
    size_t n = 0;
    for (const OverflowableItem& it : args) {
        const rust::Expr* e = overflowable_to_expr(it);
        if (e && e->kind == rust::ExprKind::Closure) n++;
    }
    return n > 1;
}

}  // namespace rustfmt
