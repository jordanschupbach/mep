// External scanner for the Bash grammar (grammar.js).
//
// Bash's lexer is context-driven (the manual's "Shell Operation" and
// "Quoting"; parse.y's read_token): whether `=` ends a name, whether `}`
// closes a brace group or is a word, whether `<<` starts a here-document
// all depend on what the parser expects next. The tokens below are the
// ones a context-free lexer can't decide, and each is decided here from
// `valid_symbols`, which is how the parser says what it expects.
//
// State: the stack of here-documents whose bodies are still to come (a
// line may open several: `cat <<A <<B`), each with its delimiter, whether
// it is quoted (no expansions in the body) and whether `<<-` strips
// leading tabs; plus the paren depth an extglob pattern was cut at by an
// expansion inside it. Both are serialized.
//
// The order of the checks in scan() is load-bearing: several tokens can be
// valid at once and the first family that recognizes the input wins. It is
// the order the node API was established with (tree-sitter-bash 0.25),
// kept so that trees are identical; see README.md.

#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    HEREDOC_START,
    SIMPLE_HEREDOC_BODY,
    HEREDOC_BODY_BEGINNING,
    HEREDOC_CONTENT,
    HEREDOC_END,
    FILE_DESCRIPTOR,
    EMPTY_VALUE,
    CONCAT,
    VARIABLE_NAME,
    TEST_OPERATOR,
    REGEX,
    REGEX_NO_SLASH,
    REGEX_NO_SPACE,
    EXPANSION_WORD,
    EXTGLOB_PATTERN,
    BARE_DOLLAR,
    BRACE_START,
    IMMEDIATE_DOUBLE_HASH,
    EXPANSION_SYM_HASH,
    EXPANSION_SYM_BANG,
    EXPANSION_SYM_EQUAL,
    CLOSING_BRACE,
    CLOSING_BRACKET,
    HEREDOC_ARROW,
    HEREDOC_ARROW_DASH,
    NEWLINE,
    OPENING_PAREN,
    ESAC,
    ERROR_RECOVERY,
};

typedef Array(char) String;

typedef struct {
    bool quoted;         // `<<'EOF'`, `<<"EOF"`, `<<\EOF`: the body is literal
    bool started;        // the body's first token has been returned
    bool strip_tabs;     // `<<-`
    String delimiter;    // unquoted, no terminator
} Heredoc;

typedef struct {
    uint8_t glob_paren_depth;
    Array(Heredoc) heredocs;
} Scanner;

// What a token family's scan concluded: it read a token, it is sure there
// is none here, or the input isn't its business and the next family in
// scan()'s order should look.
typedef enum { NO_TOKEN, TOKEN, PASS } Outcome;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static inline bool at(TSLexer *lexer, int32_t c) { return lexer->lookahead == c; }

static inline bool is_space(TSLexer *lexer) { return iswspace(lexer->lookahead); }

static inline Outcome produce(TSLexer *lexer, enum TokenType type, bool ok) {
    lexer->result_symbol = type;
    return ok ? TOKEN : NO_TOKEN;
}

// ---- here-documents --------------------------------------------------------
// Bash: "Here Documents". The delimiter is the word after `<<`, quote
// removal applied; any quoting at all makes the body literal.

static void heredoc_clear(Heredoc *heredoc) {
    heredoc->quoted = false;
    heredoc->started = false;
    heredoc->strip_tabs = false;
    array_clear(&heredoc->delimiter);
}

static void heredoc_push(Scanner *scanner, bool strip_tabs) {
    Heredoc heredoc = {.quoted = false, .started = false, .strip_tabs = strip_tabs, .delimiter = array_new()};
    array_push(&scanner->heredocs, heredoc);
}

static void heredoc_pop(Scanner *scanner) {
    Heredoc *heredoc = array_back(&scanner->heredocs);
    array_delete(&heredoc->delimiter);
    array_pop(&scanner->heredocs);
}

// The delimiter word: one quoted run or up to whitespace, a backslash
// taking the next character literally. False when there is none.
static bool read_delimiter(TSLexer *lexer, String *out) {
    int32_t quote = 0;
    if (at(lexer, '\'') || at(lexer, '"')) {
        quote = lexer->lookahead;
        advance(lexer);
    }
    bool empty = true;
    while (lexer->lookahead != 0 &&
           !(quote ? (at(lexer, quote) || at(lexer, '\r') || at(lexer, '\n')) : is_space(lexer))) {
        if (at(lexer, '\\')) {
            advance(lexer);
            if (lexer->lookahead == 0) return false;
        }
        empty = false;
        array_push(out, (char)lexer->lookahead);
        advance(lexer);
    }
    if (quote && at(lexer, quote)) advance(lexer);
    return !empty;
}

static Outcome scan_heredoc_start(Heredoc *heredoc, TSLexer *lexer) {
    while (is_space(lexer)) skip(lexer);
    lexer->result_symbol = HEREDOC_START;
    heredoc->quoted = at(lexer, '\'') || at(lexer, '"') || at(lexer, '\\');
    if (!read_delimiter(lexer, &heredoc->delimiter)) {
        array_clear(&heredoc->delimiter);
        return NO_TOKEN;
    }
    return TOKEN;
}

// Reads as much of the delimiter as the line starts with. True when the
// line starts with all of it.
static bool at_delimiter(Heredoc *heredoc, TSLexer *lexer) {
    uint32_t n = heredoc->delimiter.size;
    if (n == 0) return false;
    uint32_t i = 0;
    while (i < n && lexer->lookahead != 0 && !at(lexer, '\n') && lexer->lookahead == heredoc->delimiter.contents[i]) {
        advance(lexer);
        i++;
    }
    return i == n;
}

// A body is returned in pieces around its expansions: the text up to the
// first expansion (`middle`, the body's beginning or a content piece) and,
// once the delimiter line is reached, the text before it (`end`). A body
// with no expansions at all comes back whole as `end` (SIMPLE_HEREDOC_BODY).
static Outcome scan_heredoc_content(Scanner *scanner, TSLexer *lexer, enum TokenType middle, enum TokenType end) {
    Heredoc *heredoc = array_back(&scanner->heredocs);
    bool did_advance = false;
    for (;;) {
        switch (lexer->lookahead) {
            case 0:
                if (lexer->eof(lexer) && did_advance) {
                    // Unterminated: the body runs to the end of the file.
                    heredoc_clear(heredoc);
                    return produce(lexer, end, true);
                }
                return NO_TOKEN;

            case '\\':
                did_advance = true;
                advance(lexer);
                advance(lexer);
                break;

            case '$':
                if (heredoc->quoted) {
                    did_advance = true;
                    advance(lexer);
                    break;
                }
                if (did_advance) {
                    // Text before an expansion: end the piece here.
                    lexer->mark_end(lexer);
                    lexer->result_symbol = middle;
                    heredoc->started = true;
                    advance(lexer);
                    if (iswalpha(lexer->lookahead) || at(lexer, '{') || at(lexer, '(')) return TOKEN;
                    break;
                }
                if (middle == HEREDOC_BODY_BEGINNING && lexer->get_column(lexer) == 0) {
                    // The body starts with an expansion: an empty beginning.
                    heredoc->started = true;
                    return produce(lexer, middle, true);
                }
                return NO_TOKEN;

            case '\n':
                if (did_advance) {
                    advance(lexer);
                } else {
                    skip(lexer);
                }
                did_advance = true;
                if (heredoc->strip_tabs) {
                    while (is_space(lexer)) advance(lexer);
                }
                lexer->result_symbol = heredoc->started ? middle : end;
                lexer->mark_end(lexer);
                if (at_delimiter(heredoc, lexer)) {
                    if (lexer->result_symbol == HEREDOC_END) heredoc_pop(scanner);
                    return TOKEN;
                }
                break;

            default:
                if (lexer->get_column(lexer) == 0) {
                    // The first line of the body: the delimiter may already
                    // be here (an empty body), indented under `<<-`.
                    while (is_space(lexer)) {
                        if (did_advance) {
                            advance(lexer);
                        } else {
                            skip(lexer);
                        }
                    }
                    if (end != SIMPLE_HEREDOC_BODY) {
                        lexer->result_symbol = middle;
                        if (at_delimiter(heredoc, lexer)) return TOKEN;
                    } else {
                        lexer->result_symbol = end;
                        lexer->mark_end(lexer);
                        if (at_delimiter(heredoc, lexer)) return TOKEN;
                    }
                }
                did_advance = true;
                advance(lexer);
                break;
        }
    }
}

// ---- concatenation -----------------------------------------------------------
// Bash words are made of adjacent pieces (`a"b"$c`); CONCAT is the
// zero-width token between two pieces with nothing between them.

static Outcome scan_concat(TSLexer *lexer, const bool *valid) {
    int32_t c = lexer->lookahead;
    // Inside ${x:-word} the shell's operators are only text: `${2:+ (from
    // $2)}`, `${x:+$x;}`. (The external `}` is only ever the one closing a
    // ${...}, so its being valid says where we are.)
    bool in_expansion_word = valid[CLOSING_BRACE];
    bool operator_char = c == '>' || c == '<' || c == ')' || c == '(' || c == ';' || c == '&' || c == '|';
    bool ends_word = c == 0 || iswspace(c) || (operator_char && !in_expansion_word) ||
                     (c == '}' && valid[CLOSING_BRACE]) || (c == ']' && valid[CLOSING_BRACKET]);
    if (!ends_word) {
        lexer->result_symbol = CONCAT;
        if (c == '`') {
            // a`b`: a concatenation only if the backquoted part ends the word.
            lexer->mark_end(lexer);
            advance(lexer);
            while (!at(lexer, '`') && !lexer->eof(lexer)) advance(lexer);
            if (lexer->eof(lexer)) return NO_TOKEN;
            advance(lexer);
            return iswspace(lexer->lookahead) || lexer->eof(lexer) ? TOKEN : NO_TOKEN;
        }
        if (c != '\\') return TOKEN;
        // An escaped quote or backslash continues the word; any other
        // escape is left to the rest of scan(), which sees the character
        // after the backslash.
        lexer->mark_end(lexer);
        advance(lexer);
        if (at(lexer, '"') || at(lexer, '\'') || at(lexer, '\\')) return TOKEN;
        if (lexer->eof(lexer)) return NO_TOKEN;
    }
    if (iswspace(lexer->lookahead) && valid[CLOSING_BRACE] && !valid[EXPANSION_WORD]) {
        return produce(lexer, CONCAT, true);
    }
    return PASS;
}

// ---- parameter expansion operators -------------------------------------------

// `##` in ${x##pattern} (longest prefix removal), unless it is the whole
// of ${##} -- the length of $#.
static Outcome scan_immediate_double_hash(TSLexer *lexer) {
    if (!at(lexer, '#')) return PASS;
    lexer->mark_end(lexer);
    advance(lexer);
    if (!at(lexer, '#')) return PASS;
    advance(lexer);
    if (at(lexer, '}')) return PASS;
    lexer->mark_end(lexer);
    return produce(lexer, IMMEDIATE_DOUBLE_HASH, true);
}

// A trailing run of # ! = right before the closing brace: ${#}, ${!#},
// ${x#}, ${!=}.
static Outcome scan_expansion_symbol(TSLexer *lexer) {
    int32_t c = lexer->lookahead;
    if (c != '#' && c != '=' && c != '!') return PASS;
    lexer->result_symbol = c == '#' ? EXPANSION_SYM_HASH : c == '!' ? EXPANSION_SYM_BANG : EXPANSION_SYM_EQUAL;
    advance(lexer);
    lexer->mark_end(lexer);
    while (at(lexer, '#') || at(lexer, '=') || at(lexer, '!')) advance(lexer);
    while (is_space(lexer)) skip(lexer);
    return at(lexer, '}') ? TOKEN : NO_TOKEN;
}

// ---- test operators -----------------------------------------------------------
// Bash: "Bash Conditional Expressions". `-f`, `-nt`, `-eq`: a dash and
// letters, then a space.

static Outcome scan_regex(TSLexer *lexer, const bool *valid);
static Outcome scan_extglob(Scanner *scanner, TSLexer *lexer, const bool *valid, bool after_backslash);
static Outcome scan_bare_dollar(TSLexer *lexer);

typedef enum { FROM_REGEX, FROM_EXTGLOB, FROM_EXPANSION_WORD, FROM_BRACE_START } Stage;
static bool scan_from(Stage stage, Scanner *scanner, TSLexer *lexer, const bool *valid);

// Reads past the `${...}` or `$(...)` the lexer is on (bracket-balanced,
// on one line) and says whether an extglob group, `@(`, `?(`, `*(`, `+(`
// or `!(`, comes right after it. Only looks: the caller has marked the
// token's end already.
static bool extglob_group_after_expansion(TSLexer *lexer) {
    advance(lexer);  // the `$`
    int32_t open = lexer->lookahead;
    if (open != '{' && open != '(') return false;
    int32_t close = open == '{' ? '}' : ')';
    unsigned depth = 0;
    do {
        if (lexer->eof(lexer) || at(lexer, '\n')) return false;
        if (at(lexer, open)) depth++;
        if (at(lexer, close)) depth--;
        advance(lexer);
    } while (depth > 0);
    int32_t c = lexer->lookahead;
    if (c != '@' && c != '?' && c != '*' && c != '+' && c != '!') return false;
    advance(lexer);
    return at(lexer, '(');
}

static Outcome scan_test_operator(Scanner *scanner, TSLexer *lexer, const bool *valid) {
    while (is_space(lexer) && !at(lexer, '\n')) skip(lexer);

    if (at(lexer, '\\')) {
        if (valid[EXTGLOB_PATTERN]) {
            // A case pattern after a line continuation is read as one on the
            // same line would be; an escaped character starts a pattern.
            skip(lexer);
            if (at(lexer, '\r')) skip(lexer);
            if (!at(lexer, '\n')) return scan_extglob(scanner, lexer, valid, true) == TOKEN ? TOKEN : NO_TOKEN;
            skip(lexer);
            while (is_space(lexer) && !at(lexer, '\n')) skip(lexer);
        } else if (valid[REGEX_NO_SPACE]) {
            return scan_from(FROM_REGEX, scanner, lexer, valid) ? TOKEN : NO_TOKEN;
        } else {
        // A line continuation inside [[ ]].
        skip(lexer);
        if (lexer->eof(lexer)) return NO_TOKEN;
        if (at(lexer, '\r')) {
            skip(lexer);
            if (at(lexer, '\n')) skip(lexer);
        } else if (at(lexer, '\n')) {
            skip(lexer);
        } else {
            return NO_TOKEN;
        }
        while (is_space(lexer)) skip(lexer);
        }
    }

    if (at(lexer, '\n') && !valid[NEWLINE]) {
        skip(lexer);
        while (is_space(lexer)) skip(lexer);
    }

    if (at(lexer, '-')) {
        advance(lexer);
        if (valid[EXTGLOB_PATTERN] && at(lexer, '$')) {
            // `-${opts}@(a|b))` in a case pattern: the dash is a pattern
            // piece of its own, before the expansion -- when an extglob
            // group follows it. (Otherwise `-${opts}[ab]` is a word.)
            lexer->mark_end(lexer);
            return produce(lexer, EXTGLOB_PATTERN, extglob_group_after_expansion(lexer));
        }
        bool letters = false;
        while (iswalpha(lexer->lookahead)) {
            letters = true;
            advance(lexer);
        }
        if (is_space(lexer) && letters) {
            lexer->mark_end(lexer);
            advance(lexer);
            if (at(lexer, '}') && valid[CLOSING_BRACE]) {
                // ${x:-word }: the "operator" is the expansion's word.
                if (valid[EXPANSION_WORD]) {
                    lexer->mark_end(lexer);
                    return produce(lexer, EXPANSION_WORD, true);
                }
                return NO_TOKEN;
            }
            return produce(lexer, TEST_OPERATOR, true);
        }
        if (is_space(lexer) && valid[EXTGLOB_PATTERN]) return produce(lexer, EXTGLOB_PATTERN, true);
    }

    if (valid[BARE_DOLLAR] && !valid[ERROR_RECOVERY]) {
        Outcome dollar = scan_bare_dollar(lexer);
        if (dollar == TOKEN) return TOKEN;
    }
    return PASS;
}

// ---- names, file descriptors, here-document arrows ---------------------------
// A name is only a VARIABLE_NAME token when what follows makes it one
// (Bash: "Shell Parameters"): `=`, `+=`, `[` in an assignment, an
// expansion operator inside ${...}. Otherwise it is an ordinary word,
// which the grammar's own lexer reads.

// `{varname}>file`: bash allocates a descriptor and stores it in varname
// ("Redirections"). Called after the `{`, on a name's first character;
// true, with the token's end marked, when `}` and then `<` or `>` follow.
static bool scan_named_descriptor(TSLexer *lexer) {
    while (iswalnum(lexer->lookahead) || at(lexer, '_')) advance(lexer);
    if (!at(lexer, '}')) return false;
    advance(lexer);
    lexer->mark_end(lexer);
    return at(lexer, '<') || at(lexer, '>');
}

static Outcome scan_brace_start_after_open(TSLexer *lexer);

// Where a name was looked for but the next character can't start one.
static Outcome scan_after_non_name(Scanner *scanner, TSLexer *lexer, const bool *valid) {
    if (at(lexer, '{')) return scan_from(FROM_BRACE_START, scanner, lexer, valid) ? TOKEN : NO_TOKEN;
    if (valid[EXPANSION_WORD]) return scan_from(FROM_EXPANSION_WORD, scanner, lexer, valid) ? TOKEN : NO_TOKEN;
    if (valid[EXTGLOB_PATTERN]) return scan_from(FROM_EXTGLOB, scanner, lexer, valid) ? TOKEN : NO_TOKEN;
    return NO_TOKEN;
}

static Outcome scan_name(Scanner *scanner, TSLexer *lexer, const bool *valid) {
    for (;;) {
        if ((at(lexer, ' ') || at(lexer, '\t') || at(lexer, '\r') || (at(lexer, '\n') && !valid[NEWLINE])) &&
            !valid[EXPANSION_WORD]) {
            skip(lexer);
        } else if (at(lexer, '\\')) {
            skip(lexer);
            if (lexer->eof(lexer)) {
                lexer->mark_end(lexer);
                return produce(lexer, VARIABLE_NAME, true);
            }
            if (at(lexer, '\r')) skip(lexer);
            if (at(lexer, '\n')) {
                skip(lexer);
            } else {
                if (at(lexer, '\\') && valid[EXPANSION_WORD]) {
                    return scan_from(FROM_EXPANSION_WORD, scanner, lexer, valid) ? TOKEN : NO_TOKEN;
                }
                return NO_TOKEN;
            }
        } else {
            break;
        }
    }

    // Special parameters are never names: *, @, ?, - and 0 followed by an
    // operator. Before whitespace they may be a glob in a case pattern.
    // `_` and `0` can go on to be a name or a number; the others can't, so
    // `@prog@` (a nixpkgs placeholder) is a word, not a name `@prog`.
    bool consumed = false, is_number = true;
    if (!valid[EXPANSION_WORD] &&
        (at(lexer, '*') || at(lexer, '@') || at(lexer, '?') || at(lexer, '-') || at(lexer, '0') || at(lexer, '_'))) {
        int32_t first = lexer->lookahead;
        lexer->mark_end(lexer);
        advance(lexer);
        int32_t c = lexer->lookahead;
        if (c == '=' || c == '[' || c == ':' || c == '-' || c == '%' || c == '#' || c == '/') return NO_TOKEN;
        if (valid[EXTGLOB_PATTERN] && iswspace(c)) {
            lexer->mark_end(lexer);
            return produce(lexer, EXTGLOB_PATTERN, true);
        }
        // Only `_` and `0` go on into a name or number; for the others a
        // name character next means a word like `@prog@`. Anything else is
        // looked at as it would be after any other non-name character.
        if (iswalnum(c) || c == '_') {
            if (first != '0' && first != '_') return NO_TOKEN;
            consumed = true;
            is_number = first == '0';
        } else {
            return scan_after_non_name(scanner, lexer, valid);
        }
    }

    if (valid[HEREDOC_ARROW] && at(lexer, '<')) {
        advance(lexer);
        if (!at(lexer, '<')) return NO_TOKEN;
        advance(lexer);
        if (at(lexer, '-')) {
            advance(lexer);
            heredoc_push(scanner, true);
            return produce(lexer, HEREDOC_ARROW_DASH, true);
        }
        if (at(lexer, '<') || at(lexer, '=')) return NO_TOKEN;  // `<<<`, `<<=`
        heredoc_push(scanner, false);
        return produce(lexer, HEREDOC_ARROW, true);
    }

    if (consumed) {
        // Already inside a name or number.
    } else if (iswdigit(lexer->lookahead)) {
        advance(lexer);
    } else if (iswalpha(lexer->lookahead) || at(lexer, '_')) {
        is_number = false;
        advance(lexer);
    } else if (at(lexer, '{') && valid[FILE_DESCRIPTOR]) {
        // Either {fd}> or a sequence expression {1..3}: the character after
        // the brace tells them apart, and reading it consumes the brace.
        advance(lexer);
        if (iswalpha(lexer->lookahead) || at(lexer, '_')) {
            return produce(lexer, FILE_DESCRIPTOR, scan_named_descriptor(lexer));
        }
        if (!valid[BRACE_START] || valid[ERROR_RECOVERY]) return NO_TOKEN;
        return scan_brace_start_after_open(lexer);
    } else {
        return scan_after_non_name(scanner, lexer, valid);
    }
    for (;;) {
        if (iswdigit(lexer->lookahead)) {
            advance(lexer);
        } else if (iswalpha(lexer->lookahead) || at(lexer, '_')) {
            is_number = false;
            advance(lexer);
        } else {
            break;
        }
    }

    // `2>`, `10<`: a file descriptor (Bash: "Redirections").
    if (is_number && valid[FILE_DESCRIPTOR] && (at(lexer, '>') || at(lexer, '<'))) {
        return produce(lexer, FILE_DESCRIPTOR, true);
    }

    if (!valid[VARIABLE_NAME]) return NO_TOKEN;
    if (at(lexer, '+')) {
        lexer->mark_end(lexer);
        advance(lexer);
        return produce(lexer, VARIABLE_NAME, at(lexer, '=') || at(lexer, ':') || valid[CLOSING_BRACE]);
    }
    if (at(lexer, '/')) return NO_TOKEN;
    if (at(lexer, '=') || at(lexer, '[') || (at(lexer, ':') && !valid[CLOSING_BRACE] && !valid[OPENING_PAREN]) ||
        at(lexer, '%') || (at(lexer, '#') && !is_number) || (at(lexer, '@') && valid[CLOSING_BRACE]) ||
        (at(lexer, '-') && valid[CLOSING_BRACE])) {
        lexer->mark_end(lexer);
        return produce(lexer, VARIABLE_NAME, true);
    }
    if (at(lexer, '?')) {
        lexer->mark_end(lexer);
        advance(lexer);
        return produce(lexer, VARIABLE_NAME, iswalpha(lexer->lookahead));
    }
    return NO_TOKEN;
}

// A `$` standing alone as an argument: `echo $`, `"$"`.
static Outcome scan_bare_dollar(TSLexer *lexer) {
    while (is_space(lexer) && !at(lexer, '\n') && !lexer->eof(lexer)) skip(lexer);
    if (!at(lexer, '$')) return NO_TOKEN;
    advance(lexer);
    lexer->result_symbol = BARE_DOLLAR;
    lexer->mark_end(lexer);
    return is_space(lexer) || lexer->eof(lexer) || at(lexer, '"') ? TOKEN : NO_TOKEN;
}

// ---- regular expressions -------------------------------------------------------
// Three contexts: the right side of `=~` in [[ ]] (REGEX_NO_SPACE: it
// ends at unbracketed whitespace), a pattern in ${x/pattern/...}
// (REGEX_NO_SLASH: it ends at `/`), and ${x#pattern} and friends (REGEX).

typedef struct {
    bool done;
    bool advanced;
    bool special;  // saw something other than [A-Za-z0-9$_-]: not a plain word
    bool after_escape;
    bool in_single_quote;
    // A bracket expression, `[...]`, is opaque: parens and braces in it
    // are characters. Only its `]` ends it -- not one just after the `[`
    // or `[^` (`[^]]`), nor the one ending a `[:class:]` inside.
    bool in_bracket;
    bool bracket_start;
    bool class_may_open;  // just after a `[` inside the bracket
    int32_t class_char;   // `:`, `=` or `.` of an open [:class:], or 0
    int32_t prev;
    uint32_t parens;
    uint32_t braces;
} RegexState;

static Outcome scan_regex(TSLexer *lexer, const bool *valid) {
    if (!(valid[REGEX] || valid[REGEX_NO_SLASH] || valid[REGEX_NO_SPACE]) || valid[ERROR_RECOVERY]) return PASS;
    if (valid[REGEX] || valid[REGEX_NO_SPACE]) {
        while (is_space(lexer)) skip(lexer);
    }
    bool quote = at(lexer, '"') || at(lexer, '\'');
    if (quote && !(at(lexer, '\'') && (valid[REGEX_NO_SLASH] || valid[REGEX_NO_SPACE]))) return PASS;

    if (at(lexer, '$') && valid[REGEX_NO_SLASH]) {
        lexer->mark_end(lexer);
        advance(lexer);
        if (at(lexer, '(')) return NO_TOKEN;
    }
    lexer->mark_end(lexer);

    RegexState st = {0};
    while (!st.done) {
        if (st.in_single_quote && at(lexer, '\'')) {
            st.in_single_quote = false;
            advance(lexer);
            lexer->mark_end(lexer);
        }
        if (!st.in_single_quote && !st.in_bracket && at(lexer, '$') && !valid[REGEX_NO_SPACE]) {
            // ANSI-C quoting in a ${x%pattern}: `$'a\'b'` is one piece, its
            // `\'` escaped.
            advance(lexer);
            st.advanced = true;
            st.after_escape = false;
            if (at(lexer, '\'')) {
                advance(lexer);
                while (!lexer->eof(lexer) && !at(lexer, '\'')) {
                    if (at(lexer, '\\')) advance(lexer);
                    if (!lexer->eof(lexer)) advance(lexer);
                }
                if (at(lexer, '\'')) advance(lexer);
            }
            lexer->mark_end(lexer);
            continue;
        }
        int32_t c = lexer->lookahead;
        if (st.in_bracket && c == '}' && st.braces == 0 && !valid[REGEX_NO_SPACE]) {
            // Inside ${...} the closing brace ends the pattern even in an
            // unclosed bracket, as it does for bash: `${host#[}`.
            st.in_bracket = false;
        }
        if (st.in_bracket) {
            if (c == 0) return NO_TOKEN;
            bool start = st.bracket_start;
            st.bracket_start = false;
            if (st.class_char) {
                if (c == ']' && st.prev == st.class_char) st.class_char = 0;
            } else if (start && (c == '^' || c == '!')) {
                st.bracket_start = true;
            } else if (st.class_may_open && (c == ':' || c == '=' || c == '.')) {
                st.class_char = c;
            } else if (c == ']' && !start) {
                st.in_bracket = false;
            }
            st.class_may_open = c == '[' && !st.class_char;
            st.after_escape = false;
            // Nothing below applies to a character in a bracket, but for a
            // quote: `[$' \t']`.
            if (c != '\'') c = 'x';
        }
        st.prev = lexer->lookahead;
        switch (c) {
            case '\\':
                st.after_escape = true;
                break;
            case 0:
                return NO_TOKEN;
            case '(':
                st.parens++;
                st.after_escape = false;
                break;
            case '[':
                if (!st.after_escape) {
                    st.in_bracket = true;
                    st.bracket_start = true;
                }
                st.after_escape = false;
                break;
            case '{':
                if (!st.after_escape) st.braces++;
                st.after_escape = false;
                break;
            case ')':
                if (st.parens == 0) st.done = true;
                st.parens--;
                st.after_escape = false;
                break;
            case ']':
                // In ${x%pattern} only the brace ends the pattern: `${h%]*}`.
                if (!valid[REGEX]) st.done = true;
                st.after_escape = false;
                break;
            case '}':
                if (st.braces == 0) st.done = true;
                st.braces--;
                st.after_escape = false;
                break;
            case '\'':
                if (st.after_escape) {
                    // `\'` is a quote character, not a quote: `${x#* \'}`.
                    st.after_escape = false;
                    break;
                }
                st.in_single_quote = !st.in_single_quote;
                advance(lexer);
                st.advanced = true;
                continue;
            default:
                st.after_escape = false;
                break;
        }
        if (st.done) break;

        if (valid[REGEX]) {
            bool space = !st.in_single_quote && is_space(lexer);
            advance(lexer);
            st.advanced = true;
            if (!space || st.parens > 0) lexer->mark_end(lexer);
        } else if (valid[REGEX_NO_SLASH]) {
            if (at(lexer, '/')) {
                lexer->mark_end(lexer);
                return produce(lexer, REGEX_NO_SLASH, st.advanced);
            }
            if (at(lexer, '\\')) {
                advance(lexer);
                st.advanced = true;
                if (!lexer->eof(lexer) && !at(lexer, '[') && !at(lexer, '/')) {
                    advance(lexer);
                    lexer->mark_end(lexer);
                    st.after_escape = false;  // the escaped character is consumed
                }
            } else {
                bool space = !st.in_single_quote && is_space(lexer);
                advance(lexer);
                st.advanced = true;
                if (!space) lexer->mark_end(lexer);
            }
        } else if (valid[REGEX_NO_SPACE]) {
            if (at(lexer, '\\')) {
                st.special = true;
                advance(lexer);
                if (!lexer->eof(lexer)) advance(lexer);
                st.after_escape = false;  // the escaped character is consumed
            } else if (at(lexer, '$')) {
                lexer->mark_end(lexer);
                advance(lexer);
                if (at(lexer, '(')) return NO_TOKEN;  // a command substitution, not a regex
                if (is_space(lexer)) {
                    // A trailing `$` anchors: always a regex.
                    lexer->mark_end(lexer);
                    return produce(lexer, REGEX_NO_SPACE, true);
                }
            } else {
                bool space = !st.in_single_quote && is_space(lexer);
                if (space && st.parens == 0) {
                    lexer->mark_end(lexer);
                    return produce(lexer, REGEX_NO_SPACE, st.special);
                }
                int32_t c = lexer->lookahead;
                if (!iswalnum(c) && c != '$' && c != '-' && c != '_') st.special = true;
                advance(lexer);
            }
        }
    }

    enum TokenType type = valid[REGEX_NO_SLASH] ? REGEX_NO_SLASH : valid[REGEX_NO_SPACE] ? REGEX_NO_SPACE : REGEX;
    return produce(lexer, type, !(valid[REGEX] && !st.advanced));
}

// ---- extended globs -------------------------------------------------------------
// Bash: "Pattern Matching" with extglob: ?(...) *(...) +(...) @(...)
// !(...), as case patterns and on the right of == / != in [[ ]]. A
// pattern of plain letters is left to the grammar as a word.

static bool at_esac(TSLexer *lexer) {
    // Called on 'e'; consumes what it matches.
    advance(lexer);
    if (!at(lexer, 's')) return false;
    advance(lexer);
    if (!at(lexer, 'a')) return false;
    advance(lexer);
    if (!at(lexer, 'c')) return false;
    advance(lexer);
    return is_space(lexer);
}

static inline bool alpha_or_dot(int32_t c) { return iswalpha(c) || c == '.' || c == '\\'; }

// `after_backslash`: the caller has already skipped a backslash here.
static Outcome scan_extglob(Scanner *scanner, TSLexer *lexer, const bool *valid, bool after_backslash) {
    if (!valid[EXTGLOB_PATTERN] || valid[ERROR_RECOVERY]) return PASS;
    // Blanks and line continuations before the pattern. A backslash before
    // a blank or a quote starts the pattern with that escaped character
    // (its token starts after the backslash, which is skipped by then).
    bool escaped = false;
    for (;;) {
        if (after_backslash || at(lexer, '\\')) {
            if (!after_backslash) skip(lexer);
            after_backslash = false;
            if (at(lexer, '\r')) skip(lexer);
            if (at(lexer, '\n')) {
                skip(lexer);
                continue;
            }
            if (!(is_space(lexer) || at(lexer, '"'))) return NO_TOKEN;
            escaped = true;
            break;
        }
        if (!is_space(lexer)) break;
        skip(lexer);
    }

    int32_t c = lexer->lookahead;
    // A path-like pattern, `/..?(/*)`, is one only with an extglob group in it.
    bool starts_with_slash = c == '/';
    if (escaped) {
        advance(lexer);
    } else if (!(c == '?' || c == '*' || c == '+' || c == '@' || c == '!' || c == '-' || c == ')' || c == '.' ||
                 c == '[' || c == '/' || iswalpha(c))) {
        scanner->glob_paren_depth = 0;
        return NO_TOKEN;
    }

    if (at(lexer, ')') && scanner->glob_paren_depth == 0) {
        lexer->mark_end(lexer);
        advance(lexer);
        if (is_space(lexer)) return NO_TOKEN;
    }

    lexer->mark_end(lexer);
    bool non_alpha = !iswalpha(lexer->lookahead);
    if (!at(lexer, '[')) {
        if (at(lexer, 'e')) {
            lexer->mark_end(lexer);
            if (at_esac(lexer)) return NO_TOKEN;  // the `esac` closing the case
        } else {
            advance(lexer);
        }
    }

    // `-word` is just a word: look for something more.
    if (at(lexer, '-')) {
        lexer->mark_end(lexer);
        advance(lexer);
        while (iswalnum(lexer->lookahead)) advance(lexer);
        if (at(lexer, ')') || at(lexer, '\\') || at(lexer, '.')) return NO_TOKEN;
        lexer->mark_end(lexer);
    }

    // A one-character case pattern: `*)`, `-)`.
    if (at(lexer, ')') && scanner->glob_paren_depth == 0) {
        lexer->mark_end(lexer);
        advance(lexer);
        if (is_space(lexer)) return produce(lexer, EXTGLOB_PATTERN, non_alpha && !starts_with_slash);
    }

    if (is_space(lexer)) {
        lexer->mark_end(lexer);
        scanner->glob_paren_depth = 0;
        return produce(lexer, EXTGLOB_PATTERN, !starts_with_slash);
    }

    if (at(lexer, '$')) {
        lexer->mark_end(lexer);
        advance(lexer);
        if (at(lexer, '{') || at(lexer, '(')) return produce(lexer, EXTGLOB_PATTERN, !starts_with_slash);
    }

    if (at(lexer, '|')) {
        lexer->mark_end(lexer);
        advance(lexer);
        return produce(lexer, EXTGLOB_PATTERN, !starts_with_slash);
    }

    // A prefix before the pattern proper: `*.@(jar|zip)`, `--@(a|b)`. It
    // is only a pattern if an extglob group follows; `*.txt` stays a word.
    c = lexer->lookahead;
    bool need_group = starts_with_slash || c == '.' || c == '@' || c == '!' || c == '+' || c == '-';
    if (!need_group && !iswalnum(c) && c != '(' && c != '"' && c != '[' && c != '?' && c != '/' && c != '\\' &&
        c != '_' && c != '*') {
        return NO_TOKEN;
    }

    bool special = non_alpha, saw_group = false;
    int32_t prev = 0;
    uint32_t parens = scanner->glob_paren_depth, brackets = 0, braces = 0;
    bool done = false;
#define GLOB(ok) produce(lexer, EXTGLOB_PATTERN, (ok) && (saw_group || !need_group))
    while (!done) {
        switch (lexer->lookahead) {
            case 0:
                return NO_TOKEN;
            case '(':
                if (prev == '?' || prev == '*' || prev == '+' || prev == '@' || prev == '!') saw_group = true;
                parens++;
                break;
            case '[':
                brackets++;
                break;
            case '{':
                braces++;
                break;
            case ')':
                if (parens == 0) done = true;
                parens--;
                break;
            case ']':
                if (brackets == 0) done = true;
                brackets--;
                break;
            case '}':
                if (braces == 0) done = true;
                braces--;
                break;
        }

        if (at(lexer, '|')) {
            lexer->mark_end(lexer);
            advance(lexer);
            prev = '|';
            if (parens == 0 && brackets == 0 && braces == 0) return GLOB(true);
            // The next alternative's first character goes through the
            // bracket counting too: `@(help|[h?])`.
            continue;
        }

        if (done) break;
        bool space = is_space(lexer);
        if (at(lexer, '$')) {
            lexer->mark_end(lexer);
            special = true;  // `$` is neither a letter nor a dot
            advance(lexer);
            prev = '$';
            if (at(lexer, '(') || at(lexer, '{')) {
                // An expansion inside the pattern: stop here and remember
                // how deep in parens the pattern was, to resume after it.
                scanner->glob_paren_depth = (uint8_t)parens;
                return GLOB(special);
            }
        }
        if (parens > 0 && (at(lexer, '\'') || at(lexer, '"'))) {
            // A quoted alternative inside a group: `@(a|'')`, `@(x|"y z")`.
            int32_t quote = lexer->lookahead;
            advance(lexer);
            while (!lexer->eof(lexer) && !at(lexer, quote) && !at(lexer, '\n')) {
                if (quote == '"' && at(lexer, '\\')) advance(lexer);
                advance(lexer);
            }
            if (!at(lexer, quote)) return NO_TOKEN;
            advance(lexer);
            lexer->mark_end(lexer);
            special = true;
            prev = quote;
            continue;
        }
        // Blanks end the pattern outside a group (inside one, `?( *)`, they
        // are part of it, up to the end of the line); quotes start a piece
        // of their own.
        if ((space && (parens == 0 || at(lexer, '\n'))) || at(lexer, '"') || at(lexer, '\'')) {
            lexer->mark_end(lexer);
            scanner->glob_paren_depth = 0;
            return GLOB(special);
        }
        prev = lexer->lookahead;
        if (at(lexer, '\\')) {
            // An escape takes the next character, whatever it is: `\ `,
            // `\'`, `\(` are text, and `\\'` is a backslash, then a quote.
            advance(lexer);
            if (!lexer->eof(lexer)) advance(lexer);
            prev = 0;
        } else {
            if (!alpha_or_dot(lexer->lookahead)) special = true;
            advance(lexer);
        }
        if (!space) lexer->mark_end(lexer);
    }

    scanner->glob_paren_depth = 0;
    return GLOB(special);
#undef GLOB
}

// ---- words inside ${...} ---------------------------------------------------------
// The word of ${x:-word}, ${x/a/word}: anything up to the closing brace or
// an expansion, which the grammar reads itself.

static Outcome scan_expansion_word(TSLexer *lexer, const bool *valid) {
    if (!valid[EXPANSION_WORD]) return PASS;
    bool advanced = false, advanced_space = false;
    for (;;) {
        if (at(lexer, '"')) return NO_TOKEN;
        if (at(lexer, '$')) {
            lexer->mark_end(lexer);
            advance(lexer);
            if (at(lexer, '{') || at(lexer, '(') || at(lexer, '\'') || iswalnum(lexer->lookahead)) {
                return produce(lexer, EXPANSION_WORD, advanced);
            }
            advanced = true;
        }
        if (at(lexer, '}')) {
            lexer->mark_end(lexer);
            return produce(lexer, EXPANSION_WORD, advanced || advanced_space);
        }
        if (at(lexer, '(') && !(advanced || advanced_space)) {
            lexer->mark_end(lexer);
            advance(lexer);
            while (!at(lexer, ')') && !lexer->eof(lexer)) {
                if (at(lexer, '$')) {
                    // A $( or ${ inside: a word glued to an expansion.
                    lexer->mark_end(lexer);
                    advance(lexer);
                    if (at(lexer, '{') || at(lexer, '(') || at(lexer, '\'') || iswalnum(lexer->lookahead)) {
                        return produce(lexer, EXPANSION_WORD, advanced);
                    }
                    advanced = true;
                } else {
                    advanced = advanced || !is_space(lexer);
                    advanced_space = advanced_space || is_space(lexer);
                    advance(lexer);
                }
            }
            lexer->mark_end(lexer);
            if (!at(lexer, ')')) return NO_TOKEN;
            advanced = true;
            advance(lexer);
            lexer->mark_end(lexer);
            if (at(lexer, '}')) return NO_TOKEN;
        }
        if (at(lexer, '\'')) return NO_TOKEN;
        if (lexer->eof(lexer)) return NO_TOKEN;
        advanced = advanced || !is_space(lexer);
        advanced_space = advanced_space || is_space(lexer);
        advance(lexer);
    }
}

// ---- brace expansion --------------------------------------------------------------
// The `{` of a sequence expression {1..10} (Bash: "Brace Expansion"); any
// other `{` is a word character or a brace group.

static Outcome scan_brace_start(TSLexer *lexer, const bool *valid) {
    if (!valid[BRACE_START] || valid[ERROR_RECOVERY]) return PASS;
    while (is_space(lexer)) skip(lexer);
    if (!at(lexer, '{')) return NO_TOKEN;
    advance(lexer);
    return scan_brace_start_after_open(lexer);
}

static Outcome scan_brace_start_after_open(TSLexer *lexer) {
    lexer->mark_end(lexer);
    while (iswdigit(lexer->lookahead)) advance(lexer);
    if (!at(lexer, '.')) return NO_TOKEN;
    advance(lexer);
    if (!at(lexer, '.')) return NO_TOKEN;
    advance(lexer);
    while (iswdigit(lexer->lookahead)) advance(lexer);
    return produce(lexer, BRACE_START, at(lexer, '}'));
}

// The last families, in order, each looking only if the previous passed.
static bool scan_from(Stage stage, Scanner *scanner, TSLexer *lexer, const bool *valid) {
    Outcome r = PASS;
    switch (stage) {
        case FROM_REGEX:
            r = scan_regex(lexer, valid);
            if (r != PASS) return r == TOKEN;
            // fall through
        case FROM_EXTGLOB:
            r = scan_extglob(scanner, lexer, valid, false);
            if (r != PASS) return r == TOKEN;
            // fall through
        case FROM_EXPANSION_WORD:
            r = scan_expansion_word(lexer, valid);
            if (r != PASS) return r == TOKEN;
            // fall through
        case FROM_BRACE_START:
            r = scan_brace_start(lexer, valid);
            return r == TOKEN;
    }
    return false;
}

static bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid) {
    bool recovering = valid[ERROR_RECOVERY];
    Outcome r;

    if (valid[CONCAT] && !recovering) {
        r = scan_concat(lexer, valid);
        if (r != PASS) return r == TOKEN;
    }
    if (valid[IMMEDIATE_DOUBLE_HASH] && !recovering) {
        r = scan_immediate_double_hash(lexer);
        if (r != PASS) return r == TOKEN;
    }
    if (valid[EXPANSION_SYM_HASH] && !recovering) {
        r = scan_expansion_symbol(lexer);
        if (r != PASS) return r == TOKEN;
    }
    if (valid[EMPTY_VALUE] && (is_space(lexer) || lexer->eof(lexer) || at(lexer, ';') || at(lexer, '&'))) {
        // `x=` with nothing after it.
        return produce(lexer, EMPTY_VALUE, true) == TOKEN;
    }

    if (scanner->heredocs.size > 0 && !recovering) {
        Heredoc *heredoc = array_back(&scanner->heredocs);
        if ((valid[HEREDOC_BODY_BEGINNING] || valid[SIMPLE_HEREDOC_BODY]) && !heredoc->started) {
            return scan_heredoc_content(scanner, lexer, HEREDOC_BODY_BEGINNING, SIMPLE_HEREDOC_BODY) == TOKEN;
        }
    }
    if (valid[HEREDOC_END] && scanner->heredocs.size > 0) {
        if (at_delimiter(array_back(&scanner->heredocs), lexer)) {
            heredoc_pop(scanner);
            return produce(lexer, HEREDOC_END, true) == TOKEN;
        }
    }
    if (scanner->heredocs.size > 0 && !recovering) {
        Heredoc *heredoc = array_back(&scanner->heredocs);
        if (valid[HEREDOC_CONTENT] && heredoc->started) {
            return scan_heredoc_content(scanner, lexer, HEREDOC_CONTENT, HEREDOC_END) == TOKEN;
        }
        if (valid[HEREDOC_START]) return scan_heredoc_start(heredoc, lexer) == TOKEN;
    }

    if (valid[TEST_OPERATOR] && !valid[EXPANSION_WORD]) {
        r = scan_test_operator(scanner, lexer, valid);
        if (r != PASS) return r == TOKEN;
    }

    if ((valid[VARIABLE_NAME] || valid[FILE_DESCRIPTOR] || valid[HEREDOC_ARROW]) && !valid[REGEX_NO_SLASH] &&
        !recovering) {
        return scan_name(scanner, lexer, valid) == TOKEN;
    }

    if (valid[BARE_DOLLAR] && !recovering && scan_bare_dollar(lexer) == TOKEN) return true;

    return scan_from(FROM_REGEX, scanner, lexer, valid);
}

// ---- tree-sitter's interface ---------------------------------------------------------

void *tree_sitter_bash_external_scanner_create(void) {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    array_init(&scanner->heredocs);
    return scanner;
}

bool tree_sitter_bash_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    return scan((Scanner *)payload, lexer, valid_symbols);
}

// Layout: glob paren depth, heredoc count, then per heredoc its three
// flags, its delimiter's length (uint32) and the delimiter. A stack too
// big for the buffer serializes as far as it fits.
unsigned tree_sitter_bash_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    unsigned size = 0;
    buffer[size++] = (char)scanner->glob_paren_depth;
    unsigned count_at = size++;
    uint8_t count = 0;
    for (uint32_t i = 0; i < scanner->heredocs.size && count < UINT8_MAX; i++) {
        Heredoc *heredoc = array_get(&scanner->heredocs, i);
        uint32_t len = heredoc->delimiter.size;
        if (size + 3 + sizeof(uint32_t) + len > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) break;
        buffer[size++] = (char)heredoc->quoted;
        buffer[size++] = (char)heredoc->started;
        buffer[size++] = (char)heredoc->strip_tabs;
        memcpy(&buffer[size], &len, sizeof(uint32_t));
        size += sizeof(uint32_t);
        if (len > 0) memcpy(&buffer[size], heredoc->delimiter.contents, len);
        size += len;
        count++;
    }
    buffer[count_at] = (char)count;
    return size;
}

void tree_sitter_bash_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    for (uint32_t i = 0; i < scanner->heredocs.size; i++) {
        array_delete(&array_get(&scanner->heredocs, i)->delimiter);
    }
    array_clear(&scanner->heredocs);
    scanner->glob_paren_depth = 0;
    if (length < 2) return;

    unsigned size = 0;
    scanner->glob_paren_depth = (uint8_t)buffer[size++];
    uint8_t count = (uint8_t)buffer[size++];
    for (uint8_t i = 0; i < count && size + 3 + sizeof(uint32_t) <= length; i++) {
        Heredoc heredoc = {.delimiter = array_new()};
        heredoc.quoted = buffer[size++];
        heredoc.started = buffer[size++];
        heredoc.strip_tabs = buffer[size++];
        uint32_t len;
        memcpy(&len, &buffer[size], sizeof(uint32_t));
        size += sizeof(uint32_t);
        if (len > length - size) len = length - size;
        if (len > 0) {
            array_reserve(&heredoc.delimiter, len);
            memcpy(heredoc.delimiter.contents, &buffer[size], len);
            heredoc.delimiter.size = len;
        }
        size += len;
        array_push(&scanner->heredocs, heredoc);
    }
}

void tree_sitter_bash_external_scanner_destroy(void *payload) {
    Scanner *scanner = (Scanner *)payload;
    for (uint32_t i = 0; i < scanner->heredocs.size; i++) {
        array_delete(&array_get(&scanner->heredocs, i)->delimiter);
    }
    array_delete(&scanner->heredocs);
    free(scanner);
}
