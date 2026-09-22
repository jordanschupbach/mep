#ifndef MEP_PYTHON_LSP_H
#define MEP_PYTHON_LSP_H

#include <string>
#include <vector>

// The analysis half of mep's own Python language server (the wire half is
// python_lsp_server.cpp, the `mep-python-lsp` binary; kBuiltinLsp's
// `python_ls` registry entry in main.cpp is what spawns it). The syntax
// tree everything here answers from is python_ast.h's.
//
// Deliberately raylib-free and I/O-light, for the same reason org_lsp.h
// is: everything is a pure function over plain std::string lines, so it
// is testable without a GL context, without a language-server client, and
// without a process (python_lsp_test.cpp drives all of it directly). The
// one exception is the filesystem lookup behind PythonLspOptions::
// check_files -- resolving `import sibling` to `sibling.py` next to the
// document, and listing a directory for import completions -- which is
// why that is a flag rather than unconditional.
//
// Scope, named rather than silently omitted (the same convention
// org_lsp.h uses):
//   - One file is the whole world. Nothing here reads another module's
//     source, so an imported name is known to exist and nothing more:
//     `os.pathh` is reported only because os is in this server's own
//     curated stdlib table, never because anything was imported and
//     introspected. That table (PythonLspModuleMembers) covers the
//     standard library's most-used modules and is data, not discovery.
//   - No type inference worth the name. A variable's type is tracked
//     only when it is obvious from the assignment itself -- a literal, a
//     call to a class defined in this file, or a call to a known builtin
//     constructor -- and only to drive attribute completion and hover.
//     Nothing infers through function returns, containers or reassignment.
//   - Diagnostics are the ones a reader could confirm from the file
//     alone: syntax errors, scope errors ("this name is never defined in
//     any enclosing scope"), binding hygiene (unused import, unused
//     local, redefinition), and a small set of idiom checks (mutable
//     default argument, `is` against a literal, bare `except:`). There is
//     no type checking, and there never will be one here: a checker that
//     cannot see the imported modules would be wrong far more often than
//     it was right.
//   - `from x import *` switches the undefined-name check off for the
//     whole file rather than guessing what the star brought in. Same for
//     a module that calls `globals().update(...)` or defines `__getattr__`
//     at module level.
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16 code
// units -- see python_ast.h, and org_lsp.h for why mep's client wants it
// that way.

// --- Options ----------------------------------------------------------

struct PythonLspOptions {
    // Directory the document lives in. `import sibling` and
    // `from .mod import x` resolve against it; empty disables both
    // regardless of `check_files`.
    std::string doc_dir;
    // The document's own base name ("server.py"). `__init__.py` gets one
    // special rule: its imports are re-exports, so they are never
    // reported as unused.
    std::string file_name;
    // Whether to touch the filesystem at all (resolving an import to a
    // file, listing a directory for import completions). Off in tests,
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
enum class PythonLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct PythonLspDiagnostic {
    int line = 0;       // 0-based
    int col_start = 0;  // 0-based byte column, half-open [col_start, col_end)
    int col_end = 0;
    PythonLspSeverity severity = PythonLspSeverity::Error;
    // Stable machine-readable id ("undefined-name", "unused-import", ...),
    // emitted as the LSP Diagnostic.code. Tests assert on these rather
    // than on message wording, so message text stays free to improve.
    std::string code;
    std::string message;
};

/**
 * @brief Analyzes a Python document, reporting syntax, scope and idiom problems.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param opts filesystem-resolution and style options (see PythonLspOptions)
 * @return every diagnostic found, in ascending (line, col_start) order
 */
std::vector<PythonLspDiagnostic> PythonLspDiagnostics(const std::vector<std::string> &lines,
                                                      const PythonLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class PythonLspKind {
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
    Keyword = 14,
    File = 17,
    Folder = 19,
    Constant = 21,
    TypeParameter = 25,
};

struct PythonLspCompletionItem {
    // What the list shows.
    std::string label;
    // What replacing [replace_start, replace_end) with this produces.
    // Always single-line, for the same reason org_lsp.h gives: mep's
    // Editor::AcceptCompletion splices the text into one Buffer line, so
    // an embedded '\n' would corrupt the line model.
    std::string insert_text;
    PythonLspKind kind = PythonLspKind::Text;
    std::string detail;         // short right-hand annotation (a signature, a type)
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line, aligned to the identifier
    // immediately before the cursor (mep's client replaces exactly that
    // word -- see Editor::UpdateCompletionPopup).
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes Python-aware completions for a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (directory listing for import completions)
 * @return the candidates for this context, already filtered against the typed prefix; empty inside strings and comments
 */
std::vector<PythonLspCompletionItem> PythonLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                          const PythonLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct PythonLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    int line = 0;      // range the hover applies to
    int col_start = 0;
    int col_end = 0;
};

/**
 * @brief Explains the name, attribute, keyword or literal under a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
PythonLspHoverInfo PythonLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols -------------------------------------------------

struct PythonLspSymbol {
    std::string name;
    std::string detail;  // the signature, base classes, or inferred type
    int kind = 13;       // LSP SymbolKind (Class 5, Method 6, Field 8, Function 12, Variable 13, Constant 14)
    int line_start = 0;  // whole definition, for the symbol's `range`
    int line_end = 0;
    int sel_col_start = 0;  // the name itself, for `selectionRange`
    int sel_col_end = 0;
    int sel_line = 0;
    int parent = -1;  // index into the returned vector, -1 for top level
};

/**
 * @brief Lists every class, function, method and module- or class-level binding, in document order with parent links.
 * @param lines the document's text, one entry per line
 * @return one symbol per definition; empty for a document with none
 */
std::vector<PythonLspSymbol> PythonLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct PythonLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region), "comment" or "imports"
};

/**
 * @brief Computes folding ranges for suites, comment runs, the import block and multi-line strings.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<PythonLspFold> PythonLspFolds(const std::vector<std::string> &lines);

// --- Definition -------------------------------------------------------

struct PythonLspLocation {
    bool found = false;
    // Absolute path of the target file, or "" meaning "this document".
    std::string path;
    int line = 0;
    int col = 0;
};

/**
 * @brief Resolves the name under a cursor position to where it is bound (in this file, or an imported sibling module).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (`doc_dir` anchors a sibling module lookup)
 * @return the resolved location, or a default-constructed value with `found == false`
 */
PythonLspLocation PythonLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                      const PythonLspOptions &opts);

// --- References, highlight and rename ---------------------------------

struct PythonLspReference {
    int line = 0;
    int col_start = 0;
    int col_end = 0;
    bool is_write = false;  // a binding occurrence, for DocumentHighlightKind.Write
};

struct PythonLspReferenceSet {
    bool found = false;
    std::string name;
    // Why a rename must be refused, or "" when it may proceed (a builtin,
    // a keyword, an attribute whose owner is unknown).
    std::string rename_blocked_reason;
    std::vector<PythonLspReference> refs;  // ascending (line, col_start), the definition included
};

/**
 * @brief Finds every occurrence of the binding under a cursor position, within the scope that binding lives in.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the occurrences (definition first in document order), or `found == false` when the cursor is not on a name
 */
PythonLspReferenceSet PythonLspReferences(const std::vector<std::string> &lines, int line, int col);

// --- Signature help ---------------------------------------------------

struct PythonLspSignature {
    bool found = false;
    std::string label;          // "connect(host, port=8080, *, timeout=None)"
    std::string documentation;  // the callee's docstring summary, when there is one
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
PythonLspSignature PythonLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Shared vocabulary (exposed for tests, hover and completion) ------

// One entry of the server's built-in Python vocabulary: a keyword,
// builtin, exception, dunder method, module or module member, with the
// short annotation a completion list shows (`detail`, a signature where
// there is one) and the sentence hover shows (`doc`).
struct PythonLspVocabEntry {
    const char *name;
    const char *detail;
    const char *doc;
};

/** @brief The reserved-keyword vocabulary. */
const std::vector<PythonLspVocabEntry> &PythonLspKeywordVocab();
/** @brief The builtin function/type/constant vocabulary (`len`, `dict`, `None`, ...). */
const std::vector<PythonLspVocabEntry> &PythonLspBuiltinVocab();
/** @brief The builtin exception vocabulary, offered after `raise` and `except`. */
const std::vector<PythonLspVocabEntry> &PythonLspExceptionVocab();
/** @brief The dunder-method vocabulary, offered after `def` inside a class body. */
const std::vector<PythonLspVocabEntry> &PythonLspDunderVocab();
/** @brief The standard-library module vocabulary, offered after `import`. */
const std::vector<PythonLspVocabEntry> &PythonLspModuleVocab();
/** @brief A known module's members, or nullptr when this server has no table for it. */
const std::vector<PythonLspVocabEntry> *PythonLspModuleMembers(const std::string &module);
/** @brief A builtin type's members ("str", "list", "dict", ...), or nullptr when there is no table for it. */
const std::vector<PythonLspVocabEntry> *PythonLspTypeMembers(const std::string &type);
/** @brief Reports whether a name is a builtin (function, type, constant or exception). */
bool PythonLspIsBuiltin(const std::string &name);

#endif  // MEP_PYTHON_LSP_H
