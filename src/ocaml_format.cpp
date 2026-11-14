// mep's OCaml formatter: see ocaml_format.h.
//
// Three passes over the token stream, none of which can change a token:
//
//   1. Spacing, line-local: one space around binary operators (`a+b` ->
//      `a + b`), one after `,` and `;`, none before them, runs of blanks
//      collapsed to one. A gap is only ever widened, collapsed to one blank,
//      or (before `,` / `;`, which can never merge with a neighbour) removed.
//   2. Indentation: a stack machine over the tokens (frames for `let`/`in`,
//      `match`/`with`/arms, `if`/`then`/`else`, `fun`, brackets, `begin`/
//      `struct`/`sig`/`object`/`end`, `type`, structure items...) gives each
//      line's indentation from the structure alone, never from the input's
//      indentation, so the result is stable.
//   3. Breaking: an over-long line gets a newline at the first applicable
//      place (after a binding's `=`, before `|>`s, after an arm's or a
//      function's `->`, before `else`, after `;`s); then 2 runs again. Lines
//      are never joined, so this terminates.
//
// The result is lexed again and compared with the input token by token, and
// formatted again to check that it is a fixed point; either failing refuses.
#include "ocaml_format.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace ocamlfmt {

namespace {

bool IsSymChar(char c) { return c != 0 && std::strchr("!$%&*+-./:<=>?@^|~", c) != nullptr; }
bool IsIdStart(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
bool IsIdChar(unsigned char c) { return IsIdStart(c) || (c >= '0' && c <= '9') || c == '\''; }
bool IsLower(unsigned char c) { return (c >= 'a' && c <= 'z') || c == '_'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

const std::set<std::string_view> &Keywords() {
    static const std::set<std::string_view> k = {
        "and", "as", "assert", "asr", "begin", "class", "constraint", "do", "done", "downto", "else", "end",
        "exception", "external", "for", "fun", "function", "functor", "if", "in", "include", "inherit",
        "initializer", "land", "lazy", "let", "lor", "lsl", "lsr", "lxor", "match", "method", "mod", "module",
        "mutable", "new", "nonrec", "object", "of", "open", "or", "private", "rec", "sig", "struct", "then", "to",
        "try", "type", "val", "virtual", "when", "while", "with"};
    return k;
}

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    bool Run(std::vector<Token> *out, std::string *error, int *error_line) {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\n') { Newline(); i_++; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f') { i_++; continue; }
            Token t;
            t.begin = i_;
            t.line = line_;
            t.col = static_cast<int>(i_ - line_start_);
            if (!LexOne(&t, error)) {
                *error_line = err_line_ + 1;
                return false;
            }
            t.end = i_;
            t.end_line = line_;
            t.text = s_.substr(t.begin, t.end - t.begin);
            out->push_back(t);
        }
        return true;
    }

private:
    std::string_view s_;
    size_t i_ = 0, line_start_ = 0;
    int line_ = 0, err_line_ = 0;

    char At(size_t k) const { return k < s_.size() ? s_[k] : '\0'; }
    void Newline() { line_++; line_start_ = i_ + 1; }
    // Advances over s_[i_] (tracking lines).
    void Step() {
        if (s_[i_] == '\n') Newline();
        i_++;
    }

    bool OnlyBlanksBefore() const {
        for (size_t k = line_start_; k < i_; k++)
            if (s_[k] != ' ' && s_[k] != '\t') return false;
        return true;
    }

    // "..." starting at i_ (on the quote). False when unterminated.
    bool SkipString() {
        Step();
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) { Step(); Step(); continue; }
            if (c == '"') { i_++; return true; }
            Step();
        }
        return false;
    }

    // {id|...|id} at i_ (on the brace), when it is one: the id's length (or -1).
    int QuotedId(size_t k) const {
        if (At(k) != '{') return -1;
        size_t j = k + 1;
        if (At(j) == '%') {  // {%ext|...|} and {%ext id|...|id}
            j++;
            if (At(j) == '%') j++;
            const size_t name = j;
            while (IsIdChar(static_cast<unsigned char>(At(j))) || At(j) == '.') j++;
            if (j == name) return -1;
            if (At(j) == ' ') {
                j++;
                while (IsLower(static_cast<unsigned char>(At(j)))) j++;
            }
            return At(j) == '|' ? static_cast<int>(j - k) : -1;
        }
        while (IsLower(static_cast<unsigned char>(At(j)))) j++;
        return At(j) == '|' ? static_cast<int>(j - k) : -1;
    }

    bool SkipQuoted() {
        // The delimiter id: after the last space (or after `{`) up to `|`.
        const size_t open = i_;
        while (s_[i_] != '|') i_++;
        size_t idb = i_;
        while (idb > open + 1 && IsLower(static_cast<unsigned char>(s_[idb - 1]))) idb--;
        if (idb > open + 1 && s_[idb - 1] != ' ' && s_[idb - 1] != '{') idb = i_;  // {%ext|: no id
        const std::string close = "|" + std::string(s_.substr(idb, i_ - idb)) + "}";
        i_++;
        while (i_ < s_.size()) {
            if (s_.compare(i_, close.size(), close) == 0) { i_ += close.size(); return true; }
            Step();
        }
        return false;
    }

    bool SkipComment() {
        int depth = 0;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '(' && At(i_ + 1) == '*') { depth++; i_ += 2; continue; }
            if (c == '*' && At(i_ + 1) == ')') {
                i_ += 2;
                if (--depth == 0) return true;
                continue;
            }
            if (c == '"') {
                const size_t save = i_, save_ls = line_start_;
                const int save_line = line_;
                if (!SkipString()) { i_ = save; line_start_ = save_ls; line_ = save_line; Step(); }
                continue;
            }
            if (c == '{' && QuotedId(i_) > 0) {
                const size_t save = i_, save_ls = line_start_;
                const int save_line = line_;
                if (!SkipQuoted()) { i_ = save; line_start_ = save_ls; line_ = save_line; Step(); }
                continue;
            }
            if (c == '\'' && At(i_ + 1) == '"' && At(i_ + 2) == '\'') { i_ += 3; continue; }
            if (c == '\'' && At(i_ + 1) == '\\' && At(i_ + 2) == '"' && At(i_ + 3) == '\'') { i_ += 4; continue; }
            Step();
        }
        return false;
    }

    // A char literal at i_ (on the quote): its length, or 0 when this is a type variable / quote.
    size_t CharLen() const {
        if (At(i_ + 1) == '\\') {
            // Past the escaped character itself: '\'' and '\\' close on the next quote.
            for (size_t k = i_ + 3; k < i_ + 14 && k < s_.size(); k++) {
                if (s_[k] == '\'') return k + 1 - i_;
                if (s_[k] == '\n') return 0;
            }
            return 0;
        }
        if (At(i_ + 1) != '\0' && At(i_ + 1) != '\n' && At(i_ + 2) == '\'') return 3;
        // A UTF-8 character between quotes ('é' is not OCaml, but lex it as one token).
        const unsigned char b = static_cast<unsigned char>(At(i_ + 1));
        if (b >= 0xC0) {
            size_t n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            if (At(i_ + 1 + n) == '\'') return n + 2;
        }
        return 0;
    }

    bool LexOne(Token *t, std::string *error) {
        const char c = s_[i_];
        const char n = At(i_ + 1);
        err_line_ = line_;
        if (c == '(' && n == '*') {
            t->kind = TokKind::Comment;
            if (!SkipComment()) { *error = "unterminated comment"; return false; }
            return true;
        }
        if (c == '"') {
            t->kind = TokKind::String;
            if (!SkipString()) { *error = "unterminated string"; return false; }
            return true;
        }
        if (c == '{' && QuotedId(i_) > 0) {
            t->kind = TokKind::QString;
            if (!SkipQuoted()) { *error = "unterminated quoted string"; return false; }
            return true;
        }
        if (c == '#' && i_ == line_start_ && (std::isalpha(static_cast<unsigned char>(n)) || IsDigit(n) ||
                                             (n == ' ' && IsDigit(At(i_ + 2))))) {
            t->kind = TokKind::Directive;
            while (i_ < s_.size() && s_[i_] != '\n') i_++;
            while (i_ > t->begin && (s_[i_ - 1] == ' ' || s_[i_ - 1] == '\t' || s_[i_ - 1] == '\r')) i_--;
            return true;
        }
        if (c == '\'') {
            if (const size_t len = CharLen()) {
                t->kind = TokKind::Char;
                i_ += len;
                return true;
            }
            if (IsIdStart(static_cast<unsigned char>(n))) {
                t->kind = TokKind::TyVar;
                i_++;
                while (IsIdChar(static_cast<unsigned char>(At(i_)))) i_++;
                return true;
            }
            t->kind = TokKind::Punct;
            i_++;
            return true;
        }
        if (c == '`' && IsIdStart(static_cast<unsigned char>(n))) {
            t->kind = TokKind::Tag;
            i_++;
            while (IsIdChar(static_cast<unsigned char>(At(i_)))) i_++;
            return true;
        }
        if (IsDigit(c)) {
            t->kind = TokKind::Number;
            if (c == '0' && (n == 'x' || n == 'X' || n == 'o' || n == 'O' || n == 'b' || n == 'B')) {
                i_ += 2;
                while (std::isxdigit(static_cast<unsigned char>(At(i_))) || At(i_) == '_') i_++;
                if (At(i_) == '.' ) { i_++; while (std::isxdigit(static_cast<unsigned char>(At(i_))) || At(i_) == '_') i_++; }
                if ((At(i_) == 'p' || At(i_) == 'P') && (IsDigit(At(i_ + 1)) || ((At(i_ + 1) == '+' || At(i_ + 1) == '-') && IsDigit(At(i_ + 2))))) {
                    i_ += 2;
                    while (IsDigit(At(i_)) || At(i_) == '_') i_++;
                }
            } else {
                while (IsDigit(At(i_)) || At(i_) == '_') i_++;
                if (At(i_) == '.') {
                    i_++;
                    while (IsDigit(At(i_)) || At(i_) == '_') i_++;
                }
                if ((At(i_) == 'e' || At(i_) == 'E') && (IsDigit(At(i_ + 1)) || ((At(i_ + 1) == '+' || At(i_ + 1) == '-') && IsDigit(At(i_ + 2))))) {
                    i_ += 2;
                    while (IsDigit(At(i_)) || At(i_) == '_') i_++;
                }
            }
            if (std::isalpha(static_cast<unsigned char>(At(i_)))) i_++;  // l, L, n, custom suffixes
            return true;
        }
        if (IsIdStart(static_cast<unsigned char>(c))) {
            while (IsIdChar(static_cast<unsigned char>(At(i_)))) i_++;
            const std::string_view word = s_.substr(t->begin, i_ - t->begin);
            if (word == "true" || word == "false") {
                t->kind = TokKind::Ident;
            } else if (Keywords().count(word)) {
                t->kind = TokKind::Keyword;
                // Binding operators: let* let+ and* ...
                if ((word == "let" || word == "and") && At(i_) != '\0' && std::strchr("$&*+-/<=>@^|", At(i_))) {
                    while (IsSymChar(At(i_)) && At(i_) != '.' ) i_++;
                }
            } else {
                t->kind = (c >= 'A' && c <= 'Z') ? TokKind::UIdent : TokKind::Ident;
            }
            return true;
        }
        if ((c == '~' || c == '?') && IsLower(static_cast<unsigned char>(n))) {
            t->kind = TokKind::Label;
            i_++;
            while (IsIdChar(static_cast<unsigned char>(At(i_)))) i_++;
            if (At(i_) == ':') i_++;  // OCaml's LABEL / OPTLABEL token takes the colon whatever follows
            return true;
        }
        // Punctuation.
        static const char *const kPunct[] = {"[@@@", "[@@", "[%%", "[@", "[%", "[|", "[<", "[>", "{<", ">}", "|]",
                                             ";;", "(", ")", "[", "]", "{", "}", ",", ";"};
        for (const char *p : kPunct) {
            const size_t len = std::strlen(p);
            if (s_.compare(i_, len, p) == 0) {
                t->kind = TokKind::Punct;
                i_ += len;
                return true;
            }
        }
        if (IsSymChar(c) || c == '#') {
            t->kind = TokKind::Op;
            if (c == ':') {
                i_ += (n == ':' || n == '=' || n == '>') ? 2 : 1;
                return true;
            }
            if (c == '.') {
                if (n == '.') { i_ += 2; return true; }
                i_++;
                if (IsSymChar(n) || n == '#') while (IsSymChar(At(i_)) || At(i_) == '#') i_++;  // .%{ .+( dot operators
                return true;
            }
            // Maximal munch, as OCaml's lexer does (`+.`, `|>`, `>>=`, `##`): a boundary inside
            // one of its operators would let the spacing pass split it.
            i_++;
            while (IsSymChar(At(i_)) || At(i_) == '#') i_++;
            return true;
        }
        // Anything else (stray bytes): one token per byte.
        t->kind = TokKind::Punct;
        i_++;
        return true;
    }
};

// --- token classes ---------------------------------------------------------------------

bool IsKw(const Token &t, std::string_view w) { return t.kind == TokKind::Keyword && t.text == w; }
bool IsP(const Token &t, std::string_view w) { return (t.kind == TokKind::Punct || t.kind == TokKind::Op) && t.text == w; }
bool IsLetKw(const Token &t) { return t.kind == TokKind::Keyword && t.text.substr(0, 3) == "let"; }
bool IsAndKw(const Token &t) { return t.kind == TokKind::Keyword && t.text.substr(0, 3) == "and"; }

bool IsOperandEnd(const Token &t) {
    switch (t.kind) {
    case TokKind::Ident: case TokKind::UIdent: case TokKind::Number: case TokKind::Char: case TokKind::String:
    case TokKind::QString: case TokKind::TyVar: case TokKind::Tag:
        return true;
    case TokKind::Label:
        return t.text.back() != ':';
    case TokKind::Punct:
        return t.text == ")" || t.text == "]" || t.text == "}" || t.text == "|]" || t.text == ">}";
    case TokKind::Keyword:
        return t.text == "end" || t.text == "done";
    case TokKind::Op:
        return t.text == "..";  // `type t = ..`
    default:
        return false;
    }
}

// A binary operator symbol (spaced on both sides when it follows an operand).
bool IsBinaryOp(const Token &t) {
    if (t.kind != TokKind::Op) return false;
    const char c = t.text[0];
    if (c == '!') return t.text == "!=";
    if (c == '~' || c == '?' || c == '.' || c == '#') return false;
    return true;
}

bool IsInfixAtLineStart(const Token &t) {
    if (IsBinaryOp(t)) return true;
    return t.kind == TokKind::Keyword && (t.text == "mod" || t.text == "land" || t.text == "lor" || t.text == "lxor" ||
                                         t.text == "lsl" || t.text == "lsr" || t.text == "asr" || t.text == "or");
}

const char *CloserFor(std::string_view open) {
    if (open == "(") return ")";
    if (open == "{") return "}";
    if (open == "[|") return "|]";
    if (open == "{<") return ">}";
    if (open == "[" || open == "[<" || open == "[>" || open[0] == '[') return "]";
    return nullptr;
}
bool IsOpenBracket(const Token &t) { return t.kind == TokKind::Punct && CloserFor(t.text) != nullptr; }
bool IsCloseBracket(const Token &t) {
    return t.kind == TokKind::Punct && (t.text == ")" || t.text == "]" || t.text == "}" || t.text == "|]" || t.text == ">}");
}
bool IsBlockOpen(const Token &t) {
    return t.kind == TokKind::Keyword && (t.text == "begin" || t.text == "struct" || t.text == "sig" || t.text == "object");
}
bool IsItemKeyword(const Token &t) {
    if (t.kind != TokKind::Keyword) return false;
    if (IsLetKw(t)) return true;
    static const std::set<std::string_view> k = {"type", "module", "open", "include", "exception", "external", "val",
                                                 "class", "method", "inherit", "initializer"};
    return k.count(t.text) > 0;
}

// UTF-8 display width (code points).
int Width(std::string_view s) {
    int w = 0;
    for (char ch : s)
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) w++;
    return w;
}

// --- the layout ---------------------------------------------------------------------------

enum class FK { Top, Bracket, Block, Let, LetBody, LetIn, Assign, Type, TypeBody, Item, Match, Arms, ArmBody, Fun, FunBody, If, Then, Else, Loop, Do };

struct Frame {
    FK k = FK::Top;
    int base = 0;      // where its closing keyword / `|` / `and` lines up
    int inner = 0;     // indentation of its content lines
    int cont = 0;      // indentation of a continued application inside it
    int open_col = 0;  // brackets: the opener's column
    bool has_content = false;
    bool aligned = false;  // brackets: content on the opener's line sets `inner` to its column
    bool same_line = false;  // brackets: the content started on the opener's line
    bool bar = false;        // arms: the current arm started with `|`; type bodies: a `|` was seen
    int bar_col = 0;         // type bodies: the last constructor's `|` column
    std::string closer;    // brackets: the closing token
};

struct Line {
    std::vector<int> toks;  // token indices
    int blank_before = 0;   // blank lines before it (already collapsed to at most one)
    int indent = 0;         // output indentation
    bool keep = false;      // directive: original indentation kept
    int orig_indent = 0;
};

class Layout {
public:
    Layout(std::string_view src, const std::vector<Token> &toks, const Options &opts)
        : src_(src), t_(toks), opts_(opts), step_(opts.indent > 0 ? opts.indent : 2) {}

    // Lays out with the given forced breaks; false (with error) on unbalanced structure.
    bool Run(const std::vector<bool> &break_before, std::string *error, int *error_line) {
        BuildLines(break_before);
        ComputeGaps();
        return ComputeIndents(error, error_line);
    }

    std::string Render() const;
    /** @brief Breaks to add for over-long lines (indices of tokens to start a new line). */
    std::vector<int> Breaks() const;

    // Per-token marks recorded by ComputeIndents.
    std::vector<bool> binding_eq_, body_arrow_;

private:
    std::string_view src_;
    const std::vector<Token> &t_;
    Options opts_;
    int step_;
    std::vector<Line> lines_;
    std::vector<int> line_of_;       // token -> line
    std::vector<std::string> gap_;   // token -> output gap before it (same line only)
    std::vector<int> col_;           // token -> output column

    void BuildLines(const std::vector<bool> &break_before);
    void ComputeGaps();
    bool BracketOpenedSpaced(size_t k) const;
    bool ComputeIndents(std::string *error, int *error_line);
    int LineWidth(const Line &l) const;
    bool HasMultiline(const Line &l) const {
        for (int k : l.toks)
            if (t_[static_cast<size_t>(k)].end_line != t_[static_cast<size_t>(k)].line) return true;
        return false;
    }
    int EndCol(int k) const;  // output column just past token k (on its last line)
};

void Layout::BuildLines(const std::vector<bool> &break_before) {
    lines_.clear();
    line_of_.assign(t_.size(), 0);
    for (size_t k = 0; k < t_.size(); k++) {
        int newlines = 0;
        if (k == 0) {
            newlines = 1;
        } else {
            for (size_t b = t_[k - 1].end; b < t_[k].begin; b++)
                if (src_[b] == '\n') newlines++;
            if (newlines == 0 && break_before[k]) newlines = 1;
        }
        if (newlines > 0) {
            Line l;
            l.blank_before = (k == 0) ? 0 : std::min(newlines - 1, 1);
            l.orig_indent = t_[k].col;
            l.keep = t_[k].kind == TokKind::Directive;
            lines_.push_back(l);
        }
        lines_.back().toks.push_back(static_cast<int>(k));
        line_of_[k] = static_cast<int>(lines_.size()) - 1;
    }
}

// ocamlformat pads lists, arrays and records inside their brackets; not `a.{i}` / `M.[x]`
// access, not attributes or extensions.
bool SpacedOpen(const Token &t) {
    return t.kind == TokKind::Punct && (t.text == "[" || t.text == "[|" || t.text == "{" || t.text == "{<" ||
                                        t.text == "[<" || t.text == "[>");
}
bool SpacedClose(const Token &t) {
    return t.kind == TokKind::Punct && (t.text == "]" || t.text == "|]" || t.text == "}" || t.text == ">}");
}
bool IsDotted(const Token &t) { return t.kind == TokKind::Op && t.text[0] == '.' && t.text != ".."; }

// Whether the bracket closed by token k was opened by a padded opener (not `.{`, `[@`, `[%`).
bool Layout::BracketOpenedSpaced(size_t k) const {
    int depth = 0;
    for (size_t i = k; i-- > 0;) {
        const Token &t = t_[i];
        if (IsCloseBracket(t)) depth++;
        if (IsOpenBracket(t)) {
            if (depth == 0) return SpacedOpen(t) && !(i > 0 && IsDotted(t_[i - 1]));
            depth--;
        }
    }
    return false;
}

void Layout::ComputeGaps() {
    gap_.assign(t_.size(), std::string());
    for (const Line &l : lines_) {
        for (size_t j = 1; j < l.toks.size(); j++) {
            const size_t k = static_cast<size_t>(l.toks[j]);
            const Token &prev = t_[k - 1], &cur = t_[k];
            std::string orig(src_.substr(prev.end, cur.begin - prev.end));
            if (orig.find('\n') != std::string::npos) orig = " ";  // a forced break that was undone: never
            std::string g = orig;
            const bool cur_sep = IsP(cur, ",") || IsP(cur, ";");
            if (cur.kind == TokKind::Comment) {
                g = orig;  // trailing comments keep their spacing
            } else if (cur_sep && prev.kind != TokKind::Comment) {
                g.clear();
            } else if ((IsBinaryOp(cur) && IsOperandEnd(prev) && j + 1 < l.toks.size()) ||
                       (IsBinaryOp(prev) && k >= 2 && line_of_[k - 2] == line_of_[k - 1] && IsOperandEnd(t_[k - 2]))) {
                // `x:int` (a labelled argument type) keeps its tight colon.
                const bool tight_colon = (cur.text == ":" && orig.empty() &&
                                          src_.substr(cur.end, 1) != " ") ||
                                         (prev.text == ":" && orig.empty() && k >= 2 &&
                                          t_[k - 1].begin == t_[k - 2].end);
                g = tight_colon ? orig : " ";
            } else if (SpacedOpen(prev) && !(k >= 2 && line_of_[k - 2] == line_of_[k - 1] && IsDotted(t_[k - 2])) &&
                       !IsCloseBracket(cur)) {
                g = " ";  // `[ 1; 2 ]`, `{ x = 1 }`, `[| a |]`
            } else if (SpacedClose(cur) && !IsOpenBracket(prev) && BracketOpenedSpaced(k)) {
                g = " ";
            } else if ((IsP(prev, ",") || IsP(prev, ";")) && !IsP(cur, ";")) {
                g = " ";
            } else if (!orig.empty()) {
                g = " ";
            }
            gap_[k] = g;
        }
    }
}

int Layout::EndCol(int k) const {
    const Token &t = t_[static_cast<size_t>(k)];
    if (t.end_line == t.line) return col_[static_cast<size_t>(k)] + Width(t.text);
    // The last line of a multi-line token, shifted with it when it is a comment.
    const size_t nl = t.text.rfind('\n');
    int w = Width(t.text.substr(nl + 1));
    if (t.kind == TokKind::Comment) w += col_[static_cast<size_t>(k)] - t.col;
    return std::max(w, 0);
}

bool Layout::ComputeIndents(std::string *error, int *error_line) {
    col_.assign(t_.size(), 0);
    binding_eq_.assign(t_.size(), false);
    body_arrow_.assign(t_.size(), false);
    std::vector<Frame> st;
    Frame top;
    top.k = FK::Top;
    st.push_back(top);
    const Token *prev = nullptr;  // last code token
    int prev_index = -1;
    std::vector<size_t> pending;  // comment-only lines waiting for the next code line's indent
    std::vector<int> pending_attach;  // their indent if a constructor's `|` comes next (else -1)

    auto fail = [&](const Token &t, const std::string &msg) {
        *error = msg;
        *error_line = t.line + 1;
        return false;
    };
    auto block_index = [&]() {
        for (size_t i = st.size(); i-- > 0;)
            if (st[i].k == FK::Block || st[i].k == FK::Top) return i;
        return static_cast<size_t>(0);
    };
    // Finds the nearest frame satisfying pred, not crossing a bracket/block (unless pred accepts it).
    auto find = [&](auto pred) -> int {
        for (size_t i = st.size(); i-- > 0;) {
            if (pred(st[i])) return static_cast<int>(i);
            if (st[i].k == FK::Bracket || st[i].k == FK::Block || st[i].k == FK::Top) return -1;
        }
        return -1;
    };
    auto pop_to = [&](int i) { st.resize(static_cast<size_t>(i) + 1); };
    auto is_new_item = [&]() {
        return prev == nullptr || IsOperandEnd(*prev) || IsP(*prev, ";;") || IsBlockOpen(*prev);
    };
    auto match_closer = [&](const Token &t) -> int {
        for (size_t i = st.size(); i-- > 1;) {
            if (st[i].k == FK::Bracket) return st[i].closer == t.text ? static_cast<int>(i) : -2;
            if (st[i].k == FK::Block) return -2;
        }
        return -1;
    };
    auto match_end = [&]() -> int {
        for (size_t i = st.size(); i-- > 1;) {
            if (st[i].k == FK::Block) return static_cast<int>(i);
            if (st[i].k == FK::Bracket) return -2;
        }
        return -1;
    };

    // The indentation of a line whose first code token is t (frames not yet updated for t).
    auto indent_for = [&](const Token &t) -> int {
        const Frame &f = st.back();
        if (IsCloseBracket(t)) {
            const int i = match_closer(t);
            return i >= 0 ? st[static_cast<size_t>(i)].base : f.inner;
        }
        if (IsKw(t, "end")) {
            const int i = match_end();
            return i >= 0 ? st[static_cast<size_t>(i)].base : f.inner;
        }
        if (IsKw(t, "done")) {
            const int i = find([](const Frame &x) { return x.k == FK::Do; });
            return i >= 0 ? st[static_cast<size_t>(i)].base : f.inner;
        }
        if (IsKw(t, "in")) {
            const int i = find([](const Frame &x) { return x.k == FK::Let || x.k == FK::LetBody; });
            return i >= 0 ? st[static_cast<size_t>(i)].base : f.inner;
        }
        if (IsKw(t, "then")) {
            const int i = find([](const Frame &x) { return x.k == FK::If; });
            return i >= 0 ? st[static_cast<size_t>(i)].base : f.inner;
        }
        if (IsKw(t, "else")) {
            const int i = find([](const Frame &x) { return x.k == FK::Then || x.k == FK::If; });
            return i >= 0 ? st[static_cast<size_t>(i)].base : f.inner;
        }
        if (IsKw(t, "with")) {
            const int i = find([](const Frame &x) { return x.k == FK::Match; });
            if (i >= 0) return st[static_cast<size_t>(i)].base;
        }
        if (IsKw(t, "do")) {
            const int i = find([](const Frame &x) { return x.k == FK::Loop; });
            if (i >= 0) return st[static_cast<size_t>(i)].base;
        }
        if (IsAndKw(t)) {
            const int i = find([](const Frame &x) {
                return x.k == FK::Let || x.k == FK::LetBody || x.k == FK::Type || x.k == FK::TypeBody;
            });
            if (i >= 0) return st[static_cast<size_t>(i)].base;
        }
        if (IsP(t, "|")) {
            const int i = find([](const Frame &x) { return x.k == FK::Arms || x.k == FK::TypeBody; });
            if (i >= 0) {
                const Frame &x = st[static_cast<size_t>(i)];
                return x.k == FK::Arms ? x.base : x.inner;
            }
            if (f.k == FK::Bracket) return f.open_col;
        }
        if (IsP(t, ";;") || IsP(t, "[@@@")) return st[block_index()].inner;
        if (IsP(t, "[@@")) {
            // An item attribute lines up with its item.
            const int i = find([](const Frame &x) {
                return x.k == FK::Let || x.k == FK::LetBody || x.k == FK::Type || x.k == FK::TypeBody || x.k == FK::Item;
            });
            return i >= 0 ? st[static_cast<size_t>(i)].base : st[block_index()].inner;
        }
        if (IsItemKeyword(t) && is_new_item()) return st[block_index()].inner;
        if (f.k == FK::Bracket && (IsP(t, ";") || IsP(t, ","))) return f.open_col;
        // A GADT constructor's type continues four in from its `|`.
        if (f.k == FK::TypeBody && f.bar) return f.bar_col + 2 * step_;
        if (IsInfixAtLineStart(t) && prev && IsOperandEnd(*prev))
            return (f.k == FK::Bracket && f.same_line) ? f.open_col : f.inner;
        if (prev && (IsOperandEnd(*prev) || IsKw(*prev, "lazy") || IsKw(*prev, "assert")) && f.has_content) return f.cont;
        return f.inner;
    };

    for (size_t li = 0; li < lines_.size(); li++) {
        Line &line = lines_[li];
        const Token *first_code = nullptr;
        for (int k : line.toks)
            if (t_[static_cast<size_t>(k)].kind != TokKind::Comment) { first_code = &t_[static_cast<size_t>(k)]; break; }
        if (line.keep) {
            line.indent = line.orig_indent;
        } else if (first_code == nullptr && line.blank_before == 0 && li > 0 && lines_[li - 1].indent >= 0 &&
                   !lines_[li - 1].keep && st.back().k == FK::TypeBody &&
                   IsP(t_[static_cast<size_t>(lines_[li - 1].toks[0])], "|")) {
            // A comment right under a constructor documents it: ocamlformat's place for a doc
            // comment, or for any comment inside a constructor's type (after `:`, `->`, `*`); a
            // plain comment only when another constructor follows.
            const bool doc = t_[static_cast<size_t>(line.toks[0])].text.substr(0, 3) == "(**";
            const int bar_indent = lines_[li - 1].indent;
            if (doc || (prev && !IsOperandEnd(*prev))) {
                line.indent = bar_indent + 2 * step_;
            } else {
                pending.push_back(li);
                pending_attach.push_back(bar_indent + step_);
                line.indent = -1;
            }
        } else if (first_code == nullptr) {
            // Decided by the next code line.
            pending.push_back(li);
            pending_attach.push_back(-1);
            line.indent = -1;
        } else {
            const Token &first = *first_code;
            line.indent = std::max(0, indent_for(first));
            // Comment lines before this one take its indentation, unless it closes something:
            // then the indentation of what it closes.
            int closed_inner = -1;
            if (IsCloseBracket(first)) {
                const int i = match_closer(first);
                if (i >= 0) closed_inner = st[static_cast<size_t>(i)].inner;
            } else if (IsKw(first, "end")) {
                const int i = match_end();
                if (i >= 0) closed_inner = st[static_cast<size_t>(i)].inner;
            } else if (IsKw(first, "done")) {
                const int i = find([](const Frame &x) { return x.k == FK::Do; });
                if (i >= 0) closed_inner = st[static_cast<size_t>(i)].inner;
            }
            for (size_t p = 0; p < pending.size(); p++)
                lines_[pending[p]].indent = (pending_attach[p] >= 0 && IsP(first, "|")) ? pending_attach[p]
                                            : closed_inner >= 0                          ? closed_inner
                                                                                         : line.indent;
            pending.clear();
            pending_attach.clear();
        }
        // Walk the line's tokens: columns, then the frame machine.
        int col = line.indent;
        const int line_indent = line.keep ? line.orig_indent : (line.indent >= 0 ? line.indent : 0);
        for (size_t j = 0; j < line.toks.size(); j++) {
            const int k = line.toks[j];
            const Token &t = t_[static_cast<size_t>(k)];
            if (j > 0) col = EndCol(line.toks[j - 1]) + static_cast<int>(gap_[static_cast<size_t>(k)].size());
            col_[static_cast<size_t>(k)] = col;
            if (line.indent < 0) {
                // A comment-only line: its indent is not known yet; columns are fixed up below.
                continue;
            }
            if (t.kind == TokKind::Comment || t.kind == TokKind::Directive) continue;
            const bool first_on_line = (j == 0) || [&]() {
                for (size_t q = 0; q < j; q++)
                    if (t_[static_cast<size_t>(line.toks[q])].kind != TokKind::Comment) return false;
                return true;
            }();

            // 1. Tokens that close or switch frames.
            if (IsCloseBracket(t)) {
                const int i = match_closer(t);
                if (i < 0) return fail(t, "unbalanced `" + std::string(t.text) + "`");
                pop_to(i - 1);
                prev = &t; prev_index = k;
                continue;
            }
            if (IsKw(t, "end")) {
                const int i = match_end();
                if (i < 0) return fail(t, "`end` without a matching `begin`/`struct`/`sig`/`object`");
                pop_to(i - 1);
                prev = &t; prev_index = k;
                continue;
            }
            if (IsKw(t, "done")) {
                const int i = find([](const Frame &x) { return x.k == FK::Do; });
                if (i >= 0) pop_to(i - 1);
                prev = &t; prev_index = k;
                continue;
            }
            if (IsKw(t, "in")) {
                const int i = find([](const Frame &x) { return x.k == FK::Let || x.k == FK::LetBody; });
                if (i >= 0) {
                    // The body after `in` reaches as far as it can (`let ... in a; b` is one
                    // body): a LetIn frame keeps `;` from closing an enclosing `then`/`else`.
                    const int base = st[static_cast<size_t>(i)].base;
                    pop_to(i - 1);
                    if (!(st.back().k == FK::LetIn && st.back().base == base)) {
                        Frame n;
                        n.k = FK::LetIn;
                        n.base = base;
                        n.inner = base;
                        st.push_back(n);
                    }
                }
                prev = &t; prev_index = k;
                continue;
            }
            if (IsP(t, ";;")) {
                pop_to(static_cast<int>(block_index()));
                prev = &t; prev_index = k;
                continue;
            }
            if (IsKw(t, "then")) {
                const int i = find([](const Frame &x) { return x.k == FK::If; });
                if (i >= 0) {
                    pop_to(i);
                    st.back().k = FK::Then;
                    st.back().inner = st.back().base + step_;
                    st.back().has_content = false;
                }
                prev = &t; prev_index = k;
                continue;
            }
            if (IsKw(t, "else")) {
                const int i = find([](const Frame &x) { return x.k == FK::Then || x.k == FK::If; });
                if (i >= 0) {
                    pop_to(i);
                    st.back().k = FK::Else;
                    st.back().inner = st.back().base + step_;
                    st.back().has_content = false;
                }
                prev = &t; prev_index = k;
                continue;
            }
            if (IsKw(t, "with")) {
                const int i = find([](const Frame &x) { return x.k == FK::Match; });
                if (i >= 0) {
                    pop_to(i);
                    Frame &f = st.back();
                    f.k = FK::Arms;
                    f.inner = f.base;
                    f.has_content = false;
                    prev = &t; prev_index = k;
                    continue;
                }
            }
            if (IsKw(t, "do")) {
                const int i = find([](const Frame &x) { return x.k == FK::Loop; });
                if (i >= 0) {
                    pop_to(i);
                    st.back().k = FK::Do;
                    st.back().inner = st.back().base + step_;
                    st.back().has_content = false;
                    prev = &t; prev_index = k;
                    continue;
                }
            }
            if (IsAndKw(t)) {
                const int i = find([](const Frame &x) {
                    return x.k == FK::Let || x.k == FK::LetBody || x.k == FK::Type || x.k == FK::TypeBody;
                });
                if (i >= 0) {
                    pop_to(i);
                    Frame &f = st.back();
                    f.k = (f.k == FK::TypeBody || f.k == FK::Type) ? FK::Type : FK::Let;
                    f.inner = f.base + step_;
                    f.has_content = false;
                    f.bar = false;
                    prev = &t; prev_index = k;
                    continue;
                }
            }
            if (IsP(t, ";")) {
                while (st.back().k == FK::If || st.back().k == FK::Then || st.back().k == FK::Else ||
                       st.back().k == FK::Assign)
                    st.pop_back();
                prev = &t; prev_index = k;
                continue;
            }
            if (IsP(t, "|")) {
                const int i = find([](const Frame &x) { return x.k == FK::Arms; });
                if (i >= 0 && st.back().k != FK::Arms) {
                    // A new arm: close the previous arm's body.
                    bool through_body = false;
                    for (size_t q = static_cast<size_t>(i) + 1; q < st.size(); q++)
                        if (st[q].k == FK::ArmBody) through_body = true;
                    if (through_body) pop_to(i);
                }
            }
            if ((IsItemKeyword(t) || IsP(t, "[@@@")) && is_new_item()) pop_to(static_cast<int>(block_index()));
            if (IsP(t, "|") && st.back().k == FK::Arms) st.back().bar = true;
            if (IsP(t, "|") && st.back().k == FK::TypeBody) {
                st.back().bar = true;
                st.back().bar_col = col;
            }

            // 2. Content of the enclosing frame.
            Frame &f = st.back();
            if (!f.has_content) {
                f.has_content = true;
                if (first_on_line) {
                    f.cont = f.inner + step_;
                } else if (f.aligned) {
                    f.inner = col;
                    f.cont = col + step_;
                    f.same_line = true;
                } else {
                    f.cont = f.inner;
                }
            }
            const int anchor = first_on_line ? col : st.back().inner;

            // 3. Openers.
            auto push = [&](FK kind, int base, int inner) {
                Frame n;
                n.k = kind;
                n.base = base;
                n.inner = inner;
                st.push_back(n);
            };
            if (IsOpenBracket(t)) {
                Frame n;
                n.k = FK::Bracket;
                n.base = line_indent;
                n.inner = line_indent + step_;
                // `| p -> (` : the parenthesised body sits where the arm's body would.
                if (st.back().k == FK::ArmBody && prev && IsP(*prev, "->")) n.inner = st.back().inner;
                n.open_col = col;
                n.aligned = true;
                n.closer = CloserFor(t.text);
                st.push_back(n);
            } else if (IsBlockOpen(t)) {
                push(FK::Block, line_indent, line_indent + step_);
            } else if (IsLetKw(t) || IsKw(t, "method") || IsKw(t, "class")) {
                // A binding's head continues four columns in (`let f a b\n    c =`).
                push(FK::Let, anchor, anchor + 2 * step_);
            } else if (IsKw(t, "module")) {
                // `let module M = ...` is the `let`'s; `(module M : S)` is a value.
                if (!(prev && (IsLetKw(*prev) || IsP(*prev, "(") || IsP(*prev, ":") || IsP(*prev, "["))))
                    push(FK::Let, anchor, anchor + 2 * step_);
            } else if (IsKw(t, "type") && prev && (IsKw(*prev, "module") || IsKw(*prev, "class"))) {
                // `module type S = ...`: part of the module's frame.
            } else if (IsKw(t, "type") && (is_new_item() || st.back().k == FK::Block || st.back().k == FK::Top)) {
                push(FK::Type, anchor, anchor + step_);
            } else if (IsItemKeyword(t) && (is_new_item() || st.back().k == FK::Block || st.back().k == FK::Top) &&
                       !(prev && IsLetKw(*prev))) {
                push(FK::Item, anchor, anchor + step_);
            } else if ((IsP(t, ":=") || IsP(t, "<-")) && st.back().k != FK::Type) {
                // `r :=\n  value`: the value two in from the assignment's line, up to the `;`.
                push(FK::Assign, line_indent, line_indent + step_);
            } else if (IsP(t, "=") || IsP(t, "+=") || IsP(t, ":=")) {
                Frame &g = st.back();
                if ((g.k == FK::Let && t.text == "=") || g.k == FK::Type) {
                    g.k = g.k == FK::Type ? FK::TypeBody : FK::LetBody;
                    g.inner = g.base + step_;
                    g.has_content = false;
                    g.bar = false;
                    binding_eq_[static_cast<size_t>(k)] = true;
                }
            } else if (IsKw(t, "match") || IsKw(t, "try")) {
                // `begin match x with` lines its arms (and `with`) up with the `begin`.
                const int base = (!first_on_line && prev && IsKw(*prev, "begin") && st.back().k == FK::Block)
                                     ? st.back().base : anchor;
                push(FK::Match, base, base + step_);
                if (IsKw(t, "match")) st.back().bar = false;
            } else if (IsKw(t, "function")) {
                // `(function` puts its arms two in from the parenthesis.
                const int base = (!first_on_line && prev && IsP(*prev, "(") && st.back().k == FK::Bracket)
                                     ? st.back().open_col + step_ : anchor;
                push(FK::Arms, base, base);
            } else if (IsKw(t, "fun")) {
                push(FK::Fun, anchor, anchor + step_);
            } else if (IsKw(t, "if")) {
                if (prev && IsKw(*prev, "else") && st.back().k == FK::Else && !first_on_line) {
                    const int base = st.back().base;
                    st.pop_back();
                    push(FK::If, base, base + step_);
                } else {
                    // `begin if c then` lines its `else` up with the `begin`.
                    const int base = (!first_on_line && prev && IsKw(*prev, "begin") && st.back().k == FK::Block)
                                         ? st.back().base : anchor;
                    push(FK::If, base, base + step_);
                }
            } else if (IsKw(t, "for") || IsKw(t, "while")) {
                push(FK::Loop, anchor, anchor + step_);
            } else if (IsP(t, "->")) {
                Frame &g = st.back();
                if (g.k == FK::Arms) {
                    // `| P ->` bodies go four in from the bar; a first arm written without
                    // one (`with Not_found ->`) two in from its line.
                    const int body = g.bar ? g.base + 2 * step_ : line_indent + step_;
                    g.bar = false;
                    push(FK::ArmBody, g.base, body);
                    body_arrow_[static_cast<size_t>(k)] = true;
                } else if (g.k == FK::Fun) {
                    g.k = FK::FunBody;
                    g.inner = line_indent + step_;
                    g.has_content = false;
                    body_arrow_[static_cast<size_t>(k)] = true;
                }
            }
            prev = &t;
            prev_index = k;
        }
    }
    (void)prev_index;
    // Trailing comment lines: the enclosing block's indentation.
    for (size_t p = 0; p < pending.size(); p++) lines_[pending[p]].indent = st[block_index()].inner;
    for (size_t i = 1; i < st.size(); i++) {
        if (st[i].k == FK::Bracket || st[i].k == FK::Block) {
            *error = st[i].k == FK::Bracket ? "unclosed `" + std::string(1, '(') + "`-style bracket (expected `" +
                                                  st[i].closer + "`)"
                                            : "unclosed `begin`/`struct`/`sig`/`object` (expected `end`)";
            *error_line = 0;
            return false;
        }
    }
    // Columns of the comment-only lines, now that their indentation is known.
    for (Line &line : lines_) {
        if (line.indent < 0) line.indent = 0;
        int col = line.indent;
        for (size_t j = 0; j < line.toks.size(); j++) {
            const int k = line.toks[j];
            if (j > 0) col = EndCol(line.toks[j - 1]) + static_cast<int>(gap_[static_cast<size_t>(k)].size());
            col_[static_cast<size_t>(k)] = col;
        }
    }
    return true;
}

std::string Layout::Render() const {
    std::string out;
    for (size_t li = 0; li < lines_.size(); li++) {
        const Line &line = lines_[li];
        if (li > 0) out += '\n';
        for (int b = 0; b < line.blank_before; b++) out += '\n';
        out.append(static_cast<size_t>(line.indent), ' ');
        for (size_t j = 0; j < line.toks.size(); j++) {
            const size_t k = static_cast<size_t>(line.toks[j]);
            const Token &t = t_[k];
            if (j > 0) out += gap_[k];
            if (t.end_line == t.line || t.kind != TokKind::Comment) {
                out.append(t.text);
                continue;
            }
            // A multi-line comment moves with its first line, when every continuation line can.
            const int delta = col_[k] - t.col;
            std::vector<std::string_view> parts;
            size_t s = 0;
            for (size_t e; (e = t.text.find('\n', s)) != std::string_view::npos; s = e + 1) parts.push_back(t.text.substr(s, e - s));
            parts.push_back(t.text.substr(s));
            bool can = true;
            if (delta < 0) {
                for (size_t p = 1; p < parts.size(); p++) {
                    std::string_view l = parts[p];
                    if (l.find_first_not_of(" \t\r") == std::string_view::npos) continue;
                    size_t lead = 0;
                    while (lead < l.size() && l[lead] == ' ') lead++;
                    if (static_cast<int>(lead) < -delta) { can = false; break; }
                }
            }
            for (size_t p = 0; p < parts.size(); p++) {
                std::string_view l = parts[p];
                if (p > 0) {
                    out += '\n';
                    // Trailing blanks of a comment line go.
                    while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r') && p + 1 < parts.size()) l.remove_suffix(1);
                    if (l.find_first_not_of(" \t\r") == std::string_view::npos && p + 1 < parts.size()) continue;
                    if (can && delta > 0) out.append(static_cast<size_t>(delta), ' ');
                    if (can && delta < 0) l.remove_prefix(static_cast<size_t>(-delta));
                } else {
                    while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.remove_suffix(1);
                }
                out.append(l);
            }
        }
    }
    if (!out.empty()) out += '\n';
    return out;
}

int Layout::LineWidth(const Line &l) const {
    if (l.toks.empty()) return 0;
    return EndCol(l.toks.back());
}

std::vector<int> Layout::Breaks() const {
    std::vector<int> out;
    for (const Line &l : lines_) {
        if (l.keep || l.toks.size() < 2 || HasMultiline(l)) continue;
        if (LineWidth(l) <= opts_.width) continue;
        // Depth of each token relative to the line start (brackets and blocks opened on it).
        std::vector<int> depth(l.toks.size(), 0);
        int d = 0;
        for (size_t j = 0; j < l.toks.size(); j++) {
            const Token &t = t_[static_cast<size_t>(l.toks[j])];
            if (IsCloseBracket(t) || IsKw(t, "end")) d--;
            depth[j] = d;
            if (IsOpenBracket(t) || IsBlockOpen(t)) d++;
        }
        auto tok = [&](size_t j) -> const Token & { return t_[static_cast<size_t>(l.toks[j])]; };
        auto last_is_comment = [&]() { return tok(l.toks.size() - 1).kind == TokKind::Comment; };
        std::vector<int> found;
        // R1: after a binding's `=` (not before struct/sig/object/begin/function/fun).
        for (size_t j = 0; j + 1 < l.toks.size() && found.empty(); j++) {
            if (!binding_eq_[static_cast<size_t>(l.toks[j])] || depth[j] != 0) continue;
            const Token &n = tok(j + 1);
            if (IsBlockOpen(n) || IsKw(n, "function") || IsKw(n, "fun") || n.kind == TokKind::Comment) continue;
            if (j + 2 == l.toks.size() && IsKw(n, "in")) continue;
            found.push_back(l.toks[j + 1]);
        }
        // R2: before each `|>` at depth 0.
        if (found.empty()) {
            for (size_t j = 1; j < l.toks.size(); j++)
                if (IsP(tok(j), "|>") && depth[j] == 0) found.push_back(l.toks[j]);
        }
        // R3: after an arm's or a function's `->` with a body after it.
        if (found.empty()) {
            for (size_t j = 0; j + 1 < l.toks.size(); j++) {
                if (body_arrow_[static_cast<size_t>(l.toks[j])] && depth[j] == 0 && tok(j + 1).kind != TokKind::Comment) {
                    found.push_back(l.toks[j + 1]);
                    break;
                }
            }
        }
        // R4: before `else` at depth 0, then after `then`.
        if (found.empty()) {
            for (size_t j = 1; j < l.toks.size(); j++)
                if (IsKw(tok(j), "else") && depth[j] == 0) { found.push_back(l.toks[j]); break; }
        }
        if (found.empty()) {
            for (size_t j = 0; j + 1 < l.toks.size(); j++)
                if (IsKw(tok(j), "then") && depth[j] == 0 && tok(j + 1).kind != TokKind::Comment) {
                    found.push_back(l.toks[j + 1]);
                    break;
                }
        }
        // R5: after `;` at depth 0 (a sequence), then inside the first bracket (a list / record).
        for (int want = 0; want <= 1 && found.empty(); want++) {
            for (size_t j = 0; j + 1 < l.toks.size(); j++) {
                if (!IsP(tok(j), ";") || depth[j] != want) continue;
                const Token &n = tok(j + 1);
                if (n.kind == TokKind::Comment || IsCloseBracket(n) || IsKw(n, "end")) continue;
                found.push_back(l.toks[j + 1]);
            }
        }
        (void)last_is_comment;
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

// Comments compared with their continuation lines' leading blanks and all trailing blanks ignored.
std::string NormalizeComment(std::string_view c) {
    std::string out;
    size_t s = 0;
    bool first = true;
    while (true) {
        const size_t e = c.find('\n', s);
        std::string_view l = c.substr(s, e == std::string_view::npos ? std::string_view::npos : e - s);
        if (!first) {
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
        }
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t' || l.back() == '\r')) l.remove_suffix(1);
        if (!first) out += '\n';
        out.append(l);
        first = false;
        if (e == std::string_view::npos) break;
        s = e + 1;
    }
    return out;
}

std::string FormatOnce(std::string_view src, const std::vector<Token> &toks, const Options &opts, std::string *error,
                       int *error_line) {
    std::vector<bool> brk(toks.size(), false);
    std::string out;
    for (int round = 0; round < 64; round++) {
        Layout lay(src, toks, opts);
        if (!lay.Run(brk, error, error_line)) return std::string();
        out = lay.Render();
        if (!opts.break_lines) break;
        bool added = false;
        for (int k : lay.Breaks()) {
            if (!brk[static_cast<size_t>(k)]) {
                brk[static_cast<size_t>(k)] = true;
                added = true;
            }
        }
        if (!added) break;
    }
    return out;
}

}  // namespace

bool Lex(std::string_view src, std::vector<Token> *out, std::string *error, int *error_line) {
    out->clear();
    Lexer lx(src);
    return lx.Run(out, error, error_line);
}

bool SameTokens(std::string_view a, std::string_view b, std::string *why) {
    std::vector<Token> ta, tb;
    std::string e;
    int el = 0;
    if (!Lex(a, &ta, &e, &el) || !Lex(b, &tb, &e, &el)) {
        if (why) *why = "does not lex: " + e;
        return false;
    }
    if (ta.size() != tb.size()) {
        if (why) *why = "token count differs: " + std::to_string(ta.size()) + " vs " + std::to_string(tb.size());
        return false;
    }
    for (size_t k = 0; k < ta.size(); k++) {
        bool same = ta[k].kind == tb[k].kind;
        if (same) {
            if (ta[k].kind == TokKind::Comment) {
                same = NormalizeComment(ta[k].text) == NormalizeComment(tb[k].text);
            } else {
                same = ta[k].text == tb[k].text;
            }
        }
        if (!same) {
            if (why) *why = "token " + std::to_string(k) + " differs at line " + std::to_string(ta[k].line + 1) + ": `" +
                            std::string(ta[k].text.substr(0, 40)) + "` vs `" + std::string(tb[k].text.substr(0, 40)) + "`";
            return false;
        }
    }
    return true;
}

Result Format(std::string_view src, const Options &opts) {
    Result r;
    r.text = std::string(src);
    std::vector<Token> toks;
    if (!Lex(src, &toks, &r.error, &r.error_line)) return r;
    if (toks.empty()) {
        r.ok = true;
        r.text.clear();
        return r;
    }
    std::string out = FormatOnce(src, toks, opts, &r.error, &r.error_line);
    if (!r.error.empty()) return r;
    // A fixed point: formatting the result again must change nothing (a couple of rounds settle
    // what the first pass's column alignment moved).
    for (int round = 0; round < 3; round++) {
        std::vector<Token> again;
        std::string e;
        int el = 0;
        if (!Lex(out, &again, &e, &el)) {
            r.error = "internal error: the result does not lex (" + e + ")";
            return r;
        }
        std::string next = FormatOnce(out, again, opts, &e, &el);
        if (!e.empty()) {
            r.error = "internal error: the result does not format (" + e + ")";
            return r;
        }
        if (next == out) break;
        if (round == 2) {
            r.error = "internal error: the layout does not settle; nothing changed";
            return r;
        }
        out = std::move(next);
    }
    std::string why;
    if (!SameTokens(src, out, &why)) {
        r.error = "refusing to format: the result would change the code (" + why + ")";
        return r;
    }
    r.ok = true;
    r.text = std::move(out);
    return r;
}

}  // namespace ocamlfmt
