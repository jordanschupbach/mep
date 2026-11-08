#pragma once

// google-java-format's layout engine, ported to C++ for mep's Java
// formatter (java_format.h): Newlines, Input/JavaInput (the token stream
// with its comments and white space), Doc (the Level/Token/Space/Break/Tok
// tree), DocBuilder, OpsBuilder, JavaOutput and JavaCommentsHelper.
//
// Copyright 2015 Google Inc., Apache License 2.0, ported from
// google-java-format 1.36.1 (core/src/main/java/com/google/googlejavaformat/
// {Doc,DocBuilder,OpsBuilder,Input,Output,InputOutput,Indent,Newlines,
// CommentsHelper}.java and java/{JavaInput,JavaOutput,JavaCommentsHelper,
// JavacTokens}.java). Licensed under the Apache License, Version 2.0; you
// may not use this file except in compliance with the License. You may
// obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Text is UTF-8 and positions are byte offsets; everything Java measures
// with String.length() (widths, columns) is measured here in UTF-16 code
// units (Len16), so line widths agree with GJF's.

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace javafmt::gjf {

// GJF's FormatterException / FormattingError: the file is refused.
struct FormatError {
    std::string message;
    int line = 0;  // 1-based, 0 = unknown
};

// ---- text helpers (Java String / Guava CharMatcher semantics) -------------

/** @brief Java String.length() of UTF-8 text: UTF-16 code units. */
int Len16(std::string_view s);
/** @brief The byte offset of UTF-16 index `u16` in `s` (clamped to s.size()). */
size_t ByteIndex16(std::string_view s, int u16);
/** @brief Decodes the code point at s[i]; sets *len to its byte length. */
uint32_t DecodeAt(std::string_view s, size_t i, size_t *len);
/** @brief Guava CharMatcher.whitespace() (Unicode White_Space). */
bool IsGuavaWhitespace(uint32_t cp);
/** @brief Java Character.isWhitespace. */
bool IsJavaWhitespace(uint32_t cp);
/** @brief CharMatcher.whitespace().trimFrom / trimTrailingFrom / trimLeadingFrom. */
std::string_view TrimWs(std::string_view s);
std::string_view TrimTrailingWs(std::string_view s);
std::string_view TrimLeadingWs(std::string_view s);
/** @brief CharMatcher.whitespace().negate().indexIn(s), as a byte offset (-1: none). */
int IndexOfNonWs(std::string_view s);
/** @brief Java String.trim(): strips chars <= U+0020 at both ends. */
std::string_view JavaTrim(std::string_view s);
bool StartsWith(std::string_view s, std::string_view p);
bool EndsWith(std::string_view s, std::string_view p);

namespace newlines {
int Count(std::string_view input);
/** @brief Byte offset of the start of the second line, or -1. */
int FirstBreak(std::string_view input);
bool IsNewline(std::string_view input);
int HasNewlineAt(std::string_view input, size_t idx);
/** @brief "\r\n", "\n", "\r" or "" when the text doesn't end in a line break. */
std::string_view GetLineEnding(std::string_view input);
std::string GuessLineSeparator(std::string_view text);
bool ContainsBreaks(std::string_view text);
/** @brief Newlines.lineOffsetIterator: 0 and the start of every following line. */
std::vector<int> LineOffsets(std::string_view input);
/** @brief Newlines.lineIterator: the lines including their terminators. */
std::vector<std::string_view> Lines(std::string_view input);
}  // namespace newlines

// ---- ranges ---------------------------------------------------------------

// A closed-open integer range; Guava's Range.closedOpen(-1, -1) is EMPTY.
struct Range {
    int lo = -1, hi = -1;
    bool Empty() const { return lo >= hi; }
    bool operator==(const Range &o) const { return lo == o.lo && hi == o.hi; }
};
inline Range Singleton(int k) { return Range{k, k + 1}; }
Range Union(Range x, Range y);

// ---- input ------------------------------------------------------------------

// The raw lexical stream JavaInput is built from (JavacTokens.RawTok): every
// token, comment and white-space run, in order, covering the text.
enum class RawKind { Whitespace, Comment, Token, StringLiteral, Eof };
struct RawTok {
    RawKind kind = RawKind::Token;
    int pos = 0, end = 0;
    std::string string_val;  // StringLiteral only: the value, escapes processed
    std::string keyword;     // a keyword's/identifier's spelling, for TokenKind checks
};

struct Tok {
    int index = -1;
    std::string original_text;
    std::string text;
    int position = 0;
    int column = 0;
    bool is_token = false;
    std::string kind;  // a keyword spelling (TokenKind), "STRINGLITERAL", or ""
    int len16 = 0;     // originalText.length()

    int Length() const { return len16; }
    bool IsNewline() const { return newlines::IsNewline(text); }
    bool IsSlashSlashComment() const { return StartsWith(text, "//"); }
    bool IsSlashStarComment() const { return StartsWith(text, "/*"); }
    bool IsJavadocComment() const;
    bool IsComment() const { return IsSlashSlashComment() || IsSlashStarComment(); }
};

struct Token {
    const Tok *tok = nullptr;
    std::vector<const Tok *> toks_before;
    std::vector<const Tok *> toks_after;
};

class JavaInput {
public:
    /** @brief Builds the input from the text and its raw lexical stream (JavaInput(String)). */
    JavaInput(std::string text, const std::vector<RawTok> &raw);

    const std::string &Text() const { return text_; }
    int kN() const { return kN_; }
    const std::vector<Token> &Tokens() const { return tokens_; }
    const Token *GetToken(int k) const { return kToToken_[static_cast<size_t>(k)]; }
    /** @brief getPositionTokenMap().get(position), or null. */
    const Token *PositionToken(int position) const;
    /** @brief The tokens whose position ranges intersect [lo, hi). */
    std::vector<const Token *> TokensIntersecting(int lo, int hi) const;
    /** @brief getPositionToColumnMap().get(position), or -1. */
    int PositionColumn(int position) const;
    int GetLineNumber(int position) const;  // 1-based, javac's LineMap
    int GetColumnNumber(int position) const;
    int LineCount() const { return static_cast<int>(lines_.size()); }
    Range GetRanges(int line) const;
    const std::vector<Tok> &Toks() const { return toks_; }

private:
    std::string text_;
    std::vector<std::string_view> lines_;
    std::vector<Range> ranges_;
    std::vector<Tok> toks_;
    std::vector<Token> tokens_;
    struct PosRange {
        int lo, hi;  // closed
        const Token *token;
    };
    std::vector<PosRange> position_tokens_;
    std::unordered_map<int, int> position_column_;
    std::vector<const Token *> kToToken_;
    std::vector<int> line_starts_;
    int kN_ = 0;
};

const Tok *StartTok(const Token &token);
const Tok *EndTok(const Token &token);

// ---- output-side helpers ----------------------------------------------------

struct BreakTag {
    std::optional<bool> taken;
    void RecordBroken(bool broken) { taken = broken; }
    bool WasBreakTaken() const { return taken.value_or(false); }
};

struct Indent {
    // Const when cond is null; If otherwise.
    int n = 0;
    const BreakTag *cond = nullptr;
    const Indent *then_indent = nullptr;
    const Indent *else_indent = nullptr;
    int Eval() const { return cond ? (cond->WasBreakTaken() ? then_indent : else_indent)->Eval() : n; }
};

struct BlankLineWanted {
    enum Kind { Yes, No, Preserve, Conditional } kind = Preserve;
    std::vector<const BreakTag *> tags;
    std::optional<bool> Wanted() const;
    BlankLineWanted Merge(const BlankLineWanted &other) const;
    static BlankLineWanted Of(Kind k) {
        BlankLineWanted w;
        w.kind = k;
        return w;
    }
    static BlankLineWanted Cond(const BreakTag *tag) {
        BlankLineWanted w;
        w.kind = Conditional;
        w.tags.push_back(tag);
        return w;
    }
};

class JavaCommentsHelper {
public:
    explicit JavaCommentsHelper(std::string line_separator, bool format_javadoc = true)
        : line_separator_(std::move(line_separator)), format_javadoc_(format_javadoc) {}
    std::string Rewrite(const Tok &tok, int max_width, int column0) const;

private:
    std::string PreserveIndentation(const std::vector<std::string> &lines, int column0) const;
    std::string IndentLineComments(std::vector<std::string> lines, int column0) const;
    std::vector<std::string> WrapLineComments(const std::vector<std::string> &lines, int column0) const;
    std::string IndentJavadoc(const std::vector<std::string> &lines, int column0) const;
    std::string line_separator_;
    bool format_javadoc_;
};

/** @brief CommentsHelper.reformatParameterComment: `/​* name= *​/`, or nullopt. */
std::optional<std::string> ReformatParameterComment(const Tok &tok);

class JavaOutput;
class DocBuilder;

// ---- Doc --------------------------------------------------------------------

enum class FillMode { Unified, Independent, Forced };

struct State {
    int last_indent = 0, indent = 0, column = 0;
    bool must_break = false;
    State() = default;
    State(int indent0, int column0) : last_indent(indent0), indent(indent0), column(column0) {}
    State(int li, int in, int col, bool mb) : last_indent(li), indent(in), column(col), must_break(mb) {}
    State WithColumn(int c) const { return State(last_indent, indent, c, must_break); }
    State WithMustBreak(bool m) const { return State(last_indent, indent, column, m); }
};

constexpr int kMaxLineWidth = 1000;

enum class OpKind { Level, Token, Space, Break, Tok, Open, Close };

struct Op {
    explicit Op(OpKind k) : op_kind(k) {}
    virtual ~Op() = default;
    Op(const Op &) = delete;
    Op &operator=(const Op &) = delete;
    const OpKind op_kind;
};

class Doc : public Op {
public:
    using Op::Op;
    int GetWidth();
    const std::string &GetFlat();
    Range GetRange();
    virtual State ComputeBreaks(const JavaCommentsHelper &ch, int max_width, State state) = 0;
    virtual void Write(JavaOutput &out) = 0;

protected:
    virtual int ComputeWidth() = 0;
    virtual std::string ComputeFlat() = 0;
    virtual Range ComputeRange() = 0;

private:
    std::optional<int> width_;
    std::optional<std::string> flat_;
    std::optional<Range> range_;
};

class DocLevel final : public Doc {
public:
    explicit DocLevel(const Indent *plus_indent) : Doc(OpKind::Level), plus_indent_(plus_indent) {}
    void Add(Doc *doc) { docs_.push_back(doc); }
    State ComputeBreaks(const JavaCommentsHelper &ch, int max_width, State state) override;
    void Write(JavaOutput &out) override;

protected:
    int ComputeWidth() override;
    std::string ComputeFlat() override;
    Range ComputeRange() override;

private:
    State ComputeBroken(const JavaCommentsHelper &ch, int max_width, State state);
    const Indent *plus_indent_;
    std::vector<Doc *> docs_;
    bool one_line_ = false;
    std::vector<std::vector<Doc *>> splits_;
    std::vector<class DocBreak *> breaks_;
};

class DocToken final : public Doc {
public:
    DocToken(const Token *token, bool real, const Indent *plus_indent_comments_before,
             const Indent *break_and_indent_trailing_comment)
        : Doc(OpKind::Token), token_(token), real_(real), plus_indent_comments_before_(plus_indent_comments_before),
          break_and_indent_trailing_comment_(break_and_indent_trailing_comment) {}
    const Token *GetToken() const { return token_; }
    bool Real() const { return real_; }
    const Indent *PlusIndentCommentsBefore() const { return plus_indent_comments_before_; }
    const Indent *BreakAndIndentTrailingComment() const { return break_and_indent_trailing_comment_; }
    State ComputeBreaks(const JavaCommentsHelper &ch, int max_width, State state) override;
    void Write(JavaOutput &out) override;

protected:
    int ComputeWidth() override;
    std::string ComputeFlat() override;
    Range ComputeRange() override;

private:
    const Token *token_;
    bool real_;
    const Indent *plus_indent_comments_before_;
    const Indent *break_and_indent_trailing_comment_;  // null = Optional.empty()
};

class DocSpace final : public Doc {
public:
    DocSpace() : Doc(OpKind::Space) {}
    State ComputeBreaks(const JavaCommentsHelper &, int, State state) override { return state.WithColumn(state.column + 1); }
    void Write(JavaOutput &out) override;

protected:
    int ComputeWidth() override { return 1; }
    std::string ComputeFlat() override { return " "; }
    Range ComputeRange() override { return Range{}; }
};

class DocBreak final : public Doc {
public:
    DocBreak(FillMode fill_mode, std::string flat, const Indent *plus_indent, BreakTag *opt_tag)
        : Doc(OpKind::Break), fill_mode_(fill_mode), flat_text_(std::move(flat)), plus_indent_(plus_indent),
          opt_tag_(opt_tag) {}
    int GetPlusIndent() const { return plus_indent_->Eval(); }
    bool IsForced() const { return fill_mode_ == FillMode::Forced; }
    FillMode Mode() const { return fill_mode_; }
    State ComputeBreaks(State state, int last_indent, bool broken);
    State ComputeBreaks(const JavaCommentsHelper &, int, State state) override { return state; }
    void Write(JavaOutput &out) override;

protected:
    int ComputeWidth() override { return IsForced() ? kMaxLineWidth : Len16(flat_text_); }
    std::string ComputeFlat() override { return flat_text_; }
    Range ComputeRange() override { return Range{}; }

private:
    FillMode fill_mode_;
    std::string flat_text_;
    const Indent *plus_indent_;
    BreakTag *opt_tag_;
    bool broken_ = false;
    int new_indent_ = 0;
};

class DocTok final : public Doc {
public:
    explicit DocTok(const Tok *tok) : Doc(OpKind::Tok), tok_(tok) {}
    State ComputeBreaks(const JavaCommentsHelper &ch, int max_width, State state) override;
    void Write(JavaOutput &out) override;

protected:
    int ComputeWidth() override;
    std::string ComputeFlat() override;
    Range ComputeRange() override { return Singleton(tok_->index); }

private:
    const Tok *tok_;
    std::string text_;
};

struct OpenOp final : Op {
    explicit OpenOp(const Indent *pi) : Op(OpKind::Open), plus_indent(pi) {}
    const Indent *plus_indent;
};
struct CloseOp final : Op {
    CloseOp() : Op(OpKind::Close) {}
};

// Owns every Op, Indent and BreakTag of one format run.
class Arena {
public:
    template <class T, class... A>
    T *Make(A &&...args) {
        auto p = std::make_unique<T>(std::forward<A>(args)...);
        T *raw = p.get();
        ops_.push_back(std::move(p));
        return raw;
    }
    const Indent *Const(int n);
    const Indent *If(const BreakTag *cond, const Indent *then_indent, const Indent *else_indent);
    BreakTag *Tag();
    DocSpace *Space() { return &space_; }
    CloseOp *Close() { return &close_; }
    const Indent *Zero() { return Const(0); }

private:
    std::vector<std::unique_ptr<Op>> ops_;
    std::deque<Indent> indents_;
    std::map<int, const Indent *> consts_;
    std::deque<BreakTag> tags_;
    DocSpace space_;
    CloseOp close_;
};

class DocBuilder {
public:
    explicit DocBuilder(Arena &arena);
    DocBuilder &WithOps(const std::vector<Op *> &ops);
    Doc *Build() { return base_; }

private:
    Arena &arena_;
    DocLevel *base_;
    std::vector<DocLevel *> stack_;
    DocLevel *append_level_;
};

// ---- output -----------------------------------------------------------------

struct Replacement {
    int lo, hi;
    std::string text;
};

class JavaOutput {
public:
    JavaOutput(std::string line_separator, const JavaInput &input, const JavaCommentsHelper &ch);
    void BlankLine(int k, const BlankLineWanted &wanted);
    void MarkForPartialFormat(const Token &start, const Token &end);
    void Append(std::string_view text, Range range);
    void IndentBy(int indent) { spaces_pending_.append(static_cast<size_t>(indent), ' '); }
    void Flush();
    std::vector<Replacement> GetFormatReplacements(const std::vector<Range> &token_ranges) const;
    int LineCount() const { return static_cast<int>(lines_.size()); }
    const std::string &Line(int i) const { return lines_[static_cast<size_t>(i)]; }

private:
    Range GetRanges(int line) const;
    Range ExpandToBreakableRegions(Range r) const;
    std::string line_separator_;
    const JavaInput &input_;
    std::unordered_map<int, BlankLineWanted> blank_lines_;
    std::vector<std::pair<int, int>> partial_format_ranges_;  // closed, merged, sorted
    std::vector<std::string> lines_;
    std::vector<Range> ranges_;
    int kN_;
    int iLine_ = 0;
    int lastK_ = -1;
    int newlines_pending_ = 0;
    std::string line_builder_;
    std::string spaces_pending_;
};

std::string ApplyReplacements(std::string_view input, std::vector<Replacement> replacements);

// ---- OpsBuilder -----------------------------------------------------------------

class OpsBuilder {
public:
    OpsBuilder(const JavaInput &input, JavaOutput &output, Arena &arena);
    const JavaInput &Input() const { return input_; }
    Arena &GetArena() { return arena_; }

    int ActualSize(int position, int length) const;
    int ActualStartColumn(int position) const;
    void Add(Op *op);
    int Depth() const { return depth_; }
    void CheckClosed(int previous) const;
    void Sync(int input_position);
    void Drain();
    void Open(const Indent *plus_indent);
    void Close();
    /** @brief peekToken(skip): the original text of the token `skip` ahead, or nullopt. */
    std::optional<std::string_view> PeekToken(int skip = 0) const;
    const Token *CurrentToken() const;
    int TokenIndex() const { return tokenI_; }
    void GuessToken(std::string_view token);
    void TokenOp(std::string_view token, bool real, const Indent *plus_indent_comments_before,
                 const Indent *break_and_indent_trailing_comment);
    void OpChars(std::string_view op);
    void Space();
    void BreakOp(FillMode fill_mode, std::string flat, const Indent *plus_indent, BreakTag *tag = nullptr);
    void BreakOp() { BreakOp(FillMode::Unified, "", arena_.Zero()); }
    void BreakOp(const Indent *plus_indent) { BreakOp(FillMode::Unified, "", plus_indent); }
    void BreakOp(std::string flat) { BreakOp(FillMode::Unified, std::move(flat), arena_.Zero()); }
    void BreakToFill() { BreakOp(FillMode::Independent, "", arena_.Zero()); }
    void BreakToFill(std::string flat) { BreakOp(FillMode::Independent, std::move(flat), arena_.Zero()); }
    void ForcedBreak() { BreakOp(FillMode::Forced, "", arena_.Zero()); }
    void ForcedBreak(const Indent *plus_indent) { BreakOp(FillMode::Forced, "", plus_indent); }
    void MarkForPartialFormat();
    void BlankLineWantedOp(const BlankLineWanted &wanted);
    std::vector<Op *> Build();
    [[noreturn]] void Fail(const std::string &message) const;

private:
    const JavaInput &input_;
    JavaOutput &output_;
    Arena &arena_;
    std::vector<Op *> ops_;
    int tokenI_ = 0;
    int input_position_ = -2147483647 - 1;
    int depth_ = 0;
    int last_partial_format_boundary_ = -1;
};

}  // namespace javafmt::gjf
