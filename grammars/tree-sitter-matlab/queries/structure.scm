; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; `classdef Name < Base ... end`.
(class_definition name: (identifier) @name) @definition.class

; `function [out] = name (args) ... end`, at top level, nested in another
; function, or in a classdef's methods block.
(function_definition name: (_) @name) @definition.function

; A classdef's `properties` block: each property.
(properties (property name: (_) @name) @definition.field)

; `events` and `enumeration` members.
(events (identifier) @name @definition.constant)
(enumeration (enum . (identifier) @name) @definition.constant)

; A function inside a classdef's `methods` block is a method.
(methods (function_definition name: (_) @name) @definition.method)
