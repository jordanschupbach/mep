; Document outline for mep's structure sidebar (.mli): each pattern tags the
; whole definition @definition.<kind> and the node holding its name @name.

(value_specification (value_name) @name) @definition.function
(external (value_name) @name) @definition.function
(type_binding name: (type_constructor) @name body: (variant_declaration)) @definition.enum
(type_binding name: (type_constructor) @name body: (record_declaration)) @definition.struct
(type_binding name: (type_constructor) @name) @definition.type
(variant_declaration (constructor_declaration (constructor_name) @name) @definition.enummember)
(field_declaration (field_name) @name) @definition.field
(exception_definition (constructor_declaration (constructor_name) @name)) @definition.class
(module_binding (module_name) @name) @definition.module
(module_type_definition (module_type_name) @name) @definition.interface
(class_binding (class_name) @name) @definition.class
(class_type_binding (class_type_name) @name) @definition.interface
(method_specification (method_name) @name) @definition.method
