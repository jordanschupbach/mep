; Highlight captures for mep's Tcl grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (see TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and every narrower
; rule that should override them comes after. Adapted from
; tree-sitter-grammars/tree-sitter-tcl's queries (MIT, Lewis Russell).

; ---- Words and substitutions --------------------------------------------------

(variable_substitution) @variable
(quoted_word) @string
(escaped_character) @string.escape
(number) @number

((simple_word) @number
 (#match? @number "^[+-]?[0-9]+(\\.[0-9]+)?$"))

((simple_word) @boolean
 (#any-of? @boolean "true" "false" "yes" "no" "on" "off"))

; Options: `-nocase`, `-command`.
((simple_word) @variable.parameter
 (#match? @variable.parameter "^-[a-zA-Z][a-zA-Z0-9_-]*$"))

; The variables Tcl itself sets or reads (tclvars(n)).
((simple_word) @variable.builtin
 (#any-of? @variable.builtin
  "argc" "argv" "argv0" "auto_path" "env" "errorCode" "errorInfo" "tcl_interactive" "tcl_library"
  "tcl_nonwordchars" "tcl_patchLevel" "tcl_pkgPath" "tcl_platform" "tcl_precision" "tcl_rcFileName"
  "tcl_traceCompile" "tcl_traceExec" "tcl_wordchars" "tcl_version"))

; ---- Commands -----------------------------------------------------------------

(command name: (simple_word) @function.call)

; Tcl's own commands (Tcl(n)'s list, the ones without a shape of their own).
(command
  name: (simple_word) @function.builtin
  (#any-of? @function.builtin
   "after" "append" "apply" "array" "binary" "cd" "chan" "clock" "close" "concat" "dict" "encoding"
   "eof" "error" "eval" "exec" "exit" "fblocked" "fconfigure" "fcopy" "file" "fileevent" "flush"
   "format" "gets" "glob" "incr" "info" "interp" "join" "lappend" "lassign" "lindex" "linsert"
   "list" "llength" "lmap" "load" "lrange" "lrepeat" "lreplace" "lreverse" "lsearch" "lset"
   "lsort" "open" "package" "pid" "puts" "pwd" "read" "regsub" "rename" "scan" "seek" "socket"
   "source" "split" "string" "subst" "tell" "time" "trace" "unload" "unset" "update" "uplevel"
   "upvar" "variable" "vwait" "zlib" "my" "self" "next" "oo::class" "oo::define" "oo::object"))

; Control flow written as ordinary commands.
(command
  name: (simple_word) @keyword
  (#any-of? @keyword
   "break" "continue" "return" "for" "switch" "tailcall" "throw" "yield" "yieldto" "coroutine"
   "uplevel" "if" "while" "foreach" "catch" "try" "proc" "set" "global" "namespace" "expr"
   "regexp" "else" "elseif"))

; `unset a b`, `variable a b`: the arguments are variable names.
(command
  name: (simple_word) @_cmd
  arguments: (word_list (simple_word) @variable)
  (#any-of? @_cmd "unset" "variable" "upvar" "global"))

; ---- The commands with a shape of their own -------------------------------------

"proc" @keyword.function

(procedure name: (_) @function)

(argument name: (_) @variable.parameter)
(arguments (simple_word) @variable.parameter)

(set (id) @variable)
(global (simple_word) @variable)
(foreach (arguments (simple_word) @variable))

[
 "while"
 "foreach"
] @keyword.repeat

[
 "if"
 "then"
 "else"
 "elseif"
] @keyword.conditional

[
 "catch"
 "global"
 "namespace"
 "on"
 "error"
 "set"
 "try"
 "finally"
 "regexp"
] @keyword

"expr" @function.builtin

; ---- Expressions ----------------------------------------------------------------

; Math functions: `sin($x)`.
(_ (simple_word) @function.call . "(")

[
 "**" "/" "*" "%" "+" "-" "<<" ">>" ">" "<" ">=" "<=" "==" "!=" "eq" "ne" "in" "ni" "&" "^" "|"
 "&&" "||" "?" ":" "!" "~"
] @operator

(unpack) @operator

[
 "[" "]"
] @punctuation.special

[
 "{" "}" "(" ")"
] @punctuation.bracket

";" @punctuation.delimiter

(comment) @comment
