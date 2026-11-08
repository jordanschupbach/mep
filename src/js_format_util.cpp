// Prettier 3.6.2 helpers: src/utils/{skip,has-newline,is-next-line-empty,...}.js,
// src/language-js/utils/index.js and src/language-js/needs-parens.js.

#include <cstring>

#include "js_format_internal.h"

namespace jsfmt {

// ---------------------------------------------------------------- fields

static bool HasBodyList(NT t) {
    switch (t) {
        case NT::Program:
        case NT::BlockStatement:
        case NT::ClassBody:
        case NT::StaticBlock:
        case NT::TSInterfaceBody:
        case NT::TSModuleBlock:
            return true;
        default:
            return false;
    }
}

Node* Child(const Node* n, Key k) {
    if (!n) return nullptr;
    switch (k) {
        case Key::id:
        case Key::left:
        case Key::test:
        case Key::object:
        case Key::callee:
        case Key::key:
        case Key::block:
        case Key::discriminant:
        case Key::tag:
        case Key::label:
        case Key::argument:
        case Key::expression:
        case Key::meta:
        case Key::openingElement:
        case Key::openingFragment:
        case Key::local:
        case Key::param:
        case Key::declaration:
        case Key::name:
        case Key::typeName:
        case Key::objectType:
        case Key::checkType:
        case Key::typeParameter:
        case Key::literal:
        case Key::exprName:
        case Key::parameterName:
        case Key::parameter:
        case Key::qualifier:
            return n->kid[0];
        case Key::init:
        case Key::right:
        case Key::property:
        case Key::value:
        case Key::handler:
        case Key::quasi:
        case Key::superClass:
        case Key::source:
        case Key::closingElement:
        case Key::closingFragment:
        case Key::nspace:
        case Key::elementType:
        case Key::indexType:
        case Key::extendsType:
        case Key::constraint:
        case Key::initializer:
        case Key::moduleReference:
            return n->kid[1];
        case Key::consequent:
            return n->type == NT::SwitchCase ? nullptr : n->kid[1];
        case Key::update:
        case Key::alternate:
        case Key::finalizer:
        case Key::options:
        case Key::exported:
        case Key::imported:
        case Key::trueType:
        case Key::defaultType:
        case Key::nameType:
            return n->kid[2];
        case Key::falseType:
            return n->kid[3];
        case Key::body:
            return HasBodyList(n->type) ? nullptr : n->kid[3];
        case Key::typeAnnotation:
            return n->ts[0];
        case Key::typeParameters:
            return n->ts[1];
        case Key::returnType:
            return n->ts[2];
        case Key::typeArguments:
            return n->ts[3];
        default:
            return nullptr;
    }
}

const NodeList* ListOf(const Node* n, Key k) {
    if (!n) return nullptr;
    switch (k) {
        case Key::body:
            return HasBodyList(n->type) ? &n->list[0] : nullptr;
        case Key::consequent:
            return n->type == NT::SwitchCase ? &n->list[0] : nullptr;
        case Key::params:
        case Key::elements:
        case Key::properties:
        case Key::arguments:
        case Key::quasis:
        case Key::declarations:
        case Key::cases:
        case Key::specifiers:
        case Key::children:
        case Key::types:
        case Key::elementTypes:
        case Key::members:
        case Key::parameters:
        case Key::extends_:
            return &n->list[0];
        case Key::expressions:
        case Key::attributes:
        case Key::template_types:
            return &n->list[1];
        case Key::decorators:
            return &n->tslist[0];
        case Key::implements:
            return &n->tslist[1];
        default:
            return nullptr;
    }
}

// ---------------------------------------------------------------- small helpers

bool IsMethod(const Node* n) {
    return (n->Has(jsparse::kFlagMethod) && n->kind == "init") || n->kind == "get" || n->kind == "set";
}

bool IsObjectProperty(const Node* n) { return n && n->type == NT::Property && !IsMethod(n); }

int GetPrecedence(std::string_view op) {
    static const char* const kLevels[][6] = {
        {"|>"},        {"??"},       {"||"},         {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "===", "!=", "!=="},
        {"<", ">", "<=", ">=", "in", "instanceof"}, {">>", "<<", ">>>"}, {"+", "-"}, {"*", "/", "%"}, {"**"},
    };
    for (int i = 0; i < 13; i++) {
        for (const char* o : kLevels[i]) {
            if (o && op == o) return i;
        }
    }
    return -1;  // undefined
}

static bool IsEquality(std::string_view op) { return op == "==" || op == "!=" || op == "===" || op == "!=="; }
static bool IsMultiplicative(std::string_view op) { return op == "*" || op == "/" || op == "%"; }
static bool IsBitshift(std::string_view op) { return op == ">>" || op == ">>>" || op == "<<"; }

bool ShouldFlatten(std::string_view parent_op, std::string_view node_op) {
    if (GetPrecedence(node_op) != GetPrecedence(parent_op)) return false;
    if (parent_op == "**") return false;
    if (IsEquality(parent_op) && IsEquality(node_op)) return false;
    if ((node_op == "%" && IsMultiplicative(parent_op)) || (parent_op == "%" && IsMultiplicative(node_op))) {
        return false;
    }
    if (node_op != parent_op && IsMultiplicative(node_op) && IsMultiplicative(parent_op)) return false;
    if (IsBitshift(parent_op) && IsBitshift(node_op)) return false;
    return true;
}

bool IsBitwiseOperator(std::string_view op) { return IsBitshift(op) || op == "|" || op == "^" || op == "&"; }

bool IsBlockComment(const CommentRec* c) { return c->block; }
bool IsLineComment(const CommentRec* c) { return !c->block; }

bool IsTypeCastComment(const CommentRec* c) {
    if (!c->block || c->value.empty() || c->value[0] != '*') return false;
    for (std::string_view word : {std::string_view("@type"), std::string_view("@satisfies")}) {
        size_t pos = 0;
        while ((pos = c->value.find(word, pos)) != std::string_view::npos) {
            const size_t after = pos + word.size();
            const int ch = after < c->value.size() ? static_cast<unsigned char>(c->value[after]) : -1;
            const bool word_char = ch >= 0 && (std::isalnum(ch) || ch == '_');
            if (!word_char) return true;
            pos = after;
        }
    }
    return false;
}

namespace {

// JavaScript's WhiteSpace + LineTerminator (String.prototype.trim).
size_t JsSpaceAt(std::string_view s, size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') return 1;
    if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) return 2;
    if (c == 0xEF && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xBB &&
        static_cast<unsigned char>(s[i + 2]) == 0xBF) {
        return 3;
    }
    if (c == 0xE1 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x9A &&
        static_cast<unsigned char>(s[i + 2]) == 0x80) {
        return 3;
    }
    if (c == 0xE2 && i + 2 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        const auto b2 = static_cast<unsigned char>(s[i + 2]);
        if (b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xA8 || b2 == 0xA9 || b2 == 0xAF)) return 3;
        if (b1 == 0x81 && b2 == 0x9F) return 3;
    }
    if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
        static_cast<unsigned char>(s[i + 2]) == 0x80) {
        return 3;
    }
    return 0;
}

}  // namespace

std::string_view TrimJs(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t w = JsSpaceAt(s, i);
        if (!w) break;
        i += w;
    }
    s.remove_prefix(i);
    // trailing: walk back to a char boundary and test
    while (!s.empty()) {
        size_t start = s.size() - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
        const size_t w = JsSpaceAt(s, start);
        if (w && start + w == s.size()) {
            s.remove_suffix(w);
        } else {
            break;
        }
    }
    return s;
}

static std::string_view TrimStartJs(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const size_t w = JsSpaceAt(s, i);
        if (!w) break;
        i += w;
    }
    return s.substr(i);
}

bool IsIndentableBlockComment(const CommentRec* c) {
    if (!c->block) return false;
    const std::string v = "*" + std::string(c->value) + "*";
    std::string_view rest = v;
    int lines = 0;
    while (true) {
        const size_t nl = rest.find('\n');
        const std::string_view ln = rest.substr(0, nl);
        const std::string_view t = TrimStartJs(ln);
        if (t.empty() || t[0] != '*') return false;
        lines++;
        if (nl == std::string_view::npos) break;
        rest.remove_prefix(nl + 1);
    }
    return lines > 1;
}

bool IsPrettierIgnoreComment(const CommentRec* c) { return TrimJs(c->value) == "prettier-ignore" && !c->unignore; }

std::string_view Raw(const Node* n) { return n->str; }

std::string PrintNumber(std::string_view raw) {
    if (raw.size() == 1) return std::string(raw);
    std::string s(raw);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto is_digit = [&](size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
    auto head_end = [&](size_t& p) -> size_t {  // ^[+-]?[\d.]+ ; returns end, p = after sign
        p = 0;
        if (!s.empty() && (s[0] == '+' || s[0] == '-')) p = 1;
        size_t q = p;
        while (q < s.size() && (is_digit(q) || s[q] == '.')) q++;
        return q;
    };
    // Remove unnecessary plus and zeroes from scientific notation.
    {
        size_t p;
        const size_t q = head_end(p);
        if (q > p && q < s.size() && s[q] == 'e') {
            size_t r = q + 1;
            std::string sign;
            if (r < s.size() && s[r] == '+') {
                r++;
            } else if (r < s.size() && s[r] == '-') {
                sign = "-";
                r++;
            }
            size_t z = r;
            while (z < s.size() && s[z] == '0') z++;
            bool ok = true;
            if (!is_digit(z)) {
                if (z > r) {
                    z--;
                } else {
                    ok = false;
                }
            }
            if (ok) s = s.substr(0, q + 1) + sign + s.substr(z);
        }
    }
    // Remove unnecessary scientific notation (1e0).
    {
        size_t p;
        const size_t q = head_end(p);
        if (q > p && q < s.size() && s[q] == 'e') {
            size_t r = q + 1;
            if (r < s.size() && (s[r] == '+' || s[r] == '-')) r++;
            size_t z = r;
            while (z < s.size() && s[z] == '0') z++;
            if (z > r && z == s.size()) s = s.substr(0, q);
        }
    }
    // Make sure numbers always start with a digit.
    if (!s.empty() && s[0] == '.') {
        s = "0" + s;
    } else if (s.size() >= 2 && (s[0] == '+' || s[0] == '-') && s[1] == '.') {
        s = s.substr(0, 1) + "0" + s.substr(1);
    }
    // Remove extraneous trailing decimal zeroes.
    {
        const size_t dot = s.find('.');
        if (dot != std::string::npos) {
            size_t e = dot + 1;
            while (is_digit(e)) e++;
            const size_t run = e - dot - 1;
            if (run >= 2 && (e == s.size() || s[e] == 'e')) {
                size_t zeros = 0;
                while (zeros < run && s[e - 1 - zeros] == '0') zeros++;
                const size_t keep = std::max<size_t>(1, run - zeros);
                if (keep < run) s.erase(dot + 1 + keep, run - keep);
            }
        }
    }
    // Remove trailing dot.
    {
        const size_t dot = s.find('.');
        if (dot != std::string::npos && (dot + 1 == s.size() || s[dot + 1] == 'e')) s.erase(dot, 1);
    }
    return s;
}

// ---------------------------------------------------------------- text helpers

int64_t Printer::SkipSpaces(int64_t i, bool backwards) const {
    if (i == kNoIndex) return kNoIndex;
    const auto len = static_cast<int64_t>(text_.size());
    int64_t cursor = i;
    while (cursor >= 0 && cursor < len) {
        const char c = text_[static_cast<size_t>(cursor)];
        if (c != ' ' && c != '\t') return cursor;
        cursor += backwards ? -1 : 1;
    }
    if (cursor == -1 || cursor == len) return cursor;
    return kNoIndex;
}

int64_t Printer::SkipToLineEnd(int64_t i) const {
    if (i == kNoIndex) return kNoIndex;
    const auto len = static_cast<int64_t>(text_.size());
    int64_t cursor = i;
    while (cursor >= 0 && cursor < len) {
        const char c = text_[static_cast<size_t>(cursor)];
        if (c != ',' && c != ';' && c != ' ' && c != '\t') return cursor;
        cursor++;
    }
    if (cursor == -1 || cursor == len) return cursor;
    return kNoIndex;
}

int64_t Printer::SkipEverythingButNewLine(int64_t i) const {
    if (i == kNoIndex) return kNoIndex;
    const auto len = static_cast<int64_t>(text_.size());
    int64_t cursor = i;
    while (cursor >= 0 && cursor < len) {
        const char c = text_[static_cast<size_t>(cursor)];
        if (c == '\n' || c == '\r') return cursor;
        cursor++;
    }
    if (cursor == -1 || cursor == len) return cursor;
    return kNoIndex;
}

int64_t Printer::SkipNewline(int64_t i, bool backwards) const {
    if (i == kNoIndex) return kNoIndex;
    const int c = CharAt(i);
    if (backwards) {
        if (CharAt(i - 1) == '\r' && c == '\n') return i - 2;
        if (c == '\n' || c == '\r') return i - 1;
        if ((c == 0xA8 || c == 0xA9) && CharAt(i - 1) == 0x80 && CharAt(i - 2) == 0xE2) return i - 3;
    } else {
        if (c == '\r' && CharAt(i + 1) == '\n') return i + 2;
        if (c == '\n' || c == '\r') return i + 1;
        if (c == 0xE2 && CharAt(i + 1) == 0x80 && (CharAt(i + 2) == 0xA8 || CharAt(i + 2) == 0xA9)) return i + 3;
    }
    return i;
}

int64_t Printer::SkipInlineComment(int64_t i) const {
    if (i == kNoIndex) return kNoIndex;
    if (CharAt(i) == '/' && CharAt(i + 1) == '*') {
        const size_t close = text_.find("*/", static_cast<size_t>(i + 2));
        if (close != std::string_view::npos) return static_cast<int64_t>(close) + 2;
    }
    return i;
}

int64_t Printer::SkipTrailingComment(int64_t i) const {
    if (i == kNoIndex) return kNoIndex;
    if (CharAt(i) == '/' && CharAt(i + 1) == '/') return SkipEverythingButNewLine(i);
    return i;
}

bool Printer::HasNewline(int64_t i, bool backwards) const {
    const int64_t idx = SkipSpaces(backwards ? i - 1 : i, backwards);
    const int64_t idx2 = SkipNewline(idx, backwards);
    return idx != idx2;
}

bool Printer::HasNewlineInRange(int64_t start, int64_t end) const {
    for (int64_t i = std::max<int64_t>(start, 0); i < end && i < static_cast<int64_t>(text_.size()); ++i) {
        if (text_[static_cast<size_t>(i)] == '\n') return true;
    }
    return false;
}

bool Printer::IsNextLineEmptyAfterIndex(int64_t i) const {
    int64_t old_idx = 0;
    int64_t idx = i;
    bool first = true;
    while (first || idx != old_idx) {
        first = false;
        old_idx = idx;
        idx = SkipToLineEnd(idx);
        idx = SkipInlineComment(idx);
        idx = SkipSpaces(idx);
    }
    idx = SkipTrailingComment(idx);
    idx = SkipNewline(idx);
    return idx != kNoIndex && HasNewline(idx);
}

bool Printer::IsPreviousLineEmpty(int64_t start) const {
    int64_t idx = start - 1;
    idx = SkipSpaces(idx, true);
    idx = SkipNewline(idx, true);
    idx = SkipSpaces(idx, true);
    const int64_t idx2 = SkipNewline(idx, true);
    return idx != idx2;
}

int64_t Printer::GetNextNonSpaceNonCommentCharacterIndex(int64_t i) const {
    int64_t old_idx = 0;
    int64_t next = i;
    bool first = true;
    while (first || next != old_idx) {
        first = false;
        old_idx = next;
        next = SkipSpaces(next);
        next = SkipInlineComment(next);
        next = SkipTrailingComment(next);
        next = SkipNewline(next);
    }
    return next;
}

std::string Printer::GetTextWithoutComments(uint32_t start, uint32_t end) const {
    std::string text(text_.substr(start, end - start));
    for (const CommentRec* c : all_comments_) {
        if (c->start > end) break;
        if (c->end < start) continue;
        for (uint32_t k = c->start; k < c->end; k++) {
            if (k >= start && k < end) text[k - start] = ' ';
        }
    }
    return text;
}

bool Printer::IsLineAfterElementEmpty(const Node* n) const {
    int64_t cur = n->end;
    const auto len = static_cast<int64_t>(text_.size());
    while (cur >= 0 && cur < len && text_[static_cast<size_t>(cur)] != ',') {
        cur = SkipInlineComment(SkipTrailingComment(cur + 1));
        if (cur == kNoIndex) return false;
    }
    return IsNextLineEmptyAfterIndex(cur);
}

// ---------------------------------------------------------------- AstPath

bool Printer::Match(std::initializer_list<Pred> preds) const {
    size_t depth = 0;
    for (const Pred& p : preds) {
        if (depth >= stack_.size()) return false;
        const Frame& f = stack_[stack_.size() - 1 - depth];
        const Key child_key = depth == 0 ? Key::None : stack_[stack_.size() - depth].key;
        if (p && !p(f.node, child_key)) return false;
        depth++;
    }
    return true;
}

Node* Printer::FindAncestor(const std::function<bool(Node*)>& pred) const {
    for (size_t i = stack_.size() - 1; i-- > 0;) {
        if (stack_[i].node && pred(stack_[i].node)) return stack_[i].node;
    }
    return nullptr;
}

// ---------------------------------------------------------------- comments queries

std::vector<CommentRec*>* Printer::CommentsOf(const Node* n) {
    if (!n) return nullptr;
    auto it = comments_of_.find(n);
    return it == comments_of_.end() ? nullptr : &it->second;
}

bool Printer::Parenthesized(const Node* n) const { return parenthesized_.count(n) != 0; }
void Printer::SetParenthesized(const Node* n) { parenthesized_[n] = true; }

static bool CommentTest(const CommentRec* c, size_t index, size_t count, uint32_t flags,
                        const std::function<bool(CommentRec*)>& fn) {
    if ((flags & kCLeading) && !c->leading) return false;
    if ((flags & kCTrailing) && !c->trailing) return false;
    if ((flags & kCDangling) && (c->leading || c->trailing)) return false;
    if ((flags & kCBlock) && !IsBlockComment(c)) return false;
    if ((flags & kCLine) && !IsLineComment(c)) return false;
    if ((flags & kCFirst) && index != 0) return false;
    if ((flags & kCLast) && index != count - 1) return false;
    if ((flags & kCPrettierIgnore) && !IsPrettierIgnoreComment(c)) return false;
    if (fn && !fn(const_cast<CommentRec*>(c))) return false;
    return true;
}

bool Printer::HasComment(const Node* n, uint32_t flags, const std::function<bool(CommentRec*)>& fn) {
    auto* cs = CommentsOf(n);
    if (!cs || cs->empty()) return false;
    if (!flags && !fn) return true;
    for (size_t i = 0; i < cs->size(); i++) {
        if (CommentTest((*cs)[i], i, cs->size(), flags, fn)) return true;
    }
    return false;
}

std::vector<CommentRec*> Printer::GetComments(const Node* n, uint32_t flags,
                                              const std::function<bool(CommentRec*)>& fn) {
    std::vector<CommentRec*> out;
    auto* cs = CommentsOf(n);
    if (!cs) return out;
    for (size_t i = 0; i < cs->size(); i++) {
        if ((!flags && !fn) || CommentTest((*cs)[i], i, cs->size(), flags, fn)) out.push_back((*cs)[i]);
    }
    return out;
}

bool Printer::HasNodeIgnoreComment(const Node* n) {
    return n && (prettier_ignore_.count(n) != 0 || HasComment(n, kCPrettierIgnore));
}

bool Printer::HasLeadingOwnLineComment(const Node* n) {
    if (IsJsxElement(n)) return HasNodeIgnoreComment(n);
    return HasComment(n, kCLeading, [&](CommentRec* c) { return HasNewline(c->end); });
}

bool Printer::NeedsHardlineAfterDanglingComment(const Node* n) {
    if (!HasComment(n)) return false;
    auto dangling = GetComments(n, kCDangling);
    return !dangling.empty() && !IsBlockComment(dangling.back());
}

bool Printer::IsMeaningfulJsxText(const Node* n) const {
    if (!Is(n, NT::JSXText)) return false;
    const std::string_view raw = Raw(n);
    bool has_non_ws = false;
    for (char c : raw) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') {
            has_non_ws = true;
            break;
        }
    }
    return has_non_ws || raw.find('\n') == std::string_view::npos;
}

bool Printer::HasJsxIgnoreComment() {
    Node* n = node();
    Node* p = parent();
    if (!IsJsxElement(n) || !IsJsxElement(p)) return false;
    const Frame& f = stack_.back();
    if (!f.list) return false;
    Node* prev = nullptr;
    for (int i = f.index; i > 0; i--) {
        Node* candidate = (*f.list)[static_cast<size_t>(i) - 1];
        if (Is(candidate, NT::JSXText) && !IsMeaningfulJsxText(candidate)) continue;
        prev = candidate;
        break;
    }
    return Is(prev, NT::JSXExpressionContainer) && Is(prev->expression(), NT::JSXEmptyExpression) &&
           HasNodeIgnoreComment(prev->expression());
}

bool Printer::IsIgnored() { return HasNodeIgnoreComment(node()) || HasJsxIgnoreComment(); }

bool Printer::WillPrintOwnComments() {
    Node* n = node();
    Node* p = parent();
    return (IsJsxElement(n) ||
            (p && (p->type == NT::JSXSpreadAttribute || p->type == NT::JSXSpreadChild || IsUnionType(p) ||
                   ((p->type == NT::ClassDeclaration || p->type == NT::ClassExpression) && p->superClass() == n)))) &&
           (!HasNodeIgnoreComment(n) || (p && IsUnionType(p)));
}

// ---------------------------------------------------------------- utils/index.js

bool Printer::IsCallExpression(const Node* n) const {
    if (Is(n, NT::ChainExpression)) n = n->expression();
    return Is(n, NT::CallExpression);
}

bool Printer::IsMemberExpression(const Node* n) const {
    if (Is(n, NT::ChainExpression)) n = n->expression();
    return Is(n, NT::MemberExpression);
}

bool Printer::IsCallLikeExpression(const Node* n) const {
    return IsCallExpression(n) || Is(n, NT::NewExpression) || Is(n, NT::ImportExpression);
}

std::vector<Node*> Printer::GetCallArguments(const Node* n) const {
    if (Is(n, NT::ChainExpression)) return GetCallArguments(n->expression());
    std::vector<Node*> args;
    if (Is(n, NT::ImportExpression) || Is(n, NT::TSImportType)) {
        args.push_back(n->source());
        if (n->options()) args.push_back(n->options());
        return args;
    }
    for (Node* a : n->arguments()) args.push_back(a);
    return args;
}

Doc* Printer::PrintCallArgument(int index, const Args* a) {
    Node* n = node();
    if (n->type == NT::ChainExpression) {
        return Call(Key::expression, [&] { return PrintCallArgument(index, a); });
    }
    if (n->type == NT::ImportExpression || n->type == NT::TSImportType) {
        const bool has_opts = n->options() != nullptr;
        Key k = (index == 0 || index == (has_opts ? -2 : -1)) ? Key::source : Key::options;
        return Call(k, [&] { return MainPrint(a); });
    }
    if (index < 0) index += static_cast<int>(n->arguments().size());
    return CallAt(Key::arguments, index, [&] { return MainPrint(a); });
}

std::vector<Node*> Printer::GetFunctionParameters(const Node* n) const {
    std::vector<Node*> ps;
    for (Node* p : n->params()) ps.push_back(p);
    return ps;
}

bool Printer::HasRestParameter(const Node* n) const {
    const auto& ps = n->params();
    return !ps.empty() && Is(ps[ps.size() - 1], NT::RestElement);
}

bool Printer::IsSimpleCallArgument(const Node* n, int depth) {
    if (depth <= 0 || !n) return false;
    if (n->type == NT::ChainExpression || n->type == NT::TSNonNullExpression) {
        return IsSimpleCallArgument(n->expression(), depth);
    }
    auto child_simple = [&](const Node* c) { return IsSimpleCallArgument(c, depth - 1); };
    if (IsRegExpLiteral(n)) return StringWidth(n->str2) <= 5;
    if (IsLiteral(n) || n->type == NT::Identifier || n->type == NT::ThisExpression || n->type == NT::Super ||
        n->type == NT::PrivateIdentifier) {
        return true;
    }
    if (n->type == NT::TemplateLiteral) {
        for (Node* q : n->quasis()) {
            if (Raw(q).find('\n') != std::string_view::npos) return false;
        }
        for (Node* e : n->expressions()) {
            if (!child_simple(e)) return false;
        }
        return true;
    }
    if (n->type == NT::ObjectExpression) {
        for (Node* p : n->properties()) {
            if (p->type != NT::Property) return false;  // SpreadElement: no shorthand, no value
            if (p->Has(jsparse::kFlagComputed)) return false;
            if (!(p->Has(jsparse::kFlagShorthand) || (p->value() && child_simple(p->value())))) return false;
        }
        return true;
    }
    if (n->type == NT::ArrayExpression) {
        for (Node* e : n->elements()) {
            if (e && !child_simple(e)) return false;
        }
        return true;
    }
    if (IsCallLikeExpression(n)) {
        if (n->type == NT::ImportExpression || IsSimpleCallArgument(n->callee(), depth)) {
            const auto args = GetCallArguments(n);
            if (static_cast<int>(args.size()) > depth) return false;
            for (Node* a : args) {
                if (!child_simple(a)) return false;
            }
            return true;
        }
        return false;
    }
    if (IsMemberExpression(n)) {
        return IsSimpleCallArgument(n->object(), depth) && IsSimpleCallArgument(n->property(), depth);
    }
    if ((n->type == NT::UnaryExpression && (n->op == "!" || n->op == "-" || n->op == "+" || n->op == "~")) ||
        n->type == NT::UpdateExpression) {
        return IsSimpleCallArgument(n->argument(), depth);
    }
    return false;
}

bool Printer::IsLoneShortArgument(const Node* n) {
    if (HasComment(n)) return false;
    const double threshold = opts_.width * 0.25;
    auto le = [&](size_t len) { return static_cast<double>(len) <= threshold; };
    if (n->type == NT::ThisExpression || (n->type == NT::Identifier && le(Utf16Length(n->str))) ||
        (IsSignedNumericLiteral(n) && !HasComment(n->argument()))) {
        return true;
    }
    if (IsRegExpLiteral(n)) return le(Utf16Length(n->str2));
    if (IsStringLiteral(n)) return le(Utf16Length(PrintString(Raw(n))));
    if (n->type == NT::TemplateLiteral) {
        const std::string_view raw = Raw(n->quasis()[0]);
        return n->expressions().empty() && le(Utf16Length(raw)) && raw.find('\n') == std::string_view::npos;
    }
    if (n->type == NT::UnaryExpression) return IsLoneShortArgument(n->argument());
    if (n->type == NT::CallExpression && n->arguments().empty() && Is(n->callee(), NT::Identifier)) {
        return static_cast<double>(Utf16Length(n->callee()->str)) <= threshold - 2;
    }
    return IsLiteral(n);
}

namespace {

bool IsNodeMatchesPath(const Node* n, std::string_view path) {
    std::vector<std::string_view> names;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        names.push_back(path.substr(start, dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    for (size_t index = names.size(); index-- > 0;) {
        if (index == 0) return Is(n, NT::Identifier) && n->str == names[0];
        if (!Is(n, NT::MemberExpression) || n->Has(jsparse::kFlagOptional) || n->Has(jsparse::kFlagComputed) ||
            !Is(n->property(), NT::Identifier) || n->property()->str != names[index]) {
            return false;
        }
        n = n->object();
    }
    return false;
}

bool IsTestCallCallee(const Node* n) {
    static const char* const kPatterns[] = {
        "it",          "it.only",       "it.skip",          "describe",    "describe.only",
        "describe.skip", "test",        "test.only",        "test.skip",   "test.step",
        "test.describe", "test.describe.only", "test.describe.parallel", "test.describe.parallel.only",
        "test.describe.serial", "test.describe.serial.only", "skip", "xit", "xdescribe", "xtest", "fit",
        "fdescribe",   "ftest",
    };
    for (const char* p : kPatterns) {
        if (IsNodeMatchesPath(n, p)) return true;
    }
    return false;
}

bool IsAngularTestWrapper(const Node* n) {
    if (!Is(n, NT::CallExpression) || !Is(n->callee(), NT::Identifier)) return false;
    const std::string_view name = n->callee()->str;
    return name == "async" || name == "inject" || name == "fakeAsync" || name == "waitForAsync";
}

bool IsUnitTestSetupIdentifier(const Node* n) {
    return Is(n, NT::Identifier) &&
           (n->str == "beforeEach" || n->str == "beforeAll" || n->str == "afterEach" || n->str == "afterAll");
}

}  // namespace

bool Printer::IsTestCall(const Node* n, const Node* parent_node) {
    if (!Is(n, NT::CallExpression) || n->Has(jsparse::kFlagOptional)) return false;
    const auto args = GetCallArguments(n);
    if (args.size() == 1) {
        if (IsAngularTestWrapper(n) && parent_node && IsTestCall(parent_node)) return IsFunctionOrArrow(args[0]);
        if (IsUnitTestSetupIdentifier(n->callee())) return IsAngularTestWrapper(args[0]);
    } else if ((args.size() == 2 || args.size() == 3) &&
               (Is(args[0], NT::TemplateLiteral) || IsStringLiteral(args[0])) && IsTestCallCallee(n->callee())) {
        if (args.size() == 3 && !IsNumericLiteral(args[2])) return false;
        bool fn_ok;
        if (args.size() == 2) {
            fn_ok = IsFunctionOrArrow(args[1]);
        } else {
            const bool with_body = Is(args[1], NT::FunctionExpression) ||
                                   (Is(args[1], NT::ArrowFunctionExpression) &&
                                    Is(args[1]->body(), NT::BlockStatement));
            fn_ok = with_body && GetFunctionParameters(args[1]).size() <= 1;
        }
        return fn_ok || IsAngularTestWrapper(args[1]);
    }
    return false;
}

bool Printer::IsFunctionCompositionArgs(const std::vector<Node*>& args) const {
    if (args.size() <= 1) return false;
    int count = 0;
    for (Node* arg : args) {
        if (IsFunctionOrArrow(arg)) {
            count += 1;
            if (count > 1) return true;
        } else if (IsCallExpression(arg)) {
            for (Node* child : GetCallArguments(arg)) {
                if (IsFunctionOrArrow(child)) return true;
            }
        }
    }
    return false;
}

bool Printer::IsLongCurriedCallExpression() const {
    Node* n = node();
    Node* p = parent();
    return key() == Key::callee && IsCallExpression(n) && IsCallExpression(p) && !p->arguments().empty() &&
           n->arguments().size() > p->arguments().size();
}

static bool TemplateLiteralHasNewLines(const Node* t) {
    for (Node* q : t->quasis()) {
        if (Raw(q).find('\n') != std::string_view::npos) return true;
    }
    return false;
}

bool Printer::IsTemplateOnItsOwnLine(const Node* n) const {
    return ((Is(n, NT::TemplateLiteral) && TemplateLiteralHasNewLines(n)) ||
            (Is(n, NT::TaggedTemplateExpression) && TemplateLiteralHasNewLines(n->quasi()))) &&
           !HasNewline(n->start, true);
}

bool Printer::StartsWithNoLookaheadToken(const Node* n, const std::function<bool(const Node*)>& pred) const {
    switch (n->type) {
        case NT::BinaryExpression:
        case NT::LogicalExpression:
        case NT::AssignmentExpression:
            return StartsWithNoLookaheadToken(n->left(), pred);
        case NT::MemberExpression:
            return StartsWithNoLookaheadToken(n->object(), pred);
        case NT::TaggedTemplateExpression:
            if (Is(n->tag(), NT::FunctionExpression)) return false;
            return StartsWithNoLookaheadToken(n->tag(), pred);
        case NT::CallExpression:
            if (Is(n->callee(), NT::FunctionExpression)) return false;
            return StartsWithNoLookaheadToken(n->callee(), pred);
        case NT::ConditionalExpression:
            return StartsWithNoLookaheadToken(n->test(), pred);
        case NT::UpdateExpression:
            return !n->Has(jsparse::kFlagPrefix) && StartsWithNoLookaheadToken(n->argument(), pred);
        case NT::SequenceExpression:
            return StartsWithNoLookaheadToken(n->expressions()[0], pred);
        case NT::ChainExpression:
        case NT::TSSatisfiesExpression:
        case NT::TSAsExpression:
        case NT::TSNonNullExpression:
            return StartsWithNoLookaheadToken(n->expression(), pred);
        default:
            return pred(n);
    }
}

bool Printer::HasNakedLeftSide(const Node* n) const {
    switch (n->type) {
        case NT::AssignmentExpression:
        case NT::BinaryExpression:
        case NT::LogicalExpression:
        case NT::ConditionalExpression:
        case NT::SequenceExpression:
        case NT::TaggedTemplateExpression:
        case NT::ChainExpression:
        case NT::TSAsExpression:
        case NT::TSSatisfiesExpression:
        case NT::TSNonNullExpression:
            return true;
        case NT::UpdateExpression:
            return !n->Has(jsparse::kFlagPrefix);
        default:
            return IsCallExpression(n) || IsMemberExpression(n);
    }
}

Key Printer::GetLeftSideKey(const Node* n, int* index) const {
    *index = -1;
    switch (n->type) {
        case NT::SequenceExpression:
        case NT::TemplateLiteral:
            *index = 0;
            return Key::expressions;
        case NT::AssignmentExpression:
        case NT::BinaryExpression:
        case NT::LogicalExpression:
        case NT::AssignmentPattern:
            return Key::left;
        case NT::ConditionalExpression:
            return Key::test;
        case NT::MemberExpression:
            return Key::object;
        case NT::CallExpression:
        case NT::NewExpression:
            return Key::callee;
        case NT::TaggedTemplateExpression:
            return Key::tag;
        case NT::UpdateExpression:
        case NT::UnaryExpression:
        case NT::SpreadElement:
        case NT::AwaitExpression:
        case NT::YieldExpression:
        case NT::RestElement:
            return n->argument() ? Key::argument : Key::None;
        case NT::ChainExpression:
        case NT::ExpressionStatement:
        case NT::JSXExpressionContainer:
        case NT::TSAsExpression:
        case NT::TSSatisfiesExpression:
        case NT::TSNonNullExpression:
        case NT::TSTypeAssertion:
        case NT::TSInstantiationExpression:
        case NT::Decorator:
        case NT::JSXSpreadChild:
        case NT::TSExportAssignment:
        case NT::TSExternalModuleReference:
            return Key::expression;
        default:
            return Key::None;
    }
}

Node* Printer::GetLeftSide(const Node* n) const {
    // node.expressions?.[0] ?? left ?? test ?? callee ?? object ?? tag ?? argument ?? expression
    switch (n->type) {
        case NT::SequenceExpression:
        case NT::TemplateLiteral:
            return n->expressions().empty() ? nullptr : n->expressions()[0];
        default: {
            int idx;
            const Key k = GetLeftSideKey(n, &idx);
            return k == Key::None ? nullptr : Child(n, k);
        }
    }
}

bool Printer::IsJsxWhitespaceExpression(const Node* n) {
    return Is(n, NT::JSXExpressionContainer) && IsStringLiteral(n->expression()) && n->expression()->str2 == " " &&
           !HasComment(n->expression());
}

// ---------------------------------------------------------------- needs-parens.js

static bool IsDecoratorMemberExpression(const Node* n) {
    if (n->type == NT::Identifier) return true;
    if (n->type == NT::MemberExpression) {
        return !n->Has(jsparse::kFlagComputed) && !n->Has(jsparse::kFlagOptional) &&
               Is(n->property(), NT::Identifier) && IsDecoratorMemberExpression(n->object());
    }
    return false;
}

static bool CanDecoratorExpressionUnparenthesized(const Node* n) {
    if (n->type == NT::ChainExpression) n = n->expression();
    return IsDecoratorMemberExpression(n) ||
           (n->type == NT::CallExpression && !n->Has(jsparse::kFlagOptional) &&
            IsDecoratorMemberExpression(n->callee()));
}

static bool IsStatement(const Node* n) {
    switch (n->type) {
        case NT::BlockStatement:
        case NT::BreakStatement:
        case NT::ClassBody:
        case NT::ClassDeclaration:
        case NT::PropertyDefinition:
        case NT::ContinueStatement:
        case NT::DebuggerStatement:
        case NT::DoWhileStatement:
        case NT::ExportAllDeclaration:
        case NT::ExportDefaultDeclaration:
        case NT::ExportNamedDeclaration:
        case NT::ExpressionStatement:
        case NT::ForInStatement:
        case NT::ForOfStatement:
        case NT::ForStatement:
        case NT::FunctionDeclaration:
        case NT::IfStatement:
        case NT::ImportDeclaration:
        case NT::LabeledStatement:
        case NT::MethodDefinition:
        case NT::ReturnStatement:
        case NT::SwitchStatement:
        case NT::ThrowStatement:
        case NT::TryStatement:
        case NT::TSDeclareFunction:
        case NT::TSEnumDeclaration:
        case NT::TSImportEqualsDeclaration:
        case NT::TSInterfaceDeclaration:
        case NT::TSModuleDeclaration:
        case NT::TSNamespaceExportDeclaration:
        case NT::VariableDeclaration:
        case NT::WhileStatement:
        case NT::WithStatement:
            return true;
        default:
            return false;
    }
}

bool Printer::IsPathInForStatementInitializer() const {
    // walk up from the current node
    for (size_t i = stack_.size() - 1; i-- > 0;) {
        const Node* p = stack_[i].node;
        const Node* child = stack_[i + 1].node;
        if (Is(p, NT::ForStatement) && p->init() == child) return true;
    }
    return false;
}

bool Printer::ShouldAddParenthesesToChainElement() const {
    auto is_chain_expr = [](Node* n, Key k) { return k == Key::expression && n->type == NT::ChainExpression; };
    auto is_tag = [](Node* n, Key k) { return k == Key::tag && n->type == NT::TaggedTemplateExpression; };
    auto is_non_null_expr = [](Node* n, Key k) { return k == Key::expression && n->type == NT::TSNonNullExpression; };
    auto is_object_or_callee = [](Node* n, Key k) {
        return (k == Key::object && n->type == NT::MemberExpression) ||
               (k == Key::callee && n->type == NT::CallExpression);
    };
    auto is_call_or_member = [](Node* n, Key) {
        return n->type == NT::CallExpression || n->type == NT::MemberExpression;
    };
    if (Match({nullptr, is_chain_expr, is_tag})) return true;
    // (a?.b)!`` and (a?.b!)`` in TypeScript
    if (Match({nullptr, is_chain_expr, is_non_null_expr, is_tag})) return true;
    if (Match({nullptr, is_non_null_expr, is_chain_expr, is_tag})) return true;
    if (Match({is_call_or_member, is_chain_expr}) &&
        (Match({nullptr, nullptr, [](Node* n, Key k) {
                    return (k == Key::callee && ((n->type == NT::CallExpression && !n->Has(jsparse::kFlagOptional)) ||
                                                 n->type == NT::NewExpression)) ||
                           (k == Key::object && n->type == NT::MemberExpression && !n->Has(jsparse::kFlagOptional));
                }}) ||
         Match({nullptr, nullptr, is_non_null_expr, is_object_or_callee}))) {
        return true;
    }
    // Babel treats `(a?.b!).c` and `(a?.b)!.c` the same
    if (Match({is_call_or_member, is_non_null_expr, is_chain_expr, is_object_or_callee})) return true;
    return false;
}

bool Printer::ShouldWrapFunctionForExportDefault() {
    Node* n = node();
    Node* p = parent();
    if (n->type == NT::FunctionExpression || n->type == NT::ClassExpression) {
        return p->type == NT::ExportDefaultDeclaration || !NeedsParens();
    }
    if (!HasNakedLeftSide(n) || (p->type != NT::ExportDefaultDeclaration && NeedsParens())) return false;
    int idx;
    const Key k = GetLeftSideKey(n, &idx);
    if (k == Key::None) return false;
    if (idx >= 0) return CallAt(k, idx, [&] { return ShouldWrapFunctionForExportDefault(); });
    return Call(k, [&] { return ShouldWrapFunctionForExportDefault(); });
}

bool Printer::NeedsParens() {
    if (IsRoot()) return false;
    Node* n = node();
    const Key k = key();
    Node* p = parent();
    if (!n || !p) return false;

    if (IsStatement(n)) return false;

    if (n->type == NT::Identifier) {
        if (k == Key::left && ((n->str == "async" && !p->Has(jsparse::kFlagAwait)) || n->str == "let") &&
            p->type == NT::ForOfStatement) {
            return true;
        }
        if (n->str == "let") {
            Node* for_of = FindAncestor([](Node* a) { return a->type == NT::ForOfStatement; });
            Node* expression = for_of ? for_of->left() : nullptr;
            if (expression && StartsWithNoLookaheadToken(expression, [&](const Node* lm) { return lm == n; })) {
                return true;
            }
        }
        if (k == Key::object && n->str == "let" && p->type == NT::MemberExpression &&
            p->Has(jsparse::kFlagComputed) && !p->Has(jsparse::kFlagOptional)) {
            Node* st = FindAncestor([](Node* a) {
                return a->type == NT::ExpressionStatement || a->type == NT::ForStatement ||
                       a->type == NT::ForInStatement;
            });
            Node* expression = !st                                   ? nullptr
                               : st->type == NT::ExpressionStatement ? st->expression()
                               : st->type == NT::ForStatement        ? st->init()
                                                                     : st->left();
            if (expression && StartsWithNoLookaheadToken(expression, [&](const Node* lm) { return lm == n; })) {
                return true;
            }
        }
        // `(type) satisfies never;` and similar cases
        if (k == Key::expression &&
            (n->str == "await" || n->str == "interface" || n->str == "module" || n->str == "using" ||
             n->str == "yield" || n->str == "let" || n->str == "component" || n->str == "hook" || n->str == "type")) {
            Node* anc = FindAncestor([](Node* a) { return !IsBinaryCastExpression(a); });
            if (anc && anc != p && anc->type == NT::ExpressionStatement) return true;
        }
        return false;
    }

    if (n->type == NT::ObjectExpression || n->type == NT::FunctionExpression || n->type == NT::ClassExpression) {
        Node* st = FindAncestor([](Node* a) { return a->type == NT::ExpressionStatement; });
        Node* expression = st ? st->expression() : nullptr;
        if (expression && StartsWithNoLookaheadToken(expression, [&](const Node* lm) { return lm == n; })) {
            return true;
        }
    }

    if (n->type == NT::ObjectExpression) {
        Node* arrow = FindAncestor([](Node* a) { return a->type == NT::ArrowFunctionExpression; });
        Node* body = arrow ? arrow->body() : nullptr;
        if (body && body->type != NT::SequenceExpression && body->type != NT::AssignmentExpression &&
            StartsWithNoLookaheadToken(body, [&](const Node* lm) { return lm == n; })) {
            return true;
        }
    }

    switch (p->type) {
        case NT::ParenthesizedExpression:
            return false;
        case NT::ClassDeclaration:
        case NT::ClassExpression:
            if (k == Key::superClass &&
                (n->type == NT::ArrowFunctionExpression || n->type == NT::AssignmentExpression ||
                 n->type == NT::AwaitExpression || n->type == NT::BinaryExpression ||
                 n->type == NT::ConditionalExpression || n->type == NT::LogicalExpression ||
                 n->type == NT::NewExpression || n->type == NT::ObjectExpression ||
                 n->type == NT::SequenceExpression || n->type == NT::TaggedTemplateExpression ||
                 n->type == NT::UnaryExpression || n->type == NT::UpdateExpression ||
                 n->type == NT::YieldExpression || n->type == NT::TSNonNullExpression ||
                 (n->type == NT::ClassExpression && !n->decorators().empty()))) {
                return true;
            }
            break;
        case NT::ExportDefaultDeclaration:
            return ShouldWrapFunctionForExportDefault() || n->type == NT::SequenceExpression;
        case NT::Decorator:
            if (k == Key::expression && !CanDecoratorExpressionUnparenthesized(n)) return true;
            break;
        case NT::BinaryExpression:
            if (k == Key::left && (p->op == "in" || p->op == "instanceof") && n->type == NT::UnaryExpression) {
                return true;
            }
            break;
        case NT::VariableDeclarator:
            if (k == Key::init &&
                Match({nullptr, nullptr,
                       [](Node* a, Key kk) { return kk == Key::declarations && a->type == NT::VariableDeclaration; },
                       [](Node* a, Key kk) { return kk == Key::left && a->type == NT::ForInStatement; }})) {
                return true;
            }
            break;
        default:
            break;
    }

    switch (n->type) {
        case NT::UpdateExpression:
            if (p->type == NT::UnaryExpression) {
                return n->Has(jsparse::kFlagPrefix) &&
                       ((n->op == "++" && p->op == "+") || (n->op == "--" && p->op == "-"));
            }
            [[fallthrough]];
        case NT::UnaryExpression:
            switch (p->type) {
                case NT::UnaryExpression:
                    return n->op == p->op && (n->op == "+" || n->op == "-");
                case NT::MemberExpression:
                    return k == Key::object;
                case NT::TaggedTemplateExpression:
                    return true;
                case NT::NewExpression:
                case NT::CallExpression:
                    return k == Key::callee;
                case NT::BinaryExpression:
                    return k == Key::left && p->op == "**";
                case NT::TSNonNullExpression:
                    return true;
                default:
                    return false;
            }
        case NT::BinaryExpression:
            if (p->type == NT::UpdateExpression) return true;
            if (n->op == "in" && IsPathInForStatementInitializer()) return true;
            [[fallthrough]];
        case NT::TSTypeAssertion:
        case NT::TSAsExpression:
        case NT::TSSatisfiesExpression:
        case NT::LogicalExpression:
            switch (p->type) {
                case NT::TSAsExpression:
                case NT::TSSatisfiesExpression:
                    return !IsBinaryCastExpression(n);
                case NT::ConditionalExpression:
                    return IsBinaryCastExpression(n) || (n->type == NT::LogicalExpression && n->op == "??");
                case NT::CallExpression:
                case NT::NewExpression:
                    return k == Key::callee;
                case NT::ClassExpression:
                case NT::ClassDeclaration:
                    return k == Key::superClass;
                case NT::TSTypeAssertion:
                case NT::TaggedTemplateExpression:
                case NT::UnaryExpression:
                case NT::JSXSpreadAttribute:
                case NT::SpreadElement:
                case NT::AwaitExpression:
                case NT::TSNonNullExpression:
                case NT::UpdateExpression:
                    return true;
                case NT::MemberExpression:
                    return k == Key::object;
                case NT::AssignmentExpression:
                case NT::AssignmentPattern:
                    return k == Key::left && (n->type == NT::TSTypeAssertion || IsBinaryCastExpression(n));
                case NT::LogicalExpression:
                    if (n->type == NT::LogicalExpression) return p->op != n->op;
                    [[fallthrough]];
                case NT::BinaryExpression: {
                    const std::string_view op = n->op;
                    if (n->type == NT::TSTypeAssertion) return IsBitwiseOperator(p->op);  // precedence undefined
                    if (op.empty()) return true;
                    const int prec = GetPrecedence(op);
                    const std::string_view pop = p->op;
                    const int pprec = GetPrecedence(pop);
                    if (pprec > prec) return true;
                    if (k == Key::right && pprec == prec) return true;
                    if (pprec == prec && !ShouldFlatten(pop, op)) return true;
                    if (pprec < prec && op == "%") return pop == "+" || pop == "-";
                    if (IsBitwiseOperator(pop)) return true;
                    return false;
                }
                default:
                    return false;
            }
        case NT::SequenceExpression:
            return p->type != NT::ForStatement;
        case NT::YieldExpression:
            if (p->type == NT::AwaitExpression || p->type == NT::TSTypeAssertion) return true;
            [[fallthrough]];
        case NT::AwaitExpression:
            switch (p->type) {
                case NT::TaggedTemplateExpression:
                case NT::UnaryExpression:
                case NT::LogicalExpression:
                case NT::SpreadElement:
                case NT::TSAsExpression:
                case NT::TSSatisfiesExpression:
                case NT::TSNonNullExpression:
                    return true;
                case NT::MemberExpression:
                    return k == Key::object;
                case NT::NewExpression:
                case NT::CallExpression:
                    return k == Key::callee;
                case NT::ConditionalExpression:
                    return k == Key::test;
                case NT::BinaryExpression:
                    if (!n->argument() && p->op == "|>") return false;
                    return true;
                default:
                    return false;
            }
        case NT::Literal:
            if (n->lit == jsparse::LitKind::String && p->type == NT::ExpressionStatement &&
                !p->Has(jsparse::kFlagHasDirective)) {
                Node* gp = grandparent();
                return gp && (gp->type == NT::Program || gp->type == NT::BlockStatement);
            }
            return k == Key::object && IsMemberExpression(p) && IsNumericLiteral(n);
        case NT::AssignmentExpression:
            if ((k == Key::init || k == Key::update) && p->type == NT::ForStatement) return false;
            if (k == Key::expression && n->left()->type != NT::ObjectPattern && p->type == NT::ExpressionStatement) {
                return false;
            }
            if (k == Key::key && p->type == NT::TSPropertySignature) return false;
            if (p->type == NT::AssignmentExpression) return false;
            if (k == Key::expressions && p->type == NT::SequenceExpression &&
                Match({nullptr, nullptr, [](Node* a, Key kk) {
                           return (kk == Key::init || kk == Key::update) && a->type == NT::ForStatement;
                       }})) {
                return false;
            }
            if (k == Key::value && p->type == NT::Property &&
                Match({nullptr, nullptr, [](Node* a, Key kk) {
                           return kk == Key::properties && a->type == NT::ObjectPattern;
                       }})) {
                return false;
            }
            return true;
        case NT::ConditionalExpression:
            switch (p->type) {
                case NT::TaggedTemplateExpression:
                case NT::UnaryExpression:
                case NT::SpreadElement:
                case NT::BinaryExpression:
                case NT::LogicalExpression:
                case NT::ExportDefaultDeclaration:
                case NT::AwaitExpression:
                case NT::JSXSpreadAttribute:
                case NT::TSTypeAssertion:
                case NT::TSAsExpression:
                case NT::TSSatisfiesExpression:
                case NT::TSNonNullExpression:
                    return true;
                case NT::NewExpression:
                case NT::CallExpression:
                    return k == Key::callee;
                case NT::ConditionalExpression:
                    return k == Key::test;
                case NT::MemberExpression:
                    return k == Key::object;
                default:
                    return false;
            }
        case NT::FunctionExpression:
            switch (p->type) {
                case NT::NewExpression:
                case NT::CallExpression:
                    return k == Key::callee;
                case NT::TaggedTemplateExpression:
                    return true;
                default:
                    return false;
            }
        case NT::ArrowFunctionExpression:
            switch (p->type) {
                case NT::BinaryExpression:
                    return p->op != "|>" || Parenthesized(n);
                case NT::NewExpression:
                case NT::CallExpression:
                    return k == Key::callee;
                case NT::MemberExpression:
                    return k == Key::object;
                case NT::TSAsExpression:
                case NT::TSSatisfiesExpression:
                case NT::TSNonNullExpression:
                case NT::TaggedTemplateExpression:
                case NT::UnaryExpression:
                case NT::LogicalExpression:
                case NT::AwaitExpression:
                case NT::TSTypeAssertion:
                    return true;
                case NT::ConditionalExpression:
                    return k == Key::test;
                default:
                    return false;
            }
        case NT::ClassExpression:
            return p->type == NT::NewExpression && k == Key::callee;
        case NT::CallExpression:
        case NT::MemberExpression:
            if (ShouldAddParenthesesToChainElement()) return true;
            [[fallthrough]];
        case NT::TaggedTemplateExpression:
        case NT::TSNonNullExpression:
            if (k == Key::callee && p->type == NT::NewExpression) {
                const Node* object = n;
                while (object) {
                    switch (object->type) {
                        case NT::CallExpression:
                            return true;
                        case NT::MemberExpression:
                            object = object->object();
                            break;
                        case NT::TaggedTemplateExpression:
                            object = object->tag();
                            break;
                        case NT::TSNonNullExpression:
                            object = object->expression();
                            break;
                        default:
                            return false;
                    }
                }
            }
            return false;
        case NT::JSXFragment:
        case NT::JSXElement:
            return k == Key::callee || (k == Key::left && p->type == NT::BinaryExpression && p->op == "<") ||
                   (!IsArrayExpression(p) && p->type != NT::ArrowFunctionExpression &&
                    p->type != NT::AssignmentExpression && p->type != NT::AssignmentPattern &&
                    p->type != NT::BinaryExpression && p->type != NT::NewExpression &&
                    p->type != NT::ConditionalExpression && p->type != NT::ExpressionStatement &&
                    p->type != NT::JSXAttribute && p->type != NT::JSXElement &&
                    p->type != NT::JSXExpressionContainer && p->type != NT::JSXFragment &&
                    p->type != NT::LogicalExpression && !IsCallExpression(p) && !IsObjectProperty(p) &&
                    p->type != NT::ReturnStatement && p->type != NT::ThrowStatement &&
                    p->type != NT::VariableDeclarator && p->type != NT::YieldExpression);
        case NT::TSInstantiationExpression:
            return k == Key::object && IsMemberExpression(p);
        case NT::TSFunctionType:
            if (Match({nullptr,
                       [](Node* a, Key kk) { return kk == Key::typeAnnotation && a->type == NT::TSTypeAnnotation; },
                       [](Node* a, Key kk) {
                           return kk == Key::returnType && a->type == NT::ArrowFunctionExpression;
                       }})) {
                return true;
            }
            [[fallthrough]];
        case NT::TSConditionalType:
        case NT::TSConstructorType:
            if (k == Key::extendsType && IsConditionalType(n) && p->type == n->type) return true;
            if (k == Key::checkType && IsConditionalType(p)) return true;
            if (k == Key::extendsType && p->type == NT::TSConditionalType) {
                const Node* ann = n->returnType() ? n->returnType() : n->typeAnnotation();
                const Node* t = ann ? ann->typeAnnotation() : nullptr;
                if (t && t->type == NT::TSTypePredicate && t->typeAnnotation()) {
                    t = t->typeAnnotation()->typeAnnotation();
                }
                if (t && t->type == NT::TSInferType && t->typeParameter() && t->typeParameter()->constraint()) {
                    return true;
                }
            }
            [[fallthrough]];
        case NT::TSUnionType:
        case NT::TSIntersectionType:
            if ((IsUnionType(p) || p->type == NT::TSIntersectionType) && p->types().size() > 1 &&
                (!(n->type == NT::TSUnionType || n->type == NT::TSIntersectionType) || n->types().size() > 1)) {
                return true;
            }
            [[fallthrough]];
        case NT::TSInferType:
            if (n->type == NT::TSInferType) {
                if (p->type == NT::TSRestType) return false;
                if (k == Key::types && (p->type == NT::TSUnionType || p->type == NT::TSIntersectionType) &&
                    Is(n->typeParameter(), NT::TSTypeParameter) && n->typeParameter()->constraint()) {
                    return true;
                }
            }
            [[fallthrough]];
        case NT::TSTypeOperator:
            return p->type == NT::TSArrayType || p->type == NT::TSOptionalType || p->type == NT::TSRestType ||
                   (k == Key::objectType && p->type == NT::TSIndexedAccessType) || p->type == NT::TSTypeOperator;
        case NT::TSTypeQuery:
            return (k == Key::objectType && p->type == NT::TSIndexedAccessType) ||
                   (k == Key::elementType && p->type == NT::TSArrayType);
        default:
            break;
    }
    return false;
}

}  // namespace jsfmt
