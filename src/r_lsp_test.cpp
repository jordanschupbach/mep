// Windowless test for mep's own R language server -- the R reader
// (r_lsp_parse.cpp) and the analysis half (r_lsp.cpp) driven directly:
// no process, no JSON-RPC client, no GL context, and no R installation.
// CHECK(), never assert(): the Release build strips assert() entirely.
//
// Two conventions, both borrowed from org_lsp_test.cpp because they have
// earned it:
//   - Diagnostic assertions are written against RLspDiagnostic::code,
//     never against message wording, so messages stay free to improve.
//   - Every case that asserts a diagnostic *fires* is paired with one
//     asserting it stays quiet on code that is merely unusual. False
//     positives are what makes a linter get switched off, so they are
//     tested as deliberately as the true positives -- see FalsePositives()
//     below, which lints a whole realistic script and demands silence.
#include "r_lsp.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "r_lsp_parse.h"

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

/** @brief Never touches the filesystem, so the suite gives the same answers in any working directory. */
RLspOptions NoFiles() {
    RLspOptions opts;
    opts.check_files = false;
    return opts;
}

std::vector<RLspDiagnostic> Lint(const Lines &lines) { return RLspDiagnostics(lines, NoFiles()); }

/** @brief Counts diagnostics carrying a given code. */
size_t CountCode(const std::vector<RLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const RLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

/** @brief Reports whether any diagnostic carries a given code. */
bool HasCode(const std::vector<RLspDiagnostic> &diags, const std::string &code) { return CountCode(diags, code) > 0; }

/** @brief Returns the first diagnostic carrying a given code, aborting the test if there is none. */
const RLspDiagnostic &FindCode(const std::vector<RLspDiagnostic> &diags, const std::string &code) {
    for (const RLspDiagnostic &d : diags) {
        if (d.code == code) return d;
    }
    std::fprintf(stderr, "no diagnostic with code '%s'; got:\n", code.c_str());
    for (const RLspDiagnostic &d : diags) {
        std::fprintf(stderr, "  %d:%d %s: %s\n", d.range.line, d.range.col, d.code.c_str(), d.message.c_str());
    }
    Check(false, code.c_str(), __LINE__);
    return diags[0];  // unreachable: Check aborts
}

/** @brief Reports whether a completion list offers a label. */
bool Offers(const std::vector<RLspCompletionItem> &items, const std::string &label) {
    for (const RLspCompletionItem &item : items) {
        if (item.label == label) return true;
    }
    return false;
}

/** @brief Returns the completion item with a label, aborting the test if there is none. */
const RLspCompletionItem &Offered(const std::vector<RLspCompletionItem> &items, const std::string &label) {
    for (const RLspCompletionItem &item : items) {
        if (item.label == label) return item;
    }
    Check(false, label.c_str(), __LINE__);
    return items[0];  // unreachable
}

/** @brief Reports whether a string contains a substring. */
bool Contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- The reader -------------------------------------------------------

/** @brief Checks the lexer: literals, operators, strings, comments and the `\(x)` shorthand. */
void Tokens() {
    Lines lines;
    lines.push_back("x <- 'a b' # note");
    lines.push_back("y = r\"(raw \\ string)\"");
    lines.push_back("`odd name` <- 1L");
    std::vector<RComment> comments;
    const std::vector<RToken> toks = RLspTokenize(lines, &comments);
    CHECK(comments.size() == 1);
    CHECK(comments[0].text == "# note");
    CHECK(!comments[0].roxygen);

    size_t strings = 0;
    bool saw_backtick = false;
    bool saw_raw = false;
    for (const RToken &tok : toks) {
        if (tok.kind == RTokKind::Str) {
            strings++;
            if (tok.text == "raw \\ string") saw_raw = true;
        }
        if (tok.kind == RTokKind::Ident && tok.backticked && tok.text == "odd name") saw_backtick = true;
        CHECK(!tok.unterminated);
    }
    CHECK(strings == 2);
    CHECK(saw_raw);
    CHECK(saw_backtick);

    Lines roxygen;
    roxygen.push_back("#' @param x a value");
    std::vector<RComment> doc_comments;
    RLspTokenize(roxygen, &doc_comments);
    CHECK(doc_comments.size() == 1);
    CHECK(doc_comments[0].roxygen);
}

/** @brief Checks that operator precedence and associativity match R's own. */
void Precedence() {
    Lines lines;
    lines.push_back("a <- 1 + 2 * 3");
    lines.push_back("b <- -x^2");
    lines.push_back("d <- !a == b");
    lines.push_back("e <- 1:n - 1");
    const RParseResult ast = RLspParse(lines);
    CHECK(ast.ok());
    CHECK(ast.roots.size() == 4);

    // 1 + 2 * 3 parses as 1 + (2 * 3).
    const RNode &plus = ast.at(ast.at(ast.roots[0]).kids[1]);
    CHECK(plus.kind == RNodeKind::Binary);
    CHECK(plus.text == "+");
    CHECK(ast.at(plus.kids[1]).text == "*");

    // -x^2 is -(x^2): `^` binds tighter than unary minus.
    const RNode &neg = ast.at(ast.at(ast.roots[1]).kids[1]);
    CHECK(neg.kind == RNodeKind::Unary);
    CHECK(ast.at(neg.kids[0]).text == "^");

    // !a == b is !(a == b): `!` binds looser than comparison.
    const RNode &bang = ast.at(ast.at(ast.roots[2]).kids[1]);
    CHECK(bang.kind == RNodeKind::Unary);
    CHECK(bang.text == "!");
    CHECK(ast.at(bang.kids[0]).text == "==");

    // 1:n - 1 is (1:n) - 1: `:` binds tighter than arithmetic.
    const RNode &minus = ast.at(ast.at(ast.roots[3]).kids[1]);
    CHECK(minus.text == "-");
    CHECK(ast.at(minus.kids[0]).text == ":");

    // `**` is R's own deprecated spelling of `^`, and its parser still
    // takes it -- found against RcppArmadillo's test suite, which uses
    // it. A form feed is whitespace, which Matrix's test-tools-1.R
    // relies on.
    Lines old_spellings;
    old_spellings.push_back("x <- 2 ** 3 ** 2");
    old_spellings.push_back("\fy <- 1");
    const RParseResult old_ast = RLspParse(old_spellings);
    CHECK(old_ast.ok());
    CHECK(old_ast.roots.size() == 2);
    const RNode &power = old_ast.at(old_ast.at(old_ast.roots[0]).kids[1]);
    CHECK(power.text == "**");
    CHECK(old_ast.at(power.kids[1]).text == "**");  // right-associative, like ^
}

/** @brief Checks the constructs a line-based reader would get wrong. */
void Constructs() {
    Lines lines;
    lines.push_back("f <- function(x, y = 2, ...) {");
    lines.push_back("  z <- x[[1]]$name");
    lines.push_back("  m[, 1] <- stats::median(y,");
    lines.push_back("                          na.rm = TRUE)");
    lines.push_back("  g <- \\(v) v %in% c(1, 2)");
    lines.push_back("  y ~ x + I(x^2)");
    lines.push_back("  x |> sum()");
    lines.push_back("}");
    const RParseResult ast = RLspParse(lines);
    CHECK(ast.ok());
    CHECK(ast.roots.size() == 1);  // a call split across lines is still one statement

    const RNode &fn = ast.at(ast.at(ast.roots[0]).kids[1]);
    CHECK(fn.kind == RNodeKind::Function);
    CHECK(fn.args.size() == 3);
    CHECK(fn.args[1].name == "y");
    CHECK(fn.args[1].value >= 0);  // the default is parsed
    CHECK(fn.args[2].name == "...");

    const RNode &body = ast.at(fn.kids[0]);
    CHECK(body.kind == RNodeKind::Block);
    CHECK(body.kids.size() == 5);

    // `x[[1]]$name`: a `[[` subscript closed by two `]`, then a field.
    const RNode &dollar = ast.at(ast.at(body.kids[0]).kids[1]);
    CHECK(dollar.text == "$");
    CHECK(ast.at(dollar.kids[0]).kind == RNodeKind::Index);
    CHECK(ast.at(dollar.kids[0]).text == "[[");

    // `m[, 1] <- ...`: the empty first subscript is a real argument slot.
    const RNode &index = ast.at(ast.at(body.kids[1]).kids[0]);
    CHECK(index.kind == RNodeKind::Index);
    CHECK(index.args.size() == 2);
    CHECK(index.args[0].value < 0);

    // The named argument of the call split over two lines.
    const RNode &call = ast.at(ast.at(body.kids[1]).kids[1]);
    CHECK(call.kind == RNodeKind::Call);
    CHECK(call.args.size() == 2);
    CHECK(call.args[1].name == "na.rm");

    CHECK(ast.at(ast.at(body.kids[2]).kids[1]).kind == RNodeKind::Function);  // the \(v) lambda
    CHECK(ast.at(body.kids[3]).text == "~");
    CHECK(ast.at(body.kids[4]).text == "|>");
}

/** @brief Checks that a newline ends a statement exactly where R says it does. */
void Newlines() {
    Lines split;
    split.push_back("x <-");
    split.push_back("  1");
    CHECK(RLspParse(split).roots.size() == 1);  // a trailing operator continues the line

    Lines separate;
    separate.push_back("x");
    separate.push_back("-1");
    CHECK(RLspParse(separate).roots.size() == 2);  // a complete expression ends at the newline

    Lines inside_call;
    inside_call.push_back("f(a,");
    inside_call.push_back("  b)");
    CHECK(RLspParse(inside_call).roots.size() == 1);

    Lines block;
    block.push_back("f({");
    block.push_back("  a");
    block.push_back("  b");
    block.push_back("})");
    const RParseResult ast = RLspParse(block);
    CHECK(ast.ok());
    CHECK(ast.roots.size() == 1);
    // Inside the braces newlines separate statements again, however deep
    // in a call the braces are.
    const RNode &braced = ast.at(ast.at(ast.roots[0]).args[0].value);
    CHECK(braced.kind == RNodeKind::Block);
    CHECK(braced.kids.size() == 2);
}

/** @brief Checks that a syntax error costs one statement, not the rest of the file. */
void Recovery() {
    Lines lines;
    lines.push_back("f(1");
    lines.push_back("ok <- 2");
    lines.push_back("g <- function(y) y + 1");
    const RParseResult ast = RLspParse(lines);
    CHECK(!ast.ok());
    // The definition after the broken line is still found, which is what
    // keeps symbols and completion working while a file is half-typed.
    CHECK(RLspSymbols(lines).size() >= 1);
    bool saw_g = false;
    for (const RLspSymbol &sym : RLspSymbols(lines)) {
        if (sym.name == "g") saw_g = true;
    }
    CHECK(saw_g);

    Lines unclosed;
    unclosed.push_back("f <- function(x) {");
    unclosed.push_back("  x + 1");
    CHECK(HasCode(Lint(unclosed), "unclosed-brace"));

    Lines unterminated;
    unterminated.push_back("x <- \"abc");
    CHECK(HasCode(Lint(unterminated), "unterminated-string"));

    Lines dangling;
    dangling.push_back("if (a) 1");
    dangling.push_back("else 2");
    CHECK(HasCode(Lint(dangling), "else-at-top-level"));

    Lines nested_else;
    nested_else.push_back("f <- function(a) {");
    nested_else.push_back("  if (a) 1");
    nested_else.push_back("  else 2");
    nested_else.push_back("}");
    CHECK(!HasCode(Lint(nested_else), "else-at-top-level"));  // legal inside braces
}

// --- Diagnostics ------------------------------------------------------

/** @brief Checks the scope-based checks: unknown names, unused locals, and what they deliberately ignore. */
void ScopeDiagnostics() {
    Lines undefined;
    undefined.push_back("f <- function() {");
    undefined.push_back("  totl <- 1");
    undefined.push_back("  total + 1");
    undefined.push_back("}");
    const std::vector<RLspDiagnostic> diags = Lint(undefined);
    CHECK(HasCode(diags, "undefined-variable"));
    CHECK(FindCode(diags, "undefined-variable").range.line == 2);
    CHECK(HasCode(diags, "unused-variable"));

    // A function may call one defined further down the file, and a
    // parameter is never "undefined".
    Lines forward;
    forward.push_back("a <- function(x) b(x)");
    forward.push_back("b <- function(y) y + 1");
    CHECK(Lint(forward).empty());

    // A top-level value is a script's output, not an unused variable.
    Lines top_level;
    top_level.push_back("result <- 42");
    CHECK(!HasCode(Lint(top_level), "unused-variable"));

    // Anything that attaches an invisible environment switches the
    // unknown-name check off rather than guessing.
    Lines library_call;
    library_call.push_back("library(ggplot2)");
    library_call.push_back("p <- ggplot(df, aes(x = a)) + geom_point()");
    CHECK(!HasCode(Lint(library_call), "undefined-variable"));
    CHECK(!HasCode(Lint(library_call), "undefined-function"));

    // ... but a base package does not, because its exports are known.
    Lines base_library;
    base_library.push_back("library(stats)");
    base_library.push_back("x <- nosuchfunction(1)");
    CHECK(HasCode(Lint(base_library), "undefined-function"));

    // A handful of unplaceable names are typos; a dozen of them mean
    // the file runs somewhere this server cannot see (a shiny ui.R, a
    // tinytest file, a sourced script), and then it says nothing at all.
    Lines many_unknown;
    many_unknown.push_back("shinyUI(fluidPage(");
    many_unknown.push_back("  titlePanel(\"x\"),");
    many_unknown.push_back("  sidebarLayout(sidebarPanel(sliderInput(\"n\", \"n\", 1, 10, 5)),");
    many_unknown.push_back("                mainPanel(plotOutput(\"plot\")))))");
    CHECK(!HasCode(Lint(many_unknown), "undefined-function"));

    // `Rcpp::sourceCpp()` defines R functions from C++, and counts as
    // attaching something invisible just as `source()` does.
    Lines source_cpp;
    source_cpp.push_back("Rcpp::sourceCpp(\"thing.cpp\")");
    source_cpp.push_back("y <- thing_from_cpp(1)");
    CHECK(!HasCode(Lint(source_cpp), "undefined-function"));

    // A name the curated table has never heard of is still a real name.
    Lines generated_names;
    generated_names.push_back("d <- diff(c(1, 2, 3))");
    generated_names.push_back("r <- rle(d)");
    generated_names.push_back("plot(iris)");
    CHECK(Lint(generated_names).empty());

    // `assign()` with a literal name binds it.
    Lines assigned;
    assigned.push_back("assign(\"later\", 1)");
    assigned.push_back("later + 1");
    CHECK(!HasCode(Lint(assigned), "undefined-variable"));

    // A `<<-` to a name nothing else binds creates a global.
    Lines superassign;
    superassign.push_back("f <- function() counter <<- 1");
    superassign.push_back("g <- function() counter + 1");
    CHECK(!HasCode(Lint(superassign), "undefined-variable"));
}

/** @brief Checks that quoted arguments and formulas are never resolved as variables. */
void NonStandardEvaluation() {
    Lines formula;
    formula.push_back("d <- data.frame(y = 1, x = 2)");
    formula.push_back("m <- lm(y ~ x + z, data = d)");
    CHECK(!HasCode(Lint(formula), "undefined-variable"));

    Lines quoted;
    quoted.push_back("e <- quote(anything_at_all)");
    quoted.push_back("s <- substitute(whatever)");
    CHECK(!HasCode(Lint(quoted), "undefined-variable"));

    Lines with_data;
    with_data.push_back("d <- data.frame(a = 1)");
    with_data.push_back("with(d, a + b)");
    CHECK(!HasCode(Lint(with_data), "undefined-variable"));

    // A variable used only inside a formula still counts as used.
    Lines used_in_formula;
    used_in_formula.push_back("f <- function() {");
    used_in_formula.push_back("  spec <- y ~ x");
    used_in_formula.push_back("  lm(spec)");
    used_in_formula.push_back("}");
    CHECK(!HasCode(Lint(used_in_formula), "unused-variable"));
}

/** @brief Checks the correctness traps: NA/NULL comparison, vector logic, 1:length(), T/F. */
void CorrectnessDiagnostics() {
    Lines na_compare;
    na_compare.push_back("if (x == NA) 1");
    CHECK(HasCode(Lint(na_compare), "na-comparison"));

    Lines null_compare;
    null_compare.push_back("if (x == NULL) 1");
    CHECK(HasCode(Lint(null_compare), "null-comparison"));

    Lines is_na;
    is_na.push_back("x <- c(1, NA)");
    is_na.push_back("if (any(is.na(x))) 1");
    CHECK(Lint(is_na).empty());

    Lines vector_logic;
    vector_logic.push_back("if (a & b) 1");
    CHECK(HasCode(Lint(vector_logic), "vector-logic-in-condition"));

    Lines scalar_logic;
    scalar_logic.push_back("if (a && b) 1");
    CHECK(!HasCode(Lint(scalar_logic), "vector-logic-in-condition"));

    // Elementwise logic outside a condition is ordinary R.
    Lines elementwise;
    elementwise.push_back("keep <- a & b");
    CHECK(!HasCode(Lint(elementwise), "vector-logic-in-condition"));

    Lines seq_length;
    seq_length.push_back("for (i in 1:length(xs)) print(i)");
    CHECK(HasCode(Lint(seq_length), "seq-length"));
    CHECK(Contains(FindCode(Lint(seq_length), "seq-length").message, "seq_along"));

    Lines seq_along;
    seq_along.push_back("for (i in seq_along(xs)) print(i)");
    CHECK(!HasCode(Lint(seq_along), "seq-length"));

    Lines nrow_seq;
    nrow_seq.push_back("for (i in 1:nrow(d)) print(i)");
    CHECK(HasCode(Lint(nrow_seq), "seq-length"));

    // A range that does not start at 1 is just a range.
    Lines plain_range;
    plain_range.push_back("for (i in 2:length(xs)) print(i)");
    CHECK(!HasCode(Lint(plain_range), "seq-length"));

    Lines abbreviations;
    abbreviations.push_back("flag <- T");
    CHECK(HasCode(Lint(abbreviations), "true-false-abbrev"));

    Lines spelled_out;
    spelled_out.push_back("flag <- TRUE");
    CHECK(!HasCode(Lint(spelled_out), "true-false-abbrev"));

    Lines condition_assignment;
    condition_assignment.push_back("if (x = 1) 2");
    CHECK(HasCode(Lint(condition_assignment), "assignment-in-condition"));
    // ... and not also the style hint about `=`, which would be noise.
    CHECK(!HasCode(Lint(condition_assignment), "equals-assignment"));

    Lines control_flow;
    control_flow.push_back("break");
    CHECK(HasCode(Lint(control_flow), "break-outside-loop"));

    Lines in_loop;
    in_loop.push_back("for (i in 1:3) if (i > 2) break");
    CHECK(!HasCode(Lint(in_loop), "break-outside-loop"));

    Lines unreachable;
    unreachable.push_back("f <- function() {");
    unreachable.push_back("  return(1)");
    unreachable.push_back("  print(\"never\")");
    unreachable.push_back("}");
    CHECK(HasCode(Lint(unreachable), "unreachable-code"));

    Lines dots;
    dots.push_back("f <- function(x) g(...)");
    CHECK(HasCode(Lint(dots), "dots-outside-function"));

    Lines forwarded;
    forwarded.push_back("f <- function(x, ...) g(...)");
    CHECK(!HasCode(Lint(forwarded), "dots-outside-function"));
}

/** @brief Checks argument matching against the signatures the server knows. */
void ArgumentDiagnostics() {
    Lines unknown;
    unknown.push_back("n <- nchar(\"hi\", tpe = \"bytes\")");
    const std::vector<RLspDiagnostic> diags = Lint(unknown);
    CHECK(HasCode(diags, "unknown-argument"));
    CHECK(Contains(FindCode(diags, "unknown-argument").message, "type"));  // the suggestion

    Lines known;
    known.push_back("n <- nchar(\"hi\", type = \"bytes\")");
    CHECK(!HasCode(Lint(known), "unknown-argument"));

    // Partial matching is legal R, so a shortened name is not an error.
    Lines partial;
    partial.push_back("n <- nchar(\"hi\", ty = \"bytes\")");
    CHECK(!HasCode(Lint(partial), "unknown-argument"));

    // A function whose formals end in `...` accepts anything.
    Lines dots_signature;
    dots_signature.push_back("m <- mean(x, anything = TRUE)");
    CHECK(!HasCode(Lint(dots_signature), "unknown-argument"));

    Lines duplicate;
    duplicate.push_back("m <- mean(x, na.rm = TRUE, na.rm = FALSE)");
    CHECK(HasCode(Lint(duplicate), "duplicate-argument"));

    Lines too_many;
    too_many.push_back("s <- seq_len(1, 2, 3)");
    CHECK(HasCode(Lint(too_many), "too-many-arguments"));

    Lines duplicate_formal;
    duplicate_formal.push_back("f <- function(x, y, x) x");
    CHECK(HasCode(Lint(duplicate_formal), "duplicate-parameter"));

    // An empty named slot is idiomatic in switch(); an empty positional
    // one is a stray comma.
    Lines switch_call;
    switch_call.push_back("switch(x, a = , b = 1)");
    CHECK(!HasCode(Lint(switch_call), "empty-argument"));

    // A trailing comma is a comma someone forgot to delete...
    Lines stray_comma;
    stray_comma.push_back("f(a, b, )");
    CHECK(HasCode(Lint(stray_comma), "empty-argument"));

    // ... while an empty slot in the middle is how R skips an argument.
    Lines skipped_argument;
    skipped_argument.push_back("cv.tree(fit, , prune.misclass)");
    CHECK(!HasCode(Lint(skipped_argument), "empty-argument"));

    // A subscript's empty slot means "every row", and is never a stray.
    Lines subscript;
    subscript.push_back("m[, 1]");
    CHECK(!HasCode(Lint(subscript), "empty-argument"));
}

/** @brief Checks the style and hygiene checks. */
void StyleDiagnostics() {
    // A `=` among arrows is a slip and is reported...
    Lines equals;
    equals.push_back("a <- 1");
    equals.push_back("b <- 2");
    equals.push_back("x = 3");
    CHECK(HasCode(Lint(equals), "equals-assignment"));

    // ... but a file that uses `=` throughout was written that way on
    // purpose, and hinting at every line of it is how a linter gets
    // switched off.
    Lines all_equals;
    all_equals.push_back("a = 1");
    all_equals.push_back("b = 2");
    all_equals.push_back("x = 3");
    CHECK(!HasCode(Lint(all_equals), "equals-assignment"));

    Lines arrow;
    arrow.push_back("x <- 1");
    CHECK(!HasCode(Lint(arrow), "equals-assignment"));

    // A named argument is not an assignment.
    Lines named_arg;
    named_arg.push_back("m <- mean(x, na.rm = TRUE)");
    CHECK(!HasCode(Lint(named_arg), "equals-assignment"));

    Lines semicolon;
    semicolon.push_back("x <- 1;");
    semicolon.push_back("y <- 2");
    semicolon.push_back("z <- 3");
    semicolon.push_back("w <- 4");
    semicolon.push_back("v <- 5");
    CHECK(HasCode(Lint(semicolon), "trailing-semicolon"));

    // Every line ending in `;` is a deliberate habit, not a slip.
    Lines all_semicolons;
    all_semicolons.push_back("x <- 1;");
    all_semicolons.push_back("y <- 2;");
    all_semicolons.push_back("z <- 3;");
    CHECK(!HasCode(Lint(all_semicolons), "trailing-semicolon"));

    // A `;` that really does separate two statements is left alone.
    Lines separator;
    separator.push_back("x <- 1; y <- 2");
    CHECK(!HasCode(Lint(separator), "trailing-semicolon"));

    Lines shadow;
    shadow.push_back("c <- 5");
    CHECK(HasCode(Lint(shadow), "shadow-base"));

    // `df` is a base function too, and is also what every second script
    // calls its data frame. Reporting it was pure noise.
    Lines data_frame;
    data_frame.push_back("df <- data.frame(x = 1)");
    CHECK(!HasCode(Lint(data_frame), "shadow-base"));

    Lines library_inside;
    library_inside.push_back("f <- function() {");
    library_inside.push_back("  library(stats)");
    library_inside.push_back("  1");
    library_inside.push_back("}");
    CHECK(HasCode(Lint(library_inside), "library-in-function"));

    Lines library_top;
    library_top.push_back("library(stats)");
    CHECK(!HasCode(Lint(library_top), "library-in-function"));

    Lines attach;
    attach.push_back("attach(mtcars)");
    CHECK(HasCode(Lint(attach), "attach-call"));

    Lines wrong_package;
    wrong_package.push_back("x <- base::median(1)");
    CHECK(HasCode(Lint(wrong_package), "wrong-package"));

    Lines right_package;
    right_package.push_back("x <- stats::median(1)");
    CHECK(!HasCode(Lint(right_package), "wrong-package"));
}

/** @brief Checks the roxygen2 consistency checks. */
void RoxygenDiagnostics() {
    Lines mismatch;
    mismatch.push_back("#' Add numbers");
    mismatch.push_back("#' @param x first");
    mismatch.push_back("#' @param z second");
    mismatch.push_back("add <- function(x, y) x + y");
    CHECK(HasCode(Lint(mismatch), "roxygen-unknown-param"));
    CHECK(Contains(FindCode(Lint(mismatch), "roxygen-unknown-param").message, "z"));

    Lines correct;
    correct.push_back("#' Add numbers");
    correct.push_back("#' @param x first");
    correct.push_back("#' @param y second");
    correct.push_back("add <- function(x, y) x + y");
    CHECK(Lint(correct).empty());

    // One line may document several arguments.
    Lines shared;
    shared.push_back("#' @param x,y the two numbers");
    shared.push_back("add <- function(x, y) x + y");
    CHECK(!HasCode(Lint(shared), "roxygen-unknown-param"));

    Lines duplicated;
    duplicated.push_back("#' @param x first");
    duplicated.push_back("#' @param x again");
    duplicated.push_back("add <- function(x) x");
    CHECK(HasCode(Lint(duplicated), "roxygen-duplicate-param"));

    Lines unknown_tag;
    unknown_tag.push_back("#' @parm x first");
    unknown_tag.push_back("add <- function(x) x");
    CHECK(HasCode(Lint(unknown_tag), "roxygen-unknown-tag"));

    // A roxygen block that documents something other than a function is
    // not checked against formals it does not have.
    Lines data_doc;
    data_doc.push_back("#' @param x first");
    data_doc.push_back("values <- c(1, 2)");
    CHECK(!HasCode(Lint(data_doc), "roxygen-unknown-param"));
}

/** @brief Lints a whole realistic script and demands complete silence. */
void FalsePositives() {
    Lines lines;
    lines.push_back("# Data preparation ----");
    lines.push_back("");
    lines.push_back("load_data <- function(path, n_max = Inf) {");
    lines.push_back("  stopifnot(file.exists(path))");
    lines.push_back("  raw <- read.csv(path, stringsAsFactors = FALSE)");
    lines.push_back("  if (is.finite(n_max)) raw <- head(raw, n_max)");
    lines.push_back("  raw$total <- raw$price * raw$qty");
    lines.push_back("  raw[order(raw$total, decreasing = TRUE), ]");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("normalize <- function(x, na.rm = TRUE) {");
    lines.push_back("  rng <- range(x, na.rm = na.rm)");
    lines.push_back("  if (diff(rng) == 0) return(rep(0, length(x)))");
    lines.push_back("  (x - rng[1]) / diff(rng)");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("summarise_groups <- function(d, by) {");
    lines.push_back("  out <- lapply(split(d, d[[by]]), function(g) {");
    lines.push_back("    data.frame(group = g[[by]][1], total = sum(g$total, na.rm = TRUE), n = nrow(g))");
    lines.push_back("  })");
    lines.push_back("  do.call(rbind, out)");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("print.myfit <- function(x, ...) {");
    lines.push_back("  cat(\"R2:\", format(x$r2, digits = 3), \"\\n\")");
    lines.push_back("  invisible(x)");
    lines.push_back("}");
    lines.push_back("");
    lines.push_back("main <- function() {");
    lines.push_back("  d <- load_data(\"sales.csv\")");
    lines.push_back("  d$score <- normalize(d$total)");
    lines.push_back("  res <- summarise_groups(d, \"region\")");
    lines.push_back("  for (i in seq_len(nrow(res))) {");
    lines.push_back("    message(sprintf(\"%s: %.2f\", res$group[i], res$total[i]))");
    lines.push_back("  }");
    lines.push_back("  invisible(res)");
    lines.push_back("}");
    const std::vector<RLspDiagnostic> diags = Lint(lines);
    for (const RLspDiagnostic &d : diags) {
        std::fprintf(stderr, "unexpected diagnostic %d:%d %s: %s\n", d.range.line, d.range.col, d.code.c_str(),
                     d.message.c_str());
    }
    CHECK(diags.empty());
}

// --- Completion -------------------------------------------------------

/** @brief Checks that completion reads its context: locals, arguments, packages, fields, namespaces, roxygen. */
void Completion() {
    Lines lines;
    lines.push_back("library(stat)");
    lines.push_back("normalize <- function(v) v");
    lines.push_back("res <- norm");
    lines.push_back("n <- nchar(\"x\", t)");
    lines.push_back("m <- stats::med");
    lines.push_back("d <- list(alpha = 1, beta = 2)");
    lines.push_back("d$al");
    lines.push_back("#' @par");

    // Inside library(): package names, and nothing else.
    const std::vector<RLspCompletionItem> packages = RLspCompletions(lines, 0, 12, NoFiles());
    CHECK(Offers(packages, "stats"));
    CHECK(!Offers(packages, "sort"));

    // A name defined in this file, offered with its own signature.
    const std::vector<RLspCompletionItem> locals = RLspCompletions(lines, 2, 11, NoFiles());
    CHECK(Offers(locals, "normalize"));
    CHECK(Offered(locals, "normalize").detail == "normalize(v)");
    CHECK(Offered(locals, "normalize").replace_start == 7);

    // An argument name of the function being called, inserted with its `=`.
    const std::vector<RLspCompletionItem> args = RLspCompletions(lines, 3, 17, NoFiles());
    CHECK(Offers(args, "type"));
    CHECK(Offered(args, "type").insert_text == "type = ");

    // `pkg::` offers that package's exports only.
    const std::vector<RLspCompletionItem> exports = RLspCompletions(lines, 4, 15, NoFiles());
    CHECK(Offers(exports, "median"));
    CHECK(!Offers(exports, "mean"));  // base, not stats

    // `$` offers the fields the document shows the object having, and
    // never the half-typed name under the cursor.
    const std::vector<RLspCompletionItem> fields = RLspCompletions(lines, 6, 4, NoFiles());
    CHECK(Offers(fields, "alpha"));
    CHECK(!Offers(fields, "al"));

    // A roxygen tag after `@`, with the `@` left in place.
    const std::vector<RLspCompletionItem> tags = RLspCompletions(lines, 7, 8, NoFiles());
    CHECK(Offers(tags, "param"));
    CHECK(Offered(tags, "param").insert_text == "param");

    // Inside an ordinary comment there is nothing to complete.
    Lines comment;
    comment.push_back("# mea");
    CHECK(RLspCompletions(comment, 0, 5, NoFiles()).empty());

    // Nor inside a string.
    Lines str;
    str.push_back("x <- \"mea\"");
    CHECK(RLspCompletions(str, 0, 9, NoFiles()).empty());

    // The base vocabulary is offered shortest-first, so typing "su" does
    // not bury sub() under suppressPackageStartupMessages().
    Lines prefix;
    prefix.push_back("su");
    const std::vector<RLspCompletionItem> base = RLspCompletions(prefix, 0, 2, NoFiles());
    CHECK(Offers(base, "sub"));
    CHECK(Offers(base, "sum"));
    CHECK(base[0].label.size() <= base.back().label.size());
}

// --- Hover, signature, definition, references -------------------------

/** @brief Checks hover on each kind of thing a cursor can sit on. */
void Hover() {
    Lines lines;
    lines.push_back("#' Scale a vector to [0, 1]");
    lines.push_back("normalize <- function(x, na.rm = TRUE) {");
    lines.push_back("  rng <- range(x, na.rm = na.rm)");
    lines.push_back("  (x - rng[1]) / diff(rng)");
    lines.push_back("}");
    lines.push_back("total <- mean(c(1, 2))");
    lines.push_back("ok <- TRUE %in% c(TRUE)");

    const RLspHoverInfo local_fn = RLspHover(lines, 1, 3);
    CHECK(local_fn.found);
    CHECK(Contains(local_fn.text, "normalize(x, na.rm = TRUE)"));
    CHECK(Contains(local_fn.text, "line 2"));
    CHECK(Contains(local_fn.text, "Scale a vector"));  // the roxygen title

    const RLspHoverInfo local_var = RLspHover(lines, 3, 8);
    CHECK(local_var.found);
    CHECK(Contains(local_var.text, "rng"));
    CHECK(Contains(local_var.text, "line 3"));

    const RLspHoverInfo parameter = RLspHover(lines, 2, 15);
    CHECK(parameter.found);
    CHECK(Contains(parameter.text, "argument"));

    const RLspHoverInfo base_fn = RLspHover(lines, 5, 10);
    CHECK(base_fn.found);
    CHECK(Contains(base_fn.text, "mean(x, ...)"));
    CHECK(Contains(base_fn.text, "base"));

    const RLspHoverInfo op = RLspHover(lines, 6, 12);
    CHECK(op.found);
    CHECK(Contains(op.text, "%in%"));

    const RLspHoverInfo tag = RLspHover(lines, 0, 4);
    CHECK(!tag.found);  // roxygen prose, not a tag

    Lines tagged;
    tagged.push_back("#' @param x a value");
    const RLspHoverInfo param_tag = RLspHover(tagged, 0, 5);
    CHECK(param_tag.found);
    CHECK(Contains(param_tag.text, "formal argument"));

    Lines keyword;
    keyword.push_back("repeat break");
    const RLspHoverInfo repeat_kw = RLspHover(keyword, 0, 2);
    CHECK(repeat_kw.found);
    CHECK(Contains(repeat_kw.text, "repeat"));
}

/** @brief Checks signature help, for both a known function and one defined in the document. */
void SignatureHelp() {
    Lines lines;
    lines.push_back("n <- nchar(\"hi\", type = \"bytes\")");
    lines.push_back("scale2 <- function(x, factor = 2) x * factor");
    lines.push_back("y <- scale2(1, 3)");

    const RLspSignature first = RLspSignatureHelp(lines, 0, 12);
    CHECK(first.found);
    CHECK(Contains(first.label, "nchar(x,"));
    CHECK(first.active_param == 0);

    const RLspSignature second = RLspSignatureHelp(lines, 0, 18);
    CHECK(second.found);
    CHECK(second.active_param == 1);
    CHECK(second.params.size() == 4);

    const RLspSignature local = RLspSignatureHelp(lines, 2, 15);
    CHECK(local.found);
    CHECK(local.label == "scale2(x, factor = 2)");
    CHECK(local.active_param == 1);

    // Outside any call there is no signature to show.
    CHECK(!RLspSignatureHelp(lines, 1, 0).found);
}

/** @brief Checks go-to-definition and find-references, both driven by the same scope resolution. */
void DefinitionAndReferences() {
    Lines lines;
    lines.push_back("helper <- function(x) x + 1");
    lines.push_back("main <- function() {");
    lines.push_back("  total <- helper(1)");
    lines.push_back("  total <- total + helper(2)");
    lines.push_back("  total");
    lines.push_back("}");

    const RLspLocation def = RLspDefinition(lines, 2, 12, NoFiles());
    CHECK(def.found);
    CHECK(def.path.empty());  // this document
    CHECK(def.range.line == 0);
    CHECK(def.range.col == 0);

    // A local shadows nothing outside its own function.
    const RLspLocation local = RLspDefinition(lines, 4, 3, NoFiles());
    CHECK(local.found);
    CHECK(local.range.line == 2);

    const std::vector<RLspReference> refs = RLspReferences(lines, 4, 3);
    CHECK(refs.size() == 4);  // two writes and two reads
    size_t writes = 0;
    size_t definitions = 0;
    for (const RLspReference &ref : refs) {
        if (ref.is_write) writes++;
        if (ref.is_definition) definitions++;
    }
    CHECK(writes == 2);
    CHECK(definitions == 1);

    // A parameter's declaration counts as its first reference.
    const std::vector<RLspReference> param_refs = RLspReferences(lines, 0, 22);
    CHECK(param_refs.size() == 2);
    CHECK(param_refs[0].is_definition);

    // Nothing to resolve where the cursor is not on a name.
    CHECK(RLspReferences(lines, 5, 0).empty());
    CHECK(!RLspDefinition(lines, 5, 0, NoFiles()).found);
}

// --- Symbols and folding ----------------------------------------------

/** @brief Checks the outline: sections, definitions, nesting and S4 declarations. */
void Symbols() {
    Lines lines;
    lines.push_back("# Loading ----");
    lines.push_back("read_all <- function(dir) {");
    lines.push_back("  one <- function(f) readLines(f)");
    lines.push_back("  lapply(dir, one)");
    lines.push_back("}");
    lines.push_back("## Constants ----");
    lines.push_back("LIMIT <- 10");
    lines.push_back("# Classes ----");
    lines.push_back("setClass(\"Point\", representation(x = \"numeric\"))");
    lines.push_back("setGeneric(\"area\", function(shape) standardGeneric(\"area\"))");
    lines.push_back("setMethod(\"area\", \"Point\", function(shape) 0)");
    const std::vector<RLspSymbol> syms = RLspSymbols(lines);

    CHECK(syms.size() == 9);
    CHECK(syms[0].name == "Loading");
    CHECK(syms[0].kind == RLspSymbolKind::Namespace);
    CHECK(syms[0].range.end_line == 6);  // ends where the next level-1 section starts

    CHECK(syms[1].name == "read_all");
    CHECK(syms[1].kind == RLspSymbolKind::Function);
    CHECK(syms[1].detail == "(dir)");
    CHECK(syms[1].parent == 0);

    CHECK(syms[2].name == "one");  // nested inside read_all, not under the section
    CHECK(syms[2].parent == 1);

    CHECK(syms[3].name == "Constants");
    CHECK(syms[3].parent == 0);  // a `##` section nests inside the `#` one
    CHECK(syms[4].name == "LIMIT");
    CHECK(syms[4].kind == RLspSymbolKind::Variable);
    CHECK(syms[4].parent == 3);

    CHECK(syms[5].name == "Classes");
    CHECK(syms[5].parent == -1);
    CHECK(syms[6].name == "Point");
    CHECK(syms[6].kind == RLspSymbolKind::Class);
    CHECK(syms[7].name == "area");
    CHECK(syms[7].kind == RLspSymbolKind::Function);
    CHECK(syms[8].name == "area,Point");
    CHECK(syms[8].kind == RLspSymbolKind::Method);

    // A comment that is not a header is not a symbol.
    Lines plain;
    plain.push_back("# just a remark");
    plain.push_back("x <- 1");
    CHECK(RLspSymbols(plain).size() == 1);
}

/** @brief Checks folding of bodies, comment runs and section regions. */
void Folding() {
    Lines lines;
    lines.push_back("# Section ----");
    lines.push_back("# a comment");
    lines.push_back("# continued");
    lines.push_back("f <- function(x) {");
    lines.push_back("  if (x) {");
    lines.push_back("    1");
    lines.push_back("  }");
    lines.push_back("}");
    lines.push_back("g <- 1");
    const std::vector<RLspFold> folds = RLspFolds(lines);

    bool section = false;
    bool comment_run = false;
    bool body = false;
    bool inner = false;
    for (const RLspFold &fold : folds) {
        if (fold.start_line == 0 && fold.end_line == 8 && fold.kind.empty()) section = true;
        if (fold.start_line == 1 && fold.end_line == 2 && fold.kind == "comment") comment_run = true;
        if (fold.start_line == 3 && fold.end_line == 6) body = true;
        if (fold.start_line == 4 && fold.end_line == 5) inner = true;
    }
    CHECK(section);
    CHECK(comment_run);
    CHECK(body);
    CHECK(inner);

    // Nothing one line long is foldable.
    Lines flat;
    flat.push_back("f <- function(x) x");
    CHECK(RLspFolds(flat).empty());
}

// --- Vocabulary -------------------------------------------------------

/** @brief Checks the vocabulary tables and the generated base-name index. */
void Vocabulary() {
    CHECK(RLspFindFunction("mean") != nullptr);
    CHECK(std::string(RLspFindFunction("mean")->package) == "base");
    CHECK(RLspFindFunction("median") != nullptr);
    CHECK(std::string(RLspFindFunction("median")->package) == "stats");
    CHECK(RLspFindFunction("definitely_not_a_function") == nullptr);

    // Every curated entry is reachable by name, and its formals parse.
    for (const RLspFunctionEntry &entry : RLspFunctionVocab()) {
        CHECK(RLspFindFunction(entry.name) != nullptr);
        CHECK(std::string(entry.doc).size() > 0);
        const std::vector<std::string> params = RLspSplitParams(entry.params);
        for (const std::string &param : params) CHECK(!param.empty());
        // Anything the curated table knows, the generated list knows too.
        CHECK(RLspIsBaseName(entry.name));
    }

    // The generated list is binary-searched, so an out-of-order entry
    // would silently lose names: sample across the alphabet.
    CHECK(RLspIsBaseName("abs"));
    CHECK(RLspIsBaseName("diff"));
    CHECK(RLspIsBaseName("Recall"));
    CHECK(RLspIsBaseName("iris"));
    CHECK(RLspIsBaseName("nchar"));
    CHECK(RLspIsBaseName("zip"));
    CHECK(!RLspIsBaseName("dplyr_verb"));
    CHECK(!RLspIsBaseName(""));
    const std::vector<std::string> &names = RLspBaseNameList();
    CHECK(names.size() > 1000);
    for (const std::string &name : names) CHECK(RLspIsBaseName(name));

    CHECK(RLspIsBasePackage("stats"));
    CHECK(!RLspIsBasePackage("ggplot2"));

    // Splitting formals respects nesting and quoting.
    const std::vector<std::string> nested = RLspSplitParams("x, y = c(1, 2), sep = \", \", ...");
    CHECK(nested.size() == 4);
    CHECK(nested[1] == "y = c(1, 2)");
    CHECK(nested[2] == "sep = \", \"");
    CHECK(nested[3] == "...");
    CHECK(RLspSplitParams("").empty());
}

/** @brief Checks that degenerate documents answer rather than crash. */
void EdgeCases() {
    Lines empty;
    empty.push_back("");
    CHECK(Lint(empty).empty());
    CHECK(RLspSymbols(empty).empty());
    CHECK(RLspFolds(empty).empty());
    CHECK(!RLspHover(empty, 0, 0).found);
    CHECK(!RLspDefinition(empty, 0, 0, NoFiles()).found);
    CHECK(RLspReferences(empty, 0, 0).empty());
    CHECK(!RLspSignatureHelp(empty, 0, 0).found);

    // A position past the end of the document is not a crash.
    Lines one;
    one.push_back("x <- 1");
    CHECK(!RLspHover(one, 99, 99).found);
    CHECK(RLspCompletions(one, 99, 99, NoFiles()).empty() || true);  // must simply return

    // Something that is not R at all reports syntax errors and stops
    // rather than running away with them.
    Lines not_r;
    not_r.push_back("<html>");
    not_r.push_back("<body>}}}]]]");
    not_r.push_back("</body>");
    const std::vector<RLspDiagnostic> diags = Lint(not_r);
    CHECK(!diags.empty());
    CHECK(diags.size() <= 40);
}

}  // namespace

int main() {
    Tokens();
    Precedence();
    Constructs();
    Newlines();
    Recovery();
    ScopeDiagnostics();
    NonStandardEvaluation();
    CorrectnessDiagnostics();
    ArgumentDiagnostics();
    StyleDiagnostics();
    RoxygenDiagnostics();
    FalsePositives();
    Completion();
    Hover();
    SignatureHelp();
    DefinitionAndReferences();
    Symbols();
    Folding();
    Vocabulary();
    EdgeCases();
    std::printf("mep-r-lsp-test: all checks passed\n");
    return 0;
}
