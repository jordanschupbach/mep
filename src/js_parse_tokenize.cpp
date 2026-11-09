// Port of acorn 8.15.0 (MIT): token types, token contexts, the tokenizer,
// parser utilities, node construction and scope tracking. See
// js_parse_parser.h.

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#include <climits>
#include <cmath>
#include <cstring>
#include <string>

#include "js_parse_parser.h"

namespace jsparse {

// acorn's charCodeAt() past the end is NaN; INT_MIN stands in for it (it
// fails every `x >= lo && x <= hi` test, the sites that compare it
// otherwise handle it explicitly).
constexpr int kNaN = INT_MIN;

// ---------------------------------------------------------------- arena

Arena::~Arena() {
    for (char* b : blocks_) delete[] b;
}

void* Arena::Alloc(size_t size, size_t align) {
    size_t pad = (align - (reinterpret_cast<uintptr_t>(cur_) % align)) % align;
    if (cur_ == nullptr || pad + size > left_) {
        size_t bsize = std::max<size_t>(64 * 1024, size + align);
        char* b = new char[bsize];
        blocks_.push_back(b);
        cur_ = b;
        left_ = bsize;
        pad = (align - (reinterpret_cast<uintptr_t>(cur_) % align)) % align;
    }
    char* p = cur_ + pad;
    cur_ = p + size;
    left_ -= pad + size;
    return p;
}

Node* Arena::NewNode() { return new (Alloc(sizeof(Node), alignof(Node))) Node(); }

std::string_view Arena::Str(std::string_view s) {
    if (s.empty()) return {};
    char* p = static_cast<char*>(Alloc(s.size(), 1));
    std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
}

NodeList Arena::List(const std::vector<Node*>& v) {
    NodeList l;
    l.count = static_cast<uint32_t>(v.size());
    if (!v.empty()) {
        l.items = static_cast<Node**>(Alloc(sizeof(Node*) * v.size(), alignof(Node*)));
        std::copy(v.begin(), v.end(), l.items);
    }
    return l;
}

// ---------------------------------------------------------------- WTF-8

void AppendWtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string ToWtf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
            ++i;
        }
        AppendWtf8(out, c);
    }
    return out;
}

bool HasLoneSurrogate(std::string_view s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) == 0xED && (static_cast<unsigned char>(s[i + 1]) & 0xE0) == 0xA0)
            return true;
    }
    return false;
}

static void AppendCodePoint16(std::u16string& out, int code) {
    // acorn's codePointToString; NaN (kNaN) comes out as U+10000 there.
    if (code == kNaN) {
        out += static_cast<char16_t>(0xD800);
        out += static_cast<char16_t>(0xDC00);
        return;
    }
    if (code <= 0xFFFF) {
        out += static_cast<char16_t>(code);
        return;
    }
    int c = code - 0x10000;
    out += static_cast<char16_t>((c >> 10) + 0xD800);
    out += static_cast<char16_t>((c & 1023) + 0xDC00);
}

static std::string CodePointToString(int code) {
    std::u16string s;
    AppendCodePoint16(s, code);
    return ToWtf8(s);
}

// ---------------------------------------------------------------- token types

namespace tt {
// label, keyword, beforeExpr, startsExpr, isLoop, isAssign, prefix, postfix, binop
const TokenType num{"num", nullptr, false, true, false, false, false, false, -1};
const TokenType regexp{"regexp", nullptr, false, true, false, false, false, false, -1};
const TokenType string{"string", nullptr, false, true, false, false, false, false, -1};
const TokenType name{"name", nullptr, false, true, false, false, false, false, -1};
const TokenType privateId{"privateId", nullptr, false, true, false, false, false, false, -1};
const TokenType eof{"eof", nullptr, false, false, false, false, false, false, -1};
const TokenType bracketL{"[", nullptr, true, true, false, false, false, false, -1};
const TokenType bracketR{"]", nullptr, false, false, false, false, false, false, -1};
const TokenType braceL{"{", nullptr, true, true, false, false, false, false, -1};
const TokenType braceR{"}", nullptr, false, false, false, false, false, false, -1};
const TokenType parenL{"(", nullptr, true, true, false, false, false, false, -1};
const TokenType parenR{")", nullptr, false, false, false, false, false, false, -1};
const TokenType comma{",", nullptr, true, false, false, false, false, false, -1};
const TokenType semi{";", nullptr, true, false, false, false, false, false, -1};
const TokenType colon{":", nullptr, true, false, false, false, false, false, -1};
const TokenType dot{".", nullptr, false, false, false, false, false, false, -1};
const TokenType question{"?", nullptr, true, false, false, false, false, false, -1};
const TokenType questionDot{"?.", nullptr, false, false, false, false, false, false, -1};
const TokenType arrow{"=>", nullptr, true, false, false, false, false, false, -1};
const TokenType template_{"template", nullptr, false, false, false, false, false, false, -1};
const TokenType invalidTemplate{"invalidTemplate", nullptr, false, false, false, false, false, false, -1};
const TokenType ellipsis{"...", nullptr, true, false, false, false, false, false, -1};
const TokenType backQuote{"`", nullptr, false, true, false, false, false, false, -1};
const TokenType dollarBraceL{"${", nullptr, true, true, false, false, false, false, -1};
const TokenType eq{"=", nullptr, true, false, false, true, false, false, -1};
const TokenType assign{"_=", nullptr, true, false, false, true, false, false, -1};
const TokenType incDec{"++/--", nullptr, false, true, false, false, true, true, -1};
const TokenType prefix{"!/~", nullptr, true, true, false, false, true, false, -1};
const TokenType logicalOR{"||", nullptr, true, false, false, false, false, false, 1};
const TokenType logicalAND{"&&", nullptr, true, false, false, false, false, false, 2};
const TokenType bitwiseOR{"|", nullptr, true, false, false, false, false, false, 3};
const TokenType bitwiseXOR{"^", nullptr, true, false, false, false, false, false, 4};
const TokenType bitwiseAND{"&", nullptr, true, false, false, false, false, false, 5};
const TokenType equality{"==/!=/===/!==", nullptr, true, false, false, false, false, false, 6};
const TokenType relational{"</>/<=/>=", nullptr, true, false, false, false, false, false, 7};
const TokenType bitShift{"<</>>/>>>", nullptr, true, false, false, false, false, false, 8};
const TokenType plusMin{"+/-", nullptr, true, true, false, false, true, false, 9};
const TokenType modulo{"%", nullptr, true, false, false, false, false, false, 10};
const TokenType star{"*", nullptr, true, false, false, false, false, false, 10};
const TokenType slash{"/", nullptr, true, false, false, false, false, false, 10};
const TokenType starstar{"**", nullptr, true, false, false, false, false, false, -1};
const TokenType coalesce{"??", nullptr, true, false, false, false, false, false, 1};
const TokenType _break{"break", "break", false, false, false, false, false, false, -1};
const TokenType _case{"case", "case", true, false, false, false, false, false, -1};
const TokenType _catch{"catch", "catch", false, false, false, false, false, false, -1};
const TokenType _continue{"continue", "continue", false, false, false, false, false, false, -1};
const TokenType _debugger{"debugger", "debugger", false, false, false, false, false, false, -1};
const TokenType _default{"default", "default", true, false, false, false, false, false, -1};
const TokenType _do{"do", "do", true, false, true, false, false, false, -1};
const TokenType _else{"else", "else", true, false, false, false, false, false, -1};
const TokenType _finally{"finally", "finally", false, false, false, false, false, false, -1};
const TokenType _for{"for", "for", false, false, true, false, false, false, -1};
const TokenType _function{"function", "function", false, true, false, false, false, false, -1};
const TokenType _if{"if", "if", false, false, false, false, false, false, -1};
const TokenType _return{"return", "return", true, false, false, false, false, false, -1};
const TokenType _switch{"switch", "switch", false, false, false, false, false, false, -1};
const TokenType _throw{"throw", "throw", true, false, false, false, false, false, -1};
const TokenType _try{"try", "try", false, false, false, false, false, false, -1};
const TokenType _var{"var", "var", false, false, false, false, false, false, -1};
const TokenType _const{"const", "const", false, false, false, false, false, false, -1};
const TokenType _while{"while", "while", false, false, true, false, false, false, -1};
const TokenType _with{"with", "with", false, false, false, false, false, false, -1};
const TokenType _new{"new", "new", true, true, false, false, false, false, -1};
const TokenType _this{"this", "this", false, true, false, false, false, false, -1};
const TokenType _super{"super", "super", false, true, false, false, false, false, -1};
const TokenType _class{"class", "class", false, true, false, false, false, false, -1};
const TokenType _extends{"extends", "extends", true, false, false, false, false, false, -1};
const TokenType _export{"export", "export", false, false, false, false, false, false, -1};
const TokenType _import{"import", "import", false, true, false, false, false, false, -1};
const TokenType _null{"null", "null", false, true, false, false, false, false, -1};
const TokenType _true{"true", "true", false, true, false, false, false, false, -1};
const TokenType _false{"false", "false", false, true, false, false, false, false, -1};
const TokenType _in{"in", "in", true, false, false, false, false, false, 7};
const TokenType _instanceof{"instanceof", "instanceof", true, false, false, false, false, false, 7};
const TokenType _typeof{"typeof", "typeof", true, true, false, false, true, false, -1};
const TokenType _void{"void", "void", true, true, false, false, true, false, -1};
const TokenType _delete{"delete", "delete", true, true, false, false, true, false, -1};
const TokenType jsxName{"jsxName", nullptr, false, false, false, false, false, false, -1};
const TokenType jsxText{"jsxText", nullptr, true, false, false, false, false, false, -1};
const TokenType jsxTagStart{"jsxTagStart", nullptr, false, true, false, false, false, false, -1};
const TokenType jsxTagEnd{"jsxTagEnd", nullptr, false, false, false, false, false, false, -1};

const TokenType* Keyword(std::string_view w) {
    static const TokenType* const all[] = {&_break, &_case,   &_catch, &_continue, &_debugger, &_default,
                                           &_do,    &_else,   &_finally, &_for,    &_function, &_if,
                                           &_return, &_switch, &_throw, &_try,     &_var,      &_const,
                                           &_while, &_with,   &_new,   &_this,     &_super,    &_class,
                                           &_extends, &_export, &_import, &_null,  &_true,     &_false,
                                           &_in,    &_instanceof, &_typeof, &_void, &_delete};
    for (const TokenType* t : all)
        if (w == t->keyword) return t;
    return nullptr;
}
}  // namespace tt

namespace tc {
// token, isExpr, preserveSpace, override(template), generator
const TokContext b_stat{"{", false, false, false, false};
const TokContext b_expr{"{", true, false, false, false};
const TokContext b_tmpl{"${", false, false, false, false};
const TokContext p_stat{"(", false, false, false, false};
const TokContext p_expr{"(", true, false, false, false};
const TokContext q_tmpl{"`", true, true, true, false};
const TokContext f_stat{"function", false, false, false, false};
const TokContext f_expr{"function", true, false, false, false};
const TokContext f_expr_gen{"function", true, false, false, true};
const TokContext f_gen{"function", false, false, false, true};
const TokContext j_oTag{"<tag", false, false, false, false};
const TokContext j_cTag{"</tag", false, false, false, false};
const TokContext j_expr{"<tag>...</tag>", true, true, false, false};
}  // namespace tc

static bool IsFunctionCtx(const TokContext* c) { return c && std::strcmp(c->token, "function") == 0; }

static bool IsNewLine(int code) { return code == 10 || code == 13 || code == 0x2028 || code == 0x2029; }

static bool IsNonAsciiWhitespace(int ch) {
    return ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200a) || ch == 0x202f || ch == 0x205f || ch == 0x3000 ||
           ch == 0xfeff;
}

// JavaScript's \s
static bool IsJsSpace(int ch) {
    return ch == 9 || ch == 10 || ch == 11 || ch == 12 || ch == 13 || ch == 32 || ch == 0xa0 || ch == 0x2028 ||
           ch == 0x2029 || IsNonAsciiWhitespace(ch);
}

static const char* const kEcma5Keywords[] = {"break", "case",   "catch",  "continue", "debugger", "default", "do",
                                             "else",  "finally", "for",   "function", "if",       "return",  "switch",
                                             "throw", "try",    "var",    "while",    "with",     "null",    "true",
                                             "false", "instanceof", "typeof", "void", "delete",   "new",     "in",
                                             "this"};

// ---------------------------------------------------------------- parser state

Parser::DepthGuard::DepthGuard(Parser& parser) : p(parser) {
    if (++p.depth > p.maxDepth) {
        --p.depth;
        p.raise(p.start, "Too deeply nested");
    }
}

Parser::Parser(const Options& o, std::u16string_view in, Arena& a, ParseResult& r)
    : arena(a), out(r), input(in), inputLen(static_cast<int>(in.size())), type(&tt::eof) {
    ecmaVersion = o.ecma_version;
    if (ecmaVersion >= 2015 && ecmaVersion < 100000000) ecmaVersion -= 2009;
    sourceType = o.source_type;
    allowReserved = ecmaVersion < 5;
    allowHashBang = o.allow_hash_bang < 0 ? ecmaVersion >= 14 : o.allow_hash_bang != 0;
    allowReturnOutsideFunction = o.allow_return_outside_function;
    allowImportExportEverywhere = o.allow_import_export_everywhere;
    allowAwaitOutsideFunction = o.allow_await_outside_function > 0;
    allowSuperOutsideMethod = o.allow_super_outside_method;
    checkPrivateFields = o.check_private_fields;
    preserveParens = o.preserve_parens;
    recover = o.recover;
    collectTokens = o.tokens;
    maxDepth = o.max_depth;
    keywordSet = ecmaVersion >= 6 ? 6 : sourceType == SourceType::Module ? 50 : 5;

    std::vector<std::string_view> reserved;
    if (!allowReserved) {
        static const char* const r3[] = {"abstract", "boolean", "byte", "char", "class", "double", "enum", "export",
                                         "extends", "final", "float", "goto", "implements", "import", "int",
                                         "interface", "long", "native", "package", "private", "protected",
                                         "public", "short", "static", "super", "synchronized", "throws",
                                         "transient", "volatile"};
        static const char* const r5[] = {"class", "enum", "extends", "super", "const", "export", "import"};
        if (ecmaVersion >= 6) {
            reserved.push_back("enum");
        } else if (ecmaVersion == 5) {
            for (const char* w : r5) reserved.push_back(w);
        } else {
            for (const char* w : r3) reserved.push_back(w);
        }
        if (sourceType == SourceType::Module) reserved.push_back("await");
    }
    reservedWords = reserved;
    reservedWordsStrict = reserved;
    for (const char* w : {"implements", "interface", "let", "package", "private", "protected", "public", "static",
                          "yield"})
        reservedWordsStrict.push_back(w);
    reservedWordsStrictBind = reservedWordsStrict;
    reservedWordsStrictBind.push_back("eval");
    reservedWordsStrictBind.push_back("arguments");

    pos = lineStart = 0;
    curLine = 1;
    start = end = pos;
    startLoc = endLoc = curPosition();
    lastTokStart = lastTokEnd = pos;
    context.push_back(&tc::b_stat);
    exprAllowed = true;
    inModule = sourceType == SourceType::Module;
    strict = inModule || strictDirective(pos);
    if (pos == 0 && allowHashBang && inputLen >= 2 && input[0] == u'#' && input[1] == u'!') skipLineComment(2);
    enterScope(SCOPE_TOP);
}

Parser::~Parser() = default;

std::u16string_view Parser::slice(int a, int b) const {
    a = std::clamp(a, 0, inputLen);
    b = std::clamp(b, 0, inputLen);
    if (b <= a) return {};
    return input.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
}

bool Parser::lineBreakIn(int a, int b) const {
    a = std::max(a, 0);
    b = std::min(b, inputLen);
    for (int i = a; i < b; ++i)
        if (IsNewLine(input[static_cast<size_t>(i)])) return true;
    return false;
}

// skipWhiteSpace = /(?:\s|\/\/.*|\/\*[^]*?\*\/)*/g
int Parser::skipWhiteSpaceAt(int p) const {
    for (;;) {
        if (p >= inputLen) return p;
        int c = input[static_cast<size_t>(p)];
        if (IsJsSpace(c)) {
            ++p;
            continue;
        }
        if (c == '/' && p + 1 < inputLen) {
            int n = input[static_cast<size_t>(p + 1)];
            if (n == '/') {
                p += 2;
                while (p < inputLen && !IsNewLine(input[static_cast<size_t>(p)])) ++p;
                continue;
            }
            if (n == '*') {
                int q = p + 2;
                bool found = false;
                for (; q + 1 < inputLen; ++q) {
                    if (input[static_cast<size_t>(q)] == '*' && input[static_cast<size_t>(q + 1)] == '/') {
                        found = true;
                        break;
                    }
                }
                if (!found) return p;
                p = q + 2;
                continue;
            }
        }
        return p;
    }
}

bool Parser::inList(const std::vector<std::string_view>& l, std::string_view w) {
    return std::find(l.begin(), l.end(), w) != l.end();
}

bool Parser::isKeywordWord(std::string_view w) const {
    for (const char* k : kEcma5Keywords)
        if (w == k) return true;
    if (keywordSet == 50) return w == "export" || w == "import";
    if (keywordSet == 6)
        return w == "const" || w == "class" || w == "extends" || w == "export" || w == "import" || w == "super";
    return false;
}

// getLineInfo
int Parser::lineIndentAt(int p) const {
    int ls = std::clamp(p, 0, inputLen);
    while (ls > 0 && !IsNewLine(charAt(ls - 1))) --ls;
    int i = ls;
    while (i < inputLen && (charAt(i) == ' ' || charAt(i) == '\t')) ++i;
    return i - ls;
}

Pos Parser::lineInfo(int offset) const {
    int line = 1, cur = 0;
    for (;;) {
        int nextBreak = -1;
        for (int i = cur; i < offset && i < inputLen; ++i) {
            int c = input[static_cast<size_t>(i)];
            if (IsNewLine(c)) {
                nextBreak = (i < offset - 1 && c == 13 && i + 1 < inputLen && input[static_cast<size_t>(i + 1)] == 10)
                                ? i + 2
                                : i + 1;
                break;
            }
        }
        if (nextBreak < 0) return Pos{static_cast<uint32_t>(line), static_cast<uint32_t>(std::max(0, offset - cur))};
        ++line;
        cur = nextBreak;
    }
}

void Parser::raise(int p, const std::string& message) { throw SyntaxErr{message, p, pos}; }

void Parser::raiseRecoverable(int p, const std::string& message) {
    if (!recover) raise(p, message);
    if (!quietRecoverable) recordError(SyntaxErr{message, p, pos});
}

void Parser::unexpected(int p) { raise(p >= 0 ? p : start, "Unexpected token"); }

bool Parser::canAwait() {
    for (size_t i = scopeStack.size(); i-- > 0;) {
        int flags = scopeStack[i].flags;
        if (flags & (SCOPE_CLASS_STATIC_BLOCK | SCOPE_CLASS_FIELD_INIT)) return false;
        if (flags & SCOPE_FUNCTION) return (flags & SCOPE_ASYNC) > 0;
    }
    return (inModule && ecmaVersion >= 13) || allowAwaitOutsideFunction;
}

bool Parser::allowNewDotTarget() {
    for (size_t i = scopeStack.size(); i-- > 0;) {
        int flags = scopeStack[i].flags;
        if ((flags & (SCOPE_CLASS_STATIC_BLOCK | SCOPE_CLASS_FIELD_INIT)) ||
            ((flags & SCOPE_FUNCTION) && !(flags & SCOPE_ARROW)))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- parseutil

bool Parser::strictDirective(int s) {
    if (ecmaVersion < 5) return false;
    for (;;) {
        s = skipWhiteSpaceAt(s);
        // literal = /^(?:'((?:\\[^]|[^'\\])*?)'|"((?:\\[^]|[^"\\])*?)")/
        if (s >= inputLen) return false;
        int q = input[static_cast<size_t>(s)];
        if (q != '\'' && q != '"') return false;
        int i = s + 1;
        bool closed = false;
        while (i < inputLen) {
            int c = input[static_cast<size_t>(i)];
            if (c == '\\') {
                if (i + 1 >= inputLen) break;
                i += 2;
                continue;
            }
            if (c == q) {
                closed = true;
                break;
            }
            ++i;
        }
        if (!closed) return false;
        int matchLen = i + 1 - s;
        std::u16string_view content = slice(s + 1, i);
        if (content == u"use strict") {
            int e = skipWhiteSpaceAt(s + matchLen);
            int next = e < inputLen ? input[static_cast<size_t>(e)] : -1;
            if (next == ';' || next == '}') return true;
            if (!lineBreakIn(s + matchLen, e)) return false;
            static const char16_t kOps[] = u"(`.[+-/*%<>=,?^&";
            bool isOp = false;
            for (const char16_t* c = kOps; *c; ++c)
                if (next == *c) isOp = true;
            int next2 = e + 1 < inputLen ? input[static_cast<size_t>(e + 1)] : -1;
            return !(isOp || (next == '!' && next2 == '='));
        }
        s += matchLen;
        s = skipWhiteSpaceAt(s);
        if (s < inputLen && input[static_cast<size_t>(s)] == ';') ++s;
    }
}

bool Parser::eat(const TokenType& t) {
    if (type == &t) {
        next();
        return true;
    }
    return false;
}

bool Parser::isContextual(std::string_view n) const { return type == &tt::name && value == n && !containsEsc; }

bool Parser::eatContextual(std::string_view n) {
    if (!isContextual(n)) return false;
    next();
    return true;
}

void Parser::expectContextual(std::string_view n) {
    if (!eatContextual(n)) unexpected();
}

bool Parser::canInsertSemicolon() const {
    return type == &tt::eof || type == &tt::braceR || lineBreakIn(lastTokEnd, start);
}

void Parser::semicolon() {
    if (!eat(tt::semi) && !insertSemicolon()) unexpected();
}

bool Parser::afterTrailingComma(const TokenType& tokType, bool notNext) {
    if (type == &tokType) {
        if (!notNext) next();
        return true;
    }
    return false;
}

void Parser::expect(const TokenType& t) {
    if (!eat(t)) unexpected();
}

void Parser::checkPatternErrors(DestructuringErrors* r, bool isAssign) {
    if (!r) return;
    if (r->trailingComma > -1) raiseRecoverable(r->trailingComma, "Comma is not permitted after the rest element");
    int parens = isAssign ? r->parenthesizedAssign : r->parenthesizedBind;
    if (parens > -1) raiseRecoverable(parens, isAssign ? "Assigning to rvalue" : "Parenthesized pattern");
}

bool Parser::checkExpressionErrors(DestructuringErrors* r, bool andThrow) {
    if (!r) return false;
    int shorthandAssign = r->shorthandAssign, doubleProto = r->doubleProto;
    if (!andThrow) return shorthandAssign >= 0 || doubleProto >= 0;
    if (shorthandAssign >= 0)
        raise(shorthandAssign, "Shorthand property assignments are valid only in destructuring patterns");
    if (doubleProto >= 0) raiseRecoverable(doubleProto, "Redefinition of __proto__ property");
    return false;
}

void Parser::checkYieldAwaitInDefaultParams() {
    if (yieldPos && (!awaitPos || yieldPos < awaitPos)) raise(yieldPos, "Yield expression cannot be a default value");
    if (awaitPos) raise(awaitPos, "Await expression cannot be a default value");
}

bool Parser::isSimpleAssignTarget(Node* expr) {
    if (expr->type == NodeType::ParenthesizedExpression) return isSimpleAssignTarget(expr->expression());
    return expr->type == NodeType::Identifier || expr->type == NodeType::MemberExpression;
}

// ---------------------------------------------------------------- nodes

Node* Parser::startNode() { return startNodeAt(start, startLoc); }

Node* Parser::startNodeAt(int p, Pos loc) {
    Node* n = arena.NewNode();
    n->ustart = static_cast<uint32_t>(p);
    n->loc_start = loc;
    return n;
}

Node* Parser::finishNode(Node* node, NodeType t) { return finishNodeAt(node, t, lastTokEnd, lastTokEndLoc); }

Node* Parser::finishNodeAt(Node* node, NodeType t, int p, Pos loc) {
    node->type = t;
    node->uend = static_cast<uint32_t>(p);
    node->loc_end = loc;
    node->start = out.ByteOffset(node->ustart);
    node->end = out.ByteOffset(node->uend);
    return node;
}

Node* Parser::copyNode(Node* node) {
    Node* n = arena.NewNode();
    *n = *node;
    return n;
}

// ---------------------------------------------------------------- scope

bool Parser::treatFunctionsAsVarInScope(const Scope& scope) const {
    return (scope.flags & SCOPE_FUNCTION) || (!inModule && (scope.flags & SCOPE_TOP));
}

void Parser::removeUndefinedExport(std::string_view name) {
    for (auto it = undefinedExports.begin(); it != undefinedExports.end(); ++it) {
        if (it->first == name) {
            undefinedExports.erase(it);
            return;
        }
    }
}

void Parser::declareName(std::string_view name, int bindingType, int p) {
    bool redeclared = false;
    if (bindingType == BIND_LEXICAL) {
        Scope& scope = currentScope();
        redeclared = inList(scope.lexical, name) || inList(scope.functions, name) || inList(scope.var, name);
        scope.lexical.push_back(name);
        if (inModule && (scope.flags & SCOPE_TOP)) removeUndefinedExport(name);
    } else if (bindingType == BIND_SIMPLE_CATCH) {
        currentScope().lexical.push_back(name);
    } else if (bindingType == BIND_FUNCTION) {
        Scope& scope = currentScope();
        if (treatFunctionsAsVar())
            redeclared = inList(scope.lexical, name);
        else
            redeclared = inList(scope.lexical, name) || inList(scope.var, name);
        scope.functions.push_back(name);
    } else {
        for (size_t i = scopeStack.size(); i-- > 0;) {
            Scope& scope = scopeStack[i];
            if ((inList(scope.lexical, name) && !((scope.flags & SCOPE_SIMPLE_CATCH) && scope.lexical[0] == name)) ||
                (!treatFunctionsAsVarInScope(scope) && inList(scope.functions, name))) {
                redeclared = true;
                break;
            }
            scope.var.push_back(name);
            if (inModule && (scope.flags & SCOPE_TOP)) removeUndefinedExport(name);
            if (scope.flags & SCOPE_VAR) break;
        }
    }
    if (redeclared) raiseRecoverable(p, "Identifier '" + std::string(name) + "' has already been declared");
}

void Parser::checkLocalExport(Node* id) {
    if (!inList(scopeStack[0].lexical, id->str) && !inList(scopeStack[0].var, id->str)) {
        for (auto& e : undefinedExports) {
            if (e.first == id->str) {
                e.second = id;
                return;
            }
        }
        undefinedExports.emplace_back(id->str, id);
    }
}

Scope& Parser::currentVarScope() {
    for (size_t i = scopeStack.size(); i-- > 0;) {
        Scope& scope = scopeStack[i];
        if (scope.flags & (SCOPE_VAR | SCOPE_CLASS_FIELD_INIT | SCOPE_CLASS_STATIC_BLOCK)) return scope;
    }
    return scopeStack[0];
}

Scope& Parser::currentThisScope() {
    for (size_t i = scopeStack.size(); i-- > 0;) {
        Scope& scope = scopeStack[i];
        if ((scope.flags & (SCOPE_VAR | SCOPE_CLASS_FIELD_INIT | SCOPE_CLASS_STATIC_BLOCK)) &&
            !(scope.flags & SCOPE_ARROW))
            return scope;
    }
    return scopeStack[0];
}

// ---------------------------------------------------------------- context

bool Parser::braceIsBlock(const TokenType* prevType) {
    const TokContext* parent = curContext();
    if (parent == &tc::f_expr || parent == &tc::f_stat) return true;
    if (prevType == &tt::colon && (parent == &tc::b_stat || parent == &tc::b_expr)) return !parent->isExpr;
    if (prevType == &tt::_return || (prevType == &tt::name && exprAllowed)) return lineBreakIn(lastTokEnd, start);
    if (prevType == &tt::_else || prevType == &tt::semi || prevType == &tt::eof || prevType == &tt::parenR ||
        prevType == &tt::arrow)
        return true;
    if (prevType == &tt::braceL) return parent == &tc::b_stat;
    if (prevType == &tt::_var || prevType == &tt::_const || prevType == &tt::name) return false;
    return !exprAllowed;
}

bool Parser::inGeneratorContext() const {
    for (size_t i = context.size(); i-- > 1;) {
        if (IsFunctionCtx(context[i])) return context[i]->generator;
    }
    return false;
}

void Parser::overrideContext(const TokContext* tokenCtx) {
    if (curContext() != tokenCtx && !context.empty()) context.back() = tokenCtx;
}

void Parser::updateContext(const TokenType* prevType) { updateContextBase(prevType); }

void Parser::updateContextBase(const TokenType* prevType) {
    const TokenType* t = type;
    if (t->keyword && prevType == &tt::dot) {
        exprAllowed = false;
        return;
    }
    if (t == &tt::parenR || t == &tt::braceR) {
        if (context.size() <= 1) {
            exprAllowed = true;
            return;
        }
        const TokContext* o = context.back();
        context.pop_back();
        if (o == &tc::b_stat && IsFunctionCtx(curContext())) {
            o = context.back();
            context.pop_back();
        }
        exprAllowed = !o->isExpr;
    } else if (t == &tt::braceL) {
        context.push_back(braceIsBlock(prevType) ? &tc::b_stat : &tc::b_expr);
        exprAllowed = true;
    } else if (t == &tt::dollarBraceL) {
        context.push_back(&tc::b_tmpl);
        exprAllowed = true;
    } else if (t == &tt::parenL) {
        bool statementParens = prevType == &tt::_if || prevType == &tt::_for || prevType == &tt::_with ||
                               prevType == &tt::_while;
        context.push_back(statementParens ? &tc::p_stat : &tc::p_expr);
        exprAllowed = true;
    } else if (t == &tt::incDec) {
        // tokExprAllowed stays unchanged
    } else if (t == &tt::_function || t == &tt::_class) {
        if (prevType->beforeExpr && prevType != &tt::_else &&
            !(prevType == &tt::semi && curContext() != &tc::p_stat) &&
            !(prevType == &tt::_return && lineBreakIn(lastTokEnd, start)) &&
            !((prevType == &tt::colon || prevType == &tt::braceL) && curContext() == &tc::b_stat))
            context.push_back(&tc::f_expr);
        else
            context.push_back(&tc::f_stat);
        exprAllowed = false;
    } else if (t == &tt::colon) {
        if (IsFunctionCtx(curContext())) context.pop_back();
        exprAllowed = true;
    } else if (t == &tt::backQuote) {
        if (curContext() == &tc::q_tmpl)
            context.pop_back();
        else
            context.push_back(&tc::q_tmpl);
        exprAllowed = false;
    } else if (t == &tt::star) {
        if (prevType == &tt::_function && !context.empty()) {
            const TokContext*& top = context.back();
            top = top == &tc::f_expr ? &tc::f_expr_gen : &tc::f_gen;
        }
        exprAllowed = true;
    } else if (t == &tt::name) {
        bool allowed = false;
        if (ecmaVersion >= 6 && prevType != &tt::dot) {
            if ((value == "of" && !exprAllowed) || (value == "yield" && inGeneratorContext())) allowed = true;
        }
        exprAllowed = allowed;
    } else if (t == &tt::jsxTagStart) {
        context.push_back(&tc::j_expr);
        context.push_back(&tc::j_oTag);
        exprAllowed = false;
    } else if (t == &tt::jsxTagEnd) {
        const TokContext* o = nullptr;
        if (!context.empty()) {
            o = context.back();
            context.pop_back();
        }
        if ((o == &tc::j_oTag && prevType == &tt::slash) || o == &tc::j_cTag) {
            if (!context.empty()) context.pop_back();
            exprAllowed = curContext() == &tc::j_expr;
        } else {
            exprAllowed = true;
        }
    } else {
        exprAllowed = t->beforeExpr;
    }
}

// ---------------------------------------------------------------- tokenizer

void Parser::next(bool ignoreEscapeSequenceInKeyword) {
    if (!ignoreEscapeSequenceInKeyword && type->keyword && containsEsc)
        raiseRecoverable(start, std::string("Escape sequence in keyword ") + type->keyword);
    if (collectTokens) {
        Token t;
        t.type = type->label;
        t.value = str(value);
        t.ustart = static_cast<uint32_t>(start);
        t.uend = static_cast<uint32_t>(end);
        t.start = out.ByteOffset(t.ustart);
        t.end = out.ByteOffset(t.uend);
        t.loc_start = startLoc;
        t.loc_end = endLoc;
        out.tokens.push_back(t);
    }
    lastTokEnd = end;
    lastTokStart = start;
    lastTokEndLoc = endLoc;
    lastTokStartLoc = startLoc;
    nextToken();
}

void Parser::nextToken() {
    const TokContext* cur = curContext();
    if (!cur || !cur->preserveSpace) skipSpace();
    start = pos;
    startLoc = curPosition();
    if (pos >= inputLen) return finishToken(tt::eof);
    if (cur && cur->overrideTemplate) return tryReadTemplateToken();
    readToken(fullCharCodeAtPos());
}

void Parser::readToken(int code) {
    if (IsIdentifierStart(code, ecmaVersion >= 6) || code == 92) return readWord();
    getTokenFromCode(code);
}

int Parser::fullCharCodeAtPos() const {
    int code = charAt(pos);
    if (code == -1) return kNaN;
    if (code <= 0xd7ff || code >= 0xdc00) return code;
    int nx = charAt(pos + 1);
    if (nx == -1) return kNaN;
    return nx <= 0xdbff || nx >= 0xe000 ? code : (code << 10) + nx - 0x35fdc00;
}

void Parser::skipBlockComment() {
    Pos sLoc = curPosition();
    int s = pos;
    int e = -1;
    pos += 2;
    for (int i = pos; i + 1 < inputLen; ++i) {
        if (input[static_cast<size_t>(i)] == '*' && input[static_cast<size_t>(i + 1)] == '/') {
            e = i;
            break;
        }
    }
    if (e == -1) raise(pos - 2, "Unterminated comment");
    pos = e + 2;
    for (int i = s; i < pos; ++i) {
        int c = input[static_cast<size_t>(i)];
        if (IsNewLine(c)) {
            int nb = (i < pos - 1 && c == 13 && input[static_cast<size_t>(i + 1)] == 10) ? i + 2 : i + 1;
            ++curLine;
            lineStart = nb;
            i = nb - 1;
        }
    }
    Comment c;
    c.block = true;
    c.value = str(sliceStr(s + 2, e));
    c.ustart = static_cast<uint32_t>(s);
    c.uend = static_cast<uint32_t>(pos);
    c.start = out.ByteOffset(c.ustart);
    c.end = out.ByteOffset(c.uend);
    c.loc_start = sLoc;
    c.loc_end = curPosition();
    out.comments.push_back(c);
}

void Parser::skipLineComment(int startSkip) {
    int s = pos;
    Pos sLoc = curPosition();
    pos += startSkip;
    int ch = charAt(pos);
    while (pos < inputLen && !IsNewLine(ch)) ch = charAt(++pos);
    Comment c;
    c.block = false;
    c.value = str(sliceStr(s + startSkip, pos));
    c.ustart = static_cast<uint32_t>(s);
    c.uend = static_cast<uint32_t>(pos);
    c.start = out.ByteOffset(c.ustart);
    c.end = out.ByteOffset(c.uend);
    c.loc_start = sLoc;
    c.loc_end = curPosition();
    out.comments.push_back(c);
}

void Parser::skipSpace() {
    while (pos < inputLen) {
        int ch = input[static_cast<size_t>(pos)];
        switch (ch) {
        case 32:
        case 160:
            ++pos;
            break;
        case 13:
            if (charAt(pos + 1) == 10) ++pos;
            [[fallthrough]];
        case 10:
        case 8232:
        case 8233:
            ++pos;
            ++curLine;
            lineStart = pos;
            break;
        case 47:
            switch (charAt(pos + 1)) {
            case 42:
                skipBlockComment();
                break;
            case 47:
                skipLineComment(2);
                break;
            default:
                return;
            }
            break;
        default:
            if ((ch > 8 && ch < 14) || (ch >= 5760 && IsNonAsciiWhitespace(ch)))
                ++pos;
            else
                return;
        }
    }
}

void Parser::finishToken(const TokenType& t) {
    end = pos;
    endLoc = curPosition();
    const TokenType* prevType = type;
    type = &t;
    value.clear();
    valueIsNull = true;
    updateContext(prevType);
}

void Parser::finishToken(const TokenType& t, std::u16string_view val) {
    end = pos;
    endLoc = curPosition();
    const TokenType* prevType = type;
    type = &t;
    value = ToWtf8(val);
    valueIsNull = false;
    updateContext(prevType);
}

void Parser::finishOp(const TokenType& t, int size) {
    std::u16string_view s = slice(pos, pos + size);
    pos += size;
    finishToken(t, s);
}

void Parser::readToken_dot() {
    int nx = charAt(pos + 1);
    if (nx >= 48 && nx <= 57) return readNumber(true);
    int nx2 = charAt(pos + 2);
    if (ecmaVersion >= 6 && nx == 46 && nx2 == 46) {
        pos += 3;
        return finishToken(tt::ellipsis);
    }
    ++pos;
    finishToken(tt::dot);
}

void Parser::readToken_slash() {
    int nx = charAt(pos + 1);
    if (exprAllowed) {
        ++pos;
        return readRegexp();
    }
    if (nx == 61) return finishOp(tt::assign, 2);
    finishOp(tt::slash, 1);
}

void Parser::readToken_mult_modulo_exp(int code) {
    int nx = charAt(pos + 1);
    int size = 1;
    const TokenType* tokentype = code == 42 ? &tt::star : &tt::modulo;
    if (ecmaVersion >= 7 && code == 42 && nx == 42) {
        ++size;
        tokentype = &tt::starstar;
        nx = charAt(pos + 2);
    }
    if (nx == 61) return finishOp(tt::assign, size + 1);
    finishOp(*tokentype, size);
}

void Parser::readToken_pipe_amp(int code) {
    int nx = charAt(pos + 1);
    if (nx == code) {
        if (ecmaVersion >= 12) {
            int nx2 = charAt(pos + 2);
            if (nx2 == 61) return finishOp(tt::assign, 3);
        }
        return finishOp(code == 124 ? tt::logicalOR : tt::logicalAND, 2);
    }
    if (nx == 61) return finishOp(tt::assign, 2);
    finishOp(code == 124 ? tt::bitwiseOR : tt::bitwiseAND, 1);
}

void Parser::readToken_caret() {
    int nx = charAt(pos + 1);
    if (nx == 61) return finishOp(tt::assign, 2);
    finishOp(tt::bitwiseXOR, 1);
}

void Parser::readToken_plus_min(int code) {
    int nx = charAt(pos + 1);
    if (nx == code) {
        if (nx == 45 && !inModule && charAt(pos + 2) == 62 && (lastTokEnd == 0 || lineBreakIn(lastTokEnd, pos))) {
            // A `-->` line comment
            skipLineComment(3);
            skipSpace();
            return nextToken();
        }
        return finishOp(tt::incDec, 2);
    }
    if (nx == 61) return finishOp(tt::assign, 2);
    finishOp(tt::plusMin, 1);
}

void Parser::readToken_lt_gt(int code) {
    int nx = charAt(pos + 1);
    int size = 1;
    if (nx == code) {
        size = code == 62 && charAt(pos + 2) == 62 ? 3 : 2;
        if (charAt(pos + size) == 61) return finishOp(tt::assign, size + 1);
        return finishOp(tt::bitShift, size);
    }
    if (nx == 33 && code == 60 && !inModule && charAt(pos + 2) == 45 && charAt(pos + 3) == 45) {
        // `<!--`, an XML-style comment that should be interpreted as a line comment
        skipLineComment(4);
        skipSpace();
        return nextToken();
    }
    if (nx == 61) size = 2;
    finishOp(tt::relational, size);
}

void Parser::readToken_eq_excl(int code) {
    int nx = charAt(pos + 1);
    if (nx == 61) return finishOp(tt::equality, charAt(pos + 2) == 61 ? 3 : 2);
    if (code == 61 && nx == 62 && ecmaVersion >= 6) {
        pos += 2;
        return finishToken(tt::arrow);
    }
    finishOp(code == 61 ? tt::eq : tt::prefix, 1);
}

void Parser::readToken_question() {
    if (ecmaVersion >= 11) {
        int nx = charAt(pos + 1);
        if (nx == 46) {
            int nx2 = charAt(pos + 2);
            // NaN (past the end) fails both comparisons in acorn
            if (nx2 != -1 && (nx2 < 48 || nx2 > 57)) return finishOp(tt::questionDot, 2);
        }
        if (nx == 63) {
            if (ecmaVersion >= 12) {
                int nx2 = charAt(pos + 2);
                if (nx2 == 61) return finishOp(tt::assign, 3);
            }
            return finishOp(tt::coalesce, 2);
        }
    }
    finishOp(tt::question, 1);
}

void Parser::readToken_numberSign() {
    int code = 35;
    if (ecmaVersion >= 13) {
        ++pos;
        code = fullCharCodeAtPos();
        if (IsIdentifierStart(code, true) || code == 92) {
            std::u16string w = readWord1();
            return finishToken(tt::privateId, w);
        }
    }
    raise(pos, "Unexpected character '" + CodePointToString(code) + "'");
}

void Parser::getTokenFromCode(int code) {
    switch (code) {
    case 46:
        return readToken_dot();
    case 40:
        ++pos;
        return finishToken(tt::parenL);
    case 41:
        ++pos;
        return finishToken(tt::parenR);
    case 59:
        ++pos;
        return finishToken(tt::semi);
    case 44:
        ++pos;
        return finishToken(tt::comma);
    case 91:
        ++pos;
        return finishToken(tt::bracketL);
    case 93:
        ++pos;
        return finishToken(tt::bracketR);
    case 123:
        ++pos;
        return finishToken(tt::braceL);
    case 125:
        ++pos;
        return finishToken(tt::braceR);
    case 58:
        ++pos;
        return finishToken(tt::colon);
    case 96:
        if (ecmaVersion < 6) break;
        ++pos;
        return finishToken(tt::backQuote);
    case 48: {
        int nx = charAt(pos + 1);
        if (nx == 120 || nx == 88) return readRadixNumber(16);
        if (ecmaVersion >= 6) {
            if (nx == 111 || nx == 79) return readRadixNumber(8);
            if (nx == 98 || nx == 66) return readRadixNumber(2);
        }
        return readNumber(false);
    }
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
    case 56:
    case 57:
        return readNumber(false);
    case 34:
    case 39:
        return readString(code);
    case 47:
        return readToken_slash();
    case 37:
    case 42:
        return readToken_mult_modulo_exp(code);
    case 124:
    case 38:
        return readToken_pipe_amp(code);
    case 94:
        return readToken_caret();
    case 43:
    case 45:
        return readToken_plus_min(code);
    case 60:
    case 62:
        return readToken_lt_gt(code);
    case 61:
    case 33:
        return readToken_eq_excl(code);
    case 63:
        return readToken_question();
    case 126:
        return finishOp(tt::prefix, 1);
    case 35:
        return readToken_numberSign();
    default:
        break;
    }
    raise(pos, "Unexpected character '" + CodePointToString(code) + "'");
}

void Parser::readRegexp() {
    bool escaped = false, inClass = false;
    int s = pos;
    for (;;) {
        if (pos >= inputLen) raise(s, "Unterminated regular expression");
        int ch = input[static_cast<size_t>(pos)];
        if (IsNewLine(ch)) raise(s, "Unterminated regular expression");
        if (!escaped) {
            if (ch == '[')
                inClass = true;
            else if (ch == ']' && inClass)
                inClass = false;
            else if (ch == '/' && !inClass)
                break;
            escaped = ch == '\\';
        } else {
            escaped = false;
        }
        ++pos;
    }
    std::u16string_view pattern = slice(s, pos);
    ++pos;
    int flagsStart = pos;
    std::u16string flags = readWord1();
    if (containsEsc) unexpected(flagsStart);
    regexPattern = ToWtf8(pattern);
    regexFlags = ToWtf8(flags);
    if (!skipRegExpValidation) validateRegExpFlagsAndPattern(s, pattern, flags);
    end = pos;
    endLoc = curPosition();
    const TokenType* prevType = type;
    type = &tt::regexp;
    value.clear();
    valueIsNull = false;
    updateContext(prevType);
}

bool Parser::readInt(int radix, int len, bool maybeLegacyOctal, double& result) {
    // len < 0 is acorn's `undefined`; a computed negative length (readCodePoint
    // with no closing brace) is passed as INT_MIN + n and reads nothing.
    bool lenGiven = len != -1;
    bool allowSeparators = ecmaVersion >= 12 && !lenGiven;
    bool isLegacyOctal = maybeLegacyOctal && charAt(pos) == 48;
    int s = pos;
    double total = 0;
    int lastCode = 0;
    long e = lenGiven ? len : LONG_MAX;
    for (long i = 0; i < e; ++i, ++pos) {
        int code = charAt(pos);
        if (code == -1) code = kNaN;
        double val;
        if (allowSeparators && code == 95) {
            if (isLegacyOctal)
                raiseRecoverable(pos, "Numeric separator is not allowed in legacy octal numeric literals");
            if (lastCode == 95) raiseRecoverable(pos, "Numeric separator must be exactly one underscore");
            if (i == 0) raiseRecoverable(pos, "Numeric separator is not allowed at the first of digits");
            lastCode = code;
            continue;
        }
        if (code >= 97)
            val = code - 97 + 10;
        else if (code >= 65)
            val = code - 65 + 10;
        else if (code >= 48 && code <= 57)
            val = code - 48;
        else
            val = HUGE_VAL;
        if (val >= radix) break;
        lastCode = code;
        total = total * radix + val;
    }
    if (allowSeparators && lastCode == 95)
        raiseRecoverable(pos - 1, "Numeric separator is not allowed at the last of digits");
    if (pos == s || (lenGiven && pos - s != len)) return false;
    result = total;
    return true;
}

// BigInt(str).toString() for a numeric literal's digits (prefix 0x/0o/0b or
// decimal, underscores already allowed).
static std::string BigIntDecimal(std::u16string_view s) {
    int radix = 10;
    size_t i = 0;
    if (s.size() >= 2 && s[0] == '0') {
        char16_t p = s[1];
        if (p == 'x' || p == 'X') radix = 16;
        if (p == 'o' || p == 'O') radix = 8;
        if (p == 'b' || p == 'B') radix = 2;
        if (radix != 10) i = 2;
    }
    std::vector<uint32_t> limbs;  // base 1e9, little-endian
    for (; i < s.size(); ++i) {
        char16_t c = s[i];
        if (c == '_') continue;
        uint32_t d = c >= 'a' ? static_cast<uint32_t>(c - 'a' + 10)
                     : c >= 'A' ? static_cast<uint32_t>(c - 'A' + 10)
                                : static_cast<uint32_t>(c - '0');
        uint64_t carry = d;
        for (uint32_t& l : limbs) {
            uint64_t v = static_cast<uint64_t>(l) * static_cast<uint64_t>(radix) + carry;
            l = static_cast<uint32_t>(v % 1000000000u);
            carry = v / 1000000000u;
        }
        while (carry) {
            limbs.push_back(static_cast<uint32_t>(carry % 1000000000u));
            carry /= 1000000000u;
        }
    }
    if (limbs.empty()) return "0";
    std::string out = std::to_string(limbs.back());
    for (size_t k = limbs.size() - 1; k-- > 0;) {
        std::string part = std::to_string(limbs[k]);
        out += std::string(9 - part.size(), '0') + part;
    }
    return out;
}

void Parser::readRadixNumber(int radix) {
    int s = pos;
    pos += 2;
    double val = 0;
    if (!readInt(radix, -1, false, val)) raise(start + 2, "Expected number in radix " + std::to_string(radix));
    bool big = false;
    if (ecmaVersion >= 11 && charAt(pos) == 110) {
        bigintValue = BigIntDecimal(slice(s, pos));
        big = true;
        ++pos;
    } else if (IsIdentifierStart(fullCharCodeAtPos())) {
        raise(pos, "Identifier directly after number");
    }
    finishToken(tt::num);
    numValue = val;
    numIsBigInt = big;
}

void Parser::readNumber(bool startsWithDot) {
    int s = pos;
    double ignored = 0;
    if (!startsWithDot && !readInt(10, -1, true, ignored)) raise(s, "Invalid number");
    bool octal = pos - s >= 2 && charAt(s) == 48;
    if (octal && strict) raise(s, "Invalid number");
    int nx = charAt(pos);
    if (!octal && !startsWithDot && ecmaVersion >= 11 && nx == 110) {
        std::string big = BigIntDecimal(slice(s, pos));
        ++pos;
        if (IsIdentifierStart(fullCharCodeAtPos())) raise(pos, "Identifier directly after number");
        finishToken(tt::num);
        numIsBigInt = true;
        bigintValue = big;
        numValue = 0;
        return;
    }
    if (octal) {
        for (int i = s; i < pos; ++i) {
            int c = input[static_cast<size_t>(i)];
            if (c == '8' || c == '9') {
                octal = false;
                break;
            }
        }
    }
    if (nx == 46 && !octal) {
        ++pos;
        readInt(10, -1, false, ignored);
        nx = charAt(pos);
    }
    if ((nx == 69 || nx == 101) && !octal) {
        nx = charAt(++pos);
        if (nx == 43 || nx == 45) ++pos;
        if (!readInt(10, -1, false, ignored)) raise(s, "Invalid number");
    }
    if (IsIdentifierStart(fullCharCodeAtPos())) raise(pos, "Identifier directly after number");

    std::u16string_view text = slice(s, pos);
    double val = 0;
    if (octal) {
        // parseInt(str, 8)
        for (char16_t c : text) {
            if (c < '0' || c > '7') break;
            val = val * 8 + (c - '0');
        }
    } else {
        std::string a;
        for (char16_t c : text)
            if (c != '_') a += static_cast<char>(c);
        // parseFloat: correctly rounded, independent of the process locale
        static locale_t cLocale = newlocale(LC_ALL_MASK, "C", nullptr);
        val = strtod_l(a.c_str(), nullptr, cLocale);
    }
    finishToken(tt::num);
    numValue = val;
    numIsBigInt = false;
}

int Parser::readCodePoint() {
    int ch = charAt(pos);
    int code;
    if (ch == 123) {
        if (ecmaVersion < 6) unexpected();
        int codePos = ++pos;
        int close = -1;
        for (int i = pos; i < inputLen; ++i) {
            if (input[static_cast<size_t>(i)] == '}') {
                close = i;
                break;
            }
        }
        int len = close - pos;  // -1 - pos when there is none: reads nothing
        double n = 0;
        if (len < 0 || !readInt(16, len, false, n)) {
            invalidStringToken(codePos, "Bad character escape sequence");
            n = 0;
        }
        ++pos;
        if (n > 0x10FFFF) invalidStringToken(codePos, "Code point out of bounds");
        code = static_cast<int>(n);
    } else {
        code = readHexChar(4);
    }
    return code;
}

int Parser::readHexChar(int len) {
    int codePos = pos;
    double n = 0;
    if (!readInt(16, len, false, n)) invalidStringToken(codePos, "Bad character escape sequence");
    return static_cast<int>(n);
}

void Parser::readString(int quote) {
    std::u16string o;
    int chunkStart = ++pos;
    for (;;) {
        if (pos >= inputLen) raise(start, "Unterminated string constant");
        int ch = input[static_cast<size_t>(pos)];
        if (ch == quote) break;
        if (ch == 92) {
            o += slice(chunkStart, pos);
            readEscapedChar(false, o);
            chunkStart = pos;
        } else if (ch == 0x2028 || ch == 0x2029) {
            if (ecmaVersion < 10) raise(start, "Unterminated string constant");
            ++pos;
            curLine++;
            lineStart = pos;
        } else {
            if (IsNewLine(ch)) raise(start, "Unterminated string constant");
            ++pos;
        }
    }
    o += slice(chunkStart, pos++);
    finishToken(tt::string, o);
}

struct InvalidTemplateEscape {};

void Parser::tryReadTemplateToken() {
    inTemplateElement = true;
    try {
        readTmplToken();
    } catch (const InvalidTemplateEscape&) {
        readInvalidTemplateToken();
    }
    inTemplateElement = false;
}

void Parser::invalidStringToken(int position, const std::string& message) {
    if (inTemplateElement && ecmaVersion >= 9) throw InvalidTemplateEscape{};
    raise(position, message);
}

void Parser::readTmplToken() {
    std::u16string o;
    int chunkStart = pos;
    for (;;) {
        if (pos >= inputLen) raise(start, "Unterminated template");
        int ch = input[static_cast<size_t>(pos)];
        if (ch == 96 || (ch == 36 && charAt(pos + 1) == 123)) {
            if (pos == start && (type == &tt::template_ || type == &tt::invalidTemplate)) {
                if (ch == 36) {
                    pos += 2;
                    return finishToken(tt::dollarBraceL);
                }
                ++pos;
                return finishToken(tt::backQuote);
            }
            o += slice(chunkStart, pos);
            return finishToken(tt::template_, o);
        }
        if (ch == 92) {
            o += slice(chunkStart, pos);
            readEscapedChar(true, o);
            chunkStart = pos;
        } else if (IsNewLine(ch)) {
            o += slice(chunkStart, pos);
            ++pos;
            switch (ch) {
            case 13:
                if (charAt(pos) == 10) ++pos;
                [[fallthrough]];
            case 10:
                o += u'\n';
                break;
            default:
                o += static_cast<char16_t>(ch);
                break;
            }
            ++curLine;
            lineStart = pos;
            chunkStart = pos;
        } else {
            ++pos;
        }
    }
}

void Parser::readInvalidTemplateToken() {
    for (; pos < inputLen; pos++) {
        int c = input[static_cast<size_t>(pos)];
        switch (c) {
        case '\\':
            ++pos;
            break;
        case '$':
            if (charAt(pos + 1) != '{') break;
            [[fallthrough]];
        case '`':
            return finishToken(tt::invalidTemplate, slice(start, pos));
        case '\r':
            if (charAt(pos + 1) == '\n') ++pos;
            [[fallthrough]];
        case '\n':
        case 0x2028:
        case 0x2029:
            ++curLine;
            lineStart = pos + 1;
            break;
        default:
            break;
        }
    }
    raise(start, "Unterminated template");
}

void Parser::readEscapedChar(bool inTemplate, std::u16string& o) {
    int ch = charAt(++pos);
    ++pos;
    switch (ch) {
    case 110:
        o += u'\n';
        return;
    case 114:
        o += u'\r';
        return;
    case 120:
        o += static_cast<char16_t>(readHexChar(2));
        return;
    case 117:
        AppendCodePoint16(o, readCodePoint());
        return;
    case 116:
        o += u'\t';
        return;
    case 98:
        o += u'\b';
        return;
    case 118:
        o += u'\v';
        return;
    case 102:
        o += u'\f';
        return;
    case 13:
        if (charAt(pos) == 10) ++pos;
        [[fallthrough]];
    case 10:
        lineStart = pos;
        ++curLine;
        return;
    case 56:
    case 57:
        if (strict) invalidStringToken(pos - 1, "Invalid escape sequence");
        if (inTemplate) invalidStringToken(pos - 1, "Invalid escape sequence in template string");
        [[fallthrough]];
    default:
        if (ch >= 48 && ch <= 55) {
            int len = 0;
            while (len < 3 && charAt(pos - 1 + len) >= '0' && charAt(pos - 1 + len) <= '7') ++len;
            int octal = 0;
            for (int i = 0; i < len; ++i) octal = octal * 8 + (charAt(pos - 1 + i) - '0');
            if (octal > 255) {
                --len;
                octal /= 8;
            }
            pos += len - 1;
            ch = charAt(pos);
            bool isZero = len == 1 && charAt(pos - 1) == '0';
            if ((!isZero || ch == 56 || ch == 57) && (strict || inTemplate))
                invalidStringToken(pos - 1 - len,
                                   inTemplate ? "Octal literal in template string" : "Octal literal in strict mode");
            o += static_cast<char16_t>(octal);
            return;
        }
        if (IsNewLine(ch)) {
            lineStart = pos;
            ++curLine;
            return;
        }
        o += static_cast<char16_t>(ch == -1 ? 0 : ch);
        return;
    }
}

std::u16string Parser::readWord1() {
    containsEsc = false;
    std::u16string word;
    bool first = true;
    int chunkStart = pos;
    bool astral = ecmaVersion >= 6;
    while (pos < inputLen) {
        int ch = fullCharCodeAtPos();
        if (IsIdentifierChar(ch, astral)) {
            pos += ch <= 0xffff ? 1 : 2;
        } else if (ch == 92) {
            containsEsc = true;
            word += slice(chunkStart, pos);
            int escStart = pos;
            if (charAt(++pos) != 117) invalidStringToken(pos, "Expecting Unicode escape sequence \\uXXXX");
            ++pos;
            int esc = readCodePoint();
            if (!(first ? IsIdentifierStart(esc, astral) : IsIdentifierChar(esc, astral)))
                invalidStringToken(escStart, "Invalid Unicode escape");
            AppendCodePoint16(word, esc);
            chunkStart = pos;
        } else {
            break;
        }
        first = false;
    }
    word += slice(chunkStart, pos);
    return word;
}

void Parser::readWord() {
    std::u16string word = readWord1();
    const TokenType* t = &tt::name;
    std::string w = ToWtf8(word);
    if (isKeywordWord(w)) t = tt::Keyword(w);
    finishToken(*t, word);
}

}  // namespace jsparse
