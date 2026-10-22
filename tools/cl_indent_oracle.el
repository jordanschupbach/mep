;;; cl_indent_oracle.el --- the reference layout for mep's Common Lisp formatter  -*- lexical-binding: t -*-

;; Usage:
;;
;;   emacs -Q --batch -l tools/cl_indent_oracle.el FILE [OUT]
;;
;; Lays FILE out the way Emacs does with `common-lisp-indent-function'
;; (what SLIME and Sly set up for Common Lisp buffers) and
;; `indent-tabs-mode' off, plus the whitespace rules mep's own formatter
;; (src/lisp_format.cpp) adds, and writes the result to OUT, or to stdout.
;; This is what `lispfmt::Format' is measured against on the corpus.
;;
;; With MEP_ORACLE_RAW=1 in the environment only Emacs's own
;; `indent-region' runs -- no mep rules, nothing protected -- for measuring
;; how far the deliberate differences reach.
;;
;; mep's rules, in the order they run:
;;   1. whitespace: no blanks just inside parens (`( foo )' is `(foo)'); a
;;      close paren alone on its line joins the line before it unless that
;;      line ends in a `;' comment or the parens carry their own comment;
;;      no trailing blanks; at most two blank lines in a row (nothing
;;      inside a string or a #|...|# comment);
;;   2. Emacs's indent-region, except that these lines keep their
;;      indentation: a line starting inside a #|...|# comment, the line a
;;      multi-line #| comment starts on, and a line holding nothing but a
;;      single-`;' comment (Emacs sends that to `comment-column');
;;   3. a line indent-region left alone has the tabs in its indentation
;;      turned into spaces;
;;   4. no blank lines at the end of the file, and a final newline.
;;
;; Emacs's own limits show on input that is not indented yet: with a
;; `defmethod' whose body starts in column 0, `lisp-indent-defmethod's
;; `beginning-of-defun' lands on that body line and indent-region stops
;; with a scan error (no output), or gives a result it changes on a second
;; run. The formatter does what Emacs does on the indented file -- its
;; result equals this oracle run twice.

(require 'cl-indent)

(defun cl-oracle--state (pos)
  (save-excursion (syntax-ppss pos)))

(defun cl-oracle--code-p (pos)
  "Non-nil when POS is outside every string, comment and escape."
  (let ((s (cl-oracle--state pos)))
    (not (or (nth 3 s) (nth 4 s) (nth 5 s)))))

(defun cl-oracle--text-p (pos)
  "Non-nil when POS is inside a string or a #|...|# comment."
  (let ((s (cl-oracle--state pos)))
    (or (nth 3 s) (integerp (nth 4 s)))))

(defun cl-oracle--whitespace ()
  ;; `( foo' -> `(foo'
  (goto-char (point-min))
  (while (re-search-forward "(\\([ \t]+\\)[^ \t\n;]" nil t)
    (let ((b (match-beginning 1)) (e (match-end 1)))
      (if (cl-oracle--code-p (1- b))
          (progn (delete-region b e) (goto-char b))
        (goto-char e))))
  ;; `foo )' -> `foo)'; a lone `)' joins the line before.
  (goto-char (point-min))
  (while (re-search-forward "[ \t\n]+)" nil t)
    (let ((close (1- (point)))
          (b (match-beginning 0)))
      (when (cl-oracle--code-p close)
        ;; An escaped blank (`#\ ', `foo\ ') belongs to the token before.
        (when (nth 5 (cl-oracle--state b)) (setq b (1+ b)))
        (let ((s (cl-oracle--state b)))
          (unless (or (nth 4 s) (nth 3 s) (>= b close)
                      ;; A line of close parens with its own comment stays.
                      (and (save-excursion (goto-char b) (search-forward "\n" close t))
                           (save-excursion (goto-char close) (skip-chars-forward ") \t") (looking-at ";"))))
            (delete-region b close)
            (goto-char (1+ b)))))))
  ;; Trailing blanks, outside strings and #| comments.
  (goto-char (point-min))
  (while (re-search-forward "[ \t]+$" nil t)
    (unless (cl-oracle--text-p (match-beginning 0))
      (delete-region (match-beginning 0) (match-end 0))))
  ;; Runs of blank lines.
  (goto-char (point-min))
  (while (re-search-forward "\n\n\n\\(\n+\\)" nil t)
    (if (cl-oracle--text-p (1+ (match-beginning 0)))
        (goto-char (match-end 0))
      (delete-region (match-beginning 1) (match-end 1)))))

(defun cl-oracle--kept-lines ()
  "The lines indent-region must leave alone, with their text."
  (let (kept)
    (save-excursion
      (goto-char (point-min))
      (while (not (eobp))
        (let* ((bol (line-beginning-position))
               (s (cl-oracle--state bol)))
          (when (or (integerp (nth 4 s))
                    (and (not (nth 3 s))
                         (or (looking-at "[ \t]*;\\([^;]\\|$\\)")
                             (and (looking-at "[ \t]*#|")
                                  (< (line-end-position) (point-max))
                                  (integerp (nth 4 (cl-oracle--state (1+ (line-end-position)))))))))
            (push (cons (line-number-at-pos) (buffer-substring bol (line-end-position))) kept)))
        (forward-line 1)))
    kept))

(defun cl-oracle-run (file out)
  (let ((raw (equal (getenv "MEP_ORACLE_RAW") "1"))
        (coding-system-for-read 'utf-8-unix)
        (coding-system-for-write 'utf-8-unix))
    (with-temp-buffer
      (insert-file-contents file)
      (lisp-mode)
      (setq-local indent-tabs-mode nil)
      (setq-local lisp-indent-function 'common-lisp-indent-function)
      (if raw
          (indent-region (point-min) (point-max))
        (cl-oracle--whitespace)
        (let ((kept (cl-oracle--kept-lines)))
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
          (if (cl-oracle--text-p (match-beginning 0))
              (goto-char (match-end 0))
            (let ((col (current-column)))
              (delete-region (match-beginning 0) (match-end 0))
              (insert (make-string col ?\s)))))
        (goto-char (point-max))
        (skip-chars-backward " \t\n")
        (delete-region (point) (point-max))
        (unless (bobp) (insert "\n")))
      (if out
          (write-region (point-min) (point-max) out nil 'silent)
        (princ (buffer-string))))))

(when command-line-args-left
  (let ((file (pop command-line-args-left))
        (out (pop command-line-args-left)))
    (cl-oracle-run file out)
    (setq command-line-args-left nil)))

;;; cl_indent_oracle.el ends here
