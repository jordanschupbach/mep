// mep's Julia formatter: the spacing, literal, semicolon, `in` and `return`
// passes of Runic.jl 1.5.0's runestone.jl (MIT, Copyright (c) 2024 Fredrik
// Ekre and contributors), in runestone.jl's order. See
// julia_format_internal.h for how the port maps Runic's types.

#include <algorithm>

#include "julia_format_internal.h"

namespace juliafmt::detail {

namespace {

Node WsNode() { return Leaf(K("Whitespace"), jlparse::TRIVIA_FLAG, 1); }

// Length of a PCRE `\h` (horizontal space) character at s[p], 0 if none.
size_t HSpaceLen(std::string_view s, size_t p) {
    if (p >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[p]);
    if (c == ' ' || c == '\t') return 1;
    uint32_t cp = 0;
    size_t n = 0;
    if (c == 0xc2 && p + 1 < s.size()) {
        cp = static_cast<unsigned char>(s[p + 1]) & 0x3fu;
        cp |= 0x80u;
        n = 2;
    } else if ((c & 0xf0) == 0xe0 && p + 2 < s.size()) {
        cp = ((c & 0x0fu) << 12) | ((static_cast<unsigned char>(s[p + 1]) & 0x3fu) << 6) |
             (static_cast<unsigned char>(s[p + 2]) & 0x3fu);
        n = 3;
    } else {
        return 0;
    }
    if (cp == 0xa0 || cp == 0x1680 || cp == 0x180e || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f ||
        cp == 0x205f || cp == 0x3000)
        return n;
    return 0;
}

size_t Utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;
}

// replace(str, r"\h*(\r\n|\r|\n)" => '\n')
std::string NormalizeNewlines(std::string_view s) {
    std::string out;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = p;
        while (size_t n = HSpaceLen(s, q)) q += n;
        if (q < s.size() && (s[q] == '\n' || s[q] == '\r')) {
            out += '\n';
            q += (s[q] == '\r' && q + 1 < s.size() && s[q + 1] == '\n') ? 2 : 1;
            p = q;
            continue;
        }
        size_t n = std::min(Utf8Len(static_cast<unsigned char>(s[p])), s.size() - p);
        out.append(s.substr(p, n));
        p += n;
    }
    return out;
}

// replace(str, r"\n\h*" => '\n')
std::string StripAfterNewlines(std::string_view s) {
    std::string out;
    size_t p = 0;
    while (p < s.size()) {
        if (s[p] == '\n') {
            out += '\n';
            ++p;
            while (size_t n = HSpaceLen(s, p)) p += n;
            continue;
        }
        out += s[p++];
    }
    return out;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

ONode trim_trailing_whitespace(Ctx &ctx, const Node &node) {
    if (!KIn(node.kind, K("NewlineWs"), K("Comment"))) return std::nullopt;
    JF_ASSERT(node.leaf());
    std::string str = ctx.read_bytes(node);
    std::string str2;
    if (node.kind == K("NewlineWs")) {
        str2 = NormalizeNewlines(str);
        if (ctx.next_sibling == K("NewlineWs")) str2 = StripAfterNewlines(str2);
    } else {
        str2 = str;
        while (!str2.empty() && (str2.back() == ' ' || str2.back() == '\t')) str2.pop_back();
    }
    if (str == str2) return std::nullopt;
    uint32_t nb = ctx.replace_bytes(str2, node.span);
    JF_ASSERT(nb != node.span);
    return make_leaf(node, nb);
}

ONode replace_tabs_with_four_spaces(Ctx &ctx, const Node &node) {
    if (!KIn(node.kind, K("Whitespace"), K("NewlineWs"))) return std::nullopt;
    JF_ASSERT(node.leaf());
    std::string bytes = ctx.read_bytes(node);
    if (bytes.find('\t') == std::string::npos) return std::nullopt;
    std::string out;
    for (char c : bytes) {
        if (c == '\t')
            out += "    ";
        else
            out += c;
    }
    uint32_t nb = ctx.replace_bytes(out, node.span);
    return make_leaf(node, nb);
}

ONode format_hex_literals(Ctx &ctx, const Node &node) {
    if (node.kind != K("HexInt")) return std::nullopt;
    JF_ASSERT(node.flags == 0);
    JF_ASSERT(node.leaf());
    uint32_t spn = node.span;
    JF_ASSERT(spn > 2);
    static const uint32_t kTargets[] = {4, 6, 10, 18, 34};
    if (spn >= 34) return std::nullopt;
    for (uint32_t t : kTargets)
        if (spn == t) return std::nullopt;
    uint32_t target = 0;
    for (uint32_t t : kTargets) {
        if (t > spn) {
            target = t;
            break;
        }
    }
    std::string bytes = ctx.read_bytes(node);
    while (bytes.size() < target) bytes.insert(2, 1, '0');
    uint32_t nb = ctx.replace_bytes(bytes, spn);
    JF_ASSERT(nb == target);
    return Leaf(node.kind, node.flags, nb);
}

namespace {

// occursin(r"^[+-]?(?:[1-9]\d*|0)\.(?:\d*[1-9]|0)(?:[ef][+-]?(?:[1-9]\d*|0))?$", s)
bool FloatIsCanonical(std::string_view s) {
    size_t p = 0;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    size_t a = p;
    while (p < s.size() && IsDigit(s[p])) ++p;
    std::string_view ip = s.substr(a, p - a);
    if (ip.empty() || (ip[0] == '0' && ip.size() != 1)) return false;
    if (p >= s.size() || s[p] != '.') return false;
    ++p;
    a = p;
    while (p < s.size() && IsDigit(s[p])) ++p;
    std::string_view fp = s.substr(a, p - a);
    if (fp.empty() || !(fp.back() != '0' || fp == "0")) return false;
    if (p == s.size()) return true;
    if (s[p] != 'e' && s[p] != 'f') return false;
    ++p;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    a = p;
    while (p < s.size() && IsDigit(s[p])) ++p;
    std::string_view ep = s.substr(a, p - a);
    if (p != s.size()) return false;
    return !ep.empty() && (ep[0] != '0' || ep == "0");
}

std::string StripLeadingZeros(std::string s) {
    size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') ++i;
    return s.substr(i);
}

}  // namespace

ONode format_float_literals(Ctx &ctx, const Node &node) {
    if (!KIn(node.kind, K("Float"), K("Float32"))) return std::nullopt;
    JF_ASSERT(node.flags == 0);
    JF_ASSERT(node.leaf());
    std::string str = ctx.read_bytes(node);
    if (FloatIsCanonical(str)) return std::nullopt;
    if (str.find('_') != std::string::npos || str.find("0x") != std::string::npos) return std::nullopt;
    static const std::string kMinus = "\xe2\x88\x92";  // U+2212
    size_t p = 0;
    std::string sgn;
    if (p < str.size() && (str[p] == '+' || str[p] == '-')) {
        sgn = str.substr(p, 1);
        ++p;
    } else if (str.compare(p, 3, kMinus) == 0) {
        sgn = "-";
        p += 3;
    }
    size_t a = p;
    while (p < str.size() && IsDigit(str[p])) ++p;
    std::string ip = str.substr(a, p - a);
    if (p < str.size() && str[p] == '.') ++p;
    a = p;
    while (p < str.size() && IsDigit(str[p])) ++p;
    std::string fp = str.substr(a, p - a);
    std::string epm, ep;
    if (p < str.size() && (str[p] == 'e' || str[p] == 'E' || str[p] == 'f')) {
        epm = str[p] == 'E' ? "e" : str.substr(p, 1);
        ++p;
        if (p < str.size() && (str[p] == '+' || str[p] == '-')) {
            epm += str[p];
            ++p;
        } else if (str.compare(p, 3, kMinus) == 0) {
            epm += '-';
            p += 3;
        }
        a = p;
        while (p < str.size() && IsDigit(str[p])) ++p;
        ep = str.substr(a, p - a);
        if (ep.empty()) throw Refuse("TypeError: float literal ::RegexMatch");
    }
    if (p != str.size()) throw Refuse("TypeError: float literal ::RegexMatch");
    std::string out = sgn;
    out += StripLeadingZeros(ip.empty() ? "0" : ip);
    out += '.';
    std::string f = fp.empty() ? "0" : fp;
    while (f.size() > 1 && f.back() == '0') f.pop_back();
    out += f;
    if (!epm.empty()) {
        out += epm;
        out += StripLeadingZeros(ep);
    }
    uint32_t nb = ctx.replace_bytes(out, node.span);
    return Leaf(node.kind, node.flags, nb);
}

namespace {

template <class IsX> ONode spaces_around_x(Ctx &ctx, const Node &node, IsX is_x, int n_leaves_per_x = 1) {
    JF_ASSERT(!node.leaf());
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    Node ws = WsNode();
    bool looking_for_whitespace = false;
    bool looking_for_x = false;
    int n_x_leaves_visited = 0;
    auto peek = [&](int i) { return i < len(kids) ? at(kids, i + 1).kind : kNoKind; };

    auto after_x = [&](const Node &kid) {
        if (kid.kind == K("Comment")) {
        } else if (looking_for_x) {
            JF_ASSERT(is_x(kid));
            n_x_leaves_visited += 1;
            if (n_x_leaves_visited == n_leaves_per_x) {
                looking_for_x = false;
                n_x_leaves_visited = 0;
            } else {
                looking_for_whitespace = false;
            }
        } else {
            looking_for_x = kid.kind != K("Comment");
        }
    };

    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        if (kid.kind == K("NewlineWs") || (i == 1 && kid.kind == K("Whitespace"))) {
            ctx.accept(kid);
            if (any_changes) kids2->push_back(kid);
            looking_for_whitespace = false;
        } else if (looking_for_whitespace) {
            if ((kid.kind == K("Whitespace") && kid.span == 1) ||
                (kid.kind == K("Whitespace") && peek(i) == K("Comment") && peek(i + 1) == K("NewlineWs"))) {
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
                looking_for_whitespace = false;
            } else if (kid.kind == K("Whitespace")) {
                any_changes = true;
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                kids2->push_back(ws);
                ctx.replace_bytes(" ", kid.span);
                ctx.accept(ws);
                looking_for_whitespace = false;
            } else {
                any_changes = true;
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                kids2->push_back(ws);
                ctx.replace_bytes(" ", 0);
                ctx.accept(ws);
                kids2->push_back(kid);
                ctx.accept(kid);
                looking_for_whitespace = kindof(last_leaf(kid)) != K("Whitespace");
                after_x(kid);
            }
        } else {
            JF_ASSERT(kid.kind != K("Whitespace"));
            if (any_changes) kids2->push_back(kid);
            ctx.accept(kid);
            looking_for_whitespace = kindof(last_leaf(kid)) != K("Whitespace");
            after_x(kid);
        }
    }
    ctx.seek(pos);
    if (any_changes) return make_node(node, kids2);
    return std::nullopt;
}

bool IsListSep(const Node &x) { return is_ws(x) || KIn(x.kind, K(","), K(";")); }

}  // namespace

ONode spaces_in_listlike(Ctx &ctx, const Node &node) {
    if (!(KIn(node.kind, K("tuple"), K("parameters"), K("curly"), K("braces"), K("bracescat"), K("vect"), K("ref"),
              K("parens")) ||
          (node.kind == K("call") && node.flags == 0) || (node.kind == K("dotcall") && node.flags == 0) ||
          (node.kind == K("macrocall") && has_flags(node, jlparse::PARENS_FLAG)) || is_paren_block(node)))
        return std::nullopt;
    if (node.kind == K("parameters")) {
        JF_ASSERT(KIn(ctx.lineage_end(), K("tuple"), K("call"), K("dotcall"), K("macrocall"), K("curly"), K("vect"),
                      K("ref"), K("braces")));
    }
    JF_ASSERT(!node.leaf());
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    auto peek = [&](int i) { return i < len(kids) ? at(kids, i + 1).kind : kNoKind; };
    Node ws = WsNode();
    Node comma = Leaf(K(","), jlparse::TRIVIA_FLAG, 1);

    bool implicit_tuple = false;
    int opening_leaf_idx = 0, closing_leaf_idx = 0;
    if (KIn(node.kind, K("tuple"), K("call"), K("dotcall"), K("parens"), K("macrocall")) || is_paren_block(node)) {
        opening_leaf_idx = findfirst(IsKind(K("(")), kids);
        if (opening_leaf_idx == 0) {
            implicit_tuple = true;
            JF_ASSERT(node.kind == K("tuple"));
            opening_leaf_idx = findfirst(NotWs(), kids);
            if (opening_leaf_idx == 0) return std::nullopt;
            closing_leaf_idx = need(findlast(NotWs(), kids));
            if (opening_leaf_idx == closing_leaf_idx) return std::nullopt;
            opening_leaf_idx -= 1;
            closing_leaf_idx += 1;
            JF_ASSERT(opening_leaf_idx + 1 > len(kids) || findnext(IsKind(K(")")), kids, opening_leaf_idx + 1) == 0);
        } else {
            closing_leaf_idx = need(findnext(IsKind(K(")")), kids, opening_leaf_idx + 1));
            if (closing_leaf_idx == opening_leaf_idx + 1) return std::nullopt;
        }
    } else if (KIn(node.kind, K("curly"), K("braces"), K("bracescat"))) {
        opening_leaf_idx = need(findfirst(IsKind(K("{")), kids));
        closing_leaf_idx = need(findnext(IsKind(K("}")), kids, opening_leaf_idx + 1));
        if (closing_leaf_idx == opening_leaf_idx + 1) return std::nullopt;
    } else if (KIn(node.kind, K("vect"), K("ref"))) {
        opening_leaf_idx = need(findfirst(IsKind(K("[")), kids));
        closing_leaf_idx = need(findnext(IsKind(K("]")), kids, opening_leaf_idx + 1));
        if (closing_leaf_idx == opening_leaf_idx + 1) return std::nullopt;
    } else {
        JF_ASSERT(node.kind == K("parameters"));
        opening_leaf_idx = need(findfirst(IsKind(K(";")), kids));
        closing_leaf_idx = len(kids) + 1;
    }

    int n_items = 0;
    for (int j = opening_leaf_idx + 1; j <= closing_leaf_idx - 1; ++j)
        if (!IsListSep(at(kids, j))) ++n_items;
    auto not_sep = [](const Node &x) { return !IsListSep(x); };
    int first_item_idx = opening_leaf_idx + 1 <= len(kids) ? findnext(not_sep, kids, opening_leaf_idx + 1) : 0;
    if (first_item_idx != 0 && first_item_idx >= closing_leaf_idx) first_item_idx = 0;
    int last_item_idx = closing_leaf_idx - 1 >= 1 ? findprev(not_sep, kids, closing_leaf_idx - 1) : 0;
    if (last_item_idx != 0 && last_item_idx <= opening_leaf_idx) last_item_idx = 0;

    const bool multiline = is_multiline_between_idxs(ctx, node, opening_leaf_idx, closing_leaf_idx);
    const bool is_named_tuple =
        node.kind == K("tuple") && n_items == 1 && at(kids, need(first_item_idx)).kind == K("parameters");

    bool require_trailing_comma = false;
    bool allow_trailing_semi = false;
    bool allow_trailing_comma = multiline;
    if (KIn(node.kind, K("call"), K("dotcall"), K("macrocall"))) {
        require_trailing_comma = false;
    } else if (n_items > 0 && at(kids, need(last_item_idx)).kind == K("generator")) {
        require_trailing_comma = false;
    } else if (implicit_tuple) {
        require_trailing_comma = n_items == 1 && ctx.lineage_end() == K("=");
        allow_trailing_comma = ctx.lineage_end() == K("=");
    } else if (node.kind == K("tuple") && n_items == 1 && ctx.lineage_end() != K("function") &&
               at(kids, need(first_item_idx)).kind != K("parameters")) {
        require_trailing_comma = true;
    } else if (KIn(node.kind, K("bracescat"), K("parens"))) {
        require_trailing_comma = false;
    } else if (node.kind == K("block")) {
        require_trailing_comma = false;
        allow_trailing_semi = n_items < 2;
    } else if (node.kind == K("parameters")) {
        require_trailing_comma = has_tag(node, TAG_TRAILING_COMMA);
        allow_trailing_comma = has_tag(node, TAG_TRAILING_COMMA_OPT);
    } else if (n_items > 0 && predicate_contains(
                                  [](const Node &nd) {
                                      return nd.kind == K("macrocall") && !has_flags(nd, jlparse::PARENS_FLAG) &&
                                             !is_string_macro(nd);
                                  },
                                  at(kids, last_item_idx))) {
        require_trailing_comma = false;
    } else if (multiline) {
        require_trailing_comma = true;
    } else if (n_items > 0) {
        for (int j = last_item_idx + 1; j <= closing_leaf_idx - 1; ++j)
            if (at(kids, j).kind == K("NewlineWs")) require_trailing_comma = true;
    }

    enum State { expect_item, expect_comma, expect_space, expect_closing };
    auto state_after_item = [&](int i) {
        JF_ASSERT(i <= last_item_idx);
        if (i < last_item_idx) return expect_comma;
        if (i == last_item_idx && require_trailing_comma) {
            if (at(kids, last_item_idx).kind == K("parameters")) return expect_closing;
            return expect_comma;
        }
        return expect_closing;
    };
    auto count_items = [](const Node &k) {
        int c = 0;
        for (const Node &x : *verified_kids(k))
            if (!IsListSep(x)) ++c;
        return c;
    };

    State state = (node.kind == K("parameters") && n_items > 0) ? expect_space
                  : n_items > 0                                 ? expect_item
                                                                : expect_closing;
    bool any_kid_changed = false;
    size_t pos = ctx.position();
    for (int i = 1; i <= opening_leaf_idx; ++i) ctx.accept(at(kids, i));

    auto cow = [&](int n) {
        if (kids2 == kids) kids2 = SliceKids(kids, n);
    };

    for (int i = opening_leaf_idx + 1; i <= closing_leaf_idx - 1; ++i) {
        Node kid = at(kids, i);
        bool this_kid_changed = false;
        if (state == expect_item) {
            if (kid.kind == K("Whitespace") && peek(i) != K("Comment")) {
                ctx.replace_bytes("", kid.span);
                this_kid_changed = true;
                cow(i - 1);
            } else if (kid.kind == K("NewlineWs") || (kid.kind == K("Whitespace") && peek(i) == K("Comment"))) {
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
            } else if (kid.kind == K("Comment")) {
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
                state = expect_space;
            } else {
                JF_ASSERT(!jlparse::is_whitespace(kindof(first_leaf(kid))));
                if (kid.kind == K("parameters") && require_trailing_comma && i == last_item_idx &&
                    !has_tag(kid, TAG_TRAILING_COMMA)) {
                    kid = add_tag(kid, TAG_TRAILING_COMMA);
                    this_kid_changed = true;
                }
                if (kid.kind == K("parameters") && allow_trailing_comma && i == last_item_idx &&
                    !has_tag(kid, TAG_TRAILING_COMMA_OPT)) {
                    kid = add_tag(kid, TAG_TRAILING_COMMA_OPT);
                    this_kid_changed = true;
                }
                if (kid.kind == K("parameters") && !require_trailing_comma && !is_named_tuple &&
                    count_items(kid) == 0) {
                    const KidsP &grandkids = verified_kids(kid);
                    JF_ASSERT(len(grandkids) == 1 && at(grandkids, 1).kind == K(";"));
                    any_kid_changed = true;
                    cow(i - 1);
                    ctx.replace_bytes("", kid.span);
                } else {
                    any_kid_changed = any_kid_changed || this_kid_changed;
                    if (any_kid_changed) {
                        cow(i - 1);
                        kids2->push_back(kid);
                    }
                    ctx.accept(kid);
                }
                state = state_after_item(i);
            }
        } else if (state == expect_comma) {
            const bool trailing = i > last_item_idx;
            if (kid.kind == K(",") || kid.kind == K(";")) {
                const bool before_last_item = i < last_item_idx;
                if (before_last_item || require_trailing_comma) {
                    ctx.accept(kid);
                    if (any_kid_changed) kids2->push_back(kid);
                } else {
                    JF_UNREACHABLE();
                }
                state = before_last_item ? expect_space : expect_closing;
            } else if (kid.kind == K("Whitespace") && peek(i) != K("Comment")) {
                this_kid_changed = true;
                cow(i - 1);
                ctx.replace_bytes("", kid.span);
            } else if (kid.kind == K("NewlineWs") || (kid.kind == K("Whitespace") && peek(i) == K("Comment")) ||
                       kid.kind == K("Comment")) {
                int next_non_ws_idx = 0;
                for (int j = i + 1; j <= closing_leaf_idx - 1; ++j) {
                    if (!is_ws(at(kids, j))) {
                        next_non_ws_idx = j;
                        break;
                    }
                }
                Kind next_kind = next_non_ws_idx == 0 ? kNoKind : at(kids, next_non_ws_idx).kind;
                if (trailing && next_kind != K(",")) {
                    JF_ASSERT(require_trailing_comma);
                    this_kid_changed = true;
                    cow(i - 1);
                    ctx.replace_bytes(",", 0);
                    kids2->push_back(comma);
                    ctx.accept(comma);
                    state = expect_closing;
                }
                any_kid_changed = any_kid_changed || this_kid_changed;
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
            } else if (kid.kind == K("parameters")) {
                JF_ASSERT(KIn(node.kind, K("call"), K("dotcall"), K("macrocall"), K("curly"), K("tuple"), K("vect"),
                              K("ref"), K("braces")));
                if (require_trailing_comma && !has_tag(kid, TAG_TRAILING_COMMA)) {
                    kid = add_tag(kid, TAG_TRAILING_COMMA);
                    this_kid_changed = true;
                }
                if (allow_trailing_comma && !has_tag(kid, TAG_TRAILING_COMMA_OPT)) {
                    kid = add_tag(kid, TAG_TRAILING_COMMA_OPT);
                    this_kid_changed = true;
                }
                if (!require_trailing_comma && count_items(kid) == 0) {
                    const KidsP &grandkids = verified_kids(kid);
                    JF_ASSERT(len(grandkids) == 1 && at(grandkids, 1).kind == K(";"));
                    any_kid_changed = true;
                    ctx.replace_bytes("", kid.span);
                    cow(i - 1);
                } else {
                    any_kid_changed = any_kid_changed || this_kid_changed;
                    ctx.accept(kid);
                    if (any_kid_changed) {
                        cow(i - 1);
                        kids2->push_back(kid);
                    }
                }
                state = i == last_item_idx ? expect_closing : expect_item;
            } else {
                JF_UNREACHABLE();
            }
        } else if (state == expect_space) {
            if ((kid.kind == K("Whitespace") && kid.span == 1) ||
                (kid.kind == K("Whitespace") && peek(i) == K("Comment"))) {
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
                state = expect_item;
            } else if (kid.kind == K("Whitespace")) {
                this_kid_changed = true;
                cow(i - 1);
                ctx.replace_bytes(" ", kid.span);
                ctx.accept(ws);
                kids2->push_back(ws);
                state = expect_item;
            } else if (kid.kind == K("NewlineWs")) {
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
                state = expect_item;
            } else {
                JF_ASSERT(!KIn(kid.kind, K(","), K(";")));
                this_kid_changed = true;
                cow(i - 1);
                ctx.replace_bytes(" ", 0);
                kids2->push_back(ws);
                ctx.accept(ws);
                kids2->push_back(kid);
                ctx.accept(kid);
                state = state_after_item(i);
            }
        } else {
            if ((kid.kind == K(",") && !allow_trailing_comma) || (kid.kind == K(";") && !allow_trailing_semi) ||
                (kid.kind == K("Whitespace") && peek(i) != K("Comment"))) {
                this_kid_changed = true;
                cow(i - 1);
                ctx.replace_bytes("", kid.span);
            } else if ((node.kind == K("block") && kid.kind == K(";") && allow_trailing_semi) ||
                       (kid.kind == K(",") && allow_trailing_comma) ||
                       (kid.kind == K("Whitespace") && peek(i) != K("Comment"))) {
                allow_trailing_semi = n_items == 0;
                allow_trailing_comma = false;
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
            } else if (kid.kind == K("NewlineWs") || (kid.kind == K("Whitespace") && peek(i) == K("Comment")) ||
                       kid.kind == K("Comment")) {
                ctx.accept(kid);
                if (any_kid_changed) kids2->push_back(kid);
            } else {
                JF_UNREACHABLE();
            }
        }
        any_kid_changed = any_kid_changed || this_kid_changed;
    }
    if (state != expect_closing) {
        if (state == expect_comma) {
            JF_ASSERT(at(kids, last_item_idx).kind != K("parameters"));
            JF_ASSERT(require_trailing_comma);
            any_kid_changed = true;
            cow(closing_leaf_idx - 1);
            ctx.replace_bytes(",", 0);
            kids2->push_back(comma);
            ctx.accept(comma);
            state = expect_closing;
        } else {
            JF_UNREACHABLE();
        }
    }
    for (int i = closing_leaf_idx; i <= len(kids); ++i) {
        ctx.accept(at(kids, i));
        if (any_kid_changed) kids2->push_back(at(kids, i));
    }
    ctx.seek(pos);
    if (any_kid_changed) return make_node(node, kids2);
    JF_ASSERT(kids == kids2);
    return std::nullopt;
}

ONode spaces_around_operators(Ctx &ctx, const Node &node) {
    if (!((is_infix_op_call(node) && !KIn(infix_op_call_op(node).kind, K(":"), K("^"))) ||
          (KIn(node.kind, K("<:"), K(">:")) && meta_nargs(node) == 3) ||
          (node.kind == K("comparison") && !is_trivia(node))))
        return std::nullopt;
    JF_ASSERT(KIn(node.kind, K("call"), K("dotcall"), K("comparison"), K("<:"), K(">:")));
    int n = node.kind == K("dotcall") ? 2 : 1;
    return spaces_around_x(
        ctx, node, [](const Node &x) { return is_operator_leaf(x) || is_comparison_leaf(x); }, n);
}

ONode spaces_around_assignments(Ctx &ctx, const Node &node) {
    if (!(is_assignment(node) && !node.leaf())) return std::nullopt;
    return spaces_around_x(ctx, node, [](const Node &x) { return is_assignment(x) || KIn(x.kind, K("in"), K("∈")); });
}

ONode spaces_around_anonymous_function(Ctx &ctx, const Node &node) {
    if (!(node.kind == K("->") && !node.leaf())) return std::nullopt;
    return spaces_around_x(ctx, node, [](const Node &x) { return x.kind == K("->"); });
}

ONode spaces_around_ternary(Ctx &ctx, const Node &node) {
    if (!(node.kind == K("?") && !node.leaf())) return std::nullopt;
    return spaces_around_x(ctx, node, [](const Node &x) { return x.leaf() && KIn(x.kind, K("?"), K(":")); });
}

namespace {

template <class IsX> ONode no_spaces_around_x(Ctx &ctx, const Node &node, IsX is_x) {
    JF_ASSERT(!node.leaf());
    for (const Node &c : *node.kids)
        if (KIn(c.kind, K("NewlineWs"), K("Comment"))) return std::nullopt;
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    bool looking_for_x = false;
    need(findfirst(is_x, kids));
    need(findlast(is_x, kids));
    if (KIn(node.kind, K("::"), K("<:"), K(">:"))) looking_for_x = is_x(first_non_whitespace_kid(node));
    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        if (kid.kind == K("Whitespace")) {
            JF_ASSERT(1 < i && i < len(kids));
            any_changes = true;
            if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
            ctx.replace_bytes("", kid.span);
        } else {
            JF_ASSERT(!is_ws(kid));
            if (looking_for_x) JF_ASSERT(is_x(kid));
            if (any_changes) {
                if (kids == kids2) kids2 = SliceKids(kids, i - 1);
                kids2->push_back(kid);
            }
            ctx.accept(kid);
            looking_for_x = !looking_for_x;
        }
    }
    ctx.seek(pos);
    if (any_changes) {
        Node n2 = make_node(node, kids2);
        JF_ASSERT(n2.span < node.span);
        return n2;
    }
    return std::nullopt;
}

bool InQuoteOrMacrocall(const Ctx &ctx) {
    for (Kind k : ctx.lineage_kinds)
        if (KIn(k, K("quote"), K("macrocall"))) return true;
    return false;
}

}  // namespace

ONode spaces_in_export_public(Ctx &ctx, const Node &node) {
    if (node.leaf()) return std::nullopt;
    if (!(KIn(node.kind, K("export"), K("public")) || is_global_local_list(node))) return std::nullopt;
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    Node spacenode = WsNode();
    JF_ASSERT(at(kids, 1).leaf() && KIn(at(kids, 1).kind, K("export"), K("public"), K("global"), K("local")));
    ctx.accept(at(kids, 1));
    enum { expect_space, expect_identifier, expect_comma } state = expect_space;
    int i = 2;
    while (i <= len(kids)) {
        Node kid = at(kids, i);
        if (state == expect_space) {
            state = expect_identifier;
            if (kid.kind == K("NewlineWs") || (kid.kind == K("Whitespace") && kid.span == 1)) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
            } else if (kid.kind == K("Whitespace")) {
                ctx.replace_bytes(" ", kid.span);
                any_changes = true;
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                ctx.accept(spacenode);
                kids2->push_back(spacenode);
            } else if (kid.kind == K("Comment")) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
                state = expect_space;
            } else {
                any_changes = true;
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                ctx.replace_bytes(" ", 0);
                kids2->push_back(spacenode);
                ctx.accept(spacenode);
                continue;
            }
        } else if (state == expect_identifier) {
            state = expect_comma;
            if (KIn(kid.kind, K("Identifier"), K("@"), K("MacroName"), K("$"), K("var")) ||
                jlparse::is_operator(kid.kind)) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
                if (kid.kind == K("@")) state = expect_identifier;
                if (kid.kind == K("$")) JF_ASSERT(InQuoteOrMacrocall(ctx));
            } else if (kid.kind == K("parens")) {
                JF_ASSERT(kindof(first_leaf(kid)) != K("Whitespace"));
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
            } else if (KIn(kid.kind, K("Comment"), K("NewlineWs"))) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
                state = kid.kind == K("Comment") ? expect_space : expect_identifier;
            } else {
                JF_UNREACHABLE();
            }
        } else {
            state = expect_space;
            if (kid.kind == K(",")) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
            } else if (kid.kind == K("Whitespace")) {
                any_changes = true;
                ctx.replace_bytes("", kid.span);
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                state = expect_comma;
            } else {
                JF_UNREACHABLE();
            }
        }
        i += 1;
    }
    ctx.seek(pos);
    if (any_changes) return make_node(node, kids2);
    return std::nullopt;
}

ONode spaces_in_let(Ctx &ctx, const Node &node) {
    size_t p = ctx.position();
    if (node.kind != K("let") || node.leaf()) return std::nullopt;
    const KidsP &let_kids = node.kids;
    Node let_leaf = at(let_kids, 1);
    JF_ASSERT(let_leaf.kind == K("let") && let_leaf.leaf());
    const int vars_idx = 2;
    Node vars_node = at(let_kids, vars_idx);
    JF_ASSERT(!vars_node.leaf() && vars_node.kind == K("block"));
    const KidsP &kids = vars_node.kids;
    if (len(kids) == 0) {
        JF_ASSERT(vars_node.span == 0);
        return std::nullopt;
    }
    ctx.accept(let_leaf);
    int idx = 1;
    JF_ASSERT(at(kids, idx).kind == K("Whitespace"));
    idx = need(idx + 1 <= len(kids) ? findnext(NotWs(), kids, idx + 1) : 0);
    for (int i = 1; i <= idx; ++i) ctx.accept(at(kids, i));
    enum { expect_comma, expect_space, expect_var } state = expect_comma;
    bool changed = false;
    KidsP kids2 = kids;
    idx += 1;
    Node space = WsNode();
    while (idx <= len(kids)) {
        Node kid = at(kids, idx);
        if (state == expect_comma) {
            state = expect_space;
            if (kid.kind == K(",")) {
                ctx.accept(kid);
                if (changed) kids2->push_back(kid);
            } else if (kid.kind == K("Comment") || kmatch(kids, {K("Whitespace"), K("Comment")}, idx)) {
                state = expect_comma;
                ctx.accept(kid);
                if (changed) kids2->push_back(kid);
            } else if (kid.kind == K("Whitespace")) {
                JF_ASSERT(!kmatch(kids, {K("Comment")}, idx + 1));
                state = expect_comma;
                changed = true;
                if (kids2 == kids) kids2 = SliceKids(kids, idx - 1);
                ctx.replace_bytes("", kid.span);
            } else {
                JF_UNREACHABLE();
            }
        } else if (state == expect_space) {
            state = expect_var;
            if (kid.kind == K("NewlineWs") || (kid.kind == K("Whitespace") && kid.span == 1) ||
                kmatch(kids, {K("Whitespace"), K("Comment")}, idx)) {
                ctx.accept(kid);
                if (changed) kids2->push_back(kid);
            } else if (kid.kind == K("Comment")) {
                ctx.accept(kid);
                if (changed) kids2->push_back(kid);
                state = expect_space;
            } else if (kid.kind == K("Whitespace")) {
                if (kids2 == kids) kids2 = SliceKids(kids, idx - 1);
                kids2->push_back(space);
                changed = true;
                ctx.replace_bytes(" ", kid.span);
                ctx.accept(space);
            } else {
                JF_ASSERT(!is_ws(kid));
                if (kids2 == kids) kids2 = SliceKids(kids, idx - 1);
                kids2->push_back(space);
                changed = true;
                ctx.replace_bytes(" ", 0);
                ctx.accept(space);
                continue;
            }
        } else {
            state = expect_comma;
            if (KIn(kid.kind, K("Comment"), K("NewlineWs"))) {
                ctx.accept(kid);
                if (changed) kids2->push_back(kid);
                state = kid.kind == K("Comment") ? expect_space : expect_var;
            } else {
                JF_ASSERT(!is_ws(kid));
                ctx.accept(kid);
                if (changed) kids2->push_back(kid);
            }
        }
        idx += 1;
    }
    ctx.seek(p);
    if (changed) {
        Node vars2 = make_node(vars_node, kids2);
        KidsP lk = CopyKids(let_kids);
        at(lk, vars_idx) = vars2;
        return make_node(node, lk);
    }
    return std::nullopt;
}

namespace {

ONode format_as(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("as"));
    const KidsP &kids = verified_kids(node);
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    Node spacebar = WsNode();
    int idx = 1;
    Node kid = at(kids, idx);
    JF_ASSERT(kid.kind == K("importpath"));
    JF_ASSERT(!jlparse::is_whitespace(kindof(first_leaf(kid))));
    ctx.accept(kid);
    auto space = [&]() {
        idx += 1;
        Node k = at(kids, idx);
        JF_ASSERT(k.kind == K("Whitespace"));
        if (k.span == 1) {
            ctx.accept(k);
            if (any_changes) kids2->push_back(k);
        } else {
            ctx.replace_bytes(" ", k.span);
            any_changes = true;
            if (kids2 == kids) kids2 = SliceKids(kids, idx - 1);
            ctx.accept(spacebar);
            kids2->push_back(spacebar);
        }
    };
    space();
    idx += 1;
    kid = at(kids, idx);
    JF_ASSERT(kid.kind == K("as"));
    ctx.accept(kid);
    if (any_changes) kids2->push_back(kid);
    space();
    idx += 1;
    kid = at(kids, idx);
    JF_ASSERT(KIn(kid.kind, K("Identifier"), K("$"), K("@")));
    if (!kid.leaf()) JF_ASSERT(kindof(first_leaf(kid)) != K("Whitespace"));
    if (kid.kind == K("$")) JF_ASSERT(InQuoteOrMacrocall(ctx));
    ctx.accept(kid);
    if (any_changes) kids2->push_back(kid);
    if (kid.kind == K("@")) {
        idx += 1;
        kid = at(kids, idx);
        JF_ASSERT(kid.kind == K("MacroName"));
        ctx.accept(kid);
        if (any_changes) kids2->push_back(kid);
    }
    ctx.seek(pos);
    if (any_changes) return make_node(node, kids2);
    return std::nullopt;
}

}  // namespace

ONode spaces_in_import_using(Ctx &ctx, const Node &node) {
    if (!(KIn(node.kind, K("import"), K("using")) && !node.leaf())) return std::nullopt;
    KidsP kids = node.kids;
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    const bool colon_list = at(kids, 1).kind == K(":");
    Node colon_node;
    if (colon_list) {
        colon_node = at(kids, 1);
        JF_ASSERT(len(kids) == 1);
        kids = verified_kids(colon_node);
        kids2 = kids;
    }
    JF_ASSERT(KIn(at(kids, 1).kind, K("import"), K("using")));
    ctx.accept(at(kids, 1));
    Node spacebar = WsNode();
    enum { expect_space, expect_item, expect_comma } state = expect_space;
    int i = 2;
    while (i <= len(kids)) {
        Node kid = at(kids, i);
        if (state == expect_item) {
            state = expect_comma;
            if (KIn(kid.kind, K("importpath"), K("as"))) {
                ONode kid2;
                if (kid.kind == K("importpath")) {
                    JF_ASSERT(!jlparse::is_whitespace(kindof(first_leaf(kid))));
                } else {
                    kid2 = format_as(ctx, kid);
                }
                if (kid2) {
                    any_changes = true;
                    if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                    ctx.accept(*kid2);
                    kids2->push_back(*kid2);
                } else {
                    ctx.accept(kid);
                    if (any_changes) kids2->push_back(kid);
                }
            } else if (KIn(kid.kind, K("Comment"), K("NewlineWs"))) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
                state = kid.kind == K("Comment") ? expect_space : expect_item;
            } else {
                JF_UNREACHABLE();
            }
        } else if (state == expect_comma) {
            state = expect_space;
            if (kid.kind == K("Whitespace")) {
                any_changes = true;
                ctx.replace_bytes("", kid.span);
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                state = expect_comma;
            } else {
                JF_ASSERT(KIn(kid.kind, K(":"), K(",")));
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
            }
        } else {
            state = expect_item;
            if (kid.kind == K("NewlineWs") || (kid.kind == K("Whitespace") && kid.span == 1)) {
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
            } else if (kid.kind == K("Whitespace")) {
                any_changes = true;
                ctx.replace_bytes(" ", kid.span);
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                ctx.accept(spacebar);
                kids2->push_back(spacebar);
            } else if (kid.kind == K("Comment")) {
                if (any_changes) kids2->push_back(kid);
                ctx.accept(kid);
                state = expect_space;
            } else {
                JF_ASSERT(KIn(kid.kind, K("Identifier"), K("importpath"), K("as")));
                any_changes = true;
                ctx.replace_bytes(" ", 0);
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                ctx.accept(spacebar);
                kids2->push_back(spacebar);
                continue;
            }
        }
        i += 1;
    }
    ctx.seek(pos);
    if (any_changes) {
        if (colon_list) {
            Node c2 = make_node(colon_node, kids2);
            KidsP one = NewKids();
            one->push_back(c2);
            return make_node(node, one);
        }
        return make_node(node, kids2);
    }
    return std::nullopt;
}

ONode no_spaces_around_colon_etc(Ctx &ctx, const Node &node) {
    if (!((is_infix_op_call(node) && KIn(infix_op_call_op(node).kind, K(":"), K("^"))) ||
          (node.kind == K("::") && !node.leaf()) || (KIn(node.kind, K("<:"), K(">:")) && meta_nargs(node) == 2)))
        return std::nullopt;
    JF_ASSERT(KIn(node.kind, K("call"), K("::"), K("<:"), K(">:")));
    return no_spaces_around_x(ctx, node, [](const Node &x) {
        return x.leaf() && KIn(x.kind, K(":"), K("^"), K("::"), K("<:"), K(">:"));
    });
}

namespace {

ONode space_after_let(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("let") && !node.leaf());
    size_t p = ctx.position();
    const KidsP &kids = node.kids;
    Node let_node = at(kids, 1);
    JF_ASSERT(let_node.kind == K("let"));
    ctx.accept(let_node);
    const int vars_idx = 2;
    Node vars_node = at(kids, vars_idx);
    JF_ASSERT(vars_node.kind == K("block"));
    const KidsP &vars_kids = verified_kids(vars_node);
    if (len(vars_kids) == 0) {
        JF_ASSERT(vars_node.span == 0);
        ctx.seek(p);
        return std::nullopt;
    }
    Node vars_kid = at(vars_kids, 1);
    JF_ASSERT(vars_kid.kind == K("Whitespace"));
    if (vars_kid.span == 1) {
        ctx.seek(p);
        return std::nullopt;
    }
    ctx.replace_bytes(" ", vars_kid.span);
    KidsP vk = CopyKids(vars_kids);
    at(vk, 1) = WsNode();
    Node vars2 = make_node(vars_node, vk);
    KidsP k2 = CopyKids(kids);
    at(k2, vars_idx) = vars2;
    ctx.seek(p);
    return make_node(node, k2);
}

}  // namespace

ONode spaces_around_keywords(Ctx &ctx, const Node &node) {
    if (node.leaf()) return std::nullopt;
    if (node.kind == K("let")) return space_after_let(ctx, node);
    auto in_keyword_set = [](Kind k) {
        return KIn(k, K("where"), K("do"), K("mutable"), K("struct"), K("abstract"), K("primitive"), K("type"),
                   K("function"), K("if"), K("elseif"), K("catch"), K("while"), K("return"), K("local"), K("global"),
                   K("const"), K("module"), K("baremodule"));
    };
    if (!in_keyword_set(node.kind)) return std::nullopt;
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    Node ws = WsNode();
    auto peek_kind = [](Kind k) { return KIn(k, K("where"), K("do")); };
    enum { peeking_for_keyword, looking_for_keyword, looking_for_space, closing } state =
        peek_kind(node.kind) ? peeking_for_keyword : looking_for_keyword;
    bool keep_looking_for_keywords = false;
    bool space_after = true;

    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        if (state == peeking_for_keyword) {
            Node nkid = at(kids, i + 1);
            if (peek_kind(nkid.kind)) {
                state = looking_for_space;
                keep_looking_for_keywords = true;
                space_after = false;
            } else {
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
                continue;
            }
        }
        if (state == looking_for_keyword) {
            if (in_keyword_set(kid.kind)) {
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
                if (KIn(kid.kind, K("mutable"), K("abstract"), K("primitive"))) keep_looking_for_keywords = true;
                state = looking_for_space;
                if (node.kind == K("do")) {
                    int tupleidx = need(i + 1 <= len(kids) ? findnext(IsKind(K("tuple")), kids, i + 1) : 0);
                    Node tuple = at(kids, tupleidx);
                    bool any = false;
                    for (const Node &x : *verified_kids(tuple))
                        if (!(is_ws(x) || x.kind == K(";"))) any = true;
                    if (!any) state = closing;
                }
                if (node.kind == K("catch")) {
                    Node nkid = at(kids, i + 1);
                    if (nkid.kind == K("false") && nkid.span == 0) state = closing;
                }
            } else {
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
            }
        } else if (state == looking_for_space) {
            if ((kid.kind == K("Whitespace") && kid.span == 1) || kid.kind == K("NewlineWs")) {
                if (kid.kind == K("NewlineWs"))
                    JF_ASSERT(KIn(node.kind, K("where"), K("local"), K("global"), K("const")));
                ctx.accept(kid);
                if (any_changes) kids2->push_back(kid);
            } else if (kid.kind == K("Whitespace")) {
                any_changes = true;
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                ctx.replace_bytes(" ", kid.span);
                kids2->push_back(ws);
                ctx.accept(ws);
            } else {
                JF_ASSERT(kindof(first_leaf(kid)) != K("Whitespace"));
                JF_ASSERT(KIn(node.kind, K("where"), K("if"), K("elseif"), K("while"), K("do"), K("function"),
                              K("return"), K("local"), K("global"), K("module"), K("baremodule")));
                any_changes = true;
                if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
                if (!space_after) {
                    kids2->push_back(kid);
                    ctx.accept(kid);
                }
                ctx.replace_bytes(" ", 0);
                kids2->push_back(ws);
                ctx.accept(ws);
                if (space_after) {
                    kids2->push_back(kid);
                    ctx.accept(kid);
                }
            }
            state = keep_looking_for_keywords ? looking_for_keyword : closing;
            keep_looking_for_keywords = false;
            space_after = true;
        } else {
            JF_ASSERT(state == closing);
            ctx.accept(kid);
            if (any_changes) kids2->push_back(kid);
        }
    }
    ctx.seek(pos);
    if (any_changes) return make_node(node, kids2);
    return std::nullopt;
}

namespace {

ONode replace_with_in(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("=") && !node.leaf() && meta_nargs(node) == 3);
    const KidsP &kids = node.kids;
    size_t pos = ctx.position();
    int vars_index = need(findfirst(NotWs(), kids));
    int in_index = vars_index + 1 <= len(kids) ? findnext(NotWs(), kids, vars_index + 1) : 0;
    Node in_node = at(kids, in_index);
    if (in_node.kind == K("in")) {
        JF_ASSERT(is_trivia(in_node));
        JF_ASSERT(in_node.leaf());
        JF_ASSERT(ctx.position() == pos);
        return std::nullopt;
    }
    JF_ASSERT(KIn(in_node.kind, K("∈"), K("=")));
    JF_ASSERT(is_trivia(in_node));
    JF_ASSERT(in_node.leaf());
    for (int i = 1; i <= in_index - 1; ++i) ctx.accept(at(kids, i));
    uint32_t nb = ctx.replace_bytes("in", in_node.span);
    Node in2 = Leaf(K("in"), jlparse::TRIVIA_FLAG, nb);
    ctx.accept(in2);
    KidsP k2 = CopyKids(kids);
    at(k2, in_index) = in2;
    ctx.seek(pos);
    return make_node(node, k2);
}

ONode replace_with_in_cartesian(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("cartesian_iterator") && !node.leaf());
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    size_t pos = ctx.position();
    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        if (kid.kind == K("=")) {
            ONode k2 = replace_with_in(ctx, kid);
            if (k2) {
                if (kids2 == kids) kids2 = CopyKids(kids);
                at(kids2, i) = *k2;
                ctx.accept(*k2);
            } else {
                at(kids2, i) = kid;
                ctx.accept(kid);
            }
        } else {
            at(kids2, i) = kid;
            ctx.accept(kid);
        }
    }
    ctx.seek(pos);
    if (kids == kids2) return std::nullopt;
    return make_node(node, kids2);
}

ONode replace_with_in_filter(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("filter") && !node.leaf());
    size_t pos = ctx.position();
    const KidsP &kids = node.kids;
    Node kid = at(kids, 1);
    JF_ASSERT(KIn(kid.kind, K("="), K("cartesian_iterator")) && !kid.leaf());
    ONode k2 = kid.kind == K("=") ? replace_with_in(ctx, kid) : replace_with_in_cartesian(ctx, kid);
    ctx.seek(pos);
    if (!k2) return std::nullopt;
    KidsP kk = CopyKids(kids);
    at(kk, 1) = *k2;
    return make_node(node, kk);
}

}  // namespace

ONode for_loop_use_in(Ctx &ctx, const Node &node) {
    if (!((node.kind == K("for") && !node.leaf() && meta_nargs(node) == 4) || node.kind == K("generator")))
        return std::nullopt;
    size_t pos = ctx.position();
    const KidsP &kids = verified_kids(node);
    KidsP kids2 = kids;
    auto is_for_leaf = [](const Node &c) { return c.kind == K("for") && c.leaf(); };
    int for_index = need(findfirst(is_for_leaf, kids));
    int next_index = 1;
    bool any_for_changed = false;
    while (for_index != 0) {
        Node for_node = at(kids, for_index);
        JF_ASSERT(for_node.kind == K("for") && for_node.span == 3 && for_node.leaf() && is_trivia(for_node));
        for (int i = next_index; i <= for_index; ++i) ctx.accept(at(kids, i));
        int spec_index =
            need(findnext([](const Node &x) { return KIn(x.kind, K("="), K("cartesian_iterator"), K("filter")); },
                          kids, for_index + 1));
        Node spec = at(kids, spec_index);
        for (int i = for_index + 1; i <= spec_index - 1; ++i) ctx.accept(at(kids, i));
        ONode spec2;
        if (spec.kind == K("="))
            spec2 = replace_with_in(ctx, spec);
        else if (spec.kind == K("filter"))
            spec2 = replace_with_in_filter(ctx, spec);
        else {
            JF_ASSERT(spec.kind == K("cartesian_iterator"));
            spec2 = replace_with_in_cartesian(ctx, spec);
        }
        if (spec2) {
            any_for_changed = true;
            if (kids2 == kids) kids2 = CopyKids(kids);
            at(kids2, spec_index) = *spec2;
            ctx.accept(*spec2);
        } else {
            ctx.accept(spec);
        }
        for_index = spec_index + 1 <= len(kids) ? findnext(is_for_leaf, kids, spec_index + 1) : 0;
        if (for_index != 0) JF_ASSERT(node.kind == K("generator"));
        next_index = spec_index + 1;
    }
    if (!any_for_changed) {
        ctx.seek(pos);
        return std::nullopt;
    }
    for (int i = next_index; i <= len(kids2); ++i) ctx.accept(at(kids2, i));
    Node n2 = make_node(node, kids2);
    ctx.seek(pos);
    return n2;
}

ONode braces_around_where_rhs(Ctx &ctx, const Node &node) {
    if (!(node.kind == K("where") && !node.leaf())) return std::nullopt;
    const KidsP &kids = node.kids;
    size_t pos = ctx.position();
    int where_idx = need(findfirst([](const Node &x) { return x.leaf() && x.kind == K("where"); }, kids));
    int rhs_idx = need(where_idx + 1 <= len(kids) ? findnext(NotWs(), kids, where_idx + 1) : 0);
    JF_ASSERT(rhs_idx == len(kids));
    Node rhs = at(kids, rhs_idx);
    if (rhs.kind == K("braces")) return std::nullopt;
    KidsP kids2 = SliceKids(kids, rhs_idx - 1);
    for (int i = 1; i <= rhs_idx - 1; ++i) ctx.accept(at(kids, i));
    Node ob = Leaf(K("{"), 0, 1);
    Node cb = Leaf(K("}"), 0, 1);
    KidsP bk = NewKids();
    bk->push_back(ob);
    bk->push_back(rhs);
    bk->push_back(cb);
    Node rhs2 = Inner(K("braces"), 0, bk);
    kids2->push_back(rhs2);
    ctx.replace_bytes("{", 0);
    ctx.accept(ob);
    ctx.accept(rhs);
    ctx.replace_bytes("}", 0);
    ctx.accept(cb);
    ctx.seek(pos);
    return make_node(node, kids2);
}

ONode parens_around_op_calls_in_colon(Ctx &ctx, const Node &node) {
    if (!(is_infix_op_call(node) && infix_op_call_op(node).kind == K(":"))) return std::nullopt;
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any_changes = false;
    size_t pos = ctx.position();
    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        if (is_infix_op_call(kid)) {
            if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
            const KidsP &grandkids = verified_kids(kid);
            JF_ASSERT(need(findfirst(NotWs(), grandkids)) == 1);
            JF_ASSERT(need(findlast(NotWs(), grandkids)) == len(grandkids));
            Node op = Leaf(K("("), 0, 1);
            ctx.replace_bytes("(", 0);
            ctx.accept(op);
            KidsP pk = NewKids();
            pk->push_back(op);
            Node kid2 = make_node(kid, CopyKids(grandkids));
            ctx.accept(kid2);
            pk->push_back(kid2);
            Node cp = Leaf(K(")"), 0, 1);
            ctx.replace_bytes(")", 0);
            ctx.accept(cp);
            pk->push_back(cp);
            kids2->push_back(Inner(K("parens"), 0, pk));
            any_changes = true;
        } else {
            ctx.accept(kid);
            if (any_changes) kids2->push_back(kid);
        }
    }
    ctx.seek(pos);
    if (any_changes) return make_node(node, kids2);
    return std::nullopt;
}

namespace {
bool egal(const ONode &a, const ONode &b) {
    if (!a || !b) return !a && !b;
    return a->kind == b->kind && a->flags == b->flags && a->span == b->span && a->kids == b->kids &&
           a->tags == b->tags;
}
}  // namespace

ONode no_leading_and_single_trailing_newline(Ctx &ctx, const Node &node_in) {
    if (!(ctx.filemode && ctx.lineage_kinds.empty())) return std::nullopt;
    JF_ASSERT(node_in.kind == K("toplevel"));
    JF_ASSERT(!node_in.leaf());
    JF_ASSERT(ctx.position() == 0);
    Node node = node_in;
    bool changed = false;
    while (true) {
        ONode l = first_leaf(node);
        if (!(l && KIn(l->kind, K("NewlineWs"), K("Whitespace")) && len(verified_kids(node)) > 1)) break;
        changed = true;
        ctx.replace_bytes("", l->span);
        node = *replace_first_leaf(node, std::nullopt);
    }
    ctx.accept(node);
    ONode l = last_leaf(node);
    if (!l || l->kind != K("NewlineWs")) {
        KidsP k2 = CopyKids(verified_kids(node));
        k2->push_back(Leaf(K("NewlineWs"), jlparse::TRIVIA_FLAG, 1));
        ctx.replace_bytes("\n", 0);
        changed = true;
        node = make_node(node, k2);
    } else {
        ONode ll = second_last_leaf(node);
        while (ll && l->kind == ll->kind && ll->kind == K("NewlineWs")) {
            changed = true;
            ctx.seek(ctx.position() - l->span);
            node = *replace_last_leaf(node, std::nullopt);
            JF_ASSERT(egal(last_leaf(node), ll));
            l = ll;
            ll = second_last_leaf(node);
        }
    }
    if (changed) return node;
    ctx.seek(0);
    return std::nullopt;
}

ONode max_three_consecutive_newlines(Ctx &ctx, const Node &node) {
    if (node.leaf()) return std::nullopt;
    const KidsP &kids = node.kids;
    int idx = findfirst(IsKind(K("NewlineWs")), kids);
    while (idx != 0) {
        if (idx + 3 <= len(kids) && at(kids, idx + 1).kind == K("NewlineWs") &&
            at(kids, idx + 2).kind == K("NewlineWs") && at(kids, idx + 3).kind == K("NewlineWs")) {
            KidsP k2 = NewKids();
            for (int i = 1; i <= len(kids); ++i) {
                Node kid = at(kids, i);
                if (i == idx) {
                    ctx.replace_bytes("", kid.span);
                } else {
                    ctx.accept(kid);
                    k2->push_back(kid);
                }
            }
            return make_node(node, k2);
        }
        idx = idx + 1 <= len(kids) ? findnext(IsKind(K("NewlineWs")), kids, idx + 1) : 0;
    }
    return std::nullopt;
}

namespace {

ONode remove_trailing_semicolon_block(Ctx &ctx, const Node &node) {
    if (node.kind != K("block")) return std::nullopt;
    JF_ASSERT(!node.leaf());
    size_t pos = ctx.position();
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    int semi_idx = findfirst(IsKind(K(";")), kids2);
    while (semi_idx != 0) {
        int search_index = semi_idx + 1;
        if (kmatch(kids2, {K(";"), K("NewlineWs")}, semi_idx)) {
            if (kids2 == kids) kids2 = CopyKids(kids);
            const bool space_before = kmatch(kids2, {K("Whitespace"), K(";")}, semi_idx - 1);
            uint32_t span_overwrite = 0;
            int nodes_to_skip_over = 0;
            if (space_before) {
                span_overwrite = at(kids2, semi_idx - 1).span + at(kids2, semi_idx).span;
                nodes_to_skip_over = semi_idx - 2;
                kids2->erase(kids2->begin() + (semi_idx - 1));
                kids2->erase(kids2->begin() + (semi_idx - 2));
                search_index = semi_idx - 1;
            } else {
                span_overwrite = at(kids2, semi_idx).span;
                nodes_to_skip_over = semi_idx - 1;
                kids2->erase(kids2->begin() + (semi_idx - 1));
                search_index = semi_idx;
            }
            size_t p = ctx.position();
            for (int i = 1; i <= nodes_to_skip_over; ++i) ctx.accept(at(kids2, i));
            ctx.replace_bytes("", span_overwrite);
            ctx.seek(p);
        } else if (kmatch(kids2, {K(";"), K("Comment"), K("NewlineWs")}, semi_idx) ||
                   kmatch(kids2, {K(";"), K("Whitespace"), K("Comment"), K("NewlineWs")}, semi_idx)) {
            if (kids2 == kids) kids2 = CopyKids(kids);
            uint32_t ws_span = at(kids2, semi_idx).span;
            JF_ASSERT(ws_span == 1);
            const bool space_before = kmatch(kids2, {K("Whitespace"), K(";")}, semi_idx - 1);
            if (space_before) ws_span += at(kids2, semi_idx - 1).span;
            const bool space_after = kmatch(kids2, {K(";"), K("Whitespace")}, semi_idx);
            if (space_after) ws_span += at(kids2, semi_idx + 1).span;
            size_t p = ctx.position();
            for (int i = 1; i <= semi_idx - 1; ++i) ctx.accept(at(kids2, i));
            ctx.replace_bytes(" ", at(kids2, semi_idx).span);
            ctx.seek(p);
            JF_ASSERT(at(kids2, semi_idx).kind == K(";"));
            at(kids2, semi_idx) = Leaf(K("Whitespace"), jlparse::TRIVIA_FLAG, ws_span);
            if (space_after) kids2->erase(kids2->begin() + semi_idx);
            if (space_before) kids2->erase(kids2->begin() + (semi_idx - 2));
        }
        semi_idx = search_index <= len(kids2) ? findnext(IsKind(K(";")), kids2, search_index) : 0;
    }
    ctx.seek(pos);
    if (kids2 == kids) return std::nullopt;
    return make_node(node, kids2);
}

}  // namespace

ONode remove_trailing_semicolon(Ctx &ctx, const Node &node) {
    if (!node.leaf() && is_begin_block(node)) return remove_trailing_semicolon_block(ctx, node);
    if (!(!node.leaf() && KIn(node.kind, K("if"), K("elseif"), K("quote"), K("function"), K("for"), K("let"),
                              K("while"), K("macro"), K("try"), K("catch"), K("finally"), K("else"), K("do"),
                              K("struct"))))
        return std::nullopt;
    if (node.kind == K("quote") && has_flags(node, jlparse::COLON_QUOTE)) return std::nullopt;
    size_t pos = ctx.position();
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    auto block_predicate = [](const Node &x) { return x.kind == K("block") && !has_flags(x, jlparse::PARENS_FLAG); };
    int block_idx = findfirst(block_predicate, kids2);
    if (node.kind == K("let"))
        block_idx = block_idx + 1 <= len(kids2) ? findnext(block_predicate, kids2, block_idx + 1) : 0;
    bool any_changed = false;
    while (block_idx != 0) {
        size_t p = ctx.position();
        for (int i = 1; i <= block_idx - 1; ++i) ctx.accept(at(kids2, i));
        ONode b2 = remove_trailing_semicolon_block(ctx, at(kids2, block_idx));
        if (b2) {
            any_changed = true;
            if (kids2 == kids) kids2 = CopyKids(kids);
            at(kids2, block_idx) = *b2;
        }
        ctx.seek(p);
        block_idx = block_idx + 1 <= len(kids2) ? findnext(block_predicate, kids2, block_idx + 1) : 0;
    }
    ctx.seek(pos);
    if (any_changed) return make_node(node, kids2);
    return std::nullopt;
}

ONode spaces_around_comments(Ctx &ctx, const Node &node) {
    if (node.leaf()) return std::nullopt;
    size_t pos = ctx.position();
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool prev_kid_ends_with_ws = true;
    Node ws = WsNode();
    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        Node kid2 = kid;
        bool starts_with_comment = kid.kind == K("Comment");
        if (!prev_kid_ends_with_ws && !starts_with_comment) {
            ONode fl = first_leaf(kid);
            starts_with_comment = fl && fl->kind == K("Comment");
        }
        if (!prev_kid_ends_with_ws && starts_with_comment) {
            if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
            ctx.replace_bytes(" ", 0);
            if (kid.kind == K("Comment")) {
                kids2->push_back(ws);
                ctx.accept(ws);
            } else {
                kid2 = add_before_first_leaf(kid, ws);
                JF_ASSERT(kid2.span == kid.span + 1);
            }
        }
        if (kids2 != kids) kids2->push_back(kid2);
        ctx.accept(kid2);
        prev_kid_ends_with_ws =
            KIn(kid2.kind, K("Whitespace"), K("NewlineWs"), K("("), K("{"), K("["));
        if (!prev_kid_ends_with_ws) {
            ONode ll = last_leaf(kid2);
            prev_kid_ends_with_ws = ll && KIn(ll->kind, K("Whitespace"), K("NewlineWs"));
        }
    }
    ctx.seek(pos);
    if (kids == kids2) return std::nullopt;
    return make_node(node, kids2);
}

namespace {

Node return_node(Ctx &ctx, const Node &ret) {
    KidsP kids = NewKids();
    kids->push_back(Leaf(K("return"), 0, 6));
    if (!ret.leaf()) JF_ASSERT(!KIn(kindof(first_leaf(ret)), K("NewlineWs"), K("Whitespace")));
    ctx.replace_bytes("return ", 0);
    kids->push_back(WsNode());
    kids->push_back(ret);
    return Inner(K("return"), 0, kids);
}

bool has_return(const Node &node) {
    const KidsP &kids = verified_kids(node);
    if (KIn(node.kind, K("let"), K("catch"), K("else"), K("finally"))) {
        int idx = need(findfirst(IsKind(K("block")), kids));
        if (node.kind == K("let")) idx = need(idx + 1 <= len(kids) ? findnext(IsKind(K("block")), kids, idx + 1) : 0);
        return has_return(at(kids, idx));
    }
    if (KIn(node.kind, K("try"), K("if"), K("elseif"))) {
        auto pred = [](const Node &x) {
            return !x.leaf() && KIn(x.kind, K("catch"), K("else"), K("finally"), K("elseif"), K("block"));
        };
        int idx = findfirst(pred, kids);
        while (idx != 0) {
            if (has_return(at(kids, idx))) return true;
            idx = idx + 1 <= len(kids) ? findnext(pred, kids, idx + 1) : 0;
        }
        return false;
    }
    if (node.kind == K("macrocall")) {
        if (findfirst(IsKind(K("return")), kids) != 0) return true;
        for (const Node &x : *kids)
            if (!x.leaf() && KIn(x.kind, K("let"), K("try"), K("if"), K("block"), K("macrocall")) && has_return(x))
                return true;
        return false;
    }
    if (node.kind == K("block")) return findfirst(IsKind(K("return")), kids) != 0;
    JF_UNREACHABLE();
}

ONode explicit_return_block(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("block"));
    if (has_return(node)) return std::nullopt;
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    int rexpr_idx = findlast(NotWs(), kids2);
    if (rexpr_idx == 0) return std::nullopt;
    Node rexpr = at(kids2, rexpr_idx);
    JF_ASSERT(rexpr.kind != K("return"));
    const Kind rk = rexpr.kind;
    const bool simple =
        rexpr.leaf() ||
        KIn(rk, K("call"), K("dotcall"), K("tuple"), K("vect"), K("ref"), K("hcat"), K("typed_hcat"), K("vcat"),
            K("typed_vcat"), K("?"), K("&&"), K("||"), K("::"), K("juxtapose"), K("<:"), K(">:"), K("comparison"),
            K("string"), K("."), K("->"), K("comprehension"), K("do"), K("macro"), K("typed_comprehension"),
            K("where"), K("parens"), K("curly"), K("function"), K("quote"), K("global"), K("local"), K("=")) ||
        is_string_macro(rexpr) || is_assignment(rexpr) ||
        (KIn(rk, K("let"), K("if"), K("try")) && !has_return(rexpr)) ||
        (rk == K("macrocall") && !has_return(rexpr)) || (is_begin_block(rexpr) && !has_return(rexpr)) ||
        (rk == K("quote") && has_flags(rexpr, jlparse::COLON_QUOTE));
    if (simple) {
        for (int i = 1; i <= rexpr_idx - 1; ++i) ctx.accept(at(kids2, i));
        if (rk == K("call")) {
            const KidsP &call_kids = verified_kids(rexpr);
            int fname_idx = need(findfirst(NotWs(), call_kids));
            JF_ASSERT(fname_idx == 1);
            size_t p = ctx.position();
            std::string fname = ctx.read_bytes(at(call_kids, fname_idx));
            ctx.seek(p);
            if (fname.find("throw") != std::string::npos || fname.find("error") != std::string::npos)
                return std::nullopt;
        }
        if (kids2 == kids) kids2 = CopyKids(kids);
        if (!kmatch(kids2, {K("NewlineWs")}, rexpr_idx - 1)) {
            uint32_t spn = 0;
            JF_ASSERT(kindof(first_leaf(rexpr)) != K("NewlineWs"));
            if (rexpr_idx > 1) {
                Node prev = at(kids2, rexpr_idx - 1);
                if (!prev.leaf()) JF_ASSERT(!KIn(kindof(last_leaf(prev)), K("Whitespace"), K("NewlineWs")));
            }
            if (kmatch(kids2, {K("Whitespace")}, rexpr_idx - 1)) {
                spn = at(kids2, rexpr_idx - 1).span;
                kids2->erase(kids2->begin() + (rexpr_idx - 2));
                ctx.seek(ctx.position() - spn);
                rexpr_idx -= 1;
                JF_ASSERT(kindof(first_leaf(rexpr)) != K("Whitespace"));
            }
            JF_ASSERT(kindof(first_leaf(rexpr)) != K("Whitespace"));
            std::string nl = "\n" + std::string(static_cast<size_t>(4 * std::max(ctx.indent_level, 0)), ' ');
            if (ctx.indent_level < 0) throw Refuse("ArgumentError: negative repeat");
            Node nlnode = Leaf(K("NewlineWs"), jlparse::TRIVIA_FLAG, static_cast<uint32_t>(nl.size()));
            kids2->insert(kids2->begin() + (rexpr_idx - 1), nlnode);
            rexpr_idx += 1;
            ctx.replace_bytes(nl, spn);
            ctx.accept(nlnode);
        }
        Node ret = return_node(ctx, rexpr);
        at(kids2, rexpr_idx) = ret;
        return make_node(node, kids2);
    }
    if (KIn(rk, K("for"), K("while"))) {
        JF_ASSERT(at(kids2, len(kids2)).kind == K("NewlineWs"));
        JF_ASSERT(kindof(last_leaf(rexpr)) == K("end"));
        int insert_idx = len(kids);
        if (kids2 == kids) kids2 = CopyKids(kids);
        for (int i = 1; i <= insert_idx - 1; ++i) ctx.accept(at(kids2, i));
        if (ctx.indent_level < 0) throw Refuse("ArgumentError: negative repeat");
        std::string nl = "\n" + std::string(static_cast<size_t>(4 * ctx.indent_level), ' ');
        Node nlnode = Leaf(K("NewlineWs"), jlparse::TRIVIA_FLAG, static_cast<uint32_t>(nl.size()));
        kids2->insert(kids2->begin() + (insert_idx - 1), nlnode);
        ctx.replace_bytes(nl, 0);
        ctx.accept(nlnode);
        ctx.replace_bytes("return", 0);
        KidsP rk2 = NewKids();
        rk2->push_back(Leaf(K("return"), 0, 6));
        kids2->insert(kids2->begin() + insert_idx, Inner(K("return"), 0, rk2));
        return make_node(node, kids2);
    }
    return std::nullopt;
}

}  // namespace

ONode explicit_return(Ctx &ctx, const Node &node) {
    if (!(!node.leaf() && KIn(node.kind, K("function"), K("macro")))) return std::nullopt;
    if (!safe_to_insert_return(ctx)) return std::nullopt;
    const KidsP &kids = node.kids;
    size_t pos = ctx.position();
    int block_idx = findlast(IsKind(K("block")), kids);
    if (block_idx == 0) return std::nullopt;
    for (int i = 1; i <= block_idx - 1; ++i) ctx.accept(at(kids, i));
    ONode b2 = explicit_return_block(ctx, at(kids, block_idx));
    ctx.seek(pos);
    if (!b2) return std::nullopt;
    KidsP k2 = CopyKids(kids);
    at(k2, block_idx) = *b2;
    return make_node(node, k2);
}

}  // namespace juliafmt::detail
