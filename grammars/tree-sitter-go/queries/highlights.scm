; Highlight captures for mep's Go grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (see TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and every narrower
; rule that should override them comes after. Adapted from
; tree-sitter/tree-sitter-go's queries (MIT, Max Brunsfeld).

; ---- Identifiers ----------------------------------------------------------------

(identifier) @variable
(type_identifier) @type
(field_identifier) @property
(package_identifier) @module
(label_name) @label

((identifier) @constant
 (#match? @constant "^[A-Z][A-Z0-9_]+$"))

(const_spec name: (identifier) @constant)

(parameter_declaration name: (identifier) @variable.parameter)
(variadic_parameter_declaration name: (identifier) @variable.parameter)

; The predeclared types.
((type_identifier) @type.builtin
 (#any-of? @type.builtin
  "any" "bool" "byte" "comparable" "complex128" "complex64" "error" "float32" "float64" "int" "int16"
  "int32" "int64" "int8" "rune" "string" "uint" "uint16" "uint32" "uint64" "uint8" "uintptr"))

; ---- Functions ------------------------------------------------------------------

(call_expression
  function: (identifier) @function.call)

(call_expression
  function: (selector_expression
    field: (field_identifier) @function.method.call))

; The predeclared functions.
(call_expression
  function: (identifier) @function.builtin
  (#any-of? @function.builtin
   "append" "cap" "clear" "close" "complex" "copy" "delete" "imag" "len" "make" "max" "min" "new" "panic"
   "print" "println" "real" "recover"))

(function_declaration
  name: (identifier) @function)

(method_declaration
  name: (field_identifier) @function.method)

(method_elem
  name: (field_identifier) @function.method)

; ---- Operators and punctuation --------------------------------------------------

[
  "--"
  "-"
  "-="
  ":="
  "!"
  "!="
  "..."
  "*"
  "*="
  "/"
  "/="
  "&"
  "&&"
  "&="
  "&^"
  "&^="
  "%"
  "%="
  "^"
  "^="
  "+"
  "++"
  "+="
  "<-"
  "<"
  "<<"
  "<<="
  "<="
  "="
  "=="
  ">"
  ">="
  ">>"
  ">>="
  "|"
  "|="
  "||"
  "~"
] @operator

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  "."
  ":"
  ";"
] @punctuation.delimiter

; ---- Keywords -------------------------------------------------------------------

[
  "break"
  "case"
  "chan"
  "const"
  "continue"
  "default"
  "defer"
  "else"
  "fallthrough"
  "for"
  "func"
  "go"
  "goto"
  "if"
  "import"
  "interface"
  "map"
  "package"
  "range"
  "return"
  "select"
  "struct"
  "switch"
  "type"
  "var"
] @keyword

; ---- Literals -------------------------------------------------------------------

[
  (interpreted_string_literal)
  (raw_string_literal)
  (rune_literal)
] @string

(escape_sequence) @string.escape

[
  (int_literal)
  (float_literal)
  (imaginary_literal)
] @number

[
  (true)
  (false)
] @boolean

[
  (nil)
  (iota)
] @constant.builtin

(comment) @comment
