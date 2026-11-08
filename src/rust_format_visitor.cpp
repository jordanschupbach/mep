// rust_format_visitor.cpp -- rustfmt's visitor.rs: FmtVisitor, which walks
// the items and statements of a module / block and pushes their rewrites
// (and the missing spans between them) into its buffer.
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// rustfmt's `Module` is the crate here: out-of-line `mod x;` files are never
// followed. file_lines is "all", so out_of_file_lines_range! is always false
// and skip_out_of_file_lines_range_visitor! never fires.
#include "rust_format_internal.h"

namespace rustfmt {

namespace {

Span next_span(const FmtVisitor& v, uint32_t hi) { return mk_sp(v.last_pos, hi); }

// FmtVisitor::with_context: a fresh context whose macro_rewrite_failure is
// folded back into the visitor.
template <class F>
auto with_context(FmtVisitor& v, F f) {
    RewriteContext ctx = v.get_context();
    auto result = f(ctx);
    v.macro_rewrite_failure |= ctx.macro_rewrite_failure;
    return result;
}

// ParseSess::line_of_byte_pos (1-based).
size_t line_of_byte_pos(const FmtVisitor& v, uint32_t pos) {
    std::string_view text = v.snippet_provider->entire_snippet();
    if (pos > text.size()) pos = static_cast<uint32_t>(text.size());
    size_t line = 1;
    for (size_t i = 0; i < pos; i++)
        if (text[i] == '\n') line++;
    return line;
}

Span mac_span(const rust::MacCall* mac) { return mac->path->span.to(mac->args.span()); }

void push_rewrite_inner(FmtVisitor& v, Span span, Opt rewrite) {
    if (rewrite) {
        v.push_str(*rewrite);
    } else {
        std::string_view snippet = v.snippet(span);
        v.push_str(trim(snippet));
    }
    v.last_pos = span.hi;
}

void visit_mac(FmtVisitor& v, const rust::MacCall* mac, MacroPosition pos) {
    Span mspan = mac_span(mac);
    // 1 = ;
    Shape shape = v.shape().saturating_sub_width(1);
    Opt rewrite =
        with_context(v, [&](const RewriteContext& ctx) { return rewrite_macro(mac, ctx, shape, pos, mspan).opt(); });
    // As of v638 of the rustc-ap-* crates, the associated span no longer includes
    // the trailing semicolon. This determines the correct span to ensure scenarios
    // with whitespace between the delimiters and trailing semi (i.e. `foo!(abc)     ;`)
    // are formatted correctly.
    Span span = mspan;
    Delimiter style = macro_style(mac, v.get_context());
    if ((style == Delimiter::Bracket || style == Delimiter::Parenthesis) && pos == MacroPosition::Item) {
        Span search_span = mk_sp(mspan.hi, v.snippet_provider->end_pos());
        uint32_t hi = v.snippet_provider->span_before(search_span, ";");
        span = mk_sp(mspan.lo, hi + 1);
        if (rewrite && !ends_with(*rewrite, ";")) *rewrite += ";";
    }

    v.push_rewrite(span, std::move(rewrite));
}

// Remove spaces between the opening brace and the first statement or the inner attribute
// of the block.
void trim_spaces_after_opening_brace(FmtVisitor& v, const rust::Block* b, const rust::AttrVec* inner_attrs) {
    if (b->stmts.empty()) return;
    const rust::Stmt* first_stmt = b->stmts.front();
    uint32_t hi = spanned(StmtW::from_ast_node(first_stmt, false)).lo;
    if (inner_attrs) {
        rust::AttrVec inner = inner_attributes(*inner_attrs);
        if (!inner.empty()) hi = inner.front().span.lo;
    }
    Span missing_span = next_span(v, hi);
    std::string_view snippet = v.snippet(missing_span);
    auto slices = comment_code_slices(snippet);
    if (!slices.empty()) {
        const auto& [kind, off, s] = slices.front();
        (void)off;
        if (kind == CodeCharKind::Normal) {
            size_t len = s.rfind('\n');
            if (len != std::string_view::npos) v.last_pos = v.last_pos + static_cast<uint32_t>(len);
        }
    }
}

void close_block(FmtVisitor& v, Span span, bool unindent_comment) {
    const Config& config = *v.config;

    uint32_t last_hi = span.lo;
    bool unindented = false;
    bool prev_ends_with_newline = false;
    bool extra_newline = false;

    auto skip_normal = [](std::string_view s) {
        std::string_view trimmed = trim(s);
        if (trimmed.empty()) return true;
        for (char c : trimmed)
            if (c != ';') return false;
        return true;
    };

    std::string_view comment_snippet = v.snippet(span);

    bool align_to_right = false;
    if (unindent_comment && contains_comment(comment_snippet)) {
        std::string_view first_lines = comment_snippet.substr(0, comment_snippet.find('/'));
        align_to_right = last_line_width(first_lines) > last_line_width(comment_snippet);
    }

    for (const auto& [kind, offset, sub_slice] : comment_code_slices(comment_snippet)) {
        // transform_missing_snippet: identity
        if (kind == CodeCharKind::Comment) {
            if (!unindented && unindent_comment && !align_to_right) {
                unindented = true;
                v.block_indent = v.block_indent.block_unindent(config);
            }
            Span span_in_between = mk_sp(last_hi, span.lo + static_cast<uint32_t>(offset));
            std::string_view snippet_in_between = v.snippet(span_in_between);
            bool comment_on_same_line = !contains(snippet_in_between, '\n');

            Shape comment_shape = Shape::indented(v.block_indent, config).comment(config);
            if (config.style_edition() >= StyleEdition::E2024 && comment_on_same_line) {
                v.push_str(" ");
                // put the first line of the comment on the same line as the
                // block's last line
                size_t nl = sub_slice.find('\n');
                if (nl == std::string_view::npos) {
                    v.push_str(sub_slice);
                } else if (nl + 1 == sub_slice.size()) {
                    v.push_str(sub_slice.substr(0, nl));
                } else {
                    std::string_view first_line = sub_slice.substr(0, nl);
                    v.push_str(first_line);
                    v.push_str(v.block_indent.to_string_with_newline(config));

                    // put the other lines below it, shaping it as needed
                    std::string_view other_lines = sub_slice.substr(nl + 1);
                    RewriteResult comment_str = rewrite_comment(other_lines, false, comment_shape, config);
                    v.push_str(comment_str ? std::string_view(*comment_str) : other_lines);
                }
            } else {
                if (comment_on_same_line) {
                    // 1 = a space before `//`
                    size_t offset_len = 1 + sat_sub(last_line_width(v.buffer), v.block_indent.width());
                    std::optional<Shape> shp = comment_shape.visual_indent(offset_len).sub_width_opt(offset_len);
                    if (shp) {
                        comment_shape = *shp;
                    } else {
                        comment_on_same_line = false;
                    }
                }

                if (comment_on_same_line) {
                    v.push_str(" ");
                } else {
                    if (count_newlines(snippet_in_between) >= 2 || extra_newline) v.push_str("\n");
                    v.push_str(v.block_indent.to_string_with_newline(config));
                }

                RewriteResult comment_str = rewrite_comment(sub_slice, false, comment_shape, config);
                v.push_str(comment_str ? std::string_view(*comment_str) : sub_slice);
            }
        } else if (skip_normal(sub_slice)) {
            extra_newline = prev_ends_with_newline && contains(sub_slice, '\n');
            continue;
        } else {
            v.push_str(v.block_indent.to_string_with_newline(config));
            v.push_str(trim(sub_slice));
        }
        prev_ends_with_newline = ends_with(sub_slice, "\n");
        extra_newline = false;
        last_hi = span.lo + static_cast<uint32_t>(offset + sub_slice.size());
    }
    if (unindented) v.block_indent = v.block_indent.block_indent_(config);
    v.block_indent = v.block_indent.block_unindent(config);
    v.push_str(v.block_indent.to_string_with_newline(config));
    v.push_str("}");
}

bool unindent_comment_on_closing_brace(const FmtVisitor& v, const rust::Block* b) {
    return v.is_if_else_block && !b->stmts.empty();
}

void visit_stmt(FmtVisitor& v, const StmtW& stmt, bool include_empty_semi) {
    Span stmt_span = spanned(stmt);
    if (stmt.is_empty()) {
        // If the statement is empty, just skip over it. Before that, make sure any comment
        // snippet preceding the semicolon is picked up.
        std::string_view snippet = v.snippet(mk_sp(v.last_pos, stmt_span.lo));
        size_t i = snippet.find_first_not_of(' ');
        bool original_starts_with_newline = i != std::string_view::npos && starts_with_newline(snippet.substr(i));
        snippet = trim(snippet);
        if (!snippet.empty()) {
            // FIXME(calebcartwright 2021-01-03) - This exists strictly to maintain legacy
            // formatting where rustfmt would preserve redundant semicolons on Items in a
            // statement position.
            // See comment within `walk_stmts` for more info
            if (include_empty_semi) {
                v.format_missing(stmt_span.hi);
            } else {
                if (original_starts_with_newline) v.push_str("\n");

                v.push_str(v.block_indent.to_string(*v.config));
                v.push_str(snippet);
            }
        } else if (include_empty_semi) {
            v.push_str(";");
        }
        v.last_pos = stmt_span.hi;
        return;
    }

    const rust::Stmt* node = stmt.as_ast_node();
    switch (node->kind) {
    case rust::StmtKind::Item:
        v.visit_item(node->item);
        v.last_pos = stmt_span.hi;
        break;
    case rust::StmtKind::Let:
    case rust::StmtKind::Expr:
    case rust::StmtKind::Semi: {
        const rust::AttrVec* attrs = get_attrs_from_stmt(node);
        if (attrs && contains_skip(v.get_context(), *attrs)) {
            v.push_skipped_with_span(*attrs, stmt_span, get_span_without_attrs(node));
        } else {
            Shape shape = v.shape();
            Opt rewrite =
                with_context(v, [&](const RewriteContext& ctx) { return rewrite_result(ctx, stmt, shape).opt(); });
            v.push_rewrite(stmt_span, std::move(rewrite));
        }
        break;
    }
    case rust::StmtKind::MacCall: {
        const rust::MacCallStmt* mac_stmt = node->mac;
        if (v.visit_attrs(mac_stmt->attrs, rust::AttrStyle::Outer)) {
            v.push_skipped_with_span(mac_stmt->attrs, stmt_span, get_span_without_attrs(node));
        } else {
            visit_mac(v, mac_stmt->mac, MacroPosition::Statement);
        }
        v.format_missing(stmt_span.hi);
        break;
    }
    case rust::StmtKind::Empty: break;
    }
}

bool add_semi_on_last_block_stmt(const FmtVisitor& v, const rust::Stmt* stmt) {
    if (stmt->kind != rust::StmtKind::Expr) return false;
    const rust::Expr* expr = stmt->expr;

    if (v.is_macro_def) return false;

    switch (expr->kind) {
    case rust::ExprKind::Ret:
    case rust::ExprKind::Continue:
    case rust::ExprKind::Break: return v.config->trailing_semicolon();

    // TODO[reviewer-help]: This is roughly "does it end in a
    // curly". There might be a helper for this, or cases I'm
    // missing.
    case rust::ExprKind::Loop:
    case rust::ExprKind::While:
    case rust::ExprKind::ForLoop:
    case rust::ExprKind::Let:
    case rust::ExprKind::If:
    case rust::ExprKind::Match: return false;

    default: {
        // Checking the edition as before 2024 the lack of a
        // semicolon could impact temporary lifetimes[1].
        //
        // 1: https://rust-lang.github.io/rfcs/
        //      3606-temporary-lifetimes-in-tail-expressions.html
        bool allowed_to_add_semi = v.is_loop_block && v.config->edition() >= rust::Edition::E2024 &&
                                   v.config->style_edition() >= StyleEdition::E2027;
        return allowed_to_add_semi && v.config->trailing_semicolon();
    }
    }
}

// walk_stmts: rustfmt recurses on the tail slice; this is the same walk as a loop.
void walk_stmts(FmtVisitor& v, const std::vector<StmtW>& stmts, size_t start, bool include_current_empty_semi) {
    size_t idx = start;
    bool include = include_current_empty_semi;
    while (idx < stmts.size()) {
        // Extract leading `use ...;`.
        std::vector<const rust::Item*> items;
        for (size_t k = idx; k < stmts.size(); k++) {
            const rust::Item* it = stmts[k].to_item();
            if (it == nullptr || !is_use_item(it)) break;
            items.push_back(it);
        }

        if (items.empty()) {
            visit_stmt(v, stmts[idx], include);

            // FIXME(calebcartwright 2021-01-03) - This exists strictly to maintain legacy
            // formatting where rustfmt would preserve redundant semicolons on Items in a
            // statement position.
            //
            // Starting in rustc-ap-* v692 (~2020-12-01) the rustc parser now parses this as
            // two separate statements (Item and Empty kinds), whereas before it was parsed as
            // a single statement with the statement's span including the redundant semicolon.
            //
            // rustfmt typically tosses unnecessary/redundant semicolons, and eventually we
            // should toss these as well, but doing so at this time would
            // break the Stability Guarantee
            // N.B. This could be updated to utilize the version gates.
            bool include_next_empty = false;
            if (stmts.size() - idx > 1) {
                include_next_empty = stmts[idx].as_ast_node()->kind == rust::StmtKind::Item &&
                                     stmts[idx + 1].as_ast_node()->kind == rust::StmtKind::Empty;
            }
            include = include_next_empty;
            idx += 1;
        } else {
            visit_items_with_reordering(v, items);
            idx += items.size();
            include = false;
        }
    }
}

void walk_block_stmts(FmtVisitor& v, const rust::Block* b) {
    std::vector<StmtW> stmts = StmtW::from_ast_nodes(b->stmts);
    walk_stmts(v, stmts, 0, false);
}

bool is_unknown_rustfmt_attr(const std::vector<rust::PathSegment>& segments) {
    // segments[0].ident.to_string(): "rustfmt" is never printed with `r#`.
    if (segments.empty() || segments[0].ident.name != "rustfmt") return false;
    return !is_skip_attr(segments);
}

void format_mod(FmtVisitor& v, const rust::Item* item, const rust::AttrVec& attrs) {
    Span s = item->span;
    std::string vis_str = format_visibility(v.get_context(), item->vis);
    v.push_str(vis_str);
    v.push_str(format_safety(item->safety));
    v.push_str("mod ");
    std::string ident_str(rewrite_ident(v.get_context(), item->ident));
    v.push_str(ident_str);

    if (item->mod_inline) {
        Span inner_span = item->mod_inner_span;
        // (brace_style = SameLineWhere; non-default config: AlwaysNextLine)
        v.push_str(" {");
        // Hackery to account for the closing }.
        uint32_t mod_lo = v.snippet_provider->span_after(s, "{");
        std::string_view body_snippet = v.snippet(mk_sp(mod_lo, inner_span.hi - 1));
        body_snippet = trim(body_snippet);
        if (body_snippet.empty()) {
            v.push_str("}");
        } else {
            v.last_pos = mod_lo;
            v.block_indent = v.block_indent.block_indent_(*v.config);
            v.visit_attrs(attrs, rust::AttrStyle::Inner);
            v.walk_mod_items(item->items);
            Span missing_span = next_span(v, inner_span.hi - 1);
            close_block(v, missing_span, false);
        }
        v.last_pos = inner_span.hi;
    } else {
        v.push_str(";");
        v.last_pos = s.hi;
    }
}

void visit_ty_alias_kind(FmtVisitor& v, const rust::Item* item, ItemVisitorKind visitor_kind, Span span) {
    Opt rewrite = rewrite_type_alias(item, v.get_context(), v.block_indent, visitor_kind, span).opt();
    v.push_rewrite(span, std::move(rewrite));
}

void visit_assoc_item(FmtVisitor& v, const rust::Item* ai, ItemVisitorKind visitor_kind) {
    if (visitor_kind != ItemVisitorKind::AssocTraitItem && visitor_kind != ItemVisitorKind::AssocImplItem)
        panic("unreachable: visit_assoc_item", ai->span.lo);
    // TODO(calebcartwright): Not sure the skip spans are correct
    Span skip_span = ai->span;

    if (v.visit_attrs(ai->attrs, rust::AttrStyle::Outer)) {
        v.push_skipped_with_span(ai->attrs, skip_span, skip_span);
        return;
    }

    switch (ai->kind) {
    case rust::ItemKind::Const:
        // StaticParts::from_trait_item / from_impl_item
        visit_static(v, ai);
        break;
    case rust::ItemKind::Fn:
        if (ai->body != nullptr) {
            rust::AttrVec inner_attrs = inner_attributes(ai->attrs);
            v.visit_fn(ai, ai->span, &inner_attrs);
        } else {
            Indent indent = v.block_indent;
            Opt rewrite = rewrite_required_fn(v, indent, ai, ai->span).opt();
            v.push_rewrite(ai->span, std::move(rewrite));
        }
        break;
    case rust::ItemKind::TyAlias: visit_ty_alias_kind(v, ai, visitor_kind, ai->span); break;
    case rust::ItemKind::MacCall: visit_mac(v, ai->mac, MacroPosition::Item); break;
    default:
        // Delegation / DelegationMac:
        // TODO(ytmimi) #![feature(fn_delegation)]
        // add formatting for `AssocItemKind::Delegation` and `AssocItemKind::DelegationMac`
        v.push_rewrite(ai->span, std::nullopt);
        break;
    }
}

}  // namespace

// ===========================================================================

FmtVisitor::~FmtVisitor() {
    if (parent_context && macro_rewrite_failure) parent_context->macro_rewrite_failure = true;
}

FmtVisitor FmtVisitor::from_parse(rust::ParseResult* pr, const Config& config, const SnippetProvider& sp,
                                  FormatState& state) {
    // (skip_macro_invocations is empty by default)
    FmtVisitor v;
    v.parent_context = nullptr;
    v.pr = pr;
    v.buffer.reserve(sp.entire_snippet().size() * 2);
    v.last_pos = 0;
    v.block_indent = Indent::empty();
    v.config = &config;
    v.is_if_else_block = false;
    v.is_loop_block = false;
    v.snippet_provider = &sp;
    v.line_number = 0;
    v.is_macro_def = false;
    v.macro_rewrite_failure = false;
    v.state = &state;
    return v;
}

FmtVisitor FmtVisitor::from_context(const RewriteContext& ctx) {
    FmtVisitor visitor = from_parse(ctx.pr, *ctx.config, *ctx.snippet_provider, *ctx.state);
    visitor.skip_context.update(ctx.skip_context);
    visitor.parent_context = &ctx;
    return visitor;
}

std::string_view FmtVisitor::snippet(Span span) const {
    std::optional<std::string_view> s = opt_snippet(span);
    if (!s) panic("snippet: span out of range", span.lo);
    return *s;
}

RewriteContext FmtVisitor::get_context() const {
    RewriteContext ctx;
    ctx.pr = pr;
    ctx.config = config;
    ctx.snippet_provider = snippet_provider;
    ctx.state = state;
    ctx.inside_macro_ = std::make_shared<bool>(false);
    ctx.use_block = false;
    ctx.is_if_else_block_ = false;
    ctx.is_loop_block_ = false;
    ctx.force_one_line_chain = false;
    ctx.macro_rewrite_failure = false;
    ctx.is_macro_def = is_macro_def;
    ctx.skip_context = skip_context;
    ctx.skipped_range = skipped_range;
    return ctx;
}

void FmtVisitor::visit_block(const rust::Block* b, const rust::AttrVec* inner_attrs, bool has_braces) {
    DepthGuard g(*state);

    // Check if this block has braces.
    uint32_t brace_compensation = has_braces ? 1 : 0;

    last_pos = last_pos + brace_compensation;
    block_indent = block_indent.block_indent_(*config);
    push_str("{");
    trim_spaces_after_opening_brace(*this, b, inner_attrs);

    // Format inner attributes if available.
    if (inner_attrs) visit_attrs(*inner_attrs, rust::AttrStyle::Inner);

    walk_block_stmts(*this, b);

    if (!b->stmts.empty()) {
        if (add_semi_on_last_block_stmt(*this, b->stmts.back())) push_str(";");
    }

    // (out_of_file_lines_range! is false) Ignore the closing brace.
    Span missing_span = next_span(*this, b->span.hi - brace_compensation);
    close_block(*this, missing_span, unindent_comment_on_closing_brace(*this, b));
    last_pos = b->span.hi;
}

// Note that this only gets called for function definitions. Required methods
// on traits do not get handled here.
void FmtVisitor::visit_fn(const rust::Item* fn_item, Span s, const rust::AttrVec* inner_attrs) {
    Indent indent = block_indent;
    const rust::Block* block = fn_item->body;
    if (block == nullptr) panic("unreachable: visit_fn without a body", s.lo);
    std::optional<std::pair<std::string, FnBraceStyle>> rewrite =
        rewrite_fn_before_block(*this, indent, fn_item, mk_sp(s.lo, block->span.lo));

    if (rewrite) {
        const std::string& fn_str = rewrite->first;
        FnBraceStyle fn_brace_style = rewrite->second;
        format_missing_with_indent(s.lo);

        if (Opt rw = single_line_fn(*this, fn_str, block, inner_attrs)) {
            push_str(*rw);
            last_pos = s.hi;
            return;
        }

        push_str(fn_str);
        switch (fn_brace_style) {
        case FnBraceStyle::SameLine: push_str(" "); break;
        case FnBraceStyle::NextLine: push_str(block_indent.to_string_with_newline(*config)); break;
        default: panic("unreachable: fn brace style", s.lo);
        }
        last_pos = block->span.lo;
    } else {
        format_missing(block->span.lo);
    }

    visit_block(block, inner_attrs, true);
}

void FmtVisitor::visit_item(const rust::Item* item) {
    DepthGuard g(*state);

    // This is where we bail out if there is a skip attribute. This is only
    // complex in the module case. It is complex because the module could be
    // in a separate file and there might be attributes in both files, but
    // the AST lumps them all together.
    rust::AttrVec filtered_attrs;
    const rust::AttrVec* attrs = &item->attrs;
    SkipContext skip_context_saved = skip_context;
    skip_context.update_with_attrs(*pr, *attrs);

    bool should_visit_node_again;
    if (item->kind == rust::ItemKind::Use || item->kind == rust::ItemKind::ExternCrate) {
        // For use/extern crate items, skip rewriting attributes but check for a skip attribute.
        if (contains_skip(get_context(), *attrs)) {
            push_skipped_with_span(*attrs, spanned(item), spanned(item));
            should_visit_node_again = false;
        } else {
            should_visit_node_again = true;
        }
    } else if (!is_mod_decl(item) || item->kind != rust::ItemKind::Mod) {
        // `_ if !is_mod_decl(item)`: Module is inline, in this case we treat it like any other item.
        // `_`: (is_mod_decl is true for every item that is not an inline module) any other item.
        if (visit_attrs(item->attrs, rust::AttrStyle::Outer)) {
            push_skipped_with_span(item->attrs, spanned(item), spanned(item));
            should_visit_node_again = false;
        } else {
            should_visit_node_again = true;
        }
    } else if (contains_skip(get_context(), item->attrs)) {
        // Module is not inline, but should be skipped.
        should_visit_node_again = false;
    } else {
        // Module is not inline and should not be skipped. We want
        // to process only the attributes in the current file.
        filtered_attrs = filter_inline_attrs(item->attrs, spanned(item));
        // Assert because if we should skip it should be caught by
        // the above case.
        if (visit_attrs(filtered_attrs, rust::AttrStyle::Outer)) panic("assertion failed: mod decl skipped", item->span.lo);
        attrs = &filtered_attrs;
        should_visit_node_again = true;
    }

    // TODO(calebcartwright): consider enabling box_patterns feature gate
    if (should_visit_node_again) {
        switch (item->kind) {
        case rust::ItemKind::Use: format_import(*this, item); break;
        case rust::ItemKind::Impl: {
            Indent bi = block_indent;
            RewriteResult rw = with_context(*this, [&](const RewriteContext& ctx) { return format_impl(ctx, item, bi); });
            push_rewrite(item->span, rw.opt());
            break;
        }
        case rust::ItemKind::Trait: {
            Indent bi = block_indent;
            RewriteResult rw =
                with_context(*this, [&](const RewriteContext& ctx) { return format_trait(ctx, item, bi); });
            push_rewrite(item->span, rw.opt());
            break;
        }
        case rust::ItemKind::TraitAlias: {
            Shape shape = Shape::indented(block_indent, *config);
            RewriteResult rw = format_trait_alias(get_context(), item, shape);
            push_rewrite(item->span, rw.opt());
            break;
        }
        case rust::ItemKind::ExternCrate: {
            RewriteResult rw = rewrite_extern_crate(get_context(), item, shape());
            Span span = attrs->empty() ? item->span : mk_sp((*attrs)[0].span.lo, item->span.hi);
            push_rewrite(span, rw.opt());
            break;
        }
        case rust::ItemKind::Struct:
        case rust::ItemKind::Union: visit_struct(*this, item); break;
        case rust::ItemKind::Enum:
            format_missing_with_indent(item->span.lo);
            visit_enum(*this, item);
            last_pos = item->span.hi;
            break;
        case rust::ItemKind::Mod:
            format_missing_with_indent(item->span.lo);
            format_mod(*this, item, *attrs);
            break;
        case rust::ItemKind::MacCall: visit_mac(*this, item->mac, MacroPosition::Item); break;
        case rust::ItemKind::ForeignMod:
            format_missing_with_indent(item->span.lo);
            format_foreign_mod(*this, item);
            break;
        case rust::ItemKind::Static:
        case rust::ItemKind::Const: visit_static(*this, item); break;
        case rust::ItemKind::ConstBlock: {
            RewriteContext context = get_context();
            Indent offset = block_indent;
            // ConstBlockItem::span: `const` .. the block's end.
            Span span = mk_sp(item->span.lo, item->body->span.hi);
            Opt rw = rewrite_result(context, item->body,
                                    Shape::legacy(context.budget(offset.block_indent), offset.block_only()))
                         .opt();
            if (rw) rw = recover_comment_removed("const " + *rw, span, context);
            push_rewrite(item->span, std::move(rw));
            break;
        }
        case rust::ItemKind::Fn:
            if (item->body != nullptr) {
                rust::AttrVec inner_attrs = inner_attributes(item->attrs);
                visit_fn(item, item->span, &inner_attrs);
            } else {
                Indent indent = block_indent;
                Opt rewrite = rewrite_required_fn(*this, indent, item, item->span).opt();
                push_rewrite(item->span, std::move(rewrite));
            }
            break;
        case rust::ItemKind::TyAlias: visit_ty_alias_kind(*this, item, ItemVisitorKind::Item, item->span); break;
        case rust::ItemKind::GlobalAsm: {
            Opt snippet_ = std::string(snippet(item->span));
            push_rewrite(item->span, std::move(snippet_));
            break;
        }
        case rust::ItemKind::MacroDef: {
            Opt rewrite = rewrite_macro_def(get_context(), shape(), block_indent, item, item->span).opt();
            push_rewrite(item->span, std::move(rewrite));
            break;
        }
        case rust::ItemKind::Delegation:
        case rust::ItemKind::DelegationMac:
            // TODO: rewrite delegation items once syntax is established.
            // For now, leave the contents of the Span unformatted.
            push_rewrite(item->span, std::nullopt);
            break;
        }
    }
    skip_context = std::move(skip_context_saved);
}

void FmtVisitor::visit_trait_item(const rust::Item* ti) { visit_assoc_item(*this, ti, ItemVisitorKind::AssocTraitItem); }

void FmtVisitor::visit_impl_item(const rust::Item* ii) { visit_assoc_item(*this, ii, ItemVisitorKind::AssocImplItem); }

void FmtVisitor::push_str(std::string_view s) {
    line_number += count_newlines(s);
    buffer.append(s);
}

void FmtVisitor::push_rewrite(Span span, Opt rewrite) {
    format_missing_with_indent(span.lo);
    push_rewrite_inner(*this, span, std::move(rewrite));
}

void FmtVisitor::push_skipped_with_span(const rust::AttrVec& attrs, Span item_span, Span main_span) {
    format_missing_with_indent(item_span.lo);
    // do not take into account the lines with attributes as part of the skipped range
    size_t attrs_end = 0;
    bool any = false;
    for (const rust::Attribute& attr : attrs) {
        size_t l = line_of_byte_pos(*this, attr.span.hi);
        if (!any || l > attrs_end) attrs_end = l;
        any = true;
    }
    if (!any) attrs_end = 1;
    size_t first_line = line_of_byte_pos(*this, main_span.lo);
    // Statement can start after some newlines and/or spaces
    // or it can be on the same line as the last attribute.
    // So here we need to take a minimum between the two.
    size_t lo = std::min(attrs_end + 1, first_line);
    push_rewrite_inner(*this, item_span, std::nullopt);
    size_t hi = line_number + 1;
    skipped_range->push_back({lo, hi});
}

// Returns true if we should skip the following item.
bool FmtVisitor::visit_attrs(const rust::AttrVec& attrs, rust::AttrStyle style) {
    for (const rust::Attribute& attr : attrs) {
        if (!attr.is_doc_comment && attr.path != nullptr && attr.path->segments.size() == 1 &&
            attr.path->segments[0].args == nullptr && attr.path->segments[0].ident.name == "rustfmt_skip") {
            // FormatReport: ErrorKind::DeprecatedAttr (reported only; the output is unchanged)
        } else if (!attr.is_doc_comment && attr.path != nullptr && is_unknown_rustfmt_attr(attr.path->segments)) {
            // FormatReport: ErrorKind::BadAttr (reported only; the output is unchanged)
        }
    }
    RewriteContext ctx = get_context();
    if (contains_skip(ctx, attrs)) return true;

    rust::AttrVec filtered;
    for (const rust::Attribute& a : attrs)
        if (a.style == style) filtered.push_back(a);
    if (filtered.empty()) return false;

    Opt rewrite = rewrite_attrs(ctx, filtered, shape()).opt();
    Span span = mk_sp(filtered.front().span.lo, filtered.back().span.hi);
    push_rewrite(span, std::move(rewrite));

    return false;
}

void FmtVisitor::walk_mod_items(const std::vector<rust::Item*>& items) {
    std::vector<const rust::Item*> refs(items.begin(), items.end());
    visit_items_with_reordering(*this, refs);
}

void FmtVisitor::format_separate_mod(const rust::Crate* crate, uint32_t end_pos) {
    block_indent = Indent::empty();
    bool skipped = visit_attrs(crate->attrs, rust::AttrStyle::Inner);
    if (skipped) panic("Skipping module must be handled before reaching this line.");
    walk_mod_items(crate->items);
    format_missing_with_indent(end_pos);
}

void FmtVisitor::skip_empty_lines(uint32_t end_pos) {
    while (std::optional<uint32_t> pos = snippet_provider->opt_span_after(next_span(*this, end_pos), "\n")) {
        std::optional<std::string_view> snippet_ = opt_snippet(next_span(*this, *pos));
        if (!snippet_) return;  // (rustfmt would loop forever here)
        if (trim(*snippet_).empty()) {
            last_pos = *pos;
        } else {
            return;
        }
    }
}

}  // namespace rustfmt
