// Windowless test for js_lsp*.cpp -- the analysis half of mep's own
// JavaScript language server (`mep-js-lsp`). Drives the pure functions
// directly: no process, no JSON-RPC client. CHECK(), never assert(): the
// Release build strips assert().
//
// The parser's agreement with acorn is js_parse_test's and
// tools/js_parse_check.py's business; the lints' agreement with ESLint is
// measured over the corpus (help/js-lsp.org). The cases here pin each
// feature's behaviour.
#include <cstdio>
#include <string>
#include <vector>

#include "js_lsp.h"

namespace {
int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char *expr, int line) {
    g_checks++;
    if (ok) return;
    g_failed++;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expr, __FILE__, line);
}
#define CHECK(c) Check((c), #c, __LINE__)

std::vector<std::string> L(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::shared_ptr<const JsLspDocument> A(const std::string &src, JsLspDialect d = JsLspDialect::Auto) {
    return JsLspAnalyze(L(src), d);
}

std::vector<JsLspDiagnostic> Diags(const std::string &src, JsLspDialect d = JsLspDialect::Auto) {
    return JsLspDiagnostics(*A(src, d));
}

int Count(const std::vector<JsLspDiagnostic> &ds, const std::string &code) {
    int n = 0;
    for (const auto &d : ds) {
        if (d.code == code) n++;
    }
    return n;
}

const JsLspDiagnostic *Find(const std::vector<JsLspDiagnostic> &ds, const std::string &code) {
    for (const auto &d : ds) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

bool HasLabel(const std::vector<JsLspCompletionItem> &items, const std::string &label) {
    for (const auto &it : items) {
        if (it.label == label) return true;
    }
    return false;
}

int IndexOf(const std::vector<JsLspCompletionItem> &items, const std::string &label) {
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].label == label) return static_cast<int>(i);
    }
    return -1;
}

bool Contains(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

void TestDialects() {
    CHECK(JsLspDialectForPath("a/b.mjs") == JsLspDialect::Module);
    CHECK(JsLspDialectForPath("file:///x/y.CJS") == JsLspDialect::Script);
    CHECK(JsLspDialectForPath("App.jsx") == JsLspDialect::Jsx);
    CHECK(JsLspDialectForPath("a.ts") == JsLspDialect::Ts);
    CHECK(JsLspDialectForPath("a.tsx") == JsLspDialect::Tsx);
    CHECK(JsLspDialectForPath("a.mts") == JsLspDialect::Ts && JsLspDialectForPath("a.cts") == JsLspDialect::Ts);
    CHECK(JsLspDialectForPath("lib/index.d.ts") == JsLspDialect::Dts);
    CHECK(JsLspDialectForPath("a.d.mts") == JsLspDialect::Dts && JsLspDialectForPath("styles.d.css.ts") == JsLspDialect::Dts);
    CHECK(JsLspDialectForPath("dir.d/a.ts") == JsLspDialect::Ts);
    CHECK(JsLspDialectForPath("a.js") == JsLspDialect::Auto);
}

void TestSyntax() {
    // acorn's message, at acorn's position; only the first error.
    auto ds = Diags("let a = 1;\nlet b = = 2;\nlet c = = 3;\n");
    CHECK(Count(ds, "syntax-error") == 1);
    const JsLspDiagnostic *e = Find(ds, "syntax-error");
    CHECK(e && e->message == "Unexpected token" && e->range.line == 1 && e->range.col == 8);
    CHECK(e->severity == JsLspSeverity::Error);
    // No unused-* on a repaired tree.
    CHECK(Count(ds, "no-unused-vars") == 0);
    // Module first, then script: `with` is fine in a script, `import` in a module.
    CHECK(Count(Diags("with (o) { x; }\n"), "syntax-error") == 0);
    CHECK(Count(Diags("import x from 'y';\nx();\n"), "syntax-error") == 0);
    // ... but .mjs is always a module and .cjs always a script.
    CHECK(Count(Diags("with (o) { x; }\n", JsLspDialect::Module), "syntax-error") == 1);
    CHECK(Count(Diags("import x from 'y';\nx();\n", JsLspDialect::Script), "syntax-error") == 1);
    // JSX parses in a .js file too.
    CHECK(Count(Diags("const App = () => <div className=\"a\">hi</div>;\nexport default App;\n"), "syntax-error") == 0);
    const auto unterminated = Diags("const s = 'abc\n");
    CHECK(Find(unterminated, "syntax-error") && Find(unterminated, "syntax-error")->message == "Unterminated string constant");
    // A hashbang line is fine.
    CHECK(Diags("#!/usr/bin/env node\nconsole.log(1);\n").empty());
}

// ---- TypeScript ----

const JsLspDialect kTs = JsLspDialect::Ts;

bool HasUnused(const std::vector<JsLspDiagnostic> &ds, const std::string &name) {
    for (const auto &d : ds) {
        if (d.code == "no-unused-vars" && d.message.rfind("'" + name + "'", 0) == 0) return true;
    }
    return false;
}

void TestTsSyntax() {
    // Types parse; tsc's parser's errors are reported, in acorn's words.
    CHECK(Diags("export let x: number = 1;\nexport function f<T>(a: T): T { return a; }\n", kTs).empty());
    auto ds = Diags("export let x: = 1;\n", kTs);
    CHECK(Count(ds, "syntax-error") == 1);
    // `<T>x` is an assertion in .ts and a tag in .tsx.
    CHECK(Count(Diags("export const a = <any>window;\n", kTs), "syntax-error") == 0);
    CHECK(Count(Diags("export const a = <div>hi</div>;\n", JsLspDialect::Tsx), "syntax-error") == 0);
    CHECK(Count(Diags("export const a = <div>hi</div>;\n", kTs), "syntax-error") == 1);
    // tsc leaves duplicate declarations to its checker; so does the parse.
    CHECK(Count(Diags("export function f(a: string): void;\nexport function f(a: any) { return a; }\n", kTs),
                "syntax-error") == 0);
}

void TestTsUnused() {
    // A type imported only to annotate is used; an import nothing names is not.
    auto ds = Diags("import type { A } from 'a';\nimport { B, C } from 'b';\nexport let v: A = 1 as unknown as B;\n", kTs);
    CHECK(!HasUnused(ds, "A") && !HasUnused(ds, "B") && HasUnused(ds, "C"));
    // Types and values are separate namespaces: an interface does not satisfy a value use...
    ds = Diags("interface Box { w: number }\nconst Box2 = 1;\nexport const b: Box = { w: Box2 };\n", kTs);
    CHECK(!HasUnused(ds, "Box") && !HasUnused(ds, "Box2"));
    // ...and one name can be both (declaration merging): one variable, used either way.
    ds = Diags("const Val = 1;\ntype Val = number;\nexport let u: Val;\n", kTs);
    CHECK(!HasUnused(ds, "Val") && Count(ds, "no-redeclare") == 0);
    // Unused interfaces, type aliases, enums, namespaces; a self-reference is not a use.
    ds = Diags("interface Node2 { next: Node2 }\ntype Rec = { r: Rec };\nenum E { X, Y = X + 1 }\nnamespace N { export const z = 1; }\nexport {};\n",
               kTs);
    CHECK(HasUnused(ds, "Node2") && HasUnused(ds, "Rec") && HasUnused(ds, "E") && HasUnused(ds, "N"));
    CHECK(!HasUnused(ds, "X") && !HasUnused(ds, "Y") && !HasUnused(ds, "z"));  // members, exported
    CHECK(Find(ds, "no-unused-vars")->message == "'Node2' is defined but never used.");
    // Type parameters: a hint, like parameters.
    ds = Diags("export function g<T, U>(x: T): T { return x; }\n", kTs);
    CHECK(HasUnused(ds, "U") && !HasUnused(ds, "T"));
    for (const auto &d : ds) {
        if (d.message.rfind("'U'", 0) == 0) CHECK(d.severity == JsLspSeverity::Hint);
    }
    // `infer`, mapped types, conditional types.
    ds = Diags("export type El<T> = T extends Array<infer E> ? E : never;\n"
               "export type Bad<T> = T extends Array<infer F> ? T : never;\n"
               "export type M<T> = { [K in keyof T]: string };\n",
               kTs);
    CHECK(!HasUnused(ds, "E") && HasUnused(ds, "F") && !HasUnused(ds, "K"));
    // Signatures declare no variables: function types, overloads, interface methods.
    ds = Diags("export type Fn = (unusedParam: number) => void;\n"
               "export interface I { m(p: string): void; n<V>(): void }\n"
               "export function over(a: string): void;\n"
               "export function over(a: any): void { console.log(a); }\n",
               kTs);
    CHECK(!HasUnused(ds, "unusedParam") && !HasUnused(ds, "p") && !HasUnused(ds, "a") && HasUnused(ds, "V"));
    CHECK(Count(ds, "no-redeclare") == 0);
    // An overloaded function's implementation may ignore a parameter some overload has.
    ds = Diags("function h(): void;\nfunction h(x: number): void;\nfunction h(x?: number) {}\nh();\n", kTs);
    CHECK(!HasUnused(ds, "x") && !HasUnused(ds, "h"));
    // Parameter properties are members; `this` is not a parameter.
    ds = Diags("export class K {\n  constructor(private readonly pp: number, public qq = 2) {}\n"
               "  m(this: K, z: number) { return z; }\n}\n",
               kTs);
    CHECK(!HasUnused(ds, "pp") && !HasUnused(ds, "qq") && !HasUnused(ds, "this"));
    // `declare`, ambient modules, `declare global` and .d.ts files: nothing reported.
    ds = Diags("declare const amb: number;\ndeclare function ambf(x: number): void;\n"
               "declare namespace DN { const inner: number; }\ndeclare module 'm' { const mx: number; }\n"
               "declare global { interface Window { q: number } }\nexport {};\n",
               kTs);
    CHECK(Count(ds, "no-unused-vars") == 0);
    CHECK(Count(Diags("interface Unused<T> {}\ntype U2 = string;\nimport { x } from 'y';\n", JsLspDialect::Dts),
                "no-unused-vars") == 0);
    // `import x = require()`, `import y = A.B`, `export import`.
    ds = Diags("import * as NS from 'ns';\nimport eq = require('eq');\nimport eq2 = NS.Thing;\nexport import eq3 = NS.Other;\n",
               kTs);
    CHECK(HasUnused(ds, "eq") && HasUnused(ds, "eq2") && !HasUnused(ds, "eq3") && !HasUnused(ds, "NS"));
    // `typeof x` in a type: a value used only as a type.
    ds = Diags("const x9 = { a: 1 };\nexport type X9 = typeof x9;\n", kTs);
    const JsLspDiagnostic *only = Find(ds, "no-unused-vars");
    CHECK(only && only->message == "'x9' is assigned a value but only used as a type.");
    // Qualified names, heritage, decorators, type arguments, `satisfies`, enum member scope.
    ds = Diags("import * as Q from 'q';\nimport { Base, Iface, dec, Arg, Sat } from 'r';\n"
               "export interface J extends Iface {}\n"
               "@dec export class Kk extends Base<Arg> implements Q.Thing { @dec f = 1; }\n"
               "export const s = {} satisfies Sat;\n",
               kTs);
    CHECK(Count(ds, "no-unused-vars") == 0);
    // `export default Iface` and `export { type T }` name types.
    ds = Diags("interface Dflt {}\nexport default Dflt;\ntype T1 = 1;\nexport { type T1 };\n", kTs);
    CHECK(Count(ds, "no-unused-vars") == 0);
    // A local value merging with an imported type takes the import's place.
    ds = Diags("import { Src } from 'm';\nlet Src: () => Src;\nexport function mk() { return Src || (Src = () => 1 as any); }\n",
               kTs);
    CHECK(Count(ds, "no-import-assign") == 0);
}

void TestTsLints() {
    // no-undef is off: tsc checks names (with the project's lib and @types).
    CHECK(Count(Diags("export const a = documnt.title;\n", kTs), "no-undef") == 0);
    CHECK(Count(Diags("export const a = documnt.title;\n"), "no-undef") == 1);
    // no-dupe-class-members allows overloads and `declare` fields, not real duplicates.
    auto ds = Diags("export class D {\n  m(): void;\n  m(y?: number) { return y; }\n  n() {}\n  n() {}\n}\n", kTs);
    CHECK(Count(ds, "no-dupe-class-members") == 1);
    // no-redeclare allows declaration merging, not `var x; var x;`.
    ds = Diags("export interface M { a: 1 }\nexport interface M { b: 2 }\nexport class M {}\nexport namespace M {}\n"
               "export function f() {}\nexport namespace f { export const k = 1; }\nvar r = 1; var r = 2;\nexport { r };\n",
               kTs);
    CHECK(Count(ds, "no-redeclare") == 1);
    // Declarations that emit nothing are fine after a return.
    ds = Diags("export function q() {\n  return 1;\n  interface X {}\n  type Y = 1;\n  const enum Z { A }\n}\n", kTs);
    CHECK(Count(ds, "no-unreachable") == 0);
    CHECK(Count(Diags("export function q() {\n  return 1;\n  enum Z { A }\n}\n", kTs), "no-unreachable") == 1);
    // A type and a value of one name in nested scopes are not shadowing.
    ds = Diags("type T = number;\nexport function f2(x: T) { if (x) { const T = 1; return T; } }\n", kTs);
    CHECK(Count(ds, "no-shadow") == 0);
    // The other rules still apply.
    CHECK(Count(Diags("export const k = 1;\nexport function z() { k = 2; }\n", kTs), "no-const-assign") == 1);
    CHECK(Count(Diags("export const o = { a: 1, a: 2 };\n", kTs), "no-dupe-keys") == 1);
}

void TestTsFeatures() {
    const std::string src =
        "/** A point. */\n"                                         // 0
        "export interface Point { x: number; /** why */ y?: number }\n"  // 1
        "export type Pair<T> = [T, T];\n"                           // 2
        "export enum Color { Red = 1, Green }\n"                    // 3
        "export function dist(a: Point, b: Point = a): number { return a.x - b.x; }\n"  // 4
        "export const p: Point = { x: 1 };\n"                       // 5
        "export class Svc { constructor(private readonly url: string) {} get() { return this.url; } }\n"  // 6
        "export let s: string = '';\n"                              // 7
        "export let q: Promise<number>;\n";                         // 8
    auto doc = A(src, kTs);
    JsLspHoverInfo h = JsLspHover(*doc, 1, 18);  // Point
    CHECK(h.found && Contains(h.text, "```ts") && Contains(h.text, "interface Point") && Contains(h.text, "A point."));
    h = JsLspHover(*doc, 4, 27);  // Point in `a: Point`: a type reference
    CHECK(h.found && Contains(h.text, "interface Point"));
    h = JsLspHover(*doc, 2, 13);  // Pair
    CHECK(h.found && Contains(h.text, "type Pair<T> = [T, T]"));
    h = JsLspHover(*doc, 2, 18);  // T
    CHECK(h.found && Contains(h.text, "(type parameter) T"));
    h = JsLspHover(*doc, 3, 21);  // Red
    CHECK(h.found && Contains(h.text, "(enum member) Color.Red = 1"));
    h = JsLspHover(*doc, 4, 16);  // dist
    CHECK(h.found && Contains(h.text, "function dist(a: Point, b: Point = a): number"));
    h = JsLspHover(*doc, 4, 31);  // b
    CHECK(h.found && Contains(h.text, "(parameter) b: Point = a"));
    h = JsLspHover(*doc, 5, 13);  // p
    CHECK(h.found && Contains(h.text, "const p: Point"));
    h = JsLspHover(*doc, 6, 49);  // url (parameter property)
    CHECK(h.found && Contains(h.text, "(property) private readonly url: string"));
    h = JsLspHover(*doc, 1, 48);  // y member signature, with its doc
    CHECK(h.found && Contains(h.text, "y?: number") && Contains(h.text, "why"));
    h = JsLspHover(*doc, 8, 15);  // Promise: the TypeScript lib's documentation
    CHECK(h.found && Contains(h.text, "Promise"));
    // Definition and references follow the type namespace.
    auto defs = JsLspDefinition(*doc, 4, 27);
    CHECK(defs.size() == 1 && defs[0].range.line == 1 && defs[0].range.col == 17);
    CHECK(JsLspReferences(*doc, 1, 18).size() == 4);  // the declaration + 3 type references
    // Completion: `x: |` offers types, not values; elsewhere values, not types.
    auto with_line = [&](const std::string &line) {
        std::string text = src + line;
        auto d = A(text, kTs);
        return JsLspCompletions(*d, 9, static_cast<int>(line.size()));
    };
    auto items = with_line("let z: ");
    CHECK(HasLabel(items, "Point") && HasLabel(items, "Pair") && HasLabel(items, "Color") && HasLabel(items, "string") &&
          HasLabel(items, "Promise") && !HasLabel(items, "dist") && !HasLabel(items, "p"));
    items = with_line("let z2 = 1 as P");
    CHECK(HasLabel(items, "Point") && HasLabel(items, "Pair") && !HasLabel(items, "p"));
    items = with_line("di");
    CHECK(HasLabel(items, "dist") && !HasLabel(items, "Point"));
    // `x: T` members: the interface's, and the lib's for a lib type.
    items = with_line("p.");
    CHECK(HasLabel(items, "x") && HasLabel(items, "y"));
    items = with_line("s.");
    CHECK(HasLabel(items, "toUpperCase"));
    items = with_line("q.");  // q: Promise<number>, half-typed (the statement does not parse)
    CHECK(HasLabel(items, "then") && HasLabel(items, "catch"));
    // Outline: interfaces with members, enums with members, parameter properties as fields.
    auto syms = JsLspSymbols(*doc);
    auto find = [&](const std::string &name) -> const JsLspSymbol * {
        for (const auto &sy : syms) {
            if (sy.name == name) return &sy;
        }
        return nullptr;
    };
    CHECK(find("Point") && find("Point")->kind == JsLspSymbolKind::Interface);
    CHECK(find("y") && find("y")->kind == JsLspSymbolKind::Property);
    CHECK(find("Color") && find("Color")->kind == JsLspSymbolKind::Enum);
    CHECK(find("Green") && find("Green")->kind == JsLspSymbolKind::EnumMember);
    CHECK(find("url") && find("url")->kind == JsLspSymbolKind::Field);
    CHECK(find("dist") && find("dist")->detail == "(a: Point, b: Point = a): number");
    // Overloads are listed once.
    auto doc2 = A("export function o(a: string): void;\nexport function o(a: number): void;\nexport function o(a: any) {}\n", kTs);
    int n = 0;
    for (const auto &sy : JsLspSymbols(*doc2)) n += sy.name == "o";
    CHECK(n == 1);
    // Rename a type renames its type references.
    std::string err;
    auto edits = JsLspRename(*doc, 1, 18, "Pt", &err);
    CHECK(err.empty() && edits.size() == 4);
}

void TestUnused() {
    auto ds = Diags("import fs from 'fs';\nimport { a, b } from 'm';\nexport const c = a;\n");
    CHECK(Count(ds, "no-unused-vars") == 2);
    CHECK(Find(ds, "no-unused-vars")->message == "'fs' is defined but never used.");
    CHECK(Find(ds, "no-unused-vars")->unnecessary);
    // assigned but never read
    ds = Diags("export function f() {\n  let x = 1;\n  x = 2;\n}\n");
    CHECK(Count(ds, "no-unused-vars") == 1);
    // ESLint reports the last assignment (so eslint-disable-next-line above it works).
    CHECK(Find(ds, "no-unused-vars")->message == "'x' is assigned a value but never used." &&
          Find(ds, "no-unused-vars")->range.line == 2);
    // `x++;` and `x += 1;` alone do not use x.
    CHECK(Count(Diags("export function f() {\n  let n = 0;\n  n++;\n  n += 2;\n}\n"), "no-unused-vars") == 1);
    CHECK(Count(Diags("export function f() {\n  let n = 0;\n  n = n + 1;\n}\n"), "no-unused-vars") == 1);
    CHECK(Count(Diags("export function f() {\n  let n = 0;\n  return n++;\n}\n"), "no-unused-vars") == 0);
    // A function only calling itself is unused.
    CHECK(Count(Diags("export {};\nfunction f(n) { return n ? f(n - 1) : 0; }\n"), "no-unused-vars") == 1);
    // Exports, `_` names, rest siblings and /* exported */ count as used.
    CHECK(Count(Diags("export function f() {}\nexport const g = 1;\nconst h = 2;\nexport { h };\n"), "no-unused-vars") == 0);
    CHECK(Count(Diags("export function f() { const _ignored = 1; }\n"), "no-unused-vars") == 0);
    CHECK(Count(Diags("export function f(o) { const { a, ...rest } = o; return rest; }\n"), "no-unused-vars") == 0);
    // A browser script's top-level names are globals; a node script's are not.
    CHECK(Count(Diags("function onLoad() {}\nvar config = 1;\n"), "no-unused-vars") == 0);
    CHECK(Count(Diags("const fs = require('fs');\nfunction helper() {}\n"), "no-unused-vars") == 2);
    CHECK(Count(Diags("/* exported onLoad */\nconst fs = require('fs');\nfunction onLoad() {}\nfs.x();\n"),
                "no-unused-vars") == 0);
    // JSX uses components, and classic-runtime JSX uses React.
    CHECK(Count(Diags("import React from 'react';\nimport Foo from './Foo';\nexport const App = () => <Foo />;\n"),
                "no-unused-vars") == 0);
    CHECK(Count(Diags("import Foo from './Foo';\nimport * as UI from './ui';\nexport const App = () => <UI.Box><Foo/></UI.Box>;\n"),
                "no-unused-vars") == 0);
    // A lowercase JSX tag is an element, not a variable.
    CHECK(Count(Diags("import div from './d';\nexport const App = () => <div />;\n"), "no-unused-vars") == 1);
}

void TestUnusedParams() {
    // After-used, and only for functions with their own signature: a hint.
    auto ds = Diags("export {};\nfunction f(a, b, c) { return b; }\nf(1, 2, 3);\n");
    CHECK(Count(ds, "no-unused-vars") == 1);
    const JsLspDiagnostic *d = Find(ds, "no-unused-vars");
    CHECK(d->severity == JsLspSeverity::Hint && d->message == "'c' is defined but never used.");
    // A callback implements someone else's interface.
    CHECK(Count(Diags("export const xs = [1].map((v, i) => 1);\n"), "no-unused-vars") == 0);
    CHECK(Count(Diags("export {};\nfunction handler(e) {}\ndocument.addEventListener('click', handler);\n"),
                "no-unused-vars") == 0);
    // A method too.
    CHECK(Count(Diags("export class A { m(x) { return 1; } }\n"), "no-unused-vars") == 0);
    // A destructured name has no position to keep.
    CHECK(Count(Diags("export {};\nconst f = ({ a, b }, c) => b + c;\nf();\n"), "no-unused-vars") == 1);
    // Catch parameters: hint.
    ds = Diags("export function f() { try { g(); } catch (err) { return 1; } }\n");
    CHECK(Count(ds, "no-unused-vars") == 1 && Find(ds, "no-unused-vars")->severity == JsLspSeverity::Hint);
    // A setter's parameter is required.
    CHECK(Count(Diags("export const o = { set x(v) {} };\n"), "no-unused-vars") == 0);
}

void TestUndef() {
    // Only near misses of names this server knows are reported.
    auto ds = Diags("documnet.body.appendChild(x);\n");
    CHECK(Count(ds, "no-undef") == 1);
    CHECK(Find(ds, "no-undef")->message == "'documnet' is not defined. Did you mean 'document'?");
    CHECK(Count(Diags("myLibraryGlobal.init();\n"), "no-undef") == 0);
    // A local in scope.
    ds = Diags("export function f() { const counter = 1; return countr + 1; }\n");
    CHECK(Count(ds, "no-undef") == 1 && Contains(Find(ds, "no-undef")->message, "'counter'"));
    // typeof never throws; /* global */ declares.
    CHECK(Count(Diags("if (typeof documnet !== 'undefined') {}\n"), "no-undef") == 0);
    CHECK(Count(Diags("/* global documnet */\ndocumnet.x();\n"), "no-undef") == 0);
    // A name used again and again is a global the file expects, not a typo.
    CHECK(Count(Diags("consol.log(1);\nconsol.log(2);\nconsol.log(3);\n"), "no-undef") == 0);
    // Host globals: node, test runners.
    CHECK(Count(Diags("describe('x', () => { it('y', () => { expect(require('fs')).toBeTruthy(); }); });\n"),
                "no-undef") == 0);
    // In a module, assigning an undeclared name throws.
    ds = Diags("import x from 'x';\ntotal = x;\n");
    CHECK(Count(ds, "no-undef") == 1);
    // ... in a sloppy script it creates a global.
    CHECK(Count(Diags("total = 1;\nconsole.log(total);\n"), "no-undef") == 0);
    // with/eval: nothing can be known.
    CHECK(Count(Diags("with (o) { documnet.x; }\n"), "no-undef") == 0);
}

void TestAssignments() {
    auto ds = Diags("const x = 1;\nx = 2;\nexport { x };\n");
    CHECK(Count(ds, "no-const-assign") == 1 && Find(ds, "no-const-assign")->severity == JsLspSeverity::Error);
    CHECK(Find(ds, "no-const-assign")->message == "'x' is constant.");
    CHECK(Count(Diags("for (const k in o) { k; }\nfor (const v of a) { v; }\n"), "no-const-assign") == 0);
    CHECK(Count(Diags("import a from 'a';\na = 1;\n"), "no-import-assign") == 1);
    CHECK(Count(Diags("function f() {}\nf = 1;\n"), "no-func-assign") == 1);
    CHECK(Count(Diags("class C {}\nC = 1;\n"), "no-class-assign") == 1);
    ds = Diags("let y = 1;\ny = y;\nconsole.log(y);\nconst o = {};\no.a.b = o.a.b;\n[y, o] = [y, o];\n");
    CHECK(Count(ds, "no-self-assign") == 4);
    CHECK(Count(Diags("let y = 1;\ny = -y;\ny = y + 1;\nconsole.log(y);\n"), "no-self-assign") == 0);
    // var redeclared
    CHECK(Count(Diags("var a = 1;\nvar a = 2;\n"), "no-redeclare") == 1);
}

void TestStructure() {
    CHECK(Count(Diags("const o = { a: 1, b: 2, a: 3, 'b': 4, 1: 0, '1': 0 };\n"), "no-dupe-keys") == 3);
    CHECK(Count(Diags("const o = { get a() { return 1; }, set a(v) {} };\n"), "no-dupe-keys") == 0);
    CHECK(Count(Diags("const o = { get a() { return 1; }, a: 2 };\n"), "no-dupe-keys") == 1);
    CHECK(Count(Diags("class A { m() {} m() {} static m() {} get g() { return 1; } set g(v) {} }\n"),
                "no-dupe-class-members") == 1);
    CHECK(Count(Diags("switch (x) { case 1: break; case 'a': break; case 1: break; case f(): break; case f(): break; }\n"),
                "no-duplicate-case") == 2);  // same text, as ESLint compares them
    // unreachable
    auto ds = Diags("function f() {\n  return 1;\n  g();\n  h();\n}\n");
    CHECK(Count(ds, "no-unreachable") == 1);
    const JsLspDiagnostic *u = Find(ds, "no-unreachable");
    CHECK(u->range.line == 2 && u->range.end_line == 3 && u->unnecessary);
    CHECK(Count(Diags("function f() {\n  return g();\n  function g() {}\n  var x;\n}\n"), "no-unreachable") == 0);
    CHECK(Count(Diags("function f(a) {\n  if (a) return 1; else throw a;\n  g();\n}\n"), "no-unreachable") == 1);
    CHECK(Count(Diags("function f(a) {\n  if (a) return 1;\n  g();\n}\n"), "no-unreachable") == 0);
    CHECK(Count(Diags("for (;;) { if (a) { break; } else { continue; } x(); }\n"), "no-unreachable") == 1);
    CHECK(Count(Diags("function f(a) { switch (a) { case 1: return; f(); case 2: g(); } }\n"), "no-unreachable") == 1);
    CHECK(Count(Diags("function f() { try { return 1; } finally { g(); } h(); }\n"), "no-unreachable") == 1);
    CHECK(Count(Diags("function f() { try { return 1; } catch (e) { g(e); } h(); }\n"), "no-unreachable") == 0);
    // NaN, self-compare, typeof, negation
    CHECK(Count(Diags("if (x == NaN || x === Number.NaN) {}\n"), "use-isnan") == 2);
    CHECK(Count(Diags("switch (NaN) { case NaN: break; }\n"), "use-isnan") == 2);
    CHECK(Count(Diags("if (x === x) {}\nif (a.b < a.b) {}\n"), "no-self-compare") == 2);
    CHECK(Count(Diags("if (x !== x) {}\n"), "no-self-compare") == 0);  // the NaN test
    CHECK(Count(Diags("if (typeof x === 'strng' || typeof y == 'object' || 'fucntion' !== typeof z) {}\n"),
                "valid-typeof") == 2);
    CHECK(Count(Diags("if (!key in obj) {}\nif (!(key in obj)) {}\nif ((!a) in b) {}\n"), "no-unsafe-negation") == 1);
    // empty blocks: info; a comment counts as content; function bodies are fine.
    ds = Diags("if (a) {}\nif (b) { /* nothing */ }\nfunction f() {}\ntry { g(); } catch (e) {}\nswitch (a) {}\n");
    CHECK(Count(ds, "no-empty") == 3 && Find(ds, "no-empty")->severity == JsLspSeverity::Information);
    // duplicate params in sloppy code (strict code is a syntax error)
    CHECK(Count(Diags("function f(a, a) { return a; }\nf();\n"), "no-dupe-args") == 1);
    // shadowing inside one function: info
    ds = Diags("export function f(a) {\n  const x = 1;\n  if (a) {\n    const x = 2;\n    return x;\n  }\n  return x;\n}\n");
    CHECK(Count(ds, "no-shadow") == 1 && Find(ds, "no-shadow")->severity == JsLspSeverity::Information);
    CHECK(Count(Diags("export const x = 1;\nexport function f() { const x = 2; return x; }\n"), "no-shadow") == 0);
}

void TestDirectives() {
    const std::string bad = "if (x == NaN) {}\n";
    CHECK(Count(Diags(bad), "use-isnan") == 1);
    CHECK(Count(Diags("if (x == NaN) {} // eslint-disable-line\n"), "use-isnan") == 0);
    CHECK(Count(Diags("if (x == NaN) {} // eslint-disable-line use-isnan\n"), "use-isnan") == 0);
    CHECK(Count(Diags("if (x == NaN) {} // eslint-disable-line no-undef\n"), "use-isnan") == 1);
    CHECK(Count(Diags("// eslint-disable-next-line use-isnan, no-empty -- legacy\nif (x == NaN) {}\n"), "use-isnan") == 0);
    CHECK(Count(Diags("/* eslint-disable */\nif (x == NaN) {}\n/* eslint-enable */\nif (y == NaN) {}\n"), "use-isnan") == 1);
    CHECK(Count(Diags("/* eslint-disable no-empty */\nif (x == NaN) {}\n"), "use-isnan") == 1);
    // A plugin-qualified name still names our rule.
    CHECK(Count(Diags("export {};\nconst a = 1; // eslint-disable-line @typescript-eslint/no-unused-vars\n"),
                "no-unused-vars") == 0);
}

void TestHover() {
    const std::string src =
        "/**\n"
        " * Adds two numbers.\n"
        " * @param {number} a - the first\n"
        " * @returns {number} the sum\n"
        " */\n"
        "function add(a, b = 2) { return a + b; }\n"
        "const total = add(1);\n"
        "const xs = [1, 2].map(n => n * 2);\n"
        "const el = document.getElementById('x');\n"
        "Math.max(total, 1);\n"
        "el.focus();\n"
        "const cfg = { port: 80, /** the host */ host: 'h' };\n"
        "cfg.port;\n";
    auto doc = A(src);
    JsLspHoverInfo h = JsLspHover(*doc, 5, 10);  // add
    CHECK(h.found && Contains(h.text, "function add(a, b = 2)") && Contains(h.text, "Adds two numbers."));
    CHECK(Contains(h.text, "*@param* `a` `number` — the first"));
    h = JsLspHover(*doc, 6, 15);  // add( in total = add(1)
    CHECK(h.found && Contains(h.text, "function add"));
    h = JsLspHover(*doc, 5, 16);  // b
    CHECK(h.found && Contains(h.text, "(parameter) b = 2"));
    h = JsLspHover(*doc, 5, 13);  // a
    CHECK(h.found && Contains(h.text, "(parameter) a") && Contains(h.text, "the first"));
    h = JsLspHover(*doc, 7, 18);  // map
    CHECK(h.found && Contains(h.text, "Array.prototype.map") && Contains(h.text, "callbackfn"));
    h = JsLspHover(*doc, 9, 6);  // max
    CHECK(h.found && Contains(h.text, "Math.max") && Contains(h.text, "*ECMAScript*"));
    h = JsLspHover(*doc, 8, 22);  // getElementById
    CHECK(h.found && Contains(h.text, "getElementById(elementId: string)"));
    h = JsLspHover(*doc, 8, 7);  // el: its type comes from the DOM's signature
    CHECK(h.found && Contains(h.text, "*Type:* `HTMLElement`"));
    h = JsLspHover(*doc, 10, 4);  // el.focus: HTMLElement's member
    CHECK(h.found && Contains(h.text, "focus"));
    h = JsLspHover(*doc, 12, 5);  // cfg.port: the file's own member
    CHECK(h.found && Contains(h.text, "port: 80"));
    h = JsLspHover(*doc, 11, 41);  // host key with its JSDoc
    CHECK(h.found && Contains(h.text, "the host"));
    h = JsLspHover(*doc, 9, 1);  // Math
    CHECK(h.found && Contains(h.text, "class Math"));
    CHECK(!JsLspHover(*doc, 0, 1).found);  // in a comment
}

void TestCompletion() {
    const std::string src =
        "import { readFile } from 'fs';\n"
        "const config = { port: 80, host: 'x', start() {} };\n"
        "class Box { constructor() { this.width = 1; } area() { return 0; } }\n"
        "function run(alpha, beta) {\n"
        "  const local = 1;\n"
        "  \n"
        "}\n";
    auto doc = A(src);
    auto items = JsLspCompletions(*doc, 5, 2);
    CHECK(HasLabel(items, "local") && HasLabel(items, "alpha") && HasLabel(items, "run") && HasLabel(items, "config") &&
          HasLabel(items, "readFile") && HasLabel(items, "Box") && HasLabel(items, "arguments"));
    CHECK(HasLabel(items, "function") && HasLabel(items, "return"));
    CHECK(IndexOf(items, "local") < IndexOf(items, "config"));  // innermost first
    // After a dot, each on a half-typed line (the usual state when completion is asked for).
    auto member = [&](const std::string &line) {
        std::string text = src;
        const size_t at = text.find("  \n}");
        text.insert(at + 2, line);
        auto d = A(text);
        return JsLspCompletions(*d, 5, 2 + static_cast<int>(line.size()));
    };
    items = member("config.");
    CHECK(HasLabel(items, "port") && HasLabel(items, "host") && HasLabel(items, "start") && !HasLabel(items, "local"));
    items = member("Math.");
    CHECK(HasLabel(items, "max") && HasLabel(items, "PI") && HasLabel(items, "floor"));
    items = member("'abc'.");
    CHECK(HasLabel(items, "toUpperCase") && HasLabel(items, "padStart"));
    items = member("[1, 2].");
    CHECK(HasLabel(items, "push") && HasLabel(items, "map") && HasLabel(items, "length"));
    items = member("document.");
    CHECK(HasLabel(items, "getElementById") && HasLabel(items, "body"));
    items = member("document.body.");
    CHECK(HasLabel(items, "appendChild") && HasLabel(items, "focus"));
    items = member("new Box().");
    CHECK(HasLabel(items, "area"));
    items = member("const b = new Box(); b.");
    CHECK(HasLabel(items, "area") && HasLabel(items, "width"));
    items = JsLspCompletions(*A("class Box { constructor() { this.width = 1; } area() { return this. } }\n"), 0, 67);
    CHECK(HasLabel(items, "width") && HasLabel(items, "area"));
    // A prefix narrows, and the replace range covers it.
    items = JsLspCompletions(*A("const counter = 1;\ncou"), 1, 3);
    CHECK(HasLabel(items, "counter") && !HasLabel(items, "return"));
    CHECK(!items.empty() && items[0].replace_start == 0 && items[0].replace_end == 3);
    items = JsLspCompletions(*A("parseF"), 0, 6);
    CHECK(HasLabel(items, "parseFloat"));
    // Nothing in strings and comments.
    CHECK(JsLspCompletions(*A("const s = 'ab';\n// co\n"), 0, 12).empty());
    CHECK(JsLspCompletions(*A("const s = 'ab';\n// co\n"), 1, 5).empty());
}

void TestSignatures() {
    const std::string src =
        "/** Joins things. */\n"
        "function join(sep, ...parts) { return parts.join(sep); }\n"
        "join(',', a, b, c);\n"
        "parseInt('12', 10);\n"
        "Math.max(1, 2);\n"
        "[1].map(x => x, this);\n"
        "new AbortController();\n"
        "new Point(1, );\n"
        "class Point { constructor(x, y) {} }\n";
    auto doc = A(src);
    JsLspSignatureHelp sh = JsLspSignatureAt(*doc, 2, 5);
    CHECK(sh.found && sh.label == "join(sep, ...parts)" && sh.params.size() == 2 && sh.active_param == 0);
    CHECK(Contains(sh.documentation, "Joins things."));
    sh = JsLspSignatureAt(*doc, 2, 15);
    CHECK(sh.found && sh.active_param == 1);  // the rest parameter absorbs the rest
    sh = JsLspSignatureAt(*doc, 3, 15);
    CHECK(sh.found && Contains(sh.label, "parseInt(string: string, radix") && sh.params.size() == 2 && sh.active_param == 1);
    sh = JsLspSignatureAt(*doc, 4, 9);
    CHECK(sh.found && Contains(sh.label, "max(...values: number[])"));
    sh = JsLspSignatureAt(*doc, 5, 16);
    CHECK(sh.found && Contains(sh.label, "map") && sh.active_param == 1);
    sh = JsLspSignatureAt(*doc, 7, 13);
    CHECK(sh.found && sh.label == "Point(x, y)" && sh.active_param == 1);
    CHECK(!JsLspSignatureAt(*doc, 1, 3).found);
}

void TestNavigation() {
    const std::string src =
        "let count = 0;\n"
        "function bump(step) {\n"
        "  count += step;\n"
        "  return count;\n"
        "}\n"
        "{ let count = 5; count++; }\n"
        "outer: for (;;) { break outer; }\n"
        "const o = { go() {} };\n"
        "o.go();\n";
    auto doc = A(src);
    auto defs = JsLspDefinition(*doc, 3, 10);
    CHECK(defs.size() == 1 && defs[0].range.line == 0 && defs[0].range.col == 4);
    defs = JsLspDefinition(*doc, 5, 18);  // the inner count
    CHECK(defs.size() == 1 && defs[0].range.line == 5 && defs[0].range.col == 6);
    defs = JsLspDefinition(*doc, 6, 26);  // break outer
    CHECK(defs.size() == 1 && defs[0].range.line == 6 && defs[0].range.col == 0);
    defs = JsLspDefinition(*doc, 8, 3);  // o.go
    CHECK(defs.size() == 1 && defs[0].range.line == 7 && defs[0].range.col == 12);
    auto refs = JsLspReferences(*doc, 0, 5);
    CHECK(refs.size() == 3);  // declaration + 2 uses; the inner block's count is another variable
    CHECK(refs[0].is_definition && refs[1].is_write && !refs[2].is_write);
    refs = JsLspReferences(*doc, 6, 1);
    CHECK(refs.size() == 2);
}

void TestRename() {
    const std::string src =
        "const a = 1;\n"
        "function f(x) {\n"
        "  const y = 2;\n"
        "  return a + x + y;\n"
        "}\n"
        "const o = { a };\n"
        "const { a: q } = o;\n"
        "export { a };\n";
    auto doc = A(src);
    std::string err;
    JsLspRenameTarget t = JsLspPrepareRename(*doc, 0, 6);
    CHECK(t.found && t.placeholder == "a");
    auto edits = JsLspRename(*doc, 0, 6, "alpha", &err);
    CHECK(err.empty() && edits.size() == 4);
    CHECK(edits[2].new_text == "a: alpha");     // shorthand property keeps its key
    CHECK(edits[3].new_text == "alpha as a");   // the export keeps its name
    // Captures are refused both ways.
    JsLspRename(*doc, 0, 6, "y", &err);  // a's use inside f would find f's y
    CHECK(Contains(err, "captured"));
    err.clear();
    JsLspRename(*doc, 2, 9, "a", &err);  // y -> a would hide the outer a used in f
    CHECK(Contains(err, "hide"));
    err.clear();
    JsLspRename(*doc, 1, 11, "f", &err);  // the parameter x -> f is fine (f is outer and unused inside)
    CHECK(err.empty());
    JsLspRename(*doc, 0, 6, "o", &err);
    CHECK(Contains(err, "already declared"));
    err.clear();
    JsLspRename(*doc, 0, 6, "class", &err);
    CHECK(Contains(err, "not a valid"));
    err.clear();
    // Imports keep the imported name; shorthand patterns their key.
    doc = A("import { readFile } from 'fs';\nconst { port } = cfg;\nreadFile(port);\n");
    edits = JsLspRename(*doc, 2, 1, "rf", &err);
    CHECK(err.empty() && edits.size() == 2 && edits[0].new_text == "readFile as rf" && edits[1].new_text == "rf");
    edits = JsLspRename(*doc, 1, 9, "p", &err);
    CHECK(err.empty() && edits.size() == 2 && edits[0].new_text == "port: p");
    // Labels.
    doc = A("loop: for (;;) { continue loop; }\n");
    edits = JsLspRename(*doc, 0, 27, "again", &err);
    CHECK(err.empty() && edits.size() == 2);
    // Globals and members are not renamed.
    CHECK(!JsLspPrepareRename(*A("console.log(1);\n"), 0, 2).found);
}

void TestSymbols() {
    const std::string src =
        "import x from 'x';\n"
        "export function main(argv) {\n"
        "  function inner() {}\n"
        "  const helper = () => 1;\n"
        "  const local = 3;\n"
        "}\n"
        "export default class Shape extends Base {\n"
        "  static count = 0;\n"
        "  constructor() { super(); }\n"
        "  area() { return 0; }\n"
        "  get name() { return 'x'; }\n"
        "}\n"
        "const api = { get() {}, url: 'x' };\n"
        "module.exports.run = function () {};\n"
        "describe('suite', () => { it('works', () => {}); });\n";
    auto syms = JsLspSymbols(*A(src));
    auto find = [&](const std::string &n) -> const JsLspSymbol * {
        for (const auto &s : syms) {
            if (s.name == n) return &s;
        }
        return nullptr;
    };
    CHECK(find("main") && find("main")->kind == JsLspSymbolKind::Function && find("main")->detail == "(argv)");
    CHECK(find("inner") && find("inner")->parent >= 0 && syms[static_cast<size_t>(find("inner")->parent)].name == "main");
    CHECK(find("helper") && !find("local"));
    CHECK(find("Shape") && find("Shape")->kind == JsLspSymbolKind::Class && find("Shape")->detail == "extends Base");
    CHECK(find("static count") && find("static count")->kind == JsLspSymbolKind::Field);
    CHECK(find("constructor") && find("constructor")->kind == JsLspSymbolKind::Constructor);
    CHECK(find("area") && find("name") && find("name")->kind == JsLspSymbolKind::Property);
    CHECK(find("api") && find("get") && find("url"));
    CHECK(find("module.exports.run"));
    CHECK(find("suite") && find("works") && find("works")->parent >= 0);
}

void TestFolds() {
    const std::string src =
        "import a from 'a';\n"
        "import b from 'b';\n"
        "/*\n"
        " * header\n"
        " */\n"
        "function f() {\n"
        "  const o = {\n"
        "    x: 1,\n"
        "  };\n"
        "}\n"
        "// one\n"
        "// two\n";
    auto folds = JsLspFolds(*A(src));
    bool imports = false, comment = false, fn = false, obj = false, run = false;
    for (const auto &f : folds) {
        if (f.kind == "imports" && f.start_line == 0 && f.end_line == 1) imports = true;
        if (f.kind == "comment" && f.start_line == 2 && f.end_line == 4) comment = true;
        if (f.kind.empty() && f.start_line == 5 && f.end_line == 8) fn = true;
        if (f.kind.empty() && f.start_line == 6 && f.end_line == 7) obj = true;
        if (f.kind == "comment" && f.start_line == 10 && f.end_line == 11) run = true;
    }
    CHECK(imports && comment && fn && obj && run);
}

void TestPositions() {
    // Columns are bytes: a two-byte character before the error moves it by two.
    auto ds = Diags("const é = 1; const = 2;\n");
    const JsLspDiagnostic *e = Find(ds, "syntax-error");
    CHECK(e && e->range.col == 20);
    auto doc = A("const naïve = 1;\nnaïve;\n");
    CHECK(JsLspHover(*doc, 1, 3).found);
    auto refs = JsLspReferences(*doc, 1, 2);
    CHECK(refs.size() == 2 && refs[0].range.end_col == 12);
}

void TestFormatDiagnostic() {
    JsLspDiagnostic d;
    d.range.line = 2;
    d.range.col = 4;
    d.severity = JsLspSeverity::Warning;
    d.code = "no-unused-vars";
    d.message = "'x' is defined but never used.";
    CHECK(JsLspFormatDiagnostic("a.js", d) == "a.js:3:5: warning: 'x' is defined but never used. [no-unused-vars]");
    CHECK(JsLspDiagnosticFails(d));
    d.severity = JsLspSeverity::Hint;
    CHECK(!JsLspDiagnosticFails(d));
}

void TestRobustness() {
    // Half-typed code still answers.
    auto doc = A("function f(a) {\n  const s = 'x';\n  s.\n}\n");
    auto items = JsLspCompletions(*doc, 2, 4);
    CHECK(HasLabel(items, "toUpperCase"));
    doc = A("");
    CHECK(JsLspDiagnostics(*doc).empty() && JsLspSymbols(*doc).empty());
    CHECK(!JsLspHover(*doc, 0, 0).found);
    // Deep nesting does not overflow.
    std::string deep;
    for (int i = 0; i < 3000; i++) deep += "[";
    CHECK(!Diags(deep).empty());
}

}  // namespace

int main() {
    TestDialects();
    TestSyntax();
    TestTsSyntax();
    TestTsUnused();
    TestTsLints();
    TestTsFeatures();
    TestUnused();
    TestUnusedParams();
    TestUndef();
    TestAssignments();
    TestStructure();
    TestDirectives();
    TestHover();
    TestCompletion();
    TestSignatures();
    TestNavigation();
    TestRename();
    TestSymbols();
    TestFolds();
    TestPositions();
    TestFormatDiagnostic();
    TestRobustness();
    if (g_failed) {
        std::fprintf(stderr, "js_lsp_test: %d of %d checks failed\n", g_failed, g_checks);
        return 1;
    }
    std::printf("js_lsp_test: %d checks passed\n", g_checks);
    return 0;
}
