// rust_parse.h -- mep's Rust parser: a recursive-descent port of rustc_parse
// (rust-lang/rust 1.97.1, MIT OR Apache-2.0), producing the tree in
// rust_ast.h. Shared by mep-rust-lsp and the Rust formatter.
//
// Errors follow rustc's: the first diagnostic of a file is the one rustc
// prints first (`rustc -Zparse-crate-root-only`), same message, same
// line:col, as often as the port manages. After an error the parser keeps
// going where rustc recovers, so broken code still yields a tree.
//
//   auto r = rust::parse_file(text, opts);
//   for (rust::Item* item : r->crate->items) ...
//   if (!r->diagnostics.empty()) first = r->diagnostics[0];
//
// Macro bodies stay token ranges (DelimArgs::tok_lo/tok_hi into r->tokens);
// re-parse one with parse_fragment(), which is how rustfmt formats macro
// arguments (try an expression list, then items, ...).
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rust_ast.h"
#include "rust_lex.h"

namespace rust {

struct ParseOptions {
    Edition edition = Edition::E2021;
    /// Nesting depth (expressions, blocks, types, patterns, items) after which
    /// the parser stops with "nesting too deep" instead of recursing further.
    uint32_t max_depth = 256;
    /// Token bumps allowed before giving up ("parser work budget exceeded");
    /// 0 = 64 * token count + 1M.
    uint64_t work_budget = 0;
    /// Strip `#!` / frontmatter (crate-root behaviour).
    bool strip_shebang_frontmatter = true;
};

struct Diagnostic {
    std::string message;
    Span span;
    uint32_t line = 0;  // 1-based, of span.lo
    uint32_t col = 0;   // 1-based, in characters
    /// true when rustc would never have reported it: it comes after a fatal
    /// lexer / delimiter error (rustc stops there; mep parses on for the LSP).
    bool after_fatal = false;
};

/// What parse_fragment parses.
enum class FragmentKind : uint8_t {
    Expr,      // one expression
    Ty,        // one type
    Pat,       // one pattern (top-level or-patterns allowed)
    Items,     // items until the end
    Stmts,     // block statements until the end (a block body without braces)
    ExprList,  // comma-separated expressions, trailing comma allowed
    Path,      // a path in type style
    Block,     // `{ ... }`
};

struct Fragment {
    FragmentKind kind = FragmentKind::Expr;
    Expr* expr = nullptr;            // Expr
    Ty* ty = nullptr;                // Ty
    Pat* pat = nullptr;              // Pat
    Path* path = nullptr;            // Path
    Block* block = nullptr;          // Block
    std::vector<Item*> items;        // Items
    std::vector<Stmt*> stmts;        // Stmts
    std::vector<Expr*> exprs;        // ExprList
    std::vector<Diagnostic> diagnostics;
    /// Token index where parsing stopped (== end when everything was consumed).
    uint32_t end_tok = 0;
    /// No diagnostics and every token consumed.
    bool ok = false;
};

class ParseResult {
public:
    /// The text that was parsed; every span and string_view points into it.
    std::unique_ptr<const std::string> source;
    Edition edition = Edition::E2021;
    std::vector<Token> tokens;       // ends with Eof; delimiters balanced
    std::vector<uint32_t> partner;   // delimiter partner indexes
    std::vector<Comment> comments;   // every comment, in order (doc comments too)
    Span shebang;
    Span frontmatter;
    Crate* crate = nullptr;
    /// All diagnostics in rustc's emission order; [0] is the one rustc prints first.
    std::vector<Diagnostic> diagnostics;
    /// rustc's verdict: true when it would report no error at all.
    bool ok() const { return diagnostics.empty(); }
    std::unique_ptr<Arena> arena;
    std::unique_ptr<LineIndex> lines;
    std::deque<std::string> interned;  // backing store for some token symbols
    ParseOptions options;

    std::string_view text(Span s) const { return std::string_view(*source).substr(s.lo, s.hi - s.lo); }
    LineCol line_col(uint32_t offset) const { return lines->at(offset); }
};

/// Parse a whole file as a crate root.
std::unique_ptr<ParseResult> parse_file(std::string text, const ParseOptions& opts = ParseOptions());

/// Parse tokens [tok_lo, tok_hi) of an already parsed file (a macro's
/// argument tokens, say) as `kind`. Nodes are allocated in `file.arena`;
/// spans are in the file's coordinates. With `prefix` the parse stops after
/// one node (Expr / Ty / Pat / Path) and `end_tok` says where, so a caller can
/// look at the separator that follows.
Fragment parse_fragment(ParseResult& file, uint32_t tok_lo, uint32_t tok_hi, FragmentKind kind,
                        bool prefix = false);

/// Lex and parse a standalone string as `kind` (the result owns the nodes;
/// `out` receives the fragment).
std::unique_ptr<ParseResult> parse_string_fragment(std::string text, FragmentKind kind, Fragment& out,
                                                   const ParseOptions& opts = ParseOptions());

/// An S-expression dump of the tree with spans (for tests and debugging).
std::string dump_crate(const ParseResult& r);
std::string dump_expr(const ParseResult& r, const Expr* e);

}  // namespace rust
