// Windowless test for scheme_lsp.cpp -- the analysis half of mep's own
// Scheme language server (`mep-scheme-lsp`) -- and the reader under it
// (scheme_reader.h). Drives the pure functions directly: no process, no
// JSON-RPC client. CHECK(), never assert(): the Release build strips
// assert().
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "scheme_lsp.h"
#include "scheme_reader.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

std::vector<std::string> L(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<SchemeLspDiagnostic> Diags(const std::string &src) {
    return SchemeLspDiagnostics(*SchemeLspAnalyze(L(src)));
}

int Count(const std::vector<SchemeLspDiagnostic> &ds, const std::string &code) {
    int n = 0;
    for (const SchemeLspDiagnostic &d : ds)
        if (d.code == code) n++;
    return n;
}

const SchemeLspDiagnostic *Find(const std::vector<SchemeLspDiagnostic> &ds, const std::string &code) {
    for (const SchemeLspDiagnostic &d : ds)
        if (d.code == code) return &d;
    return nullptr;
}

// Guile's verdict, as `--guile-check` prints it.
std::string Verdict(const std::string &src) {
    const SchemeReadResult r = SchemeRead(src);
    if (r.ok()) return "ok";
    const SchemeReadError &e = r.errors[0];
    if (e.guile_line == 0) return e.message;
    return std::to_string(e.guile_line) + ":" + std::to_string(e.guile_col) + ": " + e.message;
}

void TestReader() {
    // Each expectation is what Guile's `read` reports on the same input
    // (tools/ts_grammar_diff/scheme_read.scm FILE).
    CHECK(Verdict("(define (f x) (* x x))\n") == "ok");
    CHECK(Verdict("(a b") == "1:5: unexpected end of input while searching for: )");
    CHECK(Verdict("(a b))") == "1:7: unexpected \")\"");
    CHECK(Verdict("(a ]") == "1:5: mismatched close paren: ]");
    CHECK(Verdict("\"abc") == "1:5: unexpected end of input while reading string");
    CHECK(Verdict("#\\foo") == "1:6: unknown character name foo");
    CHECK(Verdict("#|x") == "1:4: unterminated `#| ... |#' comment");
    CHECK(Verdict("\t(a") == "1:11: unexpected end of input while searching for: )");
    // Numbers are what string->number accepts; everything else is a symbol.
    CHECK(SchemeIsNumber("1e10") && SchemeIsNumber("#x1F") && SchemeIsNumber("-1/2") && SchemeIsNumber("+inf.0"));
    CHECK(!SchemeIsNumber("1+") && !SchemeIsNumber("...") && !SchemeIsNumber("inf.0") && !SchemeIsNumber("1/0x"));
    // Directives and Guix's G-expressions.
    CHECK(Verdict("#!r6rs\n(a)") == "ok");
    CHECK(Verdict("#!/usr/bin/guile\n!#\n(a)") == "ok");
    CHECK(Verdict("#~(a #$b #$@c)") == "ok");
    const SchemeReadResult r = SchemeRead("(a . b) 'c #(1 2) #u8(3)");
    CHECK(r.ok() && r.top.size() == 4);
    CHECK(r.at(r.top[0]).dotted && r.at(r.top[1]).kind == SchemeNodeKind::Prefix);
    CHECK(r.at(r.top[2]).kind == SchemeNodeKind::Vector && r.at(r.top[3]).kind == SchemeNodeKind::Bytevector);
}

void TestReaderDiagnostics() {
    const auto ds = Diags("(define (f x)\n  (+ x 1)\n");
    const SchemeLspDiagnostic *d = Find(ds, "unbalanced-paren");
    CHECK(d && d->severity == SchemeLspSeverity::Error && d->range.line == 0 && d->range.col == 0);
    CHECK(SchemeLspDiagnosticFails(*d));
    CHECK(SchemeLspFormatDiagnostic("a.scm", *d) ==
          "a.scm:1:1: error: unexpected end of input while searching for: ) [unbalanced-paren]");
    CHECK(Count(Diags("(a))"), "unexpected-close") == 1);
}

void TestDuplicates() {
    CHECK(Count(Diags("(let ((a 1) (a 2)) a)"), "duplicate-binding") == 1);
    CHECK(Count(Diags("(letrec ((a 1) (a 2)) a)"), "duplicate-binding") == 1);
    CHECK(Count(Diags("(lambda (a a) a)"), "duplicate-binding") == 1);
    CHECK(Count(Diags("(define (f x x) x)"), "duplicate-binding") == 1);
    CHECK(Count(Diags("(do ((i 0 (+ i 1)) (i 1)) (#t i))"), "duplicate-binding") == 1);
    CHECK(Count(Diags("(define (f) (define a 1) (define a 2) a)"), "duplicate-binding") == 1);
    // let* rebinds legally, a top-level redefinition is allowed, and only
    // one cond-expand clause is expanded.
    CHECK(Count(Diags("(let* ((a 1) (a 2)) a)"), "duplicate-binding") == 0);
    CHECK(Count(Diags("(define a 1)\n(define a 2)"), "duplicate-binding") == 0);
    CHECK(Count(Diags("(define (f)\n (cond-expand (guile (define a 1)) (else (define a 2)))\n a)"),
                "duplicate-binding") == 0);
    const auto ds = Diags("(let ((a 1) (a 2)) a)");
    CHECK(Find(ds, "duplicate-binding")->message == "let: duplicate bound variable: a");
}

void TestUnused() {
    auto ds = Diags("(define (f)\n  (let ((x 1) (y 2))\n    y))");
    CHECK(Count(ds, "unused-variable") == 1);
    const SchemeLspDiagnostic *d = Find(ds, "unused-variable");
    CHECK(d->range.line == 1 && d->range.col == 9 && d->message == "unused variable x" && d->unnecessary);
    CHECK(Count(Diags("(define (f) (let ((_x 1)) 2))"), "unused-variable") == 0);
    // A parameter is a hint: a callback implements someone else's interface.
    ds = Diags("(define (f a b) a)");
    d = Find(ds, "unused-parameter");
    CHECK(d && d->severity == SchemeLspSeverity::Hint && !SchemeLspDiagnosticFails(*d));
    CHECK(Count(Diags("(define (f) (define (g) 1) 2)"), "unused-definition") == 1);
    // Uses that only a model of the special forms sees.
    CHECK(Count(Diags("(define (f) (let ((x 1)) `(a ,x)))"), "unused-variable") == 0);
    CHECK(Count(Diags("(define (f) (let ((x 1)) #~(a #$x)))"), "unused-variable") == 0);
    CHECK(Count(Diags("(define (f) (let ((x 1)) (define-syntax m (syntax-rules () ((_) x))) (m)))"),
                "unused-variable") == 0);
    CHECK(Count(Diags("(define (f l) (let ((x 1)) (match l ((a . b) (+ a x)))))"), "unused-variable") == 0);
    CHECK(Count(Diags("(define (f) (let ((x 1)) (include \"g.scm\")))"), "unused-variable") == 0);
    // Quoted data is not a use.
    CHECK(Count(Diags("(define (f) (let ((x 1)) 'x))"), "unused-variable") == 1);
    // and-let* bindings are tests.
    CHECK(Count(Diags("(define (f a) (and-let* ((x (car a))) #t))"), "unused-variable") == 0);
    // Exported top-level names are never unused, nor are top-level ones at all.
    CHECK(Diags("(define-module (a b) #:export (f))\n(define (f) 1)\n(define g 2)").empty());
    // After a reader error the tree is a guess: no lints past it.
    CHECK(Count(Diags("(define (f) (let ((x 1)) 2)) #\\nosuchchar (define (g) (let ((y 1)) 2))"),
                "unused-variable") == 1);
}

void TestArity() {
    // Procedures the file defines.
    auto ds = Diags("(define (f a b) (+ a b))\n(f 1)");
    const SchemeLspDiagnostic *d = Find(ds, "wrong-arity");
    CHECK(d && d->severity == SchemeLspSeverity::Warning && d->message == "f takes 2 arguments, given 1");
    CHECK(d->range.line == 1 && d->range.col == 1 && d->range.end_col == 2);
    CHECK(Count(Diags("(define (f a . r) a)\n(f 1 2 3)"), "wrong-arity") == 0);
    CHECK(Find(Diags("(define (f a . r) a)\n(f)"), "wrong-arity")->message == "f takes at least 1 argument, given 0");
    CHECK(Count(Diags("(define* (f a #:optional b) a)\n(f 1 2)"), "wrong-arity") == 0);
    CHECK(Find(Diags("(define* (f a #:optional b) a)\n(f 1 2 3)"), "wrong-arity")->message ==
          "f takes 1 or 2 arguments, given 3");
    CHECK(Count(Diags("(define* (f a #:key b) a)\n(f 1 #:b 2)"), "wrong-arity") == 0);
    CHECK(Find(Diags("(define f (case-lambda ((a) a) ((a b c) a)))\n(f 1 2)"), "wrong-arity")->message ==
          "f takes 1 or 3 arguments, given 2");
    CHECK(Count(Diags("(define (g) (let loop ((i 0) (acc '())) (loop 1)))"), "wrong-arity") == 1);
    CHECK(Count(Diags("(define-record-type <p> (make-p x y) p? (x p-x))\n(make-p 1)\n(p-x)"), "wrong-arity") == 2);
    // Reassigned, redefined or shadowed: not judged.
    CHECK(Count(Diags("(define (f a) a)\n(set! f car)\n(f 1 2)"), "wrong-arity") == 0);
    CHECK(Count(Diags("(define (f a) a)\n(define (f a b) a)\n(f 1 2)"), "wrong-arity") == 0);
    CHECK(Count(Diags("(define (f a) a)\n(let ((f list)) (f 1 2))"), "wrong-arity") == 0);
    // Guile's own procedures, when the file says what it imports.
    CHECK(Find(Diags("(use-modules (srfi srfi-1))\n(car 1 2)"), "wrong-arity")->message ==
          "car takes 1 argument, given 2");
    CHECK(Count(Diags("(use-modules (srfi srfi-1))\n(assoc 1 '() eq?)"), "wrong-arity") == 0);  // srfi-1's assoc
    CHECK(Count(Diags("(define-module (a) #:use-module (ice-9 match))\n(assoc 1 '() eq?)"), "wrong-arity") == 1);
    CHECK(Count(Diags("(car 1 2)"), "wrong-arity") == 0);  // no imports declared: maybe another Scheme
    CHECK(Count(Diags("(use-modules (my private module))\n(car 1 2)"), "wrong-arity") == 0);
    CHECK(Count(Diags("(import (scheme base))\n(car 1 2)"), "wrong-arity") == 1);
    CHECK(Count(Diags("(use-modules (web uri))\n(build-uri 'http #:host \"x\")"), "wrong-arity") == 0);
    // A macro's operands are not calls.
    CHECK(Count(Diags("(define (f a) a)\n(define-syntax m (syntax-rules () ((_ x) 1)))\n(m (f 1 2))"),
                "wrong-arity") == 0);
    CHECK(Count(Diags("(use-modules (srfi srfi-1))\n(parameterize ((current-output-port p)) 1)"), "wrong-arity") ==
          0);
}

void TestNavigation() {
    const std::string src = "(define (square x)\n  (* x x))\n(define y (square 3))\n";
    const auto doc = SchemeLspAnalyze(L(src));
    // Definition of `square` from its use on line 2.
    const auto defs = SchemeLspDefinition(*doc, 2, 12);
    CHECK(defs.size() == 1 && defs[0].range.line == 0 && defs[0].range.col == 9);
    const auto refs = SchemeLspReferences(*doc, 0, 10);
    CHECK(refs.size() == 2 && refs[0].is_definition && !refs[1].is_definition);
    // Parameter references stay in their lambda.
    CHECK(SchemeLspReferences(*doc, 1, 5).size() == 3);
    const SchemeLspRenameTarget t = SchemeLspPrepareRename(*doc, 2, 12);
    CHECK(t.found && t.placeholder == "square");
    std::string err;
    const auto edits = SchemeLspRename(*doc, 2, 12, "sq", &err);
    CHECK(err.empty() && edits.size() == 2 && edits[1].new_text == "sq");
    CHECK(SchemeLspRename(*doc, 2, 12, "a b", &err).empty() && !err.empty());
    err.clear();
    CHECK(SchemeLspRename(*doc, 2, 12, "12", &err).empty() && !err.empty());
    CHECK(!SchemeLspPrepareRename(*doc, 1, 3).found);  // `*` is Guile's
    // Exports follow a rename.
    const auto doc2 = SchemeLspAnalyze(L("(define-module (m) #:export (f))\n(define (f) 1)"));
    CHECK(SchemeLspReferences(*doc2, 1, 9).size() == 2);
}

void TestHoverSignature() {
    const auto doc = SchemeLspAnalyze(L("(use-modules (srfi srfi-1))\n(define (f a b) a)\n(f 1 2)\n(fold + 0 l)\n"));
    SchemeLspHoverInfo h = SchemeLspHover(*doc, 2, 1);
    CHECK(h.found && h.text.find("(f a b)") != std::string::npos);
    h = SchemeLspHover(*doc, 3, 2);
    CHECK(h.found && h.text.find("(fold ") != std::string::npos && h.text.find("srfi srfi-1") != std::string::npos);
    h = SchemeLspHover(*doc, 1, 12);  // parameter a
    CHECK(h.found && h.text.find("parameter of f") != std::string::npos);
    SchemeLspSignatureHelp s = SchemeLspSignatureAt(*doc, 2, 3);
    CHECK(s.found && s.label == "(f a b)" && s.params.size() == 2 && s.active_param == 0);
    s = SchemeLspSignatureAt(*doc, 2, 5);
    CHECK(s.found && s.active_param == 1);
    s = SchemeLspSignatureAt(*doc, 3, 6);
    CHECK(s.found && s.label.rfind("(fold ", 0) == 0);
}

void TestCompletion() {
    const auto doc = SchemeLspAnalyze(L("(define (alpha-beta x)\n  (let ((alpha-gamma 1))\n    (alpha"));
    const auto items = SchemeLspCompletions(*doc, 2, 10);
    bool fn = false, local = false;
    for (const auto &it : items) {
        if (it.label == "alpha-beta") fn = it.kind == SchemeLspKind::Function;
        if (it.label == "alpha-gamma") local = true;
        CHECK(it.replace_start == 5 && it.replace_end == 10);
    }
    CHECK(fn && local);
    const auto doc2 = SchemeLspAnalyze(L("(call-with-cur"));
    bool found = false;
    for (const auto &it : SchemeLspCompletions(*doc2, 0, 14))
        if (it.label == "call-with-current-continuation") found = true;
    CHECK(found);
    CHECK(SchemeLspCompletions(*doc2, 0, 0).empty() || SchemeLspCompletions(*doc2, 0, 0)[0].label.size() > 0);
}

void TestSymbolsFolds() {
    const std::string src =
        "(define-module (demo core)\n"
        "  #:export (square))\n"
        ";; one\n"
        ";; two\n"
        "(define depth 3)\n"
        "(define (square x)\n"
        "  (* x x))\n"
        "(define-syntax swap!\n"
        "  (syntax-rules () ((_ a b) (let ((t a)) (set! a b) (set! b t)))))\n"
        "(define-record-type point (make-point x y) point? (x point-x) (y point-y))\n";
    const auto doc = SchemeLspAnalyze(L(src));
    const auto syms = SchemeLspSymbols(*doc);
    CHECK(syms.size() == 7);
    CHECK(syms[0].name == "(demo core)" && syms[0].kind == SchemeLspSymbolKind::Module);
    CHECK(syms[1].name == "depth" && syms[1].kind == SchemeLspSymbolKind::Variable);
    CHECK(syms[2].name == "square" && syms[2].kind == SchemeLspSymbolKind::Function);
    CHECK(syms[3].name == "swap!" && syms[4].name == "point" && syms[4].kind == SchemeLspSymbolKind::Class);
    CHECK(syms[5].name == "x" && syms[5].parent == 4 && syms[5].kind == SchemeLspSymbolKind::Field);
    const auto folds = SchemeLspFolds(*doc);
    bool module = false, comment = false, fn = false;
    for (const auto &f : folds) {
        if (f.start_line == 0 && f.end_line == 1) module = true;
        if (f.start_line == 2 && f.end_line == 3 && f.kind == "comment") comment = true;
        if (f.start_line == 5 && f.end_line == 6) fn = true;
    }
    CHECK(module && comment && fn);
}

void TestRobustness() {
    // Every query on every prefix of a file must not crash.
    const std::string src =
        "#!/usr/bin/env guile\n!#\n"
        "(define-module (demo) #:use-module (ice-9 match) #:export (run))\n"
        "(define* (run args #:key (verbose? #f))\n"
        "  (let loop ((xs args) (n 0))\n"
        "    (match xs\n"
        "      (() n)\n"
        "      (((? string? s) . rest) (loop rest (+ n (string-length s))))\n"
        "      ((_ . rest) (loop rest n)))))\n"
        "(define-syntax-rule (twice e) (begin e e))\n"
        "(receive (q r) (floor/ 7 2) `(,q . ,r))\n"
        "#;(ignored) #| block |# #(1 2) #u8(1 2) #~(a #$b)\n";
    for (size_t n = 0; n <= src.size(); n++) {
        const auto lines = L(src.substr(0, n));
        const auto doc = SchemeLspAnalyze(lines);
        const int last = static_cast<int>(lines.size()) - 1;
        const int col = static_cast<int>(lines.back().size());
        SchemeLspDiagnostics(*doc);
        SchemeLspCompletions(*doc, last, col);
        SchemeLspHover(*doc, last, col);
        SchemeLspSignatureAt(*doc, last, col);
        SchemeLspDefinition(*doc, last, col);
        SchemeLspReferences(*doc, last, col);
        SchemeLspPrepareRename(*doc, last, col);
        SchemeLspSymbols(*doc);
        SchemeLspFolds(*doc);
    }
    CHECK(true);
}

void TestFormat() {
    std::string err;
    // One edit over the changed lines: the body gets scheme-mode's two columns.
    const std::vector<SchemeLspTextEdit> e = SchemeLspFormat({"(define (f x)", "(+ x 1))"}, 100, &err);
    CHECK(err.empty());
    CHECK(e.size() == 1);
    CHECK(e.size() == 1 && e[0].new_text.find("  (+ x 1))") != std::string::npos);
    CHECK(SchemeLspFormat({"(define (f x)", "  (+ x 1))"}, 100, &err).empty());
    // A file that does not read is left alone, with the reason.
    CHECK(SchemeLspFormat({"(define (f x)", "  (+ x 1)"}, 100, &err).empty());
    CHECK(!err.empty());
}

}  // namespace

int main() {
    TestReader();
    TestReaderDiagnostics();
    TestDuplicates();
    TestUnused();
    TestArity();
    TestNavigation();
    TestHoverSignature();
    TestCompletion();
    TestSymbolsFolds();
    TestRobustness();
    TestFormat();
    std::printf("scheme_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
