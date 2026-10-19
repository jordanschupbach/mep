; Highlight captures for mep's Lua grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.

; ---- keywords -------------------------------------------------------------

"return" @keyword.return

[
  "goto"
  "in"
  "local"
  "global"
] @keyword

(label_statement) @label

(break_statement) @keyword

(do_statement ["do" "end"] @keyword)

(while_statement ["while" "do" "end"] @repeat)

(repeat_statement ["repeat" "until"] @repeat)

(for_statement ["for" "do" "end"] @repeat)

(if_statement ["if" "then" "end"] @conditional)

(elseif_statement ["elseif" "then"] @conditional)

(else_statement "else" @conditional)

(function_declaration ["function" "end"] @keyword.function)

(function_definition ["function" "end"] @keyword.function)

; ---- operators and punctuation --------------------------------------------

(binary_expression operator: _ @operator)

(unary_expression operator: _ @operator)

"=" @operator

[
  "and"
  "not"
  "or"
] @keyword.operator

[
  ";"
  ":"
  ","
  "."
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

; ---- names ----------------------------------------------------------------

(identifier) @variable

; ALL_CAPS names read as constants.
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z_0-9]*$"))

; The standard library's tables, and mep's own API.
((identifier) @module
  (#any-of? @module "string" "table" "math" "os" "io" "coroutine" "utf8" "debug" "package"))

((identifier) @variable.builtin
  (#any-of? @variable.builtin "self" "_G" "_ENV" "_VERSION" "mep"))

(attribute
  "<" @punctuation.bracket
  (identifier) @attribute
  ">" @punctuation.bracket)

(vararg_expression) @constant

(nil) @constant.builtin

[
  (false)
  (true)
] @boolean

; ---- tables ---------------------------------------------------------------

(field name: (identifier) @field)

(dot_index_expression field: (identifier) @field)

(table_constructor ["{" "}"] @constructor)

; ---- functions ------------------------------------------------------------

(parameters name: (identifier) @parameter)

(function_declaration
  name: [
    (identifier) @function
    (dot_index_expression field: (identifier) @function)
  ])

(function_declaration
  name: (method_index_expression method: (identifier) @method))

; `f = function() end`, `M.f = function() end`
(assignment_statement
  (variable_list
    .
    name: [
      (identifier) @function
      (dot_index_expression field: (identifier) @function)
    ])
  (expression_list
    .
    value: (function_definition)))

(table_constructor
  (field
    name: (identifier) @function
    value: (function_definition)))

(function_call
  name: [
    (identifier) @function.call
    (dot_index_expression field: (identifier) @function.call)
    (method_index_expression method: (identifier) @method.call)
  ])

; Lua 5.4's global functions (and 5.1's, still common in plugin code).
(function_call
  name: (identifier) @function.builtin
  (#any-of? @function.builtin
    "assert" "collectgarbage" "dofile" "error" "getmetatable" "ipairs" "load" "loadfile" "next"
    "pairs" "pcall" "print" "rawequal" "rawget" "rawlen" "rawset" "require" "select"
    "setmetatable" "tonumber" "tostring" "type" "warn" "xpcall"
    "getfenv" "setfenv" "loadstring" "module" "unpack"))

; ---- literals and comments ------------------------------------------------

(number) @number

(string) @string

(escape_sequence) @string.escape

(hash_bang_line) @preproc

(comment) @comment
