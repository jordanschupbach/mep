// The Common Lisp reader: the standard readtable (CLHS 2.4) applied to
// source text, producing lisp_reader.h's syntax tree. See that header for
// the contract and the scope notes.

#include "lisp_reader.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

bool LispRangeContains(const LispRange &r, int line, int col) {
    if (line < r.line || line > r.end_line) return false;
    if (line == r.line && col < r.col) return false;
    if (line == r.end_line && col >= r.end_col) return false;
    return true;
}

const LispNode &LispReadResult::at(int index) const {
    static const LispNode kEmpty;
    if (index < 0 || static_cast<size_t>(index) >= nodes.size()) return kEmpty;
    return nodes[static_cast<size_t>(index)];
}

bool LispIsUnqualifiedOrCl(const LispNode &node) {
    return node.package.empty() || node.package == "CL" || node.package == "COMMON-LISP";
}

bool LispIsSymbolNamed(const LispNode &node, const char *upper_name) {
    return node.kind == LispNodeKind::Symbol && !node.uninterned && LispIsUnqualifiedOrCl(node) &&
           node.name == upper_name;
}

bool LispIsKeyword(const LispNode &node) { return node.kind == LispNodeKind::Symbol && node.package == "KEYWORD"; }

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

/** @brief The standard readtable's whitespace[2] characters. */
bool IsWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/** @brief The terminating macro characters: a token ends at any of these. `#` is non-terminating. */
bool IsTerminating(char c) {
    return c == '"' || c == '\'' || c == '(' || c == ')' || c == ',' || c == ';' || c == '`';
}

/** @brief Upcases one ASCII byte and leaves every other byte (including UTF-8 continuation bytes) alone. */
char UpcaseAscii(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

/** @brief Matches `[sign] digit+` starting at `i`, returning the index past it, or npos. */
size_t MatchDigits(const std::string &t, size_t i) {
    const size_t start = i;
    while (i < t.size() && IsDigit(t[i])) i++;
    return i == start ? std::string::npos : i;
}

bool IsExponentMarker(char c) {
    const char u = UpcaseAscii(c);
    return u == 'E' || u == 'S' || u == 'F' || u == 'D' || u == 'L';
}

/** @brief Reports whether a token is a rational in `radix` ([sign] digits, or a ratio of them). */
bool IsRationalInRadix(const std::string &t, int radix, bool *zero_denominator) {
    *zero_denominator = false;
    size_t i = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) i++;
    auto digit_ok = [radix](char c) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
        return v >= 0 && v < radix;
    };
    const size_t num_start = i;
    while (i < t.size() && digit_ok(t[i])) i++;
    if (i == num_start) return false;
    if (i == t.size()) return true;
    if (t[i] != '/') return false;
    i++;
    const size_t den_start = i;
    bool all_zero = true;
    while (i < t.size() && digit_ok(t[i])) {
        if (t[i] != '0') all_zero = false;
        i++;
    }
    if (i == den_start || i != t.size()) return false;
    *zero_denominator = all_zero;
    return true;
}

// The character names an implementation is required or commonly known to
// accept (CLHS 13.1.7, plus the semi-standard and ASCII control names
// SBCL, CCL, ECL and CLISP all read). A name outside this set is still
// accepted when it has the shape of a Unicode name (`Greek_Small_Letter_Alpha`)
// or a code point (`U+03BB`), which is how every Unicode-aware Lisp spells
// the rest.
const char *const kCharNames[] = {
    "SPACE", "NEWLINE", "TAB",   "PAGE",  "RUBOUT", "LINEFEED", "RETURN", "BACKSPACE", "NUL",   "NULL",
    "BELL",  "ESCAPE",  "ESC",   "ALTMODE", "DELETE", "DEL",    "SOH",    "STX",       "ETX",   "EOT",
    "ENQ",   "ACK",     "BEL",   "BS",    "HT",     "LF",       "NL",     "VT",        "FF",    "NP",
    "CR",    "SO",      "SI",    "DLE",   "DC1",    "DC2",      "DC3",    "DC4",       "NAK",   "SYN",
    "ETB",   "CAN",     "EM",    "SUB",   "FS",     "GS",       "RS",     "US",        "SP",    "LINE_FEED",
    "FORM_FEED", "NO-BREAK_SPACE", "NBSP",
};

bool IsKnownCharName(const std::string &name);

/** @brief Levenshtein distance between two short strings. */
int Distance(const std::string &a, const std::string &b) {
    std::vector<int> prev(b.size() + 1);
    std::vector<int> cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[b.size()];
}

/**
 * @brief The standard character name an unknown one is a misspelling of, or "" when it may be a real name.
 *
 * Every Unicode-aware Lisp reads the full Unicode name list (`#\Caron`,
 * `#\Greek_Small_Letter_Alpha`), which no table here could hold. So an
 * unknown name is only reported when it is one or two edits from one of
 * the standard names -- `#\Spcae`, `#\Newlin` -- which is what a typo
 * looks like and what no Unicode name is.
 */
std::string MisspelledCharName(const std::string &name) {
    if (IsKnownCharName(name)) return std::string();
    std::string up;
    for (char c : name) up += UpcaseAscii(c);
    static const char *const kStandard[] = {"SPACE", "NEWLINE", "TAB", "PAGE", "RUBOUT", "LINEFEED", "RETURN",
                                            "BACKSPACE", "ESCAPE", "DELETE"};
    for (const char *s : kStandard) {
        const std::string cand = s;
        const int limit = cand.size() >= 5 ? 2 : 1;
        if (std::abs(static_cast<int>(cand.size()) - static_cast<int>(up.size())) > limit) continue;
        if (Distance(up, cand) <= limit) return cand;
    }
    return std::string();
}

/** @brief Reports whether `name` (2+ characters, as written after `#\`) is a character name a Lisp reads. */
bool IsKnownCharName(const std::string &name) {
    std::string up;
    for (char c : name) up += UpcaseAscii(c);
    for (const char *known : kCharNames) {
        if (up == known) return true;
    }
    // A Unicode name: SBCL and friends read any character's name with
    // spaces as underscores. Anything with an underscore or with
    // non-ASCII bytes is given the benefit of the doubt.
    for (char c : name) {
        if (c == '_' || static_cast<unsigned char>(c) >= 0x80) return true;
    }
    // A code point: U+3BB, u3BB, x3bb (CCL and SBCL both).
    size_t i = 0;
    if (up.size() > 2 && up[0] == 'U' && up[1] == '+') i = 2;
    else if (up.size() > 1 && (up[0] == 'U' || up[0] == 'X')) i = 1;
    if (i > 0) {
        bool hex = true;
        for (size_t k = i; k < up.size(); k++) {
            if (std::isxdigit(static_cast<unsigned char>(up[k])) == 0) hex = false;
        }
        if (hex) return true;
    }
    // Control-character notation (`#\^A`, `#\C-a`) some implementations accept.
    if (up.size() >= 2 && up[0] == '^') return true;
    return false;
}

/** @brief The byte length of the UTF-8 sequence a lead byte starts (1 for anything malformed). */
size_t Utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

class Reader {
  public:
    Reader(const std::vector<std::string> &lines, LispReadResult *out) : out_(out) {
        for (size_t i = 0; i < lines.size(); i++) {
            line_starts_.push_back(text_.size());
            text_ += lines[i];
            if (i + 1 < lines.size()) text_ += '\n';
        }
        if (line_starts_.empty()) line_starts_.push_back(0);
    }

    void Run() {
        may_recover_ = !ParensBalance();
        // `#!/usr/bin/sbcl --script`: SBCL skips a first line that starts
        // with `#!`, and so does every script-running wrapper.
        if (text_.compare(0, 2, "#!") == 0) {
            size_t eol = text_.find('\n');
            if (eol == std::string::npos) eol = text_.size();
            AddComment(0, eol, false);
            pos_ = eol;
        }
        for (;;) {
            SkipAtmosphere();
            if (pos_ >= text_.size()) break;
            if (text_[pos_] == ')') {
                Error(pos_, pos_ + 1, "stray-close-paren", "Unmatched `)`: there is no open form for it to close.");
                pos_++;
                continue;
            }
            recover_ = false;
            const int node = ReadForm();
            if (node >= 0) out_->top.push_back(node);
        }
    }

  private:
    LispReadResult *out_;
    std::string text_;
    std::vector<size_t> line_starts_;
    size_t pos_ = 0;
    int depth_ = 0;         // current recursion depth of ReadForm
    int list_depth_ = 0;    // how many lists are open
    int backquote_ = 0;     // backquote nesting, for comma checks
    int suppress_ = 0;      // inside a #+/#- form: token-level errors are not errors
    int unclosed_ = 0;      // lists left unclosed by the current recovery
    bool recover_ = false;  // a top-level definition was found inside an open form
    // Whether the recovery above is allowed at all: only when the file's
    // parens do not balance. A balanced file that writes a `(defun` at
    // column 0 inside an `eval-when` means exactly that, and reading it
    // any other way would invent errors.
    bool may_recover_ = false;

    /** @brief A quick structural pre-scan: do the file's parens balance, outside strings, comments and escapes? */
    bool ParensBalance() const {
        int nest = 0;
        size_t i = 0;
        const size_t n = text_.size();
        while (i < n) {
            const char c = text_[i];
            if (c == '\\') {
                i += 2;
            } else if (c == '#' && i + 1 < n && text_[i + 1] == '\\') {
                i += 3;
            } else if (c == '#' && i + 1 < n && text_[i + 1] == '|') {
                int depth = 1;
                i += 2;
                while (i < n && depth > 0) {
                    if (text_[i] == '|' && i + 1 < n && text_[i + 1] == '#') {
                        depth--;
                        i += 2;
                    } else if (text_[i] == '#' && i + 1 < n && text_[i + 1] == '|') {
                        depth++;
                        i += 2;
                    } else {
                        i++;
                    }
                }
            } else if (c == '"' || c == '|') {
                i++;
                while (i < n && text_[i] != c) i += text_[i] == '\\' ? size_t{2} : size_t{1};
                i++;
            } else if (c == ';') {
                while (i < n && text_[i] != '\n') i++;
            } else {
                if (c == '(') nest++;
                if (c == ')' && --nest < 0) return false;
                i++;
            }
        }
        return nest == 0;
    }

    // --- Positions ------------------------------------------------------

    int LineOf(size_t offset) const {
        const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
        return static_cast<int>(it - line_starts_.begin()) - 1;
    }

    LispRange RangeOf(size_t start, size_t end) const {
        LispRange r;
        r.line = LineOf(start);
        r.col = static_cast<int>(start - line_starts_[static_cast<size_t>(r.line)]);
        r.end_line = LineOf(end);
        r.end_col = static_cast<int>(end - line_starts_[static_cast<size_t>(r.end_line)]);
        return r;
    }

    int ColOf(size_t offset) const {
        return static_cast<int>(offset - line_starts_[static_cast<size_t>(LineOf(offset))]);
    }

    // --- Output ---------------------------------------------------------

    void Error(size_t start, size_t end, const char *code, const std::string &message) {
        LispReadError e;
        e.range = RangeOf(start, std::max(end, start));
        e.code = code;
        e.message = message;
        out_->errors.push_back(e);
    }

    /** @brief Records a token-level error, unless inside a feature-conditional form (read as if suppressed). */
    void SoftError(size_t start, size_t end, const char *code, const std::string &message) {
        if (suppress_ > 0) return;
        Error(start, end, code, message);
    }

    void AddComment(size_t start, size_t end, bool block) {
        LispComment c;
        c.range = RangeOf(start, end);
        c.text = text_.substr(start, end - start);
        c.block = block;
        out_->comments.push_back(c);
    }

    int NewNode(LispNodeKind kind, size_t start, size_t end) {
        LispNode n;
        n.kind = kind;
        n.range = RangeOf(start, end);
        out_->nodes.push_back(n);
        return static_cast<int>(out_->nodes.size()) - 1;
    }

    LispNode &Node(int index) { return out_->nodes[static_cast<size_t>(index)]; }

    void SetEnd(int node, size_t end) {
        const LispRange r = RangeOf(end, end);
        Node(node).range.end_line = r.line;
        Node(node).range.end_col = r.col;
    }

    void AddKid(int parent, int kid) {
        if (kid < 0) return;
        Node(parent).kids.push_back(kid);
        Node(kid).parent = parent;
    }

    // --- Atmosphere -----------------------------------------------------

    /** @brief Skips whitespace, `;` comments and `#|...|#` comments, recording the comments. */
    void SkipAtmosphere() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (IsWhitespace(c)) {
                pos_++;
            } else if (c == ';') {
                size_t eol = text_.find('\n', pos_);
                if (eol == std::string::npos) eol = text_.size();
                AddComment(pos_, eol, false);
                pos_ = eol;
            } else if (c == '#' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '|') {
                SkipBlockComment(pos_, pos_ + 2);
            } else {
                return;
            }
        }
    }

    /** @brief Skips one (nestable) `#| ... |#` comment that starts at `start` and whose body starts at `body`. */
    void SkipBlockComment(size_t start, size_t body) {
        pos_ = body;
        int nest = 1;
        while (pos_ < text_.size() && nest > 0) {
            if (text_[pos_] == '|' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '#') {
                nest--;
                pos_ += 2;
            } else if (text_[pos_] == '#' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '|') {
                nest++;
                pos_ += 2;
            } else {
                pos_++;
            }
        }
        AddComment(start, pos_, true);
        if (nest > 0) {
            Error(start, start + 2, "unterminated-comment",
                  "This `#|` comment is never closed with `|#`, so the rest of the file is comment.");
        }
    }

    // --- Forms ----------------------------------------------------------

    /**
     * @brief Reads one form at pos_ (which is at a non-atmosphere character other than `)`).
     * @return the node, or -1 when nothing could be read (the error is already recorded)
     */
    int ReadForm() {
        if (depth_ >= kLispMaxNesting) {
            const size_t start = pos_;
            SkipRawForm();
            Error(start, std::min(start + 1, text_.size()), "too-deep",
                  "This form nests more than " + std::to_string(kLispMaxNesting) +
                      " levels deep; it is not analyzed past that point.");
            return NewNode(LispNodeKind::Error, start, pos_);
        }
        depth_++;
        const int node = ReadFormInner();
        depth_--;
        return node;
    }

    int ReadFormInner() {
        const size_t start = pos_;
        const char c = text_[pos_];
        switch (c) {
            case '(': return ReadList(start, start + 1, LispNodeKind::List);
            case '\'': pos_++; return ReadPrefix(start, LispNodeKind::Quote, "'");
            case '`': {
                pos_++;
                backquote_++;
                const int n = ReadPrefix(start, LispNodeKind::Backquote, "`");
                backquote_--;
                return n;
            }
            case ',': {
                pos_++;
                LispNodeKind kind = LispNodeKind::Unquote;
                std::string spelling = ",";
                if (pos_ < text_.size() && (text_[pos_] == '@' || text_[pos_] == '.')) {
                    spelling += text_[pos_];
                    kind = LispNodeKind::Splice;
                    pos_++;
                }
                if (backquote_ == 0) {
                    SoftError(start, pos_, "comma-outside-backquote",
                              "`" + spelling + "` outside a backquote: the reader rejects a comma anywhere else.");
                }
                const int saved = backquote_;
                backquote_ = std::max(0, backquote_ - 1);
                const int n = ReadPrefix(start, kind, spelling);
                backquote_ = saved;
                return n;
            }
            case '"': return ReadString();
            case '#': return ReadDispatch();
            default: return ReadTokenForm();
        }
    }

    /** @brief Reads the operand of a prefix (`'`, `` ` ``, `,`, `#'`, `#.`, `#P`, ...) and wraps it. */
    int ReadPrefix(size_t start, LispNodeKind kind, const std::string &spelling) {
        const int node = NewNode(kind, start, pos_);
        Node(node).text = spelling;
        SkipAtmosphere();
        if (pos_ >= text_.size() || text_[pos_] == ')') {
            Error(start, start + spelling.size(), "missing-form",
                  "`" + spelling + "` must be followed by a form, but " +
                      (pos_ >= text_.size() ? std::string("the file ends here.") : std::string("a `)` follows.")));
            return node;
        }
        const int operand = ReadForm();
        AddKid(node, operand);
        SetEnd(node, pos_);
        return node;
    }

    /** @brief Reads a list or vector whose open paren occupies [start, body). */
    int ReadList(size_t start, size_t body, LispNodeKind kind) {
        const int node = NewNode(kind, start, body);
        pos_ = body;
        list_depth_++;
        bool after_dot = false;
        int forms_after_dot = 0;
        bool closed = false;
        for (;;) {
            SkipAtmosphere();
            if (pos_ >= text_.size()) break;
            const char c = text_[pos_];
            if (c == ')') {
                pos_++;
                closed = true;
                break;
            }
            // Recovery: a top-level definition written at column 0 while a
            // form is still open almost always means the form above it is
            // missing its close parens. Ending the open forms here keeps
            // one forgotten `)` from swallowing the rest of the file --
            // which is what every structural answer below depends on
            // while the user is mid-edit.
            if (may_recover_ && c == '(' && ColOf(pos_) == 0 && LooksLikeTopLevelDefinition(pos_ + 1)) {
                recover_ = true;
                break;
            }
            if (c == '.' && IsLoneDot(pos_)) {
                const size_t dot = pos_;
                pos_++;
                if (Node(node).kids.empty() || after_dot || kind != LispNodeKind::List) {
                    SoftError(dot, dot + 1, "bad-dot", "A consing dot must follow at least one element of a list.");
                } else {
                    after_dot = true;
                    Node(node).dotted = true;
                }
                continue;
            }
            const int kid = ReadForm();
            AddKid(node, kid);
            // `. #+a (x) #-a ()`: only one of those exists on any platform.
            if (after_dot && kid >= 0 && Node(kid).kind != LispNodeKind::Feature) {
                forms_after_dot++;
                if (forms_after_dot == 2) {
                    SoftError(ToOffset(Node(kid).range), pos_, "bad-dot",
                              "Only one form may follow a consing dot.");
                }
            }
            if (recover_) break;
        }
        list_depth_--;
        SetEnd(node, pos_);
        bool dot_has_feature = false;
        for (int kid : Node(node).kids) {
            if (Node(kid).kind == LispNodeKind::Feature) dot_has_feature = true;
        }
        if (after_dot && forms_after_dot == 0 && closed && !dot_has_feature) {
            SoftError(start, pos_, "bad-dot", "A consing dot must be followed by exactly one form.");
        }
        if (!closed) {
            unclosed_++;
            if (list_depth_ == 0) {
                std::string msg = unclosed_ == 1 ? "This form is never closed: a `)` is missing"
                                                 : "This form is never closed: " + std::to_string(unclosed_) +
                                                       " `)` are missing";
                msg += recover_ ? " before the next top-level definition (line " +
                                      std::to_string(LineOf(pos_) + 1) + ")."
                                : " before the end of the file.";
                Error(start, start + 1, "unbalanced-paren", msg);
                unclosed_ = 0;
            }
        }
        return node;
    }

    size_t ToOffset(const LispRange &r) const {
        return line_starts_[static_cast<size_t>(r.line)] + static_cast<size_t>(r.col);
    }

    /** @brief Reports whether the `.` at `at` is a token on its own (the consing dot). */
    bool IsLoneDot(size_t at) const {
        const size_t next = at + 1;
        return next >= text_.size() || IsWhitespace(text_[next]) || IsTerminating(text_[next]);
    }

    /** @brief Reports whether the text after an open paren names a defining form (`defun`, `in-package`, ...). */
    bool LooksLikeTopLevelDefinition(size_t at) const {
        std::string word;
        for (size_t i = at; i < text_.size() && word.size() < 24; i++) {
            const char c = text_[i];
            if (IsWhitespace(c) || IsTerminating(c)) break;
            word += UpcaseAscii(c);
        }
        if (word.rfind("CL:", 0) == 0) word = word.substr(3);
        return word.rfind("DEF", 0) == 0 || word == "IN-PACKAGE" || word == "EVAL-WHEN";
    }

    /** @brief Skips one form without building nodes: used past the nesting limit. */
    void SkipRawForm() {
        int nest = 0;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '\\' || (c == '#' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '\\')) {
                pos_ += c == '\\' ? 2 : 3;
                continue;
            }
            if (c == '"') {
                pos_++;
                while (pos_ < text_.size() && text_[pos_] != '"') pos_ += text_[pos_] == '\\' ? size_t{2} : size_t{1};
                pos_++;
                continue;
            }
            if (c == ';') {
                while (pos_ < text_.size() && text_[pos_] != '\n') pos_++;
                continue;
            }
            if (c == '(') nest++;
            if (c == ')') {
                if (nest == 0) return;
                nest--;
                pos_++;
                if (nest == 0) return;
                continue;
            }
            pos_++;
            if (nest == 0 && (IsWhitespace(c) || c == ')')) return;
        }
        pos_ = std::min(pos_, text_.size());
    }

    int ReadString() {
        const size_t start = pos_;
        pos_++;
        std::string value;
        bool closed = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '\\' && pos_ + 1 < text_.size()) {
                value += text_[pos_ + 1];
                pos_ += 2;
                continue;
            }
            if (c == '"') {
                pos_++;
                closed = true;
                break;
            }
            value += c;
            pos_++;
        }
        pos_ = std::min(pos_, text_.size());
        const int node = NewNode(LispNodeKind::String, start, pos_);
        Node(node).text = text_.substr(start, pos_ - start);
        Node(node).name = value;
        if (!closed) {
            Error(start, start + 1, "unterminated-string",
                  "This string is never closed, so the rest of the file is part of it.");
        }
        return node;
    }

    // --- Tokens ---------------------------------------------------------

    struct Token {
        size_t start = 0;
        size_t end = 0;
        std::string raw;        // exactly as written
        std::string canonical;  // escapes resolved, unescaped characters upcased
        // For each byte of `canonical`, whether it came from an escape.
        std::vector<bool> escaped_at;
        bool any_escape = false;
        bool unterminated_escape = false;
    };

    /** @brief Accumulates a token starting at pos_ under CLHS 2.3's rules (single and multiple escapes). */
    Token ScanToken() {
        Token t;
        t.start = pos_;
        bool in_multiple = false;
        size_t multiple_start = 0;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (in_multiple) {
                if (c == '|') {
                    in_multiple = false;
                    pos_++;
                } else if (c == '\\' && pos_ + 1 < text_.size()) {
                    t.canonical += text_[pos_ + 1];
                    t.escaped_at.push_back(true);
                    pos_ += 2;
                } else {
                    t.canonical += c;
                    t.escaped_at.push_back(true);
                    pos_++;
                }
                continue;
            }
            if (IsWhitespace(c) || IsTerminating(c)) break;
            if (c == '|') {
                in_multiple = true;
                multiple_start = pos_;
                t.any_escape = true;
                pos_++;
                continue;
            }
            if (c == '\\') {
                t.any_escape = true;
                if (pos_ + 1 < text_.size()) {
                    t.canonical += text_[pos_ + 1];
                    t.escaped_at.push_back(true);
                    pos_ += 2;
                } else {
                    pos_++;
                }
                continue;
            }
            t.canonical += UpcaseAscii(c);
            t.escaped_at.push_back(false);
            pos_++;
        }
        if (in_multiple) {
            t.unterminated_escape = true;
            Error(multiple_start, multiple_start + 1, "unterminated-escape",
                  "This `|` escape is never closed, so the rest of the file is part of one symbol name.");
        }
        t.end = pos_;
        t.raw = text_.substr(t.start, t.end - t.start);
        return t;
    }

    /** @brief Reads a token at pos_ and interprets it as a number or a symbol. */
    int ReadTokenForm() {
        Token t = ScanToken();
        if (t.end == t.start) {
            // Defensive: a character that starts no token. Consume it so
            // the read always makes progress.
            pos_++;
            const int node = NewNode(LispNodeKind::Error, t.start, pos_);
            Node(node).text = text_.substr(t.start, 1);
            return node;
        }
        if (!t.any_escape) {
            bool all_dots = true;
            for (char c : t.raw) {
                if (c != '.') all_dots = false;
            }
            if (all_dots) {
                SoftError(t.start, t.end, "bad-dot",
                          t.raw.size() == 1 ? "A consing dot is only allowed inside a list, after an element."
                                            : "A token made only of dots is not valid Lisp syntax.");
                const int node = NewNode(LispNodeKind::Error, t.start, t.end);
                Node(node).text = t.raw;
                return node;
            }
            if (LispIsNumberToken(t.raw)) {
                const int node = NewNode(LispNodeKind::Number, t.start, t.end);
                Node(node).text = t.raw;
                const size_t slash = t.raw.find('/');
                if (slash != std::string::npos && t.raw.find_first_not_of('0', slash + 1) == std::string::npos) {
                    SoftError(t.start, t.end, "bad-number", "A ratio with a zero denominator cannot be read.");
                }
                return node;
            }
        }
        return MakeSymbol(t, false);
    }

    /** @brief Builds a symbol node from a scanned token, splitting off any package prefix. */
    int MakeSymbol(const Token &t, bool uninterned) {
        const int node = NewNode(LispNodeKind::Symbol, uninterned ? t.start - 2 : t.start, t.end);
        LispNode &n = Node(node);
        n.text = uninterned ? "#:" + t.raw : t.raw;
        n.escaped = t.any_escape;
        n.uninterned = uninterned;
        n.name_col = ColOf(t.start);
        // Unescaped colons are package markers.
        std::vector<size_t> colons;
        for (size_t i = 0; i < t.canonical.size(); i++) {
            if (t.canonical[i] == ':' && !t.escaped_at[i]) colons.push_back(i);
        }
        if (colons.empty()) {
            n.name = t.canonical;
            return node;
        }
        if (uninterned) {
            n.name = t.canonical;
            SoftError(t.start, t.end, "bad-symbol", "An uninterned symbol (`#:`) cannot carry a package prefix.");
            return node;
        }
        const size_t first = colons.front();
        const bool double_colon = colons.size() == 2 && colons[1] == first + 1;
        if (colons.size() > 2 || (colons.size() == 2 && !double_colon)) {
            n.name = t.canonical;
            SoftError(t.start, t.end, "bad-symbol", "Too many package markers in `" + t.raw + "`.");
            return node;
        }
        const size_t name_start = first + (double_colon ? 2 : 1);
        size_t after = pos_;
        while (after < text_.size() && IsWhitespace(text_[after])) after++;
        if (name_start >= t.canonical.size() && first > 0 && after < text_.size() && text_[after] == '(') {
            pos_ = after;
            // SBCL's `sb-kernel::(form ...)`: the form is read with that
            // package current. Kept as a prefix over the form.
            n.kind = LispNodeKind::Dispatch;
            n.text = t.raw;
            n.name.clear();
            const int form = ReadForm();
            AddKid(node, form);
            SetEnd(node, pos_);
            return node;
        }
        if (name_start >= t.canonical.size()) {
            n.name = t.canonical.substr(0, first);
            SoftError(t.start, t.end, "bad-symbol", "`" + t.raw + "` has a package prefix but no symbol name.");
            return node;
        }
        n.name = t.canonical.substr(name_start);
        if (first == 0) {
            n.package = "KEYWORD";
        } else {
            n.package = t.canonical.substr(0, first);
            n.internal = double_colon;
        }
        // Map the canonical index back to a source column: only escapes
        // make the two differ, and `|...|` around a whole name is the
        // common case, so walk the raw text counting canonical bytes.
        size_t raw_i = 0;
        size_t canon_i = 0;
        while (raw_i < t.raw.size() && canon_i < name_start) {
            const char c = t.raw[raw_i];
            if (c == '|') {
                raw_i++;
                continue;
            }
            if (c == '\\') {
                raw_i += 2;
                canon_i++;
                continue;
            }
            raw_i++;
            canon_i++;
        }
        n.name_col = ColOf(t.start + raw_i);
        return node;
    }

    // --- Dispatch macros (#) ----------------------------------------------

    int ReadDispatch() {
        const size_t start = pos_;
        pos_++;
        std::string numarg;
        while (pos_ < text_.size() && IsDigit(text_[pos_])) numarg += text_[pos_++];
        if (pos_ >= text_.size()) {
            Error(start, pos_, "unknown-dispatch", "The file ends in the middle of a `#` dispatch macro.");
            const int node = NewNode(LispNodeKind::Error, start, pos_);
            Node(node).text = text_.substr(start);
            return node;
        }
        const char sub = text_[pos_];
        const char up = UpcaseAscii(sub);
        switch (up) {
            case '\\': return ReadCharacter(start);
            case '\'': pos_++; return ReadPrefix(start, LispNodeKind::Function, "#'");
            case '(': return ReadList(start, pos_ + 1, LispNodeKind::Vector);
            case '.': pos_++; return ReadPrefix(start, LispNodeKind::ReadEval, "#.");
            case ':': {
                pos_++;
                Token t = ScanToken();
                if (t.end == t.start) {
                    SoftError(start, pos_, "bad-symbol", "`#:` must be followed by a symbol name.");
                }
                return MakeSymbol(t, true);
            }
            case '*': {
                pos_++;
                Token t = ScanToken();
                const int node = NewNode(LispNodeKind::BitVector, start, t.end);
                Node(node).text = text_.substr(start, t.end - start);
                if (t.raw.find_first_not_of("01") != std::string::npos) {
                    SoftError(start, t.end, "bad-number", "A bit vector (`#*`) may only contain 0 and 1.");
                }
                return node;
            }
            case 'B':
            case 'O':
            case 'X':
            case 'R': {
                int radix = up == 'B' ? 2 : (up == 'O' ? 8 : 16);
                if (up == 'R') {
                    radix = numarg.empty() ? 0 : std::atoi(numarg.c_str());
                    if (radix < 2 || radix > 36) {
                        SoftError(start, pos_ + 1, "bad-number", "`#r` needs a radix from 2 to 36, as in `#36rZZ`.");
                        radix = 36;
                    }
                }
                pos_++;
                Token t = ScanToken();
                const int node = NewNode(LispNodeKind::Number, start, t.end);
                Node(node).text = text_.substr(start, t.end - start);
                bool zero_den = false;
                if (t.any_escape || !IsRationalInRadix(t.raw, radix, &zero_den)) {
                    SoftError(start, t.end, "bad-number",
                              "`" + Node(node).text + "` is not a rational number in radix " + std::to_string(radix) +
                                  ".");
                } else if (zero_den) {
                    SoftError(start, t.end, "bad-number", "A ratio with a zero denominator cannot be read.");
                }
                return node;
            }
            case 'C':
            case 'A':
            case 'S':
            case 'P': {
                pos_++;
                std::string spelling = "#" + numarg;
                spelling += up;
                return ReadPrefix(start, LispNodeKind::Dispatch, spelling);
            }
            case '=': {
                pos_++;
                if (numarg.empty()) {
                    SoftError(start, pos_, "unknown-dispatch", "`#=` needs a label number, as in `#1=`.");
                }
                return ReadPrefix(start, LispNodeKind::Dispatch, "#" + numarg + "=");
            }
            case '#': {
                pos_++;
                const int node = NewNode(LispNodeKind::LabelRef, start, pos_);
                Node(node).text = text_.substr(start, pos_ - start);
                if (numarg.empty()) {
                    SoftError(start, pos_, "unknown-dispatch", "`##` needs a label number, as in `#1#`.");
                }
                return node;
            }
            case '+':
            case '-': return ReadFeature(start, sub);
            case '|': {
                // `#|` reached here only after a numeric argument (`#1|`),
                // which the standard leaves undefined; treat it as a comment.
                SkipBlockComment(start, pos_ + 1);
                SkipAtmosphere();
                if (pos_ >= text_.size() || text_[pos_] == ')') {
                    const int node = NewNode(LispNodeKind::Error, start, pos_);
                    return node;
                }
                return ReadForm();
            }
            case '_':
            case '$': {
                // Clozure CL's own reader macros for foreign names (`#_malloc`,
                // `#$O_RDONLY`), part of that implementation's standard
                // readtable: read the name, raise nothing.
                pos_++;
                Token t = ScanToken();
                const int node = NewNode(LispNodeKind::Error, start, t.end);
                Node(node).text = text_.substr(start, t.end - start);
                return node;
            }
            case '<': {
                pos_++;
                SoftError(start, pos_, "unreadable-object",
                          "`#<` is how unreadable objects print; the reader always rejects it.");
                const int node = NewNode(LispNodeKind::Error, start, pos_);
                Node(node).text = "#<";
                return node;
            }
            default: break;
        }
        // Unknown, or one the standard reserves (`#)`, `#` + whitespace,
        // `#!`, `#[`, `#{`, `#?` ...): consume the subchar so the read
        // makes progress, unless it is a delimiter the enclosing form needs.
        // A terminating macro character (`#"..."`, ABCL's Java syntax) is
        // left for the normal reader, so the structure around it survives.
        if (sub != ')' && !IsWhitespace(sub) && !IsTerminating(sub)) pos_ += Utf8Length(static_cast<unsigned char>(sub));
        SoftError(start, std::max(pos_, start + 1), "unknown-dispatch",
                  IsWhitespace(sub) || sub == ')'
                      ? std::string("`#` must be followed by a dispatch character.")
                      : "`#" + std::string(1, sub) +
                            "` is not a standard dispatch macro; this file would need a readtable that defines it.");
        const int node = NewNode(LispNodeKind::Error, start, pos_);
        Node(node).text = text_.substr(start, pos_ - start);
        return node;
    }

    int ReadCharacter(size_t start) {
        pos_++;  // past the backslash
        if (pos_ >= text_.size()) {
            Error(start, pos_, "missing-form", "`#\\` must be followed by a character.");
            const int node = NewNode(LispNodeKind::Character, start, pos_);
            Node(node).text = text_.substr(start);
            return node;
        }
        // The first character is taken whatever it is -- `#\(`, `#\ `
        // and `#\;` are all characters -- and any constituents after it
        // make the whole thing a character *name*.
        const size_t name_start = pos_;
        pos_ += Utf8Length(static_cast<unsigned char>(text_[pos_]));
        pos_ = std::min(pos_, text_.size());
        while (pos_ < text_.size() && !IsWhitespace(text_[pos_]) && !IsTerminating(text_[pos_])) pos_++;
        const int node = NewNode(LispNodeKind::Character, start, pos_);
        Node(node).text = text_.substr(start, pos_ - start);
        const std::string name = text_.substr(name_start, pos_ - name_start);
        const size_t first_len = Utf8Length(static_cast<unsigned char>(name[0]));
        if (name.size() > first_len) {
            const std::string near = MisspelledCharName(name);
            if (!near.empty()) {
                std::string proper = near.substr(0, 1);
                for (size_t i = 1; i < near.size(); i++) proper += static_cast<char>(near[i] - 'A' + 'a');
                SoftError(start, pos_, "unknown-character-name",
                          "`#\\" + name + "` is not a character name; did you mean `#\\" + proper + "`?");
            }
        }
        return node;
    }

    int ReadFeature(size_t start, char sign) {
        pos_++;
        const int node = NewNode(LispNodeKind::Feature, start, pos_);
        Node(node).text = sign == '+' ? "#+" : "#-";
        SkipAtmosphere();
        if (pos_ >= text_.size() || text_[pos_] == ')') {
            Error(start, pos_, "missing-form", "`" + Node(node).text + "` must be followed by a feature expression.");
            return node;
        }
        // The feature expression is read in the keyword package: a bare
        // `sbcl` there is `:sbcl`.
        suppress_++;
        const int feature = ReadForm();
        suppress_--;
        MakeKeywords(feature);
        AddKid(node, feature);
        if (recover_) return node;
        SkipAtmosphere();
        if (pos_ >= text_.size() || text_[pos_] == ')') {
            Error(start, pos_, "missing-form",
                  "`" + Node(node).text + "` must be followed by a feature expression and then a form.");
            SetEnd(node, pos_);
            return node;
        }
        // A form that is read with *read-suppress* on whenever the feature
        // test fails -- which, from source alone, it may. Errors that the
        // suppressed reader would not raise are not raised here either.
        suppress_++;
        const int form = ReadForm();
        suppress_--;
        if (form >= 0) Node(form).conditional = true;
        AddKid(node, form);
        SetEnd(node, pos_);
        return node;
    }

    /** @brief Interprets every unqualified symbol inside a feature expression as a keyword. */
    void MakeKeywords(int node) {
        if (node < 0) return;
        LispNode &n = Node(node);
        if (n.kind == LispNodeKind::Symbol && n.package.empty() && !n.uninterned) n.package = "KEYWORD";
        const std::vector<int> kids = n.kids;
        for (int kid : kids) MakeKeywords(kid);
    }
};

}  // namespace

bool LispIsNumberToken(const std::string &t) {
    if (t.empty()) return false;
    size_t i = 0;
    if (t[i] == '+' || t[i] == '-') i++;
    if (i >= t.size()) return false;
    // integer: digits [.]   ratio: digits / digits
    size_t after = MatchDigits(t, i);
    if (after != std::string::npos) {
        if (after == t.size()) return true;
        if (t[after] == '/') {
            const size_t den = MatchDigits(t, after + 1);
            return den == t.size();
        }
        if (t[after] == '.' && after + 1 == t.size()) return true;
        // float: digits [. digits*] exponent | digits . digits+ [exponent]
        size_t j = after;
        bool has_fraction = false;
        if (t[j] == '.') {
            j++;
            const size_t frac = MatchDigits(t, j);
            if (frac != std::string::npos) {
                j = frac;
                has_fraction = true;
            }
        }
        if (j == t.size()) return has_fraction;
        if (IsExponentMarker(t[j])) {
            j++;
            if (j < t.size() && (t[j] == '+' || t[j] == '-')) j++;
            const size_t exp = MatchDigits(t, j);
            return exp == t.size();
        }
        return false;
    }
    // float with no integer part: . digits+ [exponent]
    if (t[i] != '.') return false;
    size_t j = MatchDigits(t, i + 1);
    if (j == std::string::npos) return false;
    if (j == t.size()) return true;
    if (!IsExponentMarker(t[j])) return false;
    j++;
    if (j < t.size() && (t[j] == '+' || t[j] == '-')) j++;
    return MatchDigits(t, j) == t.size();
}

LispReadResult LispRead(const std::vector<std::string> &lines) {
    LispReadResult result;
    Reader reader(lines, &result);
    reader.Run();
    return result;
}
