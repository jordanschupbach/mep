; Highlight captures for mep's R grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.

; ---- names ----------------------------------------------------------------

(identifier) @variable

(parameter name: (identifier) @variable.parameter)
(argument name: (identifier) @variable.parameter)

(namespace_operator lhs: (identifier) @module)

; Calls: `f()`, `pkg::f()`, `obj$method()`.
(call function: (identifier) @function.call)
(call function: (namespace_operator rhs: (identifier) @function.call))
(call function: (extract_operator rhs: (identifier) @function.method.call))

; Definitions: `f <- function(...)`, `f = \(x)`, `function(...) ... -> f`.
(binary_operator
  lhs: (identifier) @function
  operator: ["<-" "<<-" "=" ":="]
  rhs: (function_definition))
(binary_operator
  lhs: (function_definition)
  operator: ["->" "->>"]
  rhs: (identifier) @function)

; Functions that read as keywords.
((call function: (identifier) @keyword.return)
 (#any-of? @keyword.return "return" "invisible"))
((call function: (identifier) @exception)
 (#any-of? @exception "stop" "warning" "tryCatch" "try" "on.exit" "signalCondition"))
((call function: (identifier) @include)
 (#any-of? @include "library" "require" "requireNamespace" "loadNamespace" "source"))

; ---- keywords -------------------------------------------------------------

(function_definition name: "function" @keyword.function)
(function_definition name: "\\" @keyword.function)

[
  "if"
  "else"
] @conditional

[
  "for"
  "while"
  "repeat"
  "in"
] @repeat

[
  (next)
  (break)
] @keyword

; ---- literals -------------------------------------------------------------

[
  (true)
  (false)
] @boolean

[
  (null)
  (inf)
  (nan)
  (na)
  (dots)
  (dot_dot_i)
] @constant.builtin

[
  (integer)
  (float)
  (complex)
] @number

(string) @string
(escape_sequence) @string.escape

; ---- operators and punctuation --------------------------------------------

(unary_operator operator: _ @operator)
(binary_operator operator: _ @operator)
(extract_operator operator: _ @operator)
(namespace_operator operator: _ @operator)
(parameter "=" @operator)
(argument "=" @operator)

[
  "("
  ")"
  "{"
  "}"
  "["
  "]"
  "[["
  "]]"
] @punctuation.bracket

(comma) @punctuation.delimiter

(comment) @comment

(ERROR) @error
