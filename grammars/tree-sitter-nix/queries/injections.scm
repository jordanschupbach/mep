; A comment naming a language right before an indented string, the nixpkgs
; convention: `/* bash */ ''...''`, `# lua` on the line above.
((comment) @injection.language
  .
  (indented_string_expression) @injection.content
  (#match? @injection.language "^(#|/\\*)\\s*[a-zA-Z0-9_+-]+\\s*(\\*/)?$")
  (#gsub! @injection.language "^(#|/\\*)%s*([%w_+-]+)%s*(%*/)?$" "%2")
  (#set! injection.combined))

; Build phases and the other attributes nixpkgs runs as shell scripts.
(binding
  attrpath: (attrpath attr: (identifier) @_name)
  expression: (indented_string_expression) @injection.content
  (#match? @_name "^(([a-z][A-Za-z]*Phase)|(pre|post)[A-Z][A-Za-z]*|script|shellHook|buildCommand|installCheckPhase|text)$")
  (#set! injection.language "bash")
  (#set! injection.combined))
