#ifndef MEP_JS_LSP_H
#define MEP_JS_LSP_H

// The analysis half of mep's own JavaScript language server (the wire half
// is js_lsp_server.cpp, the `mep-js-lsp` binary; the `js_ls` entry in
// main.cpp's `mep.lsp_servers` spawns it). Everything here is a pure
// function of the document text (plus the declaration files of the modules
// it loads, see JsLspAnalyze), so the whole server is testable without a
// client (js_lsp_test.cpp). Positions are 0-based lines and 0-based byte
// columns (`positionEncoding: "utf-8"`); the server converts when a client
// only speaks UTF-16.
//
// It sits on js_parse.h (acorn 8.15 + acorn-jsx, ported with exact error
// parity) and adds a scope analysis in the manner of eslint-scope
// (js_lsp_scope.cpp): every identifier is either a declaration, a
// reference resolved to the declaration it reads or writes, or a
// property/label name. The lints (js_lsp.cpp) use ESLint's rule names as
// their codes; hover, completion and navigation (js_lsp_features.cpp) use
// the same scopes plus the generated vocabulary of js_lsp_vocab.cpp.
//
// Dialects. JsLspDialect says how a document is parsed. Ts, Tsx and Dts
// use js_parse's TypeScript dialect (Options::typescript, typescript-estree
// trees): syntax errors are tsc's parser's, the scope analysis knows the
// type namespace, and the lints are the ones typescript-eslint's
// recommended config keeps (no-undef is off: tsc checks names).
//
// Declaration files. A module a document loads by `require("x")` or
// `import ... from "x"` is typed from its TypeScript declaration file when
// one can be found (js_lsp_dts.h has the lookup and the reading, which
// js_parse's TypeScript dialect does): `x.d.ts` beside a relative module,
// the package's `types` in node_modules, `$MEP_JS_TYPES`. Hover, completion
// and signature help (js_lsp_features.cpp) then follow what it declares --
// the module's exports, `new` of its classes, methods by `extends`, the
// declared return types of calls -- with each declaration's JSDoc.

#include <memory>
#include <string>
#include <vector>

struct JsLspRange {
    int line = 0, col = 0, end_line = 0, end_col = 0;
};

enum class JsLspDialect {
    Auto,    // .js: module, else script; JSX when that is what parses
    Module,  // .mjs
    Script,  // .cjs
    Jsx,     // .jsx: Auto with JSX on
    Ts,      // .ts / .mts / .cts
    Tsx,     // .tsx
    Dts,     // .d.ts / .d.mts / .d.cts: a declaration file
};

/** @brief The dialect for a file name or URI by its extension (Auto for anything unknown). */
JsLspDialect JsLspDialectForPath(const std::string &path);

class JsLspDocument;

/**
 * @brief Parses and analyses a document; every query below runs on the result.
 *
 * `path` (the document's file, "" for none) is where `require("x")` and
 * `import ... from "x"` look for the module's TypeScript declaration file
 * (js_lsp_dts.h): beside a relative module, in `node_modules`, in the
 * package itself, and in `$MEP_JS_TYPES`. Those declarations give hover,
 * completion and signature help for what the module exports (a native
 * addon's classes and functions, typically) and simple types to the
 * variables built from them. A module without one is what it always was.
 */
std::shared_ptr<const JsLspDocument> JsLspAnalyze(const std::vector<std::string> &lines,
                                                  JsLspDialect dialect = JsLspDialect::Auto,
                                                  const std::string &path = std::string());

// --- Diagnostics ------------------------------------------------------------------

enum class JsLspSeverity { Error = 1, Warning = 2, Information = 3, Hint = 4 };

struct JsLspDiagnostic {
    JsLspRange range;
    JsLspSeverity severity = JsLspSeverity::Error;
    std::string code;  // syntax-error, or ESLint's rule name: no-unused-vars, no-undef, ...
    std::string message;
    bool unnecessary = false;  // LSP DiagnosticTag.Unnecessary
    bool deprecated = false;   // LSP DiagnosticTag.Deprecated
};

std::vector<JsLspDiagnostic> JsLspDiagnostics(const JsLspDocument &doc);
/** @brief `path:line:col: severity: message [code]`, 1-based, as `--lint` prints it. */
std::string JsLspFormatDiagnostic(const std::string &path, const JsLspDiagnostic &d);
/** @brief Whether a diagnostic makes `--lint` exit non-zero (an error or a warning). */
bool JsLspDiagnosticFails(const JsLspDiagnostic &d);

// --- Completion / hover / signature help -------------------------------------------

enum class JsLspKind {
    Text = 1, Method = 2, Function = 3, Constructor = 4, Field = 5, Variable = 6, Class = 7, Interface = 8,
    Module = 9, Property = 10, Value = 12, Enum = 13, Keyword = 14, Snippet = 15,
    EnumMember = 20, Constant = 21, Struct = 22,
};

struct JsLspCompletionItem {
    std::string label;
    std::string insert_text;
    JsLspKind kind = JsLspKind::Text;
    std::string detail;
    std::string documentation;
    int replace_start = 0, replace_end = 0;  // byte columns on the request's line
};

std::vector<JsLspCompletionItem> JsLspCompletions(const JsLspDocument &doc, int line, int col);

struct JsLspHoverInfo {
    bool found = false;
    std::string text;  // markdown
    JsLspRange range;
};

JsLspHoverInfo JsLspHover(const JsLspDocument &doc, int line, int col);

struct JsLspSignatureHelp {
    bool found = false;
    std::string label;
    std::vector<std::string> params;  // substrings of label
    std::string documentation;
    int active_param = 0;
    // Every overload, when a declaration file gives several (label/params/documentation above
    // are overloads[active_signature]'s); empty otherwise.
    struct Overload {
        std::string label;
        std::vector<std::string> params;
        std::string documentation;
    };
    std::vector<Overload> overloads;
    int active_signature = 0;
};

JsLspSignatureHelp JsLspSignatureAt(const JsLspDocument &doc, int line, int col);

// --- Navigation -------------------------------------------------------------------------

struct JsLspLocation {
    JsLspRange range;
};

std::vector<JsLspLocation> JsLspDefinition(const JsLspDocument &doc, int line, int col);

struct JsLspReference {
    JsLspRange range;
    bool is_definition = false;
    bool is_write = false;
};

std::vector<JsLspReference> JsLspReferences(const JsLspDocument &doc, int line, int col);

struct JsLspTextEdit {
    JsLspRange range;
    std::string new_text;
};

struct JsLspRenameTarget {
    bool found = false;
    JsLspRange range;
    std::string placeholder;
};

JsLspRenameTarget JsLspPrepareRename(const JsLspDocument &doc, int line, int col);
std::vector<JsLspTextEdit> JsLspRename(const JsLspDocument &doc, int line, int col, const std::string &new_name,
                                       std::string *error);

// --- Outline / folding --------------------------------------------------------------------

enum class JsLspSymbolKind {
    File = 1, Module = 2, Namespace = 3, Class = 5, Method = 6, Property = 7, Field = 8, Constructor = 9,
    Enum = 10, Interface = 11, Function = 12, Variable = 13, Constant = 14, String = 15, Number = 16, Boolean = 17,
    Array = 18, Object = 19, Key = 20, Null = 21, EnumMember = 22, Struct = 23, TypeParameter = 26,
};

struct JsLspSymbol {
    std::string name;
    std::string detail;
    JsLspSymbolKind kind = JsLspSymbolKind::Variable;
    JsLspRange range;
    JsLspRange sel_range;
    int parent = -1;
};

std::vector<JsLspSymbol> JsLspSymbols(const JsLspDocument &doc);

struct JsLspFold {
    int start_line = 0, end_line = 0;
    std::string kind;  // "", "comment" or "imports"
};

std::vector<JsLspFold> JsLspFolds(const JsLspDocument &doc);

// --- Formatting ------------------------------------------------------------------------------
//
// Not here yet: formatting will be the Prettier port (src/js_format*), wired
// into js_lsp_server.cpp's `textDocument/formatting` (see FormatDocument
// there) the way mep-lua-lsp wires lua_format.h, and into `mep.format_js`.

#endif  // MEP_JS_LSP_H
