; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; Program units.
(program (program_statement (name) @name)) @definition.module
(module (module_statement (name) @name)) @definition.module
(submodule (submodule_statement (name) @name)) @definition.module

; Procedures, also the ones inside CONTAINS and interface bodies.
(subroutine (subroutine_statement name: (_) @name)) @definition.function
(function (function_statement name: (_) @name)) @definition.function
(module_procedure (module_procedure_statement name: (_) @name)) @definition.function

; Derived types and named interfaces.
(derived_type_definition (derived_type_statement (type_name) @name)) @definition.class
(interface (interface_statement (name) @name)) @definition.interface
