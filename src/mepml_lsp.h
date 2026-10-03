#ifndef MEP_MEPML_LSP_H
#define MEP_MEPML_LSP_H

#include <string>
#include <vector>

// The analysis half of mep's own mepml language server (the wire half is
// mepml_lsp_server.cpp, the `mep-mepml-lsp` binary; kBuiltinLsp's
// `mepml_ls` registry entry in main.cpp is what spawns it).
//
// Everything here is a pure function over the document's lines, answered
// from mepml_doc.h's own parser -- the same one the editor renders from
// and the exporters write from -- so the server can never disagree with
// the rest of mep about what a document means. mepml_lsp_test.cpp drives
// all of it directly, with no process and no JSON-RPC client.
//
// What the server knows, beyond the parser's own structural diagnostics:
//   - Cross-references: citation keys (\cite / \citep against \citation
//     entries, including those of @imported files), heading anchors
//     (`[text|#anchor]` against the headings' slugs), footnotes.
//   - The filesystem, when MepmlLspOptions::check_files is on: \image and
//     \import paths, relative link targets, figures a code block wrote.
//   - Document structure: headings (nested), figures, tables and code
//     blocks as symbols; the same fold ranges the editor folds.
//
// Scope, named rather than silently omitted:
//   - Code inside a code block is not analysed: that is its own language's
//     business (a server for that language, or running it).
//   - Maths is not parsed. TeX is checked by the renderer that typesets
//     it; a syntax error there shows up where the formula is drawn.
//   - Nothing is spell- or grammar-checked.
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16 code
// units, the same convention as org_lsp.h (and the server declares
// `positionEncoding: "utf-8"`).

// --- Options ----------------------------------------------------------

struct MepmlLspOptions {
    // The document's own absolute path. \import and relative paths resolve
    // against its directory; empty (an unsaved buffer) turns off every
    // check that would need one.
    std::string doc_path;
    // Whether to touch the filesystem at all (\import expansion, missing
    // images and link targets, path completion). Off in tests.
    bool check_files = true;
};

// --- Diagnostics ------------------------------------------------------

enum class MepmlLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct MepmlLspDiagnostic {
    int line = 0;       // 0-based
    int col_start = 0;  // 0-based byte column, half-open [col_start, col_end)
    int col_end = 0;
    MepmlLspSeverity severity = MepmlLspSeverity::Error;
    // Stable machine-readable id ("unknown-citation", "unclosed-code",
    // ...), the LSP Diagnostic.code; tests and code actions key off it.
    std::string code;
    std::string message;
};

/**
 * @brief Lints a mepml document: the parser's structural problems plus cross-reference and filesystem checks.
 * @param lines the document's text, one entry per line
 * @param opts filesystem-resolution options
 * @return every diagnostic, in ascending (line, col_start) order
 */
std::vector<MepmlLspDiagnostic> MepmlLspDiagnostics(const std::vector<std::string> &lines, const MepmlLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, the ones this server emits.
enum class MepmlLspKind {
    Text = 1,
    Function = 3,
    Field = 5,
    Variable = 6,
    Module = 9,
    Property = 10,
    Value = 12,
    Keyword = 14,
    Color = 16,
    File = 17,
    Reference = 18,
    Folder = 19,
    Constant = 21,
};

struct MepmlLspCompletionItem {
    std::string label;
    // What replaces [replace_start, replace_end) on the request's line.
    // Single-line, and aligned to the letters/digits/underscores right
    // before the cursor -- mep's client replaces exactly that word (see
    // org_lsp.h) -- so completing `squares.pn` inserts only what follows
    // the word being typed.
    std::string insert_text;
    MepmlLspKind kind = MepmlLspKind::Keyword;
    std::string detail;
    std::string documentation;
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Completions for a cursor position: directives, inline commands, citation keys, colours, fonts, heading anchors, paths, metadata keys, callout keywords, code languages and options.
 * @param lines the document's text
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (path completion lists the document's directory)
 * @return the candidates, already filtered against what has been typed
 */
std::vector<MepmlLspCompletionItem> MepmlLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                        const MepmlLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct MepmlLspHoverInfo {
    bool found = false;
    std::string text;  // plain text, possibly multi-line
    int line = 0;
    int col_start = 0;
    int col_end = 0;
};

/**
 * @brief Explains what is under the cursor: a citation's entry, a footnote, a link's target, a colour, a directive, a metadata key, a code block, a heading.
 * @return the explanation and the range it covers, or `found == false`
 */
MepmlLspHoverInfo MepmlLspHover(const std::vector<std::string> &lines, int line, int col, const MepmlLspOptions &opts);

// --- Locations: definition, references, rename --------------------------

struct MepmlLspLocation {
    bool found = false;
    std::string path;  // absolute path of another file, "" for this document
    int line = 0;
    int col_start = 0;
    int col_end = 0;
};

/**
 * @brief Where the thing under the cursor is defined: a citation's \citation entry (here or in an imported file), a link's heading or file, an \import/\image file.
 */
MepmlLspLocation MepmlLspDefinition(const std::vector<std::string> &lines, int line, int col, const MepmlLspOptions &opts);

struct MepmlLspReference {
    int line = 0;
    int col_start = 0;
    int col_end = 0;
    bool is_definition = false;  // the \citation key / the heading title itself
};

struct MepmlLspReferenceSet {
    bool found = false;
    std::vector<MepmlLspReference> refs;  // document order
    // Non-empty when the thing can be found but not renamed here (a
    // citation defined in an @imported file), saying why.
    std::string rename_blocked_reason;
    // What a rename replaces each reference's range with, given the new
    // name (a heading's links take its slug, its title the name itself).
    bool is_heading = false;
};

/**
 * @brief Every mention of the citation key or heading under the cursor in this document (the \cite uses and the \citation entry; the heading and every link to its anchor).
 */
MepmlLspReferenceSet MepmlLspReferences(const std::vector<std::string> &lines, int line, int col,
                                        const MepmlLspOptions &opts);

struct MepmlLspTextEdit {
    int start_line = 0, start_col = 0;
    int end_line = 0, end_col = 0;
    std::string new_text;  // may contain '\n'
};

/**
 * @brief The edits renaming the citation key or heading under the cursor.
 * @param new_name the new key, or the heading's new title (its links follow the new slug)
 * @param error set to why nothing can be renamed, when that is the answer
 * @return the edits (empty with *error set when the rename is refused)
 */
std::vector<MepmlLspTextEdit> MepmlLspRename(const std::vector<std::string> &lines, int line, int col,
                                             const std::string &new_name, const MepmlLspOptions &opts,
                                             std::string *error);

// --- Structure ----------------------------------------------------------

// LSP SymbolKind, the ones this server emits.
enum class MepmlLspSymbolKind { Module = 2, Namespace = 3, Function = 12, Array = 18, Object = 19, Key = 20, Struct = 23 };

struct MepmlLspSymbol {
    std::string name;
    std::string detail;
    MepmlLspSymbolKind kind = MepmlLspSymbolKind::Struct;
    int line_start = 0;  // the whole construct (a heading: its section)
    int line_end = 0;
    int sel_line = 0;  // the name itself
    int sel_col_start = 0;
    int sel_col_end = 0;
    int parent = -1;  // index into the returned vector, -1 at top level
};

/**
 * @brief Headings (nested by depth) with the figures, tables and code blocks of each section under it.
 */
std::vector<MepmlLspSymbol> MepmlLspSymbols(const std::vector<std::string> &lines);

struct MepmlLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" or "comment"
};

/**
 * @brief Fold ranges: heading sections, document headers, code blocks, citations, display maths, tables, lists and comment runs -- the editor's own folds.
 */
std::vector<MepmlLspFold> MepmlLspFolds(const std::vector<std::string> &lines);

// --- Code actions and formatting ------------------------------------------

struct MepmlLspCodeAction {
    std::string title;
    std::string kind = "quickfix";  // or "source"
    std::vector<MepmlLspTextEdit> edits;
    // The diagnostic code this action fixes, "" for one that fixes none.
    std::string fixes;
};

/**
 * @brief The fixes on offer for the cursor's line: add a missing \citation entry, correct a misspelt directive, anchor or colour, pad a ragged table, align a table, rename @printbibliography.
 */
std::vector<MepmlLspCodeAction> MepmlLspCodeActions(const std::vector<std::string> &lines, int line,
                                                    const MepmlLspOptions &opts);

/**
 * @brief Formats the document: every table's pipes lined up (honouring column alignment) and ragged rows padded. Nothing else is touched.
 * @return one edit per table whose text changes; empty when the document is already formatted
 */
std::vector<MepmlLspTextEdit> MepmlLspFormat(const std::vector<std::string> &lines);

// --- Style sheets (.mepss) ---------------------------------------------
// The same server answers for a mepml style sheet (docs/mepml-spec/style.md):
// `lines` is the sheet's text.

/**
 * @brief What is wrong in a sheet: what its parser could not use (an unknown property, a value a property does not take, a malformed selector), and selectors naming an element or a part mepml has none of.
 */
std::vector<MepmlLspDiagnostic> MepssLspDiagnostics(const std::vector<std::string> &lines);
/**
 * @brief Completion in a sheet: element names, `[attributes]` and their values, `::parts` and `:states` in a selector; property names and each property's values (colour functions, theme groups, colour names) in a rule; media tags after `@media`.
 */
std::vector<MepmlLspCompletionItem> MepssLspCompletions(const std::vector<std::string> &lines, int line, int col);
/**
 * @brief Explains the property, part or function under the cursor.
 */
MepmlLspHoverInfo MepssLspHover(const std::vector<std::string> &lines, int line, int col);

#endif  // MEP_MEPML_LSP_H
