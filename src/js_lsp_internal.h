#ifndef MEP_JS_LSP_INTERNAL_H
#define MEP_JS_LSP_INTERNAL_H

// What js_lsp.cpp (diagnostics), js_lsp_features.cpp (hover, completion,
// navigation) and js_lsp_scope.cpp (the analysis that fills it in) share:
// the scope model and the analysed document. Not part of js_lsp.h's API.
//
// The scope model follows eslint-scope (BSD-2, the analysis ESLint itself
// uses), written afresh over js_parse's tree:
//   - scopes: global (script) or module, function, the name scope of a
//     named function/class expression, block, for (let/const heads),
//     switch, catch, class, with;
//   - variables: var and function declarations hoist to the nearest
//     function (or global/module) scope, let/const/class/using stay in
//     their block, parameters and the implicit `arguments` belong to the
//     function, a catch parameter to its catch clause, imports to the
//     module;
//   - references: every identifier read or written in an expression, with
//     the scope it was written in; resolved after the walk (so hoisting and
//     use-before-declaration need nothing special) by looking up the chain.
//     A reference nothing declares is "through": a global.
//
// TypeScript adds a second namespace, the way typescript-eslint's
// scope-manager does: a variable is a value, a type or both (Var::ns), and
// a reference looks for one or the other (Ref::ns) -- a type annotation
// finds an interface where an expression would walk past it. One variable
// per name and scope, so declaration merging (`interface A` + `class A`,
// `const X` + `type X`, a function and its overload signatures, a
// namespace merged into anything) is one variable with several ids.
// Interfaces, type aliases, type parameters and enum members are new kinds;
// a mapped type, a conditional type's `infer`, a function type or a
// signature gets a scope of its own (ScopeKind::Type); a namespace body is
// function-like (ScopeKind::TsModule).

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "js_lsp.h"
#include "js_lsp_dts.h"
#include "js_lsp_vocab.h"
#include "js_parse.h"

namespace jslsp {

using jsparse::Node;
using jsparse::NodeType;

enum class VarKind : uint8_t {
    Var,
    Let,
    Const,
    Using,
    Function,      // a function declaration
    Class,         // a class declaration (the outer binding)
    Param,
    CatchParam,
    Import,
    Arguments,     // the implicit `arguments` of a non-arrow function
    FunctionName,  // the name of a named function expression, inside it
    ClassName,     // a class's own name inside its body
    // TypeScript
    Interface,     // interface I {}
    TypeAlias,     // type T = ...
    TypeParam,     // <T>, `infer T`, a mapped type's key
    Enum,          // enum E {} / const enum E {}
    EnumMember,    // a member, in scope inside the enum's own initializers
    Namespace,     // namespace N {} / module N {}
};

// Var::ns / Ref::ns bits.
enum : uint8_t { kNsValue = 1, kNsType = 2, kNsAny = 3 };

enum class ScopeKind : uint8_t {
    Global, Module, Function, FunctionName, Block, For, Switch, Catch, Class, With,
    TsModule,  // a namespace / module body (var hoists to it, as to a function)
    Type,      // type parameters and `infer` names of an interface, alias, signature, mapped or conditional type
};

struct Scope;
struct Var;

struct Ref {
    const Node *id = nullptr;  // Identifier or JSXIdentifier
    Scope *from = nullptr;
    Var *var = nullptr;        // what it resolves to (null: a global)
    bool read = false, write = false;
    bool for_itself = false;   // a read only to update the same variable: `x++;`, `x += 1;`, `x = x + 1;`
    bool type_of = false;      // `typeof x`
    bool jsx = false;          // a JSX tag name
    bool export_ref = false;   // `export { x }`
    bool shorthand = false;    // `{x}` in an object literal (rename must write `x: y`)
    bool in_type = false;      // written in a type (`x: T`, `typeof x` in a type)
    uint8_t ns = kNsValue;     // what it can name: a value, a type, or either (an export)
};

struct Var {
    std::string_view name;
    VarKind kind = VarKind::Var;
    Scope *scope = nullptr;
    std::vector<const Node *> ids;  // declaring identifiers; ids[0] is the first
    const Node *decl = nullptr;     // VariableDeclarator, Function*, Class*, Import*Specifier, CatchClause, function (Param)
    const Node *init = nullptr;     // the value bound by the declaration, when it is the identifier's own
    bool has_value = false;         // the declaration gives it a value (an initializer, a loop, a pattern's source)
    int param_index = -1;           // Param: which top-level parameter it is (or is inside)
    bool in_pattern = false;        // declared inside a destructuring pattern
    bool rest_sibling = false;      // `const {a, ...rest} = o`: a is a rest sibling
    bool shorthand_pattern = false; // `const {a} = o` / `function f({a})` (rename writes `a: b`)
    bool exported = false;
    bool redeclared = false;        // `var x; var x;`
    uint8_t ns = kNsValue;          // a value, a type, or both (class, enum, namespace, import)
    bool ambient = false;           // `declare`, inside `declare namespace/module/global`, or a .d.ts
    bool param_property = false;    // `constructor(private x)`: also a member
    bool overloaded = false;        // a function with overload signatures
    bool signature_only = false;    // so far only overload signatures declare it
    bool no_unused = false;         // never reported unused (a mapped type's key)
    bool type_only_import = false;  // `import type`
    std::vector<Ref *> refs;
};

struct Scope {
    ScopeKind kind = ScopeKind::Block;
    Scope *parent = nullptr;
    const Node *node = nullptr;
    uint32_t start = 0, end = 0;  // byte range
    bool strict = false;
    bool dynamic = false;  // a `with` or a direct `eval` inside: names may resolve anywhere
    std::vector<Scope *> kids;
    std::vector<Var *> vars;
    std::unordered_map<std::string_view, Var *> names;

    bool IsFunctionLike() const {
        return kind == ScopeKind::Function || kind == ScopeKind::Global || kind == ScopeKind::Module ||
               kind == ScopeKind::TsModule;
    }
    Var *Find(std::string_view n) const {
        auto it = names.find(n);
        return it == names.end() ? nullptr : it->second;
    }
};

// What an identifier at a position is (for hover, definition, rename).
struct Hit {
    uint32_t start = 0, end = 0;
    const Node *node = nullptr;  // the identifier
    Var *var = nullptr;          // declared or referenced variable
    Ref *ref = nullptr;          // the reference (null for a declaration)
    bool member = false;         // a non-computed property name (a.b's b, {b: 1}'s b, class member b)
    bool label = false;          // a statement label
};

struct Directives {
    // `// eslint-disable-line`, `// eslint-disable-next-line`, `/* eslint-disable */` regions.
    struct Region {
        uint32_t from = 0, to = UINT32_MAX;  // byte offsets
        std::vector<std::string> rules;      // empty: every rule
    };
    std::vector<Region> regions;
    std::unordered_map<int, std::vector<std::vector<std::string>>> lines;  // line -> rule lists ({} = all)
    std::unordered_set<std::string> globals;   // /* global a, b */ and /* eslint-env */'s sets
    std::unordered_set<std::string> exported;  // /* exported a */
    std::vector<std::string> envs;
};

}  // namespace jslsp

class JsLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    std::vector<int> line_starts;
    JsLspDialect dialect = JsLspDialect::Auto;
    bool lint = true;
    bool typescript = false;  // a .ts/.tsx/.d.ts document (parsed with js_parse's TypeScript dialect)
    bool dts = false;         // a declaration file
    jsparse::ParseResult parse;

    std::vector<std::unique_ptr<jslsp::Scope>> scopes;  // scopes[0] is the global/module scope
    std::vector<std::unique_ptr<jslsp::Var>> vars;
    std::vector<std::unique_ptr<jslsp::Ref>> refs;
    std::vector<jslsp::Ref *> through;                   // references nothing declares (globals)
    std::unordered_set<std::string_view> implicit_globals;  // assigned without a declaration
    std::unordered_map<const jsparse::Node *, const jsparse::Node *> parent;
    std::unordered_map<const jsparse::Node *, jslsp::Var *> decl_var;  // declaring identifier -> its variable
    std::unordered_map<const jsparse::Node *, jslsp::Ref *> id_ref;    // referencing identifier -> its reference
    std::vector<jslsp::Hit> hits;                        // sorted by start
    jslsp::Directives directives;
    bool has_jsx = false;
    bool commonjs = false;  // require()/module.exports/exports.x: a node script
    bool uses_with_or_eval = false;
    // The declaration files (js_lsp_dts.h) of the modules this file requires or imports, by
    // specifier; null when there is none. Filled by JsLspAnalyze, and lazily for a specifier
    // typed since (completion on a half-typed line).
    std::string path;
    mutable std::map<std::string, std::shared_ptr<const JsDtsModule>> dts_modules;
    const JsDtsModule *DtsModule(const std::string &specifier) const;

    // -- positions --
    void Pos(uint32_t off, int *line, int *col) const;
    uint32_t Off(int line, int col) const;
    JsLspRange Range(uint32_t a, uint32_t b) const;
    int LineOf(uint32_t off) const;
    std::string Slice(uint32_t a, uint32_t b) const;

    // -- queries --
    const jsparse::Node *Parent(const jsparse::Node *n) const {
        auto it = parent.find(n);
        return it == parent.end() ? nullptr : it->second;
    }
    jslsp::Scope *Global() const { return scopes.empty() ? nullptr : scopes[0].get(); }
    jslsp::Scope *ScopeAt(uint32_t off) const;  // innermost scope containing off
    const jslsp::Hit *HitAt(uint32_t off) const;
    // The `/** ... */` (or `//` run) directly above the statement that holds n, as markdown.
    std::string DocCommentFor(const jsparse::Node *n) const;
};

namespace jslsp {

// js_lsp_scope.cpp
void Analyze(JsLspDocument &doc);
// A JSDoc block's text (between `/**` and `*/`) as markdown: `@param` and friends set out.
std::string JsDocMarkdown(std::string_view body);
bool IsReservedWord(std::string_view s);
bool IsIdentifierName(std::string_view s);
// True when the text at the identifier's range is the name itself (not an escaped `a`).
std::string_view NameOf(const Node *id);

// js_lsp_vocab use, shared by diagnostics and features (js_lsp_features.cpp).
using Api = JsLspApi;
const Api *FindApi(std::string_view name);
// The members of `owner` ("Math", "Array.prototype"), sorted by name.
const std::vector<const Api *> &ApiMembers(std::string_view owner);
// Top-level names (no dot), for undefined-name suggestions and completion.
const std::vector<const Api *> &ApiGlobals();
// Globals of the hosts JavaScript runs in that the vocabulary does not list:
// node's, test runners', bundlers'.
bool IsHostGlobal(std::string_view name);
// What the expression evaluates to, as a vocabulary class name ("Array",
// "String", "HTMLElement", ...) or a class declared in the file, or "".
std::string InferType(const JsLspDocument &doc, const Node *e, int depth = 0);
// The declared type in a TypeScript signature's return/property position, as a class name.
std::string TypeNameOf(std::string_view ts_type);
// The return type of a TypeScript signature's first overload, as a class name.
std::string ReturnTypeOf(std::string_view signature);
// Optimal string alignment distance, capped.
int EditDistance(std::string_view a, std::string_view b, int cap);
// Structural equality of two expressions (same shape, names, literals). Calls, `new`,
// functions and literals that make objects differ unless `tokens` (same text, as ESLint's
// no-duplicate-case compares them).
bool SameNode(const Node *a, const Node *b, bool tokens = false);

}  // namespace jslsp

#endif  // MEP_JS_LSP_INTERNAL_H
