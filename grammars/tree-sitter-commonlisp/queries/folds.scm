; Fold ranges for mep's Common Lisp grammar. Every multi-line form is
; worth collapsing -- a Lisp file has no other structure -- and mep only
; offers a fold for a node that spans more than one line.
[
  (list)
  (vector)
  (block_comment)
  (string)
] @fold
