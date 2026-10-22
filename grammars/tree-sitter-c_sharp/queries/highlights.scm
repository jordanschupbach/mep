; Highlight captures for mep's C# grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes are
; resolved later-pattern-wins (TreesitterHighlight's sort in treesitter.cpp),
; so the broad conventions come first and every narrower rule that should
; override them comes after.

; ---- Names ------------------------------------------------------------------

(identifier) @variable

(implicit_parameter) @variable.parameter

(parameter name: (identifier) @variable.parameter)

(discard) @variable.builtin

[
  "this"
  "base"
] @variable.builtin

; Members: `a.b`, `?.b`, and the declarations of fields and properties.
(member_access_expression name: (identifier) @variable.member)
(member_binding_expression name: (identifier) @variable.member)
(field_declaration (variable_declaration (variable_declarator name: (identifier) @variable.member)))
(property_declaration name: (identifier) @property)
(indexer_declaration "this" @property)
(event_declaration name: (identifier) @property)
(event_field_declaration (variable_declaration (variable_declarator name: (identifier) @property)))
(with_initializer (identifier) @property)
(subpattern (identifier) @property)
(anonymous_object_creation_expression (identifier) @property)
(initializer_expression (assignment_expression left: (identifier) @property))

(enum_member_declaration name: (identifier) @constant)

(labeled_statement (identifier) @label)
(goto_statement (identifier) @label)

; ---- Types ------------------------------------------------------------------

(predefined_type) @type.builtin

(implicit_type) @keyword

; Every `type:` field holds a type: declarations, casts, `new`, patterns,
; `typeof`, `default(T)`, parameters, catch clauses...
(_ type: (identifier) @type)
(_ type: (qualified_name name: (identifier) @type))
(_ type: (alias_qualified_name name: (identifier) @type))
(_ returns: (identifier) @type)
(_ returns: (qualified_name name: (identifier) @type))

(generic_name (identifier) @type)
(type_argument_list (identifier) @type)
(type_argument_list (qualified_name name: (identifier) @type))
(base_list (identifier) @type)
(base_list (qualified_name name: (identifier) @type))
(base_list (primary_constructor_base_type type: (identifier) @type))
(array_type type: (identifier) @type)
(nullable_type type: (identifier) @type)
(pointer_type type: (identifier) @type)
(tuple_element type: (identifier) @type)
(ref_type type: (identifier) @type)
(as_expression right: (identifier) @type)
(is_expression right: (identifier) @type)
(type_parameter_constraint (identifier) @type)
(explicit_interface_specifier (identifier) @type)
(explicit_interface_specifier (generic_name (identifier) @type))
(tuple_element name: (identifier) @variable.member)

(type_parameter name: (identifier) @type.definition)
(type_parameter_constraints_clause . (identifier) @type.definition)

[
  (class_declaration name: (identifier) @type.definition)
  (struct_declaration name: (identifier) @type.definition)
  (interface_declaration name: (identifier) @type.definition)
  (enum_declaration name: (identifier) @type.definition)
  (record_declaration name: (identifier) @type.definition)
  (delegate_declaration name: (identifier) @type.definition)
]

; Namespaces: declarations, `using` directives, `alias::`.
(namespace_declaration name: (identifier) @module)
(namespace_declaration name: (qualified_name (identifier) @module))
(namespace_declaration name: (qualified_name (qualified_name (identifier) @module)))
(file_scoped_namespace_declaration name: (identifier) @module)
(file_scoped_namespace_declaration name: (qualified_name (identifier) @module))
(file_scoped_namespace_declaration name: (qualified_name (qualified_name (identifier) @module)))
(using_directive (identifier) @module)
(using_directive (qualified_name (identifier) @module))
(using_directive (qualified_name (qualified_name (identifier) @module)))
(using_directive name: (identifier) @type.definition)
(extern_alias_directive name: (identifier) @module)
(alias_qualified_name alias: (identifier) @module)

; ---- Functions --------------------------------------------------------------

(method_declaration name: (identifier) @function.method)
(local_function_statement name: (identifier) @function)
(constructor_declaration name: (identifier) @constructor)
(destructor_declaration name: (identifier) @constructor)
(operator_declaration operator: _ @function)

(invocation_expression function: (identifier) @function.call)
(invocation_expression function: (generic_name (identifier) @function.call))
(invocation_expression function: (member_access_expression name: (identifier) @function.method.call))
(invocation_expression
  function: (member_access_expression name: (generic_name (identifier) @function.method.call)))
(invocation_expression function: (conditional_access_expression (member_binding_expression name: (identifier) @function.method.call)))

; `nameof(x)` is an operator, not a call.
((invocation_expression function: (identifier) @keyword.operator)
  (#eq? @keyword.operator "nameof"))

(attribute name: (identifier) @attribute)
(attribute name: (qualified_name name: (identifier) @attribute))
(attribute_target_specifier) @attribute

; ---- Literals ---------------------------------------------------------------

[
  (integer_literal)
  (real_literal)
] @number

[
  (string_literal)
  (verbatim_string_literal)
  (raw_string_literal)
  (interpolated_string_expression)
  (interpolation_start)
  (interpolation_quote)
] @string

(character_literal) @character

(escape_sequence) @string.escape

(string_literal_encoding) @string.special

(interpolation_format_clause) @string.special

(boolean_literal) @boolean

(null_literal) @constant.builtin

; The braces of `{expr}` in an interpolated string, painted over the string.
(interpolation_brace) @punctuation.special

; ---- Comments and directives ----------------------------------------------

(comment) @comment

(shebang_directive) @keyword.directive

[
  "#if"
  "#elif"
  "#else"
  "#endif"
  "#region"
  "#endregion"
  "#define"
  "#undef"
  "#line"
  "#pragma"
  "#nullable"
  "#error"
  "#warning"
] @keyword.directive

(preproc_if condition: (identifier) @constant.macro)
(preproc_elif condition: (identifier) @constant.macro)
(preproc_define (preproc_arg) @constant.macro)
(preproc_undef (preproc_arg) @constant.macro)
(preproc_region content: (preproc_arg) @comment)
(preproc_endregion content: (preproc_arg) @comment)
(preproc_error (preproc_arg) @string)
(preproc_warning (preproc_arg) @string)
(preproc_pragma (identifier) @constant)

; ---- Keywords ---------------------------------------------------------------

[
  "class"
  "struct"
  "interface"
  "enum"
  "record"
  "delegate"
  "event"
  "namespace"
  "operator"
  "implicit"
  "explicit"
  "extern"
  "alias"
  "global"
  "assembly"
  "module"
  "params"
  "ref"
  "out"
  "in"
  "readonly"
  "scoped"
  "unsafe"
  "fixed"
  "lock"
  "checked"
  "unchecked"
  "managed"
  "unmanaged"
  "notnull"
  "where"
  "static"
  "async"
  "get"
  "set"
  "init"
  "add"
  "remove"
] @keyword

"using" @include

[
  "if"
  "else"
  "switch"
  "case"
  "default"
  "when"
] @conditional

[
  "for"
  "foreach"
  "while"
  "do"
  "continue"
  "break"
] @repeat

[
  "return"
  "yield"
  "goto"
] @keyword.return

[
  "try"
  "catch"
  "finally"
  "throw"
] @exception

[
  "new"
  "stackalloc"
  "typeof"
  "sizeof"
  "is"
  "as"
  "and"
  "or"
  "not"
  "with"
  "await"
  "__makeref"
  "__reftype"
  "__refvalue"
] @keyword.operator

; Query expressions (LINQ).
[
  "from"
  "into"
  "join"
  "on"
  "equals"
  "let"
  "orderby"
  "ascending"
  "descending"
  "select"
  "group"
  "by"
] @keyword

; Declaration modifiers (over their keyword token, which the lists above
; also match).
(modifier) @keyword.modifier

; `default` as a value (`default(T)`, `x = default`) rather than a label.
(default_expression "default" @keyword.operator)

; ---- Punctuation and operators ---------------------------------------------

[
  ";"
  "."
  ","
  "::"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  "="
  "+="
  "-="
  "*="
  "/="
  "%="
  "&="
  "|="
  "^="
  "<<="
  ">>="
  ">>>="
  "??="
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "+"
  "-"
  "*"
  "/"
  "%"
  "&"
  "|"
  "^"
  "~"
  "!"
  "<<"
  ">>"
  ">>>"
  "&&"
  "||"
  "??"
  "?"
  "++"
  "--"
  "=>"
  "->"
  ".."
] @operator

(conditional_expression ":" @operator)

; The angle brackets of a type argument or parameter list are brackets.
(type_argument_list ["<" ">"] @punctuation.bracket)
(type_parameter_list ["<" ">"] @punctuation.bracket)
(function_pointer_type ["<" ">"] @punctuation.bracket)
