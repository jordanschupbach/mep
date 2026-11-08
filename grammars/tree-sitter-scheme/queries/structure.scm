; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; Definitions are lists named by their first symbol, as in highlights.scm.
; The query cannot tell depth, so an internal `define` in a body is listed
; too, nested under its procedure -- which is what it is.

; (define (f x) ...), (define ((curried a) b) ...): the innermost head symbol.
(list . (symbol) @_def
  . (list . (symbol) @name)
  (#any-of? @_def "define" "define-public" "define*" "define*-public" "define-inlinable"
   "define-integrable")) @definition.function

(list . (symbol) @_def
  . (list . (list . (symbol) @name))
  (#any-of? @_def "define" "define-public" "define*" "define*-public")) @definition.function

; (define name (lambda ...)) and (define name value).
(list . (symbol) @_def
  . (symbol) @name
  . (list . (symbol) @_lambda)
  (#any-of? @_def "define" "define-public")
  (#any-of? @_lambda "lambda" "lambda*" "case-lambda" "λ")) @definition.function

(list . (symbol) @_def
  . (symbol) @name
  (#any-of? @_def "define" "define-public" "define-once" "define-constant" "define-parameter"))
  @definition.variable

(list . (symbol) @_def
  . (list . (symbol) @name)
  (#any-of? @_def "define-values")) @definition.variable

; Macros: (define-syntax name ...), (define-syntax-rule (name ...) ...).
(list . (symbol) @_def
  . (symbol) @name
  (#any-of? @_def "define-syntax" "define-macro" "define-syntax-parameter")) @definition.macro

(list . (symbol) @_def
  . (list . (symbol) @name)
  (#any-of? @_def "define-syntax-rule" "define-macro")) @definition.macro

; Records and classes.
(list . (symbol) @_def
  . [(symbol) @name (list . (symbol) @name)]
  (#any-of? @_def "define-record-type" "define-record-type*" "define-class" "define-condition-type"))
  @definition.struct

(list . (symbol) @_def
  . (symbol) @name
  (#any-of? @_def "define-generic" "define-method")) @definition.method

(list . (symbol) @_def
  . (list . (symbol) @name)
  (#eq? @_def "define-method")) @definition.method

; Modules and libraries: (define-module (a b)), (define-library (a b)),
; (library (a b) ...) -- the name list is the @name.
(list . (symbol) @_def
  . (list) @name
  (#any-of? @_def "define-module" "define-library" "library")) @definition.module
