// rust_format_reorder.cpp -- rustfmt's reorder.rs and sort.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// reorder.rs: `mod`, `extern crate` and `use` declarations are reordered in
// alphabetical order (reorder_imports = reorder_modules = true,
// group_imports = Preserve, imports_granularity = Preserve).
// sort.rs: version_sort, the style guide's natural sort.
#include "rust_format_imports.h"

namespace rustfmt {

using imports::UseTree;

// ===========================================================================
// sort.rs

namespace {

/// sort.rs VersionChunk.
struct VersionChunk {
    enum Kind : uint8_t { Underscore, Str, Number } kind = Str;
    std::string_view source;  // Str(source) / Number { source }
    size_t value = 0;         // Number
    size_t zeros = 0;         // Number
};

/// sort.rs VersionChunkIter, fused (itertools' zip_longest fuses both sides):
/// once it has returned None it keeps returning None. That matters because a
/// numeric chunk that overflows usize returns None (`parse::<usize>().ok()?`)
/// in the middle of the identifier.
class VersionChunkIter {
public:
    explicit VersionChunkIter(std::string_view ident) : ident_(ident) {}

    std::optional<VersionChunk> next() {
        if (done_) return std::nullopt;
        std::optional<VersionChunk> r = next_inner();
        if (!r) done_ = true;
        return r;
    }

private:
    static size_t char_len(unsigned char b) { return b < 0x80 ? 1 : (b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2)); }
    static bool is_ascii_digit(unsigned char b) { return b >= '0' && b <= '9'; }

    std::optional<VersionChunk> next_inner() {
        if (start_ >= ident_.size()) return std::nullopt;
        auto next = static_cast<unsigned char>(ident_[start_]);
        size_t first_len = char_len(next);

        if (next == '_') {
            start_ += 1;
            VersionChunk c;
            c.kind = VersionChunk::Underscore;
            return c;
        }

        if (is_ascii_digit(next)) return parse_numeric_chunk(first_len);

        return parse_str_chunk(first_len);
    }

    std::optional<VersionChunk> parse_numeric_chunk(size_t idx) {
        size_t end = start_;
        bool is_end_of_chunk = false;

        // `chars` continues after the first char: idx is relative to start_.
        while (start_ + idx < ident_.size()) {
            auto c = static_cast<unsigned char>(ident_[start_ + idx]);
            end = start_ + idx;

            if (is_ascii_digit(c)) {
                idx += 1;
                continue;
            }

            is_end_of_chunk = true;
            break;
        }

        std::string_view source;
        if (is_end_of_chunk) {
            source = ident_.substr(start_, end - start_);
            start_ = end;
        } else {
            source = ident_.substr(start_);
            start_ = ident_.size();
        }

        size_t zeros = 0;
        while (zeros < source.size() && source[zeros] == '0') zeros++;
        // source.parse::<usize>().ok()?
        size_t value = 0;
        for (char ch : source) {
            auto d = static_cast<size_t>(ch - '0');
            if (value > (SIZE_MAX - d) / 10) return std::nullopt;
            value = value * 10 + d;
        }

        VersionChunk c;
        c.kind = VersionChunk::Number;
        c.value = value;
        c.zeros = zeros;
        c.source = source;
        return c;
    }

    std::optional<VersionChunk> parse_str_chunk(size_t idx) {
        size_t end = start_;
        bool is_end_of_chunk = false;

        while (start_ + idx < ident_.size()) {
            auto c = static_cast<unsigned char>(ident_[start_ + idx]);
            end = start_ + idx;

            if (c == '_') {
                is_end_of_chunk = true;
                break;
            }

            if (!is_ascii_digit(c)) {
                idx += char_len(c);
                continue;
            }

            is_end_of_chunk = true;
            break;
        }

        std::string_view source;
        if (is_end_of_chunk) {
            source = ident_.substr(start_, end - start_);
            start_ = end;
        } else {
            source = ident_.substr(start_);
            start_ = ident_.size();
        }

        VersionChunk c;
        c.kind = VersionChunk::Str;
        c.source = source;
        return c;
    }

    std::string_view ident_;
    size_t start_ = 0;
    bool done_ = false;
};

/// Determine which side of the version-sort comparison had more leading zeros.
enum class MoreLeadingZeros : uint8_t { Left, Right, Equal };

int ord_of(int c) { return c < 0 ? -1 : (c > 0 ? 1 : 0); }

}  // namespace

/// Compare two identifiers based on the version sorting algorithm described in
/// the style guide (https://doc.rust-lang.org/nightly/style-guide/#sorting).
int version_sort(std::string_view a, std::string_view b) {
    VersionChunkIter iter_a(a);
    VersionChunkIter iter_b(b);
    MoreLeadingZeros more_leading_zeros = MoreLeadingZeros::Equal;

    for (;;) {
        std::optional<VersionChunk> ca = iter_a.next();
        std::optional<VersionChunk> cb = iter_b.next();
        if (!ca && !cb) break;
        if (ca && !cb) return 1;   // EitherOrBoth::Left
        if (!ca && cb) return -1;  // EitherOrBoth::Right
        const VersionChunk& x = *ca;
        const VersionChunk& y = *cb;
        if (x.kind == VersionChunk::Underscore && y.kind == VersionChunk::Underscore) continue;
        if (x.kind == VersionChunk::Underscore) return -1;
        if (y.kind == VersionChunk::Underscore) return 1;
        if (x.kind == VersionChunk::Number && y.kind == VersionChunk::Number) {
            if (x.value != y.value) return x.value < y.value ? -1 : 1;
            if (x.zeros == y.zeros) continue;
            if (more_leading_zeros == MoreLeadingZeros::Equal && x.zeros > y.zeros) {
                more_leading_zeros = MoreLeadingZeros::Left;
            } else if (more_leading_zeros == MoreLeadingZeros::Equal && x.zeros < y.zeros) {
                more_leading_zeros = MoreLeadingZeros::Right;
            }
            continue;
        }
        // (Str, Str) | (Str, Number) | (Number, Str): compare the sources.
        int o = ord_of(x.source.compare(y.source));
        if (o == 0) continue;
        return o;
    }

    switch (more_leading_zeros) {
    case MoreLeadingZeros::Equal: return 0;
    case MoreLeadingZeros::Left: return -1;
    case MoreLeadingZeros::Right: return 1;
    }
    return 0;
}

// ===========================================================================
// reorder.rs

namespace {

int str_cmp(std::string_view a, std::string_view b) { return ord_of(a.compare(b)); }

/// Choose the ordering between the given two items.
int compare_items(const rust::Item* a, const rust::Item* b, const RewriteContext& ctx) {
    StyleEdition style_edition = ctx.config->style_edition();
    if (a->kind == rust::ItemKind::Mod && b->kind == rust::ItemKind::Mod) {
        if (style_edition <= StyleEdition::E2024) return str_cmp(a->ident.name, b->ident.name);
        return version_sort(a->ident.name, b->ident.name);
    }
    if (a->kind == rust::ItemKind::ExternCrate && b->kind == rust::ItemKind::ExternCrate) {
        // `extern crate foo as bar;`
        //               ^^^ Comparing this.
        std::string_view a_orig_name = a->has_orig_name ? a->orig_name.name : a->ident.name;
        std::string_view b_orig_name = b->has_orig_name ? b->orig_name.name : b->ident.name;
        int result = style_edition <= StyleEdition::E2024 ? str_cmp(a_orig_name, b_orig_name)
                                                           : version_sort(a_orig_name, b_orig_name);
        if (result != 0) return result;

        // `extern crate foo as bar;`
        //                      ^^^ Comparing this.
        if (a->has_orig_name && !b->has_orig_name) return 1;
        if (!a->has_orig_name && b->has_orig_name) return -1;
        if (!a->has_orig_name && !b->has_orig_name) return 0;
        if (style_edition <= StyleEdition::E2024) return str_cmp(a->ident.name, b->ident.name);
        return version_sort(a->ident.name, b->ident.name);
    }
    panic("internal error: entered unreachable code");
}

RewriteResult wrap_reorderable_items(const RewriteContext& ctx, const std::vector<ListItem>& list_items,
                                     Shape shape) {
    ListFormatting fmt(shape, *ctx.config);
    fmt.separator("").align_comments(false);
    return write_list(list_items, fmt);
}

RewriteResult rewrite_reorderable_item(const RewriteContext& ctx, const rust::Item* item, Shape shape) {
    switch (item->kind) {
    case rust::ItemKind::ExternCrate: return rewrite_extern_crate(ctx, item, shape);
    case rust::ItemKind::Mod: return rewrite_mod(ctx, item, shape);
    default: return Err(RewriteError::unknown());
    }
}

/// Rewrite a list of items with reordering and/or regrouping. Every item
/// in `items` must have the same `ast::ItemKind`.
RewriteResult rewrite_reorderable_or_regroupable_items(const RewriteContext& ctx,
                                                       const std::vector<const rust::Item*>& reorderable_items,
                                                       Shape shape, Span span) {
    if (reorderable_items[0]->kind == rust::ItemKind::Use) {
        std::vector<UseTree> normalized_items;
        for (const rust::Item* item : reorderable_items) {
            if (std::optional<UseTree> t = UseTree::from_ast_with_normalization(ctx, item))
                normalized_items.push_back(std::move(*t));
        }
        // Add comments before merging.
        std::vector<ListItem> list_items = itemize_list(
            *ctx.snippet_provider, normalized_items.size(), "", ";",
            [&](size_t i) { return normalized_items[i].span_().lo; },
            [&](size_t i) { return normalized_items[i].span_().hi; },
            [](size_t) { return RewriteResult(std::string()); }, span.lo, span.hi, false);
        for (size_t i = 0; i < normalized_items.size() && i < list_items.size(); i++)
            normalized_items[i].list_item = list_items[i];
        // normalize_use_trees_with_granularity: imports_granularity = Preserve
        // returns the trees unchanged (non-default config: Item / Crate / Module / One).

        // group_imports = Preserve: one group (non-default config: StdExternalCrate -> group_imports).
        std::vector<std::vector<UseTree>> regrouped_items;
        regrouped_items.push_back(std::move(normalized_items));

        // reorder_imports = true
        for (std::vector<UseTree>& items : regrouped_items) imports::sort_use_trees(items);

        // 4 = "use ", 1 = ";"
        std::optional<Shape> ns = shape.offset_left(4);
        if (!ns) return Err(RewriteError::exceeds(shape.exceeds(span)));
        Shape nested_shape0 = *ns;
        ns = nested_shape0.sub_width(1);
        if (!ns) return Err(RewriteError::exceeds(nested_shape0.exceeds(span)));
        Shape nested_shape = *ns;

        std::vector<std::string> item_vec;
        for (std::vector<UseTree>& use_group : regrouped_items) {
            if (use_group.empty()) continue;
            std::vector<ListItem> group_items;
            group_items.reserve(use_group.size());
            for (UseTree& use_tree : use_group) {
                RewriteResult item = use_tree.rewrite_top_level(ctx, nested_shape);
                if (use_tree.list_item) {
                    ListItem li = std::move(*use_tree.list_item);
                    li.item = std::move(item);
                    group_items.push_back(std::move(li));
                } else {
                    group_items.push_back(ListItem::from_item(std::move(item)));
                }
            }
            RewriteResult r = wrap_reorderable_items(ctx, group_items, nested_shape);
            if (!r) return r;
            item_vec.push_back(std::move(*r));
        }

        std::string join_string = "\n\n" + shape.indent.to_string(*ctx.config);
        return join(item_vec, join_string);
    }

    std::vector<ListItem> list_items = itemize_list(
        *ctx.snippet_provider, reorderable_items.size(), "", ";",
        [&](size_t i) { return spanned(reorderable_items[i]).lo; },
        [&](size_t i) { return spanned(reorderable_items[i]).hi; },
        [&](size_t i) { return rewrite_reorderable_item(ctx, reorderable_items[i], shape); }, span.lo, span.hi,
        false);

    std::vector<std::pair<ListItem, const rust::Item*>> item_pair_vec;
    for (size_t i = 0; i < list_items.size() && i < reorderable_items.size(); i++)
        item_pair_vec.emplace_back(std::move(list_items[i]), reorderable_items[i]);
    imports::rust_stable_sort(item_pair_vec, [&](const std::pair<ListItem, const rust::Item*>& x,
                                                 const std::pair<ListItem, const rust::Item*>& y) {
        return compare_items(x.second, y.second, ctx) < 0;
    });
    std::vector<ListItem> item_vec;
    item_vec.reserve(item_pair_vec.size());
    for (auto& pair : item_pair_vec) item_vec.push_back(std::move(pair.first));

    return wrap_reorderable_items(ctx, item_vec, shape);
}

/// attr::contains_name(&item.attrs, sym::macro_use)
bool contains_macro_use_attr(const rust::Item* item) {
    for (const rust::Attribute& a : item->attrs) {
        if (a.is_doc_comment || a.path == nullptr) continue;
        if (a.path->segments.size() == 1 && a.path->segments[0].ident.name == "macro_use") return true;
    }
    return false;
}

/// A simplified version of `ast::ItemKind`.
enum class ReorderableItemKind : uint8_t {
    ExternCrate,
    Mod,
    Use,
    /// An item that cannot be reordered. Either has an unreorderable item kind
    /// or an `macro_use` attribute.
    Other,
};

ReorderableItemKind reorderable_from(const RewriteContext& ctx, const rust::Item* item) {
    // `contains_macro_use_attr(item) | contains_skip(&item.attrs)` (non-short-circuit)
    bool mu = contains_macro_use_attr(item);
    bool sk = contains_skip(ctx, item->attrs);
    if (mu || sk) return ReorderableItemKind::Other;
    switch (item->kind) {
    case rust::ItemKind::ExternCrate: return ReorderableItemKind::ExternCrate;
    case rust::ItemKind::Mod:
        if (is_mod_decl(item)) return ReorderableItemKind::Mod;
        return ReorderableItemKind::Other;
    case rust::ItemKind::Use: return ReorderableItemKind::Use;
    default: return ReorderableItemKind::Other;
    }
}

bool is_same_item_kind(const RewriteContext& ctx, ReorderableItemKind k, const rust::Item* item) {
    return reorderable_from(ctx, item) == k;
}

bool is_reorderable(ReorderableItemKind k, const Config& config) {
    switch (k) {
    case ReorderableItemKind::ExternCrate: return config.reorder_imports();
    case ReorderableItemKind::Mod: return config.reorder_modules();
    case ReorderableItemKind::Use: return config.reorder_imports();
    case ReorderableItemKind::Other: return false;
    }
    return false;
}

bool is_regroupable(ReorderableItemKind k, const Config&) {
    switch (k) {
    case ReorderableItemKind::ExternCrate:
    case ReorderableItemKind::Mod:
    case ReorderableItemKind::Other: return false;
    // group_imports != Preserve: false at the default config.
    case ReorderableItemKind::Use: return false;
    }
    return false;
}

bool in_group(ReorderableItemKind k, const Config&) {
    switch (k) {
    case ReorderableItemKind::ExternCrate:
    case ReorderableItemKind::Mod: return true;
    // group_imports == Preserve: true at the default config.
    case ReorderableItemKind::Use: return true;
    case ReorderableItemKind::Other: return false;
    }
    return false;
}

/// psess.lookup_line_range(span), kept as byte positions: rustfmt only ever
/// asks `current.lo < last.hi + 2` (line numbers), which is "fewer than two
/// newlines between last.hi and current.lo"; that is what in_same_line_group
/// counts, with no line table to cache.
struct LineRange {
    uint32_t lo = 0;
    uint32_t hi = 0;
};

LineRange lookup_line_range(Span span) { return LineRange{span.lo, span.hi}; }

/// `current.lo < last.hi + 2` on line numbers.
bool in_same_line_group(const FmtVisitor& v, LineRange last, LineRange current) {
    std::string_view text = v.snippet_provider->entire_snippet();
    uint32_t a = last.hi;
    uint32_t b = current.lo;
    if (b <= a) return true;  // line(b) <= line(a)
    size_t newlines = 0;
    for (uint32_t i = a; i < b && i < text.size(); i++) {
        if (text[i] == '\n' && ++newlines >= 2) return false;
    }
    return true;
}

/// FmtVisitor::walk_reorderable_or_regroupable_items: format items with the
/// same item kind and reorder them. If `in_group` is `true`, then the items
/// separated by an empty line will not be reordered together. Returns how
/// many items of items[start..] it consumed.
size_t walk_reorderable_or_regroupable_items(FmtVisitor& v, const std::vector<const rust::Item*>& all_items,
                                             size_t start, ReorderableItemKind item_kind, bool in_grp) {
    RewriteContext kind_ctx = v.get_context();
    LineRange last = lookup_line_range(spanned(all_items[start]));
    size_t item_length = 0;
    for (size_t i = start; i < all_items.size(); i++) {
        const rust::Item* ppi = all_items[i];
        if (!is_same_item_kind(kind_ctx, item_kind, ppi)) break;
        if (in_grp) {
            LineRange current = lookup_line_range(spanned(ppi));
            bool in_same_group = in_same_line_group(v, last, current);
            last = current;
            if (!in_same_group) break;
        }
        item_length++;
    }
    std::vector<const rust::Item*> items(all_items.begin() + static_cast<std::ptrdiff_t>(start),
                                         all_items.begin() + static_cast<std::ptrdiff_t>(start + item_length));

    // out_of_file_lines_range!: file_lines is "all" at the default config.
    bool at_least_one_in_file_lines = !items.empty();

    if (at_least_one_in_file_lines && !items.empty()) {
        uint32_t lo = spanned(items.front()).lo;
        uint32_t hi = spanned(items.back()).hi;
        Span span = mk_sp(lo, hi);
        RewriteContext ctx = v.get_context();
        RewriteResult rw = rewrite_reorderable_or_regroupable_items(ctx, items, v.shape(), span);
        v.push_rewrite(span, rw.opt());
    } else {
        for (const rust::Item* item : items) v.push_rewrite(item->span, std::nullopt);
    }

    return item_length;
}

}  // namespace

/// FmtVisitor::visit_items_with_reordering: visits and formats the given
/// items. Items are reordered if they are consecutive and reorderable.
void visit_items_with_reordering(FmtVisitor& v, const std::vector<const rust::Item*>& items) {
    size_t pos = 0;
    while (pos < items.size()) {
        // If the next item is a `use`, `extern crate` or `mod`, then extract it and any
        // subsequent items that have the same item kind to be reordered within
        // `walk_reorderable_items`. Otherwise, just format the next item for output.
        ReorderableItemKind item_kind;
        {
            RewriteContext ctx = v.get_context();
            item_kind = reorderable_from(ctx, items[pos]);
        }
        if (is_reorderable(item_kind, *v.config) || is_regroupable(item_kind, *v.config)) {
            size_t visited_items_num =
                walk_reorderable_or_regroupable_items(v, items, pos, item_kind, in_group(item_kind, *v.config));
            pos += visited_items_num;
        } else {
            v.visit_item(items[pos]);
            pos += 1;
        }
    }
}

}  // namespace rustfmt
