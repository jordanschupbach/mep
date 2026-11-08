; Fold ranges for mep's Rust grammar: item bodies, blocks, match arms and
; literals that span lines (a one-line node never folds). Runs of comments
; are folded by the editor itself.
[
  (block)
  (declaration_list)
  (field_declaration_list)
  (ordered_field_declaration_list)
  (enum_variant_list)
  (match_block)
  (match_arm)
  (use_list)
  (field_initializer_list)
  (arguments)
  (parameters)
  (array_expression)
  (tuple_expression)
  (token_tree)
  (macro_definition)
  (closure_expression)
  (where_clause)
  (raw_string_literal)
  (string_literal)
  (block_comment)
] @fold
