#ifndef MEP_CLOJURE_READER_H
#define MEP_CLOJURE_READER_H

#include <string>
#include <vector>

// The Clojure reader behind mep's own Clojure language server: a
// hand-written implementation of clojure.lang.LispReader producing a
// small syntax tree with source ranges, plus the reader errors found on
// the way. clojure_lsp.cpp turns the tree into diagnostics and editor
// features; clojure_lsp_server.cpp puts it on the wire as
// `mep-clojure-lsp`.
//
// It follows LispReader's own rules, not a grammar's approximation of
// them: what ends a token (`a#b'c` is one symbol, `a"b` two forms), which
// tokens are invalid (`a:`, `:a/`, `08`, `1.5.5`), how `\` reads a
// character (`\(` is one, `\abc` an error), what a map must hold (an even
// number of forms, no two equal keys -- after `#?` has chosen its
// branch), and every reader macro. Its error messages are the JVM
// reader's own words, checked against Clojure 1.12 on a corpus of real
// code and damaged copies of it (tools/clojure_reader_check.clj).
//
// Scope, named rather than silently omitted:
//   - It reads source, it evaluates nothing: `#=(...)` keeps its form,
//     a tagged literal (`#inst "..."`) keeps its tag and value unchecked,
//     and `::alias/kw` is kept as written -- whether the alias exists is
//     the analysis's question, not the reader's.
//   - Reader conditionals keep every branch (the analysis looks at all of
//     them); the checks that depend on the platform -- a map's size and
//     duplicate keys -- are made the way the JVM reads it, with the :clj
//     branch (else :default).
//   - Errors never stop the read. An unclosed form, a stray closer, an
//     unterminated string -- every one is recorded and the reader carries
//     on, because an editor sees broken code constantly. A `(` at column 0
//     inside an unclosed top-level form is taken as where the missing
//     closers belong, so one forgotten `)` does not swallow the file.
//
// Positions are 0-based lines and 0-based byte columns.

struct CljRange {
    int line = 0;
    int col = 0;
    int end_line = 0;
    int end_col = 0;
};

/** @brief Reports whether (line, col) lies inside a range, end-exclusive. */
bool CljRangeContains(const CljRange &r, int line, int col);

/** @brief Reports whether (line, col) lies inside a range or right at its end. */
bool CljRangeTouches(const CljRange &r, int line, int col);

enum class CljNodeKind {
    Error,          // a token the reader could not make sense of; `text` is what was there
    Symbol,         // `ns`/`name`; `text` as written
    Keyword,        // `ns`/`name` (without the colons); `auto_resolve` for `::`
    Number,         // `text` as written
    String,         // `text` as written (quotes included); `name` the decoded value
    Character,      // `text` as written: \a, \newline, é
    Regex,          // `text` as written: #"..."
    Nil,
    Bool,           // `text` is "true" or "false"
    SymbolicValue,  // ##Inf, ##-Inf, ##NaN
    List,           // ( ... )
    Vector,         // [ ... ]
    Map,            // { ... }: kids alternate key, value
    Set,            // #{ ... }
    Fn,             // #( ... ): kids are the list's elements
    Quote,          // 'x      kids = {x}
    SyntaxQuote,    // `x      kids = {x}
    Unquote,        // ~x      kids = {x}
    UnquoteSplicing,  // ~@x   kids = {x}
    Deref,          // @x      kids = {x}
    Var,            // #'x     kids = {x}
    ReadEval,       // #=x     kids = {x}
    ReadCond,       // #?( ... ): kids alternate feature keyword, form
    ReadCondSplicing,  // #?@( ... )
    NsMap,          // #:ns{...}, #::{...}, #::alias{...}: `text` the prefix ("#:ns"), kids alternate key, value
    Tagged,         // #tag x: `text` the tag as written ("#inst"), kids = {x}
};

struct CljNode {
    CljNodeKind kind = CljNodeKind::Error;
    CljRange range;
    std::string text;
    std::string ns;    // Symbol/Keyword: the namespace part ("" for none)
    std::string name;  // Symbol/Keyword: the name part; String: the decoded value
    bool auto_resolve = false;  // Keyword written `::`
    int parent = -1;            // the enclosing node, -1 at top level
    std::vector<int> kids;
    // ^meta forms applied to this node, in source order (`^:private ^String x`).
    std::vector<int> meta;
};

struct CljComment {
    CljRange range;
    std::string text;  // `;...` or `#!...`
};

struct CljReadError {
    CljRange range;
    // Stable id: "eof", "unmatched-delimiter", "odd-map", "duplicate-key",
    // "invalid-token", "invalid-number", "invalid-character",
    // "string-escape", "eof-string", "eof-regex", "no-dispatch",
    // "bad-metadata", "metadata-target", "read-cond", "splice",
    // "nested-fn", "arg-literal", "symbolic-value", "ns-map",
    // "unreadable", "reader-tag", "regex", "too-deep".
    std::string code;
    std::string message;  // the JVM reader's message
};

constexpr int kCljMaxNesting = 1000;

struct CljReadResult {
    std::vector<CljNode> nodes;  // the arena
    std::vector<int> top;        // top-level forms, in order
    std::vector<int> discarded;  // forms read and dropped by #_ (and their own subtrees)
    std::vector<CljComment> comments;
    std::vector<CljReadError> errors;

    /** @brief Borrows a node by index (-1 or out of range yields a shared empty node). */
    const CljNode &at(int index) const;
};

/**
 * @brief Reads Clojure source into a syntax tree, recording and recovering from every reader error.
 * @param lines the document, one entry per line (no trailing newlines)
 * @param recover close an unclosed top-level form at the next `(def...` at column 0 (off: read on to the
 *        end of the file, as the JVM reader does -- for comparing with it)
 */
CljReadResult CljRead(const std::vector<std::string> &lines, bool recover = true);

/**
 * @brief The forms a collection holds the way the JVM reads it: `#?(...)` replaced by its :clj (else
 *        :default) branch or dropped, `#?@(...)` spliced.
 */
std::vector<int> CljEffectiveKids(const CljReadResult &r, int node);

/** @brief Reports whether a node is the symbol `name` with no namespace (or `clojure.core/name`). */
bool CljIsSymbol(const CljNode &n, const char *name);

#endif  // MEP_CLOJURE_READER_H
