#pragma once

// The Crystal lexer: a port of the Crystal compiler's own
// (src/compiler/crystal/syntax/{token,lexer}.cr, Crystal 1.19), so mep's
// Crystal language server and formatter read Crystal exactly as the
// compiler does -- the same tokens, the same locations (line and column,
// columns counted in characters), and the same syntax errors, word for
// word. The parser (crystal_parser.h) is, as in the compiler, a subclass
// of the lexer that drives it token by token.
//
// Crystal is Apache-2.0 licensed (Copyright 2012-2026 Manas Technology
// Solutions); this is a derived work, translated to C++ for mep.
//
// What is not ported: the `#<loc:...>` pragmas are lexed (they are
// comments the compiler gives meaning to) but only ever matter inside
// macro expansions, which an editor never sees; the string pool (strings
// are plain std::string); warnings are collected as text.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace crystal {

/** @brief A position: 1-based line and column (characters, not bytes); line 0 means unknown. */
struct Location {
    int line = 0;
    int column = 0;
    bool known() const { return line > 0; }
    bool operator==(const Location &o) const { return line == o.line && column == o.column; }
    bool operator!=(const Location &o) const { return !(*this == o); }
};

/** @brief A syntax error, as the compiler reports it. */
struct SyntaxError : std::runtime_error {
    SyntaxError(const std::string &msg, int line_, int column_, int size_ = 0)
        : std::runtime_error(msg), line(line_), column(column_), size(size_) {}
    int line;
    int column;
    int size;  // characters the error spans, 0 if not given
};

enum class Keyword : uint8_t {
    None,
    ABSTRACT, ALIAS, ALIGNOF, ANNOTATION, AS, AS_QUESTION, ASM, BEGIN, BREAK, CASE, CLASS, DEF, DO,
    ELSE, ELSIF, END, ENSURE, ENUM, EXTEND, FALSE, FOR, FUN, IF, IN, INCLUDE, INSTANCE_ALIGNOF,
    INSTANCE_SIZEOF, IS_A_QUESTION, LIB, MACRO, MODULE, NEXT, NIL, NIL_QUESTION, OF, OFFSETOF, OUT,
    POINTEROF, PRIVATE, PROTECTED, REQUIRE, RESCUE, RESPONDS_TO_QUESTION, RETURN, SELECT, SELF,
    SIZEOF, STRUCT, SUPER, THEN, TRUE, TYPE, TYPEOF, UNINITIALIZED, UNION, UNLESS, UNTIL, VERBATIM,
    WHEN, WHILE, WITH, YIELD,
};

/** @brief The keyword's spelling ("as?", "is_a?", "def"...). */
const char *KeywordName(Keyword k);

enum class Tok : uint8_t {
    EOF_, SPACE, NEWLINE,
    IDENT, CONST, INSTANCE_VAR, CLASS_VAR,
    CHAR, STRING, SYMBOL, NUMBER,
    UNDERSCORE, COMMENT,
    DELIMITER_START, DELIMITER_END,
    STRING_ARRAY_START, INTERPOLATION_START, SYMBOL_ARRAY_START, STRING_ARRAY_END,
    GLOBAL, GLOBAL_MATCH_DATA_INDEX,
    MAGIC_DIR, MAGIC_END_LINE, MAGIC_FILE, MAGIC_LINE,
    MACRO_LITERAL, MACRO_EXPRESSION_START, MACRO_CONTROL_START, MACRO_VAR, MACRO_END,
    // Operators, in codepoint order (Token::Kind's order).
    OP_BANG, OP_BANG_EQ, OP_BANG_TILDE, OP_DOLLAR_QUESTION, OP_DOLLAR_TILDE, OP_PERCENT, OP_PERCENT_EQ,
    OP_PERCENT_RCURLY, OP_AMP, OP_AMP_AMP, OP_AMP_AMP_EQ, OP_AMP_STAR, OP_AMP_STAR_STAR, OP_AMP_STAR_EQ,
    OP_AMP_PLUS, OP_AMP_PLUS_EQ, OP_AMP_MINUS, OP_AMP_MINUS_EQ, OP_AMP_EQ, OP_LPAREN, OP_RPAREN,
    OP_STAR, OP_STAR_STAR, OP_STAR_STAR_EQ, OP_STAR_EQ, OP_PLUS, OP_PLUS_EQ, OP_COMMA, OP_MINUS,
    OP_MINUS_EQ, OP_MINUS_GT, OP_PERIOD, OP_PERIOD_PERIOD, OP_PERIOD_PERIOD_PERIOD, OP_SLASH,
    OP_SLASH_SLASH, OP_SLASH_SLASH_EQ, OP_SLASH_EQ, OP_COLON, OP_COLON_COLON, OP_SEMICOLON, OP_LT,
    OP_LT_LT, OP_LT_LT_EQ, OP_LT_EQ, OP_LT_EQ_GT, OP_EQ, OP_EQ_EQ, OP_EQ_EQ_EQ, OP_EQ_GT, OP_EQ_TILDE,
    OP_GT, OP_GT_EQ, OP_GT_GT, OP_GT_GT_EQ, OP_QUESTION, OP_AT_LSQUARE, OP_LSQUARE,
    OP_LSQUARE_RSQUARE, OP_LSQUARE_RSQUARE_EQ, OP_LSQUARE_RSQUARE_QUESTION, OP_RSQUARE, OP_CARET,
    OP_CARET_EQ, OP_GRAVE, OP_LCURLY, OP_LCURLY_PERCENT, OP_LCURLY_LCURLY, OP_BAR, OP_BAR_EQ,
    OP_BAR_BAR, OP_BAR_BAR_EQ, OP_RCURLY, OP_TILDE,
};

/** @brief Token::Kind#to_s: "EOF", "IDENT", "+=", "__FILE__"... */
const char *TokName(Tok t);
bool IsOperator(Tok t);
bool IsAssignmentOperator(Tok t);
bool IsUnaryOperator(Tok t);
bool IsMagic(Tok t);

enum class NumberKind : uint8_t { I8, I16, I32, I64, I128, U8, U16, U32, U64, U128, F32, F64 };
const char *NumberKindName(NumberKind k);  // "i32", "f64"...
inline bool IsFloat(NumberKind k) { return k == NumberKind::F32 || k == NumberKind::F64; }

enum class DelimiterKind : uint8_t { STRING, REGEX, STRING_ARRAY, SYMBOL_ARRAY, COMMAND, HEREDOC };

/**
 * @brief Token::DelimiterState. `nest`/`end` are characters, except for a heredoc,
 *        whose identifier is `heredoc` and whose nest/end characters are never matched.
 */
struct DelimiterState {
    DelimiterKind kind = DelimiterKind::STRING;
    char32_t nest = 0;
    char32_t end = 0;
    std::string heredoc;  // heredoc identifier (kind == HEREDOC)
    int open_count = 0;
    int heredoc_indent = 0;
    bool allow_escapes = true;

    static DelimiterState Make(DelimiterKind k, char32_t n, char32_t e, bool escapes = true) {
        DelimiterState d;
        d.kind = k;
        d.nest = n;
        d.end = e;
        d.allow_escapes = escapes;
        return d;
    }
    static DelimiterState MakeCount(DelimiterKind k, char32_t n, char32_t e, int count) {
        DelimiterState d = Make(k, n, e);
        d.open_count = count;
        return d;
    }
    static DelimiterState Heredoc(const std::string &id, bool escapes) {
        DelimiterState d;
        d.kind = DelimiterKind::HEREDOC;
        d.heredoc = id;
        d.allow_escapes = escapes;
        return d;
    }
    bool is_heredoc() const { return kind == DelimiterKind::HEREDOC; }
    /** @brief `when string_end` against a character: never true for a heredoc. */
    bool end_is(char32_t c) const { return !is_heredoc() && end == c; }
    bool nest_is(char32_t c) const { return !is_heredoc() && nest == c; }
    DelimiterState with_open_count_delta(int d) const {
        DelimiterState s = *this;
        s.open_count += d;
        return s;
    }
    DelimiterState with_heredoc_indent(int i) const {
        DelimiterState s = *this;
        s.heredoc_indent = i;
        return s;
    }
    /** @brief The end delimiter as text (heredoc identifier or the character). */
    std::string end_text() const;
};

/** @brief Token::MacroState. */
struct MacroState {
    bool whitespace = true;
    int nest = 0;
    int control_nest = 0;
    bool has_delimiter_state = false;
    DelimiterState delimiter_state;
    bool beginning_of_line = true;
    bool yields = false;
    bool comment = false;
    bool has_heredocs = false;
    std::vector<DelimiterState> heredocs;
};

/** @brief What a token's `value` holds. */
enum class TokValue : uint8_t { None, Char, String, Keyword };

struct Token {
    Tok type = Tok::EOF_;
    TokValue vkind = TokValue::None;
    std::string str;           // value when vkind == String
    char32_t ch = 0;           // value when vkind == Char
    Keyword kw = Keyword::None;  // value when vkind == Keyword
    NumberKind number_kind = NumberKind::I32;
    int line_number = 0;
    int column_number = 0;
    DelimiterState delimiter_state;
    MacroState macro_state;
    bool passed_backslash_newline = false;
    bool has_doc = false;
    std::string doc;
    std::string raw;
    int start = 0;  // byte offset of a number token's start (for +/- re-lexing)
    bool invalid_escape = false;
    bool has_location = false;
    Location loc_override;

    Location location() const { return has_location ? loc_override : Location{line_number, column_number}; }
    bool keyword() const { return type == Tok::IDENT && vkind == TokValue::Keyword; }
    bool keyword(Keyword k) const { return type == Tok::IDENT && vkind == TokValue::Keyword && kw == k; }
    /** @brief Token#to_s: the value if any, else the kind's name. */
    std::string to_s() const;
    /** @brief `@token.value.to_s` (empty when there is no value). */
    std::string value_s() const;
    void set_str(std::string s) {
        vkind = TokValue::String;
        str = std::move(s);
    }
    void set_char(char32_t c) {
        vkind = TokValue::Char;
        ch = c;
    }
    void set_kw(Keyword k) {
        vkind = TokValue::Keyword;
        kw = k;
    }
    void copy_from(const Token &o);
};

/** @brief UTF-8 helpers shared by the lexer, parser and formatter. */
std::string Utf8Encode(char32_t c);
size_t Utf8Length(std::string_view s);  // characters
/** @brief Char#inspect: 'a', '\n', '\u{1F600}'... */
std::string CharInspect(char32_t c);
/** @brief String#inspect: "a\n\"b\"". */
std::string StringInspect(std::string_view s);

class Lexer {
public:
    explicit Lexer(std::string_view src);
    virtual ~Lexer() = default;

    virtual Token &next_token();
    Token &next_string_token(const DelimiterState &delimiter_state);
    Token &next_string_array_token();
    Token &next_macro_token(const MacroState &macro_state, bool skip_whitespace);

    Token &token() { return token_; }
    const Token &token() const { return token_; }

    // Options (Crystal's `property?`s).
    bool doc_enabled = false;
    bool comments_enabled = false;
    bool count_whitespace = false;
    bool wants_raw = false;
    bool slash_is_regex = true;
    bool wants_def_or_macro_name_ = false;
    bool wants_symbol = true;

    int line_number = 1;
    int column_number = 1;

    std::vector<std::string> warnings;  // "line:col: message"

    /** @brief When set, the text of every comment next_token skips or returns is appended (the formatter's check). */
    std::vector<std::string> *comment_sink = nullptr;

    /** @brief Heredocs pushed when their start was lexed; their bodies follow the line's end. */
    struct HeredocEntry {
        DelimiterState state;
        void *item;  // the parser's data for it
    };
    std::vector<HeredocEntry> heredocs;

    Location token_end_location();
    void slash_is_regex_on() { slash_is_regex = true; }
    void slash_is_not_regex() { slash_is_regex = false; }

    void next_token_skip_space();
    void next_token_skip_space_or_newline();
    void next_token_skip_statement_end();
    Token &next_token_never_a_symbol();
    void skip_space();
    void skip_space_or_newline();
    void skip_statement_end();
    /** @brief Skips whitespace in macro code; returns it when wants_raw (as the original does). */
    std::string skip_macro_whitespace();

    char32_t current_char() const { return cur_; }
    char32_t peek_next_char() const;
    int current_pos() const { return pos_; }
    void set_current_pos(int pos);
    std::string_view string() const { return src_; }
    std::string string_range(int start) const { return string_range(start, pos_); }
    std::string string_range(int start, int end) const {
        return std::string(src_.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
    }

    static bool ident_start(char32_t c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c > 0x9F;
    }
    static bool ident_part(char32_t c) { return ident_start(c) || (c >= '0' && c <= '9'); }
    static bool is_ident(std::string_view name);
    static bool is_setter(std::string_view name);
    static bool ident_part_or_end(char32_t c) { return ident_part(c) || c == '?' || c == '!'; }
    static char32_t closing_char(char32_t c);
    char32_t closing_char() const { return closing_char(cur_); }

    char32_t next_char();
    char32_t next_char_no_column_increment();
    char32_t next_char_check_line();
    /** @brief Char::Reader#previous_char: steps back one character (the column is the caller's). */
    void previous_char();
    void next_char_tok(Tok t) {
        next_char();
        token_.type = t;
    }

    /** @brief Runs `f`; when it returns false the reader goes back where it was. */
    template <typename F>
    bool lookahead(F &&f, bool preserve_token_on_fail = false) {
        int old_pos = pos_, old_line = line_number, old_column = column_number;
        if (preserve_token_on_fail) temp_token_.copy_from(token_);
        bool result = f();
        if (!result) {
            decode_at(old_pos);
            line_number = old_line;
            column_number = old_column;
            if (preserve_token_on_fail) token_.copy_from(temp_token_);
        }
        return result;
    }

    [[noreturn]] void raise(const std::string &message) { raise(message, line_number, column_number); }
    [[noreturn]] void raise(const std::string &message, int line, int column, int size = 0) {
        throw SyntaxError(message, line, column, size);
    }
    [[noreturn]] void raise(const std::string &message, const Location &loc) {
        throw SyntaxError(message, loc.line, loc.column);
    }
    [[noreturn]] void raise_at_token(const std::string &message, const Token &tok, int size = 0) {
        throw SyntaxError(message, tok.line_number, tok.column_number, size);
    }
    [[noreturn]] void unknown_token();

protected:
    void wants_def_or_macro_name_begin() { wants_def_or_macro_name_ = true; }
    void wants_def_or_macro_name_end() { wants_def_or_macro_name_ = false; }

    std::string_view src_;
    int pos_ = 0;
    char32_t cur_ = 0;
    int cur_width_ = 0;
    Token token_;
    Token temp_token_;
    bool wants_regex_ = true;
    bool comment_is_doc_ = true;
    bool has_token_end_location_ = false;
    Location token_end_location_;
    std::vector<DelimiterState> delimiter_state_stack_;
    int macro_curly_count_ = 0;
    bool stacked_ = false;
    int stacked_line_number_ = 1;
    int stacked_column_number_ = 1;

    void decode_at(int pos);
    void reset_token();
    void incr_column_number(int d = 1);
    void incr_line_number(int column);  // column < 0: keep the column
    void incr_line_number() { incr_line_number(1); }
    bool char_sequence(std::initializer_list<char32_t> chars, bool column_increment = true);
    bool handle_slash_r_slash_n_or_slash_n();
    void set_token_raw_from_start(int start);

    Token &consume_comment(int start);
    void consume_doc();
    void skip_comment();
    void consume_whitespace();
    void consume_newlines();
    Token &check_ident_or_keyword(Keyword k, int start);
    Token &scan_ident(int start);
    void symbol(const std::string &value);
    void next_char_and_symbol(const std::string &value) {
        next_char();
        symbol(value);
    }
    void scan_number(int start, bool negative = false);
    NumberKind consume_number_suffix();
    void raise_value_doesnt_fit_in(const char *type, int start, int pos_before_suffix,
                                   const char *alternative = nullptr);
    void check_int_fits(const char *type, bool is_unsigned, int size, int number_size,
                        const std::string &raw_number_string, int start, int pos_before_suffix, bool negative,
                        bool has_value);
    void next_string_token_noescape(const DelimiterState &ds);
    bool check_heredoc_end(const DelimiterState &ds);
    [[noreturn]] void raise_unterminated_quoted(const DelimiterState &ds);
    enum class MacroKeywordState { None, AbstractDef, Other };
    MacroKeywordState macro_starts_with_keyword(bool beginning_of_line);
    bool peek_not_ident_part_or_end_next_char();
    bool check_heredoc_start(DelimiterState *out);
    uint8_t consume_octal_escape(char32_t c);
    char32_t consume_char_unicode_escape();
    uint8_t consume_string_hex_escape();
    std::string consume_string_unicode_escape();
    std::string consume_string_unicode_brace_escape();
    char32_t consume_non_braced_unicode_escape();
    char32_t consume_braced_unicode_escape(bool allow_spaces = false);
    [[noreturn]] void expected_hexadecimal_character_in_unicode_escape();
    void string_token_escape_value(const std::string &value);
    void delimited_pair(DelimiterKind kind, char32_t nest, char32_t end, int start, bool allow_escapes = true,
                        bool advance = true);
    void consume_loc_pragma();
    void consume_heredoc_start(int start);
    void consume_symbol();
    void consume_variable(Tok type, int start);
};

}  // namespace crystal
