/**
 * @file Tree-sitter grammar for Crystal.
 *
 * mep's own grammar, written against the Crystal language reference
 * (crystal-lang.org/reference: syntax and semantics, literals, operators,
 * macros, C bindings) and the compiler's hand-written lexer and parser
 * (src/compiler/crystal/syntax/lexer.cr and parser.cr).
 *
 * Its node names, fields and tree shapes are the ones
 * crystal-lang-tools/tree-sitter-crystal established (the grammar mep loaded
 * dynamically before this one), so queries written for it keep working. So
 * is the order of the rules: where two readings survive to the same point,
 * tree-sitter keeps the one whose nodes are numbered lower, and declaration
 * order sets the numbering. Keep new rules after the ones they could
 * compete with. See README.md.
 *
 * Crystal's lexer depends on what the parser expects next far more than a
 * context-free lexer allows: whether `{` opens a block, a tuple or a tuple
 * type; whether `/` divides or starts a regex; whether `-x` is unary;
 * whether `if` is a statement or a suffix; where a heredoc body, a
 * %-literal or a macro body ends. Those tokens are read by the external
 * scanner, src/scanner.c.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// lexer.cr: a constant starts with an uppercase (or titlecase) letter, an
// identifier with a lowercase letter, `_` or any non-ASCII character.
const CONST_START = /[\p{Uppercase_Letter}\p{Titlecase_Letter}]/u;
const IDENT_START = /[a-z_\u{00a0}-\u{10ffff}]/u;
const IDENT_PART = /[0-9A-Za-z_\u{00a0}-\u{10ffff}]/u;

// The operators that are method names: what may follow `def`, `:` (an
// operator symbol) or `.` (an operator call). `&&`, `||`, `..` and the
// assignments are not methods.
const OPERATOR_METHODS = [
  '+', '-', '*', '/', '//', '%', '&', '|', '^', '**', '>>', '<<',
  '==', '!=', '<', '<=', '>', '>=', '<=>', '===',
  '[]', '[]?', '[]=',
  '!', '~', '!~', '=~',
  '&+', '&-', '&*', '&**',
];

// The combined assignments (reference: "Operators", "Combined
// assignments").
const ASSIGNMENT_OPERATORS = [
  '+=', '&+=', '-=', '&-=', '*=', '&*=', '/=', '//=', '%=',
  '|=', '&=', '^=', '**=', '<<=', '>>=', '||=', '&&=',
];

// Keywords that open a construct closed by `end`, as seen inside a macro
// body (which is text until the matching `end`): lexer.cr's
// next_macro_token keeps a nesting count with these.
const MACRO_NESTING_KEYWORDS = [
  /abstract\sclass/, /abstract\sstruct/,
  'annotation', 'begin', 'case', 'class', 'def', 'do', 'enum', 'fun', 'if',
  'lib', 'macro', 'module', 'select', 'struct', 'union', 'unless', 'until',
  'while',
];

const commaSep1 = (rule) => seq(rule, repeat(seq(',', rule)));

// A comma-separated list that may end with a comma.
const commaList1 = (rule) => seq(rule, repeat(seq(',', rule)), optional(','));

// `item ; item \n item`: items separated by terminators, stray `;`
// allowed, the last terminator optional.
const terminated = ($, item) => choice(
  seq(
    repeat1(choice(seq(item, $._terminator), prec(-1, ';'))),
    optional(item),
  ),
  item,
);

// The statements of a body, as an `expressions` node (absent when empty).
const body = ($) => field('body', seq(optional(alias($._statements, $.expressions))));

// `name : Type` in a parameter, field or signature; the colon must have
// space on both sides (parser.cr: check_space before parsing the type).
const typeField = ($) => field('type', seq($._type_field_separator, $._bare_type));

// A binary operator is a method call: `a + b` is `a.+(b)`, so its node is
// a call with the left operand as receiver and the right as the argument.
const binaryCall = ($, assoc, level, operator) => assoc(level, seq(
  field('receiver', $._expression),
  field('method', alias(operator, $.operator)),
  field('arguments', alias($._expression, $.argument_list)),
));

const unaryCall = ($, operator) => prec('unary_operator', seq(
  field('method', alias(operator, $.operator)),
  field('receiver', $._expression),
));

// Anything that can be passed as a call argument.
const argument = ($) => choice($._expression, $.splat, $.double_splat, $.named_expr, $.out);

// The keyword-and-parenthesized-type forms: sizeof(T), alignof(T), ...
const typeIntrinsic = (keyword, $) => seq(keyword, '(', $._bare_type, ')');

// An optional sign glued to a number literal: `-1` is one integer token.
const numberSign = ($) => choice(alias($.unary_minus, '-'), alias($.unary_plus, '+'));

// The proc type's return: a type when one follows (dynamic precedence
// prefers it), nothing otherwise (`-> Nil`, `->`).
const procReturn = ($) => optional(prec.dynamic(10, field('return', $._type)));

module.exports = grammar({
  name: 'crystal',

  // The external scanner's tokens, in src/scanner.c's TokenType order.
  externals: $ => [
    $._line_break,
    $._line_continuation,

    // `{` is a brace block, a hash or tuple, a named tuple, a tuple type or
    // a named tuple type, decided by context and lookahead.
    $._start_of_brace_block,
    $._start_of_hash_or_tuple,
    $._start_of_named_tuple,
    $._start_of_tuple_type,
    $._start_of_named_tuple_type,

    // `[` indexing (as opposed to an array literal argument).
    $._start_of_index_operator,

    // A zero-width token ending `with x` before its `yield`, so that
    // `with foo yield yield` reads.
    $._end_of_with_expression,

    // Unary versus binary: decided by the spaces around the operator.
    $.unary_plus,
    $.unary_minus,
    $.binary_plus,
    $.binary_minus,

    $.unary_wrapping_plus,
    $.unary_wrapping_minus,
    $.binary_wrapping_plus,
    $.binary_wrapping_minus,

    $._pointer_star,
    $._unary_star,
    $._binary_star,
    $._unary_double_star,
    $._binary_double_star,

    $._block_ampersand,
    $.binary_ampersand,

    $._beginless_range_operator,

    // `/` divides or starts a regex.
    $._regex_start,
    $._binary_slash,
    $._binary_double_slash,

    // `if`, `unless`, `rescue` and `ensure` start a statement or are a
    // suffix on one.
    $._regular_if_keyword,
    $._modifier_if_keyword,

    $._regular_unless_keyword,
    $._modifier_unless_keyword,

    $._regular_rescue_keyword,
    $._modifier_rescue_keyword,

    $._regular_ensure_keyword,
    $._modifier_ensure_keyword,

    $._modulo_operator,

    $._start_of_symbol,
    $.unquoted_symbol_content,

    // The ` : ` before a parameter's or a method's type. A scanner token
    // because a literal with leading whitespace in the grammar interacts
    // badly with whitespace extras (tree-sitter issue 4091).
    $._type_field_colon,

    $._string_literal_start,
    $._delimited_string_contents,
    $._string_literal_end,

    $._command_literal_start,
    $._command_literal_end,

    $._string_percent_literal_start,
    $._command_percent_literal_start,
    $._string_array_percent_literal_start,
    $._symbol_array_percent_literal_start,
    $._regex_percent_literal_start,
    $._percent_literal_end,
    $._delimited_array_element_start,
    $._delimited_array_element_end,

    $.heredoc_start,
    $._heredoc_body_start,
    $.heredoc_content,
    $.heredoc_end,

    $.regex_modifier,

    // Zero-width: resets the scanner's macro-text state at a macro body.
    $._macro_start,

    // `{%` when the keyword inside is `end`, `else` or `elsif`: the parser
    // needs to know which before it reads the keyword.
    $._macro_delimiter_end,
    $._macro_delimiter_else,
    $._macro_delimiter_elsif,

    $.macro_content,
    $.macro_content_nesting,

    // Never produced: their validity tells the scanner where it is.
    $._start_of_parenless_args,
    $._end_of_range,
    $._start_of_macro_var_exps,

    // Never in a rule: valid only while tree-sitter recovers from an error.
    $._error_recovery,
  ],

  extras: $ => [
    /\s/,
    $._line_continuation,
    $.loc_pragma_push,
    $.loc_pragma_pop,
    $.loc_pragma_location,
    $.comment,
    $.heredoc_body,
  ],

  word: $ => $.identifier,

  precedences: $ => [
    // The reference's operator precedence table, tightest first, then the
    // constructs that bind looser than any operator.
    [
      'index_operator',
      'dot_operator',
      'unary_operator',
      'exponential_operator',
      'multiplicative_operator',
      'additive_operator',
      'shift_operator',
      'binary_and_operator',
      'binary_or_operator',
      'equality_operator',
      'comparison_operator',
      'logical_and_operator',
      'logical_or_operator',
      'range_operator',
      'ternary_operator',
      'block_ampersand',
      'assignment_operator',
      'splat_operator',
      $.named_expr,
      'comma',
    ],

    // Types: suffixes (`T?`, `T*`, `T[4]`, `T.class`) bind tighter than
    // `|`, which binds tighter than splats and proc arrows.
    ['atomic_type', 'union_type', 'splat_type'],
    ['atomic_type', 'union_type', 'proc_type'],
    ['atomic_type', 'union_type', $._splattable_type],

    // `*a = b` is `(*a) = b`.
    [$.lhs_splat, 'assignment_operator'],

    // A brace block goes to the innermost call: `a b { 1 }` is
    // `a(b { 1 })`; a do-block to the outermost: `a b do 1 end` is
    // `a(b) do 1 end` (reference: "Blocks and Procs", precedence).
    ['brace_block_call', $._expression],
    [$._expression, 'do_end_block_call'],
    ['brace_block_call', 'no_block_call'],
    ['no_block_call', 'do_end_block_call'],

    // `a b &c` is `a(b(&c))`; `a b! &c` is `a(b!(&c))`.
    ['ampersand_block_call', $._expression],
    ['ampersand_block_call', 'no_block_call'],

    // `[] of A | B` is `[] of (A | B)`, and `[[] of Int32, Int32 -> String]`
    // reads the proc type inside.
    [$.union_type, $.array],
    [$.proc_type, $.array],
    // `{} of K => A | B` is `{} of K => (A | B)`.
    [$.union_type, $.hash],

    // `foo ? a : Int32` is a ternary, not a type declaration of `a`.
    [$._expression, $.type_declaration],
    // `fun foo() end`: `()` is an empty parameter list, not a nil body.
    [$.top_level_fun_def, $.empty_parens],

    // `a &.b.c` is `a(&.b.c)`, not `a(&.b).c`; implicit-object calls chain
    // before they are wrapped.
    [$.implicit_object_call_chainable, $.implicit_object_call_unchainable, 'block_ampersand'],
    [$.implicit_object_call_chainable, $._implicit_object_call],

    // `x.as(T)`: the parentheses belong to the pseudo-method.
    [$.pseudo_call_argument_list, $._parenthesized_type],
  ],

  conflicts: $ => [
    // `{ {} of A => B,` continues as `{ {} of A => B, 77 }` or as
    // `{ {} of A => B, C -> D }` (a proc type `Proc(B, C, D)`).
    [$._bare_type, $.proc_type],

    // `-> : (A,` continues as `(A,) -> do`, `(A, B ->) do` or
    // `(A, B) -> do`.
    [$._bare_type, $.proc_type, $.parenthesized_proc_type],

    // Where a proc type ends is undecidable at `def foo : Int32 -> (`:
    // `( Int32 )` may be its return type or a parenthesized body. The
    // scanner's type lookahead decides it where it is consulted (parser.cr
    // does the same, imperfectly); GLR covers the rest.
    [$.proc_type],
    [$.no_args_proc_type],
    [$.parenthesized_proc_type],

    // In a lib, `fun bar` may end, or continue on the next line with
    // `(UInt32) : UInt32`.
    [$.fun_def],

    // `foo ? a! : Int32` calls `a!`; `a! : Int32` alone declares it.
    [$.call, $.type_declaration],
  ],

  rules: {
    expressions: $ => seq(
      optional($._statements),
    ),

    // ---- macro bodies ------------------------------------------------------
    // A `macro` definition's body is text with `{{ }}` and `{% %}` inside,
    // ending at the `end` that balances the keywords it opened.

    _macro_def_literal_content: $ => choice(
      alias($.string, $.macro_content),
      alias($.macro_content_nesting, $.macro_content),
      seq(
        alias(choice(...MACRO_NESTING_KEYWORDS), $.macro_content),
        repeat($._macro_def_content),
        alias('end', $.macro_content),
      ),
    ),

    _macro_literal_content: $ => choice(
      alias($.string, $.macro_content),
      $.macro_content,
    ),

    // ---- statements --------------------------------------------------------

    _terminator: $ => choice($._line_break, ';'),

    _statements: $ => terminated($, $._statement),

    _lib_statements: $ => terminated($, $._lib_statement),

    _enum_statements: $ => terminated($, $._enum_statement),

    // Statements with no value of their own, allowed where a value is
    // used dynamically: `c = [b if a]` is invalid, `"#{b if a}"` is not.
    _inline_statement: $ => choice(
      $.modifier_if,
      $.modifier_unless,
      $.modifier_rescue,
      $.modifier_ensure,

      $.return,
      $.next,
      $.break,
    ),

    _statement: $ => choice(
      $._expression,
      $._inline_statement,
      $.const_assign,
      alias($.multi_assign, $.assign),
      $.annotation,
      $.annotation_def,
      $.module_def,
      $.class_def,
      $.struct_def,
      $.enum_def,
      $.lib_def,
      $.alias,
      $.method_def,
      $.abstract_method_def,
      $.macro_def,
      alias($.top_level_fun_def, $.fun_def),
      $.visibility_modifier,
      $.require,
      $.include,
      $.extend,
    ),

    // What a `lib` body holds (reference: "C bindings").
    _lib_statement: $ => choice(
      $._macro_node,
      $.alias,
      $.fun_def,
      $.type_def,
      $.c_struct_def,
      $.union_def,
      $.enum_def,
      $.global_var,
      $.const_assign,
      $.annotation,
      $.visibility_modifier,
    ),

    // What an `enum` body holds: members, methods, macros, class vars.
    _enum_statement: $ => choice(
      $._macro_node,
      $.constant,
      $.const_assign,
      $.method_def,
      $.macro_def,
      $.class_var,
      alias($.class_var_assign, $.assign),
      $.annotation,
      $.visibility_modifier,
    ),

    // `(a; b)`: several statements as one expression.
    parenthesized_expressions: $ => seq(
      '(', $._statements, ')',
    ),

    _expression: $ => choice(
      $._macro_node,

      // Literals
      $.nil,
      $.true,
      $.false,
      $.integer,
      $.float,
      $.char,
      $.array,
      $.hash,
      $.string,
      $.chained_string,
      alias($.string_percent_literal, $.string),
      alias($.string_array_percent_literal, $.array),
      alias($.operator_symbol, $.symbol),
      alias($.unquoted_symbol, $.symbol),
      alias($.quoted_symbol, $.symbol),
      alias($.symbol_array_percent_literal, $.array),
      $.heredoc_start,
      $.range,
      alias($.beginless_range, $.range),
      $.tuple,
      $.named_tuple,
      $.proc,
      $.method_proc,
      $.command,
      alias($.command_percent_literal, $.command),
      $.regex,
      alias($.regex_percent_literal, $.regex),

      // Groupings
      alias($.empty_parens, $.nil),
      alias($.parenthesized_expressions, $.expressions),
      $.begin,

      // Names
      $.self,
      $.constant,
      $.generic_instance_type,
      $.nilable_constant,
      $.pseudo_constant,
      $.special_variable,
      alias($.global_match_data_index, $.special_variable),
      $.identifier,
      $.instance_var,
      $.class_var,
      $.macro_var,
      $.type_declaration,

      // Control flow
      $.while,
      $.until,
      $.if,
      $.unless,
      $.conditional,
      $.case,
      alias($.exhaustive_case, $.case),
      $.select,

      // Calls, operators included (an operator is a method)
      $.call,

      alias($.additive_operator, $.call),
      alias($.unary_additive_operator, $.call),
      alias($.multiplicative_operator, $.call),
      alias($.exponential_operator, $.call),
      alias($.shift_operator, $.call),
      alias($.complement_operator, $.call),
      alias($.binary_and_operator, $.call),
      alias($.binary_or_operator, $.call),
      alias($.equality_operator, $.call),
      alias($.comparison_operator, $.call),
      alias($.index_operator, $.index_call),
      alias($.pseudo_call, $.call),
      $.index_call,
      $.array_like,
      $.hash_like,
      $.assign,
      alias($.uninitialized_assign, $.assign),
      alias($.operator_assign, $.op_assign),

      // Logical operators (not methods)
      $.not,
      $.and,
      $.or,

      $.asm,

      // Keywords that look like methods
      $.yield,
      $.typeof,
      $.pointerof,
      $.sizeof,
      $.instance_sizeof,
      $.alignof,
      $.instance_alignof,
      $.offsetof,
    ),

    comment: $ => /#.*/,

    // ---- literals ----------------------------------------------------------

    empty_parens: $ => seq('(', ')'),

    nil: $ => 'nil',

    true: $ => 'true',
    false: $ => 'false',

    // Reference: "Integers". Underscores anywhere after the first digit; a
    // leading zero only alone or before `_`; 0b, 0o and 0x prefixes; a
    // u8..u128 / i8..i128 suffix.
    integer: $ => {
      const digits = seq(
        choice(
          seq('0b', repeat(/[01_]/)),
          seq('0o', repeat(/[0-7_]/)),
          seq('0x', repeat(/[0-9a-fA-F_]/)),
          choice('0', seq('0_', repeat(/[0-9_]/))),
          seq(/[1-9]/, repeat(/[0-9_]/)),
        ),
        optional(choice('u8', 'u16', 'u32', 'u64', 'u128', 'i8', 'i16', 'i32', 'i64', 'i128')),
      );
      return choice(
        seq(numberSign($), token.immediate(digits)),
        token(digits),
      );
    },

    // Reference: "Floats". At least one of a fraction, an exponent or an
    // f32/f64 suffix.
    float: $ => {
      const whole = choice(
        seq(/[1-9]/, repeat(/[0-9_]/)),
        choice('0', seq('0_', repeat(/[0-9_]/))),
      );
      const fraction = seq(/\.[0-9]/, repeat(/[0-9_]/));
      const exponent = seq(/[eE]/, optional(seq(optional(/[+-]/), repeat1(/[0-9_]/))));
      const suffix = choice('f32', 'f64');
      const digits = choice(
        seq(whole, fraction, optional(exponent), optional(suffix)),
        seq(whole, optional(fraction), exponent, optional(suffix)),
        seq(whole, optional(fraction), optional(exponent), suffix),
      );
      return choice(
        seq(numberSign($), token.immediate(digits)),
        token(digits),
      );
    },

    // Reference: "Char". One character or one escape between quotes.
    char: $ => seq(
      '\'',
      choice(
        alias(token.immediate(prec(1, /[^\\]/)), $.literal_content),
        alias($.char_escape_sequence, $.escape_sequence),
      ),
      token.immediate('\''),
    ),

    char_escape_sequence: $ => token.immediate(seq('\\', choice(
      '0', '\\', '\'', 'a', 'b', 'e', 'f', 'n', 'r', 't', 'v',
      seq('u', choice(/[0-9a-fA-F]{4}/, /\{[0-9a-fA-F]{1,6}\}/)),
    ))),

    // Reference: "String". The quotes and the text between escapes and
    // interpolations come from the scanner, which tracks the delimiter.
    string: $ => seq(
      alias($._string_literal_start, '"'),
      optional($._string_literal_content),
      alias($._string_literal_end, '"'),
    ),

    _string_literal_content: $ => repeat1(choice(
      $._line_continuation,
      alias($._delimited_string_contents, $.literal_content),
      alias($.string_escape_sequence, $.escape_sequence),
      alias($.ignored_backslash, $.escape_sequence),
      $.interpolation,
    )),

    // `"a" \` newline `"b"`: strings joined across a line continuation (the
    // continuation is an extra, so it can't appear here: tree-sitter issue
    // 1950).
    chained_string: $ => seq($.string, repeat1($.string)),

    // A backslash before a character with no escape meaning stands for
    // that character.
    ignored_backslash: $ => token.immediate(seq('\\', /[^\n\\"abefnrtuvx0-7]/)),

    string_escape_sequence: $ => {
      const codepoint = /[0-9a-fA-F]{1,6}/;
      return token.immediate(seq('\\', choice(
        choice('\\', '"', 'a', 'b', 'e', 'f', 'n', 'r', 't', 'v'),
        /[0-7]{1,3}/,
        seq('x', /[0-9a-fA-F]{2}/),
        seq('u', choice(
          /[0-9a-fA-F]{4}/,
          seq('{', codepoint, repeat(seq(' ', codepoint)), '}'),
        )),
      )));
    },

    interpolation: $ => seq(
      token(prec(1, '#{')), choice($._expression, $._inline_statement), '}',
    ),

    // Reference: "Percent string literals": %(...), %Q, %q; the delimiters
    // ( [ { < nest, | doesn't.
    string_percent_literal: $ => seq(
      alias($._string_percent_literal_start, '"'),
      optional($._string_percent_literal_content),
      alias($._percent_literal_end, '"'),
    ),

    _string_percent_literal_content: $ => repeat1(choice(
      alias($._delimited_string_contents, $.literal_content),
      $.interpolation,
      alias($.string_escape_sequence, $.escape_sequence),
      alias($.ignored_backslash, $.escape_sequence),
    )),

    // %w(...) and %i(...): whitespace-separated words.
    string_array_percent_literal: $ => seq(
      alias($._string_array_percent_literal_start, '['),
      repeat(alias($.percent_literal_array_word, $.string)),
      alias($._percent_literal_end, ']'),
    ),

    symbol_array_percent_literal: $ => seq(
      alias($._symbol_array_percent_literal_start, '['),
      repeat(alias($.percent_literal_array_word, $.symbol)),
      alias($._percent_literal_end, ']'),
    ),

    percent_literal_array_word: $ => seq(
      $._delimited_array_element_start,
      repeat(choice(
        alias($._delimited_string_contents, $.literal_content),
        alias($.percent_array_escape_sequence, $.escape_sequence),
      )),
      $._delimited_array_element_end,
    ),

    // In %w/%i only whitespace and the closing delimiter can be escaped.
    percent_array_escape_sequence: $ => token.immediate(/\\[\s})\]>|]/),

    // Reference: "Heredoc". `<<-ID` is an expression (heredoc_start); its
    // body starts on the next line and is an extra wherever it lands.
    heredoc_body: $ => seq(
      $._heredoc_body_start,
      repeat(choice(
        alias($.heredoc_content, $.literal_content),
        $.interpolation,
        seq(/\s*/, alias($.string_escape_sequence, $.escape_sequence)),
        seq(/\s*/, alias($.ignored_backslash, $.escape_sequence)),
        $._line_continuation,
      )),
      $.heredoc_end,
    ),

    // Reference: "Symbol". `:+`, `:[]=`, `:foo?`, `:"any text"`.
    operator_symbol: $ => seq(
      alias($._start_of_symbol, ':'),
      alias(token.immediate(choice(...OPERATOR_METHODS)), $.literal_content),
    ),

    unquoted_symbol: $ => seq(
      alias($._start_of_symbol, ':'),
      alias($.unquoted_symbol_content, $.literal_content),
    ),

    quoted_symbol: $ => seq(
      ':"',
      seq(
        repeat(choice(
          alias(token.immediate(prec(1, /[^\\"]+/)), $.literal_content),
          alias($.string_escape_sequence, $.escape_sequence),
          alias($.ignored_backslash, $.escape_sequence),
        )),
        token.immediate('"'),
      ),
    ),

    // Reference: "Command literal": `cmd` and %x(cmd).
    command: $ => seq(
      alias($._command_literal_start, '`'),
      optional($._string_literal_content),
      alias($._command_literal_end, '`'),
    ),

    command_percent_literal: $ => seq(
      alias($._command_percent_literal_start, '`'),
      optional($._string_percent_literal_content),
      alias($._percent_literal_end, '`'),
    ),

    // Reference: "Regex": /.../imx and %r(...)imx.
    regex: $ => seq(
      alias($._regex_start, '/'),
      optional($._regex_literal_content),
      token.immediate('/'),
      optional($.regex_modifier),
    ),

    _regex_literal_content: $ => repeat1(choice(
      $.interpolation,
      alias($.regex_literal_content, $.literal_content),
    )),

    regex_literal_content: $ => prec.right(repeat1(choice(
      token.immediate(prec(1, /[^\\/]/)),
      token.immediate(/\\./),
      $._line_continuation,
    ))),

    regex_percent_literal: $ => seq(
      alias($._regex_percent_literal_start, '/'),
      optional($._regex_percent_literal_content),
      alias($._percent_literal_end, '/'),
      optional($.regex_modifier),
    ),

    _regex_percent_literal_content: $ => repeat1(choice(
      $.interpolation,
      alias($._delimited_string_contents, $.literal_content),
    )),

    // Reference: "Array": [1, 2], [] of T, [1] of T.
    array: $ => {
      const ofType = field('of', seq('of', $._bare_type));
      return choice(
        seq('[', commaList1($._expression), ']', optional(ofType)),
        seq('[', ']', ofType),
      );
    },

    // Reference: "Hash": {k => v}, {} of K => V.
    hash: $ => {
      const ofType = seq('of', field('of_key', $._bare_type), '=>', field('of_value', $._bare_type));
      return choice(
        seq(alias($._start_of_hash_or_tuple, '{'), commaList1($.hash_entry), '}', optional(ofType)),
        seq(alias($._start_of_hash_or_tuple, '{'), '}', ofType),
      );
    },

    hash_entry: $ => seq($._expression, '=>', $._expression),

    tuple: $ => seq(
      alias($._start_of_hash_or_tuple, '{'),
      commaList1($._expression),
      '}',
    ),

    named_tuple: $ => seq(
      alias($._start_of_named_tuple, '{'),
      commaList1($.named_expr),
      '}',
    ),

    // Reference: "Range": a..b, a...b, endless a.. and beginless ..b.
    range: $ => prec.left('range_operator', seq(
      field('begin', $._expression),
      field('operator', alias(choice('..', '...'), $.operator)),
      optional($._end_of_range),
      optional(field('end', $._expression)),
    )),

    beginless_range: $ => prec.left('range_operator', seq(
      field('operator', alias($._beginless_range_operator, $.operator)),
      optional($._end_of_range),
      optional(field('end', $._expression)),
    )),

    // Reference: "Proc literal": ->(x : Int32) : Int32 { x }.
    proc: $ => seq(
      '->',
      optional(seq('(', field('params', optional(alias($.proc_param_list, $.param_list))), ')')),
      optional(field('type', seq(/:\s/, $._bare_type))),
      field('block', choice(
        alias($.do_end_block, $.block),
        alias($.brace_block, $.block),
      )),
    ),

    proc_param_list: $ => commaList1($.param),

    // A proc from a method: ->foo, ->obj.bar(Int32), ->::baz.
    method_proc: $ => {
      const methodName = choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        alias($.identifier_assign, $.identifier),
      );
      return prec.right(seq(
        '->',
        choice(
          seq(
            field('receiver', choice($.identifier, $.instance_var, $.class_var, $.self, $.constant)),
            '.',
            field('method', choice(
              $.identifier,
              alias($.identifier_method_call, $.identifier),
              alias($.identifier_assign, $.identifier),
              alias($._operator_token, $.operator),
            )),
          ),
          choice($._global_method, field('method', methodName)),
        ),
        optional(seq('(', field('params', alias($.type_instance_param_list, $.param_list)), ')')),
      ));
    },

    // ---- definitions -------------------------------------------------------

    // Reference: "Annotations".
    annotation_def: $ => seq(
      'annotation',
      field('name', $.constant),
      repeat($._terminator),
      'end',
    ),

    annotation: $ => seq(
      '@[',
      $.constant,
      optional(field('arguments', alias($.annotation_argument_list, $.argument_list))),
      ']',
    ),

    // A call's argument list, but `(` needn't touch the name.
    annotation_argument_list: $ => seq(
      '(',
      optional(commaList1(argument($))),
      ')',
    ),

    module_def: $ => seq(
      'module',
      field('name', choice($.constant, $.generic_type)),
      body($),
      'end',
    ),

    class_def: $ => seq(
      optional('abstract'),
      'class',
      field('name', choice($.constant, $.generic_type)),
      optional(seq('<', field('superclass', choice($.constant, $.generic_instance_type)))),
      body($),
      'end',
    ),

    struct_def: $ => seq(
      optional('abstract'),
      'struct',
      field('name', choice($.constant, $.generic_type)),
      optional(seq('<', field('superclass', choice($.constant, $.generic_instance_type)))),
      body($),
      'end',
    ),

    enum_def: $ => seq(
      'enum',
      field('name', $.constant),
      optional(field('type', seq(/:\s/, $._bare_type))),
      field('body', seq(optional(alias($._enum_statements, $.expressions)))),
      'end',
    ),

    // Reference: "C bindings": lib, fun, type, struct, union, global vars.
    lib_def: $ => seq(
      'lib',
      field('name', choice($.constant, $.generic_type)),
      field('body', seq(optional(alias($._lib_statements, $.expressions)))),
      'end',
    ),

    // `fun name = real_name(params) : Type` at top level, with a body.
    _base_fun_def: $ => prec.right(seq(
      'fun',
      field('name', choice($.identifier, alias($.identifier_method_call, $.identifier))),
      optional(seq('=', field('real_name', choice($.identifier, $.constant, $.string)))),
      optional(seq('(', optional(field('params', alias($.fun_param_list, $.param_list))), ')')),
      optional(typeField($)),
    )),

    top_level_fun_def: $ => seq(
      $._base_fun_def,
      body($),
      'end',
    ),

    // `fun` inside a lib: a declaration, whose parameters may be bare types.
    fun_def: $ => seq(
      'fun',
      field('name', choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        $.constant,
      )),
      optional(seq('=', field('real_name', choice($.identifier, $.constant, $.string)))),
      optional(seq(
        optional($._line_break),
        '(',
        optional(field('params', alias($.fun_type_param_list, $.param_list))),
        ')',
      )),
      optional(typeField($)),
    ),

    // A trailing `, ...` is a C variadic.
    fun_param_list: $ => seq(
      commaSep1($.fun_param),
      optional(seq(',', optional('...'))),
    ),

    fun_type_param_list: $ => seq(
      commaSep1(choice($.fun_param, $._type)),
      optional(seq(',', optional('...'))),
    ),

    fun_param: $ => seq(
      field('name', choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        $.constant,
      )),
      typeField($),
    ),

    _type_field_separator: $ => alias($._type_field_colon, ':'),

    type_def: $ => seq(
      'type',
      field('name', $.constant),
      '=',
      field('type', $._bare_type),
    ),

    c_struct_def: $ => seq(
      'struct',
      field('name', $.constant),
      field('body', alias($._c_struct_expressions, $.expressions)),
      'end',
    ),

    _c_struct_expressions: $ => terminated($, $._c_struct_expression),

    _c_struct_expression: $ => choice(
      $._macro_node,
      $.include,
      $.c_struct_fields,
    ),

    // `a, b : Int32`
    c_struct_fields: $ => seq(
      field('name', commaSep1($.identifier)),
      $._type_field_separator,
      field('type', $._bare_type),
    ),

    union_def: $ => seq(
      'union',
      field('name', $.constant),
      field('body', alias($._union_expressions, $.expressions)),
      'end',
    ),

    _union_expressions: $ => terminated($, $._union_expression),

    _union_expression: $ => choice(
      $.include,
      $.union_fields,
    ),

    union_fields: $ => seq(
      field('name', commaSep1($.identifier)),
      $._type_field_separator,
      field('type', $._bare_type),
    ),

    // `$errno = real_name : Int32`
    global_var: $ => seq(
      field('name', seq('$', $.identifier)),
      optional(seq('=', field('real_name', choice($.identifier, $.constant)))),
      typeField($),
    ),

    _operator_token: $ => choice(...OPERATOR_METHODS),

    // Reference: "Methods and instance variables", "Type restrictions",
    // "forall". `def self.name`, `def Foo.name`, setter and operator names.
    _base_method_def: $ => prec.right(seq(
      'def',
      optional(field('class', seq(choice($.constant, $.self), '.'))),
      field('name', choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        alias($.identifier_assign, $.identifier),
        alias($._operator_token, $.operator),
        alias('`', $.operator),
      )),
      optional(seq('(', field('params', optional($.param_list)), ')')),
      optional(typeField($)),
      optional(field('forall', $.forall)),
    )),

    method_def: $ => seq(
      $._base_method_def,
      optional($._terminator),
      body($),
      optional($._rescue_else_ensure),
      'end',
    ),

    abstract_method_def: $ => prec.left(seq(
      'abstract',
      $._base_method_def,
      optional($._terminator),
    )),

    // Reference: "Macros".
    _macro_signature: $ => seq(
      'macro',
      field('name', choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        alias($._operator_token, $.operator),
        alias('`', $.operator),
      )),
      choice(
        seq('(', field('params', optional($.param_list)), ')'),
        $._terminator,
      ),
    ),

    macro_def: $ => seq(
      $._macro_signature,
      $._macro_start,
      field('body', optional(alias(repeat($._macro_def_content), $.expressions))),
      'end',
    ),

    include: $ => seq('include', choice($.constant, $.self, $.generic_instance_type)),

    extend: $ => seq('extend', choice($.constant, $.self, $.generic_instance_type)),

    forall: $ => seq('forall', commaSep1(alias($._constant_segment, $.constant))),

    // Splat and double-splat parameters may only appear in certain places
    // (reference: "Default parameter values, named arguments, splats,
    // tuples and overloading"); the grammar takes them anywhere and leaves
    // the rest to the compiler. A block parameter comes last.
    param_list: $ => {
      const param = choice($.param, $.splat_param, $.double_splat_param);
      return choice(
        seq(commaSep1(param), optional(seq(',', optional($.block_param)))),
        $.block_param,
      );
    },

    // `@[Ann] external_name internal_name : Type = default`
    param: $ => seq(
      repeat($.annotation),
      optional(field('extern_name', $.identifier)),
      field('name', choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        $.instance_var,
        $.class_var,
        $.macro_var,
      )),
      optional(typeField($)),
      optional(field('default', seq('=', $._expression))),
    ),

    splat_param: $ => seq(
      repeat($.annotation),
      '*',
      optional(field('name', choice($.identifier, $.instance_var, $.class_var, $.macro_var))),
      optional(typeField($)),
    ),

    double_splat_param: $ => seq(
      repeat($.annotation),
      '**',
      field('name', choice($.identifier, $.instance_var, $.class_var, $.macro_var)),
      optional(typeField($)),
    ),

    block_param: $ => seq(
      repeat($.annotation),
      '&',
      optional(field('name', choice($.identifier, $.instance_var, $.class_var, $.macro_var))),
      optional(field('type', seq(/:\s/, $._bare_type))),
    ),

    // ---- keyword expressions -----------------------------------------------

    _control_expressions: $ => choice(
      alias($.argument_list_with_parens, $.argument_list),
      alias($.argument_list_no_parens, $.argument_list),
    ),

    return: $ => seq('return', optional($._control_expressions)),

    next: $ => seq('next', optional($._control_expressions)),

    break: $ => seq('break', optional($._control_expressions)),

    // `with obj yield x`: the block runs with obj as default receiver.
    yield: $ => seq(
      optional(seq('with', field('with', $._expression), $._end_of_with_expression)),
      'yield',
      optional($._control_expressions),
    ),

    typeof: $ => seq('typeof', '(', commaList1($._expression), ')'),

    pointerof: $ => seq('pointerof', '(', $._expression, ')'),

    sizeof: $ => typeIntrinsic('sizeof', $),

    instance_sizeof: $ => typeIntrinsic('instance_sizeof', $),

    alignof: $ => typeIntrinsic('alignof', $),

    instance_alignof: $ => typeIntrinsic('instance_alignof', $),

    offsetof: $ => seq(
      'offsetof',
      '(',
      $._bare_type,
      ',',
      choice($.instance_var, $.integer),
      ')',
    ),

    // ---- names -------------------------------------------------------------

    _constant_segment: $ => token(seq(CONST_START, repeat(IDENT_PART))),

    // `Foo`, `Foo::Bar`, `::Foo`.
    constant: $ => prec.right(seq(
      optional('::'),
      $._constant_segment,
      repeat(seq('::', choice($._constant_segment))),
    )),

    // `Foo?` as an expression: the nilable type `Foo | Nil`.
    nilable_constant: $ => seq(
      choice($.constant, $.nilable_constant, $.generic_instance_type),
      token.immediate('?'),
    ),

    pseudo_constant: $ => choice('__LINE__', '__END_LINE__', '__FILE__', '__DIR__'),

    special_variable: $ => token(choice('$?', '$~')),

    // `$1`, `$2?`: the last match's groups.
    global_match_data_index: $ => token(choice(/\$[0-9]+/, /\$[0-9]+\?/)),

    identifier: $ => token(seq(IDENT_START, repeat(IDENT_PART))),
    identifier_method_call: $ => token(seq(IDENT_START, repeat(IDENT_PART), /[?!]/)),
    identifier_assign: $ => token(seq(IDENT_START, repeat(IDENT_PART), /[=]/)),

    instance_var: $ => token(seq('@', IDENT_START, repeat(IDENT_PART))),
    class_var: $ => token(seq('@@', IDENT_START, repeat(IDENT_PART))),

    // A macro's fresh variable: `%name`, `%name{key, ...}`.
    macro_var: $ => seq(
      field('name', alias(token(seq('%', IDENT_PART, repeat(IDENT_PART))), $.identifier)),
      optional(seq(
        optional($._start_of_macro_var_exps),
        token.immediate('{'),
        commaList1($._expression),
        '}',
      )),
    ),

    self: $ => 'self',

    // ---- types -------------------------------------------------------------
    // parser.cr's type grammar, roughly:
    //
    //   bare_proc_type     = proc_type | splat_type
    //   proc_type          = splat_type { "," splat_type } "->" [union_type]
    //   splat_type         = ["*"] union_type
    //   union_type         = atomic_suffix_type { "|" atomic_suffix_type }
    //   atomic_suffix_type = atomic_type [".class" | "?" | "*" | "**" | "[" type_arg "]"]
    //   type_arg           = number | sizeof | instance_sizeof | offsetof | union_type
    //   atomic_type        = "self" | "self?" | typeof | "_" | generic
    //                      | tuple_type | named_tuple_type | "->" union_type
    //                      | "(" bare_proc_type ")"
    //                      | "(" splat_type {"," splat_type} [","] ")" "->" [union_type]
    //   generic            = constant ["(" type args or named type args ")"]
    //   tuple_type         = "{" splat_type {"," splat_type} [","] "}"
    //   named_tuple_type   = "{" name ":" bare_proc_type {"," ...} [","] "}"

    // The whole type grammar.
    _bare_type: $ => choice(
      $.proc_type,
      $._splattable_type,
    ),

    // Every type but a bare proc type: what may appear where a splat may
    // (`{*T}` is a tuple type; `{A, B -> C}` is not).
    _splattable_type: $ => choice(
      $.double_splat_type,
      $.splat_type,
      $._type,
    ),

    _type: $ => choice(
      $._parenthesized_type,
      $.constant,
      $.generic_instance_type,
      $.union_type,
      $.tuple_type,
      $.named_tuple_type,
      alias($.no_args_proc_type, $.proc_type),
      alias($.parenthesized_proc_type, $.proc_type),
      $.class_type,
      $.underscore,
      $.nilable_type,
      $.pointer_type,
      $.self,
      $.typeof,
      $.static_array_type,
    ),

    // A number in type position: a StaticArray's size, UInt8[sizeof(T)].
    _numeric_type: $ => choice(
      $.integer,
      $.float,
      $.sizeof,
      $.instance_sizeof,
      $.alignof,
      $.instance_alignof,
      $.offsetof,
    ),

    class_type: $ => prec('atomic_type', seq($._type, '.', 'class')),

    union_type: $ => prec.right('union_type', seq(
      $._type,
      repeat1(prec.left('union_type', seq('|', $._type))),
    )),

    _parenthesized_type: $ => seq('(', $._bare_type, ')'),

    proc_type: $ => prec('proc_type', seq(
      commaSep1($._splattable_type),
      '->',
      procReturn($),
    )),

    no_args_proc_type: $ => prec('proc_type', seq(
      '->',
      procReturn($),
    )),

    parenthesized_proc_type: $ => prec('proc_type', seq(
      '(',
      choice(
        seq($._splattable_type, repeat1(seq(',', $._splattable_type)), optional(',')),
        seq($._bare_type, ','),
      ),
      ')',
      '->',
      procReturn($),
    )),

    splat_type: $ => prec('splat_type', seq('*', $._type)),

    double_splat_type: $ => prec('splat_type', seq(alias($._unary_double_star, '**'), $._type)),

    tuple_type: $ => seq(
      alias($._start_of_tuple_type, '{'),
      commaList1($._splattable_type),
      '}',
    ),

    named_type: $ => seq(
      field('name', choice(
        $.identifier,
        alias($._constant_segment, $.identifier),
        alias($.identifier_method_call, $.identifier),
        $.string,
        alias($.string_percent_literal, $.string),
      )),
      token.immediate(':'),
      $._bare_type,
    ),

    named_tuple_type: $ => seq(
      alias($._start_of_named_tuple_type, '{'),
      commaList1($.named_type),
      '}',
    ),

    // A generic definition's head, `class Foo(T, *U)`; its instances,
    // `Foo(Int32, String)`, are generic_instance_type.
    generic_type: $ => seq(
      $.constant,
      token.immediate('('),
      field('params', optional(alias($.type_param_list, $.param_list))),
      ')',
    ),

    type_param_splat: $ => seq('*', $.constant),

    type_param_list: $ => commaList1(choice(
      $.constant,
      $._numeric_type,
      alias($.type_param_splat, $.splat),
    )),

    generic_instance_type: $ => seq(
      $.constant,
      token.immediate('('),
      field('params', optional(alias($.type_instance_param_list, $.param_list))),
      ')',
    ),

    type_instance_param_list: $ => seq(
      choice(
        commaSep1(choice($._bare_type, $._numeric_type)),
        commaSep1($.named_type),
      ),
      optional(','),
    ),

    underscore: $ => '_',

    nilable_type: $ => prec('atomic_type', seq($._type, '?')),

    pointer_type: $ => prec('atomic_type', seq($._type, alias($._pointer_star, '*'))),

    static_array_type: $ => prec('atomic_type', seq(
      $._type, alias($._start_of_index_operator, '['), choice($.constant, $._numeric_type), ']',
    )),

    // ---- calls -------------------------------------------------------------

    _dot_call: $ => prec('dot_operator', seq(
      field('receiver', $._expression),
      '.',
      field('method', choice(
        $.identifier,
        $.constant,
        alias($.identifier_method_call, $.identifier),
        alias($._operator_token, $.operator),
        $.instance_var,
      )),
    )),

    bracket_argument_list: $ => commaList1(argument($)),

    // ---- macros ------------------------------------------------------------
    // Reference: "Macros": {{ expr }}, {% statements %}, and the {% if %},
    // {% unless %}, {% for %}, {% begin %} and {% verbatim do %} blocks,
    // whose bodies are text.

    _macro_node: $ => choice(
      $.macro_expression,
      $.macro_statement,
      $.macro_begin,
      $.macro_if,
      $.macro_unless,
      $.macro_for,
      $.macro_verbatim,
    ),

    macro_expression: $ => seq(
      '{{',
      choice($.splat, $.double_splat, $._expression, $._inline_statement),
      '}}',
    ),

    macro_statement: $ => seq(
      '{%',
      optional(alias($._statements, $.expressions)),
      '%}',
    ),

    _macro_begin_keyword: $ => seq('{%', 'begin', '%}'),

    _macro_end_keyword: $ => seq(alias($._macro_delimiter_end, '{%'), 'end', '%}'),

    _macro_else_keyword: $ => seq(alias($._macro_delimiter_else, '{%'), 'else', '%}'),

    _macro_verbatim_keyword: $ => seq('{%', 'verbatim', 'do', '%}'),

    _macro_if_cond: $ => seq(
      '{%',
      alias($._regular_if_keyword, 'if'),
      field('cond', $._expression),
      '%}',
    ),

    _macro_elsif_cond: $ => seq(
      alias($._macro_delimiter_elsif, '{%'),
      'elsif',
      field('cond', $._expression),
      '%}',
    ),

    _macro_unless_cond: $ => seq(
      '{%',
      alias($._regular_unless_keyword, 'unless'),
      field('cond', $._expression),
      '%}',
    ),

    _macro_for_expr: $ => {
      const variable = field('var', choice($.underscore, $.identifier));
      return seq(
        '{%',
        'for',
        commaSep1(variable),
        'in',
        field('cond', choice($._expression, $.splat, $.double_splat)),
        '%}',
      );
    },

    macro_begin: $ => seq(
      $._macro_begin_keyword,
      field('body', alias(repeat($._macro_content), $.expressions)),
      $._macro_end_keyword,
    ),

    macro_if: $ => seq(
      $._macro_if_cond,
      field('then', alias(repeat($._macro_content), $.expressions)),
      optional(field('else', choice($.macro_elsif, $.macro_else))),
      $._macro_end_keyword,
    ),

    macro_elsif: $ => seq(
      $._macro_elsif_cond,
      field('then', alias(repeat($._macro_content), $.expressions)),
      optional(field('else', choice($.macro_elsif, $.macro_else))),
    ),

    macro_else: $ => seq(
      $._macro_else_keyword,
      field('body', alias(repeat($._macro_content), $.expressions)),
    ),

    macro_unless: $ => seq(
      $._macro_unless_cond,
      field('then', alias(repeat($._macro_content), $.expressions)),
      optional(field('else', $.macro_else)),
      $._macro_end_keyword,
    ),

    macro_for: $ => seq(
      $._macro_for_expr,
      field('body', alias(repeat($._macro_content), $.expressions)),
      $._macro_end_keyword,
    ),

    macro_verbatim: $ => seq(
      $._macro_verbatim_keyword,
      field('body', alias(repeat($._macro_content), $.expressions)),
      $._macro_end_keyword,
    ),

    _macro_def_content: $ => choice(
      $._macro_node,
      $.macro_var,
      $._macro_def_literal_content,
      $._terminator,
    ),

    _macro_content: $ => choice(
      $._macro_node,
      $.macro_var,
      $._macro_literal_content,
      $._terminator,
    ),

    // ---- visibility, array-like and hash-like literals --------------------

    private: $ => 'private',
    protected: $ => 'protected',

    visibility_modifier: $ => seq(
      field('visibility', choice($.private, $.protected)),
      choice(
        $.call,
        $.module_def,
        $.class_def,
        $.struct_def,
        $.enum_def,
        $.lib_def,
        $.method_def,
        $.abstract_method_def,
        $.macro_def,
        $.const_assign,
        $.alias,
      ),
    ),

    // Reference: "Array-like type literal", `Set{1, 2}`.
    array_like: $ => seq(
      field('name', choice($.constant, $.generic_instance_type)),
      field('values', $.tuple),
    ),

    // Reference: "Hash-like type literal", `HTTP::Headers{"a" => "b"}`.
    hash_like: $ => seq(
      field('name', choice($.constant, $.generic_instance_type)),
      field('values', $.hash),
    ),

    // A bare identifier is a variable unless something makes it a call: a
    // receiver, a `?`/`!` suffix, parentheses, arguments or a block.
    call: $ => {
      const withReceiver = choice(
        $._dot_call,
        field('method', alias($.identifier_method_call, $.identifier)),
        $._global_method,
      );
      const bare = field('method', $.identifier);
      const args = field('arguments', choice(
        alias($.argument_list_with_parens, $.argument_list),
        alias($.argument_list_no_parens, $.argument_list),
      ));
      const argsWithBlock = field('arguments', choice(
        alias($.argument_list_with_parens_and_block, $.argument_list),
        alias($.argument_list_no_parens_with_block, $.argument_list),
      ));
      const braceBlock = field('block', alias($.brace_block, $.block));
      const doBlock = field('block', alias($.do_end_block, $.block));

      return choice(
        prec('no_block_call', seq(withReceiver, optional(args))),
        prec('no_block_call', seq(bare, args)),

        prec('brace_block_call', seq(withReceiver, optional(args), braceBlock)),
        prec('brace_block_call', seq(bare, optional(args), braceBlock)),

        prec('do_end_block_call', seq(withReceiver, optional(args), doBlock)),
        prec('do_end_block_call', seq(bare, optional(args), doBlock)),

        prec('ampersand_block_call', seq(withReceiver, argsWithBlock)),
        prec('ampersand_block_call', seq(bare, argsWithBlock)),
      );
    },

    // `::puts`: a top-level method, skipping any in scope.
    _global_method: $ => seq('::', field('method', choice(
      $.identifier,
      alias($.identifier_method_call, $.identifier),
      alias($.identifier_assign, $.identifier),
    ))),

    // ---- implicit-object calls ---------------------------------------------
    // Reference: "Short one-parameter syntax": `&.name`, and `when .odd?`.

    implicit_object_method_identifier: $ => token(seq(
      '.',
      repeat(/\s/),
      IDENT_START,
      repeat(IDENT_PART),
      optional(/[?!]/),
    )),

    implicit_object_method_operator: $ => token(seq(
      '.',
      repeat(/\s/),
      choice(...OPERATOR_METHODS),
    )),

    implicit_object_ivar: $ => token(seq(
      '.',
      repeat(/\s/),
      '@',
      IDENT_START,
      repeat(IDENT_PART),
    )),

    implicit_object_index_operator: $ => seq(
      '.',
      field('method', alias($._start_of_index_operator, $.operator)),
      field('arguments', alias($.bracket_argument_list, $.argument_list)),
      choice(']', ']?'),
    ),

    _implicit_object_call: $ => alias(
      choice(
        $.implicit_object_call_chainable,
        $.implicit_object_call_unchainable,
      ),
      $.implicit_object_call,
    ),

    // One that takes parenless arguments and so ends the chain: `&.foo 5`.
    implicit_object_call_unchainable: $ => {
      const method = field('method', choice(
        alias($.implicit_object_method_identifier, $.identifier),
        alias($.implicit_object_method_operator, $.operator),
      ));
      return seq(
        optional(field('receiver', alias($.implicit_object_call_chainable, $.implicit_object_call))),
        choice(
          seq(method, field('arguments', alias($.argument_list_no_parens, $.argument_list))),
          seq(method, field('arguments', alias($.argument_list_no_parens_with_block, $.argument_list))),
        ),
      );
    },

    // One another can follow: `&.foo(5).bar`.
    implicit_object_call_chainable: $ => {
      const method = field('method', choice(
        alias($.implicit_object_method_identifier, $.identifier),
        alias($.implicit_object_method_operator, $.operator),
      ));
      const args = field('arguments', choice(
        alias($.argument_list_with_parens, $.argument_list),
        alias($.argument_list_no_parens, $.argument_list),
      ));
      const braceBlock = field('block', alias($.brace_block, $.block));
      const doBlock = field('block', alias($.do_end_block, $.block));

      return seq(
        optional(field('receiver', alias($.implicit_object_call_chainable, $.implicit_object_call))),
        choice(
          prec.right(seq(method, optional(
            field('arguments', alias($.argument_list_with_parens, $.argument_list)),
          ))),
          seq(method, optional(args), braceBlock),
          seq(method, optional(args), doBlock),
          seq(method, field('arguments', alias($.argument_list_with_parens_and_block, $.argument_list))),
          alias($.implicit_object_ivar, $.instance_var),
          alias($.implicit_object_index_operator, $.index_call),
        ),
      );
    },

    // `when {.odd?, _}`: a tuple pattern with implicit-object calls.
    implicit_object_tuple: $ => seq(
      alias($._start_of_hash_or_tuple, '{'),
      optional(seq(commaSep1($._expression), ',')),
      choice($._implicit_object_call, $.underscore),
      repeat(seq(',', choice($._expression, $._implicit_object_call, $.underscore))),
      optional(','),
      '}',
    ),

    // The calls that can be assigned to: `a.b = c` is `a.b=(c)`.
    assign_call: $ => prec('dot_operator', seq(
      field('receiver', $._expression),
      '.',
      field('method', $.identifier),
    )),

    // `a[i]`, `a[i]?`
    index_operator: $ => prec('index_operator', seq(
      field('receiver', $._expression),
      field('method', alias($._start_of_index_operator, $.operator)),
      optional(field('arguments', alias($.bracket_argument_list, $.argument_list))),
      choice(']', ']?'),
    )),

    // `a.[i]`
    index_call: $ => seq(
      field('receiver', $._expression),
      '.',
      field('method', alias('[', $.operator)),
      field('arguments', alias($.bracket_argument_list, $.argument_list)),
      choice(']', ']?'),
    ),

    // ---- operators ---------------------------------------------------------

    not: $ => prec('unary_operator', seq(alias('!', $.operator), $._expression)),
    and: $ => prec.left('logical_and_operator', seq($._expression, alias('&&', $.operator), $._expression)),
    or: $ => prec.left('logical_or_operator', seq($._expression, alias('||', $.operator), $._expression)),

    additive_operator: $ => binaryCall($, prec.left, 'additive_operator', choice(
      $.binary_plus,
      $.binary_minus,
      $.binary_wrapping_plus,
      $.binary_wrapping_minus,
    )),

    unary_additive_operator: $ => unaryCall($, choice(
      $.unary_plus,
      $.unary_minus,
      $.unary_wrapping_plus,
      $.unary_wrapping_minus,
    )),

    multiplicative_operator: $ => binaryCall($, prec.left, 'multiplicative_operator', choice(
      $._binary_star,
      '&*',
      $._binary_slash,
      $._binary_double_slash,
      $._modulo_operator,
    )),

    exponential_operator: $ => binaryCall($, prec.right, 'exponential_operator',
      choice($._binary_double_star, '&**')),

    shift_operator: $ => binaryCall($, prec.left, 'shift_operator', choice('<<', '>>')),

    complement_operator: $ => unaryCall($, '~'),

    binary_and_operator: $ => binaryCall($, prec.left, 'binary_and_operator', $.binary_ampersand),

    binary_or_operator: $ => binaryCall($, prec.left, 'binary_or_operator', choice('|', '^')),

    equality_operator: $ => binaryCall($, prec.left, 'equality_operator',
      choice('==', '!=', '=~', '!~', '===')),

    comparison_operator: $ => binaryCall($, prec.left, 'comparison_operator',
      choice('<', '<=', '>', '>=', '<=>')),

    // Reference: "as", "is_a?", "nil?", "responds_to?": methods the
    // compiler implements, whose arguments are types or symbols.
    pseudo_call: $ => {
      const receiver = seq(field('receiver', $._expression), '.');
      return choice(
        seq(
          optional(receiver),
          field('method', alias(choice('as', 'as?', 'is_a?'), $.identifier)),
          field('arguments', alias($.pseudo_call_argument_list, $.argument_list)),
        ),
        seq(
          optional(receiver),
          field('method', alias('nil?', $.identifier)),
          optional(seq('(', ')')),
        ),
        seq(
          optional(receiver),
          field('method', alias('responds_to?', $.identifier)),
          field('arguments', alias($.pseudo_responds_to_argument_list, $.argument_list)),
        ),
      );
    },

    pseudo_call_argument_list: $ => choice(
      seq('(', $._bare_type, ')'),
      $._bare_type,
    ),

    pseudo_responds_to_argument_list: $ => {
      const symbol = alias(choice($.quoted_symbol, $.unquoted_symbol, $.operator_symbol), $.symbol);
      return choice(seq('(', symbol, ')'), symbol);
    },

    // ---- arguments ---------------------------------------------------------

    splat: $ => prec('splat_operator', seq(alias($._unary_star, '*'), $._expression)),

    double_splat: $ => prec('splat_operator', seq(alias($._unary_double_star, '**'), $._expression)),

    // `name: value`, `"quoted name": value`
    named_expr: $ => seq(
      field('name', choice(
        $.identifier,
        alias($._constant_segment, $.identifier),
        alias($.identifier_method_call, $.identifier),
        $.string,
        alias($.string_percent_literal, $.string),
      )),
      token.immediate(':'),
      choice($._expression, $.out),
    ),

    argument_list_no_parens: $ => prec.right(seq(
      optional($._start_of_parenless_args),
      argument($),
      repeat(prec('comma', seq(',', argument($)))),
    )),

    argument_list_no_parens_with_block: $ => prec.right(seq(
      optional($._start_of_parenless_args),
      optional(seq(argument($), repeat(prec('comma', seq(',', argument($)))), ',')),
      $.block_argument,
    )),

    // The `(` must touch the method name: `foo (1)` passes a parenthesized
    // expression.
    argument_list_with_parens: $ => prec.right(seq(
      token.immediate('('),
      optional(commaList1(argument($))),
      ')',
    )),

    argument_list_with_parens_and_block: $ => prec.right(seq(
      token.immediate('('),
      optional(seq(commaSep1(argument($)), ',')),
      $.block_argument,
      ')',
    )),

    // `out x`: a C function's output parameter.
    out: $ => seq('out', choice($.identifier, $.instance_var, $.underscore, $.macro_var)),

    // ---- assignment --------------------------------------------------------

    assign: $ => prec('assignment_operator', seq(
      field('lhs', choice(
        $.underscore,
        $.identifier,
        $.instance_var,
        $.class_var,
        $.macro_var,
        $.assign_call,
        $.index_call,
        alias($.index_operator, $.index_call),
        $.special_variable,
      )),
      '=',
      field('rhs', $._expression),
    )),

    const_assign: $ => prec.right('assignment_operator', seq(
      field('lhs', $.constant), '=', field('rhs', $._statement),
    )),

    // Only in an enum body.
    class_var_assign: $ => prec.right('assignment_operator', seq(
      field('lhs', $.class_var), '=', field('rhs', $._statement),
    )),

    operator_assign: $ => prec('assignment_operator', seq(
      field('lhs', choice(
        $.identifier,
        $.instance_var,
        $.class_var,
        $.macro_var,
        $.assign_call,
        $.index_call,
        alias($.index_operator, $.index_call),
      )),
      alias(choice(...ASSIGNMENT_OPERATORS), $.operator),
      field('rhs', $._expression),
    )),

    lhs_splat: $ => seq('*', choice(
      $.underscore,
      $.identifier,
      $.instance_var,
      $.class_var,
      $.macro_var,
      $.assign_call,
      $.index_call,
      alias($.index_operator, $.index_call),
    )),

    // Reference: "Multiple assignment": `a, b = 1, 2`, `*a, b = c`.
    multi_assign: $ => {
      const target = choice(
        $.underscore,
        $.identifier,
        $.instance_var,
        $.class_var,
        $.macro_var,
        $.assign_call,
        $.index_call,
        alias($.index_operator, $.index_call),
      );
      const splatTarget = field('lhs', alias($.lhs_splat, $.splat));
      const lhs = field('lhs', choice(target, alias($.lhs_splat, $.splat)));
      const lhsList = seq(repeat1(seq(lhs, ',')), lhs);
      const rhs = field('rhs', $._expression);
      const rhsList = seq(repeat1(seq(rhs, ',')), rhs);

      return choice(
        seq(splatTarget, '=', rhs),
        seq(lhsList, '=', rhs),
        seq(splatTarget, '=', rhsList),
        seq(lhsList, '=', rhsList),
      );
    },

    // `x = uninitialized Int32`
    uninitialized_assign: $ => seq(
      field('lhs', choice($.identifier, $.instance_var, $.class_var, $.global_var, $.macro_var)),
      '=',
      field('rhs', $.uninitialized_var),
    ),

    uninitialized_var: $ => seq('uninitialized', $._bare_type),

    // `x : Int32`, `@x : Int32 = 1`: the colon needs a space after it.
    type_declaration: $ => prec('assignment_operator', seq(
      field('var', choice(
        $.identifier,
        alias($.identifier_method_call, $.identifier),
        $.instance_var,
        $.class_var,
        $.macro_var,
      )),
      ':',
      token.immediate(/\s/),
      field('type', $._bare_type),
      optional(seq('=', field('value', $._expression))),
    )),

    alias: $ => seq(
      'alias',
      field('name', $.constant),
      '=',
      field('type', $._bare_type),
    ),

    // ---- blocks ------------------------------------------------------------

    block_body_param: $ => field('name', choice($.identifier, $.underscore)),

    block_body_splat_param: $ => seq(
      '*',
      field('name', choice($.identifier, $.underscore)),
    ),

    // `|(a, b), c|`: unpacking a tuple argument.
    _block_body_nested_param: $ => seq(
      '(',
      commaList1(choice(
        alias($.block_body_param, $.param),
        alias($.block_body_splat_param, $.splat_param),
        $._block_body_nested_param,
      )),
      ')',
    ),

    block_param_list: $ => commaList1(choice(
      alias($.block_body_param, $.param),
      alias($.block_body_splat_param, $.splat_param),
      $._block_body_nested_param,
    )),

    do_end_block: $ => seq(
      'do',
      optional(seq('|', field('params', alias($.block_param_list, $.param_list)), '|')),
      body($),
      optional($._rescue_else_ensure),
      'end',
    ),

    brace_block: $ => seq(
      alias($._start_of_brace_block, '{'),
      optional(seq('|', field('params', alias($.block_param_list, $.param_list)), '|')),
      body($),
      '}',
    ),

    // `&block`, `&.method`
    block_argument: $ => prec('block_ampersand', seq(
      alias($._block_ampersand, '&'),
      choice($._expression, $._implicit_object_call),
    )),

    // ---- exceptions --------------------------------------------------------

    begin: $ => seq(
      'begin',
      optional($._terminator),
      body($),
      optional($._rescue_else_ensure),
      'end',
    ),

    // `rescue`, `rescue Type`, `rescue e`, `rescue e : A | B`
    rescue: $ => {
      const variable = field('variable', $.identifier);
      const type = field('type', $._bare_type);
      return seq(
        alias($._regular_rescue_keyword, 'rescue'),
        optional(choice(
          seq(variable, alias(/:\s/, ':'), type),
          variable,
          type,
        )),
        $._terminator,
        body($),
      );
    },

    ensure: $ => seq(
      alias($._regular_ensure_keyword, 'ensure'),
      field('body', optional(alias($._statements, $.expressions))),
    ),

    modifier_rescue: $ => seq(
      $._statement,
      alias($._modifier_rescue_keyword, 'rescue'),
      field('rescue', $._expression),
    ),

    modifier_ensure: $ => seq(
      $._statement,
      alias($._modifier_ensure_keyword, 'ensure'),
      field('ensure', $._expression),
    ),

    // At least one of rescue, else and ensure (one rule rather than three
    // optionals keeps the parser smaller).
    _rescue_else_ensure: $ => choice(
      seq(
        field('rescue', repeat1($.rescue)),
        field('else', optional($.else)),
        field('ensure', optional($.ensure)),
      ),
      seq(
        field('rescue', repeat($.rescue)),
        field('else', $.else),
        field('ensure', optional($.ensure)),
      ),
      seq(
        field('rescue', repeat($.rescue)),
        field('else', optional($.else)),
        field('ensure', $.ensure),
      ),
    ),

    // ---- control flow ------------------------------------------------------

    while: $ => seq(
      'while',
      field('cond', $._expression),
      $._terminator,
      body($),
      'end',
    ),

    until: $ => seq(
      'until',
      field('cond', $._expression),
      $._terminator,
      body($),
      'end',
    ),

    if: $ => seq(
      alias($._regular_if_keyword, 'if'),
      field('cond', $._expression),
      $._terminator,
      optional(field('then', $.then)),
      optional(field('else', choice($.elsif, $.else))),
      'end',
    ),

    unless: $ => seq(
      alias($._regular_unless_keyword, 'unless'),
      field('cond', $._expression),
      $._terminator,
      optional(field('then', $.then)),
      optional(field('else', $.else)),
      'end',
    ),

    then: $ => seq($._statements),

    elsif: $ => seq(
      'elsif',
      field('cond', $._expression),
      $._terminator,
      optional(field('then', $.then)),
      optional(field('else', choice($.elsif, $.else))),
    ),

    else: $ => seq(
      'else',
      body($),
    ),

    conditional: $ => prec.right('ternary_operator', seq(
      field('cond', $._expression),
      '?',
      field('then', $._expression),
      ':',
      field('else', $._expression),
    )),

    modifier_if: $ => seq(
      field('then', $._statement),
      alias($._modifier_if_keyword, 'if'),
      field('cond', $._expression),
    ),

    modifier_unless: $ => seq(
      field('then', $._statement),
      alias($._modifier_unless_keyword, 'unless'),
      field('cond', $._expression),
    ),

    require: $ => seq('require', $.string),

    when: $ => {
      const cond = field('cond', choice(
        $._expression,
        $._implicit_object_call,
        alias($.implicit_object_tuple, $.tuple),
      ));
      return seq(
        'when',
        commaSep1(cond),
        choice('then', $._terminator),
        body($),
      );
    },

    case: $ => seq(
      'case',
      optional(field('cond', $._expression)),
      repeat($.when),
      optional($.else),
      'end',
    ),

    // Reference: "select" (channels).
    select: $ => seq(
      'select',
      repeat($.when),
      optional($.else),
      'end',
    ),

    // `case x in ...`: exhaustive case (reference: "case", "Exhaustive case").
    in: $ => {
      const cond = field('cond', choice(
        $._expression,
        $._implicit_object_call,
        alias($.implicit_object_tuple, $.tuple),
      ));
      return seq(
        'in',
        commaSep1(cond),
        choice('then', $._terminator),
        body($),
      );
    },

    exhaustive_case: $ => seq(
      'case',
      field('cond', $._expression),
      repeat1($.in),
      'end',
    ),

    // ---- inline assembly ---------------------------------------------------
    // Reference: "asm": asm("text" : outputs : inputs : clobbers : options),
    // each section optional from the right. One rule per section keeps the
    // parser small.

    asm: $ => seq(
      'asm',
      '(',
      field('text', $.string),
      optional($._asm_outputs),
      ')',
    ),

    _asm_outputs: $ => seq(':', optional(field('outputs', $.asm_operands)), optional($._asm_inputs)),

    _asm_inputs: $ => seq(':', optional(field('inputs', $.asm_operands)), optional($._asm_clobbers)),

    _asm_clobbers: $ => seq(':', optional(field('clobbers', $.asm_clobbers)), optional($._asm_options)),

    _asm_options: $ => seq(':', optional(field('options', $.asm_options))),

    asm_operands: $ => commaSep1($.asm_operand),

    asm_operand: $ => seq(
      field('constraint', $.string),
      '(',
      field('expression', $._expression),
      ')',
    ),

    asm_clobbers: $ => commaSep1($.string),

    asm_options: $ => commaSep1($.string),

    // ---- location pragmas --------------------------------------------------
    // `#<loc:push>`, `#<loc:pop>`, `#<loc:"file",line,column>`: the
    // compiler's own markers in macro-generated code.

    loc_pragma_push: $ => token(prec(1, '#<loc:push>')),
    loc_pragma_pop: $ => token(prec(1, '#<loc:pop>')),
    loc_pragma_location: $ => token(prec(1, seq(
      '#<loc:"',
      optional(/[^"]+/),
      '",',
      optional(/[0-9]+/),
      seq(',', optional(/[0-9]+/)),
      '>',
    ))),
  },
});
