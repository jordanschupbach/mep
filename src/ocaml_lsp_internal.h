#ifndef MEP_OCAML_LSP_INTERNAL_H
#define MEP_OCAML_LSP_INTERNAL_H

// Shared internals of mep's OCaml language server (ocaml_lsp*.cpp):
//
//   ocaml_lsp_index.cpp  modules: indexing a parsed .ml/.mli into items with their
//                        odoc comments, the load path, module-name resolution
//   ocaml_lsp_odoc.cpp   odoc comment markup -> Markdown
//   ocaml_lsp_vocab.cpp  keywords and the compiled-in stdlib fallback
//   ocaml_lsp.cpp        the document analysis and the features of ocaml_lsp.h
//
// OCaml is read with the vendored tree-sitter grammars (grammars/tree-sitter-ocaml:
// `ocaml` for .ml, `ocaml_interface` for .mli).

#include <tree_sitter/api.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ocaml_lsp.h"

extern "C" const TSLanguage *tree_sitter_ocaml(void);
extern "C" const TSLanguage *tree_sitter_ocaml_interface(void);

namespace mllsp {

// --- tree-sitter helpers -------------------------------------------------------------

/** @brief An owned parse tree with its source. */
struct Tree {
    std::string src;
    bool interface = false;
    TSTree *tree = nullptr;
    Tree() = default;
    Tree(const Tree &) = delete;
    Tree &operator=(const Tree &) = delete;
    ~Tree();
    TSNode Root() const { return ts_tree_root_node(tree); }
};

std::unique_ptr<Tree> Parse(std::string src, bool interface);

std::string_view Text(TSNode n, const std::string &src);
std::string Type(TSNode n);  // ts_node_type as a string
OcamlLspRange RangeOf(TSNode n);
TSNode Field(TSNode n, const char *name);
/** @brief The first named child of type @p type (null node if none). */
TSNode ChildOfType(TSNode n, const char *type);
/** @brief Whether @p n has an anonymous child token spelled @p tok. */
bool HasToken(TSNode n, const char *tok);
/** @brief Whitespace runs collapsed to one space. */
std::string Collapse(std::string_view s);

// --- modules and their items -------------------------------------------------------

enum class ItemKind {
    Value,       // let / val
    External,
    Type,
    Constructor,
    Field,
    Exception,
    Module,
    ModuleType,
    Class,
    Method,
};

struct Module;

struct Item {
    ItemKind kind = ItemKind::Value;
    std::string name;
    std::string signature;    // display form: `val map : ...`, `type 'a t = ...`, `module M : sig ... end`
    std::string type;         // a value's type (`int -> int`), "" when unknown
    std::vector<std::string> param_names;  // an implementation's `let f x ~y` parameters (no type known)
    std::string doc;          // raw odoc text (inside the (** *))
    std::string deprecated;   // [@@ocaml.deprecated "msg"] / @deprecated text
    bool is_deprecated = false;
    std::string owner_type;   // a constructor's / field's type
    OcamlLspRange range, sel;
    std::string alias;        // `module M = Foo.Bar` / `module M : module type of Foo` -> "Foo.Bar"
    std::string module_type;  // `module M : S` / `module type T = S` -> "S" (a module type path)
    std::shared_ptr<Module> members;  // a module's / module type's / class's own items
};

struct Module {
    std::string name;   // `List`; "" for anonymous structures
    std::string path;   // the file, "" for in-memory
    bool interface = false;
    std::string doc;    // the preamble (the file's first, floating, doc comment)
    std::vector<Item> items;
    std::vector<std::string> includes;  // module paths: `include Foo`, `include module type of Foo`
    std::vector<std::string> include_types;  // module type paths: `include S`
    bool opaque = false;  // a functor application or similar: members are not all known
};

/** @brief Indexes the items of a structure / signature / compilation unit node. */
std::shared_ptr<Module> IndexNode(TSNode container, const std::string &src, bool interface);
/** @brief Sets the file of a module and of all its nested modules. */
void SetPaths(Module *mod, const std::string &path);
/** @brief Parses and indexes source text. */
std::shared_ptr<Module> IndexSource(const std::string &src, bool interface, const std::string &path,
                                    const std::string &name);
/** @brief The doc comment text of a comment node, or "" when it is not a `(** *)` doc comment. */
std::string DocCommentText(std::string_view comment);

// --- the load path -----------------------------------------------------------------

/** @brief Where a document looks for modules. */
struct Env {
    std::string file;  // the document, "" for none
    std::string dir;   // its directory (or the current one)
    std::vector<std::string> include_dirs;  // `-I DIR` from a `CMD:` header
};

struct ModuleFile {
    std::string mli, ml;  // either may be empty
};

/** @brief The files of the compilation unit @p name (`List`, `Stdlib__List`, `Foo`). */
ModuleFile FindModuleFile(const std::string &name, const Env &env);
/** @brief The indexed module @p name, preferring the .mli; nullptr when not found. */
std::shared_ptr<const Module> LoadModule(const std::string &name, const Env &env);
/** @brief A file, indexed (cached by path and modification time). */
std::shared_ptr<const Module> LoadFile(const std::string &path);
/** @brief The module names a document can see (workspace, stdlib, libraries), sorted. */
std::vector<std::string> KnownModules(const Env &env);
/** @brief Stdlib (the implicitly opened module): the installed stdlib.mli, else the fallback. */
std::shared_ptr<const Module> StdlibModule();
/** @brief The installed stdlib directory ("" when OCaml is not found). */
std::string StdlibDir();
/** @brief The library directories searched (MEP_OCAML_PATH, OCAMLPATH, opam, ocamlfind). */
std::vector<std::string> LibraryDirs(const Env &env);
/** @brief The workspace root (the client's rootUri). */
void SetWorkspaceRoot(const std::string &root);
/** @brief Tests: forget every cached directory listing and module. */
void ResetCaches();

/** @brief Resolution context: the document's own module plus the load path. */
struct Scope {
    const Env *env = nullptr;
    std::shared_ptr<const Module> local;  // the document (or nullptr)
};

/** @brief The module a dotted path names (`List`, `Foo.Bar`), seen from @p scope; nullptr if unknown. */
std::shared_ptr<const Module> ResolveModulePath(const std::vector<std::string> &path, const Scope &scope,
                                                int depth = 0);
/** @brief The members of a module item (its own, its alias's, or its module type's). */
std::shared_ptr<const Module> ModuleOfItem(const Item &item, const Module *owner, const Scope &scope, int depth = 0);

struct Found {
    const Item *item = nullptr;
    std::shared_ptr<const Module> module;  // keeps item alive; the module it was found in
};

/** @brief @p name among @p mod's items (latest first, then its includes); kinds filter when non-empty. */
Found FindItem(const std::shared_ptr<const Module> &mod, const std::string &name,
               const std::set<ItemKind> &kinds, const Scope &scope, int depth = 0);
/** @brief Every item of @p mod, includes followed (shadowed names dropped). */
std::vector<Found> AllItems(const std::shared_ptr<const Module> &mod, const Scope &scope, int depth = 0);

std::vector<std::string> SplitPath(std::string_view dotted);

// --- docs -----------------------------------------------------------------------

/** @brief odoc markup to Markdown (tags become Parameters / Returns / Raises ... sections). */
std::string OdocToMarkdown(const std::string &raw);
/** @brief The first paragraph of @p raw, as plain-ish Markdown (completion details). */
std::string OdocSummary(const std::string &raw);
/** @brief Hover-style Markdown for an item: the signature in a code block, then the docs. */
std::string ItemMarkdown(const Item &item, const std::string &qualifier);

// --- vocabulary --------------------------------------------------------------------

const std::vector<std::string> &Keywords();
bool IsKeyword(std::string_view w);
/** @brief Compiled-in .mli text for Stdlib and a few modules, used when OCaml is not installed. */
const std::vector<std::pair<std::string, std::string>> &FallbackModules();  // name -> .mli source

// --- types -----------------------------------------------------------------------------

struct ArrowParam {
    std::string text;   // `?sep:string`, `'a list`
    std::string label;  // `sep` for labelled / optional, "" for positional
    bool optional = false;
};
/** @brief A type's top-level arrows: parameters and the result. */
std::vector<ArrowParam> SplitArrows(const std::string &type, std::string *result);

}  // namespace mllsp

#endif  // MEP_OCAML_LSP_INTERNAL_H
