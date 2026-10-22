#ifndef MEP_CSHARP_LSP_H
#define MEP_CSHARP_LSP_H

// The analysis half of mep's own C# language server (the wire half is
// csharp_lsp_server.cpp, the `mep-csharp-lsp` binary, also a command-line
// linter with `--lint FILE...`). No compiler is run: the file is read by
// mep's translation of Roslyn's own lexer and parser (csharp_parser.h --
// Roslyn's tree and its syntax errors, word for word), and the .NET base
// class library is known from a table generated from its reference
// assemblies and XML docs (csharp_lsp_bcl.cpp).
//
// Scope, named rather than silently omitted:
//   - Locals, parameters, range variables, pattern and out variables are
//     resolved by scope as C# scopes them (a block, a for/foreach/using/
//     catch, a lambda, a local function, a switch section).
//   - Types and members are known by name: what the file and the rest of
//     the workspace declare (CsProject) and the base class library's. Types
//     are inferred only as far as a reader can see them -- a declared type,
//     `var x = new T(...)`, a literal, a cast or `as`, a member whose type
//     is written down, a BCL member's declared type -- which is what `.`
//     completion uses; a receiver whose type is not visible gets no guesses.
//   - No semantic checks (overload resolution, conversions, definite
//     assignment): those need the compiler's binder. The lints are Roslyn's
//     syntax errors plus what a single file shows (see CsLspDiagnostics).
//
// Positions are 0-based lines and 0-based byte columns (LSP utf-8).

#include <memory>
#include <string>
#include <vector>

struct CsRange {
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    bool operator==(const CsRange &o) const {
        return start_line == o.start_line && start_col == o.start_col && end_line == o.end_line && end_col == o.end_col;
    }
};

// --- The project -----------------------------------------------------------------

struct CsProjectDef {
    std::string kind;       // "class", "struct", "interface", "enum", "record", "delegate", "method", "constructor",
                            // "property", "field", "event", "const", "enum member", "indexer"
    std::string ns;         // the namespace ("App.Models"), "" for the global one
    std::string owner;      // the enclosing type's name path ("Outer.Inner"), "" for a type at namespace level
    std::string name;       // member or type name (a type's without type parameters)
    std::string signature;  // the declaration's header as written (attributes and body left out)
    std::string doc;        // the /// <summary>, plain text
    std::string type;       // a member's type as written ("List<int>"), a method's return type
    std::string bases;      // a type's base list as written, comma-separated ("Base, IFoo")
    std::string params;     // "(int a, string b)" for methods / constructors / delegates
    bool is_static = false;
    bool is_extension = false;  // a static method whose first parameter is `this T`
    std::string path;           // file
    CsRange range;              // its name there
};

struct CsProject {
    std::vector<CsProjectDef> defs;
    // Bookkeeping: which files are in `defs`, a version bumped on every change, and the analysis's lookup
    // tables over `defs`, rebuilt only when the version moves.
    std::vector<std::string> paths;
    int version = 0;
    mutable std::shared_ptr<const void> cache;
    mutable int cache_version = -1;
};

/** @brief Adds (or replaces) what one file declares. */
void CsProjectAddFile(CsProject *project, const std::string &path, const std::vector<std::string> &lines);

// --- Diagnostics -------------------------------------------------------------------

enum class CsLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct CsLspDiagnostic {
    CsRange range;
    CsLspSeverity severity = CsLspSeverity::Error;
    std::string code;  // "CS1002" for Roslyn's own, mep's lint names otherwise
    std::string message;
    bool unnecessary = false;  // tag: an unused thing (editors dim it)
};

/**
 * @brief Lints a C# file: Roslyn's syntax errors (and #error/#warning), then, if it parses, the lints:
 *        unused locals and labels, locals assigned but never read, unreachable code, duplicate usings,
 *        self-assignment and self-comparison, a mistaken empty statement, unused private members,
 *        unused parameters and usings (hints).
 */
std::vector<CsLspDiagnostic> CsLspDiagnostics(const std::vector<std::string> &lines, const std::string &path = "");

/** @brief One diagnostic as `--lint` prints it: `path:line:col: severity: message [code]` (1-based). */
std::string CsLspFormatDiagnostic(const std::string &path, const CsLspDiagnostic &d);

/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool CsLspDiagnosticFails(const CsLspDiagnostic &d);

// --- Completion ----------------------------------------------------------------------

enum class CsLspKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    Event = 23,
    Operator = 24,
    TypeParameter = 25,
};

struct CsLspCompletionItem {
    std::string label;
    std::string insert_text;
    CsLspKind kind = CsLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0;  // byte columns on the request's line
    int replace_end = 0;
    std::string sort_text;
};

/**
 * @brief Completions at a cursor: after `x.`, the members of x's type (static ones after a type name, a
 *        namespace's contents after a namespace), base types and interfaces walked, extension methods of
 *        the imported namespaces included; otherwise locals and parameters in scope, the enclosing types'
 *        members, the file's, workspace's and imported namespaces' types, and keywords.
 */
std::vector<CsLspCompletionItem> CsLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                  const CsProject *project = nullptr);

// --- Hover -------------------------------------------------------------------------

struct CsLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    CsRange range;
};

/** @brief What the name under the cursor is: a local or parameter (and its type), a member, a type, a keyword. */
CsLspHoverInfo CsLspHover(const std::vector<std::string> &lines, int line, int col, const CsProject *project = nullptr);

// --- Signature help ------------------------------------------------------------------

struct CsLspSignature {
    std::string label;                // "string String.Substring(int startIndex, int length)"
    std::vector<std::string> params;  // substrings of `label`
    std::string documentation;
};

struct CsLspSignatureHelp {
    bool found = false;
    std::vector<CsLspSignature> signatures;  // one per overload
    int active_signature = 0;
    int active_param = -1;
};

/** @brief The overloads of the method (or constructor, after `new T(`) the cursor is in a call to. */
CsLspSignatureHelp CsLspSignatures(const std::vector<std::string> &lines, int line, int col,
                                   const CsProject *project = nullptr);

// --- Symbols, folds ---------------------------------------------------------------------

enum class CsLspSymbolKind {
    File = 1,
    Module = 2,
    Namespace = 3,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    EnumMember = 22,
    Struct = 23,
    Event = 24,
    Operator = 25,
};

struct CsLspSymbol {
    std::string name;
    std::string detail;
    CsLspSymbolKind kind = CsLspSymbolKind::Method;
    CsRange range;
    CsRange sel_range;
    int parent = -1;
};

/** @brief The outline: namespaces, types, members (methods, constructors, properties, fields, events,
 *         operators, indexers), enum members, local functions. */
std::vector<CsLspSymbol> CsLspSymbols(const std::vector<std::string> &lines);

struct CsLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "", "comment", "imports" or "region"
};

/** @brief Every multi-line declaration, block, initializer and argument list, #region..#endregion, runs of
 *         usings, doc comments and comment lines. */
std::vector<CsLspFold> CsLspFolds(const std::vector<std::string> &lines);

// --- Navigation ------------------------------------------------------------------------

struct CsLspLocation {
    bool found = false;
    std::string path;  // "" for the file itself
    CsRange range;
};

/** @brief Where the name under the cursor is declared: a local, parameter, member or type, in this file or
 *         the workspace. */
CsLspLocation CsLspDefinition(const std::vector<std::string> &lines, int line, int col,
                              const CsProject *project = nullptr);

struct CsLspReference {
    CsRange range;
    bool is_definition = false;
    bool is_write = false;
};

/** @brief Every occurrence in the file of the local, parameter, member or type under the cursor. */
std::vector<CsLspReference> CsLspReferences(const std::vector<std::string> &lines, int line, int col);

struct CsLspRenameTarget {
    bool found = false;
    CsRange range;
    std::string placeholder;
};

/** @brief What prepareRename offers: a local, parameter, label, or a type or member the file declares. */
CsLspRenameTarget CsLspPrepareRename(const std::vector<std::string> &lines, int line, int col);

struct CsLspTextEdit {
    CsRange range;
    std::string new_text;
};

/** @brief The edits renaming the name under the cursor in this file; empty with *error set when it cannot. */
std::vector<CsLspTextEdit> CsLspRename(const std::vector<std::string> &lines, int line, int col,
                                       const std::string &new_name, std::string *error);

/** @brief textDocument/formatting: mep's C# formatter (csharp_format.h, the one `gf` runs), as one edit over the changed lines. */
std::vector<CsLspTextEdit> CsLspFormat(const std::vector<std::string> &lines);

#endif  // MEP_CSHARP_LSP_H
