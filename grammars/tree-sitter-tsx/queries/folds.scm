; What .tsx folds on top of TypeScript's folds
; (grammars/tree-sitter-typescript/queries/folds.scm, which src/
; treesitter.cpp puts first): JSX elements and expressions spanning rows.
[
  (jsx_element)
  (jsx_self_closing_element)
  (jsx_expression)
] @fold
