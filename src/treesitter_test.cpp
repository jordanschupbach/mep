// Coverage for treesitter.h/.cpp: the vendored highlight queries
// (treesitter_queries.h) and the span pipeline TreesitterHighlight puts them
// through. Python's query is where most of the expectations live, since it is
// the one this project maintains rather than vendors verbatim.
//
// The rule every expectation here follows is mep's own painting rule, and it
// is worth spelling out because it is what makes a query's pattern *order*
// load-bearing: TreesitterHighlight returns spans widest-first (ties broken by
// ascending query pattern index), and the renderer -- via mep.ts_apply_captures
// -> Editor::AddDecoration, whose same-namespace decorations are drawn in
// insertion order with later draws winning -- paints them in exactly that
// order. So the capture a byte ends up wearing is the *last* span in the
// returned list that covers it, which is what WinnerAt() below computes.
//
// So the expectations below are the *ordered list* of captures covering a byte,
// which is the whole truth about it and needs no knowledge of the colour table:
// the captures are listed outermost/earliest first, and the colour on screen is
// the last one of them that mep.ts_capture_hl (main.cpp) actually maps. A
// capture with no entry there -- `variable`, `property`, `operator`,
// `punctuation.bracket`, `field`, ... -- resolves to no highlight group and
// adds no decoration at all, so it paints nothing and leaves whatever is under
// it showing. That is why a list like "function variable" is not a bug: the
// broad @function span is what is seen, with an unthemed @variable on top.

#include "treesitter.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// Row/byte-column of the `nth` (0-based) occurrence of `needle` in `src`.
struct Where {
    int row = 0;
    int col = 0;
};
Where Find(const std::string &src, const std::string &needle, int nth = 0) {
    size_t at = std::string::npos;
    size_t from = 0;
    for (int i = 0; i <= nth; i++) {
        at = src.find(needle, from);
        if (at == std::string::npos) {
            std::fprintf(stderr, "test bug: occurrence %d of \"%s\" not in source\n", nth,
                         needle.c_str());
            std::abort();
        }
        from = at + 1;
    }
    Where w;
    size_t line_start = 0;
    for (size_t i = 0; i < at; i++) {
        if (src[i] == '\n') {
            w.row++;
            line_start = i + 1;
        }
    }
    w.col = static_cast<int>(at - line_start);
    return w;
}

// Every capture covering the first byte of `needle`'s `nth` occurrence, in the
// order TreesitterHighlight returns them (= the order they are painted in),
// space-separated. Empty when nothing covers it: the byte renders in the
// buffer's Normal colour.
std::string CapturesAt(const std::string &filetype, const std::string &src,
                       const std::string &needle, int nth = 0) {
    const Where w = Find(src, needle, nth);
    std::string out;
    for (const TSHighlightSpan &s : TreesitterHighlight(filetype, src)) {
        if (s.row != w.row) continue;
        if (w.col < s.col_start || w.col >= s.col_end) continue;
        if (!out.empty()) out += ' ';
        out += s.capture;
    }
    return out;
}

void ExpectCaptures(const std::string &filetype, const std::string &src, const std::string &needle,
                    const std::string &want, int nth = 0) {
    const std::string got = CapturesAt(filetype, src, needle, nth);
    if (got == want) return;
    std::fprintf(stderr,
                 "--- %s: \"%s\" (occurrence %d) ---\nwant \"%s\"\n got \"%s\"\n"
                 "--- source ---\n%s",
                 filetype.c_str(), needle.c_str(), nth, want.c_str(), got.c_str(), src.c_str());
    Check(false, "captures match", __LINE__);
}

// The structure outline as "depth:kind:name@row" entries, one per line.
std::string Outline(const std::string &filetype, const std::string &src) {
    std::string out;
    for (const TSStructureNode &n : TreesitterStructure(filetype, src)) {
        out += std::to_string(n.depth) + ":" + n.kind + ":" + n.name + "@" + std::to_string(n.row) + "\n";
    }
    return out;
}

void ExpectOutline(const std::string &filetype, const std::string &src, const std::string &want) {
    const std::string got = Outline(filetype, src);
    if (got == want) return;
    std::fprintf(stderr, "--- %s outline ---\nwant:\n%s got:\n%s", filetype.c_str(), want.c_str(), got.c_str());
    Check(false, "outline matches", __LINE__);
}

// ---------------------------------------------------------------------------
// Python: the f-string fields this query is built to separate from the string
// ---------------------------------------------------------------------------

void TestPythonFStrings() {
    // The whole point: a replacement field is code, not string. The braces get
    // their own capture, the expression inside keeps the captures it would
    // have anywhere else, and only the literal text around them is @string.
    const std::string src =
        "name = \"x\"\n"
        "total = 2\n"
        "s = f\"has {total:.2f} of {name!r} at {os.getpid()}\"\n";
    ExpectCaptures("py", src, "f\"", "string");
    ExpectCaptures("py", src, "has", "string");
    ExpectCaptures("py", src, "{", "punctuation.interpolation");
    ExpectCaptures("py", src, "}", "punctuation.interpolation");
    ExpectCaptures("py", src, "total:", "variable");   // ordinary code, not string
    ExpectCaptures("py", src, ":.2f", "string.special");
    ExpectCaptures("py", src, "name!", "variable");
    ExpectCaptures("py", src, "!r", "character.special");
    ExpectCaptures("py", src, "getpid", "variable property function.method");

    // A plain string is still covered end to end -- the quotes included, since
    // the query captures (string_start)/(string_content)/(string_end) rather
    // than the (string) node they sit in.
    const std::string plain = "s = \"abc\"\n";
    ExpectCaptures("py", plain, "\"abc\"", "string");
    ExpectCaptures("py", plain, "abc", "string");
    ExpectCaptures("py", plain, "\"", "string", 1);  // the closing quote

    // An escape inside the text, and `{{`/`}}` -- a literal brace, which is
    // text rather than a field and must not read as one.
    const std::string escapes = "s = f\"a\\tb {{not a field}} {x}\"\n";
    ExpectCaptures("py", escapes, "\\t", "string escape");
    ExpectCaptures("py", escapes, "{{", "string string.escape");
    ExpectCaptures("py", escapes, "}}", "string string.escape");
    ExpectCaptures("py", escapes, "{x}", "punctuation.interpolation");

    // PEP 701 nesting: the inner literal is a string again, its own field a
    // field again.
    const std::string nested = "s = f\"{f'{x}'}\"\n";
    ExpectCaptures("py", nested, "f'", "string");
    ExpectCaptures("py", nested, "{x}", "punctuation.interpolation");
    ExpectCaptures("py", nested, "x}", "variable");  // the inner field's expression
}

// ---------------------------------------------------------------------------
// Python: keywords, including the ones spelled as words and the soft ones
// ---------------------------------------------------------------------------

void TestPythonKeywords() {
    // The word operators are keywords, not the uncoloured @operator they used
    // to be captured as.
    const std::string src =
        "if a in b and a is not None or not b:\n"
        "    del a\n";
    ExpectCaptures("py", src, "in ", "keyword.operator");
    ExpectCaptures("py", src, "and", "keyword.operator");
    // The grammar exposes `is not` as one token *and* as `is` + `not`, so the
    // first byte is covered twice -- by the same capture either way.
    ExpectCaptures("py", src, "is not", "keyword.operator keyword.operator");
    ExpectCaptures("py", src, "or", "keyword.operator");
    ExpectCaptures("py", src, "not b", "keyword.operator");
    ExpectCaptures("py", src, "if", "keyword");
    ExpectCaptures("py", src, "del", "keyword");
    ExpectCaptures("py", src, "None", "constant.builtin");

    // `not in` is one token in this grammar, so it is one span.
    const std::string not_in = "x = a not in b\n";
    ExpectCaptures("py", not_in, "not in", "keyword.operator keyword.operator");

    // Statement keywords across the board.
    const std::string stmts =
        "import os\n"
        "from x import y\n"
        "class A:\n"
        "    async def f(self):\n"
        "        try:\n"
        "            await g()\n"
        "        except E:\n"
        "            raise\n"
        "        finally:\n"
        "            pass\n"
        "        while True:\n"
        "            break\n"
        "        with open(p) as fh:\n"
        "            yield fh\n"
        "        assert 1, lambda: 2\n"
        "        global g2\n"
        "        nonlocal n2\n"
        "        return None\n";
    for (const char *kw : {"import", "from", "class", "async", "def", "try", "await", "except",
                           "raise", "finally", "pass", "while", "break", "with", "as", "yield",
                           "assert", "lambda", "global", "nonlocal", "return"}) {
        ExpectCaptures("py", stmts, kw, "keyword");
    }
}

void TestPythonSoftKeywords() {
    // match/case/type/_ are keywords only where the grammar says they are --
    // the same names used as ordinary identifiers must stay ordinary, or the
    // colouring would lie about what the code does.
    const std::string match =
        "match value:\n"
        "    case 1:\n"
        "        pass\n"
        "    case _:\n"
        "        pass\n";
    ExpectCaptures("py", match, "match", "keyword");
    ExpectCaptures("py", match, "case", "keyword");
    ExpectCaptures("py", match, "_", "keyword");

    const std::string alias = "type Pair = tuple[int, int]\n";
    ExpectCaptures("py", alias, "type", "keyword");

    const std::string names =
        "match = 1\n"
        "case = 2\n"
        "type = 3\n"
        "_ = 4\n"
        "for _ in xs:\n"
        "    print(match, case, type)\n";
    ExpectCaptures("py", names, "match", "variable");
    ExpectCaptures("py", names, "case", "variable");
    ExpectCaptures("py", names, "type", "variable");
    ExpectCaptures("py", names, "_ =", "variable");
    ExpectCaptures("py", names, "_ in", "variable");
}

// ---------------------------------------------------------------------------
// Python: identifier conventions, calls, annotations
// ---------------------------------------------------------------------------

void TestPythonIdentifierConventions() {
    // ALL_CAPS is a constant and CamelCase a class, in every position -- the
    // two patterns cover the identical span, so this is also the regression
    // test for the pattern-index tiebreak: before it, the same name could come
    // out @constructor in one place and @constant in another within one file.
    const std::string src =
        "MAX_SIZE = 10\n"
        "class Widget:\n"
        "    limit = MAX_SIZE\n"
        "    other = MAX_SIZE\n"
        "    kind = Widget\n";
    ExpectCaptures("py", src, "MAX_SIZE", "variable constant", 0);
    ExpectCaptures("py", src, "MAX_SIZE", "variable constant", 1);
    ExpectCaptures("py", src, "MAX_SIZE", "variable constant", 2);
    ExpectCaptures("py", src, "Widget", "variable constructor", 0);
    ExpectCaptures("py", src, "Widget", "variable constructor", 1);

    // self/cls are builtins rather than locals; a plain name is neither.
    const std::string methods =
        "class A:\n"
        "    def f(self, other):\n"
        "        return self\n"
        "    @classmethod\n"
        "    def g(cls):\n"
        "        return cls\n";
    ExpectCaptures("py", methods, "self", "variable variable.builtin");
    ExpectCaptures("py", methods, "cls", "variable variable.builtin");
    // ...and a real parameter beside them is a parameter, not a plain local:
    // self/cls are excluded from the parameter patterns precisely so the two
    // stay distinguishable in the same signature.
    ExpectCaptures("py", methods, "other", "variable variable.parameter");
}

void TestPythonParameters() {
    // Every shape a signature can bind a name in gets the parameter capture,
    // and it lands on the *name* -- not on the annotation, the default value or
    // the stars.
    const std::string src =
        "def f(a, b: int, c=1, d: str = \"x\", *args, **kwargs):\n"
        "    return a + b\n";
    ExpectCaptures("py", src, "a,", "variable variable.parameter");
    ExpectCaptures("py", src, "b:", "variable variable.parameter");
    ExpectCaptures("py", src, "c=", "variable variable.parameter");
    ExpectCaptures("py", src, "d:", "variable variable.parameter");
    ExpectCaptures("py", src, "args", "variable variable.parameter");
    ExpectCaptures("py", src, "kwargs", "variable variable.parameter");
    // The annotation and the default keep their own captures.
    ExpectCaptures("py", src, "int", "variable type");
    ExpectCaptures("py", src, "str", "variable type");
    ExpectCaptures("py", src, "1,", "number");
    ExpectCaptures("py", src, "\"x\"", "string");
    // A use in the body is an ordinary local: which of them is a parameter
    // needs scope resolution, which a query cannot do.
    ExpectCaptures("py", src, "a + b", "variable");

    // Annotated *args/**kwargs nest one level deeper in the grammar (the splat
    // sits inside a typed_parameter), and are still parameters.
    const std::string typed_splat = "def g(*args: int, **kwargs: str):\n    pass\n";
    ExpectCaptures("py", typed_splat, "args", "variable variable.parameter");
    ExpectCaptures("py", typed_splat, "kwargs", "variable variable.parameter");

    // A lambda binds parameters the same way.
    const std::string lam = "key = lambda item, n=2: item[n]\n";
    ExpectCaptures("py", lam, "item,", "variable variable.parameter");
    ExpectCaptures("py", lam, "n=", "variable variable.parameter");
    ExpectCaptures("py", lam, "item[", "variable");

    // At a call site it is the keyword's name that is the parameter; its value
    // is ordinary code. A positional argument is just an expression.
    const std::string call = "f(1, x, timeout=3, mode=MODE)\n";
    ExpectCaptures("py", call, "timeout", "variable variable.parameter");
    ExpectCaptures("py", call, "mode", "variable variable.parameter");
    ExpectCaptures("py", call, "3", "number");
    ExpectCaptures("py", call, "MODE", "variable constant");
    ExpectCaptures("py", call, "x,", "variable");
}

void TestPythonCalls() {
    const std::string src =
        "import os\n"
        "\n"
        "def helper(x):\n"
        "    return len(x) + os.stat(x).st_size + helper(x)\n";
    ExpectCaptures("py", src, "helper", "variable function", 0);  // the definition
    ExpectCaptures("py", src, "helper(x)", "variable function");  // the call
    ExpectCaptures("py", src, "len", "variable function function.builtin");
    ExpectCaptures("py", src, "stat", "variable property function.method");
    // An attribute that is not being called is a member rather than a
    // function -- and @property has no colour of its own, so it reads as
    // plain text instead of borrowing the method blue.
    ExpectCaptures("py", src, "st_size", "variable property");

    // A decorator colours its own name and its `@`, and stops there: the
    // arguments of a parametrised decorator are ordinary code.
    const std::string deco =
        "@functools.lru_cache(maxsize=None)\n"
        "def f():\n"
        "    pass\n";
    ExpectCaptures("py", deco, "@", "function");
    ExpectCaptures("py", deco, "functools", "function variable");
    ExpectCaptures("py", deco, "lru_cache", "function variable property function.method");
    ExpectCaptures("py", deco, "maxsize", "variable variable.parameter");
    ExpectCaptures("py", deco, "None", "constant.builtin");
}

void TestPythonAnnotations() {
    // A whole annotation reads as a type -- not just a bare name, so a
    // subscripted or dotted or unioned one does not come out as a type plus
    // uncoloured punctuation.
    const std::string src =
        "def f(a: int, b: list[int], c: os.PathLike, d: int | None = None) -> \"Widget\":\n"
        "    x: dict[str, int] = {}\n"
        "    return x\n";
    ExpectCaptures("py", src, "int,", "variable type");
    ExpectCaptures("py", src, "list[int]", "type variable");
    ExpectCaptures("py", src, "[int]", "type");
    ExpectCaptures("py", src, "os.PathLike", "type variable");
    ExpectCaptures("py", src, "int | None", "type variable");
    ExpectCaptures("py", src, "dict[str, int]", "type variable");
    // A literal in an annotation position keeps being a literal: the query
    // lists the annotation shapes it colours rather than taking (type (_)).
    ExpectCaptures("py", src, "\"Widget\"", "string");
    // The `None` inside the union is part of the annotation (and so wears its
    // type colour); the default value after `=` is an ordinary literal again.
    ExpectCaptures("py", src, "None", "type constant.builtin", 0);
    ExpectCaptures("py", src, "None", "constant.builtin", 1);
}

void TestPythonLiteralsAndComments() {
    const std::string src =
        "# a comment\n"
        "flags = (True, False, None)\n"
        "nums = (1, 2.5, 0x10, 1_000, 3j)\n"
        "doc = \"\"\"line one\n"
        "line two\n"
        "\"\"\"\n";
    ExpectCaptures("py", src, "# a comment", "comment");
    ExpectCaptures("py", src, "True", "constant.builtin");
    ExpectCaptures("py", src, "False", "constant.builtin");
    ExpectCaptures("py", src, "None", "constant.builtin");
    ExpectCaptures("py", src, "1,", "number");
    ExpectCaptures("py", src, "2.5", "number");
    ExpectCaptures("py", src, "0x10", "number");
    ExpectCaptures("py", src, "1_000", "number");
    ExpectCaptures("py", src, "3j", "number");
    // A multi-line string is split into one span per line (mep's decoration
    // model is per-line), so every row of it is still painted.
    ExpectCaptures("py", src, "line one", "string");
    ExpectCaptures("py", src, "line two", "string");
}

// ---------------------------------------------------------------------------
// Python: mep's own grammar (grammars/tree-sitter-python) -- the newer syntax
// its scanner and grammar handle, its folds and its outline
// ---------------------------------------------------------------------------

void TestPythonGrammar() {
    CHECK(TreesitterHasStructureQuery("py"));
    const std::string src =
        "MAX_SIZE: int = 10\n"
        "helper = 1\n"
        "type Pair[T] = tuple[T, T]\n"
        "class Shape[T]:\n"
        "    \"\"\"A shape.\n"
        "    \"\"\"\n"
        "    def area(self, *, scale=1.0) -> float:\n"
        "        s = f\"{self.name!r:>{width}} {'q'} {f\"{x}\"}\" + r'\\\\'\n"
        "        return ...\n"
        "def main():\n"
        "    try:\n"
        "        pass\n"
        "    except* ValueError as e:\n"
        "        match[0] = x \\\n"
        "            + 1\n"
        "    match e:\n"
        "        case [1, *rest] if rest:\n"
        "            pass\n";
    // PEP 701: a string in an f-string's field may reuse the outer quotes.
    ExpectCaptures("py", src, "'q'", "string");
    ExpectCaptures("py", src, "f\"{x}", "string");
    ExpectCaptures("py", src, "x}\"}\"", "variable");
    ExpectCaptures("py", src, "width", "string.special variable");
    // r'\\' is a backslash pair and a quote, not an escaped quote.
    ExpectCaptures("py", src, "\\\\'", "string");
    ExpectCaptures("py", src, "...", "constant.builtin");
    ExpectCaptures("py", src, "except", "keyword");
    ExpectCaptures("py", src, "ValueError", "variable constructor");
    // `match` and `type` are names except where they start their statements.
    ExpectCaptures("py", src, "match[0]", "variable");
    ExpectCaptures("py", src, "match e", "keyword");
    ExpectCaptures("py", src, "type Pair", "keyword");
    ExpectCaptures("py", src, "\\\n", "punctuation.special");

    ExpectOutline("py", src,
                  "0:variable:MAX_SIZE@0\n"
                  "0:type:Pair@2\n"
                  "0:class:Shape@3\n"
                  "1:function:area@6\n"
                  "0:function:main@9\n");

    bool docstring = false, except = false, match_case = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("py", src)) {
        docstring = docstring || (f.start_row == 4 && f.end_row == 5);
        except = except || (f.start_row == 12 && f.end_row == 14);
        match_case = match_case || (f.start_row == 16 && f.end_row == 17);
    }
    CHECK(docstring);
    CHECK(except);
    CHECK(match_case);
}

// ---------------------------------------------------------------------------
// Maxima: mep's own grammar (grammars/tree-sitter-maxima), compiled in
// ---------------------------------------------------------------------------

void TestMaxima() {
    CHECK(TreesitterHasGrammar("mac"));
    CHECK(TreesitterHasGrammar("max"));
    CHECK(TreesitterHasFoldQuery("mac"));
    const std::string src =
        "/* outer /* nested */ still comment */\n"
        "f(x, [y]) := block([a : 1], a + ?princ(x), -x^2)$\n"
        "g : lambda([t], t!)$\n"
        "for i : 1 thru 10 do if i > 5 then print(i) else return(%pi);\n"
        "integrate(%e^lambda, lambda, 0, 1);\n"
        ":lisp (defun h () 1)\n";
    ExpectCaptures("mac", src, "still comment", "comment");
    ExpectCaptures("mac", src, "f(x", "variable function.call function");
    ExpectCaptures("mac", src, ":=", "operator");
    ExpectCaptures("mac", src, "block", "keyword.function");
    ExpectCaptures("mac", src, "a :", "variable variable variable.parameter");
    ExpectCaptures("mac", src, "?princ", "variable function.call function.builtin");
    ExpectCaptures("mac", src, "^2", "operator");
    ExpectCaptures("mac", src, "2)", "number");
    ExpectCaptures("mac", src, "lambda([t", "keyword.function");
    ExpectCaptures("mac", src, "t]", "variable variable.parameter");
    ExpectCaptures("mac", src, "for", "repeat");
    ExpectCaptures("mac", src, "i :", "variable variable.parameter");
    ExpectCaptures("mac", src, "thru", "repeat");
    ExpectCaptures("mac", src, "if", "conditional");
    ExpectCaptures("mac", src, "print", "variable function.call function.builtin");
    ExpectCaptures("mac", src, "%pi", "variable constant.builtin");
    // `lambda` with no `(` after it is a name.
    ExpectCaptures("mac", src, "lambda,", "variable");
    ExpectCaptures("mac", src, ":lisp", "string.special");

    // Folds: the block, the loop, a multi-line call.
    const std::string folds = "f(x) := block(\n  [a],\n  a : x,\n  a)$\n";
    bool block_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("mac", folds)) block_fold = block_fold || (r.start_row == 0 && r.end_row >= 3);
    CHECK(block_fold);

    // The outline: definitions and top-level assignments.
    CHECK(TreesitterHasStructureQuery("mac"));
    ExpectOutline("mac",
                  "f(x) := x^2$\n"
                  "m(x) ::= buildq([x], x)$\n"
                  "fib[n] := fib[n-1] + fib[n-2]$\n"
                  "limit_n : 100$\n"
                  "print(f(3));\n",
                  "0:function:f@0\n0:function:m@1\n0:function:fib@2\n0:variable:limit_n@3\n");
}

// ---------------------------------------------------------------------------
// Tcl: mep's own grammar (grammars/tree-sitter-tcl), compiled in
// ---------------------------------------------------------------------------

void TestTcl() {
    CHECK(TreesitterHasGrammar("tcl"));
    CHECK(TreesitterHasGrammar("tk"));
    CHECK(TreesitterHasFoldQuery("tcl"));
    const std::string src =
        "# a comment \\\n"
        "continued\n"
        "proc ::ns::greet {name {greeting hello}} {\n"
        "    #if {$x} {\n"
        "    #}\n"
        "    set msg \"$greeting, $name($i)\"\n"
        "    if {$name eq \"\" || [string length $name] > 10} then {\n"
        "        return -code error {bad name: \"}\n"
        "    } else {\n"
        "        puts stderr $msg\n"
        "    }\n"
        "    expr {sin($x) ** 2 + 0x1F}\n"
        "    lappend out {*}$args\n"
        "}\n";
    ExpectCaptures("tcl", src, "# a comment", "comment");
    ExpectCaptures("tcl", src, "continued", "comment");
    ExpectCaptures("tcl", src, "proc", "keyword.function");
    ExpectCaptures("tcl", src, "::ns::greet", "function");
    ExpectCaptures("tcl", src, "name {", "variable.parameter");
    ExpectCaptures("tcl", src, "greeting hello", "variable.parameter");
    ExpectCaptures("tcl", src, "#if", "comment");
    ExpectCaptures("tcl", src, "msg", "variable");
    ExpectCaptures("tcl", src, "$greeting", "string variable");
    ExpectCaptures("tcl", src, "if {$name", "keyword.conditional");
    ExpectCaptures("tcl", src, "eq", "operator");
    ExpectCaptures("tcl", src, "string length", "function.call function.builtin");
    ExpectCaptures("tcl", src, "then", "keyword.conditional");
    ExpectCaptures("tcl", src, "return", "function.call keyword");
    ExpectCaptures("tcl", src, "-code", "variable.parameter");
    ExpectCaptures("tcl", src, "else", "keyword.conditional");
    ExpectCaptures("tcl", src, "sin", "function.call");
    ExpectCaptures("tcl", src, "**", "operator");
    ExpectCaptures("tcl", src, "0x1F", "number");
    ExpectCaptures("tcl", src, "{*}", "operator");

    // Folds: the proc body, the if's branches.
    bool proc_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("tcl", src)) proc_fold = proc_fold || (r.start_row == 2 && r.end_row >= 13);
    CHECK(proc_fold);

    // The outline: procs nest under their namespace.
    CHECK(TreesitterHasStructureQuery("tcl"));
    ExpectOutline("tcl",
                  "namespace eval ::app {\n"
                  "    variable count 0\n"
                  "    proc run {args} {\n"
                  "        incr count\n"
                  "    }\n"
                  "}\n"
                  "set VERSION 1.0\n"
                  "proc main {} { app::run }\n",
                  "0:module:::app@0\n1:variable:count@1\n1:function:run@2\n0:variable:VERSION@6\n0:function:main@7\n");
}

// ---------------------------------------------------------------------------
// Go: mep's own grammar (grammars/tree-sitter-go), compiled in
// ---------------------------------------------------------------------------

void TestGo() {
    CHECK(TreesitterHasGrammar("go"));
    CHECK(TreesitterHasFoldQuery("go"));
    const std::string src =
        "// Package demo is a demo.\n"
        "package demo\n"
        "\n"
        "import (\n"
        "\t\"fmt\"\n"
        "\t\"strings\"\n"
        ")\n"
        "\n"
        "const MaxDepth = 3\n"
        "\n"
        "type Shape interface {\n"
        "\tArea() float64\n"
        "}\n"
        "\n"
        "type Point struct {\n"
        "\tX, Y int\n"
        "}\n"
        "\n"
        "func (p *Point) Scale(k int) Point {\n"
        "\treturn Point{p.X * k, p.Y * k}\n"
        "}\n"
        "\n"
        "func Join(parts ...string) string {\n"
        "\tn := len(parts)\n"
        "\tv := new(int64(300))\n"
        "\tfmt.Println(n, *v, 'x', 1.5, nil, true)\n"
        "\treturn strings.Join(parts, \"\\n\")\n"
        "}\n";
    ExpectCaptures("go", src, "// Package", "comment");
    ExpectCaptures("go", src, "package", "keyword");
    ExpectCaptures("go", src, "demo\n", "module");
    ExpectCaptures("go", src, "\"fmt\"", "string");
    ExpectCaptures("go", src, "MaxDepth", "variable constant");
    ExpectCaptures("go", src, "Shape", "type");
    ExpectCaptures("go", src, "Area", "property function.method");
    ExpectCaptures("go", src, "float64", "type type.builtin");
    ExpectCaptures("go", src, "func (p", "keyword");
    ExpectCaptures("go", src, "Scale", "property function.method");
    ExpectCaptures("go", src, "k int", "variable variable.parameter");
    ExpectCaptures("go", src, "X *", "property");
    ExpectCaptures("go", src, "Join(parts", "variable function");
    ExpectCaptures("go", src, "...string", "operator");
    ExpectCaptures("go", src, ":=", "operator");
    ExpectCaptures("go", src, "len(", "variable function.call function.builtin");
    ExpectCaptures("go", src, "new(", "variable function.call function.builtin");
    ExpectCaptures("go", src, "Println", "property function.method.call");
    ExpectCaptures("go", src, "'x'", "string");
    ExpectCaptures("go", src, "1.5", "number");
    ExpectCaptures("go", src, "nil", "constant.builtin");
    ExpectCaptures("go", src, "true", "boolean");
    ExpectCaptures("go", src, "\\n", "string string.escape");

    // Folds: the import list, the struct body, a function body.
    bool import_fold = false, func_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("go", src)) {
        import_fold = import_fold || (r.start_row == 3 && r.end_row >= 6);
        func_fold = func_fold || (r.start_row == 22 && r.end_row >= 27);
    }
    CHECK(import_fold);
    CHECK(func_fold);

    // The outline: interface methods nest under their interface.
    CHECK(TreesitterHasStructureQuery("go"));
    ExpectOutline("go", src,
                  "0:constant:MaxDepth@8\n0:interface:Shape@10\n1:method:Area@11\n0:struct:Point@14\n"
                  "0:method:Scale@18\n0:function:Join@22\n");
}

// ---------------------------------------------------------------------------
// Java: mep's own grammar (grammars/tree-sitter-java), compiled in
// ---------------------------------------------------------------------------

void TestJava() {
    CHECK(TreesitterHasGrammar("java"));
    CHECK(TreesitterHasFoldQuery("java"));
    const std::string src =
        "// Demo.\n"
        "package demo;\n"
        "\n"
        "import module java.base;\n"
        "\n"
        "public record Point(int x, int y) {\n"
        "  static final int MAX_DEPTH = 0b1__0;\n"
        "\n"
        "  Point {\n"
        "    if (x < 0) throw new IllegalArgumentException(\"x\\n\");\n"
        "  }\n"
        "\n"
        "  int area(Object o, String @Deprecated ... rest) {\n"
        "    return switch (o) {\n"
        "      case Shape.Tri _, Square _ -> 1;\n"
        "      default -> rest.length;\n"
        "    };\n"
        "  }\n"
        "}\n";
    ExpectCaptures("java", src, "// Demo", "comment");
    ExpectCaptures("java", src, "package", "keyword");
    ExpectCaptures("java", src, "import", "keyword");
    ExpectCaptures("java", src, "record", "keyword");
    ExpectCaptures("java", src, "Point(", "variable type");
    ExpectCaptures("java", src, "MAX_DEPTH", "variable constant");
    ExpectCaptures("java", src, "0b1__0", "number");
    ExpectCaptures("java", src, "\"x", "string");
    ExpectCaptures("java", src, "\\n", "string string.escape");
    ExpectCaptures("java", src, "area", "variable function.method");
    ExpectCaptures("java", src, "int area", "type.builtin");
    ExpectCaptures("java", src, "switch", "keyword");

    // Folds: the record body and the method body.
    bool body_fold = false, method_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("java", src)) {
        body_fold = body_fold || (r.start_row == 5 && r.end_row >= 17);
        method_fold = method_fold || (r.start_row == 12 && r.end_row >= 16);
    }
    CHECK(body_fold);
    CHECK(method_fold);

    // The outline: members nest under their record.
    CHECK(TreesitterHasStructureQuery("java"));
    ExpectOutline("java", src,
                  "0:class:Point@5\n1:field:MAX_DEPTH@6\n1:method:Point@8\n1:method:area@12\n");
}

// ---------------------------------------------------------------------------
// D: mep's own grammar (grammars/tree-sitter-d), compiled in
// ---------------------------------------------------------------------------

void TestD() {
    CHECK(TreesitterHasGrammar("d"));
    CHECK(TreesitterHasFoldQuery("d"));
    const std::string src =
        "/// Module docs.\n"
        "module demo.shapes;\n"
        "\n"
        "import std.stdio : writeln;\n"
        "\n"
        "enum MaxDepth = 3;\n"
        "\n"
        "struct Point\n"
        "{\n"
        "    int x, y;\n"
        "    this(int a) { x = a; }\n"
        "}\n"
        "\n"
        "/+ a /+ nested +/ comment +/\n"
        "string greet(string name) in (name.length) do\n"
        "{\n"
        "    auto s = \"hi\\n\" ~ name;\n"
        "    return s ~ 'x' ~ q{int y;}c;\n"
        "}\n";
    ExpectCaptures("d", src, "/// Module", "comment");
    ExpectCaptures("d", src, "/+ a", "comment");
    ExpectCaptures("d", src, "module", "keyword");
    ExpectCaptures("d", src, "MaxDepth", "variable constant");
    ExpectCaptures("d", src, "Point", "variable type");
    ExpectCaptures("d", src, "int x", "type.builtin");
    ExpectCaptures("d", src, "greet", "variable function");
    ExpectCaptures("d", src, "\"hi", "string");
    ExpectCaptures("d", src, "\\n", "string string.escape");
    ExpectCaptures("d", src, "'x'", "number");
    ExpectCaptures("d", src, "q{", "string");
    ExpectCaptures("d", src, "do\n", "keyword");

    // Folds: the struct body, the function body.
    bool struct_fold = false, func_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("d", src)) {
        struct_fold = struct_fold || (r.start_row == 8 && r.end_row >= 11);
        func_fold = func_fold || (r.start_row == 15 && r.end_row >= 18);
    }
    CHECK(struct_fold);
    CHECK(func_fold);

    // The outline: fields and the constructor nest under their struct.
    CHECK(TreesitterHasStructureQuery("d"));
    ExpectOutline("d", src,
                  "0:module:demo.shapes@1\n0:constant:MaxDepth@5\n0:struct:Point@7\n1:field:x@9\n1:field:y@9\n"
                  "1:constructor:this@10\n0:function:greet@14\n");
}

// ---------------------------------------------------------------------------
// Nim: mep's own grammar (grammars/tree-sitter-nim), compiled in
// ---------------------------------------------------------------------------

void TestNim() {
    CHECK(TreesitterHasGrammar("nim"));
    CHECK(TreesitterHasFoldQuery("nim"));
    const std::string src =
        "## Module docs.\n"
        "import std/[strutils, tables]\n"
        "\n"
        "const MaxDepth* = 3\n"
        "\n"
        "type\n"
        "  Shape* = ref object of RootObj\n"
        "    name: string\n"
        "  Color = enum\n"
        "    red, green\n"
        "\n"
        "proc area*(s: Shape; k: int): float {.inline.} =\n"
        "  # a comment\n"
        "  result = 1.5 * k.float\n"
        "  echo \"v\\n\", 'c', nil\n"
        "\n"
        "template twice(body: untyped) =\n"
        "  body\n"
        "  body\n"
        "\n"
        "let v = @[1, 2].map(proc (x: int): int =\n"
        "  x * 2\n"
        "  )\n";
    ExpectCaptures("nim", src, "## Module", "comment.documentation");
    ExpectCaptures("nim", src, "import", "include");
    ExpectCaptures("nim", src, "MaxDepth", "variable");
    ExpectCaptures("nim", src, "area", "function");
    ExpectCaptures("nim", src, "k: int", "variable parameter");
    ExpectCaptures("nim", src, "float {", "type");
    ExpectCaptures("nim", src, "# a comment", "comment");
    ExpectCaptures("nim", src, "1.5", "float");
    ExpectCaptures("nim", src, "\"v", "string");
    ExpectCaptures("nim", src, "\\n", "string string.escape");
    ExpectCaptures("nim", src, "'c'", "character");
    ExpectCaptures("nim", src, "nil", "constant.builtin keyword");
    ExpectCaptures("nim", src, "twice", "function.macro");

    // Folds: the type section, a proc body.
    bool type_fold = false, proc_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("nim", src)) {
        type_fold = type_fold || (r.start_row == 5 && r.end_row >= 9);
        proc_fold = proc_fold || (r.start_row == 11 && r.end_row >= 14);
    }
    CHECK(type_fold);
    CHECK(proc_fold);

    // The outline: object fields and enum members nest under their type.
    CHECK(TreesitterHasStructureQuery("nim"));
    ExpectOutline("nim", src,
                  "0:constant:MaxDepth@3\n0:struct:Shape@6\n1:field:name@7\n0:enum:Color@8\n1:enummember:red@9\n"
                  "1:enummember:green@9\n0:function:area@11\n0:macro:twice@16\n0:variable:v@20\n");
}

// ---------------------------------------------------------------------------
// Rust: mep's own grammar (grammars/tree-sitter-rust), compiled in
// ---------------------------------------------------------------------------

void TestRust() {
    CHECK(TreesitterHasGrammar("rs"));
    CHECK(TreesitterHasFoldQuery("rs"));
    const std::string src =
        "//! Crate docs.\n"
        "use std::collections::HashMap;\n"
        "\n"
        "const MAX_DEPTH: usize = 3;\n"
        "\n"
        "pub trait Shape {\n"
        "    fn area(&self) -> f64;\n"
        "}\n"
        "\n"
        "#[derive(Debug)]\n"
        "struct Point<T> {\n"
        "    x: T,\n"
        "}\n"
        "\n"
        "impl<T: Copy> Point<T> {\n"
        "    fn get(&self, k: usize) -> T {\n"
        "        let m: HashMap<u8, u8> = HashMap::new();\n"
        "        println!(\"{} {}\\n\", k, 'c');\n"
        "        'outer: loop { break 'outer; }\n"
        "        self.x\n"
        "    }\n"
        "}\n"
        "\n"
        "macro_rules! square {\n"
        "    ($e:expr) => { $e * $e };\n"
        "}\n";
    ExpectCaptures("rs", src, "//! Crate", "comment comment.documentation");
    ExpectCaptures("rs", src, "use std", "keyword");
    ExpectCaptures("rs", src, "MAX_DEPTH", "constructor constant");
    ExpectCaptures("rs", src, "usize = 3", "type.builtin");
    ExpectCaptures("rs", src, "trait", "keyword");
    ExpectCaptures("rs", src, "Shape", "type");
    ExpectCaptures("rs", src, "area", "function");
    ExpectCaptures("rs", src, "self)", "variable.builtin");
    ExpectCaptures("rs", src, "#[derive", "attribute");
    ExpectCaptures("rs", src, "x: T", "property");
    ExpectCaptures("rs", src, "k: usize", "variable.parameter");
    ExpectCaptures("rs", src, "new()", "function");
    ExpectCaptures("rs", src, "println", "function.macro");
    ExpectCaptures("rs", src, "\\n", "string escape");
    ExpectCaptures("rs", src, "'c'", "string");
    ExpectCaptures("rs", src, "outer: loop", "label");
    ExpectCaptures("rs", src, "3;", "constant.builtin");
    ExpectCaptures("rs", src, "$e:", "variable");
    ExpectCaptures("rs", src, "expr)", "type.builtin");
    ExpectCaptures("rs", src, "->", "operator");

    // Folds: the trait body, the impl body, a function body.
    bool impl_fold = false, fn_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("rs", src)) {
        impl_fold = impl_fold || (r.start_row == 14 && r.end_row >= 21);
        fn_fold = fn_fold || (r.start_row == 15 && r.end_row >= 20);
    }
    CHECK(impl_fold);
    CHECK(fn_fold);

    // The outline: trait and impl methods nest under their trait or impl.
    CHECK(TreesitterHasStructureQuery("rs"));
    ExpectOutline("rs", src,
                  "0:constant:MAX_DEPTH@3\n0:interface:Shape@5\n1:function:area@6\n0:struct:Point@10\n"
                  "0:impl:Point@14\n1:function:get@15\n0:macro:square@23\n");
}

// ---------------------------------------------------------------------------
// Zig: mep's own grammar (grammars/tree-sitter-zig), compiled in
// ---------------------------------------------------------------------------

void TestZig() {
    CHECK(TreesitterHasGrammar("zig"));
    CHECK(TreesitterHasFoldQuery("zig"));
    const std::string src =
        "//! Module docs.\n"
        "const std = @import(\"std\");\n"
        "\n"
        "pub const max_depth = 3;\n"
        "\n"
        "pub const Point = struct {\n"
        "    x: i32,\n"
        "    y: i32 = 0,\n"
        "\n"
        "    pub fn scale(self: Point, k: i32) Point {\n"
        "        return .{ .x = self.x * k, .y = self.y * k };\n"
        "    }\n"
        "};\n"
        "\n"
        "const Color = enum { red, green };\n"
        "\n"
        "fn List(comptime T: type) type {\n"
        "    return struct { items: []T };\n"
        "}\n"
        "\n"
        "test \"scale\" {\n"
        "    const p = Point{ .x = 1 };\n"
        "    const io = .{ .async = async };\n"
        "    asm volatile (\"\" ::: .{ .memory = true });\n"
        "    try std.testing.expect(p.scale(2).x == 'x' or 1.5 > 0 and true);\n"
        "    _ = \"a\\n\";\n"
        "}\n";
    ExpectCaptures("zig", src, "//! Module", "comment spell comment.documentation");
    ExpectCaptures("zig", src, "const std", "keyword");
    ExpectCaptures("zig", src, "std =", "variable module");
    ExpectCaptures("zig", src, "@import", "function.builtin keyword.import");
    ExpectCaptures("zig", src, "\"std\"", "string");
    ExpectCaptures("zig", src, "pub", "keyword.modifier");
    ExpectCaptures("zig", src, "Point = struct", "variable type type");
    ExpectCaptures("zig", src, "struct {\n    x", "keyword.type");
    ExpectCaptures("zig", src, "x: i32", "variable variable.member");
    ExpectCaptures("zig", src, "i32,", "type.builtin");
    ExpectCaptures("zig", src, "fn scale", "keyword.function");
    ExpectCaptures("zig", src, "scale(self", "variable function");
    ExpectCaptures("zig", src, "k: i32", "variable variable.parameter");
    ExpectCaptures("zig", src, "return", "keyword.return");
    ExpectCaptures("zig", src, "async }", "variable");
    ExpectCaptures("zig", src, "asm", "keyword");
    ExpectCaptures("zig", src, "try", "keyword.exception");
    ExpectCaptures("zig", src, "expect(", "variable variable.member function.call");
    ExpectCaptures("zig", src, "'x'", "character");
    ExpectCaptures("zig", src, "1.5", "number.float");
    ExpectCaptures("zig", src, "or 1.5", "keyword.operator");
    ExpectCaptures("zig", src, "true)", "boolean");
    ExpectCaptures("zig", src, "\\n", "string string.escape");

    // Folds: the struct, a function body.
    bool struct_fold = false, func_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("zig", src)) {
        struct_fold = struct_fold || (r.start_row == 5 && r.end_row >= 12);
        func_fold = func_fold || (r.start_row == 9 && r.end_row >= 11);
    }
    CHECK(struct_fold);
    CHECK(func_fold);

    // The outline: fields and methods nest under their struct.
    CHECK(TreesitterHasStructureQuery("zig"));
    ExpectOutline("zig", src,
                  "0:variable:std@1\n0:variable:max_depth@3\n0:struct:Point@5\n1:field:x@6\n1:field:y@7\n"
                  "1:function:scale@9\n0:enum:Color@14\n1:enummember:red@14\n1:enummember:green@14\n"
                  "0:class:List@16\n1:field:items@17\n0:function:scale@20\n");
}

// ---------------------------------------------------------------------------
// GAP: mep's own grammar (grammars/tree-sitter-gap), compiled in
// ---------------------------------------------------------------------------

void TestGap() {
    CHECK(TreesitterHasGrammar("g"));
    CHECK(TreesitterHasGrammar("gi"));
    CHECK(TreesitterHasFoldQuery("gd"));
    const std::string src =
        "local helper;\n"
        "# a comment\n"
        "helper := function(G, arg)\n"
        "    local x;\n"
        "    if IsBound(G.name) then return \"n\\n\"; fi;\n"
        "    for x in [1..10] do Info(InfoWarning, 1, x); od;\n"
        "    return rec(a := 1.5, b := 'c');\n"
        "end;\n";
    ExpectCaptures("g", src, "local helper", "keyword.function");
    ExpectCaptures("g", src, "# a comment", "comment");
    ExpectCaptures("g", src, "helper :=", "variable function");
    ExpectCaptures("g", src, "function(", "keyword.function");
    ExpectCaptures("g", src, "G, arg", "variable variable.parameter");
    ExpectCaptures("g", src, "if", "keyword.conditional");
    ExpectCaptures("g", src, "\\n", "string string.escape");
    ExpectCaptures("g", src, "for", "keyword.repeat");
    ExpectCaptures("g", src, "1.5", "number.float");
    ExpectCaptures("g", src, "'c'", "character");

    bool fn_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("g", src)) fn_fold = fn_fold || (r.start_row == 2 && r.end_row >= 7);
    CHECK(fn_fold);

    CHECK(TreesitterHasStructureQuery("gi"));
    ExpectOutline("gi",
                  "DeclareOperation(\"Frob\", [IsGroup]);\n"
                  "InstallMethod(Frob, \"for groups\", [IsGroup], G -> G);\n"
                  "LIMIT := 10;\n"
                  "Helper := function(x)\n"
                  "    local inner;\n"
                  "    inner := y -> y;\n"
                  "    return inner(x);\n"
                  "end;\n",
                  "0:type:Frob@0\n0:method:Frob@1\n0:variable:LIMIT@2\n0:function:Helper@3\n1:function:inner@5\n");
}

// ---------------------------------------------------------------------------
// fish: mep's own grammar (grammars/tree-sitter-fish), compiled in
// ---------------------------------------------------------------------------

void TestFish() {
    CHECK(TreesitterHasGrammar("fish"));
    CHECK(TreesitterHasFoldQuery("fish"));
    const std::string src =
        "function greet -a name --description 'say hi'\n"
        "    if test -n \"$name\"; and not set -q quiet\n"
        "        echo \"hi ($name)\" $argv[1..2] ~/x *.fish 2>&1 a&b\n"
        "    else if command -q fortune\n"
        "        fortune | string upper \\\n"
        "            # a comment the continuation carries over\n"
        "            | cat\n"
        "    end\n"
        "    for i in (seq 3); continue; end\n"
        "    return 0\n"
        "end\n";
    ExpectCaptures("fish", src, "function", "keyword");
    ExpectCaptures("fish", src, "greet", "function");
    ExpectCaptures("fish", src, "'say hi'", "string");
    ExpectCaptures("fish", src, "if", "keyword");
    ExpectCaptures("fish", src, "test", "function function");
    ExpectCaptures("fish", src, "-n", "operator");
    ExpectCaptures("fish", src, "and", "keyword");
    ExpectCaptures("fish", src, "not", "keyword");
    // Only `$(` substitutes inside double quotes: `(` is text, `$name` a variable.
    ExpectCaptures("fish", src, "($name)", "string");
    ExpectCaptures("fish", src, "$name)", "string constant");
    ExpectCaptures("fish", src, "..", "constant operator");
    ExpectCaptures("fish", src, "~/", "character.special");
    ExpectCaptures("fish", src, "*.fish", "character.special");
    ExpectCaptures("fish", src, "2>", "operator");
    ExpectCaptures("fish", src, "command", "function keyword");
    // A comment after a continuation keeps the pipeline going: `cat` is a command.
    ExpectCaptures("fish", src, "# a comment", "comment");
    ExpectCaptures("fish", src, "cat", "function");
    ExpectCaptures("fish", src, "continue", "keyword");
    ExpectCaptures("fish", src, "return", "keyword");
    ExpectCaptures("fish", src, "0\n", "number");

    // Folds: the function, the if and its branches, the loop.
    bool function_fold = false, if_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("fish", src)) {
        function_fold = function_fold || (r.start_row == 0 && r.end_row == 10);
        if_fold = if_fold || (r.start_row == 1 && r.end_row == 7);
    }
    CHECK(function_fold);
    CHECK(if_fold);

    // The outline: functions (nested ones nest), exported/global settings,
    // abbreviations and aliases; a plain `set` stays out.
    CHECK(TreesitterHasStructureQuery("fish"));
    ExpectOutline("fish",
                  "set -gx EDITOR mep\n"
                  "set -l tmp 1\n"
                  "abbr -a gs git status\n"
                  "alias ll 'ls -l'\n"
                  "function outer\n"
                  "    function inner; end\n"
                  "end\n",
                  "0:variable:EDITOR@0\n0:function:gs@2\n0:function:ll@3\n0:function:outer@4\n1:function:inner@5\n");
}

// ---------------------------------------------------------------------------
// Fortran: mep's own grammar (grammars/tree-sitter-fortran), free and fixed
// form from one grammar (the scanner recognises fixed form from the text)
// ---------------------------------------------------------------------------

void TestFortran() {
    CHECK(TreesitterHasGrammar("f90"));
    CHECK(TreesitterHasGrammar("f"));
    CHECK(TreesitterHasFoldQuery("f90"));
    const std::string src =
        "module m\n"
        "  implicit none\n"
        "  integer, parameter :: dp = kind(1.0d0)\n"
        "contains\n"
        "  subroutine s(x, n) ! doc\n"
        "    real(dp), intent(inout) :: x(:)\n"
        "    integer :: n, i\n"
        "    do i = 1, n\n"
        "      if (x(i) > 0.5_dp .and. n /= 0) then\n"
        "        x(i) = x(i) + &\n"
        "               2.0_dp\n"
        "      end if\n"
        "    end do\n"
        "    print *, 'done'\n"
        "  end subroutine s\n"
        "end module m\n";
    ExpectCaptures("f90", src, "module", "keyword");
    ExpectCaptures("f90", src, "implicit", "attribute");
    ExpectCaptures("f90", src, "integer", "type");
    ExpectCaptures("f90", src, "! doc", "comment");
    ExpectCaptures("f90", src, "'done'", "string");
    ExpectCaptures("f90", src, "0.5_dp", "number");
    ExpectCaptures("f90", src, "end do", "repeat");
    int folds = 0;
    for (const TSFoldRange &r : TreesitterFoldRanges("f90", src)) {
        (void)r;
        folds++;
    }
    CHECK(folds >= 4);  // module, subroutine, do, if
    CHECK(TreesitterHasStructureQuery("f90"));
    ExpectOutline("f90", src, "0:module:m@0\n1:function:s@4\n");

    // Fixed form: `*`/`C` comment lines and column-6 continuations.
    const std::string fixed =
        "*> \\brief doc\n"
        "      SUBROUTINE FOO( A, B,\n"
        "     $                C )\n"
        "C     a comment\n"
        "      INTEGER A, B, C\n"
        "      A = B +\n"
        "     $    C\n"
        "      END\n";
    ExpectCaptures("f", fixed, "*> \\brief", "comment");
    ExpectCaptures("f", fixed, "C     a comment", "comment");
    ExpectCaptures("f", fixed, "INTEGER", "type");
    ExpectOutline("f", fixed, "0:function:FOO@1\n");
}

// ---------------------------------------------------------------------------
// MATLAB/Octave (.m): mep's own grammar (grammars/tree-sitter-matlab)
// ---------------------------------------------------------------------------

void TestMatlab() {
    CHECK(TreesitterHasGrammar("m"));
    CHECK(TreesitterHasFoldQuery("m"));
    const std::string src =
        "## -*- texinfo -*-\n"
        "function r = scale (x, k = 2)\n"
        "  if (! isempty (x) && k != 0)\n"
        "    r = (x .* k)(:);  # a column\n"
        "  endif\n"
        "  do\n"
        "    k++;\n"
        "  until (k > 10)\n"
        "  unwind_protect\n"
        "    printf (\"%d\\n\", k);\n"
        "  unwind_protect_cleanup\n"
        "    k += 1;\n"
        "  end_unwind_protect\n"
        "endfunction\n";
    ExpectCaptures("m", src, "## -*-", "comment");
    ExpectCaptures("m", src, "function", "keyword.function");
    ExpectCaptures("m", src, "scale", "variable function");
    ExpectCaptures("m", src, "2)", "number");
    ExpectCaptures("m", src, "if", "conditional");
    ExpectCaptures("m", src, "!", "operator");
    ExpectCaptures("m", src, "isempty", "variable function.call");
    ExpectCaptures("m", src, "!=", "operator");
    ExpectCaptures("m", src, "# a column", "comment");
    ExpectCaptures("m", src, "endif", "conditional");
    ExpectCaptures("m", src, "do", "repeat");
    ExpectCaptures("m", src, "++", "operator");
    ExpectCaptures("m", src, "until", "repeat");
    ExpectCaptures("m", src, "unwind_protect\n", "exception");
    ExpectCaptures("m", src, "unwind_protect_cleanup", "exception");
    ExpectCaptures("m", src, "\\n", "string string.escape");
    ExpectCaptures("m", src, "%d", "string string.special");
    ExpectCaptures("m", src, "+=", "operator");
    ExpectCaptures("m", src, "end_unwind_protect", "exception");
    ExpectCaptures("m", src, "endfunction", "keyword.function");

    // Folds: the function, the if, the do loop, the unwind_protect.
    bool function_fold = false, if_fold = false, do_fold = false, unwind_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("m", src)) {
        function_fold = function_fold || (r.start_row == 1 && r.end_row == 13);
        if_fold = if_fold || (r.start_row == 2 && r.end_row == 4);
        do_fold = do_fold || (r.start_row == 5 && r.end_row == 7);
        unwind_fold = unwind_fold || (r.start_row == 8 && r.end_row == 12);
    }
    CHECK(function_fold);
    CHECK(if_fold);
    CHECK(do_fold);
    CHECK(unwind_fold);

    // The outline: a classdef, its properties, methods and events; and
    // functions, nested ones nesting.
    CHECK(TreesitterHasStructureQuery("m"));
    ExpectOutline("m",
                  "classdef Point < handle\n"
                  "  properties\n"
                  "    x = 0\n"
                  "  endproperties\n"
                  "  methods\n"
                  "    function d = norm (p)\n"
                  "      d = abs (p.x);\n"
                  "    endfunction\n"
                  "  endmethods\n"
                  "  events\n"
                  "    Moved\n"
                  "  endevents\n"
                  "endclassdef\n",
                  "0:class:Point@0\n1:field:x@2\n1:method:norm@5\n1:constant:Moved@10\n");
    ExpectOutline("m",
                  "function outer ()\n"
                  "  function inner ()\n"
                  "  endfunction\n"
                  "endfunction\n",
                  "0:function:outer@0\n1:function:inner@1\n");
}

// ---------------------------------------------------------------------------
// Nix: mep's own grammar (grammars/tree-sitter-nix), compiled in
// ---------------------------------------------------------------------------

void TestNix() {
    CHECK(TreesitterHasGrammar("nix"));
    CHECK(TreesitterHasFoldQuery("nix"));
    const std::string src =
        "{ lib, stdenv ? null, ... }:\n"
        "let\n"
        "  # a comment\n"
        "  name = \"x-${lib.version} \\n $HOME\";\n"
        "  path = ./pkgs/${name}.nix;\n"
        "in\n"
        "stdenv.mkDerivation rec {\n"
        "  inherit name;\n"
        "  src = builtins.fetchurl https://example.org/a.tar.gz;\n"
        "  doCheck = if true then null else 1.5;\n"
        "  installPhase = ''\n"
        "    cp ''${out} ${lib.getBin stdenv}\n"
        "  '';\n"
        "  meta = map (x: x + 1) [ 2 ];\n"
        "}\n";
    ExpectCaptures("nix", src, "lib,", "variable.parameter");
    ExpectCaptures("nix", src, "?", "operator");
    ExpectCaptures("nix", src, "...", "punctuation.special");
    ExpectCaptures("nix", src, "let", "keyword");
    ExpectCaptures("nix", src, "# a comment", "comment");
    ExpectCaptures("nix", src, "name =", "property");
    // A string's text is @string, but the code in a ${...} is code again.
    ExpectCaptures("nix", src, "\"x-", "string");
    ExpectCaptures("nix", src, "x-", "string");
    ExpectCaptures("nix", src, "${lib", "punctuation.interpolation");
    ExpectCaptures("nix", src, "lib.version", "variable");
    ExpectCaptures("nix", src, "version", "property");
    ExpectCaptures("nix", src, "\\n", "string.escape");
    ExpectCaptures("nix", src, "$HOME", "string");
    // A path, with an interpolation in the middle of it.
    ExpectCaptures("nix", src, "./pkgs", "string.special.path");
    ExpectCaptures("nix", src, "name}.nix", "variable");
    ExpectCaptures("nix", src, ".nix;", "string.special.path");
    ExpectCaptures("nix", src, "mkDerivation", "property function.call");
    ExpectCaptures("nix", src, "rec", "keyword");
    ExpectCaptures("nix", src, "inherit", "keyword");
    ExpectCaptures("nix", src, "name;", "property");
    ExpectCaptures("nix", src, "builtins", "variable variable.builtin");
    ExpectCaptures("nix", src, "fetchurl", "property function.call function.builtin");
    ExpectCaptures("nix", src, "https:", "string.special.uri");
    ExpectCaptures("nix", src, "if", "conditional");
    ExpectCaptures("nix", src, "true", "variable boolean");
    ExpectCaptures("nix", src, "null else", "variable constant.builtin");
    ExpectCaptures("nix", src, "1.5", "float");
    // An indented string: its escapes, and an interpolation inside it.
    ExpectCaptures("nix", src, "''\n", "string");
    ExpectCaptures("nix", src, "cp", "string");
    ExpectCaptures("nix", src, "''${out}", "string.escape");
    ExpectCaptures("nix", src, "getBin", "property function.call");
    ExpectCaptures("nix", src, "map", "variable function.call function.builtin");
    ExpectCaptures("nix", src, "x:", "variable.parameter");
    ExpectCaptures("nix", src, "2 ]", "number");

    // `a/b` is a path in Nix, `a / b` and `a // b` are operators.
    const std::string ops = "[ (a/b) (a / b) (a //b) ]\n";
    ExpectCaptures("nix", ops, "a/b", "string.special.path");
    ExpectCaptures("nix", ops, "/ b", "operator");
    ExpectCaptures("nix", ops, "//b", "operator");

    // The outline: functions and attrsets, nested where they sit; plain
    // values (`name`, `path`, `doCheck`, ...) are left out.
    CHECK(TreesitterHasStructureQuery("nix"));
    const std::string flake =
        "{\n"
        "  inputs.nixpkgs.url = \"github:NixOS/nixpkgs\";\n"
        "  outputs = { self, nixpkgs }: {\n"
        "    packages.x86_64-linux.default = nixpkgs.stdenv.mkDerivation {\n"
        "      name = \"x\";\n"
        "    };\n"
        "    lib = rec {\n"
        "      double = x: x * 2;\n"
        "    };\n"
        "  };\n"
        "}\n";
    ExpectOutline("nix", flake,
                  "0:function:outputs@2\n"
                  "1:attrset:packages.x86_64-linux.default@3\n"
                  "1:attrset:lib@6\n"
                  "2:function:double@7\n");

    // The attrset and the indented string fold; one-line ones do not.
    const std::vector<TSFoldRange> folds = TreesitterFoldRanges("nix", src);
    bool attrset = false;
    bool indented = false;
    for (const TSFoldRange &f : folds) {
        if (f.start_row == 6 && f.end_row == 14) attrset = true;
        if (f.start_row == 10 && f.end_row == 12) indented = true;
    }
    CHECK(attrset);
    CHECK(indented);
}

// ---------------------------------------------------------------------------
// justfiles: mep's own grammar (grammars/tree-sitter-just), compiled in
// ---------------------------------------------------------------------------

void TestJust() {
    CHECK(TreesitterHasGrammar("just"));
    CHECK(TreesitterHasFoldQuery("just"));
    const std::string src =
        "set shell := [\"bash\", \"-cu\"]\n"
        "export BUILD := env(\"OUT\", 'build') / \"x\"\n"
        "alias b := build\n"
        "\n"
        "# Build it.\n"
        "[group('ci')]\n"
        "@build mode='debug' *args: clean (compile mode)\n"
        "    echo {{mode}} {{{{not}} $HOME\n"
        "    -rm -rf `pwd` \\\n"
        "        -x\n"
        "\n"
        "py:\n"
        "    #!/usr/bin/env python3\n"
        "    print({{ if mode == \"x\" { \"a\" } else { `date` } }})\n";
    ExpectCaptures("just", src, "set", "keyword");
    ExpectCaptures("just", src, "shell", "property");
    ExpectCaptures("just", src, ":=", "operator");
    ExpectCaptures("just", src, "\"bash\"", "string");
    ExpectCaptures("just", src, "export", "keyword");
    ExpectCaptures("just", src, "BUILD", "variable");
    ExpectCaptures("just", src, "env(", "function.builtin");
    ExpectCaptures("just", src, "'build'", "string");
    ExpectCaptures("just", src, "/ \"x\"", "operator");
    ExpectCaptures("just", src, "alias", "keyword");
    ExpectCaptures("just", src, "b :=", "function");
    ExpectCaptures("just", src, "build\n", "function.call");
    ExpectCaptures("just", src, "# Build", "comment");
    ExpectCaptures("just", src, "group", "attribute");
    ExpectCaptures("just", src, "@build", "operator");
    ExpectCaptures("just", src, "build mode", "function");
    ExpectCaptures("just", src, "mode=", "variable.parameter");
    ExpectCaptures("just", src, "*args", "operator");
    ExpectCaptures("just", src, "args:", "variable.parameter");
    ExpectCaptures("just", src, "clean", "function.call");
    ExpectCaptures("just", src, "compile", "function.call");
    // A body is the shell's: its text is left alone, an interpolation in it
    // is code, and `{{{{` is a literal `{{`, not an interpolation.
    ExpectCaptures("just", src, "echo", "");
    ExpectCaptures("just", src, "{{mode", "punctuation.interpolation");
    ExpectCaptures("just", src, "mode}}", "variable");
    ExpectCaptures("just", src, "{{{{", "");
    ExpectCaptures("just", src, "-rm", "operator");
    ExpectCaptures("just", src, "rm -rf", "");
    // After a `\` continuation, a leading `-` is text, not a prefix.
    ExpectCaptures("just", src, "-x", "");
    ExpectCaptures("just", src, "#!/usr", "keyword.directive");
    ExpectCaptures("just", src, "if", "conditional");
    ExpectCaptures("just", src, "==", "operator");
    ExpectCaptures("just", src, "`date`", "string.special");

    // The outline: variables, then each recipe, header to last body line.
    CHECK(TreesitterHasStructureQuery("just"));
    ExpectOutline("just", src,
                  "0:variable:BUILD@1\n"
                  "0:recipe:build@6\n"
                  "0:recipe:py@11\n");
    const std::string modules = "mod? tools 'tools.just'\nversion := `git describe`\n";
    ExpectOutline("just", modules, "0:module:tools@0\n0:variable:version@1\n");

    // Each recipe folds, from its header to its last body line -- not over
    // the blank line after it.
    const std::vector<TSFoldRange> folds = TreesitterFoldRanges("just", src);
    bool build = false;
    bool py = false;
    for (const TSFoldRange &f : folds) {
        if (f.start_row == 5 && f.end_row == 9) build = true;
        if (f.start_row == 11 && f.end_row == 13) py = true;
    }
    CHECK(build);
    CHECK(py);
}

// ---------------------------------------------------------------------------
// C: mep's own grammar (grammars/tree-sitter-c), compiled in
// ---------------------------------------------------------------------------

void TestC() {
    CHECK(TreesitterHasFoldQuery("c"));
    CHECK(TreesitterHasStructureQuery("c"));
    const std::string src =
        "#include <stdio.h>\n"
        "#define MAX(a, b) ((a) > (b) ? (a) : (b))\n"
        "typedef struct point { int x; } point_t;\n"
        "enum color { RED, GREEN };\n"
        "static int add(int a, const char *s) {\n"
        "  if (a) return printf(\"%d\\n\", a);\n"
        "#ifdef DEBUG\n"
        "  else if (s) { goto done; }\n"
        "#endif\n"
        "  else { a = sizeof(int) + RED; }\n"
        "done:\n"
        "  return p->x == NULL;\n"
        "}\n";
    ExpectCaptures("c", src, "#include", "include");
    ExpectCaptures("c", src, "<stdio.h>", "string");
    ExpectCaptures("c", src, "#define", "preproc");
    ExpectCaptures("c", src, "MAX", "variable constant function.macro");
    ExpectCaptures("c", src, "typedef", "keyword");
    ExpectCaptures("c", src, "point {", "type");
    ExpectCaptures("c", src, "x;", "property");
    ExpectCaptures("c", src, "RED,", "variable constant constant");
    ExpectCaptures("c", src, "static", "keyword");
    ExpectCaptures("c", src, "int add", "type");
    ExpectCaptures("c", src, "add", "variable function");
    ExpectCaptures("c", src, "a, const", "variable variable.parameter");
    ExpectCaptures("c", src, "s) {", "variable variable.parameter");
    ExpectCaptures("c", src, "if", "conditional");
    ExpectCaptures("c", src, "return printf", "keyword.return");
    ExpectCaptures("c", src, "printf", "variable function.call");
    ExpectCaptures("c", src, "\\n", "string string.escape");
    // An `else` the preprocessor split from its `if` is still an `else`.
    ExpectCaptures("c", src, "else if", "conditional");
    ExpectCaptures("c", src, "goto", "keyword");
    ExpectCaptures("c", src, "done;", "label");
    ExpectCaptures("c", src, "sizeof", "keyword.operator");
    ExpectCaptures("c", src, "->", "operator");
    ExpectCaptures("c", src, "NULL", "constant.builtin");

    ExpectOutline("c", src,
                  "0:struct:point@2\n"
                  "0:enum:color@3\n"
                  "0:function:add@4\n");
    ExpectOutline("c", "char **lines(void) { return 0; }\ntypedef struct { int y; } anon_t;\n",
                  "0:function:lines@0\n0:struct:anon_t@1\n");

    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("c", src)) body = body || (f.start_row == 4 && f.end_row == 12);
    CHECK(body);
}

// ---------------------------------------------------------------------------
// Lua: mep's own grammar (grammars/tree-sitter-lua), compiled in
// ---------------------------------------------------------------------------

void TestLua() {
    CHECK(TreesitterHasFoldQuery("lua"));
    CHECK(TreesitterHasStructureQuery("lua"));
    const std::string src =
        "#!/usr/bin/env lua\n"
        "-- Settings.\n"
        "local M = {}\n"
        "local MAX <const> = 10\n"
        "function M.setup(opts, ...)\n"
        "  local s = [==[long]==] .. \"a\\n\"\n"
        "  for i = 1, MAX do mep.notify(string.format('%d', i)) end\n"
        "  return self:run(nil, true)\n"
        "end\n"
        "M.handlers = {\n"
        "  on_save = function(buf) return buf end,\n"
        "}\n"
        "global.x = 1\n"
        "return M\n";
    ExpectCaptures("lua", src, "#!", "preproc");
    ExpectCaptures("lua", src, "-- Settings", "comment");
    ExpectCaptures("lua", src, "local M", "keyword");
    ExpectCaptures("lua", src, "MAX <", "variable constant");
    ExpectCaptures("lua", src, "const", "variable attribute");
    ExpectCaptures("lua", src, "function M", "keyword.function");
    ExpectCaptures("lua", src, "setup", "variable field function");
    ExpectCaptures("lua", src, "opts", "variable parameter");
    ExpectCaptures("lua", src, "...", "constant");
    ExpectCaptures("lua", src, "[==[", "string");
    ExpectCaptures("lua", src, "\\n\"", "string string.escape");
    ExpectCaptures("lua", src, "for", "repeat");
    ExpectCaptures("lua", src, "mep.", "variable variable.builtin");
    ExpectCaptures("lua", src, "notify", "variable field function.call");
    ExpectCaptures("lua", src, "string.", "variable module");
    ExpectCaptures("lua", src, "self", "variable variable.builtin");
    ExpectCaptures("lua", src, "run", "variable method.call");
    ExpectCaptures("lua", src, "nil", "constant.builtin");
    ExpectCaptures("lua", src, "true", "boolean");
    ExpectCaptures("lua", src, "on_save", "variable field function");
    // Lua 5.4's `global` is a name; 5.5's is a keyword only in declarations.
    ExpectCaptures("lua", src, "global", "variable");
    ExpectCaptures("lua", src, "return M", "keyword.return");

    // `M.handlers = {...}` holds a function but is not one itself.
    ExpectOutline("lua", src,
                  "0:function:M.setup@4\n"
                  "0:method:on_save@10\n");
    ExpectOutline("lua", "local function f() end\nlocal g = function() end\nfunction M:m() end\n",
                  "0:function:f@0\n0:function:g@1\n0:method:M:m@2\n");

    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("lua", src)) body = body || (f.start_row == 4 && f.end_row == 8);
    CHECK(body);
}

// ---------------------------------------------------------------------------
// Common Lisp: mep's own grammar (grammars/tree-sitter-commonlisp), compiled in
// ---------------------------------------------------------------------------

void TestCommonLisp() {
    for (const char *ft : {"lisp", "lsp", "cl", "asd"}) {
        CHECK(TreesitterHasGrammar(ft));
        CHECK(TreesitterHasFoldQuery(ft));
        CHECK(TreesitterHasStructureQuery(ft));
    }
    const std::string src =
        ";;; Geometry\n"
        "(defpackage :geometry (:use :cl))\n"
        "(in-package :geometry)\n"
        "(defparameter *origin* (cons 0 0))\n"
        "(defconstant +unit+ 1)\n"
        "(defstruct point x y)\n"
        "(defclass shape () ((name :initarg :name :accessor shape-name)))\n"
        "(defgeneric area (shape))\n"
        "(defmethod area ((s shape)) 0)\n"
        "(defun distance (a b &optional (scale 1.0d0))\n"
        "  \"Distance from A to B.\"\n"
        "  (let ((dx (- (point-x a) (point-x b)))\n"
        "        (dy (- (point-y a) (point-y b))))\n"
        "    (* scale (sqrt (+ (* dx dx) (* dy dy))))))\n"
        "(defmacro with-origin (&body body) `(let ((o *origin*)) ,@body))\n"
        "(loop for i from 1 to 3 collect (format nil \"~a~%\" i))\n"
        "#+sbcl (sb-ext:gc :full t)\n"
        "(mapcar #'distance '(nil) (list #\\Space))\n";
    ExpectCaptures("lisp", src, ";;; Geometry", "comment");
    ExpectCaptures("lisp", src, "defpackage", "variable function.call keyword");
    ExpectCaptures("lisp", src, ":geometry", "constant");
    ExpectCaptures("lisp", src, "*origin*", "variable variable.builtin");
    ExpectCaptures("lisp", src, "+unit+", "variable constant constant");
    ExpectCaptures("lisp", src, "point x", "variable type");
    ExpectCaptures("lisp", src, "area (shape)", "variable function");
    ExpectCaptures("lisp", src, "area ((s", "variable function.method");
    ExpectCaptures("lisp", src, "s shape))", "variable function.call variable.parameter");
    ExpectCaptures("lisp", src, "defun", "variable function.call keyword.function");
    ExpectCaptures("lisp", src, "distance (a", "variable function");
    ExpectCaptures("lisp", src, "a b &", "variable function.call variable.parameter");
    ExpectCaptures("lisp", src, "&optional", "variable variable.parameter keyword.directive");
    ExpectCaptures("lisp", src, "scale 1", "variable function.call variable.parameter");
    ExpectCaptures("lisp", src, "1.0d0", "number");
    ExpectCaptures("lisp", src, "\"Distance", "string");
    ExpectCaptures("lisp", src, "let ((dx", "variable function.call keyword");
    ExpectCaptures("lisp", src, "dx (-", "variable function.call variable");
    ExpectCaptures("lisp", src, "sqrt", "variable function.call function.builtin");
    ExpectCaptures("lisp", src, "with-origin", "variable function.macro");
    ExpectCaptures("lisp", src, "`(let", "punctuation.special");
    ExpectCaptures("lisp", src, ",@body", "punctuation.special");
    ExpectCaptures("lisp", src, "loop", "variable function.call repeat");
    ExpectCaptures("lisp", src, "for i", "variable keyword");
    ExpectCaptures("lisp", src, "collect", "variable keyword");
    ExpectCaptures("lisp", src, "~a", "string string.special");
    ExpectCaptures("lisp", src, "#+", "preproc");
    ExpectCaptures("lisp", src, "sbcl", "variable preproc");
    ExpectCaptures("lisp", src, "sb-ext:", "module");
    ExpectCaptures("lisp", src, "gc", "variable function.call");
    ExpectCaptures("lisp", src, "#'", "punctuation.special");
    ExpectCaptures("lisp", src, "distance '", "variable function");
    ExpectCaptures("lisp", src, "nil)", "variable function.call constant.builtin");
    ExpectCaptures("lisp", src, "#\\Space", "character");

    ExpectOutline("lisp", src,
                  "0:module::geometry@1\n"
                  "0:variable:*origin*@3\n"
                  "0:variable:+unit+@4\n"
                  "0:struct:point@5\n"
                  "0:class:shape@6\n"
                  "0:function:area@7\n"
                  "0:method:area@8\n"
                  "0:function:distance@9\n"
                  "0:macro:with-origin@14\n");
    ExpectOutline("lisp", "(defstruct (pt (:constructor mk)) x)\n(defun (setf thing) (v o) v)\n",
                  "0:struct:pt@0\n0:function:(setf thing)@1\n");

    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("lisp", src)) body = body || (f.start_row == 9 && f.end_row == 13);
    CHECK(body);
}

// ---------------------------------------------------------------------------
// Bash: mep's own grammar (grammars/tree-sitter-bash), compiled in
// ---------------------------------------------------------------------------

void TestBash() {
    for (const char *ft : {"sh", "bash", "zsh"}) {
        CHECK(TreesitterHasGrammar(ft));
        CHECK(TreesitterHasFoldQuery(ft));
        CHECK(TreesitterHasStructureQuery(ft));
    }
    const std::string src =
        "#!/usr/bin/env bash\n"
        "# Deploy script.\n"
        "readonly VERSION=1.2\n"
        "export PREFIX=/usr\n"
        "count=0\n"
        "greet() {\n"
        "  local name=${1:-world}\n"
        "  echo \"hello $name\" >&2\n"
        "  return 0\n"
        "}\n"
        "function cleanup {\n"
        "  rm -rf \"$tmp\" || true\n"
        "}\n"
        "for f in *.txt; do\n"
        "  if [[ -f $f && $f =~ ^a ]]; then\n"
        "    cat <<EOF\n"
        "body $f\n"
        "EOF\n"
        "  fi\n"
        "done\n"
        "case $1 in\n"
        "  start|run) greet --loud ;;\n"
        "  *) exit 1 ;;\n"
        "esac\n";
    ExpectCaptures("sh", src, "#!/usr", "comment keyword.directive");
    ExpectCaptures("sh", src, "# Deploy", "comment");
    ExpectCaptures("sh", src, "readonly", "keyword");
    ExpectCaptures("sh", src, "VERSION", "variable");
    ExpectCaptures("sh", src, "1.2", "");
    ExpectCaptures("sh", src, "greet()", "function");
    ExpectCaptures("sh", src, "local", "keyword");
    ExpectCaptures("sh", src, "${1", "punctuation.special");
    ExpectCaptures("sh", src, "echo", "function.call function.builtin");
    ExpectCaptures("sh", src, "\"hello", "string");
    ExpectCaptures("sh", src, "$name", "string punctuation.special");
    ExpectCaptures("sh", src, ">&2", "operator");
    ExpectCaptures("sh", src, "2\n  return", "number");
    ExpectCaptures("sh", src, "return 0", "function.call keyword.return");
    ExpectCaptures("sh", src, "function cleanup", "keyword.function");
    ExpectCaptures("sh", src, "cleanup {", "function");
    ExpectCaptures("sh", src, "rm -rf", "function.call");
    ExpectCaptures("sh", src, "-rf", "variable.parameter");
    ExpectCaptures("sh", src, "||", "operator");
    ExpectCaptures("sh", src, "true", "function.call boolean");
    ExpectCaptures("sh", src, "for f", "repeat");
    ExpectCaptures("sh", src, "in *.txt", "repeat");
    ExpectCaptures("sh", src, "if [[", "conditional");
    ExpectCaptures("sh", src, "[[ -f", "punctuation.bracket");
    ExpectCaptures("sh", src, "-f $f", "operator");
    ExpectCaptures("sh", src, "=~", "operator");
    ExpectCaptures("sh", src, "^a", "string.regexp");
    ExpectCaptures("sh", src, "EOF\nbody", "label");
    ExpectCaptures("sh", src, "body $f", "string");
    ExpectCaptures("sh", src, "case $1", "conditional");
    ExpectCaptures("sh", src, "$1 in", "punctuation.special");
    ExpectCaptures("sh", src, "1 in", "variable variable.builtin");
    ExpectCaptures("sh", src, "start|run", "string.regexp");
    ExpectCaptures("sh", src, ";;", "punctuation.delimiter");
    ExpectCaptures("sh", src, "exit 1", "function.call keyword.return");

    // Functions, and the top-level settings a reader looks for: readonly/
    // export declarations and ALL_CAPS assignments, not `count=0`.
    ExpectOutline("sh", src,
                  "0:variable:VERSION@2\n"
                  "0:variable:PREFIX@3\n"
                  "0:function:greet@5\n"
                  "0:function:cleanup@10\n");
    ExpectOutline("sh", "outer() {\n  inner() { :; }\n}\nX=1\n", "0:function:outer@0\n1:function:inner@1\n0:variable:X@3\n");

    bool body = false, loop = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("sh", src)) {
        body = body || (f.start_row == 5 && f.end_row == 9);
        loop = loop || (f.start_row == 13 && f.end_row == 19);
    }
    CHECK(body);
    CHECK(loop);
}

// ---------------------------------------------------------------------------
// Clojure: mep's own grammar (grammars/tree-sitter-clojure), compiled in
// ---------------------------------------------------------------------------

void TestClojure() {
    for (const char *ft : {"clj", "cljs", "cljc", "cljd", "edn", "bb"}) {
        CHECK(TreesitterHasGrammar(ft));
        CHECK(TreesitterHasFoldQuery(ft));
        CHECK(TreesitterHasStructureQuery(ft));
    }
    const std::string src =
        "#!/usr/bin/env bb\n"
        "(ns demo.core\n"
        "  (:require [clojure.string :as str]))\n"
        "(def ^:dynamic *depth* 3)\n"
        "(defrecord Point [x y])\n"
        "(defn- greet\n"
        "  \"Says hello.\"\n"
        "  [name & more]\n"
        "  (when-not (str/blank? name)\n"
        "    (println (format \"hi %s\" name) \\a #\"\\d+\" nil true 1.5)))\n"
        "(defmacro unless [c & body] `(if ~c nil (do ~@body)))\n"
        "#_(ignored form)\n"
        "(map #(* % %) (range 10))\n"
        "(.toUpperCase s) (String. \"x\") (Math/sqrt 2.0)\n";
    ExpectCaptures("clj", src, "#!/usr", "comment");
    ExpectCaptures("clj", src, "ns demo", "variable function.call keyword");
    ExpectCaptures("clj", src, "demo.core", "variable module");
    ExpectCaptures("clj", src, ":require", "constant");
    ExpectCaptures("clj", src, "clojure.string", "variable");
    ExpectCaptures("clj", src, "^:dynamic", "variable punctuation.special");
    ExpectCaptures("clj", src, ":dynamic", "variable constant attribute");
    ExpectCaptures("clj", src, "*depth*", "variable variable.builtin variable");
    ExpectCaptures("clj", src, "Point", "variable type");
    ExpectCaptures("clj", src, "greet", "variable function");
    ExpectCaptures("clj", src, "\"Says", "string string.documentation");
    ExpectCaptures("clj", src, "name &", "variable variable.parameter");
    ExpectCaptures("clj", src, "& more", "variable variable.parameter operator");
    ExpectCaptures("clj", src, "when-not", "variable function.call conditional");
    ExpectCaptures("clj", src, "str/blank?", "variable module");
    ExpectCaptures("clj", src, "blank?", "variable function.call");
    ExpectCaptures("clj", src, "println", "variable function.call function.builtin");
    ExpectCaptures("clj", src, "\\a ", "character");
    ExpectCaptures("clj", src, "#\"\\d", "string.regexp punctuation.special");
    ExpectCaptures("clj", src, "nil true", "constant.builtin");
    ExpectCaptures("clj", src, "true 1.5", "boolean");
    ExpectCaptures("clj", src, "1.5)", "number");
    ExpectCaptures("clj", src, "unless", "variable function.macro");
    ExpectCaptures("clj", src, "`(if", "punctuation.special");
    ExpectCaptures("clj", src, "~@body", "punctuation.special");
    ExpectCaptures("clj", src, "#_(ignored", "comment");
    ExpectCaptures("clj", src, "% %", "variable variable.parameter");
    ExpectCaptures("clj", src, "range", "variable function.call function.builtin");
    ExpectCaptures("clj", src, ".toUpperCase", "variable function.call function.method.call");
    ExpectCaptures("clj", src, "String.", "variable function.call constructor");
    ExpectCaptures("clj", src, "Math/", "variable module type");

    ExpectOutline("clj", src,
                  "0:module:demo.core@1\n"
                  "0:variable:*depth*@3\n"
                  "0:struct:Point@4\n"
                  "0:function:greet@5\n"
                  "0:macro:unless@10\n");

    bool defn = false, ns = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("clj", src)) {
        defn = defn || (f.start_row == 5 && f.end_row == 9);
        ns = ns || (f.start_row == 1 && f.end_row == 2);
    }
    CHECK(defn);
    CHECK(ns);
}

// ---------------------------------------------------------------------------
// Scheme: mep's own grammar (grammars/tree-sitter-scheme), compiled in
// ---------------------------------------------------------------------------

void TestScheme() {
    for (const char *ft : {"scm", "ss", "sld", "sls", "sps"}) {
        CHECK(TreesitterHasGrammar(ft));
        CHECK(TreesitterHasFoldQuery(ft));
        CHECK(TreesitterHasStructureQuery(ft));
    }
    const std::string src =
        "#!/usr/bin/guile -s\n"
        "!#\n"
        "(define-module (demo core)\n"
        "  #:use-module (srfi srfi-1))\n"
        "(define depth 3)\n"
        "(define (square x)\n"
        "  ;; multiply\n"
        "  (* x x))\n"
        "(define-syntax swap!\n"
        "  (syntax-rules () ((_ a b) (let ((t a)) (set! a b) (set! b t)))))\n"
        "(define-record-type point (make-point x y) point? (x point-x))\n"
        "#;(ignored form) (display \"hi\\n\" #\\a #t 1.5)\n"
        "#~(mkdir #$output) '(a b) #u8(1 2) #| block |#\n";
    ExpectCaptures("scm", src, "#!/usr", "comment");
    ExpectCaptures("scm", src, "define-module", "variable function keyword");
    ExpectCaptures("scm", src, "#:use-module", "constant");
    ExpectCaptures("scm", src, "square", "variable function");
    ExpectCaptures("scm", src, "* x", "variable function operator function.builtin");
    ExpectCaptures("scm", src, ";; multiply", "comment");
    ExpectCaptures("scm", src, "set!", "variable function keyword");
    ExpectCaptures("scm", src, "#;(ignored", "comment comment");
    ExpectCaptures("scm", src, "ignored", "comment comment variable function comment");
    ExpectCaptures("scm", src, "display", "variable function function.builtin");
    ExpectCaptures("scm", src, "\"hi", "string");
    ExpectCaptures("scm", src, "\\n", "string escape");
    ExpectCaptures("scm", src, "#\\a", "constant.builtin");
    ExpectCaptures("scm", src, "#t", "constant.builtin");
    ExpectCaptures("scm", src, "1.5", "number");
    ExpectCaptures("scm", src, "#~", "punctuation.special");
    ExpectCaptures("scm", src, "#$", "punctuation.special");
    ExpectCaptures("scm", src, "a b) #u8", "constant variable function constant");
    ExpectCaptures("scm", src, "#u8(", "punctuation.bracket");
    ExpectCaptures("scm", src, "#| block", "comment");

    ExpectOutline("scm", src,
                  "0:module:(demo core)@2\n"
                  "0:variable:depth@4\n"
                  "0:function:square@5\n"
                  "0:macro:swap!@8\n"
                  "0:struct:point@10\n");

    bool square = false, module = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("scm", src)) {
        square = square || (f.start_row == 5 && f.end_row == 7);
        module = module || (f.start_row == 2 && f.end_row == 3);
    }
    CHECK(square);
    CHECK(module);
}

// ---------------------------------------------------------------------------
// Crystal: mep's own grammar (grammars/tree-sitter-crystal), compiled in
// ---------------------------------------------------------------------------

void TestCrystal() {
    CHECK(TreesitterHasGrammar("cr"));
    CHECK(TreesitterHasFoldQuery("cr"));
    CHECK(TreesitterHasStructureQuery("cr"));
    const std::string src =
        "require \"json\"\n"
        "\n"
        "# A shape.\n"
        "module Geometry\n"
        "  VERSION = \"1.0\"\n"
        "\n"
        "  @[Deprecated]\n"
        "  abstract class Shape(T) < Base\n"
        "    getter name : String\n"
        "\n"
        "    def area(scale : Float64 = 1.0) : Float64\n"
        "      x = @radius * 2 if scale > 0\n"
        "      puts \"r=#{x} \\n\", :sym, 'c', /a+b/i, nil, true, 0x1F_u8\n"
        "      [1, 2].map(&.to_s).each do |s|\n"
        "        yield s\n"
        "      end\n"
        "      %w(a b).size.as(Int32)\n"
        "    end\n"
        "\n"
        "    macro emit(name)\n"
        "      def {{name.id}}; end\n"
        "    end\n"
        "  end\n"
        "\n"
        "  enum Color\n"
        "    Red\n"
        "    Green\n"
        "  end\n"
        "end\n"
        "\n"
        "lib LibC\n"
        "  fun strlen(s : UInt8*) : Int32\n"
        "end\n";
    ExpectCaptures("cr", src, "require", "keyword.import");
    ExpectCaptures("cr", src, "\"json\"", "string");
    ExpectCaptures("cr", src, "# A shape", "comment");
    ExpectCaptures("cr", src, "module", "keyword.type");
    ExpectCaptures("cr", src, "Geometry", "type type.definition");
    ExpectCaptures("cr", src, "VERSION", "type constant");
    ExpectCaptures("cr", src, "@[", "attribute");
    ExpectCaptures("cr", src, "Deprecated", "type attribute");
    ExpectCaptures("cr", src, "abstract", "keyword.modifier");
    ExpectCaptures("cr", src, "Shape", "type type type.definition");
    // A generic's parameter is a type, though it looks like an ALL_CAPS value.
    ExpectCaptures("cr", src, "T)", "type type");
    ExpectCaptures("cr", src, "Base", "type");
    ExpectCaptures("cr", src, "getter", "variable function.call function.builtin");
    ExpectCaptures("cr", src, "String", "type type.builtin");
    ExpectCaptures("cr", src, "def area", "keyword.function");
    ExpectCaptures("cr", src, "area", "variable function.method");
    ExpectCaptures("cr", src, "scale :", "variable variable.parameter");
    ExpectCaptures("cr", src, "1.0)", "number.float");
    ExpectCaptures("cr", src, "@radius", "variable.member");
    ExpectCaptures("cr", src, "if scale", "keyword.conditional");
    ExpectCaptures("cr", src, "scale > 0", "variable");
    ExpectCaptures("cr", src, "puts", "variable function.call function.builtin");
    ExpectCaptures("cr", src, "\"r=", "string");
    ExpectCaptures("cr", src, "#{x}", "string punctuation.special");
    ExpectCaptures("cr", src, "\\n", "string string.escape");
    ExpectCaptures("cr", src, ":sym", "string.special.symbol");
    ExpectCaptures("cr", src, "'c'", "character");
    ExpectCaptures("cr", src, "/a+b/", "string.regexp");
    ExpectCaptures("cr", src, "i, nil", "string.regexp character.special");
    ExpectCaptures("cr", src, "nil", "constant.builtin");
    ExpectCaptures("cr", src, "true", "boolean");
    ExpectCaptures("cr", src, "0x1F_u8", "number");
    ExpectCaptures("cr", src, "map", "variable function.call");
    ExpectCaptures("cr", src, ".to_s", "variable function.call");
    ExpectCaptures("cr", src, "do |", "keyword");
    ExpectCaptures("cr", src, "|s|", "punctuation.bracket");
    ExpectCaptures("cr", src, "s|", "variable variable.parameter");
    ExpectCaptures("cr", src, "yield", "keyword.return");
    // %w(...)'s delimiters are the literal's, not brackets.
    ExpectCaptures("cr", src, "%w(", "punctuation.bracket string");
    ExpectCaptures("cr", src, "a b)", "string");
    ExpectCaptures("cr", src, ").size", "punctuation.bracket string");
    ExpectCaptures("cr", src, "as(", "variable function.call keyword.operator");
    ExpectCaptures("cr", src, "Int32)\n", "type type.builtin");
    ExpectCaptures("cr", src, "macro", "keyword.function");
    ExpectCaptures("cr", src, "emit", "variable function.macro");
    ExpectCaptures("cr", src, "{{", "punctuation.special");
    ExpectCaptures("cr", src, "enum", "keyword.type");
    ExpectCaptures("cr", src, "Red", "type constant");
    ExpectCaptures("cr", src, "lib", "keyword.type");
    ExpectCaptures("cr", src, "LibC", "type module");
    ExpectCaptures("cr", src, "fun", "keyword.function");
    ExpectCaptures("cr", src, "strlen", "variable function");
    ExpectCaptures("cr", src, "*)", "operator type.builtin");

    // Types nest their methods, macros, constants and enum members.
    ExpectOutline("cr", src,
                  "0:module:Geometry@3\n"
                  "1:variable:VERSION@4\n"
                  "1:class:Shape@7\n"
                  "2:method:area@10\n"
                  "2:macro:emit@19\n"
                  "1:enum:Color@24\n"
                  "2:variable:Red@25\n"
                  "2:variable:Green@26\n"
                  "0:namespace:LibC@30\n"
                  "1:function:strlen@31\n");
    ExpectOutline("cr", "struct P\n  x : Int32\n  private def f; end\n  def self.g : Int32\n    1\n  end\nend\nalias Cb = Int32 -> Nil\n",
                  "0:struct:P@0\n1:method:f@2\n1:method:g@3\n0:type:Cb@7\n");

    bool klass = false, method = false, block = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("cr", src)) {
        klass = klass || (f.start_row == 7 && f.end_row == 22);
        method = method || (f.start_row == 10 && f.end_row == 17);
        block = block || (f.start_row == 13 && f.end_row == 15);
    }
    CHECK(klass);
    CHECK(method);
    CHECK(block);
}

// ---------------------------------------------------------------------------
// C#: mep's own grammar (grammars/tree-sitter-c_sharp), compiled in
// ---------------------------------------------------------------------------

void TestCSharp() {
    CHECK(TreesitterHasGrammar("cs"));
    CHECK(TreesitterHasFoldQuery("cs"));
    CHECK(TreesitterHasStructureQuery("cs"));
    const std::string src =
        "using System;\n"
        "\n"
        "namespace Demo\n"
        "{\n"
        "    /// A shape.\n"
        "    [Serializable]\n"
        "    public sealed class Shape<T> : Base where T : struct\n"
        "    {\n"
        "        public const int Max = 10;\n"
        "        public string Name { get; init; } = \"x\";\n"
        "\n"
        "        public Shape(int count) { }\n"
        "\n"
        "        public async Task<int> RunAsync(int n)\n"
        "        {\n"
        "            var list = new List<int> { 1, 2 };\n"
        "            foreach (var item in list) Console.WriteLine($\"{item:N2} \\n {{x}}\");\n"
        "            if (n is > 0 and < 9 or [.., 1]) return await Task.FromResult(n);\n"
        "            return nameof(n).Length + 'c' + 0x1F;\n"
        "        }\n"
        "    }\n"
        "\n"
        "    public enum Color { Red, Green }\n"
        "    public record Point(int X, int Y);\n"
        "}\n"
        "#if DEBUG\n"
        "#endif\n";
    ExpectCaptures("cs", src, "using", "include");
    ExpectCaptures("cs", src, "System", "variable module");
    ExpectCaptures("cs", src, "namespace", "keyword");
    ExpectCaptures("cs", src, "Demo", "variable module");
    ExpectCaptures("cs", src, "/// A shape.", "comment");
    ExpectCaptures("cs", src, "Serializable", "variable attribute");
    ExpectCaptures("cs", src, "public", "keyword.modifier");
    ExpectCaptures("cs", src, "class", "keyword");
    ExpectCaptures("cs", src, "Shape", "variable type.definition");
    ExpectCaptures("cs", src, "T>", "variable type.definition");
    ExpectCaptures("cs", src, "Base", "variable type");
    ExpectCaptures("cs", src, "where", "keyword");
    ExpectCaptures("cs", src, "int Max", "type.builtin");
    ExpectCaptures("cs", src, "Max", "variable variable.member");
    ExpectCaptures("cs", src, "Name", "variable property");
    ExpectCaptures("cs", src, "get", "keyword");
    ExpectCaptures("cs", src, "\"x\"", "string");
    ExpectCaptures("cs", src, "Shape(int", "variable constructor");
    ExpectCaptures("cs", src, "count", "variable variable.parameter");
    ExpectCaptures("cs", src, "async", "keyword keyword.modifier");
    ExpectCaptures("cs", src, "Task<int>", "variable type");
    ExpectCaptures("cs", src, "RunAsync", "variable function.method");
    ExpectCaptures("cs", src, "var", "keyword");
    ExpectCaptures("cs", src, "new", "keyword.operator");
    ExpectCaptures("cs", src, "List", "variable type");
    ExpectCaptures("cs", src, "foreach", "repeat");
    ExpectCaptures("cs", src, "WriteLine", "variable variable.member function.method.call");
    ExpectCaptures("cs", src, "$\"", "string string");
    ExpectCaptures("cs", src, "{item", "string punctuation.special");
    ExpectCaptures("cs", src, ":N2", "string string.special");
    ExpectCaptures("cs", src, "\\n", "string string.escape");
    ExpectCaptures("cs", src, "{{x}}", "string");
    ExpectCaptures("cs", src, "if", "conditional");
    ExpectCaptures("cs", src, "is >", "keyword.operator");
    ExpectCaptures("cs", src, "and", "keyword.operator");
    ExpectCaptures("cs", src, "..", "operator");
    ExpectCaptures("cs", src, "return await", "keyword.return");
    ExpectCaptures("cs", src, "await", "keyword.operator");
    ExpectCaptures("cs", src, "FromResult", "variable variable.member function.method.call");
    ExpectCaptures("cs", src, "nameof", "variable function.call keyword.operator");
    ExpectCaptures("cs", src, "'c'", "character");
    ExpectCaptures("cs", src, "0x1F", "number");
    ExpectCaptures("cs", src, "Red", "variable constant");
    ExpectCaptures("cs", src, "#if", "keyword.directive");
    ExpectCaptures("cs", src, "DEBUG", "variable constant.macro");

    // Namespaces nest their types, types their members.
    ExpectOutline("cs", src,
                  "0:namespace:Demo@2\n"
                  "1:class:Shape@6\n"
                  "2:variable:Max@8\n"
                  "2:variable:Name@9\n"
                  "2:method:Shape@11\n"
                  "2:method:RunAsync@13\n"
                  "1:enum:Color@22\n"
                  "2:variable:Red@22\n"
                  "2:variable:Green@22\n"
                  "1:class:Point@23\n");
    ExpectOutline("cs", "interface I { void M(); int P { get; } }\nstruct S { static S operator +(S a, S b) => a; }\n",
                  "0:interface:I@0\n1:method:M@0\n1:variable:P@0\n0:struct:S@1\n1:method:+@1\n");

    bool klass = false, method = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("cs", src)) {
        klass = klass || (f.start_row == 5 && f.end_row == 20);
        method = method || (f.start_row == 13 && f.end_row == 19);
    }
    CHECK(klass);
    CHECK(method);
}

// ---------------------------------------------------------------------------
// SQL: mep's own grammar (grammars/tree-sitter-sql), compiled in
// ---------------------------------------------------------------------------

void TestSQL() {
    CHECK(TreesitterHasGrammar("sql"));
    CHECK(TreesitterHasFoldQuery("sql"));
    CHECK(TreesitterHasStructureQuery("sql"));
    const std::string src =
        ".mode column\n"
        "\\set ON_ERROR_STOP on\n"
        "-- Tables.\n"
        "CREATE TABLE IF NOT EXISTS shop.fruit (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    name TEXT NOT NULL\n"
        ");\n"
        "CREATE VIEW red AS SELECT name FROM fruit WHERE color = 'red';\n"
        "CREATE INDEX fruit_name ON fruit (name);\n"
        "CREATE FUNCTION add(a integer, b integer) RETURNS integer\n"
        "LANGUAGE 'sql' AS $$ SELECT a + b $$;\n"
        "WITH cheap AS (\n"
        "    SELECT id, price FROM sale WHERE price < 1.5\n"
        ")\n"
        "SELECT f.name, count(*) AS n /* how many */\n"
        "FROM fruit AS f JOIN cheap AS c ON c.id = f.id\n"
        "ORDER BY f.name COLLATE \"C\" DESC;;\n"
        "EXPLAIN (COSTS OFF) SELECT 1;\n"
        "GRANT SELECT, USAGE ON TABLE fruit TO PUBLIC WITH GRANT OPTION;\n"
        "DO $body$ BEGIN RAISE NOTICE 'hi'; END $body$;\n";
    ExpectCaptures("sql", src, ".mode", "keyword.directive");
    ExpectCaptures("sql", src, "\\set", "keyword.directive");
    ExpectCaptures("sql", src, "-- Tables", "comment");
    ExpectCaptures("sql", src, "CREATE", "keyword");
    ExpectCaptures("sql", src, "fruit (", "type");
    ExpectCaptures("sql", src, "INTEGER", "type.builtin");
    ExpectCaptures("sql", src, "PRIMARY", "keyword");
    ExpectCaptures("sql", src, "'red'", "string");
    ExpectCaptures("sql", src, "'sql'", "");  // a name, like an unquoted `LANGUAGE sql`
    ExpectCaptures("sql", src, "count(", "type function.call");
    ExpectCaptures("sql", src, "1.5", "string float");
    ExpectCaptures("sql", src, "1;", "string number");
    ExpectCaptures("sql", src, "/* how", "comment");
    ExpectCaptures("sql", src, "AS f", "keyword");
    ExpectCaptures("sql", src, "f JOIN", "variable");
    ExpectCaptures("sql", src, "name COLLATE", "field");
    ExpectCaptures("sql", src, "COLLATE", "attribute");
    ExpectCaptures("sql", src, "COSTS", "keyword");
    ExpectCaptures("sql", src, "OFF", "keyword");
    ExpectCaptures("sql", src, "GRANT SELECT", "keyword");
    ExpectCaptures("sql", src, "USAGE", "keyword");
    ExpectCaptures("sql", src, "PUBLIC", "");
    ExpectCaptures("sql", src, "$body$ BEGIN", "string");

    ExpectOutline("sql", src,
                  "0:struct:shop.fruit@3\n"
                  "0:class:red@7\n"
                  "0:variable:fruit_name@8\n"
                  "0:function:add@9\n"
                  "0:variable:cheap@11\n");

    bool table = false, cte = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("sql", src)) {
        table = table || (f.start_row == 3 && f.end_row == 6);
        cte = cte || (f.start_row == 11 && f.end_row == 13);
    }
    CHECK(table);
    CHECK(cte);
}

// ---------------------------------------------------------------------------
// JavaScript: mep's own grammar (grammars/tree-sitter-javascript), compiled in
// ---------------------------------------------------------------------------

void TestJavascript() {
    CHECK(TreesitterHasFoldQuery("js"));
    CHECK(TreesitterHasStructureQuery("js"));
    const std::string src =
        "#!/usr/bin/env node\n"
        "import fs, { readFile as rf } from 'node:fs';\n"
        "export const MAX_SIZE = 10;\n"
        "// A comment.\n"
        "export class Point extends Base {\n"
        "  #x = 0;\n"
        "  constructor(x, { y } = {}, ...rest) {\n"
        "    super(x);\n"
        "  }\n"
        "  get norm() { return Math.hypot(this.#x, 1); }\n"
        "  onClick = () => {};\n"
        "}\n"
        "async function* gen(a) {\n"
        "  for (const k of a) if (k in a) yield await k;\n"
        "  try { throw new Error(`bad ${a}\\n`); } catch (e) { console.log(e?.message ?? /re/g); }\n"
        "}\n"
        "const area = (r) => r * r\n"
        "const el = <Panel title=\"t\"><b>hi &amp; {MAX_SIZE}</b></Panel>;\n"
        "outer: while (true) break outer;\n";
    ExpectCaptures("js", src, "#!", "preproc");
    ExpectCaptures("js", src, "import", "include");
    ExpectCaptures("js", src, "readFile", "variable");
    ExpectCaptures("js", src, "as rf", "include");
    ExpectCaptures("js", src, "'node:fs'", "string");
    ExpectCaptures("js", src, "MAX_SIZE =", "variable constructor constant");
    ExpectCaptures("js", src, "// A", "comment");
    ExpectCaptures("js", src, "class", "keyword");
    ExpectCaptures("js", src, "Point", "variable constructor");
    ExpectCaptures("js", src, "#x =", "property");
    ExpectCaptures("js", src, "constructor", "property function.method constructor");
    ExpectCaptures("js", src, "x, {", "variable variable.parameter");
    ExpectCaptures("js", src, "y }", "variable.parameter");
    ExpectCaptures("js", src, "rest", "variable variable.parameter");
    ExpectCaptures("js", src, "super", "variable.builtin");
    ExpectCaptures("js", src, "get", "keyword");
    ExpectCaptures("js", src, "norm", "property function.method");
    ExpectCaptures("js", src, "return", "keyword.return");
    ExpectCaptures("js", src, "Math", "variable constructor module");
    ExpectCaptures("js", src, "hypot", "property method.call");
    ExpectCaptures("js", src, "this", "variable.builtin");
    ExpectCaptures("js", src, "async", "keyword.function");
    ExpectCaptures("js", src, "gen", "variable function");
    ExpectCaptures("js", src, "for", "repeat");
    ExpectCaptures("js", src, "of a", "repeat");
    ExpectCaptures("js", src, "in a", "keyword.operator");
    ExpectCaptures("js", src, "yield", "keyword");
    ExpectCaptures("js", src, "try", "exception");
    ExpectCaptures("js", src, "new", "keyword.operator");
    ExpectCaptures("js", src, "Error", "variable constructor constructor");
    ExpectCaptures("js", src, "`bad", "string");
    ExpectCaptures("js", src, "${", "string punctuation.interpolation");
    ExpectCaptures("js", src, "\\n`", "string string.escape");
    ExpectCaptures("js", src, "e) {", "variable variable.parameter");
    ExpectCaptures("js", src, "console", "variable variable.builtin");
    ExpectCaptures("js", src, "?.", "punctuation.delimiter");
    ExpectCaptures("js", src, "??", "operator");
    ExpectCaptures("js", src, "/re/", "string.special operator string.special");
    ExpectCaptures("js", src, "area", "variable function");
    ExpectCaptures("js", src, "r) =>", "variable variable.parameter");
    ExpectCaptures("js", src, "=>", "operator");
    // JSX: a component is a constructor, an HTML element a tag.
    ExpectCaptures("js", src, "Panel title", "variable constructor tag constructor");
    ExpectCaptures("js", src, "title", "property attribute");
    ExpectCaptures("js", src, "b>hi", "variable tag");
    ExpectCaptures("js", src, "&amp;", "string.escape");
    ExpectCaptures("js", src, "{MAX_SIZE}", "punctuation.bracket punctuation.interpolation");
    ExpectCaptures("js", src, "true", "boolean");
    ExpectCaptures("js", src, "outer:", "label");

    ExpectOutline("js", src,
                  "0:variable:MAX_SIZE@2\n"
                  "0:class:Point@4\n"
                  "1:method:constructor@6\n"
                  "1:method:norm@9\n"
                  "1:method:onClick@10\n"
                  "0:function:gen@12\n"
                  "0:function:area@16\n");
    ExpectOutline("js", "module.exports.f = function () {};\nconst C = class {};\nconst o = { g() {}, h: () => 1 };\n",
                  "0:function:f@0\n0:class:C@1\n0:method:g@2\n0:method:h@2\n");

    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("js", src)) body = body || (f.start_row == 4 && f.end_row == 11);
    CHECK(body);
}

// ---------------------------------------------------------------------------
// TypeScript and TSX: mep's own grammars (grammars/tree-sitter-typescript,
// grammars/tree-sitter-tsx), compiled in, highlighted with TypeScript's query
// layered over JavaScript's
// ---------------------------------------------------------------------------

void TestTypescript() {
    for (const char *ft : {"ts", "mts", "cts", "tsx"}) {
        CHECK(TreesitterHasGrammar(ft));
        CHECK(TreesitterHasFoldQuery(ft));
        CHECK(TreesitterHasStructureQuery(ft));
    }
    const std::string src =
        "import type { Opts } from './opts';\n"
        "export interface Shape<in out T extends object = {}> {\n"
        "  readonly name: string;\n"
        "  area?(scale: number): number;\n"
        "}\n"
        "type Pair = [symbol: Symbol, count?: number];\n"
        "enum Color { Red, Green = 2 }\n"
        "namespace Geo.Util { export const ORIGIN = 0; }\n"
        "export abstract class Base<T> implements Shape<T> {\n"
        "  private readonly id: number = 1;\n"
        "  constructor(public label: string, opt?: Opts) { super(); }\n"
        "  abstract area(): number;\n"
        "  handle = (e: Event): void => {};\n"
        "}\n"
        "function over(x: string): string;\n"
        "function over(x: any) { return x as unknown satisfies string; }\n"
        "const el = document.querySelector<HTMLElement>('#a')!;\n"
        "const Box = styled.div<{ on?: boolean }>`color: red;`;\n"
        "pin?.delete();\n";
    ExpectCaptures("ts", src, "import", "include");
    ExpectCaptures("ts", src, "type {", "keyword");
    ExpectCaptures("ts", src, "Opts }", "variable constructor type");
    ExpectCaptures("ts", src, "'./opts'", "string");
    ExpectCaptures("ts", src, "interface", "keyword");
    ExpectCaptures("ts", src, "Shape<in", "type");
    ExpectCaptures("ts", src, "T extends", "type");
    ExpectCaptures("ts", src, "readonly", "keyword");
    ExpectCaptures("ts", src, "string;", "type.builtin");
    ExpectCaptures("ts", src, "scale", "variable variable.parameter");
    ExpectCaptures("ts", src, "<HTMLElement>", "operator punctuation.bracket");
    ExpectCaptures("ts", src, "enum", "keyword");
    ExpectCaptures("ts", src, "namespace", "keyword");
    ExpectCaptures("ts", src, "abstract class", "keyword");
    ExpectCaptures("ts", src, "private", "keyword");
    ExpectCaptures("ts", src, "label", "variable variable.parameter");
    ExpectCaptures("ts", src, "satisfies", "keyword");
    ExpectCaptures("ts", src, "'#a'", "string");
    ExpectCaptures("ts", src, "`color", "string");
    ExpectCaptures("ts", src, "delete()", "property method.call");

    ExpectOutline("ts", src,
                  "0:interface:Shape@1\n"
                  "1:method:area@3\n"
                  "0:type:Pair@5\n"
                  "0:enum:Color@6\n"
                  "0:namespace:Geo.Util@7\n"
                  "0:class:Base@8\n"
                  "1:method:constructor@10\n"
                  "1:method:area@11\n"
                  "1:method:handle@12\n"
                  "0:function:over@14\n"
                  "0:function:over@15\n");

    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("ts", src)) body = body || (f.start_row == 8 && f.end_row == 13);
    CHECK(body);

    // TSX: JavaScript's JSX patterns, TypeScript's on top; `<T>x` is JSX here.
    const std::string tsx =
        "export const App = ({ title }: Props): JSX.Element => (\n"
        "  <Panel<Item> title={title as string} class=\"x\">\n"
        "    <b>hi</b>\n"
        "  </Panel>\n"
        ");\n";
    ExpectCaptures("tsx", tsx, "Panel<", "variable constructor tag constructor type");
    ExpectCaptures("tsx", tsx, "Item>", "type");
    ExpectCaptures("tsx", tsx, "title={", "property attribute");
    ExpectCaptures("tsx", tsx, "class=", "property attribute");
    ExpectCaptures("tsx", tsx, "b>hi", "variable tag");
    ExpectCaptures("tsx", tsx, "string}", "type.builtin");
    ExpectOutline("tsx", tsx, "0:function:App@0\n");
    bool element = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("tsx", tsx)) element = element || (f.start_row == 1 && f.end_row == 3);
    CHECK(element);
}

// ---------------------------------------------------------------------------
// C++: mep's own grammar (grammars/tree-sitter-cpp), compiled in
// ---------------------------------------------------------------------------

void TestCpp() {
    CHECK(TreesitterHasFoldQuery("cpp"));
    CHECK(TreesitterHasStructureQuery("cpp"));
    const std::string src =
        "#include <vector>\n"
        "namespace geo {\n"
        "template <typename T>\n"
        "class Point : public Base {\n"
        " public:\n"
        "  explicit Point(T x) : x_(x) {}\n"
        "  virtual ~Point() = default;\n"
        "  T norm() const noexcept override { return std::sqrt(x_ * x_); }\n"
        "  bool operator==(const Point &o) const;\n"
        " private:\n"
        "  T x_ = 0;\n"
        "};\n"
        "using Pt = Point<double>;\n"
        "auto f(std::vector<int> &v, int n = 2) -> int {\n"
        "  for (auto &[k, w] : v) if constexpr (true) co_return;\n"
        "  try { throw std::runtime_error(R\"x(raw \"s\")x\"); } catch (...) {}\n"
        "  auto g = [&, n](int a) mutable { return a <=> n; };\n"
        "  return p->*m + v.size<int>() + 12_km + nullptr;\n"
        "}\n"
        "}  // namespace geo\n";
    ExpectCaptures("cpp", src, "#include", "include");
    ExpectCaptures("cpp", src, "namespace geo", "keyword");
    ExpectCaptures("cpp", src, "geo {", "module");
    ExpectCaptures("cpp", src, "template", "keyword");
    ExpectCaptures("cpp", src, "<typename", "operator punctuation.bracket");
    ExpectCaptures("cpp", src, "T>", "type type.definition");
    ExpectCaptures("cpp", src, "class", "keyword");
    ExpectCaptures("cpp", src, "Point :", "type");
    ExpectCaptures("cpp", src, "public Base", "keyword");
    ExpectCaptures("cpp", src, "explicit", "keyword");
    ExpectCaptures("cpp", src, "Point(T", "variable function constructor");
    ExpectCaptures("cpp", src, "x_(x)", "property");
    ExpectCaptures("cpp", src, "virtual", "keyword");
    ExpectCaptures("cpp", src, "Point() =", "variable constructor");
    ExpectCaptures("cpp", src, "default", "conditional keyword");
    ExpectCaptures("cpp", src, "norm", "property function.method");
    ExpectCaptures("cpp", src, "noexcept", "keyword.operator");
    ExpectCaptures("cpp", src, "override", "keyword");
    ExpectCaptures("cpp", src, "std::sqrt", "module");
    ExpectCaptures("cpp", src, "sqrt", "variable function.call");
    ExpectCaptures("cpp", src, "operator==", "function keyword");
    ExpectCaptures("cpp", src, "o)", "variable variable.parameter");
    ExpectCaptures("cpp", src, "using", "keyword");
    ExpectCaptures("cpp", src, "auto f", "type");
    ExpectCaptures("cpp", src, "f(std", "variable function");
    ExpectCaptures("cpp", src, "v, int", "variable variable.parameter");
    ExpectCaptures("cpp", src, "n = 2", "variable variable.parameter");
    ExpectCaptures("cpp", src, "for", "repeat");
    ExpectCaptures("cpp", src, "if constexpr", "conditional");
    ExpectCaptures("cpp", src, "true", "boolean");
    ExpectCaptures("cpp", src, "co_return", "keyword.return");
    ExpectCaptures("cpp", src, "try", "exception");
    ExpectCaptures("cpp", src, "throw", "exception");
    ExpectCaptures("cpp", src, "R\"x(", "string");
    ExpectCaptures("cpp", src, "x(raw", "string string.special");
    ExpectCaptures("cpp", src, "catch", "exception");
    ExpectCaptures("cpp", src, "mutable", "keyword");
    ExpectCaptures("cpp", src, "<=>", "operator");
    ExpectCaptures("cpp", src, "->*", "operator");
    ExpectCaptures("cpp", src, "size", "property function.method.call");
    ExpectCaptures("cpp", src, "_km", "type");
    ExpectCaptures("cpp", src, "nullptr", "constant.builtin");

    ExpectOutline("cpp", src,
                  "0:namespace:geo@1\n"
                  "1:class:Point@3\n"
                  "2:method:Point@5\n"
                  "2:method:~Point@6\n"
                  "2:method:norm@7\n"
                  "2:method:operator==@8\n"
                  "1:type:Pt@12\n"
                  "1:function:f@13\n");
    ExpectOutline("cpp", "struct S { int g(); };\nint S::g() { return 0; }\nint &h() { static int i; return i; }\n",
                  "0:struct:S@0\n1:method:g@0\n0:method:S::g@1\n0:function:h@2\n");

    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("cpp", src)) body = body || (f.start_row == 3 && f.end_row == 11);
    CHECK(body);
}

// ---------------------------------------------------------------------------
// Org: mep's own grammar (grammars/tree-sitter-org), compiled in
// ---------------------------------------------------------------------------

void TestOrg() {
    CHECK(TreesitterHasFoldQuery("org"));
    CHECK(TreesitterHasStructureQuery("org"));
    const std::string src =
        "#+TITLE: Notes\n"
        "* TODO [#A] Ship it :work:\n"
        "SCHEDULED: <2024-01-03 Wed>\n"
        ":PROPERTIES:\n"
        ":ID: 42\n"
        ":END:\n"
        "- [X] done item\n"
        "| a | 1 |\n"
        "|---+---|\n"
        "# a comment\n"
        "** Sub heading\n"
        "#+begin_example\n"
        "* not a heading\n"
        "#+end_example\n"
        "* DONE Second\n";
    ExpectCaptures("org", src, "#+TITLE", "OrgDirective");
    ExpectCaptures("org", src, "TITLE", "OrgDirective OrgDirectiveName");
    ExpectCaptures("org", src, "Notes", "OrgDirective OrgDirectiveValue");
    ExpectCaptures("org", src, "* TODO", "OrgStars1");
    ExpectCaptures("org", src, "TODO [", "OrgHeadlineLevel1 OrgKeywordTodo");
    ExpectCaptures("org", src, "[#A]", "OrgHeadlineLevel1 OrgPriorityCookie");
    ExpectCaptures("org", src, "Ship", "OrgHeadlineLevel1");
    ExpectCaptures("org", src, "work", "OrgTagList OrgTag");
    ExpectCaptures("org", src, "<2024", "OrgTimestampActive OrgTimestampActive");
    ExpectCaptures("org", src, "2024-01-03", "OrgTimestampActive OrgTimestampActive OrgTimestampDate");
    ExpectCaptures("org", src, ":PROPERTIES:", "OrgPropertyDrawer");
    ExpectCaptures("org", src, "ID", "OrgPropertyDrawer OrgProperty OrgPropertyName");
    ExpectCaptures("org", src, "42", "OrgPropertyDrawer OrgProperty OrgPropertyValue");
    ExpectCaptures("org", src, "- [X]", "OrgListBullet");
    ExpectCaptures("org", src, "[X]", "OrgCheckbox");
    ExpectCaptures("org", src, "X]", "OrgCheckbox OrgCheckDone");
    ExpectCaptures("org", src, "|---", "OrgTableHorizontalRuler");
    ExpectCaptures("org", src, "# a comment", "OrgComment");
    ExpectCaptures("org", src, "** Sub", "OrgStars2");
    ExpectCaptures("org", src, "Sub heading", "OrgHeadlineLevel2");
    ExpectCaptures("org", src, "#+begin_example", "OrgBlock");
    ExpectCaptures("org", src, "example\n*", "OrgBlock OrgBlockName");
    ExpectCaptures("org", src, "* not", "OrgBlock OrgBlockContents");
    ExpectCaptures("org", src, "DONE", "OrgHeadlineLevel1 OrgKeywordDone");

    // A line inside a block is its text, never a headline.
    ExpectOutline("org", src,
                  "0:heading1:TODO [#A] Ship it@1\n"
                  "1:heading2:Sub heading@10\n"
                  "0:heading1:DONE Second@14\n");

    bool section = false, block = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("org", src)) {
        section = section || (f.start_row == 1 && f.end_row == 13);
        block = block || (f.start_row == 11 && f.end_row == 13);
    }
    CHECK(section);
    CHECK(block);
}

// ---------------------------------------------------------------------------
// Markdown: mep's own grammars (grammars/tree-sitter-markdown and
// grammars/tree-sitter-markdown_inline), compiled in
// ---------------------------------------------------------------------------

void TestMarkdown() {
    CHECK(TreesitterHasGrammar("md"));
    CHECK(TreesitterHasStructureQuery("md"));
    const std::string src =
        "# Title ##\n"
        "\n"
        "Some *emphasis*, **strong**, `code` and a [link](http://x.org \"t\").\n"
        "\n"
        "> - [x] done\n"
        ">   - nested\n"
        "\n"
        "~~~\n"
        "# not a heading\n"
        "~~~\n"
        "\n"
        "Setext\n"
        "------\n"
        "\n"
        "| a | b |\n"
        "|---|:-:|\n"
        "| 1 | 2 |\n";
    ExpectCaptures("md", src, "# Title", "punctuation.special");
    ExpectCaptures("md", src, "Title", "text.title.1");
    ExpectCaptures("md", src, "*emphasis*", "text.emphasis punctuation.delimiter");
    ExpectCaptures("md", src, "emphasis*", "text.emphasis");
    ExpectCaptures("md", src, "strong**", "text.strong");
    ExpectCaptures("md", src, "`code`", "text.literal punctuation.delimiter");
    ExpectCaptures("md", src, "link]", "text.reference");
    ExpectCaptures("md", src, "http", "text.uri");
    ExpectCaptures("md", src, "> -", "punctuation.special");
    ExpectCaptures("md", src, "- [x]", "punctuation.special");
    ExpectCaptures("md", src, "~~~\n#", "text.literal punctuation.delimiter");
    ExpectCaptures("md", src, "# not", "text.literal none");
    ExpectCaptures("md", src, "Setext", "text.title.2");
    ExpectCaptures("md", src, "------", "punctuation.special");

    // A heading-looking line in a code block is not a heading; setext
    // headings count.
    ExpectOutline("md", src,
                  "0:heading1:Title ##@0\n"
                  "0:heading2:Setext@11\n");
}

// ---------------------------------------------------------------------------
// R: mep's own grammar (grammars/tree-sitter-r), compiled in
// ---------------------------------------------------------------------------

void TestR() {
    CHECK(TreesitterHasGrammar("r"));
    CHECK(TreesitterHasGrammar("R"));
    CHECK(TreesitterHasFoldQuery("r"));
    const std::string src =
        "library(dplyr)\n"
        "# Square it.\n"
        "sq <- function(x, n = 2L, ...) {\n"
        "  if (is.null(x)) stop(\"no x\\n\")\n"
        "  else x^n\n"
        "}\n"
        "out <- dplyr::filter(df, a %in% b) |> obj$run(TRUE, NA, r\"(raw)\")\n"
        "for (i in 1:10) next\n";
    ExpectCaptures("r", src, "library", "variable function.call include");
    ExpectCaptures("r", src, "dplyr)", "variable");
    ExpectCaptures("r", src, "# Square", "comment");
    ExpectCaptures("r", src, "sq", "variable function");
    ExpectCaptures("r", src, "<-", "operator");
    ExpectCaptures("r", src, "function", "keyword.function");
    ExpectCaptures("r", src, "x,", "variable variable.parameter");
    ExpectCaptures("r", src, "2L", "number");
    ExpectCaptures("r", src, "...", "constant.builtin");
    ExpectCaptures("r", src, "if", "conditional");
    ExpectCaptures("r", src, "is.null", "variable function.call");
    ExpectCaptures("r", src, "stop", "variable function.call exception");
    ExpectCaptures("r", src, "\"no", "string");
    ExpectCaptures("r", src, "\\n", "string string.escape");
    ExpectCaptures("r", src, "else", "conditional");
    ExpectCaptures("r", src, "^", "operator");
    ExpectCaptures("r", src, "dplyr::", "variable module");
    ExpectCaptures("r", src, "::", "operator");
    ExpectCaptures("r", src, "filter", "variable function.call");
    ExpectCaptures("r", src, "%in%", "operator");
    ExpectCaptures("r", src, "|>", "operator");
    ExpectCaptures("r", src, "run", "variable function.method.call");
    ExpectCaptures("r", src, "TRUE", "boolean");
    ExpectCaptures("r", src, "NA", "constant.builtin");
    ExpectCaptures("r", src, "r\"(", "string");
    ExpectCaptures("r", src, "for", "repeat");
    ExpectCaptures("r", src, "in ", "repeat");
    ExpectCaptures("r", src, "next", "keyword");

    // The function's body folds.
    bool body = false;
    for (const TSFoldRange &f : TreesitterFoldRanges("r", src)) body = body || (f.start_row == 2 && f.end_row == 5);
    CHECK(body);
}

// ---------------------------------------------------------------------------
// Julia: mep's own grammar (grammars/tree-sitter-julia), compiled in
// ---------------------------------------------------------------------------

void TestJulia() {
    CHECK(TreesitterHasGrammar("jl"));
    CHECK(TreesitterHasFoldQuery("jl"));
    CHECK(TreesitterHasStructureQuery("jl"));
    const std::string src =
        "# A demo.\n"
        "module Demo\n"
        "\n"
        "using LinearAlgebra: norm\n"
        "\n"
        "const MAX = 3\n"
        "\n"
        "struct Point{T<:Real}\n"
        "    x::T\n"
        "    y::T\n"
        "end\n"
        "\n"
        "\"\"\"Scale a point.\"\"\"\n"
        "function scale(p::Point, k)\n"
        "    for i in 1:MAX\n"
        "        k += 0.5\n"
        "    end\n"
        "    return Point(p.x * k, p.y * k)\n"
        "end\n"
        "\n"
        "macro twice(ex)\n"
        "    :($ex; $ex)\n"
        "end\n"
        "\n"
        "f(x) = isa(x, Int) ? \"int\\n\" : nothing\n"
        "\n"
        "end\n";
    ExpectCaptures("jl", src, "# A demo", "comment");
    ExpectCaptures("jl", src, "module", "keyword.import");
    ExpectCaptures("jl", src, "using", "keyword.import");
    ExpectCaptures("jl", src, "const", "keyword.modifier");
    ExpectCaptures("jl", src, "struct", "keyword.type");
    ExpectCaptures("jl", src, "Real", "type.definition type variable type type.builtin");
    ExpectCaptures("jl", src, "function", "keyword.function");
    ExpectCaptures("jl", src, "scale", "variable function.call");
    ExpectCaptures("jl", src, "for", "keyword.repeat");
    ExpectCaptures("jl", src, "0.5", "number.float");
    ExpectCaptures("jl", src, "return", "keyword.return");
    ExpectCaptures("jl", src, "\"\"\"Scale", "string string.documentation");
    ExpectCaptures("jl", src, "\\n", "string string.escape");
    ExpectCaptures("jl", src, "nothing", "variable constant.builtin");
    ExpectCaptures("jl", src, "isa(", "variable function.call function.builtin");

    bool struct_fold = false, func_fold = false;
    for (const TSFoldRange &r : TreesitterFoldRanges("jl", src)) {
        struct_fold = struct_fold || (r.start_row == 7 && r.end_row >= 9);
        func_fold = func_fold || (r.start_row == 13 && r.end_row >= 17);
    }
    CHECK(struct_fold);
    CHECK(func_fold);
    ExpectOutline("jl", src,
                  "0:module:Demo@1\n"
                  "1:constant:MAX@5\n"
                  "1:struct:Point@7\n"
                  "1:function:scale@13\n"
                  "1:macro:twice@20\n"
                  "1:function:f@24\n");
}

// ---------------------------------------------------------------------------
// R: the structure outline
// ---------------------------------------------------------------------------

void TestRStructure() {
    CHECK(TreesitterHasStructureQuery("r"));
    CHECK(TreesitterHasStructureQuery("R"));
    const std::string src =
        "f <- function(x) x + 1\n"
        "g = function(a, b = 2) {\n"
        "  inner <- \\(y) y\n"
        "  y\n"
        "}\n"
        "h <<- function() NULL\n"
        "Person <- R6Class(\"Person\", public = list(\n"
        "  greet = function() cat(\"hi\")\n"
        "))\n"
        "setClass(\"Point\", representation(x = \"numeric\"))\n"
        "setGeneric(\"area\", function(shape) standardGeneric(\"area\"))\n"
        "setMethod(\"area\", \"Point\", function(shape) 0)\n"
        "obj$method <- function() 1\n"
        "x <- 5\n"
        "print(f(x))\n";
    // Calls that aren't class/generic/method definitions (cat, print,
    // representation, standardGeneric) and plain values are not entries.
    ExpectOutline("r", src,
                  "0:function:f@0\n"
                  "0:function:g@1\n"
                  "1:function:inner@2\n"
                  "0:function:h@5\n"
                  "0:class:Person@6\n"
                  "1:method:greet@7\n"
                  "0:class:Point@9\n"
                  "0:function:area@10\n"
                  "0:method:area@11\n"
                  "0:function:obj$method@12\n");
}

// ---------------------------------------------------------------------------
// The span pipeline itself
// ---------------------------------------------------------------------------

void TestSpanOrderAndClipping() {
    const std::string src =
        "def f(x):\n"
        "    return \"a\\nb\" + f(x)\n";
    const std::vector<TSHighlightSpan> spans = TreesitterHighlight("py", src);
    CHECK(!spans.empty());
    // Widest first: the invariant the renderer's "later draws win" depends on,
    // so a narrow capture nested in a broad one is the one that shows.
    for (size_t i = 1; i < spans.size(); i++) {
        const int prev = spans[i - 1].col_end - spans[i - 1].col_start;
        const int cur = spans[i].col_end - spans[i].col_start;
        CHECK(prev >= cur);
    }
    // Every span is clipped to one line and to real columns.
    for (const TSHighlightSpan &s : spans) {
        CHECK(s.row >= 0);
        CHECK(s.col_start >= 0);
        CHECK(s.col_end > s.col_start);
    }

    // Deterministic: two runs of the same text (the second reusing the
    // incremental-reparse cache) give byte-identical spans. Equal-width spans
    // are ordered by query pattern index rather than left to the sort, which
    // is what stops a colour from depending on which run it was.
    const std::vector<TSHighlightSpan> again = TreesitterHighlight("py", src);
    CHECK(again.size() == spans.size());
    for (size_t i = 0; i < spans.size(); i++) {
        CHECK(again[i].row == spans[i].row);
        CHECK(again[i].col_start == spans[i].col_start);
        CHECK(again[i].col_end == spans[i].col_end);
        CHECK(again[i].capture == spans[i].capture);
    }
}

void TestGrammarAvailability() {
    CHECK(TreesitterHasGrammar("py"));
    CHECK(TreesitterHasGrammar("c"));
    CHECK(TreesitterHasGrammar("cpp"));
    CHECK(TreesitterHasGrammar("lua"));
    // A filetype with neither a compiled-in grammar nor a query is not
    // highlighted, and asking is not an error.
    CHECK(!TreesitterHasGrammar("no-such-filetype"));
    CHECK(TreesitterHighlight("no-such-filetype", "anything\n").empty());
    CHECK(TreesitterHighlight("py", "").empty());
}

// The pattern-index tiebreak is not a Python detail -- every grammar's query is
// written to the same "last matching pattern wins" convention, and these are
// the cases where a catch-all ((identifier) @variable) covers the exact same
// span as the specific pattern below it.
void TestTiebreakAcrossLanguages() {
    const std::string c_src =
        "#define MAXN 4\n"
        "int main(void) {\n"
        "    printf(\"%d\", MAXN);\n"
        "    return 0;\n"
        "}\n";
    ExpectCaptures("c", c_src, "main", "variable function");
    ExpectCaptures("c", c_src, "printf", "variable function.call");
    ExpectCaptures("c", c_src, "MAXN", "variable constant constant.macro", 0);  // the #define's name
    ExpectCaptures("c", c_src, "MAXN", "variable constant", 1);

    const std::string lua_src =
        "local LIMIT = 3\n"
        "if x and LIMIT then print(LIMIT) end\n";
    ExpectCaptures("lua", lua_src, "LIMIT", "variable constant", 0);
    ExpectCaptures("lua", lua_src, "LIMIT", "variable constant", 1);
    ExpectCaptures("lua", lua_src, "LIMIT", "variable constant", 2);
    ExpectCaptures("lua", lua_src, "and", "operator keyword.operator");
    ExpectCaptures("lua", lua_src, "print", "variable function.call function.builtin");
}

}  // namespace

// TreesitterHighlightRows keeps a per-row answer up to date incrementally
// (only rows an edit or a syntax change touched are re-queried; for
// Markdown, only the touched paragraphs' inline layer is re-parsed). After
// every edit it must say exactly what a fresh whole-text pass says, row by
// row and in the same order (plans/PERFORMANCE_PLAN.md 2.6).
// A row's spans in painting order: only overlapping spans' relative order
// shows, and a span containing another is at least as wide on the row, so
// a stable sort by width keeps that order while ignoring how disjoint
// spans happen to be listed -- a row the incremental path carries over
// keeps its old listing when an edit elsewhere lengthens a node through it
// (an opened template string), the whole-text pass lists it afresh.
std::vector<TSHighlightSpan> PaintOrder(std::vector<TSHighlightSpan> row) {
    std::stable_sort(row.begin(), row.end(), [](const TSHighlightSpan &a, const TSHighlightSpan &b) {
        return a.col_end - a.col_start > b.col_end - b.col_start;
    });
    return row;
}

void CheckRowsMatchWholeText(const std::string &ft, const std::string &key, const std::string &text) {
    std::vector<std::vector<TSHighlightSpan>> rows = TreesitterHighlightRows(ft, text, key);
    static int fresh = 0;
    const std::vector<TSHighlightSpan> whole = TreesitterHighlight(ft, text, "whole-" + std::to_string(fresh++));
    std::vector<std::vector<TSHighlightSpan>> want(rows.size());
    for (const TSHighlightSpan &sp : whole)
        if (sp.row >= 0 && static_cast<size_t>(sp.row) < want.size()) want[static_cast<size_t>(sp.row)].push_back(sp);
    for (auto &row : rows) row = PaintOrder(std::move(row));
    for (auto &row : want) row = PaintOrder(std::move(row));
    bool same = rows.size() == want.size();
    for (size_t r = 0; same && r < rows.size(); r++) {
        same = rows[r].size() == want[r].size();
        for (size_t k = 0; same && k < rows[r].size(); k++)
            same = rows[r][k].row == want[r][k].row && rows[r][k].col_start == want[r][k].col_start &&
                   rows[r][k].col_end == want[r][k].col_end && rows[r][k].capture == want[r][k].capture;
        if (!same) std::fprintf(stderr, "  %s: row %zu differs\n", ft.c_str(), r);
    }
    CHECK(same);
}

void TestIncrementalRowsMatchWholeText() {
    struct Case {
        std::string ft;
        std::string text;
        // (find, replace) edits applied in order, each followed by a check.
        std::vector<std::pair<std::string, std::string>> edits;
    };
    std::string md;
    for (int i = 0; i < 40; i++) {
        md += "# Heading " + std::to_string(i) + "\n\nA paragraph with *emphasis* and `code` and a\n";
        md += "[link](http://example.com/" + std::to_string(i) + ") that runs on\nto a third line **strong**.\n\n";
        md += "- item one\n- item _two_\n\n```python\nx = " + std::to_string(i) + "\n```\n\n";
    }
    std::string cpp;
    for (int i = 0; i < 60; i++)
        cpp += "int f" + std::to_string(i) + "(int a) {\n    // comment\n    return a * " + std::to_string(i) +
               "; /* block */\n    auto s = R\"x(raw " + std::to_string(i) + ")x\";\n}\n\n";
    std::string py;
    for (int i = 0; i < 60; i++)
        py += "def f" + std::to_string(i) + "(a):\n    \"\"\"doc\"\"\"\n    if a:\n        s = f\"{a[\"k\"]!r:>{w}} {{x}}\"\n    return a + " +
              std::to_string(i) + "  # note\n\n";
    // mep's own grammars, whose scanners keep state (R's raw strings) or
    // decide from context (newlines, recipe bodies, paths).
    std::string r;
    for (int i = 0; i < 50; i++)
        r += "f" + std::to_string(i) + " <- function(x) {\n  if (x) r\"(raw " + std::to_string(i) +
             ")\"\n  else x[[1]]\n}\n\n";
    std::string c_code;
    for (int i = 0; i < 50; i++)
        c_code += "#ifdef F" + std::to_string(i) + "\nstatic int f" + std::to_string(i) +
                  "(int a) {\n  if (a) return 1;\n#ifdef G\n  else if (a > 2) return 2;\n#endif\n  return 0; /* x */\n}\n#endif\n\n";
    std::string lua;
    for (int i = 0; i < 50; i++)
        lua += "local function f" + std::to_string(i) + "(x)\n  --[==[ note ]==]\n  return [[s" + std::to_string(i) +
               "]] .. x\nend\n\n";
    std::string js;
    for (int i = 0; i < 50; i++)
        js += "function f" + std::to_string(i) + "(x) {\n  /* note */ const s = `a${x}b" + std::to_string(i) +
              "`\n  return <p a=\"1\">x {s}</p>\n}\n\n";
    // TypeScript's scanner adds signature ends and optional markers to
    // JavaScript's; .tsx shares it.
    std::string ts;
    for (int i = 0; i < 50; i++)
        ts += "function f" + std::to_string(i) + "(x?: number): string;\nfunction f" + std::to_string(i) +
              "(x?: any) {\n  /* note */ const s = styled.div<P>`a${x}b" + std::to_string(i) + "`\n  return s as T\n}\n\n";
    std::string org;
    for (int i = 0; i < 40; i++)
        org += "* H" + std::to_string(i) + " :t:\n- a $x$\n  - b\n:LOGBOOK:\nCLOCK: [2024-01-01 Mon]\n:END:\n#+begin_src c\nint x" +
               std::to_string(i) + ";\n#+end_src\n\n";
    std::string nix = "{\n";
    for (int i = 0; i < 50; i++)
        nix += "  a" + std::to_string(i) + " = ./p/${x}.nix;\n  s" + std::to_string(i) + " = ''\n    v ${y}\n  '';\n";
    nix += "}\n";
    std::string lisp;
    for (int i = 0; i < 50; i++)
        lisp += "(defun f" + std::to_string(i) + " (x)\n  #| note |#\n  (f \"s" + std::to_string(i) +
                "\" x))\n\n";
    std::string just;
    for (int i = 0; i < 50; i++)
        just += "r" + std::to_string(i) + " a='x':\n    echo {{a}} \\\n        -n\n\n";
    // The scanner's state: here-documents waiting for their bodies, and
    // the paren depth of an extglob pattern cut by an expansion.
    std::string sh;
    for (int i = 0; i < 50; i++)
        sh += "f" + std::to_string(i) + "() {\n  cat <<EOF\nv $x " + std::to_string(i) +
              "\nEOF\n  case $1 in @(a|${b})) echo \"s\" ;; esac\n}\n\n";
    // fish's scanner remembers that a continuation carried a comment over
    // (its newline then continues the line too).
    std::string fish;
    for (int i = 0; i < 50; i++)
        fish += "function f" + std::to_string(i) + "\n    echo a \\\n        # why " + std::to_string(i) +
                "\n        | cat \"($x)\" '{'\nend\n\n";
    // Tcl's scanner carries the braces commented-out code leaves open, and
    // decides from the whole braced word whether it holds a script.
    // The MATLAB/Octave scanner keeps the open string's quote and whether
    // it is inside a command's arguments; block comments run to `%}`.
    std::string mat;
    for (int i = 0; i < 50; i++)
        mat += "function r = f" + std::to_string(i) + " (x)\n  %{\n  note\n  %}\n  s = \"a\\\"b " +
               std::to_string(i) + "\";  # c\n  hold on\n  r = [x' 1]';\nendfunction\n\n";
    std::string tcl;
    for (int i = 0; i < 50; i++)
        tcl += "proc f" + std::to_string(i) + " {x} {\n    #if {$x} {\n    #}\n    set re {^[a-z]+$}\n    puts \"v $x(" +
               std::to_string(i) + ") [list a]\"\n}\n\n";
    // Crystal's scanner keeps the open delimited literals (strings,
    // %-literals) and the heredocs waiting for their bodies; macro bodies
    // are text it reads.
    std::string cr;
    for (int i = 0; i < 50; i++)
        cr += "def f" + std::to_string(i) + "(x)\n  s = <<-EOS\n    v #{x} " + std::to_string(i) +
              "\n    EOS\n  t = %w(a b) + [\"s#{x}\"]\n  {% if flag?(:a) %}y{% end %}\nend\n\n";
    // C#'s scanner keeps the open interpolated strings (their `$` count,
    // quotes and braces) and the open raw string's quote count.
    std::string cs;
    for (int i = 0; i < 50; i++)
        cs += "void F" + std::to_string(i) + "(int x)\n{\n    var s = $\"v {x} " + std::to_string(i) +
              " {{y}}\";\n    var r = $$\"\"\"\n        {\"k\": {{x}}}\n        \"\"\";\n    var v = @\"a\"\"b\";\n}\n\n";
    // SQL's scanner keeps a function body's open dollar-quote tag; a DO
    // block's body is one dollar-quoted string token.
    std::string sql;
    for (int i = 0; i < 50; i++)
        sql += "CREATE FUNCTION f" + std::to_string(i) + "(a int) RETURNS int AS $fn$\n  SELECT a + " +
               std::to_string(i) + "\n$fn$;\nDO $$ BEGIN PERFORM 'x$y'; END $$;\nSELECT 'it''s', \"q\" FROM t; -- c\n\n";
    // Fortran's scanner keeps whether the file has shown itself to be fixed
    // form (comment lines by column 1, continuations by column 6) and
    // whether a free-form `&` continuation is open.
    std::string ffixed = "*> fixed form\n";
    for (int i = 0; i < 50; i++)
        ffixed += "      SUBROUTINE F" + std::to_string(i) + "( A,\n     $   B )\nC     note " + std::to_string(i) +
                  "\n      A = B +\n     $    'S" + std::to_string(i) + "'\n      END\n\n";
    // Nim's scanner keeps the layout (indentation) stack and an open long
    // string or block comment.
    std::string nim;
    for (int i = 0; i < 50; i++)
        nim += "proc f" + std::to_string(i) + "(x: int): int =\n  #[ note ]#\n  if x > 0:\n    let s = \"\"\"a " +
               std::to_string(i) + "\"\"\"\n    result = x\n  else:\n    result = 0\n\n";
    const std::vector<Case> cases = {
        {"nim", nim,
         {{"#[ note ]#\n  if x > 0:\n    let s = \"\"\"a 3", "#[ note \n  if x > 0:\n    let s = \"\"\"a 3"},  // a block comment runs on
          {"proc f9(x: int): int =\n  #[ note ]#", "proc f9(x: int): int =\n  ]# #[ note ]#"},
          {"let s = \"\"\"a 20\"\"\"", "let s = \"\"\"a 20"},  // a long string left open
          {"  if x > 0:\n    let s = \"\"\"a 30", "  if x > 0:\n  let s = \"\"\"a 30"},  // a block dedents
          {"    result = x\n  else:\n    result = 0\n\nproc f40", "      result = x\n  else:\n    result = 0\n\nproc f40"}}},
        {"f", ffixed,
         {{"*> fixed form\n", "! free form now\n"},                  // the file stops being fixed form
          {"C     note 9\n", "C     note 9 &\n"},
          {"     $    'S20'", "     $    'S20"},                    // a string left open
          {"      A = B +\n     $    'S30'", "      A = B + &\n          'S30'"}}},  // a free-form continuation
        {"sql", sql,
         {{"AS $fn$\n  SELECT a + 3\n$fn$", "AS $fn$\n  SELECT a + 3\n$fx$"},  // a body's close tag no longer matches...
          {"END $$;\nSELECT 'it''s', \"q\" FROM t; -- c\n\nCREATE FUNCTION f9",
           "END $;\nSELECT 'it''s', \"q\" FROM t; -- c\n\nCREATE FUNCTION f9"},  // a DO body runs on
          {"SELECT a + 20\n$fn$", "SELECT a + 20\n$$"},
          {"AS $fn$\n  SELECT a + 30", "AS $q$\n  SELECT a + 30"},  // a different tag opens
          {"'it''s'", "'it''s"}}},                                   // a string left open
        {"md", md,
         {{"A paragraph with *emphasis*", "A paragraph with *emph"},          // opens emphasis across lines
          {"to a third line **strong**.", "to a third line* **strong**."},  // ...and closes it two lines on
          {"# Heading 7", "Heading 7"},                                        // a heading stops being one
          {"```python\nx = 3", "```python\nx = 3\n```\n\n```"},           // fences re-pair
          {"- item _two_", "- item _two"},
          {"\n\n# Heading 20", "\n\nPlain text joining\n# Heading 20"},
          {"- item one\n- item _two_\n\n```python\nx = 25", "> - item one\n>   - item _two_\n\n```python\nx = 25"},  // a quote and a nested list open
          {"- item one\n- item _two_\n\n```python\nx = 30", "- item one\n    - item _two_\n\n~~~python\nx = 30"},  // a list deepens; a fence changes kind
          {"> - item one", "- item one"}}},                                                                       // ...and the quote closes again
        {"cpp", cpp,
         {{"// comment", "/* opens a block comment"},
          {"/* block */", "*/ /* block */"},
          {"return a * 5;", "return \"unterminated;"},
          {"int f9(int a) {", "int f9(int a) {\n    int added = 1;"},
          {"R\"x(raw 20)x\"", "R\"x(raw 20)y\""},   // a raw string's delimiter no longer closes it...
          {"R\"x(raw 30)x\"", "R\"y(raw 30)y\""},   // ...until a later one does
          {"R\"x(raw 40)", "R\"x(raw )x\" 40)"}}},  // content holding a quote-like `)x"`
        {"py", py,
         {{"\"\"\"doc\"\"\"", "\"\"\"doc"},
          {"return a + 3", "return a + 3  \"\"\""},
          {"def f10(a):", "class C10:"},
          // mep's own scanner (grammars/tree-sitter-python): its indent
          // and string-delimiter stacks are what a reparse restores.
          {"s = f\"{a[\"k\"]!r:>{w}} {{x}}\"\n    return a + 20", "s = f\"{a[\"k\"\n    return a + 20"},
          {"    if a:\n        s = f\"{a[\"k\"]!r:>{w}} {{x}}\"\n    return a + 30",
           "    if a:\n            s = f\"{a}\"\n    return a + 30"},
          {"    if a:\n        s = f\"{a[\"k\"]!r:>{w}} {{x}}\"\n    return a + 40",
           "    if a:\n    s = 1\n    return a + 40"}}},
        {"r", r,
         {{"r\"(raw 3)\"", "r\"--(raw 3)\""},  // a raw string left open runs on
          {"r\"(raw 9)\"", "r\"(raw 9)--\""},  // ...and is closed again further down
          {"  else x[[1]]\n}\n\nf12", "  else x[[1]\n}\n\nf12"},
          {"f20 <- function(x) {", "f20 <- function(x)\n{"}}},
        {"c", c_code,
         {{"#ifdef F3\n", "#if F3\n"},                         // a directive left without its condition's newline
          {"return 0; /* x */\n}\n#endif\n\n#ifdef F9", "return 0; /* x \n}\n#endif\n\n#ifdef F9"},  // a comment runs on
          {"/* x */\n}\n#endif\n\n#ifdef F20", "/* x */\n}\n\n#ifdef F20"},  // an #endif goes missing
          {"else if (a > 2) return 2;", "if (a > 2) return 2;"}}},
        {"lua", lua,
         {{"--[==[ note ]==]\n  return [[s3]]", "--[==[ note ]=]\n  return [[s3]]"},  // a block comment runs on
          {"return [[s9]] .. x", "return [[s9]] .. x ]==]"},                          // ...to a later close
          {"return [[s20]]", "return [=[s20]]"},
          {"local function f30(x)", "global function f30(x)"}}},
        {"js", js,
         {{"/* note */ const s = `a${x}b3`", "/* note const s = `a${x}b3`"},  // a block comment runs on
          {"const s = `a${x}b9`", "*/ const s = `a${x}b9`"},                  // ...and closes later
          {"`a${x}b20`", "`a${x b20`"},                                       // a substitution left open
          {"`a${x}b25`\n  return", "`a${x}b25\n  return"},                    // a template left open
          {"return <p a=\"1\">x {s}</p>\n}\n\nfunction f30", "return <p a=\"1\">x {s}\n}\n\nfunction f30"}}},  // an unclosed element
        {"ts", ts,
         {{"/* note */ const s = styled.div<P>`a${x}b3`", "/* note const s = styled.div<P>`a${x}b3`"},  // a comment runs on
          {"const s = styled.div<P>`a${x}b9`", "*/ const s = styled.div<P>`a${x}b9`"},             // ...and closes later
          {"function f20(x?: number): string;", "function f20(x?: number): string"},               // a signature's `;` goes
          {"`a${x}b25`\n  return", "`a${x}b25\n  return"}}},                                       // a template left open
        {"tsx", ts,
         {{"`a${x}b20`", "`a${x b20`"},                                                             // a substitution left open
          {"return s as T\n}\n\nfunction f30", "return <s>{T}\n}\n\nfunction f30"}}},                // an unclosed element
        {"org", org,
         {{"#+begin_src c\nint x3;", "#+begin_example c\nint x3;"},  // a block's end no longer matches...
          {"#+begin_src c\nint x9;\n#+end_src", "#+begin_src c\nint x9;\n#+end_example"},  // ...until a later one does
          {"- a $x$\n  - b\n:LOGBOOK:\nCLOCK: [2024-01-01 Mon]\n:END:\n#+begin_src c\nint x20", "- a $x\n  - b\n:LOGBOOK:\nCLOCK: [2024-01-01 Mon]\n:END:\n#+begin_src c\nint x20"},  // math left open
          {"  - b\n:LOGBOOK:\nCLOCK: [2024-01-01 Mon]\n:END:\n#+begin_src c\nint x25", "    - b\n:LOGBOOK:\nCLOCK: [2024-01-01 Mon]\n:END:\n#+begin_src c\nint x25"},  // a list deepens
          {":END:\n#+begin_src c\nint x30", "\n#+begin_src c\nint x30"},  // a drawer left open
          {"* H35", "*** H35"}}},
        {"nix", nix,
         {{"a3 = ./p/${x}.nix;", "a3 = ./p/${x.nix;"},
          {"s7 = ''", "s7 = \""},
          {"a20 = ./p", "a20 = p"}}},
        {"lisp", lisp,
         {{"#| note |#\n  (f \"s3\"", "#| note \n  (f \"s3\""},  // a block comment runs on
          {"(f \"s9\" x))", "(f \"s9\" x)) |#"},                    // ...to a later close
          {"#| note |#\n  (f \"s20\"", "#| #| note |#\n  (f \"s20\""},  // nested: one close is not enough
          {"(f \"s30\" x)", "(f \"s30 x)"}}},                        // a string left open
        {"sh", sh,
         {{"v $x 3\nEOF", "v $x 3\nEOX"},                  // a here-document runs on...
          {"v $x 9\nEOF", "v $x 9\nEOF\nEOX"},             // ...past a line that doesn't end it
          {"cat <<EOF\nv $x 20", "cat <<-'EOF'\nv $x 20"},  // quoted, tab-stripping
          {"echo \"s\" ;; esac\n}\n\nf30", "echo \"s ;; esac\n}\n\nf30"},  // a string left open
          {"@(a|${b})) echo \"s\" ;; esac\n}\n\nf40", "@(a|${b}) echo \"s\" ;; esac\n}\n\nf40"}}},
        {"fish", fish,
         {{"echo a \\\n        # why 3", "echo a\n        # why 3"},           // the continuation goes...
          {"echo a \\\n        # why 9", "echo a \\\n        # why 9 \\"},  // a comment ending in `\\` is still a comment
          {"| cat \"($x)\" '{'\nend\n\nfunction f20", "| cat \"($x)\" '{\nend\n\nfunction f20"},  // a string left open
          {"function f30", "{ function f30"}}},                                  // a block opens
        {"m", mat,
         {{"%{\n  note\n  %}\n  s = \"a\\\"b 3", "%{\n  note\n  s = \"a\\\"b 3"},  // a block comment runs on...
          {"s = \"a\\\"b 9\";", "s = \"a\\\"b 9;"},                                   // a string left open
          {"hold on\n  r = [x' 1]';\nendfunction\n\nfunction r = f20", "hold on\n  r = [x' 1;\nendfunction\n\nfunction r = f20"},  // a bracket
          {"function r = f30", "%{\nfunction r = f30"}}},                                       // a block comment opens
        {"tcl", tcl,
         {{"#if {$x} {\n    #}\n    set re {^[a-z]+$}\n    puts \"v $x(3)", "#if {$x} {\n    set re {^[a-z]+$}\n    puts \"v $x(3)"},  // the comment's brace now closes nothing...
          {"    #}\n    set re {^[a-z]+$}\n    puts \"v $x(9)", "    #}\n    #}\n    set re {^[a-z]+$}\n    puts \"v $x(9)"},  // ...and one too many
          {"set re {^[a-z]+$}\n    puts \"v $x(20)", "set re {^[a-z]+$\n    puts \"v $x(20)"},  // a braced word left open
          {"puts \"v $x(30) [list a]\"", "puts \"v $x(30) [list a]"},              // a quote left open
          {"proc f40 {x} {", "proc f40 {x} {\n    set y {\"}"}}},                // braced data with a lone quote
        {"just", just,
         {{"r3 a='x':", "r3 a='x"},
          {"echo {{a}} \\\n        -n\n\nr9", "echo {{a}}\n        -n\n\nr9"},
          {"\n\nr20", "\nr20"}}},
        {"cr", cr,
         {{"v #{x} 3\n    EOS", "v #{x} 3\n    EOX"},                    // a heredoc runs on to the next EOS
          {"12\n    EOS\n  t = %w(a b)", "12\n    EOS\n  t = %w(a b"},     // a %-literal left open
          {"20\n    EOS\n  t = %w(a b) + [\"s#{x}\"]", "20\n    EOS\n  t = %w(a b) + [\"s#{x}]"},  // a string
          {"30\n    EOS\n  t = %w(a b) + [\"s#{x}\"]\n  {% if flag?(:a) %}y{% end %}",
           "30\n    EOS\n  t = %w(a b) + [\"s#{x}\"]\n  {% if flag?(:a) %}y{% en %}"},  // a macro block left open
          {"def f40(x)\n  s = <<-EOS", "def f40(x)\n  s = <<-'EOS'"}}},          // quoted: no interpolation
        {"cs", cs,
         {{"$\"v {x} 3 {{y}}\";", "$\"v {x} 3 {{y}};"},                // an interpolated string left open
          {"$\"v {x} 9 {{y}}\"", "$\"v {x 9 {{y}}\""},                 // an interpolation left open
          {"$$\"\"\"\n        {\"k\": {{x}}}\n        \"\"\";\n    var v = @\"a\"\"b\";\n}\n\nvoid F20",
           "$$\"\"\"\n        {\"k\": {{x}}}\n        \"\";\n    var v = @\"a\"\"b\";\n}\n\nvoid F20"},  // a raw string runs on
          {"var s = $\"v {x} 30", "var s = $$\"v {x} 30"},              // braces now need two
          {"@\"a\"\"b\";\n}\n\nvoid F40", "@\"a\"b\";\n}\n\nvoid F40"}}},  // a verbatim string ends early
    };
    for (const Case &c : cases) {
        if (!TreesitterHasGrammar(c.ft)) continue;
        const std::string key = "incr-test-" + c.ft;
        std::string text = c.text;
        CheckRowsMatchWholeText(c.ft, key, text);
        for (const auto &[find, repl] : c.edits) {
            const size_t at = text.find(find);
            CHECK(at != std::string::npos);
            if (at == std::string::npos) continue;
            text.replace(at, find.size(), repl);
            CheckRowsMatchWholeText(c.ft, key, text);
        }
    }
}

int main() {
    TestIncrementalRowsMatchWholeText();
    TestPythonFStrings();
    TestPythonKeywords();
    TestPythonSoftKeywords();
    TestPythonIdentifierConventions();
    TestPythonParameters();
    TestPythonCalls();
    TestPythonAnnotations();
    TestPythonLiteralsAndComments();
    TestPythonGrammar();
    TestNix();
    TestMaxima();
    TestFish();
    TestFortran();
    TestTcl();
    TestGap();
    TestGo();
    TestJava();
    TestD();
    TestNim();
    TestJulia();
    TestRust();
    TestZig();
    TestMatlab();
    TestJust();
    TestC();
    TestLua();
    TestJavascript();
    TestTypescript();
    TestCommonLisp();
    TestBash();
    TestClojure();
    TestScheme();
    TestCrystal();
    TestCSharp();
    TestSQL();
    TestCpp();
    TestOrg();
    TestMarkdown();
    TestR();
    TestRStructure();
    TestSpanOrderAndClipping();
    TestGrammarAvailability();
    TestTiebreakAcrossLanguages();
    std::printf("treesitter tests passed\n");
    return 0;
}
