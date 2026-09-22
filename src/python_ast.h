#ifndef MEP_PYTHON_AST_H
#define MEP_PYTHON_AST_H

#include <memory>
#include <string>
#include <vector>

// The front end of mep's own Python language server: a hand-written
// tokenizer and recursive-descent parser for Python 3, plus the syntax
// tree both feed (python_lsp.h is the analysis half that consumes this,
// python_lsp_server.cpp the JSON-RPC wire half).
//
// Why a real parser here, when org_lsp.cpp deliberately refuses to build
// an element tree: Python's interesting mistakes are not local. "This
// name is never defined", "this import is never used", "this local is
// assigned and never read" are all questions about *scopes*, and a scope
// is exactly the thing a line-oriented scan cannot see. So this file pays
// for a tree, and pays for it once -- every feature (diagnostics,
// completion, hover, symbols, folding, definition, references, signature
// help) answers from the same parse.
//
// Scope, named rather than silently omitted:
//   - Syntax coverage is Python 3.12-shaped: comprehensions, walrus,
//     decorators, async/await, positional-only and keyword-only
//     parameters, star-unpacking, annotated assignment, f-strings (their
//     interpolations are parsed as real expressions, see ParsePython's
//     `fstring_parts`), `match`/`case`, `except*`, PEP 695 `type` aliases
//     and bracketed type parameters. Python 2 syntax is not supported and
//     is reported with a message that says so rather than a generic
//     "invalid syntax" (`print x`, `except E, e:`, `<>`).
//   - No evaluation, no imports followed, no type inference beyond "this
//     name was assigned a literal / a call to a class defined in this
//     file". A single file is the whole world; nothing here reads another
//     module's source.
//   - The parser recovers rather than stopping at the first error: an
//     unparsable statement is reported, skipped to the next line at a
//     usable indent, and parsing continues, so a file being typed into
//     still produces symbols and completions for the parts that are fine.
//
// Positions are 0-based lines and 0-based *byte* columns throughout, the
// same convention org_lsp.h documents and for the same reason: mep's LSP
// client feeds `character` straight to a byte column, and the server
// declares `positionEncoding: "utf-8"` to match.

// --- Positions --------------------------------------------------------

struct PyPos {
    int line = 0;  // 0-based
    int col = 0;   // 0-based byte column
};

// --- Tokens -----------------------------------------------------------

enum class PyTokKind {
    End,      // end of input
    Newline,  // a logical line ended (never emitted inside brackets)
    Indent,
    Dedent,
    Name,     // identifier that is not a reserved keyword
    Keyword,  // reserved keyword (`if`, `def`, `None`, ...); soft keywords are Name
    Number,
    String,  // any string/bytes literal, prefixes and quotes included in `text`
    Op,      // operator or delimiter
};

struct PyToken {
    PyTokKind kind = PyTokKind::End;
    std::string text;  // exactly the source bytes this token covers
    PyPos start;
    PyPos end;  // exclusive
    // String tokens only: the lowercased prefix letters (`f`, `rb`, ...),
    // so the parser can spot an f-string without re-scanning the text.
    std::string str_prefix;
    // True for a string token whose closing quote is missing; the
    // tokenizer still emits the token (ending at end of line/file) so the
    // parser can keep its place.
    bool unterminated = false;
};

// A lexical problem, reported with the same {code, message} vocabulary
// the rest of the server uses. These are produced by the tokenizer
// itself, before any grammar is involved: unterminated strings, bad
// indentation, tab/space mixing, characters Python has no token for.
struct PySyntaxError {
    PyPos start;
    PyPos end;
    std::string code;
    std::string message;
};

// --- Syntax tree ------------------------------------------------------

enum class PyNodeKind {
    Module,
    // Statements
    FunctionDef,  // `is_async` distinguishes `async def`
    ClassDef,
    Return,
    Delete,
    Assign,     // targets[], kids[0] = value
    AugAssign,  // targets[0], name = op text ("+="), kids[0] = value
    AnnAssign,  // targets[0], kids[0] = annotation, kids[1] = value (optional)
    For,        // targets[], kids[0] = iterable, body, orelse
    While,      // kids[0] = test, body, orelse
    If,         // kids[0] = test, body, orelse
    With,       // kids[] = WithItem, body
    WithItem,   // kids[0] = context expression, targets[0] = optional `as` target
    Raise,      // kids[0] = exception (optional), kids[1] = `from` value (optional)
    Try,        // body, handlers, orelse, finalbody
    ExceptHandler,  // kids[0] = type (optional), name = bound name, body
    Assert,         // kids[0] = test, kids[1] = message (optional)
    Import,         // kids[] = Alias
    ImportFrom,     // name = module (dots included), kids[] = Alias
    Alias,          // name = imported name (`*` for a star import), str_value = `as` name
    Global,         // kids[] = Name
    Nonlocal,       // kids[] = Name
    ExprStmt,       // kids[0] = expression
    Pass,
    Break,
    Continue,
    Match,      // kids[0] = subject, body = Case nodes
    Case,       // kids[0] = pattern, kids[1] = guard (optional), targets[] = captures, body
    TypeAlias,  // PEP 695 `type X = ...`: name, kids[0] = value
    // Expressions
    BoolOp,     // name = "and"/"or", kids[]
    BinOp,      // name = operator text, kids[0], kids[1]
    UnaryOp,    // name = operator text, kids[0]
    Compare,    // kids[0] = left, then one kid per comparator; name = ops joined by ' '
    Lambda,     // params, kids[0] = body
    IfExp,      // kids[0] = body, kids[1] = test, kids[2] = orelse
    Dict,       // kids[] alternating key, value (a `**expr` entry has a null key slot)
    Set,        // kids[]
    List,       // kids[]
    Tuple,      // kids[]
    ListComp,   // kids[0] = element, kids[1..] = Comprehension
    SetComp,
    DictComp,       // kids[0] = key, kids[1] = value, kids[2..] = Comprehension
    GeneratorExp,   // kids[0] = element, kids[1..] = Comprehension
    Comprehension,  // targets[], kids[0] = iterable, kids[1..] = conditions
    Await,          // kids[0]
    Yield,          // kids[0] (optional)
    YieldFrom,      // kids[0]
    Call,           // kids[0] = callee, kids[1..] = Arg/Keyword nodes
    Keyword,        // name = keyword name ("" for `**kwargs`), kids[0] = value
    Attribute,      // kids[0] = value, name = attribute
    Subscript,      // kids[0] = value, kids[1] = index
    Slice,          // kids[] = lower/upper/step, any of them possibly a Placeholder
    Starred,        // name = "*" or "**", kids[0]
    Name,           // name = identifier
    Number,         // name = literal text
    Str,            // name = raw literal text (all concatenated pieces), str_value = decoded text
    FString,        // like Str; kids[] = the parsed `{...}` interpolations
    Constant,       // name = "None"/"True"/"False"/"..."
    NamedExpr,      // walrus: targets[0] = name, kids[0] = value
    Params,         // kids[] = Param
    Param,          // name = parameter name, str_value = "*"/"**"/"/"/"", kids[0] = annotation, kids[1] = default
                    // (an annotation-less parameter with a default carries a Placeholder in kids[0])
    Placeholder,    // an omitted slot (empty slice bound) or an unparsable expression
};

struct PyNode;
using PyNodePtr = std::unique_ptr<PyNode>;

// One syntax-tree node. Children live in named vectors rather than a
// single `kids` list wherever the analysis half asks a structural
// question about them ("what does this statement bind?", "what is this
// function's body?"); everything else -- operands, call arguments, the
// value of an assignment -- is a positional `kids` entry, documented per
// kind in PyNodeKind above.
struct PyNode {
    PyNodeKind kind = PyNodeKind::Placeholder;
    PyPos start;
    PyPos end;
    std::string name;       // identifier / operator / literal text, per kind
    std::string str_value;  // decoded string, alias `as` name, parameter star, ...
    // Where `name` itself sits, when that is narrower than the whole
    // node: the attribute of `pkg.mod.func`, the name of a `def`. Hit
    // testing (hover, go to definition, rename) needs the name's own
    // span, not the span of the expression it is part of.
    PyPos name_pos;
    bool is_async = false;
    std::vector<PyNodePtr> kids;
    std::vector<PyNodePtr> targets;     // assignment / loop / capture targets
    std::vector<PyNodePtr> body;        // suite
    std::vector<PyNodePtr> orelse;      // `else` suite
    std::vector<PyNodePtr> finalbody;   // `finally` suite
    std::vector<PyNodePtr> handlers;    // `except` clauses
    std::vector<PyNodePtr> decorators;  // `@...` lines above a def/class
    std::vector<PyNodePtr> params;      // Param nodes of a def/lambda
};

// --- Parse result -----------------------------------------------------

// A run of `#` comment lines, kept out of the tree (Python's grammar has
// no place for them) but needed by folding and by the "this is a comment,
// offer no completions" check.
struct PyComment {
    int line = 0;
    int col = 0;   // byte column of the `#`
    std::string text;  // the comment including its `#`
    bool own_line = false;  // nothing but whitespace before it
};

struct PyParseResult {
    PyNodePtr module;  // always non-null, even for an empty or badly broken file
    std::vector<PySyntaxError> errors;
    std::vector<PyComment> comments;
    // Every token of the file, in order, INDENT/DEDENT/NEWLINE included.
    // Kept because several features are genuinely lexical: "is this
    // position inside a string", signature help's backwards scan for the
    // open call bracket, and the `.`-triggered completion context.
    std::vector<PyToken> tokens;
    // Logical line continuations: for each 0-based physical line, the
    // bracket nesting depth *at its start*. Used by folding and by the
    // indentation checks, which must not treat a continuation line's
    // leading whitespace as indentation.
    std::vector<int> depth_at_line;
};

/**
 * @brief Tokenizes Python source, emitting NEWLINE/INDENT/DEDENT the way CPython's own tokenizer does.
 * @param lines the source, one entry per line (no trailing newlines)
 * @param out_errors receives lexical errors (unterminated string, bad dedent, tab/space mix, stray character)
 * @param out_comments receives every comment, in document order
 * @param out_depth receives the bracket nesting depth at the start of each line
 * @return the token stream, always ending with a single End token
 */
std::vector<PyToken> TokenizePython(const std::vector<std::string> &lines, std::vector<PySyntaxError> *out_errors,
                                    std::vector<PyComment> *out_comments, std::vector<int> *out_depth);

/**
 * @brief Parses Python source into a syntax tree, recovering from errors rather than stopping at the first one.
 * @param lines the source, one entry per line (no trailing newlines)
 * @return the module node, every syntax error found, and the lexical by-products features need
 */
PyParseResult ParsePython(const std::vector<std::string> &lines);

/** @brief Reports whether `name` is a reserved Python keyword (soft keywords like `match` are not). */
bool IsPythonKeyword(const std::string &name);

/** @brief Reports whether a byte may start an identifier (ASCII rules plus any non-ASCII lead byte). */
bool IsPythonIdentStart(unsigned char c);

/** @brief Reports whether a byte may continue an identifier. */
bool IsPythonIdentChar(unsigned char c);

#endif  // MEP_PYTHON_AST_H
