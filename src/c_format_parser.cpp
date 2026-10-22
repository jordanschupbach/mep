// Splitting a C file into unwrapped lines, the port of clang-format's
// UnwrappedLineParser. An unwrapped line is one statement or declaration, a
// block's opening or closing brace, a comment line or a preprocessor
// directive, with its nesting level. #if/#else branches are handled the way
// clang-format handles them: the file is parsed once per branch combination
// ("run"), each run taking a different branch at some #if, and tokens in the
// branches a run does not take are skipped by it.

#include "c_format_internal.h"

#include <algorithm>
#include <cassert>
#include <stack>
#include <unordered_set>

namespace cfmt::detail {

namespace {

class TokenSource {
public:
    virtual ~TokenSource() = default;
    virtual FormatToken *getNextToken() = 0;
    virtual FormatToken *getPreviousToken() = 0;
    virtual FormatToken *peekNextToken(bool SkipComment = false) = 0;
    virtual bool isEOF() = 0;
    virtual unsigned getPosition() = 0;
    virtual FormatToken *setPosition(unsigned Position) = 0;
    // Skips comments and returns the next non-comment token.
    FormatToken *getNextNonComment() {
        FormatToken *Tok;
        do {
            Tok = getNextToken();
        } while (Tok->is(tok::comment));
        return Tok;
    }
};

class IndexedTokenSource : public TokenSource {
public:
    explicit IndexedTokenSource(const std::vector<FormatToken *> &Tokens) : Tokens(Tokens) {}
    FormatToken *getNextToken() override {
        if (Position >= 0 && isEOF()) return Tokens[static_cast<size_t>(Position)];
        ++Position;
        return Tokens[static_cast<size_t>(Position)];
    }
    FormatToken *getPreviousToken() override { return Position > 0 ? Tokens[static_cast<size_t>(Position - 1)] : nullptr; }
    FormatToken *peekNextToken(bool SkipComment) override {
        if (isEOF()) return Tokens[static_cast<size_t>(Position)];
        int Next = Position + 1;
        if (SkipComment)
            while (Tokens[static_cast<size_t>(Next)]->is(tok::comment)) ++Next;
        return Tokens[static_cast<size_t>(Next)];
    }
    bool isEOF() override { return Position == -1 ? false : Tokens[static_cast<size_t>(Position)]->is(tok::eof); }
    unsigned getPosition() override { return static_cast<unsigned>(Position); }
    FormatToken *setPosition(unsigned P) override {
        Position = static_cast<int>(P);
        return Tokens[static_cast<size_t>(Position)];
    }
    void reset() { Position = -1; }

private:
    const std::vector<FormatToken *> &Tokens;
    int Position = -1;
};

class ScopedMacroState : public TokenSource {
public:
    ScopedMacroState(UnwrappedLine &Line, TokenSource *&SourceRef, FormatToken *&ResetToken)
        : Line(Line), Source(SourceRef), ResetToken(ResetToken), PreviousLineLevel(Line.Level),
          PreviousTokenSource(SourceRef) {
        FakeEOF.Kind = tok::eof;
        Source = this;
        Line.Level = 0;
        Line.InPPDirective = true;
    }
    ~ScopedMacroState() override {
        Source = PreviousTokenSource;
        ResetToken = Token;
        Line.InPPDirective = false;
        Line.InMacroBody = false;
        Line.Level = PreviousLineLevel;
    }
    FormatToken *getNextToken() override {
        PreviousToken = Token;
        Token = PreviousTokenSource->getNextToken();
        if (eof()) return &FakeEOF;
        return Token;
    }
    FormatToken *getPreviousToken() override { return PreviousTokenSource->getPreviousToken(); }
    FormatToken *peekNextToken(bool SkipComment) override {
        if (eof()) return &FakeEOF;
        return PreviousTokenSource->peekNextToken(SkipComment);
    }
    bool isEOF() override { return PreviousTokenSource->isEOF(); }
    unsigned getPosition() override { return PreviousTokenSource->getPosition(); }
    FormatToken *setPosition(unsigned Position) override {
        PreviousToken = nullptr;
        Token = PreviousTokenSource->setPosition(Position);
        return Token;
    }

private:
    bool eof() {
        return Token && Token->HasUnescapedNewline &&
               !continuesLineComment(*Token, PreviousToken, /*MinColumnToken=*/PreviousToken);
    }

    FormatToken FakeEOF;
    UnwrappedLine &Line;
    TokenSource *&Source;
    FormatToken *&ResetToken;
    unsigned PreviousLineLevel;
    TokenSource *PreviousTokenSource;
    FormatToken *Token = nullptr;
    FormatToken *PreviousToken = nullptr;
};

bool isC78Type(const FormatToken &Tok) {
    return Tok.isOneOf(tok::kw_char, tok::kw_short, tok::kw_int, tok::kw_long, tok::kw_unsigned, tok::kw_float,
                       tok::kw_double, tok::identifier);
}

bool isC78ParameterDecl(const FormatToken *Tok, const FormatToken *Next, const FormatToken *FuncName) {
    if (FuncName->isNot(tok::identifier)) return false;
    const FormatToken *Prev = FuncName->Previous;
    if (!Prev || (Prev->isNot(tok::star) && !isC78Type(*Prev))) return false;
    if (!isC78Type(*Tok) && !Tok->isOneOf(tok::kw_register, tok::kw_struct, tok::kw_union)) return false;
    if (Next->isNot(tok::star) && !Next->HasIdentifierInfo) return false;
    Tok = Tok->Previous;
    if (!Tok || Tok->isNot(tok::r_paren)) return false;
    Tok = Tok->Previous;
    if (!Tok || Tok->isNot(tok::identifier)) return false;
    return Tok->Previous && Tok->Previous->isOneOf(tok::l_paren, tok::comma);
}

bool tokenCanStartNewLine(const FormatToken &Tok) {
    return !Tok.isOneOf(tok::semi, tok::l_brace, tok::period, tok::arrow, tok::less, tok::greater, tok::slash,
                        tok::percent, tok::lessless, tok::greatergreater, tok::equal, tok::plusequal, tok::minusequal,
                        tok::starequal, tok::slashequal, tok::percentequal, tok::ampequal, tok::pipeequal,
                        tok::caretequal, tok::greatergreaterequal, tok::lesslessequal, tok::colon);
}

std::string_view upper(std::string_view S, std::string &Buf) {
    Buf.assign(S);
    for (char &C : Buf)
        if (C >= 'a' && C <= 'z') C = static_cast<char>(C - 'a' + 'A');
    return Buf;
}

bool isAllUpper(std::string_view S) {
    std::string Buf;
    return upper(S, Buf) == S;
}

class UnwrappedLineParser {
public:
    UnwrappedLineParser(Source &Src, const Style &S, const RunCallback &OnRun)
        : Src(Src), S(S), OnRun(OnRun), AllTokens(Src.Tokens) {}

    void parse() {
        IndexedTokenSource TokenSource(AllTokens);
        do {
            reset();
            Tokens = &TokenSource;
            TokenSource.reset();
            readToken();
            parseFile();
            assert(eof());
            pushToken(FormatTok);
            addUnwrappedLine();
            // Every run walks one combination of #if branches; the branch
            // counts the runs so far have found say how many there will be.
            size_t Predicted = 1;
            for (int Count : PPLevelBranchCount) {
                if (Predicted < (size_t{1} << 40)) Predicted *= static_cast<size_t>(std::max(Count, 1));
            }
            if (!OnRun(Lines, Predicted)) return;
            Lines.clear();
            while (!PPLevelBranchIndex.empty() && PPLevelBranchIndex.back() + 1 >= PPLevelBranchCount.back()) {
                PPLevelBranchIndex.pop_back();
                PPLevelBranchCount.pop_back();
            }
            if (!PPLevelBranchIndex.empty()) ++PPLevelBranchIndex.back();
        } while (!PPLevelBranchIndex.empty());
    }

private:
    friend class ScopedLineState;
    friend class CompoundStatementIndenter;

    struct PPBranch {
        enum Kind { PP_Conditional, PP_Unreachable } K;
        size_t Line;
    };

    class ScopedDeclarationState {
    public:
        ScopedDeclarationState(UnwrappedLine &Line, std::vector<bool> &Stack, bool MustBeDeclaration)
            : Line(Line), Stack(Stack) {
            Line.MustBeDeclaration = MustBeDeclaration;
            Stack.push_back(MustBeDeclaration);
        }
        ~ScopedDeclarationState() {
            Stack.pop_back();
            Line.MustBeDeclaration = Stack.empty() ? true : Stack.back();
        }

    private:
        UnwrappedLine &Line;
        std::vector<bool> &Stack;
    };

    class ScopedLineState {
    public:
        ScopedLineState(UnwrappedLineParser &Parser, bool SwitchToPreprocessorLines = false)
            : Parser(Parser), OriginalLines(Parser.CurrentLines) {
            if (SwitchToPreprocessorLines)
                Parser.CurrentLines = &Parser.PreprocessorDirectives;
            else if (!Parser.Line->Tokens.empty())
                Parser.CurrentLines = &Parser.Line->Tokens.back().Children;
            PreBlockLine = std::move(Parser.Line);
            Parser.Line = std::make_unique<UnwrappedLine>();
            Parser.Line->Level = PreBlockLine->Level;
            Parser.Line->PPLevel = PreBlockLine->PPLevel;
            Parser.Line->InPPDirective = PreBlockLine->InPPDirective;
            Parser.Line->InMacroBody = PreBlockLine->InMacroBody;
            Parser.Line->UnbracedBodyLevel = PreBlockLine->UnbracedBodyLevel;
        }
        ~ScopedLineState() {
            if (!Parser.Line->Tokens.empty()) Parser.addUnwrappedLine();
            Parser.Line = std::move(PreBlockLine);
            if (Parser.CurrentLines == &Parser.PreprocessorDirectives) Parser.AtEndOfPPLine = true;
            Parser.CurrentLines = OriginalLines;
        }

    private:
        UnwrappedLineParser &Parser;
        std::unique_ptr<UnwrappedLine> PreBlockLine;
        std::vector<UnwrappedLine> *OriginalLines;
    };

    void reset() {
        PPBranchLevel = -1;
        Line = std::make_unique<UnwrappedLine>();
        CommentsBeforeNextToken.clear();
        FormatTok = nullptr;
        AtEndOfPPLine = false;
        PreprocessorDirectives.clear();
        CurrentLines = &Lines;
        DeclarationScopeStack.clear();
        PPStack.clear();
        while (!PPChainBranchIndex.empty()) PPChainBranchIndex.pop();
    }

    bool parsingPPDirective() const { return CurrentLines != &Lines; }

    void parseFile() {
        bool MustBeDeclaration = !Line->InPPDirective;
        ScopedDeclarationState DeclarationState(*Line, DeclarationScopeStack, MustBeDeclaration);
        parseLevel();
        flushComments(true);
        addUnwrappedLine();
    }

    bool precededByCommentOrPPDirective() const {
        if (!Lines.empty() && Lines.back().InPPDirective) return true;
        const FormatToken *Previous = Tokens->getPreviousToken();
        return Previous && Previous->is(tok::comment) && (Previous->IsMultiline || Previous->NewlinesBefore > 0);
    }

    bool parseLevel(const FormatToken *OpeningBrace = nullptr) {
        bool HasDoWhile = false;
        bool HasLabel = false;
        bool SwitchLabelEncountered = false;
        do {
            if (FormatTok->isAttribute()) {
                nextToken();
                if (FormatTok->is(tok::l_paren)) parseParens();
                continue;
            }
            tok Kind = FormatTok->Kind;
            auto ParseDefault = [&] {
                parseStructuralElement(OpeningBrace, HasDoWhile ? nullptr : &HasDoWhile,
                                       HasLabel ? nullptr : &HasLabel);
            };
            switch (Kind) {
            case tok::comment:
                nextToken();
                addUnwrappedLine();
                break;
            case tok::l_brace:
                if (tryToParseBracedList()) continue;
                FormatTok->setFinalizedType(TT::BlockLBrace);
                parseBlock();
                addUnwrappedLine();
                break;
            case tok::r_brace:
                if (OpeningBrace) return false;
                nextToken();
                addUnwrappedLine();
                break;
            case tok::kw_default: {
                unsigned StoredPosition = Tokens->getPosition();
                FormatToken *Next = Tokens->getNextNonComment();
                FormatTok = Tokens->setPosition(StoredPosition);
                if (!Next->isOneOf(tok::colon, tok::arrow)) {
                    parseStructuralElement();
                    break;
                }
                [[fallthrough]];
            }
            case tok::kw_case:
                if (!SwitchLabelEncountered &&
                    (S.IndentCaseLabels || (Line->InPPDirective && Line->Level == 1))) {
                    ++Line->Level;
                }
                SwitchLabelEncountered = true;
                parseStructuralElement();
                break;
            case tok::l_square:
                if (handleCppAttributes()) break;
                [[fallthrough]];
            default:
                ParseDefault();
                break;
            }
        } while (!eof());
        return false;
    }

    void calculateBraceTypes(bool ExpectClassBody = false) {
        unsigned StoredPosition = Tokens->getPosition();
        FormatToken *Tok = FormatTok;
        const FormatToken *PrevTok = Tok->Previous;
        struct StackEntry {
            FormatToken *Tok;
            const FormatToken *PrevTok;
        };
        std::vector<StackEntry> LBraceStack;
        do {
            FormatToken *NextTok = Tokens->getNextNonComment();
            if (!Line->InMacroBody) {
                while (NextTok->is(tok::hash)) {
                    NextTok = Tokens->getNextToken();
                    // pp_not_keyword or pp_define: an identifier that names no
                    // directive, or `define`.
                    if (NextTok->HasIdentifierInfo &&
                        (NextTok->PPKind == pp::none || NextTok->PPKind == pp::define))
                        break;
                    do {
                        NextTok = Tokens->getNextToken();
                    } while (!NextTok->HasUnescapedNewline && NextTok->isNot(tok::eof));
                    while (NextTok->is(tok::comment)) NextTok = Tokens->getNextToken();
                }
            }
            switch (Tok->Kind) {
            case tok::l_brace:
                Tok->BlockKind = BK_Unknown;
                LBraceStack.push_back({Tok, PrevTok});
                break;
            case tok::r_brace: {
                if (LBraceStack.empty()) break;
                FormatToken *LBrace = LBraceStack.back().Tok;
                if (LBrace->is(BK_Unknown)) {
                    bool ProbablyBracedList = false;
                    if (LBrace->isNot(TT::EnumLBrace)) {
                        bool NextIsObjCMethod = NextTok->isOneOf(tok::plus, tok::minus) && NextTok->OriginalColumn == 0;
                        ProbablyBracedList = LBrace->is(TT::BracedListLBrace);
                        ProbablyBracedList = ProbablyBracedList ||
                                             (PrevTok->isLiteral() || NextTok->isOneOf(tok::l_paren, tok::arrow));
                        ProbablyBracedList = ProbablyBracedList ||
                                             NextTok->isOneOf(tok::comma, tok::period, tok::colon, tok::r_paren,
                                                              tok::r_square, tok::ellipsis);
                        ProbablyBracedList = ProbablyBracedList ||
                                             (NextTok->is(tok::l_brace) && LBraceStack.back().PrevTok &&
                                              LBraceStack.back().PrevTok->isOneOf(tok::identifier, tok::greater));
                        ProbablyBracedList =
                            ProbablyBracedList ||
                            (NextTok->is(tok::identifier) && !PrevTok->isOneOf(tok::semi, tok::r_brace, tok::l_brace));
                        ProbablyBracedList = ProbablyBracedList ||
                                             (NextTok->is(tok::semi) && (!ExpectClassBody || LBraceStack.size() != 1));
                        ProbablyBracedList = ProbablyBracedList || (NextTok->isBinaryOperator() && !NextIsObjCMethod);
                        if (NextTok->is(tok::l_square)) {
                            NextTok = Tokens->getNextToken();
                            ProbablyBracedList = NextTok->isNot(tok::l_square);
                        }
                        if (Line->InMacroBody && PrevTok != FormatTok && !FormatTok->Previous &&
                            NextTok->is(tok::eof) && !PrevTok->isOneOf(tok::semi, BK_Block, tok::colon)) {
                            ProbablyBracedList = true;
                        }
                    }
                    BraceBlockKind BlockKind = ProbablyBracedList ? BK_BracedInit : BK_Block;
                    Tok->BlockKind = BlockKind;
                    LBrace->BlockKind = BlockKind;
                }
                LBraceStack.pop_back();
                break;
            }
            case tok::identifier:
                if (Tok->isNot(TT::StatementMacro)) break;
                [[fallthrough]];
            case tok::at:
            case tok::semi:
            case tok::kw_if:
            case tok::kw_while:
            case tok::kw_for:
            case tok::kw_switch:
            case tok::kw___try:
                if (!LBraceStack.empty() && LBraceStack.back().Tok->is(BK_Unknown))
                    LBraceStack.back().Tok->BlockKind = BK_Block;
                break;
            default:
                break;
            }
            PrevTok = Tok;
            Tok = NextTok;
        } while (Tok->isNot(tok::eof) && !LBraceStack.empty());
        for (const auto &Entry : LBraceStack)
            if (Entry.Tok->is(BK_Unknown)) Entry.Tok->BlockKind = BK_Block;
        FormatTok = Tokens->setPosition(StoredPosition);
    }

    void setPreviousRBraceType(TT Type) {
        if (FormatToken *Prev = FormatTok->getPreviousNonComment(); Prev && Prev->is(tok::r_brace))
            Prev->setFinalizedType(Type);
    }

    FormatToken *parseBlock(bool MustBeDeclaration = false, unsigned AddLevels = 1u, bool MunchSemi = true) {
        assert(FormatTok->is(tok::l_brace));
        FormatToken *Tok = FormatTok;
        FormatTok->BlockKind = BK_Block;
        const unsigned InitialLevel = Line->Level;
        nextToken(static_cast<int>(AddLevels));
        if (Line->Level > 300) return nullptr;
        addUnwrappedLine();
        ScopedDeclarationState DeclarationState(*Line, DeclarationScopeStack, MustBeDeclaration);
        if (AddLevels > 0u) Line->Level += AddLevels;
        parseLevel(Tok);
        if (eof()) return nullptr;
        if (FormatTok->isNot(tok::r_brace)) {
            Line->Level = InitialLevel;
            FormatTok->BlockKind = BK_Block;
            return nullptr;
        }
        FormatTok->BlockKind = BK_Block;
        nextToken(-static_cast<int>(AddLevels));
        Line->Level = InitialLevel;
        if (FormatTok->is(tok::arrow)) {
            nextToken();
            parseStructuralElement();
        }
        if (MunchSemi && FormatTok->is(tok::semi)) nextToken();
        return nullptr;
    }

    void parseChildBlock() {
        assert(FormatTok->is(tok::l_brace));
        FormatTok->BlockKind = BK_Block;
        const FormatToken *OpeningBrace = FormatTok;
        nextToken();
        {
            ScopedLineState LineState(*this);
            ScopedDeclarationState DeclarationState(*Line, DeclarationScopeStack, /*MustBeDeclaration=*/false);
            Line->Level += 1;
            parseLevel(OpeningBrace);
            flushComments(isOnNewLine(*FormatTok));
            Line->Level -= 1;
        }
        nextToken();
    }

    void parsePPDirective() {
        assert(FormatTok->is(tok::hash));
        ScopedMacroState MacroState(*Line, Tokens, FormatTok);
        nextToken();
        if (!FormatTok->HasIdentifierInfo) {
            parsePPUnknown();
            return;
        }
        switch (FormatTok->PPKind) {
        case pp::define:
            parsePPDefine();
            return;
        case pp::if_:
            parsePPIf(/*IfDef=*/false);
            break;
        case pp::ifdef:
        case pp::ifndef:
            parsePPIf(/*IfDef=*/true);
            break;
        case pp::else_:
        case pp::elifdef:
        case pp::elifndef:
        case pp::elif:
            parsePPElse();
            break;
        case pp::endif:
            parsePPEndIf();
            break;
        case pp::pragma:
            Line->InPragmaDirective = true;
            parsePPUnknown();
            break;
        case pp::error:
        case pp::warning:
            nextToken();
            if (!eof()) FormatTok->setFinalizedType(TT::AfterPPDirective);
            [[fallthrough]];
        default:
            parsePPUnknown();
            break;
        }
    }

    void conditionalCompilationCondition(bool Unreachable) {
        size_t L = CurrentLines->size();
        if (CurrentLines == &PreprocessorDirectives) L += Lines.size();
        if (Unreachable || (!PPStack.empty() && PPStack.back().K == PPBranch::PP_Unreachable))
            PPStack.push_back({PPBranch::PP_Unreachable, L});
        else
            PPStack.push_back({PPBranch::PP_Conditional, L});
    }

    void conditionalCompilationStart(bool Unreachable) {
        ++PPBranchLevel;
        if (PPBranchLevel == static_cast<int>(PPLevelBranchIndex.size())) {
            PPLevelBranchIndex.push_back(0);
            PPLevelBranchCount.push_back(0);
        }
        PPChainBranchIndex.push(Unreachable ? -1 : 0);
        bool Skip = PPLevelBranchIndex[static_cast<size_t>(PPBranchLevel)] > 0;
        conditionalCompilationCondition(Unreachable || Skip);
    }

    void conditionalCompilationAlternative() {
        if (!PPStack.empty()) PPStack.pop_back();
        if (!PPChainBranchIndex.empty()) ++PPChainBranchIndex.top();
        conditionalCompilationCondition(PPBranchLevel >= 0 && !PPChainBranchIndex.empty() &&
                                        PPLevelBranchIndex[static_cast<size_t>(PPBranchLevel)] != PPChainBranchIndex.top());
    }

    void conditionalCompilationEnd() {
        if (PPBranchLevel >= 0 && !PPChainBranchIndex.empty()) {
            if (PPChainBranchIndex.top() + 1 > PPLevelBranchCount[static_cast<size_t>(PPBranchLevel)])
                PPLevelBranchCount[static_cast<size_t>(PPBranchLevel)] = PPChainBranchIndex.top() + 1;
        }
        if (PPBranchLevel > -1) --PPBranchLevel;
        if (!PPChainBranchIndex.empty()) PPChainBranchIndex.pop();
        if (!PPStack.empty()) PPStack.pop_back();
    }

    void parsePPIf(bool IfDef) {
        bool IfNDef = FormatTok->is(pp::ifndef);
        nextToken();
        bool Unreachable = false;
        if (!IfDef && (FormatTok->is(tok::kw_false) || FormatTok->TokenText == "0")) Unreachable = true;
        if (IfDef && !IfNDef && FormatTok->TokenText == "SWIG") Unreachable = true;
        conditionalCompilationStart(Unreachable);
        --PPBranchLevel;
        parsePPUnknown();
        ++PPBranchLevel;
    }

    void parsePPElse() {
        if (PPBranchLevel == -1) conditionalCompilationStart(/*Unreachable=*/true);
        conditionalCompilationAlternative();
        --PPBranchLevel;
        parsePPUnknown();
        ++PPBranchLevel;
    }

    void parsePPEndIf() {
        conditionalCompilationEnd();
        parsePPUnknown();
    }

    void parsePPDefine() {
        nextToken();
        if (!FormatTok->HasIdentifierInfo) {
            parsePPUnknown();
            return;
        }
        // Inside a define, even keywords are plain identifiers.
        FormatTok->Kind = tok::identifier;
        FormatTok->HasIdentifierInfo = true;
        FormatTok->PPKind = pp::none;
        nextToken();
        if (FormatTok->Kind == tok::l_paren && !FormatTok->hasWhitespaceBefore()) parseParens();
        addUnwrappedLine();
        ++Line->Level;
        Line->PPLevel = static_cast<unsigned>(PPBranchLevel + 1);
        Line->InMacroBody = true;
        parseFile();
    }

    void parsePPUnknown() {
        while (!eof()) nextToken();
        addUnwrappedLine();
    }

    void parseStructuralElement(const FormatToken *OpeningBrace = nullptr, bool *HasDoWhile = nullptr,
                                bool *HasLabel = nullptr) {
        while (FormatTok->is(tok::l_square) && handleCppAttributes()) {
        }
        switch (FormatTok->Kind) {
        case tok::kw_asm:
            nextToken();
            if (FormatTok->is(tok::l_brace)) {
                FormatTok->setFinalizedType(TT::InlineASMBrace);
                nextToken();
                while (FormatTok && !eof()) {
                    if (FormatTok->is(tok::r_brace)) {
                        FormatTok->setFinalizedType(TT::InlineASMBrace);
                        nextToken();
                        addUnwrappedLine();
                        break;
                    }
                    FormatTok->Finalized = true;
                    nextToken();
                }
            }
            break;
        case tok::kw_if:
            parseIfThenElse();
            return;
        case tok::kw_for:
        case tok::kw_while:
            parseForOrWhileLoop();
            return;
        case tok::kw_do:
            parseDoWhile();
            if (HasDoWhile) *HasDoWhile = true;
            return;
        case tok::kw_switch:
            parseSwitch();
            return;
        case tok::kw_default: {
            nextToken();
            if (FormatTok->is(tok::colon)) {
                FormatTok->setFinalizedType(TT::CaseLabelColon);
                parseLabel();
                return;
            }
            break;
        }
        case tok::kw_case:
            parseCaseLabel();
            return;
        case tok::kw_goto:
            nextToken();
            if (FormatTok->is(tok::kw_case)) nextToken();
            break;
        case tok::kw___try:
            parseTryCatch();
            return;
        case tok::kw_extern:
            nextToken();
            if (FormatTok->is(tok::string_literal)) {
                nextToken();
                if (FormatTok->is(tok::l_brace)) {
                    if (S.BraceWrappingAfterExternBlock) addUnwrappedLine();
                    unsigned AddLevels = S.BraceWrappingAfterExternBlock ? 1u : 0u;
                    parseBlock(/*MustBeDeclaration=*/true, AddLevels);
                    addUnwrappedLine();
                    return;
                }
            }
            break;
        case tok::kw_inline:
            nextToken();
            break;
        case tok::identifier:
            if (FormatTok->is(TT::ForEachMacro)) {
                parseForOrWhileLoop();
                return;
            }
            if (FormatTok->TokenText == "import" && parseModuleImport()) return;
            if (FormatTok->TokenText == "signals" || FormatTok->TokenText == "Q_SIGNALS" ||
                FormatTok->TokenText == "slots" || FormatTok->TokenText == "Q_SLOTS") {
                nextToken();
                if (FormatTok->is(tok::colon)) {
                    nextToken();
                    addUnwrappedLine();
                    return;
                }
            }
            if (FormatTok->is(TT::StatementMacro)) {
                parseStatementMacro();
                return;
            }
            if (Tokens->peekNextToken()->is(tok::colon) && !Line->MustBeDeclaration) {
                nextToken();
                if (!Line->InMacroBody || CurrentLines->size() > 1) Line->Tokens.front().Tok->MustBreakBefore = true;
                FormatTok->setFinalizedType(TT::GotoLabelColon);
                parseLabel(!S.IndentGotoLabels);
                if (HasLabel) *HasLabel = true;
                return;
            }
            break;
        default:
            break;
        }

        (void)OpeningBrace;
        while (!eof()) {
            const FormatToken *Previous = FormatTok->Previous;
            switch (FormatTok->Kind) {
            case tok::at:
                nextToken();
                if (FormatTok->is(tok::l_brace)) {
                    nextToken();
                    parseBracedList();
                }
                break;
            case tok::kw_enum:
                if (Previous && Previous->isOneOf(tok::less, tok::arrow, tok::comma)) {
                    nextToken();
                    break;
                }
                if (!parseEnum()) break;
                break;
            case tok::kw_typedef:
                nextToken();
                if (FormatTok->HasIdentifierInfo &&
                    (FormatTok->TokenText == "NS_ENUM" || FormatTok->TokenText == "NS_OPTIONS" ||
                     FormatTok->TokenText == "CF_ENUM" || FormatTok->TokenText == "CF_OPTIONS" ||
                     FormatTok->TokenText == "CF_CLOSED_ENUM" || FormatTok->TokenText == "NS_CLOSED_ENUM")) {
                    parseEnum();
                }
                break;
            case tok::kw_struct:
            case tok::kw_union:
                if (parseStructLike()) return;
                break;
            case tok::period:
                nextToken();
                break;
            case tok::semi:
                nextToken();
                addUnwrappedLine();
                return;
            case tok::r_brace:
                addUnwrappedLine();
                return;
            case tok::l_paren: {
                parseParens();
                if (OpeningBrace || !Previous || eof()) break;
                if (isC78ParameterDecl(FormatTok, Tokens->peekNextToken(/*SkipComment=*/true), Previous)) {
                    addUnwrappedLine();
                    return;
                }
                break;
            }
            case tok::caret: {
                const FormatToken *Prev = FormatTok->getPreviousNonComment();
                nextToken();
                if (Prev && Prev->is(tok::identifier)) break;
                if (FormatTok->isAnyIdentifier() || FormatTok->isTypeName()) {
                    nextToken();
                    while (FormatTok->is(tok::star)) nextToken();
                }
                if (FormatTok->is(tok::l_paren)) parseParens();
                if (FormatTok->is(tok::l_brace)) parseChildBlock();
                break;
            }
            case tok::l_brace:
                if (!tryToParseBracedList()) {
                    if (S.BraceWrappingAfterFunction) addUnwrappedLine();
                    if (!Previous || Previous->isNot(TT::TypeDeclarationParen))
                        FormatTok->setFinalizedType(TT::FunctionLBrace);
                    parseBlock();
                    addUnwrappedLine();
                    return;
                }
                break;
            case tok::identifier: {
                if (FormatTok->is(TT::StatementMacro)) {
                    parseStatementMacro();
                    return;
                }
                std::string_view Text = FormatTok->TokenText;
                FormatToken *PreviousToken = FormatTok;
                nextToken();
                auto OneTokenSoFar = [&]() {
                    auto I = Line->Tokens.begin(), E = Line->Tokens.end();
                    while (I != E && I->Tok->is(tok::comment)) ++I;
                    return I != E && (++I == E);
                };
                if (OneTokenSoFar()) {
                    bool FunctionLike = FormatTok->is(tok::l_paren);
                    if (FunctionLike) parseParens();
                    bool FollowedByNewline = CommentsBeforeNextToken.empty()
                                                 ? FormatTok->NewlinesBefore > 0
                                                 : CommentsBeforeNextToken.front()->NewlinesBefore > 0;
                    if (FollowedByNewline && (Text.size() >= 5 || (FunctionLike && FormatTok->isNot(tok::l_paren))) &&
                        tokenCanStartNewLine(*FormatTok) && isAllUpper(Text)) {
                        if (PreviousToken->isNot(TT::UntouchableMacroFunc))
                            PreviousToken->setFinalizedType(TT::FunctionLikeOrFreestandingMacro);
                        addUnwrappedLine();
                        return;
                    }
                }
                break;
            }
            case tok::equal:
                nextToken();
                if (FormatTok->is(tok::l_brace)) {
                    nextToken();
                    parseBracedList();
                }
                break;
            case tok::l_square:
                parseSquare();
                break;
            case tok::kw_switch:
                nextToken();
                break;
            case tok::kw_case:
                parseCaseLabel();
                break;
            case tok::kw_default:
                nextToken();
                break;
            case tok::colon:
                nextToken();
                break;
            case tok::greater:
                nextToken();
                if (FormatTok->is(tok::l_brace)) FormatTok->Previous->setFinalizedType(TT::TemplateCloser);
                break;
            default:
                nextToken();
                break;
            }
        }
    }

    bool parseModuleImport() {
        // `import` followed by an identifier, ':', '<' or a string: a C++20
        // module import (clang-format checks this in C mode too).
        FormatToken *Token = Tokens->peekNextToken(/*SkipComment=*/true);
        if (!Token->HasIdentifierInfo && !Token->isOneOf(tok::colon, tok::less, tok::string_literal)) return false;
        nextToken();
        while (!eof()) {
            if (FormatTok->is(tok::colon)) {
                // TT_ModulePartitionColon: not modelled; harmless in C.
            } else if (FormatTok->is(tok::less)) {
                nextToken();
                while (!FormatTok->isOneOf(tok::semi, tok::greater) && !eof()) {
                    if (FormatTok->isNot(tok::comment) && FormatTok->TokenText.substr(0, 2) != "//")
                        FormatTok->setFinalizedType(TT::ImplicitStringLiteral);
                    nextToken();
                }
            }
            if (FormatTok->is(tok::semi)) {
                nextToken();
                break;
            }
            nextToken();
        }
        addUnwrappedLine();
        return true;
    }

    bool tryToParseBracedList() {
        if (FormatTok->is(BK_Unknown)) calculateBraceTypes();
        if (FormatTok->is(BK_Block)) return false;
        nextToken();
        parseBracedList();
        return true;
    }

    bool parseBracedList(bool IsAngleBracket = false, bool IsEnum = false) {
        bool HasError = false;
        do {
            if (FormatTok->is(IsAngleBracket ? tok::greater : tok::r_brace)) {
                if (IsEnum) {
                    FormatTok->BlockKind = BK_Block;
                    if (!S.AllowShortEnumsOnASingleLine) addUnwrappedLine();
                }
                nextToken();
                return !HasError;
            }
            switch (FormatTok->Kind) {
            case tok::l_square:
                tryToParseLambda();
                break;
            case tok::l_paren:
                parseParens();
                break;
            case tok::l_brace:
                FormatTok->BlockKind = BK_BracedInit;
                if (!IsAngleBracket) {
                    FormatToken *Prev = FormatTok->Previous;
                    if (Prev && Prev->is(tok::greater)) Prev->setFinalizedType(TT::TemplateCloser);
                }
                nextToken();
                parseBracedList();
                break;
            case tok::less:
                nextToken();
                if (IsAngleBracket) parseBracedList(/*IsAngleBracket=*/true);
                break;
            case tok::semi:
                HasError = true;
                if (!IsEnum) return false;
                nextToken();
                break;
            case tok::comma:
                nextToken();
                if (IsEnum && !S.AllowShortEnumsOnASingleLine) addUnwrappedLine();
                break;
            default:
                nextToken();
                break;
            }
        } while (!eof());
        return false;
    }

    bool parseParens(TT AmpAmpTokenType = TT::Unknown) {
        assert(FormatTok->is(tok::l_paren));
        FormatToken *LParen = FormatTok;
        FormatToken *Prev = FormatTok->Previous;
        bool SeenEqual = false;
        nextToken();
        do {
            switch (FormatTok->Kind) {
            case tok::l_paren:
                if (parseParens(AmpAmpTokenType)) SeenEqual = true;
                break;
            case tok::r_paren: {
                FormatToken *RParen = FormatTok;
                nextToken();
                if (Prev && Prev->is(tok::greater) && RParen->Previous == LParen)
                    Prev->setFinalizedType(TT::TemplateCloser);
                return SeenEqual;
            }
            case tok::r_brace:
                return SeenEqual;
            case tok::l_square:
                tryToParseLambda();
                break;
            case tok::l_brace:
                if (!tryToParseBracedList()) parseChildBlock();
                break;
            case tok::at:
                nextToken();
                if (FormatTok->is(tok::l_brace)) {
                    nextToken();
                    parseBracedList();
                }
                break;
            case tok::equal:
                SeenEqual = true;
                nextToken();
                break;
            case tok::ampamp:
                if (AmpAmpTokenType != TT::Unknown) FormatTok->setFinalizedType(AmpAmpTokenType);
                [[fallthrough]];
            default:
                nextToken();
                break;
            }
        } while (!eof());
        return SeenEqual;
    }

    void parseSquare(bool LambdaIntroducer = false) {
        if (!LambdaIntroducer) {
            assert(FormatTok->is(tok::l_square));
            if (tryToParseLambda()) return;
        }
        do {
            switch (FormatTok->Kind) {
            case tok::l_paren:
                parseParens();
                break;
            case tok::r_square:
                nextToken();
                return;
            case tok::r_brace:
                return;
            case tok::l_square:
                parseSquare();
                break;
            case tok::l_brace:
                if (!tryToParseBracedList()) parseChildBlock();
                break;
            case tok::at:
            case tok::colon:
                nextToken();
                if (FormatTok->is(tok::l_brace)) {
                    nextToken();
                    parseBracedList();
                }
                break;
            default:
                nextToken();
                break;
            }
        } while (!eof());
    }

    bool tryToParseLambda() {
        assert(FormatTok->is(tok::l_square));
        FormatToken &LSquare = *FormatTok;
        if (!tryToParseLambdaIntroducer()) return false;
        FormatToken *Arrow = nullptr;
        bool InTemplateParameterList = false;
        while (FormatTok->isNot(tok::l_brace)) {
            if (FormatTok->isTypeName() || FormatTok->isAttribute()) {
                nextToken();
                continue;
            }
            switch (FormatTok->Kind) {
            case tok::l_paren:
                parseParens(/*AmpAmpTokenType=*/TT::PointerOrReference);
                break;
            case tok::l_square:
                parseSquare();
                break;
            case tok::less:
                if (FormatTok->Previous && FormatTok->Previous->is(tok::r_square)) InTemplateParameterList = true;
                nextToken();
                break;
            case tok::kw_auto:
            case tok::kw_struct:
            case tok::kw_union:
            case tok::amp:
            case tok::star:
            case tok::kw_const:
            case tok::kw_constexpr:
            case tok::comma:
            case tok::greater:
            case tok::identifier:
            case tok::numeric_constant:
            case tok::coloncolon:
            case tok::kw_static:
                nextToken();
                break;
            case tok::plus:
            case tok::minus:
            case tok::exclaim:
            case tok::tilde:
            case tok::slash:
            case tok::percent:
            case tok::lessless:
            case tok::pipe:
            case tok::pipepipe:
            case tok::ampamp:
            case tok::caret:
            case tok::equalequal:
            case tok::exclaimequal:
            case tok::greaterequal:
            case tok::lessequal:
            case tok::question:
            case tok::colon:
            case tok::ellipsis:
            case tok::kw_true:
            case tok::kw_false:
                if (Arrow || InTemplateParameterList) {
                    nextToken();
                    break;
                }
                return true;
            case tok::arrow:
                Arrow = FormatTok;
                nextToken();
                break;
            case tok::equal:
                if (!InTemplateParameterList) return true;
                nextToken();
                break;
            default:
                return true;
            }
        }
        FormatTok->setFinalizedType(TT::LambdaLBrace);
        LSquare.setFinalizedType(TT::LambdaLSquare);
        if (Arrow) Arrow->setFinalizedType(TT::LambdaArrow);
        parseChildBlock();
        return true;
    }

    bool tryToParseLambdaIntroducer() {
        const FormatToken *Previous = FormatTok->Previous;
        const FormatToken *LeftSquare = FormatTok;
        nextToken();
        if (Previous) {
            const FormatToken *PrevPrev = Previous->getPreviousNonComment();
            if (Previous->is(tok::star) && PrevPrev && PrevPrev->isTypeName()) return false;
            if (Previous->closesScope()) {
                if (Previous->isNot(tok::r_paren)) return false;
                if (!PrevPrev || !PrevPrev->isOneOf(tok::greater, tok::r_paren)) return false;
            }
            if (Previous->HasIdentifierInfo && !Previous->is(tok::kw_return)) return false;
        }
        // isCppStructuredBinding: `auto [`.
        {
            const FormatToken *T = LeftSquare;
            do {
                T = T->getPreviousNonComment();
            } while (T && T->isOneOf(tok::kw_const, tok::kw_volatile, tok::amp, tok::ampamp));
            if (T && T->is(tok::kw_auto)) return false;
        }
        if (FormatTok->is(tok::l_square) || FormatTok->isLiteral()) return false;
        if (FormatTok->is(tok::r_square)) {
            const FormatToken *Next = Tokens->peekNextToken(/*SkipComment=*/true);
            if (Next->is(tok::greater)) return false;
        }
        parseSquare(/*LambdaIntroducer=*/true);
        return true;
    }

    void parseUnbracedBody(bool CheckEOF = false) {
        addUnwrappedLine();
        ++Line->Level;
        ++Line->UnbracedBodyLevel;
        parseStructuralElement();
        --Line->UnbracedBodyLevel;
        if (CheckEOF && eof()) addUnwrappedLine();
        --Line->Level;
    }

    void handleAttributes() {
        if (FormatTok->isAttribute())
            nextToken();
        else if (FormatTok->is(tok::l_square))
            handleCppAttributes();
    }

    bool handleCppAttributes() {
        assert(FormatTok->is(tok::l_square));
        if (!tryToParseSimpleAttribute()) return false;
        parseSquare();
        return true;
    }

    bool tryToParseSimpleAttribute() {
        unsigned StoredPosition = Tokens->getPosition();
        auto Restore = [&] { Tokens->setPosition(StoredPosition); };
        FormatToken *Tok = Tokens->getNextToken();
        if (Tok->isNot(tok::l_square)) {
            Restore();
            return false;
        }
        while (Tok->isNot(tok::eof)) {
            if (Tok->is(tok::r_square)) break;
            Tok = Tokens->getNextToken();
        }
        if (Tok->is(tok::eof)) {
            Restore();
            return false;
        }
        Tok = Tokens->getNextToken();
        if (Tok->isNot(tok::r_square)) {
            Restore();
            return false;
        }
        Tok = Tokens->getNextToken();
        bool Result = Tok->isNot(tok::semi);
        Restore();
        return Result;
    }

    void parseIfThenElse() {
        assert(FormatTok->is(tok::kw_if));
        nextToken();
        if (FormatTok->is(tok::exclaim)) nextToken();
        if (FormatTok->isOneOf(tok::kw_constexpr, tok::identifier)) nextToken();
        if (FormatTok->is(tok::l_paren)) {
            FormatTok->setFinalizedType(TT::ConditionLParen);
            parseParens();
        }
        handleAttributes();
        bool NeedsUnwrappedLine = false;
        if (FormatTok->is(tok::l_brace)) {
            FormatTok->setFinalizedType(TT::ControlStatementLBrace);
            unsigned OldLevel = Line->Level;
            parseBlock(/*MustBeDeclaration=*/false, 1u, /*MunchSemi=*/true);
            Line->Level = OldLevel;
            setPreviousRBraceType(TT::ControlStatementRBrace);
            if (S.BraceWrappingBeforeElse)
                addUnwrappedLine();
            else
                NeedsUnwrappedLine = true;
        } else {
            parseUnbracedBody();
        }
        if (FormatTok->is(tok::kw_else)) {
            nextToken();
            handleAttributes();
            if (FormatTok->is(tok::l_brace)) {
                FormatTok->setFinalizedType(TT::ElseLBrace);
                unsigned OldLevel = Line->Level;
                parseBlock(/*MustBeDeclaration=*/false, 1u, /*MunchSemi=*/true);
                Line->Level = OldLevel;
                setPreviousRBraceType(TT::ElseRBrace);
                addUnwrappedLine();
            } else if (FormatTok->is(tok::kw_if)) {
                const FormatToken *Previous = Tokens->getPreviousToken();
                const bool IsPrecededByComment = Previous->is(tok::comment);
                if (IsPrecededByComment) {
                    addUnwrappedLine();
                    ++Line->Level;
                }
                parseIfThenElse();
                if (IsPrecededByComment) --Line->Level;
            } else {
                parseUnbracedBody(/*CheckEOF=*/true);
            }
        } else {
            if (NeedsUnwrappedLine) addUnwrappedLine();
        }
    }

    void parseTryCatch() {
        nextToken();
        bool NeedsUnwrappedLine = false;
        if (FormatTok->is(tok::l_brace)) {
            unsigned OldLevel = Line->Level;
            parseBlock();
            Line->Level = OldLevel;
            NeedsUnwrappedLine = true;
        } else {
            addUnwrappedLine();
            ++Line->Level;
            parseStructuralElement();
            --Line->Level;
        }
        for (;;) {
            if (FormatTok->is(tok::at)) nextToken();
            if (!(FormatTok->is(tok::kw___finally) ||
                  (FormatTok->is(tok::identifier) && FormatTok->TokenText == "__except")))
                break;
            nextToken();
            while (FormatTok->isNot(tok::l_brace)) {
                if (FormatTok->is(tok::l_paren)) {
                    parseParens();
                    continue;
                }
                if (FormatTok->isOneOf(tok::semi, tok::r_brace) || eof()) return;
                nextToken();
            }
            NeedsUnwrappedLine = false;
            Line->MustBeDeclaration = false;
            unsigned OldLevel = Line->Level;
            parseBlock();
            Line->Level = OldLevel;
            NeedsUnwrappedLine = true;
        }
        if (NeedsUnwrappedLine) addUnwrappedLine();
    }

    void parseLoopBody(bool WrapRightBrace) {
        if (FormatTok->is(tok::l_brace)) {
            FormatTok->setFinalizedType(TT::ControlStatementLBrace);
            unsigned OldLevel = Line->Level;
            parseBlock(/*MustBeDeclaration=*/false, 1u, /*MunchSemi=*/true);
            Line->Level = OldLevel;
            setPreviousRBraceType(TT::ControlStatementRBrace);
            if (WrapRightBrace) addUnwrappedLine();
        } else {
            parseUnbracedBody();
        }
    }

    void parseForOrWhileLoop() {
        nextToken();
        if (FormatTok->is(tok::l_paren)) parseParens();
        if (S.AllowShortLoopsOnASingleLine && FormatTok->is(tok::semi) &&
            Tokens->getPreviousToken()->is(tok::r_paren)) {
            nextToken();
            addUnwrappedLine();
            return;
        }
        handleAttributes();
        parseLoopBody(/*WrapRightBrace=*/true);
    }

    void parseDoWhile() {
        nextToken();
        parseLoopBody(S.BraceWrappingBeforeWhile);
        if (FormatTok->isNot(tok::kw_while)) {
            addUnwrappedLine();
            return;
        }
        FormatTok->setFinalizedType(TT::DoWhile);
        nextToken();
        parseStructuralElement();
    }

    void parseLabel(bool LeftAlignLabel = false) {
        nextToken();
        unsigned OldLineLevel = Line->Level;
        if (LeftAlignLabel)
            Line->Level = 0;
        else if (Line->Level > 1 || (!Line->InPPDirective && Line->Level > 0))
            --Line->Level;
        if (CommentsBeforeNextToken.empty() && FormatTok->is(tok::l_brace)) {
            unsigned OldLevel = Line->Level;
            if (S.BraceWrappingAfterCaseLabel) addUnwrappedLine();
            parseBlock();
            if (FormatTok->is(tok::kw_break)) parseStructuralElement();
            addUnwrappedLine();
            Line->Level = OldLevel;
        } else {
            if (FormatTok->is(tok::semi)) nextToken();
            addUnwrappedLine();
        }
        Line->Level = OldLineLevel;
        if (FormatTok->isNot(tok::l_brace)) {
            parseStructuralElement();
            addUnwrappedLine();
        }
    }

    void parseCaseLabel() {
        assert(FormatTok->is(tok::kw_case));
        do {
            nextToken();
            if (FormatTok->is(tok::colon)) {
                FormatTok->setFinalizedType(TT::CaseLabelColon);
                break;
            }
        } while (!eof());
        parseLabel();
    }

    void parseSwitch() {
        nextToken();
        if (FormatTok->is(tok::l_paren)) parseParens();
        if (FormatTok->is(tok::l_brace)) {
            unsigned OldLevel = Line->Level;
            FormatTok->setFinalizedType(TT::ControlStatementLBrace);
            parseBlock();
            Line->Level = OldLevel;
            setPreviousRBraceType(TT::ControlStatementRBrace);
            addUnwrappedLine();
        } else {
            addUnwrappedLine();
            ++Line->Level;
            parseStructuralElement();
            --Line->Level;
        }
    }

    bool parseEnum() {
        if (FormatTok->is(tok::kw_enum)) nextToken();
        if (FormatTok->is(tok::kw_struct)) nextToken();
        while (FormatTok->is(tok::l_square))
            if (!handleCppAttributes()) return false;
        while (FormatTok->HasIdentifierInfo ||
               FormatTok->isOneOf(tok::colon, tok::coloncolon, tok::less, tok::greater, tok::comma, tok::question,
                                  tok::l_square)) {
            nextToken();
            if (FormatTok->is(tok::l_paren)) parseParens();
            if (FormatTok->is(tok::identifier)) {
                nextToken();
                if (FormatTok->is(tok::identifier)) return false;
            }
        }
        if (FormatTok->isNot(tok::l_brace)) return true;
        FormatTok->setFinalizedType(TT::EnumLBrace);
        FormatTok->BlockKind = BK_Block;
        if (!S.AllowShortEnumsOnASingleLine && S.BraceWrappingAfterEnum) addUnwrappedLine();
        nextToken();
        if (!S.AllowShortEnumsOnASingleLine) {
            addUnwrappedLine();
            Line->Level += 1;
        }
        bool HasError = !parseBracedList(/*IsAngleBracket=*/false, /*IsEnum=*/true);
        if (!S.AllowShortEnumsOnASingleLine) Line->Level -= 1;
        if (HasError) {
            if (FormatTok->is(tok::semi)) nextToken();
            addUnwrappedLine();
        }
        setPreviousRBraceType(TT::EnumRBrace);
        return true;
    }

    bool parseStructLike() {
        parseRecord();
        return false;
    }

    void parseRecord() {
        const FormatToken &InitialToken = *FormatTok;
        nextToken();
        FormatToken *ClassName = nullptr;
        bool IsDerived = false;
        auto IsNonMacroIdentifier = [](const FormatToken *Tok) {
            return Tok->is(tok::identifier) && !isAllUpper(Tok->TokenText);
        };
        while (FormatTok->isOneOf(tok::identifier, tok::coloncolon, tok::hashhash, tok::kw_alignas, tok::l_square) ||
               FormatTok->isAttribute()) {
            if (FormatTok->is(tok::l_square) && handleCppAttributes()) continue;
            FormatToken *Previous = FormatTok;
            nextToken();
            switch (FormatTok->Kind) {
            case tok::l_paren:
                if (!IsNonMacroIdentifier(Previous) || Previous->Previous == &InitialToken) parseParens();
                break;
            case tok::coloncolon:
            case tok::hashhash:
                break;
            default:
                if (ClassName || Previous->isNot(tok::identifier) || Previous->is(TT::AttributeMacro)) break;
                if (Previous->TokenText.size() == 1 || !isAllUpper(Previous->TokenText)) ClassName = Previous;
            }
        }
        auto IsListInitialization = [&] {
            if (!ClassName || IsDerived) return false;
            const FormatToken *Prev = FormatTok->getPreviousNonComment();
            return Prev != ClassName && Prev->is(tok::identifier) && tryToParseBracedList();
        };
        if (FormatTok->isOneOf(tok::colon, tok::less)) {
            int AngleNestingLevel = 0;
            do {
                if (FormatTok->is(tok::less))
                    ++AngleNestingLevel;
                else if (FormatTok->is(tok::greater))
                    --AngleNestingLevel;
                if (AngleNestingLevel == 0) {
                    if (FormatTok->is(tok::colon)) {
                        IsDerived = true;
                    } else if (!IsDerived && FormatTok->is(tok::identifier) && FormatTok->Previous &&
                               FormatTok->Previous->is(tok::coloncolon)) {
                        ClassName = FormatTok;
                    } else if (FormatTok->is(tok::l_paren) && IsNonMacroIdentifier(FormatTok->Previous)) {
                        break;
                    }
                }
                if (FormatTok->is(tok::l_brace)) {
                    if (AngleNestingLevel == 0 && IsListInitialization()) return;
                    calculateBraceTypes(/*ExpectClassBody=*/true);
                    if (!tryToParseBracedList()) break;
                }
                if (FormatTok->is(tok::l_square)) {
                    FormatToken *Previous = FormatTok->Previous;
                    if (!Previous || (Previous->isNot(tok::r_paren) && !Previous->isTypeOrIdentifier())) {
                        if (!tryToParseLambda()) continue;
                    } else {
                        parseSquare();
                        continue;
                    }
                }
                if (FormatTok->is(tok::semi)) return;
                nextToken();
            } while (!eof());
        }
        if (FormatTok->is(tok::l_brace)) {
            if (IsListInitialization()) return;
            if (ClassName) ClassName->setFinalizedType(TT::ClassHeadName);
            TT Open = InitialToken.is(tok::kw_struct) ? TT::StructLBrace
                      : InitialToken.is(tok::kw_union) ? TT::UnionLBrace
                                                       : TT::RecordLBrace;
            TT Close = InitialToken.is(tok::kw_struct) ? TT::StructRBrace
                       : InitialToken.is(tok::kw_union) ? TT::UnionRBrace
                                                        : TT::RecordRBrace;
            FormatTok->setFinalizedType(Open);
            bool Wrap = InitialToken.is(tok::kw_struct) ? S.BraceWrappingAfterStruct
                        : InitialToken.is(tok::kw_union) ? S.BraceWrappingAfterUnion
                                                         : false;
            if (Wrap) addUnwrappedLine();
            parseBlock(/*MustBeDeclaration=*/true, 1u, /*MunchSemi=*/false);
            setPreviousRBraceType(Close);
        }
    }

    void parseStatementMacro() {
        nextToken();
        if (FormatTok->is(tok::l_paren)) parseParens();
        if (FormatTok->is(tok::semi)) nextToken();
        addUnwrappedLine();
    }

    void addUnwrappedLine() {
        if (Line->Tokens.empty()) return;
        CurrentLines->push_back(std::move(*Line));
        Line->Tokens.clear();
        Line->MatchingOpeningBlockLineIndex = UnwrappedLine::kInvalidIndex;
        Line->IsContinuation = false;
        if (!parsingPPDirective() && !PreprocessorDirectives.empty()) {
            for (auto &L : PreprocessorDirectives) CurrentLines->push_back(std::move(L));
            PreprocessorDirectives.clear();
        }
        FormatTok->Previous = nullptr;
    }

    bool eof() const { return FormatTok->is(tok::eof); }

    bool isOnNewLine(const FormatToken &Tok) const {
        return (Line->InPPDirective || Tok.HasUnescapedNewline) && Tok.NewlinesBefore > 0;
    }

    bool continuesLineCommentSection(const FormatToken &Tok, const UnwrappedLine &L) const {
        if (L.Tokens.empty() || !S.ReflowComments) return false;
        std::string_view IndentContent = Tok.TokenText;
        if (IndentContent.substr(0, 2) == "//" || IndentContent.substr(0, 2) == "/*")
            IndentContent = IndentContent.substr(2);
        if (IndentContent.substr(0, 13) == " IWYU pragma:") return false;
        const FormatToken *MinColumnToken = L.Tokens.front().Tok;
        const FormatToken *PreviousToken = nullptr;
        for (const UnwrappedLineNode &Node : L.Tokens) {
            if (PreviousToken && PreviousToken->is(tok::l_brace) && isLineComment(*Node.Tok)) {
                MinColumnToken = PreviousToken;
                break;
            }
            PreviousToken = Node.Tok;
            if (Node.Tok->NewlinesBefore > 0) MinColumnToken = Node.Tok;
        }
        if (PreviousToken && PreviousToken->is(tok::l_brace)) MinColumnToken = PreviousToken;
        return continuesLineComment(Tok, L.Tokens.back().Tok, MinColumnToken);
    }

    void flushComments(bool NewlineBeforeNext) {
        bool JustComments = Line->Tokens.empty();
        for (FormatToken *Tok : CommentsBeforeNextToken) {
            Tok->ContinuesLineCommentSection = continuesLineCommentSection(*Tok, *Line);
            if (isOnNewLine(*Tok) && JustComments && !Tok->ContinuesLineCommentSection) addUnwrappedLine();
            pushToken(Tok);
        }
        if (NewlineBeforeNext && JustComments) addUnwrappedLine();
        CommentsBeforeNextToken.clear();
    }

    void nextToken(int LevelDifference = 0) {
        if (eof()) return;
        flushComments(isOnNewLine(*FormatTok));
        pushToken(FormatTok);
        FormatToken *Previous = FormatTok;
        readToken(LevelDifference);
        FormatTok->Previous = Previous;
    }

    void distributeComments(const std::vector<FormatToken *> &Comments, const FormatToken *NextTok) {
        if (Comments.empty()) return;
        bool ShouldPushCommentsInCurrentLine = true;
        bool HasTrailAlignedWithNextToken = false;
        unsigned StartOfTrailAlignedWithNextToken = 0;
        if (NextTok) {
            for (unsigned i = static_cast<unsigned>(Comments.size()) - 1; i > 0; --i) {
                if (Comments[i]->OriginalColumn == NextTok->OriginalColumn) {
                    HasTrailAlignedWithNextToken = true;
                    StartOfTrailAlignedWithNextToken = i;
                }
            }
        }
        for (unsigned i = 0, e = static_cast<unsigned>(Comments.size()); i < e; ++i) {
            FormatToken *Tok = Comments[i];
            if (HasTrailAlignedWithNextToken && i == StartOfTrailAlignedWithNextToken)
                Tok->ContinuesLineCommentSection = false;
            else
                Tok->ContinuesLineCommentSection = continuesLineCommentSection(*Tok, *Line);
            if (!Tok->ContinuesLineCommentSection && (isOnNewLine(*Tok) || Tok->IsFirst))
                ShouldPushCommentsInCurrentLine = false;
            if (ShouldPushCommentsInCurrentLine)
                pushToken(Tok);
            else
                CommentsBeforeNextToken.push_back(Tok);
        }
    }

    void readToken(int LevelDifference = 0) {
        std::vector<FormatToken *> Comments;
        bool PreviousWasComment = false;
        bool FirstNonCommentOnLine = false;
        do {
            FormatTok = Tokens->getNextToken();
            auto IsFirstNonCommentOnLine = [](bool FirstNonCommentOnLine, const FormatToken &Tok,
                                              bool PreviousWasComment) {
                auto IsFirstOnLine = [](const FormatToken &Tok) { return Tok.HasUnescapedNewline || Tok.IsFirst; };
                if (PreviousWasComment) return FirstNonCommentOnLine || IsFirstOnLine(Tok);
                return IsFirstOnLine(Tok);
            };
            FirstNonCommentOnLine = IsFirstNonCommentOnLine(FirstNonCommentOnLine, *FormatTok, PreviousWasComment);
            PreviousWasComment = FormatTok->is(tok::comment);
            while (!Line->InPPDirective && FormatTok->is(tok::hash) && FirstNonCommentOnLine) {
                distributeComments(Comments, FormatTok);
                Comments.clear();
                bool SwitchToPreprocessorLines = !Line->Tokens.empty();
                ScopedLineState BlockState(*this, SwitchToPreprocessorLines);
                Line->Level = static_cast<unsigned>(static_cast<int>(Line->Level) + LevelDifference);
                Line->Level -= Line->UnbracedBodyLevel;
                flushComments(isOnNewLine(*FormatTok));
                parsePPDirective();
                PreviousWasComment = FormatTok->is(tok::comment);
                FirstNonCommentOnLine = IsFirstNonCommentOnLine(FirstNonCommentOnLine, *FormatTok, PreviousWasComment);
            }
            if (!PPStack.empty() && PPStack.back().K == PPBranch::PP_Unreachable && !Line->InPPDirective) continue;
            if (FormatTok->isNot(tok::comment)) {
                distributeComments(Comments, FormatTok);
                Comments.clear();
                return;
            }
            Comments.push_back(FormatTok);
        } while (!eof());
        distributeComments(Comments, nullptr);
        Comments.clear();
    }

    void pushToken(FormatToken *Tok) {
        Line->Tokens.emplace_back(Tok);
        if (AtEndOfPPLine) {
            FormatToken &T = *Line->Tokens.back().Tok;
            T.MustBreakBefore = true;
            T.MustBreakBeforeFinalized = true;
            T.FirstAfterPPLine = true;
            AtEndOfPPLine = false;
        }
    }

    Source &Src;
    const Style &S;
    const RunCallback &OnRun;
    const std::vector<FormatToken *> &AllTokens;
    std::unique_ptr<UnwrappedLine> Line = std::make_unique<UnwrappedLine>();
    std::vector<UnwrappedLine> Lines;
    std::vector<UnwrappedLine> PreprocessorDirectives;
    std::vector<UnwrappedLine> *CurrentLines = &Lines;
    std::vector<FormatToken *> CommentsBeforeNextToken;
    FormatToken *FormatTok = nullptr;
    bool AtEndOfPPLine = false;
    std::vector<bool> DeclarationScopeStack;
    TokenSource *Tokens = nullptr;
    std::vector<PPBranch> PPStack;
    int PPBranchLevel = -1;
    std::vector<int> PPLevelBranchIndex;
    std::vector<int> PPLevelBranchCount;
    std::stack<int> PPChainBranchIndex;
};

}  // namespace

void ParseUnwrappedLines(Source &Src, const Style &S, const RunCallback &OnRun) {
    UnwrappedLineParser P(Src, S, OnRun);
    P.parse();
}

}  // namespace cfmt::detail
