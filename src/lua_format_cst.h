#pragma once

// The concrete syntax tree behind mep's Lua formatter (lua_format.h): every
// token keeps its leading and trailing trivia (whitespace and comments), the
// way full-moon -- the parser StyLua formats with -- attaches them: a token's
// trailing trivia runs to the end of its line (through the newline), and
// everything else before the next token is that token's leading trivia.
// The node shapes follow full-moon's AST too (Var / FunctionCall with
// prefix + suffixes, Punctuated lists, ContainedSpan brackets) because
// StyLua's layout rules are written against them.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luafmt::cst {

enum class TT { Whitespace, SingleLineComment, MultiLineComment, Shebang, Number, String, Identifier, Symbol, Eof };

struct Token {
    TT type = TT::Whitespace;
    std::string text;   // whitespace chars, comment body, number text, string body, name, symbol
    int blocks = 0;     // `=` count of a long comment / long string
    char quote = 0;     // '"', '\'', or '[' for a long string
    size_t start = 0;   // byte offsets in the source (0 for tokens the formatter made)
    size_t end = 0;

    bool IsTrivia() const {
        return type == TT::Whitespace || type == TT::SingleLineComment || type == TT::MultiLineComment ||
               type == TT::Shebang;
    }
    bool IsComment() const { return type == TT::SingleLineComment || type == TT::MultiLineComment; }
    bool IsSymbol(std::string_view s) const { return type == TT::Symbol && text == s; }
};

std::string TokenText(const Token &t);
void AppendTokenText(std::string &out, const Token &t);

struct TokenRef {
    std::vector<Token> leading;
    Token tok;
    std::vector<Token> trailing;
};

/** @brief A copyable owning pointer (Rust's Box): copies deep-copy the pointee. */
template <class T>
class Box {
  public:
    Box() = default;
    Box(T v) : p_(std::make_unique<T>(std::move(v))) {}  // NOLINT: implicit like Box::new
    Box(const Box &o) : p_(o.p_ ? std::make_unique<T>(*o.p_) : nullptr) {}
    Box(Box &&o) noexcept = default;
    Box &operator=(const Box &o) {
        if (this != &o) p_ = o.p_ ? std::make_unique<T>(*o.p_) : nullptr;
        return *this;
    }
    Box &operator=(Box &&o) noexcept = default;
    ~Box() = default;
    T &operator*() { return *p_; }
    const T &operator*() const { return *p_; }
    T *operator->() { return p_.get(); }
    const T *operator->() const { return p_.get(); }
    explicit operator bool() const { return p_ != nullptr; }

  private:
    std::unique_ptr<T> p_;
};

template <class T>
struct Pair {
    T value;
    std::optional<TokenRef> punct;
};

template <class T>
struct Punctuated {
    std::vector<Pair<T>> pairs;
    size_t size() const { return pairs.size(); }
    bool empty() const { return pairs.empty(); }
    void push(T v, std::optional<TokenRef> p = std::nullopt) { pairs.push_back(Pair<T>{std::move(v), std::move(p)}); }
};

struct ContainedSpan {
    TokenRef open;
    TokenRef close;
};

struct Expr;
struct Block;
struct FunctionCall;
struct TableConstructor;
struct FunctionBody;
struct Stmt;

enum class EK { Binary, Paren, Unary, Function, Call, Table, Number, String, Symbol, Var };

struct AnonFn {
    TokenRef function_token;
    Box<FunctionBody> body;
};

struct Prefix {
    bool is_name = true;
    TokenRef name;
    Box<Expr> expr;  // a Parentheses expression
};

struct FunctionArgs {
    enum Kind { Parens, String, Table } kind = Parens;
    ContainedSpan parens;
    Box<Punctuated<Expr>> args;
    TokenRef str;
    Box<TableConstructor> table;
};

struct Index {
    bool is_dot = true;
    TokenRef dot, name;      // Dot
    ContainedSpan brackets;  // Brackets
    Box<Expr> expr;
};

struct Suffix {
    enum Kind { kAnon, kMethod, kIndex } kind = kAnon;
    FunctionArgs args;              // kAnon, kMethod
    TokenRef colon, method_name;    // kMethod
    Index index;                    // kIndex
    bool IsCall() const { return kind != kIndex; }
};

// A FunctionCall or VarExpression: prefix then suffixes.
struct FunctionCall {
    Prefix prefix;
    std::vector<Suffix> suffixes;
};

struct Var {
    bool is_name = true;
    TokenRef name;
    Box<FunctionCall> expr;  // VarExpression
};

struct Expr {
    EK kind = EK::Symbol;
    TokenRef tok;            // Number/String/Symbol token, the operator of Binary/Unary
    Box<Expr> lhs, rhs;      // Binary lhs/rhs; Unary and Paren keep their operand in rhs
    ContainedSpan contained; // Paren
    Box<AnonFn> func;
    Box<FunctionCall> call;
    Box<TableConstructor> table;
    Box<Var> var;
};

struct Field {
    enum Kind { ExprKey, NameKey, NoKey } kind = NoKey;
    ContainedSpan brackets;  // ExprKey
    Box<Expr> key;           // ExprKey
    TokenRef name;           // NameKey
    TokenRef equal;
    Box<Expr> value;         // value (NoKey: the expression)
};

struct TableConstructor {
    ContainedSpan braces;
    Punctuated<Field> fields;
};

struct StmtSemi;

struct LastStmt {
    bool is_break = false;
    TokenRef token;              // `break` / `return`
    Punctuated<Expr> returns;
};

struct Block {
    std::vector<StmtSemi> stmts;
    Box<std::pair<LastStmt, std::optional<TokenRef>>> last;
};

struct FunctionBody {
    ContainedSpan parens;
    Punctuated<TokenRef> params;
    Block block;
    TokenRef end_token;
};

struct Attribute {
    ContainedSpan brackets;
    TokenRef name;
};

struct ElseIf {
    TokenRef elseif_token;
    Expr cond;
    TokenRef then_token;
    Block block;
};

struct FunctionName {
    Punctuated<TokenRef> names;
    std::optional<std::pair<TokenRef, TokenRef>> method;  // colon, name
};

enum class SK { Assignment, Do, Call, FunctionDecl, GenericFor, If, Local, LocalFunction, NumericFor, Repeat, While, Goto, Label };

struct Stmt {
    SK kind = SK::Call;
    // Assignment
    Punctuated<Var> vars;
    // Assignment, Local, NumericFor `=`
    std::optional<TokenRef> equal;
    // Assignment, Local, GenericFor expressions
    Punctuated<Expr> exprs;
    // Local / LocalFunction `local`
    TokenRef local_token;
    // Local names, GenericFor names
    Punctuated<TokenRef> names;
    std::vector<std::optional<Attribute>> attributes;
    // Call
    Box<FunctionCall> call;
    // Do, While, NumericFor, GenericFor, If, Repeat bodies
    Box<Block> block;
    // first keyword: do / for / if / repeat / while / function / goto / `::`
    TokenRef first;
    TokenRef end_token;  // end / `::` (Label right colons) / until (Repeat)
    // While / If / Repeat condition, NumericFor start
    Box<Expr> cond;
    TokenRef do_token;   // do / then / in (GenericFor in) ; NumericFor `do`
    TokenRef in_token;
    // NumericFor
    TokenRef index_var;
    TokenRef comma1;
    Box<Expr> limit;
    std::optional<TokenRef> comma2;
    Box<Expr> step;
    // If
    std::vector<ElseIf> elseifs;
    std::optional<TokenRef> else_token;
    Box<Block> else_block;
    // FunctionDecl / LocalFunction
    FunctionName fname;
    TokenRef name;       // LocalFunction name, Goto/Label name
    TokenRef function_token;  // LocalFunction `function`
    Box<FunctionBody> body;
};

struct StmtSemi {
    Stmt stmt;
    std::optional<TokenRef> semi;
};

struct Ast {
    Block block;
    TokenRef eof;
};

struct ParseError {
    std::string message;
    int line = 0;
};

/** @brief Lexes and parses Lua (5.1-5.4 plus the shared extensions StyLua accepts). */
bool Parse(std::string_view src, Ast &out, ParseError &err);

/** @brief Lexes `src` into tokens (trivia included); false on a lexing error. */
bool Lex(std::string_view src, std::vector<Token> &out, ParseError &err);

// ---- printing -------------------------------------------------------------
std::string Print(const TokenRef &t);
std::string Print(const Expr &e);
std::string Print(const Block &b);
std::string Print(const Stmt &s);
std::string Print(const LastStmt &s);
std::string Print(const Suffix &s);
std::string Print(const Prefix &p);
std::string Print(const Var &v);
std::string Print(const FunctionCall &c);
std::string Print(const Field &f);
std::string Print(const TableConstructor &t);
std::string Print(const FunctionArgs &a);
std::string Print(const FunctionBody &b);
std::string Print(const Attribute &a);
std::string Print(const FunctionName &n);
std::string Print(const Punctuated<Expr> &p);
std::string Print(const Punctuated<Var> &p);
std::string Print(const Punctuated<TokenRef> &p);
std::string Print(const ContainedSpan &c);
std::string Print(const Ast &a);

}  // namespace luafmt::cst
