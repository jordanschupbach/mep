#pragma once

// Internals of mep's own Ruby language server (ruby_lsp.h is the public half).
//
// The server reads Ruby with the vendored tree-sitter grammar
// (grammars/tree-sitter-ruby, compiled in as ts_ruby): every file -- the open
// document and each `.rb` file it requires through the load path -- becomes a
// FileIndex of its definitions (modules, classes, methods, attributes,
// constants) with their YARD doc comments parsed (ruby_lsp_yard.cpp). A World
// merges the indexes a document can see, and answers "what is method `m` of
// class `C`" through superclasses and included modules
// (ruby_lsp_index.cpp). ruby_lsp.cpp holds the features over them.

#include <tree_sitter/api.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ruby_lsp.h"

extern "C" const TSLanguage *tree_sitter_ruby(void);

namespace rblsp {

// --- tree-sitter helpers ------------------------------------------------------------

/** @brief An owned tree-sitter tree over an owned copy of its text. */
struct Tree {
    std::string text;
    TSTree *tree = nullptr;
    Tree() = default;
    Tree(const Tree &) = delete;
    Tree &operator=(const Tree &) = delete;
    ~Tree();
    TSNode Root() const { return ts_tree_root_node(tree); }
};

/** @brief Parses @p text as Ruby. */
std::unique_ptr<Tree> Parse(std::string text);

inline std::string_view NodeType(TSNode n) { return ts_node_is_null(n) ? std::string_view() : ts_node_type(n); }
inline bool Is(TSNode n, std::string_view t) { return !ts_node_is_null(n) && t == ts_node_type(n); }
inline std::string Text(const std::string &src, TSNode n) {
    if (ts_node_is_null(n)) return std::string();
    const uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
    if (b >= src.size() || e <= b) return std::string();
    return src.substr(b, std::min<size_t>(e, src.size()) - b);
}
inline TSNode Field(TSNode n, const char *name) { return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::char_traits<char>::length(name))); }
RubyLspRange RangeOf(TSNode n);

// --- YARD -----------------------------------------------------------------------------

struct YardParam {
    std::string name, types, text;
};
struct YardOption {
    std::string hash, key, types, def, text;
};
struct YardTyped {
    std::string types, text;
};
struct YardExample {
    std::string title, code;
};
struct YardDoc;
struct YardOverload {
    std::string signature;  // `filter(column, value = nil)`
    std::shared_ptr<YardDoc> doc;
};
struct YardDoc {
    std::string text;  // the free-text description (Markdown, as written)
    std::vector<YardParam> params;
    std::vector<YardOption> options;
    std::vector<YardTyped> returns, raises;
    std::vector<YardParam> yieldparams;
    std::vector<YardTyped> yieldreturn;
    std::string yield;  // `@yield [a, b] text`, as "[a, b] text"
    std::vector<YardExample> examples;
    std::vector<YardOverload> overloads;
    std::vector<std::string> notes, see, todo;
    std::string deprecated, since, api, author, version;
    bool deprecated_tag = false, private_tag = false, abstract_tag = false;
    bool Empty() const;
    /** @brief The first sentence of the description (for completion detail). */
    std::string Summary() const;
};

/** @brief Parses a doc comment's lines (each without its leading `#` and one space). */
std::shared_ptr<YardDoc> ParseYard(const std::vector<std::string> &lines);
/** @brief The doc as Markdown: description, then Parameters/Options/Returns/Raises/Yields/Examples. */
std::string YardMarkdown(const YardDoc &doc, const std::string &highlight_param = std::string());
/** @brief The first type of a YARD type list (`Array<Float>, nil` -> `Array<Float>`); "" for none/void. */
std::string FirstType(const std::string &types);

// --- definitions -------------------------------------------------------------------------

enum class ParamKind { Req, Opt, Rest, KeyReq, Key, KeyRest, Block, Forward, Destructured };

struct Param {
    std::string name;
    ParamKind kind = ParamKind::Req;
    std::string def;  // the default's source text
};
std::string ParamLabel(const Param &p);
/** @brief `(a, b = 1, *rest, k:, &blk)` -> params (also accepts no parentheses). */
std::vector<Param> ParseParamList(const std::string &text);

enum class DefKind { Module, Class, Method, Constant, Attr, Alias };

struct Def {
    DefKind kind = DefKind::Method;
    std::string name;
    std::string owner;  // fully qualified namespace ("" = top level / Object)
    std::string fqn;    // modules/classes: the full name
    bool singleton = false;  // a class method (`def self.x`, `class << self`, module_function)
    std::vector<Param> params;
    std::string superclass;  // classes: as written
    std::vector<std::string> nesting;  // lexical nesting where it was written (outermost first)
    std::vector<std::string> includes, extends;  // modules: as written
    std::string value;        // constants: the value's text (shortened)
    std::string attr;         // attributes: "r", "w" or "rw"
    std::string alias_of;     // aliases: the original name
    std::string visibility;   // "", "private", "protected"
    std::string return_type;  // builtins: the return type ("$self", "$elem" are placeholders)
    std::shared_ptr<YardDoc> doc;
    std::string path;  // "" = the open document; builtins have path "<builtin>"
    RubyLspRange range, sel;
    bool builtin = false;
    /** @brief `name(a, b = 1)` (methods) or the name. */
    std::string Signature() const;
};

struct Require {
    std::string name;
    bool relative = false;
    bool conditional = false;  // under an `if`/`rescue`/method/...: may never run
    RubyLspRange range;  // the string literal
};

/** @brief Definitions and requires of one file. */
struct FileIndex {
    std::string path;
    std::vector<std::unique_ptr<Def>> defs;
    std::vector<Require> reqs;
    std::vector<std::string> load_path;  // directories it adds ($LOAD_PATH.unshift, `-I` in a CMD: header)
};

/** @brief Indexes a parsed file. */
std::shared_ptr<FileIndex> IndexTree(const Tree &t, const std::string &path);
/** @brief The index of a file on disk, cached by modification time (nullptr when unreadable). */
std::shared_ptr<const FileIndex> IndexFile(const std::string &path);

// --- load path ----------------------------------------------------------------------------

/** @brief Where `require name` (or `require_relative`) from @p from_file resolves; "" when not found
 *  or not a `.rb` file. @p extra are directories from the document (CMD: -I, $LOAD_PATH). */
std::string ResolveRequire(const Require &r, const std::string &from_file, const std::vector<std::string> &extra);
/** @brief Load-path directories for a document at @p path (RUBYLIB, the workspace's lib dirs, the
 *  file's ancestors' lib dirs, then @p extra first). */
std::vector<std::string> LoadPath(const std::string &path, const std::vector<std::string> &extra);
void SetWorkspaceRoot(const std::string &root);
void SetLoadPathOverride(const std::vector<std::string> *dirs);

// --- builtins ----------------------------------------------------------------------------

/** @brief The core classes/methods mep knows without Ruby (ruby_lsp_vocab.cpp). */
std::shared_ptr<const FileIndex> Builtins();
/** @brief Ruby's keywords. */
const std::vector<std::string> &Keywords();

// --- the world a document sees -------------------------------------------------------------

struct Type {
    std::string name;  // fully qualified class name; "" = unknown
    bool meta = false; // the class object itself (`Foo` rather than `Foo.new`)
    std::string elem;  // `Array<Frame>` -> "Frame"
    bool Known() const { return !name.empty(); }
};

class World {
public:
    void Add(std::shared_ptr<const FileIndex> f);
    bool Has(const std::string &path) const { return paths_.count(path) > 0; }
    const std::vector<std::shared_ptr<const FileIndex>> &Files() const { return files_; }

    /** @brief The namespace definitions (each reopening) of @p fqn. */
    std::vector<const Def *> Namespace(const std::string &fqn) const;
    bool IsNamespace(const std::string &fqn) const { return ns_.count(fqn) > 0; }
    /** @brief Resolves constant path @p name as written inside @p nesting (outermost first): the
     *  fully qualified name of the namespace or constant, or "" when unknown. */
    std::string ResolveConst(const std::string &name, const std::vector<std::string> &nesting) const;
    /** @brief Constant definitions (not namespaces) named @p fqn. */
    std::vector<const Def *> Constants(const std::string &fqn) const;
    /** @brief Methods named @p name of class @p fqn ("" = Object), through its ancestors. */
    std::vector<const Def *> Methods(const std::string &fqn, bool singleton, const std::string &name) const;
    /** @brief Every method of @p fqn (first definition per name, nearest ancestor first). */
    std::vector<const Def *> AllMethods(const std::string &fqn, bool singleton) const;
    /** @brief The ancestors of @p fqn (itself first): included modules, then the superclass chain. */
    std::vector<std::string> Ancestors(const std::string &fqn) const;
    /** @brief Constants and namespaces directly inside @p fqn ("" = top level). */
    std::vector<const Def *> Members(const std::string &fqn) const;
    /** @brief Top-level `def`s (private methods of Object, callable anywhere). */
    std::vector<const Def *> TopLevelMethods() const;

private:
    std::vector<std::shared_ptr<const FileIndex>> files_;
    std::set<std::string> paths_;
    std::map<std::string, std::vector<const Def *>> ns_;      // fqn -> module/class defs
    std::map<std::string, std::vector<const Def *>> consts_;  // fqn -> constant defs
    // owner -> name -> defs (instance and singleton)
    std::map<std::string, std::map<std::string, std::vector<const Def *>>> methods_, smethods_;
    std::map<std::string, std::vector<const Def *>> members_;  // owner -> constants/namespaces
    std::vector<const Def *> top_;  // top-level `def`s of the files (not builtins)
    void Collect(const std::string &fqn, bool singleton, std::set<std::string> &seen,
                 std::vector<std::string> &order) const;
};

}  // namespace rblsp
