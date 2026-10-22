#ifndef MEP_BASH_LSP_H
#define MEP_BASH_LSP_H

#include <string>
#include <vector>

#include "bash_parse.h"

// The analysis half of mep's own Bash language server (the wire half is
// bash_lsp_server.cpp, the `mep-bash-lsp` binary; kBuiltinLsp's `bash_ls`
// registry entry in main.cpp is what spawns it, and the same binary is a
// command-line linter with `--lint`).
//
// Entirely in-house: no shell is started and nothing is run. Everything
// here is a pure function over the document's lines, read by bash_parse.h
// -- whose syntax errors agree with `bash -n` -- so the whole server is
// testable without a client and without bash installed
// (bash_lsp_test.cpp drives all of it directly).
//
// Scope, named rather than silently omitted:
//   - One file at a time. A script that `source`s another, or is meant to
//     be sourced, shares variables and functions this server cannot see;
//     the name checks (unused and undefined variables) calibrate themselves
//     to that, below, rather than reporting unconditionally.
//   - Variables are scoped the way a reader thinks of them, not the way
//     bash resolves them at run time (dynamic scoping): a name declared
//     `local` (or with declare/typeset) inside a function belongs to that
//     function; every other name is global.
//   - Aliases are not expanded, and `eval`'d strings are not read.
//   - Formatting is mep's Bash formatter (bash_format.h, shfmt's layout),
//     the one `gf` runs; here-document bodies and the inside of strings are
//     never touched.
//
// Positions are 0-based lines and 0-based byte columns (`positionEncoding:
// "utf-8"`), as everywhere in mep's own servers.

// --- Diagnostics --------------------------------------------------------

enum class BashLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct BashLspDiagnostic {
    BashRange range;
    BashLspSeverity severity = BashLspSeverity::Error;
    // Stable machine-readable id ("syntax-error", "unused-variable", ...),
    // the LSP Diagnostic.code and what `--lint` prints. Tests assert on
    // these, never on the message wording.
    std::string code;
    std::string message;
};

/**
 * @brief Lints a Bash script: bash's own syntax errors, then the checks listed in help/bash-lsp.org.
 * @param lines the script, one entry per line (no trailing newlines)
 * @return every diagnostic, in source order
 */
std::vector<BashLspDiagnostic> BashLspDiagnostics(const std::vector<std::string> &lines);

/** @brief `path:line:col: severity: message [code]`, 1-based, as `mep-bash-lsp --lint` prints it. */
std::string BashLspFormatDiagnostic(const std::string &path, const BashLspDiagnostic &d);

/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool BashLspDiagnosticFails(const BashLspDiagnostic &d);

// --- Completion ---------------------------------------------------------

enum class BashLspKind {
    Text = 1,
    Function = 3,
    Variable = 6,
    Module = 9,
    Keyword = 14,
    File = 17,
    Constant = 21,
    Operator = 24,
};

struct BashLspCompletionItem {
    std::string label;
    std::string insert_text;  // single-line
    BashLspKind kind = BashLspKind::Text;
    std::string detail;
    std::string documentation;
    // Byte columns on the request's line: the word being completed (after
    // a `$` or `${`, the name part only).
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Completions for a cursor position: variables after `$`/`${`, commands in command position (keywords,
 *        builtins, the file's functions, executables on PATH), a builtin's options after `-`, test operators in
 *        `[[ ]]`, and option names after `set -o` and `shopt`.
 * @param lines the script
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param path_dirs directories to look for executables in (the server passes $PATH's); empty to skip
 */
std::vector<BashLspCompletionItem> BashLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                      const std::vector<std::string> &path_dirs = {});

// --- Hover --------------------------------------------------------------

struct BashLspHoverInfo {
    bool found = false;
    std::string text;
    BashRange range;
};

/**
 * @brief Explains what is under the cursor: a builtin or keyword, a test operator, a shell variable, a
 *        parameter-expansion operator, or one of the file's functions and variables.
 */
BashLspHoverInfo BashLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Signature help -----------------------------------------------------

struct BashLspSignature {
    bool found = false;
    std::string label;  // "read [-ers] [-a array] ... [name ...]", or "greet name [greeting]"
    std::string documentation;
    std::vector<std::string> params;  // substrings of `label`
    int active_param = -1;
};

/**
 * @brief The synopsis of the command the cursor is in: a builtin's, or a file function's, its positional
 *        parameters named from `local name=$1` and the like; the active one is the argument being typed.
 */
BashLspSignature BashLspSignatureHelp(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols ---------------------------------------------------

enum class BashLspSymbolKind {
    Function = 12,
    Variable = 13,
    Constant = 14,  // readonly / declare -r
};

struct BashLspSymbol {
    std::string name;
    std::string detail;
    BashLspSymbolKind kind = BashLspSymbolKind::Function;
    BashRange range;
    BashRange sel_range;
    int parent = -1;
};

/** @brief The outline: functions (nested ones under their parent), and the global variables at top level. */
std::vector<BashLspSymbol> BashLspSymbols(const std::vector<std::string> &lines);

// --- Folding ------------------------------------------------------------

struct BashLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" or "comment"
};

/** @brief Folds for multi-line compound commands, functions, here-documents, arrays and comment runs. */
std::vector<BashLspFold> BashLspFolds(const std::vector<std::string> &lines);

// --- Definition / references / rename -----------------------------------

struct BashLspLocation {
    bool found = false;
    BashRange range;
};

/** @brief Where the variable or function under the cursor is defined (its first assignment, in its scope). */
BashLspLocation BashLspDefinition(const std::vector<std::string> &lines, int line, int col);

struct BashLspReference {
    BashRange range;
    bool is_write = false;
    bool is_definition = false;
};

/** @brief Every occurrence of the variable or function under the cursor, in its scope, in source order. */
std::vector<BashLspReference> BashLspReferences(const std::vector<std::string> &lines, int line, int col);

struct BashLspRenameTarget {
    bool found = false;
    BashRange range;
    std::string placeholder;
};

/** @brief textDocument/prepareRename: the file's own variables and functions; not specials, builtins or env. */
BashLspRenameTarget BashLspPrepareRename(const std::vector<std::string> &lines, int line, int col);

struct BashLspTextEdit {
    BashRange range;
    std::string new_text;
};

/**
 * @brief The edits renaming the variable or function under the cursor everywhere in its scope.
 * @param error why nothing can be renamed (an invalid new name, a name that is not the file's own)
 */
std::vector<BashLspTextEdit> BashLspRename(const std::vector<std::string> &lines, int line, int col,
                                           const std::string &new_name, std::string *error);

// --- Formatting ---------------------------------------------------------

/**
 * @brief Formats the script with mep's Bash formatter (bash_format.h): one edit replacing the changed lines,
 *        none when it is already formatted or cannot be (a syntax error).
 * @param default_indent the indent unit for a script with no indented code to take one from ("" for tabs)
 */
std::vector<BashLspTextEdit> BashLspFormat(const std::vector<std::string> &lines, const std::string &default_indent = "");

// --- Vocabulary (bash_lsp_vocab.cpp; exposed for tests) -----------------

struct BashLspVocabEntry {
    const char *name;
    const char *synopsis;  // the usage line, "" for a keyword or variable
    const char *doc;       // in this server's own words
};

/** @brief The shell builtins, with their usage lines. */
const std::vector<BashLspVocabEntry> &BashLspBuiltinVocab();
/** @brief The reserved words. */
const std::vector<BashLspVocabEntry> &BashLspKeywordVocab();
/** @brief The variables bash sets or reads, and the special parameters ($?, $@, ...). */
const std::vector<BashLspVocabEntry> &BashLspVariableVocab();
/** @brief The conditional-expression operators (-f, -eq, =~, ...). */
const std::vector<BashLspVocabEntry> &BashLspTestOperatorVocab();
/** @brief The parameter-expansion operators (:-, ##, //, ^^, @Q, ...). */
const std::vector<BashLspVocabEntry> &BashLspExpansionVocab();
/** @brief `set -o` option names and `shopt` option names, with what they do. */
const std::vector<BashLspVocabEntry> &BashLspSetOptionVocab();
const std::vector<BashLspVocabEntry> &BashLspShoptVocab();

/** @brief Looks a name up in a vocabulary, or null. */
const BashLspVocabEntry *BashLspFind(const std::vector<BashLspVocabEntry> &vocab, const std::string &name);

// One option of a builtin: `-r` of read, with its argument name ("" for a flag).
struct BashLspOption {
    const char *builtin;
    const char *option;
    const char *arg;
    const char *doc;
};

/** @brief The options of the builtins that have them. */
const std::vector<BashLspOption> &BashLspBuiltinOptions();

#endif  // MEP_BASH_LSP_H
