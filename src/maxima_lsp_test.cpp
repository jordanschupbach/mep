// Windowless test for maxima_lsp.cpp and maxima_ast.cpp -- the analysis
// and reading halves of mep's own Maxima language server
// (`mep-maxima-lsp`, src/maxima_lsp_server.cpp). Drives the pure
// functions directly: no process, no JSON-RPC client, no GL context, and
// no Maxima installation. CHECK(), never assert(): the Release build
// strips assert() entirely.
//
// Three conventions, the first two borrowed from org_lsp_test.cpp and
// r_lsp_test.cpp because they are what make a linter's test suite worth
// having, and the third forced by the language:
//   - Diagnostics are asserted by MaximaLspDiagnostic::code, never by
//     message wording, so the messages stay free to improve.
//   - Every check asserts the *absence* of a diagnostic on the
//     legal-but-suspicious shapes it must stay quiet about, as
//     deliberately as it asserts the true reports. A linter that cries
//     wolf gets switched off, and then its true reports go unseen too.
//   - Binding powers are asserted through the *shape of the parse*, not
//     by reading the table back. Maxima's precedences are unusual enough
//     (`^` binds tighter than unary minus; `a : b : c` groups right) that
//     a table copied correctly and applied wrongly would look fine.
#include "maxima_lsp.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "maxima_ast.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

/** @brief Options that never touch the filesystem, so the suite answers the same in any directory. */
MaximaLspOptions NoFiles() {
    MaximaLspOptions opts;
    opts.check_files = false;
    return opts;
}

std::vector<MaximaLspDiagnostic> Lint(const Lines &lines) { return MaximaLspDiagnostics(lines, NoFiles()); }

/** @brief Counts diagnostics carrying a given code. */
size_t CountCode(const std::vector<MaximaLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const MaximaLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

/** @brief Reports whether any diagnostic carries a given code. */
bool HasCode(const std::vector<MaximaLspDiagnostic> &diags, const std::string &code) {
    return CountCode(diags, code) > 0;
}

/** @brief The first diagnostic with a code, or a default-constructed one. */
MaximaLspDiagnostic FindCode(const std::vector<MaximaLspDiagnostic> &diags, const std::string &code) {
    for (const MaximaLspDiagnostic &d : diags) {
        if (d.code == code) return d;
    }
    return MaximaLspDiagnostic{};
}

// --- Parse-shape helpers ----------------------------------------------

/** @brief Renders a parsed expression as a fully-parenthesized string, so precedence is visible. */
std::string Render(const MxParseResult &parse, int node) {
    const MxNode &n = parse.at(node);
    switch (n.kind) {
        case MxNodeKind::Num:
        case MxNodeKind::Ident:
        case MxNodeKind::Lisp: return n.text;
        case MxNodeKind::Str: return "\"" + n.text + "\"";
        case MxNodeKind::Binary:
            return "(" + Render(parse, n.kids[0]) + " " + n.text + " " + Render(parse, n.kids[1]) + ")";
        case MxNodeKind::Unary: return "(" + n.text + Render(parse, n.kids[0]) + ")";
        case MxNodeKind::Postfix: return "(" + Render(parse, n.kids[0]) + n.text + ")";
        case MxNodeKind::Quote: return n.text + Render(parse, n.kids[0]);
        case MxNodeKind::Call: {
            std::string out = Render(parse, n.kids[0]) + "(";
            for (size_t i = 0; i < n.args.size(); i++) {
                if (i > 0) out += ", ";
                out += Render(parse, n.args[i].value);
            }
            return out + ")";
        }
        case MxNodeKind::Index: {
            std::string out = Render(parse, n.kids[0]) + "[";
            for (size_t i = 0; i < n.args.size(); i++) {
                if (i > 0) out += ", ";
                out += Render(parse, n.args[i].value);
            }
            return out + "]";
        }
        case MxNodeKind::List:
        case MxNodeKind::Set:
        case MxNodeKind::Paren: {
            const char *open = n.kind == MxNodeKind::List ? "[" : (n.kind == MxNodeKind::Set ? "{" : "(");
            const char *close = n.kind == MxNodeKind::List ? "]" : (n.kind == MxNodeKind::Set ? "}" : ")");
            std::string out = open;
            for (size_t i = 0; i < n.kids.size(); i++) {
                if (i > 0) out += ", ";
                out += Render(parse, n.kids[i]);
            }
            return out + close;
        }
        case MxNodeKind::If: {
            std::string out = "if";
            for (const MxArg &arg : n.args) out += " " + arg.name + " " + Render(parse, arg.value);
            return out;
        }
        case MxNodeKind::Do: {
            std::string out = "do";
            for (const MxArg &arg : n.args) out += " " + arg.name + " " + Render(parse, arg.value);
            return out;
        }
        case MxNodeKind::Label: return "&&" + n.text + " " + Render(parse, n.kids[1]);
        case MxNodeKind::LispEsc: return ":lisp";
        case MxNodeKind::Error: return "<error>";
    }
    return "?";
}

/** @brief Parses one line and renders its first statement fully parenthesized. */
std::string Shape(const std::string &source) {
    const MxParseResult parse = MxParse({source});
    if (parse.statements.empty()) return "<none>";
    return Render(parse, parse.statements.front().node);
}

}  // namespace

int main() {
    // --- The reader: tokens ------------------------------------------
    {
        std::vector<MxComment> comments;
        std::vector<MxParseError> errors;
        const std::vector<MxToken> toks =
            MxTokenize({"f(x) := x^2$", "/* a /* nested */ comment */ g : 1;"}, &comments, &errors);
        CHECK(errors.empty());
        // A nested comment is one comment: Maxima counts depth, and a
        // reader that stopped at the first `*/` would read the rest of
        // the line as code.
        CHECK(comments.size() == 1);
        CHECK(toks.front().kind == MxTokKind::Ident && toks.front().text == "f");
        // `$` and `;` are terminators, not operators.
        bool saw_dollar = false;
        for (const MxToken &t : toks) {
            if (t.kind == MxTokKind::Dollar) saw_dollar = true;
        }
        CHECK(saw_dollar);
    }
    {
        // Numbers, in every shape Maxima's own scanner accepts.
        for (const char *literal : {"1", "1.5", ".5", "1.", "1e3", "1.5e-3", "1.5b0", "2d0", "3s1", "4l2", "5f3"}) {
            const std::vector<MxToken> toks = MxTokenize({std::string(literal) + "$"}, nullptr, nullptr);
            CHECK(toks.front().kind == MxTokKind::Num);
            CHECK(toks.front().text == literal);
        }
        // `2.3.4` is two numbers touching, which is exactly why Maxima
        // rejects it: the dot before a digit starts a number.
        const std::vector<MxToken> toks = MxTokenize({"2.3.4$"}, nullptr, nullptr);
        CHECK(toks.size() >= 2);
        CHECK(toks[0].kind == MxTokKind::Num && toks[0].text == "2.3");
        CHECK(toks[1].kind == MxTokKind::Num && toks[1].text == ".4");
    }
    {
        // A backslash at end of line is deleted before anything else sees
        // it -- inside a name, inside a number, or between tokens.
        const MxParseResult joined = MxParse({"ab\\", "cd : 1$"});
        CHECK(joined.errors.empty());
        CHECK(Render(joined, joined.statements.front().node) == "(abcd : 1)");
        const MxParseResult split_number = MxParse({"x : 123\\", "456$"});
        CHECK(split_number.errors.empty());
        CHECK(Render(split_number, split_number.statements.front().node) == "(x : 123456)");
    }
    {
        // A form feed is whitespace: Maxima's own packages put page
        // breaks between sections.
        const MxParseResult parse = MxParse({"x : 1$", "\f", "y : 2$"});
        CHECK(parse.errors.empty());
        CHECK(parse.statements.size() == 2);
    }
    {
        // An escaped character is part of the name, which is how a
        // Maxima symbol can contain a space.
        const std::vector<MxToken> toks = MxTokenize({"a\\ b : 1$"}, nullptr, nullptr);
        CHECK(toks.front().kind == MxTokKind::Ident);
        CHECK(toks.front().text == "a b");
    }

    // --- The reader: precedence --------------------------------------
    {
        // Maxima's own binding powers, asserted through the parse.
        CHECK(Shape("a + b * c$") == "(a + (b * c))");
        CHECK(Shape("a * b + c$") == "((a * b) + c)");
        // `^` is right-associative and binds tighter than unary minus,
        // so `-x^2` is the negation of a square.
        CHECK(Shape("a^b^c$") == "(a ^ (b ^ c))");
        CHECK(Shape("-x^2$") == "(-(x ^ 2))");
        // ... but looser than nothing else: `-a*b` negates `a`.
        CHECK(Shape("-a*b$") == "((-a) * b)");
        CHECK(Shape("1 - 2 - 3$") == "((1 - 2) - 3)");
        CHECK(Shape("2/3/4$") == "((2 / 3) / 4)");
        // Assignment groups to the right and swallows everything to its
        // right up to the terminator.
        CHECK(Shape("a : b : c$") == "(a : (b : c))");
        CHECK(Shape("a : b + c$") == "(a : (b + c))");
        // A comparison is looser than arithmetic, `and` looser than a
        // comparison, `or` loosest of the three.
        CHECK(Shape("a + b < c and d$") == "(((a + b) < c) and d)");
        CHECK(Shape("a and b or c$") == "((a and b) or c)");
        CHECK(Shape("not a and b$") == "((nota) and b)");
        // Postfix factorial binds tighter than arithmetic.
        CHECK(Shape("2*n!$") == "(2 * (n!))");
        // The non-commutative product sits between `*` and `^`.
        CHECK(Shape("a . b + c$") == "((a . b) + c)");
        CHECK(Shape("a . b^2$") == "(a . (b ^ 2))");
    }
    {
        // A comma at statement level is Maxima's `ev` shorthand; inside
        // an argument list it separates arguments.
        CHECK(Shape("integrate(x, x), x = 2$") == "(integrate(x, x) , (x = 2))");
        CHECK(Shape("f(a, b)$") == "f(a, b)");
        // Subscripts and calls chain left to right.
        CHECK(Shape("m[1][2]$") == "m[1][2]");
        CHECK(Shape("f(x)[1]$") == "f(x)[1]");
    }
    {
        // Quoting: `'x` is a symbol and `''x` is an extra evaluation.
        CHECK(Shape("'x$") == "'x");
        CHECK(Shape("''x$") == "''x");
        CHECK(Shape("?gensym$") == "?gensym");
    }

    // --- The reader: statements and loops ----------------------------
    {
        const MxParseResult parse = MxParse({"if a then b elseif c then d else e$"});
        CHECK(parse.errors.empty());
        CHECK(Render(parse, parse.statements.front().node) == "if cond a then b cond c then d else e");
    }
    {
        // Every loop is one construct with a bag of clauses. `for i : 1`
        // is `for i from 1` -- inside a loop header, `:` is not an
        // assignment.
        CHECK(Shape("for i : 1 thru 10 step 2 do f(i)$") == "do for i from 1 thru 10 step 2 do f(i)");
        CHECK(Shape("for x in L do f(x)$") == "do for x in L do f(x)");
        CHECK(Shape("while a < b do c$") == "do while (a < b) do c");
        CHECK(Shape("do f()$") == "do do f()");
    }
    {
        // `name && expr` tags a statement, and the tag is not a use of
        // anything.
        const MxParseResult parse = MxParse({"myrule&& f(x) := x$"});
        CHECK(parse.errors.empty());
        CHECK(parse.statements.size() == 1);
        CHECK(parse.at(parse.statements.front().node).kind == MxNodeKind::Label);
    }
    {
        // `:lisp` drops into the host Lisp for the rest of the line and
        // is not read as Maxima.
        const MxParseResult parse = MxParse({":lisp (defun foo (x) (* x x))", "y : 1$"});
        CHECK(parse.errors.empty());
        CHECK(parse.statements.size() == 2);
        CHECK(parse.at(parse.statements.front().node).kind == MxNodeKind::LispEsc);
    }
    {
        // `in` is an ordinary name outside a loop header -- it is the one
        // loop word Maxima gives no prefix meaning, and its
        // physical-units package really does write `in : 0.0254 *
        // meter`. `from` is not: Maxima rejects `from : 2` too, because
        // `from` does start a loop.
        const MxParseResult parse = MxParse({"in : 1$", "print(in + 1)$"});
        CHECK(parse.errors.empty());
        CHECK(Render(parse, parse.statements.front().node) == "(in : 1)");
        CHECK(!MxParse({"from : 2$"}).errors.empty());
    }

    // --- The reader: errors and recovery -----------------------------
    {
        // Every statement needs a terminator; the file ending without
        // one is the mistake Maxima's own error message is worst at
        // explaining.
        CHECK(HasCode(Lint({"x : 1"}), "missing-terminator"));
        CHECK(!HasCode(Lint({"x : 1$"}), "missing-terminator"));
        // Two statements run together produce one report, not a cascade.
        const std::vector<MaximaLspDiagnostic> run_on = Lint({"a : 1", "b : 2$"});
        CHECK(HasCode(run_on, "syntax-error"));
        CHECK(CountCode(run_on, "syntax-error") == 1);
        // A bad statement does not lose the rest of the file.
        const MxParseResult recovered = MxParse({"a : ($", "b : 2$", "c : 3$"});
        CHECK(!recovered.errors.empty());
        CHECK(recovered.statements.size() >= 2);
    }
    {
        // Juxtaposition: `8(3*x)` and `1[t]` are what a dropped operator
        // looks like, and Maxima refuses both outright. Found by
        // mutation testing -- deleting the `+` from an expression
        // produced exactly this and nothing here noticed.
        CHECK(HasCode(Lint({"e : (5*y^2)/8(3*x*y)/4$"}), "syntax-error"));
        CHECK(HasCode(Lint({"e : f(1[t, 0, 1])$"}), "syntax-error"));
        CHECK(!HasCode(Lint({"e : 8*(3*x)$"}), "syntax-error"));
    }
    {
        // `(f)(x) := ...` is `f(x) := ...`: Maxima's reader collapses a
        // one-expression parenthesis away, so this is a definition and
        // not a shape to complain about.
        CHECK(!HasCode(Lint({"(square)(x) := x^2$"}), "bad-definition-lhs"));
        CHECK(!HasCode(Lint({"(t) := sin(t)$"}), "bad-definition-lhs"));
    }
    {
        CHECK(HasCode(Lint({"f(x := x^2$"}), "unclosed-paren"));
        CHECK(HasCode(Lint({"L : [1, 2$"}), "unclosed-bracket"));
        CHECK(HasCode(Lint({"s : \"unterminated$"}), "unterminated-string"));
        CHECK(HasCode(Lint({"/* never closed", "x : 1$"}), "unterminated-comment"));
        CHECK(HasCode(Lint({"if a b$"}), "missing-then"));
        CHECK(HasCode(Lint({"for i : 1 thru 3 f(i)$"}), "missing-do"));
        CHECK(HasCode(Lint({";"}), "empty-statement"));
    }
    {
        // An operator nothing declares is read as an operator anyway and
        // reported once, not once per use: the spelling is nearly always
        // one a package the file loads brings in.
        const std::vector<MaximaLspDiagnostic> diags = Lint({"a ~ b$", "c ~ d$", "e ~ f$"});
        CHECK(CountCode(diags, "undeclared-operator") == 1);
        CHECK(FindCode(diags, "undeclared-operator").severity == MaximaLspSeverity::Information);
        CHECK(!HasCode(diags, "syntax-error"));
    }
    {
        // A file that declares its own operator is read with it, and says
        // nothing.
        const Lines lines = {"infix(\"~~\")$", "a ~~ b$"};
        CHECK(Lint(lines).empty());
        const MxParseResult parse = MxParse(lines);
        CHECK(parse.user_operators.size() == 1);
        CHECK(parse.user_operators.front().text == "~~");
        CHECK(Render(parse, parse.statements[1].node) == "(a ~~ b)");
        // A declared prefix operator takes an operand; an undeclared
        // word never does, because that is what a missing `;` looks
        // like.
        const MxParseResult prefixed = MxParse({"prefix(\"grad\")$", "grad f$"});
        CHECK(prefixed.errors.empty());
        CHECK(Render(prefixed, prefixed.statements[1].node) == "(gradf)");
        CHECK(HasCode(Lint({"print(1)$", "foo bar$"}), "syntax-error"));
    }
    {
        // `load("vect.mac")` is a declaration of `~` written somewhere
        // else, and the reader follows it.
        const Lines lines = {"load(\"vect.mac\")$", "a ~ b$"};
        const MxParseResult parse = MxParse(lines);
        CHECK(parse.errors.empty());
        CHECK(Render(parse, parse.statements[1].node) == "(a ~ b)");
        CHECK(!HasCode(Lint(lines), "undeclared-operator"));
    }

    // --- Diagnostics: scope ------------------------------------------
    {
        // A block local nobody reads is worth saying; one that was given
        // a value is not, because Maxima binds globals dynamically and
        // `block([ratprint : false], ...)` is how a caller passes a
        // setting to a callee.
        CHECK(HasCode(Lint({"f() := block([unused], 1)$"}), "unused-local"));
        CHECK(!HasCode(Lint({"f() := block([used], used : 1, used)$"}), "unused-local"));
        CHECK(!HasCode(Lint({"f() := block([ratprint : false], g())$"}), "unused-local"));
        // Declaring a loop variable local and then looping over it is the
        // idiom, not a leftover.
        CHECK(!HasCode(Lint({"f(n) := block([i], for i : 1 thru n do print(i))$"}), "unused-local"));
        // So is declaring a name local and then defining a function of
        // that name.
        CHECK(!HasCode(Lint({"block(local(g), g(x) := x^2, g(2))$"}), "unused-local"));
        // A parameter is part of an interface, so an unused one is never
        // reported.
        CHECK(!HasCode(Lint({"f(x, t) := x^2$"}), "unused-parameter"));
        // Destructuring assignment writes each name in the list.
        CHECK(!HasCode(Lint({"f(p) := block([a, b], [a, b] : p, a + b)$"}), "unused-local"));
    }
    {
        // A name bound by makelist/sum/product over its own expression.
        CHECK(Lint({"L : makelist(i^2, i, 1, 10)$"}).empty());
        CHECK(Lint({"s : sum(k, k, 1, n)$"}).empty());
        // `buildq` binds its list over the template *and* reads the
        // names out here.
        CHECK(!HasCode(Lint({"f(x) := block([y : x], buildq([y], y + 1))$"}), "unused-local"));
    }
    {
        // Duplicates in a parameter list or a local list.
        CHECK(HasCode(Lint({"f(x, x) := x$"}), "duplicate-parameter"));
        CHECK(HasCode(Lint({"f() := block([a, a], a : 1, a)$"}), "duplicate-local"));
    }

    // --- Diagnostics: the traps the language sets --------------------
    {
        // Assigning to a constant fails at run time, and it is the one
        // assignment Maxima refuses.
        CHECK(HasCode(Lint({"%pi : 3$"}), "assign-to-protected"));
        CHECK(FindCode(Lint({"%pi : 3$"}), "assign-to-protected").severity == MaximaLspSeverity::Error);
        CHECK(!HasCode(Lint({"pi : 3$"}), "assign-to-protected"));
    }
    {
        // `=` in a statement position builds an equation and throws it
        // away; `:` is the assignment. Only where the value is actually
        // discarded -- the last expression of a block is its value.
        CHECK(HasCode(Lint({"f() := block([], x = 1, 2)$"}), "equation-as-statement"));
        CHECK(!HasCode(Lint({"f() := block([], 2, x = 1)$"}), "equation-as-statement"));
        CHECK(!HasCode(Lint({"solve(x^2 = 1, x)$"}), "equation-as-statement"));
    }
    {
        // Maxima's own loop-clause collision table.
        CHECK(HasCode(Lint({"for x in L step 2 do f(x)$"}), "loop-clause-conflict"));
        CHECK(HasCode(Lint({"for i : 1 thru 3 step 2 next i+1 do f(i)$"}), "loop-clause-conflict"));
        CHECK(!HasCode(Lint({"for i : 1 thru 3 step 2 do f(i)$"}), "loop-clause-conflict"));
        CHECK(!HasCode(Lint({"for i : 1 while i < 3 unless done do f(i)$"}), "loop-clause-conflict"));
    }
    {
        // A definition whose left side is not a name, a call or a
        // subscript is one Maxima refuses outright.
        CHECK(HasCode(Lint({"1 + 2 := 3$"}), "bad-definition-lhs"));
        CHECK(!HasCode(Lint({"f(x) := x$"}), "bad-definition-lhs"));
        CHECK(!HasCode(Lint({"f[i] := i^2$"}), "bad-definition-lhs"));
        CHECK(!HasCode(Lint({"f(x) ::= buildq([x], x^2)$"}), "bad-definition-lhs"));
    }
    {
        // Calling a function this file defines with the wrong number of
        // arguments.
        CHECK(HasCode(Lint({"f(x) := x^2$", "f(1, 2)$"}), "wrong-argument-count"));
        CHECK(!HasCode(Lint({"f(x) := x^2$", "f(1)$"}), "wrong-argument-count"));
        // A default makes an argument optional; a `[rest]` parameter
        // makes every count acceptable.
        CHECK(!HasCode(Lint({"f(x, y : 1) := x + y$", "f(1)$"}), "wrong-argument-count"));
        CHECK(!HasCode(Lint({"f([args]) := args$", "f(1, 2, 3)$"}), "wrong-argument-count"));
        // A call inside something that does not evaluate its arguments is
        // syntax, not a call.
        CHECK(!HasCode(Lint({"f(a, b, c) := a$", "gentran(while f(x) > 0 do x : x + 1)$"}), "wrong-argument-count"));
        // A file that takes definitions away again cannot be checked
        // against the definition that is still visible here.
        CHECK(!HasCode(Lint({"f(x) := x^2$", "kill(f)$", "f(1, 2)$"}), "wrong-argument-count"));
        // Nothing is ever checked against the manual's own argument
        // lists: they are a sample, and `return()` is the proof.
        CHECK(!HasCode(Lint({"f() := block([], return(), 1)$"}), "wrong-argument-count"));
        CHECK(!HasCode(Lint({"x : subst(a = b, e)$"}), "wrong-argument-count"));
    }
    {
        // Code after a `return` in the same block never runs.
        CHECK(HasCode(Lint({"f() := block([], return(1), 2)$"}), "unreachable-after-return"));
        CHECK(!HasCode(Lint({"f() := block([], if a then return(1), 2)$"}), "unreachable-after-return"));
    }
    {
        // An unknown *name* is not an error in a computer-algebra
        // system: `f(x)` with no `f` is a noun expression, and the
        // mathematics is full of them.
        CHECK(!HasCode(Lint({"e : myfunc(x) + otherfunc(y)$"}), "unknown-function"));
        // A name used once, one edit from a core name, in a file that
        // loads nothing, is the one case worth mentioning.
        CHECK(HasCode(Lint({"y : intergrate(x^2, x)$"}), "unknown-function") ||
              HasCode(Lint({"y : integrat(x^2, x)$"}), "unknown-function"));
        // ... and a file that loads a package is not that case.
        CHECK(!HasCode(Lint({"load(mypackage)$", "y : integrat(x^2, x)$"}), "unknown-function"));
    }
    {
        // Quiet on ordinary, correct Maxima.
        CHECK(Lint({"/* the golden ratio */", "phi : (1 + sqrt(5)) / 2$", "float(phi)$"}).empty());
        CHECK(Lint({"f(x) := x^2 + 1$", "integrate(f(x), x, 0, 1)$"}).empty());
        CHECK(Lint({"m : matrix([1, 2], [3, 4])$", "m . m$", "determinant(m)$"}).empty());
        CHECK(Lint({"declare(n, integer)$", "assume(n > 0)$", "is(n > 0)$"}).empty());
        CHECK(Lint({"L : [1, 2, 3]$", "map(lambda([u], u^2), L)$"}).empty());
        CHECK(Lint({"plot2d(sin(x), [x, 0, 2*%pi])$"}).empty());
    }

    // --- Hover --------------------------------------------------------
    {
        const MaximaLspHoverInfo h = MaximaLspHover({"y : integrate(x^2, x)$"}, 0, 6);
        CHECK(h.found);
        CHECK(h.text.find("integrate") != std::string::npos);
        // The range covers the name, not the whole line.
        CHECK(h.range.col == 4 && h.range.end_col == 13);
    }
    {
        // A name this file defines beats the manual.
        const MaximaLspHoverInfo h = MaximaLspHover({"foo(x, y) := x + y$", "foo(1, 2)$"}, 1, 1);
        CHECK(h.found);
        CHECK(h.text.find("foo(x, y)") != std::string::npos);
    }
    {
        // An unbound symbol is a real thing to be, and hover says so
        // rather than staying silent.
        const MaximaLspHoverInfo h = MaximaLspHover({"e : alpha + beta$"}, 0, 5);
        CHECK(h.found);
        CHECK(h.text.find("symbol") != std::string::npos);
    }
    {
        // The operators are what a newcomer gets wrong, so they hover.
        const MaximaLspHoverInfo assign = MaximaLspHover({"x : 1$"}, 0, 2);
        CHECK(assign.found && assign.text.find("Assignment") != std::string::npos);
        const MaximaLspHoverInfo equation = MaximaLspHover({"x = 1$"}, 0, 2);
        CHECK(equation.found && equation.text.find("equation") != std::string::npos);
    }
    {
        const MaximaLspHoverInfo constant = MaximaLspHover({"c : %pi$"}, 0, 5);
        CHECK(constant.found && constant.text.find("circle") != std::string::npos);
    }

    // --- Completion ---------------------------------------------------
    {
        const std::vector<MaximaLspCompletionItem> items =
            MaximaLspCompletions({"myvariable : 1$", "myv"}, 1, 3, NoFiles());
        CHECK(!items.empty());
        CHECK(items.front().label == "myvariable");
        // The replaced span is exactly the typed prefix.
        CHECK(items.front().replace_start == 0 && items.front().replace_end == 3);
    }
    {
        // What is in scope here comes before what the manual has.
        const std::vector<MaximaLspCompletionItem> items =
            MaximaLspCompletions({"f(x) := block([integrate_this], integ)$"}, 0, 36, NoFiles());
        CHECK(!items.empty());
        CHECK(items.front().label == "integrate_this");
        bool has_builtin = false;
        for (const MaximaLspCompletionItem &item : items) {
            if (item.label == "integrate") has_builtin = true;
        }
        CHECK(has_builtin);
    }
    {
        // Every candidate is single-line: mep splices it into one buffer
        // line.
        for (const MaximaLspCompletionItem &item : MaximaLspCompletions({"tri"}, 0, 3, NoFiles())) {
            CHECK(item.insert_text.find('\n') == std::string::npos);
        }
    }

    // --- Symbols, folds, definition, references -----------------------
    {
        const Lines lines = {
            "/* ---------- Helpers ---------- */",
            "square(x) := x^2$",
            "cube[i] := i^3$",
            "limit_value : 10$",
        };
        const std::vector<MaximaLspSymbol> syms = MaximaLspSymbols(lines);
        CHECK(syms.size() == 4);
        CHECK(syms[0].kind == MaximaLspSymbolKind::Namespace);
        CHECK(syms[0].name == "Helpers");
        CHECK(syms[1].name == "square" && syms[1].kind == MaximaLspSymbolKind::Function);
        CHECK(syms[2].name == "cube" && syms[2].kind == MaximaLspSymbolKind::Array);
        CHECK(syms[3].name == "limit_value" && syms[3].kind == MaximaLspSymbolKind::Variable);
        // The banner owns what follows it.
        CHECK(syms[1].parent == 0 && syms[3].parent == 0);
    }
    {
        const std::vector<MaximaLspFold> folds = MaximaLspFolds({
            "f(n) := block([acc : 0],",
            "  for i : 1 thru n do",
            "    acc : acc + i,",
            "  acc)$",
        });
        CHECK(!folds.empty());
        CHECK(folds.front().start_line == 0 && folds.front().end_line == 3);
    }
    {
        const Lines lines = {"helper(x) := x + 1$", "y : helper(2)$"};
        const MaximaLspLocation loc = MaximaLspDefinition(lines, 1, 5, NoFiles());
        CHECK(loc.found);
        CHECK(loc.path.empty());  // this document
        CHECK(loc.range.line == 0 && loc.range.col == 0);
    }
    {
        const Lines lines = {"count : 0$", "count : count + 1$", "print(count)$"};
        const std::vector<MaximaLspReference> refs = MaximaLspReferences(lines, 2, 7);
        CHECK(refs.size() == 4);
        CHECK(refs.front().is_definition);
        // The assignment on line 2 is a write; the read on its right side
        // is not.
        bool saw_write = false;
        bool saw_read = false;
        for (const MaximaLspReference &ref : refs) {
            if (ref.range.line != 1) continue;
            if (ref.is_write) saw_write = true;
            if (!ref.is_write) saw_read = true;
        }
        CHECK(saw_write && saw_read);
    }
    {
        // A free symbol is the subject of a computer-algebra document,
        // so its occurrences are still worth finding.
        const std::vector<MaximaLspReference> refs =
            MaximaLspReferences({"e : integrate(x^2, x)$", "diff(e, x)$"}, 0, 14);
        CHECK(refs.size() >= 3);
    }

    // --- Signature help -----------------------------------------------
    {
        const MaximaLspSignature sig = MaximaLspSignatureHelp({"y : integrate(x^2, "}, 0, 19);
        CHECK(sig.found);
        CHECK(sig.label.find("integrate(") == 0);
        CHECK(sig.active_param == 1);
        // Maxima overloads on arity, so the other forms are offered too.
        CHECK(!sig.alternatives.empty());
    }
    {
        // A function defined in this file beats the manual.
        const MaximaLspSignature sig = MaximaLspSignatureHelp({"mine(a, b) := a + b$", "mine(1, "}, 1, 8);
        CHECK(sig.found);
        CHECK(sig.label == "mine(a, b)");
        CHECK(sig.params.size() == 2);
        CHECK(sig.active_param == 1);
    }
    {
        // Not inside a call at all.
        CHECK(!MaximaLspSignatureHelp({"x : 1$"}, 0, 5).found);
    }

    // --- The generated vocabulary -------------------------------------
    {
        CHECK(MaximaLspIsBuiltin("integrate"));
        CHECK(MaximaLspIsBuiltin("draw2d"));
        CHECK(!MaximaLspIsBuiltin("definitely_not_a_maxima_name"));
        const MaximaBuiltinDecl *decl = MaximaLspFindBuiltin("draw2d");
        CHECK(decl != nullptr);
        CHECK(std::string(decl->package) == "draw");
        // A core name carries no package: `integrate` is always there.
        const MaximaBuiltinDecl *core = MaximaLspFindBuiltin("integrate");
        CHECK(core != nullptr && core->package[0] == '\0');
        // The table is sorted, which is what makes the lookup a binary
        // search.
        for (size_t i = 1; i < kMaximaBuiltinNameCount; i++) {
            CHECK(std::string(kMaximaBuiltinNames[i - 1]) < std::string(kMaximaBuiltinNames[i]));
        }
        CHECK(MaximaLspBuiltinNameList().size() == kMaximaBuiltinNameCount);
    }
    {
        CHECK(MaximaLspIsProtectedName("%pi"));
        CHECK(!MaximaLspIsProtectedName("x"));
        CHECK(MaximaLspFindVocab(":") != nullptr);
        CHECK(MaximaLspFindVocab("block") != nullptr);
        CHECK(MaximaLspFindVocab("not_a_name") == nullptr);
        // Every explained name is spelled the way the language spells it.
        for (const MaximaLspVocabEntry &e : MaximaLspKeywordVocab()) CHECK(e.name[0] != '\0' && e.doc[0] != '\0');
        for (const MaximaLspVocabEntry &e : MaximaLspOperatorVocab()) CHECK(MxFindOperator(e.name) != nullptr ||
                                                                            std::string(e.name) == "," ||
                                                                            std::string(e.name) == "$" ||
                                                                            std::string(e.name) == ";" ||
                                                                            std::string(e.name) == "'" ||
                                                                            std::string(e.name) == "''" ||
                                                                            std::string(e.name) == "&&");
    }
    {
        CHECK(MaximaLspSplitSignatures("f(a) | f(a, b)").size() == 2);
        CHECK(MaximaLspSplitParams("integrate(expr, x, a, b)").size() == 4);
        CHECK(MaximaLspSplitParams("block([v], expr)").size() == 2);
        CHECK(MaximaLspSplitParams("reset").empty());
    }

    std::printf("maxima-lsp tests passed\n");
    return 0;
}
