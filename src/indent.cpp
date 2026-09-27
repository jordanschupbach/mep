#include "indent.h"

#include <cstring>
#include <vector>

namespace mepindent {
namespace {

bool IsPython(const std::string &filetype) {
    return filetype == "py" || filetype == "pyi";
}

// Columns per indent level, as a number -- kShift is the same thing spelled as
// the string the callers splice in.
const int kShiftWidth = static_cast<int>(std::strlen(kShift));

// Whether `line` is blank: empty, or nothing but spaces/tabs.
bool IsBlank(const std::string &line) {
    return line.find_first_not_of(" \t") == std::string::npos;
}

// `width` columns of indent, as tabs (plus a remainder of spaces) when the
// surrounding file indents with tabs, and as spaces otherwise.
std::string MakeIndent(int width, bool use_tabs) {
    if (width <= 0) return "";
    if (!use_tabs) return std::string(static_cast<size_t>(width), ' ');
    return std::string(static_cast<size_t>(width / kShiftWidth), '\t') +
           std::string(static_cast<size_t>(width % kShiftWidth), ' ');
}

// The lines of `text`, split on '\n', with the empty segment a trailing newline
// leaves dropped (that newline is remembered by the caller and re-appended).
std::vector<std::string> SplitLines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
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

// One indent level deeper than `ws`, in whatever the file indents with, so a
// tab-indented file does not gain a four-space level. The mirror of DedentOne.
// `like` is a second sample of the file's indentation (the line being
// re-indented), consulted when `ws` is empty and so has nothing to say -- which
// is exactly the common case here, a `match` at column 0.
std::string IndentOne(const std::string &ws, const std::string &like) {
    const std::string &hint = ws.empty() ? like : ws;
    if (!hint.empty() && hint.find(' ') == std::string::npos) return ws + '\t';
    return ws + kShift;
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
    return word == "else" || word == "elif" || word == "except" || word == "finally" ||
           word == "case";
}

// Whether a statement starting with `opener` can own a `clause` -- Python's own
// pairing rules, including the ones easy to forget: `for`/`while` take an
// `else`, so does `try` (after its last `except`), and a `finally` can follow
// that `else` in turn.
bool OpensClause(const std::string &clause, const std::string &opener) {
    if (clause == "else") {
        return opener == "if" || opener == "elif" || opener == "for" || opener == "while" ||
               opener == "try" || opener == "except";
    }
    if (clause == "elif") return opener == "if" || opener == "elif";
    if (clause == "except") return opener == "try" || opener == "except";
    if (clause == "finally") return opener == "try" || opener == "except" || opener == "else";
    // A `case` belongs to the `case` above it (same column) or, when it is the
    // first one, to the `match` itself (one column further in) -- see the
    // IndentOne branch in ReindentDedentKeyword.
    if (clause == "case") return opener == "case" || opener == "match";
    return false;
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

std::optional<std::string> ReindentDedentKeyword(const std::vector<std::string> &lines, int row,
                                                 const std::string &filetype) {
    if (!IsPython(filetype)) return std::nullopt;
    if (row < 0 || row >= static_cast<int>(lines.size())) return std::nullopt;
    const std::string &current_line = lines[static_cast<size_t>(row)];
    const std::string clause = FirstWord(current_line);
    if (!IsDedentClause(clause)) return std::nullopt;
    const std::string indent = LeadingWhitespace(current_line);
    if (indent.empty()) return std::nullopt;  // already at column 0, nothing to do
    const int cur = IndentWidth(indent);

    // The statement this clause closes: the nearest line above that opens a
    // block it can attach to, at this clause's own column or further out.
    for (int j = row - 1; j >= 0; j--) {
        const std::string &above = lines[static_cast<size_t>(j)];
        if (IsBlank(above)) continue;
        const std::string ws = LeadingWhitespace(above);
        if (above[ws.size()] == '#') continue;  // a comment decides nothing
        // Deeper than the clause: that is the body being closed, not an opener
        // for it -- an `if` nested inside the block cannot own this `else`.
        if (IndentWidth(ws) > cur) continue;
        const std::string opener = FirstWord(above);
        if (!OpensClause(clause, opener)) continue;
        // Every clause takes its opener's own column -- except the first `case`
        // of a match statement, which sits one level *inside* its `match` (a
        // later `case` aligns with the `case` above it, which the same rule
        // gives for free).
        const std::string target =
            (clause == "case" && opener == "match") ? IndentOne(ws, indent) : ws;
        if (target == indent) return std::nullopt;         // already where it belongs
        if (IndentWidth(target) > cur) return std::nullopt;  // only ever dedents
        return target;
    }
    // `case` is a soft keyword, so a line starting with the word `case` and
    // containing a ':' is only a clause if a match statement is actually open
    // above it -- `case = {1: 2}` anywhere else is an ordinary assignment, and
    // the fallback below would dedent it for no reason.
    if (clause == "case") return std::nullopt;
    // Nothing above owns it (a clause being typed into an unfinished buffer):
    // one level out, which is where it would most often have belonged anyway.
    const std::string dedented = DedentOne(indent);
    if (dedented == indent) return std::nullopt;
    return dedented;
}

std::string LeadingWhitespace(const std::string &line) {
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) n++;
    return line.substr(0, n);
}

int IndentWidth(const std::string &whitespace) {
    int w = 0;
    for (char c : whitespace) w = (c == '\t') ? (w / kShiftWidth + 1) * kShiftWidth : w + 1;
    return w;
}

std::string ReindentPastedText(const std::string &text, const std::string &target_indent,
                               bool indent_first_line) {
    if (text.find('\n') == std::string::npos) return text;  // one line: no shape to keep
    const bool trailing_newline = text.back() == '\n';
    std::vector<std::string> lines = SplitLines(text);
    if (lines.empty()) return text;

    // The baseline the whole block is measured against: the first non-blank
    // line's indent. A block of nothing but blank lines has no indentation to
    // re-align, so it goes in as it came.
    int base = -1;
    for (const std::string &line : lines) {
        if (!IsBlank(line)) {
            base = IndentWidth(LeadingWhitespace(line));
            break;
        }
    }
    if (base < 0) return text;

    // Match the surrounding file's indent character rather than mixing: tabs
    // only if that is all the target indent is made of.
    const bool use_tabs = !target_indent.empty() && target_indent.find(' ') == std::string::npos;

    std::string out;
    out.reserve(text.size() + lines.size() * target_indent.size());
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) out += '\n';
        const std::string &line = lines[i];
        const bool last = i + 1 == lines.size();
        // A blank line goes in empty -- no point trailing the target indent
        // across it. The one exception is a trailing partial line (the copied
        // text ended mid-line, in whitespace): that whitespace is where the
        // cursor lands and typing continues, so it keeps its indent.
        if (IsBlank(line) && !(last && !trailing_newline && !line.empty())) continue;
        const std::string ws = LeadingWhitespace(line);
        const int rel = IndentWidth(ws) - base;
        const std::string indent = MakeIndent(rel > 0 ? rel : 0, use_tabs);
        // The first line of a charwise/Insert-mode paste is spliced onto text
        // that is already on the line, which is what puts it at the target
        // indent -- so it is stripped but not re-prefixed.
        if (i > 0 || indent_first_line) out += target_indent;
        out += indent;
        out += line.substr(ws.size());
    }
    if (trailing_newline) out += '\n';
    return out;
}

int PasteSpliceCol(const std::string &line, int col) {
    if (col < 0) return 0;
    const int indent = static_cast<int>(LeadingWhitespace(line).size());
    return col < indent ? indent : col;
}

int IndentBackspaceWidth(const std::string &line, int col) {
    if (col <= 0 || col > static_cast<int>(line.size())) return 0;
    // Only inside the leading whitespace: a space between words is not indent.
    for (int i = 0; i < col; i++) {
        if (line[static_cast<size_t>(i)] != ' ' && line[static_cast<size_t>(i)] != '\t') return 0;
    }
    if (line[static_cast<size_t>(col) - 1] != ' ') return 0;  // a literal tab is one press already
    const int vcol = IndentWidth(line.substr(0, static_cast<size_t>(col)));
    const int stop = ((vcol - 1) / kShiftWidth) * kShiftWidth;  // previous tab stop, strictly left
    int n = 0;
    while (col - n > 0 && line[static_cast<size_t>(col - n - 1)] == ' ' && vcol - n > stop) n++;
    return n;
}

int IndentDeleteWidth(const std::string &line, int col) {
    if (col < 0 || col >= static_cast<int>(line.size())) return 0;
    if (line[static_cast<size_t>(col)] != ' ') return 0;
    for (int i = 0; i < col; i++) {
        if (line[static_cast<size_t>(i)] != ' ' && line[static_cast<size_t>(i)] != '\t') return 0;
    }
    const int vcol = IndentWidth(line.substr(0, static_cast<size_t>(col)));
    const int stop = (vcol / kShiftWidth + 1) * kShiftWidth;  // next tab stop, strictly right
    int n = 0;
    while (col + n < static_cast<int>(line.size()) && line[static_cast<size_t>(col + n)] == ' ' &&
           vcol + n < stop)
        n++;
    return n;
}

}  // namespace mepindent
