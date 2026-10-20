// The external scanner for mep's Markdown block grammar: CommonMark's block
// structure (spec.commonmark.org, "Appendix: A parsing strategy"), which
// depends on what earlier lines opened.
//
// The scanner keeps the stack of open container blocks -- block quotes,
// list items (with the column their content starts at), indented and fenced
// code, and the HTML blocks -- and at every line's start "matches" them in
// order: a block quote by its `>`, a list item or indented code by enough
// indentation. Each matched block is a `block_continuation`; the first one
// that does not match is closed (`_block_close`), unless the previous line
// break was inside a paragraph, which a "lazy" line continues.
//
// A line ending is the other decision: inside a paragraph, it is soft if
// the next line, once its containers are matched, would not start a block
// that interrupts a paragraph. The scanner answers that by scanning ahead in
// "simulate" mode, which reads without moving the token's end or changing
// the stack.
//
// Tabs count to the next multiple of four columns (§2.2). A tab can belong
// to two tokens (the end of a list marker and the start of indented code),
// so the unused part of consumed indentation is carried in `indentation`.
//
// State, saved with every token: the flags below, how many open blocks the
// current line has matched, the carried indentation and column, the open
// fence's length, and the stack itself (one byte per block).

#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <string.h>
#include <wctype.h>

// In grammar.js's `externals` order.
typedef enum {
    LINE_ENDING,
    SOFT_LINE_ENDING,
    BLOCK_CLOSE,
    BLOCK_CONTINUATION,
    BLOCK_QUOTE_START,
    INDENTED_CHUNK_START,
    ATX_H1_MARKER,
    ATX_H2_MARKER,
    ATX_H3_MARKER,
    ATX_H4_MARKER,
    ATX_H5_MARKER,
    ATX_H6_MARKER,
    SETEXT_H1_UNDERLINE,
    SETEXT_H2_UNDERLINE,
    THEMATIC_BREAK,
    LIST_MARKER_MINUS,
    LIST_MARKER_PLUS,
    LIST_MARKER_STAR,
    LIST_MARKER_PARENTHESIS,
    LIST_MARKER_DOT,
    LIST_MARKER_MINUS_DONT_INTERRUPT,
    LIST_MARKER_PLUS_DONT_INTERRUPT,
    LIST_MARKER_STAR_DONT_INTERRUPT,
    LIST_MARKER_PARENTHESIS_DONT_INTERRUPT,
    LIST_MARKER_DOT_DONT_INTERRUPT,
    FENCED_CODE_BLOCK_START_BACKTICK,
    FENCED_CODE_BLOCK_START_TILDE,
    BLANK_LINE_START,
    FENCED_CODE_BLOCK_END_BACKTICK,
    FENCED_CODE_BLOCK_END_TILDE,
    HTML_BLOCK_1_START,
    HTML_BLOCK_1_END,
    HTML_BLOCK_2_START,
    HTML_BLOCK_3_START,
    HTML_BLOCK_4_START,
    HTML_BLOCK_5_START,
    HTML_BLOCK_6_START,
    HTML_BLOCK_7_START,
    CLOSE_BLOCK,
    NO_INDENTED_CHUNK,
    ERROR,
    TRIGGER_ERROR,
    TOKEN_EOF,
    MINUS_METADATA,
    PLUS_METADATA,
    PIPE_TABLE_START,
    PIPE_TABLE_LINE_ENDING,
    TOKEN_COUNT,
} TokenType;

// An open block. A list item's content starts some columns in: LIST_ITEM
// is two (`- x`), each next value one more, up to LIST_ITEM_MAX (a nine
// digit marker and four spaces). ANONYMOUS is a block the grammar closes
// (an HTML block): it always matches.
typedef enum {
    BLOCK_QUOTE,
    INDENTED_CODE_BLOCK,
    LIST_ITEM,
    LIST_ITEM_MAX = LIST_ITEM + 15,
    FENCED_CODE_BLOCK,
    ANONYMOUS,
} Block;

static bool is_list_item(uint8_t block) { return block >= LIST_ITEM && block <= LIST_ITEM_MAX; }

// The column a list item's content starts at.
static uint8_t list_item_indentation(uint8_t block) { return (uint8_t)(block - LIST_ITEM + 2); }

// §4.6 start condition 1 and 6's tag names.
static const char *const HTML_BLOCK_1_TAGS[] = {"pre", "script", "style", "textarea"};
static const char *const HTML_BLOCK_6_TAGS[] = {
    "address",  "article",  "aside",    "base",     "basefont", "blockquote", "body",     "caption", "center",
    "col",      "colgroup", "dd",       "details",  "dialog",   "dir",        "div",      "dl",      "dt",
    "fieldset", "figcaption", "figure", "footer",   "form",     "frame",      "frameset", "h1",      "h2",
    "h3",       "h4",       "h5",       "h6",       "head",     "header",     "hr",       "html",    "iframe",
    "legend",   "li",       "link",     "main",     "menu",     "menuitem",   "nav",      "noframes", "ol",
    "optgroup", "option",   "p",        "param",    "section",  "source",     "summary",  "table",   "tbody",
    "td",       "tfoot",    "th",       "thead",    "title",    "tr",         "track",    "ul",
};

// The block starts that may interrupt a paragraph (§4.8): what a line must
// start with for the line ending before it to be a hard one.
static const bool INTERRUPTS_PARAGRAPH[TOKEN_COUNT] = {
    [BLOCK_QUOTE_START] = true,
    [ATX_H1_MARKER] = true,
    [ATX_H2_MARKER] = true,
    [ATX_H3_MARKER] = true,
    [ATX_H4_MARKER] = true,
    [ATX_H5_MARKER] = true,
    [ATX_H6_MARKER] = true,
    [SETEXT_H1_UNDERLINE] = true,
    [SETEXT_H2_UNDERLINE] = true,
    [THEMATIC_BREAK] = true,
    [LIST_MARKER_MINUS] = true,
    [LIST_MARKER_PLUS] = true,
    [LIST_MARKER_STAR] = true,
    [LIST_MARKER_PARENTHESIS] = true,
    [LIST_MARKER_DOT] = true,
    [FENCED_CODE_BLOCK_START_BACKTICK] = true,
    [FENCED_CODE_BLOCK_START_TILDE] = true,
    [BLANK_LINE_START] = true,
    [HTML_BLOCK_1_START] = true,
    [HTML_BLOCK_2_START] = true,
    [HTML_BLOCK_3_START] = true,
    [HTML_BLOCK_4_START] = true,
    [HTML_BLOCK_5_START] = true,
    [HTML_BLOCK_6_START] = true,
    [PIPE_TABLE_START] = true,
};

// At a line's start, still matching the open blocks.
#define STATE_MATCHING 0x01
// The last line ending was soft (inside a paragraph): lazy continuation.
#define STATE_WAS_SOFT_LINE_BREAK 0x02
// Close the innermost block at the next line's start (`_close_block`).
#define STATE_CLOSE_BLOCK 0x10

typedef struct {
    Array(uint8_t) blocks;
    uint8_t state;
    // How many open blocks this line has matched so far.
    uint8_t matched;
    // Indentation consumed but not yet used by a token.
    uint8_t indentation;
    // The column modulo 4, for tab stops.
    uint8_t column;
    // The open fenced code block's fence length.
    uint8_t fence_length;
    // Reading ahead: no token ends here and the stack does not change.
    bool simulate;
} Scanner;

void *tree_sitter_markdown_external_scanner_create(void) {
    Scanner *s = ts_calloc(1, sizeof(Scanner));
    array_init(&s->blocks);
    return s;
}

void tree_sitter_markdown_external_scanner_destroy(void *payload) {
    Scanner *s = payload;
    array_delete(&s->blocks);
    ts_free(s);
}

#define HEADER_SIZE 5

unsigned tree_sitter_markdown_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = payload;
    buffer[0] = (char)s->state;
    buffer[1] = (char)s->matched;
    buffer[2] = (char)s->indentation;
    buffer[3] = (char)s->column;
    buffer[4] = (char)s->fence_length;
    // A stack deeper than the buffer keeps its outermost blocks: nesting
    // past a thousand levels is not a document anyone writes.
    unsigned count = s->blocks.size;
    if (count > TREE_SITTER_SERIALIZATION_BUFFER_SIZE - HEADER_SIZE) {
        count = TREE_SITTER_SERIALIZATION_BUFFER_SIZE - HEADER_SIZE;
    }
    if (count > 0) memcpy(buffer + HEADER_SIZE, s->blocks.contents, count);
    return HEADER_SIZE + count;
}

void tree_sitter_markdown_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = payload;
    array_clear(&s->blocks);
    s->state = s->matched = s->indentation = s->column = s->fence_length = 0;
    if (length < HEADER_SIZE) return;
    s->state = (uint8_t)buffer[0];
    s->matched = (uint8_t)buffer[1];
    s->indentation = (uint8_t)buffer[2];
    s->column = (uint8_t)buffer[3];
    s->fence_length = (uint8_t)buffer[4];
    unsigned count = length - HEADER_SIZE;
    array_reserve(&s->blocks, count);
    for (unsigned i = 0; i < count; i++) array_push(&s->blocks, (uint8_t)buffer[HEADER_SIZE + i]);
}

static void push_block(Scanner *s, uint8_t block) {
    if (!s->simulate) array_push(&s->blocks, block);
}

static void mark_end(Scanner *s, TSLexer *lexer) {
    if (!s->simulate) lexer->mark_end(lexer);
}

// Advances one character, keeping the column; returns the columns it
// spans (a tab: to the next tab stop).
static uint8_t advance(Scanner *s, TSLexer *lexer) {
    uint8_t width = 1;
    if (lexer->lookahead == '\t') {
        width = (uint8_t)(4 - s->column);
        s->column = 0;
    } else {
        s->column = (uint8_t)((s->column + 1) % 4);
    }
    lexer->advance(lexer, false);
    return width;
}

static bool is_blank(int32_t c) { return c == ' ' || c == '\t'; }
static bool is_line_end(int32_t c) { return c == '\n' || c == '\r'; }

static bool is_ascii_punctuation(int32_t c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

static void add_indentation(Scanner *s, uint8_t width) {
    s->indentation = s->indentation > 255 - width ? 255 : (uint8_t)(s->indentation + width);
}

// Consumes `\n`, `\r` or `\r\n`.
static void advance_line_end(Scanner *s, TSLexer *lexer) {
    if (lexer->lookahead == '\r') {
        advance(s, lexer);
        if (lexer->lookahead == '\n') advance(s, lexer);
    } else {
        advance(s, lexer);
    }
}

// Matches one open block at the current line's start: consumes what
// belongs to it (indentation, a `>`) and reports whether it continues.
static bool match(Scanner *s, TSLexer *lexer, uint8_t block) {
    if (block == INDENTED_CODE_BLOCK) {
        while (s->indentation < 4 && is_blank(lexer->lookahead)) add_indentation(s, advance(s, lexer));
        if (s->indentation >= 4 && !is_line_end(lexer->lookahead)) {
            s->indentation -= 4;
            return true;
        }
        return false;
    }
    if (is_list_item(block)) {
        const uint8_t need = list_item_indentation(block);
        while (s->indentation < need && is_blank(lexer->lookahead)) add_indentation(s, advance(s, lexer));
        if (s->indentation >= need) {
            s->indentation -= need;
            return true;
        }
        // A blank line stays inside the item.
        if (is_line_end(lexer->lookahead)) {
            s->indentation = 0;
            return true;
        }
        return false;
    }
    if (block == BLOCK_QUOTE) {
        while (is_blank(lexer->lookahead)) add_indentation(s, advance(s, lexer));
        if (lexer->lookahead != '>') return false;
        advance(s, lexer);
        s->indentation = 0;
        // One blank after the `>` belongs to it (part of a tab, maybe).
        if (is_blank(lexer->lookahead)) add_indentation(s, (uint8_t)(advance(s, lexer) - 1));
        return true;
    }
    return true;  // FENCED_CODE_BLOCK, ANONYMOUS
}

// A run of backticks or tildes: a fence that closes the open one, or opens
// a new one (§4.5).
static bool scan_fence(Scanner *s, TSLexer *lexer, const bool *valid, int32_t fence) {
    const bool backtick = fence == '`';
    uint8_t length = 0;
    while (lexer->lookahead == fence) {
        advance(s, lexer);
        if (length < 255) length++;
    }
    mark_end(s, lexer);
    // As long as the opening fence, not indented four, nothing after it:
    // the only reading is the close.
    if (valid[backtick ? FENCED_CODE_BLOCK_END_BACKTICK : FENCED_CODE_BLOCK_END_TILDE] && s->indentation < 4 &&
        length >= s->fence_length) {
        while (is_blank(lexer->lookahead)) advance(s, lexer);
        if (is_line_end(lexer->lookahead)) {
            s->fence_length = 0;
            lexer->result_symbol = backtick ? FENCED_CODE_BLOCK_END_BACKTICK : FENCED_CODE_BLOCK_END_TILDE;
            return true;
        }
    }
    // An opening fence: three or more, and a backtick fence's info string
    // has no backtick.
    if (valid[backtick ? FENCED_CODE_BLOCK_START_BACKTICK : FENCED_CODE_BLOCK_START_TILDE] && length >= 3) {
        if (backtick) {
            while (!is_line_end(lexer->lookahead) && !lexer->eof(lexer)) {
                if (lexer->lookahead == '`') return false;
                advance(s, lexer);
            }
        }
        lexer->result_symbol = backtick ? FENCED_CODE_BLOCK_START_BACKTICK : FENCED_CODE_BLOCK_START_TILDE;
        push_block(s, FENCED_CODE_BLOCK);
        s->fence_length = length;
        s->indentation = 0;
        return true;
    }
    return false;
}

// After a bullet or ordered marker and the blanks after it: the item's
// content column, pushed as the item. `after` is the blanks' width (one
// for an empty item), `marker` the marker's own width past the first
// column. Four or more blanks start indented code inside the item, so the
// item's content begins one blank after the marker and the rest is kept.
static void open_list_item(Scanner *s, uint8_t after, uint8_t marker) {
    after--;
    uint8_t content;
    if (after <= 3) {
        content = (uint8_t)(after + s->indentation);
        s->indentation = 0;
    } else {
        content = s->indentation;
        s->indentation = after;
    }
    uint32_t block = (uint32_t)LIST_ITEM + content + marker;
    push_block(s, (uint8_t)(block > LIST_ITEM_MAX ? LIST_ITEM_MAX : block));
}

// `*`: a thematic break or a bullet.
static bool scan_star(Scanner *s, TSLexer *lexer, const bool *valid) {
    advance(s, lexer);
    mark_end(s, lexer);
    unsigned stars = 1;
    uint8_t after = 0;  // blanks right after the first star
    for (;;) {
        if (lexer->lookahead == '*') {
            // `* *`: if this is a bullet, its token ends after the blanks.
            if (stars == 1 && after >= 1 && valid[LIST_MARKER_STAR]) mark_end(s, lexer);
            stars++;
            advance(s, lexer);
        } else if (is_blank(lexer->lookahead)) {
            uint8_t width = advance(s, lexer);
            if (stars == 1) after = (uint8_t)(after + width);
        } else {
            break;
        }
    }
    const bool line_end = is_line_end(lexer->lookahead);
    bool dont_interrupt = false;
    if (stars == 1 && line_end) {
        after = 1;
        // An empty item does not interrupt a paragraph.
        dont_interrupt = s->matched == s->blocks.size;
    }
    if (valid[THEMATIC_BREAK] && stars >= 3 && line_end && s->indentation < 4) {
        lexer->result_symbol = THEMATIC_BREAK;
        mark_end(s, lexer);
        s->indentation = 0;
        return true;
    }
    if (valid[dont_interrupt ? LIST_MARKER_STAR_DONT_INTERRUPT : LIST_MARKER_STAR] && after >= 1) {
        if (stars == 1) mark_end(s, lexer);
        open_list_item(s, after, 0);
        lexer->result_symbol = dont_interrupt ? LIST_MARKER_STAR_DONT_INTERRUPT : LIST_MARKER_STAR;
        return true;
    }
    return false;
}

// `_`: a thematic break.
static bool scan_underscore(Scanner *s, TSLexer *lexer, const bool *valid) {
    advance(s, lexer);
    mark_end(s, lexer);
    unsigned count = 1;
    for (;;) {
        if (lexer->lookahead == '_') {
            count++;
            advance(s, lexer);
        } else if (is_blank(lexer->lookahead)) {
            advance(s, lexer);
        } else {
            break;
        }
    }
    if (count >= 3 && is_line_end(lexer->lookahead) && valid[THEMATIC_BREAK]) {
        lexer->result_symbol = THEMATIC_BREAK;
        mark_end(s, lexer);
        s->indentation = 0;
        return true;
    }
    return false;
}

// `>`: a block quote opens (§5.1).
static bool scan_block_quote(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[BLOCK_QUOTE_START]) return false;
    advance(s, lexer);
    s->indentation = 0;
    if (is_blank(lexer->lookahead)) add_indentation(s, (uint8_t)(advance(s, lexer) - 1));
    lexer->result_symbol = BLOCK_QUOTE_START;
    push_block(s, BLOCK_QUOTE);
    return true;
}

// `#` to `######` and a blank or the line's end (§4.2).
static bool scan_atx_heading(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[ATX_H1_MARKER] || s->indentation > 3) return false;
    mark_end(s, lexer);
    unsigned level = 0;
    while (lexer->lookahead == '#' && level <= 6) {
        advance(s, lexer);
        level++;
    }
    if (level > 6 || !(is_blank(lexer->lookahead) || is_line_end(lexer->lookahead))) return false;
    lexer->result_symbol = (TokenType)(ATX_H1_MARKER + level - 1);
    s->indentation = 0;
    mark_end(s, lexer);
    return true;
}

// `===` alone on its line, under a paragraph in the same containers.
static bool scan_setext_h1(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!valid[SETEXT_H1_UNDERLINE] || s->matched != s->blocks.size) return false;
    mark_end(s, lexer);
    while (lexer->lookahead == '=') advance(s, lexer);
    while (is_blank(lexer->lookahead)) advance(s, lexer);
    if (!is_line_end(lexer->lookahead)) return false;
    lexer->result_symbol = SETEXT_H1_UNDERLINE;
    mark_end(s, lexer);
    return true;
}

// Front matter after a `---` or `+++` line: everything up to the same line
// again, newline included (the scanner is at the opening line's end).
static bool scan_metadata(Scanner *s, TSLexer *lexer, int32_t fence, TokenType token) {
    for (;;) {
        advance_line_end(s, lexer);
        unsigned count = 0;
        while (lexer->lookahead == fence) {
            count++;
            advance(s, lexer);
        }
        if (count == 3) {
            while (is_blank(lexer->lookahead)) advance(s, lexer);
            if (is_line_end(lexer->lookahead)) {
                advance_line_end(s, lexer);
                mark_end(s, lexer);
                lexer->result_symbol = token;
                return true;
            }
        }
        while (!is_line_end(lexer->lookahead) && !lexer->eof(lexer)) advance(s, lexer);
        if (lexer->eof(lexer)) return false;
    }
}

// `+`: a bullet, or TOML front matter.
static bool scan_plus(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (s->indentation > 3 ||
        !(valid[LIST_MARKER_PLUS] || valid[LIST_MARKER_PLUS_DONT_INTERRUPT] || valid[PLUS_METADATA])) {
        return false;
    }
    advance(s, lexer);
    if (valid[PLUS_METADATA] && lexer->lookahead == '+') {
        advance(s, lexer);
        if (lexer->lookahead != '+') return false;
        advance(s, lexer);
        while (is_blank(lexer->lookahead)) advance(s, lexer);
        if (!is_line_end(lexer->lookahead)) return false;
        return scan_metadata(s, lexer, '+', PLUS_METADATA);
    }
    uint8_t after = 0;
    while (is_blank(lexer->lookahead)) after = (uint8_t)(after + advance(s, lexer));
    bool dont_interrupt = false;
    if (is_line_end(lexer->lookahead)) {
        after = 1;
        dont_interrupt = true;
    }
    dont_interrupt = dont_interrupt && s->matched == s->blocks.size;
    if (after >= 1 && valid[dont_interrupt ? LIST_MARKER_PLUS_DONT_INTERRUPT : LIST_MARKER_PLUS]) {
        lexer->result_symbol = dont_interrupt ? LIST_MARKER_PLUS_DONT_INTERRUPT : LIST_MARKER_PLUS;
        open_list_item(s, after, 0);
        return true;
    }
    return false;
}

// `1.` or `1)`: an ordered list item (§5.2). Only one starting at 1, and
// not empty, may interrupt a paragraph.
static bool scan_ordered_marker(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (s->indentation > 3 || !(valid[LIST_MARKER_PARENTHESIS] || valid[LIST_MARKER_DOT] ||
                                valid[LIST_MARKER_PARENTHESIS_DONT_INTERRUPT] || valid[LIST_MARKER_DOT_DONT_INTERRUPT])) {
        return false;
    }
    uint8_t digits = 1;
    bool dont_interrupt = lexer->lookahead != '1';
    advance(s, lexer);
    while (lexer->lookahead >= '0' && lexer->lookahead <= '9') {
        dont_interrupt = true;
        if (digits < 255) digits++;
        advance(s, lexer);
    }
    if (digits > 9) return false;
    const bool dot = lexer->lookahead == '.';
    if (!dot && lexer->lookahead != ')') return false;
    advance(s, lexer);
    uint8_t after = 0;
    while (is_blank(lexer->lookahead)) after = (uint8_t)(after + advance(s, lexer));
    if (is_line_end(lexer->lookahead)) {
        after = 1;
        dont_interrupt = true;
    }
    dont_interrupt = dont_interrupt && s->matched == s->blocks.size;
    const TokenType token = dot ? (dont_interrupt ? LIST_MARKER_DOT_DONT_INTERRUPT : LIST_MARKER_DOT)
                                : (dont_interrupt ? LIST_MARKER_PARENTHESIS_DONT_INTERRUPT : LIST_MARKER_PARENTHESIS);
    if (after < 1 || !valid[token]) return false;
    lexer->result_symbol = dot ? LIST_MARKER_DOT : LIST_MARKER_PARENTHESIS;
    open_list_item(s, after, digits);
    return true;
}

// `-`: a setext underline, a thematic break, a bullet, or YAML front matter.
static bool scan_minus(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (s->indentation > 3 || !(valid[LIST_MARKER_MINUS] || valid[LIST_MARKER_MINUS_DONT_INTERRUPT] ||
                                valid[SETEXT_H2_UNDERLINE] || valid[THEMATIC_BREAK] || valid[MINUS_METADATA])) {
        return false;
    }
    mark_end(s, lexer);
    bool blank_after_minus = false;
    bool minus_after_blank = false;
    unsigned minuses = 0;
    uint8_t after = 0;  // blanks right after the first minus
    for (;;) {
        if (lexer->lookahead == '-') {
            // `- -`: if this is a bullet, its token ends after the blanks.
            if (minuses == 1 && after >= 1) mark_end(s, lexer);
            minuses++;
            advance(s, lexer);
            minus_after_blank = blank_after_minus;
        } else if (is_blank(lexer->lookahead)) {
            uint8_t width = advance(s, lexer);
            if (minuses == 1) after = (uint8_t)(after + width);
            blank_after_minus = true;
        } else {
            break;
        }
    }
    const bool line_end = is_line_end(lexer->lookahead);
    bool dont_interrupt = false;
    if (minuses == 1 && line_end) {
        after = 1;
        dont_interrupt = true;
    }
    dont_interrupt = dont_interrupt && s->matched == s->blocks.size;
    // A setext underline cannot break a lazy continuation line.
    const bool underline = minuses >= 1 && !minus_after_blank && line_end && s->matched == s->blocks.size;
    // An underline or a break is the answer unless the `---` opens front
    // matter (at the document's start, closed by a later `---`).
    bool found = false;
    if (valid[SETEXT_H2_UNDERLINE] && underline) {
        lexer->result_symbol = SETEXT_H2_UNDERLINE;
        mark_end(s, lexer);
        s->indentation = 0;
        found = true;
    } else if (valid[THEMATIC_BREAK] && minuses >= 3 && line_end) {
        lexer->result_symbol = THEMATIC_BREAK;
        mark_end(s, lexer);
        s->indentation = 0;
        found = true;
    } else if (valid[dont_interrupt ? LIST_MARKER_MINUS_DONT_INTERRUPT : LIST_MARKER_MINUS] && minuses >= 1 &&
               after >= 1) {
        if (minuses == 1) mark_end(s, lexer);
        open_list_item(s, after, 0);
        lexer->result_symbol = dont_interrupt ? LIST_MARKER_MINUS_DONT_INTERRUPT : LIST_MARKER_MINUS;
        return true;
    }
    if (minuses == 3 && !minus_after_blank && line_end && valid[MINUS_METADATA]) {
        const TSSymbol before = lexer->result_symbol;
        if (scan_metadata(s, lexer, '-', MINUS_METADATA)) return true;
        lexer->result_symbol = before;
    }
    return found;
}

static bool in_list(const char *name, const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

// Reads `!--`, `?`, `![CDATA[` or `!` and a letter (any case, as of
// CommonMark 0.31: `<!doctype html>`) after a `<` (start conditions 2-5).
static bool scan_html_special(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (lexer->lookahead == '?' && valid[HTML_BLOCK_3_START]) {
        advance(s, lexer);
        lexer->result_symbol = HTML_BLOCK_3_START;
        push_block(s, ANONYMOUS);
        return true;
    }
    if (lexer->lookahead != '!') return false;
    advance(s, lexer);
    if (lexer->lookahead == '-') {
        advance(s, lexer);
        if (lexer->lookahead == '-' && valid[HTML_BLOCK_2_START]) {
            advance(s, lexer);
            lexer->result_symbol = HTML_BLOCK_2_START;
            push_block(s, ANONYMOUS);
            return true;
        }
    } else if (((lexer->lookahead >= 'A' && lexer->lookahead <= 'Z') || (lexer->lookahead >= 'a' && lexer->lookahead <= 'z')) &&
               valid[HTML_BLOCK_4_START]) {
        advance(s, lexer);
        lexer->result_symbol = HTML_BLOCK_4_START;
        push_block(s, ANONYMOUS);
        return true;
    } else if (lexer->lookahead == '[') {
        advance(s, lexer);
        for (const char *c = "CDATA["; *c; c++) {
            if (lexer->lookahead != *c) return false;
            advance(s, lexer);
        }
        if (valid[HTML_BLOCK_5_START]) {
            lexer->result_symbol = HTML_BLOCK_5_START;
            push_block(s, ANONYMOUS);
            return true;
        }
    }
    return false;
}

// A complete open or closing tag, then only blanks to the line's end
// (start condition 7). The tag's name has been read.
static bool scan_html_complete_tag(Scanner *s, TSLexer *lexer, bool closing) {
    while (iswalnum((wint_t)lexer->lookahead) || lexer->lookahead == '-') advance(s, lexer);
    if (!closing) {
        bool had_blank = false;
        for (;;) {
            while (is_blank(lexer->lookahead)) {
                had_blank = true;
                advance(s, lexer);
            }
            if (lexer->lookahead == '/') {
                advance(s, lexer);
                break;
            }
            if (lexer->lookahead == '>') break;
            // An attribute: a name after a blank...
            if (!had_blank) return false;
            if (!iswalpha((wint_t)lexer->lookahead) && lexer->lookahead != '_' && lexer->lookahead != ':') return false;
            had_blank = false;
            advance(s, lexer);
            while (iswalnum((wint_t)lexer->lookahead) || lexer->lookahead == '_' || lexer->lookahead == '.' ||
                   lexer->lookahead == ':' || lexer->lookahead == '-') {
                advance(s, lexer);
            }
            while (is_blank(lexer->lookahead)) {
                had_blank = true;
                advance(s, lexer);
            }
            // ...and maybe `= value`.
            if (lexer->lookahead == '=') {
                advance(s, lexer);
                had_blank = false;
                while (is_blank(lexer->lookahead)) advance(s, lexer);
                if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
                    const int32_t quote = lexer->lookahead;
                    advance(s, lexer);
                    while (lexer->lookahead != quote && !is_line_end(lexer->lookahead) && !lexer->eof(lexer)) {
                        advance(s, lexer);
                    }
                    if (lexer->lookahead != quote) return false;
                    advance(s, lexer);
                } else {
                    bool any = false;
                    while (!is_blank(lexer->lookahead) && lexer->lookahead != '"' && lexer->lookahead != '\'' &&
                           lexer->lookahead != '=' && lexer->lookahead != '<' && lexer->lookahead != '>' &&
                           lexer->lookahead != '`' && !is_line_end(lexer->lookahead) && !lexer->eof(lexer)) {
                        advance(s, lexer);
                        any = true;
                    }
                    if (!any) return false;
                }
            }
        }
    } else {
        while (is_blank(lexer->lookahead)) advance(s, lexer);
    }
    if (lexer->lookahead != '>') return false;
    advance(s, lexer);
    return true;
}

// `<`: one of the HTML block start conditions (§4.6), or block 1's end.
static bool scan_html_block(Scanner *s, TSLexer *lexer, const bool *valid) {
    if (!(valid[HTML_BLOCK_1_START] || valid[HTML_BLOCK_1_END] || valid[HTML_BLOCK_2_START] ||
          valid[HTML_BLOCK_3_START] || valid[HTML_BLOCK_4_START] || valid[HTML_BLOCK_5_START] ||
          valid[HTML_BLOCK_6_START] || valid[HTML_BLOCK_7_START])) {
        return false;
    }
    advance(s, lexer);
    if (lexer->lookahead == '?' || lexer->lookahead == '!') return scan_html_special(s, lexer, valid);
    const bool closing = lexer->lookahead == '/';
    if (closing) advance(s, lexer);
    char name[11];
    size_t length = 0;
    bool too_long = false;
    while (iswalpha((wint_t)lexer->lookahead)) {
        if (length < 10) {
            name[length++] = (char)towlower((wint_t)lexer->lookahead);
        } else {
            too_long = true;
        }
        advance(s, lexer);
    }
    if (length == 0) return false;
    bool tag_closed = false;
    if (!too_long) {
        name[length] = 0;
        const bool name_ends = is_blank(lexer->lookahead) || is_line_end(lexer->lookahead) || lexer->lookahead == '>';
        if (name_ends && in_list(name, HTML_BLOCK_1_TAGS, sizeof HTML_BLOCK_1_TAGS / sizeof HTML_BLOCK_1_TAGS[0])) {
            if (closing && valid[HTML_BLOCK_1_END]) {
                lexer->result_symbol = HTML_BLOCK_1_END;
                return true;
            }
            if (!closing && valid[HTML_BLOCK_1_START]) {
                lexer->result_symbol = HTML_BLOCK_1_START;
                push_block(s, ANONYMOUS);
                return true;
            }
        }
        if (!name_ends && lexer->lookahead == '/') {
            advance(s, lexer);
            if (lexer->lookahead == '>') {
                advance(s, lexer);
                tag_closed = true;
            }
        }
        if ((name_ends || tag_closed) && valid[HTML_BLOCK_6_START] &&
            in_list(name, HTML_BLOCK_6_TAGS, sizeof HTML_BLOCK_6_TAGS / sizeof HTML_BLOCK_6_TAGS[0])) {
            lexer->result_symbol = HTML_BLOCK_6_START;
            push_block(s, ANONYMOUS);
            return true;
        }
    }
    if (!valid[HTML_BLOCK_7_START]) return false;
    if (!tag_closed && !scan_html_complete_tag(s, lexer, closing)) return false;
    while (is_blank(lexer->lookahead)) advance(s, lexer);
    if (!is_line_end(lexer->lookahead)) return false;
    lexer->result_symbol = HTML_BLOCK_7_START;
    push_block(s, ANONYMOUS);
    return true;
}

// A pipe table's first line: its header, if the next line (inside the same
// containers) is a delimiter row with as many cells (GFM §4.10). Zero-width.
static bool scan_pipe_table(Scanner *s, TSLexer *lexer) {
    mark_end(s, lexer);
    unsigned cells = 0;
    bool leading_pipe = false, trailing_pipe = false;
    if (lexer->lookahead == '|') {
        leading_pipe = true;
        advance(s, lexer);
    }
    while (!is_line_end(lexer->lookahead) && !lexer->eof(lexer)) {
        if (lexer->lookahead == '|') {
            cells++;
            trailing_pipe = true;
            advance(s, lexer);
            continue;
        }
        if (!is_blank(lexer->lookahead)) trailing_pipe = false;
        if (lexer->lookahead == '\\') {
            advance(s, lexer);
            if (is_ascii_punctuation(lexer->lookahead)) advance(s, lexer);
        } else {
            advance(s, lexer);
        }
    }
    if (cells == 0 && !(leading_pipe && trailing_pipe)) return false;
    if (!trailing_pipe) cells++;
    if (!is_line_end(lexer->lookahead)) return false;
    advance_line_end(s, lexer);

    // The next line, past its containers.
    s->indentation = 0;
    s->column = 0;
    while (is_blank(lexer->lookahead)) add_indentation(s, advance(s, lexer));
    s->simulate = true;
    for (uint32_t i = 0; i < s->blocks.size; i++) {
        if (!match(s, lexer, s->blocks.contents[i])) return false;
    }

    unsigned delimiter_cells = 0;
    if (lexer->lookahead == '|') advance(s, lexer);
    for (;;) {
        while (is_blank(lexer->lookahead)) advance(s, lexer);
        if (lexer->lookahead == '|') {
            delimiter_cells++;
            advance(s, lexer);
            continue;
        }
        if (lexer->lookahead == ':') {
            advance(s, lexer);
            if (lexer->lookahead != '-') return false;
        }
        bool minus = false;
        while (lexer->lookahead == '-') {
            minus = true;
            advance(s, lexer);
        }
        if (minus) delimiter_cells++;
        if (lexer->lookahead == ':') {
            if (!minus) return false;
            advance(s, lexer);
        }
        while (is_blank(lexer->lookahead)) advance(s, lexer);
        if (lexer->lookahead == '|') {
            if (!minus) delimiter_cells++;
            advance(s, lexer);
            continue;
        }
        if (!is_line_end(lexer->lookahead)) return false;
        break;
    }
    if (cells != delimiter_cells) return false;
    lexer->result_symbol = PIPE_TABLE_START;
    return true;
}

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid);

// What starts the line, once its containers are matched: a new block, or
// nothing the scanner reads (text, for the grammar). A character that can
// only start a block decides the scan either way (it has been consumed):
// that answer is returned as 0 or 1. -1 means nothing here, so a line
// ending may follow.
static int scan_block_start(Scanner *s, TSLexer *lexer, const bool *valid) {
    while (is_blank(lexer->lookahead)) add_indentation(s, advance(s, lexer));
    if (valid[INDENTED_CHUNK_START] && !valid[NO_INDENTED_CHUNK] && s->indentation >= 4 &&
        !is_line_end(lexer->lookahead)) {
        lexer->result_symbol = INDENTED_CHUNK_START;
        push_block(s, INDENTED_CODE_BLOCK);
        s->indentation -= 4;
        return 1;
    }
    switch (lexer->lookahead) {
        case '\r':
        case '\n':
            if (valid[BLANK_LINE_START]) {
                lexer->result_symbol = BLANK_LINE_START;  // zero-width
                return 1;
            }
            return -1;
        case '`':
        case '~':
            return scan_fence(s, lexer, valid, lexer->lookahead);
        case '*':
            return scan_star(s, lexer, valid);
        case '_':
            return scan_underscore(s, lexer, valid);
        case '>':
            return scan_block_quote(s, lexer, valid);
        case '#':
            return scan_atx_heading(s, lexer, valid);
        case '=':
            return scan_setext_h1(s, lexer, valid);
        case '+':
            return scan_plus(s, lexer, valid);
        case '-':
            return scan_minus(s, lexer, valid);
        case '<':
            return scan_html_block(s, lexer, valid);
        default:
            if (lexer->lookahead >= '0' && lexer->lookahead <= '9') return scan_ordered_marker(s, lexer, valid);
            break;
    }
    if (!is_line_end(lexer->lookahead) && valid[PIPE_TABLE_START]) return scan_pipe_table(s, lexer);
    return -1;
}

// At a line's start: match the open blocks, one `block_continuation` for
// those that match, a `_block_close` for the first that does not.
static int scan_matching(Scanner *s, TSLexer *lexer) {
    bool any = false;
    while (s->matched < s->blocks.size) {
        // `_close_block` asked for the innermost block to close here.
        if (s->matched == s->blocks.size - 1 && (s->state & STATE_CLOSE_BLOCK)) {
            if (!any) s->state &= ~STATE_CLOSE_BLOCK;
            break;
        }
        if (!match(s, lexer, s->blocks.contents[s->matched])) {
            // A lazy line: what is left is for the paragraph to decide.
            if (s->state & STATE_WAS_SOFT_LINE_BREAK) s->state &= ~STATE_MATCHING;
            break;
        }
        any = true;
        s->matched++;
    }
    if (any) {
        if (s->matched == s->blocks.size) s->state &= ~STATE_MATCHING;
        lexer->result_symbol = BLOCK_CONTINUATION;
        return 1;
    }
    if (!(s->state & STATE_WAS_SOFT_LINE_BREAK)) {
        lexer->result_symbol = BLOCK_CLOSE;
        array_pop(&s->blocks);
        if (s->matched == s->blocks.size) s->state &= ~STATE_MATCHING;
        return 1;
    }
    return 0;  // fall through to the line ending
}

// A line ending: soft (the paragraph or table goes on) if the next line,
// its containers matched, does not start a block that ends it; else hard.
static bool scan_line_ending(Scanner *s, TSLexer *lexer, const bool *valid) {
    advance_line_end(s, lexer);
    s->indentation = 0;
    s->column = 0;
    if (!(s->state & STATE_CLOSE_BLOCK) && (valid[SOFT_LINE_ENDING] || valid[PIPE_TABLE_LINE_ENDING])) {
        lexer->mark_end(lexer);
        while (is_blank(lexer->lookahead)) add_indentation(s, advance(s, lexer));
        s->simulate = true;
        const uint8_t matched_before = s->matched;
        s->matched = 0;
        bool one_matched = false;
        while (s->matched < s->blocks.size && match(s, lexer, s->blocks.contents[s->matched])) {
            s->matched++;
            one_matched = true;
        }
        const bool all_matched = s->matched == s->blocks.size;
        if (!lexer->eof(lexer) && !scan(s, lexer, INTERRUPTS_PARAGRAPH)) {
            s->matched = 0;
            s->indentation = 0;
            s->column = 0;
            if (one_matched) {
                s->state |= STATE_MATCHING;
            } else {
                s->state &= ~STATE_MATCHING;
            }
            if (valid[PIPE_TABLE_LINE_ENDING]) {
                if (all_matched) {
                    lexer->result_symbol = PIPE_TABLE_LINE_ENDING;
                    return true;
                }
            } else {
                lexer->result_symbol = SOFT_LINE_ENDING;
                s->state |= STATE_WAS_SOFT_LINE_BREAK;
                return true;
            }
        } else {
            s->matched = matched_before;
        }
        s->indentation = 0;
        s->column = 0;
    }
    if (!valid[LINE_ENDING]) return false;
    s->matched = 0;
    if (s->blocks.size > 0) {
        s->state |= STATE_MATCHING;
    } else {
        s->state &= ~STATE_MATCHING;
    }
    s->state &= ~STATE_WAS_SOFT_LINE_BREAK;
    lexer->result_symbol = LINE_ENDING;
    return true;
}

static bool scan(Scanner *s, TSLexer *lexer, const bool *valid) {
    // A grammar rule ruled its branch out: end it.
    if (valid[TRIGGER_ERROR]) {
        lexer->result_symbol = ERROR;
        return true;
    }
    if (valid[CLOSE_BLOCK]) {
        s->state |= STATE_CLOSE_BLOCK;
        lexer->result_symbol = CLOSE_BLOCK;
        return true;
    }
    if (lexer->eof(lexer)) {
        if (valid[TOKEN_EOF]) {
            lexer->result_symbol = TOKEN_EOF;
            return true;
        }
        if (s->blocks.size > 0) {
            lexer->result_symbol = BLOCK_CLOSE;
            if (!s->simulate) array_pop(&s->blocks);
            return true;
        }
        return false;
    }
    if (!(s->state & STATE_MATCHING)) {
        const int start = scan_block_start(s, lexer, valid);
        if (start >= 0) return start;
    } else if (scan_matching(s, lexer)) {
        return true;
    }
    if ((valid[LINE_ENDING] || valid[SOFT_LINE_ENDING] || valid[PIPE_TABLE_LINE_ENDING]) &&
        is_line_end(lexer->lookahead)) {
        return scan_line_ending(s, lexer, valid);
    }
    return false;
}

bool tree_sitter_markdown_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *s = payload;
    s->simulate = false;
    return scan(s, lexer, valid_symbols);
}
