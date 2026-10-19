/**
 * @file Tree-sitter grammar for justfiles (https://just.systems).
 *
 * mep's own grammar, written against just's reference grammar
 * (GRAMMAR.md in the just repository) and its lexer's rules for
 * indentation and recipe bodies.
 *
 * A justfile is line-oriented: items end at a newline, except inside
 * `(...)`, `[...]` and `{...}`, where just ignores newlines. A recipe's
 * body is the run of indented lines after its header, and its text is
 * passed to the shell untouched except for `{{...}}` interpolations. The
 * external scanner (src/scanner.c) produces the newlines, the body's
 * indent and dedent, and the body text; this file arranges them.
 */

module.exports = grammar({
  name: 'just',

  // Spaces and tabs separate tokens; a newline is whitespace only where
  // the scanner was not asked for one (inside brackets).
  extras: $ => [/[ \t\r\n]/, /\\\r?\n/, $.comment],

  externals: $ => [
    $._newline,
    $._indent,
    $._line_indent,
    $._dedent,
    $.line_prefix,
    $.text,
    // Never produced: valid only during error recovery, which the scanner
    // uses to tell that apart from a real parse state.
    $._error_sentinel,
  ],

  word: $ => $.identifier,

  // `alias x ...` and the like are an item or a recipe named `alias`
  // until the `:=` or `:` that follows says which. When both readings
  // survive -- a recipe header's parameters may run on past a newline the
  // item reading ended at -- the items' dynamic precedence picks the item.
  conflicts: $ => [
    [$._name, $.alias],
    [$._name, $.export],
    [$._name, $.unexport],
    [$._name, $.setting],
    [$._name, $.module],
  ],

  rules: {
    source_file: $ => repeat($._item),

    _item: $ => choice(
      $.assignment,
      $.export,
      $.unexport,
      $.alias,
      $.setting,
      $.import,
      $.module,
      $.recipe,
    ),

    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_\-]*/,

    // just's keywords are only keywords where they start an item, so any
    // of them can also name a variable or a recipe.
    _name: $ => choice(
      $.identifier,
      alias(choice('alias', 'export', 'unexport', 'set', 'import', 'mod', 'if', 'else', 'true', 'false'), $.identifier),
    ),

    comment: _ => token(prec(-1, /#.*/)),

    // ---- items ----------------------------------------------------------

    assignment: $ => seq(
      repeat($.attribute),
      field('name', $._name), ':=', field('value', $._expression), $._newline,
    ),

    export: $ => prec.dynamic(1, seq('export', field('name', $.identifier), ':=', field('value', $._expression), $._newline)),

    unexport: $ => prec.dynamic(1, seq('unexport', field('name', $.identifier), $._newline)),

    alias: $ => prec.dynamic(1, seq(
      repeat($.attribute),
      'alias', field('name', $.identifier), ':=', field('target', $.module_path), $._newline,
    )),

    setting: $ => prec.dynamic(1, seq(
      'set', field('name', $.identifier),
      optional(seq(':=', field('value', choice($.boolean, $._string, $.setting_list)))),
      $._newline,
    )),

    setting_list: $ => seq('[', commaSep($._string), ']'),

    import: $ => prec.dynamic(1, seq('import', optional(token.immediate('?')), field('path', $._string), $._newline)),

    module: $ => prec.dynamic(1, seq(
      repeat($.attribute),
      'mod', optional(token.immediate('?')), field('name', $.identifier),
      optional(field('path', $._string)),
      $._newline,
    )),

    // `[private]`, `[group('ci'), linux]`, `[working-directory: 'web']`,
    // each on its own line or several on one.
    attribute: $ => seq('[', commaSep1($.attribute_item), ']', optional($._newline)),

    attribute_item: $ => seq(
      field('name', $.identifier),
      optional(choice(
        seq('(', commaSep(field('argument', $._string)), ')'),
        seq(':', field('argument', $._string)),
      )),
    ),

    // ---- recipes --------------------------------------------------------

    recipe: $ => seq(
      repeat($.attribute),
      $.recipe_header,
      $._newline,
      optional($.recipe_body),
    ),

    recipe_header: $ => seq(
      optional(field('quiet', '@')),
      field('name', $._name),
      optional(field('parameters', $.parameters)),
      ':',
      optional(field('dependencies', $.dependencies)),
    ),

    parameters: $ => choice(
      seq(repeat1($.parameter), optional($.variadic_parameter)),
      $.variadic_parameter,
    ),

    parameter: $ => seq(
      optional(field('export', '$')),
      field('name', $.identifier),
      optional(seq('=', field('default', $.value))),
    ),

    variadic_parameter: $ => seq(
      field('kleene', choice('+', '*')),
      $.parameter,
    ),

    dependencies: $ => choice(
      seq(repeat1($._dependency), optional(seq('&&', repeat1($._dependency)))),
      seq('&&', repeat1($._dependency)),
    ),

    _dependency: $ => choice(
      alias($.module_path, $.dependency),
      $.dependency_expression,
    ),

    // `(build "release" target)`
    dependency_expression: $ => seq(
      '(', field('name', $.module_path), repeat(field('argument', $._expression)), ')',
    ),

    // `name`, or `module::name`
    module_path: $ => seq($.identifier, repeat(seq('::', $.identifier))),

    // `_indent` and `_line_indent` cover a line's indentation and any
    // blank lines before it; `_dedent` is zero-width.
    recipe_body: $ => seq(
      $._indent,
      $.recipe_line,
      repeat(seq($._line_indent, $.recipe_line)),
      $._dedent,
    ),

    // `line_prefix` is `@` (don't echo) and/or `-` (ignore failure).
    recipe_line: $ => seq(
      optional(field('prefix', $.line_prefix)),
      repeat1(choice($.text, $.interpolation)),
      $._newline,
    ),

    interpolation: $ => seq('{{', field('expression', $._expression), '}}'),

    // ---- expressions ----------------------------------------------------

    _expression: $ => choice($.if_expression, $.binary_expression, $.unary_expression, $.value),

    if_expression: $ => seq(
      'if', field('condition', $._condition),
      '{', field('consequence', $._expression), '}',
      'else', field('alternative', choice($.if_expression, seq('{', $._expression, '}'))),
    ),

    _condition: $ => choice($.comparison, $._expression),

    // `a == b`: an if's condition, or assert()'s first argument.
    comparison: $ => seq(
      field('left', $._expression),
      field('operator', choice('==', '!=', '=~', '!~')),
      field('right', $._expression),
    ),

    binary_expression: $ => choice(
      prec.right(1, seq(field('left', $._expression), field('operator', '||'), field('right', $._expression))),
      prec.right(2, seq(field('left', $._expression), field('operator', '&&'), field('right', $._expression))),
      prec.right(3, seq(field('left', $._expression), field('operator', choice('+', '/')), field('right', $._expression))),
    ),

    // `/ path`: an absolute path join.
    unary_expression: $ => prec(4, seq(field('operator', '/'), field('argument', $._expression))),

    value: $ => choice(
      $.function_call,
      $.identifier,
      $._string,
      $.backtick,
      seq('(', $._expression, ')'),
    ),

    // A name followed by `(` is always a call, as in just's parser, even
    // with a space between and among a dependency's arguments.
    function_call: $ => prec(1, seq(
      field('name', $.identifier),
      '(',
      commaSep(field('argument', $._condition)),
      ')',
    )),

    boolean: _ => choice('true', 'false'),

    // ---- strings --------------------------------------------------------

    _string: $ => choice($.string, $.raw_string, $.indented_string, $.indented_raw_string),

    // A `x` prefix (`x'~/bin'`) asks just to expand `~` and `$VARS`.
    string: $ => seq(
      choice('"', 'x"'),
      repeat(choice(token.immediate(prec(1, /[^"\\]+/)), $.escape_sequence)),
      token.immediate('"'),
    ),

    escape_sequence: _ => token.immediate(/\\(u\{[0-9a-fA-F]+\}|\r?\n|.)/),

    raw_string: _ => /x?'[^']*'/,

    indented_string: _ => /x?"""([^"\\]|\\(.|\r?\n)|"[^"\\]|""[^"\\])*"""/,

    indented_raw_string: _ => /x?'''([^']|'[^']|''[^'])*'''/,

    backtick: _ => choice(
      /`[^`]*`/,
      /```([^`]|`[^`]|``[^`])*```/,
    ),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)), optional(','));
}

function commaSep(rule) {
  return optional(commaSep1(rule));
}
