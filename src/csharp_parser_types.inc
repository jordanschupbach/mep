// LanguageParser's nested types (LanguageParser.cs), included inside class LanguageParser
// (csharp_parser_internal.h) before any member declaration, so every translation unit sees them.

enum class TerminatorState : uint32_t {
    EndOfFile = 0,
    IsNamespaceMemberStartOrStop = 1u << 0,
    IsAttributeDeclarationTerminator = 1u << 1,
    IsPossibleAggregateClauseStartOrStop = 1u << 2,
    IsPossibleMemberStartOrStop = 1u << 3,
    IsEndOfReturnType = 1u << 4,
    IsEndOfParameterList = 1u << 5,
    IsEndOfFieldDeclaration = 1u << 6,
    IsPossibleEndOfVariableDeclaration = 1u << 7,
    IsEndOfTypeArgumentList = 1u << 8,
    IsPossibleStatementStartOrStop = 1u << 9,
    IsEndOfFixedStatement = 1u << 10,
    IsEndOfTryBlock = 1u << 11,
    IsEndOfCatchClause = 1u << 12,
    IsEndOfFilterClause = 1u << 13,
    IsEndOfCatchBlock = 1u << 14,
    IsEndOfDoWhileExpression = 1u << 15,
    IsEndOfForStatementArgument = 1u << 16,
    IsEndOfDeclarationClause = 1u << 17,
    IsEndOfArgumentList = 1u << 18,
    IsSwitchSectionStart = 1u << 19,
    IsEndOfTypeParameterList = 1u << 20,
    IsEndOfMethodSignature = 1u << 21,
    IsEndOfNameInExplicitInterface = 1u << 22,
    IsEndOfFunctionPointerParameterList = 1u << 23,
    IsEndOfFunctionPointerParameterListErrored = 1u << 24,
    IsEndOfFunctionPointerCallingConvention = 1u << 25,
    IsEndOfTypeSignature = 1u << 26,
    IsExpressionOrPatternInCaseLabelOfSwitchStatement = 1u << 27,
    IsPatternInSwitchExpressionArm = 1u << 28,
};
static constexpr uint32_t LastTerminatorState = 1u << 28;
friend constexpr TerminatorState operator|(TerminatorState a, TerminatorState b) {
    return static_cast<TerminatorState>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
friend constexpr TerminatorState operator&(TerminatorState a, TerminatorState b) {
    return static_cast<TerminatorState>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
friend constexpr TerminatorState operator~(TerminatorState a) {
    return static_cast<TerminatorState>(~static_cast<uint32_t>(a));
}
friend TerminatorState &operator|=(TerminatorState &a, TerminatorState b) { return a = a | b; }
friend TerminatorState &operator&=(TerminatorState &a, TerminatorState b) { return a = a & b; }
friend constexpr bool Any(TerminatorState a) { return static_cast<uint32_t>(a) != 0; }

/** @brief LanguageParser.NamespaceBodyBuilder: the four lists of a namespace body, from the pool. */
struct NamespaceBodyBuilder {
    SyntaxListBuilder Externs;
    SyntaxListBuilder Usings;
    SyntaxListBuilder Attributes;
    SyntaxListBuilder Members;
    explicit NamespaceBodyBuilder(SyntaxListPool &pool)
        : Externs(pool.Allocate()), Usings(pool.Allocate()), Attributes(pool.Allocate()), Members(pool.Allocate()) {}
    void Free(SyntaxListPool &pool) {
        pool.Free(Members);
        pool.Free(Attributes);
        pool.Free(Usings);
        pool.Free(Externs);
    }
};

enum class NamespaceParts {
    None = 0,
    ExternAliases = 1,
    Usings = 2,
    GlobalAttributes = 3,
    MembersAndStatements = 4,
    TypesAndNamespaces = 5,
    TopLevelStatementsAfterTypesAndNamespaces = 6,
};

enum class AccessorDeclaringKind { Property, Indexer, Event };

enum class PostSkipAction { Continue, Abort };

enum class VariableFlags : uint32_t {
    None = 0,
    Fixed = 0x01,
    Const = 0x02,
    LocalOrField = 0x04,
    ForStatement = 0x08,
};
friend constexpr VariableFlags operator|(VariableFlags a, VariableFlags b) {
    return static_cast<VariableFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
friend constexpr VariableFlags operator&(VariableFlags a, VariableFlags b) {
    return static_cast<VariableFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
friend constexpr VariableFlags operator~(VariableFlags a) { return static_cast<VariableFlags>(~static_cast<uint32_t>(a)); }
friend constexpr bool Any(VariableFlags a) { return static_cast<uint32_t>(a) != 0; }

enum class NameOptions : uint32_t {
    None = 0,
    InExpression = 1u << 0,
    InTypeList = 1u << 1,
    PossiblePattern = 1u << 2,
    AfterIs = 1u << 3,
    DefinitePattern = 1u << 4,
    AfterOut = 1u << 5,
    AfterTupleComma = 1u << 6,
    FirstElementOfPossibleTupleLiteral = 1u << 7,
};
friend constexpr NameOptions operator|(NameOptions a, NameOptions b) {
    return static_cast<NameOptions>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
friend constexpr NameOptions operator&(NameOptions a, NameOptions b) {
    return static_cast<NameOptions>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
friend constexpr NameOptions operator~(NameOptions a) { return static_cast<NameOptions>(~static_cast<uint32_t>(a)); }
friend NameOptions &operator|=(NameOptions &a, NameOptions b) { return a = a | b; }
friend NameOptions &operator&=(NameOptions &a, NameOptions b) { return a = a & b; }
friend constexpr bool Any(NameOptions a) { return static_cast<uint32_t>(a) != 0; }

enum class ScanTypeArgumentListKind { NotTypeArgumentList, PossibleTypeArgumentList, DefiniteTypeArgumentList };

enum class ScanTypeFlags {
    NotType,
    MustBeType,
    GenericTypeOrMethod,
    GenericTypeOrExpression,
    NonGenericTypeOrExpression,
    AliasQualifiedName,
    NullableType,
    PointerOrMultiplication,
    TupleType,
};

enum class ParseTypeMode {
    Normal,
    Parameter,
    AfterIs,
    DefinitePattern,
    AfterOut,
    AfterRef,
    AfterTupleComma,
    AsExpression,
    NewExpression,
    FirstElementOfPossibleTupleLiteral,
};

enum class Precedence : uint32_t {
    Expression = 0,
    Assignment = Expression,
    Lambda = Assignment,
    Conditional,
    Coalescing,
    ConditionalOr,
    ConditionalAnd,
    LogicalOr,
    LogicalXor,
    LogicalAnd,
    Equality,
    Relational,
    Shift,
    Additive,
    Multiplicative,
    Switch,
    Range,
    Unary,
    Cast,
    PointerIndirection,
    AddressOf,
    Primary,
};

/** @brief SkipBadTokens<TNode>: the error-recovery callback of ParseCommaSeparatedSyntaxList. */
using SkipBadTokensFn = std::function<PostSkipAction(LanguageParser *parser, GreenNode *&openToken,
                                                     SyntaxListBuilder builder, SyntaxKind expectedKind,
                                                     SyntaxKind closeTokenKind)>;
using IsPossibleElementFn = std::function<bool(LanguageParser *)>;
using ParseElementFn = std::function<GreenNode *(LanguageParser *)>;
using ImmediatelyAbortFn = std::function<bool(GreenNode *)>;
