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
    const std::vector<Case> cases = {
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
        {"just", just,
         {{"r3 a='x':", "r3 a='x"},
          {"echo {{a}} \\\n        -n\n\nr9", "echo {{a}}\n        -n\n\nr9"},
          {"\n\nr20", "\nr20"}}},
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
    TestJust();
    TestC();
    TestLua();
    TestJavascript();
    TestCommonLisp();
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
