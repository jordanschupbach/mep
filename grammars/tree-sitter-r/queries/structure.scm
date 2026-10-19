; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; An assignment of a function (`f <- function(x)`, `f = \(x)`, `f <<- ...`,
; `obj$f <- ...`); a function given as a named argument -- an
; R6Class/setRefClass `public = list(greet = function() ...)` method;
; R6/reference classes by the name they're assigned to; and the S4 shapes
; by their string name: setClass/setRefClass classes, setGeneric generics,
; setMethod methods. Each call pattern anchors on the first argument (`.`)
; so a later string argument (setMethod's signature) is never the name.

(binary_operator
  lhs: [(identifier) (string) (extract_operator)] @name
  rhs: (function_definition)) @definition.function

(argument
  name: (identifier) @name
  value: (function_definition)) @definition.method

(binary_operator
  lhs: (identifier) @name
  rhs: (call function: (identifier) @_class)
  (#any-of? @_class "R6Class" "setRefClass")) @definition.class

(call
  function: (identifier) @_class
  arguments: (arguments . (argument value: (string content: (string_content) @name)))
  (#any-of? @_class "setClass" "setRefClass")) @definition.class

(call
  function: (identifier) @_generic
  arguments: (arguments . (argument value: (string content: (string_content) @name)))
  (#eq? @_generic "setGeneric")) @definition.function

(call
  function: (identifier) @_method
  arguments: (arguments . (argument value: (string content: (string_content) @name)))
  (#eq? @_method "setMethod")) @definition.method
