; Fold ranges for mep's fish grammar: every block that ends in `end` (or
; `}`), each branch of an if, each case, and a multi-line substitution.
[
  (function_definition)
  (begin_statement)
  (if_statement)
  (else_if_clause)
  (else_clause)
  (while_statement)
  (for_statement)
  (switch_statement)
  (case_clause)
  (command_substitution)
] @fold
