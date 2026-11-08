; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; A Maxima file is a list of top-level statements: the outline is its
; function and macro definitions (`f(x) := ...`, `m(x) ::= ...`, and
; `f[n](x) := ...` memoizing ones) and its top-level assignments.

(statement
  (binary_expression
    . (function_call . (atom (identifier) @name))
    [":=" "::="])) @definition.function

(statement
  (binary_expression
    . (function_call . (array_ref . (atom (identifier) @name)))
    [":=" "::="])) @definition.function

(statement
  (binary_expression
    . (array_ref . (atom (identifier) @name))
    ":=")) @definition.function

(statement
  (binary_expression
    . (atom (identifier) @name)
    ":")) @definition.variable
