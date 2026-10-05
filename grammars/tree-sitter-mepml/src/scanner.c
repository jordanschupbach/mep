// External scanner for tree-sitter-mepml.
//
// mepml is decided line by line (what a line *is* depends on its first
// characters, and sometimes on its last: a table row starts and ends with
// `|`), and its inline markers are context-sensitive (an opener needs a
// suitable character before it, a non-space after it, and a matching
// closer later in the same paragraph). Both need lookahead the grammar's
// own lexer cannot express, so this scanner produces nearly every token.
// The rules mirror mep's own parser (src/mepml_doc.cpp) -- keep the two in
// step.
//
// Tokens that need the whole line to decide but own none of it
// (_paragraph_start, _table_row_start, _directive_start,
// _list_continuation) are zero-width: the scanner reads ahead with
// mark_end still at the line's start.

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum TokenType {
    BLANK_LINE,
    NEWLINE,
    H1_MARKER, H2_MARKER, H3_MARKER, H4_MARKER, H5_MARKER, H6_MARKER,
    META_MARKER,
    COMMENT_MARKER,
    CALLOUT_MARKER,
    RESULT_BEGIN,
    RESULT_END,
    FENCE_OPEN,
    FENCE_CLOSE,
    CODE_CONTENT,
    MATH_OPEN,
    MATH_CONTENT,
    MATH_CLOSE,
    DIRECTIVE_START,
    TABLE_ROW_START,
    TABLE_DELIMITER_ROW,
    TABLE_PIPE,
    LIST_MARKER,
    TASK_MARKER,
    LIST_CONTINUATION,
    RULE,
    PARAGRAPH_START,
    TEXT,
    ESCAPE,
    VERBATIM,
    LATEX,
    ALTTEXT_MARKER,
    INLINE_COMMENT,
    LINK_OPEN,
    LINK_BAR,
    URL,
    LINK_CLOSE,
    CMD_F, CMD_FS, CMD_COLOR, CMD_FN, CMD_CITE, CMD_CITEP,
    GROUP_OPEN,
    GROUP_CLOSE,
    ARG_TEXT,
    PAREN_OPEN,
    PAREN_CLOSE,
    ARG_COMMA,
    OPEN_BOLD, CLOSE_BOLD,
    OPEN_ITALIC, CLOSE_ITALIC,
    OPEN_UNDERLINE, CLOSE_UNDERLINE,
    OPEN_SUP, CLOSE_SUP,
    OPEN_SUB, CLOSE_SUB,
    OPEN_SMALL, CLOSE_SMALL,
    OPEN_BIG, CLOSE_BIG,
    OPEN_MONO, CLOSE_MONO,
    OPEN_HIGHLIGHT, CLOSE_HIGHLIGHT,
    OPEN_STRIKE, CLOSE_STRIKE,
    OPEN_INSERT, CLOSE_INSERT,
    OPEN_DELETE, CLOSE_DELETE,
    ABSTRACT_OPEN,
    ABSTRACT_BREAK,
    RESULT_BEGIN_MARKDOWN,
    RESULT_END_ATTACHED,
    ATTRIBUTE_START,
    SLIDE_START,
    SLIDE_END,
    COMMAND_BLOCK_START,
    OPAQUE_TEXT,
    CMD_RAW,
    CMD_USER,
    MATH_TRAILING,
    BOX_START,
    BOX_LINE_START,
    BOX_OPEN,
    BOX_BREAK,
    BOX_END,
    CMD_CLASS,
    ERROR_SENTINEL,
};

// Paired markers, in the order src/mepml_doc.cpp tries them (`,,` before
// anything else so it is never read as two stray commas).
typedef struct {
    const char *open, *close;
    enum TokenType open_tok, close_tok;
    bool intraword;  // may open/close inside a word (super/subscript)
} Marker;

static const Marker kMarkers[] = {
    {",,", ",,", OPEN_SUB, CLOSE_SUB, true},
    {"*", "*", OPEN_BOLD, CLOSE_BOLD, false},
    {"~", "~", OPEN_ITALIC, CLOSE_ITALIC, false},
    {"_", "_", OPEN_UNDERLINE, CLOSE_UNDERLINE, false},
    {"^", "^", OPEN_SUP, CLOSE_SUP, true},
    {"<", ">", OPEN_SMALL, CLOSE_SMALL, false},
    {">", "<", OPEN_BIG, CLOSE_BIG, false},
    {"|", "|", OPEN_MONO, CLOSE_MONO, false},
    {"=", "=", OPEN_HIGHLIGHT, CLOSE_HIGHLIGHT, false},
    {"-", "-", OPEN_STRIKE, CLOSE_STRIKE, false},
    {"+", "+", OPEN_INSERT, CLOSE_INSERT, false},
    {"!", "!", OPEN_DELETE, CLOSE_DELETE, false},
};
#define MARKER_COUNT (sizeof(kMarkers) / sizeof(kMarkers[0]))

enum Context { CTX_PARAGRAPH = 0, CTX_LINE = 1 };
enum LinkMode { LINK_NONE = 0, LINK_TEXT = 1, LINK_URL_ONLY = 2 };
enum MathKind { MATH_NONE = 0, MATH_DOLLARS = 1, MATH_BRACKET = 2 };

#define MAX_DEPTH 32

typedef struct {
    uint8_t prev;       // last character consumed ('\n' at a line start)
    uint8_t context;    // how far an inline lookahead may reach
    uint8_t link_mode;  // inside [...]: which form
    uint8_t math_kind;  // inside display maths: which closer
    uint16_t open_mask;  // bit i: kMarkers[i] is open (text must stop at its closer)
    uint8_t depth_count;
    // Inside \abstract(...): the depth_count its own group sits at (0 when
    // outside one). Its prose runs over lines, paragraphs split by blank ones.
    uint8_t abstract_level;
    // The table the previous line belonged to: TABLE_NONE, a mepml table
    // (rows are `|` at both ends), or a GitHub-flavoured Markdown one
    // (header over a delimiter row), whose rows need no outer pipes.
    uint8_t table;
    // Bit i: open group i is a `(...)` (a \name(...) command or directive),
    // not a `{...}` one. Each counts only its own kind of bracket.
    uint32_t paren_mask;
    // The closing bracket of the slide open now (`)` for `\slide(`, `}` for
    // `\slide{` / `@slide{`), 0 outside one. Slides do not nest.
    uint8_t slide;
    // Boxes (\definition( ...): how many are open (a line holding just `)`
    // closes the innermost first); while a box's opening line is read, the
    // depth_count its group sits at (0 otherwise); and whether text after
    // its title runs on over the lines that follow (its first paragraph).
    uint8_t boxes;
    uint8_t box_level;
    uint8_t box_lead;
    uint8_t depths[MAX_DEPTH];  // bracket depth inside each open group
} Scanner;

enum TableKind { TABLE_NONE = 0, TABLE_MEPML = 1, TABLE_GFM = 2 };

// --- character classes ---------------------------------------------------

static bool is_space(int32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_blank(int32_t c) { return c == ' ' || c == '\t'; }
static bool is_alpha(int32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }
static bool is_upper(int32_t c) { return c >= 'A' && c <= 'Z'; }
static bool is_marker_char(int32_t c) { return c != 0 && strchr("*~_^<>|=-+!,", (int)c) != NULL; }
static bool pre_ok(int32_t c) {
    return c == 0 || is_space(c) || c == '(' || c == '[' || c == '{' || c == '"' || c == '\'' || c == '/' ||
           is_marker_char(c);
}
static bool post_ok(int32_t c) {
    return c == 0 || is_space(c) || (c < 128 && strchr(".,;:!?)]}\"'/", (int)c) != NULL) || is_marker_char(c);
}
static uint8_t clamp_char(int32_t c) { return c > 0 && c < 128 ? (uint8_t)c : 'a'; }

// --- lexer helpers -----------------------------------------------------------

static void adv(TSLexer *lexer) { lexer->advance(lexer, false); }
static int32_t la(TSLexer *lexer) { return lexer->eof(lexer) ? 0 : lexer->lookahead; }
static bool at_eol(TSLexer *lexer) {
    int32_t c = la(lexer);
    return c == 0 || c == '\n' || c == '\r';
}

// Emit `tok`, ending at the current mark_end, whose last character was
// `last` (the next PRE check reads it).
static bool emit(Scanner *s, TSLexer *lexer, enum TokenType tok, int32_t last) {
    lexer->result_symbol = tok;
    s->prev = clamp_char(last);
    return true;
}

static bool match_word(TSLexer *lexer, const char *w) {
    for (const char *p = w; *p; ++p) {
        if (la(lexer) != *p) return false;
        adv(lexer);
    }
    return true;
}

// --- scanner lifecycle ----------------------------------------------------

void *tree_sitter_mepml_external_scanner_create(void) {
    Scanner *s = (Scanner *)calloc(1, sizeof(Scanner));
    s->prev = '\n';
    return s;
}
void tree_sitter_mepml_external_scanner_destroy(void *payload) { free(payload); }

unsigned tree_sitter_mepml_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *s = (Scanner *)payload;
    unsigned n = 0;
    buffer[n++] = (char)s->prev;
    buffer[n++] = (char)s->context;
    buffer[n++] = (char)s->link_mode;
    buffer[n++] = (char)s->math_kind;
    buffer[n++] = (char)(s->open_mask & 0xff);
    buffer[n++] = (char)(s->open_mask >> 8);
    buffer[n++] = (char)s->depth_count;
    buffer[n++] = (char)s->abstract_level;
    buffer[n++] = (char)s->table;
    for (unsigned k = 0; k < 4; ++k) buffer[n++] = (char)((s->paren_mask >> (8 * k)) & 0xff);
    buffer[n++] = (char)s->slide;
    buffer[n++] = (char)s->boxes;
    buffer[n++] = (char)s->box_level;
    buffer[n++] = (char)s->box_lead;
    for (unsigned i = 0; i < s->depth_count && i < MAX_DEPTH; ++i) buffer[n++] = (char)s->depths[i];
    return n;
}

void tree_sitter_mepml_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *s = (Scanner *)payload;
    memset(s, 0, sizeof(*s));
    s->prev = '\n';
    if (length < 17) return;
    s->prev = (uint8_t)buffer[0];
    s->context = (uint8_t)buffer[1];
    s->link_mode = (uint8_t)buffer[2];
    s->math_kind = (uint8_t)buffer[3];
    s->open_mask = (uint16_t)((uint8_t)buffer[4] | ((uint16_t)(uint8_t)buffer[5] << 8));
    s->depth_count = (uint8_t)buffer[6];
    s->abstract_level = (uint8_t)buffer[7];
    s->table = (uint8_t)buffer[8];
    for (unsigned k = 0; k < 4; ++k) s->paren_mask |= (uint32_t)(uint8_t)buffer[9 + k] << (8 * k);
    s->slide = (uint8_t)buffer[13];
    s->boxes = (uint8_t)buffer[14];
    s->box_level = (uint8_t)buffer[15];
    s->box_lead = (uint8_t)buffer[16];
    for (unsigned i = 0; i < s->depth_count && i < MAX_DEPTH && 17 + i < length; ++i) s->depths[i] = (uint8_t)buffer[17 + i];
}

// --- groups ------------------------------------------------------------------

// The innermost open group is a `(...)` one.
static bool top_is_paren(const Scanner *s) {
    return s->depth_count > 0 && s->depth_count <= MAX_DEPTH && ((s->paren_mask >> (s->depth_count - 1)) & 1u);
}
static bool top_is_brace(const Scanner *s) { return s->depth_count > 0 && !top_is_paren(s); }
static void push_group(Scanner *s, bool paren) {
    if (s->depth_count < MAX_DEPTH) {
        s->depths[s->depth_count] = 0;
        if (paren) s->paren_mask |= 1u << s->depth_count;
        else s->paren_mask &= ~(1u << s->depth_count);
    }
    s->depth_count++;
}
static void pop_group(Scanner *s) {
    if (s->depth_count == 0) return;
    s->depth_count--;
    if (s->depth_count < MAX_DEPTH) s->paren_mask &= ~(1u << s->depth_count);
}
// Nesting inside the innermost group (its own kind of bracket only).
static uint8_t top_depth(const Scanner *s) {
    return s->depth_count > 0 && s->depth_count <= MAX_DEPTH ? s->depths[s->depth_count - 1] : 0;
}
static void top_depth_add(Scanner *s, int d) {
    if (s->depth_count == 0 || s->depth_count > MAX_DEPTH) return;
    uint8_t *v = &s->depths[s->depth_count - 1];
    if (d > 0 && *v < 255) (*v)++;
    if (d < 0 && *v > 0) (*v)--;
}

// The directives written `\name(...)` (or bare, `\toc`) at a line's start.
static bool is_backslash_directive(const char *name) {
    static const char *const kNames[] = {"import", "citation", "image", "caption", "alttext", "bibliography", "toc", "abstract"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (strcmp(name, kNames[i]) == 0) return true;
    return false;
}


// The kinds of box, `\\definition(` ... (BoxKinds in mepml_doc.cpp).
static bool is_box_kind(const char *name) {
    // (`boxed` is any kind of the document's own: `\\boxed(axiom, Title,`.)
    static const char *const kNames[] = {"definition", "theorem", "lemma",  "proposition", "corollary",
                                         "fact",       "example", "remark", "proof",       "note",
                                         "tip",        "warning", "boxed",
                                         // (Columns nest and close as boxes do.)
                                         "columns",    "column"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (strcmp(name, kNames[i]) == 0) return true;
    return false;
}

// Names mepml gives a meaning after `\\` (IsBuiltinCommandName in
// mepml_doc.cpp); any other `\\name(` calls a user command.
static bool is_builtin_command(const char *name) {
    static const char *const kNames[] = {"f",        "fs",    "color",  "fn",           "cite",
                                         "citep",    "alttext", "caption", "image",       "import",
                                         "citation", "toc",   "bibliography", "printbibliography",
                                         "abstract", "slide", "define", "raw",    "class"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (strcmp(name, kNames[i]) == 0) return true;
    return is_box_kind(name);
}

// Reads a name of letters (the lexer just past its backslash); a name too
// long for `name` is cut short but still read whole.
static int read_name(TSLexer *lexer, char *name, int cap) {
    int n = 0;
    while (is_alpha(la(lexer))) {
        if (n < cap - 1) name[n] = (char)la(lexer);
        n++;
        adv(lexer);
    }
    name[n < cap - 1 ? n : cap - 1] = 0;
    return n;
}

// The lexer at a `(`: whether its group closes (over any number of lines,
// blank ones too) at the end of a line, a `// comment` allowed after it
// (CommandBlockAt in mepml_doc.cpp). *crossed (when given): whether the
// lexer has left the line.
static bool group_ends_line_crossed(TSLexer *lexer, bool *crossed) {
    int depth = 0;
    while (true) {
        const int32_t c = la(lexer);
        if (c == 0 && lexer->eof(lexer)) return false;
        if (c == '\n' && crossed) *crossed = true;
        adv(lexer);
        if (c == '\\') {
            if (!lexer->eof(lexer)) adv(lexer);
            continue;
        }
        if (c == '(') depth++;
        if (c == ')' && --depth == 0) break;
    }
    while (is_blank(la(lexer))) adv(lexer);
    if (at_eol(lexer)) return true;
    if (la(lexer) != '/') return false;
    adv(lexer);
    return la(lexer) == '/';
}
static bool group_ends_line(TSLexer *lexer) { return group_ends_line_crossed(lexer, NULL); }

// --- lookahead scope ------------------------------------------------------------
//
// An inline construct may only close inside the stretch of text the C++
// parser would hand to its inline parser: the rest of the line for a
// heading, table cell, callout or list item line, the rest of the
// paragraph otherwise -- a paragraph ends at a blank line or at a line that
// starts some other block.

// The rest of a line, just past a bracket that could close the open
// slide: blanks, then its end or a `// comment`.
static bool slide_closer_rest(TSLexer *lexer) {
    while (is_blank(la(lexer))) adv(lexer);
    if (at_eol(lexer)) return true;
    if (la(lexer) != '/') return false;
    adv(lexer);
    return la(lexer) == '/';
}

// `slide_close`: the open slide's closing bracket, 0 when none is open;
// `box_open`: a box is open (its `)` closes it).
static bool line_starts_block(TSLexer *lexer, int32_t slide_close, bool box_open) {
    // Called with the lexer at the first character of a line.
    while (is_blank(la(lexer))) adv(lexer);
    int32_t c = la(lexer);
    if (c == 0 || c == '\n' || c == '\r') return true;  // blank line
    if ((slide_close && c == slide_close) || (box_open && c == ')')) {
        adv(lexer);
        return slide_closer_rest(lexer);
    }
    if (c == '/') {
        adv(lexer);
        return la(lexer) == '/';
    }
    if (c == '`') {
        adv(lexer);
        if (la(lexer) != '`') return false;
        adv(lexer);
        return la(lexer) == '`';
    }
    if (c == '$') {
        adv(lexer);
        return la(lexer) == '$';
    }
    if (c == '\\') {
        adv(lexer);
        if (la(lexer) == '[') return true;
        char name[64];
        const int n = read_name(lexer, name, (int)sizeof name);
        const int32_t next = la(lexer);
        if (n == 0) return false;
        if (n < (int)sizeof name && is_backslash_directive(name) && (next == '(' || is_blank(next) || at_eol(lexer))) return true;
        if (strcmp(name, "slide") == 0 && (next == '(' || next == '{')) return true;  // a slide opens
        if (next == '(' && n < (int)sizeof name && is_box_kind(name)) return true;       // a box opens
        // \define(, or \raw( / a user command's call ending a line.
        if (next != '(') return false;
        if (strcmp(name, "define") == 0) return true;
        if (n < (int)sizeof name && strcmp(name, "raw") != 0 && is_builtin_command(name)) return false;
        return group_ends_line(lexer);
    }
    if (c == '|' || c == '@') return true;  // (approximate: tables, directives)
    if (c == '>') {
        while (la(lexer) == '>') adv(lexer);
        return is_blank(la(lexer)) || at_eol(lexer);
    }
    if (c == '-' || c == '*' || c == '+') {
        adv(lexer);
        return is_blank(la(lexer)) || la(lexer) == c;
    }
    if (is_digit(c)) {
        while (is_digit(la(lexer))) adv(lexer);
        if (la(lexer) != '.' && la(lexer) != ')') return false;
        adv(lexer);
        return is_blank(la(lexer));
    }
    return false;
}

// Called at a '\n' inside a lookahead: true when the scope continues onto
// the next line.
static bool in_abstract(const Scanner *s) { return s->abstract_level > 0 && s->depth_count >= s->abstract_level; }

static bool scope_continues(Scanner *s, TSLexer *lexer) {
    if (s->context != CTX_PARAGRAPH) return false;
    if (la(lexer) == '\r') adv(lexer);
    if (la(lexer) != '\n') return false;
    adv(lexer);
    // An abstract's paragraph is every line up to a blank one, whatever
    // the line starts with (ParseAbstract in mepml_doc.cpp).
    if (in_abstract(s)) {
        while (is_blank(la(lexer))) adv(lexer);
        return !at_eol(lexer);
    }
    return !line_starts_block(lexer, s->slide, s->boxes > 0);
}

// A group has closed: leaving the abstract's own ends the abstract, and
// leaving a box's opening group ends its opening line(s).
static void group_closed(Scanner *s) {
    if (s->abstract_level > s->depth_count) s->abstract_level = 0;
    if (s->box_level > s->depth_count) s->box_level = s->box_lead = 0;
}

// A box's opening line(s) are over: its group, and anything opened in it,
// closes with them.
static void box_line_done(Scanner *s) {
    if (s->box_level == 0) return;
    while (s->depth_count >= s->box_level && s->depth_count > 0) pop_group(s);
    s->box_level = s->box_lead = 0;
    group_closed(s);
}

// The lexer at a box's `(`: whether the box closes where its text ends --
// on this line (`\\remark(Title, text)`), or at a `)` ending a later line
// of the paragraph its text starts (ParseBoxOpen in mepml_doc.cpp); *lead:
// whether text follows its title on this line.
// `skip` commas come before the title's own (`\\boxed(kind, Title,`: one).
static bool box_closes(TSLexer *lexer, int32_t slide_close, bool *lead, int skip) {
    int depth = 0;
    bool comma = false;
    *lead = false;
    while (!at_eol(lexer)) {
        const int32_t c = la(lexer);
        adv(lexer);
        if (c == '\\') {
            if (!at_eol(lexer)) adv(lexer);
            if (comma) *lead = true;
            continue;
        }
        if (c == '(') depth++;
        if (c == ')' && --depth == 0) return true;
        if (c == ',' && depth == 1 && !comma) {
            if (skip > 0) {
                skip--;
                continue;
            }
            comma = true;
            continue;
        }
        if (comma && !is_blank(c)) *lead = true;
    }
    if (!*lead) return false;
    // Its first paragraph, line by line: closed when a line ends with the
    // box's own `)`.
    while (true) {
        if (la(lexer) == '\r') adv(lexer);
        if (la(lexer) != '\n') return false;
        adv(lexer);
        // (A line of just `)` is this box's own end, as anywhere else.)
        if (line_starts_block(lexer, slide_close, true)) return false;
        int32_t last = 0;
        while (!at_eol(lexer)) {
            const int32_t c = la(lexer);
            adv(lexer);
            if (c == '\\') {
                if (!at_eol(lexer)) adv(lexer);
                last = 'a';
                continue;
            }
            if (c == '(') depth++;
            if (c == ')') depth--;
            if (!is_blank(c)) last = c;
        }
        if (depth == 0 && last == ')') return true;
    }
}

// --- inline constructs ----------------------------------------------------------

// Skips a balanced {...} or (...) whose opener is the current character
// (counting only that kind of bracket). False when it never closes inside
// the scope. *comma (when given) says whether a top-level comma was in it.
static bool skip_group_comma(Scanner *s, TSLexer *lexer, bool *comma) {
    const int32_t open = la(lexer), close = open == '(' ? ')' : '}';
    int depth = 0;
    while (true) {
        int32_t c = la(lexer);
        if (c == 0) return false;
        if (c == '\n' || c == '\r') {
            if (!scope_continues(s, lexer)) return false;
            continue;
        }
        if (c == '\\') {
            adv(lexer);
            if (!at_eol(lexer)) adv(lexer);
            continue;
        }
        if (c == open) depth++;
        if (c == ',' && depth == 1 && comma) *comma = true;
        if (c == close) {
            depth--;
            adv(lexer);
            if (depth == 0) return true;
            continue;
        }
        adv(lexer);
    }
}
static bool skip_group(Scanner *s, TSLexer *lexer) { return skip_group_comma(s, lexer, NULL); }

// Whether a closer for `m` follows (FindCloser in mepml_doc.cpp). The lexer
// sits just past the opener; `opener_next` is the character after it.
static bool closer_exists(Scanner *s, TSLexer *lexer, const Marker *m) {
    bool first = true;
    int32_t prev = 0;
    int group_depth = 0;
    int parens = 0;  // plain parentheses opened since the opener
    const size_t cl = strlen(m->close);
    while (true) {
        int32_t c = la(lexer);
        if (c == 0) return false;
        if (c == '\n' || c == '\r') {
            if (!scope_continues(s, lexer)) return false;
            prev = '\n';
            first = false;
            continue;
        }
        if (c == '\\') {
            adv(lexer);
            if (is_alpha(la(lexer))) {
                // A command's `(...)` arguments are inside it, like a `{...}`.
                while (is_alpha(la(lexer))) adv(lexer);
                if (la(lexer) == '(' && !skip_group(s, lexer)) return false;
            } else if (!at_eol(lexer)) {
                adv(lexer);
            }
            prev = 'a';
            first = false;
            continue;
        }
        if (c == '`') {
            // A verbatim span is opaque (if it closes on this line).
            adv(lexer);
            while (!at_eol(lexer) && la(lexer) != '`') adv(lexer);
            if (la(lexer) == '`') adv(lexer);
            prev = '`';
            first = false;
            continue;
        }
        if (c == '{') {
            if (!skip_group(s, lexer)) return false;
            prev = '}';
            first = false;
            continue;
        }
        // The end of an enclosing group or link ends the search.
        if (c == '}' && top_is_brace(s) && group_depth == 0) return false;
        if (top_is_paren(s) && c == '(') parens++;
        if (top_is_paren(s) && c == ')' && parens-- == 0) return false;
        if (c == ']' && s->link_mode != LINK_NONE) return false;
        if (c == '|' && s->context == CTX_LINE && m->close[0] != '|') {
            // (a table cell's pipe; checked loosely -- a closer past it
            // would belong to the next cell)
        }
        if ((char)c == m->close[0]) {
            bool whole = true;
            adv(lexer);
            if (cl == 2) {
                if (la(lexer) != m->close[1]) {
                    whole = false;
                } else {
                    adv(lexer);
                }
            }
            if (whole && !first && !is_space(prev)) {
                int32_t next = la(lexer);
                bool ok = m->intraword || post_ok(next);
                if (cl == 1 && !m->intraword && next == m->close[0]) ok = false;  // `**`
                if (ok) return true;
            }
            prev = c;
            first = false;
            continue;
        }
        prev = c;
        first = false;
        adv(lexer);
    }
}

// `...` : the next backtick in scope, not immediately after this one.
static bool scan_verbatim(Scanner *s, TSLexer *lexer) {
    adv(lexer);  // opening `
    lexer->mark_end(lexer);
    bool empty = true;
    while (true) {
        int32_t c = la(lexer);
        if (c == 0) return false;
        if (c == '\n' || c == '\r') {
            if (!scope_continues(s, lexer)) return false;
            empty = false;
            continue;
        }
        if (c == '`') {
            if (empty) return false;
            adv(lexer);
            lexer->mark_end(lexer);
            return true;
        }
        empty = false;
        adv(lexer);
    }
}

// $x$, $$x$$ or \(x\) (TryMath in mepml_doc.cpp). The lexer is at the
// first delimiter character; on success the token covers the whole span.
static bool scan_latex(Scanner *s, TSLexer *lexer) {
    int32_t c = la(lexer);
    if (c == '\\') {
        adv(lexer);  // backslash
        adv(lexer);  // (
        lexer->mark_end(lexer);
        bool empty = true;
        while (true) {
            int32_t d = la(lexer);
            if (d == 0) return false;
            if (d == '\n' || d == '\r') {
                if (!scope_continues(s, lexer)) return false;
                continue;
            }
            adv(lexer);
            if (d == '\\' && la(lexer) == ')') {
                if (empty) return false;
                adv(lexer);
                lexer->mark_end(lexer);
                return true;
            }
            empty = false;
        }
    }
    adv(lexer);  // first $
    lexer->mark_end(lexer);
    if (la(lexer) == '$') {
        adv(lexer);
        bool empty = true;
        while (true) {
            int32_t d = la(lexer);
            if (d == 0) return false;
            if (d == '\n' || d == '\r') {
                if (!scope_continues(s, lexer)) return false;
                continue;
            }
            adv(lexer);
            if (d == '$' && la(lexer) == '$') {
                if (empty) return false;
                adv(lexer);
                lexer->mark_end(lexer);
                return true;
            }
            empty = false;
        }
    }
    // Pandoc's rule: no space just inside either dollar, and a closing
    // dollar followed by a digit is a price.
    if (is_space(la(lexer)) || la(lexer) == 0) return false;
    int32_t prev = '$';
    while (true) {
        int32_t d = la(lexer);
        if (d == 0) return false;
        if (d == '\n' || d == '\r') {
            if (!scope_continues(s, lexer)) return false;
            prev = '\n';
            continue;
        }
        if (d == '\\') {
            adv(lexer);
            if (!at_eol(lexer)) adv(lexer);
            prev = 'a';
            continue;
        }
        if (d == '$') {
            if (is_space(prev)) return false;
            adv(lexer);
            if (is_digit(la(lexer))) return false;
            lexer->mark_end(lexer);
            return true;
        }
        prev = d;
        adv(lexer);
    }
}

// Whether [..] starting here (lexer just past '[') is a link, and which
// form (TryLink in mepml_doc.cpp): a top-level `|` then a non-empty url, or
// a bare url. Same line only.
static enum LinkMode classify_link(TSLexer *lexer) {
    int depth = 1;
    bool bar = false, after_bar_text = false, has_space = false, has_scheme = false;
    int32_t first = la(lexer), prev = 0, prev2 = 0;
    bool starts_ok = first == '.' || first == '#' || first == '/' || first == 'm' || first == 'f';
    (void)starts_ok;
    char head[8] = {0};
    int head_len = 0;
    while (true) {
        int32_t c = la(lexer);
        if (c == 0 || c == '\n' || c == '\r') return LINK_NONE;
        if (c == '\\') {
            adv(lexer);
            if (!at_eol(lexer)) adv(lexer);
            continue;
        }
        if (c == '[') depth++;
        if (c == ']') {
            if (--depth == 0) break;
        }
        if (c == '|' && depth == 1) {
            bar = true;
            after_bar_text = false;
        } else if (bar && !is_space(c)) {
            after_bar_text = true;
        }
        if (is_space(c)) has_space = true;
        if (c == '/' && prev == '/' && prev2 == ':') has_scheme = true;
        if (head_len < 7) head[head_len++] = (char)c;
        prev2 = prev;
        prev = c;
        adv(lexer);
    }
    if (bar) return after_bar_text ? LINK_TEXT : LINK_NONE;
    if (head_len == 0 || has_space) return LINK_NONE;
    if (has_scheme || strncmp(head, "mailto:", 7) == 0 || strncmp(head, "file:", 5) == 0 ||
        strncmp(head, "./", 2) == 0 || strncmp(head, "../", 3) == 0 || head[0] == '#')
        return LINK_URL_ONLY;
    return LINK_NONE;
}

// In a text-form link at a '|': is it the link's own bar (the last
// top-level one before the closing ']')?
static bool is_link_bar(TSLexer *lexer) {
    int depth = 0;
    while (true) {
        int32_t c = la(lexer);
        if (c == 0 || c == '\n' || c == '\r') return false;
        if (c == '\\') {
            adv(lexer);
            if (!at_eol(lexer)) adv(lexer);
            continue;
        }
        if (c == '[') depth++;
        if (c == ']') {
            if (depth == 0) return true;
            depth--;
        }
        if (c == '|' && depth == 0) return false;
        adv(lexer);
    }
}

// Reads a command's name (the lexer just past the backslash); false when it
// is too long to be one.
static bool read_command_name(TSLexer *lexer, char name[64]) {
    int n = 0;
    while (is_alpha(la(lexer)) && n < 63) {
        name[n++] = (char)la(lexer);
        adv(lexer);
    }
    name[n] = 0;
    return !is_alpha(la(lexer));
}

// \name(...) (or the older \name{...}) commands: the lexer is just past the
// name (read into `name`). Emits the command token (covering `\name`) when
// the arguments it needs follow: `(arg, text)` for a two-argument command,
// its first top-level comma ending the argument.
static bool scan_command_named(Scanner *s, TSLexer *lexer, const bool *valid, const char *name) {
    const int n = (int)strlen(name);
    if (n == 0) return false;
    enum TokenType tok;
    int groups;
    if (strcmp(name, "f") == 0) tok = CMD_F, groups = 2;
    else if (strcmp(name, "fs") == 0) tok = CMD_FS, groups = 2;
    else if (strcmp(name, "color") == 0) tok = CMD_COLOR, groups = 2;
    else if (strcmp(name, "class") == 0) tok = CMD_CLASS, groups = 2;
    else if (strcmp(name, "fn") == 0) tok = CMD_FN, groups = 1;
    else if (strcmp(name, "cite") == 0) tok = CMD_CITE, groups = 1;
    else if (strcmp(name, "citep") == 0) tok = CMD_CITEP, groups = 1;
    else if (strcmp(name, "raw") == 0) {
        // \raw(formats, text): its first comma ends the formats.
        if (!valid[CMD_RAW] || la(lexer) != '(') return false;
        lexer->mark_end(lexer);
        bool comma = false;
        if (!skip_group_comma(s, lexer, &comma) || !comma) return false;
        return emit(s, lexer, CMD_RAW, 'w');
    } else if (!is_builtin_command(name)) {
        // A user command's call (\when and \otherwise too): `\name(...)`.
        if (!valid[CMD_USER] || la(lexer) != '(') return false;
        lexer->mark_end(lexer);
        if (!skip_group(s, lexer)) return false;
        return emit(s, lexer, CMD_USER, name[n - 1]);
    } else return false;
    if (!valid[tok] || (la(lexer) != '{' && la(lexer) != '(')) return false;
    lexer->mark_end(lexer);
    int32_t last = name[n - 1];
    if (la(lexer) == '(') {
        bool comma = false;
        if (!skip_group_comma(s, lexer, &comma) || (groups == 2 && !comma)) return false;
        return emit(s, lexer, tok, last);
    }
    if (!skip_group(s, lexer)) return false;
    if (groups == 2) {
        while (is_blank(la(lexer))) adv(lexer);
        if (la(lexer) != '{' || !skip_group(s, lexer)) return false;
    }
    return emit(s, lexer, tok, last);
}

static bool scan_command(Scanner *s, TSLexer *lexer, const bool *valid) {
    char name[64];
    return read_command_name(lexer, name) && scan_command_named(s, lexer, valid, name);
}

// Emphasis: close when a closer of this kind is open and valid here, open
// when a closer follows in scope.
static bool scan_marker(Scanner *s, TSLexer *lexer, const bool *valid, bool *consumed) {
    int32_t c = la(lexer);
    for (size_t i = 0; i < MARKER_COUNT; ++i) {
        const Marker *m = &kMarkers[i];
        if (valid[m->close_tok] && c == m->close[0] && !is_space(s->prev)) {
            size_t cl = strlen(m->close);
            adv(lexer);
            *consumed = true;
            if (cl == 2) {
                if (la(lexer) != m->close[1]) return false;
                adv(lexer);
            }
            lexer->mark_end(lexer);
            int32_t next = la(lexer);
            bool ok = m->intraword || post_ok(next);
            if (cl == 1 && !m->intraword && next == m->close[0]) ok = false;
            if (ok) {
                s->prev = clamp_char(c);
                s->open_mask = (uint16_t)(s->open_mask & ~(1u << i));
                lexer->result_symbol = m->close_tok;
                return true;
            }
            return false;
        }
    }
    for (size_t i = 0; i < MARKER_COUNT; ++i) {
        const Marker *m = &kMarkers[i];
        if (!valid[m->open_tok] || c != m->open[0]) continue;
        if (!m->intraword && !pre_ok(s->prev)) continue;
        size_t ol = strlen(m->open);
        adv(lexer);
        *consumed = true;
        if (ol == 2) {
            if (la(lexer) != m->open[1]) return false;
            adv(lexer);
        }
        lexer->mark_end(lexer);
        int32_t next = la(lexer);
        if (next == 0 || is_space(next)) return false;
        if (ol == 1 && !m->intraword && next == m->open[0]) return false;  // `**`, `==`, `--`
        if (!closer_exists(s, lexer, m)) return false;
        s->prev = clamp_char(m->open[ol - 1]);
        s->open_mask = (uint16_t)(s->open_mask | (1u << i));
        lexer->result_symbol = m->open_tok;
        return true;
    }
    return false;
}

// Could a token start at this character (so plain text must stop here)?
static bool is_special(Scanner *s, int32_t c, const bool *valid) {
    switch (c) {
        case '\\': case '`': case '$': case '[': return true;
        case ']': return s->link_mode != LINK_NONE;
        case '{': case '}': return top_is_brace(s);
        case '(': case ')': return top_is_paren(s);
        case '/': return true;
        case '|': return true;
        default: break;
    }
    if (!is_marker_char(c)) return false;
    for (size_t i = 0; i < MARKER_COUNT; ++i) {
        const Marker *m = &kMarkers[i];
        if (c != m->open[0] && c != m->close[0]) continue;
        const bool open_here = valid[m->close_tok] || (s->open_mask & (1u << i));
        if (open_here && c == m->close[0] && !is_space(s->prev)) return true;
        if (valid[m->open_tok] && c == m->open[0] && (m->intraword || pre_ok(s->prev))) return true;
    }
    return false;
}

// One inline token at the current position.
static bool scan_inline(Scanner *s, TSLexer *lexer, const bool *valid) {
    int32_t c = la(lexer);
    if (c == 0 || c == '\n' || c == '\r') return false;

    // A table row's indentation belongs to its first pipe, if it has one
    // (a GFM row may start with its first cell instead).
    if (valid[TABLE_PIPE] && valid[TEXT] && s->prev == '\n' && is_blank(c)) {
        while (is_blank(la(lexer))) adv(lexer);
        if (la(lexer) == '|') {
            adv(lexer);
            lexer->mark_end(lexer);
            return emit(s, lexer, TABLE_PIPE, '|');
        }
        lexer->mark_end(lexer);
        return emit(s, lexer, TEXT, ' ');
    }

    if (valid[URL] && s->link_mode != LINK_NONE && (s->link_mode == LINK_URL_ONLY || !valid[TEXT])) {
        int32_t last = 0;
        while (!at_eol(lexer) && la(lexer) != ']') {
            last = la(lexer);
            adv(lexer);
        }
        if (last == 0) return false;
        lexer->mark_end(lexer);
        return emit(s, lexer, URL, last);
    }
    const bool paren = top_is_paren(s);
    const int32_t group_open = paren ? '(' : '{', group_close = paren ? ')' : '}';
    if (valid[ARG_TEXT] && !valid[TEXT] && c != group_close && !(c == ',' && valid[ARG_COMMA])) {
        // (In `\name(arg, text)` the argument ends at its comma.)
        int depth = 0;
        int32_t last = 0;
        while (true) {
            int32_t d = la(lexer);
            if (d == 0) break;
            if (d == '\\') {
                adv(lexer);
                last = d;
                if (!lexer->eof(lexer)) adv(lexer);
                continue;
            }
            if (d == group_open) depth++;
            if (d == group_close) {
                if (depth == 0) break;
                depth--;
            }
            if (d == ',' && depth == 0 && valid[ARG_COMMA]) break;
            last = d;
            adv(lexer);
        }
        if (last == 0) return false;
        lexer->mark_end(lexer);
        return emit(s, lexer, ARG_TEXT, last);
    }
    if (!valid[TEXT]) {
        // Group delimiters around an argument (no text allowed there).
        if (valid[LINK_CLOSE] && c == ']') {
            adv(lexer);
            lexer->mark_end(lexer);
            s->link_mode = LINK_NONE;
            return emit(s, lexer, LINK_CLOSE, ']');
        }
        if (valid[TABLE_PIPE]) {
            // A row's first pipe, after any indentation.
            while (is_blank(la(lexer))) adv(lexer);
            if (la(lexer) != '|') return false;
            adv(lexer);
            lexer->mark_end(lexer);
            return emit(s, lexer, TABLE_PIPE, '|');
        }
        if (valid[PAREN_OPEN] && c == '(') {
            adv(lexer);
            lexer->mark_end(lexer);
            push_group(s, true);
            return emit(s, lexer, PAREN_OPEN, '(');
        }
        if (valid[GROUP_OPEN]) {
            // (blanks allowed between a command's two groups)
            while (is_blank(la(lexer))) adv(lexer);
            if (la(lexer) != '{') return false;
            adv(lexer);
            lexer->mark_end(lexer);
            push_group(s, false);
            return emit(s, lexer, GROUP_OPEN, '{');
        }
        if (valid[ARG_COMMA] && c == ',') {
            // The argument's comma, and the blanks after it.
            adv(lexer);
            while (is_blank(la(lexer))) adv(lexer);
            lexer->mark_end(lexer);
            return emit(s, lexer, ARG_COMMA, ' ');
        }
        if (valid[PAREN_CLOSE] && c == ')' && paren) {
            adv(lexer);
            lexer->mark_end(lexer);
            pop_group(s);
            group_closed(s);
            return emit(s, lexer, PAREN_CLOSE, ')');
        }
        if (valid[GROUP_CLOSE] && c == '}') {
            adv(lexer);
            lexer->mark_end(lexer);
            pop_group(s);
            group_closed(s);
            return emit(s, lexer, GROUP_CLOSE, '}');
        }
        return false;
    }

    // Plain text up to the next character that might start something.
    if (!is_special(s, c, valid)) {
        int32_t last = 0;
        while (true) {
            int32_t d = la(lexer);
            if (d == 0 || d == '\n' || d == '\r' || is_special(s, d, valid)) break;
            adv(lexer);
            last = d;
            s->prev = clamp_char(d);
        }
        lexer->mark_end(lexer);
        return emit(s, lexer, TEXT, last);
    }

    // A special character: try what it could start; if nothing, it is text
    // (the token then covers whatever was consumed while trying).
    bool consumed = false;
    int32_t text_last = c;
    switch (c) {
        case '\\': {
            adv(lexer);
            consumed = true;
            lexer->mark_end(lexer);
            int32_t n = la(lexer);
            if (n == '(' ) {
                // \( ... \): restart as maths from the backslash is not
                // possible (already past it), so scan the rest here.
                adv(lexer);
                lexer->mark_end(lexer);
                bool empty = true;
                while (true) {
                    int32_t d = la(lexer);
                    if (d == 0) break;
                    if (d == '\n' || d == '\r') {
                        if (!scope_continues(s, lexer)) break;
                        continue;
                    }
                    adv(lexer);
                    if (d == '\\' && la(lexer) == ')') {
                        if (empty) break;
                        adv(lexer);
                        lexer->mark_end(lexer);
                        return emit(s, lexer, LATEX, ')');
                    }
                    empty = false;
                }
                // No `\)`: a literal parenthesis (the backslash escapes it).
                return emit(s, lexer, ESCAPE, '(');
            }
            if (is_alpha(n)) {
                if (scan_command(s, lexer, valid)) return true;
                text_last = 'a';
                break;
            }
            if (n != 0 && !is_space(n)) {
                adv(lexer);
                lexer->mark_end(lexer);
                return emit(s, lexer, ESCAPE, n);
            }
            break;
        }
        case '`':
            consumed = true;
            if (scan_verbatim(s, lexer)) return emit(s, lexer, VERBATIM, '`');
            break;
        case '$':
            consumed = true;
            if (scan_latex(s, lexer)) return emit(s, lexer, LATEX, '$');
            break;
        case '[':
            adv(lexer);
            consumed = true;
            lexer->mark_end(lexer);
            if (valid[LINK_OPEN] && s->link_mode == LINK_NONE) {
                enum LinkMode mode = classify_link(lexer);
                if (mode != LINK_NONE) {
                    s->link_mode = mode;
                    return emit(s, lexer, LINK_OPEN, '[');
                }
            }
            break;
        case ']':
            if (valid[LINK_CLOSE] && s->link_mode != LINK_NONE) {
                adv(lexer);
                lexer->mark_end(lexer);
                s->link_mode = LINK_NONE;
                return emit(s, lexer, LINK_CLOSE, ']');
            }
            adv(lexer);
            consumed = true;
            lexer->mark_end(lexer);
            break;
        case '{':
        case '(':
            adv(lexer);
            consumed = true;
            lexer->mark_end(lexer);
            if ((c == '(') == paren) top_depth_add(s, 1);
            break;
        case '}':
        case ')': {
            const bool own = (c == ')') == paren && s->depth_count > 0;
            const enum TokenType close_tok = c == ')' ? PAREN_CLOSE : GROUP_CLOSE;
            if (own && valid[close_tok] && (s->depth_count > MAX_DEPTH || top_depth(s) == 0)) {
                adv(lexer);
                lexer->mark_end(lexer);
                pop_group(s);
                group_closed(s);
                return emit(s, lexer, close_tok, c);
            }
            adv(lexer);
            consumed = true;
            lexer->mark_end(lexer);
            if (own) top_depth_add(s, -1);
            break;
        }
        case '/':
            adv(lexer);
            consumed = true;
            lexer->mark_end(lexer);
            if (la(lexer) == '/' && valid[INLINE_COMMENT] && (is_space(s->prev) || s->prev == '\n')) {
                adv(lexer);
                if (is_space(la(lexer)) || la(lexer) == 0) {
                    while (!at_eol(lexer)) adv(lexer);
                    lexer->mark_end(lexer);
                    return emit(s, lexer, INLINE_COMMENT, 'a');
                }
            }
            break;
        case '|':
            if (valid[TABLE_PIPE]) {
                adv(lexer);
                lexer->mark_end(lexer);
                // The row's trailing whitespace belongs to its last pipe.
                while (is_blank(la(lexer))) adv(lexer);
                if (at_eol(lexer)) lexer->mark_end(lexer);
                return emit(s, lexer, TABLE_PIPE, '|');
            }
            if (valid[LINK_BAR] && s->link_mode == LINK_TEXT) {
                adv(lexer);
                consumed = true;
                lexer->mark_end(lexer);
                if (is_link_bar(lexer)) return emit(s, lexer, LINK_BAR, '|');
                break;
            }
            if (scan_marker(s, lexer, valid, &consumed)) return true;
            break;
        default:
            if (scan_marker(s, lexer, valid, &consumed)) return true;
            break;
    }
    if (!consumed) {
        adv(lexer);
        lexer->mark_end(lexer);
    }
    return emit(s, lexer, TEXT, text_last);
}

// --- block level -------------------------------------------------------------------

static bool is_bibtex_or_directive(const char *name) {
    static const char *const kNames[] = {
        // (not abstract: only \abstract(...) is one; `@abstract{` is prose)
        "import", "citation", "image", "caption", "alttext", "bibliography", "printbibliography", "toc",
        "article", "book", "booklet", "conference", "inbook", "incollection", "inproceedings", "manual",
        "mastersthesis", "misc", "phdthesis", "proceedings", "techreport", "unpublished", "online", "software",
    };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (strcmp(name, kNames[i]) == 0) return true;
    return false;
}

static bool is_callout_keyword(const char *w) {
    static const char *const kWords[] = {"NOTE", "WARNING", "ERROR", "INFO", "TIP", "HINT", "IMPORTANT",
                                         "CAUTION", "TODO", "FIXME", "DANGER", "SUCCESS", "QUESTION", "EXAMPLE"};
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i)
        if (strcmp(w, kWords[i]) == 0) return true;
    return false;
}

// What a line-start branch had already consumed of a line when it gave up
// on it, for deciding whether the line is a GitHub-flavoured Markdown table
// row (src/mepml_doc.cpp: TableCells, IsDelimiterRow, GfmTableAt, GfmRowAt).
typedef struct {
    bool never;      // the line cannot be a table row (a comment)
    int pipes;       // cell pipes seen so far
    bool lead;       // the first non-blank character was a pipe
    bool delimiter;  // everything so far fits a delimiter row (- : = | blanks)
    bool tick;       // inside `verbatim`
    bool trail;      // the last non-blank character was a cell pipe
} LineSoFar;

static const LineSoFar kNothing = {false, 0, false, true, false, false};
static const LineSoFar kNotDelimiter = {false, 0, false, false, false, false};
static const LineSoFar kNever = {true, 0, false, false, false, false};

// Reads the rest of the line (to its end, not past it): its cell pipes,
// whether it ends in one, and whether it is a delimiter row.
static void scan_table_line(TSLexer *lexer, LineSoFar *st) {
    while (!at_eol(lexer)) {
        int32_t d = la(lexer);
        adv(lexer);
        if (d == '\\') {
            st->delimiter = false;
            st->trail = false;
            if (!at_eol(lexer)) adv(lexer);
            continue;
        }
        if (is_blank(d)) continue;
        if (d == '`') st->tick = !st->tick;
        if (d == '|' && !st->tick) {
            st->pipes++;
            st->trail = true;
            continue;
        }
        st->trail = false;
        if (d != '-' && d != ':' && d != '=') st->delimiter = false;
    }
}

// Cells in a scanned line, as TableCells counts them.
static int table_cells(const LineSoFar *st) {
    return st->pipes == 0 ? 0 : st->pipes + 1 - (st->lead ? 1 : 0) - (st->trail ? 1 : 0);
}

// The next line is a delimiter row with `cells` cells, and not a list
// item (`- | -`). Called at the end of the current line.
static bool next_line_delimits(TSLexer *lexer, int cells) {
    if (la(lexer) == '\r') adv(lexer);
    if (la(lexer) != '\n') return false;
    adv(lexer);
    while (is_blank(la(lexer))) adv(lexer);
    LineSoFar st = kNothing;
    int32_t c = la(lexer);
    if (c == '|') {
        st.lead = true;
        st.trail = true;
        st.pipes = 1;
        adv(lexer);
    } else if (c == '-' || c == '*' || c == '+') {
        adv(lexer);
        if (is_blank(la(lexer))) return false;  // a list item
        if (c != '-') st.delimiter = false;
    } else if (is_digit(c)) {
        return false;  // a list item, or at least not a delimiter row
    }
    scan_table_line(lexer, &st);
    const int got = table_cells(&st);
    return st.delimiter && got == cells && got > 0;
}

// A GFM table row, or the delimiter row under its header: a line with a
// cell pipe that is no other block's start. Zero-width, except that a
// delimiter row owns its line.
static bool table_line(Scanner *s, TSLexer *lexer, const bool *valid, LineSoFar st) {
    if (st.never || (!valid[TABLE_ROW_START] && !valid[TABLE_DELIMITER_ROW])) return false;
    scan_table_line(lexer, &st);
    const int cells = table_cells(&st);
    if (cells <= 0) return false;
    if (s->table == TABLE_GFM) {
        if (st.delimiter && valid[TABLE_DELIMITER_ROW]) {
            lexer->mark_end(lexer);
            return emit(s, lexer, TABLE_DELIMITER_ROW, '|');
        }
        if (valid[TABLE_ROW_START]) {
            s->context = CTX_LINE;
            s->prev = '\n';
            lexer->result_symbol = TABLE_ROW_START;
            return true;
        }
        return false;
    }
    if (valid[TABLE_ROW_START] && next_line_delimits(lexer, cells)) {
        s->table = TABLE_GFM;
        s->context = CTX_LINE;
        s->prev = '\n';
        lexer->result_symbol = TABLE_ROW_START;
        return true;
    }
    return false;
}

// The next line is a \caption or \alttext (or @caption, @alttext). Called
// at the end of a line.
static bool next_line_is_attribute(TSLexer *lexer) {
    if (la(lexer) == '\r') adv(lexer);
    if (la(lexer) != '\n') return false;
    adv(lexer);
    while (is_blank(la(lexer))) adv(lexer);
    if (la(lexer) != '@' && la(lexer) != '\\') return false;
    adv(lexer);
    char name[10] = {0};
    int n = 0;
    while (is_alpha(la(lexer)) && n < 9) {
        name[n++] = (char)la(lexer);
        adv(lexer);
    }
    return !is_alpha(la(lexer)) && (strcmp(name, "caption") == 0 || strcmp(name, "alttext") == 0);
}

// Zero-width fallbacks when a line is not any block start.
static bool fallback_line_after(Scanner *s, TSLexer *lexer, const bool *valid, bool indented, LineSoFar st) {
    if (table_line(s, lexer, valid, st)) return true;
    if (indented && valid[LIST_CONTINUATION]) {
        s->context = CTX_LINE;
        lexer->result_symbol = LIST_CONTINUATION;
        return true;
    }
    if (valid[PARAGRAPH_START]) {
        s->context = CTX_PARAGRAPH;
        s->prev = '\n';
        lexer->result_symbol = PARAGRAPH_START;
        return true;
    }
    return false;
}

static bool fallback_line(Scanner *s, TSLexer *lexer, const bool *valid, bool indented) {
    return fallback_line_after(s, lexer, valid, indented, kNotDelimiter);
}

// Called at the start of a line where some block-level token is valid.
// mark_end stays at the line start until a token that owns a prefix of the
// line is certain.
static bool scan_line_start(Scanner *s, TSLexer *lexer, const bool *valid) {
    lexer->mark_end(lexer);
    int indent = 0;
    while (is_blank(la(lexer))) {
        adv(lexer);
        indent++;
    }
    int32_t c = la(lexer);

    if (c == 0 && indent == 0) return false;
    if (c == 0 || c == '\n' || c == '\r') {
        if (!valid[BLANK_LINE]) return false;
        if (c == '\r') adv(lexer);
        if (la(lexer) == '\n') adv(lexer);
        lexer->mark_end(lexer);
        s->prev = '\n';
        s->context = CTX_PARAGRAPH;
        lexer->result_symbol = BLANK_LINE;
        return true;
    }

    // The open box's `)`, alone on its line (a `// comment` may follow):
    // boxes close before the slide they are on.
    if (s->boxes && c == ')') {
        adv(lexer);
        if (slide_closer_rest(lexer) && valid[BOX_END]) {
            s->boxes--;
            s->context = CTX_LINE;
            lexer->result_symbol = BOX_END;
            return true;
        }
        return fallback_line(s, lexer, valid, indent > 0);
    }

    // The open slide's closing bracket, alone on its line (a `// comment`
    // may follow). Zero-width: the grammar reads the bracket itself.
    if (s->slide && c == s->slide) {
        adv(lexer);
        if (slide_closer_rest(lexer) && valid[SLIDE_END]) {
            s->slide = 0;
            s->context = CTX_LINE;
            lexer->result_symbol = SLIDE_END;
            return true;
        }
        return fallback_line(s, lexer, valid, indent > 0);
    }

    // `//` family: meta, results markers, callouts, comments.
    if (c == '/') {
        adv(lexer);
        if (la(lexer) != '/') return fallback_line(s, lexer, valid, indent > 0);
        adv(lexer);
        if (la(lexer) == '?') {
            if (!valid[META_MARKER]) return fallback_line_after(s, lexer, valid, indent > 0, kNever);
            adv(lexer);
            lexer->mark_end(lexer);
            s->context = CTX_LINE;
            return emit(s, lexer, META_MARKER, '?');
        }
        if (!valid[COMMENT_MARKER] && !valid[CALLOUT_MARKER] && !valid[RESULT_BEGIN] && !valid[RESULT_END] &&
            !valid[RESULT_BEGIN_MARKDOWN] && !valid[RESULT_END_ATTACHED])
            return fallback_line_after(s, lexer, valid, indent > 0, kNever);
        bool spaced = false;
        while (is_blank(la(lexer))) {
            adv(lexer);
            spaced = true;
        }
        (void)spaced;
        lexer->mark_end(lexer);  // a comment-family line for certain
        char word[24] = {0};
        int n = 0;
        while ((is_alpha(la(lexer)) || la(lexer) == '_') && n < 23) {
            word[n++] = (char)la(lexer);
            adv(lexer);
        }
        const bool begin = (valid[RESULT_BEGIN] || valid[RESULT_BEGIN_MARKDOWN]) && strcmp(word, "result_begin") == 0;
        const bool end = (valid[RESULT_END] || valid[RESULT_END_ATTACHED]) && strcmp(word, "result_end") == 0;
        if (begin || end) {
            if (la(lexer) == ':') adv(lexer);
            while (is_blank(la(lexer))) adv(lexer);
            // The opening marker may name the results' kind: `// result_begin: html`.
            char kind[16] = {0};
            int kn = 0;
            if (begin) {
                while (is_alpha(la(lexer))) {
                    int32_t k = la(lexer);
                    if (kn < 15) kind[kn++] = (char)(is_upper(k) ? k - 'A' + 'a' : k);
                    adv(lexer);
                }
                while (is_blank(la(lexer))) adv(lexer);
            }
            if (at_eol(lexer)) {
                lexer->mark_end(lexer);
                s->context = CTX_LINE;
                if (begin) {
                    // Markdown results are the document's own blocks, up to
                    // their closing marker.
                    const bool markdown = strcmp(kind, "markdown") == 0 || strcmp(kind, "md") == 0;
                    if (markdown && valid[RESULT_BEGIN_MARKDOWN]) return emit(s, lexer, RESULT_BEGIN_MARKDOWN, ':');
                    if (!markdown && valid[RESULT_BEGIN]) return emit(s, lexer, RESULT_BEGIN, ':');
                } else {
                    // Closing Markdown results that end in a table (image,
                    // maths) captioned on the next line: the caption is that
                    // block's, so the marker is too.
                    if (valid[RESULT_END_ATTACHED] && next_line_is_attribute(lexer))
                        return emit(s, lexer, RESULT_END_ATTACHED, ':');
                    if (valid[RESULT_END]) return emit(s, lexer, RESULT_END, ':');
                }
            }
        }
        if (valid[CALLOUT_MARKER] && n > 0 && la(lexer) == ':' && is_callout_keyword(word)) {
            bool upper = true;
            for (int i = 0; i < n; ++i) upper = upper && is_upper(word[i]);
            if (upper) {
                s->context = CTX_LINE;
                return emit(s, lexer, CALLOUT_MARKER, ' ');
            }
        }
        if (valid[COMMENT_MARKER]) {
            s->context = CTX_LINE;
            return emit(s, lexer, COMMENT_MARKER, ' ');
        }
        return false;
    }

    // ``` fences.
    if (c == '`') {
        LineSoFar st = kNotDelimiter;
        adv(lexer);
        st.tick = !st.tick;
        if (la(lexer) == '`') {
            adv(lexer);
            st.tick = !st.tick;
            if (la(lexer) == '`') {
                adv(lexer);
                st.tick = !st.tick;
                if (valid[FENCE_OPEN]) {
                    lexer->mark_end(lexer);
                    return emit(s, lexer, FENCE_OPEN, '`');
                }
            }
        }
        return fallback_line_after(s, lexer, valid, indent > 0, st);
    }

    // \name(...) directives.
    if (c == '\\' && (valid[DIRECTIVE_START] || valid[ATTRIBUTE_START] || valid[SLIDE_START] || valid[COMMAND_BLOCK_START] ||
                      valid[BOX_START])) {
        adv(lexer);
        if (is_alpha(la(lexer))) {
            char name[64];
            const int n = read_name(lexer, name, (int)sizeof name);
            const bool directive = n < 16 && is_backslash_directive(name) &&
                                   (la(lexer) == '(' || is_blank(la(lexer)) || at_eol(lexer));
            // \slide( or \slide{ opens a slide (not inside another one).
            if (!directive && strcmp(name, "slide") == 0 && (la(lexer) == '(' || la(lexer) == '{') && !s->slide &&
                valid[SLIDE_START]) {
                s->slide = la(lexer) == '(' ? ')' : '}';
                s->context = CTX_LINE;
                lexer->result_symbol = SLIDE_START;
                return true;
            }
            // \definition( and its kin open a box -- or are one, closed
            // where their text ends.
            if (!directive && n < (int)sizeof name && is_box_kind(name) && la(lexer) == '(' && valid[BOX_START] &&
                valid[BOX_LINE_START]) {
                bool lead = false;
                const bool closed = box_closes(lexer, s->slide, &lead, strcmp(name, "boxed") == 0 ? 1 : 0);
                if (!closed) s->boxes++;
                s->box_lead = lead;
                s->context = CTX_LINE;
                lexer->result_symbol = closed ? BOX_LINE_START : BOX_START;
                return true;
            }
            if (directive) {
                // A caption or alt text is its own start, so the line under
                // a captioned block can tell it from the next directive.
                const bool attribute = strcmp(name, "caption") == 0 || strcmp(name, "alttext") == 0;
                if (valid[attribute ? ATTRIBUTE_START : DIRECTIVE_START]) {
                    s->context = CTX_LINE;
                    lexer->result_symbol = attribute ? ATTRIBUTE_START : DIRECTIVE_START;
                    return true;
                }
            }
            // \define(, and \raw( or a user command's call whose group
            // ends a line: a block of its own.
            if (valid[COMMAND_BLOCK_START] && la(lexer) == '(' &&
                (strcmp(name, "define") == 0 || strcmp(name, "raw") == 0 || n >= (int)sizeof name || !is_builtin_command(name))) {
                bool crossed = false;
                if (strcmp(name, "define") == 0 || group_ends_line_crossed(lexer, &crossed)) {
                    s->context = CTX_LINE;
                    lexer->result_symbol = COMMAND_BLOCK_START;
                    return true;
                }
                if (crossed) return fallback_line_after(s, lexer, valid, indent > 0, kNever);
            }
            return fallback_line(s, lexer, valid, indent > 0);
        }
        if (la(lexer) == '[' && valid[MATH_OPEN]) {
            adv(lexer);
            lexer->mark_end(lexer);
            s->math_kind = MATH_BRACKET;
            return emit(s, lexer, MATH_OPEN, '[');
        }
        if (!at_eol(lexer)) adv(lexer);
        return fallback_line(s, lexer, valid, indent > 0);
    }

    // Display maths.
    if ((c == '$' || c == '\\') && valid[MATH_OPEN]) {
        adv(lexer);
        if ((c == '$' && la(lexer) == '$') || (c == '\\' && la(lexer) == '[')) {
            adv(lexer);
            lexer->mark_end(lexer);
            s->math_kind = c == '$' ? MATH_DOLLARS : MATH_BRACKET;
            return emit(s, lexer, MATH_OPEN, c == '$' ? '$' : '[');
        }
        // A backslash escapes what follows it (a `\|` is no cell pipe).
        if (c == '\\' && !at_eol(lexer)) adv(lexer);
        return fallback_line(s, lexer, valid, indent > 0);
    }

    // > headings (column 0 only).
    if (c == '>' && indent == 0) {
        int level = 0;
        while (la(lexer) == '>') {
            adv(lexer);
            level++;
        }
        if (level <= 6 && (is_blank(la(lexer)) || at_eol(lexer)) && valid[H1_MARKER + level - 1]) {
            while (is_blank(la(lexer))) adv(lexer);
            lexer->mark_end(lexer);
            s->context = CTX_LINE;
            s->prev = ' ';
            lexer->result_symbol = H1_MARKER + level - 1;
            return true;
        }
        return fallback_line(s, lexer, valid, false);
    }

    // Rules and bullet lists: - * + = _
    if (c == '-' || c == '*' || c == '+' || c == '=' || c == '_') {
        adv(lexer);
        if (is_blank(la(lexer)) && c != '=' && c != '_') {
            if (valid[LIST_MARKER]) {
                while (is_blank(la(lexer))) adv(lexer);
                lexer->mark_end(lexer);
                s->context = CTX_LINE;
                s->prev = ' ';
                return emit(s, lexer, LIST_MARKER, ' ');
            }
            return fallback_line_after(s, lexer, valid, indent > 0, kNever);  // a list item's shape
        }
        if (c != '+') {
            int count = 1;
            while (la(lexer) == c) {
                adv(lexer);
                count++;
            }
            while (is_blank(la(lexer))) adv(lexer);
            if (count >= 3 && at_eol(lexer) && valid[RULE]) {
                lexer->mark_end(lexer);
                return emit(s, lexer, RULE, c);
            }
        }
        return fallback_line_after(s, lexer, valid, indent > 0, c == '-' || c == '=' ? kNothing : kNotDelimiter);
    }

    // Numbered lists.
    if (is_digit(c)) {
        int digits = 0;
        while (is_digit(la(lexer))) {
            adv(lexer);
            digits++;
        }
        if (digits <= 9 && (la(lexer) == '.' || la(lexer) == ')')) {
            adv(lexer);
            if (is_blank(la(lexer)) && valid[LIST_MARKER]) {
                while (is_blank(la(lexer))) adv(lexer);
                lexer->mark_end(lexer);
                s->context = CTX_LINE;
                s->prev = ' ';
                return emit(s, lexer, LIST_MARKER, ' ');
            }
        }
        return fallback_line(s, lexer, valid, indent > 0);
    }

    // Tables: a line whose trimmed text starts and ends with `|` (and,
    // for a GitHub-flavoured Markdown table, lines with a pipe anywhere --
    // see table_line).
    if (c == '|') {
        bool delimiter = true;
        int cells = 0;
        int32_t last = 0;
        bool tick = false;
        // The same line as table_line reads it (escapes count), for when
        // it is not a mepml row but may be a GFM one.
        LineSoFar st = kNothing;
        st.lead = true;
        bool escaped = false;
        while (!at_eol(lexer)) {
            int32_t d = la(lexer);
            if (d == '`') tick = !tick;
            if (!is_blank(d)) last = d;
            if (d == '|' && !tick) cells++;
            else if (!is_blank(d) && d != '-' && d != ':' && d != '=') delimiter = false;
            if (escaped) {
                escaped = false;
                st.trail = false;
            } else if (d == '\\') {
                escaped = true;
                st.delimiter = false;
                st.trail = false;
            } else if (!is_blank(d)) {
                if (d == '`') st.tick = !st.tick;
                if (d == '|' && !st.tick) {
                    st.pipes++;
                    st.trail = true;
                } else {
                    st.trail = false;
                    if (d != '-' && d != ':' && d != '=') st.delimiter = false;
                }
            }
            adv(lexer);
        }
        if (last == '|' && cells >= 2) {
            if (delimiter && valid[TABLE_DELIMITER_ROW]) {
                lexer->mark_end(lexer);
                return emit(s, lexer, TABLE_DELIMITER_ROW, '|');
            }
            if (valid[TABLE_ROW_START]) {
                // A new table: GFM when a delimiter row follows its header.
                if (s->table == TABLE_NONE)
                    s->table = next_line_delimits(lexer, table_cells(&st)) ? TABLE_GFM : TABLE_MEPML;
                s->context = CTX_LINE;
                s->prev = '\n';
                lexer->result_symbol = TABLE_ROW_START;
                return true;
            }
        }
        return fallback_line_after(s, lexer, valid, indent > 0, st);
    }

    // @directives.
    if (c == '@') {
        adv(lexer);
        char name[24] = {0};
        int n = 0;
        while (is_alpha(la(lexer)) && n < 23) {
            name[n++] = (char)la(lexer);
            adv(lexer);
        }
        const bool attribute = strcmp(name, "caption") == 0 || strcmp(name, "alttext") == 0;
        if (strcmp(name, "slide") == 0 && la(lexer) == '{' && !s->slide && valid[SLIDE_START]) {
            s->slide = '}';
            s->context = CTX_LINE;
            lexer->result_symbol = SLIDE_START;
            return true;
        }
        if (n > 0 && !is_alpha(la(lexer)) && is_bibtex_or_directive(name) && valid[attribute ? ATTRIBUTE_START : DIRECTIVE_START]) {
            s->context = CTX_LINE;
            lexer->result_symbol = attribute ? ATTRIBUTE_START : DIRECTIVE_START;
            return true;
        }
        return fallback_line(s, lexer, valid, indent > 0);
    }

    return fallback_line_after(s, lexer, valid, indent > 0, kNothing);
}

// Is the current line (lexer at its first character) a closing ``` fence?
static bool at_fence_close_line(TSLexer *lexer, bool consume) {
    (void)consume;
    while (is_blank(la(lexer))) adv(lexer);
    for (int i = 0; i < 3; ++i) {
        if (la(lexer) != '`') return false;
        adv(lexer);
    }
    while (is_blank(la(lexer))) adv(lexer);
    return at_eol(lexer);
}

bool tree_sitter_mepml_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid) {
    Scanner *s = (Scanner *)payload;
    if (valid[ERROR_SENTINEL]) return false;
    // Whether the lexer is at a line start -- asked only when a token that
    // needs one is valid: get_column walks back to the line's start, so
    // asking on every call made each token cost its column, and a long
    // line quadratic (an 8000-word paragraph on one line: 65 ms;
    // plans/MEPML_PERFORMANCE_PLAN.md).
    int col0_cached = -1;
#define COL0() (col0_cached < 0 ? (col0_cached = lexer->get_column(lexer) == 0) : col0_cached)

    // A \define's template or a \raw's text: everything up to its group's
    // closing `)` -- over any number of lines, blank ones too -- unparsed
    // (ReadGroup in mepml_doc.cpp: only parentheses nest, `\x` escapes).
    if (valid[OPAQUE_TEXT] && la(lexer) != ')' && !lexer->eof(lexer)) {
        int depth = 0;
        int32_t last = 0;
        while (!lexer->eof(lexer)) {
            const int32_t c = la(lexer);
            if (c == ')' && depth == 0) break;
            adv(lexer);
            last = c;
            if (c == '\\') {
                if (!lexer->eof(lexer)) {
                    last = la(lexer);
                    adv(lexer);
                }
                continue;
            }
            if (c == '(') depth++;
            if (c == ')') depth--;
        }
        lexer->mark_end(lexer);
        return emit(s, lexer, OPAQUE_TEXT, last);
    }

    // Code block body: everything up to the closing fence, as one token.
    if ((valid[CODE_CONTENT] || valid[FENCE_CLOSE]) && COL0()) {
        lexer->mark_end(lexer);
        if (at_fence_close_line(lexer, true)) {
            if (!valid[FENCE_CLOSE]) return false;
            lexer->mark_end(lexer);
            return emit(s, lexer, FENCE_CLOSE, '`');
        }
        if (!valid[CODE_CONTENT]) return false;
        // Not a fence: consume whole lines until one is (or EOF).
        while (true) {
            while (!at_eol(lexer)) adv(lexer);
            if (la(lexer) == '\r') adv(lexer);
            if (la(lexer) == '\n') adv(lexer);
            lexer->mark_end(lexer);
            if (lexer->eof(lexer)) break;
            if (at_fence_close_line(lexer, false)) break;
        }
        return emit(s, lexer, CODE_CONTENT, '\n');
    }

    // Text after a display-maths closer on its line (grammar.js's
    // display_math): anything but blanks or a `//` comment, which
    // _line_end reads itself. Nothing is consumed at a line end or a
    // `/` right after the closer, so the rest of this function still
    // reads the newline or the comment; after blanks, returning false
    // hands them back to _ws.
    if (valid[MATH_TRAILING] && !at_eol(lexer) && !lexer->eof(lexer) && la(lexer) != '/') {
        while (la(lexer) == ' ' || la(lexer) == '\t') adv(lexer);
        int32_t last = 0;
        if (la(lexer) == '/') {
            adv(lexer);
            if (la(lexer) == '/') return false;
            last = '/';
        } else if (at_eol(lexer) || lexer->eof(lexer)) {
            return false;
        }
        while (!at_eol(lexer) && !lexer->eof(lexer)) {
            last = la(lexer);
            adv(lexer);
        }
        lexer->mark_end(lexer);
        return emit(s, lexer, MATH_TRAILING, last);
    }

    // Display maths body and closer.
    if (valid[MATH_CONTENT] || valid[MATH_CLOSE]) {
        const char close0 = s->math_kind == MATH_BRACKET ? '\\' : '$';
        const char close1 = s->math_kind == MATH_BRACKET ? ']' : '$';
        if (la(lexer) == close0) {
            adv(lexer);
            if (la(lexer) == close1 && valid[MATH_CLOSE]) {
                adv(lexer);
                lexer->mark_end(lexer);
                s->math_kind = MATH_NONE;
                return emit(s, lexer, MATH_CLOSE, close1);
            }
        }
        if (!valid[MATH_CONTENT]) return false;
        int32_t last = 0;
        while (!lexer->eof(lexer)) {
            int32_t d = la(lexer);
            if (d == close0) {
                lexer->mark_end(lexer);
                adv(lexer);
                if (la(lexer) == close1) {
                    return last ? emit(s, lexer, MATH_CONTENT, last) : false;
                }
                last = d;
                continue;
            }
            last = d;
            adv(lexer);
        }
        lexer->mark_end(lexer);
        return last ? emit(s, lexer, MATH_CONTENT, last) : false;
    }

    // A box's `(`: its group opens; text after its title reads as a
    // paragraph's, over the lines it runs on.
    if (valid[BOX_OPEN] && la(lexer) == '(') {
        adv(lexer);
        lexer->mark_end(lexer);
        push_group(s, true);
        s->box_level = s->depth_count;
        s->context = s->box_lead ? CTX_PARAGRAPH : CTX_LINE;
        return emit(s, lexer, BOX_OPEN, '(');
    }
    // A line break in a box's first paragraph; where the paragraph ends,
    // the box's opening lines end too.
    if ((valid[BOX_BREAK] || valid[NEWLINE]) && s->box_level > 0 && s->depth_count >= s->box_level &&
        (la(lexer) == '\n' || la(lexer) == '\r')) {
        if (la(lexer) == '\r') adv(lexer);
        if (la(lexer) == '\n') {
            adv(lexer);
            lexer->mark_end(lexer);
            s->prev = '\n';
            if (s->box_lead && valid[BOX_BREAK] && !line_starts_block(lexer, s->slide, s->boxes > 0)) {
                s->context = CTX_PARAGRAPH;
                lexer->result_symbol = BOX_BREAK;
                return true;
            }
            if (!valid[NEWLINE]) return false;
            box_line_done(s);
            s->context = CTX_PARAGRAPH;
            lexer->result_symbol = NEWLINE;
            return true;
        }
    }

    // \abstract(: its group opens, and its prose reads as a paragraph's.
    if (valid[ABSTRACT_OPEN] && la(lexer) == '(') {
        adv(lexer);
        lexer->mark_end(lexer);
        push_group(s, true);
        s->abstract_level = s->depth_count;
        s->context = CTX_PARAGRAPH;
        s->prev = '\n';
        lexer->result_symbol = ABSTRACT_OPEN;
        return true;
    }
    // A line break inside an abstract, with any blank lines after it (a
    // paragraph break).
    if (valid[ABSTRACT_BREAK] && in_abstract(s) && (la(lexer) == '\n' || la(lexer) == '\r')) {
        while (true) {
            if (la(lexer) == '\r') adv(lexer);
            if (la(lexer) != '\n') break;
            adv(lexer);
            lexer->mark_end(lexer);
            while (is_blank(la(lexer))) adv(lexer);
            if (la(lexer) != '\n' && la(lexer) != '\r') break;
        }
        s->prev = '\n';
        s->context = CTX_PARAGRAPH;
        lexer->result_symbol = ABSTRACT_BREAK;
        return true;
    }

    // Line starts.
    {
        bool block_valid = valid[BLANK_LINE] || valid[PARAGRAPH_START] || valid[COMMENT_MARKER] ||
                           valid[META_MARKER] || valid[H1_MARKER] || valid[LIST_MARKER] ||
                           valid[LIST_CONTINUATION] || valid[TABLE_ROW_START] || valid[RESULT_END] ||
                           valid[RESULT_END_ATTACHED] || valid[RESULT_BEGIN_MARKDOWN] ||
                           valid[DIRECTIVE_START] || valid[ATTRIBUTE_START] || valid[FENCE_OPEN] ||
                           valid[SLIDE_START] || valid[SLIDE_END] || valid[COMMAND_BLOCK_START] || valid[BOX_START] ||
                           valid[BOX_END];
        if (block_valid && COL0()) {
            const bool ok = scan_line_start(s, lexer, valid);
            if (ok && lexer->result_symbol != TABLE_ROW_START && lexer->result_symbol != TABLE_DELIMITER_ROW)
                s->table = TABLE_NONE;
            return ok;
        }
    }
#undef COL0

    // Line ends.
    if (valid[NEWLINE]) {
        if (la(lexer) == '\r') {
            adv(lexer);
        }
        if (la(lexer) == '\n') {
            adv(lexer);
            lexer->mark_end(lexer);
            s->prev = '\n';
            if (s->context == CTX_LINE && !valid[TEXT]) s->context = CTX_PARAGRAPH;
            lexer->result_symbol = NEWLINE;
            return true;
        }
        if (lexer->eof(lexer) && s->prev != '\n') {
            lexer->mark_end(lexer);
            s->prev = '\n';
            box_line_done(s);
            lexer->result_symbol = NEWLINE;
            return true;
        }
    }

    // [ ] / [x] task markers right after a list marker.
    if (valid[TASK_MARKER] && la(lexer) == '[') {
        adv(lexer);
        lexer->mark_end(lexer);
        int32_t mid = la(lexer);
        if (mid != ' ' && mid != 'x' && mid != 'X') {
            // Not a task marker; perhaps a link (the lexer is just past '[').
            if (valid[LINK_OPEN] && s->link_mode == LINK_NONE) {
                enum LinkMode mode = classify_link(lexer);
                if (mode != LINK_NONE) {
                    s->link_mode = mode;
                    return emit(s, lexer, LINK_OPEN, '[');
                }
            }
            return valid[TEXT] ? emit(s, lexer, TEXT, '[') : false;
        }
        {
            adv(lexer);
            if (la(lexer) == ']') {
                adv(lexer);
                if (is_blank(la(lexer)) || at_eol(lexer)) {
                    while (is_blank(la(lexer))) adv(lexer);
                    lexer->mark_end(lexer);
                    return emit(s, lexer, TASK_MARKER, ' ');
                }
            }
        }
        // "[x..." that is not a task marker: the '[' alone is text.
        return valid[TEXT] ? emit(s, lexer, TEXT, '[') : false;
    }

    // \alttext(...) (or @alttext{...}) right after inline maths.
    if (valid[ALTTEXT_MARKER] && la(lexer) == '@') {
        adv(lexer);
        lexer->mark_end(lexer);
        if (match_word(lexer, "alttext") && la(lexer) == '{') {
            lexer->mark_end(lexer);
            return emit(s, lexer, ALTTEXT_MARKER, 't');
        }
        return valid[TEXT] ? emit(s, lexer, TEXT, '@') : false;
    }
    if (valid[ALTTEXT_MARKER] && la(lexer) == '\\') {
        adv(lexer);
        lexer->mark_end(lexer);
        char name[64];
        const bool named = is_alpha(la(lexer)) && read_command_name(lexer, name);
        if (named && strcmp(name, "alttext") == 0 && la(lexer) == '(') {
            lexer->mark_end(lexer);
            return emit(s, lexer, ALTTEXT_MARKER, 't');
        }
        // Some other command after the maths.
        if (named && scan_command_named(s, lexer, valid, name)) return true;
        if (!named && !at_eol(lexer) && !is_space(la(lexer)) && la(lexer) != '(') {
            adv(lexer);
            lexer->mark_end(lexer);
            return emit(s, lexer, ESCAPE, 'a');
        }
        return valid[TEXT] ? emit(s, lexer, TEXT, 'a') : false;
    }

    // A trailing // comment after a directive or display maths.
    if (valid[INLINE_COMMENT] && !valid[TEXT] && la(lexer) == '/') {
        adv(lexer);
        if (la(lexer) != '/') return false;
        while (!at_eol(lexer)) adv(lexer);
        lexer->mark_end(lexer);
        return emit(s, lexer, INLINE_COMMENT, 'a');
    }

    return scan_inline(s, lexer, valid);
}
