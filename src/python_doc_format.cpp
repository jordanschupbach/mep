#include "python_doc_format.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

namespace {
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
std::string Trim(std::string_view s) {
    while (!s.empty() && Space(s.front())) s.remove_prefix(1);
    while (!s.empty() && Space(s.back())) s.remove_suffix(1);
    return std::string(s);
}
size_t Indent(const std::string &s) { return s.find_first_not_of(' ') == std::string::npos ? s.size() : s.find_first_not_of(' '); }
bool Starts(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
// Count UTF-8 codepoints rather than bytes, and never cut a multibyte glyph.
size_t Columns(std::string_view s) {
    size_t n = 0;
    for (char c : s) if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) n++;
    return n;
}
size_t BytesForColumns(std::string_view s, size_t columns) {
    size_t i = 0, count = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80 && count++ == columns) break;
        i++;
    }
    return i;
}
std::vector<std::string> Lines(const std::string &text) {
    std::vector<std::string> out;
    std::string line;
    size_t col = 0;
    for (char c : text) {
        if (c == '\n') { out.push_back(line); line.clear(); col = 0; }
        else if (c == '\t') { const size_t n = 8 - col % 8; line.append(n, ' '); col += n; }
        else if (c != '\r') { line += c; if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) col++; }
    }
    out.push_back(line);
    return out;
}

bool Rule(const std::string &line) {
    const std::string t = Trim(line);
    return t.size() >= 3 && (t[0] == '-' || t[0] == '=' || t[0] == '~') && t.find_first_not_of(t[0]) == std::string::npos;
}
std::string Fence(const std::string &line) {
    const std::string t = Trim(line);
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~')) return {};
    const size_t end = t.find_first_not_of(t[0]);
    const size_t n = end == std::string::npos ? t.size() : end;
    return n >= 3 ? t.substr(0, n) : std::string();
}
bool Table(const std::string &line) {
    const std::string t = Trim(line);
    return (!t.empty() && t[0] == '|') || (Starts(t, "+-") && t.back() == '+');
}
bool Heading(const std::vector<std::string> &lines, size_t row) {
    const std::string t = Trim(lines[row]);
    if (t.empty()) return false;
    if (row + 1 < lines.size() && Rule(lines[row + 1])) return true;
    if (t[0] == '#') return true;
    // Google sections, and reST admonitions/directives.
    return (Indent(lines[row]) == 0 && t.back() == ':') || Starts(t, ".. ");
}
// The complete list prefix, including its indentation and following space.
size_t ListPrefix(const std::string &line) {
    size_t at = Indent(line), end = at;
    if (at == line.size()) return 0;
    if (line[at] == '-' || line[at] == '*' || line[at] == '+') end++;
    else if (line.compare(at, 3, "\xe2\x80\xa2") == 0) end += 3;  // bullet
    else {
        if (line[end] == '(') end++;
        const size_t first = end;
        while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])) != 0) end++;
        if (end == first && end < line.size() && (std::isalpha(static_cast<unsigned char>(line[end])) != 0 || line[end] == '#')) end++;
        if (end == first || end >= line.size() || (line[end] != '.' && line[end] != ')')) return 0;
        end++;
    }
    if (end >= line.size() || line[end] != ' ') return 0;
    while (end < line.size() && line[end] == ' ') end++;
    if (line.compare(end, 4, "[ ] ") == 0 || line.compare(end, 4, "[x] ") == 0 || line.compare(end, 4, "[X] ") == 0) end += 4;
    return end;
}
// reST fields and Google argument entries; NumPy's "x : type" remains
// a definition heading followed by its separately indented description.
size_t FieldPrefix(const std::string &line) {
    const size_t indent = Indent(line);
    if (indent == line.size()) return 0;
    size_t colon = line.find(':', indent + (line[indent] == ':' ? 1 : 0));
    if (colon == std::string::npos || colon + 1 == line.size()) return 0;
    if (line[indent] != ':' && (indent == 0 || (colon > 0 && line[colon - 1] == ' '))) return 0;
    if (line[colon + 1] != ' ') return 0;
    return line.find_first_not_of(' ', colon + 1);
}
bool Definition(const std::string &line) {
    const auto colon = line.find(" : ");
    if (colon == std::string::npos) return false;
    const std::string name = Trim(std::string_view(line).substr(0, colon));
    return !name.empty() && (name.find(' ') == std::string::npos || name.find(',') != std::string::npos) &&
           name.find_first_of("?!") == std::string::npos;
}
bool CodeStart(const std::string &line) {
    const auto t = Trim(line);
    const auto open = t.find('(');
    if (open != std::string::npos && open > 0 && t.substr(0, open).find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") == std::string::npos) return true;
    return Starts(t, ">>>") || Starts(t, "... ") || Starts(t, "import ") || Starts(t, "from ") ||
           Starts(t, "def ") || Starts(t, "class ") || Starts(t, "# ") || t.find(" = ") != std::string::npos ||
           ((Starts(t, "if ") || Starts(t, "for ") || Starts(t, "with ") || Starts(t, "try:")) && !t.empty() && t.back() == ':');
}

void Wrap(const std::string &body, const std::string &first, const std::string &rest, size_t width,
          std::vector<std::string> &out) {
    std::string line = first;
    size_t used = Columns(line);
    bool content = false;
    for (size_t at = 0; at < body.size();) {
        while (at < body.size() && Space(body[at])) at++;
        if (at == body.size()) break;
        size_t end = at;
        while (end < body.size() && !Space(body[end])) end++;
        std::string_view word(body.data() + at, end - at);
        if (content && used + 1 + Columns(word) > width) { out.push_back(line); line = rest; used = Columns(rest); content = false; }
        if (content) { line += ' '; used++; }
        while (used + Columns(word) > width && !word.empty()) {
            const size_t take = BytesForColumns(word, std::max<size_t>(1, width > used ? width - used : 1));
            line.append(word.substr(0, take));
            word.remove_prefix(take);
            out.push_back(line);
            line = rest;
            used = Columns(rest);
        }
        line.append(word);
        used += Columns(word);
        content = !word.empty();
        at = end;
    }
    if (content || !first.empty()) out.push_back(line);
}

// Complete declaration headers are prose-adjacent code. Split at argument
// commas only (respecting nested defaults and quoted commas), never in a name.
bool Signature(const std::vector<std::string> &lines, size_t &row, size_t width, std::vector<std::string> &out) {
    if (row != 0) return false;
    const std::string &line = lines[row];
    const size_t open = line.find('(');
    if (open == std::string::npos) return false;
    const std::string name = line.substr(0, open);
    std::string ident = name;
    if (Starts(ident, "async def ")) ident.erase(0, 10);
    else if (Starts(ident, "def ")) ident.erase(0, 4);
    else if (Starts(ident, "class ")) ident.erase(0, 6);
    if (ident.empty() || ident.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") != std::string::npos) return false;
    size_t end = row;
    while (end + 1 < lines.size() && !Trim(lines[end + 1]).empty()) end++;
    bool fits = true;
    for (size_t r = row; r <= end; r++) if (Columns(lines[r]) > width) fits = false;
    if (fits) { for (; row <= end; row++) out.push_back(lines[row]); return true; }
    std::string source;
    for (size_t r = row; r <= end; r++) source += (r == row ? "" : " ") + Trim(lines[r]);
    size_t start = open + 1, close = std::string::npos;
    int depth = 1;
    char quote = 0;
    bool escape = false;
    std::vector<std::string> params;
    for (size_t i = start; i < source.size(); i++) {
        const char c = source[i];
        if (quote != 0) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') quote = c;
        else if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (--depth == 0) { close = i; break; } }
        else if (c == ',' && depth == 1) { params.push_back(Trim(std::string_view(source).substr(start, i - start)) + ','); start = i + 1; }
    }
    if (close == std::string::npos) return false;
    const std::string last = Trim(std::string_view(source).substr(start, close - start));
    if (!last.empty()) params.push_back(last);
    out.push_back(source.substr(0, open + 1));
    for (const auto &param : params) out.push_back("    " + param);
    out.push_back(source.substr(close));
    row = end + 1;
    return true;
}
}  // namespace

std::string FormatPythonDoc(const std::string &text, int columns) {
    const size_t width = static_cast<size_t>(std::clamp(columns, 8, 240));
    auto lines = Lines(text);
    std::vector<std::string> out;
    std::string fence;
    bool doctest = false, examples = false, example_code = false;
    int literal_indent = -1, literal_pending = -1;
    for (size_t row = 0; row < lines.size();) {
        const auto &line = lines[row];
        const std::string t = Trim(line);
        const size_t indent = Indent(line);
        const auto marker = Fence(line);
        if (!fence.empty()) {
            out.push_back(line);
            if (marker.size() >= fence.size() && marker[0] == fence[0] && Trim(line) == marker) fence.clear();
            row++; continue;
        }
        if (!marker.empty()) { fence = marker; out.push_back(line); row++; continue; }
        if (t.empty()) {
            if (literal_indent >= 0 || (!out.empty() && !out.back().empty())) out.emplace_back();
            doctest = false;
            example_code = false;
            row++; continue;
        }
        if (literal_pending >= 0) {
            if (indent > static_cast<size_t>(literal_pending)) literal_indent = literal_pending;
            literal_pending = -1;
        }
        if (literal_indent >= 0) {
            if (indent > static_cast<size_t>(literal_indent)) { out.push_back(line); row++; continue; }
            literal_indent = -1;
        }
        if (Starts(t, ">>>")) doctest = true;
        if (example_code && Heading(lines, row)) example_code = false;
        if (examples && CodeStart(line)) example_code = true;
        if (doctest || example_code || Table(line)) {
            out.push_back(line);
            if (examples && indent > 0) literal_indent = static_cast<int>(indent) - 1;
            row++; continue;
        }
        if (Signature(lines, row, width, out)) continue;
        if (Heading(lines, row) || Rule(line)) {
            out.push_back(line);
            if (!Rule(line)) {
                const auto title = t.substr(0, t.find(':'));
                examples = title == "Examples" || title == "Example" || title == "Usage" || title == "# Examples";
            }
            if ((t.size() >= 2 && t.substr(t.size() - 2) == "::") || Starts(t, ".. code-block::") ||
                Starts(t, ".. code::") || Starts(t, ".. sourcecode::") || Starts(t, ".. math::"))
                literal_pending = static_cast<int>(indent);
            row++; continue;
        }
        if (Definition(line)) { out.push_back(line); row++; continue; }
        // Markdown blockquotes repeat their marker on every continuation.
        if (Starts(t, "> ")) {
            const std::string quote = line.substr(0, indent) + "> ";
            std::string paragraph = Trim(std::string_view(t).substr(2));
            row++;
            while (row < lines.size() && Starts(lines[row], quote)) {
                paragraph += ' ' + Trim(std::string_view(lines[row]).substr(quote.size()));
                row++;
            }
            Wrap(paragraph, quote, quote, width, out);
            continue;
        }
        const size_t list = ListPrefix(line);
        const size_t field = FieldPrefix(line);
        const size_t prefix = list != 0 ? list : field != 0 && field != std::string::npos ? field : indent;
        const size_t base = std::min(indent, width / 2);
        const std::string first = line.substr(0, prefix);
        const std::string rest(list != 0 ? std::min(Columns(first), width / 2) : field != 0 ? std::min(base + 4, width / 2) : base, ' ');
        std::string paragraph = line.substr(prefix);
        if (t.size() >= 2 && t.substr(t.size() - 2) == "::") literal_pending = static_cast<int>(indent);
        row++;
        while (literal_pending < 0 && row < lines.size() && !Trim(lines[row]).empty()) {
            const auto &next = lines[row];
            const size_t next_indent = Indent(next);
            if (Heading(lines, row) || Rule(next) || Table(next) || !Fence(next).empty() || ListPrefix(next) != 0 ||
                FieldPrefix(next) != 0 || Definition(next) || Starts(Trim(next), ">>>")) break;
            if ((list == 0 && field == 0 && next_indent != indent) || next_indent < indent) break;
            paragraph += ' ' + Trim(next);
            row++;
        }
        Wrap(paragraph, first, rest, width, out);
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    std::string result;
    for (const auto &line : out) { if (!result.empty()) result += '\n'; result += line; }
    return result;
}

int PythonDocColumns(int requested, int screen_width, float char_width) {
    const int limit = std::clamp(requested, 8, 240);
    if (screen_width <= 0 || char_width <= 0.0f) return limit;
    return std::min(limit, std::max(8, static_cast<int>(static_cast<float>(screen_width - 60) / char_width)));
}
