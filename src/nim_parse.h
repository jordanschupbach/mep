#ifndef MEP_NIM_PARSE_H
#define MEP_NIM_PARSE_H

// mep's Nim lexer and parser: a port of the Nim 2.2 compiler's own
// compiler/lexer.nim, nimlexbase.nim and parser.nim (MIT, (c) Andreas
// Rumpf; see copying.txt in Nim's sources), procedure for procedure, so a
// syntax error carries the compiler's message at its line and column and
// the tree is the compiler's PNode tree (TNodeKind names below).
// `just nim-parse-check` compares every error with Nim's own parser
// (tools/ts_grammar_diff/nim_check.nim) over the corpus and damaged copies.
//
// Two modes, as the compiler has two builds:
//   - the compiler's (default): comments are kept on nodes;
//   - nimpretty's (`Options::nimpretty`): the lexer also returns ordinary
//     `#` comments, the parser skips every comment token (so they never
//     reach the tree) and reports each token, and its own layout hints, to
//     an Emitter (nim_format.cpp's port of compiler/layouter.nim).
//
// One difference by design: the compiler recurses without a limit; this
// parser stops at Options::max_nest levels of nesting with an error, since
// it runs on a fixed C++ stack inside the editor.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nimparse {

// lexer.nim's TokType, in its order (the keyword range matches wordrecg).
enum class Tok : uint8_t {
    Invalid, Eof, Symbol,
    Addr, And, As, Asm, Bind, Block, Break, Case, Cast, Concept, Const, Continue, Converter, Defer, Discard,
    Distinct, Div, Do, Elif, Else, End, Enum, Except, Export, Finally, For, From, Func, If, Import, In, Include,
    Interface, Is, Isnot, Iterator, Let, Macro, Method, Mixin, Mod, Nil, Not, Notin, Object, Of, Or, Out, Proc,
    Ptr, Raise, Ref, Return, Shl, Shr, Static, Template, Try, Tuple, Type, Using, Var, When, While, Xor, Yield,
    IntLit, Int8Lit, Int16Lit, Int32Lit, Int64Lit, UIntLit, UInt8Lit, UInt16Lit, UInt32Lit, UInt64Lit, FloatLit,
    Float32Lit, Float64Lit, Float128Lit, StrLit, RStrLit, TripleStrLit, GStrLit, GTripleStrLit, CharLit, CustomLit,
    ParLe, ParRi, BracketLe, BracketRi, CurlyLe, CurlyRi, BracketDotLe, BracketDotRi, CurlyDotLe, CurlyDotRi,
    ParDotLe, ParDotRi, Comma, SemiColon, Colon, ColonColon, Equals, Dot, DotDot, BracketLeColon, Opr, Comment,
    Accent,
};

constexpr bool IsKeyword(Tok t) { return t >= Tok::Addr && t <= Tok::Yield; }
constexpr bool InRange(Tok t, Tok a, Tok b) { return t >= a && t <= b; }
/** @brief `$tokType`: a keyword's or punctuation's spelling, else the enum's name ("tkSymbol"). */
const char *TokName(Tok t);

enum Spacing : uint8_t { kLeading = 1, kTrailing = 2, kSpacingEof = 4 };

struct Token {
    Tok type = Tok::Invalid;
    int base = 0;          // 0 base10, 1 base2, 2 base8, 3 base16
    uint8_t spacing = 0;   // Spacing bits
    int indent = -1;       // >= 0 when the token starts a line
    std::string ident;     // the identifier's spelling as the ident cache knows it (first seen)
    bool has_ident = false;
    int64_t i_number = 0;
    double f_number = 0;
    std::string literal;
    int line = 0, col = 0;  // 1-based line, 0-based byte column (TLineInfo)
    // nimpretty's offsets into the source (inclusive ends).
    int offset_a = 0, offset_b = 0, comment_offset_a = 0, comment_offset_b = 0;
    // mep: the token's bytes in the source, [start, end).
    int start = 0, end = 0;
};

struct Diag {
    int line = 0;  // 1-based
    int col = 0;   // 0-based byte column
    std::string msg;
    bool warning = false;
};

/** @brief idents.nim's cache: style-insensitive (first character exact) names, first spelling wins. */
class IdentCache {
public:
    IdentCache();
    /** @brief The spelling of `s` (the first one seen); `id` is a wordrecg index or < 0. */
    const std::string &Get(std::string_view s, int *id = nullptr);
    static std::string Normalize(std::string_view s);

private:
    struct Entry {
        std::string spelling;
        int id;
    };
    std::unordered_map<std::string, Entry> map_;
    int counter_ = 0;
};

/** @brief cmpIgnoreStyle(a, b) == 0. */
bool SameIdent(std::string_view a, std::string_view b);

class Emitter;  // nimpretty's layouter (nim_format.cpp)

class Lexer {
public:
    Lexer(std::string_view src, IdentCache *cache, std::vector<Diag> *diags, bool nimpretty);
    void RawGetTok(Token &tok);

    std::string buf;  // the source, '\0'-terminated (and padded): '\0' is the end of the file
    int bufpos = 0;
    int line_start = 0;
    int line_number = 1;
    int indent_ahead = -1;
    int curr_line_indent = 0;
    bool nimpretty = false;
    IdentCache *cache;
    std::vector<Diag> *diags;
    int src_len = 0;

    int ColNumber(int pos) const { return pos > line_start ? pos - line_start : line_start - pos; }
    void Message(const std::string &msg, bool warning = false);
    void MessageTok(const Token &tok, const std::string &msg, bool warning = false);
    void MessagePos(int pos, const std::string &msg);

private:
    int HandleCRLF(int pos);
    void GetNumber(Token &tok);
    void HandleHexChar(int &xi, int position);
    void GetEscapedChar(Token &tok);
    void GetString(Token &tok, int mode);
    void GetCharacter(Token &tok);
    void GetSymbol(Token &tok);
    void EndOperator(Token &tok, int pos);
    void GetOperator(Token &tok);
    void SkipMultiLineComment(Token &tok, int start, bool is_doc);
    void ScanComment(Token &tok);
    void Skip(Token &tok);
    void LexMessageLitNum(const std::string &msg, int startpos, bool warning = false);
};

/** @brief lexer.nim's getPrecedence. */
int Precedence(const Token &tok);
/** @brief unicodeOprLen: the byte length of a Unicode operator at s (0 if none); *mul says Mul vs Add. */
int UnicodeOprLen(const char *s, bool *mul = nullptr);
/** @brief getIndentWidth: the indentation the file uses (0 if none is found), read as nimpretty reads it. */
int IndentWidth(std::string_view src);

// ast.nim's TNodeKind, the ones the parser makes.
enum class NK : uint8_t {
    Empty, Ident, CharLit, IntLit, Int8Lit, Int16Lit, Int32Lit, Int64Lit, UIntLit, UInt8Lit, UInt16Lit,
    UInt32Lit, UInt64Lit, FloatLit, Float32Lit, Float64Lit, Float128Lit, StrLit, RStrLit, TripleStrLit, NilLit,
    Call, Command, CallStrLit, Infix, Prefix, Postfix, ExprEqExpr, ExprColonExpr, IdentDefs, VarTuple, Par,
    ObjConstr, Curly, CurlyExpr, Bracket, BracketExpr, PragmaExpr, DotExpr, IfExpr, ElifExpr, ElseExpr, Lambda,
    Do, TableConstr, TupleConstr, Cast, AccQuoted, Asgn, GenericParams, FormalParams, OfInherit, ProcDef,
    MethodDef, ConverterDef, MacroDef, TemplateDef, IteratorDef, FuncDef, OfBranch, ElifBranch, ExceptBranch,
    Else, AsmStmt, Pragma, PragmaBlock, IfStmt, WhenStmt, ForStmt, WhileStmt, CaseStmt, TypeSection, VarSection,
    LetSection, ConstSection, ConstDef, TypeDef, YieldStmt, Defer, TryStmt, Finally, RaiseStmt, ReturnStmt,
    BreakStmt, ContinueStmt, BlockStmt, StaticStmt, DiscardStmt, StmtList, ImportStmt, ImportExceptStmt,
    ExportStmt, ExportExceptStmt, FromStmt, IncludeStmt, BindStmt, MixinStmt, UsingStmt, CommentStmt,
    StmtListExpr, RecList, RecCase, RecWhen, TypeOfExpr, ObjectTy, TupleTy, TupleClassTy, TypeClassTy, StaticTy,
    RefTy, PtrTy, VarTy, OutTy, DistinctTy, ProcTy, IteratorTy, EnumTy, EnumFieldDef, ArgList, With, Without,
    Bind,
};
const char *NodeKindName(NK k);

struct Node {
    NK kind = NK::Empty;
    int line = 0, col = 0;     // TLineInfo of the node (1-based line, 0-based column)
    int start = 0, end = 0;    // mep: byte range, [start, end), covering the node's tokens
    std::string ident;         // Ident / AccQuoted parts: the spelling
    int64_t int_val = 0;
    double float_val = 0;
    std::string str_val;       // string literals' (decoded) value
    std::string comment;       // doc comments attached by the parser
    int base = 0;              // nfBase2/8/16
    bool block_arg = false;    // nfBlockArg
    std::vector<Node *> kids;

    size_t size() const { return kids.size(); }
    Node *operator[](size_t i) const { return kids[i]; }
};

struct Options {
    bool nimpretty = false;
    int max_nest = 2000;
    Emitter *em = nullptr;  // nimpretty's layouter, fed every token (only with nimpretty)
};

struct Result {
    std::vector<std::unique_ptr<Node>> arena;
    Node *root = nullptr;       // an nkStmtList
    std::vector<Diag> errors;   // errors only (warnings dropped), in the order the parser reported them
    std::vector<Diag> warnings;
    std::vector<Token> comments;  // every comment token met (compiler mode: doc comments only)
    std::vector<int> line_starts;  // byte offset of each line's start
    /** @brief Byte offset of a (1-based line, 0-based column). */
    int Offset(int line, int col) const;
};

/** @brief Parses one Nim module (parseAll). */
Result Parse(std::string_view src, const Options &opts = {});

/** @brief Every token of the file in order, as the compiler's lexer reads it (comments included). */
std::vector<Token> Tokenize(std::string_view src, std::vector<Diag> *errors = nullptr);

/** @brief `$tok` / prettyTok, as error messages show a token. */
std::string TokString(const Token &tok);
std::string PrettyTok(const Token &tok);

}  // namespace nimparse

#endif
