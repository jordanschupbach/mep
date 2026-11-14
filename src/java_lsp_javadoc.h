#ifndef MEP_JAVA_LSP_JAVADOC_H
#define MEP_JAVA_LSP_JAVADOC_H

// Javadoc for mep's Java server (java_lsp.cpp's hover, `mep-java-lsp --doc`)
// and the Java UI mode's Help tab (language_ui_java.h): the declarations of
// a Java source file with the /** */ comment above each, found without a
// compiler -- in the open document, in other files, and on a source path
// (MEP_JAVA_SOURCEPATH, `-sourcepath` in a `CMD:` header, the package root
// of a file). Rosetta's Java binding of datamunge ships its documentation
// this way, as Javadoc in generated .java files.
//
// The scanner knows Java's lexical structure (comments, strings, text
// blocks, char literals) and the shape of declarations in a type body; it
// does not type-check. Local classes inside method bodies are not indexed.

#include <string>
#include <vector>

namespace javadoc {

struct Decl {
    std::string kind;       // class interface enum record annotation method constructor field constant
    std::string name;       // simple name
    std::string owner;      // enclosing types inside the file, dotted: Outer.Inner ("" at top level)
    std::string package;    // the file's package ("" for the default package)
    std::string signature;  // the declaration's header, whitespace collapsed
    std::string doc;        // the Javadoc comment's text, markers stripped ("" when there is none)
    std::string file;
    int line = 0;  // 1-based line of the name

    /** @brief package.Owner.name (dotted, without empty parts). */
    std::string Qualified() const;
};

/** @brief Every declaration of a Java source file, in source order. */
std::vector<Decl> IndexSource(const std::string &text, const std::string &file = "");

/** @brief The package a source file declares ("" for none). */
std::string PackageOf(const std::string &text);

/**
 * @brief A Javadoc comment's text (as Decl::doc holds it) as Markdown: the description with inline tags
 * and HTML turned into Markdown, then parameters, return value, exceptions and the other block tags.
 */
std::string RenderDoc(const std::string &doc);

/** @brief The Markdown Help/hover page of a declaration: its signature in a java block, then RenderDoc. */
std::string RenderDecl(const Decl &d, bool with_location = true);

/**
 * @brief Whether `d` is what NAME means: NAME's dotted parts (`#` counts as a dot, a trailing `(...)`
 * is ignored) are the tail of d's qualified name -- `filter`, `DataFrame.filter`, `DataFrame#filter(int)`,
 * `org.datamunge.DataFrame`.
 */
bool Matches(const Decl &d, const std::string &name);

struct SearchPath {
    std::vector<std::string> files;       // looked at first, in order
    std::vector<std::string> sourcepath;  // directories (searched recursively) and .java files
};

/**
 * @brief The declarations NAME means, from the files first and then the source path: a qualified
 * name is tried at its package's path under each root before the roots are scanned. Duplicates (the
 * same qualified declaration and signature in two copies of a file) count once. Types come first.
 */
std::vector<Decl> Find(const std::string &name, const SearchPath &where, size_t max_results = 20);

/** @brief The root a file's package hangs from (dir of FILE minus the package's path), or "". */
std::string PackageRoot(const std::string &file, const std::string &text);

/** @brief A path list split at ':' (and ';' on Windows), empty entries dropped. */
std::vector<std::string> SplitPathList(const std::string &list);

}  // namespace javadoc

#endif  // MEP_JAVA_LSP_JAVADOC_H
