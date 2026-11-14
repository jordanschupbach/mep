#ifndef MEP_OCAML_LSP_H
#define MEP_OCAML_LSP_H

// The analysis half of mep's own OCaml language server (the wire half is
// ocaml_lsp_server.cpp, the `mep-ocaml-lsp` binary; the `ocaml_ls` entry
// in main.cpp's kBuiltinLsp spawns it). Everything here is a pure function
// of the document text, so the whole server is testable without a client
// (ocaml_lsp_test.cpp). Positions are 0-based lines and 0-based byte
// columns (`positionEncoding: "utf-8"`).
//
// OCaml is read with the vendored tree-sitter grammars (grammars/tree-sitter-ocaml,
// `ocaml` for .ml and `ocaml_interface` for .mli). Module references (`Foo.bar`,
// `open Foo`, `module M = Foo`) are followed to foo.mli / foo.ml in the workspace,
// the standard library and the library directories (MEP_OCAML_PATH, `-I` in a
// `CMD:` header, `_opam/lib`, $OPAM_SWITCH_PREFIX/lib, OCAMLPATH, `ocamlfind
// printconf path`); .mli signatures and odoc comments serve hover, completion and
// signature help (help/ocaml-lsp.org).

#include <memory>
#include <string>
#include <vector>

struct OcamlLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

class OcamlLspDocument;

/** @brief Parses and analyses a document; every query below runs on the result. */
std::shared_ptr<const OcamlLspDocument> OcamlLspAnalyze(const std::vector<std::string> &lines,
                                                       const std::string &path = std::string());

/** @brief The workspace root (the client's rootUri): its modules are visible to every document. */
void OcamlLspSetWorkspaceRoot(const std::string &root);
/** @brief Tests: forgets cached directory listings, parsed modules and the stdlib location. */
void OcamlLspResetCaches();
/** @brief Documentation for @p name (`List.map`, `List`, `print_endline`, `Foo.t`) as seen from a
 *  document (Markdown; "" when unknown) -- `mep-ocaml-lsp --doc`, for the OCaml UI's Help. */
std::string OcamlLspDocFor(const OcamlLspDocument &doc, const std::string &name);

// --- Diagnostics ------------------------------------------------------------------

enum class OcamlLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct OcamlLspDiagnostic {
    OcamlLspRange range;
    OcamlLspSeverity severity = OcamlLspSeverity::Error;
    std::string code;  // stable id: syntax-error, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<OcamlLspDiagnostic> OcamlLspDiagnostics(const OcamlLspDocument &doc);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string OcamlLspFormatDiagnostic(const std::string &path, const OcamlLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool OcamlLspDiagnosticFails(const OcamlLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class OcamlLspKind {
    Text = 1, Method = 2, Function = 3, Constructor = 4, Field = 5, Variable = 6, Class = 7, Interface = 8,
    Module = 9, Property = 10, Value = 12, Enum = 13, Keyword = 14, Snippet = 15, EnumMember = 20,
    Constant = 21, Struct = 22, Event = 23, TypeParameter = 25,
};

struct OcamlLspCompletionItem {
    std::string label;
    std::string insert_text;
    OcamlLspKind kind = OcamlLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<OcamlLspCompletionItem> OcamlLspCompletions(const OcamlLspDocument &doc, int line, int col);

struct OcamlLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    OcamlLspRange range;
};

OcamlLspHoverInfo OcamlLspHover(const OcamlLspDocument &doc, int line, int col);

struct OcamlLspSignatureHelp {
    bool found = false;
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::vector<std::string> param_docs;  // markdown, parallel to params ("" when none)
    std::string documentation;
    int active_param = 0;
};

OcamlLspSignatureHelp OcamlLspSignatureAt(const OcamlLspDocument &doc, int line, int col);

// --- Navigation -------------------------------------------------------------------------

struct OcamlLspLocation {
    OcamlLspRange range;
    std::string path;  // "" = this document, else another file
};

/** @brief Where a name is defined: the implementation (.ml) when there is one. */
std::vector<OcamlLspLocation> OcamlLspDefinition(const OcamlLspDocument &doc, int line, int col);
/** @brief Where a name is declared: the interface (.mli) when there is one. */
std::vector<OcamlLspLocation> OcamlLspDeclaration(const OcamlLspDocument &doc, int line, int col);

struct OcamlLspReference {
    OcamlLspRange range;
    bool is_definition = false;
};

std::vector<OcamlLspReference> OcamlLspReferences(const OcamlLspDocument &doc, int line, int col);

struct OcamlLspTextEdit {
    OcamlLspRange range;
    std::string new_text;
};

struct OcamlLspRenameTarget {
    bool found = false;
    OcamlLspRange range;
    std::string placeholder;
};

OcamlLspRenameTarget OcamlLspPrepareRename(const OcamlLspDocument &doc, int line, int col);
std::vector<OcamlLspTextEdit> OcamlLspRename(const OcamlLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class OcamlLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Class = 5, Method = 6, Property = 7, Field = 8, Constructor = 9,
    Enum = 10, Interface = 11, Function = 12, Variable = 13, Constant = 14, String = 15, Number = 16,
    Boolean = 17, Array = 18, Object = 19, Key = 20, Null = 21, EnumMember = 22, Struct = 23, Event = 24,
    Operator = 25, TypeParameter = 26,
};

struct OcamlLspSymbol {
    std::string name;
    std::string detail;
    OcamlLspSymbolKind kind = OcamlLspSymbolKind::Variable;
    OcamlLspRange range;
    OcamlLspRange sel_range;
    int parent = -1;
};

std::vector<OcamlLspSymbol> OcamlLspSymbols(const OcamlLspDocument &doc);

struct OcamlLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

std::vector<OcamlLspFold> OcamlLspFolds(const OcamlLspDocument &doc);

// --- Formatting ------------------------------------------------------------------------------

/** @brief The formatter as one edit over the changed lines; none when already formatted. */
std::vector<OcamlLspTextEdit> OcamlLspFormat(const std::vector<std::string> &lines, int width, std::string *error);
/** @brief Re-indents lines @p first..@p last (0-based, inclusive) without breaking or joining lines. */
std::vector<OcamlLspTextEdit> OcamlLspFormatRange(const std::vector<std::string> &lines, int first, int last,
                                                  int width, std::string *error);

#endif  // MEP_OCAML_LSP_H
