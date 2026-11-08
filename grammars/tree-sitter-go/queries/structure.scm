; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps). A type
; declaration matches both its specific pattern (struct, interface) and the
; generic `type` one; TreesitterStructure keeps the specific kind.

(type_declaration (type_spec name: (type_identifier) @name type: (struct_type))) @definition.struct
(type_declaration (type_spec name: (type_identifier) @name type: (interface_type))) @definition.interface
(type_declaration (type_spec name: (type_identifier) @name)) @definition.type
(type_declaration (type_alias name: (type_identifier) @name)) @definition.type
(function_declaration name: (identifier) @name) @definition.function
(method_declaration name: (field_identifier) @name) @definition.method

; Methods declared in an interface, nested under it.
(method_elem name: (field_identifier) @name) @definition.method

; Package-level constants and variables (one entry per name).
(source_file (const_declaration (const_spec name: (identifier) @name) @definition.constant))
(source_file (var_declaration (var_spec name: (identifier) @name) @definition.variable))
