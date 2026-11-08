// mep's Octave reader: see octave_parse.h.
//
// Layout: the lexer (class Lexer) first, then the parser (class Parser),
// then the checks Octave makes after parsing (variable-vs-command).
// Comments of the form "Octave: ..." name the behaviour being reproduced.
#include "octave_parse.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace octparse {

namespace {

const char *const kKeywords[] = {
    "__FILE__", "__LINE__", "arguments", "break", "case", "catch", "classdef", "continue", "do", "else", "elseif",
    "end", "end_try_catch", "end_unwind_protect", "endarguments", "endclassdef", "endenumeration", "endevents",
    "endfor", "endfunction", "endif", "endmethods", "endparfor", "endproperties", "endspmd", "endswitch",
    "endwhile", "enumeration", "events", "for", "function", "get", "global", "if", "methods", "otherwise",
    "parfor", "persistent", "properties", "return", "set", "spmd", "switch", "try", "until", "unwind_protect",
    "unwind_protect_cleanup", "while",
};

bool InKeywordTable(std::string_view s) {
    for (const char *k : kKeywords)
        if (s == k) return true;
    return false;
}

// Octave's iskeyword(): the table minus the context words.
bool IsOctaveKeyword(std::string_view s) {
    if (s == "set" || s == "get" || s == "arguments" || s == "enumeration" || s == "events" || s == "methods" ||
        s == "properties")
        return false;
    return InKeywordTable(s);
}

// The end-token kinds of token.h, by keyword.
enum class EndType { None, Simple, If, For, Parfor, While, Switch, TryCatch, Unwind, Function, Classdef,
                     Properties, Methods, Events, Enumeration, Arguments, Spmd };

EndType EndTypeOf(std::string_view s) {
    if (s == "end") return EndType::Simple;
    if (s == "endif") return EndType::If;
    if (s == "endfor") return EndType::For;
    if (s == "endparfor") return EndType::Parfor;
    if (s == "endwhile") return EndType::While;
    if (s == "endswitch") return EndType::Switch;
    if (s == "end_try_catch") return EndType::TryCatch;
    if (s == "end_unwind_protect") return EndType::Unwind;
    if (s == "endfunction") return EndType::Function;
    if (s == "endclassdef") return EndType::Classdef;
    if (s == "endproperties") return EndType::Properties;
    if (s == "endmethods") return EndType::Methods;
    if (s == "endevents") return EndType::Events;
    if (s == "endenumeration") return EndType::Enumeration;
    if (s == "endarguments") return EndType::Arguments;
    if (s == "endspmd") return EndType::Spmd;
    return EndType::None;
}

const char *EndTypeName(EndType t) {
    switch (t) {
    case EndType::Simple: return "end";
    case EndType::If: return "endif";
    case EndType::For: return "endfor";
    case EndType::Parfor: return "endparfor";
    case EndType::While: return "endwhile";
    case EndType::Switch: return "endswitch";
    case EndType::TryCatch: return "end_try_catch";
    case EndType::Unwind: return "end_unwind_protect";
    case EndType::Function: return "endfunction";
    case EndType::Classdef: return "endclassdef";
    case EndType::Properties: return "endproperties";
    case EndType::Methods: return "endmethods";
    case EndType::Events: return "endevents";
    case EndType::Enumeration: return "endenumeration";
    case EndType::Arguments: return "endarguments";
    case EndType::Spmd: return "endspmd";
    case EndType::None: break;
    }
    return "";
}

bool IsSpaceTab(char c) { return c == ' ' || c == '\t'; }
bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

struct LexAbort {};

// ---------------------------------------------------------------------------------------------
// The lexer.
// ---------------------------------------------------------------------------------------------

enum class Nest : unsigned char { Bracket, Brace, Paren, AnonBody };
enum class State : unsigned char { Initial, Matrix, Command, FqIdent };

class Lexer {
public:
    Lexer(std::string_view src, ParseResult &out) : out_(out) {
        s_.assign(src.data(), src.size());
        orig_size_ = s_.size();
        // Octave's file reader hands the lexer a final newline.
        if (s_.empty() || s_.back() != '\n') s_.push_back('\n');
        obj_index_.push_back(false);
    }

    // --- the flags Octave's parser and lexer share (lexical_feedback) ---
    bool at_bos = true;
    bool looking_for_object_index = false;
    bool looking_at_indirect_ref = false;
    bool looking_at_decl_list = false;
    bool looking_at_return_list = false;
    bool looking_at_parameter_list = false;
    bool looking_at_anon_fcn_args = false;
    bool parsing_anon_fcn_body = false;
    bool maybe_classdef_get_set_method = false;
    bool classdef_element_names_are_keywords = false;
    bool parsing_classdef_decl = false;
    bool parsing_classdef_superclass = false;
    bool arguments_is_keyword = false;
    bool reading_fcn_file = false, reading_classdef_file = false, reading_script_file = true;
    int looking_at_function_handle = 0;
    int defining_fcn = 0;
    int looping = 0;
    std::vector<bool> parsed_function_name;
    std::vector<Nest> nest;

    void RemoveNest() {
        if (!nest.empty()) nest.pop_back();
    }

    /** @brief Lexes one more token onto out_.tokens. */
    void Next() {
        for (;;) {
            if (Step()) return;
        }
    }

private:
    ParseResult &out_;
    std::string s_;
    size_t orig_size_ = 0;
    size_t pos_ = 0;
    int line_ = 1, col_ = 1;  // Octave's filepos (1-based)
    // Octave lexes an inserted separator as a real character, so its file
    // column runs one ahead per synthetic token on the line; only the
    // reported error positions see it.
    int drift_ = 0;
    std::vector<char> pending_;  // synthetic characters (xunput), last = next
    std::vector<State> states_{State::Initial};
    std::vector<bool> obj_index_;  // m_looking_at_object_index, back() = front()
    int token_count_ = 0;
    int cmd_paren_ = 0;
    std::string cmd_text_;
    int cmd_begin_ = -1;
    int cmd_line_ = 0, cmd_col_ = 0;

    State St() const { return states_.back(); }
    void Push(State s) { states_.push_back(s); }
    void Pop() {
        if (states_.size() > 1) states_.pop_back();
    }

    char At(size_t i) const { return i < s_.size() ? s_[i] : '\0'; }
    bool AtEof() const { return pending_.empty() && pos_ >= s_.size(); }

    int Clamp(size_t p) const { return static_cast<int>(std::min(p, orig_size_)); }

    // --- the previous token (m_tokens.front ()) ---
    Token *Prev() { return out_.tokens.empty() ? nullptr : &out_.tokens.back(); }
    bool PrevIs(std::string_view t) {
        const Token *p = Prev();
        if (!p) return false;
        if (t == ",") return p->kind == Tok::Comma;
        if (t == ";") return p->kind == Tok::Semi;
        if (t == "\n") return p->kind == Tok::Newline;
        return p->kind == Tok::Op && p->text == t;
    }
    bool PrevIsBinop() {
        const Token *p = Prev();
        if (!p) return false;
        if (p->kind == Tok::Comma || p->kind == Tok::Semi) return true;
        if (p->kind != Tok::Op) return false;
        static const char *const ops[] = {"+",   "-",  "@",   "~",  "!",  "*",  "/",  ":",   "=",  "+=", "&=",
                                          "/=",  "./", "./=", ".\\", ".\\=", ".*", ".*=", ".^", ".^=", "&",  "&&",
                                          "==",  ">=", ">",   "<=", "<",  "!=", "~=", "|",   "||", "\\", "\\=",
                                          "*=",  "|=", "^",   "^=", "-="};
        for (const char *o : ops)
            if (p->text == o) return true;
        return false;
    }
    bool PrevIsKeyword() {
        const Token *p = Prev();
        // __FILE__ / __LINE__ come out of Octave's lexer as a string / a number.
        return p && p->kind == Tok::Keyword && p->text != "__FILE__" && p->text != "__LINE__";
    }
    bool SpaceFollowsPrev() {
        const Token *p = Prev();
        return p && p->space_after;
    }
    bool PrevMayBeCommand() {
        const Token *p = Prev();
        return p && p->may_be_command;
    }
    void MarkPrevTrailingSpace() {
        Token *p = Prev();
        if (p && p->kind != Tok::Newline) p->space_after = true;
    }
    bool WhitespaceIsSignificant() const {
        if (nest.empty()) return false;
        return nest.back() == Nest::Bracket || (nest.back() == Nest::Brace && !obj_index_.back());
    }
    bool InsideAnyObjectIndex() const {
        for (bool b : obj_index_)
            if (b) return true;
        return false;
    }
    bool ObjIndexFront() const { return !obj_index_.empty() && obj_index_.back(); }
    void PopObjIndex() {
        if (obj_index_.size() > 1) {
            obj_index_.pop_back();
        } else {
            obj_index_.back() = false;
        }
    }
    // The separator Octave unputs before an element of a matrix that a
    // space began: `[a b]`, `[a -b]`, `[f (1)]`.
    bool NeedsCommaBefore() {
        return WhitespaceIsSignificant() && SpaceFollowsPrev() && !(PrevIs("[") || PrevIs("{") || PrevIsBinop());
    }

    void Advance(size_t n) {
        for (size_t i = 0; i < n && pos_ < s_.size(); i++) {
            const char c = s_[pos_++];
            if (c == '\n' || (c == '\r' && At(pos_) != '\n')) {
                line_++;
                col_ = 1;
                drift_ = 0;
            } else if (c != '\r') {
                col_++;
            }
        }
    }
    size_t NlLen(size_t p) const {
        if (At(p) == '\n') return 1;
        if (At(p) == '\r') return At(p + 1) == '\n' ? 2 : 1;
        return 0;
    }
    bool AtBol() const { return pending_.empty() && (pos_ == 0 || s_[pos_ - 1] == '\n' || s_[pos_ - 1] == '\r'); }

    // A 0-based (line, column) for a byte offset, for the token record.
    void LineCol(size_t p, int *line, int *col) const {
        // Count from the last token start; cheap enough at lexing time.
        (void)p;
        *line = line_ - 1;
        *col = col_ - 1;
    }

    int Emit(Tok kind, std::string text, size_t len, bool count = true) {
        Token t;
        t.kind = kind;
        t.text = std::move(text);
        t.begin = Clamp(pos_);
        LineCol(pos_, &t.line, &t.col);
        Advance(len);
        t.end = Clamp(pos_);
        t.after_line = line_;
        t.after_col = col_ + drift_;
        return PushToken(std::move(t), count);
    }
    // Octave's file column overcounts some tokens; the token just pushed
    // and everything after it on the line report the extra.
    void Bump(size_t n) {
        drift_ += static_cast<int>(n);
        if (!out_.tokens.empty()) out_.tokens.back().after_col += static_cast<int>(n);
    }
    int EmitSynthetic(Tok kind, const char *text) {
        Token t;
        t.kind = kind;
        t.text = text;
        t.begin = t.end = Clamp(pos_);
        LineCol(pos_, &t.line, &t.col);
        drift_++;
        t.after_line = line_;
        t.after_col = col_ + drift_;
        t.synthetic = true;
        return PushToken(std::move(t), true);
    }
    int PushToken(Token t, bool count) {
        const bool sep = t.kind == Tok::Semi || t.kind == Tok::Comma || t.kind == Tok::Newline;
        if (arguments_is_keyword && !sep) arguments_is_keyword = false;
        if (t.kind != Tok::Newline && count) token_count_++;
        out_.tokens.push_back(std::move(t));
        return static_cast<int>(out_.tokens.size()) - 1;
    }
    void LexError(const char *msg, int after_line, int after_col) {
        Token t;
        t.kind = Tok::LexError;
        t.text = msg;
        t.begin = t.end = Clamp(pos_);
        LineCol(pos_, &t.line, &t.col);
        t.after_line = after_line;
        t.after_col = after_col + (after_line == line_ ? drift_ : 0);
        PushToken(std::move(t), true);
    }

    // Records a comment [b, e) (e excludes the newline).
    void AddComment(size_t b, size_t e, int line0, bool block, bool full_line, bool continuation = false) {
        Comment c;
        c.begin = Clamp(b);
        c.end = Clamp(e);
        c.line = line0;
        c.end_line = line_ - 1;
        c.block = block;
        c.full_line = full_line;
        c.continuation = continuation;
        // A full-line comment's NL was consumed: end_line is the line after.
        if (full_line && !block) c.end_line = line0;
        if (block) c.end_line = line_ - 2 >= line0 ? line_ - 2 : line0;
        out_.comments.push_back(c);
    }

    // --- pattern helpers -----------------------------------------------------------------

    // `{S}*{CCHAR}\{{S}*{NL}` at p: the line opening a block comment.
    bool BlockOpenAt(size_t p, char brace, size_t *len) const {
        size_t q = p;
        while (IsSpaceTab(At(q))) q++;
        if (At(q) != '%' && At(q) != '#') return false;
        if (At(q + 1) != brace) return false;
        q += 2;
        while (IsSpaceTab(At(q))) q++;
        const size_t nl = NlLen(q);
        if (!nl) return false;
        *len = q + nl - p;
        return true;
    }
    // `{S}*{CCHAR}{ANY_EXCEPT_NL}*` at p; returns the end (before the NL) or npos.
    size_t LineCommentAt(size_t p) const {
        size_t q = p;
        while (IsSpaceTab(At(q))) q++;
        if (At(q) != '%' && At(q) != '#') return std::string::npos;
        while (q < s_.size() && s_[q] != '\n' && s_[q] != '\r') q++;
        if (q >= s_.size()) return std::string::npos;
        return q;
    }
    // `\.\.\.{ANY_EXCEPT_NL}*{NL}` at p; returns the length.
    size_t ContinuationAt(size_t p) const {
        if (!(At(p) == '.' && At(p + 1) == '.' && At(p + 2) == '.')) return 0;
        size_t q = p + 3;
        while (q < s_.size() && s_[q] != '\n' && s_[q] != '\r') q++;
        const size_t nl = NlLen(q);
        if (!nl) return 0;
        return q + nl - p;
    }
    size_t IdentLen(size_t p) const {
        if (!IsIdentStart(At(p))) return 0;
        size_t q = p + 1;
        while (IsIdentChar(At(q))) q++;
        return q - p;
    }
    // {FQIDENT}: IDENT ({S}* . {S}* IDENT)*; longest.
    size_t FqIdentLen(size_t p) const {
        size_t n = IdentLen(p);
        if (!n) return 0;
        size_t q = p + n;
        for (;;) {
            size_t r = q;
            while (IsSpaceTab(At(r))) r++;
            if (At(r) != '.') break;
            r++;
            while (IsSpaceTab(At(r))) r++;
            const size_t m = IdentLen(r);
            if (!m) break;
            q = r + m;
        }
        return q - p;
    }
    // {FQIDENT}{S}*@{S}*{FQIDENT}
    size_t SuperclassRefLen(size_t p) const {
        const size_t a = FqIdentLen(p);
        if (!a) return 0;
        size_t q = p + a;
        while (IsSpaceTab(At(q))) q++;
        if (At(q) != '@') return 0;
        q++;
        while (IsSpaceTab(At(q))) q++;
        const size_t b = FqIdentLen(q);
        if (!b) return 0;
        return q + b - p;
    }
    // Octave's number patterns; 0 when none.
    size_t NumberLen(size_t p) const {
        size_t best = 0;
        // {BINARY_NUMBER} / {HEXADECIMAL_NUMBER}
        if (At(p) == '0' && (At(p + 1) == 'b' || At(p + 1) == 'B') && (At(p + 2) == '0' || At(p + 2) == '1')) {
            size_t q = p + 3;
            while (At(q) == '0' || At(q) == '1' || At(q) == '_') q++;
            q += SizeSuffix(q);
            best = std::max(best, q - p);
        }
        if (At(p) == '0' && (At(p + 1) == 'x' || At(p + 1) == 'X') && IsHex(At(p + 2))) {
            size_t q = p + 3;
            while (IsHex(At(q)) || At(q) == '_') q++;
            q += SizeSuffix(q);
            best = std::max(best, q - p);
        }
        // {DECIMAL_DIGITS}/\.[\*/\\^\'] -- digits only, but the context counts in the length
        size_t d = 0;
        if (IsDigit(At(p))) {
            d = 1;
            while (IsDigit(At(p + d)) || At(p + d) == '_') d++;
            const char c = At(p + d + 1);
            if (At(p + d) == '.' && (c == '*' || c == '/' || c == '\\' || c == '^' || c == '\'')) {
                if (d + 2 > best) {
                    // Wins over {DECIMAL_NUMBER} when longer (or equal: listed first).
                    const size_t dec = DecimalLen(p);
                    if (d + 2 >= dec) return d;
                }
            }
        }
        best = std::max(best, DecimalLen(p));
        return best;
    }
    size_t SizeSuffix(size_t q) const {
        if (At(q) != 's' && At(q) != 'u') return 0;
        if (At(q + 1) == '8') return 2;
        if ((At(q + 1) == '1' && At(q + 2) == '6') || (At(q + 1) == '3' && At(q + 2) == '2') ||
            (At(q + 1) == '6' && At(q + 2) == '4'))
            return 3;
        return 0;
    }
    size_t DigitsLen(size_t q) const {
        if (!IsDigit(At(q))) return 0;
        size_t n = 1;
        while (IsDigit(At(q + n)) || At(q + n) == '_') n++;
        return n;
    }
    // {REAL_DECIMAL}[IiJj]?
    size_t DecimalLen(size_t p) const {
        size_t q = p;
        const size_t d1 = DigitsLen(q);
        if (d1) {
            q += d1;
            if (At(q) == '.') {
                q++;
                q += DigitsLen(q);  // DIGITS\.? or DIGITS?\.DIGITS (both covered)
            }
        } else if (At(q) == '.' && DigitsLen(q + 1)) {
            q += 1 + DigitsLen(q + 1);
        } else {
            return 0;
        }
        if (At(q) == 'd' || At(q) == 'D' || At(q) == 'e' || At(q) == 'E') {
            size_t r = q + 1;
            if (At(r) == '+' || At(r) == '-') r++;
            const size_t e = DigitsLen(r);
            if (e) q = r + e;
        }
        if (At(q) == 'i' || At(q) == 'I' || At(q) == 'j' || At(q) == 'J') q++;
        return q - p;
    }

    // --- the actions --------------------------------------------------------------------

    void BeginCommand() {
        Push(State::Command);
        cmd_text_.clear();
        cmd_begin_ = -1;
        cmd_paren_ = 0;
    }
    int FinishCommandArg() {
        Token t;
        t.kind = Tok::SqString;
        t.text = cmd_text_;
        t.begin = cmd_begin_ < 0 ? Clamp(pos_) : cmd_begin_;
        t.end = Clamp(pos_);
        t.line = cmd_line_;
        t.col = cmd_col_;
        t.after_line = line_;
        t.after_col = col_ + drift_;
        t.command_arg = true;
        cmd_text_.clear();
        cmd_begin_ = -1;
        cmd_paren_ = 0;
        return PushToken(std::move(t), true);
    }
    void CmdAppend(size_t n) {
        if (cmd_begin_ < 0) {
            cmd_begin_ = Clamp(pos_);
            cmd_line_ = line_ - 1;
            cmd_col_ = col_ - 1;
        }
        cmd_text_.append(s_, pos_, n);
        Advance(n);
    }

    // Reads a string body after its opening quote (already consumed).
    // Appends to `text`; returns false on an error (a LexError was pushed).
    bool StringBody(char q, std::string *text) {
        for (;;) {
            const char c = At(pos_);
            if (pos_ >= s_.size()) {
                LexError("unterminated character string constant", line_, col_);
                return false;
            }
            if (c == '\n' || c == '\r') {
                LexError("unterminated character string constant", line_, col_);
                return false;
            }
            if (c == q) {
                if (At(pos_ + 1) == q) {
                    text->push_back(q);
                    Advance(2);
                    continue;
                }
                Advance(1);
                return true;
            }
            if (q == '"' && c == '\\') {
                const char d = At(pos_ + 1);
                if (d >= '0' && d <= '7') {
                    size_t n = 1;
                    unsigned v = 0;
                    while (n <= 3 && At(pos_ + n) >= '0' && At(pos_ + n) <= '7') {
                        v = v * 8 + static_cast<unsigned>(At(pos_ + n) - '0');
                        n++;
                    }
                    if (v > 0xff) {
                        Advance(n);  // update_token_positions over the escape
                        LexError("invalid octal escape sequence in character string", line_, col_);
                        return false;
                    }
                    text->push_back(static_cast<char>(v));
                    Advance(n);
                    continue;
                }
                if (d == 'x' && IsHex(At(pos_ + 2))) {
                    size_t n = 2;
                    unsigned v = 0;
                    while (IsHex(At(pos_ + n))) {
                        const char h = At(pos_ + n);
                        v = v * 16 + static_cast<unsigned>(IsDigit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
                        n++;
                    }
                    text->push_back(static_cast<char>(v & 0xff));
                    Advance(n);
                    continue;
                }
                const size_t nl = NlLen(pos_ + 1);
                if (nl) {  // backslash-newline: the string continues
                    Advance(1 + nl);
                    continue;
                }
                switch (d) {
                case 'a': text->push_back('\a'); break;
                case 'b': text->push_back('\b'); break;
                case 'f': text->push_back('\f'); break;
                case 'n': text->push_back('\n'); break;
                case 'r': text->push_back('\r'); break;
                case 't': text->push_back('\t'); break;
                case 'v': text->push_back('\v'); break;
                default: text->push_back(d); break;
                }
                Advance(2);
                continue;
            }
            text->push_back(c);
            Advance(1);
        }
    }

    void LexString(char q) {
        const size_t b = pos_;
        int line0 = 0, col0 = 0;
        LineCol(pos_, &line0, &col0);
        Advance(1);
        std::string text;
        if (!StringBody(q, &text)) return;
        Token t;
        t.kind = q == '"' ? Tok::DqString : Tok::SqString;
        t.text = std::move(text);
        t.begin = Clamp(b);
        t.end = Clamp(pos_);
        t.line = line0;
        t.col = col0;
        t.after_line = line_;
        t.after_col = col_ + drift_;
        looking_for_object_index = true;
        at_bos = false;
        PushToken(std::move(t), true);
    }

    int HandleOp(const std::string &text, size_t len, bool bos = false) {
        Tok kind = Tok::Op;
        if (text == ",") kind = Tok::Comma;
        if (text == ";") kind = Tok::Semi;
        const int i = Emit(kind, text, len);
        looking_for_object_index = false;
        at_bos = bos;
        if (text == "<" && parsing_classdef_decl) {
            parsing_classdef_superclass = true;
            Push(State::FqIdent);
        } else if (text == "&" && parsing_classdef_superclass) {
            Push(State::FqIdent);
        }
        return i;
    }
    int HandleSyntheticSep(char c) {
        // The "," / ";" rules, applied to a character Octave unput.
        const bool bos = !(WhitespaceIsSignificant() || ObjIndexFront());
        const int i = EmitSynthetic(c == ',' ? Tok::Comma : Tok::Semi, c == ',' ? "," : ";");
        looking_for_object_index = false;
        at_bos = bos;
        return i;
    }

    bool LooksLikeCommandArg(size_t oplen) {
        const bool space_before = SpaceFollowsPrev();
        const bool space_after = IsSpaceTab(At(pos_ + oplen));
        return space_before && !space_after && PrevMayBeCommand();
    }

    // make_keyword_token: Tok::Keyword pushed (true), or not a keyword here (false).
    bool KeywordToken(const std::string &w, size_t len) {
        if (!InKeywordTable(w)) return false;
        const bool prev_bos = at_bos;
        at_bos = false;
        if (w == "break" || w == "catch" || w == "continue" || w == "else" || w == "otherwise" || w == "return" ||
            w == "unwind_protect_cleanup") {
            at_bos = true;
        } else if (w == "persistent" || w == "global") {
            looking_at_decl_list = true;
        } else if (w == "end") {
            if (InsideAnyObjectIndex() ||
                (defining_fcn && !(looking_at_return_list ||
                                   (!parsed_function_name.empty() && parsed_function_name.back())))) {
                at_bos = prev_bos;
                return false;
            }
            at_bos = true;
        } else if (EndTypeOf(w) != EndType::None) {
            at_bos = true;
        } else if (w == "for" || w == "parfor" || w == "while") {
            looping++;
        } else if (w == "do") {
            at_bos = true;
            looping++;
        } else if (w == "try" || w == "unwind_protect" || w == "spmd") {
            at_bos = true;
        } else if (w == "get" || w == "set") {
            if (!maybe_classdef_get_set_method) {
                at_bos = prev_bos;
                return false;
            }
        } else if (w == "enumeration" || w == "events" || w == "methods" || w == "properties") {
            if (!classdef_element_names_are_keywords) {
                at_bos = prev_bos;
                return false;
            }
        } else if (w == "classdef") {
            if (token_count_ == 0) {
                reading_classdef_file = true;
                reading_script_file = false;
            }
        } else if (w == "function") {
            defining_fcn++;
            parsed_function_name.push_back(false);
            if (token_count_ == 0) {
                reading_fcn_file = true;
                reading_script_file = false;
            }
        } else if (w == "arguments") {
            // Unlike get/set and properties/..., Octave does not restore
            // m_at_beginning_of_statement here: a plain `arguments` is never a
            // command word (`arguments are given` is a syntax error).
            if (!arguments_is_keyword) return false;
        }
        Emit(Tok::Keyword, w, len);
        return true;
    }

    void HandleIdentifier(size_t len) {
        const std::string w = s_.substr(pos_, len);
        if (looking_at_indirect_ref) {
            Emit(Tok::StructElt, w, len);
            looking_for_object_index = true;
            return;
        }
        if (KeywordToken(w, len)) {
            looking_for_object_index = false;
            return;
        }
        const bool bos = at_bos;
        const int i = Emit(Tok::Name, w, len);
        if (bos && !(parsing_anon_fcn_body || w == "e" || w == "pi" || w == "I" || w == "i" || w == "J" ||
                     w == "j" || w == "Inf" || w == "inf" || w == "NaN" || w == "nan"))
            out_.tokens[static_cast<size_t>(i)].may_be_command = true;
        if (w != "end") looking_for_object_index = true;
        at_bos = false;
    }

    // `{S}*{CCHAR}...` comments starting at pos_ (LINE_COMMENT_START and
    // BLOCK_COMMENT_START). Returns after consuming a run of full-line
    // comments, or a single end-of-line comment (its newline left in place).
    void LexComments() {
        bool any_full = false;
        bool after_lone_cr = false;  // flex's at-bol flag is set by '\n' only
        for (;;) {
            // A line comment is re-read in LINE_COMMENT_START with flex's
            // at-beginning-of-line flag set (the match ended in a newline),
            // so `%{` alone on the rest of a line opens a block comment even
            // after code: `x = 1 %{`.
            size_t blen = 0;
            if (!after_lone_cr && BlockOpenAt(pos_, '{', &blen)) {
                LexBlockComment();
                at_bos = true;  // finish_comment
                return;
            }
            const size_t e = LineCommentAt(pos_);
            if (e == std::string::npos) break;
            const bool full = AtBol();
            const size_t b = pos_;
            const int line0 = line_ - 1;
            size_t q = pos_;
            const bool have_space = IsSpaceTab(At(q));
            if (!full) {
                if (have_space) MarkPrevTrailingSpace();
                Advance(e - pos_);
                AddComment(b, e, line0, false, false);
                at_bos = true;  // finish_comment
                return;         // the newline is lexed next
            }
            const size_t nl = NlLen(e);
            after_lone_cr = nl == 1 && At(e) == '\r';
            Advance(e - pos_ + nl);
            AddComment(b, e, line0, false, true);
            any_full = true;
        }
        if (any_full) at_bos = true;  // finish_comment at the end of the run
    }
    void LexBlockComment() {
        // pos_ is at a block-opening line.
        const size_t b = pos_;
        const int line0 = line_ - 1;
        int depth = 0;
        size_t last_end = pos_;
        while (pos_ < s_.size()) {
            size_t len = 0;
            if (BlockOpenAt(pos_, '{', &len)) {
                depth++;
                last_end = pos_ + len;
                Advance(len);
                continue;
            }
            if (BlockOpenAt(pos_, '}', &len)) {
                depth--;
                size_t e = pos_ + len;
                while (e > pos_ && (s_[e - 1] == '\n' || s_[e - 1] == '\r')) e--;
                last_end = e;
                Advance(len);
                if (depth == 0) {
                    AddComment(b, last_end, line0, true, true);
                    return;
                }
                continue;
            }
            size_t q = pos_;
            while (q < s_.size() && s_[q] != '\n' && s_[q] != '\r') q++;
            last_end = q;
            Advance(q - pos_ + NlLen(q));
        }
        // Unterminated: Octave reports it but returns END_OF_INPUT anyway.
        AddComment(b, std::min(last_end, s_.size()), line0, true, true);
    }

    void HandleContinuation(size_t len, bool matrix) {
        // handle_continuation: a space after the dots counts as trailing space.
        size_t q = pos_ + 3;
        const bool have_space = IsSpaceTab(At(q));
        if (have_space) MarkPrevTrailingSpace();
        while (IsSpaceTab(At(q))) q++;
        size_t e = pos_ + len;
        while (e > pos_ && (s_[e - 1] == '\n' || s_[e - 1] == '\r')) e--;
        if (q < e) AddComment(pos_ + 3, e, line_ - 1, false, false, true);
        Advance(len);
        if (matrix) MarkPrevTrailingSpace();
    }

    // One flex match. Returns true when a token was pushed.
    bool Step() {
        const size_t before = out_.tokens.size();
        StepInner();
        return out_.tokens.size() > before;
    }

    void StepInner() {
        if (!pending_.empty()) {
            const char c = pending_.back();
            pending_.pop_back();
            HandleSyntheticSep(c);
            return;
        }
        if (St() == State::Command) {
            StepCommand();
            return;
        }
        if (St() == State::FqIdent) {
            StepFqIdent();
            return;
        }
        if (pos_ >= s_.size()) {
            Token t;
            t.kind = Tok::Eof;
            t.begin = t.end = Clamp(pos_);
            LineCol(pos_, &t.line, &t.col);
            t.after_line = line_;
            t.after_col = col_ + drift_;
            out_.tokens.push_back(std::move(t));
            return;
        }
        const bool matrix = St() == State::Matrix;
        const char c = s_[pos_];

        // Comments (and the block-comment line) win over whitespace.
        {
            size_t blen = 0;
            if (BlockOpenAt(pos_, '{', &blen) || LineCommentAt(pos_) != std::string::npos) {
                LexComments();
                return;
            }
        }
        if (IsSpaceTab(c)) {
            size_t q = pos_;
            while (IsSpaceTab(At(q))) q++;
            Advance(q - pos_);
            MarkPrevTrailingSpace();
            return;
        }
        if (const size_t nl = NlLen(pos_)) {
            if (matrix) {
                Advance(nl);
                if (!nest.empty() && nest.back() == Nest::Paren) return;
                if (!(PrevIs(";") || PrevIs("[") || PrevIs("{"))) pending_.push_back(';');
                return;
            }
            if (!nest.empty() && nest.back() == Nest::Paren) {
                Advance(nl);
                at_bos = false;
                return;
            }
            if (nest.empty() || nest.back() == Nest::AnonBody) {
                Emit(Tok::Newline, "\n", nl, false);
                at_bos = true;
                return;
            }
            Advance(nl);
            LexError("unexpected internal lexer error", line_, col_);
            return;
        }
        if (const size_t cl = ContinuationAt(pos_)) {
            HandleContinuation(cl, matrix);
            return;
        }
        if (matrix && (c == ']' || c == '}')) {
            PopObjIndex();
            looking_for_object_index = true;
            at_bos = false;
            RemoveNest();
            Pop();
            Emit(Tok::Op, std::string(1, c), 1);
            return;
        }

        // Numbers, identifiers and the other word-like patterns.
        const size_t numlen = NumberLen(pos_);
        if (numlen) {
            // `.5` competes with the '.' operators; the number is longer.
            if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                BeginCommand();
                return;
            }
            if (NeedsCommaBefore()) {
                pending_.push_back(',');
                return;
            }
            Emit(Tok::Number, s_.substr(pos_, numlen), numlen);
            looking_for_object_index = false;
            at_bos = false;
            return;
        }
        if (IsIdentStart(c)) {
            const size_t id = IdentLen(pos_);
            const size_t sup = SuperclassRefLen(pos_);
            size_t getset = 0;
            if ((s_.compare(pos_, 3, "get") == 0 || s_.compare(pos_, 3, "set") == 0)) {
                size_t q = pos_ + 3;
                while (IsSpaceTab(At(q))) q++;
                if (At(q) == '(') getset = q + 1 - pos_;
            }
            if (sup > id && sup >= getset) {
                if (PrevMayBeCommand()) {
                    BeginCommand();
                    return;
                }
                const std::string txt = s_.substr(pos_, sup);
                if (at_bos) {
                    const size_t at_or_dot = txt.find_first_of("@.");
                    const size_t spc = txt.find_first_of(" \t");
                    if (spc != std::string::npos && spc < at_or_dot) {
                        HandleIdentifier(spc);
                        Bump(spc);  // increment_column (spc_pos) before handle_identifier
                        return;
                    }
                }
                looking_for_object_index = true;
                at_bos = false;
                std::string clean;
                for (char ch : txt)
                    if (!IsSpaceTab(ch)) clean.push_back(ch);
                const size_t at = clean.find('@');
                const std::string meth = clean.substr(0, at);
                const std::string cls = clean.substr(at + 1);
                bool kw = IsOctaveKeyword(meth);
                for (size_t p = 0, q; !kw; p = q + 1) {
                    q = cls.find('.', p);
                    if (IsOctaveKeyword(cls.substr(p, q == std::string::npos ? std::string::npos : q - p))) kw = true;
                    if (q == std::string::npos) break;
                }
                if (kw) {
                    Advance(sup);
                    LexError("method, class, and package names may not be keywords", line_, col_);
                    return;
                }
                Emit(Tok::SuperclassRef, clean, sup);
                Bump(sup);  // handle_superclass_identifier counts the text twice
                return;
            }
            // HANDLE_IDENTIFIER
            if (NeedsCommaBefore()) {
                pending_.push_back(',');
                return;
            }
            if (!looking_at_decl_list && PrevMayBeCommand()) {
                BeginCommand();
                return;
            }
            if (getset > id) maybe_classdef_get_set_method = false;
            HandleIdentifier(id);
            if (getset > id) Bump(3);  // the (set|get){S}*\( rule counts "get" twice
            return;
        }
        if (c == '?') {
            size_t q = pos_ + 1;
            while (IsSpaceTab(At(q))) q++;
            const size_t fq = FqIdentLen(q);
            if (fq) {
                if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                    BeginCommand();
                    return;
                }
                std::string cls;
                for (size_t k = q; k < q + fq; k++)
                    if (!IsSpaceTab(s_[k])) cls.push_back(s_[k]);
                for (size_t p = 0, e; p <= cls.size(); p = e + 1) {
                    e = cls.find('.', p);
                    if (e == std::string::npos) e = cls.size();
                    if (IsOctaveKeyword(cls.substr(p, e - p))) {
                        Advance(q + fq - pos_);
                        LexError("class and package names may not be keywords", line_, col_);
                        return;
                    }
                }
                looking_for_object_index = true;
                const size_t len = q + fq - pos_;
                Emit(Tok::MetaQuery, cls, len);
                Bump(len);  // make_meta_identifier_token counts the text again
                return;
            }
        }
        if (c == '@') {
            size_t q = pos_ + 1;
            while (IsSpaceTab(At(q))) q++;
            const size_t fq = FqIdentLen(q);
            if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                BeginCommand();
                return;
            }
            if (NeedsCommaBefore()) {
                pending_.push_back(',');
                return;
            }
            at_bos = false;
            if (!fq) {
                looking_at_function_handle++;
                looking_for_object_index = false;
                Emit(Tok::Op, "@", 1);
                return;
            }
            std::string name;
            for (size_t k = q; k < q + fq; k++)
                if (!IsSpaceTab(s_[k])) name.push_back(s_[k]);
            if (IsOctaveKeyword(name)) {
                Advance(q + fq - pos_);
                LexError("function handles may not refer to keywords", line_, col_);
                return;
            }
            looking_for_object_index = true;
            Emit(Tok::FcnHandle, name, q + fq - pos_);
            return;
        }
        if (c == '\'') {
            if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                BeginCommand();
                return;
            }
            bool string = false;
            if (at_bos) {
                string = true;
            } else if (WhitespaceIsSignificant()) {
                if (SpaceFollowsPrev()) {
                    if (PrevIs("[") || PrevIs("{") || PrevIsBinop()) {
                        string = true;
                    } else {
                        pending_.push_back(',');
                        return;
                    }
                } else {
                    string = PrevIs("[") || PrevIs("{") || PrevIsBinop() || PrevIsKeyword();
                }
            } else {
                const Token *p = Prev();
                string = !p || PrevIs("[") || PrevIs("{") || PrevIs("(") || PrevIsBinop() || PrevIsKeyword();
            }
            if (string) {
                LexString('\'');
            } else {
                Emit(Tok::Op, "'", 1);  // HERMITIAN: handle_token only, no flags change
            }
            return;
        }
        if (c == '"') {
            if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                BeginCommand();
                return;
            }
            if (WhitespaceIsSignificant() && SpaceFollowsPrev() &&
                !(PrevIs("[") || PrevIs("{") || PrevIsBinop())) {
                pending_.push_back(',');
                return;
            }
            LexString('"');
            return;
        }
        if (c == '[') {
            if (NeedsCommaBefore()) {
                pending_.push_back(',');
                return;
            }
            nest.push_back(Nest::Bracket);
            obj_index_.push_back(false);
            looking_for_object_index = false;
            at_bos = false;
            if (defining_fcn && !(!parsed_function_name.empty() && parsed_function_name.back()))
                looking_at_return_list = true;
            Push(State::Matrix);
            Emit(Tok::Op, "[", 1);
            return;
        }
        if (c == ']') {
            RemoveNest();
            PopObjIndex();
            looking_for_object_index = true;
            at_bos = false;
            Emit(Tok::Op, "]", 1);
            return;
        }
        if (c == '(') {
            if (NeedsCommaBefore()) {
                pending_.push_back(',');
                return;
            }
            obj_index_.push_back(looking_for_object_index);
            looking_at_indirect_ref = false;
            looking_for_object_index = false;
            at_bos = false;
            nest.push_back(Nest::Paren);
            Emit(Tok::Op, "(", 1);
            return;
        }
        if (c == ')') {
            RemoveNest();
            PopObjIndex();
            looking_for_object_index = true;
            at_bos = false;
            if (looking_at_anon_fcn_args) {
                looking_at_anon_fcn_args = false;
                nest.push_back(Nest::AnonBody);
            }
            Emit(Tok::Op, ")", 1);
            return;
        }
        if (c == '{') {
            if (NeedsCommaBefore()) {
                pending_.push_back(',');
                return;
            }
            nest.push_back(Nest::Brace);
            obj_index_.push_back(looking_for_object_index);
            looking_for_object_index = false;
            at_bos = false;
            Push(State::Matrix);
            Emit(Tok::Op, "{", 1);
            return;
        }
        if (c == '}') {
            PopObjIndex();
            looking_for_object_index = true;
            at_bos = false;
            RemoveNest();
            Emit(Tok::Op, "}", 1);
            return;
        }

        // Operators: the longest spelling that matches.
        static const char *const ops[] = {
            ".*=", "./=", ".\\=", ".^=", ".*", "./", ".\\", ".^", ".'", "<=", "==", "!=", "~=", ">=", "&&", "||",
            "++",  "--",  "+=",  "-=",  "*=", "/=", "\\=", "^=", "&=", "|=", ":",  "&",  "|",  "<",  ">",  "*",
            "/",   "\\",  "^",   ";",   "+",  "-",  "~",   "!",  ",",  ".",  "=",
        };
        const char *op = nullptr;
        for (const char *o : ops) {
            const size_t n = std::strlen(o);
            if (s_.compare(pos_, n, o) == 0) {
                op = o;
                break;
            }
        }
        if (!op) {
            // Unrecognized input.
            if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                BeginCommand();
                return;
            }
            Advance(1);
            LexError("invalid character", line_, col_);
            return;
        }
        const std::string o = op;
        const size_t n = o.size();
        if (o == ";" || o == ",") {
            const bool bos = !(WhitespaceIsSignificant() || ObjIndexFront());
            HandleOp(o, n, bos);
            return;
        }
        if (o == "\\" || o == ".'" || o == "=") {
            HandleOp(o, n);
            return;
        }
        if (o == "+" || o == "-" || o == "~" || o == "!" || o == "++" || o == "--") {
            // CMD_OR_UNARY_OP
            if (PrevMayBeCommand()) {
                if (LooksLikeCommandArg(n)) {
                    BeginCommand();
                    return;
                }
                HandleOp(o, n);
                return;
            }
            if (WhitespaceIsSignificant() && SpaceFollowsPrev()) {
                const bool space_after = IsSpaceTab(At(pos_ + n));
                if (!(PrevIs("[") || PrevIs("{") || PrevIsBinop() || ((o == "+" || o == "-") && space_after))) {
                    pending_.push_back(',');
                    return;
                }
            }
            HandleOp(o, n);
            return;
        }
        if (o == ".") {
            if (PrevMayBeCommand() && SpaceFollowsPrev()) {
                BeginCommand();
                return;
            }
            Emit(Tok::Op, ".", 1);
            looking_for_object_index = false;
            at_bos = false;
            return;
        }
        // CMD_OR_OP
        if (LooksLikeCommandArg(n)) {
            BeginCommand();
            return;
        }
        HandleOp(o, n);
    }

    void StepFqIdent() {
        const size_t fq = FqIdentLen(pos_);
        if (fq) {
            Pop();
            std::string id;
            for (size_t k = pos_; k < pos_ + fq; k++)
                if (!IsSpaceTab(s_[k])) id.push_back(s_[k]);
            for (size_t p = 0, e; p <= id.size(); p = e + 1) {
                e = id.find('.', p);
                if (e == std::string::npos) e = id.size();
                if (IsOctaveKeyword(id.substr(p, e - p))) {
                    Advance(fq);
                    LexError("function, method, class, and package names may not be keywords", line_, col_);
                    return;
                }
            }
            looking_for_object_index = true;
            Emit(Tok::FqIdent, id, fq);
            Bump(fq);  // make_fq_identifier_token counts the text again
            return;
        }
        if (IsSpaceTab(At(pos_))) {
            size_t q = pos_;
            while (IsSpaceTab(At(q))) q++;
            Advance(q - pos_);
            MarkPrevTrailingSpace();
            return;
        }
        if (const size_t cl = ContinuationAt(pos_)) {
            Advance(cl);
            return;
        }
        Pop();
    }

    void StepCommand() {
        const char c = At(pos_);
        if (const size_t cl = ContinuationAt(pos_)) {
            if (!cmd_text_.empty()) {
                FinishCommandArg();
                return;
            }
            HandleContinuation(cl, false);
            return;
        }
        if (c == '#' || c == '%' || c == '\n' || c == '\r' || pos_ >= s_.size()) {
            if (!cmd_text_.empty()) {
                FinishCommandArg();
                return;
            }
            if (c == '#' || c == '%') {
                const size_t b = pos_;
                const int line0 = line_ - 1;
                size_t q = pos_;
                while (q < s_.size() && s_[q] != '\n' && s_[q] != '\r') q++;
                Advance(q - pos_);
                AddComment(b, q, line0, false, false);
            }
            looking_for_object_index = false;
            Pop();
            at_bos = true;
            if (pos_ >= s_.size()) return;
            Emit(Tok::Newline, "\n", NlLen(pos_), false);
            return;
        }
        if (c == ',' || c == ';') {
            if (c == ';' || cmd_paren_ == 0) {
                if (!cmd_text_.empty()) {
                    FinishCommandArg();
                    return;
                }
                looking_for_object_index = false;
                at_bos = true;
                Pop();
                Emit(c == ',' ? Tok::Comma : Tok::Semi, std::string(1, c), 1);
                return;
            }
            CmdAppend(1);
            return;
        }
        if (c == '(' || c == '[' || c == '{') {
            size_t q = pos_;
            while (At(q) == '(' || At(q) == '[' || At(q) == '{') q++;
            cmd_paren_ += static_cast<int>(q - pos_);
            CmdAppend(q - pos_);
            return;
        }
        if (c == ')' || c == ']' || c == '}') {
            size_t q = pos_;
            while (At(q) == ')' || At(q) == ']' || At(q) == '}') q++;
            cmd_paren_ -= static_cast<int>(q - pos_);
            CmdAppend(q - pos_);
            return;
        }
        if (c == '"' || c == '\'') {
            if (cmd_paren_ == 0) {
                if (cmd_begin_ < 0) {
                    cmd_begin_ = Clamp(pos_);
                    cmd_line_ = line_ - 1;
                    cmd_col_ = col_ - 1;
                }
                Advance(1);
                std::string text;
                if (!StringBody(c, &text)) return;
                cmd_text_ += text;
                return;
            }
            CmdAppend(1);
            return;
        }
        if (IsSpaceTab(c)) {
            size_t q = pos_;
            while (IsSpaceTab(At(q))) q++;
            if (cmd_paren_ == 0) {
                if (!cmd_text_.empty()) {
                    FinishCommandArg();
                    return;
                }
                Advance(q - pos_);
                return;
            }
            CmdAppend(q - pos_);
            return;
        }
        if (c == '.') {
            CmdAppend(1);
            return;
        }
        size_t q = pos_;
        while (q < s_.size()) {
            const char d = s_[q];
            if (d == '#' || d == '%' || d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '.' || d == ',' ||
                d == ';' || d == '"' || d == '\'' || d == '(' || d == '[' || d == '{' || d == '}' || d == ']' ||
                d == ')')
                break;
            q++;
        }
        CmdAppend(q - pos_);
    }
};

// ---------------------------------------------------------------------------------------------
// The parser.
// ---------------------------------------------------------------------------------------------

struct ParseAbort {};

class Parser {
public:
    Parser(std::string_view src, const Options &opt, ParseResult &out) : out_(out), lex_(src, out), opt_(opt) {}

    void Run() {
        try {
            File();
        } catch (const ParseAbort &) {
        }
        if (out_.ok()) CheckCommandsVsVariables();
        out_.function_file = lex_.reading_fcn_file;
        out_.classdef_file = lex_.reading_classdef_file;
    }

private:
    ParseResult &out_;
    Lexer lex_;
    const Options &opt_;
    size_t cur_ = 0;  // index of the next unconsumed token
    int depth_ = 0;
    bool endfunction_found_ = false;
    bool parsing_subfunctions_ = false;
    bool parsing_local_functions_ = false;
    int fcn_depth_ = -1;
    struct Scope {
        std::string name;
        std::set<std::string> variables;
        std::vector<int> commands;  // Command nodes
        std::vector<Scope *> kids;
        Scope *nested_in = nullptr;  // a nested function sees its parent's variables
        bool explicit_end = false;   // the function was closed by end/endfunction
        bool IsVariable(const std::string &nm) const {
            // Functions without `end` are siblings (subfunctions), not nested.
            for (const Scope *s = this; s; s = s->nested_in && s->nested_in->explicit_end ? s->nested_in : nullptr)
                if (s->variables.count(nm)) return true;
            return false;
        }
    };
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::vector<Scope *> scope_stack_;
    std::vector<std::string> parent_names_;  // the function nesting, for the duplicate-name check
    std::set<std::string> all_names_;
    // Bison's error rules: a syntax error while an anonymous function's
    // body ('A') or a parameter list ('P') is open is reported by that rule.
    std::vector<char> err_ctx_;
    bool in_anon_body_ = false;
    size_t cmdarg_ok_ = static_cast<size_t>(-1);  // the token after a function name

    // --- tokens ---
    const Token &Peek() {
        while (cur_ >= out_.tokens.size()) {
            if (!out_.tokens.empty() && out_.tokens.back().kind == Tok::Eof) {
                // Octave's lexer keeps answering END_OF_INPUT.
                out_.tokens.push_back(out_.tokens.back());
                continue;
            }
            lex_.Next();
        }
        return out_.tokens[cur_];
    }
    const Token &Peek2() {
        Peek();
        const size_t save = cur_;
        cur_++;
        Peek();
        cur_ = save;
        return out_.tokens[cur_ + 1];
    }
    int Take() {
        Peek();
        return static_cast<int>(cur_++);
    }
    bool IsOp(const Token &t, std::string_view s) const { return t.kind == Tok::Op && t.text == s; }
    bool PeekOp(std::string_view s) { return IsOp(Peek(), s); }
    bool PeekKw(std::string_view s) {
        const Token &t = Peek();
        return t.kind == Tok::Keyword && t.text == s;
    }
    bool PeekSep() {
        const Tok k = Peek().kind;
        return k == Tok::Comma || k == Tok::Semi || k == Tok::Newline;
    }
    bool PeekEnd() {
        const Token &t = Peek();
        return t.kind == Tok::Keyword && EndTypeOf(t.text) != EndType::None;
    }

    // --- errors ---
    [[noreturn]] void ErrorAt(const std::string &msg, int line, int col, int tok = -1) {
        if (out_.error.empty()) {
            out_.error = msg;
            out_.error_line = line;
            out_.error_col = col;
            out_.error_tok = tok;
        }
        throw ParseAbort{};
    }
    // A syntax error at the lookahead: where Octave's lexer stood after it.
    [[noreturn]] void SyntaxError() {
        const Token &t = Peek();
        std::string msg = "syntax error";
        if (!err_ctx_.empty())
            msg = err_ctx_.back() == 'A' ? "anonymous function bodies must be single expressions" : "invalid parameter list";
        ErrorAt(msg, t.after_line, t.after_col - 1, static_cast<int>(cur_));
    }
    // bison_error (msg, tok->beg_pos ())
    [[noreturn]] void ErrorAtTokenStart(const std::string &msg, int tok) {
        const Token &t = out_.tokens[static_cast<size_t>(tok)];
        // Octave's column carries the drift of the separators its lexer
        // unput on the line (`[-1 1]end`): measure back from after_col.
        int col = t.col;
        if (!t.synthetic && t.after_line == t.line + 1 && t.end > t.begin) col = t.after_col - 1 - (t.end - t.begin);
        ErrorAt(msg, t.line + 1, col, tok);
    }
    // bison_error (msg): the lexer's position after token `tok`
    [[noreturn]] void ErrorAfter(const std::string &msg, int tok) {
        const Token &t = out_.tokens[static_cast<size_t>(tok)];
        ErrorAt(msg, t.after_line, t.after_col - 1, tok);
    }

    void Expect(std::string_view op) {
        if (!PeekOp(op)) SyntaxError();
        Take();
    }

    struct DepthGuard {
        Parser *p;
        explicit DepthGuard(Parser *pp) : p(pp) {
            if (++p->depth_ > kMaxDepth) {
                const Token &t = p->Peek();
                p->ErrorAt("nesting too deep", t.after_line, t.after_col - 1, static_cast<int>(p->cur_));
            }
        }
        ~DepthGuard() { p->depth_--; }
    };

    int NewNode(Node kind, int first) {
        AstNode n;
        n.kind = kind;
        n.first = first;
        out_.nodes.push_back(std::move(n));
        return static_cast<int>(out_.nodes.size()) - 1;
    }
    AstNode &N(int i) { return out_.nodes[static_cast<size_t>(i)]; }
    void Finish(int node) { N(node).last = static_cast<int>(cur_) - 1; }
    void Add(int node, int kid) { N(node).kids.push_back(kid); }
    int Last() const { return static_cast<int>(cur_) - 1; }

    Scope *CurScope() { return scope_stack_.empty() ? nullptr : scope_stack_.back(); }
    void MarkVariable(const std::string &name) {
        if (Scope *s = CurScope()) s->variables.insert(name);
    }

    // --- the file ---
    void File() {
        scopes_.push_back(std::make_unique<Scope>());
        scope_stack_.push_back(scopes_.back().get());
        const int root = NewNode(Node::StatementList, 0);
        out_.root = root;
        // `file : begin_file opt_sep classdef ...`
        size_t save = cur_;
        while (PeekSep()) Take();
        if (PeekKw("classdef")) {
            const int cd = Classdef();
            Add(root, cd);
            // parsing_local_fcns: functions after the class
            parsing_local_functions_ = true;
            for (;;) {
                while (PeekSep()) Take();
                if (PeekKw("function")) {
                    Add(root, Function());
                    continue;
                }
                break;
            }
            if (Peek().kind != Tok::Eof) SyntaxError();
            Finish(root);
            return;
        }
        cur_ = save;
        StatementList(root);
        if (Peek().kind != Tok::Eof) SyntaxError();
        Finish(root);
    }

    // statement_list : opt_sep opt_list; the caller checks the token after.
    void StatementList(int list) {
        while (PeekSep()) Take();
        for (;;) {
            if (!StartsStatement()) return;
            Add(list, Statement());
            if (!PeekSep()) return;
            while (PeekSep()) Take();
        }
    }
    bool StartsStatement() {
        const Token &t = Peek();
        switch (t.kind) {
        case Tok::Name:
        case Tok::Number:
        case Tok::SqString:
        case Tok::DqString:
        case Tok::FcnHandle:
        case Tok::MetaQuery:
        case Tok::SuperclassRef:
            return true;
        case Tok::Keyword: {
            const std::string &w = t.text;
            return w == "global" || w == "persistent" || w == "if" || w == "switch" || w == "while" || w == "do" ||
                   w == "for" || w == "parfor" || w == "break" || w == "continue" || w == "return" || w == "spmd" ||
                   w == "unwind_protect" || w == "try" || w == "function" || w == "__FILE__" || w == "__LINE__";
        }
        case Tok::Op: {
            const std::string &o = t.text;
            return o == "(" || o == "[" || o == "{" || o == "@" || o == "+" || o == "-" || o == "~" || o == "!" ||
                   o == "++" || o == "--";
        }
        default:
            return false;
        }
    }
    void TakeTerminator(int stmt) {
        const Tok k = Peek().kind;
        if (k == Tok::Comma || k == Tok::Semi) N(stmt).sep = static_cast<int>(cur_);
    }

    int Statement() {
        DepthGuard g(this);
        const Token &t = Peek();
        if (t.kind == Tok::Keyword && t.text != "__FILE__" && t.text != "__LINE__") return Command();
        if (t.kind == Tok::Name) {
            // word_list_cmd: identifier word_list. The words are the
            // command-syntax arguments the lexer made, but any string
            // token will do: `disp"hi"` is a command too.
            const Token &n2 = Peek2();
            if (n2.kind == Tok::SqString || n2.kind == Tok::DqString) {
                const int node = NewNode(Node::Command, static_cast<int>(cur_));
                N(node).tok = Take();
                while (Peek().kind == Tok::SqString || Peek().kind == Tok::DqString) Take();
                Finish(node);
                if (Scope *s = CurScope()) s->commands.push_back(node);
                TakeTerminator(node);
                return node;
            }
        }
        const int e = Expression();
        int node = e;
        if (N(e).kind != Node::Assign) {
            node = NewNode(Node::ExprStmt, N(e).first);
            Add(node, e);
            N(node).last = N(e).last;
        }
        TakeTerminator(node);
        return node;
    }

    // --- expressions ---
    bool IsAssignOp(const Token &t) const {
        if (t.kind != Tok::Op) return false;
        static const char *const ops[] = {"=",  "+=", "-=",  "*=",  "/=",  "\\=", "^=",
                                          ".*=", "./=", ".\\=", ".^=", "&=",  "|="};
        for (const char *o : ops)
            if (t.text == o) return true;
        return false;
    }

    int Expression() {
        DepthGuard g(this);
        if (PeekOp("@")) return AnonFcn();
        const int lhs = SimpleExpr();
        if (IsAssignOp(Peek())) {
            const int lhs_list = ValidateAssignLhs(lhs);
            const int op = Take();
            const int rhs = Expression();
            return MakeAssign(lhs, lhs_list, op, rhs);
        }
        if (N(lhs).kind == Node::Matrix || N(lhs).kind == Node::Cell) ValidateArrayList(lhs);
        return lhs;
    }

    bool IsConstantExpr(int e) {
        const AstNode &n = N(e);
        switch (n.kind) {
        case Node::Number:
        case Node::String:
            return true;
        case Node::Paren:  // a parenthesized constant is still one: `(5) = 1`
            return !n.kids.empty() && IsConstantExpr(n.kids[0]);
        case Node::Range:  // Octave folds a range of constants into one
            for (int k : n.kids)
                if (!IsConstantExpr(k)) return false;
            return true;
        case Node::Matrix:
        case Node::Cell:
        case Node::Row:
            for (int k : n.kids)
                if (!IsConstantExpr(k)) return false;
            return true;
        default:
            return false;
        }
    }
    // The value of a constant expression (IsConstantExpr), as far as
    // whether it is empty: what validate_matrix_for_assignment evaluates.
    bool ConstScalar(int e, double *v) {
        e = Unparen(e);
        const AstNode &n = N(e);
        const Token &t = out_.tokens[static_cast<size_t>(n.tok < 0 ? 0 : n.tok)];
        if (n.kind == Node::Number) {
            char *endp = nullptr;
            *v = std::strtod(t.text.c_str(), &endp);
            return endp && endp != t.text.c_str();
        }
        if (n.kind == Node::String && t.text.size() == 1) {
            *v = static_cast<unsigned char>(t.text[0]);
            return true;
        }
        return false;
    }
    bool IsEmptyConst(int e) {
        e = Unparen(e);
        const AstNode &n = N(e);
        switch (n.kind) {
        case Node::String:
            return out_.tokens[static_cast<size_t>(n.tok)].text.empty();
        case Node::Matrix:
        case Node::Cell:
            if (n.kind == Node::Cell) {
                for (int r : n.kids)
                    if (!N(r).kids.empty()) return false;
                return true;
            }
            for (int r : n.kids)
                for (int k : N(r).kids)
                    if (!IsEmptyConst(k)) return false;
            return true;
        case Node::Range: {
            double a = 0, b = 0, inc = 1;
            if (n.kids.size() == 2) {
                if (!ConstScalar(n.kids[0], &a) || !ConstScalar(n.kids[1], &b)) return false;
            } else if (n.kids.size() == 3) {
                if (!ConstScalar(n.kids[0], &a) || !ConstScalar(n.kids[1], &inc) || !ConstScalar(n.kids[2], &b))
                    return false;
            } else {
                return false;
            }
            return inc == 0 || (inc > 0 && a > b) || (inc < 0 && a < b);
        }
        default:
            return false;
        }
    }
    // A parenthesized lvalue is still one: `(q) = 1`.
    int Unparen(int e) {
        while (N(e).kind == Node::Paren && !N(e).kids.empty()) e = N(e).kids[0];
        return e;
    }
    bool IsLvalue(int e) {
        const AstNode &n = N(Unparen(e));
        switch (n.kind) {
        case Node::Identifier:
        case Node::Tilde:
        case Node::Index:
        case Node::Field:
        case Node::DynField:
            return true;
        default:
            return false;
        }
    }
    void LvalueNames(int e, std::vector<std::string> *names) {
        int k = Unparen(e);
        while (N(k).kind == Node::Index || N(k).kind == Node::Field || N(k).kind == Node::DynField)
            k = Unparen(N(k).kids[0]);
        if (N(k).kind == Node::Identifier) names->push_back(out_.tokens[static_cast<size_t>(N(k).tok)].text);
    }
    // validate_matrix_for_assignment: returns the lhs node (a Row for [a, b]).
    int ValidateAssignLhs(int lhs) {
        // bison_error (msg) uses the lexer's position: after the '=' lookahead.
        const int eq = static_cast<int>(cur_);
        if (IsConstantExpr(lhs)) {
            const bool empty = IsEmptyConst(lhs);
            ErrorAfter(empty ? "invalid empty left hand side of assignment"
                             : "invalid constant left hand side of assignment",
                       eq);
        }
        std::vector<int> elems;
        if (N(lhs).kind == Node::Matrix && N(lhs).kids.size() == 1) {
            elems = N(N(lhs).kids[0]).kids;
        } else {
            elems.push_back(lhs);
        }
        for (int e : elems)
            if (!IsLvalue(e)) ErrorAfter("invalid left hand side of assignment", eq);
        std::vector<std::string> names;
        for (int e : elems) LvalueNames(e, &names);
        for (const std::string &nm : names) MarkVariable(nm);
        return lhs;
    }
    int MakeAssign(int lhs, int, int op, int rhs) {
        const bool simple = !(N(lhs).kind == Node::Matrix && N(lhs).kids.size() == 1);
        const Token &opt = out_.tokens[static_cast<size_t>(op)];
        if (!simple && opt.text != "=") ErrorAtTokenStart("computed multiple assignment not allowed", op);
        std::vector<int> elems;
        if (simple) {
            elems.push_back(lhs);
        } else {
            elems = N(N(lhs).kids[0]).kids;
        }
        for (int e : elems) {
            int k = Unparen(e);
            while (N(k).kind == Node::Index || N(k).kind == Node::Field || N(k).kind == Node::DynField)
                k = Unparen(N(k).kids[0]);
            if (N(k).kind == Node::Identifier) {
                const std::string &nm = out_.tokens[static_cast<size_t>(N(k).tok)].text;
                if (IsOctaveKeyword(nm)) ErrorAtTokenStart("invalid assignment to keyword \"" + nm + "\"", op);
            }
        }
        const int node = NewNode(Node::Assign, N(lhs).first);
        N(node).op = op;
        Add(node, lhs);
        Add(node, rhs);
        N(node).last = N(rhs).last;
        if (in_anon_body_) AnonInvalid(node, opt.text);
        return node;
    }
    void AnonInvalid(int node, const std::string &op) {
        const Token &t = out_.tokens[static_cast<size_t>(N(node).first)];
        // Reported once the body is complete; remembered here.
        if (anon_error_.empty()) {
            anon_error_ = "invalid use of operator " + op + " in anonymous function";
            anon_error_line_ = t.line + 1;
            anon_error_col_ = t.col;
        }
    }
    std::string anon_error_;
    int anon_error_line_ = 0, anon_error_col_ = 0;

    void ValidateArrayList(int e) {
        for (int row : N(e).kids)
            for (int k : N(row).kids)
                if (N(k).kind == Node::Tilde) {
                    const int at = Last() + 1;  // the lexer stands after the lookahead
                    (void)at;
                    const Token &t = Peek();
                    ErrorAt(N(e).kind == Node::Matrix ? "invalid use of tilde (~) in matrix expression"
                                                      : "invalid use of tilde (~) in cell expression",
                            t.after_line, t.after_col - 1, static_cast<int>(cur_));
                }
    }

    int AnonFcn() {
        const int at = Take();  // '@' (the lexer counted a function handle)
        const int node = NewNode(Node::AnonFcn, at);
        N(node).op = at;
        if (!PeekOp("(")) SyntaxError();
        const int params = ParamList(true);
        Add(node, params);
        // anon_fcn_begin
        lex_.at_bos = true;
        lex_.parsing_anon_fcn_body = true;
        err_ctx_.push_back('A');
        const bool saved = in_anon_body_;
        in_anon_body_ = true;
        const int body = Expression();
        in_anon_body_ = saved;
        err_ctx_.pop_back();
        Add(node, body);
        lex_.parsing_anon_fcn_body = false;
        lex_.RemoveNest();
        N(node).last = N(body).last;
        if (!anon_error_.empty() && !in_anon_body_) {
            const std::string m = anon_error_;
            ErrorAt(m, anon_error_line_, anon_error_col_);
        }
        return node;
    }

    // simple_expr: binary operators over colon_expr / oper_expr.
    static int BinPrec(const Token &t) {
        if (t.kind != Tok::Op) return -1;
        const std::string &o = t.text;
        if (o == "||") return 1;
        if (o == "&&") return 2;
        if (o == "|") return 3;
        if (o == "&") return 4;
        if (o == "<" || o == "<=" || o == "==" || o == "!=" || o == "~=" || o == ">=" || o == ">") return 5;
        return -1;
    }
    int SimpleExpr(int min_prec = 1) {
        DepthGuard g(this);
        int left = ColonExpr();
        for (;;) {
            const int p = BinPrec(Peek());
            if (p < min_prec) break;
            const int op = Take();
            const int right = SimpleExpr(p + 1);
            const int node = NewNode(Node::Binary, N(left).first);
            N(node).op = op;
            Add(node, left);
            Add(node, right);
            N(node).last = N(right).last;
            left = node;
        }
        return left;
    }
    int ColonExpr() {
        const int a = OperExpr(1);
        if (!PeekOp(":")) return a;
        const int node = NewNode(Node::Range, N(a).first);
        Add(node, a);
        N(node).op = Take();
        Add(node, OperExpr(1));
        if (PeekOp(":")) {
            Take();
            Add(node, OperExpr(1));
        }
        N(node).last = N(N(node).kids.back()).last;
        return node;
    }

    static bool IsPrefixOp(const Token &t) {
        return t.kind == Tok::Op &&
               (t.text == "+" || t.text == "-" || t.text == "~" || t.text == "!" || t.text == "++" || t.text == "--");
    }
    // Precedence levels: + - (1), * / \ .* ./ .\ (2), unary (3), ^ .^ ' .' (4), ++ -- (5), ( . { (6).
    int OperExpr(int min_prec) {
        DepthGuard g(this);
        int left;
        if (IsPrefixOp(Peek())) {
            const int op = Take();
            const int operand = OperExpr(4);
            left = NewNode(Node::Prefix, op);
            N(left).op = op;
            Add(left, operand);
            N(left).last = N(operand).last;
            const std::string &o = out_.tokens[static_cast<size_t>(op)].text;
            if (in_anon_body_ && (o == "++" || o == "--")) AnonInvalid(left, o);
        } else {
            left = Primary();
        }
        for (;;) {
            const Token &t = Peek();
            if (t.kind != Tok::Op) break;
            const std::string &o = t.text;
            if (o == "(" || o == "{") {
                left = IndexExpr(left);
                continue;
            }
            if (o == ".") {
                left = FieldExpr(left);
                continue;
            }
            if ((o == "++" || o == "--") && min_prec <= 5) {
                left = MakePostfix(left);
                if (in_anon_body_) AnonInvalid(left, o);
                continue;
            }
            if ((o == "'" || o == ".'") && min_prec <= 4) {
                left = MakePostfix(left);
                continue;
            }
            if ((o == "^" || o == ".^") && min_prec <= 4) {
                const int op = Take();
                const int right = PowerExpr();
                left = MakeBinary(left, op, right);
                continue;
            }
            if ((o == "*" || o == "/" || o == "\\" || o == ".*" || o == "./" || o == ".\\") && min_prec <= 2) {
                const int op = Take();
                const int right = OperExpr(3);
                left = MakeBinary(left, op, right);
                continue;
            }
            if ((o == "+" || o == "-") && min_prec <= 1) {
                const int op = Take();
                const int right = OperExpr(2);
                left = MakeBinary(left, op, right);
                continue;
            }
            break;
        }
        return left;
    }
    int PowerExpr() {
        DepthGuard g(this);
        int left;
        if (IsPrefixOp(Peek())) {
            const int op = Take();
            const int operand = PowerExpr();
            left = NewNode(Node::Prefix, op);
            N(left).op = op;
            Add(left, operand);
            N(left).last = N(operand).last;
            return left;
        }
        left = Primary();
        for (;;) {
            const Token &t = Peek();
            if (t.kind != Tok::Op) break;
            if (t.text == "(" || t.text == "{") {
                left = IndexExpr(left);
            } else if (t.text == ".") {
                left = FieldExpr(left);
            } else if (t.text == "++" || t.text == "--") {
                left = MakePostfix(left);
            } else {
                break;
            }
        }
        return left;
    }
    int MakeBinary(int left, int op, int right) {
        const int node = NewNode(Node::Binary, N(left).first);
        N(node).op = op;
        Add(node, left);
        Add(node, right);
        N(node).last = N(right).last;
        return node;
    }
    int MakePostfix(int left) {
        const int op = Take();
        const int node = NewNode(Node::Postfix, N(left).first);
        N(node).op = op;
        Add(node, left);
        N(node).last = op;
        return node;
    }
    int IndexExpr(int left) {
        const int open = Take();
        const bool brace = out_.tokens[static_cast<size_t>(open)].text == "{";
        const int node = NewNode(Node::Index, N(left).first);
        N(node).op = open;
        Add(node, left);
        const char *close = brace ? "}" : ")";
        if (!PeekOp(close)) ArgList(node, close);
        if (!PeekOp(close)) SyntaxError();
        Take();
        Finish(node);
        for (size_t k = 1; k < N(node).kids.size(); k++)
            if (N(N(node).kids[k]).kind == Node::Tilde)
                ErrorAfter("invalid use of empty argument (~) in index expression", Last());
        return node;
    }
    // arg_list: (expression | ':' | '~') separated by ','.
    void ArgList(int node, const char *close) {
        for (;;) {
            Add(node, Argument(close));
            if (Peek().kind == Tok::Comma) {
                Take();
                continue;
            }
            return;
        }
    }
    int Argument(const char *close) {
        const Token &t = Peek();
        if (IsOp(t, ":")) {  // magic_colon: an expression never starts with ':'
            (void)close;
            const int node = NewNode(Node::Colon, Take());
            Finish(node);
            return node;
        }
        if (IsOp(t, "~") || IsOp(t, "!")) {
            if (IsOp(t, "~")) {
                const Token &n = Peek2();
                if (n.kind == Tok::Comma || n.kind == Tok::Semi || IsOp(n, ")") || IsOp(n, "]") || IsOp(n, "}")) {
                    const int node = NewNode(Node::Tilde, Take());
                    Finish(node);
                    return node;
                }
            }
        }
        return Expression();
    }
    int FieldExpr(int left) {
        const int dot = Take();
        lex_.looking_at_indirect_ref = true;
        const Token &t = Peek();
        if (t.kind == Tok::StructElt) {
            const int node = NewNode(Node::Field, N(left).first);
            N(node).op = dot;
            N(node).tok = Take();
            lex_.looking_at_indirect_ref = false;
            Add(node, left);
            Finish(node);
            return node;
        }
        if (IsOp(t, "(")) {
            Take();
            lex_.looking_at_indirect_ref = false;
            const int e = Expression();
            if (!PeekOp(")")) SyntaxError();
            Take();
            const int node = NewNode(Node::DynField, N(left).first);
            N(node).op = dot;
            Add(node, left);
            Add(node, e);
            Finish(node);
            return node;
        }
        SyntaxError();
    }

    int Primary() {
        DepthGuard g(this);
        const Token &t = Peek();
        switch (t.kind) {
        case Tok::Name: {
            const int node = NewNode(t.text == "end" ? Node::End : Node::Identifier, static_cast<int>(cur_));
            N(node).tok = Take();
            Finish(node);
            // A command-syntax word after a name outside statement position:
            // Octave's (LALR-merged) word_list_cmd state takes the first word
            // and only then fails, on the token after it.
            if (Peek().kind == Tok::SqString && Peek().command_arg) {
                Take();
                Peek();
                SyntaxError();
            }
            return node;
        }
        case Tok::Number: {
            const int node = NewNode(Node::Number, static_cast<int>(cur_));
            N(node).tok = Take();
            Finish(node);
            return node;
        }
        case Tok::SqString:
        case Tok::DqString: {
            if (t.command_arg && cur_ != cmdarg_ok_) SyntaxError();
            const int node = NewNode(Node::String, static_cast<int>(cur_));
            N(node).tok = Take();
            Finish(node);
            return node;
        }
        case Tok::FcnHandle:
        case Tok::MetaQuery:
        case Tok::SuperclassRef: {
            const Node k = t.kind == Tok::FcnHandle   ? Node::FcnHandle
                           : t.kind == Tok::MetaQuery ? Node::MetaQuery
                                                      : Node::SuperclassRef;
            const int node = NewNode(k, static_cast<int>(cur_));
            N(node).tok = Take();
            Finish(node);
            return node;
        }
        case Tok::Keyword:
            if (t.text == "__FILE__" || t.text == "__LINE__") {
                const int node = NewNode(t.text == "__FILE__" ? Node::String : Node::Number, static_cast<int>(cur_));
                N(node).tok = Take();
                Finish(node);
                return node;
            }
            SyntaxError();
        case Tok::Op:
            if (t.text == "(") {
                const int node = NewNode(Node::Paren, static_cast<int>(cur_));
                N(node).op = Take();
                Add(node, Expression());
                if (!PeekOp(")")) SyntaxError();
                Take();
                Finish(node);
                return node;
            }
            if (t.text == "[" || t.text == "{") return MatrixOrCell();
            SyntaxError();
        default:
            SyntaxError();
        }
    }

    // matrix / cell: rows of cell_or_matrix_row separated by ';'.
    int MatrixOrCell() {
        const int open = Take();
        const bool cell = out_.tokens[static_cast<size_t>(open)].text == "{";
        const char *close = cell ? "}" : "]";
        const int node = NewNode(cell ? Node::Cell : Node::Matrix, open);
        N(node).op = open;
        for (;;) {
            // cell_or_matrix_row: [','] [arg_list [',']]
            const int row = NewNode(Node::Row, static_cast<int>(cur_));
            if (Peek().kind == Tok::Comma) Take();
            if (!(Peek().kind == Tok::Semi || PeekOp(close))) {
                ArgListInRow(row, close);
            }
            Finish(row);
            Add(node, row);
            if (Peek().kind == Tok::Semi) {
                Take();
                continue;
            }
            break;
        }
        if (!PeekOp(close)) SyntaxError();
        Take();
        Finish(node);
        return node;
    }
    void ArgListInRow(int row, const char *close) {
        for (;;) {
            Add(row, Argument(close));
            if (Peek().kind == Tok::Comma) {
                Take();
                if (Peek().kind == Tok::Semi || PeekOp(close)) return;  // trailing ','
                continue;
            }
            return;
        }
    }

    // --- commands ---
    int Command() {
        const Token &t = Peek();
        const std::string w = t.text;
        if (w == "global" || w == "persistent") return Declaration();
        if (w == "if") return IfCommand();
        if (w == "switch") return SwitchCommand();
        if (w == "while") return WhileCommand();
        if (w == "do") return DoCommand();
        if (w == "for" || w == "parfor") return ForCommand();
        if (w == "break" || w == "continue" || w == "return") {
            const int node = NewNode(Node::Jump, static_cast<int>(cur_));
            N(node).tok = Take();
            Finish(node);
            if (w != "return" && lex_.looping == 0)
                ErrorAfter(w == "break" ? "break must appear within a loop" : "continue must appear within a loop",
                           N(node).tok);
            TakeTerminator(node);
            return node;
        }
        if (w == "spmd") {
            const int node = NewNode(Node::Spmd, Take());
            const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(body);
            Finish(body);
            Add(node, body);
            CheckEnd(EndType::Spmd);
            Finish(node);
            return node;
        }
        if (w == "unwind_protect") {
            const int node = NewNode(Node::Unwind, Take());
            const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(body);
            Finish(body);
            Add(node, body);
            if (!PeekKw("unwind_protect_cleanup")) SyntaxError();
            Take();
            const int cleanup = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(cleanup);
            Finish(cleanup);
            Add(node, cleanup);
            CheckEnd(EndType::Unwind);
            Finish(node);
            return node;
        }
        if (w == "try") return TryCommand();
        if (w == "function") return Function();
        SyntaxError();
    }

    // END, checked against the construct it closes.
    void CheckEnd(EndType expected) {
        if (!PeekEnd()) SyntaxError();
        const int tok = Take();
        const EndType got = EndTypeOf(out_.tokens[static_cast<size_t>(tok)].text);
        if (got != expected && got != EndType::Simple)
            ErrorAtTokenStart(std::string("'") + EndTypeName(expected) + "' command matched by '" + EndTypeName(got) +
                                  "'",
                              tok);
    }

    int Declaration() {
        const int node = NewNode(Node::Decl, static_cast<int>(cur_));
        N(node).tok = Take();
        // decl_init_list: one or more decl_elt
        if (Peek().kind != Tok::Name) SyntaxError();
        while (Peek().kind == Tok::Name) {
            const int elt = NewNode(Node::DeclElt, static_cast<int>(cur_));
            const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
            N(id).tok = Take();
            Finish(id);
            N(elt).name = id;
            MarkVariable(out_.tokens[static_cast<size_t>(N(id).tok)].text);
            if (PeekOp("=")) {
                Take();
                Add(elt, Expression());
            }
            Finish(elt);
            Add(node, elt);
        }
        lex_.looking_at_decl_list = false;
        Finish(node);
        TakeTerminator(node);
        return node;
    }

    // `if`/`elseif`/`case` conditions: opt_sep expression stmt_begin
    int Condition(bool opt_sep) {
        if (opt_sep)
            while (PeekSep()) Take();
        const int e = Expression();
        lex_.at_bos = true;  // stmt_begin
        return e;
    }

    int IfCommand() {
        const int node = NewNode(Node::If, static_cast<int>(cur_));
        bool first = true;
        for (;;) {
            if (first ? PeekKw("if") : PeekKw("elseif")) {
                const int clause = NewNode(Node::IfClause, static_cast<int>(cur_));
                N(clause).tok = Take();
                Add(clause, Condition(true));
                const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
                StatementList(body);
                Finish(body);
                Add(clause, body);
                Finish(clause);
                Add(node, clause);
                first = false;
                continue;
            }
            break;
        }
        if (PeekKw("else")) {
            const int clause = NewNode(Node::ElseClause, static_cast<int>(cur_));
            N(clause).tok = Take();
            const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(body);
            Finish(body);
            Add(clause, body);
            Finish(clause);
            Add(node, clause);
        }
        CheckEnd(EndType::If);
        Finish(node);
        return node;
    }

    int SwitchCommand() {
        const int node = NewNode(Node::Switch, static_cast<int>(cur_));
        N(node).tok = Take();
        Add(node, Expression());
        while (PeekSep()) Take();
        while (PeekKw("case")) {
            const int c = NewNode(Node::Case, static_cast<int>(cur_));
            N(c).tok = Take();
            Add(c, Condition(true));
            const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(body);
            Finish(body);
            Add(c, body);
            Finish(c);
            Add(node, c);
        }
        if (PeekKw("otherwise")) {
            const int c = NewNode(Node::Otherwise, static_cast<int>(cur_));
            N(c).tok = Take();
            const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(body);
            Finish(body);
            Add(c, body);
            Finish(c);
            Add(node, c);
        }
        CheckEnd(EndType::Switch);
        Finish(node);
        return node;
    }

    int WhileCommand() {
        const int node = NewNode(Node::While, static_cast<int>(cur_));
        N(node).tok = Take();
        Add(node, Condition(false));
        const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
        StatementList(body);
        Finish(body);
        Add(node, body);
        CheckEnd(EndType::While);
        lex_.looping--;
        Finish(node);
        return node;
    }

    int DoCommand() {
        const int node = NewNode(Node::DoUntil, static_cast<int>(cur_));
        N(node).tok = Take();
        const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
        StatementList(body);
        Finish(body);
        Add(node, body);
        if (!PeekKw("until")) SyntaxError();
        Take();
        Add(node, Expression());
        lex_.looping--;
        Finish(node);
        return node;
    }

    int ForCommand() {
        const int node = NewNode(Node::For, static_cast<int>(cur_));
        N(node).tok = Take();
        const bool parfor = out_.tokens[static_cast<size_t>(N(node).tok)].text == "parfor";
        bool paren = false;
        int lhs;
        if (PeekOp("(")) {
            // FOR '(' assign_lhs '=' expression ')' -- or a parenthesized lhs.
            const int open = Take();
            const int inner = SimpleExpr();
            if (PeekOp("=")) {
                paren = true;
                lhs = inner;
            } else {
                if (!PeekOp(")")) SyntaxError();
                Take();
                lhs = NewNode(Node::Paren, open);
                N(lhs).op = open;
                Add(lhs, inner);
                Finish(lhs);
                ValidateAssignLhs(lhs);
            }
        } else {
            // FOR assign_lhs: simple_expr reduces to assign_lhs (and is
            // validated) on whatever lookahead ends it, '=' or not.
            lhs = SimpleExpr();
            ValidateAssignLhs(lhs);
        }
        if (!PeekOp("=")) SyntaxError();
        if (paren) ValidateAssignLhs(lhs);
        Add(node, lhs);
        Take();
        Add(node, Expression());
        if (paren) {
            if (parfor && Peek().kind == Tok::Comma) {
                Take();
                const int maxw = Expression();
                if (!PeekOp(")")) SyntaxError();
                Take();
                const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
                StatementList(body);
                Finish(body);
                Add(node, body);
                Add(node, maxw);
            } else {
                if (!PeekOp(")")) SyntaxError();
                Take();
                const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
                StatementList(body);
                Finish(body);
                Add(node, body);
            }
        } else {
            lex_.at_bos = true;  // stmt_begin
            const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(body);
            Finish(body);
            Add(node, body);
        }
        CheckEnd(parfor ? EndType::Parfor : EndType::For);
        lex_.looping--;
        Finish(node);
        return node;
    }

    int TryCommand() {
        const int node = NewNode(Node::Try, static_cast<int>(cur_));
        N(node).tok = Take();
        const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
        StatementList(body);
        Finish(body);
        Add(node, body);
        if (PeekKw("catch")) {
            const int c = NewNode(Node::Catch, static_cast<int>(cur_));
            N(c).tok = Take();
            const int catch_line = out_.tokens[static_cast<size_t>(N(c).tok)].line;
            const int cbody = NewNode(Node::StatementList, static_cast<int>(cur_));
            StatementList(cbody);
            Finish(cbody);
            // `catch err`: an identifier on the catch line is the error variable.
            if (!N(cbody).kids.empty()) {
                const int s0 = N(cbody).kids[0];
                if (N(s0).kind == Node::ExprStmt && N(N(s0).kids[0]).kind == Node::Identifier &&
                    out_.tokens[static_cast<size_t>(N(s0).first)].line == catch_line) {
                    N(c).name = N(s0).kids[0];
                    MarkVariable(out_.tokens[static_cast<size_t>(N(N(s0).kids[0]).tok)].text);
                    N(cbody).kids.erase(N(cbody).kids.begin());
                }
            }
            Add(c, cbody);
            Finish(c);
            Add(node, c);
        }
        CheckEnd(EndType::TryCatch);
        Finish(node);
        return node;
    }

    // --- functions ---
    int ParamList(bool anon) {
        // param_list_beg
        const int open = Take();
        const int node = NewNode(Node::ParamList, open);
        N(node).op = open;
        lex_.looking_at_parameter_list = true;
        lex_.arguments_is_keyword = false;
        if (lex_.looking_at_function_handle) {
            lex_.looking_at_function_handle--;
            lex_.looking_at_anon_fcn_args = true;
        }
        (void)anon;
        err_ctx_.push_back('P');
        if (!PeekOp(")")) {
            for (;;) {
                const Token &t = Peek();
                if (IsOp(t, "~")) {
                    const int td = NewNode(Node::Tilde, Take());
                    Finish(td);
                    Add(node, td);
                } else if (t.kind == Tok::Name) {
                    const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
                    N(id).tok = Take();
                    Finish(id);
                    if (PeekOp("=")) {
                        const int elt = NewNode(Node::DeclElt, N(id).first);
                        N(elt).name = id;
                        Take();
                        Add(elt, Expression());
                        Finish(elt);
                        Add(node, elt);
                    } else {
                        Add(node, id);
                    }
                } else {
                    SyntaxError();
                }
                if (Peek().kind == Tok::Comma) {
                    Take();
                    continue;
                }
                break;
            }
            // param_list2 reduces to param_list1 (and is validated) on any
            // lookahead but ',': a duplicate name is reported before the
            // stray token is.
            if (!PeekOp(")")) ValidateParamList(node, true, static_cast<int>(cur_));
        }
        if (!PeekOp(")")) SyntaxError();
        err_ctx_.pop_back();
        // param_list_end
        Take();
        lex_.looking_at_parameter_list = false;
        lex_.arguments_is_keyword = true;
        lex_.looking_for_object_index = false;
        Finish(node);
        ValidateParamList(node, true);
        // An anonymous function's parameters live in its own scope.
        for (int k : N(node).kids) {
            if (anon) break;
            const int id = N(k).kind == Node::DeclElt ? N(k).name : k;
            if (N(id).kind == Node::Identifier) MarkVariable(out_.tokens[static_cast<size_t>(N(id).tok)].text);
        }
        return node;
    }
    // `at`: the token Octave's lexer stood after (default: the last one taken).
    void ValidateParamList(int node, bool in, int at = -1) {
        if (at < 0) at = Last();
        std::set<std::string> seen;
        for (int k : N(node).kids) {
            const int id = N(k).kind == Node::DeclElt ? N(k).name : k;
            if (N(id).kind == Node::Tilde) {
                if (!in) ErrorAfter("invalid use of ~ in output list", at);
                continue;
            }
            const std::string &nm = out_.tokens[static_cast<size_t>(N(id).tok)].text;
            if (IsOctaveKeyword(nm)) ErrorAfter("invalid use of keyword '" + nm + "' in parameter list", at);
            if (seen.count(nm)) ErrorAfter("'" + nm + "' appears more than once in parameter list", at);
            seen.insert(nm);
        }
    }

    // fcn_name: identifier | GET '.' identifier | SET '.' identifier
    int FcnName(int fn) {
        const Token &t = Peek();
        if (t.kind == Tok::Keyword && (t.text == "get" || t.text == "set")) {
            const int gs = Take();
            if (!PeekOp(".")) SyntaxError();
            Take();
            if (Peek().kind != Tok::Name) SyntaxError();
            const int id = NewNode(Node::Identifier, gs);
            N(id).tok = Take();
            Finish(id);
            if (!lex_.parsed_function_name.empty()) lex_.parsed_function_name.back() = true;
            lex_.maybe_classdef_get_set_method = false;
            lex_.arguments_is_keyword = true;
            NameFunction(fn, out_.tokens[static_cast<size_t>(gs)].text + "." +
                                 out_.tokens[static_cast<size_t>(N(id).tok)].text, id, false);
            return id;
        }
        if (t.kind != Tok::Name) SyntaxError();
        const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
        N(id).tok = Take();
        Finish(id);
        // After `return_list '='`, fcn_name: identifier is a default
        // reduction -- no lookahead is read, so a following `end` is
        // lexed with the name already in (the function's end keyword).
        // Without a return list the caller has peeked past the name.
        NameFunction(fn, out_.tokens[static_cast<size_t>(N(id).tok)].text, id, true);
        lex_.arguments_is_keyword = true;
        // A name lexed at the beginning of a statement (`for i = 1:n
        // function to integrate`) may have been a command word: its
        // argument is then an SQ_STRING that starts the function's body.
        cmdarg_ok_ = cur_;
        return id;
    }
    // make_fcn_name: the duplicate check, then the lexer learns the name is in.
    void NameFunction(int fn, const std::string &name0, int id, bool check) {
        (void)fn;
        std::string name = name0;
        if (parsing_local_functions_ && fcn_depth_ == 0) name = opt_.file_base + ">" + name;
        if (check) {
            bool ok = true;
            std::string full;
            for (size_t i = 0; i + 1 < parent_names_.size(); i++) {
                if (name == parent_names_[i]) ok = false;
                full += parent_names_[i] + ">";
            }
            full += name;
            if (ok && all_names_.count(full))
                ok = !(parsing_subfunctions_ || parsing_local_functions_ || fcn_depth_ > 0);
            if (!ok) {
                const Token &t = out_.tokens[static_cast<size_t>(N(id).tok)];
                ErrorAt("duplicate subfunction or nested function name", t.line + 1, t.col + 1);
            }
            all_names_.insert(full);
        }
        if (!parent_names_.empty()) parent_names_.back() = name;
        if (!lex_.parsed_function_name.empty()) lex_.parsed_function_name.back() = true;
        lex_.maybe_classdef_get_set_method = false;
    }

    int Function() {
        DepthGuard g(this);
        // push_fcn_symtab (reduced with FUNCTION as the lookahead)
        fcn_depth_++;
        const int kw = static_cast<int>(cur_);
        if (lex_.reading_script_file && fcn_depth_ > 0)
            ErrorAfter("nested functions not implemented in this context", kw);
        Take();  // FUNCTION
        parent_names_.push_back("");
        scopes_.push_back(std::make_unique<Scope>());
        Scope *scope = scopes_.back().get();
        if (Scope *parent = CurScope()) {
            parent->kids.push_back(scope);
            if (fcn_depth_ > 0) scope->nested_in = parent;
        }
        scope_stack_.push_back(scope);
        if (lex_.reading_classdef_file || parsing_classdef_) lex_.maybe_classdef_get_set_method = true;

        const int node = NewNode(Node::Function, kw);
        N(node).tok = kw;
        const int outs = NewNode(Node::ParamList, -1);
        // return_list '=' fcn_name | fcn_name
        const Token &t = Peek();
        if (IsOp(t, "[")) {
            N(outs).first = N(outs).op = Take();
            if (!PeekOp("]")) {
                for (;;) {
                    if (Peek().kind != Tok::Name) SyntaxError();
                    const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
                    N(id).tok = Take();
                    Finish(id);
                    Add(outs, id);
                    if (Peek().kind == Tok::Comma) {
                        Take();
                        continue;
                    }
                    break;
                }
            }
            if (!PeekOp("]")) SyntaxError();
            Take();
            Finish(outs);
            lex_.looking_at_return_list = false;
            ValidateParamList(outs, false);
            if (!PeekOp("=")) SyntaxError();
            Take();
            N(node).name = FcnName(node);
        } else if (t.kind == Tok::Name) {
            // identifier: a return list if '=' follows, else the name.
            const int id_tok = static_cast<int>(cur_);
            if (IsOp(Peek2(), "=")) {
                const int id = NewNode(Node::Identifier, id_tok);
                N(id).tok = Take();
                Finish(id);
                N(outs).first = id_tok;
                Add(outs, id);
                Finish(outs);
                lex_.looking_at_return_list = false;
                ValidateParamList(outs, false);
                Take();  // '='
                N(node).name = FcnName(node);
            } else {
                N(node).name = FcnName(node);
            }
        } else if (t.kind == Tok::Keyword && (t.text == "get" || t.text == "set")) {
            N(node).name = FcnName(node);
        } else {
            SyntaxError();
        }
        // The output names are variables -- except a trailing varargout,
        // which Octave takes out of the list: `varargout x` stays a command.
        for (size_t k = 0; k < N(outs).kids.size(); ++k) {
            const int o = N(outs).kids[k];
            if (N(o).kind != Node::Identifier) continue;
            const std::string &nm = out_.tokens[static_cast<size_t>(N(o).tok)].text;
            if (k + 1 == N(outs).kids.size() && nm == "varargout") continue;
            MarkVariable(nm);
        }
        const int ins = PeekOp("(") ? ParamList(false) : NewNode(Node::ParamList, -1);
        Add(node, ins);
        Add(node, outs);
        // function_body: statement_list | opt_sep arguments_block_list statement_list
        const int body = NewNode(Node::StatementList, static_cast<int>(cur_));
        while (PeekSep()) Take();
        while (PeekKw("arguments")) {
            Add(body, ArgumentsBlock());
            while (PeekSep()) Take();
        }
        StatementList(body);
        Finish(body);
        Add(node, body);
        // function_end: END | END_OF_INPUT
        if (Peek().kind == Tok::Eof) {
            if (endfunction_found_)
                ErrorAfter("inconsistent function endings -- if one function is explicitly ended, so must all the "
                           "others",
                           static_cast<int>(cur_));
            if (!(lex_.reading_fcn_file || lex_.reading_script_file))
                ErrorAfter("function body open at end of input", static_cast<int>(cur_));
            if (lex_.reading_classdef_file) ErrorAfter("classdef body open at end of input", static_cast<int>(cur_));
        } else if (PeekEnd()) {
            endfunction_found_ = true;
            const int e = Take();
            const EndType got = EndTypeOf(out_.tokens[static_cast<size_t>(e)].text);
            if (got != EndType::Function && got != EndType::Simple)
                ErrorAtTokenStart(std::string("'endfunction' command matched by '") + EndTypeName(got) + "'", e);
            N(node).explicit_end = true;
        } else {
            SyntaxError();
        }
        Finish(node);
        scope->explicit_end = N(node).explicit_end;
        // finish_function
        lex_.defining_fcn--;
        if (!lex_.parsed_function_name.empty()) lex_.parsed_function_name.pop_back();
        lex_.looking_at_return_list = false;
        lex_.looking_at_parameter_list = false;
        scope_stack_.pop_back();
        parent_names_.pop_back();
        if (lex_.reading_fcn_file && fcn_depth_ == 0) parsing_subfunctions_ = true;
        fcn_depth_--;
        return node;
    }

    int ArgumentsBlock() {
        const int node = NewNode(Node::ArgumentsBlock, static_cast<int>(cur_));
        N(node).tok = Take();
        lex_.arguments_is_keyword = false;
        while (PeekSep()) Take();
        if (PeekOp("(")) {  // args_attr_list
            Take();
            if (Peek().kind != Tok::Name) SyntaxError();
            Take();
            if (!PeekOp(")")) SyntaxError();
            Take();
        }
        for (;;) {
            if (Peek().kind != Tok::Name) SyntaxError();
            const int v = NewNode(Node::ArgValidation, static_cast<int>(cur_));
            const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
            N(id).tok = Take();
            Finish(id);
            Add(v, id);
            ArgValidation(v);
            Finish(v);
            Add(node, v);
            if (!PeekSep()) break;
            while (PeekSep()) Take();
            if (PeekEnd()) break;
        }
        while (PeekSep()) Take();
        CheckEnd(EndType::Arguments);
        lex_.arguments_is_keyword = true;
        Finish(node);
        return node;
    }
    // size_spec class_name validation_fcns ['=' expression]
    void ArgValidation(int v) {
        if (PeekOp("(")) {
            const int idx = NewNode(Node::Row, static_cast<int>(cur_));
            Take();
            ArgList(idx, ")");
            if (!PeekOp(")")) SyntaxError();
            Take();
            Finish(idx);
            Add(v, idx);
        }
        if (Peek().kind == Tok::Name) {
            const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
            N(id).tok = Take();
            Finish(id);
            Add(v, id);
        }
        if (PeekOp("{")) {
            const int idx = NewNode(Node::Row, static_cast<int>(cur_));
            Take();
            ArgList(idx, "}");
            if (!PeekOp("}")) SyntaxError();
            Take();
            Finish(idx);
            Add(v, idx);
        }
        if (PeekOp("=")) {
            Take();
            Add(v, Expression());
        }
    }

    // --- classdef ---
    bool parsing_classdef_ = false;

    int Classdef() {
        const int kw = Take();
        if (!lex_.reading_classdef_file)
            ErrorAfter("classdef must appear inside a file containing only a class definition", kw);
        parsing_classdef_ = true;
        lex_.parsing_classdef_decl = true;
        lex_.classdef_element_names_are_keywords = true;
        const int node = NewNode(Node::Classdef, kw);
        N(node).tok = kw;
        AttrList(node);
        if (Peek().kind != Tok::Name) SyntaxError();
        const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
        N(id).tok = Take();
        Finish(id);
        N(node).name = id;
        while (PeekSep()) Take();
        // superclass_list
        if (PeekOp("<")) {
            Take();
            for (;;) {
                if (Peek().kind != Tok::FqIdent) SyntaxError();
                const int sc = NewNode(Node::Superclass, static_cast<int>(cur_));
                N(sc).tok = Take();
                Finish(sc);
                Add(node, sc);
                if (PeekOp("&")) {
                    Take();
                    continue;
                }
                break;
            }
            while (PeekSep()) Take();
        }
        lex_.parsing_classdef_decl = false;
        lex_.parsing_classdef_superclass = false;
        // class_body
        for (;;) {
            const Token &t = Peek();
            if (t.kind != Tok::Keyword) break;
            if (t.text == "properties") {
                Add(node, PropertiesBlock());
            } else if (t.text == "methods") {
                Add(node, MethodsBlock());
            } else if (t.text == "events") {
                Add(node, SimpleBlock(EndType::Events, false));
            } else if (t.text == "enumeration") {
                Add(node, SimpleBlock(EndType::Enumeration, true));
            } else {
                break;
            }
            while (PeekSep()) Take();
        }
        lex_.classdef_element_names_are_keywords = false;
        if (!PeekEnd()) SyntaxError();
        const int e = Take();
        parsing_classdef_ = false;
        if (!opt_.file_base.empty() && out_.tokens[static_cast<size_t>(N(id).tok)].text != opt_.file_base) {
            const Token &t = out_.tokens[static_cast<size_t>(N(id).tok)];
            ErrorAt("invalid classdef definition, the class name must match the filename", t.line + 1, t.col);
        }
        // Octave names the expected end 'endswitch' here (make_classdef).
        const EndType got = EndTypeOf(out_.tokens[static_cast<size_t>(e)].text);
        if (got != EndType::Classdef && got != EndType::Simple)
            ErrorAtTokenStart(std::string("'endswitch' command matched by '") + EndTypeName(got) + "'", e);
        Finish(node);
        return node;
    }
    void AttrList(int node) {
        if (!PeekOp("(")) return;
        Take();
        for (;;) {
            const int a = NewNode(Node::Attr, static_cast<int>(cur_));
            if (PeekOp("~") || PeekOp("!")) Take();
            if (Peek().kind != Tok::Name) SyntaxError();
            const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
            N(id).tok = Take();
            Finish(id);
            Add(a, id);
            if (PeekOp("=")) {
                Take();
                Add(a, Expression());
            }
            Finish(a);
            Add(node, a);
            if (Peek().kind == Tok::Comma) {
                Take();
                continue;
            }
            break;
        }
        if (!PeekOp(")")) SyntaxError();
        Take();
        while (PeekSep()) Take();
    }
    int PropertiesBlock() {
        const int node = NewNode(Node::ClassBlock, static_cast<int>(cur_));
        N(node).tok = Take();
        lex_.classdef_element_names_are_keywords = false;
        while (PeekSep()) Take();
        AttrList(node);
        // property_list: class_property (sep class_property)* opt_sep
        while (Peek().kind == Tok::Name) {
            const int p = NewNode(Node::Property, static_cast<int>(cur_));
            const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
            N(id).tok = Take();
            Finish(id);
            Add(p, id);
            ArgValidation(p);
            Finish(p);
            Add(node, p);
            if (!PeekSep()) break;
            while (PeekSep()) Take();
        }
        lex_.classdef_element_names_are_keywords = true;
        CheckEnd(EndType::Properties);
        Finish(node);
        return node;
    }
    int MethodsBlock() {
        const int node = NewNode(Node::ClassBlock, static_cast<int>(cur_));
        N(node).tok = Take();
        lex_.classdef_element_names_are_keywords = false;
        while (PeekSep()) Take();
        AttrList(node);
        for (;;) {
            const Token &t = Peek();
            if (t.kind == Tok::Keyword && t.text == "function") {
                Add(node, Function());
            } else if (t.kind == Tok::Name || IsOp(t, "[")) {
                ExternalMethodDecl();
            } else {
                break;
            }
            while (PeekSep()) Take();
        }
        lex_.classdef_element_names_are_keywords = true;
        CheckEnd(EndType::Methods);
        Finish(node);
        return node;
    }
    // method_decl: [return_list '='] identifier [param_list] -- a signature
    // for a method defined in its own file, which Octave accepts only in an
    // @-folder, and says so once the declaration has been read.
    void ExternalMethodDecl() {
        if (PeekOp("[")) {
            Take();
            while (Peek().kind == Tok::Name || Peek().kind == Tok::Comma) Take();
            if (!PeekOp("]")) SyntaxError();
            Take();
            if (!PeekOp("=")) SyntaxError();
            Take();
        } else if (Peek().kind == Tok::Name && IsOp(Peek2(), "=")) {
            Take();
            Take();
        }
        if (Peek().kind != Tok::Name) SyntaxError();
        Take();
        if (PeekOp("(")) {
            // identifier param_list is reduced as soon as ')' is read.
            ParamList(false);
            ErrorAfter("external methods are only allowed in @-folders", Last());
        }
        Peek();
        ErrorAfter("external methods are only allowed in @-folders", static_cast<int>(cur_));
    }

    int SimpleBlock(EndType et, bool enumeration) {
        const int node = NewNode(Node::ClassBlock, static_cast<int>(cur_));
        N(node).tok = Take();
        lex_.classdef_element_names_are_keywords = false;
        while (PeekSep()) Take();
        AttrList(node);
        while (Peek().kind == Tok::Name) {
            const int item = NewNode(Node::EnumItem, static_cast<int>(cur_));
            const int id = NewNode(Node::Identifier, static_cast<int>(cur_));
            N(id).tok = Take();
            Finish(id);
            Add(item, id);
            if (enumeration) {
                if (!PeekOp("(")) SyntaxError();
                Take();
                Add(item, Expression());
                if (!PeekOp(")")) SyntaxError();
                Take();
            }
            Finish(item);
            Add(node, item);
            while (PeekSep()) Take();
        }
        lex_.classdef_element_names_are_keywords = true;
        CheckEnd(et);
        Finish(node);
        return node;
    }

    // parse_tree_validator: a command-syntax call to a name the same scope assigns.
    void CheckCommandsVsVariables() {
        for (const auto &sp : scopes_) {
            for (int c : sp->commands) {
                const Token &t = out_.tokens[static_cast<size_t>(N(c).tok)];
                if (sp->IsVariable(t.text)) {
                    out_.error = t.text + ": invalid use of symbol as both variable and command";
                    out_.error_line = t.line + 1;
                    // the token's start as Octave's lexer counted it, unput drift
                    // included (`[p ex ey];ex ey`), as in ErrorAtTokenStart
                    out_.error_col = t.col;
                    if (!t.synthetic && t.after_line == t.line + 1 && t.end > t.begin)
                        out_.error_col = t.after_col - 1 - (t.end - t.begin);
                    out_.error_tok = N(c).tok;
                    return;
                }
            }
        }
    }
};

}  // namespace

bool IsKeyword(std::string_view s) { return InKeywordTable(s); }

ParseResult Parse(std::string_view src, const Options &opt) {
    ParseResult r;
    r.src = src;
    Parser p(src, opt, r);
    p.Run();
    return r;
}

std::string CheckMessage(const ParseResult &r) {
    if (r.ok()) return "ok";
    return r.error + " near line " + std::to_string(r.error_line) + ", column " + std::to_string(r.error_col);
}

}  // namespace octparse
