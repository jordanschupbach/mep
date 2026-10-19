// External scanner for the justfile grammar (grammar.js): newlines, the
// indent and dedent around a recipe's body, and the body's lines.
//
// Stateless. just ends a recipe body at the first line with content that
// is not indented, so whether a line belongs to a body is decided from
// the line itself (looking past blank lines), and lines indented more
// deeply (a script's own blocks) are still just body text. Where the
// grammar is not asked for a newline -- inside `(...)`, `[...]` or
// `{...}` -- the scanner declines it and the newline is whitespace.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
    NEWLINE,
    INDENT,
    LINE_INDENT,
    DEDENT,
    LINE_PREFIX,
    TEXT,
    ERROR_SENTINEL,
};

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool is_line_end(TSLexer *lexer) {
    return lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->eof(lexer);
}

// A body line's text up to its end or the next `{{`. `{{{{` is just's
// escape for a literal `{{`, so it is text, and a `\` at the end of a line
// continues it: just joins the two lines before the shell sees them, so
// the next line's leading `-` or `@` is text, not a prefix.
static bool scan_text(TSLexer *lexer) {
    bool has_content = false;
    lexer->result_symbol = TEXT;
    for (;;) {
        lexer->mark_end(lexer);
        if (is_line_end(lexer)) return has_content;
        if (lexer->lookahead == '{') {
            advance(lexer);
            if (lexer->lookahead != '{') {
                has_content = true;
                continue;
            }
            advance(lexer);
            if (lexer->lookahead != '{') return has_content;
            advance(lexer);
            if (lexer->lookahead != '{') return has_content;
            advance(lexer);
            has_content = true;
            continue;
        }
        if (lexer->lookahead == '\\') {
            advance(lexer);
            if (lexer->lookahead == '\r') advance(lexer);
            if (lexer->lookahead == '\n') advance(lexer);
            has_content = true;
            continue;
        }
        advance(lexer);
        has_content = true;
    }
}

// At the start of a line, where a recipe's body may begin or go on. The
// next line with content decides: indented, it is a body line, and the
// token covers the blank lines before it and its indentation; otherwise
// the body ends, with a zero-width DEDENT at the start of the first line
// after it, so blank lines after a body are not part of it (or its fold).
static bool scan_line_start(TSLexer *lexer, const bool *valid_symbols) {
    lexer->mark_end(lexer);
    bool indented = false;
    for (;;) {
        indented = false;
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
            advance(lexer);
            indented = true;
        }
        if (lexer->lookahead == '\r') advance(lexer);
        if (lexer->lookahead != '\n') break;
        advance(lexer);
    }
    if (!indented || lexer->eof(lexer)) {
        if (!valid_symbols[DEDENT]) return false;
        lexer->result_symbol = DEDENT;
        return true;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = valid_symbols[INDENT] ? INDENT : LINE_INDENT;
    return true;
}

// `@` (don't echo) and `-` (ignore failure), in either order.
static bool scan_line_prefix(TSLexer *lexer) {
    const int32_t first = lexer->lookahead;
    advance(lexer);
    if ((lexer->lookahead == '@' || lexer->lookahead == '-') && lexer->lookahead != first) advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = LINE_PREFIX;
    return true;
}

void *tree_sitter_just_external_scanner_create(void) { return NULL; }

void tree_sitter_just_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_just_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_just_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

bool tree_sitter_just_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    (void)payload;
    if (valid_symbols[ERROR_SENTINEL]) return false;

    if (valid_symbols[DEDENT] && lexer->eof(lexer)) {
        lexer->result_symbol = DEDENT;
        return true;
    }
    if ((valid_symbols[INDENT] || valid_symbols[LINE_INDENT]) && lexer->get_column(lexer) == 0) {
        return scan_line_start(lexer, valid_symbols);
    }
    if (valid_symbols[LINE_PREFIX] && (lexer->lookahead == '@' || lexer->lookahead == '-')) {
        return scan_line_prefix(lexer);
    }
    if (valid_symbols[TEXT] && !is_line_end(lexer)) return scan_text(lexer);

    if (valid_symbols[NEWLINE]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') skip(lexer);
        if (!is_line_end(lexer)) return false;
        // One line break; a blank line after it is whitespace, or part of
        // the next body line's LINE_INDENT.
        if (lexer->lookahead == '\r') advance(lexer);
        if (lexer->lookahead == '\n') advance(lexer);
        lexer->result_symbol = NEWLINE;
        return true;
    }
    return false;
}
