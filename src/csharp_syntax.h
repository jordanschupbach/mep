#ifndef MEP_CSHARP_SYNTAX_H
#define MEP_CSHARP_SYNTAX_H

// mep's C# syntax trees: a C++ port of Roslyn's green trees (the immutable,
// position-free nodes Roslyn's parser builds -- Microsoft.CodeAnalysis's
// GreenNode and the C# InternalSyntax classes), with Roslyn's node and token
// kinds (SyntaxKind, numbered as Roslyn numbers them) and its syntax
// diagnostics (ErrorCode, worded as CSharpResources.resx words them).
//
// Roslyn is MIT-licensed: Copyright (c) .NET Foundation and Contributors
// (https://github.com/dotnet/roslyn/blob/main/License.txt). This file, the
// lexer (csharp_lexer*.cpp) and the parsers (csharp_parser*.cpp,
// csharp_directive_parser.cpp, csharp_doc_parser.cpp) are translations of
// Roslyn's src/Compilers/{Core,CSharp}/Portable/{Syntax,Parser} at the
// commit the .NET 10 SDK's compiler was built from (5.6.0-2.26329.109,
// dotnet/dotnet 35b593be).
//
// The tree, for tools built on it (the language server, a formatter):
//   - Text is UTF-16, like Roslyn's: every width and position is in UTF-16
//     code units, so positions are Roslyn's own. ParseText (csharp_parser.h)
//     takes UTF-8 and keeps a UTF-8 <-> UTF-16 offset map.
//   - A GreenNode is a node (a Syntax.xml node, its fields as slots, see
//     csharp_syntax_factory.h for reading them by name), a list (kind List:
//     the items of a SyntaxList / SeparatedSyntaxList -- an empty list is
//     nullptr and a one-item list is the item itself, as in Roslyn), a token
//     (text, leading and trailing trivia), or a trivia (text; structured
//     trivia -- directives, skipped tokens, doc comments -- are nodes sitting
//     in a token's trivia list).
//   - Nodes carry no positions: a node's position is the sum of the full
//     widths before it. SyntaxTree (csharp_parser.h) builds a red layer
//     (parents and positions) over the green tree.
//   - All nodes of a parse live in an Arena owned by its SyntaxTree; they
//     are immutable once built and may be shared.

#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace csharp {

// --- Kinds ------------------------------------------------------------------

enum class SyntaxKind : uint16_t {
#define CS_KIND(name, value) name = value,
#include "csharp_syntax_kinds.inc"
#undef CS_KIND
};

/** @brief The kind's name as Roslyn spells it ("ClassDeclaration", "IdentifierToken"). */
const char *KindName(SyntaxKind kind);

// --- Diagnostics --------------------------------------------------------------

enum class ErrorCode : uint16_t {
    Void = 0,
    Unknown = 1,
#define CS_ERROR(name, number, severity, message) name = number,
#include "csharp_errors.inc"
#undef CS_ERROR
};

enum class DiagnosticSeverity : uint8_t { Hidden, Info, Warning, Error };

/** @brief "CS1002". */
std::string ErrorId(ErrorCode code);
/** @brief The resource text with its {0} placeholders ("{0} expected"). */
const char *ErrorMessageFormat(ErrorCode code);
DiagnosticSeverity ErrorSeverity(ErrorCode code);

using Text = std::u16string_view;

/** @brief A diagnostic attached to a green node: Roslyn's SyntaxDiagnosticInfo. `offset` is relative to the
 *         node's start (its full start plus its leading trivia), as in Roslyn. Arguments are kept pre-formatted. */
struct SyntaxDiagnosticInfo {
    int offset = 0;
    int width = 0;
    ErrorCode code = ErrorCode::Void;
    uint8_t arg_count = 0;
    Text args[4];
};

/** @brief The message with its arguments substituted, as UTF-8. */
std::string FormatDiagnosticMessage(const SyntaxDiagnosticInfo &d);

// --- Text helpers ------------------------------------------------------------

std::string ToUtf8(Text text);
std::u16string ToUtf16(std::string_view utf8);

// --- Arena --------------------------------------------------------------------

/**
 * @brief Owns every green node, text and builder of one parse. Allocation is a bump pointer; nothing is freed
 *        before the arena dies. CurrentArena() is the one the factories allocate from (ArenaScope sets it).
 */
class Arena {
public:
    Arena();
    ~Arena();
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;

    void *Allocate(size_t bytes, size_t align = alignof(std::max_align_t));
    template <class T>
    T *NewArray(size_t n) {
        static_assert(std::is_trivially_destructible<T>::value, "arena types are never destroyed");
        T *p = static_cast<T *>(Allocate(sizeof(T) * (n ? n : 1), alignof(T)));
        for (size_t i = 0; i < n; i++) new (p + i) T();
        return p;
    }
    /** @brief A copy of `text` that lives as long as the arena. */
    Text CopyText(Text text);
    Text CopyText(const std::u16string &text) { return CopyText(Text(text)); }
    /** @brief Storage for a SyntaxListBuilder, freed with the arena. */
    std::vector<struct GreenNode *> *NewBuilderStorage();

private:
    std::vector<std::unique_ptr<char[]>> blocks_;
    char *cur_ = nullptr;
    size_t left_ = 0;
    std::vector<std::unique_ptr<std::vector<GreenNode *>>> builders_;
};

Arena *CurrentArena();

class ArenaScope {
public:
    explicit ArenaScope(Arena *arena);
    ~ArenaScope();
    ArenaScope(const ArenaScope &) = delete;
    ArenaScope &operator=(const ArenaScope &) = delete;

private:
    Arena *saved_;
};

// --- Green nodes ----------------------------------------------------------------

enum NodeFlags : uint16_t {
    kIsNotMissing = 1 << 0,
    kContainsDiagnostics = 1 << 1,
    kContainsStructuredTrivia = 1 << 2,
    kContainsDirectives = 1 << 3,
    kContainsSkippedText = 1 << 4,
    kInheritMask = kIsNotMissing | kContainsDiagnostics | kContainsStructuredTrivia | kContainsDirectives |
                   kContainsSkippedText,
};

enum class GreenType : uint8_t { Node, List, Token, Trivia };

/** @brief A token's literal value (Roslyn's boxed SyntaxToken.Value), for numeric and character literals. */
struct LiteralValue {
    enum Type : uint8_t { None, Int32, UInt32, Int64, UInt64, Single, Double, Decimal, Char, Bool };
    Type type = None;
    uint64_t bits = 0;   // integers (two's complement), Char (the UTF-16 unit), Bool
    double real = 0;     // Single, Double, and Decimal (approximated)
};

struct GreenNode {
    SyntaxKind kind = SyntaxKind::None;
    GreenType type = GreenType::Node;
    uint16_t flags = 0;
    uint8_t bool_fields = 0;  // a directive's IsActive / BranchTaken / ConditionValue (csharp_syntax_factory.h)
    int full_width = 0;

    // Node and List.
    int slot_count = 0;
    GreenNode **slots = nullptr;

    // Token: its kind as a contextual keyword (an identifier `var` has ContextualKind VarKeyword), text (empty
    // when missing), ValueText (an identifier's name without @ and escapes, a string's value), trivia.
    // Trivia: text.
    SyntaxKind contextual_kind = SyntaxKind::None;
    Text text;
    Text value_text;
    GreenNode *leading = nullptr;
    GreenNode *trailing = nullptr;
    const LiteralValue *value = nullptr;

    const SyntaxDiagnosticInfo *diags = nullptr;
    int diag_count = 0;

    bool IsToken() const { return type == GreenType::Token; }
    bool IsTrivia() const { return type == GreenType::Trivia; }
    bool IsList() const { return type == GreenType::List; }
    bool IsNode() const { return type == GreenType::Node; }
    bool IsMissing() const { return (flags & kIsNotMissing) == 0; }
    bool ContainsDiagnostics() const { return (flags & kContainsDiagnostics) != 0; }
    bool ContainsSkippedText() const { return (flags & kContainsSkippedText) != 0; }
    bool ContainsStructuredTrivia() const { return (flags & kContainsStructuredTrivia) != 0; }
    bool ContainsDirectives() const { return (flags & kContainsDirectives) != 0; }
    bool IsStructuredTrivia() const;
    bool IsDirective() const;
    bool IsSkippedTokensTrivia() const { return kind == SyntaxKind::SkippedTokensTrivia; }

    int SlotCount() const { return slot_count; }
    GreenNode *GetSlot(int i) const { return i < slot_count ? slots[i] : nullptr; }

    /** @brief The token's own text, or the trivia's. */
    Text Text_() const { return text; }
    /** @brief Width without the leading trivia of the first token and the trailing trivia of the last. */
    int Width() const { return full_width - GetLeadingTriviaWidth() - GetTrailingTriviaWidth(); }
    int GetLeadingTriviaWidth() const;
    int GetTrailingTriviaWidth() const;
    /** @brief A token's leading (trailing) trivia; nullptr for anything else, as in Roslyn. */
    GreenNode *GetLeadingTrivia() const;
    GreenNode *GetTrailingTrivia() const;
    /** @brief The first / last token (or trivia leaf) below this node, missing ones included; nullptr if none. */
    const GreenNode *GetFirstTerminal() const;
    const GreenNode *GetLastTerminal() const;
    /** @brief The first / last token below this node (Roslyn's CSharpSyntaxNode.GetFirstToken / GetLastToken). */
    GreenNode *GetFirstToken() const;
    GreenNode *GetLastToken() const;
    GreenNode *GetLastNonmissingToken() const;
};

/** @brief The node's diagnostics, in order. */
inline const SyntaxDiagnosticInfo *DiagsBegin(const GreenNode *n) { return n->diags; }
inline const SyntaxDiagnosticInfo *DiagsEnd(const GreenNode *n) { return n->diags + n->diag_count; }

/** @brief A copy of `node` with `diags` as its diagnostics (Roslyn's WithDiagnosticsGreen). */
GreenNode *WithDiagnosticsGreen(const GreenNode *node, const SyntaxDiagnosticInfo *diags, int count);
/** @brief A copy of `node` with `extra` appended to its diagnostics (SyntaxParser.WithAdditionalDiagnostics). */
GreenNode *WithAdditionalDiagnostics(const GreenNode *node, const SyntaxDiagnosticInfo &extra);

/** @brief Preorder list of a node and everything below it, tokens included, trivia not (GreenNode.EnumerateNodes). */
std::vector<GreenNode *> EnumerateNodes(GreenNode *node);

// --- Lists ----------------------------------------------------------------------

/** @brief A read view of a green list (Roslyn's InternalSyntax SyntaxList<T>): nullptr is empty, a non-list
 *         node is a one-item list. */
struct SyntaxList {
    GreenNode *node = nullptr;
    SyntaxList() = default;
    SyntaxList(GreenNode *n) : node(n) {}  // NOLINT: implicit, as Roslyn's conversions are
    int Count() const { return node == nullptr ? 0 : node->IsList() ? node->slot_count : 1; }
    GreenNode *operator[](int i) const { return node == nullptr ? nullptr : node->IsList() ? node->slots[i] : node; }
    bool Any() const { return node != nullptr; }
    bool Any(SyntaxKind kind) const;
    GreenNode *First() const { return (*this)[0]; }
    GreenNode *Last() const { return (*this)[Count() - 1]; }
    GreenNode *Node() const { return node; }
    operator GreenNode *() const { return node; }  // NOLINT
};

/** @brief A read view of a separated list: nodes at even indexes, separators at odd ones. */
struct SeparatedSyntaxList {
    SyntaxList list;
    SeparatedSyntaxList() = default;
    SeparatedSyntaxList(GreenNode *n) : list(n) {}  // NOLINT
    int Count() const { return (list.Count() + 1) >> 1; }
    int SeparatorCount() const { return list.Count() >> 1; }
    GreenNode *operator[](int i) const { return list[i << 1]; }
    GreenNode *GetSeparator(int i) const { return list[(i << 1) + 1]; }
    SyntaxList GetWithSeparators() const { return list; }
    GreenNode *Node() const { return list.node; }
    operator GreenNode *() const { return list.node; }  // NOLINT
};

/**
 * @brief Roslyn's SyntaxListBuilder (and SeparatedSyntaxListBuilder<T>, which is the same thing plus
 *        AddSeparator). A handle: copies refer to the same items, as the C# struct wrappers do. A
 *        default-constructed builder is null (IsNull()), like a `SyntaxListBuilder? x = null`.
 */
class SyntaxListBuilder {
public:
    SyntaxListBuilder() = default;
    explicit SyntaxListBuilder(std::vector<GreenNode *> *items) : items_(items) {}
    /** @brief A fresh builder in the current arena (`new SyntaxListBuilder(n)`). */
    static SyntaxListBuilder Create();

    bool IsNull() const { return items_ == nullptr; }
    int Count() const { return static_cast<int>(items_->size()); }
    GreenNode *&operator[](int i) { return (*items_)[static_cast<size_t>(i)]; }
    GreenNode *operator[](int i) const { return (*items_)[static_cast<size_t>(i)]; }
    GreenNode *&Last() { return items_->back(); }
    /** @brief Adds an item; a list is added item by item; nullptr is ignored. */
    SyntaxListBuilder &Add(GreenNode *item);
    void AddSeparator(GreenNode *separator) { Add(separator); }
    void AddRange(SyntaxList list) { AddRange(list, 0, list.Count()); }
    void AddRange(SyntaxList list, int offset, int length);
    void AddRange(const SyntaxListBuilder &other);
    /** @brief SeparatedSyntaxListBuilder.AddRange(nodes, count), verbatim (its offset quirk included). */
    void AddRangeSeparated(SeparatedSyntaxList nodes, int count);
    void RemoveLast() { items_->pop_back(); }
    void Clear() { items_->clear(); }
    bool Any(SyntaxKind kind) const;
    std::vector<GreenNode *> ToArray() const { return *items_; }
    /** @brief nullptr for none, the item for one, else a list node. */
    GreenNode *ToListNode() const;
    GreenNode *ToList() const { return items_ == nullptr ? nullptr : ToListNode(); }
    operator GreenNode *() const { return ToList(); }  // NOLINT: SyntaxListBuilder<T> -> SyntaxList<T>
    operator SyntaxList() const { return SyntaxList(ToList()); }  // NOLINT
    operator SeparatedSyntaxList() const { return SeparatedSyntaxList(ToList()); }  // NOLINT
    std::vector<GreenNode *> *Storage() const { return items_; }

private:
    std::vector<GreenNode *> *items_ = nullptr;
};
using SeparatedSyntaxListBuilder = SyntaxListBuilder;

/** @brief Roslyn's SyntaxListPool: hands out cleared builders and takes them back. */
class SyntaxListPool {
public:
    SyntaxListBuilder Allocate();
    SyntaxListBuilder AllocateSeparated() { return Allocate(); }
    void Free(SyntaxListBuilder builder);
    /** @brief builder.ToList(), then Free(builder) (SyntaxListPoolExtensions.ToListAndFree). */
    GreenNode *ToListAndFree(SyntaxListBuilder builder) {
        GreenNode *list = builder.ToList();
        Free(builder);
        return list;
    }

private:
    std::vector<std::vector<GreenNode *> *> free_;
};

// --- Construction (Roslyn's InternalSyntax.SyntaxFactory, the hand-written half) ---------
// The generated half (one function per Syntax.xml node) is csharp_syntax_factory.h.

namespace SyntaxFactory {

/** @brief A node of `kind` with these slots (nullptr allowed); computes widths and flags. */
GreenNode *MakeNode(SyntaxKind kind, GreenNode *const *slots, int count);
/** @brief A list node of these items (callers normally use SyntaxListBuilder). */
GreenNode *MakeList(GreenNode *const *items, int count);
GreenNode *List(GreenNode *a, GreenNode *b);
GreenNode *List(GreenNode *a, GreenNode *b, GreenNode *c);

// Trivia.
GreenNode *Trivia(SyntaxKind kind, Text text);
GreenNode *Whitespace(Text text);
GreenNode *EndOfLine(Text text);
GreenNode *Comment(Text text);
GreenNode *ConflictMarker(Text text);
GreenNode *DisabledText(Text text);
GreenNode *PreprocessingMessage(Text text);
GreenNode *DocumentationCommentExteriorTrivia(Text text);
extern GreenNode *const Space;  // a single " " (identity matters to Roslyn in a few places)

// Tokens.
/** @brief A token with its kind's well-known text; for kinds without one (identifiers, literals...), a missing token. */
GreenNode *Token(SyntaxKind kind);
GreenNode *Token(GreenNode *leading, SyntaxKind kind, GreenNode *trailing);
GreenNode *Token(GreenNode *leading, SyntaxKind kind, Text text, GreenNode *trailing);
GreenNode *Token(GreenNode *leading, SyntaxKind kind, Text text, Text value_text, GreenNode *trailing);
GreenNode *MissingToken(SyntaxKind kind);
GreenNode *MissingToken(GreenNode *leading, SyntaxKind kind, GreenNode *trailing);
GreenNode *Identifier(Text text);
GreenNode *Identifier(GreenNode *leading, Text text, GreenNode *trailing);
GreenNode *Identifier(SyntaxKind contextual_kind, GreenNode *leading, Text text, Text value_text, GreenNode *trailing);
/** @brief A token carrying a value (Roslyn's SyntaxToken.WithValue): literals, bad tokens, XML text. */
GreenNode *TokenWithValue(SyntaxKind kind, GreenNode *leading, Text text, Text value_text, const LiteralValue *value,
                          GreenNode *trailing);
GreenNode *Literal(GreenNode *leading, Text text, SyntaxKind kind, Text value, GreenNode *trailing);
GreenNode *BadToken(GreenNode *leading, Text text, GreenNode *trailing);
GreenNode *XmlTextLiteral(GreenNode *leading, Text text, Text value, GreenNode *trailing);
GreenNode *XmlTextNewLine(GreenNode *leading, Text text, Text value, GreenNode *trailing);
GreenNode *XmlTextNewLine(Text text);
GreenNode *XmlEntity(GreenNode *leading, Text text, Text value, GreenNode *trailing);

}  // namespace SyntaxFactory

// Token operations (Roslyn's SyntaxToken members).
/** @brief The token with its leading (trailing) trivia replaced; kind, text, value and diagnostics kept. */
GreenNode *TokenWithLeadingTrivia(const GreenNode *token, GreenNode *trivia);
GreenNode *TokenWithTrailingTrivia(const GreenNode *token, GreenNode *trivia);
/** @brief A copy of a node or token with one slot replaced (diagnostics kept, like the rewriter's Update). */
GreenNode *WithSlot(const GreenNode *node, int index, GreenNode *child);

// --- Facts (Roslyn's SyntaxFacts / SyntaxKindFacts, csharp_syntax_facts.cpp) ----------------

namespace SyntaxFacts {

Text GetText(SyntaxKind kind);
bool IsAnyToken(SyntaxKind kind);
bool IsTrivia(SyntaxKind kind);
bool IsPunctuation(SyntaxKind kind);
bool IsLanguagePunctuation(SyntaxKind kind);
bool IsPreprocessorPunctuation(SyntaxKind kind);
bool IsPunctuationOrKeyword(SyntaxKind kind);
bool IsLiteral(SyntaxKind kind);
bool IsKeywordKind(SyntaxKind kind);
bool IsReservedKeyword(SyntaxKind kind);
bool IsContextualKeyword(SyntaxKind kind);
bool IsQueryContextualKeyword(SyntaxKind kind);
bool IsPreprocessorKeyword(SyntaxKind kind);
bool IsPreprocessorContextualKeyword(SyntaxKind kind);
SyntaxKind GetKeywordKind(Text text);
SyntaxKind GetContextualKeywordKind(Text text);
SyntaxKind GetPreprocessorKeywordKind(Text text);
bool IsPredefinedType(SyntaxKind kind);
bool IsTypeSyntax(SyntaxKind kind);
bool IsName(SyntaxKind kind);
bool IsAccessorDeclaration(SyntaxKind kind);
SyntaxKind GetAccessorDeclarationKind(SyntaxKind keyword);
bool IsAnyUnaryExpression(SyntaxKind token);
bool IsPrefixUnaryExpression(SyntaxKind token);
bool IsPrefixUnaryExpressionOperatorToken(SyntaxKind token);
SyntaxKind GetPrefixUnaryExpression(SyntaxKind token);
bool IsPostfixUnaryExpression(SyntaxKind token);
SyntaxKind GetPostfixUnaryExpression(SyntaxKind token);
bool IsUnaryOperatorDeclarationToken(SyntaxKind token);
bool IsAnyOverloadableOperator(SyntaxKind kind);
bool IsOverloadableBinaryOperator(SyntaxKind kind);
bool IsOverloadableUnaryOperator(SyntaxKind kind);
bool IsOverloadableCompoundAssignmentOperator(SyntaxKind kind);
bool IsBinaryExpression(SyntaxKind token);
bool IsBinaryExpressionOperatorToken(SyntaxKind token);
SyntaxKind GetBinaryExpression(SyntaxKind token);
bool IsAssignmentExpression(SyntaxKind kind);
bool IsAssignmentExpressionOperatorToken(SyntaxKind token);
SyntaxKind GetAssignmentExpression(SyntaxKind token);
SyntaxKind GetCheckStatement(SyntaxKind keyword);
SyntaxKind GetLiteralExpression(SyntaxKind token);
bool IsLiteralExpression(SyntaxKind token);
SyntaxKind GetBaseTypeDeclarationKind(SyntaxKind kind);
SyntaxKind GetTypeDeclarationKind(SyntaxKind kind);
SyntaxKind GetPrimaryFunction(SyntaxKind keyword);
SyntaxKind GetSwitchLabelKind(SyntaxKind keyword);
SyntaxKind GetInstanceExpression(SyntaxKind token);
bool IsAttributeTargetSpecifier(SyntaxKind kind);
bool IsDocumentationCommentTrivia(SyntaxKind kind);
bool IsPreprocessorDirective(SyntaxKind kind);
// The rest of SyntaxKindFacts.cs.
std::vector<SyntaxKind> GetReservedKeywordKinds();
std::vector<SyntaxKind> GetKeywordKinds();
std::vector<SyntaxKind> GetPreprocessorKeywordKinds();
std::vector<SyntaxKind> GetPunctuationKinds();
std::vector<SyntaxKind> GetContextualKeywordKinds();
bool IsAccessibilityModifier(SyntaxKind kind);
bool IsGlobalMemberDeclaration(SyntaxKind kind);
bool IsTypeDeclaration(SyntaxKind kind);
bool IsNamespaceMemberDeclaration(SyntaxKind kind);
bool IsPostfixUnaryExpressionToken(SyntaxKind token);
bool IsIncrementOrDecrementOperator(SyntaxKind token);
bool IsPrimaryFunction(SyntaxKind keyword);
bool IsInstanceExpression(SyntaxKind token);
bool IsAccessorDeclarationKeyword(SyntaxKind keyword);
bool IsTypeParameterVarianceKeyword(SyntaxKind kind);
// SyntaxFacts.cs's extension methods on InternalSyntax.SyntaxToken (`token.IsIdentifierVar()`).
bool IsIdentifierVar(const GreenNode *node);
bool IsIdentifierVarOrPredefinedType(const GreenNode *node);

// Characters (SyntaxFacts.cs / CharacterInfo.cs).
bool IsHexDigit(char16_t c);
bool IsBinaryDigit(char16_t c);
bool IsDecDigit(char16_t c);
int HexValue(char16_t c);
int BinaryValue(char16_t c);
int DecValue(char16_t c);
bool IsWhitespace(char16_t ch);
bool IsNewLine(char16_t ch);
bool IsNewLine(char16_t ch1, char16_t ch2);
bool IsIdentifierStartCharacter(char16_t ch);
bool IsIdentifierPartCharacter(char16_t ch);
bool IsValidIdentifier(Text name);
bool IsLetterChar(int unicode_category);
bool IsDecimalDigitChar(int unicode_category);
bool IsConnectingChar(int unicode_category);
bool IsCombiningChar(int unicode_category);
bool IsFormattingChar(char16_t ch);
bool IsFormattingChar(int unicode_category);
bool IsNonAsciiQuotationMark(char16_t ch);
bool ContainsDroppedIdentifierCharacters(Text name);

}  // namespace SyntaxFacts

/** @brief .NET's char.GetUnicodeCategory for a UTF-16 unit (System.Globalization.UnicodeCategory numbering). */
int GetUnicodeCategory(char16_t ch);

}  // namespace csharp

#endif  // MEP_CSHARP_SYNTAX_H
