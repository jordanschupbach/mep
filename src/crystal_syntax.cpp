// The Crystal lexer: a port of src/compiler/crystal/syntax/lexer.cr
// (Crystal 1.19). See crystal_syntax.h. Function for function, in the
// order of the original, except that the original's character trie for
// keywords is a table lookup here with its exceptions written out
// (Lexer::next_token's identifier branch says which).

#include "crystal_syntax.h"

#include <cctype>
#include <cstdio>
#include <unordered_map>

namespace crystal {

// --- names --------------------------------------------------------------------------

const char *KeywordName(Keyword k) {
    switch (k) {
        case Keyword::None: return "";
        case Keyword::ABSTRACT: return "abstract";
        case Keyword::ALIAS: return "alias";
        case Keyword::ALIGNOF: return "alignof";
        case Keyword::ANNOTATION: return "annotation";
        case Keyword::AS: return "as";
        case Keyword::AS_QUESTION: return "as?";
        case Keyword::ASM: return "asm";
        case Keyword::BEGIN: return "begin";
        case Keyword::BREAK: return "break";
        case Keyword::CASE: return "case";
        case Keyword::CLASS: return "class";
        case Keyword::DEF: return "def";
        case Keyword::DO: return "do";
        case Keyword::ELSE: return "else";
        case Keyword::ELSIF: return "elsif";
        case Keyword::END: return "end";
        case Keyword::ENSURE: return "ensure";
        case Keyword::ENUM: return "enum";
        case Keyword::EXTEND: return "extend";
        case Keyword::FALSE: return "false";
        case Keyword::FOR: return "for";
        case Keyword::FUN: return "fun";
        case Keyword::IF: return "if";
        case Keyword::IN: return "in";
        case Keyword::INCLUDE: return "include";
        case Keyword::INSTANCE_ALIGNOF: return "instance_alignof";
        case Keyword::INSTANCE_SIZEOF: return "instance_sizeof";
        case Keyword::IS_A_QUESTION: return "is_a?";
        case Keyword::LIB: return "lib";
        case Keyword::MACRO: return "macro";
        case Keyword::MODULE: return "module";
        case Keyword::NEXT: return "next";
        case Keyword::NIL: return "nil";
        case Keyword::NIL_QUESTION: return "nil?";
        case Keyword::OF: return "of";
        case Keyword::OFFSETOF: return "offsetof";
        case Keyword::OUT: return "out";
        case Keyword::POINTEROF: return "pointerof";
        case Keyword::PRIVATE: return "private";
        case Keyword::PROTECTED: return "protected";
        case Keyword::REQUIRE: return "require";
        case Keyword::RESCUE: return "rescue";
        case Keyword::RESPONDS_TO_QUESTION: return "responds_to?";
        case Keyword::RETURN: return "return";
        case Keyword::SELECT: return "select";
        case Keyword::SELF: return "self";
        case Keyword::SIZEOF: return "sizeof";
        case Keyword::STRUCT: return "struct";
        case Keyword::SUPER: return "super";
        case Keyword::THEN: return "then";
        case Keyword::TRUE: return "true";
        case Keyword::TYPE: return "type";
        case Keyword::TYPEOF: return "typeof";
        case Keyword::UNINITIALIZED: return "uninitialized";
        case Keyword::UNION: return "union";
        case Keyword::UNLESS: return "unless";
        case Keyword::UNTIL: return "until";
        case Keyword::VERBATIM: return "verbatim";
        case Keyword::WHEN: return "when";
        case Keyword::WHILE: return "while";
        case Keyword::WITH: return "with";
        case Keyword::YIELD: return "yield";
    }
    return "";
}

const char *TokName(Tok t) {
    switch (t) {
        case Tok::EOF_: return "EOF";
        case Tok::SPACE: return "SPACE";
        case Tok::NEWLINE: return "NEWLINE";
        case Tok::IDENT: return "IDENT";
        case Tok::CONST: return "CONST";
        case Tok::INSTANCE_VAR: return "INSTANCE_VAR";
        case Tok::CLASS_VAR: return "CLASS_VAR";
        case Tok::CHAR: return "CHAR";
        case Tok::STRING: return "STRING";
        case Tok::SYMBOL: return "SYMBOL";
        case Tok::NUMBER: return "NUMBER";
        case Tok::UNDERSCORE: return "UNDERSCORE";
        case Tok::COMMENT: return "COMMENT";
        case Tok::DELIMITER_START: return "DELIMITER_START";
        case Tok::DELIMITER_END: return "DELIMITER_END";
        case Tok::STRING_ARRAY_START: return "STRING_ARRAY_START";
        case Tok::INTERPOLATION_START: return "INTERPOLATION_START";
        case Tok::SYMBOL_ARRAY_START: return "SYMBOL_ARRAY_START";
        case Tok::STRING_ARRAY_END: return "STRING_ARRAY_END";
        case Tok::GLOBAL: return "GLOBAL";
        case Tok::GLOBAL_MATCH_DATA_INDEX: return "GLOBAL_MATCH_DATA_INDEX";
        case Tok::MAGIC_DIR: return "__DIR__";
        case Tok::MAGIC_END_LINE: return "__END_LINE__";
        case Tok::MAGIC_FILE: return "__FILE__";
        case Tok::MAGIC_LINE: return "__LINE__";
        case Tok::MACRO_LITERAL: return "MACRO_LITERAL";
        case Tok::MACRO_EXPRESSION_START: return "MACRO_EXPRESSION_START";
        case Tok::MACRO_CONTROL_START: return "MACRO_CONTROL_START";
        case Tok::MACRO_VAR: return "MACRO_VAR";
        case Tok::MACRO_END: return "MACRO_END";
        case Tok::OP_BANG: return "!";
        case Tok::OP_BANG_EQ: return "!=";
        case Tok::OP_BANG_TILDE: return "!~";
        case Tok::OP_DOLLAR_QUESTION: return "$?";
        case Tok::OP_DOLLAR_TILDE: return "$~";
        case Tok::OP_PERCENT: return "%";
        case Tok::OP_PERCENT_EQ: return "%=";
        case Tok::OP_PERCENT_RCURLY: return "%}";
        case Tok::OP_AMP: return "&";
        case Tok::OP_AMP_AMP: return "&&";
        case Tok::OP_AMP_AMP_EQ: return "&&=";
        case Tok::OP_AMP_STAR: return "&*";
        case Tok::OP_AMP_STAR_STAR: return "&**";
        case Tok::OP_AMP_STAR_EQ: return "&*=";
        case Tok::OP_AMP_PLUS: return "&+";
        case Tok::OP_AMP_PLUS_EQ: return "&+=";
        case Tok::OP_AMP_MINUS: return "&-";
        case Tok::OP_AMP_MINUS_EQ: return "&-=";
        case Tok::OP_AMP_EQ: return "&=";
        case Tok::OP_LPAREN: return "(";
        case Tok::OP_RPAREN: return ")";
        case Tok::OP_STAR: return "*";
        case Tok::OP_STAR_STAR: return "**";
        case Tok::OP_STAR_STAR_EQ: return "**=";
        case Tok::OP_STAR_EQ: return "*=";
        case Tok::OP_PLUS: return "+";
        case Tok::OP_PLUS_EQ: return "+=";
        case Tok::OP_COMMA: return ",";
        case Tok::OP_MINUS: return "-";
        case Tok::OP_MINUS_EQ: return "-=";
        case Tok::OP_MINUS_GT: return "->";
        case Tok::OP_PERIOD: return ".";
        case Tok::OP_PERIOD_PERIOD: return "..";
        case Tok::OP_PERIOD_PERIOD_PERIOD: return "...";
        case Tok::OP_SLASH: return "/";
        case Tok::OP_SLASH_SLASH: return "//";
        case Tok::OP_SLASH_SLASH_EQ: return "//=";
        case Tok::OP_SLASH_EQ: return "/=";
        case Tok::OP_COLON: return ":";
        case Tok::OP_COLON_COLON: return "::";
        case Tok::OP_SEMICOLON: return ";";
        case Tok::OP_LT: return "<";
        case Tok::OP_LT_LT: return "<<";
        case Tok::OP_LT_LT_EQ: return "<<=";
        case Tok::OP_LT_EQ: return "<=";
        case Tok::OP_LT_EQ_GT: return "<=>";
        case Tok::OP_EQ: return "=";
        case Tok::OP_EQ_EQ: return "==";
        case Tok::OP_EQ_EQ_EQ: return "===";
        case Tok::OP_EQ_GT: return "=>";
        case Tok::OP_EQ_TILDE: return "=~";
        case Tok::OP_GT: return ">";
        case Tok::OP_GT_EQ: return ">=";
        case Tok::OP_GT_GT: return ">>";
        case Tok::OP_GT_GT_EQ: return ">>=";
        case Tok::OP_QUESTION: return "?";
        case Tok::OP_AT_LSQUARE: return "@[";
        case Tok::OP_LSQUARE: return "[";
        case Tok::OP_LSQUARE_RSQUARE: return "[]";
        case Tok::OP_LSQUARE_RSQUARE_EQ: return "[]=";
        case Tok::OP_LSQUARE_RSQUARE_QUESTION: return "[]?";
        case Tok::OP_RSQUARE: return "]";
        case Tok::OP_CARET: return "^";
        case Tok::OP_CARET_EQ: return "^=";
        case Tok::OP_GRAVE: return "`";
        case Tok::OP_LCURLY: return "{";
        case Tok::OP_LCURLY_PERCENT: return "{%";
        case Tok::OP_LCURLY_LCURLY: return "{{";
        case Tok::OP_BAR: return "|";
        case Tok::OP_BAR_EQ: return "|=";
        case Tok::OP_BAR_BAR: return "||";
        case Tok::OP_BAR_BAR_EQ: return "||=";
        case Tok::OP_RCURLY: return "}";
        case Tok::OP_TILDE: return "~";
    }
    return "";
}

bool IsOperator(Tok t) { return t >= Tok::OP_BANG && t <= Tok::OP_TILDE; }

bool IsAssignmentOperator(Tok t) {
    switch (t) {
        case Tok::OP_PLUS_EQ: case Tok::OP_MINUS_EQ: case Tok::OP_STAR_EQ: case Tok::OP_SLASH_EQ:
        case Tok::OP_SLASH_SLASH_EQ: case Tok::OP_PERCENT_EQ: case Tok::OP_BAR_EQ: case Tok::OP_AMP_EQ:
        case Tok::OP_CARET_EQ: case Tok::OP_STAR_STAR_EQ: case Tok::OP_LT_LT_EQ: case Tok::OP_GT_GT_EQ:
        case Tok::OP_BAR_BAR_EQ: case Tok::OP_AMP_AMP_EQ: case Tok::OP_AMP_PLUS_EQ: case Tok::OP_AMP_MINUS_EQ:
        case Tok::OP_AMP_STAR_EQ:
            return true;
        default:
            return false;
    }
}

bool IsUnaryOperator(Tok t) {
    return t == Tok::OP_BANG || t == Tok::OP_PLUS || t == Tok::OP_MINUS || t == Tok::OP_TILDE ||
           t == Tok::OP_AMP_PLUS || t == Tok::OP_AMP_MINUS;
}

bool IsMagic(Tok t) {
    return t == Tok::MAGIC_DIR || t == Tok::MAGIC_END_LINE || t == Tok::MAGIC_FILE || t == Tok::MAGIC_LINE;
}

const char *NumberKindName(NumberKind k) {
    switch (k) {
        case NumberKind::I8: return "i8";
        case NumberKind::I16: return "i16";
        case NumberKind::I32: return "i32";
        case NumberKind::I64: return "i64";
        case NumberKind::I128: return "i128";
        case NumberKind::U8: return "u8";
        case NumberKind::U16: return "u16";
        case NumberKind::U32: return "u32";
        case NumberKind::U64: return "u64";
        case NumberKind::U128: return "u128";
        case NumberKind::F32: return "f32";
        case NumberKind::F64: return "f64";
    }
    return "";
}

// --- UTF-8 and inspect ------------------------------------------------------------

std::string Utf8Encode(char32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

size_t Utf8Length(std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
    }
    return n;
}

namespace {

// Decodes one character at s[i]; returns its width (0 at the end). Invalid
// sequences decode as the byte itself with width 1 and set *bad.
int DecodeUtf8(std::string_view s, size_t i, char32_t *out, bool *bad) {
    *bad = false;
    if (i >= s.size()) {
        *out = 0;
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        *out = b0;
        return 1;
    }
    int n = 0;
    char32_t c = 0;
    if ((b0 & 0xE0) == 0xC0) {
        n = 2;
        c = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 3;
        c = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 4;
        c = b0 & 0x07;
    } else {
        *bad = true;
        *out = b0;
        return 1;
    }
    if (i + static_cast<size_t>(n) > s.size()) {
        *bad = true;
        *out = b0;
        return 1;
    }
    for (int k = 1; k < n; k++) {
        const auto b = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            *bad = true;
            *out = b0;
            return 1;
        }
        c = (c << 6) | (b & 0x3F);
    }
    if ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
        (c >= 0xD800 && c <= 0xDFFF)) {
        *bad = true;
        *out = b0;
        return 1;
    }
    *out = c;
    return n;
}

bool IsPrintableChar(char32_t c) {
    if (c < 0x20 || c == 0x7F) return false;
    if (c >= 0x80 && c < 0xA0) return false;
    if (c == 0x2028 || c == 0x2029 || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)) return false;
    if (c >= 0xD800 && c <= 0xDFFF) return false;
    if (c == 0xFEFF) return false;
    return true;
}

void AppendUnicodeEscape(std::string &out, char32_t c) {
    char buf[16];
    if (c <= 0xFFFF) {
        std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
    } else {
        std::snprintf(buf, sizeof buf, "\\u{%X}", static_cast<unsigned>(c));
    }
    out += buf;
}

}  // namespace

std::string CharInspect(char32_t c) {
    std::string out = "'";
    switch (c) {
        case '\'': out += "\\'"; break;
        case '\\': out += "\\\\"; break;
        case '\a': out += "\\a"; break;
        case '\b': out += "\\b"; break;
        case 0x1B: out += "\\e"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\v': out += "\\v"; break;
        case 0: out += "\\0"; break;
        default:
            if (IsPrintableChar(c)) {
                out += Utf8Encode(c);
            } else {
                char buf[16];
                std::snprintf(buf, sizeof buf, "\\u{%04X}", static_cast<unsigned>(c));
                out += buf;
            }
    }
    out += '\'';
    return out;
}

std::string StringInspect(std::string_view s) {
    std::string out = "\"";
    size_t i = 0;
    while (i < s.size()) {
        char32_t c;
        bool bad;
        const int w = DecodeUtf8(s, i, &c, &bad);
        if (bad) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", static_cast<unsigned>(static_cast<unsigned char>(s[i])));
            out += buf;
            i++;
            continue;
        }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '#':
                if (i + 1 < s.size() && s[i + 1] == '{') {
                    out += "\\#";
                } else {
                    out += '#';
                }
                break;
            case '\a': out += "\\a"; break;
            case '\b': out += "\\b"; break;
            case 0x1B: out += "\\e"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\v': out += "\\v"; break;
            default:
                if (IsPrintableChar(c)) {
                    out.append(s.substr(i, static_cast<size_t>(w)));
                } else if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
                    // Crystal writes control characters as \u0000-style escapes.
                    out += buf;
                } else {
                    AppendUnicodeEscape(out, c);
                }
        }
        i += static_cast<size_t>(w);
    }
    out += '"';
    return out;
}

std::string DelimiterState::end_text() const { return is_heredoc() ? heredoc : Utf8Encode(end); }

std::string Token::value_s() const {
    switch (vkind) {
        case TokValue::None: return "";
        case TokValue::Char: return Utf8Encode(ch);
        case TokValue::String: return str;
        case TokValue::Keyword: return KeywordName(kw);
    }
    return "";
}

std::string Token::to_s() const { return vkind != TokValue::None ? value_s() : std::string(TokName(type)); }

void Token::copy_from(const Token &o) {
    type = o.type;
    vkind = o.vkind;
    str = o.str;
    ch = o.ch;
    kw = o.kw;
    number_kind = o.number_kind;
    line_number = o.line_number;
    column_number = o.column_number;
    delimiter_state = o.delimiter_state;
    macro_state = o.macro_state;
    has_doc = o.has_doc;
    doc = o.doc;
}

// --- the reader ---------------------------------------------------------------------

Lexer::Lexer(std::string_view src) : src_(src) {
    decode_at(0);
}

void Lexer::decode_at(int pos) {
    pos_ = pos;
    bool bad = false;
    cur_width_ = DecodeUtf8(src_, static_cast<size_t>(pos), &cur_, &bad);
    if (bad) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Unexpected byte 0x%x at position %d, malformed UTF-8",
                      static_cast<unsigned>(static_cast<unsigned char>(src_[static_cast<size_t>(pos)])), pos);
        // InvalidByteSequenceError is not a SyntaxException; the compiler
        // reports it without a location.
        throw SyntaxError(buf, 0, 0);
    }
    if (cur_width_ == 0) cur_ = 0;
}

char32_t Lexer::peek_next_char() const {
    if (cur_width_ == 0) return 0;
    char32_t c;
    bool bad;
    const int w = DecodeUtf8(src_, static_cast<size_t>(pos_ + cur_width_), &c, &bad);
    return w == 0 ? 0 : c;
}

void Lexer::set_current_pos(int pos) { decode_at(pos); }

char32_t Lexer::next_char_no_column_increment() {
    if (cur_width_ > 0) decode_at(pos_ + cur_width_);
    return cur_;
}

void Lexer::previous_char() {
    int p = pos_;
    if (p == 0) return;
    p--;
    while (p > 0 && (static_cast<unsigned char>(src_[static_cast<size_t>(p)]) & 0xC0u) == 0x80u) p--;
    decode_at(p);
}

char32_t Lexer::next_char() {
    incr_column_number();
    return next_char_no_column_increment();
}

char32_t Lexer::next_char_check_line() {
    const char32_t c = next_char_no_column_increment();
    if (c == '\n') {
        incr_line_number();
    } else {
        incr_column_number();
    }
    return c;
}

void Lexer::incr_column_number(int d) {
    column_number += d;
    if (stacked_) stacked_column_number_ += d;
}

void Lexer::incr_line_number(int column) {
    line_number++;
    if (column >= 0) column_number = column;
    if (stacked_) {
        stacked_line_number_++;
        if (column >= 0) stacked_column_number_ = column;
    }
}

bool Lexer::char_sequence(std::initializer_list<char32_t> chars, bool column_increment) {
    for (char32_t c : chars) {
        const char32_t got = column_increment ? next_char() : next_char_no_column_increment();
        if (got != c) return false;
    }
    return true;
}

bool Lexer::handle_slash_r_slash_n_or_slash_n() {
    const bool is_slash_r = cur_ == '\r';
    if (is_slash_r) {
        if (next_char() != '\n') raise("expecting '\\n' after '\\r'");
    }
    return is_slash_r;
}

void Lexer::set_token_raw_from_start(int start) {
    if (wants_raw) token_.raw = string_range(start);
}

void Lexer::unknown_token() {
    std::string ch = CharInspect(cur_);
    raise("unknown token: " + ch, line_number, column_number);
}

char32_t Lexer::closing_char(char32_t c) {
    switch (c) {
        case '<': return '>';
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        default: return c;
    }
}

bool Lexer::is_ident(std::string_view name) {
    if (name.empty()) return false;
    char32_t c;
    bool bad;
    DecodeUtf8(name, 0, &c, &bad);
    return ident_start(c);
}

bool Lexer::is_setter(std::string_view name) { return is_ident(name) && !name.empty() && name.back() == '='; }

void Lexer::reset_token() {
    token_.vkind = TokValue::None;
    token_.str.clear();
    token_.line_number = line_number;
    token_.column_number = column_number;
    token_.has_location = false;
    token_.passed_backslash_newline = false;
    if (token_.type != Tok::SPACE && token_.type != Tok::NEWLINE) {
        token_.has_doc = false;
        token_.doc.clear();
    }
    token_.invalid_escape = false;
    has_token_end_location_ = false;
}

Location Lexer::token_end_location() {
    if (!has_token_end_location_) {
        token_end_location_ = Location{line_number, column_number - 1};
        has_token_end_location_ = true;
    }
    return token_end_location_;
}

void Lexer::next_token_skip_space() {
    next_token();
    skip_space();
}

void Lexer::next_token_skip_space_or_newline() {
    next_token();
    skip_space_or_newline();
}

void Lexer::next_token_skip_statement_end() {
    next_token();
    skip_statement_end();
}

Token &Lexer::next_token_never_a_symbol() {
    wants_symbol = false;
    next_token();
    wants_symbol = true;
    return token_;
}

void Lexer::skip_space() {
    while (token_.type == Tok::SPACE) next_token();
}

void Lexer::skip_space_or_newline() {
    while (token_.type == Tok::SPACE || token_.type == Tok::NEWLINE) next_token();
}

void Lexer::skip_statement_end() {
    while (token_.type == Tok::SPACE || token_.type == Tok::NEWLINE || token_.type == Tok::OP_SEMICOLON) next_token();
}

// --- next_token ---------------------------------------------------------------------

namespace {

const std::unordered_map<std::string_view, Keyword> &KeywordTable() {
    static const std::unordered_map<std::string_view, Keyword> table = {
        {"abstract", Keyword::ABSTRACT}, {"alias", Keyword::ALIAS}, {"alignof", Keyword::ALIGNOF},
        {"annotation", Keyword::ANNOTATION}, {"as", Keyword::AS}, {"asm", Keyword::ASM},
        {"begin", Keyword::BEGIN}, {"break", Keyword::BREAK}, {"case", Keyword::CASE},
        {"class", Keyword::CLASS}, {"def", Keyword::DEF}, {"do", Keyword::DO}, {"else", Keyword::ELSE},
        {"elsif", Keyword::ELSIF}, {"end", Keyword::END}, {"ensure", Keyword::ENSURE},
        {"enum", Keyword::ENUM}, {"extend", Keyword::EXTEND}, {"false", Keyword::FALSE},
        {"for", Keyword::FOR}, {"fun", Keyword::FUN}, {"if", Keyword::IF}, {"in", Keyword::IN},
        {"include", Keyword::INCLUDE}, {"instance_alignof", Keyword::INSTANCE_ALIGNOF},
        {"instance_sizeof", Keyword::INSTANCE_SIZEOF}, {"lib", Keyword::LIB}, {"macro", Keyword::MACRO},
        {"module", Keyword::MODULE}, {"next", Keyword::NEXT}, {"nil", Keyword::NIL}, {"of", Keyword::OF},
        {"offsetof", Keyword::OFFSETOF}, {"out", Keyword::OUT}, {"pointerof", Keyword::POINTEROF},
        {"private", Keyword::PRIVATE}, {"protected", Keyword::PROTECTED}, {"require", Keyword::REQUIRE},
        {"rescue", Keyword::RESCUE}, {"return", Keyword::RETURN}, {"select", Keyword::SELECT},
        {"self", Keyword::SELF}, {"sizeof", Keyword::SIZEOF}, {"struct", Keyword::STRUCT},
        {"super", Keyword::SUPER}, {"then", Keyword::THEN}, {"true", Keyword::TRUE}, {"type", Keyword::TYPE},
        {"typeof", Keyword::TYPEOF}, {"uninitialized", Keyword::UNINITIALIZED}, {"union", Keyword::UNION},
        {"unless", Keyword::UNLESS}, {"until", Keyword::UNTIL}, {"verbatim", Keyword::VERBATIM},
        {"when", Keyword::WHEN}, {"while", Keyword::WHILE}, {"with", Keyword::WITH}, {"yield", Keyword::YIELD},
    };
    return table;
}

bool StartsWith(std::string_view s, size_t at, std::string_view k) { return s.substr(at, k.size()) == k; }

bool IdentPartOrEndByte(std::string_view s, size_t at) {
    if (at >= s.size()) return false;
    const auto c = static_cast<unsigned char>(s[at]);
    return std::isalnum(c) || c == '_' || c >= 0x80 || c == '?' || c == '!';
}

// Whether the original lexer's keyword trie, reading the identifier at
// `at`, reaches a complete keyword (and so calls check_ident_or_keyword and
// returns early): the identifier starts with a keyword, except where the
// trie peeks past a keyword that prefixes another (in/include/instance_*,
// of/offsetof, type/typeof).
bool TrieMatchesKeyword(std::string_view s, size_t at) {
    if (StartsWith(s, at, "as") || StartsWith(s, at, "nil")) return true;
    if (StartsWith(s, at, "in")) {
        if (!IdentPartOrEndByte(s, at + 2)) return true;
        return StartsWith(s, at, "include") || StartsWith(s, at, "instance_sizeof") ||
               StartsWith(s, at, "instance_alignof");
    }
    if (StartsWith(s, at, "of")) {
        if (at + 2 < s.size() && s[at + 2] == 'f') return StartsWith(s, at, "offsetof");
        return true;
    }
    if (StartsWith(s, at, "type")) {
        if (at + 4 < s.size() && s[at + 4] == 'o') return StartsWith(s, at, "typeof");
        return true;
    }
    static const char *const kKeywords[] = {
        "abstract", "alias", "alignof", "annotation", "begin", "break", "case", "class", "def", "do", "else",
        "elsif", "end", "ensure", "enum", "extend", "false", "for", "fun", "if", "is_a?", "lib", "macro", "module",
        "next", "out", "pointerof", "private", "protected", "rescue", "responds_to?", "return", "require", "select",
        "self", "sizeof", "struct", "super", "then", "true", "union", "uninitialized", "unless", "until", "verbatim",
        "when", "while", "with", "yield",
    };
    for (const char *k : kKeywords) {
        if (StartsWith(s, at, k)) return true;
    }
    return false;
}

bool IsUppercase(char32_t c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c < 0x80) return false;
    // Latin-1, Latin Extended-A/B (even code points are capitals in most
    // of A), Greek and Cyrillic capitals: enough for the identifiers real
    // Crystal code names its constants with.
    if ((c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE)) return true;
    if (c >= 0x100 && c <= 0x17F) return (c % 2) == 0 && c != 0x138;
    if (c >= 0x391 && c <= 0x3AB) return c != 0x3A2;
    if (c >= 0x400 && c <= 0x42F) return true;
    return false;
}

}  // namespace

Token &Lexer::next_token() {
    // Check previous token:
    if (token_.type == Tok::NEWLINE || token_.type == Tok::EOF_) {
        comment_is_doc_ = true;
    } else if (token_.type != Tok::SPACE) {
        comment_is_doc_ = false;
    }

    reset_token();

    // Skip comments
    while (cur_ == '#') {
        const int start = pos_;
        if (lookahead([&] { return char_sequence({'<', 'l', 'o', 'c', ':'}, false); })) {
            next_char_no_column_increment();
            consume_loc_pragma();
        } else {
            if (comment_sink) {
                const size_t nl = src_.find('\n', static_cast<size_t>(start));
                comment_sink->emplace_back(src_.substr(static_cast<size_t>(start), nl == std::string_view::npos ? std::string_view::npos : nl - static_cast<size_t>(start)));
            }
            if (doc_enabled && comment_is_doc_) {
                consume_doc();
            } else if (comments_enabled) {
                return consume_comment(start);
            } else {
                skip_comment();
            }
        }
    }

    int start = pos_;
    bool reset_regex_flags = true;

    switch (cur_) {
        case 0:
            token_.type = Tok::EOF_;
            break;
        case ' ':
        case '\t':
            consume_whitespace();
            reset_regex_flags = false;
            break;
        case '\\': {
            const char32_t c = next_char();
            if (c == '\r' || c == '\n') {
                handle_slash_r_slash_n_or_slash_n();
                incr_line_number(0);
                token_.passed_backslash_newline = true;
                consume_whitespace();
                reset_regex_flags = false;
            } else {
                unknown_token();
            }
            break;
        }
        case '\n':
            next_char_tok(Tok::NEWLINE);
            incr_line_number();
            reset_regex_flags = false;
            consume_newlines();
            break;
        case '\r':
            if (next_char() == '\n') {
                next_char_tok(Tok::NEWLINE);
                incr_line_number();
                reset_regex_flags = false;
                consume_newlines();
            } else {
                raise("expected '\\n' after '\\r'");
            }
            break;
        case '=':
            switch (next_char()) {
                case '=':
                    if (next_char() == '=') {
                        next_char_tok(Tok::OP_EQ_EQ_EQ);
                    } else {
                        token_.type = Tok::OP_EQ_EQ;
                    }
                    break;
                case '>': next_char_tok(Tok::OP_EQ_GT); break;
                case '~': next_char_tok(Tok::OP_EQ_TILDE); break;
                default: token_.type = Tok::OP_EQ;
            }
            break;
        case '!':
            switch (next_char()) {
                case '=': next_char_tok(Tok::OP_BANG_EQ); break;
                case '~': next_char_tok(Tok::OP_BANG_TILDE); break;
                default: token_.type = Tok::OP_BANG;
            }
            break;
        case '<':
            switch (next_char()) {
                case '=':
                    if (next_char() == '>') {
                        next_char_tok(Tok::OP_LT_EQ_GT);
                    } else {
                        token_.type = Tok::OP_LT_EQ;
                    }
                    break;
                case '<':
                    switch (next_char()) {
                        case '=': next_char_tok(Tok::OP_LT_LT_EQ); break;
                        case '-': consume_heredoc_start(start); break;
                        default: token_.type = Tok::OP_LT_LT;
                    }
                    break;
                default: token_.type = Tok::OP_LT;
            }
            break;
        case '>':
            switch (next_char()) {
                case '=': next_char_tok(Tok::OP_GT_EQ); break;
                case '>':
                    if (next_char() == '=') {
                        next_char_tok(Tok::OP_GT_GT_EQ);
                    } else {
                        token_.type = Tok::OP_GT_GT;
                    }
                    break;
                default: token_.type = Tok::OP_GT;
            }
            break;
        case '+': {
            token_.start = start;
            const char32_t c = next_char();
            if (c == '=') {
                next_char_tok(Tok::OP_PLUS_EQ);
            } else if (c >= '0' && c <= '9') {
                scan_number(start);
            } else if (c == '+') {
                raise("postfix increment is not supported, use `exp += 1`");
            } else {
                token_.type = Tok::OP_PLUS;
            }
            break;
        }
        case '-': {
            token_.start = start;
            const char32_t c = next_char();
            if (c == '=') {
                next_char_tok(Tok::OP_MINUS_EQ);
            } else if (c == '>') {
                next_char_tok(Tok::OP_MINUS_GT);
            } else if (c >= '0' && c <= '9') {
                scan_number(start, true);
            } else if (c == '-') {
                raise("postfix decrement is not supported, use `exp -= 1`");
            } else {
                token_.type = Tok::OP_MINUS;
            }
            break;
        }
        case '*':
            switch (next_char()) {
                case '=': next_char_tok(Tok::OP_STAR_EQ); break;
                case '*':
                    if (next_char() == '=') {
                        next_char_tok(Tok::OP_STAR_STAR_EQ);
                    } else {
                        token_.type = Tok::OP_STAR_STAR;
                    }
                    break;
                default: token_.type = Tok::OP_STAR;
            }
            break;
        case '/': {
            const char32_t c = next_char();
            if ((wants_def_or_macro_name_ || !slash_is_regex) && c == '/') {
                if (next_char() == '=') {
                    next_char_tok(Tok::OP_SLASH_SLASH_EQ);
                } else {
                    token_.type = Tok::OP_SLASH_SLASH;
                }
            } else if (!slash_is_regex && c == '=') {
                next_char_tok(Tok::OP_SLASH_EQ);
            } else if (wants_def_or_macro_name_) {
                token_.type = Tok::OP_SLASH;
            } else if (slash_is_regex) {
                delimited_pair(DelimiterKind::REGEX, '/', '/', start, true, false);
            } else if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == 0) {
                token_.type = Tok::OP_SLASH;
            } else if (wants_regex_) {
                delimited_pair(DelimiterKind::REGEX, '/', '/', start, true, false);
            } else {
                token_.type = Tok::OP_SLASH;
            }
            break;
        }
        case '%':
            if (wants_def_or_macro_name_) {
                next_char_tok(Tok::OP_PERCENT);
            } else {
                const char32_t c = next_char();
                auto is_open = [](char32_t ch) { return ch == '(' || ch == '[' || ch == '{' || ch == '<' || ch == '|'; };
                switch (c) {
                    case '=': next_char_tok(Tok::OP_PERCENT_EQ); break;
                    case '(':
                    case '[':
                    case '{':
                    case '<':
                    case '|':
                        delimited_pair(DelimiterKind::STRING, cur_, closing_char(), start);
                        break;
                    case 'i':
                        if (is_open(peek_next_char())) {
                            const char32_t start_char = next_char();
                            next_char_tok(Tok::SYMBOL_ARRAY_START);
                            if (wants_raw) token_.raw = "%i" + Utf8Encode(start_char);
                            token_.delimiter_state =
                                DelimiterState::Make(DelimiterKind::SYMBOL_ARRAY, start_char, closing_char(start_char));
                        } else {
                            token_.type = Tok::OP_PERCENT;
                        }
                        break;
                    case 'q':
                        if (is_open(peek_next_char())) {
                            next_char();
                            delimited_pair(DelimiterKind::STRING, cur_, closing_char(), start, false);
                        } else {
                            token_.type = Tok::OP_PERCENT;
                        }
                        break;
                    case 'Q':
                        if (is_open(peek_next_char())) {
                            next_char();
                            delimited_pair(DelimiterKind::STRING, cur_, closing_char(), start);
                        } else {
                            token_.type = Tok::OP_PERCENT;
                        }
                        break;
                    case 'r':
                        if (is_open(peek_next_char())) {
                            next_char();
                            delimited_pair(DelimiterKind::REGEX, cur_, closing_char(), start);
                        } else {
                            token_.type = Tok::OP_PERCENT;
                        }
                        break;
                    case 'x':
                        if (is_open(peek_next_char())) {
                            next_char();
                            delimited_pair(DelimiterKind::COMMAND, cur_, closing_char(), start);
                        } else {
                            token_.type = Tok::OP_PERCENT;
                        }
                        break;
                    case 'w':
                        if (is_open(peek_next_char())) {
                            const char32_t start_char = next_char();
                            next_char_tok(Tok::STRING_ARRAY_START);
                            if (wants_raw) token_.raw = "%w" + Utf8Encode(start_char);
                            token_.delimiter_state =
                                DelimiterState::Make(DelimiterKind::STRING_ARRAY, start_char, closing_char(start_char));
                        } else {
                            token_.type = Tok::OP_PERCENT;
                        }
                        break;
                    case '}': next_char_tok(Tok::OP_PERCENT_RCURLY); break;
                    default: token_.type = Tok::OP_PERCENT;
                }
            }
            break;
        case '(': next_char_tok(Tok::OP_LPAREN); break;
        case ')': next_char_tok(Tok::OP_RPAREN); break;
        case '{':
            switch (next_char()) {
                case '%': next_char_tok(Tok::OP_LCURLY_PERCENT); break;
                case '{': next_char_tok(Tok::OP_LCURLY_LCURLY); break;
                default: token_.type = Tok::OP_LCURLY;
            }
            break;
        case '}': next_char_tok(Tok::OP_RCURLY); break;
        case '[':
            if (next_char() == ']') {
                switch (next_char()) {
                    case '=': next_char_tok(Tok::OP_LSQUARE_RSQUARE_EQ); break;
                    case '?': next_char_tok(Tok::OP_LSQUARE_RSQUARE_QUESTION); break;
                    default: token_.type = Tok::OP_LSQUARE_RSQUARE;
                }
            } else {
                token_.type = Tok::OP_LSQUARE;
            }
            break;
        case ']': next_char_tok(Tok::OP_RSQUARE); break;
        case ',': next_char_tok(Tok::OP_COMMA); break;
        case '?': next_char_tok(Tok::OP_QUESTION); break;
        case ';':
            reset_regex_flags = false;
            next_char_tok(Tok::OP_SEMICOLON);
            break;
        case ':':
            if (next_char() == ':') {
                next_char_tok(Tok::OP_COLON_COLON);
            } else if (wants_symbol) {
                consume_symbol();
            } else {
                token_.type = Tok::OP_COLON;
            }
            break;
        case '~': next_char_tok(Tok::OP_TILDE); break;
        case '.': {
            const int line = line_number, column = column_number;
            const char32_t c = next_char();
            if (c == '.') {
                if (next_char() == '.') {
                    next_char_tok(Tok::OP_PERIOD_PERIOD_PERIOD);
                } else {
                    token_.type = Tok::OP_PERIOD_PERIOD;
                }
            } else if (c >= '0' && c <= '9') {
                raise(".1 style number literal is not supported, put 0 before dot", line, column);
            } else {
                token_.type = Tok::OP_PERIOD;
            }
            break;
        }
        case '&':
            switch (next_char()) {
                case '&':
                    if (next_char() == '=') {
                        next_char_tok(Tok::OP_AMP_AMP_EQ);
                    } else {
                        token_.type = Tok::OP_AMP_AMP;
                    }
                    break;
                case '=': next_char_tok(Tok::OP_AMP_EQ); break;
                case '+':
                    if (next_char() == '=') {
                        next_char_tok(Tok::OP_AMP_PLUS_EQ);
                    } else {
                        token_.type = Tok::OP_AMP_PLUS;
                    }
                    break;
                case '-':
                    // `&->` parses like `&(->...)`: only `&` for now.
                    if (peek_next_char() == '>') {
                        token_.type = Tok::OP_AMP;
                    } else if (next_char() == '=') {
                        next_char_tok(Tok::OP_AMP_MINUS_EQ);
                    } else {
                        token_.type = Tok::OP_AMP_MINUS;
                    }
                    break;
                case '*':
                    switch (next_char()) {
                        case '*': next_char_tok(Tok::OP_AMP_STAR_STAR); break;
                        case '=': next_char_tok(Tok::OP_AMP_STAR_EQ); break;
                        default: token_.type = Tok::OP_AMP_STAR;
                    }
                    break;
                default: token_.type = Tok::OP_AMP;
            }
            break;
        case '|':
            switch (next_char()) {
                case '|':
                    if (next_char() == '=') {
                        next_char_tok(Tok::OP_BAR_BAR_EQ);
                    } else {
                        token_.type = Tok::OP_BAR_BAR;
                    }
                    break;
                case '=': next_char_tok(Tok::OP_BAR_EQ); break;
                default: token_.type = Tok::OP_BAR;
            }
            break;
        case '^':
            if (next_char() == '=') {
                next_char_tok(Tok::OP_CARET_EQ);
            } else {
                token_.type = Tok::OP_CARET;
            }
            break;
        case '\'': {
            const int line = line_number, column = column_number;
            token_.type = Tok::CHAR;
            const char32_t char1 = next_char();
            if (char1 == '\\') {
                const char32_t char2 = next_char();
                switch (char2) {
                    case '\\': token_.set_char('\\'); break;
                    case '\'': token_.set_char('\''); break;
                    case 'a': token_.set_char('\a'); break;
                    case 'b': token_.set_char('\b'); break;
                    case 'e': token_.set_char(0x1B); break;
                    case 'f': token_.set_char('\f'); break;
                    case 'n': token_.set_char('\n'); break;
                    case 'r': token_.set_char('\r'); break;
                    case 't': token_.set_char('\t'); break;
                    case 'v': token_.set_char('\v'); break;
                    case 'u': token_.set_char(consume_char_unicode_escape()); break;
                    case '0': token_.set_char(0); break;
                    case 0: raise("unterminated char literal", line, column);
                    default: raise("invalid char escape sequence '\\" + Utf8Encode(char2) + "'", line, column);
                }
            } else if (char1 == '\'') {
                raise("invalid empty char literal (did you mean '\\''?)", line, column);
            } else if (char1 == 0) {
                raise("unterminated char literal", line, column);
            } else {
                token_.set_char(char1);
            }
            if (next_char() != '\'') {
                raise("unterminated char literal, use double quotes for strings", line, column);
            }
            next_char();
            set_token_raw_from_start(start);
            break;
        }
        case '`':
            if (wants_def_or_macro_name_) {
                next_char_tok(Tok::OP_GRAVE);
            } else {
                delimited_pair(DelimiterKind::COMMAND, '`', '`', start);
            }
            break;
        case '"':
            delimited_pair(DelimiterKind::STRING, '"', '"', start);
            break;
        case '@':
            switch (next_char()) {
                case '[': next_char_tok(Tok::OP_AT_LSQUARE); break;
                case '@':
                    next_char();
                    consume_variable(Tok::CLASS_VAR, start);
                    break;
                default: consume_variable(Tok::INSTANCE_VAR, start);
            }
            break;
        case '$': {
            const char32_t c = next_char();
            if (c == '~') {
                next_char_tok(Tok::OP_DOLLAR_TILDE);
            } else if (c == '?') {
                next_char_tok(Tok::OP_DOLLAR_QUESTION);
            } else if (c >= '0' && c <= '9') {
                start = pos_;
                if (cur_ == '0') {
                    next_char();
                } else {
                    while (true) {
                        const char32_t d = next_char();
                        if (!(d >= '0' && d <= '9')) break;
                    }
                    if (cur_ == '?') next_char();
                }
                token_.type = Tok::GLOBAL_MATCH_DATA_INDEX;
                token_.set_str(string_range(start));
            } else {
                consume_variable(Tok::GLOBAL, start);
            }
            break;
        }
        default:
            if (cur_ >= '0' && cur_ <= '9') {
                scan_number(start);
                break;
            }
            if (cur_ == '_' || (cur_ >= 'a' && cur_ <= 'z')) {
                // The original walks a character trie over the keywords;
                // the result is the same as reading the whole identifier
                // and looking it up, with these exceptions kept:
                //   - `as?` is the keyword whatever follows the `?`;
                //   - `nil?`, `is_a?`, `responds_to?` are keywords unless an
                //     identifier character, `?` or `!` follows the `?`;
                //   - a lone `_` is UNDERSCORE (even before `?`/`!`);
                //   - `__DIR__`, `__FILE__`, `__LINE__`, `__END_LINE__`.
                while (ident_part(cur_)) next_char();
                const std::string word = string_range(start);
                if (word == "_") {
                    token_.type = Tok::UNDERSCORE;
                    return token_;
                }
                if (word == "__DIR__" || word == "__FILE__" || word == "__LINE__" || word == "__END_LINE__") {
                    if (!ident_part_or_end(cur_)) {
                        token_.type = word == "__DIR__"    ? Tok::MAGIC_DIR
                                      : word == "__FILE__" ? Tok::MAGIC_FILE
                                      : word == "__LINE__" ? Tok::MAGIC_LINE
                                                           : Tok::MAGIC_END_LINE;
                        return token_;
                    }
                }
                if (cur_ == '?' && word == "as") {
                    next_char_tok(Tok::IDENT);
                    token_.set_kw(Keyword::AS_QUESTION);
                    return token_;
                }
                if (cur_ == '?' && (word == "nil" || word == "is_a" || word == "responds_to")) {
                    if (!ident_part_or_end(peek_next_char())) {
                        next_char_tok(Tok::IDENT);
                        token_.set_kw(word == "nil"    ? Keyword::NIL_QUESTION
                                      : word == "is_a" ? Keyword::IS_A_QUESTION
                                                       : Keyword::RESPONDS_TO_QUESTION);
                        return token_;
                    }
                    // `nil?x`: the identifier `nil?`, then `x`.
                    next_char();
                    token_.type = Tok::IDENT;
                    token_.set_str(string_range(start));
                    return token_;
                }
                if (!ident_part_or_end(cur_)) {
                    const auto &table = KeywordTable();
                    auto it = table.find(word);
                    if (it != table.end()) {
                        token_.type = Tok::IDENT;
                        token_.set_kw(it->second);
                        return token_;
                    }
                }
                // An identifier the original's trie reads by matching a whole
                // keyword first ("assert", "default", "superclass") returns
                // from there, skipping the regex-flag reset below; any other
                // falls through to it. Which one matters for `a/b`.
                scan_ident(start);
                if (TrieMatchesKeyword(src_, static_cast<size_t>(start))) return token_;
                break;
            }
            if (IsUppercase(cur_)) {
                while (ident_part(next_char())) {
                }
                token_.type = Tok::CONST;
                token_.set_str(string_range(start));
            } else if (ident_start(cur_)) {
                next_char();
                scan_ident(start);
            } else {
                unknown_token();
            }
    }

    if (reset_regex_flags) {
        wants_regex_ = true;
        slash_is_regex = false;
    }
    return token_;
}

Token &Lexer::consume_comment(int start) {
    skip_comment();
    token_.type = Tok::COMMENT;
    token_.set_str(string_range(start));
    return token_;
}

void Lexer::consume_doc() {
    if (cur_ == ' ') next_char();
    const int start = pos_;
    skip_comment();
    if (token_.has_doc) {
        token_.doc += '\n';
    } else {
        token_.has_doc = true;
        token_.doc.clear();
    }
    token_.doc += string_range(start);
}

void Lexer::skip_comment() {
    char32_t c = cur_;
    while (c != '\n' && c != 0) c = next_char_no_column_increment();
}

void Lexer::consume_whitespace() {
    const int start = pos_;
    next_char_tok(Tok::SPACE);
    while (true) {
        if (cur_ == ' ' || cur_ == '\t') {
            next_char();
        } else if (cur_ == '\\') {
            const char32_t c = next_char();
            if (c == '\r' || c == '\n') {
                handle_slash_r_slash_n_or_slash_n();
                next_char();
                incr_line_number();
                token_.passed_backslash_newline = true;
            } else {
                unknown_token();
            }
        } else {
            break;
        }
    }
    if (count_whitespace) token_.set_str(string_range(start));
}

void Lexer::consume_newlines() {
    if (!heredocs.empty()) return;
    if (count_whitespace) return;
    while (true) {
        if (cur_ == '\n') {
            next_char_no_column_increment();
            incr_line_number(-1);
            token_.has_doc = false;
            token_.doc.clear();
        } else if (cur_ == '\r') {
            if (next_char_no_column_increment() != '\n') raise("expected '\\n' after '\\r'");
            next_char_no_column_increment();
            incr_line_number(-1);
            token_.has_doc = false;
            token_.doc.clear();
        } else {
            break;
        }
    }
}

Token &Lexer::check_ident_or_keyword(Keyword k, int start) {
    if (ident_part_or_end(peek_next_char())) {
        scan_ident(start);
    } else {
        next_char_tok(Tok::IDENT);
        token_.set_kw(k);
    }
    return token_;
}

Token &Lexer::scan_ident(int start) {
    while (ident_part(cur_)) next_char();
    if ((cur_ == '?' || cur_ == '!') && peek_next_char() != '=') next_char();
    token_.type = Tok::IDENT;
    token_.set_str(string_range(start));
    return token_;
}

void Lexer::symbol(const std::string &value) {
    token_.type = Tok::SYMBOL;
    token_.set_str(value);
    if (wants_raw) token_.raw = ":" + value;
}

// --- numbers ------------------------------------------------------------------------

namespace {

int DigitValue(char32_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 10;
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A') + 10;
    return 99;
}

// Decimal string of an unsigned number given in `base` (underscores
// allowed); empty if it does not fit in 128 bits.
std::string ToBase10(const std::string &s, int base) {
    unsigned __int128 v = 0;
    const unsigned __int128 max = ~static_cast<unsigned __int128>(0);
    bool any = false;
    for (char c : s) {
        if (c == '_') continue;
        const int d = DigitValue(static_cast<unsigned char>(c));
        if (d >= base) return "";
        if (v > (max - static_cast<unsigned>(d)) / static_cast<unsigned>(base)) return "";
        v = v * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        any = true;
    }
    if (!any) return "";
    if (v == 0) return "0";
    std::string out;
    while (v > 0) {
        out += static_cast<char>('0' + static_cast<int>(v % 10));
        v /= 10;
    }
    return std::string(out.rbegin(), out.rend());
}

// Compares a decimal digit string (optionally signed) against a bound
// given as a decimal string: does |s| fit (to_iN? / to_uN? semantics)?
bool DecimalFits(const std::string &raw, bool is_unsigned, int bits) {
    std::string s = raw;
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        s = s.substr(1);
    }
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') i++;
    s = s.substr(i);
    // The bound.
    unsigned __int128 lim;
    if (is_unsigned) {
        lim = bits == 128 ? ~static_cast<unsigned __int128>(0)
                          : ((static_cast<unsigned __int128>(1) << bits) - 1);
        if (neg) return s == "0";
    } else {
        lim = (static_cast<unsigned __int128>(1) << (bits - 1)) - (neg ? 0 : 1);
    }
    unsigned __int128 v = 0;
    for (char c : s) {
        const unsigned d = static_cast<unsigned>(c - '0');
        if (v > (lim - d) / 10) return false;
        v = v * 10 + d;
    }
    return v <= lim;
}

}  // namespace

void Lexer::raise_value_doesnt_fit_in(const char *type, int start, int pos_before_suffix, const char *alternative) {
    std::string msg = string_range(start, pos_before_suffix) + " doesn't fit in an " + type;
    if (alternative) msg += std::string(", try using the suffix ") + alternative;
    raise_at_token(msg, token_, pos_ - start);
}

void Lexer::check_int_fits(const char *type, bool is_unsigned, int size, int number_size,
                           const std::string &raw_number_string, int start, int pos_before_suffix, bool negative,
                           bool has_value) {
    if (is_unsigned && negative) {
        raise_at_token("Invalid negative value " + string_range(start, pos_before_suffix) + " for " + type, token_,
                       pos_ - start);
    }
    int bits = 0;
    const std::string t = type;
    if (t.find("128") != std::string::npos) {
        bits = 128;
    } else if (t.find("64") != std::string::npos) {
        bits = 64;
    } else if (t.find("32") != std::string::npos) {
        bits = 32;
    } else if (t.find("16") != std::string::npos) {
        bits = 16;
    } else {
        bits = 8;
    }
    if (!has_value || number_size > size ||
        (number_size == size && !DecimalFits(raw_number_string, is_unsigned, bits))) {
        raise_value_doesnt_fit_in(type, start, pos_before_suffix);
    }
}

void Lexer::scan_number(int start, bool negative) {
    token_.type = Tok::NUMBER;
    int base = 10;
    int number_size = 0;
    int suffix_size = 0;
    bool is_decimal = false;
    bool is_e_notation = false;
    bool has_underscores = false;
    bool last_is_underscore = false;
    int pos_after_prefix = start;
    int pos_before_exponent = -1;

    if (cur_ == '0') {
        const char32_t c = next_char();
        if (c == 'b') {
            base = 2;
        } else if (c == 'o') {
            base = 8;
        } else if (c == 'x') {
            base = 16;
        } else if (c >= '0' && c <= '9') {
            raise_at_token("octal constants should be prefixed with 0o", token_, pos_ - start);
        } else if (c == '_') {
            const char32_t n = next_char();
            if (n >= '0' && n <= '9') raise_at_token("octal constants should be prefixed with 0o", token_, pos_ - start);
            has_underscores = last_is_underscore = true;
        }
        if (base != 10) {
            next_char();
            pos_after_prefix = pos_;
            if (cur_ == '_') raise_at_token("unexpected '_' in number", token_, pos_ - start);
            const int digit = cur_ < 128 ? DigitValue(cur_) : 99;
            if (!(digit < base)) raise_at_token("numeric literal without digits", token_, pos_ - start);
        }
    }

    while (true) {
        while (true) {
            const int digit = cur_ < 128 ? DigitValue(cur_) : 99;
            if (!(digit < base)) break;
            if (!(number_size == 0 && cur_ == '0')) number_size++;
            next_char();
            last_is_underscore = false;
        }
        if (pos_before_exponent >= 0) {
            if (!(pos_ > pos_before_exponent)) raise_at_token("invalid decimal number exponent", token_, pos_ - start);
        }
        bool stop = false;
        switch (cur_) {
            case '_':
                if (last_is_underscore)
                    raise_at_token("consecutive underscores in numbers aren't allowed", token_, pos_ - start);
                has_underscores = last_is_underscore = true;
                break;
            case '.': {
                if (last_is_underscore) raise_at_token("unexpected '_' in number", token_, pos_ - start);
                const char32_t p = peek_next_char();
                if (is_decimal || base != 10 || !(p >= '0' && p <= '9')) {
                    stop = true;
                    break;
                }
                is_decimal = true;
                break;
            }
            case 'e':
            case 'E': {
                last_is_underscore = false;
                if (is_e_notation || base != 10) {
                    stop = true;
                    break;
                }
                is_e_notation = is_decimal = true;
                const char32_t p = peek_next_char();
                if (p == '+' || p == '-') next_char();
                if (peek_next_char() == '_') raise_at_token("unexpected '_' in number", token_, pos_ - start);
                pos_before_exponent = pos_ + 1;
                break;
            }
            case 'i':
            case 'u':
            case 'f': {
                if (cur_ == 'f' && base != 10) {
                    if (base == 2) raise_at_token("binary float literal is not supported", token_, pos_ - start);
                    if (base == 8) raise_at_token("octal float literal is not supported", token_, pos_ - start);
                    stop = true;
                    break;
                }
                const int before_suffix_pos = pos_;
                token_.number_kind = consume_number_suffix();
                next_char();
                suffix_size = pos_ - before_suffix_pos;
                if (last_is_underscore) suffix_size++;
                stop = true;
                break;
            }
            default:
                if (last_is_underscore) raise_at_token("trailing '_' in number", token_, pos_ - start);
                stop = true;
        }
        if (stop) break;
        next_char();
    }

    set_token_raw_from_start(start);

    const int pos_before_suffix = pos_ - suffix_size;
    std::string raw_number_string = string_range(pos_after_prefix, pos_before_suffix);
    bool has_value = true;
    if (base == 10) {
        if (has_underscores) {
            std::string s;
            for (char c : raw_number_string) {
                if (c != '_') s += c;
            }
            raw_number_string = s;
        }
        token_.set_str(raw_number_string);
    } else {
        std::string base10 = ToBase10(raw_number_string, base);
        if (!base10.empty()) {
            number_size = static_cast<int>(base10.size());
            const char first = src_[static_cast<size_t>(start)];
            if (first == '+' || first == '-') base10 = std::string(1, first) + base10;
            raw_number_string = base10;
            token_.set_str(base10);
        } else {
            // `@token.value` stays as it was: nil after reset_token.
            has_value = false;
        }
    }

    if (is_decimal) {
        if (suffix_size == 0) token_.number_kind = NumberKind::F64;
        if (!IsFloat(token_.number_kind)) {
            raise_at_token(std::string("Invalid suffix ") + NumberKindName(token_.number_kind) + " for decimal number",
                           token_, pos_ - start);
        }
        return;
    }

    if (suffix_size == 0) {
        const char *int64 = "Int64";
        const char *uint64 = "UInt64";
        if (!has_value) raise_value_doesnt_fit_in(negative ? int64 : uint64, start, pos_before_suffix);
        if (number_size <= 9) {
            token_.number_kind = NumberKind::I32;
        } else if (number_size == 10) {
            token_.number_kind = DecimalFits(raw_number_string, false, 32) ? NumberKind::I32 : NumberKind::I64;
        } else if (number_size <= 18) {
            token_.number_kind = NumberKind::I64;
        } else if (number_size == 19) {
            if (DecimalFits(raw_number_string, false, 64)) {
                token_.number_kind = NumberKind::I64;
            } else if (negative) {
                raise_value_doesnt_fit_in(int64, start, pos_before_suffix, "i128");
            } else {
                warnings.push_back(std::to_string(token_.line_number) + ":" + std::to_string(token_.column_number) +
                                   ": " + string_range(start, pos_before_suffix) +
                                   " doesn't fit in an Int64, try using the suffix u64 or i128");
                token_.number_kind = NumberKind::U64;
            }
        } else if (number_size == 20) {
            if (negative) raise_value_doesnt_fit_in(int64, start, pos_before_suffix, "i128");
            if (DecimalFits(raw_number_string, true, 64)) {
                warnings.push_back(std::to_string(token_.line_number) + ":" + std::to_string(token_.column_number) +
                                   ": " + string_range(start, pos_before_suffix) +
                                   " doesn't fit in an Int64, try using the suffix u64 or i128");
                token_.number_kind = NumberKind::U64;
            } else {
                raise_value_doesnt_fit_in(uint64, start, pos_before_suffix, "i128");
            }
        } else if (number_size <= 38) {
            raise_value_doesnt_fit_in(negative ? int64 : uint64, start, pos_before_suffix, "i128");
        } else if (number_size == 39) {
            if (DecimalFits(raw_number_string, false, 128)) {
                raise_value_doesnt_fit_in(negative ? int64 : uint64, start, pos_before_suffix, "i128");
            } else if (DecimalFits(raw_number_string, true, 128)) {
                raise_value_doesnt_fit_in(uint64, start, pos_before_suffix, "u128");
            } else {
                raise_value_doesnt_fit_in(negative ? int64 : uint64, start, pos_before_suffix);
            }
        } else {
            raise_value_doesnt_fit_in(negative ? int64 : uint64, start, pos_before_suffix);
        }
    } else {
        switch (token_.number_kind) {
            case NumberKind::I8: check_int_fits("Int8", false, 3, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::U8: check_int_fits("UInt8", true, 3, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::I16: check_int_fits("Int16", false, 5, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::U16: check_int_fits("UInt16", true, 5, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::I32: check_int_fits("Int32", false, 10, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::U32: check_int_fits("UInt32", true, 10, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::I64: check_int_fits("Int64", false, 19, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::U64: check_int_fits("UInt64", true, 20, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::I128: check_int_fits("Int128", false, 39, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            case NumberKind::U128: check_int_fits("UInt128", true, 39, number_size, raw_number_string, start, pos_before_suffix, negative, has_value); break;
            default: break;
        }
    }
}

NumberKind Lexer::consume_number_suffix() {
    switch (cur_) {
        case 'i':
            switch (next_char()) {
                case '8': return NumberKind::I8;
                case '1':
                    switch (next_char()) {
                        case '2':
                            if (next_char() == '8') return NumberKind::I128;
                            break;
                        case '6': return NumberKind::I16;
                        default: break;
                    }
                    break;
                case '3':
                    if (next_char() == '2') return NumberKind::I32;
                    break;
                case '6':
                    if (next_char() == '4') return NumberKind::I64;
                    break;
                default: break;
            }
            raise("invalid int suffix");
        case 'u':
            switch (next_char()) {
                case '8': return NumberKind::U8;
                case '1':
                    switch (next_char()) {
                        case '2':
                            if (next_char() == '8') return NumberKind::U128;
                            break;
                        case '6': return NumberKind::U16;
                        default: break;
                    }
                    break;
                case '3':
                    if (next_char() == '2') return NumberKind::U32;
                    break;
                case '6':
                    if (next_char() == '4') return NumberKind::U64;
                    break;
                default: break;
            }
            raise("invalid uint suffix");
        case 'f':
            switch (next_char()) {
                case '3':
                    if (next_char() == '2') return NumberKind::F32;
                    break;
                case '6':
                    if (next_char() == '4') return NumberKind::F64;
                    break;
                default: break;
            }
            raise("invalid float suffix");
        default: break;
    }
    raise("BUG: invalid suffix");
}

// --- strings ------------------------------------------------------------------------

Token &Lexer::next_string_token(const DelimiterState &delimiter_state_in) {
    const DelimiterState delimiter_state = delimiter_state_in;  // may alias token_.delimiter_state
    const bool was_newline = token_.type == Tok::NEWLINE;
    reset_token();

    token_.line_number = line_number;
    token_.delimiter_state = delimiter_state;

    const int start = pos_;
    const int string_open_count = delimiter_state.open_count;

    // For empty heredocs:
    if (was_newline && delimiter_state.is_heredoc()) {
        if (check_heredoc_end(delimiter_state)) {
            set_token_raw_from_start(start);
            return token_;
        }
    }

    if (cur_ == 0) {
        raise_unterminated_quoted(delimiter_state);
    } else if (delimiter_state.end_is(cur_)) {
        next_char();
        if (string_open_count == 0) {
            token_.type = Tok::DELIMITER_END;
        } else {
            token_.type = Tok::STRING;
            token_.set_str(Utf8Encode(delimiter_state.end));
            token_.delimiter_state = delimiter_state.with_open_count_delta(-1);
        }
    } else if (delimiter_state.nest_is(cur_)) {
        next_char_tok(Tok::STRING);
        token_.set_str(Utf8Encode(delimiter_state.nest));
        token_.delimiter_state = delimiter_state.with_open_count_delta(+1);
    } else if (cur_ == '\\') {
        if (delimiter_state.allow_escapes) {
            if (delimiter_state.kind == DelimiterKind::REGEX) {
                const char32_t c = next_char();
                if (c == 0) raise_unterminated_quoted(delimiter_state);
                next_char_tok(Tok::STRING);
                if (c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') {
                    token_.set_str(Utf8Encode(c));
                } else {
                    token_.set_str("\\" + Utf8Encode(c));
                }
            } else {
                const char32_t c = next_char();
                if (c == '\\') {
                    string_token_escape_value("\\");
                } else if (delimiter_state.end_is(c)) {
                    string_token_escape_value(Utf8Encode(delimiter_state.end));
                } else if (delimiter_state.nest_is(c)) {
                    string_token_escape_value(Utf8Encode(delimiter_state.nest));
                } else {
                    switch (c) {
                        case '#': string_token_escape_value("#"); break;
                        case 'a': string_token_escape_value("\a"); break;
                        case 'b': string_token_escape_value("\b"); break;
                        case 'n': string_token_escape_value("\n"); break;
                        case 'r': string_token_escape_value("\r"); break;
                        case 't': string_token_escape_value("\t"); break;
                        case 'v': string_token_escape_value("\v"); break;
                        case 'f': string_token_escape_value("\f"); break;
                        case 'e': string_token_escape_value("\x1B"); break;
                        case 'x': {
                            const uint8_t v = consume_string_hex_escape();
                            next_char_tok(Tok::STRING);
                            token_.set_str(std::string(1, static_cast<char>(v)));
                            break;
                        }
                        case 'u': {
                            std::string v = consume_string_unicode_escape();
                            next_char_tok(Tok::STRING);
                            token_.set_str(std::move(v));
                            break;
                        }
                        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                            const uint8_t v = consume_octal_escape(c);
                            next_char_tok(Tok::STRING);
                            token_.set_str(std::string(1, static_cast<char>(v)));
                            break;
                        }
                        case '\r':
                        case '\n': {
                            handle_slash_r_slash_n_or_slash_n();
                            incr_line_number();
                            token_.line_number = line_number;
                            while (true) {
                                const char32_t ch = next_char();
                                if (ch == 0) {
                                    raise_unterminated_quoted(delimiter_state);
                                } else if (ch == '\n') {
                                    incr_line_number();
                                    token_.line_number = line_number;
                                } else if (ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r') {
                                    // continue
                                } else {
                                    break;
                                }
                            }
                            next_string_token(delimiter_state);
                            // The raw spans the whole `\<newline>...`, from this call's start.
                            set_token_raw_from_start(start);
                            return token_;
                        }
                        case 0: raise_unterminated_quoted(delimiter_state);
                        default:
                            token_.type = Tok::STRING;
                            token_.set_str(Utf8Encode(cur_));
                            token_.invalid_escape = true;
                            next_char();
                    }
                }
            }
        } else {
            token_.type = Tok::STRING;
            token_.set_str(Utf8Encode(cur_));
            next_char();
        }
    } else if (cur_ == '#') {
        if (delimiter_state.allow_escapes && peek_next_char() == '{') {
            next_char();
            next_char_tok(Tok::INTERPOLATION_START);
        } else {
            next_char_tok(Tok::STRING);
            token_.set_str("#");
        }
    } else if (cur_ == '\r' || cur_ == '\n') {
        const bool is_slash_r = handle_slash_r_slash_n_or_slash_n();
        next_char();
        incr_line_number(1);
        token_.line_number = line_number;
        token_.column_number = column_number;
        if (delimiter_state.is_heredoc()) {
            if (!check_heredoc_end(delimiter_state)) {
                next_string_token_noescape(delimiter_state);
                token_.set_str(string_range(start));
            }
        } else {
            token_.type = Tok::STRING;
            token_.set_str(is_slash_r ? "\r\n" : "\n");
        }
    } else {
        next_string_token_noescape(delimiter_state);
        token_.set_str(string_range(start));
    }

    set_token_raw_from_start(start);
    return token_;
}

void Lexer::next_string_token_noescape(const DelimiterState &ds) {
    while (!(ds.end_is(cur_) || ds.nest_is(cur_) || cur_ == 0 || cur_ == '\\' || cur_ == '#' || cur_ == '\r' ||
             cur_ == '\n')) {
        next_char();
    }
    token_.type = Tok::STRING;
}

bool Lexer::check_heredoc_end(const DelimiterState &ds) {
    const std::string string_end = ds.end_text();
    const int old_pos = pos_;
    const int old_column = column_number;

    while (cur_ == ' ' || cur_ == '\t') next_char();
    const int indent = column_number - 1;

    if (!string_end.empty() && Utf8Encode(cur_) == string_end.substr(0, Utf8Encode(cur_).size()) && cur_ != 0) {
        bool reached_end = false;
        size_t i = 0;
        while (i < string_end.size()) {
            char32_t c;
            bool bad;
            const int w = DecodeUtf8(string_end, i, &c, &bad);
            if (c != cur_) {
                reached_end = false;
                break;
            }
            next_char();
            reached_end = true;
            i += static_cast<size_t>(w);
        }
        if (reached_end && (cur_ == '\n' || cur_ == 0 || (cur_ == '\r' && peek_next_char() == '\n' && next_char()))) {
            token_.type = Tok::DELIMITER_END;
            token_.delimiter_state = ds.with_heredoc_indent(indent);
            return true;
        }
    }

    set_current_pos(old_pos);
    column_number = old_column;
    token_.column_number = column_number;
    return false;
}

void Lexer::raise_unterminated_quoted(const DelimiterState &ds) {
    std::string msg;
    switch (ds.kind) {
        case DelimiterKind::COMMAND: msg = "Unterminated command literal"; break;
        case DelimiterKind::REGEX: msg = "Unterminated regular expression"; break;
        case DelimiterKind::HEREDOC:
            msg = "Unterminated heredoc: can't find \"" + ds.heredoc + "\" anywhere before the end of file";
            break;
        case DelimiterKind::STRING: msg = "Unterminated string literal"; break;
        default: msg = "unreachable";
    }
    raise(msg, line_number, column_number);
}

// --- macros -------------------------------------------------------------------------

Token &Lexer::next_macro_token(const MacroState &macro_state_in, bool skip_whitespace) {
    const MacroState macro_state = macro_state_in;  // may alias token_.macro_state
    reset_token();

    int nest = macro_state.nest;
    const int control_nest = macro_state.control_nest;
    bool whitespace = macro_state.whitespace;
    bool has_ds = macro_state.has_delimiter_state;
    DelimiterState delimiter_state = macro_state.delimiter_state;
    bool beginning_of_line = macro_state.beginning_of_line;
    bool comment = macro_state.comment;
    bool has_heredocs = macro_state.has_heredocs;
    std::vector<DelimiterState> heredocs_ = macro_state.heredocs;
    bool yields = false;

    auto make_state = [&]() {
        MacroState s;
        s.whitespace = whitespace;
        s.nest = nest;
        s.control_nest = control_nest;
        s.has_delimiter_state = has_ds;
        s.delimiter_state = delimiter_state;
        s.beginning_of_line = beginning_of_line;
        s.yields = yields;
        s.comment = comment;
        s.has_heredocs = has_heredocs;
        s.heredocs = heredocs_;
        return s;
    };

    if (skip_whitespace) skip_macro_whitespace();

    token_.has_location = false;
    token_.line_number = line_number;
    token_.column_number = column_number;

    int start = pos_;

    if (cur_ == 0) {
        token_.type = Tok::EOF_;
        return token_;
    }

    if (cur_ == '\\' && peek_next_char() == '{') {
        beginning_of_line = false;
        next_char();
        start = pos_;
        if (next_char() == '%') {
            char32_t c;
            while (true) {
                c = next_char_check_line();
                if (!(c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r')) break;
            }
            switch (c) {
                case 'e':
                    if (char_sequence({'n', 'd'}) && !ident_part_or_end(peek_next_char())) {
                        next_char();
                        nest--;
                    }
                    break;
                case 'f':
                    if (char_sequence({'o', 'r'}) && !ident_part_or_end(peek_next_char())) {
                        next_char();
                        nest++;
                    }
                    break;
                case 'i':
                    if (next_char() == 'f' && !ident_part_or_end(peek_next_char())) {
                        next_char();
                        nest++;
                    }
                    break;
                case 'u':
                    if (char_sequence({'n', 'l', 'e', 's', 's'}) && !ident_part_or_end(peek_next_char())) {
                        next_char();
                        nest++;
                    }
                    break;
                default: break;
            }
        }
        token_.type = Tok::MACRO_LITERAL;
        token_.set_str(string_range(start));
        token_.macro_state = make_state();
        set_token_raw_from_start(start);
        return token_;
    }

    if (cur_ == '\\' && peek_next_char() == '%') {
        beginning_of_line = false;
        next_char();
        next_char_tok(Tok::MACRO_LITERAL);
        token_.set_str("%");
        token_.macro_state = make_state();
        token_.raw = "%";
        return token_;
    }

    if (cur_ == '{') {
        const char32_t c = next_char();
        if (c == '{') {
            beginning_of_line = false;
            next_char_tok(Tok::MACRO_EXPRESSION_START);
            token_.macro_state = make_state();
            return token_;
        } else if (c == '%') {
            beginning_of_line = false;
            next_char_tok(Tok::MACRO_CONTROL_START);
            token_.macro_state = make_state();
            return token_;
        } else {
            if (macro_curly_count_ > 0) macro_curly_count_++;
        }
    }

    if (comment || (!has_ds && cur_ == '#')) {
        comment = true;
        char32_t c = cur_;
        if (cur_ == '#') c = next_char();
        while (true) {
            if (c == '\n') {
                comment = false;
                beginning_of_line = true;
                whitespace = true;
                next_char();
                incr_line_number();
                token_.line_number = line_number;
                token_.column_number = column_number;
                break;
            } else if (c == '{') {
                break;
            } else if (c == '\\') {
                if (peek_next_char() == '{') break;
                c = next_char();
            } else if (c == 0) {
                raise("unterminated macro");
            } else {
                c = next_char();
            }
        }
        token_.type = Tok::MACRO_LITERAL;
        token_.set_str(string_range(start));
        token_.macro_state = make_state();
        set_token_raw_from_start(start);
        return token_;
    }

    if (!has_ds && cur_ == '%' && ident_start(peek_next_char())) {
        char32_t c = next_char();
        auto is_open = [](char32_t ch) { return ch == '(' || ch == '<' || ch == '[' || ch == '{' || ch == '|'; };
        DelimiterKind kind = DelimiterKind::STRING;
        bool literal = true;
        if (c == 'q' && is_open(peek_next_char())) {
            kind = DelimiterKind::STRING;
        } else if (c == 'Q' && is_open(peek_next_char())) {
            kind = DelimiterKind::STRING;
        } else if (c == 'i' && is_open(peek_next_char())) {
            kind = DelimiterKind::SYMBOL_ARRAY;
        } else if (c == 'w' && is_open(peek_next_char())) {
            kind = DelimiterKind::STRING_ARRAY;
        } else if (c == 'x' && is_open(peek_next_char())) {
            kind = DelimiterKind::COMMAND;
        } else if (c == 'r' && is_open(peek_next_char())) {
            kind = DelimiterKind::REGEX;
        } else {
            literal = false;
        }
        if (literal) {
            next_char();
            delimiter_state = DelimiterState::MakeCount(kind, cur_, closing_char(), 1);
            has_ds = true;
            next_char();
        } else {
            start = pos_;
            while (ident_part(c)) c = next_char();
            beginning_of_line = false;
            token_.type = Tok::MACRO_VAR;
            token_.set_str(string_range(start));
            token_.macro_state = make_state();
            return token_;
        }
    }

    if (!has_ds && cur_ == 'e') {
        if (next_char() == 'n') {
            beginning_of_line = false;
            const char32_t c = next_char();
            if (c == 'd') {
                if (whitespace && !ident_part_or_end(peek_next_char()) && peek_next_char() != ':') {
                    if (nest == 0 && control_nest == 0) {
                        next_char_tok(Tok::MACRO_END);
                        token_.macro_state = MacroState();
                        return token_;
                    } else {
                        nest--;
                        whitespace = cur_ == ' ' || cur_ == '\t' || cur_ == '\n' || cur_ == '\v' || cur_ == '\f' ||
                                     cur_ == '\r';
                        next_char();
                    }
                }
            } else if (c == 'u') {
                if (!has_ds && whitespace && next_char() == 'm' && !ident_part_or_end(next_char())) {
                    nest++;
                    whitespace = true;
                }
            }
        } else {
            whitespace = false;
            beginning_of_line = false;
        }
    }

    char32_t c = cur_;
    auto is_ws = [](char32_t ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r'; };

    while (!(c == '{' || c == 0 || (c == '\\' && (peek_next_char() == '{' || peek_next_char() == '%')) ||
             (whitespace && !has_ds && c == 'e'))) {
        bool do_next = true;  // `char = next_char` at the bottom of the loop
        switch (c) {
            case '\n':
                incr_line_number(0);
                whitespace = true;
                beginning_of_line = true;
                c = next_char();
                if (!has_ds && has_heredocs && !heredocs_.empty()) {
                    delimiter_state = heredocs_.front();
                    heredocs_.erase(heredocs_.begin());
                    has_ds = true;
                }
                if (has_ds && delimiter_state.is_heredoc() && check_heredoc_end(delimiter_state)) {
                    c = cur_;
                    if (has_heredocs && !heredocs_.empty()) {
                        delimiter_state = heredocs_.front();
                        heredocs_.erase(heredocs_.begin());
                        has_ds = true;
                    } else {
                        has_ds = false;
                    }
                }
                do_next = false;
                break;
            case '\\':
                c = next_char();
                if (has_ds) {
                    if (delimiter_state.end_is(c)) {
                        c = next_char();
                    } else if (c == '\\') {
                        c = next_char();
                    }
                    whitespace = false;
                } else {
                    whitespace = false;
                }
                do_next = false;
                break;
            case '\'':
            case '"':
                if (has_ds) {
                    if (delimiter_state.end_is(c)) has_ds = false;
                } else {
                    delimiter_state = DelimiterState::Make(DelimiterKind::STRING, c, c);
                    has_ds = true;
                }
                whitespace = false;
                break;
            case '%': {
                const char32_t p = peek_next_char();
                if (p == '(' || p == '[' || p == '<' || p == '{' || p == '|') {
                    next_char();
                    delimiter_state = DelimiterState::MakeCount(DelimiterKind::STRING, p, closing_char(), 1);
                    has_ds = true;
                } else {
                    whitespace = false;
                    if (!has_ds && ident_start(p)) goto done;
                }
                break;
            }
            case '#':
                if (has_ds) {
                    if (peek_next_char() == '{') {
                        c = next_char();
                        macro_curly_count_++;
                        delimiter_state_stack_.push_back(delimiter_state);
                        has_ds = false;
                    }
                    whitespace = false;
                } else {
                    goto done;
                }
                break;
            case '}':
                if (has_ds && delimiter_state.end_is('}')) {
                    delimiter_state = delimiter_state.with_open_count_delta(-1);
                    if (delimiter_state.open_count == 0) has_ds = false;
                } else if (macro_curly_count_ > 0) {
                    if (macro_curly_count_ == 1) {
                        delimiter_state = delimiter_state_stack_.back();
                        delimiter_state_stack_.pop_back();
                        has_ds = true;
                    }
                    macro_curly_count_--;
                }
                break;
            case '<': {
                DelimiterState hd;
                if (!has_ds && delimiter_state_stack_.empty() && lookahead([&] { return check_heredoc_start(&hd); })) {
                    has_heredocs = true;
                    heredocs_.push_back(hd);
                    c = cur_;
                    do_next = false;
                }
                break;
            }
            default: {
                if (!has_ds && whitespace &&
                    lookahead([&] {
                        return c == 'y' && char_sequence({'i', 'e', 'l', 'd'}) && !ident_part_or_end(peek_next_char());
                    })) {
                    yields = true;
                    c = cur_;
                    whitespace = true;
                    beginning_of_line = false;
                } else {
                    MacroKeywordState keyword = MacroKeywordState::None;
                    if (!has_ds && whitespace) {
                        lookahead([&] {
                            keyword = macro_starts_with_keyword(beginning_of_line);
                            return keyword != MacroKeywordState::None;
                        });
                    }
                    if (keyword != MacroKeywordState::None) {
                        c = cur_;
                        if (keyword != MacroKeywordState::AbstractDef) nest++;
                        whitespace = true;
                        beginning_of_line = false;
                        do_next = false;
                    } else {
                        c = cur_;
                        if (has_ds) {
                            if (delimiter_state.nest_is(c)) {
                                delimiter_state = delimiter_state.with_open_count_delta(+1);
                            } else if (delimiter_state.end_is(c)) {
                                delimiter_state = delimiter_state.with_open_count_delta(-1);
                                if (delimiter_state.open_count == 0) has_ds = false;
                            }
                        }
                        if (c == '=' && is_ws(peek_next_char())) {
                            whitespace = false;
                            beginning_of_line = true;
                        } else {
                            whitespace = is_ws(c) || c == ';' || c == '(' || c == '[' || c == '{';
                            if (beginning_of_line && !whitespace) beginning_of_line = false;
                        }
                    }
                }
            }
        }
        if (do_next) c = next_char();
    }
done:

    token_.type = Tok::MACRO_LITERAL;
    token_.set_str(string_range(start));
    token_.macro_state = make_state();
    set_token_raw_from_start(start);
    return token_;
}

std::string Lexer::skip_macro_whitespace() {
    const int start = pos_;
    while (cur_ == ' ' || cur_ == '\t' || cur_ == '\n' || cur_ == '\v' || cur_ == '\f' || cur_ == '\r') {
        if (cur_ == '\n') incr_line_number(0);
        next_char();
    }
    return wants_raw ? string_range(start) : std::string();
}

bool Lexer::peek_not_ident_part_or_end_next_char() {
    if (ident_part_or_end(peek_next_char()) || peek_next_char() == ':') return false;
    next_char();
    return true;
}

Lexer::MacroKeywordState Lexer::macro_starts_with_keyword(bool beginning_of_line) {
    using M = MacroKeywordState;
    auto is_ws = [](char32_t ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r'; };
    switch (cur_) {
        case 'a':
            switch (next_char()) {
                case 'b':
                    if (char_sequence({'s', 't', 'r', 'a', 'c', 't'}) && is_ws(next_char())) {
                        switch (next_char()) {
                            case 'd':
                                if (char_sequence({'e', 'f'}) && peek_not_ident_part_or_end_next_char()) return M::AbstractDef;
                                return M::None;
                            case 'c':
                                if (char_sequence({'l', 'a', 's', 's'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                                return M::None;
                            case 's':
                                if (char_sequence({'t', 'r', 'u', 'c', 't'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                                return M::None;
                            default: return M::None;
                        }
                    }
                    return M::None;
                case 'n':
                    if (char_sequence({'n', 'o', 't', 'a', 't', 'i', 'o', 'n'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                default: return M::None;
            }
        case 'b':
            if (char_sequence({'e', 'g', 'i', 'n'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
            return M::None;
        case 'c':
            switch (next_char()) {
                case 'a':
                    if (char_sequence({'s', 'e'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                case 'l':
                    if (char_sequence({'a', 's', 's'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                default: return M::None;
            }
        case 'd':
            switch (next_char()) {
                case 'o':
                    if (peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                case 'e':
                    if (next_char() == 'f' && peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                default: return M::None;
            }
        case 'f':
            if (char_sequence({'u', 'n'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
            return M::None;
        case 'i':
            if (beginning_of_line && next_char() == 'f' && peek_not_ident_part_or_end_next_char()) return M::Other;
            return M::None;
        case 'l':
            if (char_sequence({'i', 'b'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
            return M::None;
        case 'm':
            switch (next_char()) {
                case 'a':
                    if (char_sequence({'c', 'r', 'o'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                case 'o':
                    if (char_sequence({'d', 'u', 'l', 'e'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                    return M::None;
                default: return M::None;
            }
        case 's':
            switch (next_char()) {
                case 'e':
                    if (char_sequence({'l', 'e', 'c', 't'}) && !ident_part_or_end(peek_next_char()) && next_char()) return M::Other;
                    return M::None;
                case 't':
                    if (char_sequence({'r', 'u', 'c', 't'}) && !ident_part_or_end(peek_next_char()) && next_char()) return M::Other;
                    return M::None;
                default: return M::None;
            }
        case 'u':
            if (next_char() == 'n') {
                switch (next_char()) {
                    case 'i':
                        if (char_sequence({'o', 'n'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                        return M::None;
                    case 'l':
                        if (beginning_of_line && char_sequence({'e', 's', 's'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                        return M::None;
                    case 't':
                        if (beginning_of_line && char_sequence({'i', 'l'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
                        return M::None;
                    default: return M::None;
                }
            }
            return M::None;
        case 'w':
            if (beginning_of_line && char_sequence({'h', 'i', 'l', 'e'}) && peek_not_ident_part_or_end_next_char()) return M::Other;
            return M::None;
        default: return M::None;
    }
}

bool Lexer::check_heredoc_start(DelimiterState *out) {
    if (!(cur_ == '<' && char_sequence({'<', '-'}))) return false;
    bool has_single_quote = false;
    bool found_closing_single_quote = false;
    if (next_char() == '\'') {
        has_single_quote = true;
        next_char();
    }
    if (!ident_part(cur_)) return false;
    const int start_here = pos_;
    int end_here = 0;
    while (true) {
        const char32_t c = next_char();
        if (c == '\r') {
            if (peek_next_char() == '\n') {
                end_here = pos_;
                next_char();
                break;
            }
            return false;
        } else if (c == '\n') {
            end_here = pos_;
            break;
        } else if (ident_part(c)) {
            // ok
        } else if (c == 0) {
            return false;
        } else if (has_single_quote) {
            if (c == '\'') {
                found_closing_single_quote = true;
                end_here = pos_;
                next_char();
                break;
            }
        } else {
            end_here = pos_;
            break;
        }
    }
    if (has_single_quote && !found_closing_single_quote) return false;
    *out = DelimiterState::Heredoc(string_range(start_here, end_here), !has_single_quote);
    return true;
}

// --- escapes ------------------------------------------------------------------------

uint8_t Lexer::consume_octal_escape(char32_t c) {
    int value = static_cast<int>(c - '0');
    int count = 1;
    while (count <= 3 && peek_next_char() >= '0' && peek_next_char() < '8') {
        next_char();
        value = value * 8 + static_cast<int>(cur_ - '0');
        count++;
    }
    if (value >= 256) raise("octal value too big");
    return static_cast<uint8_t>(value);
}

char32_t Lexer::consume_char_unicode_escape() {
    if (peek_next_char() == '{') {
        next_char();
        return consume_braced_unicode_escape();
    }
    return consume_non_braced_unicode_escape();
}

namespace {
int HexValue(char32_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A') + 10;
    return -1;
}
}  // namespace

uint8_t Lexer::consume_string_hex_escape() {
    const int high = HexValue(next_char());
    if (high < 0) raise("invalid hex escape");
    const int low = HexValue(next_char());
    if (low < 0) raise("invalid hex escape");
    return static_cast<uint8_t>((high << 4) | low);
}

std::string Lexer::consume_string_unicode_escape() {
    if (peek_next_char() == '{') {
        next_char();
        return consume_string_unicode_brace_escape();
    }
    return Utf8Encode(consume_non_braced_unicode_escape());
}

std::string Lexer::consume_string_unicode_brace_escape() {
    std::string out;
    while (true) {
        out += Utf8Encode(consume_braced_unicode_escape(true));
        if (cur_ != ' ') break;
    }
    return out;
}

char32_t Lexer::consume_non_braced_unicode_escape() {
    uint32_t codepoint = 0;
    for (int i = 0; i < 4; i++) {
        const int h = HexValue(next_char());
        if (h < 0) expected_hexadecimal_character_in_unicode_escape();
        codepoint = 16 * codepoint + static_cast<uint32_t>(h);
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) raise("invalid unicode codepoint (surrogate half)");
    return codepoint;
}

char32_t Lexer::consume_braced_unicode_escape(bool allow_spaces) {
    uint32_t codepoint = 0;
    bool found_curly = false, found_space = false, found_digit = false;
    char32_t c = 0;
    for (int i = 0; i < 6; i++) {
        c = next_char();
        if (c == '}') {
            found_curly = true;
            break;
        } else if (c == ' ') {
            if (allow_spaces) {
                found_space = true;
                break;
            }
            expected_hexadecimal_character_in_unicode_escape();
        } else {
            const int h = HexValue(c);
            if (h < 0) expected_hexadecimal_character_in_unicode_escape();
            codepoint = 16 * codepoint + static_cast<uint32_t>(h);
            found_digit = true;
        }
    }
    if (!found_digit) {
        expected_hexadecimal_character_in_unicode_escape();
    } else if (codepoint > 0x10FFFF) {
        raise("invalid unicode codepoint (too large)");
    } else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        raise("invalid unicode codepoint (surrogate half)");
    }
    if (!found_space) {
        if (!found_curly) c = next_char();
        if (c != '}') raise("expected '}' to close unicode escape");
    }
    return codepoint;
}

void Lexer::expected_hexadecimal_character_in_unicode_escape() {
    raise("expected hexadecimal character in unicode escape");
}

void Lexer::string_token_escape_value(const std::string &value) {
    next_char_tok(Tok::STRING);
    token_.set_str(value);
}

void Lexer::delimited_pair(DelimiterKind kind, char32_t nest, char32_t end, int start, bool allow_escapes,
                           bool advance) {
    if (advance) next_char();
    token_.type = Tok::DELIMITER_START;
    token_.delimiter_state = DelimiterState::Make(kind, nest, end, allow_escapes);
    set_token_raw_from_start(start);
}

Token &Lexer::next_string_array_token() {
    auto is_ws = [](char32_t ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r'; };
    while (true) {
        if (cur_ == '\n') {
            next_char();
            incr_line_number(1);
        } else if (is_ws(cur_)) {
            next_char();
        } else {
            break;
        }
    }

    reset_token();

    if (token_.delimiter_state.end_is(cur_)) {
        if (wants_raw) token_.raw = Utf8Encode(cur_);
        next_char_tok(Tok::STRING_ARRAY_END);
        return token_;
    }

    const int start = pos_;
    int sub_start = start;
    std::string value;
    bool escaped = false;
    while (true) {
        if (cur_ == 0) {
            break;  // raise is handled by parser
        } else if (token_.delimiter_state.end_is(cur_)) {
            if (!escaped) {
                if (token_.delimiter_state.open_count == 0) break;
                token_.delimiter_state = token_.delimiter_state.with_open_count_delta(-1);
            }
        } else if (token_.delimiter_state.nest_is(cur_)) {
            if (!escaped) token_.delimiter_state = token_.delimiter_state.with_open_count_delta(+1);
        } else if (is_ws(cur_)) {
            if (!escaped) break;
        } else {
            if (escaped) value += '\\';
        }
        escaped = cur_ == '\\';
        if (escaped) {
            value += string_range(sub_start, pos_);
            sub_start = pos_ + 1;
        }
        next_char();
    }

    if (start == pos_) {
        token_.type = Tok::EOF_;
        return token_;
    }

    value += string_range(sub_start, pos_);
    token_.type = Tok::STRING;
    token_.set_str(std::move(value));
    set_token_raw_from_start(start);
    return token_;
}

void Lexer::consume_loc_pragma() {
    // `#<loc:"file",line,column>`, `#<loc:push>`, `#<loc:pop>`: only
    // produced by macro expansions. Lexed for fidelity; locations follow.
    if (cur_ == '"') {
        next_char_no_column_increment();
        const int filename_pos = pos_;
        while (true) {
            if (cur_ == '"') break;
            if (cur_ == 0) raise("unexpected end of file in loc pragma");
            next_char_no_column_increment();
        }
        incr_column_number((pos_ - filename_pos) + 7);
        next_char();
        if (cur_ != ',') raise("expected ',' in loc pragma after filename");
        next_char();
        int line = 0;
        while (true) {
            if (cur_ >= '0' && cur_ <= '9') {
                line = 10 * line + static_cast<int>(cur_ - '0');
            } else if (cur_ == ',') {
                next_char();
                break;
            } else {
                raise("expected digit or ',' in loc pragma for line number");
            }
            next_char();
        }
        int column = 0;
        while (true) {
            if (cur_ >= '0' && cur_ <= '9') {
                column = 10 * column + static_cast<int>(cur_ - '0');
            } else if (cur_ == '>') {
                next_char();
                break;
            } else {
                raise("expected digit or '>' in loc pragma for column_number number");
            }
            next_char();
        }
        token_.line_number = line_number = line;
        token_.column_number = column_number = column;
    } else if (cur_ == 'p') {
        next_char_no_column_increment();
        if (cur_ == 'o') {
            if (!char_sequence({'p', '>'}, false)) raise("expected #<loc:push>, #<loc:pop> or #<loc:\"...\">");
            next_char_no_column_increment();
            incr_column_number(10);
            if (stacked_) {
                stacked_ = false;
                token_.line_number = line_number = stacked_line_number_;
                token_.column_number = column_number = stacked_column_number_;
            }
        } else if (cur_ == 'u') {
            if (!char_sequence({'s', 'h', '>'}, false)) raise("expected #<loc:push>, #<loc:pop> or #<loc:\"...\">");
            next_char_no_column_increment();
            incr_column_number(11);
            token_.line_number = line_number;
            token_.column_number = column_number;
            if (!stacked_) {
                stacked_ = true;
                stacked_line_number_ = line_number;
                stacked_column_number_ = column_number;
            }
        } else {
            raise("expected #<loc:push>, #<loc:pop> or #<loc:\"...\">");
        }
    } else {
        raise("expected #<loc:push>, #<loc:pop> or #<loc:\"...\">");
    }
}

void Lexer::consume_heredoc_start(int start) {
    bool has_single_quote = false;
    bool found_closing_single_quote = false;
    if (next_char() == '\'') {
        has_single_quote = true;
        next_char();
    }
    if (!ident_part(cur_)) raise("heredoc identifier starts with invalid character");
    const int start_here = pos_;
    int end_here = 0;
    while (true) {
        char32_t c = next_char();
        if (c == '\r') {
            if (peek_next_char() == '\n') {
                end_here = pos_;
                next_char();
                break;
            }
            raise("expecting '\\n' after '\\r'");
        } else if (c == '\n') {
            end_here = pos_;
            break;
        } else if (ident_part(c)) {
            // ok
        } else if (c == 0) {
            raise("Unexpected EOF on heredoc identifier");
        } else if (has_single_quote) {
            if (c == '\'') {
                found_closing_single_quote = true;
                end_here = pos_;
                c = next_char();
                if (c == 0) raise("Unexpected EOF on heredoc identifier");
                break;
            }
        } else {
            end_here = pos_;
            break;
        }
    }
    if (has_single_quote && !found_closing_single_quote) raise("expecting closing single quote");
    const std::string here = string_range(start_here, end_here);
    token_.type = Tok::DELIMITER_START;
    token_.delimiter_state = DelimiterState::Heredoc(here, !has_single_quote);
    set_token_raw_from_start(start);
}

void Lexer::consume_symbol() {
    const char32_t c = cur_;
    switch (c) {
        case ':': next_char_tok(Tok::OP_COLON_COLON); return;
        case '+': next_char_and_symbol("+"); return;
        case '-': next_char_and_symbol("-"); return;
        case '*':
            if (next_char() == '*') {
                next_char_and_symbol("**");
            } else {
                symbol("*");
            }
            return;
        case '/':
            if (next_char() == '/') {
                next_char_and_symbol("//");
            } else {
                symbol("/");
            }
            return;
        case '=':
            switch (next_char()) {
                case '=':
                    if (next_char() == '=') {
                        next_char_and_symbol("===");
                    } else {
                        symbol("==");
                    }
                    return;
                case '~': next_char_and_symbol("=~"); return;
                default: unknown_token();
            }
        case '!':
            switch (next_char()) {
                case '=': next_char_and_symbol("!="); return;
                case '~': next_char_and_symbol("!~"); return;
                default: symbol("!"); return;
            }
        case '<':
            switch (next_char()) {
                case '=':
                    if (next_char() == '>') {
                        next_char_and_symbol("<=>");
                    } else {
                        symbol("<=");
                    }
                    return;
                case '<': next_char_and_symbol("<<"); return;
                default: symbol("<"); return;
            }
        case '>':
            switch (next_char()) {
                case '=': next_char_and_symbol(">="); return;
                case '>': next_char_and_symbol(">>"); return;
                default: symbol(">"); return;
            }
        case '&':
            switch (next_char()) {
                case '+': next_char_and_symbol("&+"); return;
                case '-': next_char_and_symbol("&-"); return;
                case '*':
                    if (next_char() == '*') {
                        next_char_and_symbol("&**");
                    } else {
                        symbol("&*");
                    }
                    return;
                default: symbol("&"); return;
            }
        case '|': next_char_and_symbol("|"); return;
        case '^': next_char_and_symbol("^"); return;
        case '~': next_char_and_symbol("~"); return;
        case '%': next_char_and_symbol("%"); return;
        case '[':
            if (next_char() == ']') {
                switch (next_char()) {
                    case '=': next_char_and_symbol("[]="); return;
                    case '?': next_char_and_symbol("[]?"); return;
                    default: symbol("[]"); return;
                }
            }
            unknown_token();
        case '"': {
            const int line = line_number, column = column_number;
            const int start = pos_ + 1;
            std::string io;
            while (true) {
                char32_t ch = next_char();
                if (ch == '\\') {
                    ch = next_char();
                    switch (ch) {
                        case 'a': io += '\a'; break;
                        case 'b': io += '\b'; break;
                        case 'n': io += '\n'; break;
                        case 'r': io += '\r'; break;
                        case 't': io += '\t'; break;
                        case 'v': io += '\v'; break;
                        case 'f': io += '\f'; break;
                        case 'e': io += '\x1B'; break;
                        case 'x': io += static_cast<char>(consume_string_hex_escape()); break;
                        case 'u': io += consume_string_unicode_escape(); break;
                        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
                            io += static_cast<char>(consume_octal_escape(ch));
                            break;
                        case '\n':
                            incr_line_number(-1);
                            io += '\n';
                            break;
                        case 0: raise("unterminated quoted symbol", line, column);
                        default: io += Utf8Encode(ch);
                    }
                } else if (ch == '"') {
                    break;
                } else if (ch == 0) {
                    raise("unterminated quoted symbol", line, column);
                } else {
                    io += Utf8Encode(ch);
                }
            }
            token_.type = Tok::SYMBOL;
            token_.set_str(io);
            next_char();
            set_token_raw_from_start(start - 2);
            return;
        }
        default:
            if (ident_start(c)) {
                const int start = pos_;
                while (ident_part(next_char())) {
                }
                if (cur_ == '?' || ((cur_ == '!' || cur_ == '=') && peek_next_char() != '=')) next_char();
                token_.type = Tok::SYMBOL;
                token_.set_str(string_range(start));
                set_token_raw_from_start(start - 1);
            } else {
                token_.type = Tok::OP_COLON;
            }
    }
}

void Lexer::consume_variable(Tok type, int start) {
    if (ident_start(cur_)) {
        while (ident_part(next_char())) {
        }
        token_.type = type;
        token_.set_str(string_range(start));
    } else {
        unknown_token();
    }
}

}  // namespace crystal
