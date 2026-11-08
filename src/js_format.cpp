// mep's JavaScript formatter driver: Prettier 3.6.2's main/core.js
// (BOM, end-of-line normalisation), parse/acorn.js and
// parse/postprocess/*.js, then printing, then the self-check. See
// js_format.h.

#include "js_format.h"

#include <algorithm>
#include <cstring>

#include "js_format_internal.h"

namespace jsfmt {

namespace {

// node's readFileSync(f, "utf8"): ill-formed sequences become U+FFFD.
std::string SanitizeUtf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            out += static_cast<char>(b);
            i++;
            continue;
        }
        int need = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            if (b == 0xE0) lo = 0xA0;
            if (b == 0xED) hi = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            if (b == 0xF0) lo = 0x90;
            if (b == 0xF4) hi = 0x8F;
        } else {
            out += "\xEF\xBF\xBD";
            i++;
            continue;
        }
        size_t j = i + 1;
        int got = 0;
        bool bad = false;
        while (got < need) {
            if (j >= n) {
                bad = true;
                break;
            }
            const auto c = static_cast<unsigned char>(s[j]);
            const unsigned char l = got == 0 ? lo : 0x80;
            const unsigned char h = got == 0 ? hi : 0xBF;
            if (c < l || c > h) {
                bad = true;
                break;
            }
            j++;
            got++;
        }
        if (bad) {
            out += "\xEF\xBF\xBD";
            i = j;
            continue;
        }
        out.append(s.data() + i, j - i);
        i = j;
    }
    return out;
}

bool IsAllJsWhitespace(std::string_view s) { return TrimJs(s).empty(); }

int LineOf(std::string_view text, uint32_t offset) {
    int line = 1;
    for (size_t i = 0; i < offset && i < text.size(); i++) {
        if (text[i] == '\n') line++;
    }
    return line;
}

// parse/acorn.js: module (imports anywhere), then script. parse/typescript.js
// (typescript-estree): one parse, the dialect from the file name.
jsparse::ParseResult ParseLikePrettier(std::string_view text, const Options& opts, bool preserve_parens) {
    jsparse::Options o;
    o.jsx = opts.jsx;
    if (opts.typescript) {
        o.typescript = true;
        o.dts = opts.dts;
        o.recover = false;
        o.source_type = jsparse::SourceType::Module;
        o.allow_import_export_everywhere = true;
        o.allow_return_outside_function = true;
        o.allow_super_outside_method = true;
        o.check_private_fields = false;
        return jsparse::Parse(text, o);
    }
    o.preserve_parens = preserve_parens;
    o.allow_return_outside_function = true;
    o.allow_super_outside_method = true;
    o.check_private_fields = false;
    o.recover = false;
    o.source_type = jsparse::SourceType::Module;
    o.allow_import_export_everywhere = true;
    jsparse::ParseResult m = jsparse::Parse(text, o);
    if (!m.has_error) return m;
    o.source_type = jsparse::SourceType::Script;
    o.allow_import_export_everywhere = false;
    jsparse::ParseResult s = jsparse::Parse(text, o);
    if (!s.has_error) return s;
    return m;
}

// Iterative AST depth.
size_t TreeDepth(Node* root, size_t cap) {
    std::vector<std::pair<Node*, size_t>> stack{{root, 1}};
    size_t max = 0;
    std::vector<Node*> kids;
    while (!stack.empty()) {
        auto [n, d] = stack.back();
        stack.pop_back();
        if (!n) continue;
        max = std::max(max, d);
        if (max > cap) return max;
        jsparse::CollectChildren(n, kids);
        for (Node* k : kids) stack.push_back({k, d + 1});
    }
    return max;
}

size_t CountNodes(Node* root) {
    size_t count = 0;
    std::vector<Node*> stack{root};
    std::vector<Node*> kids;
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        if (!n) continue;
        count++;
        jsparse::CollectChildren(n, kids);
        stack.insert(stack.end(), kids.begin(), kids.end());
    }
    return count;
}

// ---------------------------------------------------------------- postprocess

struct Postprocess {
    std::string_view text;
    std::deque<Node>& synthetic;
    Printer& printer;
    std::vector<uint32_t> type_cast_ends;

    bool IsUnbalanced(const Node* n) const {
        return n->type == NT::LogicalExpression && n->right()->type == NT::LogicalExpression &&
               n->op == n->right()->op;
    }

    Node* Rebalance(Node* n) {
        while (IsUnbalanced(n)) {
            Node* r = n->right();
            Node& left = synthetic.emplace_back();
            left.type = NT::LogicalExpression;
            left.op = n->op;
            left.left() = n->left();
            left.right() = r->left();
            left.start = n->left()->start;
            left.end = r->left()->end;
            Node* new_left = Rebalance(&left);
            Node& top = synthetic.emplace_back();
            top.type = NT::LogicalExpression;
            top.op = n->op;
            top.left() = new_left;
            top.right() = r->right();
            top.start = n->start;
            top.end = n->end;
            n = &top;
        }
        return n;
    }

    // Returns the replacement for n (post-order, like visitNode).
    Node* Visit(Node* n) {
        if (!n) return n;
        for (Node*& k : n->kid) k = Visit(k);
        for (NodeList& l : n->list) {
            for (uint32_t i = 0; i < l.count; i++) l.items[i] = Visit(l.items[i]);
        }
        for (Node*& k : n->ts) k = Visit(k);
        for (NodeList& l : n->tslist) {
            for (uint32_t i = 0; i < l.count; i++) l.items[i] = Visit(l.items[i]);
        }
        switch (n->type) {
            case NT::TSUnionType:
            case NT::TSIntersectionType:
                if (n->types().size() == 1) return n->types()[0];
                break;
            case NT::ParenthesizedExpression: {
                const uint32_t start = n->start;
                bool keep = false;
                uint32_t prev_end = 0;
                bool have = false;
                for (uint32_t e : type_cast_ends) {
                    if (e <= start) {
                        prev_end = e;
                        have = true;
                    }
                }
                if (have && prev_end) keep = TrimJs(text.substr(prev_end, start - prev_end)).empty();
                if (!keep) {
                    printer.SetParenthesized(n->expression());
                    return n->expression();
                }
                break;
            }
            case NT::LogicalExpression:
                if (IsUnbalanced(n)) return Rebalance(n);
                break;
            case NT::VariableDeclaration: {
                const NodeList& ds = n->declarations();
                if (!ds.empty()) {
                    Node* last = ds[ds.size() - 1];
                    if (last->init() && (last->end >= text.size() || text[last->end] != ';')) n->end = last->end;
                }
                break;
            }
            default:
                break;
        }
        return n;
    }
};

// ---------------------------------------------------------------- self-check

struct Compare {
    std::string_view a_text, b_text;
    std::string why;
    size_t budget = 0;

    bool Fail(const Node* a, const char* what) {
        if (why.empty()) {
            why = std::string("output AST differs (") + what + " at " +
                  (a ? jsparse::NodeTypeName(a->type) : "null") + ")";
        }
        return false;
    }

    static void LogicalOperands(const Node* n, std::string_view op, std::vector<const Node*>& out) {
        if (n->type == NT::LogicalExpression && n->op == op) {
            LogicalOperands(n->left(), op, out);
            LogicalOperands(n->right(), op, out);
        } else {
            out.push_back(n);
        }
    }

    static std::string KeyString(const Node* k) {
        if (k->type == NT::Identifier) return std::string(k->str);
        if (k->type == NT::Literal) {
            if (k->lit == jsparse::LitKind::String) return std::string(k->str2);
            if (k->lit == jsparse::LitKind::Number) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g", k->num);
                return std::string("#") + buf;
            }
        }
        return "";
    }

    static bool IsKeyed(const Node* n) {
        return n->type == NT::Property || n->type == NT::MethodDefinition || n->type == NT::PropertyDefinition ||
               n->type == NT::TSPropertySignature;
    }

    // postprocess: a one-type union/intersection is its type.
    static const Node* Unwrap(const Node* n) {
        while (n && (n->type == NT::TSUnionType || n->type == NT::TSIntersectionType) && n->types().size() == 1) {
            n = n->types()[0];
        }
        return n;
    }
    static bool IsChainOfNonNull(const Node* n) {
        return n->type == NT::ChainExpression && Is(n->expression(), NT::TSNonNullExpression);
    }
    static bool IsNonNullOfChain(const Node* n) {
        return n->type == NT::TSNonNullExpression && Is(n->expression(), NT::ChainExpression);
    }

    static void JsxTokens(const Node* el, std::vector<const Node*>& nodes, std::vector<std::string_view>& words) {
        for (const Node* c : el->children()) {
            if (c->type == NT::JSXText) {
                std::string_view raw = c->str;
                size_t i = 0;
                while (i < raw.size()) {
                    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\n' || raw[i] == '\r' || raw[i] == '\t')) i++;
                    size_t j = i;
                    while (j < raw.size() && !(raw[j] == ' ' || raw[j] == '\n' || raw[j] == '\r' || raw[j] == '\t')) j++;
                    if (j > i) words.push_back(raw.substr(i, j - i));
                    i = j;
                }
                continue;
            }
            if (c->type == NT::JSXExpressionContainer && IsStringLiteral(c->expression()) && c->expression()->str2 == " ") {
                continue;
            }
            nodes.push_back(c);
            words.push_back("\x01");
        }
    }

    bool Lists(const NodeList& a, const NodeList& b, bool skip_empty, const Node* owner) {
        std::vector<const Node*> la, lb;
        for (const Node* x : a) {
            if (!(skip_empty && x && x->type == NT::EmptyStatement)) la.push_back(x);
        }
        for (const Node* x : b) {
            if (!(skip_empty && x && x->type == NT::EmptyStatement)) lb.push_back(x);
        }
        if (la.size() != lb.size()) return Fail(owner, "list length");
        for (size_t i = 0; i < la.size(); i++) {
            if (!Same(la[i], lb[i])) return false;
        }
        return true;
    }

    bool Same(const Node* a, const Node* b) {
        a = Unwrap(a);
        b = Unwrap(b);
        if (!a || !b) return a == b ? true : Fail(a ? a : b, "missing node");
        if (++budget > 50'000'000) return Fail(a, "budget");
        // clean.js: ChainExpression(TSNonNull(x)) is TSNonNull(ChainExpression(x)).
        if ((IsChainOfNonNull(a) && IsNonNullOfChain(b)) || (IsNonNullOfChain(a) && IsChainOfNonNull(b))) {
            return Same(a->expression()->expression(), b->expression()->expression());
        }
        if (a->type != b->type) return Fail(a, "type");
        switch (a->type) {
            case NT::Literal:
                if (a->lit != b->lit) return Fail(a, "literal kind");
                switch (a->lit) {
                    case jsparse::LitKind::String:
                        return a->str2 == b->str2 ? true : Fail(a, "string");
                    case jsparse::LitKind::Number:
                        return (a->num == b->num || (a->num != a->num && b->num != b->num)) ? true : Fail(a, "number");
                    case jsparse::LitKind::BigInt:
                        return a->str2 == b->str2 ? true : Fail(a, "bigint");
                    case jsparse::LitKind::RegExp: {
                        std::string fa(a->str3), fb(b->str3);
                        std::sort(fa.begin(), fa.end());
                        std::sort(fb.begin(), fb.end());
                        return a->str2 == b->str2 && fa == fb ? true : Fail(a, "regexp");
                    }
                    default:
                        return a->Has(jsparse::kFlagBoolValue) == b->Has(jsparse::kFlagBoolValue) ? true
                                                                                                   : Fail(a, "literal");
                }
            case NT::JSXText:
                return true;  // compared through JsxTokens
            case NT::LogicalExpression: {
                std::vector<const Node*> oa, ob;
                LogicalOperands(a, a->op, oa);
                LogicalOperands(b, a->op, ob);
                if (a->op != b->op || oa.size() != ob.size()) return Fail(a, "logical chain");
                for (size_t i = 0; i < oa.size(); i++) {
                    if (!Same(oa[i], ob[i])) return false;
                }
                return true;
            }
            default:
                break;
        }
        if (a->flags != b->flags) return Fail(a, "flags");
        if (a->op != b->op || a->kind != b->kind) return Fail(a, "operator/kind");
        if (a->tsflags != b->tsflags || a->accessibility != b->accessibility) return Fail(a, "modifiers");
        if (a->type == NT::Identifier || a->type == NT::PrivateIdentifier || a->type == NT::JSXIdentifier ||
            a->type == NT::TemplateElement) {
            if (a->str != b->str) return Fail(a, "name");
        }
        if (a->type == NT::ExpressionStatement && a->Has(jsparse::kFlagHasDirective) && a->str != b->str) {
            return Fail(a, "directive");
        }
        if (a->type == NT::JSXElement || a->type == NT::JSXFragment) {
            if (!Same(a->kid[0], b->kid[0]) || !Same(a->kid[1], b->kid[1])) return false;
            std::vector<const Node*> na, nb;
            std::vector<std::string_view> wa, wb;
            JsxTokens(a, na, wa);
            JsxTokens(b, nb, wb);
            if (wa != wb || na.size() != nb.size()) return Fail(a, "JSX children");
            for (size_t i = 0; i < na.size(); i++) {
                if (!Same(na[i], nb[i])) return false;
            }
            return true;
        }
        for (int i = 0; i < 4; i++) {
            if (i == 0 && IsKeyed(a) && !a->Has(jsparse::kFlagComputed)) {
                if (!a->kid[0] || !b->kid[0] || KeyString(a->kid[0]) != KeyString(b->kid[0])) return Fail(a, "key");
                continue;
            }
            if (!Same(a->kid[i], b->kid[i])) return false;
        }
        const bool stmt_list = a->type == NT::Program || a->type == NT::BlockStatement ||
                               a->type == NT::StaticBlock || a->type == NT::SwitchCase || a->type == NT::ClassBody;
        for (int i = 0; i < 2; i++) {
            if (!Lists(a->list[i], b->list[i], stmt_list && i == 0, a)) return false;
        }
        for (int i = 0; i < 4; i++) {
            if (!Same(a->ts[i], b->ts[i])) return false;
        }
        for (int i = 0; i < 2; i++) {
            if (!Lists(a->tslist[i], b->tslist[i], false, a)) return false;
        }
        return true;
    }
};

// A comment as comparable pieces. A line comment splits at each `//`:
// Prettier can print two line comments as one (`a // x // y`, then a
// single comment on the next parse), and that keeps every word.
void NormalizeComment(const jsparse::Comment& c, std::string_view text, std::vector<std::string>& out) {
    if (c.block) {
        std::string s = "B";
        for (char ch : c.value) {
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') s += ch;
        }
        out.push_back(std::move(s));
        return;
    }
    std::string_view t = text.substr(c.start, c.end - c.start);
    if (t.substr(0, 2) != "//") {  // #! <!-- -->
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
        out.push_back("L" + std::string(t));
        return;
    }
    t.remove_prefix(2);
    while (true) {
        const size_t next = t.find("//");
        std::string_view piece = t.substr(0, next);
        while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) piece.remove_prefix(1);
        while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t' || piece.back() == '\r')) {
            piece.remove_suffix(1);
        }
        out.push_back("L//" + std::string(piece));
        if (next == std::string_view::npos) break;
        t.remove_prefix(next + 2);
    }
}

}  // namespace

void SetDialectForPath(Options& opts, std::string_view path) {
    auto ends = [&](std::string_view suffix) {
        if (path.size() < suffix.size()) return false;
        for (size_t i = 0; i < suffix.size(); i++) {
            const char c = path[path.size() - suffix.size() + i];
            const char l = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
            if (l != suffix[i]) return false;
        }
        return true;
    };
    opts.typescript = false;
    opts.jsx = true;
    opts.dts = false;
    if (ends(".tsx")) {
        opts.typescript = true;
    } else if (ends(".ts") || ends(".mts") || ends(".cts")) {
        opts.typescript = true;
        opts.jsx = false;
        opts.dts = ends(".d.ts") || ends(".d.mts") || ends(".d.cts");
    }
}

Result Format(std::string_view src, const Options& opts) {
    Result r;
    r.text = std::string(src);
    std::string text = SanitizeUtf8(src);
    bool has_bom = false;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        has_bom = true;
        text.erase(0, 3);
    }
    if (text.find('\r') != std::string::npos) {
        std::string t;
        t.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\r') {
                t += '\n';
                if (i + 1 < text.size() && text[i + 1] == '\n') i++;
            } else {
                t += text[i];
            }
        }
        text.swap(t);
    }
    if (IsAllJsWhitespace(text)) {
        r.ok = true;
        r.text = has_bom ? "\xEF\xBB\xBF" : "";
        return r;
    }

    jsparse::ParseResult parsed = ParseLikePrettier(text, opts, !opts.typescript);
    if (parsed.has_error || !parsed.program) {
        r.error = parsed.errors.empty() ? "parse error" : parsed.errors[0].message;
        r.error_line = parsed.errors.empty() ? 0 : static_cast<int>(parsed.errors[0].loc.line);
        return r;
    }
    if (TreeDepth(parsed.program, static_cast<size_t>(opts.max_depth)) > static_cast<size_t>(opts.max_depth)) {
        r.error = "nesting too deep to format";
        return r;
    }

    try {
        DocArena arena;
        const size_t nodes = CountNodes(parsed.program);
        arena.SetBudget(2'000'000 + nodes * 400);
        Printer printer(text, opts, arena);
        printer.print_limit = 1'000'000 + nodes * 60;

        // Comments: mergeNestledJsdocComments, then type-cast ends.
        std::deque<CommentRec> comment_store;
        std::vector<CommentRec*> comments;
        std::deque<std::string> merged_values;
        for (const jsparse::Comment& c : parsed.comments) {
            CommentRec& rec = comment_store.emplace_back();
            rec.block = c.block;
            rec.value = c.value;
            rec.start = c.start;
            rec.end = c.end;
            comments.push_back(&rec);
        }
        if (comments.size() >= 2) {
            CommentRec* following = nullptr;
            for (size_t i = comments.size(); i-- > 0;) {
                CommentRec* c = comments[i];
                if (following && c->end == following->start && IsIndentableBlockComment(c) &&
                    IsIndentableBlockComment(following)) {
                    merged_values.push_back(std::string(c->value) + "*//*" + std::string(following->value));
                    c->value = merged_values.back();
                    c->end = following->end;
                    comments.erase(comments.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                }
                following = c;
            }
        }
        std::deque<Node> synthetic;
        Postprocess pp{text, synthetic, printer, {}};
        for (CommentRec* c : comments) {
            if (IsTypeCastComment(c)) pp.type_cast_ends.push_back(c->end);
        }
        Node* program = pp.Visit(parsed.program);
        program->start = 0;
        program->end = static_cast<uint32_t>(text.size());

        printer.stack_.push_back(Frame{program, Key::None, -1, nullptr});
        printer.Attach(program, comments);
        Doc* doc = printer.MainPrint(nullptr);
        if (const CommentRec* lost = printer.UnprintedComment()) {
            r.error = "comment \"" + std::string(TrimJs(lost->value)) + "\" was not printed";
            r.error_line = LineOf(text, lost->start);
            return r;
        }
        PrintOptions po;
        po.width = opts.width;
        po.tab_width = opts.indent;
        po.use_tabs = opts.use_tabs;
        uint64_t work = 0;
        po.work = &work;
        po.work_limit = 400'000'000ULL + static_cast<uint64_t>(nodes) * 20000ULL;
        std::string out = PrintDocToString(arena, doc, po);

        if (opts.verify) {
            jsparse::ParseResult before = ParseLikePrettier(text, opts, false);
            jsparse::ParseResult after = ParseLikePrettier(out, opts, false);
            if (after.has_error || !after.program) {
                r.error = "formatted output does not parse: " +
                          (after.errors.empty() ? std::string("?") : after.errors[0].Full());
                return r;
            }
            Compare cmp{text, out, {}, 0};
            if (!cmp.Same(before.program, after.program)) {
                r.error = cmp.why;
                return r;
            }
            std::vector<std::string> ca, cb;
            for (const auto& c : before.comments) NormalizeComment(c, text, ca);
            for (const auto& c : after.comments) NormalizeComment(c, out, cb);
            std::sort(ca.begin(), ca.end());
            std::sort(cb.begin(), cb.end());
            if (ca != cb) {
                r.error = "comments differ after formatting";
                return r;
            }
        }
        r.ok = true;
        r.text = has_bom ? "\xEF\xBB\xBF" + out : out;
        return r;
    } catch (const BudgetExceeded&) {
        r.error = "input too large or complex to format";
        return r;
    } catch (const UnsupportedNode& u) {
        r.error = std::string("unsupported syntax: ") + jsparse::NodeTypeName(u.node->type);
        r.error_line = u.node ? LineOf(text, u.node->start) : 0;
        return r;
    } catch (const ArgExpansionBailout&) {
        r.error = "internal error (argument expansion)";
        return r;
    } catch (const std::exception& e) {
        r.error = std::string("internal error: ") + e.what();
        return r;
    }
}

}  // namespace jsfmt
