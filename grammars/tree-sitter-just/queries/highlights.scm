; Highlight captures for mep's justfile grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.

; ---- names ----------------------------------------------------------------

(value (identifier) @variable)

(assignment name: (identifier) @variable)
(export name: (identifier) @variable)
(unexport name: (identifier) @variable)

(recipe_header name: (identifier) @function)
(alias name: (identifier) @function)
(alias target: (module_path (identifier) @function.call))
(dependency (identifier) @function.call)
(dependency_expression name: (module_path (identifier) @function.call))
(module name: (identifier) @module)

(parameter name: (identifier) @variable.parameter)

(function_call name: (identifier) @function.builtin)

(setting name: (identifier) @property)

(attribute_item name: (identifier) @attribute)

; ---- keywords and operators -----------------------------------------------

[
  "alias"
  "export"
  "unexport"
  "set"
] @keyword

[
  "import"
  "mod"
] @include

[
  "if"
  "else"
] @conditional

(boolean) @boolean

(binary_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(comparison operator: _ @operator)

[
  ":="
  "="
  "&&"
  "::"
] @operator

(recipe_header quiet: "@" @operator)
(parameter export: "$" @operator)
(variadic_parameter kleene: _ @operator)
(line_prefix) @operator

; ---- strings --------------------------------------------------------------

[
  (string)
  (raw_string)
  (indented_string)
  (indented_raw_string)
] @string

(escape_sequence) @string.escape

; A backtick runs a command: not text, so it gets a colour of its own.
(backtick) @string.special

(interpolation
  "{{" @punctuation.interpolation
  "}}" @punctuation.interpolation)

; ---- recipe bodies --------------------------------------------------------

; The body itself is shell (or a script) and stays uncoloured; its shebang
; line says which interpreter runs it.
(recipe_body
  .
  (recipe_line
    .
    (text) @keyword.directive)
  (#match? @keyword.directive "^#!"))

; ---- punctuation ----------------------------------------------------------

[
  ","
  ":"
] @punctuation.delimiter

[
  "("
  ")"
  "["
  "]"
  "{"
  "}"
] @punctuation.bracket

(comment) @comment
