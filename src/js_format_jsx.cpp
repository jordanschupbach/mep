// Prettier 3.6.2's JSX printer: language-js/print/jsx.js.

#include "js_format_internal.h"

namespace jsfmt {

namespace {

bool IsJsxSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// text.split(/([ \n\r\t]+)/): words at even indices, whitespace runs at odd.
std::vector<std::string_view> SplitJsx(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    size_t word_start = 0;
    while (i < s.size()) {
        if (IsJsxSpace(s[i])) {
            out.push_back(s.substr(word_start, i - word_start));
            size_t j = i;
            while (j < s.size() && IsJsxSpace(s[j])) j++;
            out.push_back(s.substr(i, j - i));
            i = j;
            word_start = j;
        } else {
            i++;
        }
    }
    out.push_back(s.substr(word_start));
    return out;
}

std::string_view TrimJsx(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && IsJsxSpace(s[a])) a++;
    while (b > a && IsJsxSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool HasNewlineChar(std::string_view s) { return s.find('\n') != std::string_view::npos; }

}  // namespace

// `child.length === 1` with JS semantics: a string's UTF-16 length, an
// array doc's length; other docs have none.
static bool LengthIsOne(Doc* d) {
    if (!d) return false;
    if (d->kind == DocKind::String) return Utf16Length(d->str) == 1;
    if (d->kind == DocKind::Array) return d->n == 1;
    return false;
}

static Doc* SeparatorNoWhitespace(DocArena& a, bool fbt, Doc* child, const Node* child_node, const Node* next) {
    if (fbt) return nullptr;
    if ((child_node->type == NT::JSXElement && !child_node->closing()) ||
        (next && next->type == NT::JSXElement && !next->closing())) {
        return LengthIsOne(child) ? a.softline : a.hardline;
    }
    return a.softline;
}

static Doc* SeparatorWithWhitespace(DocArena& a, bool fbt, Doc* child, const Node* child_node, const Node* next) {
    if (fbt) return a.hardline;
    if (LengthIsOne(child)) {
        return (child_node->type == NT::JSXElement && !child_node->closing()) ||
                       (next && next->type == NT::JSXElement && !next->closing())
                   ? a.hardline
                   : a.softline;
    }
    return a.hardline;
}

Parts Printer::PrintJsxChildren(Doc* jsx_whitespace, bool is_fbt) {
    Doc* prev_part = nullptr;
    Parts parts{nullptr};
    auto push = [&](Doc* d) {
        prev_part = d;
        Doc* last = parts.back();
        parts.back() = Cat({last, d});
    };
    auto push_line = [&](Doc* d) {
        if (!d) return;
        prev_part = d;
        parts.push_back(d);
        parts.push_back(nullptr);
    };
    Each(Key::children, [&](int) {
        Node* n = node();
        Node* next = Next();
        if (n->type == NT::JSXText) {
            const std::string_view text = Raw(n);
            if (IsMeaningfulJsxText(n)) {
                std::vector<std::string_view> words = SplitJsx(text);
                size_t b = 0, e = words.size();
                if (words[0].empty()) {
                    b++;
                    if (HasNewlineChar(words[b])) {
                        Doc* w1 = b + 1 < e ? S(words[b + 1]) : nullptr;
                        push_line(SeparatorWithWhitespace(a_, is_fbt, w1, n, next));
                    } else {
                        push_line(jsx_whitespace);
                    }
                    b++;
                }
                bool has_end_ws = false;
                std::string_view end_ws;
                if (e > b && words[e - 1].empty()) {
                    e--;
                    if (e > b) {
                        end_ws = words[e - 1];
                        e--;
                        has_end_ws = true;
                    }
                }
                if (e <= b) return;
                for (size_t i = b; i < e; i++) {
                    if ((i - b) % 2 == 1) {
                        push_line(line());
                    } else {
                        push(S(words[i]));
                    }
                }
                if (has_end_ws) {
                    if (HasNewlineChar(end_ws)) {
                        push_line(SeparatorWithWhitespace(a_, is_fbt, prev_part, n, next));
                    } else {
                        push_line(jsx_whitespace);
                    }
                } else {
                    push_line(SeparatorNoWhitespace(a_, is_fbt, prev_part, n, next));
                }
            } else if (HasNewlineChar(text)) {
                size_t count = 0;
                for (char c : text) {
                    if (c == '\n') count++;
                }
                if (count > 1) push_line(hardline());
            } else {
                push_line(jsx_whitespace);
            }
        } else {
            push(Print());
            if (next && IsMeaningfulJsxText(next)) {
                const std::string_view trimmed = TrimJsx(Raw(next));
                const auto words = SplitJsx(trimmed);
                push_line(SeparatorNoWhitespace(a_, is_fbt, S(words[0]), n, next));
            } else {
                push_line(hardline());
            }
        }
    });
    return parts;
}

static bool IsEmptyJsxElement(const Printer& p, const Node* n) {
    const auto& ch = n->children();
    if (ch.empty()) return true;
    if (ch.size() > 1) return false;
    return ch[0]->type == NT::JSXText && !p.IsMeaningfulJsxText(ch[0]);
}

Doc* Printer::PrintJsxElementInternal() {
    Node* n = node();
    if (n->type == NT::JSXElement && IsEmptyJsxElement(*this, n)) {
        return Cat({Print(Key::openingElement), Print(Key::closingElement)});
    }
    Doc* opening = Print(Key::openingElement);
    Doc* closing = Print(Key::closingElement);
    if (n->children().size() == 1 && n->children()[0]->type == NT::JSXExpressionContainer &&
        (n->children()[0]->expression()->type == NT::TemplateLiteral ||
         n->children()[0]->expression()->type == NT::TaggedTemplateExpression)) {
        return Cat({opening, Cat(PrintAll(Key::children)), closing});
    }

    // Replace `{" "}` with a JSXText " " (Prettier mutates node.children).
    {
        NodeList& ch = n->list[0];
        bool any = false;
        for (Node* c : ch) {
            if (IsJsxWhitespaceExpression(c)) any = true;
        }
        if (any) {
            auto& items = synthetic_items_.emplace_back(ch.begin(), ch.end());
            for (Node*& c : items) {
                if (IsJsxWhitespaceExpression(c)) {
                    Node& t = synthetic_.emplace_back();
                    t.type = NT::JSXText;
                    t.str = " ";
                    t.str2 = " ";
                    t.start = c->start;
                    t.end = c->end;
                    c = &t;
                }
            }
            ch.items = items.data();
            ch.count = static_cast<uint32_t>(items.size());
        }
    }

    bool contains_tag = false, contains_text = false;
    int expressions = 0;
    for (Node* c : n->children()) {
        if (IsJsxElement(c)) contains_tag = true;
        if (c->type == NT::JSXExpressionContainer) expressions++;
        if (IsMeaningfulJsxText(c)) contains_text = true;
    }
    const bool multiple_attrs = n->type == NT::JSXElement && n->opening()->attributes().size() > 1;
    bool forced_break = WillBreak(opening) || contains_tag || multiple_attrs || expressions > 1;

    const char* raw_ws_text = opts_.single_quote ? "{' '}" : "{\" \"}";
    Doc* raw_ws = S(raw_ws_text);
    Doc* jsx_ws = IfBreak(Cat({raw_ws, softline()}), S(" "));
    const bool is_fbt = n->type == NT::JSXElement && Is(n->opening()->name(), NT::JSXIdentifier) &&
                        n->opening()->name()->str == "fbt";

    Parts children = PrintJsxChildren(jsx_ws, is_fbt);

    auto at = [&](size_t i) -> Doc* { return i < children.size() ? children[i] : reinterpret_cast<Doc*>(1); };
    auto is_empty = [](Doc* d) { return d == nullptr; };
    Doc* const hl = hardline();
    Doc* const sl = softline();
    for (size_t idx = children.size() >= 2 ? children.size() - 1 : 0; idx-- > 0;) {
        const size_t i = idx;
        Doc* c0 = at(i);
        Doc* c1 = at(i + 1);
        Doc* c2 = at(i + 2);
        const bool pair_empty = is_empty(c0) && is_empty(c1);
        const bool pair_hardlines = c0 == hl && is_empty(c1) && c2 == hl;
        const bool line_then_ws = (c0 == sl || c0 == hl) && is_empty(c1) && c2 == jsx_ws;
        const bool ws_then_line = c0 == jsx_ws && is_empty(c1) && (c2 == sl || c2 == hl);
        const bool double_ws = c0 == jsx_ws && is_empty(c1) && c2 == jsx_ws;
        const bool hard_soft = (c0 == sl && is_empty(c1) && c2 == hl) || (c0 == hl && is_empty(c1) && c2 == sl);
        if ((pair_hardlines && contains_text) || pair_empty || line_then_ws || double_ws || hard_soft) {
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i),
                           children.begin() + static_cast<std::ptrdiff_t>(std::min(i + 2, children.size())));
        } else if (ws_then_line) {
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i + 1),
                           children.begin() + static_cast<std::ptrdiff_t>(std::min(i + 3, children.size())));
        }
        if (idx > children.size()) idx = children.size();
    }

    auto empty_or_line = [&](Doc* d) { return d == nullptr || d == line() || d == hl || d == sl; };
    while (!children.empty() && empty_or_line(children.back())) children.pop_back();
    while (children.size() > 1 && empty_or_line(children[0]) && empty_or_line(children[1])) {
        children.erase(children.begin(), children.begin() + 2);
    }

    Parts multiline{nullptr};
    for (size_t i = 0; i < children.size(); i++) {
        Doc* child = children[i];
        if (child == jsx_ws) {
            if (i == 1 && a_.IsEmptyDoc(children[0])) {
                if (children.size() == 2) {
                    Doc* last = multiline.back();
                    multiline.back() = Cat({last, raw_ws});
                    continue;
                }
                multiline.push_back(Cat({raw_ws, hl}));
                multiline.push_back(nullptr);
                continue;
            } else if (i == children.size() - 1) {
                Doc* last = multiline.back();
                multiline.back() = Cat({last, raw_ws});
                continue;
            } else if (i >= 2 && children[i - 1] == nullptr && children[i - 2] == hl) {
                Doc* last = multiline.back();
                multiline.back() = Cat({last, raw_ws});
                continue;
            }
        }
        if (i % 2 == 0) {
            Doc* last = multiline.back();
            multiline.back() = Cat({last, child});
        } else {
            multiline.push_back(child);
            multiline.push_back(nullptr);
        }
        if (WillBreak(child)) forced_break = true;
    }

    Doc* content = contains_text ? a_.Fill(multiline) : Group(Cat(multiline), true);
    Doc* multi_line_elem = Group(Cat({opening, Indent(Cat({hl, content})), hl, closing}));
    if (forced_break) return multi_line_elem;
    Parts flat{opening};
    flat.insert(flat.end(), children.begin(), children.end());
    flat.push_back(closing);
    return a_.ConditionalGroup({Group(Cat(flat)), multi_line_elem});
}

static bool ShouldBreakJsxElement(const Printer& p) {
    return p.Match({nullptr, [](Node* x, Key) { return x->type == NT::ArrowFunctionExpression; },
                    [&](Node* x, Key) { return p.IsCallExpression(x); }}) &&
           (p.Match({nullptr, nullptr, nullptr, [](Node* x, Key) { return x->type == NT::JSXExpressionContainer; }}) ||
            p.Match({nullptr, nullptr, nullptr, [](Node* x, Key) { return x->type == NT::ChainExpression; },
                     [](Node* x, Key) { return x->type == NT::JSXExpressionContainer; }}));
}

Doc* Printer::MaybeWrapJsxElementInParens(Doc* elem) {
    Node* p = parent();
    switch (p->type) {
        case NT::ArrayExpression:
        case NT::JSXAttribute:
        case NT::JSXElement:
        case NT::JSXExpressionContainer:
        case NT::JSXFragment:
        case NT::ExpressionStatement:
        case NT::CallExpression:
        case NT::ConditionalExpression:
            return elem;
        default:
            break;
    }
    const bool should_break = ShouldBreakJsxElement(*this);
    const bool needs_parens = NeedsParens();
    return Group(Cat({needs_parens ? nullptr : IfBreak(S("(")), Indent(Cat({softline(), elem})), softline(),
                      needs_parens ? nullptr : IfBreak(S(")"))}),
                 should_break);
}

Doc* Printer::PrintJsxElement() { return MaybeWrapJsxElementInParens(PrintComments(PrintJsxElementInternal())); }

static std::string ReplaceAll(std::string s, std::string_view from, std::string_view to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

Doc* Printer::PrintJsxAttribute() {
    Node* n = node();
    Parts parts{Print(Key::name)};
    if (n->value()) {
        Doc* res;
        if (IsStringLiteral(n->value())) {
            const std::string_view raw = Raw(n->value());
            std::string final_s(raw.substr(1, raw.size() - 2));
            final_s = ReplaceAll(final_s, "&apos;", "'");
            final_s = ReplaceAll(final_s, "&quot;", "\"");
            size_t dq = 0, sq = 0;
            for (char c : final_s) {
                if (c == '"') dq++;
                if (c == '\'') sq++;
            }
            const bool prefer_single = opts_.jsx_single_quote;
            const size_t pc = prefer_single ? sq : dq;
            const size_t ac = prefer_single ? dq : sq;
            const char quote = pc > ac ? (prefer_single ? '"' : '\'') : (prefer_single ? '\'' : '"');
            final_s = quote == '"' ? ReplaceAll(final_s, "\"", "&quot;") : ReplaceAll(final_s, "'", "&apos;");
            std::string whole = std::string(1, quote) + final_s + std::string(1, quote);
            Doc* own = a_.Own(std::move(whole));
            res = Call(Key::value, [&] { return PrintComments(a_.ReplaceEndOfLine(own->str)); });
        } else {
            res = Print(Key::value);
        }
        parts.push_back(S("="));
        parts.push_back(res);
    }
    return Cat(parts);
}

Doc* Printer::PrintJsxExpressionContainer() {
    Node* n = node();
    std::function<bool(const Node*, const Node*)> should_inline = [&](const Node* x, const Node* par) -> bool {
        if (x->type == NT::JSXEmptyExpression) return true;
        if (HasComment(x)) return false;
        return IsArrayExpression(x) || IsObjectExpression(x) || x->type == NT::ArrowFunctionExpression ||
               (x->type == NT::AwaitExpression &&
                (should_inline(x->argument(), x) || x->argument()->type == NT::JSXElement)) ||
               IsCallExpression(x) || x->type == NT::FunctionExpression || x->type == NT::TemplateLiteral ||
               x->type == NT::TaggedTemplateExpression ||
               (IsJsxElement(par) && (x->type == NT::ConditionalExpression || IsBinaryish(x)));
    };
    if (should_inline(n->expression(), parent())) {
        return Group(Cat({S("{"), Print(Key::expression), a_.line_suffix_boundary, S("}")}));
    }
    return Group(
        Cat({S("{"), Indent(Cat({softline(), Print(Key::expression)})), softline(), a_.line_suffix_boundary, S("}")}));
}

Doc* Printer::PrintJsxOpeningElement() {
    Node* n = node();
    const bool name_has_comments = HasComment(n->name()) || HasComment(n->typeArguments());
    const auto& attrs = n->attributes();
    if (n->Has(jsparse::kFlagSelfClosing) && attrs.empty() && !name_has_comments) {
        return Cat({S("<"), Print(Key::name), Print(Key::typeArguments), S(" />")});
    }
    if (attrs.size() == 1 && attrs[0]->type == NT::JSXAttribute && IsStringLiteral(attrs[0]->value()) &&
        attrs[0]->value()->str2.find('\n') == std::string_view::npos && !name_has_comments &&
        !HasComment(attrs[0])) {
        Parts ps{S("<"), Print(Key::name), Print(Key::typeArguments), S(" ")};
        Parts printed = PrintAll(Key::attributes);
        ps.insert(ps.end(), printed.begin(), printed.end());
        ps.push_back(S(n->Has(jsparse::kFlagSelfClosing) ? " />" : ">"));
        return Group(Cat(ps));
    }
    bool should_break = false;
    for (Node* a : attrs) {
        if (a->type == NT::JSXAttribute && IsStringLiteral(a->value()) &&
            a->value()->str2.find('\n') != std::string_view::npos) {
            should_break = true;
        }
    }
    Parts attr_parts;
    Each(Key::attributes, [&](int) { attr_parts.push_back(Cat({line(), Print()})); });
    Parts ps{S("<"), Print(Key::name), Print(Key::typeArguments), Indent(Cat(attr_parts))};
    if (n->Has(jsparse::kFlagSelfClosing)) {
        ps.push_back(line());
        ps.push_back(S("/>"));
    } else {
        const bool last_attr_trailing = !attrs.empty() && HasComment(attrs[attrs.size() - 1], kCTrailing);
        const bool same_line = (attrs.empty() && !name_has_comments) ||
                               (opts_.bracket_same_line && (!name_has_comments || !attrs.empty()) &&
                                !last_attr_trailing);
        if (same_line) {
            ps.push_back(S(">"));
        } else {
            ps.push_back(softline());
            ps.push_back(S(">"));
        }
    }
    return Group(Cat(ps), should_break);
}

Doc* Printer::PrintJsxClosingElement() {
    Node* n = node();
    Parts parts{S("</")};
    Doc* printed = Print(Key::name);
    if (HasComment(n->name(), kCLeading | kCLine)) {
        parts.push_back(Indent(Cat({hardline(), printed})));
        parts.push_back(hardline());
    } else if (HasComment(n->name(), kCLeading | kCBlock)) {
        parts.push_back(S(" "));
        parts.push_back(printed);
    } else {
        parts.push_back(printed);
    }
    parts.push_back(S(">"));
    return Cat(parts);
}

Doc* Printer::PrintJsxOpeningClosingFragment() {
    Node* n = node();
    const bool has_comment = HasComment(n);
    const bool has_own_line = HasComment(n, kCLine);
    const bool opening = n->type == NT::JSXOpeningFragment;
    return Cat({S(opening ? "<" : "</"),
                Indent(Cat({has_own_line ? hardline() : (has_comment && !opening ? S(" ") : nullptr),
                            PrintDanglingComments()})),
                has_own_line ? hardline() : nullptr, S(">")});
}

Doc* Printer::PrintJsxEmptyExpression() {
    Node* n = node();
    const bool requires_hardline = HasComment(n, kCLine);
    return Cat({PrintDanglingComments(requires_hardline), requires_hardline ? hardline() : nullptr});
}

Doc* Printer::PrintJsxSpreadAttribute() {
    Doc* inner = Call(node()->type == NT::JSXSpreadAttribute ? Key::argument : Key::expression, [&] {
        Node* a = node();
        Doc* printed = Cat({S("..."), MainPrint(nullptr)});
        if (!HasComment(a) || !WillPrintOwnComments()) return printed;
        return Cat({Indent(Cat({softline(), PrintComments(printed)})), softline()});
    });
    return Cat({S("{"), inner, S("}")});
}

Doc* Printer::PrintJsx() {
    Node* n = node();
    switch (n->type) {
        case NT::JSXAttribute:
            return PrintJsxAttribute();
        case NT::JSXIdentifier:
            return S(n->str);
        case NT::JSXNamespacedName:
            return Cat({Print(Key::nspace), S(":"), Print(Key::name)});
        case NT::JSXMemberExpression:
            return Cat({Print(Key::object), S("."), Print(Key::property)});
        case NT::JSXSpreadAttribute:
        case NT::JSXSpreadChild:
            return PrintJsxSpreadAttribute();
        case NT::JSXExpressionContainer:
            return PrintJsxExpressionContainer();
        case NT::JSXFragment:
        case NT::JSXElement:
            return PrintJsxElement();
        case NT::JSXOpeningElement:
            return PrintJsxOpeningElement();
        case NT::JSXClosingElement:
            return PrintJsxClosingElement();
        case NT::JSXOpeningFragment:
        case NT::JSXClosingFragment:
            return PrintJsxOpeningClosingFragment();
        case NT::JSXEmptyExpression:
            return PrintJsxEmptyExpression();
        default:
            throw UnsupportedNode{n};
    }
}

}  // namespace jsfmt
