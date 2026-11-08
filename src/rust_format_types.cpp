// rust_format_types.cpp -- rustfmt's types.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

namespace rustfmt {

namespace {

// `expr?` on a RewriteResult: binds the string or returns the error.
#define TY_TRY(var, expr)                                  \
    auto var##_res_ = (expr);                              \
    if (!var##_res_) return Err(var##_res_.error());       \
    std::string var = std::move(*var##_res_)

// `shape.<op>(n, span)?` on the Result<Shape, ExceedsMaxWidthError> forms.
#define TY_TRY_SHAPE(var, base, call, sp)                               \
    std::optional<Shape> var##_opt_ = (base).call;                      \
    if (!var##_opt_) return Err(RewriteError::exceeds((base).width, sp)); \
    Shape var = *var##_opt_

// `w.checked_sub(n).max_width_error(w, span)?`
#define TY_TRY_BUDGET(var, w, n, sp)                              \
    std::optional<size_t> var##_opt_ = checked_sub((w), (n));      \
    if (!var##_opt_) return Err(RewriteError::exceeds((w), sp));  \
    size_t var = *var##_opt_

// rustc's Span::new (mk_sp): swaps lo and hi when lo > hi.
Span sp_new(uint32_t lo, uint32_t hi) { return lo > hi ? Span(hi, lo) : Span(lo, hi); }

RewriteResult rewrite_generic_args(const rust::GenericArgs* gen_args, const RewriteContext& context, Shape shape,
                                   Span span);
RewriteResult join_bounds(const RewriteContext& context, Shape shape, const rust::GenericBounds& items,
                          bool need_indent);

// The rustfmt string literals for ast::BoundConstness / BoundAsyncness / BoundPolarity::as_str().
const char* bound_constness_str(rust::BoundConstnessKind k) {
    switch (k) {
    case rust::BoundConstnessKind::Never: return "";
    case rust::BoundConstnessKind::Always: return "const";
    case rust::BoundConstnessKind::Maybe: return "[const]";
    }
    return "";
}
const char* bound_polarity_str(rust::BoundPolarityKind k) {
    switch (k) {
    case rust::BoundPolarityKind::Positive: return "";
    case rust::BoundPolarityKind::Negative: return "!";
    case rust::BoundPolarityKind::Maybe: return "?";
    }
    return "";
}

// ---------------------------------------------------------------------------
// rewrite_segment / rewrite_path_segments

// Formats a path segment. There are some hacks involved to correctly determine
// the segment's associated span since it's not part of the AST.
//
// The span_lo is assumed to be greater than the end of any previous segment's
// parameters and lesser or equal than the start of current segment.
//
// span_hi is assumed equal to the end of the entire path.
//
// When the segment contains a positive number of parameters, we update span_lo
// so that invariants described above will hold for the next segment.
RewriteResult rewrite_segment(PathContext path_context, const rust::PathSegment& segment, uint32_t& span_lo,
                              uint32_t span_hi, const RewriteContext& context, Shape shape0) {
    std::string result;
    result.reserve(128);
    result += rewrite_ident(context, segment.ident);

    size_t ident_len = result.size();
    Span span = sp_new(span_lo, span_hi);
    // use_block_indent() is always true: offset_left (shrink_left is the Visual branch).
    TY_TRY_SHAPE(shape, shape0, offset_left(ident_len), span);

    if (segment.args) {
        TY_TRY(generics_str, rewrite_generic_args(segment.args, context, shape, sp_new(span_lo, span_hi)));
        const rust::GenericArgs& data = *segment.args;
        if (data.kind == rust::GenericArgs::AngleBracketed && !data.args.empty()) {
            // HACK: squeeze out the span between the identifier and the parameters.
            // The hack is required so that we don't remove the separator inside macro calls.
            // This does not work in the presence of comment, hoping that people are
            // sane about where to put their comment.
            std::string_view separator_snippet = trim(context.snippet(sp_new(segment.ident.span.hi, data.span.lo)));
            bool force_separator = context.inside_macro() && starts_with(separator_snippet, "::");
            const char* separator = (path_context == PathContext::Expr || force_separator) ? "::" : "";
            result += separator;

            // Update position of last bracket.
            span_lo = context.snippet_provider->span_after(sp_new(span_lo, span_hi), "<");
        }
        result += generics_str;
    }

    return result;
}

RewriteResult rewrite_path_segments(PathContext path_context, std::string buffer,
                                    const std::vector<rust::PathSegment>& segments, size_t begin, size_t end,
                                    uint32_t span_lo, uint32_t span_hi, const RewriteContext& context,
                                    Shape shape0) {
    bool first = true;
    Shape shape = shape0.visual_indent(0);

    for (size_t i = begin; i < end && i < segments.size(); i++) {
        const rust::PathSegment& segment = segments[i];
        // Indicates a global path, shouldn't be rendered.
        if (segment.ident.name == "{{root}}") continue;  // kw::PathRoot
        if (first) {
            first = false;
        } else {
            buffer += "::";
        }

        size_t extra = extra_offset(buffer, shape);
        TY_TRY_SHAPE(new_shape, shape, shrink_left(extra), sp_new(span_lo, span_hi));
        TY_TRY(segment_string, rewrite_segment(path_context, segment, span_lo, span_hi, context, new_shape));

        buffer += segment_string;
    }

    return buffer;
}

// ---------------------------------------------------------------------------
// format_function_type (Fn(A) -> B sugar and fn pointers)

// If the return type is multi-lined, then force to use multiple lines for
// arguments as well.
DefinitiveListTactic get_tactics(const std::vector<ListItem>& item_vec, std::string_view output, Shape shape) {
    if (contains(output, '\n')) {
        return DefinitiveListTactic::Vertical();
    }
    return definitive_tactic(item_vec, ListTactic::HorizontalVertical(), Separator::Comma,
                             // 2 is for the case of ',\n'
                             sat_sub(shape.width, 2 + output.size()));
}

template <class T>
RewriteResult format_function_type(const std::vector<T*>& inputs, const rust::FnRetTy& output_ty, bool variadic,
                                   Span span, const RewriteContext& context, Shape shape) {
    // IndentStyle::Block: 4 = " -> " (block_left is the Visual branch).
    TY_TRY_SHAPE(ty_shape, shape, offset_left(4), span);
    std::string output;
    if (output_ty.ty) {
        TY_TRY(type_str, rewrite_result(context, output_ty.ty, ty_shape));
        output = " -> " + type_str;
    }

    // use_block_indent() is always true.
    Shape list_shape = Shape::indented(shape.block().indent.block_indent_(*context.config), *context.config);

    bool is_inputs_empty = inputs.empty();
    uint32_t list_lo = context.snippet_provider->span_after(span, "(");
    std::string list_str;
    DefinitiveListTactic tactic;
    if (is_inputs_empty) {
        tactic = get_tactics({}, output, shape);
        uint32_t list_hi = context.snippet_provider->span_before(span, ")");
        std::optional<std::string_view> snip = context.snippet_provider->span_to_snippet(mk_sp(list_lo, list_hi));
        if (!snip) return Err(RewriteError::unknown());
        std::string_view comment = trim(*snip);
        if (starts_with(comment, "//")) {
            list_str = list_shape.indent.to_string_with_newline(*context.config) + std::string(comment) +
                       shape.block().indent.to_string_with_newline(*context.config);
        } else {
            list_str = std::string(comment);
        }
    } else {
        std::vector<ListItem> item_vec = itemize_list(
            *context.snippet_provider, inputs.size(), ")", ",",
            [&](size_t i) { return spanned(inputs[i]).lo; },
            [&](size_t i) { return spanned(inputs[i]).hi; },
            [&](size_t i) { return rewrite_result(context, inputs[i], list_shape); },
            list_lo, span.hi, false);

        tactic = get_tactics(item_vec, output, shape);
        // !use_block_indent() is always false; trailing_comma() = Vertical.
        SeparatorTactic trailing_separator = variadic ? SeparatorTactic::Never : SeparatorTactic::Vertical;

        ListFormatting fmt(list_shape, *context.config);
        fmt.tactic(tactic)
            .trailing_separator(trailing_separator)
            .ends_with_newline(tactic.ends_with_newline(context.config->indent_style()))
            .preserve_newline(true);
        TY_TRY(written, write_list(item_vec, fmt));
        list_str = std::move(written);
    }

    std::string args;
    if (tactic == DefinitiveListTactic::Horizontal() || is_inputs_empty) {
        args = "(" + list_str + ")";
    } else {
        args = "(" + list_shape.indent.to_string_with_newline(*context.config) + list_str +
               shape.block().indent.to_string_with_newline(*context.config) + ")";
    }
    if (output.empty() || last_line_width(args) + first_line_width(output) <= shape.width) {
        return args + output;
    }
    return args + "\n" + list_shape.indent.to_string(*context.config) + std::string(trim_start(output));
}

const char* type_bound_colon(const RewriteContext& context) { return colon_spaces(*context.config); }

RewriteResult rewrite_generic_args(const rust::GenericArgs* gen_args, const RewriteContext& context, Shape shape,
                                   Span span) {
    switch (gen_args->kind) {
    case rust::GenericArgs::AngleBracketed: {
        if (gen_args->args.empty()) return std::string();
        std::vector<SegmentParam> params;
        params.reserve(gen_args->args.size());
        for (const rust::AngleBracketedArg& a : gen_args->args) params.push_back(SegmentParam::from_generic_arg(a));
        std::vector<OverflowableItem> args;
        args.reserve(params.size());
        for (const SegmentParam& p : params) args.push_back(RwNode::of(&p));
        return rewrite_with_angle_brackets(context, "", args, shape, span);
    }
    case rust::GenericArgs::Parenthesized:
        return format_function_type(gen_args->inputs, gen_args->output, false, gen_args->span, context, shape);
    case rust::GenericArgs::ParenthesizedElided:
        return std::string("(..)");
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_bounded_lifetime(const rust::Lifetime& lt, const rust::GenericBounds& bounds, Span span,
                                       const RewriteContext& context, Shape shape0) {
    TY_TRY(result, rewrite_result(context, &lt, shape0));

    if (bounds.empty()) return result;
    const char* colon = type_bound_colon(context);
    size_t overhead = last_line_width(result) + std::string_view(colon).size();
    TY_TRY_SHAPE(shape, shape0, sub_width(overhead), span);
    TY_TRY(joined, join_bounds(context, shape, bounds, true));
    return result + colon + joined;
}

RewriteResult rewrite_fn_ptr(const rust::FnPtrTy* fn_ptr, Span span, const RewriteContext& context, Shape shape) {
    std::string result;
    result.reserve(128);

    if (Opt lifetime_str = rewrite_bound_params(context, shape, fn_ptr->generic_params)) {
        result += "for<";
        // 6 = "for<> ".len(), 4 = "for<".
        // This doesn't work out so nicely for multiline situation with lots of
        // rightward drift. If that is a problem, we could use the list stuff.
        result += *lifetime_str;
        result += "> ";
    }

    result += format_safety(fn_ptr->safety);

    // force_explicit_abi() is true.
    result += format_extern(context, fn_ptr->ext, context.config->force_explicit_abi());

    result += "fn";

    // use_block_indent(): offset_left (visual_indent + sub_width is the Visual branch).
    TY_TRY_SHAPE(func_ty_shape, shape, offset_left(result.size()), span);

    TY_TRY(rewrite, format_function_type(fn_ptr->decl->inputs, fn_ptr->decl->output, fn_ptr->decl->c_variadic(),
                                         span, context, func_ty_shape));

    result += rewrite;
    return result;
}

bool is_trait_bound(const rust::GenericBound& b) { return b.kind != rust::GenericBound::Outlives; }

bool is_generic_bounds_in_order(const rust::GenericBounds& generic_bounds) {
    std::optional<size_t> last_trait_index;
    for (size_t i = generic_bounds.size(); i-- > 0;) {
        if (is_trait_bound(generic_bounds[i])) {
            last_trait_index = i;
            break;
        }
    }
    std::optional<size_t> first_lifetime_index;
    for (size_t i = 0; i < generic_bounds.size(); i++) {
        if (!is_trait_bound(generic_bounds[i])) {
            first_lifetime_index = i;
            break;
        }
    }
    if (last_trait_index && first_lifetime_index) return *last_trait_index < *first_lifetime_index;
    return true;
}

bool is_bound_extendable(std::string_view s, const rust::GenericBound& b) {
    switch (b.kind) {
    case rust::GenericBound::Outlives: return true;
    // We treat `use<>` like a trait bound here.
    case rust::GenericBound::Trait:
    case rust::GenericBound::Use: return last_line_extendable(s);
    }
    return false;
}

// Whether a GenericBound item is a PathSegment segment that includes internal array
// that contains more than one item
bool is_item_with_multi_items_array(const rust::GenericBound& item) {
    switch (item.kind) {
    case rust::GenericBound::Trait: {
        const std::vector<rust::PathSegment>& segments = item.trait.trait_ref->segments;
        if (segments.size() > 1) return true;
        if (segments.empty()) panic("join_bounds: empty trait path", item.span().lo);
        if (const rust::GenericArgs* args_in = segments[0].args) {
            return args_in->kind == rust::GenericArgs::AngleBracketed && args_in->args.size() > 1;
        }
        return false;
    }
    case rust::GenericBound::Use: return item.use_args.size() > 1;
    default: return false;
    }
}

RewriteResult join_bounds_inner(const RewriteContext& context, Shape shape0, const rust::GenericBounds& items,
                                bool need_indent, bool force_newline) {
    bool generic_bounds_in_order = is_generic_bounds_in_order(items);

    // try_fold state: (strs, prev_trailing_span, prev_extendable)
    std::string strs;
    std::optional<Span> prev_trailing_span;
    bool prev_extendable = false;
    for (size_t i = 0; i < items.size(); i++) {
        const rust::GenericBound& item = items[i];
        std::optional<Span> trailing_span;
        if (i < items.size() - 1) {
            uint32_t hi =
                context.snippet_provider->span_before(sp_new(items[i + 1].span().lo, item.span().hi), "+");
            trailing_span = sp_new(item.span().hi, hi);
        }
        std::optional<Span> leading_span;
        bool has_leading_comment = false;
        if (i > 0) {
            uint32_t lo =
                context.snippet_provider->span_after(sp_new(items[i - 1].span().hi, item.span().lo), "+");
            Span span = sp_new(lo, item.span().lo);
            bool has_comments = contains_comment(context.snippet(span));
            leading_span = sp_new(lo, item.span().lo);
            has_leading_comment = has_comments;
        }
        bool prev_has_trailing_comment = prev_trailing_span ? contains_comment(context.snippet(*prev_trailing_span)) : false;

        Shape shape = (need_indent && force_newline)
                          ? shape0.block_indent(context.config->tab_spaces()).with_max_width(*context.config)
                          : shape0;
        std::string whitespace = (force_newline && (!prev_extendable || !generic_bounds_in_order))
                                     ? shape.indent.to_string_with_newline(*context.config)
                                     : std::string(" ");

        // type_punctuation_density() = Wide.
        std::string joiner_owned = whitespace + "+ ";
        std::string_view joiner = joiner_owned;
        if (has_leading_comment) joiner = trim_end(joiner);
        if (prev_has_trailing_comment) joiner = trim_start(joiner);

        bool extendable;
        std::string trailing_str;
        if (i == 0) {
            TY_TRY(bound_str, rewrite_result(context, &item, shape));
            extendable = is_bound_extendable(bound_str, item);
            trailing_str = std::move(bound_str);
        } else {
            TY_TRY(bound_str, rewrite_result(context, &item, shape));
            if (leading_span && has_leading_comment) {
                extendable = is_bound_extendable(bound_str, item);
                TY_TRY(combined, combine_strs_with_missing_comments(context, joiner, bound_str, *leading_span,
                                                                    shape, true));
                trailing_str = std::move(combined);
            } else {
                extendable = is_bound_extendable(bound_str, item);
                trailing_str = std::string(joiner) + bound_str;
            }
        }
        if (prev_trailing_span && prev_has_trailing_comment) {
            TY_TRY(v, combine_strs_with_missing_comments(context, strs, trailing_str, *prev_trailing_span, shape,
                                                         true));
            strs = std::move(v);
        } else {
            strs += trailing_str;
        }
        prev_trailing_span = trailing_span;
        prev_extendable = extendable;
    }

    // Whether to retry with a forced newline:
    //   Only if result is not already multiline and did not exceed line width,
    //   and either there is more than one item;
    //       or the single item is of type `Trait`,
    //          and any of the internal arrays contains more than one item;
    bool retry_with_force_newline;
    if (context.config->style_edition() <= StyleEdition::E2021) {
        retry_with_force_newline =
            !force_newline && items.size() > 1 && (contains(strs, '\n') || strs.size() > shape0.width);
    } else if (force_newline) {
        retry_with_force_newline = false;
    } else if (!contains(strs, '\n') && strs.size() <= shape0.width) {
        retry_with_force_newline = false;
    } else if (items.size() > 1) {
        retry_with_force_newline = true;
    } else {
        retry_with_force_newline = is_item_with_multi_items_array(items[0]);
    }

    if (retry_with_force_newline) return join_bounds_inner(context, shape0, items, need_indent, true);
    return strs;
}

RewriteResult join_bounds(const RewriteContext& context, Shape shape, const rust::GenericBounds& items,
                          bool need_indent) {
    return join_bounds_inner(context, shape, items, need_indent, false);
}

Span bounds_span(const rust::GenericBounds& b) {
    if (b.empty()) return Span();
    return Span(b.front().span().lo, b.back().span().hi);
}

}  // namespace

// ---------------------------------------------------------------------------
// rewrite_path

// Does not wrap on simple segments.
RewriteResult rewrite_path(const RewriteContext& context, PathContext path_context, const rust::QSelf* qself,
                           const rust::Path* path, Shape shape) {
    size_t skip_count = qself ? qself->position : 0;

    std::string result;
    result.reserve(32);

    if (path_is_global(path) && !qself && path_context != PathContext::Import) {
        result += "::";
    }

    uint32_t span_lo = path->span.lo;

    if (qself) {
        result += '<';

        TY_TRY(fmt_ty, rewrite_result(context, qself->ty, shape));
        result += fmt_ty;

        if (skip_count > 0) {
            result += " as ";
            if (path_is_global(path) && path_context != PathContext::Import) {
                result += "::";
            }

            // 3 = ">::".len()
            TY_TRY_SHAPE(shape2, shape, sub_width(3), path->span);

            TY_TRY(r, rewrite_path_segments(PathContext::Type, std::move(result), path->segments, 0, skip_count,
                                            span_lo, path->span.hi, context, shape2));
            result = std::move(r);
        }

        result += ">::";
        span_lo = qself->ty->span.hi + 1;
    }

    return rewrite_path_segments(path_context, std::move(result), path->segments, skip_count, path->segments.size(),
                                 span_lo, path->span.hi, context, shape);
}

// ---------------------------------------------------------------------------
// SegmentParam

SegmentParam SegmentParam::from_generic_arg(const rust::AngleBracketedArg& arg) {
    SegmentParam p;
    if (arg.is_constraint) {
        p.kind = Binding;
        p.binding = arg.constraint;
        return p;
    }
    switch (arg.arg.kind) {
    case rust::GenericArg::Lifetime:
        p.kind = LifeTime;
        p.lifetime = &arg.arg.lifetime;
        break;
    case rust::GenericArg::Type:
        p.kind = Type;
        p.ty = arg.arg.ty;
        break;
    case rust::GenericArg::Const:
        p.kind = Const;
        p.konst = &arg.arg.konst;
        break;
    }
    return p;
}

Span spanned(const SegmentParam* x) {
    switch (x->kind) {
    case SegmentParam::Const: return x->konst->value->span;
    case SegmentParam::LifeTime: return x->lifetime->ident.span;
    case SegmentParam::Type: return x->ty->span;
    case SegmentParam::Binding: return x->binding->span;
    }
    return Span();
}

RewriteResult rewrite_result(const RewriteContext& ctx, const SegmentParam* x, Shape shape) {
    switch (x->kind) {
    case SegmentParam::Const: return rewrite_result(ctx, x->konst, shape);
    case SegmentParam::LifeTime: return rewrite_result(ctx, x->lifetime, shape);
    case SegmentParam::Type: return rewrite_result(ctx, x->ty, shape);
    case SegmentParam::Binding: return rewrite_result(ctx, x->binding, shape);
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_result(const RewriteContext& ctx, const rust::PreciseCapturingArg* x, Shape shape) {
    if (x->is_lifetime) return rewrite_result(ctx, &x->lifetime, shape);
    return rewrite_path(ctx, PathContext::Type, nullptr, x->path, shape);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::AssocItemConstraint* x, Shape shape) {
    std::string result;
    result.reserve(128);
    result += rewrite_ident(context, x->ident);

    if (x->gen_args) {
        TY_TRY_BUDGET(budget, shape.width, result.size(), x->span);
        Shape shape2 = Shape::legacy(budget, shape.indent + result.size());
        TY_TRY(gen_str, rewrite_generic_args(x->gen_args, context, shape2, x->gen_args->span));
        result += gen_str;
    }

    // type_punctuation_density() = Wide.
    const char* infix = x->is_equality ? " = " : ": ";
    result += infix;

    TY_TRY_BUDGET(budget, shape.width, result.size(), x->span);
    Shape shape3 = Shape::legacy(budget, shape.indent + result.size());
    // impl Rewrite for ast::AssocItemConstraintKind
    RewriteResult rewrite = RewriteResult(RewriteError::unknown());
    if (x->is_equality) {
        if (x->term_ty) {
            rewrite = rewrite_result(context, x->term_ty, shape3);
        } else {
            rewrite = rewrite_result(context, &x->term_const, shape3);
        }
    } else {
        static const rust::GenericBounds kNoBounds;
        rewrite = rewrite_result(context, x->bounds ? x->bounds : &kNoBounds, shape3);
    }
    if (!rewrite) return Err(rewrite.error());
    result += *rewrite;

    return result;
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::WherePredicate* x, Shape shape) {
    TY_TRY(attrs_str, rewrite_attrs(context, x->attrs, shape));
    // FIXME: dead spans?
    std::string pred_str;
    switch (x->kind) {
    case rust::WherePredicate::Bound: {
        TY_TRY(type_str, rewrite_result(context, x->bounded_ty, shape));
        std::string_view colon = trim_end(type_bound_colon(context));
        std::string lhs;
        if (Opt binder_str = rewrite_bound_params(context, shape, x->bound_generic_params)) {
            lhs = "for<" + *binder_str + "> " + type_str + std::string(colon);
        } else {
            lhs = type_str + std::string(colon);
        }
        TY_TRY(r, rewrite_assign_rhs(context, std::move(lhs), RwNode::bounds(&x->bounds, bounds_span(x->bounds)),
                                     RhsAssignKind::bounds(), shape));
        pred_str = std::move(r);
        break;
    }
    case rust::WherePredicate::Region: {
        TY_TRY(r, rewrite_bounded_lifetime(x->lifetime, x->bounds, x->span, context, shape));
        pred_str = std::move(r);
        break;
    }
    case rust::WherePredicate::Eq: {
        TY_TRY(lhs, rewrite_result(context, x->lhs_ty, shape));
        std::string lhs_ty_str = lhs + " =";
        TY_TRY(r, rewrite_assign_rhs(context, std::move(lhs_ty_str), RwNode::of(x->rhs_ty), RhsAssignKind::ty(),
                                     shape));
        pred_str = std::move(r);
        break;
    }
    }

    std::string result;
    result.reserve(attrs_str.size() + pred_str.size() + 1);
    result += attrs_str;
    uint32_t pred_start = x->span.lo;
    size_t line_len = last_line_width(attrs_str) + 1 + first_line_width(pred_str);
    const rust::Attribute* last_attr = x->attrs.empty() ? nullptr : &x->attrs.back();
    if (last_attr && contains_comment(context.snippet(sp_new(last_attr->span.hi, pred_start)))) {
        Shape s = shape;
        s.width = std::min(shape.width, context.config->inline_attribute_width());
        TY_TRY(r, combine_strs_with_missing_comments(context, result, pred_str, sp_new(last_attr->span.hi, pred_start),
                                                     s, !last_attr->is_doc_comment));
        result = std::move(r);
    } else {
        if (!x->attrs.empty()) {
            if (context.config->inline_attribute_width() < line_len || x->attrs.size() > 1 ||
                x->attrs.back().is_doc_comment) {
                result += shape.indent.to_string_with_newline(*context.config);
            } else {
                result += ' ';
            }
        }
        result += pred_str;
    }

    return result;
}

RewriteResult rewrite_result(const RewriteContext& ctx, const rust::GenericArg* x, Shape shape) {
    switch (x->kind) {
    case rust::GenericArg::Lifetime: return rewrite_result(ctx, &x->lifetime, shape);
    case rust::GenericArg::Type: return rewrite_result(ctx, x->ty, shape);
    case rust::GenericArg::Const: return rewrite_result(ctx, &x->konst, shape);
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_result(const RewriteContext& ctx, const rust::AnonConst* x, Shape shape) {
    return format_expr(x->value, ExprType::SubExpression, ctx, shape);
}

RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Lifetime* x, Shape) {
    return std::string(ctx.snippet(x->ident.span));
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::GenericBound* x, Shape shape) {
    switch (x->kind) {
    case rust::GenericBound::Trait: {
        std::string_view snippet = context.snippet(x->span());
        bool has_paren = starts_with(snippet, "(") && ends_with(snippet, ")");
        TY_TRY(s, rewrite_result(context, &x->trait, shape));
        if (has_paren) return "(" + s + ")";
        return s;
    }
    case rust::GenericBound::Use: {
        std::vector<OverflowableItem> args;
        args.reserve(x->use_args.size());
        for (const rust::PreciseCapturingArg& a : x->use_args) args.push_back(RwNode::of(&a));
        return rewrite_with_angle_brackets(context, "use", args, shape, x->use_span);
    }
    case rust::GenericBound::Outlives: return rewrite_result(context, &x->lifetime, shape);
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::GenericBounds* x, Shape shape) {
    if (x->empty()) return std::string();
    return join_bounds(context, shape, *x, true);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::GenericParam* x, Shape shape) {
    // FIXME: If there are more than one attributes, this will force multiline.
    RewriteResult attrs_rw = rewrite_attrs(context, x->attrs, shape);
    std::string result = attrs_rw ? *attrs_rw : std::string();
    bool has_attrs = !result.empty();

    std::string param;
    param.reserve(128);

    uint32_t param_start;
    if (x->kind == rust::GenericParam::Const) {
        param += "const ";
        param += rewrite_ident(context, x->ident);
        param += ": ";
        TY_TRY(ty_str, rewrite_result(context, x->const_ty, shape));
        param += ty_str;
        if (x->const_default) {
            // type_punctuation_density() = Wide.
            param += " = ";
            TY_TRY_BUDGET(budget, shape.width, param.size(), spanned(x));
            TY_TRY(rewrite, rewrite_result(context, x->const_default, Shape::legacy(budget, shape.indent)));
            param += rewrite;
        }
        param_start = x->const_span.lo;
    } else {
        param += rewrite_ident(context, x->ident);
        param_start = x->ident.span.lo;
    }

    if (!x->bounds.empty()) {
        param += type_bound_colon(context);
        TY_TRY(b, rewrite_result(context, &x->bounds, shape));
        param += b;
    }
    if (x->kind == rust::GenericParam::Type && x->default_ty) {
        // type_punctuation_density() = Wide.
        param += " = ";
        TY_TRY_BUDGET(budget, shape.width, param.size(), spanned(x));
        TY_TRY(rewrite, rewrite_result(context, x->default_ty, Shape::legacy(budget, shape.indent + param.size())));
        param += rewrite;
    }

    const rust::Attribute* last_attr = x->attrs.empty() ? nullptr : &x->attrs.back();
    if (last_attr && contains_comment(context.snippet(sp_new(last_attr->span.hi, param_start)))) {
        TY_TRY(r, combine_strs_with_missing_comments(context, result, param, sp_new(last_attr->span.hi, param_start),
                                                     shape, !last_attr->is_doc_comment));
        result = std::move(r);
    } else {
        // When rewriting generic params, an extra newline should be put
        // if the attributes end with a doc comment
        if (last_attr && last_attr->is_doc_comment) {
            result += shape.indent.to_string_with_newline(*context.config);
        } else if (has_attrs) {
            result += ' ';
        }
        result += param;
    }

    return result;
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::PolyTraitRef* x, Shape shape0) {
    std::string binder;
    Shape shape = shape0;
    if (Opt lifetime_str = rewrite_bound_params(context, shape0, x->bound_generic_params)) {
        // 6 is "for<> ".len()
        size_t extra = lifetime_str->size() + 6;
        TY_TRY_SHAPE(s, shape0, offset_left(extra), x->span);
        binder = "for<" + *lifetime_str + "> ";
        shape = s;
    }

    std::string constness = bound_constness_str(x->modifiers.constness);
    if (!constness.empty()) constness += ' ';
    std::string asyncness = x->modifiers.async_ ? "async" : "";
    if (!asyncness.empty()) asyncness += ' ';
    std::string_view polarity = bound_polarity_str(x->modifiers.polarity);
    TY_TRY_SHAPE(shape2, shape, offset_left(constness.size() + polarity.size()), x->span);

    TY_TRY(path_str, rewrite_trait_ref(context, x->trait_ref, shape2));
    return binder + constness + asyncness + std::string(polarity) + path_str;
}

RewriteResult rewrite_trait_ref(const RewriteContext& ctx, const rust::Path* trait_ref, Shape shape) {
    return rewrite_path(ctx, PathContext::Type, nullptr, trait_ref, shape);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::Ty* x, Shape shape) {
    DepthGuard g(*context.state);
    switch (x->kind) {
    case rust::TyKind::TraitObject: {
        // we have to consider 'dyn' keyword is used or not!!!
        Shape s = shape;
        const char* prefix = "";
        if (x->dyn_syntax) {
            TY_TRY_SHAPE(s2, shape, offset_left(4), spanned(x));
            s = s2;
            prefix = "dyn ";
        }
        TY_TRY(res, rewrite_result(context, &x->bounds, s));
        // We may have falsely removed a trailing `+` inside macro call.
        if (context.inside_macro() && x->bounds.size() == 1 && ends_with(context.snippet(x->span), "+") &&
            !ends_with(res, "+")) {
            res += '+';
        }
        return prefix + res;
    }
    case rust::TyKind::Ptr: {
        const char* prefix = x->mutbl == rust::Mutability::Mut ? "*mut " : "*const ";
        return rewrite_unary_prefix(context, prefix, RwNode::of(x->inner), shape);
    }
    case rust::TyKind::Ref:
    case rust::TyKind::PinnedRef: {
        const char* mut_str = format_mutability(x->mutbl);
        size_t mut_len = std::string_view(mut_str).size();
        std::string result;
        result.reserve(128);
        result += '&';
        uint32_t ref_hi = context.snippet_provider->span_after(spanned(x), "&");
        uint32_t cmnt_lo = ref_hi;

        if (x->has_lifetime) {
            const rust::Lifetime& lifetime = x->lifetime;
            TY_TRY_BUDGET(lt_budget, shape.width, 2 + mut_len, spanned(x));
            TY_TRY(lt_str, rewrite_result(context, &lifetime, Shape::legacy(lt_budget, shape.indent + 2 + mut_len)));
            Span before_lt_span = sp_new(cmnt_lo, lifetime.ident.span.lo);
            if (contains_comment(context.snippet(before_lt_span))) {
                TY_TRY(r, combine_strs_with_missing_comments(context, result, lt_str, before_lt_span, shape, true));
                result = std::move(r);
            } else {
                result += lt_str;
            }
            result += ' ';
            cmnt_lo = lifetime.ident.span.hi;
        }

        if (x->kind == rust::TyKind::PinnedRef) {
            result += "pin ";
            if (x->mutbl == rust::Mutability::Not) result += "const ";
        }

        if (x->mutbl == rust::Mutability::Mut) {
            uint32_t mut_hi = context.snippet_provider->span_after(spanned(x), "mut");
            Span before_mut_span = sp_new(cmnt_lo, mut_hi - 3);
            if (contains_comment(context.snippet(before_mut_span))) {
                TY_TRY(r, combine_strs_with_missing_comments(context, trim_end(result), mut_str, before_mut_span,
                                                             shape, true));
                result = std::move(r);
            } else {
                result += mut_str;
            }
            cmnt_lo = mut_hi;
        }

        Span before_ty_span = sp_new(cmnt_lo, x->inner->span.lo);
        if (contains_comment(context.snippet(before_ty_span))) {
            TY_TRY(ty_str, rewrite_result(context, x->inner, shape));
            TY_TRY(r, combine_strs_with_missing_comments(context, trim_end(result), ty_str, before_ty_span, shape,
                                                         true));
            result = std::move(r);
        } else {
            size_t used_width = last_line_width(result);
            TY_TRY_BUDGET(budget, shape.width, used_width, spanned(x));
            TY_TRY(ty_str, rewrite_result(context, x->inner, Shape::legacy(budget, shape.indent + used_width)));
            result += ty_str;
        }

        return result;
    }
    // FIXME: we drop any comments here, even though it's a silly place to put
    // comments.
    case rust::TyKind::Paren: {
        // (indent_style() == Visual is non-default config)
        if (context.config->style_edition() <= StyleEdition::E2021) {
            TY_TRY_BUDGET(budget, shape.width, 2, spanned(x));
            TY_TRY(ty_str, rewrite_result(context, x->inner, Shape::legacy(budget, shape.indent + 1)));
            return "(" + ty_str + ")";
        }

        // 2 = ()
        if (std::optional<Shape> sh = shape.sub_width_opt(2)) {
            RewriteResult s = rewrite_result(context, x->inner, *sh);
            if (s && !contains(*s, '\n')) return "(" + *s + ")";
        }

        std::string indent_str = shape.indent.to_string_with_newline(*context.config);
        Shape shape2 = shape.block_indent(context.config->tab_spaces()).with_max_width(*context.config);
        TY_TRY(rw, rewrite_result(context, x->inner, shape2));
        return "(" + shape2.to_string_with_newline(*context.config) + rw + indent_str + ")";
    }
    case rust::TyKind::Slice: {
        TY_TRY_BUDGET(budget, shape.width, 4, spanned(x));
        TY_TRY(ty_str, rewrite_result(context, x->inner, Shape::legacy(budget, shape.indent + 1)));
        return "[" + ty_str + "]";
    }
    case rust::TyKind::Tup: {
        std::vector<OverflowableItem> items;
        items.reserve(x->elems.size());
        for (const rust::Ty* t : x->elems) items.push_back(RwNode::of(t));
        return rewrite_tuple(context, items, x->span, shape, x->elems.size() == 1);
    }
    case rust::TyKind::Path: return rewrite_path(context, PathContext::Type, x->qself, x->path, shape);
    case rust::TyKind::Array:
        return rewrite_pair(RwNode::of(x->inner), RwNode::of(x->len.value), PairParts{"[", "; ", "]"}, context, shape,
                            SeparatorPlace::Back);
    case rust::TyKind::Infer:
        if (shape.width >= 1) return std::string("_");
        return Err(RewriteError::exceeds(shape.width, spanned(x)));
    case rust::TyKind::FnPtr: return rewrite_fn_ptr(x->fn_ptr, x->span, context, shape);
    case rust::TyKind::Never: return std::string("!");
    case rust::TyKind::MacCall: return rewrite_macro(x->mac, context, shape, MacroPosition::Expression, x->span);
    case rust::TyKind::ImplicitSelf: return std::string();
    case rust::TyKind::ImplTrait: {
        // Empty trait is not a parser error.
        if (x->bounds.empty()) return std::string("impl");
        RewriteResult rw = RewriteResult(RewriteError::unknown());
        if (context.config->style_edition() <= StyleEdition::E2021) {
            rw = rewrite_result(context, &x->bounds, shape);
        } else if (context.config->style_edition() == StyleEdition::E2024) {
            rw = join_bounds(context, shape, x->bounds, false);
        } else {
            size_t offset = 5;  // "impl ".len()
            TY_TRY_SHAPE(s, shape, offset_left(offset), spanned(x));
            rw = join_bounds(context, s, x->bounds, false);
        }
        if (!rw) return Err(rw.error());
        const char* space = rw->empty() ? "" : " ";
        return std::string("impl") + space + *rw;
    }
    case rust::TyKind::CVarArgs: return std::string("...");
    case rust::TyKind::Dummy:
    case rust::TyKind::Err: return std::string(context.snippet(x->span));
    // TyKind::Typeof is gone from rustc 1.97's AST (rustc rejects `typeof`); kept verbatim like Err.
    case rust::TyKind::Typeof: return std::string(context.snippet(x->span));
    case rust::TyKind::Pat: {
        TY_TRY(ty, rewrite_result(context, x->inner, shape));
        TY_TRY(pat, rewrite_result(context, x->pat, shape));
        return ty + " is " + pat;
    }
    case rust::TyKind::FieldOf: {
        TY_TRY(ty, rewrite_result(context, x->inner, shape));
        // Ident's Display: raw identifiers print with `r#`.
        auto ident_str = [](const rust::Ident& id) { return (id.raw ? "r#" : "") + std::string(id.name); };
        if (x->has_variant) {
            return "builtin # field_of(" + ty + ", " + ident_str(x->variant) + "." + ident_str(x->field) + ")";
        }
        return "builtin # field_of(" + ty + ", " + ident_str(x->field) + ")";
    }
    case rust::TyKind::UnsafeBinder: {
        std::string result;
        if (x->generic_params.empty()) {
            // We always want to write `unsafe<>` since `unsafe<> Ty`
            // and `Ty` are distinct types.
            result += "unsafe<> ";
        } else if (Opt lifetime_str = rewrite_bound_params(context, shape, x->generic_params)) {
            result += "unsafe<";
            result += *lifetime_str;
            result += "> ";
        }

        // use_block_indent(): offset_left.
        TY_TRY_SHAPE(inner_ty_shape, shape, offset_left(result.size()), spanned(x));

        TY_TRY(rewrite, rewrite_result(context, x->inner, inner_ty_shape));
        result += rewrite;
        return result;
    }
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::TyPat* x, Shape shape) {
    switch (x->kind) {
    case rust::TyPat::Range:
        return rewrite_range_pat(context, shape, x->start ? x->start->value : nullptr,
                                 x->end ? x->end->value : nullptr, x->range_end, x->range_end_span, x->span);
    case rust::TyPat::Or: {
        bool first = true;
        std::string s;
        for (const rust::TyPat* variant : x->alts) {
            if (first) {
                first = false;
            } else {
                s += " | ";
            }
            TY_TRY(v, rewrite_result(context, variant, shape));
            s += v;
        }
        return s;
    }
    case rust::TyPat::NotNull:
    case rust::TyPat::Err: return Err(RewriteError::unknown());
    }
    return Err(RewriteError::unknown());
}

const rust::GenericBounds* opaque_ty(const rust::Ty* ty) {
    if (ty && ty->kind == rust::TyKind::ImplTrait) return &ty->bounds;
    return nullptr;
}

bool can_be_overflowed_type(const RewriteContext& context, const rust::Ty* ty, size_t len) {
    switch (ty->kind) {
    case rust::TyKind::Tup: return context.use_block_indent() && len == 1;
    case rust::TyKind::Ref:
    case rust::TyKind::PinnedRef:
    case rust::TyKind::Ptr: return can_be_overflowed_type(context, ty->inner, len);
    default: return false;
    }
}

/// Returns `None` if there is no `GenericParam` in the list
Opt rewrite_bound_params(const RewriteContext& context, Shape shape, const std::vector<rust::GenericParam*>& params) {
    std::vector<std::string> strs;
    strs.reserve(params.size());
    for (const rust::GenericParam* param : params) {
        Opt r = rewrite_result(context, param, shape).opt();
        if (!r) return std::nullopt;
        strs.push_back(std::move(*r));
    }
    std::string result = join(strs, ", ");
    if (result.empty()) return std::nullopt;
    return result;
}

}  // namespace rustfmt
