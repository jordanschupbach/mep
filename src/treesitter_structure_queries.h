#pragma once

// Document-structure queries for the sidebar (main.cpp's kBuiltinStructure /
// mep.ts_structure): one query per language, each pattern tagging the
// *whole* definition node `@definition.<kind>` (used for the entry's
// end-row, so containment/nesting can be computed generically in
// TreesitterStructure) and the identifier node actually holding the
// display name `@name` (used for the entry's own row/col -- clicking it
// jumps straight to the name, not the top of a possibly multi-line
// signature). Same single-pattern-per-shape convention real tags.scm
// files use upstream, hand-verified against each grammar (via a
// standalone probe binary linked straight against libtree_sitter, not
// guessed) rather than copied from an unseen source -- every language
// below was actually run against a small fixture exercising its own
// declaration shapes (free functions, methods, nested types, pointer/
// qualified returns, decorators/exports, ...) before being kept here.
//
// A definition node deliberately captured by more than one pattern here
// (Go's plain `type_declaration` pattern also matches a struct/interface
// type's own declaration) is intentional, not a bug -- TreesitterStructure
// dedups by exact byte span and keeps the more specific (non-"type") kind
// when two patterns land on the identical node.

static const char *kStructureC = R"TSQ(
(struct_specifier name: (type_identifier) @name body: (_)) @definition.struct
(enum_specifier name: (type_identifier) @name body: (_)) @definition.enum
(union_specifier name: (type_identifier) @name body: (_)) @definition.union
(type_definition type: (struct_specifier) declarator: (type_identifier) @name) @definition.struct
(type_definition type: (enum_specifier) declarator: (type_identifier) @name) @definition.enum
(type_definition type: (union_specifier) declarator: (type_identifier) @name) @definition.union
(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function
(function_definition declarator: (pointer_declarator declarator: (function_declarator declarator: (identifier) @name))) @definition.function
)TSQ";

static const char *kStructureCpp = R"TSQ(
(namespace_definition name: (_) @name) @definition.namespace
(enum_specifier name: (type_identifier) @name body: (_)) @definition.enum
(struct_specifier name: (type_identifier) @name body: (_)) @definition.struct
(class_specifier name: (type_identifier) @name body: (_)) @definition.class
(union_specifier name: (type_identifier) @name body: (_)) @definition.union
(field_declaration declarator: (function_declarator declarator: (field_identifier) @name)) @definition.method
(declaration declarator: (function_declarator declarator: (identifier) @name)) @definition.method
(declaration declarator: (function_declarator declarator: (destructor_name) @name)) @definition.method
(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function
(function_definition declarator: (function_declarator declarator: (qualified_identifier) @name)) @definition.method
(function_definition declarator: (function_declarator declarator: (destructor_name) @name)) @definition.method
(function_definition declarator: (function_declarator declarator: (field_identifier) @name)) @definition.method
(function_definition declarator: (pointer_declarator declarator: (function_declarator declarator: (identifier) @name))) @definition.function
(function_definition declarator: (pointer_declarator declarator: (function_declarator declarator: (qualified_identifier) @name))) @definition.method
(function_definition declarator: (reference_declarator (function_declarator declarator: (identifier) @name))) @definition.function
(function_definition declarator: (reference_declarator (function_declarator declarator: (qualified_identifier) @name))) @definition.method
)TSQ";

static const char *kStructureLua = R"TSQ(
(function_declaration name: (identifier) @name) @definition.function
(function_declaration name: (dot_index_expression) @name) @definition.function
(function_declaration name: (method_index_expression) @name) @definition.method
(variable_declaration
  (assignment_statement
    (variable_list name: (identifier) @name)
    (expression_list value: (function_definition)))) @definition.function
)TSQ";

static const char *kStructurePython = R"TSQ(
(class_definition name: (identifier) @name) @definition.class
(function_definition name: (identifier) @name) @definition.function
)TSQ";

static const char *kStructureJavascript = R"TSQ(
(class_declaration name: (identifier) @name) @definition.class
(function_declaration name: (identifier) @name) @definition.function
(generator_function_declaration name: (identifier) @name) @definition.function
(method_definition name: (property_identifier) @name) @definition.method
(variable_declarator name: (identifier) @name value: (arrow_function)) @definition.function
(variable_declarator name: (identifier) @name value: (function_expression)) @definition.function
)TSQ";

// Typescript/tsx share this: JSX adds element syntax, not new declaration
// node shapes, so the same query covers both (see treesitter.cpp's
// DynamicStructureQueryTable, which keys both filetypes to this string).
static const char *kStructureTypescript = R"TSQ(
(interface_declaration name: (type_identifier) @name) @definition.interface
(type_alias_declaration name: (type_identifier) @name) @definition.type
(enum_declaration name: (identifier) @name) @definition.enum
(class_declaration name: (type_identifier) @name) @definition.class
(abstract_class_declaration name: (type_identifier) @name) @definition.class
(method_definition name: (property_identifier) @name) @definition.method
(method_signature name: (property_identifier) @name) @definition.method
(abstract_method_signature name: (property_identifier) @name) @definition.method
(function_declaration name: (identifier) @name) @definition.function
(variable_declarator name: (identifier) @name value: (arrow_function)) @definition.function
(variable_declarator name: (identifier) @name value: (function_expression)) @definition.function
(internal_module name: (identifier) @name) @definition.namespace
)TSQ";

static const char *kStructureGo = R"TSQ(
(type_declaration (type_spec name: (type_identifier) @name type: (struct_type))) @definition.struct
(type_declaration (type_spec name: (type_identifier) @name type: (interface_type))) @definition.interface
(type_declaration (type_spec name: (type_identifier) @name)) @definition.type
(function_declaration name: (identifier) @name) @definition.function
(method_declaration name: (field_identifier) @name) @definition.method
)TSQ";

static const char *kStructureRust = R"TSQ(
(struct_item name: (type_identifier) @name) @definition.struct
(enum_item name: (type_identifier) @name) @definition.enum
(trait_item name: (type_identifier) @name) @definition.interface
(impl_item type: (type_identifier) @name) @definition.impl
(function_item name: (identifier) @name) @definition.function
(mod_item name: (identifier) @name) @definition.namespace
(type_item name: (type_identifier) @name) @definition.type
)TSQ";

static const char *kStructureJava = R"TSQ(
(interface_declaration name: (identifier) @name) @definition.interface
(class_declaration name: (identifier) @name) @definition.class
(enum_declaration name: (identifier) @name) @definition.enum
(record_declaration name: (identifier) @name) @definition.class
(constructor_declaration name: (identifier) @name) @definition.method
(method_declaration name: (identifier) @name) @definition.method
)TSQ";

static const char *kStructureRuby = R"TSQ(
(module name: (constant) @name) @definition.namespace
(class name: (constant) @name) @definition.class
(method name: (identifier) @name) @definition.method
(singleton_method name: (identifier) @name) @definition.method
)TSQ";

static const char *kStructureCSharp = R"TSQ(
(namespace_declaration name: (_) @name) @definition.namespace
(interface_declaration name: (identifier) @name) @definition.interface
(class_declaration name: (identifier) @name) @definition.class
(struct_declaration name: (identifier) @name) @definition.struct
(enum_declaration name: (identifier) @name) @definition.enum
(constructor_declaration name: (identifier) @name) @definition.method
(method_declaration name: (identifier) @name) @definition.method
)TSQ";

static const char *kStructurePhp = R"TSQ(
(namespace_definition name: (_) @name) @definition.namespace
(interface_declaration name: (name) @name) @definition.interface
(trait_declaration name: (name) @name) @definition.trait
(enum_declaration name: (name) @name) @definition.enum
(class_declaration name: (name) @name) @definition.class
(method_declaration name: (name) @name) @definition.method
(function_definition name: (name) @name) @definition.function
)TSQ";
