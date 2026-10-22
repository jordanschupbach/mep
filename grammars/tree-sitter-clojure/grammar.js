/**
 * @file Clojure, mep's own grammar.
 *
 * Follows the Clojure reader (clojure.lang.LispReader, and the reader
 * reference at clojure.org/reference/reader): the tree is what the reader
 * reads -- atoms, collections and the reader macros' forms. There is no
 * `defn` node; what a list means is decided by the queries, from its first
 * symbol, which keeps user macros on an equal footing with core's.
 *
 * Node names, fields and rule order are the ones sogaiu/tree-sitter-clojure
 * established (the grammar mep loaded dynamically before this one), so
 * queries written for it keep working.
 *
 * Whitespace is not an extra: it is a child (`_gap`) wherever the reader
 * allows it. That is what lets metadata (`^:private foo`) and the other
 * prefixes own the form they prefix, comments and `#_` included.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const re = (...parts) => new RegExp(parts.join(''));

// The characters java.lang.Character.isWhitespace accepts, plus the comma
// (the reader treats `,` as whitespace). U+00A0, U+2007 and U+202F are
// "non-breaking" and so not whitespace in Java.
const UNICODE_SPACES =
  '\\u000B\\u001C\\u001D\\u001E\\u001F' +
  '\\u2028\\u2029\\u1680' +
  '\\u2000\\u2001\\u2002\\u2003\\u2004\\u2005\\u2006\\u2008' +
  '\\u2009\\u200a\\u205f\\u3000';

const WHITESPACE_CHAR = re('[', '\\f\\n\\r\\t, ', UNICODE_SPACES, ']');

const WHITESPACE = token(repeat1(WHITESPACE_CHAR));

// `;` to the end of the line; `#!` too (a shebang, and anywhere else).
const COMMENT = token(re('(;|#!).*\n?'));

// ---- numbers (LispReader's intPat, ratioPat, floatPat) ----------------------

const DIGIT = re('[0-9]');
const ALPHANUMERIC = re('[0-9a-zA-Z]');
const HEX_DIGIT = re('[0-9a-fA-F]');
const OCTAL_DIGIT = re('[0-7]');

const HEX_NUMBER = seq('0', re('[xX]'), repeat1(HEX_DIGIT), optional('N'));

const OCTAL_NUMBER = seq('0', repeat1(OCTAL_DIGIT), optional('N'));

// 2r1010, 36rZZ: the radix and digits are left to the reader to check.
const RADIX_NUMBER = seq(repeat1(DIGIT), re('[rR]'), repeat1(ALPHANUMERIC));

const RATIO = seq(repeat1(DIGIT), '/', repeat1(DIGIT));

const DOUBLE = seq(
  repeat1(DIGIT),
  optional(seq('.', repeat(DIGIT))),
  optional(seq(re('[eE]'), optional(re('[+-]')), repeat1(DIGIT))),
  optional('M'),
);

const INTEGER = seq(repeat1(DIGIT), optional(re('[MN]')));

const NUMBER = token(prec(10, seq(
  optional(re('[+-]')),
  choice(HEX_NUMBER, OCTAL_NUMBER, RADIX_NUMBER, RATIO, DOUBLE, INTEGER),
)));

const NIL = token('nil');

const BOOLEAN = token(choice('false', 'true'));

// ---- keywords --------------------------------------------------------------

// What may follow the `:` of a keyword: anything that does not end a
// token, and not `:` or `/` themselves (they are handled by the rules).
const KEYWORD_HEAD = re(
  '[^',
  '\\f\\n\\r\\t ',
  '()', '\\[\\]', '{}', '"',
  '@~^;`', '\\\\', ',:/',
  UNICODE_SPACES,
  ']',
);

const KEYWORD_BODY = choice(re('[:\']'), KEYWORD_HEAD);

const KEYWORD_NAMESPACED_BODY = token(repeat1(choice(re('[:\'/]'), KEYWORD_HEAD)));

const KEYWORD_NO_SIGIL = token(seq(KEYWORD_HEAD, repeat(KEYWORD_BODY)));

const KEYWORD_MARK = token(':');

// `::kw` resolves in the current namespace, `::alias/kw` in an alias's.
const AUTO_RESOLVE_MARK = token('::');

// ---- strings and characters ------------------------------------------------

const STRING = token(seq(
  '"',
  repeat(re('[^"\\\\]')),
  repeat(seq('\\', re('.'), repeat(re('[^"\\\\]')))),
  '"',
));

const OCTAL_CHAR = seq('o', choice(seq(DIGIT, DIGIT, DIGIT), seq(DIGIT, DIGIT), DIGIT));

const NAMED_CHAR = choice('backspace', 'formfeed', 'newline', 'return', 'space', 'tab');

const UNICODE = seq('u', HEX_DIGIT, HEX_DIGIT, HEX_DIGIT, HEX_DIGIT);

// Any single character after the backslash, a newline included.
const ANY_CHAR = re('.|\n');

const CHARACTER = token(seq('\\', choice(OCTAL_CHAR, NAMED_CHAR, UNICODE, ANY_CHAR)));

// ---- symbols ---------------------------------------------------------------

// A symbol may not start with a digit (that is a number), `:` (a keyword),
// `#` or `'` (reader macros), or `/` (bare `/` is division, below).
const SYMBOL_HEAD = re(
  '[^',
  '\\f\\n\\r\\t ',
  '/',
  '()\\[\\]{}', '"',
  '@~^;`', '\\\\',
  ',:#\'0-9',
  UNICODE_SPACES,
  ']',
);

const NS_DELIMITER = token('/');

const SYMBOL_BODY = choice(SYMBOL_HEAD, re('[:#\'0-9]'));

// After `ns/`, the name may itself contain `/` (`clojure.core//`).
const SYMBOL_NAMESPACED_NAME = token(repeat1(choice(SYMBOL_HEAD, re('[/:#\'0-9]'))));

const SYMBOL = token(seq(SYMBOL_HEAD, repeat(SYMBOL_BODY)));

module.exports = grammar({
  name: 'clojure',

  extras: _ => [],

  conflicts: _ => [],

  inline: $ => [
    $._kwd_leading_slash,
    $._kwd_just_slash,
    $._kwd_qualified,
    $._kwd_unqualified,
    $._kwd_marker,
    $._sym_qualified,
    $._sym_unqualified,
  ],

  rules: {
    source: $ => repeat(choice($._form, $._gap)),

    // What may come between forms: whitespace, comments, and `#_` with
    // the form it discards.
    _gap: $ => choice($._ws, $.comment, $.dis_expr),

    _ws: _ => WHITESPACE,

    comment: _ => COMMENT,

    dis_expr: $ => seq(
      field('marker', '#_'),
      repeat($._gap),
      field('value', $._form),
    ),

    _form: $ => choice(
      // atoms
      $.num_lit,
      $.kwd_lit,
      $.str_lit,
      $.char_lit,
      $.nil_lit,
      $.bool_lit,
      $.sym_lit,
      // collections
      $.list_lit,
      $.map_lit,
      $.vec_lit,
      // the dispatch macros, `#...`
      $.set_lit,
      $.anon_fn_lit,
      $.regex_lit,
      $.read_cond_lit,
      $.splicing_read_cond_lit,
      $.ns_map_lit,
      $.var_quoting_lit,
      $.sym_val_lit,
      $.evaling_lit,
      $.tagged_or_ctor_lit,
      // the other reader macros
      $.derefing_lit,
      $.quoting_lit,
      $.syn_quoting_lit,
      $.unquote_splicing_lit,
      $.unquoting_lit,
    ),

    num_lit: _ => NUMBER,

    kwd_lit: $ => choice(
      $._kwd_leading_slash,
      $._kwd_just_slash,
      $._kwd_qualified,
      $._kwd_unqualified,
    ),

    // :/usr/bin/env -- no namespace, the name is everything after the slash
    _kwd_leading_slash: $ => seq(
      field('marker', $._kwd_marker),
      field('delimiter', NS_DELIMITER),
      field('name', alias(KEYWORD_NAMESPACED_BODY, $.kwd_name)),
    ),

    // :/ -- the name is "/"
    _kwd_just_slash: $ => seq(
      field('marker', $._kwd_marker),
      field('name', alias(NS_DELIMITER, $.kwd_name)),
    ),

    _kwd_qualified: $ => prec(2, seq(
      field('marker', $._kwd_marker),
      field('namespace', alias(KEYWORD_NO_SIGIL, $.kwd_ns)),
      field('delimiter', NS_DELIMITER),
      field('name', alias(KEYWORD_NAMESPACED_BODY, $.kwd_name)),
    )),

    _kwd_unqualified: $ => prec(1, seq(
      field('marker', $._kwd_marker),
      field('name', alias(KEYWORD_NO_SIGIL, $.kwd_name)),
    )),

    _kwd_marker: _ => choice(KEYWORD_MARK, AUTO_RESOLVE_MARK),

    str_lit: _ => STRING,

    char_lit: _ => CHARACTER,

    nil_lit: _ => NIL,

    bool_lit: _ => BOOLEAN,

    sym_lit: $ => seq(
      repeat($._metadata_lit),
      choice($._sym_qualified, $._sym_unqualified),
    ),

    _sym_qualified: $ => prec(1, seq(
      field('namespace', alias(SYMBOL, $.sym_ns)),
      field('delimiter', NS_DELIMITER),
      field('name', alias(SYMBOL_NAMESPACED_NAME, $.sym_name)),
    )),

    // A bare `/` is the division symbol.
    _sym_unqualified: $ => field('name', alias(choice(NS_DELIMITER, SYMBOL), $.sym_name)),

    // Metadata belongs to the form it precedes, so it is a child of that
    // form: `^:private ^String x` is one sym_lit with two meta_lit fields.
    _metadata_lit: $ => seq(
      choice(field('meta', $.meta_lit), field('old_meta', $.old_meta_lit)),
      optional(repeat($._gap)),
    ),

    meta_lit: $ => seq(
      field('marker', '^'),
      repeat($._gap),
      field('value', $._form),
    ),

    // `#^`, the pre-1.2 spelling of `^`.
    old_meta_lit: $ => seq(
      field('marker', '#^'),
      repeat($._gap),
      field('value', $._form),
    ),

    list_lit: $ => seq(repeat($._metadata_lit), $._bare_list_lit),

    _bare_list_lit: $ => seq(
      field('open', '('),
      repeat(choice(field('value', $._form), $._gap)),
      field('close', ')'),
    ),

    map_lit: $ => seq(repeat($._metadata_lit), $._bare_map_lit),

    _bare_map_lit: $ => seq(
      field('open', '{'),
      repeat(choice(field('value', $._form), $._gap)),
      field('close', '}'),
    ),

    vec_lit: $ => seq(repeat($._metadata_lit), $._bare_vec_lit),

    _bare_vec_lit: $ => seq(
      field('open', '['),
      repeat(choice(field('value', $._form), $._gap)),
      field('close', ']'),
    ),

    set_lit: $ => seq(repeat($._metadata_lit), $._bare_set_lit),

    _bare_set_lit: $ => seq(
      field('marker', '#'),
      field('open', '{'),
      repeat(choice(field('value', $._form), $._gap)),
      field('close', '}'),
    ),

    // #(* % %)
    anon_fn_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#'),
      $._bare_list_lit,
    ),

    // #"\d+" -- the pattern's text is java.util.regex's, read raw.
    regex_lit: _ => seq(field('marker', '#'), STRING),

    // #?(:clj a :cljs b). Whitespace may follow the marker, but neither a
    // comment nor `#_`.
    read_cond_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#?'),
      repeat($._ws),
      $._bare_list_lit,
    ),

    // #?@(:clj [a b]) -- splices into the enclosing collection.
    splicing_read_cond_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#?@'),
      repeat($._ws),
      $._bare_list_lit,
    ),

    auto_res_mark: _ => AUTO_RESOLVE_MARK,

    // #:person{:name "x"} and #::{:a 1}: the map's keys take the namespace.
    ns_map_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#'),
      field('prefix', choice($.auto_res_mark, $.kwd_lit)),
      repeat($._gap),
      $._bare_map_lit,
    ),

    // #'foo, (var foo)
    var_quoting_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#\''),
      repeat($._gap),
      field('value', $._form),
    ),

    // ##Inf, ##-Inf, ##NaN
    sym_val_lit: $ => seq(
      field('marker', '##'),
      repeat($._gap),
      field('value', $._form),
    ),

    // #=(+ 1 2): evaluated at read time when *read-eval* allows it.
    evaling_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#='),
      repeat($._gap),
      field('value', choice($.list_lit, $.read_cond_lit, $.sym_lit)),
    ),

    // #inst "2020-01-01", #uuid "...", and record constructors:
    // #user.Point[1 2], #user.Point{:x 1 :y 2}.
    tagged_or_ctor_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '#'),
      repeat($._gap),
      field('tag', $.sym_lit),
      repeat($._gap),
      field('value', $._form),
    ),

    // @x, (deref x)
    derefing_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '@'),
      repeat($._gap),
      field('value', $._form),
    ),

    // 'x, (quote x)
    quoting_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '\''),
      repeat($._gap),
      field('value', $._form),
    ),

    // `x: namespace-qualifies its symbols, and ~/~@ inside unquote.
    syn_quoting_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '`'),
      repeat($._gap),
      field('value', $._form),
    ),

    unquote_splicing_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '~@'),
      repeat($._gap),
      field('value', $._form),
    ),

    unquoting_lit: $ => seq(
      repeat($._metadata_lit),
      field('marker', '~'),
      repeat($._gap),
      field('value', $._form),
    ),
  },
});
