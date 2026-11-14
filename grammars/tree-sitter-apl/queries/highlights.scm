; APL highlighting (GNU APL and Dyalog APL).

(comment) @comment
(string) @string
(number) @number
(zilde) @constant.builtin

(identifier) @variable
(qualified_name) @variable

; ⎕ names, # and ##.
(system_name) @function.builtin
(system_command) @keyword.directive

; Glyphs: primitive functions and operators.
(primitive) @function.builtin
(operator) @operator
(branch) @keyword.return
(diamond) @punctuation.delimiter
"←" @operator

; dfns: ⍺ ⍵ ⍺⍺ ⍵⍵ and ∇ / ∇∇.
(special_name) @variable.parameter.builtin
(self_reference) @function.builtin
(guard ":" @keyword.conditional)
(error_guard "::" @keyword.exception)
["{" "}"] @punctuation.bracket
["(" ")" "[" "]"] @punctuation.bracket
";" @punctuation.delimiter

; Tradfns: ∇ header ... ∇.
"∇" @keyword.function
"⍫" @keyword.function
(tradfn_header name: (identifier) @function)
(tradfn_header (operator_spec operator: (identifier) @function))
(tradfn_header (operator_spec left_operand: (identifier) @variable.parameter))
(tradfn_header (operator_spec right_operand: (identifier) @variable.parameter))
(tradfn_header left: (identifier) @variable.parameter)
(tradfn_header left: (optional_name (identifier) @variable.parameter))
(tradfn_header right: (identifier) @variable.parameter)
(tradfn_header right: (optional_name (identifier) @variable.parameter))
(local (identifier) @variable)

; name←{...}: a dfn definition.
(assignment target: (identifier) @function . "←" . value: (dfn) .)

(label name: (identifier) @label)
(label ":" @label)

; Dyalog control structures and :Namespace scripts.
(keyword) @keyword
(namespace name: (_) @module)
(class_definition name: (identifier) @type)
