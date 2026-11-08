#pragma once

// Internals of mep's JavaScript formatter (js_format.h): Prettier 3.6.2's
// estree printer, comment attachment and AstPath, ported over jsparse's
// ESTree (js_parse.h). Each method carries the name of the Prettier
// function it ports, so a difference from `prettier` can be chased by
// reading the two side by side:
//
//   js_format.cpp           core: parse, postprocess, print, self-check
//   js_format_comments.cpp  main/comments/attach.js, print.js and
//                           language-js/comments/handle-comments.js
//   js_format_util.cpp      language-js/utils/index.js, needs-parens.js,
//                           utils/{skip,has-newline,...}.js
//   js_format_print.cpp     print/index.js, estree.js, statement, block,
//                           class, module, literal, property, misc
//   js_format_expr.cpp      member(-chain), call(-arguments), assignment,
//                           binaryish, ternary, arrow, function(-params),
//                           object, array, template-literal
//   js_format_jsx.cpp       print/jsx.js
//
//   js_format_ts.cpp        print/typescript.js, type-annotation.js,
//                           type-parameters.js, interface.js, enum.js,
//                           mapped-type.js, cast-expression.js, decorators.js
//
// TypeScript: the printer dispatches on jsparse::NodeType; TS node kinds go
// to PrintTypescript (print/typescript.js) from PrintEstree, the TS parts of
// the shared printers (type annotations, type parameters, modifiers) sit in
// the estree printers where Prettier has them, and NeedsParens has the TS
// rules.

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "js_format.h"
#include "js_format_doc.h"
#include "js_parse.h"

namespace jsfmt {

using jsparse::Node;
using jsparse::NodeList;
using NT = jsparse::NodeType;

// ESTree field names (Prettier's path.call(print, "callee") keys).
enum class Key : uint8_t {
    None,
    id, left, test, object, callee, key, block, discriminant, tag, label, argument, expression, meta,
    openingElement, openingFragment, local, param, declaration, name,
    init, right, consequent, property, value, handler, quasi, superClass, source, closingElement,
    closingFragment, nspace,
    update, alternate, finalizer, options, exported, imported,
    body,
    params, elements, properties, arguments, quasis, declarations, cases, specifiers, children,
    expressions, attributes,
    // TypeScript (js_parse.h "TypeScript fields")
    typeAnnotation, typeParameters, returnType, typeArguments,  // ts[0..3]
    decorators, implements,                                     // tslist[0..1]
    typeName, objectType, checkType, typeParameter, literal, exprName, parameterName, parameter, qualifier,  // kid[0]
    elementType, indexType, extendsType, constraint, initializer, moduleReference,                       // kid[1]
    trueType, defaultType, nameType,                                                                       // kid[2]
    falseType,                                                                                             // kid[3]
    types, elementTypes, members, parameters, extends_,  // list[0]
    template_types,                                      // list[1] (TSTemplateLiteralType `types`)
};

// The node in field `k` of `n` (nullptr when the field is a list or empty).
Node* Child(const Node* n, Key k);
// The list in field `k` of `n` (nullptr when it is not a list field there).
const NodeList* ListOf(const Node* n, Key k);

enum class Placement : uint8_t { None, OwnLine, EndOfLine, Remaining };

struct CommentRec {
    bool block = false;
    std::string_view value;
    uint32_t start = 0, end = 0;
    bool leading = false, trailing = false, printed = false;
    bool ignored = false;  // inside a prettier-ignore'd node (printedComments)
    Placement placement = Placement::None;
    Node* enclosing = nullptr;
    Node* preceding = nullptr;
    Node* following = nullptr;
    Key marker = Key::None;  // addDanglingComment(node, comment, marker)
    bool unignore = false;   // a prettier-ignore handled by handle-comments.js
};

// CommentCheckFlags.
enum : uint32_t {
    kCLeading = 1u << 1,
    kCTrailing = 1u << 2,
    kCDangling = 1u << 3,
    kCBlock = 1u << 4,
    kCLine = 1u << 5,
    kCPrettierIgnore = 1u << 6,
    kCFirst = 1u << 7,
    kCLast = 1u << 8,
};

enum class Layout : uint8_t {
    None,
    BreakAfterOperator,
    NeverBreakAfterOperator,
    Fluid,
    BreakLhs,
    Chain,
    ChainTail,
    ChainTailArrowChain,
    OnlyLeft,
};

// The `args` of print(selector, args).
struct Args {
    bool expand_first = false;
    bool expand_last = false;
    Layout layout = Layout::None;
};

struct Frame {
    Node* node;
    Key key;
    int index;  // -1 when not in a list
    const NodeList* list;
};

// Thrown by printArrowFunction/printFunctionParameters (ArgExpansionBailout).
struct ArgExpansionBailout {};
// Thrown for a node kind the printer has no case for (Prettier's UnexpectedNodeError).
struct UnsupportedNode {
    const Node* node;
};

class Printer {
public:
    Printer(std::string_view text, const Options& opts, DocArena& arena);

    // attachComments + printAstToDoc.
    void Attach(Node* program, std::vector<CommentRec*> comments);
    Doc* PrintProgram(Node* program);
    // ensureAllCommentsPrinted: the first comment never printed, or null.
    const CommentRec* UnprintedComment() const;

    // Node side data.
    std::vector<CommentRec*>* CommentsOf(const Node* n);
    bool Parenthesized(const Node* n) const;
    void SetParenthesized(const Node* n);

    size_t print_calls = 0;
    size_t print_limit = SIZE_MAX;

    // ---- AstPath
    std::vector<Frame> stack_;
    Node* node() const { return stack_.back().node; }
    Node* parent() const { return stack_.size() >= 2 ? stack_[stack_.size() - 2].node : nullptr; }
    Node* grandparent() const { return stack_.size() >= 3 ? stack_[stack_.size() - 3].node : nullptr; }
    Node* GetParentNode(size_t count) const {
        return stack_.size() >= count + 2 ? stack_[stack_.size() - 2 - count].node : nullptr;
    }
    Key key() const { return stack_.back().key; }
    int index() const { return stack_.back().index; }
    bool IsRoot() const { return stack_.size() == 1; }
    bool IsLast() const {
        const Frame& f = stack_.back();
        return f.list && f.index == static_cast<int>(f.list->size()) - 1;
    }
    Node* Next() const {
        const Frame& f = stack_.back();
        return f.list && f.index + 1 < static_cast<int>(f.list->size()) ? (*f.list)[static_cast<size_t>(f.index) + 1]
                                                                       : nullptr;
    }
    using Pred = std::function<bool(Node*, Key)>;
    bool Match(std::initializer_list<Pred> preds) const;
    Node* FindAncestor(const std::function<bool(Node*)>& pred) const;

    template <class Fn>
    auto Call(Key k, Fn&& fn) {
        Node* child = Child(node(), k);
        stack_.push_back(Frame{child, k, -1, nullptr});
        struct Pop {
            std::vector<Frame>& s;
            ~Pop() { s.pop_back(); }
        } pop{stack_};
        return fn();
    }
    template <class Fn>
    void Each(Key k, Fn&& fn) {
        const NodeList* list = ListOf(node(), k);
        if (!list) return;
        for (uint32_t i = 0; i < list->size(); i++) {
            stack_.push_back(Frame{(*list)[i], k, static_cast<int>(i), list});
            struct Pop {
                std::vector<Frame>& s;
                ~Pop() { s.pop_back(); }
            } pop{stack_};
            fn(static_cast<int>(i));
        }
    }
    template <class Fn>
    auto CallAt(Key k, int i, Fn&& fn) {
        const NodeList* list = ListOf(node(), k);
        stack_.push_back(Frame{(*list)[static_cast<size_t>(i)], k, i, list});
        struct Pop {
            std::vector<Frame>& s;
            ~Pop() { s.pop_back(); }
        } pop{stack_};
        return fn();
    }

    // ---- printing (ast-to-doc.js mainPrint)
    Doc* Print();                         // print() on the current path
    Doc* Print(Key k);                    // print("k")
    Doc* PrintArgs(Key k, const Args& a); // print("k", args) -- never cached
    Doc* PrintAt(Key k, int i);           // print(["k", i])
    Parts PrintAll(Key k);                // path.map(print, "k")
    Doc* MainPrint(const Args* a);
    Doc* CallPluginPrint(const Args* a);

    // ---- print/index.js, estree.js
    Doc* PrintNode(const Args* a);
    Doc* PrintWithoutParens(const Args* a);
    Doc* PrintEstree(const Args* a);
    Doc* PrintIgnored();
    Doc* InheritLabel(Doc* doc, const std::function<Doc*(Doc*)>& fn);
    bool ShouldPrintLeadingSemicolon();
    bool ExpressionNeedsAsiProtection();
    bool IsDirectiveLiteral() const;
    Doc* PrintCallee();

    // ---- main/comments/print.js
    Doc* PrintComment(CommentRec* c);
    Doc* PrintLeadingComment(CommentRec* c);
    struct TrailingPrinted {
        Doc* doc;
        bool is_block;
        bool has_line_suffix;
    };
    TrailingPrinted PrintTrailingComment(CommentRec* c, const TrailingPrinted* prev);
    Doc* PrintDanglingComments(bool indent = false, const std::function<bool(CommentRec*)>& filter = nullptr,
                               Key marker = Key::None);
    struct Separate {
        Doc* leading;
        Doc* trailing;
        bool any;
    };
    Separate PrintCommentsSeparately();
    Doc* PrintComments(Doc* doc);  // printComments(path, doc)

    // ---- comment queries (utils/index.js)
    bool HasComment(const Node* n, uint32_t flags = 0, const std::function<bool(CommentRec*)>& fn = nullptr);
    std::vector<CommentRec*> GetComments(const Node* n, uint32_t flags = 0,
                                         const std::function<bool(CommentRec*)>& fn = nullptr);
    bool HasNodeIgnoreComment(const Node* n);
    bool HasLeadingOwnLineComment(const Node* n);
    bool NeedsHardlineAfterDanglingComment(const Node* n);
    bool IsIgnored();
    bool HasJsxIgnoreComment();
    bool WillPrintOwnComments();

    // ---- text helpers (utils/*.js); kNoIndex is Prettier's `false`
    static constexpr int64_t kNoIndex = INT64_MIN;
    std::string_view text_;
    int CharAt(int64_t i) const {
        return i >= 0 && i < static_cast<int64_t>(text_.size()) ? static_cast<unsigned char>(text_[static_cast<size_t>(i)])
                                                                 : -1;
    }
    int64_t SkipSpaces(int64_t i, bool backwards = false) const;
    int64_t SkipToLineEnd(int64_t i) const;
    int64_t SkipEverythingButNewLine(int64_t i) const;
    int64_t SkipNewline(int64_t i, bool backwards = false) const;
    int64_t SkipInlineComment(int64_t i) const;
    int64_t SkipTrailingComment(int64_t i) const;
    bool HasNewline(int64_t i, bool backwards = false) const;
    bool HasNewlineInRange(int64_t start, int64_t end) const;
    bool IsNextLineEmptyAfterIndex(int64_t i) const;
    bool IsNextLineEmpty(const Node* n) const { return IsNextLineEmptyAfterIndex(n->end); }
    bool IsPreviousLineEmpty(int64_t i) const;
    int64_t GetNextNonSpaceNonCommentCharacterIndex(int64_t i) const;
    int GetNextNonSpaceNonCommentCharacter(int64_t i) const { return CharAt(GetNextNonSpaceNonCommentCharacterIndex(i)); }
    std::string GetTextWithoutComments(uint32_t start, uint32_t end) const;
    bool IsLineAfterElementEmpty(const Node* n) const;

    // ---- language-js/utils/index.js
    bool IsCallExpression(const Node* n) const;
    bool IsMemberExpression(const Node* n) const;
    bool IsCallLikeExpression(const Node* n) const;
    bool IsMemberish(const Node* n) const { return IsMemberExpression(n); }
    std::vector<Node*> GetCallArguments(const Node* n) const;
    template <class Fn>
    void IterateCallArgumentsPath(Fn&& fn);
    Doc* PrintCallArgument(int index, const Args* a);  // print(getCallArgumentSelector(node, i), args)
    std::vector<Node*> GetFunctionParameters(const Node* n) const;
    bool HasRestParameter(const Node* n) const;
    bool IsSimpleCallArgument(const Node* n, int depth = 2);
    bool IsLoneShortArgument(const Node* n);
    bool IsTestCall(const Node* n, const Node* parent = nullptr);
    bool IsFunctionCompositionArgs(const std::vector<Node*>& args) const;
    bool IsLongCurriedCallExpression() const;
    bool IsTemplateOnItsOwnLine(const Node* n) const;
    bool IsSimpleExpressionByNodeCount(const Node* n, int max = 5) const;
    bool StartsWithNoLookaheadToken(const Node* n, const std::function<bool(const Node*)>& pred) const;
    bool HasNakedLeftSide(const Node* n) const;
    Node* GetLeftSide(const Node* n) const;
    Key GetLeftSideKey(const Node* n, int* index) const;
    bool IsJsxWhitespaceExpression(const Node* n);
    bool IsMeaningfulJsxText(const Node* n) const;

    // ---- needs-parens.js, semicolon.js
    bool NeedsParens();
    bool ShouldWrapFunctionForExportDefault();
    bool ShouldAddParenthesesToChainElement() const;
    bool IsPathInForStatementInitializer() const;

    // ---- printers (one per Prettier function)
    Doc* PrintStatementSequence(Key k);
    Doc* PrintBlock();
    Doc* PrintBlockBody();
    Doc* PrintExpressionStatement();
    Doc* PrintVariableDeclaration();
    Doc* PrintIf();
    Doc* PrintFor();
    Doc* PrintTry();
    Doc* PrintCatch();
    Doc* PrintSwitch();
    Doc* PrintSwitchCase();
    Doc* PrintReturnOrThrow(std::string_view keyword);
    bool ReturnArgumentHasLeadingComment(const Node* arg);
    Doc* AdjustClause(const Node* n, Doc* clause, bool force_space = false);
    Doc* PrintLiteral();
    Doc* PrintStringLiteralRaw(std::string_view raw);
    std::string PrintString(std::string_view raw) const;
    Doc* PrintClass();
    Doc* PrintClassBody();
    Doc* PrintClassMethod();
    Doc* PrintClassProperty();
    Doc* PrintMethod();
    Doc* PrintMethodValue();
    Doc* PrintPropertyKey();
    Doc* PrintProperty();
    bool IsStringKeySafeToUnquote(const Node* prop) const;
    Doc* PrintImportDeclaration();
    Doc* PrintExportDeclaration();
    Doc* PrintModuleSpecifiers();
    Doc* PrintModuleSource();
    Doc* PrintImportAttributes();
    Doc* PrintModuleSpecifier();
    bool ShouldPrintSpecifiers(const Node* n) const;

    Doc* PrintMemberExpression();
    Doc* PrintMemberLookup();
    Doc* PrintOptionalToken();
    Doc* PrintCallExpression();
    bool IsCommonsJsOrAmdModuleDefinition();
    Doc* PrintMemberChain();
    Doc* PrintCallArguments();
    bool CouldExpandArg(const Node* arg, bool arrow_chain_recursion = false);
    bool ShouldExpandLastArg(const std::vector<Node*>& args, const Parts& arg_docs);
    bool ShouldExpandFirstArg(const std::vector<Node*>& args);
    bool IsHopefullyShortCallArgument(const Node* n);
    bool IsReactHookCallWithDepsArray(const std::vector<Node*>& args);
    bool IsConciselyPrintedArray(const Node* n);

    Doc* PrintAssignment(Doc* left, Doc* op, Key right_key);
    Layout ChooseLayout(Doc* left, Key right_key);
    bool ShouldBreakAfterOperator(bool has_short_key);
    bool IsPoorlyBreakableMemberOrCallChain(bool deep = false);
    bool IsComplexDestructuring(const Node* n) const;
    bool IsObjectPropertyWithShortKey(const Node* n, Doc* key_doc);
    Doc* PrintAssignmentExpression();
    Doc* PrintVariableDeclarator();

    Doc* PrintBinaryish();
    Parts PrintBinaryishExpressions(bool is_nested, bool is_inside_parens);
    bool ShouldInlineLogicalExpression(const Node* n) const;

    Doc* PrintTernary();
    bool ShouldExtraIndentForConditionalExpression();

    Doc* PrintArrowFunction(const Args* a);
    Doc* PrintArrowFunctionSignature(const Args* a);
    bool MayBreakAfterShortPrefix(const Node* body, Doc* body_doc);
    bool ShouldAddParensIfNotBreak(const Node* n);
    Doc* PrintFunction(const Args* a);
    Doc* PrintFunctionParameters(bool expand_arg, bool print_type_params);
    bool ShouldHugTheOnlyFunctionParameter(const Node* n);
    bool IsDecoratedFunction() const;
    bool CanPrintParamsWithoutParens(const Node* n);

    Doc* PrintObject();
    Doc* PrintArray();
    Doc* PrintArrayElements();
    Doc* PrintArrayElementsConcisely(Doc* trailing_comma);
    Doc* PrintTemplateLiteral();
    Doc* PrintTaggedTemplateLiteral();
    bool IsJestEachTemplateLiteral();
    Doc* PrintJestEachTemplateLiteral();
    Doc* PrintSequence();
    Doc* PrintAwait();
    Doc* PrintUnary();

    // jsx.js
    Doc* PrintJsx();
    Doc* PrintJsxElement();
    Doc* PrintJsxElementInternal();
    Parts PrintJsxChildren(Doc* jsx_whitespace, bool is_fbt);
    Doc* MaybeWrapJsxElementInParens(Doc* elem);
    Doc* PrintJsxAttribute();
    Doc* PrintJsxExpressionContainer();
    Doc* PrintJsxOpeningElement();
    Doc* PrintJsxClosingElement();
    Doc* PrintJsxOpeningClosingFragment();
    Doc* PrintJsxEmptyExpression();
    Doc* PrintJsxSpreadAttribute();

    // TypeScript and decorators (js_format_ts.cpp; tokens/heritage in print, params in expr)
    Doc* PrintTypescript(const Args* a);
    bool ShouldHugType(const Node* n);
    std::string_view TypeAnnotationFirstToken();
    Doc* PrintTypeAnnotationProperty(Key k = Key::typeAnnotation);
    Doc* PrintTypeAnnotation();
    Doc* PrintTypeAlias();
    Doc* PrintIntersectionType();
    Doc* PrintUnionType();
    Doc* PrintFunctionType();
    Doc* PrintTypePredicate();
    Doc* PrintDanglingCommentsForInline();
    Doc* PrintTypeParameters(Key params_key);
    Doc* PrintTypeParameter();
    Doc* PrintInterface();
    Doc* PrintMappedType();
    Doc* PrintBinaryCastExpression();
    bool HasNewlineBetweenOrAfterDecorators(const Node* n);
    Doc* PrintClassMemberDecorators();
    Doc* PrintDecoratorsBeforeExport();
    Doc* PrintDecorators();
    Doc* PrintReturnType();
    Doc* PrintDefiniteToken();
    Doc* PrintDeclareToken();
    Doc* PrintAbstractToken();
    Doc* PrintAccessibilityToken();
    Doc* PrintFunctionTypeParameters();
    bool ShouldGroupFunctionParameters(const Node* fn, Doc* return_type_doc);
    bool ShouldBreakFunctionParameters(const Node* fn) const;
    bool ShouldIndentOnlyHeritageClauses(const Node* n);
    Doc* PrintHeritageClauses(Key list_key, std::string_view list_name);
    GroupId HeritageGroupId(const Node* n);
    GroupId TypeParametersGroupId(const Node* n);
    bool IsCallExpressionWithComplexTypeArguments(const Node* n);
    std::unordered_map<const Node*, GroupId> type_params_ids_;
    std::unordered_set<const Node*> prettier_ignore_;  // node.prettierIgnore

    // builders shortcuts
    DocArena& a_;
    Doc* S(std::string_view s) { return a_.Str(s); }
    Doc* Cat(std::initializer_list<Doc*> p) { return a_.Cat(p); }
    Doc* Cat(const Parts& p) { return a_.Cat(p); }
    Doc* Group(Doc* d, bool brk = false, GroupId id = 0) { return a_.Group(d, brk, id); }
    Doc* Indent(Doc* d) { return a_.Indent(d); }
    Doc* IfBreak(Doc* b, Doc* f = nullptr, GroupId id = 0) { return a_.IfBreak(b, f, id); }
    Doc* line() const { return a_.line; }
    Doc* softline() const { return a_.softline; }
    Doc* hardline() const { return a_.hardline; }
    bool WillBreak(Doc* d) { return a_.WillBreak(d); }
    static bool HasLabel(const Doc* d, uint32_t bit) {
        return d && d->kind == DocKind::Label && (d->label & bit);
    }

    const Options& opts_;
    std::unordered_map<const Node*, std::vector<CommentRec*>> comments_of_;
    std::unordered_map<const Node*, bool> parenthesized_;
    std::unordered_map<const Node*, Doc*> cache_;
    std::unordered_map<const Node*, GroupId> heritage_ids_;
    std::vector<CommentRec*> all_comments_;
    std::deque<Node> synthetic_;
    std::deque<NodeList> synthetic_lists_;
    std::deque<std::vector<Node*>> synthetic_items_;
    bool in_jest_each_ = false;

    // comments/attach.js
    void DecorateComment(Node* node, CommentRec* c, Node* enclosing);
    const std::vector<Node*>& SortedChildNodes(Node* n);
    std::unordered_map<const Node*, std::vector<Node*>> child_cache_;
    void AddLeading(Node* n, CommentRec* c);
    void AddTrailing(Node* n, CommentRec* c);
    void AddDangling(Node* n, CommentRec* c);
    bool HandleOwnLine(CommentRec* c, Node* ast, bool is_last);
    bool HandleEndOfLine(CommentRec* c, Node* ast, bool is_last);
    bool HandleRemaining(CommentRec* c, Node* ast, bool is_last);
};

// Type helpers.
inline bool Is(const Node* n, NT t) { return n && n->type == t; }
inline bool IsStringLiteral(const Node* n) { return n && n->type == NT::Literal && n->lit == jsparse::LitKind::String; }
inline bool IsNumericLiteral(const Node* n) { return n && n->type == NT::Literal && n->lit == jsparse::LitKind::Number; }
inline bool IsRegExpLiteral(const Node* n) { return n && n->type == NT::Literal && n->lit == jsparse::LitKind::RegExp; }
inline bool IsBooleanLiteral(const Node* n) { return n && n->type == NT::Literal && n->lit == jsparse::LitKind::Bool; }
inline bool IsLiteral(const Node* n) { return n && n->type == NT::Literal; }
inline bool IsJsxElement(const Node* n) { return n && (n->type == NT::JSXElement || n->type == NT::JSXFragment); }
inline bool IsObjectExpression(const Node* n) { return Is(n, NT::ObjectExpression); }
inline bool IsArrayExpression(const Node* n) { return Is(n, NT::ArrayExpression); }
inline bool IsFunctionOrArrow(const Node* n) {
    return n && (n->type == NT::FunctionExpression || n->type == NT::ArrowFunctionExpression);
}
inline bool IsBinaryish(const Node* n) {
    return n && (n->type == NT::BinaryExpression || n->type == NT::LogicalExpression);
}
inline bool IsSignedNumericLiteral(const Node* n) {
    return Is(n, NT::UnaryExpression) && (n->op == "+" || n->op == "-") && IsNumericLiteral(n->argument());
}
bool IsMethod(const Node* n);
bool IsTsKeywordType(const Node* n);
bool IsSimpleType(const Node* n);
bool IsObjectType(const Node* n);
bool IsUnionType(const Node* n);
bool IsBinaryCastExpression(const Node* n);
inline bool IsConditionalType(const Node* n) { return Is(n, NT::TSConditionalType); }
bool IsObjectProperty(const Node* n);
int GetPrecedence(std::string_view op);
bool ShouldFlatten(std::string_view parent_op, std::string_view node_op);
bool IsBitwiseOperator(std::string_view op);
bool IsLineComment(const CommentRec* c);
bool IsBlockComment(const CommentRec* c);
bool IsTypeCastComment(const CommentRec* c);
bool IsIndentableBlockComment(const CommentRec* c);
bool IsPrettierIgnoreComment(const CommentRec* c);
std::string_view TrimJs(std::string_view s);  // String.prototype.trim
std::string PrintNumber(std::string_view raw);
std::string_view Raw(const Node* n);  // getRaw

template <class Fn>
void Printer::IterateCallArgumentsPath(Fn&& fn) {
    Node* n = node();
    if (n->type == NT::ChainExpression) {
        Call(Key::expression, [&] {
            IterateCallArgumentsPath(fn);
            return 0;
        });
        return;
    }
    if (n->type == NT::ImportExpression || n->type == NT::TSImportType) {
        Call(Key::source, [&] {
            fn(0);
            return 0;
        });
        if (n->options()) {
            Call(Key::options, [&] {
                fn(1);
                return 0;
            });
        }
    } else {
        Each(Key::arguments, [&](int i) { fn(i); });
    }
}

}  // namespace jsfmt
