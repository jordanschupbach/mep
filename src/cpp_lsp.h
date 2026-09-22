#ifndef MEP_CPP_LSP_H
#define MEP_CPP_LSP_H

#include <string>
#include <vector>

// The analysis half of mep's own C++ language server (the wire half is
// cpp_lsp_server.cpp, the `mep-cpp-lsp` binary; kBuiltinLsp's `cpp_ls`
// registry entry in main.cpp is what spawns it). The syntax tree
// everything here answers from is cpp_ast.h's.
//
// Deliberately raylib-free and I/O-light, for the same reason org_lsp.h
// and python_lsp.h are: every feature is a pure function over plain
// std::string lines, so it is testable without a GL context, without a
// language-server client and without a process (cpp_lsp_test.cpp drives
// all of it directly). The one exception is the filesystem lookup behind
// CppLspOptions::check_files -- resolving `#include "sibling.h"` to a
// file next to the document, and listing a directory for include
// completions -- which is why that is a flag rather than unconditional.
//
// Scope, named rather than silently omitted (the same convention
// org_lsp.h and python_lsp.h use, and here it matters more than in
// either, because C++ tooling is usually a compiler in disguise):
//
//   - **One file is the whole world, plus the headers next to it.**
//     Nothing here runs a compiler, reads a compilation database or
//     resolves a system include. `#include <vector>` tells this server
//     that the standard library is in play and nothing more: `std::`
//     completions come from a curated table compiled into the binary
//     (cpp_lsp_vocab.cpp), never from having read <vector>.
//   - **No type checking, and there never will be one here.** A checker
//     that cannot see the types it is checking would be wrong far more
//     often than right. What is reported instead is what a careful
//     reader could confirm from the file in front of them: syntax and
//     preprocessor errors, binding hygiene (a local assigned and never
//     read, a static function nobody calls, a declaration shadowing
//     another), and a small set of idiom checks whose evidence is
//     entirely local -- a member initializer list out of order against
//     the fields right above it, a class with virtual functions and a
//     public non-virtual destructor, an assignment inside an `if`.
//   - **Names are resolved, not guessed.** The scope model is real
//     (namespaces, classes with their bases, functions, blocks,
//     using-directives and using-declarations), which is what makes go
//     to definition, find references and rename mean something. Where it
//     runs out -- a member of a class whose base is in another header,
//     a name a macro invented -- the answer is "unknown", and an unknown
//     name is never reported as an error.
//   - **The undefined-name check switches itself off** for a file that
//     includes a header this server could not read, the way python_lsp's
//     does for `from x import *`. In practice that is nearly every .cpp
//     file, which is the honest outcome: see CppLspDiagnostics.
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16
// code units -- see cpp_ast.h, and org_lsp.h for why mep's client wants
// it that way.

// --- Options ----------------------------------------------------------

struct CppLspOptions {
    // Directory the document lives in. `#include "sibling.h"` resolves
    // against it; empty disables include resolution regardless of
    // `check_files`.
    std::string doc_dir;
    // The document's own base name ("editor.cpp"). A header gets two
    // rules a source file does not: its include guard is checked, and
    // `using namespace` at file scope is reported.
    std::string file_name;
    // Extra directories a quoted include may resolve against, tried in
    // order after `doc_dir` (a project's own `include/`).
    std::vector<std::string> include_dirs;
    // Macros to treat as defined before the file starts, each `NAME` or
    // `NAME=value`. The host-shaped set (`__cplusplus`, `__GNUC__`,
    // `__linux__`, ...) is always predefined; this is for a project's
    // own build-system defines.
    std::vector<std::string> defines;
    // Whether to touch the filesystem at all (resolving an include to a
    // file, listing a directory for include completions). Off in tests,
    // and off for an unsaved buffer with no directory to resolve against.
    bool check_files = true;
    // Columns past which a line earns a Hint, or 0 for no length check.
    // Not a style opinion this server holds on its own -- it is off
    // unless a caller asks for it.
    int max_line_length = 0;
};

// --- Diagnostics ------------------------------------------------------

// LSP DiagnosticSeverity, spelled out so callers don't have to remember
// which end of the scale is which.
enum class CppLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct CppLspDiagnostic {
    int line = 0;       // 0-based
    int col_start = 0;  // 0-based byte column, half-open [col_start, col_end)
    int col_end = 0;
    CppLspSeverity severity = CppLspSeverity::Error;
    // Stable machine-readable id ("unused-variable", "member-init-order",
    // ...), emitted as the LSP Diagnostic.code. Tests assert on these
    // rather than on message wording, so message text stays free to
    // improve.
    std::string code;
    std::string message;
};

/**
 * @brief Analyzes a C++ document, reporting syntax, preprocessor, binding and idiom problems.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param opts filesystem-resolution, predefined-macro and style options (see CppLspOptions)
 * @return every diagnostic found, in ascending (line, col_start) order
 */
std::vector<CppLspDiagnostic> CppLspDiagnostics(const std::vector<std::string> &lines, const CppLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class CppLspKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Module = 9,
    Property = 10,
    Value = 12,
    Enum = 13,
    Keyword = 14,
    File = 17,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    TypeParameter = 25,
};

struct CppLspCompletionItem {
    // What the list shows.
    std::string label;
    // What replacing [replace_start, replace_end) with this produces.
    // Always single-line, for the same reason org_lsp.h gives: mep's
    // Editor::AcceptCompletion splices the text into one Buffer line, so
    // an embedded '\n' would corrupt the line model.
    std::string insert_text;
    CppLspKind kind = CppLspKind::Text;
    std::string detail;         // short right-hand annotation (a signature, a type)
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line, aligned to the identifier
    // immediately before the cursor (mep's client replaces exactly that
    // word -- see Editor::UpdateCompletionPopup).
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes C++-aware completions for a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (directory listing for include completions)
 * @return the candidates for this context, already filtered against the typed prefix; empty inside strings and comments
 */
std::vector<CppLspCompletionItem> CppLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const CppLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct CppLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    int line = 0;      // range the hover applies to
    int col_start = 0;
    int col_end = 0;
};

/**
 * @brief Explains the name, member, keyword, macro or literal under a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (an `#include` hover names the file it resolved to)
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
CppLspHoverInfo CppLspHover(const std::vector<std::string> &lines, int line, int col, const CppLspOptions &opts);

// --- Document symbols -------------------------------------------------

struct CppLspSymbol {
    std::string name;
    std::string detail;  // the signature, the base clause, or the declared type
    // LSP SymbolKind: File 1, Module 2, Namespace 3, Class 5, Method 6,
    // Property 7, Field 8, Constructor 9, Enum 10, Function 12,
    // Variable 13, Constant 14, Struct 23, TypeParameter 26.
    int kind = 13;
    int line_start = 0;  // whole definition, for the symbol's `range`
    int line_end = 0;
    int sel_col_start = 0;  // the name itself, for `selectionRange`
    int sel_col_end = 0;
    int sel_line = 0;
    int parent = -1;  // index into the returned vector, -1 for top level
};

/**
 * @brief Lists every namespace, class, function, member, enum, typedef and macro, in document order with parent links.
 * @param lines the document's text, one entry per line
 * @return one symbol per declaration; empty for a document with none
 */
std::vector<CppLspSymbol> CppLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct CppLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region), "comment", "imports" or "region"
};

/**
 * @brief Computes folding ranges for braced blocks, comment runs, the include block and `#if` groups.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<CppLspFold> CppLspFolds(const std::vector<std::string> &lines);

// --- Definition -------------------------------------------------------

struct CppLspLocation {
    bool found = false;
    // Absolute path of the target file, or "" meaning "this document".
    std::string path;
    int line = 0;
    int col = 0;
};

/**
 * @brief Resolves the name under a cursor position to where it is declared (in this file, or an included header).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (`doc_dir` anchors an `#include "..."` lookup)
 * @return the resolved location, or a default-constructed value with `found == false`
 */
CppLspLocation CppLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                const CppLspOptions &opts);

// --- References, highlight and rename ---------------------------------

struct CppLspReference {
    int line = 0;
    int col_start = 0;
    int col_end = 0;
    bool is_write = false;  // a declaration or an assignment, for DocumentHighlightKind.Write
};

struct CppLspReferenceSet {
    bool found = false;
    std::string name;
    // Why a rename must be refused, or "" when it may proceed (a
    // keyword, a name from outside this file, a macro-produced token).
    std::string rename_blocked_reason;
    std::vector<CppLspReference> refs;  // ascending (line, col_start), the declaration included
};

/**
 * @brief Finds every occurrence of the declaration under a cursor position, within the scope it lives in.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the occurrences (declaration first in document order), or `found == false` when the cursor is not on a name
 */
CppLspReferenceSet CppLspReferences(const std::vector<std::string> &lines, int line, int col);

// --- Signature help ---------------------------------------------------

struct CppLspSignature {
    bool found = false;
    std::string label;          // "connect(const Host &h, int port = 80)"
    std::string documentation;  // the callee's leading comment, when there is one
    std::vector<std::string> params;  // each parameter's own label, for highlighting
    int active_param = 0;
};

/**
 * @brief Describes the call the cursor sits inside, and which of its parameters the cursor is on.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the signature, or a default-constructed value with `found == false` when the cursor is not in a call
 */
CppLspSignature CppLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Shared vocabulary (exposed for tests, hover and completion) ------

// One entry of the server's built-in C++ vocabulary: a keyword, a
// standard-library name, a header or a member, with the short annotation
// a completion list shows (`detail`, a signature where there is one) and
// the sentence hover shows (`doc`).
struct CppLspVocabEntry {
    const char *name;
    const char *detail;
    const char *doc;
};

/** @brief The reserved-keyword vocabulary. */
const std::vector<CppLspVocabEntry> &CppLspKeywordVocab();
/** @brief The standard-library name vocabulary (`std::vector`, `std::sort`, `size_t`, ...), without the `std::`. */
const std::vector<CppLspVocabEntry> &CppLspStdVocab();
/** @brief The preprocessor-directive vocabulary, offered after a `#`. */
const std::vector<CppLspVocabEntry> &CppLspDirectiveVocab();
/** @brief The standard-header vocabulary, offered inside `#include <...>`. */
const std::vector<CppLspVocabEntry> &CppLspHeaderVocab();
/** @brief A standard type's members ("std::string", "std::vector", ...), or nullptr when there is no table for it. */
const std::vector<CppLspVocabEntry> *CppLspTypeMembers(const std::string &type);
/** @brief Reports whether a name is one this server knows from the standard library or the language itself. */
bool CppLspIsKnownName(const std::string &name);
/** @brief The header a standard name lives in ("vector" for `std::vector`), or "" when unknown. */
std::string CppLspHeaderForName(const std::string &name);

#endif  // MEP_CPP_LSP_H
