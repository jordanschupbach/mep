// rust_format_imports.h -- the private interface between rust_format_imports.cpp
// (rustfmt's imports.rs) and rust_format_reorder.cpp (reorder.rs): imports.rs's
// own UseTree / UseSegment / UseSegmentKind model. Only those two files include it.
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "rust_format_internal.h"

namespace rustfmt {
namespace imports {

struct UseTree;

/// imports.rs UseSegmentKind (#[derive(Clone, Eq, Hash, PartialEq)]); the
/// variant order is the declaration order.
enum class UseSegmentKindTag : uint8_t { Ident, Slf, Super, Crate, Glob, List };

/// imports.rs UseSegment { kind: UseSegmentKind, style_edition }.
struct UseSegment {
    UseSegmentKindTag tag = UseSegmentKindTag::Glob;
    std::string ident;                 // Ident(ident, _)
    std::optional<std::string> alias;  // Ident(_, alias), Slf(alias), Super(alias), Crate(alias)
    std::vector<UseTree> list;         // List(list)
    StyleEdition style_edition = StyleEdition::E2015;

    UseSegment remove_alias() const;
    /// impl Display for UseSegmentKind
    std::string to_string() const;
    /// impl Ord for UseSegment: <0, 0, >0
    int cmp(const UseSegment& other) const;
    /// #[derive(PartialEq)]
    bool operator==(const UseSegment& other) const;
    bool operator!=(const UseSegment& other) const { return !(*this == other); }
};

/// imports.rs UseTree.
struct UseTree {
    std::vector<UseSegment> path;
    Span span;
    // Comment information within nested use tree.
    std::optional<ListItem> list_item;
    // Additional fields for top level use items (None = null).
    const rust::Visibility* visibility = nullptr;
    const rust::AttrVec* attrs = nullptr;

    /// impl Spanned for UseTree
    Span span_() const;
    /// impl Display for UseTree
    std::string to_string() const;
    /// impl Ord for UseTree: <0, 0, >0
    int cmp(const UseTree& other) const;
    /// impl PartialEq for UseTree: the paths only.
    bool operator==(const UseTree& other) const { return path == other.path; }
    bool operator!=(const UseTree& other) const { return !(*this == other); }

    RewriteResult rewrite_top_level(const RewriteContext& ctx, Shape shape) const;
    /// impl Rewrite for UseTree (no attributes, visibility or trailing `;`).
    RewriteResult rewrite_result(const RewriteContext& ctx, Shape shape) const;

    static UseTree from_path(std::vector<UseSegment> path, Span span);
    static std::optional<UseTree> from_ast_with_normalization(const RewriteContext& ctx, const rust::Item* item);
    static UseTree from_ast(const RewriteContext& ctx, const rust::UseTree* a, std::optional<ListItem> list_item,
                            const rust::Visibility* visibility, std::optional<uint32_t> opt_lo,
                            const rust::AttrVec* attrs);
    UseTree normalize() &&;
    bool has_comment() const;
    // Not ported (only reachable with imports_granularity != Preserve, a
    // non-default config): contains_comment, same_visibility, share_prefix,
    // flatten, merge, nest_trailing_self, merge_rest, merge_use_trees_inner,
    // flatten_use_trees, UseSegment::{equal_except_alias, get_alias, contains_comment}.
};

/// Rust's slice::sort_by for the lengths rustfmt sorts here: driftsort runs
/// insertion_sort_shift_left for len <= 20, which is reproduced exactly (so
/// an ordering that is not a strict weak order still comes out as rustfmt's);
/// longer slices use std::stable_sort (identical for a consistent ordering).
template <class T, class Less>
void rust_stable_sort(std::vector<T>& v, Less is_less) {
    if (v.size() < 2) return;
    if (v.size() <= 20) {
        for (size_t i = 1; i < v.size(); i++) {
            if (!is_less(v[i], v[i - 1])) continue;
            T tmp = std::move(v[i]);
            size_t j = i;
            do {
                v[j] = std::move(v[j - 1]);
                j--;
            } while (j > 0 && is_less(tmp, v[j - 1]));
            v[j] = std::move(tmp);
        }
        return;
    }
    std::stable_sort(v.begin(), v.end(), is_less);
}

/// slice::sort() over UseTree's Ord (stable).
void sort_use_trees(std::vector<UseTree>& v);
/// Vec::dedup() over UseTree's PartialEq.
void dedup_use_trees(std::vector<UseTree>& v);

}  // namespace imports
}  // namespace rustfmt
