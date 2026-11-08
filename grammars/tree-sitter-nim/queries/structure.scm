; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps). The
; name is the identifier without its export marker. A type declaration
; matches its specific pattern (object, enum, concept) and the generic one;
; TreesitterStructure keeps the specific kind.

(proc_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.function
(func_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.function
(iterator_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.function
(converter_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.function
(method_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.method
(template_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.macro
(macro_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.macro

(type_declaration (type_symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) [(object_declaration) (ref_type (object_declaration)) (pointer_type (object_declaration))]) @definition.struct
(type_declaration (type_symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) (enum_declaration)) @definition.enum
(type_declaration (type_symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) (concept_declaration)) @definition.interface
(type_declaration (type_symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)])) @definition.type

; Object fields and enum members, nested under their type.
(field_declaration (symbol_declaration_list (symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.field))
(enum_field_declaration (symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.enummember)

; Module-level constants and variables (one entry per name).
(source_file (const_section (variable_declaration (symbol_declaration_list (symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.constant))))
(source_file (let_section (variable_declaration (symbol_declaration_list (symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.variable))))
(source_file (var_section (variable_declaration (symbol_declaration_list (symbol_declaration name: [(identifier) @name (accent_quoted) @name (exported_symbol (identifier) @name) (exported_symbol (accent_quoted) @name)]) @definition.variable))))
