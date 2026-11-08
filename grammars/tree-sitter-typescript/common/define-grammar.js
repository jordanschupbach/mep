/**
 * @file Tree-sitter grammars for TypeScript and TSX, the two dialects.
 *
 * mep's own grammars, built on mep's JavaScript grammar
 * (grammars/tree-sitter-javascript), which they extend the way TypeScript
 * extends JavaScript: types, type annotations, generics, enums,
 * interfaces, namespaces, declarations, accessibility modifiers and the
 * expressions TypeScript adds (`x as T`, `x satisfies T`, `x!`, `f<T>`,
 * and in .ts `<T>x`). The language reference is TypeScript's own parser
 * (src/compiler/parser.ts, 5.9); the old grammar is the reference for the
 * trees.
 *
 * Its node names, fields and tree shapes are the ones
 * tree-sitter/tree-sitter-typescript 0.23 established (MIT; Max Brunsfeld,
 * Amaan Qureshi and contributors), the grammar mep loaded before, so
 * queries written for it keep working. The rules follow that grammar's
 * common/define-grammar.js, adapted to mep's JavaScript grammar; see
 * README.md for where they differ and how the two were compared.
 *
 * The dialects differ in one place: `<T>x` is a type assertion in .ts and
 * a JSX element in .tsx (TypeScript decides the same way, by file
 * extension). Everything else is shared.
 *
 * New rules are declared in the old grammar's order, after the
 * JavaScript ones, and that is load-bearing: where two parses of an
 * ambiguous construct survive to the same point, tree-sitter keeps the one
 * whose node types are numbered lower.
 *
 * The external scanner (common/scanner.h, included by each dialect's
 * src/scanner.c) adds to JavaScript's what TypeScript leaves to context: a
 * signature with no body ends at a line break unless a `{` follows, a `?`
 * followed by `:`, `)` or `,` is an optional marker and not a conditional,
 * and a line break before `(` or `[` ends a type.
 */

const JavaScript = require('../../tree-sitter-javascript/grammar.js');

const sepBy1 = (separator, rule) => seq(rule, repeat(seq(separator, rule)));
const sepBy = (separator, rule) => optional(sepBy1(separator, rule));
const commaSep1 = rule => sepBy1(',', rule);
const commaSep = rule => sepBy(',', rule);

module.exports = function defineGrammar(dialect) {
  if (dialect !== 'typescript' && dialect !== 'tsx') throw new Error(`Unknown dialect ${dialect}`);

  return grammar(JavaScript, {
    name: dialect,

    externals: ($, previous) => previous.concat([
      // A body-less signature's end: like _automatic_semicolon, but not
      // before a `{` on the next line (that `{` opens the body).
      $._function_signature_automatic_semicolon,
      // Never produced: valid only while the parser recovers from an error.
      $.__error_recovery,
    ]),

    supertypes: ($, previous) => previous.concat([
      $.type,
      $.primary_type,
    ]),

    precedences: ($, previous) => previous.concat([
      [
        'call',
        'instantiation',
        'unary',
        'binary',
        $.await_expression,
        $.arrow_function,
      ],
      [
        'extends',
        'instantiation',
      ],
      [
        $.intersection_type,
        $.union_type,
        $.conditional_type,
        $.function_type,
        'binary',
        $.type_predicate,
        $.readonly_type,
      ],
      [$.mapped_type_clause, $.primary_expression],
      [$.accessibility_modifier, $.primary_expression],
      ['unary_void', $.expression],
      [$.extends_clause, $.primary_expression],
      ['unary', 'assign'],
      ['declaration', $.expression],
      [$.predefined_type, $.unary_expression],
      [$.type, $.flow_maybe_type],
      [$.tuple_type, $.array_type, $.pattern, $.type],
      [$.readonly_type, $.pattern],
      [$.readonly_type, $.primary_expression],
      [$.type_query, $.subscript_expression, $.expression],
      [$.type_query, $._type_query_subscript_expression],
      [$.nested_type_identifier, $.generic_type, $.primary_type, $.lookup_type, $.index_type_query, $.type],
      [$.as_expression, $.satisfies_expression, $.primary_type],
      [$._type_query_member_expression, $.member_expression],
      [$.member_expression, $._type_query_member_expression_in_type_annotation],
      [$._type_query_member_expression, $.primary_expression],
      [$._type_query_subscript_expression, $.subscript_expression],
      [$._type_query_subscript_expression, $.primary_expression],
      [$._type_query_call_expression, $.primary_expression],
      [$._type_query_instantiation_expression, $.primary_expression],
      [$.type_query, $.primary_expression],
      [$.override_modifier, $.primary_expression],
      [$.decorator_call_expression, $.decorator],
      [$.literal_type, $.pattern],
      [$.predefined_type, $.pattern],
      [$.call_expression, $._type_query_call_expression],
      [$.call_expression, $._type_query_call_expression_in_type_annotation],
      [$.new_expression, $.primary_expression],
      [$.meta_property, $.primary_expression],
      [$.construct_signature, $._property_name],
    ]),

    // Generic calls (`f<T>(x)` against `f < T > (x)`), arrow functions'
    // typed parameters against parenthesized expressions, object and array
    // literals against object and tuple types: each is known only from
    // what follows, as in TypeScript's parser, which looks ahead
    // (parser.ts's tryParse / isStartOfType).
    conflicts: ($, previous) => previous.concat([
      [$.call_expression, $.instantiation_expression, $.binary_expression],
      [$.call_expression, $.instantiation_expression, $.binary_expression, $.unary_expression],
      [$.call_expression, $.instantiation_expression, $.binary_expression, $.update_expression],
      [$.call_expression, $.instantiation_expression, $.binary_expression, $.await_expression],

      // A parenthesized class expression.
      [$.class],

      [$.nested_identifier, $.nested_type_identifier, $.primary_expression],
      [$.nested_identifier, $.nested_type_identifier],

      [$._call_signature, $.function_type],
      [$._call_signature, $.constructor_type],

      [$.primary_expression, $._parameter_name],
      [$.primary_expression, $._parameter_name, $.primary_type],
      [$.primary_expression, $.literal_type],
      [$.primary_expression, $.literal_type, $.rest_pattern],
      [$.primary_expression, $.predefined_type, $.rest_pattern],
      [$.primary_expression, $.primary_type],
      [$.primary_expression, $.generic_type],
      [$.primary_expression, $.predefined_type],
      [$.primary_expression, $.pattern, $.primary_type],
      [$._parameter_name, $.primary_type],
      [$.pattern, $.primary_type],

      [$.optional_tuple_parameter, $.primary_type],
      [$.optional_tuple_parameter, $.predefined_type],
      [$.rest_pattern, $.primary_type, $.primary_expression],

      [$.object, $.object_type],
      [$.object, $.object_pattern, $.object_type],
      [$.object, $.object_pattern, $._property_name],
      [$.object_pattern, $.object_type],

      [$.array, $.tuple_type],
      [$.array, $.array_pattern, $.tuple_type],
      [$.array_pattern, $.tuple_type],

      [$.template_literal_type, $.template_string],
    ]).concat(dialect === 'typescript' ? [
      [$.primary_type, $.type_parameter],
    ] : [
      [$.jsx_opening_element, $.type_parameter],
      [$.jsx_namespace_name, $.primary_type],
    ]),

    inline: ($, previous) => previous
      .filter(rule => !['_formal_parameter', '_call_signature'].includes(rule.name))
      .concat([
        $._type_identifier,
        $._jsx_start_opening_element,
      ]),

    rules: {
      // Class fields, with TypeScript's modifiers, `?`/`!` and a type.
      public_field_definition: $ => seq(
        repeat(field('decorator', $.decorator)),
        optional(choice(
          seq('declare', optional($.accessibility_modifier)),
          seq($.accessibility_modifier, optional('declare')),
        )),
        choice(
          seq(optional('static'), optional($.override_modifier), optional('readonly')),
          seq(optional('abstract'), optional('readonly')),
          seq(optional('readonly'), optional('abstract')),
          // Auto-accessors (4.9): `static accessor x = 1`.
          seq(optional('static'), optional($.override_modifier), 'accessor'),
        ),
        field('name', $._property_name),
        optional(choice('?', '!')),
        field('type', optional($.type_annotation)),
        optional($._initializer),
      ),

      // `catch (e: unknown)`: TypeScript allows only `any` and `unknown`
      // there, a checker error rather than a syntax one, so any type parses.
      catch_clause: $ => seq(
        'catch',
        optional(seq(
          '(',
          field('parameter', choice($.identifier, $._destructuring_pattern)),
          optional(field('type', $.type_annotation)),
          ')',
        )),
        field('body', $.statement_block),
      ),

      // `for (using x = ...;;)`, `for (await using x = ...;;)` (5.2).
      for_statement: $ => seq(
        'for',
        '(',
        choice(
          field('initializer', choice($.lexical_declaration, $.variable_declaration, $.using_declaration)),
          seq(field('initializer', $._expressions), ';'),
          field('initializer', $.empty_statement),
        ),
        field('condition', choice(seq($._expressions, ';'), $.empty_statement)),
        field('increment', optional($._expressions)),
        ')',
        field('body', $.statement),
      ),

      // Calls take type arguments: `f<T>(x)`, `a?.<T>(x)`.
      //
      // `?.` is spelled as optional_chain (aliased to the bare `?.` the old
      // grammar had) wherever it is not that node: as long as no rule uses
      // the string itself, optional_chain stays a token, and the state
      // after it lexes the property name with the property-name reserved
      // words (none), so `a?.delete()` reads `delete` as a name.
      call_expression: $ => choice(
        prec('call', seq(
          field('function', choice($.expression, $.import)),
          field('type_arguments', optional($.type_arguments)),
          field('arguments', $.arguments),
        )),
        // A tagged template with type arguments: styled.div<P>`...`. Its
        // tag is read the way a generic call's is, so the two part only at
        // the `(` or the backtick.
        prec('call', seq(
          field('function', $.expression),
          field('type_arguments', $.type_arguments),
          field('arguments', $.template_string),
        )),
        prec('template_call', seq(
          field('function', choice($.primary_expression, $.new_expression)),
          field('arguments', $.template_string),
        )),
        prec('member', seq(
          field('function', $.primary_expression),
          alias($.optional_chain, '?.'),
          field('type_arguments', optional($.type_arguments)),
          field('arguments', $.arguments),
        )),
      ),

      new_expression: $ => prec.right('new', seq(
        'new',
        field('constructor', $.primary_expression),
        field('type_arguments', optional($.type_arguments)),
        field('arguments', optional($.arguments)),
      )),

      _augmented_assignment_lhs: ($, previous) => choice(previous, $.non_null_expression),

      _lhs_expression: ($, previous) => choice(previous, $.non_null_expression),

      primary_expression: ($, previous) => choice(previous, $.non_null_expression),

      // .ts has type assertions (`<T>x`) and no JSX; .tsx the reverse.
      expression: ($, previous) => {
        const choices = [
          $.as_expression,
          $.satisfies_expression,
          $.instantiation_expression,
          $.internal_module,
        ];
        if (dialect === 'typescript') {
          choices.push($.type_assertion);
          choices.push(...previous.members.filter(member => member.name !== '_jsx_element'));
        } else {
          choices.push(...previous.members);
        }
        return choice(...choices);
      },

      // A JSX tag may take type arguments: `<Select<Option> ...>`.
      _jsx_start_opening_element: $ => seq(
        '<',
        optional(seq(
          choice(
            field('name', choice($._jsx_identifier, alias($.this, $.identifier), $.jsx_namespace_name)),
            seq(
              field('name', choice($.identifier, alias($._jsx_member_expression, $.member_expression))),
              field('type_arguments', optional($.type_arguments)),
            ),
          ),
          repeat(field('attribute', $._jsx_attribute)),
        )),
      ),

      // `a.b.c`: a namespace's or an import alias's path, or a qualified
      // type name's qualifier.
      nested_identifier: $ => prec('member', seq(
        field('object', choice($.identifier, alias($.nested_identifier, $.member_expression))),
        '.',
        field('property', reserved('properties', alias($.identifier, $.property_identifier))),
      )),

      // A dotted tag name, which may start with `this` (`<this.Component />`,
      // the `this` an identifier node as in the old grammar).
      _jsx_member_expression: $ => prec('member', seq(
        field('object', choice(
          $.identifier,
          alias($.this, $.identifier),
          alias($._jsx_member_expression, $.member_expression),
        )),
        '.',
        field('property', alias($.identifier, $.property_identifier)),
      )),

      _jsx_element_name: $ => choice(
        $._jsx_identifier,
        alias($.this, $.identifier),
        alias($._jsx_member_expression, $.member_expression),
        $.jsx_namespace_name,
      ),

      // Attribute names are IdentifierNames too: `<label class="a" for="b">`.
      _jsx_attribute_name: $ => choice(
        alias($.jsx_identifier, $.property_identifier),
        reserved('properties', alias($.identifier, $.property_identifier)),
        $.jsx_namespace_name,
      ),

      jsx_opening_element: $ => prec.dynamic(-1, seq(
        $._jsx_start_opening_element,
        '>',
      )),

      jsx_self_closing_element: $ => prec.dynamic(-1, seq(
        $._jsx_start_opening_element,
        '/>',
      )),

      // An exported name is any IdentifierName: `export { default } from
      // "x"`, `export { x as while }` (an identifier node, as in the old
      // grammar and TypeScript's own tree).
      _module_export_name: $ => choice(reserved('properties', $.identifier), $.string),

      // `export { type A }`.
      export_specifier: (_, previous) => seq(
        optional(choice('type', 'typeof')),
        previous,
      ),

      _import_identifier: $ => choice($.identifier, alias('type', $.identifier)),

      // `import { type A, type as B }`.
      import_specifier: $ => seq(
        optional(choice('type', 'typeof')),
        choice(
          field('name', $._import_identifier),
          seq(
            field('name', choice($._module_export_name, alias('type', $.identifier))),
            'as',
            field('alias', $._import_identifier),
          ),
        ),
      ),

      // `assert` is the import attributes' older spelling.
      import_attribute: $ => seq(choice('with', 'assert'), $.object),

      import_clause: $ => choice(
        $.namespace_import,
        $.named_imports,
        seq(
          $._import_identifier,
          optional(seq(',', choice($.namespace_import, $.named_imports))),
        ),
      ),

      // `import type ...`, and the CommonJS import `import x = require(...)`.
      import_statement: $ => seq(
        'import',
        optional(choice('type', 'typeof')),
        choice(
          seq($.import_clause, $._from_clause),
          $.import_require_clause,
          field('source', $.string),
        ),
        optional($.import_attribute),
        $._semicolon,
      ),

      // `export type {...}`, `export = x`, `export as namespace N`.
      export_statement: ($, previous) => choice(
        previous,
        seq('export', 'type', $.export_clause, optional($._from_clause), $._semicolon),
        seq('export', '=', $.expression, $._semicolon),
        seq('export', 'as', 'namespace', $.identifier, $._semicolon),
      ),

      non_null_expression: $ => prec.left('unary', seq($.expression, '!')),

      // `let x: T = ...`, and the definite assignment `let x!: T`.
      variable_declarator: $ => choice(
        seq(
          field('name', choice($.identifier, $._destructuring_pattern)),
          field('type', optional($.type_annotation)),
          optional($._initializer),
        ),
        prec('declaration', seq(
          field('name', $.identifier),
          '!',
          field('type', $.type_annotation),
        )),
      ),

      method_signature: $ => seq(
        optional($.accessibility_modifier),
        optional('static'),
        optional($.override_modifier),
        optional('readonly'),
        optional('async'),
        optional(choice('get', 'set', '*')),
        field('name', $._property_name),
        optional('?'),
        $._call_signature,
      ),

      abstract_method_signature: $ => seq(
        optional($.accessibility_modifier),
        'abstract',
        optional($.override_modifier),
        optional(choice('get', 'set', '*')),
        field('name', $._property_name),
        optional('?'),
        $._call_signature,
      ),

      // `(x: T)` is a parenthesized expression until `=>` makes it an arrow
      // function's parameters.
      parenthesized_expression: $ => seq(
        '(',
        choice(
          seq($.expression, field('type', optional($.type_annotation))),
          $.sequence_expression,
        ),
        ')',
      ),

      _formal_parameter: $ => choice($.required_parameter, $.optional_parameter),

      // An overload or ambient declaration: a function with no body.
      function_signature: $ => seq(
        optional('async'),
        'function',
        field('name', $.identifier),
        $._call_signature,
        choice($._semicolon, $._function_signature_automatic_semicolon),
      ),

      decorator: $ => seq(
        '@',
        choice(
          $.identifier,
          alias($.decorator_member_expression, $.member_expression),
          alias($.decorator_call_expression, $.call_expression),
          alias($.decorator_parenthesized_expression, $.parenthesized_expression),
        ),
      ),

      decorator_call_expression: $ => prec('call', seq(
        field('function', choice(
          $.identifier,
          alias($.decorator_member_expression, $.member_expression),
        )),
        optional(field('type_arguments', $.type_arguments)),
        field('arguments', $.arguments),
      )),

      // `@(expr)`: any expression (`@(this.decorator)`).
      decorator_parenthesized_expression: $ => seq('(', $.expression, ')'),

      // A method signature (an overload, or an abstract or ambient method)
      // ends where a function signature does: a line break ends it unless a
      // `{` follows, which makes it a definition.
      class_body: $ => seq(
        '{',
        repeat(choice(
          seq(
            repeat(field('decorator', $.decorator)),
            $.method_definition,
            optional($._semicolon),
          ),
          seq($.method_signature, choice($._function_signature_automatic_semicolon, ',')),
          $.class_static_block,
          seq(
            choice(
              $.abstract_method_signature,
              $.index_signature,
              $.method_signature,
              $.public_field_definition,
            ),
            choice($._semicolon, ','),
          ),
          ';',
        )),
        '}',
      ),

      method_definition: $ => prec.left(seq(
        optional($.accessibility_modifier),
        optional('static'),
        optional($.override_modifier),
        optional('readonly'),
        optional('async'),
        optional(choice('get', 'set', '*')),
        field('name', $._property_name),
        optional('?'),
        $._call_signature,
        field('body', $.statement_block),
      )),

      declaration: ($, previous) => choice(
        previous,
        $.function_signature,
        $.abstract_class_declaration,
        $.module,
        prec('declaration', $.internal_module),
        $.type_alias_declaration,
        $.enum_declaration,
        $.interface_declaration,
        $.import_alias,
        $.ambient_declaration,
      ),

      type_assertion: $ => prec.left('unary', seq($.type_arguments, $.expression)),

      as_expression: $ => prec.left('binary', seq($.expression, 'as', choice('const', $.type))),

      satisfies_expression: $ => prec.left('binary', seq($.expression, 'satisfies', $.type)),

      instantiation_expression: $ => prec('instantiation', seq(
        $.expression,
        field('type_arguments', $.type_arguments),
      )),

      class_heritage: $ => choice(
        seq($.extends_clause, optional($.implements_clause)),
        $.implements_clause,
      ),

      import_require_clause: $ => seq(
        $.identifier,
        '=',
        'require',
        '(',
        field('source', $.string),
        ')',
      ),

      extends_clause: $ => seq('extends', commaSep1($._extends_clause_single)),

      _extends_clause_single: $ => prec('extends', seq(
        field('value', $.expression),
        field('type_arguments', optional($.type_arguments)),
      )),

      implements_clause: $ => seq('implements', commaSep1($.type)),

      ambient_declaration: $ => seq(
        'declare',
        choice(
          $.declaration,
          seq('global', $.statement_block),
          seq('module', '.', alias($.identifier, $.property_identifier), ':', $.type, $._semicolon),
        ),
      ),

      class: $ => prec('literal', seq(
        repeat(field('decorator', $.decorator)),
        'class',
        field('name', optional($._type_identifier)),
        field('type_parameters', optional($.type_parameters)),
        optional($.class_heritage),
        field('body', $.class_body),
      )),

      abstract_class_declaration: $ => prec('declaration', seq(
        repeat(field('decorator', $.decorator)),
        'abstract',
        'class',
        field('name', $._type_identifier),
        field('type_parameters', optional($.type_parameters)),
        optional($.class_heritage),
        field('body', $.class_body),
      )),

      class_declaration: $ => prec.left('declaration', seq(
        repeat(field('decorator', $.decorator)),
        'class',
        field('name', $._type_identifier),
        field('type_parameters', optional($.type_parameters)),
        optional($.class_heritage),
        field('body', $.class_body),
        optional($._automatic_semicolon),
      )),

      module: $ => seq('module', $._module),

      internal_module: $ => seq('namespace', $._module),

      // `declare module "x";` has no body.
      _module: $ => prec.right(seq(
        field('name', choice($.string, $.identifier, $.nested_identifier)),
        field('body', optional($.statement_block)),
      )),

      import_alias: $ => seq(
        'import',
        $.identifier,
        '=',
        choice($.identifier, $.nested_identifier),
        $._semicolon,
      ),

      // `A.B.C` in a type. The names after a dot are IdentifierNames
      // (`Bool.false`, `x.default.T`), and so below in `typeof a.delete` and
      // import types.
      nested_type_identifier: $ => prec('member', seq(
        field('module', choice($.identifier, $.nested_identifier)),
        '.',
        field('name', reserved('properties', alias($.identifier, $.type_identifier))),
      )),

      interface_declaration: $ => seq(
        'interface',
        field('name', $._type_identifier),
        field('type_parameters', optional($.type_parameters)),
        optional($.extends_type_clause),
        field('body', alias($.object_type, $.interface_body)),
      ),

      extends_type_clause: $ => seq(
        'extends',
        commaSep1(field('type', choice(
          $._type_identifier,
          $.nested_type_identifier,
          $.generic_type,
        ))),
      ),

      enum_declaration: $ => seq(
        optional('const'),
        'enum',
        field('name', $.identifier),
        field('body', $.enum_body),
      ),

      enum_body: $ => seq(
        '{',
        optional(seq(
          sepBy1(',', choice(field('name', $._property_name), $.enum_assignment)),
          optional(','),
        )),
        '}',
      ),

      enum_assignment: $ => seq(field('name', $._property_name), $._initializer),

      type_alias_declaration: $ => seq(
        'type',
        field('name', $._type_identifier),
        field('type_parameters', optional($.type_parameters)),
        '=',
        field('value', $.type),
        $._semicolon,
      ),

      accessibility_modifier: _ => choice('public', 'private', 'protected'),

      override_modifier: _ => 'override',

      required_parameter: $ => seq(
        $._parameter_name,
        field('type', optional($.type_annotation)),
        optional($._initializer),
      ),

      optional_parameter: $ => seq(
        $._parameter_name,
        '?',
        field('type', optional($.type_annotation)),
        optional($._initializer),
      ),

      _parameter_name: $ => seq(
        repeat(field('decorator', $.decorator)),
        optional($.accessibility_modifier),
        optional($.override_modifier),
        optional('readonly'),
        field('pattern', choice($.pattern, $.this)),
      ),

      // Mapped types' modifiers: `-?:`, `+?:`, `?:`.
      omitting_type_annotation: $ => seq('-?:', $.type),
      adding_type_annotation: $ => seq('+?:', $.type),
      opting_type_annotation: $ => seq('?:', $.type),
      type_annotation: $ => seq(':', $.type),

      // `x: import('y').Z`: an import type's member access, read with lower
      // precedence than the type queries after `typeof` below.
      _type_query_member_expression_in_type_annotation: $ => seq(
        field('object', choice(
          $.import,
          alias($._type_query_member_expression_in_type_annotation, $.member_expression),
          alias($._type_query_call_expression_in_type_annotation, $.call_expression),
        )),
        '.',
        field('property', choice(
          $.private_property_identifier,
          reserved('properties', alias($.identifier, $.property_identifier)),
        )),
      ),

      _type_query_call_expression_in_type_annotation: $ => seq(
        field('function', choice(
          $.import,
          alias($._type_query_member_expression_in_type_annotation, $.member_expression),
        )),
        field('arguments', $.arguments),
      ),

      asserts: $ => seq('asserts', choice($.type_predicate, $.identifier, $.this)),

      asserts_annotation: $ => seq(seq(':', $.asserts)),

      type: $ => choice(
        $.primary_type,
        $.function_type,
        $.readonly_type,
        $.constructor_type,
        $.infer_type,
        prec(-1, alias($._type_query_member_expression_in_type_annotation, $.member_expression)),
        prec(-1, alias($._type_query_call_expression_in_type_annotation, $.call_expression)),
      ),

      // Labelled tuple elements: `[name: T, rest?: U, ...more: V[]]`. A
      // label may be one of TypeScript's contextual keywords (`[symbol: S]`).
      tuple_parameter: $ => seq(
        field('name', choice($.identifier, alias($._reserved_identifier, $.identifier), $.rest_pattern)),
        field('type', $.type_annotation),
      ),

      optional_tuple_parameter: $ => seq(
        field('name', choice($.identifier, alias($._reserved_identifier, $.identifier))),
        '?',
        field('type', $.type_annotation),
      ),

      optional_type: $ => seq($.type, '?'),
      rest_type: $ => seq('...', $.type),

      _tuple_type_member: $ => choice(
        alias($.tuple_parameter, $.required_parameter),
        alias($.optional_tuple_parameter, $.optional_parameter),
        $.optional_type,
        $.rest_type,
        $.type,
      ),

      constructor_type: $ => prec.left(seq(
        optional('abstract'),
        'new',
        field('type_parameters', optional($.type_parameters)),
        field('parameters', $.formal_parameters),
        '=>',
        field('type', $.type),
      )),

      primary_type: $ => choice(
        $.parenthesized_type,
        $.predefined_type,
        $._type_identifier,
        $.nested_type_identifier,
        $.generic_type,
        $.object_type,
        $.array_type,
        $.tuple_type,
        $.flow_maybe_type,
        $.type_query,
        $.index_type_query,
        alias($.this, $.this_type),
        $.existential_type,
        $.literal_type,
        $.lookup_type,
        $.conditional_type,
        $.template_literal_type,
        $.intersection_type,
        $.union_type,
        'const',
      ),

      template_type: $ => seq('${', choice($.primary_type, $.infer_type), '}'),

      template_literal_type: $ => seq(
        '`',
        repeat(choice(
          alias($._template_chars, $.string_fragment),
          $.template_type,
        )),
        '`',
      ),

      infer_type: $ => prec.right(seq(
        'infer',
        $._type_identifier,
        optional(seq('extends', $.type)),
      )),

      conditional_type: $ => prec.right(seq(
        field('left', $.type),
        'extends',
        field('right', $.type),
        '?',
        field('consequence', $.type),
        ':',
        field('alternative', $.type),
      )),

      generic_type: $ => prec('call', seq(
        field('name', choice($._type_identifier, $.nested_type_identifier)),
        field('type_arguments', $.type_arguments),
      )),

      // `x is T`. The name may lex as a predefined type's keyword
      // (`object is Foo`), so those are names here too.
      type_predicate: $ => seq(
        field('name', choice(
          $.identifier,
          $.this,
          alias($.predefined_type, $.identifier),
        )),
        'is',
        field('type', $.type),
      ),

      type_predicate_annotation: $ => seq(seq(':', $.type_predicate)),

      // `typeof x.y[0]`: a type query takes only names, member and index
      // accesses, calls (of `import`) and instantiations.
      _type_query_member_expression: $ => seq(
        field('object', choice(
          $.identifier,
          $.this,
          alias($._type_query_subscript_expression, $.subscript_expression),
          alias($._type_query_member_expression, $.member_expression),
          alias($._type_query_call_expression, $.call_expression),
        )),
        choice('.', alias($.optional_chain, '?.')),
        field('property', choice(
          $.private_property_identifier,
          reserved('properties', alias($.identifier, $.property_identifier)),
        )),
      ),

      _type_query_subscript_expression: $ => seq(
        field('object', choice(
          $.identifier,
          $.this,
          alias($._type_query_subscript_expression, $.subscript_expression),
          alias($._type_query_member_expression, $.member_expression),
          alias($._type_query_call_expression, $.call_expression),
        )),
        optional(alias($.optional_chain, '?.')),
        '[',
        field('index', choice($.predefined_type, $.string, $.number)),
        ']',
      ),

      _type_query_call_expression: $ => seq(
        field('function', choice(
          $.import,
          $.identifier,
          alias($._type_query_member_expression, $.member_expression),
          alias($._type_query_subscript_expression, $.subscript_expression),
        )),
        field('arguments', $.arguments),
      ),

      _type_query_instantiation_expression: $ => seq(
        field('function', choice(
          $.import,
          $.identifier,
          alias($._type_query_member_expression, $.member_expression),
          alias($._type_query_subscript_expression, $.subscript_expression),
        )),
        field('type_arguments', $.type_arguments),
      ),

      type_query: $ => prec.right(seq(
        'typeof',
        choice(
          alias($._type_query_subscript_expression, $.subscript_expression),
          alias($._type_query_member_expression, $.member_expression),
          alias($._type_query_call_expression, $.call_expression),
          alias($._type_query_instantiation_expression, $.instantiation_expression),
          $.identifier,
          $.this,
        ),
      )),

      // `keyof T`, and `keyof readonly T[]` (keyof takes any type operator).
      index_type_query: $ => seq('keyof', choice($.primary_type, $.readonly_type)),

      lookup_type: $ => seq($.primary_type, '[', $.type, ']'),

      mapped_type_clause: $ => seq(
        field('name', $._type_identifier),
        'in',
        field('type', $.type),
        optional(seq('as', field('alias', $.type))),
      ),

      literal_type: $ => choice(
        alias($._number, $.unary_expression),
        $.number,
        $.string,
        $.true,
        $.false,
        $.null,
        $.undefined,
      ),

      _number: $ => prec.left(1, seq(
        field('operator', choice('-', '+')),
        field('argument', $.number),
      )),

      existential_type: _ => '*',

      flow_maybe_type: $ => prec.right(seq('?', $.primary_type)),

      parenthesized_type: $ => seq('(', $.type, ')'),

      predefined_type: _ => choice(
        'any',
        'number',
        'boolean',
        'string',
        'symbol',
        alias(seq('unique', 'symbol'), 'unique symbol'),
        'void',
        'unknown',
        'string',
        'never',
        'object',
      ),

      type_arguments: $ => seq('<', commaSep1($.type), optional(','), '>'),

      // Members are separated by `,`, `;` or a line break.
      object_type: $ => seq(
        choice('{', '{|'),
        optional(seq(
          optional(choice(',', ';')),
          sepBy1(
            choice(',', $._semicolon),
            choice(
              $.export_statement,
              $.property_signature,
              $.call_signature,
              $.construct_signature,
              $.index_signature,
              $.method_signature,
            ),
          ),
          optional(choice(',', $._semicolon)),
        )),
        choice('}', '|}'),
      ),

      call_signature: $ => $._call_signature,

      property_signature: $ => seq(
        optional($.accessibility_modifier),
        optional('static'),
        optional($.override_modifier),
        optional('readonly'),
        field('name', $._property_name),
        optional('?'),
        field('type', optional($.type_annotation)),
      ),

      _call_signature: $ => seq(
        field('type_parameters', optional($.type_parameters)),
        field('parameters', $.formal_parameters),
        field('return_type', optional(
          choice($.type_annotation, $.asserts_annotation, $.type_predicate_annotation),
        )),
      ),

      type_parameters: $ => seq('<', commaSep1($.type_parameter), optional(','), '>'),

      // `const` (5.0) and the variance annotations `in` and `out` (4.7):
      // `<const T>`, `<in out T>`. `out` is also a name (`<out>`).
      type_parameter: $ => seq(
        optional('const'),
        optional('in'),
        optional('out'),
        field('name', choice($._type_identifier, alias('out', $.type_identifier))),
        field('constraint', optional($.constraint)),
        field('value', optional($.default_type)),
      ),

      default_type: $ => seq('=', $.type),

      constraint: $ => seq(choice('extends', ':'), $.type),

      construct_signature: $ => seq(
        optional('abstract'),
        'new',
        field('type_parameters', optional($.type_parameters)),
        field('parameters', $.formal_parameters),
        field('type', optional($.type_annotation)),
      ),

      // `[key: string]: T`, and a mapped type's `[K in keyof T]: U`.
      index_signature: $ => seq(
        optional(seq(
          field('sign', optional(choice('-', '+'))),
          'readonly',
        )),
        '[',
        choice(
          seq(
            field('name', choice($.identifier, alias($._reserved_identifier, $.identifier))),
            ':',
            field('index_type', $.type),
          ),
          $.mapped_type_clause,
        ),
        ']',
        field('type', choice(
          $.type_annotation,
          $.omitting_type_annotation,
          $.adding_type_annotation,
          $.opting_type_annotation,
        )),
      ),

      array_type: $ => seq($.primary_type, '[', ']'),

      tuple_type: $ => seq('[', commaSep($._tuple_type_member), optional(','), ']'),

      readonly_type: $ => seq('readonly', $.type),

      // A leading `|` or `&` is allowed: `type A = | B | C`.
      union_type: $ => prec.left(seq(optional($.type), '|', $.type)),
      intersection_type: $ => prec.left(seq(optional($.type), '&', $.type)),

      function_type: $ => prec.left(seq(
        field('type_parameters', optional($.type_parameters)),
        field('parameters', $.formal_parameters),
        '=>',
        field('return_type', choice($.type, $.asserts, $.type_predicate)),
      )),

      _type_identifier: $ => alias($.identifier, $.type_identifier),

      // TypeScript's contextual keywords, which are names wherever an
      // expression may also start with them.
      _reserved_identifier: (_, previous) => choice(
        'declare',
        'namespace',
        'type',
        'public',
        'private',
        'protected',
        'override',
        'readonly',
        'module',
        'any',
        'number',
        'boolean',
        'string',
        'symbol',
        'export',
        'object',
        'new',
        previous,
      ),
    },
  });
};
