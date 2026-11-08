; Blocks and their clauses, definitions, bracketed literals and argument or
; parameter lists, imports, templates, comments, and TypeScript's object
; types, interface and enum bodies; mep folds only the captures that span
; more than one row. .tsx adds its JSX elements
; (grammars/tree-sitter-tsx/queries/folds.scm).
[
  (function_declaration)
  (function_expression)
  (function_signature)
  (arrow_function)
  (generator_function)
  (generator_function_declaration)
  (method_definition)
  (class_body)
  (class_static_block)
  (statement_block)
  (switch_body)
  (switch_case)
  (switch_default)
  (catch_clause)
  (finally_clause)
  (else_clause)
  (object)
  (object_pattern)
  (array)
  (array_pattern)
  (arguments)
  (formal_parameters)
  (named_imports)
  (export_clause)
  (template_string)
  (parenthesized_expression)
  (comment)
  (interface_body)
  (object_type)
  (enum_body)
  (type_arguments)
  (type_parameters)
] @fold
