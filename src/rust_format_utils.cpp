// rust_format_utils.cpp -- rustfmt's utils.rs, shape.rs, spanned.rs,
// source_map.rs and skip.rs, plus the Rust `str` helpers the port leans on.
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include <algorithm>

#include "rust_format_internal.h"

namespace rustfmt {

namespace {
#include "rust_format_width.inc"

uint32_t next_char(std::string_view s, size_t& i) {
    size_t len = 1;
    uint32_t c = rust::decode_utf8(s, i, len);
    i += len;
    return c;
}

// char::is_whitespace (Unicode White_Space).
bool is_ws(uint32_t c) {
    return (c >= 0x9 && c <= 0xD) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

size_t char_width(uint32_t c) {
    if (c < 0x20 || c == 0x7F) return 0;
    if (c < 0x80) return 1;
    size_t lo = 0, hi = sizeof(kWidthRanges) / sizeof(kWidthRanges[0]);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < kWidthRanges[mid].lo) {
            hi = mid;
        } else if (c > kWidthRanges[mid].hi) {
            lo = mid + 1;
        } else {
            return kWidthRanges[mid].w;
        }
    }
    return 1;
}
}  // namespace

[[noreturn]] void panic(const std::string& what, uint32_t at) { throw FormatPanic(what, at); }

DepthGuard::DepthGuard(FormatState& st) : st_(st) {
    st_.depth++;
    if (st_.depth > st_.max_depth) {
        st_.depth--;
        panic("nesting too deep");
    }
    if (st_.work_budget != 0 && ++st_.work > st_.work_budget) {
        st_.depth--;
        panic("formatter work budget exceeded");
    }
}

// ---------------------------------------------------------------------------
// String helpers.

std::string_view trim_start(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t j = i;
        if (!is_ws(next_char(s, j))) break;
        i = j;
    }
    return s.substr(i);
}

std::string_view trim_end(std::string_view s) {
    size_t end = s.size();
    while (end > 0) {
        // Step back to the start of the last code point.
        size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
        size_t j = start;
        if (!is_ws(next_char(s, j))) break;
        end = start;
    }
    return s.substr(0, end);
}

std::string_view trim(std::string_view s) { return trim_end(trim_start(s)); }
bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }
bool contains(std::string_view s, char c) { return s.find(c) != std::string_view::npos; }

std::vector<std::string_view> lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        std::string_view line = nl == std::string_view::npos ? s.substr(start) : s.substr(start, nl - start);
        if (nl != std::string_view::npos && !line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.push_back(line);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string repeat(std::string_view s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; i++) out += s;
    return out;
}

size_t unicode_str_width(std::string_view s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            w += (b < 0x20 || b == 0x7F) ? 0 : 1;
            i++;
            continue;
        }
        w += char_width(next_char(s, i));
    }
    return w;
}

// ---------------------------------------------------------------------------
// shape.rs

Indent Indent::from_width(const Config& /*config*/, size_t width) { return Indent(width, 0); }

Indent Indent::block_indent_(const Config& config) const {
    return Indent(block_indent + config.tab_spaces(), alignment);
}

Indent Indent::block_unindent(const Config& config) const {
    if (block_indent < config.tab_spaces()) return Indent(block_indent, 0);
    return Indent(block_indent - config.tab_spaces(), alignment);
}

std::string Indent::to_string_inner(const Config& /*config*/, size_t offset) const {
    // hard_tabs = false: no tabs, width() spaces.
    std::string out;
    if (offset == 0) out += '\n';
    out.append(width(), ' ');
    return out;
}

std::string Indent::to_string(const Config& config) const { return to_string_inner(config, 1); }
std::string Indent::to_string_with_newline(const Config& config) const { return to_string_inner(config, 0); }

Shape Shape::visual_indent(size_t delta) const {
    size_t alignment = offset + delta;
    return Shape{width, Indent(indent.block_indent, alignment), alignment};
}

Shape Shape::block_indent(size_t delta) const {
    if (indent.alignment == 0) return Shape{width, Indent(indent.block_indent + delta, 0), 0};
    return Shape{width, indent + delta, indent.alignment + delta};
}

Shape Shape::comment(const Config& config) const {
    Shape s = *this;
    s.width = std::min(width, sat_sub(config.comment_width(), indent.width()));
    return s;
}

std::string Shape::to_string_with_newline(const Config& config) const {
    Indent offset_indent = indent;
    offset_indent.alignment = offset;
    return offset_indent.to_string_inner(config, 0);
}

// ---------------------------------------------------------------------------
// rewrite.rs / visitor.rs SnippetProvider / source_map.rs

std::string_view RewriteContext::snippet(Span span) const {
    auto s = snippet_provider->span_to_snippet(span);
    if (!s) panic("bad span", span.lo);
    return *s;
}

std::optional<uint32_t> SnippetProvider::opt_span_before(Span original, std::string_view needle) const {
    auto snippet = span_to_snippet(original);
    if (!snippet) return std::nullopt;
    auto offset = find_uncommented(*snippet, needle);
    if (!offset) return std::nullopt;
    return original.lo + static_cast<uint32_t>(*offset);
}

std::optional<uint32_t> SnippetProvider::opt_span_after(Span original, std::string_view needle) const {
    auto pos = opt_span_before(original, needle);
    if (!pos) return std::nullopt;
    return *pos + static_cast<uint32_t>(needle.size());
}

uint32_t SnippetProvider::span_after(Span original, std::string_view needle) const {
    auto pos = opt_span_after(original, needle);
    if (!pos) panic("bad span: `" + std::string(needle) + "`", original.lo);
    return *pos;
}

uint32_t SnippetProvider::span_before(Span original, std::string_view needle) const {
    auto pos = opt_span_before(original, needle);
    if (!pos) panic("bad span: `" + std::string(needle) + "`", original.lo);
    return *pos;
}

uint32_t SnippetProvider::span_after_last(Span original, std::string_view needle) const {
    auto snippet = span_to_snippet(original);
    if (!snippet) panic("bad span", original.lo);
    size_t offset = 0;
    while (auto add = find_uncommented(snippet->substr(offset), needle)) offset += *add + needle.size();
    return original.lo + static_cast<uint32_t>(offset);
}

uint32_t SnippetProvider::span_before_last(Span original, std::string_view needle) const {
    auto snippet = span_to_snippet(original);
    if (!snippet) panic("bad span", original.lo);
    size_t offset = 0;
    while (auto add = find_uncommented(snippet->substr(offset), needle)) offset += *add + needle.size();
    // `offset as u32 - 1`: wraps when nothing was found, as in rustfmt.
    return original.lo + static_cast<uint32_t>(offset) - 1;
}

// ---------------------------------------------------------------------------
// skip.rs

void SkipNameContext::update(const SkipNameContext& other) {
    if (all) return;
    if (other.all) {
        skip_all();
        return;
    }
    values.insert(other.values.begin(), other.values.end());
}

namespace {
std::vector<std::string> get_skip_names(const rust::ParseResult& pr, std::string_view kind,
                                        const rust::AttrVec& attrs) {
    std::vector<std::string> skip_names;
    for (const rust::Attribute& attr : attrs) {
        if (attr.is_doc_comment || attr.path == nullptr) continue;  // meta_item_list() is None
        const auto& segs = attr.path->segments;
        if (segs.size() != 3 || segs[0].ident.name != "rustfmt" || segs[1].ident.name != "skip" ||
            segs[2].ident.name != kind || segs[2].args != nullptr)
            continue;
        if (attr.args.kind != rust::AttrArgs::Delimited || attr.args.delim.delim != rust::DelimKind::Paren) continue;
        // meta_item_list(): `name, name, ...`; ident() is Some for a one-segment path word.
        // Anything that is not a meta-item list makes the whole list None.
        std::vector<std::string> names;
        bool ok = true;
        uint32_t i = attr.args.delim.tok_lo, hi = attr.args.delim.tok_hi;
        while (i < hi && ok) {
            const rust::Token& t = pr.tokens[i];
            if (t.kind == rust::Tok::Ident) {
                // A path (a::b) is a meta item without ident(); a word is pushed.
                uint32_t j = i + 1;
                bool single = true;
                while (j + 1 < hi && pr.tokens[j].kind == rust::Tok::PathSep && pr.tokens[j + 1].kind == rust::Tok::Ident) {
                    single = false;
                    j += 2;
                }
                if (j < hi && pr.tokens[j].kind != rust::Tok::Comma) {
                    ok = false;  // `name = lit` / `name(..)`: real meta items, but not names; keep simple
                    break;
                }
                if (single) names.push_back(t.raw ? "r#" + std::string(t.sym) : std::string(t.sym));
                i = j;
            } else if (t.kind == rust::Tok::Literal) {
                i++;
            } else {
                ok = false;
                break;
            }
            if (i < hi) {
                if (pr.tokens[i].kind != rust::Tok::Comma) {
                    ok = false;
                    break;
                }
                i++;
            }
        }
        if (ok) skip_names.insert(skip_names.end(), names.begin(), names.end());
    }
    return skip_names;
}
}  // namespace

void SkipContext::update_with_attrs(const rust::ParseResult& pr, const rust::AttrVec& attrs) {
    macros.extend(get_skip_names(pr, "macros", attrs));
    attributes.extend(get_skip_names(pr, "attributes", attrs));
}

bool is_skip_attr(const std::vector<rust::PathSegment>& segments) {
    if (segments.size() < 2 || segments[0].ident.name != "rustfmt") return false;
    switch (segments.size()) {
    case 2: return segments[1].ident.name == "skip";
    case 3:
        return segments[1].ident.name == "skip" && segments[2].args == nullptr &&
               (segments[2].ident.name == "macros" || segments[2].ident.name == "attributes");
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// spanned.rs

namespace {
Span with_attrs_lo_hi(const rust::AttrVec& attrs, uint32_t lo, uint32_t hi) {
    for (const rust::Attribute& a : attrs)
        if (a.style == rust::AttrStyle::Outer) return mk_sp(a.span.lo, hi);
    return mk_sp(lo, hi);
}
}  // namespace

Span spanned(const rust::Item* x) { return with_attrs_lo_hi(x->attrs, x->span.lo, x->span.hi); }
Span spanned(const rust::Expr* x) { return with_attrs_lo_hi(x->attrs, x->span.lo, x->span.hi); }
Span spanned(const rust::ExprField* x) { return with_attrs_lo_hi(x->attrs, x->span.lo, x->span.hi); }
Span spanned(const rust::Local* x) { return with_attrs_lo_hi(x->attrs, x->span.lo, x->span.hi); }
Span spanned(const rust::WherePredicate* x) { return with_attrs_lo_hi(x->attrs, x->span.lo, x->span.hi); }

Span spanned(const rust::Stmt* x) {
    switch (x->kind) {
    case rust::StmtKind::Let: return mk_sp(spanned(x->local).lo, x->span.hi);
    case rust::StmtKind::Item: return mk_sp(spanned(x->item).lo, x->span.hi);
    case rust::StmtKind::Expr:
    case rust::StmtKind::Semi: return mk_sp(spanned(x->expr).lo, x->span.hi);
    case rust::StmtKind::MacCall:
        if (x->mac->attrs.empty()) return x->span;
        return mk_sp(x->mac->attrs[0].span.lo, x->span.hi);
    case rust::StmtKind::Empty: return x->span;
    }
    return x->span;
}

Span spanned(const rust::Pat* x) { return x->span; }
Span spanned(const rust::Ty* x) { return x->span; }

Span spanned(const rust::Arm* x) {
    uint32_t lo = x->attrs.empty() ? x->pat->span.lo : x->attrs[0].span.lo;
    uint32_t hi = x->body ? x->body->span.hi : x->pat->span.hi;
    return with_attrs_lo_hi(x->attrs, lo, hi);
}

Span spanned(const rust::Param* x) {
    if (is_named_param(x)) return mk_sp(span_lo_for_param(x), x->ty->span.hi);
    return x->ty->span;
}

Span spanned(const rust::GenericParam* x) {
    uint32_t lo;
    if (!x->attrs.empty()) {
        lo = x->attrs[0].span.lo;
    } else if (x->kind == rust::GenericParam::Const) {
        lo = x->const_span.lo;
    } else {
        lo = x->ident.span.lo;
    }
    uint32_t hi = x->bounds.empty() ? x->ident.span.hi : x->bounds.back().span().hi;
    uint32_t ty_hi = hi;
    if (x->kind == rust::GenericParam::Type && x->default_ty) ty_hi = spanned(x->default_ty).hi;
    if (x->kind == rust::GenericParam::Const && x->const_ty) ty_hi = spanned(x->const_ty).hi;
    return mk_sp(lo, std::max(hi, ty_hi));
}

Span spanned(const rust::FieldDef* x) { return with_attrs_lo_hi(x->attrs, x->span.lo, x->ty->span.hi); }

Span spanned(const rust::FnRetTy* x) { return x->ty ? x->ty->span : x->span; }

Span spanned(const rust::GenericArg* x) {
    switch (x->kind) {
    case rust::GenericArg::Lifetime: return x->lifetime.ident.span;
    case rust::GenericArg::Type: return spanned(x->ty);
    case rust::GenericArg::Const: return spanned(x->konst.value);
    }
    return Span();
}

Span spanned(const rust::GenericBound* x) { return x->span(); }

Span spanned(const rust::PreciseCapturingArg* x) { return x->is_lifetime ? x->lifetime.ident.span : x->path->span; }

// ---------------------------------------------------------------------------
// utils.rs

std::string_view rewrite_ident(const RewriteContext& ctx, const rust::Ident& ident) { return ctx.snippet(ident.span); }

size_t extra_offset(std::string_view text, Shape shape) {
    size_t idx = text.rfind('\n');
    if (idx == std::string_view::npos) return text.size();
    return sat_sub(text.size(), idx + 1 + shape.used_width());
}

bool path_is_global(const rust::Path* path) {
    return path && !path->segments.empty() && path->segments[0].ident.name == "{{root}}";
}

std::string path_to_string(const RewriteContext& ctx, const rust::Path* path) {
    std::string out;
    for (size_t i = 0; i < path->segments.size(); i++) {
        const rust::PathSegment& seg = path->segments[i];
        if (i) out += "::";
        if (seg.ident.name == "{{root}}") continue;
        out += seg.ident.raw ? "r#" : "";
        out += seg.ident.name;
        if (seg.args) out += ctx.snippet(seg.args->span);
    }
    return out;
}

bool is_same_visibility(const RewriteContext& ctx, const rust::Visibility& a, const rust::Visibility& b) {
    using K = rust::Visibility::Kind;
    if (a.kind == K::Restricted && b.kind == K::Restricted)
        return path_to_string(ctx, a.path) == path_to_string(ctx, b.path);
    return (a.kind == K::Public && b.kind == K::Public) || (a.kind == K::Inherited && b.kind == K::Inherited);
}

std::string format_visibility(const RewriteContext& ctx, const rust::Visibility& vis) {
    switch (vis.kind) {
    case rust::Visibility::Public: return "pub ";
    case rust::Visibility::Inherited: return "";
    case rust::Visibility::Restricted: {
        std::vector<std::string> segs;
        const auto& segments = vis.path->segments;
        size_t start = path_is_global(vis.path) ? 1 : 0;
        for (size_t i = start; i < segments.size(); i++) segs.emplace_back(rewrite_ident(ctx, segments[i].ident));
        std::string path = join(segs, "::");
        const char* in_str = (path == "crate" || path == "self" || path == "super") ? "" : "in ";
        return "pub(" + std::string(in_str) + path + ") ";
    }
    }
    return "";
}

std::string format_impl_restriction(const RewriteContext& ctx, const rust::ImplRestriction& r) {
    if (!r.restricted) return "";
    std::vector<std::string> segs;
    const auto& segments = r.path->segments;
    size_t start = path_is_global(r.path) ? 1 : 0;
    for (size_t i = start; i < segments.size(); i++) segs.emplace_back(rewrite_ident(ctx, segments[i].ident));
    return "impl(" + std::string(r.shorthand ? "" : "in ") + join(segs, "::") + ") ";
}

const char* format_coro(const rust::CoroutineKind& k) {
    switch (k.kind) {
    case rust::CoroutineKind::Async: return "async ";
    case rust::CoroutineKind::Gen: return "gen ";
    case rust::CoroutineKind::AsyncGen: return "async gen ";
    case rust::CoroutineKind::None: return "";
    }
    return "";
}

const char* format_constness(const rust::Constness& c) { return c.yes ? "const " : ""; }
const char* format_constness_right(const rust::Constness& c) { return c.yes ? " const" : ""; }

const char* format_defaultness(const rust::Defaultness& d) {
    switch (d.kind) {
    case rust::Defaultness::Implicit: return "";
    case rust::Defaultness::Default: return "default ";
    case rust::Defaultness::Final: return "final ";
    }
    return "";
}

const char* format_safety(const rust::Safety& s) {
    switch (s.kind) {
    case rust::Safety::Unsafe: return "unsafe ";
    case rust::Safety::Safe: return "safe ";
    case rust::Safety::Default: return "";
    }
    return "";
}

const char* format_auto(bool is_auto) { return is_auto ? "auto " : ""; }
const char* format_mutability(rust::Mutability m) { return m == rust::Mutability::Mut ? "mut " : ""; }

std::pair<const char*, const char*> format_pinnedness_and_mutability(rust::Pinnedness p, rust::Mutability m) {
    bool pinned = p == rust::Pinnedness::Pinned, mut = m == rust::Mutability::Mut;
    if (pinned) return {"pin ", mut ? "mut " : "const "};
    return {"", mut ? "mut " : ""};
}

std::string format_extern(const RewriteContext& ctx, const rust::Extern& ext, bool explicit_abi) {
    switch (ext.kind) {
    case rust::Extern::None: return "";
    case rust::Extern::Implicit: return explicit_abi ? "extern \"C\" " : "extern ";
    case rust::Extern::Explicit: {
        // abi.symbol_unescaped: the ABI string's contents (ABI strings have no escapes in practice).
        (void)ctx;
        if (ext.abi.symbol == "C" && !explicit_abi) return "extern ";
        return "extern \"" + std::string(ext.abi.symbol) + "\" ";
    }
    }
    return "";
}

rust::AttrVec filter_attributes(const rust::AttrVec& attrs, rust::AttrStyle style) {
    rust::AttrVec out;
    for (const rust::Attribute& a : attrs)
        if (a.style == style) out.push_back(a);
    return out;
}
rust::AttrVec inner_attributes(const rust::AttrVec& attrs) { return filter_attributes(attrs, rust::AttrStyle::Inner); }
rust::AttrVec outer_attributes(const rust::AttrVec& attrs) { return filter_attributes(attrs, rust::AttrStyle::Outer); }

bool is_single_line(std::string_view s) { return !contains(s, '\n'); }

bool first_line_contains_single_line_comment(std::string_view s) {
    auto ls = lines(s);
    return !ls.empty() && contains(ls.front(), "//");
}

bool last_line_contains_single_line_comment(std::string_view s) {
    auto ls = lines(s);
    return !ls.empty() && contains(ls.back(), "//");
}

bool is_attributes_extendable(std::string_view attrs_str) {
    return !contains(attrs_str, '\n') && !last_line_contains_single_line_comment(attrs_str);
}

size_t first_line_width(std::string_view s) {
    size_t nl = s.find('\n');
    return unicode_str_width(nl == std::string_view::npos ? s : s.substr(0, nl));
}

size_t last_line_width(std::string_view s) {
    size_t nl = s.rfind('\n');
    return unicode_str_width(nl == std::string_view::npos ? s : s.substr(nl + 1));
}

size_t last_line_used_width(std::string_view s, size_t offset) {
    if (contains(s, '\n')) return last_line_width(s);
    return offset + unicode_str_width(s);
}

size_t trimmed_last_line_width(std::string_view s) {
    size_t nl = s.rfind('\n');
    return unicode_str_width(trim(nl == std::string_view::npos ? s : s.substr(nl + 1)));
}

bool last_line_extendable(std::string_view s) {
    if (ends_with(s, "\"#")) return true;
    // Walk the chars backwards.
    size_t end = s.size();
    while (end > 0) {
        size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
        size_t j = start;
        uint32_t c = next_char(s, j);
        end = start;
        if (c == '(' || c == ')' || c == ']' || c == '}' || c == '?' || c == '>') continue;
        if (c == '\n') break;
        if (is_ws(c)) continue;
        return false;
    }
    return true;
}

namespace {
bool is_skip(const RewriteContext& ctx, const MetaItem* mi);
bool is_skip_nested(const RewriteContext& ctx, const MetaItemInner& inner) {
    return !inner.is_lit && is_skip(ctx, inner.mi);
}
bool is_skip(const RewriteContext& ctx, const MetaItem* mi) {
    switch (mi->kind) {
    case MetaItem::Word: {
        std::string p = path_to_string(ctx, mi->path);
        return p == "rustfmt::skip" || p == "rustfmt_skip";
    }
    case MetaItem::List:
        return mi->path->segments.size() == 1 && mi->path->segments[0].ident.name == "cfg_attr" &&
               mi->list.size() == 2 && is_skip_nested(ctx, mi->list[1]);
    case MetaItem::NameValue: return false;
    }
    return false;
}
}  // namespace

bool contains_skip(const RewriteContext& ctx, const rust::AttrVec& attrs) {
    for (const rust::Attribute& a : attrs) {
        if (a.is_doc_comment) continue;
        const MetaItem* mi = parse_meta(ctx, a);
        if (mi && is_skip(ctx, mi)) return true;
    }
    return false;
}

bool semicolon_for_expr(const RewriteContext& ctx, const rust::Expr* expr) {
    if (ctx.is_macro_def) return false;
    switch (expr->kind) {
    case rust::ExprKind::Ret:
    case rust::ExprKind::Continue:
    case rust::ExprKind::Break: return ctx.config->trailing_semicolon();
    default: return false;
    }
}

bool semicolon_for_stmt(const RewriteContext& ctx, const rust::Stmt* stmt, bool is_last_expr) {
    switch (stmt->kind) {
    case rust::StmtKind::Semi:
        switch (stmt->expr->kind) {
        case rust::ExprKind::While:
        case rust::ExprKind::Loop:
        case rust::ExprKind::ForLoop: return false;
        case rust::ExprKind::Break:
        case rust::ExprKind::Continue:
        case rust::ExprKind::Ret: return ctx.config->trailing_semicolon() || !is_last_expr;
        default: return true;
        }
    case rust::StmtKind::Expr: return false;
    default: return true;
    }
}

const rust::Expr* stmt_expr(const rust::Stmt* stmt) {
    return stmt->kind == rust::StmtKind::Expr ? stmt->expr : nullptr;
}

std::pair<size_t, size_t> count_lf_crlf(std::string_view input) {
    size_t lf = 0, crlf = 0;
    bool is_crlf = false;
    for (char c : input) {
        if (c == '\r') {
            is_crlf = true;
        } else if (c == '\n' && is_crlf) {
            crlf++;
        } else if (c == '\n') {
            lf++;
        } else {
            is_crlf = false;
        }
    }
    return {lf, crlf};
}

size_t count_newlines(std::string_view input) {
    return static_cast<size_t>(std::count(input.begin(), input.end(), '\n'));
}

Opt wrap_str(std::string s, size_t max_width, Shape shape) {
    if (filtered_str_fits(s, max_width, shape)) return s;
    return std::nullopt;
}

bool filtered_str_fits(std::string_view snippet_in, size_t max_width, Shape shape) {
    std::string snippet = filter_normal_code(snippet_in);
    if (!snippet.empty()) {
        if (first_line_width(snippet) > shape.width) return false;
        if (is_single_line(snippet)) return true;
        auto ls = lines(snippet);
        for (size_t i = 1; i < ls.size(); i++)
            if (unicode_str_width(ls[i]) > max_width) return false;
        if (last_line_width(snippet) > shape.used_width() + shape.width) return false;
    }
    return true;
}

const char* colon_spaces(const Config& /*config*/) { return ": "; }

const rust::Expr* left_most_sub_expr(const rust::Expr* e) {
    for (;;) {
        switch (e->kind) {
        case rust::ExprKind::Call:
        case rust::ExprKind::Binary:
        case rust::ExprKind::Cast:
        case rust::ExprKind::Type:
        case rust::ExprKind::Assign:
        case rust::ExprKind::AssignOp:
        case rust::ExprKind::Field:
        case rust::ExprKind::Index:
        case rust::ExprKind::Try: e = e->lhs; continue;
        case rust::ExprKind::Range:
            if (e->lhs) {
                e = e->lhs;
                continue;
            }
            return e;
        default: return e;
        }
    }
}

bool starts_with_newline(std::string_view s) { return starts_with(s, "\n") || starts_with(s, "\r\n"); }

bool first_line_ends_with(std::string_view s, char c) {
    auto ls = lines(s);
    return !ls.empty() && !ls.front().empty() && ls.front().back() == c;
}

bool is_block_expr(const RewriteContext& ctx, const rust::Expr* expr, std::string_view repr) {
    using K = rust::ExprKind;
    switch (expr->kind) {
    case K::MacCall:
    case K::FormatArgs:
    case K::Call:
    case K::MethodCall:
    case K::Array:
    case K::Struct:
    case K::While:
    case K::If:
    case K::Block:
    case K::ConstBlock:
    case K::Gen:
    case K::Loop:
    case K::ForLoop:
    case K::TryBlock:
    case K::Match: return contains(repr, '\n');
    case K::Paren:
    case K::Unary:
    case K::Try: return is_block_expr(ctx, expr->lhs, repr);
    case K::Binary:
    case K::Index: return is_block_expr(ctx, expr->rhs, repr);
    case K::Yield:
        if (expr->yield_kind == rust::YieldKind::Prefix && expr->lhs) return is_block_expr(ctx, expr->lhs, repr);
        return false;
    case K::Closure: return is_block_expr(ctx, expr->closure->body, repr);
    case K::Lit: return contains(repr, '\n') && trimmed_last_line_width(repr) <= ctx.config->tab_spaces();
    default: return false;
    }
}

std::string remove_trailing_white_spaces(std::string_view text) {
    std::string buffer;
    buffer.reserve(text.size());
    std::string space_buffer;
    for (const ClassifiedChar& cc : char_classes(text)) {
        std::string_view ch = text.substr(cc.pos, cc.len);
        if (cc.ch == '\n') {
            if (cc.kind == FullCodeCharKind::InString) buffer += space_buffer;
            space_buffer.clear();
            buffer += '\n';
        } else if (is_ws(cc.ch)) {
            space_buffer += ch;
        } else {
            if (!space_buffer.empty()) {
                buffer += space_buffer;
                space_buffer.clear();
            }
            buffer += ch;
        }
    }
    return buffer;
}

namespace {
size_t get_prefix_space_width(const Config& config, std::string_view s) {
    size_t width = 0;
    for (char c : s) {
        if (c == ' ') {
            width += 1;
        } else if (c == '\t') {
            width += config.tab_spaces();
        } else {
            return width;
        }
    }
    return width;
}
}  // namespace

Opt trim_left_preserve_layout(std::string_view orig, Indent indent, const Config& config) {
    auto ls = line_classes(orig);
    if (ls.empty()) return std::nullopt;
    std::string first_line(trim_end(ls[0].second));
    struct TL {
        bool trimmed;
        std::string line;
        std::optional<size_t> prefix;
    };
    std::vector<TL> trimmed_lines;
    bool veto_trim = false;
    std::optional<size_t> min_prefix;
    const bool se2024 = config.style_edition() >= StyleEdition::E2024;
    for (size_t k = 1; k < ls.size(); k++) {
        FullCodeCharKind kind = ls[k].first;
        std::string line = ls[k].second;
        bool trimmed = true;
        std::optional<size_t> prefix_space_width;
        if (!is_empty_line(line)) prefix_space_width = get_prefix_space_width(config, line);
        bool new_veto = (kind == FullCodeCharKind::InString || (se2024 && kind == FullCodeCharKind::InStringCommented)) &&
                        !ends_with(line, "\\");
        if (veto_trim || new_veto) {
            veto_trim = new_veto;
            trimmed = false;
        } else {
            line = std::string(trim(line));
        }
        trimmed_lines.push_back(TL{trimmed, line, prefix_space_width});
        std::optional<size_t> contrib;
        if ((kind == FullCodeCharKind::InStringCommented || kind == FullCodeCharKind::EndStringCommented) && se2024) {
            contrib = std::nullopt;
        } else if (kind == FullCodeCharKind::InString || kind == FullCodeCharKind::EndString) {
            contrib = std::nullopt;
        } else {
            contrib = prefix_space_width;
        }
        if (contrib && (!min_prefix || *contrib < *min_prefix)) min_prefix = contrib;
    }
    if (!min_prefix) return std::nullopt;
    std::vector<std::string> out;
    for (const TL& t : trimmed_lines) {
        if (!t.trimmed) {
            out.push_back(t.line);
        } else if (t.prefix) {
            size_t new_indent_width = indent.width() + sat_sub(*t.prefix, *min_prefix);
            out.push_back(Indent::from_width(config, new_indent_width).to_string(config) + t.line);
        } else {
            out.emplace_back();
        }
    }
    return first_line + "\n" + join(out, "\n");
}

bool indent_next_line(uint8_t full_code_char_kind, std::string_view /*line*/, const Config& config) {
    auto kind = static_cast<FullCodeCharKind>(full_code_char_kind);
    if (fcck_is_string(kind)) return false;  // format_strings = false
    if (config.style_edition() >= StyleEdition::E2024) return !fcck_is_commented_string(kind);
    return true;
}

bool is_empty_line(std::string_view s) { return trim_start(s).empty(); }

}  // namespace rustfmt
