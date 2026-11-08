#ifndef MEP_OCTAVE_PARSE_H
#define MEP_OCTAVE_PARSE_H

// mep's reader for Octave (and the MATLAB syntax Octave accepts): a lexer
// and a recursive-descent parser that make the decisions Octave 11's own
// lex.ll / oct-parse.yy make, written from their behaviour (Octave is GPL;
// none of its code is here).
//
// The lexer is the interesting half. Octave's is contextual: whether a
// space separates two matrix elements, whether `'` starts a string or is a
// transpose, whether `disp -x` is a command-syntax call, whether `end` is a
// keyword or the magic index -- each depends on the previous token and on
// flags the parser sets as it goes (inside an index, after `.`, in a
// function header, in a classdef block). So the parser pulls tokens one at
// a time and sets the same flags at the same points Octave's grammar
// actions do, and the lexer reads them.
//
// Errors are Octave's: the first one stops the parse (Octave aborts there
// too), with Octave's message and its "near line L, column C" position --
// L/C is where Octave's lexer stood after the token it choked on. The
// semantic checks Octave's parser makes while building the tree are made
// too (mismatched `endfor`/`endif`, `break` outside a loop, a duplicate
// subfunction, an invalid assignment target, a variable also used as a
// command, inconsistent function endings, ...). `OctaveCheckMessage`
// prints the result the way `__parse_file__` reports it, which is what
// tools/octave_parse_check.py compares with octave-cli.

#include <string>
#include <string_view>
#include <vector>

namespace octparse {

enum class Tok : unsigned char {
    Eof,
    Newline,     // a statement-ending newline ('\n' token)
    Comma,       // ',' (possibly synthetic: a separator the lexer inserted inside [] / {})
    Semi,        // ';' (possibly synthetic: a matrix row break from a newline)
    Name,        // an identifier (also `end` inside an index)
    StructElt,   // a field name after '.'
    Number,
    SqString,    // '...' (also a command-syntax argument: see `command_arg`)
    DqString,    // "..."
    Keyword,     // text is the keyword
    Op,          // operators and brackets; text is the spelling
    FcnHandle,   // @name (text is the name, dots allowed)
    MetaQuery,   // ?pkg.Class
    SuperclassRef,  // method@pkg.Class
    FqIdent,     // a classdef superclass name
    LexError,
};

struct Token {
    Tok kind = Tok::Eof;
    std::string text;       // spelling; strings: the value (escapes decoded); keywords/ops: spelling
    int begin = 0, end = 0;  // byte range in the source (synthetic tokens are empty)
    int line = 0, col = 0;   // 0-based start (line, byte column)
    int after_line = 1, after_col = 1;  // Octave's file position after the token (1-based)
    bool space_after = false;
    bool may_be_command = false;
    bool synthetic = false;
    bool command_arg = false;  // a word of a command-syntax call
};

struct Comment {
    int begin = 0, end = 0;   // byte range (block comments: from the `%{` line to the `%}` line)
    int line = 0, end_line = 0;
    bool block = false;       // %{ ... %}
    bool full_line = false;   // nothing but blanks before it on its line
    bool continuation = false;  // the text after a `...`
};

enum class Node : unsigned char {
    // statements
    StatementList,  // kids: statements
    ExprStmt,       // kids[0] expression; `sep` is the terminator token (-1 if none)
    Assign,         // kids[0] lhs (Matrix for [a,b]=...), kids[1] rhs; `op` the '=' or op= token
    Command,        // command syntax: name token `tok`, kids: none; args are tokens tok+1..last
    If,             // kids: IfClause/ElseClause...
    IfClause,       // kids[0] condition, kids[1] body (elseif too: `tok` is the keyword)
    ElseClause,     // kids[0] body
    Switch,         // kids[0] subject, then Case/Otherwise
    Case,           // kids[0] label, kids[1] body
    Otherwise,      // kids[0] body
    While,          // kids[0] cond, kids[1] body
    DoUntil,        // kids[0] body, kids[1] cond
    For,            // kids[0] lhs, kids[1] range, kids[2] body, [kids[3] parfor max workers]
    Try,            // kids[0] body, [kids[1] Catch]
    Catch,          // kids[0] body; `name` = identifier node (-1 if none)
    Unwind,         // kids[0] body, kids[1] cleanup
    Spmd,           // kids[0] body
    Decl,           // global/persistent; kids: DeclElt
    DeclElt,        // `name`; kids[0] initializer (optional)
    Jump,           // break / continue / return (`tok`)
    Function,       // `name` node, kids[0] ParamList (in), kids[1] ParamList (out), kids[2] body;
                    // nested functions are statements of the body
    ParamList,      // kids: Identifier/Tilde/DeclElt
    ArgumentsBlock, // kids: ArgValidation...
    ArgValidation,  // kids[0] name, then size/class/validators/default
    Classdef,       // `name` identifier; kids: Attr..., Superclass..., blocks
    ClassBlock,     // properties/methods/events/enumeration; `tok` is the keyword; kids: items
    Property,       // kids[0] name identifier, rest validation/default
    Attr,           // kids[0] identifier, [kids[1] value]
    Superclass,     // `tok` FqIdent
    EnumItem,       // kids[0] name, kids[1..] args
    MethodDecl,     // external method signature
    // expressions
    Identifier,     // tok
    Number,         // tok
    String,         // tok
    Colon,          // magic ':' as an argument
    Tilde,          // magic '~'
    End,            // `end` inside an index
    Matrix,         // kids: Row
    Cell,           // kids: Row
    Row,            // kids: elements
    Paren,          // kids[0]
    Prefix,         // `op` token; kids[0]
    Postfix,        // `op` token; kids[0]
    Binary,         // `op` token; kids[0], kids[1]
    Range,          // kids: base, [increment], limit (2 or 3)
    Index,          // kids[0] object, kids[1..] args; `op` is the '(' or '{' token
    Field,          // kids[0] object; `tok` is the StructElt name
    DynField,       // kids[0] object, kids[1] the field expression
    AnonFcn,        // kids[0] ParamList, kids[1] body
    FcnHandle,      // tok
    MetaQuery,      // tok
    SuperclassRef,  // tok
};

struct AstNode {
    Node kind = Node::StatementList;
    int first = -1, last = -1;  // token range (inclusive); -1 when empty
    int tok = -1;                // the node's main token (name, keyword, literal)
    int op = -1;                 // operator / bracket token
    int sep = -1;                // ExprStmt/Assign/Command: the terminating ',' or ';' (or -1)
    int name = -1;               // Function / Catch / DeclElt / Classdef: identifier node
    bool explicit_end = false;   // Function: closed by end/endfunction
    std::vector<int> kids;
};

struct Options {
    // The file's base name without extension ("" = unknown): a classdef
    // must be named after its file, and a function file's primary function
    // after the file is only a warning in Octave, so it is not checked.
    std::string file_base;
};

struct ParseResult {
    std::string_view src;
    std::vector<Token> tokens;  // in source order, including synthetic separators and newlines
    std::vector<Comment> comments;
    std::vector<AstNode> nodes;
    int root = -1;  // a StatementList
    bool function_file = false, classdef_file = false;
    // The first error, as Octave reports it; ok when message is empty.
    std::string error;
    int error_line = 0, error_col = 0;  // Octave's "near line L, column C" (1-based line, column as printed)
    int error_tok = -1;                 // the offending token, when there is one
    bool ok() const { return error.empty(); }
};

/** @brief Reads Octave source. Never throws; nesting is capped (kMaxDepth) with an error. */
ParseResult Parse(std::string_view src, const Options &opt = {});

/** @brief What `__parse_file__` would say: "ok", or "<message> near line L, column C". */
std::string CheckMessage(const ParseResult &r);

/** @brief Octave's keywords (iskeyword(), plus the classdef/arguments context words). */
bool IsKeyword(std::string_view s);

constexpr int kMaxDepth = 400;

}  // namespace octparse

#endif  // MEP_OCTAVE_PARSE_H
