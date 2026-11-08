#include "tree_sitter/parser.h"

/// Block comments and immediate parentheses are easy to parse, but strings
/// require extra-attention.
///
/// The main problems that arise when parsing strings are:
/// 1. Triple quoted strings allow single quotes inside. e.g. """ "foo" """.
/// 2. Strings can have arbitrary interpolations, including other strings.
///    e.g. "echo $("foo")"
/// 3. Non-standard string literals don't allow interpolations or escape
///    sequences, but you can always write \" and \`.
/// All of the above also applies to command literals.
enum TokenType {
    BLOCK_COMMENT_REST,
    IMMEDIATE_PAREN,
    IMMEDIATE_BRACKET,
    IMMEDIATE_BRACE,
    IMMEDIATE_STRING_START,
    IMMEDIATE_COMMAND_START,
    CONTENT_CMD_1,
    CONTENT_CMD_1_RAW,
    CONTENT_CMD_3,
    CONTENT_CMD_3_RAW,
    CONTENT_STR_1,
    CONTENT_STR_1_RAW,
    CONTENT_STR_3,
    CONTENT_STR_3_RAW,
    END_CMD,
    END_STR,
    ADJOINT_QUOTE,
    JUXTAPOSE,
    SPACED_COLON,
    TERNARY_COLON,
    BEGIN_INDEX,
};

void *tree_sitter_julia_external_scanner_create(void) {
    return NULL;
}

void tree_sitter_julia_external_scanner_destroy(void *payload) {}

unsigned tree_sitter_julia_external_scanner_serialize(void *payload, char *buffer) { return 0; }

void tree_sitter_julia_external_scanner_deserialize(void *payload, const char *buffer, unsigned size) {}

// Scanner functions

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static void mark_end(TSLexer *lexer) { lexer->mark_end(lexer); }

static bool scan_content(TSLexer *lexer, TSSymbol content_symbol, char end_char, unsigned n_delim, bool interp) {
    TSSymbol end_symbol = (end_char == '"') ? END_STR : END_CMD;
    bool has_content = false;
    int32_t next;
    while ((next = lexer->lookahead)) {
        mark_end(lexer);
        if (interp && (next == '$' || next == '\\')) {
            lexer->result_symbol = content_symbol;
            return has_content;
        } else if (next == '\\') {
            // Parse backslash in raw strings (check escaped delimiters and '\\')
            advance(lexer);
            next = lexer->lookahead;
            if (next == end_char || next == '\\') {
                lexer->result_symbol = content_symbol;
                return has_content;
            }
        } else {
            bool is_end_delimiter = true;
            for (unsigned i = 1; i <= n_delim; i++) {
                if (lexer->lookahead == end_char) {
                    advance(lexer);
                } else {
                    is_end_delimiter = false;
                    break;
                }
            }
            if (is_end_delimiter) {
                if (has_content) {
                    lexer->result_symbol = content_symbol;
                    return true;
                } else {
                    mark_end(lexer);
                    lexer->result_symbol = end_symbol;
                    return true;
                }
            }
        }
        advance(lexer);
        has_content = true;
    }
    return false;
}

static bool scan_block_comment(TSLexer *lexer) {
    // NOTE: The first `#=` is scanned by tree-sitter
    bool after_eq = false;
    unsigned nesting_depth = 1;
    for (;;) {
        switch (lexer->lookahead) {
            case '=':
                advance(lexer);
                after_eq = true;
                break;
            case '#':
                advance(lexer);
                if (after_eq) {
                    after_eq = false;
                    nesting_depth--;
                    if (nesting_depth == 0) {
                        lexer->result_symbol = BLOCK_COMMENT_REST;
                        return true;
                    }
                } else {
                    after_eq = false;
                    if (lexer->lookahead == '=') {
                        advance(lexer);
                        nesting_depth++;
                    }
                }
                break;
            case '\0':
                return false;
            default:
                advance(lexer);
                after_eq = false;
                break;
        }
    }
}

// mep: an approximation of Julia's identifier characters, enough to tell a
// name from an operator or punctuation after a closing bracket.
static bool is_ident_start(int32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') return true;
    if (c < 0x80) return false;
    if (c == 0xA0 || c == 0xA6 || c == 0xAC || c == 0xB1 || c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x206F) return false;  // general punctuation, spaces
    if (c >= 0x2190 && c <= 0x221D) return false;  // arrows, operators (∞ 221E is a name)
    if (c >= 0x2223 && c <= 0x23FF) return false;  // more operators, technical
    if (c >= 0x27C0 && c <= 0x2BFF) return false;  // arrows, math operators
    if (c >= 0x3000 && c <= 0x303F) return false;  // CJK punctuation
    return true;
}

static bool is_ident_char(int32_t c) {
    return is_ident_start(c) || (c >= '0' && c <= '9') || c == '!' ||
           (c >= 0x2070 && c <= 0x209C) || (c >= 0x2032 && c <= 0x2037);
}

// A name right after `)`/`]` multiplies (`(a+b)x`) unless it is a keyword.
static bool scan_juxtapose(TSLexer *lexer) {
    static const char *const keywords[] = {
        "end", "in", "isa", "where", "for", "if", "do", "else", "elseif", "catch",
        "finally", "try", "while", "let", "begin", "function", "return", "outer", "as",
    };
    if (!is_ident_start(lexer->lookahead)) return false;
    mark_end(lexer);  // zero width
    char word[12];
    unsigned n = 0;
    bool ascii = true;
    while (is_ident_char(lexer->lookahead)) {
        if (lexer->lookahead >= 0x80) ascii = false;
        if (n < sizeof word - 1) word[n] = (char)lexer->lookahead;
        n++;
        advance(lexer);
    }
    if (ascii && n < sizeof word) {
        word[n] = 0;
        for (unsigned i = 0; i < sizeof keywords / sizeof *keywords; i++) {
            const char *k = keywords[i];
            unsigned j = 0;
            while (k[j] && k[j] == word[j]) j++;
            if (!k[j] && !word[j]) return false;
        }
    }
    lexer->result_symbol = JUXTAPOSE;
    return true;
}

// mep: `begin` as a name, which it is inside an index (`a[begin+1:end]`,
// `x[begin]`): the word followed, on the same line, by something that can
// only continue an expression (an operator, a closing bracket, a comma).
// Otherwise it is the keyword and opens a block (`@testset begin ...`).
static bool scan_begin_index(TSLexer *lexer) {
    static const char word[] = "begin";
    for (int i = 0; word[i]; i++) {
        if (lexer->lookahead != word[i]) return false;
        advance(lexer);
    }
    int32_t c = lexer->lookahead;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '!' || c > 127)
        return false;
    mark_end(lexer);
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') advance(lexer);
    c = lexer->lookahead;
    switch (c) {
    case ']': case ')': case ',': case '+': case '-': case '*': case '/': case ':':
    case '^': case '%': case '<': case '>': case '=': case '&': case '|': case '.': case '\\':
        lexer->result_symbol = BEGIN_INDEX;
        return true;
    default:
        return false;
    }
}

bool tree_sitter_julia_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    // mep: string content and a ternary's colon are never valid together, so
    // both at once means tree-sitter is recovering from an error (every
    // symbol valid); the newer tokens stay out of that.
    bool recovering = valid_symbols[CONTENT_STR_1] && valid_symbols[TERNARY_COLON];
    if (!recovering && valid_symbols[JUXTAPOSE] && scan_juxtapose(lexer)) {
        return true;
    }
    bool colon_ok = valid_symbols[TERNARY_COLON] || valid_symbols[SPACED_COLON];
    if (!recovering && valid_symbols[BEGIN_INDEX]) {
        bool skipped = false;
        if (!colon_ok)
            while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\n' || lexer->lookahead == '\r') {
                lexer->advance(lexer, true);
                skipped = true;
            }
        // Blanks consumed: nothing below may match after them (the immediate
        // tokens need no blank), so a miss lets the internal lexer start over.
        if (lexer->lookahead == 'b') return scan_begin_index(lexer);
        if (skipped) return false;
    }
    int32_t c = lexer->lookahead;
    if (!recovering && colon_ok && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':')) {
        bool blank_before = false, newline_before = false;
        for (;;) {
            c = lexer->lookahead;
            if (c == ' ' || c == '\t') {
                blank_before = true;
            } else if (c == '\n' || c == '\r') {
                // A newline ends a statement unless a ternary is pending.
                if (!valid_symbols[TERNARY_COLON]) return false;
                newline_before = true;
            } else {
                break;
            }
            lexer->advance(lexer, true);
        }
        if (lexer->lookahead == ':') {
            advance(lexer);
            if (lexer->lookahead == ':' || lexer->lookahead == '=') return false;
            if (valid_symbols[TERNARY_COLON]) {
                mark_end(lexer);
                lexer->result_symbol = TERNARY_COLON;
                return true;
            }
            if (blank_before && !newline_before && (lexer->lookahead == ' ' || lexer->lookahead == '\t')) {
                mark_end(lexer);
                lexer->result_symbol = SPACED_COLON;
                return true;
            }
            return false;
        }
        // Blanks were consumed: let the internal lexer start over.
        return false;
    }

    if (valid_symbols[IMMEDIATE_PAREN] && lexer->lookahead == '(') {
        lexer->result_symbol = IMMEDIATE_PAREN;
        return true;
    } else if (valid_symbols[IMMEDIATE_BRACKET] && lexer->lookahead == '[') {
        lexer->result_symbol = IMMEDIATE_BRACKET;
        return true;
    } else if (valid_symbols[IMMEDIATE_BRACE] && lexer->lookahead == '{') {
        lexer->result_symbol = IMMEDIATE_BRACE;
        return true;
    } else if (valid_symbols[IMMEDIATE_STRING_START] && lexer->lookahead == '"') {
        lexer->result_symbol = IMMEDIATE_STRING_START;
        return true;
    } else if (valid_symbols[IMMEDIATE_COMMAND_START] && lexer->lookahead == '`') {
        lexer->result_symbol = IMMEDIATE_COMMAND_START;
        return true;
    } else if (valid_symbols[ADJOINT_QUOTE] && lexer->lookahead == '\'') {
        // mep: a quote right after an operand is the adjoint operator, as in
        // Julia's tokenizer -- never the start of a character literal.
        advance(lexer);
        lexer->result_symbol = ADJOINT_QUOTE;
        return true;
    }

    if (valid_symbols[BLOCK_COMMENT_REST] && scan_block_comment(lexer)) {
        return true;
    }

    if (valid_symbols[CONTENT_STR_1] && scan_content(lexer, CONTENT_STR_1, '"', 1, true)) {
        return true;
    }

    if (valid_symbols[CONTENT_STR_3] && scan_content(lexer, CONTENT_STR_3, '"', 3, true)) {
        return true;
    }

    if (valid_symbols[CONTENT_CMD_1] && scan_content(lexer, CONTENT_CMD_1, '`', 1, true)) {
        return true;
    }

    if (valid_symbols[CONTENT_CMD_3] && scan_content(lexer, CONTENT_CMD_3, '`', 3, true)) {
        return true;
    }

    if (valid_symbols[CONTENT_STR_1_RAW] && scan_content(lexer, CONTENT_STR_1_RAW, '"', 1, false)) {
        return true;
    }

    if (valid_symbols[CONTENT_STR_3_RAW] && scan_content(lexer, CONTENT_STR_3_RAW, '"', 3, false)) {
        return true;
    }

    if (valid_symbols[CONTENT_CMD_1_RAW] && scan_content(lexer, CONTENT_CMD_1_RAW, '`', 1, false)) {
        return true;
    }

    if (valid_symbols[CONTENT_CMD_3_RAW] && scan_content(lexer, CONTENT_CMD_3_RAW, '`', 3, false)) {
        return true;
    }

    return false;
}
