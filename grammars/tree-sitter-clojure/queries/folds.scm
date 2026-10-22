; Fold ranges for mep's Clojure grammar. Every multi-line form is worth
; collapsing -- a Clojure file has no other structure -- and mep only
; offers a fold for a node that spans more than one line.
[
  (list_lit)
  (vec_lit)
  (map_lit)
  (set_lit)
  (anon_fn_lit)
  (read_cond_lit)
  (splicing_read_cond_lit)
  (ns_map_lit)
  (str_lit)
] @fold
