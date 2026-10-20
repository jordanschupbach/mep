// The external scanner for mep's C++ grammar: raw string literals
// ([lex.string]), whose closing delimiter must repeat the opening one --
// `R"x(...)x"` -- which no regular token can express.
//
//   raw_string_delimiter  after `R"`, the d-char-sequence before `(`: up to
//                         16 characters, none of them a space, `(`, `)`,
//                         `\` or a control character. After the content's
//                         `)`, the same characters again.
//   raw_string_content    everything up to `)delimiter"`.
//
// The open string's delimiter is the state, saved with every token so an
// incremental reparse resumes inside a raw string exactly.

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <string.h>

enum TokenType {
    RAW_STRING_DELIMITER,
    RAW_STRING_CONTENT,
};

// [lex.string]: "The maximum length of a d-char-sequence shall be 16."
#define MAX_DELIMITER 16

typedef struct {
    unsigned char length;
    char chars[MAX_DELIMITER];
} Scanner;

void *tree_sitter_cpp_external_scanner_create(void) {
    Scanner *scanner = (Scanner *)ts_calloc(1, sizeof(Scanner));
    return scanner;
}

void tree_sitter_cpp_external_scanner_destroy(void *payload) { ts_free(payload); }

unsigned tree_sitter_cpp_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    buffer[0] = (char)scanner->length;
    memcpy(buffer + 1, scanner->chars, scanner->length);
    return 1 + scanner->length;
}

void tree_sitter_cpp_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->length = 0;
    if (length == 0) return;
    unsigned n = (unsigned char)buffer[0];
    if (n > MAX_DELIMITER || n + 1 > length) return;
    scanner->length = (unsigned char)n;
    memcpy(scanner->chars, buffer + 1, n);
}

static bool is_d_char(int32_t c) {
    return c > ' ' && c < 0x7f && c != '(' && c != ')' && c != '\\';
}

// The opening delimiter: at least one d-char, then `(` (not consumed).
static bool scan_opening_delimiter(Scanner *scanner, TSLexer *lexer) {
    scanner->length = 0;
    while (is_d_char(lexer->lookahead)) {
        if (scanner->length == MAX_DELIMITER) return false;
        scanner->chars[scanner->length++] = (char)lexer->lookahead;
        lexer->advance(lexer, false);
    }
    if (scanner->length == 0 || lexer->lookahead != '(') {
        scanner->length = 0;
        return false;
    }
    lexer->result_symbol = RAW_STRING_DELIMITER;
    return true;
}

// The closing delimiter, after the content's `)`: the opening one again.
static bool scan_closing_delimiter(Scanner *scanner, TSLexer *lexer) {
    for (unsigned i = 0; i < scanner->length; i++) {
        if (lexer->lookahead != scanner->chars[i]) return false;
        lexer->advance(lexer, false);
    }
    scanner->length = 0;
    lexer->result_symbol = RAW_STRING_DELIMITER;
    return true;
}

// Everything up to `)delimiter"`, which is left for the grammar and the
// delimiter token. May be empty.
static bool scan_content(Scanner *scanner, TSLexer *lexer) {
    lexer->result_symbol = RAW_STRING_CONTENT;
    for (;;) {
        if (lexer->eof(lexer)) return false;
        if (lexer->lookahead == ')') {
            lexer->mark_end(lexer);
            lexer->advance(lexer, false);
            unsigned i = 0;
            while (i < scanner->length && lexer->lookahead == scanner->chars[i]) {
                lexer->advance(lexer, false);
                i++;
            }
            if (i == scanner->length && lexer->lookahead == '"') return true;
            // Not the end: carry on from here, which may itself be a `)`.
            continue;
        }
        lexer->advance(lexer, false);
    }
}

bool tree_sitter_cpp_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    // Both valid is error recovery, where neither can be told apart from
    // ordinary code.
    if (valid_symbols[RAW_STRING_DELIMITER] && valid_symbols[RAW_STRING_CONTENT]) return false;
    if (valid_symbols[RAW_STRING_DELIMITER]) {
        // The opening delimiter is read right after `R"`, the closing one
        // after `)`: an open string's delimiter says which this is.
        return scanner->length == 0 ? scan_opening_delimiter(scanner, lexer)
                                    : scan_closing_delimiter(scanner, lexer);
    }
    if (valid_symbols[RAW_STRING_CONTENT]) return scan_content(scanner, lexer);
    return false;
}
