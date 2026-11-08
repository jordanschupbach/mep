; Fold ranges for mep's Java grammar: type bodies, method and constructor
; bodies, blocks, switch bodies, array initializers, multi-line argument
; lists, import runs are folded by the editor's comment/paren logic, and
; block comments and text blocks span lines.
[
  (class_body)
  (interface_body)
  (enum_body)
  (annotation_type_body)
  (record_declaration body: (_) @fold)
  (constructor_body)
  (block)
  (switch_block)
  (array_initializer)
  (element_value_array_initializer)
  (argument_list)
  (formal_parameters)
  (module_body)
  (block_comment)
] @fold
