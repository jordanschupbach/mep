// External scanner for mep's C# grammar (grammars/tree-sitter-c_sharp).
//
// C#'s tokens are context-free except for strings whose end depends on how
// they began (spec §6.4.5.6 and the raw string literal feature spec):
//
//   * raw string literals, `"""` ... `"""`: opened by three or more quotes
//     and closed by the same number;
//   * interpolated strings, `$"..{x}.."`, `$@"..."`/`@$"..."`, and raw
//     interpolated strings `$$"""..{{x}}.."""`: an interpolation opens with
//     as many braces as the string has `$`s, and the text between
//     interpolations ends at a quote run, a brace run or (in a regular
//     string) an escape or a newline.
//
// Interpolations nest (`$"{(b ? $"{x}" : "")}"`), so the scanner keeps a
// stack of the open interpolated strings. It also reads `_optional_semi`,
// the `;` a namespace or type body may end with.
//
// Which token is wanted comes from tree-sitter's valid_symbols. The checks
// run in a fixed order and share what earlier checks consumed: a quote run
// too short to close a raw interpolated string becomes part of its text,
// as does a brace run too short to open an interpolation.

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <wctype.h>

// In grammar.js's `externals` order.
enum TokenType {
    OPTIONAL_SEMI,
    INTERPOLATION_REGULAR_START,
    INTERPOLATION_VERBATIM_START,
    INTERPOLATION_RAW_START,
    INTERPOLATION_START_QUOTE,
    INTERPOLATION_END_QUOTE,
    INTERPOLATION_OPEN_BRACE,
    INTERPOLATION_CLOSE_BRACE,
    INTERPOLATION_STRING_CONTENT,
    RAW_STRING_START,
    RAW_STRING_END,
    RAW_STRING_CONTENT,
};

// The kind of an open interpolated string, as bits: a raw string opened
// with `$` and `@` is never made, but the bits keep the checks independent.
enum StringKind {
    KIND_REGULAR = 1,
    KIND_VERBATIM = 2,
    KIND_RAW = 4,
};

// One open interpolated string.
typedef struct {
    uint8_t dollars;      // `$` count: braces needed to open an interpolation
    uint8_t open_braces;  // braces that opened the current interpolation (0: in text)
    uint8_t quotes;       // quotes that opened the string (closing needs as many)
    uint8_t kind;         // StringKind bits
} OpenString;

typedef struct {
    uint8_t raw_quotes;  // quotes that opened the current raw string literal
    Array(OpenString) strings;
} Scanner;

// What the checks of one scan share: the quotes and braces already
// consumed and whether any text was.
typedef struct {
    uint8_t quotes;
    uint8_t braces;
    bool consumed;
} Progress;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static uint8_t consume_run(TSLexer *lexer, int32_t c) {
    uint8_t n = 0;
    while (lexer->lookahead == c) {
        advance(lexer);
        n++;
    }
    return n;
}

void *tree_sitter_c_sharp_external_scanner_create(void) {
    Scanner *scanner = ts_calloc(1, sizeof(Scanner));
    array_init(&scanner->strings);
    return scanner;
}

void tree_sitter_c_sharp_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    array_delete(&scanner->strings);
    ts_free(scanner);
}

// Layout: raw_quotes, the stack depth, then four bytes per open string.
// A stack too deep for the buffer serializes as nothing (a fresh state).
unsigned tree_sitter_c_sharp_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    uint32_t depth = scanner->strings.size;
    if (depth * 4 + 2 > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) return 0;

    unsigned n = 0;
    buffer[n++] = (char)scanner->raw_quotes;
    buffer[n++] = (char)depth;
    for (uint32_t i = 0; i < depth; i++) {
        OpenString *s = &scanner->strings.contents[i];
        buffer[n++] = (char)s->dollars;
        buffer[n++] = (char)s->open_braces;
        buffer[n++] = (char)s->quotes;
        buffer[n++] = (char)s->kind;
    }
    return n;
}

void tree_sitter_c_sharp_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->raw_quotes = 0;
    array_clear(&scanner->strings);
    if (length < 2) return;

    unsigned n = 0;
    scanner->raw_quotes = (uint8_t)buffer[n++];
    uint32_t depth = (uint8_t)buffer[n++];
    if (2 + depth * 4 > length) depth = (length - 2) / 4;
    array_reserve(&scanner->strings, depth);
    for (uint32_t i = 0; i < depth; i++) {
        OpenString s;
        s.dollars = (uint8_t)buffer[n++];
        s.open_braces = (uint8_t)buffer[n++];
        s.quotes = (uint8_t)buffer[n++];
        s.kind = (uint8_t)buffer[n++];
        array_push(&scanner->strings, s);
    }
}

static inline OpenString *innermost(Scanner *scanner) {
    return scanner->strings.size > 0 ? array_back(&scanner->strings) : NULL;
}

// At a `{` in an interpolated string's text: are these braces (counted
// from progress->braces) the start of an interpolation, so the text ends
// here? Marks the text's end before them first.
static bool at_interpolation_braces(TSLexer *lexer, OpenString *s, Progress *p) {
    lexer->mark_end(lexer);
    while (lexer->lookahead == '{' && p->braces < s->open_braces) {
        advance(lexer);
        p->braces++;
    }
    return p->braces == s->open_braces && (p->braces == 0 || lexer->lookahead != '{');
}

// ---- raw string literals ------------------------------------------------------

// `"""` or more: a raw string's opening. A shorter run is left consumed in
// progress->quotes for the checks after this one (which then find nothing).
static bool scan_raw_string_start(Scanner *scanner, TSLexer *lexer, Progress *p) {
    while (iswspace(lexer->lookahead)) skip(lexer);
    if (lexer->lookahead != '"') return false;
    p->quotes += consume_run(lexer, '"');
    if (p->quotes < 3) return false;
    lexer->result_symbol = RAW_STRING_START;
    scanner->raw_quotes = p->quotes;
    return true;
}

static bool scan_raw_string_end(Scanner *scanner, TSLexer *lexer, Progress *p) {
    if (lexer->lookahead != '"') return false;
    p->quotes += consume_run(lexer, '"');
    if (p->quotes == scanner->raw_quotes) {
        lexer->result_symbol = RAW_STRING_END;
        scanner->raw_quotes = 0;
        return true;
    }
    p->consumed = p->quotes > 0;
    return false;
}

// Everything up to the closing quote run (or the end of input).
static bool scan_raw_string_content(Scanner *scanner, TSLexer *lexer, Progress *p) {
    lexer->result_symbol = RAW_STRING_CONTENT;
    while (lexer->lookahead) {
        if (lexer->lookahead == '"') {
            lexer->mark_end(lexer);
            p->quotes = consume_run(lexer, '"');
            if (p->quotes == scanner->raw_quotes) return true;
        }
        advance(lexer);
        p->consumed = true;
    }
    lexer->mark_end(lexer);
    return true;
}

// ---- interpolated strings ----------------------------------------------------

// `$"`, `$@"`, `@$"`, `$$"""`: the token is the `$`s and `@` (the quotes
// are interpolation_start_quote). Pushes the string unless it is `$""`,
// an empty regular string, whose quotes are then read as start and end.
static bool scan_interpolation_start(Scanner *scanner, TSLexer *lexer, Progress *p) {
    while (iswspace(lexer->lookahead)) skip(lexer);

    bool verbatim = false;
    if (lexer->lookahead == '@') {
        verbatim = true;
        advance(lexer);
    }
    uint8_t dollars = 0;
    while (lexer->lookahead == '$' && p->quotes == 0) {
        advance(lexer);
        dollars++;
    }
    if (dollars == 0 || (lexer->lookahead != '"' && lexer->lookahead != '@')) return false;

    OpenString s = {.dollars = dollars, .open_braces = 0, .quotes = 0, .kind = 0};
    lexer->result_symbol = INTERPOLATION_REGULAR_START;
    if (verbatim || lexer->lookahead == '@') {
        if (lexer->lookahead == '@') {
            advance(lexer);
            verbatim = true;
        }
        lexer->result_symbol = INTERPOLATION_VERBATIM_START;
        s.kind = KIND_VERBATIM;
    }
    lexer->mark_end(lexer);

    // Peek at the quotes: three or more open a raw string; one opens a
    // regular (or verbatim) string, and so do two -- `$""`, empty.
    // (tree-sitter-c-sharp 0.23.5 pushed nothing for `$""`, leaving its
    // quotes unreadable.)
    advance(lexer);
    if (lexer->lookahead == '"' && !verbatim) {
        advance(lexer);
        if (lexer->lookahead == '"') {
            lexer->result_symbol = INTERPOLATION_RAW_START;
            s.kind |= KIND_RAW;
        } else {
            s.kind |= KIND_REGULAR;
        }
    } else {
        s.kind |= KIND_REGULAR;
    }
    array_push(&scanner->strings, s);
    return true;
}

static bool scan_start_quote(TSLexer *lexer, OpenString *s) {
    if (s->kind & (KIND_REGULAR | KIND_VERBATIM)) {
        if (lexer->lookahead == '"') {
            advance(lexer);
            s->quotes++;
        }
    } else {
        while (lexer->lookahead == '"') {
            advance(lexer);
            s->quotes++;
        }
    }
    lexer->result_symbol = INTERPOLATION_START_QUOTE;
    return s->quotes > 0;
}

static bool end_string(Scanner *scanner, TSLexer *lexer) {
    lexer->result_symbol = INTERPOLATION_END_QUOTE;
    array_pop(&scanner->strings);
    return true;
}

// The closing quotes. A raw string's run of another length is text
// (progress). In a verbatim string `""` is an escaped quote, so of a run
// the pairs are text and an odd quote left over closes the string; a
// regular string ends at its first quote. (tree-sitter-c-sharp 0.23.5
// read every string's run like a raw one, so a verbatim string ending in
// an escaped quote, `...""{x}"""`, ran on past its end.)
static bool scan_end_quote(Scanner *scanner, TSLexer *lexer, Progress *p) {
    OpenString *s = innermost(scanner);
    if (s->kind & KIND_RAW) {
        p->quotes += consume_run(lexer, '"');
        if (p->quotes == s->quotes) return end_string(scanner, lexer);
        p->consumed = p->quotes > 0;
        return false;
    }
    if (lexer->lookahead != '"') return false;
    advance(lexer);
    if ((s->kind & KIND_VERBATIM) && lexer->lookahead == '"') {
        advance(lexer);
        p->quotes += 2;
        p->consumed = true;
        return false;
    }
    return end_string(scanner, lexer);
}

static bool open_interpolation(TSLexer *lexer, OpenString *s, uint8_t braces) {
    s->open_braces = braces;
    lexer->result_symbol = INTERPOLATION_OPEN_BRACE;
    return true;
}

// An interpolation's opening braces.
//
// A raw string with n `$`s opens one with a run of n to 2n-1 `{` (the
// first ones of a longer run are literal braces; they are read as part of
// the opening here). A shorter run is text.
//
// In a regular or verbatim string (one `$`) `{{` is an escaped brace: of a
// run, the pairs are text and an odd brace left over opens an
// interpolation. The text before that brace is returned here, as
// interpolation_string_content (`{{{x}`); an even run is left consumed for
// scan_string_content to continue. (tree-sitter-c-sharp 0.23.5 read every
// run longer than the `$`s as text, missing the interpolation.)
static bool scan_open_brace(TSLexer *lexer, OpenString *s, Progress *p, bool content_valid) {
    if (s->kind & KIND_RAW) {
        while (lexer->lookahead == '{' && p->braces < s->dollars) {
            advance(lexer);
            p->braces++;
        }
        if (p->braces == 0 || p->braces < s->dollars) return false;
        if (lexer->lookahead != '{') return open_interpolation(lexer, s, p->braces);
        uint8_t extra = consume_run(lexer, '{');
        if (extra < s->dollars) return open_interpolation(lexer, s, s->dollars);
        p->braces += extra;
        return false;
    }

    if (lexer->lookahead != '{') return false;
    advance(lexer);
    if (lexer->lookahead != '{') return open_interpolation(lexer, s, 1);
    for (;;) {
        // A pair: text.
        advance(lexer);
        p->consumed = true;
        if (lexer->lookahead != '{') return false;
        lexer->mark_end(lexer);
        advance(lexer);
        if (lexer->lookahead != '{') {
            // One left over: the text ends before it, and the next scan
            // opens the interpolation with it.
            if (!content_valid) return false;
            lexer->result_symbol = INTERPOLATION_STRING_CONTENT;
            return true;
        }
    }
}

// As many `}` as opened the interpolation (blanks before them included).
static bool scan_close_brace(TSLexer *lexer, OpenString *s) {
    uint8_t braces = 0;
    while (iswspace(lexer->lookahead)) advance(lexer);
    while (lexer->lookahead == '}') {
        advance(lexer);
        braces++;
        if (braces == s->open_braces) {
            s->open_braces = 0;
            lexer->result_symbol = INTERPOLATION_CLOSE_BRACE;
            return true;
        }
    }
    return false;
}

// Text up to the closing quotes or the next interpolation; a regular
// string's text also stops at `\` (an escape_sequence token) and at a
// newline (an error: regular strings are one line). Succeeds only if
// something was consumed, here or by an earlier check of this scan.
static bool scan_string_content(TSLexer *lexer, OpenString *s, Progress *p) {
    lexer->result_symbol = INTERPOLATION_STRING_CONTENT;
    while (lexer->lookahead) {
        if (s->kind & KIND_RAW) {
            if (lexer->lookahead == '"') {
                lexer->mark_end(lexer);
                advance(lexer);
                if (lexer->lookahead == '"') {
                    advance(lexer);
                    uint8_t run = 2 + consume_run(lexer, '"');
                    if (run == s->quotes) return p->consumed;
                }
            }
            if (lexer->lookahead == '{' && at_interpolation_braces(lexer, s, p)) return p->consumed;
        } else if (s->kind & KIND_VERBATIM) {
            if (lexer->lookahead == '"') {
                // `""` is an escaped quote; one quote ends the string.
                lexer->mark_end(lexer);
                advance(lexer);
                if (lexer->lookahead == '"') {
                    advance(lexer);
                    continue;
                }
                return p->consumed;
            }
            if (lexer->lookahead == '{' && at_interpolation_braces(lexer, s, p)) return p->consumed;
        } else if (s->kind & KIND_REGULAR) {
            if (lexer->lookahead == '\\' || lexer->lookahead == '\n' || lexer->lookahead == '"') {
                lexer->mark_end(lexer);
                return p->consumed;
            }
            if (lexer->lookahead == '{' && at_interpolation_braces(lexer, s, p)) return p->consumed;
        }

        if (lexer->lookahead != '{') p->braces = 0;
        advance(lexer);
        p->consumed = true;
    }
    lexer->mark_end(lexer);
    return p->consumed;
}

bool tree_sitter_c_sharp_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    Progress progress = {0, 0, false};

    // Both wanted at once only in error recovery, where tree-sitter's own
    // lexer gives better trees.
    if (valid_symbols[OPTIONAL_SEMI] && valid_symbols[INTERPOLATION_REGULAR_START]) return false;

    if (valid_symbols[OPTIONAL_SEMI]) {
        lexer->result_symbol = OPTIONAL_SEMI;
        if (lexer->lookahead == ';') advance(lexer);
        return true;
    }

    if (valid_symbols[RAW_STRING_START] && scan_raw_string_start(scanner, lexer, &progress)) return true;
    if (valid_symbols[RAW_STRING_END] && scan_raw_string_end(scanner, lexer, &progress)) return true;
    if (valid_symbols[RAW_STRING_CONTENT]) return scan_raw_string_content(scanner, lexer, &progress);

    if ((valid_symbols[INTERPOLATION_REGULAR_START] || valid_symbols[INTERPOLATION_VERBATIM_START] ||
         valid_symbols[INTERPOLATION_RAW_START]) &&
        scan_interpolation_start(scanner, lexer, &progress)) {
        return true;
    }

    OpenString *s = innermost(scanner);
    if (s == NULL) return false;

    if (valid_symbols[INTERPOLATION_START_QUOTE]) return scan_start_quote(lexer, s);
    if (valid_symbols[INTERPOLATION_END_QUOTE] && scan_end_quote(scanner, lexer, &progress)) return true;
    if (valid_symbols[INTERPOLATION_OPEN_BRACE] &&
        scan_open_brace(lexer, s, &progress, valid_symbols[INTERPOLATION_STRING_CONTENT])) {
        return true;
    }
    if (valid_symbols[INTERPOLATION_CLOSE_BRACE]) return scan_close_brace(lexer, s);
    if (valid_symbols[INTERPOLATION_STRING_CONTENT]) return scan_string_content(lexer, s, &progress);
    return false;
}
