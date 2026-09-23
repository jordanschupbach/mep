// The part of mep's Maxima vocabulary that carries prose: the keywords,
// the operators, the constants, and the functions common enough that an
// explanation in an editor popup earns its place.
//
// Hand-written, unlike maxima_lsp_builtin_names.cpp next to it, which is
// generated from a real Maxima installation and carries names, kinds and
// argument lists only. The split is deliberate: the generated half has
// to stay mechanically true to whatever Maxima is installed, and the
// explanations here have to say the thing a person editing a `.mac` file
// actually needs -- which for this language is usually "this is not the
// operator you think it is".

#include <algorithm>
#include <string>
#include <vector>

#include "maxima_lsp.h"

namespace {

const std::vector<MaximaLspVocabEntry> kKeywords = {
    {"if", "if cond then a else b",
     "A conditional expression, not a statement: it has a value, and with no `else` that value is `false`."},
    {"then", "if cond then ...", "Introduces the value an `if` takes when its condition holds."},
    {"else", "if cond then a else b", "Introduces the value an `if` takes when its condition does not hold."},
    {"elseif", "if a then x elseif b then y",
     "Continues an `if` with another condition, without nesting a second `if` inside the `else`."},
    {"do", "for i: 1 thru n do body",
     "Closes a loop header and introduces its body. Every Maxima loop ends with `do`, whatever clauses came before."},
    {"for", "for i: 1 thru 10 do ...",
     "Names a loop's control variable. The variable is local to the loop, whatever it meant outside."},
    {"from", "for i from 1 thru 10 do ...",
     "The value a counting loop starts at. Written `for i: 1` just as often -- inside a loop header, `:` means "
     "`from`."},
    {"step", "for i: 1 thru 10 step 2 do ...",
     "How much a counting loop adds each time round. Cannot be combined with `next`."},
    {"next", "for i: 1 next 2*i thru 100 do ...",
     "The expression that produces the loop variable's next value. Cannot be combined with `step`."},
    {"thru", "for i: 1 thru n do ...", "The last value a counting loop runs to, inclusive."},
    {"in", "for x in [a, b, c] do ...",
     "Iterates over the elements of a list rather than counting. Only a loop word here -- everywhere else `in` is "
     "an ordinary name."},
    {"while", "while cond do ...", "Repeats as long as its condition holds; it is tested before each pass."},
    {"unless", "unless cond do ...", "Repeats until its condition holds -- `while not cond`, written the other way."},
    {"and", "a and b", "Logical conjunction. Evaluates left to right and stops at the first `false`."},
    {"or", "a or b", "Logical disjunction. Evaluates left to right and stops at the first `true`."},
    {"not", "not a", "Logical negation. Binds tighter than `and` and `or`, looser than any comparison."},
};

const std::vector<MaximaLspVocabEntry> kOperators = {
    {":", "x : expr",
     "Assignment. This is the one that stores a value -- `=` builds an equation and stores nothing."},
    {":=", "f(x) := expr",
     "Function definition. The body is left unevaluated until the function is called, which is the difference "
     "between this and `define`."},
    {"::", "x :: expr", "Assigns to whatever the left side evaluates to, rather than to the name written there."},
    {"::=", "f(x) ::= expr", "Macro definition: the body produces an expression, which is then evaluated."},
    {"=", "a = b",
     "Builds an equation. It does not assign and it does not test -- `solve` and `ev` consume these, `is(a = b)` "
     "tests one, and `:` is assignment."},
    {"#", "a # b", "Not equal. Maxima spells inequality `#`; `!=` is not an operator."},
    {"<", "a < b", "Less than. A comparison is a predicate: `is(...)` or an `if` decides its truth."},
    {"<=", "a <= b", "Less than or equal."},
    {">", "a > b", "Greater than."},
    {">=", "a >= b", "Greater than or equal."},
    {"+", "a + b", "Addition, and unary plus. Unary `+`/`-` bind tighter than `*` but looser than `^`."},
    {"-", "a - b", "Subtraction, and negation. `-x^2` is `-(x^2)`, because `^` binds tighter."},
    {"*", "a * b", "Commutative multiplication."},
    {"/", "a / b", "Division. Maxima keeps the result exact: `1/3` stays a rational, it does not become 0.333."},
    {"^", "a ^ b", "Exponentiation, right-associative: `a^b^c` is `a^(b^c)`."},
    {"**", "a ** b", "Exponentiation, the FORTRAN spelling of `^`."},
    {"^^", "a ^^ b", "Non-commutative exponentiation, the `.` product repeated."},
    {".", "a . b",
     "Non-commutative multiplication -- matrix product, and anything else declared `nonscalar`. A dot between two "
     "numbers is a decimal point, so write spaces around this one."},
    {"!", "n!", "Factorial, written after its argument."},
    {"!!", "n!!", "Double factorial: the product of every other integer down from `n`."},
    {"'", "'expr",
     "Quotes an expression: it is read but not evaluated, which is how a function name is passed as a value and how "
     "`'diff` stays a noun."},
    {"''", "''expr", "The opposite of `'`: evaluates its argument an extra time, while the line is being read."},
    {"@", "s@field", "Reads a field out of a structure made with `defstruct`."},
    {",", "expr, x = 1",
     "At statement level this is `ev`: it evaluates the expression on the left with the settings on the right."},
    {"$", "expr$", "Ends a statement without displaying its value. Library files end every line this way."},
    {";", "expr;", "Ends a statement and displays its value."},
    {"&&", "name && expr", "Tags the statement that follows with a name."},
};

const std::vector<MaximaLspVocabEntry> kConstants = {
    {"%pi", "constant", "The circle constant. Stays exact until something asks for a float."},
    {"%e", "constant", "The base of the natural logarithm."},
    {"%i", "constant", "The imaginary unit; `%i^2` simplifies to -1."},
    {"%phi", "constant", "The golden ratio, (1 + sqrt(5))/2."},
    {"%gamma", "constant", "The Euler-Mascheroni constant."},
    {"%catalan", "constant", "Catalan's constant."},
    {"true", "boolean", "The true value. Comparisons produce these only once something decides them."},
    {"false", "boolean", "The false value, and what an `if` with no `else` returns."},
    {"unknown", "boolean", "What `is` answers when it can neither prove nor refute a predicate."},
    {"inf", "constant", "Real positive infinity."},
    {"minf", "constant", "Real negative infinity."},
    {"infinity", "constant", "Complex infinity: unbounded magnitude, unspecified direction."},
    {"und", "constant", "Undefined -- a limit that does not exist."},
    {"ind", "constant", "Indefinite but bounded -- a limit that oscillates, like `sin(1/x)` at zero."},
    {"%", "system variable", "The value of the previous expression."},
    {"%%", "system variable", "Inside a `block` or a compound statement, the value of the previous expression in it."},
    {"done", "constant", "What a function returns when it exists for its effect rather than its value."},
    {"all", "constant", "The blanket argument `kill`, `remvalue` and friends take."},
};

// The functions worth a sentence in an editor. Two or three hundred more
// exist and are known by name (the generated table); these are the ones a
// person actually reaches for, where knowing the *shape* of the answer
// matters as much as the argument list.
const std::vector<MaximaLspVocabEntry> kFunctions = {
    {"block", "block([locals], expr, ...)",
     "A sequence of expressions whose value is the last one, with its own local variables. `return` leaves it "
     "early; a name not in the list is the global one."},
    {"lambda", "lambda([args], expr, ...)", "An anonymous function. Its body is a `block` without the name."},
    {"local", "local(name, ...)", "Declares names local to the enclosing block, including their function definitions."},
    {"return", "return(expr)", "Leaves the enclosing `block` at once with this value."},
    {"define", "define(f(x), expr)",
     "Defines a function with its body evaluated first, unlike `:=` which keeps the body as written."},
    {"ev", "ev(expr, arg, ...)",
     "Re-evaluates an expression under extra settings: substitutions, flags like `expand`, or `numer` for a float."},
    {"subst", "subst(a, b, expr)", "Replaces every `b` in `expr` with `a`, syntactically and without evaluating."},
    {"at", "at(expr, [x = a])", "The value of an expression at a point, kept as a noun until it can be computed."},
    {"integrate", "integrate(expr, x) | integrate(expr, x, a, b)",
     "Indefinite or definite integral. Returns a noun `'integrate(...)` when it cannot find a closed form."},
    {"diff", "diff(expr, x, n)",
     "Derivative. With no variable, the total differential; quoted as `'diff` it stays an unevaluated derivative."},
    {"limit", "limit(expr, x, a, dir)",
     "A limit, optionally one-sided with `plus` or `minus`. Answers `und` or `ind` when there is no single value."},
    {"sum", "sum(expr, i, lo, hi)",
     "A summation. `simpsum: true` or `simplify_sum` is what turns a symbolic one into a closed form."},
    {"product", "product(expr, i, lo, hi)", "A product, the multiplicative counterpart of `sum`."},
    {"taylor", "taylor(expr, x, a, n)", "A truncated series about `a`, carrying its own order tag."},
    {"solve", "solve(expr, x)",
     "Solves an equation or a list of them exactly, returning a list of `x = ...` equations -- not the values "
     "themselves."},
    {"find_root", "find_root(expr, x, a, b)", "A numerical root inside a bracket where the expression changes sign."},
    {"factor", "factor(expr)", "Factors a polynomial or an integer."},
    {"expand", "expand(expr)", "Multiplies out products and powers."},
    {"ratsimp", "ratsimp(expr)", "Simplifies to a single ratio of expanded polynomials."},
    {"radcan", "radcan(expr)", "Canonical form for expressions with radicals, exponentials and logarithms."},
    {"trigsimp", "trigsimp(expr)", "Simplifies using the Pythagorean identities."},
    {"trigreduce", "trigreduce(expr)", "Rewrites products and powers of trig functions as sums of multiple angles."},
    {"trigexpand", "trigexpand(expr)", "Expands trig functions of sums and multiples into single angles."},
    {"rectform", "rectform(expr)", "Splits a complex expression into real and imaginary parts."},
    {"float", "float(expr)", "Converts to ordinary floating point."},
    {"bfloat", "bfloat(expr)", "Converts to a bigfloat, with `fpprec` digits of precision."},
    {"is", "is(pred)",
     "Decides a predicate, using whatever `assume` has been told. Returns `unknown` rather than guessing."},
    {"assume", "assume(pred, ...)", "Records a fact about a symbol for later predicates to use."},
    {"declare", "declare(name, property)",
     "Gives a symbol a property -- `integer`, `constant`, `nonscalar`, `even` -- that simplification then honours."},
    {"matrix", "matrix(row, ...)", "Builds a matrix from lists of rows. The product operator for matrices is `.`."},
    {"determinant", "determinant(m)", "The determinant of a square matrix."},
    {"invert", "invert(m)", "The inverse of a square matrix."},
    {"transpose", "transpose(m)", "The transpose of a matrix, or of a list read as a column."},
    {"makelist", "makelist(expr, i, lo, hi)", "Builds a list by evaluating an expression for each value of a variable."},
    {"create_list", "create_list(expr, i, list, ...)", "Builds a list over several variables at once."},
    {"map", "map(f, expr, ...)", "Applies a function to the top-level parts of an expression."},
    {"apply", "apply(f, [args])", "Calls a function with a list as its argument list."},
    {"lambda_apply", "apply(lambda([x], ...), [args])", "The usual way to call an anonymous function on a list."},
    {"length", "length(expr)", "The number of top-level parts: elements of a list, rows of a matrix, terms of a sum."},
    {"first", "first(expr)", "The first part. `second` through `tenth` and `last` are there too."},
    {"rest", "rest(expr, n)", "Everything but the first part, or but the first `n`."},
    {"cons", "cons(a, list)", "A list with one more element on the front."},
    {"append", "append(list, ...)", "Joins lists end to end."},
    {"sort", "sort(list, pred)", "Sorts a list, by an ordering predicate when one is given."},
    {"reverse", "reverse(list)", "Reverses a list."},
    {"member", "member(a, list)", "Whether a list has that element at its top level."},
    {"sublist", "sublist(list, pred)", "The elements a predicate accepts."},
    {"delete", "delete(a, expr, n)", "Removes occurrences of a term, at most `n` of them when a count is given."},
    {"listp", "listp(expr)", "Whether an expression is a list."},
    {"atom", "atom(expr)", "Whether an expression has no parts -- a number, a string or a bare symbol."},
    {"numberp", "numberp(expr)", "Whether an expression is a literal number. `%pi` is not one."},
    {"integerp", "integerp(expr)", "Whether an expression is a literal integer."},
    {"freeof", "freeof(x, expr)", "Whether a symbol does not occur anywhere in an expression."},
    {"print", "print(expr, ...)", "Prints its arguments on one line and returns the last of them."},
    {"display", "display(expr, ...)", "Prints each argument as `name = value`."},
    {"printf", "printf(dest, template, ...)", "Formatted output, in the Common Lisp format language."},
    {"plot2d", "plot2d(expr, [x, a, b], options)",
     "Plots in two dimensions through gnuplot. `[png_file, \"f.png\"]` or `set_plot_option` sends it to a file."},
    {"plot3d", "plot3d(expr, [x, a, b], [y, c, d])", "Plots a surface through gnuplot."},
    {"draw2d", "draw2d(options, objects)",
     "The `draw` package's two-dimensional plot: a scene built out of objects and options. Needs `load(draw)`."},
    {"draw3d", "draw3d(options, objects)", "The `draw` package's three-dimensional plot. Needs `load(draw)`."},
    {"load", "load(name)",
     "Loads a package or a file: a bare name is looked for on Maxima's search path, a string is a path."},
    {"batch", "batch(path)", "Reads a file statement by statement, echoing each one and its value."},
    {"batchload", "batchload(path)", "Reads a file without displaying anything, which is what `load` uses."},
    {"kill", "kill(name, ...)", "Forgets everything about a symbol. `kill(all)` resets the session."},
    {"string", "string(expr)", "The expression as Maxima would print it, as a string."},
    {"parse_string", "parse_string(s)", "Reads a string as a Maxima expression, without evaluating it."},
    {"concat", "concat(a, ...)", "Joins its arguments into one symbol -- how a name is built at run time."},
    {"error", "error(msg, ...)", "Stops with a message, unwinding to the top level."},
    {"errcatch", "errcatch(expr, ...)", "Evaluates expressions, returning `[]` instead of stopping if one errors."},
    {"catch", "catch(expr, ...)", "Evaluates expressions until one of them calls `throw`."},
    {"throw", "throw(expr)", "Leaves the innermost `catch` with this value."},
    {"infix", "infix(op, lbp, rbp)",
     "Adds an infix operator to the language for the rest of the session -- the file's own grammar, extended."},
    {"prefix", "prefix(op, rbp)", "Adds a prefix operator."},
    {"postfix", "postfix(op, lbp)", "Adds a postfix operator."},
    {"nary", "nary(op, bp)", "Adds an operator that takes any number of operands at one level."},
    {"matchfix", "matchfix(open, close)", "Adds a bracketing pair that reads its contents as an argument list."},
    {"defstruct", "defstruct(f(field, ...))", "Declares a structure type; `@` reads its fields."},
    {"tex", "tex(expr)", "Prints an expression as TeX."},
    {"grind", "grind(expr)", "Prints an expression in a form Maxima can read back."},
    {"fpprec", "fpprec", "How many digits a bigfloat carries. Assigning to it changes every later `bfloat`."},
    {"numer", "numer", "When true, `ev` turns exact constants into floats."},
    {"display2d", "display2d",
     "When true, results are drawn as two-dimensional art. Setting it `false` gives one flat line per result, which "
     "is what a pipe or an editor wants."},
    {"linel", "linel", "The column width output is wrapped at."},
    {"ratprint", "ratprint", "When false, silences the \"rat: replaced\" notes."},
    {"keepfloat", "keepfloat", "When true, rational arithmetic leaves floats alone instead of converting them."},
};

}  // namespace

const std::vector<MaximaLspVocabEntry> &MaximaLspKeywordVocab() { return kKeywords; }
const std::vector<MaximaLspVocabEntry> &MaximaLspOperatorVocab() { return kOperators; }
const std::vector<MaximaLspVocabEntry> &MaximaLspConstantVocab() { return kConstants; }
const std::vector<MaximaLspVocabEntry> &MaximaLspFunctionVocab() { return kFunctions; }

const MaximaLspVocabEntry *MaximaLspFindVocab(const std::string &name) {
    for (const std::vector<MaximaLspVocabEntry> *table : {&kKeywords, &kOperators, &kConstants, &kFunctions}) {
        for (const MaximaLspVocabEntry &e : *table) {
            if (name == e.name) return &e;
        }
    }
    return nullptr;
}
