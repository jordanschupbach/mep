; The structure sidebar's outline for mep's Java grammar: types (classes,
; interfaces, enums, records, annotation types), their constructors and
; methods, enum constants and fields.
(interface_declaration name: (identifier) @name) @definition.interface
(class_declaration name: (identifier) @name) @definition.class
(enum_declaration name: (identifier) @name) @definition.enum
(record_declaration name: (identifier) @name) @definition.class
(annotation_type_declaration name: (identifier) @name) @definition.interface
(constructor_declaration name: (identifier) @name) @definition.method
(compact_constructor_declaration name: (identifier) @name) @definition.method
(method_declaration name: (identifier) @name) @definition.method
(enum_constant name: (identifier) @name) @definition.enummember
(field_declaration declarator: (variable_declarator name: (identifier) @name)) @definition.field
