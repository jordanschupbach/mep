; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; Types nest their methods, constants and inner types. A generic type's
; name is its constant, without the parameters. `private`/`protected`
; wrap a definition without changing it.

(module_def name: [(constant) @name (generic_type (constant) @name)]) @definition.module
(class_def name: [(constant) @name (generic_type (constant) @name)]) @definition.class
(struct_def name: [(constant) @name (generic_type (constant) @name)]) @definition.struct
(enum_def name: (constant) @name) @definition.enum
(annotation_def name: (constant) @name) @definition.type
(alias name: (constant) @name) @definition.type

; Every `def` is a method (top-level ones are methods of the program),
; `def self.name` too; abstract ones have no body but are part of the API.
(method_def name: (_) @name) @definition.method
(abstract_method_def name: (_) @name) @definition.method

(macro_def name: (_) @name) @definition.macro

; C bindings: a lib is a namespace of funs, structs, unions and types.
(lib_def name: [(constant) @name (generic_type (constant) @name)]) @definition.namespace
(fun_def name: (_) @name) @definition.function
(c_struct_def name: (constant) @name) @definition.struct
(union_def name: (constant) @name) @definition.union
(type_def name: (constant) @name) @definition.type

; Constants: `VERSION = "1.0"`, and an enum's members.
(const_assign lhs: (constant) @name) @definition.variable
(enum_def body: (expressions (constant) @name @definition.variable))
