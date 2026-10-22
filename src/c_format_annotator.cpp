// Token annotation for mep's C formatter: the port of clang-format's
// TokenAnnotator. For each unwrapped line it matches brackets, decides what
// each ambiguous token is (a `*` that declares a pointer or multiplies, a
// `)` that ends a cast, an identifier that starts a declaration's name, a `<`
// that clang-format takes for a template opener), inserts the "fake
// parentheses" that record operator precedence, and computes per token the
// spaces required before it, whether a break before it is required or
// allowed, and the penalty for breaking there.

#include "c_format_internal.h"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace cfmt::detail {

// --- FormatToken predicates ------------------------------------------------------

bool FormatToken::isStringLiteral() const {
    return isOneOf(tok::string_literal, tok::wide_string_literal, tok::utf8_string_literal, tok::utf16_string_literal,
                   tok::utf32_string_literal);
}

bool FormatToken::isLiteral() const {
    return isOneOf(tok::numeric_constant, tok::char_constant, tok::wide_char_constant, tok::utf8_char_constant,
                   tok::utf16_char_constant, tok::utf32_char_constant) ||
           isStringLiteral();
}

bool FormatToken::isIf() const {
    return is(tok::kw_if) || endsSequence(tok::kw_constexpr, tok::kw_if) || endsSequence(tok::identifier, tok::kw_if);
}

bool FormatToken::closesScopeAfterBlock() const {
    if (BlockKind == BK_Block) return true;
    if (closesScope()) return Previous->closesScopeAfterBlock();
    return false;
}

bool FormatToken::canBePointerOrReferenceQualifier() const {
    return isOneOf(tok::kw_const, tok::kw_restrict, tok::kw_volatile, tok::kw__Nonnull, tok::kw__Nullable,
                   tok::kw__Null_unspecified, tok::kw___ptr32, tok::kw___ptr64, tok::kw___funcref) ||
           isAttribute();
}

bool FormatToken::isSimpleTypeSpecifier() const {
    return isOneOf(tok::kw_short, tok::kw_long, tok::kw___int64, tok::kw___int128, tok::kw_signed, tok::kw_unsigned,
                   tok::kw_void, tok::kw_char, tok::kw_int, tok::kw_half, tok::kw_float, tok::kw_double,
                   tok::kw___bf16, tok::kw__Float16, tok::kw___float128, tok::kw___ibm128, tok::kw_wchar_t,
                   tok::kw_bool, tok::kw__Bool, tok::kw___auto_type, tok::kw_typeof, tok::kw_char8_t);
}

bool FormatToken::isTypeName() const {
    if (isSimpleTypeSpecifier()) return true;
    if (Kind != tok::identifier) return false;
    static const std::unordered_set<std::string_view> CppNonKeywordTypes = {
        "clock_t",  "int16_t", "int32_t", "int64_t",  "int8_t",   "intptr_t", "ptrdiff_t",
        "size_t",   "time_t",  "uint16_t", "uint32_t", "uint64_t", "uint8_t",  "uintptr_t",
    };
    return CppNonKeywordTypes.count(TokenText) != 0;
}

prec::Level FormatToken::getPrecedence() const {
    switch (Kind) {
    case tok::greater:
        return prec::Relational;
    case tok::greatergreater:
        return prec::Shift;
    case tok::comma:
        return prec::Comma;
    case tok::equal:
    case tok::starequal:
    case tok::slashequal:
    case tok::percentequal:
    case tok::plusequal:
    case tok::minusequal:
    case tok::lesslessequal:
    case tok::greatergreaterequal:
    case tok::ampequal:
    case tok::caretequal:
    case tok::pipeequal:
        return prec::Assignment;
    case tok::question:
        return prec::Conditional;
    case tok::pipepipe:
        return prec::LogicalOr;
    case tok::ampamp:
        return prec::LogicalAnd;
    case tok::pipe:
        return prec::InclusiveOr;
    case tok::caret:
        return prec::ExclusiveOr;
    case tok::amp:
        return prec::And;
    case tok::exclaimequal:
    case tok::equalequal:
        return prec::Equality;
    case tok::lessequal:
    case tok::less:
    case tok::greaterequal:
        return prec::Relational;
    case tok::lessless:
        return prec::Shift;
    case tok::plus:
    case tok::minus:
        return prec::Additive;
    case tok::percent:
    case tok::slash:
    case tok::star:
        return prec::Multiplicative;
    default:
        return prec::Unknown;
    }
}

bool FormatToken::isFunctionLikeKeyword() const {
    if (isAttribute()) return true;
    return isOneOf(tok::kw_return, tok::kw_sizeof, tok::kw_alignof, tok::kw_alignas, tok::kw_static_assert,
                   tok::kw__Atomic);
}

bool FormatToken::isLabelString() const {
    if (isNot(tok::string_literal)) return false;
    std::string_view Content = TokenText;
    if (!Content.empty() && (Content.front() == '"' || Content.front() == '\'')) Content.remove_prefix(1);
    if (!Content.empty() && (Content.back() == '"' || Content.back() == '\'')) Content.remove_suffix(1);
    size_t B = Content.find_first_not_of(" \t\n\v\f\r");
    if (B == std::string_view::npos) return false;
    size_t E = Content.find_last_not_of(" \t\n\v\f\r");
    Content = Content.substr(B, E - B + 1);
    return Content.size() > 1 && (Content.back() == ':' || Content.back() == '=');
}

bool FormatToken::opensBlockOrBlockTypeList() const {
    return is(TT::ArrayInitializerLSquare) ||
           (is(tok::l_brace) && (BlockKind == BK_Block || is(TT::DictLiteral)));
}

bool isLineComment(const FormatToken &T) { return T.is(tok::comment) && T.TokenText.substr(0, 2) != "/*"; }

bool continuesLineComment(const FormatToken &FormatTok, const FormatToken *Previous,
                          const FormatToken *MinColumnToken) {
    if (!Previous || !MinColumnToken) return false;
    unsigned MinContinueColumn = MinColumnToken->OriginalColumn + (isLineComment(*MinColumnToken) ? 0 : 1);
    return isLineComment(FormatTok) && FormatTok.NewlinesBefore == 1 && isLineComment(*Previous) &&
           FormatTok.OriginalColumn >= MinContinueColumn;
}

bool startsNextParameter(const FormatToken &Current) {
    const FormatToken &Previous = *Current.Previous;
    return Previous.is(tok::comma) && !Current.isTrailingComment();
}

// --- AnnotatedLine ---------------------------------------------------------------

AnnotatedLine::AnnotatedLine(const UnwrappedLine &Line)
    : First(Line.Tokens.front().Tok), Level(Line.Level), PPLevel(Line.PPLevel),
      MatchingOpeningBlockLineIndex(Line.MatchingOpeningBlockLineIndex),
      MatchingClosingBlockLineIndex(Line.MatchingClosingBlockLineIndex), InPPDirective(Line.InPPDirective),
      InPragmaDirective(Line.InPragmaDirective), InMacroBody(Line.InMacroBody),
      MustBeDeclaration(Line.MustBeDeclaration), IsContinuation(Line.IsContinuation) {
    First->Previous = nullptr;
    FormatToken *Current = First;
    auto addChildren = [this](const UnwrappedLineNode &Node, FormatToken *Cur) {
        Cur->Children.clear();
        for (const UnwrappedLine &Child : Node.Children) {
            Children.push_back(new AnnotatedLine(Child));
            Cur->Children.push_back(Children.back());
        }
    };
    addChildren(Line.Tokens.front(), Current);
    for (size_t i = 1; i < Line.Tokens.size(); ++i) {
        const UnwrappedLineNode &Node = Line.Tokens[i];
        Current->Next = Node.Tok;
        Node.Tok->Previous = Current;
        Current = Current->Next;
        addChildren(Node, Current);
    }
    Last = Current;
    Last->Next = nullptr;
}

AnnotatedLine::~AnnotatedLine() {
    for (AnnotatedLine *Child : Children) delete Child;
    for (FormatToken *Current = First; Current; Current = Current->Next) {
        Current->Children.clear();
        Current->Role.reset();
    }
}

namespace {

bool mustBreakAfterAttributes(const FormatToken &Tok) {
    // BreakAfterAttributes: Leave.
    return Tok.NewlinesBefore > 0;
}

bool startsWithInitStatement(const AnnotatedLine &Line) {
    return Line.startsWith(tok::kw_for) || Line.startsWith(tok::kw_if) || Line.startsWith(tok::kw_switch);
}

bool canBeObjCSelectorComponent(const FormatToken &Tok) { return Tok.HasIdentifierInfo; }

bool isLambdaParameterList(const FormatToken *Left) {
    if (Left->Previous && Left->Previous->is(tok::greater) && Left->Previous->MatchingParen &&
        Left->Previous->MatchingParen->is(TT::TemplateOpener)) {
        Left = Left->Previous->MatchingParen;
    }
    return Left->Previous && Left->Previous->is(tok::r_square) && Left->Previous->MatchingParen &&
           Left->Previous->MatchingParen->is(TT::LambdaLSquare);
}

bool isCppAttribute(const FormatToken &Tok) {
    if (!Tok.startsSequence(tok::l_square, tok::l_square)) return false;
    if (Tok.Previous && Tok.Previous->is(tok::at)) return false;
    const FormatToken *AttrTok = Tok.Next->Next;
    if (!AttrTok) return false;
    if (AttrTok->isNot(tok::identifier)) return false;
    while (AttrTok && !AttrTok->startsSequence(tok::r_square, tok::r_square)) {
        if (AttrTok->is(tok::colon) || AttrTok->startsSequence(tok::identifier, tok::identifier) ||
            AttrTok->startsSequence(tok::r_paren, tok::identifier)) {
            return false;
        }
        if (AttrTok->is(tok::ellipsis)) return true;
        AttrTok = AttrTok->Next;
    }
    return AttrTok && AttrTok->startsSequence(tok::r_square, tok::r_square);
}

bool isCppStructuredBinding(const FormatToken &Tok) {
    if (Tok.isNot(tok::l_square)) return false;
    const FormatToken *T = &Tok;
    do {
        T = T->getPreviousNonComment();
    } while (T && T->isOneOf(tok::kw_const, tok::kw_volatile, tok::amp, tok::ampamp));
    return T && T->is(tok::kw_auto);
}

bool isAllUpperCase(std::string_view S) {
    for (char C : S)
        if (C >= 'a' && C <= 'z') return false;
    return true;
}

ScopeType getScopeType(const FormatToken &Token) {
    switch (Token.Type) {
    case TT::StructLBrace:
    case TT::UnionLBrace:
        return ST_Class;
    default:
        return ST_Other;
    }
}

constexpr int PrecedenceUnaryOperator = prec::PointerToMember + 1;
constexpr int PrecedenceArrowAndPeriod = prec::PointerToMember + 2;

class AnnotatingParser {
public:
    AnnotatingParser(const Style &S, AnnotatedLine &Line, std::vector<ScopeType> &Scopes)
        : S(S), Line(Line), CurrentToken(Line.First), Scopes(Scopes) {
        Contexts.push_back(Context(tok::unknown, 1, /*IsExpression=*/false));
        resetTokenMetadata();
    }

    LineType parseLine() {
        if (!CurrentToken) return LT_Invalid;
        NonTemplateLess.clear();
        if (!Line.InMacroBody && CurrentToken->is(tok::hash)) {
            LineType Type = parsePreprocessorDirective();
            if (Type != LT_Invalid) return Type;
        }
        if (CurrentToken->HasIdentifierInfo && CurrentToken->PPKind == pp::import && CurrentToken->Next &&
            CurrentToken->Next->isOneOf(tok::string_literal, tok::identifier, tok::kw_static)) {
            next();
            parseIncludeDirective();
            return LT_ImportStatement;
        }
        if (CurrentToken->is(tok::less) && Line.Last->is(tok::greater)) {
            parseIncludeDirective();
            return LT_ImportStatement;
        }
        while (CurrentToken) {
            if (!consumeToken()) return LT_Invalid;
        }
        if (Line.startsWith(TT::ObjCMethodSpecifier)) return LT_ObjCMethodDecl;
        return LT_Other;
    }

private:
    struct Context {
        Context(tok ContextKind, unsigned BindingStrength, bool IsExpression)
            : ContextKind(ContextKind), BindingStrength(BindingStrength), IsExpression(IsExpression) {}
        tok ContextKind;
        unsigned BindingStrength;
        bool IsExpression;
        unsigned LongestObjCSelectorName = 0;
        bool ColonIsForRangeExpr = false;
        bool ColonIsDictLiteral = false;
        bool ColonIsObjCMethodExpr = false;
        FormatToken *FirstObjCSelectorName = nullptr;
        FormatToken *FirstStartOfName = nullptr;
        bool CanBeExpression = true;
        bool CaretFound = false;
        bool InCpp11AttributeSpecifier = false;
        enum { Unknown, CtorInitializer, ForEachMacro, InheritanceList, TemplateArgument, C11GenericSelection } ContextType =
            Unknown;
    };

    struct ScopedContextCreator {
        AnnotatingParser &P;
        ScopedContextCreator(AnnotatingParser &P, tok ContextKind, unsigned Increase) : P(P) {
            P.Contexts.push_back(
                Context(ContextKind, P.Contexts.back().BindingStrength + Increase, P.Contexts.back().IsExpression));
        }
        ~ScopedContextCreator() { P.Contexts.pop_back(); }
    };

    bool parseAngle() {
        if (!CurrentToken) return false;
        FormatToken *Left = CurrentToken->Previous;
        if (!Left) return false;
        if (NonTemplateLess.count(Left) > 0) return false;
        const FormatToken *BeforeLess = Left->Previous;
        if (BeforeLess) {
            if (BeforeLess->isLiteral()) return false;
            if (BeforeLess->is(tok::r_brace)) return false;
            if (BeforeLess->is(tok::r_paren) && Contexts.size() > 1) return false;
        }
        Left->ParentBracket = Contexts.back().ContextKind;
        ScopedContextCreator ContextCreator(*this, tok::less, 12);
        Contexts.back().IsExpression = false;
        if (BeforeLess) Contexts.back().ContextType = Context::TemplateArgument;
        for (bool SeenTernaryOperator = false, MaybeAngles = true; CurrentToken;) {
            const bool InExpr = Contexts[Contexts.size() - 2].IsExpression;
            if (CurrentToken->is(tok::greater)) {
                const FormatToken *Next = CurrentToken->Next;
                if (CurrentToken->isNot(TT::TemplateCloser)) {
                    if (Next && Next->is(tok::greater) && Left->ParentBracket != tok::less &&
                        CurrentToken->TokenText.data() + 1 == Next->TokenText.data()) {
                        return false;
                    }
                    if (InExpr && SeenTernaryOperator && (!Next || !Next->isOneOf(tok::l_paren, tok::l_brace)))
                        return false;
                    if (!MaybeAngles) return false;
                }
                Left->MatchingParen = CurrentToken;
                CurrentToken->MatchingParen = Left;
                CurrentToken->setType(TT::TemplateCloser);
                if (Next && Next->isLiteral()) return false;
                next();
                return true;
            }
            if (CurrentToken->isOneOf(tok::r_paren, tok::r_square, tok::r_brace)) return false;
            const FormatToken &Prev = *CurrentToken->Previous;
            if (MaybeAngles && InExpr && Prev.is(TT::BinaryOperator) && Prev.isOneOf(tok::pipepipe, tok::ampamp))
                MaybeAngles = false;
            if (Prev.isOneOf(tok::question, tok::colon)) SeenTernaryOperator = true;
            updateParameterCount(Left, CurrentToken);
            if (!consumeToken()) return false;
        }
        return false;
    }

    bool parseUntouchableParens() {
        while (CurrentToken) {
            CurrentToken->Finalized = true;
            switch (CurrentToken->Kind) {
            case tok::l_paren:
                next();
                if (!parseUntouchableParens()) return false;
                continue;
            case tok::r_paren:
                next();
                return true;
            default:
                break;
            }
            next();
        }
        return false;
    }

    bool parseParens(bool IsIf = false) {
        if (!CurrentToken) return false;
        FormatToken &OpeningParen = *CurrentToken->Previous;
        FormatToken *PrevNonComment = OpeningParen.getPreviousNonComment();
        OpeningParen.ParentBracket = Contexts.back().ContextKind;
        ScopedContextCreator ContextCreator(*this, tok::l_paren, 1);
        Contexts.back().ColonIsForRangeExpr = Contexts.size() == 2 && Contexts[0].ColonIsForRangeExpr;
        if (OpeningParen.Previous && OpeningParen.Previous->is(TT::UntouchableMacroFunc)) {
            OpeningParen.Finalized = true;
            return parseUntouchableParens();
        }
        if (OpeningParen.Previous &&
            (OpeningParen.Previous->isOneOf(tok::kw_static_assert, tok::kw_while, tok::l_paren, tok::comma,
                                            TT::CastRParen, TT::BinaryOperator) ||
             OpeningParen.Previous->isIf())) {
            Contexts.back().IsExpression = true;
        } else if (isLambdaParameterList(&OpeningParen)) {
            OpeningParen.setType(TT::LambdaDefinitionLParen);
            Contexts.back().IsExpression = false;
        } else if (OpeningParen.Previous && OpeningParen.Previous->is(tok::kw__Generic)) {
            Contexts.back().ContextType = Context::C11GenericSelection;
            Contexts.back().IsExpression = true;
        } else if (Line.InPPDirective && (!OpeningParen.Previous || OpeningParen.Previous->isNot(tok::identifier))) {
            Contexts.back().IsExpression = true;
        } else if (Contexts[Contexts.size() - 2].CaretFound) {
            Contexts.back().IsExpression = false;
        } else if (OpeningParen.Previous && OpeningParen.Previous->is(TT::ForEachMacro)) {
            Contexts.back().ContextType = Context::ForEachMacro;
            Contexts.back().IsExpression = false;
        } else if (OpeningParen.Previous && OpeningParen.Previous->MatchingParen &&
                   OpeningParen.Previous->MatchingParen->isOneOf(TT::ObjCBlockLParen, TT::FunctionTypeLParen)) {
            Contexts.back().IsExpression = false;
        } else if (!Line.MustBeDeclaration && (!Line.InPPDirective || (Line.InMacroBody && !Scopes.empty()))) {
            bool IsForOrCatch = OpeningParen.Previous && OpeningParen.Previous->is(tok::kw_for);
            Contexts.back().IsExpression = !IsForOrCatch;
        }
        if (PrevNonComment && OpeningParen.is(TT::Unknown)) {
            if (PrevNonComment->isAttribute()) {
                OpeningParen.setType(TT::AttributeLParen);
            } else if (PrevNonComment->isOneOf(tok::kw_typeof, tok::kw__Atomic)) {
                OpeningParen.setType(TT::TypeDeclarationParen);
                if (PrevNonComment->is(tok::kw_typeof)) Contexts.back().IsExpression = true;
            }
        }
        bool MightBeFunctionType = !Contexts[Contexts.size() - 2].IsExpression;
        bool ProbablyFunctionType = CurrentToken->isPointerOrReference() || CurrentToken->is(tok::caret);
        bool HasMultipleLines = false;
        bool HasMultipleParametersOnALine = false;
        bool MightBeObjCForRangeLoop = OpeningParen.Previous && OpeningParen.Previous->is(tok::kw_for);
        FormatToken *PossibleObjCForInToken = nullptr;
        while (CurrentToken) {
            const FormatToken &Prev = *CurrentToken->Previous;
            const FormatToken *PrevPrev = Prev.Previous;
            if (Prev.is(TT::PointerOrReference) && PrevPrev->isOneOf(tok::l_paren, tok::coloncolon))
                ProbablyFunctionType = true;
            if (CurrentToken->is(tok::comma)) MightBeFunctionType = false;
            if (Prev.is(TT::BinaryOperator)) Contexts.back().IsExpression = true;
            if (CurrentToken->is(tok::r_paren)) {
                if (Prev.is(TT::PointerOrReference) && (PrevPrev == &OpeningParen || PrevPrev->is(tok::coloncolon)))
                    MightBeFunctionType = true;
                if (MightBeFunctionType && ProbablyFunctionType && CurrentToken->Next &&
                    (CurrentToken->Next->is(tok::l_paren) ||
                     (CurrentToken->Next->is(tok::l_square) &&
                      (Line.MustBeDeclaration || (PrevNonComment && PrevNonComment->isTypeName()))))) {
                    OpeningParen.setType(OpeningParen.Next->is(tok::caret) ? TT::ObjCBlockLParen
                                                                           : TT::FunctionTypeLParen);
                }
                OpeningParen.MatchingParen = CurrentToken;
                CurrentToken->MatchingParen = &OpeningParen;
                if (CurrentToken->Next && CurrentToken->Next->is(tok::l_brace) && OpeningParen.Previous &&
                    OpeningParen.Previous->is(tok::l_paren)) {
                    for (FormatToken *Tok = &OpeningParen; Tok != CurrentToken; Tok = Tok->Next)
                        if (Tok->is(TT::BinaryOperator) && Tok->isPointerOrReference())
                            Tok->setType(TT::PointerOrReference);
                }
                if (OpeningParen.is(TT::AttributeLParen)) CurrentToken->setType(TT::AttributeRParen);
                if (OpeningParen.is(TT::TypeDeclarationParen)) CurrentToken->setType(TT::TypeDeclarationParen);
                if (OpeningParen.Previous && OpeningParen.Previous->is(TT::AttributeSquare))
                    CurrentToken->setType(TT::AttributeSquare);
                if (!HasMultipleLines)
                    OpeningParen.PackingKind = PPK_Inconclusive;
                else if (HasMultipleParametersOnALine)
                    OpeningParen.PackingKind = PPK_BinPacked;
                else
                    OpeningParen.PackingKind = PPK_OnePerLine;
                next();
                return true;
            }
            if (CurrentToken->isOneOf(tok::r_square, tok::r_brace)) return false;
            if (CurrentToken->is(tok::l_brace) && OpeningParen.is(TT::ObjCBlockLParen))
                OpeningParen.setType(TT::Unknown);
            if (CurrentToken->is(tok::comma) && CurrentToken->Next && !CurrentToken->Next->HasUnescapedNewline &&
                !CurrentToken->Next->isTrailingComment()) {
                HasMultipleParametersOnALine = true;
            }
            bool ProbablyFunctionTypeLParen = CurrentToken->is(tok::l_paren) && CurrentToken->Next &&
                                              CurrentToken->Next->isOneOf(tok::star, tok::amp, tok::caret);
            if ((Prev.isOneOf(tok::kw_const, tok::kw_auto) || Prev.isTypeName()) &&
                !(CurrentToken->is(tok::l_brace) ||
                  (CurrentToken->is(tok::l_paren) && !ProbablyFunctionTypeLParen))) {
                Contexts.back().IsExpression = false;
            }
            if (CurrentToken->isOneOf(tok::semi, tok::colon)) {
                MightBeObjCForRangeLoop = false;
                if (PossibleObjCForInToken) {
                    PossibleObjCForInToken->setType(TT::Unknown);
                    PossibleObjCForInToken = nullptr;
                }
            }
            if (IsIf && CurrentToken->is(tok::semi)) {
                for (FormatToken *Tok = OpeningParen.Next;
                     Tok != CurrentToken && !Tok->isOneOf(tok::equal, tok::l_paren, tok::l_brace); Tok = Tok->Next) {
                    if (Tok->isPointerOrReference()) Tok->setFinalizedType(TT::PointerOrReference);
                }
            }
            if (MightBeObjCForRangeLoop && CurrentToken->is(tok::identifier) && CurrentToken->TokenText == "in") {
                PossibleObjCForInToken = CurrentToken;
                PossibleObjCForInToken->setType(TT::ObjCForIn);
            }
            if (CurrentToken->is(tok::comma)) Contexts.back().CanBeExpression = true;
            FormatToken *Tok = CurrentToken;
            if (!consumeToken()) return false;
            updateParameterCount(&OpeningParen, Tok);
            if (CurrentToken && CurrentToken->HasUnescapedNewline) HasMultipleLines = true;
        }
        return false;
    }

    bool parseSquare() {
        if (!CurrentToken) return false;
        FormatToken *Left = CurrentToken->Previous;
        Left->ParentBracket = Contexts.back().ContextKind;
        FormatToken *Parent = Left->getPreviousNonComment();
        bool CppArrayTemplates = Parent && Parent->is(TT::TemplateCloser) &&
                                 (Contexts.back().CanBeExpression || Contexts.back().IsExpression ||
                                  Contexts.back().ContextType == Context::TemplateArgument);
        const bool IsInnerSquare = Contexts.back().InCpp11AttributeSpecifier;
        const bool IsCpp11AttributeSpecifier = isCppAttribute(*Left) || IsInnerSquare;
        bool InsideInlineASM = Line.startsWith(tok::kw_asm);
        bool IsCppStructuredBinding = isCppStructuredBinding(*Left);
        bool StartsObjCMethodExpr =
            !IsCppStructuredBinding && !InsideInlineASM && !CppArrayTemplates && !IsCpp11AttributeSpecifier &&
            Contexts.back().CanBeExpression && Left->isNot(TT::LambdaLSquare) &&
            !CurrentToken->isOneOf(tok::l_brace, tok::r_square) &&
            (!Parent || Parent->isOneOf(tok::colon, tok::l_square, tok::l_paren, tok::kw_return) ||
             Parent->isUnaryOperator() || Parent->isOneOf(TT::ObjCForIn, TT::CastRParen) ||
             Parent->getPrecedence() > prec::Unknown);
        bool ColonFound = false;
        unsigned BindingIncrease = 1;
        if (IsCppStructuredBinding) {
            Left->setType(TT::StructuredBindingLSquare);
        } else if (Left->is(TT::Unknown)) {
            if (StartsObjCMethodExpr) {
                Left->setType(TT::ObjCMethodExpr);
            } else if (InsideInlineASM) {
                Left->setType(TT::InlineASMSymbolicNameLSquare);
            } else if (IsCpp11AttributeSpecifier) {
                Left->setType(TT::AttributeSquare);
                if (!IsInnerSquare && Left->Previous) Left->Previous->EndsCppAttributeGroup = false;
            } else if (Contexts.back().ContextKind == tok::l_brace && Parent &&
                       Parent->isOneOf(tok::l_brace, tok::comma)) {
                Left->setType(TT::DesignatedInitializerLSquare);
            } else if (CurrentToken->is(tok::r_square) && Parent && Parent->is(TT::TemplateCloser)) {
                Left->setType(TT::ArraySubscriptLSquare);
            } else if (!CppArrayTemplates && Parent &&
                       Parent->isOneOf(TT::BinaryOperator, TT::TemplateCloser, tok::at, tok::comma, tok::l_paren,
                                       tok::l_square, tok::question, tok::colon, tok::kw_return, tok::kw_default)) {
                Left->setType(TT::ArrayInitializerLSquare);
            } else {
                BindingIncrease = 10;
                Left->setType(TT::ArraySubscriptLSquare);
            }
        }
        ScopedContextCreator ContextCreator(*this, tok::l_square, BindingIncrease);
        Contexts.back().IsExpression = true;
        Contexts.back().ColonIsObjCMethodExpr = StartsObjCMethodExpr;
        Contexts.back().InCpp11AttributeSpecifier = IsCpp11AttributeSpecifier;
        while (CurrentToken) {
            if (CurrentToken->is(tok::r_square)) {
                if (IsCpp11AttributeSpecifier) {
                    CurrentToken->setType(TT::AttributeSquare);
                    if (!IsInnerSquare) CurrentToken->EndsCppAttributeGroup = true;
                }
                if (((CurrentToken->Next && CurrentToken->Next->is(tok::l_paren)) ||
                     (CurrentToken->Previous && CurrentToken->Previous->Previous == Left)) &&
                    Left->is(TT::ObjCMethodExpr)) {
                    StartsObjCMethodExpr = false;
                    Left->setType(TT::Unknown);
                }
                if (StartsObjCMethodExpr && CurrentToken->Previous != Left) {
                    CurrentToken->setType(TT::ObjCMethodExpr);
                    if (!ColonFound && CurrentToken->Previous && CurrentToken->Previous->is(TT::Unknown) &&
                        canBeObjCSelectorComponent(*CurrentToken->Previous)) {
                        CurrentToken->Previous->setType(TT::SelectorName);
                    }
                    if (Parent && Parent->is(TT::PointerOrReference)) Parent->overwriteFixedType(TT::BinaryOperator);
                }
                Left->MatchingParen = CurrentToken;
                CurrentToken->MatchingParen = Left;
                if (!Contexts.back().FirstObjCSelectorName) {
                    FormatToken *Previous = CurrentToken->getPreviousNonComment();
                    if (Previous && Previous->is(TT::SelectorName)) {
                        Previous->ObjCSelectorNameParts = 1;
                        Contexts.back().FirstObjCSelectorName = Previous;
                    }
                } else {
                    Left->ParameterCount = Contexts.back().FirstObjCSelectorName->ObjCSelectorNameParts;
                }
                if (Contexts.back().FirstObjCSelectorName) {
                    Contexts.back().FirstObjCSelectorName->LongestObjCSelectorName =
                        Contexts.back().LongestObjCSelectorName;
                    if (Left->BlockParameterCount > 1)
                        Contexts.back().FirstObjCSelectorName->LongestObjCSelectorName = 0;
                }
                next();
                return true;
            }
            if (CurrentToken->isOneOf(tok::r_paren, tok::r_brace)) return false;
            if (CurrentToken->is(tok::colon)) {
                if (IsCpp11AttributeSpecifier && CurrentToken->endsSequence(tok::colon, tok::identifier) &&
                    CurrentToken->Previous && CurrentToken->Previous->Previous &&
                    CurrentToken->Previous->Previous->TokenText == "using") {
                    CurrentToken->setType(TT::AttributeColon);
                } else if (!Line.InPragmaDirective &&
                           Left->isOneOf(TT::ArraySubscriptLSquare, TT::DesignatedInitializerLSquare)) {
                    Left->setType(TT::ObjCMethodExpr);
                    StartsObjCMethodExpr = true;
                    Contexts.back().ColonIsObjCMethodExpr = true;
                    if (Parent && Parent->is(tok::r_paren)) Parent->setType(TT::CastRParen);
                }
                ColonFound = true;
            }
            if (CurrentToken->is(tok::comma) && Left->is(TT::ObjCMethodExpr) && !ColonFound)
                Left->setType(TT::ArrayInitializerLSquare);
            FormatToken *Tok = CurrentToken;
            if (!consumeToken()) return false;
            updateParameterCount(Left, Tok);
        }
        return false;
    }

    bool parseBrace() {
        if (!CurrentToken) return true;
        FormatToken &OpeningBrace = *CurrentToken->Previous;
        OpeningBrace.ParentBracket = Contexts.back().ContextKind;
        if (Contexts.back().CaretFound) OpeningBrace.overwriteFixedType(TT::ObjCBlockLBrace);
        Contexts.back().CaretFound = false;
        ScopedContextCreator ContextCreator(*this, tok::l_brace, 1);
        Contexts.back().ColonIsDictLiteral = true;
        if (OpeningBrace.is(BK_BracedInit)) Contexts.back().IsExpression = true;
        while (CurrentToken) {
            if (CurrentToken->is(tok::r_brace)) {
                if (!Scopes.empty()) Scopes.pop_back();
                OpeningBrace.MatchingParen = CurrentToken;
                CurrentToken->MatchingParen = &OpeningBrace;
                next();
                return true;
            }
            if (CurrentToken->isOneOf(tok::r_paren, tok::r_square)) return false;
            updateParameterCount(&OpeningBrace, CurrentToken);
            if (CurrentToken->isOneOf(tok::colon, tok::l_brace, tok::less)) {
                if (CurrentToken->is(tok::colon) && OpeningBrace.is(TT::Unknown)) OpeningBrace.setType(TT::DictLiteral);
            }
            if (!consumeToken()) return false;
        }
        return true;
    }

    void updateParameterCount(FormatToken *Left, FormatToken *Current) {
        if (Current->is(tok::l_brace) && Current->is(BK_Block)) ++Left->BlockParameterCount;
        if (Current->is(tok::comma)) {
            ++Left->ParameterCount;
            if (!Left->Role) Left->Role = makeCommaSeparatedList(S);
            Left->Role->CommaFound(Current);
        } else if (Left->ParameterCount == 0 && Current->isNot(tok::comment)) {
            Left->ParameterCount = 1;
        }
    }

    bool parseConditional() {
        while (CurrentToken) {
            if (CurrentToken->is(tok::colon) && CurrentToken->is(TT::Unknown)) {
                CurrentToken->setType(TT::ConditionalExpr);
                next();
                return true;
            }
            if (!consumeToken()) return false;
        }
        return false;
    }

    bool consumeToken() {
        {
            const FormatToken *Prev = CurrentToken->getPreviousNonComment();
            if (Prev && Prev->is(tok::r_square) && Prev->is(TT::AttributeSquare) &&
                CurrentToken->isOneOf(tok::kw_if, tok::kw_switch, tok::kw_case, tok::kw_default, tok::kw_for,
                                      tok::kw_while) &&
                mustBreakAfterAttributes(*CurrentToken)) {
                CurrentToken->MustBreakBefore = true;
            }
        }
        FormatToken *Tok = CurrentToken;
        next();
        FormatToken *Prev = Tok->getPreviousNonComment();
        FormatToken *Next = Tok->getNextNonComment();
        bool IsIf = false;
        switch (Tok->Kind) {
        case tok::plus:
        case tok::minus:
            if (!Prev && Line.MustBeDeclaration) Tok->setType(TT::ObjCMethodSpecifier);
            break;
        case tok::colon:
            if (!Prev) return false;
            if (Tok->isTypeFinalized()) break;
            if (Line.First->is(tok::kw_asm)) {
                Tok->setType(TT::InlineASMColon);
            } else if (Contexts.back().ColonIsDictLiteral) {
                Tok->setType(TT::DictLiteral);
            } else if (Contexts.back().ColonIsObjCMethodExpr || Line.startsWith(TT::ObjCMethodSpecifier)) {
                Tok->setType(TT::ObjCMethodExpr);
                const FormatToken *PrevPrev = Prev->Previous;
                bool UnknownIdentifierInMethodDeclaration = Line.startsWith(TT::ObjCMethodSpecifier) &&
                                                            Prev->is(tok::identifier) && Prev->is(TT::Unknown);
                if (!PrevPrev ||
                    !(PrevPrev->is(TT::CastRParen) || (PrevPrev->is(TT::ObjCMethodExpr) && PrevPrev->is(tok::colon))) ||
                    PrevPrev->is(tok::r_square) || Contexts.back().LongestObjCSelectorName == 0 ||
                    UnknownIdentifierInMethodDeclaration) {
                    Prev->setType(TT::SelectorName);
                    if (!Contexts.back().FirstObjCSelectorName)
                        Contexts.back().FirstObjCSelectorName = Prev;
                    else if (Prev->ColumnWidth > Contexts.back().LongestObjCSelectorName)
                        Contexts.back().LongestObjCSelectorName = Prev->ColumnWidth;
                    Prev->ParameterIndex = Contexts.back().FirstObjCSelectorName->ObjCSelectorNameParts;
                    ++Contexts.back().FirstObjCSelectorName->ObjCSelectorNameParts;
                }
            } else if (Contexts.back().ColonIsForRangeExpr) {
                Tok->setType(TT::RangeBasedForLoopColon);
                for (FormatToken *Token = Prev; Token && !Token->isOneOf(tok::semi, tok::l_paren);
                     Token = Token->Previous) {
                    if (Token->isPointerOrReference()) Token->setFinalizedType(TT::PointerOrReference);
                }
            } else if (Contexts.back().ContextType == Context::C11GenericSelection) {
                Tok->setType(TT::GenericSelectionColon);
                if (Prev->isPointerOrReference()) Prev->setFinalizedType(TT::PointerOrReference);
            } else if ((CurrentToken && CurrentToken->is(tok::numeric_constant)) ||
                       (Prev->is(TT::StartOfName) && !Scopes.empty() && Scopes.back() == ST_Class)) {
                Tok->setType(TT::BitFieldColon);
            } else if (Contexts.size() == 1 &&
                       !Line.getFirstNonComment()->isOneOf(tok::kw_enum, tok::kw_case, tok::kw_default) &&
                       !Line.startsWith(tok::kw_typedef, tok::kw_enum)) {
                if (Prev->is(tok::r_paren))
                    Tok->setType(TT::CtorInitializerColon);
                else
                    Tok->setType(TT::InheritanceColon);
            } else if (canBeObjCSelectorComponent(*Prev) && Next &&
                       (Next->isOneOf(tok::r_paren, tok::comma) ||
                        (canBeObjCSelectorComponent(*Next) && Next->Next && Next->Next->is(tok::colon)))) {
                Tok->setType(TT::ObjCMethodExpr);
            }
            break;
        case tok::kw_if:
            if (CurrentToken && CurrentToken->isOneOf(tok::kw_constexpr, tok::identifier)) next();
            IsIf = true;
            [[fallthrough]];
        case tok::kw_while:
            if (CurrentToken && CurrentToken->is(tok::l_paren)) {
                next();
                if (!parseParens(IsIf)) return false;
            }
            break;
        case tok::kw_for:
            Contexts.back().ColonIsForRangeExpr = true;
            if (!CurrentToken || CurrentToken->isNot(tok::l_paren)) return false;
            next();
            if (!parseParens()) return false;
            break;
        case tok::l_paren:
            if (!parseParens()) return false;
            if (Line.MustBeDeclaration && Contexts.size() == 1 && !Contexts.back().IsExpression &&
                !Line.startsWith(tok::l_paren) && !Tok->is(TT::TypeDeclarationParen)) {
                if (!Prev || (!Prev->isAttribute() && !Prev->is(TT::BinaryOperator))) {
                    Line.MightBeFunctionDecl = true;
                    Tok->MightBeFunctionDeclParen = true;
                }
            }
            break;
        case tok::l_square:
            if (!parseSquare()) return false;
            break;
        case tok::l_brace:
            Scopes.push_back(getScopeType(*Tok));
            if (!parseBrace()) return false;
            break;
        case tok::less:
            if (parseAngle()) {
                Tok->setType(TT::TemplateOpener);
            } else {
                Tok->setType(TT::BinaryOperator);
                NonTemplateLess.insert(Tok);
                CurrentToken = Tok;
                next();
            }
            break;
        case tok::r_paren:
        case tok::r_square:
            return false;
        case tok::r_brace:
            if (!Scopes.empty()) Scopes.pop_back();
            if (Prev) return false;
            break;
        case tok::greater:
            if (Tok->is(TT::Unknown)) Tok->setType(TT::BinaryOperator);
            if (Prev && Prev->is(TT::TemplateCloser)) Tok->SpacesRequiredBefore = 1;
            break;
        case tok::question:
            parseConditional();
            break;
        case tok::comma:
            switch (Contexts.back().ContextType) {
            case Context::CtorInitializer:
                Tok->setType(TT::CtorInitializerComma);
                break;
            case Context::InheritanceList:
                Tok->setType(TT::InheritanceComma);
                break;
            default:
                if (Contexts.back().FirstStartOfName && (Contexts.size() == 1 || startsWithInitStatement(Line))) {
                    Contexts.back().FirstStartOfName->PartOfMultiVariableDeclStmt = true;
                    Line.IsMultiVariableDeclStmt = true;
                }
                break;
            }
            if (Contexts.back().ContextType == Context::ForEachMacro) Contexts.back().IsExpression = true;
            break;
        case tok::identifier:
            if (Tok->TokenText == "__has_include" || Tok->TokenText == "__has_include_next") parseHasInclude();
            if (Next && Next->is(tok::l_paren) && Prev &&
                Prev->isOneOf(tok::kw___cdecl, tok::kw___stdcall, tok::kw___fastcall, tok::kw___thiscall,
                              tok::kw___regcall, tok::kw___vectorcall)) {
                Tok->setFinalizedType(TT::FunctionDeclarationName);
                Next->setFinalizedType(TT::FunctionDeclarationLParen);
            }
            break;
        default:
            break;
        }
        return true;
    }

    void parseIncludeDirective() {
        if (CurrentToken && CurrentToken->is(tok::less)) {
            next();
            while (CurrentToken) {
                if (CurrentToken->isNot(tok::comment) && CurrentToken->TokenText.substr(0, 2) != "//")
                    CurrentToken->setType(TT::ImplicitStringLiteral);
                next();
            }
        }
    }

    void parseWarningOrError() {
        next();
        next();
        while (CurrentToken) {
            CurrentToken->setType(TT::ImplicitStringLiteral);
            next();
        }
    }

    void parsePragma() {
        next();  // pragma
        if (CurrentToken && CurrentToken->is(tok::identifier) &&
            (CurrentToken->TokenText == "mark" || CurrentToken->TokenText == "option" ||
             CurrentToken->TokenText == "region")) {
            bool IsMarkOrRegion = CurrentToken->TokenText == "mark" || CurrentToken->TokenText == "region";
            next();
            next();
            while (CurrentToken) {
                if (IsMarkOrRegion || CurrentToken->Previous->is(TT::BinaryOperator))
                    CurrentToken->setType(TT::ImplicitStringLiteral);
                next();
            }
        }
    }

    void parseHasInclude() {
        if (!CurrentToken || CurrentToken->isNot(tok::l_paren)) return;
        next();
        parseIncludeDirective();
        next();
    }

    LineType parsePreprocessorDirective() {
        LineType Type = LT_PreprocessorDirective;
        next();
        if (!CurrentToken) return Type;
        if (CurrentToken->is(tok::numeric_constant)) {
            CurrentToken->SpacesRequiredBefore = 1;
            return Type;
        }
        if (!CurrentToken->HasIdentifierInfo) return Type;
        switch (CurrentToken->PPKind) {
        case pp::include:
        case pp::include_next:
        case pp::import:
            next();
            parseIncludeDirective();
            Type = LT_ImportStatement;
            break;
        case pp::error:
        case pp::warning:
            parseWarningOrError();
            break;
        case pp::pragma:
            parsePragma();
            break;
        case pp::if_:
        case pp::elif:
            Contexts.back().IsExpression = true;
            next();
            if (CurrentToken) CurrentToken->SpacesRequiredBefore = 1;
            parseLine();
            break;
        default:
            break;
        }
        while (CurrentToken) {
            FormatToken *Tok = CurrentToken;
            next();
            if (Tok->is(tok::l_paren))
                parseParens();
            else if (Tok->TokenText == "__has_include" || Tok->TokenText == "__has_include_next")
                parseHasInclude();
        }
        return Type;
    }

    void resetTokenMetadata() {
        if (!CurrentToken) return;
        if (!CurrentToken->isTypeFinalized() &&
            !CurrentToken->isOneOf(TT::LambdaLSquare, TT::LambdaLBrace, TT::AttributeMacro, TT::IfMacro,
                                   TT::ForEachMacro, TT::FunctionLBrace, TT::ImplicitStringLiteral,
                                   TT::InlineASMBrace, TT::LambdaArrow, TT::UntouchableMacroFunc,
                                   TT::StatementAttributeLikeMacro, TT::FunctionLikeOrFreestandingMacro,
                                   TT::EnumLBrace, TT::RecordLBrace, TT::StructLBrace, TT::UnionLBrace,
                                   TT::BracedListLBrace)) {
            CurrentToken->setType(TT::Unknown);
        }
        CurrentToken->Role.reset();
        CurrentToken->MatchingParen = nullptr;
        CurrentToken->FakeLParens.clear();
        CurrentToken->FakeRParens = 0;
    }

    void next() {
        if (!CurrentToken) return;
        CurrentToken->NestingLevel = static_cast<unsigned>(Contexts.size() - 1);
        CurrentToken->BindingStrength = Contexts.back().BindingStrength;
        modifyContext(*CurrentToken);
        determineTokenType(*CurrentToken);
        CurrentToken = CurrentToken->Next;
        resetTokenMetadata();
    }

    void modifyContext(const FormatToken &Current) {
        auto AssignmentStartsExpression = [&]() {
            if (Current.getPrecedence() != prec::Assignment) return false;
            if (Line.First->is(tok::kw_return)) return false;
            return true;
        };
        if (AssignmentStartsExpression()) {
            Contexts.back().IsExpression = true;
            if (!Line.startsWith(TT::UnaryOperator)) {
                for (FormatToken *Previous = Current.Previous;
                     Previous && Previous->Previous && !Previous->Previous->isOneOf(tok::comma, tok::semi);
                     Previous = Previous->Previous) {
                    if (Previous->isOneOf(tok::r_square, tok::r_paren, tok::greater)) {
                        Previous = Previous->MatchingParen;
                        if (!Previous) break;
                    }
                    if (Previous->opensScope()) break;
                    if (Previous->isOneOf(TT::BinaryOperator, TT::UnaryOperator) && Previous->isPointerOrReference() &&
                        Previous->Previous && Previous->Previous->isNot(tok::equal)) {
                        Previous->setType(TT::PointerOrReference);
                    }
                }
            }
        } else if (Current.is(tok::lessless)) {
            Contexts.back().IsExpression = true;
        } else if (Current.is(tok::kw_return)) {
            Contexts.back().IsExpression = true;
        } else if (Current.is(TT::TrailingReturnArrow)) {
            Contexts.back().IsExpression = false;
        } else if (Current.is(TT::LambdaArrow) || (Current.is(tok::identifier) && Current.TokenText == "assert")) {
            Contexts.back().IsExpression = false;
        } else if (Current.Previous && Current.Previous->is(TT::CtorInitializerColon)) {
            Contexts.back().IsExpression = true;
            Contexts.back().ContextType = Context::CtorInitializer;
        } else if (Current.Previous && Current.Previous->is(TT::InheritanceColon)) {
            Contexts.back().ContextType = Context::InheritanceList;
        } else if (Current.isOneOf(tok::r_paren, tok::greater, tok::comma)) {
            for (FormatToken *Previous = Current.Previous; Previous && Previous->isOneOf(tok::star, tok::amp);
                 Previous = Previous->Previous) {
                Previous->setType(TT::PointerOrReference);
            }
            if (Line.MustBeDeclaration && Contexts.front().ContextType != Context::CtorInitializer)
                Contexts.back().IsExpression = false;
        } else if (Current.is(tok::semi) || Current.is(tok::exclaim)) {
            Contexts.back().IsExpression = true;
        }
    }

    static FormatToken *untilMatchingParen(FormatToken *Current) {
        int ParenLevel = 0;
        while (Current) {
            if (Current->is(tok::l_paren)) ++ParenLevel;
            if (Current->is(tok::r_paren)) --ParenLevel;
            if (ParenLevel < 1) break;
            Current = Current->Next;
        }
        return Current;
    }

    static bool isDeductionGuide(FormatToken &Current) {
        if (Current.Previous && Current.Previous->is(tok::r_paren) &&
            Current.startsSequence(tok::arrow, tok::identifier, tok::less)) {
            FormatToken *TemplateCloser = Current.Next->Next;
            int NestingLevel = 0;
            while (TemplateCloser) {
                if (TemplateCloser->is(tok::l_paren)) {
                    TemplateCloser = untilMatchingParen(TemplateCloser);
                    if (!TemplateCloser) break;
                }
                if (TemplateCloser->is(tok::less)) ++NestingLevel;
                if (TemplateCloser->is(tok::greater)) --NestingLevel;
                if (NestingLevel < 1) break;
                TemplateCloser = TemplateCloser->Next;
            }
            if (TemplateCloser && TemplateCloser->Next && TemplateCloser->Next->is(tok::semi) &&
                Current.Previous->MatchingParen) {
                FormatToken *LeadingIdentifier = Current.Previous->MatchingParen->Previous;
                return LeadingIdentifier && LeadingIdentifier->TokenText == Current.Next->TokenText;
            }
        }
        return false;
    }

    void determineTokenType(FormatToken &Current) {
        if (Current.isNot(TT::Unknown)) return;
        if (isStartOfName(Current) && (!Line.MightBeFunctionDecl || Current.NestingLevel != 0)) {
            Contexts.back().FirstStartOfName = &Current;
            Current.setType(TT::StartOfName);
        } else if (Current.is(tok::semi)) {
            Contexts.back().FirstStartOfName = nullptr;
        } else if (Current.isOneOf(tok::kw_auto, tok::kw___auto_type)) {
            AutoFound = true;
        } else if (Current.is(tok::arrow) && AutoFound && Line.MightBeFunctionDecl && Current.NestingLevel == 0 &&
                   !Current.Previous->is(tok::identifier)) {
            Current.setType(TT::TrailingReturnArrow);
        } else if (Current.is(tok::arrow) && Current.Previous && Current.Previous->is(tok::r_brace) &&
                   Current.Previous->is(BK_Block)) {
            Current.setType(TT::TrailingReturnArrow);
        } else if (isDeductionGuide(Current)) {
            Current.setType(TT::TrailingReturnArrow);
        } else if (Current.isPointerOrReference()) {
            Current.setType(determineStarAmpUsage(Current, Contexts.back().CanBeExpression && Contexts.back().IsExpression,
                                                  Contexts.back().ContextType == Context::TemplateArgument));
        } else if (Current.isOneOf(tok::minus, tok::plus, tok::caret)) {
            Current.setType(determinePlusMinusCaretUsage(Current));
            if (Current.is(TT::UnaryOperator) && Current.is(tok::caret)) Contexts.back().CaretFound = true;
        } else if (Current.isOneOf(tok::minusminus, tok::plusplus)) {
            Current.setType(determineIncrementUsage(Current));
        } else if (Current.isOneOf(tok::exclaim, tok::tilde)) {
            Current.setType(TT::UnaryOperator);
        } else if (Current.is(tok::question)) {
            Current.setType(TT::ConditionalExpr);
        } else if (Current.isBinaryOperator() && (!Current.Previous || Current.Previous->isNot(tok::l_square)) &&
                   Current.isNot(tok::greater)) {
            Current.setType(TT::BinaryOperator);
        } else if (Current.is(tok::comment)) {
            if (Current.TokenText.substr(0, 2) == "/*") {
                std::string_view T = Current.TokenText;
                if (T.size() >= 4 && T.substr(T.size() - 2) == "*/")
                    Current.setType(TT::BlockComment);
                else
                    Current.Kind = tok::unknown;
            } else {
                Current.setType(TT::LineComment);
            }
        } else if (Current.is(tok::r_paren)) {
            if (rParenEndsCast(Current)) Current.setType(TT::CastRParen);
            if (Current.MatchingParen && Current.Next && !Current.Next->isBinaryOperator() &&
                !Current.Next->isOneOf(tok::semi, tok::colon, tok::l_brace, tok::l_paren, tok::comma, tok::period,
                                       tok::arrow, tok::coloncolon)) {
                if (FormatToken *AfterParen = Current.MatchingParen->Next; AfterParen && AfterParen->isNot(tok::caret)) {
                    if (FormatToken *BeforeParen = Current.MatchingParen->Previous;
                        BeforeParen && BeforeParen->is(tok::identifier) && isAllUpperCase(BeforeParen->TokenText) &&
                        (!BeforeParen->Previous || BeforeParen->Previous->ClosesTemplateDeclaration)) {
                        Current.setType(TT::FunctionAnnotationRParen);
                    }
                }
            }
        } else if (Current.is(tok::period)) {
            FormatToken *PreviousNoComment = Current.getPreviousNonComment();
            if (PreviousNoComment && PreviousNoComment->isOneOf(tok::comma, tok::l_brace))
                Current.setType(TT::DesignatedInitializerPeriod);
        } else if (canBeObjCSelectorComponent(Current) && Current.Previous && Current.Previous->is(TT::CastRParen) &&
                   Current.Previous->MatchingParen && Current.Previous->MatchingParen->Previous &&
                   Current.Previous->MatchingParen->Previous->is(TT::ObjCMethodSpecifier)) {
            Current.setType(TT::SelectorName);
        } else if (Current.isOneOf(tok::identifier, tok::kw_const) && Current.Previous &&
                   !Current.Previous->isOneOf(tok::equal, tok::at, TT::CtorInitializerComma,
                                              TT::CtorInitializerColon) &&
                   Line.MightBeFunctionDecl && Contexts.size() == 1) {
            Current.setType(TT::TrailingAnnotation);
        }
    }

    bool isStartOfName(const FormatToken &Tok) {
        if (!Tok.Previous || Tok.isNot(tok::identifier) || Tok.is(TT::ClassHeadName)) return false;
        if (Tok.TokenText == "final" && Tok.Previous && Tok.Previous->is(TT::ClassHeadName)) return false;
        if (const FormatToken *NextNonComment = Tok.getNextNonComment();
            (!NextNonComment && !Line.InMacroBody) ||
            (NextNonComment &&
             (NextNonComment->isPointerOrReference() ||
              NextNonComment->isOneOf(TT::ClassHeadName, tok::string_literal) ||
              (Line.InPragmaDirective && NextNonComment->is(tok::identifier))))) {
            return false;
        }
        if (Tok.Previous->is(tok::identifier) &&
            (Tok.Previous->TokenText == "instanceof" || Tok.Previous->TokenText == "as"))
            return false;
        FormatToken *PreviousNotConst = Tok.getPreviousNonComment();
        while (PreviousNotConst && PreviousNotConst->is(tok::kw_const))
            PreviousNotConst = PreviousNotConst->getPreviousNonComment();
        if (!PreviousNotConst) return false;
        bool IsPPKeyword = PreviousNotConst->is(tok::identifier) && PreviousNotConst->Previous &&
                           PreviousNotConst->Previous->is(tok::hash);
        if (PreviousNotConst->is(TT::TemplateCloser)) {
            return PreviousNotConst->MatchingParen && PreviousNotConst->MatchingParen->Previous &&
                   PreviousNotConst->MatchingParen->Previous->isNot(tok::period);
        }
        if ((PreviousNotConst->is(tok::r_paren) && PreviousNotConst->is(TT::TypeDeclarationParen)) ||
            PreviousNotConst->is(TT::AttributeRParen)) {
            return true;
        }
        if (IsPPKeyword) return false;
        if (PreviousNotConst->isOneOf(tok::identifier, tok::kw_auto) &&
            PreviousNotConst->isNot(TT::StatementAttributeLikeMacro)) {
            return true;
        }
        if (PreviousNotConst->is(TT::PointerOrReference)) return true;
        if (PreviousNotConst->isTypeName()) return true;
        return false;
    }

    bool rParenEndsCast(const FormatToken &Tok) {
        if (!Tok.MatchingParen || !Tok.Previous) return false;
        const FormatToken *LParen = Tok.MatchingParen;
        const FormatToken *BeforeRParen = Tok.Previous;
        const FormatToken *AfterRParen = Tok.Next;
        if (BeforeRParen == LParen || !AfterRParen) return false;
        FormatToken *LeftOfParens = LParen->getPreviousNonComment();
        if (LeftOfParens) {
            if (LeftOfParens->is(tok::r_paren) && LeftOfParens->isNot(TT::CastRParen)) {
                if (!LeftOfParens->MatchingParen || !LeftOfParens->MatchingParen->Previous) return false;
                LeftOfParens = LeftOfParens->MatchingParen->Previous;
            }
            if (LeftOfParens->HasIdentifierInfo &&
                !(LeftOfParens->is(tok::kw_return) || LeftOfParens->is(tok::kw_case) ||
                  (LeftOfParens->is(tok::identifier) && LeftOfParens->TokenText == "in"))) {
                return false;
            }
            if (LeftOfParens->isOneOf(tok::at, tok::r_square, TT::TemplateCloser, tok::ellipsis)) return false;
        }
        if (AfterRParen->is(tok::question) || (AfterRParen->is(tok::ampamp) && !BeforeRParen->isTypeName()))
            return false;
        if (AfterRParen->isOneOf(tok::kw_volatile, tok::kw_const, tok::arrow) ||
            (AfterRParen->is(tok::identifier) &&
             (AfterRParen->TokenText == "override" || AfterRParen->TokenText == "final")) ||
            isCppAttribute(*AfterRParen)) {
            return false;
        }
        if (AfterRParen->isOneOf(tok::kw_sizeof, tok::kw_alignof) ||
            (AfterRParen->isLiteral() && AfterRParen->isNot(tok::string_literal))) {
            return true;
        }
        auto IsNonVariableTemplate = [](const FormatToken &T) {
            if (T.isNot(TT::TemplateCloser)) return false;
            const FormatToken *Less = T.MatchingParen;
            if (!Less) return false;
            const FormatToken *BeforeLess = Less->getPreviousNonComment();
            return BeforeLess != nullptr;
        };
        auto IsQualifiedPointerOrReference = [](const FormatToken *T) {
            while (T) {
                if (T->is(TT::AttributeRParen)) {
                    if (const FormatToken *Tk = T->MatchingParen ? T->MatchingParen->Previous : nullptr;
                        Tk && Tk->isAttribute()) {
                        T = Tk->Previous;
                        continue;
                    }
                } else if (T->is(TT::AttributeSquare)) {
                    if (T->MatchingParen && T->MatchingParen->Previous) {
                        T = T->MatchingParen->Previous;
                        continue;
                    }
                } else if (T->canBePointerOrReferenceQualifier()) {
                    T = T->Previous;
                    continue;
                }
                break;
            }
            return T && T->is(TT::PointerOrReference);
        };
        bool ParensAreType = IsNonVariableTemplate(*BeforeRParen) || BeforeRParen->is(TT::TypeDeclarationParen) ||
                             BeforeRParen->isTypeName() ||
                             (!BeforeRParen->isTypeName() && IsQualifiedPointerOrReference(BeforeRParen));
        bool ParensCouldEndDecl = AfterRParen->isOneOf(tok::equal, tok::semi, tok::l_brace, tok::greater);
        if (ParensAreType && !ParensCouldEndDecl) return true;
        if (!LeftOfParens) return false;
        for (const FormatToken *Token = LParen->Next; Token != &Tok; Token = Token->Next)
            if (Token->is(TT::BinaryOperator)) return false;
        if (AfterRParen->is(tok::identifier)) return true;
        if (AfterRParen->is(tok::l_paren)) {
            for (const FormatToken *Prev = BeforeRParen; Prev->is(tok::identifier);) {
                Prev = Prev->Previous;
                if (Prev->is(tok::coloncolon)) Prev = Prev->Previous;
                if (Prev == LParen) return true;
            }
        }
        if (!AfterRParen->Next) return false;
        if (AfterRParen->is(tok::l_brace) && AfterRParen->BlockKind == BK_BracedInit) return true;
        const bool NextIsAmpOrStar = AfterRParen->isOneOf(tok::amp, tok::star);
        if (!(AfterRParen->isUnaryOperator() || NextIsAmpOrStar) || AfterRParen->is(tok::plus) ||
            !AfterRParen->Next->isOneOf(tok::identifier, tok::numeric_constant)) {
            return false;
        }
        if (NextIsAmpOrStar && (AfterRParen->Next->is(tok::numeric_constant) || Line.InPPDirective)) return false;
        if (Line.InPPDirective && AfterRParen->is(tok::minus)) return false;
        const FormatToken *Prev = BeforeRParen;
        if (Prev->is(tok::r_paren)) {
            if (Prev->is(TT::CastRParen)) return false;
            Prev = Prev->MatchingParen;
            if (!Prev) return false;
            Prev = Prev->Previous;
            if (!Prev || Prev->isNot(tok::r_paren)) return false;
            Prev = Prev->MatchingParen;
            return Prev && Prev->is(TT::FunctionTypeLParen);
        }
        for (Prev = BeforeRParen; Prev != LParen; Prev = Prev->Previous)
            if (!Prev->isOneOf(tok::kw_const, tok::identifier, tok::coloncolon)) return false;
        return true;
    }

    bool determineUnaryOperatorByUsage(const FormatToken &Tok) {
        const FormatToken *PrevToken = Tok.getPreviousNonComment();
        if (!PrevToken) return true;
        if (PrevToken->isOneOf(TT::ConditionalExpr, tok::l_paren, tok::comma, tok::colon, tok::semi, tok::equal,
                               tok::question, tok::l_square, tok::l_brace, tok::kw_case, tok::kw_return)) {
            return true;
        }
        if (PrevToken->is(tok::kw_sizeof)) return true;
        if (PrevToken->isOneOf(TT::CastRParen, TT::UnaryOperator)) return true;
        if (PrevToken->is(TT::BinaryOperator)) return true;
        return false;
    }

    TT determineStarAmpUsage(const FormatToken &Tok, bool IsExpression, bool InTemplateArgument) {
        const FormatToken *PrevToken = Tok.getPreviousNonComment();
        if (!PrevToken) return TT::UnaryOperator;
        if (PrevToken->isTypeName()) return TT::PointerOrReference;
        FormatToken *NextToken = Tok.getNextNonComment();
        if (!NextToken) return TT::PointerOrReference;
        if (NextToken->is(tok::greater)) {
            NextToken->setFinalizedType(TT::TemplateCloser);
            return TT::PointerOrReference;
        }
        if (NextToken->isOneOf(tok::arrow, tok::equal, tok::comma, tok::r_paren) ||
            NextToken->canBePointerOrReferenceQualifier() ||
            (NextToken->is(tok::l_brace) && !NextToken->getNextNonComment())) {
            return TT::PointerOrReference;
        }
        if (PrevToken->is(tok::coloncolon)) return TT::PointerOrReference;
        if (PrevToken->is(tok::r_paren) && PrevToken->is(TT::TypeDeclarationParen)) return TT::PointerOrReference;
        if (determineUnaryOperatorByUsage(Tok)) return TT::UnaryOperator;
        if (NextToken->is(tok::l_square) && NextToken->isNot(TT::LambdaLSquare)) return TT::PointerOrReference;
        if (NextToken->isOneOf(tok::comma, tok::semi)) return TT::PointerOrReference;
        if (PrevToken->is(tok::r_brace) && Tok.is(tok::star) && !PrevToken->MatchingParen)
            return TT::PointerOrReference;
        if (PrevToken->isLiteral() || PrevToken->isOneOf(tok::r_paren, tok::r_square, tok::kw_true, tok::kw_false,
                                                         tok::r_brace)) {
            return TT::BinaryOperator;
        }
        const FormatToken *NextNonParen = NextToken;
        while (NextNonParen && NextNonParen->is(tok::l_paren)) NextNonParen = NextNonParen->getNextNonComment();
        if (NextNonParen && (NextNonParen->isLiteral() || NextNonParen->isOneOf(tok::kw_true, tok::kw_false) ||
                             NextNonParen->isUnaryOperator())) {
            return TT::BinaryOperator;
        }
        if (InTemplateArgument && NextToken->isAnyIdentifier()) return TT::BinaryOperator;
        if (Tok.is(tok::ampamp) && NextToken->isOneOf(tok::l_paren, tok::star, tok::amp)) return TT::BinaryOperator;
        if (NextToken->isAnyIdentifier()) {
            FormatToken *NextNextToken = NextToken->getNextNonComment();
            if (NextNextToken) {
                if (NextNextToken->is(tok::arrow)) return TT::BinaryOperator;
                if (NextNextToken->isPointerOrReference()) {
                    NextNextToken->setFinalizedType(TT::BinaryOperator);
                    return TT::BinaryOperator;
                }
            }
        }
        if (IsExpression && !Contexts.back().CaretFound) return TT::BinaryOperator;
        if (!Scopes.empty() && Scopes.back() == ST_Class) return TT::PointerOrReference;
        auto IsChainedOperatorAmpOrMember = [](const FormatToken *token) {
            return !token || token->isOneOf(tok::amp, tok::period, tok::arrow);
        };
        if (Tok.is(tok::amp) && PrevToken->isAnyIdentifier() &&
            IsChainedOperatorAmpOrMember(PrevToken->getPreviousNonComment()) && NextToken &&
            NextToken->isAnyIdentifier()) {
            if (FormatToken *NextNext = NextToken->getNextNonComment();
                NextNext && (IsChainedOperatorAmpOrMember(NextNext) || NextNext->is(tok::semi))) {
                return TT::BinaryOperator;
            }
        }
        return TT::PointerOrReference;
    }

    TT determinePlusMinusCaretUsage(const FormatToken &Tok) {
        if (determineUnaryOperatorByUsage(Tok)) return TT::UnaryOperator;
        const FormatToken *PrevToken = Tok.getPreviousNonComment();
        if (!PrevToken) return TT::UnaryOperator;
        if (PrevToken->is(tok::at)) return TT::UnaryOperator;
        return TT::BinaryOperator;
    }

    TT determineIncrementUsage(const FormatToken &Tok) {
        const FormatToken *PrevToken = Tok.getPreviousNonComment();
        if (!PrevToken || PrevToken->is(TT::CastRParen)) return TT::UnaryOperator;
        if (PrevToken->isOneOf(tok::r_paren, tok::r_square, tok::identifier)) return TT::TrailingUnaryOperator;
        return TT::UnaryOperator;
    }

    std::vector<Context> Contexts;
    const Style &S;
    AnnotatedLine &Line;
    FormatToken *CurrentToken;
    bool AutoFound = false;
    std::vector<ScopeType> &Scopes;
    std::unordered_set<FormatToken *> NonTemplateLess;
};

class ExpressionParser {
public:
    explicit ExpressionParser(AnnotatedLine &Line) : Current(Line.First) {}

    void parse(int Precedence = 0) {
        while (Current && (Current->is(tok::kw_return) ||
                           (Current->is(tok::colon) && Current->isOneOf(TT::ObjCMethodExpr, TT::DictLiteral)))) {
            next();
        }
        if (!Current || Precedence > PrecedenceArrowAndPeriod) return;
        if (Precedence == prec::Conditional) {
            parseConditionalExpr();
            return;
        }
        if (Precedence == PrecedenceUnaryOperator) {
            parseUnaryOperator();
            return;
        }
        FormatToken *Start = Current;
        FormatToken *LatestOperator = nullptr;
        unsigned OperatorIndex = 0;
        while (Current) {
            parse(Precedence + 1);
            int CurrentPrecedence = getCurrentPrecedence();
            if (Precedence == CurrentPrecedence && Current && Current->is(TT::SelectorName)) {
                if (LatestOperator) addFakeParenthesis(Start, prec::Level(Precedence));
                Start = Current;
            }
            if (!Current || (Current->closesScope() && Current->MatchingParen) ||
                (CurrentPrecedence != -1 && CurrentPrecedence < Precedence) ||
                (CurrentPrecedence == prec::Conditional && Precedence == prec::Assignment &&
                 Current->is(tok::colon))) {
                break;
            }
            if (Current->opensScope()) {
                while (Current && (!Current->closesScope() || Current->opensScope())) {
                    next();
                    parse();
                }
                next();
            } else {
                if (CurrentPrecedence == Precedence) {
                    if (LatestOperator) LatestOperator->NextOperator = Current;
                    LatestOperator = Current;
                    Current->OperatorIndex = OperatorIndex;
                    ++OperatorIndex;
                }
                next(/*SkipPastLeadingComments=*/Precedence > 0);
            }
        }
        if (LatestOperator && (Current || Precedence > 0)) {
            if (Precedence == PrecedenceArrowAndPeriod)
                addFakeParenthesis(Start, prec::Unknown);
            else
                addFakeParenthesis(Start, prec::Level(Precedence));
        }
    }

private:
    int getCurrentPrecedence() {
        if (Current) {
            const FormatToken *NextNonComment = Current->getNextNonComment();
            if (Current->is(TT::ConditionalExpr)) return prec::Conditional;
            if (NextNonComment && Current->is(TT::SelectorName) && NextNonComment->is(TT::DictLiteral))
                return prec::Assignment;
            if (Current->is(TT::LambdaArrow)) return prec::Comma;
            if (Current->isOneOf(tok::semi, TT::InlineASMColon, TT::SelectorName) ||
                (Current->is(tok::comment) && NextNonComment && NextNonComment->is(TT::SelectorName))) {
                return 0;
            }
            if (Current->is(TT::RangeBasedForLoopColon)) return prec::Comma;
            if (Current->is(TT::BinaryOperator) || Current->is(tok::comma)) return Current->getPrecedence();
            if (Current->isOneOf(tok::period, tok::arrow) && Current->isNot(TT::TrailingReturnArrow))
                return PrecedenceArrowAndPeriod;
        }
        return -1;
    }

    void addFakeParenthesis(FormatToken *Start, prec::Level Precedence) {
        Start->FakeLParens.push_back(Precedence);
        if (Precedence > prec::Unknown) Start->StartsBinaryExpression = true;
        FormatToken *End = Current ? Current->getPreviousNonComment() : nullptr;
        if (End) {
            ++End->FakeRParens;
            if (Precedence > prec::Unknown) End->EndsBinaryExpression = true;
        }
    }

    void parseUnaryOperator() {
        std::vector<FormatToken *> Tokens;
        while (Current && Current->is(TT::UnaryOperator)) {
            Tokens.push_back(Current);
            next();
        }
        parse(PrecedenceArrowAndPeriod);
        for (auto It = Tokens.rbegin(); It != Tokens.rend(); ++It) addFakeParenthesis(*It, prec::Unknown);
    }

    void parseConditionalExpr() {
        while (Current && Current->isTrailingComment()) next();
        FormatToken *Start = Current;
        parse(prec::LogicalOr);
        if (!Current || Current->isNot(tok::question)) return;
        next();
        parse(prec::Assignment);
        if (!Current || Current->isNot(TT::ConditionalExpr)) return;
        next();
        parse(prec::Assignment);
        addFakeParenthesis(Start, prec::Conditional);
    }

    void next(bool SkipPastLeadingComments = true) {
        if (Current) Current = Current->Next;
        while (Current && (Current->NewlinesBefore == 0 || SkipPastLeadingComments) && Current->isTrailingComment())
            Current = Current->Next;
    }

    FormatToken *Current;
};

unsigned maxNestingDepth(const AnnotatedLine &Line) {
    unsigned Result = 0;
    for (const FormatToken *Tok = Line.First; Tok; Tok = Tok->Next) Result = std::max(Result, Tok->NestingLevel);
    return Result;
}

FormatToken *getFunctionName(const AnnotatedLine &Line, FormatToken *&OpeningParen) {
    for (FormatToken *Tok = Line.getFirstNonComment(), *Name = nullptr; Tok; Tok = Tok->getNextNonComment()) {
        if (Tok->is(tok::l_square) && Tok->is(TT::AttributeSquare)) {
            Tok = Tok->MatchingParen;
            if (!Tok) break;
            continue;
        }
        if (Name) {
            if (Tok->is(tok::l_paren) && Tok->is(TT::Unknown) && Tok->MatchingParen) {
                OpeningParen = Tok;
                return Name;
            }
            return nullptr;
        }
        if (Tok->isOneOf(tok::kw_inline, tok::kw_constexpr)) continue;
        if (Tok->is(tok::coloncolon)) {
            Tok = Tok->Next;
            if (!Tok) break;
        }
        while (Tok->startsSequence(tok::identifier, tok::coloncolon)) {
            Tok = Tok->Next->Next;
            if (!Tok) return nullptr;
        }
        if (Tok->is(tok::tilde)) {
            Tok = Tok->Next;
            if (!Tok) break;
        }
        if (Tok->isNot(tok::identifier) || Tok->isNot(TT::Unknown)) break;
        Name = Tok;
    }
    return nullptr;
}

bool isCtorOrDtorName(const FormatToken *Tok) {
    const FormatToken *Prev = Tok->Previous;
    if (Prev && Prev->is(tok::tilde)) Prev = Prev->Previous;
    if (!Prev || !Prev->endsSequence(tok::coloncolon, tok::identifier)) return false;
    return Prev->Previous->TokenText == Tok->TokenText;
}

bool isFunctionDeclarationName(const FormatToken &Current, const AnnotatedLine &Line, FormatToken *&ClosingParen) {
    if (Current.is(TT::FunctionDeclarationName)) return true;
    if (!Current.is(tok::identifier)) return false;
    const FormatToken *Next = Current.Next;
    if (Current.isNot(TT::StartOfName) || Current.NestingLevel != 0) return false;
    while (Next && Next->startsSequence(tok::hashhash, tok::identifier)) Next = Next->Next->Next;
    for (; Next; Next = Next->Next) {
        if (Next->is(TT::TemplateOpener) && Next->MatchingParen) {
            Next = Next->MatchingParen;
        } else if (Next->is(tok::coloncolon)) {
            Next = Next->Next;
            if (!Next) return false;
            if (Next->isNot(tok::identifier)) return false;
        } else if (isCppAttribute(*Next)) {
            Next = Next->MatchingParen;
            if (!Next) return false;
        } else if (Next->is(tok::l_paren)) {
            break;
        } else {
            return false;
        }
    }
    if (!Next || Next->isNot(tok::l_paren) || !Next->MatchingParen) return false;
    ClosingParen = Next->MatchingParen;
    if (Line.Last->is(tok::l_brace)) return true;
    if (Next->Next == ClosingParen) return true;
    if (ClosingParen->Next && ClosingParen->Next->is(TT::PointerOrReference)) return true;
    if (Next->Next && Next->Next->is(tok::identifier) && !Line.endsWith(tok::semi)) return true;
    for (const FormatToken *Tok = Next->Next; Tok && Tok != ClosingParen; Tok = Tok->Next) {
        if (Tok->is(TT::TypeDeclarationParen)) return true;
        if (Tok->isOneOf(tok::l_paren, TT::TemplateOpener) && Tok->MatchingParen) {
            Tok = Tok->MatchingParen;
            continue;
        }
        if (Tok->is(tok::kw_const) || Tok->isTypeName() ||
            Tok->isOneOf(TT::PointerOrReference, TT::StartOfName, tok::ellipsis)) {
            return true;
        }
        if (Tok->isOneOf(tok::l_brace, TT::ObjCMethodExpr) || Tok->isLiteral()) return false;
    }
    return false;
}

bool isAllmanBrace(const FormatToken &Tok) {
    return Tok.is(tok::l_brace) && Tok.is(BK_Block) && !Tok.isOneOf(TT::ObjCBlockLBrace, TT::LambdaLBrace, TT::DictLiteral);
}

}  // namespace

// --- TokenAnnotator ------------------------------------------------------------------

void TokenAnnotator::setCommentLineLevels(std::vector<AnnotatedLine *> &Lines) const {
    const AnnotatedLine *NextNonCommentLine = nullptr;
    for (auto It = Lines.rbegin(); It != Lines.rend(); ++It) {
        AnnotatedLine *Line = *It;
        bool IsClangFormatOff = Line->First->TokenText == "/* clang-format off */" ||
                                Line->First->TokenText.substr(0, 18) == "// clang-format off";
        if (NextNonCommentLine && NextNonCommentLine->First->NewlinesBefore < 2 && Line->isComment() &&
            !IsClangFormatOff && NextNonCommentLine->First->OriginalColumn == Line->First->OriginalColumn) {
            const bool PPDirectiveOrImportStmt = NextNonCommentLine->Type == LT_PreprocessorDirective ||
                                                 NextNonCommentLine->Type == LT_ImportStatement;
            if (PPDirectiveOrImportStmt) Line->Type = LT_CommentAbovePPDirective;
            Line->Level = PPDirectiveOrImportStmt ? 0 : NextNonCommentLine->Level;
        } else {
            NextNonCommentLine = Line->First->isNot(tok::r_brace) ? Line : nullptr;
        }
        setCommentLineLevels(Line->Children);
    }
}

void TokenAnnotator::annotate(AnnotatedLine &Line) {
    if (!Line.InMacroBody) MacroBodyScopes.clear();
    auto &ScopeStack = Line.InMacroBody ? MacroBodyScopes : Scopes;
    AnnotatingParser Parser(Style_, Line, ScopeStack);
    Line.Type = Parser.parseLine();
    if (!Line.Children.empty()) {
        ScopeStack.push_back(ST_Other);
        for (AnnotatedLine *Child : Line.Children) annotate(*Child);
        if (!ScopeStack.empty()) ScopeStack.pop_back();
    }
    if (maxNestingDepth(Line) > 50) Line.Type = LT_Invalid;
    if (Line.Type == LT_Invalid) return;
    ExpressionParser ExprParser(Line);
    ExprParser.parse();
    {
        FormatToken *OpeningParen = nullptr;
        FormatToken *Tok = getFunctionName(Line, OpeningParen);
        if (Tok && ((!ScopeStack.empty() && ScopeStack.back() == ST_Class) || Line.endsWith(TT::FunctionLBrace) ||
                    isCtorOrDtorName(Tok))) {
            Tok->setFinalizedType(TT::CtorDtorDeclName);
            OpeningParen->setFinalizedType(TT::FunctionDeclarationLParen);
        }
    }
    if (Line.startsWith(TT::ObjCMethodSpecifier)) Line.Type = LT_ObjCMethodDecl;
    FormatToken *First = Line.First;
    First->SpacesRequiredBefore = 1;
    First->CanBreakBefore = First->MustBreakBefore;
}

void TokenAnnotator::calculateFormattingInformation(AnnotatedLine &Line) const {
    if (Line.Computed) return;
    Line.Computed = true;
    for (AnnotatedLine *ChildLine : Line.Children) calculateFormattingInformation(*ChildLine);
    FormatToken *First = Line.First;
    First->TotalLength = First->IsMultiline ? Style_.ColumnLimit : Line.FirstStartColumn + First->ColumnWidth;
    const FormatToken *FirstNonComment = Line.getFirstNonComment();
    bool SeenName = false;
    bool LineIsFunctionDeclaration = false;
    FormatToken *AfterLastAttribute = nullptr;
    FormatToken *ClosingParen = nullptr;
    for (FormatToken *Tok = FirstNonComment ? FirstNonComment->Next : nullptr; Tok && Tok->isNot(BK_BracedInit);
         Tok = Tok->Next) {
        if (Tok->is(TT::StartOfName)) SeenName = true;
        if (Tok->Previous->EndsCppAttributeGroup) AfterLastAttribute = Tok;
        if (const bool IsCtorOrDtor = Tok->is(TT::CtorDtorDeclName);
            IsCtorOrDtor || isFunctionDeclarationName(*Tok, Line, ClosingParen)) {
            if (!IsCtorOrDtor) Tok->setFinalizedType(TT::FunctionDeclarationName);
            LineIsFunctionDeclaration = true;
            SeenName = true;
            if (ClosingParen) {
                FormatToken *OpeningParen = ClosingParen->MatchingParen;
                if (OpeningParen->is(TT::Unknown)) OpeningParen->setType(TT::FunctionDeclarationLParen);
            }
            break;
        }
    }
    if ((LineIsFunctionDeclaration || (FirstNonComment && FirstNonComment->is(TT::CtorDtorDeclName))) &&
        Line.endsWith(tok::semi, tok::r_brace)) {
        FormatToken *Tok = Line.Last->Previous;
        while (Tok->isNot(tok::r_brace)) Tok = Tok->Previous;
        if (FormatToken *LBrace = Tok->MatchingParen; LBrace && LBrace->is(TT::Unknown)) {
            Tok->BlockKind = BK_Block;
            LBrace->BlockKind = BK_Block;
            LBrace->setFinalizedType(TT::FunctionLBrace);
        }
    }
    if (SeenName && AfterLastAttribute && mustBreakAfterAttributes(*AfterLastAttribute)) {
        AfterLastAttribute->MustBreakBefore = true;
        if (LineIsFunctionDeclaration) Line.ReturnTypeWrapped = true;
    }
    if (LineIsFunctionDeclaration && ClosingParen) {
        for (FormatToken *Tok = ClosingParen->Next; Tok; Tok = Tok->Next) {
            if (Tok->is(TT::CtorInitializerColon)) break;
            if (Tok->is(tok::arrow)) {
                Tok->setType(TT::TrailingReturnArrow);
                break;
            }
            if (Tok->isNot(TT::TrailingAnnotation)) continue;
            const FormatToken *Next = Tok->Next;
            if (!Next || Next->isNot(tok::l_paren)) continue;
            Tok = Next->MatchingParen;
            if (!Tok) break;
        }
    }
    if (First->is(TT::ElseLBrace)) {
        First->CanBreakBefore = true;
        First->MustBreakBefore = true;
    }
    bool InFunctionDecl = Line.MightBeFunctionDecl;
    for (FormatToken *Current = First->Next; Current; Current = Current->Next) {
        const FormatToken *Prev = Current->Previous;
        if (Current->is(TT::LineComment)) {
            if (Prev->is(BK_BracedInit) && Prev->opensScope())
                Current->SpacesRequiredBefore = 0;
            else
                Current->SpacesRequiredBefore = Style_.SpacesBeforeTrailingComments;
            if (!Current->HasUnescapedNewline) {
                for (FormatToken *Parameter = Current->Previous; Parameter; Parameter = Parameter->Previous) {
                    if (Parameter->isOneOf(tok::comment, tok::r_brace)) break;
                    if (Parameter->Previous && Parameter->Previous->is(tok::comma)) {
                        if (Parameter->Previous->isNot(TT::CtorInitializerComma) && Parameter->HasUnescapedNewline)
                            Parameter->MustBreakBefore = true;
                        break;
                    }
                }
            }
        } else if (!Current->Finalized && Current->SpacesRequiredBefore == 0 && spaceRequiredBefore(Line, *Current)) {
            Current->SpacesRequiredBefore = 1;
        }
        const auto &Children = Prev->Children;
        if (!Children.empty() && Children.back()->Last->is(TT::LineComment)) {
            Current->MustBreakBefore = true;
        } else {
            Current->MustBreakBefore = Current->MustBreakBefore || mustBreakBefore(Line, *Current);
        }
        Current->CanBreakBefore = Current->MustBreakBefore || canBreakBefore(Line, *Current);
        unsigned ChildSize = 0;
        if (Prev->Children.size() == 1) {
            FormatToken &LastOfChild = *Prev->Children[0]->Last;
            ChildSize = LastOfChild.isTrailingComment() ? Style_.ColumnLimit : LastOfChild.TotalLength + 1;
        }
        if (Current->MustBreakBefore || Prev->Children.size() > 1 ||
            (Prev->Children.size() == 1 && Prev->Children[0]->First->MustBreakBefore) || Current->IsMultiline) {
            Current->TotalLength = Prev->TotalLength + Style_.ColumnLimit;
        } else {
            Current->TotalLength =
                Prev->TotalLength + Current->ColumnWidth + ChildSize + Current->SpacesRequiredBefore;
        }
        if (Current->is(TT::CtorInitializerColon)) InFunctionDecl = false;
        Current->SplitPenalty = splitPenalty(Line, *Current, InFunctionDecl);
        Current->SplitPenalty += 20 * Current->BindingStrength;
    }
    calculateUnbreakableTailLengths(Line);
    unsigned IndentLevel = Line.Level;
    for (FormatToken *Current = First; Current; Current = Current->Next) {
        if (Current->Role) Current->Role->precomputeFormattingInfos(Current);
        if (Current->MatchingParen && Current->MatchingParen->opensBlockOrBlockTypeList() && IndentLevel > 0)
            --IndentLevel;
        Current->IndentLevel = IndentLevel;
        if (Current->opensBlockOrBlockTypeList()) ++IndentLevel;
    }
}

void TokenAnnotator::calculateUnbreakableTailLengths(AnnotatedLine &Line) const {
    unsigned UnbreakableTailLength = 0;
    for (FormatToken *Current = Line.Last; Current; Current = Current->Previous) {
        Current->UnbreakableTailLength = UnbreakableTailLength;
        if (Current->CanBreakBefore || Current->isOneOf(tok::comment, tok::string_literal))
            UnbreakableTailLength = 0;
        else
            UnbreakableTailLength += Current->ColumnWidth + Current->SpacesRequiredBefore;
    }
}

unsigned TokenAnnotator::splitPenalty(const AnnotatedLine &Line, const FormatToken &Tok, bool InFunctionDecl) const {
    const FormatToken &Left = *Tok.Previous;
    const FormatToken &Right = Tok;
    if (Left.is(tok::semi)) return 0;
    if (Right.is(tok::identifier) && Right.Next && Right.Next->is(TT::DictLiteral)) return 1;
    if (Right.is(tok::l_square)) {
        if (Left.is(tok::r_square)) return 200;
        if (Right.is(TT::LambdaLSquare) && Left.is(tok::equal)) return 35;
        if (!Right.isOneOf(TT::ObjCMethodExpr, TT::LambdaLSquare, TT::ArrayInitializerLSquare,
                           TT::DesignatedInitializerLSquare, TT::AttributeSquare)) {
            return 500;
        }
    }
    if (Left.is(tok::coloncolon)) return Style_.PenaltyBreakScopeResolution;
    if (Right.isOneOf(TT::StartOfName, TT::FunctionDeclarationName)) {
        if (Line.startsWith(tok::kw_for) && Right.PartOfMultiVariableDeclStmt) return 3;
        if (Left.is(TT::StartOfName)) return 110;
        if (InFunctionDecl && Right.NestingLevel == 0) return Style_.PenaltyReturnTypeOnItsOwnLine;
        return 200;
    }
    if (Right.is(TT::PointerOrReference)) return 190;
    if (Right.is(TT::LambdaArrow)) return 110;
    if (Left.is(tok::equal) && Right.is(tok::l_brace)) return 160;
    if (Left.is(TT::CastRParen)) return 100;
    if (Left.isOneOf(tok::kw_struct, tok::kw_union)) return 5000;
    if (Left.is(tok::comment)) return 1000;
    if (Left.isOneOf(TT::RangeBasedForLoopColon, TT::InheritanceColon, TT::CtorInitializerColon)) return 2;
    if (Right.isMemberAccess()) {
        const FormatToken *NextOperator = Right.NextOperator;
        const unsigned Penalty = Style_.PenaltyBreakBeforeMemberAccess;
        return NextOperator && NextOperator->Previous->closesScope() ? std::min(Penalty, 35u) : Penalty;
    }
    if (Right.is(TT::TrailingAnnotation) && (!Right.Next || Right.Next->isNot(tok::l_paren))) {
        if (Line.startsWith(TT::ObjCMethodSpecifier)) return 10;
        bool is_short_annotation = Right.TokenText.size() < 10;
        return (Left.is(tok::r_paren) ? 100 : 120) + (is_short_annotation ? 50 : 0);
    }
    if (Line.startsWith(tok::kw_for) && Left.is(tok::equal)) return 4;
    if (Right.is(TT::SelectorName)) return 0;
    if (Left.is(tok::colon) && Left.is(TT::ObjCMethodExpr)) return Line.MightBeFunctionDecl ? 50 : 500;
    if (Left.is(tok::l_paren) && Style_.PenaltyBreakOpenParenthesis != 0) return Style_.PenaltyBreakOpenParenthesis;
    if (Left.is(tok::l_paren) && InFunctionDecl) return 100;
    if (Left.is(tok::l_paren) && Left.Previous &&
        (Left.Previous->isOneOf(tok::kw_for, tok::kw__Generic) || Left.Previous->isIf())) {
        return 1000;
    }
    if (Left.is(tok::equal) && InFunctionDecl) return 110;
    if (Right.is(tok::r_brace)) return 1;
    if (Left.is(TT::TemplateOpener)) return 100;
    if (Left.opensScope()) {
        if (Left.is(tok::l_brace)) return Left.ParameterCount > 1 ? Style_.PenaltyBreakBeforeFirstCallParameter : 19;
        return Left.ParameterCount > 1 ? Style_.PenaltyBreakBeforeFirstCallParameter : 19;
    }
    if (Left.is(TT::UnaryOperator)) return 60;
    if (Left.isOneOf(tok::plus, tok::comma) && Left.Previous && Left.Previous->isLabelString() &&
        (Left.NextOperator || Left.OperatorIndex != 0)) {
        return 50;
    }
    if (Right.is(tok::plus) && Left.isLabelString() && (Right.NextOperator || Right.OperatorIndex != 0)) return 25;
    if (Left.is(tok::comma)) return 1;
    if (Right.is(tok::lessless) && Left.isLabelString() && (Right.NextOperator || Right.OperatorIndex != 1)) return 25;
    if (Right.is(tok::lessless)) {
        if (Left.isNot(tok::r_paren) || Right.OperatorIndex > 0) return 2;
        return 1;
    }
    if (Left.ClosesTemplateDeclaration) return Style_.PenaltyBreakTemplateDeclaration;
    if (Left.is(TT::ConditionalExpr)) return prec::Conditional;
    prec::Level Level = Left.getPrecedence();
    if (Level == prec::Unknown) Level = Right.getPrecedence();
    if (Level == prec::Assignment) return Style_.PenaltyBreakAssignment;
    if (Level != prec::Unknown) return static_cast<unsigned>(Level);
    return 3;
}

bool TokenAnnotator::spaceRequiredBeforeParens(const FormatToken &Right) const {
    if (Style_.SpaceBeforeParens == Style::SBPO_Always) return true;
    (void)Right;
    return false;
}

bool TokenAnnotator::spaceRequiredBetween(const AnnotatedLine &Line, const FormatToken &Left,
                                          const FormatToken &Right) const {
    if (Left.is(tok::kw_return) && !Right.isOneOf(tok::semi, tok::r_paren, tok::hashhash)) return true;
    if (Right.is(tok::hashhash)) return Left.is(tok::hash);
    if (Left.isOneOf(tok::hashhash, tok::hash)) return Right.is(tok::hash);
    if (Left.is(BK_Block) && Right.is(tok::r_brace) && Right.MatchingParen == &Left && Line.Children.empty())
        return false;  // SpaceInEmptyBlock
    if (Left.is(tok::kw_auto) && Right.isOneOf(TT::LambdaLBrace, TT::FunctionLBrace, TT::FunctionTypeLParen))
        return true;
    if (Left.is(tok::kw_auto) && Right.isOneOf(tok::l_paren, tok::l_brace)) return false;
    const FormatToken *BeforeLeft = Left.Previous;
    if (Left.is(tok::l_paren) || Right.is(tok::r_paren)) return false;  // SpacesInParens: Never
    if (Right.isOneOf(tok::semi, tok::comma)) return false;
    if (Left.isOneOf(tok::exclaim, tok::tilde)) return false;
    if (Left.is(tok::at) && Right.isOneOf(tok::identifier, tok::string_literal, tok::char_constant,
                                          tok::numeric_constant, tok::l_paren, tok::l_brace, tok::kw_true,
                                          tok::kw_false)) {
        return false;
    }
    if (Left.is(tok::colon)) return Left.isNot(TT::ObjCMethodExpr);
    if (Left.is(tok::coloncolon)) return false;
    if (Left.is(tok::less) || Right.isOneOf(tok::greater, tok::less)) return false;
    if (Right.is(tok::ellipsis)) {
        return Left.isLiteral() || (Left.is(tok::identifier) && BeforeLeft && BeforeLeft->is(tok::kw_case));
    }
    if (Left.is(tok::l_square) && Right.is(tok::amp)) return false;
    auto PointerAlignment = [&](const FormatToken &) { return Style_.PointerAlignment; };
    if (Right.is(TT::PointerOrReference)) {
        if (Left.is(tok::r_paren) && Line.MightBeFunctionDecl) {
            if (!Left.MatchingParen) return true;
            FormatToken *TokenBeforeMatchingParen = Left.MatchingParen->getPreviousNonComment();
            if (!TokenBeforeMatchingParen || Left.isNot(TT::TypeDeclarationParen)) return true;
        }
        if (Left.isLiteral()) return true;
        if (Left.isTypeOrIdentifier() && Right.Next && Right.Next->Next &&
            Right.Next->Next->is(TT::RangeBasedForLoopColon)) {
            return PointerAlignment(Right) != Style::PAS_Left;
        }
        return !Left.isOneOf(TT::PointerOrReference, tok::l_paren) &&
               (PointerAlignment(Right) != Style::PAS_Left ||
                (Line.IsMultiVariableDeclStmt &&
                 (Left.NestingLevel == 0 || (Left.NestingLevel == 1 && startsWithInitStatement(Line)))));
    }
    if (Right.is(TT::FunctionTypeLParen) && Left.isNot(tok::l_paren) &&
        (Left.isNot(TT::PointerOrReference) ||
         (PointerAlignment(Left) != Style::PAS_Right && !Line.IsMultiVariableDeclStmt))) {
        return true;
    }
    if (Left.is(TT::PointerOrReference)) {
        if (Right.isLiteral()) return true;
        if (Right.is(TT::BlockComment)) return true;
        if (Right.isOneOf(tok::identifier) &&
            (Right.TokenText == "override" || Right.TokenText == "final") && Right.isNot(TT::StartOfName))
            return true;
        if (Right.is(tok::l_brace) && Right.is(BK_Block)) return true;
        if (BeforeLeft && BeforeLeft->isTypeOrIdentifier() && Right.Next && Right.Next->is(TT::RangeBasedForLoopColon))
            return PointerAlignment(Left) != Style::PAS_Right;
        if (Right.isOneOf(TT::PointerOrReference, TT::ArraySubscriptLSquare, tok::l_paren)) return false;
        if (PointerAlignment(Left) == Style::PAS_Right) return false;
        if (Line.IsMultiVariableDeclStmt &&
            (Left.NestingLevel == Line.First->NestingLevel ||
             ((Left.NestingLevel == Line.First->NestingLevel + 1) && startsWithInitStatement(Line)))) {
            return false;
        }
        if (!BeforeLeft) return false;
        if (BeforeLeft->is(tok::coloncolon)) {
            if (Left.isNot(tok::star)) return false;
            if (!Right.startsSequence(tok::identifier, tok::r_paren)) return true;
            const FormatToken *LParen = Right.Next->MatchingParen;
            return !LParen || LParen->isNot(TT::FunctionTypeLParen);
        }
        return !BeforeLeft->isOneOf(tok::l_paren, tok::l_square);
    }
    if (Left.is(tok::ellipsis) && BeforeLeft && BeforeLeft->isPointerOrReference())
        return Style_.PointerAlignment != Style::PAS_Right;
    if (Right.is(tok::star) && Left.is(tok::l_paren)) return false;
    if (Left.is(tok::star) && Right.isPointerOrReference()) return false;
    if (Right.isPointerOrReference()) {
        const FormatToken *Previous = &Left;
        while (Previous) {
            if (Previous->is(tok::identifier) || Previous->isTypeName()) {
                Previous = Previous->getPreviousNonComment();
                continue;
            }
            if (Previous->is(TT::TemplateCloser) && Previous->MatchingParen) {
                Previous = Previous->MatchingParen->getPreviousNonComment();
                continue;
            }
            if (Previous->is(tok::coloncolon)) {
                Previous = Previous->getPreviousNonComment();
                continue;
            }
            break;
        }
        if (Previous) {
            if (Previous->is(tok::kw_const) || Previous->is(tok::kw_volatile))
                return Style_.PointerAlignment != Style::PAS_Left;
        }
    }
    // SpacesInContainerLiterals: true in LLVM style.
    if (Left.is(tok::l_square)) return Left.is(TT::ArrayInitializerLSquare) && Right.isNot(tok::r_square);
    if (Right.is(tok::r_square))
        return Right.MatchingParen && Right.MatchingParen->is(TT::ArrayInitializerLSquare);
    if (Right.is(tok::l_square) &&
        !Right.isOneOf(TT::ObjCMethodExpr, TT::LambdaLSquare, TT::DesignatedInitializerLSquare,
                       TT::StructuredBindingLSquare, TT::AttributeSquare) &&
        !Left.isOneOf(tok::numeric_constant, TT::DictLiteral)) {
        return false;
    }
    if (Left.is(tok::l_brace) && Right.is(tok::r_brace)) return !Left.Children.empty();
    if ((Left.is(tok::l_brace) && Left.isNot(BK_Block)) ||
        (Right.is(tok::r_brace) && Right.MatchingParen && Right.MatchingParen->isNot(BK_Block))) {
        return false;  // Cpp11BracedListStyle
    }
    if (Left.is(TT::BlockComment)) {
        std::string_view T = Left.TokenText;
        return !(T.size() >= 3 && T.substr(T.size() - 3) == "=*/");
    }
    if (Left.is(TT::TemplateCloser) && Right.is(TT::AttributeSquare)) return true;
    if (Right.is(tok::l_paren)) {
        if (Left.is(TT::TemplateCloser) && Right.isNot(TT::FunctionTypeLParen)) return spaceRequiredBeforeParens(Right);
        if (Left.is(TT::AttributeRParen) || (Left.is(tok::r_square) && Left.is(TT::AttributeSquare))) return true;
        if (Left.is(TT::ForEachMacro)) return true;  // AfterForeachMacros
        if (Left.is(TT::IfMacro)) return true;       // AfterIfMacros
        if (Left.is(tok::semi)) return true;
        // Left.isIf(/*AllowConstexprMacro=*/Line.Type != LT_PreprocessorDirective)
        const bool LeftIsIf = Left.is(tok::kw_if) || Left.endsSequence(tok::kw_constexpr, tok::kw_if) ||
                              (Left.endsSequence(tok::identifier, tok::kw_if) && Line.Type != LT_PreprocessorDirective);
        if (Left.isOneOf(tok::kw_for, tok::kw_while, tok::kw_switch, tok::kw_case, TT::ForEachMacro, TT::ObjCForIn) ||
            (Left.HasIdentifierInfo && Left.PPKind == pp::elif) || LeftIsIf || Right.is(TT::ConditionLParen)) {
            return Style_.SpaceBeforeParens != Style::SBPO_Never;  // AfterControlStatements
        }
        if (Line.MightBeFunctionDecl && Right.is(TT::FunctionDeclarationLParen)) return spaceRequiredBeforeParens(Right);
        if (Line.Type != LT_PreprocessorDirective && Left.is(tok::r_square) && Left.MatchingParen &&
            Left.MatchingParen->is(TT::LambdaLSquare)) {
            return spaceRequiredBeforeParens(Right);
        }
        if (!BeforeLeft || !BeforeLeft->isOneOf(tok::period, tok::arrow)) {
            if (Left.is(tok::identifier) && Left.TokenText == "__except")
                return Style_.SpaceBeforeParens != Style::SBPO_Never;
        }
        if (Line.Type != LT_PreprocessorDirective && (Left.HasIdentifierInfo || Left.is(tok::r_paren)))
            return spaceRequiredBeforeParens(Right);
        return false;
    }
    if (Left.is(tok::at)) return false;
    if (Right.is(TT::UnaryOperator)) {
        return !Left.isOneOf(tok::l_paren, tok::l_square, tok::at) &&
               (Left.isNot(tok::colon) || Left.isNot(TT::ObjCMethodExpr));
    }
    if ((Left.isOneOf(tok::identifier, tok::greater, tok::r_square, tok::r_paren) || Left.isTypeName()) &&
        Right.is(tok::l_brace) && Right.getNextNonComment() && Right.isNot(BK_Block)) {
        return false;
    }
    if (Left.is(tok::period) || Right.is(tok::period)) return false;
    if (Right.is(tok::hash) && Left.is(tok::identifier) &&
        (Left.TokenText == "L" || Left.TokenText == "u" || Left.TokenText == "U" || Left.TokenText == "u8" ||
         Left.TokenText == "LR" || Left.TokenText == "uR" || Left.TokenText == "UR" || Left.TokenText == "u8R")) {
        return false;
    }
    if (Left.is(TT::TemplateCloser) && Left.MatchingParen && Left.MatchingParen->Previous &&
        (Left.MatchingParen->Previous->is(tok::period) || Left.MatchingParen->Previous->is(tok::coloncolon))) {
        return false;
    }
    if (Left.is(TT::TemplateCloser) && Right.is(tok::l_square)) return false;
    if (Left.is(tok::l_brace) && Left.endsSequence(TT::DictLiteral, tok::at)) return false;
    if (Right.is(tok::r_brace) && Right.MatchingParen && Right.MatchingParen->endsSequence(TT::DictLiteral, tok::at))
        return false;
    if (Right.is(TT::TrailingAnnotation) && Right.isOneOf(tok::amp, tok::ampamp) &&
        Left.isOneOf(tok::kw_const, tok::kw_volatile) && (!Right.Next || Right.Next->is(tok::semi))) {
        return Style_.PointerAlignment != Style::PAS_Left;
    }
    return true;
}

bool TokenAnnotator::spaceRequiredBefore(const AnnotatedLine &Line, const FormatToken &Right) const {
    const FormatToken &Left = *Right.Previous;
    if (Left.Finalized) return Right.hasWhitespaceBefore();
    if (Right.HasIdentifierInfo && Left.HasIdentifierInfo) return true;
    if (Left.is(tok::star) && Right.is(tok::comment)) return true;
    const FormatToken *BeforeLeft = Left.Previous;
    (void)BeforeLeft;
    if (Right.is(tok::period) && Left.is(tok::numeric_constant)) return true;
    if (Left.is(tok::identifier) && Left.TokenText == "import" && Right.isOneOf(tok::less, tok::ellipsis)) return true;
    if (Right.is(TT::AfterPPDirective)) return true;
    if (Left.is(tok::ellipsis) && Right.is(tok::identifier) && Line.First->is(tok::identifier) &&
        Line.First->TokenText == "import") {
        return false;
    }
    if (Left.isOneOf(TT::AttributeRParen, TT::AttributeMacro) && Right.is(tok::coloncolon)) return true;
    if (Left.is(tok::identifier) && Right.is(tok::numeric_constant)) return Right.TokenText[0] != '.';
    if (Left.HasIdentifierInfo && Right.isLiteral()) return true;
    if (Left.is(TT::ImplicitStringLiteral)) return Right.hasWhitespaceBefore();
    if (Line.Type == LT_ObjCMethodDecl) {
        if (Left.is(TT::ObjCMethodSpecifier)) return true;
        if (Left.is(tok::r_paren) && Left.isNot(TT::AttributeRParen) && canBeObjCSelectorComponent(Right))
            return false;
    }
    if (Right.isOneOf(TT::TrailingReturnArrow, TT::LambdaArrow) ||
        Left.isOneOf(TT::TrailingReturnArrow, TT::LambdaArrow)) {
        return true;
    }
    if (Left.is(tok::comma)) return true;
    if (Right.is(tok::comma)) return false;
    if (Right.is(TT::ObjCBlockLParen)) return true;
    if (Right.is(TT::CtorInitializerColon)) return true;
    if (Left.is(TT::BitFieldColon)) return true;
    if (Right.is(tok::colon)) {
        if (Right.is(TT::CaseLabelColon)) return false;
        if (Right.is(TT::GotoLabelColon)) return false;
        if (!Right.getNextNonComment()) return false;
        if (Right.is(TT::ObjCMethodExpr)) return false;
        if (Left.is(tok::question)) return false;
        if (Right.is(TT::InlineASMColon) && Left.is(tok::coloncolon)) return false;
        if (Right.is(TT::DictLiteral)) return true;
        if (Right.is(TT::AttributeColon)) return false;
        if (Right.is(TT::GenericSelectionColon)) return false;
        if (Right.is(TT::BitFieldColon)) return true;
        return true;
    }
    if ((Left.isOneOf(tok::minus, tok::minusminus) && Right.isOneOf(tok::minus, tok::minusminus)) ||
        (Left.isOneOf(tok::plus, tok::plusplus) && Right.isOneOf(tok::plus, tok::plusplus))) {
        return true;
    }
    if (Left.is(TT::UnaryOperator)) {
        if (Left.is(tok::amp) && Right.is(tok::r_square)) return false;
        if (Left.isNot(tok::exclaim)) return false;
        return false;  // SpaceAfterLogicalNot
    }
    if (Left.is(TT::CastRParen)) return Style_.SpaceAfterCStyleCast || Right.isOneOf(TT::BinaryOperator, TT::SelectorName);
    if (Left.is(tok::greater) && Right.is(tok::greater)) return false;
    if (Right.isOneOf(tok::arrow) || Left.isOneOf(tok::arrow, tok::period) ||
        (Right.is(tok::period) && Right.isNot(TT::DesignatedInitializerPeriod))) {
        return false;
    }
    if (!Style_.SpaceBeforeAssignmentOperators && Left.isNot(TT::TemplateCloser) &&
        Right.getPrecedence() == prec::Assignment) {
        return false;
    }
    if (Right.is(tok::coloncolon) && Left.is(tok::identifier)) return Right.hasWhitespaceBefore();
    if (Right.is(tok::coloncolon) && !Left.isOneOf(tok::l_brace, tok::comment, tok::l_paren)) {
        return !(Left.isOneOf(tok::l_paren, tok::r_paren, tok::l_square, TT::TemplateOpener, TT::TemplateCloser));
    }
    if ((Left.is(TT::TemplateOpener)) != (Right.is(TT::TemplateCloser))) return false;
    if (Left.is(tok::r_paren) && Left.isNot(TT::TypeDeclarationParen) && Right.is(TT::PointerOrReference) &&
        Right.isOneOf(tok::amp, tok::ampamp)) {
        return true;
    }
    if (Right.is(TT::StructuredBindingLSquare))
        return !Left.isOneOf(tok::amp, tok::ampamp) || Style_.PointerAlignment != Style::PAS_Right;
    if (Right.Next && Right.Next->is(TT::StructuredBindingLSquare) && Right.isOneOf(tok::amp, tok::ampamp))
        return Style_.PointerAlignment != Style::PAS_Left;
    if ((Right.is(TT::BinaryOperator) && Left.isNot(tok::l_paren)) ||
        (Left.isOneOf(TT::BinaryOperator, TT::ConditionalExpr) && Right.isNot(tok::r_paren))) {
        return true;
    }
    if (Right.is(tok::less) && Left.isNot(tok::l_paren) && Line.Type == LT_ImportStatement) return true;
    if (Right.is(TT::TrailingUnaryOperator)) return false;
    return spaceRequiredBetween(Line, Left, Right);
}

bool TokenAnnotator::mustBreakBefore(const AnnotatedLine &Line, const FormatToken &Right) const {
    if (Right.NewlinesBefore > 1 && Style_.MaxEmptyLinesToKeep > 0) return true;
    const FormatToken &Left = *Right.Previous;
    const FormatToken *BeforeLeft = Left.Previous;
    const FormatToken *AfterRight = Right.Next;
    if (Style_.BreakAdjacentStringLiterals && Left.isStringLiteral() && Right.isStringLiteral()) return true;
    {
        const FormatToken *BeforeClosingBrace = nullptr;
        if (Left.isOneOf(tok::l_brace, TT::ArrayInitializerLSquare) && Left.isNot(BK_Block) && Left.MatchingParen) {
            BeforeClosingBrace = Left.MatchingParen->Previous;
        } else if (Right.MatchingParen && Right.MatchingParen->isOneOf(tok::l_brace, TT::ArrayInitializerLSquare)) {
            BeforeClosingBrace = &Left;
        }
        if (BeforeClosingBrace && (BeforeClosingBrace->is(tok::comma) || BeforeClosingBrace->isTrailingComment()))
            return true;
    }
    if (Right.is(tok::comment)) {
        return Left.isNot(BK_BracedInit) && Left.isNot(TT::CtorInitializerColon) &&
               (Right.NewlinesBefore > 0 && Right.HasUnescapedNewline);
    }
    if (Left.isTrailingComment()) return true;
    if (Left.IsUnterminatedLiteral) return true;
    if (BeforeLeft && BeforeLeft->is(tok::lessless) && Left.is(tok::string_literal) && Right.is(tok::lessless) &&
        AfterRight && AfterRight->is(tok::string_literal)) {
        return Right.NewlinesBefore > 0;
    }
    if (Right.is(TT::InlineASMBrace)) return Right.HasUnescapedNewline;
    if (isAllmanBrace(Left) || isAllmanBrace(Right)) {
        if (Style_.BraceWrappingAfterEnum && (Line.startsWith(tok::kw_enum) || Line.startsWith(tok::kw_typedef, tok::kw_enum)))
            return true;
        if (Right.isNot(TT::FunctionLBrace)) return Line.startsWith(tok::kw_struct) && Style_.BraceWrappingAfterStruct;
    }
    if (Left.is(TT::ObjCBlockLBrace) && Style_.AllowShortBlocksOnASingleLine == Style::SBS_Never) return true;
    return false;
}

bool TokenAnnotator::canBreakBefore(const AnnotatedLine &Line, const FormatToken &Right) const {
    const FormatToken &Left = *Right.Previous;
    if (Right.is(tok::r_brace)) return Right.MatchingParen && Right.MatchingParen->is(BK_Block);
    if (Right.is(tok::r_paren)) return false;
    if (Right.is(TT::TemplateCloser)) return false;
    if (Left.is(tok::at)) return false;
    if (Right.is(TT::PointerOrReference)) {
        return Line.IsMultiVariableDeclStmt ||
               (Style_.PointerAlignment == Style::PAS_Right &&
                !(Right.Next && Right.Next->isOneOf(TT::FunctionDeclarationName, tok::kw_const)));
    }
    if (Right.isOneOf(TT::StartOfName, TT::FunctionDeclarationName, TT::ClassHeadName)) return true;
    if (Left.is(TT::PointerOrReference)) return false;
    if (Right.isTrailingComment()) return Left.is(BK_BracedInit);
    if (Left.is(tok::question) && Right.is(tok::colon)) return false;
    if (Right.is(TT::ConditionalExpr) || Right.is(tok::question)) return Style_.BreakBeforeTernaryOperators;
    if (Left.is(TT::ConditionalExpr) || Left.is(tok::question)) return !Style_.BreakBeforeTernaryOperators;
    if (Left.is(TT::InheritanceColon)) return false;
    if (Right.is(TT::InheritanceColon)) return true;
    if (Right.is(TT::ObjCMethodExpr) && Right.isNot(tok::r_square) && Left.isNot(TT::SelectorName)) return true;
    if (Right.is(tok::colon) && !Right.isOneOf(TT::CtorInitializerColon, TT::InlineASMColon, TT::BitFieldColon))
        return false;
    if (Left.is(tok::colon) && Left.isOneOf(TT::DictLiteral, TT::ObjCMethodExpr)) return true;
    if (Right.is(TT::SelectorName) || (Right.is(tok::identifier) && Right.Next && Right.Next->is(TT::ObjCMethodExpr)))
        return Left.isNot(tok::period);
    if (Left.ClosesTemplateDeclaration) return true;
    if (Left.is(TT::FunctionAnnotationRParen)) return true;
    if (Right.is(TT::RangeBasedForLoopColon)) return false;
    if (Left.is(TT::RangeBasedForLoopColon)) return true;
    if (Left.is(TT::TemplateCloser) && Right.is(TT::TemplateOpener)) return true;
    if ((Left.is(tok::greater) && Right.is(tok::greater)) || (Left.is(tok::less) && Right.is(tok::less))) return false;
    if (Left.isOneOf(TT::TemplateCloser, TT::UnaryOperator)) return false;
    if (Left.is(TT::AttributeLParen) || (Left.is(tok::l_paren) && Left.is(TT::TypeDeclarationParen))) return false;
    if (Left.is(tok::l_paren) && Left.Previous && Left.Previous->isOneOf(TT::BinaryOperator, TT::CastRParen))
        return false;
    if (Right.is(TT::ImplicitStringLiteral)) return false;
    if (Right.is(tok::r_square) && Right.MatchingParen && Right.MatchingParen->is(TT::LambdaLSquare)) return false;
    if (Left.is(TT::TrailingAnnotation)) {
        return !Right.isOneOf(tok::l_brace, tok::semi, tok::equal, tok::l_paren, tok::less, tok::coloncolon);
    }
    if (Right.isAttribute()) return true;
    if (Right.is(tok::l_square) && Right.is(TT::AttributeSquare)) return Left.isNot(TT::AttributeSquare);
    if (Left.is(tok::identifier) && Right.is(tok::string_literal)) return true;
    if (Right.is(tok::identifier) && Right.Next && Right.Next->is(TT::DictLiteral)) return true;
    if (Left.is(TT::CtorInitializerColon)) return false;
    if (Right.is(TT::CtorInitializerColon)) return true;
    if (Left.is(TT::ArrayInitializerLSquare)) return true;
    if ((Left.isBinaryOperator() || Left.is(TT::BinaryOperator)) && Left.isNot(tok::lessless)) {
        return true;
    }
    if ((Left.is(TT::AttributeSquare) && Right.is(tok::l_square)) ||
        (Left.is(tok::r_square) && Right.is(TT::AttributeSquare))) {
        return false;
    }
    return Left.isOneOf(tok::comma, tok::coloncolon, tok::semi, tok::l_brace, tok::kw_struct, tok::comment) ||
           Right.isMemberAccess() ||
           Right.isOneOf(TT::TrailingReturnArrow, TT::LambdaArrow, tok::lessless, tok::colon, tok::l_square, tok::at) ||
           (Left.is(tok::r_paren) && Right.isOneOf(tok::identifier, tok::kw_const)) ||
           (Left.is(tok::l_paren) && Right.isNot(tok::r_paren)) ||
           (Left.is(TT::TemplateOpener) && Right.isNot(TT::TemplateCloser));
}

}  // namespace cfmt::detail
