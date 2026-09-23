// Coverage for maxima_format.h/.cpp: mep's in-house Maxima formatter.
// Every case below is an input/expected-output pair checked twice --
// once for the formatting itself and once fed back through Format() to
// prove the result is a fixed point, since a formatter that keeps
// changing its own output is a formatter `gf` cannot be run twice on.
//
// The properties that matter most for something that rewrites the user's
// buffer in place get their own sections: a file it cannot read comes
// back byte-identical with ok == false, the tokens of the output always
// match the tokens of the input (Format()'s own self-check), and the
// things Maxima cannot afford to have rewritten -- a statement's `;`
// versus `$`, a string's escapes, a comment -- come through untouched.

#include "maxima_format.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

std::string Fmt(const std::string &src, int width = 80) {
    mxfmt::Options opt;
    opt.width = width;
    mxfmt::Result r = mxfmt::Format(src, opt);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line, r.error.c_str(),
                     src.c_str());
        std::abort();
    }
    return r.text;
}

// Formats `src`, checks it against `want`, then re-formats the result and
// checks that nothing moved the second time.
void Same(const std::string &src, const std::string &want, int width = 80) {
    const std::string got = Fmt(src, width);
    if (got != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s", src.c_str(),
                     want.c_str(), got.c_str());
        std::abort();
    }
    const std::string again = Fmt(got, width);
    if (again != got) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- first ---\n%s--- second ---\n%s", got.c_str(), again.c_str());
        std::abort();
    }
}

/** @brief Checks that a source is left exactly as it is. */
void Unchanged(const std::string &src) { Same(src, src); }

}  // namespace

int main() {
    // --- Spacing: structure is spaced, arithmetic is dense ------------
    {
        Same("x:1$\n", "x : 1$\n");
        Same("f(x):=x^2+1$\n", "f(x) := x^2+1$\n");
        Same("y : a*b - c/d$\n", "y : a*b-c/d$\n");
        // Comparisons, logic and equations are structure.
        Same("p:a>0 and b#c$\n", "p : a > 0 and b # c$\n");
        Same("solve(x^2=1,x)$\n", "solve(x^2 = 1, x)$\n");
        // A call's parenthesis is tight; its arguments are separated by
        // a space.
        Same("f ( a , b )$\n", "f(a, b)$\n");
        Same("m[i,j]$\n", "m[i, j]$\n");
        // The non-commutative product always keeps its spaces: `1 . 2`
        // written tight is the number 1.2.
        Same("a.b$\n", "a . b$\n");
        Same("1 . 2$\n", "1 . 2$\n");
        // Prefix and postfix operators are tight; `not` is a word.
        Same("z : -x!$\n", "z : -x!$\n");
        Same("q : not  a$\n", "q : not a$\n");
        Same("v : 'f(x)$\n", "v : 'f(x)$\n");
    }

    // --- The terminator is meaning, not layout ------------------------
    {
        // `;` displays the result and `$` does not, so neither is ever
        // rewritten into the other.
        Same("a:1;\n", "a : 1;\n");
        Same("a:1$\n", "a : 1$\n");
        Same("a:1$ b:2;\n", "a : 1$\nb : 2;\n");
    }

    // --- Literals come through exactly as written ---------------------
    {
        // A string's escapes are the author's: `"\n"` is the one-letter
        // string `n` in Maxima, and re-spelling it would be a change.
        Unchanged("s : \"a\\\"b\"$\n");
        Unchanged("s : \"tab\\there\"$\n");
        // Numbers keep their own spelling, bigfloats and all.
        Unchanged("n : 1.5b0$\n");
        Unchanged("n : .5$\n");
        Unchanged("n : 1.5e-3$\n");
        // A `\`-escaped name is a name.
        Unchanged("a\\ b : 1$\n");
        // A host-Lisp symbol is not Maxima's to reformat.
        Unchanged("x : ?gensym$\n");
    }

    // --- Statements, blanks and comments ------------------------------
    {
        // Blank runs between statements collapse to one.
        Same("a : 1$\n\n\n\nb : 2$\n", "a : 1$\n\nb : 2$\n");
        // A comment on its own line stays on its own line, and the gap
        // between it and what it describes is kept.
        Unchanged("/* about b */\nb : 2$\n");
        Unchanged("/* about b */\n\nb : 2$\n");
        // A comment after code trails the statement it is on.
        Same("b:2$ /* why */\n", "b : 2$ /* why */\n");
        // A comment inside a construct is attached to what follows it,
        // which is what stops it being dropped when the call is
        // re-wrapped.
        Same("f(a, /* second */ b)$\n",
             "f(\n"
             "  a,\n"
             "  /* second */\n"
             "  b)$\n");
        // A multi-line comment keeps its own interior layout.
        Unchanged("/* one\n   two */\nx : 1$\n");
    }

    // --- Breaking ------------------------------------------------------
    {
        // A block keeps its local list on the opening line and puts one
        // statement per line under it -- the shape `grind` prints and
        // the shape the shipped packages are written in.
        Same("f(x,y):=block([a:1,b],a:x+y,if a>0 then b:a else b:-a,return(b))$\n",
             "f(x, y) := block([a : 1, b],\n"
             "  a : x+y,\n"
             "  if a > 0 then b : a else b : -a,\n"
             "  return(b))$\n");
        // A call that does not fit packs its arguments to the margin.
        Same("result : some_function(alpha, beta, gamma, delta, epsilon, zeta, eta, theta)$\n",
             "result : some_function(\n"
             "  alpha, beta, gamma, delta, epsilon, zeta, eta, theta)$\n",
             60);
        // A list is data: it packs rather than exploding one per line.
        Same("L : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18]$\n",
             "L : [\n"
             "  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,\n"
             "  18]$\n",
             60);
        // A long conditional breaks before each keyword.
        Same("if first_condition_here then first_result_here else second_result_here$\n",
             "if first_condition_here\n"
             "  then first_result_here\n"
             "  else second_result_here$\n",
             40);
        // A loop keeps its clauses together and breaks its body.
        Same("for i : 1 thru n do print(one_thing, another_thing, a_third_thing)$\n",
             "for i : 1 thru n do print(\n"
             "  one_thing, another_thing, a_third_thing)$\n",
             50);
        // An assignment whose right side has nowhere better to break
        // moves the right side down a line.
        Same("some_long_variable_name : first_term+second_term+third_term$\n",
             "some_long_variable_name :\n"
             "  first_term+second_term+third_term$\n",
             40);
    }

    // --- Things the reader knows and a formatter must respect ---------
    {
        // A loop clause keeps the spelling it was written with: `for i :
        // 1` and `for i from 1` are the same loop, and rewriting one
        // into the other would change the tokens.
        Unchanged("for i : 1 thru 3 do print(i)$\n");
        Unchanged("for i from 1 thru 3 do print(i)$\n");
        // A `:lisp` line is host-Lisp source and is emitted verbatim.
        Unchanged(":lisp (defun foo (x) (* x x))\n");
        // A `&&` statement tag survives.
        Same("mytag&&f(x):=x$\n", "mytag && f(x) := x$\n");
        // An operator the file declares for itself is read and printed
        // as one operator, not as two.
        Same("infix(\"@@\")$\na@@b$\n", "infix(\"@@\")$\na @@ b$\n");
        // ... and a declared operator that could weld itself onto the
        // next name gets the space that keeps it apart. `infix("/_")`
        // means `a/_b` would come back as `a /_ b`, so the division has
        // to be written with a space.
        Same("infix(\"/_\", 150, 150)$\nx : tend/_u$\n", "infix(\"/_\", 150, 150)$\nx : tend /_ u$\n");
        Same("infix(\"/_\", 150, 150)$\nx : a/b$\n", "infix(\"/_\", 150, 150)$\nx : a/b$\n");
    }

    // --- Refusals -----------------------------------------------------
    {
        // A file that cannot be read comes back byte-identical.
        const std::string broken = "f(x :=  $\n";
        mxfmt::Result r = mxfmt::Format(broken, mxfmt::Options());
        CHECK(!r.ok);
        CHECK(r.text == broken);
        CHECK(!r.error.empty());
        CHECK(r.error_line > 0);

        // A missing terminator is a reason to refuse: the fix is the
        // author's, not the formatter's.
        mxfmt::Result unterminated = mxfmt::Format("x : 1\n", mxfmt::Options());
        CHECK(!unterminated.ok);
        CHECK(unterminated.text == "x : 1\n");

        // Nesting deeper than the reader's own limit is reported rather
        // than run off the stack -- `gf` runs inside the editor.
        std::string deep = "x : ";
        for (int i = 0; i < 5000; i++) deep += "(";
        deep += "1";
        for (int i = 0; i < 5000; i++) deep += ")";
        deep += "$\n";
        mxfmt::Result nested = mxfmt::Format(deep, mxfmt::Options());
        CHECK(!nested.ok);
        CHECK(nested.text == deep);
    }

    // --- Empty and whitespace-only input ------------------------------
    {
        mxfmt::Result empty = mxfmt::Format("", mxfmt::Options());
        CHECK(empty.ok);
        CHECK(empty.text == "\n");
        mxfmt::Result blank = mxfmt::Format("\n\n\n", mxfmt::Options());
        CHECK(blank.ok);
        CHECK(blank.text == "\n");
        // A file of nothing but a comment keeps it.
        Unchanged("/* nothing else here */\n");
    }

    std::printf("maxima_format tests passed\n");
    return 0;
}
