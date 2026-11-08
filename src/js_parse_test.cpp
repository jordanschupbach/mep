// mep-js-parse-test: unit tests for the jsparse acorn port, and the
// acorn-shaped JSON dumper tools/js_parse_check.py compares with acorn.
//
//   mep-js-parse-test                       run the unit tests
//   mep-js-parse-test --json [--module|--script|--auto] [--jsx] [--keep-going] FILE
//       print {"ast":...,"comments":[...]} or {"error",...} for FILE
//       (--auto, the default: module, falling back to script); with
//       --keep-going, the recovered tree plus "errors":[...] even on error;
//       with --tokens, the token stream as [[label, start, end], ...]
//   mep-js-parse-test --json-list LIST OUTDIR
//       the same for every "MODE JSX PATH" line of LIST (MODE is module,
//       script or auto; JSX 0/1, or ts/tsx/dts for TypeScript), writing
//       OUTDIR/<line number>.json
//   mep-js-parse-test --json --ts|--tsx|--dts|--mts|--cts FILE
//       the TypeScript dialect (typescript-estree's TS nodes)
//   mep-js-parse-test --errors-list LIST
//       for every "LANG PATH" line (LANG js/jsx/ts/tsx/dts) one line:
//       "ok" or "error UPOS LINE:COL MESSAGE" (the first error)
//   mep-js-parse-test --shape --ts|--tsx|--dts|--mts|--cts FILE
//       every node in source order, "TYPE START END" (UTF-16 offsets), one
//       per line, then "error UPOS MESSAGE" lines (the tree comparison with
//       typescript-estree, tools/js_parse_ts_check.py --trees)
//   mep-js-parse-test --shape-list LIST OUTDIR
//       the same for every "LANG PATH" line of LIST, into OUTDIR/<n>.txt

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "js_parse.h"

using namespace jsparse;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "js_parse_test.cpp:%d: CHECK failed: %s\n", line, expr);
    }
}
#define CHECK(c) Check((c), #c, __LINE__)

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// lang: "js", "jsx", "ts", "tsx" or "dts" (the --json-list JSX column:
// 0 and 1 are js and jsx).
ParseResult Run(const std::string& src, const std::string& mode, const std::string& lang) {
    Options o;
    o.jsx = lang == "jsx" || lang == "1" || lang == "tsx";
    if (lang == "ts" || lang == "tsx" || lang == "dts" || lang == "mts" || lang == "cts") {
        o.typescript = true;
        o.dts = lang == "dts";
        o.mts = lang == "mts";
        o.source_type = SourceType::Module;
        return Parse(src, o);
    }
    if (mode == "module") {
        o.source_type = SourceType::Module;
        return Parse(src, o);
    }
    if (mode == "script") {
        o.source_type = SourceType::Script;
        return Parse(src, o);
    }
    return ParseAuto(src, o);
}

// Every node, "TYPE START END" (UTF-16), in source order; then the errors.
std::string Shape(const ParseResult& r) {
    std::string out;
    auto walk = [&](auto& self, const Node* n) -> void {
        out += NodeTypeName(n->type);
        out += " " + std::to_string(n->ustart) + " " + std::to_string(n->uend) + "\n";
        ForEachChild(n, [&](const Node* k) { self(self, k); });
    };
    if (r.program) walk(walk, r.program);
    for (const Error& e : r.errors) out += "error " + std::to_string(e.upos) + " " + e.message + "\n";
    return out;
}

ParseResult P(const std::string& src, bool module = false, bool jsx = false) {
    Options o;
    o.jsx = jsx;
    o.source_type = module ? SourceType::Module : SourceType::Script;
    return Parse(src, o);
}

std::string FirstError(const ParseResult& r) { return r.errors.empty() ? std::string() : r.errors[0].Full(); }

int CountType(const Node* n, NodeType t) {
    if (!n) return 0;
    int c = n->type == t ? 1 : 0;
    ForEachChild(n, [&](Node* k) { c += CountType(k, t); });
    return c;
}

void TestBasics() {
    ParseResult r = P("let x = 1 + 2 * 3;\nfoo(x);");
    CHECK(!r.has_error);
    CHECK(r.program && r.program->type == NodeType::Program);
    CHECK(r.program->body_list().size() == 2);
    Node* decl = r.program->body_list()[0];
    CHECK(decl->type == NodeType::VariableDeclaration && decl->kind == "let");
    Node* init = decl->declarations()[0]->init();
    CHECK(init->type == NodeType::BinaryExpression && init->op == "+");
    CHECK(init->right()->op == "*");
    CHECK(r.program->body_list()[1]->loc_start.line == 2);
    CHECK(ToJson(r).find("\"sourceType\":\"script\"") != std::string::npos);
}

void TestPositions() {
    // "é" is 2 bytes / 1 UTF-16 unit; "😀" is 4 bytes / 2 units.
    ParseResult r = P("'é😀'; x");
    CHECK(!r.has_error);
    Node* x = r.program->body_list()[1]->expression();
    CHECK(x->type == NodeType::Identifier);
    CHECK(x->ustart == 7);
    CHECK(x->start == 10);
    CHECK(x->loc_start.column == 7);
    CHECK(r.ByteOffset(7) == 10);
    CHECK(r.U16Offset(10) == 7);
    Node* lit = r.program->body_list()[0]->expression();
    CHECK(lit->str2 == "é😀");
    CHECK(lit->str == "'é😀'");
}

void TestErrors() {
    ParseResult r = P("var a = ;");
    CHECK(r.has_error);
    CHECK(FirstError(r) == "Unexpected token (1:8)");
    CHECK(r.errors[0].upos == 8);
    CHECK(r.first_error_offset == 8);

    CHECK(FirstError(P("let let = 1")) == "let is disallowed as a lexically bound name (1:4)");
    CHECK(FirstError(P("'use strict'; with (a) {}")) == "'with' in strict mode (1:14)");
    CHECK(FirstError(P("/a(/")) == "Invalid regular expression: /a(/: Unterminated group (1:1)");
    CHECK(FirstError(P("x = 08n")) == "Identifier directly after number (1:6)");
    CHECK(FirstError(P("a ?? b || c")) ==
          "Logical expressions and coalesce expressions cannot be mixed. Wrap either by parentheses (1:7)");
    CHECK(FirstError(P("export default 1")) == "'import' and 'export' may appear only with 'sourceType: module' (1:0)");
    CHECK(!P("export default 1", true).has_error);
    CHECK(FirstError(P("`a${b`")) == "Unterminated template literal (1:6)");
}

void TestRecovery() {
    ParseResult r = P("let a = 1;\nlet b = ;\nfunction f() { return 3 }\nlet c = 2;");
    CHECK(r.has_error);
    CHECK(FirstError(r) == "Unexpected token (2:8)");
    CHECK(CountType(r.program, NodeType::FunctionDeclaration) == 1);
    CHECK(CountType(r.program, NodeType::VariableDeclaration) == 2);

    ParseResult r2 = P("function g() {\n  if (x {\n  }\n  return 1;\n}\nconst z = 5;");
    CHECK(r2.has_error);
    bool sawZ = false;
    for (Node* s : r2.program->body_list())
        if (s->type == NodeType::VariableDeclaration && s->declarations()[0]->id()->str == "z") sawZ = true;
    CHECK(sawZ);

    ParseResult r3 = P("class A {\n  m() { ) }\n  n() {}\n}\n");
    CHECK(r3.has_error);
    CHECK(CountType(r3.program, NodeType::MethodDefinition) >= 1);

    // First token broken.
    ParseResult r4 = P("'abc\nfoo();");
    CHECK(r4.has_error);
    CHECK(FirstError(r4) == "Unterminated string constant (1:0)");
    CHECK(CountType(r4.program, NodeType::CallExpression) == 1);

    // Pathological nesting is an error, not a crash.
    std::string deep(100000, '(');
    ParseResult r5 = P(deep);
    CHECK(r5.has_error);
    std::string deep2;
    for (int i = 0; i < 50000; ++i) deep2 += "{";
    CHECK(P(deep2).has_error);
    std::string deep3;
    for (int i = 0; i < 50000; ++i) deep3 += "[";
    CHECK(P(deep3).has_error);

    Options o;
    o.recover = false;
    ParseResult r6 = Parse("a b c; d", o);
    CHECK(r6.has_error && r6.errors.size() == 1 && r6.program == nullptr);
}

void TestJsx() {
    ParseResult r = P("const e = <div className=\"a\" {...p}>hi &amp; {x}<br/></div>;", false, true);
    CHECK(!r.has_error);
    CHECK(CountType(r.program, NodeType::JSXElement) == 2);
    CHECK(CountType(r.program, NodeType::JSXSpreadAttribute) == 1);
    CHECK(CountType(r.program, NodeType::JSXText) == 1);
    CHECK(ToJson(r).find("\"value\":\"hi & \"") != std::string::npos);
    CHECK(FirstError(P("<a></b>", false, true)) == "Expected corresponding JSX closing tag for <a> (1:3)");
    CHECK(FirstError(P("<a>}</a>", false, true)) ==
          "Unexpected token `}`. Did you mean `&rbrace;` or `{\"}\"}`? (1:3)");
    CHECK(P("<a/>", false, false).has_error);
    CHECK(!P("<>x</>", false, true).has_error);
}

void TestCommentsAndTokens() {
    Options o;
    o.tokens = true;
    ParseResult r = Parse("/* a */ x // b\n#!", o);
    CHECK(r.comments.size() == 2);
    CHECK(r.comments[0].block && r.comments[0].value == " a ");
    CHECK(!r.comments[1].block && r.comments[1].value == " b");
    CHECK(r.has_error);  // '#!' only at the very start

    ParseResult h = Parse("#!/usr/bin/env node\nx");
    CHECK(!h.has_error);
    CHECK(h.comments.size() == 1 && h.comments[0].value == "/usr/bin/env node");

    ParseResult t = Parse("a = b", o);
    CHECK(t.tokens.size() == 4);  // a, =, b, eof
    CHECK(std::strcmp(t.tokens[1].type, "=") == 0);
}

void TestModernSyntax() {
    const char* ok[] = {
        "class A { #x = 1; static { this.y = 2 } get #z() { return this.#x } has(o) { return #x in o } }",
        "a?.b?.[c]?.(d)",
        "x ?\?= y; x ||= z; x &&= w",
        "const {a, b: [c = 1, ...d], ...e} = f",
        "async function* g() { for await (const x of y) yield* x }",
        "label: for (;;) { continue label }",
        "/(?<year>\\d{4})-\\k<year>/u; /[\\p{L}--[a-z]]/v",
        "1_000_000n + 0x1Fn + 0o7 + 0b1 + .5e-3",
        "tag`a${b}c\\unicode`",
        "import(\"x\", { with: { type: \"json\" } })",
    };
    for (const char* src : ok) {
        ParseResult r = Parse(src, Options{});
        if (r.has_error) std::fprintf(stderr, "  unexpected error for %s: %s\n", src, FirstError(r).c_str());
        CHECK(!r.has_error);
    }
    Options m;
    m.source_type = SourceType::Module;
    CHECK(!Parse("import x, {y as z} from 'm' with {type: 'json'}; export * as ns from 'n'; await 1;", m).has_error);
    CHECK(FirstError(Parse("export {q}", m)) == "Export 'q' is not defined (1:8)");
}

void TestJsonShape() {
    ParseResult r = P("({a, b: 1n, c: /x/g})");
    std::string j = ToJson(r);
    CHECK(j.find("\"bigint\":\"1\"") != std::string::npos);
    CHECK(j.find("\"regex\":{\"pattern\":\"x\",\"flags\":\"g\"}") != std::string::npos);
    CHECK(j.find("\"shorthand\":true") != std::string::npos);
    ParseResult e = P("a b");
    CHECK(ToJson(e) == "{\"error\":\"Unexpected token (1:2)\",\"pos\":2,\"line\":1,\"column\":2}");
}

ParseResult T(const std::string& src, const std::string& lang = "ts") { return Run(src, "", lang); }

// The first node of type t in source order, or nullptr.
const Node* Find(const Node* n, NodeType t) {
    if (!n) return nullptr;
    if (n->type == t) return n;
    const Node* found = nullptr;
    ForEachChild(n, [&](const Node* k) {
        if (!found) found = Find(k, t);
    });
    return found;
}

void TestTypeScript() {
    const char* ok[] = {
        "let x: number = 1; const y!: string; function f<T extends object = {}>(a?: T, ...r: T[]): asserts a is T {}",
        "interface I<T> extends A, B<T> { a?: T; readonly [k: string]: unknown; (x: number): void; new (): I<T>; m?<U>(): U }",
        "type M<T> = { readonly [K in keyof T as `get${Capitalize<K & string>}`]-?: () => T[K] }",
        "type C<T> = T extends (infer U extends string)[] ? U : T extends `${infer H}${string}` ? H : never",
        "enum E { A = 1, B = A << 1, 'c' } const enum F { X } declare enum G {}",
        "namespace A.B { export const x = 1 } declare module 'm' { export = x } declare global { var g: number }",
        "abstract class C<T> extends D<T> implements I, J { private constructor(public readonly x: number, protected y?: string) { super() }"
        " abstract m(): void; static #p = 1; declare d: number; override accessor a = 1; get g(): number { return 1 } }",
        "x as const; y satisfies T; z!.w!; <T>v; f<string>(1); new G<number>(); const g = f<string>;",
        "import type { A } from 'a'; import { type B, C } from 'b'; import D = require('d'); import E = N.M; export type { A };"
        " export = D; export as namespace NS; export import F = N;",
        "@dec class K { @dec() m(@inject() p: number) {} @a.b<c>() prop; }",
        "function o(a: string): void; function o(a: number): void; function o(a: any) {}",
        "const a = async <T,>(x: T): Promise<T> => x; const b = (x: number): x is 1 => true; let c = (a, b) => a;",
        "a < b > c; f(a < b, c > d); x = y ? (z): w => 1 : v;",
        "let t: [a: string, b?: number, ...c: boolean[]]; let u: typeof import('x'); let v: abstract new () => void;",
    };
    for (const char* src : ok) {
        ParseResult r = T(src);
        if (r.has_error) std::fprintf(stderr, "  unexpected TS error for %s: %s\n", src, FirstError(r).c_str());
        CHECK(!r.has_error);
        CHECK(r.typescript);
    }

    // node shapes (typescript-estree's)
    ParseResult r = T("let x: Array<number> = [];");
    const Node* id = Find(r.program, NodeType::Identifier);
    CHECK(id && id->str == "x" && id->typeAnnotation() && id->typeAnnotation()->type == NodeType::TSTypeAnnotation);
    CHECK(id->uend == 20);  // the annotation is part of the binding's range
    const Node* ref = Find(r.program, NodeType::TSTypeReference);
    CHECK(ref && ref->typeArguments() && ref->typeArguments()->params().size() == 1);
    CHECK(ToJson(r).find("\"typeAnnotation\":{\"type\":\"TSTypeAnnotation\"") != std::string::npos);
    CHECK(ToJson(P("let x = 1")).find("typeAnnotation") == std::string::npos);  // JS JSON is unchanged

    ParseResult cls = T("class A { constructor(private x: number) {} }");
    const Node* pp = Find(cls.program, NodeType::TSParameterProperty);
    CHECK(pp && pp->accessibility == "private" && pp->parameter()->str == "x");
    ParseResult ex = T("export interface I {} export declare const c: number;");
    CHECK(CountType(ex.program, NodeType::ExportNamedDeclaration) == 2);
    CHECK(ex.program->body_list()[0]->HasTs(kTsTypeOnly));
    CHECK(Find(ex.program, NodeType::VariableDeclaration)->HasTs(kTsDeclare));
    CHECK(CountType(T("function f(): void; function f() {}").program, NodeType::TSDeclareFunction) == 1);
    CHECK(Find(T("x!").program, NodeType::TSNonNullExpression));
    CHECK(Find(T("<T>x").program, NodeType::TSTypeAssertion));
    CHECK(Find(T("f<T>").program, NodeType::TSInstantiationExpression));
    CHECK(Find(T("(a: T) => a").program, NodeType::ArrowFunctionExpression));

    // TSX: `<T>` is a tag, `<T,>` a generic arrow; JSX type arguments
    CHECK(Find(T("<T>x</T>", "tsx").program, NodeType::JSXElement));
    CHECK(Find(T("<T,>(x: T) => x", "tsx").program, NodeType::ArrowFunctionExpression));
    CHECK(!T("<C<string> a='1' />; <D<T>></D>", "tsx").has_error);
    CHECK(T("<T>x", "tsx").has_error);
    // TSX: as / satisfies / ! inside expression containers and attributes
    for (const char* src : {"a = <div>{x as any}</div>;", "a = <div>{x satisfies T}</div>;", "a = <div>{x!}</div>;",
                            "a = <div a={x as T}>{x as T}<b/>{y!}</div>;"}) {
        ParseResult rx = T(src, "tsx");
        if (rx.has_error) std::fprintf(stderr, "  unexpected TSX error for %s: %s\n", src, FirstError(rx).c_str());
        CHECK(!rx.has_error);
    }
    CHECK(CountType(T("a = <div>{x as any}{y satisfies T}</div>", "tsx").program, NodeType::JSXExpressionContainer) == 2);
    // `(f<T>).x` is fine, `f<T>.x` is tsc's error
    CHECK(!T("(Array<string>).name").has_error);
    CHECK(FirstError(T("Array<string>.name")) ==
          "An instantiation expression cannot be followed by a property access (1:5)");
    // JSDoc types stay JSDoc types (tsc's parser accepts them)
    ParseResult jd = T("let a: ?string, b: string?, c: !T, d: T!, e: *, f: Array<?>, g: [x?, ...y?], "
                       "h: function(this: A, string=, ...number): void;");
    CHECK(!jd.has_error);
    CHECK(CountType(jd.program, NodeType::TSJSDocNullableType) == 3);  // ?string, string?, the rest's y?
    CHECK(CountType(jd.program, NodeType::TSJSDocNonNullableType) == 2);
    CHECK(CountType(jd.program, NodeType::TSJSDocAllType) == 1);
    CHECK(CountType(jd.program, NodeType::TSJSDocUnknownType) == 1);
    CHECK(CountType(jd.program, NodeType::TSOptionalType) == 1);  // the tuple's x?
    CHECK(CountType(jd.program, NodeType::TSJSDocFunctionType) == 1);
    CHECK(CountType(jd.program, NodeType::TSJSDocOptionalType) == 1);
    ParseResult pr = T("let b: string?");
    const Node* post = Find(pr.program, NodeType::TSJSDocNullableType);
    CHECK(post && post->HasTs(kTsPostfix) && post->typeAnnotation()->typeAnnotation()->type == NodeType::TSStringKeyword);
    CHECK(ToJson(T("let b: !T")).find("\"postfix\":false") != std::string::npos);
    // top-level await: a call in a script-like .ts/.cts, an await in a module or .mts
    CHECK(Find(T("await(1);").program, NodeType::CallExpression));
    CHECK(Find(T("await(1);", "cts").program, NodeType::CallExpression));
    CHECK(Find(T("await(1);", "mts").program, NodeType::AwaitExpression));
    CHECK(Find(T("await(1); export {};").program, NodeType::AwaitExpression));
    CHECK(Find(T("function f() { await(1); }", "mts").program, NodeType::CallExpression));
    // .d.ts and declare
    CHECK(!T("declare function f(): void; export declare class C { m(): void; }", "dts").has_error);

    // errors: tsc's positions, acorn's style
    CHECK(FirstError(T("let x: = 1")) == "Unexpected token (1:7)");
    CHECK(FirstError(T("interface { }")) == "Unexpected token (1:10)");
    CHECK(FirstError(T("enum E { A = }")) == "Unexpected token (1:13)");
    CHECK(FirstError(T("var x = 01")) == "Octal literals are not allowed (1:8)");
    CHECK(FirstError(T("a?.#b")) == "An optional chain cannot contain private identifiers (1:3)");
    // tsc leaves these to its checker: no parse error
    CHECK(!T("'use strict'; with (a) {} var let = 1; delete x; return; yield = 1; arguments = 2").has_error);
    CHECK(!T("var; for (var of of) {} const x; class A { constructor() { super() } }").has_error);
    // JS options never see TypeScript syntax
    CHECK(P("let x: number").has_error);

    // recovery keeps going past a broken member
    ParseResult rec = T("class A { m(: number) {} n() {} }\ninterface I { a: }\nlet ok = 1");
    CHECK(rec.has_error);
    CHECK(CountType(rec.program, NodeType::VariableDeclaration) == 1);
}

int RunUnitTests() {
    TestBasics();
    TestPositions();
    TestErrors();
    TestRecovery();
    TestJsx();
    TestCommentsAndTokens();
    TestModernSyntax();
    TestJsonShape();
    TestTypeScript();
    if (g_failures) {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("js_parse_test: all %d checks passed\n", g_checks);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--json") == 0) {
        std::string mode = "auto";
        bool jsx = false, keepGoing = false, tokens = false;
        std::string lang;
        const char* file = nullptr;
        for (int i = 2; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--module"))
                mode = "module";
            else if (!std::strcmp(argv[i], "--script"))
                mode = "script";
            else if (!std::strcmp(argv[i], "--auto"))
                mode = "auto";
            else if (!std::strcmp(argv[i], "--jsx"))
                jsx = true;
            else if (!std::strcmp(argv[i], "--ts"))
                lang = "ts";
            else if (!std::strcmp(argv[i], "--tsx"))
                lang = "tsx";
            else if (!std::strcmp(argv[i], "--dts"))
                lang = "dts";
            else if (!std::strcmp(argv[i], "--mts"))
                lang = "mts";
            else if (!std::strcmp(argv[i], "--cts"))
                lang = "cts";
            else if (!std::strcmp(argv[i], "--keep-going"))
                keepGoing = true;
            else if (!std::strcmp(argv[i], "--tokens"))
                tokens = true;
            else
                file = argv[i];
        }
        std::string src;
        if (!file || !ReadFile(file, src)) {
            std::fprintf(stderr, "usage: %s --json [--module|--script|--auto] [--jsx] FILE\n", argv[0]);
            return 2;
        }
        if (tokens) {
            // acorn's onToken stream: [[label, start, end], ...] (UTF-16)
            Options o;
            o.jsx = jsx || lang == "tsx";
            o.typescript = lang == "ts" || lang == "tsx" || lang == "dts";
            o.dts = lang == "dts";
            o.tokens = true;
            o.source_type = mode == "script" ? SourceType::Script : SourceType::Module;
            ParseResult t = Parse(src, o);
            if (mode == "auto" && t.has_error) {
                o.source_type = SourceType::Script;
                ParseResult t2 = Parse(src, o);
                if (!t2.has_error) t = std::move(t2);
            }
            std::string o2 = "[";
            for (size_t i = 0; i < t.tokens.size(); ++i) {
                const Token& k = t.tokens[i];
                if (i) o2 += ",";
                std::string label = k.type;
                std::string esc;
                for (char c : label) {
                    if (c == '"' || c == '\\') esc += '\\';
                    esc += c;
                }
                o2 += "[\"" + esc + "\"," + std::to_string(k.ustart) + "," + std::to_string(k.uend) + "]";
            }
            o2 += "]\n";
            std::fwrite(o2.data(), 1, o2.size(), stdout);
            return t.has_error ? 1 : 0;
        }
        if (lang.empty()) lang = jsx ? "jsx" : "js";
        ParseResult r = Run(src, mode, lang);
        std::string j;
        if (keepGoing) {
            // the recovered tree, then every error recorded
            std::vector<Error> errors = r.errors;
            r.has_error = false;
            j = ToJson(r);
            j.pop_back();
            j += ",\"errors\":[";
            for (size_t i = 0; i < errors.size(); ++i) {
                if (i) j += ",";
                ParseResult e;
                e.has_error = true;
                e.errors.push_back(errors[i]);
                j += ToJson(e);
            }
            j += "]}";
        } else {
            j = ToJson(r);
        }
        std::fwrite(j.data(), 1, j.size(), stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "--json-list") == 0) {
        std::ifstream list(argv[2]);
        std::string line;
        int n = 0;
        while (std::getline(list, line)) {
            ++n;
            std::istringstream ls(line);
            std::string mode, jsx, path;
            ls >> mode >> jsx;
            std::getline(ls >> std::ws, path);
            std::string src;
            std::string j;
            if (!ReadFile(path, src))
                j = "{\"io_error\":true}";
            else
                j = ToJson(Run(src, mode, jsx));
            std::ofstream o(std::string(argv[3]) + "/" + std::to_string(n) + ".json", std::ios::binary);
            o << j << '\n';
        }
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--errors-list") == 0) {
        std::ifstream list(argv[2]);
        std::string line;
        while (std::getline(list, line)) {
            std::istringstream ls(line);
            std::string lang, path;
            ls >> lang;
            std::getline(ls >> std::ws, path);
            std::string src;
            if (!ReadFile(path, src)) {
                std::printf("io_error\n");
                continue;
            }
            ParseResult r = Run(src, "auto", lang);
            if (!r.has_error || r.errors.empty()) {
                std::printf("ok\n");
            } else {
                const Error& e = r.errors[0];
                std::printf("error %u %u:%u %s\n", e.upos, e.loc.line, e.loc.column, e.message.c_str());
            }
            std::fflush(stdout);
        }
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "--shape") == 0) {
        std::string src;
        if (!ReadFile(argv[3], src)) return 2;
        ParseResult r = Run(src, "auto", argv[2] + 2);
        std::string out = Shape(r);
        std::fwrite(out.data(), 1, out.size(), stdout);
        return r.has_error ? 1 : 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "--shape-list") == 0) {
        std::ifstream list(argv[2]);
        std::string line;
        int n = 0;
        while (std::getline(list, line)) {
            ++n;
            std::istringstream ls(line);
            std::string lang, path;
            ls >> lang;
            std::getline(ls >> std::ws, path);
            std::string src, out;
            if (ReadFile(path, src)) out = Shape(Run(src, "auto", lang));
            std::ofstream o(std::string(argv[3]) + "/" + std::to_string(n) + ".txt", std::ios::binary);
            o << out;
        }
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--bench") == 0) {
        std::string src;
        if (!ReadFile(argv[2], src)) return 2;
        auto t0 = std::chrono::steady_clock::now();
        ParseResult r = ParseAuto(src);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("%s: %.2f ms, %s\n", argv[2], std::chrono::duration<double, std::milli>(t1 - t0).count(),
                    r.has_error ? FirstError(r).c_str() : "ok");
        return 0;
    }
    return RunUnitTests();
}
