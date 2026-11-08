#ifndef MEP_SCHEME_READER_H
#define MEP_SCHEME_READER_H

#include <string>
#include <string_view>
#include <vector>

// The Scheme reader behind mep's own Scheme language server (scheme_lsp.h)
// and formatter (scheme_format.h): a hand-written reader that makes the
// decisions Guile's `read` makes (Guile 3.0's ice-9/read.scm, with its
// default read options: square brackets on, no keyword prefix, no curly
// infix), producing a small syntax tree with source ranges, the comments,
// and the reader errors.
//
// Guile is the oracle (tools/scheme_read_check.py runs this reader and
// Guile's `read` over a corpus and its damaged copies): a file this reader
// rejects is one Guile rejects, with the same message at the same
// line:column. Two things follow from reading the way Guile does rather
// than the way a report's grammar reads:
//   - a token is a number when Guile's string->number accepts it and a
//     symbol otherwise (`1+`, `...`, `->x`, `vars'` are symbols: `'` is
//     not a delimiter), so only `#`-prefixed numbers (`#x1G`) are errors;
//   - `#!r6rs`, `#!fold-case`, `#!no-fold-case`, `#!curly-infix` and
//     `#!curly-infix-and-bracket-lists` change the reader for the rest of
//     the file, any other `#!` opens a comment that runs to `!#`, and
//     Guix's G-expression syntax (`#~` `#$` `#$@` `#+` `#+@`) is read as
//     `(guix gexp)` installs it.
//
// Errors do not stop the read: Guile stops at the first one, which is what
// `first_error` reports, but an editor sees broken code constantly, so the
// reader recovers (a stray `)` is skipped, a list left open at the end of
// the file is closed) and keeps building the tree.
//
// Positions are 0-based lines and 0-based *byte* columns.

struct SchemeRange {
    int line = 0;      // 0-based start line
    int col = 0;       // 0-based start byte column
    int end_line = 0;  // 0-based end line
    int end_col = 0;   // 0-based end byte column, exclusive
};

/** @brief Reports whether a (line, col) position lies inside a range, end-inclusive (a cursor just after a token is on it). */
bool SchemeRangeContains(const SchemeRange &r, int line, int col);

enum class SchemeNodeKind {
    Error,      // a token the reader could not make sense of
    Symbol,     // `name` is the symbol's name (folded under #!fold-case, `#{...}#` decoded)
    Number,     // `text` as written
    String,     // `text` as written, quotes included
    Character,  // `text` as written: #\a, #\space, #\(
    Boolean,    // #t #f #true #false
    Keyword,    // #:name -- `name` is the name
    Nil,        // #nil
    BitVector,  // #*0101
    List,       // ( ... ) [ ... ] { ... }: kids are the elements; when `dotted`, the last kid is the cdr
    Vector,     // #( ... )
    Bytevector, // #vu8( ... ) #u8( ... ), and SRFI 4 / array literals: `text` is the prefix ("#u8(", "#f64(", "#2(")
    Prefix,     // ' ` , ,@ #' #` #, #,@ #~ #$ #$@ #+ #+@: `text` is the prefix, kids = {operand}
};

struct SchemeNode {
    SchemeNodeKind kind = SchemeNodeKind::Error;
    SchemeRange range;
    std::string text;  // the spelling (atoms) or the prefix (Prefix, Bytevector)
    std::string name;  // Symbol/Keyword: the name read
    char open = '(';   // List: '(' '[' or '{'
    bool dotted = false;
    bool closed = true;  // List/Vector: false when the file ended inside it
    int parent = -1;
    std::vector<int> kids;
};

enum class SchemeCommentKind { Line, Block, Datum, Shebang, Directive };

struct SchemeComment {
    SchemeCommentKind kind = SchemeCommentKind::Line;
    SchemeRange range;
    int datum = -1;  // Datum: the node `#;` comments out (not in any kids list)
};

struct SchemeReadError {
    SchemeRange range;  // where the editor underlines it
    // Guile's report: 1-based line and column as `read` prints them (its
    // column counts characters, a tab to the next multiple of 8), or 0
    // when Guile's message carries no position.
    int guile_line = 0, guile_col = 0;
    std::string message;  // Guile's wording
    std::string code;     // stable id: unbalanced-paren, unexpected-close, bad-escape, ...
};

// How deeply forms may nest before the reader stops building tree nodes
// (it is recursive); far beyond anything written by hand.
constexpr int kSchemeMaxNesting = 1000;

struct SchemeReadResult {
    std::vector<SchemeNode> nodes;  // the arena
    std::vector<int> top;           // top-level data, in source order
    std::vector<SchemeComment> comments;
    std::vector<SchemeReadError> errors;  // errors[0] is Guile's error when there is one
    bool fold_case = false;               // a #!fold-case is in effect at the end
    bool r6rs = false;                    // the file has a #!r6rs

    bool ok() const { return errors.empty(); }
    const SchemeNode &at(int index) const;
};

/** @brief Reads Scheme source the way Guile's `read` does, recovering from every error. */
SchemeReadResult SchemeRead(std::string_view text);

/** @brief Guile's string->number as a predicate: whether a token reads as a number (radix 10 by default). */
bool SchemeIsNumber(std::string_view token);

/** @brief Whether string->number would throw on a number token (an exponent a double cannot hold), and the exponent. */
bool SchemeNumberOutOfRange(std::string_view token, long *exponent);

#endif  // MEP_SCHEME_READER_H
