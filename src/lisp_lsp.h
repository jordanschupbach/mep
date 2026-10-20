#ifndef MEP_LISP_LSP_H
#define MEP_LISP_LSP_H

#include <string>
#include <vector>

#include "lisp_lsp_cl_names.h"
#include "lisp_reader.h"

// The analysis half of mep's own Common Lisp language server (the wire
// half is lisp_lsp_server.cpp, the `mep-lisp-lsp` binary; kBuiltinLsp's
// `lisp_ls` registry entry in main.cpp is what spawns it, and the same
// binary is a command-line linter with `--lint`).
//
// Entirely in-house: no Lisp image is started, no SLIME/Swank, no
// system is loaded and nothing is evaluated. Everything here is a pure
// function over plain std::string lines -- read by lisp_reader.h, then
// analyzed -- so the whole server is testable without a client, without
// a process and without a Lisp installed (lisp_lsp_test.cpp drives all
// of it directly).
//
// Scope, named rather than silently omitted (the convention
// maxima_lsp.h and r_lsp.h use):
//   - This is a *source* analyzer for one file. It knows the standard's
//     special operators and macros, its scoping rules and the 978
//     symbols of the COMMON-LISP package (generated from SBCL into
//     lisp_lsp_cl_names.cpp); it does not know what other files of the
//     same system define, does not macroexpand anything, and does not
//     infer types.
//   - Macros it does not know are walked as if their arguments were
//     code, so a variable used inside one still counts as used -- but
//     the name checks (undefined-function, argument counts, setq of an
//     unknown variable) are switched off inside them, because a macro
//     can bind anything.
//   - The two whole-file name checks, `undefined-function` and
//     `setq-undefined`, calibrate themselves to the document rather than
//     reporting unconditionally, for the reason maxima_lsp.h gives: they
//     run only when the file's package is known to use nothing but
//     COMMON-LISP (no `in-package` at all, `cl-user`, or a `defpackage`
//     in the same file whose :use list is only :cl), the file defines
//     nothing dynamically (`(setf (fdefinition ...))`, `eval`, `load`,
//     a `defmacro` that expands into a definition, `use-package`), and
//     it contains no more than five distinct names it cannot place.
//     Past that, the file is part of a larger system and the server
//     would be guessing.
//   - `#+`/`#-` code is evaluated against a conservative feature set
//     (SBCL on 64-bit Linux, plus the features every implementation
//     shares). `#+(or)` and `#+nil` code is dead and skipped entirely;
//     code for any other feature is read for its definitions and uses,
//     but no name check runs inside it and its definitions do not count
//     as redefinitions.
//   - Formatting is re-indentation only (lisp_indent.h): standard
//     Lisp indentation applied to leading whitespace, nothing else on a
//     line touched, and nothing inside a string or `#|` comment moved.
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16 code
// units -- what mep's own LSP client consumes, and what
// lisp_lsp_server.cpp's `positionEncoding: "utf-8"` declares.

// --- Diagnostics ------------------------------------------------------

// LSP DiagnosticSeverity, spelled out so callers don't have to remember
// which end of the scale is which.
enum class LispLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct LispLspDiagnostic {
    LispRange range;
    LispLspSeverity severity = LispLspSeverity::Error;
    // Stable machine-readable id ("unused-variable", "unbalanced-paren",
    // ...), emitted as the LSP Diagnostic.code and printed by `--lint`.
    // Tests assert on these rather than on message wording, so message
    // text stays free to improve.
    std::string code;
    std::string message;
};

/**
 * @brief Lints a Common Lisp document: reader errors, scope (unused/ignored variables), call shapes and style.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @return every diagnostic found, in ascending source order
 */
std::vector<LispLspDiagnostic> LispLspDiagnostics(const std::vector<std::string> &lines);

/**
 * @brief Formats one diagnostic the way `mep-lisp-lsp --lint` prints it.
 * @param path the file name to print (as given on the command line, `-` for stdin is printed as `<stdin>`)
 * @param d the diagnostic
 * @return `path:line:col: severity: message [code]`, with 1-based line and column and no trailing newline
 */
std::string LispLspFormatDiagnostic(const std::string &path, const LispLspDiagnostic &d);

/** @brief Reports whether a diagnostic should make `--lint` exit non-zero (an error or a warning). */
bool LispLspDiagnosticFails(const LispLspDiagnostic &d);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class LispLspKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Keyword = 14,
    Constant = 21,
    Struct = 22,
    Operator = 24,
    TypeParameter = 25,
};

struct LispLspCompletionItem {
    std::string label;        // what the list shows
    std::string insert_text;  // always single-line: mep splices it into one Buffer line
    LispLspKind kind = LispLspKind::Function;
    std::string detail;         // short right-hand annotation (a lambda list, a kind)
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line, aligned to the symbol
    // immediately before the cursor (package prefix excluded) and never
    // wider: mep's client ignores textEdit and replaces exactly that word.
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes completions for a cursor position: visible locals, file definitions, keyword arguments, CL symbols.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the candidates for this context, already filtered against the typed prefix, in the case it was typed
 */
std::vector<LispLspCompletionItem> LispLspCompletions(const std::vector<std::string> &lines, int line, int col);

// --- Hover ------------------------------------------------------------

struct LispLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    LispRange range;   // the range the hover applies to
};

/**
 * @brief Explains the symbol under a cursor position: a local, a file definition, a CL symbol, a loop or lambda-list keyword.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
LispLspHoverInfo LispLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols -------------------------------------------------

// LSP SymbolKind, only the handful this server emits.
enum class LispLspSymbolKind {
    Namespace = 3,  // `;;;; Section` banner comments
    Package = 4,    // defpackage, in-package
    Class = 5,      // defclass, define-condition
    Method = 6,     // defmethod
    Field = 8,      // a class or structure slot
    Interface = 11, // defgeneric
    Function = 12,  // defun, defmacro, define-compiler-macro, defsetf, define-modify-macro
    Variable = 13,  // defvar, defparameter, define-symbol-macro
    Constant = 14,  // defconstant
    Struct = 23,    // defstruct
    TypeParameter = 26,  // deftype
};

struct LispLspSymbol {
    std::string name;
    std::string detail;  // the lambda list, the defining operator, a method's specializers
    LispLspSymbolKind kind = LispLspSymbolKind::Function;
    LispRange range;      // the whole definition, for the symbol's `range`
    LispRange sel_range;  // the name itself, for `selectionRange`
    int parent = -1;      // index into the returned vector, -1 for top level
};

/**
 * @brief Lists a document's outline: banner sections, definitions of every standard kind, and class/structure slots.
 * @param lines the document's text, one entry per line
 * @return the symbols in document order, parents before children
 */
std::vector<LispLspSymbol> LispLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct LispLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region) or "comment"
};

/**
 * @brief Computes folding ranges for multi-line forms, block comments and runs of `;` comment lines.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<LispLspFold> LispLspFolds(const std::vector<std::string> &lines);

// --- Definition / references / rename --------------------------------

struct LispLspLocation {
    bool found = false;
    LispRange range;  // always in this document
};

/**
 * @brief Resolves the symbol under a cursor position to its binding: a local, a parameter, a block or tag, a file definition.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the resolved location, or a default-constructed value with `found == false`
 */
LispLspLocation LispLspDefinition(const std::vector<std::string> &lines, int line, int col);

struct LispLspReference {
    LispRange range;  // the symbol-name part only (no package prefix)
    bool is_write = false;       // an assignment (setq/setf) rather than a read
    bool is_definition = false;  // the occurrence that binds the name
};

/**
 * @brief Finds every occurrence of the symbol under a cursor position that refers to the same thing.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the occurrences in document order, including the definition; empty when the cursor is not on a symbol.
 *         A local resolves to its own binding's uses; anything else matches by name, quoted uses included.
 */
std::vector<LispLspReference> LispLspReferences(const std::vector<std::string> &lines, int line, int col);

struct LispLspRenameTarget {
    bool found = false;
    LispRange range;          // the symbol-name part under the cursor
    std::string placeholder;  // its current spelling
};

/**
 * @brief Answers textDocument/prepareRename: only locals and names this file defines can be renamed.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the renameable range, or `found == false` (a CL symbol, a keyword, a name from another package)
 */
LispLspRenameTarget LispLspPrepareRename(const std::vector<std::string> &lines, int line, int col);

struct LispLspTextEdit {
    LispRange range;
    std::string new_text;
};

/**
 * @brief Computes the edits that rename the symbol under a cursor position everywhere it refers to the same thing.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param new_name the replacement spelling; must read back as one plain symbol
 * @param error receives why nothing can be renamed (may be null)
 * @return the edits in document order; empty with `*error` set when the rename is refused
 */
std::vector<LispLspTextEdit> LispLspRename(const std::vector<std::string> &lines, int line, int col,
                                           const std::string &new_name, std::string *error);

// --- Signature help ---------------------------------------------------

struct LispLspSignature {
    bool found = false;
    std::string label;  // "(gethash key hash-table &optional default)"
    std::string documentation;
    std::vector<std::string> params;  // one entry per parameter, each a substring of `label`
    int active_param = -1;            // index into `params`, or -1
};

/**
 * @brief Describes the call the cursor sits inside, and which lambda-list parameter the argument being typed fills.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the signature and active parameter, or a default-constructed value with `found == false`
 */
LispLspSignature LispLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Formatting -------------------------------------------------------

/**
 * @brief Re-indents a document (lisp_indent.h's rules) and returns one whole-line edit per line that changed.
 * @param lines the document's text, one entry per line
 * @return the edits in document order; empty when the document is already indented
 */
std::vector<LispLspTextEdit> LispLspFormat(const std::vector<std::string> &lines);

// --- Shared vocabulary (exposed for tests, hover and completion) ------

// A name this server can say something about in its own words. The full
// COMMON-LISP symbol table lives in the generated lisp_lsp_cl_names.cpp;
// this is the part that carries prose.
struct LispLspVocabEntry {
    const char *name;    // canonical upper-case name, "&KEY" for lambda-list keywords
    const char *detail;  // a usage shape, or a short category
    const char *doc;     // one or two sentences, in this server's own words
};

/** @brief The 25 special operators. */
const std::vector<LispLspVocabEntry> &LispLspSpecialFormVocab();
/** @brief The standard macros common enough to carry a written explanation. */
const std::vector<LispLspVocabEntry> &LispLspMacroVocab();
/** @brief The standard functions common enough to carry a written explanation. */
const std::vector<LispLspVocabEntry> &LispLspFunctionVocab();
/** @brief The lambda-list keywords (`&optional`, `&key`, ...). */
const std::vector<LispLspVocabEntry> &LispLspLambdaKeywordVocab();
/** @brief The declaration identifiers (`ignore`, `special`, `optimize`, ...). */
const std::vector<LispLspVocabEntry> &LispLspDeclarationVocab();
/** @brief LOOP's clause keywords (`for`, `collect`, `being`, ...), which LOOP compares by name. */
const std::vector<LispLspVocabEntry> &LispLspLoopKeywordVocab();
/** @brief Looks up an explained name across every vocabulary above except LOOP's, or null. */
const LispLspVocabEntry *LispLspFindVocab(const std::string &upper_name);
/** @brief Looks up a LOOP clause keyword, or null. */
const LispLspVocabEntry *LispLspFindLoopKeyword(const std::string &upper_name);

/** @brief Looks up an external symbol of COMMON-LISP by canonical name, or null. */
const LispClDecl *LispLspFindCl(const std::string &upper_name);
/** @brief Reports whether a CL symbol's `kinds` include a given role ("function", "macro", "variable", ...). */
bool LispLspClHasKind(const LispClDecl &decl, const char *kind);

// --- Lambda lists (shared with the test suite) -------------------------

// The call shape a lambda list accepts, enough to count a call's
// arguments against.
struct LispLambdaShape {
    int required = 0;
    int optional = 0;
    bool rest = false;  // &rest or &body: any number past the positional ones
    bool key = false;
    bool allow_other_keys = false;
    std::vector<std::string> keys;  // canonical keyword names &key accepts ("TEST", "KEY")
};

/**
 * @brief Reads a lambda list written as text ("key hash-table &optional default") into its call shape.
 * @param text the lambda list without its outer parentheses, as lisp_lsp_cl_names.cpp stores it
 */
LispLambdaShape LispLspShapeOfLambdaList(const std::string &text);

/**
 * @brief Splits a lambda list's text into its top-level elements ("(a b) &key (c 1)" -> "(a b)", "&key", "(c 1)").
 */
std::vector<std::string> LispLspSplitLambdaList(const std::string &text);

#endif  // MEP_LISP_LSP_H
