// The external scanner of mep's COBOL grammar: the parts of COBOL's
// reference format a context-free grammar cannot see.
//
// Fixed format (GnuCOBOL's default): columns 1-6 are the sequence area,
// column 7 the indicator (`*` or `/` a comment line, `-` a continuation,
// `D` a debugging line), the code runs from column 8 to 72, and columns
// 73-80 are the identification area. Free format (`>>SOURCE FORMAT FREE`,
// or `cobc -free`): the whole line is code. `*>` starts a comment in
// either format.
//
// The format is part of the scanner's state. A file starts out fixed; a
// `>>SOURCE` / `$SET SOURCEFORMAT` directive switches it, and so does the
// first line with code in the sequence area that is not a sequence number
// (the same evidence mep's COBOL highlighter and language server use,
// src/cobol_syntax.cpp's CobolLineFormats), via the zero-width
// _format_switch token.
//
// Based on the scanner of yutaro-sakamoto/tree-sitter-cobol (MIT).

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    WHITE_SPACES,
    SEQUENCE_AREA,
    IDENTIFICATION_AREA,
    COMMENT,
    COMMENT_ENTRY,
    MULTILINE_STRING,
    COMPILER_DIRECTIVE,
    INDICATOR,
    FORMAT_SWITCH,
    ERROR_SENTINEL,
};

// What the format is and why: a directive's word is final, a guess from the text is not.
enum { FORMAT_FIXED = 0, FORMAT_FREE_GUESSED = 1, FORMAT_FREE = 2 };

typedef struct {
    uint8_t free_format;  // one of the FORMAT_ values
    // The current line has a tab before its code: a tab expands to column 8
    // in fixed format, so the line has no sequence area or indicator to read.
    bool tab_line;
} Scanner;

void *tree_sitter_cobol_external_scanner_create(void) {
    Scanner *s = (Scanner *)ts_calloc(1, sizeof(Scanner));
    return s;
}

void tree_sitter_cobol_external_scanner_destroy(void *payload) { ts_free(payload); }

unsigned tree_sitter_cobol_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = (Scanner *)payload;
    buffer[0] = (char)s->free_format;
    buffer[1] = (char)s->tab_line;
    return 2;
}

void tree_sitter_cobol_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    s->free_format = length > 0 ? (uint8_t)buffer[0] : FORMAT_FIXED;
    s->tab_line = length > 1 && buffer[1];
}

static bool is_white_space(int32_t c) { return iswspace((wint_t)c) || c == ';' || c == ','; }

static bool at_eol(TSLexer *lexer) { return lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->eof(lexer); }

static int32_t upper(int32_t c) { return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c; }

// The end of a fixed-format line's code area (column 73, 0-based 72); free format has none.
static uint32_t code_limit(const Scanner *s) { return s->free_format ? UINT32_MAX : 72; }

// The words that end a comment entry (AUTHOR. ... and the like): the next paragraph or division.
static const char *const kCommentEntryEnd[] = {
    "AUTHOR", "INSTALLATION", "DATE-WRITTEN", "DATE-COMPILED", "SECURITY", "REMARKS",
    "IDENTIFICATION DIVISION", "ENVIRONMENT DIVISION", "DATA DIVISION", "PROCEDURE DIVISION",
};
#define COMMENT_ENTRY_END_COUNT (sizeof(kCommentEntryEnd) / sizeof(kCommentEntryEnd[0]))

// Reads the start of the line (after its leading blanks) and says whether it is one of
// kCommentEntryEnd. Consumes what it reads, so the caller goes on from there.
static bool starts_with_entry_end(Scanner *s, TSLexer *lexer) {
    char text[32];
    size_t n = 0;
    while (!at_eol(lexer) && lexer->get_column(lexer) < code_limit(s) && n < sizeof(text) - 1) {
        const int32_t c = lexer->lookahead;
        if (c == ' ' || c == '\t') {
            if (n > 0 && text[n - 1] != ' ') text[n++] = ' ';
        } else if (iswalnum((wint_t)c) || c == '-') {
            text[n++] = (char)upper(c);
        } else {
            break;
        }
        lexer->advance(lexer, false);
    }
    while (n > 0 && text[n - 1] == ' ') n--;
    text[n] = 0;
    for (size_t i = 0; i < COMMENT_ENTRY_END_COUNT; i++) {
        const size_t k = strlen(kCommentEntryEnd[i]);
        if (strncmp(text, kCommentEntryEnd[i], k) == 0 && (text[k] == 0 || text[k] == ' ')) return true;
    }
    return false;
}

// A directive: `>>...` or `$SET ...` (and `$IF` and friends) up to the end of the line or a `*>`.
// `>>SOURCE [FORMAT] [IS] FREE|FIXED` and `$SET SOURCEFORMAT"FREE"` switch the format.
static bool scan_directive(Scanner *s, TSLexer *lexer) {
    char text[160];
    size_t n = 0;
    if (lexer->lookahead == '>') {
        lexer->advance(lexer, false);
        if (lexer->lookahead != '>') return false;
        lexer->advance(lexer, false);
    } else if (lexer->lookahead == '$') {
        lexer->advance(lexer, false);
        static const char *const kDollar[] = {"SET", "IF", "ELSE", "ELIF", "END", "DISPLAY"};
        char word[8];
        size_t w = 0;
        while (w < sizeof(word) - 1 && iswalpha((wint_t)lexer->lookahead)) {
            word[w++] = (char)upper(lexer->lookahead);
            lexer->advance(lexer, false);
        }
        word[w] = 0;
        bool known = false;
        for (size_t i = 0; i < sizeof(kDollar) / sizeof(kDollar[0]); i++) known = known || strcmp(word, kDollar[i]) == 0;
        if (!known || !(lexer->lookahead == ' ' || lexer->lookahead == '\t' || at_eol(lexer))) return false;
        memcpy(text, word, w);
        n = w;
    } else {
        return false;
    }
    lexer->mark_end(lexer);
    while (!at_eol(lexer)) {
        if (lexer->lookahead == '*') {
            lexer->advance(lexer, false);
            if (lexer->lookahead == '>') break;  // a comment follows; the directive ends before it
            if (n < sizeof(text) - 1) text[n++] = '*';
            lexer->mark_end(lexer);
            continue;
        }
        if (n < sizeof(text) - 1) text[n++] = (char)upper(lexer->lookahead);
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
    }
    text[n] = 0;
    const char *source = strstr(text, "SOURCE");
    if (source) {
        if (strstr(source, "FREE")) s->free_format = FORMAT_FREE;
        else if (strstr(source, "FIXED")) s->free_format = FORMAT_FIXED;
    }
    lexer->result_symbol = COMPILER_DIRECTIVE;
    return true;
}

// A fixed-format literal, possibly continued: `"abc` up to column 72, then on the next line a `-`
// indicator and the rest from its next quote.
static bool scan_multiline_string(Scanner *s, TSLexer *lexer) {
    for (;;) {
        if (lexer->lookahead != '"' && lexer->lookahead != '\'') return false;
        const int32_t quote = lexer->lookahead;
        lexer->advance(lexer, false);
        while (lexer->lookahead != quote && !at_eol(lexer) && lexer->get_column(lexer) < 72) lexer->advance(lexer, false);
        if (lexer->lookahead == quote) {
            lexer->advance(lexer, false);
            if (lexer->lookahead == quote) continue;  // a doubled quote: the literal goes on
            lexer->result_symbol = MULTILINE_STRING;
            lexer->mark_end(lexer);
            return true;
        }
        // Up to column 72 without a closing quote: the literal goes on in the next line's
        // continuation, from its first quote.
        while (!lexer->eof(lexer) && lexer->lookahead != '\n') lexer->advance(lexer, false);
        if (lexer->eof(lexer)) return false;
        lexer->advance(lexer, false);
        for (int i = 0; i <= 5; i++) {
            if (at_eol(lexer)) return false;
            lexer->advance(lexer, false);
        }
        if (lexer->lookahead != '-') return false;
        lexer->advance(lexer, false);
        while (lexer->lookahead == ' ' && lexer->get_column(lexer) < 72) lexer->advance(lexer, false);
        if (lexer->lookahead != quote) return false;
        // The continuation's opening quote is not part of the value, but the token runs on.
    }
}

bool tree_sitter_cobol_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = (Scanner *)payload;
    if (lexer->eof(lexer)) return false;
    const bool error_mode = valid_symbols[ERROR_SENTINEL];

    if (valid_symbols[WHITE_SPACES] && is_white_space(lexer->lookahead)) {
        while (is_white_space(lexer->lookahead)) {
            if (lexer->lookahead == '\n') {
                s->tab_line = false;
            } else if (lexer->lookahead == '\t' && lexer->get_column(lexer) < 7) {
                s->tab_line = true;
            }
            lexer->advance(lexer, true);
        }
        lexer->result_symbol = WHITE_SPACES;
        lexer->mark_end(lexer);
        return true;
    }

    const uint32_t column = lexer->get_column(lexer);
    if (column == 0) s->tab_line = false;
    const bool fixed = !s->free_format && !s->tab_line;

    // A guess of free format gives way to a line that is plainly fixed: a six-digit sequence
    // number, then an indicator (a fixed-format file with a word in one line's sequence area).
    if (s->free_format == FORMAT_FREE_GUESSED && column == 0 && iswdigit((wint_t)lexer->lookahead) &&
        valid_symbols[SEQUENCE_AREA]) {
        int digits = 0;
        while (digits < 6 && iswdigit((wint_t)lexer->lookahead)) {
            lexer->advance(lexer, false);
            digits++;
        }
        const int32_t c = lexer->lookahead;
        if (digits == 6 && (c == ' ' || c == '*' || c == '/' || c == '-' || c == 'D' || c == 'd' || at_eol(lexer))) {
            s->free_format = FORMAT_FIXED;
            lexer->mark_end(lexer);
            lexer->result_symbol = SEQUENCE_AREA;
            return true;
        }
        return false;
    }

    // The sequence area: a sequence number (digits), nothing, or six characters of anything
    // followed by an indicator (GnuCOBOL ignores what is there). Anything else there is code, so
    // the file is in free format.
    const bool directive_start = lexer->lookahead == '>' || lexer->lookahead == '$';
    if (fixed && column <= 5 && !directive_start) {
        lexer->mark_end(lexer);
        bool sequence = true;
        bool blank = false;  // a blank in the area, which a word filling all six columns has not
        while (!at_eol(lexer) && lexer->get_column(lexer) <= 5) {
            if (!iswdigit((wint_t)lexer->lookahead) && lexer->lookahead != ' ') sequence = false;
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') blank = true;
            lexer->advance(lexer, false);
        }
        if (!sequence && column == 0 && !blank && lexer->get_column(lexer) == 6) {
            const int32_t c = lexer->lookahead;
            sequence = c == ' ' || c == '*' || c == '/' || c == '-' || c == 'D' || c == 'd' || at_eol(lexer);
        }
        if (sequence && valid_symbols[SEQUENCE_AREA]) {
            lexer->mark_end(lexer);
            lexer->result_symbol = SEQUENCE_AREA;
            return true;
        }
        if (!sequence && valid_symbols[FORMAT_SWITCH]) {
            s->free_format = FORMAT_FREE_GUESSED;
            lexer->result_symbol = FORMAT_SWITCH;  // zero-width: the line is lexed again as free format
            return true;
        }
        return false;
    }

    // The indicator column.
    if (fixed && column == 6) {
        const int32_t c = lexer->lookahead;
        if ((c == '*' || c == '/') && valid_symbols[COMMENT]) {
            while (!at_eol(lexer)) lexer->advance(lexer, false);
            lexer->result_symbol = COMMENT;
            lexer->mark_end(lexer);
            return true;
        }
        if ((c == '-' || c == 'D' || c == 'd') && valid_symbols[INDICATOR]) {
            lexer->advance(lexer, false);
            if (c == '-' || lexer->lookahead == ' ' || lexer->lookahead == '\t' || at_eol(lexer)) {
                lexer->result_symbol = INDICATOR;
                lexer->mark_end(lexer);
                return true;
            }
            return false;
        }
    }

    // The identification area: columns 73 on.
    if (fixed && column >= 72 && valid_symbols[IDENTIFICATION_AREA] && !at_eol(lexer)) {
        while (!at_eol(lexer)) lexer->advance(lexer, false);
        lexer->result_symbol = IDENTIFICATION_AREA;
        lexer->mark_end(lexer);
        return true;
    }

    if (directive_start) return valid_symbols[COMPILER_DIRECTIVE] && scan_directive(s, lexer);

    if (lexer->lookahead == '*') {
        if (!valid_symbols[COMMENT]) return false;
        lexer->advance(lexer, false);
        if (lexer->lookahead != '>') return false;
        while (!at_eol(lexer) && lexer->get_column(lexer) < code_limit(s)) lexer->advance(lexer, false);
        lexer->result_symbol = COMMENT;
        lexer->mark_end(lexer);
        return true;
    }

    // In error recovery every token is valid, and a comment entry would swallow any line.
    if (valid_symbols[COMMENT_ENTRY] && !error_mode) {
        if (at_eol(lexer)) return false;
        if (!starts_with_entry_end(s, lexer)) {
            // The line up to the end of its code area.
            while (!at_eol(lexer) && lexer->get_column(lexer) < code_limit(s)) lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            lexer->result_symbol = COMMENT_ENTRY;
            return true;
        }
        return false;
    }

    if (valid_symbols[MULTILINE_STRING] && fixed && !error_mode) return scan_multiline_string(s, lexer);

    return false;
}
