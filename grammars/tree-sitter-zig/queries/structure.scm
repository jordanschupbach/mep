; Document outline for mep's structure sidebar: each pattern tags the whole
; declaration @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps). A
; container bound to a name (`const Foo = struct { ... };`) is that kind of
; type, and its own declarations and fields nest under it. No two patterns
; match one node: the generic ones exclude what the specific ones take.

(variable_declaration (identifier) @name "=" . (struct_declaration)) @definition.struct
(variable_declaration (identifier) @name "=" . (union_declaration)) @definition.union
(variable_declaration (identifier) @name "=" . (enum_declaration)) @definition.enum
(variable_declaration (identifier) @name "=" . (opaque_declaration)) @definition.type
(variable_declaration (identifier) @name "=" . (error_set_declaration)) @definition.enum

; A generic type is a function returning `type`.
(function_declaration name: (identifier) @name type: (builtin_type) @_t (#eq? @_t "type")) @definition.class
(function_declaration name: (identifier) @name type: (_) @_t (#not-eq? @_t "type")) @definition.function

(test_declaration (string (string_content) @name)) @definition.function
(test_declaration (identifier) @name) @definition.function

; Other constants and variables, at the top level or in a container.
((source_file (variable_declaration (identifier) @name "=" . (_) @_v) @definition.variable)
  (#not-match? @_v "^((extern|packed) +)?((struct|union|enum|opaque)\\b|error *[{])"))
((struct_declaration (variable_declaration (identifier) @name "=" . (_) @_v) @definition.variable)
  (#not-match? @_v "^((extern|packed) +)?((struct|union|enum|opaque)\\b|error *[{])"))
((union_declaration (variable_declaration (identifier) @name "=" . (_) @_v) @definition.variable)
  (#not-match? @_v "^((extern|packed) +)?((struct|union|enum|opaque)\\b|error *[{])"))
((enum_declaration (variable_declaration (identifier) @name "=" . (_) @_v) @definition.variable)
  (#not-match? @_v "^((extern|packed) +)?((struct|union|enum|opaque)\\b|error *[{])"))

; Fields of a struct or union, and enum members.
(struct_declaration (container_field name: (identifier) @name) @definition.field)
(union_declaration (container_field name: (identifier) @name) @definition.field)
(enum_declaration (container_field name: (identifier) @name) @definition.enummember)
