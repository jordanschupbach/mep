; Highlight captures for mep's Scheme grammar (nvim-treesitter naming).
;
; Pattern order is load-bearing: two captures over the exact same bytes
; are resolved later-pattern-wins (TreesitterHighlight's sort in
; treesitter.cpp), so the broad conventions come first and the narrower
; rules that override them after. The patterns of the grammar this one
; replaces (6cdh/tree-sitter-scheme's queries/highlights.scm, MIT) are
; all here, so a Scheme file colours as it did; R7RS's and Guile's
; special forms, `#:keywords` and Guix's G-expressions are added.

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
(vector "#(" @punctuation.bracket)
(byte_vector ["#vu8(" "#u8("] @punctuation.bracket)

(number) @number
(character) @constant.builtin
(boolean) @constant.builtin
(symbol) @variable
(keyword) @constant

(string) @string

(escape_sequence) @escape

; Calls: the first symbol of any list.
(list
  .
  (symbol) @function)

; [x y]: a binding list in R6RS style.
(list
  .
  "["
  .
  (symbol)+ @variable
  .
  "]")

((symbol) @operator
 (#match? @operator "^(\\+|-|\\*|/|=|>|<|>=|<=)$"))

; Special forms and the standard syntax keywords (R5RS/R6RS/R7RS-small),
; plus Guile's module forms.
(list
  .
  (symbol) @keyword
  (#match? @keyword
   "^(define-syntax|let\\*|lambda|λ|case|=>|quote-splicing|unquote-splicing|set!|let|letrec|letrec-syntax|let-values|let\\*-values|do|else|define|cond|syntax-rules|unquote|begin|quote|let-syntax|and|if|quasiquote|letrec|delay|or|when|unless|identifier-syntax|assert|library|export|import|rename|only|except|prefix)$"
   ))

(list
  .
  (symbol) @keyword
  (#match? @keyword
   "^(letrec\\*|case-lambda|define-values|define-record-type|parameterize|guard|delay-force|define-library|include|include-ci|include-library-declarations|cond-expand|syntax-case|with-syntax|syntax|quasisyntax|unsyntax|unsyntax-splicing|define-module|use-modules|define-public|define\\*|define\\*-public|lambda\\*|define-syntax-rule|define-macro|eval-when|receive|let-optional|let-keywords|match|match-lambda|match-let|define-inlinable|while)$"
   ))

(list
  .
  (symbol) @function.builtin
  (#match? @function.builtin
   "^(caar|cadr|call-with-input-file|call-with-output-file|cdar|cddr|list|open-input-file|open-output-file|with-input-from-file|with-output-to-file|\\*|\\+|-|/|<|<=|=|>|>=|abs|acos|angle|append|apply|asin|assoc|assq|assv|atan|boolean\\?|caaaar|caaadr|caaar|caadar|caaddr|caadr|cadaar|cadadr|cadar|caddar|cadddr|caddr|call-with-current-continuation|call-with-values|car|cdaaar|cdaadr|cdaar|cdadar|cdaddr|cdadr|cddaar|cddadr|cddar|cdddar|cddddr|cdddr|cdr|ceiling|char->integer|char-alphabetic\\?|char-ci<=\\?|char-ci<\\?|char-ci=\\?|char-ci>=\\?|char-ci>\\?|char-downcase|char-lower-case\\?|char-numeric\\?|char-ready\\?|char-upcase|char-upper-case\\?|char-whitespace\\?|char<=\\?|char<\\?|char=\\?|char>=\\?|char>\\?|char\\?|close-input-port|close-output-port|complex\\?|cons|cos|current-error-port|current-input-port|current-output-port|denominator|display|dynamic-wind|eof-object\\?|eq\\?|equal\\?|eqv\\?|eval|even\\?|exact->inexact|exact\\?|exp|expt|floor|flush-output|for-each|force|gcd|imag-part|inexact->exact|inexact\\?|input-port\\?|integer->char|integer\\?|interaction-environment|lcm|length|list->string|list->vector|list-ref|list-tail|list\\?|load|log|magnitude|make-polar|make-rectangular|make-string|make-vector|map|max|member|memq|memv|min|modulo|negative\\?|newline|not|null-environment|null\\?|number->string|number\\?|numerator|odd\\?|output-port\\?|pair\\?|peek-char|positive\\?|procedure\\?|quotient|rational\\?|rationalize|read|read-char|real-part|real\\?|remainder|reverse|round|scheme-report-environment|set-car!|set-cdr!|sin|sqrt|string|string->list|string->number|string->symbol|string-append|string-ci<=\\?|string-ci<\\?|string-ci=\\?|string-ci>=\\?|string-ci>\\?|string-copy|string-fill!|string-length|string-ref|string-set!|string<=\\?|string<\\?|string=\\?|string>=\\?|string>\\?|string\\?|substring|symbol->string|symbol\\?|tan|transcript-off|transcript-on|truncate|values|vector|vector->list|vector-fill!|vector-length|vector-ref|vector-set!|vector\\?|write|write-char|zero\\?)$"
   ))

;; quote ;;

;; hardcoded highlight four levels of nested structure

; 'atom
(quote
  _ @constant)

; '()
(quote
  (_ _* @constant))

; '(())
(quote
  (_ (_ _* @constant)))

; '((()))
(quote
  (_ (_ (_ _* @constant))))

;; sexp comment ;;

;; hardcoded highlight four levels of nested structure

; #;atom
(comment
  _ @comment)

; #;(list)
(comment
  (_ _* @comment))

; #;(list (list))
(comment
  (_ (_ _* @comment)))

; #;(list (list (list)))
(comment
  (_ (_ (_ _ @comment))))

; Guix's G-expressions: the prefixes, like the quote family's.
(gexp "#~" @punctuation.special)
(ungexp ["#$" "#+"] @punctuation.special)
(ungexp_splicing ["#$@" "#+@"] @punctuation.special)

[(comment)
 (block_comment)
 (directive)] @comment
