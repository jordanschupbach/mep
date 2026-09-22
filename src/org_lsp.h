#ifndef MEP_ORG_LSP_H
#define MEP_ORG_LSP_H

#include <string>
#include <vector>

// The analysis half of mep's own org-mode language server (the wire half
// is org_lsp_server.cpp, the `mep-org-lsp` binary; kBuiltinLsp's
// `org_ls` registry entry in main.cpp is what spawns it).
//
// Deliberately raylib-free and I/O-light, for the same reason org_doc.h
// is: everything here is a pure function over plain std::string lines, so
// it is testable without a GL context, without a language-server client,
// and without a process (org_lsp_test.cpp drives all of it directly). The
// one exception is the on-disk existence check behind
// OrgLspOptions::check_files, which is why that is a flag rather than
// unconditional -- the test suite runs with it off.
//
// Scope, named rather than silently omitted (the same convention
// org_doc.h uses):
//   - This is a *linter plus completer*, not a full org parser producing
//     an AST. Nothing here builds an element tree; every check is a
//     line-oriented scan carrying a small structural state (the open
//     block stack, the open drawer, the current headline).
//   - Contents of a "literal" block (src/example/export/comment, and the
//     verbatim parts of verse) are never scanned for links, timestamps,
//     footnotes or emphasis: org treats them as literal text, and a help
//     page writing `[[file:x]]` inside `#+begin_example` to *show* the
//     syntax must not be told its link target is missing.
//   - Emphasis (`*bold*`, `/italic/`, ...) is never diagnosed. Org does
//     not treat an unmatched marker as an error -- it just renders
//     literally -- so every "unclosed emphasis" report would be a false
//     positive on ordinary prose.
//   - Table structure (column counts, `#+TBLFM:` references) is not
//     checked. Org tables are lenient about ragged rows and the formula
//     language is its own evaluator's business (formula.h).
//   - `#+SETUPFILE:` is not followed, so keywords/TODO sequences/macros
//     defined in another file are invisible here. A `#+TODO:` line in the
//     document itself is honored (via org_doc.h's ParseOrgOutline).
//
// Positions are 0-based lines and 0-based *byte* columns, not UTF-16 code
// units. That is what mep's own LSP client consumes (it feeds
// `range.start.character` straight to a byte column, see
// mep.lsp_render_diagnostics), and org_lsp_server.cpp declares
// `positionEncoding: "utf-8"` in its ServerCapabilities so a 3.17 client
// agrees rather than silently mis-scaling a line with non-ASCII in it.

// --- Options ----------------------------------------------------------

struct OrgLspOptions {
    // Directory the document lives in. Relative `file:` link targets and
    // `#+INCLUDE:` paths resolve against this; empty disables both checks
    // regardless of `check_files`.
    std::string doc_dir;
    // Whether to touch the filesystem at all (missing `file:` link
    // target, missing `#+INCLUDE:` file). Off in tests, and off for an
    // unsaved buffer that has no directory to resolve against.
    bool check_files = true;
};

// --- Diagnostics ------------------------------------------------------

// LSP DiagnosticSeverity, spelled out so callers don't have to remember
// which end of the scale is which.
enum class OrgLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct OrgLspDiagnostic {
    int line = 0;       // 0-based
    int col_start = 0;  // 0-based byte column, half-open [col_start, col_end)
    int col_end = 0;
    OrgLspSeverity severity = OrgLspSeverity::Error;
    // Stable machine-readable id ("unclosed-block", "bad-date", ...),
    // emitted as the LSP Diagnostic.code. Tests assert on these rather
    // than on message wording, so message text stays free to improve.
    std::string code;
    std::string message;
};

/**
 * @brief Lints an org document, reporting structural and syntax problems.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param opts filesystem-resolution options (see OrgLspOptions)
 * @return every diagnostic found, in ascending (line, col_start) order
 */
std::vector<OrgLspDiagnostic> OrgLspDiagnostics(const std::vector<std::string> &lines, const OrgLspOptions &opts);

// --- Completion -------------------------------------------------------

// LSP CompletionItemKind, only the handful this server actually emits.
enum class OrgLspKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Module = 9,
    Property = 10,
    Value = 12,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    Color = 16,
    File = 17,
    Reference = 18,
    Folder = 19,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    Event = 23,
    Operator = 24,
};

struct OrgLspCompletionItem {
    // What the list shows.
    std::string label;
    // What replacing [replace_start, replace_end) with this produces.
    // Always single-line: mep's Editor::AcceptCompletion splices the text
    // into one Buffer line with a plain string insert, so an embedded
    // '\n' would corrupt the line model rather than open a new line. That
    // rules out multi-line "structure template" items (`<s TAB`'s full
    // begin/end pair); the `#+end_<name>` completion offered while a
    // block is open covers the same need one line at a time.
    std::string insert_text;
    OrgLspKind kind = OrgLspKind::Keyword;
    std::string detail;         // short right-hand annotation
    std::string documentation;  // longer plain-text explanation
    // Byte columns on the request's own line. Always aligned to the
    // alnum/underscore word immediately before the cursor, never wider:
    // mep's client ignores textEdit and replaces exactly that word (see
    // Editor::UpdateCompletionPopup), so an item whose range reached back
    // over the `#+` would duplicate it.
    int replace_start = 0;
    int replace_end = 0;
};

/**
 * @brief Computes org-aware completions for a cursor position.
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (directory listing for `file:` link targets)
 * @return the candidates for this context, already filtered against the typed word prefix; empty where org offers nothing
 */
std::vector<OrgLspCompletionItem> OrgLspCompletions(const std::vector<std::string> &lines, int line, int col,
                                                    const OrgLspOptions &opts);

// --- Hover ------------------------------------------------------------

struct OrgLspHoverInfo {
    bool found = false;
    std::string text;   // plain text, possibly multi-line
    int line = 0;       // range the hover applies to
    int col_start = 0;
    int col_end = 0;
};

/**
 * @brief Explains the org construct under a cursor position (keyword, block, header argument, link, timestamp, entity).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @return the explanation and the range it covers, or a default-constructed value with `found == false`
 */
OrgLspHoverInfo OrgLspHover(const std::vector<std::string> &lines, int line, int col);

// --- Document symbols -------------------------------------------------

struct OrgLspSymbol {
    std::string name;
    std::string detail;  // TODO keyword / priority / tags, when present
    int line_start = 0;  // whole subtree, for the symbol's `range`
    int line_end = 0;
    int sel_col_start = 0;  // the title text itself, for `selectionRange`
    int sel_col_end = 0;
    int parent = -1;  // index into the returned vector, -1 for top level
};

/**
 * @brief Lists every headline as a document symbol, in document order with parent links.
 * @param lines the document's text, one entry per line
 * @return one symbol per headline; empty for a document with no headlines
 */
std::vector<OrgLspSymbol> OrgLspSymbols(const std::vector<std::string> &lines);

// --- Folding ----------------------------------------------------------

struct OrgLspFold {
    int start_line = 0;
    int end_line = 0;
    std::string kind;  // "" (a plain region) or "comment"
};

/**
 * @brief Computes folding ranges for headline subtrees, blocks and drawers.
 * @param lines the document's text, one entry per line
 * @return every foldable range, in ascending start-line order
 */
std::vector<OrgLspFold> OrgLspFolds(const std::vector<std::string> &lines);

// --- Definition -------------------------------------------------------

struct OrgLspLocation {
    bool found = false;
    // Absolute path of the target file, or "" meaning "this document".
    std::string path;
    int line = 0;
    int col = 0;
};

/**
 * @brief Resolves the link under a cursor position to a location (an internal headline/target/name, or another file).
 * @param lines the document's text, one entry per line
 * @param line 0-based cursor line
 * @param col 0-based cursor byte column
 * @param opts filesystem-resolution options (`doc_dir` anchors a relative `file:` target)
 * @return the resolved location, or a default-constructed value with `found == false`
 */
OrgLspLocation OrgLspDefinition(const std::vector<std::string> &lines, int line, int col, const OrgLspOptions &opts);

// --- Shared vocabulary (exposed for tests and for hover) ---------------

// One entry of the server's built-in org vocabulary: a keyword, block
// name, header argument, `#+OPTIONS:` switch, `#+STARTUP:` word or node
// property, with the one-line explanation hover and completion both show.
struct OrgLspVocabEntry {
    const char *name;
    const char *detail;
    const char *doc;
};

/** @brief The `#+KEYWORD:` vocabulary (document/export keywords), canonical uppercase. */
const std::vector<OrgLspVocabEntry> &OrgLspKeywordVocab();
/** @brief The `#+begin_...`/`#+end_...` block-name vocabulary, canonical lowercase. */
const std::vector<OrgLspVocabEntry> &OrgLspBlockVocab();
/** @brief The babel `:header-argument` vocabulary (names carry no leading colon). */
const std::vector<OrgLspVocabEntry> &OrgLspHeaderArgVocab();
/** @brief The `#+OPTIONS:` switch vocabulary (names carry their trailing colon, e.g. "toc:"). */
const std::vector<OrgLspVocabEntry> &OrgLspOptionVocab();
/** @brief The `#+STARTUP:` word vocabulary. */
const std::vector<OrgLspVocabEntry> &OrgLspStartupVocab();
/** @brief The node-property vocabulary for a `:PROPERTIES:` drawer (names carry no colons). */
const std::vector<OrgLspVocabEntry> &OrgLspPropertyVocab();
/** @brief The babel source languages mep can execute (mep.org_babel_langs' own key set). */
const std::vector<OrgLspVocabEntry> &OrgLspBabelLangVocab();
/** @brief The `\name` entity vocabulary (names carry no leading backslash). */
const std::vector<OrgLspVocabEntry> &OrgLspEntityVocab();

#endif  // MEP_ORG_LSP_H
