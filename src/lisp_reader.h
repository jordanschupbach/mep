#ifndef MEP_LISP_READER_H
#define MEP_LISP_READER_H

#include <string>
#include <vector>

// The Common Lisp reader behind mep's own Common Lisp language server: a
// hand-written implementation of the standard readtable (CLHS 2.4)
// producing a small syntax tree with source ranges, plus the reader
// errors found on the way. lisp_lsp.cpp turns that tree into diagnostics
// and editor features; lisp_lsp_server.cpp puts it on the wire as
// `mep-lisp-lsp`.
//
// Hand-written for the reasons maxima_ast.h gives, and with one that is
// specific to Lisp: the language's own reader is the parser, and every
// implementation's reader lives inside a running image. A grammar
// approximation would disagree with that reader exactly where it
// matters -- `#\(` is a character and not an open paren, `a#b` is one
// symbol, `|foo bar|` is one symbol, `1.` is an integer and `.5` a
// float -- so this follows the standard's token and dispatch-macro
// rules directly.
//
// Scope, named rather than silently omitted:
//   - This reads *source*, not Lisp data. Symbols are not interned
//     anywhere and nothing is evaluated: `#.` keeps its form unevaluated
//     and `#+`/`#-` keep both the feature expression and the form, with
//     the form marked `conditional` so the analysis half can decide what
//     to do with code that may not exist on a given implementation.
//   - The readtable is the standard one. A file that installs reader
//     macros of its own (`set-dispatch-macro-character`,
//     named-readtables' `in-readtable`) reads differently in a real
//     image; lisp_lsp.cpp notices those files and stops reporting the
//     dispatch characters it does not know.
//   - *read-base* is 10 and readtable-case is :upcase, the defaults.
//     Symbol names are canonicalized the way that reader would (ASCII
//     upcased, escaped characters kept verbatim), and the original
//     spelling is kept alongside for display.
//   - Errors never stop the read. An unbalanced paren, a stray `)`, an
//     unterminated string or `#|` comment, an unknown `#` dispatch
//     character -- every one is recorded and the reader carries on,
//     because an editor sees broken code constantly and a server that
//     gave up at the first error would go blank while the user types.
//
// Positions are 0-based lines and 0-based *byte* columns, the convention
// every other mep server uses and what its LSP client reads.

// --- Source ranges ----------------------------------------------------

struct LispRange {
    int line = 0;      // 0-based start line
    int col = 0;       // 0-based start byte column
    int end_line = 0;  // 0-based end line
    int end_col = 0;   // 0-based end byte column, exclusive
};

/** @brief Reports whether a (line, col) position lies inside a range, end-exclusive. */
bool LispRangeContains(const LispRange &r, int line, int col);

// --- Syntax tree ------------------------------------------------------

enum class LispNodeKind {
    Error,      // a token the reader could not make sense of; `text` is what was there
    Symbol,     // see `name`/`package` below
    Number,     // `text` as written: 42, -1/2, 1.5d0, #x1F, #b101, #36rZZ
    String,     // `text` as written (quotes included); `name` holds the decoded value
    Character,  // `text` as written: #\a, #\Space, #\(
    BitVector,  // #*1011
    LabelRef,   // #1#
    List,       // ( ... ): kids are the elements; when `dotted`, the last kid is the cdr
    Vector,     // #( ... ) or #3( ... ): kids are the elements
    Quote,      // 'x            kids = {operand}
    Backquote,  // `x            kids = {operand}
    Unquote,    // ,x            kids = {operand}
    Splice,     // ,@x or ,.x    kids = {operand}
    Function,   // #'x           kids = {operand}
    ReadEval,   // #.x           kids = {operand}
    Feature,    // #+f x / #-f x text = "#+" or "#-", kids = {feature expression, form}
    // Every other dispatch macro that takes a following form: #p"...",
    // #S(...), #2A(...), #c(...), #1=... -- `text` is the prefix exactly
    // as written ("#P", "#S", "#2A", "#C", "#1="), kids = {operand}.
    Dispatch,
};

struct LispNode {
    LispNodeKind kind = LispNodeKind::Error;
    LispRange range;
    // The original spelling for atoms; the prefix for the prefix kinds
    // ("'", "`", ",", ",@", "#'", "#.", "#+", "#P", ...).
    std::string text;
    // Symbol: the canonical name the standard reader would intern --
    // unescaped ASCII upcased, `|...|` and `\` escapes resolved -- with
    // no package prefix. String: the decoded value.
    std::string name;
    // Symbol: the canonical package prefix ("" for none, "KEYWORD" for a
    // leading colon, otherwise the upcased package name as written).
    std::string package;
    int name_col = 0;         // Symbol: byte column where the symbol-name part starts (after any prefix)
    bool internal = false;    // Symbol: written with `::`
    bool uninterned = false;  // Symbol: written `#:name`
    bool escaped = false;     // Symbol: contained a `|` or `\` escape
    bool dotted = false;      // List: the last kid is a dotted cdr
    // The form is the operand of a `#+`/`#-` feature expression, so it
    // only exists on implementations whose features satisfy it.
    bool conditional = false;
    int parent = -1;  // index of the enclosing node, -1 at top level
    std::vector<int> kids;
};

struct LispComment {
    LispRange range;
    std::string text;    // the whole comment, delimiters included
    bool block = false;  // `#| ... |#` rather than `;`
};

struct LispReadError {
    LispRange range;
    // Stable id: "unbalanced-paren", "stray-close-paren",
    // "unterminated-string", "unterminated-comment",
    // "unterminated-escape", "unknown-dispatch", "bad-number",
    // "bad-symbol", "bad-dot", "unknown-character-name",
    // "comma-outside-backquote", "missing-form", "unreadable-object",
    // "too-deep".
    std::string code;
    std::string message;  // human wording, free to improve
};

// How deeply one form may nest before the reader stops building tree
// nodes for it. The reader is recursive; nothing a person writes comes
// anywhere near this, and generated input that does must cost a
// diagnostic rather than the process's stack.
constexpr int kLispMaxNesting = 1000;

struct LispReadResult {
    std::vector<LispNode> nodes;  // the arena; every index refers into this
    std::vector<int> top;         // the top-level forms, in source order
    std::vector<LispComment> comments;
    std::vector<LispReadError> errors;

    /** @brief Reports whether the document read without a single reader error. */
    bool ok() const { return errors.empty(); }
    /** @brief Borrows a node by index (-1 or out of range yields a shared empty node). */
    const LispNode &at(int index) const;
};

/**
 * @brief Reads Common Lisp source into a syntax tree, recording and recovering from every reader error.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @return the arena, the top-level forms, the comments and every reader error found
 */
LispReadResult LispRead(const std::vector<std::string> &lines);

// --- Small shared predicates -------------------------------------------

/**
 * @brief Reports whether a node is a symbol with this canonical name in the COMMON-LISP package (or unqualified).
 * @param node the node to test
 * @param upper_name the canonical (upper-case) name, e.g. "DEFUN"
 */
bool LispIsSymbolNamed(const LispNode &node, const char *upper_name);

/** @brief Reports whether a node is a keyword symbol (`:foo`). */
bool LispIsKeyword(const LispNode &node);

/** @brief Reports whether a symbol node is written without a package prefix, or with `cl:`/`common-lisp:`. */
bool LispIsUnqualifiedOrCl(const LispNode &node);

/** @brief Reports whether a token (no escapes) has the standard syntax of a number in radix 10. */
bool LispIsNumberToken(const std::string &token);

#endif  // MEP_LISP_READER_H
