; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; A Nix file's shape is its nested attrsets, so the outline is the
; bindings that have a shape of their own -- functions, attrsets, and calls
; given an attrset (`mkDerivation { ... }`, `mkShell { ... }`) -- nested by
; where they sit. Bindings to plain values are left out.

(binding
  attrpath: (attrpath) @name
  expression: (function_expression)) @definition.function

(binding
  attrpath: (attrpath) @name
  expression: [
    (attrset_expression)
    (rec_attrset_expression)
  ]) @definition.attrset

(binding
  attrpath: (attrpath) @name
  expression: (apply_expression
    argument: [
      (attrset_expression)
      (rec_attrset_expression)
    ])) @definition.attrset
