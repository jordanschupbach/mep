// External scanner for the Nix grammar (grammar.js): string contents and
// paths, the two places where Nix's lexer (src/libexpr/lexer.l) depends on
// more than one token of context. Stateless -- every decision is made from
// the characters at the current position.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>

enum TokenType {
    STRING_FRAGMENT,
    INDENTED_STRING_FRAGMENT,
    PATH_START,
    HPATH_START,
    PATH_FRAGMENT,
    ERROR_SENTINEL,
};

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// lexer.l's PATH_CHAR.
static bool is_path_char(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-' || c == '+';
}

// The text of a "..." string up to the next `"`, `\` or `${`. A `$` not
// followed by `{` is text, and so is `$$`: lexer.l reads `$${` as a literal
// `$` and then `${` as text, not as an interpolation.
static bool scan_string_fragment(TSLexer *lexer) {
    bool has_content = false;
    lexer->result_symbol = STRING_FRAGMENT;
    for (;;) {
        lexer->mark_end(lexer);
        switch (lexer->lookahead) {
        case '"':
        case '\\':
            return has_content;
        case '$':
            advance(lexer);
            if (lexer->lookahead == '{') return has_content;
            if (lexer->lookahead == '$') advance(lexer);
            has_content = true;
            break;
        case 0:
            if (lexer->eof(lexer)) return has_content;
            advance(lexer);
            has_content = true;
            break;
        default:
            advance(lexer);
            has_content = true;
        }
    }
}

// The text of a ''...'' string up to the next `''` (its end, or one of
// the `''$` / `'''` / `''\x` escapes, which the grammar lexes) or `${`.
static bool scan_indented_string_fragment(TSLexer *lexer) {
    bool has_content = false;
    lexer->result_symbol = INDENTED_STRING_FRAGMENT;
    for (;;) {
        lexer->mark_end(lexer);
        switch (lexer->lookahead) {
        case '\'':
            advance(lexer);
            if (lexer->lookahead == '\'') return has_content;
            has_content = true;
            break;
        case '$':
            advance(lexer);
            if (lexer->lookahead == '{') return has_content;
            if (lexer->lookahead == '$') advance(lexer);
            has_content = true;
            break;
        case 0:
            if (lexer->eof(lexer)) return has_content;
            advance(lexer);
            has_content = true;
            break;
        default:
            advance(lexer);
            has_content = true;
        }
    }
}

// The `/seg/seg` part of a path, after its prefix (lexer.l's PATH and
// PATH_SEG): one or more `/` + PATH_CHAR+ segments, or a `/` directly
// before `${`. Returns whether that made a path; the token ends after the
// last complete segment (or the `/` before `${`).
static bool scan_path_segments(TSLexer *lexer) {
    bool is_path = false;
    while (lexer->lookahead == '/') {
        advance(lexer);
        if (is_path_char(lexer->lookahead)) {
            while (is_path_char(lexer->lookahead)) advance(lexer);
            lexer->mark_end(lexer);
            is_path = true;
            continue;
        }
        if (lexer->lookahead == '$') {
            lexer->mark_end(lexer);
            advance(lexer);
            return lexer->lookahead == '{';
        }
        break;
    }
    return is_path;
}

// The start of a path expression, wherever an expression can start: `./a`,
// `../a`, `/a`, `a/b` (a bare `a/b` is a path in Nix, not a division), or
// `~/a`. Anything else -- an identifier, a number, `//`, `/*` -- is left to
// the grammar's own lexer.
static bool scan_path_start(TSLexer *lexer, const bool *valid_symbols) {
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\n' ||
           lexer->lookahead == '\r' || lexer->lookahead == '\f') {
        skip(lexer);
    }
    if (lexer->lookahead == '~') {
        if (!valid_symbols[HPATH_START]) return false;
        advance(lexer);
        if (lexer->lookahead != '/') return false;
        lexer->result_symbol = HPATH_START;
        return scan_path_segments(lexer);
    }
    if (!valid_symbols[PATH_START]) return false;
    while (is_path_char(lexer->lookahead)) advance(lexer);
    lexer->result_symbol = PATH_START;
    return scan_path_segments(lexer);
}

// More of a path after an interpolation in it: `.nix` in `./${name}.nix`.
static bool scan_path_fragment(TSLexer *lexer) {
    bool has_content = false;
    while (is_path_char(lexer->lookahead) || lexer->lookahead == '/') {
        advance(lexer);
        has_content = true;
    }
    lexer->result_symbol = PATH_FRAGMENT;
    return has_content;
}

void *tree_sitter_nix_external_scanner_create(void) { return NULL; }

void tree_sitter_nix_external_scanner_destroy(void *payload) { (void)payload; }

unsigned tree_sitter_nix_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}

void tree_sitter_nix_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

bool tree_sitter_nix_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    (void)payload;
    if (valid_symbols[ERROR_SENTINEL]) return false;
    if (valid_symbols[STRING_FRAGMENT]) return scan_string_fragment(lexer);
    if (valid_symbols[INDENTED_STRING_FRAGMENT]) return scan_indented_string_fragment(lexer);
    if (valid_symbols[PATH_FRAGMENT] && (is_path_char(lexer->lookahead) || lexer->lookahead == '/')) {
        return scan_path_fragment(lexer);
    }
    if (valid_symbols[PATH_START] || valid_symbols[HPATH_START]) return scan_path_start(lexer, valid_symbols);
    return false;
}
