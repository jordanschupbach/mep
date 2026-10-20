; Highlight captures for mep's C++ grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.
;
; The C grammar's query (grammars/tree-sitter-c/queries/highlights.scm),
; less what C++ lacks (`auto` is a type here, not a storage class), with
; C++'s own constructs after each part.

; ---- names ----------------------------------------------------------------

(identifier) @variable

; ALL_CAPS names read as constants (macros, mostly).
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z\\d_]*$"))

(field_identifier) @property

(statement_identifier) @label

(enumerator name: (identifier) @constant)

(namespace_identifier) @module

[
  (type_identifier)
  (primitive_type)
  (sized_type_specifier)
  (auto)
] @type

; A capitalized name in a scope is a type or a constant's class
; (`Kind::Value`, `Point::Point`).
(qualified_identifier
  scope: (namespace_identifier) @type
  (#match? @type "^[A-Z]"))

(type_parameter_declaration (type_identifier) @type.definition)
(variadic_type_parameter_declaration (type_identifier) @type.definition)
(optional_type_parameter_declaration name: (type_identifier) @type.definition)

(parameter_declaration declarator: (identifier) @variable.parameter)
(parameter_declaration declarator: (pointer_declarator declarator: (identifier) @variable.parameter))
(parameter_declaration
  declarator: (pointer_declarator
    declarator: (pointer_declarator declarator: (identifier) @variable.parameter)))
(parameter_declaration declarator: (array_declarator declarator: (identifier) @variable.parameter))
(parameter_declaration declarator: (reference_declarator (identifier) @variable.parameter))
(optional_parameter_declaration declarator: (identifier) @variable.parameter)
(optional_parameter_declaration declarator: (reference_declarator (identifier) @variable.parameter))
(optional_parameter_declaration declarator: (pointer_declarator declarator: (identifier) @variable.parameter))
(variadic_parameter_declaration declarator: (variadic_declarator (identifier) @variable.parameter))
(variadic_parameter_declaration
  declarator: (reference_declarator (variadic_declarator (identifier) @variable.parameter)))

(this) @variable.builtin

; ---- functions and macros -------------------------------------------------

(function_declarator declarator: (identifier) @function)
(function_declarator declarator: (parenthesized_declarator (pointer_declarator declarator: (identifier) @function)))
(function_declarator declarator: (qualified_identifier name: (identifier) @function))
(function_declarator
  declarator: (qualified_identifier name: (qualified_identifier name: (identifier) @function)))
(function_declarator declarator: (field_identifier) @function.method)
(function_declarator declarator: (template_function name: (identifier) @function))
(function_declarator declarator: (operator_name) @function)
(function_declarator declarator: (qualified_identifier name: (operator_name) @function))

; Constructors (no return type) and destructors.
(function_definition !type declarator: (function_declarator declarator: (identifier) @constructor))
(function_definition
  !type
  declarator: (function_declarator declarator: (qualified_identifier name: (identifier) @constructor)))
(declaration !type declarator: (function_declarator declarator: (identifier) @constructor))
(function_declarator declarator: (destructor_name (identifier) @constructor))
(function_declarator declarator: (qualified_identifier name: (destructor_name (identifier) @constructor)))

(call_expression function: (identifier) @function.call)
(call_expression function: (field_expression field: (field_identifier) @function.method.call))
(call_expression function: (qualified_identifier name: (identifier) @function.call))
(call_expression
  function: (qualified_identifier name: (qualified_identifier name: (identifier) @function.call)))
(call_expression function: (template_function name: (identifier) @function.call))
(call_expression
  function: (qualified_identifier name: (template_function name: (identifier) @function.call)))
(call_expression function: (field_expression field: (template_method name: (field_identifier) @function.method.call)))

(preproc_def name: (identifier) @constant.macro)
(preproc_function_def name: (identifier) @function.macro)
(preproc_params (identifier) @variable.parameter)

(attribute name: (identifier) @attribute)
(attribute_specifier (argument_list (identifier) @attribute))

; ---- keywords -------------------------------------------------------------

[
  "typedef"
  "struct"
  "union"
  "enum"
  "extern"
  "static"
  "register"
  "inline"
  "__inline"
  "__inline__"
  "__forceinline"
  "thread_local"
  "__thread"
  "const"
  "constexpr"
  "volatile"
  "restrict"
  "__restrict__"
  "_Atomic"
  "_Noreturn"
  "noreturn"
  "__extension__"
  "__attribute__"
  "__attribute"
  "__declspec"
  "asm"
  "__asm__"
  "__asm"
  "goto"
  "break"
  "continue"
  (ms_call_modifier)
  (gnu_asm_qualifier)
  ; C++
  "class"
  "namespace"
  "template"
  "typename"
  "using"
  "concept"
  "requires"
  "friend"
  "virtual"
  "explicit"
  "mutable"
  "constinit"
  "consteval"
  "operator"
  "static_assert"
  (access_specifier)
  (virtual_specifier)
] @keyword

[
  "sizeof"
  "offsetof"
  "_Generic"
  "_Alignof"
  "alignof"
  "__alignof__"
  "__alignof"
  "_alignof"
  "alignas"
  "_Alignas"
  ; C++
  "new"
  "delete"
  "decltype"
  "noexcept"
  "co_await"
  "and"
  "or"
  "not"
  "xor"
  "bitand"
  "bitor"
  "compl"
  "not_eq"
  "and_eq"
  "or_eq"
  "xor_eq"
] @keyword.operator

[
  "return"
  "co_return"
  "co_yield"
] @keyword.return

[
  "if"
  "else"
  "switch"
  "case"
  "default"
] @conditional

[
  "for"
  "while"
  "do"
] @repeat

[
  "__try"
  "__except"
  "__finally"
  "__leave"
  "try"
  "catch"
  "throw"
] @exception

; `= default;` and `= delete;` are declarations' keywords, not a switch's
; label or an operator.
(default_method_clause "default" @keyword)
(delete_method_clause "delete" @keyword)

; ---- the preprocessor -----------------------------------------------------

[
  "#define"
  "#if"
  "#ifdef"
  "#ifndef"
  "#elif"
  "#elifdef"
  "#elifndef"
  "#else"
  "#endif"
  (preproc_directive)
] @preproc

"#include" @include

"defined" @keyword.operator

; ---- operators and punctuation --------------------------------------------

[
  "="
  "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  "+" "-" "*" "/" "%"
  "++" "--"
  "==" "!=" "<" ">" "<=" ">=" "<=>"
  "&&" "||" "!"
  "&" "|" "^" "~" "<<" ">>"
  "->" ".*" "->*"
  "?"
] @operator

(conditional_expression ":" @operator)

[
  "."
  ","
  ";"
  ":"
  "::"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
  "[["
  "]]"
] @punctuation.bracket

; A template's angle brackets are brackets, not comparisons.
(template_argument_list ["<" ">"] @punctuation.bracket)
(template_parameter_list ["<" ">"] @punctuation.bracket)

"..." @punctuation.special

(lambda_default_capture) @operator

; ---- literals and comments ------------------------------------------------

(number_literal) @number

(char_literal) @character

[
  (string_literal)
  (raw_string_literal)
  (system_lib_string)
] @string

(raw_string_delimiter) @string.special

(escape_sequence) @string.escape

(literal_suffix) @type

[
  (true)
  (false)
] @boolean

(null) @constant.builtin

(comment) @comment
