; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
; The same file serves .ml (ocaml) and, copied, .mli (ocaml_interface).

; Module-level `let`s only (not the ones inside expressions): functions
; (`let f x = ...`, `let f = fun ...`, `let f = function ...`), then values.
(compilation_unit
  (value_definition (let_binding pattern: (value_name) @name (parameter)) @definition.function))
(compilation_unit
  (value_definition
    (let_binding
      pattern: (value_name) @name
      body: [(fun_expression) (function_expression)]) @definition.function))
(compilation_unit
  (value_definition (let_binding pattern: (value_name) @name . body: (_) @_body) @definition.variable)
  (#not-match? @_body "^(fun|function)[ \t\n(]"))
(compilation_unit
  (value_definition
    (let_binding pattern: (value_name) @name . type: (_) . body: (_) @_body) @definition.variable)
  (#not-match? @_body "^(fun|function)[ \t\n(]"))
(structure
  (value_definition (let_binding pattern: (value_name) @name (parameter)) @definition.function))
(structure
  (value_definition
    (let_binding
      pattern: (value_name) @name
      body: [(fun_expression) (function_expression)]) @definition.function))
(structure
  (value_definition (let_binding pattern: (value_name) @name . body: (_) @_body) @definition.variable)
  (#not-match? @_body "^(fun|function)[ \t\n(]"))
(structure
  (value_definition
    (let_binding pattern: (value_name) @name . type: (_) . body: (_) @_body) @definition.variable)
  (#not-match? @_body "^(fun|function)[ \t\n(]"))

; Interfaces: `val` and `external`.
(value_specification (value_name) @name) @definition.function
(external (value_name) @name) @definition.function

; Types, their constructors and record fields.
(type_binding name: (type_constructor) @name body: (variant_declaration)) @definition.enum
(type_binding name: (type_constructor) @name body: (record_declaration)) @definition.struct
(type_binding name: (type_constructor) @name) @definition.type
(variant_declaration (constructor_declaration (constructor_name) @name) @definition.enummember)
(field_declaration (field_name) @name) @definition.field
(exception_definition (constructor_declaration (constructor_name) @name)) @definition.class

; Modules, module types, classes and methods.
(module_binding (module_name) @name) @definition.module
(module_type_definition (module_type_name) @name) @definition.interface
(class_binding (class_name) @name) @definition.class
(class_type_binding (class_type_name) @name) @definition.interface
(method_definition (method_name) @name) @definition.method
(method_specification (method_name) @name) @definition.method
