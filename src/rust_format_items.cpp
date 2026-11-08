// rust_format_items.cpp -- rustfmt's items.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// mep's AST flattens rustc's ItemKind payloads into rust::Item: an AssocItem
// or ForeignItem is a rust::Item with ctx = Trait/Impl/Foreign, and FnSig /
// StaticParts / StructParts / TyAliasRewriteInfo are built from the Item.
// FmtVisitor methods of items.rs are free functions taking FmtVisitor&.
#include <algorithm>
#include <tuple>

#include "rust_format_internal.h"

namespace rustfmt {

// ---------------------------------------------------------------------------
// `?` helpers. TRY(decl, expr): expr is a RewriteResult / Expected<T>; on error
// return the error (converts to any Expected<U> return type). TRY_OPT: same
// for functions returning an optional (Option<..>).

#define RF_CAT2(a, b) a##b
#define RF_CAT(a, b) RF_CAT2(a, b)
#define TRY(decl, expr)                                                 \
    auto RF_CAT(try_, __LINE__) = (expr);                               \
    if (!RF_CAT(try_, __LINE__)) return RF_CAT(try_, __LINE__).error(); \
    decl = std::move(*RF_CAT(try_, __LINE__))
#define TRY_OPT(decl, expr)                                       \
    auto RF_CAT(tryo_, __LINE__) = (expr);                        \
    if (!RF_CAT(tryo_, __LINE__)) return std::nullopt;            \
    decl = std::move(*RF_CAT(tryo_, __LINE__))

namespace {

// config: brace_style = SameLineWhere (the default); the other values are
// kept as an enum so the branches read like rustfmt.
enum class BraceStyle : uint8_t { AlwaysNextLine, PreferSameLine, SameLineWhere };
BraceStyle brace_style(const Config&) { return BraceStyle::SameLineWhere; }
SeparatorTactic trailing_comma(const Config&) { return SeparatorTactic::Vertical; }

// The Result<Shape, ExceedsMaxWidthError> forms of the Shape methods.
Expected<Shape> sub_width(Shape s, size_t delta, Span span) {
    std::optional<Shape> r = s.sub_width_opt(delta);
    if (!r) return RewriteError::exceeds(s.width, span);
    return *r;
}
Expected<Shape> offset_left(Shape s, size_t delta, Span span) {
    std::optional<Shape> r = s.offset_left_opt(delta);
    if (!r) return RewriteError::exceeds(s.width, span);
    return *r;
}
Expected<Shape> block_left(Shape s, size_t delta, Span span) { return sub_width(s.block_indent(delta), delta, span); }

/// const DEFAULT_VISIBILITY (Inherited, DUMMY_SP).
const rust::Visibility& default_visibility() {
    static const rust::Visibility v{};
    return v;
}

size_t char_count(std::string_view s) {
    size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
    return n;
}

/// str::split_whitespace().collect::<Vec<_>>().join(" ")
std::string split_whitespace_join(std::string_view s) {
    std::string out;
    size_t i = 0;
    auto ws_len = [&](size_t at) -> size_t {
        unsigned char c = static_cast<unsigned char>(s[at]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C) return 1;
        if (c < 0x80) return 0;
        // Decode one UTF-8 char and test Unicode White_Space.
        uint32_t cp = 0;
        size_t len = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
        else return 0;
        if (at + len > s.size()) return 0;
        for (size_t k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3Fu);
        bool ws = cp == 0x85 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
                  cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
        return ws ? len : 0;
    };
    bool first = true;
    while (i < s.size()) {
        size_t w = ws_len(i);
        if (w) {
            i += w;
            continue;
        }
        size_t start = i;
        while (i < s.size() && ws_len(i) == 0) i++;
        if (!first) out += ' ';
        out.append(s.substr(start, i - start));
        first = false;
    }
    return out;
}

RwNode bounds_node(const rust::GenericBounds* b) {
    Span sp;
    if (!b->empty()) sp = b->front().span().to(b->back().span());
    return RwNode::bounds(b, sp);
}

}  // namespace

static const char* type_annotation_separator(const Config& config) { return colon_spaces(config); }

// ---------------------------------------------------------------------------
// impl Rewrite for ast::Local

static bool same_line_else_kw_and_brace(std::string_view init_str, const RewriteContext& context, Span else_kw_span,
                                        Shape init_shape);
static bool allow_single_line_let_else_block(std::string_view result, const rust::Block* block);

// Statements of the form
// let pat: ty = init; or let pat: ty = init else { .. };
RewriteResult rewrite_result(const RewriteContext& context, const rust::Local* local, Shape shape) {
    DepthGuard g(*context.state);
    const Config& config = *context.config;

    if (contains_skip(context, local->attrs)) return Err(RewriteError::skip());

    // FIXME(super_let): Implement formatting
    if (local->is_super) return Err(RewriteError::skip());

    TRY(std::string attrs_str, rewrite_attrs(context, local->attrs, shape));
    std::string result;
    if (attrs_str.empty()) {
        result = "let ";
    } else {
        TRY(result, combine_strs_with_missing_comments(context, attrs_str, "let ",
                                                       mk_sp(local->attrs.back().span.hi, local->span.lo), shape,
                                                       false));
    }
    size_t let_kw_offset = result.size() - 4;

    // 4 = "let ".len()
    TRY(Shape pat_shape0, offset_left(shape, 4, spanned(local)));
    // 1 = ;
    TRY(Shape pat_shape, sub_width(pat_shape0, 1, spanned(local)));
    TRY(std::string pat_str, rewrite_result(context, local->pat, pat_shape));

    result += pat_str;

    // String that is placed within the assignment pattern and expression.
    std::string infix;
    {
        if (local->ty) {
            std::string_view separator = type_annotation_separator(config);
            Shape base = contains(pat_str, '\n') ? shape.with_max_width(config) : shape;
            TRY(Shape ty_shape0, offset_left(base, last_line_width(result) + separator.size(), spanned(local)));
            // 2 = ` =`
            TRY(Shape ty_shape, sub_width(ty_shape0, 2, spanned(local)));

            TRY(std::string rewrite, rewrite_result(context, local->ty, ty_shape));

            infix += separator;
            infix += rewrite;
        }

        if (local->kind != rust::LocalKind::Decl) infix += " =";
    }

    result += infix;

    if (local->kind != rust::LocalKind::Decl && local->init) {
        const rust::Expr* init = local->init;
        const rust::Block* else_block = local->kind == rust::LocalKind::InitElse ? local->els : nullptr;
        // 1 = trailing semicolon;
        TRY(Shape nested_shape, sub_width(shape, 1, spanned(local)));

        TRY(result, rewrite_assign_rhs(context, result, RwNode::of(init), RhsAssignKind::of_expr(init), nested_shape));

        if (else_block) {
            const rust::Block* block = else_block;
            Span else_kw_span = init->span.between(block->span);
            // Strip attributes and comments to check if newline is needed before the else
            // keyword from the initializer part. (#5901)
            StyleEdition style_edition = config.style_edition();
            std::string_view init_str = style_edition >= StyleEdition::E2024
                                            ? std::string_view(result).substr(let_kw_offset)
                                            : std::string_view(result);
            bool force_newline_else =
                contains(pat_str, '\n') || !same_line_else_kw_and_brace(init_str, context, else_kw_span, nested_shape);
            std::string else_kw = rewrite_else_kw_with_comments(force_newline_else, true, context, else_kw_span, shape);
            result += else_kw;

            // At this point we've written `let {pat} = {expr} else' into the buffer, and we
            // want to calculate up front if there's room to write the divergent block on the
            // same line. The available space varies based on indentation so we clamp the width
            // on the smaller of `shape.width` and `single_line_let_else_max_width`.
            size_t max_width = std::min(shape.width, config.single_line_let_else_max_width());

            // If available_space hits zero we know for sure this will be a multi-lined block
            std::string_view assign_str_with_else_kw = style_edition >= StyleEdition::E2024
                                                           ? std::string_view(result).substr(let_kw_offset)
                                                           : std::string_view(result);
            size_t available_space = sat_sub(max_width, assign_str_with_else_kw.size());

            bool allow_single_line = !force_newline_else && available_space > 0 &&
                                     allow_single_line_let_else_block(assign_str_with_else_kw, block);

            TRY(std::string rw_else_block, rewrite_let_else_block(block, allow_single_line, context, shape));

            bool single_line_else = !contains(rw_else_block, '\n');
            // +1 for the trailing `;`
            bool else_block_exceeds_width = rw_else_block.size() + 1 > available_space;

            if (allow_single_line && single_line_else && else_block_exceeds_width) {
                // writing this on one line would exceed the available width
                // so rewrite the else block over multiple lines.
                TRY(rw_else_block, rewrite_let_else_block(block, false, context, shape));
            }

            result += rw_else_block;
        }
    }

    result += ';';
    return result;
}

/// When the initializer expression is multi-lined, then the else keyword and opening brace of the
/// block ( i.e. "else {") should be put on the same line as the end of the initializer expression
/// if all the following are true:
///
/// 1. The initializer expression ends with one or more closing parentheses, square brackets,
///    or braces
/// 2. There is nothing else on that line
/// 3. That line is not indented beyond the indent on the first line of the let keyword
static bool same_line_else_kw_and_brace(std::string_view init_str, const RewriteContext& context, Span else_kw_span,
                                        Shape init_shape) {
    if (!contains(init_str, '\n')) {
        // initializer expression is single lined. The "else {" can only be placed on the same line
        // as the initializer expression if there is enough room for it.
        // 7 = ` else {`
        return sat_sub(init_shape.width, init_str.size()) >= 7;
    }

    // 1. The initializer expression ends with one or more `)`, `]`, `}`.
    if (init_str.empty()) return false;
    char last = init_str.back();
    if (last != ')' && last != ']' && last != '}') return false;

    // 2. There is nothing else on that line
    // For example, there are no comments
    std::string_view else_kw_snippet = trim(context.snippet(else_kw_span));
    if (else_kw_snippet != "else") return false;

    // 3. The last line of the initializer expression is not indented beyond the `let` keyword
    std::string indent = init_shape.indent.to_string(*context.config);
    std::vector<std::string_view> ls = lines(init_str);
    if (ls.empty()) panic("initializer expression is multi-lined");
    std::string_view l = ls.back();
    if (!starts_with(l, indent)) return false;
    l = l.substr(indent.size());
    // !l.starts_with(char::is_whitespace)
    return trim_start(l).size() == l.size();
}

static bool allow_single_line_let_else_block(std::string_view result, const rust::Block* block) {
    if (contains(result, '\n')) return false;
    if (block->stmts.size() <= 1) return true;
    return false;
}

// ---------------------------------------------------------------------------
// FnSig

namespace {

/// Represents a fn's signature.
struct FnSig {
    const rust::FnDecl* decl = nullptr;
    const rust::Generics* generics = nullptr;
    rust::Extern ext;
    rust::CoroutineKind coroutine_kind;
    rust::Constness constness;
    rust::Defaultness defaultness;
    rust::Safety safety;
    const rust::Visibility* visibility = nullptr;

    /// FnSig::from_method_sig / FnSig::from_fn_kind (identical for mep's flattened Item).
    static FnSig from_item(const rust::Item* fn_item) {
        FnSig s;
        s.safety = fn_item->sig.header.safety;
        s.coroutine_kind = fn_item->sig.header.coroutine_kind;
        s.constness = fn_item->sig.header.constness;
        s.defaultness = fn_item->defaultness;
        s.ext = fn_item->sig.header.ext;
        s.decl = fn_item->sig.decl;
        s.generics = &fn_item->generics;
        s.visibility = &fn_item->vis;
        return s;
    }

    std::string to_str(const RewriteContext& context) const {
        std::string result;
        // Vis defaultness constness unsafety abi.
        result += format_visibility(context, *visibility);
        result += format_defaultness(defaultness);
        result += format_constness(constness);
        result += format_coro(coroutine_kind);
        result += format_safety(safety);
        result += format_extern(context, ext, context.config->force_explicit_abi());
        return result;
    }
};

/// Kind of spaces to put before `where`.
enum class WhereClauseSpace : uint8_t {
    /// A single space.
    Space,
    /// A new line.
    Newline,
    /// Nothing.
    None,
};

struct WhereClauseOption {
    bool suppress_comma_ = false;  // Force no trailing comma
    WhereClauseSpace snuggle_ = WhereClauseSpace::Newline;
    bool allow_single_line_ = false;  // Try single line where-clause instead of vertical layout
    bool veto_single_line_ = false;   // Disallow a single-line where-clause.

    static WhereClauseOption new_(bool suppress_comma, WhereClauseSpace snuggle) {
        WhereClauseOption o;
        o.suppress_comma_ = suppress_comma;
        o.snuggle_ = snuggle;
        return o;
    }
    static WhereClauseOption snuggled(std::string_view current) {
        WhereClauseOption o;
        o.snuggle_ = last_line_width(current) == 1 ? WhereClauseSpace::Space : WhereClauseSpace::Newline;
        return o;
    }
    void suppress_comma() { suppress_comma_ = true; }
    void allow_single_line() { allow_single_line_ = true; }
    void snuggle() { snuggle_ = WhereClauseSpace::Space; }
    void veto_single_line() { veto_single_line_ = true; }
};

enum class BracePos : uint8_t { None, Auto, ForceSameLine };

struct StructParts {
    std::string_view prefix;
    rust::Ident ident;
    const rust::Visibility* vis = nullptr;
    const rust::VariantData* def = nullptr;
    const rust::Generics* generics = nullptr;  // Option
    Span span;
};

struct StaticParts {
    std::string_view prefix;
    rust::Safety safety;
    const rust::Visibility* vis = nullptr;
    rust::Ident ident;
    const rust::Generics* generics = nullptr;  // Option
    const rust::Ty* ty = nullptr;
    rust::Mutability mutability = rust::Mutability::Not;
    const rust::Expr* expr_opt = nullptr;
    std::optional<rust::Defaultness> defaultness;
    Span span;
};

struct TyAliasRewriteInfo {
    const RewriteContext* context;
    Indent indent;
    const rust::Generics* generics;
    const rust::WhereClause* after_where_clause;
    rust::Ident ident;
    Span span;
};

}  // namespace

static RewriteResult rewrite_where_clause(const RewriteContext& context, const rust::WhereClause& where_clause,
                                          BraceStyle brace_style, Shape shape, bool on_new_line,
                                          std::string_view terminator, std::optional<uint32_t> span_end,
                                          uint32_t span_end_before_where, WhereClauseOption where_clause_option);
static RewriteResult rewrite_generics(const RewriteContext& context, std::string_view ident,
                                      const rust::Generics& generics, Shape shape);
static Opt format_generics(const RewriteContext& context, const rust::Generics& generics, BraceStyle brace_style,
                           BracePos brace_pos, Indent offset, Span span, size_t used_width);
static std::string format_header(const RewriteContext& context, std::string_view item_name, rust::Ident ident,
                                 const rust::Visibility& vis, Indent offset);
static Expected<std::tuple<std::string, bool, bool>> rewrite_fn_base(const RewriteContext& context, Indent indent,
                                                                     rust::Ident ident, const FnSig& fn_sig,
                                                                     Span span, FnBraceStyle fn_brace_style);
static FnBraceStyle newline_for_brace(const Config& config, const rust::WhereClause& where_clause);
static Opt format_struct(const RewriteContext& context, const StructParts& struct_parts, Indent offset,
                         std::optional<size_t> one_line_width);
static void format_empty_struct_or_tuple(const RewriteContext& context, Span span, Indent offset,
                                         std::string& result, std::string_view opener, std::string_view closer);
static RewriteResult rewrite_foreign_item(const RewriteContext& context, const rust::Item* item, Shape shape);

// ---------------------------------------------------------------------------
// FmtVisitor methods (items.rs `impl FmtVisitor`)

namespace {
// items.rs's private `Item` (only built from a foreign mod).
struct ForeignModItem {
    rust::Safety safety;
    std::string abi;
    const std::vector<rust::Item*>* body = nullptr;
    Span span;
};
}  // namespace

// FmtVisitor::format_item
static void format_item(FmtVisitor& v, const ForeignModItem& item) {
    v.buffer += format_safety(item.safety);
    v.buffer += item.abi;

    std::string_view snippet = v.snippet(item.span);
    std::optional<size_t> bp = find_uncommented(snippet, "{");
    if (!bp) panic("format_item: no `{`", item.span.lo);
    size_t brace_pos = *bp;

    v.push_str("{");
    if (!item.body->empty() || contains_comment(snippet.substr(brace_pos))) {
        // FIXME: this skips comments between the extern keyword and the opening
        // brace.
        v.last_pos = item.span.lo + static_cast<uint32_t>(brace_pos) + 1;
        v.block_indent = v.block_indent.block_indent_(*v.config);

        if (!item.body->empty()) {
            // format_body_element
            for (const rust::Item* it : *item.body) v.visit_foreign_item(it);
        }

        v.format_missing_no_indent(item.span.hi - 1);
        v.block_indent = v.block_indent.block_unindent(*v.config);
        std::string indent_str = v.block_indent.to_string(*v.config);
        v.push_str(indent_str);
    }

    v.push_str("}");
    v.last_pos = item.span.hi;
}

void format_foreign_mod(FmtVisitor& v, const rust::Item* item) {
    // Item::from_foreign_mod
    ForeignModItem fm;
    fm.safety = item->safety;
    rust::Extern ext;  // ast::Extern::from_abi(fm.abi, DUMMY_SP)
    if (item->has_abi) {
        ext.kind = rust::Extern::Explicit;
        ext.abi = item->abi;
    } else {
        ext.kind = rust::Extern::Implicit;
    }
    RewriteContext ctx = v.get_context();
    fm.abi = format_extern(ctx, ext, v.config->force_explicit_abi());
    fm.body = &item->items;
    fm.span = item->span;
    format_item(v, fm);
}

// FmtVisitor::format_foreign_item
void FmtVisitor::visit_foreign_item(const rust::Item* item) {
    Opt rewrite = rewrite_foreign_item(get_context(), item, shape()).opt();
    uint32_t hi = item->span.hi;
    Span span = item->attrs.empty() ? item->span : mk_sp(item->attrs[0].span.lo, hi);
    push_rewrite(span, rewrite);
    last_pos = hi;
}

std::optional<std::pair<std::string, FnBraceStyle>> rewrite_fn_before_block(FmtVisitor& v, Indent indent,
                                                                            const rust::Item* fn_item, Span span) {
    RewriteContext context = v.get_context();
    FnSig fn_sig = FnSig::from_item(fn_item);

    FnBraceStyle fn_brace_style = newline_for_brace(*v.config, fn_sig.generics->where_clause);
    auto r = rewrite_fn_base(context, indent, fn_item->ident, fn_sig, span, fn_brace_style);
    if (!r) return std::nullopt;
    std::string result = std::get<0>(*r);
    bool force_newline_brace = std::get<2>(*r);

    // 2 = ` {`
    if (brace_style(*v.config) == BraceStyle::AlwaysNextLine || force_newline_brace ||
        last_line_width(result) + 2 > v.shape().width) {
        fn_brace_style = FnBraceStyle::NextLine;
    }

    return std::make_pair(std::move(result), fn_brace_style);
}

RewriteResult rewrite_required_fn(FmtVisitor& v, Indent indent, const rust::Item* fn_item, Span span_in) {
    // Drop semicolon or it will be interpreted as comment.
    Span span = mk_sp(span_in.lo, span_in.hi - 1);
    RewriteContext context = v.get_context();

    TRY(auto base, rewrite_fn_base(context, indent, fn_item->ident, FnSig::from_item(fn_item), span,
                                   FnBraceStyle::None));
    std::string result = std::move(std::get<0>(base));
    bool ends_with_comment = std::get<1>(base);

    // If `result` ends with a comment, then remember to add a newline
    if (ends_with_comment) result += indent.to_string_with_newline(*context.config);

    // Re-attach semicolon
    result += ';';

    return result;
}

Opt single_line_fn(const FmtVisitor& v, std::string_view fn_str, const rust::Block* block,
                   const rust::AttrVec* inner_attrs) {
    if (contains(fn_str, '\n') || (inner_attrs && !inner_attrs->empty())) return std::nullopt;

    RewriteContext context = v.get_context();

    if (v.config->empty_item_single_line() && is_empty_block(context, block, nullptr) &&
        v.block_indent.width() + fn_str.size() + 3 <= v.config->max_width() &&
        !last_line_contains_single_line_comment(fn_str)) {
        return std::string(fn_str) + " {}";
    }

    // non-default config: fn_single_line (false: the one-statement body form is never used).
    return std::nullopt;
}

static Opt rewrite_static(const RewriteContext& context, const StaticParts& static_parts, Indent offset);

void visit_static(FmtVisitor& v, const rust::Item* item) {
    // StaticParts::from_item / from_trait_item / from_impl_item
    StaticParts sp;
    if (item->kind == rust::ItemKind::Static) {
        sp.prefix = "static";
        sp.safety = item->safety;
        sp.mutability = item->mutbl;
        sp.generics = nullptr;
    } else if (item->kind == rust::ItemKind::Const) {
        sp.defaultness = item->defaultness;
        sp.prefix = item->type_const ? "type const" : "const";
        sp.safety = rust::Safety{};
        sp.mutability = rust::Mutability::Not;
        sp.generics = &item->generics;
    } else {
        panic("visit_static: not a static or const item", item->span.lo);
    }
    sp.vis = &item->vis;
    sp.ident = item->ident;
    sp.ty = item->ty;
    sp.expr_opt = item->expr;
    sp.span = item->span;

    Opt rewrite = rewrite_static(v.get_context(), sp, v.block_indent);
    v.push_rewrite(sp.span, rewrite);
}

// StructParts::from_item
static StructParts struct_parts_from_item(const rust::Item* item) {
    StructParts p;
    if (item->kind == rust::ItemKind::Struct) p.prefix = "struct ";
    else if (item->kind == rust::ItemKind::Union) p.prefix = "union ";
    else panic("StructParts::from_item: not a struct or union", item->span.lo);
    p.ident = item->ident;
    p.vis = &item->vis;
    p.def = &item->data;
    p.generics = &item->generics;
    p.span = item->span;
    return p;
}

void visit_struct(FmtVisitor& v, const rust::Item* item) {
    StructParts struct_parts = struct_parts_from_item(item);
    bool is_tuple = struct_parts.def->kind == rust::VariantData::Tuple;
    Opt rewrite = format_struct(v.get_context(), struct_parts, v.block_indent, std::nullopt);
    if (rewrite && is_tuple) *rewrite += ";";
    v.push_rewrite(struct_parts.span, rewrite);
}

static Opt format_variant_list(FmtVisitor& v, const rust::Item* enum_item, uint32_t body_lo, uint32_t body_hi);

void visit_enum(FmtVisitor& v, const rust::Item* item) {
    rust::Ident ident = item->ident;
    const rust::Generics& generics = item->generics;
    Span span = item->span;
    const std::vector<rust::Variant*>& variants = item->variants;

    std::string enum_header = format_header(v.get_context(), "enum ", ident, item->vis, v.block_indent);
    v.push_str(enum_header);

    std::string_view enum_snippet = v.snippet(span);
    std::optional<size_t> bp = find_uncommented(enum_snippet, "{");
    if (!bp) panic("visit_enum: no `{`", span.lo);
    uint32_t body_start = span.lo + static_cast<uint32_t>(*bp) + 1;
    Opt generics_str = format_generics(v.get_context(), generics, brace_style(*v.config),
                                       variants.empty() ? BracePos::ForceSameLine : BracePos::Auto, v.block_indent,
                                       // make a span that starts right after `enum Foo`
                                       mk_sp(ident.span.hi, body_start), last_line_width(enum_header));
    if (!generics_str) panic("visit_enum: format_generics failed", span.lo);
    v.push_str(*generics_str);

    v.last_pos = body_start;

    Opt rw = format_variant_list(v, item, body_start, span.hi);
    if (rw && variants.empty()) {
        v.push_str(*rw);
    } else {
        v.push_rewrite(mk_sp(body_start, span.hi), rw);
        v.block_indent = v.block_indent.block_unindent(*v.config);
    }
}

static Opt format_variant(const FmtVisitor& v, const rust::Variant* field, size_t one_line_width,
                          size_t pad_discrim_ident_to);

// Format the body of an enum definition
static Opt format_variant_list(FmtVisitor& v, const rust::Item* enum_item, uint32_t body_lo, uint32_t body_hi) {
    const std::vector<rust::Variant*>& variants = enum_item->variants;
    if (variants.empty()) {
        std::string buffer;
        // 1 = "}"
        Span span = mk_sp(body_lo, body_hi - 1);
        format_empty_struct_or_tuple(v.get_context(), span, v.block_indent, buffer, "", "}");
        return buffer;
    }
    std::string result;
    Indent original_offset = v.block_indent;
    v.block_indent = v.block_indent.block_indent_(*v.config);

    // If enum variants have discriminants, try to vertically align those,
    // provided the discrims are not shifted too much  to the right
    size_t align_threshold = v.config->enum_discrim_align_threshold();
    std::vector<size_t> discr_ident_lens;
    {
        RewriteContext ctx = v.get_context();
        for (const rust::Variant* var : variants)
            if (var->disr_expr) discr_ident_lens.push_back(rewrite_ident(ctx, var->ident).size());
    }
    // cut the list at the point of longest discrim shorter than the threshold
    // All of the discrims under the threshold will get padded, and all above - left as is.
    size_t pad_discrim_ident_to = 0;
    for (size_t l : discr_ident_lens)
        if (l <= align_threshold) pad_discrim_ident_to = std::max(pad_discrim_ident_to, l);

    auto itemize_list_with = [&](size_t one_line_width) {
        return itemize_list(
            *v.snippet_provider, variants.size(), "}", ",",
            [&](size_t i) -> uint32_t {
                const rust::Variant* f = variants[i];
                return !f->attrs.empty() ? f->attrs[0].span.lo : f->span.lo;
            },
            [&](size_t i) -> uint32_t { return variants[i]->span.hi; },
            [&](size_t i) -> RewriteResult {
                return unknown_error(format_variant(v, variants[i], one_line_width, pad_discrim_ident_to));
            },
            body_lo, body_hi, false);
    };
    std::vector<ListItem> items = itemize_list_with(v.config->struct_variant_width());

    // If one of the variants use multiple lines, use multi-lined formatting for all variants.
    bool has_multiline_variant = false, has_single_line_variant = false;
    for (const ListItem& item : items) {
        if (contains(item.inner_as_ref(), '\n')) has_multiline_variant = true;
        else has_single_line_variant = true;
    }
    if (has_multiline_variant && has_single_line_variant) items = itemize_list_with(0);

    TRY_OPT(Shape shape, v.shape().sub_width_opt(2));
    ListFormatting fmt = ListFormatting(shape, *v.config).trailing_separator(trailing_comma(*v.config)).preserve_newline(true);

    TRY_OPT(std::string list, write_list(items, fmt));
    result += list;
    result += original_offset.to_string_with_newline(*v.config);
    result += '}';
    return result;
}

static Span enum_variant_span(const rust::Variant* variant, const RewriteContext& context);

// Variant of an enum.
static Opt format_variant(const FmtVisitor& v, const rust::Variant* field, size_t one_line_width,
                          size_t pad_discrim_ident_to) {
    if (contains_skip(v.get_context(), field->attrs)) {
        uint32_t lo = field->attrs[0].span.lo;
        Span span = mk_sp(lo, field->span.hi);
        return std::string(v.snippet(span));
    }

    RewriteContext context = v.get_context();
    Shape shape = v.shape();
    std::string attrs_str;
    if (context.config->style_edition() >= StyleEdition::E2024) {
        TRY_OPT(attrs_str, rewrite_attrs(context, field->attrs, shape));
    } else {
        // StyleEdition::Edition20{15|18|21} formatting that was off by 1. See issue #5801
        TRY_OPT(Shape s1, shape.sub_width_opt(1));
        TRY_OPT(attrs_str, rewrite_attrs(context, field->attrs, s1));
    }
    // sub_width(1) to take the trailing comma into account
    TRY_OPT(shape, shape.sub_width_opt(1));

    uint32_t lo = field->attrs.empty() ? field->span.lo : field->attrs.back().span.hi;
    Span span = mk_sp(lo, field->span.lo);

    std::string variant_body;
    switch (field->data.kind) {
    case rust::VariantData::Tuple:
    case rust::VariantData::Struct: {
        // StructParts::from_variant
        StructParts sp;
        sp.prefix = "";
        sp.ident = field->ident;
        sp.vis = &default_visibility();
        sp.def = &field->data;
        sp.generics = nullptr;
        sp.span = enum_variant_span(field, context);
        TRY_OPT(variant_body, format_struct(context, sp, v.block_indent, one_line_width));
        break;
    }
    case rust::VariantData::Unit: variant_body = std::string(rewrite_ident(context, field->ident)); break;
    }

    if (field->disr_expr) {
        // format!("{variant_body:pad_discrim_ident_to$} =")
        std::string lhs = variant_body;
        size_t n = char_count(lhs);
        if (n < pad_discrim_ident_to) lhs.append(pad_discrim_ident_to - n, ' ');
        lhs += " =";
        const rust::Expr* ex = field->disr_expr->value;
        TRY_OPT(variant_body, rewrite_assign_rhs_with(context, lhs, RwNode::of(ex), shape, RhsAssignKind::of_expr(ex),
                                                      RhsTactics::AllowOverflow));
    }

    return combine_strs_with_missing_comments(context, attrs_str, variant_body, span, shape, false).opt();
}

// FmtVisitor::visit_impl_items
static void visit_impl_items(FmtVisitor& v, const std::vector<rust::Item*>& items) {
    // non-default config: reorder_impl_items (false: items keep their order).
    for (const rust::Item* item : items) v.visit_impl_item(item);
}

// ---------------------------------------------------------------------------
// impl

static RewriteResult format_impl_ref_and_type(const RewriteContext& context, const rust::Item* item, Indent offset);
static Expected<bool> is_impl_single_line(const RewriteContext& context, const std::vector<rust::Item*>& items,
                                          std::string_view result, std::string_view where_clause_str,
                                          const rust::Item* item);

RewriteResult format_impl(const RewriteContext& context, const rust::Item* item, Indent offset) {
    DepthGuard g(*context.state);
    const Config& config = *context.config;
    const rust::Generics& generics = item->generics;
    const rust::Ty* self_ty = item->ty;
    const std::vector<rust::Item*>& items = item->items;

    std::string result;
    TRY(std::string ref_and_type, format_impl_ref_and_type(context, item, offset));
    std::string sep = offset.to_string_with_newline(config);
    result += ref_and_type;

    size_t where_budget = contains(result, '\n') ? config.max_width() : context.budget(last_line_width(result));

    WhereClauseOption option = WhereClauseOption::snuggled(ref_and_type);
    std::string_view snippet = context.snippet(item->span);
    std::optional<size_t> op = find_uncommented(snippet, "{");
    if (!op) return Err(RewriteError::unknown());
    size_t open_pos = *op + 1;
    if (!contains_comment(snippet.substr(open_pos)) && items.empty() &&
        generics.where_clause.predicates.size() == 1 && !contains(result, '\n')) {
        option.suppress_comma();
        option.snuggle();
        option.allow_single_line();
    }

    Span missing_span = mk_sp(self_ty->span.hi, item->span.hi);
    std::optional<uint32_t> where_span_end = context.snippet_provider->opt_span_before(missing_span, "{");
    TRY(std::string where_clause_str,
        rewrite_where_clause(context, generics.where_clause, brace_style(config),
                             Shape::legacy(where_budget, offset.block_only()), false, "{", where_span_end,
                             self_ty->span.hi, option));

    // If there is no where-clause, we may have missing comments between the trait name and
    // the opening brace.
    if (generics.where_clause.predicates.empty()) {
        if (where_span_end) {
            RewriteResult mc = recover_missing_comment_in_span(mk_sp(self_ty->span.hi, *where_span_end),
                                                               Shape::indented(offset, config), context,
                                                               last_line_width(result));
            if (mc && !mc->empty()) result += *mc;
        }
    }

    TRY(bool single_line, is_impl_single_line(context, items, result, where_clause_str, item));
    if (single_line) {
        result += where_clause_str;
        if (contains(where_clause_str, '\n')) {
            // If there is only one where-clause predicate
            // and the where-clause spans multiple lines,
            // then recover the suppressed comma in single line where-clause formatting
            if (generics.where_clause.predicates.size() == 1) result += ',';
        }
        if (contains(where_clause_str, '\n') || last_line_contains_single_line_comment(result)) {
            result += sep + "{" + sep + "}";
        } else {
            result += " {}";
        }
        return result;
    }

    result += where_clause_str;

    bool need_newline = last_line_contains_single_line_comment(result) || contains(result, '\n');
    BraceStyle bs = brace_style(config);
    if (need_newline) {
        result += sep;
    } else if (bs == BraceStyle::AlwaysNextLine) {
        result += sep;
    } else if (bs == BraceStyle::PreferSameLine) {
        result += ' ';
    } else {  // SameLineWhere
        if (!where_clause_str.empty()) result += sep;
        else result += ' ';
    }

    result += '{';
    // this is an impl body snippet(impl SampleImpl { /* here */ })
    uint32_t lo = std::max(self_ty->span.hi, generics.where_clause.span.hi);
    std::string_view snippet2 = context.snippet(mk_sp(lo, item->span.hi));
    std::optional<size_t> op2 = find_uncommented(snippet2, "{");
    if (!op2) return Err(RewriteError::unknown());
    size_t open_pos2 = *op2 + 1;
    if (!items.empty() || contains_comment(snippet2.substr(open_pos2))) {
        FmtVisitor visitor = FmtVisitor::from_context(context);
        Indent item_indent = offset.block_only().block_indent_(config);
        visitor.block_indent = item_indent;
        visitor.last_pos = lo + static_cast<uint32_t>(open_pos2);

        (void)visitor.visit_attrs(item->attrs, rust::AttrStyle::Inner);
        visit_impl_items(visitor, items);

        visitor.format_missing(item->span.hi - 1);

        std::string inner_indent_str = visitor.block_indent.to_string_with_newline(config);
        std::string outer_indent_str = offset.block_only().to_string_with_newline(config);

        result += inner_indent_str;
        result += trim(visitor.buffer);
        result += outer_indent_str;
    } else if (need_newline || !config.empty_item_single_line()) {
        result += sep;
    }

    result += '}';

    return result;
}

static Expected<bool> is_impl_single_line(const RewriteContext& context, const std::vector<rust::Item*>& items,
                                          std::string_view result, std::string_view where_clause_str,
                                          const rust::Item* item) {
    std::string_view snippet = context.snippet(item->span);
    std::optional<size_t> op = find_uncommented(snippet, "{");
    if (!op) return RewriteError::unknown();
    size_t open_pos = *op + 1;

    return context.config->empty_item_single_line() && items.empty() && !contains(result, '\n') &&
           result.size() + where_clause_str.size() <= context.config->max_width() &&
           !contains_comment(snippet.substr(open_pos));
}

static Expected<Shape> generics_shape_from_config(const Config& config, Shape shape, size_t offset, Span span);
static RewriteResult rewrite_trait_ref(const RewriteContext& context, const rust::Path* trait_ref, Indent offset,
                                       std::string_view polarity_str, size_t result_len);

static RewriteResult format_impl_ref_and_type(const RewriteContext& context, const rust::Item* item, Indent offset) {
    const Config& config = *context.config;
    const rust::Generics& generics = item->generics;
    bool of_trait = item->of_trait;
    const rust::Ty* self_ty = item->ty;
    rust::Constness constness = item->constness;
    std::string result;

    result += format_visibility(context, item->vis);

    if (of_trait) {
        result += format_defaultness(item->defaultness);
        result += format_safety(item->safety);
    } else {
        result += format_constness(constness);
    }

    Shape shape;
    if (config.style_edition() >= StyleEdition::E2024) {
        shape = Shape::indented(offset + last_line_width(result), config);
    } else {
        TRY(shape, generics_shape_from_config(config, Shape::indented(offset + last_line_width(result), config), 0,
                                              item->span));
    }
    TRY(std::string generics_str, rewrite_generics(context, "impl", generics, shape));
    result += generics_str;

    size_t trait_ref_overhead;
    if (of_trait) {
        result += format_constness_right(constness);
        std::string_view polarity_str = item->negative ? "!" : "";
        size_t result_len = last_line_width(result);
        TRY(std::string tr, rewrite_trait_ref(context, item->trait_ref, offset, polarity_str, result_len));
        result += tr;
        trait_ref_overhead = 4;  // " for".len()
    } else {
        trait_ref_overhead = 0;
    }

    // Try to put the self type in a single line.
    size_t curly_brace_overhead;
    if (generics.where_clause.predicates.empty()) {
        // If there is no where-clause adapt budget for type formatting to take space and curly
        // brace into account.
        curly_brace_overhead = brace_style(config) == BraceStyle::AlwaysNextLine ? 0 : 2;
    } else {
        curly_brace_overhead = 0;
    }
    size_t used_space = last_line_width(result) + trait_ref_overhead + curly_brace_overhead;
    // 1 = space before the type.
    size_t budget = context.budget(used_space + 1);
    Opt self_ty_str = rewrite_result(context, self_ty, Shape::legacy(budget, offset)).opt();
    if (self_ty_str) {
        if (!contains(*self_ty_str, '\n')) {
            if (of_trait) result += " for ";
            else result += ' ';
            result += *self_ty_str;
            return result;
        }
    }

    // Couldn't fit the self type on a single line, put it on a new line.
    result += '\n';
    // Add indentation of one additional tab.
    Indent new_line_offset = offset.block_indent_(config);
    result += new_line_offset.to_string(config);
    if (of_trait) result += "for ";
    size_t budget2 = context.budget(last_line_width(result));
    // non-default config: indent_style = Visual (type_offset = new_line_offset + trait_ref_overhead)
    Indent type_offset = new_line_offset;
    TRY(std::string ty_str, rewrite_result(context, self_ty, Shape::legacy(budget2, type_offset)));
    result += ty_str;
    return result;
}

// items.rs's rewrite_trait_ref (the 5-argument one; types.rs's is impl Rewrite for TraitRef).
static RewriteResult rewrite_trait_ref(const RewriteContext& context, const rust::Path* trait_ref, Indent offset,
                                       std::string_view polarity_str, size_t result_len) {
    const Config& config = *context.config;
    // 1 = space between generics and trait_ref
    size_t used_space = 1 + polarity_str.size() + result_len;
    Shape shape = Shape::indented(offset + used_space, config);
    RewriteResult trait_ref_str = rewrite_trait_ref(context, trait_ref, shape);
    if (trait_ref_str) {
        if (!contains(*trait_ref_str, '\n')) return " " + std::string(polarity_str) + *trait_ref_str;
    }
    // We could not make enough space for trait_ref, so put it on new line.
    Indent offset2 = offset.block_indent_(config);
    Shape shape2 = Shape::indented(offset2, config);
    TRY(std::string trait_ref_str2, rewrite_trait_ref(context, trait_ref, shape2));
    return offset2.to_string_with_newline(config) + std::string(polarity_str) + trait_ref_str2;
}

static Span enum_variant_span(const rust::Variant* variant, const RewriteContext& context) {
    if (variant->disr_expr) {
        Span span_before_consts = variant->span.until(variant->disr_expr->value->span);
        uint32_t hi = 0;
        switch (variant->data.kind) {
        case rust::VariantData::Struct: hi = context.snippet_provider->span_after_last(span_before_consts, "}"); break;
        case rust::VariantData::Tuple: hi = context.snippet_provider->span_after_last(span_before_consts, ")"); break;
        case rust::VariantData::Unit: hi = variant->ident.span.hi; break;
        }
        return mk_sp(span_before_consts.lo, hi);
    }
    return variant->span;
}

static Opt format_unit_struct(const RewriteContext& context, const StructParts& p, Indent offset);
static Opt format_tuple_struct(const RewriteContext& context, const StructParts& struct_parts,
                               const std::vector<rust::FieldDef*>& fields, Indent offset);
static Opt format_struct_struct(const RewriteContext& context, const StructParts& struct_parts,
                                const std::vector<rust::FieldDef*>& fields, Indent offset,
                                std::optional<size_t> one_line_width);

static Opt format_struct(const RewriteContext& context, const StructParts& struct_parts, Indent offset,
                         std::optional<size_t> one_line_width) {
    switch (struct_parts.def->kind) {
    case rust::VariantData::Unit: return format_unit_struct(context, struct_parts, offset);
    case rust::VariantData::Tuple: return format_tuple_struct(context, struct_parts, struct_parts.def->fields, offset);
    case rust::VariantData::Struct:
        return format_struct_struct(context, struct_parts, struct_parts.def->fields, offset, one_line_width);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// trait

RewriteResult format_trait(const RewriteContext& context, const rust::Item* item, Indent offset) {
    DepthGuard g(*context.state);
    const Config& config = *context.config;
    rust::Ident ident = item->ident;
    const rust::Generics& generics = item->generics;
    const rust::GenericBounds& bounds = item->bounds;
    const std::vector<rust::Item*>& items = item->items;

    std::string result;
    std::string header = format_visibility(context, item->vis) +
                         format_impl_restriction(context, item->impl_restriction) +
                         format_constness(item->constness) + format_safety(item->safety) + format_auto(item->is_auto) +
                         "trait ";
    result += header;

    uint32_t body_lo = context.snippet_provider->span_after(item->span, "{");

    TRY(Shape shape, offset_left(Shape::indented(offset, config), result.size(), item->span));
    TRY(std::string generics_str, rewrite_generics(context, rewrite_ident(context, ident), generics, shape));
    result += generics_str;

    // FIXME(#2055): rustfmt fails to format when there are comments between trait bounds.
    if (!bounds.empty()) {
        // Retrieve *unnormalized* ident (See #6069)
        std::string_view source_ident = context.snippet(ident.span);
        uint32_t ident_hi = context.snippet_provider->span_after(item->span, source_ident);
        uint32_t bound_hi = bounds.back().span().hi;
        std::string_view snippet = context.snippet(mk_sp(ident_hi, bound_hi));
        if (contains_comment(snippet)) return Err(RewriteError::unknown());

        TRY(result, rewrite_assign_rhs_with(context, result + ":", bounds_node(&bounds), shape,
                                            RhsAssignKind::bounds(), RhsTactics::ForceNextLineWithoutIndent));
    }

    // Rewrite where-clause.
    if (!generics.where_clause.predicates.empty()) {
        bool where_on_new_line = config.indent_style() != IndentStyle::Block;

        size_t where_budget = context.budget(last_line_width(result));
        uint32_t pos_before_where = bounds.empty() ? generics.where_clause.span.lo : bounds.back().span().hi;
        WhereClauseOption option = WhereClauseOption::snuggled(generics_str);
        TRY(std::string where_clause_str,
            rewrite_where_clause(context, generics.where_clause, brace_style(config),
                                 Shape::legacy(where_budget, offset.block_only()), where_on_new_line, "{",
                                 std::nullopt, pos_before_where, option));

        // If the where-clause cannot fit on the same line,
        // put the where-clause on a new line
        if (!contains(where_clause_str, '\n') &&
            last_line_width(result) + where_clause_str.size() + offset.width() > config.comment_width()) {
            size_t width = offset.block_indent + config.tab_spaces() - 1;
            Indent where_indent(0, width);
            result += where_indent.to_string_with_newline(config);
        }
        result += where_clause_str;
    } else {
        std::string_view item_snippet = context.snippet(item->span);
        size_t lo = item_snippet.find('/');
        if (lo != std::string_view::npos) {
            // 1 = `{`
            uint32_t comment_hi = !generics.params.empty() ? generics.span.lo - 1 : body_lo - 1;
            uint32_t comment_lo = item->span.lo + static_cast<uint32_t>(lo);
            if (comment_lo < comment_hi) {
                RewriteResult mc = recover_missing_comment_in_span(mk_sp(comment_lo, comment_hi),
                                                                   Shape::indented(offset, config), context,
                                                                   last_line_width(result));
                if (mc && !mc->empty()) result += *mc;
            }
        }
    }

    Span block_span = mk_sp(generics.where_clause.span.hi, item->span.hi);
    std::string_view snippet = context.snippet(block_span);
    std::optional<size_t> op = find_uncommented(snippet, "{");
    if (!op) return Err(RewriteError::unknown());
    size_t open_pos = *op + 1;

    BraceStyle bs = brace_style(config);
    if (last_line_contains_single_line_comment(result) ||
        last_line_width(result) + 2 > context.budget(offset.width())) {
        result += offset.to_string_with_newline(config);
    } else if (config.empty_item_single_line() && items.empty() && !contains(result, '\n') &&
               !contains_comment(snippet.substr(open_pos))) {
        result += " {}";
        return result;
    } else if (bs == BraceStyle::AlwaysNextLine) {
        result += offset.to_string_with_newline(config);
    } else if (bs == BraceStyle::PreferSameLine) {
        result += ' ';
    } else {  // SameLineWhere
        if (contains(result, '\n') || (!generics.where_clause.predicates.empty() && !items.empty())) {
            result += offset.to_string_with_newline(config);
        } else {
            result += ' ';
        }
    }
    result += '{';

    std::string outer_indent_str = offset.block_only().to_string_with_newline(config);

    if (!items.empty() || contains_comment(snippet.substr(open_pos))) {
        FmtVisitor visitor = FmtVisitor::from_context(context);
        visitor.block_indent = offset.block_only().block_indent_(config);
        visitor.last_pos = block_span.lo + static_cast<uint32_t>(open_pos);

        for (const rust::Item* it : items) visitor.visit_trait_item(it);

        visitor.format_missing(item->span.hi - 1);

        std::string inner_indent_str = visitor.block_indent.to_string_with_newline(config);

        result += inner_indent_str;
        result += trim(visitor.buffer);
        result += outer_indent_str;
    } else if (contains(result, '\n')) {
        result += outer_indent_str;
    }

    result += '}';
    return result;
}

// impl Rewrite for TraitAliasBounds
static RewriteResult rewrite_trait_alias_bounds(const RewriteContext& context, const rust::GenericBounds* generic_bounds,
                                                const rust::Generics* generics, Shape shape) {
    TRY(std::string generic_bounds_str, rewrite_result(context, generic_bounds, shape));

    WhereClauseOption option = WhereClauseOption::new_(true, WhereClauseSpace::None);
    option.allow_single_line();

    TRY(std::string where_str,
        rewrite_where_clause(context, generics->where_clause, brace_style(*context.config), shape, false, ";",
                             std::nullopt, generics->where_clause.span.lo, option));

    bool fits_single_line = !contains(generic_bounds_str, '\n') && !contains(where_str, '\n') &&
                            generic_bounds_str.size() + where_str.size() < shape.width;
    std::string space;
    if (generic_bounds_str.empty() || where_str.empty()) {
        space = "";
    } else if (fits_single_line) {
        space = " ";
    } else {
        space = shape.indent.to_string_with_newline(*context.config);
    }

    return generic_bounds_str + space + where_str;
}

RewriteResult format_trait_alias(const RewriteContext& context, const rust::Item* item, Shape shape) {
    Span span = item->span;
    std::string_view alias = rewrite_ident(context, item->ident);
    // 6 = "trait ", 2 = " ="
    TRY(Shape g_shape0, offset_left(shape, 6, span));
    TRY(Shape g_shape, sub_width(g_shape0, 2, span));
    TRY(std::string generics_str, rewrite_generics(context, alias, item->generics, g_shape));
    std::string vis_str = format_visibility(context, item->vis);
    std::string_view constness = format_constness(item->constness);
    std::string lhs = vis_str + std::string(constness) + "trait " + generics_str + " =";
    // 1 = ";"
    const rust::GenericBounds* gb = &item->bounds;
    const rust::Generics* gen = &item->generics;
    RwNode trait_alias_bounds = RwNode::opaque(Span(), [gb, gen](const RewriteContext& c, Shape s) {
        return rewrite_trait_alias_bounds(c, gb, gen, s);
    });
    TRY(Shape rhs_shape, sub_width(shape, 1, item->generics.span));
    TRY(std::string result, rewrite_assign_rhs(context, lhs, trait_alias_bounds, RhsAssignKind::bounds(), rhs_shape));
    return result + ";";
}

// ---------------------------------------------------------------------------
// structs

static Opt format_unit_struct(const RewriteContext& context, const StructParts& p, Indent offset) {
    std::string header_str = format_header(context, p.prefix, p.ident, *p.vis, offset);
    std::string generics_str;
    if (p.generics) {
        uint32_t hi = context.snippet_provider->span_before_last(p.span, ";");
        TRY_OPT(generics_str, format_generics(context, *p.generics, brace_style(*context.config), BracePos::None,
                                              offset,
                                              // make a span that starts right after `struct Foo`
                                              mk_sp(p.ident.span.hi, hi), last_line_width(header_str)));
    }
    return header_str + generics_str + ";";
}

static Opt rewrite_with_alignment(const std::vector<rust::FieldDef*>& fields, const RewriteContext& context,
                                  Shape shape, Span span, size_t one_line_width);

static Opt format_struct_struct(const RewriteContext& context, const StructParts& struct_parts,
                                const std::vector<rust::FieldDef*>& fields, Indent offset,
                                std::optional<size_t> one_line_width) {
    const Config& config = *context.config;
    std::string result;
    Span span = struct_parts.span;

    std::string header_str = format_header(context, struct_parts.prefix, struct_parts.ident, *struct_parts.vis, offset);
    result += header_str;

    uint32_t header_hi = struct_parts.ident.span.hi;
    uint32_t body_lo;
    if (struct_parts.generics) {
        // Adjust the span to start at the end of the generic arguments before searching for the '{'
        Span span2 = span.with_lo(struct_parts.generics->where_clause.span.hi);
        body_lo = context.snippet_provider->span_after(span2, "{");
    } else {
        body_lo = context.snippet_provider->span_after(span, "{");
    }

    std::string generics_str;
    if (struct_parts.generics) {
        TRY_OPT(generics_str,
                format_generics(context, *struct_parts.generics, brace_style(config),
                                fields.empty() ? BracePos::ForceSameLine : BracePos::Auto, offset,
                                // make a span that starts right after `struct Foo`
                                mk_sp(header_hi, body_lo), last_line_width(result)));
    } else {
        // 3 = ` {}`, 2 = ` {`.
        size_t overhead = fields.empty() ? 3 : 2;
        if ((brace_style(config) == BraceStyle::AlwaysNextLine && !fields.empty()) ||
            config.max_width() < overhead + result.size()) {
            generics_str = "\n" + offset.block_only().to_string(config) + "{";
        } else {
            generics_str = " {";
        }
    }
    // 1 = `}`
    size_t overhead = fields.empty() ? 1 : 0;
    size_t total_width = result.size() + generics_str.size() + overhead;
    if (!generics_str.empty() && !contains(generics_str, '\n') && total_width > config.max_width()) {
        result += '\n';
        result += offset.to_string(config);
        result += trim_start(generics_str);
    } else {
        result += generics_str;
    }

    if (fields.empty()) {
        Span inner_span = mk_sp(body_lo, span.hi - 1);
        format_empty_struct_or_tuple(context, inner_span, offset, result, "", "}");
        return result;
    }

    // 3 = ` ` and ` }`
    size_t one_line_budget = context.budget(result.size() + 3 + offset.width());
    one_line_budget = one_line_width ? std::min(*one_line_width, one_line_budget) : 0;

    TRY_OPT(Shape items_shape, Shape::indented(offset.block_indent_(config), config).sub_width_opt(1));
    TRY_OPT(std::string items_str,
            rewrite_with_alignment(fields, context, items_shape, mk_sp(body_lo, span.hi), one_line_budget));

    if (!contains(items_str, '\n') && !contains(result, '\n') && items_str.size() <= one_line_budget &&
        !last_line_contains_single_line_comment(items_str)) {
        return result + " " + items_str + " }";
    }
    return result + "\n" + offset.block_indent_(config).to_string(config) + items_str + "\n" +
           offset.to_string(config) + "}";
}

static uint32_t get_bytepos_after_visibility(const rust::Visibility& vis, Span default_span) {
    if (vis.kind == rust::Visibility::Restricted) return vis.span.hi;
    return default_span.lo;
}

// Format tuple or struct without any fields. We need to make sure that the comments
// inside the delimiters are preserved.
static void format_empty_struct_or_tuple(const RewriteContext& context, Span span, Indent offset,
                                         std::string& result, std::string_view opener, std::string_view closer) {
    const Config& config = *context.config;
    // 3 = " {}" or "();"
    size_t used_width = last_line_used_width(result, offset.width()) + 3;
    if (used_width > config.max_width()) result += offset.to_string_with_newline(config);
    result += opener;

    // indented shape for proper indenting of multi-line comments
    Shape shape = Shape::indented(offset.block_indent_(config), config);
    RewriteResult r = rewrite_missing_comment(span, shape, context);
    if (r) {
        const std::string& s = *r;
        if (!s.empty()) {
            bool is_multi_line = !is_single_line(s);
            if (is_multi_line || first_line_contains_single_line_comment(s)) {
                std::string nested_indent_str = offset.block_indent_(config).to_string_with_newline(config);
                result += nested_indent_str;
            }
            result += s;
            if (is_multi_line || last_line_contains_single_line_comment(s)) {
                result += offset.to_string_with_newline(config);
            }
        }
    } else {
        result += context.snippet(span);
    }
    result += closer;
}

static Opt format_tuple_struct(const RewriteContext& context, const StructParts& struct_parts,
                               const std::vector<rust::FieldDef*>& fields, Indent offset) {
    const Config& config = *context.config;
    std::string result;
    Span span = struct_parts.span;

    std::string header_str = format_header(context, struct_parts.prefix, struct_parts.ident, *struct_parts.vis, offset);
    result += header_str;

    uint32_t body_lo;
    if (fields.empty()) {
        uint32_t lo = get_bytepos_after_visibility(*struct_parts.vis, span);
        body_lo = context.snippet_provider->span_after(mk_sp(lo, span.hi), "(");
    } else {
        body_lo = fields[0]->span.lo;
    }
    uint32_t body_hi;
    if (fields.empty()) {
        body_hi = context.snippet_provider->span_after(mk_sp(body_lo, span.hi), ")");
    } else {
        // This is a dirty hack to work around a missing `)` from the span of the last field.
        Span last_arg_span = fields.back()->span;
        std::optional<uint32_t> h = context.snippet_provider->opt_span_after(mk_sp(last_arg_span.hi, span.hi), ")");
        body_hi = h ? *h : last_arg_span.hi;
    }

    std::string where_clause_str;
    if (struct_parts.generics) {
        const rust::Generics& generics = *struct_parts.generics;
        size_t budget = context.budget(last_line_width(header_str));
        Shape shape = Shape::legacy(budget, offset);
        TRY_OPT(std::string generics_str, rewrite_generics(context, "", generics, shape));
        result += generics_str;

        size_t where_budget = context.budget(last_line_width(result));
        WhereClauseOption option = WhereClauseOption::new_(true, WhereClauseSpace::Newline);
        TRY_OPT(where_clause_str, rewrite_where_clause(context, generics.where_clause, brace_style(config),
                                                       Shape::legacy(where_budget, offset.block_only()), false, ";",
                                                       std::nullopt, body_hi, option));
    }

    if (fields.empty()) {
        uint32_t body_hi2 = context.snippet_provider->span_before(mk_sp(body_lo, span.hi), ")");
        Span inner_span = mk_sp(body_lo, body_hi2);
        format_empty_struct_or_tuple(context, inner_span, offset, result, "(", ")");
    } else {
        uint32_t lo = struct_parts.generics ? struct_parts.generics->span.hi : struct_parts.ident.span.hi;
        TRY_OPT(Shape shape, Shape::indented(offset, config).sub_width_opt(1));
        std::vector<OverflowableItem> items;
        items.reserve(fields.size());
        for (const rust::FieldDef* f : fields) items.push_back(RwNode::of(f));
        TRY_OPT(result, rewrite_with_parens(context, result, items, shape, mk_sp(lo, span.hi), config.fn_call_width(),
                                            std::nullopt));
    }

    if (!where_clause_str.empty() && !contains(where_clause_str, '\n') &&
        (contains(result, '\n') ||
         offset.block_indent + result.size() + where_clause_str.size() + 1 > config.max_width())) {
        // We need to put the where-clause on a new line, but we didn't
        // know that earlier, so the where-clause will not be indented properly.
        result += '\n';
        result += (offset.block_only() + (config.tab_spaces() - 1)).to_string(config);
    }
    result += where_clause_str;

    return result;
}

// ---------------------------------------------------------------------------
// vertical.rs's rewrite_with_alignment for struct fields (AlignedItem for
// ast::FieldDef), at the default struct_field_align_threshold = 0: one group.

// AlignedItem::rewrite_prefix for ast::FieldDef
static RewriteResult field_rewrite_prefix(const RewriteContext& context, const rust::FieldDef* field, Shape shape) {
    TRY(std::string attrs_str, rewrite_attrs(context, field->attrs, shape));
    Span missing_span = field->attrs.empty() ? mk_sp(field->span.lo, field->span.lo)
                                             : mk_sp(field->attrs.back().span.hi, field->span.lo);
    bool attrs_extendable = !field->has_ident && is_attributes_extendable(attrs_str);
    TRY(std::string field_str, rewrite_struct_field_prefix(context, field));
    return combine_strs_with_missing_comments(context, attrs_str, field_str, missing_span, shape, attrs_extendable);
}

static std::pair<size_t, size_t> struct_field_prefix_max_min_width(const RewriteContext& context,
                                                                   const std::vector<rust::FieldDef*>& fields,
                                                                   Shape shape) {
    size_t max_len = 0, min_len = SIZE_MAX;
    for (const rust::FieldDef* field : fields) {
        RewriteResult r = field_rewrite_prefix(context, field, shape);
        if (!r) return {0, 0};
        size_t len = trimmed_last_line_width(*r);
        max_len = std::max(max_len, len);
        min_len = std::min(min_len, len);
    }
    return {max_len, min_len};
}

static Opt rewrite_aligned_items_inner(const RewriteContext& context, const std::vector<rust::FieldDef*>& fields,
                                       Span span, Indent offset, size_t one_line_width,
                                       bool force_trailing_separator) {
    const Config& config = *context.config;
    // 1 = ","
    TRY_OPT(Shape item_shape, Shape::indented(offset, config).sub_width_opt(1));
    auto mm = struct_field_prefix_max_min_width(context, fields, item_shape);
    size_t field_prefix_max_width = mm.first;
    size_t max_diff = sat_sub(mm.first, mm.second);
    if (max_diff > config.struct_field_align_threshold()) field_prefix_max_width = 0;

    std::vector<ListItem> items = itemize_list(
        *context.snippet_provider, fields.size(), "}", ",",
        [&](size_t i) -> uint32_t { return spanned(fields[i]).lo; },
        [&](size_t i) -> uint32_t { return spanned(fields[i]).hi; },
        [&](size_t i) -> RewriteResult {
            return rewrite_struct_field(context, fields[i], item_shape, field_prefix_max_width);
        },
        span.lo, span.hi, false);

    DefinitiveListTactic tactic =
        definitive_tactic(items, ListTactic::HorizontalVertical(), Separator::Comma, one_line_width);

    if (tactic == DefinitiveListTactic::Horizontal()) {
        // since the items fits on a line, there is no need to align them
        for (size_t i = 0; i < fields.size() && i < items.size(); i++) {
            if (items[i].item.ok()) items[i].item = rewrite_struct_field(context, fields[i], item_shape, 0);
        }
    }

    SeparatorTactic separator_tactic = force_trailing_separator ? SeparatorTactic::Always : trailing_comma(config);

    ListFormatting fmt =
        ListFormatting(item_shape, config).tactic(tactic).trailing_separator(separator_tactic).preserve_newline(true);
    return write_list(items, fmt).opt();
}

static Opt rewrite_with_alignment(const std::vector<rust::FieldDef*>& fields, const RewriteContext& context,
                                  Shape shape, Span span, size_t one_line_width) {
    // non-default config: struct_field_align_threshold > 0 (group_aligned_items); the
    // default makes one group of every field.
    std::string_view spaces = "";
    // rest is empty: init_last_pos = span.hi(), the whole span.
    Span init_span = mk_sp(span.lo, span.hi);
    // if another group follows, we must force a separator
    bool force_separator = false;

    TRY_OPT(std::string result,
            rewrite_aligned_items_inner(context, fields, init_span, shape.indent, one_line_width, force_separator));
    return result + std::string(spaces);
}

// ---------------------------------------------------------------------------
// type aliases

static RewriteResult rewrite_ty(const TyAliasRewriteInfo& rw_info, const rust::GenericBounds* generic_bounds_opt,
                                const std::optional<RwNode>& rhs, uint32_t rhs_hi, const rust::Visibility& vis);

// OpaqueType (impl Rewrite: rewrite() only, rewrite_result = rewrite().unknown_error())
static RwNode opaque_type_node(const rust::GenericBounds* bounds) {
    return RwNode::opaque(Span(), [bounds](const RewriteContext& context, Shape shape) -> RewriteResult {
        std::optional<Shape> s = shape.offset_left_opt(5);  // `impl `
        if (!s) return Err(RewriteError::unknown());
        Opt b = rewrite_result(context, bounds, *s).opt();
        if (!b) return Err(RewriteError::unknown());
        return "impl " + *b;
    });
}

RewriteResult rewrite_type_alias(const rust::Item* item, const RewriteContext& context, Indent indent,
                                 ItemVisitorKind visitor_kind, Span span) {
    rust::Defaultness defaultness = item->defaultness;
    const rust::Generics& generics = item->generics;
    const rust::GenericBounds& bounds = item->bounds;
    const rust::Ty* ty = item->ty;
    std::optional<RwNode> ty_opt;
    if (ty) ty_opt = RwNode::of(ty);
    uint32_t rhs_hi = ty ? ty->span.hi : generics.where_clause.span.hi;
    TyAliasRewriteInfo rw_info{&context, indent, &generics, &item->after_where_clause, item->ident, span};
    const rust::GenericBounds* op_ty = opaque_ty(ty);
    // Type Aliases are formatted slightly differently depending on the context
    // in which they appear, whether they are opaque, and whether they are associated.
    // https://rustc-dev-guide.rust-lang.org/opaque-types-type-alias-impl-trait.html
    // https://github.com/rust-dev-tools/fmt-rfcs/blob/master/guide/items.md#type-aliases
    if (visitor_kind != ItemVisitorKind::AssocImplItem) {
        if (op_ty) return rewrite_ty(rw_info, &bounds, opaque_type_node(op_ty), rhs_hi, item->vis);
        return rewrite_ty(rw_info, &bounds, ty_opt, rhs_hi, item->vis);
    }
    std::string result;
    if (op_ty) {
        TRY(result, rewrite_ty(rw_info, &bounds, opaque_type_node(op_ty), rhs_hi, default_visibility()));
    } else {
        TRY(result, rewrite_ty(rw_info, &bounds, ty_opt, rhs_hi, item->vis));
    }
    if (defaultness.kind == rust::Defaultness::Default) return "default " + result;
    return result;
}

static RewriteResult rewrite_ty(const TyAliasRewriteInfo& rw_info, const rust::GenericBounds* generic_bounds_opt,
                                const std::optional<RwNode>& rhs, uint32_t rhs_hi, const rust::Visibility& vis) {
    const RewriteContext& context = *rw_info.context;
    const Config& config = *context.config;
    Indent indent = rw_info.indent;
    const rust::Generics& generics = *rw_info.generics;
    const rust::WhereClause& after_where_clause = *rw_info.after_where_clause;
    Span span = rw_info.span;

    std::string result;
    result += format_visibility(context, vis) + "type ";
    std::string_view ident_str = rewrite_ident(context, rw_info.ident);

    if (generics.params.empty()) {
        result += ident_str;
    } else {
        // 2 = `= `
        Shape g_shape0 = Shape::indented(indent, config);
        TRY(Shape g_shape1, offset_left(g_shape0, result.size(), span));
        TRY(Shape g_shape, sub_width(g_shape1, 2, span));
        TRY(std::string generics_str, rewrite_generics(context, ident_str, generics, g_shape));
        result += generics_str;
    }

    if (generic_bounds_opt) {
        const rust::GenericBounds& bounds = *generic_bounds_opt;
        if (!bounds.empty()) {
            // 2 = `: `
            Shape shape0 = Shape::indented(indent, config);
            TRY(Shape shape, offset_left(shape0, result.size() + 2, span));
            TRY(std::string b, rewrite_result(context, &bounds, shape));
            result += ": " + b;
        }
    }

    size_t where_budget = context.budget(last_line_width(result));
    WhereClauseOption option = WhereClauseOption::snuggled(result);
    if (!rhs) option.suppress_comma();
    TRY(std::string before_where_clause_str,
        rewrite_where_clause(context, generics.where_clause, brace_style(config), Shape::legacy(where_budget, indent),
                             false, "=", std::nullopt, generics.span.hi, option));
    result += before_where_clause_str;

    if (rhs) {
        // If there are any where clauses, add a newline before the assignment.
        // If there is a before where clause, do not indent, but if there is
        // only an after where clause, additionally indent the type.
        if (!generics.where_clause.predicates.empty()) {
            result += indent.to_string_with_newline(config);
        } else if (!after_where_clause.predicates.empty()) {
            result += indent.block_indent_(config).to_string_with_newline(config);
        } else {
            result += ' ';
        }

        std::optional<Span> comment_span;
        if (std::optional<uint32_t> op_lo = context.snippet_provider->opt_span_before(span, "="))
            comment_span = mk_sp(generics.where_clause.span.hi, *op_lo);

        std::string lhs;
        bool done = false;
        if (comment_span) {
            std::optional<std::string_view> cs = context.snippet_provider->span_to_snippet(*comment_span);
            if (!cs) return Err(RewriteError::unknown());
            if (contains_comment(*cs)) {
                Shape comment_shape;
                if (!generics.where_clause.predicates.empty()) {
                    comment_shape = Shape::indented(indent, config);
                } else {
                    Shape shape = Shape::indented(indent, config);
                    TRY(comment_shape, block_left(shape, config.tab_spaces(), span));
                }

                TRY(lhs, combine_strs_with_missing_comments(context, trim_end(result), "=", *comment_span,
                                                            comment_shape, true));
                done = true;
            }
        }
        if (!done) lhs = result + "=";

        // 1 = `;` unless there's a trailing where clause
        Shape shape = Shape::indented(indent, config);
        if (after_where_clause.predicates.empty()) {
            TRY(shape, sub_width(Shape::indented(indent, config), 1, span));
        }
        TRY(result, rewrite_assign_rhs(context, lhs, *rhs, RhsAssignKind::ty(), shape));
    }

    if (!after_where_clause.predicates.empty()) {
        WhereClauseOption option2 = WhereClauseOption::new_(true, WhereClauseSpace::Newline);
        TRY(std::string after_where_clause_str,
            rewrite_where_clause(context, after_where_clause, brace_style(config), Shape::indented(indent, config),
                                 false, ";", std::nullopt, rhs_hi, option2));
        result += after_where_clause_str;
    }

    result += ";";
    return result;
}

// ---------------------------------------------------------------------------
// struct fields

static std::pair<const char*, const char*> type_annotation_spacing(const Config& config) {
    return {config.space_before_colon() ? " " : "", config.space_after_colon() ? " " : ""};
}

RewriteResult rewrite_struct_field_prefix(const RewriteContext& context, const rust::FieldDef* field) {
    std::string vis = format_visibility(context, field->vis);
    std::string_view safety = format_safety(field->safety);
    auto spacing = type_annotation_spacing(*context.config);
    if (field->has_ident) {
        return vis + std::string(safety) + std::string(rewrite_ident(context, field->ident)) + spacing.first + ":";
    }
    return vis + std::string(safety);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::FieldDef* x, Shape shape) {
    return rewrite_struct_field(context, x, shape, 0);
}

RewriteResult rewrite_struct_field(const RewriteContext& context, const rust::FieldDef* field, Shape shape,
                                   size_t lhs_max_width) {
    // FIXME(default_field_values): Implement formatting.
    if (field->default_) return Err(RewriteError::unknown());

    if (contains_skip(context, field->attrs)) return std::string(context.snippet(spanned(field)));

    auto spacing_pair = type_annotation_spacing(*context.config);
    TRY(std::string prefix, rewrite_struct_field_prefix(context, field));

    TRY(std::string attrs_str, rewrite_attrs(context, field->attrs, shape));
    bool attrs_extendable = !field->has_ident && is_attributes_extendable(attrs_str);
    Span missing_span = field->attrs.empty() ? mk_sp(field->span.lo, field->span.lo)
                                             : mk_sp(field->attrs.back().span.hi, field->span.lo);
    std::string spacing = field->has_ident ? spacing_pair.second : "";
    // Try to put everything on a single line.
    TRY(std::string attr_prefix,
        combine_strs_with_missing_comments(context, attrs_str, prefix, missing_span, shape, attrs_extendable));
    size_t overhead = trimmed_last_line_width(attr_prefix);
    size_t lhs_offset = sat_sub(lhs_max_width, overhead);
    for (size_t i = 0; i < lhs_offset; i++) spacing += ' ';
    // In this extreme case we will be missing a space between an attribute and a field.
    if (prefix.empty() && !attrs_str.empty() && attrs_extendable && spacing.empty()) spacing += ' ';

    Opt orig_ty;
    if (std::optional<Shape> ty_shape = shape.offset_left_opt(overhead + spacing.size()))
        orig_ty = rewrite_result(context, field->ty, *ty_shape).opt();

    if (orig_ty) {
        if (!contains(*orig_ty, '\n') && !contains_comment(context.snippet(missing_span)))
            return attr_prefix + spacing + *orig_ty;
    }

    bool is_prefix_empty = prefix.empty();
    // We must use multiline. We are going to put attributes and a field on different lines.
    TRY(std::string field_str, rewrite_assign_rhs(context, prefix, RwNode::of(field->ty), RhsAssignKind::ty(), shape));
    // Remove a leading white-space from `rewrite_assign_rhs()` when rewriting a tuple struct.
    std::string_view fs = is_prefix_empty ? trim_start(field_str) : std::string_view(field_str);
    return combine_strs_with_missing_comments(context, attrs_str, fs, missing_span, shape, false);
}

// ---------------------------------------------------------------------------
// statics / consts

static Opt rewrite_static(const RewriteContext& context, const StaticParts& static_parts, Indent offset) {
    const Config& config = *context.config;
    // For now, if this static (or const) has generics, then bail.
    if (static_parts.generics) {
        const rust::Generics& g = *static_parts.generics;
        bool where_empty = !g.where_clause.has_where_token && g.where_clause.predicates.empty();
        if (!g.params.empty() || !where_empty) return std::nullopt;
    }

    std::string_view colon = colon_spaces(config);
    std::string prefix = format_visibility(context, *static_parts.vis) +
                         (static_parts.defaultness ? format_defaultness(*static_parts.defaultness) : "") +
                         format_safety(static_parts.safety) + std::string(static_parts.prefix) + " " +
                         format_mutability(static_parts.mutability) +
                         std::string(rewrite_ident(context, static_parts.ident)) + std::string(colon);
    // 2 = " =".len()
    TRY_OPT(Shape ty_shape, Shape::indented(offset.block_only(), config).offset_left_opt(prefix.size() + 2));
    std::string ty_str;
    Opt ts = rewrite_result(context, static_parts.ty, ty_shape).opt();
    if (ts) {
        ty_str = *ts;
    } else {
        if (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
        Indent nested_indent = offset.block_indent_(config);
        Shape nested_shape = Shape::indented(nested_indent, config);
        TRY_OPT(std::string t2, rewrite_result(context, static_parts.ty, nested_shape).opt());
        ty_str = nested_indent.to_string_with_newline(config) + t2;
    }

    if (const rust::Expr* expr = static_parts.expr_opt) {
        uint32_t comments_lo = context.snippet_provider->span_after(static_parts.span, "=");
        uint32_t expr_lo = expr->span.lo;
        Span comments_span = mk_sp(comments_lo, expr_lo);

        std::string lhs = prefix + ty_str + " =";

        // 1 = ;
        size_t remaining_width = context.budget(offset.block_indent + 1);
        RewriteResult r = rewrite_assign_rhs_with_comments(
            context, lhs, RwNode::of(expr), Shape::legacy(remaining_width, offset.block_only()),
            RhsAssignKind::of_expr(expr), RhsTactics::Default, comments_span, true);
        if (!r) return std::nullopt;
        std::string s = recover_comment_removed(std::move(*r), static_parts.span, context);
        if (!ends_with(s, ";")) s += ";";
        return s;
    }
    return prefix + ty_str + ";";
}

// ---------------------------------------------------------------------------
// impl Rewrite for ast::FnRetTy

RewriteResult rewrite_result(const RewriteContext& context, const rust::FnRetTy* x, Shape shape) {
    if (x->is_default()) return std::string();
    const size_t arrow_width = 3;  // "-> ".len()
    if (context.config->style_edition() <= StyleEdition::E2021 ||
        context.config->indent_style() == IndentStyle::Visual) {
        std::optional<size_t> inner_width = checked_sub(shape.width, arrow_width);
        if (!inner_width) return Err(RewriteError::exceeds(shape.width, spanned(x)));
        TRY(std::string r, rewrite_result(context, x->ty, Shape::legacy(*inner_width, shape.indent + arrow_width)));
        return "-> " + r;
    }

    TRY(Shape shape2, offset_left(shape, arrow_width, spanned(x)));
    TRY(std::string s, rewrite_result(context, x->ty, shape2));
    return "-> " + s;
}

// ---------------------------------------------------------------------------
// impl Rewrite for ast::Param

static bool is_empty_infer(const rust::Ty* ty, Span pat_span) {
    if (ty->kind == rust::TyKind::Infer) return ty->span.hi == pat_span.hi;
    return false;
}

/// Recover any missing comments between the param and the type.
///
/// # Returns
///
/// A 2-len tuple with the comment before the colon in first position, and the comment after the
/// colon in second position.
static std::pair<std::string, std::string> get_missing_param_comments(const RewriteContext& context, Span pat_span,
                                                                      Span ty_span, Shape shape) {
    Span missing_comment_span = mk_sp(pat_span.hi, ty_span.lo);

    Span span_before_colon;
    {
        uint32_t missing_comment_span_hi = context.snippet_provider->span_before(missing_comment_span, ":");
        span_before_colon = mk_sp(pat_span.hi, missing_comment_span_hi);
    }
    Span span_after_colon;
    {
        uint32_t missing_comment_span_lo = context.snippet_provider->span_after(missing_comment_span, ":");
        span_after_colon = mk_sp(missing_comment_span_lo, ty_span.lo);
    }

    std::string comment_before_colon;
    {
        RewriteResult c = rewrite_missing_comment(span_before_colon, shape, context);
        if (c && !c->empty()) comment_before_colon = " " + *c;
    }
    std::string comment_after_colon;
    {
        RewriteResult c = rewrite_missing_comment(span_after_colon, shape, context);
        if (c && !c->empty()) comment_after_colon = *c + " ";
    }
    return {comment_before_colon, comment_after_colon};
}

namespace {
/// ast::ExplicitSelf (Param::to_self)
struct ExplicitSelf {
    enum Kind : uint8_t { Value, Region, Pinned, Explicit } kind = Value;
    rust::Mutability mutbl = rust::Mutability::Not;
    bool has_lifetime = false;
    rust::Lifetime lifetime;
    const rust::Ty* ty = nullptr;  // Explicit
};

std::optional<ExplicitSelf> param_to_self(const rust::Param* p) {
    const rust::Pat* pat = p->pat;
    if (!pat || pat->kind != rust::PatKind::Ident || pat->by_ref || pat->ident.name != "self") return std::nullopt;
    ExplicitSelf es;
    const rust::Ty* ty = p->ty;
    if (ty->kind == rust::TyKind::ImplicitSelf) {
        es.kind = ExplicitSelf::Value;
        es.mutbl = pat->mutbl;
    } else if ((ty->kind == rust::TyKind::Ref || ty->kind == rust::TyKind::PinnedRef) && ty->inner &&
               ty->inner->kind == rust::TyKind::ImplicitSelf) {
        es.kind = ty->kind == rust::TyKind::Ref ? ExplicitSelf::Region : ExplicitSelf::Pinned;
        es.mutbl = ty->mutbl;
        es.has_lifetime = ty->has_lifetime;
        es.lifetime = ty->lifetime;
    } else {
        es.kind = ExplicitSelf::Explicit;
        es.ty = ty;
        es.mutbl = pat->mutbl;
    }
    return es;
}
}  // namespace

static RewriteResult rewrite_opt_lifetime(const RewriteContext& context, bool has, const rust::Lifetime& l) {
    if (!has) return std::string();
    TRY(std::string result, rewrite_result(context, &l, Shape::legacy(context.config->max_width(), Indent::empty())));
    result += ' ';
    return result;
}

static RewriteResult rewrite_explicit_self(const RewriteContext& context, const ExplicitSelf& explicit_self,
                                           std::string_view param_attrs, Span span, Shape shape,
                                           bool has_multiple_attr_lines) {
    std::string self_str;
    switch (explicit_self.kind) {
    case ExplicitSelf::Region: {
        std::string_view mut_str = format_mutability(explicit_self.mutbl);
        TRY(std::string lifetime_str, rewrite_opt_lifetime(context, explicit_self.has_lifetime, explicit_self.lifetime));
        self_str = "&" + lifetime_str + std::string(mut_str) + "self";
        break;
    }
    case ExplicitSelf::Pinned: {
        std::string_view mut_str = explicit_self.mutbl == rust::Mutability::Mut ? "mut" : "const";  // ptr_str
        TRY(std::string lifetime_str, rewrite_opt_lifetime(context, explicit_self.has_lifetime, explicit_self.lifetime));
        self_str = "&" + lifetime_str + "pin " + std::string(mut_str) + " self";
        break;
    }
    case ExplicitSelf::Explicit: {
        TRY(std::string type_str, rewrite_result(context, explicit_self.ty,
                                                 Shape::legacy(context.config->max_width(), Indent::empty())));
        self_str = std::string(format_mutability(explicit_self.mutbl)) + "self: " + type_str;
        break;
    }
    case ExplicitSelf::Value: self_str = std::string(format_mutability(explicit_self.mutbl)) + "self"; break;
    }
    return combine_strs_with_missing_comments(context, param_attrs, self_str, span, shape, !has_multiple_attr_lines);
}

RewriteResult rewrite_result(const RewriteContext& context, const rust::Param* self, Shape shape) {
    TRY(std::string param_attrs_result, rewrite_attrs(context, self->attrs, Shape::legacy(shape.width, shape.indent)));
    // N.B. Doc comments aren't typically valid syntax, but could appear
    // in the presence of certain macros - https://github.com/rust-lang/rustfmt/issues/4936
    Span span;
    bool has_multiple_attr_lines = false, has_doc_comments = false;
    if (!self->attrs.empty()) {
        span = mk_sp(self->attrs.back().span.hi, self->pat->span.lo);
        has_multiple_attr_lines = contains(param_attrs_result, '\n');
        for (const rust::Attribute& a : self->attrs)
            if (a.is_doc_comment) has_doc_comments = true;
    } else {
        span = mk_sp(self->span.lo, self->span.lo);
    }

    if (std::optional<ExplicitSelf> explicit_self = param_to_self(self)) {
        return rewrite_explicit_self(context, *explicit_self, param_attrs_result, span, shape,
                                     has_multiple_attr_lines);
    }
    if (is_named_param(self)) {
        TRY(std::string param_name, rewrite_result(context, self->pat, Shape::legacy(shape.width, shape.indent)));
        TRY(std::string result, combine_strs_with_missing_comments(context, param_attrs_result, param_name, span, shape,
                                                                   !has_multiple_attr_lines && !has_doc_comments));

        if (!is_empty_infer(self->ty, self->pat->span)) {
            auto comments = get_missing_param_comments(context, self->pat->span, self->ty->span, shape);
            const std::string& before_comment = comments.first;
            const std::string& after_comment = comments.second;
            result += before_comment;
            result += colon_spaces(*context.config);
            result += after_comment;
            size_t overhead = last_line_width(result);
            std::optional<size_t> max_width = checked_sub(shape.width, overhead);
            if (!max_width) return Err(RewriteError::exceeds(shape.width, spanned(self)));
            RewriteResult ty_str = rewrite_result(context, self->ty, Shape::legacy(*max_width, shape.indent));
            if (ty_str) {
                result += *ty_str;
            } else {
                std::string prev_str = param_attrs_result.empty()
                                           ? param_attrs_result
                                           : param_attrs_result + shape.to_string_with_newline(*context.config);

                TRY(result, combine_strs_with_missing_comments(context, prev_str, param_name, span, shape,
                                                               !has_multiple_attr_lines));
                result += before_comment;
                result += colon_spaces(*context.config);
                result += after_comment;
                size_t overhead2 = last_line_width(result);
                std::optional<size_t> max_width2 = checked_sub(shape.width, overhead2);
                if (!max_width2) return Err(RewriteError::exceeds(shape.width, spanned(self)));
                TRY(std::string ty_str2, rewrite_result(context, self->ty, Shape::legacy(*max_width2, shape.indent)));
                result += ty_str2;
            }
        }

        return result;
    }
    return rewrite_result(context, self->ty, shape);
}

uint32_t span_lo_for_param(const rust::Param* param) {
    if (param->attrs.empty()) {
        if (is_named_param(param)) return param->pat->span.lo;
        return param->ty->span.lo;
    }
    return param->attrs[0].span.lo;
}

uint32_t span_hi_for_param(const RewriteContext& context, const rust::Param* param) {
    if (param->ty->kind == rust::TyKind::Infer) {
        if (context.snippet(param->ty->span) == "_") return param->ty->span.hi;
        if (is_named_param(param)) return param->pat->span.hi;
    }
    return param->ty->span.hi;
}

bool is_named_param(const rust::Param* param) { return !(param->pat && param->pat->kind == rust::PatKind::Missing); }

// ---------------------------------------------------------------------------
// fn signatures

static std::tuple<size_t, size_t, Indent> compute_budgets_for_params(const RewriteContext& context,
                                                                     std::string_view result, Indent indent,
                                                                     size_t ret_str_len, FnBraceStyle fn_brace_style,
                                                                     bool force_vertical_layout);
static RewriteResult rewrite_params(const RewriteContext& context, const std::vector<rust::Param*>& params,
                                    size_t one_line_budget, size_t multi_line_budget, Indent indent,
                                    Indent param_indent, Span span, bool variadic);

// Return type is (result, force_new_line_for_brace)
static Expected<std::tuple<std::string, bool, bool>> rewrite_fn_base(const RewriteContext& context, Indent indent,
                                                                     rust::Ident ident, const FnSig& fn_sig,
                                                                     Span span, FnBraceStyle fn_brace_style) {
    const Config& config = *context.config;
    bool force_new_line_for_brace = false;

    const rust::WhereClause& where_clause = fn_sig.generics->where_clause;

    std::string result;
    result += fn_sig.to_str(context);

    // fn foo
    result += "fn ";

    // Generics.
    size_t overhead = fn_brace_style == FnBraceStyle::SameLine ? 4 /* `() {` */ : 2 /* `()` */;
    size_t used_width = last_line_used_width(result, indent.width());
    size_t one_line_budget0 = context.budget(used_width + overhead);
    Shape shape{one_line_budget0, indent, used_width};
    const rust::FnDecl* fd = fn_sig.decl;
    TRY(std::string generics_str, rewrite_generics(context, rewrite_ident(context, ident), *fn_sig.generics, shape));
    result += generics_str;

    bool snuggle_angle_bracket = false;
    {
        std::vector<std::string_view> gl = lines(generics_str);
        if (!gl.empty()) snuggle_angle_bracket = trim_start(gl.back()).size() == 1;
    }

    // Note that the width and indent don't really matter, we'll re-layout the
    // return type later anyway.
    TRY(std::string ret_str, rewrite_result(context, &fd->output, Shape::indented(indent, config)));

    bool multi_line_ret_str = contains(ret_str, '\n');
    size_t ret_str_len = multi_line_ret_str ? 0 : ret_str.size();

    // Params.
    auto budgets =
        compute_budgets_for_params(context, result, indent, ret_str_len, fn_brace_style, multi_line_ret_str);
    size_t one_line_budget = std::get<0>(budgets);
    size_t multi_line_budget = std::get<1>(budgets);
    Indent param_indent = std::get<2>(budgets);

    result += '(';
    // Check if vertical layout was forced.
    // non-default config: indent_style = Visual (a newline after `(` when one_line_budget == 0).
    (void)snuggle_angle_bracket;

    uint32_t params_end;
    if (fd->inputs.empty()) {
        params_end = context.snippet_provider->span_after(mk_sp(fn_sig.generics->span.hi, span.hi), ")");
    } else {
        Span last_span = mk_sp(spanned(fd->inputs.back()).hi, span.hi);
        params_end = context.snippet_provider->span_after(last_span, ")");
    }
    Span params_span =
        mk_sp(context.snippet_provider->span_after(mk_sp(fn_sig.generics->span.hi, span.hi), "("), params_end);
    TRY(std::string param_str, rewrite_params(context, fd->inputs, one_line_budget, multi_line_budget, indent,
                                              param_indent, params_span, fd->c_variadic()));

    bool put_params_in_block =
        (contains(param_str, '\n') || param_str.size() > one_line_budget) && !fd->inputs.empty();

    bool params_last_line_contains_comment = false;
    bool no_params_and_over_max_width = false;

    if (put_params_in_block) {
        param_indent = indent.block_indent_(config);
        result += param_indent.to_string_with_newline(config);
        result += param_str;
        result += indent.to_string_with_newline(config);
        result += ')';
    } else {
        result += param_str;
        size_t used_width2 = last_line_used_width(result, indent.width()) + first_line_width(ret_str);
        // Put the closing brace on the next line if it overflows the max width.
        // 1 = `)`
        bool closing_paren_overflow_max_width = fd->inputs.empty() && used_width2 + 1 > config.max_width();
        // If the last line of params contains comment, we cannot put the closing paren
        // on the same line.
        {
            std::vector<std::string_view> pl = lines(param_str);
            params_last_line_contains_comment = !pl.empty() && contains(pl.back(), "//");
        }

        if (config.style_edition() >= StyleEdition::E2024) {
            if (closing_paren_overflow_max_width) {
                result += ')';
                result += indent.to_string_with_newline(config);
                no_params_and_over_max_width = true;
            } else if (params_last_line_contains_comment) {
                result += indent.to_string_with_newline(config);
                result += ')';
                no_params_and_over_max_width = true;
            } else {
                result += ')';
            }
        } else {
            if (closing_paren_overflow_max_width || params_last_line_contains_comment)
                result += indent.to_string_with_newline(config);
            result += ')';
        }
    }

    // Return type.
    if (!fd->output.is_default()) {
        bool ret_should_indent;
        if (put_params_in_block || fd->inputs.empty()) {
            // If our params are block layout then we surely must have space.
            ret_should_indent = false;
        } else if (params_last_line_contains_comment) {
            ret_should_indent = false;
        } else if (contains(result, '\n') || multi_line_ret_str) {
            ret_should_indent = true;
        } else {
            // If the return type would push over the max width, then put the return type on
            // a new line. With the +1 for the signature length an additional space between
            // the closing parenthesis of the param and the arrow '->' is considered.
            size_t sig_length = result.size() + indent.width() + ret_str_len + 1;

            // If there is no where-clause, take into account the space after the return type
            // and the brace.
            if (where_clause.predicates.empty()) sig_length += 2;

            ret_should_indent = sig_length > config.max_width();
        }
        Shape ret_shape;
        if (ret_should_indent) {
            if (config.style_edition() <= StyleEdition::E2021 || config.indent_style() == IndentStyle::Visual) {
                Indent ind;
                if (param_str.empty()) {
                    // Aligning with nonexistent params looks silly.
                    force_new_line_for_brace = true;
                    ind = indent + 4;
                } else {
                    // FIXME: we might want to check that using the param indent
                    // doesn't blow our budget, and if it does, then fallback to
                    // the where-clause indent.
                    ind = param_indent;
                }

                result += ind.to_string_with_newline(config);
                ret_shape = Shape::indented(ind, config);
            } else {
                Shape rs = Shape::indented(indent, config);
                if (param_str.empty()) {
                    // Aligning with nonexistent params looks silly.
                    force_new_line_for_brace = true;
                    // context.use_block_indent() is always true
                    std::optional<Shape> o = rs.offset_left_opt(4);
                    if (o) rs = *o;
                }

                result += rs.indent.to_string_with_newline(config);
                ret_shape = rs;
            }
        } else {
            if (config.style_edition() >= StyleEdition::E2024) {
                if (!param_str.empty() || !no_params_and_over_max_width) result += ' ';
            } else {
                result += ' ';
            }

            Shape rs = Shape::indented(indent, config);
            std::optional<Shape> o = rs.offset_left_opt(last_line_width(result));
            ret_shape = o ? *o : rs;
        }

        if (multi_line_ret_str || ret_should_indent) {
            // Now that we know the proper indent and width, we need to
            // re-layout the return type.
            TRY(std::string ret_str2, rewrite_result(context, &fd->output, ret_shape));
            result += ret_str2;
        } else {
            result += ret_str;
        }

        // Comment between return type and the end of the decl.
        uint32_t snippet_lo = spanned(&fd->output).hi;
        if (where_clause.predicates.empty()) {
            uint32_t snippet_hi = span.hi;
            std::string_view snippet = context.snippet(mk_sp(snippet_lo, snippet_hi));
            // Try to preserve the layout of the original snippet.
            bool original_starts_with_newline = false;
            {
                size_t i = snippet.find_first_not_of(' ');
                if (i != std::string_view::npos) original_starts_with_newline = starts_with_newline(snippet.substr(i));
            }
            bool original_ends_with_newline = false;
            {
                size_t i = snippet.find_last_not_of(' ');
                if (i != std::string_view::npos) original_ends_with_newline = ends_with(snippet.substr(i), "\n");
            }
            snippet = trim(snippet);
            if (!snippet.empty()) {
                result += original_starts_with_newline ? '\n' : ' ';
                result += snippet;
                if (original_ends_with_newline) force_new_line_for_brace = true;
            }
        }
    }

    uint32_t pos_before_where = fd->output.is_default() ? params_span.hi : fd->output.ty->span.hi;

    bool is_params_multi_lined = contains(param_str, '\n');

    WhereClauseSpace space =
        put_params_in_block && ret_str.empty() ? WhereClauseSpace::Space : WhereClauseSpace::Newline;
    WhereClauseOption option = WhereClauseOption::new_(fn_brace_style == FnBraceStyle::None, space);
    if (is_params_multi_lined) option.veto_single_line();
    TRY(std::string where_clause_str,
        rewrite_where_clause(context, where_clause, brace_style(config), Shape::indented(indent, config), true, "{",
                             span.hi, pos_before_where, option));
    // If there are neither where-clause nor return type, we may be missing comments between
    // params and `{`.
    if (where_clause_str.empty()) {
        if (fd->output.is_default()) {
            Span ret_span = fd->output.span;
            RewriteResult mc = recover_missing_comment_in_span(
                // from after the closing paren to right before block or semicolon
                mk_sp(ret_span.lo, span.hi), shape, context, last_line_width(result));
            if (mc && !mc->empty()) {
                result += *mc;
                force_new_line_for_brace = true;
            }
        }
    }

    result += where_clause_str;

    bool ends_with_comment = last_line_contains_single_line_comment(result);
    force_new_line_for_brace |= ends_with_comment;
    force_new_line_for_brace |= is_params_multi_lined && config.where_single_line() && !where_clause_str.empty();
    return std::make_tuple(std::move(result), ends_with_comment, force_new_line_for_brace);
}

static RewriteResult rewrite_params(const RewriteContext& context, const std::vector<rust::Param*>& params,
                                    size_t one_line_budget, size_t multi_line_budget, Indent indent,
                                    Indent param_indent, Span span, bool variadic) {
    const Config& config = *context.config;
    if (params.empty()) {
        std::string_view comment = trim(context.snippet(mk_sp(span.lo,
                                                              // to remove ')'
                                                              span.hi - 1)));
        return std::string(comment);
    }
    std::vector<ListItem> param_items = itemize_list(
        *context.snippet_provider, params.size(), ")", ",",
        [&](size_t i) -> uint32_t { return span_lo_for_param(params[i]); },
        [&](size_t i) -> uint32_t { return params[i]->ty->span.hi; },
        [&](size_t i) -> RewriteResult {
            RewriteResult r = rewrite_result(context, params[i], Shape::legacy(multi_line_budget, param_indent));
            if (r) return r;
            return std::string(context.snippet(spanned(params[i])));
        },
        span.lo, span.hi, false);

    // fn_params_layout = Tall: to_list_tactic = HorizontalVertical
    DefinitiveListTactic tactic =
        definitive_tactic(param_items, ListTactic::HorizontalVertical(), Separator::Comma, one_line_budget);
    size_t budget = tactic == DefinitiveListTactic::Horizontal() ? one_line_budget : multi_line_budget;
    // non-default config: indent_style = Visual (indent = param_indent, no trailing separator)
    Indent indent2 = indent.block_indent_(config);
    SeparatorTactic trailing_separator = variadic ? SeparatorTactic::Never : trailing_comma(config);
    ListFormatting fmt = ListFormatting(Shape::legacy(budget, indent2), config)
                             .tactic(tactic)
                             .trailing_separator(trailing_separator)
                             .ends_with_newline(tactic.ends_with_newline(config.indent_style()))
                             .preserve_newline(true);
    return write_list(param_items, fmt);
}

static std::tuple<size_t, size_t, Indent> compute_budgets_for_params(const RewriteContext& context,
                                                                     std::string_view result, Indent indent,
                                                                     size_t ret_str_len, FnBraceStyle fn_brace_style,
                                                                     bool force_vertical_layout) {
    const Config& config = *context.config;
    // Try keeping everything on the same line.
    if (!contains(result, '\n') && !force_vertical_layout) {
        // 2 = `()`, 3 = `() `, space is before ret_string.
        size_t overhead = ret_str_len == 0 ? 2 : 3;
        size_t used_space = indent.width() + result.size() + ret_str_len + overhead;
        switch (fn_brace_style) {
        case FnBraceStyle::None: used_space += 1; break;      // 1 = `;`
        case FnBraceStyle::SameLine: used_space += 2; break;  // 2 = `{}`
        case FnBraceStyle::NextLine: break;
        }
        size_t one_line_budget = context.budget(used_space);

        if (one_line_budget > 0) {
            // non-default config: indent_style = Visual
            Indent ind = indent.block_indent_(config);
            return {one_line_budget, context.budget(ind.width() + 1), ind};
        }
    }

    // Didn't work. we must force vertical layout and put params on a newline.
    Indent new_indent = indent.block_indent_(config);
    // 1 = `,`
    size_t used_space = new_indent.width() + 1;
    return {0, context.budget(used_space), new_indent};
}

static FnBraceStyle newline_for_brace(const Config& config, const rust::WhereClause& where_clause) {
    size_t predicate_count = where_clause.predicates.size();

    if (config.where_single_line() && predicate_count == 1) return FnBraceStyle::SameLine;
    BraceStyle bs = brace_style(config);

    bool use_next_line =
        bs == BraceStyle::AlwaysNextLine || (bs == BraceStyle::SameLineWhere && predicate_count > 0);
    return use_next_line ? FnBraceStyle::NextLine : FnBraceStyle::SameLine;
}

static RewriteResult rewrite_generics(const RewriteContext& context, std::string_view ident,
                                      const rust::Generics& generics, Shape shape) {
    // FIXME: convert bounds to where-clauses where they get too big or if
    // there is a where-clause at all.

    if (generics.params.empty()) return std::string(ident);

    std::vector<OverflowableItem> params;
    params.reserve(generics.params.size());
    for (const rust::GenericParam* p : generics.params) params.push_back(RwNode::of(p));
    return rewrite_with_angle_brackets(context, ident, params, shape, generics.span);
}

static Expected<Shape> generics_shape_from_config(const Config& config, Shape shape, size_t offset, Span span) {
    (void)offset;  // non-default config: indent_style = Visual
    // 1 = ","
    return sub_width(shape.block().block_indent(config.tab_spaces()).with_max_width(config), 1, span);
}

// ---------------------------------------------------------------------------
// where clauses

static Expected<std::pair<std::string, bool>> rewrite_where_keyword(const RewriteContext& context,
                                                                   const std::vector<rust::WherePredicate*>& predicates,
                                                                   Span where_span, Shape shape,
                                                                   uint32_t span_end_before_where,
                                                                   WhereClauseOption where_clause_option);
static RewriteResult rewrite_bounds_on_where_clause(const RewriteContext& context,
                                                    const std::vector<rust::WherePredicate*>& predicates, Shape shape,
                                                    std::string_view terminator, std::optional<uint32_t> span_end,
                                                    WhereClauseOption where_clause_option, bool force_single_line);

static RewriteResult rewrite_where_clause_rfc_style(const RewriteContext& context,
                                                    const std::vector<rust::WherePredicate*>& predicates,
                                                    Span where_span, Shape shape, std::string_view terminator,
                                                    std::optional<uint32_t> span_end, uint32_t span_end_before_where,
                                                    WhereClauseOption where_clause_option) {
    const Config& config = *context.config;
    TRY(auto kw, rewrite_where_keyword(context, predicates, where_span, shape, span_end_before_where,
                                       where_clause_option));
    std::string where_keyword = std::move(kw.first);
    bool allow_single_line = kw.second;

    // 1 = `,`
    TRY(Shape cs0, block_left(shape.block().with_max_width(config), config.tab_spaces(), where_span));
    TRY(Shape clause_shape, sub_width(cs0, 1, where_span));
    bool force_single_line =
        config.where_single_line() && predicates.size() == 1 && !where_clause_option.veto_single_line_;

    TRY(std::string preds_str, rewrite_bounds_on_where_clause(context, predicates, clause_shape, terminator, span_end,
                                                              where_clause_option, force_single_line));

    // 6 = `where `
    std::string clause_sep;
    if ((allow_single_line && !contains(preds_str, '\n') && 6 + preds_str.size() <= shape.width) ||
        force_single_line) {
        clause_sep = " ";
    } else {
        clause_sep = clause_shape.indent.to_string_with_newline(config);
    }

    return where_keyword + clause_sep + preds_str;
}

static std::pair<Span, Span> missing_span_before_after_where(uint32_t before_item_span_end,
                                                             const std::vector<rust::WherePredicate*>& predicates,
                                                             Span where_span);
static Expected<std::pair<std::string, std::string>> rewrite_comments_before_after_where(const RewriteContext& context,
                                                                                         Span span_before_where,
                                                                                         Span span_after_where,
                                                                                         Shape shape);

/// Rewrite `where` and comment around it.
static Expected<std::pair<std::string, bool>> rewrite_where_keyword(const RewriteContext& context,
                                                                   const std::vector<rust::WherePredicate*>& predicates,
                                                                   Span where_span, Shape shape,
                                                                   uint32_t span_end_before_where,
                                                                   WhereClauseOption where_clause_option) {
    const Config& config = *context.config;
    Shape block_shape = shape.block().with_max_width(config);
    // 1 = `,`
    TRY(Shape cs0, block_left(block_shape, config.tab_spaces(), where_span));
    TRY(Shape clause_shape, sub_width(cs0, 1, where_span));

    auto comment_separator = [&](std::string_view comment, Shape s) -> std::string {
        if (comment.empty()) return std::string();
        return s.indent.to_string_with_newline(config);
    };

    auto spans = missing_span_before_after_where(span_end_before_where, predicates, where_span);
    TRY(auto comments, rewrite_comments_before_after_where(context, spans.first, spans.second, shape));
    const std::string& comment_before = comments.first;
    const std::string& comment_after = comments.second;

    std::string starting_newline;
    if (where_clause_option.snuggle_ == WhereClauseSpace::Space && comment_before.empty()) {
        starting_newline = " ";
    } else if (where_clause_option.snuggle_ == WhereClauseSpace::None) {
        starting_newline = "";
    } else {
        starting_newline = block_shape.indent.to_string_with_newline(config);
    }

    std::string newline_before_where = comment_separator(comment_before, shape);
    std::string newline_after_where = comment_separator(comment_after, clause_shape);
    std::string result =
        starting_newline + comment_before + newline_before_where + "where" + newline_after_where + comment_after;
    bool allow_single_line = where_clause_option.allow_single_line_ && comment_before.empty() && comment_after.empty();

    return std::make_pair(std::move(result), allow_single_line);
}

/// Rewrite bounds on a where clause.
static RewriteResult rewrite_bounds_on_where_clause(const RewriteContext& context,
                                                    const std::vector<rust::WherePredicate*>& predicates, Shape shape,
                                                    std::string_view terminator, std::optional<uint32_t> span_end,
                                                    WhereClauseOption where_clause_option, bool force_single_line) {
    const Config& config = *context.config;
    uint32_t span_start = spanned(predicates[0]).lo;
    // If we don't have the start of the next span, then use the end of the
    // predicates, but that means we miss comments.
    uint32_t end_of_preds = spanned(predicates.back()).hi;
    uint32_t span_end2 = span_end ? *span_end : end_of_preds;
    std::vector<ListItem> items = itemize_list(
        *context.snippet_provider, predicates.size(), terminator, ",",
        [&](size_t i) -> uint32_t { return spanned(predicates[i]).lo; },
        [&](size_t i) -> uint32_t { return spanned(predicates[i]).hi; },
        [&](size_t i) -> RewriteResult { return rewrite_result(context, predicates[i], shape); }, span_start,
        span_end2, false);
    SeparatorTactic comma_tactic = where_clause_option.suppress_comma_ || force_single_line ? SeparatorTactic::Never
                                                                                           : trailing_comma(config);

    // shape should be vertical only and only if we have `force_single_line` option enabled
    // and the number of items of the where-clause is equal to 1
    DefinitiveListTactic shape_tactic =
        force_single_line ? DefinitiveListTactic::Horizontal() : DefinitiveListTactic::Vertical();

    bool preserve_newline = config.style_edition() <= StyleEdition::E2021;

    ListFormatting fmt = ListFormatting(shape, config)
                             .tactic(shape_tactic)
                             .trailing_separator(comma_tactic)
                             .preserve_newline(preserve_newline);
    return write_list(items, fmt);
}

static RewriteResult rewrite_where_clause(const RewriteContext& context, const rust::WhereClause& where_clause,
                                          BraceStyle brace_style_, Shape shape, bool on_new_line,
                                          std::string_view terminator, std::optional<uint32_t> span_end,
                                          uint32_t span_end_before_where, WhereClauseOption where_clause_option) {
    const std::vector<rust::WherePredicate*>& predicates = where_clause.predicates;
    Span where_span = where_clause.span;

    if (predicates.empty()) return std::string();

    // indent_style is always Block: the rfc style. (The Visual-style layout that
    // follows in rustfmt is a non-default config path.)
    (void)brace_style_;
    (void)on_new_line;
    return rewrite_where_clause_rfc_style(context, predicates, where_span, shape, terminator, span_end,
                                          span_end_before_where, where_clause_option);
}

static std::pair<Span, Span> missing_span_before_after_where(uint32_t before_item_span_end,
                                                             const std::vector<rust::WherePredicate*>& predicates,
                                                             Span where_span) {
    Span missing_span_before = mk_sp(before_item_span_end, where_span.lo);
    // 5 = `where`
    uint32_t pos_after_where = where_span.lo + 5;
    Span missing_span_after = mk_sp(pos_after_where, spanned(predicates[0]).lo);
    return {missing_span_before, missing_span_after};
}

static Expected<std::pair<std::string, std::string>> rewrite_comments_before_after_where(const RewriteContext& context,
                                                                                         Span span_before_where,
                                                                                         Span span_after_where,
                                                                                         Shape shape) {
    TRY(std::string before_comment, rewrite_missing_comment(span_before_where, shape, context));
    TRY(std::string after_comment,
        rewrite_missing_comment(span_after_where, shape.block_indent(context.config->tab_spaces()), context));
    return std::make_pair(std::move(before_comment), std::move(after_comment));
}

static std::string format_header(const RewriteContext& context, std::string_view item_name, rust::Ident ident,
                                 const rust::Visibility& vis, Indent offset) {
    std::string result;
    Shape shape = Shape::indented(offset, *context.config);

    result += trim(format_visibility(context, vis));

    // Check for a missing comment between the visibility and the item name.
    uint32_t after_vis = vis.span.hi;
    if (std::optional<uint32_t> before_item_name =
            context.snippet_provider->opt_span_before(mk_sp(vis.span.lo, ident.span.hi), trim(item_name))) {
        Span missing_span = mk_sp(after_vis, *before_item_name);
        RewriteResult rwc = combine_strs_with_missing_comments(context, result, item_name, missing_span, shape,
                                                               /* allow_extend */ true);
        if (rwc) result = *rwc;
    }

    result += rewrite_ident(context, ident);

    return result;
}

static Opt format_generics(const RewriteContext& context, const rust::Generics& generics, BraceStyle brace_style_,
                           BracePos brace_pos, Indent offset, Span span, size_t used_width) {
    const Config& config = *context.config;
    Shape shape = Shape::legacy(context.budget(used_width + offset.width()), offset);
    TRY_OPT(std::string result, rewrite_generics(context, "", generics, shape));

    // If the generics are not parameterized then generics.span.hi() == 0,
    // so we use span.lo(), which is the position after `struct Foo`.
    uint32_t span_end_before_where = !generics.params.empty() ? generics.span.hi : span.lo;
    bool same_line_brace;
    Opt missed_comments;
    if (!generics.where_clause.predicates.empty()) {
        size_t budget = context.budget(last_line_used_width(result, offset.width()));
        WhereClauseOption option = WhereClauseOption::snuggled(result);
        if (brace_pos == BracePos::None) option.suppress_comma_ = true;
        TRY_OPT(std::string where_clause_str,
                rewrite_where_clause(context, generics.where_clause, brace_style_,
                                     Shape::legacy(budget, offset.block_only()), true, "{", span.hi,
                                     span_end_before_where, option));
        result += where_clause_str;
        same_line_brace = brace_pos == BracePos::ForceSameLine || brace_style_ == BraceStyle::PreferSameLine;
        // missed comments are taken care of in #rewrite_where_clause
        missed_comments = std::nullopt;
    } else {
        same_line_brace = brace_pos == BracePos::ForceSameLine ||
                          ((contains(result, '\n') && brace_style_ == BraceStyle::PreferSameLine) ||
                           brace_style_ != BraceStyle::AlwaysNextLine) ||
                          trimmed_last_line_width(result) == 1;
        uint32_t hi = brace_pos == BracePos::None ? span.hi : context.snippet_provider->span_before_last(span, "{");
        missed_comments = rewrite_missing_comment(mk_sp(span_end_before_where, hi), shape, context).opt();
    }
    // add missing comments
    bool missed_line_comments = false;
    if (missed_comments && !missed_comments->empty()) {
        bool is_block = is_last_comment_block(*missed_comments);
        std::string_view sep = is_block ? " " : "\n";
        result += sep;
        result += *missed_comments;
        missed_line_comments = !is_block;
    }
    if (brace_pos == BracePos::None) return result;
    size_t total_used_width = last_line_used_width(result, used_width);
    size_t remaining_budget = context.budget(total_used_width);
    // If the same line brace if forced, it indicates that we are rewriting an item with empty body,
    // and hence we take the closer into account as well for one line budget.
    // We assume that the closer has the same length as the opener.
    size_t overhead = brace_pos == BracePos::ForceSameLine ? 3 /* ` {}` */ : 2 /* ` {` */;
    bool forbid_same_line_brace = missed_line_comments || overhead > remaining_budget;
    if (!forbid_same_line_brace && same_line_brace) {
        result += ' ';
    } else {
        result += '\n';
        result += offset.block_only().to_string(config);
    }
    result += '{';

    return result;
}

// ---------------------------------------------------------------------------
// impl Rewrite for ast::ForeignItem

static RewriteResult rewrite_foreign_item(const RewriteContext& context, const rust::Item* self, Shape shape) {
    TRY(std::string attrs_str, rewrite_attrs(context, self->attrs, shape));
    // Drop semicolon or it will be interpreted as comment.
    // FIXME: this may be a faulty span from libsyntax.
    Span span = mk_sp(self->span.lo, self->span.hi - 1);

    std::string item_str;
    switch (self->kind) {
    case rust::ItemKind::Fn: {
        if (self->body) {
            FmtVisitor visitor = FmtVisitor::from_context(context);
            visitor.block_indent = shape.indent;
            visitor.last_pos = self->span.lo;
            rust::AttrVec inner_attrs = inner_attributes(self->attrs);
            visitor.visit_fn(self, self->span, &inner_attrs);
            item_str = visitor.buffer;
        } else {
            TRY(auto base, rewrite_fn_base(context, shape.indent, self->ident, FnSig::from_item(self), span,
                                           FnBraceStyle::None));
            item_str = std::get<0>(base) + ";";
        }
        break;
    }
    case rust::ItemKind::Static: {
        // FIXME(#21): we're dropping potential comments in between the
        // function kw here.
        std::string vis = format_visibility(context, self->vis);
        std::string_view safety = format_safety(self->safety);
        std::string_view mut_str = format_mutability(self->mutbl);
        std::string prefix = vis + std::string(safety) + "static " + std::string(mut_str) +
                             std::string(rewrite_ident(context, self->ident)) + ":";
        // 1 = ;
        TRY(Shape s1, sub_width(shape, 1, self->ty->span));
        TRY(item_str, rewrite_assign_rhs(context, prefix, RwNode::of(self->ty), RhsAssignKind::ty(), s1));
        item_str += ";";
        break;
    }
    case rust::ItemKind::TyAlias: {
        TRY(item_str, rewrite_type_alias(self, context, shape.indent, ItemVisitorKind::ForeignItem, self->span));
        break;
    }
    case rust::ItemKind::MacCall: {
        TRY(item_str, rewrite_macro(self->mac, context, shape, MacroPosition::Item, self->span));
        break;
    }
    default: panic("rewrite_foreign_item: unexpected foreign item kind", self->span.lo);
    }

    Span missing_span = self->attrs.empty() ? mk_sp(self->span.lo, self->span.lo)
                                            : mk_sp(self->attrs.back().span.hi, self->span.lo);
    return combine_strs_with_missing_comments(context, attrs_str, item_str, missing_span, shape, false);
}

// ---------------------------------------------------------------------------
// mod / extern crate

/// Rewrite the attributes of an item.
static RewriteResult rewrite_attrs(const RewriteContext& context, const rust::Item* item, std::string_view item_str,
                                   Shape shape) {
    rust::AttrVec attrs = filter_inline_attrs(item->attrs, spanned(item));
    TRY(std::string attrs_str, rewrite_attrs(context, attrs, shape));

    Span missed_span = attrs.empty() ? mk_sp(item->span.lo, item->span.lo) : mk_sp(attrs.back().span.hi, item->span.lo);

    bool allow_extend = false;
    if (attrs.size() == 1) {
        size_t line_len = attrs_str.size() + 1 + item_str.size();
        allow_extend = !attrs.front().is_doc_comment && context.config->inline_attribute_width() >= line_len;
    }

    return combine_strs_with_missing_comments(context, attrs_str, item_str, missed_span, shape, allow_extend);
}

/// Rewrite an inline mod.
/// The given shape is used to format the mod's attributes.
RewriteResult rewrite_mod(const RewriteContext& context, const rust::Item* item, Shape attrs_shape) {
    std::string result;
    result += format_visibility(context, item->vis);
    result += "mod ";
    result += rewrite_ident(context, item->ident);
    result += ';';
    return rewrite_attrs(context, item, result, attrs_shape);
}

/// Rewrite `extern crate foo;`.
/// The given shape is used to format the extern crate's attributes.
RewriteResult rewrite_extern_crate(const RewriteContext& context, const rust::Item* item, Shape attrs_shape) {
    if (!is_extern_crate(item)) panic("assertion failed: is_extern_crate(item)", item->span.lo);
    std::string_view new_str = context.snippet(item->span);
    std::string item_str;
    if (contains_comment(new_str)) {
        item_str = std::string(new_str);
    } else {
        std::string no_whitespace = split_whitespace_join(new_str);
        // Regex::new(r"\s;").replace(..): the first match only (only ' ' is left).
        size_t p = no_whitespace.find(" ;");
        if (p != std::string::npos) no_whitespace.replace(p, 2, ";");
        item_str = std::move(no_whitespace);
    }
    return rewrite_attrs(context, item, item_str, attrs_shape);
}

/// Returns `true` for `mod foo;`, false for `mod foo { .. }`.
bool is_mod_decl(const rust::Item* item) { return !(item->kind == rust::ItemKind::Mod && item->mod_inline); }

bool is_use_item(const rust::Item* item) { return item->kind == rust::ItemKind::Use; }

bool is_extern_crate(const rust::Item* item) { return item->kind == rust::ItemKind::ExternCrate; }

#undef TRY
#undef TRY_OPT
#undef RF_CAT
#undef RF_CAT2

}  // namespace rustfmt
