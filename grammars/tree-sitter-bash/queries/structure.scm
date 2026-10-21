; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; `name() { ...; }` and `function name { ...; }`. A function defined inside
; another nests under it.
(function_definition name: (word) @name) @definition.function

; Top-level settings a reader looks for: `readonly NAME=...`, `export
; NAME=...`, `declare -r NAME=...`, and plain ALL_CAPS assignments
; (`VERSION=1.2`) -- so ordinary variables stay out of the outline.
(program
  (declaration_command
    (variable_assignment name: (variable_name) @name)) @definition.variable)

(program
  (variable_assignment name: (variable_name) @name) @definition.variable
  (#match? @name "^[A-Z][A-Z0-9_]*$"))
