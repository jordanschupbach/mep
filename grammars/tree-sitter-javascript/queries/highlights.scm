; Highlight captures for mep's JavaScript grammar (nvim-treesitter naming).
;
; Where two patterns capture the same node, mep paints the later one on
; top, so the general patterns come first and the specific ones after.
;
; TypeScript's query is layered over this one (treesitter.cpp's
; TypescriptHighlights), so the first part names only nodes, tokens and
; shapes tree-sitter-typescript has too -- one it lacks would make the
; whole query fail to compile there. The two sections after it are cut
; out by their headers: "JavaScript only" for .ts and .tsx, "JSX" for .ts.

; ---- names ----------------------------------------------------------------

(identifier) @variable

[
  (property_identifier)
  (private_property_identifier)
] @property

; Capitalized names are classes and constructors, by convention.
((identifier) @constructor
  (#match? @constructor "^[A-Z]"))

; ALL_CAPS names read as constants.
([
  (identifier)
  (shorthand_property_identifier)
  (shorthand_property_identifier_pattern)
] @constant
  (#match? @constant "^[A-Z_][A-Z\\d_]+$"))

((identifier) @variable.builtin
  (#any-of? @variable.builtin
    "arguments" "module" "exports" "console" "window" "document" "globalThis" "process"))

((identifier) @module
  (#any-of? @module "Math" "JSON" "Reflect" "Atomics" "Intl"))

(statement_identifier) @label

; ---- functions --------------------------------------------------------------

(function_expression name: (identifier) @function)
(function_declaration name: (identifier) @function)
(generator_function name: (identifier) @function)
(generator_function_declaration name: (identifier) @function)

(method_definition name: [(property_identifier) (private_property_identifier)] @function.method)
(method_definition name: (property_identifier) @constructor
  (#eq? @constructor "constructor"))

(pair
  key: (property_identifier) @function.method
  value: [(function_expression) (arrow_function)])

(assignment_expression
  left: (member_expression property: (property_identifier) @function.method)
  right: [(function_expression) (arrow_function)])

(variable_declarator
  name: (identifier) @function
  value: [(function_expression) (arrow_function)])

(assignment_expression
  left: (identifier) @function
  right: [(function_expression) (arrow_function)])

(call_expression function: (identifier) @function.call)

(call_expression
  function: (member_expression
    property: [(property_identifier) (private_property_identifier)] @method.call))

(new_expression constructor: (identifier) @constructor)

((call_expression function: (identifier) @function.builtin)
  (#any-of? @function.builtin
    "require" "parseInt" "parseFloat" "isNaN" "isFinite" "encodeURI" "encodeURIComponent"
    "decodeURI" "decodeURIComponent" "setTimeout" "setInterval" "clearTimeout" "clearInterval"
    "queueMicrotask" "structuredClone" "fetch"))

(decorator "@" @attribute)
(decorator (identifier) @attribute)
(decorator (call_expression function: (identifier) @attribute))
(decorator (member_expression property: (property_identifier) @attribute))

; ---- literals ---------------------------------------------------------------

[
  (this)
  (super)
] @variable.builtin

[
  (true)
  (false)
] @boolean

[
  (null)
  (undefined)
] @constant.builtin

(number) @number

[
  (string)
  (template_string)
] @string

(escape_sequence) @string.escape

(regex) @string.special

(template_substitution
  "${" @punctuation.interpolation
  "}" @punctuation.interpolation)

[
  (comment)
  (html_comment)
] @comment

(hash_bang_line) @preproc

; ---- keywords ---------------------------------------------------------------

[
  "class"
  "const"
  "debugger"
  "default"
  "extends"
  "get"
  "let"
  "set"
  "static"
  "var"
  "with"
] @keyword

(meta_property ["new" "import" "target" "meta"] @keyword)

[
  "async"
  "function"
] @keyword.function

[
  "await"
  "yield"
] @keyword

[
  "return"
] @keyword.return

[
  "if"
  "else"
  "switch"
  "case"
] @conditional

(switch_default "default" @conditional)

[
  "for"
  "while"
  "do"
  "break"
  "continue"
] @repeat

[
  "try"
  "catch"
  "finally"
  "throw"
] @exception

[
  "import"
  "export"
  "from"
  "as"
] @include

(import) @include

[
  "delete"
  "in"
  "instanceof"
  "new"
  "typeof"
  "void"
] @keyword.operator

(for_in_statement ["in" "of"] @repeat)

; ---- operators and punctuation ----------------------------------------------

[
  "-" "--" "-=" "+" "++" "+=" "*" "*=" "**" "**=" "/" "/=" "%" "%="
  "<" "<=" "<<" "<<=" "=" "==" "===" "!" "!=" "!==" "=>" ">" ">="
  ">>" ">>=" ">>>" ">>>=" "~" "^" "&" "|" "^=" "&=" "|=" "&&" "||"
  "??" "&&=" "||=" "??=" "..."
] @operator

[
  ";"
  (optional_chain)
  "."
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

; A conditional's `?` and `:` read as keywords (an object's `:` does not).
(ternary_expression ["?" ":"] @conditional)

; A regex's slashes are part of the literal, not division.
(regex "/" @string.special)

; ---- JavaScript only: parameters -----------------------------------------
; (TypeScript wraps each parameter in required_parameter/optional_parameter,
; so these could never match its tree: cut off for .ts and .tsx.)

(formal_parameters (identifier) @variable.parameter)
(formal_parameters (assignment_pattern left: (identifier) @variable.parameter))
(formal_parameters (rest_pattern (identifier) @variable.parameter))
(formal_parameters (object_pattern (shorthand_property_identifier_pattern) @variable.parameter))
(formal_parameters (assignment_pattern left: (object_pattern (shorthand_property_identifier_pattern) @variable.parameter)))
(formal_parameters (array_pattern (identifier) @variable.parameter))
(arrow_function parameter: (identifier) @variable.parameter)
(catch_clause parameter: (identifier) @variable.parameter)

; ---- JSX (not in tree-sitter-typescript's .ts grammar: cut off there) -----

(jsx_opening_element name: (identifier) @tag)
(jsx_closing_element name: (identifier) @tag)
(jsx_self_closing_element name: (identifier) @tag)

; A capitalized element name is a component, not an HTML tag.
(jsx_opening_element name: (identifier) @constructor (#match? @constructor "^[A-Z]"))
(jsx_closing_element name: (identifier) @constructor (#match? @constructor "^[A-Z]"))
(jsx_self_closing_element name: (identifier) @constructor (#match? @constructor "^[A-Z]"))

(jsx_opening_element name: (member_expression) @constructor)
(jsx_closing_element name: (member_expression) @constructor)
(jsx_self_closing_element name: (member_expression) @constructor)

(jsx_namespace_name) @tag

(jsx_attribute (property_identifier) @attribute)

(jsx_opening_element ["<" ">"] @punctuation.bracket)
(jsx_closing_element ["</" ">"] @punctuation.bracket)
(jsx_self_closing_element ["<" "/>"] @punctuation.bracket)

(html_character_reference) @string.escape

(jsx_expression
  "{" @punctuation.interpolation
  "}" @punctuation.interpolation)
