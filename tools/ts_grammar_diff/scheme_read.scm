;;; scheme_read.scm -- "does this file read?" for mep's Scheme tools.
;;;
;;;   guile --no-auto-compile -s tools/ts_grammar_diff/scheme_read.scm FILE
;;;
;;; Reads every datum of FILE with Guile's own `read' (nothing is
;;; evaluated or expanded) and exits 0 when the whole file reads, 1 with
;;; Guile's message -- FILE:LINE:COL: ... -- on stderr when it does not.
;;; Guile's reader takes R7RS-small and R6RS lexical syntax (`#;', nested
;;; `#|...|#', `[...]', `#u8(', `#vu8(', `#!r6rs'/`#!fold-case', `#:kw')
;;; plus its own extensions, so it is the oracle for grammars/
;;; tree-sitter-scheme's corpus diff and for mep-scheme-lsp's syntax
;;; errors (tools/scheme_read_check.py).
;;;
;;; Guix's G-expression syntax is installed first, the way `(guix gexp)'
;;; installs it (`#~x' `#$x' `#$@x' `#+x' `#+@x'), since a Guix file is
;;; read with it; nothing else in Guile's syntax uses those characters.

(use-modules (ice-9 rdelim))

(define (gexp-reader plain splicing)
  (lambda (chr port)
    (if (eqv? (peek-char port) #\@)
        (begin (read-char port) (list splicing (read port)))
        (list plain (read port)))))
(read-hash-extend #\~ (lambda (chr port) (list 'gexp (read port))))
(read-hash-extend #\$ (gexp-reader 'ungexp 'ungexp-splicing))
(read-hash-extend #\+ (gexp-reader 'ungexp-native 'ungexp-native-splicing))

;;; With --batch LIST instead of FILE: reads every file named in LIST (one
;;; path per line) and prints "PATH<TAB>ok" or "PATH<TAB>MESSAGE" (the
;;; message without the file name) for each, in one process.

(define (message key rest)
  ;; read-error / syntax-error: (subr message args rest) with the port's
  ;; position already in the message as FILE:LINE:COL.
  (call-with-output-string
   (lambda (o)
     (if (and (pair? rest) (= (length rest) 4) (string? (cadr rest)))
         ;; One of Guile's messages ("invalid bytevector prefix") passes an
         ;; argument it has no ~ for; format refuses that, so the message
         ;; is printed as it stands.
         (display (catch #t
                    (lambda () (apply format #f (cadr rest) (caddr rest)))
                    (lambda _ (cadr rest)))
                  o)
         (write (cons key rest) o)))))

(define (read-file file)
  "#f when FILE reads, else Guile's message."
  (catch #t
    (lambda ()
      (call-with-input-file file
        (lambda (port)
          (set-port-filename! port file)
          (let loop ()
            (let ((datum (read port)))
              (unless (eof-object? datum)
                (loop)))))
        #:guess-encoding #t)
      #f)
    (lambda (key . rest)
      (message key rest))))

(define args (cdr (command-line)))
(if (and (pair? args) (string=? (car args) "--batch"))
    (call-with-input-file (cadr args)
      (lambda (list-port)
        (let loop ()
          (let ((line (read-line list-port)))
            (unless (eof-object? line)
              (let ((msg (read-file line)))
                (format #t "~a\t~a\n" line
                        (if msg
                            (let ((prefix (string-append line ":")))
                              (if (string-prefix? prefix msg)
                                  (substring msg (string-length prefix))
                                  msg))
                            "ok")))
              (loop))))))
    (let ((msg (read-file (car args))))
      (when msg
        (display msg (current-error-port))
        (newline (current-error-port))
        (exit 1))))
(exit 0)
