// External scanner for mep's fish grammar.
//
// Started from ram02z/tree-sitter-fish's scanner (public domain, the
// Unlicense): the four zero-width/brace tokens below are its, unchanged in
// behaviour. Added for mep: line continuations and the comments a
// continuation carries over, which fish skips together with their newline
// (`cmd \` / `# why` / `| next` is one pipeline).

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <wctype.h>

enum TokenType {
    CONCAT,
    BRACKET_CONCAT,
    CONCAT_LIST,
    BEGIN_BRACE,
    LINE_CONTINUATION,
    COMMENT,
    COMMENT_NEWLINE,
    IN_STRING,
    LINE_CONTINUATION_GAP,
};

// The scanner's one bit of state: the next token is a comment on a line a
// continuation carried over (so its newline does not end the statement).
typedef struct {
    bool continued;
} Scanner;

void *tree_sitter_fish_external_scanner_create(void) { return ts_calloc(1, sizeof(Scanner)); }
void tree_sitter_fish_external_scanner_destroy(void *payload) { ts_free(payload); }

unsigned tree_sitter_fish_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = (Scanner *)payload;
    buffer[0] = s->continued ? 1 : 0;
    return 1;
}

void tree_sitter_fish_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    s->continued = length > 0 && buffer[0] != 0;
}

static bool is_blank(int32_t c) { return c == ' ' || c == '\t'; }

// A character AMP_IN_WORD lets follow an `&` (grammar.js's word-continue set).
static bool is_word_char(int32_t c) {
    switch (c) {
    case 0: case '$': case '*': case '~': case '(': case ')': case '{': case '}': case '[': case ']':
    case '<': case '>': case '"': case '\'': case '&': case '|': case ';': case '\\': case '#':
        return false;
    default:
        return !iswspace(c);
    }
}

static void skip_blanks(TSLexer *lexer) {
    while (is_blank(lexer->lookahead)) lexer->advance(lexer, true);
}

// After a continuation or a carried-over comment's newline: is the next thing
// on the line (past blanks, which the lexer may look at) a comment?
static bool comment_follows(TSLexer *lexer) {
    while (is_blank(lexer->lookahead)) lexer->advance(lexer, false);
    return lexer->lookahead == '#';
}

bool tree_sitter_fish_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = (Scanner *)payload;
    // Error recovery asks for every token at once; the string sentinel is
    // then valid too, which keeps continuations from arming the flag.
    bool in_string = valid_symbols[IN_STRING];

    if (s->continued) {
        if (valid_symbols[COMMENT_NEWLINE] && (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
            if (lexer->lookahead == '\r') lexer->advance(lexer, false);
            if (lexer->lookahead == '\n') lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            s->continued = comment_follows(lexer);
            lexer->result_symbol = COMMENT_NEWLINE;
            return true;
        }
        if (valid_symbols[COMMENT]) {
            skip_blanks(lexer);
            if (lexer->lookahead == '#') {
                while (lexer->lookahead != '\n' && lexer->lookahead != '\r' && !lexer->eof(lexer))
                    lexer->advance(lexer, false);
                lexer->mark_end(lexer);
                // Its newline continues the line (unless the file ends here).
                s->continued = !lexer->eof(lexer);
                lexer->result_symbol = COMMENT;
                return true;
            }
        }
        s->continued = false;
        return false;
    }

    // BEGIN_BRACE: a `{` where a command may start opens a block, whatever
    // follows it (fish's tokenizer: `{{echo inner}` is two blocks). Must take
    // priority over the internal '{' token used by brace_expansion.
    if (valid_symbols[BEGIN_BRACE] && !in_string) {
        skip_blanks(lexer);
        if (lexer->lookahead == '{') {
            lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            lexer->result_symbol = BEGIN_BRACE;
            return true;
        }
        // Not a brace: a continuation may still stand here (below).
    }

    if (valid_symbols[CONCAT_LIST] && lexer->lookahead == '[') {
        lexer->result_symbol = CONCAT_LIST;
        return true;
    }

    if (valid_symbols[CONCAT]) {
        int32_t c = lexer->lookahead;
        if (!(c == 0 || c == '>' || c == '<' || c == ')' || c == '}' || c == ';' || c == '&' || c == '|' || iswspace(c))) {
            lexer->result_symbol = CONCAT;
            return true;
        }
        if (c == '&') {
            // `"q"&z`: an `&` a word character follows is the word's (see
            // AMP_IN_WORD in grammar.js).
            lexer->mark_end(lexer);
            lexer->advance(lexer, false);
            if (is_word_char(lexer->lookahead)) {
                lexer->result_symbol = CONCAT;
                return true;
            }
            return false;
        }
    }

    if (valid_symbols[BRACKET_CONCAT]) {
        int32_t c = lexer->lookahead;
        if (!(c == 0 || c == ')' || c == '}' || c == ',' || iswspace(c))) {
            lexer->result_symbol = BRACKET_CONCAT;
            return true;
        }
    }

    if (valid_symbols[LINE_CONTINUATION] || valid_symbols[LINE_CONTINUATION_GAP]) {
        skip_blanks(lexer);
        if (lexer->lookahead != '\\') return false;
        lexer->advance(lexer, false);
        if (lexer->lookahead == '\r') lexer->advance(lexer, false);
        if (lexer->lookahead != '\n') return false;
        lexer->advance(lexer, false);
        lexer->mark_end(lexer);
        s->continued = !in_string && comment_follows(lexer);
        // Where a command may start (BEGIN_BRACE is valid exactly there), the
        // continuation is a gap, not the command's name.
        lexer->result_symbol =
            valid_symbols[BEGIN_BRACE] && !in_string && valid_symbols[LINE_CONTINUATION_GAP] ? LINE_CONTINUATION_GAP
                                                                                            : LINE_CONTINUATION;
        return true;
    }

    return false;
}
