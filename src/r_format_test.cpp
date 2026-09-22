// Coverage for r_format.h/.cpp: mep's in-house R formatter. Every case
// below is an input/expected-output pair checked twice -- once for the
// formatting itself and once fed back through Format() to prove the
// result is a fixed point, since a formatter that keeps changing its own
// output is a formatter `gf` cannot be run twice on.
//
// The two properties that matter most for a formatter that rewrites the
// user's buffer in place get their own sections: a file it cannot parse
// comes back byte-identical with ok == false (TestErrors), and the
// tokens of the output always match the tokens of the input
// (TestSemanticsPreserved, which leans on Format()'s own self-check).

#include "r_format.h"

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
    rfmt::Options opt;
    opt.width = width;
    rfmt::Result r = rfmt::Format(src, opt);
    if (!r.ok) {
        std::fprintf(stderr, "FORMAT FAILED (line %d): %s\nsource:\n%s\n", r.error_line,
                     r.error.c_str(), src.c_str());
        std::abort();
    }
    return r.text;
}

// Formats `src`, checks it against `want`, then re-formats the result and
// checks that nothing moved the second time.
void Same(const std::string &src, const std::string &want, int width = 80) {
    std::string got = Fmt(src, width);
    if (got != want) {
        std::fprintf(stderr, "MISMATCH\n--- input ---\n%s--- want ---\n%s--- got ---\n%s",
                     src.c_str(), want.c_str(), got.c_str());
        std::abort();
    }
    std::string again = Fmt(got, width);
    if (again != got) {
        std::fprintf(stderr, "NOT IDEMPOTENT\n--- once ---\n%s--- twice ---\n%s", got.c_str(),
                     again.c_str());
        std::abort();
    }
}

void TestSpacing() {
    // Binary operators get spaces; the tight ones (^ : :: ::: $ @) do not,
    // and neither do unary operators or the parenthesis of a call.
    Same("a+b*c", "a + b * c\n");
    Same("x^2", "x^2\n");
    Same("1:n", "1:n\n");
    Same("stats::median( x )", "stats::median(x)\n");
    Same("base:::sum(x)", "base:::sum(x)\n");
    Same("df $ col", "df$col\n");
    Same("obj @ slot", "obj@slot\n");
    Same("f( a , b )", "f(a, b)\n");
    Same("-x", "-x\n");
    Same("!ok", "!ok\n");
    Same("x<-c(1,-2)", "x <- c(1, -2)\n");
    Same("a<=b&&c>=d", "a <= b && c >= d\n");
    Same("y~x+z", "y ~ x + z\n");
    Same("f(~x)", "f(~x)\n");
    Same("x%in%y", "x %in% y\n");
    Same("x%%2", "x %% 2\n");
    // Indexing is tight, and R's missing argument is a real one: dropping
    // it would change `df[1, ]` (a row) into `df[1]` (a column).
    Same("df[ 1 , ]", "df[1, ]\n");
    Same("df[ , 2]", "df[, 2]\n");
    Same("l[[ 'k' ]]", "l[[\"k\"]]\n");
    Same("x[[i]][j]$name", "x[[i]][j]$name\n");
    // `**` is R's deprecated spelling of `^`.
    Same("2**8", "2^8\n");
    // Trailing whitespace and tabs go away; the file ends in one newline.
    Same("x <- 1   \n\n\n", "x <- 1\n");
    Same("\tx <- 1", "x <- 1\n");
}

void TestAssignment() {
    // Statement-position `=` becomes `<-` (tidyverse style), including
    // down a chain of them...
    Same("x = 1", "x <- 1\n");
    Same("x = y = 2", "x <- y <- 2\n");
    Same("f <- function() {\n  a = 1\n}", "f <- function() {\n  a <- 1\n}\n");
    // ...but a named argument's `=` is a different operator entirely and
    // is never touched, nor is `<<-` or the right assignments.
    Same("f(a = 1)", "f(a = 1)\n");
    Same("f(a = 1, b = g(c = 2))", "f(a = 1, b = g(c = 2))\n");
    Same("x <<- 1", "x <<- 1\n");
    Same("1 -> x", "1 -> x\n");
    Same("1 ->> x", "1 ->> x\n");
    // `=` inside a control-flow body is not a statement of a block, so it
    // stays as written rather than being rewritten in place.
    Same("if (a) b = 1", "if (a) b = 1\n");
}

void TestStrings() {
    // Single quotes become double quotes, un-escaping what no longer
    // needs escaping...
    Same("x <- 'hi'", "x <- \"hi\"\n");
    Same("x <- 'it\\'s'", "x <- \"it's\"\n");
    // ...unless that would mean adding escapes, in which case the
    // author's own quoting was the better one.
    Same("x <- 'say \"hi\"'", "x <- 'say \"hi\"'\n");
    // Raw strings and escapes inside double quotes are passed through.
    Same("x <- r\"(a\\b\"c)\"", "x <- r\"(a\\b\"c)\"\n");
    Same("x <- \"tab\\there\"", "x <- \"tab\\there\"\n");
    // A name may be a string, and stays quoted exactly as R needs it.
    Same("l[['a b']]", "l[[\"a b\"]]\n");
    Same("f('nm' = 1)", "f(\"nm\" = 1)\n");
    // Backtick names survive untouched.
    Same("`my var` <- 1", "`my var` <- 1\n");
}

void TestBlocksAndControlFlow() {
    Same("if(x){a}", "if (x) {\n  a\n}\n");
    Same("if(x){a}else{b}", "if (x) {\n  a\n} else {\n  b\n}\n");
    // else-if chains stay flat rather than nesting one level per branch.
    Same("if (x) {\n1\n} else if (y) {\n2\n} else {\n3\n}",
         "if (x) {\n  1\n} else if (y) {\n  2\n} else {\n  3\n}\n");
    // A brace-less body is the author's choice and is kept -- the
    // formatter never inserts braces, because that changes the code it
    // was asked only to lay out.
    Same("if (a) b else c", "if (a) b else c\n");
    Same("for(i in 1:10){print(i)}", "for (i in 1:10) {\n  print(i)\n}\n");
    Same("while(TRUE){break}", "while (TRUE) {\n  break\n}\n");
    Same("repeat{next}", "repeat {\n  next\n}\n");
    Same("while (TRUE) next", "while (TRUE) next\n");
    // An empty block stays on its line instead of becoming a lone brace.
    Same("f <- function() {}", "f <- function() {}\n");
    Same("f <- function() {\n}", "f <- function() {}\n");
    // Semicolons become line breaks.
    Same("a; b", "a\nb\n");
    Same("f <- function() {\n  a <- 1; b <- 2\n}", "f <- function() {\n  a <- 1\n  b <- 2\n}\n");
    // Nesting indents two spaces per level.
    Same("f <- function() {\nif (a) {\nwhile (b) {\ng()\n}\n}\n}",
         "f <- function() {\n  if (a) {\n    while (b) {\n      g()\n    }\n  }\n}\n");
    // R lets a block be called directly; base R's own plotmath demo does.
    Same("expression({}(x , y))", "expression({}(x, y))\n");
}

void TestFunctions() {
    Same("function(x,y=2)x+y", "function(x, y = 2) x + y\n");
    Same("\\(x)x*2", "\\(x) x * 2\n");
    Same("f<-function(...)list(...)", "f <- function(...) list(...)\n");
    // A parameter list too wide for the line breaks one per line, with
    // the `)` back at the definition's own indentation.
    Same("f <- function(alpha_value, beta_value, gamma_value, delta_value) NULL",
         "f <- function(\n  alpha_value,\n  beta_value,\n  gamma_value,\n  delta_value\n) NULL\n",
         50);
}

void TestLineBreaking() {
    // Fits: left alone. Does not fit: one argument per line.
    Same("f(a, b)", "f(a, b)\n", 20);
    Same("some_function(alpha, beta, gamma)",
         "some_function(\n  alpha,\n  beta,\n  gamma\n)\n", 20);
    // Nested calls break only as far out as they have to.
    Same("outer(inner(a, b), c)", "outer(\n  inner(a, b),\n  c\n)\n", 20);
    // An assignment lets the value break inside itself rather than
    // pushing it down a line...
    Same("result <- compute(alpha, beta)", "result <- compute(\n  alpha,\n  beta\n)\n", 20);
    // ...but a value with nowhere to break does go below the operator.
    Same("result_variable <- some_extremely_long_identifier_name",
         "result_variable <-\n  some_extremely_long_identifier_name\n", 30);
    // Operator chains break at every step of the chain, all at one
    // indentation: pipelines and ggplot `+` stacks are the point.
    Same("x |> filter(a) |> mutate(b) |> summarise(c)",
         "x |>\n  filter(a) |>\n  mutate(b) |>\n  summarise(c)\n", 30);
    Same("p <- ggplot(d) + geom_point() + theme_bw()",
         "p <- ggplot(d) +\n  geom_point() +\n  theme_bw()\n", 30);
    Same("total <- alpha + beta + gamma", "total <- alpha +\n  beta +\n  gamma\n", 20);
    // A condition that does not fit breaks inside its own parentheses.
    Same("if (alpha_value && beta_value) {\nf()\n}",
         "if (\n  alpha_value && beta_value\n) {\n  f()\n}\n", 30);
    // A trailing block argument "hugs" the call instead of exploding the
    // argument list -- the shape test_that()/lapply() are written in.
    Same("test_that('it works', {\nexpect_true(TRUE)\n})",
         "test_that(\"it works\", {\n  expect_true(TRUE)\n})\n");
    Same("lapply(xs, function(i) {\ni + 1\n})", "lapply(xs, function(i) {\n  i + 1\n})\n");
    Same("with(data, {\nx\n})", "with(data, {\n  x\n})\n");
    // The width is configurable, and the same input laid out to a
    // narrower margin breaks earlier.
    Same("f(alpha, beta, gamma)", "f(alpha, beta, gamma)\n", 40);
    Same("f(alpha, beta, gamma)", "f(\n  alpha,\n  beta,\n  gamma\n)\n", 20);
    // Nothing breakable: the line is allowed to overflow rather than
    // being mangled.
    Same("xxxxxxxxxxxxxxxxxxxxxxxx", "xxxxxxxxxxxxxxxxxxxxxxxx\n", 20);
    // Width is counted in characters, not bytes, so non-ASCII text is
    // measured the way it looks on screen.
    Same("f(\"\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\", b)", "f(\"\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\", b)\n", 14);
}

void TestComments() {
    // A comment gets its space after the hash...
    Same("#hello", "# hello\n");
    Same("####", "####\n");
    // ...but roxygen and shebang prefixes are left exactly alone, since
    // other tools read the character right after the hashes.
    Same("#' @export", "#' @export\n");
    Same("#!/usr/bin/env Rscript", "#!/usr/bin/env Rscript\n");
    // Leading, trailing and operator-trailing positions all survive, and
    // a trailing comment keeps its statement.
    Same("# lead\nx <- 1 # trail", "# lead\nx <- 1 # trail\n");
    Same("x <- a + # why\n  b", "x <- a + # why\n  b\n");
    Same("x <- # why\n  1", "x <- # why\n  1\n");
    // Inside an argument list, both before and after the argument.
    Same("f(\n  # about a\n  a,\n  b # about b\n)", "f(\n  # about a\n  a,\n  b # about b\n)\n");
    // A comment with no expression after it still belongs to the
    // construct it was written in.
    Same("f(\n  a,\n  # done\n)", "f(\n  a,\n  # done\n)\n");
    Same("f <- function() {\n  # nothing yet\n}", "f <- function() {\n  # nothing yet\n}\n");
    // A comment forces its construct to break even when it would fit.
    Same("f(a, # note\nb)", "f(\n  a, # note\n  b\n)\n");
    // Comments at the end of a file are kept.
    Same("x <- 1\n# the end", "x <- 1\n# the end\n");
    // A comment inside a block body, and one between two statements.
    Same("f <- function() {\n  # step one\n  a()\n  # step two\n  b()\n}",
         "f <- function() {\n  # step one\n  a()\n  # step two\n  b()\n}\n");
}

void TestBlankLines() {
    // One blank line between statements is kept, several collapse to one,
    // and the ones at the very top and bottom of a file or block go away.
    Same("a\n\nb", "a\n\nb\n");
    Same("a\n\n\n\nb", "a\n\nb\n");
    Same("\n\nx <- 1\n\n\n", "x <- 1\n");
    Same("f <- function() {\n\n  a\n\n\n  b\n\n}", "f <- function() {\n  a\n\n  b\n}\n");
    // A blank line between a comment and the code it introduces is the
    // author's paragraphing and is kept.
    Same("# header\n\nx <- 1", "# header\n\nx <- 1\n");
    Same("# one\n\n# two\nx <- 1", "# one\n\n# two\nx <- 1\n");
}

void TestErrors() {
    // Unparseable input comes back byte-identical, with the line to
    // report -- `gf` shows the message and leaves the buffer alone.
    const char *broken[] = {
        "f(a",
        "x <- ",
        "}",
        "if (x",
        "x |>\n  head() |>",
        "'unterminated",
        "for (i in) x",
    };
    for (const char *src : broken) {
        rfmt::Result r = rfmt::Format(src);
        CHECK(!r.ok);
        CHECK(r.text == src);
        CHECK(!r.error.empty());
    }
    // Pathological nesting is refused rather than recursed into: the
    // formatter runs inside the editor, so a file like this has to come
    // back as an error and not as a stack overflow.
    {
        std::string deep = std::string(5000, '(') + "1" + std::string(5000, ')');
        rfmt::Result r = rfmt::Format(deep);
        CHECK(!r.ok);
        CHECK(r.text == deep);
        std::string chain = "x <- a";
        for (int i = 0; i < 5000; i++) chain += " + a";
        rfmt::Result rc = rfmt::Format(chain);
        CHECK(!rc.ok);
        CHECK(rc.text == chain);
        // What real code reaches is still formatted.
        std::string fine = std::string(100, '(') + "1" + std::string(100, ')');
        CHECK(rfmt::Format(fine).ok);
    }

    rfmt::Result line_check = rfmt::Format("x <- 1\ny <- 2\nf(a\n");
    CHECK(!line_check.ok);
    CHECK(line_check.error_line >= 3);

    // An empty (or whitespace-only) file formats to an empty file rather
    // than to a stray newline.
    CHECK(Fmt("") == "");
    CHECK(Fmt("\n\n  \n") == "");
}

void TestSemanticsPreserved() {
    // Format() re-tokenizes its own output and compares it against the
    // input, so anything that reaches the caller with ok == true has the
    // same tokens it started with. These are the shapes most likely to
    // lose one: operator precedence that only parentheses record, R's
    // missing arguments, and `if`/`else` binding.
    const char *sources[] = {
        "(a + b) * c",
        "a + (b * c)",
        "-(x^2)",
        "(-x)^2",
        "df[i, , drop = FALSE]",
        "x[[1]][[2]]",
        "if (a) if (b) c else d",
        "f(function(x) if (x) 1 else 2, y)",
        "a %>% b() %>% (function(x) x)()",
        "y ~ . - x",
        "x <- if (a) 1 else 2",
        "`if`(a, b, c)",
        "quote({\n  a\n  b\n})",
        "switch(x, a = , b = 2, 3)",
        "function(x, ...) UseMethod(\"f\")",
    };
    for (const char *src : sources) {
        rfmt::Result r = rfmt::Format(src);
        CHECK(r.ok);
        // Formatting an already-formatted file is a no-op.
        rfmt::Result again = rfmt::Format(r.text);
        CHECK(again.ok);
        CHECK(again.text == r.text);
    }
}

}  // namespace

int main() {
    TestSpacing();
    TestAssignment();
    TestStrings();
    TestBlocksAndControlFlow();
    TestFunctions();
    TestLineBreaking();
    TestComments();
    TestBlankLines();
    TestErrors();
    TestSemanticsPreserved();
    std::printf("r_format_test: all checks passed\n");
    return 0;
}
