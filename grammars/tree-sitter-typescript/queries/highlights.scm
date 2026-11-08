; What TypeScript adds to JavaScript's highlights (nvim-treesitter naming).
; mep layers it over grammars/tree-sitter-javascript/queries/highlights.scm
; (all of it but its "JavaScript only" section, and its "JSX" section only
; for .tsx: src/treesitter.cpp's LayeredTypescriptHighlights), so the
; patterns here come later and win where both match. The TSX grammar uses
; this query too (grammars/tree-sitter-tsx/queries/highlights.scm).

; Types

(type_identifier) @type
(predefined_type) @type.builtin

((identifier) @type
 (#match? @type "^[A-Z]"))

(type_arguments
  "<" @punctuation.bracket
  ">" @punctuation.bracket)

; Variables

(required_parameter (identifier) @variable.parameter)
(optional_parameter (identifier) @variable.parameter)

; Keywords

[ "abstract"
  "declare"
  "enum"
  "export"
  "implements"
  "interface"
  "keyof"
  "namespace"
  "private"
  "protected"
  "public"
  "type"
  "readonly"
  "override"
  "satisfies"
] @keyword
