// External scanner for the R grammar (grammar.js): significant newlines
// and raw strings.
//
// Newlines. R ends an expression at a newline only where one may end, at
// the top level and in a `{...}` body; inside `(...)` and `[...]` a
// newline is whitespace. The grammar asks for a newline token exactly where
// an expression may end, so the scanner gives one whenever it is asked and
// leaves the newline to `extras` otherwise. One exception: a newline before
// `else` never ends the `if` (R allows that inside braces; at the top level
// it is an error R reports, and not ending the `if` reads it the way it was
// meant).
//
// Raw strings. `r"(...)"`, `R'[...]'`, `r"--{...}--"`: the content runs to
// the matching close bracket, dashes and quote, which the scanner keeps as
// state between the open, content and close tokens.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum TokenType {
    NEWLINE,
    RAW_STRING_OPEN,
    RAW_STRING_CONTENT,
    RAW_STRING_CLOSE,
    PROGRAM_START,
    ERROR_SENTINEL,
};

typedef struct {
    // The open raw string's closing delimiter: bracket, dashes, quote.
    // `quote` is 0 outside a raw string.
    char quote;
    char close_bracket;
    uint8_t dashes;
} Scanner;

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool is_identifier_char(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c > 127;
}

// After a line break: does the next token start with `else`? Looks past
// blank lines and comment lines; the lexer's position is reset by the
// caller's mark_end either way.
static bool else_follows(TSLexer *lexer) {
    for (;;) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
               lexer->lookahead == '\n' || lexer->lookahead == '\f') {
            advance(lexer);
        }
        if (lexer->lookahead != '#') break;
        while (lexer->lookahead != '\n' && !lexer->eof(lexer)) advance(lexer);
    }
    const char *word = "else";
    for (int i = 0; word[i]; i++) {
        if (lexer->lookahead != word[i]) return false;
        advance(lexer);
    }
    return !is_identifier_char(lexer->lookahead);
}

// 1: a newline token; 0: no newline here; -1: a newline before `else`,
// which is whitespace -- and the lexer has moved past it, so the caller
// must not try another token from here.
static int scan_newline(TSLexer *lexer) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
           lexer->lookahead == '\f') {
        skip(lexer);
    }
    if (lexer->lookahead != '\n') return 0;
    advance(lexer);
    lexer->mark_end(lexer);
    if (else_follows(lexer)) return -1;
    lexer->result_symbol = NEWLINE;
    return 1;
}

static bool scan_raw_string_open(Scanner *s, TSLexer *lexer) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
           lexer->lookahead == '\n' || lexer->lookahead == '\f') {
        skip(lexer);
    }
    if (lexer->lookahead != 'r' && lexer->lookahead != 'R') return false;
    advance(lexer);
    if (lexer->lookahead != '"' && lexer->lookahead != '\'') return false;
    const char quote = (char)lexer->lookahead;
    advance(lexer);
    uint8_t dashes = 0;
    while (lexer->lookahead == '-') {
        if (dashes == UINT8_MAX) return false;
        dashes++;
        advance(lexer);
    }
    char close_bracket;
    switch (lexer->lookahead) {
    case '(': close_bracket = ')'; break;
    case '[': close_bracket = ']'; break;
    case '{': close_bracket = '}'; break;
    default: return false;
    }
    advance(lexer);
    lexer->mark_end(lexer);
    s->quote = quote;
    s->close_bracket = close_bracket;
    s->dashes = dashes;
    lexer->result_symbol = RAW_STRING_OPEN;
    return true;
}

// Consumes the closing delimiter if it is next; true if it was.
static bool consume_raw_close(Scanner *s, TSLexer *lexer) {
    if (lexer->lookahead != s->close_bracket) return false;
    advance(lexer);
    for (uint8_t i = 0; i < s->dashes; i++) {
        if (lexer->lookahead != '-') return false;
        advance(lexer);
    }
    if (lexer->lookahead != s->quote) return false;
    advance(lexer);
    return true;
}

// The content up to the closing delimiter; or, for an empty raw string,
// the closing delimiter itself (it has been read by then).
static bool scan_raw_string_content(Scanner *s, TSLexer *lexer, const bool *valid_symbols) {
    bool has_content = false;
    for (;;) {
        lexer->mark_end(lexer);
        if (lexer->eof(lexer)) break;
        if (lexer->lookahead != s->close_bracket) {
            advance(lexer);
            has_content = true;
            continue;
        }
        if (consume_raw_close(s, lexer)) {
            if (has_content) break;
            if (!valid_symbols[RAW_STRING_CLOSE]) return false;
            lexer->mark_end(lexer);
            s->quote = 0;
            lexer->result_symbol = RAW_STRING_CLOSE;
            return true;
        }
        // Not the close after all: what was read is content.
        has_content = true;
    }
    lexer->result_symbol = RAW_STRING_CONTENT;
    return has_content;
}

static bool scan_raw_string_close(Scanner *s, TSLexer *lexer) {
    if (!consume_raw_close(s, lexer)) return false;
    lexer->mark_end(lexer);
    s->quote = 0;
    lexer->result_symbol = RAW_STRING_CLOSE;
    return true;
}

void *tree_sitter_r_external_scanner_create(void) {
    Scanner *s = (Scanner *)calloc(1, sizeof(Scanner));
    return s;
}

void tree_sitter_r_external_scanner_destroy(void *payload) { free(payload); }

unsigned tree_sitter_r_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = (Scanner *)payload;
    buffer[0] = s->quote;
    buffer[1] = s->close_bracket;
    buffer[2] = (char)s->dashes;
    return 3;
}

void tree_sitter_r_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    memset(s, 0, sizeof(Scanner));
    if (length != 3) return;
    s->quote = buffer[0];
    s->close_bracket = buffer[1];
    s->dashes = (uint8_t)buffer[2];
}

bool tree_sitter_r_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = (Scanner *)payload;
    if (valid_symbols[ERROR_SENTINEL]) return false;

    // Valid only in the parser's first state, at byte 0.
    if (valid_symbols[PROGRAM_START]) {
        lexer->result_symbol = PROGRAM_START;
        return true;
    }
    if (s->quote) {
        if (valid_symbols[RAW_STRING_CONTENT]) return scan_raw_string_content(s, lexer, valid_symbols);
        if (valid_symbols[RAW_STRING_CLOSE]) return scan_raw_string_close(s, lexer);
        return false;
    }
    if (valid_symbols[NEWLINE]) {
        const int newline = scan_newline(lexer);
        if (newline != 0) return newline > 0;
    }
    if (valid_symbols[RAW_STRING_OPEN]) return scan_raw_string_open(s, lexer);
    return false;
}
