#ifndef MEP_R_LSP_PARSE_H
#define MEP_R_LSP_PARSE_H

#include <string>
#include <vector>

// The R reader behind mep's own R language server: a hand-written lexer
// and recursive-descent parser producing a small AST, plus the syntax
// diagnostics found on the way. r_lsp.cpp turns that AST into
// diagnostics/completion/hover/symbols; r_lsp_server.cpp puts it on the
// wire as `mep-r-lsp`.
//
// Hand-written rather than tree-sitter-r (which mep does vendor, for
// highlighting) for two reasons that matter to a *server* specifically:
// a parse error here carries the token it choked on and the construct it
// was in, so the message can say "expected ')' to close the call opened
// at line 4" instead of pointing at an anonymous ERROR node; and the
// binary stays dependency-free the same way `mep-org-lsp` is, so the
// analysis half is a pure function over lines that the test suite drives
// directly.
//
// Scope, named rather than silently omitted:
//   - This parses R *source*, not R semantics. No evaluation, no types,
//     no S4/R6 class model beyond recognizing the calls that declare one.
//   - Deparsing is not round-trippable: the AST keeps every construct's
//     source range, but comments live in a separate list (RParseResult::
//     comments) rather than being attached to the nodes around them.
//   - `...`-forwarding, `assign()`, `get()`, `eval()` and friends are
//     recognized syntactically where that helps a check, never followed.
//
// Positions are 0-based lines and 0-based *byte* columns throughout, the
// same convention org_lsp.h documents and for the same reason: mep's LSP
// client reads `character` as a byte column.

// --- Source ranges ----------------------------------------------------

struct RRange {
    int line = 0;      // 0-based start line
    int col = 0;       // 0-based start byte column
    int end_line = 0;  // 0-based end line
    int end_col = 0;   // 0-based end byte column, exclusive
};

/** @brief Reports whether a (line, col) position lies inside a range, end-exclusive. */
bool RRangeContains(const RRange &r, int line, int col);

// --- Tokens -----------------------------------------------------------

enum class RTokKind {
    End,      // end of input
    Ident,    // a name, possibly `backticked`
    Num,      // numeric literal, including the 1L / 1i / 0x1f forms
    Str,      // string literal, including the r"(raw)" forms
    True,     // TRUE (the reserved word, not the rebindable `T`)
    False,    // FALSE
    Null,     // NULL
    Na,       // NA and its typed NA_integer_ / NA_real_ / NA_character_ forms
    Inf,      // Inf
    Nan,      // NaN
    If,
    Else,
    For,
    While,
    Repeat,
    Function,  // `function` and the `\(x)` shorthand
    Break,
    Next,
    In,
    Dots,      // ... and ..1, ..2
    Op,        // every operator, spelled out in `text`
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,     // [
    DblLBracket,  // [[ (only when the two brackets are adjacent, as R lexes it)
    RBracket,     // ] -- `]]` is always two of these, so x[[y[1]]] nests correctly
    Comma,
    Semi,
    Newline,  // a statement separator: suppressed inside ( ) and [ ], kept inside { }
};

struct RToken {
    RTokKind kind = RTokKind::End;
    // Identifiers: the name with any backticks stripped. Strings: the
    // decoded value. Numbers and operators: exactly as written.
    std::string text;
    RRange range;
    bool backticked = false;  // `x y` -- a name that is not word-shaped
    bool unterminated = false;  // a string literal that ran to end of line/input
};

struct RComment {
    std::string text;  // the whole comment including its leading '#'
    RRange range;
    bool roxygen = false;  // starts with #' -- documentation, not a remark
};

// --- AST --------------------------------------------------------------

enum class RNodeKind {
    Error,    // a construct that failed to parse; kids hold whatever was salvaged
    Num,
    Str,
    Bool,     // text is "TRUE" or "FALSE"
    Null,
    Na,       // text distinguishes NA / NA_integer_ / NA_real_ / NA_character_
    Inf,
    Nan,
    Ident,    // text is the name
    Dots,     // ... / ..1
    Missing,  // an empty argument slot: x[, 1] or f(a, )
    Call,     // kids[0] is the callee; args are the arguments
    Index,    // text is "[" or "[["; kids[0] is the object, args the subscripts
    Function,  // args are the formals (value = default or -1); kids[0] is the body
    If,        // kids = {cond, then} or {cond, then, else}
    For,       // kids = {var (an Ident), seq, body}
    While,     // kids = {cond, body}
    Repeat,    // kids = {body}
    Break,
    Next,
    Block,   // { ... }: kids are the statements
    Paren,   // ( expr ): kids = {expr}
    Binary,  // text is the operator, kids = {lhs, rhs}
    Unary,   // text is the operator, kids = {operand}
};

// One argument of a call or subscript, or one formal of a function.
struct RArg {
    std::string name;   // "" for a positional argument
    RRange name_range;  // where the name itself was written (only if named)
    int value = -1;     // node index, or -1 for a missing/empty slot
};

struct RNode {
    RNodeKind kind = RNodeKind::Error;
    RRange range;
    std::string text;
    std::vector<int> kids;
    std::vector<RArg> args;
};

// --- Parse errors -----------------------------------------------------

struct RParseError {
    RRange range;
    std::string code;     // stable id: "syntax-error", "unclosed-paren", ...
    std::string message;  // human wording, free to improve
};

struct RParseResult {
    std::vector<RNode> nodes;   // the arena; indices above refer into this
    std::vector<int> roots;     // top-level expressions, in document order
    std::vector<RComment> comments;
    std::vector<RParseError> errors;

    /** @brief Reports whether the document parsed cleanly. */
    bool ok() const { return errors.empty(); }
    /** @brief Borrows a node by index (index -1 or out of range yields a shared empty node). */
    const RNode &at(int index) const;
};

/**
 * @brief Tokenizes R source, collecting comments separately from the token stream.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param comments_out receives every comment line found, in document order (may be null)
 * @return the token stream, always ending in an RTokKind::End token
 */
std::vector<RToken> RLspTokenize(const std::vector<std::string> &lines, std::vector<RComment> *comments_out);

/**
 * @brief Parses R source into an AST, recovering at statement boundaries so one bad line does not lose the file.
 * @param lines the document's text, one entry per line
 * @return the arena, the top-level expression indices, the comments and every syntax error found
 */
RParseResult RLspParse(const std::vector<std::string> &lines);

#endif  // MEP_R_LSP_PARSE_H
