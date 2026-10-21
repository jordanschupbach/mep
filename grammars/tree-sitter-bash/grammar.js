/**
 * @file Tree-sitter grammar for Bash.
 *
 * mep's own grammar, written against the Bash 5.3 reference manual
 * (chapter 3, "Basic Shell Features": shell syntax, shell commands, shell
 * functions, parameters, expansions, redirections; chapter 6.4,
 * conditional expressions; 6.5, shell arithmetic) and parse.y.
 *
 * Its node names, fields and tree shapes are the ones tree-sitter-bash
 * 0.25 established (the grammar mep loaded dynamically before this one),
 * so queries written for it keep working. So is the order of the rules:
 * where two readings survive to the same point, tree-sitter keeps the one
 * whose nodes are numbered lower, and declaration order sets the
 * numbering. Keep new rules after the ones they could compete with.
 *
 * The shell's lexer depends on what the parser expects next (a `}` is a
 * word or closes a group, `=` makes a name or is a word, `<<` opens a
 * here-document whose body comes after the line ends). Those tokens are
 * read by the external scanner, src/scanner.c.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// Characters that end an unquoted word (the manual's metacharacters, plus
// the quoting and expansion characters that start a new piece of one).
const SPECIAL_CHARACTERS = [
  '\'', '"',
  '<', '>',
  '{', '}',
  '\\[', '\\]',
  '(', ')',
  '`', '$',
  '|', '&', ';',
  '\\',
  '\\s',
];

// Operator precedence, lowest first. Arithmetic follows the manual's
// "Shell Arithmetic" table; the test operators sit between comparison and
// the unary operators.
const PREC = {
  UPDATE: 0,
  ASSIGN: 1,
  TERNARY: 2,
  LOGICAL_OR: 3,
  LOGICAL_AND: 4,
  BITWISE_OR: 5,
  BITWISE_XOR: 6,
  BITWISE_AND: 7,
  EQUALITY: 8,
  COMPARE: 9,
  TEST: 10,
  UNARY: 11,
  SHIFT: 12,
  ADD: 13,
  MULTIPLY: 14,
  EXPONENT: 15,
  NEGATE: 16,
  PREFIX: 17,
  POSTFIX: 18,
};

// Binary operators of arithmetic ($(( )), (( )), C-style for), each with
// its precedence. `=~` appears only because [[ ]] shares expressions.
const ASSIGNMENT_OPERATORS = ['+=', '-=', '*=', '/=', '%=', '**=', '<<=', '>>=', '&=', '^=', '|='];

module.exports = grammar({
  name: 'bash',

  conflicts: $ => [
    [$._expression, $.command_name],
    [$.command, $.variable_assignments],
    [$.redirected_statement, $.command],
    [$.redirected_statement, $.command_substitution],
    [$.function_definition, $.command_name],
    [$.pipeline],
  ],

  inline: $ => [
    $._statement,
    $._terminator,
    $._literal,
    $._terminated_statement,
    $._primary_expression,
    $._simple_variable_name,
    $._multiline_variable_name,
    $._special_variable_name,
    $._c_word,
    $._statement_not_subshell,
    $._redirect,
  ],

  // In the scanner's TokenType order.
  externals: $ => [
    $.heredoc_start,
    $.simple_heredoc_body,
    $._heredoc_body_beginning,
    $.heredoc_content,
    $.heredoc_end,
    $.file_descriptor,
    $._empty_value,
    $._concat,
    $.variable_name, // a name followed by `=`, `+=`, `[` or an expansion operator
    $.test_operator,
    $.regex,
    $._regex_no_slash,
    $._regex_no_space,
    $._expansion_word,
    $.extglob_pattern,
    $._bare_dollar,
    $._brace_start,
    $._immediate_double_hash,
    $._external_expansion_sym_hash,
    $._external_expansion_sym_bang,
    $._external_expansion_sym_equal,
    '}',
    ']',
    '<<',
    '<<-',
    /\n/,
    '(',
    'esac',
    $.__error_recovery,
  ],

  // Blanks, comments and line continuations (`\` newline) separate tokens.
  extras: $ => [
    $.comment,
    /\s/,
    /\\\r?\n/,
    /\\( |\t|\v|\f)/,
  ],

  supertypes: $ => [
    $._statement,
    $._expression,
    $._primary_expression,
  ],

  word: $ => $.word,

  rules: {
    // ---- 3.2.4 Lists of commands --------------------------------------------

    program: $ => optional($._statements),

    _statements: $ => prec(1, seq(
      repeat(seq($._statement, $._terminator)),
      $._statement,
      optional($._terminator),
    )),

    // A body that must end in a terminator: `if a; then` -- the `a;`.
    _terminated_statement: $ => repeat1(seq($._statement, $._terminator)),

    _statement: $ => choice(
      $._statement_not_subshell,
      $.subshell,
    ),

    _statement_not_subshell: $ => choice(
      $.redirected_statement,
      $.variable_assignment,
      $.variable_assignments,
      $.command,
      $.declaration_command,
      $.unset_command,
      $.test_command,
      $.negated_command,
      $.for_statement,
      $.c_style_for_statement,
      $.while_statement,
      $.if_statement,
      $.case_statement,
      $.pipeline,
      $.list,
      $.compound_statement,
      $.function_definition,
    ),

    // What one stage of a pipeline can be. Not a list: `|` binds tighter
    // than `&&` and `||`, so `a | b && c` is a list of a pipeline.
    _statement_not_pipeline: $ => prec(1, choice(
      $.redirected_statement,
      $.variable_assignment,
      $.variable_assignments,
      $.command,
      $.declaration_command,
      $.unset_command,
      $.test_command,
      $.negated_command,
      $.for_statement,
      $.c_style_for_statement,
      $.while_statement,
      $.if_statement,
      $.case_statement,
      $.compound_statement,
      $.function_definition,
      $.subshell,
    )),

    // ---- 3.6 Redirections, around a whole command ---------------------------

    redirected_statement: $ => prec.dynamic(-1, prec.right(-1, choice(
      seq(
        field('body', $._statement),
        field('redirect', repeat1(choice($.file_redirect, $.heredoc_redirect))),
      ),
      // A compound command takes a here-string too (a simple command's
      // is one of its own arguments): `while read l; do ...; done <<<"$x"`.
      seq(
        field('body', choice(
          $.if_statement, $.while_statement, $.for_statement, $.c_style_for_statement, $.case_statement,
          $.compound_statement, $.subshell, $.test_command,
        )),
        $.herestring_redirect,
      ),
      field('redirect', repeat1($._redirect)),
      $.herestring_redirect,
    ))),

    // ---- 3.2.5.1 Looping constructs -----------------------------------------

    // for name [in [words] ;] do ...; done -- and `select`, which shares
    // it. Without `in`, the terminator is optional (`for arg do`, POSIX);
    // with it, the word list may be empty (`for x in ; do`).
    for_statement: $ => seq(
      choice('for', 'select'),
      field('variable', $._simple_variable_name),
      choice(
        seq('in', field('value', repeat($._literal)), $._terminator),
        optional($._terminator),
      ),
      field('body', $.do_group),
    ),

    // for (( init; test; update )) -- arithmetic, in C's syntax.
    c_style_for_statement: $ => seq(
      'for',
      '((',
      choice($._for_body),
      '))',
      optional(';'),
      field('body', choice($.do_group, $.compound_statement)),
    ),
    _for_body: $ => seq(
      field('initializer', commaSep($._c_expression)),
      $._c_terminator,
      field('condition', commaSep($._c_expression)),
      $._c_terminator,
      field('update', commaSep($._c_expression)),
    ),

    _c_expression: $ => choice(
      $._c_expression_not_assignment,
      alias($._c_variable_assignment, $.variable_assignment),
    ),
    _c_expression_not_assignment: $ => choice(
      $._c_word,
      $.simple_expansion,
      $.expansion,
      $.number,
      $.string,
      alias($._c_unary_expression, $.unary_expression),
      alias($._c_binary_expression, $.binary_expression),
      alias($._c_postfix_expression, $.postfix_expression),
      alias($._c_parenthesized_expression, $.parenthesized_expression),
      $.command_substitution,
    ),

    _c_variable_assignment: $ => seq(
      field('name', alias($._c_word, $.variable_name)),
      '=',
      field('value', $._c_expression),
    ),
    _c_unary_expression: $ => prec(PREC.PREFIX, seq(
      field('operator', choice('++', '--')),
      $._c_expression_not_assignment,
    )),
    _c_binary_expression: $ => binaryTable($._c_expression_not_assignment, [
      [choice(...ASSIGNMENT_OPERATORS), PREC.UPDATE],
      [choice('||', '-o'), PREC.LOGICAL_OR],
      [choice('&&', '-a'), PREC.LOGICAL_AND],
      ['|', PREC.BITWISE_OR],
      ['^', PREC.BITWISE_XOR],
      ['&', PREC.BITWISE_AND],
      [choice('==', '!='), PREC.EQUALITY],
      [choice('<', '>', '<=', '>='), PREC.COMPARE],
      [choice('<<', '>>'), PREC.SHIFT],
      [choice('+', '-'), PREC.ADD],
      [choice('*', '/', '%'), PREC.MULTIPLY],
      ['**', PREC.EXPONENT],
    ], true),
    _c_postfix_expression: $ => prec(PREC.POSTFIX, seq(
      $._c_expression_not_assignment,
      field('operator', choice('++', '--')),
    )),
    _c_parenthesized_expression: $ => seq('(', commaSep1($._c_expression), ')'),
    _c_word: $ => alias(/[a-zA-Z_][a-zA-Z0-9_]*/, $.word),

    // while/until list; do list; done
    while_statement: $ => seq(
      choice('while', 'until'),
      field('condition', $._terminated_statement),
      field('body', $.do_group),
    ),

    do_group: $ => seq('do', optional($._terminated_statement), 'done'),

    // ---- 3.2.5.2 Conditional constructs -------------------------------------

    if_statement: $ => seq(
      'if',
      field('condition', $._terminated_statement),
      'then',
      optional($._terminated_statement),
      repeat($.elif_clause),
      optional($.else_clause),
      'fi',
    ),

    elif_clause: $ => seq('elif', $._terminated_statement, 'then', optional($._terminated_statement)),

    else_clause: $ => seq('else', optional($._terminated_statement)),

    // case word in [ [(] pattern [| pattern]...) list ;; ]... esac
    case_statement: $ => seq(
      'case',
      field('value', $._literal),
      optional($._terminator),
      'in',
      optional($._terminator),
      optional(seq(
        repeat($.case_item),
        alias($.last_case_item, $.case_item),
      )),
      'esac',
    ),

    case_item: $ => seq(
      choice(seq(
        optional('('),
        field('value', choice($._literal, $._extglob_blob)),
        repeat(seq('|', field('value', choice($._literal, $._extglob_blob)))),
        ')',
      )),
      optional($._statements),
      prec(1, choice(
        field('termination', ';;'),
        field('fallthrough', choice(';&', ';;&')),
      )),
    ),

    // The last item's `;;` may be left out.
    last_case_item: $ => seq(
      optional('('),
      field('value', choice($._literal, $._extglob_blob)),
      repeat(seq('|', field('value', choice($._literal, $._extglob_blob)))),
      ')',
      optional($._statements),
      optional(prec(1, ';;')),
    ),

    // ---- 3.3 Shell functions ------------------------------------------------

    // fname () body, or function fname [()] body.
    function_definition: $ => prec.right(seq(
      choice(
        seq('function', field('name', $.word), optional(seq('(', ')'))),
        seq(field('name', $.word), '(', ')'),
      ),
      field('body', choice($.compound_statement, $.subshell, $.test_command, $.if_statement)),
      field('redirect', optional($._redirect)),
    )),

    // ---- 3.2.5.3 Grouping commands, and (( )) --------------------------------

    compound_statement: $ => choice(
      seq('{', optional($._terminated_statement), token(prec(-1, '}'))),
      seq('((', repeat(seq($._arithmetic_expression, ',')), $._arithmetic_expression, '))'),
    ),

    subshell: $ => seq('(', $._statements, ')'),

    // ---- 3.2.3 Pipelines, 3.2.4 lists ---------------------------------------

    pipeline: $ => prec.right(seq(
      $._statement_not_pipeline,
      repeat1(seq(choice('|', '|&'), $._statement_not_pipeline)),
    )),

    list: $ => prec.left(-1, seq($._statement, choice('&&', '||'), $._statement)),

    // ---- 3.2.2 Simple commands, and the builtins with syntax of their own ----

    negated_command: $ => seq(
      '!',
      choice(prec(2, $.command), prec(1, $.variable_assignment), $.test_command, $.subshell),
    ),

    // [ expr ] (the test builtin) and [[ expr ]] (6.4).
    test_command: $ => seq(choice(
      seq('[', optional(choice($._expression, $.redirected_statement)), ']'),
      seq(
        '[[',
        choice($._expression, alias($._test_command_binary_expression, $.binary_expression)),
        ']]',
      ),
    )),

    _test_command_binary_expression: $ => prec(PREC.ASSIGN, seq(
      field('left', $._expression),
      field('operator', '='),
      field('right', alias($._regex_no_space, $.regex)),
    )),

    // declare/typeset/export/readonly/local: their arguments are
    // assignments, parsed as such.
    declaration_command: $ => prec.left(seq(
      choice('declare', 'typeset', 'export', 'readonly', 'local'),
      repeat(choice($._literal, $._simple_variable_name, $.variable_assignment)),
    )),

    unset_command: $ => prec.left(seq(
      choice('unset', 'unsetenv'),
      repeat(choice($._literal, $._simple_variable_name)),
    )),

    // [assignments] [redirections] name [arguments]
    command: $ => prec.left(seq(
      repeat(choice($.variable_assignment, field('redirect', $._redirect))),
      field('name', $.command_name),
      choice(
        repeat(choice(
          field('argument', $._literal),
          field('argument', alias($._bare_dollar, '$')),
          field('argument', seq(choice('=~', '=='), choice($._literal, $.regex))),
          field('redirect', $.herestring_redirect),
        )),
        $.subshell,
      ),
    )),

    command_name: $ => $._literal,

    // ---- 3.4 Shell parameters -----------------------------------------------

    variable_assignment: $ => seq(
      field('name', choice($.variable_name, $.subscript)),
      choice('=', '+='),
      field('value', choice($._literal, $.array, $._empty_value, alias($._comment_word, $.word))),
    ),

    variable_assignments: $ => seq($.variable_assignment, repeat1($.variable_assignment)),

    // name[index]
    subscript: $ => seq(
      field('name', $.variable_name),
      '[',
      field('index', choice($._literal, $.binary_expression, $.unary_expression, $.compound_statement, $.subshell)),
      optional($._concat),
      ']',
      optional($._concat),
    ),

    // ---- 3.6 Redirections ---------------------------------------------------

    file_redirect: $ => prec.left(seq(
      field('descriptor', optional($.file_descriptor)),
      choice(
        seq(
          choice('<', '>', '>>', '&>', '&>>', '<&', '>&', '>|', '<>'),
          field('destination', repeat1($._literal)),
        ),
        // Closing a descriptor.
        seq(choice('<&-', '>&-'), optional(field('destination', $._literal))),
      ),
    )),

    // 3.6.6: the rest of the line still follows the `<<WORD`, then the body.
    heredoc_redirect: $ => seq(
      field('descriptor', optional($.file_descriptor)),
      choice('<<', '<<-'),
      $.heredoc_start,
      optional(choice(
        alias($._heredoc_pipeline, $.pipeline),
        seq(field('redirect', repeat1($._redirect)), optional($._heredoc_expression)),
        $._heredoc_expression,
        $._heredoc_command,
      )),
      /\n/,
      choice($._heredoc_body, $._simple_heredoc_body),
    ),

    _heredoc_pipeline: $ => seq(choice('|', '|&'), $._statement),

    _heredoc_expression: $ => seq(
      field('operator', choice('||', '&&')),
      field('right', $._statement),
    ),

    _heredoc_command: $ => repeat1(field('argument', $._literal)),

    _heredoc_body: $ => seq($.heredoc_body, $.heredoc_end),

    heredoc_body: $ => seq(
      $._heredoc_body_beginning,
      repeat(choice($.expansion, $.simple_expansion, $.command_substitution, $.heredoc_content)),
    ),

    _simple_heredoc_body: $ => seq(alias($.simple_heredoc_body, $.heredoc_body), $.heredoc_end),

    // 3.6.7: <<< word
    herestring_redirect: $ => prec.left(seq(
      field('descriptor', optional($.file_descriptor)),
      '<<<',
      $._literal,
    )),

    _redirect: $ => choice($.file_redirect, $.herestring_redirect),

    // ---- 6.4 Conditional expressions ---------------------------------------

    _expression: $ => choice(
      $._literal,
      $.unary_expression,
      $.ternary_expression,
      $.binary_expression,
      $.postfix_expression,
      $.parenthesized_expression,
    ),

    binary_expression: $ => choice(
      binaryTable($._expression, [
        [choice(...ASSIGNMENT_OPERATORS), PREC.UPDATE],
        [choice('=', '=~'), PREC.ASSIGN],
        ['||', PREC.LOGICAL_OR],
        ['&&', PREC.LOGICAL_AND],
        ['|', PREC.BITWISE_OR],
        ['^', PREC.BITWISE_XOR],
        ['&', PREC.BITWISE_AND],
        [choice('==', '!='), PREC.EQUALITY],
        [choice('<', '>', '<=', '>='), PREC.COMPARE],
        [$.test_operator, PREC.TEST],
        [choice('<<', '>>'), PREC.SHIFT],
        [choice('+', '-'), PREC.ADD],
        [choice('*', '/', '%'), PREC.MULTIPLY],
        ['**', PREC.EXPONENT],
      ], true),
      // [[ x =~ regex ]]: the right side is a regex, up to whitespace.
      prec(PREC.ASSIGN, seq(
        field('left', $._expression),
        field('operator', '=~'),
        field('right', alias($._regex_no_space, $.regex)),
      )),
      // [[ x == pattern ]]: the right side is a pattern.
      prec(PREC.EQUALITY, seq(
        field('left', $._expression),
        field('operator', choice('==', '!=')),
        field('right', $._extglob_blob),
      )),
    ),

    ternary_expression: $ => prec.left(PREC.TERNARY, seq(
      field('condition', $._expression),
      '?',
      field('consequence', $._expression),
      ':',
      field('alternative', $._expression),
    )),

    unary_expression: $ => choice(
      prec(PREC.PREFIX, seq(field('operator', tokenLiterals(1, '++', '--')), $._expression)),
      prec(PREC.UNARY, seq(field('operator', tokenLiterals(1, '-', '+', '~')), $._expression)),
      prec.right(PREC.UNARY, seq(field('operator', '!'), $._expression)),
      prec.right(PREC.TEST, seq(field('operator', $.test_operator), $._expression)),
    ),

    postfix_expression: $ => prec(PREC.POSTFIX, seq($._expression, field('operator', choice('++', '--')))),

    // In [ ], test(1) groups with escaped parentheses: `[ \( a -o b \) ]`.
    parenthesized_expression: $ => choice(
      seq('(', $._expression, ')'),
      seq('\\(', $._expression, '\\)'),
    ),

    // ---- 3.1.2 Quoting, 3.5 expansions: the pieces of a word -----------------

    _literal: $ => choice(
      $.concatenation,
      $._primary_expression,
      alias(prec(-2, repeat1($._special_character)), $.word),
    ),

    _primary_expression: $ => choice(
      $.word,
      alias($.test_operator, $.word),
      $.string,
      $.raw_string,
      $.translated_string,
      $.ansi_c_string,
      $.number,
      $.expansion,
      $.simple_expansion,
      $.command_substitution,
      $.process_substitution,
      $.arithmetic_expansion,
      $.brace_expression,
    ),

    // 3.5.5: $(( expr )), and the obsolete $[ expr ].
    arithmetic_expansion: $ => choice(
      seq('$((', commaSep1($._arithmetic_expression), '))'),
      seq('$[', $._arithmetic_expression, ']'),
    ),

    // 3.5.1: the sequence expression {x..y}.
    brace_expression: $ => seq(
      alias($._brace_start, '{'),
      alias(token.immediate(/\d+/), $.number),
      token.immediate('..'),
      alias(token.immediate(/\d+/), $.number),
      token.immediate('}'),
    ),

    // ---- 6.5 Shell arithmetic -----------------------------------------------

    _arithmetic_expression: $ => prec(1, choice(
      $._arithmetic_literal,
      alias($._arithmetic_unary_expression, $.unary_expression),
      alias($._arithmetic_ternary_expression, $.ternary_expression),
      alias($._arithmetic_binary_expression, $.binary_expression),
      alias($._arithmetic_postfix_expression, $.postfix_expression),
      alias($._arithmetic_parenthesized_expression, $.parenthesized_expression),
      $.command_substitution,
    )),

    _arithmetic_literal: $ => prec(1, choice(
      $.number,
      $.subscript,
      $.simple_expansion,
      $.expansion,
      $._simple_variable_name,
      $.variable_name,
      $.string,
      $.raw_string,
    )),

    _arithmetic_binary_expression: $ => binaryTable($._arithmetic_expression, [
      [choice(...ASSIGNMENT_OPERATORS), PREC.UPDATE],
      [choice('=', '=~'), PREC.ASSIGN],
      ['||', PREC.LOGICAL_OR],
      ['&&', PREC.LOGICAL_AND],
      ['|', PREC.BITWISE_OR],
      ['^', PREC.BITWISE_XOR],
      ['&', PREC.BITWISE_AND],
      [choice('==', '!='), PREC.EQUALITY],
      [choice('<', '>', '<=', '>='), PREC.COMPARE],
      [choice('<<', '>>'), PREC.SHIFT],
      [choice('+', '-'), PREC.ADD],
      [choice('*', '/', '%'), PREC.MULTIPLY],
      ['**', PREC.EXPONENT],
    ], false),

    _arithmetic_ternary_expression: $ => prec.left(PREC.TERNARY, seq(
      field('condition', $._arithmetic_expression),
      '?',
      field('consequence', $._arithmetic_expression),
      ':',
      field('alternative', $._arithmetic_expression),
    )),

    _arithmetic_unary_expression: $ => choice(
      prec(PREC.PREFIX, seq(field('operator', tokenLiterals(1, '++', '--')), $._arithmetic_expression)),
      prec(PREC.UNARY, seq(field('operator', tokenLiterals(1, '-', '+', '~')), $._arithmetic_expression)),
      prec.right(PREC.UNARY, seq(field('operator', '!'), $._arithmetic_expression)),
    ),

    _arithmetic_postfix_expression: $ => prec(PREC.POSTFIX, seq(
      $._arithmetic_expression,
      field('operator', choice('++', '--')),
    )),

    _arithmetic_parenthesized_expression: $ => seq('(', $._arithmetic_expression, ')'),

    // ---- words and their pieces ----------------------------------------------

    // Adjacent pieces with nothing between them: a"b"$c.
    concatenation: $ => prec(-1, seq(
      choice($._primary_expression, alias($._special_character, $.word)),
      repeat1(seq(
        choice($._concat, alias(/`\s*`/, '``')),
        choice(
          $._primary_expression,
          alias($._special_character, $.word),
          alias($._comment_word, $.word),
          alias($._bare_dollar, '$'),
        ),
      )),
      optional(seq($._concat, '$')),
    )),

    _special_character: _ => token(prec(-1, choice('{', '}', '[', ']'))),

    // 3.1.2.3 Double quotes: expansions inside, everything else literal.
    string: $ => seq(
      '"',
      repeat(seq(
        choice(
          seq(optional('$'), $.string_content),
          $.expansion,
          $.simple_expansion,
          $.command_substitution,
          $.arithmetic_expansion,
        ),
        optional($._concat),
      )),
      optional('$'),
      '"',
    ),

    string_content: _ => token(prec(-1, /([^"`$\\\r\n]|\\(.|\r?\n))+/)),

    // 3.1.2.5 Locale-specific translation: $"...".
    translated_string: $ => seq('$', $.string),

    // name=(a b c)
    array: $ => seq('(', repeat($._literal), ')'),

    // 3.1.2.2 Single quotes.
    raw_string: _ => /'[^']*'/,

    // 3.1.2.4 ANSI-C quoting: $'...'.
    ansi_c_string: _ => /\$'([^']|\\')*'/,

    // Integers, hex, and base#digits (the digits may be an expansion:
    // `10#$n`, the usual way to read a zero-padded number as decimal).
    number: $ => choice(
      /-?(0x)?[0-9]+(#[0-9A-Za-z@_]+)?/,
      seq(/-?(0x)?[0-9]+#/, choice($.expansion, $.command_substitution, $.simple_expansion)),
    ),

    // 3.5.3: $name, $1, $@, $?, ...
    simple_expansion: $ => seq(
      '$',
      choice(
        $._simple_variable_name,
        $._multiline_variable_name,
        $._special_variable_name,
        $.variable_name,
        alias('!', $.special_variable_name),
        alias('#', $.special_variable_name),
      ),
    ),

    // 3.5.3: ${...}, with every operator the manual lists.
    expansion: $ => seq('${', optional($._expansion_body), '}'),
    _expansion_body: $ => choice(
      // ${!##} ${!#}
      repeat1(field('operator', choice(
        alias($._external_expansion_sym_hash, '#'),
        alias($._external_expansion_sym_bang, '!'),
        alias($._external_expansion_sym_equal, '='),
      ))),
      seq(
        optional(field('operator', token.immediate('!'))),
        choice($.variable_name, $._simple_variable_name, $._special_variable_name, $.subscript),
        choice(
          $._expansion_expression,
          $._expansion_regex,
          $._expansion_regex_replacement,
          $._expansion_regex_removal,
          $._expansion_max_length,
          $._expansion_operator,
        ),
      ),
      // ${!prefix*} ${!prefix@}
      seq(
        field('operator', token.immediate('!')),
        choice($._simple_variable_name, $.variable_name),
        optional(field('operator', choice(token.immediate('@'), token.immediate('*')))),
      ),
      // ${x} ${#x} ${!x}
      seq(
        optional(field('operator', immediateLiterals('#', '!', '='))),
        choice($.subscript, $._simple_variable_name, $._special_variable_name, $.command_substitution),
        repeat(field('operator', choice(
          alias($._external_expansion_sym_hash, '#'),
          alias($._external_expansion_sym_bang, '!'),
          alias($._external_expansion_sym_equal, '='),
        ))),
      ),
    ),

    // ${x:-word} ${x=word} ${x:+word} ${x:?word} ...
    _expansion_expression: $ => prec(1, seq(
      field('operator', immediateLiterals('=', ':=', '-', ':-', '+', ':+', '?', ':?')),
      optional(seq(choice(
        alias($._concatenation_in_expansion, $.concatenation),
        $.command_substitution,
        $.word,
        $.expansion,
        $.simple_expansion,
        $.array,
        $.string,
        $.raw_string,
        $.ansi_c_string,
        alias($._expansion_word, $.word),
      ))),
    )),

    // ${x#pattern} ${x##pattern} ${x%pattern} ${x%%pattern}
    _expansion_regex: $ => seq(
      field('operator', choice('#', alias($._immediate_double_hash, '##'), '%', '%%')),
      repeat(choice(
        $.regex,
        alias(')', $.regex),
        $.string,
        $.raw_string,
        alias(/\s+/, $.regex),
      )),
    ),

    // ${x/pattern/string} ${x//...} ${x/#...} ${x/%...}
    _expansion_regex_replacement: $ => seq(
      field('operator', choice('/', '//', '/#', '/%')),
      optional(choice(
        alias($._regex_no_slash, $.regex),
        $.string,
        $.command_substitution,
        seq($.string, alias($._regex_no_slash, $.regex)),
      )),
      // The replacement, and its `/`, may be left out.
      optional(seq(
        field('operator', '/'),
        optional(seq(
          choice(
            $._primary_expression,
            alias(prec(-2, repeat1($._special_character)), $.word),
            seq($.command_substitution, alias($._expansion_word, $.word)),
            alias($._expansion_word, $.word),
            alias($._concatenation_in_expansion, $.concatenation),
            $.array,
          ),
          field('operator', optional('/')),
        )),
      )),
    ),

    // Case modification: ${x,} ${x,,} ${x^} ${x^^}
    _expansion_regex_removal: $ => seq(
      field('operator', choice(',', ',,', '^', '^^')),
      optional($.regex),
    ),

    // Substrings: ${x:offset} ${x:offset:length}
    _expansion_max_length: $ => seq(
      field('operator', ':'),
      optional(choice(
        $._simple_variable_name,
        $.simple_expansion,  // ${a[@]:$n}
        $.number,
        $.arithmetic_expansion,
        $.expansion,
        $.parenthesized_expression,
        $.command_substitution,
        alias($._expansion_max_length_binary_expression, $.binary_expression),
        /\n/,
      )),
      optional(seq(
        field('operator', ':'),
        optional($.simple_expansion),
        optional(choice(
          $._simple_variable_name,
          $.number,
          $.arithmetic_expansion,
          $.expansion,
          $.parenthesized_expression,
          $.command_substitution,
          alias($._expansion_max_length_binary_expression, $.binary_expression),
          /\n/,
        )),
      )),
    ),

    _expansion_max_length_expression: $ => choice(
      $._simple_variable_name,
      $.number,
      $.expansion,
      alias($._expansion_max_length_binary_expression, $.binary_expression),
    ),
    _expansion_max_length_binary_expression: $ => choice(
      ...[[choice('+', '-'), PREC.ADD], [choice('*', '/', '%'), PREC.MULTIPLY]].map(([operator, precedence]) =>
        prec.left(/** @type {number} */ (precedence), seq(
          $._expansion_max_length_expression,
          field('operator', /** @type {RuleOrLiteral} */ (operator)),
          $._expansion_max_length_expression,
        )),
      ),
    ),

    // Transformations: ${x@Q} ${x@U} ...
    _expansion_operator: _ => seq(
      field('operator', token.immediate('@')),
      field('operator', immediateLiterals('U', 'u', 'L', 'Q', 'E', 'P', 'A', 'K', 'a', 'k')),
    ),

    _concatenation_in_expansion: $ => prec(-2, seq(
      choice(...expansionPieces($)),
      repeat1(seq(
        choice($._concat, alias(/`\s*`/, '``')),
        choice(...expansionPieces($)),
      )),
    )),

    // 3.5.4: $(...) and `...`
    command_substitution: $ => choice(
      seq('$(', $._statements, ')'),
      seq('$(', field('redirect', $.file_redirect), ')'),  // $(< file)
      prec(1, seq('`', $._statements, '`')),
      seq('$`', $._statements, '`'),
    ),

    // 3.5.6: <(...) and >(...)
    process_substitution: $ => seq(choice('<(', '>('), $._statements, ')'),

    // A pattern with quoted or expanded pieces in it: `*"$x"*`, `?*' '?*`.
    _extglob_blob: $ => prec.right(choice(
      $.extglob_pattern,
      seq(
        $.extglob_pattern,
        repeat1(prec.left(seq(
          choice($.string, $.raw_string, $.expansion, $.command_substitution),
          // What follows a piece may be plain text: `-${opts}p)`.
          optional(choice($.extglob_pattern, $.word)),
        ))),
      ),
    )),

    // 3.1.3: from an unquoted # that starts a word to the end of the line.
    comment: _ => token(prec(-10, /#.*/)),

    // A word that may start with `#` (where a comment can't begin: after `=`).
    _comment_word: _ => token(prec(-8, seq(
      choice(noneOf(...SPECIAL_CHARACTERS), seq('\\', noneOf('\\s'))),
      repeat(choice(noneOf(...SPECIAL_CHARACTERS), seq('\\', noneOf('\\s')), '\\ ')),
    ))),

    _simple_variable_name: $ => alias(/\w+/, $.variable_name),
    _multiline_variable_name: $ => alias(token(prec(-1, /(\w|\\\r?\n)+/)), $.variable_name),

    // 3.4.2 Special parameters.
    _special_variable_name: $ => alias(choice('*', '@', '?', '!', '#', '-', '$', '_'), $.special_variable_name),

    word: _ => token(seq(
      choice(noneOf('#', ...SPECIAL_CHARACTERS), seq('\\', noneOf('\\s'))),
      repeat(choice(noneOf(...SPECIAL_CHARACTERS), seq('\\', noneOf('\\s')), '\\ ')),
    )),

    _c_terminator: _ => choice(';', /\n/, '&'),
    _terminator: _ => choice(';', ';;', /\n/, '&'),
  },
});

/**
 * The pieces a word inside ${...} can be made of.
 *
 * @param {GrammarSymbols<string>} $
 * @returns {RuleOrLiteral[]}
 */
function expansionPieces($) {
  return [
    $.word,
    $.variable_name,
    $.simple_expansion,
    $.expansion,
    $.string,
    $.raw_string,
    $.ansi_c_string,
    $.command_substitution,
    alias($._expansion_word, $.word),
    $.array,
    $.process_substitution,
  ];
}

/**
 * Binary expressions over `operand`, one alternative per [operator,
 * precedence] row. Left-associative, except `**` when `rightPower`.
 *
 * @param {RuleOrLiteral} operand
 * @param {[RuleOrLiteral, number][]} table
 * @param {boolean} rightPower
 * @returns {ChoiceRule}
 */
function binaryTable(operand, table, rightPower) {
  return choice(...table.map(([operator, precedence]) =>
    prec[rightPower && operator === '**' ? 'right' : 'left'](precedence, seq(
      field('left', operand),
      field('operator', operator),
      field('right', operand),
    )),
  ));
}

/**
 * A regex matching any one character but `characters`.
 *
 * @param {...string} characters
 * @returns {RegExp}
 */
function noneOf(...characters) {
  return new RegExp('[^' + characters.map(c => c == '\\' ? '\\\\' : c).join('') + ']');
}

/** @param {RuleOrLiteral} rule */
function commaSep(rule) {
  return optional(commaSep1(rule));
}

/** @param {RuleOrLiteral} rule */
function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}

/** @param {...string} literals */
function immediateLiterals(...literals) {
  return choice(...literals.map(l => token.immediate(l)));
}

/**
 * @param {number} precedence
 * @param {...string} literals
 */
function tokenLiterals(precedence, ...literals) {
  return choice(...literals.map(l => token(prec(precedence, l))));
}
