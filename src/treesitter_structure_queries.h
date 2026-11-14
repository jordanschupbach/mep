#pragma once

// Document-structure queries for the sidebar, for the dynamically-loaded
// languages (treesitter.cpp's DynamicStructureQueryTable); the compiled-in
// grammars' are their own queries/structure.scm, embedded via
// <name>_queries.h. Read by main.cpp's kBuiltinStructure
// (mep.ts_structure): one query per language, each pattern tagging the
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
// (a plain `type` pattern that also matches a struct/interface type's own
// declaration, as in grammars/tree-sitter-go/queries/structure.scm) is intentional, not a bug -- TreesitterStructure
// dedups by exact byte span and keeps the more specific (non-"type") kind
// when two patterns land on the identical node.

static const char *kStructurePhp = R"TSQ(
(namespace_definition name: (_) @name) @definition.namespace
(interface_declaration name: (name) @name) @definition.interface
(trait_declaration name: (name) @name) @definition.trait
(enum_declaration name: (name) @name) @definition.enum
(class_declaration name: (name) @name) @definition.class
(method_declaration name: (name) @name) @definition.method
(function_definition name: (name) @name) @definition.function
)TSQ";
