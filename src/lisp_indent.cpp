// Standard Common Lisp indentation over plain text. See lisp_indent.h for
// the rules and for why this is a character scanner and not a parser.

#include "lisp_indent.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {

struct SpecEntry {
    const char *name;
    int args;
};

// Distinguished-argument counts, after SLIME's cl-indent.el and Emacs's
// own lisp-indent-function properties. An operator not listed here, and
// not matching one of the prefix rules in LispIndentDistinguishedArgs,
// indents as an ordinary call.
const SpecEntry kSpecs[] = {
    {"BLOCK", 1},
    {"CASE", 1},
    {"CATCH", 1},
    {"CCASE", 1},
    {"CTYPECASE", 1},
    {"DEFCLASS", 2},
    {"DEFGENERIC", 2},
    {"DEFINE-COMPILER-MACRO", 2},
    {"DEFINE-CONDITION", 2},
    {"DEFINE-SETF-EXPANDER", 2},
    {"DEFMACRO", 2},
    {"DEFSETF", 2},
    {"DEFTYPE", 2},
    {"DEFUN", 2},
    {"DESTRUCTURING-BIND", 2},
    {"DO", 2},
    {"DO*", 2},
    {"DOLIST", 1},
    {"DOTIMES", 1},
    {"ECASE", 1},
    {"ETYPECASE", 1},
    {"EVAL-WHEN", 1},
    {"FLET", 1},
    {"HANDLER-BIND", 1},
    {"HANDLER-CASE", 1},
    {"IGNORE-ERRORS", 0},
    {"LABELS", 1},
    {"LAMBDA", 1},
    {"LET", 1},
    {"LET*", 1},
    {"LOCALLY", 0},
    {"MACROLET", 1},
    {"MULTIPLE-VALUE-BIND", 2},
    {"MULTIPLE-VALUE-PROG1", 1},
    {"PRINT-UNREADABLE-OBJECT", 1},
    {"PROG", 1},
    {"PROG*", 1},
    {"PROG1", 1},
    {"PROG2", 2},
    {"PROGN", 0},
    {"PROGV", 2},
    {"RESTART-BIND", 1},
    {"RESTART-CASE", 1},
    {"RETURN-FROM", 1},
    {"SYMBOL-MACROLET", 1},
    {"TAGBODY", 0},
    {"TYPECASE", 1},
    {"UNLESS", 1},
    {"UNWIND-PROTECT", 1},
    {"WHEN", 1},
    {"WITH-ACCESSORS", 2},
    {"WITH-SLOTS", 2},
};

/** @brief What a list is to the form around it, which decides how its own elements align. */
enum class Role {
    Code,        // an ordinary form
    Data,        // align every element with the first: quoted data, vectors, lambda and binding lists
    LocalFns,    // the definition list of flet/labels/macrolet: each element is a LocalFn
    LocalFn,     // one (name lambda-list . body) definition, or a handler-case/restart-case clause
    CaseClause,  // a case/typecase clause: elements align with the key
    Feature,     // the feature expression after #+/#-: not an element of its parent
};

struct Frame {
    int open_col = 0;
    int open_line = 0;
    Role role = Role::Code;
    bool data_inherit = false;  // children of this list are data too (it was quoted)
    std::string op;             // canonical name of a symbol first element, "" otherwise
    bool op_is_symbol = false;
    int elems = 0;  // elements started so far, the operator included
    int first_elem_col = -1;
    int first_arg_col = -1;  // the second element's column, when it is on the open paren's line
    bool list_arg_seen = false;  // defmethod: a list element after the name (the lambda list) has started
};

char Up(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

bool IsTerm(char c) { return c == '"' || c == '\'' || c == '(' || c == ')' || c == ',' || c == ';' || c == '`'; }

bool StartsWith(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

class Indenter {
  public:
    /** @brief Whether the next line starts inside a string, a `|...|` escape or a block comment. */
    bool InText() const { return in_string_ || in_bar_ || comment_depth_ > 0; }

    /** @brief The column a line starting now should be indented to (meaningless while InText()). */
    int Indent() const {
        if (stack_.empty()) return 0;
        const Frame &f = stack_.back();
        if (f.elems == 0) return f.open_col + 1;
        if (f.role == Role::Data || f.role == Role::Feature || f.role == Role::CaseClause || !f.op_is_symbol) {
            return f.first_elem_col;
        }
        const int args_before = f.elems - 1;
        int spec = f.role == Role::LocalFn ? 1 : LispIndentDistinguishedArgs(f.op);
        if (f.op == "DEFMETHOD") {
            // (defmethod name qualifier* lambda-list . body): the lambda
            // list is the last distinguished argument, wherever it falls.
            return f.list_arg_seen ? f.open_col + 2 : f.open_col + 4;
        }
        if (f.op == "LOOP") {
            return f.first_arg_col >= 0 ? f.first_arg_col : f.open_col + 2;
        }
        if (spec >= 0) return args_before < spec ? f.open_col + 4 : f.open_col + 2;
        return f.first_arg_col >= 0 ? f.first_arg_col : f.open_col + 1;
    }

    /** @brief Advances the scanner over one line of text. */
    void Scan(const std::string &text, int line_no) {
        line_ = line_no;
        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            if (comment_depth_ > 0) {
                if (text[i] == '|' && i + 1 < n && text[i + 1] == '#') {
                    comment_depth_--;
                    i += 2;
                } else if (text[i] == '#' && i + 1 < n && text[i + 1] == '|') {
                    comment_depth_++;
                    i += 2;
                } else {
                    i++;
                }
                continue;
            }
            if (in_string_) {
                if (text[i] == '\\') {
                    i += 2;
                } else if (text[i] == '"') {
                    in_string_ = false;
                    i++;
                } else {
                    i++;
                }
                continue;
            }
            if (in_bar_ || in_token_) {
                i = ContinueToken(text, i);
                continue;
            }
            const char c = text[i];
            const int col = static_cast<int>(i);
            if (IsWs(c)) {
                i++;
            } else if (c == ';') {
                return;  // the rest of the line is a comment
            } else if (c == '(') {
                OpenList(col);
                i++;
            } else if (c == ')') {
                CloseList();
                i++;
            } else if (c == '"') {
                StartElement(col);
                in_string_ = true;
                i++;
            } else if (c == '\'' || c == '`') {
                StartPrefixedElement(col);
                // Only a quote makes data. A backquote is almost always a
                // code template -- `(defun ,name ...) -- and indents as code.
                pending_quote_ = c == '\'';
                i++;
            } else if (c == ',') {
                StartPrefixedElement(col);
                pending_quote_ = false;
                i++;
                if (i < n && (text[i] == '@' || text[i] == '.')) i++;
            } else if (c == '#') {
                i = Dispatch(text, i);
            } else {
                StartElement(col);
                pending_quote_ = false;
                in_token_ = true;
                token_.clear();
                i = ContinueToken(text, i);
            }
        }
        // A token ends at the end of its line unless an escape carries it on.
        if (in_token_ && !in_bar_) EndToken();
    }

  private:
    std::vector<Frame> stack_;
    int line_ = 0;
    bool in_string_ = false;
    bool in_bar_ = false;
    bool in_token_ = false;
    int comment_depth_ = 0;
    // The next element was announced by a prefix (`'`, `#'`, `#+f`, ...)
    // that has already counted it.
    bool pending_ = false;
    bool pending_quote_ = false;  // that prefix was a quote or backquote: the list is data
    bool skip_feature_ = false;   // the next complete element is a #+/#- feature expression
    std::string token_;           // the token being scanned, upcased
    int token_frame_ = -1;        // stack depth the token belongs to
    bool token_is_op_ = false;    // the token is its list's first element

    /** @brief Records that an element starts at `col` in the innermost list. */
    void StartElement(int col) {
        if (skip_feature_) return;  // the feature expression itself is not an element
        if (pending_) {
            // The prefix already counted this element; the element proper
            // carries on from it.
            pending_ = false;
            return;
        }
        if (stack_.empty()) {
            pending_quote_ = false;
            return;
        }
        Frame &f = stack_.back();
        if (f.elems == 0) f.first_elem_col = col;
        if (f.elems == 1 && line_ == f.open_line) f.first_arg_col = col;
        f.elems++;
    }

    /** @brief Same as StartElement, for a prefix: the element it introduces is still to come. */
    void StartPrefixedElement(int col) {
        StartElement(col);
        if (!skip_feature_) pending_ = true;
    }

    void OpenList(int col) {
        Role role = Role::Code;
        bool inherit = false;
        const bool skipping_feature = skip_feature_;
        if (skipping_feature) {
            role = Role::Feature;
            skip_feature_ = false;
        } else {
            const bool quoted = pending_quote_;
            pending_quote_ = false;
            StartElement(col);
            if (quoted) {
                role = Role::Data;
                inherit = true;
            } else if (!stack_.empty()) {
                role = ChildRole(stack_.back());
                if (stack_.back().data_inherit) {
                    role = Role::Data;
                    inherit = true;
                }
            }
        }
        if (!stack_.empty() && !skipping_feature && stack_.back().op == "DEFMETHOD" && stack_.back().elems >= 3) {
            stack_.back().list_arg_seen = true;
        }
        Frame f;
        f.open_col = col;
        f.open_line = line_;
        f.role = role;
        f.data_inherit = inherit;
        stack_.push_back(f);
        token_is_op_ = false;
    }

    void CloseList() {
        if (in_token_) EndToken();
        if (stack_.empty()) return;  // a stray close paren: nothing to close
        const bool was_feature = stack_.back().role == Role::Feature;
        stack_.pop_back();
        // A closed feature expression leaves its prefix waiting for the
        // conditional form, which is the element it counted.
        if (was_feature) pending_ = true;
    }

    /** @brief The role of a list that is element number `parent.elems` (0-based, already counted) of `parent`. */
    static Role ChildRole(const Frame &parent) {
        const int index = parent.elems - 1;  // this list's own position in the parent
        const std::string &op = parent.op;
        if (parent.role == Role::LocalFns) return Role::LocalFn;
        if (parent.role == Role::LocalFn) return index == 1 ? Role::Data : Role::Code;
        if (!parent.op_is_symbol) return Role::Code;
        if (op == "DEFUN" || op == "DEFMACRO" || op == "DEFGENERIC" || op == "DEFINE-COMPILER-MACRO" ||
            op == "DEFTYPE" || op == "DEFINE-SETF-EXPANDER") {
            return index == 2 ? Role::Data : Role::Code;
        }
        if (op == "DEFMETHOD") return (index >= 2 && !parent.list_arg_seen) ? Role::Data : Role::Code;
        if (op == "LAMBDA") return index == 1 ? Role::Data : Role::Code;
        if (op == "DESTRUCTURING-BIND" || op == "MULTIPLE-VALUE-BIND" || op == "WITH-SLOTS" ||
            op == "WITH-ACCESSORS") {
            return index == 1 ? Role::Data : Role::Code;
        }
        if (op == "LET" || op == "LET*" || op == "PROG" || op == "PROG*" || op == "SYMBOL-MACROLET" || op == "DO" ||
            op == "DO*" || op == "HANDLER-BIND" || op == "RESTART-BIND") {
            return index == 1 ? Role::Data : Role::Code;
        }
        if (op == "FLET" || op == "LABELS" || op == "MACROLET") return index == 1 ? Role::LocalFns : Role::Code;
        if (op == "HANDLER-CASE" || op == "RESTART-CASE") return index >= 2 ? Role::LocalFn : Role::Code;
        if (op == "CASE" || op == "ECASE" || op == "CCASE" || op == "TYPECASE" || op == "ETYPECASE" ||
            op == "CTYPECASE") {
            return index >= 2 ? Role::CaseClause : Role::Code;
        }
        return Role::Code;
    }

    /** @brief Handles a `#` at `i`, returning the index past whatever it consumed. */
    size_t Dispatch(const std::string &text, size_t i) {
        const size_t n = text.size();
        const int col = static_cast<int>(i);
        size_t j = i + 1;
        while (j < n && text[j] >= '0' && text[j] <= '9') j++;
        if (j >= n) {
            StartElement(col);
            return n;
        }
        const char sub = Up(text[j]);
        if (sub == '|') {
            comment_depth_ = 1;
            return j + 1;
        }
        if (sub == '\\') {
            // #\x, #\(, #\Space: one character, then any constituents.
            StartElement(col);
            size_t k = j + 2;
            while (k < n && !IsWs(text[k]) && !IsTerm(text[k])) k++;
            return k > n ? n : k;
        }
        if (sub == '(') {
            StartElement(col);
            pending_quote_ = false;
            OpenListAsData(col);
            return j + 1;
        }
        if (sub == '+' || sub == '-') {
            StartPrefixedElement(col);
            skip_feature_ = true;
            return j + 1;
        }
        if (sub == '\'' || sub == '.' || sub == 'P' || sub == 'S' || sub == 'A' || sub == 'C' || sub == '=') {
            StartPrefixedElement(col);
            if (sub == 'A' || sub == 'S' || sub == 'C') pending_quote_ = true;  // literal data
            return j + 1;
        }
        // #:sym, #x1F, #*101, #1#, and anything unknown: a token.
        StartElement(col);
        pending_quote_ = false;
        in_token_ = true;
        token_.clear();
        return ContinueToken(text, j + 1);
    }

    /** @brief A vector's list: always data, and whatever pending prefix opened it is consumed. */
    void OpenListAsData(int col) {
        pending_ = false;
        Frame f;
        f.open_col = col;
        f.open_line = line_;
        f.role = Role::Data;
        f.data_inherit = true;
        stack_.push_back(f);
    }

    size_t ContinueToken(const std::string &text, size_t i) {
        if (!in_token_) {
            in_token_ = true;
            token_.clear();
        }
        if (token_frame_ < 0) {
            token_frame_ = static_cast<int>(stack_.size());
            token_is_op_ = !stack_.empty() && stack_.back().elems == 1 && !skip_feature_;
        }
        const size_t n = text.size();
        while (i < n) {
            const char c = text[i];
            if (in_bar_) {
                if (c == '\\') {
                    i += 2;
                    continue;
                }
                if (c == '|') in_bar_ = false;
                token_ += c;
                i++;
                continue;
            }
            if (IsWs(c) || IsTerm(c)) {
                EndToken();
                return i;
            }
            if (c == '|') {
                in_bar_ = true;
                i++;
                continue;
            }
            if (c == '\\') {
                if (i + 1 < n) token_ += text[i + 1];
                i += 2;
                continue;
            }
            token_ += Up(c);
            i++;
        }
        return n;
    }

    void EndToken() {
        in_token_ = false;
        if (skip_feature_) {
            // A one-symbol feature expression (`#+sbcl`): done, and the
            // conditional form that follows is the counted element.
            skip_feature_ = false;
            pending_ = true;
        } else if (token_is_op_ && !stack_.empty() && token_frame_ == static_cast<int>(stack_.size())) {
            Frame &f = stack_.back();
            std::string name = token_;
            // A package prefix does not change how a form indents.
            const size_t colon = name.rfind(':');
            if (colon != std::string::npos) name = name.substr(colon + 1);
            f.op = name;
            f.op_is_symbol = !name.empty() && !(name[0] >= '0' && name[0] <= '9') && token_.rfind(':', 0) != 0;
            // A keyword in operator position (`(:export ...)`) is data
            // with a symbol in front, and indents as an ordinary call.
            if (token_.rfind(':', 0) == 0) f.op_is_symbol = true;
        }
        token_.clear();
        token_frame_ = -1;
        token_is_op_ = false;
    }
};

/** @brief Counts a line's leading blanks. */
int LeadingBlanks(const std::string &s) {
    int n = 0;
    while (static_cast<size_t>(n) < s.size() && (s[static_cast<size_t>(n)] == ' ' || s[static_cast<size_t>(n)] == '\t')) {
        n++;
    }
    return n;
}

}  // namespace

int LispIndentDistinguishedArgs(const std::string &name) {
    for (const SpecEntry &e : kSpecs) {
        if (name == e.name) return e.args;
    }
    if (StartsWith(name, "DEF")) return 1;
    if (StartsWith(name, "WITH-") || StartsWith(name, "DO-")) return 1;
    return -1;
}

int LispIndentForLine(const std::vector<std::string> &lines, int line) {
    if (line <= 0) return 0;
    Indenter ind;
    const size_t stop = static_cast<size_t>(line) < lines.size() ? static_cast<size_t>(line) : lines.size();
    for (size_t i = 0; i < stop; i++) ind.Scan(lines[i], static_cast<int>(i));
    if (ind.InText()) {
        // Typing on in a docstring or a block comment: keep the previous
        // line's margin.
        return LeadingBlanks(lines[stop - 1]);
    }
    return ind.Indent();
}

std::vector<std::string> LispReindent(const std::vector<std::string> &lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    Indenter ind;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string &src = lines[i];
        std::string result = src;
        if (!ind.InText()) {
            const int blanks = LeadingBlanks(src);
            const std::string body = src.substr(static_cast<size_t>(blanks));
            if (body.empty()) {
                result.clear();
            } else if (body[0] == ';' && !StartsWith(body, ";;")) {
                // A single-semicolon comment is a margin comment: its column is the author's.
            } else if (StartsWith(body, ";;;")) {
                // Emacs leaves `;;;` comment lines where they are too.
            } else {
                result = std::string(static_cast<size_t>(ind.Indent()), ' ') + body;
            }
        }
        ind.Scan(result, static_cast<int>(i));
        out.push_back(result);
    }
    return out;
}
