// The external scanner for mep's Org grammar: what depends on lines
// already read, which the grammar cannot remember.
//
//   _liststart     zero-width, before a bullet indented deeper than the
//                  enclosing list's (or any bullet, outside a list).
//   _listitemend   zero-width, before the next bullet of the same kind at
//                  the same indentation: the item ends, the list goes on.
//   _listend       zero-width, where the list ends: a shallower line, a
//                  line at the same indentation that is not the same kind
//                  of bullet, a blank line followed by another, or the end.
//   bullet         `-`, `+`, `*`, `1.`, `1)`, `a.`, `A)`, ...
//   _stars         zero-width, before a headline's stars.
//   _sectionend    zero-width, before a headline with as many stars as the
//                  open section's, or fewer, or at the end of the input.
//   _eof           zero-width, the end of the input.
//   _link_open     `[[`, when `]]` closes it later on the line.
//   latex_math_single_dollar
//                  a `$` that opens inline math (a closing `$` follows on
//                  the line, not inside a word) or closes it.
//   text_dollar    any other `$` where math could have opened.
//   error_sentinel never produced: valid only during error recovery.
//   _drawer_start  zero-width, before a line that is just `:NAME:` (a
//                  drawer's first line; not `:END:`, which closes one).
//   _property_drawer_start
//                  the same, for `:PROPERTIES:` where a headline's property
//                  drawer may start (elsewhere it is an ordinary drawer).
//   _block_begin   zero-width, before `#+begin_NAME`; remembers NAME.
//   _block_end     zero-width, before `#+end_NAME` with the open block's
//                  own NAME (any case); any other `#+end_` line is text.
//                  When no line later in the input closes the block by
//                  name (a typo, an edit half made), any `#+end_` closes
//                  it, so one bad name does not swallow the document.
//   _math_start    zero-width, before `\(`, `\[` or `$$` when `\)`, `\]` or
//                  `$$` closes it later on the line.
//   _footnote_start
//                  zero-width, before `[fn:` when a `]` closes it before
//                  the paragraph ends (a blank line).
//
// State: the indentation and bullet kind of each open list, the star
// count of each open section, and whether a `$` math span is open; all
// saved with every token so incremental reparses resume exactly.

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <string.h>
#include <wctype.h>

enum TokenType {
    LIST_START,
    LIST_END,
    LIST_ITEM_END,
    BULLET,
    STARS,
    SECTION_END,
    END_OF_FILE,
    LINK_OPEN,
    LATEX_MATH_SINGLE_DOLLAR,
    TEXT_DOLLAR,
    ERROR_SENTINEL,
    DRAWER_START,
    PROPERTY_DRAWER_START,
    BLOCK_BEGIN,
    BLOCK_END,
    MATH_START,
    FOOTNOTE_START,
};

typedef enum {
    NOT_A_BULLET,
    DASH,
    PLUS,
    STAR,
    LOWER_DOT,
    UPPER_DOT,
    LOWER_PAREN,
    UPPER_PAREN,
    NUMBER_DOT,
    NUMBER_PAREN,
} Bullet;

#define MAX_BLOCK_NAME 32

typedef struct {
    Array(int16_t) indents;  // each open list's indentation; -1 at the bottom
    Array(uint8_t) bullets;  // each open list's bullet kind; NOT_A_BULLET at the bottom
    Array(uint8_t) sections; // each open section's star count; 0 at the bottom
    bool in_dollar_math;
    // The open block's name, lower-cased; empty when none is open. Longer
    // names are compared on this prefix.
    uint8_t block_name_length;
    char block_name[MAX_BLOCK_NAME];
    bool block_any_end;  // no `#+end_NAME` follows: the next `#+end_` closes it
} Scanner;

static void reset(Scanner *s) {
    array_clear(&s->indents);
    array_push(&s->indents, -1);
    array_clear(&s->bullets);
    array_push(&s->bullets, NOT_A_BULLET);
    array_clear(&s->sections);
    array_push(&s->sections, 0);
    s->in_dollar_math = false;
    s->block_name_length = 0;
    s->block_any_end = false;
}

void *tree_sitter_org_external_scanner_create(void) {
    Scanner *s = ts_calloc(1, sizeof(Scanner));
    array_init(&s->indents);
    array_init(&s->bullets);
    array_init(&s->sections);
    reset(s);
    return s;
}

void tree_sitter_org_external_scanner_destroy(void *payload) {
    Scanner *s = payload;
    array_delete(&s->indents);
    array_delete(&s->bullets);
    array_delete(&s->sections);
    ts_free(s);
}

// Layout: the dollar flag; the list count and, per list, its indentation
// (two bytes) and bullet kind; the section count and each star count.
// Lists and sections deeper than fit are dropped from the save, innermost
// first, which only loses accuracy where nesting is absurd.
unsigned tree_sitter_org_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = payload;
    unsigned n = 0;
    buffer[n++] = (char)s->in_dollar_math;
    buffer[n++] = (char)(s->block_name_length | (s->block_any_end ? 0x80 : 0));
    memcpy(buffer + n, s->block_name, s->block_name_length);
    n += s->block_name_length;
    unsigned lists = s->indents.size - 1;
    unsigned room = (TREE_SITTER_SERIALIZATION_BUFFER_SIZE - n - 2) / 3;
    if (lists > room) lists = room;
    if (lists > 255) lists = 255;
    buffer[n++] = (char)lists;
    for (unsigned i = 1; i <= lists; i++) {
        uint16_t indent = (uint16_t)s->indents.contents[i];
        buffer[n++] = (char)(indent & 0xff);
        buffer[n++] = (char)(indent >> 8);
        buffer[n++] = (char)s->bullets.contents[i];
    }
    unsigned sections = s->sections.size - 1;
    unsigned left = TREE_SITTER_SERIALIZATION_BUFFER_SIZE - n - 1;
    if (sections > left) sections = left;
    if (sections > 255) sections = 255;
    buffer[n++] = (char)sections;
    for (unsigned i = 1; i <= sections; i++) buffer[n++] = (char)s->sections.contents[i];
    return n;
}

void tree_sitter_org_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = payload;
    reset(s);
    if (length < 3) return;
    unsigned n = 0;
    s->in_dollar_math = buffer[n++] != 0;
    unsigned name_length = (uint8_t)buffer[n++];
    s->block_any_end = (name_length & 0x80) != 0;
    name_length &= 0x7f;
    if (name_length > MAX_BLOCK_NAME || n + name_length > length) return;
    s->block_name_length = (uint8_t)name_length;
    memcpy(s->block_name, buffer + n, name_length);
    n += name_length;
    if (n >= length) return;
    unsigned lists = (uint8_t)buffer[n++];
    for (unsigned i = 0; i < lists && n + 3 <= length; i++) {
        uint16_t indent = (uint16_t)((uint8_t)buffer[n] | ((uint8_t)buffer[n + 1] << 8));
        array_push(&s->indents, (int16_t)indent);
        array_push(&s->bullets, (uint8_t)buffer[n + 2]);
        n += 3;
    }
    if (n >= length) return;
    unsigned sections = (uint8_t)buffer[n++];
    for (unsigned i = 0; i < sections && n < length; i++) array_push(&s->sections, (uint8_t)buffer[n++]);
}

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool is_alnum_ascii(int32_t c) {
    return ('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z');
}

// Reads a bullet, if one starts here: the marker and a blank after it.
// Consumes what it reads either way.
static Bullet read_bullet(TSLexer *lexer) {
    int32_t c = lexer->lookahead;
    if (c == '-' || c == '+' || c == '*') {
        advance(lexer);
        if (!iswspace(lexer->lookahead)) return NOT_A_BULLET;
        return c == '-' ? DASH : c == '+' ? PLUS : STAR;
    }
    Bullet dot = NOT_A_BULLET, paren = NOT_A_BULLET;
    if ('a' <= c && c <= 'z') {
        advance(lexer);
        dot = LOWER_DOT, paren = LOWER_PAREN;
    } else if ('A' <= c && c <= 'Z') {
        advance(lexer);
        dot = UPPER_DOT, paren = UPPER_PAREN;
    } else if ('0' <= c && c <= '9') {
        while ('0' <= lexer->lookahead && lexer->lookahead <= '9') advance(lexer);
        dot = NUMBER_DOT, paren = NUMBER_PAREN;
    } else {
        return NOT_A_BULLET;
    }
    Bullet b = lexer->lookahead == '.' ? dot : lexer->lookahead == ')' ? paren : NOT_A_BULLET;
    if (b == NOT_A_BULLET) return NOT_A_BULLET;
    advance(lexer);
    return iswspace(lexer->lookahead) ? b : NOT_A_BULLET;
}

// Reads `#+begin_` or `#+end_` (any case), telling which.
static bool read_block_marker(TSLexer *lexer, bool *begin) {
    advance(lexer);
    if (lexer->lookahead != '+') return false;
    advance(lexer);
    const char *word = towlower(lexer->lookahead) == 'b' ? "begin_" : "end_";
    *begin = word[0] == 'b';
    for (const char *c = word; *c; c++) {
        if (towlower(lexer->lookahead) != (wint_t)*c) return false;
        advance(lexer);
    }
    return true;
}

// Looks ahead for a line `#+end_NAME` (any case, after any indentation)
// closing the block whose `#+begin_NAME` line the lexer is on.
static bool find_block_end(TSLexer *lexer, const char *name, unsigned length) {
    for (;;) {
        while (lexer->lookahead != '\n' && !lexer->eof(lexer)) advance(lexer);
        if (lexer->eof(lexer)) return false;
        advance(lexer);
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') advance(lexer);
        if (lexer->lookahead != '#') continue;
        bool begin = false;
        if (!read_block_marker(lexer, &begin) || begin) continue;
        unsigned i = 0;
        while (i < length && !lexer->eof(lexer) && (char)towlower(lexer->lookahead) == name[i]) {
            advance(lexer);
            i++;
        }
        if (i == length && (iswspace(lexer->lookahead) || lexer->eof(lexer))) return true;
    }
}

// After `[[` (the token's end marked): a link, if a `]]` after some text
// closes it on this line.
static bool scan_link(TSLexer *lexer) {
    bool content = false;
    while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        advance(lexer);
        if (c == ']' && lexer->lookahead == ']') {
            if (!content) return false;
            lexer->result_symbol = LINK_OPEN;
            return true;
        }
        content = true;
    }
    return false;
}

// Looks along the rest of the line for `first` followed by `second`.
static bool find_on_line(TSLexer *lexer, int32_t first, int32_t second) {
    while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
        int32_t c = lexer->lookahead;
        advance(lexer);
        if (c == first && lexer->lookahead == second) return true;
    }
    return false;
}

static bool end_list(Scanner *s, TSLexer *lexer) {
    array_pop(&s->indents);
    array_pop(&s->bullets);
    lexer->result_symbol = LIST_END;
    return true;
}

static int16_t add_indent(int16_t indent, int32_t c) {
    int32_t width = c == '\t' ? 8 : 1;
    return indent > INT16_MAX - width ? INT16_MAX : (int16_t)(indent + width);
}

bool tree_sitter_org_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = payload;
    if (valid_symbols[ERROR_SENTINEL]) return false;

    // Zero-width tokens end here, before the line's indentation.
    lexer->mark_end(lexer);
    int16_t indent = 0;
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        indent = add_indent(indent, lexer->lookahead);
        skip(lexer);
    }
    if (lexer->eof(lexer)) {
        if (valid_symbols[LIST_END]) return end_list(s, lexer);
        if (valid_symbols[SECTION_END]) {
            lexer->result_symbol = SECTION_END;
            return true;
        }
        if (valid_symbols[END_OF_FILE]) {
            lexer->result_symbol = END_OF_FILE;
            return true;
        }
        return false;
    }

    // Where a list item could end: past blank lines, the next line's
    // indentation and first characters decide.
    int newlines = 0;
    if (valid_symbols[LIST_END] || valid_symbols[LIST_ITEM_END]) {
        for (;;) {
            if (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                indent = add_indent(indent, lexer->lookahead);
            } else if (lexer->eof(lexer)) {
                return end_list(s, lexer);
            } else if (lexer->lookahead == '\n') {
                if (++newlines > 1) return end_list(s, lexer);
                indent = 0;
            } else {
                break;
            }
            skip(lexer);
        }
        int16_t top = *array_back(&s->indents);
        if (indent < top) return end_list(s, lexer);
        if (indent == top) {
            if (read_bullet(lexer) == *array_back(&s->bullets)) {
                lexer->result_symbol = LIST_ITEM_END;
                return true;
            }
            return end_list(s, lexer);
        }
    }

    // `#+begin_NAME` opening a block, or `#+end_NAME` closing the open one.
    if ((valid_symbols[BLOCK_BEGIN] || valid_symbols[BLOCK_END]) && lexer->lookahead == '#') {
        lexer->mark_end(lexer);
        bool begin = false;
        if (!read_block_marker(lexer, &begin)) return false;
        char name[MAX_BLOCK_NAME];
        unsigned length = 0;
        while (!iswspace(lexer->lookahead) && !lexer->eof(lexer)) {
            if (length < MAX_BLOCK_NAME) name[length] = (char)towlower(lexer->lookahead);
            length++;
            advance(lexer);
        }
        if (length == 0) return false;
        if (length > MAX_BLOCK_NAME) length = MAX_BLOCK_NAME;
        if (begin && valid_symbols[BLOCK_BEGIN]) {
            memcpy(s->block_name, name, length);
            s->block_name_length = (uint8_t)length;
            s->block_any_end = !find_block_end(lexer, name, length);
            lexer->result_symbol = BLOCK_BEGIN;
            return true;
        }
        if (!begin && valid_symbols[BLOCK_END] &&
            (s->block_any_end || (length == s->block_name_length && memcmp(name, s->block_name, length) == 0))) {
            s->block_name_length = 0;
            s->block_any_end = false;
            lexer->result_symbol = BLOCK_END;
            return true;
        }
        return false;
    }

    // A drawer's first line: `:NAME:` and nothing else on the line.
    if ((valid_symbols[DRAWER_START] || valid_symbols[PROPERTY_DRAWER_START]) && lexer->lookahead == ':') {
        lexer->mark_end(lexer);
        advance(lexer);
        char name[11];
        unsigned length = 0;
        while (iswalnum(lexer->lookahead) || lexer->lookahead == '_' || lexer->lookahead == '-') {
            if (length < sizeof name) name[length] = (char)towlower(lexer->lookahead);
            length++;
            advance(lexer);
        }
        if (length == 0 || lexer->lookahead != ':') return false;
        if (length == 3 && memcmp(name, "end", 3) == 0) return false;
        bool properties = length == 10 && memcmp(name, "properties", 10) == 0;
        advance(lexer);
        while (lexer->lookahead == ' ' || lexer->lookahead == '\t') advance(lexer);
        if (lexer->lookahead != '\n' && lexer->lookahead != '\r' && !lexer->eof(lexer)) return false;
        if (properties && valid_symbols[PROPERTY_DRAWER_START]) {
            lexer->result_symbol = PROPERTY_DRAWER_START;
            return true;
        }
        if (!valid_symbols[DRAWER_START]) return false;
        lexer->result_symbol = DRAWER_START;
        return true;
    }

    // A headline: stars at column 0 and a blank after them.
    if (indent == 0 && lexer->lookahead == '*') {
        lexer->mark_end(lexer);
        unsigned stars = 0;
        while (lexer->lookahead == '*') {
            stars++;
            skip(lexer);
        }
        if (lexer->lookahead == '\n' || !iswspace(lexer->lookahead)) return false;
        if (valid_symbols[SECTION_END] && stars <= *array_back(&s->sections)) {
            array_pop(&s->sections);
            lexer->result_symbol = SECTION_END;
            return true;
        }
        if (valid_symbols[STARS]) {
            array_push(&s->sections, stars > 255 ? 255 : (uint8_t)stars);
            lexer->result_symbol = STARS;
            return true;
        }
        return false;
    }

    // A bullet: the next item of the open list, or a deeper list's first.
    if ((valid_symbols[LIST_START] || valid_symbols[BULLET]) && newlines == 0) {
        Bullet bullet = read_bullet(lexer);
        if (valid_symbols[BULLET] && bullet == *array_back(&s->bullets) && indent == *array_back(&s->indents)) {
            lexer->mark_end(lexer);
            lexer->result_symbol = BULLET;
            return true;
        }
        if (valid_symbols[LIST_START] && bullet != NOT_A_BULLET && indent > *array_back(&s->indents)) {
            array_push(&s->indents, indent);
            array_push(&s->bullets, (uint8_t)bullet);
            lexer->result_symbol = LIST_START;
            return true;
        }
    }

    // `\(` or `\[`, if its closer follows on the line.
    if (valid_symbols[MATH_START] && lexer->lookahead == '\\') {
        lexer->mark_end(lexer);
        advance(lexer);
        int32_t close = lexer->lookahead == '(' ? ')' : lexer->lookahead == '[' ? ']' : 0;
        if (!close) return false;
        advance(lexer);
        if (!find_on_line(lexer, '\\', close)) return false;
        lexer->result_symbol = MATH_START;
        return true;
    }

    // `[fn:`, if a `]` closes it before a blank line.
    if (valid_symbols[FOOTNOTE_START] && lexer->lookahead == '[') {
        lexer->mark_end(lexer);
        advance(lexer);
        if (towlower(lexer->lookahead) == 'f') {
            advance(lexer);
            if (towlower(lexer->lookahead) != 'n') return false;
            advance(lexer);
            if (lexer->lookahead != ':') return false;
            advance(lexer);
            int newlines = 0;
            while (!lexer->eof(lexer)) {
                if (lexer->lookahead == ']') {
                    lexer->result_symbol = FOOTNOTE_START;
                    return true;
                }
                if (lexer->lookahead == '\n') {
                    if (++newlines > 1) return false;
                } else if (!iswspace(lexer->lookahead)) {
                    newlines = 0;
                }
                advance(lexer);
            }
            return false;
        }
        if (lexer->lookahead != '[' || !valid_symbols[LINK_OPEN]) return false;
        // Not a footnote: a link's `[[`, already one character in.
        advance(lexer);
        lexer->mark_end(lexer);
        return scan_link(lexer);
    }

    // `[[`, if a `]]` after some text closes it on this line.
    if (valid_symbols[LINK_OPEN] && lexer->lookahead == '[') {
        advance(lexer);
        if (lexer->lookahead != '[') return false;
        advance(lexer);
        lexer->mark_end(lexer);
        return scan_link(lexer);
    }

    // `$`: closes open math; opens math when a closing `$` follows on the
    // line, not inside a word (`$5 and $x` is text). `$$` opens display
    // math when another `$$` follows on the line (the grammar's tokens).
    if (lexer->lookahead == '$') {
        if (valid_symbols[MATH_START]) lexer->mark_end(lexer);
        advance(lexer);
        if (lexer->lookahead == '$' && valid_symbols[MATH_START]) {
            advance(lexer);
            if (!find_on_line(lexer, '$', '$')) return false;
            lexer->result_symbol = MATH_START;
            return true;
        }
        lexer->mark_end(lexer);
        if (!valid_symbols[LATEX_MATH_SINGLE_DOLLAR]) return false;
        if (s->in_dollar_math) {
            s->in_dollar_math = false;
            lexer->result_symbol = LATEX_MATH_SINGLE_DOLLAR;
            return true;
        }
        if (lexer->lookahead == '$') return false;
        while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
            advance(lexer);
            if (lexer->lookahead == '$') {
                advance(lexer);
                if (is_alnum_ascii(lexer->lookahead)) {
                    if (valid_symbols[TEXT_DOLLAR]) {
                        lexer->result_symbol = TEXT_DOLLAR;
                        return true;
                    }
                } else {
                    s->in_dollar_math = true;
                    lexer->result_symbol = LATEX_MATH_SINGLE_DOLLAR;
                    return true;
                }
            }
        }
        if (valid_symbols[TEXT_DOLLAR]) {
            lexer->result_symbol = TEXT_DOLLAR;
            return true;
        }
    }
    return false;
}
