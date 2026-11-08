// rust_format_imports.cpp -- rustfmt's imports.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// imports.rs translates each `use` tree into its own representation
// (UseTree / UseSegment, see rust_format_imports.h), normalises it, orders it
// and prints it back. Only the default config is ported: imports_granularity
// = Preserve (so the flatten / merge machinery is never reached),
// group_imports = Preserve, imports_layout = Mixed, imports_indent = Block.
#include "rust_format_imports.h"

namespace rustfmt {

using imports::UseSegment;
using imports::UseSegmentKindTag;
using imports::UseTree;

namespace {

int ord(int c) { return c < 0 ? -1 : (c > 0 ? 1 : 0); }

/// `str::cmp`: byte-wise.
int str_cmp(std::string_view a, std::string_view b) { return ord(a.compare(b)); }

/// `Option<String>::cmp`: None < Some.
int opt_cmp(const std::optional<std::string>& a, const std::optional<std::string>& b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return str_cmp(*a, *b);
}

/// `str::trim_start_matches("r#")`.
std::string_view trim_start_matches_raw(std::string_view s) {
    while (s.size() >= 2 && s[0] == 'r' && s[1] == '#') s.remove_prefix(2);
    return s;
}

/// Decodes one UTF-8 char at s[i] (the text is valid UTF-8: it is source text).
uint32_t decode_char(std::string_view s, size_t i, size_t* len) {
    auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        *len = 1;
        return b;
    }
    size_t n = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2);
    uint32_t c = b & (n == 2 ? 0x1Fu : (n == 3 ? 0x0Fu : 0x07u));
    for (size_t k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    *len = n;
    return c;
}

/// char::is_uppercase (the Unicode Uppercase property). ASCII is exact; the
/// non-ASCII part covers the common alphabets (Latin-1, Latin Extended-A,
/// Latin Extended Additional, Greek, Cyrillic, Armenian, fullwidth, circled
/// letters, Roman numerals), not the whole table.
bool char_is_uppercase(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z';
    if ((c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE)) return true;
    if (c >= 0x100 && c <= 0x137) return (c & 1) == 0;
    if (c >= 0x139 && c <= 0x148) return (c & 1) == 1;
    if (c >= 0x14A && c <= 0x177) return (c & 1) == 0;
    if (c == 0x178 || c == 0x179 || c == 0x17B || c == 0x17D) return true;
    if (c == 0x386 || (c >= 0x388 && c <= 0x38A) || c == 0x38C || c == 0x38E || c == 0x38F) return true;
    if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return true;
    if (c >= 0x400 && c <= 0x42F) return true;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF)) return (c & 1) == 0;
    if (c >= 0x531 && c <= 0x556) return true;
    if ((c >= 0x1E00 && c <= 0x1E94) || (c >= 0x1EA0 && c <= 0x1EFE)) return (c & 1) == 0;
    if (c >= 0x2160 && c <= 0x216F) return true;
    if (c >= 0x24B6 && c <= 0x24CF) return true;
    if (c >= 0xFF21 && c <= 0xFF3A) return true;
    return false;
}

/// char::is_numeric (general categories Nd, Nl, No). ASCII is exact; the
/// non-ASCII part covers the common decimal digit blocks and number forms.
bool char_is_numeric(uint32_t c) {
    if (c < 0x80) return c >= '0' && c <= '9';
    if (c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0xBC && c <= 0xBE)) return true;
    static const uint32_t kZeros[] = {0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66,
                                      0xCE6, 0xD66, 0xDE6, 0xE50, 0xED0, 0xF20, 0x1040, 0x1090, 0x17E0,
                                      0x1810, 0xFF10};
    for (uint32_t z : kZeros)
        if (c >= z && c <= z + 9) return true;
    if (c >= 0x2150 && c <= 0x2189) return true;
    if (c >= 0x2460 && c <= 0x249B) return true;
    return false;
}

// is_upper_snake_case (inside Ord for UseSegment).
bool is_upper_snake_case(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 1;
        uint32_t c = decode_char(s, i, &len);
        if (!(char_is_uppercase(c) || c == '_' || char_is_numeric(c))) return false;
        i += len;
    }
    return true;
}

// ident.starts_with(char::is_uppercase)
bool starts_with_uppercase(std::string_view s) {
    if (s.empty()) return false;
    size_t len = 1;
    return char_is_uppercase(decode_char(s, 0, &len));
}

/// `ast::Path::is_global`: the first segment is `{{root}}` (kw::PathRoot).
bool path_is_global_(const rust::Path* path) {
    return path != nullptr && !path->segments.empty() && path->segments[0].ident.name == "{{root}}";
}

/// ast::UseTree::hi_span (rustc_ast).
Span hi_span(const rust::UseTree* a) {
    switch (a->kind) {
    case rust::UseTree::Simple: return a->has_rename ? a->rename.span : a->prefix->span;
    case rust::UseTree::Nested: return a->nested_span;
    case rust::UseTree::Glob: return a->glob_span;
    }
    return a->span;
}

UseSegment make_segment(UseSegmentKindTag tag, std::string ident, std::optional<std::string> alias,
                        StyleEdition style_edition) {
    UseSegment s;
    s.tag = tag;
    s.ident = std::move(ident);
    s.alias = std::move(alias);
    s.style_edition = style_edition;
    return s;
}

// UseSegment::from_path_segment
std::optional<UseSegment> from_path_segment(const RewriteContext& ctx, const rust::PathSegment& path_seg,
                                            bool modsep) {
    std::string_view name = rewrite_ident(ctx, path_seg.ident);
    if (name.empty()) return std::nullopt;
    UseSegmentKindTag tag = UseSegmentKindTag::Ident;
    std::string ident;
    if (name == "self") {
        tag = UseSegmentKindTag::Slf;
    } else if (name == "super") {
        tag = UseSegmentKindTag::Super;
    } else if (name == "crate") {
        tag = UseSegmentKindTag::Crate;
    } else {
        ident = std::string(modsep ? "::" : "") + std::string(name);
    }
    return make_segment(tag, std::move(ident), std::nullopt, ctx.config->style_edition());
}

RewriteResult exceeds(Shape shape, Span span) { return Err(RewriteError::exceeds(shape.exceeds(span))); }

RewriteResult rewrite_nested_use_tree(const RewriteContext& ctx, const std::vector<UseTree>& use_tree_list,
                                      Shape shape) {
    std::vector<ListItem> list_items;
    list_items.reserve(use_tree_list.size());
    // imports_indent = Block (non-default config: Visual -> shape.visual_indent(0))
    std::optional<Shape> nested_shape_opt =
        shape.block_indent(ctx.config->tab_spaces()).with_max_width(*ctx.config).sub_width_opt(1);
    if (!nested_shape_opt) return Err(RewriteError::unknown());
    Shape nested_shape = *nested_shape_opt;
    for (const UseTree& use_tree : use_tree_list) {
        if (use_tree.list_item) {
            ListItem list_item = *use_tree.list_item;
            list_item.item = use_tree.rewrite_result(ctx, nested_shape);
            list_items.push_back(std::move(list_item));
        } else {
            RewriteResult r = use_tree.rewrite_result(ctx, nested_shape);
            if (!r) return r;
            list_items.push_back(ListItem::from_str(std::move(*r)));
        }
    }
    bool has_nested_list = false;
    for (const UseTree& use_segment : use_tree_list) {
        if (!use_segment.path.empty() && use_segment.path.back().tag == UseSegmentKindTag::List) {
            has_nested_list = true;
            break;
        }
    }

    size_t remaining_width = has_nested_list ? 0 : sat_sub(shape.width, 2);

    // imports_layout = Mixed
    DefinitiveListTactic tactic = definitive_tactic(list_items, ListTactic::Mixed(), Separator::Comma, remaining_width);

    // imports_indent = Block
    bool ends_with_newline = tactic != DefinitiveListTactic::Horizontal();
    // trailing_comma = Vertical
    SeparatorTactic trailing_separator = ends_with_newline ? SeparatorTactic::Vertical : SeparatorTactic::Never;
    ListFormatting fmt(nested_shape, *ctx.config);
    fmt.tactic(tactic)
        .trailing_separator(trailing_separator)
        .ends_with_newline(ends_with_newline)
        .preserve_newline(true)
        .nested(has_nested_list);

    RewriteResult list_str_r = write_list(list_items, fmt);
    if (!list_str_r) return list_str_r;
    const std::string& list_str = *list_str_r;

    std::string result;
    if (list_str.find('\n') != std::string::npos || list_str.size() > remaining_width ||
        tactic == DefinitiveListTactic::Vertical()) {
        result = "{\n" + nested_shape.indent.to_string(*ctx.config) + list_str + "\n" +
                 shape.indent.to_string(*ctx.config) + "}";
    } else {
        result = "{" + list_str + "}";
    }
    return result;
}

// impl Rewrite for UseSegment
RewriteResult rewrite_segment(const RewriteContext& ctx, const UseSegment& seg, Shape shape) {
    switch (seg.tag) {
    case UseSegmentKindTag::Ident:
        if (seg.alias) return seg.ident + " as " + *seg.alias;
        return seg.ident;
    case UseSegmentKindTag::Slf:
        if (seg.alias) return "self as " + *seg.alias;
        return std::string("self");
    case UseSegmentKindTag::Super:
        if (seg.alias) return "super as " + *seg.alias;
        return std::string("super");
    case UseSegmentKindTag::Crate:
        if (seg.alias) return "crate as " + *seg.alias;
        return std::string("crate");
    case UseSegmentKindTag::Glob: return std::string("*");
    case UseSegmentKindTag::List: {
        // 1 = "{" and "}"
        std::optional<Shape> s = shape.offset_left_opt(1);
        if (s) s = s->sub_width_opt(1);
        if (!s) return Err(RewriteError::unknown());
        return rewrite_nested_use_tree(ctx, seg.list, *s);
    }
    }
    return Err(RewriteError::unknown());
}

}  // namespace

/// path_to_imported_ident: the last segment's ident (`Ordering` for
/// `std::cmp::Ordering`, `self` for `std::cmp::self`).
static const rust::Ident& path_to_imported_ident(const rust::Path* path) {
    if (path == nullptr || path->segments.empty()) panic("path_to_imported_ident: empty path");
    return path->segments.back().ident;
}

namespace imports {

// ---------------------------------------------------------------------------
// UseSegment

UseSegment UseSegment::remove_alias() const {
    switch (tag) {
    case UseSegmentKindTag::Ident:
    case UseSegmentKindTag::Slf:
    case UseSegmentKindTag::Super:
    case UseSegmentKindTag::Crate: return make_segment(tag, ident, std::nullopt, style_edition);
    default: return *this;
    }
}

std::string UseSegment::to_string() const {
    switch (tag) {
    case UseSegmentKindTag::Glob: return "*";
    case UseSegmentKindTag::Ident: return alias ? ident + " as " + *alias : ident;
    case UseSegmentKindTag::Slf: return "self";
    case UseSegmentKindTag::Super: return "super";
    case UseSegmentKindTag::Crate: return "crate";
    case UseSegmentKindTag::List: {
        std::string out = "{";
        for (size_t i = 0; i < list.size(); i++) {
            if (i != 0) out += ", ";
            out += list[i].to_string();
        }
        out += "}";
        return out;
    }
    }
    return std::string();
}

bool UseSegment::operator==(const UseSegment& o) const {
    if (tag != o.tag) return false;
    bool kind_eq = true;
    switch (tag) {
    case UseSegmentKindTag::Ident: kind_eq = ident == o.ident && alias == o.alias; break;
    case UseSegmentKindTag::Slf:
    case UseSegmentKindTag::Super:
    case UseSegmentKindTag::Crate: kind_eq = alias == o.alias; break;
    case UseSegmentKindTag::Glob: kind_eq = true; break;
    case UseSegmentKindTag::List: kind_eq = list == o.list; break;
    }
    return kind_eq && style_edition == o.style_edition;
}

// impl Ord for UseSegment
int UseSegment::cmp(const UseSegment& other) const {
    using T = UseSegmentKindTag;
    const T a = tag;
    const T b = other.tag;
    const bool edition2024 = style_edition >= StyleEdition::E2024;
    if (a == b && (a == T::Slf || a == T::Super || a == T::Crate)) {
        if (alias && other.alias) {
            if (edition2024) return version_sort(trim_start_matches_raw(*alias), trim_start_matches_raw(*other.alias));
            return opt_cmp(alias, other.alias);
        }
        return opt_cmp(alias, other.alias);
    }
    if (a == T::Glob && b == T::Glob) return 0;
    if (a == T::Ident && b == T::Ident) {
        std::string_view ia = ident;
        std::string_view ib = other.ident;
        if (edition2024) {
            ia = trim_start_matches_raw(ia);
            ib = trim_start_matches_raw(ib);
        }
        int ident_ord;
        if (edition2024) {
            ident_ord = version_sort(ia, ib);
        } else {
            // sorting_key: (is_upper_snake_case, starts_with uppercase, ident);
            // snake_case < CamelCase < UPPER_SNAKE_CASE
            bool ua = is_upper_snake_case(ia);
            bool ub = is_upper_snake_case(ib);
            if (ua != ub) {
                ident_ord = ua ? 1 : -1;
            } else {
                bool ca = starts_with_uppercase(ia);
                bool cb = starts_with_uppercase(ib);
                if (ca != cb)
                    ident_ord = ca ? 1 : -1;
                else
                    ident_ord = str_cmp(ia, ib);
            }
        }
        if (ident_ord != 0) return ident_ord;
        if (!alias && other.alias) return -1;
        if (alias && !other.alias) return 1;
        if (alias && other.alias) {
            if (edition2024) return version_sort(trim_start_matches_raw(*alias), trim_start_matches_raw(*other.alias));
            return str_cmp(*alias, *other.alias);
        }
        return 0;
    }
    if (a == T::List && b == T::List) {
        // a.iter().cmp(b.iter())
        size_t n = list.size() < other.list.size() ? list.size() : other.list.size();
        for (size_t i = 0; i < n; i++) {
            int c = list[i].cmp(other.list[i]);
            if (c != 0) return c;
        }
        return list.size() < other.list.size() ? -1 : (list.size() > other.list.size() ? 1 : 0);
    }
    if (a == T::Slf) return -1;
    if (b == T::Slf) return 1;
    if (a == T::Super) return -1;
    if (b == T::Super) return 1;
    if (a == T::Crate) return -1;
    if (b == T::Crate) return 1;
    if (a == T::Ident) return -1;
    if (b == T::Ident) return 1;
    if (a == T::Glob) return -1;
    if (b == T::Glob) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// UseTree

Span UseTree::span_() const {
    uint32_t lo = span.lo;
    if (attrs != nullptr && !attrs->empty()) lo = attrs->front().span.lo;
    return mk_sp(lo, span.hi);
}

std::string UseTree::to_string() const {
    std::string out;
    for (size_t i = 0; i < path.size(); i++) {
        if (i != 0) out += "::";
        out += path[i].to_string();
    }
    return out;
}

// impl Ord for UseTree
int UseTree::cmp(const UseTree& other) const {
    size_t n = path.size() < other.path.size() ? path.size() : other.path.size();
    for (size_t i = 0; i < n; i++) {
        const UseSegment& a = path[i];
        const UseSegment& b = other.path[i];
        int o = a.cmp(b);
        // The comparison without aliases is a hack to avoid situations like
        // comparing `a::b` to `a as c` - where the latter should be ordered
        // first since it is shorter.
        if (o != 0) {
            // remove_alias() is the identity for Glob / List: skip the copy.
            bool a_plain = a.tag == UseSegmentKindTag::Glob || a.tag == UseSegmentKindTag::List;
            bool b_plain = b.tag == UseSegmentKindTag::Glob || b.tag == UseSegmentKindTag::List;
            int o2;
            if (a_plain && b_plain) {
                o2 = o;
            } else if (a_plain) {
                o2 = a.cmp(b.remove_alias());
            } else if (b_plain) {
                o2 = a.remove_alias().cmp(b);
            } else {
                o2 = a.remove_alias().cmp(b.remove_alias());
            }
            if (o2 != 0) return o;
        }
    }
    return path.size() < other.path.size() ? -1 : (path.size() > other.path.size() ? 1 : 0);
}

// Rewrite use tree with `use ` and a trailing `;`.
RewriteResult UseTree::rewrite_top_level(const RewriteContext& ctx, Shape shape) const {
    std::string vis = visibility != nullptr ? format_visibility(ctx, *visibility) : std::string();
    std::optional<Shape> s = shape.offset_left(vis.size());
    if (!s) return exceeds(shape, span_());
    RewriteResult r = rewrite_result(ctx, *s);
    if (!r) return r;
    std::string use_str = r->empty() ? std::string() : vis + "use " + *r + ";";
    if (attrs != nullptr && !attrs->empty()) {
        RewriteResult attr_str = rewrite_attrs(ctx, *attrs, shape);
        if (!attr_str) return attr_str;
        uint32_t lo = attrs->back().span.hi;
        uint32_t hi = span.lo;
        Span sp = mk_sp(lo, hi);

        bool allow_extend = false;
        if (attrs->size() == 1) {
            size_t line_len = attr_str->size() + 1 + use_str.size();
            allow_extend = !attrs->front().is_doc_comment && ctx.config->inline_attribute_width() >= line_len;
        }
        return combine_strs_with_missing_comments(ctx, *attr_str, use_str, sp, shape, allow_extend);
    }
    return use_str;
}

// FIXME: Use correct span? (rustfmt): the span of a reconstructed tree is not
// used for comments afterwards.
UseTree UseTree::from_path(std::vector<UseSegment> p, Span sp) {
    UseTree t;
    t.path = std::move(p);
    t.span = sp;
    return t;
}

std::optional<UseTree> UseTree::from_ast_with_normalization(const RewriteContext& ctx, const rust::Item* item) {
    if (item->kind != rust::ItemKind::Use) return std::nullopt;
    return from_ast(ctx, item->use_tree, std::nullopt, &item->vis, item->span.lo,
                    item->attrs.empty() ? nullptr : &item->attrs)
        .normalize();
}

UseTree UseTree::from_ast(const RewriteContext& ctx, const rust::UseTree* a, std::optional<ListItem> list_item,
                          const rust::Visibility* visibility, std::optional<uint32_t> opt_lo,
                          const rust::AttrVec* attrs) {
    DepthGuard g(*ctx.state);
    Span sp = opt_lo ? mk_sp(*opt_lo, hi_span(a).hi) : a->span;
    UseTree result;
    result.span = sp;
    result.list_item = std::move(list_item);
    result.visibility = visibility;
    result.attrs = attrs;

    const rust::Path* prefix = a->prefix;
    const bool leading_modsep = ctx.config->edition() >= rust::Edition::E2018 && path_is_global_(prefix);

    bool modsep = leading_modsep;

    for (const rust::PathSegment& p : prefix->segments) {
        if (std::optional<UseSegment> use_segment = from_path_segment(ctx, p, modsep)) {
            result.path.push_back(std::move(*use_segment));
            modsep = false;
        }
    }

    const StyleEdition style_edition = ctx.config->style_edition();

    switch (a->kind) {
    case rust::UseTree::Glob:
        // in case of a global path and the glob starts at the root, e.g., "::*"
        if (prefix->segments.size() == 1 && leading_modsep)
            result.path.push_back(make_segment(UseSegmentKindTag::Ident, "", std::nullopt, style_edition));
        result.path.push_back(make_segment(UseSegmentKindTag::Glob, "", std::nullopt, style_edition));
        break;
    case rust::UseTree::Nested: {
        // Extract comments between nested use items.
        // This needs to be done before sorting use items.
        const std::vector<rust::UseTree*>& list = a->items;
        std::vector<ListItem> items = itemize_list(
            *ctx.snippet_provider, list.size(), "}", ",",
            [&](size_t i) { return list[i]->prefix->span.lo; }, [&](size_t i) { return hi_span(list[i]).hi; },
            [](size_t) { return RewriteResult(std::string()); },  // We only need comments for now.
            ctx.snippet_provider->span_after(a->span, "{"), hi_span(a).hi, false);

        // in case of a global path and the nested list starts at the root,
        // e.g., "::{foo, bar}"
        if (prefix->segments.size() == 1 && leading_modsep)
            result.path.push_back(make_segment(UseSegmentKindTag::Ident, "", std::nullopt, style_edition));
        UseSegment seg = make_segment(UseSegmentKindTag::List, "", std::nullopt, style_edition);
        size_t n = list.size() < items.size() ? list.size() : items.size();
        seg.list.reserve(n);
        for (size_t i = 0; i < n; i++)
            seg.list.push_back(from_ast(ctx, list[i], std::move(items[i]), nullptr, std::nullopt, nullptr));
        result.path.push_back(std::move(seg));
        break;
    }
    case rust::UseTree::Simple: {
        // If the path has leading double colons and is composed of only 2 segments, then we
        // bypass the call to path_to_imported_ident which would get only the ident and
        // lose the path root, e.g., `that` in `::that`.
        // The span of `a.prefix` contains the leading colons.
        std::string name;
        if (prefix->segments.size() == 2 && leading_modsep)
            name = std::string(ctx.snippet(prefix->span));
        else
            name = std::string(rewrite_ident(ctx, path_to_imported_ident(prefix)));
        std::optional<std::string> alias;
        if (a->has_rename) {
            const rust::Ident& ident = a->rename;
            // `sym::underscore_imports` is the symbol "underscore_imports" (not
            // `_`); transcribed as rustfmt has it.
            if (ident.name == "underscore_imports") {
                // for impl-only-use
                alias = "_";
            } else if (ident.name == path_to_imported_ident(prefix).name) {
                // Ident equality: same symbol (and syntax context).
                alias = std::nullopt;
            } else {
                alias = std::string(rewrite_ident(ctx, ident));
            }
        }
        UseSegmentKindTag tag = UseSegmentKindTag::Ident;
        if (name == "self")
            tag = UseSegmentKindTag::Slf;
        else if (name == "super")
            tag = UseSegmentKindTag::Super;
        else if (name == "crate")
            tag = UseSegmentKindTag::Crate;
        UseSegment segment =
            make_segment(tag, tag == UseSegmentKindTag::Ident ? std::move(name) : std::string(), std::move(alias),
                         style_edition);

        // `name` is already in result.
        if (!result.path.empty()) result.path.pop_back();
        result.path.push_back(std::move(segment));
        break;
    }
    }
    return result;
}

// Do the adjustments that rustfmt does elsewhere to use paths.
UseTree UseTree::normalize() && {
    if (path.empty()) panic("Empty use tree?");
    UseSegment last = std::move(path.back());
    path.pop_back();
    // Hack around borrow checker.
    bool normalize_sole_list = false;
    bool aliased_self = false;

    // Remove foo::{} or self without attributes.
    if (attrs == nullptr) {
        if (last.tag == UseSegmentKindTag::List && last.list.empty()) {
            path.clear();
            return std::move(*this);
        }
        if (last.tag == UseSegmentKindTag::Slf && !last.alias && path.empty() && visibility != nullptr) {
            path.clear();
            return std::move(*this);
        }
    }

    // Normalise foo::self -> foo.
    if (last.tag == UseSegmentKindTag::Slf && !last.alias) {
        if (!path.empty()) {
            if (path.back().tag != UseSegmentKindTag::Slf) return std::move(*this);
        }
    }

    // Normalise foo::self as bar -> foo as bar.
    if (last.tag == UseSegmentKindTag::Slf) {
        if (!path.empty() && path.back().tag == UseSegmentKindTag::Ident && !path.back().alias) aliased_self = true;
    }

    bool done = false;
    if (aliased_self) {
        UseSegment& prev = path.back();
        if (prev.tag != UseSegmentKindTag::Ident) panic("internal error: entered unreachable code");
        if (prev.alias) panic("assertion failed: old_rename.is_none()");
        if (last.alias) {
            prev.alias = last.alias;
            done = true;
        }
    }

    if (done) return std::move(*this);

    // Normalise foo::{bar} -> foo::bar
    if (last.tag == UseSegmentKindTag::List) {
        if (last.list.size() == 1 && last.list[0].to_string() != "self" && !last.list[0].has_comment())
            normalize_sole_list = true;
    }

    if (normalize_sole_list) {
        for (const UseSegment& seg : last.list[0].path) path.push_back(seg);
        return std::move(*this).normalize();
    }

    // Recursively normalize elements of a list use (including sorting the list).
    if (last.tag == UseSegmentKindTag::List) {
        std::vector<UseTree> list;
        list.reserve(last.list.size());
        for (UseTree& t : last.list) list.push_back(std::move(t).normalize());
        sort_use_trees(list);
        dedup_use_trees(list);
        StyleEdition se = last.style_edition;
        last = make_segment(UseSegmentKindTag::List, "", std::nullopt, se);
        last.list = std::move(list);
    }

    path.push_back(std::move(last));
    return std::move(*this);
}

bool UseTree::has_comment() const { return list_item && list_item->has_comment(); }

// impl Rewrite for UseTree: does NOT format attributes and visibility or add a trailing `;`.
RewriteResult UseTree::rewrite_result(const RewriteContext& ctx, Shape shape) const {
    DepthGuard g(*ctx.state);
    std::string result;
    for (size_t i = 0; i < path.size(); i++) {
        RewriteResult segment_str = rewrite_segment(ctx, path[i], shape);
        if (!segment_str) return segment_str;
        result += *segment_str;
        if (i + 1 < path.size()) {
            result += "::";
            // 2 = "::"
            std::optional<Shape> s = shape.offset_left(2 + segment_str->size());
            if (!s) return exceeds(shape, span_());
            shape = *s;
        }
    }
    return result;
}

void sort_use_trees(std::vector<UseTree>& v) {
    rust_stable_sort(v, [](const UseTree& a, const UseTree& b) { return a.cmp(b) < 0; });
}

void dedup_use_trees(std::vector<UseTree>& v) {
    if (v.size() < 2) return;
    size_t w = 1;
    for (size_t r = 1; r < v.size(); r++) {
        if (v[r] == v[w - 1]) continue;
        if (r != w) v[w] = std::move(v[r]);
        w++;
    }
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(w), v.end());
}

}  // namespace imports

// FmtVisitor::format_import
void format_import(FmtVisitor& v, const rust::Item* item) {
    Span span = spanned(item);
    Shape shape = v.shape();
    RewriteContext ctx = v.get_context();
    Opt rw = UseTree::from_ast(ctx, item->use_tree, std::nullopt, &item->vis, item->span.lo, &item->attrs)
                 .rewrite_top_level(ctx, shape)
                 .opt();
    if (rw && rw->empty()) {
        // Format up to last newline
        Span prev_span = mk_sp(v.last_pos, span.lo);
        std::string_view trimmed_snippet = trim_end(v.snippet(prev_span));
        uint32_t span_end = v.last_pos + static_cast<uint32_t>(trimmed_snippet.size());
        v.format_missing(span_end);
        // We have an excessive newline from the removed import.
        if (!v.buffer.empty() && v.buffer.back() == '\n') {
            v.buffer.pop_back();
            v.line_number -= 1;
        }
        v.last_pos = span.hi;
    } else if (rw) {
        v.format_missing_with_indent(span.lo);
        v.push_str(*rw);
        v.last_pos = span.hi;
    } else {
        v.format_missing_with_indent(span.lo);
        v.format_missing(span.hi);
    }
}

}  // namespace rustfmt
