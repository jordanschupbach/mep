// External scanner for mep's Python grammar (grammar.js).
//
// Two stacks, both serialized with every token:
//
//   indents      the indentation of each open block (the reference's §2.1.8
//                algorithm: a deeper line pushes and is an INDENT, a
//                shallower one pops one level per DEDENT);
//   delimiters   each open string's quote, prefix flags and triple-ness, so
//                a string inside an f-string's `{...}` can reuse the quotes
//                of the one around it (PEP 701).
//
// INDENT, DEDENT and NEWLINE are zero-width, at the end of the line's last
// token: the whitespace, blank lines and comments after it are extras. Line
// breaks inside brackets are not NEWLINEs: the grammar never expects one
// there, so they fall through to the extras.

#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <stdint.h>
#include <string.h>

enum TokenType {
    NEWLINE,
    INDENT,
    DEDENT,
    STRING_START,
    STRING_CONTENT,
    ESCAPE_INTERPOLATION,
    STRING_END,
    COMMENT,
    CLOSE_BRACKET,
    CLOSE_PAREN,
    CLOSE_BRACE,
    EXCEPT,
};

// One open string, in a byte.
enum {
    SINGLE_QUOTE = 1 << 0,
    DOUBLE_QUOTE = 1 << 1,
    BACK_QUOTE = 1 << 2, // Python 2's `x` repr
    RAW = 1 << 3,
    FORMAT = 1 << 4, // f-strings and t-strings
    TRIPLE = 1 << 5,
    BYTES = 1 << 6,
};

typedef struct {
    Array(uint16_t) indents; // indents[0] is the module's 0, never popped
    Array(uint8_t) delimiters;
} Scanner;

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static int32_t quote_of(uint8_t delimiter) {
    if (delimiter & SINGLE_QUOTE) return '\'';
    if (delimiter & DOUBLE_QUOTE) return '"';
    return '`';
}

static bool is_quote(int32_t c) { return c == '"' || c == '\'' || c == '`'; }

static bool in_format_string(const Scanner *scanner) {
    return scanner->delimiters.size > 0 && (*array_back(&scanner->delimiters) & FORMAT);
}

// `{{` or `}}` in an f-string. A single brace is the grammar's (it opens an
// interpolation, or closes one); returning false leaves it to it.
static bool scan_escaped_brace(TSLexer *lexer) {
    const int32_t brace = lexer->lookahead;
    advance(lexer);
    if (lexer->lookahead != brace) return false;
    advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = ESCAPE_INTERPOLATION;
    return true;
}

// A string's text up to the next thing the grammar lexes itself (a
// backslash escape, an f-string's brace) or to its closing quotes, which
// are then the STRING_END.
static bool scan_string(Scanner *scanner, TSLexer *lexer) {
    const uint8_t delimiter = *array_back(&scanner->delimiters);
    const int32_t quote = quote_of(delimiter);
    bool has_content = false;
    for (;;) {
        const int32_t c = lexer->lookahead;
        if (lexer->eof(lexer)) break;
        if ((delimiter & FORMAT) && (c == '{' || c == '}')) break;
        if (c == '\\') {
            if (delimiter & RAW) {
                // A backslash escapes nothing, but still keeps the quote
                // (or backslash, or line break) after it in the string.
                advance(lexer);
                if (lexer->lookahead == quote || lexer->lookahead == '\\') {
                    advance(lexer);
                } else if (lexer->lookahead == '\r') {
                    advance(lexer);
                    if (lexer->lookahead == '\n') advance(lexer);
                } else if (lexer->lookahead == '\n') {
                    advance(lexer);
                }
                has_content = true;
                continue;
            }
            if (delimiter & BYTES) {
                // \N{...}, \u and \U are not escapes in bytes: plain text.
                lexer->mark_end(lexer);
                advance(lexer);
                if (lexer->lookahead == 'N' || lexer->lookahead == 'u' || lexer->lookahead == 'U') {
                    advance(lexer);
                    has_content = true;
                    continue;
                }
                lexer->result_symbol = STRING_CONTENT;
                return has_content;
            }
            break;
        }
        if (c == quote) {
            lexer->mark_end(lexer);
            advance(lexer);
            if (!(delimiter & TRIPLE)) {
                if (has_content) {
                    lexer->result_symbol = STRING_CONTENT;
                    return true;
                }
                lexer->mark_end(lexer);
                array_pop(&scanner->delimiters);
                lexer->result_symbol = STRING_END;
                return true;
            }
            // One or two quotes inside a triple-quoted string are text.
            if (lexer->lookahead == quote) {
                advance(lexer);
                if (lexer->lookahead == quote) {
                    if (has_content) {
                        lexer->result_symbol = STRING_CONTENT;
                        return true;
                    }
                    advance(lexer);
                    lexer->mark_end(lexer);
                    array_pop(&scanner->delimiters);
                    lexer->result_symbol = STRING_END;
                    return true;
                }
            }
            lexer->mark_end(lexer);
            lexer->result_symbol = STRING_CONTENT;
            return true;
        }
        if (c == '\n' && !(delimiter & TRIPLE) && has_content) {
            // An unterminated one-line string. Producing no token here,
            // rather than its text up to the line break, is what recovers
            // best on damaged code (fewer bytes end up in ERROR nodes).
            return false;
        }
        advance(lexer);
        has_content = true;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = STRING_CONTENT;
    return has_content;
}

// A string's prefix and opening quotes.
static bool scan_string_start(Scanner *scanner, TSLexer *lexer) {
    uint8_t delimiter = 0;
    for (;; advance(lexer)) {
        switch (lexer->lookahead) {
            case 'f': case 'F': case 't': case 'T': delimiter |= FORMAT; continue;
            case 'r': case 'R': delimiter |= RAW; continue;
            case 'b': case 'B': delimiter |= BYTES; continue;
            case 'u': case 'U': continue;
        }
        break;
    }
    // A name like `bar` or `rt` is the grammar's identifier, not a prefix.
    if (!is_quote(lexer->lookahead)) return false;
    const int32_t quote = lexer->lookahead;
    delimiter |= quote == '\'' ? SINGLE_QUOTE : quote == '"' ? DOUBLE_QUOTE : BACK_QUOTE;
    advance(lexer);
    lexer->mark_end(lexer);
    if (quote != '`' && lexer->lookahead == quote) {
        advance(lexer);
        if (lexer->lookahead == quote) {
            advance(lexer);
            lexer->mark_end(lexer);
            delimiter |= TRIPLE;
        }
        // Otherwise an empty string: the second quote is its STRING_END.
    }
    if (scanner->delimiters.size >= UINT8_MAX) return false;
    array_push(&scanner->delimiters, delimiter);
    lexer->result_symbol = STRING_START;
    return true;
}

bool tree_sitter_python_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;

    // Error recovery offers every token at once; none of a string's
    // interior is trusted then.
    const bool error_recovery = valid_symbols[STRING_CONTENT] && valid_symbols[INDENT];
    const bool within_brackets = valid_symbols[CLOSE_BRACE] || valid_symbols[CLOSE_PAREN] || valid_symbols[CLOSE_BRACKET];

    if (scanner->delimiters.size > 0 && !error_recovery) {
        const uint8_t delimiter = *array_back(&scanner->delimiters);
        if (valid_symbols[ESCAPE_INTERPOLATION] && (delimiter & FORMAT) &&
            (lexer->lookahead == '{' || lexer->lookahead == '}')) {
            return scan_escaped_brace(lexer);
        }
        if (valid_symbols[STRING_CONTENT]) return scan_string(scanner, lexer);
    }

    // The end of a line, then the indentation of the next one that has code.
    lexer->mark_end(lexer);
    bool found_end_of_line = false;
    uint16_t indent = 0;
    int32_t first_comment_indent = -1;
    for (;;) {
        const int32_t c = lexer->lookahead;
        if (c == '\n') {
            found_end_of_line = true;
            indent = 0;
            skip(lexer);
        } else if (c == ' ') {
            if (indent < UINT16_MAX) indent++;
            skip(lexer);
        } else if (c == '\t') {
            // Tabs to the next multiple of 8, as the tokenizer counts them.
            indent = indent > UINT16_MAX - 8 ? UINT16_MAX : (uint16_t)((indent / 8 + 1) * 8);
            skip(lexer);
        } else if (c == '\r' || c == '\f') {
            indent = 0;
            skip(lexer);
        } else if (c == '#' && (valid_symbols[INDENT] || valid_symbols[DEDENT] || valid_symbols[NEWLINE] ||
                                valid_symbols[EXCEPT])) {
            // A comment after code on the same line is just an extra.
            if (!found_end_of_line) return false;
            // Comment lines don't count for indentation, but the first one
            // decides whether the comments stay in the block (see below).
            if (first_comment_indent == -1) first_comment_indent = indent;
            while (lexer->lookahead && lexer->lookahead != '\n' && !lexer->eof(lexer)) skip(lexer);
            skip(lexer);
            indent = 0;
        } else if (c == '\\') {
            // An explicit line join is not the end of a line.
            skip(lexer);
            if (lexer->lookahead == '\r') skip(lexer);
            if (lexer->lookahead != '\n' && !lexer->eof(lexer)) return false;
            skip(lexer);
        } else if (lexer->eof(lexer)) {
            indent = 0;
            found_end_of_line = true;
            break;
        } else {
            break;
        }
    }

    if (found_end_of_line) {
        const uint16_t current = *array_back(&scanner->indents);
        if (valid_symbols[INDENT] && indent > current) {
            array_push(&scanner->indents, indent);
            lexer->result_symbol = INDENT;
            return true;
        }
        // A DEDENT where expected, and also where nothing at all fits (a
        // line was left unfinished), so recovery resumes in the right block
        // -- but not inside brackets or an f-string's `{...}`, where a
        // line's indentation means nothing. (Inside any other string we
        // only get here when it was left unterminated.) Comments indented
        // to the block (or deeper) stay in it: the dedent waits until
        // after them.
        const bool next_is_string = valid_symbols[STRING_START] && is_quote(lexer->lookahead);
        if ((valid_symbols[DEDENT] || (!valid_symbols[NEWLINE] && !next_is_string && !within_brackets)) &&
            indent < current && scanner->indents.size > 1 && !in_format_string(scanner) &&
            first_comment_indent < (int32_t)current) {
            array_pop(&scanner->indents);
            lexer->result_symbol = DEDENT;
            return true;
        }
        if (valid_symbols[NEWLINE] && !error_recovery) {
            lexer->result_symbol = NEWLINE;
            return true;
        }
    }

    if (first_comment_indent == -1 && valid_symbols[STRING_START]) return scan_string_start(scanner, lexer);
    return false;
}

// [delimiter count] [delimiters...] [indents[1..], two bytes each, LE]
unsigned tree_sitter_python_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    unsigned size = 0;
    const uint32_t delimiters = scanner->delimiters.size;
    buffer[size++] = (char)delimiters;
    if (delimiters > 0) memcpy(&buffer[size], scanner->delimiters.contents, delimiters);
    size += delimiters;
    for (uint32_t i = 1; i < scanner->indents.size && size + 2 <= TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++) {
        const uint16_t indent = *array_get(&scanner->indents, i);
        buffer[size++] = (char)(indent & 0xFF);
        buffer[size++] = (char)(indent >> 8);
    }
    return size;
}

void tree_sitter_python_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    array_clear(&scanner->delimiters);
    array_clear(&scanner->indents);
    array_push(&scanner->indents, 0);
    if (length == 0) return;
    unsigned size = 0;
    const uint32_t delimiters = (uint8_t)buffer[size++];
    if (delimiters > 0 && size + delimiters <= length) {
        array_extend(&scanner->delimiters, delimiters, &buffer[size]);
    }
    size += delimiters;
    for (; size + 1 < length; size += 2) {
        array_push(&scanner->indents, (uint16_t)((uint8_t)buffer[size] | ((uint8_t)buffer[size + 1] << 8)));
    }
}

void *tree_sitter_python_external_scanner_create(void) {
    Scanner *scanner = ts_calloc(1, sizeof(Scanner));
    array_init(&scanner->indents);
    array_init(&scanner->delimiters);
    array_push(&scanner->indents, 0);
    return scanner;
}

void tree_sitter_python_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    array_delete(&scanner->indents);
    array_delete(&scanner->delimiters);
    ts_free(scanner);
}
