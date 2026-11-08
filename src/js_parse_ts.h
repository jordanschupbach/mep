#pragma once

// jsparse::TsParser: the TypeScript dialect of mep's JavaScript parser
// (Options::typescript), on top of the acorn port (Parser) and its JSX
// layer (JsxParser), the way acorn-typescript sits on acorn. The trees are
// typescript-estree's (TSTypeAnnotation, TSInterfaceDeclaration, ...; see
// js_parse.h for where each field lives); what parses -- and how an
// ambiguity like `a < b > (c)` or `<T>(x) => x` resolves -- follows
// TypeScript's own parser (src/compiler/parser.ts, TypeScript 5.9), which
// tools/js_parse_ts_check.py checks file by file against tsc.
//
//   js_parse_ts.cpp             tokenizer hooks, speculation, types
//   js_parse_ts_statement.cpp   declarations, modifiers, classes, modules
//   js_parse_ts_expression.cpp  expressions, arrow functions, parameters
//
// Speculation. tsc decides several things by parsing ahead and rolling
// back (lookAhead / tryParse). Here that is Parser's recovery snapshot plus
// the error list: lookAhead() always rolls back, tryParse() rolls back
// when the parse throws or gives up (returns null).
//
// Errors. tsc's parser accepts a lot that its binder and checker then
// reject (duplicate declarations, strict-mode identifiers, `break` outside
// a loop, modifiers in the wrong place, `1 = 2`, ...). A TS parse reports
// what tsc's parser reports, so acorn's early errors of that kind are
// dropped (raiseRecoverable filters them, the lvalue checks are off, there
// is no strict mode). The messages keep acorn's style ("Unexpected token").

#include <string>
#include <vector>

#include "js_parse_parser.h"

namespace jsparse {

namespace tt {
extern const TokenType at;  // `@` (decorators)
}  // namespace tt

class TsParser : public JsxParser {
public:
    TsParser(const Options& options, std::u16string_view input, Arena& arena, ParseResult& out);

    // tsc's reparseTopLevelAwait: Parse() parses a module file again with
    // the top level an await context when the first parse met `await` there.
    bool topLevelAwait = false;
    bool sawTopLevelAwait = false;
    // processPragmasIntoFields: tsc's errors for the file's leading `///`
    // directives (bad <reference>, two <amd-module> names), which come after
    // every syntax error -- Parse() calls it when there was none.
    void tsCheckPragmas();

protected:
    bool tsx;  // JSX on (a .tsx file)
    bool dts;  // a declaration file

    // ---- context (tsc's contextFlags that matter to the parse) ----
    bool inDisallowConditionalTypes = false;
    bool inDecorator = false;     // `@a.b[c]`: no element access
    bool inType = false;          // parsing a type (for token re-reads)
    int inAmbient = 0;            // inside `declare` / a .d.ts
    bool pendingNoArrowReturnType = false;  // parseMaybeAssign's allowReturnTypeInArrowFunction (false)
    bool curAllowArrowReturnType = true;

    // ---- speculation ----
    struct TsSave {
        Snapshot snap;
        size_t errors;
        int sawErrors;
        bool hasError;
        uint32_t firstErrorOffset;
        bool inDisallowConditionalTypes, inDecorator, inType;
        int inAmbient;
    };
    TsSave tsSave() const;
    void tsRestore(const TsSave& s);
    template <class F>
    bool lookAhead(F&& f) {
        TsSave s = tsSave();
        bool r = false;
        try {
            r = f();
        } catch (const SyntaxErr&) {
            r = false;
        }
        tsRestore(s);
        return r;
    }
    template <class F>
    Node* tryParse(F&& f) {
        TsSave s = tsSave();
        Node* r = nullptr;
        try {
            r = f();
        } catch (const SyntaxErr&) {
            r = nullptr;
        }
        if (!r) tsRestore(s);
        return r;
    }

    // ---- token helpers ----
    bool isWord(std::string_view w) const { return type == &tt::name && value == w && !containsEsc; }
    bool isIdentToken();                       // tsc's isIdentifier()
    bool isIdentOrKeywordToken() const;        // tokenIsIdentifierOrKeyword
    bool isLiteralPropertyNameToken() const;   // identifier/keyword, string, number
    bool isModifierToken() const;              // isModifierKind
    bool isLt();                               // `<` (re-reads a JSX tag start / `<<`)
    bool isGtStart() const;                    // a token starting with `>`
    void expectGt();                           // `>`, splitting `>>`, `>=`, ...
    bool eatLt();
    void splitToken(int len, const TokenType& t, const char* val);
    void reReadLtAsRelational();
    bool hasPrecedingLineBreak() const { return lineBreakIn(lastTokEnd, start); }
    // The next token (one token of lookahead).
    struct Peek {
        const TokenType* type;
        std::string value;
        bool lineBreakBefore;
        bool esc;
        int start;
    };
    Peek peek();
    bool peekIs(const TokenType& t) { return peek().type == &t; }
    bool nextTokenIsIdentOnSameLine();
    bool nextTokenIsIdentOrKeywordOnSameLine();
    bool nextTokenCanFollowModifier();
    bool canFollowModifier() const;
    bool parseAnyContextualModifier();         // isModifierKind && tryParse(nextTokenCanFollowModifier)
    Node* finishAt(Node* n, NodeType t, Node* endNode);
    Node* extendTo(Node* n);                   // set n's end to the last token's end

    // ---- types (js_parse_ts.cpp) ----
    Node* tsParseType();
    Node* tsParseTypeInContext(bool disallowConditional);
    Node* tsParseTypeAnnotation();             // at `:` -> TSTypeAnnotation (from the colon)
    Node* tsTryParseTypeAnnotation();          // `:`? -> TSTypeAnnotation or null
    Node* tsParseReturnType(bool arrow, bool isType);  // `:` / `=>` T, or null
    Node* tsWrapAnnotation(int startPos, Pos startLoc, Node* type);
    Node* tsParseTypeOrTypePredicate();
    Node* tsParseUnionOrIntersection(bool isUnion);
    Node* tsParseTypeOperatorOrHigher();
    Node* tsParsePostfixTypeOrHigher();
    Node* tsParseNonArrayType();
    Node* tsParseKeywordType(NodeType t);
    Node* tsParseTypeReference();
    Node* tsParseEntityName(bool allowReservedWords, bool thisExpression = false);
    Node* tsParseTypeQuery();
    Node* tsParseImportType(int startPos, Pos startLoc, bool isTypeOf);
    Node* tsParseLiteralType(bool negative);
    Node* tsParseTemplateType();
    Node* tsParseTypeLiteralOrMapped();
    Node* tsParseMappedType();
    Node* tsOptionalTupleType(Node* t);
    Node* tsJsDocWrap(NodeType t, Node* inner, int s, Pos sl, bool postfix);
    Node* tsParseTupleType();
    Node* tsParseTupleElement();
    Node* tsParseParenthesizedType();
    Node* tsParseFunctionOrConstructorType();
    Node* tsParseInferType();
    Node* tsParseThisTypePredicate(Node* thisType);
    Node* tsParseAssertsTypePredicate();
    Node* tsParseTypeMember();
    Node* tsParseSignatureMember(NodeType kind, Node* node);
    Node* tsParseIndexSignature(Node* node);
    Node* tsParsePropertyOrMethodSignature(Node* node);
    std::vector<Node*> tsParseObjectTypeMembers();
    Node* tsParseTypeArguments();              // at `<` -> TSTypeParameterInstantiation
    Node* tsTryParseTypeArguments() { return isLt() ? tsParseTypeArguments() : nullptr; }
    Node* tsParseTypeArgumentsInExpression();  // tryParse body: null unless it can follow
    Node* tsTryParseTypeParameters();          // `<`? -> TSTypeParameterDeclaration or null
    Node* tsParseTypeParameter(bool permitModifiers);
    bool tsIsStartOfType(bool inStartOfParameter = false);
    bool tsIsStartOfFunctionTypeOrConstructorType();
    bool tsIsUnambiguouslyStartOfFunctionType();
    bool tsSkipParameterStart();
    bool tsIsStartOfMappedType();
    bool tsIsUnambiguouslyIndexSignature();
    bool tsIsIndexSignature();
    bool tsIsTupleElementName();
    bool tsIsTypeMemberStart();
    Node* tsParsePropertyNameNode(Node* holder);  // fills holder->key and computed
    std::vector<Node*> tsParseParams();        // `(` params `)` (functions, signatures, function types)
    void tsParseTypeMemberSemicolon();

    // ---- statements (js_parse_ts_statement.cpp) ----
    struct Mods {
        std::vector<Node*> decorators;
        int start = -1;  // first modifier or decorator
        Pos startLoc;
        int afterDecorators = -1;  // start of the first modifier after leading decorators
        Pos afterDecoratorsLoc;
        bool exp = false, def = false, decl = false, abstract_ = false, async_ = false, const_ = false;
        bool static_ = false, readonly_ = false, override_ = false, accessor_ = false, in_ = false, out_ = false;
        std::string_view access;
        int count = 0;  // modifiers (not decorators)
        int exportPos = -1;
        Pos exportLoc;
        int innerStart = -1;  // first modifier after `export` / `export default`
        Pos innerLoc;
    };
    Node* tsParseDecorator();
    std::vector<Node*> tsParseDecorators();
    bool tsTryParseModifier(Mods& m, bool permitConst, bool stopOnStaticBlock);
    void tsParseModifiers(Mods& m, bool allowDecorators, bool permitConst = false, bool stopOnStaticBlock = false);
    bool tsIsDeclaration();
    bool tsIsStartOfDeclaration() { return lookAhead([&] { return tsIsDeclaration(); }); }
    Node* tsParseDeclaration(std::vector<std::string>* exports);
    Node* tsParseDeclarationWorker(Mods& m, int innerStart, Pos innerLoc);
    Node* tsParseInterface(Node* node);
    Node* tsParseTypeAlias(Node* node);
    Node* tsParseEnum(Node* node, bool isConst);
    Node* tsParseModule(Node* node);
    Node* tsParseModuleOrNamespace(Node* node, bool nested);
    Node* tsParseModuleBlock();
    Node* tsParseImportDeclaration(Node* node);
    Node* tsParseImportEquals(Node* node, Node* id, bool isTypeOnly);
    Node* tsParseExportDeclaration(Node* node, std::vector<std::string>* exports);
    Node* tsParseExportAssignment(Node* node);
    Node* tsParseHeritageExpression(NodeType t);
    Node* tsParseClassTs(Node* node, bool isStatement, Mods* mods);
    Node* tsParseClassMember(bool hasSuper);
    Node* tsParseClassMethodTs(Node* method, bool isGenerator, bool isAsync, bool allowsDirectSuper);
    Node* tsParseFunctionTs(Node* node, bool statement, bool isAsync, bool nullableId, bool declare);
    Node* tsParseModuleExportName();
    Node* tsParseImportOrExportSpecifier(bool isImport);

    Node* parseStatement(const char* context, bool topLevel = false, std::vector<std::string>* exports = nullptr) override;
    Node* parseFunction(Node* node, int statement, bool allowExpressionBody = false, bool isAsync = false,
                        ForInit forInit = ForInit::No) override;
    void parseFunctionParams(Node* node) override;
    Node* parseClass(Node* node, int isStatement) override;
    Node* parseVar(Node* node, bool isFor, std::string_view kind, bool allowMissingInitializer = false) override;
    void parseVarId(Node* decl, std::string_view kind) override;
    Node* parseCatchClauseParam() override;
    Node* parseBreakContinueStatement(Node* node, const char* keyword) override;
    Node* parseLabeledStatement(Node* node, const std::string& maybeName, Node* expr, const char* context) override;
    Node* parseImport(Node* node) override;
    Node* parseExport(Node* node, std::vector<std::string>* exports) override;

    // ---- expressions (js_parse_ts_expression.cpp) ----
    Node* parseMaybeAssign(ForInit forInit = ForInit::No, DestructuringErrors* r = nullptr,
                           bool afterLeftParseParenItem = false) override;
    Node* parseMaybeConditional(ForInit forInit, DestructuringErrors* r) override;
    Node* parseExprOp(Node* left, int leftStartPos, Pos leftStartLoc, int minPrec, ForInit forInit) override;
    Node* parseMaybeUnary(DestructuringErrors* r, bool sawUnary, bool incDec, ForInit forInit) override;
    Node* parseSubscript(Node* base, int startPos, Pos startLoc, bool noCalls, bool maybeAsyncArrow,
                         bool optionalChained, ForInit forInit) override;
    Node* parseExprAtom(DestructuringErrors* r = nullptr, ForInit forInit = ForInit::No, bool forNew = false) override;
    Node* parseNew() override;
    Node* parseParenItem(Node* item) override;
    void parsePropertyValue(Node* prop, bool isPattern, bool isGenerator, bool isAsync, int startPos, Pos startLoc,
                            DestructuringErrors* r, bool containsEsc) override;
    Node* parseMethod(bool isGenerator, bool isAsync = false, bool allowDirectSuper = false) override;
    Node* toAssignable(Node* node, bool isBinding, DestructuringErrors* r = nullptr) override;
    void toAssignableList(Node** items, size_t n, bool isBinding) override;
    void checkLValSimple(Node* expr, int bindingType = BIND_NONE,
                         std::vector<std::string_view>* checkClashes = nullptr) override;
    void checkLValPattern(Node* expr, int bindingType = BIND_NONE,
                          std::vector<std::string_view>* checkClashes = nullptr) override;
    void checkLValInnerPattern(Node* expr, int bindingType = BIND_NONE,
                               std::vector<std::string_view>* checkClashes = nullptr) override;
    Node* parseBindingAtom() override;
    std::vector<Node*> parseBindingList(const TokenType& close, bool allowEmpty, bool allowTrailingComma,
                                        bool allowModifiers = false) override;
    Node* parseAssignableListItem(bool allowModifiers) override;
    Node* parseBindingListItem(Node* param) override;
    void checkUnreserved(Node* ref) override;
    void raiseRecoverable(int at, const std::string& message) override;
    void checkPatternErrors(DestructuringErrors* r, bool isAssign) override;
    bool checkExpressionErrors(DestructuringErrors* r, bool andThrow = false) override;
    void checkYieldAwaitInDefaultParams() override;
    Node* parseYield(ForInit forInit) override;
    bool shouldParseArrow(std::vector<Node*>& exprList) override;
    bool shouldParseAsyncArrow() override;
    Node* parseForIn(Node* node, Node* init) override;
    bool tsHeritageElementStarts();
    Node* parseExpressionStatement(Node* node, Node* expr) override;
    Node* parseThrowStatement(Node* node) override;
    bool tsNextIsUsingBinding(bool disallowOf);
    Node* parseForStatement(Node* node) override;
    Node* parseProperty(bool isPattern, DestructuringErrors* r) override;
    Node* parsePropertyName(Node* prop) override;
    Node* parseExprImport(bool forNew) override;
    bool tsInAwaitContext();
    Node* tsParseModuleSpecifier();             // a string, or (for the checker to reject) any expression
    NodeList tsParseImportAttributes();          // `with { ... }` / `assert { ... }`, or none

    Node* tsTryParseArrow(ForInit forInit, bool allowReturnType);
    int tsIsParenthesizedArrow();              // 1 true, 0 false, 2 unknown
    // parseParenthesizedArrowFunctionExpression. Speculative (allowAmbiguity
    // false): gaveUp when it is not an arrow function; committed once the
    // head is known to be one (an error after that is the arrow's).
    Node* tsParseParenArrow(bool allowAmbiguity, bool allowReturnType, ForInit forInit, bool& gaveUp,
                            bool& committed);
    std::vector<int> tsNotArrowAt;             // tsc's notParenthesizedArrow
    std::vector<Node*> tsParseParamList(const TokenType& close, bool speculative, bool& gaveUp);
    Node* tsParseParam(bool speculative, bool& gaveUp);
    void tsBlockContext();                     // a body's `{` read as an expression brace: make it a block
    Node* tsParseTypeAssertion(ForInit forInit);
    bool tsCanFollowTypeArgumentsInExpression();
    bool tsIsStartOfExpression();
    bool tsIsBinaryOperator();
    Node* tsParseDecoratedClassExpression();

    // ---- tokenizer ----
    void readToken(int code) override;
    void getTokenFromCode(int code) override;
    void updateContext(const TokenType* prevType) override;
    Node* jsx_parseAttributeValue() override;
    Node* jsx_parseExpressionContainer() override;
    Node* jsx_parseOpeningElementAt(int startPos, Pos startLoc) override;
};

}  // namespace jsparse
