// External scanner for the Crystal grammar (grammar.js).
//
// Crystal's lexer (src/compiler/crystal/syntax/lexer.cr) is driven by the
// parser: the parser tells it whether a space was significant, whether a
// slash may start a regex, whether `{` may open a block, and the lexer has
// separate modes for string, heredoc and macro text. The tokens below are
// the ones a context-free lexer can't decide; each is decided here from
// `valid_symbols`, which is how the tree-sitter parser says what it
// expects.
//
// State, all serialized so an incremental reparse restores it:
//   * the stack of open delimited literals ("...", `...`, %(...), %w[...],
//     %r{...}): an interpolation inside one can open another;
//   * the queue of heredocs whose bodies are still to come (a line may
//     open several, and an interpolation in a body may open one more);
//   * where macro text is: inside a comment, and whether `if`/`unless`/
//     `while`/`until` would start a statement there or be a suffix;
//   * whether the previous token was a line continuation.
//
// The order of the checks in scan() is load-bearing: several tokens can be
// valid at once and the first family that recognizes the input wins. It is
// the order the node API was established with (crystal-lang-tools'
// tree-sitter-crystal), kept so that trees are identical; see README.md.
// So are the exact advance/skip/mark_end sequences: they set token ranges,
// and a hidden token's range shows up in its parent's.

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    LINE_BREAK,
    LINE_CONTINUATION,

    START_OF_BRACE_BLOCK,
    START_OF_HASH_OR_TUPLE,
    START_OF_NAMED_TUPLE,
    START_OF_TUPLE_TYPE,
    START_OF_NAMED_TUPLE_TYPE,

    START_OF_INDEX_OPERATOR,

    END_OF_WITH_EXPRESSION,

    UNARY_PLUS,
    UNARY_MINUS,
    BINARY_PLUS,
    BINARY_MINUS,

    UNARY_WRAPPING_PLUS,
    UNARY_WRAPPING_MINUS,
    BINARY_WRAPPING_PLUS,
    BINARY_WRAPPING_MINUS,

    POINTER_STAR,
    UNARY_STAR,
    BINARY_STAR,
    UNARY_DOUBLE_STAR,
    BINARY_DOUBLE_STAR,

    BLOCK_AMPERSAND,
    BINARY_AMPERSAND,

    BEGINLESS_RANGE_OPERATOR,

    REGEX_START,
    BINARY_SLASH,
    BINARY_DOUBLE_SLASH,

    REGULAR_IF_KEYWORD,
    MODIFIER_IF_KEYWORD,
    REGULAR_UNLESS_KEYWORD,
    MODIFIER_UNLESS_KEYWORD,
    REGULAR_RESCUE_KEYWORD,
    MODIFIER_RESCUE_KEYWORD,
    REGULAR_ENSURE_KEYWORD,
    MODIFIER_ENSURE_KEYWORD,

    MODULO_OPERATOR,

    START_OF_SYMBOL,
    UNQUOTED_SYMBOL_CONTENT,

    TYPE_FIELD_COLON,

    STRING_LITERAL_START,
    DELIMITED_STRING_CONTENTS,
    STRING_LITERAL_END,

    COMMAND_LITERAL_START,
    COMMAND_LITERAL_END,

    STRING_PERCENT_LITERAL_START,
    COMMAND_PERCENT_LITERAL_START,
    STRING_ARRAY_PERCENT_LITERAL_START,
    SYMBOL_ARRAY_PERCENT_LITERAL_START,
    REGEX_PERCENT_LITERAL_START,
    PERCENT_LITERAL_END,
    DELIMITED_ARRAY_ELEMENT_START,
    DELIMITED_ARRAY_ELEMENT_END,

    HEREDOC_START,
    HEREDOC_BODY_START,
    HEREDOC_CONTENT,
    HEREDOC_END,

    REGEX_MODIFIER,

    MACRO_START,
    MACRO_DELIMITER_END,
    MACRO_DELIMITER_ELSE,
    MACRO_DELIMITER_ELSIF,
    MACRO_CONTENT,
    MACRO_CONTENT_NESTING,

    // Never produced: only their validity is read.
    START_OF_PARENLESS_ARGS,
    END_OF_RANGE,
    START_OF_MACRO_VAR_EXPS,
    ERROR_RECOVERY,

    NO_TOKEN_YET,
};

// ---- state -------------------------------------------------------------------

// What kind of text a delimited literal holds (reference: "Literals").
typedef enum {
    TEXT_STRING,     // "..." and %(...) %Q(...): escapes and interpolation
    TEXT_RAW,        // %q(...): neither
    TEXT_COMMAND,    // `...` and %x(...): like a string
    TEXT_WORDS,      // %w(...): whitespace-separated words
    TEXT_SYMBOLS,    // %i(...): the same, as symbols
    TEXT_REGEX,      // %r(...): interpolation, backslashes kept as written
} TextKind;

typedef struct {
    uint8_t open;    // ( [ { < nest; " ` | don't (open == close)
    uint8_t close;
    uint8_t depth;   // unclosed nested opening delimiters
    uint8_t kind;    // TextKind
} Literal;

typedef struct {
    bool interpolates;        // <<-ID: escapes and #{}; <<-'ID': neither
    bool started;             // its body has begun
    Array(int32_t) word;      // the identifier, as codepoints
} Heredoc;

#define MAX_LITERALS 16
#define MAX_HEREDOCS 16
// Codepoints across all pending heredoc identifiers; each is serialized
// in three bytes, so this bounds the state at well under the 1 KiB
// tree-sitter allows.
#define MAX_HEREDOC_CODEPOINTS 160

typedef struct {
    // Set during a scan when whitespace was skipped before the token: the
    // difference between `foo -1` (an argument) and `foo - 1`.
    bool leading_space;
    // The previous token was a line continuation (a backslash-newline):
    // the next line isn't where a pending heredoc body starts.
    bool after_continuation;

    // Macro text (lexer.cr's next_macro_token): inside a `#` comment, where
    // keywords don't count; and whether `if`/`unless`/`while`/`until` here
    // would start a statement (true at a line's start, after `=` and an
    // opening brace) rather than be a suffix.
    bool macro_in_comment;
    bool macro_statement_keyword_ok;

    Array(Literal) literals;
    Array(Heredoc) heredocs;
} Scanner;

// What a token family's scan concluded: it read a token, it is sure there
// is none here, or the input isn't its business and the next family in
// scan()'s order should look.
typedef enum { PASS, TOKEN, NO_TOKEN } Outcome;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

// Skipping moves the token's start: never call it after mark_end.
static inline void skip(Scanner *s, TSLexer *lexer) {
    s->leading_space = true;
    lexer->advance(lexer, true);
}

static inline bool at(TSLexer *lexer, int32_t c) { return lexer->lookahead == c; }

static inline bool produce(TSLexer *lexer, enum TokenType type) {
    lexer->result_symbol = type;
    return true;
}

static inline bool is_ascii_alnum(int32_t c) {
    return ('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z');
}

// lexer.cr's ident_part?: what may continue an identifier or a constant.
static inline bool is_ident_part(int32_t c) { return is_ascii_alnum(c) || c == '_' || c >= 0xa0; }

// What may start an identifier, a constant or an unquoted symbol.
static inline bool is_name_start(int32_t c) {
    return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || c == '_' || c >= 0xa0;
}

// The end of a keyword: nothing that would make it a longer name, a
// `foo?` or a `foo!` follows.
static inline bool continues_name(int32_t c) { return is_ident_part(c) || c == '?' || c == '!'; }

static inline bool is_blank_or_newline(int32_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void skip_blanks(TSLexer *lexer) {
    while (at(lexer, ' ') || at(lexer, '\t')) advance(lexer);
}

static void skip_blanks_and_newlines(TSLexer *lexer) {
    while (is_blank_or_newline(lexer->lookahead)) advance(lexer);
}

// Consumes `chars` while they match; false at the first mismatch.
static bool read_chars(TSLexer *lexer, const char *chars) {
    for (; *chars; chars++) {
        if (lexer->lookahead != (unsigned char)*chars) return false;
        advance(lexer);
    }
    return true;
}

// The rest of a keyword (its first character already consumed), and then
// the end of the name.
static bool read_rest(TSLexer *lexer, const char *rest) {
    return read_chars(lexer, rest) && !continues_name(lexer->lookahead);
}

// The closing delimiter of a %-literal opened by `c`, or 0.
static int32_t closing_delimiter(int32_t c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '<': return '>';
        case '|': return '|';
        default: return 0;
    }
}

static inline Literal *top_literal(Scanner *s) { return s->literals.size ? array_back(&s->literals) : NULL; }

static void push_literal(Scanner *s, int32_t open, int32_t close, TextKind kind) {
    Literal literal = {.open = (uint8_t)open, .close = (uint8_t)close, .depth = 0, .kind = (uint8_t)kind};
    array_push(&s->literals, literal);
}

// ---- heredocs ------------------------------------------------------------------
// Reference: "Heredoc". `<<-ID` (or `<<-'ID'`, no escapes or interpolation)
// is an expression; its body starts on the next line and ends at a line
// holding only ID, after optional indentation. Bodies come in the order
// their heredocs were opened, except that one opened inside another's body
// (in an interpolation) comes right after the line that opened it.

static bool any_heredoc_started(Scanner *s) {
    for (uint32_t i = 0; i < s->heredocs.size; i++) {
        if (array_get(&s->heredocs, i)->started) return true;
    }
    return false;
}

static bool next_heredoc_pending(Scanner *s) {
    return s->heredocs.size > 0 && !array_front(&s->heredocs)->started;
}

static uint32_t heredoc_codepoints(Scanner *s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->heredocs.size; i++) n += array_get(&s->heredocs, i)->word.size;
    return n;
}

// The first heredoc in the queue is done.
static void heredoc_pop_front(Scanner *s) {
    if (s->heredocs.size == 0) return;
    array_delete(&array_front(&s->heredocs)->word);
    array_erase(&s->heredocs, 0);
}

// Queue a heredoc: last, or -- opened inside a body -- just before the body
// being read, so it is the next to start.
static void heredoc_enqueue(Scanner *s, Heredoc heredoc) {
    uint32_t index = s->heredocs.size;
    for (uint32_t i = 0; i < s->heredocs.size; i++) {
        if (array_get(&s->heredocs, i)->started) {
            index = i;
            break;
        }
    }
    array_insert(&s->heredocs, index, heredoc);
}

// `<<-` was read: the identifier, one identifier character first even when
// quoted. False (no token) when there is none, it is unterminated, or it
// doesn't fit in the state.
static bool scan_heredoc_start(Scanner *s, TSLexer *lexer) {
    bool quoted = false;
    if (at(lexer, '\'')) {
        quoted = true;
        advance(lexer);
    }
    if (s->heredocs.size >= MAX_HEREDOCS || !is_ident_part(lexer->lookahead)) return false;
    uint32_t room = MAX_HEREDOC_CODEPOINTS - heredoc_codepoints(s);
    if (room == 0) return false;

    Heredoc heredoc = {.interpolates = !quoted, .started = false, .word = array_new()};
    bool closed = false;
    for (;;) {
        if (lexer->eof(lexer) || at(lexer, '\r') || at(lexer, '\n')) break;
        if (quoted && at(lexer, '\'')) {
            closed = true;
            advance(lexer);
            break;
        }
        if (!quoted && !is_ident_part(lexer->lookahead)) break;
        if (heredoc.word.size == room) {
            array_delete(&heredoc.word);
            return false;
        }
        array_push(&heredoc.word, lexer->lookahead);
        advance(lexer);
    }
    if (quoted && !closed) {
        array_delete(&heredoc.word);
        return false;
    }
    heredoc_enqueue(s, heredoc);
    return produce(lexer, HEREDOC_START);
}

// A pending body starts here when nothing else has started it: at the
// start of a line that doesn't continue the previous one. (After an
// ordinary line break scan_whitespace starts it instead.)
static bool scan_heredoc_body_start(Scanner *s, TSLexer *lexer, const bool *valid) {
    // get_column at EOF can loop, so EOF is checked first.
    if (valid[HEREDOC_BODY_START] && next_heredoc_pending(s) && !s->after_continuation && !lexer->eof(lexer) &&
        lexer->get_column(lexer) == 0) {
        array_front(&s->heredocs)->started = true;
        return produce(lexer, HEREDOC_BODY_START);
    }
    return false;
}

// A body's text, across as many lines as possible, up to an escape, an
// interpolation or the terminating line; or that terminating line itself
// (HEREDOC_END).
static bool scan_heredoc_content(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!any_heredoc_started(s)) return false;

    // The body being read is the started one. If the queue's head isn't
    // started, an interpolation on the current line opened a heredoc whose
    // body comes next: stop at the end of this line.
    Heredoc *heredoc = array_front(&s->heredocs);
    bool nested_pending = !heredoc->started;
    if (nested_pending) {
        for (uint32_t i = 1; i < s->heredocs.size; i++) {
            if (array_get(&s->heredocs, i)->started) {
                heredoc = array_get(&s->heredocs, i);
                break;
            }
        }
    }

    bool found = false;
    for (;;) {
        // At the start of a line.
        if (found && nested_pending) return true;

        if (valid[HEREDOC_END] && !lexer->eof(lexer) && lexer->get_column(lexer) == 0) {
            if (found) {
                lexer->mark_end(lexer);
                while (at(lexer, ' ') || at(lexer, '\t')) advance(lexer);
            } else {
                while (at(lexer, ' ') || at(lexer, '\t')) skip(s, lexer);
                lexer->mark_end(lexer);
            }
            uint32_t matched = 0;
            while (matched < heredoc->word.size && lexer->lookahead == *array_get(&heredoc->word, matched)) {
                advance(lexer);
                matched++;
            }
            bool line_ends = at(lexer, '\n') || at(lexer, '\r') || lexer->eof(lexer);
            if (matched == heredoc->word.size && line_ends) {
                // The text before it is a token of its own; the next scan
                // comes back here for the terminator.
                if (found) return true;
                heredoc_pop_front(s);
                lexer->mark_end(lexer);
                return produce(lexer, HEREDOC_END);
            }
            if (matched > 0) {
                found = true;
                lexer->mark_end(lexer);
            }
        }

        lexer->result_symbol = HEREDOC_CONTENT;
        for (;;) {
            if (lexer->eof(lexer)) return found;
            int32_t c = lexer->lookahead;
            if (c == '\\' && heredoc->interpolates) return found;
            if (c == '#' && heredoc->interpolates) {
                lexer->mark_end(lexer);
                advance(lexer);
                if (at(lexer, '{')) return found;
                found = true;
                lexer->mark_end(lexer);
                continue;
            }
            if (c == '\r') {
                advance(lexer);
                lexer->mark_end(lexer);
                found = true;
                if (!at(lexer, '\n')) continue;
                c = '\n';
            }
            advance(lexer);
            lexer->mark_end(lexer);
            found = true;
            if (c == '\n') break;
        }
    }
}

// ---- whitespace and line breaks ----------------------------------------------
// A newline ends a statement unless the next line continues the
// expression: a leading `.` (a method chain; `..` is a range, which does
// end it), a comment line, or ` : Type` after a parameter list.
//
// False: no token can start here (a control character). True with
// result_symbol NO_TOKEN_YET: whitespace skipped, look further.

static bool scan_whitespace(Scanner *s, TSLexer *lexer, const bool *valid) {
    bool crossed_newline = false;
    for (;;) {
        switch (lexer->lookahead) {
            case ' ':
            case '\t':
            case '\r':
                skip(s, lexer);
                break;

            case '\n':
                if (valid[HEREDOC_BODY_START] && next_heredoc_pending(s)) {
                    // A zero-width token after the newline, which is skipped.
                    array_front(&s->heredocs)->started = true;
                    skip(s, lexer);
                    return produce(lexer, HEREDOC_BODY_START);
                }
                if (valid[LINE_BREAK] && !crossed_newline) {
                    advance(lexer);
                    lexer->mark_end(lexer);
                    crossed_newline = true;
                    s->leading_space = true;
                } else {
                    skip(s, lexer);
                }
                break;

            case '\v':
            case '\f':
                // Not allowed in code, but they separate %w words.
                if (s->literals.size > 0) {
                    skip(s, lexer);
                    break;
                }
                return false;

            default:
                if (!crossed_newline) return true;
                if (at(lexer, '.')) {
                    advance(lexer);
                    if (at(lexer, '.')) lexer->result_symbol = LINE_BREAK;
                } else if (at(lexer, '#')) {
                    // A comment doesn't end the continuation.
                } else if (at(lexer, ':') && valid[TYPE_FIELD_COLON]) {
                    advance(lexer);
                    if (iswspace(lexer->lookahead)) {
                        advance(lexer);
                        lexer->mark_end(lexer);
                        lexer->result_symbol = TYPE_FIELD_COLON;
                    } else {
                        lexer->result_symbol = LINE_BREAK;
                    }
                } else {
                    lexer->result_symbol = LINE_BREAK;
                }
                return true;
        }
    }
}

// ---- delimited literals --------------------------------------------------------
// The text of the innermost open literal, up to an escape, an
// interpolation, the closing delimiter, or (in %w/%i) a word's end. PASS
// leaves the closing delimiter and line continuations to later checks.

static Outcome stop(bool found) { return found ? TOKEN : NO_TOKEN; }

static Outcome scan_literal_text(Scanner *s, TSLexer *lexer, const bool *valid) {
    Literal *literal = top_literal(s);
    bool found = false;
    lexer->result_symbol = DELIMITED_STRING_CONTENTS;

    for (;;) {
        if (lexer->eof(lexer)) return stop(found);
        int32_t c = lexer->lookahead;
        bool words = literal->kind == TEXT_WORDS || literal->kind == TEXT_SYMBOLS;

        switch (c) {
            case '\\':
                if (literal->kind == TEXT_STRING || literal->kind == TEXT_COMMAND) {
                    // An escape sequence (the grammar's) or a line continuation.
                    return found ? TOKEN : PASS;
                }
                if (literal->kind == TEXT_REGEX) {
                    // Kept as written: the backslash and what it escapes.
                    advance(lexer);
                } else if (words) {
                    // Only whitespace and the closing delimiter escape.
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (iswspace(lexer->lookahead) || lexer->lookahead == literal->close) return stop(found);
                    found = true;
                    lexer->mark_end(lexer);
                    continue;
                }
                break;

            case '#':
                if (literal->kind == TEXT_RAW || words) break;
                lexer->mark_end(lexer);
                advance(lexer);
                if (at(lexer, '{')) return stop(found);
                found = true;
                lexer->mark_end(lexer);
                continue;

            case ' ':
            case '\t':
            case '\n':
            case '\r':
            case '\v':
            case '\f':
                if (words) {
                    if (found) return TOKEN;
                    if (valid[DELIMITED_ARRAY_ELEMENT_END]) {
                        lexer->result_symbol = DELIMITED_ARRAY_ELEMENT_END;
                        return TOKEN;
                    }
                }
                break;

            case '"':
            case '|':
            case '`':
                // Delimiters that can't nest.
                if (c == literal->close) {
                    if (found) return TOKEN;
                    if (valid[DELIMITED_ARRAY_ELEMENT_END]) {
                        lexer->result_symbol = DELIMITED_ARRAY_ELEMENT_END;
                        return TOKEN;
                    }
                    return PASS;
                }
                break;

            case '(':
            case '[':
            case '{':
            case '<':
                if (c == literal->open) literal->depth++;
                break;

            case ')':
            case ']':
            case '}':
            case '>':
                if (c == literal->close) {
                    if (literal->depth == 0) {
                        if (found) return TOKEN;
                        if (valid[DELIMITED_ARRAY_ELEMENT_END]) {
                            lexer->result_symbol = DELIMITED_ARRAY_ELEMENT_END;
                            return TOKEN;
                        }
                        return PASS;
                    }
                    literal->depth--;
                }
                break;
        }

        advance(lexer);
        lexer->mark_end(lexer);
        found = true;
    }
}

// ---- macro text ------------------------------------------------------------------
// Reference: "Macros". Between `{{ }}` and `{% %}`, a macro body is text.
// In a `macro` definition's body (MACRO_CONTENT_NESTING valid) the text
// ends at the `end` that closes the definition, so keywords that open a
// construct count (lexer.cr's next_macro_token keeps the same count): the
// scan stops before each one, the grammar reads it, and the grammar's
// nesting decides which `end` is the last. `if`, `unless`, `while` and
// `until` open one only where a statement could start. In the bodies of
// `{% if %}` and friends (MACRO_CONTENT) nothing nests.
//
// The scan may stop at a keyword with text before it (TOKEN) and is then
// called again at the keyword, where it finds no text (NO_TOKEN) and the
// grammar's own token for the keyword matches.

typedef struct {
    bool nesting;       // keywords count
    bool found;         // text consumed: the scan will produce a token
    bool keyword_ok;    // a nesting keyword may start here (not mid-word)
} MacroScan;

// Reads a whole word (identifier characters and one trailing ? or !):
// true when it is exactly `rest` -- after the characters already consumed
// -- and isn't a named-tuple key (`begin:`). Stops right after a match.
static bool macro_word_is(TSLexer *lexer, const char *rest) {
    bool match = true;
    for (; *rest; rest++) {
        if (lexer->lookahead != (unsigned char)*rest) {
            match = false;
            break;
        }
        advance(lexer);
    }
    if (at(lexer, ':')) return false;
    if (!match || continues_name(lexer->lookahead)) {
        while (is_ident_part(lexer->lookahead)) advance(lexer);
        if (continues_name(lexer->lookahead)) advance(lexer);
        return false;
    }
    return true;
}

// A nesting keyword was read: stop before it if it counts here.
static Outcome macro_keyword(Scanner *s, MacroScan *m) {
    if (m->nesting && !s->macro_in_comment && m->keyword_ok) return stop(m->found);
    return PASS;
}

// `if`, `unless`, `while` or `until` was read where it could start a
// statement.
static Outcome macro_statement_keyword(Scanner *s, MacroScan *m) {
    if (m->nesting && !s->macro_in_comment && m->keyword_ok) {
        if (m->found) return TOKEN;
        s->macro_statement_keyword_ok = false;
        return NO_TOKEN;
    }
    return PASS;
}

// A word starting with a letter that may begin a keyword. PASS: the word
// is text, go on.
static Outcome scan_macro_word(Scanner *s, TSLexer *lexer, MacroScan *m) {
    Outcome out = PASS;
    switch (lexer->lookahead) {
        case 'a':
            advance(lexer);
            s->macro_statement_keyword_ok = false;
            if (at(lexer, 'b')) {
                // `abstract class` and `abstract struct` open one;
                // `abstract def` doesn't (it has no body).
                if (macro_word_is(lexer, "bstract") && iswspace(lexer->lookahead)) {
                    advance(lexer);
                    if (at(lexer, 'c')) {
                        if (macro_word_is(lexer, "class")) out = macro_keyword(s, m);
                    } else if (at(lexer, 's')) {
                        if (macro_word_is(lexer, "struct")) out = macro_keyword(s, m);
                    } else if (at(lexer, 'd')) {
                        macro_word_is(lexer, "def");
                    }
                }
            } else if (at(lexer, 'n')) {
                if (macro_word_is(lexer, "nnotation")) out = macro_keyword(s, m);
            }
            break;

        case 'b':
            s->macro_statement_keyword_ok = false;
            if (macro_word_is(lexer, "begin")) out = macro_keyword(s, m);
            break;

        case 'c':
            advance(lexer);
            s->macro_statement_keyword_ok = false;
            if (at(lexer, 'l')) {
                if (macro_word_is(lexer, "lass")) out = macro_keyword(s, m);
            } else if (at(lexer, 'a')) {
                if (macro_word_is(lexer, "ase")) out = macro_keyword(s, m);
            }
            break;

        case 'd':
            advance(lexer);
            s->macro_statement_keyword_ok = false;
            if (at(lexer, 'e')) {
                if (macro_word_is(lexer, "ef")) out = macro_keyword(s, m);
            } else if (at(lexer, 'o')) {
                if (macro_word_is(lexer, "o")) out = macro_keyword(s, m);
            }
            break;

        case 'e':
            advance(lexer);
            s->macro_statement_keyword_ok = false;
            if (at(lexer, 'n')) {
                advance(lexer);
                if (at(lexer, 'd')) {
                    if (macro_word_is(lexer, "d")) out = macro_keyword(s, m);
                } else if (at(lexer, 'u')) {
                    if (macro_word_is(lexer, "um")) out = macro_keyword(s, m);
                }
            }
            break;

        case 'f':
            s->macro_statement_keyword_ok = false;
            if (macro_word_is(lexer, "fun")) out = macro_keyword(s, m);
            break;

        case 'l':
            s->macro_statement_keyword_ok = false;
            if (macro_word_is(lexer, "lib")) out = macro_keyword(s, m);
            break;

        case 'm':
            advance(lexer);
            s->macro_statement_keyword_ok = false;
            if (at(lexer, 'a')) {
                if (macro_word_is(lexer, "acro")) out = macro_keyword(s, m);
            } else if (at(lexer, 'o')) {
                if (macro_word_is(lexer, "odule")) out = macro_keyword(s, m);
            }
            break;

        case 's':
            advance(lexer);
            s->macro_statement_keyword_ok = false;
            if (at(lexer, 'e')) {
                if (macro_word_is(lexer, "elect")) out = macro_keyword(s, m);
            } else if (at(lexer, 't')) {
                if (macro_word_is(lexer, "truct")) out = macro_keyword(s, m);
            }
            break;

        case 'i':
        case 'w': {
            const char *word = at(lexer, 'i') ? "if" : "while";
            if (!s->macro_statement_keyword_ok) {
                advance(lexer);
            } else if (macro_word_is(lexer, word)) {
                out = macro_statement_keyword(s, m);
            } else {
                s->macro_statement_keyword_ok = false;
            }
            break;
        }

        case 'u':
            advance(lexer);
            if (!at(lexer, 'n')) break;
            advance(lexer);
            if (at(lexer, 'i')) {
                s->macro_statement_keyword_ok = false;
                if (macro_word_is(lexer, "ion")) out = macro_keyword(s, m);
            } else if (at(lexer, 'l')) {
                if (s->macro_statement_keyword_ok && macro_word_is(lexer, "less")) out = macro_statement_keyword(s, m);
            } else if (at(lexer, 't')) {
                if (s->macro_statement_keyword_ok && macro_word_is(lexer, "til")) out = macro_statement_keyword(s, m);
            } else {
                s->macro_statement_keyword_ok = false;
            }
            break;
    }
    return out;
}

// `{%` was read: which keyword follows decides MACRO_DELIMITER_END, _ELSE
// or _ELSIF; a plain `{%` is the grammar's token.
static Outcome scan_macro_delimiter(TSLexer *lexer) {
    advance(lexer);
    lexer->mark_end(lexer);
    while (iswspace(lexer->lookahead)) advance(lexer);
    if (!at(lexer, 'e')) return NO_TOKEN;
    advance(lexer);
    if (at(lexer, 'n')) {
        if (macro_word_is(lexer, "nd")) {
            lexer->result_symbol = MACRO_DELIMITER_END;
            return TOKEN;
        }
    } else if (at(lexer, 'l')) {
        advance(lexer);
        if (!at(lexer, 's')) return NO_TOKEN;
        advance(lexer);
        if (at(lexer, 'e')) {
            if (macro_word_is(lexer, "e")) {
                lexer->result_symbol = MACRO_DELIMITER_ELSE;
                return TOKEN;
            }
        } else if (at(lexer, 'i')) {
            if (macro_word_is(lexer, "if")) {
                lexer->result_symbol = MACRO_DELIMITER_ELSIF;
                return TOKEN;
            }
        }
    }
    return NO_TOKEN;
}

static Outcome scan_macro_text(Scanner *s, TSLexer *lexer, const bool *valid) {
    MacroScan m = {.nesting = false, .found = false, .keyword_ok = true};
    lexer->result_symbol = MACRO_CONTENT;
    if (valid[MACRO_CONTENT_NESTING]) {
        m.nesting = true;
        lexer->result_symbol = MACRO_CONTENT_NESTING;
    }

    for (;;) {
        if (lexer->eof(lexer)) return stop(m.found);
        int32_t c = lexer->lookahead;

        switch (c) {
            case '{':
                lexer->mark_end(lexer);
                // `%var{` -- the grammar reads the keys.
                if (valid[START_OF_MACRO_VAR_EXPS] && !m.found) return NO_TOKEN;
                advance(lexer);
                if (at(lexer, '{') || at(lexer, '%')) {
                    // `{{` or `{%`: after it, `if` is a suffix.
                    s->macro_statement_keyword_ok = false;
                    if (m.found) return TOKEN;
                    return scan_macro_delimiter(lexer);
                }
                // A tuple, hash or brace block opens.
                m.found = true;
                m.keyword_ok = true;
                s->macro_statement_keyword_ok = true;
                lexer->mark_end(lexer);
                continue;

            case '}':
                lexer->mark_end(lexer);
                advance(lexer);
                s->macro_statement_keyword_ok = false;
                if (at(lexer, '}')) return stop(m.found);
                m.found = true;
                m.keyword_ok = true;
                lexer->mark_end(lexer);
                continue;

            case '%':
                lexer->mark_end(lexer);
                advance(lexer);
                s->macro_statement_keyword_ok = false;
                if (at(lexer, '}')) return stop(m.found);
                if (at(lexer, 'i') || at(lexer, 'q') || at(lexer, 'Q') || at(lexer, 'r') || at(lexer, 'w') ||
                    at(lexer, 'x')) {
                    // A %-literal's opening, read as text.
                    advance(lexer);
                    if (at(lexer, '(') || at(lexer, '<') || at(lexer, '[') || at(lexer, '{') || at(lexer, '|')) {
                        advance(lexer);
                    }
                } else if (is_ident_part(lexer->lookahead)) {
                    // A macro variable: the grammar's.
                    return stop(m.found);
                }
                m.found = true;
                m.keyword_ok = false;
                lexer->mark_end(lexer);
                continue;

            case '"':
                s->macro_statement_keyword_ok = false;
                if (valid[STRING_LITERAL_START]) return m.found ? TOKEN : PASS;
                advance(lexer);
                lexer->mark_end(lexer);
                m.found = true;
                m.keyword_ok = false;
                continue;

            case '#':
                // The rest of the line is a comment: keywords don't count.
                advance(lexer);
                lexer->mark_end(lexer);
                s->macro_in_comment = true;
                m.found = true;
                m.keyword_ok = false;
                s->macro_statement_keyword_ok = false;
                continue;

            case 'a':
            case 'b':
            case 'c':
            case 'd':
            case 'e':
            case 'f':
            case 'i':
            case 'l':
            case 'm':
            case 's':
            case 'u':
            case 'w': {
                lexer->mark_end(lexer);
                Outcome out = scan_macro_word(s, lexer, &m);
                if (out != PASS) return out;
                // A keyword can't start right after a word.
                m.found = true;
                m.keyword_ok = false;
                lexer->mark_end(lexer);
                continue;
            }

            case '\n':
                advance(lexer);
                lexer->mark_end(lexer);
                m.found = true;
                s->macro_in_comment = false;
                m.keyword_ok = true;
                s->macro_statement_keyword_ok = true;
                continue;

            case ' ':
            case '\t':
            case '\f':
            case '\v':
            case '\r':
            case '(':
            case '[':
                advance(lexer);
                lexer->mark_end(lexer);
                m.found = true;
                m.keyword_ok = true;
                continue;

            case '=':
                advance(lexer);
                lexer->mark_end(lexer);
                m.found = true;
                m.keyword_ok = false;
                // `x = if ...`: a statement keyword after `= `.
                if (iswspace(lexer->lookahead)) s->macro_statement_keyword_ok = true;
                continue;
        }

        advance(lexer);
        lexer->mark_end(lexer);
        m.found = true;
        m.keyword_ok = false;
        s->macro_statement_keyword_ok = false;
    }
}

// ---- symbols and regex modifiers -------------------------------------------------

// `:name`, `:name?`, `:name!`, `:name=` (not `:name==`, which compares).
static bool scan_symbol_name(TSLexer *lexer) {
    if (!is_name_start(lexer->lookahead)) return false;
    lexer->result_symbol = UNQUOTED_SYMBOL_CONTENT;
    advance(lexer);
    while (is_ident_part(lexer->lookahead)) advance(lexer);
    if (at(lexer, '?')) {
        advance(lexer);
    } else if (at(lexer, '!') || at(lexer, '=')) {
        lexer->mark_end(lexer);
        advance(lexer);
        if (!at(lexer, '=')) lexer->mark_end(lexer);
    }
    return true;
}

// `/re/imx`: the flags must touch the closing slash.
static bool scan_regex_modifier(Scanner *s, TSLexer *lexer) {
    if (s->leading_space) return false;
    bool found = false;
    while (at(lexer, 'i') || at(lexer, 'm') || at(lexer, 'x')) {
        found = true;
        advance(lexer);
    }
    return found && produce(lexer, REGEX_MODIFIER);
}


// ---- type lookahead ----------------------------------------------------------------
// Whether a `{` opens a tuple type or a block / hash: parser.cr decides
// "is this the start of a type?" with a rough look at the next tokens
// (Parser#type_start?), and so does this, with the same rough edges. These
// only look: the caller has called mark_end before them.

typedef enum { LOOKS_UNKNOWN, LOOKS_TYPE, LOOKS_NAMED_TUPLE } Looks;

static void skip_constant_segment(TSLexer *lexer) {
    if ('A' <= lexer->lookahead && lexer->lookahead <= 'Z') {
        advance(lexer);
        while (is_ident_part(lexer->lookahead)) advance(lexer);
    }
}

// Skips a "..." or %(...)/%q(...)/%Q(...) literal: a named tuple key.
static void skip_string_key(TSLexer *lexer) {
    bool escapes = true, nests = false;
    int32_t open = 0, close = 0;
    if (at(lexer, '"')) {
        open = close = '"';
    } else if (at(lexer, '%')) {
        advance(lexer);
        if (at(lexer, 'q')) {
            escapes = false;
            advance(lexer);
        } else if (at(lexer, 'Q')) {
            advance(lexer);
        }
        close = closing_delimiter(lexer->lookahead);
        if (close) {
            open = lexer->lookahead;
            nests = open != '|';
        }
    }
    if (!open) return;
    advance(lexer);
    uint32_t depth = 0;
    while (!lexer->eof(lexer)) {
        if (at(lexer, '\\') && escapes) {
            advance(lexer);
            advance(lexer);
        } else if (lexer->lookahead == close) {
            advance(lexer);
            if (depth == 0) return;
            depth--;
        } else if (lexer->lookahead == open && nests) {
            advance(lexer);
            depth++;
        } else {
            advance(lexer);
        }
    }
}

// A key and a colon -- `name:`, `name?:`, `"any text":` -- start a named
// tuple (`::` doesn't).
static Looks looks_named_tuple_key(TSLexer *lexer, bool in_name) {
    if (in_name || is_name_start(lexer->lookahead)) {
        while (is_ident_part(lexer->lookahead)) advance(lexer);
        if (at(lexer, '!') || at(lexer, '?')) advance(lexer);
        if (at(lexer, ':')) {
            advance(lexer);
            return at(lexer, ':') ? LOOKS_UNKNOWN : LOOKS_NAMED_TUPLE;
        }
    }
    if (at(lexer, '"') || at(lexer, '%')) {
        skip_string_key(lexer);
        if (at(lexer, ':')) {
            advance(lexer);
            return at(lexer, ':') ? LOOKS_UNKNOWN : LOOKS_NAMED_TUPLE;
        }
    }
    return LOOKS_UNKNOWN;
}

// After a type's name: a suffix (`?`, `*`, `.class`) or something that
// ends a type (`|`, `,`, `)`, `->`, `=>`, `=`, a newline...).
static Looks looks_after_type(TSLexer *lexer) {
    if (lexer->eof(lexer)) return LOOKS_TYPE;
    switch (lexer->lookahead) {
        case '.':
            advance(lexer);
            skip_blanks_and_newlines(lexer);
            if (!read_chars(lexer, "class")) return LOOKS_UNKNOWN;
            return is_ident_part(lexer->lookahead) ? LOOKS_UNKNOWN : LOOKS_TYPE;

        case '?':
        case '*':
            advance(lexer);
            return looks_after_type(lexer);

        case '-':
            advance(lexer);
            return at(lexer, '>') ? LOOKS_TYPE : LOOKS_UNKNOWN;

        case '=':
            advance(lexer);
            return at(lexer, '=') || at(lexer, '~') ? LOOKS_UNKNOWN : LOOKS_TYPE;

        case '|':
        case ',':
        case ';':
        case '\n':
        case '(':
        case ')':
        case '[':
        case ']':
            return LOOKS_TYPE;

        default:
            return LOOKS_UNKNOWN;
    }
}

// The rest of `self` or `typeof` was read; anything that continues the
// name makes it some other name, perhaps a named tuple key.
static bool keyword_continues(TSLexer *lexer, bool question_too) {
    return is_ident_part(lexer->lookahead) || at(lexer, '!') || (question_too && at(lexer, '?'));
}

static Looks looks_like_type(TSLexer *lexer) {
    skip_blanks(lexer);
    if (lexer->eof(lexer)) return LOOKS_UNKNOWN;

    while (at(lexer, '{') || at(lexer, '(')) {
        advance(lexer);
        skip_blanks_and_newlines(lexer);
    }

    // `typeof(...)` and `self` are types; other lowercase names are not
    // (unless they are a named tuple's keys). A partial match falls
    // through to the checks below with whatever character stopped it.
    if (at(lexer, 't')) {
        advance(lexer);
        if (read_chars(lexer, "ypeof")) {
            if (at(lexer, ':')) return LOOKS_NAMED_TUPLE;
            if (keyword_continues(lexer, true)) return looks_named_tuple_key(lexer, true);
            return LOOKS_TYPE;
        }
    } else if (at(lexer, 's')) {
        advance(lexer);
        if (read_chars(lexer, "elf")) {
            if (at(lexer, ':')) return LOOKS_NAMED_TUPLE;
            if (keyword_continues(lexer, false)) return looks_named_tuple_key(lexer, true);
            skip_blanks(lexer);
            return looks_after_type(lexer);
        }
    } else if (('a' <= lexer->lookahead && lexer->lookahead <= 'z') || lexer->lookahead >= 0xa0) {
        return looks_named_tuple_key(lexer, false);
    }

    // A constant path: `Foo`, `Foo::Bar`; `Foo:` is a key.
    while ('A' <= lexer->lookahead && lexer->lookahead <= 'Z') {
        skip_constant_segment(lexer);
        if (!at(lexer, ':')) {
            skip_blanks(lexer);
            return looks_after_type(lexer);
        }
        advance(lexer);
        if (!at(lexer, ':')) return LOOKS_NAMED_TUPLE;
        advance(lexer);
        skip_blanks_and_newlines(lexer);
    }

    switch (lexer->lookahead) {
        case '_':
            // `_` alone is the underscore type.
            advance(lexer);
            if (!iswalnum(lexer->lookahead)) return LOOKS_TYPE;
            break;
        case '-':
            advance(lexer);
            if (at(lexer, '>')) return LOOKS_TYPE;
            break;
        case '*':
            // A splat of a type (not `**`, which no type starts with).
            advance(lexer);
            skip_blanks_and_newlines(lexer);
            if (at(lexer, '*')) return LOOKS_UNKNOWN;
            return looks_like_type(lexer) == LOOKS_TYPE ? LOOKS_TYPE : LOOKS_UNKNOWN;
        case '"':
        case '%':
            return looks_named_tuple_key(lexer, false);
    }
    return LOOKS_UNKNOWN;
}

// ---- `{` ----------------------------------------------------------------------------
// A `{` (not `{{` or `{%`) opens a brace block, a hash or tuple, a named
// tuple, a tuple type or a named tuple type. Which of those the parser
// can take narrows it; where several remain, the rules are parser.cr's.

static bool scan_open_brace(TSLexer *lexer, const bool *valid) {
    bool block = valid[START_OF_BRACE_BLOCK];
    bool expr = valid[START_OF_HASH_OR_TUPLE] || valid[START_OF_NAMED_TUPLE];
    bool type = valid[START_OF_TUPLE_TYPE] || valid[START_OF_NAMED_TUPLE_TYPE];

    if (block && expr && type) return false;  // only while recovering from an error

    if (block && expr) {
        // A call's first parenless argument can't start with `{`: after a
        // method name it is the call's block (`foo { 1 }`); a tuple
        // argument needs parentheses.
        if (valid[START_OF_PARENLESS_ARGS]) return produce(lexer, START_OF_BRACE_BLOCK);
        // `foo 1 .. { 4 }`: after a range operator it is always a value.
        if (valid[END_OF_RANGE]) {
            lexer->mark_end(lexer);
            skip_blanks_and_newlines(lexer);
            return produce(lexer, looks_named_tuple_key(lexer, false) == LOOKS_NAMED_TUPLE ? START_OF_NAMED_TUPLE
                                                                                        : START_OF_HASH_OR_TUPLE);
        }
        // `Set{1, 2}`: an array-like or hash-like literal (no named tuple).
        if (valid[START_OF_HASH_OR_TUPLE] && !valid[START_OF_NAMED_TUPLE]) {
            return produce(lexer, START_OF_HASH_OR_TUPLE);
        }
        return false;
    }

    if (block && type) {
        // `-> : -> {->} {`: a tuple type, then the proc's block; `-> : -> {
        // ; ... }`: the block. Anything like a named tuple's key is taken
        // as not a type, as parser.cr does.
        lexer->mark_end(lexer);
        return produce(lexer, looks_like_type(lexer) == LOOKS_TYPE ? START_OF_TUPLE_TYPE : START_OF_BRACE_BLOCK);
    }

    if (expr && type) {
        // `def foo : ->{Char, Char}; ...` versus `def foo : ->{'a' => 'b'}`.
        lexer->mark_end(lexer);
        switch (looks_like_type(lexer)) {
            case LOOKS_TYPE: return produce(lexer, START_OF_TUPLE_TYPE);
            case LOOKS_NAMED_TUPLE: return produce(lexer, START_OF_NAMED_TUPLE);
            default: return produce(lexer, START_OF_HASH_OR_TUPLE);
        }
    }

    if (expr) {
        lexer->mark_end(lexer);
        skip_blanks_and_newlines(lexer);
        if (valid[START_OF_HASH_OR_TUPLE] && !valid[START_OF_NAMED_TUPLE]) {
            return produce(lexer, START_OF_HASH_OR_TUPLE);
        }
        if (valid[START_OF_NAMED_TUPLE] && !valid[START_OF_HASH_OR_TUPLE]) {
            return produce(lexer, START_OF_NAMED_TUPLE);
        }
        return produce(lexer, looks_named_tuple_key(lexer, false) == LOOKS_NAMED_TUPLE ? START_OF_NAMED_TUPLE
                                                                                    : START_OF_HASH_OR_TUPLE);
    }

    if (type) {
        lexer->mark_end(lexer);
        skip_blanks_and_newlines(lexer);
        return produce(lexer, looks_named_tuple_key(lexer, false) == LOOKS_NAMED_TUPLE ? START_OF_NAMED_TUPLE_TYPE
                                                                                    : START_OF_TUPLE_TYPE);
    }

    return block && produce(lexer, START_OF_BRACE_BLOCK);
}

// ---- operators ----------------------------------------------------------------------
// Whether an operator is unary or binary depends on the spaces around it
// (lexer.cr and parser.cr): `foo -1` passes -1 to foo, `foo - 1` and
// `foo-1` subtract. The unary reading wins when there is space before the
// operator and none after it, and right after a range operator.

static bool unary_wins(Scanner *s, TSLexer *lexer) { return s->leading_space && !iswspace(lexer->lookahead); }

// `+` and `-` (`+=`, `-=` and `->` are the grammar's).
static bool scan_plus_minus(Scanner *s, TSLexer *lexer, const bool *valid, enum TokenType unary,
                            enum TokenType binary) {
    if (!valid[unary] && !valid[binary]) return false;
    int32_t sign = lexer->lookahead;
    advance(lexer);
    if (at(lexer, '=') || (sign == '-' && at(lexer, '>'))) return false;
    bool unary_first = unary_wins(s, lexer) || valid[END_OF_RANGE];
    if (valid[unary] && unary_first) return produce(lexer, unary);
    return produce(lexer, valid[binary] ? binary : unary);
}

// `*` and `**`: a pointer type's suffix, a splat, or multiplication and
// exponentiation. A splat can't have a space after it.
static bool scan_star(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[POINTER_STAR] && !valid[UNARY_STAR] && !valid[BINARY_STAR] && !valid[UNARY_DOUBLE_STAR] &&
        !valid[BINARY_DOUBLE_STAR]) {
        return false;
    }
    advance(lexer);
    if (valid[POINTER_STAR] && !valid[ERROR_RECOVERY]) return produce(lexer, POINTER_STAR);
    if (at(lexer, '=')) return false;

    enum TokenType unary = UNARY_STAR, binary = BINARY_STAR;
    if (at(lexer, '*')) {
        advance(lexer);
        if (at(lexer, '=')) return false;
        unary = UNARY_DOUBLE_STAR;
        binary = BINARY_DOUBLE_STAR;
    }
    if (valid[unary] && unary_wins(s, lexer)) return produce(lexer, unary);
    if (valid[binary]) return produce(lexer, binary);
    if (valid[unary] && !iswspace(lexer->lookahead)) return produce(lexer, unary);
    return false;
}

// `&`: a block argument (`foo &block`, `&.name`, `&->proc`), binary and,
// or the wrapping operators `&+` and `&-`. (`&*`, `&**`, `&&` and `&=`
// are the grammar's.)
static bool scan_ampersand(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[UNARY_WRAPPING_PLUS] && !valid[UNARY_WRAPPING_MINUS] && !valid[BINARY_WRAPPING_PLUS] &&
        !valid[BINARY_WRAPPING_MINUS] && !valid[BLOCK_AMPERSAND] && !valid[BINARY_AMPERSAND]) {
        return false;
    }
    advance(lexer);
    lexer->mark_end(lexer);

    if (at(lexer, '+')) {
        // `&+` is binary whenever it can be: `foo! &+bar` adds.
        advance(lexer);
        if (at(lexer, '=')) return false;
        lexer->mark_end(lexer);
        if (valid[BINARY_WRAPPING_PLUS]) return produce(lexer, BINARY_WRAPPING_PLUS);
        if (valid[UNARY_WRAPPING_PLUS]) return produce(lexer, UNARY_WRAPPING_PLUS);
        return false;
    }

    if (at(lexer, '-')) {
        advance(lexer);
        if (at(lexer, '=')) return false;
        if (at(lexer, '>')) {
            // `&->proc`: the token is just the `&` (marked above).
            if (s->leading_space && valid[BLOCK_AMPERSAND]) return produce(lexer, BLOCK_AMPERSAND);
            if (valid[BINARY_AMPERSAND]) return produce(lexer, BINARY_AMPERSAND);
            if (valid[BLOCK_AMPERSAND]) return produce(lexer, BLOCK_AMPERSAND);
            return false;
        }
        lexer->mark_end(lexer);
        if (valid[BINARY_WRAPPING_MINUS]) return produce(lexer, BINARY_WRAPPING_MINUS);
        if (valid[UNARY_WRAPPING_MINUS]) return produce(lexer, UNARY_WRAPPING_MINUS);
        return false;
    }

    if (at(lexer, '*') || at(lexer, '&') || at(lexer, '=')) return false;

    // `&.name` is always a block argument.
    if (at(lexer, '.')) return valid[BLOCK_AMPERSAND] && produce(lexer, BLOCK_AMPERSAND);

    // `foo &bar` passes a block; `foo & bar` and `foo&bar` are binary.
    if (unary_wins(s, lexer) && valid[BLOCK_AMPERSAND]) return produce(lexer, BLOCK_AMPERSAND);
    if (valid[BINARY_AMPERSAND]) return produce(lexer, BINARY_AMPERSAND);
    if (valid[BLOCK_AMPERSAND]) return produce(lexer, BLOCK_AMPERSAND);
    return false;
}

// `/`: division, floor division `//`, or a regex. After a method name
// with space before the slash and none after (`foo /a/`), parser.cr reads
// a regex; `foo/a`, `foo / a` and `foo/ a` divide. `/=` is the grammar's
// unless only a regex can start here.
static bool scan_slash(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[REGEX_START] && !valid[BINARY_SLASH] && !valid[BINARY_DOUBLE_SLASH]) return false;
    advance(lexer);

    if (at(lexer, '=')) return valid[REGEX_START] && !valid[BINARY_SLASH] && produce(lexer, REGEX_START);

    if (at(lexer, '/') && valid[BINARY_DOUBLE_SLASH]) {
        advance(lexer);
        return !at(lexer, '=') && produce(lexer, BINARY_DOUBLE_SLASH);
    }

    if (valid[BINARY_SLASH] && !valid[REGEX_START]) return produce(lexer, BINARY_SLASH);
    if (valid[REGEX_START] && !valid[BINARY_SLASH]) return produce(lexer, REGEX_START);

    // Both: after a parenless method name, or after a range operator
    // (`1 .. /x/` is a regex).
    if (valid[START_OF_PARENLESS_ARGS]) {
        bool regex = s->leading_space && !is_blank_or_newline(lexer->lookahead);
        return produce(lexer, regex ? REGEX_START : BINARY_SLASH);
    }
    if (valid[END_OF_RANGE]) return produce(lexer, REGEX_START);
    return false;
}

// `%`: a %-literal (reference: "Percent literals"), or modulo. `%=` and
// the `%}` closing a macro statement are the grammar's.
static bool scan_percent(Scanner *s, TSLexer *lexer, const bool *valid) {
    advance(lexer);
    if (at(lexer, '}') || at(lexer, '=')) return false;

    if (!valid[STRING_PERCENT_LITERAL_START] && !valid[COMMAND_PERCENT_LITERAL_START] &&
        !valid[STRING_ARRAY_PERCENT_LITERAL_START] && !valid[SYMBOL_ARRAY_PERCENT_LITERAL_START] &&
        !valid[REGEX_PERCENT_LITERAL_START]) {
        return valid[MODULO_OPERATOR] && produce(lexer, MODULO_OPERATOR);
    }

    TextKind kind = TEXT_STRING;
    enum TokenType start = STRING_PERCENT_LITERAL_START;
    switch (lexer->lookahead) {
        case 'Q': advance(lexer); break;
        case 'q': advance(lexer); kind = TEXT_RAW; break;
        case 'x': advance(lexer); kind = TEXT_COMMAND; start = COMMAND_PERCENT_LITERAL_START; break;
        case 'w': advance(lexer); kind = TEXT_WORDS; start = STRING_ARRAY_PERCENT_LITERAL_START; break;
        case 'i': advance(lexer); kind = TEXT_SYMBOLS; start = SYMBOL_ARRAY_PERCENT_LITERAL_START; break;
        case 'r': advance(lexer); kind = TEXT_REGEX; start = REGEX_PERCENT_LITERAL_START; break;
    }

    int32_t open = lexer->lookahead, close = closing_delimiter(open);
    if (!close) return valid[MODULO_OPERATOR] && produce(lexer, MODULO_OPERATOR);
    advance(lexer);
    // Nested deeper than the state holds: an error in the tree, the outer
    // literals still read.
    if (!valid[start] || s->literals.size >= MAX_LITERALS) return false;
    push_literal(s, open, close, kind);
    return produce(lexer, start);
}

// `"` and `` ` ``: open a literal, or (when the literal stack has none
// for it) close one.
static bool scan_quote(Scanner *s, TSLexer *lexer, const bool *valid, enum TokenType start, enum TokenType end,
                       TextKind kind) {
    int32_t quote = lexer->lookahead;
    if (valid[start]) {
        advance(lexer);
        push_literal(s, quote, quote, kind);
        return produce(lexer, start);
    }
    if (valid[end]) {
        advance(lexer);
        return produce(lexer, end);
    }
    return false;
}

// A backslash-newline: the line goes on. Not inside literals that keep
// backslashes as written, nor in a quoted heredoc's body.
static bool scan_line_continuation(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[LINE_CONTINUATION]) return false;
    if (any_heredoc_started(s) && !array_front(&s->heredocs)->interpolates) return false;
    Literal *literal = top_literal(s);
    if (literal && literal->kind != TEXT_STRING && literal->kind != TEXT_COMMAND) return false;
    advance(lexer);
    if (at(lexer, '\r')) advance(lexer);
    if (!at(lexer, '\n')) return false;
    advance(lexer);
    s->after_continuation = true;
    return produce(lexer, LINE_CONTINUATION);
}

// `:`: a symbol's start, or the ` : ` before a type.
static bool scan_colon(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[START_OF_SYMBOL] && !valid[TYPE_FIELD_COLON]) return false;
    advance(lexer);
    int32_t c = lexer->lookahead;
    if (s->leading_space && iswspace(c) && valid[TYPE_FIELD_COLON]) {
        advance(lexer);
        return produce(lexer, TYPE_FIELD_COLON);
    }
    if (!valid[START_OF_SYMBOL]) return false;
    // An operator symbol (`:+`, `:[]=`) or a named one.
    if (c > 0 && c < 128 && strchr("!%&*+-/<=>[^|~", (int)c)) return produce(lexer, START_OF_SYMBOL);
    return is_name_start(c) && produce(lexer, START_OF_SYMBOL);
}

// `if`, `unless`, `rescue`, `ensure`: a statement's keyword or a suffix
// (`x if y`). Where both are possible the suffix wins: that is after
// something that may end a statement, like a parenless call's name or
// `return`.
static bool scan_keyword(TSLexer *lexer, const bool *valid, const char *rest, enum TokenType regular,
                         enum TokenType modifier) {
    if (!valid[regular] && !valid[modifier]) return false;
    advance(lexer);
    if (!read_rest(lexer, rest)) return false;
    return produce(lexer, valid[modifier] ? modifier : regular);
}

// ---- the scan ---------------------------------------------------------------------

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid) {
    s->leading_space = false;
    bool recovering = valid[ERROR_RECOVERY];

    if (scan_heredoc_body_start(s, lexer, valid)) return true;
    s->after_continuation = false;

    if (valid[DELIMITED_STRING_CONTENTS] && s->literals.size > 0) {
        Outcome out = scan_literal_text(s, lexer, valid);
        if (out != PASS) return out == TOKEN;
    }

    if (valid[HEREDOC_CONTENT] && s->heredocs.size > 0 && scan_heredoc_content(s, lexer, valid)) return true;

    if (valid[MACRO_START] && !recovering) {
        s->macro_in_comment = false;
        s->macro_statement_keyword_ok = true;
        return produce(lexer, MACRO_START);
    }

    if ((valid[MACRO_CONTENT] || valid[MACRO_CONTENT_NESTING]) && !recovering) {
        Outcome out = scan_macro_text(s, lexer, valid);
        if (out != PASS) return out == TOKEN;
    }

    if (valid[UNQUOTED_SYMBOL_CONTENT] && !recovering && scan_symbol_name(lexer)) return true;

    lexer->result_symbol = NO_TOKEN_YET;
    if (!scan_whitespace(s, lexer, valid)) return false;
    if (lexer->result_symbol != NO_TOKEN_YET) return true;

    Literal *literal = top_literal(s);
    if (literal && lexer->lookahead == literal->close) {
        enum TokenType end = valid[PERCENT_LITERAL_END]  ? PERCENT_LITERAL_END
                             : valid[STRING_LITERAL_END] ? STRING_LITERAL_END
                             : valid[COMMAND_LITERAL_END] ? COMMAND_LITERAL_END
                                                          : NO_TOKEN_YET;
        if (end != NO_TOKEN_YET) {
            advance(lexer);
            (void)array_pop(&s->literals);
            return produce(lexer, end);
        }
    }

    if (valid[DELIMITED_ARRAY_ELEMENT_START] && literal) return produce(lexer, DELIMITED_ARRAY_ELEMENT_START);

    if (valid[REGEX_MODIFIER] && scan_regex_modifier(s, lexer)) return true;

    switch (lexer->lookahead) {
        case '{':
            advance(lexer);
            if (at(lexer, '{') || at(lexer, '%')) return false;  // a macro's
            return scan_open_brace(lexer, valid);

        case '[':
            // `foo [1]` passes an array; `foo[1]` and `{42} [0]` index.
            if (!valid[START_OF_INDEX_OPERATOR]) return false;
            if (s->leading_space && valid[START_OF_PARENLESS_ARGS]) return false;
            advance(lexer);
            return produce(lexer, START_OF_INDEX_OPERATOR);

        case '<':
            if (!valid[HEREDOC_START]) return false;
            advance(lexer);
            if (!at(lexer, '<')) return false;
            advance(lexer);
            if (!at(lexer, '-')) return false;
            advance(lexer);
            return scan_heredoc_start(s, lexer);

        case '+':
            return scan_plus_minus(s, lexer, valid, UNARY_PLUS, BINARY_PLUS);
        case '-':
            return scan_plus_minus(s, lexer, valid, UNARY_MINUS, BINARY_MINUS);
        case '*':
            return scan_star(s, lexer, valid);
        case '&':
            return scan_ampersand(s, lexer, valid);
        case '/':
            return scan_slash(s, lexer, valid);
        case '%':
            return scan_percent(s, lexer, valid);
        case '"':
            return scan_quote(s, lexer, valid, STRING_LITERAL_START, STRING_LITERAL_END, TEXT_STRING);
        case '`':
            return scan_quote(s, lexer, valid, COMMAND_LITERAL_START, COMMAND_LITERAL_END, TEXT_COMMAND);
        case '\\':
            return scan_line_continuation(s, lexer, valid);
        case ':':
            return scan_colon(s, lexer, valid);

        case '.':
            // `..x` / `...x` with no begin (not after a method name, where
            // it's a range argument's own business).
            if (!valid[BEGINLESS_RANGE_OPERATOR] || valid[START_OF_PARENLESS_ARGS]) return false;
            advance(lexer);
            if (!at(lexer, '.')) return false;
            advance(lexer);
            if (at(lexer, '.')) advance(lexer);
            return produce(lexer, BEGINLESS_RANGE_OPERATOR);

        case 'e':
            return scan_keyword(lexer, valid, "nsure", REGULAR_ENSURE_KEYWORD, MODIFIER_ENSURE_KEYWORD);
        case 'i':
            return scan_keyword(lexer, valid, "f", REGULAR_IF_KEYWORD, MODIFIER_IF_KEYWORD);
        case 'r':
            return scan_keyword(lexer, valid, "escue", REGULAR_RESCUE_KEYWORD, MODIFIER_RESCUE_KEYWORD);
        case 'u':
            return scan_keyword(lexer, valid, "nless", REGULAR_UNLESS_KEYWORD, MODIFIER_UNLESS_KEYWORD);

        case 'y':
            // A zero-width token before the `yield` that ends `with x`.
            if (!valid[END_OF_WITH_EXPRESSION]) return false;
            lexer->mark_end(lexer);
            advance(lexer);
            return read_rest(lexer, "ield") && produce(lexer, END_OF_WITH_EXPRESSION);
    }
    return false;
}

// ---- lifecycle and serialization ----------------------------------------------------

static void reset(Scanner *s) {
    s->leading_space = false;
    s->after_continuation = false;
    s->macro_in_comment = false;
    s->macro_statement_keyword_ok = true;
    array_clear(&s->literals);
    for (uint32_t i = 0; i < s->heredocs.size; i++) array_delete(&array_get(&s->heredocs, i)->word);
    array_clear(&s->heredocs);
}

void *tree_sitter_crystal_external_scanner_create(void) {
    Scanner *s = ts_calloc(1, sizeof(Scanner));
    array_init(&s->literals);
    array_init(&s->heredocs);
    reset(s);
    return s;
}

void tree_sitter_crystal_external_scanner_destroy(void *payload) {
    Scanner *s = (Scanner *)payload;
    reset(s);
    array_delete(&s->literals);
    array_delete(&s->heredocs);
    ts_free(s);
}

bool tree_sitter_crystal_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    return scan((Scanner *)payload, lexer, valid_symbols);
}

// Layout: flags, literal count, the literals (4 bytes each), heredoc
// count, then each heredoc: flags, identifier length, the identifier's
// codepoints (3 bytes each). At most 4 + 16*4 + 1 + 16*2 + 160*3 = 581
// bytes.
unsigned tree_sitter_crystal_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = (Scanner *)payload;
    unsigned n = 0;
    // leading_space is only meaningful during a scan, but it is part of the
    // state tree-sitter compares when deciding whether two parse stacks
    // can merge, so it is kept: error recovery then takes the same paths
    // as with the grammar the node API comes from.
    buffer[n++] = (char)(s->after_continuation | s->macro_in_comment << 1 | s->macro_statement_keyword_ok << 2 |
                         s->leading_space << 3);
    buffer[n++] = (char)s->literals.size;
    for (uint32_t i = 0; i < s->literals.size; i++) {
        Literal *literal = array_get(&s->literals, i);
        buffer[n++] = (char)literal->open;
        buffer[n++] = (char)literal->close;
        buffer[n++] = (char)literal->depth;
        buffer[n++] = (char)literal->kind;
    }
    buffer[n++] = (char)s->heredocs.size;
    for (uint32_t i = 0; i < s->heredocs.size; i++) {
        Heredoc *heredoc = array_get(&s->heredocs, i);
        buffer[n++] = (char)(heredoc->interpolates | heredoc->started << 1);
        buffer[n++] = (char)heredoc->word.size;
        for (uint32_t j = 0; j < heredoc->word.size; j++) {
            uint32_t c = (uint32_t)*array_get(&heredoc->word, j);
            buffer[n++] = (char)(c >> 16);
            buffer[n++] = (char)(c >> 8);
            buffer[n++] = (char)c;
        }
    }
    return n;
}

void tree_sitter_crystal_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    reset(s);
    if (length == 0) return;
    const uint8_t *b = (const uint8_t *)buffer;
    unsigned n = 0;
    s->after_continuation = b[n] & 1;
    s->macro_in_comment = (b[n] >> 1) & 1;
    s->macro_statement_keyword_ok = (b[n] >> 2) & 1;
    s->leading_space = (b[n] >> 3) & 1;
    n++;
    uint8_t literals = b[n++];
    for (uint8_t i = 0; i < literals; i++) {
        Literal literal = {.open = b[n], .close = b[n + 1], .depth = b[n + 2], .kind = b[n + 3]};
        n += 4;
        array_push(&s->literals, literal);
    }
    uint8_t heredocs = b[n++];
    for (uint8_t i = 0; i < heredocs; i++) {
        Heredoc heredoc = {.interpolates = b[n] & 1, .started = (b[n] >> 1) & 1, .word = array_new()};
        uint8_t size = b[n + 1];
        n += 2;
        for (uint8_t j = 0; j < size; j++, n += 3) {
            array_push(&heredoc.word, (int32_t)((uint32_t)b[n] << 16 | (uint32_t)b[n + 1] << 8 | b[n + 2]));
        }
        array_push(&s->heredocs, heredoc);
    }
}
