// DocumentationCommentParser.cs and DocumentationCommentXmlTokens.cs (Roslyn, MIT, (c) .NET Foundation and
// Contributors): the XML doc-comment parser, with the cref and name-attribute grammars. Method for method, in
// the C# file's order.
//
// DocumentationMode is Parse (< Diagnose), so the WithAdditionalDiagnostics override drops every diagnostic
// this parser makes -- including the ones the base SyntaxParser makes through it (AddError, missing tokens).
// The error-creating paths are still ported as written (their side effects and the shapes they decide stay),
// but the diagnostics themselves are represented minimally:
//   - XmlSyntaxDiagnosticInfo(offset, width, XmlParseErrorCode, args) is a SyntaxDiagnosticInfo with code
//     WRN_XMLParseError whose first argument is the XmlParseErrorCode's name and whose remaining arguments are
//     the C# arguments (not the formatted XML-error message Roslyn would show);
//   - SyntaxDiagnosticInfo(offset, width, WRN_ErrorOverride, rawInfo, rawInfo.Code) gets the raw diagnostic's
//     formatted message and its code number as arguments (what CS1658's message prints).

#include "csharp_parser_internal.h"

namespace csharp {

namespace {

/** @brief GreenNode.WriteTo(writer, leading, trailing). */
void WriteTo(std::u16string &out, const GreenNode *node, bool leading, bool trailing) {
    if (node == nullptr) return;
    if (node->IsTrivia()) {
        out.append(node->text);
        return;
    }
    if (node->IsToken()) {
        if (leading && node->leading) WriteTo(out, node->leading, true, true);
        out.append(node->text);
        if (trailing && node->trailing) WriteTo(out, node->trailing, true, true);
        return;
    }
    int firstIndex = -1, lastIndex = -1;
    for (int i = 0; i < node->SlotCount(); i++) {
        if (node->GetSlot(i) != nullptr) {
            if (firstIndex < 0) firstIndex = i;
            lastIndex = i;
        }
    }
    for (int i = firstIndex; i >= 0 && i <= lastIndex; i++) {
        GreenNode *child = node->GetSlot(i);
        if (child != nullptr) {
            WriteTo(out, child, leading || i != firstIndex, trailing || i != lastIndex);
        }
    }
}

/** @brief node.ToString(): the text without the outer leading and trailing trivia. */
std::u16string ToString(const GreenNode *node) {
    std::u16string s;
    WriteTo(s, node, false, false);
    return s;
}

std::u16string ToFullString(const GreenNode *node) {
    std::u16string s;
    WriteTo(s, node, true, true);
    return s;
}

/** @brief LanguageParser.NoTriviaBetween. */
bool NoTriviaBetween(const GreenNode *token1, const GreenNode *token2) {
    return token1->GetTrailingTriviaWidth() == 0 && token2->GetLeadingTriviaWidth() == 0;
}

Text XmlParseErrorCodeName(DocumentationCommentParser::XmlParseErrorCode code) {
    using C = DocumentationCommentParser::XmlParseErrorCode;
    switch (code) {
        case C::XML_RefUndefinedEntity_1: return u"XML_RefUndefinedEntity_1";
        case C::XML_InvalidCharEntity: return u"XML_InvalidCharEntity";
        case C::XML_InvalidUnicodeChar: return u"XML_InvalidUnicodeChar";
        case C::XML_InvalidWhitespace: return u"XML_InvalidWhitespace";
        case C::XML_MissingEqualsAttribute: return u"XML_MissingEqualsAttribute";
        case C::XML_StringLiteralNoStartQuote: return u"XML_StringLiteralNoStartQuote";
        case C::XML_StringLiteralNoEndQuote: return u"XML_StringLiteralNoEndQuote";
        case C::XML_StringLiteralNonAsciiQuote: return u"XML_StringLiteralNonAsciiQuote";
        case C::XML_LessThanInAttributeValue: return u"XML_LessThanInAttributeValue";
        case C::XML_IncorrectComment: return u"XML_IncorrectComment";
        case C::XML_ElementTypeMatch: return u"XML_ElementTypeMatch";
        case C::XML_DuplicateAttribute: return u"XML_DuplicateAttribute";
        case C::XML_WhitespaceMissing: return u"XML_WhitespaceMissing";
        case C::XML_EndTagNotExpected: return u"XML_EndTagNotExpected";
        case C::XML_CDataEndTagNotAllowed: return u"XML_CDataEndTagNotAllowed";
        case C::XML_ExpectedIdentifier: return u"XML_ExpectedIdentifier";
        case C::XML_ExpectedEndOfTag: return u"XML_ExpectedEndOfTag";
        case C::XML_InvalidToken: return u"XML_InvalidToken";
        case C::XML_ExpectedEndOfXml: return u"XML_ExpectedEndOfXml";
        case C::XML_EndTagExpected: return u"XML_EndTagExpected";
    }
    return u"";
}

/** @brief new XmlSyntaxDiagnosticInfo(offset, width, code, args) -- minimal form, see the file comment. */
SyntaxDiagnosticInfo XmlSyntaxDiagnosticInfo(int offset, int width, DocumentationCommentParser::XmlParseErrorCode code,
                                             std::initializer_list<Text> args = {}) {
    SyntaxDiagnosticInfo d;
    d.offset = offset;
    d.width = width;
    d.code = ErrorCode::WRN_XMLParseError;
    d.args[d.arg_count++] = XmlParseErrorCodeName(code);
    for (Text a : args) {
        if (d.arg_count < 4) d.args[d.arg_count++] = CurrentArena()->CopyText(a);
    }
    return d;
}

/** @brief new SyntaxDiagnosticInfo(offset, width, ErrorCode.WRN_ErrorOverride, rawInfo, rawInfo.Code). */
SyntaxDiagnosticInfo ErrorOverride(int offset, int width, const SyntaxDiagnosticInfo &rawInfo, Text codeArg) {
    SyntaxDiagnosticInfo d;
    d.offset = offset;
    d.width = width;
    d.code = ErrorCode::WRN_ErrorOverride;
    d.arg_count = 2;
    d.args[0] = CurrentArena()->CopyText(ToUtf16(FormatDiagnosticMessage(rawInfo)));
    d.args[1] = codeArg;
    return d;
}

Text CodeNumber(ErrorCode code) { return CurrentArena()->CopyText(ToUtf16(std::to_string(static_cast<int>(code)))); }

// DocumentationCommentXmlNames
constexpr const char16_t *CrefAttributeName = u"cref";
constexpr const char16_t *NameAttributeName = u"name";
constexpr const char16_t *ParameterElementName = u"param";
constexpr const char16_t *ParameterReferenceElementName = u"paramref";
constexpr const char16_t *TypeParameterElementName = u"typeparam";
constexpr const char16_t *TypeParameterReferenceElementName = u"typeparamref";

/** @brief DocumentationCommentXmlNames.AttributeEquals / ElementEquals (C#: ordinal comparison). */
bool AttributeEquals(Text a, Text b) { return a == b; }
bool ElementEquals(Text a, Text b) { return a == b; }

}  // namespace

// --- DocumentationCommentXmlTokens.cs ---------------------------------------------------------------

namespace {

struct WellKnownXmlTokens {
    Arena arena;  // the tokens are shared by every parse (static in Roslyn), so they live here, forever
    GreenNode *see, *code, *list, *param, *value, *example, *include, *remarks, *seealso, *summary, *exception,
        *typeparam, *permission, *typeparamref;
    GreenNode *cref, *file, *name, *path, *type;

    WellKnownXmlTokens() {
        ArenaScope scope(&arena);
        // Well-known tags that typically have no leading trivia
        see = Identifier(u"see");
        code = Identifier(u"code");
        list = Identifier(u"list");
        param = Identifier(u"param");
        value = Identifier(u"value");
        example = Identifier(u"example");
        include = Identifier(u"include");
        remarks = Identifier(u"remarks");
        seealso = Identifier(u"seealso");
        summary = Identifier(u"summary");
        exception = Identifier(u"exception");
        typeparam = Identifier(u"typeparam");
        permission = Identifier(u"permission");
        typeparamref = Identifier(u"typeparamref");
        // Well-known tags that typically have a single space in leading trivia
        cref = IdentifierWithLeadingSpace(u"cref");
        file = IdentifierWithLeadingSpace(u"file");
        name = IdentifierWithLeadingSpace(u"name");
        path = IdentifierWithLeadingSpace(u"path");
        type = IdentifierWithLeadingSpace(u"type");
    }

    static GreenNode *Identifier(Text text) {
        return SyntaxFactory::Identifier(SyntaxKind::None, nullptr, text, text, /*trailing*/ nullptr);
    }
    static GreenNode *IdentifierWithLeadingSpace(Text text) {
        return SyntaxFactory::Identifier(SyntaxKind::None, SyntaxFactory::Space, text, text, /*trailing*/ nullptr);
    }
};

const WellKnownXmlTokens &XmlTokens() {
    static const WellKnownXmlTokens tokens;
    return tokens;
}

bool IsSingleSpaceTrivia(const SyntaxListBuilder &syntax) {
    // SyntaxFactory.Space.IsEquivalentTo(syntax[0])
    return syntax.Count() == 1 && syntax[0]->IsTrivia() && syntax[0]->kind == SyntaxKind::WhitespaceTrivia &&
           syntax[0]->text == u" ";
}

GreenNode *LookupXmlElementTag(Text text) {
    const WellKnownXmlTokens &t = XmlTokens();
    switch (text.size()) {
        case 3:
            if (text == u"see") return t.see;
            break;
        case 4:
            if (text == u"code") return t.code;
            if (text == u"list") return t.list;
            break;
        case 5:
            if (text == u"param") return t.param;
            if (text == u"value") return t.value;
            break;
        case 7:
            if (text == u"example") return t.example;
            if (text == u"include") return t.include;
            if (text == u"remarks") return t.remarks;
            if (text == u"seealso") return t.seealso;
            if (text == u"summary") return t.summary;
            break;
        case 9:
            if (text == u"exception") return t.exception;
            if (text == u"typeparam") return t.typeparam;
            break;
        case 10:
            if (text == u"permission") return t.permission;
            break;
        case 12:
            // (sic) Roslyn compares against TypeParameterElementName ("typeparam") here, so this never matches.
            if (text == TypeParameterElementName) return t.typeparamref;
            break;
        default:
            break;
    }

    return nullptr;
}

GreenNode *LookupXmlAttribute(Text text) {
    // It happens that all tokens have text of length 4
    if (text.size() != 4) {
        return nullptr;
    }

    const WellKnownXmlTokens &t = XmlTokens();
    if (text == u"cref") return t.cref;
    if (text == u"file") return t.file;
    if (text == u"name") return t.name;
    if (text == u"path") return t.path;
    if (text == u"type") return t.type;
    return nullptr;
}

}  // namespace

GreenNode *DocumentationCommentParser::DocumentationCommentXmlTokens::LookupToken(Text text,
                                                                                const SyntaxListBuilder &leading) {
    if (leading.IsNull()) {
        return LookupXmlElementTag(text);
    }

    if (IsSingleSpaceTrivia(leading)) {
        return LookupXmlAttribute(text);
    }

    return nullptr;
}

// --- DocumentationCommentParser.cs --------------------------------------------------------------------

DocumentationCommentParser::DocumentationCommentParser(Lexer *lexer_)
    : SyntaxParser(lexer_, LexerMode::None, /*allowModeReset*/ true) {}

void DocumentationCommentParser::ReInitialize(LexerMode modeflags) {
    SyntaxParser::ReInitialize();
    this->SetMode(LexerMode::XmlDocComment | LexerMode::XmlDocCommentLocationStart | modeflags);
    _isDelimited = (modeflags & LexerMode::XmlDocCommentStyleDelimited) != LexerMode::None;
}

LexerMode DocumentationCommentParser::SwapMode(LexerMode mode) {
    LexerMode tmp = this->Mode();
    this->SetMode(mode | (tmp & (LexerMode::MaskXmlDocCommentLocation | LexerMode::MaskXmlDocCommentStyle)));
    return tmp;
}

void DocumentationCommentParser::ResetMode(LexerMode mode) { this->SetMode(mode); }

GreenNode *DocumentationCommentParser::ParseDocumentationComment(bool &isTerminated) {
    SyntaxListBuilder nodes = _pool.Allocate();
    this->ParseXmlNodes(nodes);

    // It's possible that we finish parsing the xml, and we are still left in the middle
    // of an Xml comment. For example,
    //
    //     /// <goo></goo></uhoh>
    //                    ^
    // In this case, we stop at the caret. We need to ensure that we consume the remainder
    // of the doc comment here, since otherwise we will return the lexer to the state
    // where it recognizes C# tokens, which means that C# parser will get the </uhoh>,
    // which is not at all what we want.

    if (this->CurrentToken()->kind != SyntaxKind::EndOfDocumentationCommentToken) {
        this->ParseRemainder(nodes);
    }

    GreenNode *eoc = this->EatToken(SyntaxKind::EndOfDocumentationCommentToken);

    SyntaxList eocLeading(eoc->GetLeadingTrivia());
    isTerminated = !_isDelimited ||
                   (eocLeading.Count() > 0 && ToString(eocLeading[eocLeading.Count() - 1]) == u"*/");
    SyntaxKind kind =
        _isDelimited ? SyntaxKind::MultiLineDocumentationCommentTrivia : SyntaxKind::SingleLineDocumentationCommentTrivia;

    GreenNode *result = SyntaxFactory::DocumentationCommentTrivia(kind, nodes.ToList(), eoc);
    _pool.Free(nodes);
    return result;
}

void DocumentationCommentParser::ParseRemainder(SyntaxListBuilder nodes) {
    bool endTag = this->CurrentToken()->kind == SyntaxKind::LessThanSlashToken;

    LexerMode saveMode = this->SwapMode(LexerMode::XmlCDataSectionText);

    SyntaxListBuilder textTokens = _pool.Allocate();
    while (this->CurrentToken()->kind != SyntaxKind::EndOfDocumentationCommentToken) {
        GreenNode *token = this->EatToken();

        // TODO: It is possible that a non-literal gets in here. ]]>, specifically. Is that ok?
        textTokens.Add(token);
    }

    GreenNode *allRemainderText = SyntaxFactory::XmlText(textTokens.ToList());

    XmlParseErrorCode code = endTag ? XmlParseErrorCode::XML_EndTagNotExpected : XmlParseErrorCode::XML_ExpectedEndOfXml;
    allRemainderText = WithAdditionalDiagnostics(allRemainderText, XmlSyntaxDiagnosticInfo(0, 1, code));

    nodes.Add(allRemainderText);
    _pool.Free(textTokens);

    this->ResetMode(saveMode);
}

void DocumentationCommentParser::ParseXmlNodes(SyntaxListBuilder nodes) {
    while (true) {
        GreenNode *node = this->ParseXmlNode();
        if (node == nullptr) {
            return;
        }

        nodes.Add(node);
    }
}

GreenNode *DocumentationCommentParser::ParseXmlNode() {
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::XmlTextLiteralToken:
        case SyntaxKind::XmlTextLiteralNewLineToken:
        case SyntaxKind::XmlEntityLiteralToken:
            return this->ParseXmlText();
        case SyntaxKind::LessThanToken:
            return this->ParseXmlElement();
        case SyntaxKind::XmlCommentStartToken:
            return this->ParseXmlComment();
        case SyntaxKind::XmlCDataStartToken:
            return this->ParseXmlCDataSection();
        case SyntaxKind::XmlProcessingInstructionStartToken:
            return this->ParseXmlProcessingInstruction();
        case SyntaxKind::EndOfDocumentationCommentToken:
            return nullptr;
        default:
            // This means we have some unrecognized token. We probably need to give an error.
            return nullptr;
    }
}

bool DocumentationCommentParser::IsXmlNodeStartOrStop() {
    switch (this->CurrentToken()->kind) {
        case SyntaxKind::LessThanToken:
        case SyntaxKind::LessThanSlashToken:
        case SyntaxKind::XmlCommentStartToken:
        case SyntaxKind::XmlCDataStartToken:
        case SyntaxKind::XmlProcessingInstructionStartToken:
        case SyntaxKind::GreaterThanToken:
        case SyntaxKind::SlashGreaterThanToken:
        case SyntaxKind::EndOfDocumentationCommentToken:
            return true;
        default:
            return false;
    }
}

GreenNode *DocumentationCommentParser::ParseXmlText() {
    SyntaxListBuilder textTokens = _pool.Allocate();
    while (this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralToken ||
           this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralNewLineToken ||
           this->CurrentToken()->kind == SyntaxKind::XmlEntityLiteralToken) {
        textTokens.Add(this->EatToken());
    }

    GreenNode *list = textTokens.ToList();
    _pool.Free(textTokens);
    return SyntaxFactory::XmlText(list);
}

GreenNode *DocumentationCommentParser::ParseXmlElement() {
    GreenNode *lessThan = this->EatToken(SyntaxKind::LessThanToken);  // guaranteed
    LexerMode saveMode = this->SwapMode(LexerMode::XmlElementTag);
    GreenNode *name = this->ParseXmlName();
    if (lessThan->GetTrailingTriviaWidth() > 0 || name->GetLeadingTriviaWidth() > 0) {
        // The Xml spec disallows whitespace here: STag ::= '<' Name (S Attribute)* S? '>'
        name = this->WithXmlParseError(name, XmlParseErrorCode::XML_InvalidWhitespace);
    }

    SyntaxListBuilder attrs = _pool.Allocate();
    GreenNode *result;

    this->ParseXmlAttributes(name, attrs);

    if (this->CurrentToken()->kind == SyntaxKind::GreaterThanToken) {
        GreenNode *startTag = SyntaxFactory::XmlElementStartTag(lessThan, name, attrs, this->EatToken());
        this->SwapMode(LexerMode::XmlDocComment);
        SyntaxListBuilder nodes = _pool.Allocate();

        this->ParseXmlNodes(nodes);

        GreenNode *endName;
        GreenNode *greaterThan;

        // end tag
        GreenNode *lessThanSlash = this->EatToken(SyntaxKind::LessThanSlashToken, /*reportError*/ false);

        // If we didn't see "</", then we can't really be confident that this is actually an end tag,
        // so just insert a missing one.
        if (lessThanSlash->IsMissing()) {
            this->ResetMode(saveMode);
            lessThanSlash = this->WithXmlParseError(lessThanSlash, XmlParseErrorCode::XML_EndTagExpected,
                                                    {CurrentArena()->CopyText(ToString(name))});
            endName = SyntaxFactory::XmlName(/*prefix*/ nullptr, /*localName*/ SyntaxFactory::MissingToken(SyntaxKind::IdentifierToken));
            greaterThan = SyntaxFactory::MissingToken(SyntaxKind::GreaterThanToken);
        } else {
            this->SwapMode(LexerMode::XmlElementTag);
            endName = this->ParseXmlName();
            if (lessThanSlash->GetTrailingTriviaWidth() > 0 || endName->GetLeadingTriviaWidth() > 0) {
                // The Xml spec disallows whitespace here: STag ::= '<' Name (S Attribute)* S? '>'
                endName = this->WithXmlParseError(endName, XmlParseErrorCode::XML_InvalidWhitespace);
            }

            if (!endName->IsMissing() && !MatchingXmlNames(name, endName)) {
                endName = this->WithXmlParseError(endName, XmlParseErrorCode::XML_ElementTypeMatch,
                                                  {CurrentArena()->CopyText(ToString(endName)),
                                                   CurrentArena()->CopyText(ToString(name))});
            }

            // if we don't see the greater than token then skip the badness until we do or abort
            if (this->CurrentToken()->kind != SyntaxKind::GreaterThanToken) {
                this->SkipBadTokens(
                    endName, SyntaxListBuilder(),
                    [](DocumentationCommentParser *p) { return p->CurrentToken()->kind != SyntaxKind::GreaterThanToken; },
                    [](DocumentationCommentParser *p) { return p->IsXmlNodeStartOrStop(); },
                    XmlParseErrorCode::XML_InvalidToken);
            }

            greaterThan = this->EatToken(SyntaxKind::GreaterThanToken);
        }

        GreenNode *endTag = SyntaxFactory::XmlElementEndTag(lessThanSlash, endName, greaterThan);
        this->ResetMode(saveMode);
        result = SyntaxFactory::XmlElement(startTag, nodes.ToList(), endTag);
        _pool.Free(nodes);
    } else {
        GreenNode *slashGreater = this->EatToken(SyntaxKind::SlashGreaterThanToken, false);
        if (slashGreater->IsMissing() && !name->IsMissing()) {
            slashGreater = this->WithXmlParseError(slashGreater, XmlParseErrorCode::XML_ExpectedEndOfTag,
                                                   {CurrentArena()->CopyText(ToString(name))});
        }

        this->ResetMode(saveMode);
        result = SyntaxFactory::XmlEmptyElement(lessThan, name, attrs, slashGreater);
    }

    _pool.Free(attrs);
    return result;
}

bool DocumentationCommentParser::MatchingXmlNames(GreenNode *name, GreenNode *endName) {
    // PERF: because of deduplication we often get the same name for name and endName,
    //       so we will check for such case first before materializing text for entire nodes
    //       and comparing that.
    if (name == endName) {
        return true;
    }

    // (The C# then tries `!name.HasLeadingTrivia && !endName.HasTrailingTrivia && name.IsEquivalentTo(endName)`,
    // a shortcut whose `true` implies equal ToString()s; the ToString comparison alone decides the same.)
    return ToString(name) == ToString(endName);
}

void DocumentationCommentParser::ParseXmlAttributes(GreenNode *&elementName, SyntaxListBuilder attrs) {
    _attributesSeen.clear();
    while (true) {
        if (this->CurrentToken()->kind == SyntaxKind::IdentifierToken) {
            GreenNode *attr = this->ParseXmlAttribute(elementName);
            std::u16string attrName = ToString(GetField(attr, Field::Name));
            bool added = true;
            for (const std::u16string &seen : _attributesSeen) {
                if (seen == attrName) {
                    added = false;
                    break;
                }
            }
            if (added) _attributesSeen.push_back(attrName);
            if (!added) {
                attr = this->WithXmlParseError(attr, XmlParseErrorCode::XML_DuplicateAttribute,
                                               {CurrentArena()->CopyText(attrName)});
            }

            attrs.Add(attr);
        } else {
            SkipResult skip = this->SkipBadTokens(
                elementName, attrs,

                // not expected condition
                // (sic: IdentifierName is a node kind, never a token's, so this is always true)
                [](DocumentationCommentParser *p) { return p->CurrentToken()->kind != SyntaxKind::IdentifierName; },

                // abort condition (looks like something we might understand later)
                [](DocumentationCommentParser *p) {
                    return p->CurrentToken()->kind == SyntaxKind::GreaterThanToken ||
                           p->CurrentToken()->kind == SyntaxKind::SlashGreaterThanToken ||
                           p->CurrentToken()->kind == SyntaxKind::LessThanToken ||
                           p->CurrentToken()->kind == SyntaxKind::LessThanSlashToken ||
                           p->CurrentToken()->kind == SyntaxKind::EndOfDocumentationCommentToken ||
                           p->CurrentToken()->kind == SyntaxKind::EndOfFileToken;
                },

                XmlParseErrorCode::XML_InvalidToken);

            if (skip == SkipResult::Abort) {
                break;
            }
        }
    }
}

DocumentationCommentParser::SkipResult DocumentationCommentParser::SkipBadTokens(
    GreenNode *&startNode, SyntaxListBuilder list, ParserPredicate isNotExpectedFunction, ParserPredicate abortFunction,
    XmlParseErrorCode error) {
    SyntaxListBuilder badTokens;  // default(SyntaxListBuilder<SyntaxToken>)
    bool hasError = false;

    SkipResult result = SkipResult::Continue;
    SkipResult ret;

    while (isNotExpectedFunction(this)) {
        if (abortFunction(this)) {
            result = SkipResult::Abort;
            break;
        }

        if (badTokens.IsNull()) {
            badTokens = _pool.Allocate();
        }

        GreenNode *token = this->EatToken();
        if (!hasError) {
            token = this->WithXmlParseError(token, error, {token->text});
            hasError = true;
        }

        badTokens.Add(token);
    }

    if (!badTokens.IsNull() && badTokens.Count() > 0) {
        // use skipped text since cannot nest structured trivia under structured trivia
        if (list.IsNull() || list.Count() == 0) {
            startNode = AddTrailingSkippedSyntax(startNode, badTokens.ToListNode());
        } else {
            list[list.Count() - 1] = AddTrailingSkippedSyntax(list[list.Count() - 1], badTokens.ToListNode());
        }

        ret = result;
    } else {
        // somehow we did not consume anything, so tell caller to abort parse rule
        ret = SkipResult::Abort;
    }

    if (!badTokens.IsNull()) {
        _pool.Free(badTokens);
    }

    return ret;
}

GreenNode *DocumentationCommentParser::ParseXmlAttribute(GreenNode *elementName) {
    GreenNode *attrName = this->ParseXmlName();
    if (attrName->GetLeadingTriviaWidth() == 0) {
        // The Xml spec requires whitespace here: STag ::= '<' Name (S Attribute)* S? '>'
        attrName = this->WithXmlParseError(attrName, XmlParseErrorCode::XML_WhitespaceMissing);
    }

    GreenNode *equals = this->EatToken(SyntaxKind::EqualsToken, false);
    if (equals->IsMissing()) {
        equals = this->WithXmlParseError(equals, XmlParseErrorCode::XML_MissingEqualsAttribute);

        switch (this->CurrentToken()->kind) {
            case SyntaxKind::SingleQuoteToken:
            case SyntaxKind::DoubleQuoteToken:
                // There could be a value coming up, let's keep parsing.
                break;
            default:
                // This is probably not a complete attribute.
                return SyntaxFactory::XmlTextAttribute(attrName, equals,
                                                       SyntaxFactory::MissingToken(SyntaxKind::DoubleQuoteToken),
                                                       /*textTokens*/ nullptr,
                                                       SyntaxFactory::MissingToken(SyntaxKind::DoubleQuoteToken));
        }
    }

    GreenNode *startQuote;
    GreenNode *endQuote;
    Text attrNameText = GetField(attrName, Field::LocalName)->value_text;
    bool hasNoPrefix = GetField(attrName, Field::Prefix) == nullptr;
    if (hasNoPrefix && AttributeEquals(attrNameText, CrefAttributeName) && !IsVerbatimCref()) {
        GreenNode *cref;
        this->ParseCrefAttribute(startQuote, cref, endQuote);
        return SyntaxFactory::XmlCrefAttribute(attrName, equals, startQuote, cref, endQuote);
    } else if (hasNoPrefix && AttributeEquals(attrNameText, NameAttributeName) &&
               XmlElementSupportsNameAttribute(elementName)) {
        GreenNode *identifier;
        this->ParseNameAttribute(startQuote, identifier, endQuote);
        return SyntaxFactory::XmlNameAttribute(attrName, equals, startQuote, identifier, endQuote);
    } else {
        SyntaxListBuilder textTokens = _pool.Allocate();
        this->ParseXmlAttributeText(startQuote, textTokens, endQuote);
        GreenNode *result = SyntaxFactory::XmlTextAttribute(attrName, equals, startQuote, textTokens, endQuote);
        _pool.Free(textTokens);
        return result;
    }
}

bool DocumentationCommentParser::XmlElementSupportsNameAttribute(GreenNode *elementName) {
    if (GetField(elementName, Field::Prefix) != nullptr) {
        return false;
    }

    Text localName = GetField(elementName, Field::LocalName)->value_text;
    return ElementEquals(localName, ParameterElementName) || ElementEquals(localName, ParameterReferenceElementName) ||
           ElementEquals(localName, TypeParameterElementName) ||
           ElementEquals(localName, TypeParameterReferenceElementName);
}

bool DocumentationCommentParser::IsVerbatimCref() {
    // As in XMLDocWriter::ReplaceReferences, if the first character of the value is not colon and the second character
    // is, then don't process the cref - just emit it as-is.
    bool isVerbatim = false;

    ResetPoint resetPoint = this->GetResetPoint();

    GreenNode *openQuote = EatToken(this->CurrentToken()->kind == SyntaxKind::SingleQuoteToken
                                        ? SyntaxKind::SingleQuoteToken
                                        : SyntaxKind::DoubleQuoteToken);

    // NOTE: Don't need to save mode, since we're already using a reset point.
    this->SwapMode(LexerMode::XmlCharacter);

    GreenNode *current = this->CurrentToken();
    if ((current->kind == SyntaxKind::XmlTextLiteralToken || current->kind == SyntaxKind::XmlEntityLiteralToken) &&
        current->value_text != SyntaxFacts::GetText(openQuote->kind) && current->value_text != u":") {
        EatToken();

        current = this->CurrentToken();
        if ((current->kind == SyntaxKind::XmlTextLiteralToken || current->kind == SyntaxKind::XmlEntityLiteralToken) &&
            current->value_text == u":") {
            isVerbatim = true;
        }
    }

    this->Reset(resetPoint);
    this->Release(resetPoint);

    return isVerbatim;
}

void DocumentationCommentParser::ParseCrefAttribute(GreenNode *&startQuote, GreenNode *&cref, GreenNode *&endQuote) {
    startQuote = ParseXmlAttributeStartQuote();
    SyntaxKind quoteKind = startQuote->kind;

    {
        LexerMode saveMode =
            this->SwapMode(quoteKind == SyntaxKind::SingleQuoteToken ? LexerMode::XmlCrefQuote : LexerMode::XmlCrefDoubleQuote);

        cref = this->ParseCrefAttributeValue();

        this->ResetMode(saveMode);
    }

    endQuote = ParseXmlAttributeEndQuote(quoteKind);
}

void DocumentationCommentParser::ParseNameAttribute(GreenNode *&startQuote, GreenNode *&identifier,
                                                    GreenNode *&endQuote) {
    startQuote = ParseXmlAttributeStartQuote();
    SyntaxKind quoteKind = startQuote->kind;

    {
        LexerMode saveMode =
            this->SwapMode(quoteKind == SyntaxKind::SingleQuoteToken ? LexerMode::XmlNameQuote : LexerMode::XmlNameDoubleQuote);

        identifier = this->ParseNameAttributeValue();

        this->ResetMode(saveMode);
    }

    endQuote = ParseXmlAttributeEndQuote(quoteKind);
}

void DocumentationCommentParser::ParseXmlAttributeText(GreenNode *&startQuote, SyntaxListBuilder textTokens,
                                                       GreenNode *&endQuote) {
    startQuote = ParseXmlAttributeStartQuote();
    SyntaxKind quoteKind = startQuote->kind;

    // NOTE: Being a bit sneaky here - if the width isn't 0, we consumed something else in
    // place of the quote and we should continue parsing the attribute.
    if (startQuote->IsMissing() && startQuote->full_width == 0) {
        endQuote = SyntaxFactory::MissingToken(quoteKind);
    } else {
        LexerMode saveMode = this->SwapMode(quoteKind == SyntaxKind::SingleQuoteToken ? LexerMode::XmlAttributeTextQuote
                                                                                     : LexerMode::XmlAttributeTextDoubleQuote);

        while (this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralToken ||
               this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralNewLineToken ||
               this->CurrentToken()->kind == SyntaxKind::XmlEntityLiteralToken ||
               this->CurrentToken()->kind == SyntaxKind::LessThanToken) {
            GreenNode *token = this->EatToken();
            if (token->kind == SyntaxKind::LessThanToken) {
                // TODO: It is possible that a non-literal gets in here. <, specifically. Is that ok?
                token = this->WithXmlParseError(token, XmlParseErrorCode::XML_LessThanInAttributeValue);
            }

            textTokens.Add(token);
        }

        this->ResetMode(saveMode);

        // NOTE: This will never consume a non-ascii quote, since non-ascii quotes
        // are legal in the attribute value and are consumed by the preceding loop.
        endQuote = ParseXmlAttributeEndQuote(quoteKind);
    }
}

GreenNode *DocumentationCommentParser::ParseXmlAttributeStartQuote() {
    if (IsNonAsciiQuotationMark(this->CurrentToken())) {
        return SkipNonAsciiQuotationMark();
    }

    SyntaxKind quoteKind =
        this->CurrentToken()->kind == SyntaxKind::SingleQuoteToken ? SyntaxKind::SingleQuoteToken : SyntaxKind::DoubleQuoteToken;

    GreenNode *startQuote = this->EatToken(quoteKind, /*reportError*/ false);
    if (startQuote->IsMissing()) {
        startQuote = this->WithXmlParseError(startQuote, XmlParseErrorCode::XML_StringLiteralNoStartQuote);
    }
    return startQuote;
}

GreenNode *DocumentationCommentParser::ParseXmlAttributeEndQuote(SyntaxKind quoteKind) {
    if (IsNonAsciiQuotationMark(this->CurrentToken())) {
        return SkipNonAsciiQuotationMark();
    }

    GreenNode *endQuote = this->EatToken(quoteKind, /*reportError*/ false);
    if (endQuote->IsMissing()) {
        endQuote = this->WithXmlParseError(endQuote, XmlParseErrorCode::XML_StringLiteralNoEndQuote);
    }
    return endQuote;
}

GreenNode *DocumentationCommentParser::SkipNonAsciiQuotationMark() {
    GreenNode *quote = SyntaxFactory::MissingToken(SyntaxKind::DoubleQuoteToken);
    quote = AddTrailingSkippedSyntax(quote, EatToken());
    quote = this->WithXmlParseError(quote, XmlParseErrorCode::XML_StringLiteralNonAsciiQuote);
    return quote;
}

/// These aren't acceptable in place of ASCII quotation marks in XML,
/// but we want to consume them (and produce an appropriate error) if
/// they occur in a place where a quotation mark is legal.
bool DocumentationCommentParser::IsNonAsciiQuotationMark(GreenNode *token) {
    return token->text.size() == 1 && SyntaxFacts::IsNonAsciiQuotationMark(token->text[0]);
}

GreenNode *DocumentationCommentParser::ParseXmlName() {
    GreenNode *id = this->EatToken(SyntaxKind::IdentifierToken);
    GreenNode *prefix = nullptr;
    if (this->CurrentToken()->kind == SyntaxKind::ColonToken) {
        GreenNode *colon = this->EatToken();

        int prefixTrailingWidth = id->GetTrailingTriviaWidth();
        int colonLeadingWidth = colon->GetLeadingTriviaWidth();

        if (prefixTrailingWidth > 0 || colonLeadingWidth > 0) {
            // NOTE: offset is relative to Start (not FullStart) of colon.
            int width = prefixTrailingWidth + colonLeadingWidth;
            int offset = -width;
            colon = WithAdditionalDiagnostics(colon,
                                              XmlSyntaxDiagnosticInfo(offset, width, XmlParseErrorCode::XML_InvalidWhitespace));
        }

        prefix = SyntaxFactory::XmlPrefix(id, colon);
        id = this->EatToken(SyntaxKind::IdentifierToken);

        int colonTrailingWidth = colon->GetTrailingTriviaWidth();
        int localNameLeadingWidth = id->GetLeadingTriviaWidth();
        if (colonTrailingWidth > 0 || localNameLeadingWidth > 0) {
            // NOTE: offset is relative to Start (not FullStart) of identifier.
            int width = colonTrailingWidth + localNameLeadingWidth;
            int offset = -width;
            id = WithAdditionalDiagnostics(id, XmlSyntaxDiagnosticInfo(offset, width, XmlParseErrorCode::XML_InvalidWhitespace));

            // CONSIDER: Another interpretation would be that the local part of this name is a missing identifier and the identifier
            // we've just consumed is actually part of something else (e.g. an attribute name).
        }
    }

    return SyntaxFactory::XmlName(prefix, id);
}

GreenNode *DocumentationCommentParser::ParseXmlComment() {
    GreenNode *lessThanExclamationMinusMinusToken = this->EatToken(SyntaxKind::XmlCommentStartToken);
    LexerMode saveMode = this->SwapMode(LexerMode::XmlCommentText);
    SyntaxListBuilder textTokens = _pool.Allocate();
    while (this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralToken ||
           this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralNewLineToken ||
           this->CurrentToken()->kind == SyntaxKind::MinusMinusToken) {
        GreenNode *token = this->EatToken();
        if (token->kind == SyntaxKind::MinusMinusToken) {
            // TODO: It is possible that a non-literal gets in here. --, specifically. Is that ok?
            token = this->WithXmlParseError(token, XmlParseErrorCode::XML_IncorrectComment);
        }

        textTokens.Add(token);
    }

    GreenNode *list = textTokens.ToList();
    _pool.Free(textTokens);

    GreenNode *minusMinusGreaterThanToken = this->EatToken(SyntaxKind::XmlCommentEndToken);
    this->ResetMode(saveMode);
    return SyntaxFactory::XmlComment(lessThanExclamationMinusMinusToken, list, minusMinusGreaterThanToken);
}

GreenNode *DocumentationCommentParser::ParseXmlCDataSection() {
    GreenNode *startCDataToken = this->EatToken(SyntaxKind::XmlCDataStartToken);
    LexerMode saveMode = this->SwapMode(LexerMode::XmlCDataSectionText);
    SyntaxListBuilder textTokens = SyntaxListBuilder::Create();
    while (this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralToken ||
           this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralNewLineToken) {
        textTokens.Add(this->EatToken());
    }

    GreenNode *endCDataToken = this->EatToken(SyntaxKind::XmlCDataEndToken);
    this->ResetMode(saveMode);
    return SyntaxFactory::XmlCDataSection(startCDataToken, textTokens, endCDataToken);
}

GreenNode *DocumentationCommentParser::ParseXmlProcessingInstruction() {
    GreenNode *startProcessingInstructionToken = this->EatToken(SyntaxKind::XmlProcessingInstructionStartToken);
    LexerMode saveMode = this->SwapMode(LexerMode::XmlElementTag);  // this mode accepts names
    GreenNode *name = this->ParseXmlName();

    // NOTE: The XML spec says that name cannot be "xml" (case-insensitive comparison),
    // but Dev10 does not enforce this.

    this->SwapMode(LexerMode::XmlProcessingInstructionText);  // this mode consumes text
    SyntaxListBuilder textTokens = SyntaxListBuilder::Create();
    while (this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralToken ||
           this->CurrentToken()->kind == SyntaxKind::XmlTextLiteralNewLineToken) {
        GreenNode *textToken = this->EatToken();

        // NOTE: The XML spec says that the each text token must begin with a whitespace
        // character, but Dev10 does not enforce this.

        textTokens.Add(textToken);
    }

    GreenNode *endProcessingInstructionToken = this->EatToken(SyntaxKind::XmlProcessingInstructionEndToken);
    this->ResetMode(saveMode);
    return SyntaxFactory::XmlProcessingInstruction(startProcessingInstructionToken, name, textTokens,
                                                   endProcessingInstructionToken);
}

SyntaxDiagnosticInfo DocumentationCommentParser::GetExpectedTokenError(SyntaxKind expected, SyntaxKind actual,
                                                                       int offset, int width) {
    // NOTE: There are no errors in crefs - only warnings.  We accomplish this by wrapping every diagnostic in ErrorCode.WRN_ErrorOverride.
    if (InCref()) {
        SyntaxDiagnosticInfo rawInfo = SyntaxParser::GetExpectedTokenError(expected, actual, offset, width);
        SyntaxDiagnosticInfo crefInfo = ErrorOverride(rawInfo.offset, rawInfo.width, rawInfo, CodeNumber(rawInfo.code));
        return crefInfo;
    }

    switch (expected) {
        case SyntaxKind::IdentifierToken:
            return XmlSyntaxDiagnosticInfo(offset, width, XmlParseErrorCode::XML_ExpectedIdentifier);

        default:
            return XmlSyntaxDiagnosticInfo(offset, width, XmlParseErrorCode::XML_InvalidToken,
                                           {SyntaxFacts::GetText(actual)});
    }
}

SyntaxDiagnosticInfo DocumentationCommentParser::GetExpectedMissingNodeOrTokenError(GreenNode *missingNodeOrToken,
                                                                                    SyntaxKind expected,
                                                                                    SyntaxKind actual) {
    CS_ASSERT(missingNodeOrToken->IsMissing());

    // NOTE: There are no errors in crefs - only warnings.  We accomplish this by wrapping every diagnostic in ErrorCode.WRN_ErrorOverride.
    if (InCref()) {
        return SyntaxParser::GetExpectedMissingNodeOrTokenError(missingNodeOrToken, expected, actual);
    }

    switch (expected) {
        case SyntaxKind::IdentifierToken:
            return XmlSyntaxDiagnosticInfo(0, 0, XmlParseErrorCode::XML_ExpectedIdentifier);

        default:
            return XmlSyntaxDiagnosticInfo(0, 0, XmlParseErrorCode::XML_InvalidToken, {SyntaxFacts::GetText(actual)});
    }
}

// WithXmlParseError<TNode>(node, code[, args]) (all three C# overloads).
GreenNode *DocumentationCommentParser::WithXmlParseError(GreenNode *node, XmlParseErrorCode code,
                                                         std::initializer_list<Text> args) {
    return WithAdditionalDiagnostics(node, XmlSyntaxDiagnosticInfo(0, node->Width(), code, args));
}

GreenNode *DocumentationCommentParser::WithAdditionalDiagnostics(GreenNode *node, const SyntaxDiagnosticInfo &diagnostic) {
    // Don't attach any diagnostics to syntax nodes within a documentation comment if the DocumentationMode
    // is not at least Diagnose. (It is Parse here, so: never.)
    (void)diagnostic;
    return node;
}

// --- Cref ---------------------------------------------------------------------------------------------

/// ACASEY: This grammar is derived from the behavior and sources of the native compiler.
/// Tokens start with underscores (I've cheated for _PredefinedTypeToken, which is not actually a
/// SyntaxKind), "*" indicates "0 or more", "?" indicates "0 or 1", and parentheses are for grouping.
///
/// Cref                = CrefType _DotToken CrefMember
///                     | CrefType
///                     | CrefMember
///                     | CrefFirstType _OpenParenToken CrefParameterList? _CloseParenToken
/// CrefName            = _IdentifierToken (_LessThanToken _IdentifierToken (_CommaToken _IdentifierToken)* _GreaterThanToken)?
/// CrefFirstType       = ((_IdentifierToken _ColonColonToken)? CrefName)
///                     | _PredefinedTypeToken
/// CrefType            = CrefFirstType (_DotToken CrefName)*
/// CrefMember          = CrefName (_OpenParenToken CrefParameterList? _CloseParenToken)?
///                     | _ThisKeyword (_OpenBracketToken CrefParameterList _CloseBracketToken)?
///                     | _OperatorKeyword _OperatorToken (_OpenParenToken CrefParameterList? _CloseParenToken)?
///                     | (_ImplicitKeyword | _ExplicitKeyword) _OperatorKeyword CrefParameterType (_OpenParenToken CrefParameterList? _CloseParenToken)?
/// CrefParameterList   = CrefParameter (_CommaToken CrefParameter)*
/// CrefParameter       = (_RefKeyword | _OutKeyword)? CrefParameterType
/// CrefParameterType   = CrefParameterType2 _QuestionToken? _AsteriskToken* (_OpenBracketToken _CommaToken* _CloseBracketToken)*
/// CrefParameterType2  = (((_IdentifierToken _ColonColonToken)? CrefParameterType3) | _PredefinedTypeToken) (_DotToken CrefParameterType3)*
/// CrefParameterType3  = _IdentifierToken (_LessThanToken CrefParameterType (_CommaToken CrefParameterType)* _GreaterThanToken)?
GreenNode *DocumentationCommentParser::ParseCrefAttributeValue() {
    GreenNode *result;

    GreenNode *type = ParseCrefType(/*typeArgumentsMustBeIdentifiers*/ true, /*checkForMember*/ true);
    if (type == nullptr) {
        result = ParseMemberCref();
    } else if (IsEndOfCrefAttribute()) {
        result = SyntaxFactory::TypeCref(type);
    } else if (type->kind != SyntaxKind::QualifiedName && this->CurrentToken()->kind == SyntaxKind::OpenParenToken) {
        // Special case for crefs like "string()" and "A::B()".
        GreenNode *parameters = ParseCrefParameterList();
        result = SyntaxFactory::NameMemberCref(type, parameters);
    } else {
        GreenNode *dot = EatToken(SyntaxKind::DotToken);
        GreenNode *member = ParseMemberCref();
        result = SyntaxFactory::QualifiedCref(type, dot, member);
    }

    bool needOverallError = !IsEndOfCrefAttribute() || result->ContainsDiagnostics();

    if (!IsEndOfCrefAttribute()) {
        SyntaxListBuilder badTokens = _pool.Allocate();
        while (!IsEndOfCrefAttribute()) {
            badTokens.Add(this->EatToken());
        }
        result = AddTrailingSkippedSyntax(result, badTokens.ToListNode());
        _pool.Free(badTokens);
    }

    if (needOverallError) {
        result = this->AddError(result, ErrorCode::WRN_BadXMLRefSyntax, {ArgText(ToFullString(result))});
    }

    return result;
}

/// Parse the custom cref syntax for a named member (method, property, etc),
/// an indexer, an overloadable operator, or a user-defined conversion.
GreenNode *DocumentationCommentParser::ParseMemberCref() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::ThisKeyword:
            return ParseIndexerMemberCref();
        case SyntaxKind::OperatorKeyword:
            return ParseOperatorMemberCref();
        case SyntaxKind::ExplicitKeyword:
        case SyntaxKind::ImplicitKeyword:
            return ParseConversionOperatorMemberCref();
        case SyntaxKind::IdentifierToken:
            if (CurrentToken()->contextual_kind == SyntaxKind::ExtensionKeyword) {
                return ParsePossibleExtensionMemberCref();
            }
            return ParseNameMemberCref();
        default:
            return ParseNameMemberCref();
    }
}

/// Parse a named member (method, property, etc), with optional type
/// parameters and regular parameters.
GreenNode *DocumentationCommentParser::ParseNameMemberCref() {
    GreenNode *name = ParseCrefName(/*typeArgumentsMustBeIdentifiers*/ true);
    GreenNode *parameters = ParseCrefParameterList();

    return SyntaxFactory::NameMemberCref(name, parameters);
}

/// Parse an indexer member, with optional parameters.
GreenNode *DocumentationCommentParser::ParseIndexerMemberCref() {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ThisKeyword);
    GreenNode *thisKeyword = EatToken();
    GreenNode *parameters = ParseBracketedCrefParameterList();

    return SyntaxFactory::IndexerMemberCref(thisKeyword, parameters);
}

/// If we have `extension` (with optional type arguments) and parameter list and a dot, then we have an extension member cref.
/// Otherwise, we fall back to producing the same result as ParseNameMemberCref
GreenNode *DocumentationCommentParser::ParsePossibleExtensionMemberCref() {
    CS_ASSERT(CurrentToken()->contextual_kind == SyntaxKind::ExtensionKeyword);

    GreenNode *identifierToken = EatToken();
    GreenNode *typeArguments = (CurrentToken()->kind == SyntaxKind::LessThanToken)
                                   ? ParseTypeArguments(/*typeArgumentsMustBeIdentifiers*/ true)
                                   : nullptr;
    GreenNode *parameters = (CurrentToken()->kind == SyntaxKind::OpenParenToken) ? ParseCrefParameterList() : nullptr;

    if (parameters == nullptr || CurrentToken()->kind != SyntaxKind::DotToken) {
        GreenNode *name = typeArguments != nullptr ? SyntaxFactory::GenericName(identifierToken, typeArguments)
                                                   : SyntaxFactory::IdentifierName(identifierToken);

        return SyntaxFactory::NameMemberCref(name, parameters);
    }

    GreenNode *dotToken = EatToken(SyntaxKind::DotToken);
    GreenNode *member = ParseMemberCref();
    if (member->kind == SyntaxKind::ExtensionMemberCref) {
        member = AddErrorAsWarning(member, ErrorCode::ERR_MisplacedExtension);
    }

    return SyntaxFactory::ExtensionMemberCref(ConvertToKeyword(identifierToken), typeArguments, parameters, dotToken, member);
}

/// Parse an overloadable operator, with optional parameters.
GreenNode *DocumentationCommentParser::ParseOperatorMemberCref() {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::OperatorKeyword);
    GreenNode *operatorKeyword = EatToken();
    GreenNode *checkedKeyword = TryEatCheckedKeyword(/*isConversion*/ false, operatorKeyword);

    GreenNode *operatorToken;

    if (SyntaxFacts::IsAnyOverloadableOperator(CurrentToken()->kind)) {
        operatorToken = EatToken();
    } else {
        operatorToken = SyntaxFactory::MissingToken(SyntaxKind::PlusToken);

        // Grab the offset and width before we consume the invalid keyword and change our position.
        auto [offset, width] = this->GetDiagnosticSpanForMissingNodeOrToken(/*missingNodeOrToken*/ operatorToken);

        if (SyntaxFacts::IsUnaryOperatorDeclarationToken(CurrentToken()->kind) ||
            SyntaxFacts::IsBinaryExpressionOperatorToken(CurrentToken()->kind)) {
            operatorToken = AddTrailingSkippedSyntax(operatorToken, EatToken());
        }

        SyntaxDiagnosticInfo rawInfo = MakeError(offset, width, ErrorCode::ERR_OvlOperatorExpected);
        SyntaxDiagnosticInfo crefInfo = ErrorOverride(offset, width, rawInfo, CodeNumber(rawInfo.code));

        operatorToken = WithAdditionalDiagnostics(operatorToken, crefInfo);
    }

    // Token(operatorToken.GetLeadingTrivia(), kind, t1.Text + t2.Text [+ t3.Text], (ValueTexts), last.GetTrailingTrivia())
    auto combine = [](SyntaxKind kind, std::initializer_list<GreenNode *> tokens) {
        std::u16string text, valueText;
        for (GreenNode *t : tokens) {
            text.append(t->text);
            valueText.append(t->value_text);
        }
        GreenNode *first = *tokens.begin();
        GreenNode *last = *(tokens.end() - 1);
        return SyntaxFactory::Token(first->GetLeadingTrivia(), kind, CurrentArena()->CopyText(text),
                                    CurrentArena()->CopyText(valueText), last->GetTrailingTrivia());
    };

    // Have to fake >>/>>> because it looks like the closing of nested type parameter lists (e.g. A<A<T>>).
    // Have to fake >= so the lexer doesn't mishandle >>=.
    if (operatorToken->kind == SyntaxKind::GreaterThanToken && NoTriviaBetween(operatorToken, CurrentToken())) {
        if (CurrentToken()->kind == SyntaxKind::GreaterThanToken) {
            GreenNode *operatorToken2 = this->EatToken();

            if (NoTriviaBetween(operatorToken2, CurrentToken()) &&
                (CurrentToken()->kind == SyntaxKind::GreaterThanToken ||
                 CurrentToken()->kind == SyntaxKind::GreaterThanEqualsToken)) {
                GreenNode *operatorToken3 = this->EatToken();

                if (operatorToken3->kind == SyntaxKind::GreaterThanToken) {
                    operatorToken = combine(SyntaxKind::GreaterThanGreaterThanGreaterThanToken,
                                            {operatorToken, operatorToken2, operatorToken3});

                    operatorToken = CheckFeatureAvailability(operatorToken);  // IDS_FeatureUnsignedRightShift
                } else {
                    operatorToken = combine(SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken,
                                            {operatorToken, operatorToken2, operatorToken3});
                    operatorToken = CheckFeatureAvailability(operatorToken);  // IDS_FeatureUserDefinedCompoundAssignmentOperators
                }
            } else {
                operatorToken = combine(SyntaxKind::GreaterThanGreaterThanToken, {operatorToken, operatorToken2});
            }
        } else if (CurrentToken()->kind == SyntaxKind::EqualsToken) {
            GreenNode *operatorToken2 = this->EatToken();
            operatorToken = combine(SyntaxKind::GreaterThanEqualsToken, {operatorToken, operatorToken2});
        } else if (CurrentToken()->kind == SyntaxKind::GreaterThanEqualsToken) {
            GreenNode *operatorToken2 = this->EatToken();
            operatorToken = combine(SyntaxKind::GreaterThanGreaterThanEqualsToken, {operatorToken, operatorToken2});
            operatorToken = CheckFeatureAvailability(operatorToken);  // IDS_FeatureUserDefinedCompoundAssignmentOperators
        }
    }

    // SyntaxToken tryParseCompoundAssignmentOperatorToken(SyntaxToken operatorToken, SyntaxKind kind)
    auto tryParseCompoundAssignmentOperatorToken = [&](GreenNode *opToken, SyntaxKind kind) {
        if (NoTriviaBetween(opToken, CurrentToken()) && CurrentToken()->kind == SyntaxKind::EqualsToken) {
            GreenNode *operatorToken2 = this->EatToken();
            opToken = combine(kind, {opToken, operatorToken2});
            opToken = CheckFeatureAvailability(opToken);  // IDS_FeatureUserDefinedCompoundAssignmentOperators
        }

        return opToken;
    };

    switch (operatorToken->kind) {
        case SyntaxKind::PlusToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::PlusEqualsToken);
            break;
        case SyntaxKind::MinusToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::MinusEqualsToken);
            break;
        case SyntaxKind::AsteriskToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::AsteriskEqualsToken);
            break;
        case SyntaxKind::SlashToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::SlashEqualsToken);
            break;
        case SyntaxKind::PercentToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::PercentEqualsToken);
            break;
        case SyntaxKind::AmpersandToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::AmpersandEqualsToken);
            break;
        case SyntaxKind::BarToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::BarEqualsToken);
            break;
        case SyntaxKind::CaretToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::CaretEqualsToken);
            break;
        case SyntaxKind::LessThanLessThanToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::LessThanLessThanEqualsToken);
            break;
        case SyntaxKind::GreaterThanGreaterThanToken:
            operatorToken =
                tryParseCompoundAssignmentOperatorToken(operatorToken, SyntaxKind::GreaterThanGreaterThanEqualsToken);
            break;
        case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
            operatorToken = tryParseCompoundAssignmentOperatorToken(operatorToken,
                                                                    SyntaxKind::GreaterThanGreaterThanGreaterThanEqualsToken);
            break;
        default:
            break;
    }

    CS_ASSERT(SyntaxFacts::IsAnyOverloadableOperator(operatorToken->kind));

    GreenNode *parameters = ParseCrefParameterList();

    return SyntaxFactory::OperatorMemberCref(operatorKeyword, checkedKeyword, operatorToken, parameters);
}

GreenNode *DocumentationCommentParser::TryEatCheckedKeyword(bool isConversion, GreenNode *&operatorKeyword) {
    // SyntaxToken tryEatCheckedOrHandleUnchecked(ref SyntaxToken operatorKeyword)
    auto tryEatCheckedOrHandleUnchecked = [&](GreenNode *&opKeyword) -> GreenNode * {
        if (CurrentToken()->kind == SyntaxKind::UncheckedKeyword) {
            // if we encounter `operator unchecked`, we place the `unchecked` as skipped trivia on `operator`
            GreenNode *misplacedToken = AddErrorAsWarning(EatToken(), ErrorCode::ERR_MisplacedUnchecked);
            opKeyword = AddTrailingSkippedSyntax(opKeyword, misplacedToken);
            return nullptr;
        }

        return TryEatToken(SyntaxKind::CheckedKeyword);
    };

    GreenNode *checkedKeyword = tryEatCheckedOrHandleUnchecked(operatorKeyword);

    if (checkedKeyword != nullptr && (isConversion || SyntaxFacts::IsAnyOverloadableOperator(CurrentToken()->kind))) {
        checkedKeyword = CheckFeatureAvailability(checkedKeyword);  // IDS_FeatureCheckedUserDefinedOperators
    }

    return checkedKeyword;
}

/// Parse a user-defined conversion, with optional parameters.
GreenNode *DocumentationCommentParser::ParseConversionOperatorMemberCref() {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::ExplicitKeyword || CurrentToken()->kind == SyntaxKind::ImplicitKeyword);
    GreenNode *implicitOrExplicit = EatToken();

    GreenNode *operatorKeyword = EatToken(SyntaxKind::OperatorKeyword);
    GreenNode *checkedKeyword = TryEatCheckedKeyword(/*isConversion*/ true, operatorKeyword);

    GreenNode *type = ParseCrefType(/*typeArgumentsMustBeIdentifiers*/ false);

    GreenNode *parameters = ParseCrefParameterList();

    return SyntaxFactory::ConversionOperatorMemberCref(implicitOrExplicit, operatorKeyword, checkedKeyword, type,
                                                       parameters);
}

/// Parse a parenthesized parameter list.
GreenNode *DocumentationCommentParser::ParseCrefParameterList() {
    return ParseBaseCrefParameterList(/*useSquareBrackets*/ false);
}

/// Parse a bracketed parameter list.
GreenNode *DocumentationCommentParser::ParseBracketedCrefParameterList() {
    return ParseBaseCrefParameterList(/*useSquareBrackets*/ true);
}

/// Parse the parameter list (if any) of a cref member (name, indexer, operator, or conversion).
GreenNode *DocumentationCommentParser::ParseBaseCrefParameterList(bool useSquareBrackets) {
    SyntaxKind openKind = useSquareBrackets ? SyntaxKind::OpenBracketToken : SyntaxKind::OpenParenToken;
    SyntaxKind closeKind = useSquareBrackets ? SyntaxKind::CloseBracketToken : SyntaxKind::CloseParenToken;

    if (CurrentToken()->kind != openKind) {
        return nullptr;
    }

    GreenNode *open = EatToken(openKind);

    SeparatedSyntaxListBuilder list = _pool.AllocateSeparated();
    while (CurrentToken()->kind == SyntaxKind::CommaToken || IsPossibleCrefParameter()) {
        list.Add(ParseCrefParameter());

        if (CurrentToken()->kind != closeKind) {
            GreenNode *comma = EatToken(SyntaxKind::CommaToken);
            if (!comma->IsMissing() || IsPossibleCrefParameter()) {
                // Only do this if it won't be last in the list.
                list.AddSeparator(comma);
            } else {
                // How could this scenario arise?  If it does, just expand the if-condition.
                CS_ASSERT(CurrentToken()->kind != SyntaxKind::CommaToken);
            }
        }
    }

    // NOTE: nothing follows a cref parameter list, so there's no reason to recover here.
    // Just let the cref-level recovery code handle any remaining tokens.

    GreenNode *close = EatToken(closeKind);

    GreenNode *result = useSquareBrackets ? SyntaxFactory::CrefBracketedParameterList(open, list, close)
                                          : SyntaxFactory::CrefParameterList(open, list, close);
    _pool.Free(list);
    return result;
}

/// True if the current token could be the beginning of a cref parameter.
bool DocumentationCommentParser::IsPossibleCrefParameter() {
    SyntaxKind kind = this->CurrentToken()->kind;
    switch (kind) {
        case SyntaxKind::RefKeyword:
        case SyntaxKind::OutKeyword:
        case SyntaxKind::InKeyword:
        case SyntaxKind::IdentifierToken:
            return true;
        default:
            return SyntaxFacts::IsPredefinedType(kind);
    }
}

/// Parse an element of a cref parameter list.
/// "ref", "ref readonly", "in", "out" work, but "params", "this", and "__arglist" don't.
GreenNode *DocumentationCommentParser::ParseCrefParameter() {
    GreenNode *refKindOpt = nullptr;
    switch (CurrentToken()->kind) {
        case SyntaxKind::RefKeyword:
        case SyntaxKind::OutKeyword:
        case SyntaxKind::InKeyword:
            refKindOpt = EatToken();
            break;
        default:
            break;
    }

    GreenNode *readOnlyOpt = nullptr;
    if (CurrentToken()->kind == SyntaxKind::ReadOnlyKeyword && refKindOpt != nullptr) {
        if (refKindOpt->kind != SyntaxKind::RefKeyword) {
            // if we encounter `readonly` after `in` or `out`, we place the `readonly` as skipped trivia on the previous keyword
            GreenNode *misplacedToken = AddErrorAsWarning(EatToken(), ErrorCode::ERR_RefReadOnlyWrongOrdering);
            refKindOpt = AddTrailingSkippedSyntax(refKindOpt, misplacedToken);
        } else {
            readOnlyOpt = EatToken();
        }
    }

    GreenNode *type = ParseCrefType(/*typeArgumentsMustBeIdentifiers*/ false);
    return SyntaxFactory::CrefParameter(/*refKindKeyword*/ refKindOpt, /*readOnlyKeyword*/ readOnlyOpt, type);
}

/// Parse an identifier, optionally followed by an angle-bracketed list of type parameters.
GreenNode *DocumentationCommentParser::ParseCrefName(bool typeArgumentsMustBeIdentifiers) {
    GreenNode *identifierToken = EatToken(SyntaxKind::IdentifierToken);

    if (CurrentToken()->kind != SyntaxKind::LessThanToken) {
        return SyntaxFactory::IdentifierName(identifierToken);
    }

    return SyntaxFactory::GenericName(identifierToken, ParseTypeArguments(typeArgumentsMustBeIdentifiers));
}

GreenNode *DocumentationCommentParser::ParseTypeArguments(bool typeArgumentsMustBeIdentifiers) {
    CS_ASSERT(CurrentToken()->kind == SyntaxKind::LessThanToken);
    GreenNode *open = EatToken();

    SeparatedSyntaxListBuilder list = _pool.AllocateSeparated();
    while (true) {
        GreenNode *typeSyntax = ParseCrefType(typeArgumentsMustBeIdentifiers);

        if (typeArgumentsMustBeIdentifiers && typeSyntax->kind != SyntaxKind::IdentifierName) {
            // AddError(typeSyntax, WRN_ErrorOverride, new SyntaxDiagnosticInfo(ERR_TypeParamMustBeIdentifier),
            //          $"{(int)ErrorCode.ERR_TypeParamMustBeIdentifier:d4}")
            SyntaxDiagnosticInfo inner;
            inner.code = ErrorCode::ERR_TypeParamMustBeIdentifier;
            std::string number = std::to_string(static_cast<int>(ErrorCode::ERR_TypeParamMustBeIdentifier));
            while (number.size() < 4) number.insert(number.begin(), '0');
            typeSyntax = this->AddError(typeSyntax, ErrorCode::WRN_ErrorOverride,
                                        {ArgText(ToUtf16(FormatDiagnosticMessage(inner))), ArgText(ToUtf16(number))});
        }

        list.Add(typeSyntax);

        SyntaxKind currentKind = CurrentToken()->kind;
        if (currentKind == SyntaxKind::CommaToken || currentKind == SyntaxKind::IdentifierToken ||
            SyntaxFacts::IsPredefinedType(CurrentToken()->kind)) {
            // NOTE: if the current token is an identifier or predefined type, then we're
            // actually inserting a missing commas.
            list.AddSeparator(EatToken(SyntaxKind::CommaToken));
        } else {
            break;
        }
    }

    GreenNode *close = EatToken(SyntaxKind::GreaterThanToken);

    open = CheckFeatureAvailability(open);  // IDS_FeatureGenerics

    GreenNode *result = SyntaxFactory::TypeArgumentList(open, list, close);
    _pool.Free(list);
    return result;
}

/// Parse a type.  May include an alias, a predefined type, and/or a qualified name.
/// Pointer, nullable, or array types are only allowed if typeArgumentsMustBeIdentifiers is false.
/// Leaves a dot and a name unconsumed if the name is not followed by another dot
/// and checkForMember is true.
GreenNode *DocumentationCommentParser::ParseCrefType(bool typeArgumentsMustBeIdentifiers, bool checkForMember) {
    GreenNode *typeWithoutSuffix = ParseCrefTypeHelper(typeArgumentsMustBeIdentifiers, checkForMember);
    return typeArgumentsMustBeIdentifiers ? typeWithoutSuffix : ParseCrefTypeSuffix(typeWithoutSuffix);
}

/// Parse a type.  May include an alias, a predefined type, and/or a qualified name.
/// No pointer, nullable, or array types.
/// Leaves a dot and a name unconsumed if the name is not followed by another dot
/// and checkForMember is true.
GreenNode *DocumentationCommentParser::ParseCrefTypeHelper(bool typeArgumentsMustBeIdentifiers, bool checkForMember) {
    GreenNode *leftName;

    if (SyntaxFacts::IsPredefinedType(CurrentToken()->kind)) {
        // e.g. "int"
        // NOTE: a predefined type will not fit into a NameSyntax, so we'll return
        // immediately.  The upshot is that you can only dot into a predefined type
        // once (e.g. not "int.A.B"), which is fine because we know that none of them
        // have nested types.
        return SyntaxFactory::PredefinedType(EatToken());
    } else if (CurrentToken()->kind == SyntaxKind::IdentifierToken && PeekToken(1)->kind == SyntaxKind::ColonColonToken) {
        // e.g. "A::B"
        GreenNode *alias = EatToken();
        if (alias->contextual_kind == SyntaxKind::GlobalKeyword) {
            alias = ConvertToKeyword(alias);
        }

        alias = CheckFeatureAvailability(alias);  // IDS_FeatureGlobalNamespace

        GreenNode *colonColon = EatToken();
        GreenNode *name = ParseCrefName(typeArgumentsMustBeIdentifiers);
        leftName = SyntaxFactory::AliasQualifiedName(SyntaxFactory::IdentifierName(alias), colonColon, name);
    } else {
        // e.g. "A"
        ResetPoint resetPoint = GetResetPoint();
        leftName = ParseCrefName(typeArgumentsMustBeIdentifiers);
        if (checkForMember && (leftName->IsMissing() || CurrentToken()->kind != SyntaxKind::DotToken)) {
            // If this isn't the first part of a dotted name, then we prefer to represent it
            // as a MemberCrefSyntax.
            this->Reset(resetPoint);
            this->Release(resetPoint);

            return nullptr;
        }
        this->Release(resetPoint);
    }

    while (CurrentToken()->kind == SyntaxKind::DotToken) {
        // NOTE: we make a lot of these, but we'll reset, at most, one time.
        ResetPoint resetPoint = GetResetPoint();

        GreenNode *dot = EatToken();

        GreenNode *rightName = ParseCrefName(typeArgumentsMustBeIdentifiers);

        if (checkForMember && (rightName->IsMissing() || CurrentToken()->kind != SyntaxKind::DotToken)) {
            this->Reset(resetPoint);  // Go back to before the dot - it must have been the trailing dot.
            this->Release(resetPoint);

            return leftName;
        }

        this->Release(resetPoint);

        leftName = SyntaxFactory::QualifiedName(leftName, dot, rightName);
    }

    return leftName;
}

/// Once the name part of a type (including type parameter/argument lists) is parsed,
/// we need to consume ?, *, and rank specifiers.
GreenNode *DocumentationCommentParser::ParseCrefTypeSuffix(GreenNode *type) {
    if (CurrentToken()->kind == SyntaxKind::QuestionToken) {
        type = SyntaxFactory::NullableType(type, EatToken());
    }

    while (CurrentToken()->kind == SyntaxKind::AsteriskToken) {
        type = SyntaxFactory::PointerType(type, EatToken());
    }

    if (CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
        GreenNode *omittedArraySizeExpressionInstance =
            SyntaxFactory::OmittedArraySizeExpression(SyntaxFactory::Token(SyntaxKind::OmittedArraySizeExpressionToken));
        SyntaxListBuilder rankList = _pool.Allocate();
        while (CurrentToken()->kind == SyntaxKind::OpenBracketToken) {
            GreenNode *open = EatToken();
            SeparatedSyntaxListBuilder dimensionList = _pool.AllocateSeparated();
            while (this->CurrentToken()->kind != SyntaxKind::CloseBracketToken) {
                if (this->CurrentToken()->kind == SyntaxKind::CommaToken) {
                    // NOTE: trivia will be attached to comma, not omitted array size
                    dimensionList.Add(omittedArraySizeExpressionInstance);
                    dimensionList.AddSeparator(this->EatToken());
                } else {
                    // CONSIDER: if we expect people to try to put expressions in between
                    // the commas, then it might be more reasonable to recover by skipping
                    // tokens until we hit a CloseBracketToken (or some other terminator).
                    break;
                }
            }

            // Don't end on a comma.
            // If the omitted size would be the only element, then skip it unless sizes were expected.
            if ((dimensionList.Count() & 1) == 0) {
                dimensionList.Add(omittedArraySizeExpressionInstance);
            }

            // Eat the close brace and we're done.
            GreenNode *close = this->EatToken(SyntaxKind::CloseBracketToken);

            rankList.Add(SyntaxFactory::ArrayRankSpecifier(open, dimensionList, close));
            _pool.Free(dimensionList);
        }

        type = SyntaxFactory::ArrayType(type, rankList);
        _pool.Free(rankList);
    }
    return type;
}

/// Ends at appropriate quotation mark, EOF, or EndOfDocumentationComment.
bool DocumentationCommentParser::IsEndOfCrefAttribute() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::SingleQuoteToken:
            return (this->Mode() & LexerMode::XmlCrefQuote) == LexerMode::XmlCrefQuote;
        case SyntaxKind::DoubleQuoteToken:
            return (this->Mode() & LexerMode::XmlCrefDoubleQuote) == LexerMode::XmlCrefDoubleQuote;
        case SyntaxKind::EndOfFileToken:
        case SyntaxKind::EndOfDocumentationCommentToken:
            return true;
        case SyntaxKind::BadToken:
            // If it's a real '<' (not &lt;, etc), then we assume it's the beginning
            // of the next XML element.
            return CurrentToken()->text == SyntaxFacts::GetText(SyntaxKind::LessThanToken) ||
                   IsNonAsciiQuotationMark(CurrentToken());
        default:
            return false;
    }
}

/// Convenience method for checking the mode.
bool DocumentationCommentParser::InCref() const {
    LexerMode m = this->Mode() & (LexerMode::XmlCrefDoubleQuote | LexerMode::XmlCrefQuote);
    return m == LexerMode::XmlCrefQuote || m == LexerMode::XmlCrefDoubleQuote;
}

// --- Name attribute values ----------------------------------------------------------------------------

GreenNode *DocumentationCommentParser::ParseNameAttributeValue() {
    // Never report a parse error - just fail to bind the name later on.
    GreenNode *identifierToken = this->EatToken(SyntaxKind::IdentifierToken, /*reportError*/ false);

    if (!IsEndOfNameAttribute()) {
        SyntaxListBuilder badTokens = _pool.Allocate();
        while (!IsEndOfNameAttribute()) {
            badTokens.Add(this->EatToken());
        }
        identifierToken = AddTrailingSkippedSyntax(identifierToken, badTokens.ToListNode());
        _pool.Free(badTokens);
    }

    return SyntaxFactory::IdentifierName(identifierToken);
}

/// Ends at appropriate quotation mark, EOF, or EndOfDocumentationComment.
bool DocumentationCommentParser::IsEndOfNameAttribute() {
    switch (CurrentToken()->kind) {
        case SyntaxKind::SingleQuoteToken:
            return (this->Mode() & LexerMode::XmlNameQuote) == LexerMode::XmlNameQuote;
        case SyntaxKind::DoubleQuoteToken:
            return (this->Mode() & LexerMode::XmlNameDoubleQuote) == LexerMode::XmlNameDoubleQuote;
        case SyntaxKind::EndOfFileToken:
        case SyntaxKind::EndOfDocumentationCommentToken:
            return true;
        case SyntaxKind::BadToken:
            // If it's a real '<' (not &lt;, etc), then we assume it's the beginning
            // of the next XML element.
            return CurrentToken()->text == SyntaxFacts::GetText(SyntaxKind::LessThanToken) ||
                   IsNonAsciiQuotationMark(CurrentToken());
        default:
            return false;
    }
}

}  // namespace csharp
