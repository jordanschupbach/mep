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

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "lisp_indent.h"
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
    std::printf("mep-lisp-lsp-test: all %d checks passed\n", g_checks_run);
    return 0;
}
