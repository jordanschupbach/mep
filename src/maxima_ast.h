#ifndef MEP_MAXIMA_AST_H
#define MEP_MAXIMA_AST_H

#include <string>
#include <vector>

// The Maxima reader behind mep's own Maxima language server: a
// hand-written lexer and Pratt parser producing a small AST, plus the
// syntax diagnostics found on the way. maxima_lsp.cpp turns that AST into
// diagnostics/completion/hover/symbols; maxima_lsp_server.cpp puts it on
// the wire as `mep-maxima-lsp`.
//
// Hand-written for the reasons r_lsp_parse.h gives, and with one more
// that is specific to this language: there is no tree-sitter grammar for
// Maxima to vendor in the first place, and the reader Maxima itself ships
// (src/nparse.lisp) only exists inside a running Lisp image. So the
// binding powers below are not guessed -- they are the `lbp`/`rbp`
// properties that same file sets, read out of a live Maxima and written
// down here, which is why `^` binds tighter than unary minus (140 vs 134)
// and why `a : b : c` groups to the right (lbp 180, rbp 20).
//
// Scope, named rather than silently omitted:
//   - This parses Maxima *source*, not Maxima semantics. Nothing is
//     evaluated, simplified or type-checked, and a user-defined
//     `infix("@@")` operator is not seen: the operator table is the one
//     the language starts with.
//   - `:lisp` escapes to the host Lisp are recognized and skipped, not
//     parsed. A server that tried to read Common Lisp here would be
//     wrong more often than silent.
//   - Comments are collected into a separate list rather than attached to
//     the nodes around them, so the AST is not round-trippable.
//
// Positions are 0-based lines and 0-based *byte* columns throughout, the
// convention every other mep server uses and what its LSP client reads.

// --- Source ranges ----------------------------------------------------

struct MxRange {
    int line = 0;      // 0-based start line
    int col = 0;       // 0-based start byte column
    int end_line = 0;  // 0-based end line
    int end_col = 0;   // 0-based end byte column, exclusive
};

/** @brief Reports whether a (line, col) position lies inside a range, end-exclusive. */
bool MxRangeContains(const MxRange &r, int line, int col);

// --- Tokens -----------------------------------------------------------

enum class MxTokKind {
    End,    // end of input
    Ident,  // a name: letters, digits, `_`, `%`, and \-escaped characters
    Lisp,   // ?name or ?:keyword -- a symbol in the host Lisp package
    Num,    // 1, 1.5, .5, 1., 1.5e3, 1.5b0 (bigfloat), 1d0, 1f0, 1s0, 1l0
    Str,    // "..." with \-escapes
    Op,     // every operator, spelled out in `text` (including `and`/`or`/`not`)
    LParen,
    RParen,
    LBracket,  // [ -- a list literal, or a subscript when it follows a value
    RBracket,
    LBrace,  // { -- a set literal
    RBrace,
    Comma,
    Semi,    // ; -- terminate and display
    Dollar,  // $ -- terminate silently
    Quote,      // '  -- suppress evaluation of what follows
    QuoteEval,  // '' -- evaluate what follows an extra time
    // Keywords. Maxima is case-sensitive, so `If` is an ordinary name and
    // only these exact spellings are keywords. `in` is deliberately not
    // among them: it has no nud in Maxima's own grammar, so it is only a
    // clause word inside a loop header and an ordinary identifier
    // everywhere else -- `in: 0.0254 * meter` is a real line in the
    // physical-units package.
    If,
    Then,
    Else,
    Elseif,
    For,
    From,
    Step,
    Next,
    Thru,
    While,
    Unless,
    Do,
    Label,  // && -- the tag Maxima lets a statement be named with
};

struct MxToken {
    MxTokKind kind = MxTokKind::End;
    // Identifiers: the name with `\` escapes resolved. Strings: the
    // decoded value. Numbers and operators: exactly as written.
    std::string text;
    MxRange range;
    bool unterminated = false;  // a string that ran to end of input
};

struct MxComment {
    std::string text;  // the whole comment, `/*` and `*/` included
    MxRange range;
};

// --- AST --------------------------------------------------------------

enum class MxNodeKind {
    Error,  // a construct that failed to parse; kids hold whatever was salvaged
    Num,
    Str,
    Ident,     // text is the name
    Lisp,      // text is the `?`-prefixed host-Lisp name, `?` included
    List,      // [a, b, c] -- kids are the elements
    Set,       // {a, b, c} -- kids are the elements
    Call,      // kids[0] is the callee; args are the arguments
    Index,     // kids[0] is the object; args are the subscripts (a[i, j])
    Paren,     // ( a, b ): kids are the expressions -- more than one is an mprogn
    Binary,    // text is the operator, kids = {lhs, rhs}
    Unary,     // text is the prefix operator, kids = {operand}
    Postfix,   // text is `!` or `!!`, kids = {operand}
    Quote,     // text is "'" or "''", kids = {operand}
    If,        // args: "cond"/"then" pairs in order, then an optional "else"
    Do,        // args keyed by clause: for/from/in/step/next/thru/while/unless/do
    LispEsc,   // a `:lisp ...` line, kept whole in `text` and not parsed
    Label,     // `name && expr`: kids = {tag, expr}
};

// One argument of a call or subscript, one element of a clause list, or
// one branch of an `if`.
struct MxArg {
    // "" for a plain positional argument. An `if` uses "cond"/"then"/
    // "else"; a `do` uses its clause keyword ("for", "thru", "do", ...).
    std::string name;
    int value = -1;  // node index, or -1 for an empty slot
    MxRange range;   // the clause keyword or the argument, for diagnostics
};

struct MxNode {
    MxNodeKind kind = MxNodeKind::Error;
    MxRange range;
    std::string text;
    std::vector<int> kids;
    std::vector<MxArg> args;
};

// --- Statements -------------------------------------------------------

// One top-level expression and the terminator that closed it. Maxima
// requires one: `;` displays the value, `$` does not, and a file whose
// last expression has neither is a file Maxima reads past the end of.
struct MxStatement {
    int node = -1;
    bool terminated = false;
    char terminator = ';';  // ';' or '$', meaningful only when terminated
    MxRange range;          // the expression plus its terminator
};

// --- User-declared operators ------------------------------------------

// An operator this document declares for itself. Maxima lets a file add
// to its own grammar -- `infix("~")`, `prefix("grad")`, `postfix("!!!")`,
// `nary("&&&")` -- and the share packages use that heavily, so a reader
// that only knew the built-in table would reject the vector-algebra and
// units libraries outright.
struct MxUserOperator {
    std::string text;   // the spelling, exactly as the declaration spells it
    std::string close;  // a matchfix closer, "" for every other kind
    int lbp = 0;        // 0 when it has no infix/postfix form
    int rbp = 0;
    int prefix_bp = 0;  // 0 when it has no prefix form
    bool postfix = false;
    // "" when the file declared this itself; otherwise the shipped
    // package that declares it, which this file loads.
    std::string package;
    MxRange range;  // the declaring call, or the `load` that brought it in
};

/**
 * @brief Finds the `load`/`batch`/`batchload` calls in a token stream and returns the package names they name.
 * @param tokens a token stream from MxTokenize
 * @return each loaded package's bare name (no directory, no extension), in document order
 */
std::vector<std::string> MxScanLoadedPackages(const std::vector<MxToken> &tokens);

/**
 * @brief The operators the shipped Maxima packages declare, for a file that loads one of them.
 * @return one entry per spelling, with the package that declares it
 *
 * `load("vect.mac")` makes `~` an infix operator and `grad` a prefix one
 * for the rest of the file. Reading those without the package would mean
 * guessing that any name in front of another name is an operator, which
 * is what a missing `;` looks like -- so the guess is not made, and this
 * table is consulted only for a package the file actually loads.
 */
const std::vector<MxUserOperator> &MxPackageOperators();

/**
 * @brief Finds the `infix`/`prefix`/`postfix`/`nary`/`nofix`/`matchfix` declarations in a token stream.
 * @param tokens a token stream from MxTokenize
 * @return one entry per declaration, in document order (a spelling declared twice appears twice)
 *
 * Run before the real parse, because a declaration changes how the rest
 * of the file *lexes*: `@@` is two `@` operators until something says it
 * is one. The spelling itself is always a string literal, which is why
 * this can be read off a token stream that has not seen it yet.
 */
std::vector<MxUserOperator> MxScanOperatorDeclarations(const std::vector<MxToken> &tokens);

// --- Parse errors -----------------------------------------------------

struct MxParseError {
    MxRange range;
    std::string code;     // stable id: "syntax-error", "unclosed-paren", ...
    std::string message;  // human wording, free to improve
};

// How deeply one expression may nest before the reader gives up. Nothing
// a person writes comes close; what does come close is generated input,
// and this parser is recursive -- an editor that reads a file with
// twenty thousand nested parentheses must say so rather than run off the
// end of its stack. Both consumers of this reader care: the language
// server would lose the process, and the formatter runs inside the
// editor itself.
constexpr int kMxMaxNesting = 400;

struct MxParseResult {
    std::vector<MxNode> nodes;  // the arena; every index above refers into this
    std::vector<MxStatement> statements;
    std::vector<MxComment> comments;
    std::vector<MxParseError> errors;
    std::vector<MxUserOperator> user_operators;  // what this file declared for itself

    /** @brief Reports whether the document parsed cleanly. */
    bool ok() const { return errors.empty(); }
    /** @brief Borrows a node by index (index -1 or out of range yields a shared empty node). */
    const MxNode &at(int index) const;
};

/**
 * @brief Tokenizes Maxima source, collecting comments separately from the token stream.
 * @param lines the document's text, one entry per line (no trailing newlines)
 * @param comments_out receives every comment found, in document order (may be null)
 * @param errors_out receives lexical errors -- an unterminated string or comment (may be null)
 * @return the token stream, always ending in an MxTokKind::End token
 */
std::vector<MxToken> MxTokenize(const std::vector<std::string> &lines, std::vector<MxComment> *comments_out,
                                std::vector<MxParseError> *errors_out,
                                const std::vector<MxUserOperator> *user_ops = nullptr);

/**
 * @brief Parses Maxima source into an AST, recovering at terminators so one bad statement does not lose the file.
 * @param lines the document's text, one entry per line
 * @return the arena, the statements, the comments and every syntax error found
 */
MxParseResult MxParse(const std::vector<std::string> &lines);

// --- Operator table (shared with the analysis half and the tests) -----

// What the parser knows about one operator spelling. `lbp` is its left
// binding power (0 when it cannot appear in infix/postfix position),
// `rbp` the power its right operand is parsed at, and `prefix_bp` the
// power a prefix use parses its operand at (0 when it has no prefix
// form). These are Maxima's own numbers -- see the file header.
struct MxOperatorInfo {
    const char *text;
    int lbp;
    int rbp;
    int prefix_bp;
    bool postfix;  // `!`/`!!`: a left operand and no right one
};

/** @brief Looks up an operator's binding powers by exact spelling, or null when it is not one. */
const MxOperatorInfo *MxFindOperator(const std::string &text);

/** @brief Every operator spelling the reader knows, in table order. */
const std::vector<MxOperatorInfo> &MxOperatorTable();

/** @brief Reports whether a character may appear in a Maxima name (letters, digits, `_`, `%`). */
bool MxIsNameChar(char c);

/** @brief Reports whether a character may start a Maxima name (a letter, `_` or `%`). */
bool MxIsNameStart(char c);

#endif  // MEP_MAXIMA_AST_H
