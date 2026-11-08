#ifndef MEP_GO_LSP_H
#define MEP_GO_LSP_H

// The analysis half of mep's own Go language server (the wire half is
// go_lsp_server.cpp, the `mep-go-lsp` binary; the `go_ls` entry
// in main.cpp's kBuiltinLsp spawns it). Everything here is a pure function
// of the document text, so the whole server is testable without a client
// (go_lsp_test.cpp). Positions are 0-based lines and 0-based byte
// columns (`positionEncoding: "utf-8"`).
//
// The parser is a port of go/parser (go_parse.h): syntax errors are gofmt's
// own, message and position. Names resolve through Go's blocks within the
// one file (go_lsp_internal.h says what that can and cannot know).

#include <memory>
#include <string>
#include <vector>

struct GoLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

class GoLspDocument;

/**
 * @brief Parses and analyses a document; every query below runs on the result. `path` (may be empty)
 * only decides build constraints: a `_windows.go` file is not checked against linux's standard library.
 */
std::shared_ptr<const GoLspDocument> GoLspAnalyze(const std::vector<std::string> &lines, const std::string &path = "");

// --- Diagnostics ------------------------------------------------------------------

enum class GoLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct GoLspDiagnostic {
    GoLspRange range;
    GoLspSeverity severity = GoLspSeverity::Error;
    std::string code;  // stable id: syntax-error, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<GoLspDiagnostic> GoLspDiagnostics(const GoLspDocument &doc);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string GoLspFormatDiagnostic(const std::string &path, const GoLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool GoLspDiagnosticFails(const GoLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class GoLspKind {
    Text = 1, Method = 2, Function = 3, Field = 5, Variable = 6, Class = 7, Interface = 8, Module = 9, Property = 10,
    Value = 12, Keyword = 14, Snippet = 15, Folder = 19, Constant = 21, Struct = 22, TypeParameter = 25,
};

struct GoLspCompletionItem {
    std::string label;
    std::string insert_text;
    GoLspKind kind = GoLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<GoLspCompletionItem> GoLspCompletions(const GoLspDocument &doc, int line, int col);

struct GoLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    GoLspRange range;
};

GoLspHoverInfo GoLspHover(const GoLspDocument &doc, int line, int col);

struct GoLspSignatureHelp {
    bool found = false;
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::string documentation;
    int active_param = 0;
};

GoLspSignatureHelp GoLspSignatureAt(const GoLspDocument &doc, int line, int col);

// --- Navigation -------------------------------------------------------------------------

struct GoLspLocation {
    GoLspRange range;
};

std::vector<GoLspLocation> GoLspDefinition(const GoLspDocument &doc, int line, int col);

struct GoLspReference {
    GoLspRange range;
    bool is_definition = false;
};

std::vector<GoLspReference> GoLspReferences(const GoLspDocument &doc, int line, int col);

struct GoLspTextEdit {
    GoLspRange range;
    std::string new_text;
};

struct GoLspRenameTarget {
    bool found = false;
    GoLspRange range;
    std::string placeholder;
};

GoLspRenameTarget GoLspPrepareRename(const GoLspDocument &doc, int line, int col);
std::vector<GoLspTextEdit> GoLspRename(const GoLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class GoLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Package = 4, Class = 5, Method = 6, Property = 7, Field = 8, Function = 12,
    Interface = 11, Variable = 13, Constant = 14, String = 15, Number = 16, Boolean = 17, Array = 18, Object = 19,
    Key = 20, Null = 21, Struct = 23, TypeParameter = 26,
};

struct GoLspSymbol {
    std::string name;
    std::string detail;
    GoLspSymbolKind kind = GoLspSymbolKind::Variable;
    GoLspRange range;
    GoLspRange sel_range;
    int parent = -1;
};

std::vector<GoLspSymbol> GoLspSymbols(const GoLspDocument &doc);

struct GoLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

std::vector<GoLspFold> GoLspFolds(const GoLspDocument &doc);

// --- Formatting ------------------------------------------------------------------------------

/** @brief The formatter as one edit over the changed lines; none when already formatted. */
std::vector<GoLspTextEdit> GoLspFormat(const std::vector<std::string> &lines, int width, std::string *error);

#endif  // MEP_GO_LSP_H
