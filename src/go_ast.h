#ifndef MEP_GO_AST_H
#define MEP_GO_AST_H

// Go's syntax tree, as go/ast defines it, for mep's own Go tools: the
// parser (go_parse.cpp, a port of go/scanner + go/parser), the formatter
// (go_printer.cpp, a port of go/printer) and the language server
// (go_lsp.cpp). The node kinds, their fields and their Pos()/End() rules
// are go/ast's, so the ported code reads like the original.
//
// Portions are a translation of Go's go/token and go/ast packages:
// Copyright 2009 The Go Authors. All rights reserved. Use of this source
// code is governed by a BSD-style license (the notice is in go_parse.cpp).
//
// Positions are go/token's: a Pos is a byte offset plus one in the single
// file being parsed (a FileSet holding just that file, base 1), and 0 is
// NoPos. Nodes live in the File's arena and are referred to by raw
// pointers; Expr, Stmt, Decl and Spec are all `Node *`.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace goast {

using Pos = int;
constexpr Pos NoPos = 0;
inline bool IsValid(Pos p) { return p != NoPos; }

// go/token's Token, in its order.
enum class Tok : uint8_t {
    ILLEGAL, EOF_, COMMENT,
    // literals
    IDENT, INT, FLOAT, IMAG, CHAR, STRING,
    // operators
    ADD, SUB, MUL, QUO, REM,
    AND, OR, XOR, SHL, SHR, AND_NOT,
    ADD_ASSIGN, SUB_ASSIGN, MUL_ASSIGN, QUO_ASSIGN, REM_ASSIGN,
    AND_ASSIGN, OR_ASSIGN, XOR_ASSIGN, SHL_ASSIGN, SHR_ASSIGN, AND_NOT_ASSIGN,
    LAND, LOR, ARROW, INC, DEC,
    EQL, LSS, GTR, ASSIGN, NOT,
    NEQ, LEQ, GEQ, DEFINE, ELLIPSIS,
    LPAREN, LBRACK, LBRACE, COMMA, PERIOD,
    RPAREN, RBRACK, RBRACE, SEMICOLON, COLON,
    TILDE,
    // keywords
    BREAK, CASE, CHAN, CONST, CONTINUE,
    DEFAULT, DEFER, ELSE, FALLTHROUGH, FOR,
    FUNC, GO, GOTO, IF, IMPORT,
    INTERFACE, MAP, PACKAGE, RANGE, RETURN,
    SELECT, STRUCT, SWITCH, TYPE, VAR,
};

/** @brief go/token's Token.String(): the operator or keyword spelling, or the class name ("IDENT", "EOF"). */
const char *TokString(Tok t);
/** @brief Binary operator precedence (5 = `*`, 1 = `||`), 0 for anything else. */
int Precedence(Tok t);
constexpr int LowestPrec = 0;
constexpr int UnaryPrec = 6;
constexpr int HighestPrec = 7;
/** @brief The keyword spelled `s`, or IDENT. */
Tok Lookup(std::string_view s);
inline bool IsLiteral(Tok t) { return t >= Tok::IDENT && t <= Tok::STRING; }
inline bool IsOperator(Tok t) { return t >= Tok::ADD && t <= Tok::TILDE; }
inline bool IsKeyword(Tok t) { return t >= Tok::BREAK; }
/** @brief Whether `s` is a keyword. */
bool IsKeywordName(std::string_view s);
/** @brief Whether `s` is a Go identifier (and not a keyword). */
bool IsIdentifier(std::string_view s);
/** @brief Whether `s` starts with an upper-case letter (go/token's IsExported). */
bool IsExported(std::string_view s);

// --- Unicode, as Go's unicode package (generated tables, go_unicode.inc) ---
bool UnicodeIsLetter(uint32_t r);
bool UnicodeIsDigit(uint32_t r);
bool UnicodeIsSpace(uint32_t r);
bool UnicodeIsUpper(uint32_t r);
bool UnicodeIsLower(uint32_t r);
bool UnicodeIsPunct(uint32_t r);
/** @brief unicode.IsPrint (= strconv.IsPrint): letters, marks, numbers, punctuation, symbols and ASCII space. */
bool UnicodeIsPrint(uint32_t r);
/** @brief utf8.DecodeRune: the rune at s[i] and its width (RuneError, 1 for a bad byte; 0 at the end). */
uint32_t DecodeRune(std::string_view s, size_t i, int *width);
/** @brief utf8.DecodeLastRune. */
uint32_t DecodeLastRune(std::string_view s, int *width);
/** @brief utf8.RuneCountInString. */
int RuneCount(std::string_view s);
void AppendRune(std::string &out, uint32_t r);
constexpr uint32_t RuneError = 0xFFFD;

// --- Nodes -------------------------------------------------------------------

enum class K : uint8_t {
    // expressions and types
    BadExpr, Ident, Ellipsis, BasicLit, FuncLit, CompositeLit, ParenExpr, SelectorExpr, IndexExpr, IndexListExpr,
    SliceExpr, TypeAssertExpr, CallExpr, StarExpr, UnaryExpr, BinaryExpr, KeyValueExpr,
    ArrayType, StructType, FuncType, InterfaceType, MapType, ChanType,
    // statements
    BadStmt, DeclStmt, EmptyStmt, LabeledStmt, ExprStmt, SendStmt, IncDecStmt, AssignStmt, GoStmt, DeferStmt,
    ReturnStmt, BranchStmt, BlockStmt, IfStmt, CaseClause, SwitchStmt, TypeSwitchStmt, CommClause, SelectStmt,
    ForStmt, RangeStmt,
    // specs and declarations
    ImportSpec, ValueSpec, TypeSpec, BadDecl, GenDecl, FuncDecl,
    // the rest
    Comment, CommentGroup, Field, FieldList, File,
};

struct Node {
    K kind;
    explicit Node(K k) : kind(k) {}
    virtual ~Node() = default;
    Node(const Node &) = default;
    Node &operator=(const Node &) = default;
};

/** @brief The node's first position (go/ast's Pos()). */
Pos NodePos(const Node *n);
/** @brief The position just after the node (go/ast's End()). */
Pos NodeEnd(const Node *n);
inline bool IsExpr(const Node *n) { return n && n->kind <= K::ChanType; }
inline bool IsStmt(const Node *n) { return n && n->kind >= K::BadStmt && n->kind <= K::RangeStmt; }

template <class T> T *As(Node *n) { return n && n->kind == T::kKind ? static_cast<T *>(n) : nullptr; }
template <class T> const T *As(const Node *n) { return n && n->kind == T::kKind ? static_cast<const T *>(n) : nullptr; }

#define GOAST_KIND(name) \
    static constexpr K kKind = K::name; \
    name() : Node(K::name) {}

struct Comment : Node {
    GOAST_KIND(Comment)
    Pos Slash = NoPos;
    std::string Text;  // with the comment markers; CRs removed as go/scanner does
};

struct CommentGroup : Node {
    GOAST_KIND(CommentGroup)
    std::vector<Comment *> List;
};
/** @brief CommentGroup.Text(): the text without markers, blank lines collapsed, directives dropped. */
std::string CommentGroupText(const CommentGroup *g);

struct Ident;
struct BasicLit;
struct FieldList;
struct BlockStmt;
struct CallExpr;
struct FuncType;

struct Field : Node {
    GOAST_KIND(Field)
    CommentGroup *Doc = nullptr;
    std::vector<Ident *> Names;
    Node *Type = nullptr;
    BasicLit *Tag = nullptr;
    CommentGroup *Comment = nullptr;
};

struct FieldList : Node {
    GOAST_KIND(FieldList)
    Pos Opening = NoPos;
    std::vector<Field *> List;
    Pos Closing = NoPos;
};
int NumFields(const FieldList *f);

struct BadExpr : Node {
    GOAST_KIND(BadExpr)
    Pos From = NoPos, To = NoPos;
};
struct Ident : Node {
    GOAST_KIND(Ident)
    Pos NamePos = NoPos;
    std::string Name;
};
struct Ellipsis : Node {
    GOAST_KIND(Ellipsis)
    Pos Ellipsis_ = NoPos;
    Node *Elt = nullptr;
};
struct BasicLit : Node {
    GOAST_KIND(BasicLit)
    Pos ValuePos = NoPos;
    Pos ValueEnd = NoPos;
    Tok Kind = Tok::INT;
    std::string Value;
};
struct FuncLit : Node {
    GOAST_KIND(FuncLit)
    FuncType *Type = nullptr;
    BlockStmt *Body = nullptr;
};
struct CompositeLit : Node {
    GOAST_KIND(CompositeLit)
    Node *Type = nullptr;
    Pos Lbrace = NoPos;
    std::vector<Node *> Elts;
    Pos Rbrace = NoPos;
    bool Incomplete = false;
};
struct ParenExpr : Node {
    GOAST_KIND(ParenExpr)
    Pos Lparen = NoPos;
    Node *X = nullptr;
    Pos Rparen = NoPos;
};
struct SelectorExpr : Node {
    GOAST_KIND(SelectorExpr)
    Node *X = nullptr;
    Ident *Sel = nullptr;
};
struct IndexExpr : Node {
    GOAST_KIND(IndexExpr)
    Node *X = nullptr;
    Pos Lbrack = NoPos;
    Node *Index = nullptr;
    Pos Rbrack = NoPos;
};
struct IndexListExpr : Node {
    GOAST_KIND(IndexListExpr)
    Node *X = nullptr;
    Pos Lbrack = NoPos;
    std::vector<Node *> Indices;
    Pos Rbrack = NoPos;
};
struct SliceExpr : Node {
    GOAST_KIND(SliceExpr)
    Node *X = nullptr;
    Pos Lbrack = NoPos;
    Node *Low = nullptr, *High = nullptr, *Max = nullptr;
    bool Slice3 = false;
    Pos Rbrack = NoPos;
};
struct TypeAssertExpr : Node {
    GOAST_KIND(TypeAssertExpr)
    Node *X = nullptr;
    Pos Lparen = NoPos;
    Node *Type = nullptr;  // nullptr: x.(type)
    Pos Rparen = NoPos;
};
struct CallExpr : Node {
    GOAST_KIND(CallExpr)
    Node *Fun = nullptr;
    Pos Lparen = NoPos;
    std::vector<Node *> Args;
    Pos Ellipsis = NoPos;
    Pos Rparen = NoPos;
};
struct StarExpr : Node {
    GOAST_KIND(StarExpr)
    Pos Star = NoPos;
    Node *X = nullptr;
};
struct UnaryExpr : Node {
    GOAST_KIND(UnaryExpr)
    Pos OpPos = NoPos;
    Tok Op = Tok::ILLEGAL;
    Node *X = nullptr;
};
struct BinaryExpr : Node {
    GOAST_KIND(BinaryExpr)
    Node *X = nullptr;
    Pos OpPos = NoPos;
    Tok Op = Tok::ILLEGAL;
    Node *Y = nullptr;
};
struct KeyValueExpr : Node {
    GOAST_KIND(KeyValueExpr)
    Node *Key = nullptr;
    Pos Colon = NoPos;
    Node *Value = nullptr;
};

enum ChanDir : int { SEND = 1, RECV = 2 };

struct ArrayType : Node {
    GOAST_KIND(ArrayType)
    Pos Lbrack = NoPos;
    Node *Len = nullptr;  // nullptr: slice; Ellipsis: [...]T
    Node *Elt = nullptr;
};
struct StructType : Node {
    GOAST_KIND(StructType)
    Pos Struct = NoPos;
    FieldList *Fields = nullptr;
    bool Incomplete = false;
};
struct FuncType : Node {
    GOAST_KIND(FuncType)
    Pos Func = NoPos;
    FieldList *TypeParams = nullptr;
    FieldList *Params = nullptr;
    FieldList *Results = nullptr;
};
struct InterfaceType : Node {
    GOAST_KIND(InterfaceType)
    Pos Interface = NoPos;
    FieldList *Methods = nullptr;
    bool Incomplete = false;
};
struct MapType : Node {
    GOAST_KIND(MapType)
    Pos Map = NoPos;
    Node *Key = nullptr, *Value = nullptr;
};
struct ChanType : Node {
    GOAST_KIND(ChanType)
    Pos Begin = NoPos;
    Pos Arrow = NoPos;
    int Dir = SEND | RECV;
    Node *Value = nullptr;
};

struct BadStmt : Node {
    GOAST_KIND(BadStmt)
    Pos From = NoPos, To = NoPos;
};
struct DeclStmt : Node {
    GOAST_KIND(DeclStmt)
    Node *Decl = nullptr;
};
struct EmptyStmt : Node {
    GOAST_KIND(EmptyStmt)
    Pos Semicolon = NoPos;
    bool Implicit = false;
};
struct LabeledStmt : Node {
    GOAST_KIND(LabeledStmt)
    Ident *Label = nullptr;
    Pos Colon = NoPos;
    Node *Stmt = nullptr;
};
struct ExprStmt : Node {
    GOAST_KIND(ExprStmt)
    Node *X = nullptr;
};
struct SendStmt : Node {
    GOAST_KIND(SendStmt)
    Node *Chan = nullptr;
    Pos Arrow = NoPos;
    Node *Value = nullptr;
};
struct IncDecStmt : Node {
    GOAST_KIND(IncDecStmt)
    Node *X = nullptr;
    Pos TokPos = NoPos;
    Tok Tok_ = Tok::INC;
};
struct AssignStmt : Node {
    GOAST_KIND(AssignStmt)
    std::vector<Node *> Lhs;
    Pos TokPos = NoPos;
    Tok Tok_ = Tok::ASSIGN;
    std::vector<Node *> Rhs;
};
struct GoStmt : Node {
    GOAST_KIND(GoStmt)
    Pos Go = NoPos;
    CallExpr *Call = nullptr;
};
struct DeferStmt : Node {
    GOAST_KIND(DeferStmt)
    Pos Defer = NoPos;
    CallExpr *Call = nullptr;
};
struct ReturnStmt : Node {
    GOAST_KIND(ReturnStmt)
    Pos Return = NoPos;
    std::vector<Node *> Results;
};
struct BranchStmt : Node {
    GOAST_KIND(BranchStmt)
    Pos TokPos = NoPos;
    Tok Tok_ = Tok::BREAK;
    Ident *Label = nullptr;
};
struct BlockStmt : Node {
    GOAST_KIND(BlockStmt)
    Pos Lbrace = NoPos;
    std::vector<Node *> List;
    Pos Rbrace = NoPos;
};
struct IfStmt : Node {
    GOAST_KIND(IfStmt)
    Pos If = NoPos;
    Node *Init = nullptr;
    Node *Cond = nullptr;
    BlockStmt *Body = nullptr;
    Node *Else = nullptr;
};
struct CaseClause : Node {
    GOAST_KIND(CaseClause)
    Pos Case = NoPos;
    std::vector<Node *> List;  // empty: default
    Pos Colon = NoPos;
    std::vector<Node *> Body;
};
struct SwitchStmt : Node {
    GOAST_KIND(SwitchStmt)
    Pos Switch = NoPos;
    Node *Init = nullptr;
    Node *Tag = nullptr;
    BlockStmt *Body = nullptr;
};
struct TypeSwitchStmt : Node {
    GOAST_KIND(TypeSwitchStmt)
    Pos Switch = NoPos;
    Node *Init = nullptr;
    Node *Assign = nullptr;
    BlockStmt *Body = nullptr;
};
struct CommClause : Node {
    GOAST_KIND(CommClause)
    Pos Case = NoPos;
    Node *Comm = nullptr;  // nullptr: default
    Pos Colon = NoPos;
    std::vector<Node *> Body;
};
struct SelectStmt : Node {
    GOAST_KIND(SelectStmt)
    Pos Select = NoPos;
    BlockStmt *Body = nullptr;
};
struct ForStmt : Node {
    GOAST_KIND(ForStmt)
    Pos For = NoPos;
    Node *Init = nullptr, *Cond = nullptr, *Post = nullptr;
    BlockStmt *Body = nullptr;
};
struct RangeStmt : Node {
    GOAST_KIND(RangeStmt)
    Pos For = NoPos;
    Node *Key = nullptr, *Value = nullptr;
    Pos TokPos = NoPos;
    Tok Tok_ = Tok::ILLEGAL;  // ILLEGAL when Key == nullptr
    Pos Range = NoPos;
    Node *X = nullptr;
    BlockStmt *Body = nullptr;
};

struct ImportSpec : Node {
    GOAST_KIND(ImportSpec)
    CommentGroup *Doc = nullptr;
    Ident *Name = nullptr;
    BasicLit *Path = nullptr;
    CommentGroup *Comment = nullptr;
    Pos EndPos = NoPos;
};
struct ValueSpec : Node {
    GOAST_KIND(ValueSpec)
    CommentGroup *Doc = nullptr;
    std::vector<Ident *> Names;
    Node *Type = nullptr;
    std::vector<Node *> Values;
    CommentGroup *Comment = nullptr;
};
struct TypeSpec : Node {
    GOAST_KIND(TypeSpec)
    CommentGroup *Doc = nullptr;
    Ident *Name = nullptr;
    FieldList *TypeParams = nullptr;
    Pos Assign = NoPos;
    Node *Type = nullptr;
    CommentGroup *Comment = nullptr;
};
struct BadDecl : Node {
    GOAST_KIND(BadDecl)
    Pos From = NoPos, To = NoPos;
};
struct GenDecl : Node {
    GOAST_KIND(GenDecl)
    CommentGroup *Doc = nullptr;
    Pos TokPos = NoPos;
    Tok Tok_ = Tok::IMPORT;
    Pos Lparen = NoPos;
    std::vector<Node *> Specs;
    Pos Rparen = NoPos;
};
struct FuncDecl : Node {
    GOAST_KIND(FuncDecl)
    CommentGroup *Doc = nullptr;
    FieldList *Recv = nullptr;
    Ident *Name = nullptr;
    FuncType *Type = nullptr;
    BlockStmt *Body = nullptr;
};

struct File : Node {
    GOAST_KIND(File)
    CommentGroup *Doc = nullptr;
    Pos Package = NoPos;
    Ident *Name = nullptr;
    std::vector<Node *> Decls;
    Pos FileStart = NoPos, FileEnd = NoPos;
    std::vector<ImportSpec *> Imports;
    std::vector<CommentGroup *> Comments;
};

#undef GOAST_KIND

/** @brief Owns every node of one parse. */
class Arena {
public:
    template <class T> T *New() {
        auto p = std::make_unique<T>();
        T *raw = p.get();
        nodes_.push_back(std::move(p));
        return raw;
    }
    template <class T> T *Copy(const T &from) {
        auto p = std::make_unique<T>(from);
        T *raw = p.get();
        nodes_.push_back(std::move(p));
        return raw;
    }
    size_t size() const { return nodes_.size(); }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
};

/** @brief ast.Unparen: strips any number of parentheses. */
Node *Unparen(Node *e);
const Node *Unparen(const Node *e);

/** @brief Calls `f(child)` for each direct child of `n` in source order (go/ast's Walk order). */
template <class F> void ForEachChild(Node *n, F &&f);

}  // namespace goast

#include "go_ast_walk.h"

#endif
