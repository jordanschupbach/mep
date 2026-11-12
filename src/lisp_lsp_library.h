#ifndef MEP_LISP_LSP_LIBRARY_H
#define MEP_LISP_LSP_LIBRARY_H

// Project libraries for mep-lisp-lsp: what the ASDF systems next to a
// document define, read from their source rather than from a running
// image, so `(dm:multiply a b)` can show the documentation of a system
// the file loads (the Common Lisp bindings Rosetta generates for a C++
// library, typically) without starting a Lisp.
//
// Where the systems come from:
//   - the `*.asd` files of the document's project: the nearest directory
//     above the document that holds `.git` is the project root, searched
//     6 directories deep (skipping hidden directories, `build*`,
//     `node_modules`, `target`, `result*`); without a `.git` the
//     document's own directory (3 deep) and the `.asd` files directly in
//     the directories above it;
//   - `$MEP_LISP_LIBRARY` (':'-separated): directories searched the same
//     way, or `.asd` / `.lisp` files named directly;
//   - `$CL_SOURCE_REGISTRY` (':'-separated, `dir//` recursive), consulted
//     only for systems the document names (`asdf:load-system`,
//     `ql:quickload`, `require`) or packages it uses that the project
//     did not provide;
//   - last, still only for what is missing, the project root's `build*`
//     directories (8 deep), where CMake writes generated bindings.
// An `.asd` is read with mep's own reader: each `defsystem`'s name and
// its `:components` (`:file`, nested `:module`, `:pathname`) resolve to
// the `.lisp` files that are indexed. Those are read once per change
// (path, size and modification time) and only the index is kept.
//
// Nothing is read at all unless the document refers to a package other
// than COMMON-LISP / KEYWORD (a `pkg:` prefix, a `:use`, a
// `:local-nicknames` entry) or names a system it loads.

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

enum class LispLibKind {
    Function,
    Macro,
    Generic,
    Method,
    Class,
    Condition,
    Struct,
    Type,
    Constant,
    Variable,
    Accessor,  // a slot :reader/:accessor
};

struct LispLibDef {
    LispLibKind kind = LispLibKind::Function;
    std::string name;     // canonical symbol name ("AREA"); a setf function is "(SETF X)"
    std::string package;  // the package as written (canonical; may be a nickname), resolved by LispLibrary
    std::string lambda_list;  // as written, outer parentheses excluded
    std::string doc;
    std::string specializer;          // Method: canonical class of its first specialized required parameter
    std::vector<std::string> supers;  // Class/Condition: canonical direct superclass names
    std::string value;                // Constant/Variable: the value; Type: the expansion (short source text)
    std::string returns;              // Function: the class after `→` in its first signature line, canonical
    int line = 0;                     // the name's position in `path`
    int col = 0;
    int end_col = 0;
};

struct LispLibPackage {
    std::string name;  // canonical
    std::vector<std::string> nicknames;
    std::vector<std::string> uses;
    std::set<std::string> exports;
    std::string doc;
    int line = 0;
};

struct LispLibFile {
    std::string path;
    std::string system;  // the ASDF system it was found through ("" when named directly)
    std::vector<LispLibPackage> packages;
    std::vector<LispLibDef> defs;
};

struct LispLibSystem {
    std::string name;  // lower case, as ASDF compares them
    std::string asd;
    std::vector<std::string> files;  // absolute .lisp paths, in component order
    std::vector<std::string> depends_on;
};

/** @brief Indexes one Lisp source text (exposed for tests and benchmarks). */
LispLibFile LispLibParseSource(const std::vector<std::string> &lines, const std::string &path);

/** @brief Reads the `defsystem` forms of an `.asd` text; component paths resolve against `asd_path`'s directory. */
std::vector<LispLibSystem> LispLibParseAsd(const std::vector<std::string> &lines, const std::string &asd_path);

/** @brief The signature lines of a docstring for `name`: each `(name ...)` line's text inside the parens
 *         after the name, e.g. "a &optional (b 10)" for `  (add a &optional (b 10)) → integer`. */
std::vector<std::string> LispLibDocSignatures(const std::string &doc, const std::string &lower_name);

/** @brief Renders a Rosetta-style docstring as markdown: indented runs become code blocks, ``` fences pass. */
std::string LispLibDocMarkdown(const std::string &doc);

/** @brief What a document refers to: package names (canonical) and system names (lower case). */
struct LispLibRefs {
    std::set<std::string> packages;
    std::set<std::string> systems;
};

/** @brief The merged index a document sees. */
class LispLibrary {
public:
    std::vector<std::shared_ptr<const LispLibFile>> files;

    /** @brief A package by canonical name or global nickname, or null. */
    const LispLibPackage *Package(const std::string &name) const;
    /** @brief The file defining a package (by its primary name), or null. */
    const LispLibFile *FileOf(const LispLibPackage *p) const;
    /** @brief The definitions of `symbol` (canonical) in the package with primary name `pkg`, source order. */
    const std::vector<const LispLibDef *> &Defs(const std::string &pkg, const std::string &symbol) const;
    /** @brief The symbol names (canonical) with definitions in a package, sorted. */
    std::vector<std::string> Symbols(const std::string &pkg) const;
    /** @brief The file a definition comes from. */
    const LispLibFile *FileOfDef(const LispLibDef *d) const;

    /** @brief Builds the maps after `files` is filled. */
    void Build();

private:
    std::map<std::string, const LispLibPackage *> packages_;
    std::map<const LispLibPackage *, const LispLibFile *> package_files_;
    std::map<const LispLibDef *, const LispLibFile *> def_files_;
    std::map<std::string, std::map<std::string, std::vector<const LispLibDef *>>> defs_;
};

/** @brief The `.asd` files a document's project offers (see the header comment), cached briefly. */
std::vector<std::string> LispLibAsdFiles(const std::string &doc_path);

/**
 * @brief The library a document sees, or null when it refers to nothing indexable.
 * @param doc_path the document's filesystem path ("" when unknown: only $MEP_LISP_LIBRARY is searched)
 * @param refs what the document refers to
 */
std::shared_ptr<const LispLibrary> LispLibraryFor(const std::string &doc_path, const LispLibRefs &refs);

#endif  // MEP_LISP_LSP_LIBRARY_H
