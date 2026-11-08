; Fold ranges for mep's Tcl grammar: every braced word that spans lines
; (proc bodies, if/while/foreach bodies, namespace eval blocks, data), the
; commands that carry one, and multi-line command substitutions.
[
  (braced_word)
  (braced_word_simple)
  (command_substitution)
  (quoted_word)
] @fold
