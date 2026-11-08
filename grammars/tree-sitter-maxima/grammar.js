/**
 * @file Maxima, mep's own grammar
 * @license MIT
 *
 * A drop-in for the node names of stewsat/tree-sitter-maxima (the grammar
 * mep loaded before; it has no license, so nothing of it is copied here),
 * written from Maxima's own reader (src/nparse.lisp): the operator binding
 * powers are Maxima's, read out of a live image, as in src/maxima_ast.cpp.
 *
 * Maxima's reader is a Pratt parser: an operator has a left binding power
 * (lbp: how hard it pulls the expression on its left) and a right one (rbp:
 * the power its right operand is read at). Tree-sitter resolves a
 * shift/reduce conflict by comparing the precedence of the item being
 * reduced -- the precedence of its *last* step -- with that of the item
 * that would shift -- the precedence of the step *before* the lookahead.
 * So an infix rule puts lbp on its left operand and rbp (left-associative,
 * since Pratt stops on a tie) on its right one, and every decision is the
 * one Maxima makes: `-x^2` is `-(x^2)`, `a+b:c+d` is `a+(b:(c+d))`.
 *
 * A `prec()` around a sub-rule ends with the sub-rule: its last step gets
 * the enclosing precedence back unless the sub-rule ends the production.
 * So the left operand's lbp is written around the operand *and* the
 * operator, `prec(lbp, seq(operand, op))`.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// [operator, lbp, rbp] -- src/maxima_ast.cpp's kOperators.
const INFIX = [
  ['::=', 180, 20],
  [':=', 180, 20],
  ['::', 180, 20],
  [':', 180, 20],
  ['^^', 140, 139],
  ['**', 140, 139],
  ['^', 140, 139],
  ['.', 130, 129],
  ['*', 120, 120],
  ['/', 120, 120],
  ['+', 100, 100],
  ['-', 100, 100],
  ['->', 80, 80],
  ['<=', 80, 80],
  ['>=', 80, 80],
  ['#', 80, 80],
  ['<', 80, 80],
  ['>', 80, 80],
  ['=', 80, 80],
  ['and', 65, 65],
  ['or', 60, 60],
  ['@', 200, 201],
];

// The binding powers of the forms that are not operators.
const BP = {
  call: 200, // `(` and `[` after a value: a call or a subscript
  postfix: 160, // `!`, `!!`
  sign: 134, // prefix `+`, `-`
  quote: 190, // `'`, `''`
  not: 70,
  branch: 25, // what `then`, `else` and `do` read
  comma: 10, // the statement-level `,` (`ev` shorthand)
  user: 180, // an operator the file declares without binding powers
};

module.exports = grammar({
  name: 'maxima',

  externals: $ => [$.comment],

  // Whitespace includes form feed and vertical tab, and a backslash at the
  // end of a line joins it to the next (Maxima deletes both in its
  // character reader).
  extras: $ => [$.comment, /[\s\f\v]+/, /\\\r?\n/],

  word: $ => $.identifier,

  rules: {
    source_file: $ => repeat(choice($.statement, $.lisp_escape)),

    statement: $ => seq(
      choice($._expression, alias($._comma_expression, $.binary_expression), alias($._label, $.binary_expression)),
      choice(';', '$'),
    ),

    // `:lisp <form>`: the rest of the line is Lisp.
    lisp_escape: _ => token(seq(':lisp', optional('_quiet'), /[^\n]*/)),

    // `integrate(x, x), x = 2` -- Maxima's `ev` shorthand, only at statement level.
    _comma_expression: $ => seq(
      prec(BP.comma, seq(choice($._expression, alias($._comma_expression, $.binary_expression)), ',')),
      prec.left(BP.comma, $._expression),
    ),

    // `name && expr` tags a statement with a name.
    _label: $ => seq($.identifier, '&&', $._expression),

    _expression: $ => choice(
      $.atom,
      $.list,
      $.set,
      $.function_call,
      $.array_ref,
      $.parenthesized_expression,
      $.prefix_expression,
      $.postfix_expression,
      $.binary_expression,
      $.if_expression,
      $.block_expression,
      $.lambda_expression,
      $.for_expression,
      $.for_in_expression,
      $.while_expression,
      $.unless_expression,
    ),

    // `block` and `lambda` are ordinary names when no `(` follows:
    // `integrate(f, lambda, 0, a)`, `'lambda`.
    atom: $ => choice(
      $.number,
      $.string,
      $.identifier,
      alias(choice('block', 'lambda'), $.identifier),
    ),

    // 1, 1.5, .5, 1., 1.5e3, 1.5b0 (a bigfloat), 1d0, 1f0, 1s0, 1l0.
    number: _ => token(seq(
      choice(/\d+(\.\d*)?/, /\.\d+/),
      optional(/[eEbBdDsSlLfF][+-]?\d+/),
    )),

    // No \n escapes: a backslash takes the next character, whatever it is.
    string: _ => token(seq('"', repeat(choice(/[^"\\]/, /\\(.|\r?\n)/)), '"')),

    // Letters, digits, `_`, `%`, any non-ASCII character, and `\`-escaped
    // characters (`\-x`, `a\ b`). `?name`, `?:name` and `?"..."` are symbols
    // of the host Lisp: one character after the `?` is taken whatever it is.
    identifier: _ => token(choice(
      seq(
        choice(/[A-Za-z_%\u0080-\uffff]/, /\\[^\r\n]/),
        repeat(choice(/[A-Za-z0-9_%\u0080-\uffff]/, /\\[^\r\n]/)),
      ),
      seq('?', optional(':'), choice(/[^\r\n"\\]/, /\\[^\r\n]/), repeat(choice(/[A-Za-z0-9_%\-\u0080-\uffff]/, /\\[^\r\n]/))),
      seq('?', /"([^"\\]|\\.)*"/),
    )),

    list: $ => seq('[', optional(commaSep($._expression)), ']'),

    set: $ => seq('{', optional(commaSep($._expression)), '}'),

    function_call: $ => seq(
      prec(BP.call, seq($._expression, '(')),
      optional(commaSep($._expression)),
      ')',
    ),

    array_ref: $ => seq(
      prec(BP.call, seq($._expression, '[')),
      optional(commaSep($._expression)),
      ']',
    ),

    // `(a, b, c)` evaluates each and returns the last.
    parenthesized_expression: $ => seq('(', commaSep($._expression), ')'),

    prefix_expression: $ => choice(
      seq(choice('-', '+'), prec.left(BP.sign, $._expression)),
      seq(choice("'", "''"), prec.left(BP.quote, $._expression)),
      seq('not', prec.left(BP.not, $._expression)),
    ),

    postfix_expression: $ => prec(BP.postfix, seq($._expression, choice('!', '!!'))),

    binary_expression: $ => choice(
      ...INFIX.map(([op, lbp, rbp]) =>
        seq(prec(lbp, seq($._expression, op)), prec.left(rbp, $._expression)),
      ),
      seq(prec(BP.user, seq($._expression, $._user_operator)), prec.left(BP.user, $._expression)),
    ),

    // An operator a file declares for itself (`infix("~")`, ezunits' `` ` ``):
    // a context-free grammar cannot learn the declaration, so the punctuation
    // spellings Maxima's own packages declare are read as infix at the power
    // a declaration without numbers gets. (Word operators, `a cross b`,
    // stay errors: nothing marks them as operators.)
    _user_operator: _ => token(choice('=>', '<=>', '==>', '-->', '~', '&', '|', '@@', '@@@', '<<', '>>', '`', '``')),

    // A branch is read at power 25, so `if a then x : 1 else y : 2` keeps
    // both assignments inside, and the else of a nested `if` is the inner
    // one's (prec.right: on the tie, shift).
    if_expression: $ => seq(
      'if',
      $._expression,
      'then',
      prec.right(BP.branch, seq(
        $._expression,
        repeat($.elseif_clause),
        optional(seq('else', $._expression)),
      )),
    ),

    elseif_clause: $ => seq('elseif', $._expression, 'then', prec.right(BP.branch, $._expression)),

    // block and lambda are ordinary functions to Maxima; the node marks them.
    block_expression: $ => seq(prec(1, seq('block', '(')), optional(commaSep($._expression)), ')'),

    lambda_expression: $ => seq(prec(1, seq('lambda', '(')), optional(commaSep($._expression)), ')'),

    // Maxima's one loop form, `for v : a step s next n thru t while c unless d do body`,
    // takes its clauses in any order and every one optional. The node says
    // which kind: `for v in list` is for_in_expression, a loop opening with
    // `while` / `unless` is while_expression / unless_expression, and every
    // other loop (`for`, `thru 10 do`, a bare `do`) is for_expression.
    for_expression: $ => choice(
      seq('for', $.identifier, optional(seq(':', $._expression)), repeat($._loop_clause), $._loop_body),
      seq(choice(...['from', 'step', 'next', 'thru'].map(k => seq(k, $._expression))),
        repeat($._loop_clause), $._loop_body),
      $._loop_body,
    ),

    for_in_expression: $ => seq('for', $.identifier, 'in', $._expression, repeat($._loop_clause), $._loop_body),

    while_expression: $ => seq('while', $._expression, repeat($._loop_clause), $._loop_body),

    unless_expression: $ => seq('unless', $._expression, repeat($._loop_clause), $._loop_body),

    _loop_clause: $ => choice(...['from', 'step', 'next', 'thru', 'while', 'unless'].map(k => seq(k, $._expression))),

    _loop_body: $ => seq('do', prec.right(BP.branch, $._expression)),
  },
});

function commaSep(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
