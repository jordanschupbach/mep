; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

(namespace_definition name: (_) @name) @definition.namespace

; Named classes, structs, unions and enums with a body (templates and
; specializations included: the name is then a template_type), and a
; typedef of an anonymous one by its typedef name.
(class_specifier name: (_) @name body: (_)) @definition.class
(struct_specifier name: (_) @name body: (_)) @definition.struct
(union_specifier name: (_) @name body: (_)) @definition.union
(enum_specifier name: (_) @name body: (_)) @definition.enum
(type_definition type: (struct_specifier !name) declarator: (type_identifier) @name) @definition.struct
(type_definition type: (union_specifier !name) declarator: (type_identifier) @name) @definition.union
(type_definition type: (enum_specifier !name) declarator: (type_identifier) @name) @definition.enum

; `using Alias = ...;` and concepts.
(alias_declaration name: (type_identifier) @name) @definition.type
(concept_definition name: (identifier) @name) @definition.type

; Function definitions: free functions by name; members (defined in the
; class, or out of it as `C::f`), constructors, destructors and operators
; as methods -- whatever `*` or `&` the return type has.
(function_definition type: (_) declarator: (function_declarator declarator: (identifier) @name)) @definition.function
; No return type: a constructor defined in its class.
(function_definition !type declarator: (function_declarator declarator: (identifier) @name)) @definition.method
(function_definition declarator: (function_declarator declarator: (template_function) @name)) @definition.function
(function_definition
  declarator: (function_declarator
    declarator: [(field_identifier) (qualified_identifier) (destructor_name) (operator_name)] @name)) @definition.method
(function_definition
  declarator: [
    (pointer_declarator declarator: (function_declarator declarator: (identifier) @name))
    (reference_declarator (function_declarator declarator: (identifier) @name))
  ]) @definition.function
(function_definition
  declarator: [
    (pointer_declarator
      declarator: (function_declarator declarator: [(field_identifier) (qualified_identifier) (operator_name)] @name))
    (reference_declarator
      (function_declarator declarator: [(field_identifier) (qualified_identifier) (operator_name)] @name))
  ]) @definition.method
(function_definition
  declarator: (pointer_declarator
    declarator: (pointer_declarator declarator: (function_declarator declarator: (identifier) @name)))) @definition.function

; Member functions declared in a class body: `void f();`, `virtual ~C();`.
(field_declaration
  declarator: (function_declarator declarator: [(field_identifier) (operator_name)] @name)) @definition.method
(field_declaration_list
  (declaration declarator: (function_declarator declarator: [(identifier) (destructor_name)] @name)) @definition.method)
