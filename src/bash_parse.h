#ifndef MEP_BASH_PARSE_H
#define MEP_BASH_PARSE_H

#include <memory>
#include <string>
#include <vector>

// A Bash parser for mep's own Bash language server (bash_lsp.h). Written
// from the Bash 5.3 manual and parse.y's grammar, not from tree-sitter's:
// the server's syntax diagnostics are meant to agree with `bash -n`, and a
// tree-sitter grammar's error recovery is not bash's (it reads some valid
// scripts as errors and some broken ones as fine).
//
// It reads; it never runs anything. Aliases are not expanded (bash -n
// does not expand them either), extglob is always on (a script that uses
// it turns it on, and a parser cannot know), and arithmetic is checked
// only for balanced parentheses (bash checks it at run time).
//
// Positions are 0-based lines and 0-based byte columns; ranges are
// end-exclusive.

struct BashRange {
    int line = 0;
    int col = 0;
    int end_line = 0;
    int end_col = 0;
};

/** @brief Reports whether (line, col) lies inside a range, end-exclusive. */
bool BashRangeContains(const BashRange &r, int line, int col);

/** @brief Reports whether (line, col) lies inside a range or right at its end (a cursor after the last character). */
bool BashRangeTouches(const BashRange &r, int line, int col);

struct BashList;
struct BashWord;

// A variable named in arithmetic: `i` in `$((i + 1))`, `n` in `a[n]`.
struct BashArithRef {
    std::string name;
    BashRange range;
    bool assigned = false;  // `x = 1`, `x += 2`, `x++`, `--x`
};

struct BashWordPart;

struct BashArith {
    BashRange range;  // the expression text, without the delimiters
    std::string text;
    std::vector<BashArithRef> refs;
    std::vector<BashWordPart> parts;  // the $-expansions inside it
};

enum class BashPartKind {
    Literal,       // unquoted text, escapes and globs included as written
    SingleQuoted,  // '...'
    AnsiC,         // $'...'
    DoubleQuoted,  // "..." -- `parts` holds what is inside
    Locale,        // $"..." -- `parts` holds what is inside
    Param,         // $x, ${...}
    CommandSub,    // $(...), `...`, ${ ...; } (bash 5.3)
    ArithSub,      // $((...)), $[...]
    ProcessSub,    // <(...), >(...)
    ExtGlob,       // ?(...) *(...) +(...) @(...) !(...)
};

struct BashWordPart {
    BashPartKind kind = BashPartKind::Literal;
    BashRange range;
    std::string text;     // the source text of the whole part
    bool quoted = false;  // inside double quotes (or a quoted here-document body's sense: not split)

    // Param: `name` is a variable name, positional digits, or one of
    // @ * # ? - $ ! 0 _.
    std::string name;
    BashRange name_range;
    bool braced = false;
    bool indirect = false;   // ${!x}, ${!prefix*}
    bool length = false;     // ${#x}
    bool has_subscript = false;
    std::string subscript;   // ${a[...]}: the raw index ("@", "*", "i+1", "key")
    BashArith subscript_arith;  // the index read as arithmetic (unused for "@"/"*")
    std::string op;          // "", ":-", "-", ":=", "=", ":+", "+", ":?", "?", "#", "##", "%", "%%",
                             // "/", "//", "/#", "/%", "^", "^^", ",", ",,", ":", "@" (transform), "~", "~~"
    std::vector<BashWord> args;  // the operator's words: ${x:-a}, ${x/pat/rep}, ${x:o:l}

    // DoubleQuoted, Locale: the parts inside the quotes.
    std::vector<BashWordPart> parts;

    // CommandSub, ProcessSub: the commands inside.
    std::shared_ptr<BashList> body;
    bool backquoted = false;

    // ArithSub.
    BashArith arith;
};

struct BashWord {
    BashRange range;
    std::string text;  // the source text
    std::vector<BashWordPart> parts;
};

/** @brief The word's text with quotes removed, when it is all literal (no expansions); otherwise false. */
bool BashWordLiteral(const BashWord &word, std::string *out);

struct BashRedirect {
    BashRange range;
    std::string op;  // <, >, >>, >|, <>, <&, >&, &>, &>>, <<, <<-, <<<
    std::string fd;  // "", "2", or "{name}"
    BashRange fd_range;
    BashWord target;  // the file, descriptor, here-string word, or here-document delimiter
    // Here-documents.
    bool heredoc = false;
    bool heredoc_quoted = false;  // the delimiter was quoted: the body is literal
    std::string delimiter;
    BashRange body_range;  // the body lines, the delimiter line excluded
    std::vector<BashWordPart> body_parts;  // the expansions in an unquoted body
    bool unterminated = false;
    int heredoc_id = -1;  // the parser's own bookkeeping
};

struct BashAssignment {
    BashRange range;
    std::string name;
    BashRange name_range;
    bool has_subscript = false;
    std::string subscript;
    BashArith subscript_arith;
    bool append = false;  // +=
    bool is_array = false;  // name=( ... )
    BashWord value;
    std::vector<BashWord> elements;  // array elements, `[k]=v` written as one word
    // An argument of declare/typeset/local/export/readonly (`local x=1`),
    // rather than an assignment prefixing a command (`LANG=C sort`). Such
    // arguments are also in the command's `words`.
    bool declaration = false;
};

enum class BashCommandKind {
    Simple,
    Subshell,   // ( list )
    Group,      // { list; }
    If,
    While,
    Until,
    For,        // for name [in words]; do list; done
    Select,
    ForArith,   // for (( init; test; step )); do list; done
    Case,
    Function,
    Cond,       // [[ expr ]]
    Arith,      // (( expr ))
    Coproc,
};

struct BashCaseItem {
    BashRange range;
    std::vector<BashWord> patterns;
    std::shared_ptr<BashList> body;
    std::string terminator;  // ";;", ";&", ";;&", or "" for the last item
};

// One element of a [[ ]] expression, in source order.
struct BashCondTerm {
    enum Kind { Word, UnaryOp, BinaryOp, Not, And, Or, Open, Close } kind = Word;
    BashWord word;  // for Word; for operators, the operator's text in `word.text`
    bool pattern = false;  // the right side of == / != (a pattern) or =~ (a regex)
};

struct BashCommand {
    BashCommandKind kind = BashCommandKind::Simple;
    BashRange range;
    std::vector<BashRedirect> redirects;

    // Simple: assignments first, then the words (words[0] is the command name).
    std::vector<BashAssignment> assignments;
    std::vector<BashWord> words;

    // Compound bodies, in source order. If: cond, then, [cond, then]..., [else];
    // While/Until: cond, body; For/Select/ForArith: body; Subshell/Group: body.
    std::vector<std::shared_ptr<BashList>> bodies;
    bool has_else = false;

    // For/Select.
    std::string var;
    BashRange var_range;
    bool has_in = false;
    std::vector<BashWord> items;

    // ForArith (three parts), Arith (one).
    std::vector<BashArith> ariths;

    // Case.
    BashWord subject;
    std::vector<BashCaseItem> case_items;

    // Function: name, and its body as sub[0]. Coproc: the command as sub[0].
    std::string name;
    BashRange name_range;
    bool function_keyword = false;
    std::vector<BashCommand> sub;

    // Cond.
    std::vector<BashCondTerm> cond;

    // The reserved words and braces that frame the command (if/then/fi,
    // do/done, {/}, case/in/esac, ...), for indentation and folding.
    std::vector<BashRange> keywords;
};

struct BashPipeline {
    BashRange range;
    bool negated = false;  // ! pipeline
    bool timed = false;    // time pipeline
    std::vector<BashCommand> commands;
    std::vector<std::string> ops;  // between commands: "|" or "|&"
};

struct BashAndOr {
    BashRange range;
    std::vector<BashPipeline> pipelines;
    std::vector<std::string> ops;  // between pipelines: "&&" or "||"
    std::string terminator;        // ";", "&", "\n", or "" (end of input / closing keyword)
    BashRange terminator_range;
};

struct BashList {
    BashRange range;
    std::vector<BashAndOr> items;
};

struct BashComment {
    BashRange range;
    std::string text;  // including the `#`
};

struct BashParseError {
    BashRange range;
    std::string code;     // "syntax-error", "unexpected-eof", "unterminated-heredoc"
    std::string message;  // bash's wording where bash has one
    bool warning = false; // a here-document delimited by end of file: bash warns and carries on
};

struct BashParseResult {
    BashList program;
    std::vector<BashComment> comments;
    // The first syntax error, as bash reports it (bash stops there), plus any
    // warnings. After an error the parser resynchronizes at the next line and
    // keeps reading, so the rest of the file still has a tree to analyze.
    std::vector<BashParseError> errors;
    bool has_syntax_error = false;
    // The lines of here-document bodies (and their delimiter lines), which a
    // formatter must leave alone.
    std::vector<BashRange> heredoc_bodies;
};

/**
 * @brief Parses a Bash script.
 * @param lines the script, one entry per line (no trailing newlines)
 * @return the tree, comments, and errors
 */
BashParseResult BashParse(const std::vector<std::string> &lines);

// --- Walking ------------------------------------------------------------

// A visitor over every command in a tree, including those inside command
// substitutions, process substitutions and parameter-expansion words.
// `depth` counts enclosing function definitions (0 at top level).
struct BashVisitor {
    virtual ~BashVisitor() = default;
    /** @brief Called for each command before its children; return false to skip them. */
    virtual bool Command(const BashCommand &, const BashCommand * /*enclosing_function*/) { return true; }
    /** @brief Called for each word part (recursively, nested parts included). */
    virtual void Part(const BashWordPart &, const BashCommand * /*enclosing_function*/) {}
    /** @brief Called for each arithmetic variable reference. */
    virtual void ArithRef(const BashArithRef &, const BashCommand * /*enclosing_function*/) {}
};

/** @brief Walks every command, word part and arithmetic reference in a list. */
void BashWalk(const BashList &list, BashVisitor &visitor);

/** @brief The reserved words, as bash's parser recognizes them. */
bool BashIsReservedWord(const std::string &word);

/** @brief Reports whether a string is a valid variable name ([A-Za-z_][A-Za-z0-9_]*). */
bool BashIsName(const std::string &s);

#endif  // MEP_BASH_PARSE_H
