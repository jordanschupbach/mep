#ifndef MEP_CRYSTAL_LSP_H
#define MEP_CRYSTAL_LSP_H

// The analysis half of mep's own Crystal language server (the wire half is
// crystal_lsp_server.cpp, the `mep-crystal-lsp` binary, also a command-line
// linter with `--lint FILE...`). No compiler is run: the file is read by
// mep's port of the Crystal compiler's own lexer and parser
// (crystal_parser.h -- the same syntax errors, word for word), and the
// standard library is known from a table generated from its API docs
// (crystal_lsp_stdlib.cpp).
//
// Scope, named rather than silently omitted:
//   - Locals are resolved the way the compiler scopes them: a `def`, a
//     type body and the file each start afresh; a block or proc literal
//     sees the locals around it and keeps its own. The parser already
//     decides whether a bare name is a local or a method call, so every
//     local this server knows is one the compiler would see.
//   - Methods and types are known by name: those the file and the rest of
//     the workspace define (CrProject), and the standard library's. Types
//     are inferred only as far as a reader can see them -- a literal,
//     `T.new`, a type declaration or restriction, a method whose return
//     type is written down -- which is what `.` completion uses; a
//     receiver whose type is not visible gets every method name known.
//   - No semantic checks (an undefined method, a type mismatch): those
//     need the whole program's type inference. The lints are syntax,
//     the parser's warnings, and what a single file shows (unused
//     locals and arguments, unreachable code, a relative `require` that
//     names no file).
//   - Macros are not expanded: a macro body is text, and only the
//     `getter`/`property`/`setter` families and `record` are understood
//     for the methods and types they define.
//
// Positions are 0-based lines and 0-based byte columns (LSP utf-8).

#include <string>
#include <vector>

struct CrRange {
    int start_line = 0;
    int start_col = 0;
    int end_line = 0;
    int end_col = 0;
    bool operator==(const CrRange &o) const {
        return start_line == o.start_line && start_col == o.start_col && end_line == o.end_line && end_col == o.end_col;
    }
};

// --- The project ---------------------------------------------------------------

struct CrProjectDef {
    std::string kind;   // "def", "macro", "class", "struct", "module", "enum", "lib", "fun", "const", "alias", "annotation"
    std::string owner;  // the enclosing type's path ("Foo::Bar"), "" at the top level
    std::string name;   // method or type name (types: the last segment)
    std::string signature;  // the definition's header line(s) as written
    std::string doc;
    std::string ret;          // a def's written return type, else ""
    std::string superclass;   // a class's, as written
    std::vector<std::string> includes;
    bool class_method = false;  // `def self.foo`
    std::string path;           // file
    CrRange range;              // its name there
};

struct CrProject {
    std::vector<CrProjectDef> defs;
};

/** @brief Adds (or replaces) what one file defines. */
void CrProjectAddFile(CrProject *project, const std::string &path, const std::vector<std::string> &lines);

// --- Diagnostics -------------------------------------------------------------------

enum class CrLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct CrLspDiagnostic {
    CrRange range;
    CrLspSeverity severity = CrLspSeverity::Error;
    std::string code;
    std::string message;
    bool unnecessary = false;  // tag: an unused thing (editors dim it)
};

/**
 * @brief Lints a Crystal file: the syntax error, the parser's warnings, then the lints.
 * @param path the file's path, for resolving a relative `require` ("" skips that check)
 */
std::vector<CrLspDiagnostic> CrLspDiagnostics(const std::vector<std::string> &lines, const std::string &path = "");

/** @brief One diagnostic as `--lint` prints it: `path:line:col: severity: message [code]` (1-based). */
std::string CrLspFormatDiagnostic(const std::string &path, const CrLspDiagnostic &d);

/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool CrLspDiagnosticFails(const CrLspDiagnostic &d);

// --- Completion ----------------------------------------------------------------------

enum class CrLspKind {
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
    Operator = 24,
};

struct CrLspCompletionItem {
    std::string label;
    std::string insert_text;
    CrLspKind kind = CrLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0;  // byte columns on the request's line
    int replace_end = 0;
    std::string sort_text;
};

/**
 * @brief Completions at a cursor: after `x.`, the methods of x's type (its class methods after a type name);
 *        after `A::`, what A contains; after `@`, the type's instance variables; otherwise locals in scope,
 *        the file's and workspace's methods and types, the standard library's top-level methods and types,
 *        and keywords.
 */
std::vector<CrLspCompletionItem> CrLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                  const CrProject *project = nullptr);

// --- Hover -------------------------------------------------------------------------

struct CrLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    CrRange range;
};

/** @brief What the name under the cursor is: a local (and its type when visible), a method, a type, a constant. */
CrLspHoverInfo CrLspHover(const std::vector<std::string> &lines, int line, int col, const CrProject *project = nullptr);

// --- Signature help ------------------------------------------------------------------

struct CrLspSignature {
    std::string label;                // "def split(separator : String, limit = nil) : Array(String)"
    std::vector<std::string> params;  // substrings of `label`
    std::string documentation;
};

struct CrLspSignatureHelp {
    bool found = false;
    std::vector<CrLspSignature> signatures;  // one per overload
    int active_signature = 0;
    int active_param = -1;
};

/** @brief The overloads of the method the cursor is in a call to, and the argument it is on. */
CrLspSignatureHelp CrLspSignatures(const std::vector<std::string> &lines, int line, int col,
                                   const CrProject *project = nullptr);

// --- Symbols, folds ---------------------------------------------------------------------

enum class CrLspSymbolKind {
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
    Operator = 25,
};

struct CrLspSymbol {
    std::string name;
    std::string detail;
    CrLspSymbolKind kind = CrLspSymbolKind::Function;
    CrRange range;
    CrRange sel_range;
    int parent = -1;
};

/** @brief The outline: types, methods, macros, constants, enum members, lib funs, getters/properties. */
std::vector<CrLspSymbol> CrLspSymbols(const std::vector<std::string> &lines);

struct CrLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" or "comment"
};

/** @brief Every multi-line definition, block, control structure and literal, and runs of comment lines. */
std::vector<CrLspFold> CrLspFolds(const std::vector<std::string> &lines);

// --- Navigation ------------------------------------------------------------------------

struct CrLspLocation {
    bool found = false;
    std::string path;  // "" for the file itself
    CrRange range;
};

/** @brief Where the name under the cursor is defined: a local's first binding, an instance variable's first
 *         assignment, a method or type in the file or the workspace. */
CrLspLocation CrLspDefinition(const std::vector<std::string> &lines, int line, int col,
                              const CrProject *project = nullptr);

struct CrLspReference {
    CrRange range;
    bool is_definition = false;
    bool is_write = false;
};

/** @brief Every occurrence in the file of the local, instance variable, method or type under the cursor. */
std::vector<CrLspReference> CrLspReferences(const std::vector<std::string> &lines, int line, int col);

struct CrLspRenameTarget {
    bool found = false;
    CrRange range;
    std::string placeholder;
};

/** @brief What prepareRename offers: a local, an instance or class variable, or a method or type the file defines. */
CrLspRenameTarget CrLspPrepareRename(const std::vector<std::string> &lines, int line, int col);

struct CrLspTextEdit {
    CrRange range;
    std::string new_text;
};

/** @brief The edits renaming the name under the cursor in this file; empty with *error set when it cannot. */
std::vector<CrLspTextEdit> CrLspRename(const std::vector<std::string> &lines, int line, int col,
                                       const std::string &new_name, std::string *error);

/** @brief The file formatted with mep's Crystal formatter, as one edit (none when it is unchanged or refused). */
std::vector<CrLspTextEdit> CrLspFormat(const std::vector<std::string> &lines);

#endif  // MEP_CRYSTAL_LSP_H
