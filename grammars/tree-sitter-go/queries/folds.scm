; Fold ranges for mep's Go grammar: blocks, declarations and literals that
; span lines (function bodies, grouped imports/consts/vars/types, struct
; and interface types, composite literals, switch/select bodies, raw
; strings, runs of comments are folded by the editor itself).
[
  (block)
  (import_spec_list)
  (const_declaration)
  (var_declaration)
  (type_declaration)
  (field_declaration_list)
  (interface_type)
  (literal_value)
  (expression_switch_statement)
  (type_switch_statement)
  (select_statement)
  (communication_case)
  (expression_case)
  (type_case)
  (default_case)
  (argument_list)
  (parameter_list)
  (raw_string_literal)
  (comment)
] @fold
