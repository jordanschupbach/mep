#ifndef MEP_C_LSP_H
#define MEP_C_LSP_H

#include <string>
#include <vector>

// The analysis half of mep's own C language server (the wire half is
// c_lsp_server.cpp, the `mep-c-lsp` binary; kBuiltinLsp's `c_ls` registry
// entry in main.cpp is what spawns it). The syntax tree everything here
// answers from is c_ast.h's.
//
// Deliberately raylib-free and I/O-light, for the same reason org_lsp.h
// and python_lsp.h are: every feature is a pure function over plain
// std::string lines, so it is testable without a GL context, without a
// language-server client and without a process (c_lsp_test.cpp drives
// all of it directly). The one exception is the filesystem lookup behind
// CLspOptions::check_files -- resolving `#include "sibling.h"` to a real
// file so its declarations count as declared -- which is why that is a
// flag rather than unconditional.
//
// Scope, named rather than silently omitted:
//   - No preprocessor run, ever. An editor is looking at unpreprocessed
//     source; a server that needed `-I` flags and a compile_commands.json
//     to say anything would say nothing in most buffers. c_ast.h explains
//     what is done instead (one arm of each `#if` group, chosen by
//     evaluation where the file settles it and by bracket balance where
//     it does not).
//   - No type checking. A checker that cannot see the included headers
//     would be wrong far more often than right, so this server never
//     claims a type mismatch. The type model exists only to answer
//     "what are this struct's members" and "does this function return
//     void", and it is honest about being shallow (see c_ast.h's CType).
//   - Declarations are known from three places and no others: this file,
//     the `#include "..."` files next to it that check_files resolves,
//     and the standard-library vocabulary compiled in
//     (c_lsp_vocab.cpp). Anything a system header this server has no
//     table for might have declared is *unknown*, not undeclared -- and
//     one such `#include` switches the undeclared-name check off for the
//     whole file, because guessing there is exactly how a linter earns
//     its way into someone's disabled list.
//   - Diagnostics are the ones a reader could confirm from the file
//     alone: syntax and preprocessor errors, binding hygiene (an unused
//     local, a dead `static` function, a label nothing jumps to), and
//     the small set of mistakes C makes easy and a compiler stays quiet
//     about (`if (x = 0)`, `a & b == c`, a duplicated `case`, a
//     `printf` whose argument count does not match its format).
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16
// code units -- see c_ast.h, and org_lsp.h for why mep's client wants it
// that way.

// --- Options ----------------------------------------------------------

struct CLspOptions {
    // Directory the document lives in. `#include "sibling.h"` resolves
    // against it (and against its `include/` and `..` neighbours, which
    // is where a project's own headers usually sit); empty disables that
    // lookup regardless of `check_files`.
    std::string doc_dir;
    // The document's own base name ("parser.c"). A `.h` document is
    // treated as a header: a declaration with no definition is normal
    // there, and nothing in it is reported as unused.
    std::string file_name;
    // Whether to touch the filesystem at all (resolving an include to a
    // file, listing a directory for `#include` completions). Off in
    // tests, and off for an unsaved buffer with no directory to resolve
    // against.
    bool check_files = true;
    // Columns past which a line earns a Hint, or 0 for no length check.
    // Not a style opinion this server holds on its own -- it is off
    // unless a caller asks for it.
    int max_line_length = 0;
};

// --- Diagnostics ------------------------------------------------------

// LSP DiagnosticSeverity, spelled out so callers don't have to remember
// which end of the scale is which.
enum class CLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct CLspDiagnostic {
    int line = 0;       // 0-based
    int col_start = 0;  // 0-based byte column, half-open [col_start, col_end)
    int col_end = 0;
    CLspSeverity severity = CLspSeverity::Error;
    // Stable machine-readable id ("unused-variable", "assign-in-if",
    // ...), emitted as the LSP Diagnostic.code. Tests assert on these
    // rather than on message wording, so message text stays free to
    // improve.
    std::string code;
    std::string message;
};

/**
 * @brief Analyzes a C document, reporting syntax, preprocessor, binding and idiom problems.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param opts filesystem-resolution and style options (see CLspOptions)
 * @return every diagnostic found, in ascending (line, col_start) order
 */
std::vector<CLspDiagnostic> CLspDiagnostics(const std::vector<std::string> &lines, const CLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class CLspKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Module = 9,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    File = 17,
    Folder = 19,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    TypeParameter = 25,
};

struct CLspCompletionItem {
    // What the list shows.
    std::string label;
    // What replacing [replace_start, replace_end) with this produces.
    // Always single-line, for the same reason org_lsp.h gives: mep's
    // Editor::AcceptCompletion splices the text into one Buffer line, so
    // an embedded '\n' would corrupt the line model.
    std::string insert_text;
    CLspKind kind = CLspKind::Text;
    std::string detail;         // short right-hand annotation (a signature, a type)
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line, aligned to the identifier
    // immediately before the cursor (mep's client replaces exactly that
    // word -- see Editor::UpdateCompletionPopup).
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes C-aware completions for a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (directory listing for `#include` completions)
 * @return the candidates for this context, already filtered against the typed prefix; empty inside strings and comments
 */
std::vector<CLspCompletionItem> CLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                const CLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct CLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    int line = 0;      // range the hover applies to
    int col_start = 0;
    int col_end = 0;
};

/**
 * @brief Explains the name, member, macro, keyword or header under a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
CLspHoverInfo CLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols -------------------------------------------------

struct CLspSymbol {
    std::string name;
    std::string detail;  // the signature, the underlying type, the macro's parameters
    // LSP SymbolKind: File 1, Module 2, Function 12, Variable 13,
    // Constant 14, Struct 23, Enum 10, EnumMember 22, Field 8,
    // TypeParameter 26 (used here for a typedef).
    int kind = 13;
    int line_start = 0;  // whole definition, for the symbol's `range`
    int line_end = 0;
    int sel_col_start = 0;  // the name itself, for `selectionRange`
    int sel_col_end = 0;
    int sel_line = 0;
    int parent = -1;  // index into the returned vector, -1 for top level
};

/**
 * @brief Lists every function, type, macro and file-scope object, in document order with parent links.
 * @param lines the document's text, one entry per line
 * @return one symbol per definition; empty for a document with none
 */
std::vector<CLspSymbol> CLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct CLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region), "comment" or "imports" (the `#include` block)
};

/**
 * @brief Computes folding ranges for blocks, comment runs, the include block and `#if` groups.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<CLspFold> CLspFolds(const std::vector<std::string> &lines);

// --- Definition -------------------------------------------------------

struct CLspLocation {
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
 * @param opts filesystem-resolution options (`doc_dir` anchors the include lookup)
 * @return the resolved location, or a default-constructed value with `found == false`
 */
CLspLocation CLspDefinition(const std::vector<std::string> &lines, int line, int col, const CLspOptions &opts);

// --- References, highlight and rename ---------------------------------

struct CLspReference {
    int line = 0;
    int col_start = 0;
    int col_end = 0;
    bool is_write = false;  // a declaration or an assignment target, for DocumentHighlightKind.Write
};

struct CLspReferenceSet {
    bool found = false;
    std::string name;
    // Why a rename must be refused, or "" when it may proceed (a
    // keyword, a name from the standard library, a struct member whose
    // owning type this server could not resolve).
    std::string rename_blocked_reason;
    std::vector<CLspReference> refs;  // ascending (line, col_start), the declaration included
};

/**
 * @brief Finds every occurrence of the binding under a cursor position, within the scope that binding lives in.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the occurrences (declaration first in document order), or `found == false` when the cursor is not on a name
 */
CLspReferenceSet CLspReferences(const std::vector<std::string> &lines, int line, int col);

// --- Signature help ---------------------------------------------------

struct CLspSignature {
    bool found = false;
    std::string label;                // "memcpy(void *dest, const void *src, size_t n)"
    std::string documentation;        // the function's one-line explanation, when there is one
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
CLspSignature CLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Shared vocabulary (exposed for tests, hover and completion) ------

// One entry of the server's built-in C vocabulary: a keyword, a standard
// library function, type, macro or constant, with the short annotation a
// completion list shows (`detail`, a signature where there is one) and
// the sentence hover shows (`doc`).
struct CLspVocabEntry {
    const char *name;
    const char *detail;
    const char *doc;
};

/** @brief The reserved-keyword vocabulary. */
const std::vector<CLspVocabEntry> &CLspKeywordVocab();
/** @brief The standard header vocabulary (`stdio.h`, `string.h`, ...), offered after `#include <`. */
const std::vector<CLspVocabEntry> &CLspHeaderVocab();
/** @brief A standard header's declared names, or nullptr when this server has no table for it. */
const std::vector<CLspVocabEntry> *CLspHeaderMembers(const std::string &header);
/** @brief The preprocessor directive vocabulary, offered after a `#`. */
const std::vector<CLspVocabEntry> &CLspDirectiveVocab();
/** @brief Looks a name up across every standard header table, or nullptr when it is not a standard name. */
const CLspVocabEntry *CLspLookupStandardName(const std::string &name);
/** @brief The header a standard name comes from ("stdio.h"), or "" when the name is not a standard one. */
std::string CLspHeaderOf(const std::string &name);
/** @brief Reports whether this server has a declaration table for a standard header. */
bool CLspKnowsHeader(const std::string &header);

#endif  // MEP_C_LSP_H
