#pragma once

// Internals of mep's Nix formatter (nix_format.h), shared by its three
// parts: the reader (nix_format_parse.cpp), the printer that turns the tree
// into a layout document (nix_format_pretty.cpp) and the layout engine that
// renders that document at a width (nix_format_layout.cpp).
//
// Everything here is a port of nixfmt 1.3.1 (github.com/NixOS/nixfmt,
// src/Nixfmt/*.hs), structure for structure, so that the output is
// nixfmt's byte for byte. The names follow the Haskell ones where that
// helps to read the two side by side: Ann -> AnnHdr, Leaf, Whole, Item,
// Doc/DocE, Spacing, GroupAnn, fixup, layoutGreedy.
//
// The tree is immutable and shared (shared_ptr<const ...>): nixfmt rewrites
// the first or last token of a subtree in several places (moving a comment
// onto the next line, out of a function call, into a body), and does it by
// rebuilding just the path down to that token. MapFirst/MapLast below do
// the same, so a rewrite never disturbs the original the caller still holds.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nixfmt::detail {

// ---- the syntax tree (Nixfmt/Types.hs) -----------------------------------

enum class Tok : unsigned char {
    Integer, Float, Identifier, EnvPath,
    KAssert, KElse, KIf, KIn, KInherit, KLet, KOr, KRec, KThen, KWith,
    BraceOpen, BraceClose, BrackOpen, BrackClose, InterOpen, InterClose, ParenOpen, ParenClose,
    Assign, At, Colon, Comma, Dot, DoubleQuote, DoubleSingleQuote, Ellipsis, Question, Semicolon,
    Concat, Negate, Update, Plus, Minus, Mul, Div, And, Or, Equal, Greater, GreaterEqual, Implies,
    Less, LessEqual, Not, Unequal, PipeForward, PipeBackward,
};

struct Trivium {
    enum Kind : unsigned char { EmptyLine, LineComment, BlockComment, LangAnnotation };
    Kind kind = EmptyLine;
    bool doc = false;                // BlockComment: `/**`
    std::string text;                // LineComment (after the `#`), LangAnnotation
    std::vector<std::string> lines;  // BlockComment
};
using Trivia = std::vector<Trivium>;

// Ann without its value: the comments before a token, its source line and
// the comment after it on the same line.
struct AnnHdr {
    Trivia pre;
    int line = 1;
    std::optional<std::string> trail;
    bool HasTrivia() const { return !pre.empty() || trail.has_value(); }
};

struct Leaf : AnnHdr {
    Tok tok = Tok::Identifier;
    std::string text;  // Integer, Float, Identifier, EnvPath (without the <>)
};

struct Expr;
struct Term;
struct Param;
using ExprP = std::shared_ptr<const Expr>;
using TermP = std::shared_ptr<const Term>;
using ParamP = std::shared_ptr<const Param>;

struct Whole {
    ExprP e;
    Trivia trivia;  // the final trivia
};

struct StringPart {
    bool interp = false;
    std::string text;                    // !interp
    std::shared_ptr<const Whole> whole;  // interp
};
using StrLine = std::vector<StringPart>;

struct StrAnn : AnnHdr {
    std::vector<StrLine> lines;
};
struct PathAnn : AnnHdr {
    StrLine parts;
};
struct InterpAnn : AnnHdr {
    StringPart part;
};

struct SimpleSel {
    enum Kind : unsigned char { Id, Interp, Str };
    Kind kind = Id;
    Leaf id;
    InterpAnn interp;
    StrAnn str;
};

struct Selector {
    std::optional<Leaf> dot;
    SimpleSel sel;
};

template <class T>
struct Item {
    bool comments = false;
    T value{};
    Trivia trivia;  // comments
};

struct Binder {
    enum Kind : unsigned char { Inherit, Assignment };
    Kind kind = Assignment;
    Leaf kw;                     // Inherit: `inherit`
    TermP from;                  // Inherit: `(source)`, may be null
    std::vector<SimpleSel> ids;  // Inherit
    std::vector<Selector> sels;  // Assignment: the attribute path
    Leaf eq;                     // Assignment: `=`
    ExprP rhs;                   // Assignment
    Leaf semi;
};

struct Term {
    enum Kind : unsigned char { Token, SimpleString, IndentedString, Path, List, Set, Selection, Parenthesized };
    Kind kind = Token;
    Leaf leaf;                  // Token
    StrAnn str;                 // SimpleString, IndentedString
    PathAnn path;               // Path
    std::optional<Leaf> rec;    // Set: `rec` or `let`
    Leaf open, close;           // List, Set, Parenthesized
    std::vector<Item<TermP>> list_items;
    std::vector<Item<Binder>> set_items;
    TermP sel_term;             // Selection
    std::vector<Selector> sels; // Selection
    std::optional<Leaf> or_kw;  // Selection: `or` default
    TermP or_def;
    ExprP paren;                // Parenthesized
};

struct ParamAttr {
    bool ellipsis = false;
    Leaf name;  // or the `...`
    std::optional<Leaf> qmark;
    ExprP def;
    std::optional<Leaf> comma;
};

struct Param {
    enum Kind : unsigned char { Id, Set, Context };
    Kind kind = Id;
    Leaf name;         // Id
    Leaf open, close;  // Set
    std::vector<ParamAttr> attrs;
    ParamP first, second;  // Context: first @ second
    Leaf at;
};

struct Expr {
    enum Kind : unsigned char {
        Term, With, Let, Assert, If, Abstraction, Application, Operation, MemberCheck, Negation, Inversion
    };
    Kind kind = Term;
    TermP term;
    // With: k1 `with` a k2 `;` b.  Let: k1 `let` binders k2 `in` a.
    // Assert: k1 a k2 `;` b.  If: k1 a k2 b k3 c.  Abstraction: param k1 `:` a.
    // Application: a b.  Operation: a k1 b.  MemberCheck: a k1 `?` sels.
    // Negation / Inversion: k1 a.
    Leaf k1, k2, k3;
    ExprP a, b, c;
    std::vector<Item<Binder>> binders;
    ParamP param;
    std::vector<Selector> sels;
};

std::string TokenText(const Leaf &l);

// Haskell's isSpace, on UTF-8: the byte length of the whitespace code point
// starting at `i` (or ending just before `end`), 0 if there is none. Besides
// ASCII that is U+00A0 and the other Unicode space separators, which nixfmt
// strips from comments and skips between tokens like any space.
size_t SpaceAt(const std::string &s, size_t i);
size_t SpaceBefore(const std::string &s, size_t end);
std::string StripSpace(const std::string &s);

// LanguageElement: the first / last token of a subtree, read-only or as a
// rebuilt copy with that token changed.
using HdrFn = std::function<void(AnnHdr &)>;
const AnnHdr &FirstTok(const Expr &e);
const AnnHdr &FirstTok(const Term &t);
const AnnHdr &FirstTok(const Param &p);
ExprP MapFirst(const ExprP &e, const HdrFn &f);
TermP MapFirst(const TermP &t, const HdrFn &f);
ParamP MapFirst(const ParamP &p, const HdrFn &f);
ExprP MapLast(const ExprP &e, const HdrFn &f);
const AnnHdr &LastTok(const Expr &e);

// Semantic equality, ignoring every comment and line number (nixfmt's Eq).
bool Equal(const Whole &a, const Whole &b);
// How deeply the tree nests, measured without recursing (the printer,
// the layout and Equal all recurse, so a deeper tree is refused).
int MaxDepth(const Whole &w);
// Every word of every comment in the tree, for checking none was lost.
void CommentWords(const Whole &w, std::vector<std::string> &out);

// ---- the reader (Nixfmt/Lexer.hs, Nixfmt/Parser.hs) ------------------------

struct ParseResult {
    bool ok = false;
    Whole file;
    std::string error;
    int error_line = 0;
};
// `strict`: every token gets line 1, which is nixfmt --strict (the layout
// stops depending on where the input put its line breaks).
ParseResult ParseFile(const std::string &src, bool strict);

// ---- the layout document (Nixfmt/Predoc.hs) --------------------------------

enum class Sp : unsigned char { Softbreak, Break, Hardspace, Softspace, Space, Hardline, Emptyline, Newlines };
enum class GAnn : unsigned char { Regular, Priority, Transparent };
enum class TAnn : unsigned char { Regular, Comment, TrailingComment, Trailing };

struct DocE;
using Doc = std::vector<DocE>;

struct DocE {
    enum Kind : unsigned char { Text, Spacing, Group };
    Kind kind = Text;
    TAnn tann = TAnn::Regular;
    Sp sp = Sp::Hardspace;
    GAnn g = GAnn::Regular;
    int n = 0;     // Newlines n
    int nest = 0;  // Text: nesting depth
    int off = 0;   // Text: offset
    std::string text;
    Doc kids;  // Group
};

Doc Fixup(const Doc &d);
// unexpandSpacing': the doc on one line, or nullopt when it has a hard line
// break or (with a limit) is longer than `limit`.
std::optional<Doc> UnexpandSpacing(std::optional<int> limit, const Doc &d);
std::string LayoutGreedy(int width, int indent, const Doc &d);
int TextWidth(const std::string &s);

// ---- the printer (Nixfmt/Pretty.hs) ------------------------------------------

Doc PrettyFile(const Whole &w);

}  // namespace nixfmt::detail
