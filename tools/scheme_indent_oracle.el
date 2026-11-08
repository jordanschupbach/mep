;;; scheme_indent_oracle.el --- the reference layout for mep's Scheme formatter  -*- lexical-binding: t -*-

;; Usage:
;;
;;   emacs -Q --batch -l tools/scheme_indent_oracle.el FILE [OUT]
;;
;; Lays FILE out the way Emacs does in `scheme-mode' -- `lisp-indent-line'
;; with `scheme-indent-function' and the `scheme-indent-function'
;; properties scheme.el puts on `let', `lambda', `define-record-type'...
;; -- with `indent-tabs-mode' off, plus the whitespace rules mep's own
;; formatter (src/scheme_format.cpp) adds, and writes the result to OUT,
;; or to stdout. This is what `schemefmt::Format' is measured against on
;; the corpus (`just format-compare scheme').
;;
;; With MEP_ORACLE_RAW=1 in the environment only Emacs's own
;; `indent-region' runs -- no mep rules, nothing protected -- for measuring
;; how far the deliberate differences reach.
;;
;; mep's rules, in the order they run (the same as tools/cl_indent_oracle.el's):
;;   1. whitespace: no blanks just inside parens (`( foo )' is `(foo)'); a
;;      close paren alone on its line joins the line before it unless that
;;      line ends in a `;' comment or the parens carry their own comment;
;;      no trailing blanks; at most two blank lines in a row (nothing
;;      inside a string or a #|...|# comment);
;;   2. Emacs's indent-region, except that these lines keep their
;;      indentation: a line starting inside a #|...|# or #;DATUM comment,
;;      the line such a multi-line comment starts on, and a line holding
;;      nothing but a single-`;' comment (Emacs sends that to
;;      `comment-column');
;;   3. a line indent-region left alone has the tabs in its indentation
;;      turned into spaces;
;;   4. no blank lines at the end of the file, and a final newline.

(require 'scheme)

(defun scheme-oracle--state (pos)
  ;; syntax-ppss may run scheme-syntax-propertize, whose searches clobber
  ;; the match data: callers take their match positions first.
  (save-excursion (save-match-data (syntax-ppss pos))))

(defun scheme-oracle--code-p (pos)
  "Non-nil when POS is outside every string, comment and escape."
  (let ((s (scheme-oracle--state pos)))
    (not (or (nth 3 s) (nth 4 s) (nth 5 s)))))

(defun scheme-oracle--text-p (pos)
  "Non-nil when POS is inside a string or a nested (#|...|# or #;) comment."
  (let ((s (scheme-oracle--state pos)))
    (or (nth 3 s) (integerp (nth 4 s)))))

(defun scheme-oracle--whitespace ()
  ;; `( foo' -> `(foo', for every open-paren character.
  (goto-char (point-min))
  (while (re-search-forward "[([]\\([ \t]+\\)[^ \t\n;]" nil t)
    (let ((b (match-beginning 1)) (e (match-end 1)))
      (if (and (scheme-oracle--code-p (1- b))
               ;; `#\( ' is a character, not a paren.
               (not (eq (char-before (1- b)) ?\\)))
          (progn (delete-region b e) (goto-char b))
        (goto-char e))))
  ;; `foo )' -> `foo)'; a lone `)' joins the line before.
  (goto-char (point-min))
  (while (re-search-forward "[ \t\n]+[])]" nil t)
    (let ((close (1- (point)))
          (b (match-beginning 0)))
      (when (scheme-oracle--code-p close)
        ;; An escaped blank (`#\ ') belongs to the token before.
        (when (nth 5 (scheme-oracle--state b)) (setq b (1+ b)))
        (let ((s (scheme-oracle--state b)))
          (unless (or (nth 4 s) (nth 3 s) (>= b close)
                      ;; A line of close parens with its own comment stays.
                      (and (save-excursion (goto-char b) (search-forward "\n" close t))
                           (save-excursion (goto-char close) (skip-chars-forward ")] \t") (looking-at ";"))))
            (delete-region b close)
            (goto-char (1+ b)))))))
  ;; Trailing blanks, outside strings and nested comments.
  (goto-char (point-min))
  (while (re-search-forward "[ \t]+$" nil t)
    (let ((b (match-beginning 0)) (e (match-end 0)))
      (unless (scheme-oracle--text-p b)
        (delete-region b e))))
  ;; Runs of blank lines.
  (goto-char (point-min))
  (while (re-search-forward "\n\n\n\\(\n+\\)" nil t)
    (let ((b (match-beginning 1)) (e (match-end 1)))
      (unless (scheme-oracle--text-p (- b 2))
        (delete-region b e)
        (goto-char b)))))

(defun scheme-oracle--kept-lines ()
  "The lines indent-region must leave alone, with their text."
  (let (kept)
    (save-excursion
      (goto-char (point-min))
      (while (not (eobp))
        (let* ((bol (line-beginning-position))
               (s (scheme-oracle--state bol)))
          (when (or (integerp (nth 4 s))
                    (and (not (nth 3 s))
                         (or (looking-at "[ \t]*;\\([^;]\\|$\\)")
                             (and (looking-at "[ \t]*#[|;]")
                                  (< (line-end-position) (point-max))
                                  (integerp (nth 4 (scheme-oracle--state (1+ (line-end-position)))))))))
            (push (cons (line-number-at-pos) (buffer-substring bol (line-end-position))) kept)))
        (forward-line 1)))
    kept))

(defun scheme-oracle-run (file out)
  (let ((raw (equal (getenv "MEP_ORACLE_RAW") "1"))
        (coding-system-for-read 'utf-8-unix)
        (coding-system-for-write 'utf-8-unix))
    (with-temp-buffer
      (insert-file-contents file)
      (scheme-mode)
      (setq-local indent-tabs-mode nil)
      (syntax-propertize (point-max))
      (if raw
          (indent-region (point-min) (point-max))
        (scheme-oracle--whitespace)
        (syntax-propertize (point-max))
        (let ((kept (scheme-oracle--kept-lines)))
          (indent-region (point-min) (point-max))
          (dolist (cell kept)
            (save-excursion
              (goto-char (point-min))
              (forward-line (1- (car cell)))
              (delete-region (line-beginning-position) (line-end-position))
              (insert (cdr cell)))))
        ;; Tabs left in indentation become spaces.
        (goto-char (point-min))
        (while (re-search-forward "^[ \t]*\t[ \t]*" nil t)
          (let ((b (match-beginning 0)) (e (match-end 0)))
            (unless (scheme-oracle--text-p b)
              (let ((col (current-column)))
                (delete-region b e)
                (insert (make-string col ?\s))))))
        (goto-char (point-max))
        (skip-chars-backward " \t\n")
        (delete-region (point) (point-max))
        (unless (bobp) (insert "\n")))
      ;; Through write-region even to stdout: `princ' would print a byte
      ;; that is not UTF-8 (a Latin-1 or EUC-JP file) in Emacs's internal
      ;; form rather than as the byte it was.
      (let ((create-lockfiles nil))
        (write-region (point-min) (point-max) (or out "/dev/stdout") nil 'silent)))))

(when command-line-args-left
  (let ((file (pop command-line-args-left))
        (out (pop command-line-args-left)))
    (scheme-oracle-run file out)
    (setq command-line-args-left nil)))

;;; scheme_indent_oracle.el ends here
