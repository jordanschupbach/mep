#ifndef MEP_JAVA_AST_H
#define MEP_JAVA_AST_H

// Java's tokens and syntax tree, shaped like javac's (JDK 25), for mep's own
// Java tools: the parser (java_parse.cpp), the formatter (a port of
// google-java-format's JavaInputAstVisitor, which walks javac's JCTree) and
// the language server (java_lsp.cpp).
//
// Written from javac's observable behaviour (its token stream, its trees and
// positions as JavacTask reports them, its diagnostics), checked with
// tools/java_parse_check.py against the JDK itself; no javac code is copied.
// The names are javac's public ones: TokenKind = com.sun.tools.javac.parser.
// Tokens.TokenKind, Kind = com.sun.source.tree.Tree.Kind, the node structs
// and their fields = com.sun.tools.javac.tree.JCTree's (JCClassDecl ->
// ClassDecl, JCMethodDecl -> MethodDecl, ...), Flags = javac's Flags values.
//
// Positions are byte offsets into the original UTF-8 text (javac's are
// UTF-16 offsets; the two agree on ASCII). A Unicode escape (A) is
// decoded before lexing, as javac does, but every position stays an offset
// into the raw text. -1 is javac's NOPOS.
//
// Usage (tokens only):
//     javaast::Lexed lx = javaast::Lex(src);
//     for (const javaast::Token &t : lx.tokens) ...   // ends with EOF
//     lx.comments[t.comment_begin .. t.comment_end)   // comments before t
// Usage (tree): see java_parse.h.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace javaast {

// --- tokens ---------------------------------------------------------------------------

// javac's Tokens.TokenKind, in its order. Contextual keywords (var, yield,
// record, sealed, permits, non-sealed's parts, when, module, open, requires,
// exports, ...) are IDENTIFIER, as in javac; `_` is UNDERSCORE.
enum class TokenKind : uint8_t {
    EOF_, ERROR, IDENTIFIER,
    ABSTRACT, ASSERT, BOOLEAN, BREAK, BYTE, CASE, CATCH, CHAR, CLASS, CONST, CONTINUE, DEFAULT, DO, DOUBLE,
    ELSE, ENUM, EXTENDS, FINAL, FINALLY, FLOAT, FOR, GOTO, IF, IMPLEMENTS, IMPORT, INSTANCEOF, INT, INTERFACE,
    LONG, NATIVE, NEW, PACKAGE, PRIVATE, PROTECTED, PUBLIC, RETURN, SHORT, STATIC, STRICTFP, SUPER, SWITCH,
    SYNCHRONIZED, THIS, THROW, THROWS, TRANSIENT, TRY, VOID, VOLATILE, WHILE,
    INTLITERAL, LONGLITERAL, FLOATLITERAL, DOUBLELITERAL, CHARLITERAL, STRINGLITERAL, STRINGFRAGMENT,
    TRUE, FALSE, NULL_, UNDERSCORE,
    ARROW, COLCOL, LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET, SEMI, COMMA, DOT, ELLIPSIS,
    EQ, GT, LT, BANG, TILDE, QUES, COLON, EQEQ, LTEQ, GTEQ, BANGEQ, AMPAMP, BARBAR, PLUSPLUS, SUBSUB,
    PLUS, SUB, STAR, SLASH, AMP, BAR, CARET, PERCENT, LTLT, GTGT, GTGTGT,
    PLUSEQ, SUBEQ, STAREQ, SLASHEQ, AMPEQ, BAREQ, CARETEQ, PERCENTEQ, LTLTEQ, GTGTEQ, GTGTGTEQ,
    MONKEYS_AT, CUSTOM,
};
constexpr int kTokenKindCount = static_cast<int>(TokenKind::CUSTOM) + 1;

/** @brief The enum constant's name: "IDENTIFIER", "EOF", "NULL", "LPAREN". */
const char *TokenKindName(TokenKind k);
/** @brief The token's spelling ("(", "class", ">>>="), or "" for IDENTIFIER, literals, ERROR, EOF. */
const char *TokenKindSpelling(TokenKind k);
/** @brief javac's TokenKind.toString() as its diagnostics print it: "<identifier>", "'('", "class", "=". */
std::string TokenKindDescription(TokenKind k);
inline bool IsKeyword(TokenKind k) {
    return (k >= TokenKind::ABSTRACT && k <= TokenKind::WHILE) || k == TokenKind::TRUE || k == TokenKind::FALSE ||
           k == TokenKind::NULL_;
}
/** @brief The keyword spelled `s` (incl. true/false/null and `_` -> UNDERSCORE), or IDENTIFIER. */
TokenKind LookupKeyword(std::string_view s);

// javac's Tokens.Comment.CommentStyle.
enum class CommentStyle : uint8_t { LINE, BLOCK, JAVADOC_BLOCK, JAVADOC_LINE };
const char *CommentStyleName(CommentStyle s);

struct Comment {
    CommentStyle style = CommentStyle::LINE;
    bool deprecated = false;  // a doc comment with an @deprecated tag (sets Flags::DEPRECATED on the next declaration)
    int32_t start = 0, end = 0;  // bytes, [start, end): `//...` excludes the line terminator
};

enum TokenFlags : uint8_t {
    kTokTextBlock = 1,       // a STRINGLITERAL (or ERROR) written as """..."""
    kTokHasEscapes = 2,      // spelled with a Unicode escape or an ignorable character
    kTokDeprecated = 4,      // a doc comment before it says @deprecated (javac's Token.deprecatedFlag())
};

struct Token {
    TokenKind kind = TokenKind::EOF_;
    uint8_t flags = 0;
    uint8_t radix = 0;  // numeric literals: 2, 8, 10, 16
    int32_t start = 0, end = 0;               // bytes into the source, [start, end)
    uint32_t comment_begin = 0, comment_end = 0;  // Lexed::comments between the previous token and this one
    std::string_view name;  // IDENTIFIER and keywords: the name after escapes (views the source or Lexed's pool)
};

// A diagnostic key (javac's compiler.properties, without "compiler.err.").
struct Diag {
    int32_t pos = -1;        // byte offset (javac's preferred position)
    std::string key;         // e.g. "expected", "illegal.char"
    std::vector<std::string> args;
};
/** @brief The English message javac prints for a key and arguments ("';' expected"). */
std::string DiagMessage(const std::string &key, const std::vector<std::string> &args);

struct LexError {
    Diag diag;
    uint32_t token = 0;  // index of the token whose scanning reported it (lexing is lazy in javac)
};

struct Lexed {
    std::string_view src;
    std::vector<Token> tokens;      // always ends with EOF_
    std::vector<Comment> comments;  // in source order; tokens refer to runs of it
    std::vector<LexError> errors;   // in the order javac reports them
    std::unique_ptr<std::vector<std::string>> pool;  // storage for names spelled with escapes
};

/** @brief javac's scanner over a whole file (UTF-8; malformed bytes read as U+FFFD, as javac's decoder does). */
Lexed Lex(std::string_view src);

/** @brief The value of a STRINGLITERAL or CHARLITERAL token's text (escapes processed; a text block's
 *  indentation stripped as String.stripIndent does). UTF-8. */
std::string LiteralValue(std::string_view src, const Token &t);

// Lines and javac's columns (1-based; UTF-16 units; a tab advances to the next multiple of 8).
class LineMap {
public:
    explicit LineMap(std::string_view src);
    int Line(int32_t pos) const;    // 1-based
    int Column(int32_t pos) const;  // 1-based, javac's getColumnNumber
    int32_t LineStart(int line) const;
    int LineCount() const { return static_cast<int>(starts_.size()); }

private:
    std::string_view src_;
    std::vector<int32_t> starts_;
};

// --- trees ----------------------------------------------------------------------------

// com.sun.source.tree.Tree.Kind, in its order.
enum class Kind : uint8_t {
    ANNOTATED_TYPE, ANNOTATION, TYPE_ANNOTATION, ARRAY_ACCESS, ARRAY_TYPE, ASSERT, ASSIGNMENT, BLOCK, BREAK,
    CASE, CATCH, CLASS, COMPILATION_UNIT, CONDITIONAL_EXPRESSION, CONTINUE, DO_WHILE_LOOP, ENHANCED_FOR_LOOP,
    EXPRESSION_STATEMENT, MEMBER_SELECT, MEMBER_REFERENCE, FOR_LOOP, IDENTIFIER, IF, IMPORT, INSTANCE_OF,
    LABELED_STATEMENT, METHOD, METHOD_INVOCATION, MODIFIERS, NEW_ARRAY, NEW_CLASS, LAMBDA_EXPRESSION, PACKAGE,
    PARENTHESIZED, ANY_PATTERN, BINDING_PATTERN, DEFAULT_CASE_LABEL, CONSTANT_CASE_LABEL, PATTERN_CASE_LABEL,
    DECONSTRUCTION_PATTERN, PRIMITIVE_TYPE, RETURN, EMPTY_STATEMENT, SWITCH, SWITCH_EXPRESSION, SYNCHRONIZED,
    THROW, TRY, PARAMETERIZED_TYPE, UNION_TYPE, INTERSECTION_TYPE, TYPE_CAST, TYPE_PARAMETER, VARIABLE,
    WHILE_LOOP, POSTFIX_INCREMENT, POSTFIX_DECREMENT, PREFIX_INCREMENT, PREFIX_DECREMENT, UNARY_PLUS,
    UNARY_MINUS, BITWISE_COMPLEMENT, LOGICAL_COMPLEMENT, MULTIPLY, DIVIDE, REMAINDER, PLUS, MINUS, LEFT_SHIFT,
    RIGHT_SHIFT, UNSIGNED_RIGHT_SHIFT, LESS_THAN, GREATER_THAN, LESS_THAN_EQUAL, GREATER_THAN_EQUAL, EQUAL_TO,
    NOT_EQUAL_TO, AND, XOR, OR, CONDITIONAL_AND, CONDITIONAL_OR, MULTIPLY_ASSIGNMENT, DIVIDE_ASSIGNMENT,
    REMAINDER_ASSIGNMENT, PLUS_ASSIGNMENT, MINUS_ASSIGNMENT, LEFT_SHIFT_ASSIGNMENT, RIGHT_SHIFT_ASSIGNMENT,
    UNSIGNED_RIGHT_SHIFT_ASSIGNMENT, AND_ASSIGNMENT, XOR_ASSIGNMENT, OR_ASSIGNMENT, INT_LITERAL, LONG_LITERAL,
    FLOAT_LITERAL, DOUBLE_LITERAL, BOOLEAN_LITERAL, CHAR_LITERAL, STRING_LITERAL, NULL_LITERAL,
    UNBOUNDED_WILDCARD, EXTENDS_WILDCARD, SUPER_WILDCARD, ERRONEOUS, INTERFACE, ENUM, ANNOTATION_TYPE, MODULE,
    EXPORTS, OPENS, PROVIDES, RECORD, REQUIRES, USES, OTHER, YIELD,
};
const char *KindName(Kind k);
inline bool IsClassKind(Kind k) {
    return k == Kind::CLASS || k == Kind::INTERFACE || k == Kind::ENUM || k == Kind::ANNOTATION_TYPE ||
           k == Kind::RECORD;
}
inline bool IsUnaryKind(Kind k) { return k >= Kind::POSTFIX_INCREMENT && k <= Kind::LOGICAL_COMPLEMENT; }
inline bool IsBinaryKind(Kind k) { return k >= Kind::MULTIPLY && k <= Kind::CONDITIONAL_OR; }
inline bool IsCompoundAssignKind(Kind k) { return k >= Kind::MULTIPLY_ASSIGNMENT && k <= Kind::OR_ASSIGNMENT; }
inline bool IsLiteralKind(Kind k) { return k >= Kind::INT_LITERAL && k <= Kind::NULL_LITERAL; }
inline bool IsWildcardKind(Kind k) { return k >= Kind::UNBOUNDED_WILDCARD && k <= Kind::SUPER_WILDCARD; }
/** @brief The operator's spelling for a unary, binary or compound-assignment kind ("+", "<<=", "++"). */
const char *OperatorSpelling(Kind k);

// javac's Flags (com.sun.tools.javac.code.Flags), the ones the parser sets.
namespace Flags {
constexpr uint64_t PUBLIC = 1, PRIVATE = 2, PROTECTED = 4, STATIC = 8, FINAL = 16, SYNCHRONIZED = 32,
                   VOLATILE = 64, TRANSIENT = 128, NATIVE = 256, INTERFACE = 512, ABSTRACT = 1024,
                   STRICTFP = 2048, ANNOTATION = 8192, ENUM = 16384, DEPRECATED = 131072,
                   IMPLICIT_CLASS = 524288, GENERATED_MEMBER = 16777216, PARAMETER = 1ull << 33,
                   VARARGS = 1ull << 34, DEFAULT = 1ull << 43, SEALED = 1ull << 48,
                   COMPACT_RECORD_CONSTRUCTOR = 1ull << 51, RECORD = 1ull << 61, NON_SEALED = 1ull << 63;
}  // namespace Flags

// javac's TypeTag, for literals and primitive types.
enum class TypeTag : uint8_t { NONE, BYTE, CHAR, SHORT, LONG, FLOAT, INT, DOUBLE, BOOLEAN, VOID, CLASS, BOT, ERROR };
const char *TypeTagName(TypeTag t);

struct Tree {
    Kind kind;
    int32_t start = -1;  // TreeInfo.getStartPos
    int32_t pos = -1;    // JCTree.pos: the position javac's diagnostics anchor on
    int32_t end = -1;    // TreeInfo.getEndPos (exclusive)
    template <class T> T *As() { return static_cast<T *>(this); }
    template <class T> const T *As() const { return static_cast<const T *>(this); }
};

// An immutable list in the arena (javac's com.sun.tools.javac.util.List).
template <class T> struct List {
    T **data = nullptr;
    uint32_t count = 0;
    uint32_t size() const { return count; }
    bool empty() const { return count == 0; }
    T *operator[](size_t i) const { return data[i]; }
    T **begin() const { return data; }
    T **end() const { return data + count; }
    T *head() const { return count ? data[0] : nullptr; }
    T *last() const { return count ? data[count - 1] : nullptr; }
};

using Name = std::string_view;
using Expression = Tree;  // JCExpression
using Statement = Tree;   // JCStatement

struct Annotation;
struct Modifiers;
struct TypeParameter;
struct VariableDecl;
struct Block;
struct Case;
struct Catch;
struct ClassDecl;
struct Pattern;

struct CompilationUnit : Tree {   // COMPILATION_UNIT
    List<Tree> defs;              // PackageDecl, Import (incl. module imports), class decls, ModuleDecl
};
struct PackageDecl : Tree {       // PACKAGE
    List<Annotation> annotations;
    Expression *pid = nullptr;
};
struct Import : Tree {            // IMPORT (javac's JCImport; JCModuleImport when isModule)
    bool staticImport = false;
    bool isModule = false;        // `import module M;` (qualid is then null)
    Expression *qualid = nullptr; // a MEMBER_SELECT (JCFieldAccess)
    Expression *module = nullptr; // the module name of `import module`
};
struct ClassDecl : Tree {         // CLASS, INTERFACE, ENUM, ANNOTATION_TYPE, RECORD
    Modifiers *mods = nullptr;
    Name name;                    // empty for an anonymous class body
    List<TypeParameter> typarams;
    Expression *extending = nullptr;
    List<Expression> implementing;
    List<Expression> permitting;
    List<Tree> defs;
};
struct MethodDecl : Tree {        // METHOD
    Modifiers *mods = nullptr;
    Name name;                    // "<init>" for a constructor
    Expression *restype = nullptr;  // null for a constructor
    List<TypeParameter> typarams;
    VariableDecl *recvparam = nullptr;
    List<VariableDecl> params;
    List<Expression> thrown;
    Block *body = nullptr;
    Expression *defaultValue = nullptr;
};
struct VariableDecl : Tree {      // VARIABLE
    Modifiers *mods = nullptr;
    Name name;
    Expression *nameexpr = nullptr;  // receiver parameter's `this` / `Outer.this`
    Expression *vartype = nullptr;   // null for `var` and implicitly typed lambda parameters
    Expression *init = nullptr;
    bool declaredUsingVar = false;
};
struct Skip : Tree {};            // EMPTY_STATEMENT
struct Block : Tree {             // BLOCK
    uint64_t flags = 0;           // Flags::STATIC for a static initializer
    List<Statement> stats;
    int32_t endpos = -1;          // the closing brace
};
struct DoWhileLoop : Tree { Statement *body = nullptr; Expression *cond = nullptr; };
struct WhileLoop : Tree { Expression *cond = nullptr; Statement *body = nullptr; };
struct ForLoop : Tree {
    List<Statement> init;
    Expression *cond = nullptr;
    List<Statement> step;         // ExpressionStatements
    Statement *body = nullptr;
};
struct EnhancedForLoop : Tree { VariableDecl *var = nullptr; Expression *expr = nullptr; Statement *body = nullptr; };
struct LabeledStatement : Tree { Name label; Statement *body = nullptr; };
struct Switch : Tree {            // SWITCH
    Expression *selector = nullptr;
    List<Case> cases;
    int32_t endpos = -1;
};
enum class CaseKind : uint8_t { STATEMENT, RULE };
struct Case : Tree {              // CASE
    CaseKind caseKind = CaseKind::STATEMENT;
    List<Tree> labels;            // DEFAULT_CASE_LABEL, CONSTANT_CASE_LABEL, PATTERN_CASE_LABEL
    Expression *guard = nullptr;
    List<Statement> stats;
    Tree *body = nullptr;         // RULE: the expression, block or throw statement after `->`
};
struct SwitchExpression : Tree {  // SWITCH_EXPRESSION
    Expression *selector = nullptr;
    List<Case> cases;
    int32_t endpos = -1;
};
struct Synchronized : Tree { Expression *lock = nullptr; Block *body = nullptr; };
struct Try : Tree {
    List<Tree> resources;         // VariableDecls or expressions
    Block *body = nullptr;
    List<Catch> catchers;
    Block *finalizer = nullptr;
};
struct Catch : Tree { VariableDecl *param = nullptr; Block *body = nullptr; };
struct Conditional : Tree {       // CONDITIONAL_EXPRESSION
    Expression *cond = nullptr, *truepart = nullptr, *falsepart = nullptr;
};
struct If : Tree { Expression *cond = nullptr; Statement *thenpart = nullptr; Statement *elsepart = nullptr; };
struct ExpressionStatement : Tree { Expression *expr = nullptr; };
struct Break : Tree { Name label; };
struct Yield : Tree { Expression *value = nullptr; };
struct Continue : Tree { Name label; };
struct Return : Tree { Expression *expr = nullptr; };
struct Throw : Tree { Expression *expr = nullptr; };
struct Assert : Tree { Expression *cond = nullptr; Expression *detail = nullptr; };
struct MethodInvocation : Tree {
    List<Expression> typeargs;
    Expression *meth = nullptr;
    List<Expression> args;
};
struct NewClass : Tree {
    Expression *encl = nullptr;
    List<Expression> typeargs;
    Expression *clazz = nullptr;
    List<Expression> args;
    ClassDecl *def = nullptr;
};
struct NewArray : Tree {
    Expression *elemtype = nullptr;
    List<Expression> dims;
    List<Annotation> annotations;
    List<List<Annotation>> dimAnnotations;  // stored as lists of lists, javac's shape
    List<Expression> elems;       // null-vs-empty: has_elems says whether `{...}` was written
    bool has_elems = false;
};
enum class ParameterKind : uint8_t { IMPLICIT, EXPLICIT };
struct Lambda : Tree {            // LAMBDA_EXPRESSION
    List<VariableDecl> params;
    Tree *body = nullptr;         // an expression or a BLOCK
    ParameterKind paramKind = ParameterKind::IMPLICIT;
};
struct Parens : Tree { Expression *expr = nullptr; };
struct Assign : Tree { Expression *lhs = nullptr, *rhs = nullptr; };
struct AssignOp : Tree { Expression *lhs = nullptr, *rhs = nullptr; };   // kind: *_ASSIGNMENT
struct Unary : Tree { Expression *arg = nullptr; };                      // kind: the unary kinds
struct Binary : Tree { Expression *lhs = nullptr, *rhs = nullptr; };     // kind: the binary kinds
struct TypeCast : Tree { Tree *clazz = nullptr; Expression *expr = nullptr; };
struct InstanceOf : Tree { Expression *expr = nullptr; Tree *pattern = nullptr; };  // pattern: a type or a pattern
struct Pattern : Tree {};
struct AnyPattern : Pattern {};   // ANY_PATTERN (`_`)
struct BindingPattern : Pattern { VariableDecl *var = nullptr; };
struct DefaultCaseLabel : Tree {};
struct ConstantCaseLabel : Tree { Expression *expr = nullptr; };
struct PatternCaseLabel : Tree { Pattern *pat = nullptr; };
struct RecordPattern : Pattern {  // DECONSTRUCTION_PATTERN
    Expression *deconstructor = nullptr;
    List<Pattern> nested;
};
struct ArrayAccess : Tree { Expression *indexed = nullptr, *index = nullptr; };
struct FieldAccess : Tree { Expression *selected = nullptr; Name name; };   // MEMBER_SELECT
enum class ReferenceMode : uint8_t { INVOKE, NEW };
struct MemberReference : Tree {
    ReferenceMode mode = ReferenceMode::INVOKE;
    Name name;                    // "<init>" for ::new
    Expression *expr = nullptr;
    List<Expression> typeargs;
    bool has_typeargs = false;    // javac's typeargs is null (not empty) without `::<...>`
};
struct Ident : Tree { Name name; };   // IDENTIFIER
struct Literal : Tree {           // *_LITERAL
    TypeTag typetag = TypeTag::NONE;
    std::string_view text;        // the source spelling
};
struct PrimitiveTypeTree : Tree { TypeTag typetag = TypeTag::NONE; };   // PRIMITIVE_TYPE
struct ArrayTypeTree : Tree { Expression *elemtype = nullptr; };         // ARRAY_TYPE
struct TypeApply : Tree { Expression *clazz = nullptr; List<Expression> arguments; };  // PARAMETERIZED_TYPE
struct TypeUnion : Tree { List<Expression> alternatives; };
struct TypeIntersection : Tree { List<Expression> bounds; };
struct TypeParameter : Tree {
    Name name;
    List<Expression> bounds;
    List<Annotation> annotations;
};
enum class BoundKind : uint8_t { EXTENDS, SUPER, UNBOUND };
struct Wildcard : Tree {          // *_WILDCARD
    BoundKind boundKind = BoundKind::UNBOUND;  // javac: kind.kind
    int32_t kind_pos = -1;        // the TypeBoundKind tree's position (the `?`... javac's quirk)
    Tree *inner = nullptr;
};
struct Annotation : Tree {        // ANNOTATION or TYPE_ANNOTATION
    Tree *annotationType = nullptr;
    List<Expression> args;
};
struct Modifiers : Tree {         // MODIFIERS
    uint64_t flags = 0;
    List<Annotation> annotations;
};
struct AnnotatedType : Tree {     // ANNOTATED_TYPE
    List<Annotation> annotations;
    Expression *underlyingType = nullptr;
};
enum class ModuleKind : uint8_t { OPEN, STRONG };
struct ModuleDecl : Tree {        // MODULE
    Modifiers *mods = nullptr;
    ModuleKind type = ModuleKind::STRONG;
    Expression *qualId = nullptr;
    List<Tree> directives;
};
struct Exports : Tree { Expression *qualid = nullptr; List<Expression> moduleNames; };
struct Opens : Tree { Expression *qualid = nullptr; List<Expression> moduleNames; };
struct Provides : Tree { Expression *serviceName = nullptr; List<Expression> implNames; };
struct Requires : Tree { bool isTransitive = false, isStaticPhase = false; Expression *moduleName = nullptr; };
struct Uses : Tree { Expression *qualid = nullptr; };
struct Erroneous : Tree { List<Tree> errs; };   // ERRONEOUS

// Bump allocator for one parse: every node and list lives here; nothing has a destructor.
class Arena {
public:
    Arena() = default;
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;
    void *Allocate(size_t n, size_t align);
    template <class T> T *New(Kind k) {
        T *t = new (Allocate(sizeof(T), alignof(T))) T();
        t->kind = k;
        return t;
    }
    template <class T> List<T> MakeList(const std::vector<T *> &v) {
        List<T> l;
        if (v.empty()) return l;
        l.data = static_cast<T **>(Allocate(sizeof(T *) * v.size(), alignof(T *)));
        for (size_t i = 0; i < v.size(); i++) l.data[i] = v[i];
        l.count = static_cast<uint32_t>(v.size());
        return l;
    }
    std::string_view Intern(std::string_view s);
    size_t BytesUsed() const { return used_; }

private:
    std::vector<std::unique_ptr<char[]>> blocks_;
    char *cur_ = nullptr;
    size_t left_ = 0;
    size_t used_ = 0;
};

/** @brief Calls f(child, field, index) for each child of `t` in javac's field order (field names are
 *  JCTree's: "mods", "typarams", "defs", ...; index is -1 for a single child, else its list index).
 *  Null children are skipped. */
void ForEachChild(const Tree *t, void (*f)(const Tree *child, const char *field, int index, void *ctx), void *ctx);
template <class F> void VisitChildren(const Tree *t, F &&f) {
    ForEachChild(
        t, [](const Tree *c, const char *field, int index, void *ctx) { (*static_cast<F *>(ctx))(c, field, index); },
        &f);
}

/** @brief The tree in tools/JavaParseDump.java's format (one node per line, javac field names). */
std::string DumpTree(const Tree *t, std::string_view src);

}  // namespace javaast

#endif
