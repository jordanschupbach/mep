#pragma once

// The JavaScript parser behind mep-js-lsp and mep's JS/TS formatter: a C++
// port of acorn 8.15.0 (MIT, Marijn Haverbeke, Ingvar Stepanyan and
// contributors) plus acorn-jsx 5.3.2 (MIT) for JSX. Same tokenizer, same
// parser decisions, same ESTree tree, same error messages at the same
// positions -- tools/js_parse_check.py runs acorn itself over a 2,000-file
// corpus (and damaged copies of it) and compares the two tree for tree.
//
// What it takes (Options): ecmaVersion "latest" (acorn's 1e8; the year
// numbers 2015..2026 or 3/5/6.. work too), sourceType script or module,
// hashbang lines, JSX on or off.
//
// What it gives (ParseResult):
//   - program: an ESTree tree of arena-allocated Nodes (see Node below for
//     where each ESTree field lives),
//   - comments: acorn's onComment array (Block/Line, value, range),
//   - tokens (Options::tokens): acorn's onToken stream,
//   - errors: errors[0] is acorn's SyntaxError -- the message without the
//     " (line:col)" suffix, its position, acorn's raisedAt -- and later
//     entries are whatever the recovery ran into after it.
//
// Positions. Every node, comment and token carries three views of where
// it is:
//   start/end     byte offsets into the UTF-8 source (what mep's buffers use);
//   ustart/uend   UTF-16 code-unit offsets -- acorn's own start/end, which
//                 is what JavaScript tools and the acorn JSON compare with;
//   loc           acorn's {line, column}: line 1-based, column 0-based in
//                 UTF-16 code units since the last line break (\n, \r\n,
//                 \r, U+2028, U+2029). That is also LSP's default
//                 position encoding, so loc maps straight onto LSP Ranges.
// The source is decoded as WHATWG UTF-8 (what node's readFileSync(f,
// 'utf8') does): an invalid byte sequence becomes one U+FFFD.
//
// Strings (names, raw text, string values, comment text) are UTF-8 views
// into the result's arena. A lone surrogate in a string value (from
// "\ud800") is kept as its 3-byte generalized-UTF-8 (WTF-8) form.
//
// Error tolerance. acorn stops at the first error. This parser records it
// (that one is authoritative: same message, same position as acorn) and
// then keeps going so an editor still gets a tree for the rest of the
// file:
//   - errors acorn raises with raiseRecoverable (reserved words, duplicate
//     declarations, bad escapes in templates, invalid regexps, ...) are
//     recorded and parsing continues in place, as acorn-loose does;
//   - any other error drops the statement (or class member) it happened
//     in: parser state is rolled back to where that statement began, the
//     tokens are skipped to a resynchronisation point (a `;` or the
//     block's `}` at the statement's own bracket depth, a line that starts
//     a statement at or left of the failed statement's column, or -- when
//     brackets went missing -- a word at that column after a token that
//     ends an expression) and parsing resumes there;
//   - after an error inside a block, a statement keyword starting a line
//     at or left of the indentation of the block's `{` line closes the
//     block (its `}` is probably the thing that went missing).
// Recovery happens in every statement list (program, block, function
// body, switch case, static block) and in class bodies. Nesting deeper than
// Options::max_depth is reported ("Too deeply nested") instead of
// overflowing the stack.
//
// TypeScript (Options::typescript). The parser itself is jsparse::Parser in
// js_parse_parser.h, a class whose parse methods are the same methods acorn
// has, and the ones acorn plugins override are virtual. JSX is a subclass
// (JsxParser) overriding readToken, updateContext and parseExprAtom
// exactly as acorn-jsx does; TypeScript is TsParser (js_parse_ts.h) on top
// of JsxParser, overriding the same way acorn-typescript does, but making
// the decisions TypeScript 5.9's own parser (parser.ts) makes: what it
// accepts, where it reports the first syntax error (acorn's message style),
// how `<` / `(` arrow ambiguities and JSX vs `<T>x` resolve. Its trees use
// typescript-estree's node names and shapes (JSPARSE_TS_NODE_TYPES and the
// "TypeScript fields" at Node). tools/js_parse_ts_check.py compares it with
// ts.createSourceFile's parseDiagnostics and typescript-estree's trees.
// Token types and token contexts are objects compared by address (as in
// acorn), so a subclass can add its own.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jsparse {

// Every ESTree node type acorn and acorn-jsx produce, in one list so the
// enum, the names and the JSON writer stay in step.
#define JSPARSE_NODE_TYPES(X)                                                                                  \
    X(Program) X(Identifier) X(PrivateIdentifier) X(Literal) X(ExpressionStatement) X(BlockStatement)          \
    X(EmptyStatement) X(DebuggerStatement) X(WithStatement) X(ReturnStatement) X(LabeledStatement)             \
    X(BreakStatement) X(ContinueStatement) X(IfStatement) X(SwitchStatement) X(SwitchCase) X(ThrowStatement)  \
    X(TryStatement) X(CatchClause) X(WhileStatement) X(DoWhileStatement) X(ForStatement) X(ForInStatement)     \
    X(ForOfStatement) X(FunctionDeclaration) X(VariableDeclaration) X(VariableDeclarator) X(ThisExpression)    \
    X(Super) X(ArrayExpression) X(ObjectExpression) X(Property) X(FunctionExpression)                          \
    X(ArrowFunctionExpression) X(UnaryExpression) X(UpdateExpression) X(BinaryExpression)                      \
    X(LogicalExpression) X(AssignmentExpression) X(ConditionalExpression) X(CallExpression) X(NewExpression)   \
    X(SequenceExpression) X(YieldExpression) X(AwaitExpression) X(TemplateLiteral)                             \
    X(TaggedTemplateExpression) X(TemplateElement) X(ClassDeclaration) X(ClassExpression) X(ClassBody)         \
    X(MethodDefinition) X(PropertyDefinition) X(StaticBlock) X(MetaProperty) X(ImportExpression)               \
    X(ChainExpression) X(MemberExpression) X(ParenthesizedExpression) X(ObjectPattern) X(ArrayPattern)        \
    X(RestElement) X(AssignmentPattern) X(SpreadElement) X(ImportDeclaration) X(ImportSpecifier)               \
    X(ImportDefaultSpecifier) X(ImportNamespaceSpecifier) X(ImportAttribute) X(ExportNamedDeclaration)         \
    X(ExportDefaultDeclaration) X(ExportAllDeclaration) X(ExportSpecifier) X(JSXElement) X(JSXFragment)        \
    X(JSXOpeningElement) X(JSXClosingElement) X(JSXOpeningFragment) X(JSXClosingFragment) X(JSXAttribute)      \
    X(JSXSpreadAttribute) X(JSXIdentifier) X(JSXNamespacedName) X(JSXMemberExpression)                         \
    X(JSXExpressionContainer) X(JSXEmptyExpression) X(JSXText)

// The TypeScript node types (Options::typescript), typescript-estree's
// names and shapes (what Prettier's TS printer and typescript-eslint use).
// Decorator and AccessorProperty are ESTree proposals typescript-estree
// emits as such; JSXSpreadChild (`<a>{...xs}</a>`, which acorn-jsx rejects)
// comes from TSX only. The TSJSDoc* types are JSDoc type syntax tsc's parser
// accepts in .ts too (`*`, `?`, `?T`/`T?`, `!T`/`T!`, `function(T=): U`; its
// checker rejects them): typescript-estree's TSJSDocAllType / UnknownType /
// NullableType / NonNullableType (typeAnnotation: a TSTypeAnnotation around
// the type, postfix: kTsPostfix), plus two typescript-estree has no node for
// -- TSJSDocFunctionType (params: types, a `this:`/`new:` one as a
// TSNamedTupleMember, `...T` a TSRestType, `T=` a TSJSDocOptionalType;
// returnType) and TSJSDocOptionalType (typeAnnotation as above).
// See "TypeScript fields" at Node for where each field is.
#define JSPARSE_TS_NODE_TYPES(X)                                                                               \
    X(Decorator) X(AccessorProperty) X(TSTypeAnnotation) X(TSTypeParameterDeclaration) X(TSTypeParameter)      \
    X(TSTypeParameterInstantiation) X(TSAnyKeyword) X(TSUnknownKeyword) X(TSNumberKeyword) X(TSObjectKeyword)  \
    X(TSBooleanKeyword) X(TSBigIntKeyword) X(TSStringKeyword) X(TSSymbolKeyword) X(TSVoidKeyword)              \
    X(TSUndefinedKeyword) X(TSNullKeyword) X(TSNeverKeyword) X(TSIntrinsicKeyword) X(TSThisType)               \
    X(TSTypeReference) X(TSQualifiedName) X(TSArrayType) X(TSTupleType) X(TSNamedTupleMember) X(TSOptionalType) \
    X(TSRestType) X(TSUnionType) X(TSIntersectionType) X(TSFunctionType) X(TSConstructorType) X(TSTypeLiteral)  \
    X(TSPropertySignature) X(TSMethodSignature) X(TSIndexSignature) X(TSCallSignatureDeclaration)              \
    X(TSConstructSignatureDeclaration) X(TSTypeOperator) X(TSIndexedAccessType) X(TSConditionalType)          \
    X(TSInferType) X(TSMappedType) X(TSLiteralType) X(TSTemplateLiteralType) X(TSTypeQuery) X(TSImportType)    \
    X(TSTypePredicate) X(TSInterfaceDeclaration) X(TSInterfaceBody) X(TSInterfaceHeritage) X(TSClassImplements) \
    X(TSTypeAliasDeclaration) X(TSEnumDeclaration) X(TSEnumBody) X(TSEnumMember) X(TSModuleDeclaration)         \
    X(TSModuleBlock) X(TSDeclareFunction) X(TSEmptyBodyFunctionExpression) X(TSAbstractMethodDefinition)       \
    X(TSAbstractPropertyDefinition) X(TSAbstractAccessorProperty) X(TSParameterProperty) X(TSAsExpression)      \
    X(TSSatisfiesExpression) X(TSNonNullExpression) X(TSTypeAssertion) X(TSInstantiationExpression)            \
    X(TSImportEqualsDeclaration) X(TSExternalModuleReference) X(TSExportAssignment)                            \
    X(TSNamespaceExportDeclaration) X(JSXSpreadChild) X(TSJSDocAllType) X(TSJSDocUnknownType)                \
    X(TSJSDocNullableType) X(TSJSDocNonNullableType) X(TSJSDocOptionalType) X(TSJSDocFunctionType)

enum class NodeType : uint8_t {
    None,  // a node that was started but never finished (only seen mid-parse)
#define JSPARSE_ENUM_ITEM(n) n,
    JSPARSE_NODE_TYPES(JSPARSE_ENUM_ITEM)
    JSPARSE_TS_NODE_TYPES(JSPARSE_ENUM_ITEM)
#undef JSPARSE_ENUM_ITEM
        Count_
};

const char* NodeTypeName(NodeType t);  // "Identifier", ...

struct Pos {
    uint32_t line = 1;    // 1-based
    uint32_t column = 0;  // 0-based, UTF-16 code units (acorn's)
};

struct Node;

// A run of child nodes in the arena. Items can be null (array holes:
// `[a, , b]`).
struct NodeList {
    Node** items = nullptr;
    uint32_t count = 0;
    Node** begin() const { return items; }
    Node** end() const { return items + count; }
    uint32_t size() const { return count; }
    bool empty() const { return count == 0; }
    Node* operator[](size_t i) const { return items[i]; }
};

enum class LitKind : uint8_t { None, Null, Bool, Number, String, RegExp, BigInt };

// Node::flags bits.
enum : uint32_t {
    kFlagComputed = 1u << 0,      // MemberExpression, Property, MethodDefinition, PropertyDefinition
    kFlagOptional = 1u << 1,      // MemberExpression, CallExpression (`?.`)
    kFlagStatic = 1u << 2,        // MethodDefinition, PropertyDefinition
    kFlagGenerator = 1u << 3,     // functions
    kFlagAsync = 1u << 4,         // functions
    kFlagExpression = 1u << 5,    // functions: arrow with an expression body
    kFlagShorthand = 1u << 6,     // Property
    kFlagMethod = 1u << 7,        // Property
    kFlagPrefix = 1u << 8,        // UnaryExpression, UpdateExpression
    kFlagDelegate = 1u << 9,      // YieldExpression (`yield*`)
    kFlagSelfClosing = 1u << 10,  // JSXOpeningElement / JSXOpeningFragment
    kFlagTail = 1u << 11,         // TemplateElement
    kFlagAwait = 1u << 12,        // ForOfStatement (`for await`)
    kFlagHasDirective = 1u << 13, // ExpressionStatement: `directive` is present
    kFlagHasCooked = 1u << 14,    // TemplateElement: cooked is a string (else null)
    kFlagBoolValue = 1u << 15,    // Literal true
    kFlagHasName = 1u << 16,      // JSXOpening/ClosingElement have a `name`
    kFlagHasOptions = 1u << 17,   // ImportExpression: the `options` key is present
};

// Node::tsflags bits (TypeScript; see "TypeScript fields" at Node).
enum : uint32_t {
    kTsOptional = 1u << 0,        // `optional`: Identifier/patterns (`a?`), class members and signatures
                                  // (`m?()`), TSNamedTupleMember, TSMappedType (`]?:`)
    kTsDefinite = 1u << 1,        // `definite`: VariableDeclarator / PropertyDefinition `x!: T`
    kTsDeclare = 1u << 2,         // `declare`: declarations, PropertyDefinition
    kTsAbstract = 1u << 3,        // `abstract`: ClassDeclaration/Expression, TSConstructorType
    kTsReadonly = 1u << 4,        // `readonly`: properties, signatures, TSParameterProperty, TSMappedType
    kTsOverride = 1u << 5,        // `override`: class members, TSParameterProperty
    kTsConst = 1u << 6,           // `const`: TSEnumDeclaration, TSTypeParameter
    kTsIn = 1u << 7,              // `in`: TSTypeParameter
    kTsOut = 1u << 8,             // `out`: TSTypeParameter
    kTsAsserts = 1u << 9,         // `asserts`: TSTypePredicate
    kTsTypeOnly = 1u << 10,       // importKind/exportKind "type" (else "value"): Import/Export
                                  // declarations and specifiers, TSImportEqualsDeclaration
    kTsGlobal = 1u << 11,         // `global`: TSModuleDeclaration (`declare global {}`)
    kTsMappedPlus = 1u << 12,     // TSMappedType: `optional` is "+" (with kTsOptional)
    kTsMappedMinus = 1u << 13,    // TSMappedType: `optional` is "-" (with kTsOptional)
    kTsReadonlyPlus = 1u << 14,   // TSMappedType: `readonly` is "+" (with kTsReadonly)
    kTsReadonlyMinus = 1u << 15,  // TSMappedType: `readonly` is "-" (with kTsReadonly)
    kTsStatic = 1u << 16,         // `static` on TSParameterProperty / TSIndexSignature / signatures
    kTsPostfix = 1u << 17,        // `postfix`: TSJSDocNullableType / TSJSDocNonNullableType (`T?`, `T!`)
};

// One ESTree node. Child nodes live in four slots (kid) and two lists
// (list), and each ESTree field name always maps to the same slot (so
// node->left() means the same slot whatever the node type is):
//
//   kid[0]  id left test object callee key block discriminant tag label
//           argument expression meta openingElement/openingFragment local
//           param declaration name(JSX attribute/element/namespaced name)
//   kid[1]  init right consequent property value handler quasi superClass
//           source closingElement/closingFragment namespace
//   kid[2]  update alternate finalizer options exported imported
//   kid[3]  body (when it is a single node)
//   list[0] body (when it is a list: Program, BlockStatement, ClassBody,
//           StaticBlock) params elements properties arguments quasis
//           declarations cases consequent(SwitchCase) specifiers children
//   list[1] expressions attributes
//
// Scalars:
//   str   name (Identifier, PrivateIdentifier, JSXIdentifier), raw
//         (Literal, JSXText, TemplateElement value.raw), directive
//         (ExpressionStatement, with kFlagHasDirective)
//   str2  string value (Literal string, JSXText), regex pattern, bigint
//         digits (decimal, as acorn's `bigint`), TemplateElement cooked
//   str3  regex flags
//   num   number value (Literal number)
//   op    operator ("+", "+=", "typeof", ...)
//   kind  "var" "let" "const" "using" "await using" (VariableDeclaration),
//         "init" "get" "set" (Property), "constructor" "method" "get" "set"
//         (MethodDefinition), "script"/"module" (Program sourceType)
//
// A shorthand property `{a}` has key and value as two separate nodes over
// the same range (acorn copies the key).
//
// TypeScript fields (Options::typescript; typescript-estree's names). The
// four TS slots and two TS lists hold the fields TS adds to JS nodes and
// that TS nodes share:
//
//   ts[0]     typeAnnotation (Identifier and patterns, PropertyDefinition and
//             friends, TSPropertySignature, TSIndexSignature, TSTypeAnnotation,
//             TSAs/Satisfies/TypeAssertion, TSTypePredicate, TSTypeAliasDeclaration,
//             TSMappedType, TSOptionalType, TSRestType, TSTypeOperator)
//   ts[1]     typeParameters (functions, classes, interfaces, type aliases,
//             signatures, function/constructor types)
//   ts[2]     returnType (functions, signatures, function/constructor types)
//   ts[3]     typeArguments (Call/New/TaggedTemplate, JSXOpeningElement,
//             TSTypeReference, TSTypeQuery, TSImportType, TSInstantiationExpression,
//             TSInterfaceHeritage, TSClassImplements) and a class's
//             superTypeArguments
//   tslist[0] decorators (classes, class members, parameters)
//   tslist[1] implements (classes)
//   tsflags   kTs* bits above; accessibility "public"/"protected"/"private"
//
// TS node types put their other fields in the JS slots:
//
//   kid[0]  typeName (TSTypeReference) left (TSQualifiedName) label
//           (TSNamedTupleMember) objectType checkType
//           typeParameter (TSInferType) literal (TSLiteralType) exprName
//           (TSTypeQuery) parameterName (TSTypePredicate) name (TSTypeParameter)
//           id key expression parameter (TSParameterProperty) qualifier
//           (TSImportType)
//   kid[1]  right (TSQualifiedName) elementType (TSArrayType,
//           TSNamedTupleMember) indexType
//           extendsType constraint (TSTypeParameter, TSMappedType) initializer
//           (TSEnumMember) moduleReference source (TSImportType)
//   kid[2]  trueType default (TSTypeParameter) nameType (TSMappedType) options
//           (TSImportType)
//   kid[3]  falseType body (TSInterfaceDeclaration, TSEnumDeclaration,
//           TSModuleDeclaration)
//   list[0] params (type parameter lists, signatures, function types) types
//           (union/intersection) elementTypes members (TSTypeLiteral, TSEnumBody)
//           body (TSInterfaceBody, TSModuleBlock) extends (TSInterfaceDeclaration)
//           quasis (TSTemplateLiteralType) parameters (TSIndexSignature)
//   list[1] types (TSTemplateLiteralType)
//   kind    "method"/"get"/"set" (TSMethodSignature), "global"/"module"/
//           "namespace" (TSModuleDeclaration)
//   op      operator "keyof"/"unique"/"readonly" (TSTypeOperator)
//   flags   kFlagComputed, kFlagStatic as for the JS member nodes
struct Node {
    NodeType type = NodeType::None;
    LitKind lit = LitKind::None;
    uint32_t flags = 0;
    uint32_t start = 0, end = 0;    // byte offsets
    uint32_t ustart = 0, uend = 0;  // UTF-16 offsets (acorn's start/end)
    Pos loc_start, loc_end;
    Node* kid[4] = {nullptr, nullptr, nullptr, nullptr};
    NodeList list[2];
    std::string_view str, str2, str3, op, kind;
    double num = 0;
    // TypeScript (see above); null/empty/zero in JS trees.
    Node* ts[4] = {nullptr, nullptr, nullptr, nullptr};
    NodeList tslist[2];
    uint32_t tsflags = 0;
    std::string_view accessibility;
    bool HasTs(uint32_t f) const { return (tsflags & f) != 0; }
    void SetTs(uint32_t f, bool on) { tsflags = on ? (tsflags | f) : (tsflags & ~f); }

    bool Has(uint32_t f) const { return (flags & f) != 0; }
    void Set(uint32_t f, bool on) { flags = on ? (flags | f) : (flags & ~f); }

#define JSPARSE_KID(fn, i)                    \
    Node*& fn() { return kid[i]; }            \
    Node* fn() const { return kid[i]; }
    JSPARSE_KID(id, 0)
    JSPARSE_KID(left, 0)
    JSPARSE_KID(test, 0)
    JSPARSE_KID(object, 0)
    JSPARSE_KID(callee, 0)
    JSPARSE_KID(key, 0)
    JSPARSE_KID(block, 0)
    JSPARSE_KID(discriminant, 0)
    JSPARSE_KID(tag, 0)
    JSPARSE_KID(label, 0)
    JSPARSE_KID(argument, 0)
    JSPARSE_KID(expression, 0)
    JSPARSE_KID(meta, 0)
    JSPARSE_KID(opening, 0)  // openingElement / openingFragment
    JSPARSE_KID(local, 0)
    JSPARSE_KID(param, 0)
    JSPARSE_KID(declaration, 0)
    JSPARSE_KID(name, 0)     // JSX names
    JSPARSE_KID(init, 1)
    JSPARSE_KID(right, 1)
    JSPARSE_KID(consequent, 1)
    JSPARSE_KID(property, 1)
    JSPARSE_KID(value, 1)
    JSPARSE_KID(handler, 1)
    JSPARSE_KID(quasi, 1)
    JSPARSE_KID(superClass, 1)
    JSPARSE_KID(source, 1)
    JSPARSE_KID(closing, 1)  // closingElement / closingFragment
    JSPARSE_KID(nspace, 1)   // JSXNamespacedName.namespace
    JSPARSE_KID(update, 2)
    JSPARSE_KID(alternate, 2)
    JSPARSE_KID(finalizer, 2)
    JSPARSE_KID(options, 2)
    JSPARSE_KID(exported, 2)
    JSPARSE_KID(imported, 2)
    JSPARSE_KID(body, 3)
    // TypeScript nodes (see above)
    JSPARSE_KID(typeName, 0)
    JSPARSE_KID(objectType, 0)
    JSPARSE_KID(checkType, 0)
    JSPARSE_KID(typeParameter, 0)
    JSPARSE_KID(literal, 0)
    JSPARSE_KID(exprName, 0)
    JSPARSE_KID(parameterName, 0)
    JSPARSE_KID(parameter, 0)
    JSPARSE_KID(qualifier, 0)
    JSPARSE_KID(elementType, 1)
    JSPARSE_KID(indexType, 1)
    JSPARSE_KID(extendsType, 1)
    JSPARSE_KID(constraint, 1)
    JSPARSE_KID(initializer, 1)
    JSPARSE_KID(moduleReference, 1)
    JSPARSE_KID(trueType, 2)
    JSPARSE_KID(defaultType, 2)
    JSPARSE_KID(nameType, 2)
    JSPARSE_KID(falseType, 3)
#undef JSPARSE_KID
#define JSPARSE_TSKID(fn, i)                  \
    Node*& fn() { return ts[i]; }             \
    Node* fn() const { return ts[i]; }
    JSPARSE_TSKID(typeAnnotation, 0)
    JSPARSE_TSKID(typeParameters, 1)
    JSPARSE_TSKID(returnType, 2)
    JSPARSE_TSKID(typeArguments, 3)
#undef JSPARSE_TSKID
    NodeList& decorators() { return tslist[0]; }
    const NodeList& decorators() const { return tslist[0]; }
    NodeList& implements() { return tslist[1]; }
    const NodeList& implements() const { return tslist[1]; }
#define JSPARSE_LIST(fn, i)                          \
    NodeList& fn() { return list[i]; }               \
    const NodeList& fn() const { return list[i]; }
    JSPARSE_LIST(body_list, 0)
    JSPARSE_LIST(params, 0)
    JSPARSE_LIST(elements, 0)
    JSPARSE_LIST(properties, 0)
    JSPARSE_LIST(arguments, 0)
    JSPARSE_LIST(quasis, 0)
    JSPARSE_LIST(declarations, 0)
    JSPARSE_LIST(cases, 0)
    JSPARSE_LIST(consequent_list, 0)
    JSPARSE_LIST(specifiers, 0)
    JSPARSE_LIST(children, 0)
    JSPARSE_LIST(expressions, 1)
    JSPARSE_LIST(attributes, 1)
    JSPARSE_LIST(types, 0)          // TSUnionType / TSIntersectionType
    JSPARSE_LIST(elementTypes, 0)   // TSTupleType
    JSPARSE_LIST(members, 0)        // TSTypeLiteral / TSEnumBody
    JSPARSE_LIST(parameters, 0)     // TSIndexSignature
    JSPARSE_LIST(extends_list, 0)   // TSInterfaceDeclaration `extends`
    JSPARSE_LIST(template_types, 1) // TSTemplateLiteralType `types`
#undef JSPARSE_LIST
};

// Calls fn(child) for every non-null child node, in source order.
template <class Fn>
void ForEachChild(const Node* n, Fn&& fn);

struct Comment {
    bool block = false;      // "Block" (/* */) or "Line" (//, <!--, -->, #!)
    std::string_view value;  // text between the delimiters
    uint32_t start = 0, end = 0, ustart = 0, uend = 0;
    Pos loc_start, loc_end;
};

struct Token {
    const char* type = "";   // acorn's token type label: "name", "num", "{", "=>", "jsxName", ...
    std::string_view value;  // the token text for names/strings/operators (decoded), else empty
    uint32_t start = 0, end = 0, ustart = 0, uend = 0;
    Pos loc_start, loc_end;
};

struct Error {
    std::string message;   // acorn's text without the " (line:col)" suffix
    uint32_t upos = 0;     // acorn's err.pos (UTF-16 offset)
    uint32_t offset = 0;   // the same as a byte offset
    Pos loc;               // acorn's err.loc
    uint32_t raised_at = 0;  // acorn's err.raisedAt (UTF-16 offset)
    // acorn's err.message: "Unexpected token (3:7)".
    std::string Full() const;
};

enum class SourceType : uint8_t { Script, Module };

struct Options {
    int ecma_version = 100000000;  // "latest"; 3, 5, 6..17 or 2015..2026 also accepted
    SourceType source_type = SourceType::Script;
    bool jsx = false;
    int allow_hash_bang = -1;  // -1: acorn's default (on for ecmaVersion >= 2023)
    bool allow_return_outside_function = false;
    bool allow_import_export_everywhere = false;
    int allow_await_outside_function = -1;  // -1: acorn's default (null)
    bool allow_super_outside_method = false;
    bool check_private_fields = true;
    bool preserve_parens = false;  // ParenthesizedExpression nodes
    bool tokens = false;           // fill ParseResult::tokens
    bool recover = true;           // keep parsing after the first error
    int max_depth = 1500;          // nesting limit for the recursive descent
    // TypeScript (src/js_parse_ts*.cpp): the TS dialect, with type syntax
    // and typescript-estree's TS nodes. With jsx too it is .tsx (JSX, no
    // `<T>x` assertions); dts marks a declaration file (.d.ts). A TS parse
    // follows TypeScript's own parser: it accepts what tsc's parser accepts
    // (errors tsc leaves to its binder/checker -- duplicate declarations,
    // strict-mode words, misplaced modifiers -- are not reported), and the
    // source type does not matter (import/export are allowed anywhere).
    bool typescript = false;
    bool dts = false;
    // TypeScript: a .mts file, always an ES module, so a top-level `await`
    // is an AwaitExpression even without import/export (typescript-estree's
    // impliedNodeFormat). A .ts/.tsx/.cts file is a module only when it has
    // a top-level import or export (tsc's isFileProbablyExternalModule);
    // otherwise `await(1)` there is a call.
    bool mts = false;
};

class Arena;

struct ParseResult {
    ParseResult();
    ~ParseResult();
    ParseResult(ParseResult&&) noexcept;
    ParseResult& operator=(ParseResult&&) noexcept;

    std::unique_ptr<Arena> arena;  // owns every Node and string
    Node* program = nullptr;
    std::vector<Comment> comments;
    std::vector<Token> tokens;
    std::vector<Error> errors;  // errors[0] is acorn's
    bool has_error = false;
    uint32_t first_error_offset = 0;  // byte offset of errors[0]
    SourceType source_type = SourceType::Script;
    bool jsx = false;
    bool typescript = false;  // Options::typescript

    // u16_to_byte[i] is the byte offset of UTF-16 offset i (size n16 + 1).
    std::vector<uint32_t> u16_to_byte;
    uint32_t ByteOffset(uint32_t u16) const;
    uint32_t U16Offset(uint32_t byte) const;  // the UTF-16 offset at or before a byte offset
};

ParseResult Parse(std::string_view source, const Options& options = {});

// Parse as a module, and if that fails as a script; if both fail, the
// module result. (What `mep-js-parse-test --json --auto` and the parity
// harness do for .js/.jsx files.)
ParseResult ParseAuto(std::string_view source, Options options = {});

// acorn's JSON: JSON.stringify(acorn.parse(src, {ecmaVersion: "latest",
// locations: true, ...})). Literal regex and bigint `value`s are written as
// null (JSON.stringify can't write a RegExp or a BigInt); the `regex` and
// `bigint` fields carry them. With comments, the output is
// {"ast": ..., "comments": [...]}; on error it is
// {"error": "<message (l:c)>", "pos": N, "line": L, "column": C}.
std::string ToJson(const ParseResult& r, bool with_comments = true);

// --- implementation of the template above ---

void CollectChildren(const Node* n, std::vector<Node*>& out);  // source order

template <class Fn>
void ForEachChild(const Node* n, Fn&& fn) {
    std::vector<Node*> kids;
    CollectChildren(n, kids);
    for (Node* k : kids) fn(k);
}

}  // namespace jsparse
