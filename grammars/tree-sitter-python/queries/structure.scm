; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
; Decorated definitions nest the same way: the decorator is outside the
; function_definition, which keeps its own `def` line as the extent.

(class_definition name: (identifier) @name) @definition.class
(function_definition name: (identifier) @name) @definition.function

; `type Vector = list[float]`, `type Pair[T] = tuple[T, T]` (PEP 695)
(type_alias_statement
  left: (type [(identifier) @name (generic_type (identifier) @name)])) @definition.type

; Module-level constants, `MAX_SIZE = 10` or `TIMEOUT: float = 1.5`: an
; ALL_CAPS name, so ordinary module variables stay out of the outline.
(module
  (expression_statement
    (assignment
      left: (identifier) @name) @definition.variable)
  (#match? @name "^[A-Z][A-Z0-9_]*$"))
