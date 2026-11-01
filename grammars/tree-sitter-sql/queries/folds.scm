; Fold ranges for mep's SQL grammar: every statement (a CREATE TABLE, a long
; query), and inside one its column list, CTEs, subqueries, CASE
; expressions, function bodies and blocks. A one-line node makes no fold.
[
  (statement)
  (transaction)
  (block)
  (column_definitions)
  (cte)
  (subquery)
  (case)
  (function_body)
] @fold
