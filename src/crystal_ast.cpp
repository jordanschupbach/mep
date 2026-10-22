// The Crystal AST's helpers (see crystal_ast.h) and the dump printer that
// matches tools/crystal_ast_dump.cr (format: tools/crystal_ast_dump.md).

#include "crystal_ast.h"

#include <cstdio>
#include <functional>

namespace crystal {

const char *NodeKindName(NodeKind k) {
    switch (k) {
        case NodeKind::Nop: return "Nop";
        case NodeKind::Expressions: return "Expressions";
        case NodeKind::NilLiteral: return "NilLiteral";
        case NodeKind::BoolLiteral: return "BoolLiteral";
        case NodeKind::NumberLiteral: return "NumberLiteral";
        case NodeKind::CharLiteral: return "CharLiteral";
        case NodeKind::StringLiteral: return "StringLiteral";
        case NodeKind::StringInterpolation: return "StringInterpolation";
        case NodeKind::SymbolLiteral: return "SymbolLiteral";
        case NodeKind::ArrayLiteral: return "ArrayLiteral";
        case NodeKind::HashLiteral: return "HashLiteral";
        case NodeKind::NamedTupleLiteral: return "NamedTupleLiteral";
        case NodeKind::RangeLiteral: return "RangeLiteral";
        case NodeKind::RegexLiteral: return "RegexLiteral";
        case NodeKind::TupleLiteral: return "TupleLiteral";
        case NodeKind::Var: return "Var";
        case NodeKind::Block: return "Block";
        case NodeKind::Call: return "Call";
        case NodeKind::NamedArgument: return "NamedArgument";
        case NodeKind::If: return "If";
        case NodeKind::Unless: return "Unless";
        case NodeKind::Assign: return "Assign";
        case NodeKind::OpAssign: return "OpAssign";
        case NodeKind::MultiAssign: return "MultiAssign";
        case NodeKind::InstanceVar: return "InstanceVar";
        case NodeKind::ReadInstanceVar: return "ReadInstanceVar";
        case NodeKind::ClassVar: return "ClassVar";
        case NodeKind::Global: return "Global";
        case NodeKind::And: return "And";
        case NodeKind::Or: return "Or";
        case NodeKind::Arg: return "Arg";
        case NodeKind::ProcNotation: return "ProcNotation";
        case NodeKind::Def: return "Def";
        case NodeKind::Macro: return "Macro";
        case NodeKind::Not: return "Not";
        case NodeKind::PointerOf: return "PointerOf";
        case NodeKind::SizeOf: return "SizeOf";
        case NodeKind::InstanceSizeOf: return "InstanceSizeOf";
        case NodeKind::AlignOf: return "AlignOf";
        case NodeKind::InstanceAlignOf: return "InstanceAlignOf";
        case NodeKind::Out: return "Out";
        case NodeKind::OffsetOf: return "OffsetOf";
        case NodeKind::VisibilityModifier: return "VisibilityModifier";
        case NodeKind::IsA: return "IsA";
        case NodeKind::RespondsTo: return "RespondsTo";
        case NodeKind::Require: return "Require";
        case NodeKind::When: return "When";
        case NodeKind::Case: return "Case";
        case NodeKind::Select: return "Select";
        case NodeKind::ImplicitObj: return "ImplicitObj";
        case NodeKind::Path: return "Path";
        case NodeKind::ClassDef: return "ClassDef";
        case NodeKind::ModuleDef: return "ModuleDef";
        case NodeKind::AnnotationDef: return "AnnotationDef";
        case NodeKind::While: return "While";
        case NodeKind::Until: return "Until";
        case NodeKind::Generic: return "Generic";
        case NodeKind::TypeDeclaration: return "TypeDeclaration";
        case NodeKind::UninitializedVar: return "UninitializedVar";
        case NodeKind::Rescue: return "Rescue";
        case NodeKind::ExceptionHandler: return "ExceptionHandler";
        case NodeKind::ProcLiteral: return "ProcLiteral";
        case NodeKind::ProcPointer: return "ProcPointer";
        case NodeKind::Union: return "Union";
        case NodeKind::Self: return "Self";
        case NodeKind::Return: return "Return";
        case NodeKind::Break: return "Break";
        case NodeKind::Next: return "Next";
        case NodeKind::Yield: return "Yield";
        case NodeKind::Include: return "Include";
        case NodeKind::Extend: return "Extend";
        case NodeKind::LibDef: return "LibDef";
        case NodeKind::FunDef: return "FunDef";
        case NodeKind::TypeDef: return "TypeDef";
        case NodeKind::CStructOrUnionDef: return "CStructOrUnionDef";
        case NodeKind::EnumDef: return "EnumDef";
        case NodeKind::ExternalVar: return "ExternalVar";
        case NodeKind::Alias: return "Alias";
        case NodeKind::Metaclass: return "Metaclass";
        case NodeKind::Cast: return "Cast";
        case NodeKind::NilableCast: return "NilableCast";
        case NodeKind::TypeOf: return "TypeOf";
        case NodeKind::Annotation: return "Annotation";
        case NodeKind::MacroExpression: return "MacroExpression";
        case NodeKind::MacroLiteral: return "MacroLiteral";
        case NodeKind::MacroVerbatim: return "MacroVerbatim";
        case NodeKind::MacroIf: return "MacroIf";
        case NodeKind::MacroFor: return "MacroFor";
        case NodeKind::MacroVar: return "MacroVar";
        case NodeKind::Underscore: return "Underscore";
        case NodeKind::Splat: return "Splat";
        case NodeKind::DoubleSplat: return "DoubleSplat";
        case NodeKind::MagicConstant: return "MagicConstant";
        case NodeKind::Asm: return "Asm";
        case NodeKind::AsmOperand: return "AsmOperand";
    }
    return "?";
}

Location Loc(const Node *n) {
    if (!n) return Location();
    if (n->location.known()) return n->location;
    if (n->kind == NodeKind::Expressions && !n->exps.empty()) return Loc(n->exps.front());
    return n->location;
}

Location EndLoc(const Node *n) {
    if (!n) return Location();
    if (n->end_location.known()) return n->end_location;
    switch (n->kind) {
        case NodeKind::Expressions:
            return n->exps.empty() ? Location() : EndLoc(n->exps.back());
        case NodeKind::Assign:
        case NodeKind::OpAssign:
        case NodeKind::NamedArgument:
            return EndLoc(n->value_node);
        case NodeKind::MultiAssign:
            return n->exps2.empty() ? Location() : EndLoc(n->exps2.back());
        case NodeKind::And:
        case NodeKind::Or:
            return EndLoc(n->right);
        case NodeKind::VisibilityModifier:
            return EndLoc(n->exp);
        case NodeKind::Yield:
            return n->exps.empty() ? Location() : EndLoc(n->exps.back());
        case NodeKind::Include:
        case NodeKind::Extend:
            return EndLoc(n->name_node);
        case NodeKind::Cast:
        case NodeKind::NilableCast:
            return EndLoc(n->right);
        case NodeKind::Return:
        case NodeKind::Break:
        case NodeKind::Next:
            return n->exp ? EndLoc(n->exp) : Location();
        default:
            if (n->is_unary_expression()) return EndLoc(n->exp);
            return n->end_location;
    }
}

int NameSize(Node *n) {
    switch (n->kind) {
        case NodeKind::Call:
            if (n->name_size_ == -1) {
                const bool trim = !n->name.empty() && (n->name.back() == '=' || n->name.back() == '@');
                n->name_size_ = static_cast<int>(Utf8Length(n->name)) - (trim ? 1 : 0);
            }
            return n->name_size_;
        case NodeKind::Var: case NodeKind::InstanceVar: case NodeKind::Global: case NodeKind::Arg: case NodeKind::Def:
        case NodeKind::Macro:
            return static_cast<int>(Utf8Length(n->name));
        case NodeKind::Path: {
            int s = 0;
            for (const std::string &x : n->names) s += static_cast<int>(Utf8Length(x));
            return s + (static_cast<int>(n->names.size()) + (n->global ? 0 : -1)) * 2;
        }
        case NodeKind::TypeDeclaration:
        case NodeKind::UninitializedVar: {
            Node *v = n->var;
            if (v && (v->is(NodeKind::Var) || v->is(NodeKind::InstanceVar) || v->is(NodeKind::ClassVar))) {
                return static_cast<int>(Utf8Length(v->name));
            }
            return 0;
        }
        default:
            return 0;
    }
}

void Node::assign_doc(bool has, const std::string &d) {
    switch (kind) {
        case NodeKind::Var: case NodeKind::Call: case NodeKind::Assign: case NodeKind::Arg: case NodeKind::Def:
        case NodeKind::Macro: case NodeKind::VisibilityModifier: case NodeKind::ClassDef: case NodeKind::ModuleDef:
        case NodeKind::AnnotationDef: case NodeKind::TypeDeclaration: case NodeKind::LibDef: case NodeKind::FunDef:
        case NodeKind::TypeDef: case NodeKind::CStructOrUnionDef: case NodeKind::EnumDef: case NodeKind::ExternalVar:
        case NodeKind::Alias: case NodeKind::Annotation:
            has_doc = has;
            doc = has ? d : std::string();
            break;
        default:
            break;
    }
}

void Node::set_visibility(Visibility v) {
    switch (kind) {
        case NodeKind::Call: case NodeKind::Def: case NodeKind::Macro: case NodeKind::Path: case NodeKind::ClassDef:
        case NodeKind::ModuleDef: case NodeKind::LibDef: case NodeKind::EnumDef: case NodeKind::Alias:
            visibility = v;
            break;
        case NodeKind::Assign:
            if (target) target->set_visibility(v);
            break;
        default:
            break;
    }
}

// --- the dump -----------------------------------------------------------------------

namespace {

std::string Hex2(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "\\x%02X", v & 0xFF);
    return buf;
}

std::string Escape(std::string_view s, char quote) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            const char c = static_cast<char>(b);
            if (c == quote) {
                out += '\\';
                out += c;
            } else if (c == '\\') {
                out += "\\\\";
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c == '\r') {
                out += "\\r";
            } else if (b < 0x20 || b == 0x7F) {
                out += Hex2(b);
            } else {
                out += c;
            }
            i++;
            continue;
        }
        // Multi-byte: copy a valid sequence, escape each byte of an invalid one.
        int n = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n > 0 && i + static_cast<size_t>(n) <= s.size();
        char32_t c = n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; ok && k < n; k++) {
            const auto bb = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((bb & 0xC0) != 0x80) {
                ok = false;
            } else {
                c = (c << 6) | (bb & 0x3F);
            }
        }
        if (ok && ((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
                   (c >= 0xD800 && c <= 0xDFFF))) {
            ok = false;
        }
        if (ok) {
            out.append(s.substr(i, static_cast<size_t>(n)));
            i += static_cast<size_t>(n);
        } else {
            out += Hex2(b);
            i++;
        }
    }
    return out;
}

std::string Str(std::string_view s) { return "\"" + Escape(s, '"') + "\""; }

std::string LocS(const Location &l) {
    if (!l.known()) return "?";
    return std::to_string(l.line) + ":" + std::to_string(l.column);
}

const char *VisName(Visibility v) {
    switch (v) {
        case Visibility::Public: return "Public";
        case Visibility::Protected: return "Protected";
        case Visibility::Private: return "Private";
    }
    return "Public";
}

std::string StrArray(const std::vector<std::string> &v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ", ";
        s += Str(v[i]);
    }
    return s + "]";
}

class Dumper {
public:
    std::string out;

    void node(const Node *n, int depth, const std::string &label) {
        std::string line(static_cast<size_t>(depth) * 2, ' ');
        if (!label.empty()) line += label + ": ";
        line += NodeKindName(n->kind);
        line += " " + LocS(Loc(n)) + "-" + LocS(EndLoc(n));
        Fields f;
        fields(n, f);
        for (auto &s : f.scalars) line += " " + s;
        out += line + "\n";
        for (auto &c : f.children) c(depth + 1);
    }

private:
    struct Fields {
        std::vector<std::string> scalars;
        std::vector<std::function<void(int)>> children;
        void scalar(const char *name, const std::string &value) { scalars.push_back(std::string(name) + "=" + value); }
    };

    void child(Fields &f, const char *name, const Node *c) {
        if (!c) return;
        const std::string label = name;
        f.children.push_back([this, label, c](int d) { node(c, d, label); });
    }
    void list(Fields &f, const char *name, const std::vector<Node *> &v) {
        for (size_t i = 0; i < v.size(); i++) {
            const std::string label = std::string(name) + "[" + std::to_string(i) + "]";
            const Node *c = v[i];
            f.children.push_back([this, label, c](int d) { node(c, d, label); });
        }
    }
    static std::string B(bool b) { return b ? "true" : "false"; }

    void fields(const Node *n, Fields &f) {
        switch (n->kind) {
            case NodeKind::Nop: case NodeKind::NilLiteral: case NodeKind::ImplicitObj: case NodeKind::Self:
            case NodeKind::Underscore:
                break;
            case NodeKind::Expressions:
                list(f, "expressions", n->exps);
                f.scalar("keyword", n->keyword == ExpressionsKeyword::None    ? "None"
                                    : n->keyword == ExpressionsKeyword::Paren ? "Paren"
                                                                              : "Begin");
                break;
            case NodeKind::BoolLiteral: f.scalar("value", B(n->bool_value)); break;
            case NodeKind::NumberLiteral:
                f.scalar("value", Str(n->value));
                f.scalar("kind", NumberKindName(n->number_kind));
                break;
            case NodeKind::CharLiteral: f.scalar("value", "'" + Escape(Utf8Encode(n->ch), '\'') + "'"); break;
            case NodeKind::StringLiteral:
            case NodeKind::SymbolLiteral:
            case NodeKind::MacroLiteral:
                f.scalar("value", Str(n->value));
                break;
            case NodeKind::StringInterpolation:
                list(f, "expressions", n->exps);
                f.scalar("heredoc_indent", std::to_string(n->heredoc_indent));
                break;
            case NodeKind::ArrayLiteral:
                list(f, "elements", n->exps);
                child(f, "of", n->of);
                child(f, "name", n->name_node);
                break;
            case NodeKind::HashLiteral: {
                for (size_t i = 0; i < n->entries.size(); i++) {
                    const std::string label = "entries[" + std::to_string(i) + "]";
                    const Node *k = n->entries[i].first;
                    const Node *v = n->entries[i].second;
                    f.children.push_back([this, label, k, v](int d) { entry(label, k, v, d); });
                }
                if (n->has_of) {
                    const Node *k = n->of_key;
                    const Node *v = n->of_value;
                    f.children.push_back([this, k, v](int d) { entry("of", k, v, d); });
                }
                child(f, "name", n->name_node);
                break;
            }
            case NodeKind::NamedTupleLiteral:
                for (size_t i = 0; i < n->named_entries.size(); i++) {
                    const std::string label = "entries[" + std::to_string(i) + "]";
                    const std::string key = n->named_entries[i].first;
                    const Node *v = n->named_entries[i].second;
                    f.children.push_back([this, label, key, v](int d) {
                        out += std::string(static_cast<size_t>(d) * 2, ' ') + label +
                               ": NamedTupleLiteral::Entry key=" + Str(key) + "\n";
                        node(v, d + 1, "value");
                    });
                }
                break;
            case NodeKind::RangeLiteral:
                child(f, "from", n->left);
                child(f, "to", n->right);
                f.scalar("exclusive", B(n->exclusive));
                break;
            case NodeKind::RegexLiteral:
                child(f, "value", n->value_node);
                f.scalar("options", std::to_string(n->regex_options));
                break;
            case NodeKind::TupleLiteral: list(f, "elements", n->exps); break;
            case NodeKind::Var: case NodeKind::InstanceVar: case NodeKind::ClassVar: case NodeKind::Global:
                f.scalar("name", Str(n->name));
                break;
            case NodeKind::Block:
                list(f, "args", n->exps);
                child(f, "body", n->body);
                if (n->splat_index >= 0) f.scalar("splat_index", std::to_string(n->splat_index));
                for (const auto &kv : n->unpacks) {
                    const std::string label = "unpacks{" + std::to_string(kv.first) + "}";
                    const Node *c = kv.second;
                    f.children.push_back([this, label, c](int d) { node(c, d, label); });
                }
                break;
            case NodeKind::Call:
                child(f, "obj", n->obj);
                f.scalar("name", Str(n->name));
                list(f, "args", n->args);
                child(f, "block", n->block);
                child(f, "block_arg", n->block_arg);
                list(f, "named_args", n->named_args);
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                f.scalar("global", B(n->global));
                f.scalar("name_size", std::to_string(n->name_size_));
                f.scalar("visibility", VisName(n->visibility));
                f.scalar("args_in_brackets", B(n->args_in_brackets));
                f.scalar("has_parentheses", B(n->has_parentheses));
                break;
            case NodeKind::NamedArgument:
                f.scalar("name", Str(n->name));
                child(f, "value", n->value_node);
                break;
            case NodeKind::If:
                child(f, "cond", n->cond);
                child(f, "then", n->then_);
                child(f, "else", n->else_);
                f.scalar("ternary", B(n->ternary));
                if (n->else_location.known()) f.scalar("else_location", LocS(n->else_location));
                break;
            case NodeKind::Unless:
                child(f, "cond", n->cond);
                child(f, "then", n->then_);
                child(f, "else", n->else_);
                if (n->else_location.known()) f.scalar("else_location", LocS(n->else_location));
                break;
            case NodeKind::Assign:
                child(f, "target", n->target);
                child(f, "value", n->value_node);
                break;
            case NodeKind::OpAssign:
                child(f, "target", n->target);
                f.scalar("op", Str(n->value));
                child(f, "value", n->value_node);
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                break;
            case NodeKind::MultiAssign:
                list(f, "targets", n->exps);
                list(f, "values", n->exps2);
                break;
            case NodeKind::ReadInstanceVar:
                child(f, "obj", n->obj);
                f.scalar("name", Str(n->name));
                break;
            case NodeKind::And:
            case NodeKind::Or:
                child(f, "left", n->left);
                child(f, "right", n->right);
                break;
            case NodeKind::Arg:
                f.scalar("name", Str(n->name));
                f.scalar("external_name", Str(n->external_name));
                child(f, "default_value", n->default_value);
                child(f, "restriction", n->restriction);
                list(f, "parsed_annotations", n->annotations);
                break;
            case NodeKind::ProcNotation:
                list(f, "inputs", n->exps);
                child(f, "output", n->right);
                break;
            case NodeKind::Def:
                if (n->has_names) f.scalar("free_vars", StrArray(n->names));
                child(f, "receiver", n->receiver);
                f.scalar("name", Str(n->name));
                list(f, "args", n->args);
                child(f, "double_splat", n->double_splat);
                child(f, "body", n->body);
                child(f, "block_arg", n->block_arg);
                child(f, "return_type", n->return_type);
                if (n->block_arity >= 0) f.scalar("block_arity", std::to_string(n->block_arity));
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                if (n->splat_index >= 0) f.scalar("splat_index", std::to_string(n->splat_index));
                f.scalar("abstract", B(n->is_abstract));
                f.scalar("visibility", VisName(n->visibility));
                f.scalar("calls_super", B(n->calls_super));
                f.scalar("calls_initialize", B(n->calls_initialize));
                f.scalar("calls_previous_def", B(n->calls_previous_def));
                f.scalar("uses_block_arg", B(n->uses_block_arg));
                f.scalar("assigns_special_var", B(n->assigns_special_var));
                break;
            case NodeKind::Macro:
                f.scalar("name", Str(n->name));
                list(f, "args", n->args);
                child(f, "body", n->body);
                child(f, "double_splat", n->double_splat);
                child(f, "block_arg", n->block_arg);
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                if (n->splat_index >= 0) f.scalar("splat_index", std::to_string(n->splat_index));
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::Not: case NodeKind::PointerOf: case NodeKind::SizeOf: case NodeKind::InstanceSizeOf:
            case NodeKind::AlignOf: case NodeKind::InstanceAlignOf: case NodeKind::Out: case NodeKind::MacroVerbatim:
            case NodeKind::Splat: case NodeKind::DoubleSplat: case NodeKind::Return: case NodeKind::Break:
            case NodeKind::Next:
                child(f, "exp", n->exp);
                break;
            case NodeKind::OffsetOf:
                child(f, "offsetof_type", n->left);
                child(f, "offset", n->right);
                break;
            case NodeKind::VisibilityModifier:
                f.scalar("modifier", VisName(n->modifier));
                child(f, "exp", n->exp);
                break;
            case NodeKind::IsA:
                child(f, "obj", n->obj);
                child(f, "const", n->right);
                f.scalar("nil_check", B(n->nil_check));
                break;
            case NodeKind::RespondsTo:
                child(f, "obj", n->obj);
                f.scalar("name", Str(n->name));
                break;
            case NodeKind::Require: f.scalar("string", Str(n->value)); break;
            case NodeKind::When:
                list(f, "conds", n->exps);
                child(f, "body", n->body);
                f.scalar("exhaustive", B(n->exhaustive));
                break;
            case NodeKind::Case:
                child(f, "cond", n->cond);
                list(f, "whens", n->exps);
                child(f, "else", n->else_);
                f.scalar("exhaustive", B(n->exhaustive));
                break;
            case NodeKind::Select:
                list(f, "whens", n->exps);
                child(f, "else", n->else_);
                break;
            case NodeKind::Path:
                f.scalar("names", StrArray(n->names));
                f.scalar("global", B(n->global));
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::ClassDef:
                child(f, "name", n->name_node);
                child(f, "body", n->body);
                child(f, "superclass", n->superclass);
                if (n->has_names) f.scalar("type_vars", StrArray(n->names));
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                if (n->splat_index >= 0) f.scalar("splat_index", std::to_string(n->splat_index));
                f.scalar("abstract", B(n->is_abstract));
                f.scalar("struct", B(n->is_struct));
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::ModuleDef:
                child(f, "name", n->name_node);
                child(f, "body", n->body);
                if (n->has_names) f.scalar("type_vars", StrArray(n->names));
                if (n->splat_index >= 0) f.scalar("splat_index", std::to_string(n->splat_index));
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::AnnotationDef:
                child(f, "name", n->name_node);
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                break;
            case NodeKind::While:
            case NodeKind::Until:
                child(f, "cond", n->cond);
                child(f, "body", n->body);
                break;
            case NodeKind::Generic:
                child(f, "name", n->name_node);
                list(f, "type_vars", n->exps);
                list(f, "named_args", n->named_args);
                f.scalar("suffix", n->suffix == GenericSuffix::None       ? "None"
                                   : n->suffix == GenericSuffix::Question ? "Question"
                                   : n->suffix == GenericSuffix::Asterisk ? "Asterisk"
                                                                          : "Bracket");
                break;
            case NodeKind::TypeDeclaration:
                child(f, "var", n->var);
                child(f, "declared_type", n->declared_type);
                child(f, "value", n->value_node);
                break;
            case NodeKind::UninitializedVar:
                child(f, "var", n->var);
                child(f, "declared_type", n->declared_type);
                break;
            case NodeKind::Rescue:
                child(f, "body", n->body);
                list(f, "types", n->exps);
                if (n->has_name) f.scalar("name", Str(n->name));
                break;
            case NodeKind::ExceptionHandler:
                child(f, "body", n->body);
                list(f, "rescues", n->exps);
                child(f, "else", n->else_);
                child(f, "ensure", n->ensure_);
                if (n->else_location.known()) f.scalar("else_location", LocS(n->else_location));
                if (n->ensure_location.known()) f.scalar("ensure_location", LocS(n->ensure_location));
                f.scalar("implicit", B(n->implicit));
                f.scalar("suffix", B(n->suffix_));
                break;
            case NodeKind::ProcLiteral: child(f, "def", n->def_); break;
            case NodeKind::ProcPointer:
                child(f, "obj", n->obj);
                f.scalar("name", Str(n->name));
                list(f, "args", n->args);
                f.scalar("global", B(n->global));
                break;
            case NodeKind::Union: list(f, "types", n->exps); break;
            case NodeKind::Yield:
                list(f, "exps", n->exps);
                child(f, "scope", n->scope);
                f.scalar("has_parentheses", B(n->has_parentheses));
                break;
            case NodeKind::Include: case NodeKind::Extend: case NodeKind::Metaclass:
                child(f, "name", n->name_node);
                break;
            case NodeKind::LibDef:
                child(f, "name", n->name_node);
                child(f, "body", n->body);
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::FunDef:
                f.scalar("name", Str(n->name));
                list(f, "args", n->args);
                child(f, "return_type", n->return_type);
                child(f, "body", n->body);
                f.scalar("real_name", Str(n->value));
                f.scalar("varargs", B(n->varargs));
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                break;
            case NodeKind::TypeDef:
                f.scalar("name", Str(n->name));
                child(f, "type_spec", n->restriction);
                if (n->name_location.known()) f.scalar("name_location", LocS(n->name_location));
                break;
            case NodeKind::CStructOrUnionDef:
                f.scalar("name", Str(n->name));
                child(f, "body", n->body);
                f.scalar("union", B(n->is_union));
                break;
            case NodeKind::EnumDef:
                child(f, "name", n->name_node);
                list(f, "members", n->exps);
                child(f, "base_type", n->restriction);
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::ExternalVar:
                f.scalar("name", Str(n->name));
                child(f, "type_spec", n->restriction);
                if (n->has_real_name) f.scalar("real_name", Str(n->value));
                break;
            case NodeKind::Alias:
                child(f, "name", n->name_node);
                child(f, "value", n->value_node);
                f.scalar("visibility", VisName(n->visibility));
                break;
            case NodeKind::Cast:
            case NodeKind::NilableCast:
                child(f, "obj", n->obj);
                child(f, "to", n->right);
                break;
            case NodeKind::TypeOf: list(f, "expressions", n->exps); break;
            case NodeKind::Annotation:
                child(f, "path", n->name_node);
                list(f, "args", n->args);
                list(f, "named_args", n->named_args);
                break;
            case NodeKind::MacroExpression:
                child(f, "exp", n->exp);
                f.scalar("output", B(n->output));
                break;
            case NodeKind::MacroIf:
                child(f, "cond", n->cond);
                child(f, "then", n->then_);
                child(f, "else", n->else_);
                f.scalar("is_unless", B(n->is_unless));
                break;
            case NodeKind::MacroFor:
                list(f, "vars", n->exps);
                child(f, "exp", n->exp);
                child(f, "body", n->body);
                break;
            case NodeKind::MacroVar:
                f.scalar("name", Str(n->name));
                list(f, "exps", n->exps);
                break;
            case NodeKind::MagicConstant: f.scalar("name", TokName(n->magic)); break;
            case NodeKind::Asm:
                f.scalar("text", Str(n->value));
                list(f, "outputs", n->exps);
                list(f, "inputs", n->exps2);
                if (n->has_names) f.scalar("clobbers", StrArray(n->names));
                f.scalar("volatile", B(n->volatile_));
                f.scalar("alignstack", B(n->alignstack));
                f.scalar("intel", B(n->intel));
                f.scalar("can_throw", B(n->can_throw));
                break;
            case NodeKind::AsmOperand:
                f.scalar("constraint", Str(n->value));
                child(f, "exp", n->exp);
                break;
        }
    }

    void entry(const std::string &label, const Node *k, const Node *v, int d) {
        out += std::string(static_cast<size_t>(d) * 2, ' ') + label + ": HashLiteral::Entry\n";
        node(k, d + 1, "key");
        node(v, d + 1, "value");
    }
};

}  // namespace

std::string DumpAst(const Node *root) {
    Dumper d;
    d.node(root, 0, "");
    return d.out;
}

}  // namespace crystal
