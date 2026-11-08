#pragma once

// The Nix language parser behind mep-nix-lsp: Nix's own lexer rules and
// grammar, so that what it accepts, and the first error it reports on what
// it does not, are `nix-instantiate --parse`'s.
//
//   - The lexer follows libexpr's lexer.l rule for rule (longest match,
//     first rule on a tie; the string, indented-string and path states; the
//     byte offsets each token is given, including where a `yyless(0)`
//     rule puts them back).
//   - The parser runs LALR(1) tables built exactly as bison builds Nix's
//     parser.y (tools/nix_lalr.py -> nix_parse_tables.h), so a syntax error
//     is noticed at the same token and worded the same: "syntax error,
//     unexpected '}', expecting ';'".
//   - The checks Nix makes while parsing -- duplicate attributes (with its
//     attribute-set merging), duplicate formal arguments, dynamic attributes
//     in `let` and `inherit`, out-of-range numbers, a path's trailing slash,
//     the pipe operators' experimental feature -- run at the same reductions,
//     so the first error recorded is the one Nix stops at.
//
// Unlike Nix it does not stop there: every error is recorded, and after a
// syntax error the parser repairs the input (inserting or skipping a token,
// checked against the tokens that follow) and carries on, so an editor gets a
// tree to complete and navigate in while the user is typing. `undefined
// variable` -- the one error Nix reports after parsing -- comes from the
// scope analysis in nix_lsp.cpp.
//
// The tree mirrors Nix's own expressions (nixexpr.hh): an attribute path
// `a.b.c = 1;` becomes nested attribute sets, and a later `a.d = 2;` or
// `a = { ... };` merges into them as Nix merges, which is what decides
// duplicates and, for a `rec` set, scoping. The syntax as written is kept
// beside it (Node::bindings) for the outline and navigation.
//
// Offsets are bytes into the source.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nixparse {

struct Node;

enum class NodeKind : unsigned char {
    Var,         // name
    CurPos,      // __curPos
    Int,         // text
    Float,       // text
    String,      // "..." -- kids: interpolations; str_static/str_value when it has none
    IndString,   // ''...'' -- likewise
    Path,        // ./a, ~/a, a/b -- kids: interpolations
    SPath,       // <nixpkgs> (text without the brackets)
    Uri,         // http://... (text)
    Attrs,       // { ... } / rec { ... } / let { ... } -- attrs, dynamic_attrs, inherit_from, bindings
    List,        // kids
    Select,      // e.path or def
    HasAttr,     // e ? path
    Call,        // kids[0] applied to kids[1..]
    Lambda,      // arg / formals, body
    Let,         // attrs + body (a recursive scope)
    With,        // kids[0] = the attribute set, body
    Assert,      // kids[0], body
    If,          // kids = cond, then, else
    Not,         // kids[0]
    Negate,      // kids[0]
    BinOp,       // kids = lhs, rhs; op = the operator token
    Pipe,        // kids = lhs, rhs; op = `|>` or `<|`
    Error,       // what was left when error recovery gave up: kids are the fragments
};

/** @brief One element of an attribute path: a name, or a dynamic one (`${e}` / an interpolated string). */
struct AttrName {
    std::string name;         // static name (after unescaping)
    bool dynamic = false;
    Node *expr = nullptr;     // dynamic: the name's expression
    int start = 0, end = 0;   // where the element is written
    bool missing = false;     // inserted by error recovery
};

/** @brief An attribute of a set or `let`, as Nix's ExprAttrs keeps it (after merging). */
struct AttrDef {
    enum Kind : unsigned char { Plain, Inherited, InheritedFrom };
    std::string name;
    Node *value = nullptr;    // Inherited: a Var at the name; InheritedFrom: nullptr (the value is from.name)
    Kind kind = Plain;
    int pos = 0;              // Nix's position: the binding's start (Plain), the name (inherited)
    int name_start = 0, name_end = 0;  // where this name is written (first occurrence)
    int from = -1;            // InheritedFrom: index into the owning set's inherit_from
};

struct DynamicAttrDef {
    Node *name_expr = nullptr;
    Node *value = nullptr;
    int pos = 0;
};

/** @brief A binding as written in a set or `let` (the syntax; the merged meaning is in Node::attrs). */
struct Binding {
    enum Kind : unsigned char { Assign, Inherit, InheritFrom };
    Kind kind = Assign;
    std::vector<AttrName> path;   // Assign: the attribute path; Inherit/InheritFrom: the names
    Node *value = nullptr;        // Assign
    Node *from = nullptr;         // InheritFrom: the `(expr)`
    int start = 0, end = 0;       // the whole binding, through its `;`
};

struct Formal {
    std::string name;
    int start = 0, end = 0;
    Node *def = nullptr;  // `? default`
};

struct Node {
    NodeKind kind = NodeKind::Error;
    int start = 0, end = 0;
    std::string name;   // Var
    std::string text;   // Int, Float, Path (as written, first segment), SPath, Uri; BinOp/Pipe: the operator
    bool missing = false;  // a Var inserted by error recovery

    std::vector<Node *> kids;

    // String / IndString: a literal without interpolation, and its value (what
    // makes `"a"` or `${"a"}` a static attribute name).
    bool str_static = false;
    std::string str_value;

    // Attrs / Let
    bool recursive = false;
    bool old_let = false;    // `let { ... }`
    bool implicit = false;   // created by an attribute path (`a.b = 1;`), not written as `{ }`
    std::vector<AttrDef> attrs;
    std::vector<DynamicAttrDef> dynamic_attrs;
    std::vector<Node *> inherit_from;
    std::vector<Binding> bindings;
    int open = -1, close = -1;  // the braces' offsets (Attrs written with braces, Let's `let`/`in`)

    // Select / HasAttr
    Node *e = nullptr;
    std::vector<AttrName> path;
    Node *def = nullptr;   // Select: `or` default

    // Lambda
    std::string arg;        // `x:` or `x @ {...}:` / `{...} @ x:`
    int arg_start = -1, arg_end = -1;
    bool has_formals = false;
    bool ellipsis = false;
    std::vector<Formal> formals;
    int formals_start = -1, formals_end = -1;

    // Lambda / Let / With / Assert
    Node *body = nullptr;

    // Call made by `expr_simple or` (Nix's "cursed or"): where the `or` ends.
    int cursed_or_end = -1;
    // Var: where Nix places it when that is not its start -- the `or` of a
    // cursed or is placed at the start of the expression before it.
    int nix_pos = -1;
};

struct Diagnostic {
    int start = 0, end = 0;  // the range to underline (byte offsets)
    int nix_offset = 0;      // where Nix reports it
    std::string code;        // syntax-error, duplicate-attribute, ...
    std::string message;     // Nix's wording
    bool syntax = false;     // a syntax error (only the first is recorded)
};

struct Comment {
    int start = 0, end = 0;
    bool block = false;  // /* */
    bool doc = false;    // /** */
};

struct Options {
    std::string origin = "«string»";  // the file name Nix would print in "already defined at FILE:L:C"
    bool pipe_operators = false;       // the experimental feature (off in stock Nix)
};

struct ParseResult {
    Node *root = nullptr;
    // In the order Nix would hit them; errors[0] is what Nix reports (when it
    // is not an undefined variable, which nix_lsp.cpp finds after parsing).
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;   // the "cursed or"
    bool has_syntax_error = false;
    int syntax_error_offset = -1;
    bool recovered = true;              // false: recovery gave up; root is an Error node of fragments
    std::vector<Comment> comments;
    std::vector<std::unique_ptr<Node>> arena;
};

/** @brief Parses Nix source. Never fails: errors are in the result, the tree is repaired. */
std::unique_ptr<ParseResult> Parse(std::string_view src, const Options &opts = {});

/** @brief Nix's line and column (1-based; columns in bytes; `\r\n`, `\r` and `\n` all end a line). */
void NixLineCol(std::string_view src, int offset, int *line, int *col);

/** @brief True when `s` is an identifier Nix would read as one name (and not a keyword). */
bool IsNixIdentifier(std::string_view s);

}  // namespace nixparse
