; Document outline for mep's structure sidebar: each pattern tags the whole
; item @definition.<kind> (its extent, for nesting and the current entry)
; and the node holding its name @name (where a click jumps). Items nest
; through their bodies: methods under their impl or trait, items under
; their module.

(mod_item name: (identifier) @name) @definition.namespace
(struct_item name: (type_identifier) @name) @definition.struct
(union_item name: (type_identifier) @name) @definition.struct
(enum_item name: (type_identifier) @name) @definition.enum
(enum_variant name: (identifier) @name) @definition.enummember
(trait_item name: (type_identifier) @name) @definition.interface

; An impl is named by its self type, generic or path-qualified alike.
(impl_item type: (type_identifier) @name) @definition.impl
(impl_item type: (generic_type type: (type_identifier) @name)) @definition.impl
(impl_item type: (scoped_type_identifier name: (type_identifier) @name)) @definition.impl
(impl_item type: (generic_type type: (scoped_type_identifier name: (type_identifier) @name))) @definition.impl

(function_item name: (identifier) @name) @definition.function
(function_signature_item name: (identifier) @name) @definition.function
(type_item name: (type_identifier) @name) @definition.type
(associated_type name: (type_identifier) @name) @definition.type
(const_item name: (identifier) @name) @definition.constant
(static_item name: (identifier) @name) @definition.variable
(macro_definition name: (identifier) @name) @definition.macro
