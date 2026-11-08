#ifndef MEP_FISH_PARSE_H
#define MEP_FISH_PARSE_H

// A fish-script reader for mep's Fish language server and formatter
// (fish_lsp.cpp, fish_format.cpp). It follows the behaviour of fish 4.x's
// own reader -- the same tokens, the same tree shape (job lists, job
// conjunctions, pipelines, decorated / block / if / switch / brace
// statements) and the same first error with the same message -- so that
// `mep-fish-lsp --fish-check FILE` and `fish --no-execute FILE` agree
// (tools/fish_parse_check.py measures it). It is written from the
// documented language and its observable behaviour; no fish code is used.
//
// Everything works on UTF-32 text (fish counts characters, not bytes);
// Source keeps the byte offset of every character for the LSP's columns.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fishparse {

/** @brief The text as code points, with the UTF-8 byte offset of each one. */
struct Source {
    std::u32string text;
    std::vector<size_t> byte_of;  // size text.size() + 1
    static Source FromUtf8(std::string_view utf8);
    /** @brief Byte offset of character offset @p ch (clamped). */
    size_t Byte(size_t ch) const { return byte_of[ch < byte_of.size() ? ch : byte_of.size() - 1]; }
};

std::string ToUtf8(std::u32string_view s);

// Characters with a special meaning after unescaping (fish keeps these in a
// reserved block; any noncharacter block will do).
constexpr char32_t kHomeDirectory = 0xFDD0;
constexpr char32_t kProcessExpandSelf = 0xFDD1;
constexpr char32_t kVariableExpand = 0xFDD2;
constexpr char32_t kVariableExpandSingle = 0xFDD3;
constexpr char32_t kBraceBegin = 0xFDD4;
constexpr char32_t kBraceEnd = 0xFDD5;
constexpr char32_t kBraceSep = 0xFDD6;
constexpr char32_t kBraceSpace = 0xFDD7;
constexpr char32_t kInternalSeparator = 0xFDD8;
constexpr char32_t kVariableExpandEmpty = 0xFDD9;
constexpr char32_t kAnyChar = 0xFDDA;
constexpr char32_t kAnyString = 0xFDDB;
constexpr char32_t kAnyStringRecursive = 0xFDDC;

/** @brief Unescape a script token. @p special keeps expansion markers; false return = bad escape. */
bool Unescape(std::u32string_view in, bool special, bool allow_incomplete, std::u32string *out,
              bool no_backslashes = false);
bool ValidVarNameChar(char32_t c);
bool ValidVarName(std::u32string_view s);
bool IsWhitespace(char32_t c);

// --- Tokenizer ---------------------------------------------------------------------

enum class TokType : uint8_t { String, Pipe, Redirect, Background, End, LeftBrace, RightBrace, AndAnd, OrOr, Error, Comment };

enum class TokErr : uint8_t {
    None, UnterminatedQuote, UnterminatedSubshell, UnterminatedSlice, UnterminatedEscape, InvalidRedirect,
    InvalidPipe, ClosingUnopenedSubshell, IllegalSlice, ClosingUnopenedBrace, UnterminatedBrace,
    ExpectedPcloseFoundBclose, ExpectedBcloseFoundPclose,
};

const char *TokErrMessage(TokErr e);

struct Tok {
    TokType type = TokType::String;
    size_t offset = 0, length = 0;
    size_t error_offset = 0, error_length = 0;  // error_offset is within the token
    TokErr error = TokErr::None;
    size_t End() const { return offset + length; }
};

enum TokFlag : unsigned {
    kTokAcceptUnfinished = 1,
    kTokShowComments = 2,
    kTokArgumentList = 4,
    kTokContinueAfterError = 8,
    kTokShowBlankLines = 16,
};

class Tokenizer {
public:
    Tokenizer(std::u32string_view src, unsigned flags, std::function<void(size_t)> on_quote = {});
    bool Next(Tok *out);

private:
    char32_t At(size_t i) const { return i < src_.size() ? src_[i] : U'\0'; }
    Tok Error(TokErr e, size_t token_start, size_t error_loc, long token_length, size_t error_len);
    Tok ReadString();
    bool OpeningQuote(std::vector<size_t> &quoted_cmdsubs, size_t paren_depth, char32_t quote, size_t *err);

    std::u32string_view src_;
    size_t cursor_ = 0;
    bool has_next_ = true;
    bool brace_parser_ = true;
    bool at_command_position_ = true;
    size_t unclosed_braces_ = 0;
    bool accept_unfinished_, show_comments_, show_blank_lines_, continue_after_error_;
    bool continue_line_after_comment_ = false;
    std::function<void(size_t)> on_quote_;
};

/** @brief Where a quote starting at @p pos ends (the closing quote or a `$(` in "..."); npos if never. */
size_t QuoteEnd(std::u32string_view s, size_t pos, char32_t quote);
bool TokIsStringChar(char32_t c, char32_t next);
/** @brief The `=` of `NAME=value`, or npos. */
size_t VariableAssignmentEquals(std::u32string_view s);

/** @brief A command substitution found by LocateCmdsub: [start, end) with the parens. */
struct Cmdsub {
    size_t start = 0, end = 0, num_closing = 0;
    size_t CommandStart() const { return start + 1; }
    size_t CommandEnd() const { return end - num_closing; }
};

/** @brief -1 on a paren error, 0 when there is none, 1 when found (cursor moves past it). */
int LocateCmdsub(std::u32string_view s, size_t *cursor, bool accept_incomplete, bool *is_quoted, bool *has_dollar,
                 Cmdsub *out);

// --- Tree -----------------------------------------------------------------------------

enum class Kw : uint8_t {
    None, And, Begin, Builtin, Case, Command, Else, End, Exclam, Exec, For, Function, If, In, Not, Or, Switch,
    Time, While,
};
const char *KwName(Kw k);

enum class PTok : uint8_t {
    Invalid, String, Pipe, Redirection, Background, End, LeftBrace, RightBrace, AndAnd, OrOr, TokenizerError,
    Terminate,
};

enum class Kind : uint8_t {
    JobList, JobConjunction, JobPipeline, JobConjunctionContinuationList, JobConjunctionContinuation,
    JobContinuationList, JobContinuation, VariableAssignmentList, VariableAssignment, Statement,
    DecoratedStatement, NotStatement, BlockStatement, BlockStatementHeader, ForHeader, WhileHeader, FunctionHeader,
    BeginHeader, BraceStatement, IfStatement, IfClause, ElseifClauseList, ElseifClause, ElseClause,
    SwitchStatement, CaseItemList, CaseItem, ArgumentOrRedirectionList, ArgumentOrRedirection, ArgumentList,
    Argument, Redirection, AndorJobList, AndorJob, MaybeNewlines, Keyword, Token,
};

/** @brief Which field of its parent a node fills. */
enum class Role : uint8_t {
    None, Item, Decorator, Job, Continuations, SemiNl, Time, Variables, Statement, Continuation, Bg, Pipe,
    Newlines, Conjunction, OptDecoration, Command, ArgsOrRedirs, Kw, Contents, Header, Jobs, End, LeftBrace,
    RightBrace, VarName, KwIn, Args, Condition, AndorTail, FirstArg, IfClause, ElseifClauses, ElseClause, Body,
    Argument, Cases, Arguments, Oper, Target, Embedded,
};

struct Node {
    Kind kind = Kind::JobList;
    Role role = Role::None;
    int parent = -1;
    std::vector<int> kids;
    // Leaves (Argument, VariableAssignment, MaybeNewlines, Keyword, Token):
    bool sourced = false;
    size_t start = 0, len = 0;
    PTok tok = PTok::Invalid;  // Token leaves: the token type found
    Kw kw = Kw::None;          // Keyword leaves: the keyword found
    Kw kw_allowed = Kw::None;  // Keyword leaves: the first allowed keyword (End for an `end`)
    bool IsLeaf() const {
        return kind == Kind::Argument || kind == Kind::VariableAssignment || kind == Kind::MaybeNewlines ||
               kind == Kind::Keyword || kind == Kind::Token;
    }
};

struct Range {
    size_t start = 0, len = 0;
    size_t End() const { return start + len; }
};

struct ParseError {
    size_t start = 0, len = 0;  // character offsets; start == kNoLocation means "no line"
    std::string text;
};
constexpr size_t kNoLocation = static_cast<size_t>(-1);

struct ParseFlags {
    bool continue_after_error = false;
    bool include_comments = false;
    bool accept_incomplete_tokens = false;
    bool leave_unterminated = false;
    bool show_extra_semis = false;
};

struct Ast {
    std::vector<Node> nodes;  // nodes[0] is the top JobList
    std::vector<Range> comments;
    std::vector<Range> semis;   // extra semicolons (show_extra_semis)
    std::vector<Range> errors;  // ranges skipped or flagged by error recovery
    bool any_error = false;
    std::vector<ParseError> parse_errors;  // in order; only the first is fish's message

    const Node &operator[](int i) const { return nodes[static_cast<size_t>(i)]; }
    /** @brief Child of @p n with role @p r, or -1. */
    int Field(int n, Role r) const;
    /** @brief Source range of @p n (its sourced leaves); false if it has none. */
    bool RangeOf(int n, Range *out) const;
    bool HasSource(int n) const {
        Range r;
        return RangeOf(n, &r);
    }
};

Ast Parse(std::u32string_view src, const ParseFlags &flags);

/** @brief fish's own error check (what evaluating the file reports): parse errors, then tree checks. */
std::vector<ParseError> DetectErrors(std::u32string_view src);

/** @brief Builtins fish knows (`builtin -n`), sorted. */
bool IsBuiltin(std::u32string_view name);

}  // namespace fishparse

#endif  // MEP_FISH_PARSE_H
