/**
 * @file Tree-sitter grammar for C++.
 *
 * mep's own grammar, written against the C++23 grammar (ISO/IEC 14882:2024,
 * draft N4950, Annex A) and built on mep's C grammar
 * (grammars/tree-sitter-c), which it extends the way C++ extends C: the
 * declarators, statements and expressions C has keep C's rules, and C++'s
 * own (classes, templates, namespaces, references, lambdas, ...) are added
 * or override them. Its node names, fields and tree shapes are the ones
 * tree-sitter/tree-sitter-cpp 0.23 established, which mep vendored before
 * (third_party/grammars/cpp), so queries written for that grammar keep
 * working; see README.md.
 *
 * Rules are declared in the old grammar's order, and that is load-bearing:
 * where two parses of an ambiguous construct survive to the same point,
 * tree-sitter keeps the one whose node types are numbered lower, and node
 * types are numbered in declaration order. So the C base is reordered to
 * match (BASE_ORDER), and the C++ rules below are in that order too.
 *
 * C++ is ambiguous without knowing which names are types and which are
 * templates (`a < b > c;`, `T(x);`, `f(g<h>(i))`), which a grammar never
 * knows. Those ambiguities are left to the GLR parser (`conflicts`) and
 * settled by dynamic precedence: declarations win over expressions, and a
 * template argument list over a comparison, as they do in real code.
 *
 * The external scanner (src/scanner.c) reads raw strings, whose delimiter
 * the grammar cannot match: `R"x(...)x"`.
 */

const C = require('../tree-sitter-c/grammar.js');

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
  LAMBDA: 18,
  STRUCTURED_BINDING: -1,
};
PREC.NEW = PREC.CALL + 1;
PREC.THREE_WAY = PREC.RELATIONAL + 1;

// [expr.prim.fold]: every binary operator but `.` and `->`, and `,`.
const FOLD_OPERATORS = [
  '+', '-', '*', '/', '%', '^', '&', '|', '=', '<', '>', '<<', '>>',
  '+=', '-=', '*=', '/=', '%=', '^=', '&=', '|=', '>>=', '<<=',
  '==', '!=', '<=', '>=', '&&', '||', ',', '.*', '->*',
  'or', 'and', 'bitor', 'xor', 'bitand', 'not_eq',
];

// [lex.digraph]'s alternative tokens included.
const ASSIGNMENT_OPERATORS = [
  '=', '*=', '/=', '%=', '+=', '-=', '<<=', '>>=', '&=', '^=', '|=',
  'and_eq', 'or_eq', 'xor_eq',
];

// The C rules in the order the old grammar declared them. C++ has no K&R
// definitions, macro type specifiers or C's annotated declaration
// declarators, so those are left out of the base (DROPPED).
const BASE_ORDER = [
  'translation_unit', '_top_level_item', '_block_item', 'preproc_include', 'preproc_def',
  'preproc_function_def', 'preproc_params', 'preproc_call', 'preproc_if', 'preproc_ifdef', 'preproc_else',
  'preproc_elif', 'preproc_elifdef', 'preproc_if_in_field_declaration_list',
  'preproc_ifdef_in_field_declaration_list', 'preproc_else_in_field_declaration_list',
  'preproc_elif_in_field_declaration_list', 'preproc_elifdef_in_field_declaration_list',
  'preproc_if_in_enumerator_list', 'preproc_ifdef_in_enumerator_list', 'preproc_else_in_enumerator_list',
  'preproc_elif_in_enumerator_list', 'preproc_elifdef_in_enumerator_list',
  'preproc_if_in_enumerator_list_no_comma', 'preproc_ifdef_in_enumerator_list_no_comma',
  'preproc_else_in_enumerator_list_no_comma', 'preproc_elif_in_enumerator_list_no_comma',
  'preproc_elifdef_in_enumerator_list_no_comma', '_preproc_expression', 'preproc_parenthesized_expression',
  'preproc_defined', 'preproc_unary_expression', 'preproc_call_expression', 'preproc_argument_list',
  'preproc_binary_expression', 'function_definition', 'declaration', 'type_definition',
  '_type_definition_type', '_type_definition_declarators', '_declaration_modifiers',
  '_declaration_specifiers', 'linkage_specification', 'attribute_specifier', 'attribute',
  'attribute_declaration', 'ms_declspec_modifier', 'ms_based_modifier', 'ms_call_modifier',
  'ms_unaligned_ptr_modifier', 'ms_pointer_modifier', 'declaration_list', '_declarator', '_field_declarator',
  '_type_declarator', '_abstract_declarator', 'parenthesized_declarator', 'parenthesized_field_declarator',
  'parenthesized_type_declarator', 'abstract_parenthesized_declarator', 'attributed_declarator',
  'attributed_field_declarator', 'attributed_type_declarator', 'pointer_declarator',
  'pointer_field_declarator', 'pointer_type_declarator', 'abstract_pointer_declarator',
  'function_declarator', 'function_field_declarator', 'function_type_declarator',
  'abstract_function_declarator', 'array_declarator', 'array_field_declarator', 'array_type_declarator',
  'abstract_array_declarator', 'init_declarator', 'compound_statement', 'storage_class_specifier',
  'type_qualifier', 'alignas_qualifier', 'type_specifier', 'sized_type_specifier', 'enum_specifier',
  'enumerator_list', 'struct_specifier', 'union_specifier', 'field_declaration_list',
  '_field_declaration_list_item', 'field_declaration', 'bitfield_clause', 'enumerator', 'parameter_list',
  'parameter_declaration', 'attributed_statement', 'statement', '_top_level_statement',
  'labeled_statement', '_top_level_expression_statement', 'expression_statement', 'if_statement',
  'else_clause', 'switch_statement', 'case_statement', 'while_statement', 'do_statement', 'for_statement',
  '_for_statement_body', 'return_statement', 'break_statement', 'continue_statement', 'goto_statement',
  'seh_try_statement', 'seh_except_clause', 'seh_finally_clause', 'seh_leave_statement', 'expression',
  '_string', 'comma_expression', 'conditional_expression', 'assignment_expression', 'pointer_expression',
  'unary_expression', 'binary_expression', 'update_expression', 'cast_expression', 'type_descriptor',
  'sizeof_expression', 'alignof_expression', 'offsetof_expression', 'generic_expression',
  'subscript_expression', 'call_expression', 'gnu_asm_expression', 'gnu_asm_qualifier',
  'gnu_asm_output_operand_list', 'gnu_asm_output_operand', 'gnu_asm_input_operand_list',
  'gnu_asm_input_operand', 'gnu_asm_clobber_list', 'gnu_asm_goto_list', 'extension_expression',
  'argument_list', 'field_expression', 'compound_literal_expression', 'parenthesized_expression',
  'initializer_list', 'initializer_pair', 'subscript_designator', 'subscript_range_designator',
  'field_designator', 'char_literal', 'concatenated_string', 'string_literal', 'null',
];

const DROPPED = [
  '_old_style_function_definition', '_old_style_function_head', '_old_style_function_declarator',
  '_old_style_parameter_list', 'macro_type_specifier', 'variadic_parameter', '_declaration_declarator',
  '_function_declaration_declarator', '_field_declaration_declarator', '_empty_declaration',
];

/** C's grammar with its rules reordered to BASE_ORDER, DROPPED removed. */
function cBase() {
  const rules = {};
  for (const name of BASE_ORDER) rules[name] = C.grammar.rules[name];
  for (const [name, rule] of Object.entries(C.grammar.rules)) {
    if (!(name in rules) && !DROPPED.includes(name)) rules[name] = rule;
  }
  return {grammar: {...C.grammar, rules}};
}

/** The C++ items that may stand wherever a declaration may (top level, blocks). */
function cppDeclarations($) {
  return [
    $.namespace_definition,
    $.concept_definition,
    $.namespace_alias_definition,
    $.using_declaration,
    $.alias_declaration,
    $.static_assert_declaration,
    $.template_declaration,
    $.template_instantiation,
    alias($.constructor_or_destructor_definition, $.function_definition),
    alias($.operator_cast_definition, $.function_definition),
    alias($.operator_cast_declaration, $.declaration),
  ];
}

/** The C++ statements C lacks. */
function cppStatements($) {
  return [
    $.co_return_statement,
    $.co_yield_statement,
    $.for_range_loop,
    $.try_statement,
    $.throw_statement,
  ];
}

module.exports = grammar(cBase(), {
  name: 'cpp',

  externals: $ => [
    $.raw_string_delimiter,
    $.raw_string_content,
  ],

  conflicts: $ => [
    // C's.
    [$.type_specifier, $._declarator],
    [$.type_specifier, $.expression],
    [$.sized_type_specifier],
    [$.attributed_statement],
    [$._declaration_modifiers, $.attributed_statement],
    [$._top_level_item, $._top_level_statement],
    [$._block_item, $.statement],
    [$.type_qualifier, $.extension_expression],

    // C++'s: what a name is (a type, a template, a value) the grammar
    // cannot know.
    [$.template_function, $.template_type],
    [$.template_function, $.template_type, $.expression],
    [$.template_function, $.template_type, $.qualified_identifier],
    [$.template_type, $.qualified_type_identifier],
    [$.qualified_type_identifier, $.qualified_identifier],
    [$.comma_expression, $.initializer_list],
    [$.expression, $._declarator],
    [$.expression, $.structured_binding_declarator],
    [$.expression, $._declarator, $.type_specifier],
    [$.parameter_list, $.argument_list],
    [$.type_specifier, $.call_expression],
    [$._declaration_specifiers, $._constructor_specifiers],
    [$._binary_fold_operator, $._fold_operator],
    [$._function_declarator_seq],
    [$.type_specifier, $.sized_type_specifier],
    [$.initializer_pair, $.comma_expression],
    [$.expression_statement, $._for_statement_body],
    [$.init_statement, $._for_statement_body],
    [$.structured_binding_declarator, $._lambda_capture_identifier],
    [$.expression, $.identifier_parameter_pack_expansion],
    [$.expression, $._lambda_capture_identifier],
    [$.expression, $._lambda_capture],
    [$.expression, $.structured_binding_declarator, $._lambda_capture_identifier],
    [$.field_expression, $.template_method, $.template_type],
    [$.qualified_field_identifier, $.template_method, $.template_type],
  ],

  inline: ($, original) => original.concat([
    $._namespace_identifier,
  ]),

  precedences: $ => [
    [$.argument_list, $.type_qualifier],
    [$._expression_not_binary, $._class_name],
  ],

  rules: {
    // ---- the C rules C++ changes, in C's order -------------------------

    // C++ has no K&R definitions: `f(b) {}` is a constructor or a call.
    _top_level_item: $ => choice(
      $.function_definition,
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
      ...cppDeclarations($),
    ),

    _block_item: $ => choice(
      $.function_definition,
      $.linkage_specification,
      $.declaration,
      $.statement,
      $.attributed_statement,
      $.type_definition,
      $._empty_declaration,
      $.preproc_if,
      $.preproc_ifdef,
      $.preproc_include,
      $.preproc_def,
      $.preproc_function_def,
      $.preproc_call,
      ...cppDeclarations($),
    ),

    // [dcl.fct.def.general]: a body may be a function-try-block.
    function_definition: ($, original) => ({
      ...original,
      members: original.members.map(member => member.name !== 'body' ?
        member :
        field('body', choice(member.content, $.try_statement))),
    }),

    declaration: $ => seq(
      $._declaration_specifiers,
      commaSep1(field('declarator', choice(
        seq($._declarator, optional($.gnu_asm_expression)),
        $.init_declarator,
      ))),
      ';',
    ),

    _declaration_modifiers: ($, original) => choice(original, 'virtual'),

    attribute_specifier: $ => seq(choice('__attribute__', '__attribute'), '(', $.argument_list, ')'),

    _declarator: ($, original) => choice(
      original,
      $.reference_declarator,
      $.qualified_identifier,
      $.template_function,
      $.operator_name,
      $.destructor_name,
      $.structured_binding_declarator,
    ),

    _field_declarator: ($, original) => choice(
      original,
      alias($.reference_field_declarator, $.reference_declarator),
      $.template_method,
      $.operator_name,
    ),

    _type_declarator: ($, original) => choice(
      original,
      alias($.reference_type_declarator, $.reference_declarator),
    ),

    _abstract_declarator: ($, original) => choice(
      original,
      $.abstract_reference_declarator,
    ),

    // [dcl.fct]: after the parameters, cv- and ref-qualifiers, an
    // exception specification, attributes, a trailing return type, and
    // `override`/`final` or a requires-clause.
    function_declarator: $ => prec.dynamic(1, seq(
      field('declarator', $._declarator),
      $._function_declarator_seq,
    )),

    function_field_declarator: $ => prec.dynamic(1, seq(
      field('declarator', $._field_declarator),
      $._function_declarator_seq,
    )),

    abstract_function_declarator: $ => seq(
      field('declarator', optional($._abstract_declarator)),
      $._function_declarator_seq,
    ),

    // `T x(1);` and `T x{1};` ([dcl.init]).
    init_declarator: ($, original) => choice(
      original,
      seq(
        field('declarator', $._declarator),
        field('value', choice($.argument_list, $.initializer_list)),
      ),
    ),

    // `A b {};` is an initializer, not a block.
    compound_statement: (_, original) => prec(-1, original),

    // [dcl.type]: `mutable`, and C++20's `constinit` and `consteval`.
    type_qualifier: (_, original) => choice(original, 'mutable', 'constinit', 'consteval'),

    type_specifier: $ => choice(
      $.struct_specifier,
      $.union_specifier,
      $.enum_specifier,
      $.class_specifier,
      $.sized_type_specifier,
      $.primitive_type,
      $.template_type,
      $.dependent_type,
      $.placeholder_type_specifier,
      $.decltype,
      prec.right(choice(
        alias($.qualified_type_identifier, $.qualified_identifier),
        $._type_identifier,
      )),
    ),

    // `auto` is a type in C++11 on ([dcl.spec.auto]), no longer a storage
    // class.
    storage_class_specifier: (_, original) => choice(
      ...original.members.filter(member => member.value !== 'auto'),
    ),

    // [dcl.enum]: scoped enums, and a base type that may be qualified.
    enum_specifier: $ => prec.right(seq(
      'enum',
      optional(choice('class', 'struct')),
      choice(
        seq(
          field('name', $._class_name),
          optional($._enum_base_clause),
          optional(field('body', $.enumerator_list)),
        ),
        field('body', $.enumerator_list),
      ),
      optional($.attribute_specifier),
    )),

    // [class.pre]: a struct or union is a class.
    struct_specifier: $ => prec.right(seq('struct', $._class_declaration)),
    union_specifier: $ => prec.right(seq('union', $._class_declaration)),

    // A class body also holds member functions, templates, access
    // labels, friends, usings and static assertions ([class.mem]).
    _field_declaration_list_item: ($, original) => choice(
      original,
      $.template_declaration,
      alias($.inline_method_definition, $.function_definition),
      alias($.constructor_or_destructor_definition, $.function_definition),
      alias($.constructor_or_destructor_declaration, $.declaration),
      alias($.operator_cast_definition, $.function_definition),
      alias($.operator_cast_declaration, $.declaration),
      $.friend_declaration,
      seq($.access_specifier, ':'),
      $.alias_declaration,
      $.using_declaration,
      $.type_definition,
      $.static_assert_declaration,
      ';',
    ),

    // Default member initializers: `int x = 1;`, `int y{2};`.
    field_declaration: $ => seq(
      $._declaration_specifiers,
      commaSep(seq(
        field('declarator', $._field_declarator),
        optional(choice(
          $.bitfield_clause,
          field('default_value', $.initializer_list),
          seq('=', field('default_value', choice($.expression, $.initializer_list))),
        )),
      )),
      optional($.attribute_specifier),
      ';',
    ),

    parameter_list: $ => seq(
      '(',
      commaSep(choice(
        $.parameter_declaration,
        $.optional_parameter_declaration,
        $.variadic_parameter_declaration,
        '...',
      )),
      ')',
    ),

    _top_level_statement: ($, original) => choice(original, ...cppStatements($)),

    _non_case_statement: ($, original) => choice(original, ...cppStatements($)),

    // `if constexpr`, and `if (init; cond)` ([stmt.if]).
    if_statement: $ => prec.right(seq(
      'if',
      optional('constexpr'),
      field('condition', $.condition_clause),
      field('consequence', $.statement),
      optional(field('alternative', $.else_clause)),
    )),

    switch_statement: $ => seq(
      'switch',
      field('condition', $.condition_clause),
      field('body', $.compound_statement),
    ),

    while_statement: $ => seq(
      'while',
      field('condition', $.condition_clause),
      field('body', $.statement),
    ),

    // A plain `for` wins over a range-for where both parse (`for (int i`).
    _for_statement_body: (_, original) => prec.dynamic(1, original),

    // `return {a, b};` ([stmt.return]).
    return_statement: ($, original) => choice(
      original,
      seq('return', $.initializer_list, ';'),
    ),

    _string: $ => prec.left(choice(
      $.string_literal,
      $.raw_string_literal,
      $.concatenated_string,
    )),

    // [expr.ass]: an initializer list may be assigned (`a = {1, 2}`).
    assignment_expression: $ => prec.right(PREC.ASSIGNMENT, seq(
      field('left', $._assignment_left_expression),
      field('operator', choice(...ASSIGNMENT_OPERATORS)),
      field('right', choice($.expression, $.initializer_list)),
    )),

    // `not` and `compl` ([lex.digraph]).
    unary_expression: ($, original) => choice(
      original,
      prec.left(PREC.UNARY, seq(
        field('operator', choice('not', 'compl')),
        field('argument', $.expression),
      )),
    ),

    // `<=>`, and the alternative tokens.
    binary_expression: ($, original) => choice(
      original,
      ...[
        ['<=>', PREC.THREE_WAY],
        ['or', PREC.LOGICAL_OR],
        ['and', PREC.LOGICAL_AND],
        ['bitor', PREC.INCLUSIVE_OR],
        ['xor', PREC.EXCLUSIVE_OR],
        ['bitand', PREC.BITWISE_AND],
        ['not_eq', PREC.EQUAL],
      ].map(([operator, precedence]) => prec.left(precedence, seq(
        field('left', $.expression),
        field('operator', operator),
        field('right', $.expression),
      ))),
    ),

    type_descriptor: (_, original) => prec.right(original),

    // `sizeof...(Ts)` ([expr.sizeof]).
    sizeof_expression: ($, original) => prec.right(PREC.SIZEOF, choice(
      original,
      seq('sizeof', '...', '(', field('value', $.identifier), ')'),
    )),

    // C++23's multidimensional subscript: `a[i, j]` ([expr.sub]).
    subscript_expression: $ => prec(PREC.SUBSCRIPT, seq(
      field('argument', $.expression),
      field('indices', $.subscript_argument_list),
    )),

    // `int(x)`: a functional cast ([expr.type.conv]).
    call_expression: ($, original) => choice(original, seq(
      field('function', $.primitive_type),
      field('arguments', $.argument_list),
    )),

    // Braced arguments, and (for macros taking statements) blocks.
    argument_list: $ => seq(
      '(',
      commaSep(choice($.expression, $.initializer_list, $.compound_statement)),
      ')',
    ),

    // `a.~T()`, `a.template f<T>()`, `a.B::c`, and the pointer-to-member
    // operators `.*` and `->*` ([expr.ref], [expr.mptr.oper]). The old
    // grammar had no `->*`; with it, `->*` is no longer a token only
    // parenthesized expressions (folds) may hold, which split every
    // expression state there from its copy elsewhere.
    field_expression: $ => seq(
      prec(PREC.FIELD, seq(
        field('argument', $.expression),
        field('operator', choice('.', '.*', '->', '->*')),
      )),
      field('field', choice(
        prec.dynamic(1, $._field_identifier),
        alias($.qualified_field_identifier, $.qualified_identifier),
        $.destructor_name,
        $.template_method,
        alias($.dependent_field_identifier, $.dependent_name),
      )),
    ),

    // `T{a, b}` ([expr.type.conv]).
    compound_literal_expression: ($, original) => choice(
      original,
      seq(
        field('type', choice($._class_name, $.primitive_type)),
        field('value', $.initializer_list),
      ),
    ),

    // An assignment in parentheses is an assignment, not the start of a
    // fold (`(a = ... )`).
    parenthesized_expression: ($, original) => choice(
      original,
      seq('(', alias($._assignment_expression_lhs, $.assignment_expression), ')'),
    ),

    concatenated_string: $ => prec.right(seq(
      choice($.identifier, $.string_literal, $.raw_string_literal),
      choice($.string_literal, $.raw_string_literal),
      repeat(choice($.identifier, $.string_literal, $.raw_string_literal)),
    )),

    // ---- C++ ------------------------------------------------------------

    // `struct s;`, `class C;`: a type and nothing else.
    _empty_declaration: $ => seq($.type_specifier, ';'),

    // [dcl.spec.auto]: `auto`, `decltype(auto)`, and constrained
    // placeholders (`std::integral auto`).
    placeholder_type_specifier: $ => prec(1, seq(
      field('constraint', optional($.type_specifier)),
      choice($.auto, alias($.decltype_auto, $.decltype)),
    )),

    decltype_auto: $ => seq('decltype', '(', $.auto, ')'),

    decltype: $ => seq('decltype', '(', $.expression, ')'),

    _class_declaration: $ => seq(
      repeat(choice($.attribute_specifier, $.alignas_qualifier)),
      optional($.ms_declspec_modifier),
      repeat($.attribute_declaration),
      $._class_declaration_item,
    ),

    _class_declaration_item: $ => prec.right(seq(
      choice(
        field('name', $._class_name),
        seq(
          optional(field('name', $._class_name)),
          optional($.virtual_specifier),
          optional($.base_class_clause),
          field('body', $.field_declaration_list),
        ),
      ),
      optional($.attribute_specifier),
    )),

    class_specifier: $ => prec.right(seq('class', $._class_declaration)),

    _class_name: $ => prec.right(choice(
      $._type_identifier,
      $.template_type,
      alias($.qualified_type_identifier, $.qualified_identifier),
    )),

    virtual_specifier: _ => choice('final', 'override'),

    // `explicit`, and C++20's `explicit(bool)`.
    explicit_function_specifier: $ => choice(
      'explicit',
      prec(PREC.CALL, seq('explicit', '(', $.expression, ')')),
    ),

    // [class.derived]: `: public A, virtual protected B, C...`.
    base_class_clause: $ => seq(
      ':',
      commaSep1(seq(
        repeat($.attribute_declaration),
        optional(choice(
          $.access_specifier,
          seq($.access_specifier, 'virtual'),
          seq('virtual', $.access_specifier),
        )),
        $._class_name,
        optional('...'),
      )),
    ),

    _enum_base_clause: $ => prec.left(seq(
      ':',
      field('base', choice(
        alias($.qualified_type_identifier, $.qualified_identifier),
        $._type_identifier,
        $.primitive_type,
        $.sized_type_specifier,
      )),
    )),

    // `typename T::type` ([temp.res]).
    dependent_type: $ => prec.dynamic(-1, prec.right(seq('typename', $.type_specifier))),

    // ---- templates ([temp]) ----------------------------------------------

    template_declaration: $ => seq(
      'template',
      field('parameters', $.template_parameter_list),
      optional($.requires_clause),
      choice(
        $._empty_declaration,
        $.alias_declaration,
        $.declaration,
        $.template_declaration,
        $.function_definition,
        $.concept_definition,
        $.friend_declaration,
        alias($.constructor_or_destructor_declaration, $.declaration),
        alias($.constructor_or_destructor_definition, $.function_definition),
        alias($.operator_cast_declaration, $.declaration),
        alias($.operator_cast_definition, $.function_definition),
      ),
    ),

    // [temp.explicit]: `template void f<int>(int);`, `extern template ...`,
    // and `template class vector<int>;`, which has no declarator (the old
    // grammar reported one missing).
    template_instantiation: $ => prec(1, seq(
      optional('extern'),
      'template',
      choice(
        seq($._declaration_specifiers, optional(field('declarator', $._declarator))),
        field('declarator', $._declarator),
      ),
      ';',
    )),

    template_parameter_list: $ => seq(
      '<',
      commaSep(choice(
        $.parameter_declaration,
        $.optional_parameter_declaration,
        $.type_parameter_declaration,
        $.variadic_parameter_declaration,
        $.variadic_type_parameter_declaration,
        $.optional_type_parameter_declaration,
        $.template_template_parameter_declaration,
      )),
      alias(token(prec(1, '>')), '>'),
    ),

    type_parameter_declaration: $ => prec(1, seq(
      choice('typename', 'class'),
      optional($._type_identifier),
    )),

    variadic_type_parameter_declaration: $ => prec(1, seq(
      choice('typename', 'class'),
      '...',
      optional($._type_identifier),
    )),

    optional_type_parameter_declaration: $ => seq(
      choice('typename', 'class'),
      optional(field('name', $._type_identifier)),
      '=',
      field('default_type', $.type_specifier),
    ),

    template_template_parameter_declaration: $ => seq(
      'template',
      field('parameters', $.template_parameter_list),
      choice(
        $.type_parameter_declaration,
        $.variadic_type_parameter_declaration,
        $.optional_type_parameter_declaration,
      ),
    ),

    // [dcl.fct.default].
    optional_parameter_declaration: $ => seq(
      $._declaration_specifiers,
      field('declarator', optional(choice($._declarator, $.abstract_reference_declarator))),
      '=',
      field('default_value', $.expression),
    ),

    variadic_parameter_declaration: $ => seq(
      $._declaration_specifiers,
      field('declarator', choice(
        $.variadic_declarator,
        alias($.variadic_reference_declarator, $.reference_declarator),
      )),
    ),

    variadic_declarator: $ => seq('...', optional($.identifier)),

    variadic_reference_declarator: $ => seq(choice('&&', '&'), $.variadic_declarator),

    // `operator int()` ([class.conv.fct]).
    operator_cast: $ => prec.right(1, seq(
      'operator',
      $._declaration_specifiers,
      field('declarator', $._abstract_declarator),
    )),

    // ---- classes ([class]) ---------------------------------------------

    // A constructor's `: a(1), b{2}, Base(x)...`.
    field_initializer_list: $ => seq(':', commaSep1($.field_initializer)),

    field_initializer: $ => prec(1, seq(
      choice(
        $._field_identifier,
        $.template_method,
        alias($.qualified_field_identifier, $.qualified_identifier),
      ),
      choice($.initializer_list, $.argument_list),
      optional('...'),
    )),

    inline_method_definition: $ => seq(
      $._declaration_specifiers,
      field('declarator', $._field_declarator),
      choice(
        field('body', choice($.compound_statement, $.try_statement)),
        $.default_method_clause,
        $.delete_method_clause,
        $.pure_virtual_clause,
      ),
    ),

    _constructor_specifiers: $ => choice(
      $._declaration_modifiers,
      $.explicit_function_specifier,
    ),

    operator_cast_definition: $ => seq(
      repeat($._constructor_specifiers),
      field('declarator', choice(
        $.operator_cast,
        alias($.qualified_operator_cast_identifier, $.qualified_identifier),
      )),
      field('body', choice($.compound_statement, $.try_statement)),
    ),

    operator_cast_declaration: $ => prec(1, seq(
      repeat($._constructor_specifiers),
      field('declarator', choice(
        $.operator_cast,
        alias($.qualified_operator_cast_identifier, $.qualified_identifier),
      )),
      optional(seq('=', field('default_value', $.expression))),
      ';',
    )),

    // A constructor's function-try-block: `C() try : a(1) {} catch (...) {}`.
    constructor_try_statement: $ => seq(
      'try',
      optional($.field_initializer_list),
      field('body', $.compound_statement),
      repeat1($.catch_clause),
    ),

    // Constructors and destructors have no declaration specifiers' type.
    constructor_or_destructor_definition: $ => seq(
      repeat($._constructor_specifiers),
      field('declarator', $.function_declarator),
      choice(
        seq(optional($.field_initializer_list), field('body', $.compound_statement)),
        alias($.constructor_try_statement, $.try_statement),
        $.default_method_clause,
        $.delete_method_clause,
        $.pure_virtual_clause,
      ),
    ),

    constructor_or_destructor_declaration: $ => seq(
      repeat($._constructor_specifiers),
      field('declarator', $.function_declarator),
      ';',
    ),

    default_method_clause: _ => seq('=', 'default', ';'),

    delete_method_clause: _ => seq('=', 'delete', ';'),

    pure_virtual_clause: _ => seq('=', /0/, ';'),

    friend_declaration: $ => seq(
      optional('constexpr'),
      'friend',
      choice(
        $.declaration,
        $.function_definition,
        seq(optional(choice('class', 'struct', 'union')), $._class_name, ';'),
      ),
    ),

    access_specifier: _ => choice('public', 'private', 'protected'),

    // ---- declarators ([dcl.decl]) ----------------------------------------

    reference_declarator: $ => prec.dynamic(1, prec.right(seq(choice('&', '&&'), $._declarator))),
    reference_field_declarator: $ => prec.dynamic(1, prec.right(seq(choice('&', '&&'), $._field_declarator))),
    reference_type_declarator: $ => prec.dynamic(1, prec.right(seq(choice('&', '&&'), $._type_declarator))),
    abstract_reference_declarator: $ => prec.right(seq(choice('&', '&&'), optional($._abstract_declarator))),

    // `auto [a, b] = t;` ([dcl.struct.bind]).
    structured_binding_declarator: $ => prec.dynamic(PREC.STRUCTURED_BINDING, seq(
      '[', commaSep1($.identifier), ']',
    )),

    ref_qualifier: _ => choice('&', '&&'),

    _function_declarator_seq: $ => seq(
      field('parameters', $.parameter_list),
      optional($._function_attributes_start),
      optional($.ref_qualifier),
      optional($._function_exception_specification),
      optional($._function_attributes_end),
      optional($.trailing_return_type),
      optional($._function_postfix),
    ),

    _function_attributes_start: $ => prec(1, choice(
      seq(repeat1($.attribute_specifier), repeat($.type_qualifier)),
      seq(repeat($.attribute_specifier), repeat1($.type_qualifier)),
    )),

    _function_exception_specification: $ => choice($.noexcept, $.throw_specifier),

    _function_attributes_end: $ => prec.right(seq(
      optional($.gnu_asm_expression),
      choice(
        seq(repeat1($.attribute_specifier), repeat($.attribute_declaration)),
        seq(repeat($.attribute_specifier), repeat1($.attribute_declaration)),
      ),
    )),

    _function_postfix: $ => prec.right(choice(
      repeat1($.virtual_specifier),
      $.requires_clause,
    )),

    trailing_return_type: $ => seq('->', $.type_descriptor),

    noexcept: $ => prec.right(seq('noexcept', optional(seq('(', optional($.expression), ')')))),

    // C++03's dynamic exception specification, `throw(A, B)`.
    throw_specifier: $ => seq('throw', seq('(', commaSep($.type_descriptor), ')')),

    template_type: $ => seq(
      field('name', $._type_identifier),
      field('arguments', $.template_argument_list),
    ),

    template_method: $ => seq(
      field('name', choice($._field_identifier, $.operator_name)),
      field('arguments', $.template_argument_list),
    ),

    template_function: $ => seq(
      field('name', $.identifier),
      field('arguments', $.template_argument_list),
    ),

    // A type wins over an expression where both parse (`f<T*>`), and a
    // closing `>` ends the list before it can be a comparison.
    template_argument_list: $ => seq(
      '<',
      commaSep(choice(
        prec.dynamic(3, $.type_descriptor),
        prec.dynamic(2, alias($.type_parameter_pack_expansion, $.parameter_pack_expansion)),
        prec.dynamic(1, $.expression),
      )),
      alias(token(prec(1, '>')), '>'),
    ),

    // ---- namespaces ([basic.namespace]) ----------------------------------

    namespace_definition: $ => seq(
      optional('inline'),
      'namespace',
      optional(field('name', choice($._namespace_identifier, $.nested_namespace_specifier))),
      field('body', $.declaration_list),
    ),

    namespace_alias_definition: $ => seq(
      'namespace',
      field('name', $._namespace_identifier),
      '=',
      choice($._namespace_identifier, $.nested_namespace_specifier),
      ';',
    ),

    _namespace_specifier: $ => seq(optional('inline'), $._namespace_identifier),

    // `namespace a::inline b::c {` (C++17/20).
    nested_namespace_specifier: $ => prec(1, seq(
      optional($._namespace_specifier),
      '::',
      choice($.nested_namespace_specifier, $._namespace_specifier),
    )),

    using_declaration: $ => seq(
      'using',
      optional(choice('namespace', 'enum')),
      choice($.identifier, $.qualified_identifier),
      ';',
    ),

    alias_declaration: $ => seq(
      'using',
      field('name', $._type_identifier),
      repeat($.attribute_declaration),
      '=',
      field('type', $.type_descriptor),
      ';',
    ),

    static_assert_declaration: $ => seq(
      'static_assert',
      '(',
      field('condition', $.expression),
      optional(seq(',', field('message', choice($.string_literal, $.raw_string_literal, $.concatenated_string)))),
      ')',
      ';',
    ),

    concept_definition: $ => seq('concept', field('name', $.identifier), '=', $.expression, ';'),

    // ---- statements ([stmt]) ---------------------------------------------

    for_range_loop: $ => seq('for', '(', $._for_range_loop_body, ')', field('body', $.statement)),

    _for_range_loop_body: $ => seq(
      field('initializer', optional($.init_statement)),
      $._declaration_specifiers,
      field('declarator', $._declarator),
      ':',
      field('right', choice($.expression, $.initializer_list)),
    ),

    init_statement: $ => choice(
      $.alias_declaration,
      $.type_definition,
      $.declaration,
      $.expression_statement,
    ),

    condition_clause: $ => seq(
      '(',
      field('initializer', optional($.init_statement)),
      field('value', choice(
        $.expression,
        $.comma_expression,
        alias($.condition_declaration, $.declaration),
      )),
      ')',
    ),

    condition_declaration: $ => seq(
      $._declaration_specifiers,
      field('declarator', $._declarator),
      choice(
        seq('=', field('value', $.expression)),
        field('value', $.initializer_list),
      ),
    ),

    co_return_statement: $ => seq('co_return', optional($.expression), ';'),

    co_yield_statement: $ => seq('co_yield', $.expression, ';'),

    throw_statement: $ => seq('throw', optional($.expression), ';'),

    try_statement: $ => seq(
      'try',
      field('body', $.compound_statement),
      repeat1($.catch_clause),
    ),

    catch_clause: $ => seq(
      'catch',
      field('parameters', $.parameter_list),
      field('body', $.compound_statement),
    ),

    // ---- expressions ([expr]) --------------------------------------------

    // `R"delim(...)delim"` ([lex.string]); the delimiter is matched by the
    // scanner.
    raw_string_literal: $ => seq(
      choice('R"', 'LR"', 'uR"', 'UR"', 'u8R"'),
      choice(
        seq(
          field('delimiter', $.raw_string_delimiter),
          '(',
          $.raw_string_content,
          ')',
          $.raw_string_delimiter,
        ),
        seq('(', $.raw_string_content, ')'),
      ),
      '"',
    ),

    subscript_argument_list: $ => seq(
      '[',
      commaSep(choice($.expression, $.initializer_list)),
      ']',
    ),

    co_await_expression: $ => prec.left(PREC.UNARY, seq(
      field('operator', 'co_await'),
      field('argument', $.expression),
    )),

    // [expr.new]: `::new (place) T[n](args)`.
    new_expression: $ => prec.right(PREC.NEW, seq(
      optional('::'),
      'new',
      field('placement', optional($.argument_list)),
      field('type', $.type_specifier),
      field('declarator', optional($.new_declarator)),
      field('arguments', optional(choice($.argument_list, $.initializer_list))),
    )),

    new_declarator: $ => prec.right(seq(
      '[',
      field('length', $.expression),
      ']',
      optional($.new_declarator),
    )),

    delete_expression: $ => seq(
      optional('::'),
      'delete',
      optional(seq('[', ']')),
      $.expression,
    ),

    // [expr.prim.req]: requires-expressions and their requirements.
    type_requirement: $ => seq('typename', $._class_name),

    compound_requirement: $ => seq(
      '{', $.expression, '}',
      optional('noexcept'),
      optional($.trailing_return_type),
      ';',
    ),

    _requirement: $ => choice(
      alias($.expression_statement, $.simple_requirement),
      $.type_requirement,
      $.compound_requirement,
    ),

    requirement_seq: $ => seq('{', repeat($._requirement), '}'),

    constraint_conjunction: $ => prec.left(PREC.LOGICAL_AND, seq(
      field('left', $._requirement_clause_constraint),
      field('operator', choice('&&', 'and')),
      field('right', $._requirement_clause_constraint),
    )),

    constraint_disjunction: $ => prec.left(PREC.LOGICAL_OR, seq(
      field('left', $._requirement_clause_constraint),
      field('operator', choice('||', 'or')),
      field('right', $._requirement_clause_constraint),
    )),

    // [temp.pre]: a requires-clause holds primary expressions only.
    _requirement_clause_constraint: $ => choice(
      $.true,
      $.false,
      $._class_name,
      $.fold_expression,
      $.lambda_expression,
      $.requires_expression,
      seq('(', $.expression, ')'),
      $.constraint_conjunction,
      $.constraint_disjunction,
    ),

    requires_clause: $ => seq('requires', field('constraint', $._requirement_clause_constraint)),

    requires_parameter_list: $ => seq(
      '(',
      commaSep(choice(
        $.parameter_declaration,
        $.optional_parameter_declaration,
        $.variadic_parameter_declaration,
      )),
      ')',
    ),

    requires_expression: $ => seq(
      'requires',
      field('parameters', optional(alias($.requires_parameter_list, $.parameter_list))),
      field('requirements', $.requirement_seq),
    ),

    // [expr.prim.lambda].
    lambda_expression: $ => seq(
      field('captures', $.lambda_capture_specifier),
      optional(seq(
        field('template_parameters', $.template_parameter_list),
        optional(field('constraint', $.requires_clause)),
      )),
      optional(field('declarator', $.abstract_function_declarator)),
      field('body', $.compound_statement),
    ),

    lambda_capture_specifier: $ => prec(PREC.LAMBDA, seq(
      '[',
      choice(
        $.lambda_default_capture,
        commaSep($._lambda_capture),
        seq($.lambda_default_capture, ',', commaSep1($._lambda_capture)),
      ),
      ']',
    )),

    lambda_default_capture: _ => choice('=', '&'),

    _lambda_capture_identifier: $ => seq(
      optional('&'),
      choice(
        $.identifier,
        $.qualified_identifier,
        alias($.identifier_parameter_pack_expansion, $.parameter_pack_expansion),
      ),
    ),

    lambda_capture_initializer: $ => seq(
      optional('&'),
      field('left', $.identifier),
      '=',
      field('right', $.expression),
    ),

    _lambda_capture: $ => choice(
      seq(optional('*'), $.this),
      $._lambda_capture_identifier,
      $.lambda_capture_initializer,
    ),

    // [expr.prim.fold]: `(... op e)`, `(e op ...)`, `(e op ... op e)`.
    _fold_operator: _ => choice(...FOLD_OPERATORS),

    _binary_fold_operator: _ => choice(...FOLD_OPERATORS.map(operator =>
      seq(field('operator', operator), '...', operator))),

    _unary_left_fold: $ => seq(
      field('left', '...'),
      field('operator', $._fold_operator),
      field('right', $.expression),
    ),

    _unary_right_fold: $ => seq(
      field('left', $.expression),
      field('operator', $._fold_operator),
      field('right', '...'),
    ),

    _binary_fold: $ => seq(
      field('left', $.expression),
      $._binary_fold_operator,
      field('right', $.expression),
    ),

    fold_expression: $ => seq(
      '(',
      choice($._unary_right_fold, $._unary_left_fold, $._binary_fold),
      ')',
    ),

    parameter_pack_expansion: $ => prec(-1, seq(field('pattern', $.expression), '...')),

    type_parameter_pack_expansion: $ => seq(field('pattern', $.type_descriptor), '...'),

    identifier_parameter_pack_expansion: $ => seq($.identifier, '...'),

    destructor_name: $ => prec(1, seq('~', $.identifier)),

    dependent_identifier: $ => seq('template', $.template_function),
    dependent_field_identifier: $ => seq('template', $.template_method),
    dependent_type_identifier: $ => seq('template', $.template_type),

    // `a::`, `::`, `A<T>::`, `decltype(x)::`, `T::template U<V>::`.
    _scope_resolution: $ => prec(1, seq(
      field('scope', optional(choice(
        $._namespace_identifier,
        $.template_type,
        $.decltype,
        alias($.dependent_type_identifier, $.dependent_name),
      ))),
      '::',
    )),

    qualified_field_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.dependent_field_identifier, $.dependent_name),
        alias($.qualified_field_identifier, $.qualified_identifier),
        $.template_method,
        $._field_identifier,
        // `p->Base::~Base()`.
        $.destructor_name,
      )),
    ),

    qualified_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.dependent_identifier, $.dependent_name),
        $.qualified_identifier,
        $.template_function,
        seq(optional('template'), $.identifier),
        $.operator_name,
        $.destructor_name,
        $.pointer_type_declarator,
      )),
    ),

    qualified_type_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.dependent_type_identifier, $.dependent_name),
        alias($.qualified_type_identifier, $.qualified_identifier),
        $.template_type,
        $._type_identifier,
      )),
    ),

    qualified_operator_cast_identifier: $ => seq(
      $._scope_resolution,
      field('name', choice(
        alias($.qualified_operator_cast_identifier, $.qualified_identifier),
        $.operator_cast,
      )),
    ),

    _assignment_expression_lhs: $ => seq(
      field('left', $.expression),
      field('operator', choice(...ASSIGNMENT_OPERATORS)),
      field('right', choice($.expression, $.initializer_list)),
    ),

    // [over.oper]: every overloadable operator, and literal operators.
    operator_name: $ => prec(1, seq(
      'operator',
      choice(
        'co_await',
        '+', '-', '*', '/', '%',
        '^', '&', '|', '~',
        '!', '=', '<', '>',
        '+=', '-=', '*=', '/=', '%=', '^=', '&=', '|=',
        '<<', '>>', '>>=', '<<=',
        '==', '!=', '<=', '>=',
        '<=>',
        '&&', '||',
        '++', '--',
        ',',
        '->*',
        '->',
        '()', '[]',
        'xor', 'bitand', 'bitor', 'compl',
        'not', 'xor_eq', 'and_eq', 'or_eq', 'not_eq',
        'and', 'or',
        seq(choice('new', 'delete'), optional('[]')),
        seq('""', $.identifier),
      ),
    )),

    // `12_km`, `"abc"s` ([lex.ext]).
    user_defined_literal: $ => seq(
      choice(
        $.number_literal,
        $.char_literal,
        $.string_literal,
        $.raw_string_literal,
        $.concatenated_string,
      ),
      $.literal_suffix,
    ),

    // ---- the C expression list, extended -------------------------------

    _expression_not_binary: ($, original) => choice(
      original,
      $.co_await_expression,
      $.requires_expression,
      $.requires_clause,
      $.template_function,
      $.qualified_identifier,
      $.new_expression,
      $.delete_expression,
      $.lambda_expression,
      $.parameter_pack_expansion,
      $.this,
      $.user_defined_literal,
      $.fold_expression,
    ),

    _assignment_left_expression: ($, original) => choice(
      original,
      $.qualified_identifier,
      $.user_defined_literal,
    ),

    // [lex.icon], [lex.fcon]: C++'s own suffixes. An integer takes `u` and
    // `l`/`ll`/`z` in either order, a floating literal `f`/`l` or C++23's
    // `f16`..`f128`/`bf16`; anything else that follows is a literal_suffix
    // (`1f`, `1.5d`, `0b2`). A leading zero means octal, and a hexadecimal
    // floating literal needs its `p` exponent.
    number_literal: _ => {
      const separator = '\'';
      const digits = digit => seq(repeat1(digit), repeat(seq(separator, repeat1(digit))));
      const decimal = /[0-9]/;
      const hex = /[0-9a-fA-F]/;
      const lengthSuffix = choice(/[lL]/, 'll', 'LL', /[zZ]/);
      const integerSuffix = choice(
        seq(/[uU]/, optional(lengthSuffix)),
        seq(lengthSuffix, optional(/[uU]/)),
      );
      const floatSuffix = choice(/[fFlL]/, /[fF](16|32|64|128)/, 'bf16', 'BF16');
      const exponent = letter => seq(letter, optional(/[-+]/), digits(decimal));
      const integer = choice(
        seq(/[1-9]/, repeat(seq(optional(separator), decimal))),
        seq('0', repeat(seq(optional(separator), /[0-7]/))),
        seq(/0[xX]/, digits(hex)),
        seq(/0[bB]/, digits(/[01]/)),
      );
      const floating = choice(
        seq(digits(decimal), '.', optional(digits(decimal)), optional(exponent(/[eE]/))),
        seq('.', digits(decimal), optional(exponent(/[eE]/))),
        seq(digits(decimal), exponent(/[eE]/)),
        seq(
          /0[xX]/,
          choice(seq(digits(hex), optional(seq('.', optional(digits(hex))))), seq('.', digits(hex))),
          exponent(/[pP]/),
        ),
      );
      return token(seq(
        optional(/[-+]/),
        choice(seq(integer, optional(integerSuffix)), seq(floating, optional(floatSuffix))),
      ));
    },

    this: _ => 'this',

    auto: _ => 'auto',

    literal_suffix: _ => token.immediate(/[a-zA-Z_]\w*/),

    _namespace_identifier: $ => alias($.identifier, $.namespace_identifier),
  },
});

function commaSep(rule) {
  return optional(commaSep1(rule));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}
