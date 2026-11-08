// External scanner for the Maxima grammar (grammar.js): comments, which
// nest -- Maxima's own `gobble-comment` counts depth, so `/* a /* b */ c */`
// is one comment, and a regular expression cannot count. Stateless.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType { COMMENT };

void *tree_sitter_maxima_external_scanner_create(void) { return NULL; }
void tree_sitter_maxima_external_scanner_destroy(void *payload) { (void)payload; }
unsigned tree_sitter_maxima_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}
void tree_sitter_maxima_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

bool tree_sitter_maxima_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    (void)payload;
    if (!valid_symbols[COMMENT]) return false;
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\n' ||
           lexer->lookahead == '\r' || lexer->lookahead == '\f' || lexer->lookahead == '\v') {
        lexer->advance(lexer, true);
    }
    if (lexer->lookahead != '/') return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != '*') return false;
    lexer->advance(lexer, false);
    unsigned depth = 1;
    while (depth > 0) {
        if (lexer->eof(lexer)) break;  // unterminated: the comment runs to the end
        if (lexer->lookahead == '/') {
            lexer->advance(lexer, false);
            if (lexer->lookahead == '*') {
                lexer->advance(lexer, false);
                depth++;
            }
            continue;
        }
        if (lexer->lookahead == '*') {
            lexer->advance(lexer, false);
            if (lexer->lookahead == '/') {
                lexer->advance(lexer, false);
                depth--;
            }
            continue;
        }
        lexer->advance(lexer, false);
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = COMMENT;
    return true;
}
