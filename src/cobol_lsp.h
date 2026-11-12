#ifndef MEP_COBOL_LSP_H
#define MEP_COBOL_LSP_H

// The analysis half of mep's own COBOL language server (the wire half is
// cobol_lsp_server.cpp, the `mep-cobol-lsp` binary; the `cobol_ls` entry
// in main.cpp's kBuiltinLsp spawns it). Everything here is a function of
// the document text and the copybooks it COPYs, so the whole server is
// testable without a client (cobol_lsp_test.cpp). Positions are 0-based
// lines and 0-based byte columns (`positionEncoding: "utf-8"`).
//
// What it reads: the DATA DIVISION entries (levels 01-49, 66, 77, 78, 88,
// FD/SD), the PROCEDURE DIVISION's paragraphs and sections, and every
// `COPY name` / `COPY "file.cpy"` -- resolved against the document's
// directory, then `$COBCPY` (':'-separated), then the `includeDirs`
// setting (cobc's `-I`: mep fills it from the Run button's Setup flags),
// then `$COB_COPY_DIR`, trying the name as written, upper and lower case,
// with no extension and with .cpy/.CPY/.cbl/.CBL/.cob/.COB. Copybooks
// COPY others; those are followed too (a copybook's own directory first).
// A parsed copybook -- and its name index -- is cached by path, size and
// modification time, so a 2.7 MB generated copybook is read once.
//
// Doc comments (rosetta's convention for the bindings it generates): a
// block of consecutive lines whose comment text starts with `*>>`
// documents the next data description entry (plain `*>` comments in
// between are skipped). On an 01 record the block's head is the C++
// signature and one or more `CALL "dm_x" USING ...` lines naming the
// programs that take the record (`_get`/`_set`, `_base`, a container's
// `_new`/`_size`/..., a table's `_from_table`/...), so hover / definition /
// completion work on every CALL literal as well as on the record and its
// fields; after a blank line comes the description (markdown). A record
// in a callbacks copybook has `PROCEDURE DIVISION USING ...` instead. An
// undocumented 78 right after a documented one shares its block (rosetta
// documents an enum once, above its first constant). Fixed format (`*>>`
// in columns 7-9) and free format (`*>>` anywhere, after blanks) are both
// read; see CobolLspDocBlock for the wrapping.

#include <memory>
#include <string>
#include <vector>

struct CobolLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

class CobolLspDocument;

/**
 * @brief Parses and analyses a document; every query below runs on the result.
 * @param lines the document text
 * @param path its filesystem path ("" when unknown: COPY then resolves through the include dirs only)
 * @param include_dirs extra copybook directories (the `includeDirs` setting), searched after the document's own
 */
std::shared_ptr<const CobolLspDocument> CobolLspAnalyze(const std::vector<std::string> &lines,
                                                        const std::string &path = std::string(),
                                                        const std::vector<std::string> &include_dirs = {});

/** @brief The directories COPY searches for a document, in order (exposed for tests). */
std::vector<std::string> CobolLspCopybookDirs(const std::string &doc_path, const std::vector<std::string> &include_dirs);
/** @brief The file `COPY name` finds in `dirs`, or "" (exposed for tests). */
std::string CobolLspResolveCopy(const std::string &name, const std::vector<std::string> &dirs);

// --- Doc blocks -----------------------------------------------------------------------

/** @brief One `CALL "x" USING ...` line of a doc block (wrapped continuation lines joined). */
struct CobolLspDocCall {
    std::string line;  // `CALL "dm_x" USING DM-X DM-STATUS (what it does)`
    std::string name;  // `dm_x`
};

/**
 * @brief A `*>>` doc block split the way rosetta writes one.
 *
 * Rosetta wraps long doc lines onto continuation lines indented two more
 * spaces (`*>>   rest`); those are joined back onto the line they continue,
 * except inside a ``` code fence, where indentation is the code's own.
 * The block's head (up to its first blank line) holds the C++ signature,
 * then the record's CALL lines (several on field, director, container and
 * table records) or, in a callbacks copybook, the `PROCEDURE DIVISION
 * USING` line of the program C++ calls. A head without either is not a
 * head: the whole block is body (field docs, enum blocks).
 */
struct CobolLspDocBlock {
    std::vector<std::string> signature;  // the head's lines before the CALL lines (the C++ signature, or a summary)
    std::vector<CobolLspDocCall> calls;  // every CALL line, in order
    std::string call_line;               // calls[0].line, "" when there is none
    std::string call_name;               // calls[0].name
    std::string using_line;              // `PROCEDURE DIVISION USING ...` (callbacks copybook), "" for none
    std::vector<std::string> body;       // the rest, unwrapped, blank lines at either end dropped
};

/** @brief Joins rosetta's wrapped `*>>` continuation lines (two extra spaces) outside ``` fences. */
std::vector<std::string> CobolLspUnwrapDoc(const std::vector<std::string> &doc);

/** @brief Splits the text of a doc block (one entry per `*>>` line, marker stripped). */
CobolLspDocBlock CobolLspSplitDoc(const std::vector<std::string> &doc);

/** @brief One indexed name, as a test sees it. */
struct CobolLspItemInfo {
    std::string name;
    int level = 0;  // 1-49, 66, 77, 78, 88; 0 for a paragraph, -1 a section, -2 an FD/SD
    std::string path;  // "" for the document itself
    int line = 0;
    std::vector<std::string> doc;
    std::string call_name;             // the first CALL line's program
    std::vector<std::string> calls;    // every CALL line's program
    std::string parent;  // the enclosing item's name
};

/** @brief Every item the document and its copybooks define, own items first (exposed for tests). */
std::vector<CobolLspItemInfo> CobolLspItems(const CobolLspDocument &doc);

// --- Diagnostics ------------------------------------------------------------------

enum class CobolLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct CobolLspDiagnostic {
    CobolLspRange range;
    CobolLspSeverity severity = CobolLspSeverity::Error;
    std::string code;  // stable id: copy-not-found, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<CobolLspDiagnostic> CobolLspDiagnostics(const CobolLspDocument &doc);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string CobolLspFormatDiagnostic(const std::string &path, const CobolLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool CobolLspDiagnosticFails(const CobolLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class CobolLspKind {
    Text = 1, Method = 2, Function = 3, Field = 5, Variable = 6, Class = 7, Module = 9, Property = 10,
    Value = 12, Keyword = 14, Snippet = 15, File = 17, Constant = 21, Struct = 22,
};

struct CobolLspCompletionItem {
    std::string label;
    std::string insert_text;
    CobolLspKind kind = CobolLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<CobolLspCompletionItem> CobolLspCompletions(const CobolLspDocument &doc, int line, int col);

struct CobolLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    CobolLspRange range;
};

CobolLspHoverInfo CobolLspHover(const CobolLspDocument &doc, int line, int col);

struct CobolLspSignatureHelp {
    bool found = false;
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::string documentation;
    int active_param = 0;
};

/** @brief Inside `CALL "x" USING ...`: the CALL line of x's record, its USING operands as parameters. */
CobolLspSignatureHelp CobolLspSignatureAt(const CobolLspDocument &doc, int line, int col);

// --- Navigation -------------------------------------------------------------------------

struct CobolLspLocation {
    std::string path;  // "" for the document itself
    CobolLspRange range;
};

std::vector<CobolLspLocation> CobolLspDefinition(const CobolLspDocument &doc, int line, int col);

struct CobolLspReference {
    CobolLspRange range;
    bool is_definition = false;
};

/** @brief The word's occurrences in the document (COBOL names compare case-insensitively). */
std::vector<CobolLspReference> CobolLspReferences(const CobolLspDocument &doc, int line, int col);

struct CobolLspTextEdit {
    CobolLspRange range;
    std::string new_text;
};

struct CobolLspRenameTarget {
    bool found = false;
    CobolLspRange range;
    std::string placeholder;
};

CobolLspRenameTarget CobolLspPrepareRename(const CobolLspDocument &doc, int line, int col);
std::vector<CobolLspTextEdit> CobolLspRename(const CobolLspDocument &doc, int line, int col,
                                             const std::string &new_name, std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class CobolLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Class = 5, Method = 6, Property = 7, Field = 8, Function = 12,
    Variable = 13, Constant = 14, String = 15, Number = 16, Boolean = 17, Array = 18, Object = 19, Key = 20,
    Null = 21, Struct = 23,
};

struct CobolLspSymbol {
    std::string name;
    std::string detail;
    CobolLspSymbolKind kind = CobolLspSymbolKind::Variable;
    CobolLspRange range;
    CobolLspRange sel_range;
    int parent = -1;
};

std::vector<CobolLspSymbol> CobolLspSymbols(const CobolLspDocument &doc);

struct CobolLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

std::vector<CobolLspFold> CobolLspFolds(const CobolLspDocument &doc);

#endif  // MEP_COBOL_LSP_H
