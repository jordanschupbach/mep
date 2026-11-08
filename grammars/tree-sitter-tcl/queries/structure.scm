; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).

; `proc name args body`. A proc inside a namespace eval nests under it.
(procedure name: (_) @name) @definition.function

; `namespace eval name {...}`.
(namespace
  (word_list . (simple_word) @_eval . (_) @name)
  (#eq? @_eval "eval")) @definition.module

; TclOO and itcl: `oo::class create Name {...}`, `itcl::class Name {...}`,
; and the methods declared in them.
(command
  name: (simple_word) @_cmd
  arguments: (word_list . (simple_word) @_create . (_) @name)
  (#any-of? @_cmd "oo::class" "::oo::class")
  (#eq? @_create "create")) @definition.class

(command
  name: (simple_word) @_cmd
  arguments: (word_list . (_) @name)
  (#any-of? @_cmd "itcl::class" "::itcl::class" "snit::type" "snit::widget")) @definition.class

(command
  name: (simple_word) @_cmd
  arguments: (word_list . (_) @name)
  (#any-of? @_cmd "method" "constructor" "destructor")) @definition.method

; Top-level variables: `variable name ?value?` and `set NAME ...` in ALL_CAPS.
(command
  name: (simple_word) @_cmd
  arguments: (word_list . (simple_word) @name)
  (#eq? @_cmd "variable")) @definition.variable

(source_file
  (set (id) @name) @definition.variable
  (#match? @name "^[A-Z][A-Z0-9_]*$"))
