#ifndef MEP_MAXIMA_LSP_H
#define MEP_MAXIMA_LSP_H

#include <string>
#include <vector>

#include "maxima_ast.h"
#include "maxima_lsp_builtin_names.h"

// The analysis half of mep's own Maxima language server (the wire half is
// maxima_lsp_server.cpp, the `mep-maxima-lsp` binary; kBuiltinLsp's
// `maxima_ls` registry entry in main.cpp is what spawns it).
//
// Entirely in-house: Maxima itself is never started, no package is
// loaded, and no external linter is consulted. Everything here is a pure
// function over plain std::string lines -- parsed by maxima_ast.h, then
// analyzed -- so the whole server is testable without a GL context,
// without a client, without a process and without Maxima installed
// (maxima_lsp_test.cpp drives all of it directly). The one exception is
// the on-disk existence check behind MaximaLspOptions::check_files, which
// is why that is a flag rather than unconditional.
//
// Scope, named rather than silently omitted (the convention org_lsp.h and
// r_lsp.h use):
//   - This is a *source* analyzer. It knows Maxima's grammar, its scope
//     rules and the names its manual documents; it does not evaluate
//     anything, simplify anything, or know what an expression's value is.
//     `x : 1$ x : x + 1$` binds `x` twice and that is all it knows.
//   - A Maxima file can change its own grammar (`infix("~")`) and can
//     define names by computing them (`kill`, `define`, `translate`,
//     `:lisp`). The grammar change is followed exactly -- including
//     through a `load` of a package that ships with Maxima -- and the
//     computed definitions are recognized where they are syntactically
//     visible and never followed.
//   - Because of that, the name checks calibrate themselves to the
//     document rather than reporting unconditionally, for the reason
//     r_lsp.h gives: `undefined-name` switches off entirely for a
//     document that loads a package this server does not ship a name
//     list for, that reads another file, that builds a name at run time
//     (`concat`/`eval_string`/`:lisp`), or that simply contains six or
//     more distinct names it cannot place.
//   - Quoted expressions are respected syntactically: the operand of `'`
//     is a symbol, not a variable use, and nothing inside it is resolved.
//   - `unused-local` is reported for `block` locals and `lambda`
//     parameters only. A top-level `x : 1` in a script is the script's
//     output, not a mistake.
//   - No formatting provider. Maxima's own `grind` is the community's
//     formatter and it round-trips through the simplifier, which an
//     editor cannot do from source text alone.
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16 code
// units -- what mep's own LSP client consumes, and what
// maxima_lsp_server.cpp's `positionEncoding: "utf-8"` declares.

// --- Options ----------------------------------------------------------

struct MaximaLspOptions {
    // Directory the document lives in. A relative `load("foo.mac")` path
    // resolves against this; empty disables that check regardless of
    // check_files.
    std::string doc_dir;
    // Whether to touch the filesystem at all. Off in tests, and off for
    // an unsaved buffer that has no directory to resolve against.
    bool check_files = true;
};

// --- Diagnostics ------------------------------------------------------

// LSP DiagnosticSeverity, spelled out so callers don't have to remember
// which end of the scale is which.
enum class MaximaLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct MaximaLspDiagnostic {
    MxRange range;
    MaximaLspSeverity severity = MaximaLspSeverity::Error;
    // Stable machine-readable id ("undefined-name", "missing-terminator",
    // ...), emitted as the LSP Diagnostic.code. Tests assert on these
    // rather than on message wording, so message text stays free to
    // improve.
    std::string code;
    std::string message;
};

/**
 * @brief Lints a Maxima document: syntax, scope, loop-clause conflicts and the assignment traps the language invites.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param opts filesystem-resolution options (see MaximaLspOptions)
 * @return every diagnostic found, in ascending source order
 */
std::vector<MaximaLspDiagnostic> MaximaLspDiagnostics(const std::vector<std::string> &lines,
                                                      const MaximaLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class MaximaLspKind {
    Text = 1,
    Function = 3,
    Field = 5,
    Variable = 6,
    Module = 9,
    Value = 12,
    Keyword = 14,
    Constant = 21,
    Operator = 24,
};

struct MaximaLspCompletionItem {
    std::string label;        // what the list shows
    std::string insert_text;  // always single-line: mep splices it into one Buffer line
    MaximaLspKind kind = MaximaLspKind::Function;
    std::string detail;         // short right-hand annotation (a signature, a package)
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line, aligned to the Maxima name
    // immediately before the cursor and never wider: mep's client
    // ignores textEdit and replaces exactly that word.
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes Maxima-aware completions for a cursor position (locals, file definitions, builtins, `load` targets).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options
 * @return the candidates for this context, already filtered against the typed prefix
 */
std::vector<MaximaLspCompletionItem> MaximaLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                          const MaximaLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct MaximaLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    MxRange range;     // the range the hover applies to
};

/**
 * @brief Explains the Maxima construct under a cursor position (name, call, keyword, operator, literal).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
MaximaLspHoverInfo MaximaLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols -------------------------------------------------

// LSP SymbolKind, only the handful this server emits.
enum class MaximaLspSymbolKind {
    Function = 12,
    Variable = 13,
    Constant = 14,
    Array = 18,
    Operator = 25,
    Namespace = 3,  // used for `/* --- Section --- */` outline headers
};

struct MaximaLspSymbol {
    std::string name;
    std::string detail;  // a signature, or what kind of value it is
    MaximaLspSymbolKind kind = MaximaLspSymbolKind::Variable;
    MxRange range;      // the whole definition, for the symbol's `range`
    MxRange sel_range;  // the name itself, for `selectionRange`
    int parent = -1;    // index into the returned vector, -1 for top level
};

/**
 * @brief Lists a document's outline: banner comments, function and array definitions, operators and top-level bindings.
 * @param lines the document's text, one entry per line
 * @return the symbols in document order, parents before children
 */
std::vector<MaximaLspSymbol> MaximaLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct MaximaLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region) or "comment"
};

/**
 * @brief Computes folding ranges for multi-line blocks, calls, lists, comments and banner-delimited regions.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<MaximaLspFold> MaximaLspFolds(const std::vector<std::string> &lines);

// --- Definition -------------------------------------------------------

struct MaximaLspLocation {
    bool found = false;
    std::string path;  // absolute path of the target file, or "" meaning this document
    MxRange range;
};

/**
 * @brief Resolves the name under a cursor position to where it is bound, or a `load()` path to that file.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (`doc_dir` anchors a relative `load()` path)
 * @return the resolved location, or a default-constructed value with `found == false`
 */
MaximaLspLocation MaximaLspDefinition(const std::vector<std::string> &lines, int line, int col,
                                      const MaximaLspOptions &opts);

// --- References -------------------------------------------------------

struct MaximaLspReference {
    MxRange range;
    bool is_write = false;       // an assignment to the name, rather than a read
    bool is_definition = false;  // the occurrence that binds the name
};

/**
 * @brief Finds every occurrence of the name under a cursor position that resolves to the same binding.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the occurrences in document order, including the definition; empty when the cursor is not on a name
 */
std::vector<MaximaLspReference> MaximaLspReferences(const std::vector<std::string> &lines, int line, int col);

// --- Signature help ---------------------------------------------------

struct MaximaLspSignature {
    bool found = false;
    std::string label;  // "integrate(expr, x, a, b)"
    std::string documentation;
    std::vector<std::string> params;  // one entry per parameter, as written in the label
    int active_param = -1;            // index into `params`, or -1
    // The other argument lists the same name accepts, most useful first.
    // Maxima overloads by arity heavily (`makelist` takes one through
    // five arguments), so offering only one of them would be misleading.
    std::vector<std::string> alternatives;
};

/**
 * @brief Describes the call the cursor sits inside, and which of its arguments is being typed.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the signature and active argument, or a default-constructed value with `found == false`
 */
MaximaLspSignature MaximaLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Shared vocabulary (exposed for tests, hover and completion) ------

// A name this server can say something about in its own words: a
// keyword, an operator, a constant or one of the functions common enough
// to be worth explaining. The full name list lives in the generated
// maxima_lsp_builtin_names.cpp; this is the part that carries prose.
struct MaximaLspVocabEntry {
    const char *name;
    const char *detail;  // a signature, or a short category
    const char *doc;     // one or two sentences, in this server's own words
};

/** @brief Maxima's reserved words (`if`, `then`, `do`, `thru`, ...). */
const std::vector<MaximaLspVocabEntry> &MaximaLspKeywordVocab();
/** @brief Maxima's operators, keyed by their exact spelling (`:`, `:=`, `#`, `.`, ...). */
const std::vector<MaximaLspVocabEntry> &MaximaLspOperatorVocab();
/** @brief Maxima's constants and system variables (`%pi`, `%e`, `%i`, `true`, `%`, ...). */
const std::vector<MaximaLspVocabEntry> &MaximaLspConstantVocab();
/** @brief The functions common enough to carry a written explanation here. */
const std::vector<MaximaLspVocabEntry> &MaximaLspFunctionVocab();
/** @brief Looks up one explained name across every vocabulary above, or null. */
const MaximaLspVocabEntry *MaximaLspFindVocab(const std::string &name);

/** @brief Reports whether a name is one Maxima's own manual documents. */
bool MaximaLspIsBuiltin(const std::string &name);
/** @brief Looks up a documented name's kind/signature/package, or null when it is not one. */
const MaximaBuiltinDecl *MaximaLspFindBuiltin(const std::string &name);
/** @brief Every documented name, for completion. */
const std::vector<std::string> &MaximaLspBuiltinNameList();

/** @brief Reports whether assigning to this name is an error Maxima will refuse at run time. */
bool MaximaLspIsProtectedName(const std::string &name);

// --- Internals shared with the test suite ------------------------------

/**
 * @brief Splits a signature's parameter list ("integrate(expr, x, a, b)") into one entry per parameter.
 * @param signature the signature text
 * @return each parameter as written, in order; empty when the signature has no parentheses
 */
std::vector<std::string> MaximaLspSplitParams(const std::string &signature);

/**
 * @brief Splits a multi-form signature ("f(a) | f(a, b)") into its alternatives.
 * @param signature the `MaximaBuiltinDecl::signature` text
 * @return one entry per argument list, in the order the manual gives them
 */
std::vector<std::string> MaximaLspSplitSignatures(const std::string &signature);

#endif  // MEP_MAXIMA_LSP_H
