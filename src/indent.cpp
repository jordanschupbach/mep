#include "indent.h"

#include <cstring>

namespace mepindent {
namespace {

bool IsPython(const std::string &filetype) {
    return filetype == "py" || filetype == "pyi";
}

// The leading run of spaces/tabs of `line`.
std::string LeadingWhitespace(const std::string &line) {
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) n++;
    return line.substr(0, n);
}

// One indent level shallower than `ws`: drop a trailing 4-space shift, else a
// trailing tab, else fall to column 0. Never returns something longer than the
// input, so it can only ever dedent.
std::string DedentOne(const std::string &ws) {
    const size_t shift = std::strlen(kShift);
    if (ws.size() >= shift && ws.compare(ws.size() - shift, shift, kShift) == 0)
        return ws.substr(0, ws.size() - shift);
    if (!ws.empty() && ws.back() == '\t') return ws.substr(0, ws.size() - 1);
    return "";
}

// The last significant character of `line` -- ignoring trailing whitespace and
// a trailing `#` comment -- or '\0' if there is none. Tracks single/double
// quoted strings (with backslash escapes) so a '#' or ':' inside a string is
// not mistaken for a comment start or a block opener. Triple-quoted strings
// spanning lines are not tracked (a single line can't tell it is inside one);
// the worst case is a one-level indent guess the caller can fix.
char LastCodeChar(const std::string &line) {
    char last = '\0';
    char quote = '\0';
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (quote != '\0') {
            if (c == '\\') {
                i++;  // skip the escaped char
                continue;
            }
            if (c == quote) quote = '\0';
            last = c;
            continue;
        }
        if (c == '#') break;  // start of comment -- rest of the line is not code
        if (c == '\'' || c == '"') {
            quote = c;
            last = c;
            continue;
        }
        if (c != ' ' && c != '\t') last = c;
    }
    return last;
}

// The leading run of identifier letters of the trimmed line -- its first "word".
// Only ASCII letters, which is all the keywords we test against need.
std::string FirstWord(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t start = i;
    while (i < line.size() &&
           ((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z')))
        i++;
    // A real word boundary: the char after the letters must not continue an
    // identifier, so `returns`/`elsewhere` don't read as `return`/`else`.
    if (i < line.size() && (line[i] == '_' || (line[i] >= '0' && line[i] <= '9')))
        return "";
    return line.substr(start, i - start);
}

bool IsFlowKeyword(const std::string &word) {
    return word == "return" || word == "pass" || word == "break" ||
           word == "continue" || word == "raise";
}

bool IsDedentClause(const std::string &word) {
    return word == "else" || word == "elif" || word == "except" || word == "finally";
}

}  // namespace

std::string ComputeNewlineIndent(const std::string &line_before_cursor,
                                 const std::string &filetype) {
    std::string indent = LeadingWhitespace(line_before_cursor);
    if (!IsPython(filetype)) return indent;

    if (LastCodeChar(line_before_cursor) == ':') return indent + kShift;
    if (IsFlowKeyword(FirstWord(line_before_cursor))) return DedentOne(indent);
    return indent;
}

std::optional<std::string> ReindentDedentKeyword(const std::string &current_line,
                                                 const std::string &filetype) {
    if (!IsPython(filetype)) return std::nullopt;
    if (!IsDedentClause(FirstWord(current_line))) return std::nullopt;
    std::string indent = LeadingWhitespace(current_line);
    if (indent.empty()) return std::nullopt;  // already at column 0, nothing to do
    std::string dedented = DedentOne(indent);
    if (dedented == indent) return std::nullopt;
    return dedented;
}

}  // namespace mepindent
