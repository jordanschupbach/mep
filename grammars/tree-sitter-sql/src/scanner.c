// External scanner for mep's SQL grammar: PostgreSQL's dollar-quoted
// strings (`$$ ... $$`, `$tag$ ... $tag$`), and sqlite3's dot-commands.
//
// Rewritten for mep from DerekStride/tree-sitter-sql 0.3.11's scanner.c
// (MIT, Copyright (c) 2021 Derek Stride), keeping its three tokens and when
// each is produced:
//
//   _dollar_quoted_string_start_tag / _end_tag: a function body's opening
//       and closing tag, between which the grammar parses the body as SQL;
//   _dollar_quoted_string: a whole dollar-quoted string, as one token.
//
// What changed: the open tag lives in a fixed buffer in the state and is
// serialized as its bytes (the original freed it while serializing, so
// tree-sitter's snapshots dropped it, leaked it on every deserialize, and
// malloc'd per character); a string's end is found as PostgreSQL finds it,
// at the first occurrence of the closing tag (the original consumed any
// `$...$` run, so `$$x$y$$` missed its end); and the entry points are
// prototyped (UBSan's -fsanitize=function rejected `create()`).
//
// mep's addition: sqlite_dot_command, an extra -- a `.` and a letter in
// the first column runs to the end of the line (`.mode column`,
// `.headers on`), as the sqlite3 shell reads it. Only the scanner can see
// the column, and a `.` anywhere else is SQL's (`t.col`, `.5`).
// A tag is `$`, then anything but `$` and whitespace, then `$`, as before;
// one longer than kMaxTag bytes is not a tag.

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    DOLLAR_QUOTED_STRING_START_TAG,
    DOLLAR_QUOTED_STRING_END_TAG,
    DOLLAR_QUOTED_STRING,
    SQLITE_DOT_COMMAND,
};

// Fits TREE_SITTER_SERIALIZATION_BUFFER_SIZE (1024) with room to spare.
enum { kMaxTag = 255 };

typedef struct {
    // The open function-body tag, `$tag$` (0 bytes: none open).
    unsigned length;
    char tag[kMaxTag];
} Scanner;

typedef struct {
    unsigned length;
    bool too_long;
    char text[kMaxTag];
} Tag;

void *tree_sitter_sql_external_scanner_create(void) {
    Scanner *s = (Scanner *)ts_calloc(1, sizeof(Scanner));
    return s;
}

void tree_sitter_sql_external_scanner_destroy(void *payload) { ts_free(payload); }

unsigned tree_sitter_sql_external_scanner_serialize(void *payload, char *buffer) {
    const Scanner *s = (const Scanner *)payload;
    memcpy(buffer, s->tag, s->length);
    return s->length;
}

void tree_sitter_sql_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    s->length = 0;
    if (length > 0 && length <= kMaxTag) {
        memcpy(s->tag, buffer, length);
        s->length = length;
    }
}

static void push(Tag *t, int32_t c) {
    // Tags are compared bytewise; a non-ASCII character is stored as its
    // code point's low byte plus a marker, enough to tell tags apart.
    if (c >= 0x80) {
        if (t->length + 2 > kMaxTag) {
            t->too_long = true;
            return;
        }
        t->text[t->length++] = (char)0xff;
        t->text[t->length++] = (char)(c & 0xff);
        return;
    }
    if (t->length + 1 > kMaxTag) {
        t->too_long = true;
        return;
    }
    t->text[t->length++] = (char)c;
}

// Reads `$...$` at the lookahead, consuming it (and, when it is not a tag,
// what was read of it). True when a whole tag was read.
static bool scan_tag(TSLexer *lexer, Tag *t) {
    t->length = 0;
    t->too_long = false;
    if (lexer->lookahead != '$') return false;
    push(t, '$');
    lexer->advance(lexer, false);
    while (lexer->lookahead != '$' && !iswspace((wint_t)lexer->lookahead) && !lexer->eof(lexer)) {
        push(t, lexer->lookahead);
        lexer->advance(lexer, false);
    }
    if (lexer->lookahead != '$') return false;
    push(t, '$');
    lexer->advance(lexer, false);
    return !t->too_long;
}

static bool same_tag(const Tag *t, const char *text, unsigned length) {
    return t->length == length && memcmp(t->text, text, length) == 0;
}

// The tag's bytes for one character, for matching the body against it.
static unsigned char_bytes(int32_t c, char out[2]) {
    if (c >= 0x80) {
        out[0] = (char)0xff;
        out[1] = (char)(c & 0xff);
        return 2;
    }
    out[0] = (char)c;
    return 1;
}

// Consumes the body up to and including the closing `tag`. A tag has no
// `$` inside, so every occurrence of it starts at a `$`: a partial match
// that fails restarts at the failing character when that is a `$`.
static bool scan_body(TSLexer *lexer, const Tag *tag) {
    unsigned matched = 0;  // bytes of tag matched so far
    while (!lexer->eof(lexer)) {
        if (matched == 0) {
            if (lexer->lookahead == '$') matched = 1;
            lexer->advance(lexer, false);
            if (matched == tag->length) return true;
            continue;
        }
        char bytes[2];
        unsigned n = char_bytes(lexer->lookahead, bytes);
        if (matched + n <= tag->length && memcmp(tag->text + matched, bytes, n) == 0) {
            matched += n;
            lexer->advance(lexer, false);
            if (matched == tag->length) return true;
        } else {
            // Not this occurrence; a `$` here may start the next one.
            matched = 0;
        }
    }
    return false;
}

bool tree_sitter_sql_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = (Scanner *)payload;
    Tag t;

    if (valid_symbols[SQLITE_DOT_COMMAND]) {
        while (iswspace((wint_t)lexer->lookahead)) lexer->advance(lexer, true);
        if (lexer->lookahead == '.' && lexer->get_column(lexer) == 0) {
            lexer->advance(lexer, false);
            if (!iswalpha((wint_t)lexer->lookahead)) return false;
            while (lexer->lookahead != '\n' && !lexer->eof(lexer)) lexer->advance(lexer, false);
            lexer->result_symbol = SQLITE_DOT_COMMAND;
            return true;
        }
    }

    if (valid_symbols[DOLLAR_QUOTED_STRING_START_TAG] && s->length == 0) {
        while (iswspace((wint_t)lexer->lookahead)) lexer->advance(lexer, true);
        if (!scan_tag(lexer, &t)) return false;
        memcpy(s->tag, t.text, t.length);
        s->length = t.length;
        lexer->result_symbol = DOLLAR_QUOTED_STRING_START_TAG;
        return true;
    }

    if (valid_symbols[DOLLAR_QUOTED_STRING_END_TAG] && s->length != 0) {
        while (iswspace((wint_t)lexer->lookahead)) lexer->advance(lexer, true);
        if (!scan_tag(lexer, &t) || !same_tag(&t, s->tag, s->length)) return false;
        s->length = 0;
        lexer->result_symbol = DOLLAR_QUOTED_STRING_END_TAG;
        return true;
    }

    if (valid_symbols[DOLLAR_QUOTED_STRING]) {
        while (iswspace((wint_t)lexer->lookahead)) lexer->advance(lexer, true);
        if (!scan_tag(lexer, &t)) return false;
        // Inside a function body opened by this same tag, it is the close.
        if (s->length != 0 && same_tag(&t, s->tag, s->length)) return false;
        if (!scan_body(lexer, &t)) return false;
        lexer->result_symbol = DOLLAR_QUOTED_STRING;
        return true;
    }

    return false;
}
