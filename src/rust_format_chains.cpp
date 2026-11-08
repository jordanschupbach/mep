// rust_format_chains.cpp -- rustfmt's chains.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// Formatting of chained expressions, i.e., expressions that are chained by
// dots: struct and enum field access, method calls, and try shorthand (`?`).
// Whenever possible the entire chain is put on a single line; if that fails,
// each subexpression goes on its own line.
//
// Only ChainFormatterBlock is ported: ChainFormatterVisual (and
// get_visual_style_child_shape) is used only with indent_style = Visual
// (non-default config: indent_style).
#include "rust_format_internal.h"

#include <algorithm>

namespace rustfmt {

namespace {

// Result<(), RewriteError>: nullopt is Ok(()).
using UnitResult = std::optional<RewriteError>;

/// Provides the original input contents from the span of a chain element with
/// trailing spaces trimmed.
Opt format_overflow_style(Span span, const RewriteContext& context) {
    std::optional<std::string_view> s = context.snippet_provider->span_to_snippet(span);
    if (!s) return std::nullopt;
    std::vector<std::string> parts;
    for (std::string_view l : lines(*s)) parts.emplace_back(trim_end(l));
    return join(parts, "\n");
}

Shape get_block_child_shape(bool prev_ends_with_block, const RewriteContext& context, Shape shape) {
    return (prev_ends_with_block ? shape.block_indent(0) : shape.block_indent(context.config->tab_spaces()))
        .with_max_width(*context.config);
}

// get_visual_style_child_shape: only for indent_style = Visual (non-default config: indent_style).

enum class CommentPosition : uint8_t { Back, Top };

/// Information about an expression in a chain.
struct SubExpr {
    const rust::Expr* expr = nullptr;
    bool is_method_call_receiver = false;
};

enum class ChainItemKindTag : uint8_t { Parent, MethodCall, StructField, TupleField, Await, Use, Yield, Comment };

struct ChainItemKind {
    ChainItemKindTag tag = ChainItemKindTag::Parent;
    // Parent { expr, parens }
    const rust::Expr* expr = nullptr;
    bool parens = false;
    // MethodCall(segment, types, args)
    const rust::PathSegment* segment = nullptr;
    std::vector<const rust::GenericArg*> types;
    const std::vector<rust::Expr*>* args = nullptr;
    // StructField(ident) / TupleField(ident, nested)
    rust::Ident ident;
    bool nested = false;
    // Comment(comment, pos)
    std::string comment;
    CommentPosition pos = CommentPosition::Back;
};

bool should_add_parens(const rust::Expr* expr, const RewriteContext& context);

// ChainItemKind::is_block_like
bool is_block_like(const ChainItemKind& k, const RewriteContext& context, std::string_view reps) {
    switch (k.tag) {
    case ChainItemKindTag::Parent: return is_block_expr(context, k.expr, reps);
    default: return false;
    }
}

// ChainItemKind::is_tup_field_access
bool is_tup_field_access(const rust::Expr* expr) {
    if (expr->kind != rust::ExprKind::Field) return false;
    for (char c : expr->ident.name)
        if (c < '0' || c > '9') return false;
    return true;
}

// ChainItemKind::from_ast
std::pair<ChainItemKind, Span> chain_item_kind_from_ast(const RewriteContext& context, const rust::Expr* expr,
                                                         bool is_method_call_receiver) {
    ChainItemKind kind;
    Span span;
    switch (expr->kind) {
    case rust::ExprKind::MethodCall: {
        kind.tag = ChainItemKindTag::MethodCall;
        if (expr->seg.args && expr->seg.args->kind == rust::GenericArgs::AngleBracketed) {
            for (const rust::AngleBracketedArg& a : expr->seg.args->args)
                if (!a.is_constraint) kind.types.push_back(&a.arg);
        }
        kind.segment = &expr->seg;
        kind.args = &expr->exprs;
        span = mk_sp(expr->lhs->span.hi, expr->span.hi);
        break;
    }
    case rust::ExprKind::Field: {
        if (is_tup_field_access(expr)) {
            kind.tag = ChainItemKindTag::TupleField;
            kind.nested = is_tup_field_access(expr->lhs);
        } else {
            kind.tag = ChainItemKindTag::StructField;
        }
        kind.ident = expr->ident;
        span = mk_sp(expr->lhs->span.hi, expr->ident.span.hi);
        break;
    }
    case rust::ExprKind::Await:
        kind.tag = ChainItemKindTag::Await;
        span = mk_sp(expr->lhs->span.hi, expr->span.hi);
        break;
    case rust::ExprKind::Use:
        kind.tag = ChainItemKindTag::Use;
        span = mk_sp(expr->lhs->span.hi, expr->span.hi);
        break;
    case rust::ExprKind::Yield:
        if (expr->yield_kind == rust::YieldKind::Postfix && expr->lhs) {
            kind.tag = ChainItemKindTag::Yield;
            span = mk_sp(expr->lhs->span.hi, expr->span.hi);
            break;
        }
        [[fallthrough]];
    default:
        kind.tag = ChainItemKindTag::Parent;
        kind.expr = expr;
        kind.parens = is_method_call_receiver && should_add_parens(expr, context);
        return {std::move(kind), expr->span};
    }

    // Remove comments from the span.
    uint32_t lo = context.snippet_provider->span_before(span, ".");
    return {std::move(kind), mk_sp(lo, span.hi)};
}

/// An expression plus trailing `?`s to be formatted together.
struct ChainItem {
    ChainItemKind kind;
    size_t tries = 0;
    Span span;

    static ChainItem make(const RewriteContext& context, const SubExpr& expr, size_t tries) {
        auto [kind, span] = chain_item_kind_from_ast(context, expr.expr, expr.is_method_call_receiver);
        return ChainItem{std::move(kind), tries, span};
    }

    static ChainItem comment(Span span, std::string comment, CommentPosition pos) {
        ChainItem c;
        c.kind.tag = ChainItemKindTag::Comment;
        c.kind.comment = std::move(comment);
        c.kind.pos = pos;
        c.tries = 0;
        c.span = span;
        return c;
    }

    bool is_comment() const { return kind.tag == ChainItemKindTag::Comment; }

    RewriteResult rewrite_result(const RewriteContext& context, Shape shape) const;
};

// ChainItem::rewrite_method_call
RewriteResult rewrite_method_call(const rust::Ident& method_name, const std::vector<const rust::GenericArg*>& types,
                                  const std::vector<rust::Expr*>& args, Span span, const RewriteContext& context,
                                  Shape shape) {
    std::string type_str;
    if (!types.empty()) {
        std::vector<std::string> type_list;
        for (const rust::GenericArg* ty : types) {
            RewriteResult r = rewrite_result(context, ty, shape);
            if (!r) return r;
            type_list.push_back(std::move(*r));
        }
        type_str = "::<" + join(type_list, ", ") + ">";
    }
    std::string callee_str = ".";
    callee_str += rewrite_ident(context, method_name);
    callee_str += type_str;
    return rewrite_call(context, callee_str, args, span, shape);
}

// impl Rewrite for ChainItem
RewriteResult ChainItem::rewrite_result(const RewriteContext& context, Shape shape_in) const {
    std::optional<Shape> shape_o = shape_in.sub_width(tries);
    if (!shape_o) return Err(RewriteError::exceeds(shape_in.exceeds(span)));
    Shape shape = *shape_o;
    std::string rewrite;
    switch (kind.tag) {
    case ChainItemKindTag::Parent: {
        RewriteResult r = kind.parens ? rewrite_paren(context, kind.expr, shape, kind.expr->span)
                                      : rustfmt::rewrite_result(context, kind.expr, shape);
        if (!r) return r;
        rewrite = std::move(*r);
        break;
    }
    case ChainItemKindTag::MethodCall: {
        RewriteResult r = rewrite_method_call(kind.segment->ident, kind.types, *kind.args, span, context, shape);
        if (!r) return r;
        rewrite = std::move(*r);
        break;
    }
    case ChainItemKindTag::StructField:
        rewrite = ".";
        rewrite += rewrite_ident(context, kind.ident);
        break;
    case ChainItemKindTag::TupleField:
        rewrite = (kind.nested && context.config->style_edition() <= StyleEdition::E2021) ? " ." : ".";
        rewrite += rewrite_ident(context, kind.ident);
        break;
    case ChainItemKindTag::Await: rewrite = ".await"; break;
    case ChainItemKindTag::Use: rewrite = ".use"; break;
    case ChainItemKindTag::Yield: rewrite = ".yield"; break;
    case ChainItemKindTag::Comment: {
        RewriteResult r = rewrite_comment(kind.comment, false, shape, *context.config);
        if (!r) return r;
        rewrite = std::move(*r);
        break;
    }
    }
    return rewrite + repeat("?", tries);
}

RewriteResult format_chain_item(const ChainItem& item, const RewriteContext& context, Shape rewrite_shape,
                                bool allow_overflow) {
    if (allow_overflow) {
        RewriteResult r = item.rewrite_result(context, rewrite_shape);
        if (r) return r;
        return unknown_error(format_overflow_style(item.span, context));
    }
    return item.rewrite_result(context, rewrite_shape);
}

/// Removes try operators (`?`s) that appear in the given string. If removing
/// them leaves an empty line, remove that line as well unless it is the first
/// line (we need the first newline for detecting pre/post comment).
std::string trim_tries(std::string_view s) {
    std::string result;
    std::string line_buffer;
    for (const ClassifiedChar& cc : char_classes(s)) {
        if (cc.ch == '\n') {
            if (result.empty() || !trim(line_buffer).empty()) {
                result += line_buffer;
                result.push_back('\n');
            }
            line_buffer.clear();
        } else if (cc.ch == '?' && cc.kind == FullCodeCharKind::Normal) {
            continue;
        } else {
            line_buffer.append(s.substr(cc.pos, cc.len));
        }
    }
    if (!trim(line_buffer).empty()) result += line_buffer;
    return result;
}

/// Whether a method call's receiver needs parenthesis, like
/// `|| .. .method();`, `|| 1.. .method();`, `1. .method();`.
bool should_add_parens(const rust::Expr* expr, const RewriteContext& context) {
    switch (expr->kind) {
    case rust::ExprKind::Lit: return lit_ends_in_dot(expr->lit, context);
    case rust::ExprKind::Closure: {
        const rust::Expr* body = expr->closure->body;
        switch (body->kind) {
        case rust::ExprKind::Range: return body->limits == rust::RangeLimits::HalfOpen;
        case rust::ExprKind::Lit: return lit_ends_in_dot(body->lit, context);
        default: return false;
        }
    }
    default: return false;
    }
}

struct Chain {
    ChainItem parent;
    std::vector<ChainItem> children;

    static Chain from_ast(const rust::Expr* expr, const RewriteContext& context);
    static std::vector<SubExpr> make_subexpr_list(const rust::Expr* expr, const RewriteContext& context);
    static std::optional<SubExpr> pop_expr_chain(const SubExpr& expr, const RewriteContext& context);
    static const rust::Expr* convert_try(const rust::Expr* expr, const RewriteContext& context);
    RewriteResult rewrite_result(const RewriteContext& context, Shape shape) const;
};

bool is_tries(std::string_view s) {
    for (char c : s)
        if (c != '?') return false;
    return true;
}

bool is_post_comment(std::string_view s) {
    size_t comment_start_index = s.find('/');
    if (comment_start_index == std::string_view::npos) return false;
    size_t newline_index = s.find('\n');
    if (newline_index == std::string_view::npos) return true;
    return comment_start_index < newline_index;
}

void handle_post_comment(Span post_comment_span, std::string_view post_comment_snippet, uint32_t& prev_span_end,
                         std::vector<ChainItem>& children) {
    // trim_matches(&[' ', '\t'])
    std::string_view t = post_comment_snippet;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    if (!t.empty() && t.front() == '\n') {
        // No post comment.
        return;
    }
    std::string trimmed_snippet = trim_tries(post_comment_snippet);
    if (is_post_comment(trimmed_snippet)) {
        children.push_back(
            ChainItem::comment(post_comment_span, std::string(trim(trimmed_snippet)), CommentPosition::Back));
        prev_span_end = post_comment_span.hi;
    }
}

Chain Chain::from_ast(const rust::Expr* expr, const RewriteContext& context) {
    std::vector<SubExpr> subexpr_list = make_subexpr_list(expr, context);

    // Un-parse the expression tree into ChainItems
    std::vector<ChainItem> rev_children;
    size_t sub_tries = 0;
    for (const SubExpr& subexpr : subexpr_list) {
        if (subexpr.expr->kind == rust::ExprKind::Try) {
            sub_tries += 1;
        } else {
            rev_children.push_back(ChainItem::make(context, subexpr, sub_tries));
            sub_tries = 0;
        }
    }

    if (rev_children.empty()) panic("chain without a parent", expr->span.lo);
    ChainItem parent = std::move(rev_children.back());
    rev_children.pop_back();
    std::vector<ChainItem> children;
    uint32_t prev_span_end = parent.span.hi;
    // iter = rev_children.into_iter().rev().peekable()
    size_t i = rev_children.size();
    auto peek = [&]() -> ChainItem* { return i > 0 ? &rev_children[i - 1] : nullptr; };
    if (ChainItem* first_chain_item = peek()) {
        Span comment_span = mk_sp(prev_span_end, first_chain_item->span.lo);
        std::string_view comment_snippet = context.snippet(comment_span);
        if (!is_tries(trim(comment_snippet)))
            handle_post_comment(comment_span, comment_snippet, prev_span_end, children);
    }
    while (i > 0) {
        ChainItem chain_item = std::move(rev_children[i - 1]);
        i--;
        std::string_view comment_snippet = context.snippet(chain_item.span);
        // FIXME: Figure out the way to get a correct span when converting `try!` to `?`.
        // (use_try_shorthand is false: non-default config: use_try_shorthand)
        bool handle_comment = !is_tries(trim(comment_snippet));

        // Pre-comment
        if (handle_comment) {
            Span pre_comment_span = mk_sp(prev_span_end, chain_item.span.lo);
            std::string pre_comment_snippet = trim_tries(context.snippet(pre_comment_span));
            Opt pre_comment = extract_pre_comment(pre_comment_snippet).first;
            if (pre_comment && !pre_comment->empty())
                children.push_back(ChainItem::comment(pre_comment_span, *pre_comment, CommentPosition::Top));
        }

        prev_span_end = chain_item.span.hi;
        children.push_back(std::move(chain_item));

        // Post-comment
        if (!handle_comment || peek() == nullptr) continue;

        uint32_t next_lo = peek()->span.lo;
        Span post_comment_span = mk_sp(prev_span_end, next_lo);
        std::string_view post_comment_snippet = context.snippet(post_comment_span);
        handle_post_comment(post_comment_span, post_comment_snippet, prev_span_end, children);
    }

    return Chain{std::move(parent), std::move(children)};
}

// Returns a Vec of the prefixes of the chain.
// E.g., for input `a.b.c` we return [`a.b.c`, `a.b`, 'a']
std::vector<SubExpr> Chain::make_subexpr_list(const rust::Expr* expr, const RewriteContext& context) {
    std::vector<SubExpr> subexpr_list{SubExpr{expr, false}};
    while (std::optional<SubExpr> subexpr = pop_expr_chain(subexpr_list.back(), context))
        subexpr_list.push_back(*subexpr);
    return subexpr_list;
}

// Returns the expression's subexpression, if it exists. When the subexpr
// is a try! macro, we'll convert it to shorthand when the option is set.
std::optional<SubExpr> Chain::pop_expr_chain(const SubExpr& expr, const RewriteContext& context) {
    const rust::Expr* e = expr.expr;
    switch (e->kind) {
    case rust::ExprKind::MethodCall: return SubExpr{convert_try(e->lhs, context), true};
    case rust::ExprKind::Field:
    case rust::ExprKind::Try:
    case rust::ExprKind::Await:
    case rust::ExprKind::Use: return SubExpr{convert_try(e->lhs, context), false};
    case rust::ExprKind::Yield:
        if (e->yield_kind == rust::YieldKind::Postfix && e->lhs) return SubExpr{convert_try(e->lhs, context), false};
        return std::nullopt;
    default: return std::nullopt;
    }
}

const rust::Expr* Chain::convert_try(const rust::Expr* expr, const RewriteContext&) {
    // non-default config: use_try_shorthand (convert_try_mac only runs when it is set)
    return expr;
}

// ChainFormatterShared + ChainFormatterBlock (the only formatter with the
// default indent_style = Block), merged into one struct.
struct ChainFormatterBlock {
    // The current working set of child items: chain.children[child_start..].
    const std::vector<ChainItem>* all_children;
    size_t child_start = 0;
    // The current rewrites of items (includes trailing `?`s, but not any way to
    // connect the rewrites together).
    std::vector<std::string> rewrites;
    // Whether the chain can fit on one line.
    bool fits_single_line = false;
    // The number of children in the chain (not the current working set's size).
    size_t child_count;
    // Whether elements are allowed to overflow past the max_width limit
    bool allow_overflow = false;
    bool root_ends_with_block = false;

    explicit ChainFormatterBlock(const Chain& chain)
        : all_children(&chain.children), child_count(chain.children.size()) {
        rewrites.reserve(chain.children.size() + 1);
    }

    size_t n_children() const { return all_children->size() - child_start; }
    const ChainItem& child(size_t k) const { return (*all_children)[child_start + k]; }
    bool children_empty() const { return n_children() == 0; }

    // ChainFormatterShared::pure_root
    Opt pure_root() {
        if (children_empty()) {
            if (rewrites.size() != 1) panic("pure_root: rewrites.len() != 1");
            std::string r = std::move(rewrites.back());
            rewrites.pop_back();
            return r;
        }
        return std::nullopt;
    }

    // ChainFormatterBlock::format_root
    UnitResult format_root(const ChainItem& parent, const RewriteContext& context, Shape shape) {
        RewriteResult rr = parent.rewrite_result(context, shape);
        if (!rr) return rr.error();
        std::string root_rewrite = std::move(*rr);

        bool root_ends_with_block_ = is_block_like(parent.kind, context, root_rewrite);
        size_t tab_width = sat_sub(context.config->tab_spaces(), shape.offset);

        while (root_rewrite.size() <= tab_width && !contains(root_rewrite, '\n')) {
            const ChainItem& item = child(0);
            if (item.kind.tag == ChainItemKindTag::Comment) break;
            std::optional<Shape> shape2 = shape.offset_left(root_rewrite.size());
            if (!shape2) return RewriteError::exceeds(shape.exceeds(item.span));
            RewriteResult r = item.rewrite_result(context, *shape2);
            if (r) {
                root_rewrite += *r;
            } else {
                break;
            }

            root_ends_with_block_ = last_line_extendable(root_rewrite);

            child_start += 1;
            if (children_empty()) break;
        }
        rewrites.push_back(std::move(root_rewrite));
        root_ends_with_block = root_ends_with_block_;
        return std::nullopt;
    }

    // ChainFormatterBlock::child_shape
    Shape child_shape(const RewriteContext& context, Shape shape, Span) const {
        bool block_end = root_ends_with_block;
        return get_block_child_shape(block_end, context, shape);
    }

    // ChainFormatterShared::format_children
    UnitResult format_children(const RewriteContext& context, Shape child_shape_) {
        for (size_t k = 0; k + 1 < n_children(); k++) {
            RewriteResult rewrite = format_chain_item(child(k), context, child_shape_, allow_overflow);
            if (!rewrite) return rewrite.error();
            rewrites.push_back(std::move(*rewrite));
        }
        return std::nullopt;
    }

    // ChainFormatterShared::format_last_child (ChainFormatterBlock passes may_extend = true).
    //
    // Rewrite the last child. The last child of a chain requires special treatment. We need to
    // know whether 'overflowing' the last child make a better formatting.
    UnitResult format_last_child(bool may_extend, const RewriteContext& context, Shape shape, Shape child_shape_) {
        if (children_empty()) return RewriteError::unknown();
        const ChainItem& last = child(n_children() - 1);
        bool extendable = may_extend && last_line_extendable(rewrites[0]);
        size_t prev_last_line_width = last_line_width(rewrites[0]);

        // Total of all items excluding the last.
        size_t almost_total;
        if (extendable) {
            almost_total = prev_last_line_width;
        } else {
            almost_total = 0;
            for (const std::string& rw : rewrites) almost_total += unicode_str_width(rw);
        }
        almost_total += last.tries;
        size_t one_line_budget =
            sat_sub(child_count == 1 ? shape.width : std::min(shape.width, context.config->chain_width()), almost_total);

        bool any_comment = false;
        for (size_t k = 0; k < n_children(); k++)
            if (child(k).is_comment()) any_comment = true;
        bool all_newline_free = true;
        for (const std::string& s : rewrites)
            if (contains(s, '\n')) all_newline_free = false;
        bool all_in_one_line = !any_comment && all_newline_free && one_line_budget > 0;

        std::optional<Shape> last_shape_o;
        if (all_in_one_line) {
            last_shape_o = shape.sub_width(last.tries);
        } else if (extendable) {
            last_shape_o = child_shape_.sub_width(last.tries);
        } else {
            last_shape_o = child_shape_.sub_width(shape.rhs_overhead(*context.config) + last.tries);
        }
        if (!last_shape_o) {
            // The error carries the shape the sub_width was taken from.
            Shape from = all_in_one_line ? shape : child_shape_;
            return RewriteError::exceeds(from.exceeds(last.span));
        }
        Shape last_shape = *last_shape_o;

        Opt last_subexpr_str;
        if (all_in_one_line || extendable) {
            // First we try to 'overflow' the last child and see if it looks better than using
            // vertical layout. (use_block_indent() is always true.)
            std::optional<Shape> one_line_shape = last_shape.offset_left_opt(almost_total);

            if (one_line_shape) {
                RewriteResult rwr = last.rewrite_result(context, *one_line_shape);
                if (rwr) {
                    std::string rw = std::move(*rwr);
                    // We allow overflowing here only if both of the following conditions match:
                    // 1. The entire chain fits in a single line except the last child.
                    // 2. `last_child_str.lines().count() >= 5`.
                    size_t line_count = lines(rw).size();
                    bool could_fit_single_line = first_line_width(rw) <= one_line_budget;
                    if (could_fit_single_line && line_count >= 5) {
                        last_subexpr_str = std::move(rw);
                        fits_single_line = all_in_one_line;
                    } else {
                        // We could not know whether overflowing is better than using vertical
                        // layout, just by looking at the overflowed rewrite. Now we rewrite the
                        // last child on its own line, and compare two rewrites to choose which is
                        // better.
                        std::optional<Shape> last_shape2 =
                            child_shape_.sub_width(shape.rhs_overhead(*context.config) + last.tries);
                        if (!last_shape2) return RewriteError::exceeds(child_shape_.exceeds(last.span));
                        RewriteResult new_rw = last.rewrite_result(context, *last_shape2);
                        if (new_rw && !could_fit_single_line) {
                            last_subexpr_str = std::move(*new_rw);
                        } else if (new_rw && lines(*new_rw).size() >= line_count) {
                            last_subexpr_str = std::move(rw);
                            fits_single_line = could_fit_single_line && all_in_one_line;
                        } else if (new_rw) {
                            last_subexpr_str = std::move(*new_rw);
                        } else {
                            last_subexpr_str = std::move(rw);
                            fits_single_line = could_fit_single_line && all_in_one_line;
                        }
                    }
                }
            }
        }

        // (use_block_indent() is always true: last_shape stays.)
        // `last_subexpr_str.unwrap_or(last.rewrite_result(..)?)`: the argument is
        // evaluated eagerly, so its error propagates even when there is a value.
        RewriteResult fallback = last.rewrite_result(context, last_shape);
        if (!fallback) return fallback.error();
        rewrites.push_back(last_subexpr_str ? std::move(*last_subexpr_str) : std::move(*fallback));
        return std::nullopt;
    }

    // ChainFormatterShared::join_rewrites
    RewriteResult join_rewrites(const RewriteContext& context, Shape child_shape_) const {
        std::string connector;
        if (fits_single_line) {
            // Yay, we can put everything on one line.
        } else {
            // Use new lines.
            if (context.force_one_line_chain) {
                if (children_empty()) return Err(RewriteError::unknown());
                return Err(RewriteError::exceeds(child_shape_.width, child(n_children() - 1).span));
            }
            connector = child_shape_.to_string_with_newline(*context.config);
        }

        std::string result = rewrites[0];
        for (size_t k = 1; k < rewrites.size() && k - 1 < n_children(); k++) {
            const ChainItem& chain_item = child(k - 1);
            if (chain_item.kind.tag == ChainItemKindTag::Comment && chain_item.kind.pos == CommentPosition::Back)
                result.push_back(' ');
            else
                result += connector;
            result += rewrites[k];
        }
        return result;
    }
};

// impl Rewrite for Chain
RewriteResult Chain::rewrite_result(const RewriteContext& context, Shape shape) const {
    // indent_style is Block (non-default config: indent_style = Visual uses ChainFormatterVisual).
    ChainFormatterBlock formatter(*this);

    if (UnitResult e = formatter.format_root(parent, context, shape)) return Err(*e);
    if (Opt result = formatter.pure_root())
        return max_width_error(wrap_str(std::move(*result), context.config->max_width(), shape), shape.width,
                               parent.span);

    const ChainItem& first = children.empty() ? parent : children.front();
    const ChainItem& last = children.empty() ? parent : children.back();
    Span children_span = mk_sp(first.span.lo, last.span.hi);
    Span full_span = parent.span.with_hi(children_span.hi);

    // Decide how to layout the rest of the chain.
    Shape child_shape = formatter.child_shape(context, shape, children_span);

    if (UnitResult e = formatter.format_children(context, child_shape)) return Err(*e);
    if (UnitResult e = formatter.format_last_child(true, context, shape, child_shape)) return Err(*e);

    RewriteResult result = formatter.join_rewrites(context, child_shape);
    if (!result) return result;
    return max_width_error(wrap_str(std::move(*result), context.config->max_width(), shape), shape.width,
                           full_span);
}

}  // namespace

RewriteResult rewrite_chain(const rust::Expr* expr, const RewriteContext& context, Shape shape) {
    Chain chain = Chain::from_ast(expr, context);

    // If this is just an expression with some `?`s, then format it trivially and
    // return early.
    if (chain.children.empty()) return chain.parent.rewrite_result(context, shape);

    return chain.rewrite_result(context, shape);
}

}  // namespace rustfmt
