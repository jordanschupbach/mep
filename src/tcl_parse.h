#pragma once

// Tcl's parser, for mep-tcl-lsp: a port of Tcl 8.6.16's generic/tclParse.c
// (Tcl_ParseCommand and the word, variable, brace, quote and backslash
// parsers under it), the expression parser of generic/tclCompExpr.c
// (ParseExpr, ParseLexeme), TclParseNumber's scanner (generic/tclStrToD.c)
// and the list splitter of generic/tclUtil.c (TclFindElement). It makes the
// same decisions and reports the same messages, so a diagnostic says what
// tclsh would say. tools/tcl_parse_check.py compares the two.
//
// Tcl is copyrighted by the Regents of the University of California, Sun
// Microsystems, Inc., Scriptics Corporation, ActiveState Corporation and
// other parties, under the BSD-style terms in tcl_parse.cpp.
//
// Everything works on byte offsets into one string (the document), so a
// script inside braces is parsed in place: [start, end) of the same text.

#include <string>
#include <string_view>
#include <vector>

namespace tclparse {

enum class TokType {
    Word,        // a word with substitutions; num_components tokens follow
    SimpleWord,  // a word that is one Text token (which follows)
    ExpandWord,  // {*}word
    Text,
    Bs,          // a backslash sequence
    Command,     // [script], brackets included
    Variable,    // $name, ${name}, $name(index): a Text token (the name) then the index tokens
    SubExpr,     // expression tokens, unused by the script parser
    Operator,
};

struct Token {
    TokType type = TokType::Text;
    int start = 0;
    int size = 0;
    int num_components = 0;
};

struct Error {
    bool failed = false;
    std::string message;  // Tcl's interp result
    int pos = 0;          // parsePtr->term: where Tcl points
    bool incomplete = false;
};

// One command (Tcl_Parse). Word tokens are in `tokens`, each Word /
// SimpleWord / ExpandWord followed by its num_components component tokens.
struct Command {
    int comment_start = -1, comment_size = 0;
    int command_start = 0, command_size = 0;
    int num_words = 0;
    int term = 0;  // the terminator (or end)
    std::vector<Token> tokens;
};

/** @brief Tcl_ParseCommand over text[start, end): one command (and the comments before it). */
bool ParseCommand(std::string_view text, int start, int end, bool nested, Command *out, Error *err);

struct Script {
    std::vector<Command> commands;
    Error error;
    int end = 0;  // nested: the position of the closing `]`
};

/**
 * @brief Every command of text[start, end), as Tcl evaluates a script; stops at the first error.
 * With `nested`, the script is the inside of `[...]` and ends at its `]` (missing: "missing close-bracket").
 */
Script ParseScript(std::string_view text, int start, int end, bool nested = false);

/** @brief TclParseBackslash: the length of the backslash sequence at `pos`, appending its value to *out. */
int ParseBackslash(std::string_view text, int pos, int end, std::string *out);

// An expression (ParseExpr). The operand tokens are kept as Tcl keeps them
// (Word + components for "quoted", {braced}, $var and [script] operands);
// function names and literal barewords are listed separately.
struct ExprResult {
    Error error;
    std::vector<Token> tokens;
    struct Name {
        int start, size;
    };
    std::vector<Name> functions;  // `name(` calls
    std::vector<Name> literals;   // numbers and booleans
};

/** @brief Tcl's expression parser over text[start, end); the message is Tcl's, `in expression "..."` included. */
ExprResult ParseExpr(std::string_view text, int start, int end);

/** @brief TclParseNumber (TCL_PARSE_NO_WHITESPACE): the end of the number at `pos`, or -1 when none. */
int ParseNumber(std::string_view text, int pos, int end, bool *is_double = nullptr);

/** @brief Tcl_GetBoolean on a string: true/false, yes/no, on/off and their prefixes, or a number. */
bool IsBooleanString(std::string_view s);

// A list (TclFindElement).
struct ListElem {
    int start = 0, size = 0;  // the element's text, braces / quotes excluded
    bool braced = false, quoted = false;
    bool literal = true;  // no backslash to substitute
};

/** @brief Splits text[start, end) as a Tcl list; false (with Tcl's message) when it is not one. */
bool SplitList(std::string_view text, int start, int end, std::vector<ListElem> *out, Error *err);

/** @brief The value of a word that has no `$`/`[` substitution; false when it has one. */
bool WordLiteral(std::string_view text, const std::vector<Token> &tokens, size_t word, std::string *out);

/** @brief A list element's value (backslashes substituted unless it was braced). */
std::string ElementValue(std::string_view text, const ListElem &e);

bool IsSpace(char c);  // TclIsSpaceProc: space, \t, \n, \v, \f, \r
bool IsBareword(char c);

}  // namespace tclparse
