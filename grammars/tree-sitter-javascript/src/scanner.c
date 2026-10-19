// The external scanner for mep's JavaScript grammar: what ECMA-262 decides
// from context rather than from the characters alone.
//
//   _automatic_semicolon  §12.10: a zero-width `;` where a line break (or a
//                         `}`, or the end of input) ends a statement the
//                         next token could not continue.
//   _template_chars       a template literal's text between `${...}`s.
//   _ternary_qmark        a conditional's `?`, as opposed to `?.` / `??`.
//   html_comment          Annex B.1.1's `<!--` and `-->` line comments.
//   jsx_text              text between JSX tags.
//
// `||`, escape_sequence and regex_pattern are never produced here; the
// parser marking them valid is how the scanner knows a statement ends in
// an expression (`||` could follow), or that it is inside a string,
// template or regex (where `<!--` is just text). It keeps no state.

#include "tree_sitter/parser.h"

#include <wctype.h>

enum TokenType {
    AUTOMATIC_SEMICOLON,
    TEMPLATE_CHARS,
    TERNARY_QMARK,
    HTML_COMMENT,
    LOGICAL_OR,
    ESCAPE_SEQUENCE,
    REGEX_PATTERN,
    JSX_TEXT,
};

void *tree_sitter_javascript_external_scanner_create(void) { return NULL; }
void tree_sitter_javascript_external_scanner_destroy(void *payload) { (void)payload; }
unsigned tree_sitter_javascript_external_scanner_serialize(void *payload, char *buffer) {
    (void)payload;
    (void)buffer;
    return 0;
}
void tree_sitter_javascript_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    (void)payload;
    (void)buffer;
    (void)length;
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// §12.3 LineTerminator.
static bool is_line_terminator(int32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

// Template text runs up to the closing backtick, a `${`, or a backslash
// (escape sequences are the grammar's tokens). Empty text is no token.
static bool scan_template_chars(TSLexer *lexer) {
    lexer->result_symbol = TEMPLATE_CHARS;
    for (bool has_content = false;; has_content = true) {
        lexer->mark_end(lexer);
        switch (lexer->lookahead) {
        case '`':
        case '\\':
            return has_content;
        case '\0':
            if (lexer->eof(lexer)) return false;
            advance(lexer);
            break;
        case '$':
            advance(lexer);
            if (lexer->lookahead == '{') return has_content;
            break;
        default:
            advance(lexer);
        }
    }
}

// Skips a block comment's text and its `*/` (the `/*` already skipped).
// False if the input ends first.
static bool skip_block_comment(TSLexer *lexer, bool *saw_line_break) {
    for (;;) {
        if (lexer->eof(lexer)) return false;
        if (lexer->lookahead == '*') {
            skip(lexer);
            if (lexer->lookahead == '/') {
                skip(lexer);
                return true;
            }
        } else {
            if (is_line_terminator(lexer->lookahead)) *saw_line_break = true;
            skip(lexer);
        }
    }
}

// Skips whitespace and comments. Returns false on a `/` that starts
// neither (a division or a regex: the next token, not a comment).
// *saw_line_break is set if a line terminator was skipped -- a block
// comment holding one counts as a line break (§12.4).
static bool skip_whitespace_and_comments(TSLexer *lexer, bool *saw_line_break, bool *saw_comment) {
    for (;;) {
        while (iswspace(lexer->lookahead) || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029 ||
               lexer->lookahead == 0xFEFF) {
            if (is_line_terminator(lexer->lookahead)) *saw_line_break = true;
            skip(lexer);
        }
        if (lexer->lookahead != '/') return true;
        skip(lexer);
        if (lexer->lookahead == '/') {
            while (!lexer->eof(lexer) && !is_line_terminator(lexer->lookahead)) skip(lexer);
            *saw_comment = true;
        } else if (lexer->lookahead == '*') {
            skip(lexer);
            *saw_comment = true;
            if (!skip_block_comment(lexer, saw_line_break)) return true;
        } else {
            return false;
        }
    }
}

// At a `//` comment (its first `/` skipped): skips it, then whitespace
// and further comments, stopping at the next token, at a `/` that starts
// none, or just after a block comment not directly followed by another.
enum LookPast { AT_TOKEN, AT_SLASH, AFTER_BLOCK_COMMENT, AT_EOF_IN_COMMENT };

static enum LookPast look_past_line_comment(TSLexer *lexer) {
    bool in_line_comment = true;
    bool ignored = false;
    for (;;) {
        if (in_line_comment) {
            while (!lexer->eof(lexer) && !is_line_terminator(lexer->lookahead)) skip(lexer);
        }
        while (iswspace(lexer->lookahead) || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) skip(lexer);
        if (lexer->lookahead != '/') return AT_TOKEN;
        skip(lexer);
        if (lexer->lookahead == '/') {
            in_line_comment = true;
            continue;
        }
        if (lexer->lookahead != '*') return AT_SLASH;
        skip(lexer);
        if (!skip_block_comment(lexer, &ignored)) return AT_EOF_IN_COMMENT;
        if (lexer->lookahead != '/') return AFTER_BLOCK_COMMENT;
        in_line_comment = false;
    }
}

// §12.10.1's first rule, approximated by the next token's first
// characters: after a line break a `;` is inserted unless what follows can
// continue the statement (an operator, `(`, `[`, a template, `.`, `?`,
// `in`/`instanceof`). `++`/`--` start the next statement (the restricted
// productions of §12.10.1); so does `!` other than `!=`, and `.5`.
//
// Comments on the statement's line also decide which side of the `;` they
// fall (the old grammar's placement, kept for its trees). A `//` comment
// there stays inside the statement -- the scan stops, the parser lexes the
// comment as an extra and asks again after it -- if the statement ends in
// an expression (`||` could follow) and the next token is on a following
// line. The `;` goes before it, leaving it to the enclosing block, if a
// `}` follows, or if the statement has no open expression (`return // c`).
// The look past the comment ends after a block comment; then the rest of
// that line decides, as if it were the statement's.
static bool scan_automatic_semicolon(TSLexer *lexer, bool in_expression, bool *saw_comment) {
    lexer->result_symbol = AUTOMATIC_SEMICOLON;
    lexer->mark_end(lexer);

    if (lexer->eof(lexer) || lexer->lookahead == '}' || lexer->is_at_included_range_start(lexer)) return true;
    bool saw_line_break = false;
    while (!saw_line_break) {
        while (iswspace(lexer->lookahead) && !is_line_terminator(lexer->lookahead)) skip(lexer);
        if (lexer->lookahead != '/') break;
        skip(lexer);
        if (lexer->lookahead == '*') {
            skip(lexer);
            *saw_comment = true;
            if (!skip_block_comment(lexer, &saw_line_break)) return true;
            continue;
        }
        if (lexer->lookahead != '/') return false;

        *saw_comment = true;
        const enum LookPast stop = look_past_line_comment(lexer);
        if (stop == AT_SLASH) return false;
        if (stop == AT_EOF_IN_COMMENT || lexer->lookahead == '}') return true;
        if (stop == AFTER_BLOCK_COMMENT) continue;
        if (in_expression) return false;
        if (lexer->eof(lexer)) return true;
        saw_line_break = true;
    }
    if (!skip_whitespace_and_comments(lexer, &saw_line_break, saw_comment)) return false;
    if (lexer->eof(lexer) || lexer->lookahead == '}') return true;
    if (!saw_line_break) return false;

    switch (lexer->lookahead) {
    case '`':
    case ',':
    case ':':
    case ';':
    case '*':
    case '%':
    case '>':
    case '<':
    case '=':
    case '[':
    case '(':
    case '?':
    case '^':
    case '|':
    case '&':
    case '/':
        return false;

    case '.':
        skip(lexer);
        return iswdigit(lexer->lookahead);

    case '+':
        skip(lexer);
        return lexer->lookahead == '+';

    case '-':
        skip(lexer);
        return lexer->lookahead == '-';

    case '!':
        skip(lexer);
        return lexer->lookahead != '=';

    case 'i': {
        // `in` and `instanceof` continue the expression; any other name
        // starting with `i` (or `in...`) starts a statement.
        skip(lexer);
        if (lexer->lookahead != 'n') return true;
        skip(lexer);
        if (!iswalnum(lexer->lookahead) && lexer->lookahead != '_' && lexer->lookahead != '$') return false;
        const char *rest = "stanceof";
        for (const char *p = rest; *p; p++) {
            if (lexer->lookahead != *p) return true;
            skip(lexer);
        }
        return iswalnum(lexer->lookahead) || lexer->lookahead == '_' || lexer->lookahead == '$';
    }

    default:
        return true;
    }
}

// A `?` that is a conditional's: not `??`, and not `?.` -- unless a digit
// follows, which makes `a?.5:b` a conditional (§12.8 OptionalChainingPunctuator).
static bool scan_ternary_qmark(TSLexer *lexer) {
    while (iswspace(lexer->lookahead)) skip(lexer);
    if (lexer->lookahead != '?') return false;
    advance(lexer);
    if (lexer->lookahead == '?') return false;
    lexer->mark_end(lexer);
    lexer->result_symbol = TERNARY_QMARK;
    if (lexer->lookahead == '.') {
        advance(lexer);
        return iswdigit(lexer->lookahead);
    }
    return true;
}

// Annex B.1.1: `<!--` starts a line comment, and so does `-->` at the start
// of a line. Asked only where a statement may start, where neither can be
// an operator.
static bool scan_html_comment(TSLexer *lexer) {
    while (iswspace(lexer->lookahead) || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029) skip(lexer);

    const char *opener;
    if (lexer->lookahead == '<') opener = "<!--";
    else if (lexer->lookahead == '-') opener = "-->";
    else return false;
    for (const char *p = opener; *p; p++) {
        if (lexer->lookahead != *p) return false;
        advance(lexer);
    }
    while (!lexer->eof(lexer) && !is_line_terminator(lexer->lookahead)) advance(lexer);

    lexer->result_symbol = HTML_COMMENT;
    lexer->mark_end(lexer);
    return true;
}

// JSX text: everything up to the next tag, `{`, `}` or character
// reference. Whitespace that only indents the next line is not text: a run
// is a token unless every character in it is whitespace after a line break.
static bool scan_jsx_text(TSLexer *lexer) {
    bool saw_text = false;
    bool after_newline = false;
    while (!lexer->eof(lexer) && lexer->lookahead != '<' && lexer->lookahead != '>' && lexer->lookahead != '{' &&
           lexer->lookahead != '}' && lexer->lookahead != '&') {
        if (lexer->lookahead == '\n') {
            after_newline = true;
        } else {
            after_newline = after_newline && iswspace(lexer->lookahead);
            if (!after_newline) saw_text = true;
        }
        advance(lexer);
    }
    lexer->result_symbol = JSX_TEXT;
    lexer->mark_end(lexer);
    return saw_text;
}

bool tree_sitter_javascript_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    (void)payload;

    if (valid_symbols[TEMPLATE_CHARS]) {
        // Both at once only during error recovery: produce nothing then.
        if (valid_symbols[AUTOMATIC_SEMICOLON]) return false;
        return scan_template_chars(lexer);
    }

    if (valid_symbols[JSX_TEXT]) return scan_jsx_text(lexer);

    if (valid_symbols[AUTOMATIC_SEMICOLON]) {
        bool saw_comment = false;
        if (scan_automatic_semicolon(lexer, valid_symbols[LOGICAL_OR], &saw_comment)) return true;
        // No `;` here, and nothing skipped that the tree needs: the `?` of
        // `a\n? b : c` may still be a conditional's.
        if (!saw_comment && valid_symbols[TERNARY_QMARK] && lexer->lookahead == '?') return scan_ternary_qmark(lexer);
        return false;
    }

    if (valid_symbols[TERNARY_QMARK]) return scan_ternary_qmark(lexer);

    if (valid_symbols[HTML_COMMENT] && !valid_symbols[LOGICAL_OR] && !valid_symbols[ESCAPE_SEQUENCE] &&
        !valid_symbols[REGEX_PATTERN]) {
        return scan_html_comment(lexer);
    }

    return false;
}
