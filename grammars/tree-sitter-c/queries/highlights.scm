; Highlight captures for mep's C grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.

; ---- names ----------------------------------------------------------------

(identifier) @variable

; ALL_CAPS names read as constants (macros, mostly).
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z\\d_]*$"))

(field_identifier) @property

(statement_identifier) @label

(enumerator name: (identifier) @constant)

[
  (type_identifier)
  (primitive_type)
  (sized_type_specifier)
] @type

(parameter_declaration declarator: (identifier) @variable.parameter)
(parameter_declaration declarator: (pointer_declarator declarator: (identifier) @variable.parameter))
(parameter_declaration
  declarator: (pointer_declarator
    declarator: (pointer_declarator declarator: (identifier) @variable.parameter)))
(parameter_declaration declarator: (array_declarator declarator: (identifier) @variable.parameter))

; ---- functions and macros -------------------------------------------------

(function_declarator declarator: (identifier) @function)
(function_declarator declarator: (parenthesized_declarator (pointer_declarator declarator: (identifier) @function)))

(call_expression function: (identifier) @function.call)
(call_expression function: (field_expression field: (field_identifier) @function.call))

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
  "auto"
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
  "__declspec"
  "asm"
  "__asm__"
  "__asm"
  "goto"
  "break"
  "continue"
  (ms_call_modifier)
  (gnu_asm_qualifier)
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
] @keyword.operator

"return" @keyword.return

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
] @exception

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
  "==" "!=" "<" ">" "<=" ">="
  "&&" "||" "!"
  "&" "|" "^" "~" "<<" ">>"
  "->"
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

; ---- literals and comments ------------------------------------------------

(number_literal) @number

(char_literal) @character

[
  (string_literal)
  (system_lib_string)
] @string

(escape_sequence) @string.escape

[
  (true)
  (false)
] @boolean

(null) @constant.builtin

(comment) @comment
