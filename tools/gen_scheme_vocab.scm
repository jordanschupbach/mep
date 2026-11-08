;;; gen_scheme_vocab.scm --- mep-scheme-lsp's vocabulary, from Guile itself.
;;;
;;;   guile --no-auto-compile -s tools/gen_scheme_vocab.scm > src/scheme_lsp_names.cpp
;;;   (or: just scheme-lsp-vocab)
;;;
;;; Walks the public interface of every module listed below in a running
;;; Guile -- (guile), R7RS-small's (scheme ...) libraries, the SRFIs Guile
;;; ships, the common (ice-9 ...) and (rnrs ...) modules -- and writes one
;;; row per exported name: the module, whether it is syntax, a procedure or
;;; a variable, the argument counts Guile's own arity for the procedure
;;; allows, and its argument list.
;;;
;;; Names, kinds, arities and argument lists only, no documentation:
;;; Guile's manual is GFDL and R7RS's text has its own terms, so no prose
;;; is copied. Where Guile's primitive has no argument names (`_`), the
;;; names R7RS-small's entry headings use are filled in from the table
;;; below (`(list-tail list k)`), the names only.

(use-modules (system vm program) (system vm debug) (ice-9 match) (srfi srfi-1) (ice-9 format) (rnrs bytevectors))

(define modules
  (append
   '((guile))
   (map (lambda (m) (list 'scheme m))
        '(base case-lambda char complex cxr eval file inexact lazy load process-context read repl time write r5rs))
   (map (lambda (n) (list 'srfi (string->symbol (format #f "srfi-~a" n))))
        '(1 2 4 6 8 9 11 13 14 16 17 18 19 26 27 28 31 34 35 37 38 39 41 42 43 45 60 64 67 69 71 88 98 111 171
          197))
   (map (lambda (m) (list 'ice-9 m))
        '(and-let-star arrays binary-ports control curried-definitions documentation exceptions format ftw futures
          getopt-long hash-table i18n iconv list match optargs peg poll popen ports pretty-print q rdelim receive
          regex streams string-fun suspendable-ports textual-ports threads time vlist weak-vector))
   '((rnrs) (rnrs base) (rnrs bytevectors) (rnrs conditions) (rnrs control) (rnrs enums) (rnrs eval)
     (rnrs exceptions) (rnrs files) (rnrs hashtables) (rnrs lists) (rnrs mutable-pairs) (rnrs mutable-strings)
     (rnrs programs) (rnrs sorting) (rnrs syntax-case) (rnrs unicode) (rnrs records syntactic)
     (rnrs records procedural) (rnrs records inspection) (rnrs arithmetic fixnums) (rnrs arithmetic flonums)
     (rnrs arithmetic bitwise) (rnrs io ports) (rnrs io simple)
     (system foreign) (web uri) (web client) (web request) (web response) (web http) (web server)
     (sxml simple) (sxml match) (sxml xpath) (sxml transform) (texinfo) (language tree-il))))

;; R7RS-small's argument names for procedures Guile defines in C (whose
;; lambda lists say only `_`). `...` marks a rest argument.
(define r7rs-names
  '((car pair) (cdr pair) (cons obj1 obj2) (set-car! pair obj) (set-cdr! pair obj) (caar pair) (cadr pair)
    (cdar pair) (cddr pair) (list? obj) (pair? obj) (null? obj) (length list) (append list ...)
    (reverse list) (list-tail list k) (list-ref list k) (list-set! list k obj) (list-copy obj)
    (memq obj list) (memv obj list) (member obj list compare) (assq obj alist) (assv obj alist)
    (assoc obj alist compare) (make-list k fill) (symbol? obj) (symbol->string symbol)
    (string->symbol string) (symbol=? symbol1 symbol2 ...) (char? obj) (char->integer char)
    (integer->char n) (string? obj) (make-string k char) (string char ...) (string-length string)
    (string-ref string k) (string-set! string k char) (substring string start end)
    (string-append string ...) (string->list string start end) (list->string list)
    (string-copy string start end) (string-copy! to at from start end) (string-fill! string fill start end)
    (string=? string1 string2 ...) (string<? string1 string2 ...) (string>? string1 string2 ...)
    (string<=? string1 string2 ...) (string>=? string1 string2 ...) (vector? obj) (make-vector k fill)
    (vector obj ...) (vector-length vector) (vector-ref vector k) (vector-set! vector k obj)
    (vector->list vector start end) (list->vector list) (vector->string vector start end)
    (string->vector string start end) (vector-copy vector start end) (vector-copy! to at from start end)
    (vector-append vector ...) (vector-fill! vector fill start end) (bytevector? obj)
    (make-bytevector k byte) (bytevector byte ...) (bytevector-length bytevector)
    (bytevector-u8-ref bytevector k) (bytevector-u8-set! bytevector k byte)
    (bytevector-copy bytevector start end) (bytevector-copy! to at from start end)
    (bytevector-append bytevector ...) (utf8->string bytevector start end) (string->utf8 string start end)
    (procedure? obj) (apply proc arg1 ... args) (map proc list1 list2 ...) (string-map proc string1 string2 ...)
    (vector-map proc vector1 vector2 ...) (for-each proc list1 list2 ...)
    (string-for-each proc string1 string2 ...) (vector-for-each proc vector1 vector2 ...)
    (call-with-current-continuation proc) (call/cc proc) (values obj ...) (call-with-values producer consumer)
    (dynamic-wind before thunk after) (eq? obj1 obj2) (eqv? obj1 obj2) (equal? obj1 obj2) (not obj)
    (boolean? obj) (boolean=? boolean1 boolean2 ...) (number? obj) (complex? obj) (real? obj)
    (rational? obj) (integer? obj) (exact? z) (inexact? z) (exact-integer? z) (nan? z) (finite? z)
    (infinite? z) (= z1 z2 ...) (< x1 x2 ...) (> x1 x2 ...) (<= x1 x2 ...) (>= x1 x2 ...) (zero? z)
    (positive? x) (negative? x) (odd? n) (even? n) (max x1 x2 ...) (min x1 x2 ...) (+ z ...) (* z ...)
    (- z1 z2 ...) (/ z1 z2 ...) (abs x) (quotient n1 n2) (remainder n1 n2) (modulo n1 n2) (gcd n ...)
    (lcm n ...) (numerator q) (denominator q) (floor x) (ceiling x) (truncate x) (round x)
    (rationalize x y) (exp z) (log z1 z2) (sin z) (cos z) (tan z) (asin z) (acos z) (atan y x)
    (square z) (sqrt z) (exact-integer-sqrt k) (expt z1 z2) (exact z) (inexact z)
    (number->string z radix) (string->number string radix) (char-upcase char) (char-downcase char)
    (char-foldcase char) (char-alphabetic? char) (char-numeric? char) (char-whitespace? char)
    (char-upper-case? char) (char-lower-case? char) (digit-value char) (string-upcase string)
    (string-downcase string) (string-foldcase string) (char=? char1 char2 ...) (char<? char1 char2 ...)
    (char>? char1 char2 ...) (char<=? char1 char2 ...) (char>=? char1 char2 ...)
    (display obj port) (write obj port) (write-shared obj port) (write-simple obj port) (newline port)
    (write-char char port) (write-string string port start end) (write-u8 byte port)
    (write-bytevector bytevector port start end) (read port) (read-char port) (peek-char port)
    (read-line port) (read-string k port) (read-u8 port) (peek-u8 port) (u8-ready? port)
    (read-bytevector k port) (read-bytevector! bytevector port start end) (char-ready? port)
    (eof-object) (eof-object? obj) (flush-output-port port) (close-port port) (close-input-port port)
    (close-output-port port) (input-port? obj) (output-port? obj) (textual-port? obj) (binary-port? obj)
    (port? obj) (input-port-open? port) (output-port-open? port) (current-input-port) (current-output-port)
    (current-error-port) (open-input-string string) (open-output-string) (get-output-string port)
    (open-input-bytevector bytevector) (open-output-bytevector) (get-output-bytevector port)
    (call-with-port port proc) (call-with-input-file string proc) (call-with-output-file string proc)
    (with-input-from-file string thunk) (with-output-to-file string thunk) (open-input-file string)
    (open-output-file string) (open-binary-input-file string) (open-binary-output-file string)
    (file-exists? filename) (delete-file filename) (command-line) (exit obj) (emergency-exit obj)
    (get-environment-variable name) (get-environment-variables) (current-second) (current-jiffy)
    (jiffies-per-second) (features) (load filename environment) (eval expr environment)
    (environment list ...) (interaction-environment) (error message obj ...) (error-object? obj)
    (error-object-message error-object) (error-object-irritants error-object) (read-error? obj)
    (file-error? obj) (raise obj) (raise-continuable obj) (with-exception-handler handler thunk)
    (make-parameter value converter) (make-promise obj) (promise? obj) (force promise)
    (string-index s pred start end) (string-join string-list delimiter grammar) (list-index pred list ...)
    (assq-ref alist key) (assoc-ref alist key) (hash-ref table key default) (hash-set! table key value)
    (format destination message arg ...) (sort items less) (sort! items less)))

(define (r7rs-names-for name)
  (assq-ref r7rs-names name))

(define (kind-of value)
  (cond ((macro? value) "syntax")
        ((procedure? value) "procedure")
        (else "variable")))

;; The argument counts a procedure accepts: (min . max), max -1 for "any
;; number more", or #f when it takes keywords (counts mean little there).
(define (arity value)
  ;; Every clause of a compiled procedure (a case-lambda has several; the
  ;; minimum arity only describes the first), else the primitive's arity.
  (let ((arities (false-if-exception
                  (and (program? value) (find-program-arities (program-code value))))))
    (cond
     ((and (pair? arities)
           (any (lambda (a) (or (arity-has-keyword-args? a) (arity-allow-other-keys? a))) arities))
      #f)
     ((pair? arities)
      (cons (apply min (map arity-nreq arities))
            (if (any arity-has-rest? arities)
                -1
                (apply max (map (lambda (a) (+ (arity-nreq a) (arity-nopt a))) arities)))))
     (else
      (match (procedure-minimum-arity value)
        ((req opt rest?) (cons req (if rest? -1 (+ req opt))))
        (_ #f))))))

;; "(name a b #:optional c . rest)", with R7RS's names for `_`.
(define (signature name value)
  (let* ((ll (false-if-exception (program-lambda-list value)))
         (std (r7rs-names-for name)))
    (define (render ll)
      (with-output-to-string
        (lambda ()
          (display "(")
          (display name)
          (let lp ((ll ll) (i 0))
            (cond
             ((null? ll) #t)
             ((symbol? ll) (format #t " . ~a" (if (eq? ll '_) (std-name i #t) ll)))
             ((pair? ll)
              (let ((x (car ll)))
                (format #t " ~a" (cond ((eq? x '_) (std-name i #f))
                                       ((keyword? x) (format #f "#:~a" (keyword->symbol x)))
                                       ((pair? x) (car x))
                                       (else x)))
                (lp (cdr ll) (if (keyword? x) i (1+ i)))))
             (else #t)))
          (display ")"))))
    (define (std-name i rest?)
      (cond
       ((not std) (if rest? "rest" (format #f "arg~a" (1+ i))))
       ((and (not rest?) (< i (length std)) (not (eq? (list-ref std i) '...))) (list-ref std i))
       (else (let ((named (filter (lambda (x) (not (eq? x '...))) std)))
               (if (< i (length named)) (string-append (symbol->string (list-ref named i)) " ...") "rest")))))
    (cond
     ((and (eq? ll '_) std)
      ;; Every argument optional and unnamed: show R7RS's list.
      (format #f "(~a~{ ~a~})" name std))
     ((or (pair? ll) (null? ll) (symbol? ll)) (render ll))
     (std (format #f "(~a~{ ~a~})" name std))
     (else ""))))

(define (c-string s)
  (with-output-to-string
    (lambda ()
      (display "\"")
      (string-for-each
       (lambda (c)
         (case c
           ((#\") (display "\\\""))
           ((#\\) (display "\\\\"))
           ((#\?) (display "\\?"))  ; never a trigraph
           (else (if (or (char<? c #\space) (char>? c #\~))
                     ;; one \xHH escape per UTF-8 byte (λ is two bytes)
                     (for-each (lambda (b) (format #t "\\x~2,'0x\"\"" b))
                               (bytevector->u8-list (string->utf8 (string c))))
                     (display c)))))
       s)
      (display "\""))))

(define rows '())
(for-each
 (lambda (mod)
   (let ((iface (false-if-exception (resolve-interface mod))))
     (when iface
       (module-for-each
        (lambda (sym var)
          (let* ((value (and (variable-bound? var) (variable-ref var)))
                 (kind (kind-of value))
                 (ar (and (procedure? value) (not (macro? value)) (arity value)))
                 (sig (if (equal? kind "procedure") (signature sym value) "")))
            (set! rows (cons (list (symbol->string sym)
                                   (string-join (map symbol->string mod) " ")
                                   kind
                                   (if ar (car ar) -2)
                                   (if ar (cdr ar) -2)
                                   sig)
                             rows))))
        iface))))
 modules)

;; Sorted by name, then module, for binary search.
(set! rows (sort rows (lambda (a b)
                        (or (string<? (car a) (car b))
                            (and (string=? (car a) (car b)) (string<? (cadr a) (cadr b)))))))
(set! rows (delete-duplicates rows))

(format #t "// Generated by tools/gen_scheme_vocab.scm from GNU Guile ~a -- do not edit.\n" (version))
(display "// Names, kinds, arities and argument lists of the modules' exports;
// no documentation (see the generator for why).
#include \"scheme_lsp_names.h\"

namespace {

const SchemeVocabEntry kEntries[] = {
")
(for-each
 (match-lambda
   ((name mod kind min max sig)
    (format #t "    {~a, ~a, SchemeVocabKind::~a, ~a, ~a, ~a},\n" (c-string name) (c-string mod)
            (cond ((string=? kind "syntax") "Syntax") ((string=? kind "procedure") "Procedure") (else "Variable"))
            min max (c-string sig))))
 rows)
(display "};

}  // namespace

const SchemeVocabEntry *SchemeVocabBegin() { return kEntries; }
const SchemeVocabEntry *SchemeVocabEnd() { return kEntries + sizeof kEntries / sizeof kEntries[0]; }
")
