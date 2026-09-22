#ifndef MEP_C_AST_H
#define MEP_C_AST_H

#include <memory>
#include <string>
#include <vector>

// The front end of mep's own C language server: a hand-written lexer, a
// preprocessor model, and a recursive-descent parser for C (C23-shaped,
// with the GNU extensions real code is written in), plus the syntax tree
// all three feed. c_lsp.h is the analysis half that consumes this, and
// c_lsp_server.cpp the JSON-RPC wire half.
//
// Why a real parser, when org_lsp.cpp deliberately refuses to build one:
// the same reason python_ast.h gives. C's interesting mistakes are not
// local -- "this local is never read", "this static function is dead",
// "this goto has no label", "this switch has two `case 3:`" are all
// questions about a *structure* a line scan cannot see. So this pays for
// a tree once, and every feature answers from the same parse.
//
// The preprocessor is the part a C front end cannot copy from Python.
// mep's server never runs it: an editor is looking at unpreprocessed
// source, and a server that needed `-I` flags and a compilation database
// to say anything at all would say nothing in most buffers. Instead:
//   - Directives are lexed into CDirective records (see below) and kept
//     out of the token stream the parser walks.
//   - A conditional group (`#if` ... `#elif` ... `#else` ... `#endif`)
//     contributes exactly *one* arm to that stream, chosen by
//     SelectCPreprocessorArms: the condition is evaluated when the file
//     itself settles it (`#if 0`, `#if defined(X)` for an X this file
//     `#define`s, `__cplusplus` which is false by construction in a C
//     server), and otherwise the first arm whose brace/paren/bracket
//     nesting comes out balanced wins. That last rule is what keeps the
//     `#ifdef __cplusplus` / `extern "C" {` prologue every public header
//     opens with from unbalancing the whole file.
//   - Object-like macros that expand to nothing or to a keyword-ish
//     token run (`#define PUBLIC __attribute__((visibility("default")))`)
//     are substituted, because they otherwise sit in the middle of a
//     declaration and cost a false syntax error. Function-like macros are
//     never expanded; a call to one parses as a call, which is what it
//     looks like anyway.
// The cost is named rather than hidden: a name that only exists in a
// non-selected arm is invisible to this server, exactly as it is to a
// compiler that took the other branch.
//
// Positions are 0-based lines and 0-based *byte* columns throughout, the
// convention org_lsp.h documents and python_ast.h repeats: mep's LSP
// client feeds `character` straight to a byte column, and the server
// declares `positionEncoding: "utf-8"` to match.

// --- Positions --------------------------------------------------------

struct CPos {
    int line = 0;  // 0-based
    int col = 0;   // 0-based byte column
};

/** @brief Orders two positions by line, then byte column. */
bool CPosLess(const CPos &a, const CPos &b);

// --- Tokens -----------------------------------------------------------

enum class CTokKind {
    End,      // end of input
    Ident,    // identifier that is not a reserved keyword
    Keyword,  // reserved keyword (`int`, `while`, `_Atomic`, `__attribute__`, ...)
    Number,   // preprocessing number: `1`, `0x1f`, `1.5e-3f`, `1'000` is not C
    CharLit,  // character constant, prefix and quotes included in `text`
    String,   // string literal, prefix and quotes included in `text`
    Punct,    // operator or delimiter
    Hash,     // the `#` that opens a directive line (never seen by the parser)
};

struct CToken {
    CTokKind kind = CTokKind::End;
    std::string text;  // exactly the source bytes this token covers
    CPos start;
    CPos end;  // exclusive
    // False for a token inside a preprocessor arm SelectCPreprocessorArms
    // did not take, and for every token of a directive line. The parser
    // walks only the active ones; features that are genuinely lexical
    // ("is this position inside a string") walk them all.
    bool active = true;
    // True for a string or character literal whose closing quote is
    // missing. The token is still emitted, ending at the line's end, so
    // the parser keeps its place in a file being typed into.
    bool unterminated = false;
    // Index into CParseResult::directives for a token that belongs to a
    // directive line, or -1.
    int directive = -1;
};

// A lexical or syntactic problem, reported with the same {code, message}
// vocabulary the rest of the server uses.
struct CSyntaxError {
    CPos start;
    CPos end;
    std::string code;
    std::string message;
};

// --- Preprocessor -----------------------------------------------------

enum class CDirectiveKind {
    None,     // `#` alone: a null directive, legal and meaningless
    Include,  // `#include` / `#include_next` / `#import`
    Define,
    Undef,
    If,
    Ifdef,
    Ifndef,
    Elif,
    ElifDef,   // C23 `#elifdef` / `#elifndef` (the `ndef` form sets `negated`)
    Else,
    Endif,
    Line,
    Error,
    Warning,
    Pragma,
    Embed,    // C23 `#embed`
    Unknown,  // a `#` followed by something this server has no name for
};

struct CDirective {
    CDirectiveKind kind = CDirectiveKind::Unknown;
    int line = 0;      // 0-based line the `#` is on
    int end_line = 0;  // last line, after backslash continuations
    int col = 0;       // byte column of the `#`
    std::string keyword;   // "include", "define", ... exactly as written
    std::string name;      // macro name for define/undef/ifdef/ifndef/elifdef
    CPos name_pos;         // where `name` starts
    int name_end_col = 0;  // byte column just past `name`
    std::string text;      // everything after the keyword, whitespace-trimmed
    // #include only.
    std::string header;  // the header spelled without its <> or ""
    bool angled = false;
    int header_col = 0;      // byte column of the opening `<` or `"`
    int header_end_col = 0;  // byte column just past the closing `>` or `"`
    // #define only.
    bool function_like = false;
    bool variadic = false;  // a `...` parameter
    std::vector<std::string> params;
    std::string body;  // replacement list, whitespace-trimmed
    // Set by SelectCPreprocessorArms.
    bool active = true;    // this directive is inside a taken arm
    bool taken = false;    // a conditional directive whose own arm was taken
    int group_start = -1;  // index of the `#if`-family directive opening this
                           // conditional group, for else/elif/endif
    int matching_end = -1;  // index of the `#endif` closing this group
};

// --- Types ------------------------------------------------------------

// A deliberately shallow model of a C type: enough to answer "what are
// this expression's members", "is this function void", "does this local
// look unused", and to print something readable in hover -- and no more.
// There is no canonicalization, no compatibility rule, and no arithmetic
// conversion anywhere in this server, because a checker that cannot see
// the included headers would get all three wrong far more often than it
// got them right (c_lsp.h's Scope section says the same thing at length).
struct CType {
    std::string spelling;  // as written, whitespace normalized: "const char *"
    // The type the declarator is built on, with no pointers or arrays:
    // "int", "struct sockaddr_in", "size_t", "void". Empty for an
    // implicit int this server chose not to guess at.
    std::string base;
    int pointers = 0;    // how many `*` the declarator applies
    int array_dims = 0;  // how many `[...]` it applies
    bool is_function = false;
    bool is_const = false;
    bool is_volatile = false;
    bool is_struct = false;  // `base` names a struct/union/enum tag
    /** @brief Reports whether this is plain `void`, the return type with no value. */
    bool IsVoid() const { return base == "void" && pointers == 0 && array_dims == 0 && !is_function; }
};

// --- Syntax tree ------------------------------------------------------

enum class CNodeKind {
    TranslationUnit,
    // Declarations
    FunctionDef,   // name, type = return type, params[], body[] = the compound statement's statements
    Declaration,   // kids[] = Declarator; also carries a RecordDecl/EnumDecl kid for `struct S { ... } x;`
    Declarator,    // name, type, kids[0] = initializer (optional), params[] for a function declarator
    Param,         // name (possibly empty), type
    RecordDecl,    // str_value = "struct"/"union", name = tag (possibly empty), body[] = Field
    Field,         // name, type, kids[0] = bit-field width (optional)
    EnumDecl,      // name = tag (possibly empty), body[] = Enumerator
    Enumerator,    // name, kids[0] = value (optional)
    StaticAssert,  // kids[0] = condition, kids[1] = message (optional)
    MacroDecl,     // a file-scope `IDENT(...)` that is a macro invocation, not a declaration
    // Statements
    CompoundStmt,  // body[]
    IfStmt,        // kids[0] = condition, body[0] = then, orelse[0] = else (optional)
    WhileStmt,     // kids[0] = condition, body[0]
    DoStmt,        // body[0], kids[0] = condition
    ForStmt,       // kids[0] = init (a DeclStmt or expression or Placeholder), kids[1] = condition,
                   // kids[2] = increment, body[0]
    SwitchStmt,    // kids[0] = subject, body[0]
    CaseStmt,      // kids[0] = value, kids[1] = range end for GNU `case 1 ... 5`, body[0] = statement
    DefaultStmt,   // body[0]
    LabelStmt,     // name, body[0]
    GotoStmt,      // name (empty for a GNU computed goto, whose target is kids[0])
    ReturnStmt,    // kids[0] = value (optional)
    BreakStmt,
    ContinueStmt,
    ExprStmt,   // kids[0]
    EmptyStmt,  // a bare `;`
    DeclStmt,   // kids[0] = Declaration
    AsmStmt,    // name = the whole `asm(...)` text, unparsed on purpose
    // Expressions
    Ident,    // name
    Number,   // name = literal text
    CharLit,  // name = literal text
    StrLit,   // name = raw literal text (all concatenated pieces), str_value = decoded bytes
    Call,     // kids[0] = callee, kids[1..] = arguments
    Member,   // kids[0] = object, name = member (`.`)
    Arrow,    // kids[0] = object, name = member (`->`)
    Index,    // kids[0] = array, kids[1] = subscript
    Unary,    // name = operator text; str_value = "post" for `x++`/`x--`
    Binary,   // name = operator text, kids[0], kids[1]
    Assign,   // name = operator text ("=", "+=", ...), kids[0] = target, kids[1] = value
    Conditional,      // kids[0] = condition, kids[1] = then (may be Placeholder for GNU `a ?: b`), kids[2] = else
    Cast,             // kids[0] = TypeName, kids[1] = operand
    SizeOfExpr,       // kids[0]
    SizeOfType,       // kids[0] = TypeName
    AlignOf,          // kids[0] = TypeName
    CompoundLiteral,  // kids[0] = TypeName, kids[1] = InitList
    InitList,         // kids[]
    Designator,       // name = ".field", or kids[0] = index expression; kids.back() = the value
    Comma,            // kids[0], kids[1]
    Paren,            // kids[0] -- kept, because `if ((x = f()))` is how you say "I meant this"
    StmtExpr,         // GNU `({ ... })`: body[]
    Generic,          // C11 `_Generic`: kids[0] = controlling expression, kids[1..] = GenericAssoc
    GenericAssoc,     // kids[0] = TypeName (absent for `default`), kids[1] = value
    TypeName,         // a type in a cast, sizeof, compound literal or _Generic; carries `type`
    Placeholder,      // an omitted slot, or an expression this parser could not read
};

// Storage class and qualifier flags, as a bit set on CNode::flags.
enum CNodeFlags : unsigned {
    kCFlagStatic = 1u << 0,
    kCFlagExtern = 1u << 1,
    kCFlagInline = 1u << 2,
    kCFlagTypedef = 1u << 3,
    kCFlagRegister = 1u << 4,
    kCFlagThreadLocal = 1u << 5,
    kCFlagNoreturn = 1u << 6,
    kCFlagConst = 1u << 7,
    kCFlagVolatile = 1u << 8,
    // The declarator carries `__attribute__((unused))` / `((used))` /
    // `((cleanup(...)))`, i.e. the author already said not to ask.
    kCFlagAttrUnused = 1u << 9,
    // A `struct`/`union`/`enum` definition that is this declaration's own
    // reason to exist (`struct S { int x; };` with no declarator).
    kCFlagTagOnly = 1u << 10,
    // A declarator whose parameter list was written K&R style.
    kCFlagKandR = 1u << 11,
    kCFlagAttrFallthrough = 1u << 12,
};

struct CNode;
using CNodePtr = std::unique_ptr<CNode>;

// One syntax-tree node. Children live in named vectors wherever the
// analysis half asks a structural question about them ("what is this
// function's body?", "what does this declaration declare?"); everything
// else -- operands, call arguments, an initializer -- is a positional
// `kids` entry, documented per kind in CNodeKind above.
struct CNode {
    CNodeKind kind = CNodeKind::Placeholder;
    CPos start;
    CPos end;
    std::string name;       // identifier / operator / literal text, per kind
    std::string str_value;  // decoded string, specifier spelling, ...
    // Where `name` itself sits, when that is narrower than the whole
    // node: the member of `p->next`, the name of a function definition.
    // Hit testing (hover, definition, rename) needs the name's own span.
    CPos name_pos;
    int name_end_col = 0;
    CType type;
    unsigned flags = 0;
    std::vector<CNodePtr> kids;
    std::vector<CNodePtr> body;
    std::vector<CNodePtr> params;
    std::vector<CNodePtr> orelse;
};

// --- Parse result -----------------------------------------------------

// A comment, kept out of the tree (C's grammar has no place for one) but
// needed by folding, by the "offer no completions in here" check, and by
// the fallthrough and `unused` conventions people write in comments.
struct CComment {
    int line = 0;
    int col = 0;       // byte column of the `/`
    int end_line = 0;  // last line of a `/* */` run
    int end_col = 0;
    std::string text;  // the comment including its delimiters
    bool own_line = false;
    bool block = false;
};

struct CParseResult {
    CNodePtr unit;  // always non-null, even for an empty or badly broken file
    std::vector<CSyntaxError> errors;
    std::vector<CComment> comments;
    std::vector<CDirective> directives;
    // Every token of the file in order, inactive arms and directive lines
    // included (see CToken::active). Kept because several features are
    // genuinely lexical: "is this position inside a string or comment",
    // signature help's backwards scan for the open call bracket, and the
    // `.`/`->`-triggered completion context.
    std::vector<CToken> tokens;
    // Bracket nesting depth at the start of each 0-based physical line,
    // counting only active tokens. Folding and the indentation-shaped
    // checks use it.
    std::vector<int> depth_at_line;
    // The typedef names this file itself introduces, in declaration
    // order. The parser has to track them anyway to resolve `A * b;`, and
    // the analysis half wants the same list.
    std::vector<std::string> typedef_names;
};

/**
 * @brief Lexes C source into tokens, directives and comments, following backslash line continuations.
 * @param lines the source, one entry per line (no trailing newlines)
 * @param out_errors receives lexical errors (unterminated string, comment or character constant, stray byte)
 * @param out_comments receives every comment, in document order
 * @param out_directives receives every preprocessor directive, in document order
 * @return the token stream, always ending with a single End token; every token is still marked active
 */
std::vector<CToken> TokenizeC(const std::vector<std::string> &lines, std::vector<CSyntaxError> *out_errors,
                              std::vector<CComment> *out_comments, std::vector<CDirective> *out_directives);

/**
 * @brief Chooses one arm of every conditional group and marks the tokens and directives outside it inactive.
 * @param tokens the token stream to annotate, as returned by TokenizeC
 * @param directives the directive list to annotate (group links and `taken` are filled in here)
 * @param out_errors receives unbalanced-conditional errors (`#endif` without `#if`, a group still open at EOF)
 */
void SelectCPreprocessorArms(std::vector<CToken> &tokens, std::vector<CDirective> &directives,
                             std::vector<CSyntaxError> *out_errors);

/**
 * @brief Parses C source into a syntax tree, recovering from errors rather than stopping at the first one.
 * @param lines the source, one entry per line (no trailing newlines)
 * @return the translation unit, every error found, and the lexical by-products the features need
 */
CParseResult ParseC(const std::vector<std::string> &lines);

/** @brief Reports whether `name` is a reserved C keyword, or one of the GNU spellings this parser honours. */
bool IsCKeyword(const std::string &name);

/** @brief Reports whether `name` is a type-specifier keyword (`int`, `struct`, `_Bool`, `typeof`, ...). */
bool IsCTypeKeyword(const std::string &name);

/** @brief Reports whether a byte may start an identifier (ASCII rules plus any non-ASCII lead byte). */
bool IsCIdentStart(unsigned char c);

/** @brief Reports whether a byte may continue an identifier. */
bool IsCIdentChar(unsigned char c);

#endif  // MEP_C_AST_H
