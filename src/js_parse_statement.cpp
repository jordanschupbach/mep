// Port of acorn 8.15.0 (MIT): statement parsing (statement.js), lvalues
// (lval.js) and mep's error recovery. See js_parse_parser.h.

#include <algorithm>
#include <cstring>
#include <string>

#include "js_parse_parser.h"

namespace jsparse {

namespace {

// acorn's `context + "label"` for statement contexts.
const char* WithLabel(const char* context) {
    if (!context) return "label";
    if (std::strstr(context, "label")) return context;
    if (!std::strcmp(context, "if")) return "iflabel";
    if (!std::strcmp(context, "do")) return "dolabel";
    if (!std::strcmp(context, "while")) return "whilelabel";
    if (!std::strcmp(context, "for")) return "forlabel";
    if (!std::strcmp(context, "with")) return "withlabel";
    return "label";
}

// checkKeyName
bool CheckKeyName(Node* node, std::string_view name) {
    if (node->Has(kFlagComputed)) return false;
    Node* key = node->key();
    return (key->type == NodeType::Identifier && key->str == name) ||
           (key->type == NodeType::Literal && key->lit == LitKind::String && key->str2 == name);
}

// isPrivateNameConflicted
bool IsPrivateNameConflicted(PrivateNameScope& map, Node* element, Arena& arena) {
    std::string_view name = element->key()->str;
    std::string_view* curr = nullptr;
    for (auto& d : map.declared)
        if (d.first == name) curr = &d.second;
    std::string next = "true";
    if (element->type == NodeType::MethodDefinition && (element->kind == "get" || element->kind == "set"))
        next = std::string(element->Has(kFlagStatic) ? "s" : "i") + std::string(element->kind);
    if (curr && ((*curr == "iget" && next == "iset") || (*curr == "iset" && next == "iget") ||
                 (*curr == "sget" && next == "sset") || (*curr == "sset" && next == "sget"))) {
        *curr = "true";
        return false;
    }
    if (!curr) {
        map.declared.emplace_back(name, arena.Str(next));
        return false;
    }
    return true;
}

}  // namespace

namespace {
bool StartsStatement(const TokenType* t, const std::string& value);
}  // namespace

// ---------------------------------------------------------------- top level

Node* Parser::parse() {
    Node* node = startNode();
    if (!recover) {
        nextToken();
        return parseTopLevel(node);
    }
    // The first token has no statement around it to recover in: skip what
    // the tokenizer rejects until it reads one.
    bool first = true;
    for (;;) {
        int before = pos;
        try {
            nextToken();
            break;
        } catch (const SyntaxErr& e) {
            if (first) recordError(e);
            first = false;
            bool runaway = e.message == "Unterminated template" || e.message == "Unterminated comment" ||
                           e.message == "Unterminated JSX contents";
            int resume = runaway ? inputLen : std::min(inputLen, std::max(start + 1, before + 1));
            Pos lp = lineInfo(resume);
            pos = resume;
            curLine = static_cast<int>(lp.line);
            lineStart = resume - static_cast<int>(lp.column);
        }
    }
    return parseTopLevel(node);
}

Node* Parser::parseRecording() {
    try {
        return parse();
    } catch (const SyntaxErr& e) {
        recordError(e);
        return nullptr;
    }
}

Node* Parser::parseTopLevel(Node* node) {
    std::vector<std::string> exports;
    std::vector<Node*> body;
    while (type != &tt::eof) {
        Node* stmt = parseStatementRecovering(nullptr, true, &exports);
        if (stmt) body.push_back(stmt);
    }
    if (inModule && !undefinedExports.empty()) {
        auto& first = undefinedExports.front();
        SyntaxErr e{"Export '" + std::string(first.first) + "' is not defined", static_cast<int>(first.second->ustart),
                    pos};
        if (!recover) throw e;
        recordError(e);
    }
    node->body_list() = list(body);
    adaptDirectivePrologue(node->body_list());
    next();
    node->kind = sourceType == SourceType::Module ? "module" : "script";
    return finishNode(node, NodeType::Program);
}

bool Parser::isLet(const char* ctx) {
    if (ecmaVersion < 6 || !isContextual("let")) return false;
    int nx = skipWhiteSpaceAt(pos);
    int nextCh = charAt(nx);
    if (nextCh == 91 || nextCh == 92) return true;
    if (ctx) return false;
    if (nextCh == 123 || (nextCh > 0xd7ff && nextCh < 0xdc00)) return true;
    if (IsIdentifierStart(nextCh, true)) {
        int p = nx + 1;
        while (IsIdentifierChar(nextCh = charAt(p), true)) ++p;
        if (nextCh == 92 || (nextCh > 0xd7ff && nextCh < 0xdc00)) return true;
        std::u16string_view ident = slice(nx, p);
        if (ident != u"in" && ident != u"instanceof") return true;
    }
    return false;
}

bool Parser::isAsyncFunction() {
    if (ecmaVersion < 8 || !isContextual("async")) return false;
    int nx = skipWhiteSpaceAt(pos);
    if (lineBreakIn(pos, nx)) return false;
    if (slice(nx, nx + 8) != u"function") return false;
    if (nx + 8 == inputLen) return true;
    int after = charAt(nx + 8);
    return !(IsIdentifierChar(after, true) || (after > 0xd7ff && after < 0xdc00));
}

bool Parser::isUsingKeyword(bool isAwaitUsing, bool isFor) {
    if (ecmaVersion < 17 || !isContextual(isAwaitUsing ? "await" : "using")) return false;
    int nx = skipWhiteSpaceAt(pos);
    if (lineBreakIn(pos, nx)) return false;
    if (isAwaitUsing) {
        int awaitEndPos = nx + 5;
        int after;
        if (slice(nx, awaitEndPos) != u"using" || awaitEndPos == inputLen ||
            IsIdentifierChar(after = charAt(awaitEndPos), true) || (after > 0xd7ff && after < 0xdc00))
            return false;
        int skipEnd = skipWhiteSpaceAt(awaitEndPos);
        if (lineBreakIn(awaitEndPos, skipEnd)) return false;
    }
    if (isFor) {
        int ofEndPos = nx + 2;
        if (slice(nx, ofEndPos) == u"of") {
            int after = charAt(ofEndPos);
            if (ofEndPos == inputLen || (!IsIdentifierChar(after, true) && !(after > 0xd7ff && after < 0xdc00)))
                return false;
        }
    }
    int ch = charAt(nx);
    return IsIdentifierStart(ch, true) || ch == 92;
}

Node* Parser::parseStatement(const char* ctx, bool topLevel, std::vector<std::string>* exports) {
    DepthGuard guard(*this);
    const TokenType* starttype = type;
    Node* node = startNode();
    std::string kind;
    if (isLet(ctx)) {
        starttype = &tt::_var;
        kind = "let";
    }
    if (starttype == &tt::_break || starttype == &tt::_continue)
        return parseBreakContinueStatement(node, starttype->keyword);
    if (starttype == &tt::_debugger) return parseDebuggerStatement(node);
    if (starttype == &tt::_do) return parseDoStatement(node);
    if (starttype == &tt::_for) return parseForStatement(node);
    if (starttype == &tt::_function) {
        if ((ctx && (strict || (std::strcmp(ctx, "if") != 0 && std::strcmp(ctx, "label") != 0))) && ecmaVersion >= 6)
            unexpected();
        return parseFunctionStatement(node, false, !ctx);
    }
    if (starttype == &tt::_class) {
        if (ctx) unexpected();
        return parseClass(node, 1);
    }
    if (starttype == &tt::_if) return parseIfStatement(node);
    if (starttype == &tt::_return) return parseReturnStatement(node);
    if (starttype == &tt::_switch) return parseSwitchStatement(node);
    if (starttype == &tt::_throw) return parseThrowStatement(node);
    if (starttype == &tt::_try_) return parseTryStatement(node);
    if (starttype == &tt::_const || starttype == &tt::_var) {
        if (kind.empty()) kind = value;
        if (ctx && kind != "var") unexpected();
        return parseVarStatement(node, str(kind));
    }
    if (starttype == &tt::_while) return parseWhileStatement(node);
    if (starttype == &tt::_with) return parseWithStatement(node);
    if (starttype == &tt::braceL) return parseBlock(true, node);
    if (starttype == &tt::semi) return parseEmptyStatement(node);
    if (starttype == &tt::_export || starttype == &tt::_import) {
        if (ecmaVersion > 10 && starttype == &tt::_import) {
            int nx = skipWhiteSpaceAt(pos);
            int nextCh = charAt(nx);
            if (nextCh == 40 || nextCh == 46) return parseExpressionStatement(node, parseExpression());
        }
        if (!allowImportExportEverywhere) {
            if (!topLevel) raise(start, "'import' and 'export' may only appear at the top level");
            if (!inModule) raise(start, "'import' and 'export' may appear only with 'sourceType: module'");
        }
        return starttype == &tt::_import ? parseImport(node) : parseExport(node, exports);
    }
    if (isAsyncFunction()) {
        if (ctx) unexpected();
        next();
        return parseFunctionStatement(node, true, !ctx);
    }
    const char* usingKind = isAwaitUsing(false) ? "await using" : isUsing(false) ? "using" : nullptr;
    if (usingKind) {
        if (topLevel && sourceType == SourceType::Script)
            raise(start, "Using declaration cannot appear in the top level when source type is `script`");
        if (!std::strcmp(usingKind, "await using")) {
            if (!canAwait()) raise(start, "Await using cannot appear outside of async function");
            next();
        }
        next();
        parseVar(node, false, usingKind);
        semicolon();
        return finishNode(node, NodeType::VariableDeclaration);
    }
    std::string maybeName = value;
    Node* expr = parseExpression();
    if (starttype == &tt::name && expr->type == NodeType::Identifier && eat(tt::colon))
        return parseLabeledStatement(node, maybeName, expr, ctx);
    return parseExpressionStatement(node, expr);
}

Node* Parser::parseBreakContinueStatement(Node* node, const char* keyword) {
    bool isBreak = !std::strcmp(keyword, "break");
    next();
    if (eat(tt::semi) || insertSemicolon()) {
        node->label() = nullptr;
    } else if (type != &tt::name) {
        unexpected();
    } else {
        node->label() = parseIdent();
        semicolon();
    }
    size_t i = 0;
    for (; i < labels.size(); ++i) {
        const Label& lab = labels[i];
        if (!node->label() || lab.name == node->label()->str) {
            if (lab.kind != 0 && (isBreak || lab.kind == 1)) break;
            if (node->label() && isBreak) break;
        }
    }
    if (i == labels.size()) raise(static_cast<int>(node->ustart), std::string("Unsyntactic ") + keyword);
    return finishNode(node, isBreak ? NodeType::BreakStatement : NodeType::ContinueStatement);
}

Node* Parser::parseDebuggerStatement(Node* node) {
    next();
    semicolon();
    return finishNode(node, NodeType::DebuggerStatement);
}

Node* Parser::parseDoStatement(Node* node) {
    next();
    labels.push_back(Label{{}, 1, -1});
    node->body() = parseStatement("do");
    labels.pop_back();
    expect(tt::_while);
    node->test() = parseParenExpression();
    if (ecmaVersion >= 6)
        eat(tt::semi);
    else
        semicolon();
    return finishNode(node, NodeType::DoWhileStatement);
}

Node* Parser::parseForStatement(Node* node) {
    next();
    int awaitAt = (ecmaVersion >= 9 && canAwait() && eatContextual("await")) ? lastTokStart : -1;
    labels.push_back(Label{{}, 1, -1});
    enterScope(0);
    expect(tt::parenL);
    if (type == &tt::semi) {
        if (awaitAt > -1) unexpected(awaitAt);
        return parseFor(node, nullptr);
    }
    bool let = isLet();
    if (type == &tt::_var || type == &tt::_const || let) {
        Node* init1 = startNode();
        std::string kind = let ? "let" : value;
        next();
        parseVar(init1, true, str(kind));
        finishNode(init1, NodeType::VariableDeclaration);
        return parseForAfterInit(node, init1, awaitAt);
    }
    bool startsWithLet = isContextual("let"), isForOf = false;
    const char* usingKind = isUsing(true) ? "using" : isAwaitUsing(true) ? "await using" : nullptr;
    if (usingKind) {
        Node* init2 = startNode();
        next();
        if (!std::strcmp(usingKind, "await using")) next();
        parseVar(init2, true, usingKind);
        finishNode(init2, NodeType::VariableDeclaration);
        return parseForAfterInit(node, init2, awaitAt);
    }
    bool hadEsc = containsEsc;
    DestructuringErrors refDE;
    int initPos = start;
    Node* init = awaitAt > -1 ? parseExprSubscripts(&refDE, ForInit::Await) : parseExpression(ForInit::Yes, &refDE);
    if (type == &tt::_in || (isForOf = ecmaVersion >= 6 && isContextual("of"))) {
        if (awaitAt > -1) {
            if (type == &tt::_in) unexpected(awaitAt);
            node->Set(kFlagAwait, true);
        } else if (isForOf && ecmaVersion >= 8) {
            if (static_cast<int>(init->ustart) == initPos && !hadEsc && init->type == NodeType::Identifier &&
                init->str == "async")
                unexpected();
            else if (ecmaVersion >= 9)
                node->Set(kFlagAwait, false);
        }
        if (startsWithLet && isForOf)
            raise(static_cast<int>(init->ustart), "The left-hand side of a for-of loop may not start with 'let'.");
        toAssignable(init, false, &refDE);
        checkLValPattern(init);
        return parseForIn(node, init);
    }
    checkExpressionErrors(&refDE, true);
    if (awaitAt > -1) unexpected(awaitAt);
    return parseFor(node, init);
}

Node* Parser::parseForAfterInit(Node* node, Node* init, int awaitAt) {
    if ((type == &tt::_in || (ecmaVersion >= 6 && isContextual("of"))) && init->declarations().size() == 1) {
        if (ecmaVersion >= 9) {
            if (type == &tt::_in) {
                if (awaitAt > -1) unexpected(awaitAt);
            } else {
                node->Set(kFlagAwait, awaitAt > -1);
            }
        }
        return parseForIn(node, init);
    }
    if (awaitAt > -1) unexpected(awaitAt);
    return parseFor(node, init);
}

Node* Parser::parseFunctionStatement(Node* node, bool isAsync, bool declarationPosition) {
    next();
    return parseFunction(node, FUNC_STATEMENT | (declarationPosition ? 0 : FUNC_HANGING_STATEMENT), false, isAsync);
}

Node* Parser::parseIfStatement(Node* node) {
    next();
    node->test() = parseParenExpression();
    node->consequent() = parseStatement("if");
    node->alternate() = eat(tt::_else) ? parseStatement("if") : nullptr;
    return finishNode(node, NodeType::IfStatement);
}

Node* Parser::parseReturnStatement(Node* node) {
    if (!inFunction() && !allowReturnOutsideFunction) raise(start, "'return' outside of function");
    next();
    if (eat(tt::semi) || insertSemicolon()) {
        node->argument() = nullptr;
    } else {
        node->argument() = parseExpression();
        semicolon();
    }
    return finishNode(node, NodeType::ReturnStatement);
}

Node* Parser::parseSwitchStatement(Node* node) {
    next();
    node->discriminant() = parseParenExpression();
    std::vector<Node*> cases;
    expect(tt::braceL);
    labels.push_back(Label{{}, 2, -1});
    enterScope(0);
    Node* cur = nullptr;
    std::vector<Node*> consequent;
    bool sawDefault = false;
    auto finishCase = [&]() {
        cur->consequent_list() = list(consequent);
        consequent.clear();
        finishNode(cur, NodeType::SwitchCase);
    };
    while (type != &tt::braceR) {
        if (recover && sawErrors && type == &tt::eof) break;
        if (type == &tt::_case || type == &tt::_default) {
            bool isCase = type == &tt::_case;
            if (cur) finishCase();
            cases.push_back(cur = startNode());
            next();
            if (isCase) {
                cur->test() = parseExpression();
            } else {
                if (sawDefault) raiseRecoverable(lastTokStart, "Multiple default clauses");
                sawDefault = true;
                cur->test() = nullptr;
            }
            expect(tt::colon);
        } else {
            if (!cur) unexpected();
            Node* stmt = parseStatementRecovering(nullptr, false, nullptr);
            if (stmt) consequent.push_back(stmt);
        }
    }
    exitScope();
    if (cur) finishCase();
    next();
    labels.pop_back();
    node->cases() = list(cases);
    return finishNode(node, NodeType::SwitchStatement);
}

Node* Parser::parseThrowStatement(Node* node) {
    next();
    if (lineBreakIn(lastTokEnd, start)) raise(lastTokEnd, "Illegal newline after throw");
    node->argument() = parseExpression();
    semicolon();
    return finishNode(node, NodeType::ThrowStatement);
}

Node* Parser::parseCatchClauseParam() {
    Node* param = parseBindingAtom();
    bool simple = param->type == NodeType::Identifier;
    enterScope(simple ? SCOPE_SIMPLE_CATCH : 0);
    checkLValPattern(param, simple ? BIND_SIMPLE_CATCH : BIND_LEXICAL);
    expect(tt::parenR);
    return param;
}

Node* Parser::parseTryStatement(Node* node) {
    next();
    node->block() = parseBlock();
    node->handler() = nullptr;
    if (type == &tt::_catch) {
        Node* clause = startNode();
        next();
        if (eat(tt::parenL)) {
            clause->param() = parseCatchClauseParam();
        } else {
            if (ecmaVersion < 10) unexpected();
            clause->param() = nullptr;
            enterScope(0);
        }
        clause->body() = parseBlock(false);
        exitScope();
        node->handler() = finishNode(clause, NodeType::CatchClause);
    }
    node->finalizer() = eat(tt::_finally_) ? parseBlock() : nullptr;
    if (!node->handler() && !node->finalizer())
        raise(static_cast<int>(node->ustart), "Missing catch or finally clause");
    return finishNode(node, NodeType::TryStatement);
}

Node* Parser::parseVarStatement(Node* node, std::string_view kind, bool allowMissingInitializer) {
    next();
    parseVar(node, false, kind, allowMissingInitializer);
    semicolon();
    return finishNode(node, NodeType::VariableDeclaration);
}

Node* Parser::parseWhileStatement(Node* node) {
    next();
    node->test() = parseParenExpression();
    labels.push_back(Label{{}, 1, -1});
    node->body() = parseStatement("while");
    labels.pop_back();
    return finishNode(node, NodeType::WhileStatement);
}

Node* Parser::parseWithStatement(Node* node) {
    if (strict) raise(start, "'with' in strict mode");
    next();
    node->object() = parseParenExpression();
    node->body() = parseStatement("with");
    return finishNode(node, NodeType::WithStatement);
}

Node* Parser::parseEmptyStatement(Node* node) {
    next();
    return finishNode(node, NodeType::EmptyStatement);
}

Node* Parser::parseLabeledStatement(Node* node, const std::string& maybeName, Node* expr, const char* ctx) {
    for (const Label& label : labels)
        if (label.name == maybeName)
            raise(static_cast<int>(expr->ustart), "Label '" + maybeName + "' is already declared");
    int kind = type->isLoop ? 1 : type == &tt::_switch ? 2 : 0;
    for (size_t i = labels.size(); i-- > 0;) {
        Label& label = labels[i];
        if (label.statementStart == static_cast<int>(node->ustart)) {
            label.statementStart = start;
            label.kind = kind;
        } else {
            break;
        }
    }
    labels.push_back(Label{str(maybeName), kind, start});
    node->body() = parseStatement(WithLabel(ctx));
    labels.pop_back();
    node->label() = expr;
    return finishNode(node, NodeType::LabeledStatement);
}

Node* Parser::parseExpressionStatement(Node* node, Node* expr) {
    node->expression() = expr;
    semicolon();
    return finishNode(node, NodeType::ExpressionStatement);
}

Node* Parser::parseBlock(bool createNewLexicalScope, Node* node, bool exitStrict) {
    if (!node) node = startNode();
    std::vector<Node*> body;
    // Recovery: after an error inside this block, a statement keyword that
    // starts a line at or left of the indentation of the line the `{` is on
    // closes the block (its `}` is probably what went missing).
    const int bracePos = start;
    int openIndent = -1;  // computed when first needed (it walks the line)
    const size_t ctxDepth = context.size();
    const int errorsBefore = sawErrors;
    bool implicitClose = false;
    expect(tt::braceL);
    if (createNewLexicalScope) enterScope(0);
    while (type != &tt::braceR) {
        if (recover && sawErrors && type == &tt::eof) break;
        if (recover && sawErrors > errorsBefore && lineBreakIn(lastTokEnd, start) && StartsStatement(type, value)) {
            if (openIndent < 0) openIndent = lineIndentAt(bracePos);
            if (static_cast<int>(startLoc.column) <= openIndent) {
                implicitClose = true;
                break;
            }
        }
        Node* stmt = parseStatementRecovering(nullptr, false, nullptr);
        if (stmt) body.push_back(stmt);
    }
    if (exitStrict) strict = false;
    if (implicitClose) {
        // Undo the `{`'s token context as its `}` would have, under what
        // the current token pushed.
        size_t keep = std::min(ctxDepth, context.size());
        std::vector<const TokContext*> pushed(context.begin() + static_cast<std::ptrdiff_t>(keep), context.end());
        context.resize(keep);
        if (context.size() > 1) {
            const TokContext* o = context.back();
            context.pop_back();
            if (o == &tc::b_stat && context.size() > 1 && std::strcmp(context.back()->token, "function") == 0)
                context.pop_back();
        }
        context.insert(context.end(), pushed.begin(), pushed.end());
    } else {
        next();
    }
    if (createNewLexicalScope) exitScope();
    node->body_list() = list(body);
    return finishNode(node, NodeType::BlockStatement);
}

Node* Parser::parseFor(Node* node, Node* init) {
    node->init() = init;
    expect(tt::semi);
    node->test() = type == &tt::semi ? nullptr : parseExpression();
    expect(tt::semi);
    node->update() = type == &tt::parenR ? nullptr : parseExpression();
    expect(tt::parenR);
    node->body() = parseStatement("for");
    exitScope();
    labels.pop_back();
    return finishNode(node, NodeType::ForStatement);
}

Node* Parser::parseForIn(Node* node, Node* init) {
    bool isForIn = type == &tt::_in;
    next();
    if (init->type == NodeType::VariableDeclaration && init->declarations()[0]->init() &&
        (!isForIn || ecmaVersion < 8 || strict || init->kind != "var" ||
         init->declarations()[0]->id()->type != NodeType::Identifier))
        raise(static_cast<int>(init->ustart), std::string(isForIn ? "for-in" : "for-of") +
                                                   " loop variable declaration may not have an initializer");
    node->left() = init;
    node->right() = isForIn ? parseExpression() : parseMaybeAssign();
    expect(tt::parenR);
    node->body() = parseStatement("for");
    exitScope();
    labels.pop_back();
    return finishNode(node, isForIn ? NodeType::ForInStatement : NodeType::ForOfStatement);
}

Node* Parser::parseVar(Node* node, bool isFor, std::string_view kind, bool allowMissingInitializer) {
    std::vector<Node*> decls;
    node->kind = kind;
    for (;;) {
        Node* decl = startNode();
        parseVarId(decl, kind);
        if (eat(tt::eq)) {
            decl->init() = parseMaybeAssign(isFor ? ForInit::Yes : ForInit::No);
        } else if (!allowMissingInitializer && kind == "const" &&
                   !(type == &tt::_in || (ecmaVersion >= 6 && isContextual("of")))) {
            unexpected();
        } else if (!allowMissingInitializer && (kind == "using" || kind == "await using") && ecmaVersion >= 17 &&
                   type != &tt::_in && !isContextual("of")) {
            raise(lastTokEnd, "Missing initializer in " + std::string(kind) + " declaration");
        } else if (!allowMissingInitializer && decl->id()->type != NodeType::Identifier &&
                   !(isFor && (type == &tt::_in || isContextual("of")))) {
            raise(lastTokEnd, "Complex binding patterns require an initialization value");
        } else {
            decl->init() = nullptr;
        }
        decls.push_back(finishNode(decl, NodeType::VariableDeclarator));
        node->declarations() = list(decls);
        if (!eat(tt::comma)) break;
    }
    node->declarations() = list(decls);
    return node;
}

void Parser::parseVarId(Node* decl, std::string_view kind) {
    decl->id() = kind == "using" || kind == "await using" ? parseIdent() : parseBindingAtom();
    checkLValPattern(decl->id(), kind == "var" ? BIND_VAR : BIND_LEXICAL, nullptr);
}

Node* Parser::parseFunction(Node* node, int statement, bool allowExpressionBody, bool isAsync, ForInit forInit) {
    initFunction(node);
    if (ecmaVersion >= 9 || (ecmaVersion >= 6 && !isAsync)) {
        if (type == &tt::star && (statement & FUNC_HANGING_STATEMENT)) unexpected();
        node->Set(kFlagGenerator, eat(tt::star));
    }
    if (ecmaVersion >= 8) node->Set(kFlagAsync, isAsync);
    if (statement & FUNC_STATEMENT) {
        node->id() = (statement & FUNC_NULLABLE_ID) && type != &tt::name ? nullptr : parseIdent();
        if (node->id() && !(statement & FUNC_HANGING_STATEMENT))
            checkLValSimple(node->id(), (strict || node->Has(kFlagGenerator) || node->Has(kFlagAsync))
                                            ? (treatFunctionsAsVar() ? BIND_VAR : BIND_LEXICAL)
                                            : BIND_FUNCTION);
    }
    int oldYieldPos = yieldPos, oldAwaitPos = awaitPos, oldAwaitIdentPos = awaitIdentPos;
    yieldPos = 0;
    awaitPos = 0;
    awaitIdentPos = 0;
    enterScope(SCOPE_FUNCTION | (node->Has(kFlagAsync) ? SCOPE_ASYNC : 0) |
               (node->Has(kFlagGenerator) ? SCOPE_GENERATOR : 0));
    if (!(statement & FUNC_STATEMENT)) node->id() = type == &tt::name ? parseIdent() : nullptr;
    parseFunctionParams(node);
    parseFunctionBody(node, allowExpressionBody, false, forInit);
    yieldPos = oldYieldPos;
    awaitPos = oldAwaitPos;
    awaitIdentPos = oldAwaitIdentPos;
    return finishNode(node, (statement & FUNC_STATEMENT) ? NodeType::FunctionDeclaration : NodeType::FunctionExpression);
}

void Parser::parseFunctionParams(Node* node) {
    expect(tt::parenL);
    node->params() = list(parseBindingList(tt::parenR, false, ecmaVersion >= 8));
    checkYieldAwaitInDefaultParams();
}

Node* Parser::parseClass(Node* node, int isStatement) {
    next();
    bool oldStrict = strict;
    strict = true;
    parseClassId(node, isStatement);
    parseClassSuper(node);
    enterClassBody();
    size_t privateIndex = privateNameStack.size() - 1;
    Node* classBody = startNode();
    bool hadConstructor = false;
    std::vector<Node*> body;
    expect(tt::braceL);
    while (type != &tt::braceR) {
        if (recover && sawErrors && type == &tt::eof) break;
        Snapshot snap;
        if (recover) snap = takeSnapshot();
        try {
            Node* element = parseClassElement(node->superClass() != nullptr);
            if (element) {
                body.push_back(element);
                if (element->type == NodeType::MethodDefinition && element->kind == "constructor") {
                    if (hadConstructor)
                        raiseRecoverable(static_cast<int>(element->ustart), "Duplicate constructor in the same class");
                    hadConstructor = true;
                } else if (element->key() && element->key()->type == NodeType::PrivateIdentifier &&
                           IsPrivateNameConflicted(privateNameStack[privateIndex], element, arena)) {
                    raiseRecoverable(static_cast<int>(element->key()->ustart),
                                     "Identifier '#" + std::string(element->key()->str) + "' has already been declared");
                }
            }
        } catch (const SyntaxErr& e) {
            if (!recover) throw;
            int errAt = start;
            recordError(e);
            resync(snap, std::max(errAt, e.pos), true, false);
        }
    }
    strict = oldStrict;
    next();
    classBody->body_list() = list(body);
    node->body() = finishNode(classBody, NodeType::ClassBody);
    exitClassBody();
    return finishNode(node, isStatement ? NodeType::ClassDeclaration : NodeType::ClassExpression);
}

Node* Parser::parseClassElement(bool constructorAllowsSuper) {
    if (eat(tt::semi)) return nullptr;
    DepthGuard guard(*this);
    Node* node = startNode();
    std::string keyName;
    bool isGenerator = false, isAsync = false, isStatic = false;
    std::string kind = "method";
    if (eatContextual("static")) {
        if (ecmaVersion >= 13 && eat(tt::braceL)) {
            parseClassStaticBlock(node);
            return node;
        }
        if (isClassElementNameStart() || type == &tt::star)
            isStatic = true;
        else
            keyName = "static";
    }
    node->Set(kFlagStatic, isStatic);
    if (keyName.empty() && ecmaVersion >= 8 && eatContextual("async")) {
        if ((isClassElementNameStart() || type == &tt::star) && !canInsertSemicolon())
            isAsync = true;
        else
            keyName = "async";
    }
    if (keyName.empty() && (ecmaVersion >= 9 || !isAsync) && eat(tt::star)) isGenerator = true;
    if (keyName.empty() && !isAsync && !isGenerator) {
        std::string lastValue = value;
        if (eatContextual("get") || eatContextual("set")) {
            if (isClassElementNameStart())
                kind = lastValue;
            else
                keyName = lastValue;
        }
    }
    if (!keyName.empty()) {
        node->Set(kFlagComputed, false);
        Node* key = startNodeAt(lastTokStart, lastTokStartLoc);
        key->str = str(keyName);
        node->key() = finishNode(key, NodeType::Identifier);
    } else {
        parseClassElementName(node);
    }
    if (ecmaVersion < 13 || type == &tt::parenL || kind != "method" || isGenerator || isAsync) {
        bool isConstructor = !node->Has(kFlagStatic) && CheckKeyName(node, "constructor");
        bool allowsDirectSuper = isConstructor && constructorAllowsSuper;
        if (isConstructor && kind != "method")
            raise(static_cast<int>(node->key()->ustart), "Constructor can't have get/set modifier");
        node->kind = isConstructor ? "constructor" : kind == "get" ? "get" : kind == "set" ? "set" : "method";
        parseClassMethod(node, isGenerator, isAsync, allowsDirectSuper);
    } else {
        parseClassField(node);
    }
    return node;
}

bool Parser::isClassElementNameStart() {
    return type == &tt::name || type == &tt::privateId || type == &tt::num || type == &tt::string ||
           type == &tt::bracketL || type->keyword;
}

void Parser::parseClassElementName(Node* element) {
    if (type == &tt::privateId) {
        if (value == "constructor") raise(start, "Classes can't have an element named '#constructor'");
        element->Set(kFlagComputed, false);
        element->key() = parsePrivateIdent();
    } else {
        parsePropertyName(element);
    }
}

Node* Parser::parseClassMethod(Node* method, bool isGenerator, bool isAsync, bool allowsDirectSuper) {
    Node* key = method->key();
    if (method->kind == "constructor") {
        if (isGenerator) raise(static_cast<int>(key->ustart), "Constructor can't be a generator");
        if (isAsync) raise(static_cast<int>(key->ustart), "Constructor can't be an async method");
    } else if (method->Has(kFlagStatic) && CheckKeyName(method, "prototype")) {
        raise(static_cast<int>(key->ustart), "Classes may not have a static property named prototype");
    }
    Node* v = method->value() = parseMethod(isGenerator, isAsync, allowsDirectSuper);
    if (method->kind == "get" && v->params().size() != 0)
        raiseRecoverable(static_cast<int>(v->ustart), "getter should have no params");
    if (method->kind == "set" && v->params().size() != 1)
        raiseRecoverable(static_cast<int>(v->ustart), "setter should have exactly one param");
    if (method->kind == "set" && !v->params().empty() && v->params()[0]->type == NodeType::RestElement)
        raiseRecoverable(static_cast<int>(v->params()[0]->ustart), "Setter cannot use rest params");
    return finishNode(method, NodeType::MethodDefinition);
}

Node* Parser::parseClassField(Node* field) {
    if (CheckKeyName(field, "constructor"))
        raise(static_cast<int>(field->key()->ustart), "Classes can't have a field named 'constructor'");
    else if (field->Has(kFlagStatic) && CheckKeyName(field, "prototype"))
        raise(static_cast<int>(field->key()->ustart), "Classes can't have a static field named 'prototype'");
    if (eat(tt::eq)) {
        enterScope(SCOPE_CLASS_FIELD_INIT | SCOPE_SUPER);
        field->value() = parseMaybeAssign();
        exitScope();
    } else {
        field->value() = nullptr;
    }
    semicolon();
    return finishNode(field, NodeType::PropertyDefinition);
}

Node* Parser::parseClassStaticBlock(Node* node) {
    std::vector<Node*> body;
    std::vector<Label> oldLabels = std::move(labels);
    labels.clear();
    enterScope(SCOPE_CLASS_STATIC_BLOCK | SCOPE_SUPER);
    while (type != &tt::braceR) {
        if (recover && sawErrors && type == &tt::eof) break;
        Node* stmt = parseStatementRecovering(nullptr, false, nullptr);
        if (stmt) body.push_back(stmt);
    }
    next();
    exitScope();
    labels = std::move(oldLabels);
    node->body_list() = list(body);
    return finishNode(node, NodeType::StaticBlock);
}

void Parser::parseClassId(Node* node, int isStatement) {
    if (type == &tt::name) {
        node->id() = parseIdent();
        if (isStatement) checkLValSimple(node->id(), BIND_LEXICAL, nullptr);
    } else {
        if (isStatement == 1) unexpected();
        node->id() = nullptr;
    }
}

void Parser::parseClassSuper(Node* node) {
    node->superClass() = eat(tt::_extends) ? parseExprSubscripts(nullptr, ForInit::No) : nullptr;
}

PrivateNameScope& Parser::enterClassBody() {
    privateNameStack.emplace_back();
    return privateNameStack.back();
}

void Parser::exitClassBody() {
    PrivateNameScope ref = std::move(privateNameStack.back());
    privateNameStack.pop_back();
    if (!checkPrivateFields) return;
    PrivateNameScope* parent = privateNameStack.empty() ? nullptr : &privateNameStack.back();
    for (Node* id : ref.used) {
        bool declared = false;
        for (auto& d : ref.declared)
            if (d.first == id->str) declared = true;
        if (!declared) {
            if (parent)
                parent->used.push_back(id);
            else
                raiseRecoverable(static_cast<int>(id->ustart),
                                 "Private field '#" + std::string(id->str) + "' must be declared in an enclosing class");
        }
    }
}

// ---------------------------------------------------------------- modules

Node* Parser::parseExportAllDeclaration(Node* node, std::vector<std::string>* exports) {
    if (ecmaVersion >= 11) {
        if (eatContextual("as")) {
            node->exported() = parseModuleExportName();
            checkExportNode(exports, node->exported(), lastTokStart);
        } else {
            node->exported() = nullptr;
        }
    }
    expectContextual("from");
    if (type != &tt::string) unexpected();
    node->source() = parseExprAtom();
    if (ecmaVersion >= 16) node->attributes() = parseWithClause();
    semicolon();
    return finishNode(node, NodeType::ExportAllDeclaration);
}

Node* Parser::parseExport(Node* node, std::vector<std::string>* exports) {
    next();
    if (eat(tt::star)) return parseExportAllDeclaration(node, exports);
    if (eat(tt::_default)) {
        checkExport(exports, "default", lastTokStart);
        node->declaration() = parseExportDefaultDeclaration();
        return finishNode(node, NodeType::ExportDefaultDeclaration);
    }
    if (shouldParseExportStatement()) {
        Node* decl = node->declaration() = parseExportDeclaration(node);
        if (decl->type == NodeType::VariableDeclaration)
            checkVariableExport(exports, decl->declarations());
        else
            checkExportNode(exports, decl->id(), static_cast<int>(decl->id()->ustart));
        node->specifiers() = {};
        node->source() = nullptr;
        node->attributes() = {};
    } else {
        node->declaration() = nullptr;
        node->specifiers() = parseExportSpecifiers(exports);
        if (eatContextual("from")) {
            if (type != &tt::string) unexpected();
            node->source() = parseExprAtom();
            if (ecmaVersion >= 16) node->attributes() = parseWithClause();
        } else {
            for (Node* spec : node->specifiers()) {
                if (spec->local()->type == NodeType::Literal)
                    raise(static_cast<int>(spec->local()->ustart),
                          "A string literal cannot be used as an exported binding without `from`.");
                checkUnreserved(spec->local());
                checkLocalExport(spec->local());
            }
            node->source() = nullptr;
            node->attributes() = {};
        }
        semicolon();
    }
    return finishNode(node, NodeType::ExportNamedDeclaration);
}

Node* Parser::parseExportDeclaration(Node*) { return parseStatement(nullptr); }

Node* Parser::parseExportDefaultDeclaration() {
    bool isAsync = false;
    if (type == &tt::_function || (isAsync = isAsyncFunction())) {
        Node* fNode = startNode();
        next();
        if (isAsync) next();
        return parseFunction(fNode, FUNC_STATEMENT | FUNC_NULLABLE_ID, false, isAsync);
    }
    if (type == &tt::_class) {
        Node* cNode = startNode();
        return parseClass(cNode, 2);
    }
    Node* declaration = parseMaybeAssign();
    semicolon();
    return declaration;
}

void Parser::checkExport(std::vector<std::string>* exports, std::string_view name, int p) {
    if (!exports) return;
    if (std::find(exports->begin(), exports->end(), name) != exports->end())
        raiseRecoverable(p, "Duplicate export '" + std::string(name) + "'");
    exports->emplace_back(name);
}

void Parser::checkExportNode(std::vector<std::string>* exports, Node* name, int p) {
    if (!exports) return;
    checkExport(exports, name->type == NodeType::Identifier ? name->str : name->str2, p);
}

void Parser::checkPatternExport(std::vector<std::string>* exports, Node* pat) {
    switch (pat->type) {
    case NodeType::Identifier:
        checkExportNode(exports, pat, static_cast<int>(pat->ustart));
        break;
    case NodeType::ObjectPattern:
        for (Node* prop : pat->properties()) checkPatternExport(exports, prop);
        break;
    case NodeType::ArrayPattern:
        for (Node* elt : pat->elements())
            if (elt) checkPatternExport(exports, elt);
        break;
    case NodeType::Property:
        checkPatternExport(exports, pat->value());
        break;
    case NodeType::AssignmentPattern:
        checkPatternExport(exports, pat->left());
        break;
    case NodeType::RestElement:
        checkPatternExport(exports, pat->argument());
        break;
    default:
        break;
    }
}

void Parser::checkVariableExport(std::vector<std::string>* exports, const NodeList& decls) {
    if (!exports) return;
    for (Node* decl : decls) checkPatternExport(exports, decl->id());
}

bool Parser::shouldParseExportStatement() {
    const char* k = type->keyword;
    return (k && (!std::strcmp(k, "var") || !std::strcmp(k, "const") || !std::strcmp(k, "class") ||
                  !std::strcmp(k, "function"))) ||
           isLet() || isAsyncFunction();
}

Node* Parser::parseExportSpecifier(std::vector<std::string>* exports) {
    Node* node = startNode();
    node->local() = parseModuleExportName();
    node->exported() = eatContextual("as") ? parseModuleExportName() : node->local();
    checkExportNode(exports, node->exported(), static_cast<int>(node->exported()->ustart));
    return finishNode(node, NodeType::ExportSpecifier);
}

NodeList Parser::parseExportSpecifiers(std::vector<std::string>* exports) {
    std::vector<Node*> nodes;
    bool first = true;
    expect(tt::braceL);
    while (!eat(tt::braceR)) {
        if (!first) {
            expect(tt::comma);
            if (afterTrailingComma(tt::braceR)) break;
        } else {
            first = false;
        }
        nodes.push_back(parseExportSpecifier(exports));
    }
    return list(nodes);
}

Node* Parser::parseImport(Node* node) {
    next();
    if (type == &tt::string) {
        node->specifiers() = {};
        node->source() = parseExprAtom();
    } else {
        node->specifiers() = parseImportSpecifiers();
        expectContextual("from");
        if (type != &tt::string) unexpected();
        node->source() = parseExprAtom();
    }
    if (ecmaVersion >= 16) node->attributes() = parseWithClause();
    semicolon();
    return finishNode(node, NodeType::ImportDeclaration);
}

Node* Parser::parseImportSpecifier() {
    Node* node = startNode();
    node->imported() = parseModuleExportName();
    if (eatContextual("as")) {
        node->local() = parseIdent();
    } else {
        if (node->imported()->type == NodeType::Identifier) checkUnreserved(node->imported());
        node->local() = node->imported();
    }
    checkLValSimple(node->local(), BIND_LEXICAL);
    return finishNode(node, NodeType::ImportSpecifier);
}

Node* Parser::parseImportDefaultSpecifier() {
    Node* node = startNode();
    node->local() = parseIdent();
    checkLValSimple(node->local(), BIND_LEXICAL);
    return finishNode(node, NodeType::ImportDefaultSpecifier);
}

Node* Parser::parseImportNamespaceSpecifier() {
    Node* node = startNode();
    next();
    expectContextual("as");
    node->local() = parseIdent();
    checkLValSimple(node->local(), BIND_LEXICAL);
    return finishNode(node, NodeType::ImportNamespaceSpecifier);
}

NodeList Parser::parseImportSpecifiers() {
    std::vector<Node*> nodes;
    bool first = true;
    if (type == &tt::name) {
        nodes.push_back(parseImportDefaultSpecifier());
        if (!eat(tt::comma)) return list(nodes);
    }
    if (type == &tt::star) {
        nodes.push_back(parseImportNamespaceSpecifier());
        return list(nodes);
    }
    expect(tt::braceL);
    while (!eat(tt::braceR)) {
        if (!first) {
            expect(tt::comma);
            if (afterTrailingComma(tt::braceR)) break;
        } else {
            first = false;
        }
        nodes.push_back(parseImportSpecifier());
    }
    return list(nodes);
}

NodeList Parser::parseWithClause() {
    std::vector<Node*> nodes;
    if (!eat(tt::_with)) return list(nodes);
    expect(tt::braceL);
    std::vector<std::string_view> attributeKeys;
    bool first = true;
    while (!eat(tt::braceR)) {
        if (!first) {
            expect(tt::comma);
            if (afterTrailingComma(tt::braceR)) break;
        } else {
            first = false;
        }
        Node* attr = parseImportAttribute();
        std::string_view keyName = attr->key()->type == NodeType::Identifier ? attr->key()->str : attr->key()->str2;
        if (inList(attributeKeys, keyName))
            raiseRecoverable(static_cast<int>(attr->key()->ustart),
                             "Duplicate attribute key '" + std::string(keyName) + "'");
        attributeKeys.push_back(keyName);
        nodes.push_back(attr);
    }
    return list(nodes);
}

Node* Parser::parseImportAttribute() {
    Node* node = startNode();
    node->key() = type == &tt::string ? parseExprAtom() : parseIdent(true);
    expect(tt::colon);
    if (type != &tt::string) unexpected();
    node->value() = parseExprAtom();
    return finishNode(node, NodeType::ImportAttribute);
}

Node* Parser::parseModuleExportName() {
    if (ecmaVersion >= 13 && type == &tt::string) {
        Node* stringLiteral = parseLiteral();
        if (HasLoneSurrogate(stringLiteral->str2))
            raise(static_cast<int>(stringLiteral->ustart), "An export name cannot include a lone surrogate.");
        return stringLiteral;
    }
    return parseIdent(true);
}

void Parser::adaptDirectivePrologue(const NodeList& statements) {
    for (size_t i = 0; i < statements.size() && isDirectiveCandidate(statements[i]); ++i) {
        Node* s = statements[i];
        std::string_view raw = s->expression()->str;
        s->str = raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : std::string_view{};
        s->Set(kFlagHasDirective, true);
    }
}

bool Parser::isDirectiveCandidate(Node* statement) {
    if (!(ecmaVersion >= 5 && statement->type == NodeType::ExpressionStatement &&
          statement->expression()->type == NodeType::Literal && statement->expression()->lit == LitKind::String))
        return false;
    int c = charAt(static_cast<int>(statement->ustart));
    return c == '"' || c == '\'';
}

// ---------------------------------------------------------------- lval

Node* Parser::toAssignable(Node* node, bool isBinding, DestructuringErrors* r) {
    if (ecmaVersion >= 6 && node) {
        switch (node->type) {
        case NodeType::Identifier:
            if (inAsync() && node->str == "await")
                raise(static_cast<int>(node->ustart), "Cannot use 'await' as identifier inside an async function");
            break;
        case NodeType::ObjectPattern:
        case NodeType::ArrayPattern:
        case NodeType::AssignmentPattern:
        case NodeType::RestElement:
            break;
        case NodeType::ObjectExpression:
            node->type = NodeType::ObjectPattern;
            if (r) checkPatternErrors(r, true);
            for (Node* prop : node->properties()) {
                toAssignable(prop, isBinding);
                if (prop->type == NodeType::RestElement && (prop->argument()->type == NodeType::ArrayPattern ||
                                                            prop->argument()->type == NodeType::ObjectPattern))
                    raise(static_cast<int>(prop->argument()->ustart), "Unexpected token");
            }
            break;
        case NodeType::Property:
            if (node->kind != "init")
                raise(static_cast<int>(node->key()->ustart), "Object pattern can't contain getter or setter");
            toAssignable(node->value(), isBinding);
            break;
        case NodeType::ArrayExpression:
            node->type = NodeType::ArrayPattern;
            if (r) checkPatternErrors(r, true);
            toAssignableList(node->elements().items, node->elements().size(), isBinding);
            break;
        case NodeType::SpreadElement:
            node->type = NodeType::RestElement;
            toAssignable(node->argument(), isBinding);
            if (node->argument()->type == NodeType::AssignmentPattern)
                raise(static_cast<int>(node->argument()->ustart), "Rest elements cannot have a default value");
            break;
        case NodeType::AssignmentExpression:
            if (node->op != "=")
                raise(static_cast<int>(node->left()->uend), "Only '=' operator can be used for specifying default value.");
            node->type = NodeType::AssignmentPattern;
            node->op = {};
            toAssignable(node->left(), isBinding);
            break;
        case NodeType::ParenthesizedExpression:
            toAssignable(node->expression(), isBinding, r);
            break;
        case NodeType::ChainExpression:
            raiseRecoverable(static_cast<int>(node->ustart), "Optional chaining cannot appear in left-hand side");
            break;
        case NodeType::MemberExpression:
            if (!isBinding) break;
            [[fallthrough]];
        default:
            raise(static_cast<int>(node->ustart), "Assigning to rvalue");
        }
    } else if (r) {
        checkPatternErrors(r, true);
    }
    return node;
}

void Parser::toAssignableList(Node** items, size_t n, bool isBinding) {
    for (size_t i = 0; i < n; ++i)
        if (items[i]) toAssignable(items[i], isBinding);
    if (n) {
        Node* last = items[n - 1];
        if (ecmaVersion == 6 && isBinding && last && last->type == NodeType::RestElement &&
            last->argument()->type != NodeType::Identifier)
            unexpected(static_cast<int>(last->argument()->ustart));
    }
}

Node* Parser::parseSpread(DestructuringErrors* r) {
    Node* node = startNode();
    next();
    node->argument() = parseMaybeAssign(ForInit::No, r);
    return finishNode(node, NodeType::SpreadElement);
}

Node* Parser::parseRestBinding() {
    Node* node = startNode();
    next();
    if (ecmaVersion == 6 && type != &tt::name) unexpected();
    node->argument() = parseBindingAtom();
    return finishNode(node, NodeType::RestElement);
}

Node* Parser::parseBindingAtom() {
    DepthGuard guard(*this);
    if (ecmaVersion >= 6) {
        if (type == &tt::bracketL) {
            Node* node = startNode();
            next();
            node->elements() = list(parseBindingList(tt::bracketR, true, true));
            return finishNode(node, NodeType::ArrayPattern);
        }
        if (type == &tt::braceL) return parseObj(true);
    }
    return parseIdent();
}

std::vector<Node*> Parser::parseBindingList(const TokenType& close, bool allowEmpty, bool allowTrailingComma,
                                            bool allowModifiers) {
    std::vector<Node*> elts;
    bool first = true;
    while (!eat(close)) {
        if (first)
            first = false;
        else
            expect(tt::comma);
        if (allowEmpty && type == &tt::comma) {
            elts.push_back(nullptr);
        } else if (allowTrailingComma && afterTrailingComma(close)) {
            break;
        } else if (type == &tt::ellipsis) {
            Node* rest = parseRestBinding();
            parseBindingListItem(rest);
            elts.push_back(rest);
            if (type == &tt::comma) raiseRecoverable(start, "Comma is not permitted after the rest element");
            expect(close);
            break;
        } else {
            elts.push_back(parseAssignableListItem(allowModifiers));
        }
    }
    return elts;
}

Node* Parser::parseAssignableListItem(bool) {
    Node* elem = parseMaybeDefault(start, startLoc);
    parseBindingListItem(elem);
    return elem;
}

Node* Parser::parseMaybeDefault(int startPos, Pos sLoc, Node* left) {
    if (!left) left = parseBindingAtom();
    if (ecmaVersion < 6 || !eat(tt::eq)) return left;
    Node* node = startNodeAt(startPos, sLoc);
    node->left() = left;
    node->right() = parseMaybeAssign();
    return finishNode(node, NodeType::AssignmentPattern);
}

void Parser::checkLValSimple(Node* expr, int bindingType, std::vector<std::string_view>* checkClashes) {
    bool isBind = bindingType != BIND_NONE;
    switch (expr->type) {
    case NodeType::Identifier:
        if (strict && inList(reservedWordsStrictBind, expr->str))
            raiseRecoverable(static_cast<int>(expr->ustart),
                             (isBind ? "Binding " : "Assigning to ") + std::string(expr->str) + " in strict mode");
        if (isBind) {
            if (bindingType == BIND_LEXICAL && expr->str == "let")
                raiseRecoverable(static_cast<int>(expr->ustart), "let is disallowed as a lexically bound name");
            if (checkClashes) {
                if (inList(*checkClashes, expr->str))
                    raiseRecoverable(static_cast<int>(expr->ustart), "Argument name clash");
                checkClashes->push_back(expr->str);
            }
            if (bindingType != BIND_OUTSIDE) declareName(expr->str, bindingType, static_cast<int>(expr->ustart));
        }
        break;
    case NodeType::ChainExpression:
        raiseRecoverable(static_cast<int>(expr->ustart), "Optional chaining cannot appear in left-hand side");
        break;
    case NodeType::MemberExpression:
        if (isBind) raiseRecoverable(static_cast<int>(expr->ustart), "Binding member expression");
        break;
    case NodeType::ParenthesizedExpression:
        if (isBind) raiseRecoverable(static_cast<int>(expr->ustart), "Binding parenthesized expression");
        return checkLValSimple(expr->expression(), bindingType, checkClashes);
    default:
        raise(static_cast<int>(expr->ustart), std::string(isBind ? "Binding" : "Assigning to") + " rvalue");
    }
}

void Parser::checkLValPattern(Node* expr, int bindingType, std::vector<std::string_view>* checkClashes) {
    switch (expr->type) {
    case NodeType::ObjectPattern:
        for (Node* prop : expr->properties()) checkLValInnerPattern(prop, bindingType, checkClashes);
        break;
    case NodeType::ArrayPattern:
        for (Node* elem : expr->elements())
            if (elem) checkLValInnerPattern(elem, bindingType, checkClashes);
        break;
    default:
        checkLValSimple(expr, bindingType, checkClashes);
    }
}

void Parser::checkLValInnerPattern(Node* expr, int bindingType, std::vector<std::string_view>* checkClashes) {
    switch (expr->type) {
    case NodeType::Property:
        checkLValInnerPattern(expr->value(), bindingType, checkClashes);
        break;
    case NodeType::AssignmentPattern:
        checkLValPattern(expr->left(), bindingType, checkClashes);
        break;
    case NodeType::RestElement:
        checkLValPattern(expr->argument(), bindingType, checkClashes);
        break;
    default:
        checkLValPattern(expr, bindingType, checkClashes);
    }
}

// ---------------------------------------------------------------- recovery

void Parser::recordError(const SyntaxErr& e) {
    ++sawErrors;
    if (out.errors.size() >= 1000) return;
    Error err;
    err.message = e.message;
    err.upos = static_cast<uint32_t>(std::max(0, e.pos));
    err.offset = out.ByteOffset(err.upos);
    err.loc = lineInfo(e.pos);
    err.raised_at = static_cast<uint32_t>(std::max(0, e.raisedAt));
    if (out.errors.empty()) {
        out.has_error = true;
        out.first_error_offset = err.offset;
    }
    out.errors.push_back(std::move(err));
}

Parser::Snapshot Parser::takeSnapshot() const {
    Snapshot s;
    s.scopeDepth = scopeStack.size();
    s.context = context;
    s.labels = labels;
    s.privateDepth = privateNameStack.size();
    s.strict = strict;
    s.yieldPos = yieldPos;
    s.awaitPos = awaitPos;
    s.awaitIdentPos = awaitIdentPos;
    s.potentialArrowAt = potentialArrowAt;
    s.potentialArrowInForAwait = potentialArrowInForAwait;
    s.inTemplateElement = inTemplateElement;
    s.depth = depth;
    s.pos = pos;
    s.lineStart = lineStart;
    s.curLine = curLine;
    s.start = start;
    s.end = end;
    s.lastTokStart = lastTokStart;
    s.lastTokEnd = lastTokEnd;
    s.startLoc = startLoc;
    s.endLoc = endLoc;
    s.lastTokStartLoc = lastTokStartLoc;
    s.lastTokEndLoc = lastTokEndLoc;
    s.type = type;
    s.value = value;
    s.exprAllowed = exprAllowed;
    s.containsEsc = containsEsc;
    s.commentCount = out.comments.size();
    s.tokenCount = out.tokens.size();
    return s;
}

void Parser::restoreSnapshot(const Snapshot& s) {
    if (scopeStack.size() > s.scopeDepth) scopeStack.resize(s.scopeDepth);
    context = s.context;
    labels = s.labels;
    if (privateNameStack.size() > s.privateDepth) privateNameStack.resize(s.privateDepth);
    strict = s.strict;
    yieldPos = s.yieldPos;
    awaitPos = s.awaitPos;
    awaitIdentPos = s.awaitIdentPos;
    potentialArrowAt = s.potentialArrowAt;
    potentialArrowInForAwait = s.potentialArrowInForAwait;
    inTemplateElement = s.inTemplateElement;
    depth = s.depth;
    pos = s.pos;
    lineStart = s.lineStart;
    curLine = s.curLine;
    start = s.start;
    end = s.end;
    lastTokStart = s.lastTokStart;
    lastTokEnd = s.lastTokEnd;
    startLoc = s.startLoc;
    endLoc = s.endLoc;
    lastTokStartLoc = s.lastTokStartLoc;
    lastTokEndLoc = s.lastTokEndLoc;
    type = s.type;
    value = s.value;
    exprAllowed = s.exprAllowed;
    containsEsc = s.containsEsc;
    if (out.comments.size() > s.commentCount) out.comments.resize(s.commentCount);
    if (out.tokens.size() > s.tokenCount) out.tokens.resize(s.tokenCount);
}

Node* Parser::parseStatementRecovering(const char* ctx, bool topLevel, std::vector<std::string>* exports) {
    if (!recover) return parseStatement(ctx, topLevel, exports);
    Snapshot snap = takeSnapshot();
    try {
        return parseStatement(ctx, topLevel, exports);
    } catch (const SyntaxErr& e) {
        int errAt = start;
        recordError(e);
        resync(snap, std::max(errAt, e.pos), false, topLevel);
        return nullptr;
    }
}

namespace {

bool StartsStatement(const TokenType* t, const std::string& value) {
    if (t == &tt::_var || t == &tt::_const || t == &tt::_function || t == &tt::_class || t == &tt::_if ||
        t == &tt::_for || t == &tt::_while || t == &tt::_do || t == &tt::_return || t == &tt::_switch ||
        t == &tt::_try_ || t == &tt::_throw || t == &tt::_break || t == &tt::_continue || t == &tt::_import ||
        t == &tt::_export || t == &tt::_with || t == &tt::_debugger)
        return true;
    return t == &tt::name && (value == "let" || value == "async");
}

}  // namespace

void Parser::resync(const Snapshot& s, int errorPos, bool inClassBody, bool topLevel) {
    (void)inClassBody;
    restoreSnapshot(s);
    // Errors the skipped tokens raise (regexps, numeric separators, ...)
    // are not worth reporting.
    struct Quiet {
        bool& q;
        bool old;
        ~Quiet() { q = old; }
    } quiet{quietRecoverable, quietRecoverable};
    quietRecoverable = true;
    const int stmtStart = s.start;
    const uint32_t stmtCol = s.startLoc.column;

    // The context the statement started in, before its first token pushed one.
    std::vector<const TokContext*> base = s.context;
    auto popBase = [&](size_t n) {
        for (size_t i = 0; i < n && base.size() > 1; ++i) base.pop_back();
    };
    if (s.type == &tt::braceL || s.type == &tt::parenL || s.type == &tt::backQuote || s.type == &tt::_function ||
        s.type == &tt::_class || s.type == &tt::dollarBraceL)
        popBase(1);
    else if (s.type == &tt::jsxTagStart)
        popBase(2);

    // Advance one token without acorn's checks; a token the tokenizer
    // rejects is skipped a character at a time (or, if it ran to the end of
    // the input looking for a terminator, to the end).
    auto advance = [&]() {
        lastTokEnd = end;
        lastTokStart = start;
        lastTokEndLoc = endLoc;
        lastTokStartLoc = startLoc;
        for (;;) {
            int before = pos;
            try {
                nextToken();
                return;
            } catch (const SyntaxErr& e) {
                bool runaway = e.message == "Unterminated template" || e.message == "Unterminated comment" ||
                               e.message == "Unterminated JSX contents";
                int resume = runaway ? inputLen : std::max(start + 1, before + 1);
                if (resume > inputLen) resume = inputLen;
                // recompute the line position for the restart point
                Pos lp = lineInfo(resume);
                pos = resume;
                curLine = static_cast<int>(lp.line);
                lineStart = resume - static_cast<int>(lp.column);
                inTemplateElement = false;
                if (runaway || !context.empty()) {
                    // leave template / JSX contexts we can no longer trust
                    while (context.size() > 1 && (context.back() == &tc::q_tmpl || context.back() == &tc::j_expr ||
                                                  context.back() == &tc::j_oTag || context.back() == &tc::j_cTag))
                        context.pop_back();
                }
                if (pos >= inputLen) {
                    start = end = pos;
                    startLoc = endLoc = curPosition();
                    type = &tt::eof;
                    value.clear();
                    return;
                }
            }
        }
    };

    std::vector<char> brackets;
    bool first = true;
    int resumeAt = -1;  // re-read the token starting here
    bool resumeAfter = false;
    const TokenType* prevTok = nullptr;
    for (;;) {
        if (type == &tt::eof) break;
        if (!first && start > stmtStart) {
            bool afterErr = start >= errorPos;
            bool atLineStart = lineBreakIn(lastTokEnd, start);
            bool hasOpenBrace = std::find(brackets.begin(), brackets.end(), '{') != brackets.end();
            if (afterErr && brackets.empty() && type == &tt::semi) {
                resumeAt = start;
                resumeAfter = true;
                break;
            }
            if (type == &tt::braceR && !hasOpenBrace) {
                if (topLevel) {
                    resumeAt = start;
                    resumeAfter = true;
                } else {
                    resumeAt = start;
                }
                break;
            }
            if (afterErr && atLineStart) {
                uint32_t col = startLoc.column;
                if (brackets.empty() && (col <= stmtCol || StartsStatement(type, value))) {
                    resumeAt = start;
                    break;
                }
                if (!brackets.empty() && col <= stmtCol && StartsStatement(type, value)) {
                    resumeAt = start;
                    break;
                }
                // A word at or left of the statement's column, after a token
                // that ends an expression: brackets were lost, a new
                // statement starts here.
                if (!brackets.empty() && col <= stmtCol && (type == &tt::name || type->keyword) && prevTok &&
                    !prevTok->beforeExpr) {
                    resumeAt = start;
                    break;
                }
                if (!brackets.empty() && type == &tt::braceR && col < stmtCol) {
                    resumeAt = start;
                    break;
                }
            }
        }
        if (type == &tt::parenL) {
            brackets.push_back('(');
        } else if (type == &tt::bracketL) {
            brackets.push_back('[');
        } else if (type == &tt::braceL || type == &tt::dollarBraceL) {
            brackets.push_back('{');
        } else if (type == &tt::parenR || type == &tt::bracketR) {
            char want = type == &tt::parenR ? '(' : '[';
            auto it = std::find(brackets.rbegin(), brackets.rend(), want);
            if (it != brackets.rend()) brackets.erase(std::next(it).base(), brackets.end());
        } else if (type == &tt::braceR) {
            auto it = std::find(brackets.rbegin(), brackets.rend(), '{');
            if (it != brackets.rend()) brackets.erase(std::next(it).base(), brackets.end());
        }
        first = false;
        prevTok = type;
        advance();
    }

    // Reset the tokenizer to the resume point in the statement's context.
    if (resumeAt < 0) {
        // at EOF
        while (!out.comments.empty() && static_cast<int>(out.comments.back().ustart) >= pos) out.comments.pop_back();
        context = base;
        return;
    }
    Pos rl = resumeAfter ? endLoc : startLoc;
    int rp = resumeAfter ? end : resumeAt;
    if (resumeAfter) {
        lastTokEnd = end;
        lastTokStart = start;
        lastTokEndLoc = endLoc;
        lastTokStartLoc = startLoc;
    }
    while (!out.comments.empty() && static_cast<int>(out.comments.back().ustart) >= rp) out.comments.pop_back();
    context = base;
    exprAllowed = true;
    type = &tt::semi;
    value.clear();
    containsEsc = false;
    inTemplateElement = false;
    pos = rp;
    curLine = static_cast<int>(rl.line);
    lineStart = rp - static_cast<int>(rl.column);
    advance();
}

}  // namespace jsparse
