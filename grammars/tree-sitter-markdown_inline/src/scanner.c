// The external scanner for mep's Markdown inline grammar: what CommonMark
// decides by looking around a run of delimiters (spec.commonmark.org §6).
//
//   Code spans (`` ` ``) and LaTeX spans (`$`): a run opens a span only if a
//   run of the same length closes it later in the range; else it is text
//   (`_unclosed_span`). The open span's run length is the state.
//   Emphasis (`*`, `_`) and strikethrough (`~`): the run's first character
//   decides for the whole run whether it opens or closes, from what comes
//   right after the run (whitespace, punctuation, other) and, through the
//   grammar's never-produced `_last_token_whitespace` and
//   `_last_token_punctuation` markers, what came right before it (§6.2's
//   left- and right-flanking rules, simplified the way the old grammar
//   did). Each later character of the run is then a token of its own
//   with the same decision; the characters left and the decision are the
//   state.
//
// State, saved with every token: the decision flag, the open code and
// LaTeX spans' run lengths, and the delimiters left in the current run.

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

// In grammar.js's `externals` order.
typedef enum {
    ERROR,
    TRIGGER_ERROR,
    CODE_SPAN_START,
    CODE_SPAN_CLOSE,
    EMPHASIS_OPEN_STAR,
    EMPHASIS_OPEN_UNDERSCORE,
    EMPHASIS_CLOSE_STAR,
    EMPHASIS_CLOSE_UNDERSCORE,
    LAST_TOKEN_WHITESPACE,
    LAST_TOKEN_PUNCTUATION,
    STRIKETHROUGH_OPEN,
    STRIKETHROUGH_CLOSE,
    LATEX_SPAN_START,
    LATEX_SPAN_CLOSE,
    UNCLOSED_SPAN,
} TokenType;

// The current delimiter run opens (else it closes).
#define STATE_RUN_OPENS 0x04

typedef struct {
    uint8_t state;
    uint8_t code_span_length;
    uint8_t latex_span_length;
    // Delimiters left in the current run, each to be a token like the first.
    uint8_t run_left;
} Scanner;

void *tree_sitter_markdown_inline_external_scanner_create(void) { return ts_calloc(1, sizeof(Scanner)); }

void tree_sitter_markdown_inline_external_scanner_destroy(void *payload) { ts_free(payload); }

unsigned tree_sitter_markdown_inline_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = payload;
    buffer[0] = (char)s->state;
    buffer[1] = (char)s->code_span_length;
    buffer[2] = (char)s->latex_span_length;
    buffer[3] = (char)s->run_left;
    return 4;
}

void tree_sitter_markdown_inline_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = payload;
    s->state = s->code_span_length = s->latex_span_length = s->run_left = 0;
    if (length < 4) return;
    s->state = (uint8_t)buffer[0];
    s->code_span_length = (uint8_t)buffer[1];
    s->latex_span_length = (uint8_t)buffer[2];
    s->run_left = (uint8_t)buffer[3];
}

static bool is_punctuation(int32_t c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

// A run of backticks or dollars: the open span's close if as long, or a
// span's start if a run as long follows, or (where valid) unclosed text.
static bool scan_span(TSLexer *lexer, uint8_t *open_length, const bool *valid, int32_t delimiter, TokenType open,
                      TokenType close) {
    unsigned length = 0;
    while (lexer->lookahead == delimiter) {
        advance(lexer);
        length++;
    }
    lexer->mark_end(lexer);
    const uint8_t clamped = length > 255 ? 255 : (uint8_t)length;
    if (clamped == *open_length && valid[close]) {
        *open_length = 0;
        lexer->result_symbol = close;
        return true;
    }
    if (!valid[open]) return false;
    // Look for a run of exactly the same length.
    unsigned run = 0;
    while (!lexer->eof(lexer)) {
        if (lexer->lookahead == delimiter) {
            run++;
        } else {
            if (run == length) break;
            run = 0;
        }
        advance(lexer);
    }
    if (run == length) {
        *open_length = clamped;
        lexer->result_symbol = open;
        return true;
    }
    if (valid[UNCLOSED_SPAN]) {
        lexer->result_symbol = UNCLOSED_SPAN;
        return true;
    }
    return false;
}

// A `*`, `_` or `~`: the next delimiter of a run already decided, or the
// first of a run, which decides open or close for all of it.
static bool scan_delimiter_run(Scanner *s, TSLexer *lexer, const bool *valid, int32_t delimiter, TokenType open,
                               TokenType close) {
    advance(lexer);
    if (s->run_left > 0) {
        if ((s->state & STATE_RUN_OPENS) && valid[open]) {
            s->state &= ~STATE_RUN_OPENS;
            lexer->result_symbol = open;
            s->run_left--;
            return true;
        }
        if (valid[close]) {
            lexer->result_symbol = close;
            s->run_left--;
            return true;
        }
    }
    lexer->mark_end(lexer);
    unsigned count = 1;
    while (lexer->lookahead == delimiter) {
        count++;
        advance(lexer);
    }
    if (!valid[open] && !valid[close]) return false;
    s->run_left = count - 1 > 255 ? 255 : (uint8_t)(count - 1);
    const bool line_end = lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->eof(lexer);
    const bool next_whitespace = line_end || lexer->lookahead == ' ' || lexer->lookahead == '\t';
    const bool next_punctuation = is_punctuation(lexer->lookahead);
    // Closing wins: not after whitespace, and (after punctuation) only
    // before whitespace or punctuation.
    if (valid[close] && !valid[LAST_TOKEN_WHITESPACE] &&
        (!valid[LAST_TOKEN_PUNCTUATION] || next_punctuation || next_whitespace)) {
        s->state &= ~STATE_RUN_OPENS;
        lexer->result_symbol = close;
        return true;
    }
    // Opening: not before whitespace, and (before punctuation) only after
    // whitespace or punctuation.
    if (!next_whitespace && (!next_punctuation || valid[LAST_TOKEN_PUNCTUATION] || valid[LAST_TOKEN_WHITESPACE])) {
        s->state |= STATE_RUN_OPENS;
        lexer->result_symbol = open;
        return true;
    }
    return false;
}

bool tree_sitter_markdown_inline_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid) {
    Scanner *s = payload;
    if (valid[TRIGGER_ERROR]) {
        lexer->result_symbol = ERROR;
        return true;
    }
    switch (lexer->lookahead) {
        case '`':
            return scan_span(lexer, &s->code_span_length, valid, '`', CODE_SPAN_START, CODE_SPAN_CLOSE);
        case '$':
            return scan_span(lexer, &s->latex_span_length, valid, '$', LATEX_SPAN_START, LATEX_SPAN_CLOSE);
        case '*':
            return scan_delimiter_run(s, lexer, valid, '*', EMPHASIS_OPEN_STAR, EMPHASIS_CLOSE_STAR);
        case '_':
            return scan_delimiter_run(s, lexer, valid, '_', EMPHASIS_OPEN_UNDERSCORE, EMPHASIS_CLOSE_UNDERSCORE);
        case '~':
            return scan_delimiter_run(s, lexer, valid, '~', STRIKETHROUGH_OPEN, STRIKETHROUGH_CLOSE);
        default:
            return false;
    }
}
