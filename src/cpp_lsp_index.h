#ifndef MEP_CPP_LSP_INDEX_H
#define MEP_CPP_LSP_INDEX_H

#include <string>
#include <vector>

#include "cpp_ast.h"
#include "cpp_lsp.h"

// The scope model behind mep's C++ language server: one pass over the
// syntax tree that produces every declaration in the file, the scopes
// they live in, and every place a name is used. Both halves of the
// analysis read it -- cpp_lsp.cpp for diagnostics, cpp_lsp_features.cpp
// for completion, hover, symbols, folding, definition, references and
// signature help -- which is why it is a header of its own and not a
// detail of either.
//
// What it models, and what it deliberately does not:
//   - Namespaces (re-opened ones merge), classes with their base
//     classes, functions with their parameters, and every nested block.
//     Using-directives and using-declarations are recorded per scope, so
//     lookup honours them.
//   - Types are tracked as *written*, not deduced: a variable knows the
//     spelling of its declared type and the bare name inside it, which
//     is enough to offer members of a class defined in this file. `auto`
//     is followed only where the initializer says so outright
//     (`auto p = Point(...)`).
//   - Overloads are kept as separate symbols under the same name; the
//     argument-count check is the only thing that cares, and it steps
//     aside as soon as a name has more than one.
//   - Nothing here reads another file. A base class, a member or a type
//     from a header this server never opened resolves to nothing, and
//     every feature treats "not found" as "say nothing" rather than as
//     "report a problem".

enum class CppSymbolKind {
    Namespace,
    Class,
    Struct,
    Union,
    Enum,
    Enumerator,
    Function,
    Method,
    Constructor,
    Destructor,
    Variable,
    Field,
    Parameter,
    Typedef,
    Macro,
    Concept,
    TemplateParam,
    Label,
};

struct CppSymbol {
    std::string name;
    std::string qualified;  // "mep::Editor::DrawPane"
    CppSymbolKind kind = CppSymbolKind::Variable;
    const CppNode *node = nullptr;  // null for a macro, which has no tree node
    CppPos pos;                     // the name's own span
    CppPos end;
    CppPos range_start;  // the whole declaration, for folding and symbol ranges
    CppPos range_end;
    std::string type_text;  // the declared type, as written
    std::string type_base;  // the bare name inside it ("std::vector", "Point")
    std::string detail;     // what a completion list or hover shows
    std::string doc;        // the comment block above the declaration
    std::string access;     // "public"/"protected"/"private" for a member
    int ptr_depth = 0;
    bool is_ref = false;
    bool is_static = false;
    bool is_const = false;
    bool is_constexpr = false;
    bool is_definition = false;
    bool is_template = false;
    bool is_virtual = false;
    bool is_pure = false;
    bool is_extern = false;
    bool is_deleted = false;
    bool is_defaulted = false;
    bool is_function_like_macro = false;
    bool from_macro = false;  // the declaration came out of a macro expansion
    // Set for a declaration read out of an `#include`d header rather than
    // this document: `file` is that header's absolute path, and `pos` is
    // a position in it. Nothing reports diagnostics about these -- they
    // exist so a name from a project's own header resolves, and so go to
    // definition can jump there.
    bool external = false;
    std::string file;
    int scope = 0;            // the scope this symbol was declared in
    int child_scope = -1;     // the scope it opens (namespace, class, function)
    int reads = 0;            // how many times its value was read
    int writes = 0;           // assignments after the declaration
    bool address_taken = false;
};

struct CppScope {
    enum class Kind { File, Namespace, Class, Function, Block, Enum };
    Kind kind = Kind::File;
    int parent = -1;
    std::string name;
    std::string qualified;
    std::vector<int> symbols;
    std::vector<int> children;
    // `using namespace X;` seen in this scope, by spelled name.
    std::vector<std::string> using_directives;
    // Scopes whose names are visible here without qualification: an
    // unnamed namespace (which is implicitly used by its enclosing
    // scope) and an `inline namespace`. Without these, every helper in
    // an anonymous namespace looks undeclared to the file that defines
    // it -- which is most of what an anonymous namespace is for.
    std::vector<int> transparent_children;
    // Base classes that resolved to a class in this file, as scope
    // indices, and the ones that did not, by name. The second list is
    // what tells the member checks to stay quiet.
    std::vector<int> base_scopes;
    std::vector<std::string> unresolved_bases;
    CppPos start;
    CppPos end;
    const CppNode *node = nullptr;
    // The symbol this scope belongs to (a class, function or namespace),
    // or -1 for the file scope and plain blocks.
    int owner = -1;
    // Where this scope was *written*, when that differs from where it
    // belongs: the body of `void mep::Editor::Draw() { ... }` lives in
    // the class's scope, but it also sees everything the file had in
    // scope at the point it was written -- the anonymous namespace it
    // sits in, for one. Name lookup follows the parent chain first and
    // this afterwards, which is the order the language uses too.
    int lexical_parent = -1;
};

// One place a name appears, resolved where possible. `symbol == -1` is a
// name this server could not place -- a member of an unknown type, a
// name from a header -- and is kept anyway, because hover and the
// unknown-name gate both need to know it was there.
struct CppOccurrence {
    CppPos pos;
    int length = 0;
    int symbol = -1;
    bool is_write = false;
    bool is_declaration = false;
    bool is_member = false;  // written after a `.`, `->` or `::`
    // For a member access whose object type this server did resolve, the
    // class symbol it resolved to. Together with `symbol == -1` that is
    // the one case where "no such member" is worth reporting: the type is
    // known, fully defined here, and does not have this name.
    int owner_type = -1;
    // The scope the name was written in. Kept because resolution runs
    // twice: C++ lets a member function's body use a member declared
    // below it, and a file-scope function call a function defined later,
    // so a name that did not resolve on the way past is retried once the
    // whole file has been walked.
    int scope = 0;
    std::string name;
};

struct CppIndex {
    CppParseResult parse;
    std::vector<std::string> lines;
    std::vector<CppSymbol> symbols;
    std::vector<CppScope> scopes;
    std::vector<CppOccurrence> occurrences;
    // Gates the whole-file checks read before reporting anything about
    // names (see cpp_lsp.h on why an unknown include silences them).
    bool has_unresolved_include = false;
    bool has_unknown_using_directive = false;
    // The file `import`s a module, whose exported names nothing here can
    // see. Same gate as an unreadable include, for the same reason.
    bool imports_module = false;
    bool file_is_header = false;
    bool truncated = false;
};

/**
 * @brief Parses a document and builds its scope model, symbols and name occurrences.
 * @param lines the document's text, one entry per line
 * @param opts filesystem-resolution and predefined-macro options
 * @return the index every feature answers from
 */
CppIndex BuildCppIndex(const std::vector<std::string> &lines, const CppLspOptions &opts);

/** @brief Finds the innermost scope containing a position, or 0 (the file scope). */
int CppScopeAt(const CppIndex &index, const CppPos &pos);

/** @brief Finds the occurrence covering a position, or nullptr. */
const CppOccurrence *CppOccurrenceAt(const CppIndex &index, int line, int col);

/**
 * @brief Looks a name up from a scope outward, honouring base classes and using-directives.
 * @return the symbol index, or -1 when the name is not declared in this file
 */
int CppLookupName(const CppIndex &index, int scope, const std::string &name);

/**
 * @brief Looks a name up from a scope outward, optionally considering only names that are types.
 * @param types_only when true, skips a constructor, function or variable that shares the type's name
 */
int CppLookupNameFiltered(const CppIndex &index, int scope, const std::string &name, bool types_only);

/** @brief Looks a member name up in a class scope and its base classes. */
int CppLookupMember(const CppIndex &index, int class_scope, const std::string &name);

/** @brief Resolves a type's bare name to the class/enum symbol it names, or -1. */
int CppResolveTypeName(const CppIndex &index, int scope, const std::string &type_base);

/** @brief Reports whether a position sits inside a string literal, a character literal or a comment. */
bool CppPositionInLiteral(const CppIndex &index, int line, int col);

/** @brief Returns the identifier at a position, with its byte range, or "" when there is none. */
std::string CppWordAt(const std::vector<std::string> &lines, int line, int col, int *out_start, int *out_end);

/** @brief The comment block immediately above a line, rendered as plain text ("" when there is none). */
std::string CppDocCommentAbove(const CppIndex &index, int line);

/** @brief Renders a symbol the way hover shows it (`int Editor::width` / `void f(int a) const`). */
std::string CppSymbolDetail(const CppSymbol &symbol);

#endif  // MEP_CPP_LSP_INDEX_H
