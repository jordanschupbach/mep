#pragma once

// mep's own OCaml formatter: the builtin behind `gf` in an OCaml buffer
// (mep.format_ocaml), and textDocument/formatting / rangeFormatting in
// mep-ocaml-lsp (help/formatting.org, help/ocaml-lsp.org).
//
// It lays code out the way ocamlformat's default profile (margin 80) does
// for indentation and spacing: two columns per level, `let ... in` bodies at
// the `let`'s column, match arms aligned with their `match`, an arm's body on
// the next line four columns in from its `|`, `then`/`else` under their `if`,
// `struct`/`sig`/`begin`/`object` contents two in and `end` back out, one
// space around binary operators and after `,` and `;`. Over-long lines are
// broken conservatively (after a binding's `=`, after an arm's `->`, before
// `else`, before `|>`, after `;` in a sequence or a list).
//
// It is safe by construction and by check: it only ever changes whitespace
// between tokens (and the indentation of a comment's continuation lines,
// shifted with the comment), it verifies that the result lexes to the same
// token stream as the input and refuses otherwise, it refuses unbalanced
// brackets / `begin`...`end` / `struct`...`end`, and its result is a fixed
// point (formatting it again changes nothing).

#include <string>
#include <string_view>
#include <vector>

namespace ocamlfmt {

struct Options {
    int width = 80;          // ocamlformat's default margin
    int indent = 2;          // columns per level (an arm body gets twice this)
    bool break_lines = true; // false: never insert line breaks (range formatting keeps lines)
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats OCaml source (.ml or .mli); on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

// --- the lexer (exposed for the tests and the language server) ----------------------

enum class TokKind {
    Ident,      // lowercase identifier (also `true`, `false`)
    UIdent,     // capitalised identifier: module or constructor
    Keyword,    // `let`, `match`, ... (also `let*`-style binding operators)
    Number,
    Char,       // 'a'
    String,     // "..." (may span lines)
    QString,    // {id|...|id}
    Comment,    // (* ... *), nested
    Op,         // operator symbol: `+`, `|>`, `->`, `::`, `=`, `|`, `:`, ...
    Punct,      // ( ) [ ] { } , ; ;; [| |] [< [> {< >} [@ [@@ [@@@ [% [%%
    Label,      // ~x  ~x:  ?x  ?x:
    TyVar,      // 'a
    Tag,        // `A
    Directive,  // a `#...` line (toplevel directive / line-number directive), to its end
};

struct Token {
    TokKind kind = TokKind::Ident;
    size_t begin = 0, end = 0;  // byte offsets
    int line = 0, col = 0;      // 0-based start
    int end_line = 0;           // 0-based line of the last byte
    std::string_view text;      // into the source
};

/** @brief Lexes OCaml; returns false (with a message and a 1-based line) on an unterminated
 *  comment, string or quoted string. */
bool Lex(std::string_view src, std::vector<Token> *out, std::string *error, int *error_line);

/** @brief Whether two sources lex to the same tokens (comments compared with each
 *  continuation line's leading whitespace and every line's trailing whitespace ignored). */
bool SameTokens(std::string_view a, std::string_view b, std::string *why = nullptr);

}  // namespace ocamlfmt
