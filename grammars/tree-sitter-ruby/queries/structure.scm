; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

(module name: [(constant) (scope_resolution)] @name) @definition.namespace
(class name: [(constant) (scope_resolution)] @name) @definition.class
(singleton_class value: (_) @name) @definition.class
(method name: (_) @name) @definition.method
(singleton_method name: (_) @name) @definition.method

; Constants assigned at module/class level, `MAX = 10`.
(assignment left: (constant) @name) @definition.constant
