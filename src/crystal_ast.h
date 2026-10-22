#pragma once

// The Crystal AST: the node classes of the Crystal compiler's
// src/compiler/crystal/syntax/ast.cr (Crystal 1.19), as one tagged C++
// struct. A node's `kind` is its Crystal class; the fields below are the
// classes' instance variables, shared where several classes have one of
// the same meaning (each kind's comment in NodeKind says which it uses).
// Nodes live in an Ast arena and point at each other.
//
// Locations follow the original's getters: a node with no location of its
// own may report a child's (Expressions' first expression's; the
// end_location of an Assign is its value's, and so on) -- see Loc() and
// EndLoc().
//
// Crystal is Apache-2.0 licensed; this is a derived work (see
// crystal_syntax.h).

#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "crystal_syntax.h"

namespace crystal {

enum class NodeKind : uint8_t {
    Nop,
    Expressions,          // exps, keyword
    NilLiteral,
    BoolLiteral,          // bool_value
    NumberLiteral,        // value, number_kind
    CharLiteral,          // ch
    StringLiteral,        // value
    StringInterpolation,  // exps, heredoc_indent
    SymbolLiteral,        // value
    ArrayLiteral,         // exps (elements), of, name_node (name)
    HashLiteral,          // entries, of_key/of_value (has_of), name_node
    NamedTupleLiteral,    // named_entries
    RangeLiteral,         // from (left), to (right), exclusive
    RegexLiteral,         // value_node (value), regex_options
    TupleLiteral,         // exps (elements)
    Var,                  // name, doc
    Block,                // exps (args), body, splat_index, unpacks
    Call,                 // obj, name, args, block, block_arg, named_args, name_location, global...
    NamedArgument,        // name, value_node (value)
    If,                   // cond, then_, else_, ternary, else_location
    Unless,               // cond, then_, else_, else_location
    Assign,               // target, value_node (value), doc
    OpAssign,             // target, op (value), value_node, name_location
    MultiAssign,          // exps (targets), exps2 (values)
    InstanceVar,          // name
    ReadInstanceVar,      // obj, name
    ClassVar,             // name
    Global,               // name
    And,                  // left, right
    Or,                   // left, right
    Arg,                  // name, external_name, default_value, restriction, doc, annotations
    ProcNotation,         // exps (inputs; has_inputs), output (right)
    Def,                  // free_vars (names; has_names), receiver, name, args, double_splat, body, block_arg,
                          // return_type, block_arity, name_location, splat_index, doc, visibility, flags...
    Macro,                // name, args, body, double_splat, block_arg, name_location, splat_index, doc, visibility
    Not,                  // exp
    PointerOf,            // exp
    SizeOf,               // exp
    InstanceSizeOf,       // exp
    AlignOf,              // exp
    InstanceAlignOf,      // exp
    Out,                  // exp
    OffsetOf,             // left (offsetof_type), right (offset)
    VisibilityModifier,   // modifier, exp, doc
    IsA,                  // obj, const_ (right), nil_check
    RespondsTo,           // obj, name
    Require,              // value (string)
    When,                 // exps (conds), body, exhaustive
    Case,                 // cond, exps (whens), else_, exhaustive
    Select,               // exps (whens), else_
    ImplicitObj,
    Path,                 // names, global, visibility
    ClassDef,             // name_node (Path), body, superclass, names (type_vars; has_names), name_location, doc,
                          // splat_index, abstract, struct, visibility
    ModuleDef,            // name_node, body, names (type_vars), splat_index, name_location, doc, visibility
    AnnotationDef,        // name_node, doc, name_location
    While,                // cond, body
    Until,                // cond, body
    Generic,              // name_node, exps (type_vars), named_args, suffix
    TypeDeclaration,      // doc, var, declared_type, value_node
    UninitializedVar,     // var, declared_type
    Rescue,               // body, exps (types; has_exps), name (has_name)
    ExceptionHandler,     // body, exps (rescues; has_exps), else_, ensure_, implicit, suffix, else_location,
                          // ensure_location
    ProcLiteral,          // def_
    ProcPointer,          // obj, name, args, global
    Union,                // exps (types)
    Self,
    Return,               // exp
    Break,                // exp
    Next,                 // exp
    Yield,                // exps, scope, has_parentheses
    Include,              // name_node
    Extend,               // name_node
    LibDef,               // name_node, doc, body, name_location, visibility
    FunDef,               // name, args, return_type, body, real_name, doc, varargs, name_location
    TypeDef,              // name, doc, type_spec (restriction), name_location
    CStructOrUnionDef,    // name, doc, body, union
    EnumDef,              // name_node, exps (members), base_type (restriction), doc, visibility
    ExternalVar,          // name, doc, type_spec (restriction), real_name (has_real_name)
    Alias,                // name_node, value_node, doc, visibility
    Metaclass,            // name_node
    Cast,                 // obj, to (right)
    NilableCast,          // obj, to (right)
    TypeOf,               // exps
    Annotation,           // path (name_node), args, named_args, doc
    MacroExpression,      // exp, output
    MacroLiteral,         // value
    MacroVerbatim,        // exp
    MacroIf,              // cond, then_, else_, is_unless
    MacroFor,             // exps (vars), exp, body
    MacroVar,             // name, exps (has_exps)
    Underscore,
    Splat,                // exp
    DoubleSplat,          // exp
    MagicConstant,        // magic
    Asm,                  // value (text), exps (outputs; has_exps), exps2 (inputs; has_exps2), names (clobbers;
                          // has_names), volatile_, alignstack, intel, can_throw
    AsmOperand,           // value (constraint), exp
};

/** @brief The Crystal class name of a kind ("Call", "StringInterpolation"...). */
const char *NodeKindName(NodeKind k);

enum class Visibility : uint8_t { Public, Protected, Private };
enum class ExpressionsKeyword : uint8_t { None, Paren, Begin };
enum class GenericSuffix : uint8_t { None, Question, Asterisk, Bracket };

// Regex::CompileOptions bits the parser sets.
constexpr unsigned kRegexIgnoreCase = 0x1;
constexpr unsigned kRegexMultiline = 0x6;  // MULTILINE | DOTALL in Crystal's PCRE2 options
constexpr unsigned kRegexExtended = 0x8;

struct Node {
    NodeKind kind = NodeKind::Nop;
    Location location;      // own location (may be unknown: see Loc())
    Location end_location;  // own end location (see EndLoc())
    Location name_location;
    Location else_location;
    Location ensure_location;

    std::string name;           // Var/Call/Def/... name
    std::string value;          // literal value / string / op / text / constraint / real_name
    std::string external_name;  // Arg
    std::string doc;
    bool has_doc = false;

    char32_t ch = 0;
    NumberKind number_kind = NumberKind::I32;
    bool bool_value = false;
    unsigned regex_options = 0;
    Tok magic = Tok::EOF_;
    Visibility visibility = Visibility::Public;
    Visibility modifier = Visibility::Public;
    ExpressionsKeyword keyword = ExpressionsKeyword::None;
    GenericSuffix suffix = GenericSuffix::None;

    int splat_index = -1;  // -1: nil
    int block_arity = -1;  // -1: nil
    int heredoc_indent = 0;
    int name_size_ = -1;  // Call's cached name size (-1: compute)

    // Flags (only meaningful on the kinds that have them).
    bool global = false;       // Call, Path, ProcPointer
    bool exclusive = false;    // RangeLiteral
    bool ternary = false;      // If
    bool is_abstract = false;  // Def, ClassDef
    bool is_struct = false;    // ClassDef
    bool macro_def = false;    // Def
    bool calls_super = false, calls_initialize = false, calls_previous_def = false, uses_block_arg = false,
         assigns_special_var = false;  // Def
    bool nil_check = false;            // IsA
    bool exhaustive = false;           // When, Case
    bool has_parentheses = false;      // Call, Yield
    bool args_in_brackets = false;     // Call
    bool expansion = false;            // Call
    bool varargs = false;              // FunDef
    bool output = true;                // MacroExpression
    bool is_unless = false;            // MacroIf
    bool implicit = false;             // ExceptionHandler
    bool suffix_ = false;              // ExceptionHandler
    bool is_union = false;             // CStructOrUnionDef
    bool volatile_ = false, alignstack = false, intel = false, can_throw = false;  // Asm
    bool has_exps = false;   // nilable `exps` present (Rescue types, ProcNotation inputs, MacroVar exps...)
    bool has_exps2 = false;  // Asm inputs
    bool has_names = false;  // nilable `names` present (type_vars, free_vars, clobbers)
    bool has_name = false;   // Rescue name
    bool has_real_name = false;  // ExternalVar
    bool has_named_args = false;  // Call/Generic/Annotation named_args non-nil
    bool has_of = false;          // HashLiteral of

    // Children.
    Node *obj = nullptr;
    Node *exp = nullptr;
    Node *cond = nullptr;
    Node *then_ = nullptr;
    Node *else_ = nullptr;
    Node *body = nullptr;
    Node *target = nullptr;
    Node *value_node = nullptr;
    Node *left = nullptr;   // And/Or left, RangeLiteral from, OffsetOf type
    Node *right = nullptr;  // And/Or right, RangeLiteral to, ProcNotation output, Cast to, IsA const, OffsetOf offset
    Node *block = nullptr;
    Node *block_arg = nullptr;
    Node *receiver = nullptr;
    Node *return_type = nullptr;
    Node *restriction = nullptr;  // Arg restriction, TypeDef/ExternalVar type_spec, EnumDef base_type
    Node *default_value = nullptr;
    Node *superclass = nullptr;
    Node *of = nullptr;
    Node *name_node = nullptr;  // ArrayLiteral/HashLiteral name, ClassDef/ModuleDef/... name, Generic name, Include
    Node *scope = nullptr;
    Node *ensure_ = nullptr;
    Node *double_splat = nullptr;
    Node *def_ = nullptr;
    Node *var = nullptr;
    Node *declared_type = nullptr;
    Node *of_key = nullptr, *of_value = nullptr;  // HashLiteral of

    std::vector<Node *> exps;
    std::vector<Node *> exps2;
    std::vector<Node *> args;
    std::vector<Node *> named_args;
    std::vector<Node *> annotations;  // Arg parsed_annotations
    bool has_annotations = false;
    std::vector<std::string> names;
    std::vector<std::pair<Node *, Node *>> entries;              // HashLiteral
    std::vector<std::pair<std::string, Node *>> named_entries;  // NamedTupleLiteral
    std::map<int, Node *> unpacks;                               // Block (index -> Expressions)
    bool has_unpacks = false;

    bool is(NodeKind k) const { return kind == k; }
    bool is_binary_op() const { return kind == NodeKind::And || kind == NodeKind::Or; }
    bool is_unary_expression() const {
        switch (kind) {
            case NodeKind::Not: case NodeKind::PointerOf: case NodeKind::SizeOf: case NodeKind::InstanceSizeOf:
            case NodeKind::AlignOf: case NodeKind::InstanceAlignOf: case NodeKind::Out: case NodeKind::MacroVerbatim:
            case NodeKind::Splat: case NodeKind::DoubleSplat:
                return true;
            default:
                return false;
        }
    }
    bool is_control_expression() const {
        return kind == NodeKind::Return || kind == NodeKind::Break || kind == NodeKind::Next;
    }
    /** @brief SpecialVar#special_var?: `$~`, `$?`. */
    bool special_var() const { return !name.empty() && name[0] == '$'; }
    /** @brief Call#has_any_args?. */
    bool has_any_args() const {
        if (kind == NodeKind::Call) return !args.empty() || has_named_args || block_arg;
        if (kind == NodeKind::Def || kind == NodeKind::Macro || kind == NodeKind::ProcPointer || kind == NodeKind::Block)
            return !args.empty();
        return false;
    }
    void set_doc(const Token &t) {
        if (t.has_doc) {
            has_doc = true;
            doc = t.doc;
        }
    }
    /** @brief ASTNode#doc= for the kinds that keep a doc comment. */
    void assign_doc(bool has, const std::string &d);
    /** @brief ASTNode#visibility= for the kinds that have one (Assign forwards to its target). */
    void set_visibility(Visibility v);
};

/** @brief ASTNode#location: the node's own, or for Expressions its first expression's. */
Location Loc(const Node *n);
/** @brief ASTNode#end_location, with the subclasses' fallbacks. */
Location EndLoc(const Node *n);
/** @brief The number of characters of the name (ASTNode#name_size). */
int NameSize(Node *n);

/** @brief Owns a parse's nodes. */
class Ast {
public:
    Node *make(NodeKind k) {
        nodes_.emplace_back();
        nodes_.back().kind = k;
        return &nodes_.back();
    }
    size_t size() const { return nodes_.size(); }

private:
    std::deque<Node> nodes_;
};

/** @brief Prints a tree in tools/crystal_ast_dump.cr's format (the oracle the parser is checked against). */
std::string DumpAst(const Node *root);

}  // namespace crystal
