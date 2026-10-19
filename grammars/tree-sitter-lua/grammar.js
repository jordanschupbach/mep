/**
 * @file Tree-sitter grammar for Lua.
 *
 * mep's own grammar, written against the Lua 5.4 reference manual (§3, and
 * the complete syntax in §9) and Lua's lexer (llex.c), plus Lua 5.5's
 * `global` declarations. Its node names, fields and tree shapes are the ones
 * tree-sitter-grammars/tree-sitter-lua 0.5 established, which mep vendored
 * before (third_party/grammars/lua) -- so queries written for that grammar
 * keep working; see README.md for how the two were compared.
 *
 * Long brackets -- `[==[ ... ]==]` strings and `--[==[ ... ]==]` comments --
 * close only at the same level, so they come from the external scanner
 * (src/scanner.c), which keeps the level as state.
 */

// Higher binds tighter: §3.4.8, bottom row first.
const PREC = {
  or: 1,
  and: 2,
  compare: 3,
  bit_or: 4,
  bit_xor: 5,
  bit_and: 6,
  shift: 7,
  concat: 8, // right associative
  add: 9,
  mul: 10,
  unary: 11,
  power: 12, // right associative
};

module.exports = grammar({
  name: 'lua',

  extras: $ => [/\s/, $.comment],

  externals: $ => [
    $._line_comment_content,
    $._block_comment_start,
    $._block_comment_content,
    $._block_comment_end,
    $._block_string_start,
    $._block_string_content,
    $._block_string_end,
    // Never produced: valid only during error recovery, which the scanner
    // uses to tell that apart from a real parse state.
    $._error_sentinel,
  ],

  word: $ => $.identifier,

  rules: {
    chunk: $ => seq(
      optional($.hash_bang_line),
      repeat($._statement),
      optional($.return_statement),
    ),

    hash_bang_line: _ => /#.*/,

    // A non-empty run of statements, ending in at most one `return`.
    block: $ => choice(
      seq(repeat1($._statement), optional($.return_statement)),
      $.return_statement,
    ),

    // ---- statements -----------------------------------------------------

    _statement: $ => choice(
      $.empty_statement,
      $.assignment_statement,
      $.function_call,
      $.label_statement,
      $.break_statement,
      $.goto_statement,
      $.do_statement,
      $.while_statement,
      $.repeat_statement,
      $.if_statement,
      $.for_statement,
      $.function_declaration,
      $._declaration,
    ),

    return_statement: $ => seq('return', optional(alias($._expression_list_plain, $.expression_list)), optional(';')),

    empty_statement: _ => ';',

    assignment_statement: $ => seq(
      alias($._variable_assignment_varlist, $.variable_list),
      '=',
      alias($._variable_assignment_explist, $.expression_list),
    ),

    _variable_assignment_varlist: $ => commaSep1(field('name', $._variable)),

    _variable_assignment_explist: $ => commaSep1(field('value', $._expression)),

    label_statement: $ => seq('::', $.identifier, '::'),

    break_statement: _ => 'break',

    goto_statement: $ => seq('goto', $.identifier),

    do_statement: $ => seq('do', optional(field('body', $.block)), 'end'),

    while_statement: $ => seq(
      'while', field('condition', $._expression),
      'do', optional(field('body', $.block)),
      'end',
    ),

    repeat_statement: $ => seq(
      'repeat', optional(field('body', $.block)),
      'until', field('condition', $._expression),
    ),

    if_statement: $ => seq(
      'if', field('condition', $._expression),
      'then', optional(field('consequence', $.block)),
      repeat(field('alternative', $.elseif_statement)),
      optional(field('alternative', $.else_statement)),
      'end',
    ),

    elseif_statement: $ => seq(
      'elseif', field('condition', $._expression),
      'then', optional(field('consequence', $.block)),
    ),

    else_statement: $ => seq('else', optional(field('body', $.block))),

    for_statement: $ => seq(
      'for', field('clause', choice($.for_generic_clause, $.for_numeric_clause)),
      'do', optional(field('body', $.block)),
      'end',
    ),

    for_generic_clause: $ => seq(
      alias($._name_list, $.variable_list),
      'in',
      alias($._expression_list_plain, $.expression_list),
    ),

    for_numeric_clause: $ => seq(
      field('name', $.identifier),
      '=',
      field('start', $._expression),
      ',',
      field('end', $._expression),
      optional(seq(',', field('step', $._expression))),
    ),

    // ---- declarations ---------------------------------------------------

    // Tagged with which kind it is; the keyword is the declaration's own
    // first token (tree-sitter-lua's shape).
    _declaration: $ => choice(
      field('local_declaration', choice(
        alias($._local_function_declaration, $.function_declaration),
        alias($._local_variable_declaration, $.variable_declaration),
      )),
      // Lua 5.5.
      field('global_declaration', choice(
        alias($._global_function_declaration, $.function_declaration),
        alias($._global_variable_declaration, $.variable_declaration),
        $.implicit_variable_declaration,
      )),
    ),

    // `function a.b.c:m() ... end`: the name is only ever dotted names and
    // one optional method, never an arbitrary expression.
    function_declaration: $ => seq(
      'function',
      field('name', choice(
        $.identifier,
        alias($._function_name_dot, $.dot_index_expression),
        alias($._function_name_method, $.method_index_expression),
      )),
      $._function_body,
    ),

    _function_name_dot: $ => seq(
      field('table', choice($.identifier, alias($._function_name_dot, $.dot_index_expression))),
      '.', field('field', $.identifier),
    ),

    _function_name_method: $ => seq(
      field('table', choice($.identifier, alias($._function_name_dot, $.dot_index_expression))),
      ':', field('method', $.identifier),
    ),

    _local_function_declaration: $ => seq('local', 'function', field('name', $.identifier), $._function_body),

    _global_function_declaration: $ => seq('global', 'function', field('name', $.identifier), $._function_body),

    _local_variable_declaration: $ => seq('local', $._variable_declaration_body),

    _global_variable_declaration: $ => seq('global', $._variable_declaration_body),

    // `local a <const>, b`, `local a, b = 1, 2`, `global <const> x = 1`
    _variable_declaration_body: $ => choice(
      alias($._attribute_name_list, $.variable_list),
      alias($._local_assignment, $.assignment_statement),
    ),

    _local_assignment: $ => seq(
      alias($._attribute_name_list, $.variable_list),
      '=',
      alias($._variable_assignment_explist, $.expression_list),
    ),

    _attribute_name_list: $ => seq(
      optional(field('attribute', $.attribute)),
      commaSep1(seq(field('name', $.identifier), optional(field('attribute', $.attribute)))),
    ),

    // Lua 5.5: `global *`, `global <const> *`.
    implicit_variable_declaration: $ => seq('global', optional(field('attribute', $.attribute)), '*'),

    attribute: $ => seq('<', $.identifier, '>'),

    _name_list: $ => commaSep1(field('name', $.identifier)),

    // ---- functions ------------------------------------------------------

    _function_body: $ => seq(
      field('parameters', $.parameters),
      optional(field('body', $.block)),
      'end',
    ),

    parameters: $ => seq(
      '(',
      optional(choice(
        seq(commaSep1(field('name', $.identifier)), optional(seq(',', $.vararg_expression))),
        $.vararg_expression,
      )),
      ')',
    ),

    function_definition: $ => seq('function', $._function_body),

    // ---- expressions ----------------------------------------------------

    _expression: $ => choice(
      $.nil,
      $.false,
      $.true,
      $.number,
      $.string,
      $.vararg_expression,
      $.function_definition,
      $._prefix_expression,
      $.table_constructor,
      $.binary_expression,
      $.unary_expression,
    ),

    _expression_list_plain: $ => commaSep1($._expression),

    _prefix_expression: $ => choice($._variable, prec(1, $.function_call), $.parenthesized_expression),

    _variable: $ => choice($.identifier, $._global_name, $.bracket_index_expression, $.dot_index_expression),

    // Lua 5.5's `global` is a keyword only where it starts a declaration;
    // before 5.5 it was an ordinary name (`global.x = 1`, `global:m()`).
    _global_name: $ => alias('global', $.identifier),

    bracket_index_expression: $ => seq(
      field('table', $._prefix_expression),
      '[', field('field', $._expression), ']',
    ),

    dot_index_expression: $ => seq(
      field('table', $._prefix_expression),
      '.', field('field', $.identifier),
    ),

    method_index_expression: $ => seq(
      field('table', $._prefix_expression),
      ':', field('method', $.identifier),
    ),

    // A `(` after a value always calls it, even on the next line
    // (`f\n(g)()`): Lua's grammar is ambiguous there and Lua reads a call.
    function_call: $ => prec(1, seq(
      field('name', choice($._prefix_expression, $.method_index_expression)),
      field('arguments', $.arguments),
    )),

    arguments: $ => choice(
      seq('(', optional(commaSep1($._expression)), ')'),
      $.table_constructor,
      $.string,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    table_constructor: $ => seq('{', optional($._field_list), '}'),

    _field_list: $ => seq($.field, repeat(seq($._field_separator, $.field)), optional($._field_separator)),

    _field_separator: _ => choice(',', ';'),

    field: $ => choice(
      seq('[', field('name', $._expression), ']', '=', field('value', $._expression)),
      seq(field('name', choice($.identifier, $._global_name)), '=', field('value', $._expression)),
      field('value', $._expression),
    ),

    binary_expression: $ => choice(
      ...[
        ['or', PREC.or],
        ['and', PREC.and],
        ['<', PREC.compare], ['<=', PREC.compare], ['==', PREC.compare],
        ['~=', PREC.compare], ['>=', PREC.compare], ['>', PREC.compare],
        ['|', PREC.bit_or],
        ['~', PREC.bit_xor],
        ['&', PREC.bit_and],
        ['<<', PREC.shift], ['>>', PREC.shift],
        ['+', PREC.add], ['-', PREC.add],
        ['*', PREC.mul], ['/', PREC.mul], ['//', PREC.mul], ['%', PREC.mul],
      ].map(([op, level]) => prec.left(level, seq(
        field('left', $._expression), field('operator', op), field('right', $._expression),
      ))),
      ...[['..', PREC.concat], ['^', PREC.power]].map(([op, level]) => prec.right(level, seq(
        field('left', $._expression), field('operator', op), field('right', $._expression),
      ))),
    ),

    unary_expression: $ => prec.left(PREC.unary, seq(
      field('operator', choice('not', '#', '-', '~')),
      field('operand', $._expression),
    )),

    // ---- literals -------------------------------------------------------

    nil: _ => 'nil',
    false: _ => 'false',
    true: _ => 'true',

    vararg_expression: _ => '...',

    number: _ => token(choice(
      /0[xX]([0-9a-fA-F]+(\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)([pP][+-]?[0-9]+)?/,
      /([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?/,
    )),

    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_]*/,

    string: $ => choice(
      seq('"', optional(field('content', alias($._double_quoted_content, $.string_content))), token.immediate('"')),
      seq("'", optional(field('content', alias($._single_quoted_content, $.string_content))), token.immediate("'")),
      seq(
        alias($._block_string_start, '[['),
        field('content', alias($._block_string_content, $.string_content)),
        alias($._block_string_end, ']]'),
      ),
    ),

    _double_quoted_content: $ => repeat1(choice(token.immediate(prec(1, /[^"\\\r\n]+/)), $.escape_sequence)),

    _single_quoted_content: $ => repeat1(choice(token.immediate(prec(1, /[^'\\\r\n]+/)), $.escape_sequence)),

    escape_sequence: _ => token.immediate(seq('\\', choice(
      /[abfnrtv\\"']/,
      /\r?\n/,
      /z\s*/,
      /x[0-9a-fA-F]{2}/,
      /[0-9]{1,3}/,
      /u\{[0-9a-fA-F]+\}/,
    ))),

    // After `--`, the scanner decides: a long bracket opens a block
    // comment; anything else is the rest of the line (possibly empty).
    comment: $ => seq('--', choice(
      field('content', alias($._line_comment_content, $.comment_content)),
      seq(
        alias($._block_comment_start, '[['),
        field('content', alias($._block_comment_content, $.comment_content)),
        alias($._block_comment_end, ']]'),
      ),
    )),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
