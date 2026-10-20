; Document outline for mep's structure sidebar: each pattern tags the whole
; definition @definition.<kind> (its extent, for nesting and the current
; entry) and the node holding its name @name (where a click jumps).
;
; Definitions are lists named by their operator, as in highlights.scm.
; Only top-level ones and those nested in a top-level form (an
; EVAL-WHEN, a PROGN) are definitions a reader would look for, but the
; query cannot tell depth, so a DEFUN inside a LET is listed too -- which
; is what it is.

(list . (symbol) @_def
  . [(symbol) (qualified_symbol)] @name
  (#any-of? @_def "defun" "defgeneric" "define-compiler-macro")) @definition.function

(list . (symbol) @_def
  . [(symbol) (qualified_symbol)] @name
  (#any-of? @_def "defmacro" "define-modify-macro" "define-symbol-macro"
   "define-setf-expander" "defsetf")) @definition.macro

(list . (symbol) @_def
  . [(symbol) (qualified_symbol) (list)] @name
  (#eq? @_def "defmethod")) @definition.method

; (defun (setf name) ...)
(list . (symbol) @_def
  . (list) @name
  (#any-of? @_def "defun" "defgeneric")) @definition.function

(list . (symbol) @_def
  . (symbol) @name
  (#any-of? @_def "defclass" "define-condition")) @definition.class

(list . (symbol) @_def
  . (symbol) @name
  (#eq? @_def "defstruct")) @definition.struct

; (defstruct (point (:constructor make-pt)) ...)
(list . (symbol) @_def
  . (list . (symbol) @name)
  (#eq? @_def "defstruct")) @definition.struct

(list . (symbol) @_def
  . (symbol) @name
  (#eq? @_def "deftype")) @definition.type

(list . (symbol) @_def
  . (symbol) @name
  (#any-of? @_def "defvar" "defparameter" "defconstant")) @definition.variable

(list . (symbol) @_def
  . [(symbol) (keyword) (uninterned_symbol) (string)] @name
  (#eq? @_def "defpackage")) @definition.module
