/**
 * @file Tree-sitter grammar for C.
 *
 * mep's own grammar, written against the C23 grammar (ISO/IEC 9899:2024
 * Annex A, draft N3096), the GNU C extensions mep's corpus uses (statement
 * expressions, `asm`, `__attribute__`, `__extension__`, `?:`, case ranges,
 * designator ranges) and the Microsoft ones (`__declspec`, calling
 * conventions, pointer modifiers, structured exception handling). Its node
 * names, fields and tree shapes are the ones tree-sitter/tree-sitter-c 0.24
 * established, which mep vendored before (third_party/grammars/c), so
 * queries written for that grammar keep working; see README.md.
 *
 * C cannot be parsed without knowing which names are types, which a
 * grammar never knows, so `a * b;` is both a declaration and an expression.
 * Those ambiguities are left to the GLR parser (`conflicts`) and settled by
 * dynamic precedence -- declarations win, as they do in real code.
 *
 * The preprocessor is parsed where it appears: `#if` blocks may hold
 * top-level items, statements, struct fields or enumerators, so each
 * conditional exists once per context (preprocIf below) and is aliased back
 * to the same node names.
 */

const PREC = {
  PAREN_DECLARATOR: -10,
  ASSIGNMENT: -2,
  CONDITIONAL: -1,
  DEFAULT: 0,
  LOGICAL_OR: 1,
  LOGICAL_AND: 2,
  INCLUSIVE_OR: 3,
  EXCLUSIVE_OR: 4,
  BITWISE_AND: 5,
  EQUAL: 6,
  RELATIONAL: 7,
  OFFSETOF: 8,
  SHIFT: 9,
  ADD: 10,
  MULTIPLY: 11,
  CAST: 12,
  SIZEOF: 13,
  UNARY: 14,
  CALL: 15,
  FIELD: 16,
  SUBSCRIPT: 17,
};

const SIZE_MODIFIERS = ['signed', 'unsigned', 'long', 'short'];

const PRIMITIVE_TYPES = [
  'bool', 'char', 'int', 'float', 'double', 'void',
  'size_t', 'ssize_t', 'ptrdiff_t', 'intptr_t', 'uintptr_t', 'charptr_t', 'nullptr_t', 'max_align_t',
  ...[8, 16, 32, 64].map(n => `int${n}_t`),
  ...[8, 16, 32, 64].map(n => `uint${n}_t`),
  ...[8, 16, 32, 64].map(n => `char${n}_t`),
];

module.exports = grammar({
  name: 'c',

  extras: $ => [/\s|\\\r?\n/, $.comment],

  inline: $ => [
    $._type_identifier,
    $._field_identifier,
    $._statement_identifier,
    $._non_case_statement,
    $._assignment_left_expression,
    $._expression_not_binary,
  ],

  supertypes: $ => [
    $.expression,
    $.statement,
    $.type_specifier,
    $._declarator,
    $._field_declarator,
    $._type_declarator,
    $._abstract_declarator,
  ],

  conflicts: $ => [
    [$.type_specifier, $._declarator],
    [$.type_specifier, $._declarator, $.macro_type_specifier],
    [$.type_specifier, $.expression],
    [$.type_specifier, $.expression, $.macro_type_specifier],
    [$.type_specifier, $.macro_type_specifier],
    [$.type_specifier, $.sized_type_specifier],
    [$.sized_type_specifier],
    [$.attributed_statement],
    [$._declaration_modifiers, $.attributed_statement],
    [$.enum_specifier],
    [$.type_specifier, $._old_style_parameter_list],
    [$.parameter_list, $._old_style_parameter_list],
    [$._block_item, $.statement],
    [$._top_level_item, $._top_level_statement],
    [$.type_specifier, $._top_level_expression_statement],
    [$.type_qualifier, $.extension_expression],
  ],

  word: $ => $.identifier,

  rules: {
    translation_unit: $ => repeat($._top_level_item),

    _top_level_item: $ => choice(
      $.function_definition,
      alias($._old_style_function_definition, $.function_definition),
      $.linkage_specification,
      $.declaration,
      $._top_level_statement,
      $.attributed_statement,
      $.type_definition,
      $._empty_declaration,
      $.preproc_if,
      $.preproc_ifdef,
      $.preproc_include,
      $.preproc_def,
      $.preproc_function_def,
      $.preproc_call,
    ),

    _block_item: $ => choice(
      $.function_definition,
      alias($._old_style_function_definition, $.function_definition),
      $.linkage_specification,
      $.declaration,
      $.statement,
      // `if (a) {...} #if X else if (b) {...} #endif else {...}`: an
      // if/else chain the preprocessor splits. Each arm's `else` stands on
      // its own; right after an `if`, an `else` still belongs to it.
      $.else_clause,
      $.attributed_statement,
      $.type_definition,
      $._empty_declaration,
      $.preproc_if,
      $.preproc_ifdef,
      $.preproc_include,
      $.preproc_def,
      $.preproc_function_def,
      $.preproc_call,
    ),

    // ---- the preprocessor -----------------------------------------------

    preproc_include: $ => seq(
      preprocessor('include'),
      field('path', choice(
        $.string_literal,
        $.system_lib_string,
        $.identifier,
        alias($.preproc_call_expression, $.call_expression),
      )),
      token.immediate(/\r?\n/),
    ),

    preproc_def: $ => seq(
      preprocessor('define'),
      field('name', $.identifier),
      field('value', optional($.preproc_arg)),
      token.immediate(/\r?\n/),
    ),

    preproc_function_def: $ => seq(
      preprocessor('define'),
      field('name', $.identifier),
      field('parameters', $.preproc_params),
      field('value', optional($.preproc_arg)),
      token.immediate(/\r?\n/),
    ),

    preproc_params: $ => seq(token.immediate('('), commaSep(choice($.identifier, '...')), ')'),

    preproc_call: $ => seq(
      field('directive', $.preproc_directive),
      field('argument', optional($.preproc_arg)),
      token.immediate(/\r?\n/),
    ),

    ...preprocIf('', $ => $._block_item),
    ...preprocIf('_in_field_declaration_list', $ => $._field_declaration_list_item),
    ...preprocIf('_in_enumerator_list', $ => seq($.enumerator, ',')),
    ...preprocIf('_in_enumerator_list_no_comma', $ => $.enumerator, -1),

    preproc_arg: _ => token(prec(-1, /\S([^/\n]|\/[^*]|\\\r?\n)*/)),

    preproc_directive: _ => /#[ \t]*[a-zA-Z0-9]\w*/,

    _preproc_expression: $ => choice(
      $.identifier,
      alias($.preproc_call_expression, $.call_expression),
      $.number_literal,
      $.char_literal,
      $.preproc_defined,
      alias($.preproc_unary_expression, $.unary_expression),
      alias($.preproc_binary_expression, $.binary_expression),
      alias($.preproc_parenthesized_expression, $.parenthesized_expression),
    ),

    preproc_parenthesized_expression: $ => seq('(', $._preproc_expression, ')'),

    preproc_defined: $ => choice(
      prec(PREC.CALL, seq('defined', '(', $.identifier, ')')),
      seq('defined', $.identifier),
    ),

    preproc_unary_expression: $ => prec.left(PREC.UNARY, seq(
      field('operator', choice('!', '~', '-', '+')),
      field('argument', $._preproc_expression),
    )),

    preproc_call_expression: $ => prec(PREC.CALL, seq(
      field('function', $.identifier),
      field('arguments', alias($.preproc_argument_list, $.argument_list)),
    )),

    preproc_argument_list: $ => seq('(', commaSep($._preproc_expression), ')'),

    preproc_binary_expression: $ => choice(...binaryOperators().map(([operator, precedence]) =>
      prec.left(precedence, seq(
        field('left', $._preproc_expression),
        field('operator', operator),
        field('right', $._preproc_expression),
      )),
    )),

    // ---- declarations and definitions -----------------------------------

    function_definition: $ => seq(
      optional($.ms_call_modifier),
      $._declaration_specifiers,
      optional($.ms_call_modifier),
      field('declarator', $._declarator),
      field('body', $.compound_statement),
    ),

    // K&R: `int f(a, b) int a; char *b; { ... }`. With no parameter
    // declarations, `f(b) {}` could be either style; the modern one (`b` a
    // parameter's type) wins there, and only there.
    _old_style_function_definition: $ => choice(
      seq($._old_style_function_head, repeat1($.declaration), field('body', $.compound_statement)),
      prec.dynamic(-1, seq($._old_style_function_head, field('body', $.compound_statement))),
    ),

    _old_style_function_head: $ => seq(
      optional($.ms_call_modifier),
      $._declaration_specifiers,
      field('declarator', alias($._old_style_function_declarator, $.function_declarator)),
    ),

    declaration: $ => seq(
      $._declaration_specifiers,
      commaSep1(field('declarator', choice(
        seq(optional($.ms_call_modifier), $._declaration_declarator, optional($.gnu_asm_expression)),
        $.init_declarator,
      ))),
      ';',
    ),

    type_definition: $ => seq(
      optional('__extension__'),
      'typedef',
      $._type_definition_type,
      $._type_definition_declarators,
      repeat($.attribute_specifier),
      ';',
    ),

    _type_definition_type: $ => seq(repeat($.type_qualifier), field('type', $.type_specifier), repeat($.type_qualifier)),

    _type_definition_declarators: $ => commaSep1(field('declarator', $._type_declarator)),

    _declaration_modifiers: $ => choice(
      $.storage_class_specifier,
      $.type_qualifier,
      $.attribute_specifier,
      $.attribute_declaration,
      $.ms_declspec_modifier,
    ),

    _declaration_specifiers: $ => prec.right(seq(
      repeat($._declaration_modifiers),
      field('type', $.type_specifier),
      repeat($._declaration_modifiers),
    )),

    linkage_specification: $ => seq(
      'extern',
      field('value', $.string_literal),
      field('body', choice($.function_definition, $.declaration, $.declaration_list)),
    ),

    attribute_specifier: $ => seq('__attribute__', '(', $.argument_list, ')'),

    attribute: $ => seq(
      optional(seq(field('prefix', $.identifier), '::')),
      field('name', $.identifier),
      optional($.argument_list),
    ),

    attribute_declaration: $ => seq('[[', commaSep1($.attribute), ']]'),

    ms_declspec_modifier: $ => seq('__declspec', '(', $.identifier, ')'),

    ms_based_modifier: $ => seq('__based', $.argument_list),

    ms_call_modifier: _ => choice('__cdecl', '__clrcall', '__stdcall', '__fastcall', '__thiscall', '__vectorcall'),

    ms_restrict_modifier: _ => '__restrict',
    ms_unsigned_ptr_modifier: _ => '__uptr',
    ms_signed_ptr_modifier: _ => '__sptr',
    ms_unaligned_ptr_modifier: _ => choice('_unaligned', '__unaligned'),

    ms_pointer_modifier: $ => choice(
      $.ms_unaligned_ptr_modifier,
      $.ms_restrict_modifier,
      $.ms_unsigned_ptr_modifier,
      $.ms_signed_ptr_modifier,
    ),

    declaration_list: $ => seq('{', repeat($._block_item), '}'),

    // ---- declarators ----------------------------------------------------
    // Three families, by what the innermost name is: an ordinary
    // identifier, a field name (in a struct), or a type name (in a typedef);
    // and an abstract family with no name at all (casts, parameters).

    _declarator: $ => choice(
      $.attributed_declarator,
      $.pointer_declarator,
      $.function_declarator,
      $.array_declarator,
      $.parenthesized_declarator,
      $.identifier,
    ),

    _declaration_declarator: $ => choice(
      $.attributed_declarator,
      $.pointer_declarator,
      alias($._function_declaration_declarator, $.function_declarator),
      $.array_declarator,
      $.parenthesized_declarator,
      $.identifier,
    ),

    _field_declarator: $ => choice(
      alias($.attributed_field_declarator, $.attributed_declarator),
      alias($.pointer_field_declarator, $.pointer_declarator),
      alias($.function_field_declarator, $.function_declarator),
      alias($.array_field_declarator, $.array_declarator),
      alias($.parenthesized_field_declarator, $.parenthesized_declarator),
      $._field_identifier,
    ),

    _type_declarator: $ => choice(
      alias($.attributed_type_declarator, $.attributed_declarator),
      alias($.pointer_type_declarator, $.pointer_declarator),
      alias($.function_type_declarator, $.function_declarator),
      alias($.array_type_declarator, $.array_declarator),
      alias($.parenthesized_type_declarator, $.parenthesized_declarator),
      $._type_identifier,
      alias(choice('signed', 'unsigned', 'long', 'short'), $.primitive_type),
      $.primitive_type,
    ),

    _abstract_declarator: $ => choice(
      $.abstract_pointer_declarator,
      $.abstract_function_declarator,
      $.abstract_array_declarator,
      $.abstract_parenthesized_declarator,
    ),

    parenthesized_declarator: $ => prec.dynamic(PREC.PAREN_DECLARATOR, seq(
      '(', optional($.ms_call_modifier), $._declarator, ')',
    )),
    parenthesized_field_declarator: $ => prec.dynamic(PREC.PAREN_DECLARATOR, seq(
      '(', optional($.ms_call_modifier), $._field_declarator, ')',
    )),
    parenthesized_type_declarator: $ => prec.dynamic(PREC.PAREN_DECLARATOR, seq(
      '(', optional($.ms_call_modifier), $._type_declarator, ')',
    )),
    abstract_parenthesized_declarator: $ => prec(1, seq(
      '(', optional($.ms_call_modifier), $._abstract_declarator, ')',
    )),

    attributed_declarator: $ => prec.right(seq($._declarator, repeat1($.attribute_declaration))),
    attributed_field_declarator: $ => prec.right(seq($._field_declarator, repeat1($.attribute_declaration))),
    attributed_type_declarator: $ => prec.right(seq($._type_declarator, repeat1($.attribute_declaration))),

    pointer_declarator: $ => prec.dynamic(1, prec.right(seq(
      optional($.ms_based_modifier), '*', repeat($.ms_pointer_modifier), repeat($.type_qualifier),
      field('declarator', $._declarator),
    ))),
    pointer_field_declarator: $ => prec.dynamic(1, prec.right(seq(
      optional($.ms_based_modifier), '*', repeat($.ms_pointer_modifier), repeat($.type_qualifier),
      field('declarator', $._field_declarator),
    ))),
    pointer_type_declarator: $ => prec.dynamic(1, prec.right(seq(
      optional($.ms_based_modifier), '*', repeat($.ms_pointer_modifier), repeat($.type_qualifier),
      field('declarator', $._type_declarator),
    ))),
    abstract_pointer_declarator: $ => prec.dynamic(1, prec.right(seq(
      '*', repeat($.type_qualifier),
      field('declarator', optional($._abstract_declarator)),
    ))),

    // `void f(void) __attribute__((noreturn)) MACRO;`: what may follow the
    // parameters is attributes, asm labels and (unknown to the grammar)
    // annotation macros, bare or called.
    function_declarator: $ => prec.right(1, seq(
      field('declarator', $._declarator),
      field('parameters', $.parameter_list),
      optional($.gnu_asm_expression),
      repeat(choice(
        $.attribute_specifier,
        $.identifier,
        alias($.preproc_call_expression, $.call_expression),
      )),
    )),
    _function_declaration_declarator: $ => prec.right(1, seq(
      field('declarator', $._declarator),
      field('parameters', $.parameter_list),
      optional($.gnu_asm_expression),
      repeat(choice(
        $.attribute_specifier,
        $.identifier,
        alias($.preproc_call_expression, $.call_expression),
      )),
    )),
    function_field_declarator: $ => prec(1, seq(
      field('declarator', $._field_declarator),
      field('parameters', $.parameter_list),
    )),
    function_type_declarator: $ => prec(1, seq(
      field('declarator', $._type_declarator),
      field('parameters', $.parameter_list),
    )),
    abstract_function_declarator: $ => prec(1, seq(
      field('declarator', optional($._abstract_declarator)),
      field('parameters', $.parameter_list),
    )),
    _old_style_function_declarator: $ => seq(
      field('declarator', $._declarator),
      field('parameters', alias($._old_style_parameter_list, $.parameter_list)),
    ),

    array_declarator: $ => prec(1, seq(
      field('declarator', $._declarator),
      '[', repeat(choice($.type_qualifier, 'static')), field('size', optional(choice($.expression, '*'))), ']',
    )),
    array_field_declarator: $ => prec(1, seq(
      field('declarator', $._field_declarator),
      '[', repeat(choice($.type_qualifier, 'static')), field('size', optional(choice($.expression, '*'))), ']',
    )),
    array_type_declarator: $ => prec(1, seq(
      field('declarator', $._type_declarator),
      '[', repeat(choice($.type_qualifier, 'static')), field('size', optional(choice($.expression, '*'))), ']',
    )),
    abstract_array_declarator: $ => prec(1, seq(
      field('declarator', optional($._abstract_declarator)),
      '[', repeat(choice($.type_qualifier, 'static')), field('size', optional(choice($.expression, '*'))), ']',
    )),

    init_declarator: $ => seq(
      field('declarator', $._declarator),
      '=',
      field('value', choice($.initializer_list, $.expression)),
    ),

    // ---- types ----------------------------------------------------------

    storage_class_specifier: _ => choice(
      'extern', 'static', 'auto', 'register', 'inline', '__inline', '__inline__', '__forceinline',
      'thread_local', '__thread',
    ),

    type_qualifier: $ => choice(
      'const', 'constexpr', 'volatile', 'restrict', '__restrict__', '__extension__', '_Atomic',
      '_Noreturn', 'noreturn', '_Nonnull',
      $.alignas_qualifier,
    ),

    alignas_qualifier: $ => seq(choice('alignas', '_Alignas'), '(', choice($.expression, $.type_descriptor), ')'),

    type_specifier: $ => choice(
      $.struct_specifier,
      $.union_specifier,
      $.enum_specifier,
      $.macro_type_specifier,
      $.sized_type_specifier,
      $.primitive_type,
      $._type_identifier,
    ),

    // `unsigned`, `long long int`, `unsigned char`, `long double`, and the
    // order-free mixes C allows (`int long unsigned`).
    sized_type_specifier: $ => choice(
      seq(
        repeat(choice(...SIZE_MODIFIERS)),
        field('type', choice(prec.dynamic(-1, $._type_identifier), $.primitive_type)),
        repeat1(choice(...SIZE_MODIFIERS)),
      ),
      seq(
        repeat1(choice(...SIZE_MODIFIERS)),
        repeat($.type_qualifier),
        field('type', optional(choice(prec.dynamic(-1, $._type_identifier), $.primitive_type))),
        repeat(choice(...SIZE_MODIFIERS)),
      ),
    ),

    primitive_type: _ => token(choice(...PRIMITIVE_TYPES)),

    enum_specifier: $ => seq(
      'enum',
      choice(
        seq(
          field('name', $._type_identifier),
          optional(seq(':', field('underlying_type', $.primitive_type))),
          field('body', optional($.enumerator_list)),
        ),
        field('body', $.enumerator_list),
      ),
      optional($.attribute_specifier),
    ),

    enumerator_list: $ => seq(
      '{',
      repeat(choice(
        seq($.enumerator, ','),
        alias($.preproc_if_in_enumerator_list, $.preproc_if),
        alias($.preproc_ifdef_in_enumerator_list, $.preproc_ifdef),
        seq($.preproc_call, ','),
      )),
      optional(seq(
        choice(
          $.enumerator,
          alias($.preproc_if_in_enumerator_list_no_comma, $.preproc_if),
          alias($.preproc_ifdef_in_enumerator_list_no_comma, $.preproc_ifdef),
          $.preproc_call,
        ),
      )),
      '}',
    ),

    struct_specifier: $ => prec.right(seq(
      'struct',
      optional($.attribute_specifier),
      optional($.ms_declspec_modifier),
      choice(
        seq(field('name', $._type_identifier), field('body', optional($.field_declaration_list))),
        field('body', $.field_declaration_list),
      ),
      optional($.attribute_specifier),
    )),

    union_specifier: $ => prec.right(seq(
      'union',
      optional($.ms_declspec_modifier),
      choice(
        seq(field('name', $._type_identifier), field('body', optional($.field_declaration_list))),
        field('body', $.field_declaration_list),
      ),
      optional($.attribute_specifier),
    )),

    field_declaration_list: $ => seq('{', repeat($._field_declaration_list_item), '}'),

    _field_declaration_list_item: $ => choice(
      $.field_declaration,
      $.preproc_def,
      $.preproc_function_def,
      $.preproc_call,
      alias($.preproc_if_in_field_declaration_list, $.preproc_if),
      alias($.preproc_ifdef_in_field_declaration_list, $.preproc_ifdef),
    ),

    field_declaration: $ => seq(
      $._declaration_specifiers,
      optional($._field_declaration_declarator),
      optional($.attribute_specifier),
      ';',
    ),

    _field_declaration_declarator: $ => commaSep1(seq(
      field('declarator', $._field_declarator),
      optional($.bitfield_clause),
    )),

    bitfield_clause: $ => seq(':', $.expression),

    enumerator: $ => seq(
      field('name', $.identifier),
      optional(seq('=', field('value', $.expression))),
    ),

    variadic_parameter: _ => '...',

    parameter_list: $ => seq(
      '(',
      choice(
        commaSep(choice($.parameter_declaration, $.variadic_parameter)),
        $.compound_statement,
      ),
      ')',
    ),

    _old_style_parameter_list: $ => seq('(', commaSep(choice($.identifier, $.variadic_parameter)), ')'),

    parameter_declaration: $ => seq(
      $._declaration_specifiers,
      optional(field('declarator', choice($._declarator, $._abstract_declarator))),
    ),

    // `FOO(int) x;`: a macro that expands to a type.
    macro_type_specifier: $ => prec.dynamic(-1, seq(
      field('name', $.identifier),
      '(', field('type', $.type_descriptor), ')',
    )),

    // `struct s;`, `enum e { ... };`: a type and nothing else. A bare
    // `NAME;` reads as an expression statement (a macro call), not this.
    _empty_declaration: $ => prec.dynamic(-1, seq($.type_specifier, ';')),

    type_descriptor: $ => seq(
      repeat($.type_qualifier),
      field('type', $.type_specifier),
      repeat($.type_qualifier),
      field('declarator', optional($._abstract_declarator)),
    ),

    // ---- statements -----------------------------------------------------

    statement: $ => choice($.case_statement, $._non_case_statement),

    _non_case_statement: $ => choice(
      $.attributed_statement,
      $.labeled_statement,
      $.compound_statement,
      $.expression_statement,
      $.if_statement,
      $.switch_statement,
      $.do_statement,
      $.while_statement,
      $.for_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.goto_statement,
      $.seh_try_statement,
      $.seh_leave_statement,
    ),

    // At the top level an expression statement may not be a binary
    // expression: `a * b;` there is a declaration.
    _top_level_statement: $ => choice(
      $.case_statement,
      $.attributed_statement,
      $.labeled_statement,
      $.compound_statement,
      alias($._top_level_expression_statement, $.expression_statement),
      $.if_statement,
      $.switch_statement,
      $.do_statement,
      $.while_statement,
      $.for_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.goto_statement,
    ),

    _top_level_expression_statement: $ => seq($._expression_not_binary, ';'),

    attributed_statement: $ => seq(repeat1($.attribute_declaration), $.statement),

    labeled_statement: $ => seq(
      field('label', $._statement_identifier),
      ':',
      choice($.declaration, $.statement),
    ),

    compound_statement: $ => seq('{', repeat($._block_item), '}'),

    expression_statement: $ => seq(optional(choice($.expression, $.comma_expression)), ';'),

    if_statement: $ => prec.right(seq(
      'if',
      field('condition', $.parenthesized_expression),
      field('consequence', $.statement),
      optional(field('alternative', $.else_clause)),
    )),

    else_clause: $ => seq('else', $.statement),

    switch_statement: $ => seq(
      'switch',
      field('condition', $.parenthesized_expression),
      field('body', $.compound_statement),
    ),

    case_statement: $ => prec.right(seq(
      choice(seq('case', field('value', $.expression)), 'default'),
      ':',
      repeat(choice($._non_case_statement, $.declaration, $.type_definition)),
    )),

    while_statement: $ => seq(
      'while',
      field('condition', $.parenthesized_expression),
      field('body', $.statement),
    ),

    do_statement: $ => seq(
      'do',
      field('body', $.statement),
      'while',
      field('condition', $.parenthesized_expression),
      ';',
    ),

    for_statement: $ => seq('for', '(', $._for_statement_body, ')', field('body', $.statement)),

    _for_statement_body: $ => seq(
      choice(
        field('initializer', $.declaration),
        seq(field('initializer', optional(choice($.expression, $.comma_expression))), ';'),
      ),
      field('condition', optional(choice($.expression, $.comma_expression))),
      ';',
      field('update', optional(choice($.expression, $.comma_expression))),
    ),

    return_statement: $ => seq('return', optional(choice($.expression, $.comma_expression)), ';'),

    break_statement: _ => seq('break', ';'),

    continue_statement: _ => seq('continue', ';'),

    goto_statement: $ => seq('goto', field('label', $._statement_identifier), ';'),

    seh_try_statement: $ => seq(
      '__try',
      field('body', $.compound_statement),
      choice($.seh_except_clause, $.seh_finally_clause),
    ),

    seh_except_clause: $ => seq('__except', field('filter', $.parenthesized_expression), field('body', $.compound_statement)),

    seh_finally_clause: $ => seq('__finally', field('body', $.compound_statement)),

    seh_leave_statement: _ => seq('__leave', ';'),

    // ---- expressions ----------------------------------------------------

    expression: $ => choice($._expression_not_binary, $.binary_expression),

    _expression_not_binary: $ => choice(
      $.conditional_expression,
      $.assignment_expression,
      $.unary_expression,
      $.update_expression,
      $.cast_expression,
      $.pointer_expression,
      $.sizeof_expression,
      $.alignof_expression,
      $.offsetof_expression,
      $.generic_expression,
      $.subscript_expression,
      $.call_expression,
      $.field_expression,
      $.compound_literal_expression,
      $.identifier,
      $.number_literal,
      $._string,
      $.true,
      $.false,
      $.null,
      $.char_literal,
      $.parenthesized_expression,
      $.gnu_asm_expression,
      $.extension_expression,
    ),

    _string: $ => prec.left(choice($.string_literal, $.concatenated_string)),

    comma_expression: $ => seq(
      field('left', $.expression),
      ',',
      field('right', choice($.expression, $.comma_expression)),
    ),

    // `a ?: b` is GNU's.
    conditional_expression: $ => prec.right(PREC.CONDITIONAL, seq(
      field('condition', $.expression),
      '?',
      optional(field('consequence', choice($.expression, $.comma_expression))),
      ':',
      field('alternative', $.expression),
    )),

    _assignment_left_expression: $ => choice(
      $.identifier,
      $.call_expression,
      $.field_expression,
      $.pointer_expression,
      $.subscript_expression,
      $.parenthesized_expression,
    ),

    assignment_expression: $ => prec.right(PREC.ASSIGNMENT, seq(
      field('left', $._assignment_left_expression),
      field('operator', choice('=', '*=', '/=', '%=', '+=', '-=', '<<=', '>>=', '&=', '^=', '|=')),
      field('right', $.expression),
    )),

    pointer_expression: $ => prec.left(PREC.CAST, seq(
      field('operator', choice('*', '&')),
      field('argument', $.expression),
    )),

    unary_expression: $ => prec.left(PREC.UNARY, seq(
      field('operator', choice('!', '~', '-', '+')),
      field('argument', $.expression),
    )),

    binary_expression: $ => choice(...binaryOperators().map(([operator, precedence]) =>
      prec.left(precedence, seq(
        field('left', $.expression),
        field('operator', operator),
        field('right', $.expression),
      )),
    )),

    update_expression: $ => {
      const op = choice('--', '++');
      return prec.right(PREC.UNARY, choice(
        seq(field('operator', op), field('argument', $.expression)),
        seq(field('argument', $.expression), field('operator', op)),
      ));
    },

    cast_expression: $ => prec(PREC.CAST, seq(
      '(', field('type', $.type_descriptor), ')',
      field('value', $.expression),
    )),

    sizeof_expression: $ => prec(PREC.SIZEOF, seq(
      'sizeof',
      choice(
        field('value', $.expression),
        seq('(', field('type', $.type_descriptor), ')'),
      ),
    )),

    alignof_expression: $ => prec(PREC.SIZEOF, seq(
      choice('__alignof__', '__alignof', '_alignof', 'alignof', '_Alignof'),
      seq('(', field('type', $.type_descriptor), ')'),
    )),

    offsetof_expression: $ => prec(PREC.OFFSETOF, seq(
      'offsetof',
      seq('(', field('type', $.type_descriptor), ',', field('member', $._field_identifier), ')'),
    )),

    generic_expression: $ => prec(PREC.CALL, seq(
      '_Generic',
      '(',
      $.expression,
      ',',
      commaSep1(seq($.type_descriptor, ':', $.expression)),
      ')',
    )),

    subscript_expression: $ => prec(PREC.SUBSCRIPT, seq(
      field('argument', $.expression),
      '[', field('index', $.expression), ']',
    )),

    call_expression: $ => prec(PREC.CALL, seq(
      field('function', $.expression),
      field('arguments', $.argument_list),
    )),

    gnu_asm_expression: $ => prec(PREC.CALL, seq(
      choice('asm', '__asm__', '__asm'),
      repeat($.gnu_asm_qualifier),
      '(',
      field('assembly_code', $._string),
      optional(seq(
        field('output_operands', $.gnu_asm_output_operand_list),
        optional(seq(
          field('input_operands', $.gnu_asm_input_operand_list),
          optional(seq(
            field('clobbers', $.gnu_asm_clobber_list),
            optional(field('goto_labels', $.gnu_asm_goto_list)),
          )),
        )),
      )),
      ')',
    )),

    gnu_asm_qualifier: _ => choice('volatile', '__volatile__', 'inline', 'goto'),

    gnu_asm_output_operand_list: $ => seq(':', commaSep(field('operand', $.gnu_asm_output_operand))),

    gnu_asm_output_operand: $ => seq(
      optional(seq('[', field('symbol', $.identifier), ']')),
      field('constraint', $.string_literal),
      '(', field('value', $.expression), ')',
    ),

    gnu_asm_input_operand_list: $ => seq(':', commaSep(field('operand', $.gnu_asm_input_operand))),

    gnu_asm_input_operand: $ => seq(
      optional(seq('[', field('symbol', $.identifier), ']')),
      field('constraint', $.string_literal),
      '(', field('value', $.expression), ')',
    ),

    gnu_asm_clobber_list: $ => seq(':', commaSep(field('register', $._string))),

    gnu_asm_goto_list: $ => seq(':', commaSep(field('label', $.identifier))),

    extension_expression: $ => seq('__extension__', $.expression),

    argument_list: $ => seq('(', commaSep(choice($.expression, $.compound_statement)), ')'),

    field_expression: $ => seq(
      prec(PREC.FIELD, seq(
        field('argument', $.expression),
        field('operator', choice('.', '->')),
      )),
      field('field', $._field_identifier),
    ),

    compound_literal_expression: $ => seq(
      '(', field('type', $.type_descriptor), ')',
      field('value', $.initializer_list),
    ),

    // `({ ... })` is a GNU statement expression.
    parenthesized_expression: $ => seq(
      '(',
      choice($.expression, $.comma_expression, $.compound_statement),
      ')',
    ),

    initializer_list: $ => seq(
      '{',
      commaSep(choice($.initializer_pair, $.expression, $.initializer_list)),
      optional(','),
      '}',
    ),

    initializer_pair: $ => choice(
      seq(
        field('designator', repeat1(choice($.subscript_designator, $.field_designator, $.subscript_range_designator))),
        '=',
        field('value', choice($.expression, $.initializer_list)),
      ),
      // GNU's old `field: value`.
      seq(field('designator', $._field_identifier), ':', field('value', choice($.expression, $.initializer_list))),
    ),

    subscript_designator: $ => seq('[', $.expression, ']'),

    subscript_range_designator: $ => seq('[', field('start', $.expression), '...', field('end', $.expression), ']'),

    field_designator: $ => seq('.', $._field_identifier),

    // ---- literals -------------------------------------------------------

    // Decimal, octal, hex and binary integers and floats, with C23's `'`
    // digit separators and any run of suffix letters (u, l, f, ..., and
    // the vendor ones), and a sign so `-1` in a constant is one token.
    number_literal: _ => {
      const separator = '\'';
      const hex = /[0-9a-fA-F]/;
      const decimal = /[0-9]/;
      const hexDigits = seq(repeat1(hex), repeat(seq(separator, repeat1(hex))));
      const decimalDigits = seq(repeat1(decimal), repeat(seq(separator, repeat1(decimal))));
      return token(seq(
        optional(/[-\+]/),
        optional(choice(/0[xX]/, /0[bB]/)),
        choice(
          seq(
            choice(decimalDigits, seq(/0[bB]/, decimalDigits), seq(/0[xX]/, hexDigits)),
            optional(seq('.', optional(hexDigits))),
          ),
          seq('.', decimalDigits),
        ),
        optional(seq(/[eEpP]/, optional(seq(optional(/[-\+]/), hexDigits)))),
        /[uUlLwWfFbBdD]*/,
      ));
    },

    char_literal: $ => seq(
      choice('L\'', 'u\'', 'U\'', 'u8\'', '\''),
      repeat1(choice($.escape_sequence, alias(token.immediate(/[^\n']/), $.character))),
      '\'',
    ),

    // `"a" "b"`, and through macros: `"%" PRId64 "\n"`, `"=" VERSION`. At
    // least one real string, so `a b` is never one.
    concatenated_string: $ => prec.right(choice(
      seq($.string_literal, repeat1(choice($.string_literal, $.identifier))),
      seq($.identifier, $.string_literal, repeat(choice($.string_literal, $.identifier))),
    )),

    string_literal: $ => seq(
      choice('L"', 'u"', 'U"', 'u8"', '"'),
      repeat(choice(alias(token.immediate(prec(1, /[^\\"\n]+/)), $.string_content), $.escape_sequence)),
      '"',
    ),

    escape_sequence: _ => token(prec(1, seq('\\', choice(
      /[^xuU]/,
      /\d{2,3}/,
      /x[0-9a-fA-F]{1,4}/,
      /u[0-9a-fA-F]{4}/,
      /U[0-9a-fA-F]{8}/,
    )))),

    system_lib_string: _ => token(seq('<', repeat(choice(/[^>\n]/, '\\>')), '>')),

    true: _ => token(choice('TRUE', 'true')),
    false: _ => token(choice('FALSE', 'false')),
    null: _ => choice('NULL', 'nullptr'),

    identifier: _ =>
      /(\p{XID_Start}|\$|_|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8})(\p{XID_Continue}|\$|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8})*/,

    _type_identifier: $ => alias($.identifier, $.type_identifier),
    _field_identifier: $ => alias($.identifier, $.field_identifier),
    _statement_identifier: $ => alias($.identifier, $.statement_identifier),

    // `//` comments run on across a backslash-newline, as in C.
    comment: _ => token(choice(
      seq('//', /(\\+(.|\r?\n)|[^\\\n])*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    )),
  },
});

/** The `#` directive token: `#`, optional blanks, the name. */
function preprocessor(command) {
  return alias(new RegExp('#[ \t]*' + command), '#' + command);
}

/**
 * `#if`/`#ifdef` and their `#elif`/`#else` arms holding `content`, as
 * rules suffixed with `suffix`; aliased to the plain names where used.
 */
function preprocIf(suffix, content, precedence = 0) {
  function alternativeBlock($) {
    return choice(
      suffix ? alias($['preproc_else' + suffix], $.preproc_else) : $.preproc_else,
      suffix ? alias($['preproc_elif' + suffix], $.preproc_elif) : $.preproc_elif,
      suffix ? alias($['preproc_elifdef' + suffix], $.preproc_elifdef) : $.preproc_elifdef,
    );
  }

  return {
    ['preproc_if' + suffix]: $ => prec(precedence, seq(
      preprocessor('if'),
      field('condition', $._preproc_expression),
      '\n',
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
      preprocessor('endif'),
    )),

    ['preproc_ifdef' + suffix]: $ => prec(precedence, seq(
      choice(preprocessor('ifdef'), preprocessor('ifndef')),
      field('name', $.identifier),
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
      preprocessor('endif'),
    )),

    ['preproc_else' + suffix]: $ => prec(precedence, seq(
      preprocessor('else'),
      repeat(content($)),
    )),

    ['preproc_elif' + suffix]: $ => prec(precedence, seq(
      preprocessor('elif'),
      field('condition', $._preproc_expression),
      '\n',
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
    )),

    ['preproc_elifdef' + suffix]: $ => prec(precedence, seq(
      choice(preprocessor('elifdef'), preprocessor('elifndef')),
      field('name', $.identifier),
      repeat(content($)),
      field('alternative', optional(alternativeBlock($))),
    )),
  };
}

/** C's binary operators with their precedence (preprocessor ones included). */
function binaryOperators() {
  return [
    ['+', PREC.ADD],
    ['-', PREC.ADD],
    ['*', PREC.MULTIPLY],
    ['/', PREC.MULTIPLY],
    ['%', PREC.MULTIPLY],
    ['||', PREC.LOGICAL_OR],
    ['&&', PREC.LOGICAL_AND],
    ['|', PREC.INCLUSIVE_OR],
    ['^', PREC.EXCLUSIVE_OR],
    ['&', PREC.BITWISE_AND],
    ['==', PREC.EQUAL],
    ['!=', PREC.EQUAL],
    ['>', PREC.RELATIONAL],
    ['>=', PREC.RELATIONAL],
    ['<=', PREC.RELATIONAL],
    ['<', PREC.RELATIONAL],
    ['<<', PREC.SHIFT],
    ['>>', PREC.SHIFT],
  ];
}

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
