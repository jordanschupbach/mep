/**
 * @file Tree-sitter grammar for R.
 *
 * mep's own grammar, written against R's reference grammar and lexer
 * (src/main/gram.y in R's sources) and the operator table in R's ?Syntax.
 * Its node names, fields and shapes are the ones r-lib/tree-sitter-r 1.x
 * established, which mep vendored before (third_party/grammars/r) and which
 * R tooling everywhere queries; trees from the two agree on R's own sources
 * and a body of CRAN packages (see README.md).
 *
 * Newlines end an expression only where one may end -- at the top level and
 * in a `{...}` body -- and never inside `(...)` or `[...]`. The external
 * scanner (src/scanner.c) gives a newline token only where the grammar asks
 * for one, which is exactly that, and also handles raw strings.
 */

// Higher binds tighter. R's ?Syntax, from the bottom of its table up.
const PREC = {
  help: -2, // `?`: below even a function's body (`function() 1 ? 2`)
  function: -1, // `function(x) x + 1` and if/for/while bodies extend right
  eq_assign: 2, // `=`
  left_assign: 3, // `<-` `<<-` `:=`
  right_assign: 4, // `->` `->>`
  tilde: 5, // `~`
  or: 6, // `|` `||`
  and: 7, // `&` `&&`
  not: 8, // unary `!`
  compare: 9, // `==` `!=` `<` `>` `<=` `>=`
  add: 10, // binary `+` `-`
  mul: 11, // `*` `/`
  special: 12, // `%any%` `|>`
  seq: 13, // `:`
  unary: 14, // unary `+` `-`
  power: 15, // `^` `**`
  extract: 16, // `$` `@`
  call: 17, // `f()` `x[]` `x[[]]`
  namespace: 18, // `::` `:::`
};

const NUMBER = token(choice(
  /0[xX][0-9a-fA-F]+(\.[0-9a-fA-F]*)?([pP][+-]?[0-9]+)?/,
  /[0-9]+(\.[0-9]*)?([eE][+-]?[0-9]+)?/,
  /\.[0-9]+([eE][+-]?[0-9]+)?/,
));

module.exports = grammar({
  name: 'r',

  extras: $ => [/\s/, $.comment],

  externals: $ => [
    $._newline,
    $._raw_string_open,
    $._raw_string_content,
    $._raw_string_close,
    // Zero-width, at byte 0: anchors `program` there even when the file
    // starts with whitespace (r-lib's shape; the root would otherwise start
    // at its first token).
    $._program_start,
    // Never produced: valid only during error recovery, which the scanner
    // uses to tell that apart from a real parse state.
    $._error_sentinel,
  ],

  word: $ => $._plain_identifier,

  rules: {
    // The top level's expressions have no field (r-lib's shape); a
    // `{...}` body's are each `body`.
    program: $ => seq($._program_start, optional(body($, $._expression))),

    _separator: $ => choice($._newline, ';'),

    _expression: $ => choice(
      $.function_definition,
      $.if_statement,
      $.for_statement,
      $.while_statement,
      $.repeat_statement,
      $.braced_expression,
      $.parenthesized_expression,
      $.call,
      $.subset,
      $.subset2,
      $.namespace_operator,
      $.extract_operator,
      $.unary_operator,
      $.binary_operator,
      $.identifier,
      $.integer,
      $.float,
      $.complex,
      $.string,
      $.dots,
      $.dot_dot_i,
      $.true,
      $.false,
      $.null,
      $.inf,
      $.nan,
      $.na,
      $.next,
      $.break,
    ),

    // ---- functions and control flow -------------------------------------

    function_definition: $ => prec.right(PREC.function, seq(
      field('name', choice('function', '\\')),
      field('parameters', $.parameters),
      field('body', $._expression),
    )),

    parameters: $ => seq(
      field('open', '('),
      optional(seq(
        field('parameter', $.parameter),
        repeat(seq($.comma, field('parameter', $.parameter))),
      )),
      field('close', ')'),
    ),

    parameter: $ => seq(
      field('name', choice($.identifier, $.dots, $.dot_dot_i)),
      optional(seq('=', optional(field('default', $._expression)))),
    ),

    if_statement: $ => prec.right(PREC.function, seq(
      'if',
      field('open', '('), field('condition', $._expression), field('close', ')'),
      field('consequence', $._expression),
      optional(seq('else', field('alternative', $._expression))),
    )),

    for_statement: $ => prec.right(PREC.function, seq(
      'for',
      field('open', '('),
      field('variable', choice($.identifier, $.dots, $.dot_dot_i)),
      'in',
      field('sequence', $._expression),
      field('close', ')'),
      field('body', $._expression),
    )),

    while_statement: $ => prec.right(PREC.function, seq(
      'while',
      field('open', '('), field('condition', $._expression), field('close', ')'),
      field('body', $._expression),
    )),

    repeat_statement: $ => prec.right(PREC.function, seq('repeat', field('body', $._expression))),

    braced_expression: $ => seq(field('open', '{'), optional(body($, field('body', $._expression))), field('close', '}')),

    parenthesized_expression: $ => seq(
      field('open', '('),
      optional(field('body', $._expression)),
      field('close', ')'),
    ),

    // ---- calls and indexing ---------------------------------------------

    call: $ => prec(PREC.call, seq(
      field('function', $._expression),
      field('arguments', alias($._call_arguments, $.arguments)),
    )),

    subset: $ => prec(PREC.call, seq(
      field('function', $._expression),
      field('arguments', alias($._subset_arguments, $.arguments)),
    )),

    subset2: $ => prec(PREC.call, seq(
      field('function', $._expression),
      field('arguments', alias($._subset2_arguments, $.arguments)),
    )),

    _call_arguments: $ => seq(field('open', '('), optional($._argument_list), field('close', ')')),
    _subset_arguments: $ => seq(field('open', '['), optional($._argument_list), field('close', ']')),
    _subset2_arguments: $ => seq(field('open', '[['), optional($._argument_list), field('close', ']]')),

    // Empty arguments (`x[, 1]`, `f(a, )`) leave only their commas.
    _argument_list: $ => choice(
      seq(field('argument', $.argument), repeat(seq($.comma, optional(field('argument', $.argument))))),
      seq(repeat1(seq($.comma, optional(field('argument', $.argument))))),
    ),

    argument: $ => choice(
      prec(1, seq(
        field('name', choice($.identifier, $.string, $.dots, $.dot_dot_i, $.null)),
        '=',
        optional(field('value', $._expression)),
      )),
      field('value', $._expression),
    ),

    // ---- operators ------------------------------------------------------

    namespace_operator: $ => prec.left(PREC.namespace, seq(
      field('lhs', choice($.identifier, $.string)),
      field('operator', choice('::', ':::')),
      field('rhs', choice($.identifier, $.string, $.dots, $.dot_dot_i)),
    )),

    extract_operator: $ => prec.left(PREC.extract, seq(
      field('lhs', $._expression),
      field('operator', choice('$', '@')),
      field('rhs', choice($.identifier, $.string, $.dots, $.dot_dot_i)),
    )),

    unary_operator: $ => choice(
      prec.left(PREC.unary, seq(field('operator', choice('-', '+')), field('rhs', $._expression))),
      prec.left(PREC.not, seq(field('operator', '!'), field('rhs', $._expression))),
      prec.left(PREC.tilde, seq(field('operator', '~'), field('rhs', $._expression))),
      prec.left(PREC.help, seq(field('operator', '?'), field('rhs', $._expression))),
    ),

    binary_operator: $ => choice(
      ...[
        [prec.left, PREC.help, '?'],
        [prec.right, PREC.eq_assign, '='],
        [prec.right, PREC.left_assign, choice('<-', '<<-', ':=')],
        [prec.left, PREC.right_assign, choice('->', '->>')],
        [prec.left, PREC.tilde, '~'],
        [prec.left, PREC.or, choice('|', '||')],
        [prec.left, PREC.and, choice('&', '&&')],
        [prec.left, PREC.compare, choice('==', '!=', '<', '>', '<=', '>=')],
        [prec.left, PREC.add, choice('+', '-')],
        [prec.left, PREC.mul, choice('*', '/')],
        [prec.left, PREC.special, choice($._special, '|>')],
        [prec.left, PREC.seq, ':'],
        [prec.right, PREC.power, choice('^', '**')],
      ].map(([assoc, level, op]) => assoc(level, seq(
        field('lhs', $._expression),
        field('operator', op),
        field('rhs', $._expression),
      ))),
    ),

    // `%in%`, `%>%`, `%%`, ...
    _special: _ => alias(token(/%[^%\n]*%/), 'special'),

    // ---- literals -------------------------------------------------------

    // Before the identifier rules: `..1` matches both, and the earlier
    // rule wins a tie; `..1x` is longer, so it is an identifier.
    dot_dot_i: _ => /\.\.[0-9]+/,

    _plain_identifier: _ => /[\p{L}][\p{L}\p{N}._]*|\.([\p{L}._][\p{L}\p{N}._]*)?/,

    identifier: $ => choice(
      $._plain_identifier,
      /`([^`\\]|\\.)*`/,
      // The native pipe's placeholder: `x |> f(y = _)`.
      '_',
    ),

    dots: _ => '...',

    float: _ => NUMBER,

    integer: _ => seq(NUMBER, token.immediate('L')),

    complex: _ => seq(NUMBER, token.immediate('i')),

    true: _ => 'TRUE',
    false: _ => 'FALSE',
    null: _ => 'NULL',
    inf: _ => 'Inf',
    nan: _ => 'NaN',
    na: _ => choice('NA', 'NA_integer_', 'NA_real_', 'NA_character_', 'NA_complex_'),
    next: _ => 'next',
    break: _ => 'break',

    string: $ => choice(
      seq(
        field('open', alias('"', $.string_open)),
        optional(field('content', alias($._double_quoted_content, $.string_content))),
        field('close', alias(token.immediate('"'), $.string_close)),
      ),
      seq(
        field('open', alias("'", $.string_open)),
        optional(field('content', alias($._single_quoted_content, $.string_content))),
        field('close', alias(token.immediate("'"), $.string_close)),
      ),
      // r"(...)", R"[...]", r"---{...}---", r'(...)': no escapes.
      seq(
        field('open', alias($._raw_string_open, $.string_open)),
        optional(field('content', alias($._raw_string_content, $.string_content))),
        field('close', alias($._raw_string_close, $.string_close)),
      ),
    ),

    _double_quoted_content: $ => repeat1(choice(token.immediate(prec(1, /[^"\\]+/)), $.escape_sequence)),

    _single_quoted_content: $ => repeat1(choice(token.immediate(prec(1, /[^'\\]+/)), $.escape_sequence)),

    escape_sequence: _ => token.immediate(seq('\\', choice(
      /x[0-9a-fA-F]{1,2}/,
      /[0-7]{1,3}/,
      /u[0-9a-fA-F]{1,4}/,
      /u\{[0-9a-fA-F]{1,4}\}/,
      /U[0-9a-fA-F]{1,8}/,
      /U\{[0-9a-fA-F]{1,8}\}/,
      /(.|\r?\n)/,
    ))),

    comma: _ => ',',

    comment: _ => /#[^\r\n]*/,
  },
});

// Expressions separated by newlines or `;`, with any number of either
// around them. Two expressions with no separator are an error, as in R.
function body($, expression) {
  return choice(
    repeat1($._separator),
    seq(
      repeat($._separator),
      expression,
      repeat(seq(repeat1($._separator), expression)),
      repeat($._separator),
    ),
  );
}
