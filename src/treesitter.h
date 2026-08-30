#pragma once

// Phase 19 (NVIM_PARITY_PLAN.md) -- real Treesitter integration: a
// vendored libtree-sitter runtime plus a small core set of grammar
// sources compiled directly into mep (see CMakeLists.txt), replacing
// the earlier scoped-down hand-rolled lexer for every filetype a
// grammar is available for (main.cpp's kBuiltinSyntax keeps that lexer
// only as a final fallback). Beyond that core set, filetypes with a
// known highlight query but no compiled-in grammar are resolved by
// dlopen()ing a matching `.so` found on disk at runtime (native builds
// only -- see LoadDynamicLanguage in treesitter.cpp for the search
// path, which includes a Nix devShell's exported MEP_TS_PARSER_PATH).

#include <string>
#include <vector>

// One highlighted span, already clipped to a single buffer line (mep's
// decoration model is per-line: row, col_start, col_end). Row and
// columns are 0-indexed byte offsets; col_end is exclusive.
struct TSHighlightSpan {
    int row = 0;
    int col_start = 0;
    int col_end = 0;
    std::string capture;  // e.g. "keyword", "string.escape", "function.builtin"
};

// True if a grammar is available for this filetype (a bare extension,
// e.g. "cpp", "py" -- matches mep_lsp_filetype's convention in main.cpp)
// -- either compiled in, or, on native builds, loadable from disk at
// runtime. May attempt (and cache the result of) a dynamic-library
// search as a side effect.
bool TreesitterHasGrammar(const std::string &filetype);

// Parses `text` with the grammar for `filetype` and runs its highlight
// query, returning every capture as a line-local span. Incrementally
// reparses against the previous call's tree for the same `filetype` when
// one is cached (see treesitter.cpp's ParseCache/ComputeEdit), falling
// back to a from-scratch parse the first time a filetype is seen or
// after a grammar change. Returns an empty vector if no grammar is
// available for `filetype`.
std::vector<TSHighlightSpan> TreesitterHighlight(const std::string &filetype, const std::string &text);

// One foldable range, 0-indexed rows, both inclusive (mep's own Fold
// struct convention -- see main.cpp's Fold).
struct TSFoldRange {
    int start_row = 0;
    int end_row = 0;
};

// True if `filetype` has a fold query defined (distinct from
// TreesitterHasGrammar: fewer filetypes have a fold query than have a
// highlight query -- see treesitter_queries.h's kFolds* comment).
bool TreesitterHasFoldQuery(const std::string &filetype);

// Runs `filetype`'s fold query (if one is defined -- only the core
// compiled-in languages with a real block/body grammar node have one;
// markdown/org intentionally excluded, see treesitter_queries.h) over
// `text` and returns every capture spanning more than one line, via the
// same incremental-reparse cache TreesitterHighlight uses (a call here
// right after a TreesitterHighlight call for the same filetype/text is
// effectively free -- no reparse, the tree's already cached). Returns an
// empty vector if `filetype` has no fold query.
std::vector<TSFoldRange> TreesitterFoldRanges(const std::string &filetype, const std::string &text);

// One entry in a document's structure outline (mep's own sidebar/split
// consumer, main.cpp's kBuiltinStructure): a class/function/method/enum/
// etc definition. `row`/`col` point at the *name* token specifically (0-
// indexed, byte column) -- not the top of the definition, which may start
// several lines earlier for a multi-line signature -- so jumping there
// lands the cursor exactly on the identifier. `start_row` is the whole
// definition's own first row (0-indexed) -- may be earlier than `row`
// itself for a multi-line signature/decorator/attribute -- kept alongside
// `row` so a consumer that needs "is the cursor inside this definition"
// containment (rather than "jump to the name") has the real span to test
// against. `end_row` is the whole definition's own last row, used both for
// that same containment test and (already) to compute `depth` (nesting:
// how many other entries' [start_row, end_row] ranges contain this one).
// `kind` is the capture name's own "definition.<kind>" suffix from
// treesitter_structure_queries.h (e.g. "class", "function", "method",
// "enum", "struct", "interface", "namespace") -- deliberately not an
// enum, since it's just a display label/icon key, never branched on.
struct TSStructureNode {
    int row = 0;
    int col = 0;
    int start_row = 0;
    int end_row = 0;
    int depth = 0;
    std::string name;
    std::string kind;
};

// True if `filetype` has a structure query defined -- either compiled in
// (treesitter_structure_queries.h's core-language set) or, on native
// builds, resolvable via the same dynamic dlopen() lookup
// TreesitterHasGrammar uses.
bool TreesitterHasStructureQuery(const std::string &filetype);

// Runs `filetype`'s structure query over `text` (via the same incremental-
// reparse cache every other Treesitter* entry point shares) and returns
// every definition found, in document order, with `depth` already
// resolved. Returns an empty vector if `filetype` has no structure query
// or the query found nothing.
std::vector<TSStructureNode> TreesitterStructure(const std::string &filetype, const std::string &text);
