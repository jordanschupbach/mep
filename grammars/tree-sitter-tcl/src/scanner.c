// The external scanner for mep's Tcl grammar (grammars/tree-sitter-tcl).
//
// Tcl's lexical rules are positional -- a quote opens a quoted word only at
// the start of a word, `#` starts a comment only where a command could
// start, a braced word ends at the brace that balances it however it is
// nested -- so the parts of a word that depend on what came before are
// decided here:
//
//  _concat            zero-width: the next character continues this word
//  _var_index_open    `(` right after `$name`
//  _word_index_open   zero-width, at a `(` right after a bare word that a
//                     `)` closes before the word ends
//  unpack             `{*}` followed by more of the word
//  _braced_data       a whole braced word whose text is not something the
//                     grammar reads (a regexp, a list with a lone quote):
//                     the scanner finds its matching brace the way Tcl does
//                     (tclParse.c's ParseBraces) and checks the text with
//                     the same rules the grammar follows -- as a script
//                     where a braced word may hold commands (_script_mode
//                     is valid), as words where it holds words
//                     (_words_mode); those two are never produced, only
//                     asked about
//  _dollar_literal    a `$` that does not start a variable
//  _newline           a newline where it ends a command
//  comment            `#` to the end of the line (a backslash-newline
//                     continues it); inside braces it ends at the brace
//                     that closes them, as Tcl's brace counting does
//  _script_close      the `}` that closes a braced script
//
// Tcl counts the braces inside comments too, so commented-out code like
//
//     #if {$x} {
//     #    puts $x
//     #}
//
// is balanced only across the three lines. The scanner's one piece of state
// is that running count (`carry`): the braces comments inside braced
// scripts have left open. A `}` in a comment is the one that closes the
// braced script only when nothing is left open.

#include "tree_sitter/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
  CONCAT,
  VAR_INDEX_OPEN,
  WORD_INDEX_OPEN,
  UNPACK,
  BRACED_DATA,
  SCRIPT_MODE,
  WORDS_MODE,
  DOLLAR_LITERAL,
  NEWLINE,
  COMMENT,
  SCRIPT_CLOSE,
  ERROR_SENTINEL,
};

#define FAIL UINT32_MAX
// Brackets nest inside braced text this deep before the check gives up
// (and calls the text data): the check recurses once per `[`.
#define MAX_NEST 200

typedef struct {
  int32_t *text;
  uint32_t size;
  uint32_t capacity;
  int32_t carry;  // braces left open by comments in braced scripts
} Scanner;

void *tree_sitter_tcl_external_scanner_create(void) {
  return calloc(1, sizeof(Scanner));
}

void tree_sitter_tcl_external_scanner_destroy(void *payload) {
  Scanner *s = payload;
  free(s->text);
  free(s);
}

unsigned tree_sitter_tcl_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = payload;
  memcpy(buffer, &s->carry, sizeof s->carry);
  return sizeof s->carry;
}

void tree_sitter_tcl_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *s = payload;
  s->carry = 0;
  if (length == sizeof s->carry) memcpy(&s->carry, buffer, sizeof s->carry);
}

static void push(Scanner *s, int32_t c) {
  if (s->size == s->capacity) {
    uint32_t cap = s->capacity ? s->capacity * 2 : 256;
    int32_t *grown = realloc(s->text, cap * sizeof(int32_t));
    if (!grown) return;
    s->text = grown;
    s->capacity = cap;
  }
  s->text[s->size++] = c;
}

static bool is_blank(int32_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_space(int32_t c) { return is_blank(c) || c == '\n'; }

static bool is_name_char(int32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  return iswalnum((wint_t)c);
}

// ---- the check: is braced text something the grammar reads? -------------
//
// These mirror the grammar, not Tcl alone: they fail on what Tcl rejects
// (an unclosed quote, bracket, `${` or `$a(`) and also on what Tcl accepts
// but the grammar would misread (a `]` outside brackets; a comment on the
// last line, whose `}` the comment scanner would not take for the closing
// one while `carry` says braces are open).

static uint32_t check_script_carry(const int32_t *t, uint32_t i, uint32_t n, bool bracket, int nest, int32_t carry);

static uint32_t check_script(const int32_t *t, uint32_t i, uint32_t n, bool bracket, int nest);

static uint32_t check_var(const int32_t *t, uint32_t i, uint32_t n, int nest) {
  // t[i] is '$'.
  uint32_t j = i + 1;
  if (j < n && t[j] == '{') {
    for (uint32_t k = j + 1; k < n; k++) {
      if (t[k] == '}') return k + 1;
    }
    return FAIL;
  }
  uint32_t k = j;
  while (k < n) {
    if (is_name_char(t[k])) {
      k++;
    } else if (t[k] == ':' && k + 1 < n && t[k + 1] == ':') {
      while (k < n && t[k] == ':') k++;
    } else {
      break;
    }
  }
  if (k == j) return i + 1;  // a literal `$`
  if (k < n && t[k] == '(') {
    uint32_t m = k + 1;
    while (m < n) {
      int32_t c = t[m];
      if (c == ')') return m + 1;
      if (c == '\\') {
        m += 2;
      } else if (c == '[') {
        m = check_script(t, m + 1, n, true, nest + 1);
        if (m == FAIL) return FAIL;
      } else if (c == '$') {
        m = check_var(t, m, n, nest);
        if (m == FAIL) return FAIL;
      } else {
        m++;
      }
    }
    return FAIL;
  }
  return k;
}

static uint32_t match_brace(const int32_t *t, uint32_t i, uint32_t n) {
  // t[i] is '{'; the index after its matching '}'.
  int depth = 0;
  for (uint32_t k = i; k < n; k++) {
    int32_t c = t[k];
    if (c == '\\') {
      k++;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      if (--depth == 0) return k + 1;
    }
  }
  return FAIL;
}

// One word from t[i] (not a separator): the index where it ends.
static uint32_t check_word(const int32_t *t, uint32_t i, uint32_t n, bool bracket, int nest) {
  if (t[i] == '{') return match_brace(t, i, n);
  if (t[i] == '"') {
    i++;
    for (;;) {
      if (i >= n) return FAIL;
      int32_t c = t[i];
      if (c == '"') {
        i++;
        break;
      }
      if (c == '\\') {
        i += 2;
      } else if (c == '[') {
        i = check_script(t, i + 1, n, true, nest + 1);
        if (i == FAIL) return FAIL;
      } else if (c == '$') {
        i = check_var(t, i, n, nest);
        if (i == FAIL) return FAIL;
      } else {
        i++;
      }
    }
  }
  while (i < n) {
    int32_t c = t[i];
    if (is_space(c) || c == ';' || c == '{') return i;
    if (c == ']') return bracket ? i : FAIL;
    if (c == '}') return FAIL;
    if (c == '\\') {
      if (i + 1 < n && (t[i + 1] == '\n' || (t[i + 1] == '\r' && i + 2 < n && t[i + 2] == '\n'))) return i;
      i += 2;
    } else if (c == '[') {
      i = check_script(t, i + 1, n, true, nest + 1);
      if (i == FAIL) return FAIL;
    } else if (c == '$') {
      i = check_var(t, i, n, nest);
      if (i == FAIL) return FAIL;
    } else {
      i++;
    }
  }
  return i > n ? n : i;
}

static uint32_t skip_blanks(const int32_t *t, uint32_t i, uint32_t n) {
  while (i < n) {
    if (is_blank(t[i])) {
      i++;
    } else if (t[i] == '\\' && i + 1 < n && t[i + 1] == '\n') {
      i += 2;
    } else if (t[i] == '\\' && i + 2 < n && t[i + 1] == '\r' && t[i + 2] == '\n') {
      i += 3;
    } else {
      break;
    }
  }
  return i;
}

// A script from t[i]: commands to the end of the text, or, inside
// brackets, to the `]` that closes them (the index after it).
static uint32_t check_script(const int32_t *t, uint32_t i, uint32_t n, bool bracket, int nest) {
  return check_script_carry(t, i, n, bracket, nest, 0);
}

static uint32_t check_script_carry(const int32_t *t, uint32_t i, uint32_t n, bool bracket, int nest, int32_t carry) {
  if (nest > MAX_NEST) return FAIL;
  for (;;) {
    // Between commands.
    for (;;) {
      i = skip_blanks(t, i, n);
      if (i < n && (t[i] == '\n' || t[i] == ';')) {
        i++;
      } else {
        break;
      }
    }
    if (i >= n) return bracket ? FAIL : n;
    if (bracket && t[i] == ']') return i + 1;
    if (t[i] == '#') {
      while (i < n && t[i] != '\n') {
        if (t[i] == '\\') {
          i++;
          if (i < n && t[i] == '\r') i++;
          if (i < n) i++;
          continue;
        }
        i++;
      }
      // Running into the closing brace: the comment scanner stops there
      // only if no comment before has left braces open.
      if (i >= n && !bracket && carry != 0) return FAIL;
      continue;
    }
    // The command's words.
    for (;;) {
      i = skip_blanks(t, i, n);
      if (i >= n) return bracket ? FAIL : n;
      if (t[i] == '\n' || t[i] == ';') break;
      if (bracket && t[i] == ']') return i + 1;
      i = check_word(t, i, n, bracket, nest);
      if (i == FAIL) return FAIL;
    }
  }
}

// Words, separated by any space, newlines and `;` included: what a braced
// word that is not a script holds (a list, a regexp, an expression).
static bool check_words(const int32_t *t, uint32_t n) {
  uint32_t i = 0;
  for (;;) {
    for (;;) {
      i = skip_blanks(t, i, n);
      if (i < n && (t[i] == '\n' || t[i] == ';')) {
        i++;
      } else {
        break;
      }
    }
    if (i >= n) return true;
    i = check_word(t, i, n, false, 0);
    if (i == FAIL) return false;
  }
}

// ---- the scanner -----------------------------------------------------------

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static bool word_end(int32_t c) { return is_space(c) || c == ';' || c == ']' || c == '}' || c == 0; }

static bool scan_comment(Scanner *s, TSLexer *lexer, bool in_braces) {
  int32_t depth = 0;
  advance(lexer);  // '#'
  for (;;) {
    lexer->mark_end(lexer);
    int32_t c = lexer->lookahead;
    if (lexer->eof(lexer) || c == '\n') break;
    if (c == '\\') {
      advance(lexer);
      if (lexer->lookahead == '\r') advance(lexer);
      if (!lexer->eof(lexer)) advance(lexer);
      continue;
    }
    if (c == '{') depth++;
    if (c == '}') {
      if (in_braces && s->carry + depth <= 0) break;
      depth--;
    }
    advance(lexer);
  }
  if (in_braces) {
    s->carry += depth;
    if (s->carry < 0) s->carry = 0;
  }
  lexer->result_symbol = COMMENT;
  return true;
}

// At '{': read the braced word into s->text (its content) and decide.
static bool scan_brace(Scanner *s, TSLexer *lexer, const bool *valid) {
  bool want_data = valid[BRACED_DATA] && (valid[SCRIPT_MODE] || valid[WORDS_MODE]);
  if (!valid[UNPACK] && !want_data) return false;
  s->size = 0;
  advance(lexer);  // '{'
  int depth = 1;
  for (;;) {
    if (lexer->eof(lexer)) return false;  // unbalanced: the grammar's errors
    int32_t c = lexer->lookahead;
    if (c == '\\') {
      push(s, c);
      advance(lexer);
      if (lexer->eof(lexer)) return false;
      push(s, lexer->lookahead);
      advance(lexer);
      continue;
    }
    if (c == '{') depth++;
    if (c == '}' && --depth == 0) {
      advance(lexer);
      break;
    }
    push(s, c);
    advance(lexer);
  }
  lexer->mark_end(lexer);
  if (valid[UNPACK] && s->size == 1 && s->text[0] == '*' && !word_end(lexer->lookahead) &&
      !lexer->eof(lexer)) {
    lexer->result_symbol = UNPACK;
    return true;
  }
  // Data when any reading that could take it would fail on the text.
  if (want_data && ((valid[SCRIPT_MODE] && check_script_carry(s->text, 0, s->size, false, 0, s->carry) == FAIL) ||
                    (valid[WORDS_MODE] && !check_words(s->text, s->size)))) {
    lexer->result_symbol = BRACED_DATA;
    return true;
  }
  return false;
}

// At '(' after a bare word: does a ')' close it before the word ends?
// Inside a command substitution in it anything but a bracket goes.
static bool closes_in_word(TSLexer *lexer) {
  advance(lexer);  // '('
  int brackets = 0;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (lexer->eof(lexer)) return false;
    if (c == '\\') {
      advance(lexer);
      if (lexer->eof(lexer)) return false;
      if (brackets == 0 && (lexer->lookahead == '\n' || lexer->lookahead == '\r')) return false;
    } else if (c == '[') {
      brackets++;
    } else if (c == ']') {
      if (brackets == 0) return false;
      brackets--;
    } else if (brackets == 0) {
      if (is_space(c) || c == ';' || c == '{' || c == '}' || c == '"') return false;
      if (c == ')') return true;
    }
    advance(lexer);
  }
}

bool tree_sitter_tcl_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid) {
  Scanner *s = payload;
  int32_t c = lexer->lookahead;

  if (valid[ERROR_SENTINEL]) {
    // Error recovery: only line ends, so it can pick up at the next command.
    while (is_blank(lexer->lookahead)) lexer->advance(lexer, true);
    if (lexer->lookahead == '\n') {
      advance(lexer);
      lexer->mark_end(lexer);
      lexer->result_symbol = NEWLINE;
      return true;
    }
    return false;
  }

  // Tokens that only count with nothing before them.
  if (c == '(' && valid[VAR_INDEX_OPEN]) {
    advance(lexer);
    lexer->result_symbol = VAR_INDEX_OPEN;
    return true;
  }
  if (c == '(' && valid[WORD_INDEX_OPEN]) {
    lexer->mark_end(lexer);  // both possible results are zero-width
    if (closes_in_word(lexer)) {
      lexer->result_symbol = WORD_INDEX_OPEN;
      return true;
    }
    if (valid[CONCAT]) {
      lexer->result_symbol = CONCAT;
      return true;
    }
    return false;
  }
  if (valid[CONCAT] && !lexer->eof(lexer) && !is_space(c) && c != ';' && c != ']' && c != '}') {
    if (c != '\\') {
      lexer->mark_end(lexer);
      lexer->result_symbol = CONCAT;
      return true;
    }
    // A backslash-newline separates words; any other escape continues one.
    // (Skipping the newline moves the start of whatever token comes next
    // past the backslash too.)
    lexer->mark_end(lexer);
    advance(lexer);
    if (lexer->lookahead == '\r') advance(lexer);
    if (lexer->lookahead != '\n') {
      lexer->result_symbol = CONCAT;
      return true;
    }
    lexer->advance(lexer, true);
  }

  // Tokens at the start of a word or command: skip the space before them.
  for (;;) {
    c = lexer->lookahead;
    if (is_blank(c) || (c == '\n' && !valid[NEWLINE])) {
      lexer->advance(lexer, true);
    } else if (c == '\\') {
      // A backslash-newline is space; anything else is the grammar's.
      lexer->advance(lexer, true);
      if (lexer->lookahead == '\r') lexer->advance(lexer, true);
      if (lexer->lookahead != '\n') return false;
      lexer->advance(lexer, true);
    } else {
      break;
    }
  }
  if (lexer->eof(lexer)) return false;

  if (c == '\n' && valid[NEWLINE]) {
    advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = NEWLINE;
    return true;
  }
  if (c == '#' && valid[COMMENT]) return scan_comment(s, lexer, valid[SCRIPT_CLOSE]);
  if (c == '}' && valid[SCRIPT_CLOSE]) {
    advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = SCRIPT_CLOSE;
    return true;
  }
  if (c == '{') return scan_brace(s, lexer, valid);
  if (c == '$' && valid[DOLLAR_LITERAL]) {
    advance(lexer);
    lexer->mark_end(lexer);
    int32_t next = lexer->lookahead;
    if (lexer->eof(lexer) || next == '{' || is_name_char(next)) return false;
    if (next == ':') {
      advance(lexer);
      if (lexer->lookahead == ':') return false;
    }
    lexer->result_symbol = DOLLAR_LITERAL;
    return true;
  }
  return false;
}
