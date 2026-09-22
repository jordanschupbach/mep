#ifndef MEP_CPP_AST_H
#define MEP_CPP_AST_H

#include <map>
#include <memory>
#include <string>
#include <vector>

// The front end of mep's own C++ language server: a hand-written
// tokenizer, a preprocessor and a recursive-descent parser for C++20,
// plus the syntax tree all three feed (cpp_lsp.h is the analysis half
// that consumes this, cpp_lsp_server.cpp the JSON-RPC wire half).
//
// Why a real parser, when org_lsp.cpp deliberately refuses to build one:
// the same reason python_ast.h gives. "This member does not exist on that
// class", "this constructor's initializer list is out of order", "this
// local is written and never read" are questions about *scopes*, and a
// scope is exactly what a line-oriented scan cannot see.
//
// What makes C++ different from the Python front end, and how this file
// answers it:
//   - **The preprocessor is part of the language.** Directives are
//     collected and evaluated here (see CppPreprocessor below), not
//     skipped: `#if`/`#ifdef` groups are resolved against the macros the
//     file itself defines plus a small host-shaped predefined set, and
//     only the surviving tokens reach the parser. Feeding the parser both
//     halves of an `#if/#else` would invent redefinitions and unbalanced
//     braces in every real file; skipping directives entirely would drop
//     `extern "C" {` on the floor for the same reason.
//   - **Macros are expanded**, object-like and function-like both, but
//     only the ones this file defines. A macro from a header nobody read
//     stays an identifier, which is why the declaration parser is written
//     to tolerate unknown identifiers wherever a declaration specifier
//     may appear (`MEP_EXPORT void f();` parses, and so does a bare
//     `Q_OBJECT` in a class body).
//   - **The grammar is ambiguous without a symbol table.** `a * b;` is a
//     declaration or a multiplication depending on what `a` is, and `f<g>
//     (x)` on whether `f` is a template. This parser resolves both with
//     documented heuristics (IsDeclarationAhead, TemplateArgumentsAhead)
//     rather than pretending to do two-phase name lookup. Being wrong
//     costs a symbol's kind, never a spurious syntax error: an ambiguous
//     statement that fails to parse as a declaration is re-parsed as an
//     expression.
//
// Scope, named rather than silently omitted:
//   - Syntax coverage is C++20-shaped: templates (including variadic
//     packs, `requires` clauses and concepts), lambdas with capture
//     lists and template parameters, structured bindings, `co_await`/
//     `co_yield`/`co_return`, `consteval`/`constinit`, three-way
//     comparison, attributes, `explicit(bool)`, designated initializers,
//     range-for with an init-statement, and `if constexpr`. C++23's
//     `if consteval`, deducing `this` and multidimensional `operator[]`
//     parse too. Modules (`import`/`export module`) are recognized at
//     statement level so a module file still yields symbols, but nothing
//     resolves an imported module's contents.
//   - No template instantiation, no overload resolution, no type
//     deduction beyond what is written down (`auto x = C();` knows `C`,
//     `auto x = f();` does not). A single translation unit -- in fact a
//     single *file*, plus any quoted header sitting next to it -- is the
//     whole world.
//   - The parser recovers rather than stopping at the first error: an
//     unparsable declaration is reported once, skipped to the next
//     plausible boundary (`;` or a balanced `}`), and parsing continues,
//     so a file being typed into still produces symbols and completions
//     for the parts that are fine.
//
// Positions are 0-based lines and 0-based *byte* columns throughout, the
// same convention org_lsp.h and python_ast.h document and for the same
// reason: mep's LSP client feeds `character` straight to a byte column,
// and the server declares `positionEncoding: "utf-8"` to match.

// --- Positions --------------------------------------------------------

struct CppPos {
    int line = 0;  // 0-based
    int col = 0;   // 0-based byte column
};

/** @brief Orders two positions by line, then byte column. */
bool CppPosLess(const CppPos &a, const CppPos &b);

// --- Tokens -----------------------------------------------------------

enum class CppTokKind {
    End,      // end of input
    Ident,    // identifier that is not a reserved keyword
    Keyword,  // reserved keyword (`class`, `int`, `constexpr`, ...)
    Number,   // preprocessing number (integer, float, any suffix)
    String,   // string literal, prefixes/suffix included in `text`
    Char,     // character literal, prefixes/suffix included in `text`
    Op,       // operator or punctuator, `#` and `##` included
};

struct CppToken {
    CppTokKind kind = CppTokKind::End;
    std::string text;  // exactly the source bytes this token covers
    CppPos start;
    CppPos end;  // exclusive
    // True for the first token of a physical line (only whitespace before
    // it). The preprocessor needs this to recognize a directive, which is
    // the one place C++ is line-sensitive.
    bool bol = false;
    // True when whitespace or a comment preceded this token. Function-like
    // macro expansion needs it to rebuild spelling faithfully.
    bool space_before = false;
    // String/char tokens only: the closing quote is missing. The token is
    // still emitted (ending at end of line) so the parser keeps its place.
    bool unterminated = false;
    // True for a token the preprocessor produced rather than the file: its
    // positions are the macro *invocation's*, so a diagnostic on it points
    // somewhere the reader can see.
    bool from_macro = false;
    // The line the token's directive belongs to, or -1 outside a
    // directive. Directive tokens never reach the parser; the lexer keeps
    // them so the preprocessor can read them in place.
    int directive_line = -1;
};

// A lexical, preprocessor or grammar problem, in the {code, message}
// vocabulary the rest of the server uses (cpp_lsp.h turns these into LSP
// diagnostics unchanged).
struct CppSyntaxError {
    CppPos start;
    CppPos end;
    std::string code;
    std::string message;
};

// --- Preprocessor -----------------------------------------------------

enum class CppDirectiveKind {
    None,  // `#` alone: a null directive
    Include,
    Define,
    Undef,
    If,
    Ifdef,
    Ifndef,
    Elif,
    ElifDef,
    ElifNdef,
    Else,
    Endif,
    Pragma,
    Error,
    Warning,
    Line,
    Unknown,
};

// One `#...` line, with the pieces every consumer needs already split
// out. Directives are not part of the syntax tree -- C++'s grammar has no
// place for them -- so folding, symbols and diagnostics read them from
// here instead.
struct CppDirective {
    CppDirectiveKind kind = CppDirectiveKind::Unknown;
    CppPos start;      // the `#`
    CppPos end;        // end of the directive's last token
    int line = 0;      // the `#`'s line
    int end_line = 0;  // last physical line, differs when backslash-continued
    std::string name;  // macro name, pragma word, or the unknown directive's word
    // #include only: the spelled header, brackets/quotes stripped.
    std::string header;
    bool angled = false;  // `<h>` rather than `"h"`
    // #define only.
    bool function_like = false;
    bool variadic = false;
    std::vector<std::string> params;
    // The tokens after the directive's name: a macro's replacement list, a
    // conditional's expression, `#error`'s message.
    std::vector<CppToken> body;
    // Conditionals only: whether this group's tokens reach the parser, and
    // whether the decision was made from macros this file actually defines
    // (`certain`) or from the standard's "an undefined identifier is 0"
    // rule (`!certain`, i.e. a guess a build system could overturn).
    bool active = false;
    bool certain = true;
    int depth = 0;  // nesting depth of the enclosing conditional group
    // #if/#ifdef/#ifndef: the line of the matching #endif, or -1 when the
    // conditional is never closed. Folding wants the whole region.
    int end_directive_line = -1;
};

// A `#define`, as the expander needs it.
struct CppMacro {
    std::string name;
    bool function_like = false;
    bool variadic = false;
    std::vector<std::string> params;
    std::vector<CppToken> body;
    CppPos pos;         // where the name is defined, for go-to-definition
    int line = 0;
    bool predefined = false;  // one of this server's host-shaped predefines
};

// --- Syntax tree ------------------------------------------------------

enum class CppNodeKind {
    TranslationUnit,
    // Declarations
    Namespace,       // name ("" = anonymous), body; str_value = "inline" when inline
    NamespaceAlias,  // name = alias, str_value = target
    UsingDirective,  // name = namespace named by `using namespace X;`
    UsingDecl,       // name = the last component, str_value = the whole qualified id
    UsingAlias,      // name = alias, kids[0] = aliased type
    Typedef,         // one node per declarator: name, kids[0] = type
    Class,           // name, str_value = "class"/"struct"/"union", bases, body
    Enum,            // name, body = Enumerator nodes; str_value = "enum"/"enum class"
    Enumerator,      // name, kids[0] = value (optional)
    Function,        // name, params, kids[0] = return type, kids[1..] = MemberInit, body (when defined)
    MemberInit,      // a constructor's `: member(args)`: name = member or base, kids[] = arguments
    Param,           // name, kids[0] = type, kids[1] = default argument
    Variable,        // name, kids[0] = type, kids[1] = initializer
    Field,           // a Variable inside a class body (same layout)
                     // Field and a class member Function both carry the
                     // access specifier they were declared under in
                     // `str_value` ("public"/"protected"/"private").
    BaseSpecifier,   // name = base class, str_value = access
    Access,          // name = "public"/"protected"/"private"
    LinkageSpec,     // name = "C"/"C++", body
    StaticAssert,    // kids[0] = condition, kids[1] = message (optional)
    TemplateDecl,    // only when the declaration under it would not parse;
                     // a parsed one carries its own `template_params`
    TemplateParam,   // name, str_value = "type"/"non-type"/"template", kids[0] = type/default
    Concept,         // name, kids[0] = constraint expression
    Friend,          // kids[0] = the befriended declaration
    ModuleDecl,      // name = module name, str_value = "module"/"export module"/"import"
    Attribute,       // name = the attribute's spelling, kept for hover
    Empty,           // a stray `;`
    // Statements
    Compound,   // body
    DeclStmt,   // kids[] = the declarations this statement introduces
    ExprStmt,   // kids[0] = expression
    If,         // kids[0] = init (optional), kids[1] = condition, body, orelse
    For,        // kids[0] = init, kids[1] = condition, kids[2] = increment, body
    RangeFor,   // kids[0] = init (optional), kids[1] = the loop declaration, kids[2] = range, body
    While,      // kids[0] = condition, body
    DoWhile,    // body, kids[0] = condition
    Switch,     // kids[0] = condition, body
    Case,       // kids[0] = value, body
    Default,    // body
    Return,     // kids[0] = value (optional)
    Break,
    Continue,
    Goto,   // name = label
    Label,  // name, body[0] = the labelled statement
    Try,    // body, handlers
    Catch,  // kids[0] = the exception declaration (a Param, or absent for `...`), body
    AsmStmt,
    CoReturn,  // kids[0] = value (optional)
    // Expressions
    Id,          // name = the last component, str_value = the whole qualified id
    Literal,     // name = the literal text; str_value = "number"/"string"/"char"/"bool"/"nullptr"
    Call,        // kids[0] = callee, kids[1..] = arguments
    Member,      // kids[0] = object, name = member, str_value = "."/"->"
    Subscript,   // kids[0] = object, kids[1..] = indices
    Unary,       // name = operator, str_value = "prefix"/"postfix", kids[0]
    Binary,      // name = operator, kids[0], kids[1]
    Conditional, // kids[0] = condition, kids[1] = then, kids[2] = else
    Cast,        // name = "static_cast"/"c-style"/..., kids[0] = type, kids[1] = operand
    New,         // kids[0] = type, kids[1..] = placement/constructor arguments
    Delete,      // kids[0] = operand, str_value = "[]" for `delete[]`
    Lambda,      // params, body, kids[0] = return type (optional); str_value = capture text
    SizeOf,      // name = "sizeof"/"alignof"/"decltype"/"typeid"/"noexcept", kids[0]
    This,
    InitList,    // kids[] = elements (a braced-init-list, designators included)
    Designator,  // name = the field, kids[0] = value
    Throw,       // kids[0] = value (optional)
    Fold,        // name = operator, kids[] = operands of a fold expression
    TypeExpr,    // a type used where an expression was expected (`sizeof(int)`)
    Paren,       // kids[0]; kept so `if ((a = b))` can be told from `if (a = b)`
    // Types
    Type,        // see CppNode::type_* below
    Placeholder, // an omitted slot, or an expression that would not parse
};

struct CppNode;
using CppNodePtr = std::unique_ptr<CppNode>;

// One syntax-tree node. Same layout convention python_ast.h uses: named
// child vectors wherever the analysis half asks a structural question
// ("what is this function's body?", "what does this class derive from?"),
// positional `kids` for everything else, documented per kind above.
struct CppNode {
    CppNodeKind kind = CppNodeKind::Placeholder;
    CppPos start;
    CppPos end;
    std::string name;       // identifier / operator / literal text, per kind
    std::string str_value;  // qualifier, access, capture text, ... per kind
    // Where `name` itself sits, when that is narrower than the whole node:
    // the member of `a.b`, the name of a function definition. Hit testing
    // (hover, go to definition, rename) needs the name's own span.
    CppPos name_pos;
    CppPos name_end;
    // A declarator id written with a nested-name-specifier, spelled in
    // full ("Editor::DrawPane"). Empty when the declaration's name was
    // unqualified, which is the usual case.
    std::string qualified;
    // Type nodes only: the written spelling (`const std::vector<int> &`),
    // the bare name that spelling names (`std::vector`), and its template
    // arguments as written. `ptr_depth` counts `*`s, `is_ref` covers both
    // `&` and `&&`.
    std::string type_text;
    std::string type_base;
    std::vector<std::string> type_args;
    int ptr_depth = 0;
    bool is_ref = false;
    bool is_const = false;
    bool is_static = false;
    bool is_virtual = false;
    bool is_pure = false;      // `= 0`
    bool is_explicit = false;
    bool is_defaulted = false;  // `= default`
    bool is_deleted = false;    // `= delete`
    bool is_inline = false;
    bool is_constexpr = false;
    bool is_variadic = false;  // `...` in a parameter list or template pack
    bool is_definition = false;  // a class/function with a body, not just a declaration
    bool is_template = false;    // the declaration carries template parameters
    bool is_mutable = false;
    bool is_extern = false;
    bool is_override = false;
    bool is_noexcept = false;
    std::string trailing_qualifiers;  // a member function's `const`, `&&`, `noexcept`, ...
    std::vector<CppNodePtr> kids;
    std::vector<CppNodePtr> params;      // function/lambda parameters
    std::vector<CppNodePtr> template_params;  // `template<...>` parameters of this declaration
    std::vector<CppNodePtr> body;        // a suite, a class body, a namespace body
    std::vector<CppNodePtr> orelse;      // `else` branch
    std::vector<CppNodePtr> handlers;    // `catch` clauses
    std::vector<CppNodePtr> bases;       // base-clause specifiers
    std::vector<CppNodePtr> decorators;  // attributes written before the declaration
};

// --- Parse result -----------------------------------------------------

// A comment, kept out of the tree (C++'s grammar has no place for one)
// but needed by folding, by hover's "the documentation is the comment
// above the declaration" rule, and by the "offer no completions inside a
// comment" check.
struct CppComment {
    int line = 0;
    int col = 0;        // byte column of the `/`
    int end_line = 0;   // differs from `line` for a block comment
    int end_col = 0;
    std::string text;   // the comment including its introducer
    bool own_line = false;  // nothing but whitespace before it
    bool block = false;     // `/* */` rather than `//`
    bool doc = false;       // `///`, `//!`, `/**` or `/*!`
};

// An `#include` the preprocessor saw in an active group, with the
// resolution the analysis half asked for (empty `resolved` = not looked
// up, or not found).
struct CppInclude {
    int line = 0;
    int col = 0;         // byte column of the `#`
    int path_col = 0;    // byte column of the opening `<`/`"`
    int path_end = 0;    // one past the closing `>`/`"`
    std::string header;  // as spelled, delimiters stripped
    bool angled = false;
    std::string resolved;  // absolute path, when the server resolved it
};

struct CppParseResult {
    CppNodePtr unit;  // always non-null, even for an empty or badly broken file
    std::vector<CppSyntaxError> errors;
    std::vector<CppComment> comments;
    std::vector<CppDirective> directives;
    std::vector<CppInclude> includes;
    // Every macro the file defines, by name. A later `#undef` removes the
    // entry, so this is the state at end of file, which is what hover and
    // completion want.
    std::map<std::string, CppMacro> macros;
    // The tokens the parser actually saw (post-preprocessing), and the raw
    // ones the lexer produced (directives and inactive groups included).
    // Several features are genuinely lexical: "is this position inside a
    // string or comment", signature help's backwards scan for the open
    // call bracket, and the `.`/`->`/`::` completion context.
    std::vector<CppToken> tokens;
    std::vector<CppToken> raw_tokens;
    // For each 0-based physical line: the brace nesting depth at its
    // start, and whether the line survived conditional compilation.
    std::vector<int> depth_at_line;
    std::vector<bool> line_active;
    // True when the file's `#if` nesting never balanced, when an
    // unterminated block comment or raw string ran to end of file, or when
    // the token budget ran out -- i.e. when the tree is known to be a
    // partial view and scope-level diagnostics must stay quiet.
    bool truncated = false;
};

// Options the front end itself needs. Everything filesystem-shaped lives
// here rather than being discovered, so the parser stays a pure function
// in tests (see cpp_lsp.h's CppLspOptions, which carries a superset).
struct CppParseOptions {
    // Directory the document lives in; `#include "sibling.h"` resolves
    // against it. Empty disables include resolution entirely.
    std::string doc_dir;
    // Extra directories a quoted include may resolve against, tried in
    // order after `doc_dir` (a project's own `include/`, say).
    std::vector<std::string> include_dirs;
    bool resolve_includes = false;
    // Macros to treat as defined before the file starts, beyond the
    // host-shaped set this server predefines (`__cplusplus`, `__GNUC__`,
    // ...). Each entry is `NAME` or `NAME=value`.
    std::vector<std::string> defines;
    // Parse as C rather than C++: no `class`, no templates, no `new`, and
    // `//`-comments are still fine (C99 onwards). Reserved for the C
    // server that will sit beside this one; the C++ paths ignore it.
    bool c_mode = false;
};

/**
 * @brief Tokenizes C++ source, handling line splices, raw strings, comments and every literal prefix.
 * @param lines the source, one entry per line (no trailing newlines)
 * @param out_errors receives lexical errors (unterminated string/comment, stray character)
 * @param out_comments receives every comment, in document order
 * @return the token stream, always ending with a single End token
 */
std::vector<CppToken> TokenizeCpp(const std::vector<std::string> &lines, std::vector<CppSyntaxError> *out_errors,
                                  std::vector<CppComment> *out_comments);

/**
 * @brief Parses C++ source into a syntax tree, recovering from errors rather than stopping at the first one.
 * @param lines the source, one entry per line (no trailing newlines)
 * @param opts include-resolution and predefined-macro options
 * @return the translation unit, every error found, and the lexical by-products features need
 */
CppParseResult ParseCpp(const std::vector<std::string> &lines, const CppParseOptions &opts);

/** @brief Parses with default options (no filesystem access, no extra defines). */
CppParseResult ParseCpp(const std::vector<std::string> &lines);

/** @brief Reports whether `name` is a reserved C++ keyword (`final` and `override` are not). */
bool IsCppKeyword(const std::string &name);

/** @brief Reports whether `name` is one of the fundamental type keywords (`int`, `double`, `void`, ...). */
bool IsCppFundamentalType(const std::string &name);

/** @brief Reports whether a byte may start an identifier (ASCII rules plus any non-ASCII lead byte). */
bool IsCppIdentStart(unsigned char c);

/** @brief Reports whether a byte may continue an identifier. */
bool IsCppIdentChar(unsigned char c);

/** @brief Renders a type node the way it was written, for hover and completion detail text. */
std::string CppTypeText(const CppNode *type);

/** @brief Renders a function's signature ("connect(const Host &h, int port = 80) const"). */
std::string CppSignatureText(const CppNode *fn);

#endif  // MEP_CPP_AST_H
