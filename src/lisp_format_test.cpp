// Coverage for lisp_format.h/.cpp: mep's own Common Lisp formatter
// (Emacs's common-lisp-indent-function layout). Each case is an
// input/expected pair, checked once for the layout and once fed back
// through Format() to prove it is a fixed point. The expected outputs of
// the first group are what tools/cl_indent_oracle.el (Emacs 30's
// indent-region plus mep's whitespace rules) produces for the same input.
//
// `mep-lisp-format-test --format FILE...` prints each file formatted
// instead (the reason a file was left alone on stderr), which is how the
// formatter is compared with Emacs on a corpus
// (tools/cl_format_compare.py).

#include "lisp_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

int checks = 0;

void Same(const std::string &src, const std::string &want, const lispfmt::Options &options = {}) {
    checks++;
    const lispfmt::Result r = lispfmt::Format(src, options);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line, r.error.c_str(), src.c_str());
        std::abort();
    }
    if (r.text != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s", src.c_str(), want.c_str(),
                     r.text.c_str());
        std::abort();
    }
    const lispfmt::Result again = lispfmt::Format(r.text, options);
    if (!again.ok || again.text != r.text) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- first ---\n%s--- second ---\n%s", r.text.c_str(), again.text.c_str());
        std::abort();
    }
}

void Unchanged(const std::string &src) { Same(src, src); }

void Refused(const std::string &src, int line) {
    checks++;
    const lispfmt::Result r = lispfmt::Format(src);
    if (r.ok || r.text != src || r.error_line != line || r.error.empty()) {
        std::fprintf(stderr, "NOT REFUSED AS EXPECTED (want line %d, got ok=%d line %d: %s)\n%s", line, r.ok,
                     r.error_line, r.error.c_str(), src.c_str());
        std::abort();
    }
}

int FormatFiles(int argc, char **argv) {
    int status = 0;
    for (int k = 2; k < argc; k++) {
        std::ifstream in(argv[k], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const lispfmt::Result r = lispfmt::Format(ss.str());
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", argv[k], r.error_line, r.error.c_str());
            status = 1;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);

    // --- as Emacs lays them out (tools/cl_indent_oracle.el) ------------------------------
    // defun body two in
    Same("(defun foo (x)\n(let ((a 1)\n(b 2))\n(+ a b x)))\n",
         "(defun foo (x)\n  (let ((a 1)\n        (b 2))\n    (+ a b x)))\n");
    // call args under first
    Same("(foo a\nb\n   c)\n",
         "(foo a\n     b\n     c)\n");
    // call args one in when none on the head's line
    Same("(foo\na\nb)\n",
         "(foo\n a\n b)\n");
    // distinguished args four in
    Same("(defun foo\n(x)\nx)\n",
         "(defun foo\n    (x)\n  x)\n");
    // when: 1 distinguished
    Same("(when x\ny\nz)\n",
         "(when x\n  y\n  z)\n");
    // if: all alike
    Same("(if x\ny\nz)\n",
         "(if x\n    y\n    z)\n");
    // let bindings
    Same("(let ((a 1)\n(b 2))\na)\n",
         "(let ((a 1)\n      (b 2))\n  a)\n");
    // let bindings on their own line
    Same("(let\n((a 1))\na)\n",
         "(let\n    ((a 1))\n  a)\n");
    // flet local functions
    Same("(flet ((f (x)\nx)\n(g (y)\ny))\n(f 1))\n",
         "(flet ((f (x)\n         x)\n       (g (y)\n         y))\n  (f 1))\n");
    // cond clauses
    Same("(cond ((a b)\nc)\n(t\nd))\n",
         "(cond ((a b)\n       c)\n      (t\n       d))\n");
    // case clauses
    Same("(case x\n(1 a\nb)\n(t c))\n",
         "(case x\n  (1 a\n     b)\n  (t c))\n");
    // extended loop
    Same("(loop for x in y\ndo (foo x)\ncollect x)\n",
         "(loop for x in y\n      do (foo x)\n      collect x)\n");
    // simple loop
    Same("(loop\n(foo)\n(bar))\n",
         "(loop\n (foo)\n (bar))\n");
    // defmethod qualifier
    Same("(defmethod foo :around ((x t))\n      (call-next-method))\n",
         "(defmethod foo :around ((x t))\n  (call-next-method))\n");
    // defmethod lambda list on its own line
    Same("(defmethod foo :before\n  ((x t))\n   (bar))\n",
         "(defmethod foo :before\n    ((x t))\n  (bar))\n");
    // labels
    Same("(labels ((f (x)\n(g x))\n(g (x)\nx))\n(f 1))\n",
         "(labels ((f (x)\n           (g x))\n         (g (x)\n           x))\n  (f 1))\n");
    // defclass slots and options
    Same("(defclass point ()\n((x :initarg :x\n:reader x)\n(y :initarg :y))\n(:documentation \"A point.\"))\n",
         "(defclass point ()\n  ((x :initarg :x\n      :reader x)\n   (y :initarg :y))\n"
         "  (:documentation \"A point.\"))\n");
    // defpackage options
    Same("(defpackage :foo\n(:use :cl)\n(:export #:bar\n#:baz))\n",
         "(defpackage :foo\n  (:use :cl)\n  (:export #:bar\n           #:baz))\n");
    // defstruct
    Same("(defstruct (point (:constructor make-point (x y)))\nx\ny)\n",
         "(defstruct (point (:constructor make-point (x y)))\n  x\n  y)\n");
    // loop clause continued
    Same("(loop for x in y\ndo (foo a\nb))\n",
         "(loop for x in y\n      do (foo a\n              b))\n");
    // function lambda hack
    Same("(function (lambda (x)\nx))\n",
         "(function (lambda (x)\n  x))\n");
    // unwind-protect
    Same("(unwind-protect\n(foo)\n(bar))\n",
         "(unwind-protect\n     (foo)\n  (bar))\n");
    // nested quoted data
    Same("'((a 1)\n(b 2))\n",
         "'((a 1)\n  (b 2))\n");
    // lambda list keywords
    Same("(defun foo (a b &key c\nd)\na)\n",
         "(defun foo (a b &key c\n                  d)\n  a)\n");
    // lambda list keyword line
    Same("(defun foo (a b\n&optional c)\na)\n",
         "(defun foo (a b\n            &optional c)\n  a)\n");
    // quoted list
    Same("'(a b\nc d)\n",
         "'(a b\n  c d)\n");
    // vector
    Same("#(a b\nc)\n",
         "#(a b\n  c)\n");
    // backquote substitution keeps its spec
    Same("`(progn ,@(when x\n(list y)))\n",
         "`(progn ,@(when x\n            (list y)))\n");
    // keywords under keywords
    Same("(make-instance 'foo\n:a 1\n:b 2)\n",
         "(make-instance 'foo\n               :a 1\n               :b 2)\n");
    // keyword after keyword line
    Same("(foo :a 1\n:b 2)\n",
         "(foo :a 1\n     :b 2)\n");
    // tagbody
    Same("(tagbody\nstart\n(foo)\n(go start))\n",
         "(tagbody\n start\n   (foo)\n   (go start))\n");
    // def... is defun-like
    Same("(defknown foo (x)\nbar)\n",
         "(defknown foo (x)\n  bar)\n");
    // package prefix ignored
    Same("(sb-c:defknown foo (x)\nbar)\n",
         "(sb-c:defknown foo (x)\n  bar)\n");
    // with-... body
    Same("(with-open-file (s p)\n(read s))\n",
         "(with-open-file (s p)\n  (read s))\n");
    // destructuring-bind
    Same("(destructuring-bind (a b)\nx\n(list a b))\n",
         "(destructuring-bind (a b)\n    x\n  (list a b))\n");
    // multiple-value-bind
    Same("(multiple-value-bind\n(a b)\n(floor x)\na)\n",
         "(multiple-value-bind\n      (a b)\n    (floor x)\n  a)\n");
    // handler-case clauses
    Same("(handler-case (foo)\n(error (e)\n(print e)))\n",
         "(handler-case (foo)\n  (error (e)\n    (print e)))\n");
    // defgeneric :method
    Same("(defgeneric foo (x)\n(:method ((x t))\nx))\n",
         "(defgeneric foo (x)\n  (:method ((x t))\n    x))\n");
    // function lambda
    Same("(mapcar (lambda (x)\n(* x x))\nl)\n",
         "(mapcar (lambda (x)\n          (* x x))\n        l)\n");
    // do
    Same("(do ((i 0 (1+ i)))\n((= i 3))\n(print i))\n",
         "(do ((i 0 (1+ i)))\n    ((= i 3))\n  (print i))\n");
    // top level forms to column zero
    Same("  (foo)\n  (bar)\n",
         "(foo)\n(bar)\n");
    // no space inside parens
    Same("( foo  1 )\n",
         "(foo  1)\n");
    // lone close paren joins
    Same("(foo\n  bar\n  )\n",
         "(foo\n bar)\n");
    // close paren after a comment stays
    Same("(foo ; c\n)\n",
         "(foo ; c\n )\n");
    // close parens with their own comment stay
    Same("(progn\n(foo)\n) ; end of progn\n",
         "(progn\n  (foo)\n  ) ; end of progn\n");
    // trailing whitespace
    Same("(foo)   \n(bar)\t\n",
         "(foo)\n(bar)\n");
    // blank lines: at most two
    Same("(a)\n\n\n\n\n(b)\n",
         "(a)\n\n\n(b)\n");
    // end of file
    Same("(a)\n\n\n",
         "(a)\n");
    // tabs in a kept line's margin
    Same("(defun f ()\n\t;;; note\n  1)\n",
         "(defun f ()\n        ;;; note\n  1)\n");
    // string lines
    Same("(defun f ()\n\"First line\n   second\"\n1)\n",
         "(defun f ()\n  \"First line\n   second\"\n  1)\n");
    // block comment lines
    Same("(defun f ()\n#| one\n      two\n |#\n1)\n",
         "(defun f ()\n#| one\n      two\n |#\n  1)\n");
    // single-semicolon comment keeps its column
    Same("(defun f ()\n       ; margin\n1)\n",
         "(defun f ()\n       ; margin\n  1)\n");
    // double-semicolon comment indents
    Same("(defun f ()\n;; note\n1)\n",
         "(defun f ()\n  ;; note\n  1)\n");
    // character literals
    Same("(list #\\( #\\Space #\\)\na)\n",
         "(list #\\( #\\Space #\\)\n      a)\n");
    // bar symbols
    Same("(list |foo bar| ( baz )\na)\n",
         "(list |foo bar| (baz)\n      a)\n");
    // feature expressions
    Same("(list #+sbcl a\n#-(or ccl ecl) b)\n",
         "(list #+sbcl a\n      #-(or ccl ecl) b)\n");

    // --- unlike Emacs, deliberately ---------------------------------------------------------
    // A defmethod's qualifiers are counted in its own top-level form: Emacs
    // looks from the end of the sexp being indented, finds the body line
    // still in column 0, and stops with a scan error (or, given other code
    // below, indents the body as a lambda list until run a second time).
    Same("(defmethod foo :around ((x t))\n(call-next-method))\n",
         "(defmethod foo :around ((x t))\n  (call-next-method))\n");
    Same("(defmethod foo :before\n((x t))\n(bar))\n", "(defmethod foo :before\n    ((x t))\n  (bar))\n");
    // Nothing inside a `#|' comment moves, nor the line it starts on.
    Same("(progn\n      #| a\n  b |#\n(foo))\n", "(progn\n      #| a\n  b |#\n  (foo))\n");
    // But a `#|...|#' that closes on its line is code-like.
    Same("(progn\n      #|a|# (foo))\n", "(progn\n  #|a|# (foo))\n");

    // --- the caller's own specs -------------------------------------------------------------
    {
        lispfmt::Options o;
        o.indent["my-when"] = "1";
        o.indent["my-let"] = "let";
        o.indent["define-thing"] = "defun";
        o.indent["My-Body"] = "(&body)";
        o.indent["when"] = "";  // back to an ordinary call
        Same("(my-when x\ny)\n", "(my-when x\n  y)\n", o);
        Same("(my-let ((a 1)\n(b 2))\na)\n", "(my-let ((a 1)\n         (b 2))\n  a)\n", o);
        Same("(define-thing foo (x)\nx)\n", "(define-thing foo (x)\n  x)\n", o);
        Same("(my-body\nx)\n", "(my-body\n  x)\n", o);
        Same("(when x\ny)\n", "(when x\n      y)\n", o);
    }

    // --- refused ------------------------------------------------------------------------------
    Refused("(defun f (x)\n  (+ x 1)\n", 1);
    Refused("(a)\n(b))\n", 2);
    Refused("(a\n \"b)\n", 2);
    Refused("#| never closed\n(a)\n", 1);
    Refused("(list |a b)\n", 1);
    Unchanged("");
    Same("  \n\n", "");

    // --- one line at a time (the Enter key) ---------------------------------------------------
    checks++;
    CHECK(lispfmt::IndentLine("(defun f (x)\n", 1) == 2);
    CHECK(lispfmt::IndentLine("(foo bar\n", 1) == 5);
    CHECK(lispfmt::IndentLine("(foo\n", 1) == 1);
    CHECK(lispfmt::IndentLine("(let ((a 1))\n", 1) == 2);
    CHECK(lispfmt::IndentLine("(loop for x in y\n", 1) == 6);
    CHECK(lispfmt::IndentLine("(a)\n", 1) == 0);
    CHECK(lispfmt::IndentLine("(print \"abc\n", 1) == -1);  // inside a string
    CHECK(lispfmt::IndentLine("#| x\n", 1) == -1);          // inside a block comment
    // Re-indentation only: nothing but leading blanks changes.
    CHECK(lispfmt::Reindent("(defun f ( x )\n1 )   \n   \n") == "(defun f ( x )\n  1 )   \n\n");

    std::printf("mep-lisp-format-test: %d checks passed\n", checks);
    return 0;
}
