/**
 * Scheme, mep's own grammar (grammars/tree-sitter-scheme/README.md).
 *
 * The tree is what a Scheme reader reads: atoms, lists, vectors and the
 * quote-like prefixes, with whitespace and comments between them. There is
 * no `define` node -- what a list means is the queries' business, decided
 * from its first symbol, so a library's own `define-record-type*` reads
 * exactly like the standard's.
 *
 * One lexical syntax covers the three reports people write in -- R5RS,
 * R6RS and R7RS-small -- plus the extensions Guile, Chez, Chicken and
 * Racket share (`#:keyword`, `#!directive`, `#\nel`...). A highlighter must
 * not stop at a dialect boundary, so a token valid in any of them is
 * accepted.
 *
 * The node names, the tree shapes and the token boundaries are those of
 * 6cdh/tree-sitter-scheme (MIT, Copyright (c) 2022 6cdh), the grammar mep
 * loaded before this one, so queries written for that grammar keep
 * working; the number syntax below follows its reading of the three
 * reports, quirks included (see README.md).
 */

// ---- Character classes ------------------------------------------------------

// R7RS 7.1.1 <whitespace>, widened to Unicode's separators (R6RS 4.2.1).
const WHITESPACE = /[ \r\n\t\f\v\p{Zs}\p{Zl}\p{Zp}]/;
const INTRALINE_WHITESPACE = /[\t\p{Zs}]/;
const LINE_ENDING = /[\n\r\u{2028}\u{0085}]|(\r\n)|(\r\u{0085})/;
const ANY_CHAR = /.|[\r\n\u{85}\u{2028}\u{2029}]/;

// Anything that is not a delimiter, a prefix character or `#`/`|`/`\`.
// A symbol is a run of these (so `1+`, `->x`, `...` and `a.b` are one
// token each) or a `|...|` symbol.
const SYMBOL_CHAR = /[^ \r\n\t\f\v\p{Zs}\p{Zl}\p{Zp}#;"'`,\(\)\{\}\[\]\\\|]/;

const BAR_SYMBOL = seq(
  '|',
  repeat(choice(/[^\|\\]+/, /\\[xX][0-9a-fA-F]+;/, /\\[abtnr]/, '\\|')),
  '|');

const SYMBOL = choice(repeat1(SYMBOL_CHAR), BAR_SYMBOL);

// The name after `#!` is the same token as a symbol, spelled one level of
// token() shallower than `symbol` below on purpose: tree-sitter merges
// identical token rules, and the merged token would take the directive's
// place in the rule order -- ahead of `number` -- so `2003` would read as
// a symbol. Kept apart, a number and a symbol of the same length tie in
// the number's favour, as they did in the grammar this one replaces.
const DIRECTIVE_NAME = token(SYMBOL);

// ---- Booleans, characters, escapes --------------------------------------------

// #t/#f (every report), #true/#false (R7RS); case-insensitive.
const BOOLEAN = seq('#', choice(/[tTfF]/, /[tT][rR][uU][eE]/, /[fF][aA][lL][sS][eE]/));

// `#\` then a character name or any single character. The names are the
// union of R5RS (case-insensitive `space`/`newline`), R6RS and R7RS, the
// hex escapes of both, and the names Guile/Chez/Chicken add.
const CHARACTER = seq('#\\', choice(
  /[sS][pP][aA][cC][eE]/, /[nN][eE][wW][lL][iI][nN][eE]/,
  'nul', 'alarm', 'backspace', 'tab', 'linefeed', 'newline', 'vtab', 'page', 'return', 'esc', 'space',
  'delete', 'escape', 'null',
  /x[0-9a-fA-F]+/, /u[0-9a-fA-F]+/, /[xX][0-9a-fA-F]+/,
  'bel', 'ls', 'nel', 'rubout', 'vt',
  ANY_CHAR));

// A backslash escape inside a string: `\"` `\\` (R5RS), the mnemonic
// escapes, `\xHH;`, a line continuation (R6RS/R7RS), and -- for
// everything else -- a backslash and the character after it.
const ESCAPE = choice(
  '\\"', '\\\\',
  seq('\\', choice(
    /[abtnvfr"\\]/,
    /[xX][0-9a-fA-F]+;/,
    seq(INTRALINE_WHITESPACE, LINE_ENDING, INTRALINE_WHITESPACE),
    seq(repeat(INTRALINE_WHITESPACE), LINE_ENDING, repeat(INTRALINE_WHITESPACE)))),
  /\\./);

// ---- Numbers ----------------------------------------------------------------
//
// <num R> for R = 2, 8, 10, 16 in each report's grammar (R5RS 7.1.1, R6RS
// 4.2.1, R7RS 7.1.1); a number token is any of them. As in the grammar this
// one replaces, a radix other than 10 has no decimal form and its <ureal>
// may be empty, so `#x` and `#b-` alone are number tokens.

const DIGIT = {2: /[01]/, 8: /[0-7]/, 10: /[0-9]/, 16: /[0-9a-fA-F]/};
const SIGN = optional(/[+-]/);

// <prefix R>: radix and exactness, in either order.
function prefix(radix, exactness) {
  return choice(seq(radix, exactness), seq(exactness, radix));
}

function radixOf(r, caseInsensitiveRegex) {
  const letter = {2: 'b', 8: 'o', 10: 'd', 16: 'x'}[r];
  if (caseInsensitiveRegex) {
    const re = new RegExp(`#[${letter}${letter.toUpperCase()}]`);
    return r === 10 ? optional(re) : re;
  }
  const spelled = choice(`#${letter}`, `#${letter.toUpperCase()}`);
  return r === 10 ? optional(spelled) : spelled;
}

// R5RS: `#` digit placeholders (`12#.#`), exponent markers e s f d l.
function r5rsNumber(r) {
  const digit = DIGIT[r];
  const suffix = optional(seq(/[eEsSfFdDlL]/, SIGN, repeat1(DIGIT[10])));
  const uinteger = seq(repeat1(digit), repeat('#'));
  const decimal = r !== 10 ? '' : choice(
    seq(uinteger, suffix),
    seq('.', repeat1(digit), repeat('#'), suffix),
    seq(repeat1(digit), '.', repeat(digit), repeat('#'), suffix),
    seq(repeat1(digit), repeat1('#'), '.', repeat('#'), suffix));
  const ureal = choice(uinteger, seq(uinteger, '/', uinteger), decimal);
  const real = seq(SIGN, ureal);
  const complex = choice(
    real,
    seq(real, '@', real),
    seq(optional(real), /[+-]/, optional(ureal), 'i'));
  return seq(prefix(radixOf(r, false), optional(choice('#i', '#e', '#I', '#E'))), complex);
}

// R6RS: mantissa widths (`1.5|53`), lower-case `+nan.0`/`+inf.0`.
function r6rsNumber(r) {
  const digit = DIGIT[r];
  const suffix = optional(seq(/[eEsSfFdDlL]/, SIGN, repeat1(DIGIT[10])));
  const uinteger = repeat1(digit);
  const decimal = r !== 10 ? '' : choice(
    seq(uinteger, suffix),
    seq('.', repeat1(digit), suffix),
    seq(repeat1(digit), '.', repeat(digit), suffix),
    seq(repeat1(digit), '.', suffix));
  const mantissaWidth = optional(seq('|', repeat1(DIGIT[10])));
  const naninf = choice('nan.0', 'inf.0');
  const ureal = seq(choice(uinteger, seq(uinteger, '/', uinteger), seq(decimal, mantissaWidth)));
  const real = choice(seq(SIGN, ureal), seq(/[+-]/, naninf));
  const complex = choice(
    real,
    seq(real, '@', real),
    seq(optional(real), /[+-]/, optional(choice(ureal, naninf)), 'i'));
  return seq(prefix(radixOf(r, false), optional(choice('#i', '#e', '#I', '#E'))), complex);
}

// R7RS: case-insensitive `+inf.0`/`+nan.0`, exponent marker `e` only.
function r7rsNumber(r) {
  const digit = DIGIT[r];
  const infnan = choice(/[+-][iI][nN][fF]\.0/, /[+-][nN][aA][nN]\.0/);
  const suffix = optional(seq(/[eE]/, SIGN, repeat1(digit)));
  const uinteger = repeat1(digit);
  const decimal = r !== 10 ? '' : choice(
    seq(uinteger, suffix),
    seq('.', repeat1(digit), suffix),
    seq(repeat1(digit), '.', repeat(digit), suffix));
  const ureal = choice(uinteger, seq(uinteger, '/', uinteger), decimal);
  const real = choice(seq(SIGN, ureal), infnan);
  const complex = choice(
    real,
    seq(real, '@', real),
    seq(real, /[+-]/, ureal, 'i'),
    seq(real, /[+-]/, 'i'),
    seq(real, infnan, 'i'),
    seq(/[+-]/, ureal, 'i'),
    seq(infnan, 'i'),
    seq(/[+-]/, 'i'));
  return seq(prefix(radixOf(r, true), optional(/#[ieIE]/)), complex);
}

const NUMBER = choice(
  ...[2, 8, 10, 16].map(r5rsNumber),
  ...[2, 8, 10, 16].map(r6rsNumber),
  ...[2, 8, 10, 16].map(r7rsNumber));

// A prefix and the datum it applies to, comments allowed between them.
function prefixed($, opener) {
  return seq(opener, repeat($._intertoken), $._datum);
}

module.exports = grammar({
  name: 'scheme',

  // Whitespace and comments are children, not extras: `#;` and the quote
  // prefixes own what follows them, comments between included.
  extras: _ => [],

  rules: {
    program: $ => repeat($._token),

    _token: $ => choice($._intertoken, $._datum),

    _intertoken: $ => choice(
      token(repeat1(WHITESPACE)),
      $.directive,
      $.comment,
      $.block_comment),

    // `; ...` to the end of the line, or `#;` and the datum it comments out.
    comment: $ => choice(/;.*/, prefixed($, '#;')),

    // `#!r6rs`, `#!fold-case`, `#!eof`...
    directive: $ => seq('#!', repeat($._intertoken), DIRECTIVE_NAME),

    // `#| ... |#`, nesting; and Guile's script header, `#!/usr/bin/guile
    // -s` up to the `!#` that closes it (with no `!#` the line reads as a
    // directive, as an R6RS `#!/usr/bin/env scheme-script` line should).
    block_comment: $ => choice(
      seq(
        '#|',
        repeat(choice(prec(100, $.block_comment), ANY_CHAR)),
        prec(100, '|#')),
      token(seq('#!/', /([^!]|!+[^!#])*/, /!+#/))),

    _datum: $ => choice(
      $.boolean,
      $.character,
      $.string,
      // A token that reads as a number and as a symbol of the same length
      // is a number (`+i`, `-1`); a longer symbol wins (`1+`, `-1-`).
      $.number,
      $.symbol,
      $.vector,
      $.byte_vector,
      $.list,
      $.quote,
      $.quasiquote,
      $.unquote,
      $.unquote_splicing,
      $.syntax,
      $.quasisyntax,
      $.unsyntax,
      $.unsyntax_splicing,
      $.keyword,
      $.gexp,
      $.ungexp,
      $.ungexp_splicing),

    boolean: _ => token(BOOLEAN),

    // Declared before `symbol`: on a tie the earlier token wins.
    number: _ => token(NUMBER),

    character: _ => token(CHARACTER),

    string: $ => seq('"', repeat(choice($.escape_sequence, /[^"\\]+/)), '"'),

    escape_sequence: _ => token(ESCAPE),

    symbol: _ => token(DIRECTIVE_NAME),

    // `#:name`: Guile's, Chicken's and Racket's keywords.
    keyword: _ => token(seq('#:', SYMBOL)),

    // (...), [...] (R6RS) and {...} (curly-infix, SRFI 105).
    list: $ => choice(
      seq('(', repeat($._token), ')'),
      seq('[', repeat($._token), ']'),
      seq('{', repeat($._token), '}')),

    quote: $ => prefixed($, "'"),
    quasiquote: $ => prefixed($, '`'),
    syntax: $ => prefixed($, "#'"),
    quasisyntax: $ => prefixed($, '#`'),
    unquote: $ => prefixed($, ','),
    unquote_splicing: $ => prefixed($, ',@'),
    unsyntax: $ => prefixed($, '#,'),
    unsyntax_splicing: $ => prefixed($, '#,@'),

    vector: $ => seq('#(', repeat($._token), ')'),

    // R6RS's `#vu8(...)` (R7RS's `#u8(`: see README.md, "Known limits").
    byte_vector: $ => choice(
      seq('#vu8(', repeat($._token), ')'),
      seq('#u8(', repeat($._token), ')')),

    // Guix's G-expressions (the reader extensions `(guix gexp)` installs):
    // `#~exp`, `#$x` / `#+x` (native), `#$@xs` / `#+@xs`. Declared last so
    // the tokens above keep their places in the rule order.
    gexp: $ => prefixed($, '#~'),
    ungexp: $ => prefixed($, choice('#$', '#+')),
    ungexp_splicing: $ => prefixed($, choice('#$@', '#+@')),
  },
});
