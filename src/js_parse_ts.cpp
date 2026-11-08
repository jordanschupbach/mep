// TypeScript dialect (see js_parse_ts.h): construction, tokenizer hooks,
// speculation helpers and type parsing (tsc's parser.ts, "TYPES").

#include "js_parse_ts.h"

#include <cstring>
#include <regex>

namespace jsparse {

namespace tt {
// label, keyword, beforeExpr, startsExpr, isLoop, isAssign, prefix, postfix, binop
const TokenType at{"@", nullptr, false, false, false, false, false, false, -1};
}  // namespace tt

TsParser::TsParser(const Options& o, std::u16string_view in, Arena& a, ParseResult& r) : JsxParser(o, in, a, r) {
    tsx = o.jsx;
    dts = o.dts;
    inAmbient = dts ? 1 : 0;
    // tsc's parser has no strict mode, no scope checks and no placement
    // rules for return/import/export; its binder and checker do that.
    neverStrict = true;
    strict = false;
    allowReturnOutsideFunction = true;
    allowImportExportEverywhere = true;
    checkPrivateFields = false;
    allowSuperOutsideMethod = true;
    skipRegExpValidation = true;
    // No HTML-like comments; top-level await as in a module.
    inModule = true;
    reservedWords.clear();
    reservedWords.push_back("enum");
    reservedWordsStrict = reservedWords;
    reservedWordsStrictBind = reservedWords;
}

// ---------------------------------------------------------------- tokenizer

void TsParser::readToken(int code) {
    if (tsx) return JsxParser::readToken(code);
    Parser::readToken(code);
}

void TsParser::getTokenFromCode(int code) {
    if (code == '@') {
        ++pos;
        return finishToken(tt::at);
    }
    if (code == 0x85 || code == 0x200B) {
        // tsc's whitespace has NEL and ZERO WIDTH SPACE too
        ++pos;
        return nextToken();
    }
    if (code == '0' && charAt(pos + 1) >= '0' && charAt(pos + 1) <= '9') {
        // tsc's scanner rejects legacy octal (`01`) and leading-zero
        // decimals (`09`) in every file; acorn only in strict mode
        bool decimal = false;
        for (int i = pos + 1; charAt(i) >= '0' && charAt(i) <= '9'; ++i)
            if (charAt(i) >= '8') decimal = true;
        raiseRecoverable(pos, decimal ? "Decimals with leading zeros are not allowed"
                                      : "Octal literals are not allowed");
    }
    Parser::getTokenFromCode(code);
}

void TsParser::updateContext(const TokenType* prevType) {
    if (type == &tt::at) {
        exprAllowed = false;
        return;
    }
    JsxParser::updateContext(prevType);
}

// ---------------------------------------------------------------- pragmas

void TsParser::tsCheckPragmas() {
    if (!out.program) return;
    uint32_t limit = out.program->body_list().size() ? out.program->body_list()[0]->ustart : UINT32_MAX;
    static const std::regex xmlStart(R"re(^///\s*<(\S+)\s.*?/>)re");
    struct Found {
        int pos;
        std::string message;
    };
    // pragma names in first-seen order (tsc walks a Map), with their errors
    std::vector<std::pair<std::string, std::vector<Found>>> kinds;
    auto bucket = [&](const std::string& name) -> std::vector<Found>& {
        for (auto& k : kinds)
            if (k.first == name) return k.second;
        kinds.push_back({name, {}});
        return kinds.back().second;
    };
    auto arg = [](const std::string& text, const char* name, size_t& at) -> const char* {
        std::regex re(std::string(R"re((\s)re") + name + R"re(\s*=\s*)(?:(?:'([^']*)')|(?:"([^"]*)")))re",
                      std::regex::icase);
        std::smatch m;
        if (!std::regex_search(text, m, re)) return nullptr;
        at = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(1)) + 1;
        static thread_local std::string argValue;
        argValue = m[2].matched ? m[2].str() : m[3].str();
        return argValue.c_str();
    };
    int amdNames = 0;
    for (const Comment& c : out.comments) {
        if (c.ustart >= limit) break;
        if (c.block) continue;
        std::string text = "//" + std::string(c.value);
        std::smatch m;
        if (!std::regex_search(text, m, xmlStart)) continue;
        std::string name = m[1].str();
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        int at = static_cast<int>(c.ustart);
        if (name == "reference") {
            std::vector<Found>& errs = bucket(name);
            size_t p = 0;
            const char* noDefault = arg(text, "no-default-lib", p);
            if (noDefault && std::string(noDefault) == "true") continue;
            size_t typesAt = 0;
            if (arg(text, "types", typesAt)) {
                const char* mode = arg(text, "resolution-mode", p);
                if (mode && std::string(mode) != "import" && std::string(mode) != "require")
                    errs.push_back({static_cast<int>(out.U16Offset(c.start + static_cast<uint32_t>(typesAt))),
                                    "`resolution-mode` should be either `require` or `import`"});
                continue;
            }
            if (arg(text, "lib", p) || arg(text, "path", p)) continue;
            errs.push_back({at, "Invalid 'reference' directive syntax"});
        } else if (name == "amd-module") {
            size_t p = 0;
            if (!arg(text, "name", p)) continue;  // a pragma without its required argument is ignored
            std::vector<Found>& errs = bucket(name);
            if (amdNames++) errs.push_back({at, "An AMD module cannot have multiple name assignments"});
        }
    }
    for (auto& k : kinds)
        for (Found& f : k.second) Parser::raiseRecoverable(f.pos, f.message);
}

// ---------------------------------------------------------------- errors

void TsParser::raiseRecoverable(int p, const std::string& message) {
    // Keep what tsc's scanner and parser report; drop acorn's early errors
    // that tsc leaves to its binder and checker.
    static const char* const kKeep[] = {
        "Escape sequence in keyword",
        "Bad escape sequence",
        "Octal literal in template",
        "Invalid escape sequence in template",
        "Numeric separator",
        "The keyword 'enum' is reserved",
        "Octal literals are not allowed",
        "Decimals with leading zeros are not allowed",
        "Unexpected keyword",
        "'super' may not use type arguments",
        "Neither decorators nor modifiers may be applied to 'this' parameters",
        "An optional chain cannot contain private identifiers",
    };
    for (const char* k : kKeep)
        if (message.compare(0, std::strlen(k), k) == 0) return Parser::raiseRecoverable(p, message);
}

void TsParser::checkPatternErrors(DestructuringErrors*, bool) {}

bool TsParser::checkExpressionErrors(DestructuringErrors* r, bool andThrow) {
    // `({a = 1})` parses in tsc (a ShorthandPropertyAssignment with an
    // initializer); the checker rejects it.
    if (!r || andThrow) return false;
    return r->shorthandAssign >= 0 || r->doubleProto >= 0;
}

void TsParser::checkYieldAwaitInDefaultParams() {}

void TsParser::checkUnreserved(Node* ref) {
    int s = static_cast<int>(ref->ustart);
    std::string_view name = ref->str;
    if (inGenerator() && name == "yield") raiseRecoverable(s, "Cannot use 'yield' as identifier inside a generator");
    if (inAsync() && name == "await") raiseRecoverable(s, "Cannot use 'await' as identifier inside an async function");
    if (isKeywordWord(name)) raise(s, "Unexpected keyword '" + std::string(name) + "'");
    if (name == "enum") raiseRecoverable(s, "The keyword 'enum' is reserved");
}

void TsParser::checkLValSimple(Node*, int, std::vector<std::string_view>*) {}
void TsParser::checkLValPattern(Node*, int, std::vector<std::string_view>*) {}
void TsParser::checkLValInnerPattern(Node*, int, std::vector<std::string_view>*) {}

// ---------------------------------------------------------------- speculation

TsParser::TsSave TsParser::tsSave() const {
    TsSave s;
    s.snap = takeSnapshot();
    s.errors = out.errors.size();
    s.sawErrors = sawErrors;
    s.hasError = out.has_error;
    s.firstErrorOffset = out.first_error_offset;
    s.inDisallowConditionalTypes = inDisallowConditionalTypes;
    s.inDecorator = inDecorator;
    s.inType = inType;
    s.inAmbient = inAmbient;
    return s;
}

void TsParser::tsRestore(const TsSave& s) {
    restoreSnapshot(s.snap);
    if (out.errors.size() > s.errors) out.errors.resize(s.errors);
    sawErrors = s.sawErrors;
    out.has_error = s.hasError;
    out.first_error_offset = s.firstErrorOffset;
    inDisallowConditionalTypes = s.inDisallowConditionalTypes;
    inDecorator = s.inDecorator;
    inType = s.inType;
    inAmbient = s.inAmbient;
}

// ---------------------------------------------------------------- token helpers

bool TsParser::isIdentToken() {
    if (type != &tt::name) return false;
    if (value == "enum") return false;
    if (value == "yield" && !containsEsc && inGenerator()) return false;
    return true;
}

bool TsParser::isIdentOrKeywordToken() const { return type == &tt::name || type->keyword != nullptr; }

bool TsParser::isLiteralPropertyNameToken() const {
    return isIdentOrKeywordToken() || type == &tt::string || type == &tt::num;
}

bool TsParser::isModifierToken() const {
    if (type == &tt::_const || type == &tt::_export || type == &tt::_default || type == &tt::_in) return true;
    if (type != &tt::name) return false;
    static const char* const kMods[] = {"abstract", "accessor", "async",    "declare",   "out",    "override",
                                        "private",  "protected", "public", "readonly", "static"};
    for (const char* m : kMods)
        if (value == m) return true;
    return false;
}

bool TsParser::isLt() {
    return (type == &tt::relational && value == "<") || type == &tt::jsxTagStart ||
           (type == &tt::bitShift && value.size() >= 2 && value[0] == '<');
}

bool TsParser::isGtStart() const {
    return (type == &tt::relational || type == &tt::bitShift || type == &tt::assign) && !value.empty() &&
           value[0] == '>';
}

void TsParser::splitToken(int len, const TokenType& t, const char* val) {
    pos = start + len;
    end = pos;
    endLoc = Pos{startLoc.line, startLoc.column + static_cast<uint32_t>(len)};
    type = &t;
    value = val;
    exprAllowed = true;
}

void TsParser::reReadLtAsRelational() {
    if (type == &tt::jsxTagStart) {
        // acorn-jsx pushed j_expr and j_oTag for the tag start.
        if (context.size() >= 2 && context.back() == &tc::j_oTag) context.pop_back();
        if (!context.empty() && context.back() == &tc::j_expr) context.pop_back();
        type = &tt::relational;
        value = "<";
        exprAllowed = true;
    } else if (type == &tt::bitShift && value.size() >= 2 && value[0] == '<') {
        splitToken(1, tt::relational, "<");
    }
}

bool TsParser::eatLt() {
    if (!isLt()) return false;
    reReadLtAsRelational();
    next();
    return true;
}

void TsParser::expectGt() {
    if (type == &tt::relational && value == ">") {
        next();
        return;
    }
    if (isGtStart()) {
        splitToken(1, tt::relational, ">");
        next();
        return;
    }
    unexpected();
}

TsParser::Peek TsParser::peek() {
    TsSave s = tsSave();
    Peek p{nullptr, {}, false, false, -1};
    try {
        next();
        p = Peek{type, value, lineBreakIn(lastTokEnd, start), containsEsc, start};
    } catch (const SyntaxErr&) {
        p.type = nullptr;
    }
    tsRestore(s);
    return p;
}

bool TsParser::nextTokenIsIdentOnSameLine() {
    return lookAhead([&] {
        next();
        return isIdentToken() && !hasPrecedingLineBreak();
    });
}

bool TsParser::nextTokenIsIdentOrKeywordOnSameLine() {
    return lookAhead([&] {
        next();
        return isIdentOrKeywordToken() && !hasPrecedingLineBreak();
    });
}

bool TsParser::canFollowModifier() const {
    return type == &tt::bracketL || type == &tt::braceL || type == &tt::star || type == &tt::ellipsis ||
           type == &tt::privateId || isLiteralPropertyNameToken();
}

// nextTokenCanFollowModifier, at a modifier token (consumes tokens).
bool TsParser::nextTokenCanFollowModifier() {
    auto canFollowExport = [&]() {
        return type == &tt::at ||
               (type != &tt::star && !isWord("as") && type != &tt::braceL && canFollowModifier());
    };
    auto nextCanFollowDefault = [&]() {
        next();
        if (type == &tt::_class || type == &tt::_function || isWord("interface") || type == &tt::at) return true;
        if (isWord("abstract"))
            return lookAhead([&] {
                next();
                return type == &tt::_class && !hasPrecedingLineBreak();
            });
        if (isWord("async"))
            return lookAhead([&] {
                next();
                return type == &tt::_function && !hasPrecedingLineBreak();
            });
        return false;
    };
    if (type == &tt::_const) {
        next();
        return isWord("enum");
    }
    if (type == &tt::_export) {
        next();
        if (type == &tt::_default) return lookAhead(nextCanFollowDefault);
        if (isWord("type")) return lookAhead([&] {
            next();
            return canFollowExport();
        });
        return canFollowExport();
    }
    if (type == &tt::_default) return nextCanFollowDefault();
    if (isWord("static")) {
        next();
        return canFollowModifier();
    }
    if (isWord("get") || isWord("set")) {
        next();
        return type == &tt::bracketL || type == &tt::privateId || isLiteralPropertyNameToken();
    }
    next();
    if (hasPrecedingLineBreak()) return false;
    return canFollowModifier();
}

bool TsParser::parseAnyContextualModifier() {
    if (!isModifierToken()) return false;
    TsSave s = tsSave();
    bool ok = false;
    try {
        ok = nextTokenCanFollowModifier();
    } catch (const SyntaxErr&) {
        ok = false;
    }
    tsRestore(s);
    if (ok) next();
    return ok;
}

Node* TsParser::extendTo(Node* n) {
    if (lastTokEnd > static_cast<int>(n->uend)) finishNode(n, n->type);
    return n;
}

Node* TsParser::finishAt(Node* n, NodeType t, Node* endNode) {
    return finishNodeAt(n, t, static_cast<int>(endNode->uend), endNode->loc_end);
}

// ---------------------------------------------------------------- types

Node* TsParser::tsWrapAnnotation(int startPos, Pos sLoc, Node* t) {
    Node* a = startNodeAt(startPos, sLoc);
    a->typeAnnotation() = t;
    return finishNode(a, NodeType::TSTypeAnnotation);
}

Node* TsParser::tsParseTypeInContext(bool disallowConditional) {
    bool saved = inDisallowConditionalTypes, savedType = inType;
    inDisallowConditionalTypes = disallowConditional;
    inType = true;
    Node* t = nullptr;
    try {
        t = tsParseType();
    } catch (...) {
        inDisallowConditionalTypes = saved;
        inType = savedType;
        throw;
    }
    inDisallowConditionalTypes = saved;
    inType = savedType;
    return t;
}

Node* TsParser::tsParseTypeAnnotation() {
    int s = start;
    Pos sl = startLoc;
    expect(tt::colon);
    return tsWrapAnnotation(s, sl, tsParseTypeInContext(false));
}

Node* TsParser::tsTryParseTypeAnnotation() { return type == &tt::colon ? tsParseTypeAnnotation() : nullptr; }

// parseReturnType: `: T` (or, in a function type, `=> T`), as a
// TSTypeAnnotation starting at the `:` / `=>`.
Node* TsParser::tsParseReturnType(bool arrow, bool isType) {
    int s = start;
    Pos sl = startLoc;
    if (arrow) {
        expect(tt::arrow);
    } else if (!eat(tt::colon)) {
        if (isType && type == &tt::arrow) {
            // tsc: "':' expected", then parses the type anyway
            Parser::raiseRecoverable(start, "Unexpected token");
            next();
        } else {
            return nullptr;
        }
    }
    bool saved = inDisallowConditionalTypes, savedType = inType;
    inDisallowConditionalTypes = false;
    inType = true;
    Node* t;
    try {
        t = tsParseTypeOrTypePredicate();
    } catch (...) {
        inDisallowConditionalTypes = saved;
        inType = savedType;
        throw;
    }
    inDisallowConditionalTypes = saved;
    inType = savedType;
    return tsWrapAnnotation(s, sl, t);
}

Node* TsParser::tsParseTypeOrTypePredicate() {
    int s = start;
    Pos sl = startLoc;
    Node* param = nullptr;
    if (isIdentToken()) {
        param = tryParse([&]() -> Node* {
            Node* id = parseIdent(true);
            if (isWord("is") && !hasPrecedingLineBreak()) {
                next();
                return id;
            }
            return nullptr;
        });
    }
    Node* t = tsParseType();
    if (!param) return t;
    Node* node = startNodeAt(s, sl);
    node->parameterName() = param;
    // the predicate's annotation spans the type (without its parentheses)
    node->typeAnnotation() = finishAt(startNodeAt(static_cast<int>(t->ustart), t->loc_start),
                                      NodeType::TSTypeAnnotation, t);
    node->typeAnnotation()->typeAnnotation() = t;
    return finishNode(node, NodeType::TSTypePredicate);
}

Node* TsParser::tsParseType() {
    DepthGuard guard(*this);
    if (type == &tt::jsxTagStart) reReadLtAsRelational();
    if (tsIsStartOfFunctionTypeOrConstructorType()) return tsParseFunctionOrConstructorType();
    int s = start;
    Pos sl = startLoc;
    Node* t = tsParseUnionOrIntersection(true);
    if (!inDisallowConditionalTypes && !hasPrecedingLineBreak() && type == &tt::_extends) {
        next();
        Node* node = startNodeAt(s, sl);
        node->checkType() = t;
        node->extendsType() = tsParseTypeInContext(true);
        expect(tt::question);
        node->trueType() = tsParseTypeInContext(false);
        expect(tt::colon);
        node->falseType() = tsParseTypeInContext(false);
        return finishNode(node, NodeType::TSConditionalType);
    }
    return t;
}

Node* TsParser::tsParseUnionOrIntersection(bool isUnion) {
    const TokenType& op = isUnion ? tt::bitwiseOR : tt::bitwiseAND;
    auto constituent = [&]() { return isUnion ? tsParseUnionOrIntersection(false) : tsParseTypeOperatorOrHigher(); };
    auto toError = [&]() -> Node* {
        // tsc: function/constructor types must be parenthesized in a union
        // or intersection; it parses one anyway and reports it.
        if (!tsIsStartOfFunctionTypeOrConstructorType()) return nullptr;
        Node* t = tsParseFunctionOrConstructorType();
        Parser::raiseRecoverable(static_cast<int>(t->ustart),
                                 std::string(t->type == NodeType::TSFunctionType ? "Function" : "Constructor") +
                                     " type notation must be parenthesized when used in " +
                                     (isUnion ? "a union" : "an intersection") + " type");
        return t;
    };
    int s = start;
    Pos sl = startLoc;
    bool leading = eat(op);
    Node* t = nullptr;
    if (leading) t = toError();
    if (!t) t = constituent();
    if (type == &op || leading) {
        std::vector<Node*> types{t};
        while (eat(op)) {
            Node* n = toError();
            types.push_back(n ? n : constituent());
        }
        Node* node = startNodeAt(s, sl);
        node->types() = list(types);
        t = finishNode(node, isUnion ? NodeType::TSUnionType : NodeType::TSIntersectionType);
    }
    return t;
}

Node* TsParser::tsParseTypeOperatorOrHigher() {
    DepthGuard guard(*this);
    if (isWord("keyof") || isWord("unique") || isWord("readonly")) {
        Node* node = startNode();
        node->op = isWord("keyof") ? "keyof" : isWord("unique") ? "unique" : "readonly";
        next();
        node->typeAnnotation() = tsParseTypeOperatorOrHigher();
        return finishNode(node, NodeType::TSTypeOperator);
    }
    if (isWord("infer")) return tsParseInferType();
    bool saved = inDisallowConditionalTypes;
    inDisallowConditionalTypes = false;
    Node* t;
    try {
        t = tsParsePostfixTypeOrHigher();
    } catch (...) {
        inDisallowConditionalTypes = saved;
        throw;
    }
    inDisallowConditionalTypes = saved;
    return t;
}

Node* TsParser::tsParseInferType() {
    Node* node = startNode();
    next();  // infer
    Node* tp = startNode();
    if (!isIdentToken()) unexpected();
    tp->name() = parseIdent(true);
    Node* constraint = tryParse([&]() -> Node* {
        if (!eat(tt::_extends)) return nullptr;
        Node* c = tsParseTypeInContext(true);
        if (inDisallowConditionalTypes || type != &tt::question) return c;
        return nullptr;
    });
    tp->constraint() = constraint;
    node->typeParameter() = finishNode(tp, NodeType::TSTypeParameter);
    return finishNode(node, NodeType::TSInferType);
}

Node* TsParser::tsParsePostfixTypeOrHigher() {
    int s = start;
    Pos sl = startLoc;
    Node* t = tsParseNonArrayType();
    while (!hasPrecedingLineBreak()) {
        if (type == &tt::prefix && value == "!") {
            // JSDoc `T!` (tsc parses it; the checker rejects it)
            next();
            t = tsJsDocWrap(NodeType::TSJSDocNonNullableType, t, s, sl, true);
        } else if (type == &tt::question) {
            bool startsType = lookAhead([&] {
                next();
                return tsIsStartOfType();
            });
            if (startsType) return t;
            // JSDoc `T?`; a tuple element turns it into a TSOptionalType
            next();
            t = tsJsDocWrap(NodeType::TSJSDocNullableType, t, s, sl, true);
        } else if (type == &tt::bracketL) {
            next();
            Node* node = startNodeAt(s, sl);
            if (tsIsStartOfType()) {
                node->objectType() = t;
                node->indexType() = tsParseTypeInContext(false);
                expect(tt::bracketR);
                t = finishNode(node, NodeType::TSIndexedAccessType);
            } else {
                expect(tt::bracketR);
                node->elementType() = t;
                t = finishNode(node, NodeType::TSArrayType);
            }
        } else {
            return t;
        }
    }
    return t;
}

Node* TsParser::tsParseKeywordType(NodeType t) {
    Node* node = startNode();
    next();
    return finishNode(node, t);
}

Node* TsParser::tsParseNonArrayType() {
    DepthGuard guard(*this);
    if (type == &tt::name && !containsEsc) {
        static const struct {
            const char* word;
            NodeType t;
        } kKeywords[] = {{"any", NodeType::TSAnyKeyword},         {"unknown", NodeType::TSUnknownKeyword},
                         {"string", NodeType::TSStringKeyword},   {"number", NodeType::TSNumberKeyword},
                         {"bigint", NodeType::TSBigIntKeyword},   {"symbol", NodeType::TSSymbolKeyword},
                         {"boolean", NodeType::TSBooleanKeyword}, {"undefined", NodeType::TSUndefinedKeyword},
                         {"never", NodeType::TSNeverKeyword},     {"object", NodeType::TSObjectKeyword}};
        for (const auto& k : kKeywords) {
            if (value == k.word) {
                Peek p = peek();
                if (p.type == &tt::dot) return tsParseTypeReference();
                return tsParseKeywordType(k.t);
            }
        }
        if (value == "asserts") {
            Peek p = peek();
            if (p.type && (p.type == &tt::name || p.type->keyword) && !p.lineBreakBefore)
                return tsParseAssertsTypePredicate();
            return tsParseTypeReference();
        }
        return tsParseTypeReference();
    }
    if (type == &tt::string || type == &tt::num || type == &tt::_true || type == &tt::_false)
        return tsParseLiteralType(false);
    if (type == &tt::_null) return tsParseKeywordType(NodeType::TSNullKeyword);
    if (type == &tt::plusMin && value == "-") {
        Peek p = peek();
        if (p.type == &tt::num) return tsParseLiteralType(true);
        return tsParseTypeReference();
    }
    if (type == &tt::_void) return tsParseKeywordType(NodeType::TSVoidKeyword);
    if (type == &tt::_this) {
        Node* thisType = tsParseKeywordType(NodeType::TSThisType);
        if (isWord("is") && !hasPrecedingLineBreak()) return tsParseThisTypePredicate(thisType);
        return thisType;
    }
    if (type == &tt::_typeof) {
        bool isImport = lookAhead([&] {
            next();
            return type == &tt::_import;
        });
        if (isImport) {
            int s = start;
            Pos sl = startLoc;
            next();
            return tsParseImportType(s, sl, true);
        }
        return tsParseTypeQuery();
    }
    if (type == &tt::braceL) return tsParseTypeLiteralOrMapped();
    if (type == &tt::bracketL) return tsParseTupleType();
    if (type == &tt::parenL) return tsParseParenthesizedType();
    if (type == &tt::_import) return tsParseImportType(start, startLoc, false);
    if (type == &tt::backQuote) return tsParseTemplateType();
    // JSDoc types (`*`, `?`, `?T`, `!T`, `function(T): U`): tsc parses them
    // and its checker rejects them in TypeScript; typescript-estree's
    // TSJSDoc* nodes (see js_parse.h).
    if (type == &tt::star) {
        Node* node = startNode();
        next();
        return finishNode(node, NodeType::TSJSDocAllType);
    }
    if (type == &tt::question || type == &tt::coalesce) {
        if (type == &tt::coalesce) splitToken(1, tt::question, "?");
        int s = start;
        Pos sl = startLoc;
        Node* node = startNode();
        next();
        if (type == &tt::comma || type == &tt::braceR || type == &tt::parenR || isGtStart() || type == &tt::eq ||
            type == &tt::bitwiseOR)
            return finishNode(node, NodeType::TSJSDocUnknownType);
        return tsJsDocWrap(NodeType::TSJSDocNullableType, tsParseType(), s, sl, false);
    }
    if (type == &tt::prefix && value == "!") {
        int s = start;
        Pos sl = startLoc;
        next();
        return tsJsDocWrap(NodeType::TSJSDocNonNullableType, tsParseNonArrayType(), s, sl, false);
    }
    if (type == &tt::_function) {
        if (!peekIs(tt::parenL)) return tsParseTypeReference();
        Node* node = startNode();
        next();
        expect(tt::parenL);
        std::vector<Node*> params;
        bool first = true;
        while (!eat(tt::parenR)) {
            if (!first) {
                expect(tt::comma);
                if (eat(tt::parenR)) break;
            }
            first = false;
            int ps = start;
            Pos psl = startLoc;
            Node* label = nullptr;
            if ((type == &tt::_this || type == &tt::_new) && peekIs(tt::colon)) {
                label = startNode();
                label->str = type == &tt::_this ? "this" : "new";
                next();
                finishNode(label, NodeType::Identifier);
                next();
            }
            int ts0 = start;
            Pos tl = startLoc;
            bool rest = eat(tt::ellipsis);
            Node* t = tsParseTypeInContext(false);
            if (rest) {
                Node* r = startNodeAt(ts0, tl);
                r->typeAnnotation() = t;
                t = finishNode(r, NodeType::TSRestType);
            }
            if (eat(tt::eq)) t = tsJsDocWrap(NodeType::TSJSDocOptionalType, t, ts0, tl, true);
            if (label) {
                Node* m = startNodeAt(ps, psl);
                m->label() = label;
                m->elementType() = t;
                t = finishNode(m, NodeType::TSNamedTupleMember);
            }
            params.push_back(t);
        }
        node->params() = list(params);
        node->returnType() = tsParseReturnType(false, false);
        return finishNode(node, NodeType::TSJSDocFunctionType);
    }
    return tsParseTypeReference();
}

Node* TsParser::tsParseLiteralType(bool negative) {
    Node* node = startNode();
    Node* lit;
    if (negative) {
        Node* u = startNode();
        u->op = "-";
        u->Set(kFlagPrefix, true);
        next();
        if (type != &tt::num) unexpected();
        u->argument() = parseLiteral();
        lit = finishNode(u, NodeType::UnaryExpression);
    } else if (type == &tt::_true || type == &tt::_false) {
        lit = startNode();
        lit->lit = LitKind::Bool;
        lit->Set(kFlagBoolValue, type == &tt::_true);
        lit->str = type->keyword;
        next();
        finishNode(lit, NodeType::Literal);
    } else {
        lit = parseLiteral();
    }
    node->literal() = lit;
    return finishNode(node, NodeType::TSLiteralType);
}

Node* TsParser::tsParseTemplateType() {
    Node* outer = startNode();
    Node* tmpl = startNode();
    next();  // `
    std::vector<Node*> quasis, types;
    Node* cur = parseTemplateElement(false);
    quasis.push_back(cur);
    while (!cur->Has(kFlagTail)) {
        if (type == &tt::eof) raise(pos, "Unterminated template literal");
        expect(tt::dollarBraceL);
        types.push_back(tsParseTypeInContext(false));
        expect(tt::braceR);
        quasis.push_back(cur = parseTemplateElement(false));
    }
    next();
    if (types.empty()) {
        tmpl->quasis() = list(quasis);
        tmpl->expressions() = {};
        finishNode(tmpl, NodeType::TemplateLiteral);
        outer->literal() = tmpl;
        return finishNode(outer, NodeType::TSLiteralType);
    }
    tmpl->quasis() = list(quasis);
    tmpl->template_types() = list(types);
    return finishNode(tmpl, NodeType::TSTemplateLiteralType);
}

Node* TsParser::tsParseEntityName(bool allowReservedWords, bool thisExpression) {
    int s = start;
    Pos sl = startLoc;
    Node* entity;
    if (thisExpression && type == &tt::_this) {
        // `typeof this.x`: typescript-estree's ThisExpression
        entity = startNode();
        next();
        entity = finishNode(entity, NodeType::ThisExpression);
    } else if (allowReservedWords ? isIdentOrKeywordToken() : isIdentToken())
        entity = parseIdent(true);
    else
        unexpected();
    while (type == &tt::dot) {
        // `A.<` (JSDoc `Array.<T>`) ends the name
        bool ltNext = lookAhead([&] {
            next();
            return isLt();
        });
        if (ltNext) {
            next();
            break;
        }
        next();
        Node* q = startNodeAt(s, sl);
        q->left() = entity;
        if (type == &tt::privateId)
            q->right() = parsePrivateIdent();
        else if (isIdentOrKeywordToken())
            q->right() = parseIdent(true);
        else
            unexpected();
        entity = finishNode(q, NodeType::TSQualifiedName);
    }
    return entity;
}

Node* TsParser::tsParseTypeReference() {
    Node* node = startNode();
    node->typeName() = tsParseEntityName(true);
    if (!hasPrecedingLineBreak() && isLt()) node->typeArguments() = tsParseTypeArguments();
    return finishNode(node, NodeType::TSTypeReference);
}

Node* TsParser::tsParseTypeQuery() {
    Node* node = startNode();
    next();  // typeof
    node->exprName() = tsParseEntityName(true, true);
    if (!hasPrecedingLineBreak() && isLt()) {
        Node* args = tryParse([&] { return tsParseTypeArguments(); });
        node->typeArguments() = args;
    }
    return finishNode(node, NodeType::TSTypeQuery);
}

Node* TsParser::tsParseImportType(int s, Pos sl, bool isTypeOf) {
    Node* node = startNodeAt(isTypeOf ? start : s, isTypeOf ? startLoc : sl);
    expect(tt::_import);
    expect(tt::parenL);
    // tsc parses a type here; ESTree's `source` is the string literal.
    Node* arg = tsParseTypeInContext(false);
    node->source() = arg->type == NodeType::TSLiteralType ? arg->literal() : arg;
    if (eat(tt::comma)) {
        // tsc: `, { with: { ... } }` (or `assert:`), nothing else
        if (type != &tt::braceL) unexpected();
        int keyAt = -1;
        lookAhead([&] {
            next();
            if (!isWord("with") && !isWord("assert") && type != &tt::_with) keyAt = start;
            return true;
        });
        if (keyAt >= 0) raise(keyAt, "Unexpected token");
        node->kid[2] = parseExprAtom();  // options: { with: { ... } }
    }
    expect(tt::parenR);
    if (eat(tt::dot)) node->qualifier() = tsParseEntityName(true);
    if (!hasPrecedingLineBreak() && isLt()) node->typeArguments() = tsParseTypeArguments();
    finishNode(node, NodeType::TSImportType);
    if (!isTypeOf) return node;
    Node* q = startNodeAt(s, sl);
    q->exprName() = node;
    return finishNode(q, NodeType::TSTypeQuery);
}

Node* TsParser::tsParseThisTypePredicate(Node* thisType) {
    next();  // is
    Node* node = startNodeAt(static_cast<int>(thisType->ustart), thisType->loc_start);
    node->parameterName() = thisType;
    Node* t = tsParseTypeInContext(false);
    Node* ann = startNodeAt(static_cast<int>(t->ustart), t->loc_start);
    ann->typeAnnotation() = t;
    node->typeAnnotation() = finishAt(ann, NodeType::TSTypeAnnotation, t);
    return finishNode(node, NodeType::TSTypePredicate);
}

Node* TsParser::tsParseAssertsTypePredicate() {
    Node* node = startNode();
    next();  // asserts
    node->SetTs(kTsAsserts, true);
    if (type == &tt::_this)
        node->parameterName() = tsParseKeywordType(NodeType::TSThisType);
    else if (isIdentToken())
        node->parameterName() = parseIdent(true);
    else
        unexpected();
    if (isWord("is")) {
        next();
        Node* t = tsParseTypeInContext(false);
        Node* ann = startNodeAt(static_cast<int>(t->ustart), t->loc_start);
        ann->typeAnnotation() = t;
        node->typeAnnotation() = finishAt(ann, NodeType::TSTypeAnnotation, t);
    }
    return finishNode(node, NodeType::TSTypePredicate);
}

bool TsParser::tsIsStartOfMappedType() {
    return lookAhead([&] {
        next();
        if (type == &tt::plusMin) {
            next();
            return isWord("readonly");
        }
        if (isWord("readonly")) next();
        if (type != &tt::bracketL) return false;
        next();
        if (!isIdentToken()) return false;
        next();
        return type == &tt::_in;
    });
}

Node* TsParser::tsParseTypeLiteralOrMapped() {
    if (tsIsStartOfMappedType()) return tsParseMappedType();
    Node* node = startNode();
    node->members() = list(tsParseObjectTypeMembers());
    return finishNode(node, NodeType::TSTypeLiteral);
}

std::vector<Node*> TsParser::tsParseObjectTypeMembers() {
    std::vector<Node*> members;
    expect(tt::braceL);
    while (type != &tt::braceR) {
        if (type == &tt::eof) unexpected();
        members.push_back(tsParseTypeMember());
    }
    next();
    return members;
}

Node* TsParser::tsParseMappedType() {
    Node* node = startNode();
    expect(tt::braceL);
    if (isWord("readonly")) {
        node->SetTs(kTsReadonly, true);
        next();
    } else if (type == &tt::plusMin) {
        node->SetTs(kTsReadonly, true);
        node->SetTs(value == "+" ? kTsReadonlyPlus : kTsReadonlyMinus, true);
        next();
        if (!isWord("readonly")) unexpected();
        next();
    }
    expect(tt::bracketL);
    if (!isIdentOrKeywordToken()) unexpected();
    node->key() = parseIdent(true);
    expect(tt::_in);
    node->constraint() = tsParseTypeInContext(false);
    node->nameType() = isWord("as") ? (next(), tsParseTypeInContext(false)) : nullptr;
    expect(tt::bracketR);
    if (type == &tt::question) {
        node->SetTs(kTsOptional, true);
        next();
    } else if (type == &tt::plusMin) {
        node->SetTs(kTsOptional, true);
        node->SetTs(value == "+" ? kTsMappedPlus : kTsMappedMinus, true);
        next();
        expect(tt::question);
    }
    if (eat(tt::colon)) node->typeAnnotation() = tsParseTypeInContext(false);
    semicolon();
    // tsc parses (and the checker rejects) members after the mapping
    while (type != &tt::braceR) {
        if (type == &tt::eof) unexpected();
        tsParseTypeMember();
    }
    next();
    return finishNode(node, NodeType::TSMappedType);
}

bool TsParser::tsIsTupleElementName() {
    return lookAhead([&] {
        if (type == &tt::ellipsis) next();
        if (!isIdentOrKeywordToken()) return false;
        next();
        if (type == &tt::colon) return true;
        if (type != &tt::question) return false;
        next();
        return type == &tt::colon;
    });
}

Node* TsParser::tsParseTupleElement() {
    int s = start;
    Pos sl = startLoc;
    if (tsIsTupleElementName()) {
        bool rest = eat(tt::ellipsis);
        Node* member = startNode();
        member->label() = parseIdent(true);
        if (eat(tt::question)) member->SetTs(kTsOptional, true);
        expect(tt::colon);
        // parseTupleElementType
        int es = start;
        Pos el = startLoc;
        Node* et;
        if (eat(tt::ellipsis)) {
            Node* r = startNodeAt(es, el);
            r->typeAnnotation() = tsParseTypeInContext(false);
            et = finishNode(r, NodeType::TSRestType);
        } else {
            et = tsOptionalTupleType(tsParseTypeInContext(false));
        }
        member->elementType() = et;
        finishNode(member, NodeType::TSNamedTupleMember);
        if (!rest) return member;
        Node* r = startNodeAt(s, sl);
        r->typeAnnotation() = member;
        return finishNode(r, NodeType::TSRestType);
    }
    if (eat(tt::ellipsis)) {
        Node* r = startNodeAt(s, sl);
        r->typeAnnotation() = tsParseTypeInContext(false);
        return finishNode(r, NodeType::TSRestType);
    }
    return tsOptionalTupleType(tsParseTypeInContext(false));
}

// parseTupleElementType: a tuple element `T?` (a postfix JSDoc nullable
// type at the top) is an optional element.
Node* TsParser::tsOptionalTupleType(Node* t) {
    if (t->type != NodeType::TSJSDocNullableType || !t->HasTs(kTsPostfix)) return t;
    Node* inner = t->typeAnnotation()->typeAnnotation();
    Node* opt = startNodeAt(static_cast<int>(t->ustart), t->loc_start);
    opt->typeAnnotation() = inner;
    return finishNodeAt(opt, NodeType::TSOptionalType, static_cast<int>(t->uend), t->loc_end);
}

// A JSDoc wrapper type around `inner`, from s to the last token; its
// typeAnnotation is a TSTypeAnnotation (typescript-estree's shape) spanning
// the wrapper (prefix) or the inner type (postfix).
Node* TsParser::tsJsDocWrap(NodeType t, Node* inner, int s, Pos sl, bool postfix) {
    Node* node = startNodeAt(s, sl);
    Node* ann = postfix ? startNodeAt(static_cast<int>(inner->ustart), inner->loc_start) : startNodeAt(s, sl);
    ann->typeAnnotation() = inner;
    node->typeAnnotation() = finishNodeAt(ann, NodeType::TSTypeAnnotation, static_cast<int>(inner->uend), inner->loc_end);
    if (postfix && t != NodeType::TSJSDocOptionalType) node->SetTs(kTsPostfix, true);
    return finishNode(node, t);
}

Node* TsParser::tsParseTupleType() {
    Node* node = startNode();
    expect(tt::bracketL);
    std::vector<Node*> elems;
    bool first = true;
    while (!eat(tt::bracketR)) {
        if (!first) {
            expect(tt::comma);
            if (eat(tt::bracketR)) break;
        }
        first = false;
        elems.push_back(tsParseTupleElement());
    }
    node->elementTypes() = list(elems);
    return finishNode(node, NodeType::TSTupleType);
}

Node* TsParser::tsParseParenthesizedType() {
    expect(tt::parenL);
    Node* t = tsParseTypeInContext(false);
    expect(tt::parenR);
    return t;
}

bool TsParser::tsSkipParameterStart() {
    if (isModifierToken()) {
        Mods m;
        tsParseModifiers(m, false);
    }
    if (isIdentToken() || type == &tt::_this) {
        next();
        return true;
    }
    if (type == &tt::bracketL || type == &tt::braceL) {
        size_t before = out.errors.size();
        parseBindingAtom();
        return out.errors.size() == before;
    }
    return false;
}

bool TsParser::tsIsUnambiguouslyStartOfFunctionType() {
    return lookAhead([&] {
        next();
        if (type == &tt::parenR || type == &tt::ellipsis) return true;
        if (tsSkipParameterStart()) {
            if (type == &tt::colon || type == &tt::comma || type == &tt::question || type == &tt::eq) return true;
            if (type == &tt::parenR) {
                next();
                if (type == &tt::arrow) return true;
            }
        }
        return false;
    });
}

bool TsParser::tsIsStartOfFunctionTypeOrConstructorType() {
    if (isLt()) return true;
    if (type == &tt::parenL && tsIsUnambiguouslyStartOfFunctionType()) return true;
    if (type == &tt::_new) return true;
    if (isWord("abstract")) {
        return lookAhead([&] {
            next();
            return type == &tt::_new;
        });
    }
    return false;
}

Node* TsParser::tsParseFunctionOrConstructorType() {
    Node* node = startNode();
    bool isCtor = false;
    if (isWord("abstract")) {
        node->SetTs(kTsAbstract, true);
        next();
    }
    if (eat(tt::_new)) isCtor = true;
    bool savedType = inType;
    node->typeParameters() = tsTryParseTypeParameters();
    node->params() = list(tsParseParams());
    inType = savedType;
    node->returnType() = tsParseReturnType(true, false);
    return finishNode(node, isCtor ? NodeType::TSConstructorType : NodeType::TSFunctionType);
}

bool TsParser::tsIsStartOfType(bool inStartOfParameter) {
    if (type == &tt::name) {
        if (containsEsc) return isIdentToken();
        static const char* const kStart[] = {"any",    "unknown", "string", "number",  "bigint", "boolean",
                                             "readonly", "symbol", "unique", "undefined", "never", "object",
                                             "infer",  "asserts"};
        for (const char* w : kStart)
            if (value == w) return true;
        return isIdentToken();
    }
    if (type == &tt::_void || type == &tt::_null || type == &tt::_this || type == &tt::_typeof ||
        type == &tt::braceL || type == &tt::bracketL || isLt() || type == &tt::bitwiseOR ||
        type == &tt::bitwiseAND || type == &tt::_new || type == &tt::string || type == &tt::num ||
        type == &tt::_true || type == &tt::_false || type == &tt::star || type == &tt::question ||
        (type == &tt::prefix && value == "!") || type == &tt::ellipsis || type == &tt::_import ||
        type == &tt::backQuote)
        return true;
    if (type == &tt::_function) return !inStartOfParameter;
    if (type == &tt::plusMin && value == "-") {
        if (inStartOfParameter) return false;
        return peek().type == &tt::num;
    }
    if (type == &tt::parenL) {
        if (inStartOfParameter) return false;
        return lookAhead([&] {
            next();
            if (type == &tt::parenR) return true;
            // isStartOfParameter(false)
            if (type == &tt::ellipsis || isIdentToken() || type == &tt::bracketL || type == &tt::braceL ||
                type == &tt::privateId || isModifierToken() || type == &tt::at || type == &tt::_this)
                return true;
            return tsIsStartOfType(false);
        });
    }
    return false;
}

// ---------------------------------------------------------------- type members

void TsParser::tsParseTypeMemberSemicolon() {
    if (eat(tt::comma)) return;
    semicolon();
}

Node* TsParser::tsParsePropertyNameNode(Node* holder) {
    if (type == &tt::privateId) {
        holder->Set(kFlagComputed, false);
        Node* k = startNode();
        k->str = str(value);
        next();
        return holder->key() = finishNode(k, NodeType::PrivateIdentifier);
    }
    if (type == &tt::bracketL) {
        next();
        holder->Set(kFlagComputed, true);
        holder->key() = parseExpression();
        expect(tt::bracketR);
        return holder->key();
    }
    holder->Set(kFlagComputed, false);
    if (type == &tt::num || type == &tt::string) return holder->key() = parseLiteral();
    if (!isIdentOrKeywordToken()) unexpected();
    return holder->key() = parseIdent(true);
}

std::vector<Node*> TsParser::tsParseParams() {
    expect(tt::parenL);
    return parseBindingList(tt::parenR, false, true, true);
}

bool TsParser::tsIsUnambiguouslyIndexSignature() {
    return lookAhead([&] {
        next();  // [
        if (type == &tt::ellipsis || type == &tt::bracketR) return true;
        if (isModifierToken()) {
            next();
            if (isIdentToken()) return true;
        } else if (!isIdentToken()) {
            return false;
        } else {
            next();
        }
        if (type == &tt::colon || type == &tt::comma) return true;
        if (type != &tt::question) return false;
        next();
        return type == &tt::colon || type == &tt::comma || type == &tt::bracketR;
    });
}

bool TsParser::tsIsIndexSignature() { return type == &tt::bracketL && tsIsUnambiguouslyIndexSignature(); }

Node* TsParser::tsParseIndexSignature(Node* node) {
    expect(tt::bracketL);
    std::vector<Node*> params = parseBindingList(tt::bracketR, false, true, true);
    node->parameters() = list(params);
    node->typeAnnotation() = tsTryParseTypeAnnotation();
    return node;
}

Node* TsParser::tsParseSignatureMember(NodeType kind, Node* node) {
    if (kind == NodeType::TSConstructSignatureDeclaration) expect(tt::_new);
    node->typeParameters() = tsTryParseTypeParameters();
    node->params() = list(tsParseParams());
    node->returnType() = tsParseReturnType(false, true);
    tsParseTypeMemberSemicolon();
    return finishNode(node, kind);
}

Node* TsParser::tsParsePropertyOrMethodSignature(Node* node) {
    tsParsePropertyNameNode(node);
    if (eat(tt::question)) node->SetTs(kTsOptional, true);
    if (type == &tt::parenL || isLt()) {
        node->kind = "method";
        node->typeParameters() = tsTryParseTypeParameters();
        node->params() = list(tsParseParams());
        node->returnType() = tsParseReturnType(false, true);
        tsParseTypeMemberSemicolon();
        return finishNode(node, NodeType::TSMethodSignature);
    }
    node->typeAnnotation() = tsTryParseTypeAnnotation();
    if (type == &tt::eq) {
        // tsc parses an initializer for the checker to reject
        next();
        parseMaybeAssign();
    }
    tsParseTypeMemberSemicolon();
    return finishNode(node, NodeType::TSPropertySignature);
}

Node* TsParser::tsParseTypeMember() {
    DepthGuard guard(*this);
    Node* node = startNode();
    if (type == &tt::parenL || isLt()) return tsParseSignatureMember(NodeType::TSCallSignatureDeclaration, node);
    if (type == &tt::_new) {
        bool sig = lookAhead([&] {
            next();
            return type == &tt::parenL || isLt();
        });
        if (sig) return tsParseSignatureMember(NodeType::TSConstructSignatureDeclaration, node);
    }
    Mods m;
    tsParseModifiers(m, false);
    if (m.readonly_) node->SetTs(kTsReadonly, true);
    if (m.static_) node->Set(kFlagStatic, true);
    if (!m.access.empty()) node->accessibility = m.access;
    for (const char* acc : {"get", "set"}) {
        if (isWord(acc)) {
            TsSave s = tsSave();
            bool ok = false;
            try {
                ok = nextTokenCanFollowModifier();
            } catch (const SyntaxErr&) {
                ok = false;
            }
            tsRestore(s);
            if (ok) {
                next();
                node->kind = acc;
                tsParsePropertyNameNode(node);
                node->typeParameters() = tsTryParseTypeParameters();
                node->params() = list(tsParseParams());
                node->returnType() = tsParseReturnType(false, true);
                // parseFunctionBlockOrSemicolon
                if (type == &tt::braceL) {
                    Node* body = parseBlock();
                    (void)body;
                } else {
                    tsParseTypeMemberSemicolon();
                }
                return finishNode(node, NodeType::TSMethodSignature);
            }
        }
    }
    if (tsIsIndexSignature()) {
        tsParseIndexSignature(node);
        tsParseTypeMemberSemicolon();
        return finishNode(node, NodeType::TSIndexSignature);
    }
    return tsParsePropertyOrMethodSignature(node);
}

// ---------------------------------------------------------------- type parameters / arguments

Node* TsParser::tsParseTypeArguments() {
    Node* node = startNode();
    if (!eatLt()) unexpected();
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
    expectGt();
    node->params() = list(params);
    return finishNode(node, NodeType::TSTypeParameterInstantiation);
}

Node* TsParser::tsParseTypeParameter(bool permitModifiers) {
    Node* node = startNode();
    if (permitModifiers) {
        for (;;) {
            if (type == &tt::_const) {
                bool ok = lookAhead([&] {
                    next();
                    return !hasPrecedingLineBreak() && canFollowModifier();
                });
                if (!ok) break;
                next();
                node->SetTs(kTsConst, true);
            } else if (type == &tt::_in || isWord("out")) {
                bool isIn = type == &tt::_in;
                if (!parseAnyContextualModifier()) break;
                node->SetTs(isIn ? kTsIn : kTsOut, true);
            } else if (isModifierToken()) {
                if (!parseAnyContextualModifier()) break;
            } else {
                break;
            }
        }
    }
    if (!isIdentToken()) unexpected();
    node->name() = parseIdent(true);
    if (eat(tt::_extends)) {
        if (tsIsStartOfType() || !tsIsStartOfExpression())
            node->constraint() = tsParseTypeInContext(false);
        else
            node->constraint() = parseMaybeUnary(nullptr, false, false, ForInit::No);
    }
    if (eat(tt::eq)) node->defaultType() = tsParseTypeInContext(false);
    return finishNode(node, NodeType::TSTypeParameter);
}

Node* TsParser::tsTryParseTypeParameters() {
    if (!isLt()) return nullptr;
    Node* node = startNode();
    eatLt();
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
        params.push_back(tsParseTypeParameter(true));
    }
    inType = savedType;
    expectGt();
    node->params() = list(params);
    return finishNode(node, NodeType::TSTypeParameterDeclaration);
}

}  // namespace jsparse
