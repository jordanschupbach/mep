; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; Namespaces nest their types, and types their members and nested types. A
; generic type is listed by its name, without the parameters.

(namespace_declaration name: (_) @name) @definition.namespace
(file_scoped_namespace_declaration name: (_) @name) @definition.namespace

(class_declaration name: (identifier) @name) @definition.class
(record_declaration name: (identifier) @name) @definition.class
(struct_declaration name: (identifier) @name) @definition.struct
(interface_declaration name: (identifier) @name) @definition.interface
(enum_declaration name: (identifier) @name) @definition.enum
(delegate_declaration name: (identifier) @name) @definition.type

; Members with code: methods, constructors, the finalizer, operators
; (named by their operator, or a conversion by its target type), and the
; local functions inside them.
(method_declaration name: (identifier) @name) @definition.method
(constructor_declaration name: (identifier) @name) @definition.method
(destructor_declaration name: (identifier) @name) @definition.method
(operator_declaration operator: _ @name) @definition.method
(conversion_operator_declaration type: (_) @name) @definition.method
(local_function_statement name: (identifier) @name) @definition.function

; Data members: properties, indexers (`this`), events, fields (one entry
; per declared name), and an enum's members.
(property_declaration name: (identifier) @name) @definition.variable
(indexer_declaration "this" @name) @definition.variable
(event_declaration name: (identifier) @name) @definition.variable
(event_field_declaration
  (variable_declaration (variable_declarator name: (identifier) @name) @definition.variable))
(field_declaration
  (variable_declaration (variable_declarator name: (identifier) @name) @definition.variable))
(enum_member_declaration name: (identifier) @name) @definition.variable
