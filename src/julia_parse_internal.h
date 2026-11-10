#ifndef MEP_JULIA_PARSE_INTERNAL_H
#define MEP_JULIA_PARSE_INTERNAL_H

// Internals of the Julia parser (julia_parse.h): characters, the lexer and the
// ParseStream. Ported from JuliaSyntax.jl 0.4.10 (MIT, Copyright (c) 2021
// Julia Computing and contributors).
//
// Indices follow Julia: token, range and lookahead indices are 1-based (0
// means "none"), and SyntaxToken::next_byte is a 1-based byte index, so the
// arithmetic reads exactly like parse_stream.jl.

#include "julia_parse.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jlparse {

//------------------------------------------------------------------------------
// Characters. A JChar is Julia's `Char`: the UTF-8 bytes of the character
// left-aligned in 32 bits ('a' == 0x61000000), malformed sequences included,
// so comparisons and validity checks behave as in Julia.

using JChar = uint32_t;
inline constexpr JChar kEofChar = 0xffffffffu;  // typemax(Char)

/** @brief Julia's Char for code point @p cp. */
constexpr JChar C(char32_t cp) {
    uint32_t u = static_cast<uint32_t>(cp);
    if (u < 0x80) return u << 24;
    if (u < 0x800) return ((0xc0u | (u >> 6)) << 24) | ((0x80u | (u & 0x3f)) << 16);
    if (u < 0x10000)
        return ((0xe0u | (u >> 12)) << 24) | ((0x80u | ((u >> 6) & 0x3f)) << 16) | ((0x80u | (u & 0x3f)) << 8);
    return ((0xf0u | (u >> 18)) << 24) | ((0x80u | ((u >> 12) & 0x3f)) << 16) |
           ((0x80u | ((u >> 6) & 0x3f)) << 8) | (0x80u | (u & 0x3f));
}

bool ismalformed(JChar c);
bool isoverlong(JChar c);
bool isvalid(JChar c);
/** @brief Code point of a valid (or overlong-but-wellformed) char. */
uint32_t codepoint(JChar c);
/** @brief Number of bytes in @p c's encoding (1-4). */
int ncodeunits(JChar c);
/** @brief utf8proc category code (Base.Unicode.category_code); 31 for malformed. */
int category_code(JChar c);
bool isprint(JChar c);
bool isspace(JChar c);
inline bool isdigit(JChar c) { return c >= C('0') && c <= C('9'); }
// MSVC's <ctype.h> defines isascii as a macro, which would mangle this name.
#ifdef isascii
#undef isascii
#endif
inline bool isascii(JChar c) { return c < 0x80000000u; }

/** @brief `read(io, Char)` at byte @p pos of @p s (pos < size); sets @p len to the bytes read. */
JChar ReadCharIO(std::string_view s, size_t pos, size_t *len);
/** @brief String iteration (`iterate(::String, i)`) at byte @p pos. */
JChar ReadCharStr(std::string_view s, size_t pos, size_t *len);

// Base.is_id_start_char / Base.is_id_char (valid chars only).
bool BaseIsIdStartChar(JChar c);
bool BaseIsIdChar(JChar c);

// Tokenizer predicates (tokenize.jl).
bool is_identifier_char(JChar c);
bool is_identifier_start_char(JChar c);
bool is_invisible_char(JChar c);
bool is_never_id_char(JChar c);
bool is_operator_start_char(JChar c);
bool is_dottable_operator_start_char(JChar c);
bool isopsuffix(JChar c);
bool optakessuffix(Kind k);

/** @brief Julia's `repr(c)` for a Char. */
std::string ReprChar(JChar c);
/** @brief Julia's `repr(s)` for a String. */
std::string ReprString(std::string_view s);

//------------------------------------------------------------------------------
// Lexer (tokenize.jl)

struct StringState {
    bool triplestr = false;
    bool raw = false;
    JChar delim = 0;
    int paren_depth = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view text, size_t start = 0);
    RawToken next_token(bool start = true);

private:
    std::string_view text_;
    size_t io_pos_ = 0;  // position of the underlying IOBuffer
    int64_t token_startpos_ = 0;
    Kind last_token_ = K("error");
    std::vector<StringState> string_states_;
    JChar chars_[4] = {};
    int64_t charspos_[4] = {};
    bool dotop_ = false;

    JChar io_readchar();
    JChar peekchar() const { return chars_[1]; }
    JChar dpeekchar2() const { return chars_[2]; }
    JChar peekchar3() const { return chars_[3]; }
    int64_t position() const { return charspos_[0]; }
    int64_t startpos() const { return token_startpos_; }
    void start_token() { token_startpos_ = charspos_[0]; }
    JChar readchar();
    bool accept(JChar c);
    bool accept_str(std::u32string_view set);
    template <class F> bool accept_f(F f);
    template <class F> bool accept_batch(F f);
    RawToken emit(Kind kind, bool maybe_op = true);

    RawToken _next_token(JChar c);
    RawToken lex_string_chunk();
    RawToken lex_whitespace(JChar c);
    RawToken lex_comment();
    RawToken lex_greater();
    RawToken lex_less();
    RawToken lex_equal();
    RawToken lex_colon();
    RawToken lex_exclaim();
    RawToken lex_percent();
    RawToken lex_bar();
    RawToken lex_plus();
    RawToken lex_minus();
    RawToken lex_star();
    RawToken lex_circumflex();
    RawToken lex_division();
    RawToken lex_dollar();
    RawToken lex_xor();
    template <class F> bool accept_number(F f);
    RawToken lex_digit(Kind kind);
    RawToken lex_prime();
    RawToken lex_amper();
    RawToken lex_quote();
    bool string_terminates(JChar delim, bool triplestr) const;
    RawToken lex_forwardslash();
    RawToken lex_backslash();
    RawToken lex_dot();
    RawToken lex_backtick();
    RawToken lex_identifier(JChar c);
};

//------------------------------------------------------------------------------
// ParseStream (parse_stream.jl)

struct SyntaxToken {
    Kind kind{};
    RawFlags flags = 0;
    Kind orig_kind{};
    bool preceding_whitespace = false;
    uint32_t next_byte = 0;  // 1-based index of the byte after the token
};

struct TaggedRange {
    Kind kind{};
    RawFlags flags = 0;
    uint32_t first_token = 0;  // 1-based
    uint32_t last_token = 0;   // 1-based
};

struct ParseStreamPosition {
    uint32_t token_index = 0;  // index of last token in output
    uint32_t range_index = 0;
    bool operator==(const ParseStreamPosition &o) const {
        return token_index == o.token_index && range_index == o.range_index;
    }
    bool operator!=(const ParseStreamPosition &o) const { return !(*this == o); }
};
inline constexpr ParseStreamPosition NO_POSITION{0, 0};

struct PeekBehind {
    Kind kind{};
    RawFlags flags = 0;
    Kind orig_kind{};
    bool is_leaf = true;
};

struct FullToken {
    Kind kind{};
    RawFlags flags = 0;
    uint32_t first_byte = 0;
    uint32_t last_byte = 0;
    uint32_t span() const { return 1 + last_byte - first_byte; }
};

struct SplitSpec {
    int nbyte;
    Kind kind;
    RawFlags flags;
};

class ParseStream {
public:
    explicit ParseStream(std::string_view text);

    std::string_view text;
    Lexer lexer;
    std::vector<SyntaxToken> lookahead;
    size_t lookahead_index = 1;
    std::vector<SyntaxToken> tokens;
    std::vector<TaggedRange> ranges;
    std::vector<Diagnostic> diagnostics;
    int peek_count = 0;
    int version_major = 1, version_minor = 12;

    // Tokens (1-based indices) that 1.0.2 remaps to Identifier, so its
    // validate_tokens doesn't report them (the "not a unary operator" path).
    std::vector<size_t> remapped_102;

    // Depth safety (not in JuliaSyntax).
    int depth = 0;
    bool depth_exceeded = false;

    // 1-based accessors
    SyntaxToken &tok(size_t i) { return tokens[i - 1]; }
    const SyntaxToken &tok(size_t i) const { return tokens[i - 1]; }
    TaggedRange &rng(size_t i) { return ranges[i - 1]; }
    const TaggedRange &rng(size_t i) const { return ranges[i - 1]; }
    SyntaxToken &la(size_t i) { return lookahead[i - 1]; }

    bool token_is_last(ParseStreamPosition pos) const;
    uint32_t token_first_byte(size_t i) const { return tok(i - 1).next_byte; }
    uint32_t token_last_byte(size_t i) const { return tok(i).next_byte - 1; }
    uint32_t lookahead_token_first_byte(size_t i) {
        return i == 1 ? _next_byte() : la(i - 1).next_byte;
    }
    uint32_t lookahead_token_last_byte(size_t i) { return la(i).next_byte - 1; }
    uint32_t _next_byte() const { return tokens.back().next_byte; }

    void _buffer_lookahead_tokens();
    size_t _lookahead_index(size_t n, bool skip_newlines);
    Kind peek(size_t n = 1, bool skip_newlines = false, bool skip_whitespace = true);
    SyntaxToken peek_token(size_t n = 1, bool skip_newlines = false, bool skip_whitespace = true);
    FullToken peek_full_token(size_t n = 1, bool skip_newlines = false, bool skip_whitespace = true);
    PeekBehind peek_behind(ParseStreamPosition pos) const;
    ParseStreamPosition first_child_position(ParseStreamPosition pos) const;
    ParseStreamPosition last_child_position(ParseStreamPosition pos) const;  // 1.0.2
    ParseStreamPosition peek_behind_pos(bool skip_trivia = true, bool skip_parens = true) const;
    PeekBehind peek_behind(bool skip_trivia = true, bool skip_parens = true) const {
        return peek_behind(peek_behind_pos(skip_trivia, skip_parens));
    }

    void _bump_until_n(size_t n, RawFlags flags, Kind remap_kind = K("None"));
    ParseStreamPosition bump(RawFlags flags = EMPTY_FLAGS, bool skip_newlines = false, const char *error = nullptr,
                             Kind remap_kind = K("None"));
    ParseStreamPosition bump(RawFlags flags, bool skip_newlines, const std::string &error, Kind remap_kind);
    ParseStreamPosition bump_trivia(RawFlags flags = EMPTY_FLAGS, bool skip_newlines = true,
                                    const char *error = nullptr);
    ParseStreamPosition bump_invisible(Kind kind, RawFlags flags = EMPTY_FLAGS, const std::string *error = nullptr);
    ParseStreamPosition bump_glue(Kind kind, RawFlags flags);
    ParseStreamPosition bump_split(std::initializer_list<SplitSpec> split_spec);
    void reset_node(ParseStreamPosition pos, const Kind *kind, const RawFlags *flags);
    bool steal_token_bytes(ParseStreamPosition pos, uint32_t numbytes);
    ParseStreamPosition position() const {
        return {static_cast<uint32_t>(tokens.size()), static_cast<uint32_t>(ranges.size())};
    }
    ParseStreamPosition emit(ParseStreamPosition mark, Kind kind, RawFlags flags = EMPTY_FLAGS,
                             const std::string *error = nullptr);
    void emit_diagnostic_range(int64_t fbyte, int64_t lbyte, Level level, std::string msg);
    void emit_diagnostic_here(bool whitespace, Level level, std::string msg);
    void emit_diagnostic_mark(ParseStreamPosition mark, Level level, std::string msg, bool trim_whitespace = true);
    void emit_diagnostic_marks(ParseStreamPosition mark, ParseStreamPosition end_mark, Level level, std::string msg);

    void validate_tokens();
    Tree build_tree() const;
};

// Parser entry point (parser.jl parse_toplevel).
void ParseToplevel(ParseStream &stream);

// Literal parsing (literal_parsing.jl).
enum class FloatStatus { Ok, Underflow, Overflow };
FloatStatus parse_float_literal(bool is_float32, std::string_view txt, size_t firstind, size_t endind,
                                double *value);
/** @brief Unescapes txt[firstind, endind) (1-based) into @p out (if non-null); diagnostics into @p diags. */
bool unescape_julia_string(std::string *out, std::string_view txt, size_t firstind, size_t endind,
                           std::vector<Diagnostic> *diags);

}  // namespace jlparse

#endif  // MEP_JULIA_PARSE_INTERNAL_H
