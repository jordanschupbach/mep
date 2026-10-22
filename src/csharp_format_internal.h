#pragma once

// Internals of mep's C# formatter (csharp_format.h): a port of Roslyn's
// formatting engine -- Microsoft.CodeAnalysis.Formatting and its C# rules,
// src/Workspaces/SharedUtilitiesAndExtensions/Compiler/{Core,CSharp}/Formatting
// at the commit the .NET 10 SDK's dotnet-format was built from
// (5.6.0-2.26329.109, dotnet/dotnet 35b593be) -- over mep's port of Roslyn's
// parser (csharp_parser.h), whose trees are Roslyn's own.
//
// Roslyn is MIT-licensed: Copyright (c) .NET Foundation and Contributors
// (https://github.com/dotnet/roslyn/blob/main/License.txt). This header and
// csharp_format*.cpp are a derived work.
//
// How it is laid out (Roslyn's names, so the original reads alongside):
//   csharp_format_engine.cpp  AbstractFormatEngine and its OperationApplier,
//                             TokenStream, FormattingContext with its interval
//                             trees (ContextMutableIntervalTree over Roslyn's
//                             AVL MutableIntervalTree), FormattingResult;
//   csharp_format_trivia.cpp  TriviaDataFactory (whitespace, complex trivia,
//                             the analyzers), AbstractTriviaFormatter and
//                             CSharpTriviaFormatter (comments, doc comments,
//                             directives through CSharpStructuredTriviaFormatEngine);
//   csharp_format_rules.cpp   the C# formatting rules, FormattingHelpers and the
//                             syntax extension methods they call;
//   csharp_format.cpp         Format(): parse, run the engine, proof-read.
//
// What is left out, because it cannot happen when formatting parsed text with
// the default options (what `dotnet format whitespace` does without an
// .editorconfig): elastic trivia and annotations (ElasticTriviaFormattingRule
// is a no-op), selection formatting (InitialContextFinder,
// BottomUpBaseIndentationFinder), alignment operations (no C# rule adds any),
// and options other than the defaults beyond the line options (tabs, sizes,
// new line).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "csharp_parser.h"

namespace csfmt {

using csharp::SyntaxKind;
using csharp::TextSpan;
using Node = csharp::SyntaxNode;
using Token = const csharp::SyntaxNode *;  // nullptr is Roslyn's default(SyntaxToken) (kind None)
using Trivia = const csharp::SyntaxTrivia *;
using U16 = std::u16string;
using U16View = std::u16string_view;

struct LineOptions {
    bool use_tabs = false;
    int tab_size = 4;
    int indentation_size = 4;
    U16 new_line = u"\n";
};

inline TextSpan FromBounds(int s, int e) { return TextSpan{s, e}; }
inline bool SpanEq(TextSpan a, TextSpan b) { return a.start == b.start && a.end == b.end; }
inline bool SpanIsEmpty(TextSpan a) { return a.end == a.start; }

// --- The tree ----------------------------------------------------------------------------------

/** @brief The parsed file plus the navigation Roslyn's SyntaxToken/SyntaxNode API gives the formatter. */
class Tree {
public:
    explicit Tree(const csharp::SyntaxTree *tree);

    const csharp::SyntaxTree *syntax() const { return tree_; }
    const Node *Root() const { return tree_->Root(); }
    const U16 &Text() const { return tree_->Text16(); }
    U16View TextOf(TextSpan s) const {
        return U16View(tree_->Text16()).substr(static_cast<size_t>(s.start), static_cast<size_t>(s.end - s.start));
    }

    /** @brief Every token of the main tree, zero-width ones included, in order. */
    const std::vector<Token> &Tokens() const { return tokens_; }
    /** @brief Index of a main-tree token in Tokens(); -1 for tokens inside structured trivia. */
    int IndexOf(Token t) const;

    Token NextToken(Token t, bool include_zero_width = false) const;
    Token PreviousToken(Token t, bool include_zero_width = false) const;

    // Region / endregion pairs (DirectiveSyntaxExtensions.GetMatchingDirective for regions).
    const Node *MatchingRegion(const Node *endregion) const;

    // FindTrivia(pos) on the main tree (not inside structured trivia): the trivia whose full span holds pos.
    Trivia FindTrivia(int pos) const;

private:
    const csharp::SyntaxTree *tree_;
    std::vector<Token> tokens_;
    std::unordered_map<Token, int> index_;
    std::unordered_map<const Node *, const Node *> region_pairs_;
};

// Token / node helpers (Roslyn's SyntaxToken / SyntaxNode members).
inline SyntaxKind Kind(Token t) { return t ? t->Kind() : SyntaxKind::None; }
inline SyntaxKind KindOf(const Node *n) { return n ? n->Kind() : SyntaxKind::None; }
inline const Node *Parent(const Node *n) { return n ? n->parent : nullptr; }
inline bool IsKind(const Node *n, SyntaxKind k) { return n && n->Kind() == k; }
inline bool IsParentKind(const Node *n, SyntaxKind k) { return n && IsKind(n->parent, k); }
inline int SpanStart(Token t) { return t ? t->Span().start : 0; }
inline TextSpan SpanOf(Token t) { return t ? t->Span() : TextSpan{0, 0}; }
inline TextSpan FullSpanOf(const Node *t) { return t ? t->FullSpan() : TextSpan{0, 0}; }
inline int Width(Token t) { return t ? static_cast<int>(t->green->text.size()) : 0; }
inline int FullWidth(const Node *t) { return t ? t->green->full_width : 0; }
inline bool IsMissing(Token t) { return t && t->IsMissing(); }
inline U16View TokenText(Token t) { return t ? U16View(t->green->text) : U16View(); }
inline bool HasLeadingTrivia(Token t) { return t && !t->leading.empty(); }
inline bool HasTrailingTrivia(Token t) { return t && !t->trailing.empty(); }

/** @brief First / last token of a node; Roslyn's GetFirstToken(includeZeroWidth) (false skips zero-width tokens). */
Token FirstToken(const Node *n, bool include_zero_width = false);
Token LastToken(const Node *n, bool include_zero_width = false);
/** @brief The field child of a node (a token or a node), nullptr when absent. */
const Node *F(const Node *n, csharp::Field f);
/** @brief The items of a list field (separators excluded when `skip_separators`). */
std::vector<const Node *> Items(const Node *n, csharp::Field f, bool skip_separators = true);

// Syntax.xml class tests (by kind).
enum ClassBit : uint64_t {
    kStatement = 1ull << 0,
    kMemberDeclaration = 1ull << 1,
    kExpression = 1ull << 2,
    kBaseTypeDeclaration = 1ull << 3,
    kAnonymousFunctionExpression = 1ull << 4,
    kBaseObjectCreationExpression = 1ull << 5,
    kInitializerExpression = 1ull << 6,
    kPrefixUnaryExpression = 1ull << 7,
    kPostfixUnaryExpression = 1ull << 8,
    kBinaryExpression = 1ull << 9,
    kAssignmentExpression = 1ull << 10,
    kSwitchLabel = 1ull << 11,
    kStructuredTrivia = 1ull << 12,
    kBranchingDirective = 1ull << 13,
    kAttributeList = 1ull << 14,
    kAccessorDeclaration = 1ull << 15,
    kPointerType = 1ull << 16,
    kDestructorDeclaration = 1ull << 17,
    kQueryClause = 1ull << 18,
    kSelectOrGroupClause = 1ull << 19,
};
uint64_t ClassBits(SyntaxKind kind);
inline bool Is(const Node *n, ClassBit b) { return n && (ClassBits(n->Kind()) & b) != 0; }

// --- Trivia text helpers -------------------------------------------------------------------------

U16View TriviaText(const Tree &tree, Trivia t);  // ToFullString()
bool IsRegularOrDocComment(SyntaxKind k);
bool IsDocComment(SyntaxKind k);

// String extensions (StringExtensions / FormattingExtensions).
int ConvertTabToSpace(U16View text, int tab_size, int initial_column, int end_position);
int GetColumnFromLineOffset(U16View line, int end_position, int tab_size);
U16View GetLastLineText(U16View text);
bool ContainsLineBreak(U16View text);
int GetNumberOfLineBreaks(U16View text);
int GetTextColumn(U16View text, int tab_size, int initial_column);
int GetFirstNonWhitespaceIndexInString(U16View text);
U16 CreateIndentationString(int desired, bool use_tab, int tab_size);
bool IsCSharpWhitespace(char16_t c);  // SyntaxFacts.IsWhitespace
bool IsCSharpNewLine(char16_t c);     // SyntaxFacts.IsNewLine
bool IsDotNetWhiteSpace(char16_t c);  // char.IsWhiteSpace

// --- Operations ----------------------------------------------------------------------------------

enum class NewLinesOption { PreserveLines, ForceLines, ForceLinesIfOnSingleLine };
enum class SpacesOption {
    PreserveSpaces,
    DefaultSpacesIfOnSingleLine,
    ForceSpacesIfOnSingleLine,
    ForceSpaces,
    DynamicSpaceToIndentationIfOnSingleLine
};

struct NewLinesOp {
    bool valid = false;
    int line = 0;
    NewLinesOption option = NewLinesOption::PreserveLines;
};
struct SpacesOp {
    bool valid = false;
    int space = 0;
    SpacesOption option = SpacesOption::PreserveSpaces;
};
inline NewLinesOp Lines(int line, NewLinesOption o) { return {true, line, o}; }
inline SpacesOp Spaces(int space, SpacesOption o) { return {true, space, o}; }

enum IndentBlockOption : int {
    kRelativeToFirstTokenOnBaseTokenLine = 0x2,
    kRelativePosition = 0x4,
    kAbsolutePosition = 0x8,
    kRelativePositionMask = kRelativeToFirstTokenOnBaseTokenLine | kRelativePosition,
    kIndentIfConditionOfAnchorToken = 0x10,
};

struct IndentBlockOperation {
    Token base_token = nullptr;
    TextSpan span;
    int option = 0;
    Token start_token = nullptr;
    Token end_token = nullptr;
    bool is_relative = false;
    int delta = 0;  // IndentationDeltaOrPosition
};

enum SuppressOption : int {
    kSuppressNone = 0,
    kNoWrappingIfOnSingleLine = 0x1,
    kNoWrappingIfOnMultipleLine = 0x2,
    kNoWrapping = 0x3,
    kNoSpacingIfOnSingleLine = 0x4,
    kNoSpacingIfOnMultipleLine = 0x8,
    kNoSpacing = 0xC,
    kIgnoreElasticWrapping = 0x10,
    kDisableFormatting = 0x20,
};

struct SuppressOperation {
    TextSpan span;
    int option = 0;
    Token start_token = nullptr;
    Token end_token = nullptr;
};

struct AnchorIndentationOperation {
    Token anchor_token = nullptr;
    TextSpan span;
    Token end_token = nullptr;
};

// --- Rules ---------------------------------------------------------------------------------------

/** @brief All the C# rules in Roslyn's chain order (CSharpSyntaxFormatting._rules), as one object. */
class Rules {
public:
    explicit Rules(const Tree &tree) : tree_(tree) {}
    void AddSuppressOperations(std::vector<SuppressOperation> &list, const Node *node) const;
    void AddAnchorIndentationOperations(std::vector<AnchorIndentationOperation> &list, const Node *node) const;
    void AddIndentBlockOperations(std::vector<IndentBlockOperation> &list, const Node *node) const;
    NewLinesOp GetAdjustNewLinesOperation(Token previous, Token current) const;
    SpacesOp GetAdjustSpacesOperation(Token previous, Token current) const;

    const Tree &tree() const { return tree_; }

private:
    const Tree &tree_;
};

/** @brief CSharpHeaderFacts.IsOnIfStatementHeader / IsOnWhileStatementHeader, for GetAdjustedIndentationDelta. */
const Node *ConditionStatementForHeader(const Tree &tree, int position);

// --- Engine --------------------------------------------------------------------------------------

class FormattingContext;
class TokenStream;
class Engine;
class TriviaFactory;

constexpr int kApplyBeginningOfTree = -1;
constexpr int kApplyEndOfTree = -2;

/** @brief One trivia of the list the trivia formatter builds (FormatToSyntaxTrivia's SyntaxTrivia). */
struct OutTrivia {
    SyntaxKind kind = SyntaxKind::None;
    U16 text;
};

/** @brief The base of every TriviaData (Roslyn's TriviaData and AbstractTriviaDataFactory's classes). */
struct TriviaData {
    enum class Type { Whitespace, ModifiedWhitespace, FormattedWhitespace, Complex, ModifiedComplex, FormattedComplex };
    explicit TriviaData(Type t) : type(t) {}
    virtual ~TriviaData() = default;

    Type type;
    int line_breaks = 0;
    int spaces = 0;
    bool SecondTokenIsFirstTokenOnLine() const { return line_breaks > 0; }
    virtual bool ContainsChanges() const { return false; }
    virtual TriviaData *WithSpace(int space, FormattingContext &context) = 0;
    virtual TriviaData *WithLine(int line, int indentation, FormattingContext &context) = 0;
    virtual TriviaData *WithIndentation(int indentation, FormattingContext &context) = 0;
    /** @brief Formats, handing the result to the engine for `pair_index` (or kApplyBeginning/EndOfTree). */
    virtual void Format(FormattingContext &context, int pair_index) = 0;
    /** @brief The new text between the two tokens (data with ContainsChanges() only). */
    virtual U16 NewText() const { return U16(); }
};

/** @brief TokenData: a token and its index in the stream (-1 outside it). */
struct TokenData {
    int index = -1;
    Token token = nullptr;
};

/** @brief TreeData + TokenStream: the tokens being formatted and the trivia between them (original or changed). */
class TokenStream {
public:
    TokenStream(Engine &engine, std::vector<Token> tokens);

    int TokenCount() const { return static_cast<int>(tokens_.size()); }
    Token GetToken(int i) const { return tokens_[static_cast<size_t>(i)]; }
    TokenData GetTokenData(Token token) const;
    TokenData GetPreviousTokenData(TokenData t) const;
    TokenData GetNextTokenData(TokenData t) const;
    TokenData FirstTokenInStream() const { return {0, tokens_.front()}; }
    TokenData LastTokenInStream() const { return {TokenCount() - 1, tokens_.back()}; }
    bool FormatBeginningOfTree() const;
    bool FormatEndOfTree() const;
    bool IsFormattingWholeDocument() const { return FormatBeginningOfTree() && FormatEndOfTree(); }

    Token FirstTokenOfBaseTokenLine(Token token) const;
    bool TwoTokensOriginallyOnSameLine(Token t1, Token t2) const { return TwoTokensOnSameLineWorker(t1, t2, true); }
    bool TwoTokensOnSameLine(Token t1, Token t2) const { return TwoTokensOnSameLineWorker(t1, t2, false); }

    void ApplyBeginningOfTreeChange(TriviaData *data) { begin_change_ = data; }
    void ApplyEndOfTreeChange(TriviaData *data) { end_change_ = data; }
    void ApplyChange(int pair_index, TriviaData *data);

    int GetCurrentColumn(Token token) const { return GetColumn(GetTokenData(token), false); }
    int GetCurrentColumn(TokenData t) const { return GetColumn(t, false); }
    int GetOriginalColumn(Token token) const { return GetColumn(GetTokenData(token), true); }
    void GetTokenLength(Token token, int *length, bool *on_multiple_lines) const;

    TriviaData *GetTriviaData(TokenData t1, TokenData t2) const;
    TriviaData *GetOriginalTriviaData(TokenData t1, TokenData t2) const;
    TriviaData *GetTriviaDataAtBeginningOfTree() const;
    TriviaData *GetTriviaDataAtEndOfTree() const;
    TriviaData *GetTriviaData(int pair_index) const;
    TriviaData *GetOriginalTriviaData(int pair_index) const;
    bool IsFirstTokenOnLine(Token token) const;

private:
    int GetColumn(TokenData t, bool original) const;
    bool TwoTokensOnSameLineWorker(Token t1, Token t2, bool original) const;
    bool IsFirstTokenOnLine(TokenData t1, TokenData t2) const;
    TriviaData *Getter(TokenData t1, TokenData t2, bool original) const {
        return original ? GetOriginalTriviaData(t1, t2) : GetTriviaData(t1, t2);
    }

    Engine &engine_;
    std::vector<Token> tokens_;
    std::unordered_map<Token, int> index_;
    mutable std::vector<TriviaData *> original_;
    std::vector<TriviaData *> changes_;
    TriviaData *begin_change_ = nullptr;
    TriviaData *end_change_ = nullptr;
};

/** @brief AbstractFormatEngine (+ TreeData): one run over the main tree or over one structured trivia. */
class Engine {
public:
    Engine(const Tree &tree, const Rules &rules, const LineOptions &options, const Node *root, bool structured,
           int initial_column);
    ~Engine();

    /** @brief Runs every phase; afterwards Result() is the formatted text of the root (its full span). */
    void Format();
    U16 Result() const;
    bool HasChanges() const;

    const Tree &tree;
    const Rules &rules;
    const LineOptions &options;
    const Node *root;
    bool structured;
    int initial_column;  // TreeData.StructuredTrivia's initial column
    Token first_token = nullptr;  // TreeData._firstToken / _lastToken
    Token last_token = nullptr;
    int start_position = 0;  // Root.FullSpan
    int end_position = 0;

    bool IsFirstToken(Token t) const { return t == first_token; }
    bool IsLastToken(Token t) const { return t == last_token; }
    /** @brief TreeData.GetTextBetween (token1 or token2 may be nullptr for the edges). */
    U16View GetTextBetween(Token t1, Token t2) const;
    int GetOriginalColumn(Token t) const;  // TreeData.GetOriginalColumn

    template <class T, class... Args>
    T *New(Args &&...args) {
        auto p = std::make_unique<T>(std::forward<Args>(args)...);
        T *raw = p.get();
        pool_.push_back(std::move(p));
        return raw;
    }

    TriviaFactory *factory = nullptr;
    TokenStream *stream = nullptr;
    FormattingContext *context = nullptr;

private:
    std::vector<std::unique_ptr<TriviaData>> pool_;
    std::unique_ptr<TriviaFactory> factory_owner_;
    std::unique_ptr<TokenStream> stream_owner_;
    std::unique_ptr<FormattingContext> context_owner_;
};

/** @brief FormattingContext's interface used by trivia data and the trivia formatter. */
class FormattingContext {
public:
    explicit FormattingContext(Engine &engine);
    ~FormattingContext();

    Engine &engine;
    TokenStream &Stream() const { return *engine.stream; }
    const LineOptions &Options() const { return engine.options; }

    void InitializeWholeDocument();
    void AddIndentBlockOperations(std::vector<IndentBlockOperation> &ops);
    void AddIndentBlockOperation(const IndentBlockOperation &op);
    void AddSuppressOperations(const std::vector<SuppressOperation> &ops);
    void AddAnchorIndentationOperation(const AnchorIndentationOperation &op);

    int GetBaseIndentation(Token token) const { return GetBaseIndentation(SpanStart(token)); }
    int GetBaseIndentation(int position) const;
    std::vector<const IndentBlockOperation *> GetAllRelativeIndentBlockOperations() const;
    int GetAnchorDeltaFromOriginalColumn(Token token) const;
    int GetDeltaFromPreviousChangesMap(Token token, const std::unordered_map<Token, int> &map) const;

    bool IsWrappingSuppressed(TextSpan span) const;
    bool IsSpacingSuppressed(TextSpan span) const;
    bool IsSpacingSuppressed(int pair_index) const;
    bool IsFormattingDisabled(TextSpan span) const;
    bool IsFormattingDisabled(int pair_index) const;

    /** @brief The applier of TriviaData.Format: the regular one, or the beginning / end of tree ones. */
    void Apply(int pair_index, TriviaData *data);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

/** @brief AbstractTriviaDataFactory + C# TriviaDataFactory. */
class TriviaFactory {
public:
    explicit TriviaFactory(Engine &engine);
    TriviaData *CreateLeadingTrivia(Token token);
    TriviaData *CreateTrailingTrivia(Token token);
    TriviaData *Create(Token token1, Token token2);

    Engine &engine;

    TriviaData *GetSpaceTriviaData(int space);
    TriviaData *GetWhitespaceTriviaData(int line_breaks, int indentation, bool use_trivia_as_is);

private:
    std::vector<TriviaData *> spaces_;
    std::vector<TriviaData *> whitespaces_;  // [lineIndex * 20 + level]
};

/** @brief The trivia of two tokens in order: token1's trailing then token2's leading (TriviaList). */
std::vector<Trivia> TriviaListBetween(Token token1, Token token2);

/** @brief Runs the engine over the main tree; returns the formatted text. */
U16 FormatTree(const Tree &tree, const Rules &rules, const LineOptions &options);
/** @brief CSharpStructuredTriviaFormatEngine.Format(...).GetFormattedRoot() as text. */
U16 FormatStructuredTrivia(const Tree &tree, const Rules &rules, const LineOptions &options, Trivia trivia,
                           int initial_column, bool *changed);

}  // namespace csfmt
