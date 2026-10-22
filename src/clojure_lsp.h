#ifndef MEP_CLOJURE_LSP_H
#define MEP_CLOJURE_LSP_H

#include <map>
#include <string>
#include <vector>

#include "clojure_reader.h"

// The analysis half of mep's own Clojure language server (the wire half is
// clojure_lsp_server.cpp, the `mep-clojure-lsp` binary, also a command-line
// linter with `--lint`). No JVM is started and nothing is evaluated: the
// file is read by clojure_reader.h and analyzed here, with clojure.core and
// the namespaces that ship with Clojure known from a table generated from
// Clojure itself (clojure_lsp_core_names.cpp).
//
// Scope, named rather than silently omitted:
//   - One file at a time, plus what the rest of the project defines when
//     the caller passes a CljProject (the server indexes the workspace's
//     .clj/.cljc files): its namespaces' public vars, their arglists and
//     docstrings, and where they are.
//   - Macros are not expanded. clojure.core's are known form by form (what
//     `let`, `fn`, `for`, `defrecord`, `case`, `->`... bind and evaluate).
//     A call this server cannot identify -- a library's macro, say -- is
//     walked as code, and every symbol written in a vector among its
//     arguments counts as bound inside it (`go-loop [x 1]`, `are [x y]`):
//     the common shape of a macro that binds names.
//   - The name checks (unresolved symbol, namespace or var) run on Clojure
//     code: a .clj/.bb file, or the :clj and :default branches of a .cljc
//     one -- not on ClojureScript, whose cljs.core and js/ interop this
//     server does not know -- and they calibrate themselves to the file:
//     a file that :use's or :refer :all's a namespace the server does not
//     know, or has more than five names it cannot place, is part of a
//     context it cannot see, and gets none.
//
// Positions are 0-based lines and 0-based byte columns.

// --- The project ---------------------------------------------------------

struct CljProjectVar {
    std::string ns;
    std::string name;
    std::string kind;      // "function", "macro", "var"
    std::string arglists;  // one per line: "[x]\n[x y]"
    std::string doc;
    std::string path;      // the file defining it
    CljRange range;        // its name there
    bool is_private = false;
};

struct CljProject {
    std::vector<CljProjectVar> vars;
    std::map<std::string, std::string> ns_paths;  // namespace -> the file declaring it
};

/** @brief Adds (or replaces) what one file declares: its namespace and its vars. */
void CljProjectAddFile(CljProject *project, const std::string &path, const std::vector<std::string> &lines);

// --- Diagnostics -----------------------------------------------------------

enum class CljLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct CljLspDiagnostic {
    CljRange range;
    CljLspSeverity severity = CljLspSeverity::Error;
    std::string code;
    std::string message;
};

/**
 * @brief Lints a Clojure file: reader errors, then the compiler's own errors this server can see (an unresolved
 *        name, a use before its definition, a wrong argument count, a malformed binding form), then style.
 * @param path the file's path: its extension decides .clj/.cljc/.cljs/.edn, and the `ns` name is checked against it
 */
std::vector<CljLspDiagnostic> CljLspDiagnostics(const std::vector<std::string> &lines, const std::string &path = "",
                                                const CljProject *project = nullptr);

/** @brief One diagnostic as `--lint` prints it: `path:line:col: severity: message [code]` (1-based). */
std::string CljLspFormatDiagnostic(const std::string &path, const CljLspDiagnostic &d);

/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool CljLspDiagnosticFails(const CljLspDiagnostic &d);

// --- Completion --------------------------------------------------------------

enum class CljLspKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Keyword = 14,
    Snippet = 15,
    Constant = 21,
    Struct = 22,
    Operator = 24,
};

struct CljLspCompletionItem {
    std::string label;
    std::string insert_text;
    CljLspKind kind = CljLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0;  // byte columns on the request's line
    int replace_end = 0;
    std::string sort_text;
};

/**
 * @brief Completions at a cursor: locals in scope first, then the file's vars, referred names, clojure.core
 *        and special forms; after `alias/`, that namespace's public vars; after `:`, the keywords the file uses;
 *        after `(:require [`, namespace names.
 */
std::vector<CljLspCompletionItem> CljLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const std::string &path = "", const CljProject *project = nullptr);

// --- Hover ---------------------------------------------------------------------

struct CljLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    CljRange range;
};

/** @brief What the symbol under the cursor is: its arglists and docstring, its namespace, or the local it names. */
CljLspHoverInfo CljLspHover(const std::vector<std::string> &lines, int line, int col, const std::string &path = "",
                            const CljProject *project = nullptr);

// --- Signature help --------------------------------------------------------------

struct CljLspSignature {
    std::string label;                // "(assoc map key val & kvs)"
    std::vector<std::string> params;  // substrings of `label`
};

struct CljLspSignatureHelp {
    bool found = false;
    std::vector<CljLspSignature> signatures;  // one per arity
    int active_signature = 0;
    int active_param = -1;
    std::string documentation;
};

/** @brief The arglists of the function the cursor is in a call to, the matching arity and argument active. */
CljLspSignatureHelp CljLspSignatures(const std::vector<std::string> &lines, int line, int col,
                                     const std::string &path = "", const CljProject *project = nullptr);

// --- Symbols, folds ------------------------------------------------------------

enum class CljLspSymbolKind {
    Module = 2,
    Namespace = 3,
    Class = 5,
    Method = 6,
    Field = 8,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    Struct = 23,
};

struct CljLspSymbol {
    std::string name;
    std::string detail;
    CljLspSymbolKind kind = CljLspSymbolKind::Function;
    CljRange range;
    CljRange sel_range;
    int parent = -1;
};

/** @brief The outline: the namespace, then each definition; a protocol's methods and a record's fields under it. */
std::vector<CljLspSymbol> CljLspSymbols(const std::vector<std::string> &lines);

struct CljLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" or "comment"
};

/** @brief Every multi-line form, and runs of comment lines. */
std::vector<CljLspFold> CljLspFolds(const std::vector<std::string> &lines);

// --- Navigation ------------------------------------------------------------------

struct CljLspLocation {
    bool found = false;
    std::string path;  // "" for the file itself
    CljRange range;
};

/** @brief Where the name under the cursor is defined: a local's binding, the file's def, a project var. */
CljLspLocation CljLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                const std::string &path = "", const CljProject *project = nullptr);

struct CljLspReference {
    CljRange range;
    bool is_definition = false;
};

/** @brief Every occurrence in the file of the local or var under the cursor (a local: in its scope). */
std::vector<CljLspReference> CljLspReferences(const std::vector<std::string> &lines, int line, int col,
                                              const std::string &path = "");

struct CljLspRenameTarget {
    bool found = false;
    CljRange range;
    std::string placeholder;
};

/** @brief The name prepareRename offers: a local, or one of the file's own vars; not core, not a keyword. */
CljLspRenameTarget CljLspPrepareRename(const std::vector<std::string> &lines, int line, int col,
                                       const std::string &path = "");

struct CljLspTextEdit {
    CljRange range;
    std::string new_text;
};

/** @brief The edits renaming the local or var under the cursor in this file. */
std::vector<CljLspTextEdit> CljLspRename(const std::vector<std::string> &lines, int line, int col,
                                         const std::string &new_name, std::string *error, const std::string &path = "");

// --- Vocabulary (exposed for tests) ------------------------------------------------

struct CljLspVar {
    std::string ns, name, kind, arglists, doc, added, deprecated;
    bool is_private = false;
};

/** @brief A var (public or private) of clojure.core or another shipped namespace, or a special form (ns "");
 *         nullptr if unknown. */
const CljLspVar *CljLspCoreVar(const std::string &ns, const std::string &name);

/** @brief Every public var of a shipped namespace (empty for one this server does not know). */
std::vector<const CljLspVar *> CljLspNamespaceVars(const std::string &ns);

#endif  // MEP_CLOJURE_LSP_H
