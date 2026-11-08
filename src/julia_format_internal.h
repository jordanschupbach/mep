#pragma once

// Internals of mep's Julia formatter (julia_format.h), a port of Runic.jl
// 1.5.0 (MIT, Copyright (c) 2024 Fredrik Ekre and contributors).
//
// The port follows Runic's source function for function, so the files read
// side by side with Runic's:
//   julia_format.cpp         Runic.jl (Node, Context, the format_node! driver,
//                            format toggles) and chisels.jl (tree utilities,
//                            normalize_tree!)
//   julia_format_passes.cpp  runestone.jl, the spacing / literal / semicolon /
//                            `in` / return passes
//   julia_format_indent.cpp  runestone.jl, insert_delete_mark_newlines and the
//                            indent_* family, four_space_indent,
//                            indent_multiline_strings
//
// Runic's Node is an immutable struct whose kids are a shared, mutable
// Vector{Node}; several passes mutate kid vectors in place and rely on the
// aliasing (`kids′ === kids`). Node keeps that model: `kids` is a shared_ptr
// (null for a leaf), copies share the vector, and `===` on kid vectors is
// pointer equality. Kid vectors are indexed 1-based through at()/find*() to
// keep the Julia index arithmetic verbatim.
//
// Runic's `@assert`s, `unreachable()`s and failed type assertions abort the
// formatting of the whole file; here they throw Refuse, and Format() reports
// ok=false -- the same files Runic refuses.

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "julia_parse.h"

namespace juliafmt::detail {

using jlparse::K;
using jlparse::Kind;
using jlparse::RawFlags;

/** @brief Thrown where Runic would throw (an @assert, unreachable(), a type assertion). */
struct Refuse : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void RefuseAt(const char *what, int line);
#define JF_ASSERT(c)                                    \
    do {                                                \
        if (!(c)) ::juliafmt::detail::RefuseAt(#c, __LINE__); \
    } while (0)
#define JF_UNREACHABLE() ::juliafmt::detail::RefuseAt("unreachable", __LINE__)

using Tag = uint32_t;
inline constexpr Tag TAG_INDENT = Tag(1) << 0;
inline constexpr Tag TAG_DEDENT = Tag(1) << 1;
inline constexpr Tag TAG_PRE_DEDENT = Tag(1) << 2;
inline constexpr Tag TAG_LINE_CONT = Tag(1) << 31;
inline constexpr Tag TAG_TRAILING_COMMA = Tag(1) << 4;
inline constexpr Tag TAG_TRAILING_COMMA_OPT = Tag(1) << 5;

/** @brief Stand-in for Julia's `nothing` where a Kind is expected. */
inline constexpr Kind kNoKind = static_cast<Kind>(0xffff);

struct Node;
using KidVec = std::vector<Node>;
using KidsP = std::shared_ptr<KidVec>;

/** @brief Runic's Node: a re-packed green node plus formatter tags. */
struct Node {
    Kind kind{};
    RawFlags flags = 0;
    uint32_t span = 0;
    KidsP kids;  ///< null: a leaf
    Tag tags = 0;

    bool leaf() const { return !kids; }
};

using ONode = std::optional<Node>;

/** @brief A leaf node. */
inline Node Leaf(Kind k, RawFlags f, uint32_t span, Tag tags = 0) { return Node{k, f, span, nullptr, tags}; }
/** @brief A node over @p kids (span = sum of the kids' spans), tags 0. */
Node Inner(Kind k, RawFlags f, KidsP kids);
/** @brief make_node(node, kids′, tags): same head, new kids. */
Node make_node(const Node &node, KidsP kids);
inline Node make_node(const Node &node, KidsP kids, Tag tags) {
    Node n = make_node(node, std::move(kids));
    n.tags = tags;
    return n;
}
/** @brief make_node(leaf, span′). */
Node make_leaf(const Node &node, uint32_t span);
inline Node add_tag(const Node &n, Tag t) {
    Node m = n;
    m.tags |= t;
    return m;
}
inline bool has_tag(const Node &n, Tag t) { return (n.tags & t) != 0; }

// Kid vectors, 1-based.
inline KidsP NewKids() { return std::make_shared<KidVec>(); }
inline KidsP CopyKids(const KidsP &k) { return std::make_shared<KidVec>(*k); }
/** @brief `kids[1:n]` (a copy). */
KidsP SliceKids(const KidsP &k, int n);
inline int len(const KidsP &k) { return static_cast<int>(k->size()); }
Node &at(const KidsP &k, int i);
const KidsP &verified_kids(const Node &n);
inline Kind kindof(const ONode &n) {
    if (!n) throw Refuse("kind(nothing)");
    return n->kind;
}

template <class... Ks> inline bool KIn(Kind k, Ks... ks) { return ((k == ks) || ...); }

inline bool is_ws(const Node &n) { return jlparse::is_whitespace(n.kind); }
inline bool is_trivia(const Node &n) { return jlparse::is_trivia(n.flags); }
inline bool has_flags(const Node &n, RawFlags f) { return jlparse::has_flags(n.flags, f); }

/** @brief findnext(pred, kids, i) -- 1-based, 0 for `nothing`. */
template <class P> int findnext(P &&pred, const KidsP &k, int i) {
    if (i < 1) throw Refuse("BoundsError");
    for (int j = i; j <= len(k); ++j)
        if (pred((*k)[static_cast<size_t>(j - 1)])) return j;
    return 0;
}
template <class P> int findfirst(P &&pred, const KidsP &k) { return findnext(pred, k, 1); }
/** @brief findprev(pred, kids, i) -- 1-based, 0 for `nothing`. */
template <class P> int findprev(P &&pred, const KidsP &k, int i) {
    if (i > len(k)) throw Refuse("BoundsError");
    for (int j = i; j >= 1; --j)
        if (pred((*k)[static_cast<size_t>(j - 1)])) return j;
    return 0;
}
template <class P> int findlast(P &&pred, const KidsP &k) { return findprev(pred, k, len(k)); }
/** @brief `x::Int` on a find result. */
inline int need(int i) {
    if (i == 0) throw Refuse("TypeError: nothing::Int");
    return i;
}
inline auto IsKind(Kind k) {
    return [k](const Node &n) { return n.kind == k; };
}
inline auto NotWs() {
    return [](const Node &n) { return !is_ws(n); };
}
/** @brief kmatch(kids, kinds, i). */
bool kmatch(const KidsP &kids, std::initializer_list<Kind> kinds, int i = 1);

// chisels.jl
ONode first_leaf(const Node &n);
ONode last_leaf(const Node &n);
ONode second_last_leaf(const Node &n);
int meta_nargs(const Node &n);
bool is_assignment(const Node &n);
bool is_infix_op_call(const Node &n);
Node infix_op_call_op(const Node &n);
bool is_comparison_leaf(const Node &n);
bool is_operator_leaf(const Node &n);
Node first_non_whitespace_kid(const Node &n);
bool is_begin_block(const Node &n, Kind token = K("begin"));
bool is_paren_block(const Node &n);
bool is_global_local_list(const Node &n);
bool is_string_macro(const Node &n);
bool is_triple_string(const Node &n);
bool is_triple_string_macro(const Node &n);
bool is_triple_thing(const Node &n);
template <class P> bool predicate_contains(P &&pred, const Node &n) {
    if (pred(n)) return true;
    if (n.leaf()) return false;
    for (const Node &k : *n.kids)
        if (predicate_contains(pred, k)) return true;
    return false;
}
template <class P> bool any_leaf(P &&pred, const Node &n) {
    if (n.leaf()) return pred(n);
    for (const Node &k : *n.kids)
        if (any_leaf(pred, k)) return true;
    return false;
}
ONode replace_first_leaf(const Node &n, const ONode &kid);  // nullopt = nullnode
ONode replace_last_leaf(const Node &n, const ONode &kid);
Node add_before_first_leaf(const Node &n, const Node &kid);
ONode continue_newlines(const Node &n, bool leading = true, bool trailing = true);

/** @brief Runic's output IOBuffer: the formatted text and a position. */
struct FmtIO {
    std::string buf;
    size_t pos = 0;
    void seek(size_t p) { pos = p < buf.size() ? p : buf.size(); }
};

/** @brief Runic's Context (the fields the formatting passes use). */
struct Ctx {
    FmtIO io;
    bool filemode = true;
    int indent_level = 0;
    bool format_on = true;
    Kind next_sibling = kNoKind;  ///< kind of the next sibling, kNoKind for `nothing`
    std::vector<Kind> lineage_kinds;
    std::vector<std::string> lineage_macros;

    size_t position() const { return io.pos; }
    void seek(size_t p) { io.seek(p); }
    /** @brief accept_node!: advance over the node's bytes. */
    void accept(const Node &n) { io.seek(io.pos + n.span); }
    /** @brief read_bytes(ctx, node). */
    std::string read_bytes(const Node &n) const;
    /** @brief replace_bytes!(ctx, bytes, size): returns the bytes written. */
    uint32_t replace_bytes(std::string_view bytes, size_t size);
    /** @brief peek(io) of the byte at the position. */
    uint8_t peek() const;
    Kind lineage_end() const {
        if (lineage_kinds.empty()) throw Refuse("BoundsError: lineage_kinds[end]");
        return lineage_kinds.back();
    }
};

bool is_variable_assignment(const Ctx &ctx, const Node &n);
bool is_multiline_between_idxs(Ctx &ctx, const Node &node, int opening_idx, int closing_idx);
bool contains_multiline_triple_string(Ctx &ctx, const Node &node);
std::string macrocall_name(Ctx &ctx, const Node &node);
bool safe_to_insert_return(const Ctx &ctx);

/** @brief format_node!: nullopt = accepted, else the replacement. */
ONode format_node(Ctx &ctx, const Node &node);

// julia_format_passes.cpp
ONode trim_trailing_whitespace(Ctx &ctx, const Node &node);
ONode replace_tabs_with_four_spaces(Ctx &ctx, const Node &node);
ONode format_hex_literals(Ctx &ctx, const Node &node);
ONode format_float_literals(Ctx &ctx, const Node &node);
ONode spaces_in_listlike(Ctx &ctx, const Node &node);
ONode spaces_around_operators(Ctx &ctx, const Node &node);
ONode spaces_around_assignments(Ctx &ctx, const Node &node);
ONode spaces_around_anonymous_function(Ctx &ctx, const Node &node);
ONode spaces_around_ternary(Ctx &ctx, const Node &node);
ONode spaces_in_export_public(Ctx &ctx, const Node &node);
ONode spaces_in_let(Ctx &ctx, const Node &node);
ONode spaces_in_import_using(Ctx &ctx, const Node &node);
ONode no_spaces_around_colon_etc(Ctx &ctx, const Node &node);
ONode spaces_around_keywords(Ctx &ctx, const Node &node);
ONode for_loop_use_in(Ctx &ctx, const Node &node);
ONode braces_around_where_rhs(Ctx &ctx, const Node &node);
ONode parens_around_op_calls_in_colon(Ctx &ctx, const Node &node);
ONode no_leading_and_single_trailing_newline(Ctx &ctx, const Node &node);
ONode max_three_consecutive_newlines(Ctx &ctx, const Node &node);
ONode remove_trailing_semicolon(Ctx &ctx, const Node &node);
ONode spaces_around_comments(Ctx &ctx, const Node &node);
ONode explicit_return(Ctx &ctx, const Node &node);

// julia_format_indent.cpp
ONode four_space_indent(Ctx &ctx, const Node &node);
ONode insert_delete_mark_newlines(Ctx &ctx, const Node &node);
ONode indent_multiline_strings(Ctx &ctx, const Node &node);

}  // namespace juliafmt::detail
