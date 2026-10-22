#ifndef MEP_CSHARP_LSP_ANALYSIS_H
#define MEP_CSHARP_LSP_ANALYSIS_H

// The C# language server's model of a file (shared by csharp_lsp*.cpp):
// the parsed document, the binding of names to the locals, parameters,
// labels and members they mean, the types the file declares, and a small
// type model over the file, the workspace (CsProject) and the BCL table
// (csharp_lsp_bcl.cpp) -- enough to say what `x.` offers and what a name
// under the cursor is, without a compiler.

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "csharp_lsp.h"
#include "csharp_lsp_bcl.h"
#include "csharp_parser.h"

namespace cslsp {

using csharp::Field;
using csharp::SyntaxKind;
using csharp::SyntaxNode;
using csharp::SyntaxTree;
using csharp::TextSpan;

// --- The document ------------------------------------------------------------------

struct Doc {
    std::vector<std::string> lines;
    std::unique_ptr<SyntaxTree> tree;

    explicit Doc(const std::vector<std::string> &lines);
    CsRange RangeOf(TextSpan span) const;
    CsRange RangeOf(const SyntaxNode *n) const { return RangeOf(n->Span()); }
    int OffsetAt(int line, int col) const { return tree->OffsetOf(line, col); }
    std::string Text(const SyntaxNode *n) const { return n ? tree->Utf8(n->Span()) : std::string(); }
    std::string Text(TextSpan s) const { return tree->Utf8(s); }
    /** @brief A token's text (UTF-8); for an identifier, its value (no @). */
    static std::string TokenValue(const SyntaxNode *token);
    /** @brief The identifier-ish token at a cursor: the one under it, else the one just left of it. */
    const SyntaxNode *TokenAt(int line, int col) const;
};

/** @brief The nearest ancestor (or the node itself) whose kind is one of `kinds`. */
const SyntaxNode *Ancestor(const SyntaxNode *n, std::initializer_list<SyntaxKind> kinds);
/** @brief Whether `kind` is a type declaration (class, struct, interface, record, record struct, enum, delegate,
 *         extension block). */
bool IsTypeDeclKind(SyntaxKind kind);
/** @brief The doc comment's <summary> above a declaration, as plain text. */
std::string DocSummary(const Doc &doc, const SyntaxNode *decl);
/** @brief A declaration's header as written: attributes, the body and any initializer left out. */
std::string DeclHeader(const Doc &doc, const SyntaxNode *decl);
/** @brief Whether a member declaration's modifiers include `kind` (StaticKeyword, PrivateKeyword...). */
bool HasModifier(const SyntaxNode *decl, SyntaxKind kind);
/** @brief The name token of a type or member declaration (nullptr for ones without a name). */
const SyntaxNode *DeclNameToken(const SyntaxNode *decl);

// --- Binding: locals, parameters, labels, local functions --------------------------------

enum class SymKind { Local, Parameter, RangeVariable, LocalFunction, Label, TypeParameter, PatternVariable,
                     OutVariable, ForEachVariable, CatchVariable, UsingVariable };

struct LocalSymbol {
    SymKind kind = SymKind::Local;
    std::string name;
    const SyntaxNode *name_token = nullptr;  // where it is declared
    const SyntaxNode *decl = nullptr;        // the declarator / parameter / designation / statement
    const SyntaxNode *type = nullptr;        // its type as written (nullptr if none; may be `var`)
    const SyntaxNode *init = nullptr;        // a local's initializer expression, a foreach's collection
    const SyntaxNode *scope = nullptr;       // the node it is visible in
    std::vector<const SyntaxNode *> refs;    // identifier tokens referring to it (not the declaration)
    std::vector<bool> ref_is_write;          // a pure write (x = ..., out x)
    std::vector<const SyntaxNode *> writes;  // assigned expressions (for x = <expr>), the initializer first
    bool in_macro_scope = false;
};

struct Binding {
    std::vector<LocalSymbol> symbols;
    std::unordered_map<const SyntaxNode *, int> by_token;  // a name token (declaration or reference) -> symbol
};

Binding Bind(const Doc &doc);

/** @brief The symbol a token names (declaration or reference); -1 when it is not a local-ish name. */
int SymbolAt(const Binding &b, const SyntaxNode *token);

// --- The types the file declares -------------------------------------------------------

/** @brief A type declared in source (this file or the workspace). */
struct SourceType {
    std::string ns;     // namespace
    std::string owner;  // enclosing types, "Outer.Inner"
    std::string name;   // without type parameters
    std::vector<std::string> type_params;
    std::string kind;   // class, struct, interface, enum, record, record struct, delegate
    std::vector<std::string> bases;  // as written
    std::string doc;
    std::string signature;
    std::string path;  // "" for the current file
    CsRange range;
    std::string FullName() const;
};

/** @brief A member declared in source. */
struct SourceMember {
    std::string owner_full;  // the type's FullName()
    std::string name;
    std::string kind;     // method, constructor, property, field, event, const, enum member, indexer
    std::string type;     // as written ("" for constructors)
    std::string params;   // "(int a, string b)" / "[int i]"
    std::string signature;
    std::string doc;
    bool is_static = false;
    bool is_extension = false;
    std::string path;
    CsRange range;
};

struct SourceIndex {
    std::vector<SourceType> types;
    std::vector<SourceMember> members;
};

/** @brief Collects the types and members a parsed file declares. */
void IndexDeclarations(const Doc &doc, const std::string &path, SourceIndex *out);

// --- Type names --------------------------------------------------------------------------

/** @brief A parsed type name: `System.Collections.Generic.List<int>[]?`, `(int a, string b)`, `T*`. */
struct TypeName {
    std::string name;  // qualified as written, without type arguments ("List", "System.String", "int")
    std::vector<TypeName> args;
    std::vector<int> ranks;  // array ranks, outermost first
    bool nullable = false;
    int pointer = 0;
    std::vector<TypeName> tuple;  // a tuple's element types (name empty then)
    std::vector<std::string> tuple_names;
    bool Empty() const { return name.empty() && tuple.empty(); }
    bool IsArray() const { return !ranks.empty(); }
};

TypeName ParseTypeName(const std::string &text);
/** @brief Short display ("List<int>", "string[]"): namespaces dropped, keywords for special types. */
std::string ShowType(const TypeName &t);
/** @brief Replaces type parameters by their arguments. */
TypeName Substitute(const TypeName &t, const std::map<std::string, TypeName> &subst);
/** @brief "int" -> "System.Int32" and the other C# keyword types; anything else unchanged. */
std::string KeywordTypeFullName(const std::string &name);
/** @brief "System.Int32" -> "int"; anything else unchanged. */
std::string FullNameKeyword(const std::string &name);

// --- The type universe ------------------------------------------------------------------

/** @brief A resolved type: a BCL type or a source one, with its type arguments bound. */
struct ResolvedType {
    const CsBclType *bcl = nullptr;
    const SourceType *source = nullptr;
    TypeName name;  // as referenced (with arguments)
    std::map<std::string, TypeName> subst;
    bool is_static_ref = false;  // the expression names the type itself (Console.), not a value of it
    std::string ns;              // the expression names a namespace (System.)
    bool Known() const { return bcl || source; }
    std::string FullName() const;
    std::string Kind() const;
};

/** @brief A member found on a type (BCL or source), with the receiver's substitution applied to its type. */
struct MemberInfo {
    const CsBclMember *bcl = nullptr;
    const SourceMember *source = nullptr;
    std::string name;
    std::string kind;       // method, constructor, property, field, event, const, enum member, indexer
    TypeName type;          // substituted
    std::string signature;  // display
    std::string params;
    std::string doc;
    std::string owner;  // display name of the declaring type
    bool is_static = false;
    bool is_extension = false;
};

/** @brief Source declarations with their lookup indexes (one file's, or a whole workspace's). */
struct SourceTables {
    std::vector<SourceType> types;
    std::vector<SourceMember> members;
    std::multimap<std::string, size_t> by_name;   // simple name (with `n for generics) -> types
    std::multimap<std::string, size_t> by_owner;  // owner full name -> members
    std::vector<std::string> namespaces;
    void Load(const SourceIndex &idx);
};

class Universe {
public:
    /** @param current the current file's declarations (positions in it); project the workspace's (may be null; its
     *         tables are built once per project version and shared). */
    Universe(const SourceIndex *current, const CsProject *project);

    /** @brief Resolves a type name seen at `context` (a node, for its namespace, usings and enclosing types). */
    ResolvedType Resolve(const TypeName &t, const std::vector<std::string> &usings, const std::string &ns,
                         const std::vector<std::string> &enclosing) const;
    /** @brief Members named `name` (every member when empty) on the type, walking base types and interfaces. */
    std::vector<MemberInfo> Members(const ResolvedType &t, const std::string &name, bool include_inherited = true) const;
    /** @brief Extension methods applicable to a receiver of type `t` from the imported namespaces. */
    std::vector<MemberInfo> Extensions(const ResolvedType &t, const std::string &name,
                                       const std::vector<std::string> &usings) const;
    /** @brief Types visible by simple name through `usings` / `ns` (for completion). */
    std::vector<ResolvedType> VisibleTypes(const std::vector<std::string> &usings, const std::string &ns) const;
    /** @brief Whether `ns` is a namespace this universe knows (BCL or source), and its child namespaces. */
    bool IsNamespace(const std::string &ns) const;
    std::vector<std::string> ChildNamespaces(const std::string &ns) const;
    /** @brief Types directly in namespace `ns`. */
    std::vector<ResolvedType> TypesIn(const std::string &ns) const;
    /** @brief The direct base types and interfaces of a type, resolved. */
    std::vector<ResolvedType> Bases(const ResolvedType &t) const;
    /** @brief The source layers: the current file's first, then the workspace's. */
    const std::vector<std::shared_ptr<const SourceTables>> &Layers() const { return layers_; }

private:
    std::vector<std::shared_ptr<const SourceTables>> layers_;
};

/** @brief The BCL table's indexes (built once). */
struct BclIndex {
    const CsBclType *types = nullptr;
    size_t type_count = 0;
    const CsBclMember *members = nullptr;
    size_t member_count = 0;
    std::multimap<std::string, size_t> by_simple;          // "List" -> types
    std::unordered_map<std::string, size_t> by_full;       // "System.Collections.Generic.List`1" -> type
    std::unordered_map<std::string, std::pair<size_t, size_t>> member_range;  // owner name -> [begin, end)
    std::multimap<std::string, size_t> extensions;         // method name -> members (extension methods)
    std::vector<std::string> namespaces;
};
const BclIndex &Bcl();
/** @brief "System.Collections.Generic.List<T>" -> "System.Collections.Generic.List`1". */
std::string MetadataKey(const TypeName &t);
/** @brief The namespace of a BCL type ("System.Collections.Generic"). */
std::string BclNamespace(const CsBclType &t);

// --- Context of a position ------------------------------------------------------------------

/** @brief What surrounds a node: namespace, usings, enclosing type declarations. */
struct Context {
    std::string ns;
    std::vector<std::string> usings;  // namespaces imported (file + namespace usings + implicit global usings)
    std::vector<std::string> enclosing;  // type names, outermost first
    const SyntaxNode *type_decl = nullptr;  // the innermost enclosing type declaration
    const SyntaxNode *member_decl = nullptr;  // the enclosing member
    bool in_static = false;
};

Context ContextAt(const Doc &doc, const SyntaxNode *node);

// --- Expression types ------------------------------------------------------------------------

/** @brief Everything needed to type expressions in one file. */
struct Model {
    const Doc &doc;
    const Binding &binding;
    const Universe &universe;
    const SourceIndex &current;
    Model(const Doc &d, const Binding &b, const Universe &u, const SourceIndex &c)
        : doc(d), binding(b), universe(u), current(c) {}

    /** @brief The type of an expression, as far as a reader can see it (Known() false otherwise). */
    ResolvedType TypeOf(const SyntaxNode *expr, int depth = 0) const;
    /** @brief The declared type of a local symbol (`var` inferred from its initializer). */
    ResolvedType TypeOfSymbol(int symbol, int depth = 0) const;
    /** @brief The type a type syntax node denotes. */
    ResolvedType ResolveTypeNode(const SyntaxNode *type) const;
    ResolvedType ResolveTypeText(const std::string &text, const SyntaxNode *context) const;
    /** @brief Members of the enclosing type(s) of `node` named `name`. */
    std::vector<MemberInfo> EnclosingMembers(const SyntaxNode *node, const std::string &name) const;
    /** @brief The members an access `expr.name` means (methods: every overload). */
    std::vector<MemberInfo> MembersOf(const ResolvedType &receiver, const std::string &name,
                                      const SyntaxNode *context) const;
};

}  // namespace cslsp

#endif  // MEP_CSHARP_LSP_ANALYSIS_H
