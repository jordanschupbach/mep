// rust_format_attr.cpp -- rustfmt's attr.rs (plus rustc_ast's
// `Attribute::meta()` / `meta_item_list()`, which build MetaItems from an
// attribute's tokens).
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

namespace rustfmt {

// ===========================================================================
// rustc_ast attr/mod.rs: MetaItem::from_tokens and friends, over the flat
// token vector of the file (delimiters are tokens; `partner` pairs them).

namespace {

using rust::Tok;
using rust::Token;

struct MetaParser {
    const rust::ParseResult& pr;
    rust::Arena& arena;

    const Token& tok(uint32_t i) const { return pr.tokens[i]; }
    static bool is_open(Tok k) { return k == Tok::OpenParen || k == Tok::OpenBrace || k == Tok::OpenBracket; }
    static bool is_close(Tok k) { return k == Tok::CloseParen || k == Tok::CloseBrace || k == Tok::CloseBracket; }
    // The token tree at `i`: one token, or a delimited group up to its partner.
    uint32_t tree_end(uint32_t i) const {
        if (is_open(tok(i).kind) && i < pr.partner.size() && pr.partner[i] != UINT32_MAX) return pr.partner[i] + 1;
        return i + 1;
    }
    Span tree_span(uint32_t i) const {
        if (is_open(tok(i).kind) && i < pr.partner.size() && pr.partner[i] != UINT32_MAX)
            return tok(i).span.to(tok(pr.partner[i]).span);
        return tok(i).span;
    }

    // MetaItemLit::from_token: token::Lit::from_token then LitKind::from_token_lit(..).ok().
    std::optional<rust::Lit> lit_from_token(const Token& t) const {
        rust::Lit lit;
        if (t.kind == Tok::Literal) {
            lit.kind = t.lit;
            lit.symbol = t.sym;
            lit.suffix = t.suffix;
            lit.n_hashes = t.n_hashes;
            lit.span = t.span;
        } else if (t.kind == Tok::Ident && !t.raw && (t.kw == rust::Kw::True || t.kw == rust::Kw::False)) {
            lit.kind = rust::LitKind::Bool;
            lit.symbol = t.sym;
            lit.span = t.span;
        } else {
            return std::nullopt;
        }
        if (!lit_ok(lit)) return std::nullopt;
        return lit;
    }

    static bool strip_and_parse_int(std::string_view s) {
        std::string digits;
        for (char c : s)
            if (c != '_') digits += c;
        unsigned base = 10;
        size_t start = 0;
        if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b')) {
            base = digits[1] == 'x' ? 16u : digits[1] == 'o' ? 8u : 2u;
            start = 2;
        }
        // u128::from_str_radix: non-empty, every digit valid, no overflow.
        if (start >= digits.size()) return false;
        unsigned __int128 v = 0;
        const unsigned __int128 max = ~static_cast<unsigned __int128>(0);
        for (size_t i = start; i < digits.size(); i++) {
            char c = digits[i];
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            if (d >= base) return false;
            if (v > (max - d) / base) return false;
            v = v * base + d;
        }
        return true;
    }

    // LitKind::from_token_lit(..).is_ok()
    static bool lit_ok(const rust::Lit& lit) {
        using rust::LitKind;
        bool may_have_suffix = lit.kind == LitKind::Integer || lit.kind == LitKind::Float || lit.kind == LitKind::Err;
        if (!lit.suffix.empty() && !may_have_suffix) return false;
        auto float_suffix_ok = [](std::string_view suf) {
            return suf.empty() || suf == "f16" || suf == "f32" || suf == "f64" || suf == "f128";
        };
        switch (lit.kind) {
        case LitKind::Integer: {
            std::string_view s = lit.symbol;
            bool non_decimal = s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b');
            std::string_view suf = lit.suffix;
            if (!suf.empty()) {
                static const char* const kInt[] = {"isize", "i8",  "i16", "i32", "i64",  "i128",
                                                   "usize", "u8",  "u16", "u32", "u64",  "u128"};
                bool known = false;
                for (const char* k : kInt)
                    if (suf == k) known = true;
                if (!known) {
                    if (suf[0] == 'f') return !non_decimal && float_suffix_ok(suf);  // filtered_float_lit
                    return false;
                }
            }
            return strip_and_parse_int(s);
        }
        case LitKind::Float: return float_suffix_ok(lit.suffix);
        default: return true;
        }
    }

    // MetaItemKind::list_from_tokens over tokens [lo, hi).
    std::optional<std::vector<MetaItemInner>> list_from_tokens(uint32_t lo, uint32_t hi) const {
        std::vector<MetaItemInner> result;
        uint32_t i = lo;
        while (i < hi) {
            std::optional<MetaItemInner> item = inner_from_tokens(i, hi);
            if (!item) return std::nullopt;
            result.push_back(*item);
            if (i >= hi) break;
            if (tok(i).kind == Tok::Comma) {
                i++;
            } else {
                return std::nullopt;
            }
        }
        return result;
    }

    // MetaItemInner::from_tokens
    std::optional<MetaItemInner> inner_from_tokens(uint32_t& i, uint32_t hi) const {
        if (i < hi && !is_open(tok(i).kind)) {
            if (std::optional<rust::Lit> lit = lit_from_token(tok(i))) {
                i++;
                MetaItemInner r;
                r.is_lit = true;
                r.lit = *lit;
                r.span = lit->span;
                return r;
            }
        }
        MetaItem* mi = meta_item_from_tokens(i, hi);
        if (!mi) return std::nullopt;
        MetaItemInner r;
        r.mi = mi;
        r.span = mi->span;
        return r;
    }

    static rust::PathSegment segment(std::string_view name, Span span, bool raw) {
        rust::PathSegment seg;
        seg.ident.name = name;
        seg.ident.span = span;
        seg.ident.raw = raw;
        return seg;
    }

    // MetaItem::from_tokens
    MetaItem* meta_item_from_tokens(uint32_t& i, uint32_t hi) const {
        if (i >= hi) return nullptr;
        const Token& first = tok(i);
        if (first.kind != Tok::Ident && first.kind != Tok::PathSep) return nullptr;
        rust::Path* path = arena.make<rust::Path>();
        Span span = first.span;
        i++;
        bool single = false;
        if (first.kind == Tok::Ident) {
            path->segments.push_back(segment(first.sym, first.span, first.raw));
            if (i < hi && tok(i).kind == Tok::PathSep) {
                i++;
            } else {
                path->span = first.span;
                single = true;
            }
        } else {
            path->segments.push_back(segment("{{root}}", first.span, false));
        }
        if (!single) {
            for (;;) {
                if (i >= hi || tok(i).kind != Tok::Ident) return nullptr;
                path->segments.push_back(segment(tok(i).sym, tok(i).span, tok(i).raw));
                i++;
                if (i < hi && tok(i).kind == Tok::PathSep) {
                    i++;
                    continue;
                }
                break;
            }
            path->span = span.with_hi(path->segments.back().ident.span.hi);
        }
        std::optional<uint32_t> list_closing_paren_pos;
        if (i < hi) list_closing_paren_pos = tree_span(i).hi;
        MetaItem* mi = arena.make<MetaItem>();
        mi->path = path;
        if (!kind_from_tokens(i, hi, *mi)) return nullptr;
        uint32_t end;
        switch (mi->kind) {
        case MetaItem::NameValue: end = mi->value.span.hi; break;
        case MetaItem::List: end = list_closing_paren_pos ? *list_closing_paren_pos : path->span.hi; break;
        default: end = path->span.hi; break;
        }
        mi->span = path->span.with_hi(end);
        return mi;
    }

    // MetaItemKind::from_tokens
    bool kind_from_tokens(uint32_t& i, uint32_t hi, MetaItem& mi) const {
        if (i < hi && tok(i).kind == Tok::OpenParen) {
            uint32_t end = tree_end(i);
            Span sp = tree_span(i);
            std::optional<std::vector<MetaItemInner>> list = list_from_tokens(i + 1, end - 1);
            i = end;
            if (!list) return false;
            mi.kind = MetaItem::List;
            mi.list = std::move(*list);
            mi.list_span = sp;
            return true;
        }
        if (i < hi && is_open(tok(i).kind)) return false;
        if (i < hi && tok(i).kind == Tok::Eq) {
            i++;
            // name_value_from_tokens
            if (i >= hi || is_open(tok(i).kind) || is_close(tok(i).kind)) return false;
            std::optional<rust::Lit> lit = lit_from_token(tok(i));
            i++;
            if (!lit) return false;
            mi.kind = MetaItem::NameValue;
            mi.value = *lit;
            return true;
        }
        mi.kind = MetaItem::Word;
        return true;
    }
};

}  // namespace

MetaItem* parse_meta(const RewriteContext& ctx, const rust::Attribute& attr) {
    if (attr.is_doc_comment || attr.path == nullptr) return nullptr;
    MetaParser p{*ctx.pr, ctx.arena()};
    // AttrItem::meta_kind -> MetaItemKind::from_attr_args
    MetaItem* mi = ctx.arena().make<MetaItem>();
    switch (attr.args.kind) {
    case rust::AttrArgs::Empty: mi->kind = MetaItem::Word; break;
    case rust::AttrArgs::Delimited: {
        if (attr.args.delim.delim != rust::DelimKind::Paren) return nullptr;
        std::optional<std::vector<MetaItemInner>> list = p.list_from_tokens(attr.args.delim.tok_lo, attr.args.delim.tok_hi);
        if (!list) return nullptr;
        mi->kind = MetaItem::List;
        mi->list = std::move(*list);
        mi->list_span = attr.args.delim.span();
        break;
    }
    case rust::AttrArgs::Eq: {
        const rust::Expr* e = attr.args.expr;
        if (e == nullptr || e->kind != rust::ExprKind::Lit) return nullptr;
        rust::Lit lit = e->lit;
        lit.span = e->span;
        if (!MetaParser::lit_ok(lit)) return nullptr;
        mi->kind = MetaItem::NameValue;
        mi->value = lit;
        break;
    }
    }
    // AttrItem::meta: unsafety is Default, the path is the attribute's, the span the attribute's.
    mi->path = attr.path;
    mi->span = attr.span;
    return mi;
}

namespace {

// Attribute::meta_item_list(): a parenthesised meta-item list, else None.
std::optional<std::vector<MetaItemInner>> meta_item_list(const RewriteContext& ctx, const rust::Attribute& attr) {
    if (attr.is_doc_comment || attr.path == nullptr) return std::nullopt;
    if (attr.args.kind != rust::AttrArgs::Delimited || attr.args.delim.delim != rust::DelimKind::Paren)
        return std::nullopt;
    MetaParser p{*ctx.pr, ctx.arena()};
    return p.list_from_tokens(attr.args.delim.tok_lo, attr.args.delim.tok_hi);
}

// Attribute::name(): the name of a one-segment path (Symbol: without `r#`).
std::optional<std::string_view> attr_name(const rust::Attribute& attr) {
    if (attr.is_doc_comment || attr.path == nullptr || attr.path->segments.size() != 1) return std::nullopt;
    return attr.path->segments[0].ident.name;
}

}  // namespace

Span spanned(const MetaItemInner* x) { return x->is_lit ? x->lit.span : x->mi->span; }

// ===========================================================================
// attr.rs

/// Returns attributes on the given statement.
const rust::AttrVec* get_attrs_from_stmt(const rust::Stmt* stmt) {
    static const rust::AttrVec kEmpty;
    switch (stmt->kind) {
    case rust::StmtKind::Let: return &stmt->local->attrs;
    case rust::StmtKind::Item: return &stmt->item->attrs;
    case rust::StmtKind::Expr:
    case rust::StmtKind::Semi: return &stmt->expr->attrs;
    case rust::StmtKind::MacCall: return &stmt->mac->attrs;
    case rust::StmtKind::Empty: return &kEmpty;
    }
    return &kEmpty;
}

Span get_span_without_attrs(const rust::Stmt* stmt) {
    switch (stmt->kind) {
    case rust::StmtKind::Let: return stmt->local->span;
    case rust::StmtKind::Item: return stmt->item->span;
    case rust::StmtKind::Expr:
    case rust::StmtKind::Semi: return stmt->expr->span;
    case rust::StmtKind::MacCall: {
        const rust::MacCall* m = stmt->mac->mac;
        return m->path->span.to(m->args.span());  // MacCall::span()
    }
    case rust::StmtKind::Empty: return stmt->span;
    }
    return stmt->span;
}

/// Returns attributes that are within `outer_span`.
rust::AttrVec filter_inline_attrs(const rust::AttrVec& attrs, Span outer_span) {
    rust::AttrVec out;
    for (const rust::Attribute& a : attrs)
        if (outer_span.lo <= a.span.lo && a.span.hi <= outer_span.hi) out.push_back(a);
    return out;
}

namespace {

// attr.has_name(sym::derive): a one-segment, arg-free path `derive`.
bool is_derive(const rust::Attribute& attr) {
    return !attr.is_doc_comment && attr.path != nullptr && attr.path->segments.size() == 1 &&
           attr.path->segments[0].args == nullptr && attr.path->segments[0].ident.name == "derive";
}

const char* attr_prefix(const rust::Attribute& attr) { return attr.style == rust::AttrStyle::Inner ? "#!" : "#"; }

// The shape of the arguments to a function-like attribute.
std::optional<Shape> argument_shape(size_t left, size_t /*right*/, bool combine, Shape shape,
                                    const RewriteContext& ctx) {
    // IndentStyle::Block (non-default config: indent_style = Visual)
    if (combine) return shape.offset_left_opt(left);
    return shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config);
}

Opt format_derive(const rust::Attribute* derives, size_t n, Shape shape, const RewriteContext& ctx) {
    // Collect all items from all attributes
    std::vector<ListItem> all_items;
    for (size_t k = 0; k < n; k++) {
        const rust::Attribute& attr = derives[k];
        // Parse the derive items and extract the span for each item; if any
        // attribute is not parseable, none of the attributes will be
        // reformatted.
        std::optional<std::vector<MetaItemInner>> list = meta_item_list(ctx, attr);
        if (!list) return std::nullopt;
        std::vector<Span> item_spans;
        for (const MetaItemInner& m : *list) item_spans.push_back(spanned(&m));
        std::vector<ListItem> items = itemize_list(
            *ctx.snippet_provider, item_spans.size(), ")", ",",
            [&](size_t i) { return item_spans[i].lo; }, [&](size_t i) { return item_spans[i].hi; },
            [&](size_t i) { return RewriteResult(std::string(ctx.snippet(item_spans[i]))); },
            // We update derive attribute spans to start after the opening '('
            // This helps us focus parsing to just what's inside #[derive(...)]
            ctx.snippet_provider->span_after(attr.span, "("), attr.span.hi, false);
        for (ListItem& it : items) all_items.push_back(std::move(it));
    }

    // Collect formatting parameters.
    std::string_view prefix = attr_prefix(derives[0]);
    std::optional<Shape> arg_shape = argument_shape(std::string_view("[derive()]").size() + prefix.size(),
                                                    std::string_view(")]").size(), false, shape, ctx);
    if (!arg_shape) return std::nullopt;
    std::optional<Shape> ols = shape.offset_left_opt(std::string_view("[derive()]").size() + prefix.size());
    if (!ols) return std::nullopt;
    ols = ols->sub_width_opt(std::string_view("()]").size());
    if (!ols) return std::nullopt;
    size_t one_line_budget = ols->width;

    DefinitiveListTactic tactic =
        definitive_tactic(all_items, ListTactic::HorizontalVertical(), Separator::Comma, arg_shape->width);
    // IndentStyle::Block: we always add the trailing comma and remove it if it is not needed.
    SeparatorTactic trailing_separator = SeparatorTactic::Always;

    // Format the collection of items.
    ListFormatting fmt(*arg_shape, *ctx.config);
    fmt.tactic(tactic).trailing_separator(trailing_separator).ends_with_newline(false);
    RewriteResult item_rw = write_list(all_items, fmt);
    if (!item_rw) return std::nullopt;
    const std::string& item_str = *item_rw;

    // Determine if the result will be nested, i.e. if we're using the block
    // indent style and either the items are on multiple lines or we've exceeded
    // our budget to fit on a single line.
    bool nested = contains(item_str, '\n') || item_str.size() > one_line_budget;

    // Format the final result.
    std::string result;
    result += prefix;
    result += "[derive(";
    if (nested) {
        result += arg_shape->indent.to_string_with_newline(*ctx.config);
        result += item_str;
        result += shape.indent.to_string_with_newline(*ctx.config);
    } else if (ends_with(item_str, ",")) {
        // (trailing_comma = Vertical) Remove the trailing comma.
        result += std::string_view(item_str).substr(0, item_str.size() - 1);
    } else {
        result += item_str;
    }
    result += ")]";
    return result;
}

/// Returns the length of the first group of attributes that fills the given predicate.
/// We consider two doc comments are in different group if they are separated by normal comments.
template <class P>
size_t take_while_with_pred(const RewriteContext& ctx, const rust::Attribute* attrs, size_t n, P pred) {
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        const rust::Attribute& attr = attrs[i];
        if (pred(attr)) {
            len++;
        } else {
            break;
        }
        if (i + 1 < n) {
            const rust::Attribute& next_attr = attrs[i + 1];
            // Extract comments between two attributes.
            Span span_between_attr = mk_sp(attr.span.hi, next_attr.span.lo);
            std::string_view snippet = ctx.snippet(span_between_attr);
            if (count_newlines(snippet) >= 2 || contains(snippet, '/')) break;
        }
    }
    return len;
}

/// Rewrite the any doc comments which come before any other attributes.
Expected<std::pair<size_t, Opt>> rewrite_initial_doc_comments(const RewriteContext& ctx,
                                                              const rust::Attribute* attrs, size_t n,
                                                              Shape shape) {
    if (n == 0) return std::pair<size_t, Opt>(0, std::nullopt);
    // Rewrite doc comments
    size_t sugared_docs =
        take_while_with_pred(ctx, attrs, n, [](const rust::Attribute& a) { return a.is_doc_comment; });
    if (sugared_docs > 0) {
        std::string snippet;
        for (size_t i = 0; i < sugared_docs; i++) {
            if (i) snippet += "\n";
            snippet += ctx.snippet(attrs[i].span);
        }
        RewriteResult rw = rewrite_doc_comment(snippet, shape.comment(*ctx.config), *ctx.config);
        if (!rw) return rw.error();
        return std::pair<size_t, Opt>(sugared_docs, std::move(*rw));
    }
    return std::pair<size_t, Opt>(0, std::nullopt);
}

bool is_unicode_ws(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::pair<const char*, const char*> has_newlines_before_after_comment(std::string_view comment) {
    // Look at before and after comment and see if there are any empty lines.
    size_t comment_begin = comment.find('/');
    size_t len = comment_begin == std::string_view::npos ? comment.size() : comment_begin;
    bool mlb = count_newlines(comment.substr(0, len)) > 1;
    bool mla;
    if (comment_begin == std::string_view::npos) {
        mla = mlb;
    } else {
        // comment.chars().rev().take_while(|c| c.is_whitespace()).filter(|&c| c == '\n').count() > 1
        size_t count = 0;
        size_t end = comment.size();
        while (end > 0) {
            size_t start = end - 1;
            while (start > 0 && (static_cast<unsigned char>(comment[start]) & 0xC0) == 0x80) start--;
            size_t clen = 0;
            uint32_t c = rust::decode_utf8(comment, start, clen);
            if (!is_unicode_ws(c)) break;
            if (c == '\n') count++;
            end = start;
        }
        mla = count > 1;
    }
    return {mlb ? "\n" : "", mla ? "\n" : ""};
}

}  // namespace

RewriteResult rewrite_result(const RewriteContext& ctx, const MetaItemInner* x, Shape shape) {
    if (!x->is_lit) return rewrite_result(ctx, x->mi, shape);
    return rewrite_literal(ctx, x->lit, x->lit.span, shape);
}

RewriteResult rewrite_result(const RewriteContext& ctx, const MetaItem* x, Shape shape) {
    switch (x->kind) {
    case MetaItem::Word: return rewrite_path(ctx, PathContext::Type, nullptr, x->path, shape);
    case MetaItem::List: {
        RewriteResult path = rewrite_path(ctx, PathContext::Type, nullptr, x->path, shape);
        if (!path) return path;
        bool has_trailing_comma = span_ends_with_comma(ctx, x->span);
        std::vector<OverflowableItem> items;
        for (const MetaItemInner& m : x->list) items.push_back(RwNode::of(&m));
        // 1 = "]"
        std::optional<Shape> sh = shape.sub_width(1);
        if (!sh) return Err(RewriteError::exceeds(shape.exceeds(x->span)));
        return rewrite_with_parens(ctx, *path, items, *sh, x->span, ctx.config->attr_fn_like_width(),
                                   has_trailing_comma ? SeparatorTactic::Always : SeparatorTactic::Never);
    }
    case MetaItem::NameValue: {
        RewriteResult path = rewrite_path(ctx, PathContext::Type, nullptr, x->path, shape);
        if (!path) return path;
        // 3 = ` = `
        std::optional<Shape> lit_shape = shape.shrink_left(path->size() + 3);
        if (!lit_shape) return Err(RewriteError::exceeds(shape.exceeds(x->span)));
        // `rewrite_literal` returns `None` when `lit` exceeds max
        // width. Since a literal is basically unformattable unless it
        // is a string literal (and only if `format_strings` is set),
        // we might be better off ignoring the fact that the attribute
        // is longer than the max width and continue on formatting.
        // See #2479 for example.
        RewriteResult value = rewrite_literal(ctx, x->value, x->value.span, *lit_shape);
        std::string v = value ? *value : std::string(ctx.snippet(x->value.span));
        return *path + " = " + v;
    }
    }
    return Err(RewriteError::unknown());
}

RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Attribute* x, Shape shape) {
    std::string_view snippet = ctx.snippet(x->span);
    if (x->is_doc_comment) return rewrite_doc_comment(snippet, shape.comment(*ctx.config), *ctx.config);
    std::optional<std::string_view> name = attr_name(*x);
    bool should_skip = name ? ctx.skip_context.attributes.skip(*name) : false;
    std::string_view prefix = attr_prefix(*x);

    if (should_skip || contains_comment(snippet)) return std::string(snippet);

    if (const MetaItem* meta = parse_meta(ctx, *x)) {
        // non-default config: normalize_doc_attributes
        // 1 = `[`
        std::optional<Shape> sh = shape.offset_left(prefix.size() + 1);
        if (!sh) return Err(RewriteError::exceeds(shape.exceeds(x->span)));
        RewriteResult rw = rewrite_result(ctx, meta, *sh);
        if (!rw) return std::string(snippet);
        if (x->unsafety.kind == rust::Safety::Unsafe) return std::string(prefix) + "[unsafe(" + *rw + ")]";
        return std::string(prefix) + "[" + *rw + "]";
    }
    return std::string(snippet);
}

RewriteResult rewrite_attrs(const RewriteContext& ctx, const rust::AttrVec& attrs, Shape shape) {
    return rewrite_attrs(ctx, attrs.data(), attrs.size(), shape);
}

// impl Rewrite for [ast::Attribute]
RewriteResult rewrite_attrs(const RewriteContext& ctx, const rust::Attribute* attrs, size_t n, Shape shape) {
    if (n == 0) return std::string();

    // The current remaining attributes.
    std::string result;

    // Determine if the source text is annotated with `#[rustfmt::skip::attributes(derive)]`
    // or `#![rustfmt::skip::attributes(derive)]`
    bool skip_derives = ctx.skip_context.attributes.skip("derive");

    // This is not just a simple map because we need to handle doc comments
    // (where we take as many doc comment attributes as possible) and possibly
    // merging derives into a single attribute.
    for (;;) {
        if (n == 0) return result;

        // Handle doc comments.
        Expected<std::pair<size_t, Opt>> doc = rewrite_initial_doc_comments(ctx, attrs, n, shape);
        if (!doc) return Err(doc.error());
        size_t doc_comment_len = doc->first;
        if (doc_comment_len > 0) {
            if (!doc->second) panic("doc comments, but no result");
            result += *doc->second;

            if (doc_comment_len < n) {
                Span missing_span = mk_sp(attrs[doc_comment_len - 1].span.hi, attrs[doc_comment_len].span.lo);
                std::string_view snippet = ctx.snippet(missing_span);
                auto [mla, mlb] = has_newlines_before_after_comment(snippet);
                RewriteResult comment =
                    recover_missing_comment_in_span(missing_span, shape.with_max_width(*ctx.config), ctx, 0);
                if (!comment) return comment;
                std::string c;
                if (comment->empty()) {
                    c = std::string("\n") + mlb;
                } else {
                    c = std::string(mla) + *comment + "\n" + mlb;
                }
                result += c;
                result += shape.indent.to_string(*ctx.config);
            }

            attrs += doc_comment_len;
            n -= doc_comment_len;
            continue;
        }

        // Handle derives if we will merge them.
        if (!skip_derives && ctx.config->merge_derives() && is_derive(attrs[0])) {
            size_t derives = take_while_with_pred(ctx, attrs, n, is_derive);
            Opt derive_str = format_derive(attrs, derives, shape, ctx);
            if (!derive_str) return Err(RewriteError::unknown());
            result += *derive_str;

            if (derives < n) {
                Span missing_span = mk_sp(attrs[derives - 1].span.hi, attrs[derives].span.lo);
                RewriteResult comment =
                    recover_missing_comment_in_span(missing_span, shape.with_max_width(*ctx.config), ctx, 0);
                if (!comment) return comment;
                result += *comment;
                if (attrs[derives].is_doc_comment) {
                    std::string_view snippet = ctx.snippet(missing_span);
                    result += has_newlines_before_after_comment(snippet).second;
                }
                result += '\n';
                result += shape.indent.to_string(*ctx.config);
            }

            attrs += derives;
            n -= derives;
            continue;
        }

        // If we get here, then we have a regular attribute, just handle one
        // at a time.

        RewriteResult formatted_attr = rewrite_result(ctx, &attrs[0], shape);
        if (!formatted_attr) return formatted_attr;
        result += *formatted_attr;

        if (n > 1) {
            Span missing_span = mk_sp(attrs[0].span.hi, attrs[1].span.lo);
            RewriteResult comment =
                recover_missing_comment_in_span(missing_span, shape.with_max_width(*ctx.config), ctx, 0);
            if (!comment) return comment;
            result += *comment;
            if (attrs[1].is_doc_comment) {
                std::string_view snippet = ctx.snippet(missing_span);
                result += has_newlines_before_after_comment(snippet).second;
            }
            result += '\n';
            result += shape.indent.to_string(*ctx.config);
        }

        attrs += 1;
        n -= 1;
    }
}

}  // namespace rustfmt
