// Port of acorn-jsx 5.3.2 (MIT), with its default options
// (allowNamespaces: true, allowNamespacedObjects: false).

#include <cmath>
#include <string>

#include "js_parse_parser.h"

namespace jsparse {

namespace {

bool IsNewLineCh(int ch) { return ch == 10 || ch == 13 || ch == 0x2028 || ch == 0x2029; }

bool AllMatch(std::u16string_view s, bool hex) {
    if (s.empty()) return false;
    for (char16_t c : s) {
        bool ok = (c >= u'0' && c <= u'9') || (hex && ((c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F')));
        if (!ok) return false;
    }
    return true;
}

// String.fromCharCode(parseInt(s, radix)) for an all-digit s.
char16_t FromCharCodeParse(std::u16string_view s, int radix) {
    double v = 0;
    for (char16_t c : s) {
        int d = c <= u'9' ? c - u'0' : (c | 0x20) - u'a' + 10;
        v = v * radix + d;
    }
    // ToUint16
    double m = std::fmod(v, 65536.0);
    return static_cast<char16_t>(static_cast<uint32_t>(m));
}

// getQualifiedJSXName; nullopt for acorn's undefined.
bool QualifiedName(Node* object, std::string& out) {
    if (!object) return false;
    if (object->type == NodeType::JSXIdentifier) {
        out = std::string(object->str);
        return true;
    }
    if (object->type == NodeType::JSXNamespacedName) {
        out = std::string(object->nspace()->str) + ":" + std::string(object->name()->str);
        return true;
    }
    if (object->type == NodeType::JSXMemberExpression) {
        std::string a, b;
        if (!QualifiedName(object->object(), a)) a = "undefined";
        if (!QualifiedName(object->property(), b)) b = "undefined";
        out = a + "." + b;
        return true;
    }
    return false;
}

}  // namespace

void JsxParser::jsx_readToken() {
    std::u16string outStr;
    int chunkStart = pos;
    for (;;) {
        if (pos >= inputLen) raise(start, "Unterminated JSX contents");
        int ch = input[static_cast<size_t>(pos)];
        switch (ch) {
        case 60:
        case 123:
            if (pos == start) {
                if (ch == 60 && exprAllowed) {
                    ++pos;
                    return finishToken(tt::jsxTagStart);
                }
                return getTokenFromCode(ch);
            }
            outStr += slice(chunkStart, pos);
            return finishToken(tt::jsxText, outStr);
        case 38:
            outStr += slice(chunkStart, pos);
            outStr += jsx_readEntity();
            chunkStart = pos;
            break;
        case 62:
        case 125: {
            std::string c(1, static_cast<char>(ch));
            raise(pos, "Unexpected token `" + c + "`. Did you mean `" + (ch == 62 ? "&gt;" : "&rbrace;") + "` or `{\"" +
                           c + "\"}`?");
        }
        default:
            if (IsNewLineCh(ch)) {
                outStr += slice(chunkStart, pos);
                outStr += jsx_readNewLine(true);
                chunkStart = pos;
            } else {
                ++pos;
            }
        }
    }
}

std::u16string JsxParser::jsx_readNewLine(bool normalizeCRLF) {
    int ch = input[static_cast<size_t>(pos)];
    std::u16string o;
    ++pos;
    if (ch == 13 && charAt(pos) == 10) {
        ++pos;
        o = normalizeCRLF ? u"\n" : u"\r\n";
    } else {
        o = std::u16string(1, static_cast<char16_t>(ch));
    }
    ++curLine;
    lineStart = pos;
    return o;
}

void JsxParser::jsx_readString(int quote) {
    std::u16string o;
    int chunkStart = ++pos;
    for (;;) {
        if (pos >= inputLen) raise(start, "Unterminated string constant");
        int ch = input[static_cast<size_t>(pos)];
        if (ch == quote) break;
        if (ch == 38) {
            o += slice(chunkStart, pos);
            o += jsx_readEntity();
            chunkStart = pos;
        } else if (IsNewLineCh(ch)) {
            o += slice(chunkStart, pos);
            o += jsx_readNewLine(false);
            chunkStart = pos;
        } else {
            ++pos;
        }
    }
    o += slice(chunkStart, pos++);
    finishToken(tt::string, o);
}

std::u16string JsxParser::jsx_readEntity() {
    std::u16string s;
    int count = 0;
    bool found = false;
    std::u16string entity;
    if (charAt(pos) != '&') raise(pos, "Entity must start with an ampersand");
    int startPos = ++pos;
    while (pos < inputLen && count++ < 10) {
        char16_t ch = input[static_cast<size_t>(pos++)];
        if (ch == u';') {
            if (!s.empty() && s[0] == u'#') {
                if (s.size() > 1 && s[1] == u'x') {
                    std::u16string_view h = std::u16string_view(s).substr(2);
                    if (AllMatch(h, true)) {
                        entity = std::u16string(1, FromCharCodeParse(h, 16));
                        found = true;
                    }
                } else {
                    std::u16string_view d = std::u16string_view(s).substr(1);
                    if (AllMatch(d, false)) {
                        entity = std::u16string(1, FromCharCodeParse(d, 10));
                        found = true;
                    }
                }
            } else {
                int code = XhtmlEntityCode(s);
                if (code >= 0) {
                    entity = std::u16string(1, static_cast<char16_t>(code));
                    found = true;
                } else if (s == u"toString" || s == u"valueOf") {
                    // XHTMLEntities is a plain object: inherited methods
                    // stringify as functions.
                    entity = u"function " + s + u"() { [native code] }";
                    found = true;
                } else if (s == u"__proto__") {
                    entity = u"[object Object]";
                    found = true;
                }
            }
            break;
        }
        s += ch;
    }
    if (!found) {
        pos = startPos;
        return u"&";
    }
    return entity;
}

void JsxParser::jsx_readWord() {
    int ch;
    int s = pos;
    do {
        ch = charAt(++pos);
    } while ((ch >= 0 && IsIdentifierChar(ch)) || ch == 45);
    finishToken(tt::jsxName, slice(s, pos));
}

Node* JsxParser::jsx_parseIdentifier() {
    Node* node = startNode();
    if (type == &tt::jsxName)
        node->str = str(value);
    else if (type->keyword)
        node->str = type->keyword;
    else
        unexpected();
    next();
    return finishNode(node, NodeType::JSXIdentifier);
}

Node* JsxParser::jsx_parseNamespacedName() {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* nm = jsx_parseIdentifier();
    if (!eat(tt::colon)) return nm;
    Node* node = startNodeAt(startPos, sLoc);
    node->nspace() = nm;
    node->name() = jsx_parseIdentifier();
    return finishNode(node, NodeType::JSXNamespacedName);
}

Node* JsxParser::jsx_parseElementName() {
    if (type == &tt::jsxTagEnd) return nullptr;
    int startPos = start;
    Pos sLoc = startLoc;
    Node* node = jsx_parseNamespacedName();
    if (type == &tt::dot && node->type == NodeType::JSXNamespacedName) unexpected();
    while (eat(tt::dot)) {
        Node* newNode = startNodeAt(startPos, sLoc);
        newNode->object() = node;
        newNode->property() = jsx_parseIdentifier();
        node = finishNode(newNode, NodeType::JSXMemberExpression);
    }
    return node;
}

Node* JsxParser::jsx_parseAttributeValue() {
    if (type == &tt::braceL) {
        Node* node = jsx_parseExpressionContainer();
        if (node->expression()->type == NodeType::JSXEmptyExpression)
            raise(static_cast<int>(node->ustart), "JSX attributes must only be assigned a non-empty expression");
        return node;
    }
    if (type == &tt::jsxTagStart || type == &tt::string) return parseExprAtom();
    raise(start, "JSX value should be either an expression or a quoted JSX text");
}

Node* JsxParser::jsx_parseEmptyExpression() {
    Node* node = startNodeAt(lastTokEnd, lastTokEndLoc);
    return finishNodeAt(node, NodeType::JSXEmptyExpression, start, startLoc);
}

Node* JsxParser::jsx_parseExpressionContainer() {
    Node* node = startNode();
    next();
    node->expression() = type == &tt::braceR ? jsx_parseEmptyExpression() : parseExpression();
    expect(tt::braceR);
    return finishNode(node, NodeType::JSXExpressionContainer);
}

Node* JsxParser::jsx_parseAttribute() {
    Node* node = startNode();
    if (eat(tt::braceL)) {
        expect(tt::ellipsis);
        node->argument() = parseMaybeAssign();
        expect(tt::braceR);
        return finishNode(node, NodeType::JSXSpreadAttribute);
    }
    node->name() = jsx_parseNamespacedName();
    node->value() = eat(tt::eq) ? jsx_parseAttributeValue() : nullptr;
    return finishNode(node, NodeType::JSXAttribute);
}

Node* JsxParser::jsx_parseOpeningElementAt(int startPos, Pos sLoc) {
    Node* node = startNodeAt(startPos, sLoc);
    std::vector<Node*> attributes;
    Node* nodeName = jsx_parseElementName();
    if (nodeName) node->name() = nodeName;
    while (type != &tt::slash && type != &tt::jsxTagEnd) attributes.push_back(jsx_parseAttribute());
    node->attributes() = list(attributes);
    node->Set(kFlagSelfClosing, eat(tt::slash));
    expect(tt::jsxTagEnd);
    return finishNode(node, nodeName ? NodeType::JSXOpeningElement : NodeType::JSXOpeningFragment);
}

Node* JsxParser::jsx_parseClosingElementAt(int startPos, Pos sLoc) {
    Node* node = startNodeAt(startPos, sLoc);
    Node* nodeName = jsx_parseElementName();
    if (nodeName) node->name() = nodeName;
    expect(tt::jsxTagEnd);
    return finishNode(node, nodeName ? NodeType::JSXClosingElement : NodeType::JSXClosingFragment);
}

Node* JsxParser::jsx_parseElementAt(int startPos, Pos sLoc) {
    DepthGuard guard(*this);
    Node* node = startNodeAt(startPos, sLoc);
    std::vector<Node*> children;
    Node* openingElement = jsx_parseOpeningElementAt(startPos, sLoc);
    Node* closingElement = nullptr;
    if (!openingElement->Has(kFlagSelfClosing)) {
        bool done = false;
        while (!done) {
            if (type == &tt::jsxTagStart) {
                startPos = start;
                sLoc = startLoc;
                next();
                if (eat(tt::slash)) {
                    closingElement = jsx_parseClosingElementAt(startPos, sLoc);
                    done = true;
                } else {
                    children.push_back(jsx_parseElementAt(startPos, sLoc));
                }
            } else if (type == &tt::jsxText) {
                children.push_back(parseExprAtom());
            } else if (type == &tt::braceL) {
                children.push_back(jsx_parseExpressionContainer());
            } else {
                unexpected();
            }
        }
        std::string closingName, openingName;
        bool hasClosing = QualifiedName(closingElement->name(), closingName);
        bool hasOpening = QualifiedName(openingElement->name(), openingName);
        if (hasClosing != hasOpening || closingName != openingName)
            raise(static_cast<int>(closingElement->ustart),
                  "Expected corresponding JSX closing tag for <" + (hasOpening ? openingName : "undefined") + ">");
    }
    bool isElement = openingElement->name() != nullptr;
    node->opening() = openingElement;
    node->closing() = closingElement;
    node->children() = list(children);
    if (type == &tt::relational && value == "<")
        raise(start, "Adjacent JSX elements must be wrapped in an enclosing tag");
    return finishNode(node, isElement ? NodeType::JSXElement : NodeType::JSXFragment);
}

Node* JsxParser::jsx_parseText() {
    // acorn's parseLiteral(this.value), retyped: raw ending in "n" gets a
    // `bigint` (equal to the value), flagged with LitKind::BigInt.
    Node* node = startNode();
    node->str2 = str(value);
    node->str = str(sliceStr(start, end));
    node->lit = !node->str.empty() && node->str.back() == 'n' ? LitKind::BigInt : LitKind::String;
    next();
    return finishNode(node, NodeType::JSXText);
}

Node* JsxParser::jsx_parseElement() {
    int startPos = start;
    Pos sLoc = startLoc;
    next();
    return jsx_parseElementAt(startPos, sLoc);
}

Node* JsxParser::parseExprAtom(DestructuringErrors* r, ForInit, bool) {
    if (type == &tt::jsxText) return jsx_parseText();
    if (type == &tt::jsxTagStart) return jsx_parseElement();
    // acorn-jsx passes only refShortHandDefaultPos on: forInit and forNew
    // are dropped.
    return Parser::parseExprAtom(r, ForInit::No, false);
}

void JsxParser::readToken(int code) {
    const TokContext* ctx = curContext();
    if (ctx == &tc::j_expr) return jsx_readToken();
    if (ctx == &tc::j_oTag || ctx == &tc::j_cTag) {
        if (IsIdentifierStart(code)) return jsx_readWord();
        if (code == 62) {
            ++pos;
            return finishToken(tt::jsxTagEnd);
        }
        if ((code == 34 || code == 39) && ctx == &tc::j_oTag) return jsx_readString(code);
    }
    if (code == 60 && exprAllowed && charAt(pos + 1) != 33) {
        ++pos;
        return finishToken(tt::jsxTagStart);
    }
    Parser::readToken(code);
}

void JsxParser::updateContext(const TokenType* prevType) {
    if (type == &tt::braceL) {
        const TokContext* cur = curContext();
        if (cur == &tc::j_oTag)
            context.push_back(&tc::b_expr);
        else if (cur == &tc::j_expr)
            context.push_back(&tc::b_tmpl);
        else
            Parser::updateContext(prevType);
        exprAllowed = true;
    } else if (type == &tt::slash && prevType == &tt::jsxTagStart) {
        context.resize(context.size() >= 2 ? context.size() - 2 : 0);
        context.push_back(&tc::j_cTag);
        exprAllowed = false;
    } else {
        Parser::updateContext(prevType);
    }
}

}  // namespace jsparse
