// TypeScript dialect (see js_parse_ts.h): statements and declarations --
// decorators and modifiers, interfaces, type aliases, enums, namespaces,
// classes, functions with overloads, and TS's import/export forms
// (tsc's parser.ts, "DECLARATIONS").

#include <algorithm>

#include "js_parse_ts.h"

namespace jsparse {

namespace {

bool IsFunctionContext(const TokContext* c) { return c && std::string_view(c->token) == "function"; }

}  // namespace

// ---------------------------------------------------------------- decorators / modifiers

Node* TsParser::tsParseDecorator() {
    Node* node = startNode();
    expect(tt::at);
    bool saved = inDecorator;
    inDecorator = true;
    try {
        node->expression() = parseExprSubscripts(nullptr, ForInit::No);
    } catch (...) {
        inDecorator = saved;
        throw;
    }
    inDecorator = saved;
    return finishNode(node, NodeType::Decorator);
}

std::vector<Node*> TsParser::tsParseDecorators() {
    std::vector<Node*> d;
    while (type == &tt::at) d.push_back(tsParseDecorator());
    return d;
}

bool TsParser::tsTryParseModifier(Mods& m, bool permitConst, bool stopOnStaticBlock) {
    int s = start;
    Pos sl = startLoc;
    std::string word = type == &tt::name ? value : type->keyword ? type->keyword : "";
    if (type == &tt::_const && permitConst) {
        // tryParse(nextTokenIsOnSameLineAndCanFollowModifier): consumes `const`
        bool ok = lookAhead([&] {
            next();
            return !hasPrecedingLineBreak() && canFollowModifier();
        });
        if (!ok) return false;
        next();
    } else if (stopOnStaticBlock && isWord("static") && peekIs(tt::braceL)) {
        return false;
    } else if (m.static_ && isWord("static")) {
        return false;
    } else if (!parseAnyContextualModifier()) {
        return false;
    }
    if (m.start < 0) {
        m.start = s;
        m.startLoc = sl;
    }
    if (m.afterDecorators < 0) {
        m.afterDecorators = s;
        m.afterDecoratorsLoc = sl;
    }
    ++m.count;
    if (word == "export") {
        m.exp = true;
        m.exportPos = s;
        m.exportLoc = sl;
        m.innerStart = -1;
    } else if (word == "default") {
        m.def = true;
        m.innerStart = -1;
    } else {
        if (m.innerStart < 0) {
            m.innerStart = s;
            m.innerLoc = sl;
        }
        if (word == "declare") m.decl = true;
        else if (word == "abstract") m.abstract_ = true;
        else if (word == "async") m.async_ = true;
        else if (word == "const") m.const_ = true;
        else if (word == "static") m.static_ = true;
        else if (word == "readonly") m.readonly_ = true;
        else if (word == "override") m.override_ = true;
        else if (word == "accessor") m.accessor_ = true;
        else if (word == "in") m.in_ = true;
        else if (word == "out") m.out_ = true;
        else if (word == "public" || word == "private" || word == "protected") m.access = str(word);
    }
    return true;
}

void TsParser::tsParseModifiers(Mods& m, bool allowDecorators, bool permitConst, bool stopOnStaticBlock) {
    auto decorators = [&]() {
        while (type == &tt::at) {
            int s = start;
            Pos sl = startLoc;
            if (m.start < 0) {
                m.start = s;
                m.startLoc = sl;
            }
            if (m.innerStart < 0 && m.exp) {
                m.innerStart = s;
                m.innerLoc = sl;
            }
            m.decorators.push_back(tsParseDecorator());
        }
    };
    if (allowDecorators && type == &tt::at) {
        decorators();
        // decorators before the modifiers: the declaration starts at them
        // unless an `export` follows (tsc's export node starts at `export`)
        if (!m.decorators.empty()) {
            m.innerStart = m.start;
            m.innerLoc = m.startLoc;
        }
    }
    int before = m.count;
    while (tsTryParseModifier(m, permitConst, stopOnStaticBlock)) {
    }
    if (m.count > before && allowDecorators && type == &tt::at) {
        decorators();
        while (tsTryParseModifier(m, permitConst, stopOnStaticBlock)) {
        }
    }
}

// isDeclaration (consumes tokens: call inside lookAhead).
bool TsParser::tsIsDeclaration() {
    for (;;) {
        if (type == &tt::_var || type == &tt::_const || type == &tt::_function || type == &tt::_class ||
            isWord("let") || isWord("enum"))
            return true;
        if (type == &tt::at) return true;
        if (isWord("using")) {
            next();
            return (isIdentToken() || type == &tt::braceL) && !hasPrecedingLineBreak();
        }
        if (isWord("await")) {
            next();
            if (!isWord("using") || hasPrecedingLineBreak()) return false;
            next();
            return (isIdentToken() || type == &tt::braceL) && !hasPrecedingLineBreak();
        }
        if (isWord("interface") || isWord("type")) {
            next();
            return isIdentToken() && !hasPrecedingLineBreak();
        }
        if (isWord("module") || isWord("namespace")) {
            next();
            return (isIdentToken() || type == &tt::string) && !hasPrecedingLineBreak();
        }
        if (isWord("abstract") || isWord("accessor") || isWord("async") || isWord("declare") || isWord("private") ||
            isWord("protected") || isWord("public") || isWord("readonly")) {
            bool wasDeclare = isWord("declare");
            next();
            if (hasPrecedingLineBreak()) return false;
            if (wasDeclare && isWord("type")) return true;
            continue;
        }
        if (isWord("global")) {
            next();
            return type == &tt::braceL || isIdentToken() || type == &tt::_export;
        }
        if (type == &tt::_import) {
            next();
            return type == &tt::string || type == &tt::star || type == &tt::braceL || isIdentOrKeywordToken();
        }
        if (type == &tt::_export) {
            next();
            const TokenType* cur = type;
            bool curIsAs = isWord("as");
            if (isWord("type")) {
                Peek p = peek();
                cur = p.type;
                curIsAs = p.type == &tt::name && p.value == "as";
            }
            if (cur == &tt::eq || cur == &tt::star || cur == &tt::braceL || cur == &tt::_default || curIsAs ||
                cur == &tt::at)
                return true;
            continue;
        }
        if (isWord("static")) {
            next();
            continue;
        }
        return false;
    }
}

// ---------------------------------------------------------------- statements

Node* TsParser::parseStatement(const char* ctx, bool topLevel, std::vector<std::string>* exports) {
    DepthGuard guard(*this);
    if (type == &tt::at) return tsParseDeclaration(exports);
    if (type == &tt::_import && lookAhead([&] {
            next();
            return isLt();
        })) {
        // `import<T>(...)` / `import<T>`: an expression statement
        Node* node = startNode();
        Node* expr = parseExpression();
        return parseExpressionStatement(node, expr);
    }
    bool maybeDecl = false;
    if (type == &tt::_const || type == &tt::_export || type == &tt::_import) {
        maybeDecl = true;
    } else if (type == &tt::name && !containsEsc) {
        static const char* const kWords[] = {"async",    "interface", "type",     "module", "namespace",
                                             "declare",  "abstract",  "accessor", "private", "protected",
                                             "public",   "static",    "readonly", "global",  "enum"};
        for (const char* w : kWords)
            if (value == w) maybeDecl = true;
    }
    if (maybeDecl && tsIsStartOfDeclaration()) return tsParseDeclaration(exports);
    if (ctx) {
        // tsc parses declarations as statements anywhere (the checker
        // reports the ones in the wrong place).
        Node* node = startNode();
        if (type == &tt::_function) return parseFunctionStatement(node, false, false);
        if (type == &tt::_class) return parseClass(node, 1);
        if (type == &tt::_const || (isWord("let") && lookAhead([&] {
                                        next();
                                        return isIdentToken() || type == &tt::braceL || type == &tt::bracketL;
                                    }))) {
            std::string kind = type == &tt::_const ? "const" : "let";
            return parseVarStatement(node, str(kind));
        }
    }
    if (isWord("using") && lookAhead([&] { return tsNextIsUsingBinding(false); })) {
        Node* node = startNode();
        next();
        parseVar(node, false, "using");
        semicolon();
        return finishNode(node, NodeType::VariableDeclaration);
    }
    if (isWord("await") && lookAhead([&] {
            next();
            return isWord("using") && tsNextIsUsingBinding(false);
        })) {
        Node* node = startNode();
        next();
        next();
        parseVar(node, false, "await using");
        semicolon();
        return finishNode(node, NodeType::VariableDeclaration);
    }
    if ((isWord("using") || isWord("await")) && !containsEsc) {
        // not a declaration by tsc's rules (acorn's `using` paths differ):
        // an expression statement, or a label
        Node* node = startNode();
        std::string name = value;
        Node* expr = parseExpression();
        if (expr->type == NodeType::Identifier && eat(tt::colon))
            return parseLabeledStatement(node, name, expr, ctx);
        return parseExpressionStatement(node, expr);
    }
    if (isWord("let") && !ctx) {
        // tsc: `let` followed by an identifier, `[` or `{` declares.
        bool decl = lookAhead([&] {
            next();
            return isIdentToken() || type == &tt::braceL || type == &tt::bracketL;
        });
        if (decl) {
            Node* node = startNode();
            return parseVarStatement(node, "let");
        }
    }
    return JsxParser::parseStatement(ctx, topLevel, exports);
}

Node* TsParser::tsParseDeclaration(std::vector<std::string>*) {
    Mods m;
    int s = start;
    Pos sl = startLoc;
    tsParseModifiers(m, true);
    if (m.start < 0) {
        m.start = s;
        m.startLoc = sl;
    }
    int innerStart = m.innerStart >= 0 ? m.innerStart : start;
    Pos innerLoc = m.innerStart >= 0 ? m.innerLoc : startLoc;
    if (m.decl) ++inAmbient;
    Node* decl;
    try {
        decl = tsParseDeclarationWorker(m, innerStart, innerLoc);
    } catch (...) {
        if (m.decl) --inAmbient;
        throw;
    }
    if (m.decl) --inAmbient;
    if (!m.exp) return decl;
    // `export` (and `export default`) as a modifier: wrap the declaration.
    Node* ex = startNodeAt(m.exportPos, m.exportLoc);
    ex->declaration() = decl;
    if (m.def) return finishNode(ex, NodeType::ExportDefaultDeclaration);
    ex->specifiers() = {};
    ex->source() = nullptr;
    ex->attributes() = {};
    if (decl->type == NodeType::TSInterfaceDeclaration || decl->type == NodeType::TSTypeAliasDeclaration ||
        decl->HasTs(kTsDeclare))
        ex->SetTs(kTsTypeOnly, true);
    return finishNode(ex, NodeType::ExportNamedDeclaration);
}

Node* TsParser::tsParseDeclarationWorker(Mods& m, int innerStart, Pos innerLoc) {
    Node* node = startNodeAt(innerStart, innerLoc);
    node->tslist[0] = list(m.decorators);
    auto setDeclare = [&](Node* n) {
        if (m.decl) n->SetTs(kTsDeclare, true);
        return n;
    };
    if (type == &tt::_var || type == &tt::_const || isWord("let") || isWord("using") || isWord("await")) {
        std::string kind;
        if (isWord("await")) {
            next();
            if (!isWord("using")) unexpected();
            kind = "await using";
        } else {
            kind = type == &tt::name ? value : type->keyword;
        }
        next();
        parseVar(node, false, str(kind));
        semicolon();
        node->tslist[0] = {};
        return setDeclare(finishNode(node, NodeType::VariableDeclaration));
    }
    if (type == &tt::_function) {
        next();
        node->tslist[0] = {};
        Node* f = tsParseFunctionTs(node, true, m.async_, m.def, m.decl);
        return setDeclare(f);
    }
    if (type == &tt::_class) return setDeclare(tsParseClassTs(node, true, &m));
    if (isWord("interface")) {
        node->tslist[0] = {};
        return setDeclare(tsParseInterface(node));
    }
    if (isWord("type")) {
        node->tslist[0] = {};
        return setDeclare(tsParseTypeAlias(node));
    }
    if (isWord("enum")) {
        node->tslist[0] = {};
        return setDeclare(tsParseEnum(node, m.const_));
    }
    if (isWord("global") || isWord("module") || isWord("namespace")) {
        node->tslist[0] = {};
        return setDeclare(tsParseModule(node));
    }
    if (type == &tt::_import) {
        node->tslist[0] = {};
        return tsParseImportDeclaration(node);
    }
    if (type == &tt::_export) {
        node->tslist[0] = {};
        return tsParseExportDeclaration(node, nullptr);
    }
    // modifiers or decorators with no declaration after them
    unexpected();
}

// ---------------------------------------------------------------- variables

// nextTokenIsBindingIdentifierOrStartOfDestructuringOnSameLine.
bool TsParser::tsNextIsUsingBinding(bool disallowOf) {
    next();
    if (disallowOf && isWord("of")) {
        next();
        return type == &tt::eq || type == &tt::semi || type == &tt::colon;
    }
    return (isIdentToken() || type == &tt::braceL) && !hasPrecedingLineBreak();
}

// parseErrorForMissingSemicolonAfter: tsc reports nothing when the
// statement is the bare identifier `declare` (`declare readonly\nx`).
Node* TsParser::parseExpressionStatement(Node* node, Node* expr) {
    node->expression() = expr;
    bool bareDeclare = expr->type == NodeType::Identifier && expr->str == "declare" && !containsEsc;
    if (bareDeclare)
        eat(tt::semi);
    else
        semicolon();
    return finishNode(node, NodeType::ExpressionStatement);
}

Node* TsParser::parseThrowStatement(Node* node) {
    next();
    // tsc: a line break leaves the expression missing, without a diagnostic.
    if (hasPrecedingLineBreak()) {
        Node* id = startNodeAt(lastTokEnd, lastTokEndLoc);
        node->argument() = finishNodeAt(id, NodeType::Identifier, lastTokEnd, lastTokEndLoc);
    } else {
        node->argument() = parseExpression();
    }
    semicolon();
    return finishNode(node, NodeType::ThrowStatement);
}

// parseVariableDeclarationList. tsc leaves missing initializers (`const
// x;`), empty lists (`var;`) and trailing commas to the checker.
Node* TsParser::parseVar(Node* node, bool isFor, std::string_view kind, bool) {
    std::vector<Node*> decls;
    node->kind = kind;
    auto listEnds = [&]() {
        return canInsertSemicolon() || type == &tt::semi || type == &tt::_in || isWord("of") ||
               type == &tt::arrow;
    };
    // tsc canFollowContextualOfKeyword: `for (var of X)` is an empty list.
    if (isFor && isWord("of") && lookAhead([&] {
            next();
            if (!isIdentToken()) return false;
            next();
            return type == &tt::parenR;
        })) {
        node->declarations() = {};
        return node;
    }
    bool first = true;
    for (;;) {
        bool startsBinding = type == &tt::name || type == &tt::bracketL || type == &tt::braceL;
        if (!startsBinding && listEnds() && (first || decls.size())) break;
        first = false;
        Node* decl = startNode();
        parseVarId(decl, kind);
        if (type == &tt::eq) {
            next();
            decl->init() = parseMaybeAssign(isFor ? ForInit::Yes : ForInit::No);
        } else {
            decl->init() = nullptr;
        }
        decls.push_back(finishNode(decl, NodeType::VariableDeclarator));
        if (!eat(tt::comma)) break;
    }
    node->declarations() = list(decls);
    return node;
}

// parseForOrForInOrForOfStatement: `for await` anywhere, any number of
// declarations before `of`, initializers before `in`.
Node* TsParser::parseForStatement(Node* node) {
    next();
    bool isAwait = isWord("await");
    if (isAwait) next();
    labels.push_back(Label{{}, 1, -1});
    enterScope(0);
    expect(tt::parenL);
    Node* init = nullptr;
    if (type != &tt::semi) {
        // tsc: var/let/const, `using` + binding on the same line (`of`
        // only when followed by = ; :), `await using` + binding.
        bool decl = type == &tt::_var || type == &tt::_const || isWord("let");
        if (!decl && isWord("using")) decl = lookAhead([&] { return tsNextIsUsingBinding(true); });
        bool awaitUsing = !decl && isWord("await") && lookAhead([&] {
                              next();
                              return isWord("using") && tsNextIsUsingBinding(false);
                          });
        if (decl || awaitUsing) {
            Node* d = startNode();
            std::string kind = awaitUsing ? "await using" : type == &tt::name ? value : type->keyword;
            if (awaitUsing) next();
            next();
            parseVar(d, true, str(kind));
            init = finishNode(d, NodeType::VariableDeclaration);
        } else {
            init = parseExpression(ForInit::Yes);
        }
    }
    if (isAwait || (init && isWord("of"))) {
        expectContextual("of");
        node->Set(kFlagAwait, isAwait);
        if (init->type != NodeType::VariableDeclaration) toAssignable(init, false);
        node->left() = init;
        node->right() = parseMaybeAssign();
        expect(tt::parenR);
        node->body() = parseStatement("for");
        exitScope();
        labels.pop_back();
        return finishNode(node, NodeType::ForOfStatement);
    }
    if (init && type == &tt::_in) {
        if (init->type != NodeType::VariableDeclaration) toAssignable(init, false);
        return parseForIn(node, init);
    }
    return parseFor(node, init);
}

void TsParser::parseVarId(Node* decl, std::string_view kind) {
    (void)kind;
    Node* id = decl->id() = parseBindingAtom();
    if (type == &tt::prefix && value == "!" && !hasPrecedingLineBreak()) {
        next();
        decl->SetTs(kTsDefinite, true);
    }
    if (type == &tt::colon) {
        id->typeAnnotation() = tsParseTypeAnnotation();
        extendTo(id);
    }
}

Node* TsParser::parseCatchClauseParam() {
    Node* param = parseBindingAtom();
    if (type == &tt::colon) {
        param->typeAnnotation() = tsParseTypeAnnotation();
        extendTo(param);
    }
    if (eat(tt::eq)) parseMaybeAssign();  // tsc parses it; the checker rejects it
    enterScope(param->type == NodeType::Identifier ? SCOPE_SIMPLE_CATCH : 0);
    expect(tt::parenR);
    return param;
}

Node* TsParser::parseBreakContinueStatement(Node* node, const char* keyword) {
    // Label checks are tsc's binder's.
    bool isBreak = std::string_view(keyword) == "break";
    next();
    if (eat(tt::semi) || insertSemicolon()) {
        node->label() = nullptr;
    } else if (!isIdentToken()) {
        unexpected();
    } else {
        node->label() = parseIdent();
        semicolon();
    }
    return finishNode(node, isBreak ? NodeType::BreakStatement : NodeType::ContinueStatement);
}

Node* TsParser::parseLabeledStatement(Node* node, const std::string& maybeName, Node* expr, const char*) {
    int kind = type->isLoop ? 1 : type == &tt::_switch ? 2 : 0;
    labels.push_back(Label{str(maybeName), kind, start});
    node->body() = parseStatement("label");
    labels.pop_back();
    node->label() = expr;
    return finishNode(node, NodeType::LabeledStatement);
}

// ---------------------------------------------------------------- functions

Node* TsParser::parseFunction(Node* node, int statement, bool allowExpressionBody, bool isAsync, ForInit forInit) {
    (void)allowExpressionBody;
    (void)forInit;
    return tsParseFunctionTs(node, (statement & FUNC_STATEMENT) != 0, isAsync, (statement & FUNC_NULLABLE_ID) != 0,
                             false);
}

// After `function` (the current token is `*` or the name).
Node* TsParser::tsParseFunctionTs(Node* node, bool statement, bool isAsync, bool nullableId, bool declare) {
    const size_t ctxDepth = context.size();  // the `function` token's context is on top
    initFunction(node);
    node->Set(kFlagGenerator, eat(tt::star));
    node->Set(kFlagAsync, isAsync);
    // tsc's parseBindingIdentifier: `yield` / `await` are names here too
    auto bindingName = [&]() { return type == &tt::name && value != "enum"; };
    if (statement) {
        if (bindingName())
            node->id() = parseIdent(true);
        else if (nullableId)
            node->id() = nullptr;
        else
            node->id() = parseIdent();
    }
    int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
    yieldPos = 0;
    awaitPos = 0;
    awaitIdentPos = 0;
    enterScope(SCOPE_FUNCTION | (node->Has(kFlagAsync) ? SCOPE_ASYNC : 0) |
               (node->Has(kFlagGenerator) ? SCOPE_GENERATOR : 0));
    if (!statement) node->id() = bindingName() ? parseIdent(true) : nullptr;
    node->typeParameters() = tsTryParseTypeParameters();
    parseFunctionParams(node);
    node->returnType() = tsParseReturnType(false, false);
    NodeType t = statement ? NodeType::FunctionDeclaration : NodeType::FunctionExpression;
    if (type == &tt::braceL || !statement) {
        tsBlockContext();
        parseFunctionBody(node, false, false, ForInit::No);
    } else {
        // An overload or a declaration: no body (parseFunctionBlockOrSemicolon).
        semicolon();
        exitScope();
        node->body() = nullptr;
        if (ctxDepth >= 1 && ctxDepth - 1 < context.size() && IsFunctionContext(context[ctxDepth - 1]))
            context.erase(context.begin() + static_cast<std::ptrdiff_t>(ctxDepth - 1));
        t = NodeType::TSDeclareFunction;
    }
    if (declare) node->SetTs(kTsDeclare, true);
    yieldPos = oldYieldPos;
    awaitPos = oldAwaitPos;
    awaitIdentPos = oldAwaitIdentPos;
    return finishNode(node, t);
}

void TsParser::parseFunctionParams(Node* node) { node->params() = list(tsParseParams()); }

// ---------------------------------------------------------------- interfaces, aliases, enums, modules

Node* TsParser::tsParseHeritageExpression(NodeType t) {
    Node* node = startNode();
    Node* expr = parseExprSubscripts(nullptr, ForInit::No);
    if (expr->type == NodeType::TSInstantiationExpression) {
        node->expression() = expr->expression();
        node->typeArguments() = expr->typeArguments();
    } else {
        node->expression() = expr;
        if (isLt()) node->typeArguments() = tsParseTypeArguments();
    }
    return finishNode(node, t);
}

// isListElement(HeritageClauseElement): the list may be empty (`extends {`
// is the body unless `{}` is followed by `,` `{` extends implements).
bool TsParser::tsHeritageElementStarts() {
    if (type == &tt::braceL)
        return lookAhead([&] {
            next();
            if (type != &tt::braceR) return true;
            next();
            return type == &tt::comma || type == &tt::braceL || type == &tt::_extends || isWord("implements");
        });
    if (type == &tt::_extends || isWord("implements")) return false;
    return isIdentToken() || type == &tt::_this || type == &tt::_super || type == &tt::_null ||
           type == &tt::_true || type == &tt::_false || type == &tt::num || type == &tt::string ||
           type == &tt::backQuote || type == &tt::parenL || type == &tt::bracketL || type == &tt::_function ||
           type == &tt::_class || type == &tt::_new || type == &tt::regexp || type == &tt::_import ||
           type == &tt::privateId;
}

Node* TsParser::tsParseInterface(Node* node) {
    next();  // interface
    if (!isIdentToken()) unexpected();
    node->id() = parseIdent();
    node->typeParameters() = tsTryParseTypeParameters();
    std::vector<Node*> heritage;
    while (type == &tt::_extends || isWord("implements")) {
        bool isExtends = type == &tt::_extends;
        next();
        while (tsHeritageElementStarts()) {
            Node* h = tsParseHeritageExpression(NodeType::TSInterfaceHeritage);
            if (isExtends) heritage.push_back(h);
            if (!eat(tt::comma)) break;
        }
    }
    node->extends_list() = list(heritage);
    tsBlockContext();
    Node* body = startNode();
    body->body_list() = list(tsParseObjectTypeMembers());
    node->body() = finishNode(body, NodeType::TSInterfaceBody);
    return finishNode(node, NodeType::TSInterfaceDeclaration);
}

Node* TsParser::tsParseTypeAlias(Node* node) {
    next();  // type
    if (hasPrecedingLineBreak()) Parser::raiseRecoverable(start, "Line break not permitted here");
    if (!isIdentToken()) unexpected();
    node->id() = parseIdent();
    node->typeParameters() = tsTryParseTypeParameters();
    expect(tt::eq);
    Node* t = nullptr;
    if (isWord("intrinsic")) {
        t = tryParse([&]() -> Node* {
            Node* k = tsParseKeywordType(NodeType::TSIntrinsicKeyword);
            return type == &tt::dot ? nullptr : k;
        });
    }
    if (!t) t = tsParseTypeInContext(false);
    node->typeAnnotation() = t;
    semicolon();
    return finishNode(node, NodeType::TSTypeAliasDeclaration);
}

Node* TsParser::tsParseEnum(Node* node, bool isConst) {
    next();  // enum
    if (isConst) node->SetTs(kTsConst, true);
    if (!isIdentToken()) unexpected();
    node->id() = parseIdent();
    Node* body = startNode();
    expect(tt::braceL);
    std::vector<Node*> members;
    bool first = true;
    while (!eat(tt::braceR)) {
        if (!first) {
            expect(tt::comma);
            if (eat(tt::braceR)) break;
        }
        first = false;
        Node* m = startNode();
        tsParsePropertyNameNode(m);  // the member's `id` is the property name slot
        if (eat(tt::eq)) m->initializer() = parseMaybeAssign();
        members.push_back(finishNode(m, NodeType::TSEnumMember));
    }
    body->members() = list(members);
    node->body() = finishNode(body, NodeType::TSEnumBody);
    return finishNode(node, NodeType::TSEnumDeclaration);
}

Node* TsParser::tsParseModuleBlock() {
    tsBlockContext();
    Node* node = startNode();
    expect(tt::braceL);
    std::vector<Node*> body;
    enterScope(0);
    while (type != &tt::braceR) {
        if (type == &tt::eof) {
            if (recover && sawErrors) break;
            unexpected();
        }
        Node* stmt = parseStatementRecovering(nullptr, true, nullptr);
        if (stmt) body.push_back(stmt);
    }
    exitScope();
    next();
    node->body_list() = list(body);
    return finishNode(node, NodeType::TSModuleBlock);
}

Node* TsParser::tsParseModuleOrNamespace(Node* node, bool nested) {
    (void)nested;
    int s = start;
    Pos sl = startLoc;
    Node* id;
    if (!isIdentToken()) unexpected();
    id = parseIdent();
    while (eat(tt::dot)) {
        Node* q = startNodeAt(s, sl);
        q->left() = id;
        if (!isIdentOrKeywordToken()) unexpected();
        q->right() = parseIdent(true);
        id = finishNode(q, NodeType::TSQualifiedName);
    }
    node->id() = id;
    node->body() = tsParseModuleBlock();
    return finishNode(node, NodeType::TSModuleDeclaration);
}

Node* TsParser::tsParseModule(Node* node) {
    if (isWord("global")) {
        node->kind = "global";
        node->SetTs(kTsGlobal, true);
        node->id() = parseIdent(true);
        if (type == &tt::braceL)
            node->body() = tsParseModuleBlock();
        else
            semicolon();
        return finishNode(node, NodeType::TSModuleDeclaration);
    }
    if (isWord("namespace")) {
        node->kind = "namespace";
        next();
        return tsParseModuleOrNamespace(node, false);
    }
    node->kind = "module";
    next();  // module
    if (type == &tt::string) {
        node->id() = parseLiteral();
        if (type == &tt::braceL)
            node->body() = tsParseModuleBlock();
        else
            semicolon();
        return finishNode(node, NodeType::TSModuleDeclaration);
    }
    return tsParseModuleOrNamespace(node, false);
}

// ---------------------------------------------------------------- classes

Node* TsParser::parseClass(Node* node, int isStatement) { return tsParseClassTs(node, isStatement != 0, nullptr); }

Node* TsParser::tsParseClassTs(Node* node, bool isStatement, Mods* mods) {
    next();  // class
    if (mods) {
        node->decorators() = list(mods->decorators);
        if (mods->abstract_) node->SetTs(kTsAbstract, true);
    }
    // tsc: `class implements X` is a class with no name
    bool implementsClause = isWord("implements") && lookAhead([&] {
                                next();
                                return isIdentOrKeywordToken();
                            });
    node->id() = isIdentToken() && !implementsClause ? parseIdent() : nullptr;
    node->typeParameters() = tsTryParseTypeParameters();
    std::vector<Node*> implements;
    while (type == &tt::_extends || isWord("implements")) {
        bool isExtends = type == &tt::_extends;
        next();
        while (tsHeritageElementStarts()) {
            if (isExtends) {
                Node* h = tsParseHeritageExpression(NodeType::TSInterfaceHeritage);
                if (!node->superClass()) {
                    node->superClass() = h->expression();
                    node->typeArguments() = h->typeArguments();
                }
            } else {
                implements.push_back(tsParseHeritageExpression(NodeType::TSClassImplements));
            }
            if (!eat(tt::comma)) break;
        }
    }
    node->implements() = list(implements);
    tsBlockContext();
    enterClassBody();
    Node* classBody = startNode();
    std::vector<Node*> body;
    expect(tt::braceL);
    while (type != &tt::braceR) {
        if (recover && sawErrors && type == &tt::eof) break;
        Snapshot snap;
        if (recover) snap = takeSnapshot();
        try {
            Node* element = tsParseClassMember(node->superClass() != nullptr);
            if (element) body.push_back(element);
        } catch (const SyntaxErr& e) {
            if (!recover) throw;
            int errAt = start;
            recordError(e);
            resync(snap, std::max(errAt, e.pos), true, false);
        }
    }
    next();
    classBody->body_list() = list(body);
    node->body() = finishNode(classBody, NodeType::ClassBody);
    exitClassBody();
    return finishNode(node, isStatement ? NodeType::ClassDeclaration : NodeType::ClassExpression);
}

Node* TsParser::tsParseClassMethodTs(Node* method, bool isGenerator, bool isAsync, bool allowsDirectSuper) {
    method->value() = parseMethod(isGenerator, isAsync, allowsDirectSuper);
    return finishNode(method, method->HasTs(kTsAbstract) ? NodeType::TSAbstractMethodDefinition
                                                          : NodeType::MethodDefinition);
}

Node* TsParser::tsParseClassMember(bool hasSuper) {
    if (eat(tt::semi)) return nullptr;
    DepthGuard guard(*this);
    Node* node = startNode();
    Mods m;
    tsParseModifiers(m, true, true, true);
    node->decorators() = list(m.decorators);
    auto applyMods = [&](Node* n) {
        n->Set(kFlagStatic, m.static_);
        if (m.abstract_) n->SetTs(kTsAbstract, true);
        if (m.readonly_) n->SetTs(kTsReadonly, true);
        if (m.override_) n->SetTs(kTsOverride, true);
        if (m.decl) n->SetTs(kTsDeclare, true);
        if (!m.access.empty()) n->accessibility = m.access;
    };
    applyMods(node);
    if (isWord("static") && peekIs(tt::braceL)) {
        next();
        expect(tt::braceL);
        parseClassStaticBlock(node);
        node->decorators() = {};
        return node;
    }
    for (const char* acc : {"get", "set"}) {
        if (!isWord(acc)) continue;
        TsSave s = tsSave();
        bool ok = false;
        try {
            ok = nextTokenCanFollowModifier();
        } catch (const SyntaxErr&) {
            ok = false;
        }
        tsRestore(s);
        if (!ok) continue;
        next();
        node->kind = acc;
        tsParsePropertyNameNode(node);
        return tsParseClassMethodTs(node, false, false, false);
    }
    // tryParseConstructorDeclaration: `constructor` always starts one (tsc
    // reports a `constructor` property's missing `(`); "constructor" does
    // when a `(` follows.
    bool isCtorName = (type == &tt::name && value == "constructor") ||
                      (type == &tt::string && value == "constructor" && peekIs(tt::parenL));
    if (isCtorName) {
        node->Set(kFlagComputed, false);
        node->key() = type == &tt::string ? parseLiteral() : parseIdent(true);
        node->kind = "constructor";
        return tsParseClassMethodTs(node, false, false, hasSuper);
    }
    if (tsIsIndexSignature()) {
        tsParseIndexSignature(node);
        tsParseTypeMemberSemicolon();
        node->decorators() = {};
        if (m.static_) {
            node->Set(kFlagStatic, false);
            node->SetTs(kTsStatic, true);
        }
        return finishNode(node, NodeType::TSIndexSignature);
    }
    if (isLiteralPropertyNameToken() || type == &tt::star || type == &tt::bracketL || type == &tt::privateId) {
        bool isGenerator = eat(tt::star);
        tsParsePropertyNameNode(node);
        if (eat(tt::question)) node->SetTs(kTsOptional, true);
        if (isGenerator || type == &tt::parenL || isLt()) {
            node->kind = "method";
            return tsParseClassMethodTs(node, isGenerator, m.async_, false);
        }
        // property
        if (!node->HasTs(kTsOptional) && !hasPrecedingLineBreak() && type == &tt::prefix && value == "!") {
            next();
            node->SetTs(kTsDefinite, true);
        }
        node->typeAnnotation() = tsTryParseTypeAnnotation();
        if (eat(tt::eq)) {
            enterScope(SCOPE_CLASS_FIELD_INIT | SCOPE_SUPER);
            node->value() = parseMaybeAssign();
            exitScope();
        } else {
            node->value() = nullptr;
        }
        semicolon();
        NodeType t = m.accessor_ ? (m.abstract_ ? NodeType::TSAbstractAccessorProperty : NodeType::AccessorProperty)
                                 : (m.abstract_ ? NodeType::TSAbstractPropertyDefinition : NodeType::PropertyDefinition);
        return finishNode(node, t);
    }
    // modifiers with no member name after them: "Declaration expected"
    unexpected();
}

// ---------------------------------------------------------------- import / export

Node* TsParser::tsParseModuleExportName() {
    if (type == &tt::string) return parseLiteral();
    if (!isIdentOrKeywordToken()) unexpected();
    return parseIdent(true);
}

// parseImportOrExportSpecifier, with its `type` modifier rules.
Node* TsParser::tsParseImportOrExportSpecifier(bool isImport) {
    Node* node = startNode();
    bool typeOnly = false;
    Node* propertyName = nullptr;
    bool canParseAsKeyword = true;
    auto canParseName = [&]() { return isIdentOrKeywordToken() || type == &tt::string; };
    // tsc: an imported binding written as a reserved word is an error
    // (`import { default }`); `yield`, `await`, `let` ... are fine
    bool localIsKeyword = false;
    auto exportName = [&]() {
        localIsKeyword = type->keyword != nullptr && type != &tt::name;
        return tsParseModuleExportName();
    };
    Node* name = exportName();
    if (name->type == NodeType::Identifier && name->str == "type") {
        if (isWord("as")) {
            Node* firstAs = parseIdent(true);
            if (isWord("as")) {
                Node* secondAs = parseIdent(true);
                if (canParseName()) {
                    typeOnly = true;
                    propertyName = firstAs;
                    name = exportName();
                    canParseAsKeyword = false;
                } else {
                    propertyName = name;
                    name = secondAs;
                    canParseAsKeyword = false;
                }
            } else if (canParseName()) {
                propertyName = name;
                canParseAsKeyword = false;
                name = exportName();
            } else {
                typeOnly = true;
                name = firstAs;
            }
        } else if (canParseName()) {
            typeOnly = true;
            name = exportName();
        }
    }
    if (canParseAsKeyword && isWord("as")) {
        propertyName = name;
        next();
        name = exportName();
    }
    if (isImport) {
        if (name->type != NodeType::Identifier) raise(static_cast<int>(name->ustart), "Unexpected token");
        if (localIsKeyword) raiseRecoverable(static_cast<int>(name->ustart), "Unexpected keyword '" + std::string(name->str) + "'");
        node->imported() = propertyName ? propertyName : name;
        node->local() = name;
        if (typeOnly) node->SetTs(kTsTypeOnly, true);
        return finishNode(node, NodeType::ImportSpecifier);
    }
    node->local() = propertyName ? propertyName : name;
    node->exported() = name;
    if (typeOnly) node->SetTs(kTsTypeOnly, true);
    return finishNode(node, NodeType::ExportSpecifier);
}

namespace {

// tsc's import attributes: `with { type: "json" }` (or `assert`).
}  // namespace

Node* TsParser::parseImport(Node* node) {
    // tsc: an import declaration starts `import` + string, `*`, `{` or a name
    bool clause = lookAhead([&] {
        next();
        return type == &tt::string || type == &tt::star || type == &tt::braceL || isIdentOrKeywordToken();
    });
    if (!clause) unexpected();
    return tsParseImportDeclaration(node);
}

Node* TsParser::tsParseImportDeclaration(Node* node) {
    expect(tt::_import);
    Node* identifier = nullptr;
    if (isIdentToken()) identifier = parseIdent();
    bool typeOnly = false;
    if (identifier && identifier->str == "type" &&
        (!isWord("from") || (isIdentToken() && lookAhead([&] {
                                 next();
                                 return isWord("from") || type == &tt::eq;
                             }))) &&
        (isIdentToken() || type == &tt::star || type == &tt::braceL)) {
        typeOnly = true;
        identifier = isIdentToken() ? parseIdent() : nullptr;
    } else if (identifier && identifier->str == "defer" &&
               (isWord("from") ? !peekIs(tt::string) : type != &tt::comma && type != &tt::eq)) {
        node->kind = "defer";  // `import defer * as ns from "m"` (phase)
        identifier = isIdentToken() ? parseIdent() : nullptr;
    }
    if (identifier && type != &tt::comma && !isWord("from") && node->kind != "defer")
        return tsParseImportEquals(node, identifier, typeOnly);
    std::vector<Node*> specs;
    bool hasClause = identifier || type == &tt::star || type == &tt::braceL;
    if (identifier) {
        Node* d = startNodeAt(static_cast<int>(identifier->ustart), identifier->loc_start);
        d->local() = identifier;
        specs.push_back(finishAt(d, NodeType::ImportDefaultSpecifier, identifier));
    }
    if (!identifier || eat(tt::comma)) {
        if (type == &tt::star) {
            Node* ns = startNode();
            next();
            expectContextual("as");
            if (!isIdentToken()) unexpected();
            ns->local() = parseIdent();
            specs.push_back(finishNode(ns, NodeType::ImportNamespaceSpecifier));
        } else if (hasClause || type == &tt::braceL) {
            expect(tt::braceL);
            bool first = true;
            while (!eat(tt::braceR)) {
                if (!first) {
                    expect(tt::comma);
                    if (eat(tt::braceR)) break;
                }
                first = false;
                specs.push_back(tsParseImportOrExportSpecifier(true));
            }
        }
    }
    if (hasClause) expectContextual("from");
    node->specifiers() = list(specs);
    node->source() = tsParseModuleSpecifier();
    node->attributes() = tsParseImportAttributes();
    semicolon();
    if (typeOnly) node->SetTs(kTsTypeOnly, true);
    return finishNode(node, NodeType::ImportDeclaration);
}

Node* TsParser::tsParseModuleSpecifier() {
    if (type == &tt::string) return parseLiteral();
    return parseExpression();
}

NodeList TsParser::tsParseImportAttributes() {
    if (!((type == &tt::_with || isWord("assert")) && !hasPrecedingLineBreak())) return {};
    next();
    expect(tt::braceL);
    std::vector<Node*> attrs;
    bool first = true;
    while (!eat(tt::braceR)) {
        if (!first) {
            if (!eat(tt::semi)) expect(tt::comma);
            if (eat(tt::braceR)) break;
        }
        first = false;
        Node* a = startNode();
        if (type == &tt::string || type == &tt::num)
            a->key() = parseLiteral();
        else if (isIdentOrKeywordToken())
            a->key() = parseIdent(true);
        else
            unexpected();
        expect(tt::colon);
        a->value() = parseMaybeAssign();
        attrs.push_back(finishNode(a, NodeType::ImportAttribute));
    }
    return list(attrs);
}

Node* TsParser::tsParseImportEquals(Node* node, Node* id, bool isTypeOnly) {
    expect(tt::eq);
    node->id() = id;
    if (isWord("require") && peekIs(tt::parenL)) {
        Node* ref = startNode();
        next();
        expect(tt::parenL);
        ref->expression() = tsParseModuleSpecifier();
        expect(tt::parenR);
        node->moduleReference() = finishNode(ref, NodeType::TSExternalModuleReference);
    } else {
        node->moduleReference() = tsParseEntityName(false);
    }
    semicolon();
    if (isTypeOnly) node->SetTs(kTsTypeOnly, true);
    return finishNode(node, NodeType::TSImportEqualsDeclaration);
}

Node* TsParser::parseExport(Node* node, std::vector<std::string>* exports) {
    return tsParseExportDeclaration(node, exports);
}

// At `export` when it is not a modifier: `export =`, `export default
// <expression>`, `export as namespace`, `export {...}`, `export *`.
Node* TsParser::tsParseExportDeclaration(Node* node, std::vector<std::string>*) {
    expect(tt::_export);
    if (eat(tt::eq)) {
        node->expression() = parseMaybeAssign();
        semicolon();
        return finishNode(node, NodeType::TSExportAssignment);
    }
    if (eat(tt::_default)) {
        // tsc: an expression (declarations went the modifier way)
        bool isAsync = isWord("async") && lookAhead([&] {
                           next();
                           return type == &tt::_function && !hasPrecedingLineBreak();
                       });
        if (type == &tt::_function || isAsync || type == &tt::_class) {
            Node* inner = startNode();
            if (type == &tt::_class) {
                node->declaration() = tsParseClassTs(inner, true, nullptr);
            } else {
                if (isAsync) next();
                next();
                node->declaration() = tsParseFunctionTs(inner, true, isAsync, true, false);
            }
        } else {
            node->declaration() = parseMaybeAssign();
            semicolon();
        }
        return finishNode(node, NodeType::ExportDefaultDeclaration);
    }
    if (isWord("as")) {
        next();
        expectContextual("namespace");
        if (!isIdentToken()) unexpected();
        node->id() = parseIdent();
        semicolon();
        return finishNode(node, NodeType::TSNamespaceExportDeclaration);
    }
    bool typeOnly = false;
    if (isWord("type") && lookAhead([&] {
            next();
            return type == &tt::star || type == &tt::braceL;
        })) {
        next();
        typeOnly = true;
    }
    if (typeOnly) node->SetTs(kTsTypeOnly, true);
    auto parseSource = [&]() {
        node->source() = tsParseModuleSpecifier();
        node->attributes() = tsParseImportAttributes();
    };
    if (eat(tt::star)) {
        node->exported() = isWord("as") ? (next(), tsParseModuleExportName()) : nullptr;
        expectContextual("from");
        parseSource();
        semicolon();
        return finishNode(node, NodeType::ExportAllDeclaration);
    }
    std::vector<Node*> specs;
    expect(tt::braceL);
    bool first = true;
    while (!eat(tt::braceR)) {
        if (!first) {
            expect(tt::comma);
            if (eat(tt::braceR)) break;
        }
        first = false;
        specs.push_back(tsParseImportOrExportSpecifier(false));
    }
    node->declaration() = nullptr;
    node->specifiers() = list(specs);
    node->source() = nullptr;
    node->attributes() = {};
    if (isWord("from") || (type == &tt::string && !hasPrecedingLineBreak())) {
        expectContextual("from");
        parseSource();
    }
    semicolon();
    return finishNode(node, NodeType::ExportNamedDeclaration);
}

}  // namespace jsparse
