; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

(module_definition name: (identifier) @name) @definition.module

; `function f(x)`, `function Base.show(io, x)`, `function (+)(a, b)`,
; `function ∘ end`, with a return type and `where` clauses.
(function_definition
  (signature
    [
      (identifier) @name
      (operator) @name
      (call_expression . [(identifier) (field_expression) (operator)] @name)
      (typed_expression . (call_expression . [(identifier) (field_expression) (operator)] @name))
      (where_expression . (call_expression . [(identifier) (field_expression) (operator)] @name))
      (where_expression
        . (typed_expression . (call_expression . [(identifier) (field_expression) (operator)] @name)))
      (where_expression
        . (where_expression . (call_expression . [(identifier) (field_expression) (operator)] @name)))
    ])) @definition.function

; The short form `f(x) = ...` (also `f(x)::T = ...` and `f(x) where T = ...`),
; at the top level and in a module -- not the closures inside a function.
(source_file
  (assignment
    .
    [
      (call_expression . [(identifier) (field_expression) (operator)] @name)
      (typed_expression . (call_expression . [(identifier) (field_expression) (operator)] @name))
      (where_expression . (call_expression . [(identifier) (field_expression) (operator)] @name))
      (where_expression
        . (typed_expression . (call_expression . [(identifier) (field_expression) (operator)] @name)))
    ]) @definition.function)
(module_definition
  (block
    (assignment
      .
      [
        (call_expression . [(identifier) (field_expression) (operator)] @name)
        (typed_expression . (call_expression . [(identifier) (field_expression) (operator)] @name))
        (where_expression . (call_expression . [(identifier) (field_expression) (operator)] @name))
        (where_expression
          . (typed_expression . (call_expression . [(identifier) (field_expression) (operator)] @name)))
      ]) @definition.function))

(macro_definition
  (signature
    [
      (identifier) @name
      (call_expression . (identifier) @name)
    ])) @definition.macro

; `struct S`, `mutable struct S{T} <: A`
(struct_definition
  (type_head
    [
      (identifier) @name
      (parametrized_type_expression . (identifier) @name)
      (binary_expression . [(identifier) @name (parametrized_type_expression . (identifier) @name)])
    ])) @definition.struct

; `abstract type A{T} <: B end`, `primitive type U 8 end`
(abstract_definition
  (type_head
    [
      (identifier) @name
      (parametrized_type_expression . (identifier) @name)
      (binary_expression . [(identifier) @name (parametrized_type_expression . (identifier) @name)])
    ])) @definition.type
(primitive_definition
  (type_head
    [
      (identifier) @name
      (parametrized_type_expression . (identifier) @name)
      (binary_expression . [(identifier) @name (parametrized_type_expression . (identifier) @name)])
    ])) @definition.type

; `const X = 1`, `const X::Int = 1`
(const_statement
  (assignment . [(identifier) @name (typed_expression . (identifier) @name)])) @definition.constant
