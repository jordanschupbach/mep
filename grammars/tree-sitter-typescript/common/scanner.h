// The external scanner for mep's TypeScript and TSX grammars, shared by
// both dialects (each one's src/scanner.c includes this and exports it
// under its own name). It is JavaScript's (grammars/tree-sitter-javascript/
// src/scanner.c) with what TypeScript adds:
//
//   _function_signature_automatic_semicolon
//                         the end of a signature with no body (an overload,
//                         an abstract or ambient method): a line break ends
//                         it unless a `{` follows, which opens the body.
//   _ternary_qmark        not a `?` followed by `:`, `)` or `,` -- that is an
//                         optional parameter's or property's marker.
//   _automatic_semicolon  a `}` followed by `:` is a destructured
//                         parameter's type (`({a}: T) => ...`), not the end
//                         of a statement; a line break before `(` or `[`
//                         ends a type (`type A = B` then `[x] = y`), but
//                         not an expression.
//
// Where a comment ends up -- inside the statement before an automatic
// semicolon or after it -- follows tree-sitter-typescript 0.23's scanner
// (the grammar mep loaded before), whose trees these grammars reproduce: a
// comment on the statement's own line stays inside it.
//
// `||`, escape_sequence and regex_pattern are never produced here; the
// parser marking them valid is how the scanner knows a statement ends in
// an expression, or that it is inside a string, template or regex. It keeps
// no state.

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
    FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON,
    ERROR_RECOVERY,
};

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static bool is_line_terminator(int32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

static bool is_identifier_part(int32_t c) { return iswalnum(c) || c == '_' || c == '$'; }

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

// Skips whitespace and comments after the line break. False on a `/` that
// starts neither (a division or a regex). *saw_line_comment is set if a
// `//` comment was skipped.
static bool skip_whitespace_and_comments(TSLexer *lexer, bool *saw_line_comment) {
    for (;;) {
        while (iswspace(lexer->lookahead) || lexer->lookahead == 0x2028 || lexer->lookahead == 0x2029 ||
               lexer->lookahead == 0xFEFF) {
            skip(lexer);
        }
        if (lexer->lookahead != '/') return true;
        skip(lexer);
        if (lexer->lookahead == '/') {
            while (!lexer->eof(lexer) && !is_line_terminator(lexer->lookahead)) skip(lexer);
            *saw_line_comment = true;
        } else if (lexer->lookahead == '*') {
            skip(lexer);
            for (;;) {
                if (lexer->eof(lexer)) return true;
                if (lexer->lookahead == '*') {
                    skip(lexer);
                    if (lexer->lookahead == '/') {
                        skip(lexer);
                        break;
                    }
                } else {
                    skip(lexer);
                }
            }
        } else {
            return false;
        }
    }
}

// §12.10.1's first rule, approximated by the next token's first characters:
// after a line break a `;` is inserted unless what follows can continue
// the statement. `++`/`--` start the next statement; so do `!` other than
// `!=`, and `.5`.
static bool scan_automatic_semicolon(TSLexer *lexer, const bool *valid_symbols, bool *saw_line_comment) {
    lexer->result_symbol = AUTOMATIC_SEMICOLON;
    lexer->mark_end(lexer);

    if (lexer->is_at_included_range_start(lexer)) return true;
    for (;;) {
        if (lexer->eof(lexer)) return true;
        if (lexer->lookahead == '}') {
            // `({a}: {a: number}) => a`: the `}` closes a pattern whose type
            // follows, unless this is a conditional's consequence (where
            // `||` is valid, `{...} : b`).
            do {
                skip(lexer);
            } while (iswspace(lexer->lookahead));
            if (lexer->lookahead == ':') return valid_symbols[LOGICAL_OR];
            return true;
        }
        if (is_line_terminator(lexer->lookahead) && lexer->lookahead != '\r') break;
        // Anything but whitespace before the line break (a comment, say)
        // is lexed first; the parser asks again after it.
        if (!iswspace(lexer->lookahead)) return false;
        skip(lexer);
    }
    skip(lexer);

    if (!skip_whitespace_and_comments(lexer, saw_line_comment)) return false;
    if (lexer->eof(lexer)) return true;

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
    case '?':
    case '^':
    case '|':
    case '&':
    case '/':
        return false;

    case '{':
        return !valid_symbols[FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON];

    // In an expression a `(` or `[` continues it (a call, an index); in a
    // type it can't, and the type ends. The parser says which: `||` is
    // valid only after an expression.
    case '(':
    case '[':
        return !valid_symbols[LOGICAL_OR];

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
        // starting with `i` starts a statement.
        skip(lexer);
        if (lexer->lookahead != 'n') return true;
        skip(lexer);
        if (!is_identifier_part(lexer->lookahead)) return false;
        for (const char *p = "stanceof"; *p; p++) {
            if (lexer->lookahead != *p) return true;
            skip(lexer);
        }
        return is_identifier_part(lexer->lookahead);
    }

    default:
        return true;
    }
}

// A `?` that is a conditional's: not `??` or `?.` (unless a digit follows:
// `a?.5:b`), and not an optional marker -- `x?: T`, `(x?)`, `(x?, y)`.
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
    while (iswspace(lexer->lookahead)) advance(lexer);
    return lexer->lookahead != ':' && lexer->lookahead != ')' && lexer->lookahead != ',';
}

// Annex B.1.1: `<!--` starts a line comment, and so does `-->` at the start
// of a line. Asked only where a statement may start.
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

// JSX text (.tsx): everything up to the next tag, `{`, `}` or character
// reference. Whitespace that only indents the next line is not text.
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

static bool typescript_scan(TSLexer *lexer, const bool *valid_symbols) {
    if (valid_symbols[TEMPLATE_CHARS]) {
        // Both at once only during error recovery: produce nothing then.
        if (valid_symbols[AUTOMATIC_SEMICOLON]) return false;
        return scan_template_chars(lexer);
    }

    if (valid_symbols[JSX_TEXT] && scan_jsx_text(lexer)) return true;

    if (valid_symbols[AUTOMATIC_SEMICOLON] || valid_symbols[FUNCTION_SIGNATURE_AUTOMATIC_SEMICOLON]) {
        bool saw_line_comment = false;
        if (scan_automatic_semicolon(lexer, valid_symbols, &saw_line_comment)) return true;
        // No `;` here, and no comment skipped that the tree needs: the `?`
        // of `a\n? b : c` may still be a conditional's.
        if (!saw_line_comment && valid_symbols[TERNARY_QMARK] && lexer->lookahead == '?') {
            return scan_ternary_qmark(lexer);
        }
        return false;
    }

    if (valid_symbols[TERNARY_QMARK]) return scan_ternary_qmark(lexer);

    if (valid_symbols[HTML_COMMENT] && !valid_symbols[LOGICAL_OR] && !valid_symbols[ESCAPE_SEQUENCE] &&
        !valid_symbols[REGEX_PATTERN]) {
        return scan_html_comment(lexer);
    }

    return false;
}
