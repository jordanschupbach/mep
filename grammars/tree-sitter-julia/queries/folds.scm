; Fold ranges for mep's Julia grammar: every block that ends in `end`, each
; branch of an if and each clause of a try, and any bracketed list or
; string (a docstring) spanning lines; mep folds only the captures that span
; more than one row.
[
  (module_definition)
  (function_definition)
  (macro_definition)
  (struct_definition)
  (abstract_definition)
  (primitive_definition)
  (compound_statement)
  (quote_statement)
  (let_statement)
  (if_statement)
  (elseif_clause)
  (else_clause)
  (for_statement)
  (while_statement)
  (try_statement)
  (catch_clause)
  (finally_clause)
  (do_clause)
  (argument_list)
  (tuple_expression)
  (vector_expression)
  (matrix_expression)
  (comprehension_expression)
  (curly_expression)
  (string_literal)
  (command_literal)
  (prefixed_string_literal)
] @fold
