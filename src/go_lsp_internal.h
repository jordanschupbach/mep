#ifndef MEP_GO_LSP_INTERNAL_H
#define MEP_GO_LSP_INTERNAL_H

// What go_lsp.cpp (parse, scopes, diagnostics) and go_lsp_features.cpp
// (completion, hover, navigation, outline) share: the analysed document.
//
// The model is one file seen alone. Names resolve through Go's blocks --
// function and block scopes, the file block (imports), the package block
// (this file's top-level declarations) and the universe -- so anything a
// file can know about itself is exact: locals, labels, parameters, imports.
// A name declared in another file of the same package is simply not found,
// which is why nothing here ever reports an undefined plain name.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "go_lsp.h"
#include "go_parse.h"

namespace golsp {

enum class SymKind { Var, Const, Type, Func, Method, Field, Param, Result, Import, Label, TypeParam, TypeSwitch };

struct Sym {
    std::string name;
    SymKind kind = SymKind::Var;
    goast::Pos pos = goast::NoPos;    // the declaring identifier
    goast::Node *decl = nullptr;      // FuncDecl, TypeSpec, ValueSpec, ImportSpec, Field, AssignStmt, ...
    goast::Node *type = nullptr;      // declared type expression, when written
    goast::Node *value = nullptr;     // initialiser (the whole rhs expression), when there is one
    int value_index = -1;             // which result of a multi-value rhs (`a, b := f()`), -1 when 1:1
    bool range_key = false, range_value = false;  // declared by `for k, v := range value`
    int scope = -1;
    bool local = false;               // declared inside a function
    bool used = false;
    std::string import_path;          // Import: the path; Method/Field: the owning type's name
    goast::CommentGroup *doc = nullptr;
};

struct Ref {
    goast::Pos pos = goast::NoPos;
    int len = 0;
    int sym = -1;           // -1: not resolved in this file
    bool write = false;     // a plain `x = ...` / redeclaring `:=`
    bool decl = false;      // the declaring occurrence
    goast::Ident *ident = nullptr;
    goast::Node *selector_base = nullptr;  // for X.Sel's Sel: X
    int scope = -1;
};

struct Scope {
    int parent = -1;
    goast::Pos start = goast::NoPos, end = goast::NoPos;  // the region the scope covers
    std::map<std::string, int> names;
    bool function = false;  // a function's outermost scope
};

struct ImportInfo {
    goast::ImportSpec *spec = nullptr;
    std::string path;
    std::string name;      // the name it is used by (explicit, the package's own, or a guess)
    bool certain = false;  // `name` is known (explicit, or a standard-library package)
    int sym = -1;
};

}  // namespace golsp

class GoLspDocument {
public:
    std::vector<std::string> lines;
    std::string text;
    goparse::Result parse;
    std::vector<golsp::Sym> syms;
    std::vector<golsp::Ref> refs;  // sorted by pos
    std::vector<golsp::Scope> scopes;
    std::vector<golsp::ImportInfo> imports;
    std::vector<GoLspDiagnostic> semantic;  // checks beyond syntax (only when the file parses cleanly)
    int package_scope = 0, file_scope = 1;
    std::string path;
    bool host_build = true;  // the file builds for linux/amd64 (its //go:build line and name allow it)

    GoLspRange RangeOf(goast::Pos a, goast::Pos b) const;
    GoLspRange RangeOfNode(const goast::Node *n) const { return RangeOf(goast::NodePos(n), goast::NodeEnd(n)); }
    /** @brief The Pos of 0-based (line, byte col), clamped into the text. */
    goast::Pos PosAt(int line, int col) const;
    std::string Slice(goast::Pos a, goast::Pos b) const;
    std::string SliceNode(const goast::Node *n) const { return Slice(goast::NodePos(n), goast::NodeEnd(n)); }
    /** @brief The reference whose identifier covers `p` (or ends exactly at it), or nullptr. */
    const golsp::Ref *RefAt(goast::Pos p) const;
    /** @brief The innermost scope containing `p`. */
    int ScopeAt(goast::Pos p) const;
    /** @brief Resolves `name` as seen from position `p` (locals declared after `p` do not count). */
    int Lookup(const std::string &name, goast::Pos p) const;
};

namespace golsp {

/** @brief The standard-library package's own name (`package rand` for math/rand/v2), or "" if not in the table. */
std::string StdPackageName(const std::string &path);
/** @brief The import name a path is used by when not renamed: the package's own for the standard library, else a guess. */
std::string GuessImportName(const std::string &path, bool *certain);
/** @brief Whether `name` is predeclared (universe block). */
bool IsUniverse(const std::string &name);
/** @brief A declaration rendered for hover: the source with bodies dropped, at most `max_lines` lines. */
std::string DeclText(const GoLspDocument &doc, const Sym &s);

/** @brief A type, written in the file or in the standard-library table's signatures (`pkg` then says whose names it uses). */
struct TypeRef {
    goast::Node *expr = nullptr;
    std::string pkg;  // "" = this file; else the import path the expression's names belong to
    bool ok() const { return expr != nullptr; }
};

/** @brief Best-effort static type of an expression (no full type checking). */
TypeRef TypeOf(const GoLspDocument &doc, goast::Node *e, goast::Pos at, int depth = 0);

struct Member {
    std::string name;
    std::string detail;  // signature or field type
    std::string doc;
    bool method = false;
    goast::Pos pos = goast::NoPos;  // in this file, when declared here
    TypeRef type;                   // field type / method result (for chains)
    bool deprecated = false;
};

/** @brief Fields and methods of a type (local struct/interface types and standard-library types). */
std::vector<Member> MembersOf(const GoLspDocument &doc, const TypeRef &t);

}  // namespace golsp

#endif
