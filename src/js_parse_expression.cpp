// Port of acorn 8.15.0 (MIT): expression parsing (expression.js). See
// js_parse_parser.h.

#include <cstring>
#include <string>

#include "js_parse_parser.h"

namespace jsparse {

namespace {

// Operator strings live in static storage so Node::op can point at them.
std::string_view InternOp(const std::string& op) {
    static const char* const kOps[] = {
        "=",   "+=",  "-=",  "*=",   "/=",  "%=",  "**=", "<<=", ">>=", ">>>=", "|=", "^=", "&=", "||=", "&&=",
        "?\?=", "||",  "&&",  "??",   "|",   "^",   "&",   "==",  "!=",  "===",  "!==", "<", ">", "<=",  ">=",
        "<<",  ">>",  ">>>", "+",    "-",   "*",   "/",   "%",   "**",  "in",   "instanceof", "!", "~", "typeof",
        "void", "delete", "++", "--"};
    for (const char* o : kOps)
        if (op == o) return o;
    return {};
}

bool IsLocalVariableAccess(Node* node) {
    return node->type == NodeType::Identifier ||
           (node->type == NodeType::ParenthesizedExpression && IsLocalVariableAccess(node->expression()));
}

bool IsPrivateFieldAccess(Node* node) {
    return (node->type == NodeType::MemberExpression && node->property()->type == NodeType::PrivateIdentifier) ||
           (node->type == NodeType::ChainExpression && IsPrivateFieldAccess(node->expression())) ||
           (node->type == NodeType::ParenthesizedExpression && IsPrivateFieldAccess(node->expression()));
}

std::string NormalizeTemplateRaw(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            o += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            o += s[i];
        }
    }
    return o;
}

}  // namespace

std::string JsNumberToString(double v);  // js_parse_api.cpp

// ---------------------------------------------------------------- property clash

void Parser::checkPropClash(Node* prop, PropHash& propHash, DestructuringErrors* r) {
    if (ecmaVersion >= 9 && prop->type == NodeType::SpreadElement) return;
    if (ecmaVersion >= 6 && (prop->Has(kFlagComputed) || prop->Has(kFlagMethod) || prop->Has(kFlagShorthand))) return;
    Node* key = prop->key();
    std::string name;
    if (key->type == NodeType::Identifier) {
        name = key->str;
    } else if (key->type == NodeType::Literal) {
        switch (key->lit) {
        case LitKind::String:
            name = key->str2;
            break;
        case LitKind::Number:
            name = JsNumberToString(key->num);
            break;
        case LitKind::BigInt:
            name = key->str2;
            break;
        case LitKind::Null:
            name = "null";
            break;
        case LitKind::Bool:
            name = key->Has(kFlagBoolValue) ? "true" : "false";
            break;
        default:
            name = std::string("/") + std::string(key->str2) + "/" + std::string(key->str3);
            break;
        }
    } else {
        return;
    }
    std::string_view kind = prop->kind;
    if (ecmaVersion >= 6) {
        if (name == "__proto__" && kind == "init") {
            if (propHash.proto) {
                if (r) {
                    if (r->doubleProto < 0) r->doubleProto = static_cast<int>(key->ustart);
                } else {
                    raiseRecoverable(static_cast<int>(key->ustart), "Redefinition of __proto__ property");
                }
            }
            propHash.proto = true;
        }
        return;
    }
    PropHashEntry* other = nullptr;
    for (auto& e : propHash.entries)
        if (e.name == name) other = &e;
    if (other) {
        bool redefinition;
        if (kind == "init")
            redefinition = (strict && other->init) || other->get || other->set;
        else
            redefinition = other->init || (kind == "get" ? other->get : other->set);
        if (redefinition) raiseRecoverable(static_cast<int>(key->ustart), "Redefinition of property");
    } else {
        propHash.entries.push_back(PropHashEntry{name, false, false, false});
        other = &propHash.entries.back();
    }
    if (kind == "init") other->init = true;
    if (kind == "get") other->get = true;
    if (kind == "set") other->set = true;
}

// ---------------------------------------------------------------- expressions

Node* Parser::parseExpression(ForInit forInit, DestructuringErrors* r) {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr = parseMaybeAssign(forInit, r);
    if (type == &tt::comma) {
        Node* node = startNodeAt(startPos, sLoc);
        std::vector<Node*> exprs{expr};
        while (eat(tt::comma)) exprs.push_back(parseMaybeAssign(forInit, r));
        node->expressions() = list(exprs);
        return finishNode(node, NodeType::SequenceExpression);
    }
    return expr;
}

Node* Parser::parseMaybeAssign(ForInit forInit, DestructuringErrors* r, bool afterLeftParseParenItem) {
    DepthGuard guard(*this);
    if (isContextual("yield")) {
        if (inGenerator()) return parseYield(forInit);
        exprAllowed = false;
    }
    bool ownDestructuringErrors = false;
    int oldParenAssign = -1, oldTrailingComma = -1, oldDoubleProto = -1;
    DestructuringErrors own;
    if (r) {
        oldParenAssign = r->parenthesizedAssign;
        oldTrailingComma = r->trailingComma;
        oldDoubleProto = r->doubleProto;
        r->parenthesizedAssign = r->trailingComma = -1;
    } else {
        r = &own;
        ownDestructuringErrors = true;
    }
    int startPos = start;
    Pos sLoc = startLoc;
    if (type == &tt::parenL || type == &tt::name) {
        potentialArrowAt = start;
        potentialArrowInForAwait = forInit == ForInit::Await;
    }
    Node* left = parseMaybeConditional(forInit, r);
    if (afterLeftParseParenItem) left = parseParenItem(left);
    if (type->isAssign) {
        Node* node = startNodeAt(startPos, sLoc);
        node->op = InternOp(value);
        if (type == &tt::eq) left = toAssignable(left, false, r);
        if (!ownDestructuringErrors) r->parenthesizedAssign = r->trailingComma = r->doubleProto = -1;
        if (r->shorthandAssign >= static_cast<int>(left->ustart)) r->shorthandAssign = -1;
        if (type == &tt::eq)
            checkLValPattern(left);
        else
            checkLValSimple(left);
        node->left() = left;
        next();
        node->right() = parseMaybeAssign(forInit);
        if (oldDoubleProto > -1) r->doubleProto = oldDoubleProto;
        return finishNode(node, NodeType::AssignmentExpression);
    }
    if (ownDestructuringErrors) checkExpressionErrors(r, true);
    if (oldParenAssign > -1) r->parenthesizedAssign = oldParenAssign;
    if (oldTrailingComma > -1) r->trailingComma = oldTrailingComma;
    return left;
}

Node* Parser::parseMaybeConditional(ForInit forInit, DestructuringErrors* r) {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr = parseExprOps(forInit, r);
    if (checkExpressionErrors(r)) return expr;
    if (eat(tt::question)) {
        Node* node = startNodeAt(startPos, sLoc);
        node->test() = expr;
        node->consequent() = parseMaybeAssign();
        expect(tt::colon);
        node->alternate() = parseMaybeAssign(forInit);
        return finishNode(node, NodeType::ConditionalExpression);
    }
    return expr;
}

Node* Parser::parseExprOps(ForInit forInit, DestructuringErrors* r) {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr = parseMaybeUnary(r, false, false, forInit);
    if (checkExpressionErrors(r)) return expr;
    return static_cast<int>(expr->ustart) == startPos && expr->type == NodeType::ArrowFunctionExpression
               ? expr
               : parseExprOp(expr, startPos, sLoc, -1, forInit);
}

Node* Parser::parseExprOp(Node* left, int leftStartPos, Pos leftStartLoc, int minPrec, ForInit forInit) {
    // acorn's tail call `return this.parseExprOp(node, ...)` is the loop.
    for (;;) {
        int prec = type->binop;
        if (prec < 0 || (forInit != ForInit::No && type == &tt::_in) || prec <= minPrec) return left;
        bool logical = type == &tt::logicalOR || type == &tt::logicalAND;
        bool coalesce = type == &tt::coalesce;
        if (coalesce) prec = tt::logicalAND.binop;
        std::string op = value;
        next();
        int startPos = start;
        Pos sLoc = startLoc;
        Node* right = parseExprOp(parseMaybeUnary(nullptr, false, false, forInit), startPos, sLoc, prec, forInit);
        Node* node = buildBinary(leftStartPos, leftStartLoc, left, right, InternOp(op), logical || coalesce);
        if ((logical && type == &tt::coalesce) ||
            (coalesce && (type == &tt::logicalOR || type == &tt::logicalAND)))
            raiseRecoverable(start,
                             "Logical expressions and coalesce expressions cannot be mixed. Wrap either by parentheses");
        left = node;
    }
}

Node* Parser::buildBinary(int startPos, Pos sLoc, Node* left, Node* right, std::string_view op, bool logical) {
    if (right->type == NodeType::PrivateIdentifier)
        raise(static_cast<int>(right->ustart), "Private identifier can only be left side of binary expression");
    Node* node = startNodeAt(startPos, sLoc);
    node->left() = left;
    node->op = op;
    node->right() = right;
    return finishNode(node, logical ? NodeType::LogicalExpression : NodeType::BinaryExpression);
}

Node* Parser::parseMaybeUnary(DestructuringErrors* r, bool sawUnary, bool incDec, ForInit forInit) {
    DepthGuard guard(*this);
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr;
    if (isContextual("await") && canAwait()) {
        expr = parseAwait(forInit);
        sawUnary = true;
    } else if (type->prefix) {
        Node* node = startNode();
        bool update = type == &tt::incDec;
        node->op = InternOp(value);
        node->Set(kFlagPrefix, true);
        next();
        node->argument() = parseMaybeUnary(nullptr, true, update, forInit);
        checkExpressionErrors(r, true);
        if (update)
            checkLValSimple(node->argument());
        else if (strict && node->op == "delete" && IsLocalVariableAccess(node->argument()))
            raiseRecoverable(static_cast<int>(node->ustart), "Deleting local variable in strict mode");
        else if (node->op == "delete" && IsPrivateFieldAccess(node->argument()))
            raiseRecoverable(static_cast<int>(node->ustart), "Private fields can not be deleted");
        else
            sawUnary = true;
        expr = finishNode(node, update ? NodeType::UpdateExpression : NodeType::UnaryExpression);
    } else if (!sawUnary && type == &tt::privateId) {
        if ((forInit != ForInit::No || privateNameStack.empty()) && checkPrivateFields) unexpected();
        expr = parsePrivateIdent();
        if (type != &tt::_in) unexpected();
    } else {
        expr = parseExprSubscripts(r, forInit);
        if (checkExpressionErrors(r)) return expr;
        while (type->postfix && !canInsertSemicolon()) {
            Node* node = startNodeAt(startPos, sLoc);
            node->op = InternOp(value);
            node->Set(kFlagPrefix, false);
            node->argument() = expr;
            checkLValSimple(expr);
            next();
            expr = finishNode(node, NodeType::UpdateExpression);
        }
    }
    if (!incDec && eat(tt::starstar)) {
        if (sawUnary) unexpected(lastTokStart);
        return buildBinary(startPos, sLoc, expr, parseMaybeUnary(nullptr, false, false, forInit), "**", false);
    }
    return expr;
}

Node* Parser::parseExprSubscripts(DestructuringErrors* r, ForInit forInit) {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr = parseExprAtom(r, forInit);
    if (expr->type == NodeType::ArrowFunctionExpression && slice(lastTokStart, lastTokEnd) != u")") return expr;
    Node* result = parseSubscripts(expr, startPos, sLoc, false, forInit);
    if (r && result->type == NodeType::MemberExpression) {
        int rs = static_cast<int>(result->ustart);
        if (r->parenthesizedAssign >= rs) r->parenthesizedAssign = -1;
        if (r->parenthesizedBind >= rs) r->parenthesizedBind = -1;
        if (r->trailingComma >= rs) r->trailingComma = -1;
    }
    return result;
}

Node* Parser::parseSubscripts(Node* base, int startPos, Pos sLoc, bool noCalls, ForInit forInit) {
    bool maybeAsyncArrow = ecmaVersion >= 8 && base->type == NodeType::Identifier && base->str == "async" &&
                           lastTokEnd == static_cast<int>(base->uend) && !canInsertSemicolon() &&
                           base->uend - base->ustart == 5 && potentialArrowAt == static_cast<int>(base->ustart);
    bool optionalChained = false;
    for (;;) {
        Node* element = parseSubscript(base, startPos, sLoc, noCalls, maybeAsyncArrow, optionalChained, forInit);
        if ((element->type == NodeType::MemberExpression || element->type == NodeType::CallExpression) &&
            element->Has(kFlagOptional))
            optionalChained = true;
        if (element == base || element->type == NodeType::ArrowFunctionExpression) {
            if (optionalChained) {
                Node* chainNode = startNodeAt(startPos, sLoc);
                chainNode->expression() = element;
                element = finishNode(chainNode, NodeType::ChainExpression);
            }
            return element;
        }
        base = element;
    }
}

bool Parser::shouldParseAsyncArrow() { return !canInsertSemicolon() && eat(tt::arrow); }

Node* Parser::parseSubscriptAsyncArrow(int startPos, Pos sLoc, std::vector<Node*>& exprList, ForInit forInit) {
    return parseArrowExpression(startNodeAt(startPos, sLoc), exprList, true, forInit);
}

Node* Parser::parseSubscript(Node* base, int startPos, Pos sLoc, bool noCalls, bool maybeAsyncArrow,
                             bool optionalChained, ForInit forInit) {
    bool optionalSupported = ecmaVersion >= 11;
    bool optional = optionalSupported && eat(tt::questionDot);
    if (noCalls && optional) raise(lastTokStart, "Optional chaining cannot appear in the callee of new expressions");
    bool computed = eat(tt::bracketL);
    if (computed || (optional && type != &tt::parenL && type != &tt::backQuote) || eat(tt::dot)) {
        Node* node = startNodeAt(startPos, sLoc);
        node->object() = base;
        if (computed) {
            node->property() = parseExpression();
            expect(tt::bracketR);
        } else if (type == &tt::privateId && base->type != NodeType::Super) {
            node->property() = parsePrivateIdent();
        } else {
            node->property() = parseIdent(true);
        }
        node->Set(kFlagComputed, computed);
        if (optionalSupported) node->Set(kFlagOptional, optional);
        base = finishNode(node, NodeType::MemberExpression);
    } else if (!noCalls && eat(tt::parenL)) {
        DestructuringErrors refDE;
        int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
        yieldPos = 0;
        awaitPos = 0;
        awaitIdentPos = 0;
        std::vector<Node*> exprList = parseExprList(tt::parenR, ecmaVersion >= 8, false, &refDE);
        if (maybeAsyncArrow && !optional && shouldParseAsyncArrow()) {
            checkPatternErrors(&refDE, false);
            checkYieldAwaitInDefaultParams();
            if (awaitIdentPos > 0)
                raise(awaitIdentPos, "Cannot use 'await' as identifier inside an async function");
            yieldPos = oldYieldPos;
            awaitPos = oldAwaitPos;
            awaitIdentPos = oldAwaitIdentPos;
            return parseSubscriptAsyncArrow(startPos, sLoc, exprList, forInit);
        }
        checkExpressionErrors(&refDE, true);
        yieldPos = oldYieldPos ? oldYieldPos : yieldPos;
        awaitPos = oldAwaitPos ? oldAwaitPos : awaitPos;
        awaitIdentPos = oldAwaitIdentPos ? oldAwaitIdentPos : awaitIdentPos;
        Node* node = startNodeAt(startPos, sLoc);
        node->callee() = base;
        node->arguments() = list(exprList);
        if (optionalSupported) node->Set(kFlagOptional, optional);
        base = finishNode(node, NodeType::CallExpression);
    } else if (type == &tt::backQuote) {
        if (optional || optionalChained)
            raise(start, "Optional chaining cannot appear in the tag of tagged template expressions");
        Node* node = startNodeAt(startPos, sLoc);
        node->tag() = base;
        node->quasi() = parseTemplate(true);
        base = finishNode(node, NodeType::TaggedTemplateExpression);
    }
    return base;
}

Node* Parser::parseExprAtom(DestructuringErrors* r, ForInit forInit, bool forNew) {
    return parseExprAtomBase(r, forInit, forNew);
}

Node* Parser::parseExprAtomBase(DestructuringErrors* r, ForInit forInit, bool forNew) {
    DepthGuard guard(*this);
    if (type == &tt::slash) readRegexp();
    Node* node;
    bool canBeArrow = potentialArrowAt == start;
    if (type == &tt::_super) {
        if (!allowSuper()) raise(start, "'super' keyword outside a method");
        node = startNode();
        next();
        if (type == &tt::parenL && !allowDirectSuper())
            raise(static_cast<int>(node->ustart), "super() call outside constructor of a subclass");
        if (type != &tt::dot && type != &tt::bracketL && type != &tt::parenL) unexpected();
        return finishNode(node, NodeType::Super);
    }
    if (type == &tt::_this) {
        node = startNode();
        next();
        return finishNode(node, NodeType::ThisExpression);
    }
    if (type == &tt::name) {
        int startPos = start;
        Pos sLoc = startLoc;
        bool hadEsc = containsEsc;
        Node* id = parseIdent(false);
        if (ecmaVersion >= 8 && !hadEsc && id->str == "async" && !canInsertSemicolon() && eat(tt::_function)) {
            overrideContext(&tc::f_expr);
            return parseFunction(startNodeAt(startPos, sLoc), 0, false, true, forInit);
        }
        if (canBeArrow && !canInsertSemicolon()) {
            if (eat(tt::arrow)) {
                std::vector<Node*> params{id};
                return parseArrowExpression(startNodeAt(startPos, sLoc), params, false, forInit);
            }
            if (ecmaVersion >= 8 && id->str == "async" && type == &tt::name && !hadEsc &&
                (!potentialArrowInForAwait || value != "of" || containsEsc)) {
                id = parseIdent(false);
                if (canInsertSemicolon() || !eat(tt::arrow)) unexpected();
                std::vector<Node*> params{id};
                return parseArrowExpression(startNodeAt(startPos, sLoc), params, true, forInit);
            }
        }
        return id;
    }
    if (type == &tt::regexp || type == &tt::num || type == &tt::string) return parseLiteral();
    if (type == &tt::_null || type == &tt::_true || type == &tt::_false) {
        node = startNode();
        node->lit = type == &tt::_null ? LitKind::Null : LitKind::Bool;
        node->Set(kFlagBoolValue, type == &tt::_true);
        node->str = type->keyword;
        next();
        return finishNode(node, NodeType::Literal);
    }
    if (type == &tt::parenL) {
        int s = start;
        Node* expr = parseParenAndDistinguishExpression(canBeArrow, forInit);
        if (r) {
            if (r->parenthesizedAssign < 0 && !isSimpleAssignTarget(expr)) r->parenthesizedAssign = s;
            if (r->parenthesizedBind < 0) r->parenthesizedBind = s;
        }
        return expr;
    }
    if (type == &tt::bracketL) {
        node = startNode();
        next();
        node->elements() = list(parseExprList(tt::bracketR, true, true, r));
        return finishNode(node, NodeType::ArrayExpression);
    }
    if (type == &tt::braceL) {
        overrideContext(&tc::b_expr);
        return parseObj(false, r);
    }
    if (type == &tt::_function) {
        node = startNode();
        next();
        return parseFunction(node, 0);
    }
    if (type == &tt::_class) return parseClass(startNode(), 0);
    if (type == &tt::_new) return parseNew();
    if (type == &tt::backQuote) return parseTemplate();
    if (type == &tt::_import) {
        if (ecmaVersion >= 11) return parseExprImport(forNew);
        unexpected();
    }
    return parseExprAtomDefault();
}

Node* Parser::parseExprAtomDefault() { unexpected(); }

Node* Parser::parseExprImport(bool forNew) {
    Node* node = startNode();
    if (containsEsc) raiseRecoverable(start, "Escape sequence in keyword import");
    next();
    if (type == &tt::parenL && !forNew) return parseDynamicImport(node);
    if (type == &tt::dot) {
        Node* meta = startNodeAt(static_cast<int>(node->ustart), node->loc_start);
        meta->str = "import";
        node->meta() = finishNode(meta, NodeType::Identifier);
        return parseImportMeta(node);
    }
    unexpected();
}

Node* Parser::parseDynamicImport(Node* node) {
    next();
    node->source() = parseMaybeAssign();
    if (ecmaVersion >= 16) {
        node->Set(kFlagHasOptions, true);
        if (!eat(tt::parenR)) {
            expect(tt::comma);
            if (!afterTrailingComma(tt::parenR)) {
                node->options() = parseMaybeAssign();
                if (!eat(tt::parenR)) {
                    expect(tt::comma);
                    if (!afterTrailingComma(tt::parenR)) unexpected();
                }
            } else {
                node->options() = nullptr;
            }
        } else {
            node->options() = nullptr;
        }
    } else {
        if (!eat(tt::parenR)) {
            int errorPos = start;
            if (eat(tt::comma) && eat(tt::parenR))
                raiseRecoverable(errorPos, "Trailing comma is not allowed in import()");
            else
                unexpected(errorPos);
        }
    }
    return finishNode(node, NodeType::ImportExpression);
}

Node* Parser::parseImportMeta(Node* node) {
    next();
    bool hadEsc = containsEsc;
    node->property() = parseIdent(true);
    if (node->property()->str != "meta")
        raiseRecoverable(static_cast<int>(node->property()->ustart),
                         "The only valid meta property for import is 'import.meta'");
    if (hadEsc) raiseRecoverable(static_cast<int>(node->ustart), "'import.meta' must not contain escaped characters");
    if (sourceType != SourceType::Module && !allowImportExportEverywhere)
        raiseRecoverable(static_cast<int>(node->ustart), "Cannot use 'import.meta' outside a module");
    return finishNode(node, NodeType::MetaProperty);
}

Node* Parser::parseLiteral() {
    Node* node = startNode();
    if (type == &tt::num) {
        node->lit = numIsBigInt ? LitKind::BigInt : LitKind::Number;
        node->num = numValue;
        if (numIsBigInt) node->str2 = str(bigintValue);
    } else if (type == &tt::regexp) {
        node->lit = LitKind::RegExp;
        node->str2 = str(regexPattern);
        node->str3 = str(regexFlags);
    } else {
        node->lit = LitKind::String;
        node->str2 = str(value);
    }
    node->str = str(sliceStr(start, end));
    next();
    return finishNode(node, NodeType::Literal);
}

Node* Parser::parseParenExpression() {
    expect(tt::parenL);
    Node* val = parseExpression();
    expect(tt::parenR);
    return val;
}

bool Parser::shouldParseArrow(std::vector<Node*>&) { return !canInsertSemicolon(); }

Node* Parser::parseParenAndDistinguishExpression(bool canBeArrow, ForInit forInit) {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* val;
    bool allowTrailingComma = ecmaVersion >= 8;
    if (ecmaVersion >= 6) {
        next();
        int innerStartPos = start;
        Pos innerStartLoc = startLoc;
        std::vector<Node*> exprList;
        bool first = true, lastIsComma = false;
        DestructuringErrors refDE;
        int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, spreadStart = 0;
        yieldPos = 0;
        awaitPos = 0;
        while (type != &tt::parenR) {
            if (first)
                first = false;
            else
                expect(tt::comma);
            if (allowTrailingComma && afterTrailingComma(tt::parenR, true)) {
                lastIsComma = true;
                break;
            } else if (type == &tt::ellipsis) {
                spreadStart = start;
                exprList.push_back(parseParenItem(parseRestBinding()));
                if (type == &tt::comma) raiseRecoverable(start, "Comma is not permitted after the rest element");
                break;
            } else {
                exprList.push_back(parseMaybeAssign(ForInit::No, &refDE, true));
            }
        }
        int innerEndPos = lastTokEnd;
        Pos innerEndLoc = lastTokEndLoc;
        expect(tt::parenR);
        if (canBeArrow && shouldParseArrow(exprList) && eat(tt::arrow)) {
            checkPatternErrors(&refDE, false);
            checkYieldAwaitInDefaultParams();
            yieldPos = oldYieldPos;
            awaitPos = oldAwaitPos;
            return parseParenArrowList(startPos, sLoc, exprList, forInit);
        }
        if (exprList.empty() || lastIsComma) unexpected(lastTokStart);
        if (spreadStart) unexpected(spreadStart);
        checkExpressionErrors(&refDE, true);
        yieldPos = oldYieldPos ? oldYieldPos : yieldPos;
        awaitPos = oldAwaitPos ? oldAwaitPos : awaitPos;
        if (exprList.size() > 1) {
            val = startNodeAt(innerStartPos, innerStartLoc);
            val->expressions() = list(exprList);
            finishNodeAt(val, NodeType::SequenceExpression, innerEndPos, innerEndLoc);
        } else {
            val = exprList[0];
        }
    } else {
        val = parseParenExpression();
    }
    if (preserveParens) {
        Node* par = startNodeAt(startPos, sLoc);
        par->expression() = val;
        return finishNode(par, NodeType::ParenthesizedExpression);
    }
    return val;
}

Node* Parser::parseParenArrowList(int startPos, Pos sLoc, std::vector<Node*>& exprList, ForInit forInit) {
    return parseArrowExpression(startNodeAt(startPos, sLoc), exprList, false, forInit);
}

Node* Parser::parseNew() {
    if (containsEsc) raiseRecoverable(start, "Escape sequence in keyword new");
    Node* node = startNode();
    next();
    if (ecmaVersion >= 6 && type == &tt::dot) {
        Node* meta = startNodeAt(static_cast<int>(node->ustart), node->loc_start);
        meta->str = "new";
        node->meta() = finishNode(meta, NodeType::Identifier);
        next();
        bool hadEsc = containsEsc;
        node->property() = parseIdent(true);
        if (node->property()->str != "target")
            raiseRecoverable(static_cast<int>(node->property()->ustart),
                             "The only valid meta property for new is 'new.target'");
        if (hadEsc)
            raiseRecoverable(static_cast<int>(node->ustart), "'new.target' must not contain escaped characters");
        if (!allowNewDotTarget())
            raiseRecoverable(static_cast<int>(node->ustart),
                             "'new.target' can only be used in functions and class static block");
        return finishNode(node, NodeType::MetaProperty);
    }
    int startPos = start;
    Pos sLoc = startLoc;
    node->callee() = parseSubscripts(parseExprAtom(nullptr, ForInit::No, true), startPos, sLoc, true, ForInit::No);
    if (eat(tt::parenL))
        node->arguments() = list(parseExprList(tt::parenR, ecmaVersion >= 8, false));
    else
        node->arguments() = {};
    return finishNode(node, NodeType::NewExpression);
}

Node* Parser::parseTemplateElement(bool isTagged) {
    Node* elem = startNode();
    if (type == &tt::invalidTemplate) {
        if (!isTagged) raiseRecoverable(start, "Bad escape sequence in untagged template literal");
        elem->str = str(NormalizeTemplateRaw(value));
        elem->Set(kFlagHasCooked, false);
    } else {
        elem->str = str(NormalizeTemplateRaw(sliceStr(start, end)));
        elem->str2 = str(value);
        elem->Set(kFlagHasCooked, true);
    }
    next();
    elem->Set(kFlagTail, type == &tt::backQuote);
    return finishNode(elem, NodeType::TemplateElement);
}

Node* Parser::parseTemplate(bool isTagged) {
    Node* node = startNode();
    next();
    std::vector<Node*> expressions;
    Node* curElt = parseTemplateElement(isTagged);
    std::vector<Node*> quasis{curElt};
    while (!curElt->Has(kFlagTail)) {
        if (type == &tt::eof) raise(pos, "Unterminated template literal");
        expect(tt::dollarBraceL);
        expressions.push_back(parseExpression());
        expect(tt::braceR);
        quasis.push_back(curElt = parseTemplateElement(isTagged));
    }
    next();
    node->expressions() = list(expressions);
    node->quasis() = list(quasis);
    return finishNode(node, NodeType::TemplateLiteral);
}

bool Parser::isAsyncProp(Node* prop) {
    return !prop->Has(kFlagComputed) && prop->key()->type == NodeType::Identifier && prop->key()->str == "async" &&
           (type == &tt::name || type == &tt::num || type == &tt::string || type == &tt::bracketL || type->keyword ||
            (ecmaVersion >= 9 && type == &tt::star)) &&
           !lineBreakIn(lastTokEnd, start);
}

Node* Parser::parseObj(bool isPattern, DestructuringErrors* r) {
    Node* node = startNode();
    bool first = true;
    PropHash propHash;
    std::vector<Node*> props;
    next();
    while (!eat(tt::braceR)) {
        if (!first) {
            expect(tt::comma);
            if (ecmaVersion >= 5 && afterTrailingComma(tt::braceR)) break;
        } else {
            first = false;
        }
        Node* prop = parseProperty(isPattern, r);
        if (!isPattern) checkPropClash(prop, propHash, r);
        props.push_back(prop);
    }
    node->properties() = list(props);
    return finishNode(node, isPattern ? NodeType::ObjectPattern : NodeType::ObjectExpression);
}

Node* Parser::parseProperty(bool isPattern, DestructuringErrors* r) {
    Node* prop = startNode();
    bool isGenerator = false, isAsync = false;
    int startPos = 0;
    Pos sLoc;
    if (ecmaVersion >= 9 && eat(tt::ellipsis)) {
        if (isPattern) {
            prop->argument() = parseIdent(false);
            if (type == &tt::comma) raiseRecoverable(start, "Comma is not permitted after the rest element");
            return finishNode(prop, NodeType::RestElement);
        }
        prop->argument() = parseMaybeAssign(ForInit::No, r);
        if (type == &tt::comma && r && r->trailingComma < 0) r->trailingComma = start;
        return finishNode(prop, NodeType::SpreadElement);
    }
    if (ecmaVersion >= 6) {
        prop->Set(kFlagMethod, false);
        prop->Set(kFlagShorthand, false);
        if (isPattern || r) {
            startPos = start;
            sLoc = startLoc;
        }
        if (!isPattern) isGenerator = eat(tt::star);
    }
    bool hadEsc = containsEsc;
    parsePropertyName(prop);
    if (!isPattern && !hadEsc && ecmaVersion >= 8 && !isGenerator && isAsyncProp(prop)) {
        isAsync = true;
        isGenerator = ecmaVersion >= 9 && eat(tt::star);
        parsePropertyName(prop);
    } else {
        isAsync = false;
    }
    parsePropertyValue(prop, isPattern, isGenerator, isAsync, startPos, sLoc, r, hadEsc);
    return finishNode(prop, NodeType::Property);
}

void Parser::parseGetterSetter(Node* prop) {
    std::string_view kind = prop->key()->str == "get" ? "get" : "set";
    parsePropertyName(prop);
    prop->value() = parseMethod(false);
    prop->kind = kind;
    size_t paramCount = kind == "get" ? 0 : 1;
    Node* v = prop->value();
    if (v->params().size() != paramCount) {
        int s = static_cast<int>(v->ustart);
        if (kind == "get")
            raiseRecoverable(s, "getter should have no params");
        else
            raiseRecoverable(s, "setter should have exactly one param");
    } else if (kind == "set" && v->params()[0]->type == NodeType::RestElement) {
        raiseRecoverable(static_cast<int>(v->params()[0]->ustart), "Setter cannot use rest params");
    }
}

void Parser::parsePropertyValue(Node* prop, bool isPattern, bool isGenerator, bool isAsync, int startPos, Pos sLoc,
                                DestructuringErrors* r, bool hadEsc) {
    if ((isGenerator || isAsync) && type == &tt::colon) unexpected();
    if (eat(tt::colon)) {
        prop->value() = isPattern ? parseMaybeDefault(start, startLoc) : parseMaybeAssign(ForInit::No, r);
        prop->kind = "init";
    } else if (ecmaVersion >= 6 && type == &tt::parenL) {
        if (isPattern) unexpected();
        prop->Set(kFlagMethod, true);
        prop->value() = parseMethod(isGenerator, isAsync);
        prop->kind = "init";
    } else if (!isPattern && !hadEsc && ecmaVersion >= 5 && !prop->Has(kFlagComputed) &&
               prop->key()->type == NodeType::Identifier && (prop->key()->str == "get" || prop->key()->str == "set") &&
               (type != &tt::comma && type != &tt::braceR && type != &tt::eq)) {
        if (isGenerator || isAsync) unexpected();
        parseGetterSetter(prop);
    } else if (ecmaVersion >= 6 && !prop->Has(kFlagComputed) && prop->key()->type == NodeType::Identifier) {
        if (isGenerator || isAsync) unexpected();
        checkUnreserved(prop->key());
        if (prop->key()->str == "await" && !awaitIdentPos) awaitIdentPos = startPos;
        if (isPattern) {
            prop->value() = parseMaybeDefault(startPos, sLoc, copyNode(prop->key()));
        } else if (type == &tt::eq && r) {
            if (r->shorthandAssign < 0) r->shorthandAssign = start;
            prop->value() = parseMaybeDefault(startPos, sLoc, copyNode(prop->key()));
        } else {
            prop->value() = copyNode(prop->key());
        }
        prop->kind = "init";
        prop->Set(kFlagShorthand, true);
    } else {
        unexpected();
    }
}

Node* Parser::parsePropertyName(Node* prop) {
    if (ecmaVersion >= 6) {
        if (eat(tt::bracketL)) {
            prop->Set(kFlagComputed, true);
            prop->key() = parseMaybeAssign();
            expect(tt::bracketR);
            return prop->key();
        }
        prop->Set(kFlagComputed, false);
    }
    return prop->key() = type == &tt::num || type == &tt::string ? parseExprAtom() : parseIdent(true);
}

void Parser::initFunction(Node* node) {
    node->id() = nullptr;
    if (ecmaVersion >= 6) {
        node->Set(kFlagGenerator, false);
        node->Set(kFlagExpression, false);
    }
    if (ecmaVersion >= 8) node->Set(kFlagAsync, false);
}

Node* Parser::parseMethod(bool isGenerator, bool isAsync, bool allowDirectSuper) {
    Node* node = startNode();
    int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
    initFunction(node);
    if (ecmaVersion >= 6) node->Set(kFlagGenerator, isGenerator);
    if (ecmaVersion >= 8) node->Set(kFlagAsync, isAsync);
    yieldPos = 0;
    awaitPos = 0;
    awaitIdentPos = 0;
    enterScope(SCOPE_FUNCTION | (isAsync ? SCOPE_ASYNC : 0) | (node->Has(kFlagGenerator) ? SCOPE_GENERATOR : 0) |
               SCOPE_SUPER | (allowDirectSuper ? SCOPE_DIRECT_SUPER : 0));
    expect(tt::parenL);
    node->params() = list(parseBindingList(tt::parenR, false, ecmaVersion >= 8));
    checkYieldAwaitInDefaultParams();
    parseFunctionBody(node, false, true, ForInit::No);
    yieldPos = oldYieldPos;
    awaitPos = oldAwaitPos;
    awaitIdentPos = oldAwaitIdentPos;
    return finishNode(node, NodeType::FunctionExpression);
}

Node* Parser::parseArrowExpression(Node* node, std::vector<Node*>& params, bool isAsync, ForInit forInit) {
    int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
    enterScope(SCOPE_FUNCTION | (isAsync ? SCOPE_ASYNC : 0) | SCOPE_ARROW);
    initFunction(node);
    if (ecmaVersion >= 8) node->Set(kFlagAsync, isAsync);
    yieldPos = 0;
    awaitPos = 0;
    awaitIdentPos = 0;
    toAssignableList(params.data(), params.size(), true);
    node->params() = list(params);
    parseFunctionBody(node, true, false, forInit);
    yieldPos = oldYieldPos;
    awaitPos = oldAwaitPos;
    awaitIdentPos = oldAwaitIdentPos;
    return finishNode(node, NodeType::ArrowFunctionExpression);
}

void Parser::parseFunctionBody(Node* node, bool isArrowFunction, bool isMethod, ForInit forInit) {
    bool isExpression = isArrowFunction && type != &tt::braceL;
    bool oldStrict = strict, useStrict = false;
    if (isExpression) {
        node->body() = parseMaybeAssign(forInit);
        node->Set(kFlagExpression, true);
        checkParams(node, false);
    } else {
        bool nonSimple = ecmaVersion >= 7 && !isSimpleParamList(node->params());
        if (!oldStrict || nonSimple) {
            useStrict = !neverStrict && strictDirective(end);
            if (useStrict && nonSimple)
                raiseRecoverable(static_cast<int>(node->ustart),
                                 "Illegal 'use strict' directive in function with non-simple parameter list");
        }
        std::vector<Label> oldLabels = std::move(labels);
        labels.clear();
        if (useStrict) strict = true;
        checkParams(node, !oldStrict && !useStrict && !isArrowFunction && !isMethod && isSimpleParamList(node->params()));
        if (strict && node->id()) checkLValSimple(node->id(), BIND_OUTSIDE);
        node->body() = parseBlock(false, nullptr, useStrict && !oldStrict);
        node->Set(kFlagExpression, false);
        adaptDirectivePrologue(node->body()->body_list());
        labels = std::move(oldLabels);
    }
    exitScope();
}

bool Parser::isSimpleParamList(const NodeList& params) {
    for (Node* p : params)
        if (p->type != NodeType::Identifier) return false;
    return true;
}

void Parser::checkParams(Node* node, bool allowDuplicates) {
    std::vector<std::string_view> nameHash;
    for (Node* param : node->params()) checkLValInnerPattern(param, BIND_VAR, allowDuplicates ? nullptr : &nameHash);
}

std::vector<Node*> Parser::parseExprList(const TokenType& close, bool allowTrailingComma, bool allowEmpty,
                                         DestructuringErrors* r) {
    std::vector<Node*> elts;
    bool first = true;
    while (!eat(close)) {
        if (!first) {
            expect(tt::comma);
            if (allowTrailingComma && afterTrailingComma(close)) break;
        } else {
            first = false;
        }
        Node* elt;
        if (allowEmpty && type == &tt::comma) {
            elt = nullptr;
        } else if (type == &tt::ellipsis) {
            elt = parseSpread(r);
            if (r && type == &tt::comma && r->trailingComma < 0) r->trailingComma = start;
        } else {
            elt = parseMaybeAssign(ForInit::No, r);
        }
        elts.push_back(elt);
    }
    return elts;
}

void Parser::checkUnreserved(Node* ref) {
    int s = static_cast<int>(ref->ustart), e = static_cast<int>(ref->uend);
    std::string_view name = ref->str;
    if (inGenerator() && name == "yield") raiseRecoverable(s, "Cannot use 'yield' as identifier inside a generator");
    if (inAsync() && name == "await") raiseRecoverable(s, "Cannot use 'await' as identifier inside an async function");
    if (!(currentThisScope().flags & SCOPE_VAR) && name == "arguments")
        raiseRecoverable(s, "Cannot use 'arguments' in class field initializer");
    if (inClassStaticBlock() && (name == "arguments" || name == "await"))
        raise(s, "Cannot use " + std::string(name) + " in class static initialization block");
    if (isKeywordWord(name)) raise(s, "Unexpected keyword '" + std::string(name) + "'");
    if (ecmaVersion < 6 && slice(s, e).find(u'\\') != std::u16string_view::npos) return;
    if (inList(strict ? reservedWordsStrict : reservedWords, name)) {
        if (!inAsync() && name == "await") raiseRecoverable(s, "Cannot use keyword 'await' outside an async function");
        raiseRecoverable(s, "The keyword '" + std::string(name) + "' is reserved");
    }
}

Node* Parser::parseIdent(bool liberal) {
    Node* node = parseIdentNode();
    next(liberal);
    finishNode(node, NodeType::Identifier);
    if (!liberal) {
        checkUnreserved(node);
        if (node->str == "await" && !awaitIdentPos) awaitIdentPos = static_cast<int>(node->ustart);
    }
    return node;
}

Node* Parser::parseIdentNode() {
    Node* node = startNode();
    if (type == &tt::name) {
        node->str = str(value);
    } else if (type->keyword) {
        node->str = type->keyword;
        if ((node->str == "class" || node->str == "function") &&
            (lastTokEnd != lastTokStart + 1 || charAt(lastTokStart) != 46)) {
            if (!context.empty()) context.pop_back();
        }
        type = &tt::name;
    } else {
        unexpected();
    }
    return node;
}

Node* Parser::parsePrivateIdent() {
    Node* node = startNode();
    if (type == &tt::privateId)
        node->str = str(value);
    else
        unexpected();
    next();
    finishNode(node, NodeType::PrivateIdentifier);
    if (checkPrivateFields) {
        if (privateNameStack.empty())
            raise(static_cast<int>(node->ustart),
                  "Private field '#" + std::string(node->str) + "' must be declared in an enclosing class");
        privateNameStack.back().used.push_back(node);
    }
    return node;
}

Node* Parser::parseYield(ForInit forInit) {
    if (!yieldPos) yieldPos = start;
    Node* node = startNode();
    next();
    if (type == &tt::semi || canInsertSemicolon() || (type != &tt::star && !type->startsExpr)) {
        node->Set(kFlagDelegate, false);
        node->argument() = nullptr;
    } else {
        node->Set(kFlagDelegate, eat(tt::star));
        node->argument() = parseMaybeAssign(forInit);
    }
    return finishNode(node, NodeType::YieldExpression);
}

Node* Parser::parseAwait(ForInit forInit) {
    if (!awaitPos) awaitPos = start;
    Node* node = startNode();
    next();
    node->argument() = parseMaybeUnary(nullptr, true, false, forInit);
    return finishNode(node, NodeType::AwaitExpression);
}

}  // namespace jsparse
