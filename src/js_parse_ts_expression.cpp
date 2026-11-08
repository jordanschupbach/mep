// TypeScript dialect (see js_parse_ts.h): expressions -- arrow functions
// and their ambiguities, `as` / `satisfies`, `x!`, `<T>x`, type arguments
// on calls, `new` and tagged templates, instantiation expressions,
// parameters (modifiers, `this`, `?`, annotations) and bodyless methods
// (tsc's parser.ts, "EXPRESSIONS").

#include <algorithm>
#include <cstring>

#include "js_parse_ts.h"

namespace jsparse {

namespace {

std::string_view InternTsOp(const std::string& op) {
    static const char* const kOps[] = {
        "=",   "+=",  "-=",  "*=",   "/=",  "%=",  "**=", "<<=", ">>=", ">>>=", "|=", "^=", "&=", "||=", "&&=",
        "?\?=", "||",  "&&",  "??",   "|",   "^",   "&",   "==",  "!=",  "===",  "!==", "<", ">", "<=",  ">=",
        "<<",  ">>",  ">>>", "+",    "-",   "*",   "/",   "%",   "**",  "in",   "instanceof", "!", "~", "typeof",
        "void", "delete", "++", "--"};
    for (const char* o : kOps)
        if (op == o) return o;
    return {};
}

// tsc's isLeftHandSideExpression, on the ESTree node (`parenthesized`: the
// expression had parentheses around it, which tsc keeps as a node).
bool IsLeftHandSide(const Node* n, bool parenthesized) {
    if (parenthesized) return true;
    switch (n->type) {
    case NodeType::MemberExpression:
    case NodeType::CallExpression:
    case NodeType::NewExpression:
    case NodeType::ChainExpression:
    case NodeType::JSXElement:
    case NodeType::JSXFragment:
    case NodeType::TaggedTemplateExpression:
    case NodeType::ArrayExpression:
    case NodeType::ObjectExpression:
    case NodeType::ArrayPattern:
    case NodeType::ObjectPattern:
    case NodeType::ClassExpression:
    case NodeType::FunctionExpression:
    case NodeType::Identifier:
    case NodeType::PrivateIdentifier:
    case NodeType::Literal:
    case NodeType::TemplateLiteral:
    case NodeType::ThisExpression:
    case NodeType::Super:
    case NodeType::TSNonNullExpression:
    case NodeType::TSInstantiationExpression:
    case NodeType::MetaProperty:
    case NodeType::ImportExpression:
    case NodeType::ParenthesizedExpression:
        return true;
    default:
        return false;
    }
}

}  // namespace

// ---------------------------------------------------------------- assignment / arrows

Node* TsParser::parseMaybeAssign(ForInit forInit, DestructuringErrors* r, bool afterLeftParseParenItem) {
    DepthGuard guard(*this);
    bool allowReturnType = !pendingNoArrowReturnType;
    pendingNoArrowReturnType = false;
    struct Restore {
        TsParser& p;
        bool allow, decorator;
        ~Restore() {
            p.curAllowArrowReturnType = allow;
            p.inDecorator = decorator;
        }
    } restore{*this, curAllowArrowReturnType, inDecorator};
    curAllowArrowReturnType = allowReturnType;
    inDecorator = false;

    if (isWord("yield")) {
        if (inGenerator()) return parseYield(forInit);
        // tsc: `yield x` outside a generator is still a yield expression
        // (the checker reports it).
        bool looksLikeYield = lookAhead([&] {
            next();
            return (isIdentOrKeywordToken() || type == &tt::num || type == &tt::string) && !hasPrecedingLineBreak();
        });
        if (looksLikeYield) return parseYield(forInit);
        exprAllowed = false;
    }
    if (type == &tt::parenL || isLt() || isWord("async")) {
        Node* arrow = tsTryParseArrow(forInit, allowReturnType);
        if (arrow) return arrow;
    }
    int startPos = start;
    Pos sLoc = startLoc;
    if (type == &tt::parenL || type == &tt::name) {
        potentialArrowAt = start;
        potentialArrowInForAwait = forInit == ForInit::Await;
    }
    DestructuringErrors own;
    if (!r) r = &own;
    Node* left = parseMaybeConditional(forInit, r);
    if (afterLeftParseParenItem) left = parseParenItem(left);
    if (type->isAssign) {
        if (!IsLeftHandSide(left, static_cast<int>(left->ustart) > startPos)) return left;
        Node* node = startNodeAt(startPos, sLoc);
        node->op = InternTsOp(value);
        if (type == &tt::eq) left = toAssignable(left, false, r);
        r->shorthandAssign = -1;
        node->left() = left;
        next();
        pendingNoArrowReturnType = !allowReturnType;
        node->right() = parseMaybeAssign(forInit);
        return finishNode(node, NodeType::AssignmentExpression);
    }
    return left;
}

// parseYieldExpression: an argument follows when the next token on the same
// line can start an expression (`yield <T>x`).
Node* TsParser::parseYield(ForInit forInit) {
    if (!yieldPos) yieldPos = start;
    Node* node = startNode();
    next();
    if (!hasPrecedingLineBreak() && (type == &tt::star || tsIsStartOfExpression())) {
        node->Set(kFlagDelegate, eat(tt::star));
        node->argument() = parseMaybeAssign(forInit);
    } else {
        node->Set(kFlagDelegate, false);
        node->argument() = nullptr;
    }
    return finishNode(node, NodeType::YieldExpression);
}

Node* TsParser::parseMaybeConditional(ForInit forInit, DestructuringErrors* r) {
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr = parseExprOps(forInit, r);
    if (checkExpressionErrors(r)) return expr;
    if (eat(tt::question)) {
        Node* node = startNodeAt(startPos, sLoc);
        node->test() = expr;
        // tsc: no return type on an arrow function in the true branch
        // unless it is unambiguous (`a ? (x): T => x : y`).
        pendingNoArrowReturnType = true;
        node->consequent() = parseMaybeAssign();
        expect(tt::colon);
        pendingNoArrowReturnType = !curAllowArrowReturnType;
        node->alternate() = parseMaybeAssign(forInit);
        return finishNode(node, NodeType::ConditionalExpression);
    }
    return expr;
}

// tryParseParenthesizedArrowFunctionExpression.
Node* TsParser::tsTryParseArrow(ForInit forInit, bool allowReturnType) {
    int at = start;
    if (std::find(tsNotArrowAt.begin(), tsNotArrowAt.end(), at) != tsNotArrowAt.end()) return nullptr;
    int tri = tsIsParenthesizedArrow();
    if (tri == 0) return nullptr;
    bool gaveUp = false, committed = false;
    if (tri == 1) return tsParseParenArrow(true, true, forInit, gaveUp, committed);
    TsSave s = tsSave();
    Node* n = nullptr;
    try {
        n = tsParseParenArrow(false, allowReturnType, forInit, gaveUp, committed);
    } catch (const SyntaxErr&) {
        if (committed) throw;
        n = nullptr;
    }
    if (!n || gaveUp) {
        tsRestore(s);
        tsNotArrowAt.push_back(at);
        return nullptr;
    }
    return n;
}

// isParenthesizedArrowFunctionExpression (a lookahead).
int TsParser::tsIsParenthesizedArrow() {
    TsSave s = tsSave();
    int result = 0;
    try {
        result = [&]() -> int {
            if (isWord("async")) {
                next();
                if (hasPrecedingLineBreak()) return 0;
                if (type != &tt::parenL && !isLt()) return 0;
            }
            bool paren = type == &tt::parenL;
            if (!paren) reReadLtAsRelational();
            next();
            if (paren) {
                if (type == &tt::parenR) {
                    next();
                    return type == &tt::arrow || type == &tt::colon || type == &tt::braceL ? 1 : 0;
                }
                if (type == &tt::bracketL || type == &tt::braceL) return 2;
                if (type == &tt::ellipsis) return 1;
                if (isModifierToken() && !isWord("async")) {
                    bool identNext = lookAhead([&] {
                        next();
                        return isIdentToken();
                    });
                    if (identNext) {
                        next();
                        return isWord("as") ? 0 : 1;
                    }
                }
                if (!isIdentToken() && type != &tt::_this) return 0;
                next();
                if (type == &tt::colon) return 1;
                if (type == &tt::question) {
                    next();
                    return type == &tt::colon || type == &tt::comma || type == &tt::eq || type == &tt::parenR ? 1 : 0;
                }
                if (type == &tt::comma || type == &tt::eq || type == &tt::parenR) return 2;
                return 0;
            }
            if (!isIdentToken() && type != &tt::_const) return 0;
            if (!tsx) return 2;
            // JSX: `<T,>` / `<T extends U>` / `<T = U>` start an arrow
            // function, anything else is an element.
            if (type == &tt::_const) next();
            next();
            if (type == &tt::_extends) {
                next();
                if (type == &tt::eq || (type == &tt::relational && value == ">") || type == &tt::jsxTagEnd ||
                    type == &tt::slash || type == &tt::regexp || isGtStart())
                    return 0;
                return 1;
            }
            return type == &tt::comma || type == &tt::eq ? 1 : 0;
        }();
    } catch (const SyntaxErr&) {
        result = 0;
    }
    tsRestore(s);
    return result;
}

void TsParser::tsBlockContext() {
    if (type == &tt::braceL && !context.empty() && context.back() == &tc::b_expr) context.back() = &tc::b_stat;
}

Node* TsParser::tsParseParenArrow(bool allowAmbiguity, bool allowReturnType, ForInit forInit, bool& gaveUp,
                                  bool& committed) {
    Node* node = startNode();
    bool isAsync = false;
    if (isWord("async")) {
        next();
        isAsync = true;
    }
    initFunction(node);
    node->Set(kFlagAsync, isAsync);
    int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
    yieldPos = 0;
    awaitPos = 0;
    awaitIdentPos = 0;
    enterScope(SCOPE_FUNCTION | SCOPE_ARROW | (isAsync ? SCOPE_ASYNC : 0));
    node->typeParameters() = tsTryParseTypeParameters();
    if (type != &tt::parenL) {
        if (!allowAmbiguity) {
            gaveUp = true;
            return nullptr;
        }
        unexpected();
    }
    next();
    std::vector<Node*> params = tsParseParamList(tt::parenR, !allowAmbiguity, gaveUp);
    if (gaveUp) return nullptr;
    node->params() = list(params);
    bool hasReturnColon = type == &tt::colon;
    node->returnType() = tsParseReturnType(false, false);
    if (!allowAmbiguity && type != &tt::arrow && type != &tt::braceL) {
        gaveUp = true;
        return nullptr;
    }
    if (allowReturnType || !hasReturnColon) committed = true;
    if (type == &tt::braceL)
        Parser::raiseRecoverable(start, "Unexpected token");  // tsc: '=>' expected
    else
        expect(tt::arrow);
    if (type == &tt::braceL) {
        tsBlockContext();
    } else {
        pendingNoArrowReturnType = !allowReturnType;
    }
    parseFunctionBody(node, true, false, forInit);
    pendingNoArrowReturnType = false;
    yieldPos = oldYieldPos;
    awaitPos = oldAwaitPos;
    awaitIdentPos = oldAwaitIdentPos;
    if (!allowReturnType && hasReturnColon && type != &tt::colon) {
        gaveUp = true;
        return nullptr;
    }
    return finishNode(node, NodeType::ArrowFunctionExpression);
}

bool TsParser::shouldParseArrow(std::vector<Node*>&) { return false; }
bool TsParser::shouldParseAsyncArrow() { return false; }

// ---------------------------------------------------------------- parameters

std::vector<Node*> TsParser::parseBindingList(const TokenType& close, bool allowEmpty, bool allowTrailingComma,
                                              bool allowModifiers) {
    if (allowModifiers) {
        bool gaveUp = false;
        return tsParseParamList(close, false, gaveUp);
    }
    // An array binding pattern (parseArrayBindingElement): a rest element
    // anywhere, with an initializer, a trailing comma after it -- all the
    // checker's to report.
    (void)allowTrailingComma;
    std::vector<Node*> elts;
    bool first = true;
    while (!eat(close)) {
        if (!first) {
            expect(tt::comma);
            if (eat(close)) break;
        }
        first = false;
        if (allowEmpty && type == &tt::comma) {
            elts.push_back(nullptr);
            continue;
        }
        if (type == &tt::ellipsis) {
            Node* rest = startNode();
            next();
            int s = start;
            Pos sl = startLoc;
            Node* arg = parseBindingAtom();
            if (type == &tt::eq) arg = parseMaybeDefault(s, sl, arg);
            rest->argument() = arg;
            elts.push_back(finishNode(rest, NodeType::RestElement));
            continue;
        }
        elts.push_back(parseMaybeDefault(start, startLoc));
    }
    return elts;
}

std::vector<Node*> TsParser::tsParseParamList(const TokenType& close, bool speculative, bool& gaveUp) {
    std::vector<Node*> elts;
    bool first = true;
    while (!eat(close)) {
        if (!first) {
            expect(tt::comma);
            if (eat(close)) break;
        }
        first = false;
        Node* p = tsParseParam(speculative, gaveUp);
        if (gaveUp) return {};
        elts.push_back(p);
    }
    return elts;
}

// parseParameterWorker.
Node* TsParser::tsParseParam(bool speculative, bool& gaveUp) {
    DepthGuard guard(*this);
    int s = start;
    Pos sl = startLoc;
    Mods m;
    tsParseModifiers(m, true);
    if (type == &tt::_this) {
        if (m.start >= 0) raiseRecoverable(s, "Neither decorators nor modifiers may be applied to 'this' parameters");
        Node* id = startNode();
        id->str = "this";
        next();
        finishNode(id, NodeType::Identifier);
        if (type == &tt::colon) {
            id->typeAnnotation() = tsParseTypeAnnotation();
            extendTo(id);
        }
        return id;
    }
    Node* rest = nullptr;
    if (type == &tt::ellipsis) {
        // typescript-estree: a rest parameter starts at its decorators/modifiers
        rest = m.start < 0 ? startNode() : startNodeAt(s, sl);
        next();
    }
    if (speculative && !((type == &tt::name && value != "enum") || type == &tt::bracketL || type == &tt::braceL)) {
        gaveUp = true;
        return nullptr;
    }
    Node* target = parseBindingAtom();
    Node* node = target;
    bool optional = eat(tt::question);
    Node* ann = tsTryParseTypeAnnotation();
    if (rest) {
        rest->argument() = target;
        rest->typeAnnotation() = ann;
        if (optional) rest->SetTs(kTsOptional, true);
        node = finishNode(rest, NodeType::RestElement);
    } else {
        if (optional) target->SetTs(kTsOptional, true);
        if (ann) target->typeAnnotation() = ann;
        if (optional || ann) extendTo(target);
    }
    if (type == &tt::eq) {
        next();
        Node* ap = startNodeAt(static_cast<int>(node->ustart), node->loc_start);
        ap->left() = node;
        ap->right() = parseMaybeAssign();
        node = finishNode(ap, NodeType::AssignmentPattern);
    }
    if (!m.access.empty() || m.readonly_ || m.override_ || m.static_ || m.exp) {
        Node* pp = startNodeAt(s, sl);
        pp->parameter() = node;
        pp->accessibility = m.access;
        if (m.readonly_) pp->SetTs(kTsReadonly, true);
        if (m.override_) pp->SetTs(kTsOverride, true);
        if (m.static_) pp->SetTs(kTsStatic, true);
        pp->decorators() = list(m.decorators);
        return finishNode(pp, NodeType::TSParameterProperty);
    }
    if (!m.decorators.empty()) node->decorators() = list(m.decorators);
    return node;
}

Node* TsParser::parseAssignableListItem(bool allowModifiers) { return Parser::parseAssignableListItem(allowModifiers); }
Node* TsParser::parseBindingListItem(Node* param) { return param; }
Node* TsParser::parseBindingAtom() { return Parser::parseBindingAtom(); }

// tsc doesn't check assignment targets in the parser; only the shape
// changes (ESTree's patterns).
Node* TsParser::toAssignable(Node* node, bool isBinding, DestructuringErrors*) {
    if (!node) return node;
    switch (node->type) {
    case NodeType::ObjectExpression:
        node->type = NodeType::ObjectPattern;
        for (Node* prop : node->properties()) toAssignable(prop, isBinding);
        break;
    case NodeType::Property:
        if (node->kind == "init" && !node->Has(kFlagMethod)) toAssignable(node->value(), isBinding);
        break;
    case NodeType::ArrayExpression:
        node->type = NodeType::ArrayPattern;
        toAssignableList(node->elements().items, node->elements().size(), isBinding);
        break;
    case NodeType::SpreadElement:
        node->type = NodeType::RestElement;
        toAssignable(node->argument(), isBinding);
        break;
    case NodeType::AssignmentExpression:
        if (node->op == "=") {
            node->type = NodeType::AssignmentPattern;
            node->op = {};
            toAssignable(node->left(), isBinding);
        }
        break;
    case NodeType::ParenthesizedExpression:
        toAssignable(node->expression(), isBinding);
        break;
    default:
        break;
    }
    return node;
}

void TsParser::toAssignableList(Node** items, size_t n, bool isBinding) {
    for (size_t i = 0; i < n; ++i)
        if (items[i]) toAssignable(items[i], isBinding);
}

Node* TsParser::parseForIn(Node* node, Node* init) {
    // tsc leaves `for (var x = 1 in o)` to the checker.
    bool isForIn = type == &tt::_in;
    next();
    node->left() = init;
    node->right() = isForIn ? parseExpression() : parseMaybeAssign();
    expect(tt::parenR);
    node->body() = parseStatement("for");
    exitScope();
    labels.pop_back();
    return finishNode(node, isForIn ? NodeType::ForInStatement : NodeType::ForOfStatement);
}

// ---------------------------------------------------------------- binary / unary

bool TsParser::tsIsBinaryOperator() {
    return (type->binop >= 0 && type != &tt::jsxTagStart) || isWord("as") || isWord("satisfies");
}

bool TsParser::tsIsStartOfExpression() {
    if (type->startsExpr) return true;
    if (type == &tt::relational && value == "<") return true;
    if (type == &tt::at || type == &tt::slash || type == &tt::regexp || (type == &tt::assign && value == "/="))
        return true;
    return tsIsBinaryOperator();
}

Node* TsParser::parseExprOp(Node* left, int leftStartPos, Pos leftStartLoc, int minPrec, ForInit forInit) {
    for (;;) {
        if ((isWord("as") || isWord("satisfies")) && !hasPrecedingLineBreak()) {
            int prec = tt::relational.binop;
            if (prec <= minPrec) return left;
            bool isAs = isWord("as");
            next();
            Node* node = startNodeAt(leftStartPos, leftStartLoc);
            node->expression() = left;
            node->typeAnnotation() = tsParseTypeInContext(false);
            left = finishNode(node, isAs ? NodeType::TSAsExpression : NodeType::TSSatisfiesExpression);
            continue;
        }
        int prec = type->binop;
        if (prec < 0 || type == &tt::jsxTagStart || (forInit != ForInit::No && type == &tt::_in) || prec <= minPrec)
            return left;
        bool logical = type == &tt::logicalOR || type == &tt::logicalAND;
        bool coalesce = type == &tt::coalesce;
        if (coalesce) prec = tt::logicalAND.binop;
        bool rightAssoc = type == &tt::starstar;
        std::string op = value;
        next();
        int startPos = start;
        Pos sLoc = startLoc;
        Node* right = parseExprOp(parseMaybeUnary(nullptr, false, false, forInit), startPos, sLoc,
                                  rightAssoc ? prec - 1 : prec, forInit);
        left = buildBinary(leftStartPos, leftStartLoc, left, right, InternTsOp(op), logical || coalesce);
    }
}

// tsc's `<T>x` (TypeAssertion; .ts only).
Node* TsParser::tsParseTypeAssertion(ForInit forInit) {
    Node* node = startNode();
    next();  // <
    node->typeAnnotation() = tsParseTypeInContext(false);
    expectGt();
    node->expression() = parseMaybeUnary(nullptr, false, true, forInit);
    return finishNode(node, NodeType::TSTypeAssertion);
}

Node* TsParser::parseMaybeUnary(DestructuringErrors* r, bool sawUnary, bool incDec, ForInit forInit) {
    DepthGuard guard(*this);
    int startPos = start;
    Pos sLoc = startLoc;
    Node* expr;
    bool unary = false;
    auto awaitHere = [&]() {
        if (!isWord("await")) return false;
        if (tsInAwaitContext()) return true;
        // tsc: `await x` outside an async function is still an await
        // expression (the checker reports it).
        return lookAhead([&] {
            next();
            return (isIdentOrKeywordToken() || type == &tt::num || type == &tt::string) && !hasPrecedingLineBreak();
        });
    };
    if (!tsx && type == &tt::relational && value == "<") {
        expr = tsParseTypeAssertion(forInit);
        unary = true;
    } else if (awaitHere()) {
        if (!awaitPos) awaitPos = start;
        Node* node = startNode();
        next();
        node->argument() = parseMaybeUnary(nullptr, true, true, forInit);
        expr = finishNode(node, NodeType::AwaitExpression);
        unary = true;
    } else if (type->prefix) {
        Node* node = startNode();
        bool update = type == &tt::incDec;
        node->op = InternTsOp(value);
        node->Set(kFlagPrefix, true);
        next();
        // tsc parseUpdateExpression: `++` / `--` take a left-hand-side
        // expression (`++ ++x`, `++x++` are errors)
        node->argument() = update ? parseExprSubscripts(nullptr, forInit) : parseMaybeUnary(nullptr, true, true, forInit);
        expr = finishNode(node, update ? NodeType::UpdateExpression : NodeType::UnaryExpression);
        unary = !update;
    } else {
        expr = parseExprSubscripts(r, forInit);
        if (checkExpressionErrors(r)) return expr;
        if (type->postfix && !canInsertSemicolon()) {
            Node* node = startNodeAt(startPos, sLoc);
            node->op = InternTsOp(value);
            node->Set(kFlagPrefix, false);
            node->argument() = expr;
            next();
            expr = finishNode(node, NodeType::UpdateExpression);
        }
    }
    (void)sawUnary;
    if (!incDec && type == &tt::starstar) {
        // tsc: "An unary expression with the '-' operator is not allowed in
        // the left-hand side of an exponentiation expression."
        if (unary) Parser::raiseRecoverable(startPos, "Unexpected token");
        next();
        return buildBinary(startPos, sLoc, expr, parseMaybeUnary(nullptr, false, false, forInit), "**", false);
    }
    return expr;
}

// ---------------------------------------------------------------- member / call

// parseTypeArgumentsInExpression (run under tryParse).
Node* TsParser::tsParseTypeArgumentsInExpression() {
    if (!isLt()) return nullptr;
    Node* node = startNode();
    reReadLtAsRelational();
    next();
    std::vector<Node*> params;
    bool savedType = inType;
    inType = true;
    bool first = true;
    while (!isGtStart()) {
        if (!first) {
            expect(tt::comma);
            if (isGtStart()) break;
        }
        first = false;
        params.push_back(tsParseTypeInContext(false));
    }
    inType = savedType;
    if (!(type == &tt::relational && value == ">")) return nullptr;
    node->params() = list(params);
    // The token after `>` is read as after an expression.
    exprAllowed = false;
    next();
    finishNodeAt(node, NodeType::TSTypeParameterInstantiation, lastTokEnd, lastTokEndLoc);
    return tsCanFollowTypeArgumentsInExpression() ? node : nullptr;
}

bool TsParser::tsCanFollowTypeArgumentsInExpression() {
    if (type == &tt::parenL || type == &tt::backQuote) return true;
    if ((type == &tt::relational && (value == "<" || value == ">")) || type == &tt::jsxTagStart ||
        type == &tt::plusMin)
        return false;
    return hasPrecedingLineBreak() || tsIsBinaryOperator() || !tsIsStartOfExpression();
}

Node* TsParser::parseSubscript(Node* base, int startPos, Pos sLoc, bool noCalls, bool maybeAsyncArrow,
                               bool optionalChained, ForInit forInit) {
    // x!
    if (type == &tt::prefix && value == "!" && !hasPrecedingLineBreak()) {
        exprAllowed = false;
        next();
        Node* node = startNodeAt(startPos, sLoc);
        node->expression() = base;
        return finishNode(node, NodeType::TSNonNullExpression);
    }
    // a?.`x` and a?.b`x`: tsc parses them (the checker rejects them)
    if (type == &tt::backQuote && optionalChained) {
        Node* node = startNodeAt(startPos, sLoc);
        node->tag() = base;
        node->quasi() = parseTemplate(true);
        return finishNode(node, NodeType::TaggedTemplateExpression);
    }
    if (type == &tt::questionDot && peekIs(tt::backQuote)) {
        next();
        Node* node = startNodeAt(startPos, sLoc);
        node->tag() = base;
        node->quasi() = parseTemplate(true);
        return finishNode(node, NodeType::TaggedTemplateExpression);
    }
    // a?.<T>(x)
    if (type == &tt::questionDot) {
        bool ltNext = lookAhead([&] {
            next();
            return isLt();
        });
        if (ltNext) {
            if (noCalls) raise(start, "Optional chaining cannot appear in the callee of new expressions");
            next();
            Node* targs = tryParse([&] { return tsParseTypeArgumentsInExpression(); });
            if (type == &tt::backQuote) {
                Node* node = startNodeAt(startPos, sLoc);
                node->tag() = base;
                node->typeArguments() = targs;
                node->quasi() = parseTemplate(true);
                return finishNode(node, NodeType::TaggedTemplateExpression);
            }
            if (!targs && type != &tt::parenL) unexpected();
            expect(tt::parenL);
            Node* node = startNodeAt(startPos, sLoc);
            node->callee() = base;
            node->arguments() = list(parseExprList(tt::parenR, true, false));
            node->typeArguments() = targs;
            node->Set(kFlagOptional, true);
            return finishNode(node, NodeType::CallExpression);
        }
    }
    // f<T>(x), f<T>`...`, f<T>
    if (isLt()) {
        Node* targs = tryParse([&] { return tsParseTypeArgumentsInExpression(); });
        if (targs) {
            if (!noCalls && type == &tt::parenL) {
                next();
                Node* node = startNodeAt(startPos, sLoc);
                node->callee() = base;
                node->arguments() = list(parseExprList(tt::parenR, true, false));
                node->typeArguments() = targs;
                node->Set(kFlagOptional, false);
                return finishNode(node, NodeType::CallExpression);
            }
            if (type == &tt::backQuote) {
                Node* node = startNodeAt(startPos, sLoc);
                node->tag() = base;
                node->typeArguments() = targs;
                node->quasi() = parseTemplate(true);
                return finishNode(node, NodeType::TaggedTemplateExpression);
            }
            Node* node = startNodeAt(startPos, sLoc);
            node->expression() = base;
            node->typeArguments() = targs;
            return finishNode(node, NodeType::TSInstantiationExpression);
        }
    }
    // (a parenthesized one, `(f<T>).x`, is fine)
    if (base->type == NodeType::TSInstantiationExpression && type == &tt::dot &&
        static_cast<int>(base->uend) == lastTokEnd)
        Parser::raiseRecoverable(static_cast<int>(base->typeArguments()->ustart),
                                 "An instantiation expression cannot be followed by a property access");
    if (type == &tt::questionDot) {
        int privAt = -1;
        lookAhead([&] {
            next();
            if (type == &tt::privateId) privAt = start;
            return true;
        });
        if (privAt >= 0) raiseRecoverable(privAt, "An optional chain cannot contain private identifiers");
    }
    // `@a[b]` in a decorator: the `[` starts a computed member name
    if (inDecorator && type == &tt::bracketL) return base;
    return Parser::parseSubscript(base, startPos, sLoc, noCalls, maybeAsyncArrow, optionalChained, forInit);
}

Node* TsParser::parseExprAtom(DestructuringErrors* r, ForInit forInit, bool forNew) {
    if (tsx && type == &tt::relational && value == "<") {
        // tsc rescans: a `<` read as an operator (after `x\n` ended the
        // previous statement by ASI) starts a JSX element here.
        pos = start;
        exprAllowed = true;
        nextToken();
    }
    if (type == &tt::at) return tsParseDecoratedClassExpression();
    if (type == &tt::privateId) return parsePrivateIdent();  // `#x in o`, and anywhere for the checker
    if (type == &tt::_super) {
        // tsc: `super` must be followed by `(`, `.` or `[` (where it may be
        // is the checker's business)
        Node* node = startNode();
        next();
        if (type != &tt::dot && type != &tt::bracketL && type != &tt::parenL && !isLt()) unexpected();
        if (isLt()) raiseRecoverable(start, "'super' may not use type arguments");
        return finishNode(node, NodeType::Super);
    }
    if (isWord("async") && !containsEsc && lookAhead([&] {
            next();
            if (type != &tt::name || hasPrecedingLineBreak()) return false;
            next();
            return type != &tt::arrow;
        })) {
        // tsc: `async x` not followed by `=>` leaves `async` an identifier
        // (`for (async of xs)`)
        return parseIdent(false);
    }
    if (type == &tt::name && potentialArrowAt == start && !containsEsc && value != "async") {
        // `x => y`, also with a line break before the `=>` (the checker's error)
        bool arrowNext = peekIs(tt::arrow);
        if (arrowNext) {
            int s = start;
            Pos sl = startLoc;
            std::vector<Node*> params{parseIdent(false)};
            next();  // =>
            pendingNoArrowReturnType = !curAllowArrowReturnType;
            return parseArrowExpression(startNodeAt(s, sl), params, false, forInit);
        }
    }
    if (type == &tt::_function) {
        // the name of a function expression is a binding identifier even
        // when it is `yield` / `await`
        Node* node = startNode();
        next();
        return tsParseFunctionTs(node, false, false, false, false);
    }
    return JsxParser::parseExprAtom(r, forInit, forNew);
}

bool TsParser::tsInAwaitContext() {
    for (size_t i = scopeStack.size(); i-- > 0;) {
        int flags = scopeStack[i].flags;
        if (flags & SCOPE_CLASS_STATIC_BLOCK) return true;
        if (flags & SCOPE_CLASS_FIELD_INIT) return false;
        if (flags & SCOPE_FUNCTION) return (flags & SCOPE_ASYNC) > 0;
    }
    sawTopLevelAwait = true;
    return topLevelAwait;
}

// tsc: `import(...)` is a call -- any arguments (spread, none, three) and
// type arguments are the checker's to reject.
Node* TsParser::parseExprImport(bool forNew) {
    bool call = lookAhead([&] {
        next();
        return type == &tt::parenL || isLt();
    });
    if (!call || forNew) return Parser::parseExprImport(forNew);
    Node* node = startNode();
    next();
    if (isLt()) {
        node->typeArguments() = tryParse([&] { return tsParseTypeArgumentsInExpression(); });
        if (!node->typeArguments()) {
            // `import < a > b`: comparisons (tsc parses `import` as an
            // expression and leaves it to the checker)
            Pos importEnd = node->loc_start;
            importEnd.column += 6;
            node->str = "import";
            return finishNodeAt(node, NodeType::Identifier, static_cast<int>(node->ustart) + 6, importEnd);
        }
    }
    if (type != &tt::parenL && node->typeArguments()) {
        // `import<T>`: an instantiation of `import` (the checker's error)
        Pos importEnd = node->loc_start;
        importEnd.column += 6;
        Node* id = startNodeAt(static_cast<int>(node->ustart), node->loc_start);
        id->str = "import";
        id = finishNodeAt(id, NodeType::Identifier, static_cast<int>(node->ustart) + 6, importEnd);
        Node* inst = startNodeAt(static_cast<int>(node->ustart), node->loc_start);
        inst->expression() = id;
        inst->typeArguments() = node->typeArguments();
        return finishNode(inst, NodeType::TSInstantiationExpression);
    }
    expect(tt::parenL);
    std::vector<Node*> args = parseExprList(tt::parenR, true, false);
    node->source() = args.size() > 0 ? args[0] : nullptr;
    node->options() = args.size() > 1 ? args[1] : nullptr;
    node->Set(kFlagHasOptions, true);
    return finishNode(node, NodeType::ImportExpression);
}

// Object literal members may carry modifiers and decorators in tsc's parser
// (`{ public a: 1 }`); the checker rejects them.
Node* TsParser::parseProperty(bool isPattern, DestructuringErrors* r) {
    int modStart = -1;
    Pos modLoc;
    // `async` is a modifier here only before a `key:` (`{ async a: 1 }`);
    // else it is the base parser's (async methods, a key named async)
    bool asyncModifier = isWord("async") && lookAhead([&] {
                             next();
                             if (hasPrecedingLineBreak()) return false;
                             if (!isIdentOrKeywordToken() && type != &tt::string && type != &tt::num) return false;
                             next();
                             return type == &tt::colon;
                         });
    if (!isPattern && (type == &tt::at || asyncModifier || (isModifierToken() && !isWord("async")))) {
        Mods m;
        modStart = start;
        modLoc = startLoc;
        tsParseModifiers(m, true);
        if (m.start < 0) modStart = -1;
    }
    if (isPattern && type == &tt::ellipsis) {
        // tsc parseObjectBindingElement: `...name` or `...key: target`
        Node* prop = startNode();
        next();
        bool ident = isIdentToken();
        Node* holder = startNode();
        Node* key = parsePropertyName(holder);
        if (ident && type != &tt::colon) {
            prop->argument() = key;
        } else {
            // the checker's error; typescript-estree keeps the key
            expect(tt::colon);
            parseBindingAtom();
            prop->argument() = key;
        }
        return finishNode(prop, NodeType::RestElement);
    }
    Node* prop = Parser::parseProperty(isPattern, r);
    if (modStart >= 0) {
        // typescript-estree: the member's range includes its modifiers
        prop->ustart = static_cast<uint32_t>(modStart);
        prop->loc_start = modLoc;
        prop->start = out.ByteOffset(prop->ustart);
    }
    return prop;
}

Node* TsParser::parsePropertyName(Node* prop) {
    if (type == &tt::privateId) {
        prop->Set(kFlagComputed, false);
        return prop->key() = parsePrivateIdent();
    }
    if (eat(tt::bracketL)) {
        // tsc parseComputedPropertyName: a full Expression (`[a, b]: 1`)
        prop->Set(kFlagComputed, true);
        prop->key() = parseExpression();
        expect(tt::bracketR);
        return prop->key();
    }
    return Parser::parsePropertyName(prop);
}

Node* TsParser::tsParseDecoratedClassExpression() {
    int s = start;
    Pos sl = startLoc;
    Mods m;
    tsParseModifiers(m, true);
    if (type != &tt::_class) unexpected();
    Node* node = startNodeAt(s, sl);
    return tsParseClassTs(node, false, &m);
}

Node* TsParser::parseNew() {
    bool meta = lookAhead([&] {
        next();
        return type == &tt::dot;
    });
    if (meta) return Parser::parseNew();
    Node* node = startNode();
    next();
    // tsc's parsePrimaryExpression: no JSX element or `<T>x` after `new`
    if (type == &tt::jsxTagStart || isLt()) unexpected();
    int startPos = start;
    Pos sLoc = startLoc;
    Node* callee = parseSubscripts(parseExprAtom(nullptr, ForInit::No, true), startPos, sLoc, true, ForInit::No);
    if (callee->type == NodeType::TSInstantiationExpression) {
        node->typeArguments() = callee->typeArguments();
        callee = callee->expression();
    }
    node->callee() = callee;
    if (eat(tt::parenL))
        node->arguments() = list(parseExprList(tt::parenR, true, false));
    else
        node->arguments() = {};
    return finishNode(node, NodeType::NewExpression);
}

Node* TsParser::parseParenItem(Node* item) { return item; }

// ---------------------------------------------------------------- object literals / methods

void TsParser::parsePropertyValue(Node* prop, bool isPattern, bool isGenerator, bool isAsync, int startPos,
                                  Pos sLoc, DestructuringErrors* r, bool hadEsc) {
    if (!isPattern) {
        // `{ a?() {} }` / `{ a!: 1 }`: tsc parses them (the checker rejects)
        if (type == &tt::question) {
            bool methodNext = lookAhead([&] {
                next();
                return type == &tt::parenL || isLt() || type == &tt::colon || type == &tt::comma ||
                       type == &tt::braceR || type == &tt::eq;
            });
            if (methodNext) {
                next();
                prop->SetTs(kTsOptional, true);
            }
        } else if (type == &tt::prefix && value == "!") {
            exprAllowed = false;
            next();
            prop->SetTs(kTsDefinite, true);
        }
        if (isLt()) {
            prop->Set(kFlagMethod, true);
            prop->value() = parseMethod(isGenerator, isAsync);
            prop->kind = "init";
            return;
        }
    }
    Parser::parsePropertyValue(prop, isPattern, isGenerator, isAsync, startPos, sLoc, r, hadEsc);
}

Node* TsParser::parseMethod(bool isGenerator, bool isAsync, bool allowDirectSuper) {
    Node* node = startNode();
    int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
    initFunction(node);
    node->Set(kFlagGenerator, isGenerator);
    node->Set(kFlagAsync, isAsync);
    yieldPos = 0;
    awaitPos = 0;
    awaitIdentPos = 0;
    enterScope(SCOPE_FUNCTION | (isAsync ? SCOPE_ASYNC : 0) | (isGenerator ? SCOPE_GENERATOR : 0) | SCOPE_SUPER |
               (allowDirectSuper ? SCOPE_DIRECT_SUPER : 0));
    node->typeParameters() = tsTryParseTypeParameters();
    node->params() = list(tsParseParams());
    node->returnType() = tsParseReturnType(false, false);
    NodeType t = NodeType::FunctionExpression;
    if (type == &tt::braceL) {
        tsBlockContext();
        parseFunctionBody(node, false, true, ForInit::No);
    } else {
        // parseFunctionBlockOrSemicolon: an overload or abstract member
        semicolon();
        exitScope();
        node->body() = nullptr;
        t = NodeType::TSEmptyBodyFunctionExpression;
    }
    yieldPos = oldYieldPos;
    awaitPos = oldAwaitPos;
    awaitIdentPos = oldAwaitIdentPos;
    return finishNode(node, t);
}

// ---------------------------------------------------------------- JSX

Node* TsParser::jsx_parseOpeningElementAt(int startPos, Pos sLoc) {
    Node* node = startNodeAt(startPos, sLoc);
    std::vector<Node*> attributes;
    Node* nodeName = jsx_parseElementName();
    if (nodeName) node->name() = nodeName;
    if (nodeName && type == &tt::relational && value == "<") {
        // `<C<T> ...>`: type arguments, read with the ordinary tokenizer
        context.push_back(&tc::b_expr);
        Node* ta = startNode();
        next();
        std::vector<Node*> params;
        bool first = true;
        while (!isGtStart()) {
            if (!first) {
                expect(tt::comma);
                if (isGtStart()) break;
            }
            first = false;
            params.push_back(tsParseTypeInContext(false));
        }
        if (!(type == &tt::relational && value == ">")) splitToken(1, tt::relational, ">");
        ta->params() = list(params);
        finishNodeAt(ta, NodeType::TSTypeParameterInstantiation, end, endLoc);
        node->typeArguments() = ta;
        if (!context.empty() && context.back() == &tc::b_expr) context.pop_back();
        exprAllowed = false;  // `<C<T> />`: the `/` closes the tag
        next();
    }
    while (type != &tt::slash && type != &tt::jsxTagEnd) attributes.push_back(jsx_parseAttribute());
    node->attributes() = list(attributes);
    node->Set(kFlagSelfClosing, eat(tt::slash));
    expect(tt::jsxTagEnd);
    return finishNode(node, nodeName ? NodeType::JSXOpeningElement : NodeType::JSXOpeningFragment);
}

// tsc: `attr={}` is the checker's error, not the parser's.
Node* TsParser::jsx_parseAttributeValue() {
    if (type == &tt::braceL) {
        if (peekIs(tt::ellipsis)) {
            next();
            unexpected();  // `a={...b}`: no spread in an attribute value
        }
        return jsx_parseExpressionContainer();
    }
    return JsxParser::jsx_parseAttributeValue();
}

// `{...children}` is a JSXSpreadChild (attributes read their own `{...}`).
Node* TsParser::jsx_parseExpressionContainer() {
    Node* node = startNode();
    next();
    if (type == &tt::ellipsis) {
        next();
        node->expression() = parseExpression();
        expect(tt::braceR);
        return finishNode(node, NodeType::JSXSpreadChild);
    }
    node->expression() = type == &tt::braceR ? jsx_parseEmptyExpression() : parseExpression();
    expect(tt::braceR);
    return finishNode(node, NodeType::JSXExpressionContainer);
}

}  // namespace jsparse
