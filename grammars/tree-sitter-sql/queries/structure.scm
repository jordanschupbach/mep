; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; What a schema file defines: tables, views, indexes, triggers, functions,
; types, sequences, schemas and databases; and a query's named pieces, its
; CTEs. The kinds reuse the sidebar's vocabulary (kBuiltinStructure in
; main.cpp): a table is a struct, a view a class, a CTE a variable.
;
; A statement can hold several object references (a trigger names its
; table and function too), so the name is the one right after the keyword
; that says what is being created.

(create_table
  [(keyword_table) (keyword_exists)] . (object_reference) @name) @definition.struct

(create_view
  [(keyword_view) (keyword_exists)] . (object_reference) @name) @definition.class

(create_materialized_view
  [(keyword_view) (keyword_exists)] . (object_reference) @name) @definition.class

(create_index column: (identifier) @name) @definition.variable

(create_trigger
  [(keyword_trigger) (keyword_exists)] . (object_reference) @name) @definition.method

(create_function
  (keyword_function) . (object_reference) @name) @definition.function

(create_type
  (keyword_type) . (object_reference) @name) @definition.type

(create_sequence
  [(keyword_sequence) (keyword_exists)] . (object_reference) @name) @definition.variable

(create_schema (identifier) @name) @definition.namespace

(create_database (identifier) @name) @definition.namespace

(cte . (identifier) @name) @definition.variable
