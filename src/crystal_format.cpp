// mep's Crystal formatter: Format() with its proof-reading, and the
// formatter's plumbing -- the original's lexer-sync helpers, writing,
// indentation and `finish` with its post-passes. See crystal_format.h and
// crystal_format_internal.h.

#include "crystal_format.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "crystal_format_internal.h"

namespace crfmt {

using crystal::Lexer;
using crystal::SyntaxError;

// --- text helpers ---------------------------------------------------------------------

namespace {

// Decodes the character at byte `i`; returns its width (1 for a stray byte).
size_t DecodeAt(std::string_view s, size_t i, char32_t *out) {
    const auto b = static_cast<unsigned char>(s[i]);
    size_t n;
    char32_t c;
    if (b >= 0xF0 && i + 3 < s.size()) {
        n = 4;
        c = b & 0x07u;
    } else if (b >= 0xE0 && i + 2 < s.size()) {
        n = 3;
        c = b & 0x0Fu;
    } else if (b >= 0xC0 && i + 1 < s.size()) {
        n = 2;
        c = b & 0x1Fu;
    } else {
        *out = b;
        return 1;
    }
    for (size_t k = 1; k < n; k++) c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    *out = c;
    return n;
}

// Char#whitespace?: ASCII whitespace or Unicode White_Space.
bool UnicodeWhitespace(char32_t c) {
    if (AsciiWhitespace(c)) return true;
    switch (c) {
        case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

// Byte offset of character `n` (or s.size() when past the end); `ok` false past the end.
size_t ByteOfChar(std::string_view s, size_t n, bool *ok) {
    size_t i = 0;
    size_t k = 0;
    while (k < n && i < s.size()) {
        char32_t c;
        i += DecodeAt(s, i, &c);
        k++;
    }
    *ok = k == n;
    return i;
}

[[noreturn]] void IndexError() { throw FormatBug("Index out of bounds"); }

}  // namespace

std::string Strip(std::string_view s) {
    std::string r = Rstrip(s);
    size_t i = 0;
    while (i < r.size()) {
        char32_t c;
        const size_t w = DecodeAt(r, i, &c);
        if (!UnicodeWhitespace(c)) break;
        i += w;
    }
    return r.substr(i);
}

std::string Rstrip(std::string_view s) {
    size_t end = s.size();
    while (end > 0) {
        size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0u) == 0x80u) start--;
        char32_t c;
        DecodeAt(s, start, &c);
        if (!UnicodeWhitespace(c)) break;
        end = start;
    }
    return std::string(s.substr(0, end));
}

std::string CharsFrom(std::string_view s, size_t from) {
    bool ok;
    const size_t b = ByteOfChar(s, from, &ok);
    if (!ok) IndexError();
    return std::string(s.substr(b));
}

std::string CharsTo(std::string_view s, size_t n) {
    bool ok;
    return std::string(s.substr(0, ByteOfChar(s, n, &ok)));
}

char32_t CharAt(std::string_view s, size_t i) {
    bool ok;
    const size_t b = ByteOfChar(s, i, &ok);
    if (!ok || b >= s.size()) return 0;
    char32_t c;
    DecodeAt(s, b, &c);
    return c;
}

namespace {

bool StartsWith(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool EndsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

size_t CharCount(std::string_view s) { return crystal::Utf8Length(s); }

// Crystal's String#[](start, count) for a range with an open or given end (characters).
std::string CrSlice(std::string_view s, long start, long end_excl, bool to_end) {
    const long size = static_cast<long>(CharCount(s));
    if (start < 0) start += size;
    if (start < 0 || start > size) IndexError();
    long end = to_end ? size : end_excl;
    if (!to_end && end < 0) end += size;
    end = std::min(end, size);
    const long count = std::max(end - start, 0L);
    bool ok;
    const size_t b = ByteOfChar(s, static_cast<size_t>(start), &ok);
    const size_t e = ByteOfChar(s, static_cast<size_t>(start + count), &ok);
    return std::string(s.substr(b, e - b));
}

template <typename V>
auto &ArrayAt(V &v, long i) {
    const long size = static_cast<long>(v.size());
    if (i < 0) i += size;
    if (i < 0 || i >= size) IndexError();
    return v[static_cast<size_t>(i)];
}

std::vector<std::string> SplitLines(const std::string &s) {  // String#split('\n')
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::vector<std::string> Lines(const std::string &s) {  // String#lines (chomped)
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        size_t end = nl;
        if (end > start && s[end - 1] == '\r') end--;
        out.push_back(s.substr(start, end - start));
        start = nl + 1;
    }
    return out;
}

}  // namespace

// --- construction ----------------------------------------------------------------------

Formatter::Formatter(std::string_view source) : source_(source), lexer_(source_) {
    lexer_.comments_enabled = true;
    lexer_.count_whitespace = true;
    lexer_.wants_raw = true;
    comment_columns_.push_back(kNoColumn);
    tok_ = &lexer_.next_token();
    vars.emplace_back();
    shebang_ = tok_->type == Tok::COMMENT && StartsWith(tok_->value_s(), "#!");
}

std::string Formatter::FormatSource(std::string_view source) {
    crystal::Ast ast;
    crystal::Parser parser(source, ast);
    Node *nodes = parser.parse();
    Formatter formatter(source);
    formatter.skip_space_or_newline();
    formatter.accept(nodes);
    return formatter.finish();
}

Node *Formatter::string_literal(const std::string &value) {
    Node *n = make(NodeKind::StringLiteral);
    n->value = value;
    return n;
}

Node *Formatter::new_call(Node *obj, const std::string &name, std::vector<Node *> args) {
    Node *n = make(NodeKind::Call);
    n->obj = obj;
    n->name = name;
    n->args = std::move(args);
    return n;
}

Node *Formatter::expressions_from(const std::vector<Node *> &exps) {
    if (exps.empty()) return nop();
    if (exps.size() == 1) return exps[0];
    Node *e = make(NodeKind::Expressions);
    e->exps = exps;
    return e;
}

// --- the token stream -------------------------------------------------------------------

Token &Formatter::next_token() {
    const int current_line_number = lexer_.line_number;
    tok_ = &lexer_.next_token();
    if (tok_->type == Tok::DELIMITER_START) {
        increment_lines(lexer_.line_number - current_line_number);
    } else if (tok_->type == Tok::NEWLINE) {
        if (!lexer_.heredocs.empty() && !consuming_heredocs_) {
            write_line();
            consume_heredocs();
        }
    }
    return *tok_;
}

Token &Formatter::next_string_token() {
    const int current_line_number = lexer_.line_number;
    const crystal::DelimiterState ds = tok_->delimiter_state;
    tok_ = &lexer_.next_string_token(ds);
    increment_lines(lexer_.line_number - current_line_number);
    return *tok_;
}

Token &Formatter::next_string_array_token() {
    tok_ = &lexer_.next_string_array_token();
    return *tok_;
}

void Formatter::next_macro_token() {
    const int current_line_number = lexer_.line_number;
    const char32_t ch = lexer_.current_char();
    const crystal::MacroState ms = macro_state_;
    tok_ = &lexer_.next_macro_token(ms, false);
    macro_state_ = tok_->macro_state;
    increment_lines(lexer_.line_number - current_line_number);
    // Unescape
    if (ch == '\\' && !StartsWith(tok_->raw, "\\")) tok_->raw = "\\" + tok_->raw;
}

bool Formatter::skip_space(bool write_comma, bool consume_newline) {
    const int base_column = column_;
    bool has_space = false;

    if (tok_->type == Tok::SPACE) {
        if (tok_->passed_backslash_newline) {
            if (write_comma) {
                write(", ");
            } else {
                write(" ");
            }
            write("\\");
            write_line();
            if (!passed_backslash_newline_) indent_ += 2;
            write_indent();
            next_token();
            passed_backslash_newline_ = true;
            if (tok_->type == Tok::SPACE || tok_->type == Tok::COMMENT) {
                return skip_space(write_comma, consume_newline);
            }
            return false;
        }
        next_token();
        has_space = true;
    }

    if (tok_->type == Tok::COMMENT) {
        const bool needs_space = has_space && base_column != 0;
        if (write_comma) {
            write(", ");
        } else if (needs_space) {
            write(" ");
        }
        write_comment(!needs_space, consume_newline);
        return true;
    }
    return false;
}

bool Formatter::skip_space_at(int indent, bool write_comma) {
    return indent_at(indent, [&] { return skip_space(write_comma); });
}

bool Formatter::skip_space_or_newline(bool last, bool at_least_one, bool next_comes_end) {
    (void)next_comes_end;
    const bool just_wrote_line = wrote_newline_;
    const int base_column = column_;
    bool has_space = false;
    int newlines = 0;
    while (true) {
        if (tok_->type == Tok::SPACE) {
            has_space = true;
            next_token();
        } else if (tok_->type == Tok::NEWLINE) {
            newlines++;
            next_token();
        } else if (tok_->type == Tok::OP_SEMICOLON) {
            next_token();
        } else {
            break;
        }
    }
    if (tok_->type == Tok::COMMENT) {
        const bool needs_space = has_space && newlines == 0 && base_column != 0;
        if (needs_space) {
            write(" ");
        } else if (last && newlines > 0) {
            if (newlines > 1) {
                if (!just_wrote_line && !wrote_newline_) write_line();
                write_line();
            } else if (!wrote_newline_) {
                write_line();
            } else if (at_least_one) {
                write_line();
            }
        }
        write_comment(!needs_space, true, last);
        return true;
    }
    return false;
}

bool Formatter::skip_space_or_newline_at(int indent, bool last, bool at_least_one, bool next_comes_end) {
    return indent_at(indent, [&] { return skip_space_or_newline(last, at_least_one, next_comes_end); });
}

bool Formatter::skip_space_write_line() {
    const bool found_comment = skip_space();
    if (!found_comment && !wrote_newline_) write_line();
    return found_comment;
}

void Formatter::skip_semicolon() {
    while (tok_->type == Tok::OP_SEMICOLON) next_token();
}

bool Formatter::skip_semicolon_or_space() {
    bool found_comment = false;
    while (true) {
        if (tok_->type == Tok::OP_SEMICOLON) {
            next_token();
        } else if (tok_->type == Tok::SPACE) {
            if (!found_comment) found_comment = skip_space();
        } else {
            break;
        }
    }
    return found_comment;
}

void Formatter::skip_semicolon_or_space_or_newline() {
    while (true) {
        if (tok_->type == Tok::OP_SEMICOLON) {
            next_token();
        } else if (tok_->type == Tok::SPACE || tok_->type == Tok::NEWLINE) {
            skip_space_or_newline();
        } else {
            break;
        }
    }
}

void Formatter::jump_semicolon() {
    skip_space();
    skip_semicolon();
    skip_space_or_newline();
}

void Formatter::write_comment(bool needs_indent, bool consume_newline, bool next_comes_end) {
    while (tok_->type == Tok::COMMENT) {
        const bool empty_line = line_output_.empty();
        if (empty_line && needs_indent) write_indent();

        std::string value = Strip(tok_->value_s());
        const std::string raw_after_comment_value = CharsFrom(value, 1);
        const std::string after_comment_value = Strip(raw_after_comment_value);
        if (StartsWith(after_comment_value, "=>")) {
            value = "# => " + Strip(CharsFrom(after_comment_value, 2));
        } else if (std::all_of(after_comment_value.begin(), after_comment_value.end(),
                               [](char c) { return AsciiWhitespace(static_cast<unsigned char>(c)) || c == '#'; })) {
            // Nothing, leave sequences of whitespaces and '#' as is
        } else {
            const char32_t char_1 = CharAt(value, 1);
            if (char_1 && !AsciiWhitespace(char_1)) value = "# " + Strip(CharsFrom(value, 1));
        }

        if (!last_write_.empty() && !AsciiWhitespace(static_cast<unsigned char>(last_write_.back()))) write(" ");

        if (!Strip(line_output_).empty()) comment_columns_.back() = column_;

        if (empty_line) {
            if (StartsWith(after_comment_value, "```")) {
                // Determine code fence language (what comes after "```")
                std::string language = Strip(after_comment_value.substr(3));
                if (current_doc_comment_ && language.empty()) {
                    // A code fence ends when nothing comes after "```"
                    current_doc_comment_->end_line = line_ - 1;
                    if (current_doc_comment_->needs_format) doc_comments_.push_back(*current_doc_comment_);
                    current_doc_comment_.reset();
                } else {
                    // Normalize crystal language tag
                    if (language == "cr" || language == "crystal") {
                        if (EndsWith(value, language)) value.resize(value.size() - language.size());
                        language.clear();
                    }
                    // We only format crystal code (empty by default means crystal)
                    const bool needs_format = language.empty();
                    current_doc_comment_ = CommentInfo{line_ + 1, line_ + 1, true, needs_format};
                }
            }
        }

        wrote_comment_ = true;
        write(value);
        next_token_skip_space();
        if (consume_newline) {
            consume_newlines(next_comes_end);
            skip_space_or_newline();
        }
    }
}

void Formatter::consume_newlines(bool next_comes_end) {
    if (tok_->type == Tok::NEWLINE) {
        if (!wrote_newline_) write_line();
        next_token_skip_space();

        if (tok_->type == Tok::NEWLINE && !next_comes_end) {
            write_line();
            wrote_double_newlines_ = true;
        }

        skip_space_or_newline(next_comes_end, true);
    }
}

void Formatter::indent_node(Node *node) {
    indent2([&] { accept(node); });
}

void Formatter::indent_node(int indent, Node *node) {
    indent_at(indent, [&] { accept(node); });
}

void Formatter::no_indent(Node *node) {
    no_indent_do([&] { accept(node); });
}

void Formatter::write_indent(int indent) {
    for (int i = 0; i < indent; i++) write(" ");
}

void Formatter::write_indent(int indent, Node *node) {
    if (!node->is(NodeKind::Nop)) write_indent(indent);
    indent_node(indent, node);
}

void Formatter::write(std::string_view s) {
    output_ += s;
    line_output_ += s;
    const size_t last_newline = s.rfind('\n');
    if (last_newline != std::string_view::npos) {
        column_ = static_cast<int>(CharCount(s.substr(last_newline + 1)));
    } else {
        column_ += static_cast<int>(CharCount(s));
    }
    wrote_newline_ = false;
    wrote_double_newlines_ = false;
    last_write_ = s;
}

void Formatter::write_line() {
    wrote_double_newlines_ = false;
    if (!wrote_comment_) current_doc_comment_.reset();
    wrote_comment_ = false;

    output_ += '\n';
    line_output_.clear();
    column_ = 0;
    wrote_newline_ = true;
    increment_line();
    last_write_.clear();
    if (passed_backslash_newline_) {
        passed_backslash_newline_ = false;
        indent_ -= 2;
    }
}

void Formatter::increment_line() {
    line_++;
    comment_columns_.push_back(kNoColumn);
}

void Formatter::decrement_line() {
    line_--;
    if (comment_columns_.empty()) IndexError();
    comment_columns_.pop_back();
}

void Formatter::increment_lines(int count) {
    if (count < 0) {
        for (int i = 0; i < -count; i++) decrement_line();
    } else {
        for (int i = 0; i < count; i++) increment_line();
    }
}

// --- finish and the post-passes ----------------------------------------------------------

std::string Formatter::finish() {
    if (paren_count_ > 0) throw FormatBug("BUG: unclosed parenthesis");

    skip_space();
    write_line();
    skip_space_or_newline(true);

    // rstrip instead of string in case this is a subformat
    // (we want to preserve the leading indentation)
    std::string result = Rstrip(output_);

    std::vector<std::string> lines = SplitLines(result);
    fix_heredocs(lines);
    align_infos(lines, when_infos_);
    align_infos(lines, hash_infos_);
    align_infos(lines, assign_infos_);
    align_comments(lines);
    format_doc_comments(lines);
    result.clear();
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) result += '\n';
        if (no_rstrip_lines.count(static_cast<int>(i))) {
            result += lines[i];
        } else {
            result += Rstrip(lines[i]);
        }
    }
    result += '\n';
    if (result == "\n") result.clear();
    if (shebang_) result = CrSlice(result, 0, 1, false) + CrSlice(result, 2, 0, true);
    return result;
}

void Formatter::fix_heredocs(std::vector<std::string> &lines) {
    for (const HeredocFix &fix : heredoc_fixes_) {
        int min_difference = 0;
        bool first = true;
        for (int line_number = fix.start_line; line_number <= fix.end_line; line_number++) {
            const int c = leading_space_count(ArrayAt(lines, line_number), fix.difference);
            if (first || c < min_difference) min_difference = c;
            first = false;
        }
        if (first) throw FormatBug("Empty enumerable");
        const int indent_before_start = leading_space_count(ArrayAt(lines, fix.start_line - 1), fix.difference);
        min_difference = std::max(min_difference - indent_before_start, 0);

        for (int line_number = fix.start_line; line_number <= fix.end_line; line_number++) {
            std::string &line = ArrayAt(lines, line_number);
            line = CrSlice(line, min_difference, 0, true);
        }
    }
}

int Formatter::leading_space_count(const std::string &line, int max_count) {
    for (int index = 0; index < max_count; index++) {
        if (static_cast<size_t>(index) >= line.size() || line[static_cast<size_t>(index)] != ' ') return index;
    }
    return max_count;
}

void Formatter::align_infos(std::vector<std::string> &lines, const std::vector<AlignInfo> &infos) {
    bool has_max = false;
    int max_size = 0;
    const AlignInfo *last_info = nullptr;

    for (size_t i = 0; i < infos.size(); i++) {
        const AlignInfo &info = infos[i];
        if (has_max) {
            align_info(lines, info, max_size);
        } else {
            auto found = find_last_info(infos, &info, i + 1);
            last_info = found.first;
            max_size = found.second;
            has_max = true;
            align_info(lines, info, max_size);
        }

        if (last_info && info.line == last_info->line) {
            last_info = nullptr;
            has_max = false;
        }
    }
}

std::pair<const Formatter::AlignInfo *, int> Formatter::find_last_info(const std::vector<AlignInfo> &infos,
                                                                        const AlignInfo *base, size_t i) {
    int max_size = base->size();
    while (i < infos.size()) {
        const AlignInfo &current = infos[i];
        if (current.id != base->id) break;
        if (base->line + 1 != current.line) break;

        base = &current;
        if (base->size() > 0 && base->size() > max_size) max_size = base->size();
        i++;
    }
    return {base, max_size};
}

void Formatter::align_info(std::vector<std::string> &lines, const AlignInfo &info, int max_size) {
    const int gap = max_size - info.size();
    if (gap == 0) return;

    std::string &line = ArrayAt(lines, info.line);
    const int middle = info.number ? info.start_column : info.middle_column;

    std::string before = CrSlice(line, 0, middle, false);
    std::string after = CrSlice(line, middle, 0, true);
    std::string result = before;
    for (int k = 0; k < gap; k++) result += ' ';
    result += after;
    line = result;

    // Make sure to move the comment in this line too
    if (info.line >= 0 && static_cast<size_t>(info.line) < comment_columns_.size()) {
        int &comment_column = comment_columns_[static_cast<size_t>(info.line)];
        if (comment_column != kNoColumn) comment_column += gap;
    }
}

void Formatter::align_comments(std::vector<std::string> &lines) {
    bool has_max = false;
    int max_column = 0;

    for (size_t i = 0; i < lines.size(); i++) {
        const int comment_column = i < comment_columns_.size() ? comment_columns_[i] : kNoColumn;
        if (comment_column != kNoColumn) {
            if (!has_max) {
                max_column = find_max_column(i + 1, comment_column);
                has_max = true;
            }
            lines[i] = align_comment(lines[i], comment_column, max_column);
        } else {
            has_max = false;
        }
    }
}

int Formatter::find_max_column(size_t base, int max) {
    while (base < comment_columns_.size()) {
        const int comment_column = comment_columns_[base];
        if (comment_column == kNoColumn) break;
        if (comment_column > max) max = comment_column;
        base++;
    }
    return max;
}

std::string Formatter::align_comment(const std::string &line, int comment_column, int max_column) {
    if (comment_column == max_column) return line;

    std::string source_line = CrSlice(line, 0, comment_column, false);
    std::string comment_line = CrSlice(line, comment_column, 0, true);
    const int gap = max_column - comment_column;

    std::string result = source_line;
    for (int k = 0; k < gap; k++) result += ' ';
    result += comment_line;
    return result;
}

void Formatter::format_doc_comments(std::vector<std::string> &lines) {
    for (auto it = doc_comments_.rbegin(); it != doc_comments_.rend(); ++it) {
        const CommentInfo &doc_comment = *it;
        if (doc_comment.start_line > doc_comment.end_line) continue;

        const std::string &first_line = ArrayAt(lines, doc_comment.start_line);
        const size_t sharp_byte = first_line.find('#');
        if (sharp_byte == std::string::npos) throw FormatBug("Nil assertion failed");
        const size_t sharp_index = CharCount(std::string_view(first_line).substr(0, sharp_byte));

        std::string comment;
        for (int i = doc_comment.start_line; i <= doc_comment.end_line; i++) {
            std::string line = CrSlice(Strip(ArrayAt(lines, i)), 1, 0, true);
            if (!line.empty() && line[0] == ' ') line = line.substr(1);
            comment += line;
            comment += '\n';
        }

        try {
            const std::string formatted_comment = FormatSource(comment);
            std::vector<std::string> formatted_lines = Lines(formatted_comment);
            for (std::string &line : formatted_lines) line = std::string(sharp_index, ' ') + "# " + line;
            // lines[start..end] = formatted_lines
            ArrayAt(lines, doc_comment.start_line);
            const auto first = lines.begin() + doc_comment.start_line;
            const auto last = lines.begin() + std::min<long>(doc_comment.end_line + 1, static_cast<long>(lines.size()));
            lines.erase(first, last);
            lines.insert(lines.begin() + doc_comment.start_line, formatted_lines.begin(), formatted_lines.end());
        } catch (const SyntaxError &) {
            // For now we don't care if doc comments have syntax errors,
            // they shouldn't prevent formatting the real code
        }
    }
}

// --- writing keywords and tokens ---------------------------------------------------------

void Formatter::write_keyword(Keyword keyword) {
    check_keyword(keyword);
    write(keyword);
    next_token();
}

void Formatter::write_keyword(const char *before, Keyword keyword) {
    write(before);
    write_keyword(keyword);
}

void Formatter::write_keyword(Keyword keyword, const char *after, bool skip_space_or_newline_) {
    write_keyword(keyword);
    write(after);
    if (skip_space_or_newline_) skip_space_or_newline();
}

void Formatter::write_keyword(const char *before, Keyword keyword, const char *after) {
    const bool passed_backslash_newline = tok_->passed_backslash_newline;
    skip_space();
    if (passed_backslash_newline && std::strcmp(before, " ") == 0) {
        // Nothing
    } else {
        write(before);
    }
    write_keyword(keyword);
    write(after);
    skip_space_or_newline();
}

void Formatter::write_token(Tok type) {
    check(type);
    write(type);
    next_token();
}

void Formatter::write_token(const char *before, Tok type) {
    write(before);
    write_token(type);
}

void Formatter::write_token(Tok type, const char *after) {
    write_token(type);
    write(after);
}

void Formatter::write_token(const char *before, Tok type, const char *after) {
    write(before);
    write_token(type);
    write(after);
}

void Formatter::unexpected(const std::string &expecting) {
    throw FormatBug("expecting " + expecting + ", not `" + crystal::TokName(tok_->type) + ", " + tok_->value_s() +
                    "`, at :" + std::to_string(tok_->line_number) + ":" + std::to_string(tok_->column_number));
}

void Formatter::check_keyword(Keyword k) {
    if (!tok_->keyword(k)) unexpected(std::string("keyword ") + crystal::KeywordName(k));
}

void Formatter::check_keyword(Keyword a, Keyword b) {
    if (!tok_->keyword(a) && !tok_->keyword(b))
        unexpected(std::string("keyword ") + crystal::KeywordName(a) + " or " + crystal::KeywordName(b));
}

void Formatter::check(Tok type) {
    if (tok_->type != type) unexpected(crystal::TokName(type));
}

void Formatter::check(Tok a, Tok b) {
    if (tok_->type != a && tok_->type != b) unexpected(std::string(crystal::TokName(a)) + " or " + crystal::TokName(b));
}

void Formatter::check_end() {
    if (tok_->type == Tok::OP_SEMICOLON) next_token_skip_space_or_newline();
    check_keyword(Keyword::END);
}

bool Formatter::check_macro_whitespace() {
    if (lexer_.current_char() == '\\' && AsciiWhitespace(lexer_.peek_next_char())) {
        lexer_.next_char();
        write("\\");
        write(lexer_.skip_macro_whitespace());
        macro_state_.whitespace = true;
        increment_line();
        return true;
    }
    return false;
}

// --- Format() and the proof-reading --------------------------------------------------------

namespace {

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string NoSpace(std::string_view s) {
    std::string out;
    for (char c : s)
        if (!IsWs(c)) out += c;
    return out;
}

// Parses for the proof-reading: the tree, the comments, or the error.
struct Checked {
    crystal::Ast ast;
    Node *root = nullptr;
    std::vector<std::string> comments;
    std::string error;
    int error_line = 0;
};

std::unique_ptr<Checked> ParseForCheck(std::string_view src, bool want_comments) {
    auto c = std::make_unique<Checked>();
    try {
        crystal::Parser parser(src, c->ast);
        if (want_comments) parser.comment_sink = &c->comments;
        c->root = parser.parse();
    } catch (const SyntaxError &e) {
        c->root = nullptr;
        c->error = e.what();
        c->error_line = e.line;
    }
    return c;
}

bool SameTree(const Node *a, const Node *b);

// The texts the trees being compared were parsed from.
struct CheckSources {
    std::string_view a, b;
};
thread_local const CheckSources *g_sources = nullptr;

// Whether `src` has `what` at `loc` (a line and a column in characters).
bool TextAt(std::string_view src, crystal::Location loc, std::string_view what) {
    if (!loc.known()) return false;
    size_t i = 0;
    for (int line = 1; line < loc.line; line++) {
        i = src.find('\n', i);
        if (i == std::string_view::npos) return false;
        i++;
    }
    for (int col = 1; col < loc.column && i < src.size(); col++) {
        i++;
        while (i < src.size() && (static_cast<unsigned char>(src[i]) & 0xC0u) == 0x80u) i++;
    }
    return src.substr(i, what.size()) == what;
}

// A `__LINE__` (which the parser replaces with its line number) on both sides.
bool BothMagicLine(const Node *a, const Node *b) {
    return g_sources && a->kind == NodeKind::NumberLiteral && b->kind == NodeKind::NumberLiteral &&
           TextAt(g_sources->a, crystal::Loc(a), "__LINE__") && TextAt(g_sources->b, crystal::Loc(b), "__LINE__");
}

// An Expressions that is a single parenthesized expression is that expression
// (the formatter writes `foo (a)` as `foo(a)`).
const Node *Unwrap(const Node *n) {
    while (n && n->kind == NodeKind::Expressions && n->keyword == crystal::ExpressionsKeyword::Paren && n->exps.size() == 1 &&
           !n->exps[0]->is(NodeKind::Nop))
        n = n->exps[0];
    return n;
}

// The children of an Expressions with runs of macro literals joined.
struct Piece {
    const Node *node = nullptr;
    std::string literal;  // when node is null
};

std::vector<Piece> Pieces(const std::vector<Node *> &exps, NodeKind joined = NodeKind::MacroLiteral) {
    std::vector<Piece> out;
    for (const Node *e : exps) {
        if (e->kind == joined) {
            if (out.empty() || out.back().node) out.push_back(Piece{});
            out.back().literal += e->value;
        } else {
            out.push_back(Piece{e, {}});
        }
    }
    return out;
}

// Two runs of macro text: the same but for whitespace, or both Crystal code that parses to the same tree.
bool SameMacroText(const std::string &a, const std::string &b) {
    if (NoSpace(a) == NoSpace(b)) return true;
    auto pa = ParseForCheck(a, false);
    auto pb = ParseForCheck(b, false);
    if (!pa->root || !pb->root) return false;
    const CheckSources sources{a, b};
    const CheckSources *outer = g_sources;
    g_sources = &sources;
    const bool same = SameTree(pa->root, pb->root);
    g_sources = outer;
    return same;
}

// A string's pieces with runs of literal text joined (the formatter writes `\;` as `;`, one piece fewer).
bool SameStringPieces(const std::vector<Node *> &a, const std::vector<Node *> &b) {
    const std::vector<Piece> pa = Pieces(a, NodeKind::StringLiteral), pb = Pieces(b, NodeKind::StringLiteral);
    if (pa.size() != pb.size()) return false;
    for (size_t i = 0; i < pa.size(); i++) {
        if (!pa[i].node != !pb[i].node) return false;
        if (pa[i].node ? !SameTree(pa[i].node, pb[i].node) : pa[i].literal != pb[i].literal) return false;
    }
    return true;
}

bool SameList(const std::vector<Node *> &a, const std::vector<Node *> &b) {
    const std::vector<Piece> pa = Pieces(a), pb = Pieces(b);
    if (pa.size() != pb.size()) return false;
    for (size_t i = 0; i < pa.size(); i++) {
        if (!pa[i].node != !pb[i].node) return false;
        if (pa[i].node) {
            if (!SameTree(pa[i].node, pb[i].node)) return false;
        } else if (!SameMacroText(pa[i].literal, pb[i].literal)) {
            return false;
        }
    }
    return true;
}

// `def foo(&)` is how the formatter writes a yielding `def foo`.
bool AnonymousBlockArg(const Node *def, const Node *arg) {
    return def->kind == NodeKind::Def && def->block_arity >= 0 &&
           (!arg || (arg->name.empty() && !arg->restriction && arg->annotations.empty()));
}

bool SameTree(const Node *a, const Node *b) {
    a = Unwrap(a);
    b = Unwrap(b);
    if (!a || !b) return !a && !b;
    // `- 1` (a call of unary minus) is written `-1` (a negative literal).
    auto signed_literal = [](const Node *call, const Node *lit) {
        return call->kind == NodeKind::Call && (call->name == "-" || call->name == "+") && call->obj &&
               call->obj->kind == NodeKind::NumberLiteral && call->args.empty() && !call->block &&
               !call->block_arg && !call->has_named_args && lit->kind == NodeKind::NumberLiteral &&
               lit->number_kind == call->obj->number_kind && lit->value == call->name + call->obj->value;
    };
    if (signed_literal(a, b) || signed_literal(b, a)) return true;
    if (BothMagicLine(a, b)) return true;
    if (a->kind != b->kind) {
        // A lone macro literal against an Expressions of them (or the reverse).
        auto lit = [](const Node *n, std::string *out) {
            if (n->kind == NodeKind::MacroLiteral) {
                *out = n->value;
                return true;
            }
            if (n->kind != NodeKind::Expressions) return false;
            for (const Node *e : n->exps) {
                if (e->kind != NodeKind::MacroLiteral) return false;
                *out += e->value;
            }
            return true;
        };
        std::string ta, tb;
        return lit(a, &ta) && lit(b, &tb) && SameMacroText(ta, tb);
    }
    if (a->kind == NodeKind::MacroLiteral) return SameMacroText(a->value, b->value);
    if (a->name != b->name || a->value != b->value || a->external_name != b->external_name || a->ch != b->ch ||
        a->number_kind != b->number_kind || a->bool_value != b->bool_value || a->regex_options != b->regex_options ||
        a->magic != b->magic || a->visibility != b->visibility || a->modifier != b->modifier ||
        a->suffix != b->suffix || a->splat_index != b->splat_index || a->block_arity != b->block_arity)
        return false;
    if (a->global != b->global || a->exclusive != b->exclusive || a->ternary != b->ternary ||
        a->is_abstract != b->is_abstract || a->is_struct != b->is_struct || a->nil_check != b->nil_check ||
        a->exhaustive != b->exhaustive || a->args_in_brackets != b->args_in_brackets || a->varargs != b->varargs ||
        a->output != b->output || a->is_unless != b->is_unless || a->implicit != b->implicit ||
        a->suffix_ != b->suffix_ || a->is_union != b->is_union || a->volatile_ != b->volatile_ ||
        a->alignstack != b->alignstack || a->intel != b->intel || a->can_throw != b->can_throw ||
        a->has_exps != b->has_exps || a->has_exps2 != b->has_exps2 || a->has_names != b->has_names ||
        a->has_name != b->has_name || a->has_real_name != b->has_real_name || a->has_of != b->has_of ||
        a->names != b->names)
        return false;
    if (a->kind == NodeKind::Expressions && a->keyword != b->keyword) return false;
    if (a->kind == NodeKind::StringInterpolation && !SameStringPieces(a->exps, b->exps)) return false;
    const Node *ca[] = {a->obj, a->exp, a->cond, a->then_, a->else_, a->body, a->target, a->value_node, a->left,
                        a->right, a->block, a->receiver, a->return_type, a->restriction, a->default_value,
                        a->superclass, a->of, a->name_node, a->scope, a->ensure_, a->double_splat, a->def_, a->var,
                        a->declared_type, a->of_key, a->of_value};
    const Node *cb[] = {b->obj, b->exp, b->cond, b->then_, b->else_, b->body, b->target, b->value_node, b->left,
                        b->right, b->block, b->receiver, b->return_type, b->restriction, b->default_value,
                        b->superclass, b->of, b->name_node, b->scope, b->ensure_, b->double_splat, b->def_, b->var,
                        b->declared_type, b->of_key, b->of_value};
    for (size_t i = 0; i < sizeof(ca) / sizeof(ca[0]); i++)
        if (!SameTree(ca[i], cb[i])) return false;
    if (!(AnonymousBlockArg(a, a->block_arg) && AnonymousBlockArg(b, b->block_arg)) &&
        !SameTree(a->block_arg, b->block_arg))
        return false;
    if ((a->kind != NodeKind::StringInterpolation && !SameList(a->exps, b->exps)) || !SameList(a->exps2, b->exps2) || !SameList(a->args, b->args) ||
        !SameList(a->named_args, b->named_args) || !SameList(a->annotations, b->annotations))
        return false;
    if (a->entries.size() != b->entries.size() || a->named_entries.size() != b->named_entries.size() ||
        a->unpacks.size() != b->unpacks.size())
        return false;
    for (size_t i = 0; i < a->entries.size(); i++)
        if (!SameTree(a->entries[i].first, b->entries[i].first) || !SameTree(a->entries[i].second, b->entries[i].second))
            return false;
    for (size_t i = 0; i < a->named_entries.size(); i++)
        if (a->named_entries[i].first != b->named_entries[i].first ||
            !SameTree(a->named_entries[i].second, b->named_entries[i].second))
            return false;
    for (auto ia = a->unpacks.begin(), ib = b->unpacks.begin(); ia != a->unpacks.end(); ++ia, ++ib)
        if (ia->first != ib->first || !SameTree(ia->second, ib->second)) return false;
    return true;
}

// The comments' text to compare: all of it without whitespace or `#`
// (the formatter respaces a comment, and may turn a `#` line into a blank
// one), except that what is between ``` fences -- code the formatter
// formats -- only counts as a fence.
std::string CommentText(const std::vector<std::string> &comments) {
    std::string out;
    bool in_fence = false;
    for (const std::string &c : comments) {
        std::string t;
        for (char ch : c)
            if (!IsWs(ch) && ch != '#') t += ch;
        const bool fence = t.compare(0, 3, "```") == 0;
        if (fence) {
            in_fence = !in_fence;
            out += "```";
        } else if (!in_fence) {
            out += t;
        }
    }
    return out;
}

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    Result r;
    r.text = std::string(src);
    std::string out;
    try {
        out = Formatter::FormatSource(src);
    } catch (const SyntaxError &e) {
        r.error = std::string("syntax error: ") + e.what();
        r.error_line = e.line;
        return r;
    } catch (const std::exception &e) {
        r.error = std::string("couldn't format: ") + e.what();
        return r;
    }
    if (opts.verify && out != src) {
        auto before = ParseForCheck(src, true);
        auto after = ParseForCheck(out, true);
        if (!before->root) {
            r.error = "syntax error: " + before->error;
            r.error_line = before->error_line;
            return r;
        }
        if (!after->root) {
            r.error = "the formatted text does not parse (" + after->error + " at line " +
                      std::to_string(after->error_line) + ")";
            return r;
        }
        const CheckSources sources{src, out};
        g_sources = &sources;
        const bool same = SameTree(before->root, after->root);
        g_sources = nullptr;
        if (!same) {
            r.error = "the formatted text does not parse to the same program";
            return r;
        }
        if (CommentText(before->comments) != CommentText(after->comments)) {
            r.error = "the formatted text does not have the same comments";
            return r;
        }
    }
    r.ok = true;
    r.text = std::move(out);
    return r;
}

}  // namespace crfmt
