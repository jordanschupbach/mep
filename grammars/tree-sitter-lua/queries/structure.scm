; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; `function f()`, `local function f()`, `function M.f()`, `function M:f()`
(function_declaration name: (identifier) @name) @definition.function
(function_declaration name: (dot_index_expression) @name) @definition.function
(function_declaration name: (method_index_expression) @name) @definition.method

; `f = function() end`, `M.f = function() end`, and `local f = function()
; end` (whose variable_declaration holds the same assignment_statement)
(assignment_statement
  (variable_list . name: [(identifier) (dot_index_expression)] @name)
  (expression_list . value: (function_definition))) @definition.function

; `{ f = function() end }`: a module table's or class's methods.
(field
  name: (identifier) @name
  value: (function_definition)) @definition.method
