/**
 * @file Tree-sitter grammar for the Nix expression language.
 *
 * mep's own grammar, written against Nix's reference lexer and parser
 * (src/libexpr/lexer.l, parser.y) rather than ported from another one. The
 * node names follow the established tree-sitter-nix ones, so queries written
 * for that grammar mostly carry over.
 *
 * Operator precedence is encoded by layering, the way parser.y does it:
 *
 *   _expression    functions, assert, with, let, if    (never an operand)
 *   _expr_op       unary/binary operators and `?`
 *   _expr_apply    function application                (left associative)
 *   _expr_select   `a.b.c` and `a.b or default`        (list elements)
 *   _expr_simple   literals, variables, (), {}, []
 *
 * Paths and string contents are context-sensitive, so they come from the
 * hand-written external scanner in src/scanner.c.
 */

const PREC = {
  pipe: 1,
  impl: 2,
  or: 3,
  and: 4,
  eq: 5,
  compare: 6,
  update: 7,
  not: 8,
  add: 9,
  mul: 10,
  concat: 11,
  has_attr: 12,
  negate: 13,
};

module.exports = grammar({
  name: 'nix',

  extras: $ => [/\s/, $.comment],

  externals: $ => [
    $.string_fragment,
    $.indented_string_fragment,
    $._path_start,
    $._hpath_start,
    $.path_fragment,
    // Never produced: valid only during error recovery, which the scanner
    // uses to tell that apart from a real parse state.
    $._error_sentinel,
  ],

  word: $ => $.identifier,

  supertypes: $ => [$._expression],

  rules: {
    source_code: $ => optional(field('expression', $._expression)),

    _expression: $ => choice(
      $.function_expression,
      $.assert_expression,
      $.with_expression,
      $.let_expression,
      $.if_expression,
      $._expr_op,
    ),

    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_'\-]*/,

    // ---- functions and the other expression-level forms ----------------

    function_expression: $ => choice(
      seq(field('universal', $.identifier), ':', field('body', $._expression)),
      seq(field('formals', $.formals), ':', field('body', $._expression)),
      seq(field('formals', $.formals), '@', field('universal', $.identifier), ':', field('body', $._expression)),
      seq(field('universal', $.identifier), '@', field('formals', $.formals), ':', field('body', $._expression)),
    ),

    // `{ a, b ? 1, ... }`; parser.y allows a trailing comma, and `...` only last.
    formals: $ => seq(
      '{',
      repeat(seq(field('formal', $.formal), ',')),
      optional(choice(field('formal', $.formal), field('ellipses', $.ellipses))),
      '}',
    ),

    formal: $ => seq(field('name', $.identifier), optional(seq('?', field('default', $._expression)))),

    ellipses: _ => '...',

    assert_expression: $ => seq('assert', field('condition', $._expression), ';', field('body', $._expression)),

    with_expression: $ => seq('with', field('environment', $._expression), ';', field('body', $._expression)),

    let_expression: $ => seq('let', optional($.binding_set), 'in', field('body', $._expression)),

    if_expression: $ => seq(
      'if', field('condition', $._expression),
      'then', field('consequence', $._expression),
      'else', field('alternative', $._expression),
    ),

    // ---- operators ------------------------------------------------------

    _expr_op: $ => choice(
      $.has_attr_expression,
      $.unary_expression,
      $.binary_expression,
      $._expr_apply,
    ),

    unary_expression: $ => choice(
      prec(PREC.not, seq(field('operator', '!'), field('argument', $._expr_op))),
      prec(PREC.negate, seq(field('operator', '-'), field('argument', $._expr_op))),
    ),

    has_attr_expression: $ => prec(PREC.has_attr, seq(
      field('expression', $._expr_op),
      field('operator', '?'),
      field('attrpath', $.attrpath),
    )),

    binary_expression: $ => choice(
      ...[
        [prec.left, PREC.pipe, '|>'],
        [prec.right, PREC.pipe, '<|'],
        [prec.right, PREC.impl, '->'],
        [prec.left, PREC.or, '||'],
        [prec.left, PREC.and, '&&'],
        [prec.left, PREC.eq, '=='],
        [prec.left, PREC.eq, '!='],
        [prec.left, PREC.compare, '<'],
        [prec.left, PREC.compare, '>'],
        [prec.left, PREC.compare, '<='],
        [prec.left, PREC.compare, '>='],
        [prec.right, PREC.update, '//'],
        [prec.left, PREC.add, '+'],
        [prec.left, PREC.add, '-'],
        [prec.left, PREC.mul, '*'],
        [prec.left, PREC.mul, '/'],
        [prec.right, PREC.concat, '++'],
      ].map(([assoc, level, op]) => assoc(level, seq(
        field('left', $._expr_op),
        field('operator', op),
        field('right', $._expr_op),
      ))),
    ),

    // ---- application and selection --------------------------------------

    _expr_apply: $ => choice($.apply_expression, $._expr_select),

    apply_expression: $ => seq(field('function', $._expr_apply), field('argument', $._expr_select)),

    _expr_select: $ => choice($.select_expression, $._expr_simple),

    select_expression: $ => seq(
      field('expression', $._expr_simple),
      '.',
      field('attrpath', $.attrpath),
      optional(seq('or', field('default', $._expr_select))),
    ),

    // ---- simple expressions ---------------------------------------------

    _expr_simple: $ => choice(
      $.variable_expression,
      $.integer_expression,
      $.float_expression,
      $.string_expression,
      $.indented_string_expression,
      $.path_expression,
      $.hpath_expression,
      $.spath_expression,
      $.uri_expression,
      $.parenthesized_expression,
      $.attrset_expression,
      $.let_attrset_expression,
      $.rec_attrset_expression,
      $.list_expression,
    ),

    variable_expression: $ => field('name', $.identifier),

    integer_expression: _ => /[0-9]+/,

    float_expression: _ => /(([1-9][0-9]*\.[0-9]*)|(0?\.[0-9]+))([Ee][+-]?[0-9]+)?/,

    // `<nixpkgs/lib>`
    spath_expression: _ => /<[a-zA-Z0-9._\-+]+(\/[a-zA-Z0-9._\-+]+)*>/,

    uri_expression: _ => /[a-zA-Z][a-zA-Z0-9+\-.]*:[a-zA-Z0-9%/?:@&=+$,\-_.!~*']+/,

    // `./a/b`, `/abs`, `a/b`, `./${name}.nix`
    path_expression: $ => seq(
      alias($._path_start, $.path_fragment),
      repeat(choice($.path_fragment, $.interpolation)),
    ),

    // `~/a`, `~/${x}`
    hpath_expression: $ => seq(
      alias($._hpath_start, $.path_fragment),
      repeat(choice($.path_fragment, $.interpolation)),
    ),

    string_expression: $ => seq(
      '"',
      repeat(choice($.string_fragment, $.interpolation, $.escape_sequence)),
      '"',
    ),

    escape_sequence: _ => token.immediate(/\\(.|\r?\n)/),

    indented_string_expression: $ => seq(
      "''",
      repeat(choice(
        $.indented_string_fragment,
        $.interpolation,
        alias($._indented_escape_sequence, $.escape_sequence),
      )),
      "''",
    ),

    // `''$` (a literal `$`), `'''` (a literal `''`), `''\n` and friends.
    _indented_escape_sequence: _ => token.immediate(choice("''$", "'''", /''\\(.|\r?\n)/)),

    interpolation: $ => seq('${', field('expression', $._expression), '}'),

    parenthesized_expression: $ => seq('(', field('expression', $._expression), ')'),

    list_expression: $ => seq('[', repeat(field('element', $._expr_select)), ']'),

    attrset_expression: $ => seq('{', optional($.binding_set), '}'),

    rec_attrset_expression: $ => seq('rec', '{', optional($.binding_set), '}'),

    // The old `let { body = ...; }` form.
    let_attrset_expression: $ => seq('let', '{', optional($.binding_set), '}'),

    // ---- bindings -------------------------------------------------------

    binding_set: $ => repeat1(field('binding', choice($.binding, $.inherit, $.inherit_from))),

    binding: $ => seq(field('attrpath', $.attrpath), '=', field('expression', $._expression), ';'),

    inherit: $ => seq('inherit', field('attrs', $.inherited_attrs), ';'),

    inherit_from: $ => seq(
      'inherit', '(', field('expression', $._expression), ')',
      field('attrs', $.inherited_attrs), ';',
    ),

    inherited_attrs: $ => repeat1(field('attr', $._attr)),

    attrpath: $ => seq(field('attr', $._attr), repeat(seq('.', field('attr', $._attr)))),

    _attr: $ => choice($.identifier, $.string_expression, $.interpolation),

    comment: _ => token(choice(
      seq('#', /.*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),
  },
});
