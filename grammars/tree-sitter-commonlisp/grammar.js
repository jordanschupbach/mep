/**
 * @file Tree-sitter grammar for Common Lisp.
 *
 * mep's own grammar, written against the Common Lisp reader as the
 * standard defines it (CLHS chapter 2, "Syntax": the standard syntax
 * types of 2.1.4, the token interpretation of 2.3 and the standard macro
 * characters of 2.4).
 *
 * Common Lisp has no syntax beyond what the reader reads: a `defun` is a
 * list whose first element happens to be the symbol DEFUN. So the tree is
 * the reader's -- lists, atoms and the reader macros' forms -- and what a
 * list *means* (a definition, a binding form, a call) is left to the
 * queries, which match on the operator's name. That is also what keeps
 * the grammar honest about user macros: a `define-foo` reads exactly like
 * a `defun`.
 *
 * The one thing a regular lexer cannot do is the nestable block comment
 * `#| ... #| ... |# ... |#`, which the external scanner (src/scanner.c)
 * reads, along with the text of strings.
 */

// A constituent character, or an escaped one: `\x` escapes one character
// and `|...|` a run of them (CLHS 2.1.4.6, 2.1.4.7). The terminating macro
// characters ( ) ' " ` , ; and whitespace end a token. `:` is a package
// marker, so a symbol's own characters never include it; `#` is a
// non-terminating macro character, part of a token anywhere but its start.
const CONSTITUENT = /[^\s()'"`,;|\\:]/;
const ESCAPE = /\\[^\n]|\|[^|]*\|/;
const START = /[^\s()'"`,;|\\:#]/;

const symbolBody = () => seq(
  choice(START, ESCAPE),
  repeat(choice(CONSTITUENT, ESCAPE)),
);

// Potential numbers (CLHS 2.3.1) that the standard reads as numbers:
// integers (with an optional trailing decimal point), ratios, and floats
// with any of the exponent markers e s f d l.
const DECIMAL_NUMBER = /[+-]?(\d+\.?|\d+\/\d+|\d*\.\d+([eEsSfFdDlL][+-]?\d+)?|\d+(\.\d*)?[eEsSfFdDlL][+-]?\d+)/;
// #b #o #x and #nR rationals (CLHS 2.4.8.7-2.4.8.10).
const RADIX_NUMBER = /#([bB][+-]?[01]+(\/[01]+)?|[oO][+-]?[0-7]+(\/[0-7]+)?|[xX][+-]?[0-9a-fA-F]+(\/[0-9a-fA-F]+)?|\d+[rR][+-]?[0-9a-zA-Z]+(\/[0-9a-zA-Z]+)?)/;

module.exports = grammar({
  name: 'commonlisp',

  extras: $ => [/\s/, $.comment, $.block_comment],

  // The scanner also reads a string's text, so that `#|` inside a string
  // is never taken for a comment (block_comment, an extra, is valid
  // everywhere -- the string's own token being valid is how the scanner
  // knows it is inside one).
  externals: $ => [
    $.block_comment,
    $._string_content,
    // Never produced: valid only during error recovery, which the scanner
    // uses to tell that apart from a real parse state.
    $._error_sentinel,
  ],

  rules: {
    source: $ => seq(optional($.shebang), repeat($._form)),

    // `#!/usr/bin/env sbcl --script`: not Lisp, but SBCL, CLISP and ECL
    // all skip it on a script's first line.
    shebang: _ => token(prec(1, /#![^\n]*/)),

    _form: $ => choice(
      $.list,
      $.vector,
      $.quote,
      $.quasiquote,
      $.unquote,
      $.unquote_splicing,
      $.function,
      $.read_eval,
      $.feature_expression,
      $.label,
      $.label_reference,
      $.array,
      $.structure,
      $.complex,
      $.pathname,
      $.bit_vector,
      // number before the symbols: `12` matches both, and the rule that
      // comes first wins a tie of equal length; `1+` is longer as a symbol.
      $.number,
      $.string,
      $.character,
      $.keyword,
      $.uninterned_symbol,
      $.qualified_symbol,
      $.package_form,
      $.symbol,
    ),

    list: $ => seq('(', repeat(choice($._form, $.dot)), ')'),

    // The consing dot of `(a . b)`. A lone `.` is not a symbol (CLHS
    // 2.3.3), and as a string token it wins over the symbol pattern.
    dot: _ => '.',

    // #( ... ), with an optional length: #3(a b c).
    vector: $ => seq(/#\d*\(/, repeat($._form), ')'),

    quote: $ => seq('\'', field('value', $._form)),
    quasiquote: $ => seq('`', field('value', $._form)),
    unquote: $ => seq(',', field('value', $._form)),
    // ,@ and its destructive cousin ,.
    unquote_splicing: $ => seq(choice(',@', ',.'), field('value', $._form)),

    // #'f and #'(lambda ...)
    function: $ => seq('#\'', field('value', $._form)),

    // #.form: evaluated at read time.
    read_eval: $ => seq('#.', field('value', $._form)),

    // #+sbcl form, #-(or ccl ecl) form.
    feature_expression: $ => seq(
      field('operator', choice('#+', '#-')),
      field('feature', $._form),
      field('value', $._form),
    ),

    // #1=(a . #1#)
    label: $ => seq(/#\d+=/, field('value', $._form)),
    label_reference: _ => /#\d+#/,

    // #2A((1 2) (3 4))
    array: $ => seq(/#\d*[aA]/, field('value', $._form)),
    // #S(point :x 1 :y 2)
    structure: $ => seq(/#[sS]/, field('value', $.list)),
    // #C(1 2)
    complex: $ => seq(/#[cC]/, field('value', $.list)),
    // #P"/tmp/x.lisp"
    pathname: $ => seq(/#[pP]/, field('value', $.string)),
    // #*1011
    bit_vector: _ => /#\*[01]*/,

    number: _ => token(choice(DECIMAL_NUMBER, RADIX_NUMBER)),

    string: $ => seq(
      '"',
      repeat(choice(
        $._string_content,
        $.escape_sequence,
        $.format_directive,
      )),
      token.immediate('"'),
    ),
    escape_sequence: _ => token.immediate(/\\(.|\n)/),
    // A FORMAT directive (CLHS 22.3): prefix parameters (digits, 'c, v, #
    // separated by commas), the : and @ modifiers, then the directive
    // character. Any string can hold one; only a FORMAT control string
    // gives it meaning, which a reader cannot know -- but a `~` outside
    // one is rare enough that marking it everywhere is the useful choice.
    format_directive: _ => token.immediate(/~([0-9vV#,]|'.)*[:@]*[^"\\]/),

    // #\a, #\Space, #\( -- a name is a run of constituents after the
    // first character (CLHS 2.4.8.1).
    character: _ => token(seq('#\\', choice(/[^\n]/, /[a-zA-Z][a-zA-Z0-9_+-]+/))),

    // :key, and ::key, which reads the same.
    keyword: _ => token(seq(/::?/, repeat1(choice(CONSTITUENT, ESCAPE)))),

    // #:gensym-like
    uninterned_symbol: _ => token(seq('#:', repeat1(choice(CONSTITUENT, ESCAPE)))),

    // package:symbol and package::symbol. The package and its marker are
    // one token, so `foo:bar` is never read as `foo` then the keyword
    // `:bar` -- the longer match wins.
    qualified_symbol: $ => seq(
      field('package', $._package),
      field('name', alias(token.immediate(repeat1(choice(CONSTITUENT, ESCAPE))), $.symbol)),
    ),

    // sb-impl::(a b c): SBCL's extended package prefix, which reads the
    // form with *PACKAGE* bound to that package. SBCL's own sources use it
    // throughout; no other reader does, so it only ever appears there.
    package_form: $ => seq(field('package', $._package), field('value', $.list)),

    _package: $ => alias(token(seq(symbolBody(), /::?/)), $.package),

    symbol: _ => token(symbolBody()),

    comment: _ => token(seq(';', /[^\n]*/)),
  },
});
