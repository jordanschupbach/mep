#ifndef MEP_CSHARP_PARSER_INTERNAL_H
#define MEP_CSHARP_PARSER_INTERNAL_H

// The insides of mep's C# parser: a method-for-method translation of
// Roslyn's lexer and parsers (src/Compilers/CSharp/Portable/Parser/*.cs, MIT,
// (c) .NET Foundation and Contributors; see csharp_syntax.h for the commit).
// The public entry point is csharp_parser.h; nothing here is meant for
// callers outside the csharp_* sources.
//
// Translation conventions (so the C++ can be read side by side with the C#):
//   - Classes, methods, fields and locals keep Roslyn's names: Lexer,
//     SyntaxParser, LanguageParser, DirectiveParser, DocumentationCommentParser;
//     ParseQualifiedName(), _termState, ...  C# properties become methods
//     (this.CurrentToken -> CurrentToken()).
//   - SyntaxToken and every XxxSyntax are GreenNode*; SyntaxList<T> is
//     SyntaxList, SeparatedSyntaxList<T> is SeparatedSyntaxList, and both
//     list builders are SyntaxListBuilder (a handle, like the C# structs).
//     A node's field is GetField(node, Field::X) (csharp_syntax_factory.h).
//   - `ref`/`out` parameters are references; `bool?` is std::optional<bool>;
//     C# strings are std::u16string (owned) or Text (a view into the source
//     or the parse's Arena).
//   - The options are fixed: LanguageVersion.Preview (every feature enabled,
//     so CheckFeatureAvailability never reports), SourceCodeKind.Regular,
//     DocumentationMode.Parse, no preprocessor symbols defined.
//   - Debug.Assert is CS_ASSERT (compiled out).
//   - The incremental-parsing machinery (Blender over an old tree,
//     QuickScanner, node caches) is not ported: those reuse work, they never
//     change a tree. What a fresh parse does through them is (the doc-comment
//     parser's mode-resettable token stream re-lexes from each token's saved
//     lexer state, as the Blender does with no old tree).

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "csharp_syntax.h"
#include "csharp_syntax_factory.h"

#define CS_ASSERT(x) ((void)0)

namespace csharp {

/** @brief Bit operations for a flags enum class. */
#define CS_FLAGS_ENUM(E)                                                                                          \
    inline constexpr E operator|(E a, E b) {                                                                      \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) | static_cast<std::underlying_type_t<E>>(b)); \
    }                                                                                                             \
    inline constexpr E operator&(E a, E b) {                                                                      \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) & static_cast<std::underlying_type_t<E>>(b)); \
    }                                                                                                             \
    inline constexpr E operator~(E a) { return static_cast<E>(~static_cast<std::underlying_type_t<E>>(a)); }      \
    inline E &operator|=(E &a, E b) { return a = a | b; }                                                         \
    inline E &operator&=(E &a, E b) { return a = a & b; }                                                         \
    inline constexpr bool Any(E a) { return static_cast<std::underlying_type_t<E>>(a) != 0; }

// --- Lexer modes (Lexer.cs) ------------------------------------------------------------

enum class LexerMode : uint32_t {
    None = 0,
    Syntax = 0x0001,
    DebuggerSyntax = 0x0002,
    Directive = 0x0004,
    XmlDocComment = 0x0008,
    XmlElementTag = 0x0010,
    XmlAttributeTextQuote = 0x0020,
    XmlAttributeTextDoubleQuote = 0x0040,
    XmlCrefQuote = 0x0080,
    XmlCrefDoubleQuote = 0x0100,
    XmlNameQuote = 0x0200,
    XmlNameDoubleQuote = 0x0400,
    XmlCDataSectionText = 0x0800,
    XmlCommentText = 0x1000,
    XmlProcessingInstructionText = 0x2000,
    XmlCharacter = 0x4000,
    MaskLexMode = 0xFFFF,
    XmlDocCommentLocationStart = 0x00000,
    XmlDocCommentLocationInterior = 0x10000,
    XmlDocCommentLocationExterior = 0x20000,
    XmlDocCommentLocationEnd = 0x40000,
    MaskXmlDocCommentLocation = 0xF0000,
    XmlDocCommentStyleSingleLine = 0x000000,
    XmlDocCommentStyleDelimited = 0x100000,
    MaskXmlDocCommentStyle = 0x300000,
};
CS_FLAGS_ENUM(LexerMode)

enum class XmlDocCommentLocation : uint32_t { Start = 0, Interior = 1, Exterior = 2, End = 4 };
enum class XmlDocCommentStyle : uint32_t { SingleLine = 0, Delimited = 1 };

// --- Directives (Directives.cs) -------------------------------------------------------------

/** @brief A directive on the stack: wraps a directive trivia node. */
struct Directive {
    GreenNode *node = nullptr;
    Directive() = default;
    explicit Directive(GreenNode *n) : node(n) {}
    SyntaxKind Kind() const { return node->kind; }
    /** @brief A #define / #undef's name (its ValueText); empty for other directives. */
    std::u16string GetIdentifier() const;
    bool HasIdentifier() const;
    bool IsActive() const;
    bool BranchTaken() const;
};

enum class DefineState { Defined, Undefined, Unspecified };

/**
 * @brief Roslyn's DirectiveStack: an immutable cons list of directives. Like the C# struct it can be null
 *        (default-constructed, IsNull()) as well as empty (Empty()) -- the two behave differently and a reset
 *        of the doc-comment parser's token stream can leave the lexer with a null one, as in Roslyn.
 */
class DirectiveStack {
public:
    struct Cons;
    DirectiveStack() = default;
    static DirectiveStack Empty();
    bool IsNull() const { return list_ == nullptr; }
    bool IsEmpty() const;
    bool SeenAnyIfDirectives() const { return seen_any_if_; }
    DefineState IsDefined(Text id) const;
    bool PreviousBranchTaken() const;
    bool HasUnfinishedIf() const;
    bool HasPreviousIfOrElif() const;
    bool HasUnfinishedRegion() const;
    DirectiveStack Add(Directive directive) const;

private:
    DirectiveStack(std::shared_ptr<const Cons> list, bool seen) : list_(std::move(list)), seen_any_if_(seen) {}
    std::shared_ptr<const Cons> list_;
    bool seen_any_if_ = false;
    friend struct DirectiveStackAccess;
};

/** @brief Applies the directives found in a node / token / trivia list to a stack (CSharpSyntaxNode.ApplyDirectives). */
DirectiveStack ApplyDirectives(const GreenNode *node, DirectiveStack stack);

// --- Text window (SlidingTextWindow.cs) ------------------------------------------------------------

/** @brief The lexer's cursor over the whole text (Roslyn slides a window; the text here is all in memory). */
struct SlidingTextWindow {
    static constexpr char16_t InvalidCharacter = u'￿';

    Text text;  // SlidingTextWindow.Text
    int position = 0;

    int Position() const { return position; }
    Text Text_() const { return text; }
    void Reset(int pos) { position = pos; }
    bool IsReallyAtEnd() const { return position >= static_cast<int>(text.size()); }
    void AdvanceChar() { position++; }
    void AdvanceChar(int n) { position += n; }
    bool TryAdvance(char16_t c) {
        if (PeekChar() != c) return false;
        AdvanceChar();
        return true;
    }
    void AdvancePastNewLine() { AdvanceChar(GetNewLineWidth()); }
    int GetNewLineWidth() const { return GetNewLineWidth(PeekChar(), PeekChar(1)); }
    static int GetNewLineWidth(char16_t current, char16_t next) {
        return current == u'\r' && next == u'\n' ? 2 : 1;
    }
    /** @brief The current character, then advance; InvalidCharacter (not advancing) at the end. */
    char16_t NextChar() {
        char16_t c = PeekChar();
        if (c != InvalidCharacter || !IsReallyAtEnd()) position++;
        return c;
    }
    char16_t PeekChar() const {
        return position >= 0 && position < static_cast<int>(text.size()) ? text[static_cast<size_t>(position)]
                                                                          : InvalidCharacter;
    }
    char16_t PeekChar(int delta) const {
        int p = position + delta;
        return p >= 0 && p < static_cast<int>(text.size()) ? text[static_cast<size_t>(p)] : InvalidCharacter;
    }
    char16_t PreviousChar() const { return position > 0 ? text[static_cast<size_t>(position - 1)] : InvalidCharacter; }
    bool AdvanceIfMatches(Text desired) {
        for (size_t i = 0; i < desired.size(); i++) {
            if (PeekChar(static_cast<int>(i)) != desired[i]) return false;
        }
        AdvanceChar(static_cast<int>(desired.size()));
        return true;
    }
    Text GetText(int start, int length) const {
        return text.substr(static_cast<size_t>(start), static_cast<size_t>(length));
    }
    Text GetText(int start) const { return GetText(start, position - start); }
};

// --- Lexer (Lexer*.cs, AbstractLexer.cs) ---------------------------------------------------------------

class DirectiveParser;
class DocumentationCommentParser;

/** @brief System.Range over a text, as the interpolated-string scanner reports them. */
struct TextRange {
    int start = 0;
    int end = 0;
    int Length() const { return end - start; }
};

enum class SpecialType : uint8_t { None, System_Int32, System_UInt32, System_Int64, System_UInt64, System_Single,
                                   System_Double, System_Decimal };

class Lexer {
public:
    struct TokenInfo {
        SyntaxKind Kind = SyntaxKind::None;
        SyntaxKind ContextualKind = SyntaxKind::None;
        Text Text_;  // TokenInfo.Text
        bool HasText = false;  // Text != null
        SpecialType ValueKind = SpecialType::None;
        bool RequiresTextForXmlEntity = false;
        bool HasIdentifierEscapeSequence = false;
        Text StringValue;
        bool HasStringValue = false;  // StringValue != null
        char16_t CharValue = 0;
        int32_t IntValue = 0;
        uint32_t UintValue = 0;
        int64_t LongValue = 0;
        uint64_t UlongValue = 0;
        float FloatValue = 0;
        double DoubleValue = 0;
        double DecimalValue = 0;  // decimal, approximated (the range check is done exactly while scanning)
        bool IsVerbatim = false;
    };

    enum class InterpolatedStringKind { Normal, Verbatim, SingleLineRaw, MultiLineRaw };

    struct Interpolation {
        TextRange OpenBraceRange;
        TextRange ColonRange;  // empty if there is no colon
        TextRange CloseBraceRange;  // empty if there was no close brace (an error)
        bool HasColon() const { return ColonRange.start != ColonRange.end; }
    };

    /** @param text the whole source (or a substring for an interpolation); must outlive the lexer's tokens. */
    explicit Lexer(Text text, bool allowPreprocessorDirectives = true, bool interpolationFollowedByColon = false);
    ~Lexer();
    Lexer(const Lexer &) = delete;
    Lexer &operator=(const Lexer &) = delete;

    GreenNode *Lex(LexerMode mode);
    /** @brief Lex(ref LexerMode mode): also updates `mode` to the lexer's mode afterwards. */
    GreenNode *LexRef(LexerMode &mode);
    DirectiveStack Directives() const { return _directives; }
    bool InterpolationFollowedByColon() const { return _interpolationFollowedByColon; }
    bool SuppressDocumentationCommentParse() const { return false; }  // DocumentationMode.Parse
    void Reset(int position, DirectiveStack directives) {
        TextWindow.Reset(position);
        _directives = std::move(directives);
    }
    /** @brief Leading / trailing trivia at the current position (LexSyntaxLeadingTrivia().Node). */
    GreenNode *LexSyntaxLeadingTrivia();
    GreenNode *LexSyntaxTrailingTrivia();
    GreenNode *LexEndOfDirectiveWithOptionalPreprocessingMessage();
    GreenNode *LexEndOfDirectiveWithOptionalContent(GreenNode *&content);
    void ScanInterpolatedOrRawStringLiteralTop(TokenInfo &info, bool isInterpolatedString,
                                               std::optional<SyntaxDiagnosticInfo> &error,
                                               InterpolatedStringKind &kind, TextRange &openQuoteRange,
                                               std::vector<Interpolation> *interpolations, TextRange &closeQuoteRange);
    bool TryScanXmlEntity(char16_t &ch, char16_t &surrogate);

    SlidingTextWindow TextWindow;

    // AbstractLexer.
protected:
    int LexemeStartPosition = 0;
    std::vector<SyntaxDiagnosticInfo> _errors;
    bool _hasErrors = false;  // _errors != null
    void Start() {
        LexemeStartPosition = TextWindow.Position();
        _errors.clear();
        _hasErrors = false;
    }
    bool HasErrors() const { return _hasErrors; }
    void AddError(int position, int width, ErrorCode code) { AddError(MakeError(position, width, code)); }
    void AddError(int position, int width, ErrorCode code, std::initializer_list<Text> args) {
        AddError(MakeError(position, width, code, args));
    }
    void AddError(ErrorCode code) { AddError(MakeError(code)); }
    void AddError(ErrorCode code, std::initializer_list<Text> args) { AddError(MakeError(code, args)); }
    void AddError(const std::optional<SyntaxDiagnosticInfo> &error) {
        if (error) AddError(*error);
    }
    void AddError(const SyntaxDiagnosticInfo &error) {
        _errors.push_back(error);
        _hasErrors = true;
    }
    SyntaxDiagnosticInfo MakeError(int position, int width, ErrorCode code) const;
    SyntaxDiagnosticInfo MakeError(int position, int width, ErrorCode code, std::initializer_list<Text> args) const;
    static SyntaxDiagnosticInfo MakeError(ErrorCode code);
    static SyntaxDiagnosticInfo MakeError(ErrorCode code, std::initializer_list<Text> args);
    int GetLexemeOffsetFromPosition(int position) const {
        return position >= LexemeStartPosition ? position - LexemeStartPosition : position;
    }
    Text GetNonInternedLexemeText() const { return TextWindow.GetText(LexemeStartPosition); }
    Text GetInternedLexemeText() const { return TextWindow.GetText(LexemeStartPosition); }
    int CurrentLexemeWidth() const { return TextWindow.Position() - LexemeStartPosition; }

    // Lexer state shared by the lexer's translation units.
    LexerMode _mode = LexerMode::None;
    std::u16string _builder;  // StringBuilder _builder
    std::u16string _identBuffer;
    DirectiveStack _directives;
    bool _allowPreprocessorDirectives = true;
    bool _interpolationFollowedByColon = false;
    int _badTokenCount = 0;
    std::unique_ptr<DocumentationCommentParser> _xmlParser;
    std::unique_ptr<DirectiveParser> _directiveParser;
    SyntaxListBuilder _leadingTriviaCache;
    SyntaxListBuilder _trailingTriviaCache;
    SyntaxListBuilder _directiveTriviaCache;  // null until first used

    static LexerMode ModeOf(LexerMode mode) { return mode & LexerMode::MaskLexMode; }
    bool ModeIs(LexerMode mode) const { return ModeOf(_mode) == mode; }
    static XmlDocCommentLocation LocationOf(LexerMode mode) {
        return static_cast<XmlDocCommentLocation>(static_cast<uint32_t>(mode & LexerMode::MaskXmlDocCommentLocation) >> 16);
    }
    bool LocationIs(XmlDocCommentLocation location) const { return LocationOf(_mode) == location; }
    void MutateLocation(XmlDocCommentLocation location) {
        _mode = _mode & ~LexerMode::MaskXmlDocCommentLocation;
        _mode = _mode | static_cast<LexerMode>(static_cast<uint32_t>(location) << 16);
    }
    static XmlDocCommentStyle StyleOf(LexerMode mode) {
        return static_cast<XmlDocCommentStyle>(static_cast<uint32_t>(mode & LexerMode::MaskXmlDocCommentStyle) >> 20);
    }
    bool StyleIs(XmlDocCommentStyle style) const { return StyleOf(_mode) == style; }
    bool InDocumentationComment() const;

    /** @brief Interns a string built by the lexer (arena copy, so tokens can keep it). */
    static Text Intern(const std::u16string &s) { return CurrentArena()->CopyText(s); }

    // The rest of the lexer's members, by translation unit:
#include "csharp_lexer_members.inc"   // Lexer.cs up to the XML doc-comment lexing (csharp_lexer.cpp)
#include "csharp_lexer_members2.inc"  // Lexer.cs XML/cref lexing, Lexer_StringLiteral.cs, Lexer_RawStringLiteral.cs
                                      // (csharp_lexer_xml.cpp, csharp_lexer_string.cpp)
    friend class DirectiveParser;
    friend class DocumentationCommentParser;
    friend class LanguageParser;
};

// --- SyntaxParser (SyntaxParser.cs) ------------------------------------------------------------------

/** @brief Thrown when parsing nests deeper than the stack allows (Roslyn's InsufficientExecutionStackException). */
struct InsufficientExecutionStack {};

/** @brief Roslyn's StackGuard.EnsureSufficientExecutionStack: throws InsufficientExecutionStack when deep. */
void EnsureSufficientExecutionStack(int recursionDepth);

class SyntaxParser {
public:
    virtual ~SyntaxParser();
    SyntaxParser(const SyntaxParser &) = delete;
    SyntaxParser &operator=(const SyntaxParser &) = delete;

    struct ResetPoint {
        int ResetCount = 0;
        LexerMode Mode = LexerMode::None;
        int Position = 0;
        GreenNode *PrevTokenTrailingTrivia = nullptr;
    };

protected:
    SyntaxParser(Lexer *lexer, LexerMode mode, bool allowModeReset);

    Lexer *lexer;

    void ReInitialize();
    bool IsIncremental() const { return false; }
    ResetPoint GetResetPoint();
    void Reset(ResetPoint &point);
    void Release(ResetPoint &point);
    bool IsScript() const { return false; }
    LexerMode Mode() const { return _mode; }
    void SetMode(LexerMode value);  // the C# Mode setter

    GreenNode *CurrentToken();
    GreenNode *PeekToken(int n);
    GreenNode *EatToken();
    GreenNode *TryEatToken(SyntaxKind kind);
    void ForceEndOfFile();
    GreenNode *EatToken(SyntaxKind kind);
    GreenNode *EatTokenAsKind(SyntaxKind expected);
    GreenNode *CreateMissingToken(SyntaxKind expected, SyntaxKind actual);
    GreenNode *CreateMissingToken(SyntaxKind expected, ErrorCode code, bool reportError);
    GreenNode *EatToken(SyntaxKind kind, bool reportError);
    GreenNode *EatToken(SyntaxKind kind, ErrorCode code, bool reportError = true);
    GreenNode *EatTokenEvenWithIncorrectKind(SyntaxKind kind);
    GreenNode *EatTokenWithPrejudice(ErrorCode errorCode, std::initializer_list<Text> args = {});
    GreenNode *EatContextualToken(SyntaxKind kind, ErrorCode code);
    GreenNode *EatContextualToken(SyntaxKind kind);
    virtual SyntaxDiagnosticInfo GetExpectedTokenError(SyntaxKind expected, SyntaxKind actual, int offset, int width);
    virtual SyntaxDiagnosticInfo GetExpectedMissingNodeOrTokenError(GreenNode *missingNodeOrToken, SyntaxKind expected,
                                                                   SyntaxKind actual);
    static ErrorCode GetExpectedTokenErrorCode(SyntaxKind expected, SyntaxKind actual);

    /** @brief A copy of the node with `diagnostic` added (overridden by the doc-comment parser). */
    virtual GreenNode *WithAdditionalDiagnostics(GreenNode *node, const SyntaxDiagnosticInfo &diagnostic);
    GreenNode *AddError(GreenNode *node, ErrorCode code);
    GreenNode *AddError(GreenNode *nodeOrToken, ErrorCode code, std::initializer_list<Text> args);
    GreenNode *AddErrorAsWarning(GreenNode *node, ErrorCode code, std::initializer_list<Text> args = {});
    std::pair<int, int> GetDiagnosticSpanForMissingNodeOrToken(GreenNode *missingNodeOrToken);
    GreenNode *AddError(GreenNode *node, int offset, int length, ErrorCode code, std::initializer_list<Text> args = {});
    GreenNode *AddErrorToFirstToken(GreenNode *node, ErrorCode code, std::initializer_list<Text> args = {});
    GreenNode *AddErrorToLastToken(GreenNode *node, ErrorCode code);
    static void GetOffsetAndWidthForLastToken(GreenNode *node, int &offset, int &width);
    static SyntaxDiagnosticInfo MakeError(int offset, int width, ErrorCode code, std::initializer_list<Text> args = {});
    static SyntaxDiagnosticInfo MakeError(GreenNode *node, ErrorCode code, std::initializer_list<Text> args = {});
    static SyntaxDiagnosticInfo MakeError(ErrorCode code, std::initializer_list<Text> args = {});

    GreenNode *AddLeadingSkippedSyntax(GreenNode *node, GreenNode *skippedSyntax);
    void AddTrailingSkippedSyntax(SyntaxListBuilder list, GreenNode *skippedSyntax);
    GreenNode *AddTrailingSkippedSyntax(GreenNode *node, GreenNode *skippedSyntax);
    GreenNode *AddSkippedSyntax(GreenNode *target, GreenNode *skippedSyntax, bool trailing);
    static GreenNode *ConvertToKeyword(GreenNode *token);
    static GreenNode *ConvertToIdentifier(GreenNode *token);
    DirectiveStack Directives() const { return lexer->Directives(); }
    /** @brief Every feature is enabled (LanguageVersion.Preview): CheckFeatureAvailability returns the node. */
    static GreenNode *CheckFeatureAvailability(GreenNode *node, int /*feature*/ = 0, bool /*forceWarning*/ = false) {
        return node;
    }
    static bool IsFeatureEnabled(int /*feature*/ = 0) { return true; }
    bool IsMakingProgress(int &lastTokenPosition, bool assertIfFalse = true);
    int CurrentTokenPosition() const { return _firstToken + _tokenOffset; }

    /** @brief Helpers for diagnostic arguments: an integer, or a token's text. */
    static Text ArgInt(int64_t value);
    static Text ArgText(const std::u16string &s) { return CurrentArena()->CopyText(s); }
    /** @brief An enum value formatted as String.Format would (a SyntaxKind's name). */
    static Text ArgKind(SyntaxKind kind);

private:
    struct BlendedToken {
        GreenNode *Token = nullptr;
        // The Blender's state after this token (no old tree): where the lexer stood and what it had seen.
        int NewPosition = 0;
        DirectiveStack NewDirectives;
        LexerMode NewLexerDrivenMode = LexerMode::None;
    };
    struct BlenderState {
        int NewPosition = 0;
        DirectiveStack NewDirectives;
        LexerMode NewLexerDrivenMode = LexerMode::None;
    };

    BlendedToken ReadBlendedToken(const BlenderState &from, LexerMode mode);
    GreenNode *FetchCurrentToken();
    void AddNewToken();
    void AddLexedToken(GreenNode *token);
    void MoveToNextToken();

    bool _allowModeReset = false;
    LexerMode _mode = LexerMode::None;
    GreenNode *_currentToken = nullptr;
    std::vector<GreenNode *> _lexedTokens;
    std::vector<BlendedToken> _blendedTokens;  // used when _allowModeReset
    BlenderState _firstBlender;
    GreenNode *_prevTokenTrailingTrivia = nullptr;
    int _firstToken = 0;
    int _tokenOffset = 0;
    int _tokenCount = 0;
    int _resetCount = 0;
    int _resetStart = 0;
};

// --- LanguageParser (LanguageParser*.cs) ----------------------------------------------------------------

/** @brief Symbols.DeclarationModifiers. */
enum class DeclarationModifiers : uint32_t {
    None = 0,
    Abstract = 1u << 0,
    Sealed = 1u << 1,
    Static = 1u << 2,
    New = 1u << 3,
    Public = 1u << 4,
    Protected = 1u << 5,
    Internal = 1u << 6,
    ProtectedInternal = 1u << 7,
    Private = 1u << 8,
    PrivateProtected = 1u << 9,
    ReadOnly = 1u << 10,
    Const = 1u << 11,
    Volatile = 1u << 12,
    Extern = 1u << 13,
    Partial = 1u << 14,
    Unsafe = 1u << 15,
    Fixed = 1u << 16,
    Virtual = 1u << 17,
    Override = 1u << 18,
    Indexer = 1u << 19,
    Async = 1u << 20,
    Ref = 1u << 21,
    Required = 1u << 22,
    Scoped = 1u << 23,
    File = 1u << 24,
    All = (1u << 25) - 1,
    Unset = 1u << 25,
    AccessibilityMask = PrivateProtected | Private | Protected | Internal | ProtectedInternal | Public,
};
CS_FLAGS_ENUM(DeclarationModifiers)

class LanguageParser : public SyntaxParser {
public:
    explicit LanguageParser(Lexer *lexer, LexerMode lexerMode = LexerMode::Syntax);
    ~LanguageParser() override;

    GreenNode *ParseCompilationUnit();

#include "csharp_parser_types.inc"  // the nested enums and structs (TerminatorState, ParseTypeMode, ...)

    // LanguageParser's own ResetPoint (base point plus the parser state it restores); it hides
    // SyntaxParser::ResetPoint inside LanguageParser, as the C# `new struct ResetPoint` does.
    struct ResetPoint {
        SyntaxParser::ResetPoint BaseResetPoint;
        TerminatorState TerminatorState_ = TerminatorState::EndOfFile;
        bool IsInAsync = false;
        bool IsInQuery = false;
        bool IsInFieldKeywordContext = false;
    };
    // The members, by translation unit (each .inc declares what its .cpp defines):
#include "csharp_parser_members_a.inc"  // LanguageParser.cs: compilation unit .. type declarations
#include "csharp_parser_members_b.inc"  // LanguageParser.cs: members, accessors, parameters
#include "csharp_parser_members_c.inc"  // LanguageParser.cs: fields, variables, names and types
#include "csharp_parser_members_d.inc"  // LanguageParser.cs: statements
#include "csharp_parser_members_e.inc"  // LanguageParser.cs: expressions
#include "csharp_parser_members_f.inc"  // LanguageParser.cs: lambdas .. queries; _Patterns.cs; _InterpolatedString.cs

    // State (LanguageParser.cs, SyntaxFactoryContext.cs).
    SyntaxListPool _pool;
    int _recursionDepth = 0;
    TerminatorState _termState = TerminatorState::EndOfFile;
    struct SyntaxFactoryContext {
        bool IsInAsync = false;
        bool ForceConditionalAccessExpression = false;
        bool IsInQuery = false;
        bool IsInFieldKeywordContext = false;
    } _syntaxFactoryContext;
    bool IsInAsync() const { return _syntaxFactoryContext.IsInAsync; }
    void SetIsInAsync(bool v) { _syntaxFactoryContext.IsInAsync = v; }
    bool IsInQuery() const { return _syntaxFactoryContext.IsInQuery; }
    void SetIsInQuery(bool v) { _syntaxFactoryContext.IsInQuery = v; }
    bool IsInFieldKeywordContext() const { return _syntaxFactoryContext.IsInFieldKeywordContext; }
    void SetIsInFieldKeywordContext(bool v) { _syntaxFactoryContext.IsInFieldKeywordContext = v; }
    bool ForceConditionalAccessExpression() const { return _syntaxFactoryContext.ForceConditionalAccessExpression; }
    void SetForceConditionalAccessExpression(bool v) { _syntaxFactoryContext.ForceConditionalAccessExpression = v; }

    ResetPoint GetResetPoint();
    void Reset(ResetPoint &state);
    void Release(ResetPoint &state);

    /** @brief `using var rp = GetDisposableResetPoint(resetOnDispose)`: resets (if asked) and releases at scope end. */
    class DisposableResetPoint {
    public:
        DisposableResetPoint(LanguageParser *parser, bool resetOnDispose)
            : _languageParser(parser), _resetOnDispose(resetOnDispose), _resetPoint(parser->GetResetPoint()) {}
        ~DisposableResetPoint() {
            if (_resetOnDispose) Reset();
            _languageParser->Release(_resetPoint);
        }
        DisposableResetPoint(const DisposableResetPoint &) = delete;
        DisposableResetPoint &operator=(const DisposableResetPoint &) = delete;
        void Reset() { _languageParser->Reset(_resetPoint); }

    private:
        LanguageParser *_languageParser;
        bool _resetOnDispose;
        ResetPoint _resetPoint;
    };

    /** @brief `using var _ = new ParserSyntaxContextResetter(this, isInAsyncContext: ..., ...)`. */
    class ParserSyntaxContextResetter {
    public:
        ParserSyntaxContextResetter(LanguageParser *parser, std::optional<bool> isInAsyncContext = std::nullopt,
                                    std::optional<bool> isInQueryContext = std::nullopt,
                                    std::optional<bool> isInFieldKeywordContext = std::nullopt,
                                    std::optional<bool> forceConditionalAccessExpression = std::nullopt)
            : _parser(parser), _saved(parser->_syntaxFactoryContext) {
            if (isInAsyncContext) parser->_syntaxFactoryContext.IsInAsync = *isInAsyncContext;
            if (isInQueryContext) parser->_syntaxFactoryContext.IsInQuery = *isInQueryContext;
            if (isInFieldKeywordContext) parser->_syntaxFactoryContext.IsInFieldKeywordContext = *isInFieldKeywordContext;
            if (forceConditionalAccessExpression) {
                parser->_syntaxFactoryContext.ForceConditionalAccessExpression = *forceConditionalAccessExpression;
            }
        }
        ~ParserSyntaxContextResetter() { _parser->_syntaxFactoryContext = _saved; }
        ParserSyntaxContextResetter(const ParserSyntaxContextResetter &) = delete;
        ParserSyntaxContextResetter &operator=(const ParserSyntaxContextResetter &) = delete;

    private:
        LanguageParser *_parser;
        SyntaxFactoryContext _saved;
    };
};

// --- DirectiveParser (DirectiveParser.cs) ---------------------------------------------------------------

class DirectiveParser : public SyntaxParser {
public:
    explicit DirectiveParser(Lexer *lexer);
    void ReInitialize(DirectiveStack context);
    GreenNode *ParseDirective(bool isActive, bool endIsActive, bool isFollowingToken);

#include "csharp_directive_parser_members.inc"

    DirectiveStack _context;
};

// --- DocumentationCommentParser (DocumentationCommentParser.cs) ------------------------------------------

class DocumentationCommentParser : public SyntaxParser {
public:
    explicit DocumentationCommentParser(Lexer *lexer);
    void ReInitialize(LexerMode modeflags);
    GreenNode *ParseDocumentationComment(bool &isTerminated);

#include "csharp_doc_parser_members.inc"

    SyntaxListPool _pool;
    bool _isDelimited = false;
};

}  // namespace csharp

#endif  // MEP_CSHARP_PARSER_INTERNAL_H
