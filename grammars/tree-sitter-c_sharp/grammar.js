/**
 * @file Tree-sitter grammar for C#.
 *
 * mep's own grammar, written against the C# language specification (the
 * ECMA-334 standard and its C# 8+ drafts at github.com/dotnet/csharpstandard,
 * plus the feature specifications for what came after: records, patterns,
 * raw and interpolated raw strings, collection expressions, function
 * pointers, file-local types) and Roslyn's hand-written parser
 * (src/Compilers/CSharp/Portable/Parser/LanguageParser.cs and Lexer.cs),
 * which is what decides the corners the specification leaves to prose.
 *
 * Its node names, fields, supertypes and tree shapes are the ones
 * tree-sitter/tree-sitter-c-sharp 0.23.5 established (the grammar mep loaded
 * dynamically before this one, MIT; see README.md), so queries written for
 * it keep working. So is the order of the rules: where two readings survive
 * to the same point, tree-sitter keeps the one whose nodes are numbered
 * lower, and declaration order sets the numbering. Keep new rules after the
 * ones they could compete with.
 *
 * C# is mostly context-free at the token level. The exceptions are the
 * strings whose end depends on how they started -- interpolated strings
 * (`$"..{x}.."`, `$@"..."`, `$$"""..{{x}}.."""`) and raw strings
 * (`"""..."""` with any number of quotes) -- which the external scanner,
 * src/scanner.c, reads. The preprocessor (spec §6.5) is in the grammar:
 * `#if` blocks are parsed where they commonly appear (between members,
 * statements, enum members, attributes, inside an expression) and every
 * other directive is an extra, allowed anywhere.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// Binding strength, loosest to tightest, from the spec's operator table
// (§12.4.2 "Operator precedence and associativity"), with the extra levels
// a GLR grammar needs: member access and invocation bind tightest, then
// prefix operators and casts, then `..` (C# 8 ranges), then `switch` and
// `with` expressions, then the binary operators.
const PREC = {
  SELECT: 0,
  ASSIGN: 1,
  CONDITIONAL: 2,
  COALESCING: 3,
  LOGICAL_OR: 4,
  LOGICAL_AND: 5,
  OR: 6,
  XOR: 7,
  AND: 8,
  EQUAL: 9,
  REL: 10,
  SHIFT: 11,
  ADD: 12,
  MULT: 13,
  WITH: 14,
  SWITCH: 15,
  RANGE: 16,
  CAST: 17,
  UNARY: 17,
  PREFIX: 17,
  POSTFIX: 18,
  INVOCATION: 18,
  DOT: 18,
  GENERIC: 19,
};

// The binary operators (§12.10-§12.15) and how tightly each binds. All are
// left-associative except `??` (§12.16), which is listed separately.
const BINARY_OPERATORS = [
  ['&&', PREC.LOGICAL_AND],
  ['||', PREC.LOGICAL_OR],
  ['>>', PREC.SHIFT],
  ['>>>', PREC.SHIFT],
  ['<<', PREC.SHIFT],
  ['&', PREC.AND],
  ['^', PREC.XOR],
  ['|', PREC.OR],
  ['+', PREC.ADD],
  ['-', PREC.ADD],
  ['*', PREC.MULT],
  ['/', PREC.MULT],
  ['%', PREC.MULT],
  ['<', PREC.REL],
  ['<=', PREC.REL],
  ['==', PREC.EQUAL],
  ['!=', PREC.EQUAL],
  ['>=', PREC.REL],
  ['>', PREC.REL],
];

// Simple and compound assignment (§12.21), `>>>=` from C# 11.
const ASSIGNMENT_OPERATORS = [
  '=', '+=', '-=', '*=', '/=', '%=', '&=', '^=', '|=', '<<=', '>>=', '>>>=', '??=',
];

// The operators a user-defined `operator` declaration may overload
// (§15.10.2-§15.10.3; `>>>` from C# 11): unary first, then binary.
const OVERLOADABLE_OPERATORS = [
  '!', '~', '++', '--', 'true', 'false',
  '+', '-', '*', '/', '%', '^', '|', '&', '<<', '>>', '>>>',
  '==', '!=', '>', '<', '>=', '<=',
];

// Declaration modifiers (§15.2.2, §15.3.1, §15.6.1 and the later
// additions: `file` (C# 11), `required` (C# 11)). `ref` and `scoped` are
// not here: a `ref` before `struct` is part of the struct declaration, and
// otherwise `ref`/`scoped` belong to a type (ref_type, scoped_type) or a
// parameter.
const MODIFIERS = [
  'abstract', 'async', 'const', 'extern', 'file', 'fixed', 'internal', 'new', 'override', 'partial',
  'private', 'protected', 'public', 'readonly', 'required', 'sealed', 'static', 'unsafe', 'virtual',
  'volatile',
];

// The keywords that name simple types (§8.3.1), plus `object`, `string`
// and `void`, and `nint`/`nuint` (C# 9, contextual in the language but
// reserved here like the rest).
const PREDEFINED_TYPES = [
  'bool', 'byte', 'char', 'decimal', 'double', 'float', 'int', 'long', 'object', 'sbyte', 'short',
  'string', 'uint', 'ulong', 'ushort', 'nint', 'nuint', 'void',
];

// Contextual keywords (§6.4.4) that are keywords only somewhere: in this
// grammar they are tokens of their own (so the places they are keywords
// can name them) and also identifiers everywhere else.
const CONTEXTUAL_IDENTIFIERS = [
  'alias', 'ascending', 'by', 'descending', 'equals', 'file', 'from', 'global', 'group', 'into', 'join',
  'let', 'notnull', 'on', 'orderby', 'scoped', 'select', 'unmanaged', 'var', 'when', 'where', 'yield',
];

// More contextual keywords that are identifiers outside their construct,
// which tree-sitter-c-sharp 0.23.5 did not allow as names: the attribute
// targets (`[type]` is a collection expression of `type`) and `with` (`var
// with = ...`). (`async` and `await` still are not: telling `async x =>
// ...` from a type named `async` needs the semantic context Roslyn has.)
const LATER_CONTEXTUAL_IDENTIFIERS = ['field', 'method', 'param', 'property', 'type', 'typevar', 'with'];

// Parameter and lambda-parameter modifiers that read as `modifier` nodes
// in a parameter (§15.6.2: `this` for extension methods, `scoped` (C# 11),
// `ref readonly` (C# 12)).
const PARAMETER_MODIFIERS = ['this', 'scoped', 'ref', 'out', 'in', 'readonly'];

// Where an attribute section applies (§22.3), before the `:`.
const ATTRIBUTE_TARGETS = ['field', 'event', 'method', 'param', 'property', 'return', 'type', 'typevar'];

// Unmanaged calling conventions a function pointer may name (§23.x of the
// function pointers feature spec); any other identifier is allowed too.
const CALLING_CONVENTIONS = ['Cdecl', 'Stdcall', 'Thiscall', 'Fastcall'];

// Decimal digits with C# 7's `_` separators between (not around) them.
const DECIMAL_DIGITS = /([0-9][0-9_]*[0-9]|[0-9])/;

// The `u8` suffix of a UTF-8 string literal (C# 11).
const UTF8_SUFFIX = /(u|U)8/;

module.exports = grammar({
  name: 'c_sharp',

  // The GLR conflicts C#'s grammar needs: places where a prefix reads two
  // ways and only what follows decides (`A<B>` a generic name or two
  // comparisons; `(a, b)` a tuple, a tuple type, a pattern or a parameter
  // list; `x is T y` a type or a constant; contextual keywords as names).
  conflicts: $ => [
    // Names: `A<...>` generic or not; a name or a type parameter; a
    // property subpattern's name.
    [$._simple_name, $.generic_name],
    [$._simple_name, $.type_parameter],
    [$._simple_name, $.subpattern],

    // Tuples: an element is a type, a declaration or a pattern.
    [$.tuple_element, $.type_pattern],
    [$.tuple_element, $.using_variable_declarator],
    [$.tuple_element, $.declaration_expression],

    // `(a, b)` deconstruction versus a parameter list or a name.
    [$.tuple_pattern, $.parameter],
    [$.tuple_pattern, $._simple_name],

    [$.lvalue_expression, $._name],
    [$.parameter, $.lvalue_expression],

    // Types: `T?` nullable versus conditional, `T[]` array versus element
    // access, `T*` pointer versus multiplication.
    [$.type, $.attribute],
    [$.type, $.nullable_type],
    [$.type, $.nullable_type, $.array_creation_expression],
    [$.type, $._array_base_type],
    [$.type, $._array_base_type, $.array_creation_expression],
    [$.type, $.array_creation_expression],
    [$.type, $._pointer_base_type],

    [$.qualified_name, $.member_access_expression],
    [$.qualified_name, $.explicit_interface_specifier],

    [$._array_base_type, $.stackalloc_expression],

    // A constant pattern is an expression, read as one until `is`/`case`
    // context decides.
    [$.constant_pattern, $.non_lvalue_expression],
    [$.constant_pattern, $._expression_statement_expression],
    [$.constant_pattern, $.lvalue_expression],
    [$.constant_pattern, $._name],
    [$.constant_pattern, $.lvalue_expression, $._name],

    // `Name(...)` in a pattern: a positional pattern or an invocation.
    [$.type, $._name_invocation_pattern, $.recursive_pattern],
    [$.attribute, $.type, $._name_invocation_pattern, $.recursive_pattern],

    [$.parenthesized_pattern, $._parenthesized_pattern_with_designation],

    // `[a, ..b]`: a collection expression or a list pattern.
    [$.expression_element, $.argument],
    [$.spread_element, $.range_expression],
    [$.collection_expression, $.list_pattern],

    // Contextual keywords are identifiers until what follows makes them
    // keywords (§6.4.4).
    [$._reserved_identifier, $.modifier],
    [$._reserved_identifier, $.scoped_type],
    [$._reserved_identifier, $.implicit_type],
    [$._reserved_identifier, $.from_clause],
    [$._reserved_identifier, $.implicit_type, $.var_pattern],
    [$._reserved_identifier, $.type_parameter_constraint],
    [$._reserved_identifier, $.parameter, $.scoped_type],
    [$._reserved_identifier, $.parameter],
    [$._simple_name, $.parameter],
    [$.tuple_element, $.parameter, $.declaration_expression],
    [$.parameter, $.tuple_element],

    [$.event_declaration, $.variable_declarator],

    [$.base_list],
    [$.using_directive, $.modifier],
    [$.using_directive],

    [$._constructor_declaration_initializer, $._simple_name],
  ],

  // The external scanner's tokens, in src/scanner.c's TokenType order.
  externals: $ => [
    // The `;` a namespace or type body may end with: consumed when there,
    // zero-width otherwise.
    $._optional_semi,

    // An interpolated string's opening: `$`s (and `@`) up to the quote,
    // regular, verbatim or raw.
    $.interpolation_regular_start,
    $.interpolation_verbatim_start,
    $.interpolation_raw_start,
    // Its quotes (one, or three or more for a raw string).
    $.interpolation_start_quote,
    $.interpolation_end_quote,
    // As many `{`/`}` as the string has `$`s.
    $.interpolation_open_brace,
    $.interpolation_close_brace,
    // Text between interpolations.
    $.interpolation_string_content,

    // A raw string literal: its run of quotes, its text, the closing run.
    $.raw_string_start,
    $.raw_string_end,
    $.raw_string_content,
  ],

  // Whitespace (§6.3.4, with no-break space, BOM and ideographic space),
  // comments, and the directives that may sit on any line (§6.5).
  extras: $ => [
    /[\s\u00A0\uFEFF\u3000]+/,
    $.comment,
    $.preproc_region,
    $.preproc_endregion,
    $.preproc_line,
    $.preproc_pragma,
    $.preproc_nullable,
    $.preproc_error,
    $.preproc_warning,
    $.preproc_define,
    $.preproc_undef,
  ],

  inline: $ => [
    $._namespace_member_declaration,
    $._object_creation_type,
    $._nullable_base_type,
    $._parameter_type_with_modifiers,
    $._top_level_item_no_statement,
  ],

  precedences: $ => [
    // `new { a }`: a member declarator, not a name in a block.
    [$._anonymous_object_member_declarator, $._simple_name],
    // `{` after `=>` or as a statement is a block.
    [$.block, $.initializer_expression],
  ],

  supertypes: $ => [
    $.declaration,
    $.expression,
    $.non_lvalue_expression,
    $.lvalue_expression,
    $.literal,
    $.statement,
    $.type,
    $.type_declaration,
    $.pattern,
  ],

  word: $ => $._identifier_token,

  rules: {
    // ---- Compilation units and namespaces (§14) ---------------------------

    // A file: an optional `#!` line (C# scripts, `dotnet run app.cs`), then
    // extern aliases, usings, global attributes, namespace members and --
    // since C# 9 -- top-level statements, in any order (the order rules
    // are the compiler's, not the parser's).
    compilation_unit: $ => seq(
      optional($.shebang_directive),
      repeat($._top_level_item),
    ),

    _top_level_item: $ => prec(2, choice(
      $._top_level_item_no_statement,
      $.global_statement,
    )),

    _top_level_item_no_statement: $ => choice(
      $.extern_alias_directive,
      $.using_directive,
      $.global_attribute,
      alias($.preproc_if_in_top_level, $.preproc_if),
      $._namespace_member_declaration,
      $.file_scoped_namespace_declaration,
    ),

    // A top-level statement (C# 9).
    global_statement: $ => prec(1, $.statement),

    // `extern alias X;` (§14.4).
    extern_alias_directive: $ => seq('extern', 'alias', field('name', $.identifier), ';'),

    // `using N;`, `using static T;`, `using A = T;` (§14.5), with C# 10's
    // `global` and C# 12's `unsafe` alias (an alias may now name any type).
    using_directive: $ => seq(
      optional('global'),
      'using',
      choice(
        seq(
          optional('unsafe'),
          field('name', $.identifier),
          '=',
          $.type,
        ),
        seq(
          repeat(choice('static', 'unsafe')),
          $._name,
        ),
      ),
      ';',
    ),

    // ---- Attributes (§22) ---------------------------------------------------

    // `[assembly: A]`, `[module: B]`: attributes on the whole program.
    global_attribute: $ => seq(
      '[',
      choice('assembly', 'module'),
      ':',
      commaList1($.attribute),
      optional(','),
      ']',
    ),

    attribute: $ => seq(
      field('name', $._name),
      optional($.attribute_argument_list),
    ),

    // `(positional, Named = value)`; C# 11's generic attributes are names
    // like any other.
    attribute_argument_list: $ => prec(-1, seq(
      '(',
      optionalList($.attribute_argument),
      ')',
    )),

    // A named argument is `Name = value` for a property or field, `name:
    // value` for a constructor parameter.
    attribute_argument: $ => prec(-1, seq(
      optional(prec(1, seq(field('name', $.identifier), choice(':', '=')))),
      $.expression,
    )),

    // `[target: A, B(1)]`, trailing comma allowed (§22.3).
    attribute_list: $ => seq(
      '[',
      optional($.attribute_target_specifier),
      commaList1($.attribute),
      optional(','),
      ']',
    ),

    _attribute_list: $ => choice($.attribute_list, $.preproc_if_in_attribute_list),

    // `[type: A]` is an attribute target even where `[type: x]` could be an
    // indexer's named argument.
    attribute_target_specifier: _ => prec(1, seq(choice(...ATTRIBUTE_TARGETS), ':')),

    _namespace_member_declaration: $ => choice(
      $.namespace_declaration,
      $.type_declaration,
    ),

    // `namespace A.B { ... }` (§14.3); the `;` after the brace is allowed.
    namespace_declaration: $ => seq(
      'namespace',
      field('name', $._name),
      field('body', $.declaration_list),
      $._optional_semi,
    ),

    // `namespace A.B;` (C# 10): the rest of the file is its body, but the
    // tree keeps the members as siblings.
    file_scoped_namespace_declaration: $ => seq(
      'namespace',
      field('name', $._name),
      ';',
    ),

    // ---- Type declarations (§15 classes, §16 structs, §18 interfaces,
    // §19 enums, §20 delegates, records) -----------------------------------

    type_declaration: $ => choice(
      $.class_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.interface_declaration,
      $.delegate_declaration,
      $.record_declaration,
    ),

    // Each type declaration is a header rule (attributes, modifiers,
    // keyword, name, type parameters, primary constructor parameters (C#
    // 12), base list, constraints) and a body. The parts of the header may
    // come in any order here: Roslyn reads them leniently too and reports
    // misplacement as a semantic error.
    class_declaration: $ => seq(
      $._class_declaration_initializer,
      $._declaration_list_body,
    ),

    _class_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      'class',
      field('name', $.identifier),
      repeat(choice($.type_parameter_list, $.parameter_list, $.base_list)),
      repeat($.type_parameter_constraints_clause),
    ),

    struct_declaration: $ => seq(
      $._struct_declaration_initializer,
      $._declaration_list_body,
    ),

    // `ref struct` (C# 7.2).
    _struct_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      optional('ref'),
      'struct',
      field('name', $.identifier),
      repeat(choice($.type_parameter_list, $.parameter_list, $.base_list)),
      repeat($.type_parameter_constraints_clause),
    ),

    // `enum E : byte { A, B = 2, }` or a forward `enum E;`.
    enum_declaration: $ => seq(
      $._enum_declaration_initializer,
      choice(
        seq(field('body', $.enum_member_declaration_list), $._optional_semi),
        ';',
      ),
    ),

    _enum_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      'enum',
      field('name', $.identifier),
      optional($.base_list),
    ),

    // §19.4: members separated by commas, a trailing comma allowed; `#if`
    // around a member is common enough to be parsed.
    enum_member_declaration_list: $ => seq(
      '{',
      optionalList(choice(
        $.enum_member_declaration,
        alias($.preproc_if_in_enum_member_declaration, $.preproc_if),
      )),
      optional(','),
      '}',
    ),

    enum_member_declaration: $ => seq(
      repeat($._attribute_list),
      field('name', $.identifier),
      optional(seq('=', field('value', $.expression))),
    ),

    interface_declaration: $ => seq(
      $._interface_declaration_initializer,
      $._declaration_list_body,
    ),

    _interface_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      'interface',
      field('name', $.identifier),
      field('type_parameters', optional($.type_parameter_list)),
      optional($.base_list),
      repeat($.type_parameter_constraints_clause),
    ),

    // `delegate R D<T>(P p) where T : C;` (§20.2).
    delegate_declaration: $ => seq(
      $._delegate_declaration_initializer,
      repeat($.type_parameter_constraints_clause),
      ';',
    ),

    _delegate_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      'delegate',
      field('type', $.type),
      field('name', $.identifier),
      field('type_parameters', optional($.type_parameter_list)),
      field('parameters', $.parameter_list),
    ),

    // `record R(int X);`, `record class`, `record struct` (C# 9, 10).
    record_declaration: $ => seq(
      $._record_declaration_initializer,
      $._declaration_list_body,
    ),

    _record_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      'record',
      optional(choice('class', 'struct')),
      field('name', $.identifier),
      repeat(choice($.type_parameter_list, $.parameter_list)),
      optional(alias($.record_base, $.base_list)),
      repeat($.type_parameter_constraints_clause),
    ),

    // A record's base list may pass the primary constructor's arguments to
    // its base: `record B(int X) : A(X), I;`.
    record_base: $ => choice(
      seq(':', commaList1($._name)),
      seq(':', $.primary_constructor_base_type, optional(seq(',', commaList1($._name)))),
    ),

    // A type's body, or `;` for a body-less declaration (records, and C#
    // 12's `class C;`).
    _declaration_list_body: $ => choice(
      seq(field('body', $.declaration_list), $._optional_semi),
      ';',
    ),

    primary_constructor_base_type: $ => seq(
      field('type', $._name),
      $.argument_list,
    ),

    modifier: _ => prec.right(choice(...MODIFIERS)),

    // `<in T, out U, [A] V>`: variance (§18.2.3) and attributes allowed.
    type_parameter_list: $ => seq('<', commaList1($.type_parameter), '>'),

    type_parameter: $ => seq(
      repeat($._attribute_list),
      optional(choice('in', 'out')),
      field('name', $.identifier),
    ),

    // `: Base, IFace` (§15.2.4); C# 12 lets a class with a primary
    // constructor pass arguments to its base.
    base_list: $ => seq(':', commaList1(seq($.type, optional($.argument_list)))),

    // `where T : class?, new()` (§15.2.5).
    type_parameter_constraints_clause: $ => seq(
      'where',
      $.identifier,
      ':',
      commaList1($.type_parameter_constraint),
    ),

    type_parameter_constraint: $ => choice(
      seq('class', optional('?')),
      'struct',
      'notnull',
      'unmanaged',
      $.constructor_constraint,
      field('type', $.type),
    ),

    constructor_constraint: _ => seq('new', '(', ')'),

    // ---- Members (§15.3 and following) ---------------------------------

    // `public static T operator +(T a, T b)` (§15.10), with C# 11's
    // `checked` operators and static abstract interface members
    // (`static abstract T I.operator +(...)`).
    operator_declaration: $ => seq(
      ...declarationPrefix($),
      field('type', $.type),
      optional($.explicit_interface_specifier),
      'operator',
      optional('checked'),
      field('operator', choice(...OVERLOADABLE_OPERATORS)),
      field('parameters', $.parameter_list),
      $._function_body,
    ),

    // `public static implicit operator T(U u)` (§15.10.4). The interface
    // specifier, `operator` and `checked` are read as one run, which keeps
    // the parse table small; their order is the compiler's to check.
    conversion_operator_declaration: $ => seq(
      ...declarationPrefix($),
      choice(
        'implicit',
        'explicit',
      ),
      repeat1(choice(
        $.explicit_interface_specifier,
        'operator',
        'checked',
      )),
      field('type', $.type),
      field('parameters', $.parameter_list),
      $._function_body,
    ),

    declaration_list: $ => seq(
      '{',
      repeat($.declaration),
      '}',
    ),

    // Anything a type or namespace body may hold.
    declaration: $ => choice(
      $.class_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.delegate_declaration,
      $.field_declaration,
      $.method_declaration,
      $.event_declaration,
      $.event_field_declaration,
      $.record_declaration,
      $.constructor_declaration,
      $.destructor_declaration,
      $.indexer_declaration,
      $.interface_declaration,
      $.namespace_declaration,
      $.operator_declaration,
      $.conversion_operator_declaration,
      $.property_declaration,
      $.using_directive,
      $.preproc_if,
    ),

    // `const int A = 1, B = 2;`, `int x;` (§15.4, §15.5).
    field_declaration: $ => seq(
      ...declarationPrefix($),
      $.variable_declaration,
      ';',
    ),

    // `C(int x) : base(x) { }` (§15.11).
    constructor_declaration: $ => seq(
      $._constructor_declaration_initializer,
      $._function_body,
    ),

    _constructor_declaration_initializer: $ => seq(
      ...declarationPrefix($),
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      optional($.constructor_initializer),
    ),

    // `~C() { }` (§15.13): `extern` or `unsafe` may precede it (§23.2;
    // tree-sitter-c-sharp 0.23.5 had only `extern`).
    destructor_declaration: $ => seq(
      repeat($._attribute_list),
      optional(choice('extern', 'unsafe')),
      '~',
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      $._function_body,
    ),

    // `T I.M<U>(P p) where U : C { }` (§15.6).
    method_declaration: $ => seq(
      ...declarationPrefix($),
      field('returns', $.type),
      optional($.explicit_interface_specifier),
      field('name', $.identifier),
      field('type_parameters', optional($.type_parameter_list)),
      field('parameters', $.parameter_list),
      repeat($.type_parameter_constraints_clause),
      $._function_body,
    ),

    // `event EventHandler E { add { } remove { } }` (§15.8.1).
    event_declaration: $ => seq(
      ...declarationPrefix($),
      'event',
      field('type', $.type),
      optional($.explicit_interface_specifier),
      field('name', $.identifier),
      choice(
        field('accessors', $.accessor_list),
        ';',
      ),
    ),

    // `event EventHandler A, B;`: field-like events (§15.8.2).
    event_field_declaration: $ => prec.dynamic(1, seq(
      ...declarationPrefix($),
      'event',
      $.variable_declaration,
      ';',
    )),

    accessor_list: $ => seq(
      '{',
      repeat($.accessor_declaration),
      '}',
    ),

    // `get`, `set`, `init` (C# 9), `add`, `remove`; any other name parses
    // so that a typo is one bad accessor, not a broken property.
    accessor_declaration: $ => seq(
      ...declarationPrefix($),
      field('name', choice('get', 'set', 'add', 'remove', 'init', $.identifier)),
      $._function_body,
    ),

    // `T this[int i] { get; }` (§15.9).
    indexer_declaration: $ => seq(
      ...declarationPrefix($),
      field('type', $.type),
      optional($.explicit_interface_specifier),
      'this',
      field('parameters', $.bracketed_parameter_list),
      choice(
        field('accessors', $.accessor_list),
        seq(field('value', $.arrow_expression_clause), ';'),
      ),
    ),

    bracketed_parameter_list: $ => seq(
      '[',
      optionalSeparated(choice($.parameter, $._parameter_array), ','),
      ']',
    ),

    // `T P { get; set; } = init;` or `T P => expr;` (§15.7).
    property_declaration: $ => seq(
      ...declarationPrefix($),
      field('type', $.type),
      optional($.explicit_interface_specifier),
      field('name', $.identifier),
      choice(
        seq(
          field('accessors', $.accessor_list),
          optional(seq('=', field('value', $.expression), ';')),
        ),
        seq(
          field('value', $.arrow_expression_clause),
          ';',
        ),
      ),
    ),

    // The `I.` of an explicit interface member implementation (§18.6.2).
    explicit_interface_specifier: $ => prec(PREC.DOT, seq(
      $._name,
      '.',
    )),

    // ---- Parameters and arguments (§15.6.2, §12.6.2) ---------------------

    parameter_list: $ => seq(
      '(',
      optionalSeparated(choice($.parameter, $._parameter_array), ','),
      ')',
    ),

    _parameter_type_with_modifiers: $ => seq(
      repeat(prec.left(alias(
        choice(...PARAMETER_MODIFIERS),
        $.modifier,
      ))),
      field('type', $.type),
    ),

    // A parameter's type is optional: lambda parameters may leave it out.
    parameter: $ => seq(
      repeat($._attribute_list),
      optional($._parameter_type_with_modifiers),
      field('name', $.identifier),
      optional(seq('=', $.expression)),
    ),

    // `params T[] rest` (C# 13: any collection type). Not a node of its
    // own: its parts sit in the parameter list.
    _parameter_array: $ => seq(
      repeat($._attribute_list),
      'params',
      field('type', $.type),
      field('name', $.identifier),
    ),

    // `: base(...)` / `: this(...)` (§15.11.2).
    constructor_initializer: $ => seq(
      ':',
      choice('base', 'this'),
      $.argument_list,
    ),

    argument_list: $ => seq('(', optionalList($.argument), ')'),

    // `(a, _, (b, c))`: a deconstructing declaration's names.
    tuple_pattern: $ => seq(
      '(',
      commaList1(choice(
        field('name', $.identifier),
        $.discard,
        $.tuple_pattern,
      )),
      ')',
    ),

    // `name: ref x`, `out var y`, `out int z` (§12.6.2; C# 7's out
    // variables are declaration expressions).
    argument: $ => prec(1, seq(
      optional(seq(field('name', $.identifier), ':')),
      optional(choice('ref', 'out', 'in')),
      choice(
        $.expression,
        $.declaration_expression,
      ),
    )),

    block: $ => seq('{', repeat($.statement), '}'),

    // `=> expr`: an expression-bodied member (§15.6.1).
    arrow_expression_clause: $ => seq('=>', $.expression),

    _function_body: $ => choice(
      field('body', $.block),
      seq(field('body', $.arrow_expression_clause), ';'),
      ';',
    ),

    // `T a = 1, b` (§13.6.2), shared by fields, locals, `for`, `fixed`.
    variable_declaration: $ => seq(
      field('type', $.type),
      commaList1($.variable_declarator),
    ),

    // The declaration inside `using (...)`: no deconstruction or array
    // size there.
    using_variable_declaration: $ => seq(
      field('type', $.type),
      commaList1(alias($.using_variable_declarator, $.variable_declarator)),
    ),

    // `x`, `(a, b)` (a deconstruction), `buf[16]` (a fixed-size buffer,
    // §23.8), each with an optional initializer.
    variable_declarator: $ => seq(
      choice(field('name', $.identifier), $.tuple_pattern),
      optional($.bracketed_argument_list),
      optional(seq('=', $.expression)),
    ),

    using_variable_declarator: $ => seq(
      field('name', $.identifier),
      optional(seq('=', $.expression)),
    ),

    // `[a, b]`: an indexer's arguments (§12.8.12), trailing comma allowed
    // for recovery.
    bracketed_argument_list: $ => seq(
      '[',
      commaList1($.argument),
      optional(','),
      ']',
    ),

    // ---- Names (§7.6) ----------------------------------------------------

    qualified_identifier: $ => separated1($.identifier, '.'),

    _name: $ => choice(
      $.alias_qualified_name,
      $.qualified_name,
      $._simple_name,
    ),

    // `global::System`, `alias::Name` (§14.8).
    alias_qualified_name: $ => seq(
      field('alias', $.identifier),
      '::',
      field('name', $._simple_name),
    ),

    _simple_name: $ => choice(
      $.identifier,
      $.generic_name,
    ),

    qualified_name: $ => prec(PREC.DOT, seq(
      field('qualifier', $._name),
      '.',
      field('name', $._simple_name),
    )),

    generic_name: $ => seq($.identifier, $.type_argument_list),

    // `<int, string>`, or `<,>` in an unbound `typeof(Dictionary<,>)`.
    type_argument_list: $ => seq(
      '<',
      choice(
        repeat(','),
        commaList1($.type),
      ),
      '>',
    ),

    // ---- Types (§8) --------------------------------------------------------

    type: $ => choice(
      $.implicit_type,
      $.array_type,
      $._name,
      $.nullable_type,
      $.pointer_type,
      $.function_pointer_type,
      $.predefined_type,
      $.tuple_type,
      $.ref_type,
      $.scoped_type,
    ),

    // `var` (§13.6.2): a type only where a declaration is possible, so its
    // dynamic precedence prefers that reading over `var` the identifier.
    implicit_type: _ => prec.dynamic(1, 'var'),

    // `T[]`, `T[,]`, `T[][]`, and in `new T[n]` the sizes (§17.2.1).
    array_type: $ => seq(
      field('type', $._array_base_type),
      field('rank', $.array_rank_specifier),
    ),

    _array_base_type: $ => choice(
      $.array_type,
      $._name,
      $.nullable_type,
      $.pointer_type,
      $.function_pointer_type,
      $.predefined_type,
      $.tuple_type,
    ),

    array_rank_specifier: $ => seq(
      '[',
      optionalList(optional($.expression)),
      ']',
    ),

    // `T?` (§8.3.12, and C# 8's nullable reference types).
    nullable_type: $ => seq(field('type', $._nullable_base_type), '?'),

    _nullable_base_type: $ => choice(
      $.array_type,
      $._name,
      $.predefined_type,
      $.tuple_type,
    ),

    // `T*` (§23.3).
    pointer_type: $ => seq(field('type', $._pointer_base_type), '*'),

    _pointer_base_type: $ => choice(
      $._name,
      $.nullable_type,
      $.pointer_type,
      $.function_pointer_type,
      $.predefined_type,
      $.tuple_type,
    ),

    // `delegate* unmanaged[Cdecl]<int, ref byte, void>` (C# 9): parameter
    // types, then the return type last.
    function_pointer_type: $ => seq(
      'delegate',
      '*',
      optional($.calling_convention),
      '<',
      repeat(seq($.function_pointer_parameter, ',')),
      field('returns', $.type),
      '>',
    ),

    calling_convention: $ => choice(
      'managed',
      seq(
        'unmanaged',
        optional(seq(
          '[',
          commaList1(choice(
            ...CALLING_CONVENTIONS,
            $.identifier,
          )),
          ']',
        )),
      ),
    ),

    // `ref readonly` from C# 12 (tree-sitter-c-sharp 0.23.5 had only `ref`,
    // `out`, `in`).
    function_pointer_parameter: $ => seq(
      optional(choice('ref', 'out', 'in', seq('ref', 'readonly'))),
      field('type', $._ref_base_type),
    ),

    predefined_type: _ => token(choice(...PREDEFINED_TYPES)),

    // `ref T`, `ref readonly T` (C# 7: ref returns and ref locals).
    ref_type: $ => seq(
      'ref',
      optional('readonly'),
      field('type', $.type),
    ),

    _ref_base_type: $ => choice(
      $.implicit_type,
      $._name,
      $.nullable_type,
      $.array_type,
      $.pointer_type,
      $.function_pointer_type,
      $.predefined_type,
      $.tuple_type,
    ),

    // `scoped ref T`, `scoped Span<T>` (C# 11).
    scoped_type: $ => seq(
      'scoped',
      field('type', $._scoped_base_type),
    ),

    _scoped_base_type: $ => choice(
      $._name,
      $.ref_type,
    ),

    // `(int, string name)` (§8.3.11): at least two elements.
    tuple_type: $ => seq(
      '(',
      commaList2($.tuple_element),
      ')',
    ),

    tuple_element: $ => seq(
      field('type', $.type),
      field('name', optional($.identifier)),
    ),

    // ---- Statements (§13) --------------------------------------------------

    statement: $ => prec(1, choice(
      $.block,
      $.break_statement,
      $.checked_statement,
      $.continue_statement,
      $.do_statement,
      $.empty_statement,
      $.expression_statement,
      $.fixed_statement,
      $.for_statement,
      $.return_statement,
      $.lock_statement,
      $.yield_statement,
      $.switch_statement,
      $.throw_statement,
      $.try_statement,
      $.unsafe_statement,
      $.using_statement,
      $.foreach_statement,
      $.goto_statement,
      $.labeled_statement,
      $.if_statement,
      $.while_statement,
      $.local_declaration_statement,
      $.local_function_statement,
      alias($.preproc_if_in_top_level, $.preproc_if),
    )),

    break_statement: _ => seq('break', ';'),

    checked_statement: $ => seq(choice('checked', 'unchecked'), $.block),

    continue_statement: _ => seq('continue', ';'),

    do_statement: $ => seq(
      'do',
      field('body', $.statement),
      'while',
      '(',
      field('condition', $.expression),
      ')',
      ';',
    ),

    empty_statement: _ => ';',

    // §13.7: only assignments, calls, `++`/`--`, `await` and `new` may
    // stand as statements (CS0201), which keeps `a < b;` from reading as
    // a statement and leaves room for declarations like `A<B> c;`.
    expression_statement: $ => seq($._expression_statement_expression, ';'),

    // `fixed (byte* p = buf) ...` (§23.7).
    fixed_statement: $ => seq('fixed', '(', $.variable_declaration, ')', $.statement),

    for_statement: $ => seq(
      'for',
      $._for_statement_conditions,
      field('body', $.statement),
    ),

    _for_statement_conditions: $ => seq(
      '(',
      field('initializer', optional(
        choice($.variable_declaration, commaList1($.expression)),
      )),
      ';',
      field('condition', optional($.expression)),
      ';',
      field('update', optional(commaList1($.expression))),
      ')',
    ),

    return_statement: $ => seq('return', optional($.expression), ';'),

    lock_statement: $ => seq('lock', '(', $.expression, ')', $.statement),

    // `yield return x;`, `yield break;` (§13.15).
    yield_statement: $ => seq(
      'yield',
      choice(
        seq('return', $.expression),
        'break',
      ),
      ';',
    ),

    // `switch (x) { ... }`, or `switch (a, b) { ... }` on a tuple literal.
    switch_statement: $ => seq(
      'switch',
      choice(
        seq(
          '(',
          field('value', $.expression),
          ')',
        ),
        field('value', $.tuple_expression),
      ),
      field('body', $.switch_body),
    ),

    switch_body: $ => seq('{', repeat($.switch_section), '}'),

    // One label and the statements after it; `case P when c:` (C# 7). A
    // section with several labels is several sections, the earlier ones
    // empty.
    switch_section: $ => prec.left(seq(
      choice(
        seq(
          'case',
          choice(
            $.expression,
            seq($.pattern, optional($.when_clause)),
          ),
        ),
        'default',
      ),
      ':',
      repeat($.statement),
    )),

    throw_statement: $ => seq('throw', optional($.expression), ';'),

    try_statement: $ => seq(
      'try',
      field('body', $.block),
      repeat($.catch_clause),
      optional($.finally_clause),
    ),

    // `catch (E e) when (filter) { }` (§13.11).
    catch_clause: $ => seq(
      'catch',
      repeat(choice($.catch_declaration, $.catch_filter_clause)),
      field('body', $.block),
    ),

    catch_declaration: $ => seq(
      '(',
      field('type', $.type),
      optional(field('name', $.identifier)),
      ')',
    ),

    catch_filter_clause: $ => seq('when', '(', $.expression, ')'),

    finally_clause: $ => seq('finally', $.block),

    unsafe_statement: $ => seq('unsafe', $.block),

    // `using (var r = ...) ...`, `await using (...)` (§13.14, C# 8).
    using_statement: $ => seq(
      optional('await'),
      'using',
      '(',
      choice(
        alias($.using_variable_declaration, $.variable_declaration),
        $.expression,
      ),
      ')',
      field('body', $.statement),
    ),

    foreach_statement: $ => seq(
      $._foreach_statement_initializer,
      field('body', $.statement),
    ),

    // `foreach (T x in e)`, `foreach (var (a, b) in e)`, and the
    // deconstructing `foreach ((a, b) in e)`; `await foreach` (C# 8).
    _foreach_statement_initializer: $ => seq(
      optional('await'),
      'foreach',
      '(',
      choice(
        seq(
          field('type', $.type),
          field('left', choice($.identifier, $.tuple_pattern)),
        ),
        field('left', $.expression),
      ),
      'in',
      field('right', $.expression),
      ')',
    ),

    // `goto L;`, `goto case 1;`, `goto default;` (§13.10.4).
    goto_statement: $ => seq(
      'goto',
      optional(choice('case', 'default')),
      optional($.expression),
      ';',
    ),

    labeled_statement: $ => seq(
      $.identifier,
      ':',
      $.statement,
    ),

    // A dangling `else` binds to the nearest `if` (prec.right).
    if_statement: $ => prec.right(seq(
      'if',
      '(',
      field('condition', $.expression),
      ')',
      field('consequence', $.statement),
      optional(seq(
        'else',
        field('alternative', $.statement),
      )),
    )),

    while_statement: $ => seq(
      'while',
      '(',
      field('condition', $.expression),
      ')',
      field('body', $.statement),
    ),

    // `int x = 1;`, `const int y = 2;`, `using var r = ...;` (C# 8),
    // `await using var r = ...;`, `ref int z = ref a;`.
    local_declaration_statement: $ => seq(
      optional('await'),
      optional('using'),
      repeat($.modifier),
      $.variable_declaration,
      ';',
    ),

    // A local function (C# 7): a method declared in a block.
    local_function_statement: $ => seq(
      $._local_function_declaration,
      repeat($.type_parameter_constraints_clause),
      $._function_body,
    ),

    _local_function_declaration: $ => seq(
      ...declarationPrefix($),
      field('type', $.type),
      field('name', $.identifier),
      field('type_parameters', optional($.type_parameter_list)),
      field('parameters', $.parameter_list),
    ),

    // ---- Patterns (§11) ----------------------------------------------------

    pattern: $ => choice(
      $.constant_pattern,
      $.declaration_pattern,
      $.discard,
      $.recursive_pattern,
      $.var_pattern,
      $.negated_pattern,
      // `(p) x`: a parenthesized pattern with a designation is a positional
      // pattern of one. It comes before parenthesized_pattern so that both
      // readings stay alive (a GLR conflict) until the designation decides.
      prec.dynamic(1, alias($._parenthesized_pattern_with_designation, $.recursive_pattern)),
      $.parenthesized_pattern,
      $.relational_pattern,
      $.or_pattern,
      $.and_pattern,
      $.list_pattern,
      $.type_pattern,
    ),

    _parenthesized_pattern_with_designation: $ => seq(
      '(',
      $.pattern,
      ')',
      $._variable_designation,
    ),

    // A constant pattern is a constant expression (§11.2.3); which
    // expressions are constant is semantics, so this is the expressions
    // that can be one, an invocation included (`nameof(X)`).
    constant_pattern: $ => choice(
      $.binary_expression,
      $.default_expression,
      $.interpolated_string_expression,
      $.parenthesized_expression,
      $.postfix_unary_expression,
      $.prefix_unary_expression,
      $.sizeof_expression,
      $.tuple_expression,
      $.typeof_expression,
      $.member_access_expression,
      alias($._name_invocation_pattern, $.invocation_expression),
      alias($._complex_invocation_expression, $.invocation_expression),
      $.cast_expression,
      $._simple_name,
      $.literal,
    ),

    // `Name(...)`: an invocation (`nameof(x)`) in conflict with a
    // positional pattern `Type(subpatterns)`.
    _name_invocation_pattern: $ => seq(
      field('function', $._name),
      field('arguments', $.argument_list),
    ),

    // The other invocations a constant pattern may be: on a member access,
    // an element, a call result, a parenthesized or cast expression.
    _complex_invocation_expression: $ => prec(PREC.INVOCATION, seq(
      field('function', choice(
        $.member_access_expression,
        $.element_access_expression,
        $.invocation_expression,
        $.parenthesized_expression,
        $.conditional_access_expression,
        $.cast_expression,
      )),
      field('arguments', $.argument_list),
    )),

    discard: _ => '_',

    parenthesized_pattern: $ => seq('(', $.pattern, ')'),

    var_pattern: $ => seq('var', $._variable_designation),

    // A bare type (C# 9).
    type_pattern: $ => prec.right(field('type', $.type)),

    // `[1, .., var last] list` (C# 11); slices are _slice_pattern, below.
    list_pattern: $ => prec.right(seq(
      '[',
      optional(seq(
        commaList1(choice($.pattern, $._slice_pattern)),
        optional(','),
      )),
      ']',
      optional($._variable_designation),
    )),

    // `Type(positional) { property: p } name` with every part optional but
    // one clause (§11.2.6). The alternatives are spelled out so that a name
    // followed by `(...)` stays in conflict with an invocation, and the
    // dynamic precedences prefer reading a trailing identifier as the
    // designation.
    recursive_pattern: $ => prec.left(choice(
      prec.dynamic(1, seq(
        field('type', $._name),
        $.positional_pattern_clause,
        optional($.property_pattern_clause),
        $._variable_designation,
      )),
      prec.dynamic(-1, seq(
        field('type', $._name),
        $.positional_pattern_clause,
        optional($.property_pattern_clause),
      )),
      prec.dynamic(1, seq(
        $.positional_pattern_clause,
        $._variable_designation,
      )),
      $.positional_pattern_clause,
      seq(
        field('type', $.type),
        choice(
          seq(
            $.positional_pattern_clause,
            optional($.property_pattern_clause),
          ),
          $.property_pattern_clause,
        ),
        optional($._variable_designation),
      ),
      seq(
        choice(
          seq(
            $.positional_pattern_clause,
            $.property_pattern_clause,
          ),
          $.property_pattern_clause,
        ),
        optional($._variable_designation),
      ),
    )),

    positional_pattern_clause: $ => prec(1, seq(
      '(',
      optional(optionalList($.subpattern)),
      ')',
    )),

    property_pattern_clause: $ => prec(1, seq(
      '{',
      optionalList($.subpattern),
      optional(','),
      '}',
    )),

    // `Name: p` or, since C# 10, `A.B.C: p` (an extended property
    // pattern), or just `p`.
    subpattern: $ => prec.right(seq(
      optional(
        choice(
          seq($.expression, ':'),
          seq($.identifier, ':'),
        ),
      ),
      $.pattern,
    )),

    // `< 0`, `>= 'a'` (C# 9).
    relational_pattern: $ => choice(
      seq('<', $.expression),
      seq('<=', $.expression),
      seq('>', $.expression),
      seq('>=', $.expression),
    ),

    negated_pattern: $ => seq('not', $.pattern),

    // `and` binds tighter than `or`, as `&` does `|`.
    and_pattern: $ => prec.left(PREC.AND, seq(
      field('left', $.pattern),
      field('operator', 'and'),
      field('right', $.pattern),
    )),

    or_pattern: $ => prec.left(PREC.OR, seq(
      field('left', $.pattern),
      field('operator', 'or'),
      field('right', $.pattern),
    )),

    declaration_pattern: $ => seq(
      field('type', $.type),
      $._variable_designation,
    ),

    // The name a pattern binds: `x`, `_`, `(a, b)`.
    _variable_designation: $ => prec(1, choice(
      $.discard,
      $.parenthesized_variable_designation,
      field('name', $.identifier),
    )),

    parenthesized_variable_designation: $ => seq(
      '(',
      optionalList($._variable_designation),
      ')',
    ),

    // ---- Expressions (§12) -------------------------------------------------

    // Expressions are split by whether they may be assigned to (and so
    // appear on the left of `=`): lvalue_expression and the rest.
    expression: $ => choice(
      $.non_lvalue_expression,
      $.lvalue_expression,
    ),

    non_lvalue_expression: $ => choice(
      'base',
      $.binary_expression,
      $.interpolated_string_expression,
      $.conditional_expression,
      $.conditional_access_expression,
      $.literal,
      $._expression_statement_expression,
      $.is_expression,
      $.is_pattern_expression,
      $.as_expression,
      $.cast_expression,
      $.checked_expression,
      $.collection_expression,
      $.switch_expression,
      $.throw_expression,
      $.default_expression,
      $.lambda_expression,
      $.with_expression,
      $.sizeof_expression,
      $.typeof_expression,
      $.makeref_expression,
      $.ref_expression,
      $.reftype_expression,
      $.refvalue_expression,
      $.stackalloc_expression,
      $.range_expression,
      $.array_creation_expression,
      $.anonymous_method_expression,
      $.anonymous_object_creation_expression,
      $.implicit_array_creation_expression,
      $.implicit_object_creation_expression,
      $.implicit_stackalloc_expression,
      $.initializer_expression,
      $.query_expression,
      alias($.preproc_if_in_expression, $.preproc_if),
    ),

    // What may be assigned to: variables, members, elements, `this` in a
    // struct, a tuple to deconstruct into, `*p`, and `[i]` inside an
    // object initializer (an element binding).
    lvalue_expression: $ => choice(
      'this',
      $.member_access_expression,
      $.tuple_expression,
      $._simple_name,
      $.element_access_expression,
      alias($.bracketed_argument_list, $.element_binding_expression),
      alias($._pointer_indirection_expression, $.prefix_unary_expression),
      alias($._parenthesized_lvalue_expression, $.parenthesized_expression),
    ),

    // The expressions an expression statement may be (§13.7).
    _expression_statement_expression: $ => choice(
      $.assignment_expression,
      $.invocation_expression,
      $.postfix_unary_expression,
      $.prefix_unary_expression,
      $.await_expression,
      $.object_creation_expression,
      $.parenthesized_expression,
    ),

    assignment_expression: $ => seq(
      field('left', $.lvalue_expression),
      field('operator', choice(...ASSIGNMENT_OPERATORS)),
      field('right', $.expression),
    ),

    binary_expression: $ => choice(
      ...BINARY_OPERATORS.map(([operator, level]) => prec.left(level, seq(
        field('left', $.expression),
        // @ts-ignore
        field('operator', operator),
        field('right', $.expression),
      ))),
      // `a ?? b ?? c` is `a ?? (b ?? c)`.
      prec.right(PREC.COALESCING, seq(
        field('left', $.expression),
        field('operator', '??'),
        field('right', $.expression),
      )),
    ),

    // `x++`, `x--`, and `x!` (C# 8's null-forgiving operator).
    postfix_unary_expression: $ => prec(PREC.POSTFIX, seq(
      $.expression,
      choice('++', '--', '!'),
    )),

    // `^i` is C# 8's index-from-end.
    prefix_unary_expression: $ => prec(PREC.UNARY, seq(
      choice('++', '--', '+', '-', '!', '~', '&', '^'),
      $.expression,
    )),

    // `*p` (§23.6.2), assignable, so its own rule under lvalue_expression.
    // The operand is a unary expression (§12.9): an lvalue, or `*(p + i)`,
    // `*p++`, `*f()`, `*(int*)q` (tree-sitter-c-sharp 0.23.5 allowed only an
    // lvalue, so `*(p + 1) = x` and `*(int*)q` were errors). Listed rather
    // than `expression` so that this rule's precedence decides `*p = x` as
    // `(*p) = x`; see _pointer_operand_cast for the cast. The forms beyond
    // an lvalue lose to any other reading (dynamic precedence), so that
    // `(a) * (b + 1)` stays a multiplication rather than becoming a cast
    // of `*(b + 1)`.
    _pointer_indirection_expression: $ => prec.right(PREC.UNARY, seq(
      '*',
      choice(
        $.lvalue_expression,
        prec.dynamic(-2, choice(
          $.parenthesized_expression,
          $.postfix_unary_expression,
          $.invocation_expression,
          alias($._pointer_operand_cast, $.cast_expression),
          alias($._pointer_operand_postfix, $.postfix_unary_expression),
        )),
      ),
    )),

    // -- Query expressions (§12.20) --

    query_expression: $ => seq($.from_clause, $._query_body),

    from_clause: $ => seq(
      'from',
      optional(field('type', $.type)),
      field('name', $.identifier),
      'in',
      $.expression,
    ),

    // Clauses ending in `select` or `group`, continued by `into x ...`.
    _query_body: $ => prec.right(separated1(
      seq(
        repeat($._query_clause),
        $._select_or_group_clause,
      ),
      seq('into', $.identifier),
    )),

    _query_clause: $ => choice(
      $.from_clause,
      $.join_clause,
      $.let_clause,
      $.order_by_clause,
      $.where_clause,
    ),

    join_clause: $ => seq(
      'join',
      $._join_header,
      $._join_body,
      optional($.join_into_clause),
    ),

    _join_header: $ => seq(optional(field('type', $.type)), $.identifier, 'in', $.expression),

    _join_body: $ => seq('on', $.expression, 'equals', $.expression),

    join_into_clause: $ => seq('into', $.identifier),

    let_clause: $ => seq(
      'let',
      $.identifier,
      '=',
      $.expression,
    ),

    order_by_clause: $ => seq(
      'orderby',
      commaList1($._ordering),
    ),

    _ordering: $ => seq(
      $.expression,
      optional(choice('ascending', 'descending')),
    ),

    where_clause: $ => seq('where', $.expression),

    _select_or_group_clause: $ => choice(
      $.group_clause,
      $.select_clause,
    ),

    group_clause: $ => seq('group', $.expression, 'by', $.expression),

    select_clause: $ => seq('select', $.expression),

    // -- Operators and primary expressions --

    conditional_expression: $ => prec.right(PREC.CONDITIONAL, seq(
      field('condition', $.expression),
      '?',
      field('consequence', $.expression),
      ':',
      field('alternative', $.expression),
    )),

    // `a?.b`, `a?[i]` (§12.8.8): the binding after `?` applies to `a`.
    conditional_access_expression: $ => prec.right(PREC.CONDITIONAL, seq(
      field('condition', $.expression),
      '?',
      choice(
        $.member_binding_expression,
        alias($.bracketed_argument_list, $.element_binding_expression),
      ),
    )),

    as_expression: $ => prec(PREC.REL, seq(
      field('left', $.expression),
      field('operator', 'as'),
      field('right', $.type),
    )),

    // `x is T` reads as a type test; `x is P` with any other pattern as a
    // pattern match (C# 7).
    is_expression: $ => prec(PREC.REL, seq(
      field('left', $.expression),
      field('operator', 'is'),
      field('right', $.type),
    )),

    is_pattern_expression: $ => prec(PREC.REL, seq(
      field('expression', $.expression),
      'is',
      field('pattern', $.pattern),
    )),

    // `(T)x` (§12.9.7): binds tighter than binary operators and looser
    // than a call; when `(a)-b` reads both ways, the cast is preferred
    // dynamically, as Roslyn's lookahead rules prefer it for a type name.
    cast_expression: $ => prec(PREC.CAST, prec.dynamic(1, seq(
      '(',
      field('type', $.type),
      ')',
      field('value', $.expression),
    ))),

    checked_expression: $ => seq(
      choice('checked', 'unchecked'),
      '(',
      $.expression,
      ')',
    ),

    invocation_expression: $ => prec(PREC.INVOCATION, seq(
      field('function', $.expression),
      field('arguments', $.argument_list),
    )),

    // `x switch { P when c => e, _ => f }` (C# 8).
    switch_expression: $ => prec(PREC.SWITCH, seq(
      $.expression,
      'switch',
      $._switch_expression_body,
    )),
    _switch_expression_body: $ => seq(
      '{',
      optionalList($.switch_expression_arm),
      optional(','),
      '}',
    ),

    switch_expression_arm: $ => seq(
      $.pattern,
      optional($.when_clause),
      '=>',
      $.expression,
    ),

    when_clause: $ => seq('when', $.expression),

    await_expression: $ => prec.right(PREC.UNARY, seq(
      'await',
      $.expression,
    )),

    // `x ?? throw new E()` (C# 7).
    throw_expression: $ => seq('throw', $.expression),

    element_access_expression: $ => prec(PREC.POSTFIX, seq(
      field('expression', $.expression),
      field('subscript', $.bracketed_argument_list),
    )),

    // -- Interpolated strings (§12.8.3) --
    //
    // The scanner reads the opening (`$`, `$@`/`@$`, or `$$...` before
    // three or more quotes), then the quotes, the text and the braces; an
    // interpolation is as many braces as the opening had `$`s (one for a
    // regular or verbatim string). The quotes of a raw interpolated string
    // are interpolation_quote nodes; the others are anonymous `"`.
    interpolated_string_expression: $ => choice(
      seq(
        alias($.interpolation_regular_start, $.interpolation_start),
        alias($.interpolation_start_quote, '"'),
        repeat($._interpolated_string_content),
        alias($.interpolation_end_quote, '"'),
      ),
      seq(
        alias($.interpolation_verbatim_start, $.interpolation_start),
        alias($.interpolation_start_quote, '"'),
        repeat($._interpolated_verbatim_string_content),
        alias($.interpolation_end_quote, '"'),
      ),
      seq(
        alias($.interpolation_raw_start, $.interpolation_start),
        alias($.interpolation_start_quote, $.interpolation_quote),
        repeat($._interpolated_raw_string_content),
        alias($.interpolation_end_quote, $.interpolation_quote),
      ),
    ),

    // Only a regular string has escape sequences.
    _interpolated_string_content: $ => choice(
      alias($.interpolation_string_content, $.string_content),
      $.escape_sequence,
      $.interpolation,
    ),

    _interpolated_verbatim_string_content: $ => choice(
      alias($.interpolation_string_content, $.string_content),
      $.interpolation,
    ),

    _interpolated_raw_string_content: $ => choice(
      alias($.interpolation_string_content, $.string_content),
      $.interpolation,
    ),

    // `{expr,alignment:format}`.
    interpolation: $ => seq(
      alias($.interpolation_open_brace, $.interpolation_brace),
      $.expression,
      optional($.interpolation_alignment_clause),
      optional($.interpolation_format_clause),
      alias($.interpolation_close_brace, $.interpolation_brace),
    ),

    interpolation_alignment_clause: $ => seq(',', $.expression),

    // `:HH:mm`; in a regular string the format may hold escape sequences
    // (`{d:HH \"h\"}`), which tree-sitter-c-sharp 0.23.5 did not allow.
    interpolation_format_clause: _ => seq(':', /([^}"\\]|\\.)+/),

    // `a.b`, `int.Parse`, `p->x` (§23.6.3).
    member_access_expression: $ => prec(PREC.DOT, seq(
      field('expression', choice($.expression, $.predefined_type, $._name)),
      choice('.', '->'),
      field('name', $._simple_name),
    )),

    // The `.b` of `a?.b`.
    member_binding_expression: $ => seq(
      '.',
      field('name', $._simple_name),
    ),

    // `new T(args) { init }` (§12.8.17.2).
    object_creation_expression: $ => prec.right(seq(
      'new',
      field('type', $.type),
      field('arguments', optional($.argument_list)),
      field('initializer', optional($.initializer_expression)),
    )),

    _object_creation_type: $ => choice(
      $._name,
      $.nullable_type,
      $.predefined_type,
    ),

    parenthesized_expression: $ => seq(
      '(',
      $.non_lvalue_expression,
      ')',
    ),

    _parenthesized_lvalue_expression: $ => seq('(', $.lvalue_expression, ')'),

    // `x => e`, `(int a, b) => { }`, `async (a) => ...`, C# 10's explicit
    // return type `int (x) => x` and attributes on the lambda.
    lambda_expression: $ => prec(-1, seq(
      $._lambda_expression_init,
      '=>',
      field('body', choice($.block, $.expression)),
    )),

    _lambda_expression_init: $ => prec(-1, seq(
      repeat($._attribute_list),
      repeat(prec(-1, alias(choice('static', 'async'), $.modifier))),
      optional(field('type', $.type)),
      field('parameters', $._lambda_parameters),
    ),
    ),

    _lambda_parameters: $ => prec(-1, choice(
      $.parameter_list,
      alias($.identifier, $.implicit_parameter),
    )),

    // `new int[n] { ... }` (§12.8.17.5).
    array_creation_expression: $ => prec.dynamic(PREC.UNARY, seq(
      'new',
      field('type', $.array_type),
      optional($.initializer_expression),
    )),

    // `delegate (int x) { }` (§12.19), `static`/`async` before it.
    anonymous_method_expression: $ => seq(
      repeat(prec(-1, alias(choice('static', 'async'), $.modifier))),
      'delegate',
      optional(field('parameters', $.parameter_list)),
      $.block,
    ),

    // `new { A = 1, b.C }` (§12.8.17.7).
    anonymous_object_creation_expression: $ => seq(
      'new',
      '{',
      optionalList($._anonymous_object_member_declarator),
      optional(','),
      '}',
    ),

    _anonymous_object_member_declarator: $ => choice(
      seq($.identifier, '=', $.expression),
      $.expression,
    ),

    // `new[] { 1, 2 }`, `new[,] { ... }`.
    implicit_array_creation_expression: $ => seq(
      'new',
      '[',
      repeat(','),
      ']',
      $.initializer_expression,
    ),

    // Target-typed `new(args)` (C# 9).
    implicit_object_creation_expression: $ => prec.right(seq(
      'new',
      $.argument_list,
      optional($.initializer_expression),
    )),

    implicit_stackalloc_expression: $ => seq(
      'stackalloc',
      '[',
      ']',
      $.initializer_expression,
    ),

    // `[1, 2, ..rest]` (C# 12).
    collection_expression: $ => seq(
      '[',
      optional(seq(
        commaList1($.collection_element),
        optional(','),
      )),
      ']',
    ),

    collection_element: $ => choice(
      $.expression_element,
      $.spread_element,
    ),

    expression_element: $ => prec(1, $.expression),

    spread_element: $ => prec.dynamic(1, prec(PREC.RANGE, seq('..', $.expression))),

    // `{ a, b }`, `{ X = 1, [0] = 2 }`, `{ { k, v } }`: array, object and
    // collection initializers share this shape (§12.8.17.3-4).
    initializer_expression: $ => seq(
      '{',
      optionalList($.expression),
      optional(','),
      '}',
    ),

    // `int x` where an expression is expected: an out variable or a
    // deconstruction target (C# 7).
    declaration_expression: $ => prec.dynamic(1, seq(
      field('type', $.type),
      field('name', $.identifier),
    )),

    // `default(T)`, or the target-typed `default` (C# 7.1).
    default_expression: $ => prec.right(seq(
      'default',
      optional(seq(
        '(',
        field('type', $.type),
        ')',
      )),
    )),

    // `r with { X = 1 }` (C# 9).
    with_expression: $ => prec.left(PREC.WITH, seq(
      $.expression,
      'with',
      $._with_body,
    )),
    _with_body: $ => seq(
      '{',
      optionalList($.with_initializer),
      optional(','),
      '}',
    ),

    with_initializer: $ => seq($.identifier, '=', $.expression),

    sizeof_expression: $ => typeOperand('sizeof', $),

    typeof_expression: $ => typeOperand('typeof', $),

    // The undocumented TypedReference keywords Roslyn still parses.
    makeref_expression: $ => seq(
      '__makeref',
      '(',
      $.expression,
      ')',
    ),

    // `ref x` as a value (a ref return, a ref local's initializer).
    ref_expression: $ => seq('ref', $.expression),

    reftype_expression: $ => seq(
      '__reftype',
      '(',
      $.expression,
      ')',
    ),

    refvalue_expression: $ => seq(
      '__refvalue',
      '(',
      field('value', $.expression),
      ',',
      field('type', $.type),
      ')',
    ),

    // `stackalloc int[n]` (§12.8.22).
    stackalloc_expression: $ => prec.left(seq(
      'stackalloc',
      field('type', $.array_type),
      optional($.initializer_expression),
    )),

    // `a..b`, `..b`, `a..`, `..` (C# 8).
    range_expression: $ => prec.right(PREC.RANGE, seq(
      optional($.expression),
      '..',
      optional($.expression),
    )),

    // `(a, b: 2)`: at least two elements (one is a parenthesized
    // expression).
    tuple_expression: $ => seq(
      '(',
      commaList2($.argument),
      ')',
    ),

    // ---- Literals (§6.4.5) -------------------------------------------------

    literal: $ => choice(
      $.null_literal,
      $.character_literal,
      $.integer_literal,
      $.real_literal,
      $.boolean_literal,
      $.string_literal,
      $.verbatim_string_literal,
      $.raw_string_literal,
    ),

    null_literal: _ => 'null',

    character_literal: $ => seq(
      '\'',
      choice($.character_literal_content, $.escape_sequence),
      '\'',
    ),

    character_literal_content: $ => token.immediate(/[^'\\]/),

    // Decimal, `0x` hex and `0b` binary (C# 7) with `_` separators, and a
    // `u`/`l` suffix in either order or case (§6.4.5.3).
    integer_literal: _ => token(seq(
      choice(
        DECIMAL_DIGITS,
        (/0[xX][0-9a-fA-F_]*[0-9a-fA-F]+/),
        (/0[bB][01_]*[01]+/),
      ),
      optional(/([uU][lL]?|[lL][uU]?)/),
    )),

    // `1.5`, `.5`, `1e10`, and an integer with a real suffix (`1f`, `2m`).
    real_literal: _ => {
      const suffix = /[fFdDmM]/;
      const exponent = /[eE][+-]?[0-9][0-9_]*/;
      return token(choice(
        seq(DECIMAL_DIGITS, '.', DECIMAL_DIGITS, optional(exponent), optional(suffix)),
        seq('.', DECIMAL_DIGITS, optional(exponent), optional(suffix)),
        seq(DECIMAL_DIGITS, exponent, optional(suffix)),
        seq(DECIMAL_DIGITS, suffix),
      ));
    },

    // `"text\n"`, `"bytes"u8`; a regular string ends at the line's end.
    string_literal: $ => seq(
      '"',
      repeat(choice(
        $.string_literal_content,
        $.escape_sequence,
      )),
      '"',
      optional($.string_literal_encoding),
    ),

    string_literal_content: _ => token.immediate(prec(1, /[^"\\\n]+/)),

    // `\x` takes one to four hex digits; `\e` is C# 13.
    escape_sequence: _ => token(choice(
      /\\x[0-9a-fA-F]{1,4}/,
      /\\u[0-9a-fA-F]{4}/,
      /\\U[0-9a-fA-F]{8}/,
      /\\[abefnrtv'\"\\\?0]/,
    )),

    string_literal_encoding: _ => token.immediate(UTF8_SUFFIX),

    // `@"C:\dir"`: no escapes, `""` is a quote, newlines allowed.
    verbatim_string_literal: _ => token(seq(
      '@"',
      repeat(choice(
        /[^"]/,
        '""',
      )),
      '"',
      optional(UTF8_SUFFIX),
    )),

    // `"""..."""` (C# 11): the scanner matches the closing run's length.
    raw_string_literal: $ => seq(
      $.raw_string_start,
      $.raw_string_content,
      $.raw_string_end,
      optional(UTF8_SUFFIX),
    ),

    boolean_literal: _ => choice('true', 'false'),

    // ---- Identifiers (§6.4.3) ---------------------------------------------

    // Letters and connectors per Unicode's XID properties, `\u` escapes
    // allowed, `@` to use a keyword as a name.
    _identifier_token: _ => token(seq(
      optional('@'),
      /(\p{XID_Start}|_|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8})(\p{XID_Continue}|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8})*/,
    )),
    identifier: $ => choice(
      $._identifier_token,
      $._reserved_identifier,
    ),

    _reserved_identifier: _ => choice(...CONTEXTUAL_IDENTIFIERS, ...LATER_CONTEXTUAL_IDENTIFIERS),

    // ---- Preprocessing directives (§6.5) ------------------------------------

    // `#if` blocks, once per place their contents may be: between
    // declarations, at the top level or among statements, inside an
    // expression, among enum members, among attribute lists.
    ...conditionalDirective('', $ => $.declaration),
    ...conditionalDirective('_in_top_level', $ => choice($._top_level_item_no_statement, $.statement)),
    ...conditionalDirective('_in_expression', $ => $.expression, -2, false),
    ...conditionalDirective('_in_enum_member_declaration', $ => $.enum_member_declaration, 0, false),
    ...conditionalDirective('_in_attribute_list', $ => $.attribute_list, -1, false),

    // The rest of a directive's line, up to a comment.
    preproc_arg: _ => token(prec(-1, /\S([^/\n]|\/[^*]|\\\r?\n)*/)),
    preproc_directive: _ => /#[ \t]*[a-zA-Z0-9]\w*/,

    // `#if` conditions (§6.5.5): symbols, `true`/`false`, `!`, `==`, `!=`,
    // `&&`, `||` and parentheses.
    _preproc_expression: $ => choice(
      $.identifier,
      $.boolean_literal,
      $.integer_literal,
      $.character_literal,
      alias($.preproc_unary_expression, $.unary_expression),
      alias($.preproc_binary_expression, $.binary_expression),
      alias($.preproc_parenthesized_expression, $.parenthesized_expression),
    ),

    preproc_parenthesized_expression: $ => seq(
      '(',
      $._preproc_expression,
      ')',
    ),

    preproc_unary_expression: $ => prec.left(PREC.UNARY, seq(
      field('operator', '!'),
      field('argument', $._preproc_expression),
    )),

    preproc_binary_expression: $ => choice(...[
      ['||', PREC.LOGICAL_OR],
      ['&&', PREC.LOGICAL_AND],
      ['==', PREC.EQUAL],
      ['!=', PREC.EQUAL],
    ].map(([operator, level]) => prec.left(level, seq(
      field('left', $._preproc_expression),
      // @ts-ignore
      field('operator', operator),
      field('right', $._preproc_expression),
    )))),

    // `#region Name` / `#endregion` (§6.5.7).
    preproc_region: $ => seq(
      directive('region'),
      optional(field('content', $.preproc_arg)),
      /\n/,
    ),

    preproc_endregion: $ => seq(
      directive('endregion'),
      optional(field('content', $.preproc_arg)),
      /\n/,
    ),

    // `#line 200 "file"`, `#line default`, `#line hidden`, and C# 10's
    // span form `#line (1, 1) - (5, 60) 10 "file"` (§6.5.8).
    preproc_line: $ => seq(
      directive('line'),
      choice(
        'default',
        'hidden',
        seq($.integer_literal, optional($.string_literal)),
        seq(
          '(', $.integer_literal, ',', $.integer_literal, ')',
          '-',
          '(', $.integer_literal, ',', $.integer_literal, ')',
          optional($.integer_literal),
          $.string_literal,
        ),
      ),
      /\n/,
    ),

    // `#pragma warning disable CS0168, IDE0051`, `#pragma checksum` (§6.5.10).
    preproc_pragma: $ => seq(
      directive('pragma'),
      choice(
        seq('warning',
          choice('disable', 'restore'),
          optionalList(
            choice(
              $.identifier,
              $.integer_literal,
            ))),
        seq('checksum', $.string_literal, $.string_literal, $.string_literal),
      ),
      /\n/,
    ),

    // `#nullable enable warnings` (C# 8, §6.5.9).
    preproc_nullable: _ => seq(
      directive('nullable'),
      choice('enable', 'disable', 'restore'),
      optional(choice('annotations', 'warnings')),
      /\n/,
    ),

    preproc_error: $ => seq(
      directive('error'),
      $.preproc_arg,
      /\n/,
    ),

    preproc_warning: $ => seq(
      directive('warning'),
      $.preproc_arg,
      /\n/,
    ),

    preproc_define: $ => seq(
      directive('define'),
      $.preproc_arg,
      /\n/,
    ),

    preproc_undef: $ => seq(
      directive('undef'),
      $.preproc_arg,
      /\n/,
    ),

    // `#!/usr/bin/env dotnet` on a script's first line.
    shebang_directive: _ => token(seq('#!', /.*/)),

    // `// ...` and `/* ... */` (§6.3.3); doc comments are `///` comments.
    comment: _ => token(choice(
      seq('//', /[^\n\r]*/),
      seq(
        '/*',
        /[^*]*\*+([^/*][^*]*\*+)*/,
        '/',
      ),
    )),

    // ---- Corrections to tree-sitter-c-sharp 0.23.5 --------------------------
    //
    // Rules this grammar has and the one it replaces did not. They come last
    // so that every earlier rule keeps its number (and so its place in
    // tree-sitter's tie-breaks); see README.md, "Where it differs".

    // A list pattern's slice (C# 11): `..`, or `..` and a pattern for the
    // slice (`.. var rest`, `.. { Length: > 2 }`). Hidden, so a list
    // pattern's children are its patterns and `..` tokens as before. It
    // outranks a range expression: in a pattern `[..` starts a slice,
    // never a collection expression of a range (which is not a constant).
    _slice_pattern: $ => prec(PREC.RANGE + 1, seq('..', optional($.pattern))),

    // The cast in `*(int*)p`: a cast_expression whose operand is one of the
    // forms a pointer indirection takes, so that in `*(int*)p = x` the
    // cast ends before the `=`.
    _pointer_operand_cast: $ => prec(PREC.CAST, prec.dynamic(1, seq(
      '(',
      field('type', $.type),
      ')',
      field('value', choice(
        $.lvalue_expression,
        $.parenthesized_expression,
        $.postfix_unary_expression,
        $.invocation_expression,
      )),
    ))),

    // `p++` in `*p++`: postfix operators bind tighter than `*`, so this is
    // `*(p++)` (tree-sitter-c-sharp 0.23.5 read `(*p)++`, and then `*p++ =
    // x` was an error).
    _pointer_operand_postfix: $ => prec(PREC.POSTFIX, seq(
      $.lvalue_expression,
      choice('++', '--', '!'),
    )),
  },
});

// ---- Helpers ----------------------------------------------------------------

/**
 * What a member or type declaration starts with: attribute sections, then
 * modifiers. Spread into a seq().
 *
 * @param {GrammarSymbols<string>} $
 * @returns {Rule[]}
 */
function declarationPrefix($) {
  return [repeat($._attribute_list), repeat($.modifier)];
}

/**
 * `keyword(Type)`: sizeof and typeof.
 *
 * @param {string} keyword
 * @param {GrammarSymbols<string>} $
 * @returns {SeqRule}
 */
function typeOperand(keyword, $) {
  return seq(keyword, '(', field('type', $.type), ')');
}

/**
 * A directive keyword: `#`, optional blanks, the name -- `# if` is `#if`.
 * The node is the anonymous `#name`.
 *
 * @param {string} name
 * @returns {AliasRule}
 */
function directive(name) {
  return alias(new RegExp('#[ \t]*' + name), '#' + name);
}

/**
 * `#if cond ... #elif cond ... #else ... #endif` holding `content`: the rules
 * preproc_if<suffix>, preproc_else<suffix> and preproc_elif<suffix> (the
 * latter two aliased to the unsuffixed names). A branch holds any number of
 * items, or with `many` false at most one (where the directive sits inside
 * a list whose separators the branches cannot carry).
 *
 * @param {string} suffix
 * @param {RuleBuilder<string>} content
 * @param {number} precedence
 * @param {boolean} many
 * @returns {RuleBuilders<string, string>}
 */
function conditionalDirective(suffix, content, precedence = 0, many = true) {
  const branchBody = $ => (many ? repeat(content($)) : optional(content($)));
  const nextBranch = $ => choice(
    suffix ? alias($['preproc_else' + suffix], $.preproc_else) : $.preproc_else,
    suffix ? alias($['preproc_elif' + suffix], $.preproc_elif) : $.preproc_elif,
  );

  return {
    ['preproc_if' + suffix]: $ => prec(precedence, seq(
      directive('if'),
      field('condition', $._preproc_expression),
      /\n/,
      branchBody($),
      field('alternative', optional(nextBranch($))),
      directive('endif'),
    )),

    ['preproc_else' + suffix]: $ => prec(precedence, seq(
      directive('else'),
      branchBody($),
    )),

    ['preproc_elif' + suffix]: $ => prec(precedence, seq(
      directive('elif'),
      field('condition', $._preproc_expression),
      /\n/,
      branchBody($),
      field('alternative', optional(nextBranch($))),
    )),
  };
}

/**
 * One or more of `rule`, comma-separated.
 *
 * @param {Rule} rule
 * @returns {SeqRule}
 */
function commaList1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}

/**
 * Two or more of `rule`, comma-separated (tuples).
 *
 * @param {Rule} rule
 * @returns {SeqRule}
 */
function commaList2(rule) {
  return seq(rule, repeat1(seq(',', rule)));
}

/**
 * Zero or more of `rule`, comma-separated.
 *
 * @param {Rule} rule
 * @returns {ChoiceRule}
 */
function optionalList(rule) {
  return optional(commaList1(rule));
}

/**
 * One or more of `rule` separated by `separator`.
 *
 * @param {RuleOrLiteral} rule
 * @param {RuleOrLiteral} separator
 * @returns {SeqRule}
 */
function separated1(rule, separator) {
  return seq(rule, repeat(seq(separator, rule)));
}

/**
 * Zero or more of `rule` separated by `separator`.
 *
 * @param {RuleOrLiteral} rule
 * @param {RuleOrLiteral} separator
 * @returns {ChoiceRule}
 */
function optionalSeparated(rule, separator) {
  return optional(separated1(rule, separator));
}
