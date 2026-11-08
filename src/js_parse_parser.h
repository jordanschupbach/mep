#pragma once

// jsparse::Parser: the port of acorn 8.15.0's Parser (tokenizer, token
// contexts, statement/expression/lval parsing, scopes, the RegExp
// validator) and JsxParser, the port of acorn-jsx 5.3.2. Method names are
// acorn's, so a line of acorn and its port can be read side by side.
//
// Internally everything is in UTF-16 code units, like acorn's `input`
// string: positions (pos, start, end, ustart/uend) are UTF-16 offsets;
// finishNode also fills the byte offsets. Token values and node strings are
// WTF-8 std::strings / string_views.
//
// Extension: the methods acorn plugins override are virtual. JsxParser
// overrides readToken / updateContext / parseExprAtom as acorn-jsx does.
// TsParser (js_parse_ts.h) derives from JsxParser for TypeScript and
// overrides the same way acorn-typescript overrides acorn (parseStatement
// for interfaces and enums, parseExprOp for `as` and `satisfies`, ...),
// adding its own TokenType and NodeType values.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "js_parse.h"

namespace jsparse {

bool IsIdentifierStart(int code, bool astral = true);
bool IsIdentifierChar(int code, bool astral = true);
int XhtmlEntityCode(std::u16string_view name);  // -1 if not an entity

// Bump allocator for nodes and strings.
class Arena {
public:
    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena();
    void* Alloc(size_t size, size_t align);
    Node* NewNode();
    std::string_view Str(std::string_view s);
    NodeList List(const std::vector<Node*>& v);

private:
    std::vector<char*> blocks_;
    char* cur_ = nullptr;
    size_t left_ = 0;
};

// UTF-16 <-> WTF-8 helpers.
void AppendWtf8(std::string& out, uint32_t cp);
std::string ToWtf8(std::u16string_view s);
bool HasLoneSurrogate(std::string_view wtf8);

struct TokenType {
    const char* label;
    const char* keyword;  // nullptr unless a keyword token
    bool beforeExpr;
    bool startsExpr;
    bool isLoop;
    bool isAssign;
    bool prefix;
    bool postfix;
    int binop;  // -1: not a binary operator
};

namespace tt {
extern const TokenType num, regexp, string, name, privateId, eof;
extern const TokenType bracketL, bracketR, braceL, braceR, parenL, parenR, comma, semi, colon, dot, question,
    questionDot, arrow, template_, invalidTemplate, ellipsis, backQuote, dollarBraceL;
extern const TokenType eq, assign, incDec, prefix, logicalOR, logicalAND, bitwiseOR, bitwiseXOR, bitwiseAND,
    equality, relational, bitShift, plusMin, modulo, star, slash, starstar, coalesce;
extern const TokenType _break, _case, _catch, _continue, _debugger, _default, _do, _else, _finally, _for,
    _function, _if, _return, _switch, _throw, _try, _var, _const, _while, _with, _new, _this, _super, _class,
    _extends, _export, _import, _null, _true, _false, _in, _instanceof, _typeof, _void, _delete;
// acorn-jsx
extern const TokenType jsxName, jsxText, jsxTagStart, jsxTagEnd;
const TokenType* Keyword(std::string_view word);  // the keyword token type for a word, or nullptr
}  // namespace tt

struct TokContext {
    const char* token;
    bool isExpr;
    bool preserveSpace;
    bool overrideTemplate;  // q_tmpl: the context's override reads a template token
    bool generator;
};

namespace tc {
extern const TokContext b_stat, b_expr, b_tmpl, p_stat, p_expr, q_tmpl, f_stat, f_expr, f_expr_gen, f_gen;
extern const TokContext j_oTag, j_cTag, j_expr;  // acorn-jsx
}  // namespace tc

// acorn's SyntaxError.
struct SyntaxErr {
    std::string message;
    int pos;
    int raisedAt;
};

// Scope flags and binding kinds (acorn's SCOPE_* / BIND_*).
enum : int {
    SCOPE_TOP = 1,
    SCOPE_FUNCTION = 2,
    SCOPE_ASYNC = 4,
    SCOPE_GENERATOR = 8,
    SCOPE_ARROW = 16,
    SCOPE_SIMPLE_CATCH = 32,
    SCOPE_SUPER = 64,
    SCOPE_DIRECT_SUPER = 128,
    SCOPE_CLASS_STATIC_BLOCK = 256,
    SCOPE_CLASS_FIELD_INIT = 512,
    SCOPE_VAR = SCOPE_TOP | SCOPE_FUNCTION | SCOPE_CLASS_STATIC_BLOCK,
};
enum : int { BIND_NONE = 0, BIND_VAR = 1, BIND_LEXICAL = 2, BIND_FUNCTION = 3, BIND_SIMPLE_CATCH = 4, BIND_OUTSIDE = 5 };
enum : int { FUNC_STATEMENT = 1, FUNC_HANGING_STATEMENT = 2, FUNC_NULLABLE_ID = 4 };

struct Scope {
    int flags;
    std::vector<std::string_view> var, lexical, functions;
};

struct Label {
    std::string_view name;
    int kind;  // 0: none (null), 1: loop, 2: switch
    int statementStart;
};

struct DestructuringErrors {
    int shorthandAssign = -1;
    int trailingComma = -1;
    int parenthesizedAssign = -1;
    int parenthesizedBind = -1;
    int doubleProto = -1;
};

struct PrivateNameScope {
    std::vector<std::pair<std::string_view, std::string_view>> declared;  // name -> "true"/"iget"/...
    std::vector<Node*> used;
};

// forInit in acorn is false / true / "await".
enum class ForInit : uint8_t { No, Yes, Await };

class Parser {
public:
    Parser(const Options& options, std::u16string_view input, Arena& arena, ParseResult& out);
    virtual ~Parser();
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    Node* parse();
    // parse(), with an error that escapes the recovery recorded (the
    // result is then null).
    Node* parseRecording();

protected:
    // ---- options ----
    int ecmaVersion;
    SourceType sourceType;
    bool allowReserved;
    bool allowHashBang;
    bool allowReturnOutsideFunction;
    bool allowImportExportEverywhere;
    bool allowAwaitOutsideFunction;
    bool allowSuperOutsideMethod;
    bool checkPrivateFields;
    bool preserveParens;
    bool recover;
    bool collectTokens;
    int maxDepth;
    int keywordSet;  // which keyword list: 5, 6 or 50 ("5module")
    std::vector<std::string_view> reservedWords, reservedWordsStrict, reservedWordsStrictBind;

    Arena& arena;
    ParseResult& out;
    std::u16string_view input;
    int inputLen;

    // ---- tokenizer state ----
    bool containsEsc = false;
    int pos = 0, lineStart = 0, curLine = 1;
    const TokenType* type;
    std::string value;          // WTF-8: names, strings, templates, operators, keywords
    bool valueIsNull = false;   // number tokens' bigint failed / regex: never
    double numValue = 0;        // num tokens
    bool numIsBigInt = false;   // num tokens: value is a BigInt (bigintValue is its decimal string)
    std::string bigintValue;
    std::string regexPattern, regexFlags;  // regexp tokens
    int start = 0, end = 0;
    Pos startLoc, endLoc;
    int lastTokStart = 0, lastTokEnd = 0;
    Pos lastTokStartLoc, lastTokEndLoc;
    std::vector<const TokContext*> context;
    bool exprAllowed = true;
    bool inModule;
    bool strict;
    int potentialArrowAt = -1;
    bool potentialArrowInForAwait = false;
    int yieldPos = 0, awaitPos = 0, awaitIdentPos = 0;
    std::vector<Label> labels;
    std::vector<std::pair<std::string_view, Node*>> undefinedExports;
    std::vector<Scope> scopeStack;
    std::vector<PrivateNameScope> privateNameStack;
    bool inTemplateElement = false;
    // TypeScript: never enter strict mode ("use strict" directives and class
    // bodies leave it off; tsc's parser has no strict-mode errors).
    bool neverStrict = false;
    // TypeScript: no RegExp pattern/flag validation (tsc's checker does it).
    bool skipRegExpValidation = false;
    int depth = 0;
    int sawErrors = 0;  // errors recorded so far (recovery)

    // ---- helpers ----
    int charAt(int i) const { return i >= 0 && i < inputLen ? input[static_cast<size_t>(i)] : -1; }
    std::u16string_view slice(int a, int b) const;
    std::string sliceStr(int a, int b) const { return ToWtf8(slice(a, b)); }
    bool lineBreakIn(int a, int b) const;  // lineBreak.test(input.slice(a, b))
    int lineIndentAt(int p) const;         // columns of leading space/tab on p's line (recovery)
    int skipWhiteSpaceAt(int p) const;     // skipWhiteSpace regex from p, returns end
    bool isKeywordWord(std::string_view w) const;
    static bool inList(const std::vector<std::string_view>& l, std::string_view w);
    Pos curPosition() const { return Pos{static_cast<uint32_t>(curLine), static_cast<uint32_t>(pos - lineStart)}; }
    Pos lineInfo(int offset) const;
    std::string_view str(std::string_view s) { return arena.Str(s); }
    NodeList list(const std::vector<Node*>& v) { return arena.List(v); }

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser);
        ~DepthGuard() { --p.depth; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
    };

    // ---- errors ----
    [[noreturn]] void raise(int pos, const std::string& message);
    // acorn's raiseRecoverable: an error the parse can continue past (acorn
    // throws it like any other; acorn-loose ignores it). With recovery on,
    // it is recorded and parsing continues in place -- the first error is
    // still the one acorn throws, and the statement around it survives.
    virtual void raiseRecoverable(int at, const std::string& message);
    bool quietRecoverable = false;  // resync's token scan: drop them
    [[noreturn]] void unexpected(int pos = -1);
    void recordError(const SyntaxErr& e);

    // ---- state accessors (acorn's prototypeAccessors) ----
    bool inFunction() { return (currentVarScope().flags & SCOPE_FUNCTION) > 0; }
    bool inGenerator() { return (currentVarScope().flags & SCOPE_GENERATOR) > 0; }
    bool inAsync() { return (currentVarScope().flags & SCOPE_ASYNC) > 0; }
    bool canAwait();
    bool allowSuper() { return (currentThisScope().flags & SCOPE_SUPER) > 0 || allowSuperOutsideMethod; }
    bool allowDirectSuper() { return (currentThisScope().flags & SCOPE_DIRECT_SUPER) > 0; }
    bool treatFunctionsAsVar() { return treatFunctionsAsVarInScope(currentScope()); }
    bool allowNewDotTarget();
    bool inClassStaticBlock() { return (currentVarScope().flags & SCOPE_CLASS_STATIC_BLOCK) > 0; }

    // ---- parseutil ----
    bool strictDirective(int start);
    bool eat(const TokenType& t);
    bool isContextual(std::string_view name) const;
    bool eatContextual(std::string_view name);
    void expectContextual(std::string_view name);
    bool canInsertSemicolon() const;
    bool insertSemicolon() { return canInsertSemicolon(); }
    void semicolon();
    bool afterTrailingComma(const TokenType& tokType, bool notNext = false);
    void expect(const TokenType& t);
    virtual void checkPatternErrors(DestructuringErrors* r, bool isAssign);
    virtual bool checkExpressionErrors(DestructuringErrors* r, bool andThrow = false);
    virtual void checkYieldAwaitInDefaultParams();
    bool isSimpleAssignTarget(Node* expr);

    // ---- node ----
    Node* startNode();
    Node* startNodeAt(int pos, Pos loc);
    Node* finishNode(Node* node, NodeType type);
    Node* finishNodeAt(Node* node, NodeType type, int pos, Pos loc);
    Node* copyNode(Node* node);

    // ---- scope ----
    void enterScope(int flags) { scopeStack.push_back(Scope{flags, {}, {}, {}}); }
    void exitScope() { scopeStack.pop_back(); }
    bool treatFunctionsAsVarInScope(const Scope& scope) const;
    void declareName(std::string_view name, int bindingType, int pos);
    void checkLocalExport(Node* id);
    Scope& currentScope() { return scopeStack.back(); }
    Scope& currentVarScope();
    Scope& currentThisScope();
    void removeUndefinedExport(std::string_view name);

    // ---- tokenizer ----
    virtual void next(bool ignoreEscapeSequenceInKeyword = false);
    virtual void nextToken();
    virtual void readToken(int code);
    virtual void getTokenFromCode(int code);
    virtual void updateContext(const TokenType* prevType);
    void updateContextBase(const TokenType* prevType);
    const TokContext* curContext() const { return context.empty() ? nullptr : context.back(); }
    bool braceIsBlock(const TokenType* prevType);
    bool inGeneratorContext() const;
    void overrideContext(const TokContext* tokenCtx);
    int fullCharCodeAtPos() const;
    void skipBlockComment();
    void skipLineComment(int startSkip);
    void skipSpace();
    void finishToken(const TokenType& type);
    void finishToken(const TokenType& type, std::u16string_view val);
    void finishOp(const TokenType& type, int size);
    void readToken_dot();
    void readToken_slash();
    void readToken_mult_modulo_exp(int code);
    void readToken_pipe_amp(int code);
    void readToken_caret();
    void readToken_plus_min(int code);
    void readToken_lt_gt(int code);
    void readToken_eq_excl(int code);
    void readToken_question();
    void readToken_numberSign();
    void readRegexp();
    bool readInt(int radix, int len, bool maybeLegacyOctal, double& out);  // false: acorn's null
    void readRadixNumber(int radix);
    void readNumber(bool startsWithDot);
    int readCodePoint();
    void readString(int quote);
    void tryReadTemplateToken();
    void invalidStringToken(int position, const std::string& message);
    void readTmplToken();
    void readInvalidTemplateToken();
    void readEscapedChar(bool inTemplate, std::u16string& out);
    int readHexChar(int len);
    std::u16string readWord1();
    void readWord();

    // ---- statements ----
    Node* parseTopLevel(Node* node);
    bool isLet(const char* context = nullptr);
    bool isAsyncFunction();
    bool isUsingKeyword(bool isAwaitUsing, bool isFor);
    bool isAwaitUsing(bool isFor) { return isUsingKeyword(true, isFor); }
    bool isUsing(bool isFor) { return isUsingKeyword(false, isFor); }
    virtual Node* parseStatement(const char* context, bool topLevel = false, std::vector<std::string>* exports = nullptr);
    Node* parseStatementRecovering(const char* context, bool topLevel, std::vector<std::string>* exports);
    virtual Node* parseBreakContinueStatement(Node* node, const char* keyword);
    virtual Node* parseDebuggerStatement(Node* node);
    virtual Node* parseDoStatement(Node* node);
    virtual Node* parseForStatement(Node* node);
    virtual Node* parseForAfterInit(Node* node, Node* init, int awaitAt);
    virtual Node* parseFunctionStatement(Node* node, bool isAsync, bool declarationPosition);
    virtual Node* parseIfStatement(Node* node);
    virtual Node* parseReturnStatement(Node* node);
    virtual Node* parseSwitchStatement(Node* node);
    virtual Node* parseThrowStatement(Node* node);
    virtual Node* parseCatchClauseParam();
    virtual Node* parseTryStatement(Node* node);
    virtual Node* parseVarStatement(Node* node, std::string_view kind, bool allowMissingInitializer = false);
    virtual Node* parseWhileStatement(Node* node);
    virtual Node* parseWithStatement(Node* node);
    virtual Node* parseEmptyStatement(Node* node);
    virtual Node* parseLabeledStatement(Node* node, const std::string& maybeName, Node* expr, const char* context);
    virtual Node* parseExpressionStatement(Node* node, Node* expr);
    virtual Node* parseBlock(bool createNewLexicalScope = true, Node* node = nullptr, bool exitStrict = false);
    virtual Node* parseFor(Node* node, Node* init);
    virtual Node* parseForIn(Node* node, Node* init);
    virtual Node* parseVar(Node* node, bool isFor, std::string_view kind, bool allowMissingInitializer = false);
    virtual void parseVarId(Node* decl, std::string_view kind);
    virtual Node* parseFunction(Node* node, int statement, bool allowExpressionBody = false, bool isAsync = false,
                                ForInit forInit = ForInit::No);
    virtual void parseFunctionParams(Node* node);
    virtual Node* parseClass(Node* node, int isStatement);  // 0 false, 1 true, 2 "nullableID"
    virtual Node* parseClassElement(bool constructorAllowsSuper);
    virtual bool isClassElementNameStart();
    virtual void parseClassElementName(Node* element);
    virtual Node* parseClassMethod(Node* method, bool isGenerator, bool isAsync, bool allowsDirectSuper);
    virtual Node* parseClassField(Node* field);
    virtual Node* parseClassStaticBlock(Node* node);
    virtual void parseClassId(Node* node, int isStatement);
    virtual void parseClassSuper(Node* node);
    PrivateNameScope& enterClassBody();
    void exitClassBody();
    virtual Node* parseExportAllDeclaration(Node* node, std::vector<std::string>* exports);
    virtual Node* parseExport(Node* node, std::vector<std::string>* exports);
    virtual Node* parseExportDeclaration(Node* node);
    virtual Node* parseExportDefaultDeclaration();
    void checkExport(std::vector<std::string>* exports, std::string_view name, int pos);
    void checkExportNode(std::vector<std::string>* exports, Node* name, int pos);
    void checkPatternExport(std::vector<std::string>* exports, Node* pat);
    void checkVariableExport(std::vector<std::string>* exports, const NodeList& decls);
    virtual bool shouldParseExportStatement();
    virtual Node* parseExportSpecifier(std::vector<std::string>* exports);
    NodeList parseExportSpecifiers(std::vector<std::string>* exports);
    virtual Node* parseImport(Node* node);
    virtual Node* parseImportSpecifier();
    virtual Node* parseImportDefaultSpecifier();
    virtual Node* parseImportNamespaceSpecifier();
    NodeList parseImportSpecifiers();
    NodeList parseWithClause();
    virtual Node* parseImportAttribute();
    virtual Node* parseModuleExportName();
    void adaptDirectivePrologue(const NodeList& statements);
    bool isDirectiveCandidate(Node* statement);

    // ---- lval ----
    virtual Node* toAssignable(Node* node, bool isBinding, DestructuringErrors* r = nullptr);
    virtual void toAssignableList(Node** items, size_t n, bool isBinding);
    virtual Node* parseSpread(DestructuringErrors* r);
    virtual Node* parseRestBinding();
    virtual Node* parseBindingAtom();
    virtual std::vector<Node*> parseBindingList(const TokenType& close, bool allowEmpty, bool allowTrailingComma,
                                                bool allowModifiers = false);
    virtual Node* parseAssignableListItem(bool allowModifiers);
    virtual Node* parseBindingListItem(Node* param) { return param; }
    virtual Node* parseMaybeDefault(int startPos, Pos startLoc, Node* left = nullptr);
    virtual void checkLValSimple(Node* expr, int bindingType = BIND_NONE, std::vector<std::string_view>* checkClashes = nullptr);
    virtual void checkLValPattern(Node* expr, int bindingType = BIND_NONE, std::vector<std::string_view>* checkClashes = nullptr);
    virtual void checkLValInnerPattern(Node* expr, int bindingType = BIND_NONE,
                                       std::vector<std::string_view>* checkClashes = nullptr);

    // ---- expressions ----
    struct PropHashEntry {
        std::string name;
        bool init = false, get = false, set = false;
    };
    struct PropHash {
        bool proto = false;
        std::vector<PropHashEntry> entries;
    };
    void checkPropClash(Node* prop, PropHash& propHash, DestructuringErrors* r);
    virtual Node* parseExpression(ForInit forInit = ForInit::No, DestructuringErrors* r = nullptr);
    // afterLeftParse: acorn's parseParenItem callback (the only one acorn passes)
    virtual Node* parseMaybeAssign(ForInit forInit = ForInit::No, DestructuringErrors* r = nullptr,
                                   bool afterLeftParseParenItem = false);
    virtual Node* parseMaybeConditional(ForInit forInit, DestructuringErrors* r);
    virtual Node* parseExprOps(ForInit forInit, DestructuringErrors* r);
    virtual Node* parseExprOp(Node* left, int leftStartPos, Pos leftStartLoc, int minPrec, ForInit forInit);
    Node* buildBinary(int startPos, Pos startLoc, Node* left, Node* right, std::string_view op, bool logical);
    virtual Node* parseMaybeUnary(DestructuringErrors* r, bool sawUnary, bool incDec, ForInit forInit);
    virtual Node* parseExprSubscripts(DestructuringErrors* r, ForInit forInit);
    virtual Node* parseSubscripts(Node* base, int startPos, Pos startLoc, bool noCalls, ForInit forInit);
    virtual bool shouldParseAsyncArrow();
    virtual Node* parseSubscriptAsyncArrow(int startPos, Pos startLoc, std::vector<Node*>& exprList, ForInit forInit);
    virtual Node* parseSubscript(Node* base, int startPos, Pos startLoc, bool noCalls, bool maybeAsyncArrow,
                                 bool optionalChained, ForInit forInit);
    virtual Node* parseExprAtom(DestructuringErrors* r = nullptr, ForInit forInit = ForInit::No, bool forNew = false);
    Node* parseExprAtomBase(DestructuringErrors* r, ForInit forInit, bool forNew);
    virtual Node* parseExprAtomDefault();
    virtual Node* parseExprImport(bool forNew);
    virtual Node* parseDynamicImport(Node* node);
    virtual Node* parseImportMeta(Node* node);
    virtual Node* parseLiteral();
    virtual Node* parseParenExpression();
    virtual bool shouldParseArrow(std::vector<Node*>& exprList);
    virtual Node* parseParenAndDistinguishExpression(bool canBeArrow, ForInit forInit);
    virtual Node* parseParenItem(Node* item) { return item; }
    virtual Node* parseParenArrowList(int startPos, Pos startLoc, std::vector<Node*>& exprList, ForInit forInit);
    virtual Node* parseNew();
    virtual Node* parseTemplateElement(bool isTagged);
    virtual Node* parseTemplate(bool isTagged = false);
    bool isAsyncProp(Node* prop);
    virtual Node* parseObj(bool isPattern, DestructuringErrors* r = nullptr);
    virtual Node* parseProperty(bool isPattern, DestructuringErrors* r);
    virtual void parseGetterSetter(Node* prop);
    virtual void parsePropertyValue(Node* prop, bool isPattern, bool isGenerator, bool isAsync, int startPos,
                                    Pos startLoc, DestructuringErrors* r, bool containsEsc);
    virtual Node* parsePropertyName(Node* prop);
    void initFunction(Node* node);
    virtual Node* parseMethod(bool isGenerator, bool isAsync = false, bool allowDirectSuper = false);
    virtual Node* parseArrowExpression(Node* node, std::vector<Node*>& params, bool isAsync, ForInit forInit);
    virtual void parseFunctionBody(Node* node, bool isArrowFunction, bool isMethod, ForInit forInit);
    bool isSimpleParamList(const NodeList& params);
    void checkParams(Node* node, bool allowDuplicates);
    virtual std::vector<Node*> parseExprList(const TokenType& close, bool allowTrailingComma, bool allowEmpty,
                                             DestructuringErrors* r = nullptr);
    virtual void checkUnreserved(Node* ref);
    virtual Node* parseIdent(bool liberal = false);
    virtual Node* parseIdentNode();
    virtual Node* parsePrivateIdent();
    virtual Node* parseYield(ForInit forInit);
    virtual Node* parseAwait(ForInit forInit);

    // ---- regexp ----
    // validateRegExpFlags + validateRegExpPattern (js_parse_regexp.cpp)
    void validateRegExpFlagsAndPattern(int start, std::u16string_view pattern, std::u16string_view flags);
    friend struct RegexpParser;

    // ---- recovery ----
    struct Snapshot {
        size_t scopeDepth;
        std::vector<const TokContext*> context;
        std::vector<Label> labels;
        size_t privateDepth;
        bool strict;
        int yieldPos, awaitPos, awaitIdentPos;
        int potentialArrowAt;
        bool potentialArrowInForAwait;
        bool inTemplateElement;
        int depth;
        // token state at the snapshot
        int pos, lineStart, curLine, start, end, lastTokStart, lastTokEnd;
        Pos startLoc, endLoc, lastTokStartLoc, lastTokEndLoc;
        const TokenType* type;
        std::string value;
        bool exprAllowed;
        bool containsEsc;
        size_t commentCount, tokenCount;
    };
    Snapshot takeSnapshot() const;
    void restoreSnapshot(const Snapshot& s);
    // After an error in a statement (or class member) that began at the
    // snapshot: skip to where parsing can resume. Returns false at EOF.
    void resync(const Snapshot& s, int errorPos, bool inClassBody, bool topLevel);
};

class JsxParser : public Parser {
public:
    using Parser::Parser;

protected:
    void jsx_readToken();
    std::u16string jsx_readNewLine(bool normalizeCRLF);
    void jsx_readString(int quote);
    std::u16string jsx_readEntity();
    void jsx_readWord();
    Node* jsx_parseIdentifier();
    Node* jsx_parseNamespacedName();
    Node* jsx_parseElementName();  // nullptr: acorn's ''
    virtual Node* jsx_parseAttributeValue();
    Node* jsx_parseEmptyExpression();
    virtual Node* jsx_parseExpressionContainer();
    Node* jsx_parseAttribute();
    virtual Node* jsx_parseOpeningElementAt(int startPos, Pos startLoc);
    Node* jsx_parseClosingElementAt(int startPos, Pos startLoc);
    Node* jsx_parseElementAt(int startPos, Pos startLoc);
    Node* jsx_parseText();
    Node* jsx_parseElement();

    Node* parseExprAtom(DestructuringErrors* r = nullptr, ForInit forInit = ForInit::No, bool forNew = false) override;
    void readToken(int code) override;
    void updateContext(const TokenType* prevType) override;
};

}  // namespace jsparse
