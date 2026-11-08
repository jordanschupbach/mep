// rust_format_overflow.cpp -- rustfmt's overflow.rs (and the RwNode dispatch)
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

#include <algorithm>

namespace rustfmt {

// ===========================================================================
// RwNode: the `R: Rewrite + Spanned` dispatch.

RewriteResult rewrite_result(const RewriteContext& ctx, const RwNode& n, Shape shape) {
    switch (n.kind) {
    case RwNode::None: panic("rewrite_result: empty RwNode");
    case RwNode::Expr: return rewrite_result(ctx, n.as<rust::Expr>(), shape);
    case RwNode::GenericParam: return rewrite_result(ctx, n.as<rust::GenericParam>(), shape);
    case RwNode::MacroArg: return rewrite_result(ctx, n.as<MacroArg>(), shape);
    case RwNode::MetaItemInner: return rewrite_result(ctx, n.as<MetaItemInner>(), shape);
    case RwNode::SegmentParam: return rewrite_result(ctx, n.as<SegmentParam>(), shape);
    case RwNode::FieldDef: return rewrite_result(ctx, n.as<rust::FieldDef>(), shape);
    case RwNode::TuplePatField: return rewrite_result(ctx, n.as<TuplePatField>(), shape);
    case RwNode::Ty: return rewrite_result(ctx, n.as<rust::Ty>(), shape);
    case RwNode::Pat: return rewrite_result(ctx, n.as<rust::Pat>(), shape);
    case RwNode::PreciseCapturingArg: return rewrite_result(ctx, n.as<rust::PreciseCapturingArg>(), shape);
    case RwNode::GenericBounds: return rewrite_result(ctx, n.as<rust::GenericBounds>(), shape);
    case RwNode::GenericBound: return rewrite_result(ctx, n.as<rust::GenericBound>(), shape);
    case RwNode::Lifetime: return rewrite_result(ctx, n.as<rust::Lifetime>(), shape);
    case RwNode::AnonConst: return rewrite_result(ctx, n.as<rust::AnonConst>(), shape);
    case RwNode::Path: panic("rewrite_result: rustfmt has no Rewrite for ast::Path (use TraitRef)");
    case RwNode::UseTree: panic("rewrite_result: UseTree is rewritten by imports.rs, not through RwNode");
    case RwNode::RangeOperand: return rewrite_result(ctx, n.as<RangeOperand>(), shape);
    case RwNode::FnRetTy: return rewrite_result(ctx, n.as<rust::FnRetTy>(), shape);
    case RwNode::TyPat: return rewrite_result(ctx, n.as<rust::TyPat>(), shape);
    case RwNode::TraitRef: return rewrite_trait_ref(ctx, n.as<rust::Path>(), shape);
    case RwNode::PolyTraitRef: return rewrite_result(ctx, n.as<rust::PolyTraitRef>(), shape);
    case RwNode::Opaque:
        if (!n.opaque_rewrite) panic("rewrite_result: Opaque RwNode without a rewrite");
        return n.opaque_rewrite(ctx, shape);
    }
    panic("rewrite_result: bad RwNode kind");
}

Span spanned(const RwNode& n) {
    switch (n.kind) {
    case RwNode::None: panic("spanned: empty RwNode");
    case RwNode::Expr: return spanned(n.as<rust::Expr>());
    case RwNode::GenericParam: return spanned(n.as<rust::GenericParam>());
    case RwNode::MacroArg: return spanned(n.as<MacroArg>());
    case RwNode::MetaItemInner: return spanned(n.as<MetaItemInner>());
    case RwNode::SegmentParam: return spanned(n.as<SegmentParam>());
    case RwNode::FieldDef: return spanned(n.as<rust::FieldDef>());
    case RwNode::TuplePatField: return spanned(n.as<TuplePatField>());
    case RwNode::Ty: return spanned(n.as<rust::Ty>());
    case RwNode::Pat: return spanned(n.as<rust::Pat>());
    case RwNode::PreciseCapturingArg: return spanned(n.as<rust::PreciseCapturingArg>());
    case RwNode::GenericBound: return spanned(n.as<rust::GenericBound>());
    case RwNode::Lifetime: return n.as<rust::Lifetime>()->ident.span;
    case RwNode::AnonConst: return spanned(n.as<rust::AnonConst>()->value);
    case RwNode::Path: return n.as<rust::Path>()->span;
    case RwNode::UseTree: return n.as<rust::UseTree>()->span;
    case RwNode::RangeOperand: return spanned(n.as<RangeOperand>());
    case RwNode::FnRetTy: return spanned(n.as<rust::FnRetTy>());
    case RwNode::TyPat: return n.as<rust::TyPat>()->span;
    case RwNode::TraitRef: return n.as<rust::Path>()->span;
    case RwNode::PolyTraitRef: return n.as<rust::PolyTraitRef>()->span;
    case RwNode::GenericBounds:
    case RwNode::Opaque: return n.span_override;
    }
    panic("spanned: bad RwNode kind");
}

// ===========================================================================
// overflow.rs

namespace {

struct SpecialCase {
    std::string_view name;
    size_t num_args_before;
};

/// A list of `format!`-like macros, that take a long format string and a list of arguments to
/// format.
///
/// Organized as a list of `(&str, usize)` tuples, giving the name of the macro and the number of
/// arguments before the format string (none for `format!("format", ...)`, one for `assert!(result,
/// "format", ...)`, two for `assert_eq!(left, right, "format", ...)`).
constexpr SpecialCase SPECIAL_CASE_MACROS[] = {
    // format! like macros
    // From the Rust Standard Library.
    {"eprint!", 0},
    {"eprintln!", 0},
    {"format!", 0},
    {"format_args!", 0},
    {"print!", 0},
    {"println!", 0},
    {"panic!", 0},
    {"unreachable!", 0},
    // From the `log` crate.
    {"debug!", 0},
    {"error!", 0},
    {"info!", 0},
    {"warn!", 0},
    // write! like macros
    {"assert!", 1},
    {"debug_assert!", 1},
    {"write!", 1},
    {"writeln!", 1},
    // assert_eq! like macros
    {"assert_eq!", 2},
    {"assert_ne!", 2},
    {"debug_assert_eq!", 2},
    {"debug_assert_ne!", 2},
};

/// Additional special case macros for version 2; these are separated to avoid breaking changes in
/// version 1.
constexpr SpecialCase SPECIAL_CASE_MACROS_V2[] = {
    // From the `log` crate.
    {"trace!", 0},
};

constexpr SpecialCase SPECIAL_CASE_ATTR[] = {
    // From the `failure` crate.
    {"fail", 0},
};

// OverflowableItem::has_attrs
bool overflowable_has_attrs(const OverflowableItem& it) {
    switch (it.kind) {
    case RwNode::Expr: return !it.as<rust::Expr>()->attrs.empty();
    case RwNode::GenericParam: return !it.as<rust::GenericParam>()->attrs.empty();
    case RwNode::FieldDef: return !it.as<rust::FieldDef>()->attrs.empty();
    case RwNode::MacroArg: {
        const MacroArg* m = it.as<MacroArg>();
        if (m->kind == MacroArg::Expr) return !m->expr->attrs.empty();
        if (m->kind == MacroArg::Item) return !m->item->attrs.empty();
        return false;
    }
    default: return false;
    }
}

// OverflowableItem::special_cases: base cases chained with the additional ones.
std::optional<size_t> find_special_case(const OverflowableItem& it, const Config& config, std::string_view callee) {
    if (it.kind == RwNode::MacroArg) {
        for (const SpecialCase& c : SPECIAL_CASE_MACROS) {
            if (c.name == callee) return c.num_args_before;
        }
        if (config.style_edition() >= StyleEdition::E2024) {
            for (const SpecialCase& c : SPECIAL_CASE_MACROS_V2) {
                if (c.name == callee) return c.num_args_before;
            }
        }
    } else if (it.kind == RwNode::MetaItemInner) {
        for (const SpecialCase& c : SPECIAL_CASE_ATTR) {
            if (c.name == callee) return c.num_args_before;
        }
    }
    return std::nullopt;
}

std::vector<OverflowableItem> sub_items(const std::vector<OverflowableItem>& v, size_t b, size_t e) {
    return std::vector<OverflowableItem>(v.begin() + static_cast<std::ptrdiff_t>(b),
                                         v.begin() + static_cast<std::ptrdiff_t>(e));
}

}  // namespace

bool overflowable_is_simple(const OverflowableItem& it) {
    switch (it.kind) {
    case RwNode::Expr: return is_simple_expr(it.as<rust::Expr>());
    case RwNode::MacroArg: {
        const MacroArg* m = it.as<MacroArg>();
        if (m->kind == MacroArg::Keyword) return true;
        if (m->kind == MacroArg::Expr) return is_simple_expr(m->expr);
        return false;
    }
    case RwNode::MetaItemInner: {
        const MetaItemInner* mi = it.as<MetaItemInner>();
        if (mi->is_lit) return true;
        return mi->mi->kind == MetaItem::Word;
    }
    // FIXME: Why don't we consider `SegmentParam` to be simple?
    // FIXME: If we also fix `SegmentParam`, then we should apply the same
    // heuristic to `PreciseCapturingArg`.
    default: return false;
    }
}

bool overflowable_is_expr(const OverflowableItem& it) {
    return it.kind == RwNode::Expr || (it.kind == RwNode::MacroArg && it.as<MacroArg>()->kind == MacroArg::Expr);
}

bool overflowable_is_nested_call(const OverflowableItem& it) {
    if (it.kind == RwNode::Expr) return is_nested_call(it.as<rust::Expr>());
    if (it.kind == RwNode::MacroArg && it.as<MacroArg>()->kind == MacroArg::Expr) {
        return is_nested_call(it.as<MacroArg>()->expr);
    }
    return false;
}

const rust::Expr* overflowable_to_expr(const OverflowableItem& it) {
    if (it.kind == RwNode::Expr) return it.as<rust::Expr>();
    if (it.kind == RwNode::MacroArg && it.as<MacroArg>()->kind == MacroArg::Expr) return it.as<MacroArg>()->expr;
    return nullptr;
}

bool overflowable_can_be_overflowed(const OverflowableItem& it, const RewriteContext& ctx, size_t len) {
    switch (it.kind) {
    case RwNode::Expr: return can_be_overflowed_expr(ctx, it.as<rust::Expr>(), len);
    case RwNode::MacroArg: {
        const MacroArg* m = it.as<MacroArg>();
        switch (m->kind) {
        case MacroArg::Expr: return can_be_overflowed_expr(ctx, m->expr, len);
        case MacroArg::Ty: return can_be_overflowed_type(ctx, m->ty, len);
        case MacroArg::Pat: return false;
        case MacroArg::Item: return len == 1;
        case MacroArg::Keyword: return false;
        }
        return false;
    }
    case RwNode::MetaItemInner:
        if (len == 1) return !it.as<MetaItemInner>()->is_lit;
        return false;
    case RwNode::SegmentParam: {
        const SegmentParam* sp = it.as<SegmentParam>();
        if (sp->kind == SegmentParam::Type) return can_be_overflowed_type(ctx, sp->ty, len);
        return false;
    }
    case RwNode::TuplePatField: return can_be_overflowed_pat(ctx, it.as<TuplePatField>(), len);
    case RwNode::Ty: return can_be_overflowed_type(ctx, it.as<rust::Ty>(), len);
    default: return false;
    }
}

namespace {

bool can_be_overflowed(const RewriteContext& ctx, const std::vector<OverflowableItem>& items) {
    if (items.empty()) return false;
    return overflowable_can_be_overflowed(items.back(), ctx, items.size());
}

/// Returns a shape for the last argument which is going to be overflowed.
std::optional<Shape> last_item_shape(const std::vector<OverflowableItem>& lists, const std::vector<ListItem>& items,
                                     Shape shape, size_t args_max_width) {
    if (items.size() == 1) {
        if (lists.empty()) return std::nullopt;
        if (!overflowable_is_nested_call(lists[0])) return shape;
    }
    size_t offset = 0;
    for (size_t k = 0; k + 1 < items.size(); ++k) {
        // 2 = ", "
        offset += 2 + items[k].inner_as_ref().size();
    }
    Shape s = shape;
    s.width = std::min(args_max_width, shape.width);
    return s.offset_left_opt(offset);
}

Shape shape_from_indent_style(const RewriteContext& ctx, Shape shape, size_t overhead_visual, size_t offset) {
    // non-default config: indent_style = Visual (shape.visual_indent(offset), overhead_visual)
    (void)overhead_visual;
    (void)offset;
    Shape s = shape.block().block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);
    size_t overhead = 1;  // 1 = ","
    s.width = sat_sub(s.width, overhead);
    return s;
}

bool no_long_items(const std::vector<ListItem>& list, size_t short_array_element_width_threshold) {
    for (const ListItem& item : list) {
        if (item.inner_as_ref().size() > short_array_element_width_threshold) return false;
    }
    return true;
}

struct Context {
    const RewriteContext& context;
    const std::vector<OverflowableItem>& items;
    std::string_view ident;
    std::string_view prefix;
    std::string_view suffix;
    Shape one_line_shape;
    Shape nested_shape;
    Span span;
    size_t item_max_width = 0;
    size_t one_line_width = 0;
    std::optional<SeparatorTactic> force_separator_tactic;
    std::optional<std::pair<std::string_view, std::string_view>> custom_delims;

    Context(const RewriteContext& ctx, const std::vector<OverflowableItem>& items_, std::string_view ident_,
            Shape shape, Span span_, std::string_view prefix_, std::string_view suffix_, size_t item_max_width_,
            std::optional<SeparatorTactic> force_separator_tactic_,
            std::optional<std::pair<std::string_view, std::string_view>> custom_delims_)
        : context(ctx), items(items_), ident(ident_), prefix(prefix_), suffix(suffix_), span(span_),
          item_max_width(item_max_width_), force_separator_tactic(force_separator_tactic_),
          custom_delims(custom_delims_) {
        size_t used_width = extra_offset(ident, shape);
        // 1 = `()`
        one_line_width = sat_sub(shape.width, used_width + 2);

        // 1 = "(" or ")"
        std::optional<Shape> ols = shape.offset_left_opt(last_line_width(ident) + 1);
        if (ols) ols = ols->sub_width_opt(1);
        if (ols) {
            one_line_shape = *ols;
        } else {
            one_line_shape = shape;
            one_line_shape.width = 0;
        }
        nested_shape = shape_from_indent_style(context, shape, used_width + 2, used_width + 1);
    }

    const OverflowableItem* last_item() const { return items.empty() ? nullptr : &items.back(); }

    Span items_span() const {
        uint32_t span_lo = context.snippet_provider->span_after(span, prefix);
        return mk_sp(span_lo, span.hi);
    }

    Opt rewrite_last_item_with_overflow(ListItem& last_list_item, Shape shape) const {
        const OverflowableItem* last = last_item();
        if (!last) return std::nullopt;
        Opt rewrite;
        if (last->kind == RwNode::Expr) {
            const rust::Expr* expr = last->as<rust::Expr>();
            switch (expr->kind) {
            // When overflowing the closure which consists of a single control flow
            // expression, force to use block if its condition uses multi line.
            case rust::ExprKind::Closure:
                // If the argument consists of multiple closures, we do not overflow
                // the last closure.
                if (args_have_many_closure(items)) {
                    rewrite = std::nullopt;
                } else {
                    rewrite = rewrite_last_closure(context, expr, shape).opt();
                }
                break;

            // When overflowing the expressions which consists of a control flow
            // expression, avoid condition to use multi line.
            case rust::ExprKind::If:
            case rust::ExprKind::ForLoop:
            case rust::ExprKind::Loop:
            case rust::ExprKind::While:
            case rust::ExprKind::Match: {
                Opt cond = rewrite_cond(context, expr, shape);
                bool multi_line = cond.has_value() && contains(*cond, '\n');
                if (multi_line) {
                    rewrite = std::nullopt;
                } else {
                    rewrite = rustfmt::rewrite(context, *last, shape);
                }
                break;
            }

            default: rewrite = rustfmt::rewrite(context, *last, shape); break;
            }
        } else {
            rewrite = rustfmt::rewrite(context, *last, shape);
        }

        if (rewrite) {
            // splitn(2, *).next().unwrap() is always safe.
            size_t nl = rewrite->find('\n');
            last_list_item.item = RewriteResult(rewrite->substr(0, nl));
            return rewrite;
        }
        return std::nullopt;
    }

    DefinitiveListTactic default_tactic(const std::vector<ListItem>& list_items) const {
        return definitive_tactic(list_items, ListTactic::LimitedHorizontalVertical(item_max_width), Separator::Comma,
                                 one_line_width);
    }

    DefinitiveListTactic try_overflow_last_item(std::vector<ListItem>& list_items) const {
        // 1 = "("
        bool combine_arg_with_callee = items.size() == 1 && overflowable_is_expr(items[0]) &&
                                       !overflowable_has_attrs(items[0]) &&
                                       ident.size() < context.config->tab_spaces();
        bool overflow_last = combine_arg_with_callee || can_be_overflowed(context, items);

        // Replace the last item with its first line to see if it fits with
        // first arguments.
        Opt placeholder;
        if (overflow_last) {
            bool old_value = context.force_one_line_chain;
            const OverflowableItem* last = last_item();
            if (last && last->kind == RwNode::Expr && !combine_arg_with_callee &&
                is_method_call(last->as<rust::Expr>())) {
                context.force_one_line_chain = true;
            } else if (last && last->kind == RwNode::MacroArg && last->as<MacroArg>()->kind == MacroArg::Expr &&
                       !combine_arg_with_callee && is_method_call(last->as<MacroArg>()->expr) &&
                       context.config->style_edition() >= StyleEdition::E2024) {
                context.force_one_line_chain = true;
            }
            std::optional<Shape> arg_shape = last_item_shape(items, list_items, one_line_shape, item_max_width);
            Opt result;
            if (arg_shape) result = rewrite_last_item_with_overflow(list_items[items.size() - 1], *arg_shape);
            context.force_one_line_chain = old_value;
            placeholder = std::move(result);
        }

        DefinitiveListTactic tactic = definitive_tactic(
            list_items, ListTactic::LimitedHorizontalVertical(item_max_width), Separator::Comma, one_line_width);

        // Replace the stub with the full overflowing last argument if the rewrite
        // succeeded and its first line fits with the other arguments.
        if (overflow_last && tactic == DefinitiveListTactic::Horizontal() && placeholder.has_value() &&
            items.size() == 1) {
            const std::string& overflowed = *placeholder;
            // When we are rewriting a nested function call, we restrict the
            // budget for the inner function to avoid them being deeply nested.
            // However, when the inner function has a prefix or a suffix
            // (e.g., `foo() as u32`), this budget reduction may produce poorly
            // formatted code, where a prefix or a suffix being left on its own
            // line. Here we explicitly check those cases.
            if (count_newlines(overflowed) == 1) {
                Opt rw = rustfmt::rewrite(context, items.back(), nested_shape);
                bool no_newline = rw.has_value() && !contains(*rw, '\n');
                if (no_newline) {
                    list_items[items.size() - 1].item = unknown_error(std::move(rw));
                } else {
                    list_items[items.size() - 1].item = RewriteResult(overflowed);
                }
            } else {
                list_items[items.size() - 1].item = RewriteResult(overflowed);
            }
        } else if (overflow_last && tactic == DefinitiveListTactic::Horizontal() && placeholder.has_value()) {
            list_items[items.size() - 1].item = unknown_error(std::move(placeholder));
        } else if (!items.empty()) {
            list_items[items.size() - 1].item = unknown_error(rustfmt::rewrite(context, items.back(), nested_shape));

            // Use horizontal layout for a function with a single argument as long as
            // everything fits in a single line.
            // `self.one_line_width == 0` means vertical layout is forced.
            if (items.size() == 1 && one_line_width != 0 && !list_items[0].has_comment() &&
                !contains(list_items[0].inner_as_ref(), '\n') && total_item_width(list_items[0]) <= one_line_width) {
                tactic = DefinitiveListTactic::Horizontal();
            } else {
                tactic = default_tactic(list_items);

                if (tactic == DefinitiveListTactic::Vertical()) {
                    if (std::optional<std::pair<bool, size_t>> off =
                            maybe_get_args_offset(ident, items, *context.config)) {
                        auto [all_simple, num_args_before] = *off;
                        const ListItem* b = list_items.data();
                        bool one_line = all_simple &&
                                        definitive_tactic(b, b + num_args_before, ListTactic::HorizontalVertical(),
                                                          Separator::Comma, nested_shape.width) ==
                                            DefinitiveListTactic::Horizontal() &&
                                        definitive_tactic(b + num_args_before + 1, b + list_items.size(),
                                                          ListTactic::HorizontalVertical(), Separator::Comma,
                                                          nested_shape.width) == DefinitiveListTactic::Horizontal();

                        if (one_line) tactic = DefinitiveListTactic::SpecialMacro(num_args_before);
                    } else if (is_every_expr_simple(items) &&
                               no_long_items(list_items, context.config->short_array_element_width_threshold())) {
                        tactic = DefinitiveListTactic::Mixed();
                    }
                }
            }
        }

        return tactic;
    }

    Expected<std::pair<bool, std::string>> rewrite_items() const {
        Span items_sp = items_span();

        std::vector<ListItem> list_items = itemize_list(
            *context.snippet_provider, items.size(), suffix, ",",
            [&](size_t k) { return spanned(items[k]).lo; }, [&](size_t k) { return spanned(items[k]).hi; },
            [&](size_t k) { return rewrite_result(context, items[k], nested_shape); }, items_sp.lo, items_sp.hi,
            true);

        // Try letting the last argument overflow to the next line with block
        // indentation. If its first line fits on one line with the other arguments,
        // we format the function arguments horizontally.
        DefinitiveListTactic tactic = try_overflow_last_item(list_items);
        SeparatorTactic trailing_separator = SeparatorTactic::Vertical;  // config.trailing_comma()
        if (force_separator_tactic) trailing_separator = *force_separator_tactic;
        // (!use_block_indent() => Never: never with the default indent_style)
        bool ends_with_newline = false;
        switch (tactic.kind) {
        case DefinitiveListTacticKind::Vertical:
        case DefinitiveListTacticKind::Mixed: ends_with_newline = context.use_block_indent(); break;
        default: ends_with_newline = false; break;
        }

        ListFormatting fmt(nested_shape, *context.config);
        fmt.tactic(tactic).trailing_separator(trailing_separator).ends_with_newline(ends_with_newline);

        RewriteResult r = write_list(list_items, fmt);
        if (!r) return Expected<std::pair<bool, std::string>>(r.error());
        return std::make_pair(tactic == DefinitiveListTactic::Horizontal(), std::move(*r));
    }

    std::string wrap_items(std::string_view items_str, Shape shape_in, bool is_extendable) const {
        Shape shape = shape_in;
        shape.width = sat_sub(shape_in.width, last_line_width(ident));

        std::string_view pfx = prefix, sfx = suffix;
        if (custom_delims) {
            pfx = custom_delims->first;
            sfx = custom_delims->second;
        }

        size_t extend_width = items_str.empty() ? 2 : first_line_width(items_str) + 1;
        std::string nested_indent_str = nested_shape.indent.to_string_with_newline(*context.config);
        std::string indent_str = shape.block().indent.to_string_with_newline(*context.config);
        std::string result;
        result.reserve(ident.size() + items_str.size() + 2 + indent_str.size() + nested_indent_str.size());
        result += ident;
        result += pfx;
        bool force_single_line = false;
        if (context.config->style_edition() >= StyleEdition::E2024) {
            force_single_line = !context.use_block_indent() || (is_extendable && extend_width <= shape.width);
        } else {
            // 2 = `()`
            bool fits_one_line = items_str.size() + 2 <= shape.width;
            force_single_line = !context.use_block_indent() ||
                                (context.inside_macro() && !contains(items_str, '\n') && fits_one_line) ||
                                (is_extendable && extend_width <= shape.width);
        }
        if (force_single_line) {
            result += items_str;
        } else {
            if (!items_str.empty()) {
                result += nested_indent_str;
                result += items_str;
            }
            result += indent_str;
        }
        result += sfx;
        return result;
    }

    RewriteResult rewrite(Shape shape) const {
        Expected<std::pair<bool, std::string>> r = rewrite_items();
        if (!r) return Err(r.error());
        auto& [extendable, items_str] = *r;

        // If we are using visual indent style and failed to format, retry with block indent.
        // (non-default config: indent_style = Visual -- need_block_indent / use_block retry)

        return RewriteResult(wrap_items(items_str, shape, extendable));
    }
};

}  // namespace

RewriteResult rewrite_with_parens(const RewriteContext& ctx, std::string_view ident,
                                  const std::vector<OverflowableItem>& items, Shape shape, Span span,
                                  size_t item_max_width, std::optional<SeparatorTactic> force_separator_tactic) {
    return Context(ctx, items, ident, shape, span, "(", ")", item_max_width, force_separator_tactic, std::nullopt)
        .rewrite(shape);
}

RewriteResult rewrite_with_angle_brackets(const RewriteContext& ctx, std::string_view ident,
                                          const std::vector<OverflowableItem>& items, Shape shape, Span span) {
    return Context(ctx, items, ident, shape, span, "<", ">", ctx.config->max_width(), std::nullopt, std::nullopt)
        .rewrite(shape);
}

RewriteResult rewrite_with_square_brackets(const RewriteContext& ctx, std::string_view name,
                                           const std::vector<OverflowableItem>& items, Shape shape, Span span,
                                           std::optional<SeparatorTactic> force_separator_tactic,
                                           std::optional<Delimiter> delim_token) {
    std::string_view lhs = "[", rhs = "]";
    if (delim_token == Delimiter::Parenthesis) {
        lhs = "(";
        rhs = ")";
    } else if (delim_token == Delimiter::Brace) {
        lhs = "{";
        rhs = "}";
    }
    return Context(ctx, items, name, shape, span, lhs, rhs, ctx.config->array_width(), force_separator_tactic,
                   std::make_pair(std::string_view("["), std::string_view("]")))
        .rewrite(shape);
}

/// In case special-case style is required, returns an offset from which we start horizontal layout.
std::optional<std::pair<bool, size_t>> maybe_get_args_offset(std::string_view callee_str,
                                                             const std::vector<OverflowableItem>& args,
                                                             const Config& config) {
    if (args.empty()) return std::nullopt;
    if (std::optional<size_t> found = find_special_case(args[0], config, callee_str)) {
        size_t num_args_before = *found;
        bool all_simple = args.size() > num_args_before &&
                          is_every_expr_simple(sub_items(args, 0, num_args_before)) &&
                          is_every_expr_simple(sub_items(args, num_args_before + 1, args.size()));
        return std::make_pair(all_simple, num_args_before);
    }
    return std::nullopt;
}

}  // namespace rustfmt
