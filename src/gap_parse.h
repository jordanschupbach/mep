#ifndef MEP_GAP_PARSE_H
#define MEP_GAP_PARSE_H

// A GAP reader for mep's GAP language server and formatter (gap_lsp.cpp,
// gap_format.cpp). It makes the decisions GAP 4.15's own reader makes --
// the same tokens (scanner.c's numbers, escapes, keywords, the record
// component after `.`), the same grammar (read.c's statements, the
// expression levels and/and/not/relation/+-/*/mod/^ with its one-sign and
// non-associative `^` rules, selectors, permutations, ranges, records,
// lambdas, call options) and the same first syntax error, worded the same
// and at the same line -- so that `mep-gap-lsp --gap-check FILE` agrees
// with GAP's `ReadAsFunction` (tools/gap_parse_check.py measures it). It is
// written from the language's documentation and observed behaviour; no GAP
// code (GPL) is used.
//
// Two modes: a file GAP `Read`s (the default: bare expression statements,
// `quit;` and `?help` lines are allowed at the top level), and a function
// body, as `ReadAsFunction` reads a file (Options::as_function).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gapparse {

enum class T : uint8_t {
    Eof,
    Ident,
    Int,
    Float,
    String,
    Char,
    // keywords
    And, Atomic, Break, Continue, Do, Elif, Else, End, False, Fi, For, Function, If, In, Local, Mod, Not, Od,
    Or, Readonly, Readwrite, Rec, Repeat, Return, Then, True, Until, While, Quit, QQuit, IsBound, Unbind,
    TryNextMethod, Info, Assert,
    // punctuation
    Assign,      // :=
    Colon,       // :
    Eq, Ne, Lt, Le, Gt, Ge,
    Plus, Minus, Star, Slash, Caret,
    Tilde,       // ~
    Dot,         // .
    DotDot,      // ..
    DotDotDot,   // ...
    BangDot,     // !.
    BangBracket, // ![
    Arrow,       // ->
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma,
    Semi,        // ;
    DualSemi,    // ;;
    Help,        // ?topic (to the end of the line)
    Illegal,
};

/** @brief The spelling GAP uses for a token kind in "X expected" ("identifier" for T::Ident). */
const char *TokenName(T kind);
bool IsKeyword(T kind);

struct Token {
    T kind = T::Eof;
    uint32_t begin = 0, end = 0;  // byte offsets
    int line = 0, col = 0;        // 0-based line and byte column of `begin`
    std::string name;             // an identifier's name (escapes resolved); a keyword's spelling
    std::string error;            // a scanner error met reading this token (reported when it is reached)
    bool newline_before = false;  // a line break between the previous token and this one
};

struct Comment {
    uint32_t begin = 0, end = 0;
    int line = 0, col = 0;
    bool pragma = false;  // `#%`
};

/** @brief Scans @p text into tokens (always ending with an Eof token) and comments. */
void Tokenize(std::string_view text, std::vector<Token> *tokens, std::vector<Comment> *comments);

// --- Tree --------------------------------------------------------------------------

enum class K : uint8_t {
    File,
    // statements
    Assign, CallStat, ExprStat, If, Elif, Else, While, Repeat, For, Atomic, QualExpr, Break, Continue, Return,
    Empty, Unbind, Info, Assert, TryNext, Quit, Help, Locals,
    // expressions
    Ident, Int, Float, String, Char, True, False, Tilde, Binary, Unary, Not, Call, Option, Index, SubList,
    PosIndex, PosSubList, Field, CompField, List, Hole, Range, Rec, RecEntry, Perm, Cycle, IdPerm, Paren,
    Function, Lambda, Params, Param, IsBound,
};

const char *KindName(K kind);

/**
 * One node. `first`/`last` are inclusive token indices; `tok` is the node's own token (an identifier, a
 * literal, an operator, the keyword that starts a statement). Children, by kind:
 *   File: statements (a Locals first, for a ReadAsFunction-style file)
 *   Assign: target, value          CallStat/ExprStat: the expression
 *   If: cond, then-block..., Elif..., Else    (blocks are flattened: see `split`)
 *   Elif: cond, stmts...           Else: stmts...
 *   While: cond, stmts...          Repeat: stmts..., cond (the last child)
 *   For: var (Ident), values, stmts...        Atomic: QualExpr..., stmts...   (`split`)
 *   QualExpr: expr (tok = the qualifier or -1) Return: [expr]
 *   Unbind: operand    Info/Assert: args...    Locals: Ident...
 *   Binary: left, right (tok = operator)       Unary/Not: operand
 *   Call: callee, args..., Option...           Option: name[, value] (name: Ident or Paren)
 *   Index/PosIndex: object, index[, index2]    SubList/PosSubList: object, positions
 *   Field/CompField: object, name (Ident, Int or Paren)
 *   List: elements (Hole for an empty slot)    Range: first, [second,] last
 *   Rec: RecEntry...    RecEntry: name (Ident, Int or Paren), value
 *   Perm: Cycle...      Cycle: points...       Paren: expr
 *   Function: Params, [Locals], stmts... (`split` = index of the first statement)
 *   Lambda: Params, body expression            Params: Param...  (Param: tok = the name)
 *   IsBound: operand
 */
struct Node {
    K kind = K::File;
    int first = -1, last = -1;
    int tok = -1;
    std::vector<int> kids;
    int split = 0;          // If: kids before `split` are cond+then-block; For/Atomic/Function: first stmt
    bool variadic = false;  // Param: `x...` or the lone parameter `arg`; Params: the function is variadic
    int qualifier = -1;     // Param (atomic function): the readonly/readwrite token
    bool atomic = false;    // Function: `atomic function`
};

struct Error {
    std::string message;  // GAP's wording, without "Syntax error: "
    int line = 0, col = 0;  // 0-based; the token GAP was looking at
    uint32_t offset = 0;
    bool at_eof = false;
};

struct Options {
    bool as_function = false;  // ReadAsFunction: the file is a function body
};

struct Parse {
    std::string_view text;
    std::vector<Token> tokens;
    std::vector<Comment> comments;
    std::vector<Node> nodes;  // nodes[0] is the File
    std::vector<Error> errors;  // errors[0] is the one GAP reports first
    std::vector<int> top_exprs;  // top-level ExprStat nodes (bare expressions)

    const Node &N(int i) const { return nodes[static_cast<size_t>(i)]; }
    const Token &Tok(int i) const { return tokens[static_cast<size_t>(i)]; }
    std::string_view Spelling(int tok) const {
        const Token &t = Tok(tok);
        return text.substr(t.begin, t.end - t.begin);
    }
};

/** @brief Reads @p text. Never throws; a syntax error ends the statement it is in and reading resumes at the next. */
Parse ParseGap(std::string_view text, const Options &opt = {});

/** @brief GAP's own verdict as `--gap-check` prints it: "ok", or `LINE:COL: message` (1-based). By default
 *  as ReadAsFunction reads the file (a function body); @p as_function false reads it as a file GAP `Read`s. */
std::string GapCheck(std::string_view text, bool as_function = true);

constexpr int kMaxNesting = 400;  // deeper than this is refused (GAP's own C stack ends much later)

}  // namespace gapparse

#endif  // MEP_GAP_PARSE_H
