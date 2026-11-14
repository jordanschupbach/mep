#pragma once

// Shared between julia_lsp.cpp (the syntax view, scope analysis and
// diagnostics) and julia_lsp_features.cpp (hover, completion, navigation,
// outline, folds): the document as the analysis sees it.
//
// The syntax view is JuliaSyntax's SyntaxNode: the green tree of mep's
// JuliaSyntax port (julia_parse.h) without trivia, each node a kind, flags
// and a byte range, a leaf's text being the source it covers. Shapes are
// JuliaSyntax 0.4's (`(= (call f x) body)` for a short-form method,
// `(. a (quote b))` for a field, `(for (= i r) (block ...))`).

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "julia_lsp.h"
#include "julia_lsp_packages.h"
#include "julia_lsp_vocab.h"
#include "julia_parse.h"

namespace jllsp {

// The node kinds the analysis distinguishes; everything else is Other.
enum class K : uint8_t {
    Other, Ident, MacroName, StrMacroName, Var, Call, DotCall, MacroCall, Function, Macro, Struct, Abstract,
    Primitive, Module, Let, For, While, If, Elseif, Else, Try, Catch, Finally, Block, Assign, OpAssign, DotAssign,
    Local, Global, Const, Return, Break, Continue, Tuple, Parameters, Arrow, Do, Quote, Interp, Dot, DColon, Where,
    Curly, Braces, Comprehension, Generator, Filter, CartIter, Using, Import, Export, Public, ImportPath, Colon, As,
    Splat, Ref, String, StrLit, Parens, Error, Toplevel, Doc, Subtype, Supertype, Outer, Inert, False, True, Ternary,
    AndAnd, OrOr, Vect, Integer, Float, Char, Nothing,
};

struct Node {
    K k = K::Other;
    jlparse::Kind kind{};
    uint16_t flags = 0;
    uint32_t b = 0, e = 0;  // byte range, 0-based, end exclusive
    uint32_t kid0 = 0, nkid = 0;  // into Doc::kids
    int parent = -1;
    bool leaf = false;
};

enum class ScopeKind : uint8_t { Module, Function, Soft, Let, Struct, Macro };

struct Scope {
    ScopeKind kind = ScopeKind::Module;
    int parent = -1;
    int module = 0;        // the enclosing Module scope (itself for a module)
    int node = -1;         // the construct that opened it
    uint32_t b = 0, e = 0;
    std::map<std::string, int, std::less<>> names;  // name -> var
    // Module scopes: what makes its globals unknowable.
    bool opaque = false;           // include(), @eval, eval(), an unknown `using`
    bool bare = false;             // baremodule: no implicit Base
    std::vector<std::string> usings;  // `using X` of a module the vocabulary knows
    std::vector<std::string> unknown_usings;
    bool has_eval = false;         // eval/@eval/@locals in this function: no unused checks
};

enum class VarKind : uint8_t {
    Global, Local, Argument, KwArgument, TypeParam, Function, Macro, Struct, AbstractType, PrimitiveType, ModuleName,
    Const, Import, LoopVar, CatchVar, LetVar,
};

struct Var {
    std::string name;
    VarKind kind = VarKind::Local;
    int scope = -1;
    uint32_t b = 0, e = 0;  // the defining occurrence
    int def_node = -1;      // the definition (function/struct/assignment) for hover
    int reads = 0, writes = 0;
    bool from_macro = false;   // first seen inside a macro call's arguments
    bool destructured = false; // one part of `a, b = ...`
    bool underscore = false;
    bool splat = false;        // a `args...` argument
    std::string type;          // `x::T` -- T's source, for hover
    std::vector<int> defs;     // every definition node (methods)
};

struct Occ {
    uint32_t b = 0, e = 0;
    int var = -1;  // -1: unresolved
    bool def = false, write = false;
    int scope = -1;
    std::string name;
};

struct Doc {
    std::vector<std::string> lines;
    std::string text;
    std::vector<uint32_t> line_starts;
    jlparse::ParseResult parse;
    std::vector<Node> nodes;
    std::vector<uint32_t> kids;
    int root = -1;
    std::vector<Scope> scopes;
    std::vector<Var> vars;
    std::vector<Occ> occs;
    std::vector<JuliaLspDiagnostic> diags;
    bool syntax_errors = false;
    std::string path;  // the document's file, "" for none
    std::vector<std::shared_ptr<const Package>> packages;  // what its `using`s found

    std::string_view Text(const Node &n) const { return std::string_view(text).substr(n.b, n.e - n.b); }
    std::string_view Text(int i) const { return Text(nodes[static_cast<size_t>(i)]); }
    const Node &N(int i) const { return nodes[static_cast<size_t>(i)]; }
    int Kid(int i, uint32_t k) const { return static_cast<int>(kids[N(i).kid0 + k]); }
    uint32_t NKids(int i) const { return N(i).nkid; }
    JuliaLspRange Range(uint32_t b, uint32_t e) const;
    void Pos(uint32_t byte, int *line, int *col) const;
    uint32_t Offset(int line, int col) const;
};

/** @brief An identifier node's name (`var"x y"` -> `x y`). */
std::string IdentName(const Doc &d, int node);
/** @brief The innermost scope containing byte @p at. */
int ScopeAt(const Doc &d, uint32_t at);
/** @brief The innermost named node covering @p at (leaves preferred). */
int NodeAt(const Doc &d, uint32_t at);
/** @brief Resolves @p name from scope @p scope as the analysis does; -1 when not in the file. */
int Lookup(const Doc &d, int scope, std::string_view name);
/** @brief The vocabulary rows for a name visible in module scope @p module (Base/Core, `using`ed stdlibs). */
std::vector<const JuliaLspApi *> VocabFor(const Doc &d, int module, std::string_view name);
/** @brief Every vocabulary row named @p name, any module. */
std::vector<const JuliaLspApi *> VocabAll(std::string_view name);
/** @brief Every row named @p name: the document's packages' (exported or not) first, then the vocabulary's. */
std::vector<const JuliaLspApi *> VocabAll(const Doc &d, std::string_view name);
/** @brief The package `using`ed by the document under @p name, or nullptr. */
const Package *PackageNamed(const Doc &d, std::string_view name);
/** @brief Whether package lookup is running (packages' own `using`s are not followed). */
bool BuildingPackage();
/** @brief The docstring attached to definition node @p def (a `doc` parent or `@doc`), or "". */
std::string DocstringFor(const Doc &d, int def);
/** @brief A definition's signature source, one line (`f(x::Int, y=1)`). */
std::string SignatureText(const Doc &d, int def);

}  // namespace jllsp

class JuliaLspDocument {
public:
    jllsp::Doc d;
};
