#ifndef MEP_APL_LSP_H
#define MEP_APL_LSP_H

// The analysis half of mep's own APL language server (the wire half is
// apl_lsp_server.cpp, the `mep-apl-lsp` binary; the `apl_ls` entry in
// main.cpp's mep.lsp_servers spawns it). Everything here is a pure function
// of the document text (plus, for cross-file answers, the other files'
// analyses: AplLspWorkspace), so the whole server is testable without a
// client (apl_lsp_test.cpp). Positions are 0-based lines and 0-based byte
// columns (`positionEncoding: "utf-8"`).
//
// GNU APL and Dyalog APL: tradfns (∇ header ... ∇, or a Dyalog .aplf/.aplo
// file whose first line is the header), dfns and dops assigned to names,
// :Namespace / :Class scripts, Dyalog control structures, labels.
//
// Documentation is the APL convention: the ⍝ lines right after a tradfn's
// header (or right after a dfn's opening `name←{` line), or a ⍝ block right
// before `name←{`. Hover, completion, signature help and --doc show it as
// Markdown.

#include <memory>
#include <string>
#include <vector>

struct AplLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

// --- Definitions -----------------------------------------------------------------

enum class AplDefKind { Tradfn, TradOp, Dfn, Dop, Namespace, Class, Variable, Label };

/** @brief A name a document defines: a function, operator, namespace, class, variable or label. */
struct AplDef {
    AplDefKind kind = AplDefKind::Variable;
    std::string name;       // the simple name
    std::string qualified;  // ns.sub.name (the enclosing :Namespace / :Class names), or name
    std::string header;     // tradfn header without ∇ ("Z←A PLUS B;t"), or "name←{" line for a dfn
    std::string result, left, right, left_operand, right_operand;
    bool left_optional = false;  // {L} in the header, or a dfn that sets ⍺←
    bool shy_result = false;     // {R}←
    std::vector<std::string> locals;
    std::vector<std::string> labels;  // a tradfn's labels
    std::string doc;  // the ⍝ documentation, Markdown
    AplLspRange range;      // the whole definition
    AplLspRange sel_range;  // the name
    int parent = -1;        // index of the enclosing namespace/class definition
    int owner = -1;         // for a Label: the tradfn it belongs to
    std::string path;       // file the definition is in ("" for the document itself)
};

class AplLspDocument;

/** @brief Parses and analyses a document; every query below runs on the result. @p path picks .aplf handling. */
std::shared_ptr<const AplLspDocument> AplLspAnalyze(const std::vector<std::string> &lines,
                                                    const std::string &path = std::string());
/** @brief The definitions of a document, in source order. */
const std::vector<AplDef> &AplLspDefinitions(const AplLspDocument &doc);
const std::string &AplLspPath(const AplLspDocument &doc);

/** @brief Other files' definitions visible from a document (workspace files, MEP_APL_PATH). */
struct AplLspWorkspace {
    std::vector<std::shared_ptr<const AplLspDocument>> docs;
};

// --- Diagnostics ------------------------------------------------------------------

enum class AplLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct AplLspDiagnostic {
    AplLspRange range;
    AplLspSeverity severity = AplLspSeverity::Error;
    std::string code;  // stable id: syntax-error, unbalanced, control-structure, unknown-system-name, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<AplLspDiagnostic> AplLspDiagnostics(const AplLspDocument &doc);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string AplLspFormatDiagnostic(const std::string &path, const AplLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool AplLspDiagnosticFails(const AplLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class AplLspKind {
    Text = 1, Method = 2, Function = 3, Field = 5, Variable = 6, Class = 7, Module = 9, Property = 10,
    Value = 12, Keyword = 14, Snippet = 15, Constant = 21, Struct = 22, Operator = 24,
};

struct AplLspCompletionItem {
    std::string label;
    std::string insert_text;
    std::string filter_text;  // what the client matches the typed prefix against ("" = label)
    AplLspKind kind = AplLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<AplLspCompletionItem> AplLspCompletions(const AplLspDocument &doc, int line, int col,
                                                    const AplLspWorkspace *ws = nullptr);

struct AplLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    AplLspRange range;
};

AplLspHoverInfo AplLspHover(const AplLspDocument &doc, int line, int col, const AplLspWorkspace *ws = nullptr);

struct AplLspSignatureHelp {
    bool found = false;
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::string documentation;
    int active_param = 0;
};

AplLspSignatureHelp AplLspSignatureAt(const AplLspDocument &doc, int line, int col,
                                      const AplLspWorkspace *ws = nullptr);

/** @brief Markdown documentation for a name: a glyph, ⎕name, control word, )command or user definition. */
std::string AplLspDocFor(const std::string &name, const AplLspDocument *doc, const AplLspWorkspace *ws);
/** @brief Markdown for one user definition (header block, kind, doc, location). */
std::string AplLspDefMarkdown(const AplDef &d);

// --- Navigation -------------------------------------------------------------------------

struct AplLspLocation {
    std::string path;  // "" for the document itself
    AplLspRange range;
};

std::vector<AplLspLocation> AplLspDefinition(const AplLspDocument &doc, int line, int col,
                                             const AplLspWorkspace *ws = nullptr);

struct AplLspReference {
    AplLspRange range;
    bool is_definition = false;
};

std::vector<AplLspReference> AplLspReferences(const AplLspDocument &doc, int line, int col);

struct AplLspTextEdit {
    AplLspRange range;
    std::string new_text;
};

struct AplLspRenameTarget {
    bool found = false;
    AplLspRange range;
    std::string placeholder;
};

AplLspRenameTarget AplLspPrepareRename(const AplLspDocument &doc, int line, int col);
std::vector<AplLspTextEdit> AplLspRename(const AplLspDocument &doc, int line, int col, const std::string &new_name,
                                         std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class AplLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Class = 5, Method = 6, Property = 7, Field = 8, Function = 12,
    Variable = 13, Constant = 14, String = 15, Number = 16, Boolean = 17, Array = 18, Object = 19, Key = 20,
    Null = 21, Operator = 25,
};

struct AplLspSymbol {
    std::string name;
    std::string detail;
    AplLspSymbolKind kind = AplLspSymbolKind::Variable;
    AplLspRange range;
    AplLspRange sel_range;
    int parent = -1;
};

std::vector<AplLspSymbol> AplLspSymbols(const AplLspDocument &doc);

struct AplLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

std::vector<AplLspFold> AplLspFolds(const AplLspDocument &doc);

// --- Formatting ------------------------------------------------------------------------------

/** @brief The formatter as one edit over the changed lines; none when already formatted. */
std::vector<AplLspTextEdit> AplLspFormat(const std::vector<std::string> &lines, int width, std::string *error);

#endif  // MEP_APL_LSP_H
