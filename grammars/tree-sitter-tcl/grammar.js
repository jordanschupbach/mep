/**
 * @file Tcl grammar for tree-sitter: mep's own (grammars/tree-sitter-tcl).
 *
 * A drop-in for tree-sitter-grammars/tree-sitter-tcl (MIT, Lewis Russell;
 * the grammar mep loaded before): the same node types, fields and shapes,
 * so queries written for that one carry over. What changed is how words
 * are read, which now follows Tcl's own rules (the dodekalogue,
 * Tcl_ParseCommand in tclParse.c) -- see README.md:
 *
 *  - Braces: `{...}` extends to the matching brace, counted the way Tcl
 *    counts them. A braced word is read as a script (its commands get
 *    trees) only when its text is one; otherwise -- a regexp, a list of
 *    data, a string with an odd quote -- it is a `braced_word` with no
 *    children. The external scanner (src/scanner.c) decides.
 *  - Comments run to the end of the line, continued by a backslash-newline,
 *    and inside braces end at the brace that closes them.
 *  - Bare words may contain `(`, `)`, `"`, `!`, `#` and a lone `$`;
 *    `$name(index)` takes any index up to its `)`, spaces included.
 *  - The command-specific shapes (`set`, `if`, `proc`, `foreach`, `while`,
 *    `expr`, `catch`, `try`, `regexp`, `global`, `namespace`) are kept for
 *    the forms the old grammar read; any other form of those commands -- `if
 *    {...} then {...}` aside, which is read too -- is an ordinary `command`
 *    rather than a parse error (a GLR split; the
 *    specific shape wins when both read the text).
 *  - `expr` operands follow Tcl's expression syntax: numbers in every base,
 *    `**` right-associative, `?:` right-associative, functions of several
 *    arguments.
 */

// Taken from the expr man page
const PREC = {
  unary: 150, // - + ~ !
  exp: 140, // **
  muldiv: 130, // * / %
  addsub: 120, // + -
  shift: 110, // << >>
  compare: 100, // > < >= <=
  equal_bool: 90, // == !=
  equal_string: 80, // eq ne
  contain: 70, // in ni
  and_bit: 60, // &
  xor_bit: 50, // ^
  or_bit: 40, // |
  and_logical: 30, // &&
  or_logical: 20, // ||
  ternary: 10, // x ? y : z
};

// The commands that have a shape of their own. Each is also readable as an
// ordinary command (its keyword then the `name`), for the forms the shape
// does not cover.
const BUILTINS = ['set', 'if', 'proc', 'while', 'foreach', 'global', 'namespace', 'try', 'expr', 'catch', 'regexp',
  'else', 'elseif'];

// Characters that end a bare word or start some other part of one.
const WORD_STOP = '\\s\\\\\\[\\]$;{}';

module.exports = grammar({
  name: 'tcl',

  externals: $ => [
    $._concat,
    $._var_index_open,
    $._word_index_open,
    $.unpack,
    $._braced_data,
    $._script_mode,
    $._words_mode,
    $._dollar_literal,
    $._newline,
    $.comment,
    $._script_close,
    $._error_sentinel,
  ],

  inline: $ => [
    $._builtin,
    $._terminator,
    $._word,
  ],

  extras: _ => [
    /\s+/,
    /\\\r?\n/,
  ],

  conflicts: $ => [
    [$._keyword_name, $.elseif],
    [$._keyword_name, $.else],
    [$._keyword_name, $.catch],
    [$._keyword_name, $.procedure],
    [$._keyword_name, $.if],
    [$._keyword_name, $.set],
    [$.try, $._keyword_name],
    [$.namespace, $._keyword_name],
    [$.global, $._keyword_name],
    [$.foreach, $._keyword_name],
    [$.expr_cmd, $._keyword_name],
    [$.while, $._keyword_name],
    [$.regexp, $._keyword_name],
  ],

  rules: {
    source_file: $ => seq(
      repeat(seq(optional($._command), $._terminator)),
      optional($._command),
    ),

    _terminator: $ => choice($._newline, ';'),

    _builtin: $ => choice(
      $._conditional,
      $.global,
      $.namespace,
      $.procedure,
      $.set,
      $.try,
      $.foreach,
      $.expr_cmd,
      $.while,
      $.catch,
      $.regexp,
    ),

    // regexp ?switches? exp string ?matchVar? ?subMatchVar subMatchVar ...?
    regexp: $ => seq(
      'regexp',
      $._word_simple, // exp
      $._concat_word, // string
      repeat($._concat_word),
    ),

    while: $ => seq('while', $.expr, $._word),

    expr_cmd: $ => seq('expr', $.expr),

    foreach: $ => seq('foreach', $.arguments, $._word_simple, $._word),

    global: $ => seq('global', repeat($._concat_word)),

    namespace: $ => seq('namespace', $.word_list),

    try: $ => seq(
      'try',
      $._word,
      optional(seq(
        'on',
        'error',
        $.arguments,
        $._word,
      )),
      optional($.finally),
    ),

    finally: $ => seq('finally', $._word),

    _command: $ => choice(
      $._builtin,
      $.comment,
      $.command,
    ),

    // The keyword form loses to the command's own shape whenever both read
    // the text without errors.
    command: $ => choice(
      seq(field('name', $._word), optional(field('arguments', $.word_list))),
      prec.dynamic(-1000, seq(field('name', $._keyword_name), optional(field('arguments', $.word_list)))),
    ),

    // A command with a shape of its own, used in a form the shape does not
    // cover (`if {a} {b} {c}`, `set x(1) $y $z`, `catch {a} r o`).
    _keyword_name: $ => alias(choice(...BUILTINS), $.simple_word),

    word_list: $ => repeat1($._word),

    _word: $ => seq(
      optional($.unpack),
      choice(
        $.braced_word,
        $._concat_word,
      ),
    ),

    // A word's pieces, adjacent (the scanner's `_concat` says so). A quote
    // only opens a quoted word at the start of a word; inside one it is an
    // ordinary character, as `(` and `)` are anywhere but after a name.
    // Braces inside a word (`$a{b}`) are a piece of it.
    _concat_word: $ => seq(
      choice($._piece, $.quoted_word),
      repeat(seq($._concat, choice($._piece, $._quote_literal, $.braced_word_simple))),
    ),

    _piece: $ => choice(
      $.escaped_character,
      $.command_substitution,
      seq($.simple_word, optional(alias($._word_array_index, $.array_index))),
      $.variable_substitution,
      $._paren_literal,
      alias($._dollar_literal, $.simple_word),
    ),

    _word_simple: $ => seq(
      $._word_simple_first,
      repeat(seq($._concat, $._word_simple_piece)),
    ),

    _word_simple_first: $ => choice(
      $._word_simple_start,
      $.quoted_word,
      $.braced_word_simple,
    ),

    _word_simple_piece: $ => choice(
      $._word_simple_start,
      $._quote_literal,
      $.braced_word_simple,
    ),

    _word_simple_start: $ => choice(
      $.escaped_character,
      $.command_substitution,
      $.simple_word,
      $.variable_substitution,
      $._paren_literal,
      alias($._dollar_literal, $.simple_word),
    ),

    _paren_literal: $ => alias(new RegExp(`[()][^${WORD_STOP}()]*`), $.simple_word),
    _quote_literal: $ => alias(new RegExp(`"[^${WORD_STOP}()]*`), $.simple_word),

    // A variable's name: letters, digits, underscores and `::` separators.
    _var_name: _ => token.immediate(/([\p{L}\p{N}_]|::+)+/),

    array_index: $ => seq($._var_index_open, repeat($._index_piece), ')'),

    // The scanner's zero-width `_word_index_open` says a `)` closes the `(`.
    _word_array_index: $ => seq($._word_index_open, token.immediate('('), repeat($._index_piece), ')'),

    _index_piece: $ => choice(
      alias(/[^$\[\\)]+/, $.simple_word),
      $.variable_substitution,
      $.command_substitution,
      $.escaped_character,
      $._dollar_literal,
    ),

    variable_substitution: $ => seq(
      choice(
        seq('$', alias($._var_name, $.id)),
        seq('$', token.immediate('{'), token.immediate(/[^}]*/), '}'),
      ),
      optional($.array_index),
    ),

    braced_word: $ => choice(
      seq(
        '{',
        repeat($._terminator),
        optional(seq(
          $._command,
          repeat(seq(repeat1($._terminator), $._command)),
          repeat($._terminator),
        )),
        alias($._script_close, '}'),
      ),
      $._braced_data,
      $._script_mode,
    ),

    braced_word_simple: $ => choice(
      seq('{', repeat(choice($._word_simple, ';')), '}'),
      $._braced_data,
      $._words_mode,
    ),

    set: $ => seq(
      'set',
      choice(
        seq(alias($.simple_word, $.id), optional(alias($._word_array_index, $.array_index))),
        seq('$', token.immediate('{'), token.immediate(/[^}]+/), '}'),
      ),
      optional($._word_simple),
    ),

    procedure: $ => seq(
      'proc',
      field('name', $._word),
      field('arguments', $.arguments),
      field('body', $._word),
    ),

    _argument_word: $ => choice($.simple_word, $.quoted_word, $.braced_word),

    argument: $ => choice(
      field('name', $.simple_word),
      seq(
        '{',
        field('name', $.simple_word),
        optional(field('default', $._argument_word)),
        '}',
      ),
    ),

    arguments: $ => choice(
      seq('{', repeat($.argument), '}'),
      $.simple_word,
    ),

    number: _ => token(choice(
      /[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?/,
      /[+-]?0[xX][0-9a-fA-F]+/,
      /[+-]?0[oO][0-7]+/,
      /[+-]?0[bB][01]+/,
    )),

    _boolean: _ => token(prec(1, choice(
      /[Tt][Rr][Uu][Ee]/,
      /[Ff][Aa][Ll][Ss][Ee]/,
      /[Yy][Ee][Ss]/,
      /[Nn][Oo]/,
      /[Oo][Nn]/,
      /[Oo][Ff][Ff]/,
    ))),

    _expr_atom_no_brace: $ => choice(
      // As a numeric value, either integer or floating-point.
      $.number,

      // As a boolean value, using any form understood by string is boolean.
      $._boolean,

      // As a mathematical function whose arguments have any of the above
      // forms for operands, such as sin($x).
      seq(
        alias(token(/[a-zA-Z_][a-zA-Z0-9_]*(::[a-zA-Z0-9_]+)*/), $.simple_word),
        token.immediate('('),
        optional(seq($._expr, repeat(seq(',', $._expr)))),
        ')',
      ),

      // As a Tcl command enclosed in brackets.
      $.command_substitution,

      // As a string enclosed in double-quotes, with substitutions.
      $.quoted_word,

      // As a Tcl variable, using standard $ notation.
      $.variable_substitution,
    ),

    _expr: $ => choice(
      $.unary_expr,
      $.binop_expr,
      $.ternary_expr,
      $.escaped_character,
      seq('(', $._expr, ')'),
      $._expr_atom_no_brace,

      // As a string enclosed in braces, without substitutions.
      $.braced_word_simple,
    ),

    // An unbraced condition is one word, read with the same tokens as any
    // other word so that a form the shape does not cover can still be read
    // as an ordinary command: a bare word in it is labelled a number.
    expr: $ => choice(
      seq('{', $._expr, '}'),
      alias($.simple_word, $.number),
      $.command_substitution,
      $.quoted_word,
      $.variable_substitution,
    ),

    unary_expr: $ => prec.left(PREC.unary, seq(choice('-', '+', '~', '!'), $._expr)),

    binop_expr: $ => choice(
      prec.right(PREC.exp, seq($._expr, '**', $._expr)),
      prec.left(PREC.muldiv, seq($._expr, choice('/', '*', '%'), $._expr)),
      prec.left(PREC.addsub, seq($._expr, choice('+', '-'), $._expr)),
      prec.left(PREC.shift, seq($._expr, choice('<<', '>>'), $._expr)),
      prec.left(PREC.compare, seq($._expr, choice('>', '<', '>=', '<='), $._expr)),
      prec.left(PREC.equal_bool, seq($._expr, choice('==', '!='), $._expr)),
      prec.left(PREC.equal_string, seq($._expr, choice('eq', 'ne'), $._expr)),
      prec.left(PREC.contain, seq($._expr, choice('in', 'ni'), $._expr)),
      prec.left(PREC.and_bit, seq($._expr, '&', $._expr)),
      prec.left(PREC.xor_bit, seq($._expr, '^', $._expr)),
      prec.left(PREC.or_bit, seq($._expr, '|', $._expr)),
      prec.left(PREC.and_logical, seq($._expr, '&&', $._expr)),
      prec.left(PREC.or_logical, seq($._expr, '||', $._expr)),
    ),

    ternary_expr: $ => prec.right(PREC.ternary, seq($._expr, '?', $._expr, ':', $._expr)),

    elseif: $ => seq(
      'elseif',
      field('condition', $.expr),
      optional('then'),
      field('consequence', $._word),
    ),

    else: $ => seq(
      'else',
      field('consequence', $._word),
    ),

    if: $ => seq(
      'if',
      field('condition', $.expr),
      optional('then'),
      field('consequence', $._word),
      repeat(field('alternative', $.elseif)),
      optional(field('alternative', choice($.else, alias($._implicit_else, $.else)))),
    ),

    // `if c b1 b2`: the `else` is optional.
    _implicit_else: $ => field('consequence', $._word),

    _conditional: $ => choice(
      $.if,
      $.else,
      $.elseif,
    ),

    // catch script ?varName?
    catch: $ => seq(
      'catch',
      $._word,
      optional($._concat_word),
    ),

    quoted_word: $ => seq(
      '"',
      repeat(choice(
        $.variable_substitution,
        $._quoted_word_content,
        $.command_substitution,
        $.escaped_character,
        $._dollar_literal,
      )),
      '"',
    ),

    escaped_character: _ => /\\./,

    _quoted_word_content: _ => token(prec(-1, /[^$\\\["]+/)),

    command_substitution: $ => seq(
      '[',
      repeat($._terminator),
      optional(seq(
        $._command,
        repeat(seq(repeat1($._terminator), $._command)),
        repeat($._terminator),
      )),
      ']',
    ),

    simple_word: _ => new RegExp(`[^${WORD_STOP}"()][^${WORD_STOP}()]*`),
  },
});
