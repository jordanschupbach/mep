#include "js_engine.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

// A hand-rolled tokenizer + recursive-descent/precedence-climbing parser +
// tree-walking interpreter for a *subset* of JS -- not spec-compliant, not
// aiming to run real-world scripts, only small hand-written ones against a
// page's DOM (see js_engine.h's own header for the exact rationale/use
// case this was scoped to).
//
// Supported: var/let/const (all three are plain mutable bindings in the
// current function/global scope -- no let/const block-scoping or temporal-
// dead-zone semantics, since nothing this is meant to run depends on that
// distinction); function declarations/expressions and arrow functions,
// all with real closures; if/else, while, for(;;), return, break,
// continue, blocks; number/string/boolean/null/undefined/array/object
// literals; template literals (`...${expr}...`); +-*/%, comparisons
// (== and != behave like === and !== -- no ToPrimitive/type-coercion
// ladder, since every value this engine's own DOM bindings hand back is
// already a definite type), && || ! (short-circuiting), unary -/+/typeof,
// = and compound assignment, ?:, member/index access, calls.
//
// Not supported at all: `this`, `new`, prototypes/classes, generators/
// async, destructuring, spread/rest, labeled statements, getters/setters
// on plain objects (only the DOM bindings below have magic properties),
// regex literals, try/catch (a script that would use it just throws
// itself instead -- see RunScripts's own per-script catch-all).
//
// DOM binding surface -- deliberately this small, see js_engine.h:
// document.getElementById(id), document.title (get/set), a DOM-wrapping
// object's .textContent (get/set), console.log(...args), and a bare
// `window` object scripts can assign arbitrary properties onto (no BOM
// methods -- setTimeout/location/etc. aren't implemented) so the common
// "window.Foo = {...}" config-stashing pattern doesn't throw
// ReferenceError.

namespace {

// ---------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------

enum class VType { Undefined, Null, Number, String, Boolean, Object };

struct ObjectData;
using ObjectPtr = std::shared_ptr<ObjectData>;
struct Environment;
using EnvPtr = std::shared_ptr<Environment>;
struct Node;

struct Value {
    VType type = VType::Undefined;
    double num = 0;
    std::string str;
    bool boolean = false;
    ObjectPtr obj;

    static Value Undef() { return Value{}; }
    static Value MakeNull() {
        Value v;
        v.type = VType::Null;
        return v;
    }
    static Value Num(double d) {
        Value v;
        v.type = VType::Number;
        v.num = d;
        return v;
    }
    static Value Str(std::string s) {
        Value v;
        v.type = VType::String;
        v.str = std::move(s);
        return v;
    }
    static Value Bool(bool b) {
        Value v;
        v.type = VType::Boolean;
        v.boolean = b;
        return v;
    }
    static Value Obj(ObjectPtr o) {
        Value v;
        v.type = VType::Object;
        v.obj = std::move(o);
        return v;
    }

    bool Truthy() const {
        switch (type) {
            case VType::Undefined:
            case VType::Null:
                return false;
            case VType::Boolean:
                return boolean;
            case VType::Number:
                return num != 0 && !std::isnan(num);
            case VType::String:
                return !str.empty();
            case VType::Object:
                return true;
        }
        return false;
    }
};

// A single "kind" field serves plain objects, arrays (numeric string keys
// in `props`, plus a maintained "length"), user-defined functions/arrow
// functions (`fn_node`+`closure`), native/builtin functions (`native`),
// and DOM element wrappers (`dom_node`) -- a real engine would split these
// into a class hierarchy; one struct is simpler here since nothing but
// property get/set (below) ever needs to branch on which kind it is.
using NativeFn = std::function<Value(std::vector<Value> &, bool &threw, std::string &err)>;

struct ObjectData {
    std::unordered_map<std::string, Value> props;
    bool is_array = false;
    bool is_function = false;
    bool is_document = false;

    const Node *fn_node = nullptr;  // function/arrow AST node (params+body); owned by the Program this ran from, so this stays valid for RunScripts's own duration
    EnvPtr closure;

    NativeFn native;

    DomNode *dom_node = nullptr;
    HtmlDoc *owner_doc = nullptr;  // only set on the `document` object, for .title
};

struct Environment {
    std::unordered_map<std::string, Value> vars;
    EnvPtr parent;

    Value *Find(const std::string &name) {
        for (Environment *e = this; e != nullptr; e = e->parent.get()) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
    void Define(const std::string &name, Value v) { vars[name] = std::move(v); }
};

std::string NumberToString(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return std::signbit(d) ? "0" : "0";
    if (d == static_cast<double>(static_cast<long long>(d)) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
        return buf;
    }
    std::ostringstream oss;
    oss.precision(15);
    oss << d;
    return oss.str();
}

long ArrayLength(const ObjectPtr &obj) {
    auto it = obj->props.find("length");
    if (it == obj->props.end()) return 0;
    return static_cast<long>(it->second.num);
}

std::string ToDisplayString(const Value &v);

std::string JoinArrayForDisplay(const ObjectPtr &obj) {
    std::string out;
    long len = ArrayLength(obj);
    for (long i = 0; i < len; i++) {
        if (i) out += ",";
        auto it = obj->props.find(std::to_string(i));
        if (it != obj->props.end()) out += ToDisplayString(it->second);
    }
    return out;
}

std::string ToDisplayString(const Value &v) {
    switch (v.type) {
        case VType::Undefined:
            return "undefined";
        case VType::Null:
            return "null";
        case VType::Boolean:
            return v.boolean ? "true" : "false";
        case VType::Number:
            return NumberToString(v.num);
        case VType::String:
            return v.str;
        case VType::Object:
            if (!v.obj) return "null";
            if (v.obj->dom_node) return "[object HTMLElement]";
            if (v.obj->is_array) return JoinArrayForDisplay(v.obj);
            if (v.obj->is_function) return "function";
            return "[object Object]";
    }
    return "";
}

double ToNumber(const Value &v) {
    switch (v.type) {
        case VType::Number:
            return v.num;
        case VType::Boolean:
            return v.boolean ? 1 : 0;
        case VType::Null:
            return 0;
        case VType::Undefined:
            return std::nan("");
        case VType::String: {
            if (v.str.empty()) return 0;
            const char *s = v.str.c_str();
            char *end = nullptr;
            double d = std::strtod(s, &end);
            if (end == s) return std::nan("");
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
            return (*end == '\0') ? d : std::nan("");
        }
        case VType::Object:
            return std::nan("");
    }
    return std::nan("");
}

bool StrictEquals(const Value &a, const Value &b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case VType::Undefined:
        case VType::Null:
            return true;
        case VType::Boolean:
            return a.boolean == b.boolean;
        case VType::Number:
            return a.num == b.num;
        case VType::String:
            return a.str == b.str;
        case VType::Object:
            return a.obj == b.obj;
    }
    return false;
}

std::string GetTextContent(const DomNode *n) {
    std::string out;
    for (const auto &c : n->children) {
        if (c->type == DomNodeType::Text) {
            out += c->text;
        } else {
            out += GetTextContent(c.get());
        }
    }
    return out;
}

// Replaces every child with a single Text node -- matches real DOM's own
// `el.textContent = x` (any existing children, element or text, are gone),
// not an append. Deliberately doesn't call ComputeStyles: a Text node's
// own style is never consulted by anything (js_engine.h's own comment) and
// this doesn't touch the element's tag/attrs/position, so the element's
// *own* already-computed style stays correct.
void SetTextContent(DomNode *n, const std::string &text) {
    n->children.clear();
    auto t = std::make_unique<DomNode>();
    t->type = DomNodeType::Text;
    t->text = text;
    t->parent = n;
    n->children.push_back(std::move(t));
}

DomNode *FindById(DomNode *n, const std::string &id) {
    if (n->type == DomNodeType::Element && n->Id() == id) return n;
    for (auto &c : n->children) {
        if (DomNode *found = FindById(c.get(), id)) return found;
    }
    return nullptr;
}

bool IsArrayIndexKey(const std::string &key, long &idx) {
    if (key.empty()) return false;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
    }
    idx = std::strtol(key.c_str(), nullptr, 10);
    return true;
}

Value GetProp(const ObjectPtr &obj, const std::string &key) {
    if (!obj) return Value::Undef();
    if (obj->dom_node && key == "textContent") return Value::Str(GetTextContent(obj->dom_node));
    if (obj->is_document && key == "title") return Value::Str(obj->owner_doc ? obj->owner_doc->title : "");
    auto it = obj->props.find(key);
    if (it != obj->props.end()) return it->second;
    return Value::Undef();
}

void SetProp(const ObjectPtr &obj, const std::string &key, Value val) {
    if (!obj) return;
    if (obj->dom_node && key == "textContent") {
        SetTextContent(obj->dom_node, ToDisplayString(val));
        return;
    }
    if (obj->is_document && key == "title") {
        if (obj->owner_doc) obj->owner_doc->title = ToDisplayString(val);
        return;
    }
    long idx;
    if (obj->is_array && IsArrayIndexKey(key, idx)) {
        obj->props[key] = val;
        long cur_len = ArrayLength(obj);
        if (idx + 1 > cur_len) obj->props["length"] = Value::Num(static_cast<double>(idx + 1));
        return;
    }
    obj->props[key] = std::move(val);
}

// ---------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------

enum class Tok {
    End,
    Num,
    Str,
    TemplateStr,
    Ident,
    KwVar,
    KwLet,
    KwConst,
    KwFunction,
    KwReturn,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwTrue,
    KwFalse,
    KwNull,
    KwUndefined,
    KwBreak,
    KwContinue,
    KwTypeof,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Semicolon,
    Comma,
    Dot,
    Colon,
    Question,
    Arrow,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    Assign,
    EqEq,
    EqEqEq,
    NotEq,
    NotEqEq,
    Lt,
    Gt,
    LtEq,
    GtEq,
    AndAnd,
    OrOr,
    Bang,
    PlusPlus,
    MinusMinus,
};

struct Token {
    Tok type = Tok::End;
    std::string text;  // identifier name, string literal's decoded value, or raw text for TemplateStr (re-scanned by the parser -- see ParseTemplateLiteral)
    double num = 0;
    int pos = 0;  // byte offset in source, for error messages
};

struct Lexer {
    std::string src;
    size_t i = 0;

    explicit Lexer(std::string s) : src(std::move(s)) {}

    void SkipTrivia() {
        for (;;) {
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) i++;
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '/') {
                while (i < src.size() && src[i] != '\n') i++;
                continue;
            }
            if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) i++;
                i = std::min(src.size(), i + 2);
                continue;
            }
            break;
        }
    }

    std::string ReadQuoted(char quote) {
        std::string out;
        i++;  // opening quote
        while (i < src.size() && src[i] != quote) {
            if (src[i] == '\\' && i + 1 < src.size()) {
                char c = src[i + 1];
                switch (c) {
                    case 'n':
                        out += '\n';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '\'':
                        out += '\'';
                        break;
                    case '"':
                        out += '"';
                        break;
                    case '`':
                        out += '`';
                        break;
                    default:
                        out += c;
                }
                i += 2;
            } else {
                out += src[i++];
            }
        }
        if (i < src.size()) i++;  // closing quote
        return out;
    }

    Token Next() {
        SkipTrivia();
        Token t;
        t.pos = static_cast<int>(i);
        if (i >= src.size()) {
            t.type = Tok::End;
            return t;
        }
        char c = src[i];
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t start = i;
            while (i < src.size() && (std::isdigit(static_cast<unsigned char>(src[i])) || src[i] == '.')) i++;
            if (i < src.size() && (src[i] == 'e' || src[i] == 'E')) {
                i++;
                if (i < src.size() && (src[i] == '+' || src[i] == '-')) i++;
                while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i]))) i++;
            }
            t.type = Tok::Num;
            t.num = std::strtod(src.substr(start, i - start).c_str(), nullptr);
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            size_t start = i;
            while (i < src.size() && (std::isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_' || src[i] == '$')) i++;
            std::string word = src.substr(start, i - start);
            static const std::unordered_map<std::string, Tok> kKeywords = {
                {"var", Tok::KwVar},       {"let", Tok::KwLet},         {"const", Tok::KwConst},
                {"function", Tok::KwFunction}, {"return", Tok::KwReturn}, {"if", Tok::KwIf},
                {"else", Tok::KwElse},     {"while", Tok::KwWhile},     {"for", Tok::KwFor},
                {"true", Tok::KwTrue},     {"false", Tok::KwFalse},     {"null", Tok::KwNull},
                {"undefined", Tok::KwUndefined}, {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},
                {"typeof", Tok::KwTypeof},
            };
            auto it = kKeywords.find(word);
            t.type = it != kKeywords.end() ? it->second : Tok::Ident;
            t.text = word;
            return t;
        }
        if (c == '"' || c == '\'') {
            t.type = Tok::Str;
            t.text = ReadQuoted(c);
            return t;
        }
        if (c == '`') {
            // Handed to the parser raw (with escapes still literal) -- it
            // re-scans this for ${...} interpolation boundaries itself
            // (ParseTemplateLiteral), since splitting that here would mean
            // the lexer producing a *sequence* of tokens for one backtick
            // literal, which doesn't fit this single-Token-per-Next() shape.
            size_t start = i + 1;
            i++;
            int depth = 0;
            while (i < src.size() && !(src[i] == '`' && depth == 0)) {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    i += 2;
                    continue;
                }
                if (src[i] == '$' && i + 1 < src.size() && src[i + 1] == '{') depth++;
                if (src[i] == '}' && depth > 0) depth--;
                i++;
            }
            t.type = Tok::TemplateStr;
            t.text = src.substr(start, i - start);
            if (i < src.size()) i++;  // closing backtick
            return t;
        }
        auto two = [&](char c2, Tok two_tok, Tok one_tok) {
            if (i + 1 < src.size() && src[i + 1] == c2) {
                i += 2;
                t.type = two_tok;
            } else {
                i += 1;
                t.type = one_tok;
            }
        };
        switch (c) {
            case '(':
                i++;
                t.type = Tok::LParen;
                return t;
            case ')':
                i++;
                t.type = Tok::RParen;
                return t;
            case '{':
                i++;
                t.type = Tok::LBrace;
                return t;
            case '}':
                i++;
                t.type = Tok::RBrace;
                return t;
            case '[':
                i++;
                t.type = Tok::LBracket;
                return t;
            case ']':
                i++;
                t.type = Tok::RBracket;
                return t;
            case ';':
                i++;
                t.type = Tok::Semicolon;
                return t;
            case ',':
                i++;
                t.type = Tok::Comma;
                return t;
            case '.':
                i++;
                t.type = Tok::Dot;
                return t;
            case ':':
                i++;
                t.type = Tok::Colon;
                return t;
            case '?':
                i++;
                t.type = Tok::Question;
                return t;
            case '+':
                if (i + 1 < src.size() && src[i + 1] == '+') {
                    i += 2;
                    t.type = Tok::PlusPlus;
                    return t;
                }
                two('=', Tok::PlusEq, Tok::Plus);
                return t;
            case '-':
                if (i + 1 < src.size() && src[i + 1] == '-') {
                    i += 2;
                    t.type = Tok::MinusMinus;
                    return t;
                }
                two('=', Tok::MinusEq, Tok::Minus);
                return t;
            case '*':
                two('=', Tok::StarEq, Tok::Star);
                return t;
            case '%':
                i++;
                t.type = Tok::Percent;
                return t;
            case '/':
                two('=', Tok::SlashEq, Tok::Slash);
                return t;
            case '<':
                two('=', Tok::LtEq, Tok::Lt);
                return t;
            case '>':
                two('=', Tok::GtEq, Tok::Gt);
                return t;
            case '&':
                if (i + 1 < src.size() && src[i + 1] == '&') {
                    i += 2;
                    t.type = Tok::AndAnd;
                    return t;
                }
                i++;
                t.type = Tok::End;  // unsupported bitwise &, treat as end-of-useful-input rather than mis-lex
                return t;
            case '|':
                if (i + 1 < src.size() && src[i + 1] == '|') {
                    i += 2;
                    t.type = Tok::OrOr;
                    return t;
                }
                i++;
                t.type = Tok::End;
                return t;
            case '=':
                if (i + 2 < src.size() && src[i + 1] == '=' && src[i + 2] == '=') {
                    i += 3;
                    t.type = Tok::EqEqEq;
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '=') {
                    i += 2;
                    t.type = Tok::EqEq;
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '>') {
                    i += 2;
                    t.type = Tok::Arrow;
                    return t;
                }
                i++;
                t.type = Tok::Assign;
                return t;
            case '!':
                if (i + 2 < src.size() && src[i + 1] == '=' && src[i + 2] == '=') {
                    i += 3;
                    t.type = Tok::NotEqEq;
                    return t;
                }
                if (i + 1 < src.size() && src[i + 1] == '=') {
                    i += 2;
                    t.type = Tok::NotEq;
                    return t;
                }
                i++;
                t.type = Tok::Bang;
                return t;
            default:
                i++;
                t.type = Tok::End;  // unrecognized byte -- surfaces as an unexpected-end parse error rather than looping
                return t;
        }
    }
};

// ---------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------

enum class NodeKind {
    NumberLit, StringLit, BoolLit, NullLit, UndefinedLit, TemplateLit, ArrayLit, ObjectLit,
    Ident, Unary, Update, Binary, Logical, Assign, Member, Call, Conditional, FunctionExpr,
    ExprStmt, VarDecl, Block, If, While, For, Return, Break, Continue, FunctionDecl, Program,
};

struct Node {
    NodeKind kind;
    // Literals
    double num = 0;
    std::string str;
    bool boolean = false;  // BoolLit's value, or (reused) Update's prefix-vs-postfix flag -- true means prefix
    // TemplateLit: alternating literal-text parts (is_expr_part[k]==false) and expr parts (true)
    std::vector<bool> is_expr_part;
    std::vector<std::string> template_texts;
    std::vector<std::unique_ptr<Node>> template_exprs;
    // ArrayLit
    std::vector<std::unique_ptr<Node>> elements;
    // ObjectLit
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> obj_props;
    // Ident
    std::string name;
    // Unary/Binary/Logical/Assign
    std::string op;
    std::unique_ptr<Node> a, b, c;  // generic operand slots (unary: a; binary/logical/assign: a,b; conditional: a=cond,b=then,c=else)
    // Member
    bool computed = false;  // obj[expr] vs obj.prop
    std::string prop_name;
    // Call
    std::vector<std::unique_ptr<Node>> args;
    // FunctionExpr / FunctionDecl
    std::vector<std::string> params;
    std::vector<std::unique_ptr<Node>> body;  // Block's statement list, or a single implicit-return expr for a concise arrow body (see arrow_expr_body)
    bool arrow_expr_body = false;
    // VarDecl
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> declarators;
    // If/While/For/Block share a/b/c/body loosely; kept explicit per-kind below for clarity at eval time
    std::unique_ptr<Node> init, cond, update, then_branch, else_branch;

    explicit Node(NodeKind k) : kind(k) {}
};

using NodePtr = std::unique_ptr<Node>;

// ---------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------

struct ParseError {
    std::string message;
};

struct Parser {
    Lexer lex;
    Token cur;
    bool ok = true;
    std::string error;

    explicit Parser(std::string src) : lex(std::move(src)) { cur = lex.Next(); }

    void Advance() { cur = lex.Next(); }

    void Fail(const std::string &msg) {
        if (ok) {
            ok = false;
            error = msg;
        }
    }

    bool Check(Tok t) const { return cur.type == t; }

    bool Match(Tok t) {
        if (cur.type == t) {
            Advance();
            return true;
        }
        return false;
    }

    void Expect(Tok t, const char *what) {
        if (!Match(t)) Fail(std::string("expected ") + what);
    }

    NodePtr ParseProgram() {
        auto prog = std::make_unique<Node>(NodeKind::Program);
        while (ok && !Check(Tok::End)) {
            prog->body.push_back(ParseStatement());
            if (!ok) break;
        }
        return prog;
    }

    NodePtr ParseBlock() {
        Expect(Tok::LBrace, "'{'");
        auto blk = std::make_unique<Node>(NodeKind::Block);
        while (ok && !Check(Tok::RBrace) && !Check(Tok::End)) {
            blk->body.push_back(ParseStatement());
        }
        Expect(Tok::RBrace, "'}'");
        return blk;
    }

    NodePtr ParseStatement() {
        if (!ok) return std::make_unique<Node>(NodeKind::Block);
        if (Check(Tok::LBrace)) return ParseBlock();
        if (Check(Tok::Semicolon)) {
            Advance();
            return std::make_unique<Node>(NodeKind::Block);  // empty statement, represented as an empty block
        }
        if (Check(Tok::KwVar) || Check(Tok::KwLet) || Check(Tok::KwConst)) return ParseVarDecl();
        if (Check(Tok::KwFunction)) return ParseFunctionDecl();
        if (Check(Tok::KwIf)) return ParseIf();
        if (Check(Tok::KwWhile)) return ParseWhile();
        if (Check(Tok::KwFor)) return ParseFor();
        if (Check(Tok::KwReturn)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Return);
            if (!Check(Tok::Semicolon) && !Check(Tok::RBrace) && !Check(Tok::End)) n->a = ParseExpression();
            Match(Tok::Semicolon);
            return n;
        }
        if (Check(Tok::KwBreak)) {
            Advance();
            Match(Tok::Semicolon);
            return std::make_unique<Node>(NodeKind::Break);
        }
        if (Check(Tok::KwContinue)) {
            Advance();
            Match(Tok::Semicolon);
            return std::make_unique<Node>(NodeKind::Continue);
        }
        auto n = std::make_unique<Node>(NodeKind::ExprStmt);
        n->a = ParseExpression();
        Match(Tok::Semicolon);
        return n;
    }

    NodePtr ParseVarDecl() {
        Advance();  // var/let/const
        auto n = std::make_unique<Node>(NodeKind::VarDecl);
        for (;;) {
            if (!Check(Tok::Ident)) {
                Fail("expected identifier in declaration");
                break;
            }
            std::string name = cur.text;
            Advance();
            NodePtr init;
            if (Match(Tok::Assign)) init = ParseAssignExpr();
            n->declarators.emplace_back(name, std::move(init));
            if (!Match(Tok::Comma)) break;
        }
        Match(Tok::Semicolon);
        return n;
    }

    NodePtr ParseFunctionDecl() {
        Advance();  // function
        auto n = std::make_unique<Node>(NodeKind::FunctionDecl);
        if (Check(Tok::Ident)) {
            n->name = cur.text;
            Advance();
        } else {
            Fail("expected function name");
        }
        ParseParamsAndBody(*n);
        return n;
    }

    void ParseParamsAndBody(Node &n) {
        Expect(Tok::LParen, "'('");
        while (ok && !Check(Tok::RParen)) {
            if (!Check(Tok::Ident)) {
                Fail("expected parameter name");
                break;
            }
            n.params.push_back(cur.text);
            Advance();
            if (!Match(Tok::Comma)) break;
        }
        Expect(Tok::RParen, "')'");
        NodePtr blk = ParseBlock();
        n.body = std::move(blk->body);
    }

    NodePtr ParseIf() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::If);
        n->cond = ParseExpression();
        Expect(Tok::RParen, "')'");
        n->then_branch = ParseStatement();
        if (Match(Tok::KwElse)) n->else_branch = ParseStatement();
        return n;
    }

    NodePtr ParseWhile() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::While);
        n->cond = ParseExpression();
        Expect(Tok::RParen, "')'");
        n->then_branch = ParseStatement();
        return n;
    }

    NodePtr ParseFor() {
        Advance();
        Expect(Tok::LParen, "'('");
        auto n = std::make_unique<Node>(NodeKind::For);
        if (!Check(Tok::Semicolon)) {
            if (Check(Tok::KwVar) || Check(Tok::KwLet) || Check(Tok::KwConst)) {
                n->init = ParseVarDecl();  // consumes its own trailing ';'
            } else {
                auto es = std::make_unique<Node>(NodeKind::ExprStmt);
                es->a = ParseExpression();
                n->init = std::move(es);
                Expect(Tok::Semicolon, "';'");
            }
        } else {
            Advance();
        }
        if (!Check(Tok::Semicolon)) n->cond = ParseExpression();
        Expect(Tok::Semicolon, "';'");
        if (!Check(Tok::RParen)) n->update = ParseExpression();
        Expect(Tok::RParen, "')'");
        n->then_branch = ParseStatement();
        return n;
    }

    // ---- Expressions, lowest to highest precedence ----

    NodePtr ParseExpression() { return ParseAssignExpr(); }

    bool IsAssignOp(Tok t) const {
        return t == Tok::Assign || t == Tok::PlusEq || t == Tok::MinusEq || t == Tok::StarEq || t == Tok::SlashEq;
    }

    NodePtr ParseAssignExpr() {
        // Arrow-function lookahead: `ident => ...` or `(params) => ...`.
        // A single bare identifier is easy to detect with one token of
        // lookahead; a parenthesized param list needs a full speculative
        // parse (cheap here -- scripts this engine targets are tiny), so
        // this snapshots the lexer position and rewinds if it turns out
        // not to be an arrow after all.
        if (Check(Tok::Ident)) {
            std::string maybe_name = cur.text;
            size_t save_i = lex.i;
            Token save_cur = cur;
            Advance();
            if (Check(Tok::Arrow)) {
                Advance();
                auto fn = std::make_unique<Node>(NodeKind::FunctionExpr);
                fn->params.push_back(maybe_name);
                ParseArrowBody(*fn);
                return fn;
            }
            lex.i = save_i;
            cur = save_cur;
        } else if (Check(Tok::LParen)) {
            size_t save_i = lex.i;
            Token save_cur = cur;
            bool save_ok = ok;
            std::vector<std::string> params;
            bool looks_like_params = true;
            Advance();
            while (ok && !Check(Tok::RParen)) {
                if (!Check(Tok::Ident)) {
                    looks_like_params = false;
                    break;
                }
                params.push_back(cur.text);
                Advance();
                if (!Match(Tok::Comma)) break;
            }
            if (looks_like_params && Check(Tok::RParen)) {
                Advance();
                if (Check(Tok::Arrow)) {
                    Advance();
                    auto fn = std::make_unique<Node>(NodeKind::FunctionExpr);
                    fn->params = std::move(params);
                    ParseArrowBody(*fn);
                    return fn;
                }
            }
            lex.i = save_i;
            cur = save_cur;
            ok = save_ok;
            error.clear();
        }

        NodePtr left = ParseConditional();
        if (ok && IsAssignOp(cur.type)) {
            std::string op = cur.text;
            Tok t = cur.type;
            op = (t == Tok::Assign) ? "=" : (t == Tok::PlusEq) ? "+=" : (t == Tok::MinusEq) ? "-=" : (t == Tok::StarEq) ? "*=" : "/=";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Assign);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseAssignExpr();
            return n;
        }
        return left;
    }

    void ParseArrowBody(Node &fn) {
        if (Check(Tok::LBrace)) {
            NodePtr blk = ParseBlock();
            fn.body = std::move(blk->body);
        } else {
            fn.arrow_expr_body = true;
            fn.body.push_back(ParseAssignExpr());
        }
    }

    NodePtr ParseConditional() {
        NodePtr cond = ParseLogicalOr();
        if (Match(Tok::Question)) {
            auto n = std::make_unique<Node>(NodeKind::Conditional);
            n->a = std::move(cond);
            n->b = ParseAssignExpr();
            Expect(Tok::Colon, "':'");
            n->c = ParseAssignExpr();
            return n;
        }
        return cond;
    }

    NodePtr ParseLogicalOr() {
        NodePtr left = ParseLogicalAnd();
        while (Check(Tok::OrOr)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Logical);
            n->op = "||";
            n->a = std::move(left);
            n->b = ParseLogicalAnd();
            left = std::move(n);
        }
        return left;
    }

    NodePtr ParseLogicalAnd() {
        NodePtr left = ParseEquality();
        while (Check(Tok::AndAnd)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Logical);
            n->op = "&&";
            n->a = std::move(left);
            n->b = ParseEquality();
            left = std::move(n);
        }
        return left;
    }

    NodePtr ParseEquality() {
        NodePtr left = ParseRelational();
        for (;;) {
            std::string op;
            if (Check(Tok::EqEq) || Check(Tok::EqEqEq)) op = "==";
            else if (Check(Tok::NotEq) || Check(Tok::NotEqEq))
                op = "!=";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseRelational();
            left = std::move(n);
        }
        return left;
    }

    NodePtr ParseRelational() {
        NodePtr left = ParseAdditive();
        for (;;) {
            std::string op;
            if (Check(Tok::Lt)) op = "<";
            else if (Check(Tok::Gt))
                op = ">";
            else if (Check(Tok::LtEq))
                op = "<=";
            else if (Check(Tok::GtEq))
                op = ">=";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseAdditive();
            left = std::move(n);
        }
        return left;
    }

    NodePtr ParseAdditive() {
        NodePtr left = ParseMultiplicative();
        for (;;) {
            std::string op;
            if (Check(Tok::Plus)) op = "+";
            else if (Check(Tok::Minus))
                op = "-";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseMultiplicative();
            left = std::move(n);
        }
        return left;
    }

    NodePtr ParseMultiplicative() {
        NodePtr left = ParseUnary();
        for (;;) {
            std::string op;
            if (Check(Tok::Star)) op = "*";
            else if (Check(Tok::Slash))
                op = "/";
            else if (Check(Tok::Percent))
                op = "%";
            else
                break;
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Binary);
            n->op = op;
            n->a = std::move(left);
            n->b = ParseUnary();
            left = std::move(n);
        }
        return left;
    }

    NodePtr ParseUnary() {
        if (Check(Tok::Minus) || Check(Tok::Plus) || Check(Tok::Bang) || Check(Tok::KwTypeof)) {
            std::string op = Check(Tok::Minus) ? "-" : Check(Tok::Plus) ? "+" : Check(Tok::Bang) ? "!" : "typeof";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Unary);
            n->op = op;
            n->a = ParseUnary();
            return n;
        }
        if (Check(Tok::PlusPlus) || Check(Tok::MinusMinus)) {
            std::string op = Check(Tok::PlusPlus) ? "++" : "--";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Update);
            n->op = op;
            n->boolean = true;  // prefix
            n->a = ParseUnary();
            return n;
        }
        NodePtr expr = ParseCallOrMember();
        if (Check(Tok::PlusPlus) || Check(Tok::MinusMinus)) {
            std::string op = Check(Tok::PlusPlus) ? "++" : "--";
            Advance();
            auto n = std::make_unique<Node>(NodeKind::Update);
            n->op = op;
            n->boolean = false;  // postfix
            n->a = std::move(expr);
            return n;
        }
        return expr;
    }

    NodePtr ParseCallOrMember() {
        NodePtr expr = ParsePrimary();
        for (;;) {
            if (Match(Tok::Dot)) {
                if (!Check(Tok::Ident)) {
                    Fail("expected property name after '.'");
                    return expr;
                }
                auto n = std::make_unique<Node>(NodeKind::Member);
                n->a = std::move(expr);
                n->prop_name = cur.text;
                n->computed = false;
                Advance();
                expr = std::move(n);
            } else if (Match(Tok::LBracket)) {
                auto n = std::make_unique<Node>(NodeKind::Member);
                n->a = std::move(expr);
                n->b = ParseExpression();
                n->computed = true;
                Expect(Tok::RBracket, "']'");
                expr = std::move(n);
            } else if (Match(Tok::LParen)) {
                auto n = std::make_unique<Node>(NodeKind::Call);
                n->a = std::move(expr);
                while (ok && !Check(Tok::RParen)) {
                    n->args.push_back(ParseAssignExpr());
                    if (!Match(Tok::Comma)) break;
                }
                Expect(Tok::RParen, "')'");
                expr = std::move(n);
            } else {
                break;
            }
        }
        return expr;
    }

    NodePtr ParsePrimary() {
        if (Check(Tok::Num)) {
            auto n = std::make_unique<Node>(NodeKind::NumberLit);
            n->num = cur.num;
            Advance();
            return n;
        }
        if (Check(Tok::Str)) {
            auto n = std::make_unique<Node>(NodeKind::StringLit);
            n->str = cur.text;
            Advance();
            return n;
        }
        if (Check(Tok::TemplateStr)) return ParseTemplateLiteral();
        if (Check(Tok::KwTrue) || Check(Tok::KwFalse)) {
            auto n = std::make_unique<Node>(NodeKind::BoolLit);
            n->boolean = Check(Tok::KwTrue);
            Advance();
            return n;
        }
        if (Check(Tok::KwNull)) {
            Advance();
            return std::make_unique<Node>(NodeKind::NullLit);
        }
        if (Check(Tok::KwUndefined)) {
            Advance();
            return std::make_unique<Node>(NodeKind::UndefinedLit);
        }
        if (Check(Tok::Ident)) {
            auto n = std::make_unique<Node>(NodeKind::Ident);
            n->name = cur.text;
            Advance();
            return n;
        }
        if (Check(Tok::KwFunction)) {
            Advance();
            auto n = std::make_unique<Node>(NodeKind::FunctionExpr);
            if (Check(Tok::Ident)) {
                n->name = cur.text;
                Advance();
            }
            ParseParamsAndBody(*n);
            return n;
        }
        if (Match(Tok::LParen)) {
            NodePtr n = ParseExpression();
            Expect(Tok::RParen, "')'");
            return n;
        }
        if (Match(Tok::LBracket)) {
            auto n = std::make_unique<Node>(NodeKind::ArrayLit);
            while (ok && !Check(Tok::RBracket)) {
                n->elements.push_back(ParseAssignExpr());
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBracket, "']'");
            return n;
        }
        if (Match(Tok::LBrace)) {
            auto n = std::make_unique<Node>(NodeKind::ObjectLit);
            while (ok && !Check(Tok::RBrace)) {
                std::string key;
                if (Check(Tok::Ident) || Check(Tok::KwTrue) || Check(Tok::KwFalse) || Check(Tok::KwNull)) {
                    key = cur.text.empty() ? "" : cur.text;
                    Advance();
                } else if (Check(Tok::Str)) {
                    key = cur.text;
                    Advance();
                } else {
                    Fail("expected property key");
                    break;
                }
                Expect(Tok::Colon, "':'");
                n->obj_props.emplace_back(key, ParseAssignExpr());
                if (!Match(Tok::Comma)) break;
            }
            Expect(Tok::RBrace, "'}'");
            return n;
        }
        Fail("unexpected token in expression");
        return std::make_unique<Node>(NodeKind::UndefinedLit);
    }

    NodePtr ParseTemplateLiteral() {
        // The lexer already isolated the raw `...${...}...` body (with
        // ${...} nesting balanced); re-scanning it here with its own
        // recursive Parser instances is simpler than threading template
        // interpolation through the main token stream.
        std::string raw = cur.text;
        Advance();
        auto n = std::make_unique<Node>(NodeKind::TemplateLit);
        std::string lit;
        size_t i = 0;
        while (i < raw.size()) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char c = raw[i + 1];
                if (c == 'n') lit += '\n';
                else if (c == 't')
                    lit += '\t';
                else
                    lit += c;
                i += 2;
                continue;
            }
            if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                n->is_expr_part.push_back(false);
                n->template_texts.push_back(lit);
                lit.clear();
                size_t start = i + 2;
                int depth = 1;
                size_t j = start;
                while (j < raw.size() && depth > 0) {
                    if (raw[j] == '{') depth++;
                    else if (raw[j] == '}')
                        depth--;
                    if (depth > 0) j++;
                }
                std::string expr_src = raw.substr(start, j - start);
                Parser sub(expr_src);
                NodePtr expr = sub.ParseExpression();
                if (!sub.ok) Fail("template literal: " + sub.error);
                n->is_expr_part.push_back(true);
                n->template_exprs.push_back(std::move(expr));
                n->template_texts.push_back("");
                i = j + 1;
                continue;
            }
            lit += raw[i++];
        }
        n->is_expr_part.push_back(false);
        n->template_texts.push_back(lit);
        return n;
    }
};

// ---------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------

enum class CompletionType { Normal, Return, Break, Continue, Throw };

struct Completion {
    CompletionType type = CompletionType::Normal;
    Value value;

    static Completion Norm(Value v = Value::Undef()) { return {CompletionType::Normal, std::move(v)}; }
    static Completion Ret(Value v) { return {CompletionType::Return, std::move(v)}; }
    static Completion Brk() { return {CompletionType::Break, Value::Undef()}; }
    static Completion Cont() { return {CompletionType::Continue, Value::Undef()}; }
    static Completion Thr(const std::string &msg) { return {CompletionType::Throw, Value::Str(msg)}; }

    bool IsAbrupt() const { return type != CompletionType::Normal; }
};

// Bounds both "how many statements/loop-iterations/calls has this one
// script run" (an infinite `while(true){}` or unbounded recursion would
// otherwise hang mep -- it's single-threaded and synchronous, see
// js_engine.h) and native call-stack depth independently, since the two
// fail differently: a tight loop with no recursion never grows the C++
// stack at all, while deep recursion can blow it long before the step
// count above gets anywhere near its own limit. Both bounds are generous
// (a legitimate small script won't come close) but finite.
constexpr long kMaxSteps = 4'000'000;
constexpr int kMaxCallDepth = 300;

struct Interpreter {
    long steps = 0;
    int call_depth = 0;
    EnvPtr global;

    bool StepGuard(Completion &out) {
        if (++steps > kMaxSteps) {
            out = Completion::Thr("script exceeded step limit (possible infinite loop)");
            return true;
        }
        return false;
    }
};

Completion EvalExpr(Interpreter &interp, const Node &n, EnvPtr &env);
Completion ExecStmt(Interpreter &interp, const Node &n, EnvPtr &env);

Completion ExecBlockBody(Interpreter &interp, const std::vector<NodePtr> &stmts, EnvPtr &env) {
    // One-pass function hoisting: a direct-child `function foo(){}` in this
    // block is bound before any statement runs, so sibling statements
    // (including ones textually *before* it) can call it -- matches the
    // common "helper defined lower in the same script" pattern real JS
    // hoisting also allows, without implementing full hoisting semantics
    // for var declarations too (those stay bound at their own statement,
    // per this file's own header comment on var/let/const).
    for (const auto &s : stmts) {
        if (s->kind == NodeKind::FunctionDecl) {
            auto obj = std::make_shared<ObjectData>();
            obj->is_function = true;
            obj->fn_node = s.get();
            obj->closure = env;
            env->Define(s->name, Value::Obj(obj));
        }
    }
    for (const auto &s : stmts) {
        Completion c;
        if (interp.StepGuard(c)) return c;
        c = ExecStmt(interp, *s, env);
        if (c.IsAbrupt()) return c;
    }
    return Completion::Norm();
}

Completion CallFunction(Interpreter &interp, const ObjectPtr &fn, std::vector<Value> &args) {
    if (fn->native) {
        bool threw = false;
        std::string err;
        Value v = fn->native(args, threw, err);
        return threw ? Completion::Thr(err) : Completion::Norm(v);
    }
    if (!fn->fn_node) return Completion::Thr("value is not callable");
    if (++interp.call_depth > kMaxCallDepth) {
        interp.call_depth--;
        return Completion::Thr("script exceeded maximum call depth (possible unbounded recursion)");
    }
    EnvPtr scope = std::make_shared<Environment>();
    scope->parent = fn->closure;
    const Node &def = *fn->fn_node;
    for (size_t i = 0; i < def.params.size(); i++) {
        scope->Define(def.params[i], i < args.size() ? args[i] : Value::Undef());
    }
    if (def.arrow_expr_body) {
        // A concise arrow body's expression value *is* the return value --
        // unlike a block body, there's no explicit `return` to produce a
        // Return completion, so this returns straight from the expression's
        // own (already-Normal-or-Throw) completion rather than falling
        // through to the Return-completion check below, which would never
        // match and silently discard the value as undefined.
        Completion body_result = EvalExpr(interp, *def.body[0], scope);
        interp.call_depth--;
        if (body_result.type == CompletionType::Throw) return body_result;
        return Completion::Norm(body_result.value);
    }
    Completion result = ExecBlockBody(interp, def.body, scope);
    interp.call_depth--;
    if (result.type == CompletionType::Return) return Completion::Norm(result.value);
    if (result.type == CompletionType::Throw) return result;
    return Completion::Norm(Value::Undef());
}

// Returns false (with `out` set to a Throw completion) if `target` isn't
// something assignable to (a bare identifier, a.b, or a[expr]) -- every
// caller propagates that the same way any other abrupt completion is
// propagated.
bool AssignTo(Interpreter &interp, const Node &target, Value val, EnvPtr &env, Completion &out) {
    if (target.kind == NodeKind::Ident) {
        Value *slot = env->Find(target.name);
        if (slot) {
            *slot = std::move(val);
        } else {
            interp.global->Define(target.name, std::move(val));  // undeclared assignment creates a global, matching non-strict-mode JS
        }
        return true;
    }
    if (target.kind == NodeKind::Member) {
        Completion objc = EvalExpr(interp, *target.a, env);
        if (objc.IsAbrupt()) {
            out = objc;
            return false;
        }
        if (objc.value.type != VType::Object || !objc.value.obj) {
            out = Completion::Thr("cannot set property of non-object");
            return false;
        }
        std::string key = target.prop_name;
        if (target.computed) {
            Completion keyc = EvalExpr(interp, *target.b, env);
            if (keyc.IsAbrupt()) {
                out = keyc;
                return false;
            }
            key = keyc.value.type == VType::Number ? NumberToString(keyc.value.num) : ToDisplayString(keyc.value);
        }
        SetProp(objc.value.obj, key, val);
        return true;
    }
    out = Completion::Thr("invalid assignment target");
    return false;
}

Completion EvalExpr(Interpreter &interp, const Node &n, EnvPtr &env) {
    Completion guard;
    if (interp.StepGuard(guard)) return guard;

    switch (n.kind) {
        case NodeKind::NumberLit:
            return Completion::Norm(Value::Num(n.num));
        case NodeKind::StringLit:
            return Completion::Norm(Value::Str(n.str));
        case NodeKind::BoolLit:
            return Completion::Norm(Value::Bool(n.boolean));
        case NodeKind::NullLit:
            return Completion::Norm(Value::MakeNull());
        case NodeKind::UndefinedLit:
            return Completion::Norm(Value::Undef());
        case NodeKind::TemplateLit: {
            std::string out;
            size_t expr_i = 0;
            for (size_t i = 0; i < n.is_expr_part.size(); i++) {
                if (!n.is_expr_part[i]) {
                    out += n.template_texts[i];
                } else {
                    Completion c = EvalExpr(interp, *n.template_exprs[expr_i++], env);
                    if (c.IsAbrupt()) return c;
                    out += ToDisplayString(c.value);
                }
            }
            return Completion::Norm(Value::Str(out));
        }
        case NodeKind::ArrayLit: {
            auto obj = std::make_shared<ObjectData>();
            obj->is_array = true;
            for (size_t i = 0; i < n.elements.size(); i++) {
                Completion c = EvalExpr(interp, *n.elements[i], env);
                if (c.IsAbrupt()) return c;
                obj->props[std::to_string(i)] = c.value;
            }
            obj->props["length"] = Value::Num(static_cast<double>(n.elements.size()));
            return Completion::Norm(Value::Obj(obj));
        }
        case NodeKind::ObjectLit: {
            auto obj = std::make_shared<ObjectData>();
            for (auto &kv : n.obj_props) {
                Completion c = EvalExpr(interp, *kv.second, env);
                if (c.IsAbrupt()) return c;
                obj->props[kv.first] = c.value;
            }
            return Completion::Norm(Value::Obj(obj));
        }
        case NodeKind::Ident: {
            Value *slot = env->Find(n.name);
            if (!slot) return Completion::Thr("'" + n.name + "' is not defined");
            return Completion::Norm(*slot);
        }
        case NodeKind::Unary: {
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
            if (n.op == "-") return Completion::Norm(Value::Num(-ToNumber(c.value)));
            if (n.op == "+") return Completion::Norm(Value::Num(ToNumber(c.value)));
            if (n.op == "!") return Completion::Norm(Value::Bool(!c.value.Truthy()));
            if (n.op == "typeof") {
                switch (c.value.type) {
                    case VType::Undefined:
                        return Completion::Norm(Value::Str("undefined"));
                    case VType::Null:
                        return Completion::Norm(Value::Str("object"));
                    case VType::Number:
                        return Completion::Norm(Value::Str("number"));
                    case VType::String:
                        return Completion::Norm(Value::Str("string"));
                    case VType::Boolean:
                        return Completion::Norm(Value::Str("boolean"));
                    case VType::Object:
                        return Completion::Norm(Value::Str(c.value.obj && c.value.obj->is_function ? "function" : "object"));
                }
            }
            return Completion::Thr("unsupported unary operator");
        }
        case NodeKind::Update: {
            Completion cur = EvalExpr(interp, *n.a, env);
            if (cur.IsAbrupt()) return cur;
            double old_val = ToNumber(cur.value);
            double new_val = n.op == "++" ? old_val + 1 : old_val - 1;
            Completion out;
            if (!AssignTo(interp, *n.a, Value::Num(new_val), env, out)) return out;
            // Prefix yields the updated value (n.boolean==true, see ParseUnary);
            // postfix yields the pre-update value -- the one real behavioral
            // difference between `++i` and `i++` this engine bothers to model.
            return Completion::Norm(Value::Num(n.boolean ? new_val : old_val));
        }
        case NodeKind::Logical: {
            Completion l = EvalExpr(interp, *n.a, env);
            if (l.IsAbrupt()) return l;
            if (n.op == "&&") return l.value.Truthy() ? EvalExpr(interp, *n.b, env) : l;
            return l.value.Truthy() ? l : EvalExpr(interp, *n.b, env);
        }
        case NodeKind::Binary: {
            Completion l = EvalExpr(interp, *n.a, env);
            if (l.IsAbrupt()) return l;
            Completion r = EvalExpr(interp, *n.b, env);
            if (r.IsAbrupt()) return r;
            const Value &lv = l.value;
            const Value &rv = r.value;
            if (n.op == "+") {
                if (lv.type == VType::String || rv.type == VType::String) {
                    return Completion::Norm(Value::Str(ToDisplayString(lv) + ToDisplayString(rv)));
                }
                return Completion::Norm(Value::Num(ToNumber(lv) + ToNumber(rv)));
            }
            if (n.op == "-") return Completion::Norm(Value::Num(ToNumber(lv) - ToNumber(rv)));
            if (n.op == "*") return Completion::Norm(Value::Num(ToNumber(lv) * ToNumber(rv)));
            if (n.op == "/") return Completion::Norm(Value::Num(ToNumber(lv) / ToNumber(rv)));
            if (n.op == "%") return Completion::Norm(Value::Num(std::fmod(ToNumber(lv), ToNumber(rv))));
            if (n.op == "==") return Completion::Norm(Value::Bool(StrictEquals(lv, rv)));
            if (n.op == "!=") return Completion::Norm(Value::Bool(!StrictEquals(lv, rv)));
            if (n.op == "<" || n.op == ">" || n.op == "<=" || n.op == ">=") {
                bool result;
                if (lv.type == VType::String && rv.type == VType::String) {
                    result = n.op == "<" ? lv.str < rv.str : n.op == ">" ? lv.str > rv.str : n.op == "<=" ? lv.str <= rv.str : lv.str >= rv.str;
                } else {
                    double a = ToNumber(lv), b = ToNumber(rv);
                    result = n.op == "<" ? a < b : n.op == ">" ? a > b : n.op == "<=" ? a <= b : a >= b;
                }
                return Completion::Norm(Value::Bool(result));
            }
            return Completion::Thr("unsupported binary operator '" + n.op + "'");
        }
        case NodeKind::Assign: {
            Completion rhs;
            if (n.op == "=") {
                rhs = EvalExpr(interp, *n.b, env);
            } else {
                Completion cur = EvalExpr(interp, *n.a, env);
                if (cur.IsAbrupt()) return cur;
                Completion r = EvalExpr(interp, *n.b, env);
                if (r.IsAbrupt()) return r;
                if (n.op == "+=" && (cur.value.type == VType::String || r.value.type == VType::String)) {
                    rhs = Completion::Norm(Value::Str(ToDisplayString(cur.value) + ToDisplayString(r.value)));
                } else {
                    double a = ToNumber(cur.value), b = ToNumber(r.value);
                    double result = n.op == "+=" ? a + b : n.op == "-=" ? a - b : n.op == "*=" ? a * b : a / b;
                    rhs = Completion::Norm(Value::Num(result));
                }
            }
            if (rhs.IsAbrupt()) return rhs;
            Completion out;
            if (!AssignTo(interp, *n.a, rhs.value, env, out)) return out;
            return Completion::Norm(rhs.value);
        }
        case NodeKind::Member: {
            Completion objc = EvalExpr(interp, *n.a, env);
            if (objc.IsAbrupt()) return objc;
            if (objc.value.type != VType::Object || !objc.value.obj) {
                return Completion::Thr("cannot read property of " + ToDisplayString(objc.value));
            }
            std::string key = n.prop_name;
            if (n.computed) {
                Completion keyc = EvalExpr(interp, *n.b, env);
                if (keyc.IsAbrupt()) return keyc;
                key = keyc.value.type == VType::Number ? NumberToString(keyc.value.num) : ToDisplayString(keyc.value);
            }
            return Completion::Norm(GetProp(objc.value.obj, key));
        }
        case NodeKind::Call: {
            Completion calleec = EvalExpr(interp, *n.a, env);
            if (calleec.IsAbrupt()) return calleec;
            if (calleec.value.type != VType::Object || !calleec.value.obj || !calleec.value.obj->is_function) {
                return Completion::Thr("value is not a function");
            }
            std::vector<Value> args;
            for (auto &a : n.args) {
                Completion c = EvalExpr(interp, *a, env);
                if (c.IsAbrupt()) return c;
                args.push_back(c.value);
            }
            return CallFunction(interp, calleec.value.obj, args);
        }
        case NodeKind::Conditional: {
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
            return c.value.Truthy() ? EvalExpr(interp, *n.b, env) : EvalExpr(interp, *n.c, env);
        }
        case NodeKind::FunctionExpr: {
            auto obj = std::make_shared<ObjectData>();
            obj->is_function = true;
            obj->fn_node = &n;
            obj->closure = env;
            return Completion::Norm(Value::Obj(obj));
        }
        default:
            return Completion::Thr("expression not supported in this context");
    }
}

Completion ExecStmt(Interpreter &interp, const Node &n, EnvPtr &env) {
    switch (n.kind) {
        case NodeKind::Block: {
            EnvPtr inner = std::make_shared<Environment>();
            inner->parent = env;
            return ExecBlockBody(interp, n.body, inner);
        }
        case NodeKind::ExprStmt:
            return EvalExpr(interp, *n.a, env);
        case NodeKind::VarDecl: {
            for (auto &d : n.declarators) {
                Value v = Value::Undef();
                if (d.second) {
                    Completion c = EvalExpr(interp, *d.second, env);
                    if (c.IsAbrupt()) return c;
                    v = c.value;
                }
                env->Define(d.first, v);
            }
            return Completion::Norm();
        }
        case NodeKind::FunctionDecl:
            return Completion::Norm();  // already bound by ExecBlockBody's hoisting pass
        case NodeKind::If: {
            Completion c = EvalExpr(interp, *n.cond, env);
            if (c.IsAbrupt()) return c;
            if (c.value.Truthy()) return ExecStmt(interp, *n.then_branch, env);
            if (n.else_branch) return ExecStmt(interp, *n.else_branch, env);
            return Completion::Norm();
        }
        case NodeKind::While: {
            for (;;) {
                Completion guard;
                if (interp.StepGuard(guard)) return guard;
                Completion c = EvalExpr(interp, *n.cond, env);
                if (c.IsAbrupt()) return c;
                if (!c.value.Truthy()) break;
                Completion body = ExecStmt(interp, *n.then_branch, env);
                if (body.type == CompletionType::Break) break;
                if (body.type == CompletionType::Return || body.type == CompletionType::Throw) return body;
            }
            return Completion::Norm();
        }
        case NodeKind::For: {
            EnvPtr loop_env = std::make_shared<Environment>();
            loop_env->parent = env;
            if (n.init) {
                Completion c = ExecStmt(interp, *n.init, loop_env);
                if (c.IsAbrupt()) return c;
            }
            for (;;) {
                Completion guard;
                if (interp.StepGuard(guard)) return guard;
                if (n.cond) {
                    Completion c = EvalExpr(interp, *n.cond, loop_env);
                    if (c.IsAbrupt()) return c;
                    if (!c.value.Truthy()) break;
                }
                Completion body = ExecStmt(interp, *n.then_branch, loop_env);
                if (body.type == CompletionType::Break) break;
                if (body.type == CompletionType::Return || body.type == CompletionType::Throw) return body;
                if (n.update) {
                    Completion c = EvalExpr(interp, *n.update, loop_env);
                    if (c.IsAbrupt()) return c;
                }
            }
            return Completion::Norm();
        }
        case NodeKind::Return: {
            if (!n.a) return Completion::Ret(Value::Undef());
            Completion c = EvalExpr(interp, *n.a, env);
            if (c.IsAbrupt()) return c;
            return Completion::Ret(c.value);
        }
        case NodeKind::Break:
            return Completion::Brk();
        case NodeKind::Continue:
            return Completion::Cont();
        default:
            return EvalExpr(interp, n, env);
    }
}

Value MakeNativeFn(NativeFn fn) {
    auto obj = std::make_shared<ObjectData>();
    obj->is_function = true;
    obj->native = std::move(fn);
    return Value::Obj(obj);
}

void SetupGlobals(EnvPtr &global, HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log) {
    auto console = std::make_shared<ObjectData>();
    console->props["log"] = MakeNativeFn([&on_console_log](std::vector<Value> &args, bool &, std::string &) {
        std::string line;
        for (size_t i = 0; i < args.size(); i++) {
            if (i) line += " ";
            line += ToDisplayString(args[i]);
        }
        on_console_log(line);
        return Value::Undef();
    });
    global->Define("console", Value::Obj(console));

    auto document = std::make_shared<ObjectData>();
    document->is_document = true;
    document->owner_doc = &doc;
    document->props["getElementById"] = MakeNativeFn([&doc](std::vector<Value> &args, bool &, std::string &) {
        if (args.empty() || args[0].type != VType::String) return Value::MakeNull();
        DomNode *found = doc.root ? FindById(doc.root.get(), args[0].str) : nullptr;
        if (!found) return Value::MakeNull();
        auto wrapper = std::make_shared<ObjectData>();
        wrapper->dom_node = found;
        return Value::Obj(wrapper);
    });
    global->Define("document", Value::Obj(document));

    // A plain, otherwise-inert object -- enough for the extremely common
    // "window.Foo = {...}" config-stashing pattern (MathJax's own
    // bootstrap script, among many others) to assign a property instead
    // of throwing ReferenceError, without pretending this engine has any
    // of the real BOM (setTimeout/location/etc. -- see js_engine.h's own
    // header on what's deliberately not implemented yet).
    global->Define("window", Value::Obj(std::make_shared<ObjectData>()));
}

}  // namespace

void RunScripts(HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log,
                 const std::function<void(const std::string &)> &on_error) {
    for (const std::string &script : doc.scripts) {
        Parser parser(script);
        NodePtr program = parser.ParseProgram();
        if (!parser.ok) {
            on_error("script parse error: " + parser.error);
            continue;
        }
        Interpreter interp;
        interp.global = std::make_shared<Environment>();
        SetupGlobals(interp.global, doc, on_console_log);
        EnvPtr scope = interp.global;
        Completion result = ExecBlockBody(interp, program->body, scope);
        if (result.type == CompletionType::Throw) {
            on_error("script error: " + ToDisplayString(result.value));
        }
        // Program keeps every Node (including function bodies closures may
        // still reference) alive via `program`'s own ownership for exactly
        // this call's duration -- any ObjectData::fn_node pointing into it
        // that somehow escaped RunScripts (it can't: nothing above stores a
        // closure anywhere outside this function's own locals) would
        // dangle after this loop iteration ends. Not a concern in practice
        // since document.* never hands a script's own functions back to
        // the caller.
    }
}
