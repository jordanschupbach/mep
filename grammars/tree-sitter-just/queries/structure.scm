; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

(recipe
  (recipe_header name: (identifier) @name)) @definition.recipe

(module name: (identifier) @name) @definition.module

(assignment name: (identifier) @name) @definition.variable

(export name: (identifier) @name) @definition.variable
