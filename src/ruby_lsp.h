#ifndef MEP_RUBY_LSP_H
#define MEP_RUBY_LSP_H

// The analysis half of mep's own Ruby language server (the wire half is
// ruby_lsp_server.cpp, the `mep-ruby-lsp` binary; the `ruby_ls` entry
// in main.cpp's kBuiltinLsp spawns it). Everything here is a pure function
// of the document text, so the whole server is testable without a client
// (ruby_lsp_test.cpp). Positions are 0-based lines and 0-based byte
// columns (`positionEncoding: "utf-8"`).
//
// Ruby is read with the vendored tree-sitter grammar (grammars/tree-sitter-ruby).
// `require`/`require_relative` are followed through the load path (RUBYLIB,
// `-I` in a `CMD:` header, `$LOAD_PATH` additions, gem-like `lib/` directories
// of the workspace and the file's ancestors), and the YARD doc comments of the
// definitions found there serve hover, completion, signature help and
// definition (help/ruby-lsp.org).

#include <memory>
#include <string>
#include <vector>

struct RubyLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

class RubyLspDocument;

/** @brief Parses and analyses a document; every query below runs on the result. `path` (the
 *  document's file, "" for none) is where `require_relative` and the load-path search start. */
std::shared_ptr<const RubyLspDocument> RubyLspAnalyze(const std::vector<std::string> &lines,
                                                      const std::string &path = std::string());
/** @brief The workspace root (the client's rootUri): its `lib/` directories join the load path. */
void RubyLspSetWorkspaceRoot(const std::string &root);
/** @brief Tests: replaces the environment's load path (RUBYLIB; nullptr restores it). */
void RubyLspSetLoadPath(const std::vector<std::string> *dirs);
/** @brief Documentation for @p name (`Foo`, `Foo::Bar`, `Foo#meth`, `Foo.meth`, `meth`) as seen
 *  from a document (Markdown; "" when unknown) -- `mep-ruby-lsp --doc`, for the Ruby UI's Help. */
std::string RubyLspDocFor(const RubyLspDocument &doc, const std::string &name);

// --- Diagnostics ------------------------------------------------------------------

enum class RubyLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct RubyLspDiagnostic {
    RubyLspRange range;
    RubyLspSeverity severity = RubyLspSeverity::Error;
    std::string code;  // stable id: syntax-error, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<RubyLspDiagnostic> RubyLspDiagnostics(const RubyLspDocument &doc);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string RubyLspFormatDiagnostic(const std::string &path, const RubyLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool RubyLspDiagnosticFails(const RubyLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class RubyLspKind {
    Text = 1, Method = 2, Function = 3, Field = 5, Variable = 6, Class = 7, Module = 9, Property = 10,
    Value = 12, Keyword = 14, Snippet = 15, Constant = 21, Struct = 22,
};

struct RubyLspCompletionItem {
    std::string label;
    std::string insert_text;
    RubyLspKind kind = RubyLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<RubyLspCompletionItem> RubyLspCompletions(const RubyLspDocument &doc, int line, int col);

struct RubyLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    RubyLspRange range;
};

RubyLspHoverInfo RubyLspHover(const RubyLspDocument &doc, int line, int col);

struct RubyLspSignatureParam {
    std::string label;  // a substring of the signature's label
    std::string documentation;  // markdown: the YARD @param text
};

struct RubyLspSignature {
    std::string label;
    std::vector<RubyLspSignatureParam> params;
    std::string documentation;  // markdown
};

struct RubyLspSignatureHelp {
    bool found = false;
    std::vector<RubyLspSignature> signatures;  // one per YARD @overload, else one
    int active_signature = 0;
    int active_param = 0;
};

RubyLspSignatureHelp RubyLspSignatureAt(const RubyLspDocument &doc, int line, int col);

// --- Navigation -------------------------------------------------------------------------

struct RubyLspLocation {
    RubyLspRange range;
    std::string path;  // "" = this document, else another file
};

std::vector<RubyLspLocation> RubyLspDefinition(const RubyLspDocument &doc, int line, int col);

struct RubyLspReference {
    RubyLspRange range;
    bool is_definition = false;
};

std::vector<RubyLspReference> RubyLspReferences(const RubyLspDocument &doc, int line, int col);

struct RubyLspTextEdit {
    RubyLspRange range;
    std::string new_text;
};

struct RubyLspRenameTarget {
    bool found = false;
    RubyLspRange range;
    std::string placeholder;
};

RubyLspRenameTarget RubyLspPrepareRename(const RubyLspDocument &doc, int line, int col);
std::vector<RubyLspTextEdit> RubyLspRename(const RubyLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class RubyLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Class = 5, Method = 6, Property = 7, Field = 8, Function = 12,
    Variable = 13, Constant = 14, String = 15, Number = 16, Boolean = 17, Array = 18, Object = 19, Key = 20,
    Null = 21,
};

struct RubyLspSymbol {
    std::string name;
    std::string detail;
    RubyLspSymbolKind kind = RubyLspSymbolKind::Variable;
    RubyLspRange range;
    RubyLspRange sel_range;
    int parent = -1;
};

std::vector<RubyLspSymbol> RubyLspSymbols(const RubyLspDocument &doc);

struct RubyLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

std::vector<RubyLspFold> RubyLspFolds(const RubyLspDocument &doc);

// --- Formatting ------------------------------------------------------------------------------

/** @brief The formatter as one edit over the changed lines; none when already formatted. */
std::vector<RubyLspTextEdit> RubyLspFormat(const std::vector<std::string> &lines, int width, std::string *error);

#endif  // MEP_RUBY_LSP_H
