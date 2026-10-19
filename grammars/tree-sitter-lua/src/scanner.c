// External scanner for the Lua grammar (grammar.js): long brackets.
//
// `[[ ... ]]`, `[==[ ... ]==]` strings and `--[[ ... ]]` comments close
// only at a `]`, as many `=`, and `]` -- the same level they opened with --
// so the level is kept as state between the open, content and close tokens
// (and serialized, so an incremental reparse resumes inside one correctly).
// The content is everything between the brackets, a newline right after
// the opening one included, and may be empty.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum TokenType {
    LINE_COMMENT_CONTENT,
    BLOCK_COMMENT_START,
    BLOCK_COMMENT_CONTENT,
    BLOCK_COMMENT_END,
    BLOCK_STRING_START,
    BLOCK_STRING_CONTENT,
    BLOCK_STRING_END,
    ERROR_SENTINEL,
};

typedef struct {
    // The open long bracket's level (its count of `=`); -1 outside one.
    int32_t level;
} Scanner;

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// `[`, `=`*level, `[`; records the level. The lexer is at the first `[`.
static bool scan_open(Scanner *s, TSLexer *lexer) {
    if (lexer->lookahead != '[') return false;
    advance(lexer);
    int32_t level = 0;
    while (lexer->lookahead == '=') {
        level++;
        advance(lexer);
    }
    if (lexer->lookahead != '[') return false;
    advance(lexer);
    lexer->mark_end(lexer);
    s->level = level;
    return true;
}

// `]`, `=`*level, `]`: consumes it and returns true if that is what is
// next; otherwise consumes some of it and returns false.
static bool consume_close(Scanner *s, TSLexer *lexer) {
    if (lexer->lookahead != ']') return false;
    advance(lexer);
    for (int32_t i = 0; i < s->level; i++) {
        if (lexer->lookahead != '=') return false;
        advance(lexer);
    }
    if (lexer->lookahead != ']') return false;
    advance(lexer);
    return true;
}

// Everything up to the closing bracket (or the end of the file).
static bool scan_content(TSLexer *lexer, Scanner *s, enum TokenType token) {
    for (;;) {
        lexer->mark_end(lexer);
        if (lexer->eof(lexer)) break;
        if (lexer->lookahead == ']') {
            if (consume_close(s, lexer)) break;
            continue;  // what was read is content after all
        }
        advance(lexer);
    }
    lexer->result_symbol = token;
    return true;
}

static bool scan_close(Scanner *s, TSLexer *lexer, enum TokenType token) {
    if (!consume_close(s, lexer)) return false;
    lexer->mark_end(lexer);
    s->level = -1;
    lexer->result_symbol = token;
    return true;
}

void *tree_sitter_lua_external_scanner_create(void) {
    Scanner *s = (Scanner *)calloc(1, sizeof(Scanner));
    if (s) s->level = -1;
    return s;
}

void tree_sitter_lua_external_scanner_destroy(void *payload) { free(payload); }

unsigned tree_sitter_lua_external_scanner_serialize(void *payload, char *buffer) {
    const Scanner *s = (const Scanner *)payload;
    buffer[0] = (char)(s->level & 0xff);
    buffer[1] = (char)((s->level >> 8) & 0xff);
    buffer[2] = (char)((s->level >> 16) & 0xff);
    buffer[3] = (char)((s->level >> 24) & 0xff);
    return 4;
}

void tree_sitter_lua_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    s->level = -1;
    if (length != 4) return;
    s->level = (int32_t)((uint32_t)(uint8_t)buffer[0] | (uint32_t)(uint8_t)buffer[1] << 8 |
                         (uint32_t)(uint8_t)buffer[2] << 16 | (uint32_t)(uint8_t)buffer[3] << 24);
}

bool tree_sitter_lua_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = (Scanner *)payload;
    if (valid_symbols[ERROR_SENTINEL]) return false;

    if (s->level >= 0) {
        if (valid_symbols[BLOCK_STRING_CONTENT]) return scan_content(lexer, s, BLOCK_STRING_CONTENT);
        if (valid_symbols[BLOCK_COMMENT_CONTENT]) return scan_content(lexer, s, BLOCK_COMMENT_CONTENT);
        if (valid_symbols[BLOCK_STRING_END]) return scan_close(s, lexer, BLOCK_STRING_END);
        if (valid_symbols[BLOCK_COMMENT_END]) return scan_close(s, lexer, BLOCK_COMMENT_END);
        return false;
    }

    // Right after `--`, with no whitespace skipped (`-- [[` is a line
    // comment): a long bracket, or else the rest of the line -- which may
    // be empty, and takes in whatever a failed long bracket read.
    if (valid_symbols[BLOCK_COMMENT_START] && valid_symbols[LINE_COMMENT_CONTENT]) {
        if (lexer->lookahead == '[' && scan_open(s, lexer)) {
            lexer->result_symbol = BLOCK_COMMENT_START;
            return true;
        }
        while (lexer->lookahead != '\n' && lexer->lookahead != '\r' && !lexer->eof(lexer)) advance(lexer);
        lexer->mark_end(lexer);
        lexer->result_symbol = LINE_COMMENT_CONTENT;
        return true;
    }
    if (valid_symbols[BLOCK_STRING_START]) {
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\r' ||
               lexer->lookahead == '\n' || lexer->lookahead == '\f' || lexer->lookahead == '\v') {
            skip(lexer);
        }
        if (!scan_open(s, lexer)) return false;
        lexer->result_symbol = BLOCK_STRING_START;
        return true;
    }
    return false;
}
