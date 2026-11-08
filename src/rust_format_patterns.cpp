// rust_format_patterns.cpp -- rustfmt's patterns.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

namespace rustfmt {

namespace {

// `expr?` on a RewriteResult: binds the string or returns the error.
#define PAT_TRY(var, expr)                                 \
    auto var##_res_ = (expr);                              \
    if (!var##_res_) return Err(var##_res_.error());       \
    std::string var = std::move(*var##_res_)

// `shape.<op>(n, span)?` on the Result<Shape, ExceedsMaxWidthError> forms.
#define PAT_TRY_SHAPE(var, base, call, sp)                              \
    std::optional<Shape> var##_opt_ = (base).call;                      \
    if (!var##_opt_) return Err(RewriteError::exceeds((base).width, sp)); \
    Shape var = *var##_opt_

// rustc's Span::new (mk_sp): swaps lo and hi when lo > hi.
Span sp_new(uint32_t lo, uint32_t hi) { return lo > hi ? Span(hi, lo) : Span(lo, hi); }

bool is_short_pattern_inner(const RewriteContext& context, const rust::Pat* pat) {
    switch (pat->kind) {
    case rust::PatKind::Missing: panic("is_short_pattern: PatKind::Missing", pat->span.lo);
    case rust::PatKind::Rest:
    case rust::PatKind::Never:
    case rust::PatKind::Wild:
    case rust::PatKind::Err: return true;
    case rust::PatKind::Expr: {
        const rust::Expr* expr = pat->expr;
        switch (expr->kind) {
        case rust::ExprKind::Lit: return true;
        case rust::ExprKind::Unary:
            if (expr->unop == rust::UnOp::Neg) {
                if (expr->lhs->kind == rust::ExprKind::Lit) return true;
                panic("is_short_pattern: unexpected negated pattern expression", expr->span.lo);
            }
            panic("is_short_pattern: unexpected pattern expression", expr->span.lo);
        case rust::ExprKind::ConstBlock:
        case rust::ExprKind::Path: return context.config->style_edition() <= StyleEdition::E2024;
        default: panic("is_short_pattern: unexpected pattern expression", expr->span.lo);
        }
    }
    case rust::PatKind::Ident: return pat->sub == nullptr;
    case rust::PatKind::Struct:
    case rust::PatKind::MacCall:
    case rust::PatKind::Slice:
    case rust::PatKind::Path:
    case rust::PatKind::Range:
    case rust::PatKind::Guard: return false;
    case rust::PatKind::Tuple: return pat->elems.size() <= 1;
    case rust::PatKind::TupleStruct: return pat->path->segments.size() <= 1 && pat->elems.size() <= 1;
    case rust::PatKind::Box:
    case rust::PatKind::Deref:
    case rust::PatKind::Ref:
    case rust::PatKind::Paren: return is_short_pattern_inner(context, pat->sub);
    case rust::PatKind::Or:
        for (const rust::Pat* p : pat->elems) {
            if (!is_short_pattern_inner(context, p)) return false;
        }
        return true;
    }
    return false;
}

bool tuple_pat_field_is_dotdot(const TuplePatField& f) {
    if (!f.dotdot) return f.pat->kind == rust::PatKind::Rest;
    return true;
}

RewriteResult rewrite_struct_pat(const rust::QSelf* qself, const rust::Path* path,
                                 const std::vector<rust::PatField>& fields, bool ellipsis, Span span,
                                 const RewriteContext& context, Shape shape) {
    // 2 =  ` {`
    PAT_TRY_SHAPE(path_shape, shape, sub_width(2), span);
    PAT_TRY(path_str, rewrite_path(context, PathContext::Expr, qself, path, path_shape));

    if (fields.empty() && !ellipsis) {
        return path_str + " {}";
    }

    std::string_view ellipsis_str = ellipsis ? ", .." : "";
    std::string_view terminator = ellipsis ? ".." : "}";

    // 3 = ` { `, 2 = ` }`.
    auto shapes = struct_lit_shape(shape, context, path_str.size() + 3, ellipsis_str.size() + 2);
    if (!shapes) return Err(RewriteError::exceeds(shape.width, span));
    std::optional<Shape> h_shape = shapes->first;
    Shape v_shape = shapes->second;

    std::vector<ListItem> item_vec = itemize_list(
        *context.snippet_provider, fields.size(), terminator, ",",
        [&](size_t i) {
            const rust::PatField& f = fields[i];
            return f.attrs.empty() ? f.span.lo : f.attrs.front().span.lo;
        },
        [&](size_t i) { return fields[i].span.hi; },
        [&](size_t i) { return rewrite_result(context, &fields[i], v_shape); },
        context.snippet_provider->span_after(span, "{"), span.hi, false);

    DefinitiveListTactic tactic = struct_lit_tactic(h_shape, context, item_vec);
    Shape nested_shape = shape_for_tactic(tactic, h_shape, v_shape);
    ListFormatting fmt = struct_lit_formatting(nested_shape, tactic, context, false);

    PAT_TRY(fields_str, write_list(item_vec, fmt));
    size_t one_line_width = h_shape ? h_shape->width : 0;

    bool has_trailing_comma = fmt.needs_trailing_separator();

    if (ellipsis) {
        if (contains(fields_str, '\n') || fields_str.size() > one_line_width) {
            // Add a missing trailing comma.
            if (!has_trailing_comma) fields_str += ',';
            fields_str += '\n';
            fields_str += nested_shape.indent.to_string(*context.config);
        } else {
            if (!fields_str.empty()) {
                // there are preceding struct fields being matched on
                if (has_trailing_comma) {
                    fields_str += ' ';
                } else {
                    fields_str += ", ";
                }
            }
        }
        fields_str += "..";
    }

    // ast::Pat doesn't have attrs so use &[]
    static const rust::AttrVec kNoAttrs;
    PAT_TRY(wrapped, wrap_struct_field(context, kNoAttrs, fields_str, shape, v_shape, one_line_width));
    return path_str + " {" + wrapped + "}";
}

size_t count_wildcard_suffix_len(const RewriteContext& context, const std::vector<TuplePatField>& patterns,
                                 Span span, Shape shape) {
    size_t suffix_len = 0;

    std::vector<ListItem> items = itemize_list(
        *context.snippet_provider, patterns.size(), ")", ",", [&](size_t i) { return spanned(&patterns[i]).lo; },
        [&](size_t i) { return spanned(&patterns[i]).hi; },
        [&](size_t i) { return rewrite_result(context, &patterns[i], shape); },
        context.snippet_provider->span_after(span, "("), span.hi - 1, false);

    for (size_t k = items.size(); k-- > 0;) {
        const ListItem& item = items[k];
        if (!(item.item.ok() && *item.item == "_")) break;
        suffix_len += 1;

        if (item.has_comment()) break;
    }

    return suffix_len;
}

RewriteResult rewrite_tuple_pat(const std::vector<rust::Pat*>& pats, Opt path_str, Span span,
                                const RewriteContext& context, Shape shape) {
    if (pats.empty()) {
        return path_str.value_or(std::string()) + "()";
    }
    std::vector<TuplePatField> pat_vec;
    pat_vec.reserve(pats.size());
    for (const rust::Pat* p : pats) pat_vec.push_back(TuplePatField{p, false, Span()});

    size_t wildcard_suffix_len = count_wildcard_suffix_len(context, pat_vec, span, shape);
    // non-default config: condense_wildcard_suffixes (only taken when it is true and
    // wildcard_suffix_len >= 2).
    (void)wildcard_suffix_len;

    bool is_last_pat_dotdot = !pat_vec.empty() && tuple_pat_field_is_dotdot(pat_vec.back());
    bool add_comma = !path_str && pat_vec.size() == 1 && !is_last_pat_dotdot;
    std::string path = path_str.value_or(std::string());

    std::vector<OverflowableItem> items;
    items.reserve(pat_vec.size());
    for (const TuplePatField& f : pat_vec) items.push_back(RwNode::of(&f));
    return rewrite_with_parens(context, path, items, shape, span, context.config->max_width(),
                               add_comma ? std::optional<SeparatorTactic>(SeparatorTactic::Always) : std::nullopt);
}

RewriteResult rewrite_ident_pat(const RewriteContext& context, const rust::Pat* self, Shape shape) {
    std::string_view mut_prefix = trim(format_mutability(self->mutbl));

    std::string_view ref_kw, pin_infix, mut_infix;
    if (self->by_ref) {
        auto pm = format_pinnedness_and_mutability(self->pinned, self->by_ref_mutbl);
        ref_kw = "ref";
        pin_infix = trim(pm.first);
        mut_infix = trim(pm.second);
    }
    std::string_view id_str = rewrite_ident(context, self->ident);
    const SnippetProvider& sp = *context.snippet_provider;
    std::string sub_pat;
    if (const rust::Pat* p = self->sub) {
        // 2 - `@ `.
        std::optional<size_t> width = checked_sub(
            shape.width, mut_prefix.size() + ref_kw.size() + pin_infix.size() + mut_infix.size() + id_str.size() + 2);
        if (!width) return Err(RewriteError::exceeds(shape.width, spanned(p)));
        uint32_t lo = sp.span_after(self->span, "@");
        PAT_TRY(p_str, rewrite_result(context, p, Shape::legacy(*width, shape.indent)));
        PAT_TRY(r, combine_strs_with_missing_comments(context, "@", p_str, sp_new(lo, p->span.lo), shape, true));
        sub_pat = std::move(r);
    }

    // combine prefix and ref
    uint32_t first_lo;
    std::string first;
    if (!mut_prefix.empty() && !ref_kw.empty()) {
        uint32_t lo = sp.span_after(self->span, "mut");
        uint32_t hi = sp.span_before(self->span, "ref");
        first_lo = sp.span_after(self->span, "ref");
        PAT_TRY(r, combine_strs_with_missing_comments(context, mut_prefix, ref_kw, sp_new(lo, hi), shape, true));
        first = std::move(r);
    } else if (!mut_prefix.empty()) {
        first_lo = sp.span_after(self->span, "mut");
        first = std::string(mut_prefix);
    } else if (!ref_kw.empty()) {
        first_lo = sp.span_after(self->span, "ref");
        first = std::string(ref_kw);
    } else {
        first_lo = self->span.lo;
        first = "";
    }

    // combine result of above and pin
    uint32_t second_lo;
    std::string second;
    if (!first.empty() && !pin_infix.empty()) {
        uint32_t lo = sp.span_after(self->span, "ref");
        uint32_t hi = sp.span_before(self->span, "pin");
        second_lo = sp.span_after(self->span, "pin");
        PAT_TRY(r, combine_strs_with_missing_comments(context, first, pin_infix, sp_new(lo, hi), shape, true));
        second = std::move(r);
    } else if (!first.empty()) {
        second_lo = first_lo;
        second = std::move(first);
    } else if (!pin_infix.empty()) {
        panic("pin_infix necessarily follows a ref", self->span.lo);
    } else {
        second_lo = self->span.lo;
        second = "";
    }

    // combine result of above and const|mut
    uint32_t third_lo;
    std::string third;
    if (!second.empty() && !mut_infix.empty()) {
        uint32_t lo = sp.span_after(self->span, pin_infix.empty() ? "ref" : "pin");
        Span end_span = sp_new(second_lo, self->span.hi);
        uint32_t hi = sp.span_before(end_span, mut_infix);
        third_lo = sp.span_after(end_span, mut_infix);
        PAT_TRY(r, combine_strs_with_missing_comments(context, second, mut_infix, sp_new(lo, hi), shape, true));
        third = std::move(r);
    } else if (!second.empty()) {
        third_lo = second_lo;
        third = std::move(second);
    } else if (!mut_infix.empty()) {
        panic("mut_infix necessarily follows a pin or ref", self->span.lo);
    } else {
        third_lo = self->span.lo;
        third = "";
    }

    std::string next;
    if (!sub_pat.empty()) {
        uint32_t hi = sp.span_before(self->span, "@");
        PAT_TRY(r, combine_strs_with_missing_comments(context, id_str, sub_pat, sp_new(self->ident.span.hi, hi), shape,
                                                      true));
        next = std::move(r);
    } else {
        next = std::string(id_str);
    }

    return combine_strs_with_missing_comments(context, third, next, sp_new(third_lo, self->ident.span.lo), shape, true);
}

}  // namespace

/// Returns `true` if the given pattern is "short".
/// A short pattern is defined by the following grammar:
///
/// `[small, ntp]`:
///     - single token
///     - `&[single-line, ntp]`
///
/// `[small]`:
///     - `[small, ntp]`
///     - unary tuple constructor `([small, ntp])`
///     - `&[small]`
bool is_short_pattern(const RewriteContext& context, const rust::Pat* pat, std::string_view pat_str) {
    // We also require that the pattern is reasonably 'small' with its literal width.
    return pat_str.size() <= 20 && !contains(pat_str, '\n') && is_short_pattern_inner(context, pat);
}

Span spanned(const RangeOperand* x) { return x->span; }

RewriteResult rewrite_result(const RewriteContext& ctx, const RangeOperand* x, Shape shape) {
    if (!x->operand) return std::string();
    return rewrite_result(ctx, x->operand, shape);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::Pat* x, Shape shape) {
    DepthGuard g(*context.state);
    switch (x->kind) {
    case rust::PatKind::Missing: panic("rewrite Pat: PatKind::Missing", x->span.lo);
    case rust::PatKind::Or: {
        std::vector<std::string> pat_strs;
        pat_strs.reserve(x->elems.size());
        for (const rust::Pat* p : x->elems) {
            PAT_TRY(s, rewrite_result(context, p, shape));
            pat_strs.push_back(std::move(s));
        }

        bool use_mixed_layout = true;
        for (size_t i = 0; i < x->elems.size(); i++) {
            if (!is_short_pattern(context, x->elems[i], pat_strs[i])) {
                use_mixed_layout = false;
                break;
            }
        }
        std::vector<ListItem> items;
        items.reserve(pat_strs.size());
        for (std::string& s : pat_strs) items.push_back(ListItem::from_str(std::move(s)));
        DefinitiveListTactic tactic =
            use_mixed_layout ? DefinitiveListTactic::Mixed()
                             : definitive_tactic(items, ListTactic::HorizontalVertical(), Separator::VerticalBar,
                                                 shape.width);
        ListFormatting fmt(shape, *context.config);
        // binop_separator() = Front.
        fmt.tactic(tactic).separator(" |").separator_place(SeparatorPlace::Front).ends_with_newline(false);
        return write_list(items, fmt);
    }
    case rust::PatKind::Box: return rewrite_unary_prefix(context, "box ", RwNode::of(x->sub), shape);
    case rust::PatKind::Ident: return rewrite_ident_pat(context, x, shape);
    case rust::PatKind::Wild:
        if (1 <= shape.width) return std::string("_");
        return Err(RewriteError::exceeds(1, x->span));
    case rust::PatKind::Rest:
        if (1 <= shape.width) return std::string("..");
        return Err(RewriteError::exceeds(1, x->span));
    case rust::PatKind::Never: return Err(RewriteError::unknown());
    case rust::PatKind::Range:
        return rewrite_range_pat(context, shape, x->range_lo, x->range_hi, x->range_end, x->range_end_span, x->span);
    case rust::PatKind::Ref: {
        auto pm = format_pinnedness_and_mutability(x->pinned, x->mutbl);
        std::string prefix = std::string("&") + pm.first + pm.second;
        return rewrite_unary_prefix(context, prefix, RwNode::of(x->sub), shape);
    }
    case rust::PatKind::Tuple: return rewrite_tuple_pat(x->elems, std::nullopt, x->span, context, shape);
    case rust::PatKind::Path: return rewrite_path(context, PathContext::Expr, x->qself, x->path, shape);
    case rust::PatKind::TupleStruct: {
        PAT_TRY(path_str, rewrite_path(context, PathContext::Expr, x->qself, x->path, shape));
        return rewrite_tuple_pat(x->elems, std::move(path_str), x->span, context, shape);
    }
    case rust::PatKind::Expr: return rewrite_result(context, x->expr, shape);
    case rust::PatKind::Slice: {
        if (context.config->style_edition() <= StyleEdition::E2021) {
            std::vector<std::string> rw;
            rw.reserve(x->elems.size());
            for (const rust::Pat* p : x->elems) {
                RewriteResult r = rewrite_result(context, p, shape);
                rw.push_back(r ? *r : std::string(context.snippet(p->span)));
            }
            return "[" + join(rw, ", ") + "]";
        }
        std::vector<OverflowableItem> items;
        items.reserve(x->elems.size());
        for (const rust::Pat* p : x->elems) items.push_back(RwNode::of(p));
        return rewrite_with_square_brackets(context, "", items, shape, x->span, std::nullopt, std::nullopt);
    }
    case rust::PatKind::Struct:
        return rewrite_struct_pat(x->qself, x->path, x->fields, x->rest == rust::Pat::Rest::Rest, x->span, context,
                                  shape);
    case rust::PatKind::MacCall: return rewrite_macro(x->mac, context, shape, MacroPosition::Pat, x->span);
    case rust::PatKind::Paren: {
        PAT_TRY_SHAPE(s1, shape, offset_left(1), x->span);
        PAT_TRY_SHAPE(s2, s1, sub_width(1), x->span);
        PAT_TRY(inner_pat, rewrite_result(context, x->sub, s2));
        return "(" + inner_pat + ")";
    }
    case rust::PatKind::Guard: return std::string(context.snippet(x->span));
    case rust::PatKind::Deref: return Err(RewriteError::unknown());
    case rust::PatKind::Err: return Err(RewriteError::unknown());
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_range_pat(const RewriteContext& context, Shape shape, const rust::Expr* lhs, const rust::Expr* rhs,
                                rust::RangeEndKind end_kind, Span end_span, Span span) {
    std::string infix;
    switch (end_kind) {
    case rust::RangeEndKind::IncludedDotDotDot: infix = "..."; break;
    case rust::RangeEndKind::IncludedDotDotEq: infix = "..="; break;
    case rust::RangeEndKind::Excluded: infix = ".."; break;
    }
    // non-default config: spaces_around_ranges
    Span lspan = sp_new(span.lo, end_span.lo);  // span.with_hi(end_kind.span.lo())
    Span rspan = sp_new(end_span.hi, span.hi);  // span.with_lo(end_kind.span.hi())
    RangeOperand l{lhs, lspan};
    RangeOperand r{rhs, rspan};
    return rewrite_pair(RwNode::of(&l), RwNode::of(&r), PairParts::infix_(infix), context, shape,
                        SeparatorPlace::Front);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::PatField* x, Shape shape) {
    uint32_t hi_pos = x->attrs.empty() ? x->pat->span.lo : x->attrs.back().span.hi;

    std::string attrs_str;
    if (!x->attrs.empty()) {
        PAT_TRY(a, rewrite_attrs(context, x->attrs, shape));
        attrs_str = std::move(a);
    }

    PAT_TRY(pat_str, rewrite_result(context, x->pat, shape));
    if (x->is_shorthand) {
        return combine_strs_with_missing_comments(context, attrs_str, pat_str, sp_new(hi_pos, x->pat->span.lo), shape,
                                                  false);
    }
    Shape nested_shape = shape.block_indent(context.config->tab_spaces());
    std::string_view id_str = rewrite_ident(context, x->ident);
    size_t one_line_width = id_str.size() + 2 + pat_str.size();
    std::string pat_and_id_str;
    if (one_line_width <= shape.width) {
        pat_and_id_str = std::string(id_str) + ": " + pat_str;
    } else {
        PAT_TRY(p2, rewrite_result(context, x->pat, nested_shape));
        pat_and_id_str = std::string(id_str) + ":\n" + nested_shape.indent.to_string(*context.config) + p2;
    }
    return combine_strs_with_missing_comments(context, attrs_str, pat_and_id_str, sp_new(hi_pos, x->pat->span.lo),
                                              nested_shape, false);
}

RewriteResult rewrite_result(const RewriteContext& ctx, const TuplePatField* x, Shape shape) {
    if (x->dotdot) return std::string("..");
    return rewrite_result(ctx, x->pat, shape);
}

Span spanned(const TuplePatField* x) {
    if (x->dotdot) return x->span;
    return spanned(x->pat);
}

bool can_be_overflowed_pat(const RewriteContext& context, const TuplePatField* pat, size_t len) {
    if (pat->dotdot) return false;
    const rust::Pat* p = pat->pat;
    switch (p->kind) {
    case rust::PatKind::Path:
    case rust::PatKind::Tuple:
    case rust::PatKind::Struct:
    case rust::PatKind::TupleStruct: return context.use_block_indent() && len == 1;
    case rust::PatKind::Ref:
    case rust::PatKind::Box: {
        TuplePatField inner{p->sub, false, Span()};
        return can_be_overflowed_pat(context, &inner, len);
    }
    case rust::PatKind::Expr: return can_be_overflowed_expr(context, p->expr, len);
    default: return false;
    }
}

}  // namespace rustfmt
