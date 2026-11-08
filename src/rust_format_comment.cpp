// rust_format_comment.cpp -- rustfmt's comment.rs and string.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// Rust iterates `char`s; this port iterates UTF-8 code points (byte offsets
// are kept, exactly like `char_indices`). `char::is_whitespace` is Unicode
// White_Space (is_uws below), not the lexer's Pattern_White_Space.
#include "rust_format_internal.h"

#include <algorithm>
#include <tuple>

namespace rustfmt {

namespace {

// ---------------------------------------------------------------------------
// Small char helpers.

/// char::is_whitespace (Unicode White_Space).
bool is_uws(uint32_t c) {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0d);
    switch (c) {
        case 0x85: case 0xa0: case 0x1680: case 0x2028: case 0x2029: case 0x202f: case 0x205f: case 0x3000:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200a;
    }
}

/// char::is_alphanumeric (Alphabetic || Numeric). ASCII is exact; beyond
/// ASCII this approximates with XID_Continue minus the obvious non-alphanumerics
/// (connector punctuation, middle dot, combining marks of the basic block).
bool is_ualnum(uint32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c == 0xb7 || c == 0x203f || c == 0x2040 || c == 0x2054 || c == 0xfe33 || c == 0xfe34 ||
        (c >= 0xfe4d && c <= 0xfe4f) || c == 0xff3f || c == 0x387)
        return false;
    if (c >= 0x300 && c <= 0x36f) return false;
    return rust::is_id_continue(c);
}

uint32_t decode_at(std::string_view s, size_t i, size_t& len) { return rust::decode_utf8(s, i, len); }

/// s.chars().nth(n)
std::optional<uint32_t> nth_char(std::string_view s, size_t n) {
    size_t i = 0;
    for (size_t k = 0; i < s.size(); k++) {
        size_t len = 1;
        uint32_t c = decode_at(s, i, len);
        if (k == n) return c;
        i += len;
    }
    return std::nullopt;
}

/// str::find(|c| !c.is_whitespace())
std::optional<size_t> find_non_ws(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 1;
        uint32_t c = decode_at(s, i, len);
        if (!is_uws(c)) return i;
        i += len;
    }
    return std::nullopt;
}

/// s.chars().take_while(|c| c.is_whitespace()).count()
size_t leading_ws_chars(std::string_view s) {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        size_t len = 1;
        uint32_t c = decode_at(s, i, len);
        if (!is_uws(c)) break;
        n++;
        i += len;
    }
    return n;
}

/// String::pop(): removes the last char.
void pop_char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) i--;
    s.resize(i);
}

/// &s[from..] with Rust's char-boundary panic.
std::string_view slice_from(std::string_view s, size_t from) {
    if (from > s.size() || (from < s.size() && (static_cast<unsigned char>(s[from]) & 0xc0) == 0x80))
        panic("comment.rs: byte index is not a char boundary");
    return s.substr(from);
}

/// str::matches(pat).count() (non-overlapping).
size_t count_matches(std::string_view s, std::string_view pat) {
    size_t n = 0, i = 0;
    while (true) {
        size_t p = s.find(pat, i);
        if (p == std::string_view::npos) break;
        n++;
        i = p + (pat.empty() ? 1 : pat.size());
        if (i > s.size()) break;
    }
    return n;
}

}  // namespace

// ===========================================================================
// CommentStyle

static bool is_custom_comment(std::string_view comment) {
    if (!starts_with(comment, "//")) return false;
    if (auto c = nth_char(comment, 2)) return !is_ualnum(*c) && !is_uws(*c);
    return false;
}

static std::string_view custom_opener(std::string_view s) {
    auto ls = lines(s);
    if (ls.empty()) return "";
    std::string_view first_line = ls[0];
    size_t space_index = first_line.find(' ');
    if (space_index == std::string_view::npos) return first_line;
    return first_line.substr(0, space_index + 1);
}

bool CommentStyle::is_line_comment() const {
    return kind == CommentStyleKind::DoubleSlash || kind == CommentStyleKind::TripleSlash ||
           kind == CommentStyleKind::Doc || kind == CommentStyleKind::Custom;
}

bool CommentStyle::is_block_comment() const {
    return kind == CommentStyleKind::SingleBullet || kind == CommentStyleKind::DoubleBullet ||
           kind == CommentStyleKind::Exclamation;
}

bool CommentStyle::is_doc_comment() const {
    return kind == CommentStyleKind::TripleSlash || kind == CommentStyleKind::Doc;
}

std::string_view CommentStyle::opener() const {
    switch (kind) {
        case CommentStyleKind::DoubleSlash: return "// ";
        case CommentStyleKind::TripleSlash: return "/// ";
        case CommentStyleKind::Doc: return "//! ";
        case CommentStyleKind::SingleBullet: return "/* ";
        case CommentStyleKind::DoubleBullet: return "/** ";
        case CommentStyleKind::Exclamation: return "/*! ";
        case CommentStyleKind::Custom: return custom;
    }
    return "";
}

std::string_view CommentStyle::closer() const {
    switch (kind) {
        case CommentStyleKind::DoubleSlash:
        case CommentStyleKind::TripleSlash:
        case CommentStyleKind::Custom:
        case CommentStyleKind::Doc: return "";
        case CommentStyleKind::SingleBullet:
        case CommentStyleKind::DoubleBullet:
        case CommentStyleKind::Exclamation: return " */";
    }
    return "";
}

std::string_view CommentStyle::line_start() const {
    switch (kind) {
        case CommentStyleKind::DoubleSlash: return "// ";
        case CommentStyleKind::TripleSlash: return "/// ";
        case CommentStyleKind::Doc: return "//! ";
        case CommentStyleKind::SingleBullet:
        case CommentStyleKind::DoubleBullet:
        case CommentStyleKind::Exclamation: return " * ";
        case CommentStyleKind::Custom: return custom;
    }
    return "";
}

static bool operator==(const CommentStyle& a, const CommentStyle& b) {
    return a.kind == b.kind && (a.kind != CommentStyleKind::Custom || a.custom == b.custom);
}

static CommentStyle mk_style(CommentStyleKind k, std::string_view custom = {}) {
    CommentStyle s;
    s.kind = k;
    s.custom = custom;
    return s;
}

CommentStyle comment_style(std::string_view orig, bool normalize_comments) {
    auto triple = [&] {
        if (!starts_with(orig, "///")) return false;
        auto c = nth_char(orig, 3);
        return !c || *c != '/';
    };
    if (!normalize_comments) {
        if (starts_with(orig, "/**") && !starts_with(orig, "/**/")) return mk_style(CommentStyleKind::DoubleBullet);
        if (starts_with(orig, "/*!")) return mk_style(CommentStyleKind::Exclamation);
        if (starts_with(orig, "/*")) return mk_style(CommentStyleKind::SingleBullet);
        if (triple()) return mk_style(CommentStyleKind::TripleSlash);
        if (starts_with(orig, "//!")) return mk_style(CommentStyleKind::Doc);
        if (is_custom_comment(orig)) return mk_style(CommentStyleKind::Custom, custom_opener(orig));
        return mk_style(CommentStyleKind::DoubleSlash);
    }
    if (triple() || (starts_with(orig, "/**") && !starts_with(orig, "/**/")))
        return mk_style(CommentStyleKind::TripleSlash);
    if (starts_with(orig, "//!") || starts_with(orig, "/*!")) return mk_style(CommentStyleKind::Doc);
    if (is_custom_comment(orig)) return mk_style(CommentStyleKind::Custom, custom_opener(orig));
    return mk_style(CommentStyleKind::DoubleSlash);
}

bool is_last_comment_block(std::string_view s) { return ends_with(trim_end(s), "*/"); }

// ===========================================================================
// combine_strs_with_missing_comments

RewriteResult combine_strs_with_missing_comments(const RewriteContext& ctx, std::string_view prev_str,
                                                 std::string_view next_str, Span span, Shape shape,
                                                 bool allow_extend) {
    std::string result;
    result.reserve(prev_str.size() + next_str.size() + shape.indent.width() + 128);
    result.append(prev_str);
    bool allow_one_line = !contains(prev_str, '\n') && !contains(next_str, '\n');
    std::string_view first_sep0 =
        (prev_str.empty() || next_str.empty() || trimmed_last_line_width(prev_str) == 0) ? "" : " ";
    size_t one_line_width = last_line_width(prev_str) + first_line_width(next_str) + first_sep0.size();

    const Config& config = *ctx.config;
    Indent indent = shape.indent;
    RewriteResult mc = rewrite_missing_comment(span, shape, ctx);
    if (!mc) return mc;
    const std::string& missing_comment = *mc;

    if (missing_comment.empty()) {
        if (allow_extend && one_line_width <= shape.width) {
            result.append(first_sep0);
        } else if (!prev_str.empty()) {
            result += indent.to_string_with_newline(config);
        }
        result.append(next_str);
        return result;
    }

    // We have a missing comment between the first expression and the second expression.

    // Peek the original source code and find out whether there is a newline between the first
    // expression and the second expression or the missing comment. We will preserve the original
    // layout whenever possible.
    std::string_view original_snippet = ctx.snippet(span);
    bool prefer_same_line;
    size_t pos = original_snippet.find('/');
    if (pos != std::string_view::npos) {
        prefer_same_line = !contains(original_snippet.substr(0, pos), '\n');
    } else {
        prefer_same_line = !contains(original_snippet, '\n');
    }

    one_line_width -= first_sep0.size();
    std::string first_sep;
    if (prev_str.empty() || missing_comment.empty()) {
        first_sep = "";
    } else {
        size_t olw = last_line_width(prev_str) + first_line_width(missing_comment) + 1;
        if (prefer_same_line && olw <= shape.width) {
            first_sep = " ";
        } else {
            first_sep = indent.to_string_with_newline(config);
        }
    }
    result += first_sep;
    result += missing_comment;

    std::string second_sep;
    if (missing_comment.empty() || next_str.empty()) {
        second_sep = "";
    } else if (starts_with(missing_comment, "//")) {
        second_sep = indent.to_string_with_newline(config);
    } else {
        one_line_width += missing_comment.size() + first_sep.size() + 1;
        allow_one_line = allow_one_line && !starts_with(missing_comment, "//") && !contains(missing_comment, '\n');
        if (prefer_same_line && allow_one_line && one_line_width <= shape.width) {
            second_sep = " ";
        } else {
            second_sep = indent.to_string_with_newline(config);
        }
    }
    result += second_sep;
    result.append(next_str);
    return result;
}

// ===========================================================================
// identify_comment and friends

static RewriteResult identify_comment(std::string_view orig, bool block_style, Shape shape, const Config& config,
                                      bool is_doc_comment);

RewriteResult rewrite_doc_comment(std::string_view orig, Shape shape, const Config& config) {
    return identify_comment(orig, false, shape, config, true);
}

RewriteResult rewrite_comment(std::string_view orig, bool block_style, Shape shape, const Config& config) {
    return identify_comment(orig, block_style, shape, config, false);
}

// identify_comment::compute_len
static size_t compute_len(std::string_view orig, std::string_view line) {
    if (orig.size() > line.size()) {
        if (orig[line.size()] == '\r') return line.size() + 2;
        return line.size() + 1;
    }
    return line.size();
}

// identify_comment::consume_same_line_comments
static std::pair<bool, size_t> consume_same_line_comments(CommentStyle style, std::string_view orig,
                                                          std::string_view line_start) {
    size_t first_group_ending = 0;
    bool hbl = false;
    for (std::string_view line : lines(orig)) {
        std::string_view trimmed_line = trim_start(line);
        if (trimmed_line.empty()) {
            hbl = true;
            break;
        } else if (starts_with(trimmed_line, line_start) || comment_style(trimmed_line, false) == style) {
            first_group_ending += compute_len(slice_from(orig, first_group_ending), line);
        } else {
            break;
        }
    }
    return {hbl, first_group_ending};
}

static std::string light_rewrite_comment(std::string_view orig, Indent offset, const Config& config,
                                         bool is_doc_comment);
static RewriteResult rewrite_comment_inner(std::string_view orig, bool block_style, CommentStyle style, Shape shape,
                                           const Config& config, bool is_doc_comment);

static RewriteResult identify_comment(std::string_view orig, bool block_style, Shape shape, const Config& config,
                                      bool is_doc_comment) {
    CommentStyle style = comment_style(orig, false);

    bool has_bare_lines = false;
    size_t first_group_ending = 0;
    switch (style.kind) {
        case CommentStyleKind::DoubleSlash:
        case CommentStyleKind::TripleSlash:
        case CommentStyleKind::Doc: {
            std::string_view line_start = trim_start(style.line_start());
            std::tie(has_bare_lines, first_group_ending) = consume_same_line_comments(style, orig, line_start);
            break;
        }
        case CommentStyleKind::Custom: {
            std::string_view trimmed_opener = trim_end(style.custom);
            std::tie(has_bare_lines, first_group_ending) = consume_same_line_comments(style, orig, trimmed_opener);
            break;
        }
        // for a block comment, search for the closing symbol
        case CommentStyleKind::DoubleBullet:
        case CommentStyleKind::SingleBullet:
        case CommentStyleKind::Exclamation: {
            std::string_view closer = trim_start(style.closer());
            size_t count = count_matches(orig, closer);
            size_t closing_symbol_offset = 0;
            bool hbl = false;
            bool first = true;
            for (std::string_view line : lines(orig)) {
                closing_symbol_offset += compute_len(slice_from(orig, closing_symbol_offset), line);
                std::string_view trimmed_line = trim_start(line);
                if (!starts_with(trimmed_line, "*") && !starts_with(trimmed_line, "//") &&
                    !starts_with(trimmed_line, "/*")) {
                    hbl = true;
                }
                // Remove opener from consideration when searching for closer
                if (first) {
                    std::string_view opener = trim_end(style.opener());
                    trimmed_line = slice_from(trimmed_line, opener.size());
                    first = false;
                }
                if (ends_with(trimmed_line, closer)) {
                    count -= 1;  // wrapping, like rustfmt's release build
                    if (count == 0) break;
                }
            }
            has_bare_lines = hbl;
            first_group_ending = closing_symbol_offset;
            break;
        }
    }

    // orig.split_at(first_group_ending)
    std::string_view rest = slice_from(orig, first_group_ending);
    std::string_view first_group = orig.substr(0, first_group_ending);
    std::string rewritten_first_group;
    if (!config.normalize_comments() && has_bare_lines && style.is_block_comment()) {
        Opt t = trim_left_preserve_layout(first_group, shape.indent, config);
        if (!t) return Err(RewriteError::unknown());
        rewritten_first_group = std::move(*t);
    } else if (!config.normalize_comments() && !config.wrap_comments() &&
               !(
                   // `format_code_in_doc_comments` should only take effect on doc comments,
                   // so we only consider it when this comment block is a doc comment block.
                   is_doc_comment && config.format_code_in_doc_comments())) {
        rewritten_first_group = light_rewrite_comment(first_group, shape.indent, config, is_doc_comment);
    } else {
        // Only reachable with non-default config (normalize_comments / wrap_comments /
        // format_code_in_doc_comments).
        RewriteResult r = rewrite_comment_inner(first_group, block_style, style, shape, config,
                                                is_doc_comment || style.is_doc_comment());
        if (!r) return r;
        rewritten_first_group = std::move(*r);
    }
    if (rest.empty()) return rewritten_first_group;
    RewriteResult rest_r = identify_comment(trim_start(rest), block_style, shape, config, is_doc_comment);
    if (!rest_r) return rest_r;
    std::string out = std::move(rewritten_first_group);
    out += '\n';
    // insert back the blank line
    if (has_bare_lines && style.is_line_comment()) out += '\n';
    out += shape.indent.to_string(config);
    out += *rest_r;
    return out;
}

// ---------------------------------------------------------------------------
// CodeBlockAttribute, ItemizedBlock, CommentRewrite (only fully exercised with
// wrap_comments / normalize_comments / format_code_in_doc_comments on).

enum class CodeBlockAttribute : uint8_t { Rust, NotRust };

// CodeBlockAttribute::new
static CodeBlockAttribute code_block_attribute_new(std::string_view attributes) {
    size_t i = 0;
    while (true) {
        size_t comma = attributes.find(',', i);
        std::string_view attribute =
            trim(attributes.substr(i, comma == std::string_view::npos ? std::string_view::npos : comma - i));
        if (attribute == "" || attribute == "rust" || attribute == "should_panic" || attribute == "no_run" ||
            attribute == "edition2015" || attribute == "edition2018" || attribute == "edition2021") {
            // ok
        } else {
            return CodeBlockAttribute::NotRust;
        }
        if (comma == std::string_view::npos) break;
        i = comma + 1;
    }
    return CodeBlockAttribute::Rust;
}

static std::string itemized_block_quote_start(std::string_view line, std::string line_start, size_t remove_indent);

struct ItemizedBlock {
    std::vector<std::string> lines_;
    size_t indent = 0;
    std::string opener;
    std::string line_start;

    static std::optional<size_t> get_marker_length(std::string_view trimmed) {
        for (std::string_view s : {"* ", "- ", "> ", "+ "}) {
            if (starts_with(trimmed, s)) return 2;  // All items in `itemized_start` have length 2.
        }
        for (std::string_view suffix : {". ", ") "}) {
            size_t p = trimmed.find(suffix);
            if (p != std::string_view::npos) {
                std::string_view prefix = trimmed.substr(0, p);
                bool has_leading_digits = prefix.size() >= 1 && prefix.size() <= 2 &&
                                          std::all_of(prefix.begin(), prefix.end(),
                                                      [](char c) { return c >= '0' && c <= '9'; });
                if (has_leading_digits) return prefix.size() + suffix.size();
            }
        }
        return std::nullopt;  // No markers found.
    }

    // ItemizedBlock::new
    static std::optional<ItemizedBlock> new_(std::string_view line) {
        auto marker_length = get_marker_length(trim_start(line));
        if (!marker_length) return std::nullopt;
        size_t space_to_marker = leading_ws_chars(line);
        size_t ind = space_to_marker + *marker_length;
        std::string ls = repeat(" ", ind);
        // Markdown blockquote start with a "> "
        if (starts_with(trim_start(line), ">")) {
            ls = itemized_block_quote_start(line, ls, 2);
            ind = ls.size();
        }
        ItemizedBlock ib;
        ib.lines_.push_back(std::string(slice_from(line, ind)));
        ib.indent = ind;
        ib.opener = std::string(line.substr(0, ind));
        ib.line_start = std::move(ls);
        return ib;
    }

    StringFormat create_string_format(const StringFormat& fmt) const {
        StringFormat f(Shape::legacy(sat_sub(fmt.shape.width, indent), Indent::empty()), *fmt.config);
        f.opener = "";
        f.closer = "";
        f.line_start = "";
        f.line_end = "";
        f.trim_end = true;
        return f;
    }

    bool add_line(std::string_view line) {
        if (!get_marker_length(trim_start(line)) && indent <= leading_ws_chars(line)) {
            lines_.push_back(std::string(line));
            return true;
        }
        return false;
    }

    std::string trimmed_block_as_string() const {
        std::string acc;
        for (const auto& line : lines_) {
            acc += trim_start(line);
            acc += ' ';
        }
        return acc;
    }

    std::string original_block_as_string() const { return join(lines_, "\n"); }
};

static std::string itemized_block_quote_start(std::string_view line, std::string line_start, size_t remove_indent) {
    size_t quote_level = 0;
    size_t i = 0;
    while (i < line.size()) {
        size_t len = 1;
        uint32_t c = decode_at(line, i, len);
        if (is_ualnum(c)) break;
        if (c == '>') quote_level++;
        i += len;
    }
    for (size_t k = 0; k < remove_indent; k++) pop_char(line_start);
    for (size_t k = 0; k < quote_level; k++) line_start += "> ";
    return line_start;
}

static constexpr std::string_view RUSTFMT_CUSTOM_COMMENT_PREFIX = "//#### ";

static std::string hide_sharp_behind_comment(std::string_view s) {
    std::string_view s_trimmed = trim(s);
    if (starts_with(s_trimmed, "# ") || s_trimmed == "#") {
        std::string r(RUSTFMT_CUSTOM_COMMENT_PREFIX);
        r += s;
        return r;
    }
    return std::string(s);
}

static std::string trim_custom_comment_prefix(std::string_view s) {
    std::vector<std::string> out;
    for (std::string_view line : lines(s)) {
        std::string_view left_trimmed = trim_start(line);
        if (starts_with(left_trimmed, RUSTFMT_CUSTOM_COMMENT_PREFIX)) {
            while (starts_with(left_trimmed, RUSTFMT_CUSTOM_COMMENT_PREFIX))
                left_trimmed.remove_prefix(RUSTFMT_CUSTOM_COMMENT_PREFIX.size());
            out.emplace_back(left_trimmed);
        } else {
            out.emplace_back(line);
        }
    }
    return join(out, "\n");
}

/// Returns `true` if the given string MAY include URLs or alike.
static bool has_url(std::string_view s) {
    // reference_link_url = ^\[.+\]\s?:
    auto reference_link = [&] {
        if (!starts_with(s, "[")) return false;
        // `.+` is greedy and excludes '\n'; any `]` followed by \s?: after at least one char works.
        size_t line_end = s.find('\n');
        std::string_view l = s.substr(0, line_end);
        for (size_t j = 2; j < l.size(); j++) {
            if (l[j] != ']') continue;
            std::string_view after = l.substr(j + 1);
            if (starts_with(after, ":")) return true;
            if (!after.empty()) {
                size_t len = 1;
                uint32_t c = decode_at(after, 0, len);
                // \s may also be the '\n' that ended `l`
                if (is_uws(c) && starts_with(after.substr(len), ":")) return true;
            } else if (line_end != std::string_view::npos && starts_with(s.substr(line_end + 1), ":")) {
                return true;
            }
        }
        return false;
    };
    return contains(s, "https://") || contains(s, "http://") || contains(s, "ftp://") || contains(s, "file://") ||
           reference_link();
}

/// Returns true if the given string may be part of a Markdown table.
static bool is_table_item(std::string_view s) {
    s = trim_start(s);
    if (!starts_with(s, "|")) return false;
    size_t p = s.rfind('|');
    return !(p == 0 || p == std::string_view::npos);
}

struct CommentRewrite {
    std::string result;
    std::string code_block_buffer;
    bool is_prev_line_multi_line = false;
    std::optional<CodeBlockAttribute> code_block_attr;
    std::optional<ItemizedBlock> item_block;
    std::string comment_line_separator;
    std::string indent_str;
    size_t max_width = 0;
    Indent fmt_indent;
    StringFormat fmt;

    std::string opener;
    std::string closer;
    std::string line_start;
    CommentStyle style;

    CommentRewrite(std::string_view orig, bool block_style, Shape shape, const Config& config)
        : fmt(shape, config) {
        CommentStyle st = block_style ? mk_style(CommentStyleKind::SingleBullet)
                                      : comment_style(orig, config.normalize_comments());
        std::string_view op = st.opener(), cl = st.closer(), ls = st.line_start();
        auto mw = checked_sub(shape.width, cl.size() + op.size());
        max_width = mw ? *mw : 1;
        indent_str = shape.indent.to_string_with_newline(config);
        result.reserve(orig.size() * 2);
        comment_line_separator = indent_str + std::string(ls);
        fmt_indent = shape.indent;
        fmt.opener = "";
        fmt.closer = "";
        fmt.line_start = ls;
        fmt.line_end = "";
        fmt.shape = Shape::legacy(max_width, shape.indent);
        fmt.trim_end = true;
        opener = std::string(op);
        closer = std::string(cl);
        line_start = std::string(ls);
        style = st;
        result += op;
    }

    static std::string join_block(std::string_view s, std::string_view sep) {
        std::string result;
        auto ls = lines(s);
        for (size_t k = 0; k < ls.size(); k++) {
            result += ls[k];
            if (k + 1 < ls.size()) {
                if (ls[k + 1].empty())
                    result += trim_end(sep);
                else
                    result += sep;
            }
        }
        return result;
    }

    bool buffer_contains_comment() const { return result.size() != opener.size(); }

    void push_item_block(const ItemizedBlock& ib) {
        fmt.shape = Shape::legacy(max_width, fmt_indent);
        StringFormat item_fmt = ib.create_string_format(fmt);
        result += ib.opener;
        Opt s = rewrite_string(ib.trimmed_block_as_string(), item_fmt, sat_sub(max_width, ib.indent));
        if (s) {
            result += join_block(*s, comment_line_separator + ib.line_start);
        } else {
            result += join_block(ib.original_block_as_string(), comment_line_separator);
        }
    }

    std::string finish() {
        if (!code_block_buffer.empty()) {
            // There is a code block that is not properly enclosed by backticks.
            // We will leave them untouched.
            result += comment_line_separator;
            result += join_block(trim_custom_comment_prefix(code_block_buffer), comment_line_separator);
        }
        if (item_block) {
            // the last few lines are part of an itemized block
            fmt.shape = Shape::legacy(max_width, fmt_indent);
            // only push a comment_line_separator for ItemizedBlocks if the comment is not empty
            if (buffer_contains_comment()) result += comment_line_separator;
            push_item_block(*item_block);
        }
        result += closer;
        if (ends_with(result, opener) && ends_with(opener, " ")) {
            // Trailing space.
            pop_char(result);
        }
        return result;
    }

    bool handle_line(std::string_view orig, size_t i, std::string_view line, bool has_leading_whitespace,
                     bool is_doc_comment) {
        size_t num_newlines = count_newlines(orig);
        bool is_last = i == num_newlines;
        bool needs_new_comment_line =
            style.is_block_comment() ? (num_newlines > 0 || buffer_contains_comment()) : buffer_contains_comment();

        if (item_block) {
            if (item_block->add_line(line)) return false;
            is_prev_line_multi_line = false;
            // only push a comment_line_separator if we need to start a new comment line
            if (needs_new_comment_line) result += comment_line_separator;
            push_item_block(*item_block);
        } else if (code_block_attr) {
            if (starts_with(line, "```")) {
                // non-default config: format_code_in_doc_comments (the Rust code block would be
                // formatted with format_code_block here).
                std::string code_block = trim_custom_comment_prefix(code_block_buffer);
                if (!code_block.empty()) {
                    result += comment_line_separator;
                    result += join_block(code_block, comment_line_separator);
                }
                code_block_buffer.clear();
                result += comment_line_separator;
                result += line;
                code_block_attr.reset();
            } else {
                code_block_buffer += hide_sharp_behind_comment(line);
                code_block_buffer += '\n';
            }
            return false;
        }

        code_block_attr.reset();
        item_block.reset();
        if (starts_with(line, "```")) {
            code_block_attr = code_block_attribute_new(line.substr(3));
        } else if (fmt.config->wrap_comments()) {
            if (auto ib = ItemizedBlock::new_(line)) {
                item_block = std::move(*ib);
                return false;
            }
        }

        if (result == opener) {
            bool force_leading_whitespace = opener == "/* " && count_newlines(orig) == 0;
            if (!has_leading_whitespace && !force_leading_whitespace && ends_with(result, " ")) pop_char(result);
            if (line.empty()) return false;
        } else if (is_prev_line_multi_line && !line.empty()) {
            result += ' ';
        } else if (is_last && line.empty()) {
            // trailing blank lines are unwanted
            if (!closer.empty()) result += indent_str;
            return true;
        } else {
            result += comment_line_separator;
            if (!has_leading_whitespace && ends_with(result, " ")) pop_char(result);
        }

        bool is_markdown_header_doc_comment = is_doc_comment && starts_with(line, "#");

        bool should_wrap_comment = fmt.config->wrap_comments() && !is_markdown_header_doc_comment &&
                                   unicode_str_width(line) > fmt.shape.width && !has_url(line) &&
                                   !is_table_item(line);

        if (should_wrap_comment) {
            Opt s = rewrite_string(line, fmt, max_width);
            if (s) {
                is_prev_line_multi_line = contains(*s, '\n');
                result += *s;
            } else if (is_prev_line_multi_line) {
                // We failed to put the current `line` next to the previous `line`.
                // Remove the trailing space, then start rewrite on the next line.
                pop_char(result);
                result += comment_line_separator;
                fmt.shape = Shape::legacy(max_width, fmt_indent);
                Opt s2 = rewrite_string(line, fmt, max_width);
                if (s2) {
                    is_prev_line_multi_line = contains(*s2, '\n');
                    result += *s2;
                } else {
                    is_prev_line_multi_line = false;
                    result += line;
                }
            } else {
                is_prev_line_multi_line = false;
                result += line;
            }

            if (is_prev_line_multi_line) {
                // 1 = " "
                size_t offset = 1 + last_line_width(result) - line_start.size();
                fmt.shape = Shape{sat_sub(max_width, offset), fmt_indent, fmt.shape.offset + offset};
            } else {
                fmt.shape = Shape::legacy(max_width, fmt_indent);
            }
        } else {
            if (line.empty() && ends_with(result, " ") && !is_last) {
                // Remove space if this is an empty comment or a doc comment.
                pop_char(result);
            }
            if (code_block_attr && is_prev_line_multi_line) result += comment_line_separator;
            result += line;
            fmt.shape = Shape::legacy(max_width, fmt_indent);
            is_prev_line_multi_line = false;
        }
        return false;
    }
};

/// Trim trailing whitespaces unless they consist of two or more whitespaces.
static std::string_view trim_end_unless_two_whitespaces(std::string_view s, bool is_doc_comment) {
    if (is_doc_comment && ends_with(s, "  ")) return s;
    return trim_end(s);
}

/// Trims comment characters and possibly a single space from the left of a string.
static std::pair<std::string_view, bool> left_trim_comment_line(std::string_view line, const CommentStyle& style) {
    if (starts_with(line, "//! ") || starts_with(line, "/// ") || starts_with(line, "/*! ") ||
        starts_with(line, "/** ")) {
        return {line.substr(4), true};
    }
    if (style.kind == CommentStyleKind::Custom) {
        std::string_view op = style.custom;
        if (starts_with(line, op)) return {line.substr(op.size()), true};
        return {slice_from(line, trim_end(op).size()), false};
    }
    if (starts_with(line, "/* ") || starts_with(line, "// ") || starts_with(line, "//!") || starts_with(line, "///") ||
        starts_with(line, "** ") || starts_with(line, "/*!") ||
        (starts_with(line, "/**") && !starts_with(line, "/**/"))) {
        return {line.substr(3), line[2] == ' '};
    }
    if (starts_with(line, "/*") || starts_with(line, "* ") || starts_with(line, "//") || starts_with(line, "**")) {
        return {line.substr(2), line[1] == ' '};
    }
    if (starts_with(line, "*")) return {line.substr(1), false};
    return {line, starts_with(line, " ")};
}

static RewriteResult rewrite_comment_inner(std::string_view orig, bool block_style, CommentStyle style, Shape shape,
                                           const Config& config, bool is_doc_comment) {
    CommentRewrite rewriter(orig, block_style, shape, config);

    size_t line_breaks = count_newlines(trim_end(orig));
    auto ls = lines(orig);
    for (size_t i = 0; i < ls.size(); i++) {
        std::string_view line = trim_end_unless_two_whitespaces(trim_start(ls[i]), is_doc_comment);
        // Drop old closer.
        if (i == line_breaks && ends_with(line, "*/") && !starts_with(line, "//")) {
            line = trim_end(line.substr(0, line.size() - 2));
        }
        auto [l2, has_leading_whitespace] = left_trim_comment_line(line, style);
        if (starts_with(orig, "/*") && line_breaks == 0) l2 = trim_start(l2);
        has_leading_whitespace = has_leading_whitespace || config.normalize_comments();
        if (rewriter.handle_line(orig, i, l2, has_leading_whitespace, is_doc_comment)) break;
    }
    return rewriter.finish();
}

/// Given the span, rewrite the missing comment inside it if available.
RewriteResult rewrite_missing_comment(Span span, Shape shape, const RewriteContext& ctx) {
    std::string_view missing_snippet = ctx.snippet(span);
    std::string_view trimmed_snippet = trim(missing_snippet);
    // check the span starts with a comment
    size_t pos = trimmed_snippet.find('/');
    if (!trimmed_snippet.empty() && pos != std::string_view::npos) {
        return rewrite_comment(trimmed_snippet, false, shape, *ctx.config);
    }
    return std::string();
}

RewriteResult recover_missing_comment_in_span(Span span, Shape shape, const RewriteContext& ctx, size_t used_width) {
    RewriteResult mc = rewrite_missing_comment(span, shape, ctx);
    if (!mc) return mc;
    if (mc->empty()) return std::string();
    std::string_view missing_snippet = ctx.snippet(span);
    size_t pos = missing_snippet.find('/');
    if (pos == std::string_view::npos) return Err(RewriteError::unknown());
    // 1 = ` `
    size_t total_width = mc->size() + used_width + 1;
    bool force_new_line_before_comment =
        contains(missing_snippet.substr(0, pos), '\n') || total_width > ctx.config->max_width();
    std::string sep = force_new_line_before_comment ? shape.indent.to_string_with_newline(*ctx.config) : " ";
    return sep + *mc;
}

/// Trims whitespace and aligns to indent, but otherwise does not change comments.
static std::string light_rewrite_comment(std::string_view orig, Indent offset, const Config& config,
                                         bool is_doc_comment) {
    std::string sep = "\n" + offset.to_string(config);
    std::string out;
    bool first = true;
    for (std::string_view l : lines(orig)) {
        // This is basically just l.trim(), but in the case that a line starts
        // with `*` we want to leave one space before it, so it aligns with the
        // `*` in `/*`.
        auto fnw = find_non_ws(l);
        std::string_view left_trimmed;
        if (fnw) {
            if (l[*fnw] == '*' && *fnw > 0) {
                left_trimmed = slice_from(l, *fnw - 1);
            } else {
                left_trimmed = l.substr(*fnw);
            }
        }
        if (!first) out += sep;
        first = false;
        // Preserve markdown's double-space line break syntax in doc comment.
        out += trim_end_unless_two_whitespaces(left_trimmed, is_doc_comment);
    }
    return out;
}

// ===========================================================================
// CharClasses

namespace {

enum class Status : uint8_t {
    Normal,
    LitString,
    LitStringEscape,
    LitRawString,
    RawStringPrefix,
    RawStringSuffix,
    LitChar,
    LitCharEscape,
    BlockComment,
    StringInBlockComment,
    BlockCommentOpening,
    BlockCommentClosing,
    LineComment,
};

/// CharClasses over `s.char_indices()`. MultiPeek is modelled by peek_n:
/// within one next() call, rustfmt's successive peeks see successive chars.
class CharClasses {
public:
    explicit CharClasses(std::string_view s) : s_(s) {}
    Status status = Status::Normal;
    uint32_t n = 0;  // the sharps / deepness payload of `status`

    std::optional<ClassifiedChar> next() {
        if (pos_ >= s_.size()) return std::nullopt;
        size_t len = 1;
        uint32_t chr = decode_at(s_, pos_, len);
        ClassifiedChar item{FullCodeCharKind::Normal, pos_, chr, len};
        pos_ += len;
        peek_pos_ = pos_;
        FullCodeCharKind char_kind = FullCodeCharKind::Normal;
        auto ret = [&](FullCodeCharKind k) {
            item.kind = k;
            return item;
        };
        switch (status) {
            case Status::LitRawString: {
                uint32_t sharps = n;
                char_kind = FullCodeCharKind::InString;
                if (chr == '"') {
                    if (sharps == 0) {
                        char_kind = FullCodeCharKind::Normal;
                        set(Status::Normal);
                    } else if (is_raw_string_suffix(sharps)) {
                        set(Status::RawStringSuffix, sharps);
                    } else {
                        set(Status::LitRawString, sharps);
                    }
                }
                break;
            }
            case Status::RawStringPrefix: {
                char_kind = FullCodeCharKind::InString;
                if (chr == '#')
                    set(Status::RawStringPrefix, n + 1);
                else if (chr == '"')
                    set(Status::LitRawString, n);
                else
                    set(Status::Normal);  // Unreachable.
                break;
            }
            case Status::RawStringSuffix: {
                if (chr == '#') {
                    if (n == 1) {
                        set(Status::Normal);
                    } else {
                        char_kind = FullCodeCharKind::InString;
                        set(Status::RawStringSuffix, n - 1);
                    }
                } else {
                    set(Status::Normal);  // Unreachable
                }
                break;
            }
            case Status::LitString:
                char_kind = FullCodeCharKind::InString;
                if (chr == '"')
                    set(Status::Normal);
                else if (chr == '\\')
                    set(Status::LitStringEscape);
                break;
            case Status::LitStringEscape:
                char_kind = FullCodeCharKind::InString;
                set(Status::LitString);
                break;
            case Status::LitChar:
                if (chr == '\\')
                    set(Status::LitCharEscape);
                else if (chr == '\'')
                    set(Status::Normal);
                break;
            case Status::LitCharEscape: set(Status::LitChar); break;
            case Status::Normal:
                switch (chr) {
                    case 'r': {
                        auto p = peek();
                        if (p && (*p == '#' || *p == '"')) {
                            char_kind = FullCodeCharKind::InString;
                            set(Status::RawStringPrefix, 0);
                        } else {
                            set(Status::Normal);
                        }
                        break;
                    }
                    case '"':
                        char_kind = FullCodeCharKind::InString;
                        set(Status::LitString);
                        break;
                    case '\'': {
                        // HACK: Work around mut borrow. (Two successive MultiPeek peeks: the
                        // second one sees the char after the next.)
                        auto p1 = peek();
                        if (p1 && *p1 == '\\') {
                            set(Status::LitChar);
                            return ret(char_kind);
                        }
                        auto p2 = peek();
                        if (p2 && *p2 == '\'')
                            set(Status::LitChar);
                        else
                            set(Status::Normal);
                        break;
                    }
                    case '/': {
                        auto p = peek();
                        if (p && *p == '*') {
                            set(Status::BlockCommentOpening, 1);
                            return ret(FullCodeCharKind::StartComment);
                        }
                        if (p && *p == '/') {
                            set(Status::LineComment);
                            return ret(FullCodeCharKind::StartComment);
                        }
                        set(Status::Normal);
                        break;
                    }
                    default: set(Status::Normal); break;
                }
                break;
            case Status::StringInBlockComment: {
                uint32_t deepness = n;
                char_kind = FullCodeCharKind::InStringCommented;
                if (chr == '"') {
                    set(Status::BlockComment, deepness);
                } else if (chr == '*') {
                    auto p = peek();
                    if (p && *p == '/') {
                        char_kind = FullCodeCharKind::InComment;
                        set(Status::BlockCommentClosing, deepness - 1);
                    }
                }
                break;
            }
            case Status::BlockComment: {
                uint32_t deepness = n;
                if (deepness == 0) panic("comment.rs: CharClasses BlockComment(0)");
                char_kind = FullCodeCharKind::InComment;
                auto p = peek();
                if (p && *p == '/' && chr == '*') {
                    set(Status::BlockCommentClosing, deepness - 1);
                } else if (p && *p == '*' && chr == '/') {
                    set(Status::BlockCommentOpening, deepness + 1);
                } else if (chr == '"') {
                    set(Status::StringInBlockComment, deepness);
                }
                break;
            }
            case Status::BlockCommentOpening:
                if (chr != '*') panic("comment.rs: CharClasses BlockCommentOpening");
                set(Status::BlockComment, n);
                return ret(FullCodeCharKind::InComment);
            case Status::BlockCommentClosing:
                if (chr != '/') panic("comment.rs: CharClasses BlockCommentClosing");
                if (n == 0) {
                    set(Status::Normal);
                    return ret(FullCodeCharKind::EndComment);
                }
                set(Status::BlockComment, n);
                return ret(FullCodeCharKind::InComment);
            case Status::LineComment:
                if (chr == '\n') {
                    set(Status::Normal);
                    return ret(FullCodeCharKind::EndComment);
                }
                return ret(FullCodeCharKind::InComment);
        }
        return ret(char_kind);
    }

private:
    std::string_view s_;
    size_t pos_ = 0;
    size_t peek_pos_ = 0;

    void set(Status st, uint32_t v = 0) {
        status = st;
        n = v;
    }
    /// MultiPeek::peek: each call within one next() sees the following char.
    std::optional<uint32_t> peek() {
        if (peek_pos_ >= s_.size()) return std::nullopt;
        size_t len = 1;
        uint32_t c = decode_at(s_, peek_pos_, len);
        peek_pos_ += len;
        return c;
    }
    bool is_raw_string_suffix(uint32_t count) {
        for (uint32_t k = 0; k < count; k++) {
            auto p = peek();
            if (p && *p == '#') continue;
            return false;
        }
        return true;
    }
};

}  // namespace

bool fcck_is_comment(FullCodeCharKind k) {
    switch (k) {
        case FullCodeCharKind::StartComment:
        case FullCodeCharKind::InComment:
        case FullCodeCharKind::EndComment:
        case FullCodeCharKind::StartStringCommented:
        case FullCodeCharKind::InStringCommented:
        case FullCodeCharKind::EndStringCommented: return true;
        default: return false;
    }
}

bool fcck_inside_comment(FullCodeCharKind k) {
    switch (k) {
        case FullCodeCharKind::InComment:
        case FullCodeCharKind::StartStringCommented:
        case FullCodeCharKind::InStringCommented:
        case FullCodeCharKind::EndStringCommented: return true;
        default: return false;
    }
}

bool fcck_is_string(FullCodeCharKind k) {
    return k == FullCodeCharKind::InString || k == FullCodeCharKind::StartString;
}

bool fcck_is_commented_string(FullCodeCharKind k) {
    return k == FullCodeCharKind::InStringCommented || k == FullCodeCharKind::StartStringCommented;
}

CodeCharKind fcck_to_codecharkind(FullCodeCharKind k) {
    return fcck_is_comment(k) ? CodeCharKind::Comment : CodeCharKind::Normal;
}

std::vector<ClassifiedChar> char_classes(std::string_view s) {
    std::vector<ClassifiedChar> out;
    out.reserve(s.size());
    CharClasses it(s);
    while (auto c = it.next()) out.push_back(*c);
    return out;
}

// FindUncommented::find_uncommented
std::optional<size_t> find_uncommented(std::string_view s, std::string_view pat) {
    size_t needle = 0;  // byte position in pat (needle_iter)
    CharClasses it(s);
    while (auto cc = it.next()) {
        if (needle >= pat.size()) return cc->pos - pat.size();
        size_t len = 1;
        uint32_t c = decode_at(pat, needle, len);
        needle += len;
        if ((cc->kind == FullCodeCharKind::Normal || cc->kind == FullCodeCharKind::InString) && cc->ch == c) {
            // matched
        } else {
            needle = 0;
        }
    }
    // Handle case where the pattern is a suffix of the search string
    if (needle < pat.size()) return std::nullopt;
    return s.size() - pat.size();
}

// FindUncommented::find_last_uncommented
std::optional<size_t> find_last_uncommented(std::string_view s, std::string_view pat) {
    auto left = find_uncommented(s, pat);
    if (!left) return std::nullopt;
    size_t result = *left;
    // add 1 to use find_last_uncommented for &str after pat
    while (auto next = find_last_uncommented(slice_from(s, result + 1), pat)) result += *next + 1;
    return result;
}

std::optional<size_t> find_comment_end(std::string_view s) {
    CharClasses it(s);
    while (auto cc = it.next()) {
        if (cc->kind == FullCodeCharKind::Normal || cc->kind == FullCodeCharKind::InString) return cc->pos;
    }
    // Handle case where the comment ends at the end of `s`.
    if (it.status == Status::Normal) return s.size();
    return std::nullopt;
}

bool contains_comment(std::string_view text) {
    CharClasses it(text);
    while (auto cc = it.next()) {
        if (fcck_is_comment(cc->kind)) return true;
    }
    return false;
}

// LineClasses
std::vector<std::pair<FullCodeCharKind, std::string>> line_classes(std::string_view s) {
    std::vector<std::pair<FullCodeCharKind, std::string>> out;
    CharClasses it(s);
    std::optional<ClassifiedChar> peeked = it.next();
    FullCodeCharKind kind = FullCodeCharKind::Normal;  // self.kind
    while (peeked) {
        std::string line;
        FullCodeCharKind start_kind = peeked->kind;
        while (peeked) {
            ClassifiedChar cc = *peeked;
            peeked = it.next();
            // needed to set the kind of the ending character on the last line
            kind = cc.kind;
            if (cc.ch == '\n') {
                if (start_kind == FullCodeCharKind::Normal && cc.kind == FullCodeCharKind::InString)
                    kind = FullCodeCharKind::StartString;
                else if (start_kind == FullCodeCharKind::InString && cc.kind == FullCodeCharKind::Normal)
                    kind = FullCodeCharKind::EndString;
                else if (start_kind == FullCodeCharKind::InComment && cc.kind == FullCodeCharKind::InStringCommented)
                    kind = FullCodeCharKind::StartStringCommented;
                else if (start_kind == FullCodeCharKind::InStringCommented && cc.kind == FullCodeCharKind::InComment)
                    kind = FullCodeCharKind::EndStringCommented;
                break;
            }
            line.append(s.substr(cc.pos, cc.len));
        }
        // Workaround for CRLF newline.
        if (ends_with(line, "\r")) line.pop_back();
        out.emplace_back(kind, std::move(line));
    }
    return out;
}

// UngroupedCommentCodeSlices
std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> ungrouped_comment_code_slices(std::string_view s) {
    std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> out;
    CharClasses it(s);
    std::optional<ClassifiedChar> peeked = it.next();  // the Peekable's look-ahead
    auto next = [&]() {
        std::optional<ClassifiedChar> r = peeked;
        if (peeked) peeked = it.next();
        return r;
    };
    while (true) {
        auto first = next();
        if (!first) break;
        FullCodeCharKind kind = first->kind;
        size_t start_idx = first->pos;
        switch (kind) {
            case FullCodeCharKind::Normal:
            case FullCodeCharKind::InString:
                // Consume all the Normal code
                while (peeked) {
                    if (fcck_is_comment(peeked->kind)) break;
                    next();
                }
                break;
            case FullCodeCharKind::StartComment:
                // Consume the whole comment
                while (true) {
                    auto n = next();
                    if (n && fcck_inside_comment(n->kind)) continue;
                    break;
                }
                break;
            default: panic("comment.rs: UngroupedCommentCodeSlices");
        }
        std::string_view slice = peeked ? s.substr(start_idx, peeked->pos - start_idx) : s.substr(start_idx);
        out.emplace_back(fcck_is_comment(kind) ? CodeCharKind::Comment : CodeCharKind::Normal, start_idx, slice);
    }
    return out;
}

// CommentCodeSlices
std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> comment_code_slices(std::string_view s) {
    std::vector<std::tuple<CodeCharKind, size_t, std::string_view>> out;
    CodeCharKind last_slice_kind = CodeCharKind::Comment;
    size_t last_slice_end = 0;
    while (last_slice_end != s.size()) {
        size_t sub_slice_end = last_slice_end;
        std::optional<size_t> first_whitespace;
        std::string_view subslice = s.substr(last_slice_end);
        CharClasses it(subslice);
        while (auto cc = it.next()) {
            size_t i = cc->pos;
            uint32_t c = cc->ch;
            bool is_comment_connector = last_slice_kind == CodeCharKind::Normal &&
                                        subslice.substr(0, 2) == "//" && (c == ' ' || c == '\t');
            if (is_comment_connector && !first_whitespace) first_whitespace = i;
            if (fcck_to_codecharkind(cc->kind) == last_slice_kind && !is_comment_connector) {
                size_t last_index = first_whitespace ? *first_whitespace : i;
                sub_slice_end = last_slice_end + last_index;
                break;
            }
            if (!is_comment_connector) first_whitespace.reset();
        }
        if (!it.next() && sub_slice_end == last_slice_end) {
            // This was the last subslice.
            sub_slice_end = first_whitespace ? last_slice_end + *first_whitespace : s.size();
        }
        CodeCharKind kind = last_slice_kind == CodeCharKind::Comment ? CodeCharKind::Normal : CodeCharKind::Comment;
        out.emplace_back(kind, last_slice_end, s.substr(last_slice_end, sub_slice_end - last_slice_end));
        last_slice_end = sub_slice_end;
        last_slice_kind = kind;
    }
    return out;
}

// ---------------------------------------------------------------------------
// recover_comment_removed / changed_comment_content / CommentReducer

static std::string_view remove_comment_header(std::string_view comment) {
    if (starts_with(comment, "///") || starts_with(comment, "//!")) return comment.substr(3);
    if (starts_with(comment, "//")) return comment.substr(2);
    if ((starts_with(comment, "/**") && !starts_with(comment, "/**/")) || starts_with(comment, "/*!")) {
        if (comment.size() < 5) panic("comment.rs: remove_comment_header slice");
        return comment.substr(3, comment.size() - 5);
    }
    if (!starts_with(comment, "/*")) panic("comment.rs: string is not a comment");
    if (comment.size() < 4) panic("comment.rs: remove_comment_header slice");
    return comment.substr(2, comment.size() - 4);
}

/// CommentReducer::new(comment) collected: the payload chars of one comment.
static void comment_reducer(std::string_view comment, std::vector<uint32_t>& out) {
    bool is_block = starts_with(comment, "/*");
    std::string_view body = remove_comment_header(comment);
    bool at_start_line = false;  // There are no supplementary '*' on the first line.
    size_t i = 0;
    auto next = [&]() -> std::optional<uint32_t> {
        if (i >= body.size()) return std::nullopt;
        size_t len = 1;
        uint32_t c = decode_at(body, i, len);
        i += len;
        return c;
    };
    while (true) {
        auto c = next();
        if (!c) return;
        if (is_block && at_start_line) {
            while (is_uws(*c)) {
                c = next();
                if (!c) return;
            }
            // Ignore leading '*'.
            if (*c == '*') {
                c = next();
                if (!c) return;
            }
        } else if (*c == '\n') {
            at_start_line = true;
        }
        if (!is_uws(*c)) out.push_back(*c);
    }
}

static std::vector<uint32_t> code_comment_content(std::string_view code) {
    std::vector<uint32_t> out;
    for (auto& [kind, off, slice] : ungrouped_comment_code_slices(code)) {
        (void)off;
        if (kind == CodeCharKind::Comment) comment_reducer(slice, out);
    }
    return out;
}

/// Returns `true` if the two strings of code have the same payload of comments.
static bool changed_comment_content(std::string_view orig, std::string_view new_) {
    return code_comment_content(orig) != code_comment_content(new_);
}

std::string recover_comment_removed(std::string new_, Span span, const RewriteContext& ctx) {
    std::string_view snippet = ctx.snippet(span);
    if (snippet != new_ && changed_comment_content(snippet, new_)) {
        // We missed some comments. Warn and keep the original text.
        if (ctx.config->error_on_unformatted()) ctx.state->has_unformatted_code = true;  // LostComment
        return std::string(snippet);
    }
    return new_;
}

std::string filter_normal_code(std::string_view code) {
    std::string buffer;
    buffer.reserve(code.size());
    for (auto& [kind, line] : line_classes(code)) {
        switch (kind) {
            case FullCodeCharKind::Normal:
            case FullCodeCharKind::StartString:
            case FullCodeCharKind::InString:
            case FullCodeCharKind::EndString:
                buffer += line;
                buffer += '\n';
                break;
            default: break;
        }
    }
    if (!ends_with(code, "\n") && ends_with(buffer, "\n")) buffer.pop_back();
    return buffer;
}

// ===========================================================================
// string.rs

namespace {

constexpr size_t MIN_STRING = 10;

/// General_Category = Mn / Me / Mc (approximate) or a grapheme extender.
bool is_grapheme_extend(uint32_t c) {
    return (c >= 0x300 && c <= 0x36f) || (c >= 0x483 && c <= 0x489) || (c >= 0x591 && c <= 0x5bd) ||
           (c >= 0x610 && c <= 0x61a) || (c >= 0x64b && c <= 0x65f) || (c >= 0x900 && c <= 0x903) ||
           (c >= 0x93a && c <= 0x94f && c != 0x93d) || (c >= 0x1ab0 && c <= 0x1aff) ||
           (c >= 0x1dc0 && c <= 0x1dff) || c == 0x200c || c == 0x200d || (c >= 0x20d0 && c <= 0x20ff) ||
           (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f) || (c >= 0x1f3fb && c <= 0x1f3ff) ||
           (c >= 0xe0100 && c <= 0xe01ef) || (c >= 0xe0020 && c <= 0xe007f);
}

/// UnicodeSegmentation::graphemes(s, false), approximated: "\r\n" is one
/// cluster, combining marks / ZWJ sequences / variation selectors attach to
/// the previous char.
std::vector<std::string_view> graphemes(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i, len = 1;
        uint32_t c = decode_at(s, i, len);
        i += len;
        if (c == '\r' && i < s.size() && s[i] == '\n') {
            i++;
        } else if (c != '\r' && c != '\n') {
            bool after_zwj = false;
            while (i < s.size()) {
                size_t l2 = 1;
                uint32_t d = decode_at(s, i, l2);
                if (is_grapheme_extend(d) || (after_zwj && d >= 0x1f000)) {
                    after_zwj = d == 0x200d;
                    i += l2;
                } else {
                    break;
                }
            }
        }
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

bool g_is_new_line(std::string_view g) { return starts_with(g, "\n") || starts_with(g, "\r\n"); }

bool g_is_whitespace(std::string_view g) {
    size_t i = 0;
    while (i < g.size()) {
        size_t len = 1;
        uint32_t c = decode_at(g, i, len);
        if (!is_uws(c)) return false;
        i += len;
    }
    return true;
}

/// GeneralCategory::OtherPunctuation (Po), approximated: ASCII exact plus the
/// common Latin-1 / general punctuation / CJK Po characters.
bool is_po(uint32_t c) {
    switch (c) {
        case '!': case '"': case '#': case '%': case '&': case '\'': case '*': case ',': case '.': case '/':
        case ':': case ';': case '?': case '@': case '\\':
        case 0xa1: case 0xa7: case 0xb6: case 0xb7: case 0xbf: case 0x37e: case 0x387:
        case 0x2016: case 0x2017: case 0x2020: case 0x2021: case 0x2022: case 0x2023: case 0x2024: case 0x2025:
        case 0x2026: case 0x2027: case 0x2030: case 0x2031: case 0x2032: case 0x2033: case 0x2034: case 0x2035:
        case 0x2036: case 0x2037: case 0x2038: case 0x203b: case 0x203c: case 0x203d: case 0x203e:
        case 0x3001: case 0x3002: case 0x3003: case 0x30fb: case 0xff01: case 0xff0c: case 0xff0e: case 0xff1a:
        case 0xff1b: case 0xff1f:
            return true;
        default: return false;
    }
}

bool g_is_punctuation(std::string_view g) {
    size_t i = 0;
    while (i < g.size()) {
        size_t len = 1;
        uint32_t c = decode_at(g, i, len);
        if (!is_po(c)) return false;
        i += len;
    }
    return true;
}

size_t graphemes_width(const std::vector<std::string_view>& gs, size_t from) {
    size_t w = 0;
    for (size_t k = from; k < gs.size(); k++) w += unicode_str_width(gs[k]);
    return w;
}

std::string concat(const std::vector<std::string_view>& gs, size_t from, size_t to) {
    std::string r;
    for (size_t k = from; k < to && k < gs.size(); k++) r += gs[k];
    return r;
}

bool not_whitespace_except_line_feed(std::string_view g) { return g_is_new_line(g) || !g_is_whitespace(g); }

/// [[:space:]] (ASCII).
bool is_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

/// Regex `([^\\](\\\\)*)\\[\n\r][[:space:]]*` replaced by "$1".
std::string strip_line_breaks(std::string_view s) {
    std::string out;
    size_t copied = 0;  // s[..copied] is in out
    size_t p = 0;       // search position
    while (p < s.size()) {
        bool matched = false;
        size_t i = p;
        while (i < s.size()) {
            size_t len = 1;
            uint32_t c = decode_at(s, i, len);
            if (c != '\\') {
                size_t j = i + len;
                size_t k = j;
                while (k < s.size() && s[k] == '\\') k++;
                size_t run = k - j;
                if (run % 2 == 1 && k < s.size() && (s[k] == '\n' || s[k] == '\r')) {
                    // match: X + (run-1) backslashes kept
                    out.append(s.substr(copied, k - 1 - copied));
                    size_t e = k + 1;
                    while (e < s.size() && is_ascii_space(s[e])) e++;
                    copied = e;
                    p = e;
                    matched = true;
                    break;
                }
            }
            i += len;
        }
        if (!matched) break;
    }
    out.append(s.substr(copied));
    return out;
}

/// Trims whitespaces to the right except for the line feed character.
std::string trim_end_but_line_feed(bool trim_end_, std::string result) {
    if (!trim_end_) return result;
    // trim_end_matches(|c| c.is_whitespace() && c != '\n')
    while (!result.empty()) {
        size_t i = result.size() - 1;
        while (i > 0 && (static_cast<unsigned char>(result[i]) & 0xc0) == 0x80) i--;
        size_t len = 1;
        uint32_t c = decode_at(result, i, len);
        if (is_uws(c) && c != '\n')
            result.resize(i);
        else
            break;
    }
    return result;
}

struct SnippetState {
    enum Kind : uint8_t { EndOfInput, LineEnd, EndWithLineFeed } kind = EndOfInput;
    std::string line;
    size_t len = 0;
};

/// Returns the index to the end of the URL if the split at index of the given string includes a
/// URL or alike. Otherwise, returns `None`.
std::optional<size_t> detect_url(const std::vector<std::string_view>& s, size_t base, size_t index) {
    // s is graphemes[base..]; index is relative to base.
    size_t n = s.size() - base;
    size_t start = 0;
    for (size_t k = index + 1; k-- > 0;) {
        if (g_is_whitespace(s[base + k])) {
            start = k + 1;
            break;
        }
    }
    // 8 = minimum length for a string to contain a URL
    if (n < start + 8) return std::nullopt;
    std::string split = concat(s, base + start, s.size());
    if (contains(split, "https://") || contains(split, "http://") || contains(split, "ftp://") ||
        contains(split, "file://")) {
        for (size_t k = index; k < n; k++) {
            if (g_is_whitespace(s[base + k])) return index + (k - index) - 1;
        }
        return n - 1;
    }
    return std::nullopt;
}

bool is_part_of_type(const std::vector<std::string_view>& in, size_t base, size_t pos) {
    size_t n = in.size() - base;
    auto at = [&](size_t k) { return in[base + k]; };
    if (pos + 1 < n && at(pos) == ":" && at(pos + 1) == ":") return true;
    size_t p0 = sat_sub(pos, 1);
    return pos < n && at(p0) == ":" && at(pos) == ":" && p0 + 1 == pos;
}

bool is_valid_linebreak(const std::vector<std::string_view>& in, size_t base, size_t pos) {
    if (g_is_whitespace(in[base + pos])) return true;
    if (g_is_punctuation(in[base + pos]) && !is_part_of_type(in, base, pos)) return true;
    return false;
}

/// Break the input string at a boundary character around the offset `max_width`. `input` is
/// graphemes[base..].
SnippetState break_string(size_t max_width, bool trim_end_, std::string_view line_end,
                          const std::vector<std::string_view>& gs, size_t base) {
    size_t n = gs.size() - base;
    auto in = [&](size_t k) { return gs[base + k]; };
    auto break_at = [&](size_t index /* grapheme at index is included */) -> SnippetState {
        // Take in any whitespaces to the left/right of `input[index]` while
        // preserving line feeds
        size_t index_minus_ws = index;
        for (size_t k = index + 1; k-- > 0;) {
            if (not_whitespace_except_line_feed(in(k))) {
                index_minus_ws = k;
                break;
            }
        }
        // Take into account newlines occurring in input[0..=index], i.e., the possible next new
        // line.
        for (size_t i = 0; i <= index; i++) {
            if (g_is_new_line(in(i))) {
                if (i <= index_minus_ws) {
                    std::string line = concat(gs, base, base + i);
                    std::string_view lv = line;
                    if (trim_end_) lv = trim_end(lv);
                    return SnippetState{SnippetState::EndWithLineFeed, std::string(lv) + "\n", i + 1};
                }
                break;
            }
        }
        size_t index_plus_ws = index;
        for (size_t i = 0; index + 1 + i < n; i++) {
            std::string_view g = in(index + 1 + i);
            if (!trim_end_ && g_is_new_line(g)) {
                return SnippetState{SnippetState::EndWithLineFeed, concat(gs, base, base + index + 2 + i),
                                    index + 2 + i};
            } else if (not_whitespace_except_line_feed(g)) {
                index_plus_ws = index + i;
                break;
            }
        }
        if (trim_end_)
            return SnippetState{SnippetState::LineEnd, concat(gs, base, base + index_minus_ws + 1), index_plus_ws + 1};
        return SnippetState{SnippetState::LineEnd, concat(gs, base, base + index_plus_ws + 1), index_plus_ws + 1};
    };

    // find a first index where the unicode width of input[0..x] become > max_width
    size_t max_width_index_in_input = 0;
    {
        size_t cur_width = 0;
        for (size_t i = 0; i < n; i++) {
            cur_width += unicode_str_width(in(i));
            max_width_index_in_input = i;
            if (cur_width > max_width) break;
        }
    }
    if (max_width_index_in_input == 0) return SnippetState{SnippetState::EndOfInput, concat(gs, base, gs.size()), 0};

    // Find the position in input for breaking the string
    if (line_end.empty() && trim_end_ && !g_is_whitespace(in(max_width_index_in_input - 1)) &&
        g_is_whitespace(in(max_width_index_in_input))) {
        // At a breaking point already
        return break_at(max_width_index_in_input - 1);
    }
    if (auto url_index_end = detect_url(gs, base, max_width_index_in_input)) {
        size_t skip_pos = 0;
        for (size_t k = *url_index_end + 1; k < n; k++) {
            if (not_whitespace_except_line_feed(in(k))) {
                skip_pos = k - (*url_index_end + 1);
                break;
            }
        }
        size_t index_plus_ws = *url_index_end + skip_pos;
        if (trim_end_)
            return SnippetState{SnippetState::LineEnd, concat(gs, base, base + *url_index_end + 1), index_plus_ws + 1};
        return SnippetState{SnippetState::LineEnd, concat(gs, base, base + index_plus_ws + 1), index_plus_ws + 1};
    }

    std::optional<size_t> ws_index;
    for (size_t k = max_width_index_in_input; k-- > 0;) {
        if (g_is_whitespace(in(k))) {
            ws_index = k;
            break;
        }
    }
    // Found a whitespace and what is on its left side is big enough.
    if (ws_index && *ws_index >= MIN_STRING) return break_at(*ws_index);
    // No whitespace found, try looking for a punctuation instead
    std::optional<size_t> punct;
    for (size_t k = max_width_index_in_input; k-- > 0;) {
        if (is_valid_linebreak(gs, base, k)) {
            punct = k;
            break;
        }
    }
    // Found a punctuation and what is on its left side is big enough.
    if (punct && *punct >= MIN_STRING) return break_at(*punct);
    // Either no boundary character was found to the left of `input[max_chars]`, or the line
    // got too small. We try searching for a boundary character to the right.
    for (size_t k = max_width_index_in_input; k < n; k++) {
        if (is_valid_linebreak(gs, base, k)) return break_at(k);
    }
    // No boundary to the right, the input cannot be broken
    return SnippetState{SnippetState::EndOfInput, concat(gs, base, gs.size()), 0};
}

}  // namespace

Opt rewrite_string(std::string_view orig, const StringFormat& fmt, size_t newline_max_chars) {
    // StringFormat::max_width_with_indent
    auto mwi = checked_sub(fmt.shape.width, fmt.opener.size() + fmt.line_end.size() + 1);
    if (!mwi) return std::nullopt;
    size_t max_width_with_indent = *mwi + 1;
    // StringFormat::max_width_without_indent
    auto mwo = checked_sub(fmt.config->max_width(), fmt.line_end.size());
    if (!mwo) return std::nullopt;
    size_t max_width_without_indent = *mwo;
    std::string indent_with_newline = fmt.shape.indent.to_string_with_newline(*fmt.config);
    std::string indent_without_newline = fmt.shape.indent.to_string(*fmt.config);

    // Strip line breaks.
    // With this regex applied, all remaining whitespaces are significant
    std::string stripped_str = strip_line_breaks(orig);
    std::vector<std::string_view> gs = graphemes(stripped_str);

    // `cur_start` is the position in `orig` of the start of the current line.
    size_t cur_start = 0;
    std::string result;
    result += fmt.opener;

    // Snip a line at a time from `stripped_str` until it is used up. Push the snippet
    // onto result.
    size_t cur_max_width = max_width_with_indent;
    bool is_bareline_ok = fmt.line_start.empty() || g_is_whitespace(fmt.line_start);
    while (true) {
        // All the input starting at cur_start fits on the current line
        if (graphemes_width(gs, cur_start) <= cur_max_width) {
            for (size_t i = 0; cur_start + i < gs.size(); i++) {
                std::string_view g = gs[cur_start + i];
                if (g_is_new_line(g)) {
                    // take care of blank lines
                    result = trim_end_but_line_feed(fmt.trim_end, std::move(result));
                    result += '\n';
                    if (!is_bareline_ok && cur_start + i + 1 < gs.size()) {
                        result += indent_without_newline;
                        result += fmt.line_start;
                    }
                } else {
                    result += g;
                }
            }
            result = trim_end_but_line_feed(fmt.trim_end, std::move(result));
            break;
        }

        // The input starting at cur_start needs to be broken
        SnippetState st = break_string(cur_max_width, fmt.trim_end, fmt.line_end, gs, cur_start);
        if (st.kind == SnippetState::LineEnd) {
            result += st.line;
            result += fmt.line_end;
            result += indent_with_newline;
            result += fmt.line_start;
            cur_max_width = newline_max_chars;
            cur_start += st.len;
        } else if (st.kind == SnippetState::EndWithLineFeed) {
            if (st.line == "\n" && fmt.trim_end) result = std::string(trim_end(result));
            result += st.line;
            if (is_bareline_ok) {
                // the next line can benefit from the full width
                cur_max_width = max_width_without_indent;
            } else {
                result += indent_without_newline;
                result += fmt.line_start;
                cur_max_width = max_width_with_indent;
            }
            cur_start += st.len;
        } else {
            result += st.line;
            break;
        }
    }

    result += fmt.closer;
    return wrap_str(std::move(result), fmt.config->max_width(), fmt.shape);
}

}  // namespace rustfmt
