; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

(class_declaration name: (identifier) @name) @definition.class
(function_declaration name: (identifier) @name) @definition.function
(generator_function_declaration name: (identifier) @name) @definition.function

; Methods, getters and setters, `#private` ones included; field arrow
; functions (`onClick = () => {...}`) are methods too.
(method_definition name: [(property_identifier) (private_property_identifier)] @name) @definition.method
(field_definition
  property: [(property_identifier) (private_property_identifier)] @name
  value: [(arrow_function) (function_expression)]) @definition.method

; `const f = () => {}`, `let g = function () {}`, `const C = class {}`.
(variable_declarator
  name: (identifier) @name
  value: [(arrow_function) (function_expression) (generator_function)]) @definition.function
(variable_declarator name: (identifier) @name value: (class)) @definition.class

; `module.exports.f = function () {}`, `Foo.prototype.bar = () => {}`.
(expression_statement
  (assignment_expression
    left: (member_expression property: (property_identifier) @name)
    right: [(arrow_function) (function_expression)])) @definition.function

; Functions in an object literal: `{ f() {}, g: () => {} }` (methods are
; covered above).
(pair
  key: (property_identifier) @name
  value: [(arrow_function) (function_expression)]) @definition.method

; Top-level ALL_CAPS constants, `const MAX_SIZE = 10` -- two characters at
; least, so a one-letter class or component (`const C = class {}`) stays
; what it is.
(program
  (lexical_declaration
    (variable_declarator name: (identifier) @name) @definition.variable)
  (#match? @name "^[A-Z][A-Z0-9_]+$"))
(program
  (export_statement
    (lexical_declaration
      (variable_declarator name: (identifier) @name) @definition.variable))
  (#match? @name "^[A-Z][A-Z0-9_]+$"))
