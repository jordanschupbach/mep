#pragma once

// Internals of mep's Tcl formatter (tcl_format.cpp): the syntax tree,
// lexer and parser ported from tclint 0.6.2's syntax_tree.py, lexer.py,
// parser.py and commands/builtin.py (MIT, Noah Moroze). The parser knows
// which arguments of Tcl's builtin commands are scripts, expressions or
// lists -- that is what the formatter recurses into.

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace tclfmt::detail {

struct Pos {
    int line = 1;
    int col = 1;
};

enum class K {
    Script,
    Comment,
    Command,
    CommandSub,
    BareWord,
    BracedWord,
    QuotedWord,
    Compound,
    VarSub,
    ArgExpansion,
    List,
    Expression,
    BracedExpression,
    ParenExpression,
    UnaryOp,
    BinaryOp,
    TernaryOp,
    Function,
    LiteralBlock,  // formatter only: a tclfmt-disable region, copied verbatim
};

struct Node {
    K kind = K::BareWord;
    std::string value;  // BareWord/BracedWord text, Comment text, VarSub name
    Pos pos;
    Pos end;  // first character after the node
    std::vector<Node *> kids;
    bool braced = false;  // VarSub: ${name}
    bool paren = false;   // VarSub: had an index in parens (tclint drops an empty one)
    std::vector<std::string> block;  // LiteralBlock lines
};

/** Thrown for a Tcl syntax error (tclint's TclSyntaxError): the file is refused. */
struct SyntaxError {
    std::string message;
    Pos pos;
};

/** Thrown when nesting is deeper than the formatter is willing to recurse. */
struct DepthError {};

class Parser {
  public:
    /** Parses a whole script; throws SyntaxError / DepthError. */
    Node *Parse(const std::string &text);

    // Used by the command table (tcl_format_parse.cpp).
    Node *ParseScriptArg(Node *node);
    Node *ParseList(Node *node);
    Node *ParseExpression(Node *node);
    Node *ParseAt(const std::string &text, Pos start);
    Node *New(K kind);

  private:
    std::deque<Node> arena_;
    int depth_ = 0;       // nested scripts and lists
    int expr_depth_ = 0;  // expression recursion
    friend struct ParserImpl;
};

/** tclint's `node.contents`: the literal text of a word without substitutions, or null. */
const std::string *Contents(const Node *n);

}  // namespace tclfmt::detail
