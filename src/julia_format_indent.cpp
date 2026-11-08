// mep's Julia formatter: the indentation passes of Runic.jl 1.5.0's
// runestone.jl (MIT, Copyright (c) 2024 Fredrik Ekre and contributors) --
// four_space_indent, insert_delete_mark_newlines with the indent_* family, and
// indent_multiline_strings. See julia_format_internal.h for the port's map.

#include "julia_format_internal.h"

namespace juliafmt::detail {

namespace {

Node NlNode(uint32_t span) { return Leaf(K("NewlineWs"), jlparse::TRIVIA_FLAG, span); }

std::string Spaces(int n) {
    if (n < 0) throw Refuse("ArgumentError: negative repeat");
    return std::string(static_cast<size_t>(n), ' ');
}

ONode indent_block(Ctx &ctx, const Node &node, bool allow_empty = true, bool do_indent = true);

// The view helpers of indent_block: `v` is the parent vector, `o` the view offset.
struct View {
    KidsP p;
    int o = 0;
    int len() const { return detail::len(p) - 2 * o; }
    Node &operator[](int i) const { return at(p, i + o); }
};

}  // namespace

ONode four_space_indent(Ctx &ctx, const Node &node) {
    if (node.kind != K("NewlineWs")) return std::nullopt;
    if (ctx.next_sibling == K("NewlineWs")) return std::nullopt;
    std::string bytes = ctx.read_bytes(node);
    JF_ASSERT(bytes.find('\r') == std::string::npos);
    JF_ASSERT(!bytes.empty() && bytes[0] == '\n');
    int indent_level = ctx.indent_level;
    if (has_tag(node, TAG_PRE_DEDENT)) indent_level -= 1;
    if (has_tag(node, TAG_LINE_CONT)) indent_level += 1;
    int spn2 = 1 + 4 * indent_level;
    if (spn2 < 1) throw Refuse("ArgumentError: negative resize");
    if (static_cast<uint32_t>(spn2) == node.span) return std::nullopt;
    std::string out = "\n" + Spaces(spn2 - 1);
    ctx.replace_bytes(out, node.span);
    return Node{node.kind, node.flags, static_cast<uint32_t>(spn2), nullptr, node.tags};
}

namespace {

ONode indent_function_or_macro(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    bool any = false;
    const int func_idx = 1;
    Node func_node = at(kids, func_idx);
    JF_ASSERT(func_node.leaf() && KIn(func_node.kind, K("function"), K("macro")));
    if (!has_tag(func_node, TAG_INDENT)) {
        at(kids, func_idx) = add_tag(func_node, TAG_INDENT);
        any = true;
    }
    int sig_idx = need(func_idx + 1 <= len(kids) ? findnext(NotWs(), kids, func_idx + 1) : 0);
    Node sig = at(kids, sig_idx);
    auto end_tag = [&](int from) {
        int end_idx = need(from <= len(kids) ? findnext(IsKind(K("end")), kids, from) : 0);
        Node end_node = at(kids, end_idx);
        JF_ASSERT(end_node.leaf() && end_node.kind == K("end"));
        if (!has_tag(end_node, TAG_DEDENT)) {
            at(kids, end_idx) = add_tag(end_node, TAG_DEDENT);
            any = true;
        }
    };
    if (sig.kind == K("Identifier") || !KIn(sig.kind, K("call"), K("where"), K("::"), K("tuple"), K("parens"))) {
        end_tag(sig_idx + 1);
        return any ? ONode(make_node(node, kids)) : std::nullopt;
    }
    JF_ASSERT(!sig.leaf());
    int block_idx = sig_idx + 1;
    {
        size_t p = ctx.position();
        for (int i = 1; i <= block_idx - 1; ++i) ctx.accept(at(kids, i));
        ONode b = indent_block(ctx, at(kids, block_idx));
        if (b) {
            at(kids, block_idx) = *b;
            any = true;
        }
        ctx.seek(p);
    }
    end_tag(block_idx + 1);
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_let_varblock(Ctx &, const Node &node) {
    JF_ASSERT(node.kind == K("block") && !node.leaf());
    const KidsP &kids = node.kids;
    bool changed = false;
    if (len(kids) == 0) {
        JF_ASSERT(node.span == 0);
        return std::nullopt;
    }
    int i = 1;
    JF_ASSERT(at(kids, i).kind == K("Whitespace"));
    i = i + 1 <= len(kids) ? findnext(NotWs(), kids, i + 1) : 0;
    if (i == 0) return std::nullopt;
    JF_ASSERT(KIn(at(kids, i).kind, K("Identifier"), K("="), K("$"), K("macrocall")));
    while (true) {
        i = i + 1 <= len(kids) ? findnext(IsKind(K("NewlineWs")), kids, i + 1) : 0;
        if (i == 0) break;
        if (!has_tag(at(kids, i), TAG_LINE_CONT)) {
            at(kids, i) = add_tag(at(kids, i), TAG_LINE_CONT);
            changed = true;
        }
    }
    return changed ? ONode(make_node(node, kids)) : std::nullopt;
}

// Run indent_block (or `f`) on kids[idx] with the stream advanced over kids[1:idx-1].
template <class F> bool SubBlock(Ctx &ctx, const KidsP &kids, int idx, F f) {
    size_t p = ctx.position();
    for (int i = 1; i <= idx - 1; ++i) ctx.accept(at(kids, i));
    ONode b = f(at(kids, idx));
    bool changed = false;
    if (b) {
        at(kids, idx) = *b;
        changed = true;
    }
    ctx.seek(p);
    return changed;
}

ONode indent_let(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    bool any = false;
    Node let_node = at(kids, 1);
    JF_ASSERT(let_node.leaf() && let_node.kind == K("let"));
    if (!has_tag(let_node, TAG_INDENT)) {
        at(kids, 1) = add_tag(let_node, TAG_INDENT);
        any = true;
    }
    const int vars_idx = 2;
    Node vars_node = at(kids, vars_idx);
    JF_ASSERT(!vars_node.leaf() && vars_node.kind == K("block"));
    any |= SubBlock(ctx, kids, vars_idx, [&](const Node &n) { return indent_let_varblock(ctx, n); });
    int block_idx = need(findnext(IsKind(K("block")), kids, vars_idx + 1));
    JF_ASSERT(!at(kids, block_idx).leaf());
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    int end_idx = need(block_idx + 1 <= len(kids) ? findnext(IsKind(K("end")), kids, block_idx + 1) : 0);
    JF_ASSERT(at(kids, end_idx).leaf());
    if (!has_tag(at(kids, end_idx), TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(at(kids, end_idx), TAG_DEDENT);
        any = true;
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_begin(Ctx &ctx, const Node &node_in, Kind block_kind = K("begin")) {
    JF_ASSERT(node_in.kind == K("block"));
    size_t pos = ctx.position();
    Node node = node_in;
    ONode n2 = indent_block(ctx, node);
    if (n2) node = *n2;
    const KidsP &kids = verified_kids(node);
    bool any = false;
    Node begin_node = at(kids, 1);
    JF_ASSERT(begin_node.leaf() && begin_node.kind == block_kind);
    if (!has_tag(begin_node, TAG_INDENT)) {
        at(kids, 1) = add_tag(begin_node, TAG_INDENT);
        any = true;
    }
    int end_idx = findlast(IsKind(K("end")), kids);
    JF_ASSERT(end_idx == len(kids));
    int ln_idx = end_idx - 1;
    Node ln = at(kids, ln_idx);
    if (ln.kind == K("NewlineWs") && !has_tag(ln, TAG_PRE_DEDENT)) {
        at(kids, ln_idx) = add_tag(ln, TAG_PRE_DEDENT);
        any = true;
    }
    Node end_node = at(kids, end_idx);
    JF_ASSERT(end_node.leaf() && end_node.kind == K("end"));
    if (!has_tag(end_node, TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(end_node, TAG_DEDENT);
        any = true;
    }
    ctx.seek(pos);
    // Note: Runic resets any_kid_changed after indent_block, so a block that
    // only changed there is not returned (the kids were mutated in place).
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_block(Ctx &ctx, const Node &node, bool allow_empty, bool do_indent) {
    JF_ASSERT(node.kind == K("block") && !node.leaf());
    JF_ASSERT(!has_flags(node, jlparse::PARENS_FLAG));
    const KidsP &kids = node.kids;
    size_t pos = ctx.position();
    bool any = false;
    const bool is_begin_end = len(kids) > 2 && KIn(at(kids, 1).kind, K("begin"), K("quote")) &&
                              at(kids, len(kids)).kind == K("end");
    const int o = is_begin_end ? 1 : 0;
    View v{kids, o};
    if (is_begin_end) ctx.accept(at(kids, 1));

    auto vfind_first = [&](auto pred) {
        for (int i = 1; i <= v.len(); ++i)
            if (pred(v[i])) return i;
        return 0;
    };
    auto vfind_last = [&](auto pred) {
        for (int i = v.len(); i >= 1; --i)
            if (pred(v[i])) return i;
        return 0;
    };
    auto vkmatch = [&](std::initializer_list<Kind> ks) {
        if (static_cast<int>(ks.size()) > v.len()) return false;
        int j = 1;
        for (Kind k : ks)
            if (v[j++].kind != k) return false;
        return true;
    };
    auto is_nl = [](const Node &x) { return x.kind == K("NewlineWs"); };

    if (allow_empty && vfind_first(NotWs()) == 0 && vfind_first(is_nl) == 0) return std::nullopt;

    int trailing_idx = vfind_last(is_nl);
    if (trailing_idx == 0 || trailing_idx != v.len()) {
        v.p = CopyKids(kids);
        size_t p = ctx.position();
        for (int i = 1; i <= v.len(); ++i) ctx.accept(v[i]);
        if (v.len() > 0 && v[v.len()].kind == K("Whitespace")) {
            uint32_t spn = v[v.len()].span;
            ctx.seek(ctx.position() - spn);
            ctx.replace_bytes("", spn);
            v.p->erase(v.p->begin() + (v.len() + o - 1));
        }
        ctx.replace_bytes("\n", 0);
        Node k = NlNode(1);
        if (do_indent) k = add_tag(k, TAG_PRE_DEDENT);
        v.p->insert(v.p->begin() + (v.len() + o), k);
        ctx.seek(p);
        any = true;
    } else if (do_indent && !has_tag(v[trailing_idx], TAG_PRE_DEDENT)) {
        v.p = CopyKids(kids);
        v[trailing_idx] = add_tag(v[trailing_idx], TAG_PRE_DEDENT);
        any = true;
    }
    trailing_idx = need(vfind_last(is_nl));
    JF_ASSERT(trailing_idx == v.len());

    const Kind WS = K("Whitespace"), SC = K(";"), CM = K("Comment"), NL = K("NewlineWs");
    const bool acceptable = vkmatch({NL}) || vkmatch({SC, NL}) || vkmatch({WS, SC, NL}) || vkmatch({CM, NL}) ||
                            vkmatch({WS, CM, NL}) || vkmatch({SC, CM, NL}) || vkmatch({SC, WS, CM, NL}) ||
                            vkmatch({WS, SC, CM, NL}) || vkmatch({WS, SC, WS, CM, NL});
    if (!acceptable) {
        int insert_idx = 1;
        if (vkmatch({WS, SC, WS, CM}))
            insert_idx = 5;
        else if (vkmatch({SC, WS, CM}) || vkmatch({WS, SC, CM}))
            insert_idx = 4;
        else if (vkmatch({WS, SC}) || vkmatch({WS, CM}) || vkmatch({SC, CM}))
            insert_idx = 3;
        else if (vkmatch({SC}) || vkmatch({CM}))
            insert_idx = 2;
        if (kids == v.p) v.p = CopyKids(kids);
        uint32_t wsspn = 0;
        if (v[insert_idx].kind == K("Whitespace")) {
            wsspn = v[insert_idx].span;
            v.p->erase(v.p->begin() + (insert_idx + o - 1));
        }
        std::string nl = "\n" + Spaces(4 * (ctx.indent_level + 1));
        for (int i = 1; i <= insert_idx - 1; ++i) ctx.accept(v[i]);
        ctx.replace_bytes(nl, wsspn);
        Node k = NlNode(static_cast<uint32_t>(nl.size()));
        if (insert_idx < 1 || insert_idx + o - 1 > len(v.p)) throw Refuse("BoundsError: insert!");
        v.p->insert(v.p->begin() + (insert_idx + o - 1), k);
        any = true;
    }
    ctx.seek(pos);
    return any ? ONode(make_node(node, v.p)) : std::nullopt;
}

ONode indent_catch(Ctx &ctx, const Node &node) {
    JF_ASSERT(KIn(node.kind, K("catch"), K("else"), K("finally")));
    const KidsP &kids = verified_kids(node);
    bool any = false;
    Node catch_node = at(kids, 1);
    JF_ASSERT(catch_node.leaf() && KIn(catch_node.kind, K("catch"), K("else"), K("finally")));
    if (!has_tag(catch_node, TAG_INDENT)) {
        at(kids, 1) = add_tag(catch_node, TAG_INDENT);
        any = true;
    }
    if (!has_tag(catch_node, TAG_DEDENT)) {
        at(kids, 1) = add_tag(catch_node, TAG_DEDENT);
        any = true;
    }
    int block_idx = need(2 <= len(kids) ? findnext(IsKind(K("block")), kids, 2) : 0);
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

int FindNext(const KidsP &kids, Kind k, int from) { return from <= len(kids) ? findnext(IsKind(k), kids, from) : 0; }

ONode indent_try(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("try") && !node.leaf());
    const KidsP &kids = node.kids;
    bool any = false;
    Node try_node = at(kids, 1);
    JF_ASSERT(try_node.leaf() && try_node.kind == K("try"));
    if (!has_tag(try_node, TAG_INDENT)) {
        at(kids, 1) = add_tag(try_node, TAG_INDENT);
        any = true;
    }
    int try_block_idx = need(2 <= len(kids) ? findnext(NotWs(), kids, 2) : 0);
    any |= SubBlock(ctx, kids, try_block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    int catch_idx = need(try_block_idx + 1 <= len(kids)
                             ? findnext([](const Node &x) { return KIn(x.kind, K("catch"), K("finally")); }, kids,
                                        try_block_idx + 1)
                             : 0);
    JF_ASSERT(!at(kids, catch_idx).leaf());
    any |= SubBlock(ctx, kids, catch_idx, [&](const Node &n) { return indent_catch(ctx, n); });
    int else_idx = FindNext(kids, K("else"), catch_idx + 1);
    if (else_idx != 0) any |= SubBlock(ctx, kids, else_idx, [&](const Node &n) { return indent_catch(ctx, n); });
    Kind other = at(kids, catch_idx).kind == K("catch") ? K("finally") : K("catch");
    int finally_idx = FindNext(kids, other, (else_idx ? else_idx : catch_idx) + 1);
    if (finally_idx != 0)
        any |= SubBlock(ctx, kids, finally_idx, [&](const Node &n) { return indent_catch(ctx, n); });
    int end_idx = need(FindNext(kids, K("end"), (finally_idx ? finally_idx : else_idx ? else_idx : catch_idx) + 1));
    JF_ASSERT(at(kids, end_idx).leaf());
    if (!has_tag(at(kids, end_idx), TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(at(kids, end_idx), TAG_DEDENT);
        any = true;
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_if(Ctx &ctx, const Node &node) {
    JF_ASSERT(KIn(node.kind, K("if"), K("elseif")) && !node.leaf());
    const KidsP &kids = node.kids;
    bool any = false;
    Node if_node = at(kids, 1);
    JF_ASSERT(if_node.leaf() && KIn(if_node.kind, K("if"), K("elseif")));
    if (!has_tag(if_node, TAG_INDENT)) {
        if_node = add_tag(if_node, TAG_INDENT);
        any = true;
    }
    if (node.kind == K("elseif") && !has_tag(if_node, TAG_DEDENT)) {
        if_node = add_tag(if_node, TAG_DEDENT);
        any = true;
    }
    at(kids, 1) = if_node;
    int cond_idx = need(2 <= len(kids) ? findnext(NotWs(), kids, 2) : 0);
    int block_idx = need(cond_idx + 1 <= len(kids) ? findnext(NotWs(), kids, cond_idx + 1) : 0);
    JF_ASSERT(block_idx == cond_idx + 1);
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    int elseif_idx = FindNext(kids, K("elseif"), block_idx + 1);
    if (elseif_idx != 0) {
        JF_ASSERT(!at(kids, elseif_idx).leaf());
        any |= SubBlock(ctx, kids, elseif_idx, [&](const Node &n) { return indent_if(ctx, n); });
    }
    int else_idx = FindNext(kids, K("else"), (elseif_idx ? elseif_idx : block_idx) + 1);
    if (else_idx != 0) {
        Node else_node = at(kids, else_idx);
        JF_ASSERT(else_node.leaf());
        if (!has_tag(else_node, TAG_INDENT)) {
            else_node = add_tag(else_node, TAG_INDENT);
            any = true;
        }
        if (!has_tag(else_node, TAG_DEDENT)) {
            else_node = add_tag(else_node, TAG_DEDENT);
            any = true;
        }
        at(kids, else_idx) = else_node;
        int else_block_idx = need(else_idx + 1 <= len(kids) ? findnext(NotWs(), kids, else_idx + 1) : 0);
        JF_ASSERT(at(kids, else_block_idx).kind == K("block"));
        any |= SubBlock(ctx, kids, else_block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    }
    int end_idx = FindNext(kids, K("end"), (else_idx ? else_idx : elseif_idx ? elseif_idx : block_idx) + 1);
    JF_ASSERT((node.kind == K("elseif")) == (end_idx == 0));
    if (end_idx != 0) {
        JF_ASSERT(at(kids, end_idx).leaf());
        if (!has_tag(at(kids, end_idx), TAG_DEDENT)) {
            at(kids, end_idx) = add_tag(at(kids, end_idx), TAG_DEDENT);
            any = true;
        }
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_triple_thing(Ctx &ctx, const Node &node) {
    JF_ASSERT(is_triple_thing(node));
    if (is_triple_string(node)) return has_tag(node, TAG_LINE_CONT) ? std::nullopt : ONode(add_tag(node, TAG_LINE_CONT));
    const KidsP &kids = node.kids;
    const int idx = is_triple_string_macro(node) ? 2 : 1;
    ONode k2 = indent_triple_thing(ctx, at(kids, idx));
    if (!k2) return std::nullopt;
    KidsP kk = CopyKids(kids);
    at(kk, idx) = *k2;
    return make_node(node, kk);
}

ONode indent_newlines_between_indices(Ctx &ctx, const Node &node, int open_idx, int close_idx,
                                      bool indent_closing_token) {
    const KidsP &kids = verified_kids(node);
    bool any = false;
    for (int i = open_idx; i <= close_idx; ++i) {
        Node kid = at(kids, i);
        bool changed = false;
        if (!indent_closing_token && i == close_idx - 1 && kid.kind == K("NewlineWs")) continue;
        if (kid.kind == K("NewlineWs") && !has_tag(kid, TAG_LINE_CONT)) {
            kid = add_tag(kid, TAG_LINE_CONT);
            changed = true;
        } else if (is_triple_thing(kid) && (i != open_idx || has_tag(node, TAG_LINE_CONT))) {
            ONode k2 = indent_triple_thing(ctx, kid);
            if (k2) {
                kid = *k2;
                changed = true;
            }
        }
        bool leading = i != open_idx;
        bool trailing = !(i == close_idx - 1 && !indent_closing_token);
        ONode k2 = continue_newlines(kid, leading, trailing);
        if (k2) {
            kid = *k2;
            changed = true;
        }
        if (changed) at(kids, i) = kid;
        any = any || changed;
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_listlike(Ctx &ctx, const Node &node, int open_idx, int close_idx) {
    const KidsP &kids = verified_kids(node);
    KidsP kids2 = kids;
    bool any = false;
    if (open_idx == close_idx) return std::nullopt;
    if (!is_multiline_between_idxs(ctx, node, open_idx, close_idx)) return std::nullopt;
    size_t pos = ctx.position();
    auto cow = [&](int n) {
        if (kids2 == kids) kids2 = SliceKids(kids, n);
    };
    for (int i = 1; i <= open_idx - 1; ++i) ctx.accept(at(kids, i));
    Node kid = at(kids, open_idx);
    JF_ASSERT(kid.leaf());
    JF_ASSERT(kid.kind != K("NewlineWs"));
    if (!has_tag(kid, TAG_INDENT)) {
        kid = add_tag(kid, TAG_INDENT);
        cow(open_idx - 1);
        any = true;
    }
    if (any) kids2->push_back(kid);
    ctx.accept(kid);
    kid = at(kids, open_idx + 1);
    int idx_after_leading_nl = open_idx + 2;
    if (kid.kind == K("NewlineWs")) {
        if (any) kids2->push_back(kid);
        ctx.accept(kid);
    } else if (kmatch(kids, {K("Comment"), K("NewlineWs")}, open_idx + 1)) {
        for (int i = open_idx + 1; i <= open_idx + 2; ++i) {
            ctx.accept(at(kids, i));
            if (any) kids2->push_back(at(kids, i));
        }
        idx_after_leading_nl += 1;
    } else if (kmatch(kids, {K("Whitespace"), K("Comment"), K("NewlineWs")}, open_idx + 1)) {
        for (int i = open_idx + 1; i <= open_idx + 3; ++i) {
            ctx.accept(at(kids, i));
            if (any) kids2->push_back(at(kids, i));
        }
        idx_after_leading_nl += 2;
    } else if (kid.kind == K("Whitespace")) {
        kid = NlNode(kid.span + 1);
        ctx.replace_bytes("\n", 0);
        cow(open_idx - 1);
        any = true;
        kids2->push_back(kid);
        ctx.accept(kid);
    } else if (kid.kind == K("parameters")) {
        const KidsP &grandkids = verified_kids(kid);
        int semi_idx = need(findfirst(IsKind(K(";")), grandkids));
        int next_idx = semi_idx + 1;
        Kind nk = at(grandkids, next_idx).kind;
        if (nk == K("NewlineWs")) {
            if (any) kids2->push_back(kid);
            ctx.accept(kid);
        } else if (nk == K("Whitespace")) {
            size_t p = ctx.position();
            for (int k = 1; k <= next_idx - 1; ++k) ctx.accept(at(grandkids, k));
            ctx.replace_bytes("\n", 0);
            ctx.seek(p);
            Node gk = at(grandkids, next_idx);
            KidsP g2 = CopyKids(grandkids);
            at(g2, next_idx) = NlNode(1 + gk.span);
            kid = make_node(kid, g2);
            cow(open_idx - 1);
            any = true;
            kids2->push_back(kid);
            ctx.accept(kid);
        } else {
            size_t p = ctx.position();
            for (int k = 1; k <= semi_idx; ++k) ctx.accept(at(grandkids, k));
            ctx.replace_bytes("\n", 0);
            ctx.seek(p);
            KidsP g2 = CopyKids(grandkids);
            g2->insert(g2->begin() + (next_idx - 1), NlNode(1));
            kid = make_node(kid, g2);
            cow(open_idx - 1);
            any = true;
            kids2->push_back(kid);
            ctx.accept(kid);
        }
    } else {
        JF_ASSERT(kindof(first_leaf(kid)) != K("Whitespace"));
        Node nlws = NlNode(1);
        ctx.replace_bytes("\n", 0);
        cow(open_idx - 1);
        any = true;
        kids2->push_back(nlws);
        ctx.accept(nlws);
        kids2->push_back(kid);
        ctx.accept(kid);
    }
    for (int i = idx_after_leading_nl; i <= close_idx - 2; ++i) {
        Node k = at(kids, i);
        if (any) kids2->push_back(k);
        ctx.accept(k);
    }
    if (idx_after_leading_nl == close_idx) {
        if (any) {
            kid = kids2->back();
            kids2->pop_back();
        } else {
            kid = at(kids, close_idx - 1);
        }
        ctx.seek(ctx.position() - kid.span);
    } else {
        kid = at(kids, close_idx - 1);
    }
    if (kid.kind == K("NewlineWs") && has_tag(kid, TAG_PRE_DEDENT)) {
        if (any) kids2->push_back(kid);
        ctx.accept(kid);
    } else if (kid.kind == K("NewlineWs")) {
        kid = add_tag(kid, TAG_PRE_DEDENT);
        cow(close_idx - 2);
        any = true;
        kids2->push_back(kid);
        ctx.accept(kid);
    } else {
        JF_ASSERT(kindof(last_leaf(kid)) != K("NewlineWs"));
        if (kid.kind == K("Whitespace")) {
            kid = add_tag(NlNode(kid.span + 1), TAG_PRE_DEDENT);
            ctx.replace_bytes("\n", 0);
            cow(open_idx - 1);
            any = true;
            kids2->push_back(kid);
            ctx.accept(kid);
        } else {
            JF_ASSERT(kindof(last_leaf(kid)) != K("Whitespace"));
            cow(open_idx - 1);
            any = true;
            kids2->push_back(kid);
            ctx.accept(kid);
            Node nlws = add_tag(NlNode(1), TAG_PRE_DEDENT);
            ctx.replace_bytes("\n", 0);
            kids2->push_back(nlws);
            ctx.accept(nlws);
        }
    }
    kid = at(kids, close_idx);
    JF_ASSERT(kid.leaf());
    if (!has_tag(kid, TAG_DEDENT)) {
        kid = add_tag(kid, TAG_DEDENT);
        cow(close_idx - 1);
        any = true;
    }
    if (any) kids2->push_back(kid);
    ctx.accept(kid);
    JF_ASSERT(close_idx == len(kids));
    ctx.seek(pos);
    return any ? ONode(make_node(node, kids2)) : std::nullopt;
}

ONode indent_delimited(Ctx &ctx, const Node &node, Kind open, Kind close) {
    const KidsP &kids = verified_kids(node);
    int o = need(findfirst(IsKind(open), kids));
    int c = need(FindNext(kids, close, o + 1));
    return indent_listlike(ctx, node, o, c);
}

ONode continue_all_newlines(Ctx &ctx, const Node &node, bool is_last, bool is_first) {
    if (node.leaf()) {
        if (node.kind == K("NewlineWs") && !has_tag(node, TAG_LINE_CONT) && !(is_last || is_first))
            return add_tag(node, TAG_LINE_CONT);
        return std::nullopt;
    }
    if (is_triple_thing(node)) {
        if (!is_first) return indent_triple_thing(ctx, node);
        return std::nullopt;
    }
    bool any = false;
    const KidsP &kids = node.kids;
    for (int i = 1; i <= len(kids); ++i) {
        ONode k2 = continue_all_newlines(ctx, at(kids, i), i == len(kids), is_first && i == 1);
        if (k2) {
            at(kids, i) = *k2;
            any = true;
        }
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}
ONode continue_all_newlines(Ctx &ctx, const Node &node) { return continue_all_newlines(ctx, node, node.leaf(), true); }

ONode indent_op_call(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    int first = need(findfirst(NotWs(), kids));
    int last = need(findlast(NotWs(), kids));
    return indent_newlines_between_indices(ctx, node, first, last, true);
}

ONode indent_loop(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    bool any = false;
    int for_idx = need(findfirst([](const Node &x) { return KIn(x.kind, K("for"), K("while")); }, kids));
    if (!has_tag(at(kids, for_idx), TAG_INDENT)) {
        at(kids, for_idx) = add_tag(at(kids, for_idx), TAG_INDENT);
        any = true;
    }
    int block_idx = need(findlast(IsKind(K("block")), kids));
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    int end_idx = need(findlast(IsKind(K("end")), kids));
    if (!has_tag(at(kids, end_idx), TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(at(kids, end_idx), TAG_DEDENT);
        any = true;
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_tuple(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    int o = findfirst(IsKind(K("(")), kids);
    if (o == 0) return continue_all_newlines(ctx, node);
    int c = need(FindNext(kids, K(")"), o + 1));
    JF_ASSERT(o == 1);
    JF_ASSERT(c == len(kids));
    return indent_listlike(ctx, node, o, c);
}

ONode indent_struct(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    bool any = false;
    int struct_idx = need(findfirst(NotWs(), kids));
    JF_ASSERT(KIn(at(kids, struct_idx).kind, K("mutable"), K("struct")));
    if (!has_tag(at(kids, struct_idx), TAG_INDENT)) {
        at(kids, struct_idx) = add_tag(at(kids, struct_idx), TAG_INDENT);
        any = true;
    }
    int block_idx = need(FindNext(kids, K("block"), struct_idx + 1));
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    int end_idx = need(findlast(IsKind(K("end")), kids));
    if (!has_tag(at(kids, end_idx), TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(at(kids, end_idx), TAG_DEDENT);
        any = true;
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_assignment(Ctx &ctx, const Node &node) {
    JF_ASSERT(!node.leaf());
    JF_ASSERT(is_variable_assignment(ctx, node));
    const KidsP &kids = node.kids;
    int lhsidx = need(findfirst(NotWs(), kids));
    int eqidx = need(lhsidx + 1 <= len(kids) ? findnext(NotWs(), kids, lhsidx + 1) : 0);
    JF_ASSERT(node.kind == at(kids, eqidx).kind);
    JF_ASSERT(len(kids) > eqidx);
    int rhsidx = need(findnext(NotWs(), kids, eqidx + 1));
    JF_ASSERT(rhsidx == len(kids));
    if (rhsidx - 1 < eqidx + 1) return std::nullopt;
    Node rhs = at(kids, rhsidx);
    if (KIn(rhs.kind, K("if"), K("try"), K("function"), K("let")) || is_triple_thing(rhs)) return std::nullopt;
    KidsP kids2 = kids;
    bool changed = false;
    for (int i = eqidx + 1; i <= rhsidx - 1; ++i) {
        Node kid = at(kids, i);
        Node kid2 = kid;
        if (kid.kind == K("NewlineWs") && !has_tag(kid, TAG_LINE_CONT)) {
            kid2 = add_tag(kid, TAG_LINE_CONT);
            changed = true;
        }
        if (changed) {
            if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
            kids2->push_back(kid2);
        }
    }
    if (!has_tag(rhs, TAG_LINE_CONT)) {
        rhs = add_tag(rhs, TAG_LINE_CONT);
        changed = true;
        if (kids2 == kids) kids2 = SliceKids(kids, rhsidx - 1);
        kids2->push_back(rhs);
    } else if (changed) {
        kids2->push_back(rhs);
    }
    if (changed) return make_node(node, kids2);
    return std::nullopt;
}

ONode indent_do(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    bool any = false;
    int do_idx = need(findfirst(IsKind(K("do")), kids));
    if (!has_tag(at(kids, do_idx), TAG_INDENT)) {
        at(kids, do_idx) = add_tag(at(kids, do_idx), TAG_INDENT);
        any = true;
    }
    int block_idx = need(FindNext(kids, K("block"), do_idx + 1));
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n); });
    int end_idx = need(FindNext(kids, K("end"), block_idx + 1));
    if (!has_tag(at(kids, end_idx), TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(at(kids, end_idx), TAG_DEDENT);
        any = true;
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_quote(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    if (has_flags(node, jlparse::COLON_QUOTE)) return std::nullopt;
    int block_idx = findfirst(IsKind(K("block")), kids);
    if (block_idx == 0) return std::nullopt;
    JF_ASSERT(block_idx == 1);
    ONode b = indent_begin(ctx, at(kids, block_idx), K("quote"));
    if (!b) return std::nullopt;
    at(kids, block_idx) = *b;
    return make_node(node, kids);
}

ONode indent_module(Ctx &ctx, const Node &node, bool do_indent);

ONode indent_doc_module(Ctx &ctx, const Node &node, bool do_indent) {
    const KidsP &kids = verified_kids(node);
    int mod_idx = need(findfirst(IsKind(K("module")), kids));
    size_t pos = ctx.position();
    for (int i = 1; i <= mod_idx - 1; ++i) ctx.accept(at(kids, i));
    ONode m = indent_module(ctx, at(kids, mod_idx), do_indent);
    ctx.seek(pos);
    if (!m) return std::nullopt;
    KidsP k2 = CopyKids(kids);
    at(k2, mod_idx) = *m;
    return make_node(node, k2);
}

ONode indent_module(Ctx &ctx, const Node &node, bool do_indent) {
    JF_ASSERT(KIn(node.kind, K("module"), K("doc")));
    if (node.kind == K("doc")) return indent_doc_module(ctx, node, do_indent);
    const KidsP &kids = verified_kids(node);
    bool any = false;
    size_t pos = ctx.position();
    Node mod_node = at(kids, 1);
    JF_ASSERT(mod_node.leaf() && KIn(mod_node.kind, K("module"), K("baremodule")));
    if (do_indent && !has_tag(mod_node, TAG_INDENT)) {
        at(kids, 1) = add_tag(mod_node, TAG_INDENT);
        any = true;
    }
    int modname_idx = need(2 <= len(kids) ? findnext(NotWs(), kids, 2) : 0);
    int block_idx = need(FindNext(kids, K("block"), modname_idx + 1));
    JF_ASSERT(block_idx == modname_idx + 1);
    any |= SubBlock(ctx, kids, block_idx, [&](const Node &n) { return indent_block(ctx, n, true, do_indent); });
    int end_idx = FindNext(kids, K("end"), block_idx + 1);
    Node end_node = at(kids, end_idx);
    JF_ASSERT(end_node.leaf() && end_node.kind == K("end"));
    if (do_indent && !has_tag(end_node, TAG_DEDENT)) {
        at(kids, end_idx) = add_tag(end_node, TAG_DEDENT);
        any = true;
    }
    ctx.seek(pos);
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_toplevel(Ctx &ctx, const Node &node) {
    const KidsP &kids = verified_kids(node);
    int mod_idx = findfirst(
        [](const Node &x) {
            return x.kind == K("module") ||
                   (x.kind == K("doc") && findfirst(IsKind(K("module")), verified_kids(x)) != 0);
        },
        kids);
    if (mod_idx == 0) return std::nullopt;
    int nonws = 0;
    for (const Node &x : *kids)
        if (!is_ws(x)) ++nonws;
    const bool do_indent = nonws > 1;
    bool any = false;
    while (mod_idx != 0) {
        any |= SubBlock(ctx, kids, mod_idx, [&](const Node &n) { return indent_module(ctx, n, do_indent); });
        mod_idx = FindNext(kids, K("module"), mod_idx + 1);
    }
    return any ? ONode(make_node(node, kids)) : std::nullopt;
}

ONode indent_local_global(Ctx &, const Node &node) {
    JF_ASSERT(!is_global_local_list(node));
    const KidsP &kids = verified_kids(node);
    int kw = need(findfirst([](const Node &x) { return x.leaf() && KIn(x.kind, K("local"), K("global")); }, kids));
    int nonws = need(kw + 1 <= len(kids) ? findnext(NotWs(), kids, kw + 1) : 0);
    JF_ASSERT(kindof(first_leaf(at(kids, nonws))) != K("NewlineWs"));
    bool changed = false;
    for (int i = kw + 1; i <= nonws - 1; ++i) {
        if (at(kids, i).kind == K("NewlineWs") && !has_tag(at(kids, i), TAG_LINE_CONT)) {
            at(kids, i) = add_tag(at(kids, i), TAG_LINE_CONT);
            changed = true;
        }
    }
    return changed ? ONode(make_node(node, kids)) : std::nullopt;
}

}  // namespace

ONode insert_delete_mark_newlines(Ctx &ctx, const Node &node) {
    if (node.leaf()) return std::nullopt;
    const Kind k = node.kind;
    if (KIn(k, K("function"), K("macro"))) return indent_function_or_macro(ctx, node);
    if (k == K("if")) return indent_if(ctx, node);
    if (k == K("let")) return indent_let(ctx, node);
    if (is_begin_block(node)) return indent_begin(ctx, node);
    if (KIn(k, K("call"), K("dotcall")) && node.flags == 0) return indent_delimited(ctx, node, K("("), K(")"));
    if (k == K("macrocall") && has_flags(node, jlparse::PARENS_FLAG))
        return indent_delimited(ctx, node, K("("), K(")"));
    if (is_infix_op_call(node)) return indent_op_call(ctx, node);
    if (KIn(k, K("for"), K("while"))) return indent_loop(ctx, node);
    if (k == K("tuple")) return indent_tuple(ctx, node);
    if (k == K("struct")) return indent_struct(ctx, node);
    if (k == K("parens")) return indent_delimited(ctx, node, K("("), K(")"));
    if (KIn(k, K("curly"), K("braces"), K("bracescat"))) return indent_delimited(ctx, node, K("{"), K("}"));
    if (KIn(k, K("||"), K("&&"))) return indent_op_call(ctx, node);
    if (KIn(k, K("using"), K("import"), K("export"), K("public")) || is_global_local_list(node))
        return continue_all_newlines(ctx, node);
    if (KIn(k, K("local"), K("global"))) return indent_local_global(ctx, node);
    if (is_variable_assignment(ctx, node)) return indent_assignment(ctx, node);
    if (k == K("parameters")) return std::nullopt;
    if (k == K("?")) return continue_all_newlines(ctx, node);
    if (KIn(k, K("cartesian_iterator"), K("generator"))) return continue_all_newlines(ctx, node);
    if (k == K("try")) return indent_try(ctx, node);
    if (k == K("quote")) return indent_quote(ctx, node);
    if (k == K("do")) return indent_do(ctx, node);
    if (is_paren_block(node)) return indent_delimited(ctx, node, K("("), K(")"));
    if (KIn(k, K("vect"), K("vcat"), K("typed_vcat"), K("ncat"), K("ref"), K("comprehension"),
            K("typed_comprehension")))
        return indent_delimited(ctx, node, K("["), K("]"));
    if (k == K("row")) return std::nullopt;
    if (k == K("comparison")) return continue_all_newlines(ctx, node);
    if (k == K("toplevel")) return indent_toplevel(ctx, node);
    if (k == K("module")) {
        bool do_indent = false;
        for (Kind l : ctx.lineage_kinds)
            if (l == K("module")) do_indent = true;
        return indent_module(ctx, node, do_indent);
    }
    return std::nullopt;
}

ONode indent_multiline_strings(Ctx &ctx, const Node &node) {
    if (!is_triple_string(node)) return std::nullopt;
    const bool str = node.kind == K("string");
    const Kind triplekind = str ? K("\"\"\"") : K("```");
    const Kind itemkind = str ? K("String") : K("CmdString");
    int indent_span = 4 * ctx.indent_level;
    if (has_tag(node, TAG_LINE_CONT)) indent_span += 4;
    const bool indented = indent_span > 0;
    size_t pos = ctx.position();
    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any = false;
    if (!indented && findfirst(IsKind(K("Whitespace")), kids) == 0) return std::nullopt;
    int open_idx = need(findfirst(IsKind(triplekind), kids));
    int close_idx = need(findlast(IsKind(triplekind), kids));
    JF_ASSERT(close_idx == len(kids));
    ctx.accept(at(kids, open_idx));
    auto cow = [&](int n) {
        if (kids2 == kids) kids2 = SliceKids(kids, n);
    };
    auto all_blank = [](std::string_view b) {
        for (char c : b)
            if (c != ' ' && c != '\t') return false;
        return true;
    };
    int idx = open_idx + 1;
    bool expect_indent_ws = false;
    while (idx < close_idx) {
        Node kid = at(kids, idx);
        if (!expect_indent_ws) {
            if (kid.kind == itemkind) {
                if (indented && kid.span > 0 && ctx.read_bytes(kid).back() == '\n') expect_indent_ws = true;
                ctx.accept(kid);
                if (any) kids2->push_back(kid);
            } else if (kid.kind == K("Whitespace")) {
                std::string bytes = ctx.read_bytes(kid);
                const size_t want = 2 + static_cast<size_t>(std::max(indent_span, 0));
                if (bytes.size() == want && bytes[0] == '\\' && bytes[1] == '\n') {
                    JF_ASSERT(all_blank(std::string_view(bytes).substr(2)));
                    ctx.accept(kid);
                    if (any) kids2->push_back(kid);
                } else if (bytes.size() >= 2 && bytes[0] == '\\' && bytes[1] == '\n') {
                    JF_ASSERT(all_blank(std::string_view(bytes).substr(2)));
                    if (bytes.size() < want)
                        bytes.resize(want, ' ');
                    else
                        bytes.resize(want);
                    ctx.replace_bytes(bytes, kid.span);
                    cow(idx - 1);
                    Node k2 = Leaf(K("Whitespace"), jlparse::TRIVIA_FLAG, static_cast<uint32_t>(bytes.size()));
                    ctx.accept(k2);
                    kids2->push_back(k2);
                    any = true;
                } else {
                    JF_ASSERT(all_blank(bytes));
                    ctx.replace_bytes("", kid.span);
                    cow(idx - 1);
                    any = true;
                }
            } else {
                ctx.accept(kid);
                if (any) kids2->push_back(kid);
            }
        } else {
            expect_indent_ws = false;
            if (kid.kind == itemkind && kid.span == 1 && ctx.peek() == '\n') {
                continue;
            }
            bool cond = false;
            if (kid.kind == K("Whitespace") && idx + 1 < close_idx && at(kids, idx + 1).kind == itemkind &&
                at(kids, idx + 1).span == 1) {
                size_t peekpos = ctx.position();
                ctx.accept(kid);
                ctx.accept(at(kids, idx + 1));
                ctx.seek(ctx.position() - 1);
                cond = ctx.peek() == '\n';
                ctx.seek(peekpos);
            }
            if (cond) continue;
            if (kid.kind == K("Whitespace") && static_cast<int>(kid.span) == indent_span) {
                for (char c : ctx.read_bytes(kid)) JF_ASSERT(c == ' ');
                ctx.accept(kid);
                if (any) kids2->push_back(kid);
            } else if (kid.kind == K("Whitespace")) {
                ctx.replace_bytes(Spaces(indent_span), kid.span);
                cow(idx - 1);
                Node k2 = Node{kid.kind, kid.flags, static_cast<uint32_t>(indent_span), nullptr, kid.tags};
                any = true;
                kids2->push_back(k2);
                ctx.accept(k2);
            } else {
                ctx.replace_bytes(Spaces(indent_span), 0);
                cow(idx - 1);
                Node k2 = Leaf(K("Whitespace"), jlparse::TRIVIA_FLAG, static_cast<uint32_t>(indent_span));
                any = true;
                kids2->push_back(k2);
                ctx.accept(k2);
                continue;
            }
        }
        idx += 1;
    }
    if (expect_indent_ws) {
        ctx.replace_bytes(Spaces(indent_span), 0);
        cow(idx - 1);
        Node k2 = Leaf(K("Whitespace"), jlparse::TRIVIA_FLAG, static_cast<uint32_t>(indent_span));
        any = true;
        kids2->push_back(k2);
        ctx.accept(k2);
    }
    JF_ASSERT(idx == close_idx);
    Node close_kid = at(kids, close_idx);
    JF_ASSERT(close_kid.kind == triplekind);
    ctx.accept(close_kid);
    if (any) kids2->push_back(close_kid);
    ctx.seek(pos);
    return any ? ONode(make_node(node, kids2)) : std::nullopt;
}

}  // namespace juliafmt::detail
