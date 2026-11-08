; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps). The
; TSX grammar uses it too: JSX adds element syntax, not declarations.

(interface_declaration name: (type_identifier) @name) @definition.interface
(type_alias_declaration name: (type_identifier) @name) @definition.type
(enum_declaration name: (identifier) @name) @definition.enum
(class_declaration name: (type_identifier) @name) @definition.class
(abstract_class_declaration name: (type_identifier) @name) @definition.class
(internal_module name: [(identifier) (nested_identifier) (string)] @name) @definition.namespace
(module name: [(identifier) (nested_identifier) (string)] @name) @definition.namespace

(function_declaration name: (identifier) @name) @definition.function
(generator_function_declaration name: (identifier) @name) @definition.function
(function_signature name: (identifier) @name) @definition.function

; Methods, getters and setters (`#private` ones included), signatures in
; interfaces and abstract classes, and fields holding a function.
(method_definition name: [(property_identifier) (private_property_identifier)] @name) @definition.method
(method_signature name: [(property_identifier) (private_property_identifier)] @name) @definition.method
(abstract_method_signature name: [(property_identifier) (private_property_identifier)] @name) @definition.method
(public_field_definition
  name: [(property_identifier) (private_property_identifier)] @name
  value: [(arrow_function) (function_expression)]) @definition.method

; `const f = () => {}`, `let g = function () {}`, `const C = class {}`.
(variable_declarator
  name: (identifier) @name
  value: [(arrow_function) (function_expression) (generator_function)]) @definition.function
(variable_declarator name: (identifier) @name value: (class)) @definition.class
