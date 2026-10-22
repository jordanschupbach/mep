// SyntaxParser, the base of mep's C# parsers: a translation of Roslyn's
// src/Compilers/CSharp/Portable/Parser/SyntaxParser.cs (MIT, (c) .NET
// Foundation and Contributors), see csharp_parser_internal.h.
//
// Two token streams, as in Roslyn: a plain array of lexed tokens (the
// language and directive parsers, which never change the lexer mode), and,
// for a parser that may (the doc-comment parser), the "blended" stream --
// with no old tree to reuse, Roslyn's Blender just lexes, but each token
// remembers where the lexer stood after it, so a mode change re-lexes from
// there.
#include <algorithm>
#include <string>

#include "csharp_parser_internal.h"

namespace csharp {

// --- Stack guard -------------------------------------------------------------------------

void EnsureSufficientExecutionStack(int recursionDepth) {
    // Roslyn checks the real stack once recursion passes a small depth and throws
    // InsufficientExecutionStackException when little is left. The parser runs on a thread with a large
    // stack (csharp_parser.cpp); this bound keeps it well inside it.
    constexpr int kMaxDepth = 4000;
    if (recursionDepth > kMaxDepth) throw InsufficientExecutionStack();
}

// --- Construction ------------------------------------------------------------------------------

SyntaxParser::SyntaxParser(Lexer *lexer_, LexerMode mode, bool allowModeReset)
    : lexer(lexer_), _allowModeReset(allowModeReset), _mode(mode) {
    if (_allowModeReset) _firstBlender = BlenderState{lexer->TextWindow.Position(), DirectiveStack(), LexerMode::None};
    // Roslyn pre-lexes up to 4096 tokens here for the language parser; lexing is the same whenever it happens.
}

SyntaxParser::~SyntaxParser() = default;

void SyntaxParser::ReInitialize() {
    _firstToken = 0;
    _tokenOffset = 0;
    _tokenCount = 0;
    _resetCount = 0;
    _resetStart = 0;
    _currentToken = nullptr;
    _prevTokenTrailingTrivia = nullptr;
    _lexedTokens.clear();
    _blendedTokens.clear();
    if (_allowModeReset) _firstBlender = BlenderState{lexer->TextWindow.Position(), DirectiveStack(), LexerMode::None};
}

// --- Reset points ----------------------------------------------------------------------------------

SyntaxParser::ResetPoint SyntaxParser::GetResetPoint() {
    int pos = CurrentTokenPosition();
    if (_resetCount == 0) _resetStart = pos;  // low water mark
    _resetCount++;
    return ResetPoint{_resetCount, _mode, pos, _prevTokenTrailingTrivia};
}

void SyntaxParser::Reset(ResetPoint &point) {
    int offset = point.Position - _firstToken;
    CS_ASSERT(offset >= 0);
    if (offset >= _tokenCount) {
        // Re-fetch tokens to the position in the reset point.
        PeekToken(offset - _tokenOffset);
        offset = point.Position - _firstToken;
    }
    _mode = point.Mode;
    _tokenOffset = offset;
    _currentToken = nullptr;
    _prevTokenTrailingTrivia = point.PrevTokenTrailingTrivia;
    // (Roslyn then drops blended slots holding nodes rather than tokens; without an old tree there are none.)
}

void SyntaxParser::Release(ResetPoint &point) {
    CS_ASSERT(_resetCount == point.ResetCount);
    (void)point;
    _resetCount--;
    if (_resetCount == 0) _resetStart = -1;
}

void SyntaxParser::SetMode(LexerMode value) {
    if (_mode != value) {
        CS_ASSERT(_allowModeReset);
        _mode = value;
        _currentToken = nullptr;
        _tokenCount = _tokenOffset;
    }
}

// --- The token stream ------------------------------------------------------------------------------

GreenNode *SyntaxParser::CurrentToken() {
    if (_currentToken == nullptr) _currentToken = FetchCurrentToken();
    return _currentToken;
}

GreenNode *SyntaxParser::FetchCurrentToken() {
    if (_tokenOffset >= _tokenCount) AddNewToken();
    if (_allowModeReset) return _blendedTokens[static_cast<size_t>(_tokenOffset)].Token;
    return _lexedTokens[static_cast<size_t>(_tokenOffset)];
}

SyntaxParser::BlendedToken SyntaxParser::ReadBlendedToken(const BlenderState &from, LexerMode mode) {
    // Blender.Reader.LexNewToken with no old tree.
    if (lexer->TextWindow.Position() != from.NewPosition) lexer->Reset(from.NewPosition, from.NewDirectives);
    if (mode >= LexerMode::XmlDocComment) mode = mode | from.NewLexerDrivenMode;
    GreenNode *token = lexer->LexRef(mode);
    BlendedToken result;
    result.Token = token;
    result.NewDirectives = lexer->Directives();
    result.NewLexerDrivenMode = mode & (LexerMode::MaskXmlDocCommentLocation | LexerMode::MaskXmlDocCommentStyle);
    result.NewPosition = from.NewPosition + token->full_width;
    return result;
}

void SyntaxParser::AddNewToken() {
    if (_allowModeReset) {
        BlenderState from;
        if (_tokenCount > 0) {
            const BlendedToken &prev = _blendedTokens[static_cast<size_t>(_tokenCount - 1)];
            from = BlenderState{prev.NewPosition, prev.NewDirectives, prev.NewLexerDrivenMode};
        } else {
            from = _firstBlender;
        }
        BlendedToken t = ReadBlendedToken(from, _mode);
        if (static_cast<size_t>(_tokenCount) >= _blendedTokens.size()) {
            _blendedTokens.resize(static_cast<size_t>(_tokenCount) + 1);
        }
        _blendedTokens[static_cast<size_t>(_tokenCount)] = t;
        _tokenCount++;
    } else {
        AddLexedToken(lexer->Lex(_mode));
    }
}

void SyntaxParser::AddLexedToken(GreenNode *token) {
    if (static_cast<size_t>(_tokenCount) >= _lexedTokens.size()) {
        _lexedTokens.resize(static_cast<size_t>(_tokenCount) + 1);
    }
    _lexedTokens[static_cast<size_t>(_tokenCount)] = token;
    _tokenCount++;
}

GreenNode *SyntaxParser::PeekToken(int n) {
    CS_ASSERT(n >= 0);
    while (_tokenOffset + n >= _tokenCount) AddNewToken();
    if (_allowModeReset) return _blendedTokens[static_cast<size_t>(_tokenOffset + n)].Token;
    return _lexedTokens[static_cast<size_t>(_tokenOffset + n)];
}

GreenNode *SyntaxParser::EatToken() {
    GreenNode *ct = CurrentToken();
    MoveToNextToken();
    return ct;
}

GreenNode *SyntaxParser::TryEatToken(SyntaxKind kind) { return CurrentToken()->kind == kind ? EatToken() : nullptr; }

void SyntaxParser::MoveToNextToken() {
    _prevTokenTrailingTrivia = _currentToken->GetTrailingTrivia();
    _currentToken = nullptr;
    _tokenOffset++;
}

void SyntaxParser::ForceEndOfFile() { _currentToken = SyntaxFactory::Token(SyntaxKind::EndOfFileToken); }

GreenNode *SyntaxParser::EatToken(SyntaxKind kind) {
    GreenNode *ct = CurrentToken();
    if (ct->kind == kind) {
        MoveToNextToken();
        return ct;
    }
    return CreateMissingToken(kind, CurrentToken()->kind);
}

GreenNode *SyntaxParser::EatTokenAsKind(SyntaxKind expected) {
    GreenNode *ct = CurrentToken();
    if (ct->kind == expected) {
        MoveToNextToken();
        return ct;
    }
    GreenNode *replacement = CreateMissingToken(expected, CurrentToken()->kind);
    return AddTrailingSkippedSyntax(replacement, EatToken());
}

GreenNode *SyntaxParser::CreateMissingToken(SyntaxKind expected, SyntaxKind actual) {
    GreenNode *token = SyntaxFactory::MissingToken(expected);
    return WithAdditionalDiagnostics(token, GetExpectedMissingNodeOrTokenError(token, expected, actual));
}

GreenNode *SyntaxParser::CreateMissingToken(SyntaxKind expected, ErrorCode code, bool reportError) {
    GreenNode *token = SyntaxFactory::MissingToken(expected);
    if (reportError) token = AddError(token, code);
    return token;
}

GreenNode *SyntaxParser::EatToken(SyntaxKind kind, bool reportError) {
    if (reportError) return EatToken(kind);
    if (CurrentToken()->kind != kind) return SyntaxFactory::MissingToken(kind);
    return EatToken();
}

GreenNode *SyntaxParser::EatToken(SyntaxKind kind, ErrorCode code, bool reportError) {
    if (CurrentToken()->kind != kind) return CreateMissingToken(kind, code, reportError);
    return EatToken();
}

GreenNode *SyntaxParser::EatTokenEvenWithIncorrectKind(SyntaxKind kind) {
    GreenNode *token = CurrentToken();
    if (token->kind != kind) {
        // Same line as the last token: squiggle this one as the wrong kind. Next line: a zero-width squiggle at
        // the end of the previous token.
        int offset = 0;
        int width = token->Width();
        if (SyntaxList(_prevTokenTrailingTrivia).Any(SyntaxKind::EndOfLineTrivia)) {
            offset = -(_prevTokenTrailingTrivia->full_width + token->GetLeadingTriviaWidth());
            width = 0;
        }
        token = WithAdditionalDiagnostics(token, GetExpectedTokenError(kind, token->kind, offset, width));
    }
    MoveToNextToken();
    return token;
}

GreenNode *SyntaxParser::EatTokenWithPrejudice(ErrorCode errorCode, std::initializer_list<Text> args) {
    GreenNode *token = EatToken();
    return WithAdditionalDiagnostics(token, MakeError(0, token->Width(), errorCode, args));
}

GreenNode *SyntaxParser::EatContextualToken(SyntaxKind kind, ErrorCode code) {
    if (CurrentToken()->contextual_kind != kind) return CreateMissingToken(kind, code, true);
    return ConvertToKeyword(EatToken());
}

GreenNode *SyntaxParser::EatContextualToken(SyntaxKind kind) {
    SyntaxKind contextualKind = CurrentToken()->contextual_kind;
    if (contextualKind != kind) return CreateMissingToken(kind, contextualKind);
    return ConvertToKeyword(EatToken());
}

SyntaxDiagnosticInfo SyntaxParser::GetExpectedTokenError(SyntaxKind expected, SyntaxKind actual, int offset, int width) {
    ErrorCode code = GetExpectedTokenErrorCode(expected, actual);
    if (code == ErrorCode::ERR_SyntaxError) return MakeError(offset, width, code, {SyntaxFacts::GetText(expected)});
    if (code == ErrorCode::ERR_IdentifierExpectedKW) {
        return MakeError(offset, width, code, {Text(), SyntaxFacts::GetText(actual)});
    }
    return MakeError(offset, width, code);
}

SyntaxDiagnosticInfo SyntaxParser::GetExpectedMissingNodeOrTokenError(GreenNode *missingNodeOrToken, SyntaxKind expected,
                                                                    SyntaxKind actual) {
    auto [offset, width] = GetDiagnosticSpanForMissingNodeOrToken(missingNodeOrToken);
    return GetExpectedTokenError(expected, actual, offset, width);
}

ErrorCode SyntaxParser::GetExpectedTokenErrorCode(SyntaxKind expected, SyntaxKind actual) {
    switch (expected) {
    case SyntaxKind::IdentifierToken:
        return SyntaxFacts::IsReservedKeyword(actual) ? ErrorCode::ERR_IdentifierExpectedKW
                                                      : ErrorCode::ERR_IdentifierExpected;
    case SyntaxKind::SemicolonToken:
        return ErrorCode::ERR_SemicolonExpected;
    case SyntaxKind::CloseParenToken:
        return ErrorCode::ERR_CloseParenExpected;
    case SyntaxKind::OpenBraceToken:
        return ErrorCode::ERR_LbraceExpected;
    case SyntaxKind::CloseBraceToken:
        return ErrorCode::ERR_RbraceExpected;
    default:
        return ErrorCode::ERR_SyntaxError;
    }
}

// --- Diagnostics ------------------------------------------------------------------------------------

GreenNode *SyntaxParser::WithAdditionalDiagnostics(GreenNode *node, const SyntaxDiagnosticInfo &diagnostic) {
    return csharp::WithAdditionalDiagnostics(node, diagnostic);
}

GreenNode *SyntaxParser::AddError(GreenNode *node, ErrorCode code) { return AddError(node, code, {}); }

GreenNode *SyntaxParser::AddErrorAsWarning(GreenNode *node, ErrorCode code, std::initializer_list<Text> args) {
    // WRN_ErrorOverride: "{0}. See also error CS{1}." with the inner message and number.
    SyntaxDiagnosticInfo inner = MakeError(node, code, args);
    std::u16string message = ToUtf16(FormatDiagnosticMessage(inner));
    return AddError(node, ErrorCode::WRN_ErrorOverride,
                    {ArgText(message), ArgInt(static_cast<int>(code))});
}

GreenNode *SyntaxParser::AddError(GreenNode *nodeOrToken, ErrorCode code, std::initializer_list<Text> args) {
    if (!nodeOrToken->IsMissing()) {
        // A real node or token (or the EOF token): the diagnostic covers its width, from its start.
        return WithAdditionalDiagnostics(nodeOrToken, MakeError(nodeOrToken, code, args));
    }
    auto [offset, width] = GetDiagnosticSpanForMissingNodeOrToken(nodeOrToken);
    return WithAdditionalDiagnostics(nodeOrToken, MakeError(offset, width, code, args));
}

std::pair<int, int> SyntaxParser::GetDiagnosticSpanForMissingNodeOrToken(GreenNode *missingNodeOrToken) {
    CS_ASSERT(missingNodeOrToken->IsMissing());
    if (!missingNodeOrToken->ContainsSkippedText()) {
        // No skipped text: on the token that follows, or, when the previous token ends its line, a zero-width
        // diagnostic at the end of that line.
        GreenNode *trivia = _prevTokenTrailingTrivia;
        if (SyntaxList(trivia).Any(SyntaxKind::EndOfLineTrivia)) {
            return {-missingNodeOrToken->GetLeadingTriviaWidth() - trivia->full_width, 0};
        }
        GreenNode *token = CurrentToken();
        return {missingNodeOrToken->Width() + missingNodeOrToken->GetTrailingTriviaWidth() +
                    token->GetLeadingTriviaWidth(),
                token->Width()};
    }
    // Skipped text inside: on the first skipped token.
    int offset = 0;
    for (GreenNode *child : EnumerateNodes(missingNodeOrToken)) {
        if (!child->IsToken()) continue;
        if (!child->ContainsSkippedText()) {
            offset += child->full_width;
            continue;
        }
        std::vector<GreenNode *> all;
        SyntaxList leading(child->GetLeadingTrivia()), trailing(child->GetTrailingTrivia());
        for (int i = 0; i < leading.Count(); i++) all.push_back(leading[i]);
        for (int i = 0; i < trailing.Count(); i++) all.push_back(trailing[i]);
        for (GreenNode *trivia : all) {
            if (!trivia->IsSkippedTokensTrivia()) {
                offset += trivia->full_width;
                continue;
            }
            return {offset, trivia->Width()};
        }
        return {0, 0};  // unreachable in Roslyn
    }
    return {0, 0};  // unreachable in Roslyn
}

GreenNode *SyntaxParser::AddError(GreenNode *node, int offset, int length, ErrorCode code,
                                  std::initializer_list<Text> args) {
    return WithAdditionalDiagnostics(node, MakeError(offset, length, code, args));
}

GreenNode *SyntaxParser::AddErrorToFirstToken(GreenNode *node, ErrorCode code, std::initializer_list<Text> args) {
    GreenNode *firstToken = node->GetFirstToken();
    return WithAdditionalDiagnostics(node, MakeError(0, firstToken->Width(), code, args));
}

GreenNode *SyntaxParser::AddErrorToLastToken(GreenNode *node, ErrorCode code) {
    int offset = 0, width = 0;
    GetOffsetAndWidthForLastToken(node, offset, width);
    return WithAdditionalDiagnostics(node, MakeError(offset, width, code));
}

void SyntaxParser::GetOffsetAndWidthForLastToken(GreenNode *node, int &offset, int &width) {
    GreenNode *lastToken = node->GetLastNonmissingToken();
    offset = node->Width() + node->GetTrailingTriviaWidth();  // the end of the node
    width = 0;
    if (lastToken != nullptr) {
        offset -= lastToken->full_width;
        offset += lastToken->GetLeadingTriviaWidth();
        width = lastToken->Width();
    }
}

SyntaxDiagnosticInfo SyntaxParser::MakeError(int offset, int width, ErrorCode code, std::initializer_list<Text> args) {
    SyntaxDiagnosticInfo d;
    d.offset = offset;
    d.width = width;
    d.code = code;
    for (Text a : args) {
        if (d.arg_count < 4) d.args[d.arg_count++] = a;
    }
    return d;
}

SyntaxDiagnosticInfo SyntaxParser::MakeError(GreenNode *node, ErrorCode code, std::initializer_list<Text> args) {
    return MakeError(0, node->Width(), code, args);
}

SyntaxDiagnosticInfo SyntaxParser::MakeError(ErrorCode code, std::initializer_list<Text> args) {
    return MakeError(0, 0, code, args);
}

Text SyntaxParser::ArgInt(int64_t value) { return CurrentArena()->CopyText(ToUtf16(std::to_string(value))); }

Text SyntaxParser::ArgKind(SyntaxKind kind) { return CurrentArena()->CopyText(ToUtf16(KindName(kind))); }

// --- Skipped syntax ------------------------------------------------------------------------------

namespace {

/** @brief The rewriter Roslyn's SyntaxFirstTokenReplacer is: swaps the first token, shifting diagnostics on the
 *         nodes it rebuilds by `delta`. */
GreenNode *ReplaceFirstToken(GreenNode *node, GreenNode *newToken, int delta, bool &found) {
    if (node == nullptr || found) return node;
    if (node->IsToken()) {
        found = true;
        return newToken;
    }
    // Visit the slots in order until the token is found; rebuild this node if a child changed.
    GreenNode *result = node;
    for (int i = 0; i < node->slot_count && !found; i++) {
        GreenNode *child = node->slots[i];
        if (child == nullptr) continue;
        GreenNode *replaced = ReplaceFirstToken(child, newToken, delta, found);
        if (replaced != child) result = WithSlot(result, i, replaced);
    }
    // UpdateDiagnosticOffset(base.Visit(node), delta)
    if (result->diag_count > 0) {
        std::vector<SyntaxDiagnosticInfo> diags(result->diags, result->diags + result->diag_count);
        for (SyntaxDiagnosticInfo &d : diags) d.offset += delta;
        result = WithDiagnosticsGreen(result, diags.data(), static_cast<int>(diags.size()));
    }
    return result;
}

int CountChildNodesAndTokens(const GreenNode *node) {
    // ChildNodesAndTokens().Count: slots, with lists counted by their items.
    int count = 0;
    for (int i = 0; i < node->slot_count; i++) {
        const GreenNode *c = node->slots[i];
        if (!c) continue;
        count += c->IsList() ? c->slot_count : 1;
    }
    return count;
}

/** @brief Roslyn's SyntaxLastTokenReplacer: visits nodes counting down to the last token. */
GreenNode *ReplaceLastTokenVisit(GreenNode *node, GreenNode *newToken, int &count, bool &found) {
    if (node == nullptr || found) return node;
    count--;
    if (count != 0) return node;
    if (node->IsToken()) {
        found = true;
        return newToken;
    }
    count += CountChildNodesAndTokens(node);
    // base.Visit(node): visit every slot (lists visit their items), rebuild when changed.
    GreenNode *result = node;
    for (int i = 0; i < node->slot_count; i++) {
        GreenNode *child = node->slots[i];
        if (child == nullptr) continue;
        GreenNode *replaced;
        if (child->IsList()) {
            GreenNode *list = child;
            for (int j = 0; j < child->slot_count; j++) {
                GreenNode *item = child->slots[j];
                GreenNode *r = ReplaceLastTokenVisit(item, newToken, count, found);
                if (r != item) list = WithSlot(list, j, r);
            }
            replaced = list;
        } else {
            replaced = ReplaceLastTokenVisit(child, newToken, count, found);
        }
        if (replaced != child) result = WithSlot(result, i, replaced);
    }
    return result;
}

}  // namespace

GreenNode *SyntaxParser::AddLeadingSkippedSyntax(GreenNode *node, GreenNode *skippedSyntax) {
    if (skippedSyntax == nullptr) return node;
    GreenNode *oldToken = node->IsToken() ? node : node->GetFirstToken();
    GreenNode *newToken = AddSkippedSyntax(oldToken, skippedSyntax, false);
    bool found = false;
    return ReplaceFirstToken(node, newToken, skippedSyntax->full_width, found);
}

void SyntaxParser::AddTrailingSkippedSyntax(SyntaxListBuilder list, GreenNode *skippedSyntax) {
    list[list.Count() - 1] = AddTrailingSkippedSyntax(list[list.Count() - 1], skippedSyntax);
}

GreenNode *SyntaxParser::AddTrailingSkippedSyntax(GreenNode *node, GreenNode *skippedSyntax) {
    if (node->IsToken()) return AddSkippedSyntax(node, skippedSyntax, true);
    GreenNode *lastToken = node->GetLastToken();
    GreenNode *newToken = AddSkippedSyntax(lastToken, skippedSyntax, true);
    int count = 1;
    bool found = false;
    return ReplaceLastTokenVisit(node, newToken, count, found);
}

GreenNode *SyntaxParser::AddSkippedSyntax(GreenNode *target, GreenNode *skippedSyntax, bool trailing) {
    SyntaxListBuilder builder = SyntaxListBuilder::Create();
    int currentOffset;
    if (trailing) {
        currentOffset = target->Width() + target->GetTrailingTriviaWidth();
        builder.Add(target->GetTrailingTrivia());
    } else {
        currentOffset = -target->GetLeadingTriviaWidth() - skippedSyntax->full_width;
    }

    std::optional<SyntaxDiagnosticInfo> diagnostic;
    int finalDiagnosticOffset = 0;

    for (GreenNode *node : EnumerateNodes(skippedSyntax)) {
        if (node->IsToken()) {
            GreenNode *token = node;
            builder.Add(token->GetLeadingTrivia());
            if (token->Width() > 0) {
                GreenNode *bare = TokenWithTrailingTrivia(TokenWithLeadingTrivia(token, nullptr), nullptr);
                builder.Add(SyntaxFactory::SkippedTokensTrivia(bare));
            } else if (token->diag_count > 0) {
                // A zero-width token is not kept, but its first diagnostic moves to the target.
                const SyntaxDiagnosticInfo &existing = token->diags[0];
                diagnostic = existing;
                finalDiagnosticOffset = currentOffset + token->GetLeadingTriviaWidth() + existing.offset;
            }
            builder.Add(token->GetTrailingTrivia());
            currentOffset += token->full_width;
        } else if (node->ContainsDiagnostics() && !diagnostic) {
            if (node->diag_count > 0) {
                const SyntaxDiagnosticInfo &existing = node->diags[0];
                diagnostic = existing;
                finalDiagnosticOffset = currentOffset + node->GetLeadingTriviaWidth() + existing.offset;
            }
        }
    }

    if (diagnostic) {
        SyntaxDiagnosticInfo moved = *diagnostic;
        moved.offset = finalDiagnosticOffset;
        target = WithAdditionalDiagnostics(target, moved);
    }

    if (trailing) return TokenWithTrailingTrivia(target, builder.ToListNode());
    builder.AddRange(SyntaxList(target->GetLeadingTrivia()));
    return TokenWithLeadingTrivia(target, builder.ToListNode());
}

GreenNode *SyntaxParser::ConvertToKeyword(GreenNode *token) {
    if (token->kind != token->contextual_kind) {
        GreenNode *kw = token->IsMissing()
                            ? SyntaxFactory::MissingToken(token->leading, token->contextual_kind, token->trailing)
                            : SyntaxFactory::Token(token->leading, token->contextual_kind, token->trailing);
        if (token->diag_count > 0) kw = WithDiagnosticsGreen(kw, token->diags, token->diag_count);
        return kw;
    }
    return token;
}

GreenNode *SyntaxParser::ConvertToIdentifier(GreenNode *token) {
    CS_ASSERT(!token->IsMissing());
    GreenNode *identifier =
        SyntaxFactory::Identifier(token->kind, token->leading, token->text, token->value_text, token->trailing);
    if (token->ContainsDiagnostics()) identifier = WithDiagnosticsGreen(identifier, token->diags, token->diag_count);
    return identifier;
}

bool SyntaxParser::IsMakingProgress(int &lastTokenPosition, bool assertIfFalse) {
    (void)assertIfFalse;
    int pos = CurrentTokenPosition();
    if (pos > lastTokenPosition) {
        lastTokenPosition = pos;
        return true;
    }
    return false;
}

}  // namespace csharp
