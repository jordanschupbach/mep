/**
 * @file zig_tokenizer.cpp
 * @brief Zig's tokenizer (lib/std/zig/tokenizer.zig, Zig 0.16), ported
 *        state for state.
 *
 * Ported from Zig, MIT license, Copyright (c) Zig contributors; the notice is
 * in zig_ast.h. Each `continue :state X` of the Zig source is a `goto X` here;
 * states that advance on entry (`self.index += 1` first) do so at their label,
 * exactly as in the Zig switch, so the two read side by side.
 */
#include "zig_ast.h"


namespace zig {

namespace {

constexpr std::string_view kTokenTagNames[] = {
#define ZIG_X(name) #name,
    ZIG_TOKEN_TAGS(ZIG_X)
#undef ZIG_X
};

struct Keyword {
    std::string_view text;
    TokenTag tag;
};

constexpr Keyword kKeywords[] = {
    {"addrspace", TokenTag::keyword_addrspace},
    {"align", TokenTag::keyword_align},
    {"allowzero", TokenTag::keyword_allowzero},
    {"and", TokenTag::keyword_and},
    {"anyframe", TokenTag::keyword_anyframe},
    {"anytype", TokenTag::keyword_anytype},
    {"asm", TokenTag::keyword_asm},
    {"break", TokenTag::keyword_break},
    {"callconv", TokenTag::keyword_callconv},
    {"catch", TokenTag::keyword_catch},
    {"comptime", TokenTag::keyword_comptime},
    {"const", TokenTag::keyword_const},
    {"continue", TokenTag::keyword_continue},
    {"defer", TokenTag::keyword_defer},
    {"else", TokenTag::keyword_else},
    {"enum", TokenTag::keyword_enum},
    {"errdefer", TokenTag::keyword_errdefer},
    {"error", TokenTag::keyword_error},
    {"export", TokenTag::keyword_export},
    {"extern", TokenTag::keyword_extern},
    {"fn", TokenTag::keyword_fn},
    {"for", TokenTag::keyword_for},
    {"if", TokenTag::keyword_if},
    {"inline", TokenTag::keyword_inline},
    {"noalias", TokenTag::keyword_noalias},
    {"noinline", TokenTag::keyword_noinline},
    {"nosuspend", TokenTag::keyword_nosuspend},
    {"opaque", TokenTag::keyword_opaque},
    {"or", TokenTag::keyword_or},
    {"orelse", TokenTag::keyword_orelse},
    {"packed", TokenTag::keyword_packed},
    {"pub", TokenTag::keyword_pub},
    {"resume", TokenTag::keyword_resume},
    {"return", TokenTag::keyword_return},
    {"linksection", TokenTag::keyword_linksection},
    {"struct", TokenTag::keyword_struct},
    {"suspend", TokenTag::keyword_suspend},
    {"switch", TokenTag::keyword_switch},
    {"test", TokenTag::keyword_test},
    {"threadlocal", TokenTag::keyword_threadlocal},
    {"try", TokenTag::keyword_try},
    {"union", TokenTag::keyword_union},
    {"unreachable", TokenTag::keyword_unreachable},
    {"var", TokenTag::keyword_var},
    {"volatile", TokenTag::keyword_volatile},
    {"while", TokenTag::keyword_while},
};

bool isIdentChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9');
}

bool isIdentStart(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

// '_', 'a'...'d', 'f'...'o', 'q'...'z', 'A'...'D', 'F'...'O', 'Q'...'Z', '0'...'9'
bool isNumberBodyChar(unsigned char c) {
    if (c == '_' || (c >= '0' && c <= '9')) return true;
    if (c == 'e' || c == 'E' || c == 'p' || c == 'P') return false;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// 0x01...0x09, 0x0b...0x1f, 0x7f
bool isControlNoLf(unsigned char c) { return (c >= 0x01 && c <= 0x09) || (c >= 0x0b && c <= 0x1f) || c == 0x7f; }

// 0x01...0x09, 0x0b...0x0c, 0x0e...0x1f, 0x7f
bool isControlNoLfCr(unsigned char c) {
    return (c >= 0x01 && c <= 0x09) || c == 0x0b || c == 0x0c || (c >= 0x0e && c <= 0x1f) || c == 0x7f;
}

}  // namespace

std::string_view tokenTagName(TokenTag tag) { return kTokenTagNames[static_cast<std::size_t>(tag)]; }

std::optional<TokenTag> getKeyword(std::string_view bytes) {
    for (const Keyword& k : kKeywords)
        if (k.text == bytes) return k.tag;
    return std::nullopt;
}

std::string_view tokenLexeme(TokenTag tag) {
    switch (tag) {
        case TokenTag::invalid:
        case TokenTag::identifier:
        case TokenTag::string_literal:
        case TokenTag::multiline_string_literal_line:
        case TokenTag::char_literal:
        case TokenTag::eof:
        case TokenTag::builtin:
        case TokenTag::number_literal:
        case TokenTag::doc_comment:
        case TokenTag::container_doc_comment: return {};
        case TokenTag::invalid_periodasterisks: return ".**";
        case TokenTag::bang: return "!";
        case TokenTag::pipe: return "|";
        case TokenTag::pipe_pipe: return "||";
        case TokenTag::pipe_equal: return "|=";
        case TokenTag::equal: return "=";
        case TokenTag::equal_equal: return "==";
        case TokenTag::equal_angle_bracket_right: return "=>";
        case TokenTag::bang_equal: return "!=";
        case TokenTag::l_paren: return "(";
        case TokenTag::r_paren: return ")";
        case TokenTag::semicolon: return ";";
        case TokenTag::percent: return "%";
        case TokenTag::percent_equal: return "%=";
        case TokenTag::l_brace: return "{";
        case TokenTag::r_brace: return "}";
        case TokenTag::l_bracket: return "[";
        case TokenTag::r_bracket: return "]";
        case TokenTag::period: return ".";
        case TokenTag::period_asterisk: return ".*";
        case TokenTag::ellipsis2: return "..";
        case TokenTag::ellipsis3: return "...";
        case TokenTag::caret: return "^";
        case TokenTag::caret_equal: return "^=";
        case TokenTag::plus: return "+";
        case TokenTag::plus_plus: return "++";
        case TokenTag::plus_equal: return "+=";
        case TokenTag::plus_percent: return "+%";
        case TokenTag::plus_percent_equal: return "+%=";
        case TokenTag::plus_pipe: return "+|";
        case TokenTag::plus_pipe_equal: return "+|=";
        case TokenTag::minus: return "-";
        case TokenTag::minus_equal: return "-=";
        case TokenTag::minus_percent: return "-%";
        case TokenTag::minus_percent_equal: return "-%=";
        case TokenTag::minus_pipe: return "-|";
        case TokenTag::minus_pipe_equal: return "-|=";
        case TokenTag::asterisk: return "*";
        case TokenTag::asterisk_equal: return "*=";
        case TokenTag::asterisk_asterisk: return "**";
        case TokenTag::asterisk_percent: return "*%";
        case TokenTag::asterisk_percent_equal: return "*%=";
        case TokenTag::asterisk_pipe: return "*|";
        case TokenTag::asterisk_pipe_equal: return "*|=";
        case TokenTag::arrow: return "->";
        case TokenTag::colon: return ":";
        case TokenTag::slash: return "/";
        case TokenTag::slash_equal: return "/=";
        case TokenTag::comma: return ",";
        case TokenTag::ampersand: return "&";
        case TokenTag::ampersand_equal: return "&=";
        case TokenTag::question_mark: return "?";
        case TokenTag::angle_bracket_left: return "<";
        case TokenTag::angle_bracket_left_equal: return "<=";
        case TokenTag::angle_bracket_angle_bracket_left: return "<<";
        case TokenTag::angle_bracket_angle_bracket_left_equal: return "<<=";
        case TokenTag::angle_bracket_angle_bracket_left_pipe: return "<<|";
        case TokenTag::angle_bracket_angle_bracket_left_pipe_equal: return "<<|=";
        case TokenTag::angle_bracket_right: return ">";
        case TokenTag::angle_bracket_right_equal: return ">=";
        case TokenTag::angle_bracket_angle_bracket_right: return ">>";
        case TokenTag::angle_bracket_angle_bracket_right_equal: return ">>=";
        case TokenTag::tilde: return "~";
        default: break;
    }
    // keyword_*: the name after the prefix.
    std::string_view name = tokenTagName(tag);
    if (name.substr(0, 8) == "keyword_") return name.substr(8);
    return {};
}

std::string_view tokenSymbol(TokenTag tag) {
    std::string_view lex = tokenLexeme(tag);
    if (!lex.empty()) return lex;
    switch (tag) {
        case TokenTag::invalid: return "invalid token";
        case TokenTag::identifier: return "an identifier";
        case TokenTag::string_literal: return "a string literal";
        case TokenTag::multiline_string_literal_line: return "a multiline string literal";
        case TokenTag::char_literal: return "a character literal";
        case TokenTag::eof: return "EOF";
        case TokenTag::builtin: return "a builtin function";
        case TokenTag::number_literal: return "a number literal";
        case TokenTag::doc_comment:
        case TokenTag::container_doc_comment: return "a document comment";
        default: return {};
    }
}

Tokenizer Tokenizer::init(const char* buffer, std::size_t len) {
    // Skip the UTF-8 BOM if present.
    bool bom = len >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
               static_cast<unsigned char>(buffer[1]) == 0xBB && static_cast<unsigned char>(buffer[2]) == 0xBF;
    return Tokenizer{buffer, len, bom ? std::size_t{3} : std::size_t{0}};
}

Token Tokenizer::next() {
    Token result{TokenTag::invalid, index, 0};
    auto at = [this](std::size_t i) { return static_cast<unsigned char>(buffer[i]); };

    // state: start
start:
    switch (at(index)) {
        case 0:
            if (index == len) return Token{TokenTag::eof, index, index};
            goto invalid;
        case ' ':
        case '\n':
        case '\t':
        case '\r':
            index += 1;
            result.start = index;
            goto start;
        case '"': result.tag = TokenTag::string_literal; goto string_literal;
        case '\'': result.tag = TokenTag::char_literal; goto char_literal;
        case '@': goto saw_at_sign;
        case '=': goto equal;
        case '!': goto bang;
        case '|': goto pipe;
        case '(': result.tag = TokenTag::l_paren; index += 1; goto done;
        case ')': result.tag = TokenTag::r_paren; index += 1; goto done;
        case '[': result.tag = TokenTag::l_bracket; index += 1; goto done;
        case ']': result.tag = TokenTag::r_bracket; index += 1; goto done;
        case ';': result.tag = TokenTag::semicolon; index += 1; goto done;
        case ',': result.tag = TokenTag::comma; index += 1; goto done;
        case '?': result.tag = TokenTag::question_mark; index += 1; goto done;
        case ':': result.tag = TokenTag::colon; index += 1; goto done;
        case '%': goto percent;
        case '*': goto asterisk;
        case '+': goto plus;
        case '<': goto angle_bracket_left;
        case '>': goto angle_bracket_right;
        case '^': goto caret;
        case '\\': result.tag = TokenTag::multiline_string_literal_line; goto backslash;
        case '{': result.tag = TokenTag::l_brace; index += 1; goto done;
        case '}': result.tag = TokenTag::r_brace; index += 1; goto done;
        case '~': result.tag = TokenTag::tilde; index += 1; goto done;
        case '.': goto period;
        case '-': goto minus;
        case '/': goto slash;
        case '&': goto ampersand;
        default:
            if (isIdentStart(at(index))) {
                result.tag = TokenTag::identifier;
                goto identifier;
            }
            if (at(index) >= '0' && at(index) <= '9') {
                result.tag = TokenTag::number_literal;
                index += 1;
                goto int_;
            }
            goto invalid;
    }

expect_newline:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index == len) {
                result.tag = TokenTag::invalid;
                goto done;
            }
            goto invalid;
        case '\n':
            index += 1;
            result.start = index;
            goto start;
        default: goto invalid;
    }

invalid:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index == len) {
                result.tag = TokenTag::invalid;
                goto done;
            }
            goto invalid;
        case '\n': result.tag = TokenTag::invalid; goto done;
        default: goto invalid;
    }

saw_at_sign:
    index += 1;
    switch (at(index)) {
        case 0:
        case '\n': result.tag = TokenTag::invalid; goto done;
        case '"': result.tag = TokenTag::identifier; goto string_literal;
        default:
            if (isIdentStart(at(index))) {
                result.tag = TokenTag::builtin;
                goto builtin;
            }
            goto invalid;
    }

ampersand:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::ampersand_equal;
        index += 1;
    } else {
        result.tag = TokenTag::ampersand;
    }
    goto done;

asterisk:
    index += 1;
    switch (at(index)) {
        case '=': result.tag = TokenTag::asterisk_equal; index += 1; goto done;
        case '*': result.tag = TokenTag::asterisk_asterisk; index += 1; goto done;
        case '%': goto asterisk_percent;
        case '|': goto asterisk_pipe;
        default: result.tag = TokenTag::asterisk; goto done;
    }

asterisk_percent:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::asterisk_percent_equal;
        index += 1;
    } else {
        result.tag = TokenTag::asterisk_percent;
    }
    goto done;

asterisk_pipe:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::asterisk_pipe_equal;
        index += 1;
    } else {
        result.tag = TokenTag::asterisk_pipe;
    }
    goto done;

percent:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::percent_equal;
        index += 1;
    } else {
        result.tag = TokenTag::percent;
    }
    goto done;

plus:
    index += 1;
    switch (at(index)) {
        case '=': result.tag = TokenTag::plus_equal; index += 1; goto done;
        case '+': result.tag = TokenTag::plus_plus; index += 1; goto done;
        case '%': goto plus_percent;
        case '|': goto plus_pipe;
        default: result.tag = TokenTag::plus; goto done;
    }

plus_percent:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::plus_percent_equal;
        index += 1;
    } else {
        result.tag = TokenTag::plus_percent;
    }
    goto done;

plus_pipe:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::plus_pipe_equal;
        index += 1;
    } else {
        result.tag = TokenTag::plus_pipe;
    }
    goto done;

caret:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::caret_equal;
        index += 1;
    } else {
        result.tag = TokenTag::caret;
    }
    goto done;

identifier:
    index += 1;
    if (isIdentChar(at(index))) goto identifier;
    if (auto kw = getKeyword(std::string_view(buffer + result.start, index - result.start))) result.tag = *kw;
    goto done;

builtin:
    index += 1;
    if (isIdentChar(at(index))) goto builtin;
    goto done;

backslash:
    index += 1;
    switch (at(index)) {
        case 0: result.tag = TokenTag::invalid; goto done;
        case '\\': goto multiline_string_literal_line;
        case '\n': result.tag = TokenTag::invalid; goto done;
        default: goto invalid;
    }

string_literal:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index != len) goto invalid;
            result.tag = TokenTag::invalid;
            goto done;
        case '\n': result.tag = TokenTag::invalid; goto done;
        case '\\': goto string_literal_backslash;
        case '"': index += 1; goto done;
        default:
            if (isControlNoLf(at(index))) goto invalid;
            goto string_literal;
    }

string_literal_backslash:
    index += 1;
    switch (at(index)) {
        case 0:
        case '\n': result.tag = TokenTag::invalid; goto done;
        default:
            if (isControlNoLf(at(index))) goto invalid;
            goto string_literal;
    }

char_literal:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index != len) goto invalid;
            result.tag = TokenTag::invalid;
            goto done;
        case '\n': result.tag = TokenTag::invalid; goto done;
        case '\\': goto char_literal_backslash;
        case '\'': index += 1; goto done;
        default:
            if (isControlNoLf(at(index))) goto invalid;
            goto char_literal;
    }

char_literal_backslash:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index != len) goto invalid;
            result.tag = TokenTag::invalid;
            goto done;
        case '\n': result.tag = TokenTag::invalid; goto done;
        default:
            if (isControlNoLf(at(index))) goto invalid;
            goto char_literal;
    }

multiline_string_literal_line:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index != len) goto invalid;
            goto done;
        case '\n': goto done;
        case '\r':
            if (at(index + 1) != '\n') goto invalid;
            goto done;
        default:
            if (isControlNoLfCr(at(index))) goto invalid;
            goto multiline_string_literal_line;
    }

bang:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::bang_equal;
        index += 1;
    } else {
        result.tag = TokenTag::bang;
    }
    goto done;

pipe:
    index += 1;
    switch (at(index)) {
        case '=': result.tag = TokenTag::pipe_equal; index += 1; goto done;
        case '|': result.tag = TokenTag::pipe_pipe; index += 1; goto done;
        default: result.tag = TokenTag::pipe; goto done;
    }

equal:
    index += 1;
    switch (at(index)) {
        case '=': result.tag = TokenTag::equal_equal; index += 1; goto done;
        case '>': result.tag = TokenTag::equal_angle_bracket_right; index += 1; goto done;
        default: result.tag = TokenTag::equal; goto done;
    }

minus:
    index += 1;
    switch (at(index)) {
        case '>': result.tag = TokenTag::arrow; index += 1; goto done;
        case '=': result.tag = TokenTag::minus_equal; index += 1; goto done;
        case '%': goto minus_percent;
        case '|': goto minus_pipe;
        default: result.tag = TokenTag::minus; goto done;
    }

minus_percent:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::minus_percent_equal;
        index += 1;
    } else {
        result.tag = TokenTag::minus_percent;
    }
    goto done;

minus_pipe:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::minus_pipe_equal;
        index += 1;
    } else {
        result.tag = TokenTag::minus_pipe;
    }
    goto done;

angle_bracket_left:
    index += 1;
    switch (at(index)) {
        case '<': goto angle_bracket_angle_bracket_left;
        case '=': result.tag = TokenTag::angle_bracket_left_equal; index += 1; goto done;
        default: result.tag = TokenTag::angle_bracket_left; goto done;
    }

angle_bracket_angle_bracket_left:
    index += 1;
    switch (at(index)) {
        case '=': result.tag = TokenTag::angle_bracket_angle_bracket_left_equal; index += 1; goto done;
        case '|': goto angle_bracket_angle_bracket_left_pipe;
        default: result.tag = TokenTag::angle_bracket_angle_bracket_left; goto done;
    }

angle_bracket_angle_bracket_left_pipe:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::angle_bracket_angle_bracket_left_pipe_equal;
        index += 1;
    } else {
        result.tag = TokenTag::angle_bracket_angle_bracket_left_pipe;
    }
    goto done;

angle_bracket_right:
    index += 1;
    switch (at(index)) {
        case '>': goto angle_bracket_angle_bracket_right;
        case '=': result.tag = TokenTag::angle_bracket_right_equal; index += 1; goto done;
        default: result.tag = TokenTag::angle_bracket_right; goto done;
    }

angle_bracket_angle_bracket_right:
    index += 1;
    if (at(index) == '=') {
        result.tag = TokenTag::angle_bracket_angle_bracket_right_equal;
        index += 1;
    } else {
        result.tag = TokenTag::angle_bracket_angle_bracket_right;
    }
    goto done;

period:
    index += 1;
    switch (at(index)) {
        case '.': goto period_2;
        case '*': goto period_asterisk;
        default: result.tag = TokenTag::period; goto done;
    }

period_2:
    index += 1;
    if (at(index) == '.') {
        result.tag = TokenTag::ellipsis3;
        index += 1;
    } else {
        result.tag = TokenTag::ellipsis2;
    }
    goto done;

period_asterisk:
    index += 1;
    if (at(index) == '*')
        result.tag = TokenTag::invalid_periodasterisks;
    else
        result.tag = TokenTag::period_asterisk;
    goto done;

slash:
    index += 1;
    switch (at(index)) {
        case '/': goto line_comment_start;
        case '=': result.tag = TokenTag::slash_equal; index += 1; goto done;
        default: result.tag = TokenTag::slash; goto done;
    }

line_comment_start:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index != len) goto invalid;
            return Token{TokenTag::eof, index, index};
        case '!': result.tag = TokenTag::container_doc_comment; goto doc_comment;
        case '\n':
            index += 1;
            result.start = index;
            goto start;
        case '/': goto doc_comment_start;
        case '\r': goto expect_newline;
        default:
            if (isControlNoLfCr(at(index))) goto invalid;
            goto line_comment;
    }

doc_comment_start:
    index += 1;
    switch (at(index)) {
        case 0:
        case '\n': result.tag = TokenTag::doc_comment; goto done;
        case '\r':
            if (at(index + 1) == '\n') {
                result.tag = TokenTag::doc_comment;
                goto done;
            }
            goto invalid;
        case '/': goto line_comment;
        default:
            if (isControlNoLfCr(at(index))) goto invalid;
            result.tag = TokenTag::doc_comment;
            goto doc_comment;
    }

line_comment:
    index += 1;
    switch (at(index)) {
        case 0:
            if (index != len) goto invalid;
            return Token{TokenTag::eof, index, index};
        case '\n':
            index += 1;
            result.start = index;
            goto start;
        case '\r': goto expect_newline;
        default:
            if (isControlNoLfCr(at(index))) goto invalid;
            goto line_comment;
    }

doc_comment:
    index += 1;
    switch (at(index)) {
        case 0:
        case '\n': goto done;
        case '\r':
            if (at(index + 1) != '\n') goto invalid;
            goto done;
        default:
            if (isControlNoLfCr(at(index))) goto invalid;
            goto doc_comment;
    }

int_:  // no advance on entry
    if (at(index) == '.') goto int_period;
    if (isNumberBodyChar(at(index))) {
        index += 1;
        goto int_;
    }
    if (at(index) == 'e' || at(index) == 'E' || at(index) == 'p' || at(index) == 'P') goto int_exponent;
    goto done;

int_exponent:
    index += 1;
    if (at(index) == '-' || at(index) == '+') {
        index += 1;
        goto float_;
    }
    goto int_;

int_period:
    index += 1;
    if (isNumberBodyChar(at(index))) {
        index += 1;
        goto float_;
    }
    if (at(index) == 'e' || at(index) == 'E' || at(index) == 'p' || at(index) == 'P') goto float_exponent;
    index -= 1;
    goto done;

float_:  // no advance on entry
    if (isNumberBodyChar(at(index))) {
        index += 1;
        goto float_;
    }
    if (at(index) == 'e' || at(index) == 'E' || at(index) == 'p' || at(index) == 'P') goto float_exponent;
    goto done;

float_exponent:
    index += 1;
    if (at(index) == '-' || at(index) == '+') {
        index += 1;
        goto float_;
    }
    goto float_;

done:
    result.end = index;
    return result;
}

}  // namespace zig
