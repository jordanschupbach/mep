// Windowless test for python_lsp.cpp and python_ast.cpp -- the analysis
// and parsing halves of mep's own Python language server
// (`mep-python-lsp`, src/python_lsp_server.cpp). Drives the pure
// functions directly: no process, no JSON-RPC client, no GL context.
// CHECK(), never assert(): the Release build strips assert() entirely.
//
// Two conventions, both borrowed from org_lsp_test.cpp because they are
// what make a linter's test suite worth having:
//   - Diagnostics are asserted by PythonLspDiagnostic::code, never by
//     message wording, so the messages stay free to improve.
//   - Every check asserts the *absence* of a diagnostic on the
//     legal-but-suspicious shapes it must stay quiet about, as
//     deliberately as it asserts the true reports. A linter that cries
//     wolf gets switched off, and then its true reports go unseen too.
#include "python_lsp.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "python_ast.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

/** @brief Options that never touch the filesystem, so the suite answers the same in any directory. */
PythonLspOptions NoFiles() {
    PythonLspOptions opts;
    opts.check_files = false;
    return opts;
}

std::vector<PythonLspDiagnostic> Lint(const Lines &lines) { return PythonLspDiagnostics(lines, NoFiles()); }

/** @brief Counts diagnostics carrying a given code. */
size_t CountCode(const std::vector<PythonLspDiagnostic> &diags, const std::string &code) {
    size_t n = 0;
    for (const PythonLspDiagnostic &d : diags) {
        if (d.code == code) n++;
    }
    return n;
}

/** @brief Reports whether any diagnostic carries a given code. */
bool HasCode(const std::vector<PythonLspDiagnostic> &diags, const std::string &code) {
    return CountCode(diags, code) > 0;
}

/** @brief Reports whether linting these lines produces a given code. */
bool Reports(const Lines &lines, const std::string &code) { return HasCode(Lint(lines), code); }

// Returned by value, not by reference: GCC's -Wdangling-reference
// cannot tell that the reference points into `diags` rather than at the
// string literal parameter, and a copy of a diagnostic costs nothing.
/** @brief Returns the first diagnostic carrying a given code, aborting the test if there is none. */
PythonLspDiagnostic FindCode(const std::vector<PythonLspDiagnostic> &diags, const std::string &code) {
    for (const PythonLspDiagnostic &d : diags) {
        if (d.code == code) return d;
    }
    Check(false, code.c_str(), __LINE__);
    return diags[0];  // unreachable: Check aborts
}

/** @brief Reports whether a completion list offers a label. */
bool Offers(const std::vector<PythonLspCompletionItem> &items, const std::string &label) {
    for (const PythonLspCompletionItem &i : items) {
        if (i.label == label) return true;
    }
    return false;
}

/** @brief The completions at a position, with no filesystem access. */
std::vector<PythonLspCompletionItem> Complete(const Lines &lines, int line, int col) {
    return PythonLspCompletions(lines, line, col, NoFiles());
}

/** @brief Reports whether a symbol list contains a name. */
bool HasSymbol(const std::vector<PythonLspSymbol> &syms, const std::string &name) {
    for (const PythonLspSymbol &s : syms) {
        if (s.name == name) return true;
    }
    return false;
}

/** @brief Returns the named symbol, aborting the test if there is none (by value, see FindCode). */
PythonLspSymbol FindSymbol(const std::vector<PythonLspSymbol> &syms, const std::string &name) {
    for (const PythonLspSymbol &s : syms) {
        if (s.name == name) return s;
    }
    Check(false, name.c_str(), __LINE__);
    return syms[0];  // unreachable
}

// --- Tokenizer and parser ---------------------------------------------

void TestTokenizer() {
    // Indentation is structure: the tokenizer must synthesize the
    // INDENT/DEDENT pair the grammar hangs everything else off.
    const Lines block = {"if x:", "    y = 1", "z = 2"};
    std::vector<PySyntaxError> errors;
    const std::vector<PyToken> tokens = TokenizePython(block, &errors, nullptr, nullptr);
    CHECK(errors.empty());
    size_t indents = 0, dedents = 0;
    for (const PyToken &t : tokens) {
        if (t.kind == PyTokKind::Indent) indents++;
        if (t.kind == PyTokKind::Dedent) dedents++;
    }
    CHECK(indents == 1);
    CHECK(dedents == 1);

    // A blank line and a comment line inside a suite must not close it.
    CHECK(ParsePython({"def f():", "    a = 1", "", "    # note", "    return a"}).errors.empty());

    // Implicit line joining inside brackets, explicit joining with a
    // backslash: neither ends the logical line.
    CHECK(ParsePython({"x = [1,", "     2,", "     3]"}).errors.empty());
    CHECK(ParsePython({"x = 1 + \\", "    2"}).errors.empty());

    // Strings: every prefix, both quote styles, and triple quotes that
    // swallow newlines whole.
    CHECK(ParsePython({"a = r'\\d+'", "b = b\"bytes\"", "c = rb'raw'", "d = f\"{a}\"", "e = u'u'"}).errors.empty());
    CHECK(ParsePython({"s = '''", "spanning", "lines'''", "t = 1"}).errors.empty());
    CHECK(ParsePython({"s = 'unterminated"}).errors.size() == 1);
    CHECK(ParsePython({"s = 'unterminated"}).errors[0].code == "unterminated-string");
    CHECK(ParsePython({"s = '''never closed"}).errors[0].code == "unterminated-string");
    // A quote inside a replacement field is a nested literal (PEP 701),
    // not the end of the f-string.
    CHECK(ParsePython({"x = f'{d['key']} tail'"}).errors.empty());
    CHECK(ParsePython({"x = f'{f' {inner}' if inner else ''} tail'"}).errors.empty());

    // Numbers, including the ones that are not numbers at all.
    CHECK(ParsePython({"a = 0xFF", "b = 0o17", "c = 0b1010", "d = 1_000_000", "e = 1.5e-3", "f = 3j"}).errors.empty());
    CHECK(ParsePython({"x = 0x"}).errors[0].code == "invalid-number");
    CHECK(ParsePython({"x = 123abc"}).errors[0].code == "invalid-number");

    // Brackets, tabs and stray characters.
    CHECK(ParsePython({"x = (1, 2"}).errors[0].code == "unclosed-bracket");
    CHECK(ParsePython({"x = [1, 2)"}).errors[0].code == "mismatched-bracket");
    CHECK(ParsePython({"x = 1 $ 2"}).errors[0].code == "invalid-character");
    CHECK(ParsePython({"if x:", "    a = 1", "  b = 2"}).errors[0].code == "bad-dedent");

    // A token's span is what every squiggle is drawn from, so it has to
    // be exact.
    const PyParseResult parsed = ParsePython({"value = 42"});
    CHECK(parsed.tokens[0].kind == PyTokKind::Name);
    CHECK(parsed.tokens[0].start.col == 0);
    CHECK(parsed.tokens[0].end.col == 5);
}

void TestParserCoverage() {
    // Everything in this list is legal Python that a parser written from
    // the 3.8-era grammar would reject; each one earned its place by
    // being found in real code.
    const Lines modern = {
        "from __future__ import annotations",
        "import asyncio",
        "",
        "type Alias = list[int]",
        "",
        "async def fetch(url: str, *, timeout: float = 1.0) -> bytes:",
        "    async with open(url) as f:",
        "        async for chunk in f:",
        "            await asyncio.sleep(0)",
        "    return b''",
        "",
        "def positional(a, b, /, c, *args, d=1, **kwargs):",
        "    return a, b, c, d, args, kwargs",
        "",
        "def matcher(command):",
        "    match command.split():",
        "        case [action]:",
        "            return action",
        "        case [action, obj] if obj:",
        "            return action, obj",
        "        case {'key': value, **rest}:",
        "            return value, rest",
        "        case Point(x=0, y=0) | None:",
        "            return None",
        "        case _:",
        "            return None",
        "",
        "def walrus(items):",
        "    if (n := len(items)) > 3:",
        "        return n",
        "    return [y for x in items if (y := x) is not None]",
        "",
        "class Point:",
        "    x: int = 0",
        "    y: int = 0",
        "",
        "def groups():",
        "    try:",
        "        pass",
        "    except* ValueError:",
        "        pass",
        "    with (open('a') as a, open('b') as b):",
        "        return a, b",
        "",
        "lam = lambda a, *, b=2: a + b",
        "cond = 1 if lam else 2",
        "sliced = [1, 2, 3][::2]",
        "nested = {k: [v for v in range(3)] for k in 'ab'}",
        "starred = [*sliced, *[1]]",
        "chained = 0 < len(sliced) <= 3",
    };
    const PyParseResult parsed = ParsePython(modern);
    for (const PySyntaxError &e : parsed.errors) {
        std::fprintf(stderr, "unexpected parse error %d:%d %s: %s\n", e.start.line + 1, e.start.col, e.code.c_str(),
                     e.message.c_str());
    }
    CHECK(parsed.errors.empty());
    CHECK(Lint(modern).empty() || !HasCode(Lint(modern), "undefined-name"));

    // Python 2 gets told what it is, rather than a generic "invalid
    // syntax" -- the file it comes from is usually Python 2 throughout.
    CHECK(ParsePython({"print 'hello'"}).errors[0].code == "python2-print");
    CHECK(ParsePython({"try:", "    pass", "except ValueError, e:", "    pass"}).errors[0].code == "python2-except");
    CHECK(ParsePython({"x = 1 <> 2"}).errors[0].code == "python2-ne");
    CHECK(ParsePython({"x = 10L"}).errors[0].code == "python2-long");
    // ... but Python 3.14's unparenthesized except list, which is spelled
    // the same way, is not Python 2.
    CHECK(ParsePython({"try:", "    pass", "except ValueError, TypeError:", "    pass"}).errors.empty());
    // `print(...)` and `match`/`type` as ordinary names stay ordinary.
    CHECK(ParsePython({"print('hello')"}).errors.empty());
    CHECK(ParsePython({"match = re.match(p, s)", "type = 'a'", "case = 1"}).errors.empty());

    // Recovery: a broken line must not cost the rest of the file.
    const PyParseResult broken = ParsePython({"def good():", "    return 1", "", "def bad(:", "    pass", "",
                                              "def also_good():", "    return 2"});
    CHECK(!broken.errors.empty());
    CHECK(HasSymbol(PythonLspSymbols({"def good():", "    return 1", "", "def bad(:", "    pass", "",
                                      "def also_good():", "    return 2"}),
                    "also_good"));

    // Pathological nesting is bounded rather than fatal.
    std::string deep(400, '(');
    CHECK(!ParsePython({"x = " + deep}).errors.empty());
}

// --- Diagnostics ------------------------------------------------------

void TestSyntaxDiagnostics() {
    // The bracket is the real mistake here, and it is what gets
    // reported -- the parser keeps one error per line, so the cascade of
    // complaints about the tokens after it stays out of the way.
    CHECK(Reports({"def f(:", "    pass"}, "unclosed-bracket"));
    CHECK(Reports({"from os import"}, "expected-name"));
    CHECK(CountCode(Lint({"def f(:", "    pass"}), "unclosed-bracket") == 1);
    CHECK(Reports({"if True:", "pass"}, "expected-block"));
    CHECK(Reports({"try:", "    pass"}, "try-without-except"));
    CHECK(Reports({"try:", "    pass", "except:", "    pass", "except ValueError:", "    pass"},
                  "except-after-bare"));
    // Both halves of a syntax error report at the error, and the
    // scope-derived checks stand down until the file parses -- a
    // half-typed line must not produce a screenful of undefined names.
    const std::vector<PythonLspDiagnostic> mid_edit = Lint({"import os", "def f(:", "    return os.sep", "bogus_name"});
    CHECK(!mid_edit.empty());
    CHECK(!HasCode(mid_edit, "undefined-name"));
    CHECK(!HasCode(mid_edit, "unused-import"));
}

void TestUndefinedName() {
    CHECK(Reports({"x = undefined_thing"}, "undefined-name"));
    CHECK(Reports({"def f():", "    return missing"}, "undefined-name"));
    // Order does not matter at module level, and builtins are defined.
    CHECK(!Reports({"def f():", "    return helper()", "", "def helper():", "    return 1"}, "undefined-name"));
    CHECK(!Reports({"print(len([1]), OSError, __name__, __file__)"}, "undefined-name"));
    // Every binding form has to count as one.
    CHECK(!Reports({"import os", "print(os.sep)"}, "undefined-name"));
    CHECK(!Reports({"from os import sep", "print(sep)"}, "undefined-name"));
    CHECK(!Reports({"for item in []:", "    print(item)"}, "undefined-name"));
    CHECK(!Reports({"with open('f') as fh:", "    print(fh)"}, "undefined-name"));
    CHECK(!Reports({"try:", "    pass", "except OSError as err:", "    print(err)"}, "undefined-name"));
    CHECK(!Reports({"print([y for y in range(3)])"}, "undefined-name"));
    CHECK(!Reports({"def f(a, b=1, *rest, c, **kw):", "    return a, b, rest, c, kw"}, "undefined-name"));
    CHECK(!Reports({"def f():", "    global counter", "    counter = 1", "", "def g():", "    return counter"},
                   "undefined-name"));
    CHECK(!Reports({"lam = lambda v: v + 1"}, "undefined-name"));
    CHECK(!Reports({"def f(x):", "    match x:", "        case [a, b]:", "            return a + b"},
                   "undefined-name"));
    CHECK(!Reports({"import os", "name = f'{os.sep}'"}, "undefined-name"));
    // A class body's names are visible to the body, not to methods
    // nested in it -- and a comprehension in a class body reads its
    // first iterable in the class scope, which is the only way it can
    // see a class variable at all.
    CHECK(Reports({"class C:", "    size = 1", "    def m(self):", "        return size"}, "undefined-name"));
    CHECK(!Reports({"class C:", "    names = ['a']", "    upper = [n.upper() for n in names]"}, "undefined-name"));
    // A star import makes the module's namespace unknowable, so the
    // check switches itself off rather than guessing.
    CHECK(!Reports({"from os.path import *", "print(join('a', 'b'))"}, "undefined-name"));
    CHECK(Reports({"from os.path import *", "print(join('a', 'b'))"}, "star-import"));
    // So does anything that writes the namespace at run time.
    CHECK(!Reports({"globals().update({'x': 1})", "print(x)"}, "undefined-name"));
    // A string annotation is a forward reference, not a claim that the
    // name exists -- `Literal["w"]` would otherwise invent one.
    CHECK(!Reports({"from typing import Literal", "def f(mode: Literal['w']) -> None:", "    return None"},
                   "undefined-name"));
    CHECK(!Reports({"from mod import Node", "def f(n: 'Node') -> 'Node':", "    return n"}, "unused-import"));
}

void TestUnusedBindings() {
    CHECK(Reports({"import os"}, "unused-import"));
    CHECK(Reports({"from os import sep"}, "unused-import"));
    CHECK(!Reports({"import os", "print(os.sep)"}, "unused-import"));
    // `import os.path` binds `os`, so using `os` uses the import.
    CHECK(!Reports({"import os.path", "print(os.path.sep)"}, "unused-import"));
    CHECK(!Reports({"import numpy as np", "print(np)"}, "unused-import"));
    // The conventions that make an unused import deliberate.
    CHECK(!Reports({"from __future__ import annotations"}, "unused-import"));
    CHECK(!Reports({"import os", "__all__ = ['os']"}, "unused-import"));
    PythonLspOptions package_init = NoFiles();
    package_init.file_name = "__init__.py";
    CHECK(!HasCode(PythonLspDiagnostics({"from .core import Engine"}, package_init), "unused-import"));

    CHECK(Reports({"def f():", "    total = 1", "    return 2"}, "unused-variable"));
    CHECK(!Reports({"def f():", "    total = 1", "    return total"}, "unused-variable"));
    CHECK(!Reports({"total = 1"}, "unused-variable"));  // module level is a namespace, not a scratchpad
    CHECK(!Reports({"def f():", "    _unused = 1", "    return 2"}, "unused-variable"));
    CHECK(!Reports({"def f(pair):", "    a, b = pair", "    return b"}, "unused-variable"));
    CHECK(!Reports({"def f(x):", "    if x:", "        n = 1", "    else:", "        n = 2", "    return n"},
                   "unused-variable"));
    CHECK(!Reports({"def f():", "    global cache", "    cache = 1"}, "unused-variable"));
    CHECK(!Reports({"def f(x):", "    x += 1", "    return x"}, "unused-variable"));
}

void TestRedefinition() {
    CHECK(Reports({"def f():", "    pass", "", "def f():", "    pass"}, "redefinition"));
    CHECK(Reports({"import json", "import json"}, "redefinition"));
    CHECK(!Reports({"def f():", "    pass", "", "print(f)", "", "def f():", "    pass"}, "redefinition"));
    // The shapes that redefine a name on purpose.
    CHECK(!Reports({"try:", "    import ujson as json", "except ImportError:", "    import json", "print(json)"},
                   "redefinition"));
    CHECK(!Reports({"import os", "import os.path", "print(os.path)"}, "redefinition"));
    CHECK(!Reports({"class C:", "    @property", "    def v(self):", "        return 1", "",
                    "    @v.setter", "    def v(self, x):", "        pass"},
                   "redefinition"));
    CHECK(!Reports({"from typing import overload", "@overload", "def f(x: int) -> int: ...", "@overload",
                    "def f(x: str) -> str: ...", "def f(x):", "    return x"},
                   "redefinition"));
    CHECK(!Reports({"x = 1", "x = 2"}, "redefinition"));  // a variable is not a definition
}

void TestIdiomDiagnostics() {
    CHECK(Reports({"def f(items=[]):", "    return items"}, "mutable-default"));
    CHECK(Reports({"def f(items=dict()):", "    return items"}, "mutable-default"));
    CHECK(!Reports({"def f(items=None):", "    return items"}, "mutable-default"));
    CHECK(!Reports({"def f(items=()):", "    return items"}, "mutable-default"));

    CHECK(Reports({"x = 1", "if x is 'a':", "    pass"}, "is-literal"));
    CHECK(Reports({"x = 1", "if x is 3:", "    pass"}, "is-literal"));
    CHECK(!Reports({"x = 1", "if x is None:", "    pass"}, "is-literal"));
    CHECK(Reports({"x = 1", "if x == None:", "    pass"}, "compare-to-singleton"));
    CHECK(!Reports({"x = 1", "if x == 3:", "    pass"}, "compare-to-singleton"));
    CHECK(Reports({"x = 1", "if type(x) == type(1):", "    pass"}, "type-comparison"));

    CHECK(Reports({"try:", "    pass", "except:", "    pass"}, "bare-except"));
    CHECK(!Reports({"try:", "    pass", "except Exception:", "    pass"}, "bare-except"));
    CHECK(Reports({"try:", "    pass", "except OSError:", "    pass", "except OSError:", "    pass"},
                  "duplicate-except"));

    CHECK(Reports({"x = 1", "assert (x, 'message')"}, "assert-tuple"));
    CHECK(!Reports({"x = 1", "assert x, 'message'"}, "assert-tuple"));

    CHECK(Reports({"d = {'a': 1, 'a': 2}"}, "duplicate-key"));
    CHECK(!Reports({"d = {'a': 1, 'b': 2}"}, "duplicate-key"));
    CHECK(!Reports({"k = 'a'", "d = {k: 1, 'a': 2}"}, "duplicate-key"));

    CHECK(Reports({"def f(a, a):", "    return a"}, "duplicate-param"));
    CHECK(Reports({"def f():", "    return 1", "    print('dead')"}, "unreachable"));
    CHECK(!Reports({"def f(x):", "    if x:", "        return 1", "    return 2"}, "unreachable"));

    CHECK(Reports({"x = f''"}, "f-string-without-placeholders"));
    CHECK(!Reports({"x = 1", "y = f'{x}'"}, "f-string-without-placeholders"));

    CHECK(Reports({"def f():", "    raise NotImplemented"}, "raise-not-implemented"));
    CHECK(!Reports({"def f():", "    raise NotImplementedError"}, "raise-not-implemented"));

    CHECK(Reports({"def f(a, b):", "    a == b"}, "no-effect"));
    CHECK(!Reports({"def f(a):", "    a.method()"}, "no-effect"));
    CHECK(!Reports({"def f():", "    '''A docstring is not a dead expression.'''", "    ..."}, "no-effect"));
    // The pytest idiom whose whole point is an expression that raises.
    CHECK(!Reports({"import pytest", "def test(a, b):", "    with pytest.raises(TypeError):", "        a + b"},
                   "no-effect"));

    CHECK(Reports({"def f():", "    try:", "        return 1", "    finally:", "        return 2"},
                  "return-in-finally"));
    CHECK(Reports({"list = [1]", "print(list)"}, "shadow-builtin"));
    CHECK(!Reports({"class C:", "    def print(self):", "        pass"}, "shadow-builtin"));
    CHECK(Reports({"l = 1", "print(l)"}, "ambiguous-name"));
}

void TestStructuralDiagnostics() {
    CHECK(Reports({"return 1"}, "return-outside-function"));
    CHECK(Reports({"yield 1"}, "yield-outside-function"));
    CHECK(Reports({"break"}, "outside-loop"));
    CHECK(Reports({"nonlocal x"}, "nonlocal-at-module"));
    CHECK(Reports({"def f():", "    await g()"}, "await-outside-async"));
    CHECK(!Reports({"async def f():", "    await g()"}, "await-outside-async"));
    CHECK(!Reports({"def f():", "    for i in range(3):", "        break", "    return i"}, "outside-loop"));
    CHECK(!Reports({"def gen():", "    yield 1"}, "yield-outside-function"));

    CHECK(Reports({"class C:", "    def m():", "        pass"}, "no-self"));
    CHECK(!Reports({"class C:", "    @staticmethod", "    def m():", "        pass"}, "no-self"));
    CHECK(!Reports({"class C:", "    def m(self):", "        pass"}, "no-self"));
    CHECK(Reports({"class C:", "    def m(this):", "        pass"}, "self-name"));
    CHECK(!Reports({"class C:", "    @classmethod", "    def m(cls):", "        pass"}, "self-name"));
    CHECK(!Reports({"class C:", "    def __new__(cls):", "        pass"}, "self-name"));
    CHECK(!Reports({"def free(value):", "    return value"}, "self-name"));
    CHECK(Reports({"class C:", "    def __init__(self):", "        return self"}, "init-returns-value"));
}

void TestDiagnosticPositions() {
    const std::vector<PythonLspDiagnostic> diags = Lint({"import os", "x = nope"});
    const PythonLspDiagnostic undefined = FindCode(diags, "undefined-name");
    CHECK(undefined.line == 1);
    CHECK(undefined.col_start == 4);
    CHECK(undefined.col_end == 8);
    CHECK(undefined.severity == PythonLspSeverity::Error);
    const PythonLspDiagnostic unused = FindCode(diags, "unused-import");
    CHECK(unused.line == 0);
    CHECK(unused.severity == PythonLspSeverity::Warning);
    // Ascending order, so a client walking the list walks the file.
    for (size_t i = 1; i < diags.size(); i++) {
        CHECK(diags[i - 1].line <= diags[i].line);
    }
    // The line-length hint is opt-in, and off by default.
    const Lines wide = {std::string(50, 'x') + " = 1"};
    CHECK(!Reports(wide, "line-too-long"));
    PythonLspOptions narrow = NoFiles();
    narrow.max_line_length = 20;
    CHECK(HasCode(PythonLspDiagnostics(wide, narrow), "line-too-long"));
}

void TestEmptyAndOddDocuments() {
    CHECK(Lint({}).empty());
    CHECK(Lint({""}).empty());
    CHECK(Lint({"", "", ""}).empty());
    CHECK(Lint({"# just a comment"}).empty());
    CHECK(PythonLspSymbols({}).empty());
    CHECK(PythonLspFolds({}).empty());
    CHECK(!PythonLspHover({}, 0, 0).found);
    CHECK(Complete({}, 0, 0).empty() || true);  // no crash is the requirement
    CHECK(!PythonLspDefinition({}, 0, 0, NoFiles()).found);
    CHECK(!PythonLspReferences({}, 0, 0).found);
    CHECK(!PythonLspSignatureHelp({}, 0, 0).found);
    // Out-of-range positions are a client bug, not a server crash.
    CHECK(Complete({"x = 1"}, 99, 0).empty());
    CHECK(!PythonLspHover({"x = 1"}, 99, 99).found);
    CHECK(!PythonLspHover({"x = 1"}, 0, 99).found);
}

// --- Completion -------------------------------------------------------

void TestCompletion() {
    const Lines doc = {
        "import os",                       // 0
        "import json",                     // 1
        "from collections import deque",   // 2
        "",                                // 3
        "",                                // 4
        "class Shape:",                    // 5
        "    sides = 0",                   // 6
        "",                                // 7
        "    def __init__(self, name):",   // 8
        "        self.name = name",        // 9
        "",                                // 10
        "    def area(self):",             // 11
        "        return 0.0",              // 12
        "",                                // 13
        "",                                // 14
        "def build(count):",               // 15
        "    shapes = []",                 // 16
        "    text = 'hello'",              // 17
        "    return shapes, text, count",  // 18
    };
    // A plain prefix offers what is in scope, then builtins, then
    // keywords -- and filters on what has been typed.
    const std::vector<PythonLspCompletionItem> plain = Complete(doc, 18, 11);
    CHECK(Offers(plain, "shapes"));
    CHECK(Offers(plain, "count"));
    CHECK(Offers(plain, "build"));
    CHECK(Offers(plain, "Shape"));
    CHECK(Offers(plain, "sorted"));
    CHECK(Offers(plain, "return"));
    CHECK(!Offers(plain, "sides"));  // a class variable is not in a function's scope

    const Lines typing = {"import os", "def f():", "    ret"};
    const std::vector<PythonLspCompletionItem> filtered = Complete(typing, 2, 7);
    CHECK(Offers(filtered, "return"));
    CHECK(!Offers(filtered, "os"));
    CHECK(filtered.front().replace_start == 4);
    CHECK(filtered.front().replace_end == 7);

    // A module's members come from the server's own table, so `os.`
    // works with no Python installed anywhere.
    const Lines module_dot = {"import os", "x = os."};
    const std::vector<PythonLspCompletionItem> members = Complete(module_dot, 1, 7);
    CHECK(Offers(members, "getcwd"));
    CHECK(Offers(members, "environ"));
    CHECK(!Offers(members, "print"));
    const Lines nested_dot = {"import os", "x = os.path."};
    CHECK(Offers(Complete(nested_dot, 1, 12), "join"));
    const Lines from_import = {"from json import "};
    CHECK(Offers(Complete(from_import, 0, 17), "dumps"));
    const Lines import_line = {"import "};
    CHECK(Offers(Complete(import_line, 0, 7), "itertools"));

    // `self.` sees the class's own members and its bases'.
    const Lines self_dot = {"class Base:", "    def shared(self):", "        pass", "", "class C(Base):",
                            "    def __init__(self):", "        self.field = 1", "", "    def m(self):",
                            "        return self."};
    const std::vector<PythonLspCompletionItem> self_members = Complete(self_dot, 9, 20);
    CHECK(Offers(self_members, "field"));
    CHECK(Offers(self_members, "m"));
    CHECK(Offers(self_members, "shared"));

    // A local whose type is obvious from its assignment gets that type's
    // methods.
    const Lines str_dot = {"def f():", "    text = 'hello'", "    return text."};
    CHECK(Offers(Complete(str_dot, 2, 16), "upper"));
    const Lines instance_dot = {"class C:", "    def m(self):", "        pass", "", "def f():", "    c = C()",
                                "    return c."};
    CHECK(Offers(Complete(instance_dot, 6, 13), "m"));

    // Context-specific lists.
    CHECK(Offers(Complete({"raise "}, 0, 6), "ValueError"));
    CHECK(Offers(Complete({"class C:", "    def "}, 1, 8), "__init__"));
    CHECK(Offers(Complete({"@"}, 0, 1), "property"));

    // Prose is not code.
    CHECK(Complete({"x = 1  # a comment about x"}, 0, 20).empty());
    CHECK(Complete({"x = 'some string value'"}, 0, 14).empty());
    // ... but an f-string's replacement field is.
    const Lines fstring = {"value = 1", "text = f'{val}'"};
    CHECK(Offers(Complete(fstring, 1, 13), "value"));

    // Every item is single-line: mep's client splices insert_text into
    // one buffer line (see python_lsp.h).
    for (const PythonLspCompletionItem &item : plain) {
        CHECK(item.insert_text.find('\n') == std::string::npos);
        CHECK(!item.label.empty());
    }
}

// --- Hover ------------------------------------------------------------

void TestHover() {
    const Lines doc = {
        "import os",                                  // 0
        "",                                           // 1
        "",                                           // 2
        "def greet(name, punctuation='!'):",          // 3
        "    \"\"\"Say hello to someone.\"\"\"",       // 4
        "    return 'hi ' + name + punctuation",      // 5
        "",                                           // 6
        "",                                           // 7
        "class Greeter:",                             // 8
        "    \"\"\"Greets people.\"\"\"",              // 9
        "",                                           // 10
        "    def __init__(self):",                    // 11
        "        self.count = 0",                     // 12
        "",                                           // 13
        "",                                           // 14
        "message = greet('world')",                   // 15
        "path = os.sep",                              // 16
    };
    const PythonLspHoverInfo fn = PythonLspHover(doc, 15, 11);
    CHECK(fn.found);
    CHECK(fn.text.find("def greet(name, punctuation='!')") != std::string::npos);
    CHECK(fn.text.find("Say hello to someone.") != std::string::npos);
    CHECK(fn.line == 15);
    CHECK(fn.col_start == 10);
    CHECK(fn.col_end == 15);

    const PythonLspHoverInfo param = PythonLspHover(doc, 5, 22);
    CHECK(param.found);
    CHECK(param.text.find("parameter name") != std::string::npos);

    const PythonLspHoverInfo cls = PythonLspHover({"class C:", "    pass", "x = C()"}, 2, 4);
    CHECK(cls.found);
    CHECK(cls.text.find("class C") != std::string::npos);

    // A builtin, a keyword and a module member all answer from the
    // server's own tables.
    const PythonLspHoverInfo builtin = PythonLspHover({"x = len([1])"}, 0, 5);
    CHECK(builtin.found);
    CHECK(builtin.text.find("len") != std::string::npos);
    const PythonLspHoverInfo keyword = PythonLspHover({"def f():", "    return 1"}, 1, 6);
    CHECK(keyword.found);
    CHECK(keyword.text.find("keyword") != std::string::npos);
    const PythonLspHoverInfo member = PythonLspHover(doc, 16, 11);
    CHECK(member.found);

    // Nothing to say is said with silence, not with a guess.
    CHECK(!PythonLspHover({"x = 1  # comment"}, 0, 12).found);
    CHECK(!PythonLspHover({"import mystery", "mystery.thing"}, 1, 10).found);
}

// --- Symbols and folding ----------------------------------------------

void TestSymbols() {
    const Lines doc = {
        "CONSTANT = 1",                 // 0
        "variable = 2",                 // 1
        "",                             // 2
        "",                             // 3
        "class Outer:",                 // 4
        "    field: int = 0",           // 5
        "",                             // 6
        "    def method(self, x):",     // 7
        "        local = x",            // 8
        "        return local",         // 9
        "",                             // 10
        "    class Inner:",             // 11
        "        pass",                 // 12
        "",                             // 13
        "",                             // 14
        "async def worker():",          // 15
        "    return 1",                 // 16
    };
    const std::vector<PythonLspSymbol> syms = PythonLspSymbols(doc);
    CHECK(HasSymbol(syms, "CONSTANT"));
    CHECK(HasSymbol(syms, "Outer"));
    CHECK(HasSymbol(syms, "method"));
    CHECK(HasSymbol(syms, "Inner"));
    CHECK(HasSymbol(syms, "worker"));
    CHECK(!HasSymbol(syms, "local"));  // a function's locals are not the outline

    const PythonLspSymbol constant = FindSymbol(syms, "CONSTANT");
    CHECK(constant.kind == 14);  // SymbolKind.Constant
    CHECK(FindSymbol(syms, "variable").kind == 13);
    const PythonLspSymbol outer = FindSymbol(syms, "Outer");
    CHECK(outer.kind == 5);
    CHECK(outer.parent == -1);
    CHECK(outer.line_start == 4);
    CHECK(outer.line_end == 12);  // the class ends with its last statement, not on the next one
    const PythonLspSymbol method = FindSymbol(syms, "method");
    CHECK(method.kind == 6);  // SymbolKind.Method
    CHECK(method.detail == "(self, x)");
    CHECK(method.parent >= 0);
    CHECK(syms[static_cast<size_t>(method.parent)].name == "Outer");
    CHECK(method.sel_line == 7);
    CHECK(method.sel_col_start == 8);
    CHECK(FindSymbol(syms, "worker").detail.rfind("async", 0) == 0);
    // A definition guarded by `if TYPE_CHECKING:` is still a definition.
    CHECK(HasSymbol(PythonLspSymbols({"if True:", "    def guarded():", "        pass"}), "guarded"));
}

void TestFolding() {
    const Lines doc = {
        "import os",          // 0
        "import sys",         // 1
        "",                   // 2
        "# a comment",        // 3
        "# continued",        // 4
        "def f(x):",          // 5
        "    if x:",          // 6
        "        return 1",   // 7
        "    else:",          // 8
        "        return 2",   // 9
        "",                   // 10
        "text = '''",         // 11
        "multi",              // 12
        "line'''",            // 13
    };
    const std::vector<PythonLspFold> folds = PythonLspFolds(doc);
    bool imports = false, comment = false, function = false, branch = false, string_fold = false;
    for (const PythonLspFold &f : folds) {
        if (f.kind == "imports" && f.start_line == 0 && f.end_line == 1) imports = true;
        if (f.kind == "comment" && f.start_line == 3 && f.end_line == 4) comment = true;
        if (f.start_line == 5 && f.end_line == 9) function = true;
        if (f.start_line == 6 && f.end_line == 7) branch = true;
        if (f.start_line == 11 && f.end_line == 13) string_fold = true;
    }
    CHECK(imports);
    CHECK(comment);
    CHECK(function);
    CHECK(branch);
    CHECK(string_fold);
    for (const PythonLspFold &f : folds) {
        CHECK(f.end_line > f.start_line);  // a one-line fold is not a fold
    }
    // A single comment line is not a run.
    for (const PythonLspFold &f : PythonLspFolds({"# alone", "x = 1"})) {
        CHECK(f.kind != "comment");
    }
}

// --- Definition, references, signature help ---------------------------

void TestDefinition() {
    const Lines doc = {
        "def helper():",       // 0
        "    return 1",        // 1
        "",                    // 2
        "",                    // 3
        "class Thing:",        // 4
        "    def method(self):",  // 5
        "        self.value = 1",  // 6
        "",                    // 7
        "    def use(self):",  // 8
        "        return self.value",  // 9
        "",                    // 10
        "",                    // 11
        "def main(arg):",      // 12
        "    result = helper()",  // 13
        "    return result, arg",  // 14
    };
    const PythonLspLocation fn = PythonLspDefinition(doc, 13, 15, NoFiles());
    CHECK(fn.found);
    CHECK(fn.path.empty());  // this document
    CHECK(fn.line == 0);
    CHECK(fn.col == 4);

    const PythonLspLocation local = PythonLspDefinition(doc, 14, 12, NoFiles());
    CHECK(local.found);
    CHECK(local.line == 13);

    const PythonLspLocation param = PythonLspDefinition(doc, 14, 21, NoFiles());
    CHECK(param.found);
    CHECK(param.line == 12);

    // An attribute resolves through `self` to where the class sets it.
    const PythonLspLocation attr = PythonLspDefinition(doc, 9, 21, NoFiles());
    CHECK(attr.found);
    CHECK(attr.line == 6);

    CHECK(!PythonLspDefinition(doc, 2, 0, NoFiles()).found);
    CHECK(!PythonLspDefinition({"import mystery", "mystery.thing()"}, 1, 10, NoFiles()).found);
}

void TestReferences() {
    const Lines doc = {
        "value = 1",            // 0
        "",                     // 1
        "",                     // 2
        "def f():",             // 3
        "    value = 2",        // 4
        "    return value",     // 5
        "",                     // 6
        "",                     // 7
        "print(value)",         // 8
    };
    // The local `value` and the module-level one are different bindings,
    // and a rename of one must not touch the other.
    const PythonLspReferenceSet local = PythonLspReferences(doc, 5, 11);
    CHECK(local.found);
    CHECK(local.name == "value");
    CHECK(local.refs.size() == 2);
    CHECK(local.refs[0].line == 4);
    CHECK(local.refs[0].is_write);
    CHECK(local.refs[1].line == 5);
    CHECK(!local.refs[1].is_write);
    CHECK(local.rename_blocked_reason.empty());

    const PythonLspReferenceSet global_refs = PythonLspReferences(doc, 8, 6);
    CHECK(global_refs.refs.size() == 2);
    CHECK(global_refs.refs[0].line == 0);
    CHECK(global_refs.refs[1].line == 8);

    // A builtin has no binding in this file, so renaming it is refused.
    const PythonLspReferenceSet builtin = PythonLspReferences({"print(len([1]))"}, 0, 7);
    CHECK(builtin.found);
    CHECK(!builtin.rename_blocked_reason.empty());
    CHECK(!PythonLspReferences({"x = 1"}, 0, 4).found);
}

void TestSignatureHelp() {
    const Lines doc = {
        "def connect(host, port=8080, timeout=None):",  // 0
        "    \"\"\"Open a connection.\"\"\"",            // 1
        "    return host, port, timeout",               // 2
        "",                                             // 3
        "",                                             // 4
        "class Client:",                                // 5
        "    def __init__(self, url, retries=3):",      // 6
        "        self.url = url",                       // 7
        "",                                             // 8
        "    def send(self, payload):",                 // 9
        "        return payload",                       // 10
        "",                                             // 11
        "",                                             // 12
        "conn = connect('localhost', 80)",              // 13
        "client = Client('http://x')",                  // 14
    };
    const PythonLspSignature first = PythonLspSignatureHelp(doc, 13, 16);
    CHECK(first.found);
    CHECK(first.label == "connect(host, port=8080, timeout=None)");
    CHECK(first.params.size() == 3);
    CHECK(first.active_param == 0);
    CHECK(first.documentation.find("Open a connection.") != std::string::npos);

    const PythonLspSignature second = PythonLspSignatureHelp(doc, 13, 29);
    CHECK(second.found);
    CHECK(second.active_param == 1);

    // A keyword argument jumps the highlight to that parameter.
    const Lines keyworded = {"def f(a, b=1, c=2):", "    return a, b, c", "f(1, c=)"};
    const PythonLspSignature kw = PythonLspSignatureHelp(keyworded, 2, 7);
    CHECK(kw.found);
    CHECK(kw.active_param == 2);

    // A class call shows its __init__ without the `self` the caller does
    // not pass.
    const PythonLspSignature ctor = PythonLspSignatureHelp(doc, 14, 25);
    CHECK(ctor.found);
    CHECK(ctor.params.size() == 2);
    CHECK(ctor.params[0] == "url");

    // A method through an instance, a builtin, and a module function.
    const Lines method_call = {"class C:", "    def m(self, x):", "        return x", "c = C()", "c.m(1)"};
    const PythonLspSignature method = PythonLspSignatureHelp(method_call, 4, 4);
    CHECK(method.found);
    CHECK(method.params.size() == 1);
    const PythonLspSignature builtin = PythonLspSignatureHelp({"x = sorted([1])"}, 0, 12);
    CHECK(builtin.found);
    CHECK(builtin.label.rfind("sorted(", 0) == 0);
    const PythonLspSignature module_fn = PythonLspSignatureHelp({"import os", "p = os.path.join('a', 'b')"}, 1, 18);
    CHECK(module_fn.found);

    // Not in a call at all.
    CHECK(!PythonLspSignatureHelp(doc, 2, 4).found);
    CHECK(!PythonLspSignatureHelp({"x = [1, 2]"}, 0, 8).found);
}

// --- Vocabulary -------------------------------------------------------

void TestVocabulary() {
    CHECK(!PythonLspKeywordVocab().empty());
    CHECK(!PythonLspBuiltinVocab().empty());
    CHECK(!PythonLspExceptionVocab().empty());
    CHECK(!PythonLspDunderVocab().empty());
    CHECK(!PythonLspModuleVocab().empty());
    CHECK(PythonLspIsBuiltin("len"));
    CHECK(PythonLspIsBuiltin("ValueError"));
    CHECK(!PythonLspIsBuiltin("numpy"));
    CHECK(!PythonLspIsBuiltin("self"));  // a convention, not a builtin
    CHECK(PythonLspModuleMembers("os") != nullptr);
    CHECK(PythonLspModuleMembers("os.path") != nullptr);
    CHECK(PythonLspModuleMembers("not_a_module") == nullptr);
    CHECK(PythonLspTypeMembers("str") != nullptr);
    CHECK(PythonLspTypeMembers("MyClass") == nullptr);
    // Every table is binary-searched, so every table must be sorted, and
    // every entry must have its three strings.
    const std::vector<const std::vector<PythonLspVocabEntry> *> tables = {
        &PythonLspKeywordVocab(),   &PythonLspBuiltinVocab(),    &PythonLspExceptionVocab(),
        &PythonLspDunderVocab(),    &PythonLspModuleVocab(),     PythonLspModuleMembers("os"),
        PythonLspTypeMembers("str"),
    };
    for (const std::vector<PythonLspVocabEntry> *table : tables) {
        CHECK(table != nullptr);
        for (size_t i = 0; i < table->size(); i++) {
            CHECK((*table)[i].name != nullptr && (*table)[i].name[0] != '\0');
            CHECK((*table)[i].detail != nullptr);
            CHECK((*table)[i].doc != nullptr);
            if (i > 0) CHECK(std::string((*table)[i - 1].name) < std::string((*table)[i].name));
        }
    }
}

// --- A whole realistic file -------------------------------------------

void TestRealisticFileIsQuiet() {
    // The point of this one is the absence of output: ordinary,
    // idiomatic Python must produce no diagnostics at all. Every false
    // positive this suite ever caught was caught here first.
    const Lines doc = {
        "\"\"\"A small module.\"\"\"",
        "from __future__ import annotations",
        "",
        "import json",
        "import os",
        "from dataclasses import dataclass, field",
        "from typing import Any",
        "",
        "DEFAULT_PATH = os.path.join('.', 'config.json')",
        "",
        "",
        "@dataclass",
        "class Config:",
        "    \"\"\"Loaded settings.\"\"\"",
        "",
        "    name: str",
        "    tags: list[str] = field(default_factory=list)",
        "    extra: dict[str, Any] | None = None",
        "",
        "    def label(self) -> str:",
        "        \"\"\"Return a display label.\"\"\"",
        "        suffix = ', '.join(self.tags)",
        "        return f'{self.name} ({suffix})' if suffix else self.name",
        "",
        "",
        "def load(path: str = DEFAULT_PATH) -> Config:",
        "    \"\"\"Read a config file.\"\"\"",
        "    try:",
        "        with open(path, encoding='utf-8') as handle:",
        "            raw = json.load(handle)",
        "    except OSError:",
        "        return Config(name='default')",
        "    tags = [str(tag) for tag in raw.get('tags', [])]",
        "    return Config(name=raw['name'], tags=tags, extra=raw.get('extra'))",
        "",
        "",
        "def summarize(configs: list[Config]) -> dict[str, int]:",
        "    \"\"\"Count tags across configs.\"\"\"",
        "    counts: dict[str, int] = {}",
        "    for config in configs:",
        "        for tag in config.tags:",
        "            counts[tag] = counts.get(tag, 0) + 1",
        "    return counts",
        "",
        "",
        "if __name__ == '__main__':",
        "    print(summarize([load()]))",
    };
    const std::vector<PythonLspDiagnostic> diags = Lint(doc);
    for (const PythonLspDiagnostic &d : diags) {
        std::fprintf(stderr, "unexpected diagnostic %d:%d %s: %s\n", d.line + 1, d.col_start, d.code.c_str(),
                     d.message.c_str());
    }
    CHECK(diags.empty());
    // ... and the features still have something to say about it.
    CHECK(HasSymbol(PythonLspSymbols(doc), "Config"));
    CHECK(HasSymbol(PythonLspSymbols(doc), "summarize"));
    CHECK(!PythonLspFolds(doc).empty());
    for (size_t i = 0; i < doc.size(); i++) {
        if (doc[i].find("print(summarize(") == std::string::npos) continue;
        const PythonLspHoverInfo call = PythonLspHover(doc, static_cast<int>(i), 12);
        CHECK(call.found);
        CHECK(call.text.find("Count tags across configs.") != std::string::npos);
    }
}

}  // namespace

int main() {
    TestTokenizer();
    TestParserCoverage();
    TestSyntaxDiagnostics();
    TestUndefinedName();
    TestUnusedBindings();
    TestRedefinition();
    TestIdiomDiagnostics();
    TestStructuralDiagnostics();
    TestDiagnosticPositions();
    TestEmptyAndOddDocuments();
    TestCompletion();
    TestHover();
    TestSymbols();
    TestFolding();
    TestDefinition();
    TestReferences();
    TestSignatureHelp();
    TestVocabulary();
    TestRealisticFileIsQuiet();
    std::printf("mep-python-lsp-test: all checks passed\n");
    return 0;
}
