#ifndef MEP_R_LSP_H
#define MEP_R_LSP_H

#include <string>
#include <vector>

#include "r_lsp_parse.h"

// The analysis half of mep's own R language server (the wire half is
// r_lsp_server.cpp, the `mep-r-lsp` binary; kBuiltinLsp's `r_ls` registry
// entry in main.cpp is what spawns it).
//
// Entirely in-house: R itself is never started, `languageserver` is never
// loaded, and no external linter is consulted. Everything here is a pure
// function over plain std::string lines -- parsed by r_lsp_parse.h, then
// analyzed -- so the whole server is testable without a GL context,
// without a client, without a process and without R installed
// (r_lsp_test.cpp drives all of it directly). The one exception is the
// on-disk existence check behind RLspOptions::check_files, which is why
// that is a flag rather than unconditional.
//
// Scope, named rather than silently omitted (the same convention
// org_lsp.h uses):
//   - This is a *source* analyzer. It knows R's grammar, its scope rules
//     and the signatures of the base packages; it does not evaluate
//     anything, has no type inference, and cannot see inside an installed
//     third-party package.
//   - Because of that, several checks calibrate themselves to the
//     document rather than reporting unconditionally. Reporting every
//     ggplot2 verb as undefined is exactly the false-positive storm that
//     makes people turn a linter off, so:
//       * `undefined-variable`/`undefined-function` switch off entirely
//         for a document that attaches something the server cannot see
//         (a `library()` of a non-base package, `source()`,
//         `Rcpp::sourceCpp()`, `attach()`, `load()`, a `get()`/`eval()`
//         of a computed name) -- and also for one that simply contains
//         six or more distinct names it cannot place, which is what a
//         shiny `ui.R` or a `tinytest` file looks like from here.
//       * `equals-assignment` fires only where the document's own
//         assignments are mostly `<-`, and `trailing-semicolon` only
//         where semicolons are not the house style. Written that way
//         throughout, both are someone's deliberate choice.
//   - Non-standard evaluation is respected syntactically, not
//     semantically: the arguments of `quote`, `substitute`, `with`,
//     `subset`, `aes` and friends, and both sides of a `~` formula, are
//     never resolved as variable uses. They are not followed, either.
//   - `unused-variable` is reported inside function bodies only. A
//     top-level `x <- 1` in a script is the script's output, not a
//     mistake.
//   - No formatting provider. R's community formatters (styler, air)
//     disagree on enough that shipping a third opinion inside an editor
//     would be a liability rather than a feature.
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16 code
// units -- what mep's own LSP client consumes, and what
// r_lsp_server.cpp's `positionEncoding: "utf-8"` declares.

// --- Options ----------------------------------------------------------

struct RLspOptions {
    // Directory the document lives in. Relative `source()` paths resolve
    // against this; empty disables that check regardless of check_files.
    std::string doc_dir;
    // Whether to touch the filesystem at all. Off in tests, and off for
    // an unsaved buffer that has no directory to resolve against.
    bool check_files = true;
};

// --- Diagnostics ------------------------------------------------------

// LSP DiagnosticSeverity, spelled out so callers don't have to remember
// which end of the scale is which.
enum class RLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct RLspDiagnostic {
    RRange range;
    RLspSeverity severity = RLspSeverity::Error;
    // Stable machine-readable id ("undefined-variable", "na-comparison",
    // ...), emitted as the LSP Diagnostic.code. Tests assert on these
    // rather than on message wording, so message text stays free to
    // improve.
    std::string code;
    std::string message;
};

/**
 * @brief Lints an R document: syntax, scope, argument matching, common correctness traps and roxygen consistency.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param opts filesystem-resolution options (see RLspOptions)
 * @return every diagnostic found, in ascending source order
 */
std::vector<RLspDiagnostic> RLspDiagnostics(const std::vector<std::string> &lines, const RLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class RLspKind {
    Text = 1,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Module = 9,
    Value = 12,
    Keyword = 14,
    Constant = 21,
};

struct RLspCompletionItem {
    // What the list shows.
    std::string label;
    // What replacing [replace_start, replace_end) with this produces.
    // Always single-line, for the reason org_lsp.h gives: mep's
    // Editor::AcceptCompletion splices the text into one Buffer line, so
    // an embedded '\n' would corrupt the line model.
    std::string insert_text;
    RLspKind kind = RLspKind::Function;
    std::string detail;         // short right-hand annotation (a signature, a package)
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line. Aligned to the R name
    // immediately before the cursor and never wider: mep's client
    // ignores textEdit and replaces exactly that word.
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes R-aware completions for a cursor position (locals, base functions, argument names, `pkg::`, roxygen tags).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options
 * @return the candidates for this context, already filtered against the typed prefix; empty where R offers nothing
 */
std::vector<RLspCompletionItem> RLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                const RLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct RLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    RRange range;      // the range the hover applies to
};

/**
 * @brief Explains the R construct under a cursor position (name, call, keyword, operator, literal).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
RLspHoverInfo RLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols -------------------------------------------------

// LSP SymbolKind, only the handful this server emits.
enum class RLspSymbolKind {
    Function = 12,
    Variable = 13,
    Constant = 14,
    Class = 5,
    Method = 6,
    Field = 8,
    Namespace = 3,  // used for `# Section ----` outline headers
};

struct RLspSymbol {
    std::string name;
    std::string detail;  // a signature, or what kind of value it is
    RLspSymbolKind kind = RLspSymbolKind::Variable;
    RRange range;      // the whole definition, for the symbol's `range`
    RRange sel_range;  // the name itself, for `selectionRange`
    int parent = -1;   // index into the returned vector, -1 for top level
};

/**
 * @brief Lists a document's outline: `# Section ----` headers, top-level definitions, S4/R6 classes and nested functions.
 * @param lines the document's text, one entry per line
 * @return the symbols in document order, parents before children
 */
std::vector<RLspSymbol> RLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct RLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region) or "comment"
};

/**
 * @brief Computes folding ranges for braced bodies, multi-line calls, comment runs and `# Section ----` regions.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<RLspFold> RLspFolds(const std::vector<std::string> &lines);

// --- Definition -------------------------------------------------------

struct RLspLocation {
    bool found = false;
    // Absolute path of the target file, or "" meaning "this document".
    std::string path;
    RRange range;
};

/**
 * @brief Resolves the name under a cursor position to where it is bound, or a `source()` path to that file.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (`doc_dir` anchors a relative `source()` path)
 * @return the resolved location, or a default-constructed value with `found == false`
 */
RLspLocation RLspDefinition(const std::vector<std::string> &lines, int line, int col, const RLspOptions &opts);

// --- References -------------------------------------------------------

struct RLspReference {
    RRange range;
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
std::vector<RLspReference> RLspReferences(const std::vector<std::string> &lines, int line, int col);

// --- Signature help ---------------------------------------------------

struct RLspSignature {
    bool found = false;
    std::string label;  // "mean(x, trim = 0, na.rm = FALSE, ...)"
    std::string documentation;
    std::vector<std::string> params;  // one entry per formal, as written in the label
    int active_param = -1;            // index into `params`, or -1
};

/**
 * @brief Describes the call the cursor sits inside, and which of its arguments is being typed.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the signature and active argument, or a default-constructed value with `found == false`
 */
RLspSignature RLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Shared vocabulary (exposed for tests, hover and completion) ------

// One function of the R distribution this server knows about, with the
// formals it takes and a one-line explanation.
struct RLspFunctionEntry {
    const char *name;
    const char *package;  // "base", "stats", "utils", ...
    const char *params;   // formals exactly as R declares them, "" for none
    const char *doc;
};

// A non-function name: a reserved word, a constant, a roxygen tag or an
// operator, with the one-line explanation hover shows.
struct RLspVocabEntry {
    const char *name;
    const char *detail;
    const char *doc;
};

/** @brief Every function the server knows, across the base R packages, grouped by topic. */
const std::vector<RLspFunctionEntry> &RLspFunctionVocab();
/** @brief Looks up one function by name, or null when it is not one this server knows. */
const RLspFunctionEntry *RLspFindFunction(const std::string &name);
/** @brief R's reserved words (`if`, `function`, `repeat`, ...). */
const std::vector<RLspVocabEntry> &RLspKeywordVocab();
/** @brief R's constants (`TRUE`, `NULL`, `NA_character_`, `pi`, `letters`, ...). */
const std::vector<RLspVocabEntry> &RLspConstantVocab();
/** @brief R's operators, keyed by their exact spelling (`<-`, `%in%`, `|>`, ...). */
const std::vector<RLspVocabEntry> &RLspOperatorVocab();
/** @brief The roxygen2 tag vocabulary (names carry no leading `@`). */
const std::vector<RLspVocabEntry> &RLspRoxygenVocab();
/** @brief Package names offered after `library(`, base packages first. */
const std::vector<RLspVocabEntry> &RLspPackageVocab();
/** @brief Reports whether a package's exports are known to this server (the base distribution's). */
bool RLspIsBasePackage(const std::string &name);
/**
 * @brief Reports whether a name is exported by the base R distribution (or is one of its data sets).
 *
 * Backed by the generated table in r_lsp_base_names.cpp, which is the
 * whole export list rather than the documented subset in
 * RLspFunctionVocab(): knowing that `diff` exists is what keeps a real
 * call from being reported as undefined, even where there is no
 * signature to show for it.
 */
bool RLspIsBaseName(const std::string &name);
/** @brief Every name RLspIsBaseName() knows, for completion. */
const std::vector<std::string> &RLspBaseNameList();

// --- Internals shared with the test suite ------------------------------

/**
 * @brief Splits an R formals string ("x, trim = 0, ...") into one entry per formal.
 * @param params the `RLspFunctionEntry::params` text
 * @return each formal exactly as written, in order
 */
std::vector<std::string> RLspSplitParams(const std::string &params);

#endif  // MEP_R_LSP_H
