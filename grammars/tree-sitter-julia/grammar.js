/**
 * @file Julia grammar for tree-sitter
 * @author Max Brunsfeld <maxbrunsfeld@gmail.com>
 * @author Sergio A. Vargas <savargasqu+git@unal.edu.co>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const PREC = [
  'afunc',
  'pair',
  'conditional',
  'arrow',
  'lazy_or',
  'lazy_and',
  'where', // FIXME
  'comparison',
  'pipe_left',
  'pipe_right',
  'colon',
  'plus',
  'times',
  'rational',
  'bitshift',
  'prefix',
  'power',
  'decl',
  'dot',
].reduce((result, name, index) => {
  result[name] = index + 10;
  return result;
}, {});

PREC.array = -1;
PREC.tilde = 2; // mep
PREC.tuple = -1;
PREC.assign = -2;
PREC.stmt = -3;
PREC.macro_arg = -4;

const OPERATORS = {
  assignment: `
    += -= *= /= //= \\= ^= %= <<= >>= >>>= |= &=
    −= ÷= ⊻= ≔ ⩴ ≕
  `,

  arrow: `
    <-- --> <-->
    ← → ↔ ↚ ↛ ↞ ↠ ↢ ↣ ↦ ↤ ↮ ⇎ ⇍ ⇏ ⇐ ⇒ ⇔ ⇴ ⇶ ⇷ ⇸ ⇹ ⇺ ⇻ ⇼ ⇽ ⇾ ⇿ ⟵ ⟶ ⟷ ⟹ ⟺ ⟻ ⟼ ⟽ ⟾ ⟿
    ⤀ ⤁ ⤂ ⤃ ⤄ ⤅ ⤆ ⤇ ⤌ ⤍ ⤎ ⤏ ⤐ ⤑ ⤔ ⤕ ⤖ ⤗ ⤘ ⤝ ⤞ ⤟ ⤠ ⥄ ⥅ ⥆ ⥇ ⥈ ⥊ ⥋ ⥎ ⥐ ⥒ ⥓ ⥖ ⥗ ⥚ ⥛ ⥞
    ⥟ ⥢ ⥤ ⥦ ⥧ ⥨ ⥩ ⥪ ⥫ ⥬ ⥭ ⥰ ⧴ ⬱ ⬰ ⬲ ⬳ ⬴ ⬵ ⬶ ⬷ ⬸ ⬹ ⬺ ⬻ ⬼ ⬽ ⬾ ⬿ ⭀ ⭁ ⭂ ⭃ ⥷ ⭄ ⥺ ⭇ ⭈ ⭉
    ⭊ ⭋ ⭌ ￩ ￫ ⇜ ⇝ ↜ ↝ ↩ ↪ ↫ ↬ ↼ ↽ ⇀ ⇁ ⇄ ⇆ ⇇ ⇉ ⇋ ⇌ ⇚ ⇛ ⇠ ⇢ ↷ ↶ ↺ ↻
  `,

  comparison: `
    > < >= <= == === != !==
    ≥ ≤ ≡ ≠ ≢ ∈ ∉ ∋ ∌ ⊆ ⊈ ⊂ ⊄ ⊊ ∝ ∊ ∍ ∥ ∦ ∷ ∺ ∻ ∽ ∾ ≁ ≃ ≂ ≄ ≅ ≆ ≇ ≈ ≉ ≊ ≋ ≌ ≍ ≎ ≐
    ≑ ≒ ≓ ≖ ≗ ≘ ≙ ≚ ≛ ≜ ≝ ≞ ≟ ≣ ≦ ≧ ≨ ≩ ≪ ≫ ≬ ≭ ≮ ≯ ≰ ≱ ≲ ≳ ≴ ≵ ≶ ≷ ≸ ≹ ≺ ≻ ≼ ≽ ≾
    ≿ ⊀ ⊁ ⊃ ⊅ ⊇ ⊉ ⊋ ⊏ ⊐ ⊑ ⊒ ⊜ ⊩ ⊬ ⊮ ⊰ ⊱ ⊲ ⊳ ⊴ ⊵ ⊶ ⊷ ⋍ ⋐ ⋑ ⋕ ⋖ ⋗ ⋘ ⋙ ⋚ ⋛ ⋜ ⋝ ⋞ ⋟ ⋠
    ⋡ ⋢ ⋣ ⋤ ⋥ ⋦ ⋧ ⋨ ⋩ ⋪ ⋫ ⋬ ⋭ ⋲ ⋳ ⋴ ⋵ ⋶ ⋷ ⋸ ⋹ ⋺ ⋻ ⋼ ⋽ ⋾ ⋿ ⟈ ⟉ ⟒ ⦷ ⧀ ⧁ ⧡ ⧣ ⧤ ⧥ ⩦ ⩧
    ⩪ ⩫ ⩬ ⩭ ⩮ ⩯ ⩰ ⩱ ⩲ ⩳ ⩵ ⩶ ⩷ ⩸ ⩹ ⩺ ⩻ ⩼ ⩽ ⩾ ⩿ ⪀ ⪁ ⪂ ⪃ ⪄ ⪅ ⪆ ⪇ ⪈ ⪉ ⪊ ⪋ ⪌ ⪍ ⪎ ⪏ ⪐ ⪑
    ⪒ ⪓ ⪔ ⪕ ⪖ ⪗ ⪘ ⪙ ⪚ ⪛ ⪜ ⪝ ⪞ ⪟ ⪠ ⪡ ⪢ ⪣ ⪤ ⪥ ⪦ ⪧ ⪨ ⪩ ⪪ ⪫ ⪬ ⪭ ⪮ ⪯ ⪰ ⪱ ⪲ ⪳ ⪴ ⪵ ⪶ ⪷ ⪸
    ⪹ ⪺ ⪻ ⪼ ⪽ ⪾ ⪿ ⫀ ⫁ ⫂ ⫃ ⫄ ⫅ ⫆ ⫇ ⫈ ⫉ ⫊ ⫋ ⫌ ⫍ ⫎ ⫏ ⫐ ⫑ ⫒ ⫓ ⫔ ⫕ ⫖ ⫗ ⫘ ⫙ ⫷ ⫸ ⫹ ⫺ ⊢ ⊣
    ⟂ ⫪ ⫫
  `,

  ellipsis: '… ⁝ ⋮ ⋱ ⋰ ⋯',

  plus: `
    ++ |
    − ¦ ⊕ ⊖ ⊞ ⊟ ∪ ∨ ⊔ ∔ ∸ ≏ ⊎ ⊻ ⊽ ⋎ ⋓ ⟇ ⧺ ⧻ ⨈ ⨢ ⨣ ⨤ ⨥ ⨦ ⨧ ⨨ ⨩ ⨪ ⨫ ⨬ ⨭ ⨮ ⨹ ⨺ ⩁ ⩂ ⩅
    ⩊ ⩌ ⩏ ⩐ ⩒ ⩔ ⩖ ⩗ ⩛ ⩝ ⩡ ⩢ ⩣
  `,

  times: `
    * / % & \\
    ⌿ ÷ · · ⋅ ∘ × ∩ ∧ ⊗ ⊘ ⊙ ⊚ ⊛ ⊠ ⊡ ⊓ ∗ ∙ ∤ ⅋ ≀ ⊼ ⋄ ⋆ ⋇ ⋉ ⋊ ⋋ ⋌ ⋏ ⋒ ⟑ ⦸ ⦼ ⦾ ⦿ ⧶ ⧷
    ⨇ ⨰ ⨱ ⨲ ⨳ ⨴ ⨵ ⨶ ⨷ ⨸ ⨻ ⨼ ⨽ ⩀ ⩃ ⩄ ⩋ ⩍ ⩎ ⩑ ⩓ ⩕ ⩘ ⩚ ⩜ ⩞ ⩟ ⩠ ⫛ ⊍ ▷ ⨝ ⟕ ⟖ ⟗ ⨟
  `,

  bitshift: '<< >> >>>',

  power: `
    ^
    ↑ ↓ ⇵ ⟰ ⟱ ⤈ ⤉ ⤊ ⤋ ⤒ ⤓ ⥉ ⥌ ⥍ ⥏ ⥑ ⥔ ⥕ ⥘ ⥙ ⥜ ⥝ ⥠ ⥡ ⥣ ⥥ ⥮ ⥯ ￪ ￬
  `,

  unary: '! ¬ √ ∛ ∜',

  unary_plus: '+ - ± ∓',
};

// The characters Julia accepts after an operator as part of its name
// (JuliaSyntax: `is_op_suffix_char`), the common ones.
const OP_SUFFIX = {type: 'PATTERN', value:
  '[\\x{B2}\\x{B3}\\x{B9}\\x{2B0}-\\x{2B8}\\x{2E1}-\\x{2E3}\\x{300}-\\x{36F}' +
  '\\x{1D2C}-\\x{1D6A}\\x{1D9C}-\\x{1DBF}\\x{2032}-\\x{2037}\\x{2057}' +
  '\\x{2070}-\\x{209C}\\x{20D0}-\\x{20F0}\\x{2C7C}\\x{2C7D}\\x{A71B}-\\x{A71D}]+'};

const ESCAPE_SEQUENCE = token(seq(
  '\\',
  choice(
    /[^uUx0-7]/,
    /[uU][0-9a-fA-F]{1,6}/, // unicode codepoints
    /[0-7]{1,3}/,
    /x[0-9a-fA-F]{1,2}/, // mep: `"\x7"`
  ),
));

// Keywords that can be quoted. Some still fail depending on the context.
const KEYWORDS = choice(
  'baremodule',
  'module',
  'abstract',
  'primitive',
  'mutable',
  'struct',
  'quote',
  'let',
  'if',
  'else',
  'elseif',
  'try',
  'catch',
  'finally',
  'for',
  'while',
  'break',
  'continue',
  'using',
  'import',
  'const',
  'global',
  'local',
  'end',
  // mep: the rest of Julia's reserved words and the word operators, which
  // `:in`, `:where`, `:function` ... quote just the same.
  'begin',
  'function',
  'macro',
  'do',
  'return',
  'export',
  'public',
  'in',
  'isa',
  'where',
);

module.exports = grammar({
  name: 'julia',

  word: $ => $._word_identifier,

  inline: $ => [
    $._block_form,
    $._terminator,
    $._definition,
    $._statement,
    $._operation,
  ],

  externals: $ => [
    $._block_comment_rest,
    $._immediate_paren,
    $._immediate_bracket,
    $._immediate_brace,
    $._immediate_string_start,
    $._immediate_command_start,
    $._content_cmd_1,
    $._content_cmd_1_raw,
    $._content_cmd_3,
    $._content_cmd_3_raw,
    $._content_str_1,
    $._content_str_1_raw,
    $._content_str_3,
    $._content_str_3_raw,
    $._end_cmd,
    $._end_str,
    $._adjoint_quote,
    $._juxtapose,
    $._spaced_colon,
    $._ternary_colon,
    $._begin_index,
  ],

  conflicts: $ => [
    [$.juxtaposition_expression, $._juxtaposable_primary], // adjoint, then `isa(`
    [$.juxtaposition_expression, $._expression],
    [$.matrix_row, $.comprehension_expression], // Comprehensions with newlines
    [$.parenthesized_expression, $.tuple_expression],
  ],

  supertypes: $ => [
    $._expression,
    $._statement,
    $._definition,
  ],

  extras: $ => [
    /\s/,
    $.line_comment,
    $.block_comment,
  ],

  rules: {
    source_file: $ => optional(seq(
      sep1($._terminator, $._block_form),
      optional($._terminator)
    )),

    block: $ => seq(
      sep1($._terminator, $._block_form),
      optional($._terminator)
    ),

    _block_form: $ => choice(
      $._expression,
      $.assignment,
      $.open_tuple,
    ),

    _bracket_form: $ => choice(
      $._expression,
      alias($._closed_assignment, $.assignment),
    ),

    open_tuple: $ => prec(PREC.tuple, seq(
      $._expression,
      repeat1(seq(',', $._expression))
    )),

    // mep: `a, = f()` and `a, b, = xs` -- a destructuring left-hand side may
    // end with a comma (only before `=`, so a trailing comma never ends a
    // statement and a newline after one still continues it).
    _open_tuple_trailing_comma: $ => seq(
      $._expression,
      repeat(seq(',', $._expression)),
      ',',
    ),

    // assignments inside blocks
    assignment: $ => prec.right(PREC.assign, seq(
      choice(
        $._primary_expression,
        $.open_tuple,
        alias($._open_tuple_trailing_comma, $.open_tuple),
        $._operation,
        $.operator,
      ),
      alias('=', $.operator),
      $._block_form,
    )),

    // assignments inside brackets
    _closed_assignment: $ => prec.right(PREC.assign, seq(
      choice(
        $._primary_expression,
        $._operation,
        $.operator,
      ),
      alias('=', $.operator),
      $._bracket_form,
    )),

    _expression: $ => choice(
      $._definition,
      $._statement,
      $._primary_expression,
      $._operation,
      $.compound_assignment_expression,
      $.macrocall_expression,
      $.arrow_function_expression,
      $.juxtaposition_expression,
      $.ternary_expression,
      $.operator,
      $.integer_literal,
      $.float_literal,
      // mep: `a[begin+1:end]`; the scanner tells it from a block's `begin`.
      alias($._begin_index, $.identifier),
    ),

    // Definitions

    _definition: $ => choice(
      $.module_definition,
      $.abstract_definition,
      $.primitive_definition,
      $.struct_definition,
      $.function_definition,
      $.macro_definition,
    ),

    module_definition: $ => seq(
      choice('module', 'baremodule'),
      field('name', choice($.identifier, $.interpolation_expression)),
      optional($._terminator),
      optional($.block),
      'end',
    ),

    // TODO: Rename
    type_head: $ => prec(PREC.stmt, choice(
      $._primary_expression,
      $.binary_expression,
      $.where_expression, // mep: `struct S{N} <: T where {N} end`
    )),

    abstract_definition: $ => seq(
      'abstract',
      'type',
      $.type_head,
      optional($._terminator), // mep: `abstract type T; end`
      'end',
    ),

    primitive_definition: $ => seq(
      'primitive',
      'type',
      $.type_head,
      // mep: the size is any expression (`9*8`, `$(sizeof(T) * 8)`)
      $._expression,
      optional($._terminator),
      'end',
    ),

    struct_definition: $ => seq(
      optional('mutable'),
      'struct',
      $.type_head,
      optional($._terminator),
      optional($.block),
      'end',
    ),

    signature: $ => prec(PREC.stmt, choice(
      $.identifier, // zero-method definition
      alias(choice('isa', 'in'), $.identifier), // mep: `function in end`
      $.operator, // mep: `function ∘ end`
      $.interpolation_expression, // mep: `function $name end`
      $.call_expression,
      alias($.tuple_expression, $.argument_list), // anonymous function
      $.typed_expression,
      $.where_expression,
    )),

    function_definition: $ => seq(
      'function',
      $.signature,
      optional($._terminator),
      optional($.block),
      'end',
    ),

    macro_definition: $ => seq(
      'macro',
      $.signature,
      optional($._terminator),
      optional($.block),
      'end',
    ),


    // Statements

    _statement: $ => choice(
      // block statements:
      $.compound_statement,
      $.quote_statement,
      $.let_statement,
      $.if_statement,
      $.try_statement,
      $.for_statement,
      $.while_statement,
      // simple statements:
      $.break_statement,
      $.continue_statement,
      $.return_statement,
      $.const_statement,
      $.global_statement,
      $.local_statement,
      $.export_statement,
      $.import_statement,
      $.public_statement,
      $.using_statement,
    ),

    compound_statement: $ => seq('begin', optional($._terminator), optional($.block), 'end'),

    quote_statement: $ => seq('quote', optional($._terminator), optional($.block), 'end'),

    let_statement: $ => seq(
      'let',
      choice(
        seq(
          sep(',', $._bracket_form),
          $._terminator,
          optional($.block),
        ),
        // mep: `let x = 1 end` -- bindings and no body, on one line
        sep1(',', $._bracket_form),
      ),
      'end',
    ),

    if_statement: $ => seq(
      'if',
      field('condition', $._expression),
      optional($._terminator),
      optional($.block),
      field('alternative', repeat($.elseif_clause)),
      field('alternative', optional($.else_clause)),
      'end',
    ),

    elseif_clause: $ => seq(
      'elseif',
      field('condition', $._expression),
      optional($._terminator),
      optional($.block),
    ),

    else_clause: $ => seq(
      'else',
      optional($._terminator),
      optional($.block),
    ),

    try_statement: $ => seq(
      'try',
      optional($._terminator),
      optional($.block),
      choice(
        seq(
          $.catch_clause,
          optional($.else_clause),
          optional($.finally_clause),
        ),
        seq(
          $.finally_clause,
          optional($.catch_clause),
          // `else` is not valid here.
        ),
      ),
      'end',
    ),

    catch_clause: $ => prec(1, seq(
      'catch',
      optional($.identifier),
      optional($._terminator),
      optional($.block),
    )),

    finally_clause: $ => seq(
      'finally',
      optional($._terminator),
      optional($.block),
    ),

    for_statement: $ => seq(
      'for',
      sep1(',', $.for_binding),
      optional($._terminator),
      optional($.block),
      'end',
    ),

    while_statement: $ => seq(
      'while',
      field('condition', $._expression),
      optional($._terminator),
      optional($.block),
      'end',
    ),

    break_statement: _ => 'break',

    continue_statement: _ => 'continue',

    return_statement: $ => prec.right(PREC.stmt, seq(
      'return',
      optional($._block_form),
    )),

    const_statement: $ => prec.right(PREC.stmt, seq(
      'const',
      $._block_form,
    )),

    global_statement: $ => prec.right(PREC.stmt, seq(
      'global',
      $._block_form,
    )),

    local_statement: $ => prec.right(PREC.stmt, seq(
      'local',
      $._block_form,
    )),

    import_alias: $ => seq($._importable, 'as', $._exportable),

    import_path: $ => seq(
      token(repeat1('.')),
      choice(
        $.identifier,
        $._scoped_identifier,
        $.macro_identifier, // mep: `import ..@inline`
        $._scoped_import_member,
      ),
    ),

    // mep: a dotted path ending in a macro or a parenthesised operator:
    // `Base.@kwdef`, `Math.@horner`, `Base.(:)`, `Base.(+)`.
    _scoped_import_member: $ => seq(
      choice($.identifier, $.interpolation_expression),
      repeat(seq(
        token.immediate('.'),
        choice($.identifier, $.interpolation_expression),
      )),
      token.immediate('.'),
      choice(
        $.macro_identifier,
        parenthesize($.operator),
      ),
    ),

    _exportable: $ => choice(
      $.identifier,
      $.prefixed_string_literal, // mep: `export var"⨟"`
      $.macro_identifier,
      $.operator,
      $.interpolation_expression,
      parenthesize($._exportable),
    ),

    _importable: $ => choice(
      $._exportable,
      alias($._scoped_identifier, $.import_path),
      alias($._scoped_import_member, $.import_path),
      $.import_path,
    ),

    _import_list: $ => prec.right(sep1(',', choice(
      $._importable,
      $.import_alias,
    ))),

    selected_import: $ => seq(
      $._importable,
      token.immediate(':'),
      $._import_list,
    ),

    export_statement: $ => seq('export', prec.right(sep1(',', $._exportable))),

    // mep: the list may start on the next line (base/public.jl); the newline
    // is a token here because `public` alone is also a name.
    public_statement: $ => seq('public', repeat(/\r?\n/), prec.right(sep1(',', $._exportable))),

    import_statement: $ => seq(
      'import',
      choice(
        $._import_list,
        $.selected_import,
      ),
    ),

    using_statement: $ => seq(
      'using',
      choice(
        $._import_list,
        $.selected_import,
      ),
    ),

    // Primary expressions can be called, indexed, accessed, and type parametrized.
    _primary_expression: $ => choice(
      $._juxtaposable_primary,
      // mep: `isa` and `in` are names as well as operators: `@assert isa(x, T)`,
      // `in.(xs, d)`, `f === isa`. Never as the right of a juxtaposition, where
      // they are the operator (`1 isa T`, `A' in B`); and the operator when
      // both read (`if x isa⏎T`).
      prec.dynamic(-1, alias(choice('isa', 'in'), $.identifier)),
    ),

    _juxtaposable_primary: $ => choice(
      $.identifier,
      // mep: words that are keywords only in one place are names elsewhere:
      // `public = 1`, `macro public(ex)`, `mutable = true`.
      // (`public +` stays a public statement, `mutable struct` a struct.)
      prec(-1, alias(choice('public', 'mutable'), $.identifier)),
      $.boolean_literal,
      $.curly_expression, // Only valid in macros
      $.parenthesized_expression,
      $.tuple_expression,
      $._array,
      $._string,
      $.adjoint_expression,
      $.broadcast_call_expression,
      $.call_expression,
      alias($._closed_macrocall_expression, $.macrocall_expression),
      $.parametrized_type_expression,
      $.field_expression,
      $.index_expression,
      $.interpolation_expression,
      $.quote_expression,
    ),

    _array: $ => choice(
      $.comprehension_expression,
      $.matrix_expression,
      $.vector_expression,
    ),

    comprehension_expression: $ => prec(PREC.array, seq(
      '[',
      $._bracket_form,
      optional($._terminator),
      $.for_clause,
      repeat(choice(
        $.for_clause,
        $.if_clause,
      )),
      ']',
    )),

    generator: $ => seq(
      $._bracket_form,
      $.for_clause,
      repeat(choice(
        $.for_clause,
        $.if_clause,
      )),
    ),

    if_clause: $ => seq(
      'if',
      $._expression,
    ),

    for_clause: $ => prec.right(seq(
      'for',
      sep1(',', $.for_binding),
    )),

    for_binding: $ => prec(1, seq(
      optional('outer'),
      choice(
        $.identifier,
        alias(choice('isa', 'outer'), $.identifier), // mep: `for isa in ISAs`, `for outer in xs`
        $.prefixed_string_literal, // mep: `for var"%j" = ...`
        $.tuple_expression,
        $.typed_expression,
        $.interpolation_expression,
      ),
      alias(choice('in', '=', '∈'), $.operator),
      $._expression,
    )),

    matrix_expression: $ => prec(PREC.array, seq(
      '[',
      $.matrix_row,
      repeat(seq($._terminator, $.matrix_row)),
      optional($._terminator),
      ']',
    )),

    matrix_row: $ => repeat1(prec(PREC.array, $._bracket_form)),

    vector_expression: $ => seq(
      '[',
      choice(
        seq(sep(',', $._bracket_form), optional(',')),
        // mep: parameters after a comma list, as in a call: JuMP's
        // `x[i = 1:2, j = 1:2; i != j]` (an index with a condition).
        seq(
          $._bracket_form,
          repeat1(seq(',', $._bracket_form)),
          optional(','),
          $._semicolon,
          sep(',', $._bracket_form),
          optional(','),
        ),
      ),
      ']',
    ),

    parenthesized_expression: $ => prec.dynamic(1, parenthesize(
      sep1($._semicolon, choice(
        $._bracket_form,
        $.generator,
      )),
      optional($._semicolon),
    )),

    tuple_expression: $ => parenthesize(
      optional($._semicolon),
      // mep: `f(x, ;k)`, `(x,; y)`, `f(x;)` -- a comma may come before the
      // semicolon, and a semicolon may end the list.
      sep(choice(',', $._semicolon, seq(',', $._semicolon)), choice(
        $._bracket_form,
        $.generator,
      )),
      optional(choice(',', $._semicolon, seq(',', $._semicolon))),
    ),

    curly_expression: $ => seq(
      '{',
      // mep: `where {X; Trait{X}}` (SimpleTraits)
      sep(choice(',', $._semicolon), $._bracket_form),
      optional(','),
      '}',
    ),

    adjoint_expression: $ => seq(
      $._primary_expression,
      // mep: the scanner's, so that a `'` right after an operand is always
      // the adjoint (`sin'''(x)`), even where a character could start.
      alias($._adjoint_quote, '\''),
    ),

    field_expression: $ => prec(PREC.dot, seq(
      field('value', $._primary_expression),
      token.immediate('.'),
      choice(
        $.identifier,
        $.interpolation_expression,
        $.quote_expression,
        $._string,
      ),
    )),

    index_expression: $ => seq(
      $._primary_expression,
      $._immediate_bracket,
      $._array,
    ),

    parametrized_type_expression: $ => seq(
      $._primary_expression,
      $._immediate_brace,
      $.curly_expression,
    ),

    call_expression: $ => seq(
      choice($._primary_expression, $.operator),
      $._immediate_paren,
      alias($.tuple_expression, $.argument_list),
      optional($.do_clause),
    ),

    broadcast_call_expression: $ => seq(
      $._primary_expression,
      token.immediate('.'),
      $._immediate_paren,
      alias($.tuple_expression, $.argument_list),
      optional($.do_clause),
    ),

    _qualified_macro_identifier: $ => seq(
      $._primary_expression,
      token.immediate('.'),
      $.macro_identifier,
    ),

    _macro_head: $ => choice(
      alias($._qualified_macro_identifier, $.field_expression),
      $.macro_identifier,
    ),

    _closed_macrocall_expression: $ => seq(
      $._macro_head,
      choice(
        seq($._immediate_brace, $.curly_expression),
        seq($._immediate_bracket, $._array),
        seq(
          $._immediate_paren,
          alias($.tuple_expression, $.argument_list),
          optional($.do_clause)
        ),
      ),
    ),

    macrocall_expression: $ => prec.right(seq($._macro_head, optional($.macro_argument_list))),

    macro_argument_list: $ => prec.left(repeat1(prec(PREC.macro_arg, $._block_form))),

    do_clause: $ => seq(
      'do',
      choice(
        $._terminator,
        // mep: with arguments, the body may follow on the same line
        // (`do io end`, `do x @inline; x end`).
        seq(sep1(',', $._bracket_form), optional($._terminator)),
      ),
      optional($.block),
      'end',
    ),

    interpolation_expression: $ => prec.right(PREC.prefix, seq(
      '$',
      choice(
        $.interpolation_expression, // mep: `$$x` in nested quotes
        $.integer_literal,
        $.float_literal,
        $.identifier,
        $.curly_expression,
        $.parenthesized_expression,
        $.tuple_expression,
        $._array,
        $._string,
      ),
    )),

    quote_expression: $ => prec.right(PREC.prefix, seq(
      ':',
      choice(
        $.integer_literal,
        $.float_literal,
        $._string,
        $.identifier,
        alias($._begin_index, $.identifier), // mep: `[:begin, :end]`
        $.operator,
        seq($._immediate_brace, $.curly_expression),
        seq($._immediate_bracket, $._array),
        seq($._immediate_paren, choice(
          $.parenthesized_expression,
          $.tuple_expression,
          // Syntactic operators in parentheses
          parenthesize(
            alias(
              choice(
                '::', ':=', '.=', '=',
                $._assignment_operator,
                $._lazy_or_operator,
                $._lazy_and_operator,
                $._syntactic_operator,
              ),
              $.operator,
            ),
          ),
        )),
        // Syntactic operators without parentheses
        alias(
          choice(
            $._assignment_operator,
            $._lazy_or_operator,
            $._lazy_and_operator,
            $._syntactic_operator,
          ),
          $.operator,
        ),
        alias(token.immediate(KEYWORDS), $.identifier),
      ),
    )),


    // Operations

    _operation: $ => choice(
      $.unary_expression,
      $.binary_expression,
      $.range_expression,
      $.splat_expression,
      $.typed_expression,
      $.unary_typed_expression,
      $.where_expression,
    ),

    binary_expression: $ => {
      const table = [
        [prec.right, PREC.pair, $._pair_operator],
        [prec.right, PREC.arrow, $._arrow_operator],
        [prec.left, PREC.lazy_or, $._lazy_or_operator],
        [prec.left, PREC.lazy_and, $._lazy_and_operator],
        // mep: `in`/`isa` are names too, so a newline after them is a token
        // where a statement could follow (`if alg isa⏎ T`); take it here.
        [prec.left, PREC.comparison, choice('in', 'isa'), true],
        [prec.left, PREC.comparison, choice($._comparison_operator, $._type_order_operator)],
        [prec.right, PREC.pipe_left, $._pipe_left_operator],
        [prec.left, PREC.pipe_right, $._pipe_right_operator],
        [prec.left, PREC.colon, $._ellipsis_operator],
        [prec.left, PREC.plus, choice($._unary_plus_operator, $._plus_operator)],
        [prec.left, PREC.times, $._times_operator],
        [prec.left, PREC.rational, $._rational_operator],
        [prec.left, PREC.bitshift, $._bitshift_operator],
        [prec.left, PREC.power, $._power_operator],
      ];

      return choice(...table.map(([fn, prec, op, nl]) => fn(prec, seq(
        $._expression,
        alias(op, $.operator),
        ...(nl ? [repeat(/\r?\n/)] : []),
        $._expression,
      ))));
    },

    unary_expression: $ => prec.right(PREC.prefix, seq(
      alias(choice(
        $._tilde_operator,
        $._type_order_operator,
        $._unary_operator,
        $._unary_plus_operator,
      ), $.operator),
      $._expression,
    )),

    range_expression: $ => prec.left(PREC.colon, seq(
      $._expression,
      choice(
        token.immediate(':'),
        // mep: `a : b` with blanks on both sides is a range too (Julia's
        // `for i = n : -1 : 1`); the scanner tells it from a ternary's colon.
        alias($._spaced_colon, ':'),
      ),
      $._expression,
    )),

    splat_expression: $ => prec(PREC.colon, seq($._expression, '...')),

    ternary_expression: $ => prec.right(PREC.conditional, seq(
      $._expression,
      '?',
      $._bracket_form,
      alias($._ternary_colon, ':'),
      $._bracket_form,
    )),

    typed_expression: $ => prec(PREC.decl, seq(
      $._expression,
      '::',
      choice(
        $._primary_expression,
        $.unary_typed_expression, // mep: `g::::IsDirected` (SimpleTraits)
      ),
    )),

    unary_typed_expression: $ => prec.right(PREC.prefix, seq(
      '::',
      $._primary_expression,
    )),

    arrow_function_expression: $ => prec.right(PREC.afunc, seq(
      choice(
        $.identifier,
        alias($.tuple_expression, $.argument_list),
        $.typed_expression,
        // mep: Julia takes any operand before `->`: `@nospecialize(x) -> ...`,
        // `::T -> ...`, `$arg -> $body`, `f(x) -> ...` (patterns in MLStyle).
        alias($._closed_macrocall_expression, $.macrocall_expression),
        $.unary_typed_expression,
        $.interpolation_expression,
        $.call_expression,
      ),
      '->',
      $._bracket_form,
    )),

    juxtaposition_expression: $ => prec.left(seq(
      choice(
        $.integer_literal,
        $.float_literal,
        $.adjoint_expression,
        // mep: a closing bracket followed, with no space, by a name:
        // `(2μ-σ)t`, `T(2)x`, `[1,2]u"km"`, `rand()u"m"`, `x[1]y`. The
        // scanner's zero-width `_juxtapose` checks the "no space" and that
        // the name is not a keyword (`f(x)in y`).
        seq(
          choice(
            $.parenthesized_expression,
            $.call_expression,
            $.index_expression,
            $.vector_expression,
            $.matrix_expression,
          ),
          $._juxtapose,
        ),
      ),
      $._juxtaposable_primary,
    )),

    compound_assignment_expression: $ => choice(
      prec.right(PREC.assign, seq(
        $._primary_expression,
        alias($._assignment_operator, $.operator),
        $._expression,
      )),
      // mep: `~` is a call at assignment precedence in Julia: any operand on
      // the left (`0 ~ x`, `a + b ~ c`), and it may stand inside brackets
      // (`[D(x) ~ a*x, ...]`), so it binds tighter than a matrix row's
      // juxtaposition and looser than any binary operator.
      prec.right(PREC.tilde, seq(
        $._expression,
        alias($._tilde_operator, $.operator),
        $._expression,
      )),
    ),

    where_expression: $ => prec.left(PREC.where, seq(
      $._expression,
      'where',
      $._expression,
    )),


    // Tokens

    macro_identifier: $ => seq('@', choice(
      $.identifier,
      $.operator,
      alias($._syntactic_operator, $.operator),
      alias($._scoped_identifier, $.field_expression),
    )),

    _scoped_identifier: $ => seq(
      choice($.identifier, $.interpolation_expression),
      repeat1(
        seq(
          token.immediate('.'),
          choice($.identifier, $.interpolation_expression),
        ),
      ),
    ),

    _word_identifier: _ => {
      const nonIdentifierCharacters = [
        '#',
        '$',
        ',',
        ':',
        ';',
        '@',
        '~',
        '(', ')',
        '{', '}',
        ...Object.values(OPERATORS),
      ].join(' ')
        .trim()
        .replace(/!/g, '')
        .replace(/-/g, '')
        .replace(/\\/g, '\\\\')
        .replace(/\s+/g, '');

      // Some symbols in Sm and So unicode categories that are identifiers
      const validMathSymbols = '°∀-∇∎-∑∫-∳' +
        // mep: the rest of the symbols Julia lets a name start with
        // (`Base.is_id_start_char`): `∞`, `⊥`, `⊤`, box drawing and
        // geometric shapes (`◣`), dingbats, and the emoji planes (`😎`,
        // `🐰`, `⛵`) -- operator characters left out.
        [
          '221E-2222', '223F', '22A4-22A5', '22BE-22C3',
          '2300-2307', '230C-231F', '2322-2328', '232B-233E', '2340-237B', '237D-239A',
          '23B4-23DB', '23E2-23FF', '2500-25B6', '25B8-25C0', '25C2-2767', '2794-27C1',
          '2B00-2B2F', '2B4D-2B73', '1F000-1FAFF',
        ].map(r => r.split('-').map(c => '\\x{' + c + '}').join('-')).join('');

      // (Upstream left emoji out for fear of the parser's size; as ranges in
      // one character class they cost no parse states.)
      const start = `[_\\p{XID_Start}${validMathSymbols}&&[^0-9#*]]`;
      const rest = `[^"'\`\\s\\.\\-\\[\\]${nonIdentifierCharacters}]*`;
      // A PATTERN rule rather than `new RegExp(...)`: the regex is Rust's
      // syntax (`\p{XID_Start}`, `&&` class intersection), which only the
      // generator reads (mep-ts-generate's JS engine refuses \p{} in a
      // RegExp it would have to run itself).
      return {type: 'PATTERN', value: start + rest};
    },

    identifier: $ => $._word_identifier,

    // Literals

    boolean_literal: _ => choice('true', 'false'),

    integer_literal: _ => choice(
      token(seq('0b', numeral('01'))),
      token(seq('0o', numeral('0-7'))),
      token(seq('0x', numeral('0-9a-fA-F'))),
      numeral('0-9'),
    ),

    float_literal: _ => {
      const dec = numeral('0-9');
      const hex = numeral('0-9a-fA-F');
      const exponent = /[eEf][+-]?\d+/;
      const hex_exponent = /[pP][+-]?\d+/; // mep: `0x1.P0`

      const leading_period = token(seq(
        '.',
        dec,
        optional(exponent),
      ));

      // This has to be split into two tokens to avoid conflicts with ellipsis
      const trailing_period = seq(
        dec,
        token.immediate(seq(
          '.',
          optional(dec),
          optional(exponent),
        )),
      );

      const just_exponent = token(seq(dec, exponent));

      const hex_float = token(seq(
        choice(
          seq('0x', hex, optional('.'), optional(hex)),
          seq('0x.', hex),
        ),
        hex_exponent,
      ));

      return choice(leading_period, trailing_period, just_exponent, hex_float);
    },

    _string: $ => choice(
      $.character_literal,
      $.string_literal,
      $.command_literal,
      $.prefixed_string_literal,
      $.prefixed_command_literal,
    ),

    escape_sequence: _ => ESCAPE_SEQUENCE,

    character_literal: _ => token(seq(
      '\'',
      choice(
        /[^'\\]/,
        '\'', // mep: `'''` is the quote character
        // mep: Julia's escapes in a character: `\x7` (one or two hex
        // digits), `\U001f428` (up to eight)
        seq('\\', choice(
          /[^uUx0-7]/,
          /u[0-9a-fA-F]{1,4}/,
          /U[0-9a-fA-F]{1,8}/,
          /[0-7]{1,3}/,
          /x[0-9a-fA-F]{1,2}/,
        )),
      ),
      '\'',
    )),

    _delimiter_str_1: _ => '"',
    _delimiter_str_3: _ => '"""',
    _delimiter_cmd_1: _ => '`',
    _delimiter_cmd_3: _ => '```',

    string_literal: $ => choice(
      seq(
        $._delimiter_str_1,
        repeat(choice(alias($._content_str_1, $.content), $.string_interpolation, $.escape_sequence)),
        $._end_str,
      ),
      seq(
        $._delimiter_str_3,
        repeat(choice(alias($._content_str_3, $.content), $.string_interpolation, $.escape_sequence)),
        $._end_str,
      ),
    ),

    command_literal: $ => choice(
      seq(
        $._delimiter_cmd_1,
        repeat(choice(alias($._content_cmd_1, $.content), $.string_interpolation, $.escape_sequence)),
        $._end_cmd,
      ),
      seq(
        $._delimiter_cmd_3,
        repeat(choice(alias($._content_cmd_3, $.content), $.string_interpolation, $.escape_sequence)),
        $._end_cmd,
      ),
    ),

    prefixed_string_literal: $ => prec.left(seq(
      field('prefix', $.identifier),
      $._immediate_string_start,
      choice(
        seq(
          $._delimiter_str_1,
          repeat(choice(alias($._content_str_1_raw, $.content), $.escape_sequence)),
          $._end_str,
        ),
        seq(
          $._delimiter_str_3,
          repeat(choice(alias($._content_str_3_raw, $.content), $.escape_sequence)),
          $._end_str,
        ),
      ),
      optional(field('suffix', $.identifier)),
    )),

    prefixed_command_literal: $ => prec.left(seq(
      field('prefix', $.identifier),
      $._immediate_command_start,
      choice(
        seq(
          $._delimiter_cmd_1,
          repeat(choice(alias($._content_cmd_1_raw, $.content), $.escape_sequence)),
          $._end_cmd,
        ),
        seq(
          $._delimiter_cmd_3,
          repeat(choice(alias($._content_cmd_3_raw, $.content), $.escape_sequence)),
          $._end_cmd,
        ),
      ),
      optional(field('suffix', $.identifier)),
    )),

    string_interpolation: $ => seq(
      '$',
      choice(
        $.identifier,
        seq($._immediate_paren, parenthesize($._bracket_form)),
      ),
    ),

    operator: $ => choice(
      // NOTE: Syntactic operators (&&, +=, etc) cannot be used as identifiers.
      $._pair_operator,
      $._arrow_operator,
      $._comparison_operator,
      $._pipe_left_operator,
      $._pipe_right_operator,
      $._ellipsis_operator,
      ':',
      $._plus_operator,
      $._times_operator,
      $._rational_operator,
      $._bitshift_operator,
      $._power_operator,
      $._tilde_operator,
      $._type_order_operator,
      $._unary_operator,
      $._unary_plus_operator,
    ),

    _assignment_operator: _ => choice(':=', '$=', '.=', addDot(OPERATORS.assignment)),

    _pair_operator: _ => addDot('=>'),

    _arrow_operator: _ => addDot(OPERATORS.arrow),

    _lazy_or_operator: _ => addDot('||'),

    _lazy_and_operator: _ => addDot('&&'),

    _comparison_operator: _ => addDot(OPERATORS.comparison),

    _pipe_right_operator: _ => addDot('|>'),

    _pipe_left_operator: _ => addDot('<|'),

    _ellipsis_operator: _ => token(choice('..', addDot(OPERATORS.ellipsis))),

    _plus_operator: _ => addDot(OPERATORS.plus),

    _times_operator: _ => addDot(OPERATORS.times),

    _rational_operator: _ => addDot('//'),

    _bitshift_operator: _ => addDot(OPERATORS.bitshift),

    _power_operator: _ => addDot(OPERATORS.power),


    _tilde_operator: _ => addDot('~'), // unary or assignment

    _type_order_operator: _ => addDot('<: >:'), // unary or comparison

    _unary_operator: _ => addDot(OPERATORS.unary),

    _unary_plus_operator: _ => addDot(OPERATORS.unary_plus),


    _syntactic_operator: _ => choice('$', '.', '...', '->', '?'),


    _semicolon: _ => seq(';', repeat(token.immediate(';'))),

    _terminator: $ => choice(/\r?\n/, $._semicolon),

    block_comment: $ => seq(/#=/, $._block_comment_rest),

    // FIXME: This is currently a seq to avoid conflicts with block_comment
    line_comment: _ => seq(/#/, /.*/),
  },
});

/**
 *
 * @param {RuleOrLiteral} separator
 * @param {RuleOrLiteral} rule
 */
function sep(separator, rule) {
  return optional(sep1(separator, rule));
}

/**
 *
 * @param {RuleOrLiteral} separator
 * @param {RuleOrLiteral} rule
 */
function sep1(separator, rule) {
  return seq(rule, repeat(seq(separator, rule)));
}

/**
 *
 * @param {string} operatorString
 */
function addDot(operatorString) {
  // (mep's JS engine splits on strings only.)
  const operators = operatorString.trim().replace(/\s+/g, ' ').split(' ');
  return token(seq(
    optional('.'),
    operators.length > 1 ? choice(...operators) : operators[0],
    // mep: Julia lets an operator carry a suffix -- sub/superscripts, primes,
    // combining marks: `==ₜ`, `<ₑ`, `*ₛ`, `⊔ᵢ`, `+′`.
    optional(OP_SUFFIX),
  ));
}

/**
 * @param {string} range
 */
function numeral(range) {
  return RegExp(`[${range}]([${range}]|_[${range}])*`);
}

/**
 *
 * @param {...any} rules
 */
function parenthesize(...rules) {
  return seq('(', ...rules, ')');
}
