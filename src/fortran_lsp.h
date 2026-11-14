#ifndef MEP_FORTRAN_LSP_H
#define MEP_FORTRAN_LSP_H

// The analysis half of mep's own Fortran language server (the wire half is
// fortran_lsp_server.cpp, the `mep-fortran-lsp` binary; the `fortran_ls` entry
// in main.cpp's kBuiltinLsp spawns it). Everything here is a pure function
// of the document text, so the whole server is testable without a client
// (fortran_lsp_test.cpp). Positions are 0-based lines and 0-based byte
// columns (`positionEncoding: "utf-8"`).
//
// The reader underneath (fortran_parse.h) handles free and fixed form and
// models the C preprocessor gfortran runs on .F90 files. Modules defined in
// the same file are followed through USE; a USE of any other module is
// handed to a FortranLspModuleSource (fortran_lsp_modules.h finds the
// module's file on disk), whose analysis then answers hover, completion,
// signature help and definition for the names it exports -- with their
// FORD documentation (`!>` above an entity, `!!` below or trailing). The
// lints stay per file: a USE of a module from another file, or an INCLUDE
// the reader cannot open, turns off the lints that would need to see it.

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct FortranLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

class FortranLspDocument;

/** @brief Where the analysis finds modules that a file USEs but does not define. Asked only
 *  while FortranLspAnalyze runs; the documents it returns are kept by the analysis. */
class FortranLspModuleSource {
public:
    virtual ~FortranLspModuleSource() = default;
    /** @brief The analysis of the file defining module @p name (lower case), or nullptr. */
    virtual std::shared_ptr<const FortranLspDocument> Module(const std::string &name) = 0;
    /** @brief Module names it knows, offered as completions after `use`. */
    virtual std::vector<std::string> ModuleNames() { return {}; }
};

/** @brief Parses and analyses a document; every query below runs on the result. */
/** @p path picks the source form (.f/.for fixed) and whether the preprocessor runs (.F90). */
std::shared_ptr<const FortranLspDocument> FortranLspAnalyze(const std::vector<std::string> &lines,
                                                            const std::string &path = "x.f90");
/** @brief The same, resolving USE of modules from other files through @p modules (may be null). */
std::shared_ptr<const FortranLspDocument> FortranLspAnalyze(const std::vector<std::string> &lines,
                                                            const std::string &path,
                                                            FortranLspModuleSource *modules);
/** @brief The path the document was analysed under. */
const std::string &FortranLspPath(const FortranLspDocument &doc);
/** @brief The modules (and submodules) the document defines, lower case. */
std::vector<std::string> FortranLspModulesDefined(const FortranLspDocument &doc);
/** @brief The modules from other files the analysis resolved (module name -> its analysis). */
std::vector<std::pair<std::string, std::shared_ptr<const FortranLspDocument>>> FortranLspImports(
    const FortranLspDocument &doc);
/** @brief The names of the modules @p text defines (`module NAME` lines, not `module procedure`),
 *  lower case: a cheap scan, no parse. */
std::vector<std::string> FortranLspScanModuleNames(const std::string &text);

// --- Diagnostics ------------------------------------------------------------------

enum class FortranLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct FortranLspDiagnostic {
    FortranLspRange range;
    FortranLspSeverity severity = FortranLspSeverity::Error;
    std::string code;  // stable id: syntax-error, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<FortranLspDiagnostic> FortranLspDiagnostics(const FortranLspDocument &doc);
/** @brief The reader's verdict on @p text (named @p path for its form), in the shape
 *  tools/ts_grammar_diff/fortran_check.py prints gfortran's: "" when there is no syntax
 *  error, else the first as `LINE: message` (LINE 0: at the end of the file).
 *  tools/fortran_parse_check.py compares the two. */
std::string FortranLspGfortranCheck(const std::string &text, const std::string &path);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string FortranLspFormatDiagnostic(const std::string &path, const FortranLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool FortranLspDiagnosticFails(const FortranLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class FortranLspKind {
    Text = 1, Method = 2, Function = 3, Field = 5, Variable = 6, Class = 7, Module = 9, Property = 10,
    Value = 12, Keyword = 14, Snippet = 15, Constant = 21, Struct = 22,
};

struct FortranLspCompletionItem {
    std::string label;
    std::string insert_text;
    FortranLspKind kind = FortranLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<FortranLspCompletionItem> FortranLspCompletions(const FortranLspDocument &doc, int line, int col);

struct FortranLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    FortranLspRange range;
};

FortranLspHoverInfo FortranLspHover(const FortranLspDocument &doc, int line, int col);

struct FortranLspSignature {
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::vector<std::string> param_docs;  // markdown, one per param ("" for none)
    std::string documentation;
};

struct FortranLspSignatureHelp {
    bool found = false;
    // the active signature (signatures[active_signature]), as the fields used before generics
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::string documentation;
    int active_param = 0;
    // every candidate: one per specific procedure of a generic interface
    std::vector<FortranLspSignature> signatures;
    int active_signature = 0;
};

FortranLspSignatureHelp FortranLspSignatureAt(const FortranLspDocument &doc, int line, int col);

// --- Navigation -------------------------------------------------------------------------

struct FortranLspLocation {
    FortranLspRange range;
    std::string path;  // the file it is in; "" for the document asked about
};

std::vector<FortranLspLocation> FortranLspDefinition(const FortranLspDocument &doc, int line, int col);

struct FortranLspReference {
    FortranLspRange range;
    bool is_definition = false;
};

std::vector<FortranLspReference> FortranLspReferences(const FortranLspDocument &doc, int line, int col);

struct FortranLspTextEdit {
    FortranLspRange range;
    std::string new_text;
};

struct FortranLspRenameTarget {
    bool found = false;
    FortranLspRange range;
    std::string placeholder;
};

FortranLspRenameTarget FortranLspPrepareRename(const FortranLspDocument &doc, int line, int col);
std::vector<FortranLspTextEdit> FortranLspRename(const FortranLspDocument &doc, int line, int col,
                                               const std::string &new_name, std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class FortranLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Class = 5, Method = 6, Property = 7, Field = 8, Function = 12,
    Variable = 13, Constant = 14, String = 15, Number = 16, Boolean = 17, Array = 18, Object = 19, Key = 20,
    Null = 21,
};

struct FortranLspSymbol {
    std::string name;
    std::string detail;
    FortranLspSymbolKind kind = FortranLspSymbolKind::Variable;
    FortranLspRange range;
    FortranLspRange sel_range;
    int parent = -1;
};

std::vector<FortranLspSymbol> FortranLspSymbols(const FortranLspDocument &doc);

struct FortranLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "" or "comment"
};

std::vector<FortranLspFold> FortranLspFolds(const FortranLspDocument &doc);

// --- Formatting ------------------------------------------------------------------------------

/** @brief The formatter as one edit over the changed lines; none when already formatted. */
std::vector<FortranLspTextEdit> FortranLspFormat(const std::vector<std::string> &lines, const std::string &path,
                                                 std::string *error);

#endif  // MEP_FORTRAN_LSP_H
