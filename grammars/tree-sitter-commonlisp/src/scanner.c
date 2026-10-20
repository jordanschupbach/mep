// External scanner for the Common Lisp grammar (grammar.js): nestable
// block comments and the text of strings.
//
// Stateless. `#| ... |#` nests (CLHS 2.4.8.19), so the scanner counts
// depth while it reads one, but a comment is always read whole within a
// single call: nothing carries over between tokens. A string's text is a
// run of anything but `"`, `\` and `~` -- the closing quote, an escape and
// a FORMAT directive are the grammar's own tokens (but for a `~` just
// before the closing quote or an escape, which is text). Reading it here is what
// keeps `"#|"` a string: the block comment is an extra, valid everywhere,
// so the scanner has to know it is inside a string to leave it alone, and
// the string's text being a valid token is how it knows.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
    BLOCK_COMMENT,
    STRING_CONTENT,
    ERROR_SENTINEL,
};

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool is_space(int32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static bool scan_string_content(TSLexer *lexer) {
    bool has_content = false;
    lexer->result_symbol = STRING_CONTENT;
    for (;;) {
        lexer->mark_end(lexer);
        if (lexer->eof(lexer) || lexer->lookahead == '"' || lexer->lookahead == '\\') return has_content;
        if (lexer->lookahead == '~') {
            // A `~` right before the closing quote -- "~", a home
            // directory -- or before an escape, as in "\"~\"", directs
            // nothing; it is text. Anywhere else the directive is the
            // grammar's token, so the text stops here.
            advance(lexer);
            if (!lexer->eof(lexer) && lexer->lookahead != '"' && lexer->lookahead != '\\') return has_content;
        } else {
            advance(lexer);
        }
        has_content = true;
    }
}

// After the opening `#|`. An unterminated comment runs to the end of the
// file, as it does for the reader -- which is an error there, but here
// it is the one reading that keeps the rest of the file from being
// misparsed as code while the closing `|#` is still being typed.
static bool scan_block_comment(TSLexer *lexer) {
    unsigned depth = 1;
    while (depth > 0) {
        if (lexer->eof(lexer)) break;
        if (lexer->lookahead == '|') {
            advance(lexer);
            if (lexer->lookahead == '#') {
                advance(lexer);
                depth--;
            }
        } else if (lexer->lookahead == '#') {
            advance(lexer);
            if (lexer->lookahead == '|') {
                advance(lexer);
                depth++;
            }
        } else {
            advance(lexer);
        }
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = BLOCK_COMMENT;
    return true;
}

void *tree_sitter_commonlisp_external_scanner_create(void) { return NULL; }

void tree_sitter_commonlisp_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_commonlisp_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_commonlisp_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

bool tree_sitter_commonlisp_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    (void)payload;
    const bool recovering = valid_symbols[ERROR_SENTINEL];
    if (valid_symbols[STRING_CONTENT] && !recovering) return scan_string_content(lexer);
    if (!valid_symbols[BLOCK_COMMENT]) return false;
    while (is_space(lexer->lookahead)) skip(lexer);
    if (lexer->lookahead != '#') return false;
    advance(lexer);
    if (lexer->lookahead != '|') return false;
    advance(lexer);
    return scan_block_comment(lexer);
}
