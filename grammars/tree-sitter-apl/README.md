# tree-sitter-apl

mep's own [tree-sitter](https://tree-sitter.github.io/) grammar for **APL**:
GNU APL and Dyalog APL source, compiled into mep for `.apl` `.apls` `.aplf`
`.aplo` `.apln` `.aplc` `.apli` `.dyalog` files (highlighting, folds and the
structure sidebar). No usable MIT/BSD tree-sitter-apl existed to vendor, so
it was written for mep (MIT, like mep).

APL can't be parsed into function applications without knowing which names
are functions, so an expression is a flat run of atoms: numbers (`¯2.5E¯3`,
`1J2`), strings (`'it''s'`, GNU's `"..."`), names, `ns.qualified.names`
(`#.a.b`, `##.x`, `⎕SE.x`), `⎕names`, `⍬`, primitive functions and
operators, `→`, parentheses, `[;]` indexing and dfns. The structure an editor
needs is real nodes:

* `assignment` (`x←…`, `x+←1`, `x[i]←…`), `dfn` with `guard` (`:`),
  `error_guard` (`::`), `special_name` (`⍺ ⍵ ⍺⍺ ⍵⍵`, GNU's `λ χ`) and
  `self_reference` (`∇ ∇∇`);
* `tradfn` (`∇ header … ∇`, optionally `∇⍫`) with a `tradfn_header`: result,
  left, name, right fields, `{optional}` arguments, `(LO op RO)` operator
  headers and `local`s (`;a;⎕IO`), and `label`s (`L1:`);
* Dyalog control structures (`if_statement`, `for_statement`,
  `while_statement`, `repeat_statement`, `select_statement`,
  `trap_statement`, `with_statement`, `hold_statement`, with their clauses),
  case-insensitive, closed by their own `:End…` or `:End`; other control
  words (`:Leave`, `:Return`, `:Access`, `:Field`, …) are
  `control_statement`s;
* `namespace` (`:Namespace … :EndNamespace`), `class_definition`
  (`:Class`/`:Interface`) and `property`;
* `system_command` (`)LOAD ws`, `]box on`) and comments (`⍝ …`, a `#!` line).

Statements end at a newline or `⋄`. At the start of a statement outside a
dfn, `∇` opens or closes a tradfn; inside a dfn it is self-reference.

A Dyalog `.aplf` file (a function without `∇`) parses as statements: its
header line is an ordinary expression here; mep-apl-lsp knows better.

Regenerate with `just apl-grammar` (mep-ts-generate, then the corpus in
`test/corpus`).
