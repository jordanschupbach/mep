// rust_format_missed_spans.cpp -- rustfmt's missed_spans.rs: the source
// between formatted AST nodes (comments, blank lines, unformattable code).
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
//
// Default config: file_lines is "all" (every within_file_lines_range check is
// true, out_of_file_lines_range! is false) and emit_mode is not Coverage
// (transform_missing_snippet is the identity).
#include "rust_format_internal.h"

namespace rustfmt {

namespace {

struct SnippetStatus {
    /// An offset to the current line from the beginning of the original snippet.
    size_t line_start = 0;
    /// A length of trailing whitespaces on the current line.
    std::optional<size_t> last_wspace;
    /// The current line number (only used for file_lines, kept for parity).
    size_t cur_line = 0;
};

// format_missing_inner's process_last_snippet closures.
enum class LastSnippet : uint8_t { Plain, Indent, NoIndent };

bool output_at_start(const FmtVisitor& v) { return v.buffer.empty(); }

// char::is_whitespace (Unicode White_Space).
bool is_unicode_ws(uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

void process_last_snippet(FmtVisitor& v, LastSnippet how, std::string_view last_snippet, std::string_view snippet) {
    if (how == LastSnippet::Plain) {
        v.push_str(last_snippet);
        return;
    }
    // format_missing_indent
    v.push_str(trim_end(last_snippet));
    if (last_snippet == snippet && !output_at_start(v)) {
        // No new lines in the snippet.
        v.push_str("\n");
    }
    if (how == LastSnippet::Indent) v.push_str(v.block_indent.to_string(*v.config));
}

void push_vertical_spaces(FmtVisitor& v, size_t newline_count) {
    size_t offset = 0;
    for (size_t i = v.buffer.size(); i > 0 && v.buffer[i - 1] == '\n'; i--) offset++;
    size_t newline_upper_bound = v.config->blank_lines_upper_bound() + 1;
    size_t newline_lower_bound = v.config->blank_lines_lower_bound() + 1;

    if (newline_count + offset > newline_upper_bound) {
        if (offset >= newline_upper_bound) {
            newline_count = 0;
        } else {
            newline_count = newline_upper_bound - offset;
        }
    } else if (newline_count + offset < newline_lower_bound) {
        if (offset >= newline_lower_bound) {
            newline_count = 0;
        } else {
            newline_count = newline_lower_bound - offset;
        }
    }

    v.push_str(std::string(newline_count, '\n'));
}

void process_comment(FmtVisitor& v, SnippetStatus& status, std::string_view snippet, std::string_view big_snippet,
                     size_t offset, std::string_view subslice) {
    // big_snippet.chars().rev().find(|rev_c| ![' ', '\t'].contains(rev_c)): only compared
    // with '{' and '\n' below, so the last non-blank byte is enough.
    std::optional<char> last_char;
    for (size_t i = big_snippet.size(); i > 0; i--) {
        char c = big_snippet[i - 1];
        if (c != ' ' && c != '\t') {
            last_char = c;
            break;
        }
    }

    bool fix_indent = !last_char || *last_char == '{' || *last_char == '\n';
    bool on_same_line = false;

    Indent comment_indent;
    if (fix_indent) {
        if (last_char && *last_char == '{') v.push_str("\n");
        std::string indent_str = v.block_indent.to_string(*v.config);
        v.push_str(indent_str);
        comment_indent = v.block_indent;
    } else if (v.config->style_edition() >= StyleEdition::E2024 && !starts_with(snippet, "\n")) {
        // The comment appears on the same line as the previous formatted code.
        // Assuming that comment is logically associated with that code, we want to keep it on
        // the same level and avoid mixing it with possible other comment.
        on_same_line = true;
        v.push_str(" ");
        comment_indent = v.block_indent;
    } else {
        v.push_str(" ");
        comment_indent = Indent::from_width(*v.config, last_line_width(v.buffer));
    }

    size_t comment_width = std::min(v.config->comment_width(), v.config->max_width() - v.block_indent.width());
    Shape comment_shape = Shape::legacy(comment_width, comment_indent);

    if (on_same_line) {
        size_t nl = subslice.find('\n');
        if (nl == std::string_view::npos) {
            v.push_str(subslice);
        } else if (nl + 1 == subslice.size()) {
            v.push_str(subslice.substr(0, nl));
        } else {
            // keep first line as is: if it were too long and wrapped, it may get mixed
            // with the other lines.
            std::string_view first_line = subslice.substr(0, nl);
            v.push_str(first_line);
            v.push_str(comment_indent.to_string_with_newline(*v.config));

            std::string_view other_lines = subslice.substr(nl + 1);
            RewriteResult comment_str = rewrite_comment(other_lines, false, comment_shape, *v.config);
            v.push_str(comment_str ? std::string_view(*comment_str) : other_lines);
        }
    } else {
        RewriteResult comment_str = rewrite_comment(subslice, false, comment_shape, *v.config);
        v.push_str(comment_str ? std::string_view(*comment_str) : subslice);
    }

    status.last_wspace = std::nullopt;
    status.line_start = offset + subslice.size();

    // Add a newline:
    // - if there isn't one already
    // - otherwise, only if the last line is a line comment
    if (status.line_start <= snippet.size()) {
        std::string_view rest = snippet.substr(status.line_start);
        // skip trailing whitespaces
        size_t k = 0;
        while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) k++;
        if (k < rest.size() && (rest[k] == '\n' || rest[k] == '\r')) {
            if (!is_last_comment_block(subslice)) v.push_str("\n");
        } else {
            v.push_str("\n");
        }
    }

    status.cur_line += count_newlines(subslice);
}

void process_missing_code(FmtVisitor& v, SnippetStatus& status, std::string_view snippet, std::string_view subslice,
                          size_t offset) {
    for (size_t j = 0; j < subslice.size();) {
        size_t clen = 0;
        uint32_t c = rust::decode_utf8(subslice, j, clen);
        size_t i = j + offset;
        j += clen;

        if (c == '\n') {
            // (file_lines is all: skip_this_line is false)
            if (status.last_wspace) {
                size_t lw = *status.last_wspace;
                if (lw < status.line_start) panic("slice index starts after its end", static_cast<uint32_t>(lw));
                v.push_str(snippet.substr(status.line_start, lw - status.line_start));
                v.push_str("\n");
                status.last_wspace = std::nullopt;
            } else {
                v.push_str(snippet.substr(status.line_start, i + 1 - status.line_start));
            }

            status.cur_line += 1;
            status.line_start = i + 1;
        } else if (is_unicode_ws(c) && !status.last_wspace) {
            status.last_wspace = i;
        } else {
            status.last_wspace = std::nullopt;
        }
    }

    size_t end = subslice.size() + offset;
    std::string_view remaining = trim(snippet.substr(status.line_start, end - status.line_start));
    if (!remaining.empty()) {
        v.push_str(v.block_indent.to_string(*v.config));
        v.push_str(remaining);
        status.line_start = end;
    }
}

void write_snippet_inner(FmtVisitor& v, std::string_view big_snippet, std::string_view old_snippet, size_t big_diff,
                         Span /*span*/, LastSnippet how) {
    // Trim whitespace from the right hand side of each line.
    // Annoyingly, the library functions for splitting by lines etc. are not
    // quite right, so we must do it ourselves.
    SnippetStatus status;

    std::string_view snippet = old_snippet;  // transform_missing_snippet: identity

    for (const auto& [kind, offset, subslice] : comment_code_slices(snippet)) {
        auto [lf_count, crlf_count] = count_lf_crlf(subslice);
        size_t newline_count = lf_count + crlf_count;
        if (kind == CodeCharKind::Comment) {
            // 1: comment.
            process_comment(v, status, snippet, big_snippet.substr(0, offset + big_diff), offset, subslice);
        } else if (trim(subslice).empty() && newline_count > 0) {
            // 2: blank lines.
            push_vertical_spaces(v, newline_count);
            status.cur_line += newline_count;
            // To avoid any issues with whitespace unicode chars just add the len of the slice
            status.line_start = offset + subslice.size();
        } else {
            // 3: code which we failed to format or which is not within file-lines range.
            process_missing_code(v, status, snippet, subslice, offset);
        }
    }

    std::string_view last_snippet = snippet.substr(status.line_start);
    process_last_snippet(v, how, last_snippet, snippet);
}

void write_snippet(FmtVisitor& v, Span span, LastSnippet how) {
    // Get a snippet from the file start to the span's hi without allocating.
    // We need it to determine what precedes the current comment. If the comment
    // follows code on the same line, we won't touch it.
    uint32_t big_span_lo = v.snippet_provider->start_pos();
    std::string_view big_snippet = v.snippet_provider->entire_snippet();
    size_t big_diff = span.lo - big_span_lo;

    std::string_view snippet = v.snippet(span);

    write_snippet_inner(v, big_snippet, snippet, big_diff, span, how);
}

void format_missing_inner(FmtVisitor& v, uint32_t end, LastSnippet how) {
    uint32_t start = v.last_pos;

    if (start == end) {
        // Do nothing if this is the beginning of the file.
        if (!output_at_start(v)) process_last_snippet(v, how, "", "");
        return;
    }

    if (!(start < end)) panic("Request to format inverted span", start);

    v.last_pos = end;
    Span span = mk_sp(start, end);
    std::string_view snippet = v.snippet(span);

    // Do nothing for spaces in the beginning of the file
    if (start == 0 && end == snippet.size() && trim(snippet).empty()) return;

    if (trim(snippet).empty()) {
        // Keep vertical spaces within range.
        push_vertical_spaces(v, count_newlines(snippet));
        process_last_snippet(v, how, "", snippet);
    } else {
        write_snippet(v, span, how);
    }
}

void format_missing_indent(FmtVisitor& v, uint32_t end, bool should_indent) {
    format_missing_inner(v, end, should_indent ? LastSnippet::Indent : LastSnippet::NoIndent);
}

}  // namespace

void FmtVisitor::format_missing(uint32_t end) {
    // HACK(topecongiro): we use `format_missing()` to extract a missing comment between
    // a macro (or similar) and a trailing semicolon. Here we just try to avoid calling
    // `format_missing_inner` in the common case where there is no such comment.
    // This is a hack, ideally we should fix a possible bug in `format_missing_inner`
    // or refactor `visit_mac` and `rewrite_macro`, but this should suffice to fix the
    // issue (#2727).
    std::string_view missing_snippet = snippet(mk_sp(last_pos, end));
    if (trim(missing_snippet) == ";") {
        push_str(";");
        last_pos = end;
        return;
    }
    format_missing_inner(*this, end, LastSnippet::Plain);
}

void FmtVisitor::format_missing_with_indent(uint32_t end) { format_missing_indent(*this, end, true); }

void FmtVisitor::format_missing_no_indent(uint32_t end) { format_missing_indent(*this, end, false); }

}  // namespace rustfmt
