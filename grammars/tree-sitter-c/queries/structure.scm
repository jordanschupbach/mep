; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; Named structs, unions and enums, defined (with a body) on their own or in
; a typedef; and a typedef of an anonymous one, by its typedef name.
(struct_specifier name: (type_identifier) @name body: (_)) @definition.struct
(union_specifier name: (type_identifier) @name body: (_)) @definition.union
(enum_specifier name: (type_identifier) @name body: (_)) @definition.enum
(type_definition type: (struct_specifier !name) declarator: (type_identifier) @name) @definition.struct
(type_definition type: (union_specifier !name) declarator: (type_identifier) @name) @definition.union
(type_definition type: (enum_specifier !name) declarator: (type_identifier) @name) @definition.enum

; Function definitions, however many `*` the return type has.
(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function
(function_definition
  declarator: (pointer_declarator declarator: (function_declarator declarator: (identifier) @name))) @definition.function
(function_definition
  declarator: (pointer_declarator
    declarator: (pointer_declarator declarator: (function_declarator declarator: (identifier) @name)))) @definition.function
