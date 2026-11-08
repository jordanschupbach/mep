#pragma once

// The Lua 5.4 parser behind mep-lua-lsp (and the tree mep's Lua formatter
// prints). It follows Lua's own llex.c and lparser.c decision for decision,
// so that what it accepts, and the first error it reports on what it does
// not, are `luac -p`'s -- same message, same "near" token, same line:
//
//   - the lexer reads numerals, escapes, long brackets and line breaks as
//     llex.c does, and shows a token in a message the way Lua's scanner
//     buffer holds it ("near '\"abc'" for an unfinished string);
//   - the parser makes the same checks at the same moments: `goto` and
//     labels, `<const>`/`<close>`, `...` outside a vararg function, the
//     200-locals / 255-upvalues limits, assignment to a call ("syntax
//     error"), and reports a line as `ls->linenumber` would be then.
//
// It also resolves every name the way the compiler does (locals, upvalues
// through enclosing functions, else a global), which is what the server's
// lints and navigation are built on.
//
// Unlike luac it does not stop at the first error: after a syntax error it
// skips to the next statement and goes on, so an editor keeps an outline,
// completion and navigation in a half-typed file. Only the first error is
// Lua's; ParseResult::first_error says where that was.
//
// Offsets are bytes into the source; lines are 1-based like Lua's.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace luaparse {

struct Node;
struct Var;

enum class Kind : unsigned char {
    // expressions
    Nil, True, False, Vararg, Number, String, Function, Table, Binary, Unary, Paren,
    Name,    // a variable: `var` is the local it resolves to, null for a global
    Index,   // obj[key]
    Field,   // obj.name
    Call,    // fn(args) / fn"s" / fn{...}; method: obj:name(args)
    // statements
    Local, Assign, CallStat, Do, While, Repeat, If, NumFor, GenFor, FunctionStat, LocalFunction,
    Return, Break, Goto, Label, Empty,
    Error,   // what was skipped after a syntax error
};

enum class ArgStyle : unsigned char { Parens, String, Table };

struct TableField {
    enum KindT : unsigned char { Positional, Named, Keyed } kind = Positional;
    std::string name;             // Named
    int name_start = 0, name_end = 0;
    Node *key = nullptr;          // Keyed: [key]
    Node *value = nullptr;
    int start = 0, end = 0;
    char sep = 0;                 // the ',' or ';' after the field, 0 for none
};

struct Block {
    std::vector<Node *> stats;
    int start = 0, end = 0;
};

struct FuncBody {
    std::vector<Var *> params;    // includes the implicit `self` of a method first
    bool is_method = false;
    bool vararg = false;
    int open_paren = 0, close_paren = 0;
    Block body;
    int end_kw = -1;              // the `end` offset
    int line = 0, last_line = 0;  // linedefined / lastlinedefined
};

struct IfClause {
    Node *cond = nullptr;         // null for `else`
    Block body;
    int kw = 0;                   // offset of if/elseif/else
};

struct Node {
    Kind kind = Kind::Error;
    int start = 0, end = 0;       // byte range
    int line = 0;                 // line of the first token
    std::string text;             // Name: the name; Number: as written; String: as written; Binary/Unary: operator;
                                  // Field/method Call: the field name; Goto/Label: the label
    std::string value;            // String: the value (escapes processed)
    int name_start = 0, name_end = 0;  // Field / method Call / Goto / Label: where the name is written
    Var *var = nullptr;           // Name: its local (null: global)
    bool is_write = false;        // Name: an assignment target
    bool custom_env = false;      // Name (global): a local `_ENV` is in scope, so it indexes that table

    Node *a = nullptr;            // Binary lhs / Unary operand / Paren inner / Index,Field,Call object / NumFor start
    Node *b = nullptr;            // Binary rhs / Index key / NumFor limit / Repeat cond / While cond
    Node *c = nullptr;            // NumFor step
    std::vector<Node *> list;     // Call args / Local,Assign,Return,GenFor exprs
    std::vector<Node *> targets;  // Assign targets; FunctionStat: the name path (Name then Fields)
    std::vector<Var *> vars;      // Local / NumFor / GenFor / LocalFunction
    ArgStyle args = ArgStyle::Parens;
    bool method = false;          // Call with ':'; FunctionStat `a:b`
    std::vector<TableField> fields;
    FuncBody *func = nullptr;     // Function / FunctionStat / LocalFunction
    Block block;                  // Do / While / Repeat / NumFor / GenFor body
    std::vector<IfClause> clauses;
    bool semicolon = false;       // Return followed by ';'
    int open = -1, close = -1;    // Paren / Table / Call-paren / Index brackets offsets
};

struct Var {
    enum KindT : unsigned char { Local, Param, Self, ForVar, LocalFunction } kind = Local;
    enum Attrib : unsigned char { None, Const, Close } attrib = None;
    std::string name;
    int start = 0, end = 0;       // where it is declared (Self: the method name's span)
    Node *decl = nullptr;         // the Local / LocalFunction / For / Function node that declares it
    FuncBody *owner = nullptr;    // the function whose local it is (null: the main chunk)
    std::vector<Node *> reads;
    std::vector<Node *> writes;   // assignments after the declaration
    std::vector<Node *> upvalue_uses;  // reads/writes from inside a nested function
    int scope_start = 0, scope_end = 0;  // where the name is visible
    Var *shadows = nullptr;       // an outer variable of the same name visible at the declaration
};

struct Comment {
    int start = 0, end = 0;
    bool block = false;           // --[[ ]]
    int line = 0;
};

struct Error {
    int line = 0;                 // Lua's line
    int start = 0, end = 0;       // the range to underline
    std::string message;          // without "file:line: "
    std::string code;             // syntax-error, goto, const-assign, limit, ...
};

struct ParseResult {
    Block chunk;
    std::vector<Error> errors;    // errors[0] (when present) is what luac reports; later ones are guesses
    bool has_error = false;
    int first_error_offset = -1;  // where Lua stopped: analysis after this point is on a repaired tree
    std::vector<Comment> comments;
    std::vector<Var *> vars;      // every local, in declaration order
    std::vector<Node *> globals;  // every Name that resolved to a global
    std::vector<std::unique_ptr<Node>> node_arena;
    std::vector<std::unique_ptr<Var>> var_arena;
    std::vector<std::unique_ptr<FuncBody>> func_arena;
    int shebang_end = 0;          // a first line starting with '#' (skipped, as luaL_loadfile does)
};

/** @brief Parses a Lua chunk. Never fails: errors are in the result. */
std::unique_ptr<ParseResult> Parse(std::string_view src);

/** @brief "file:line: message", as luac prints it after "luac: ". */
std::string FormatError(const std::string &chunkname, const Error &e);

bool IsReserved(std::string_view s);

}  // namespace luaparse
