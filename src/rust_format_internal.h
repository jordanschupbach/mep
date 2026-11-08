// rust_format_internal.h -- the shared core of mep's Rust formatter: a
// structure-for-structure port of rustfmt (ported from rustfmt
// (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0)).
//
// Every rustfmt module becomes one (or a few) rust_format_<module>.cpp files;
// this header is the cross-module surface: Config / Indent / Shape /
// RewriteResult / RewriteContext / SnippetProvider (rustfmt's config, shape.rs,
// rewrite.rs, visitor.rs's SnippetProvider), the Spanned trait, and the
// pub(crate) functions and types each module exports, under a heading per
// module. Rust -> C++ conventions used throughout:
//
//  - usize is size_t (wrapping arithmetic, exactly like rustfmt's release
//    build); `saturating_sub` is sat_sub(); `checked_sub` is checked_sub().
//  - Option<String> is Opt (std::optional<std::string>); RewriteResult is a
//    small Expected (ok value or RewriteError). `.ok()` -> .opt();
//    `.unknown_error()` -> unknown_error(opt); `?` is written out.
//  - `impl Rewrite for T` is the overload `rewrite_result(ctx, const T*, shape)`
//    (and `rewrite()` = its .opt()); `impl Spanned for T` is `spanned(const T*)`.
//    Generic code over "R: Rewrite + Spanned" takes a RwNode (a tagged pointer
//    to any rewritable node), see the overflow.rs section.
//  - &str results that rustfmt borrows from the source are std::string_view
//    into ctx.src(); owned strings are std::string.
//  - panic!() / unreachable!() / a bad span throw FormatPanic; the depth cap
//    and the work budget throw it too. Format() catches it and refuses.
//  - The config is rustfmt's default config: branches for non-default option
//    values are dropped (with a "// non-default config" note where rustfmt
//    branches), except style_edition 2015 vs 2024, which both exist.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rust_ast.h"
#include "rust_format.h"
#include "rust_lex.h"
#include "rust_parse.h"

namespace rustfmt {

using rust::Span;
using Opt = std::optional<std::string>;

// ===========================================================================
// Panics, depth cap, work budget.

struct FormatPanic : std::runtime_error {
    uint32_t pos = 0;  // byte offset it is about, when known (0 = none)
    explicit FormatPanic(const std::string& what, uint32_t at = 0) : std::runtime_error(what), pos(at) {}
};

[[noreturn]] void panic(const std::string& what, uint32_t at = 0);

// ===========================================================================
// Small numeric helpers (Rust usize semantics).

inline size_t sat_sub(size_t a, size_t b) { return a > b ? a - b : 0; }
inline std::optional<size_t> checked_sub(size_t a, size_t b) {
    if (b > a) return std::nullopt;
    return a - b;
}

// ===========================================================================
// config (rustfmt's Config with its defaults; the width heuristics follow
// `use_small_heuristics = "Default"`, scaled by max_width as rustfmt does).

enum class StyleEdition : uint8_t { E2015, E2018, E2021, E2024, E2027 };
enum class IndentStyle : uint8_t { Visual, Block };

struct Config {
    size_t max_width_ = 100;
    size_t tab_spaces_ = 4;
    rust::Edition edition_ = rust::Edition::E2015;
    StyleEdition style_edition_ = StyleEdition::E2015;

    size_t max_width() const { return max_width_; }
    size_t tab_spaces() const { return tab_spaces_; }
    bool hard_tabs() const { return false; }
    rust::Edition edition() const { return edition_; }
    StyleEdition style_edition() const { return style_edition_; }
    IndentStyle indent_style() const { return IndentStyle::Block; }
    // Width heuristics (`use_small_heuristics = Default`): value * max_width / 100.
    // WidthHeuristics::scaled: unchanged up to max_width 100; above, times the
    // ratio rounded to 0.1 (f32 arithmetic, as rustfmt).
    size_t scaled(size_t v) const {
        if (max_width_ <= 100) return v;
        float ratio = std::round(static_cast<float>(max_width_) / 100.0f * 10.0f) / 10.0f;
        return static_cast<size_t>(std::lround(static_cast<float>(v) * ratio));
    }
    size_t fn_call_width() const { return scaled(60); }
    size_t attr_fn_like_width() const { return scaled(70); }
    size_t struct_lit_width() const { return scaled(18); }
    size_t struct_variant_width() const { return scaled(35); }
    size_t array_width() const { return scaled(60); }
    size_t chain_width() const { return scaled(60); }
    size_t single_line_if_else_max_width() const { return scaled(50); }
    size_t single_line_let_else_max_width() const { return scaled(50); }
    size_t short_array_element_width_threshold() const { return 10; }
    size_t comment_width() const { return 80; }
    size_t doc_comment_code_block_width() const { return 100; }
    size_t blank_lines_upper_bound() const { return 1; }
    size_t blank_lines_lower_bound() const { return 0; }
    // Boolean options at their defaults (kept as functions so ported code reads like rustfmt).
    bool wrap_comments() const { return false; }
    bool format_code_in_doc_comments() const { return false; }
    bool normalize_comments() const { return false; }
    bool normalize_doc_attributes() const { return false; }
    bool format_strings() const { return false; }
    bool format_macro_matchers() const { return false; }
    bool format_macro_bodies() const { return true; }
    bool empty_item_single_line() const { return true; }
    bool struct_lit_single_line() const { return true; }
    bool fn_single_line() const { return false; }
    bool where_single_line() const { return false; }
    bool reorder_imports() const { return true; }
    bool reorder_modules() const { return true; }
    bool reorder_impl_items() const { return false; }
    bool space_before_colon() const { return false; }
    bool space_after_colon() const { return true; }
    bool spaces_around_ranges() const { return false; }
    bool remove_nested_parens() const { return true; }
    bool combine_control_expr() const { return true; }
    bool overflow_delimited_expr() const { return false; }
    bool match_arm_blocks() const { return true; }
    bool match_arm_indent() const { return true; }
    bool force_multiline_blocks() const { return false; }
    bool trailing_semicolon() const { return true; }
    bool match_block_trailing_comma() const { return false; }
    bool merge_derives() const { return true; }
    bool use_try_shorthand() const { return false; }
    bool use_field_init_shorthand() const { return false; }
    bool force_explicit_abi() const { return true; }
    bool condense_wildcard_suffixes() const { return false; }
    bool format_generated_files() const { return true; }
    bool error_on_line_overflow() const { return false; }
    /// true only while formatting a macro_rules! body as a snippet (lib.rs format_snippet).
    bool error_on_unformatted_ = false;
    bool error_on_unformatted() const { return error_on_unformatted_; }
    size_t struct_field_align_threshold() const { return 0; }
    size_t enum_discrim_align_threshold() const { return 0; }
    size_t inline_attribute_width() const { return 0; }
    // binop_separator = Front, imports_indent = Block, imports_layout = Mixed,
    // imports_granularity = Preserve, group_imports = Preserve,
    // type_punctuation_density = Wide, fn_params_layout = Tall,
    // brace_style = SameLineWhere, control_brace_style = AlwaysSameLine,
    // trailing_comma = Vertical, match_arm_leading_pipes = Never,
    // newline_style = Auto, hex_literal_case = Preserve,
    // float_literal_trailing_zero = Preserve: all fixed.
};

// ===========================================================================
// shape.rs

struct Indent {
    size_t block_indent = 0;
    size_t alignment = 0;

    Indent() = default;
    Indent(size_t b, size_t a) : block_indent(b), alignment(a) {}
    static Indent from_width(const Config& config, size_t width);
    static Indent empty() { return Indent(0, 0); }
    Indent block_only() const { return Indent(block_indent, 0); }
    Indent block_indent_(const Config& config) const;  // rustfmt's Indent::block_indent(config)
    Indent block_unindent(const Config& config) const;
    size_t width() const { return block_indent + alignment; }
    std::string to_string(const Config& config) const;               // spaces only
    std::string to_string_with_newline(const Config& config) const;  // "\n" + spaces
    std::string to_string_inner(const Config& config, size_t offset) const;
    Indent operator+(Indent o) const { return Indent(block_indent + o.block_indent, alignment + o.alignment); }
    Indent operator-(Indent o) const { return Indent(block_indent - o.block_indent, alignment - o.alignment); }
    Indent operator+(size_t n) const { return Indent(block_indent, alignment + n); }
    Indent operator-(size_t n) const { return Indent(block_indent, alignment - n); }
};

struct ExceedsMaxWidthError {
    size_t configured_width = 0;
    Span span;
};

constexpr size_t INFINITE_SHAPE_WIDTH = 8096;

struct Shape {
    size_t width = 0;
    Indent indent;
    size_t offset = 0;

    static Shape legacy(size_t width, Indent indent) { return Shape{width, indent, indent.alignment}; }
    static Shape indented(Indent indent, const Config& config) {
        return Shape{sat_sub(config.max_width(), indent.width()), indent, indent.alignment};
    }
    Shape with_max_width(const Config& config) const {
        Shape s = *this;
        s.width = sat_sub(config.max_width(), indent.width());
        return s;
    }
    Shape visual_indent(size_t delta) const;
    Shape block_indent(size_t delta) const;
    std::optional<Shape> block_left(size_t delta) const { return block_indent(delta).sub_width_opt(delta); }
    Shape add_offset(size_t delta) const {
        Shape s = *this;
        s.offset += delta;
        return s;
    }
    Shape block() const {
        Shape s = *this;
        s.indent = indent.block_only();
        return s;
    }
    Shape saturating_sub_width(size_t delta) const {
        Shape s = *this;
        s.width = sat_sub(width, delta);
        return s;
    }
    std::optional<Shape> sub_width_opt(size_t delta) const {
        if (delta > width) return std::nullopt;
        Shape s = *this;
        s.width = width - delta;
        return s;
    }
    std::optional<Shape> shrink_left_opt(size_t delta) const {
        if (delta > width) return std::nullopt;
        return Shape{width - delta, indent + delta, offset + delta};
    }
    std::optional<Shape> offset_left_opt(size_t delta) const { return add_offset(delta).sub_width_opt(delta); }
    // The Result<Shape, ExceedsMaxWidthError> forms: same as the _opt ones
    // (callers that need the error build it with exceeds(span)).
    std::optional<Shape> sub_width(size_t delta) const { return sub_width_opt(delta); }
    std::optional<Shape> shrink_left(size_t delta) const { return shrink_left_opt(delta); }
    std::optional<Shape> offset_left(size_t delta) const { return offset_left_opt(delta); }
    ExceedsMaxWidthError exceeds(Span span) const { return ExceedsMaxWidthError{width, span}; }
    size_t used_width() const { return indent.block_indent + offset; }
    size_t rhs_overhead(const Config& config) const { return sat_sub(config.max_width(), used_width() + width); }
    Shape comment(const Config& config) const;
    std::string to_string_with_newline(const Config& config) const;
    Shape infinite_width() const {
        Shape s = *this;
        s.width = INFINITE_SHAPE_WIDTH;
        return s;
    }
};

// ===========================================================================
// rewrite.rs

enum class MacroErrorKind : uint8_t { ParseFailure, ReplaceMacroVariable, Unknown };

struct RewriteError {
    enum Kind : uint8_t { NotImplementedMacro, MacroFailure, ExceedsMaxWidth, SkipFormatting, Unknown };
    Kind kind = Unknown;
    MacroErrorKind macro_kind = MacroErrorKind::Unknown;
    size_t configured_width = 0;
    Span span;
    static RewriteError unknown() { return RewriteError{}; }
    static RewriteError skip() { return RewriteError{SkipFormatting, MacroErrorKind::Unknown, 0, Span()}; }
    static RewriteError exceeds(size_t w, Span s) { return RewriteError{ExceedsMaxWidth, MacroErrorKind::Unknown, w, s}; }
    static RewriteError exceeds(const ExceedsMaxWidthError& e) { return exceeds(e.configured_width, e.span); }
    static RewriteError macro_failure(MacroErrorKind k, Span s) { return RewriteError{MacroFailure, k, 0, s}; }
};

/// Result<T, RewriteError>.
template <class T>
class Expected {
public:
    Expected(T v) : val_(std::move(v)) {}  // NOLINT: implicit like Ok(..)
    Expected(RewriteError e) : err_(e) {}  // NOLINT: implicit like Err(..)
    bool ok() const { return val_.has_value(); }
    explicit operator bool() const { return ok(); }
    T& operator*() { return *val_; }
    const T& operator*() const { return *val_; }
    T* operator->() { return &*val_; }
    const T* operator->() const { return &*val_; }
    T& value() { return *val_; }
    const T& value() const { return *val_; }
    const RewriteError& error() const { return err_; }
    std::optional<T> opt() const& { return val_; }
    std::optional<T> opt() && { return std::move(val_); }

private:
    std::optional<T> val_;
    RewriteError err_;
};

using RewriteResult = Expected<std::string>;

inline RewriteResult Err(RewriteError e) { return RewriteResult(e); }
inline RewriteResult unknown_error(Opt o) {
    if (o) return RewriteResult(std::move(*o));
    return RewriteResult(RewriteError::unknown());
}
inline RewriteResult max_width_error(Opt o, size_t width, Span span) {
    if (o) return RewriteResult(std::move(*o));
    return RewriteResult(RewriteError::exceeds(width, span));
}

/// skip.rs: SkipContext / SkipNameContext.
struct SkipNameContext {
    bool all = false;
    std::unordered_set<std::string> values;
    void extend(const std::vector<std::string>& names) {
        if (!all) values.insert(names.begin(), names.end());
    }
    void update(const SkipNameContext& other);
    bool skip(std::string_view name) const { return all || values.count(std::string(name)) != 0; }
    void skip_all() {
        all = true;
        values.clear();
    }
};
struct SkipContext {
    SkipNameContext macros;
    SkipNameContext attributes;
    /// get_skip_names reads `#[rustfmt::skip::macros(..)]` argument tokens from pr.tokens.
    void update_with_attrs(const rust::ParseResult& pr, const rust::AttrVec& attrs);
    void update(const SkipContext& other) {
        macros.update(other.macros);
        attributes.update(other.attributes);
    }
};
bool is_skip_attr(const std::vector<rust::PathSegment>& segments);

/// visitor.rs: SnippetProvider over the text being formatted (spans are
/// byte offsets into it).
class SnippetProvider {
public:
    explicit SnippetProvider(std::string_view text) : text_(text) {}
    std::optional<std::string_view> span_to_snippet(Span span) const {
        if (span.lo > span.hi || span.hi > text_.size()) return std::nullopt;
        return text_.substr(span.lo, span.hi - span.lo);
    }
    std::string_view entire_snippet() const { return text_; }
    uint32_t start_pos() const { return 0; }
    uint32_t end_pos() const { return static_cast<uint32_t>(text_.size()); }
    // source_map.rs SpanUtils (searches skip comments and string literals: find_uncommented).
    uint32_t span_after(Span original, std::string_view needle) const;       // panics if absent
    uint32_t span_after_last(Span original, std::string_view needle) const;
    uint32_t span_before(Span original, std::string_view needle) const;      // panics if absent
    uint32_t span_before_last(Span original, std::string_view needle) const;
    std::optional<uint32_t> opt_span_after(Span original, std::string_view needle) const;
    std::optional<uint32_t> opt_span_before(Span original, std::string_view needle) const;

private:
    std::string_view text_;
};

/// Shared, mutable state of one Format() call (rustfmt's ParseSess + report bits).
struct FormatState {
    uint64_t work = 0;         // rewrite steps so far
    uint64_t work_budget = 0;  // FormatPanic when exceeded
    uint32_t depth = 0;        // current rewrite nesting
    uint32_t max_depth = 200;
    /// FormatReport: something was left unformatted / a line could not be
    /// made to fit. rustfmt reports these as errors but still emits the file.
    /// (comment.rs recover_comment_removed with error_on_unformatted -> LostComment;
    /// format_lines' trailing-whitespace check -> TrailingWhitespace.)
    bool has_unformatted_code = false;
    bool has_macro_format_failure = false;
};

/// RAII depth guard: throws FormatPanic past FormatState::max_depth, and
/// counts one unit of work (FormatPanic past the budget).
class DepthGuard {
public:
    explicit DepthGuard(FormatState& st);
    ~DepthGuard() { st_.depth--; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

private:
    FormatState& st_;
};

struct RewriteContext {
    rust::ParseResult* pr = nullptr;  // the file being formatted (arena for synthetic nodes)
    const Config* config = nullptr;
    const SnippetProvider* snippet_provider = nullptr;
    FormatState* state = nullptr;
    std::shared_ptr<bool> inside_macro_ = std::make_shared<bool>(false);
    mutable bool use_block = false;
    mutable bool is_if_else_block_ = false;
    mutable bool is_loop_block_ = false;
    mutable bool force_one_line_chain = false;
    mutable bool macro_rewrite_failure = false;
    bool is_macro_def = false;
    SkipContext skip_context;
    std::shared_ptr<std::vector<std::pair<size_t, size_t>>> skipped_range =
        std::make_shared<std::vector<std::pair<size_t, size_t>>>();

    std::string_view src() const { return snippet_provider->entire_snippet(); }
    /// context.snippet(span): panics on a span outside the text.
    std::string_view snippet(Span span) const;
    bool use_block_indent() const { return true; }
    size_t budget(size_t used_width) const { return sat_sub(config->max_width(), used_width); }
    bool inside_macro() const { return *inside_macro_; }
    /// enter_macro(): sets inside_macro, returns the previous value
    /// (InsideMacroGuard::is_nested); restore with `*ctx.inside_macro_ = prev`.
    bool enter_macro() const {
        bool prev = *inside_macro_;
        *inside_macro_ = true;
        return prev;
    }
    void leave_macro() const { *inside_macro_ = false; }
    bool is_if_else_block() const { return is_if_else_block_; }
    bool is_loop_block() const { return is_loop_block_; }
    rust::Arena& arena() const { return *pr->arena; }
    rust::Edition edition() const { return config->edition(); }
};

/// RAII form of rustfmt's InsideMacroGuard.
class InsideMacroGuard {
public:
    explicit InsideMacroGuard(const RewriteContext& ctx) : ctx_(ctx), prev_(ctx.enter_macro()) {}
    ~InsideMacroGuard() { *ctx_.inside_macro_ = prev_; }
    bool is_nested() const { return prev_; }
    InsideMacroGuard(const InsideMacroGuard&) = delete;
    InsideMacroGuard& operator=(const InsideMacroGuard&) = delete;

private:
    const RewriteContext& ctx_;
    bool prev_;
};

// ===========================================================================
// spanned.rs: Spanned, with rustfmt's attribute-inclusive spans.

/// rustc's Span::new: swaps lo and hi when lo > hi.
inline Span mk_sp(uint32_t lo, uint32_t hi) { return lo <= hi ? Span(lo, hi) : Span(hi, lo); }
inline Span mk_sp_lo_plus_one(uint32_t lo) { return Span(lo, lo + 1); }

Span spanned(const rust::Item* x);
Span spanned(const rust::Expr* x);
Span spanned(const rust::ExprField* x);
Span spanned(const rust::Local* x);
Span spanned(const rust::WherePredicate* x);
Span spanned(const rust::Stmt* x);
Span spanned(const rust::Pat* x);
Span spanned(const rust::Ty* x);
Span spanned(const rust::Arm* x);
Span spanned(const rust::Param* x);
Span spanned(const rust::GenericParam* x);
Span spanned(const rust::FieldDef* x);
Span spanned(const rust::FnRetTy* x);
Span spanned(const rust::GenericArg* x);
Span spanned(const rust::GenericBound* x);
Span spanned(const rust::PreciseCapturingArg* x);

// ===========================================================================
// utils.rs (rust_format_utils.cpp)

std::string_view rewrite_ident(const RewriteContext& ctx, const rust::Ident& ident);
size_t extra_offset(std::string_view text, Shape shape);
bool is_same_visibility(const RewriteContext& ctx, const rust::Visibility& a, const rust::Visibility& b);
std::string format_visibility(const RewriteContext& ctx, const rust::Visibility& vis);
std::string format_impl_restriction(const RewriteContext& ctx, const rust::ImplRestriction& r);
const char* format_coro(const rust::CoroutineKind& k);  // "" for None
const char* format_constness(const rust::Constness& c);
const char* format_constness_right(const rust::Constness& c);
const char* format_defaultness(const rust::Defaultness& d);
const char* format_safety(const rust::Safety& s);
const char* format_auto(bool is_auto);
const char* format_mutability(rust::Mutability m);
std::pair<const char*, const char*> format_pinnedness_and_mutability(rust::Pinnedness p, rust::Mutability m);
std::string format_extern(const RewriteContext& ctx, const rust::Extern& ext, bool explicit_abi);
rust::AttrVec filter_attributes(const rust::AttrVec& attrs, rust::AttrStyle style);
rust::AttrVec inner_attributes(const rust::AttrVec& attrs);
rust::AttrVec outer_attributes(const rust::AttrVec& attrs);
bool is_single_line(std::string_view s);
bool first_line_contains_single_line_comment(std::string_view s);
bool last_line_contains_single_line_comment(std::string_view s);
bool is_attributes_extendable(std::string_view attrs_str);
size_t first_line_width(std::string_view s);
size_t last_line_width(std::string_view s);
size_t last_line_used_width(std::string_view s, size_t offset);
size_t trimmed_last_line_width(std::string_view s);
bool last_line_extendable(std::string_view s);
bool contains_skip(const RewriteContext& ctx, const rust::AttrVec& attrs);
bool semicolon_for_expr(const RewriteContext& ctx, const rust::Expr* expr);
bool semicolon_for_stmt(const RewriteContext& ctx, const rust::Stmt* stmt, bool is_last_expr);
const rust::Expr* stmt_expr(const rust::Stmt* stmt);
std::pair<size_t, size_t> count_lf_crlf(std::string_view input);
size_t count_newlines(std::string_view input);
Opt wrap_str(std::string s, size_t max_width, Shape shape);
bool filtered_str_fits(std::string_view snippet, size_t max_width, Shape shape);
const char* colon_spaces(const Config& config);
const rust::Expr* left_most_sub_expr(const rust::Expr* e);
bool starts_with_newline(std::string_view s);
bool first_line_ends_with(std::string_view s, char c);
bool is_block_expr(const RewriteContext& ctx, const rust::Expr* expr, std::string_view repr);
std::string remove_trailing_white_spaces(std::string_view text);
Opt trim_left_preserve_layout(std::string_view orig, Indent indent, const Config& config);
bool is_empty_line(std::string_view s);
/// indent_next_line (takes a FullCodeCharKind, declared in the comment.rs section).
bool indent_next_line(uint8_t full_code_char_kind, std::string_view line, const Config& config);
size_t unicode_str_width(std::string_view s);
// String helpers mirroring Rust's str methods.
std::string_view trim(std::string_view s);        // str::trim (Unicode White_Space)
std::string_view trim_start(std::string_view s);  // str::trim_start
std::string_view trim_end(std::string_view s);    // str::trim_end
bool starts_with(std::string_view s, std::string_view p);
bool ends_with(std::string_view s, std::string_view p);
bool contains(std::string_view s, std::string_view p);
bool contains(std::string_view s, char c);
std::vector<std::string_view> lines(std::string_view s);  // str::lines (drops one trailing "\n", strips "\r")
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string repeat(std::string_view s, size_t n);
/// pprust::path_to_string (segments joined by "::", generic args included as written).
std::string path_to_string(const RewriteContext& ctx, const rust::Path* path);
bool path_is_global(const rust::Path* path);  // first segment is `{{root}}` (empty ident)

// ===========================================================================
// comment.rs, string.rs (rust_format_comment.cpp)

enum class CommentStyleKind : uint8_t { DoubleSlash, TripleSlash, Doc, SingleBullet, DoubleBullet, Exclamation, Custom };
struct CommentStyle {
    CommentStyleKind kind = CommentStyleKind::DoubleSlash;
    std::string_view custom;  // Custom: the opener
    bool is_line_comment() const;
    bool is_block_comment() const;
    bool is_doc_comment() const;
    std::string_view opener() const;
    std::string_view closer() const;
    std::string_view line_start() const;
};
CommentStyle comment_style(std::string_view orig, bool normalize_comments);

enum class CodeCharKind : uint8_t { Normal, Comment };
enum class FullCodeCharKind : uint8_t {
    Normal, StartComment, InComment, EndComment, StartStringCommented, EndStringCommented,
    InStringCommented, InString, StartString, EndString,
};
bool fcck_is_comment(FullCodeCharKind k);
bool fcck_inside_comment(FullCodeCharKind k);
bool fcck_is_string(FullCodeCharKind k);
bool fcck_is_commented_string(FullCodeCharKind k);
CodeCharKind fcck_to_codecharkind(FullCodeCharKind k);

/// CharClasses over a string: (kind, byte offset, char) per character.
struct ClassifiedChar {
    FullCodeCharKind kind;
    size_t pos;   // byte offset of the char
    uint32_t ch;  // the code point
    size_t len;   // its UTF-8 length
};
std::vector<ClassifiedChar> char_classes(std::string_view s);
/// LineClasses: (kind, line) per line ("\n" stripped).
std::vector<std::pair<FullCodeCharKind, std::string>> line_classes(std::string_view s);
/// CommentCodeSlices: (kind, start offset, slice).
std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> comment_code_slices(std::string_view s);
/// UngroupedCommentCodeSlices (used by missed_spans).
std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> ungrouped_comment_code_slices(std::string_view s);

/// FindUncommented::find_uncommented.
std::optional<size_t> find_uncommented(std::string_view s, std::string_view pat);
/// FindUncommented::find_last_uncommented.
std::optional<size_t> find_last_uncommented(std::string_view s, std::string_view pat);
bool is_last_comment_block(std::string_view s);
RewriteResult combine_strs_with_missing_comments(const RewriteContext& ctx, std::string_view prev_str,
                                                 std::string_view next_str, Span span, Shape shape,
                                                 bool allow_extend);
RewriteResult rewrite_doc_comment(std::string_view orig, Shape shape, const Config& config);
RewriteResult rewrite_comment(std::string_view orig, bool block_style, Shape shape, const Config& config);
RewriteResult rewrite_missing_comment(Span span, Shape shape, const RewriteContext& ctx);
RewriteResult recover_missing_comment_in_span(Span span, Shape shape, const RewriteContext& ctx,
                                              size_t used_width);
std::optional<size_t> find_comment_end(std::string_view s);
bool contains_comment(std::string_view text);
std::string recover_comment_removed(std::string new_, Span span, const RewriteContext& ctx);
std::string filter_normal_code(std::string_view code);

struct StringFormat {
    std::string_view opener = "\"";
    std::string_view closer = "\"";
    std::string_view line_start = " ";
    std::string_view line_end = "\\";
    Shape shape;
    bool trim_end = false;
    const Config* config = nullptr;
    StringFormat(Shape s, const Config& c) : shape(s), config(&c) {}
};
Opt rewrite_string(std::string_view orig, const StringFormat& fmt, size_t newline_max_chars);

// ===========================================================================
// attr.rs (rust_format_attr.cpp)

/// rustc_ast's MetaItem / MetaItemInner, built from an attribute's tokens by
/// parse_meta() (rustc's `Attribute::meta()`: None when the args are not a
/// meta-item grammar, e.g. `#[doc = include_str!(..)]` or `#[attr(a b)]`).
struct MetaItem;
struct MetaItemInner {
    bool is_lit = false;
    rust::Lit lit;           // is_lit
    MetaItem* mi = nullptr;  // !is_lit
    Span span;
};
struct MetaItem {
    enum Kind : uint8_t { Word, List, NameValue } kind = Word;
    rust::Safety unsafety;
    rust::Path* path = nullptr;
    std::vector<MetaItemInner> list;  // List
    rust::Lit value;                  // NameValue
    Span list_span;                   // List: the parenthesised part
    Span span;
};
/// Attribute::meta(); nodes are allocated in ctx.arena(). null when not a meta item.
MetaItem* parse_meta(const RewriteContext& ctx, const rust::Attribute& attr);
Span spanned(const MetaItemInner* x);

const rust::AttrVec* get_attrs_from_stmt(const rust::Stmt* stmt);  // may be null for Empty
Span get_span_without_attrs(const rust::Stmt* stmt);
rust::AttrVec filter_inline_attrs(const rust::AttrVec& attrs, Span outer_span);
RewriteResult rewrite_result(const RewriteContext& ctx, const MetaItemInner* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const MetaItem* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Attribute* x, Shape shape);
/// impl Rewrite for [ast::Attribute]
RewriteResult rewrite_attrs(const RewriteContext& ctx, const rust::AttrVec& attrs, Shape shape);
RewriteResult rewrite_attrs(const RewriteContext& ctx, const rust::Attribute* attrs, size_t n, Shape shape);

// ===========================================================================
// lists.rs (rust_format_lists.cpp)

enum class DefinitiveListTacticKind : uint8_t { Vertical, Horizontal, Mixed, SpecialMacro };
struct DefinitiveListTactic {
    DefinitiveListTacticKind kind = DefinitiveListTacticKind::Vertical;
    size_t special = 0;  // SpecialMacro(n)
    static DefinitiveListTactic Vertical() { return {DefinitiveListTacticKind::Vertical, 0}; }
    static DefinitiveListTactic Horizontal() { return {DefinitiveListTacticKind::Horizontal, 0}; }
    static DefinitiveListTactic Mixed() { return {DefinitiveListTacticKind::Mixed, 0}; }
    static DefinitiveListTactic SpecialMacro(size_t n) { return {DefinitiveListTacticKind::SpecialMacro, n}; }
    bool operator==(const DefinitiveListTactic& o) const { return kind == o.kind && special == o.special; }
    bool operator!=(const DefinitiveListTactic& o) const { return !(*this == o); }
    bool ends_with_newline(IndentStyle) const { return kind != DefinitiveListTacticKind::Horizontal; }
};
enum class ListTacticKind : uint8_t { Vertical, Horizontal, HorizontalVertical, LimitedHorizontalVertical, Mixed };
struct ListTactic {
    ListTacticKind kind = ListTacticKind::HorizontalVertical;
    size_t limit = 0;
    static ListTactic Vertical() { return {ListTacticKind::Vertical, 0}; }
    static ListTactic Horizontal() { return {ListTacticKind::Horizontal, 0}; }
    static ListTactic HorizontalVertical() { return {ListTacticKind::HorizontalVertical, 0}; }
    static ListTactic LimitedHorizontalVertical(size_t n) { return {ListTacticKind::LimitedHorizontalVertical, n}; }
    static ListTactic Mixed() { return {ListTacticKind::Mixed, 0}; }
};
enum class SeparatorTactic : uint8_t { Always, Never, Vertical };
inline SeparatorTactic separator_tactic_from_bool(bool b) { return b ? SeparatorTactic::Always : SeparatorTactic::Never; }
enum class SeparatorPlace : uint8_t { Front, Back };
SeparatorPlace separator_place_from_tactic(SeparatorPlace def, DefinitiveListTactic tactic, std::string_view sep);
enum class Separator : uint8_t { Comma, VerticalBar };
inline size_t separator_len(Separator s) { return s == Separator::Comma ? 2 : 3; }

struct ListFormatting {
    DefinitiveListTactic tactic_ = DefinitiveListTactic::Vertical();
    std::string_view separator_ = ",";
    SeparatorTactic trailing_separator_ = SeparatorTactic::Never;
    SeparatorPlace separator_place_ = SeparatorPlace::Back;
    Shape shape_;
    bool ends_with_newline_ = true;
    bool preserve_newline_ = false;
    bool nested_ = false;
    bool align_comments_ = true;
    const Config* config_ = nullptr;

    ListFormatting(Shape shape, const Config& config) : shape_(shape), config_(&config) {}
    ListFormatting& tactic(DefinitiveListTactic t) { tactic_ = t; return *this; }
    ListFormatting& separator(std::string_view s) { separator_ = s; return *this; }
    ListFormatting& trailing_separator(SeparatorTactic t) { trailing_separator_ = t; return *this; }
    ListFormatting& separator_place(SeparatorPlace p) { separator_place_ = p; return *this; }
    ListFormatting& ends_with_newline(bool b) { ends_with_newline_ = b; return *this; }
    ListFormatting& preserve_newline(bool b) { preserve_newline_ = b; return *this; }
    ListFormatting& nested(bool b) { nested_ = b; return *this; }
    ListFormatting& align_comments(bool b) { align_comments_ = b; return *this; }
    bool needs_trailing_separator() const;
};

enum class ListItemCommentStyle : uint8_t { SameLine, DifferentLine, None };
struct ListItem {
    Opt pre_comment;
    ListItemCommentStyle pre_comment_style = ListItemCommentStyle::None;
    RewriteResult item = RewriteResult(std::string());
    Opt post_comment;
    bool new_lines = false;

    static ListItem from_item(RewriteResult item);
    static ListItem from_str(std::string s);
    std::string_view inner_as_ref() const { return item.ok() ? std::string_view(*item) : std::string_view(); }
    bool is_different_group() const;
    bool is_multiline() const;
    bool has_single_line_comment() const;
    bool has_comment() const;
    bool is_substantial() const;
};

DefinitiveListTactic definitive_tactic(const std::vector<ListItem>& items, ListTactic tactic, Separator sep,
                                       size_t width);
/// definitive_tactic over a slice [begin, end) (rustfmt passes `&list_items[a..b]`).
DefinitiveListTactic definitive_tactic(const ListItem* begin, const ListItem* end, ListTactic tactic, Separator sep,
                                       size_t width);
RewriteResult write_list(const std::vector<ListItem>& items, const ListFormatting& formatting);
std::pair<Opt, ListItemCommentStyle> extract_pre_comment(std::string_view pre_snippet);
Opt extract_post_comment(std::string_view post_snippet, size_t comment_end, std::string_view separator,
                         bool is_last);
size_t get_comment_end(std::string_view post_snippet, std::string_view separator, std::string_view terminator,
                       bool is_last);
bool has_extra_newline(std::string_view post_snippet, size_t comment_end);

/// itemize_list over `n` elements, eagerly: get_lo/get_hi/get_item_string are
/// called with the element index. (rustfmt's ListItems iterator; the items
/// come out in order, all of them.)
std::vector<ListItem> itemize_list(const SnippetProvider& sp, size_t n, std::string_view terminator,
                                   std::string_view separator, const std::function<uint32_t(size_t)>& get_lo,
                                   const std::function<uint32_t(size_t)>& get_hi,
                                   const std::function<RewriteResult(size_t)>& get_item_string,
                                   uint32_t prev_span_end, uint32_t next_span_start, bool leave_last);
size_t total_item_width(const ListItem& item);
/// struct_lit_shape: nullopt is the ExceedsMaxWidthError.
std::optional<std::pair<std::optional<Shape>, Shape>> struct_lit_shape(Shape shape, const RewriteContext& ctx,
                                                                       size_t prefix_width, size_t suffix_width);
DefinitiveListTactic struct_lit_tactic(std::optional<Shape> h_shape, const RewriteContext& ctx,
                                       const std::vector<ListItem>& items);
Shape shape_for_tactic(DefinitiveListTactic tactic, std::optional<Shape> h_shape, Shape v_shape);
ListFormatting struct_lit_formatting(Shape shape, DefinitiveListTactic tactic, const RewriteContext& ctx,
                                     bool force_no_trailing_comma);

// ===========================================================================
// RwNode: a tagged pointer to anything rewritable (rustfmt's generic
// `R: Rewrite + Spanned` parameters and overflow.rs's OverflowableItem).

struct MacroArg;
struct SegmentParam;
struct TuplePatField;
struct RangeOperand;

struct RwNode {
    enum Kind : uint8_t {
        None, Expr, GenericParam, MacroArg, MetaItemInner, SegmentParam, FieldDef, TuplePatField, Ty, Pat,
        PreciseCapturingArg,
        // Not overflowable, but rewritable + spanned (rewrite_pair / rewrite_assign_rhs operands ...):
        GenericBounds, GenericBound, Lifetime, AnonConst, Path, UseTree, RangeOperand, FnRetTy, TyPat,
        TraitRef, PolyTraitRef, Opaque,
    };
    Kind kind = None;
    const void* p = nullptr;
    // GenericBounds (a list has no span of its own): the span to use.
    Span span_override;
    // Opaque: any other rewritable thing, as closures.
    std::function<RewriteResult(const RewriteContext&, Shape)> opaque_rewrite;

    RwNode() = default;
    RwNode(Kind k, const void* ptr) : kind(k), p(ptr) {}
    static RwNode of(const rust::Expr* e) { return RwNode(Expr, e); }
    static RwNode of(const rust::Ty* t) { return RwNode(Ty, t); }
    static RwNode of(const rust::Pat* t) { return RwNode(Pat, t); }
    static RwNode of(const rust::GenericParam* t) { return RwNode(GenericParam, t); }
    static RwNode of(const rust::FieldDef* t) { return RwNode(FieldDef, t); }
    static RwNode of(const rust::PreciseCapturingArg* t) { return RwNode(PreciseCapturingArg, t); }
    static RwNode of(const struct MetaItemInner* t) { return RwNode(MetaItemInner, t); }
    static RwNode of(const struct MacroArg* t) { return RwNode(MacroArg, t); }
    static RwNode of(const struct SegmentParam* t) { return RwNode(SegmentParam, t); }
    static RwNode of(const struct TuplePatField* t) { return RwNode(TuplePatField, t); }
    static RwNode of(const rust::GenericBound* t) { return RwNode(GenericBound, t); }
    static RwNode of(const rust::Lifetime* t) { return RwNode(Lifetime, t); }
    static RwNode of(const rust::AnonConst* t) { return RwNode(AnonConst, t); }
    static RwNode of(const rust::FnRetTy* t) { return RwNode(FnRetTy, t); }
    static RwNode of(const rust::TyPat* t) { return RwNode(TyPat, t); }
    static RwNode of(const rust::PolyTraitRef* t) { return RwNode(PolyTraitRef, t); }
    static RwNode of(const struct RangeOperand* t) { return RwNode(RangeOperand, t); }
    static RwNode bounds(const rust::GenericBounds* b, Span span) {
        RwNode n(GenericBounds, b);
        n.span_override = span;
        return n;
    }
    static RwNode opaque(Span span, std::function<RewriteResult(const RewriteContext&, Shape)> f) {
        RwNode n(Opaque, nullptr);
        n.span_override = span;
        n.opaque_rewrite = std::move(f);
        return n;
    }
    template <class T>
    const T* as() const { return static_cast<const T*>(p); }
};
RewriteResult rewrite_result(const RewriteContext& ctx, const RwNode& n, Shape shape);
inline Opt rewrite(const RewriteContext& ctx, const RwNode& n, Shape shape) { return rewrite_result(ctx, n, shape).opt(); }
Span spanned(const RwNode& n);

// ===========================================================================
// overflow.rs (rust_format_overflow.cpp)

using OverflowableItem = RwNode;  // restricted to the first block of RwNode kinds
bool overflowable_is_simple(const OverflowableItem& it);
bool overflowable_is_expr(const OverflowableItem& it);
bool overflowable_is_nested_call(const OverflowableItem& it);
const rust::Expr* overflowable_to_expr(const OverflowableItem& it);
bool overflowable_can_be_overflowed(const OverflowableItem& it, const RewriteContext& ctx, size_t len);

/// Delimiter for rewrite_with_square_brackets' delim_token.
enum class Delimiter : uint8_t { Parenthesis, Brace, Bracket, Invisible };

RewriteResult rewrite_with_parens(const RewriteContext& ctx, std::string_view ident,
                                  const std::vector<OverflowableItem>& items, Shape shape, Span span,
                                  size_t item_max_width, std::optional<SeparatorTactic> force_separator_tactic);
RewriteResult rewrite_with_angle_brackets(const RewriteContext& ctx, std::string_view ident,
                                          const std::vector<OverflowableItem>& items, Shape shape, Span span);
RewriteResult rewrite_with_square_brackets(const RewriteContext& ctx, std::string_view name,
                                           const std::vector<OverflowableItem>& items, Shape shape, Span span,
                                           std::optional<SeparatorTactic> force_separator_tactic,
                                           std::optional<Delimiter> delim_token);
std::optional<std::pair<bool, size_t>> maybe_get_args_offset(std::string_view callee_str,
                                                             const std::vector<OverflowableItem>& args,
                                                             const Config& config);

// ===========================================================================
// pairs.rs (rust_format_pairs.cpp)

struct PairParts {
    std::string_view prefix;
    std::string_view infix;
    std::string_view suffix;
    static PairParts infix_(std::string_view i) { return PairParts{"", i, ""}; }
};
RewriteResult rewrite_all_pairs(const rust::Expr* expr, Shape shape, const RewriteContext& ctx);
RewriteResult rewrite_pair(const RwNode& lhs, const RwNode& rhs, PairParts pp, const RewriteContext& ctx,
                           Shape shape, SeparatorPlace separator_place);

// ===========================================================================
// vertical.rs (rust_format_vertical.cpp) -- only reachable with
// struct_field_align_threshold > 0, which is not the default; kept as a stub.

// ===========================================================================
// expr.rs (rust_format_expr.cpp)

enum class ExprType : uint8_t { Statement, SubExpression };
struct RhsAssignKind {
    enum Kind : uint8_t { Expr, Bounds, Ty } kind = Ty;
    const rust::Expr* expr = nullptr;  // Expr: the expression (rustfmt keeps its ExprKind)
    static RhsAssignKind of_expr(const rust::Expr* e) { return RhsAssignKind{Expr, e}; }
    static RhsAssignKind bounds() { return RhsAssignKind{Bounds, nullptr}; }
    static RhsAssignKind ty() { return RhsAssignKind{Ty, nullptr}; }
};
enum class RhsTactics : uint8_t { Default, ForceNextLineWithoutIndent, AllowOverflow };

bool lit_ends_in_dot(const rust::Lit& lit, const RewriteContext& ctx);
bool float_lit_ends_in_dot(std::string_view symbol, std::string_view suffix);
RewriteResult format_expr(const rust::Expr* expr, ExprType expr_type, const RewriteContext& ctx, Shape shape);
RewriteResult rewrite_array(std::string_view name, const std::vector<OverflowableItem>& exprs, Span span,
                            const RewriteContext& ctx, Shape shape,
                            std::optional<SeparatorTactic> force_separator_tactic,
                            std::optional<Delimiter> delim_token);
/// attrs may be null (None).
RewriteResult rewrite_block_with_visitor(const RewriteContext& ctx, std::string_view prefix, const rust::Block* block,
                                         const rust::AttrVec* attrs, const rust::Label* label, Shape shape,
                                         bool has_braces);
RewriteResult rewrite_let_else_block(const rust::Block* block, bool allow_single_line, const RewriteContext& ctx,
                                     Shape shape);
Opt rewrite_cond(const RewriteContext& ctx, const rust::Expr* expr, Shape shape);
std::string rewrite_else_kw_with_comments(bool force_newline_else, bool is_last, const RewriteContext& ctx,
                                          Span span, Shape shape);
bool block_contains_comment(const RewriteContext& ctx, const rust::Block* block);
bool is_simple_block(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs);
bool is_simple_block_stmt(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs);
bool is_empty_block(const RewriteContext& ctx, const rust::Block* block, const rust::AttrVec* attrs);
bool stmt_is_expr(const rust::Stmt* stmt);
bool is_unsafe_block(const rust::Block* block);
RewriteResult rewrite_literal(const RewriteContext& ctx, const rust::Lit& lit, Span span, Shape shape);
RewriteResult rewrite_call(const RewriteContext& ctx, std::string_view callee, const std::vector<rust::Expr*>& args,
                           Span span, Shape shape);
bool is_simple_expr(const rust::Expr* expr);
bool is_every_expr_simple(const std::vector<OverflowableItem>& lists);
bool can_be_overflowed_expr(const RewriteContext& ctx, const rust::Expr* expr, size_t args_len);
bool is_nested_call(const rust::Expr* expr);
bool span_ends_with_comma(const RewriteContext& ctx, Span span);
RewriteResult rewrite_paren(const RewriteContext& ctx, const rust::Expr* subexpr, Shape shape, Span span);
RewriteResult wrap_struct_field(const RewriteContext& ctx, const rust::AttrVec& attrs, std::string_view fields_str,
                                Shape shape, Shape nested_shape, size_t one_line_width);
const char* struct_lit_field_separator(const Config& config);
RewriteResult rewrite_field(const RewriteContext& ctx, const rust::ExprField* field, Shape shape,
                            size_t prefix_max_width);
RewriteResult rewrite_tuple(const RewriteContext& ctx, const std::vector<OverflowableItem>& items, Span span,
                            Shape shape, bool is_singleton_tuple);
RewriteResult rewrite_unary_prefix(const RewriteContext& ctx, std::string_view prefix, const RwNode& rewrite,
                                   Shape shape);
RewriteResult rewrite_unary_suffix(const RewriteContext& ctx, std::string_view suffix, const RwNode& rewrite,
                                   Shape shape);
RewriteResult rewrite_assign_rhs(const RewriteContext& ctx, std::string lhs, const RwNode& ex,
                                 const RhsAssignKind& rhs_kind, Shape shape);
RewriteResult rewrite_assign_rhs_expr(const RewriteContext& ctx, std::string_view lhs, const RwNode& ex, Shape shape,
                                      const RhsAssignKind& rhs_kind, RhsTactics rhs_tactics);
RewriteResult rewrite_assign_rhs_with(const RewriteContext& ctx, std::string lhs, const RwNode& ex, Shape shape,
                                      const RhsAssignKind& rhs_kind, RhsTactics rhs_tactics);
RewriteResult rewrite_assign_rhs_with_comments(const RewriteContext& ctx, std::string lhs, const RwNode& ex,
                                               Shape shape, const RhsAssignKind& rhs_kind, RhsTactics rhs_tactics,
                                               Span between_span, bool allow_extend);
bool prefer_next_line(std::string_view orig_rhs, std::string_view next_line_rhs, RhsTactics rhs_tactics);
bool is_method_call(const rust::Expr* expr);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Expr* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Block* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::ExprField* x, Shape shape);

// ===========================================================================
// stmt.rs (rust_format_stmt.cpp)

/// rustfmt's stmt::Stmt wrapper.
struct StmtW {
    const rust::Stmt* inner = nullptr;
    bool is_last = false;
    const rust::Stmt* as_ast_node() const { return inner; }
    const rust::Item* to_item() const;
    bool is_empty() const;
    bool is_last_expr() const;
    static std::optional<StmtW> from_simple_block(const RewriteContext& ctx, const rust::Block* block,
                                                  const rust::AttrVec* attrs);
    static StmtW from_ast_node(const rust::Stmt* inner, bool is_last) { return StmtW{inner, is_last}; }
    static std::vector<StmtW> from_ast_nodes(const std::vector<rust::Stmt*>& stmts);
};
Span spanned(const StmtW& s);
RewriteResult rewrite_result(const RewriteContext& ctx, const StmtW& s, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Local* x, Shape shape);  // items.rs

// ===========================================================================
// closures.rs (rust_format_closures.cpp)

RewriteResult rewrite_closure(const rust::Closure* closure, Span span, const RewriteContext& ctx, Shape shape);
RewriteResult rewrite_last_closure(const RewriteContext& ctx, const rust::Expr* expr, Shape shape);
bool args_have_many_closure(const std::vector<OverflowableItem>& args);

// ===========================================================================
// matches.rs (rust_format_matches.cpp)

RewriteResult rewrite_match(const RewriteContext& ctx, const rust::Expr* cond, const std::vector<rust::Arm>& arms,
                            Shape shape, Span span, const rust::AttrVec& attrs, rust::MatchKind match_kind);

// ===========================================================================
// chains.rs (rust_format_chains.cpp)

RewriteResult rewrite_chain(const rust::Expr* expr, const RewriteContext& ctx, Shape shape);

// ===========================================================================
// patterns.rs (rust_format_patterns.cpp)

struct TuplePatField {
    const rust::Pat* pat = nullptr;  // Pat
    bool dotdot = false;             // Dotdot(span)
    Span span;
};
Span spanned(const TuplePatField* x);
/// patterns.rs's RangeOperand<'a, T>: an optional range bound.
struct RangeOperand {
    const rust::Expr* operand = nullptr;  // null = none
    Span span;
};
Span spanned(const RangeOperand* x);
bool is_short_pattern(const RewriteContext& ctx, const rust::Pat* pat, std::string_view pat_str);
RewriteResult rewrite_range_pat(const RewriteContext& ctx, Shape shape, const rust::Expr* lhs, const rust::Expr* rhs,
                                rust::RangeEndKind end_kind, Span end_span, Span span);
bool can_be_overflowed_pat(const RewriteContext& ctx, const TuplePatField* pat, size_t len);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Pat* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::PatField* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const TuplePatField* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const RangeOperand* x, Shape shape);

// ===========================================================================
// types.rs (rust_format_types.cpp)

enum class PathContext : uint8_t { Expr, Type, Import };
/// types.rs's SegmentParam.
struct SegmentParam {
    enum Kind : uint8_t { Const, LifeTime, Type, Binding } kind = Type;
    const rust::AnonConst* konst = nullptr;
    const rust::Lifetime* lifetime = nullptr;
    const rust::Ty* ty = nullptr;
    const rust::AssocItemConstraint* binding = nullptr;
    static SegmentParam from_generic_arg(const rust::AngleBracketedArg& arg);
};
Span spanned(const SegmentParam* x);
RewriteResult rewrite_path(const RewriteContext& ctx, PathContext path_context, const rust::QSelf* qself,
                           const rust::Path* path, Shape shape);
const rust::GenericBounds* opaque_ty(const rust::Ty* ty);  // ty may be null
bool can_be_overflowed_type(const RewriteContext& ctx, const rust::Ty* ty, size_t len);
Opt rewrite_bound_params(const RewriteContext& ctx, Shape shape, const std::vector<rust::GenericParam*>& params);
RewriteResult rewrite_result(const RewriteContext& ctx, const SegmentParam* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::PreciseCapturingArg* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::AssocItemConstraint* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::WherePredicate* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::GenericArg* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::AnonConst* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Lifetime* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::GenericBound* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::GenericBounds* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::GenericParam* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::PolyTraitRef* x, Shape shape);
/// impl Rewrite for ast::TraitRef (a path)
RewriteResult rewrite_trait_ref(const RewriteContext& ctx, const rust::Path* trait_ref, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Ty* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::TyPat* x, Shape shape);

// ===========================================================================
// items.rs (rust_format_items.cpp)

enum class ItemVisitorKind : uint8_t { Item, AssocTraitItem, AssocImplItem, ForeignItem };
enum class FnBraceStyle : uint8_t { SameLine, NextLine, None };

class FmtVisitor;

RewriteResult format_impl(const RewriteContext& ctx, const rust::Item* item, Indent offset);
RewriteResult format_trait(const RewriteContext& ctx, const rust::Item* item, Indent offset);
RewriteResult format_trait_alias(const RewriteContext& ctx, const rust::Item* item, Shape shape);
RewriteResult rewrite_type_alias(const rust::Item* item, const RewriteContext& ctx, Indent indent,
                                 ItemVisitorKind visitor_kind, Span span);
RewriteResult rewrite_struct_field_prefix(const RewriteContext& ctx, const rust::FieldDef* field);
RewriteResult rewrite_struct_field(const RewriteContext& ctx, const rust::FieldDef* field, Shape shape,
                                   size_t lhs_max_width);
uint32_t span_lo_for_param(const rust::Param* param);
uint32_t span_hi_for_param(const RewriteContext& ctx, const rust::Param* param);
bool is_named_param(const rust::Param* param);
RewriteResult rewrite_mod(const RewriteContext& ctx, const rust::Item* item, Shape attrs_shape);
RewriteResult rewrite_extern_crate(const RewriteContext& ctx, const rust::Item* item, Shape attrs_shape);
bool is_mod_decl(const rust::Item* item);
bool is_use_item(const rust::Item* item);
bool is_extern_crate(const rust::Item* item);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::FieldDef* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::FnRetTy* x, Shape shape);
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Param* x, Shape shape);
// FmtVisitor methods that live in items.rs, as free functions:
void visit_static(FmtVisitor& v, const rust::Item* item);           // Static / Const items
void visit_struct(FmtVisitor& v, const rust::Item* item);           // Struct / Union items
void visit_enum(FmtVisitor& v, const rust::Item* item);
void format_foreign_mod(FmtVisitor& v, const rust::Item* item);
/// rewrite_fn_before_block for a fn item (any context); nullopt = failure.
std::optional<std::pair<std::string, FnBraceStyle>> rewrite_fn_before_block(FmtVisitor& v, Indent indent,
                                                                            const rust::Item* fn_item, Span span);
/// A closure-style fn signature without a body (trait methods `fn f();`).
RewriteResult rewrite_required_fn(FmtVisitor& v, Indent indent, const rust::Item* fn_item, Span span);
Opt single_line_fn(const FmtVisitor& v, std::string_view fn_str, const rust::Block* block,
                   const rust::AttrVec* inner_attrs);

// ===========================================================================
// imports.rs, reorder.rs, sort.rs (rust_format_imports.cpp, rust_format_reorder.cpp)

void format_import(FmtVisitor& v, const rust::Item* item);
/// reorder.rs: FmtVisitor::visit_items_with_reordering.
void visit_items_with_reordering(FmtVisitor& v, const std::vector<const rust::Item*>& items);
/// sort.rs
int version_sort(std::string_view a, std::string_view b);  // <0, 0, >0

// ===========================================================================
// macros.rs (rust_format_macros.cpp)

enum class MacroPosition : uint8_t { Item, Statement, Expression, Pat };
struct MacroArg {
    enum Kind : uint8_t { Expr, Ty, Pat, Item, Keyword } kind = Expr;
    rust::Expr* expr = nullptr;
    rust::Ty* ty = nullptr;
    rust::Pat* pat = nullptr;
    rust::Item* item = nullptr;
    rust::Ident keyword;  // Keyword(ident, span)
    Span span;            // Keyword
    bool is_item() const { return kind == Item; }
};
Span spanned(const MacroArg* x);
RewriteResult rewrite_result(const RewriteContext& ctx, const MacroArg* x, Shape shape);
RewriteResult rewrite_macro(const rust::MacCall* mac, const RewriteContext& ctx, Shape shape, MacroPosition position,
                            Span span);
RewriteResult rewrite_macro_def(const RewriteContext& ctx, Shape shape, Indent indent, const rust::Item* def,
                                Span span);
/// convert_try_mac: `try!(e)` as an `e?` expression (allocated in the arena), or null.
const rust::Expr* convert_try_mac(const rust::MacCall* mac, const RewriteContext& ctx);
Delimiter macro_style(const rust::MacCall* mac, const RewriteContext& ctx);
/// impl Rewrite for ast::Item (macros.rs: items inside macro arguments).
RewriteResult rewrite_result(const RewriteContext& ctx, const rust::Item* x, Shape shape);

// ===========================================================================
// visitor.rs, missed_spans.rs (rust_format_visitor.cpp, rust_format_missed_spans.cpp)

class FmtVisitor {
public:
    const RewriteContext* parent_context = nullptr;
    rust::ParseResult* pr = nullptr;
    std::string buffer;
    uint32_t last_pos = 0;
    Indent block_indent;
    const Config* config = nullptr;
    bool is_if_else_block = false;
    bool is_loop_block = false;
    const SnippetProvider* snippet_provider = nullptr;
    size_t line_number = 0;
    std::shared_ptr<std::vector<std::pair<size_t, size_t>>> skipped_range =
        std::make_shared<std::vector<std::pair<size_t, size_t>>>();
    bool macro_rewrite_failure = false;
    FormatState* state = nullptr;
    SkipContext skip_context;
    bool is_macro_def = false;

    static FmtVisitor from_context(const RewriteContext& ctx);
    static FmtVisitor from_parse(rust::ParseResult* pr, const Config& config, const SnippetProvider& sp,
                                 FormatState& state);
    ~FmtVisitor();  // propagates macro_rewrite_failure to parent_context
    FmtVisitor(FmtVisitor&&) = default;

    Shape shape() const { return Shape::indented(block_indent, *config); }
    std::optional<std::string_view> opt_snippet(Span span) const { return snippet_provider->span_to_snippet(span); }
    std::string_view snippet(Span span) const;
    RewriteContext get_context() const;

    // visitor.rs
    void visit_block(const rust::Block* b, const rust::AttrVec* inner_attrs, bool has_braces);
    /// visit_fn for a fn item with a body.
    void visit_fn(const rust::Item* fn_item, Span s, const rust::AttrVec* inner_attrs);
    void visit_item(const rust::Item* item);
    void visit_trait_item(const rust::Item* ti);
    void visit_impl_item(const rust::Item* ii);
    void visit_foreign_item(const rust::Item* fi);  // items.rs format_foreign_item
    void push_str(std::string_view s);
    void push_rewrite(Span span, Opt rewrite);
    void push_skipped_with_span(const rust::AttrVec& attrs, Span item_span, Span main_span);
    /// true when the attributes say skip (the caller then pushes the item verbatim).
    bool visit_attrs(const rust::AttrVec& attrs, rust::AttrStyle style);
    void skip_empty_lines(uint32_t end_pos);
    void walk_mod_items(const std::vector<rust::Item*>& items);
    void format_separate_mod(const rust::Crate* crate, uint32_t end_pos);  // the whole file

    // missed_spans.rs
    void format_missing(uint32_t end);
    void format_missing_with_indent(uint32_t end);
    void format_missing_no_indent(uint32_t end);

private:
    FmtVisitor() = default;
};

// ===========================================================================
// lib.rs / formatting.rs (rust_format.cpp)

/// format_snippet: format `snippet` as a whole file (a macro def body, a
/// code block); nullopt when it cannot be parsed or formatted cleanly.
struct FormattedSnippet {
    std::string snippet;
    std::vector<size_t> non_formatted_ranges;
};
std::optional<FormattedSnippet> format_snippet(std::string_view snippet, const Config& config, bool is_macro_def,
                                               FormatState& state);
/// format_code_block: wraps `code_snippet` in `fn main() {` .. `}` and formats it.
std::optional<FormattedSnippet> format_code_block(std::string_view code_snippet, const Config& config,
                                                  bool is_macro_def, FormatState& state);

}  // namespace rustfmt
