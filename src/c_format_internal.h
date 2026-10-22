#pragma once

// Internals of mep's C formatter (c_format.h). Not part of its API.
//
// The formatter is a port, restricted to C, of clang-format's pipeline: the
// lexer (FormatTokenLexer), the parser that splits a file into unwrapped
// lines (UnwrappedLineParser), the token annotator (TokenAnnotator), the
// line-breaking search (ContinuationIndenter, UnwrappedLineFormatter), comment
// and string breaking (BreakableToken) and the whitespace pass that aligns
// trailing comments and macro backslashes (WhitespaceManager). Its rules,
// penalties and heuristics follow clang-format 21's, read in C mode (what
// clang-format uses for a .c file: C11/C23 keywords, no C++ keywords), so the
// output matches `clang-format --style=LLVM` as closely as a port can.
//
// clang-format is part of the LLVM project, licensed under the Apache License
// v2.0 with LLVM Exceptions (https://llvm.org/LICENSE.txt); this port keeps
// its structure and names so the two can be read side by side.
//
// Everything here works on byte offsets into the one source buffer the
// formatter was given; tokens point into it and are never copied.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cfmt::detail {

// --- Token kinds ----------------------------------------------------------------
//
// clang's tok::TokenKind, for the tokens a C file can contain. Keywords the
// port never tests by name are lexed as kw_other: still keywords (they have
// "identifier info" and are word-like), but not identifiers.
enum class tok : uint16_t {
    unknown,
    eof,
    comment,
    identifier,
    numeric_constant,
    char_constant,
    wide_char_constant,
    utf8_char_constant,
    utf16_char_constant,
    utf32_char_constant,
    string_literal,
    wide_string_literal,
    utf8_string_literal,
    utf16_string_literal,
    utf32_string_literal,
    // Punctuators.
    l_square,
    r_square,
    l_paren,
    r_paren,
    l_brace,
    r_brace,
    period,
    ellipsis,
    amp,
    ampamp,
    ampequal,
    star,
    starequal,
    plus,
    plusplus,
    plusequal,
    minus,
    arrow,
    minusminus,
    minusequal,
    tilde,
    exclaim,
    exclaimequal,
    slash,
    slashequal,
    percent,
    percentequal,
    less,
    lessless,
    lessequal,
    lesslessequal,
    greater,
    greatergreater,
    greaterequal,
    greatergreaterequal,
    caret,
    caretequal,
    pipe,
    pipepipe,
    pipeequal,
    question,
    colon,
    coloncolon,
    semi,
    equal,
    equalequal,
    comma,
    hash,
    hashhash,
    hashat,
    at,
    // Keywords tested by name.
    kw_auto,
    kw_break,
    kw_case,
    kw_char,
    kw_const,
    kw_continue,
    kw_default,
    kw_do,
    kw_double,
    kw_else,
    kw_enum,
    kw_extern,
    kw_float,
    kw_for,
    kw_goto,
    kw_if,
    kw_int,
    kw_long,
    kw_register,
    kw_return,
    kw_short,
    kw_signed,
    kw_sizeof,
    kw_static,
    kw_struct,
    kw_switch,
    kw_typedef,
    kw_union,
    kw_unsigned,
    kw_void,
    kw_volatile,
    kw_while,
    kw__Atomic,
    kw__Bool,
    kw__Generic,
    kw_alignas,
    kw_alignof,
    kw_asm,
    kw_bool,
    kw_true,
    kw_false,
    kw_restrict,
    kw_inline,
    kw_constexpr,
    kw_static_assert,
    kw_typeof,
    kw_char8_t,
    kw_wchar_t,
    kw_half,
    kw___bf16,
    kw__Float16,
    kw___float128,
    kw___ibm128,
    kw___int64,
    kw___int128,
    kw___auto_type,
    kw___attribute,
    kw___declspec,
    kw___try,
    kw___finally,
    kw___cdecl,
    kw___stdcall,
    kw___fastcall,
    kw___thiscall,
    kw___regcall,
    kw___vectorcall,
    kw__Nonnull,
    kw__Nullable,
    kw__Null_unspecified,
    kw___ptr32,
    kw___ptr64,
    kw___funcref,
    kw_other,
};

// Preprocessor keyword of an identifier or keyword token's spelling (clang's
// IdentifierInfo::getPPKeywordID).
enum class pp : uint8_t {
    none,
    if_,
    ifdef,
    ifndef,
    elif,
    elifdef,
    elifndef,
    else_,
    endif,
    defined,
    include,
    include_next,
    import,
    define,
    undef,
    line,
    error,
    warning,
    pragma,
    other,
};

// clang::prec::Level.
namespace prec {
enum Level : int {
    Unknown = 0,
    Comma = 1,
    Assignment = 2,
    Conditional = 3,
    LogicalOr = 4,
    LogicalAnd = 5,
    InclusiveOr = 6,
    ExclusiveOr = 7,
    And = 8,
    Equality = 9,
    Relational = 10,
    Spaceship = 11,
    Shift = 12,
    Additive = 13,
    Multiplicative = 14,
    PointerToMember = 15,
};
}  // namespace prec

// The token types (TT_*) the C port can assign.
enum class TT : uint8_t {
    Unknown,
    AfterPPDirective,
    ArrayInitializerLSquare,
    ArraySubscriptLSquare,
    AttributeColon,
    AttributeLParen,
    AttributeMacro,
    AttributeRParen,
    AttributeSquare,
    BinaryOperator,
    BitFieldColon,
    BlockComment,
    BlockLBrace,
    BracedListLBrace,
    CaseLabelColon,
    CastRParen,
    ClassHeadName,
    ConditionLParen,
    ConditionalExpr,
    ControlStatementLBrace,
    ControlStatementRBrace,
    CtorDtorDeclName,
    CtorInitializerColon,
    CtorInitializerComma,
    DesignatedInitializerLSquare,
    DesignatedInitializerPeriod,
    DictLiteral,
    DoWhile,
    ElseLBrace,
    ElseRBrace,
    EnumLBrace,
    EnumRBrace,
    ForEachMacro,
    FunctionAnnotationRParen,
    FunctionDeclarationLParen,
    FunctionDeclarationName,
    FunctionLBrace,
    FunctionLikeOrFreestandingMacro,
    FunctionTypeLParen,
    GenericSelectionColon,
    GotoLabelColon,
    IfMacro,
    ImplicitStringLiteral,
    InheritanceColon,
    InheritanceComma,
    InlineASMBrace,
    InlineASMColon,
    InlineASMSymbolicNameLSquare,
    LambdaArrow,
    LambdaDefinitionLParen,
    LambdaLBrace,
    LambdaLSquare,
    LineComment,
    ObjCBlockLBrace,
    ObjCBlockLParen,
    ObjCForIn,
    ObjCMethodExpr,
    ObjCMethodSpecifier,
    PointerOrReference,
    RangeBasedForLoopColon,
    RecordLBrace,
    RecordRBrace,
    SelectorName,
    StartOfName,
    StatementAttributeLikeMacro,
    StatementMacro,
    StructLBrace,
    StructRBrace,
    StructuredBindingLSquare,
    TemplateCloser,
    TemplateOpener,
    TrailingAnnotation,
    TrailingReturnArrow,
    TrailingUnaryOperator,
    TypeDeclarationParen,
    UnaryOperator,
    UnionLBrace,
    UnionRBrace,
    UntouchableMacroFunc,
};

enum BraceBlockKind : uint8_t { BK_Unknown, BK_Block, BK_BracedInit };
enum ParameterPackingKind : uint8_t { PPK_BinPacked, PPK_OnePerLine, PPK_Inconclusive };
enum FormatDecision : uint8_t { FD_Unformatted, FD_Continue, FD_Break };

// --- Style ---------------------------------------------------------------------------
//
// The options the port reads, at clang-format's LLVM-style values. Only the
// ones a C file is affected by are here; the rest of LLVM style is built into
// the code as constants (C has no access modifiers, templates, lambdas...).
struct Style {
    unsigned ColumnLimit = 80;
    unsigned IndentWidth = 2;
    unsigned ContinuationIndentWidth = 4;
    unsigned TabWidth = 8;
    enum UseTabStyle { UT_Never, UT_ForIndentation, UT_ForContinuationAndIndentation, UT_AlignWithSpaces, UT_Always };
    UseTabStyle UseTab = UT_Never;
    unsigned MaxEmptyLinesToKeep = 1;
    bool KeepEmptyLinesAtStartOfBlock = true;
    bool IndentCaseLabels = false;
    bool IndentGotoLabels = true;
    bool AllowShortEnumsOnASingleLine = true;
    enum ShortFunctionStyle { SFS_None, SFS_InlineOnly, SFS_Empty, SFS_Inline, SFS_All };
    ShortFunctionStyle AllowShortFunctionsOnASingleLine = SFS_All;
    bool AllowShortIfStatementsOnASingleLine = false;
    bool AllowShortLoopsOnASingleLine = false;
    bool AllowShortCaseLabelsOnASingleLine = false;
    enum ShortBlockStyle { SBS_Never, SBS_Empty, SBS_Always };
    ShortBlockStyle AllowShortBlocksOnASingleLine = SBS_Never;
    // Brace wrapping (BS_Attach: nothing wraps).
    bool BraceWrappingAfterFunction = false;
    bool BraceWrappingAfterStruct = false;
    bool BraceWrappingAfterUnion = false;
    bool BraceWrappingAfterEnum = false;
    bool BraceWrappingAfterControlStatement = false;  // BWACS_Never
    bool BraceWrappingBeforeElse = false;
    bool BraceWrappingBeforeWhile = false;
    bool BraceWrappingAfterExternBlock = false;
    bool BraceWrappingAfterCaseLabel = false;
    bool BraceWrappingSplitEmptyFunction = true;
    bool BraceWrappingSplitEmptyRecord = true;
    enum PointerAlignmentStyle { PAS_Left, PAS_Right, PAS_Middle };
    PointerAlignmentStyle PointerAlignment = PAS_Right;
    bool SpaceAfterCStyleCast = false;
    bool SpaceBeforeAssignmentOperators = true;
    enum SpaceBeforeParensStyle { SBPO_Never, SBPO_ControlStatements, SBPO_Always };
    SpaceBeforeParensStyle SpaceBeforeParens = SBPO_ControlStatements;
    unsigned SpacesBeforeTrailingComments = 1;
    bool AlignTrailingComments = true;
    bool AlignEscapedNewlinesRight = true;  // ENAS_Right; false is ENAS_Left
    bool AlignOperands = true;              // OAS_Align
    bool BinPackArguments = true;
    bool BinPackParameters = true;
    bool AllowAllArgumentsOnNextLine = true;
    bool AllowAllParametersOfDeclarationOnNextLine = true;
    bool BreakBeforeTernaryOperators = true;
    bool BreakStringLiterals = true;
    bool ReflowComments = true;
    bool BreakAdjacentStringLiterals = true;
    unsigned PenaltyBreakAssignment = prec::Assignment;
    unsigned PenaltyBreakBeforeFirstCallParameter = 19;
    unsigned PenaltyBreakBeforeMemberAccess = 150;
    unsigned PenaltyBreakComment = 300;
    unsigned PenaltyBreakFirstLessLess = 120;
    unsigned PenaltyBreakOpenParenthesis = 0;
    unsigned PenaltyBreakScopeResolution = 500;
    unsigned PenaltyBreakString = 1000;
    unsigned PenaltyBreakTemplateDeclaration = prec::Relational;
    unsigned PenaltyExcessCharacter = 1000000;
    unsigned PenaltyIndentedWhitespace = 0;
    unsigned PenaltyReturnTypeOnItsOwnLine = 60;
};

// --- Tokens -------------------------------------------------------------------------

struct AnnotatedLine;
struct LineState;
class ContinuationIndenter;
struct FormatToken;

// A token's special role in formatting: the comma-separated list behind a
// `{` or `(` (FormatToken.h's TokenRole / CommaSeparatedList).
class TokenRole {
public:
    virtual ~TokenRole() = default;
    virtual void precomputeFormattingInfos(const FormatToken *) {}
    virtual unsigned formatFromToken(LineState &, ContinuationIndenter *, bool) { return 0; }
    virtual unsigned formatAfterToken(LineState &, ContinuationIndenter *, bool) { return 0; }
    virtual void CommaFound(const FormatToken *) {}
    virtual const FormatToken *lastComma() { return nullptr; }
};

struct FormatToken {
    // The token as lexed.
    tok Kind = tok::unknown;
    pp PPKind = pp::none;
    bool HasIdentifierInfo = false;  // an identifier or a keyword
    std::string_view TokenText;
    // The whitespace before the token: [WhitespaceStart, WhitespaceEnd) in the
    // source. Tokens split out of one source token (`>>`) have an empty range.
    size_t WhitespaceStart = 0;
    size_t WhitespaceEnd = 0;

    bool HasUnescapedNewline = false;
    bool IsMultiline = false;
    bool IsFirst = false;
    bool MustBreakBefore = false;
    bool MustBreakBeforeFinalized = false;
    bool IsUnterminatedLiteral = false;
    bool CanBreakBefore = false;
    bool ClosesTemplateDeclaration = false;
    bool StartsBinaryExpression = false;
    bool EndsBinaryExpression = false;
    bool PartOfMultiVariableDeclStmt = false;
    bool ContinuesLineCommentSection = false;
    bool Finalized = false;
    bool EndsCppAttributeGroup = false;
    BraceBlockKind BlockKind = BK_Unknown;
    FormatDecision Decision = FD_Unformatted;
    ParameterPackingKind PackingKind = PPK_Inconclusive;
    bool TypeIsFinalized = false;
    TT Type = TT::Unknown;

    unsigned NewlinesBefore = 0;
    int Newlines = -1;
    unsigned LastNewlineOffset = 0;
    unsigned ColumnWidth = 0;
    unsigned LastLineColumnWidth = 0;
    unsigned SpacesRequiredBefore = 0;
    unsigned ParameterCount = 0;
    unsigned BlockParameterCount = 0;
    tok ParentBracket = tok::unknown;
    unsigned TotalLength = 0;
    unsigned OriginalColumn = 0;
    unsigned UnbreakableTailLength = 0;
    unsigned BindingStrength = 0;
    unsigned NestingLevel = 0;
    unsigned IndentLevel = 0;
    unsigned SplitPenalty = 0;
    unsigned LongestObjCSelectorName = 0;
    unsigned ObjCSelectorNameParts = 0;
    unsigned ParameterIndex = 0;
    std::vector<prec::Level> FakeLParens;
    unsigned FakeRParens = 0;
    unsigned OperatorIndex = 0;
    FormatToken *NextOperator = nullptr;
    FormatToken *MatchingParen = nullptr;
    FormatToken *Previous = nullptr;
    FormatToken *Next = nullptr;
    bool StartsColumn = false;
    bool Optional = false;
    bool MightBeFunctionDeclParen = false;
    bool FirstAfterPPLine = false;
    std::vector<AnnotatedLine *> Children;
    std::shared_ptr<TokenRole> Role;

    // --- Predicates (FormatToken.h) ---
    bool is(tok K) const { return Kind == K; }
    bool is(TT T) const { return Type == T; }
    bool is(pp P) const { return HasIdentifierInfo && PPKind == P; }
    bool is(BraceBlockKind B) const { return BlockKind == B; }
    bool is(ParameterPackingKind P) const { return PackingKind == P; }
    template <typename A> bool isOneOf(A K) const { return is(K); }
    template <typename A, typename... Ts> bool isOneOf(A K, Ts... Ks) const { return is(K) || isOneOf(Ks...); }
    template <typename T> bool isNot(T K) const { return !is(K); }

    void setType(TT T) { Type = T; }
    void setFinalizedType(TT T) {
        Type = T;
        TypeIsFinalized = true;
    }
    void overwriteFixedType(TT T) {
        TypeIsFinalized = false;
        Type = T;
    }
    bool isTypeFinalized() const { return TypeIsFinalized; }

    bool hasWhitespaceBefore() const { return WhitespaceStart != WhitespaceEnd; }
    bool isLiteral() const;
    bool isStringLiteral() const;
    bool isAnyIdentifier() const { return Kind == tok::identifier; }
    bool isIf() const;
    bool closesScopeAfterBlock() const;
    bool isAttribute() const { return isOneOf(tok::kw___attribute, tok::kw___declspec, TT::AttributeMacro); }
    bool canBePointerOrReferenceQualifier() const;
    bool isSimpleTypeSpecifier() const;
    bool isTypeName() const;
    bool isTypeOrIdentifier() const { return isTypeName() || isOneOf(tok::kw_auto, tok::identifier); }
    bool opensScope() const { return isOneOf(tok::l_paren, tok::l_brace, tok::l_square, TT::TemplateOpener); }
    bool closesScope() const { return isOneOf(tok::r_paren, tok::r_brace, tok::r_square, TT::TemplateCloser); }
    bool isMemberAccess() const {
        return isOneOf(tok::arrow, tok::period) &&
               !isOneOf(TT::DesignatedInitializerPeriod, TT::TrailingReturnArrow, TT::LambdaArrow);
    }
    bool isPointerOrReference() const { return isOneOf(tok::star, tok::amp, tok::ampamp); }
    bool isUnaryOperator() const {
        return isOneOf(tok::plus, tok::plusplus, tok::minus, tok::minusminus, tok::exclaim, tok::tilde,
                       tok::kw_sizeof, tok::kw_alignof);
    }
    prec::Level getPrecedence() const;
    bool isBinaryOperator() const { return getPrecedence() > prec::Comma; }
    bool isTrailingComment() const {
        return is(tok::comment) && (is(TT::LineComment) || !Next || Next->NewlinesBefore > 0);
    }
    bool isFunctionLikeKeyword() const;
    bool isLabelString() const;
    FormatToken *getPreviousNonComment() const {
        FormatToken *T = Previous;
        while (T && T->is(tok::comment)) T = T->Previous;
        return T;
    }
    FormatToken *getNextNonComment() const {
        FormatToken *T = Next;
        while (T && T->is(tok::comment)) T = T->Next;
        return T;
    }
    bool opensBlockOrBlockTypeList() const;
    bool closesBlockOrBlockTypeList() const { return MatchingParen && MatchingParen->opensBlockOrBlockTypeList(); }

    template <typename A> bool startsSequence(A K1) const {
        if (is(tok::comment) && Next) return Next->startsSequence(K1);
        return is(K1);
    }
    template <typename A, typename... Ts> bool startsSequence(A K1, Ts... Tokens) const {
        if (is(tok::comment) && Next) return Next->startsSequence(K1, Tokens...);
        return is(K1) && Next && Next->startsSequence(Tokens...);
    }
    template <typename A> bool endsSequence(A K1) const {
        if (is(tok::comment) && Previous) return Previous->endsSequence(K1);
        return is(K1);
    }
    template <typename A, typename... Ts> bool endsSequence(A K1, Ts... Tokens) const {
        if (is(tok::comment) && Previous) return Previous->endsSequence(K1, Tokens...);
        return is(K1) && Previous && Previous->endsSequence(Tokens...);
    }
};

bool isLineComment(const FormatToken &T);
bool continuesLineComment(const FormatToken &FormatTok, const FormatToken *Previous, const FormatToken *MinColumnToken);
bool startsNextParameter(const FormatToken &Current);
bool switchesFormatting(const FormatToken &Token);

// --- Lines ---------------------------------------------------------------------------

struct UnwrappedLine;
struct UnwrappedLineNode {
    explicit UnwrappedLineNode(FormatToken *T = nullptr);
    FormatToken *Tok;
    std::vector<UnwrappedLine> Children;
};

struct UnwrappedLine {
    std::vector<UnwrappedLineNode> Tokens;
    unsigned Level = 0;
    unsigned PPLevel = 0;
    bool InPPDirective = false;
    bool InPragmaDirective = false;
    bool InMacroBody = false;
    unsigned UnbracedBodyLevel = 0;
    bool MustBeDeclaration = false;
    bool IsContinuation = false;
    static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);
    size_t MatchingOpeningBlockLineIndex = kInvalidIndex;
    size_t MatchingClosingBlockLineIndex = kInvalidIndex;
};

inline UnwrappedLineNode::UnwrappedLineNode(FormatToken *T) : Tok(T) {}

enum LineType {
    LT_Invalid,
    LT_ImportStatement,
    LT_ObjCMethodDecl,
    LT_Other,
    LT_PreprocessorDirective,
    LT_CommentAbovePPDirective,
};

enum ScopeType { ST_Class, ST_Other };

struct AnnotatedLine {
    explicit AnnotatedLine(const UnwrappedLine &Line);
    ~AnnotatedLine();
    AnnotatedLine(const AnnotatedLine &) = delete;
    AnnotatedLine &operator=(const AnnotatedLine &) = delete;

    bool isComment() const { return First && First->is(tok::comment) && !First->getNextNonComment(); }
    template <typename... Ts> bool startsWith(Ts... Tokens) const { return First && First->startsSequence(Tokens...); }
    template <typename... Ts> bool endsWith(Ts... Tokens) const { return Last && Last->endsSequence(Tokens...); }
    bool mightBeFunctionDefinition() const { return !endsWith(tok::semi); }
    FormatToken *getFirstNonComment() const { return First->is(tok::comment) ? First->getNextNonComment() : First; }
    FormatToken *getLastNonComment() const { return Last->is(tok::comment) ? Last->getPreviousNonComment() : Last; }

    FormatToken *First = nullptr;
    FormatToken *Last = nullptr;
    std::vector<AnnotatedLine *> Children;
    LineType Type = LT_Other;
    unsigned Level = 0;
    unsigned PPLevel = 0;
    size_t MatchingOpeningBlockLineIndex = UnwrappedLine::kInvalidIndex;
    size_t MatchingClosingBlockLineIndex = UnwrappedLine::kInvalidIndex;
    bool InPPDirective = false;
    bool InPragmaDirective = false;
    bool InMacroBody = false;
    bool MustBeDeclaration = false;
    bool MightBeFunctionDecl = false;
    bool IsMultiVariableDeclStmt = false;
    bool Computed = false;
    bool ReturnTypeWrapped = false;
    bool IsContinuation = false;
    unsigned FirstStartColumn = 0;
};

struct Style;
std::shared_ptr<TokenRole> makeCommaSeparatedList(const Style &S);

// --- The stages ----------------------------------------------------------------

// The source being formatted, and every token lexed from it.
struct Source {
    std::string_view Text;
    std::deque<FormatToken> Arena;  // stable addresses
    std::vector<FormatToken *> Tokens;  // in order, ending with eof
    FormatToken *make() {
        Arena.emplace_back();
        return &Arena.back();
    }
};

/** @brief Lexes the whole source into Source::Tokens (FormatTokenLexer::lex). */
void Lex(Source &Src, const Style &S);

/** @brief Parses the tokens into runs of unwrapped lines, one run per #if branch combination (UnwrappedLineParser::parse). */
// Called with the lines of each run (one combination of #if branches) in
// turn, and the number of runs the branch counts found so far predict;
// returning false stops the parse.
using RunCallback = std::function<bool(std::vector<UnwrappedLine> &Lines, size_t PredictedRuns)>;
void ParseUnwrappedLines(Source &Src, const Style &S, const RunCallback &OnRun);

class TokenAnnotator {
public:
    explicit TokenAnnotator(const Style &S) : Style_(S) {}
    void setCommentLineLevels(std::vector<AnnotatedLine *> &Lines) const;
    void annotate(AnnotatedLine &Line);
    void calculateFormattingInformation(AnnotatedLine &Line) const;

private:
    unsigned splitPenalty(const AnnotatedLine &Line, const FormatToken &Tok, bool InFunctionDecl) const;
    bool spaceRequiredBeforeParens(const FormatToken &Right) const;
    bool spaceRequiredBetween(const AnnotatedLine &Line, const FormatToken &Left, const FormatToken &Right) const;
    bool spaceRequiredBefore(const AnnotatedLine &Line, const FormatToken &Right) const;
    bool mustBreakBefore(const AnnotatedLine &Line, const FormatToken &Right) const;
    bool canBreakBefore(const AnnotatedLine &Line, const FormatToken &Right) const;
    void calculateUnbreakableTailLengths(AnnotatedLine &Line) const;

    const Style &Style_;
    std::vector<ScopeType> Scopes;
    std::vector<ScopeType> MacroBodyScopes;
};

// A whitespace (or in-token) replacement, WhitespaceManager::Change.
struct Change {
    const FormatToken *Tok = nullptr;
    bool CreateReplacement = false;
    size_t OriginalStart = 0;  // the range replaced
    size_t OriginalEnd = 0;
    unsigned StartOfTokenColumn = 0;
    unsigned NewlinesBefore = 0;
    std::string PreviousLinePostfix;
    std::string CurrentLinePrefix;
    bool IsAligned = false;
    bool ContinuesPPDirective = false;
    int Spaces = 0;
    bool IsInsideToken = false;
    bool IsTrailingComment = false;
    unsigned TokenLength = 0;
    unsigned PreviousEndOfTokenColumn = 0;
    unsigned EscapedNewlineColumn = 0;
    const Change *StartOfBlockComment = nullptr;
    int IndentationOffset = 0;
    int ConditionalsLevel = 0;
};

// A replacement of a source range with new text.
struct Replacement {
    size_t Offset = 0;
    size_t Length = 0;
    std::string Text;
};

class WhitespaceManager {
public:
    WhitespaceManager(const Source &Src, const Style &S, bool UseCRLF) : Src_(Src), Style_(S), UseCRLF_(UseCRLF) {}
    void replaceWhitespace(FormatToken &Tok, unsigned Newlines, unsigned Spaces, unsigned StartOfTokenColumn,
                           bool IsAligned = false, bool InPPDirective = false);
    void addUntouchableToken(const FormatToken &Tok, bool InPPDirective);
    void replaceWhitespaceInToken(const FormatToken &Tok, unsigned Offset, unsigned ReplaceChars,
                                  std::string_view PreviousPostfix, std::string_view CurrentPrefix,
                                  bool InPPDirective, unsigned Newlines, int Spaces);
    bool useCRLF() const { return UseCRLF_; }
    std::vector<Replacement> generateReplacements();

private:
    void calculateLineBreakInformation();
    void alignChainedConditionals();
    void alignTrailingComments();
    void alignTrailingComments(unsigned Start, unsigned End, unsigned Column);
    void alignEscapedNewlines();
    void alignEscapedNewlines(unsigned Start, unsigned End, unsigned Column);
    void generateChanges(std::vector<Replacement> &Out);
    void appendNewlineText(std::string &Text, const Change &C) const;
    void appendEscapedNewlineText(std::string &Text, unsigned Newlines, unsigned PreviousEndOfTokenColumn,
                                  unsigned EscapedNewlineColumn) const;
    void appendIndentText(std::string &Text, unsigned IndentLevel, unsigned Spaces, unsigned WhitespaceStartColumn,
                          bool IsAligned) const;
    unsigned appendTabIndent(std::string &Text, unsigned Spaces, unsigned Indentation) const;

    const Source &Src_;
    const Style &Style_;
    bool UseCRLF_;
    std::vector<Change> Changes;
};

// --- Continuation indenter state (ContinuationIndenter.h) ------------------------

struct ParenState {
    ParenState(const FormatToken *Tok, unsigned Indent, unsigned LastSpace, bool AvoidBinPacking, bool NoLineBreak)
        : Tok(Tok), Indent(Indent), LastSpace(LastSpace), NestedBlockIndent(Indent), AvoidBinPacking(AvoidBinPacking),
          NoLineBreak(NoLineBreak) {}
    const FormatToken *Tok;
    unsigned Indent;
    unsigned LastSpace;
    unsigned NestedBlockIndent;
    unsigned FirstLessLess = 0;
    unsigned QuestionColumn = 0;
    unsigned ColonPos = 0;
    unsigned StartOfFunctionCall = 0;
    unsigned StartOfArraySubscripts = 0;
    unsigned CallContinuation = 0;
    unsigned VariablePos = 0;
    bool IsAligned = false;
    bool BreakBeforeClosingBrace = false;
    bool AvoidBinPacking;
    bool BreakBeforeParameter = false;
    bool NoLineBreak;
    bool NoLineBreakInOperand = false;
    bool LastOperatorWrapped = true;
    bool ContainsLineBreak = false;
    bool ContainsUnwrappedBuilder = false;
    bool AlignColons = true;
    bool ObjCSelectorNameFound = false;
    bool HasMultipleNestedBlocks = false;
    bool NestedBlockInlined = false;
    bool IsChainedConditional = false;
    bool IsWrappedConditional = false;
    bool UnindentOperator = false;

    bool operator<(const ParenState &Other) const;
};

struct LineState {
    unsigned Column = 0;
    FormatToken *NextToken = nullptr;
    bool NoContinuation = false;
    unsigned StartOfLineLevel = 0;
    unsigned LowestLevelOnLine = 0;
    unsigned StartOfStringLiteral = 0;
    bool NoLineBreak = false;
    std::vector<ParenState> Stack;
    bool IgnoreStackForComparison = false;
    unsigned FirstIndent = 0;
    const AnnotatedLine *Line = nullptr;

    bool operator<(const LineState &Other) const;
};

class ContinuationIndenter {
public:
    ContinuationIndenter(const Source &Src, const Style &S, WhitespaceManager &Whitespaces)
        : Src(Src), S(S), Whitespaces(Whitespaces) {}
    LineState getInitialState(unsigned FirstIndent, unsigned FirstStartColumn, const AnnotatedLine *Line, bool DryRun);
    bool canBreak(const LineState &State);
    bool mustBreak(const LineState &State);
    unsigned addTokenToState(LineState &State, bool Newline, bool DryRun, unsigned ExtraSpaces = 0);
    unsigned getColumnLimit(const LineState &State) const;
    const Style &style() const { return S; }

private:
    unsigned moveStateToNextToken(LineState &State, bool DryRun, bool Newline);
    void moveStatePastFakeLParens(LineState &State, bool Newline);
    void moveStatePastFakeRParens(LineState &State);
    void moveStatePastScopeOpener(LineState &State, bool Newline);
    void moveStatePastScopeCloser(LineState &State);
    void moveStateToNewBlock(LineState &State, bool NewLine);
    unsigned handleEndOfLine(const FormatToken &Current, LineState &State, bool DryRun, bool AllowBreak, bool Newline);
    std::pair<unsigned, bool> breakProtrudingToken(const FormatToken &Current, LineState &State, bool AllowBreak,
                                                   bool DryRun, bool Strict);
    void addTokenOnCurrentLine(LineState &State, bool DryRun, unsigned ExtraSpaces);
    unsigned addTokenOnNewLine(LineState &State, bool DryRun);
    unsigned getNewLineColumn(const LineState &State);
    unsigned addMultilineToken(const FormatToken &Current, LineState &State);
    unsigned spellingColumn(size_t Offset) const;

    const Source &Src;
    const Style &S;
    WhitespaceManager &Whitespaces;
};

/** @brief Formats one run's annotated lines, recording whitespace changes (UnwrappedLineFormatter::format). */
unsigned FormatLines(const std::vector<AnnotatedLine *> &Lines, const Source &Src, const Style &S,
                     WhitespaceManager &Whitespaces);

// --- Shared helpers -----------------------------------------------------------------

/** @brief Display width of `Text` starting at `StartColumn`, tabs expanded (encoding::columnWidthWithTabs). */
unsigned columnWidthWithTabs(std::string_view Text, unsigned StartColumn, unsigned TabWidth);
/** @brief Display width of `Text` (encoding::columnWidth). */
unsigned columnWidth(std::string_view Text);
/** @brief Number of bytes of the UTF-8 code point that starts with `FirstChar`. */
unsigned getCodePointNumBytes(char FirstChar);
/** @brief Length of the escape sequence at the start of `Text` (encoding::getEscapeSequenceLength). */
unsigned getEscapeSequenceLength(std::string_view Text);

}  // namespace cfmt::detail
