; Fold ranges for mep's Scheme grammar. Every multi-line form is worth
; collapsing -- a Scheme file has no other structure -- and mep only offers
; a fold for a node that spans more than one line.
[
  (list)
  (vector)
  (byte_vector)
  (string)
  (block_comment)
] @fold
