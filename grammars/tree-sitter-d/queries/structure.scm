; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

(module_declaration (module_fqn) @name) @definition.module

(struct_declaration (struct) . (identifier) @name) @definition.struct
(union_declaration (union) . (identifier) @name) @definition.struct
(class_declaration (class) . (identifier) @name) @definition.class
(interface_declaration (interface) . (identifier) @name) @definition.interface
(enum_declaration (enum) . (identifier) @name) @definition.enum
(enum_declaration (enum_member . (identifier) @name) @definition.enummember)

(template_declaration (template) . (identifier) @name) @definition.template
(mixin_template_declaration (template_declaration (template) . (identifier) @name)) @definition.template

(function_declaration (identifier) @name) @definition.function
(constructor (this) @name) @definition.constructor
(destructor (this) @name) @definition.constructor
(postblit (this) @name) @definition.constructor
(unittest_declaration (unittest) @name) @definition.function

; Fields, and module-level constants, variables and aliases (one per name).
(aggregate_body (variable_declaration (declarator . (identifier) @name) @definition.field))
(source_file (variable_declaration (declarator . (identifier) @name) @definition.variable))
(module_def (variable_declaration (declarator . (identifier) @name) @definition.variable))
(manifest_declarator . (identifier) @name) @definition.constant
(alias_declaration (alias_initializer . (identifier) @name) @definition.type)
