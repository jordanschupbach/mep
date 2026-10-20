; Highlight captures for mep's Common Lisp grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (see TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and every narrower
; rule that should override them comes after.
;
; The grammar is the reader's: a `defun` is a list whose first symbol is
; DEFUN. Everything below that knows what a form means does so by the
; operator's name. Names are matched in lowercase, how nearly all Common
; Lisp is written today; code in upper case still reads correctly, it just
; gets the plain symbol colours.

; Atoms

(symbol) @variable

(comment) @comment
(block_comment) @comment
(shebang) @preproc

(string) @string
(escape_sequence) @string.escape
(format_directive) @string.special
(pathname) @string.special.path

(number) @number
(complex) @number
(character) @character
(bit_vector) @constant

(keyword) @constant
(uninterned_symbol) @constant
(package) @module

; *special-variables* by the earmuffs convention, +constants+ by theirs.
((symbol) @variable.builtin
 (#match? @variable.builtin "^\\*.+\\*$"))
((symbol) @constant
 (#match? @constant "^\\+.+\\+$"))

; Punctuation and reader macros

["(" ")"] @punctuation.bracket
(vector ")" @punctuation.bracket)
(dot) @punctuation.delimiter

(quote "'" @punctuation.special)
(quasiquote "`" @punctuation.special)
(unquote "," @punctuation.special)
(unquote_splicing [",@" ",."] @punctuation.special)
(function "#'" @punctuation.special)
(read_eval "#." @preproc)
(label) @label
(label_reference) @label

; Calls: the operator position of any list.

(list . (symbol) @function.call)
(list . (qualified_symbol name: (symbol) @function.call))

; #'name: a function designator.
(function value: (symbol) @function)
(function value: (qualified_symbol name: (symbol) @function))

; The standard's functions people call most -- not all 750 of them: a
; long #any-of? list is compared on every list head, every pass.
((list . (symbol) @function.builtin)
 (#any-of? @function.builtin
  "car" "cdr" "cons" "list" "list*" "append" "nconc" "reverse" "nreverse" "length"
  "first" "second" "third" "rest" "last" "butlast" "nth" "nthcdr" "elt" "aref"
  "svref" "gethash" "remhash" "make-hash-table" "maphash" "clrhash"
  "mapcar" "mapc" "mapcan" "maplist" "mapl" "map" "reduce" "remove" "remove-if"
  "remove-if-not" "delete" "delete-if" "find" "find-if" "position" "position-if"
  "count" "count-if" "member" "assoc" "rassoc" "getf" "subseq" "sort" "stable-sort"
  "copy-list" "copy-seq" "concatenate" "coerce" "make-array" "make-list" "vector"
  "vector-push" "vector-push-extend" "fill" "replace" "search" "mismatch"
  "funcall" "apply" "values" "values-list" "identity" "constantly" "complement"
  "eq" "eql" "equal" "equalp" "not" "null" "atom" "consp" "listp" "symbolp"
  "stringp" "numberp" "integerp" "floatp" "functionp" "characterp" "vectorp"
  "arrayp" "hash-table-p" "keywordp" "zerop" "plusp" "minusp" "evenp" "oddp"
  "typep" "subtypep" "type-of" "class-of" "find-class" "make-instance"
  "slot-value" "slot-boundp" "change-class" "initialize-instance" "print-object"
  "+" "-" "*" "/" "=" "/=" "<" ">" "<=" ">=" "1+" "1-" "min" "max" "abs" "mod"
  "rem" "floor" "ceiling" "truncate" "round" "sqrt" "expt" "exp" "log" "random"
  "format" "print" "prin1" "princ" "terpri" "fresh-line" "write" "write-string"
  "write-line" "write-char" "read" "read-line" "read-char" "read-from-string"
  "prin1-to-string" "princ-to-string" "open" "close" "finish-output" "force-output"
  "string" "string=" "string-equal" "string<" "string-upcase" "string-downcase"
  "string-trim" "char" "schar" "char=" "char-code" "code-char" "parse-integer"
  "intern" "find-symbol" "symbol-name" "symbol-value" "symbol-function" "gensym"
  "find-package" "make-symbol" "fboundp" "boundp" "fdefinition" "macroexpand"
  "macroexpand-1" "eval" "compile" "load" "require" "provide"))

; Special operators and the standard macros that read as syntax.

((list . (symbol) @keyword)
 (#any-of? @keyword
  "let" "let*" "flet" "labels" "macrolet" "symbol-macrolet" "progn" "prog1" "prog2"
  "prog" "prog*" "block" "return" "return-from" "tagbody" "go" "catch" "throw"
  "setq" "setf" "psetq" "psetf" "incf" "decf" "push" "pop" "pushnew" "rotatef"
  "shiftf" "multiple-value-bind" "multiple-value-call" "multiple-value-prog1"
  "multiple-value-list" "multiple-value-setq" "nth-value" "destructuring-bind"
  "with-slots" "with-accessors" "with-open-file" "with-open-stream"
  "with-output-to-string" "with-input-from-string" "with-standard-io-syntax"
  "with-simple-restart" "with-condition-restarts" "with-hash-table-iterator"
  "with-package-iterator" "with-compilation-unit" "eval-when" "locally" "the"
  "declare" "declaim" "proclaim" "load-time-value" "function" "quote" "progv"
  "define-symbol-macro" "in-package" "defpackage" "defvar" "defparameter"
  "defconstant" "defclass" "defstruct" "define-condition" "deftype"
  "print-unreadable-object" "time"))

((list . (symbol) @keyword.function)
 (#any-of? @keyword.function
  "defun" "defmacro" "defgeneric" "defmethod" "lambda" "define-compiler-macro"
  "defsetf" "define-setf-expander" "define-modify-macro" "define-method-combination"))

((list . (symbol) @keyword.operator)
 (#any-of? @keyword.operator "and" "or"))

((list . (symbol) @conditional)
 (#any-of? @conditional
  "if" "when" "unless" "cond" "case" "ecase" "ccase" "typecase" "etypecase"
  "ctypecase"))

((list . (symbol) @repeat)
 (#any-of? @repeat
  "loop" "do" "do*" "dolist" "dotimes" "do-symbols" "do-external-symbols"
  "do-all-symbols"))

((list . (symbol) @exception)
 (#any-of? @exception
  "handler-case" "handler-bind" "restart-case" "restart-bind" "ignore-errors"
  "unwind-protect" "error" "cerror" "warn" "signal" "assert" "check-type"
  "invoke-restart" "invoke-debugger"))

; LOOP's own keywords, anywhere directly in the LOOP form.
((list . (symbol) @_loop
   (symbol) @keyword)
 (#eq? @_loop "loop")
 (#any-of? @keyword
  "for" "as" "with" "in" "on" "across" "being" "the" "each" "hash-key" "hash-keys"
  "hash-value" "hash-values" "of" "using" "from" "upfrom" "downfrom" "to" "upto"
  "downto" "below" "above" "by" "then" "=" "and" "do" "doing" "collect" "collecting"
  "append" "appending" "nconc" "nconcing" "sum" "summing" "count" "counting"
  "maximize" "maximizing" "minimize" "minimizing" "into" "when" "if" "unless"
  "else" "end" "while" "until" "repeat" "always" "never" "thereis" "finally"
  "initially" "return" "named"))

; Definitions: the name a form defines.

(list . (symbol) @_def
  . [(symbol) (qualified_symbol)] @function
  (#any-of? @_def "defun" "defgeneric" "define-compiler-macro"))

(list . (symbol) @_def
  . [(symbol) (qualified_symbol)] @function.macro
  (#any-of? @_def "defmacro" "define-modify-macro" "define-symbol-macro"))

(list . (symbol) @_def
  . [(symbol) (qualified_symbol)] @function.method
  (#eq? @_def "defmethod"))

; (defun (setf name) ...) and (defmethod (setf name) ...)
(list . (symbol) @_def
  . (list . (symbol) @_setf . (symbol) @function)
  (#any-of? @_def "defun" "defgeneric" "defmethod")
  (#eq? @_setf "setf"))

(list . (symbol) @_def
  . (symbol) @type
  (#any-of? @_def "defclass" "defstruct" "define-condition" "deftype"))

; (defstruct (point (:constructor make-pt)) ...)
(list . (symbol) @_def
  . (list . (symbol) @type)
  (#eq? @_def "defstruct"))

(list . (symbol) @_def
  . (symbol) @constant
  (#eq? @_def "defconstant"))

; Parameters: the lambda list right after a definition's name, or a
; LAMBDA's own. A required parameter is a bare symbol; an &optional or
; &key one, or a DEFMETHOD's specialized one, is a list it heads.
(list . (symbol) @_def
  . (_)
  . (list [(symbol) @variable.parameter (list . (symbol) @variable.parameter)])
  (#any-of? @_def "defun" "defmacro" "defgeneric" "define-compiler-macro"))

(list . (symbol) @_def
  . (_)
  (keyword)*
  . (list [(symbol) @variable.parameter (list . (symbol) @variable.parameter)])
  (#eq? @_def "defmethod"))

(list . (symbol) @_def
  . (list [(symbol) @variable.parameter (list . (symbol) @variable.parameter)])
  (#eq? @_def "lambda"))

; Local bindings: (let ((x 1) y) ...) binds x and y, and the operator
; position of a binding is not a call.
(list . (symbol) @_let
  . (list [(symbol) @variable (list . (symbol) @variable)])
  (#any-of? @_let "let" "let*" "symbol-macrolet" "prog" "prog*" "with-slots"))

(list . (symbol) @_bind
  . (list (symbol) @variable)
  (#any-of? @_bind "multiple-value-bind" "destructuring-bind"))

(list . (symbol) @_do
  . (list . (symbol) @variable)
  (#any-of? @_do "dolist" "dotimes" "do-symbols" "do-external-symbols" "do-all-symbols"
   "with-open-file" "with-open-stream" "with-output-to-string" "with-input-from-string"
   "with-hash-table-iterator" "with-package-iterator"))

(list . (symbol) @_do
  . (list (list . (symbol) @variable))
  (#any-of? @_do "do" "do*"))

; (flet ((helper (x) ...)) ...): local functions and their parameters.
(list . (symbol) @_flet
  . (list (list . (symbol) @function
     . (list [(symbol) @variable.parameter (list . (symbol) @variable.parameter)])))
  (#any-of? @_flet "flet" "labels" "macrolet"))

; #+sbcl / #-(or ccl ecl): the feature test is the reader's, not code
; (after the calls, so `or` there is not one).
(feature_expression operator: _ @preproc)
(feature_expression feature: (symbol) @preproc)
(feature_expression feature: (keyword) @preproc)
(feature_expression feature: (list (symbol) @preproc))
(feature_expression feature: (list (keyword) @preproc))

; T and NIL, even at a list's head: (cond (... ) (t ...)) is not a call.
((symbol) @constant.builtin
 (#any-of? @constant.builtin "t" "nil"))

; Lambda-list keywords, wherever they are.
((symbol) @keyword.directive
 (#match? @keyword.directive "^&"))

; Quoted data is not code: the head of a quoted list is just a symbol.
((quote value: (list . (symbol) @variable))
 (#not-any-of? @variable "t" "nil"))
