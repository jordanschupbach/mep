#ifndef MEP_KOTLIN_PSI_H
#define MEP_KOTLIN_PSI_H

// The Kotlin PSI tree: what the Kotlin compiler's own parser (KotlinParsing
// over IntelliJ's PsiBuilder) builds, node for node and leaf for leaf. mep's
// parser (kotlin_parse.h) produces it; the Kotlin LSP reads its syntax errors
// and the formatter (a ktfmt port) walks it the way ktfmt walks PSI.
//
// The tree is an arena (KtTree::nodes) of KtNode. Every byte of the text is
// covered by exactly one leaf: tokens, whitespace (WHITE_SPACE) and comments
// (EOL_COMMENT, BLOCK_COMMENT, KDoc, SHEBANG_COMMENT) are all leaves, placed
// where the PSI places them (a declaration's leading doc comment is its first
// child, a trailing `// comment` on its line is its last child, and so on).
// Composite nodes are element types (FUN, CLASS_BODY, CALL_EXPRESSION, ...).
// ERROR_ELEMENT is a composite carrying the parser's message (possibly empty:
// a zero-width error). Lazily parsed elements (function BLOCKs, LAMBDA_
// EXPRESSIONs) are always expanded, as the IDE shows them once opened. KDoc
// comments are one leaf (their inner grammar is not modelled).
//
// Offsets are UTF-8 byte offsets into KtTree::text, which is the source with
// line ends normalised to \n (as the PSI sees it; see KtTree::ToOriginal).
//
// Type names (KtTypeName) are the PSI's own IElementType.toString(): elements
// print their KtNodeTypes name (FUN, WHEN_CONDITION_WITH_EXPRESSION, the root
// prints "kotlin.FILE"), hard and soft keywords print their text ("fun",
// "override"), other tokens their KtTokens name (IDENTIFIER, LBRACE,
// FLOAT_CONSTANT for a float literal), KDoc comments "KDoc". KtTree::Dump()
// prints exactly what `kotlin-check --tree` (tools/ts_grammar_diff/
// KotlinCheck.java) prints from the real compiler.
//
// Ported from the Kotlin compiler (JetBrains, Apache License 2.0),
// KtTokens.java, KtNodeTypes.java, version 2.4.10.
//
// API (stable; the formatter codes against it):
//   KtType                       one enum for tokens and elements
//   KtTypeName(t)                the PSI's toString() of a type
//   KtIsToken(t)                 a leaf type (tokens, whitespace, comments)
//   KtIsWhitespaceOrComment(t)   WHITE_SPACE and the four comment kinds
//   KtIsKeyword(t), KtIsSoftKeyword(t), KtIsModifierKeyword(t)
//   KtNode: type, start, end, parent/first_child/last_child/next/prev,
//           Text(), IsLeaf(), Child(t), NextSibling skipping trivia ...
//   KtTree: text, nodes (nodes[0] is the root), Root(), Dump(), errors()

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kt {

// Leaf types. X(enumerator, toString, kind): kind 0 plain token, 1 hard
// keyword (KtKeywordToken, not soft), 2 soft keyword, 3 parser pseudo-token.
#define KT_TOKEN_TYPES(X)                                         \
    X(WHITE_SPACE, "WHITE_SPACE", 0)                              \
    X(BAD_CHARACTER, "BAD_CHARACTER", 0)                          \
    X(EOL_COMMENT, "EOL_COMMENT", 0)                              \
    X(BLOCK_COMMENT, "BLOCK_COMMENT", 0)                          \
    X(DOC_COMMENT, "KDoc", 0)                                     \
    X(SHEBANG_COMMENT, "SHEBANG_COMMENT", 0)                      \
    X(RESERVED, "RESERVED", 0)                                    \
    X(INTEGER_LITERAL, "INTEGER_LITERAL", 0)                      \
    X(FLOAT_LITERAL, "FLOAT_CONSTANT", 0)                         \
    X(CHARACTER_LITERAL, "CHARACTER_LITERAL", 0)                  \
    X(INTERPOLATION_PREFIX, "INTERPOLATION_PREFIX", 0)            \
    X(CLOSING_QUOTE, "CLOSING_QUOTE", 0)                          \
    X(OPEN_QUOTE, "OPEN_QUOTE", 0)                                \
    X(REGULAR_STRING_PART, "REGULAR_STRING_PART", 0)              \
    X(ESCAPE_SEQUENCE, "ESCAPE_SEQUENCE", 0)                      \
    X(SHORT_TEMPLATE_ENTRY_START, "SHORT_TEMPLATE_ENTRY_START", 0) \
    X(LONG_TEMPLATE_ENTRY_START, "LONG_TEMPLATE_ENTRY_START", 0)  \
    X(LONG_TEMPLATE_ENTRY_END, "LONG_TEMPLATE_ENTRY_END", 0)      \
    X(DANGLING_NEWLINE, "DANGLING_NEWLINE", 0)                    \
    X(PACKAGE_KEYWORD, "package", 1)                              \
    X(AS_KEYWORD, "as", 1)                                        \
    X(TYPE_ALIAS_KEYWORD, "typealias", 1)                         \
    X(CLASS_KEYWORD, "class", 1)                                  \
    X(THIS_KEYWORD, "this", 1)                                    \
    X(SUPER_KEYWORD, "super", 1)                                  \
    X(VAL_KEYWORD, "val", 1)                                      \
    X(VAR_KEYWORD, "var", 1)                                      \
    X(FUN_KEYWORD, "fun", 1)                                      \
    X(FOR_KEYWORD, "for", 1)                                      \
    X(NULL_KEYWORD, "null", 1)                                    \
    X(TRUE_KEYWORD, "true", 1)                                    \
    X(FALSE_KEYWORD, "false", 1)                                  \
    X(IS_KEYWORD, "is", 1)                                        \
    X(IN_KEYWORD, "in", 1)                                        \
    X(THROW_KEYWORD, "throw", 1)                                  \
    X(RETURN_KEYWORD, "return", 1)                                \
    X(BREAK_KEYWORD, "break", 1)                                  \
    X(CONTINUE_KEYWORD, "continue", 1)                            \
    X(OBJECT_KEYWORD, "object", 1)                                \
    X(IF_KEYWORD, "if", 1)                                        \
    X(TRY_KEYWORD, "try", 1)                                      \
    X(ELSE_KEYWORD, "else", 1)                                    \
    X(WHILE_KEYWORD, "while", 1)                                  \
    X(DO_KEYWORD, "do", 1)                                        \
    X(WHEN_KEYWORD, "when", 1)                                    \
    X(INTERFACE_KEYWORD, "interface", 1)                          \
    X(TYPEOF_KEYWORD, "typeof", 1)                                \
    X(AS_SAFE, "AS_SAFE", 1)                                      \
    X(NOT_IN, "NOT_IN", 1)                                        \
    X(NOT_IS, "NOT_IS", 1)                                        \
    X(IDENTIFIER, "IDENTIFIER", 0)                                \
    X(FIELD_IDENTIFIER, "FIELD_IDENTIFIER", 0)                    \
    X(LBRACKET, "LBRACKET", 0)                                    \
    X(RBRACKET, "RBRACKET", 0)                                    \
    X(LBRACE, "LBRACE", 0)                                        \
    X(RBRACE, "RBRACE", 0)                                        \
    X(LPAR, "LPAR", 0)                                            \
    X(RPAR, "RPAR", 0)                                            \
    X(DOT, "DOT", 0)                                              \
    X(PLUSPLUS, "PLUSPLUS", 0)                                    \
    X(MINUSMINUS, "MINUSMINUS", 0)                                \
    X(MUL, "MUL", 0)                                              \
    X(PLUS, "PLUS", 0)                                            \
    X(MINUS, "MINUS", 0)                                          \
    X(EXCL, "EXCL", 0)                                            \
    X(DIV, "DIV", 0)                                              \
    X(PERC, "PERC", 0)                                            \
    X(LT, "LT", 0)                                                \
    X(GT, "GT", 0)                                                \
    X(LTEQ, "LTEQ", 0)                                            \
    X(GTEQ, "GTEQ", 0)                                            \
    X(EQEQEQ, "EQEQEQ", 0)                                        \
    X(ARROW, "ARROW", 0)                                          \
    X(DOUBLE_ARROW, "DOUBLE_ARROW", 0)                            \
    X(EXCLEQEQEQ, "EXCLEQEQEQ", 0)                                \
    X(EQEQ, "EQEQ", 0)                                            \
    X(EXCLEQ, "EXCLEQ", 0)                                        \
    X(EXCLEXCL, "EXCLEXCL", 0)                                    \
    X(ANDAND, "ANDAND", 0)                                        \
    X(AND, "AND", 0)                                              \
    X(OROR, "OROR", 0)                                            \
    X(SAFE_ACCESS, "SAFE_ACCESS", 0)                              \
    X(ELVIS, "ELVIS", 0)                                          \
    X(QUEST, "QUEST", 0)                                          \
    X(COLONCOLON, "COLONCOLON", 0)                                \
    X(COLON, "COLON", 0)                                          \
    X(SEMICOLON, "SEMICOLON", 0)                                  \
    X(DOUBLE_SEMICOLON, "DOUBLE_SEMICOLON", 0)                    \
    X(RANGE, "RANGE", 0)                                          \
    X(RANGE_UNTIL, "RANGE_UNTIL", 0)                              \
    X(EQ, "EQ", 0)                                                \
    X(MULTEQ, "MULTEQ", 0)                                        \
    X(DIVEQ, "DIVEQ", 0)                                          \
    X(PERCEQ, "PERCEQ", 0)                                        \
    X(PLUSEQ, "PLUSEQ", 0)                                        \
    X(MINUSEQ, "MINUSEQ", 0)                                      \
    X(HASH, "HASH", 0)                                            \
    X(AT, "AT", 0)                                                \
    X(COMMA, "COMMA", 0)                                          \
    X(EOL_OR_SEMICOLON, "EOL_OR_SEMICOLON", 3)                    \
    X(ALL_KEYWORD, "all", 2)                                      \
    X(FILE_KEYWORD, "file", 2)                                    \
    X(FIELD_KEYWORD, "field", 2)                                  \
    X(PROPERTY_KEYWORD, "property", 2)                            \
    X(RECEIVER_KEYWORD, "receiver", 2)                            \
    X(PARAM_KEYWORD, "param", 2)                                  \
    X(SETPARAM_KEYWORD, "setparam", 2)                            \
    X(DELEGATE_KEYWORD, "delegate", 2)                            \
    X(IMPORT_KEYWORD, "import", 2)                                \
    X(WHERE_KEYWORD, "where", 2)                                  \
    X(BY_KEYWORD, "by", 2)                                        \
    X(GET_KEYWORD, "get", 2)                                      \
    X(SET_KEYWORD, "set", 2)                                      \
    X(CONSTRUCTOR_KEYWORD, "constructor", 2)                      \
    X(INIT_KEYWORD, "init", 2)                                    \
    X(CONTEXT_KEYWORD, "context", 2)                              \
    X(ABSTRACT_KEYWORD, "abstract", 2)                            \
    X(ENUM_KEYWORD, "enum", 2)                                    \
    X(CONTRACT_KEYWORD, "contract", 2)                            \
    X(OPEN_KEYWORD, "open", 2)                                    \
    X(INNER_KEYWORD, "inner", 2)                                  \
    X(OVERRIDE_KEYWORD, "override", 2)                            \
    X(PRIVATE_KEYWORD, "private", 2)                              \
    X(PUBLIC_KEYWORD, "public", 2)                                \
    X(INTERNAL_KEYWORD, "internal", 2)                            \
    X(PROTECTED_KEYWORD, "protected", 2)                          \
    X(CATCH_KEYWORD, "catch", 2)                                  \
    X(OUT_KEYWORD, "out", 2)                                      \
    X(VARARG_KEYWORD, "vararg", 2)                                \
    X(REIFIED_KEYWORD, "reified", 2)                              \
    X(DYNAMIC_KEYWORD, "dynamic", 2)                              \
    X(COMPANION_KEYWORD, "companion", 2)                          \
    X(SEALED_KEYWORD, "sealed", 2)                                \
    X(FINALLY_KEYWORD, "finally", 2)                              \
    X(FINAL_KEYWORD, "final", 2)                                  \
    X(LATEINIT_KEYWORD, "lateinit", 2)                            \
    X(DATA_KEYWORD, "data", 2)                                    \
    X(VALUE_KEYWORD, "value", 2)                                  \
    X(INLINE_KEYWORD, "inline", 2)                                \
    X(NOINLINE_KEYWORD, "noinline", 2)                            \
    X(TAILREC_KEYWORD, "tailrec", 2)                              \
    X(EXTERNAL_KEYWORD, "external", 2)                            \
    X(ANNOTATION_KEYWORD, "annotation", 2)                        \
    X(CROSSINLINE_KEYWORD, "crossinline", 2)                      \
    X(OPERATOR_KEYWORD, "operator", 2)                            \
    X(INFIX_KEYWORD, "infix", 2)                                  \
    X(CONST_KEYWORD, "const", 2)                                  \
    X(SUSPEND_KEYWORD, "suspend", 2)                              \
    X(EXPECT_KEYWORD, "expect", 2)                                \
    X(ACTUAL_KEYWORD, "actual", 2)

// Composite (element) types: X(enumerator, toString).
#define KT_ELEMENT_TYPES(X)                                                          \
    X(KT_FILE, "kotlin.FILE")                                                        \
    X(ERROR_ELEMENT, "ERROR_ELEMENT")                                                \
    X(CLASS, "CLASS")                                                                \
    X(FUN, "FUN")                                                                    \
    X(PROPERTY, "PROPERTY")                                                          \
    X(DESTRUCTURING_DECLARATION, "DESTRUCTURING_DECLARATION")                        \
    X(DESTRUCTURING_DECLARATION_ENTRY, "DESTRUCTURING_DECLARATION_ENTRY")            \
    X(OBJECT_DECLARATION, "OBJECT_DECLARATION")                                      \
    X(TYPEALIAS, "TYPEALIAS")                                                        \
    X(COMPANION_BLOCK, "COMPANION_BLOCK")                                            \
    X(ENUM_ENTRY, "ENUM_ENTRY")                                                      \
    X(CLASS_INITIALIZER, "CLASS_INITIALIZER")                                        \
    X(SCRIPT_INITIALIZER, "SCRIPT_INITIALIZER")                                      \
    X(SECONDARY_CONSTRUCTOR, "SECONDARY_CONSTRUCTOR")                                \
    X(PRIMARY_CONSTRUCTOR, "PRIMARY_CONSTRUCTOR")                                    \
    X(CONTEXT_RECEIVER, "CONTEXT_RECEIVER")                                          \
    X(CONTEXT_PARAMETER_LIST, "CONTEXT_PARAMETER_LIST")                              \
    X(TYPE_PARAMETER_LIST, "TYPE_PARAMETER_LIST")                                    \
    X(TYPE_PARAMETER, "TYPE_PARAMETER")                                              \
    X(SUPER_TYPE_LIST, "SUPER_TYPE_LIST")                                            \
    X(DELEGATED_SUPER_TYPE_ENTRY, "DELEGATED_SUPER_TYPE_ENTRY")                      \
    X(SUPER_TYPE_CALL_ENTRY, "SUPER_TYPE_CALL_ENTRY")                                \
    X(SUPER_TYPE_ENTRY, "SUPER_TYPE_ENTRY")                                          \
    X(PROPERTY_DELEGATE, "PROPERTY_DELEGATE")                                        \
    X(CONSTRUCTOR_CALLEE, "CONSTRUCTOR_CALLEE")                                      \
    X(VALUE_PARAMETER_LIST, "VALUE_PARAMETER_LIST")                                  \
    X(VALUE_PARAMETER, "VALUE_PARAMETER")                                            \
    X(CLASS_BODY, "CLASS_BODY")                                                      \
    X(IMPORT_LIST, "IMPORT_LIST")                                                    \
    X(FILE_ANNOTATION_LIST, "FILE_ANNOTATION_LIST")                                  \
    X(IMPORT_DIRECTIVE, "IMPORT_DIRECTIVE")                                          \
    X(IMPORT_ALIAS, "IMPORT_ALIAS")                                                  \
    X(MODIFIER_LIST, "MODIFIER_LIST")                                                \
    X(ANNOTATION, "ANNOTATION")                                                      \
    X(ANNOTATION_ENTRY, "ANNOTATION_ENTRY")                                          \
    X(ANNOTATION_TARGET, "ANNOTATION_TARGET")                                        \
    X(TYPE_ARGUMENT_LIST, "TYPE_ARGUMENT_LIST")                                      \
    X(VALUE_ARGUMENT_LIST, "VALUE_ARGUMENT_LIST")                                    \
    X(VALUE_ARGUMENT, "VALUE_ARGUMENT")                                              \
    X(CONTRACT_EFFECT_LIST, "CONTRACT_EFFECT_LIST")                                  \
    X(CONTRACT_EFFECT, "CONTRACT_EFFECT")                                            \
    X(LAMBDA_ARGUMENT, "LAMBDA_ARGUMENT")                                            \
    X(VALUE_ARGUMENT_NAME, "VALUE_ARGUMENT_NAME")                                    \
    X(TYPE_REFERENCE, "TYPE_REFERENCE")                                              \
    X(USER_TYPE, "USER_TYPE")                                                        \
    X(DYNAMIC_TYPE, "DYNAMIC_TYPE")                                                  \
    X(FUNCTION_TYPE, "FUNCTION_TYPE")                                                \
    X(FUNCTION_TYPE_RECEIVER, "FUNCTION_TYPE_RECEIVER")                              \
    X(NULLABLE_TYPE, "NULLABLE_TYPE")                                                \
    X(INTERSECTION_TYPE, "INTERSECTION_TYPE")                                        \
    X(TYPE_PROJECTION, "TYPE_PROJECTION")                                            \
    X(PROPERTY_ACCESSOR, "PROPERTY_ACCESSOR")                                        \
    X(BACKING_FIELD, "BACKING_FIELD")                                                \
    X(INITIALIZER_LIST, "INITIALIZER_LIST")                                          \
    X(TYPE_CONSTRAINT_LIST, "TYPE_CONSTRAINT_LIST")                                  \
    X(TYPE_CONSTRAINT, "TYPE_CONSTRAINT")                                            \
    X(CONSTRUCTOR_DELEGATION_CALL, "CONSTRUCTOR_DELEGATION_CALL")                    \
    X(CONSTRUCTOR_DELEGATION_REFERENCE, "CONSTRUCTOR_DELEGATION_REFERENCE")          \
    X(NULL_, "NULL")                                                                 \
    X(BOOLEAN_CONSTANT, "BOOLEAN_CONSTANT")                                          \
    X(FLOAT_CONSTANT, "FLOAT_CONSTANT")                                              \
    X(CHARACTER_CONSTANT, "CHARACTER_CONSTANT")                                      \
    X(INTEGER_CONSTANT, "INTEGER_CONSTANT")                                          \
    X(STRING_TEMPLATE, "STRING_TEMPLATE")                                            \
    X(LONG_STRING_TEMPLATE_ENTRY, "LONG_STRING_TEMPLATE_ENTRY")                      \
    X(SHORT_STRING_TEMPLATE_ENTRY, "SHORT_STRING_TEMPLATE_ENTRY")                    \
    X(LITERAL_STRING_TEMPLATE_ENTRY, "LITERAL_STRING_TEMPLATE_ENTRY")                \
    X(ESCAPE_STRING_TEMPLATE_ENTRY, "ESCAPE_STRING_TEMPLATE_ENTRY")                  \
    X(STRING_INTERPOLATION_PREFIX, "STRING_INTERPOLATION_PREFIX")                    \
    X(PARENTHESIZED, "PARENTHESIZED")                                                \
    X(RETURN, "RETURN")                                                              \
    X(THROW, "THROW")                                                                \
    X(CONTINUE, "CONTINUE")                                                          \
    X(BREAK, "BREAK")                                                                \
    X(IF, "IF")                                                                      \
    X(CONDITION, "CONDITION")                                                        \
    X(THEN, "THEN")                                                                  \
    X(ELSE, "ELSE")                                                                  \
    X(TRY, "TRY")                                                                    \
    X(CATCH, "CATCH")                                                                \
    X(FINALLY, "FINALLY")                                                            \
    X(FOR, "FOR")                                                                    \
    X(WHILE, "WHILE")                                                                \
    X(DO_WHILE, "DO_WHILE")                                                          \
    X(LOOP_RANGE, "LOOP_RANGE")                                                      \
    X(BODY, "BODY")                                                                  \
    X(BLOCK, "BLOCK")                                                                \
    X(LAMBDA_EXPRESSION, "LAMBDA_EXPRESSION")                                        \
    X(FUNCTION_LITERAL, "FUNCTION_LITERAL")                                          \
    X(ANNOTATED_EXPRESSION, "ANNOTATED_EXPRESSION")                                  \
    X(REFERENCE_EXPRESSION, "REFERENCE_EXPRESSION")                                  \
    X(ENUM_ENTRY_SUPERCLASS_REFERENCE_EXPRESSION, "ENUM_ENTRY_SUPERCLASS_REFERENCE_EXPRESSION") \
    X(OPERATION_REFERENCE, "OPERATION_REFERENCE")                                    \
    X(LABEL, "LABEL")                                                                \
    X(LABEL_QUALIFIER, "LABEL_QUALIFIER")                                            \
    X(THIS_EXPRESSION, "THIS_EXPRESSION")                                            \
    X(SUPER_EXPRESSION, "SUPER_EXPRESSION")                                          \
    X(BINARY_EXPRESSION, "BINARY_EXPRESSION")                                        \
    X(BINARY_WITH_TYPE, "BINARY_WITH_TYPE")                                          \
    X(IS_EXPRESSION, "IS_EXPRESSION")                                                \
    X(PREFIX_EXPRESSION, "PREFIX_EXPRESSION")                                        \
    X(POSTFIX_EXPRESSION, "POSTFIX_EXPRESSION")                                      \
    X(LABELED_EXPRESSION, "LABELED_EXPRESSION")                                      \
    X(CALL_EXPRESSION, "CALL_EXPRESSION")                                            \
    X(ARRAY_ACCESS_EXPRESSION, "ARRAY_ACCESS_EXPRESSION")                            \
    X(INDICES, "INDICES")                                                            \
    X(DOT_QUALIFIED_EXPRESSION, "DOT_QUALIFIED_EXPRESSION")                          \
    X(CALLABLE_REFERENCE_EXPRESSION, "CALLABLE_REFERENCE_EXPRESSION")                \
    X(CLASS_LITERAL_EXPRESSION, "CLASS_LITERAL_EXPRESSION")                          \
    X(SAFE_ACCESS_EXPRESSION, "SAFE_ACCESS_EXPRESSION")                              \
    X(OBJECT_LITERAL, "OBJECT_LITERAL")                                              \
    X(WHEN, "WHEN")                                                                  \
    X(WHEN_ENTRY, "WHEN_ENTRY")                                                      \
    X(WHEN_ENTRY_GUARD, "WHEN_ENTRY_GUARD")                                          \
    X(WHEN_CONDITION_IN_RANGE, "WHEN_CONDITION_IN_RANGE")                            \
    X(WHEN_CONDITION_IS_PATTERN, "WHEN_CONDITION_IS_PATTERN")                        \
    X(WHEN_CONDITION_EXPRESSION, "WHEN_CONDITION_WITH_EXPRESSION")                   \
    X(COLLECTION_LITERAL_EXPRESSION, "COLLECTION_LITERAL_EXPRESSION")                \
    X(PACKAGE_DIRECTIVE, "PACKAGE_DIRECTIVE")                                        \
    X(SCRIPT, "SCRIPT")

enum class KtType : uint16_t {
    NONE = 0,  // "no token": PsiBuilder's null (end of input)
#define KT_X_TOKEN(e, s, k) e,
    KT_TOKEN_TYPES(KT_X_TOKEN)
#undef KT_X_TOKEN
#define KT_X_ELEMENT(e, s) e,
    KT_ELEMENT_TYPES(KT_X_ELEMENT)
#undef KT_X_ELEMENT
    kCount
};

constexpr size_t kKtTypeCount = static_cast<size_t>(KtType::kCount);

/** @brief The PSI's toString() of a type ("FUN", "fun", "IDENTIFIER", "kotlin.FILE"). */
const char *KtTypeName(KtType t);
/** @brief A leaf type (token, whitespace, comment, BAD_CHARACTER). */
inline bool KtIsToken(KtType t) { return t > KtType::NONE && t < KtType::KT_FILE; }
/** @brief An element (composite) type, ERROR_ELEMENT included. */
inline bool KtIsElement(KtType t) { return t >= KtType::KT_FILE && t < KtType::kCount; }
/** @brief WHITE_SPACE, EOL_COMMENT, BLOCK_COMMENT, KDoc, SHEBANG_COMMENT. */
bool KtIsWhitespaceOrComment(KtType t);
/** @brief EOL_COMMENT, BLOCK_COMMENT, KDoc, SHEBANG_COMMENT. */
bool KtIsComment(KtType t);
/** @brief A KtKeywordToken: hard keywords (`fun`, `as?` ...) and soft ones (`override`, `get`). */
bool KtIsKeyword(KtType t);
/** @brief A soft keyword (`get`, `override`, `data` ...); the lexer emits these as IDENTIFIER and
 *  the parser remaps them where they act as keywords. */
bool KtIsSoftKeyword(KtType t);
/** @brief One of KtTokens.MODIFIER_KEYWORDS (`public` ... `crossinline`, plus `fun` and `in`). */
bool KtIsModifierKeyword(KtType t);

class KtTree;

struct KtNode {
    KtType type = KtType::NONE;
    uint32_t start = 0, end = 0;  // byte offsets into tree->text
    KtNode *parent = nullptr;
    KtNode *first_child = nullptr, *last_child = nullptr;
    KtNode *next = nullptr, *prev = nullptr;  // siblings
    const KtTree *tree = nullptr;
    int32_t message = -1;  // ERROR_ELEMENT: index into tree->messages

    bool IsLeaf() const { return KtIsToken(type); }
    bool IsError() const { return type == KtType::ERROR_ELEMENT; }
    bool IsTrivia() const { return KtIsWhitespaceOrComment(type); }
    /** @brief The node's source text (a view into the tree's text). */
    std::string_view Text() const;
    /** @brief ERROR_ELEMENT's message ("" for other nodes). */
    std::string_view Message() const;
    /** @brief First direct child of the given type, or null (PSI's findChildByType). */
    KtNode *Child(KtType t) const;
    /** @brief Last direct child of the given type, or null. */
    KtNode *LastChild(KtType t) const;
    /** @brief All direct children of the given type, in order. */
    std::vector<KtNode *> Children(KtType t) const;
    /** @brief All direct children, in order. */
    std::vector<KtNode *> Children() const;
    /** @brief Next/previous sibling that is not whitespace or a comment. */
    KtNode *NextNonTrivia() const;
    KtNode *PrevNonTrivia() const;
    /** @brief First/last direct child that is not whitespace or a comment. */
    KtNode *FirstNonTriviaChild() const;
    KtNode *LastNonTriviaChild() const;
    /** @brief Nearest ancestor (not this node) of the given type, or null. */
    KtNode *Ancestor(KtType t) const;
    /** @brief The first / last leaf at or below this node (null for an empty element). */
    const KtNode *FirstLeaf() const;
    const KtNode *LastLeaf() const;
};

struct KtSyntaxError {
    std::string message;
    uint32_t offset = 0;  // byte offset into the normalised text
    int line = 0;         // 1-based
    int col = 0;          // 1-based, in UTF-16 units (as the PSI and kotlinc count)
};

class KtTree {
public:
    KtTree() = default;
    KtTree(const KtTree &) = delete;
    KtTree &operator=(const KtTree &) = delete;

    std::string text;                   // the parsed text, line ends normalised to \n
    std::vector<KtNode> nodes;          // nodes[0] is the root (KT_FILE); never reallocated after parsing
    std::vector<std::string> messages;  // ERROR_ELEMENT messages
    std::vector<KtSyntaxError> errors;  // every ERROR_ELEMENT in tree (pre-)order
    bool depth_limited = false;         // parsing stopped at the nesting cap (see kotlin_parse.h)

    const KtNode *Root() const { return nodes.empty() ? nullptr : &nodes[0]; }
    KtNode *Root() { return nodes.empty() ? nullptr : &nodes[0]; }
    /** @brief The tree as `kotlin-check --tree` prints it: one line per node, two spaces of indent
     *  per depth, `TYPE START END`, an ERROR_ELEMENT adds ` message`. */
    std::string Dump() const;
    /** @brief 1-based line and UTF-16 column of a byte offset into text. */
    void LineCol(uint32_t offset, int *line, int *col) const;
    /** @brief The byte offset in the original (un-normalised) input of an offset into text. */
    uint32_t ToOriginal(uint32_t offset) const;
    /** @brief The deepest leaf containing offset (the one starting there at a boundary). */
    const KtNode *LeafAt(uint32_t offset) const;

    // Internal: offsets of each \n-normalised byte in the original input; empty when the input
    // had no \r.
    std::vector<uint32_t> original_offsets;
};

/** @brief Normalises \r\n and lone \r to \n (IntelliJ's StringUtil.convertLineSeparators). When
 *  map is given, (*map)[i] is the original offset of out[i] (plus one entry for the end). */
std::string KtNormalizeLineEnds(std::string_view in, std::vector<uint32_t> *map = nullptr);

}  // namespace kt

#endif
