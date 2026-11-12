// Windowless test for lisp_reader.cpp, lisp_indent.cpp and lisp_lsp.cpp
// -- the reading, indenting and analysis halves of mep's own Common Lisp
// language server (`mep-lisp-lsp`, src/lisp_lsp_server.cpp). Drives the
// pure functions directly: no process, no JSON-RPC client, no GL context
// and no Lisp installation. CHECK(), never assert(): the Release build
// strips assert() entirely.
//
// The conventions are maxima_lsp_test.cpp's:
//   - Diagnostics are asserted by LispLspDiagnostic::code, never by
//     message wording, so the messages stay free to improve.
//   - Every check asserts the *absence* of a diagnostic on the
//     legal-but-suspicious shapes it must stay quiet about, as
//     deliberately as it asserts the true reports. A linter that cries
//     wolf on idiomatic Lisp gets switched off.
//   - The reader is asserted through the shape of what it read, because
//     Lisp's token rules (`1.` is an integer, `a#b` one symbol, `#\(` a
//     character) are exactly where a plausible reader goes wrong.
#include "lisp_lsp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <stdlib.h>
#define getpid _getpid
static int setenv(const char *k, const char *v, int) { return _putenv_s(k, v); }
static int unsetenv(const char *k) { return _putenv_s(k, ""); }
#else
#include <unistd.h>
#endif

#include "lisp_indent.h"
#include "lisp_lsp_library.h"
#include "lisp_reader.h"

namespace {
int g_checks_run = 0;

void Check(bool condition, const char *expression, int line) {
    g_checks_run++;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

size_t CountCode(const std::vector<LispLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const LispLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

bool HasCode(const Lines &lines, const std::string &code) { return CountCode(LispLspDiagnostics(lines), code) > 0; }

LispLspDiagnostic FindCode(const Lines &lines, const std::string &code) {
    for (const LispLspDiagnostic &d : LispLspDiagnostics(lines)) {
        if (d.code == code) return d;
    }
    return LispLspDiagnostic{};
}

bool ReadErrorCode(const Lines &lines, const std::string &code) {
    for (const LispReadError &e : LispRead(lines).errors) {
        if (e.code == code) return true;
    }
    return false;
}

/** @brief Renders a node back as canonical text, so a read's shape is visible. */
std::string Render(const LispReadResult &r, int node) {
    const LispNode &n = r.at(node);
    auto list = [&](const char *open) {
        std::string out = open;
        for (size_t i = 0; i < n.kids.size(); i++) {
            if (i > 0) out += ' ';
            if (n.dotted && i + 1 == n.kids.size()) out += ". ";
            out += Render(r, n.kids[i]);
        }
        return out + ")";
    };
    switch (n.kind) {
        case LispNodeKind::Symbol:
            if (n.uninterned) return "#:" + n.name;
            if (n.package.empty()) return n.name;
            if (n.package == "KEYWORD") return ":" + n.name;
            return n.package + (n.internal ? "::" : ":") + n.name;
        case LispNodeKind::String: return "\"" + n.name + "\"";
        case LispNodeKind::List: return list("(");
        case LispNodeKind::Vector: return list("#(");
        case LispNodeKind::Quote:
        case LispNodeKind::Backquote:
        case LispNodeKind::Unquote:
        case LispNodeKind::Splice:
        case LispNodeKind::Function:
        case LispNodeKind::ReadEval:
        case LispNodeKind::Dispatch: return n.text + (n.kids.empty() ? "" : Render(r, n.kids[0]));
        case LispNodeKind::Feature:
            return n.text + (n.kids.empty() ? "" : Render(r, n.kids[0])) + " " +
                   (n.kids.size() > 1 ? Render(r, n.kids[1]) : "");
        default: return n.text;
    }
}

std::string ReadOne(const std::string &text) {
    const LispReadResult r = LispRead({text});
    if (r.top.empty()) return "<nothing>";
    std::string out;
    for (size_t i = 0; i < r.top.size(); i++) {
        if (i > 0) out += " | ";
        out += Render(r, r.top[i]);
    }
    return out;
}

LispNodeKind KindOf(const std::string &text) {
    const LispReadResult r = LispRead({text});
    return r.top.empty() ? LispNodeKind::Error : r.at(r.top[0]).kind;
}

// --- Reader -------------------------------------------------------------

void TestReaderAtoms() {
    CHECK(ReadOne("(a b . c)") == "(A B . C)");
    CHECK(ReadOne("#(1 2 3)") == "#(1 2 3)");
    CHECK(ReadOne("'x `(a ,b ,@c) #'f") == "'X | `(A ,B ,@C) | #'F");
    CHECK(ReadOne("\"a\\\"b\\\\c\"") == "\"a\"b\\c\"");
    // Numbers, by the standard's token syntax.
    for (const char *num : {"42", "-17", "+3", "1.", "-1/2", "3/4", "1.5", ".5", "1e10", "1.5d0", "-2.5f-3", "6.02E23",
                            "#x1F", "#b101", "#o17", "#36rZZ"}) {
        CHECK(KindOf(num) == LispNodeKind::Number);
    }
    for (const char *sym : {"1+", "1-", "+", "-", "...a", "a.b", "1a", "a#b", "+inf", "1/2/3", ".5.", "e10"}) {
        CHECK(KindOf(sym) == LispNodeKind::Symbol);
    }
    CHECK(LispIsNumberToken("1.") && !LispIsNumberToken("."));
    // Characters: the first character is taken whatever it is.
    CHECK(ReadOne("(#\\( #\\) #\\; #\\\" #\\Space #\\a)") == "(#\\( #\\) #\\; #\\\" #\\Space #\\a)");
    CHECK(KindOf("#\\Newline") == LispNodeKind::Character);
    // Symbols: case, packages, escapes.
    CHECK(ReadOne("foo") == "FOO");
    CHECK(ReadOne("cl:car") == "CL:CAR");
    CHECK(ReadOne("pkg::internal") == "PKG::INTERNAL");
    CHECK(ReadOne(":key") == ":KEY");
    CHECK(ReadOne("#:gensym") == "#:GENSYM");
    CHECK(ReadOne("|Foo Bar|") == "Foo Bar");
    CHECK(ReadOne("a\\ b") == "A B");
    CHECK(ReadOne("|a|b") == "aB");
    {
        const LispReadResult r = LispRead({"(defun Foo-Bar ())"});
        const LispNode &name = r.at(r.at(r.top[0]).kids[1]);
        CHECK(name.name == "FOO-BAR");
        CHECK(name.text == "Foo-Bar");  // the original spelling survives
    }
    {
        const LispReadResult r = LispRead({"  alexandria:flatten"});
        const LispNode &s = r.at(r.top[0]);
        CHECK(s.package == "ALEXANDRIA" && s.name == "FLATTEN");
        CHECK(s.range.col == 2 && s.name_col == 13);
    }
    // Dispatch macros that take a form.
    CHECK(ReadOne("#p\"/tmp/x\"") == "#P\"/tmp/x\"");
    CHECK(ReadOne("#S(point :x 1)") == "#S(POINT :X 1)");
    CHECK(ReadOne("#2A((1 2) (3 4))") == "#2A((1 2) (3 4))");
    CHECK(ReadOne("#c(1 2)") == "#C(1 2)");
    CHECK(ReadOne("#1=(a . #1#)") == "#1=(A . #1#)");
    CHECK(ReadOne("#.(+ 1 2)") == "#.(+ 1 2)");
    CHECK(KindOf("#*1011") == LispNodeKind::BitVector);
    // SBCL's package-local form syntax.
    CHECK(KindOf("sb-kernel::(foo bar)") == LispNodeKind::Dispatch);
}

void TestReaderComments() {
    const LispReadResult r = LispRead({"; one", "a #| outer #| inner |# still |# b", "#|", "x|#", "c"});
    CHECK(r.ok());
    CHECK(r.top.size() == 3);
    CHECK(r.comments.size() == 3);
    CHECK(r.comments[1].block && r.comments[2].block);
    CHECK(r.comments[2].range.line == 2 && r.comments[2].range.end_line == 3);
    // A shebang line is a comment.
    CHECK(LispRead({"#!/usr/bin/sbcl --script", "(print 1)"}).ok());
}

void TestReaderFeatures() {
    const LispReadResult r = LispRead({"(list #+sbcl a #-(or ccl ecl) b #+(or) (broken #< form))"});
    CHECK(r.ok());  // nothing inside a feature-conditional form is a reader error
    const LispNode &l = r.at(r.top[0]);
    CHECK(l.kids.size() == 4);
    const LispNode &f = r.at(l.kids[1]);
    CHECK(f.kind == LispNodeKind::Feature && f.text == "#+");
    CHECK(r.at(f.kids[0]).package == "KEYWORD");  // features read as keywords
    CHECK(r.at(f.kids[1]).conditional);
    CHECK(Render(r, l.kids[2]) == "#-(:OR :CCL :ECL) B");
}

void TestReaderErrors() {
    // Every one recorded, none fatal.
    CHECK(ReadErrorCode({"(defun f (x)", "  (+ x 1)"}, "unbalanced-paren"));
    CHECK(ReadErrorCode({"(a))"}, "stray-close-paren"));
    CHECK(ReadErrorCode({"(print \"abc)"}, "unterminated-string"));
    CHECK(ReadErrorCode({"#| never closed", "(a)"}, "unterminated-comment"));
    CHECK(ReadErrorCode({"|abc"}, "unterminated-escape"));
    CHECK(ReadErrorCode({"#?foo"}, "unknown-dispatch"));
    CHECK(ReadErrorCode({"#xZZ"}, "bad-number"));
    CHECK(ReadErrorCode({"1/0"}, "bad-number"));
    CHECK(ReadErrorCode({"#b102"}, "bad-number"));
    CHECK(ReadErrorCode({"a:b:c"}, "bad-symbol"));
    CHECK(ReadErrorCode({"(. a)"}, "bad-dot"));
    CHECK(ReadErrorCode({"(a . b c)"}, "bad-dot"));
    CHECK(ReadErrorCode({".."}, "bad-dot"));
    CHECK(ReadErrorCode({"(list ,x)"}, "comma-outside-backquote"));
    CHECK(!ReadErrorCode({"`(list ,x ,@y)"}, "comma-outside-backquote"));
    CHECK(ReadErrorCode({"(quote ')"}, "missing-form"));
    CHECK(ReadErrorCode({"#<foo>"}, "unreadable-object"));
    CHECK(ReadErrorCode({"#\\Spcae"}, "unknown-character-name"));
    CHECK(!ReadErrorCode({"#\\Greek_Small_Letter_Alpha #\\U+03BB #\\Caron #\\Nul"}, "unknown-character-name"));
    // A dotted pair with platform-dependent tails is fine.
    CHECK(LispRead({"(a . #+sbcl (b) #-sbcl (c))"}).ok());

    // Recovery: a missing paren does not swallow the next definition.
    const LispReadResult r = LispRead({"(defun f (x)", "  (let ((y x))", "    y)", "(defun g () 2)"});
    CHECK(r.top.size() == 2);
    CHECK(r.errors.size() == 1 && r.errors[0].code == "unbalanced-paren");
    CHECK(r.errors[0].range.line == 0);
    // ... but a balanced file that writes a definition at column 0 inside
    // an eval-when means it: no recovery, no error.
    const LispReadResult ok = LispRead({"(eval-when (:compile-toplevel)", "(defun h () 1))"});
    CHECK(ok.ok() && ok.top.size() == 1);

    // Nesting far past the limit costs a diagnostic, not the stack.
    std::string deep(5000, '(');
    deep += std::string(5000, ')');
    const LispReadResult d = LispRead({deep});
    bool too_deep = false;
    for (const LispReadError &e : d.errors) too_deep = too_deep || e.code == "too-deep";
    CHECK(too_deep);
    // Garbage never crashes.
    LispRead({"#", "#\\", "\"", "|", ")))", "#+", "'", ",@", "#1=", "#(", "(."});
}

// --- Diagnostics ---------------------------------------------------------

void TestUnusedVariables() {
    CHECK(HasCode({"(defun f (x y) x)"}, "unused-variable"));
    CHECK(FindCode({"(defun f (x y) x)"}, "unused-variable").range.col == 12);
    CHECK(!HasCode({"(defun f (x y) (declare (ignore y)) x)"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (x y) (declare (ignorable y)) x)"}, "unused-variable"));
    CHECK(HasCode({"(let ((a 1) (b 2)) a)"}, "unused-variable"));
    CHECK(HasCode({"(lambda (x) 1)"}, "unused-variable"));
    CHECK(HasCode({"(multiple-value-bind (q r) (floor 7 2) q)"}, "unused-variable"));
    CHECK(HasCode({"(handler-case (foo) (error (e) nil))"}, "unused-variable"));
    CHECK(HasCode({"(defun f (&optional (x 1 x-p)) x)"}, "unused-variable"));  // the supplied-p
    CHECK(HasCode({"(let ((x 1)) (setq x 2))"}, "unused-variable"));          // assigned, never read
    // Quiet on the idiomatic shapes.
    CHECK(!HasCode({"(defmethod area ((s square) unused) 0)"}, "unused-variable"));  // method parameters
    CHECK(!HasCode({"(dolist (x '(1 2)) (print 1))"}, "unused-variable"));
    CHECK(!HasCode({"(dotimes (i 3) (print 1))"}, "unused-variable"));
    CHECK(!HasCode({"(loop for x in '(1 2) count t)"}, "unused-variable"));
    CHECK(!HasCode({"(let ((*print-pretty* nil)) (print 1))"}, "unused-variable"));  // a special binding
    CHECK(!HasCode({"(defvar *x* 1)", "(defun f () (let ((*x* 2)) (g)))"}, "unused-variable"));
    CHECK(!HasCode({"(defmacro m (a) `(list ,a))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (x) (other-pkg:with-thing (x) 1))"}, "unused-variable"));  // a foreign macro
    CHECK(!HasCode({"(defun f (key s) (as-member (key s) 1))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (x) #+ccl (ccl:frob x) 1)"}, "unused-variable"));  // used on another platform
    CHECK(!HasCode({"(defun f (x) (let ((y x)) (declare (special y)) (g)))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (_x) 1)"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (n) (loop for i from 1 upto n collect i))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (count) (loop for i below count collect i))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (x) (macrolet ((m () 'x)) (m)))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (s) (pprint-logical-block (s nil) (write 1 :stream s)))"}, "unused-variable"));
    CHECK(!HasCode({"(defun f (x) (with-foo-bar () 1))"}, "unused-variable"));  // may capture x by name
    // An ignored variable that is used.
    CHECK(HasCode({"(defun f (x) (declare (ignore x)) x)"}, "used-ignored-variable"));
    CHECK(!HasCode({"(defun f (x) (declare (ignorable x)) x)"}, "used-ignored-variable"));
    // Local functions.
    CHECK(HasCode({"(defun f () (flet ((g () 1)) 2))"}, "unused-function"));
    CHECK(!HasCode({"(defun f () (flet ((g () 1)) (g)))"}, "unused-function"));
    CHECK(!HasCode({"(defun f () (labels ((g () 1)) (mapcar #'g '(1))))"}, "unused-function"));
}

void TestShapeDiagnostics() {
    CHECK(HasCode({"(defun f (x x) x)"}, "duplicate-parameter"));
    CHECK(HasCode({"(defun f (a &key a) a)"}, "duplicate-parameter"));
    CHECK(!HasCode({"(defun f (a &aux (a (list a))) a)"}, "duplicate-parameter"));  // &aux rebinds legally
    CHECK(HasCode({"(let ((x 1) (x 2)) x)"}, "duplicate-binding"));
    CHECK(!HasCode({"(let* ((x 1) (x (1+ x))) x)"}, "duplicate-binding"));
    CHECK(HasCode({"(let ((x 1 2)) x)"}, "malformed-binding"));
    CHECK(HasCode({"(let ((1 x)) x)"}, "malformed-binding"));
    CHECK(HasCode({"(let x x)"}, "malformed-binding"));
    CHECK(!HasCode({"(let (x (y) (z 1)) (list x y z))"}, "malformed-binding"));
    CHECK(!HasCode({"(let ((x #+sbcl 1 #-sbcl 2)) x)"}, "malformed-binding"));
    CHECK(HasCode({"(let ((t 1)) t)"}, "bind-constant"));
    CHECK(HasCode({"(defun f (:key) 1)"}, "bind-constant"));
    CHECK(HasCode({"(defun f (&key a &optional b) (list a b))"}, "malformed-lambda-list"));
    CHECK(HasCode({"(defun f (&rest) 1)"}, "malformed-lambda-list"));
    CHECK(HasCode({"(defun f (&rest a b) (list a b))"}, "malformed-lambda-list"));
    CHECK(!HasCode({"(defmacro m ((a b) &body body &environment env) (list a b body env))"}, "malformed-lambda-list"));
    CHECK(HasCode({"(defun f (a &optional b &key c) (list a b c))"}, "optional-and-key"));
    CHECK(FindCode({"(defun f (a &optional b &key c) (list a b c))"}, "optional-and-key").severity ==
          LispLspSeverity::Information);
    CHECK(!HasCode({"(defun f (a &optional b) (list a b))"}, "optional-and-key"));
    CHECK(HasCode({"(if a b c d)"}, "if-arity"));
    CHECK(HasCode({"(if a)"}, "if-arity"));
    CHECK(!HasCode({"(if a b)"}, "if-arity"));
    CHECK(!HasCode({"(if a #+sbcl b #-sbcl c d)"}, "if-arity"));
    CHECK(HasCode({"(setf a 1 b)"}, "odd-setf-args"));
    CHECK(!HasCode({"(setf a 1 b 2)"}, "odd-setf-args"));
    CHECK(HasCode({"(defun f (x) (print x) (declare (fixnum x)) x)"}, "misplaced-declare"));
    CHECK(HasCode({"(progn (declare (special x)) 1)"}, "misplaced-declare"));
    CHECK(HasCode({"(when t (declare (optimize speed)) 1)"}, "misplaced-declare"));
    CHECK(!HasCode({"(defun f (x) \"Doc.\" (declare (fixnum x)) x)"}, "misplaced-declare"));
    CHECK(!HasCode({"(defun f (x) (declare (fixnum x)) \"Doc.\" x)"}, "misplaced-declare"));
    CHECK(!HasCode({"(let ((x 1)) (declare (fixnum x)) (declare (optimize speed)) x)"}, "misplaced-declare"));
    CHECK(!HasCode({"(dolist (x l) #-genera (declare (string x)) (print x))"}, "misplaced-declare"));
    CHECK(!HasCode({"(defun f (x) (declare #.*opt*) x)"}, "misplaced-declare"));
    CHECK(!HasCode({"(my:with-thing (x) (declare (ignore x)) 1)"}, "misplaced-declare"));
    CHECK(!HasCode({"(with-open-file (s \"f\") (declare (stream s)) (read s))"}, "misplaced-declare"));
}

void TestCallDiagnostics() {
    // CL functions, from the generated lambda lists.
    CHECK(HasCode({"(car)"}, "wrong-arg-count"));
    CHECK(HasCode({"(car a b)"}, "wrong-arg-count"));
    CHECK(HasCode({"(gethash k)"}, "wrong-arg-count"));
    CHECK(!HasCode({"(gethash k h)", "(gethash k h 0)", "(list)", "(+ 1 2 3 4)", "(format t \"~a\" 1 2)"},
                   "wrong-arg-count"));
    CHECK(HasCode({"(make-hash-table :test)"}, "odd-keyword-args"));
    CHECK(!HasCode({"(make-hash-table :test 'equal :size 10)"}, "odd-keyword-args"));
    CHECK(HasCode({"(write-string s :start 1)"}, "odd-keyword-args"));  // the &optional stream ate :start
    CHECK(!HasCode({"(list 1 #+sbcl 2)", "(car #+sbcl a #-sbcl b)"}, "wrong-arg-count"));
    // File-local definitions.
    const Lines local = {"(defun area (w h &optional (d 1)) (* w h d))", "(area 1)", "(area 1 2)", "(area 1 2 3)",
                         "(area 1 2 3 4)"};
    CHECK(CountCode(LispLspDiagnostics(local), "wrong-arg-count") == 2);
    CHECK(HasCode({"(defun f (&key a b) (list a b))", "(f :c 1)"}, "unknown-keyword"));
    CHECK(!HasCode({"(defun f (&key a b) (list a b))", "(f :a 1 :b 2)"}, "unknown-keyword"));
    CHECK(!HasCode({"(defun f (&key a &allow-other-keys) a)", "(f :c 1)"}, "unknown-keyword"));
    CHECK(!HasCode({"(defun f (&key a) a)", "(f :c 1 :allow-other-keys t)"}, "unknown-keyword"));
    CHECK(!HasCode({"(defun f (&key ((:the-key k))) k)", "(f :the-key 1)"}, "unknown-keyword"));
    CHECK(HasCode({"(defmacro m (a &body b) (list a b))", "(m)"}, "wrong-arg-count"));
    CHECK(!HasCode({"(defmacro m (a &body b) (list a b))", "(m 1 2 3 4)"}, "wrong-arg-count"));
    CHECK(HasCode({"(defun f () (flet ((g (x) x)) (g)))"}, "wrong-arg-count"));
    // A name defined twice has no single lambda list to check against.
    CHECK(!HasCode({"#+sbcl (defun f (a) a)", "#-sbcl (defun f (a b) (list a b))", "(f 1 2)"}, "wrong-arg-count"));
    // Structures and classes define functions too.
    CHECK(HasCode({"(defstruct point x y)", "(point-x)"}, "wrong-arg-count"));
    CHECK(HasCode({"(defstruct point x y)", "(make-point :z 1)"}, "unknown-keyword"));
    CHECK(!HasCode({"(defstruct point x y)", "(make-point :x 1 :y 2)", "(point-p 1)", "(copy-point p)"},
                   "wrong-arg-count"));
    CHECK(!HasCode({"(defstruct (point (:constructor new-point (x y))) x y)", "(new-point 1 2)"}, "wrong-arg-count"));

    // eq on numbers and characters.
    CHECK(HasCode({"(eq x 1.5)"}, "eq-on-number"));
    CHECK(FindCode({"(eq x 1.5)"}, "eq-on-number").severity == LispLspSeverity::Warning);
    CHECK(FindCode({"(eq x 1)"}, "eq-on-number").severity == LispLspSeverity::Information);
    CHECK(HasCode({"(eq #\\a c)"}, "eq-on-number"));
    CHECK(!HasCode({"(eql x 1)", "(eq x 'a)", "(eq x nil)"}, "eq-on-number"));
    // Quoting what evaluates to itself.
    CHECK(HasCode({"(print ':foo)"}, "quoted-self-evaluating"));
    CHECK(HasCode({"(print '42)"}, "quoted-self-evaluating"));
    CHECK(HasCode({"(print '\"s\")"}, "quoted-self-evaluating"));
    CHECK(!HasCode({"(print 'foo)", "(print '(1 2))", "(print 'nil)", "`(a ',b)"}, "quoted-self-evaluating"));
    CHECK(!HasCode({"(print '(:a 1))"}, "quoted-self-evaluating"));
    // case keys.
    CHECK(HasCode({"(case x ('a 1))"}, "quoted-case-key"));
    CHECK(HasCode({"(case x (('a 'b) 1))"}, "quoted-case-key"));
    CHECK(!HasCode({"(case x (a 1) ((b c) 2) (otherwise 3))"}, "quoted-case-key"));
    // Redefinition.
    CHECK(HasCode({"(defun f () 1)", "(defun f () 2)"}, "redefinition"));
    CHECK(FindCode({"(defun f () 1)", "(defun f () 2)"}, "redefinition").range.line == 1);
    CHECK(!HasCode({"#+sbcl (defun f () 1)", "#-sbcl (defun f () 2)"}, "redefinition"));
    CHECK(!HasCode({"(defgeneric f (x))", "(defmethod f ((x t)) 1)", "(defmethod f ((x integer)) 2)"},
                   "redefinition"));
    CHECK(!HasCode({"(in-package :a)", "(defun f () 1)", "(in-package :b)", "(defun f () 2)"}, "redefinition"));
}

void TestNamingDiagnostics() {
    CHECK(HasCode({"(defvar counter 0)"}, "defvar-naming"));
    CHECK(FindCode({"(defvar counter 0)"}, "defvar-naming").severity == LispLspSeverity::Hint);
    CHECK(!HasCode({"(defvar *counter* 0)", "(defparameter *limit* 10)"}, "defvar-naming"));
    CHECK(HasCode({"(defconstant pi2 6.28)"}, "defconstant-naming"));
    CHECK(!HasCode({"(defconstant +pi2+ 6.28)"}, "defconstant-naming"));
    // A file with its own consistent house style is not nagged.
    CHECK(!HasCode({"(defconstant a 1)", "(defconstant b 2)", "(defconstant c 3)"}, "defconstant-naming"));

    const Lines pkg = {"(defpackage :demo (:use :cl))", "(in-package :demo)"};
    auto with_pkg = [&pkg](const Lines &body) {
        Lines all = pkg;
        all.insert(all.end(), body.begin(), body.end());
        return all;
    };
    CHECK(HasCode(with_pkg({"(defun f () (let ((*depth* 0)) (g)))", "(defun g () 1)"}), "lexical-earmuffs"));
    CHECK(!HasCode(with_pkg({"(defvar *depth* 0)", "(defun f () (let ((*depth* 1)) (g)))", "(defun g () 1)"}),
                   "lexical-earmuffs"));
    CHECK(!HasCode(with_pkg({"(defun f () (let ((*print-base* 16)) (print 1)))"}), "lexical-earmuffs"));
    // Unknown in CL-USER: an implementation's special may be inherited.
    CHECK(!HasCode({"(defun f () (let ((*stepper-hook* 0)) (g)))", "(defun g () 1)"}, "lexical-earmuffs"));
}

void TestNameCalibration() {
    const Lines own = {"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f () (frobnicate 1))"};
    CHECK(HasCode(own, "undefined-function"));
    // CL-USER: only a near-miss of a known name is reported there.
    CHECK(HasCode({"(defun f (l) (mapcr #'1+ l))"}, "undefined-function"));
    CHECK(FindCode({"(defun f (l) (mapcr #'1+ l))"}, "undefined-function").message.find("Did you mean `mapc") !=
          std::string::npos);
    CHECK(!HasCode({"(defun f () (frobnicate 1))"}, "undefined-function"));
    CHECK(!HasCode({"(exit)", "(sb-ext:quit)"}, "undefined-function"));
    // The file's package is defined elsewhere: nothing is known.
    CHECK(!HasCode({"(in-package :somewhere)", "(defun f () (frobnicate 1))"}, "undefined-function"));
    // Uses another package: nothing is known.
    CHECK(!HasCode({"(defpackage :demo (:use :cl :alexandria))", "(in-package :demo)", "(defun f () (flatten 1))"},
                   "undefined-function"));
    // :import-from names are known.
    CHECK(!HasCode({"(defpackage :demo (:use :cl) (:import-from :alexandria #:flatten))", "(in-package :demo)",
                    "(defun f () (flatten 1))"},
                   "undefined-function"));
    // Defined anywhere in the file, in any order, or locally.
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f () (g) (h))",
                    "(defun g () 1)", "(defmacro h () 2)", "(defun k () (flet ((local () 1)) (local)))"},
                   "undefined-function"));
    // More than five unknown names: part of something bigger.
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f () (a1) (a2) (a3) (a4) (a5) (a6))"},
                   "undefined-function"));
    // Defined dynamically.
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)",
                    "(defmacro define-thing (n) `(defun ,n () 1))", "(define-thing zap)", "(defun f () (zap))"},
                   "undefined-function"));
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(setf (fdefinition 'zap) #'car)",
                    "(defun f () (zap 1))"},
                   "undefined-function"));
    // Not under a feature for another platform.
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f () #+ccl (frob 1) 2)"},
                   "undefined-function"));
    // CALL-NEXT-METHOD is every method's.
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defmethod m (x) (call-next-method))"},
                   "undefined-function"));

    // setq of a name nothing binds.
    CHECK(HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f () (setq total 1))"},
                  "setq-undefined"));
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defvar total 0)",
                    "(defun f () (setq total 1))"},
                   "setq-undefined"));
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f (x) (setq x 1) x)"},
                   "setq-undefined"));
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)", "(defun f () (setf *elsewhere* 1))"},
                   "setq-undefined"));
    CHECK(!HasCode({"(defpackage :demo (:use :cl))", "(in-package :demo)",
                    "(defun f (o) (with-slots (total) o (setf total 1)))"},
                   "setq-undefined"));
    CHECK(!HasCode({"(in-package :elsewhere)", "(defun f () (setq total 1))"}, "setq-undefined"));
}

void TestReaderDiagnosticsAndReadtables() {
    CHECK(HasCode({"(defun f ("}, "unbalanced-paren"));
    CHECK(HasCode({"#[1 2]"}, "unknown-dispatch"));
    // A file that installs its own reader macros reads them fine.
    CHECK(!HasCode({"(named-readtables:in-readtable :interpol)", "#?\"x\""}, "unknown-dispatch"));
    CHECK(!HasCode({"(set-dispatch-macro-character #\\# #\\? #'reader)", "#?x"}, "unknown-dispatch"));
    CHECK(!HasCode({"(ccl::foo #_malloc #$O_RDONLY)"}, "unknown-dispatch"));
}

void TestCleanFile() {
    // An idiomatic file is entirely quiet.
    const Lines clean = {
        ";;;; demo.lisp",
        "(defpackage #:demo",
        "  (:use #:cl)",
        "  (:export #:area #:shape))",
        "(in-package #:demo)",
        "",
        "(defvar *registry* (make-hash-table :test 'equal))",
        "(defconstant +golden+ 1.618d0)",
        "",
        "(defclass shape ()",
        "  ((name :initarg :name :initform \"?\" :accessor shape-name)))",
        "",
        "(defgeneric area (shape)",
        "  (:documentation \"The area.\")",
        "  (:method ((s shape)) (declare (ignore s)) 0))",
        "",
        "(defstruct (circle (:conc-name c-)) (r 1 :type real))",
        "",
        "(defmethod area :around ((s shape))",
        "  (let ((result (call-next-method)))",
        "    (if (numberp result) result 0)))",
        "",
        "(defun register (s &key (key (shape-name s)) force)",
        "  \"Remember S.\"",
        "  (declare (type shape s))",
        "  (when (or force (not (gethash key *registry*)))",
        "    (setf (gethash key *registry*) s))",
        "  (loop for k being the hash-keys of *registry* using (hash-value v)",
        "        when (eq v s) collect k into keys",
        "        finally (return (values keys (length keys)))))",
        "",
        "(defun circle-area (c)",
        "  (* pi (expt (c-r c) 2)))",
        "",
        "(defun walk (tree fn)",
        "  (labels ((rec (x depth)",
        "             (cond ((null x) nil)",
        "                   ((consp x) (rec (car x) (1+ depth)) (rec (cdr x) depth))",
        "                   (t (funcall fn x depth)))))",
        "    (rec tree 0)))",
        "",
        "(defun safe-div (a b)",
        "  (handler-case (/ a b)",
        "    (division-by-zero () nil)",
        "    (:no-error (q) q)))",
        "",
        "(defun parse (string)",
        "  (with-input-from-string (in string)",
        "    (destructuring-bind (op &rest args) (read in)",
        "      (multiple-value-bind (n ok) (values (length args) t)",
        "        (list op n ok)))))",
        "",
        "(defmacro with-timing ((var) &body body)",
        "  (let ((start (gensym)))",
        "    `(let ((,start (get-internal-real-time)))",
        "       (multiple-value-prog1 (progn ,@body)",
        "         (setf ,var (- (get-internal-real-time) ,start))))))",
        "",
        "(defun timed (thunk)",
        "  (let ((elapsed 0))",
        "    (with-timing (elapsed) (funcall thunk))",
        "    elapsed))",
        "",
        "(defun counts (list)",
        "  (let ((table (make-hash-table)))",
        "    (dolist (x list table)",
        "      (incf (gethash x table 0)))))",
        "",
        "(defun f2 (x)",
        "  (block outer",
        "    (tagbody",
        "     top",
        "       (when (> x 10) (return-from outer x))",
        "       (incf x)",
        "       (go top))))",
        "",
        "(defun classify (x)",
        "  (typecase x",
        "    (integer :int)",
        "    #+sbcl (sb-ext:word :word)",
        "    (t (ecase (type-of x) (symbol :sym)))))",
        "",
        "(defun do-sum (n)",
        "  (do ((i 0 (1+ i))",
        "       (acc 0 (+ acc i)))",
        "      ((>= i n) acc)))",
    };
    const std::vector<LispLspDiagnostic> diags = LispLspDiagnostics(clean);
    for (const LispLspDiagnostic &d : diags) std::fprintf(stderr, "unexpected: %s\n", LispLspFormatDiagnostic("clean", d).c_str());
    CHECK(diags.empty());
}

// --- Editor features -------------------------------------------------------

void TestSymbols() {
    const Lines doc = {
        ";;;; Shapes",                                         // 0
        "(defclass shape () ((name :accessor shape-name) w))",  // 1
        "(defstruct point x (y 0))",                            // 2
        ";;;; Functions",                                      // 3
        "(defun area (s) s)",                                   // 4
        "(defmethod area ((s point)) 0)",                       // 5
        "(defgeneric perimeter (s))",                           // 6
        "(defmacro with-x (&body b) b)",                        // 7
        "(defvar *v* 1)",                                       // 8
        "(defconstant +c+ 2)",                                  // 9
        "(defun (setf thing) (v o) (list v o))",                // 10
        "(eval-when (:compile-toplevel) (defun hidden () 1))",  // 11
        "#+(or) (defun dead () 1)",                             // 12
        "(define-condition oops (error) ())",                   // 13
        "(deftype index () 'fixnum)",                           // 14
        "(in-package :foo)",                                    // 15
    };
    const std::vector<LispLspSymbol> syms = LispLspSymbols(doc);
    auto find = [&syms](const std::string &name) -> const LispLspSymbol * {
        for (const LispLspSymbol &s : syms) {
            if (s.name == name) return &s;
        }
        return nullptr;
    };
    CHECK(find("Shapes") != nullptr && find("Shapes")->kind == LispLspSymbolKind::Namespace);
    CHECK(find("Functions") != nullptr);
    CHECK(find("shape") != nullptr && find("shape")->kind == LispLspSymbolKind::Class);
    const LispLspSymbol *name_slot = find("name");
    CHECK(name_slot != nullptr && name_slot->kind == LispLspSymbolKind::Field);
    CHECK(syms[static_cast<size_t>(name_slot->parent)].name == "shape");
    CHECK(find("w") != nullptr && find("w")->kind == LispLspSymbolKind::Field);
    CHECK(find("point") != nullptr && find("point")->kind == LispLspSymbolKind::Struct);
    CHECK(find("y") != nullptr && find("y")->kind == LispLspSymbolKind::Field);
    const LispLspSymbol *area = find("area");
    CHECK(area != nullptr && area->kind == LispLspSymbolKind::Function && area->detail == "(s)");
    CHECK(syms[static_cast<size_t>(area->parent)].name == "Functions");
    bool method = false;
    for (const LispLspSymbol &s : syms) {
        if (s.name == "area" && s.kind == LispLspSymbolKind::Method && s.detail == "(point)") method = true;
    }
    CHECK(method);
    CHECK(find("perimeter")->kind == LispLspSymbolKind::Interface);
    CHECK(find("with-x")->kind == LispLspSymbolKind::Function);
    CHECK(find("*v*")->kind == LispLspSymbolKind::Variable);
    CHECK(find("+c+")->kind == LispLspSymbolKind::Constant);
    CHECK(find("(setf thing)") != nullptr);
    CHECK(find("hidden") != nullptr);
    CHECK(find("dead") == nullptr);
    CHECK(find("oops")->kind == LispLspSymbolKind::Class);
    CHECK(find("index")->kind == LispLspSymbolKind::TypeParameter);
    CHECK(find(":foo")->kind == LispLspSymbolKind::Package);
    CHECK(find("area")->sel_range.line == 4 && find("area")->sel_range.col == 7);
}

void TestFolds() {
    const Lines doc = {
        ";; one",            // 0
        ";; two",            // 1
        "(defun f (x)",      // 2
        "  (let ((y x))",    // 3
        "    y))",           // 4
        "#| block",          // 5
        "   comment |#",     // 6
        "(a) ; trailing",    // 7
        "; lone",            // 8
    };
    const std::vector<LispLspFold> folds = LispLspFolds(doc);
    auto has = [&folds](int a, int b, const char *kind) {
        for (const LispLspFold &f : folds) {
            if (f.start_line == a && f.end_line == b && f.kind == kind) return true;
        }
        return false;
    };
    CHECK(has(0, 1, "comment"));
    CHECK(has(2, 4, ""));
    CHECK(has(3, 4, ""));
    CHECK(has(5, 6, "comment"));
    for (const LispLspFold &f : folds) CHECK(f.start_line != 7 && f.start_line != 8);
}

void TestDefinitionReferencesRename() {
    const Lines doc = {
        "(defun helper (n) (* n 2))",                    // 0
        "(defun main (list)",                            // 1
        "  (let ((n 1))",                                // 2
        "    (setq n (helper n))",                       // 3
        "    (mapcar #'helper list)",                    // 4
        "    (funcall 'helper n)))",                     // 5
        "(defun other (n) (block b (return-from b n)))",  // 6
    };
    // The local `n` on line 3 goes to the let binding, not the parameter of helper.
    LispLspLocation loc = LispLspDefinition(doc, 3, 11);
    CHECK(loc.found && loc.range.line == 2 && loc.range.col == 9);
    loc = LispLspDefinition(doc, 3, 14);  // helper
    CHECK(loc.found && loc.range.line == 0 && loc.range.col == 7);
    loc = LispLspDefinition(doc, 5, 15);  // 'helper (quoted) falls back to the definition
    CHECK(loc.found && loc.range.line == 0);
    loc = LispLspDefinition(doc, 6, 39);  // return-from b
    CHECK(loc.found && loc.range.line == 6 && loc.range.col == 24);
    CHECK(!LispLspDefinition(doc, 4, 6).found);  // mapcar is CL's

    const std::vector<LispLspReference> refs = LispLspReferences(doc, 2, 9);
    CHECK(refs.size() == 4);  // binding, setq target, helper's argument, funcall argument
    CHECK(refs[0].is_definition);
    CHECK(refs[1].is_write);
    const std::vector<LispLspReference> fn = LispLspReferences(doc, 0, 8);
    CHECK(fn.size() == 4);  // defun, call, #'helper, 'helper
    const std::vector<LispLspReference> cl = LispLspReferences(doc, 4, 6);
    CHECK(cl.size() == 1);

    LispLspRenameTarget t = LispLspPrepareRename(doc, 2, 9);
    CHECK(t.found && t.placeholder == "n");
    CHECK(!LispLspPrepareRename(doc, 4, 6).found);  // mapcar
    std::string err;
    std::vector<LispLspTextEdit> edits = LispLspRename(doc, 2, 9, "count", &err);
    CHECK(err.empty() && edits.size() == 4);
    for (const LispLspTextEdit &e : edits) CHECK(e.new_text == "count" && e.range.line >= 2 && e.range.line <= 5);
    edits = LispLspRename(doc, 0, 8, "twice", &err);
    CHECK(edits.size() == 4);
    edits = LispLspRename(doc, 2, 9, "two words", &err);
    CHECK(edits.empty() && !err.empty());
    err.clear();
    edits = LispLspRename(doc, 2, 9, "pkg:x", &err);
    CHECK(edits.empty() && !err.empty());
    // Declarations follow a rename.
    const Lines decl = {"(defun f (x) (declare (ignorable x)) x)"};
    CHECK(LispLspRename(decl, 0, 10, "y", &err).size() == 3);
    // A package prefix is kept: only the name part is replaced.
    const Lines qual = {"(defun f () 1)", "(cl-user::f)"};
    edits = LispLspRename(qual, 0, 7, "g", &err);
    CHECK(edits.size() == 2 && edits[1].range.col == 10);
}

void TestSignatureHelp() {
    const Lines doc = {"(defun area (w h &optional (d 1) &key unit) (list w h d unit))",
                       "(area 1 2 3 :unit :m)",
                       "(gethash k table ",
                       "(make-hash-table :test 'eql :size "};
    LispLspSignature sig = LispLspSignatureHelp(doc, 1, 6);
    CHECK(sig.found && sig.label == "(area w h &optional (d 1) &key unit)");
    CHECK(sig.params.size() == 4);
    CHECK(sig.active_param == 0);
    CHECK(LispLspSignatureHelp(doc, 1, 10).active_param == 2);  // the optional d
    CHECK(LispLspSignatureHelp(doc, 1, 13).active_param == 3);  // on :unit
    CHECK(LispLspSignatureHelp(doc, 1, 19).active_param == 3);  // its value
    sig = LispLspSignatureHelp(doc, 2, 17);
    CHECK(sig.found && sig.label == "(gethash key hash-table &optional default)");
    CHECK(sig.active_param == 2);
    sig = LispLspSignatureHelp(doc, 3, 34);
    CHECK(sig.found && sig.params[static_cast<size_t>(sig.active_param)].rfind("(size", 0) == 0);
    CHECK(!LispLspSignatureHelp(doc, 1, 2).found);  // on the operator itself
}

void TestCompletionAndHover() {
    const Lines doc = {"(defun my-helper (alpha) alpha)", "(defun g (beta)", "  (let ((gamma 1))",
                       "    (my-h", "    (mapc", "    (make-hash-table :te", "    ga", "    (MAPC"};
    auto has = [](const std::vector<LispLspCompletionItem> &items, const std::string &label) {
        for (const LispLspCompletionItem &i : items) {
            if (i.label == label) return true;
        }
        return false;
    };
    std::vector<LispLspCompletionItem> items = LispLspCompletions(doc, 3, 9);
    CHECK(has(items, "my-helper"));
    CHECK(items[0].replace_start == 5 && items[0].replace_end == 9);
    items = LispLspCompletions(doc, 4, 9);
    CHECK(has(items, "mapcar") && has(items, "mapcan"));
    items = LispLspCompletions(doc, 5, 24);
    CHECK(!items.empty() && items[0].label == "test");
    items = LispLspCompletions(doc, 6, 6);
    CHECK(has(items, "gamma"));
    items = LispLspCompletions(doc, 7, 9);
    bool upper = false;
    for (const LispLspCompletionItem &i : items) upper = upper || (i.label == "mapcar" && i.insert_text == "MAPCAR");
    CHECK(upper);  // typed in upper case, inserted in upper case
    CHECK(LispLspCompletions(doc, 3, 9)[0].insert_text == "my-helper");

    LispLspHoverInfo h = LispLspHover(doc, 3, 3);  // nothing known: my-h is a prefix
    (void)h;
    h = LispLspHover({"(gethash k h)"}, 0, 3);
    CHECK(h.found && h.text.find("(gethash key hash-table &optional default)") != std::string::npos);
    h = LispLspHover({"(defun f (x) \"Doubles X.\" (* x 2))", "(f 1)"}, 1, 1);
    CHECK(h.found && h.text.find("Doubles X.") != std::string::npos && h.text.find("(f x)") != std::string::npos);
    h = LispLspHover({"(loop for x in l collect x)"}, 0, 7);
    CHECK(h.found && h.text.find("loop keyword") != std::string::npos);
    h = LispLspHover({"(defun f (&key a) a)"}, 0, 11);
    CHECK(h.found && h.text.find("lambda-list keyword") != std::string::npos);
    h = LispLspHover({"(let ((x 1)) x)"}, 0, 13);
    CHECK(h.found && h.text.find("lexical variable") != std::string::npos);
    h = LispLspHover({"(if a b)"}, 0, 2);
    CHECK(h.found && h.text.find("special operator") != std::string::npos);
}

// --- Indentation ---------------------------------------------------------

void CheckIndent(const Lines &expected) {
    // Flatten the expected text, then re-indent it and compare.
    Lines flat;
    for (const std::string &l : expected) {
        const size_t b = l.find_first_not_of(' ');
        flat.push_back(b == std::string::npos ? std::string() : l.substr(b));
    }
    const Lines got = LispReindent(flat);
    for (size_t i = 0; i < expected.size(); i++) {
        if (got[i] != expected[i]) {
            std::fprintf(stderr, "indent line %zu: got [%s] want [%s]\n", i, got[i].c_str(), expected[i].c_str());
        }
        CHECK(got[i] == expected[i]);
    }
    // And the single-line entry point agrees, line by line, given the
    // correctly indented lines above.
    for (size_t i = 1; i < expected.size(); i++) {
        const size_t b = expected[i].find_first_not_of(' ');
        if (b == std::string::npos) continue;
        CHECK(LispIndentForLine(expected, static_cast<int>(i)) == static_cast<int>(b));
    }
}

void TestIndentation() {
    CheckIndent({
        "(defun foo (x y",
        "            &optional z)",
        "  \"Doc.\"",
        "  (let ((a 1)",
        "        (b 2))",
        "    (when (> a b)",
        "      (print a))",
        "    (list 1 2",
        "          3)",
        "    (funcall",
        "     #'foo",
        "     1)))",
    });
    CheckIndent({
        "(cond ((= x 1)",
        "       (bar x))",
        "      (t",
        "       (baz y)))",
    });
    CheckIndent({
        "(flet ((helper (q)",
        "         (* q 2)))",
        "  (handler-case (helper 1)",
        "    (error (e)",
        "      (print e))))",
    });
    CheckIndent({
        "(multiple-value-bind (q r)",
        "    (floor 10 3)",
        "  (list q r))",
    });
    CheckIndent({
        "(defclass point ()",
        "  ((x :initarg :x",
        "      :accessor x))",
        "  (:documentation \"pt\"))",
    });
    CheckIndent({
        "(loop for i from 0 below 10",
        "      collect i)",
    });
    CheckIndent({
        "'(a b",
        "  c)",
    });
    CheckIndent({
        "`(defun ,name ()",
        "   (body))",
    });
    CheckIndent({
        "(defmethod area :around ((s square))",
        "  (call-next-method))",
    });
    CheckIndent({
        "(case x",
        "  (:a (one)",
        "      (two))",
        "  (t 3))",
    });
    CheckIndent({
        "(alexandria:when-let ((x (foo)))",
        "  x)",
    });
    // Lambda lists continue two past the last &keyword (Emacs's
    // lisp-lambda-list-keyword-parameter-indentation); `&allow-other-keys))'
    // is not a keyword to its regexp, which wants a blank after it.
    CheckIndent({
        "(defgeneric create (regex &key case-insensitive",
        "                            multi-line",
        "                            &allow-other-keys))",
    });
    // LOOP: every line of an extended loop six past its paren, as Emacs does.
    CheckIndent({
        "(loop for x in list",
        "      do (print x)",
        "      (print 2)",
        "      collect x)",
    });
    CheckIndent({
        "(cond",
        "  ((a) b)",
        "  (t c))",
    });
    CheckIndent({
        "(deftest copy.1",
        "    (let ((a 1))",
        "      a)",
        "  1)",
    });
    CheckIndent({
        "(defvar *x*",
        "  1",
        "  \"Doc.\")",
    });
    CheckIndent({
        "(asdf:defsystem \"demo\"",
        "    :depends-on (:alexandria)",
        "    :components ((:file \"a\")",
        "                 (:file \"b\")))",
    });
    CheckIndent({
        "(destructuring-bind",
        "      (a b)",
        "    list",
        "  (+ a b))",
    });
    CheckIndent({
        "(list #\\( a",
        "      b)",
    });
    // Strings and block comments are never touched.
    const Lines text = {"(defun f ()", "  \"First line", "        stays here\"", "#| x", "      y |#", "1)"};
    const Lines out = LispReindent(text);
    CHECK(out[2] == text[2] && out[4] == text[4]);
    // Comments: `;;;` and margin `;` lines stay put; `;;` indents like code.
    const Lines comments = LispReindent({"(defun f ()", ";;; top", "      ; margin", ";; inner", "1)"});
    CHECK(comments[1] == ";;; top" && comments[2] == "      ; margin" && comments[3] == "  ;; inner");
    // Blank lines lose their whitespace.
    CHECK(LispReindent({"(a", "   ", "b)"})[1].empty());
    // Incomplete code above: just count open forms.
    CHECK(LispIndentForLine({"(defun f (x)", "  (let ((y 1))", ""}, 2) == 4);
    CHECK(LispIndentForLine({"(defun f (x)", ""}, 1) == 2);
    CHECK(LispIndentForLine({"(foo bar", ""}, 1) == 5);
    CHECK(LispIndentForLine({"(foo", ""}, 1) == 1);
    CHECK(LispIndentForLine({"(a))) (b", ""}, 1) == 0);  // stray parens close the form, as in Emacs
    CHECK(LispIndentForLine({"  (print \"abc", ""}, 1) == 2);  // inside a string: the previous margin
    CHECK(LispIndentForLine({"(a)", ""}, 1) == 0);
    CHECK(LispIndentForLine({}, 0) == 0);
    // Formatting is lisp_format.h's formatter, as one edit over the lines that changed.
    const std::vector<LispLspTextEdit> edits = LispLspFormat({"(defun f ( )", "1", "  )", "(g)  "});
    CHECK(edits.size() == 1 && edits[0].range.line == 0 && edits[0].range.end_line == 4 &&
          edits[0].new_text == "(defun f ()\n  1)\n(g)\n");
    CHECK(LispLspFormat({"(defun f ()", "  1)"}).empty());
    CHECK(LispLspFormat({"(defun f ()", "1"}).empty());  // an unclosed paren: left alone
}

// --- CLI ----------------------------------------------------------------------

void TestLintFormat() {
    LispLspDiagnostic d;
    d.range.line = 4;
    d.range.col = 2;
    d.severity = LispLspSeverity::Warning;
    d.code = "unused-variable";
    d.message = "`x` is never used.";
    CHECK(LispLspFormatDiagnostic("src/a.lisp", d) == "src/a.lisp:5:3: warning: `x` is never used. [unused-variable]");
    CHECK(LispLspFormatDiagnostic("-", d).rfind("<stdin>:5:3:", 0) == 0);
    CHECK(LispLspDiagnosticFails(d));
    d.severity = LispLspSeverity::Hint;
    CHECK(LispLspFormatDiagnostic("a", d).find(": hint: ") != std::string::npos);
    CHECK(!LispLspDiagnosticFails(d));
    d.severity = LispLspSeverity::Information;
    CHECK(LispLspFormatDiagnostic("a", d).find(": info: ") != std::string::npos);
    d.severity = LispLspSeverity::Error;
    CHECK(LispLspFormatDiagnostic("a", d).find(": error: ") != std::string::npos);
}

void TestTables() {
    CHECK(kLispClDeclCount == 978);
    for (size_t i = 1; i < kLispClDeclCount; i++) CHECK(std::string(kLispClDecls[i - 1].name) < kLispClDecls[i].name);
    const LispClDecl *d = LispLspFindCl("MAKE-HASH-TABLE");
    CHECK(d != nullptr && LispLspClHasKind(*d, "function"));
    CHECK(LispLspClHasKind(*LispLspFindCl("LIST"), "class"));
    CHECK(LispLspClHasKind(*LispLspFindCl("IF"), "special-operator"));
    CHECK(LispLspFindCl("NOT-A-CL-SYMBOL") == nullptr);
    CHECK(LispLspSpecialFormVocab().size() == 25);
    for (const LispLspVocabEntry &e : LispLspSpecialFormVocab()) {
        CHECK(LispLspClHasKind(*LispLspFindCl(e.name), "special-operator"));
    }
    CHECK(LispLspFindVocab("&KEY") != nullptr && LispLspFindLoopKeyword("COLLECT") != nullptr);
    const LispLambdaShape s = LispLspShapeOfLambdaList("key hash-table &optional default");
    CHECK(s.required == 2 && s.optional == 1 && !s.rest && !s.key);
    const LispLambdaShape k = LispLspShapeOfLambdaList("&key (test 'eql) ((:size sz) 7) &allow-other-keys");
    CHECK(k.key && k.allow_other_keys && k.keys.size() == 2 && k.keys[0] == "TEST" && k.keys[1] == "SIZE");
    CHECK(LispLspShapeOfLambdaList("&whole w name &body b").required == 1);
    const std::vector<std::string> parts = LispLspSplitLambdaList("(a b) &key (c \"x y\")");
    CHECK(parts.size() == 3 && parts[2] == "(c \"x y\")");
}


// --- Project libraries (lisp_lsp_library.h) ----------------------------

void WriteFile(const std::filesystem::path &p, const std::string &text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

const char kDmAsd[] = R"lisp(;;;; Generated by Rosetta.
(asdf:defsystem "datamunge"
  :depends-on ("cffi")
  :components ((:module "src" :components ((:file "package") (:file "datamunge")))))
)lisp";

const char kDmPackage[] = R"lisp((defpackage #:datamunge
  (:use)
  (:nicknames #:dm)
  (:documentation "Native C++ module `datamunge`, bound by Rosetta.")
  (:export #:base #:dual #:make-dual #:expr #:make-expr #:leaf #:make-leaf #:multiply #:value
           #:add #:kind #:+golden+ #:ref))
(defpackage #:datamunge.rosetta (:use #:common-lisp))
)lisp";

const char kDmSource[] = R"lisp((in-package #:datamunge.rosetta)

(defun %call (id self args) (list id self args))

(defclass datamunge:base () ()
  (:documentation "Base of every value."))

(defclass datamunge:dual (datamunge:base) ()
  (:documentation "A dual number.

Constructors:
  (make-dual value derivative)"))

(defclass datamunge:expr (datamunge:base) ()
  (:documentation "An expression."))

(defclass datamunge:leaf (datamunge:dual) ()
  (:documentation "A leaf dual."))

(defun datamunge:make-dual (value derivative)
  "A dual number.

  (make-dual value derivative) → dual
    value       double-float  the value
    derivative  double-float  its derivative"
  (%call 1 nil (list value derivative)))

(defun datamunge:make-expr (x)
  "An expression.

  (make-expr x) → expr"
  (%call 2 nil (list x)))

(defun datamunge:make-leaf ()
  "A leaf.

  (make-leaf) → leaf"
  (%call 3 nil nil))

(defgeneric datamunge:multiply (self &rest args)
  (:documentation "Methods: dual, expr."))

(defmethod datamunge:multiply ((self datamunge:dual) &rest args)
  "Multiplies two duals.

  (multiply dual other) → dual"
  (%call 4 self args))

(defmethod datamunge:multiply ((self datamunge:expr) &rest args)
  "Multiplies two expressions.

  (multiply expr other) → expr"
  (%call 5 self args))

(defgeneric datamunge:value (self &rest args)
  (:documentation "Methods: dual (field)."))

(defmethod datamunge:value ((self datamunge:dual) &rest args)
  "The value.

  (value dual) → double-float"
  (%call 6 self args))

(defmethod (setf datamunge:value) (new (self datamunge:dual) &rest args)
  (%call 7 self (cons new args)))

(defun datamunge:add (a &optional (b nil b-p) (c nil c-p))
  "Adds three integers.

  (add a &optional (b 10) (c 100)) → integer
    a  integer  first term
    → the sum

Errors (signalled as DATAMUNGE:NATIVE-ERROR):
  std::out_of_range: never"
  (%call 8 nil (cond (c-p (list a b c)) (b-p (list a b)) (t (list a)))))

(deftype datamunge:kind ()
  "Kinds.

Values: :small (0), :large (1)."
  '(member :small :large))

(defconstant datamunge:+golden+ 1.618d0
  "The golden ratio.")
)lisp";

int Find(const Lines &lines, const std::string &needle, int nth = 0) {
    for (size_t l = 0; l < lines.size(); l++) {
        for (size_t at = lines[l].find(needle); at != std::string::npos; at = lines[l].find(needle, at + 1)) {
            if (nth-- == 0) return static_cast<int>(l * 1000 + at);
        }
    }
    return -1;
}

/** @brief Hover text at the `nth` occurrence of `needle`, `offset` bytes into it. */
LispLspHoverInfo HoverOn(const Lines &lines, const std::string &path, const std::string &needle, int offset, int nth = 0) {
    const int at = Find(lines, needle, nth);
    CHECK(at >= 0);
    return LispLspHover(lines, at / 1000, at % 1000 + offset, path);
}

bool Has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

void TestProjectLibrary() {
    namespace fs = std::filesystem;
    const char *tmp_env = std::getenv("TMPDIR");
    const fs::path base = fs::path(tmp_env && *tmp_env ? tmp_env : "/tmp") / ("mep-lisp-lsp-test-" + std::to_string(getpid()));
    fs::remove_all(base);
    const fs::path root = base / "project";
    fs::create_directories(root / ".git");
    WriteFile(root / "lib/dm/datamunge.asd", kDmAsd);
    WriteFile(root / "lib/dm/src/package.lisp", kDmPackage);
    WriteFile(root / "lib/dm/src/datamunge.lisp", kDmSource);
    // Skipped directories are not searched.
    WriteFile(root / "build/other/datamunge.asd", "(asdf:defsystem \"datamunge\" :components ((:file \"x\")))");
    const std::string user = (root / "examples/autodiff_ex.lisp").string();

    // Discovery: the project's .asd, the nested :module resolved, build/ skipped.
    const std::vector<std::string> asds = LispLibAsdFiles(user);
    CHECK(asds.size() == 1 && Has(asds[0], "lib/dm/datamunge.asd"));
    const std::vector<LispLibSystem> systems = LispLibParseAsd(
        {"(asdf:defsystem \"datamunge\" :components ((:module \"src\" :components ((:file \"package\") (:file \"datamunge\")))))"},
        (root / "lib/dm/datamunge.asd").string());
    CHECK(systems.size() == 1 && systems[0].name == "datamunge" && systems[0].files.size() == 2 &&
          Has(systems[0].files[1], "lib/dm/src/datamunge.lisp"));

    // The index of one file.
    {
        std::vector<std::string> src;
        std::string cur;
        for (char ch : std::string(kDmSource)) {
            if (ch == '\n') {
                src.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        const LispLibFile f = LispLibParseSource(src, "datamunge.lisp");
        int methods = 0;
        bool setf = false;
        for (const LispLibDef &d : f.defs) {
            if (d.kind == LispLibKind::Method && d.name == "MULTIPLY") methods++;
            if (d.name == "(SETF VALUE)" && d.specializer == "DUAL") setf = true;
            if (d.name == "MAKE-DUAL") CHECK(d.returns == "DUAL" && d.package == "DATAMUNGE");
            if (d.name == "%CALL") CHECK(d.package == "DATAMUNGE.ROSETTA");
            if (d.name == "LEAF") CHECK(d.supers.size() == 1 && d.supers[0] == "DUAL");
        }
        CHECK(methods == 2 && setf);
        CHECK(LispLibDocSignatures("Adds.\n\n  (add a &optional (b 10)) → integer\n    a  integer", "add") ==
              std::vector<std::string>{"a &optional (b 10)"});
        const std::string md = LispLibDocMarkdown("Adds.\n\n  (add a) → integer\n    a  integer\n\nMore.");
        CHECK(md == "Adds.\n\n```lisp\n(add a) → integer\n  a  integer\n```\n\nMore.");
    }

    // Global nickname: hover shows the docstring as markdown.
    const Lines user_lines = {
        "(asdf:load-system \"datamunge\")",
        "(defpackage #:demo (:use #:cl) (:local-nicknames (#:d #:datamunge)))",
        "(in-package #:demo)",
        "(print (dm:add 1 2))",
        "(print (d:make-dual 2 1))",
        "(print datamunge:+golden+)",
        "(defun f (x) (d:multiply x x))",
        "(list (d:make-expr 1) (d:make-expr 2) (d:make-expr 3) (typep 1 'd:kind) (make-instance 'd:dual))",
    };
    LispLspHoverInfo h = HoverOn(user_lines, user, "dm:add", 4);
    CHECK(h.found && h.markdown && Has(h.text, "**function** `datamunge:add`") && Has(h.text, "Adds three integers.") &&
          Has(h.text, "```lisp\n(add a &optional (b 10)") && Has(h.text, "System `datamunge`"));
    // Local nickname.
    h = HoverOn(user_lines, user, "d:make-dual", 4);
    CHECK(h.found && Has(h.text, "`datamunge:make-dual`") && Has(h.text, "(make-dual value derivative) → dual"));
    // The package prefix itself.
    h = HoverOn(user_lines, user, "dm:add", 0);
    CHECK(h.found && Has(h.text, "**package** `DATAMUNGE`") && Has(h.text, "bound by Rosetta"));
    h = HoverOn(user_lines, user, "datamunge:+golden+", 12);
    CHECK(h.found && Has(h.text, "**constant**") && Has(h.text, "1.618d0") && Has(h.text, "golden ratio"));
    // A generic function: the class the file mentions most (expr) wins over source order (dual).
    h = HoverOn(user_lines, user, "d:multiply", 4);
    CHECK(h.found && Has(h.text, "**generic function** `datamunge:multiply` — method on `expr`") &&
          Has(h.text, "Multiplies two expressions.") && Has(h.text, "*Also a method of `dual`.*"));
    // A class: its doc, constructor and methods.
    h = HoverOn(user_lines, user, "'d:dual", 5);
    CHECK(h.found && Has(h.text, "**class** `datamunge:dual` — subclass of `base`") && Has(h.text, "A dual number.") &&
          Has(h.text, "*Methods:* `multiply`, `value`"));
    // A deftype enum.
    h = HoverOn(user_lines, user, "'d:kind", 4);
    CHECK(h.found && Has(h.text, "**type** `datamunge:kind`") && Has(h.text, "Values: :small (0)") &&
          Has(h.text, "(member :small :large)"));
    // No undefined-function (or any) report for library names.
    CHECK(LispLspDiagnostics(user_lines).empty());

    // The receiver's class decides, when the file binds it from a constructor.
    Lines let_lines = user_lines;
    let_lines.push_back("(let ((a (d:make-dual 2 1))) (d:multiply a a))");
    h = HoverOn(let_lines, user, "(d:multiply a", 5);
    CHECK(h.found && Has(h.text, "method on `dual`") && Has(h.text, "Multiplies two duals.") &&
          Has(h.text, "receiver's class `dual`"));
    let_lines.push_back("(defparameter *e* (d:make-expr 3))");
    let_lines.push_back("(defparameter *l* (d:make-leaf))");
    let_lines.push_back("(d:multiply *l* *e*)");
    h = HoverOn(let_lines, user, "(d:multiply *l*", 5);  // leaf inherits dual's method
    CHECK(h.found && Has(h.text, "method on `dual`") && Has(h.text, "Inherited by the receiver's class `leaf`") == false);
    CHECK(Has(h.text, "receiver's class `leaf`"));
    // A direct constructor call as receiver.
    let_lines.push_back("(d:multiply (d:make-dual 1 0) 2)");
    h = HoverOn(let_lines, user, "(d:multiply (d:make-dual", 5);
    CHECK(h.found && Has(h.text, "method on `dual`"));
    // Go to definition: the library's source file.
    const int at = Find(let_lines, "(d:multiply a");
    const LispLspLocation loc = LispLspDefinition(let_lines, at / 1000, at % 1000 + 5, user);
    CHECK(loc.found && Has(loc.path, "datamunge.lisp") && loc.range.line > 0);

    // Completion after a nickname: the package's exports, with kinds and docs.
    Lines comp = user_lines;
    comp.push_back("(d:mu");
    std::vector<LispLspCompletionItem> items = LispLspCompletions(comp, static_cast<int>(comp.size()) - 1, 5, user);
    CHECK(items.size() == 1 && items[0].label == "multiply" && items[0].kind == LispLspKind::Method &&
          items[0].replace_start == 3 && items[0].replace_end == 5);
    comp.back() = "(dm:";
    items = LispLspCompletions(comp, static_cast<int>(comp.size()) - 1, 4, user);
    bool add = false;
    bool kind = false;
    for (const LispLspCompletionItem &it : items) {
        if (it.label == "add") add = it.kind == LispLspKind::Function && Has(it.detail, "(add a &optional (b 10)") &&
                                     it.documentation == "Adds three integers.";
        if (it.label == "kind") kind = it.kind == LispLspKind::TypeParameter;
        CHECK(it.label != "%call");
    }
    CHECK(items.size() == 13 && add && kind);
    comp.back() = "(datamunge.rosetta::%c";
    items = LispLspCompletions(comp, static_cast<int>(comp.size()) - 1, 22, user);
    CHECK(items.size() == 1 && items[0].label == "%call");
    comp.back() = "(nope:";
    CHECK(LispLspCompletions(comp, static_cast<int>(comp.size()) - 1, 6, user).empty());

    // Signature help: the docstring's signature line, active parameter moving along.
    Lines sig = user_lines;
    sig.push_back("(dm:add 1 ");
    const LispLspSignature s = LispLspSignatureHelp(sig, static_cast<int>(sig.size()) - 1, 10, user);
    CHECK(s.found && s.label == "(dm:add a &optional (b 10) (c 100))" && s.active_param == 1 &&
          s.documentation == "Adds three integers.");

    // A package that :uses the library: unqualified names resolve, and nothing is reported.
    const Lines use_lines = {
        "(defpackage #:demo2 (:use #:cl #:datamunge))",
        "(in-package #:demo2)",
        "(print (add 1 2))",
        "(print (frobnicate 1))",
    };
    h = HoverOn(use_lines, user, "(add", 1);
    CHECK(h.found && Has(h.text, "`datamunge:add`"));
    CHECK(CountCode(LispLspDiagnostics(use_lines), "undefined-function") == 0);
    CHECK(!HoverOn(use_lines, user, "frobnicate", 1).found);

    // A changed source is re-read.
    std::string changed = kDmSource;
    changed.replace(changed.find("Adds three integers."), 20, "Sums three integers.");
    WriteFile(root / "lib/dm/src/datamunge.lisp", changed);
    fs::last_write_time(root / "lib/dm/src/datamunge.lisp", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    h = HoverOn(user_lines, user, "dm:add", 4);
    CHECK(h.found && Has(h.text, "Sums three integers."));

    // No .asd anywhere: nothing found, nothing crashes; without a path neither.
    const fs::path bare = base / "bare";
    fs::create_directories(bare / ".git");
    const std::string bare_user = (bare / "x.lisp").string();
    CHECK(!HoverOn(user_lines, bare_user, "dm:add", 4).found);
    CHECK(!HoverOn(user_lines, std::string(), "dm:add", 4).found);
    CHECK(LispLspCompletions(comp, static_cast<int>(comp.size()) - 1, 6, bare_user).empty());
    CHECK(LispLspDiagnostics(user_lines).empty());

    // A system CMake generated under build/ is found once nothing else provides it.
    {
        const fs::path gen = base / "generated";
        fs::create_directories(gen / ".git");
        WriteFile(gen / "build/lisp/features.asd", "(asdf:defsystem \"features\" :components ((:file \"features\")))");
        WriteFile(gen / "build/lisp/features.lisp",
                  "(defpackage #:features (:use) (:nicknames #:ft) (:export #:add))\n"
                  "(defun features:add (a b) \"Adds.\n\n  (add a b) → integer\" (+ a b))\n");
        const Lines g = {"(defpackage #:demo (:use #:cl) (:local-nicknames (#:f #:features)))", "(f:add 1 2)"};
        h = HoverOn(g, (gen / "examples/use.lisp").string(), "f:add", 3);
        CHECK(h.found && Has(h.text, "`features:add`") && Has(h.text, "System `features`"));
    }

    // A system the project provides is indexed once, even when the document also names something
    // missing (so the build directories are searched) and build/ holds an installed copy of it.
    {
        const fs::path dup = base / "duplicated";
        fs::create_directories(dup / ".git");
        const std::string asd = "(asdf:defsystem \"features\" :components ((:file \"features\")))";
        const std::string lisp = "(defpackage #:features (:use) (:nicknames #:ft) (:export #:shape))\n"
                                 "(defclass features:shape () () (:documentation \"A shape.\"))\n";
        WriteFile(dup / "src/lisp/features.asd", asd);
        WriteFile(dup / "src/lisp/features.lisp", lisp);
        WriteFile(dup / "build/prefix/share/features.asd", asd);
        WriteFile(dup / "build/prefix/share/features.lisp", lisp);
        const Lines g = {"(asdf:load-system \"features\")", "(print (list 'ft:shape 'nowhere:thing))"};
        h = HoverOn(g, (dup / "examples/use.lisp").string(), "ft:shape", 3);
        CHECK(h.found && Has(h.text, "A shape.") && h.text.find("A shape.") == h.text.rfind("A shape."));
    }

    // $MEP_LISP_LIBRARY finds the system from outside the project.
    setenv("MEP_LISP_LIBRARY", (root / "lib").string().c_str(), 1);
    CHECK(HoverOn(user_lines, bare_user, "dm:add", 4).found);
    unsetenv("MEP_LISP_LIBRARY");

    // Performance: a generated-size file (2000 methods on 200 classes) is read once per change.
    {
        std::string big = kDmPackage;
        big += "(in-package #:datamunge.rosetta)\n";
        for (int c = 0; c < 200; c++) {
            big += "(defclass datamunge:c" + std::to_string(c) + " (datamunge:base) ()\n  (:documentation \"Class " +
                   std::to_string(c) + ".\"))\n";
        }
        for (int g = 0; g < 400; g++) {
            big += "(defgeneric datamunge:g" + std::to_string(g) + " (self &rest args)\n  (:documentation \"Methods.\"))\n";
        }
        for (int m = 0; m < 2000; m++) {
            const std::string c = std::to_string((m + m / 400) % 200);  // g7: c7, c8, c9, c10, c11
            big += "(defmethod datamunge:g" + std::to_string(m % 400) + " ((self datamunge:c" + c +
                   ") &rest args)\n  \"Method " + std::to_string(m) + ".\n\n  (g" + std::to_string(m % 400) + " c" + c +
                   " x) → double-float\n    x  double-float  an argument\"\n  (%call " + std::to_string(m) +
                   " self args))\n";
        }
        WriteFile(root / "lib/dm/src/datamunge.lisp", big);
        fs::last_write_time(root / "lib/dm/src/datamunge.lisp", fs::file_time_type::clock::now() + std::chrono::seconds(10));
        const Lines big_user = {"(print (dm:g7 (dm:make-c9)))"};
        const auto t0 = std::chrono::steady_clock::now();
        h = HoverOn(big_user, user, "dm:g7", 4);
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; i++) h = HoverOn(big_user, user, "dm:g7", 4);
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(h.found && Has(h.text, "method on `c9`") && Has(h.text, "Method 807.") &&
              Has(h.text, "receiver's class `c9`; also a method of `c7`, `c8`, `c10`, `c11`.*") &&
              Has(h.text, "Not exported from `DATAMUNGE`"));
        const double first = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double again = std::chrono::duration<double, std::milli>(t2 - t1).count() / 20;
        std::printf("project library: %zu-line source indexed and hovered in %.1f ms, hover after that %.2f ms\n",
                    static_cast<size_t>(std::count(big.begin(), big.end(), '\n')), first, again);
        CHECK(again < first);
    }
    fs::remove_all(base);
}
}  // namespace

int main() {
    TestReaderAtoms();
    TestReaderComments();
    TestReaderFeatures();
    TestReaderErrors();
    TestUnusedVariables();
    TestShapeDiagnostics();
    TestCallDiagnostics();
    TestNamingDiagnostics();
    TestNameCalibration();
    TestReaderDiagnosticsAndReadtables();
    TestCleanFile();
    TestSymbols();
    TestFolds();
    TestDefinitionReferencesRename();
    TestSignatureHelp();
    TestCompletionAndHover();
    TestIndentation();
    TestLintFormat();
    TestTables();
    TestProjectLibrary();
    std::printf("mep-lisp-lsp-test: all %d checks passed\n", g_checks_run);
    return 0;
}
