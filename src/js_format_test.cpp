// mep's JavaScript formatter (Prettier 3.6.2 port), driven directly.
//
// `mep-js-format-test` runs the checks below;
// `mep-js-format-test --format [-w N] [--indent N] [--tabs] [--no-semi]
// [--single-quote] [--no-verify] [--js|--ts|--tsx] FILE...` prints each
// file formatted ('-' reads stdin), for tools/format_compare.py. The
// dialect comes from each file's name (.ts .mts .cts TypeScript, .tsx TSX,
// else JavaScript) unless a flag names one.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "js_format.h"

namespace {
int g_checks = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
    std::abort();
}
#define CHECK(c) Check((c), #c, __LINE__)

int FormatFiles(int argc, char **argv) {
    jsfmt::Options opts;
    int status = 0;
    const char *dialect = nullptr;
    for (int k = 2; k < argc; k++) {
        const std::string arg = argv[k];
        if (arg == "--js" || arg == "--ts" || arg == "--tsx") {
            dialect = arg == "--js" ? "x.js" : arg == "--ts" ? "x.ts" : "x.tsx";
            continue;
        }
        if ((arg == "-w" || arg == "--width") && k + 1 < argc) {
            opts.width = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--indent" && k + 1 < argc) {
            opts.indent = std::atoi(argv[++k]);
            continue;
        }
        if (arg == "--tabs") {
            opts.use_tabs = true;
            continue;
        }
        if (arg == "--no-semi") {
            opts.semi = false;
            continue;
        }
        if (arg == "--single-quote") {
            opts.single_quote = true;
            continue;
        }
        if (arg == "--no-verify") {
            opts.verify = false;
            continue;
        }
        std::stringstream ss;
        if (arg == "-") {
            ss << std::cin.rdbuf();
        } else {
            std::ifstream in(arg, std::ios::binary);
            ss << in.rdbuf();
        }
        jsfmt::SetDialectForPath(opts, dialect ? dialect : arg.c_str());
        const jsfmt::Result r = jsfmt::Format(ss.str(), opts);
        if (!r.ok) {
            std::fprintf(stderr, "%s:%d: %s\n", arg.c_str(), r.error_line, r.error.c_str());
            status = 1;
            continue;
        }
        std::fwrite(r.text.data(), 1, r.text.size(), stdout);
    }
    return status;
}

// Formats `in` and checks the result is `want` (prettier 3.6.2's output for
// the same input) and is itself a fixed point.
bool Formats(const std::string &in, const std::string &want, const jsfmt::Options &opts = {}) {
    const jsfmt::Result r = jsfmt::Format(in, opts);
    if (!r.ok || r.text != want) {
        std::fprintf(stderr, "input:\n%s\ngot (ok=%d %s):\n%s\nwant:\n%s\n", in.c_str(), r.ok ? 1 : 0,
                     r.error.c_str(), r.text.c_str(), want.c_str());
        return false;
    }
    const jsfmt::Result again = jsfmt::Format(r.text, opts);
    if (!again.ok || again.text != r.text) {
        std::fprintf(stderr, "not idempotent:\n%s\nthen:\n%s\n", r.text.c_str(), again.text.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);

    const jsfmt::Options defaults;
    CHECK(defaults.width == 80);
    CHECK(defaults.indent == 2);
    CHECK(!defaults.use_tabs);
    CHECK(defaults.semi);
    CHECK(!defaults.single_quote);

    CHECK(Formats("let a = 'b'\n", "let a = \"b\";\n"));

    // quotes
    CHECK(Formats("a = 'it\\'s'; b = \"say \\\"hi\\\"\"; c = 'mixed \" \\''\n",
                  "a = \"it's\";\n"
            "b = 'say \"hi\"';\n"
            "c = \"mixed \\\" '\";\n"));
    // numbers
    CHECK(Formats("x = [0XAB, 1.50, .5, 5., 1E5, 0.0e-10, 0b11, 1_000n, 0.50e+10];\n",
                  "x = [0xab, 1.5, 0.5, 5, 1e5, 0.0e-10, 0b11, 1_000n, 0.5e10];\n"));
    // object
    CHECK(Formats("const o = {a:1, 'b':2, [c]:3, d, ...e, f(){}, get g(){return 1}, async *h(){}}\n",
                  "const o = {\n"
            "  a: 1,\n"
            "  b: 2,\n"
            "  [c]: 3,\n"
            "  d,\n"
            "  ...e,\n"
            "  f() {},\n"
            "  get g() {\n"
            "    return 1;\n"
            "  },\n"
            "  async *h() {},\n"
            "};\n"));
    // object-quoted-keys
    CHECK(Formats("o = {'a': 1, 'b-c': 2}; p = {'a': 1, b: 2}; q = {1: 1, 'x': 2}\n",
                  "o = { a: 1, \"b-c\": 2 };\n"
            "p = { a: 1, b: 2 };\n"
            "q = { 1: 1, x: 2 };\n"));
    // object-preserve-break
    CHECK(Formats("const o = {\n"
            "  a: 1, b: 2 };\n"
            "const p = { a: 1,\n"
            " b: 2 };\n",
                  "const o = {\n"
            "  a: 1,\n"
            "  b: 2,\n"
            "};\n"
            "const p = { a: 1, b: 2 };\n"));
    // array-fill
    CHECK(Formats("const nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26];\n",
                  "const nums = [\n"
            "  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,\n"
            "  23, 24, 25, 26,\n"
            "];\n"));
    // array-objects
    CHECK(Formats("x = [{a: 1, b: 2}, {a: 3, b: 4}]; y = [[1,2],[3,4]]; z = [,, 1,]\n",
                  "x = [\n"
            "  { a: 1, b: 2 },\n"
            "  { a: 3, b: 4 },\n"
            "];\n"
            "y = [\n"
            "  [1, 2],\n"
            "  [3, 4],\n"
            "];\n"
            "z = [, , 1];\n"));
    // call-break
    CHECK(Formats("someFunction(argumentNumberOne, argumentNumberTwo, argumentNumberThree, four);\n",
                  "someFunction(argumentNumberOne, argumentNumberTwo, argumentNumberThree, four);\n"));
    // call-last-arg
    CHECK(Formats("useEffect(() => { doSomething(); return () => cleanup(); }, [dependencyOne, two]);\n",
                  "useEffect(() => {\n"
            "  doSomething();\n"
            "  return () => cleanup();\n"
            "}, [dependencyOne, two]);\n"));
    // call-first-arg
    CHECK(Formats("setTimeout(function () { thing(); }, 500);\n",
                  "setTimeout(function () {\n"
            "  thing();\n"
            "}, 500);\n"));
    // arrow
    CHECK(Formats("const f = a => a * 2; const g = (a, b) => ({a, b}); const h = async () => { await x };\n",
                  "const f = (a) => a * 2;\n"
            "const g = (a, b) => ({ a, b });\n"
            "const h = async () => {\n"
            "  await x;\n"
            "};\n"));
    // arrow-chain
    CHECK(Formats("const curried = (a) => (b) => (c) => (d) => a + b + c + d + someOtherLongValue + more;\n",
                  "const curried = (a) => (b) => (c) => (d) =>\n"
            "  a + b + c + d + someOtherLongValue + more;\n"));
    // member-chain
    CHECK(Formats("const result = items.filter(item => item.active).map(item => item.value).reduce((a, b) => a + b, 0);\n",
                  "const result = items\n"
            "  .filter((item) => item.active)\n"
            "  .map((item) => item.value)\n"
            "  .reduce((a, b) => a + b, 0);\n"));
    // member-short
    CHECK(Formats("z.object({ a: z.string() }); this.x.y.z(); wrapper.find('SomeSelector').prop('children')(1);\n",
                  "z.object({ a: z.string() });\n"
            "this.x.y.z();\n"
            "wrapper.find(\"SomeSelector\").prop(\"children\")(1);\n"));
    // promise-chain
    CHECK(Formats("fetch(url).then(res => res.json()).then(data => console.log(data)).catch(err => handle(err));\n",
                  "fetch(url)\n"
            "  .then((res) => res.json())\n"
            "  .then((data) => console.log(data))\n"
            "  .catch((err) => handle(err));\n"));
    // binary
    CHECK(Formats("const isValid = someCondition && anotherCondition || yetAnotherCondition && theFinalCondition;\n",
                  "const isValid =\n"
            "  (someCondition && anotherCondition) ||\n"
            "  (yetAnotherCondition && theFinalCondition);\n"));
    // binary-mixed
    CHECK(Formats("x = a ** b ** c; y = (a + b) * c; z = a % b * c; w = a == b != c; v = (a, b);\n",
                  "x = a ** (b ** c);\n"
            "y = (a + b) * c;\n"
            "z = (a % b) * c;\n"
            "w = (a == b) != c;\n"
            "v = (a, b);\n"));
    // ternary
    CHECK(Formats("const message = isError \? 'An error occurred while processing' : isWarning \? 'Warning issued' : 'All good';\n",
                  "const message = isError\n"
            "  \? \"An error occurred while processing\"\n"
            "  : isWarning\n"
            "    \? \"Warning issued\"\n"
            "    : \"All good\";\n"));
    // ternary-short
    CHECK(Formats("x = a \? b : c\n",
                  "x = a \? b : c;\n"));
    // assignment-break
    CHECK(Formats("const someVeryLongVariableNameForTesting = anotherVeryLongFunctionName(argument) || {};\n",
                  "const someVeryLongVariableNameForTesting =\n"
            "  anotherVeryLongFunctionName(argument) || {};\n"));
    // assignment-chain
    CHECK(Formats("a = b = c = d;\n"
            "window.FooClient.setVars({ locale: getFooLocale({ page }), authorizationToken: data.token });\n",
                  "a = b = c = d;\n"
            "window.FooClient.setVars({\n"
            "  locale: getFooLocale({ page }),\n"
            "  authorizationToken: data.token,\n"
            "});\n"));
    // template
    CHECK(Formats("const s = `hello ${name} and ${ a + b } and ${fn( x )}`;\n",
                  "const s = `hello ${name} and ${a + b} and ${fn(x)}`;\n"));
    // tagged (a tag prettier has no embedded formatter for)
    CHECK(Formats("const q = tag`query { a }`; tag2`<div>${ x }</div>`;\n",
                  "const q = tag`query { a }`;\n"
                  "tag2`<div>${x}</div>`;\n"));
    // class
    CHECK(Formats("class A extends B { static x = 1; #p = 2; constructor(a) { super(a) } static { init() } get #q() { return this.#p } }\n",
                  "class A extends B {\n"
            "  static x = 1;\n"
            "  #p = 2;\n"
            "  constructor(a) {\n"
            "    super(a);\n"
            "  }\n"
            "  static {\n"
            "    init();\n"
            "  }\n"
            "  get #q() {\n"
            "    return this.#p;\n"
            "  }\n"
            "}\n"));
    // class-heritage-break
    CHECK(Formats("class LoooooooooooooooooooooooooooongClassName extends AnotherVeryLongBaseClassName {}\n",
                  "class LoooooooooooooooooooooooooooongClassName extends AnotherVeryLongBaseClassName {}\n"));
    // functions
    CHECK(Formats("function f(a = 1, {b, c} = {}, ...rest) { return a }\n"
            "async function* g() { yield* other(); }\n",
                  "function f(a = 1, { b, c } = {}, ...rest) {\n"
            "  return a;\n"
            "}\n"
            "async function* g() {\n"
            "  yield* other();\n"
            "}\n"));
    // params-break
    CHECK(Formats("function foo(parameterNumberOne, parameterNumberTwo, parameterNumberThree, four) {}\n",
                  "function foo(\n"
            "  parameterNumberOne,\n"
            "  parameterNumberTwo,\n"
            "  parameterNumberThree,\n"
            "  four,\n"
            ") {}\n"));
    // return-paren
    CHECK(Formats("function f() { return someLongCondition && anotherLongCondition && yetAnotherCondition1; }\n",
                  "function f() {\n"
            "  return someLongCondition && anotherLongCondition && yetAnotherCondition1;\n"
            "}\n"));
    // if-else
    CHECK(Formats("if (a) b(); else if (c) d(); else { e() }\n"
            "if (x) {} else {}\n",
                  "if (a) b();\n"
            "else if (c) d();\n"
            "else {\n"
            "  e();\n"
            "}\n"
            "if (x) {\n"
            "} else {\n"
            "}\n"));
    // loops
    CHECK(Formats("for (let i = 0; i < n; i++) x(i); for (const k in o) {} for await (const v of it) {} while(1); do x(); while (y)\n"
            "for (;;) {}\n",
                  "for (let i = 0; i < n; i++) x(i);\n"
            "for (const k in o) {\n"
            "}\n"
            "for await (const v of it) {\n"
            "}\n"
            "while (1);\n"
            "do x();\n"
            "while (y);\n"
            "for (;;) {}\n"));
    // switch
    CHECK(Formats("switch (x) { case 1: case 2: a(); break; default: b() }\n",
                  "switch (x) {\n"
            "  case 1:\n"
            "  case 2:\n"
            "    a();\n"
            "    break;\n"
            "  default:\n"
            "    b();\n"
            "}\n"));
    // try
    CHECK(Formats("try { a() } catch { b() } finally { c() }\n"
            "try { a() } catch (e) {}\n",
                  "try {\n"
            "  a();\n"
            "} catch {\n"
            "  b();\n"
            "} finally {\n"
            "  c();\n"
            "}\n"
            "try {\n"
            "  a();\n"
            "} catch (e) {}\n"));
    // labels
    CHECK(Formats("outer: for (;;) { continue outer; break outer; }\n",
                  "outer: for (;;) {\n"
            "  continue outer;\n"
            "  break outer;\n"
            "}\n"));
    // modules
    CHECK(Formats("import a, {b as c, d} from 'x'; import * as ns from \"y\"; import 'z'; export {a, c as e}; export * from 'w'; export const k = 1; export default function () {}\n",
                  "import a, { b as c, d } from \"x\";\n"
            "import * as ns from \"y\";\n"
            "import \"z\";\n"
            "export { a, c as e };\n"
            "export * from \"w\";\n"
            "export const k = 1;\n"
            "export default function () {}\n"));
    // import-attrs
    CHECK(Formats("import json from './data.json' with { type: 'json' };\n",
                  "import json from \"./data.json\" with { type: \"json\" };\n"));
    // import-break
    CHECK(Formats("import { aaaaaaaaaaaaaaaa, bbbbbbbbbbbbbbbbbb, cccccccccccccccccc, dddddddddddd } from 'module';\n",
                  "import {\n"
            "  aaaaaaaaaaaaaaaa,\n"
            "  bbbbbbbbbbbbbbbbbb,\n"
            "  cccccccccccccccccc,\n"
            "  dddddddddddd,\n"
            "} from \"module\";\n"));
    // directives
    CHECK(Formats("'use strict'\n"
            "\"use client\";\n"
            "function f() { 'use asm'; return 1 }\n",
                  "\"use strict\";\n"
            "\"use client\";\n"
            "function f() {\n"
            "  \"use asm\";\n"
            "  return 1;\n"
            "}\n"));
    // comments-line
    CHECK(Formats("// leading\n"
            "const a = 1; // trailing\n"
            "\n"
            "/* block */ const b = 2;\n"
            "function f(/* no params */) {}\n",
                  "// leading\n"
            "const a = 1; // trailing\n"
            "\n"
            "/* block */ const b = 2;\n"
            "function f(/* no params */) {}\n"));
    // comments-jsdoc
    CHECK(Formats("/**\n"
            "   * Doc comment\n"
            "   * @param {string} a\n"
            "   */\n"
            "function f(a) {}\n",
                  "/**\n"
            " * Doc comment\n"
            " * @param {string} a\n"
            " */\n"
            "function f(a) {}\n"));
    // comments-dangling
    CHECK(Formats("const o = {\n"
            "  // nothing here\n"
            "};\n"
            "f(/* empty */);\n"
            "if (a) {\n"
            "  // todo\n"
            "}\n",
                  "const o = {\n"
            "  // nothing here\n"
            "};\n"
            "f(/* empty */);\n"
            "if (a) {\n"
            "  // todo\n"
            "}\n"));
    // comments-ignore
    CHECK(Formats("// prettier-ignore\n"
            "const   matrix = [1,0,0,\n"
            "   0,1,0];\n"
            "const  b = 1\n",
                  "// prettier-ignore\n"
            "const   matrix = [1,0,0,\n"
            "   0,1,0];\n"
            "const b = 1;\n"));
    // blank-lines
    CHECK(Formats("a()\n"
            "\n"
            "\n"
            "\n"
            "b()\n"
            "c()\n"
            "\n",
                  "a();\n"
            "\n"
            "b();\n"
            "c();\n"));
    // unary-new-await
    CHECK(Formats("x = !a; y = - -b; z = typeof c; w = void 0; new Foo; new (a.b()); delete o[k]; async function f() { await (a || b) }\n",
                  "x = !a;\n"
            "y = -(-b);\n"
            "z = typeof c;\n"
            "w = void 0;\n"
            "new Foo();\n"
            "new (a.b())();\n"
            "delete o[k];\n"
            "async function f() {\n"
            "  await (a || b);\n"
            "}\n"));
    // optional-chain
    CHECK(Formats("a\?.b\?.[c]\?.(d); (a\?.b).c; x \?\?= y; z ||= w; q &&= r;\n",
                  "a\?.b\?.[c]\?.(d);\n"
            "(a\?.b).c;\n"
            "x \?\?= y;\n"
            "z ||= w;\n"
            "q &&= r;\n"));
    // nullish
    CHECK(Formats("const v = a \?\? b; const w = (a || b) \?\? c;\n",
                  "const v = a \?\? b;\n"
            "const w = (a || b) \?\? c;\n"));
    // sequence-in-for
    CHECK(Formats("for (i = 0, j = 10; i < j; i++, j--) {}\n",
                  "for (i = 0, j = 10; i < j; i++, j--) {}\n"));
    // regex
    CHECK(Formats("const r = /ab+c/gi; s.replace(/\\s+/g, ' ');\n",
                  "const r = /ab+c/gi;\n"
            "s.replace(/\\s+/g, \" \");\n"));
    // asi-no-semi
    CHECK(Formats("const a = 1\n"
            ";[1, 2].forEach(f)\n"
            ";`x`.length\n"
            "let b = (c)\n",
                  "const a = 1;\n"
            "[1, 2].forEach(f);\n"
            "`x`.length;\n"
            "let b = c;\n"));
    // jsx-simple
    CHECK(Formats("const el = <div className='a'>Hello {name}</div>;\n",
                  "const el = <div className=\"a\">Hello {name}</div>;\n"));
    // jsx-multiline
    CHECK(Formats("const App = () => <div><Header title=\"x\" /><Main>{items.map(i => <Item key={i.id} {...i} />)}</Main></div>;\n",
                  "const App = () => (\n"
            "  <div>\n"
            "    <Header title=\"x\" />\n"
            "    <Main>\n"
            "      {items.map((i) => (\n"
            "        <Item key={i.id} {...i} />\n"
            "      ))}\n"
            "    </Main>\n"
            "  </div>\n"
            ");\n"));
    // jsx-text-fill
    CHECK(Formats("const p = <p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore.</p>;\n",
                  "const p = (\n"
            "  <p>\n"
            "    Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod\n"
            "    tempor incididunt ut labore et dolore.\n"
            "  </p>\n"
            ");\n"));
    // jsx-conditional
    CHECK(Formats("const x = cond \? <A /> : <B prop={value} other=\"string value here\" third={thing} />;\n",
                  "const x = cond \? (\n"
            "  <A />\n"
            ") : (\n"
            "  <B prop={value} other=\"string value here\" third={thing} />\n"
            ");\n"));
    // jsx-fragment
    CHECK(Formats("f(<><a/>{' '}<b/></>)\n",
                  "f(\n"
            "  <>\n"
            "    <a /> <b />\n"
            "  </>,\n"
            ");\n"));
    // jsx-attr-quotes
    CHECK(Formats("x = <a b='\"' c=\"it's\" d='plain' />\n",
                  "x = <a b='\"' c=\"it's\" d=\"plain\" />;\n"));
    // jsx-and
    CHECK(Formats("const y = loggedIn && <UserMenu user={currentUser} onLogout={handleLogout} items={x} />;\n",
                  "const y = loggedIn && (\n"
            "  <UserMenu user={currentUser} onLogout={handleLogout} items={x} />\n"
            ");\n"));
    // crlf-bom
    CHECK(Formats("\xef""\xbb""\xbf""const a = 1;\r\n"
            "const b = 2;\r\n",
                  "\xef""\xbb""\xbf""const a = 1;\n"
            "const b = 2;\n"));
    // empty
    CHECK(Formats("   \n"
            "\n",
                  ""));
    // comment-only
    CHECK(Formats("// just a comment\n",
                  "// just a comment\n"));
    // shebang
    CHECK(Formats("#!/usr/bin/env node\n"
            "console.log(1)\n",
                  "#!/usr/bin/env node\n"
            "console.log(1);\n"));
    // long-string-concat
    CHECK(Formats("const message = 'This is a very long string that ' + variable + ' continues here and goes on';\n",
                  "const message =\n"
            "  \"This is a very long string that \" + variable + \" continues here and goes on\";\n"));
    // object-destructure-default
    CHECK(Formats("const { a = 1, b: { c }, ...rest } = obj; const [x, , y = 2, ...z] = arr;\n",
                  "const {\n"
            "  a = 1,\n"
            "  b: { c },\n"
            "  ...rest\n"
            "} = obj;\n"
            "const [x, , y = 2, ...z] = arr;\n"));
    // arrow-body-ternary
    CHECK(Formats("const fn = (a) => a \? someValueThatIsLong : anotherValueThatIsAlsoQuiteLongIndeed + 1;\n",
                  "const fn = (a) =>\n"
            "  a \? someValueThatIsLong : anotherValueThatIsAlsoQuiteLongIndeed + 1;\n"));
    // new-expression-args
    CHECK(Formats("const instance = new SomeVeryLongClassNameForTesting(firstArgument, secondArgument, x);\n",
                  "const instance = new SomeVeryLongClassNameForTesting(\n"
            "  firstArgument,\n"
            "  secondArgument,\n"
            "  x,\n"
            ");\n"));
    // test-call
    CHECK(Formats("describe('some test suite name that is quite long indeed for this', () => { it('works', async () => { expect(1).toBe(1) }) })\n",
                  "describe(\"some test suite name that is quite long indeed for this\", () => {\n"
            "  it(\"works\", async () => {\n"
            "    expect(1).toBe(1);\n"
            "  });\n"
            "});\n"));
    // getter-setter-obj
    CHECK(Formats("const o = { get a() { return 1 }, set a(v) {}, [`k${1}`]: 2, async b() {}, *c() {} }\n",
                  "const o = {\n"
            "  get a() {\n"
            "    return 1;\n"
            "  },\n"
            "  set a(v) {},\n"
            "  [`k${1}`]: 2,\n"
            "  async b() {},\n"
            "  *c() {},\n"
            "};\n"));
    // in-parens
    CHECK(Formats("for (const x of (a, b)) {} for (let i = ('x' in o); i;) {} (function () {})(); (() => {})(); (async () => {})()\n",
                  "for (const x of (a, b)) {\n"
            "}\n"
            "for (let i = (\"x\" in o); i; ) {}\n"
            "(function () {})();\n"
            "(() => {})();\n"
            "(async () => {})();\n"));
    // let-ident
    CHECK(Formats("(let[0] = 1);\n",
                  "(let)[0] = 1;\n"));

    // The expected outputs above are prettier 3.6.2's (--parser babel) for the
    // same input, default options throughout.

    // TypeScript and TSX (Options::typescript): expected outputs are prettier
    // 3.6.2's `--parser typescript` for the same input (a .ts / .tsx file).
    {
        jsfmt::Options ts;
        ts.typescript = true;
        ts.jsx = false;
        jsfmt::Options tsx;
        tsx.typescript = true;
        // interface
        CHECK(Formats("interface Foo<T> extends Bar, Baz<T> { a?: string; readonly b: number[]; m(x: T): void; [k: string]: unknown; new (x: number): Foo<T>; (y: string): void }\n",
                      "interface Foo<T> extends Bar, Baz<T> {\n"
                      "  a?: string;\n"
                      "  readonly b: number[];\n"
                      "  m(x: T): void;\n"
                      "  [k: string]: unknown;\n"
                      "  new (x: number): Foo<T>;\n"
                      "  (y: string): void;\n"
                      "}\n",
                      ts));
        // type-union-long
        CHECK(Formats("type Status = 'active' | 'inactive' | 'pending-review' | 'archived' | 'deleted-by-owner';\n",
                      "type Status =\n"
                      "  | \"active\"\n"
                      "  | \"inactive\"\n"
                      "  | \"pending-review\"\n"
                      "  | \"archived\"\n"
                      "  | \"deleted-by-owner\";\n",
                      ts));
        // union-comments
        CHECK(Formats("type A =\n"
                      "  | 'a' // first\n"
                      "  // own line\n"
                      "  | 'b';\n",
                      "type A =\n"
                      "  | \"a\" // first\n"
                      "  // own line\n"
                      "  | \"b\";\n",
                      ts));
        // enum
        CHECK(Formats("export const enum E { A = 1, B = A << 1, 'c-d' = 3 }\n"
                      "declare enum F {}\n",
                      "export const enum E {\n"
                      "  A = 1,\n"
                      "  B = A << 1,\n"
                      "  \"c-d\" = 3,\n"
                      "}\n"
                      "declare enum F {}\n",
                      ts));
        // generics
        CHECK(Formats("function f<T extends object = {}, const U extends readonly unknown[] = []>(a: T, ...rest: U): asserts a is T {}\n"
                      "const g = <T,>(x: T): T => x;\n",
                      "function f<T extends object = {}, const U extends readonly unknown[] = []>(\n"
                      "  a: T,\n"
                      "  ...rest: U\n"
                      "): asserts a is T {}\n"
                      "const g = <T>(x: T): T => x;\n",
                      ts));
        // mapped-conditional
        CHECK(Formats("type M<T> = { readonly [K in keyof T as `get${Capitalize<K & string>}`]-?: () => T[K] };\n"
                      "type C<T> = T extends string ? 'str' : T extends (infer U)[] ? U : T extends (...args: any[]) => infer R ? R : never;\n",
                      "type M<T> = {\n"
                      "  readonly [K in keyof T as `get${Capitalize<K & string>}`]-?: () => T[K];\n"
                      "};\n"
                      "type C<T> = T extends string\n"
                      "  ? \"str\"\n"
                      "  : T extends (infer U)[]\n"
                      "    ? U\n"
                      "    : T extends (...args: any[]) => infer R\n"
                      "      ? R\n"
                      "      : never;\n",
                      ts));
        // casts
        CHECK(Formats("const a = b as unknown as string;\n"
                      "const c = { x: 1 } satisfies Record<string, number>;\n"
                      "const d = <any>e;\n"
                      "(f as Foo).bar();\n"
                      "const g = h!.i!;\n",
                      "const a = b as unknown as string;\n"
                      "const c = { x: 1 } satisfies Record<string, number>;\n"
                      "const d = <any>e;\n"
                      "(f as Foo).bar();\n"
                      "const g = h!.i!;\n",
                      ts));
        // parens
        CHECK(Formats("type A = (B | C)[];\n"
                      "type D = keyof (E | F);\n"
                      "type G = (() => void) | null;\n"
                      "type H = (new () => X)[];\n"
                      "type I = (typeof j)[number];\n",
                      "type A = (B | C)[];\n"
                      "type D = keyof (E | F);\n"
                      "type G = (() => void) | null;\n"
                      "type H = (new () => X)[];\n"
                      "type I = (typeof j)[number];\n",
                      ts));
        // class
        CHECK(Formats("@Component({ selector: 'app' })\n"
                      "export abstract class C<T> extends B<T> implements I, J {\n"
                      "  @Input() name!: string;\n"
                      "  private static readonly x: number = 1;\n"
                      "  declare y: string;\n"
                      "  constructor(private readonly s: S, public t?: T) { super(); }\n"
                      "  abstract m(): void;\n"
                      "  protected override get v(): number { return 1 }\n"
                      "  accessor z = 2;\n"
                      "}\n",
                      "@Component({ selector: \"app\" })\n"
                      "export abstract class C<T> extends B<T> implements I, J {\n"
                      "  @Input() name!: string;\n"
                      "  private static readonly x: number = 1;\n"
                      "  declare y: string;\n"
                      "  constructor(\n"
                      "    private readonly s: S,\n"
                      "    public t?: T,\n"
                      "  ) {\n"
                      "    super();\n"
                      "  }\n"
                      "  abstract m(): void;\n"
                      "  protected override get v(): number {\n"
                      "    return 1;\n"
                      "  }\n"
                      "  accessor z = 2;\n"
                      "}\n",
                      ts));
        // modules
        CHECK(Formats("declare module 'm' { export function f(): void; }\n"
                      "namespace A.B.C { export const x = 1; }\n"
                      "declare global { interface Window { a: string } }\n"
                      "import type { T } from './t';\n"
                      "import x = require('x');\n"
                      "export type { U };\n"
                      "export = x;\n"
                      "export as namespace NS;\n",
                      "declare module \"m\" {\n"
                      "  export function f(): void;\n"
                      "}\n"
                      "namespace A.B.C {\n"
                      "  export const x = 1;\n"
                      "}\n"
                      "declare global {\n"
                      "  interface Window {\n"
                      "    a: string;\n"
                      "  }\n"
                      "}\n"
                      "import type { T } from \"./t\";\n"
                      "import x = require(\"x\");\n"
                      "export type { U };\n"
                      "export = x;\n"
                      "export as namespace NS;\n",
                      ts));
        // overloads
        CHECK(Formats("function f(a: string): string;\n"
                      "function f(a: number): number;\n"
                      "function f(a: any) { return a; }\n",
                      "function f(a: string): string;\n"
                      "function f(a: number): number;\n"
                      "function f(a: any) {\n"
                      "  return a;\n"
                      "}\n",
                      ts));
        // tuples-templates
        CHECK(Formats("type T = [name: string, age?: number, ...rest: boolean[]];\n"
                      "type U = `prefix-${string}-suffix`;\n"
                      "let v: unique symbol;\n"
                      "type W = import('./mod').Foo<string>;\n",
                      "type T = [name: string, age?: number, ...rest: boolean[]];\n"
                      "type U = `prefix-${string}-suffix`;\n"
                      "let v: unique symbol;\n"
                      "type W = import(\"./mod\").Foo<string>;\n",
                      ts));
        // long-signature
        CHECK(Formats("export function someVeryLongFunctionName(firstArgument: SomeLongTypeName, secondArgument: AnotherLongTypeName): Promise<Map<string, number>> {}\n",
                      "export function someVeryLongFunctionName(\n"
                      "  firstArgument: SomeLongTypeName,\n"
                      "  secondArgument: AnotherLongTypeName,\n"
                      "): Promise<Map<string, number>> {}\n",
                      ts));
        // long-generic-alias
        CHECK(Formats("type Handler<TEvent extends keyof EventMap, TContext extends Record<string, unknown>> = (event: EventMap[TEvent], context: TContext) => void;\n",
                      "type Handler<\n"
                      "  TEvent extends keyof EventMap,\n"
                      "  TContext extends Record<string, unknown>,\n"
                      "> = (event: EventMap[TEvent], context: TContext) => void;\n",
                      ts));
        // intersection-object
        CHECK(Formats("type Props = OwnProps & { children?: React.ReactNode; className?: string; onClick?: () => void };\n",
                      "type Props = OwnProps & {\n"
                      "  children?: React.ReactNode;\n"
                      "  className?: string;\n"
                      "  onClick?: () => void;\n"
                      "};\n",
                      ts));
        // tsx-component
        CHECK(Formats("const C = <T,>({ items }: { items: T[] }) => <ul>{items.map((i) => <li key={String(i)}>{String(i)}</li>)}</ul>;\n"
                      "const el = <Foo<string> value=\"x\" />;\n",
                      "const C = <T,>({ items }: { items: T[] }) => (\n"
                      "  <ul>\n"
                      "    {items.map((i) => (\n"
                      "      <li key={String(i)}>{String(i)}</li>\n"
                      "    ))}\n"
                      "  </ul>\n"
                      ");\n"
                      "const el = <Foo<string> value=\"x\" />;\n",
                      tsx));
        // The dialect follows the file name.
        jsfmt::Options d;
        jsfmt::SetDialectForPath(d, "a/b.d.ts");
        CHECK(d.typescript && !d.jsx && d.dts);
        jsfmt::SetDialectForPath(d, "x.TSX");
        CHECK(d.typescript && d.jsx && !d.dts);
        jsfmt::SetDialectForPath(d, "x.mts");
        CHECK(d.typescript && !d.jsx);
        jsfmt::SetDialectForPath(d, "x.jsx");
        CHECK(!d.typescript && d.jsx);
        // `<T>x` is a cast in .ts and JSX in .tsx; a type in JavaScript is a refusal.
        CHECK(Formats("const a = <T>b;\n", "const a = <T>b;\n", ts));
        CHECK(!jsfmt::Format("const a = <T>b;\n", tsx).ok);
        CHECK(!jsfmt::Format("let a: number = 1;\n", {}).ok);
        const jsfmt::Result bad = jsfmt::Format("interface {\n", ts);
        CHECK(!bad.ok && bad.text == "interface {\n");
    }

    // Options: width, tabs, no semicolons, single quotes.
    {
        jsfmt::Options o;
        o.width = 20;
        CHECK(Formats("foo(alpha, beta, gamma);\n", "foo(\n  alpha,\n  beta,\n  gamma,\n);\n", o));
        o = jsfmt::Options();
        o.use_tabs = true;
        CHECK(Formats("if (a) { b() }\n", "if (a) {\n\tb();\n}\n", o));
        o = jsfmt::Options();
        o.semi = false;
        CHECK(Formats("const a = 1;\n[1].map(f);\n", "const a = 1\n;[1].map(f)\n", o));
        o = jsfmt::Options();
        o.single_quote = true;
        CHECK(Formats("a = \"x\"; b = \"it's\";\n", "a = 'x';\nb = \"it's\";\n", o));
    }

    // Refusals: syntax errors and pathological nesting come back unchanged
    // with ok=false, never a crash.
    {
        const jsfmt::Result bad = jsfmt::Format("let = = 1;\n", {});
        CHECK(!bad.ok);
        CHECK(bad.text == "let = = 1;\n");
        CHECK(bad.error_line == 1);
        const std::string deep = "x = " + std::string(5000, '[') + std::string(5000, ']') + ";\n";
        const jsfmt::Result d = jsfmt::Format(deep, {});
        CHECK(!d.ok);
        CHECK(d.text == deep);
        std::string chain = "x = a";
        for (int k = 0; k < 5000; k++) chain += " + a";
        chain += ";\n";
        const jsfmt::Result c = jsfmt::Format(chain, {});
        CHECK(!c.ok);
        CHECK(c.text == chain);
        // Invalid UTF-8 is not a crash either.
        const jsfmt::Result u = jsfmt::Format("s = 1;\n\xff\xfe\n", {});
        CHECK(!u.ok);
    }

    std::printf("mep-js-format-test: %d checks passed\n", g_checks);
    return 0;
}
