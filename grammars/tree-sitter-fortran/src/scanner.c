// External scanner for mep's Fortran grammar.
//
// Started from stadelmanma/tree-sitter-fortran's scanner (MIT; see the
// README): numbers, BOZ literals, strings with `&` continuations, the
// end-of-statement token and free-form line continuations are its logic.
// mep's additions: comments are scanned here (a fixed-form comment line is
// a comment only because of its column), and fixed form's layout --
// comment lines marked in column 1, continuation lines marked in column 6.
//
// The grammar never sees a file name, so fixed form is recognised from the
// text: a file is read as free form until a line only fixed form explains
// (a `*` comment line, a `C` line that cannot be a statement, a column-6
// mark no free-form statement starts with), and as fixed form from then on
// (`fixed_form`, carried in the serialized state).

#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"
#include <ctype.h>
#include <wctype.h>

enum TokenType {
    LINE_CONTINUATION,
    INTEGER_LITERAL,
    FLOAT_LITERAL,
    BOZ_LITERAL,
    STRING_LITERAL,
    STRING_LITERAL_KIND,
    END_OF_STATEMENT,
    PREPROC_UNARY_OPERATOR,
    COMMENT,
    PREPROC_NEWLINE,
    PREPROC_NEWLINE_IMMEDIATE,
};

typedef struct {
    bool in_line_continuation;
    bool fixed_form;
} Scanner;

//  consume current character into current token and advance
static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

// ignore current character and advance
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// is `chr` ok for an identifier?
static bool is_identifier_char(int32_t chr) { return iswalnum(chr) || chr == '_'; }

static bool is_eol(TSLexer *lexer) {
    return lexer->lookahead == '\n' || lexer->lookahead == '\r' || lexer->eof(lexer);
}

static bool is_boz_sentinel(int32_t chr) {
    switch (chr) {
        case 'B':
        case 'b':
        case 'O':
        case 'o':
        case 'Z':
        case 'z':
            return true;
        default:
            return false;
    }
}

static bool is_exp_sentinel(int32_t chr) {
    switch (chr) {
        case 'D':
        case 'd':
        case 'E':
        case 'e':
        case 'Q':
        case 'q':
            return true;
        default:
            return false;
    }
}

static void consume_to_eol(TSLexer *lexer) {
    while (!is_eol(lexer)) {
        advance(lexer);
    }
}

// ---- fixed form ------------------------------------------------------------

// Can `c` be a fixed-form continuation mark before the file is known to be
// fixed form? Only a character no free-form statement starts with.
static bool unambiguous_mark(int32_t c) {
    return !(iswalnum(c) || c == '_' || c == '!' || c == '#' || c == '&' || c == '\'' || c == '"' || c == ' ' ||
             c == '\t' || c == '\n' || c == '\r' || c == 0);
}

// Is the line at the lexer (column 1) a fixed-form comment line? Consumes
// at most its first characters when it is not (the caller only looks, or
// returns false so the lexer rewinds). `*unambiguous` is set when only
// fixed form explains the line.
//
// `*` always is (no free-form statement starts with one). `C`/`c`/`D`/`d`
// are once the file is fixed form; before that, a `C` is a comment only
// when what follows cannot continue a statement (`c = 1`, `call x` and
// `c(i) = 2` are statements).
static bool fixed_comment_line(Scanner *scanner, TSLexer *lexer, bool *unambiguous) {
    int32_t c = lexer->lookahead;
    *unambiguous = false;
    if (c == '*') {
        *unambiguous = true;
        return true;
    }
    if (scanner->fixed_form) {
        return c == 'c' || c == 'C' || c == 'd' || c == 'D';
    }
    if (c != 'c' && c != 'C') {
        return false;
    }
    advance(lexer);
    c = lexer->lookahead;
    if (is_eol(lexer)) {
        *unambiguous = true;
        return true;
    }
    if (is_identifier_char(c) || c == '$') {
        return false;
    }
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
        advance(lexer);
    }
    c = lexer->lookahead;
    if (c == '=' || c == '%' || c == '(' || c == '[' || c == '&' || c == ';' || c == '!' || is_eol(lexer)) {
        return false;
    }
    *unambiguous = true;
    return true;
}

// With the lexer at column 1: skip the line's leading blanks and say
// whether it is a fixed-form continuation line (columns 1-5 blank and a
// mark in column 6 that is neither blank nor `0`, or a tab and a digit
// 1-9). On true the lexer is just before the mark. Blanks are consumed
// with `skip` so a line that turns out to be ordinary loses nothing.
static bool at_continuation_mark(Scanner *scanner, TSLexer *lexer, bool scanning) {
    int col = 0;
    bool tab = false;
    while (lexer->lookahead == ' ' && col < 5) {
        if (scanning) {
            skip(lexer);
        } else {
            advance(lexer);
        }
        col++;
    }
    if (col == 0 && lexer->lookahead == '\t') {
        if (scanning) {
            skip(lexer);
        } else {
            advance(lexer);
        }
        tab = true;
    }
    int32_t c = lexer->lookahead;
    if (tab) {
        return scanner->fixed_form && c >= '1' && c <= '9';
    }
    if (col != 5 || c == ' ' || c == '\t' || c == '0' || is_eol(lexer)) {
        return false;
    }
    return scanner->fixed_form ? true : unambiguous_mark(c);
}

// Looking past the end of a line (the lexer at the start of the next):
// does the statement go on, fixed-form style, on a continuation line after
// any comment and blank lines? Only looks.
static bool next_line_continues(Scanner *scanner, TSLexer *lexer) {
    for (int guard = 0; guard < 100000; guard++) {
        if (lexer->eof(lexer)) {
            return false;
        }
        bool unambiguous;
        if (fixed_comment_line(scanner, lexer, &unambiguous)) {
            if (!scanner->fixed_form && !unambiguous) {
                return false;
            }
            consume_to_eol(lexer);
        } else if (lexer->get_column(lexer) != 0) {
            return false;  // a `c` statement
        } else {
            if (at_continuation_mark(scanner, lexer, false)) {
                return true;
            }
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
                advance(lexer);
            }
            if (lexer->lookahead == '!' && scanner->fixed_form) {
                consume_to_eol(lexer);
            } else if (!is_eol(lexer)) {
                return false;
            }
        }
        if (lexer->eof(lexer)) {
            return false;
        }
        if (lexer->lookahead == '\r') {
            advance(lexer);
        }
        if (lexer->lookahead == '\n') {
            advance(lexer);
        }
    }
    return false;
}

// ---- literals --------------------------------------------------------------

static bool scan_int(TSLexer *lexer) {
    if (!iswdigit(lexer->lookahead)) {
        return false;
    }
    // consume digits
    while (iswdigit(lexer->lookahead)) {
        advance(lexer);  // store all digits
    }

    // handle line continuations
    if (lexer->lookahead == '&') {
        skip(lexer);
        while (iswspace(lexer->lookahead)) {
            skip(lexer);
        }
        // second '&' required to continue the literal
        if (lexer->lookahead == '&') {
            skip(lexer);
            // don't return here, as we may have finished literal on first
            // line but still have second '&'
            scan_int(lexer);
        }
    }

    lexer->mark_end(lexer);
    return true;
}

/// Scan a number of the forms 1XXX, 1.0XXX, 0.1XXX, 1.XDX, etc.
static bool scan_number(TSLexer *lexer) {
    lexer->result_symbol = INTEGER_LITERAL;
    bool digits = scan_int(lexer);
    if (lexer->lookahead == '.') {
        advance(lexer);
        // exclude decimal if followed by any letter other than d/D and e/E
        // if no leading digits are present and a non-digit follows
        // the decimal it's a nonmatch.
        if (digits && !iswalnum(lexer->lookahead)) {
            lexer->mark_end(lexer);  // add decimal to token
        }
        lexer->result_symbol = FLOAT_LITERAL;
    }
    // if next char isn't number return since we handle exp
    // notation and precision identifiers separately. If there are
    // no leading digit it's a nonmatch.
    digits = scan_int(lexer) || digits;
    if (digits) {
        // process exp notation
        if (is_exp_sentinel(lexer->lookahead)) {
            advance(lexer);
            if (lexer->lookahead == '+' || lexer->lookahead == '-') {
                advance(lexer);
            }
            if (!scan_int(lexer)) {
                return true;  // valid number token with junk after it
            }
            lexer->mark_end(lexer);
            lexer->result_symbol = FLOAT_LITERAL;
        }
    }
    return digits;
}

static bool scan_boz(TSLexer *lexer) {
    lexer->result_symbol = BOZ_LITERAL;
    bool boz_prefix = false;
    int32_t quote = '\0';
    if (is_boz_sentinel(lexer->lookahead)) {
        advance(lexer);
        boz_prefix = true;
    }
    if (lexer->lookahead == '\'' || lexer->lookahead == '"') {
        quote = lexer->lookahead;
        advance(lexer);
        if (!iswxdigit(lexer->lookahead)) {
            return false;
        }
        while (iswxdigit(lexer->lookahead)) {
            advance(lexer);  // store all hex digits
        }
        if (lexer->lookahead != quote) {
            return false;
        }
        advance(lexer);  // store enclosing quote
        if (!boz_prefix && !is_boz_sentinel(lexer->lookahead)) {
            return false;  // no boz suffix or prefix provided
        }
        lexer->mark_end(lexer);
        return true;
    }
    return false;
}

// Things that end statements in Fortran: semicolons (a grammar token),
// end-of-line, comments (they are easier to handle with line continuations
// if the statement ends before them) and EOF.
//
// Fixed form: a newline is not the end when a continuation line follows
// (past comment and blank lines). The statement then goes on: a zero-width
// line-continuation token after the newline stands for the line break, and
// the comment lines and the continuation mark are scanned from there.
static bool scan_end_of_statement(Scanner *scanner, TSLexer *lexer) {
    if (lexer->eof(lexer)) {
        skip(lexer);
        lexer->result_symbol = END_OF_STATEMENT;
        return true;
    }

    // If we're in a line continuation, then don't end the statement
    if (scanner->in_line_continuation) {
        return false;
    }

    if (lexer->lookahead == '!') {
        // In fixed form the statement may go on after the comment: decide
        // at its newline.
        if (scanner->fixed_form) {
            return false;
        }
        lexer->result_symbol = END_OF_STATEMENT;
        return true;
    }

    // Consume end of line characters, we allow '\n', '\r\n' and
    // '\r' to cover unix, MSDOS and old style Macintosh.
    if (lexer->lookahead == '\r') {
        skip(lexer);
        if (lexer->lookahead == '\n') {
            skip(lexer);
        }
    } else if (lexer->lookahead == '\n') {
        skip(lexer);
    } else {
        return false;
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = next_line_continues(scanner, lexer) ? LINE_CONTINUATION : END_OF_STATEMENT;
    return true;
}

static bool scan_start_line_continuation(Scanner *scanner, TSLexer *lexer) {
    // Now see if we should start a line continuation
    scanner->in_line_continuation = (lexer->lookahead == '&');
    if (!scanner->in_line_continuation) {
        return false;
    }
    // Consume the '&'
    advance(lexer);
    lexer->result_symbol = LINE_CONTINUATION;
    return true;
}

static bool scan_end_line_continuation(Scanner *scanner, TSLexer *lexer) {
    if (!scanner->in_line_continuation) {
        return false;
    }
    // Everything except comments ends a line continuation
    if (lexer->lookahead == '!') {
        return false;
    }

    scanner->in_line_continuation = false;

    // Consume any leading line continuation markers
    if (lexer->lookahead == '&') {
        advance(lexer);
    }
    lexer->result_symbol = LINE_CONTINUATION;
    return true;
}

static bool scan_string_literal_kind(TSLexer *lexer) {
    // Strictly, it's allowed for the kind to be an integer literal, in
    // practice I've not seen it
    if (!iswalpha(lexer->lookahead)) {
        return false;
    }

    lexer->result_symbol = STRING_LITERAL_KIND;

    // We need two characters of lookahead to see `_"`
    int32_t current_char = '\0';

    while (is_identifier_char(lexer->lookahead) && !lexer->eof(lexer)) {
        current_char = lexer->lookahead;
        // Don't capture the trailing underscore as part of the kind identifier
        if (lexer->lookahead == '_') {
            lexer->mark_end(lexer);
        }
        advance(lexer);
    }

    if ((current_char != '_') || (lexer->lookahead != '"' && lexer->lookahead != '\'')) {
        return false;
    }

    return true;
}

static bool scan_string_literal(Scanner *scanner, TSLexer *lexer) {
    const int32_t opening_quote = lexer->lookahead;

    if (opening_quote != '"' && opening_quote != '\'') {
        return false;
    }

    advance(lexer);
    lexer->result_symbol = STRING_LITERAL;

    for (;;) {
        if (lexer->eof(lexer)) {
            return false;
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            // Fixed form: a literal goes on on a continuation line (it may
            // be blank-padded to column 72 before the break).
            if (!scanner->fixed_form) {
                return false;
            }
            if (lexer->lookahead == '\r') {
                advance(lexer);
            }
            if (lexer->lookahead != '\n') {
                return false;
            }
            advance(lexer);
            if (!at_continuation_mark(scanner, lexer, false)) {
                return false;
            }
            advance(lexer);  // the mark
            continue;
        }
        // Handle line continuations: strictly speaking, we MUST have
        // both trailing '&' on first line AND leading '&' on second
        // line, though most compilers do accept string literals
        // missing the second '&'. The literal token contains the line
        // continuation as well as any blank or comment lines inside
        // the quotes.
        if (lexer->lookahead == '&' && !scanner->fixed_form) {
            advance(lexer);
            // Consume blanks up to the end of the line or non-blank
            while (iswblank(lexer->lookahead)) {
                advance(lexer);
            }
            // If we hit the end of the line, consume all whitespace,
            // including new lines
            if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                while (iswspace(lexer->lookahead)) {
                    advance(lexer);
                }
            }
        }

        // If we hit the same kind of quote that opened this literal,
        // check to see if there's two in a row, and if so, consume
        // both of them
        if (lexer->lookahead == opening_quote) {
            advance(lexer);
            // It was just one quote, so we've successfully reached the
            // end of the literal
            if (lexer->lookahead != opening_quote) {
                lexer->mark_end(lexer);
                // Free form: `'abc'&` + newline + `&'def'` joins the two
                // lines inside one literal (a doubled quote: abc'def).
                if (lexer->lookahead != '&' || scanner->fixed_form) {
                    return true;
                }
                advance(lexer);
                while (iswblank(lexer->lookahead)) {
                    advance(lexer);
                }
                if (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
                    return true;
                }
                while (iswspace(lexer->lookahead)) {
                    advance(lexer);
                }
                if (lexer->lookahead != '&') {
                    return true;
                }
                advance(lexer);
                if (lexer->lookahead != opening_quote) {
                    return true;
                }
            }
        }
        if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
            continue;
        }
        advance(lexer);
    }
}

/// Need an external scanner to catch '!' before its parsed as a comment
static bool scan_preproc_unary_operator(TSLexer *lexer) {
    const int32_t next_char = lexer->lookahead;
    if (next_char == '!' || next_char == '~' || next_char == '-' || next_char == '+') {
        advance(lexer);
        lexer->result_symbol = PREPROC_UNARY_OPERATOR;
        return true;
    }
    return false;
}

static bool scan_comment(TSLexer *lexer) {
    // Up to the newline, a carriage return included (as the old grammar's
    // `!.*` token had it).
    while (lexer->lookahead != '\n' && !lexer->eof(lexer)) {
        advance(lexer);
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = COMMENT;
    return true;
}

static bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
    bool line_start = lexer->get_column(lexer) == 0;

    // Fixed form's line starts come first: a comment line, or (with the
    // statement going on) a continuation line's mark.
    if (line_start && !scanner->in_line_continuation) {
        bool unambiguous = false;
        if (valid_symbols[COMMENT] && fixed_comment_line(scanner, lexer, &unambiguous)) {
            if (unambiguous) {
                scanner->fixed_form = true;
            }
            return scan_comment(lexer);
        }
        if (lexer->get_column(lexer) != 0) {
            return false;  // a statement starting with `c`: rewind
        }
        if (at_continuation_mark(scanner, lexer, true)) {
            int32_t mark = lexer->lookahead;
            if (!scanner->fixed_form && unambiguous_mark(mark)) {
                scanner->fixed_form = true;
            }
            advance(lexer);
            lexer->mark_end(lexer);
            lexer->result_symbol = LINE_CONTINUATION;
            return true;
        }
    }

    // Consume any leading whitespace except newlines
    while (iswblank(lexer->lookahead)) {
        skip(lexer);
    }

    // Close the current statement if we can
    if (valid_symbols[END_OF_STATEMENT]) {
        if (scan_end_of_statement(scanner, lexer)) {
            return true;
        }
    }

    // The newline ending a preprocessor line, where the grammar wants one.
    if (valid_symbols[PREPROC_NEWLINE_IMMEDIATE] && !valid_symbols[PREPROC_NEWLINE] &&
        (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
        if (lexer->lookahead == '\r') {
            advance(lexer);
        }
        if (lexer->lookahead == '\n') {
            advance(lexer);
        }
        lexer->mark_end(lexer);
        lexer->result_symbol = PREPROC_NEWLINE_IMMEDIATE;
        return true;
    }
    if (valid_symbols[PREPROC_NEWLINE] && (lexer->lookahead == '\n' || lexer->lookahead == '\r')) {
        // Like the regex token it replaces, it ends at the last newline
        // of a run of blank lines.
        while (iswspace(lexer->lookahead)) {
            bool nl = lexer->lookahead == '\n';
            advance(lexer);
            if (nl) {
                lexer->mark_end(lexer);
            }
        }
        lexer->result_symbol = PREPROC_NEWLINE;
        return true;
    }

    // We're now either in a line continuation or between statements, so
    // eat all whitespace including newlines until something more
    // interesting -- checking each new line for fixed form's marks.
    while (iswspace(lexer->lookahead)) {
        bool newline = lexer->lookahead == '\n';
        skip(lexer);
        if (newline && !scanner->in_line_continuation) {
            bool unambiguous = false;
            if (valid_symbols[COMMENT] && fixed_comment_line(scanner, lexer, &unambiguous)) {
                if (unambiguous) {
                    scanner->fixed_form = true;
                }
                return scan_comment(lexer);
            }
            if (lexer->get_column(lexer) != 0) {
                return false;  // a statement starting with `c`: rewind
            }
            if (at_continuation_mark(scanner, lexer, true)) {
                int32_t mark = lexer->lookahead;
                if (!scanner->fixed_form && unambiguous_mark(mark)) {
                    scanner->fixed_form = true;
                }
                advance(lexer);
                lexer->mark_end(lexer);
                lexer->result_symbol = LINE_CONTINUATION;
                return true;
            }
        }
    }

    if (scan_end_line_continuation(scanner, lexer)) {
        return true;
    }

    if (valid_symbols[STRING_LITERAL]) {
        if (scan_string_literal(scanner, lexer)) {
            return true;
        }
    }

    if (valid_symbols[INTEGER_LITERAL] || valid_symbols[FLOAT_LITERAL] || valid_symbols[BOZ_LITERAL]) {
        // extract out root number from expression
        if (scan_number(lexer)) {
            return true;
        }
        if (scan_boz(lexer)) {
            return true;
        }
    }

    if (valid_symbols[PREPROC_UNARY_OPERATOR]) {
        if (scan_preproc_unary_operator(lexer)) {
            return true;
        }
    }

    if (lexer->lookahead == '!' && valid_symbols[COMMENT]) {
        return scan_comment(lexer);
    }

    if (scan_start_line_continuation(scanner, lexer)) {
        return true;
    }

    if (valid_symbols[STRING_LITERAL_KIND]) {
        // This may need a lot of lookahead, so should (probably) always
        // be the last token to look for
        if (scan_string_literal_kind(lexer)) {
            return true;
        }
    }

    return false;
}

void *tree_sitter_fortran_external_scanner_create(void) { return ts_calloc(1, sizeof(Scanner)); }

bool tree_sitter_fortran_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;
    return scan(scanner, lexer, valid_symbols);
}

unsigned tree_sitter_fortran_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    buffer[0] = (char)scanner->in_line_continuation;
    buffer[1] = (char)scanner->fixed_form;
    return 2;
}

void tree_sitter_fortran_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->in_line_continuation = false;
    scanner->fixed_form = false;
    if (length > 0) {
        scanner->in_line_continuation = buffer[0];
    }
    if (length > 1) {
        scanner->fixed_form = buffer[1];
    }
}

void tree_sitter_fortran_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    ts_free(scanner);
}
