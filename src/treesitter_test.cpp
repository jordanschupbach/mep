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

// The pattern-index tiebreak is not a Python detail -- every vendored query is
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
    ExpectCaptures("c", c_src, "printf", "variable function");
    ExpectCaptures("c", c_src, "MAXN", "variable constant", 0);
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
void CheckRowsMatchWholeText(const std::string &ft, const std::string &key, const std::string &text) {
    const auto &rows = TreesitterHighlightRows(ft, text, key);
    static int fresh = 0;
    const std::vector<TSHighlightSpan> whole = TreesitterHighlight(ft, text, "whole-" + std::to_string(fresh++));
    std::vector<std::vector<TSHighlightSpan>> want(rows.size());
    for (const TSHighlightSpan &sp : whole)
        if (sp.row >= 0 && static_cast<size_t>(sp.row) < want.size()) want[static_cast<size_t>(sp.row)].push_back(sp);
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
               "; /* block */\n}\n\n";
    std::string py;
    for (int i = 0; i < 60; i++)
        py += "def f" + std::to_string(i) + "(a):\n    \"\"\"doc\"\"\"\n    return a + " + std::to_string(i) + "  # note\n\n";
    const std::vector<Case> cases = {
        {"md", md,
         {{"A paragraph with *emphasis*", "A paragraph with *emph"},          // opens emphasis across lines
          {"to a third line **strong**.", "to a third line* **strong**."},  // ...and closes it two lines on
          {"# Heading 7", "Heading 7"},                                        // a heading stops being one
          {"```python\nx = 3", "```python\nx = 3\n```\n\n```"},           // fences re-pair
          {"- item _two_", "- item _two"},
          {"\n\n# Heading 20", "\n\nPlain text joining\n# Heading 20"}}},
        {"cpp", cpp,
         {{"// comment", "/* opens a block comment"},
          {"/* block */", "*/ /* block */"},
          {"return a * 5;", "return \"unterminated;"},
          {"int f9(int a) {", "int f9(int a) {\n    int added = 1;"}}},
        {"py", py,
         {{"\"\"\"doc\"\"\"", "\"\"\"doc"},
          {"return a + 3", "return a + 3  \"\"\""},
          {"def f10(a):", "class C10:"}}},
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
    TestNix();
    TestSpanOrderAndClipping();
    TestGrammarAvailability();
    TestTiebreakAcrossLanguages();
    std::printf("treesitter tests passed\n");
    return 0;
}
