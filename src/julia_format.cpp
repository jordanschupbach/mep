// mep's own Julia formatter (julia_format.h): a port of Runic.jl 1.5.0 (MIT,
// Copyright (c) 2024 Fredrik Ekre and contributors). This file holds Runic.jl
// (Node, Context, the format_node! driver, the format toggles) and chisels.jl
// (tree utilities and normalize_tree!); see julia_format_internal.h for the
// map of the port.
//
// Self-check: Runic re-parses its output and refuses if that fails; on top of
// that Format() compares the token streams of input and output (the
// non-whitespace, non-comment leaves, comments with trailing blanks
// stripped) modulo the token edits Runic makes on purpose -- commas,
// semicolons, parentheses and braces it adds or drops, `return` it inserts,
// `=`/`∈` -> `in` in loops, and number spellings (0x0f, 1.0). Any other
// difference means the port changed the program, and Format() returns
// ok=false with the input untouched.

#include "julia_format.h"

#include <algorithm>
#include <cstring>

#include "julia_format_internal.h"

namespace juliafmt {
namespace detail {

[[noreturn]] void RefuseAt(const char *what, int line) {
    throw Refuse(std::string("assertion failed: ") + what + " (julia_format line " + std::to_string(line) + ")");
}

//------------------------------------------------------------------------------
// Node

Node Inner(Kind k, RawFlags f, KidsP kids) {
    uint32_t s = 0;
    for (const Node &c : *kids) s += c.span;
    return Node{k, f, s, std::move(kids), 0};
}

Node make_node(const Node &node, KidsP kids) {
    uint32_t s = 0;
    for (const Node &c : *kids) s += c.span;
    return Node{node.kind, node.flags, s, std::move(kids), node.tags};
}

Node make_leaf(const Node &node, uint32_t span) {
    JF_ASSERT(node.leaf());
    return Node{node.kind, node.flags, span, nullptr, node.tags};
}

KidsP SliceKids(const KidsP &k, int n) {
    if (n < 0 || n > len(k)) throw Refuse("BoundsError: kids[1:n]");
    return std::make_shared<KidVec>(k->begin(), k->begin() + n);
}

Node &at(const KidsP &k, int i) {
    if (i < 1 || i > len(k)) throw Refuse("BoundsError: kids[i]");
    return (*k)[static_cast<size_t>(i - 1)];
}

const KidsP &verified_kids(const Node &n) {
    JF_ASSERT(!n.leaf());
    return n.kids;
}

bool kmatch(const KidsP &kids, std::initializer_list<Kind> kinds, int i) {
    const int n = static_cast<int>(kinds.size());
    if (i < 1 || i + n - 1 > len(kids)) return false;
    int j = 0;
    for (Kind k : kinds) {
        if ((*kids)[static_cast<size_t>(i + j - 1)].kind != k) return false;
        ++j;
    }
    return true;
}

//------------------------------------------------------------------------------
// chisels.jl

namespace {
ONode nth_leaf(const Node &node, int nth, int &n_seen) {
    if (node.leaf()) {
        ++n_seen;
        return node;
    }
    for (const Node &kid : *node.kids) {
        ONode leaf = nth_leaf(kid, nth, n_seen);
        if (n_seen == nth) return leaf;
    }
    return std::nullopt;
}

ONode second_last_leaf_impl(const Node &node, int &n_seen) {
    if (node.leaf()) {
        ++n_seen;
        return node;
    }
    for (size_t i = node.kids->size(); i-- > 0;) {
        ONode kid = second_last_leaf_impl((*node.kids)[i], n_seen);
        if (n_seen == 2) return kid;
    }
    return std::nullopt;
}
}  // namespace

ONode first_leaf(const Node &n) {
    int seen = 0;
    ONode leaf = nth_leaf(n, 1, seen);
    return seen == 1 ? leaf : std::nullopt;
}

ONode last_leaf(const Node &n) {
    if (n.leaf()) return n;
    if (n.kids->empty()) return std::nullopt;
    return last_leaf(n.kids->back());
}

ONode second_last_leaf(const Node &n) {
    int seen = 0;
    ONode leaf = second_last_leaf_impl(n, seen);
    return seen == 2 ? leaf : std::nullopt;
}

int meta_nargs(const Node &n) {
    if (n.leaf()) return 0;
    int c = 0;
    for (const Node &k : *n.kids)
        if (!is_ws(k)) ++c;
    return c;
}

bool is_assignment(const Node &n) { return jlparse::is_prec_assignment(n.kind); }

bool is_variable_assignment(const Ctx &ctx, const Node &n) {
    if (n.leaf() || !is_assignment(n)) return false;
    Kind e = ctx.lineage_end();
    return !KIn(e, K("for"), K("generator"), K("cartesian_iterator"), K("filter"));
}

bool is_global_local_list(const Node &node) {
    if (!(KIn(node.kind, K("global"), K("local")) && !node.leaf())) return false;
    const KidsP &kids = node.kids;
    for (const Node &x : *kids)
        if (is_assignment(x)) return false;
    for (const Node &x : *kids)
        if (x.kind == K(",")) return true;
    int idx = need(findfirst([](const Node &x) { return KIn(x.kind, K("global"), K("local")); }, kids));
    idx = need(findnext(NotWs(), kids, idx + 1));
    return KIn(at(kids, idx).kind, K("Identifier"), K("var"));
}

bool is_infix_op_call(const Node &n) {
    return KIn(n.kind, K("call"), K("dotcall")) && jlparse::is_infix_op_call(n.flags);
}

Node infix_op_call_op(const Node &node) {
    JF_ASSERT(is_infix_op_call(node) || node.kind == K("||"));
    const KidsP &kids = verified_kids(node);
    int first = need(findfirst(NotWs(), kids));
    int op = need(findnext([](const Node &x) { return jlparse::is_operator(x.kind); }, kids, first + 1));
    return at(kids, op);
}

bool is_comparison_leaf(const Node &n) {
    if (n.leaf() && jlparse::is_prec_comparison(n.kind)) return true;
    if (!n.leaf() && n.kind == K(".") && meta_nargs(n) == 2 && is_comparison_leaf(at(n.kids, 2))) return true;
    return false;
}

bool is_operator_leaf(const Node &n) { return n.leaf() && jlparse::is_operator(n.kind); }

Node first_non_whitespace_kid(const Node &n) {
    const KidsP &kids = verified_kids(n);
    return at(kids, need(findfirst(NotWs(), kids)));
}

bool is_begin_block(const Node &n, Kind token) {
    if (n.kind != K("block")) return false;
    const KidsP &kids = verified_kids(n);
    return !kids->empty() && kids->front().kind == token;
}

bool is_paren_block(const Node &n) { return n.kind == K("block") && has_flags(n, jlparse::PARENS_FLAG); }

bool is_string_macro(const Node &node) {
    if (node.kind != K("macrocall")) return false;
    const KidsP &kids = verified_kids(node);
    return len(kids) >= 2 &&
           KIn(at(kids, 1).kind, K("StringMacroName"), K("CmdMacroName"), K("core_@cmd")) &&
           KIn(at(kids, 2).kind, K("string"), K("cmdstring"));
}

bool is_triple_string(const Node &n) {
    return KIn(n.kind, K("string"), K("cmdstring")) && has_flags(n, jlparse::TRIPLE_STRING_FLAG);
}

bool is_triple_string_macro(const Node &node) {
    if (node.kind == K("macrocall")) {
        const KidsP &kids = verified_kids(node);
        if (len(kids) >= 2 && KIn(at(kids, 1).kind, K("StringMacroName"), K("CmdMacroName"), K("core_@cmd")) &&
            is_triple_string(at(kids, 2)))
            return true;
    }
    return false;
}

bool is_triple_thing(const Node &n) {
    return is_triple_string(n) || is_triple_string_macro(n) ||
           (n.kind == K("juxtapose") && is_triple_string_macro(at(verified_kids(n), 1)));
}

ONode replace_first_leaf(const Node &node, const ONode &kid) {
    if (node.leaf()) return kid;
    KidsP kids2 = CopyKids(node.kids);
    ONode k2 = replace_first_leaf(at(kids2, 1), kid);
    if (!k2)
        kids2->erase(kids2->begin());
    else
        at(kids2, 1) = *k2;
    JF_ASSERT(len(kids2) > 0);
    return make_node(node, kids2);
}

ONode replace_last_leaf(const Node &node, const ONode &kid) {
    if (node.leaf()) return kid;
    KidsP kids2 = CopyKids(node.kids);
    ONode k2 = replace_last_leaf(at(kids2, len(kids2)), kid);
    if (!k2)
        kids2->pop_back();
    else
        at(kids2, len(kids2)) = *k2;
    JF_ASSERT(len(kids2) > 0);
    return make_node(node, kids2);
}

Node add_before_first_leaf(const Node &node, const Node &kid) {
    JF_ASSERT(!node.leaf());
    const KidsP &kids = node.kids;
    JF_ASSERT(len(kids) > 0);
    KidsP kids2 = CopyKids(kids);
    if (at(kids2, 1).kind == K("Comment"))
        kids2->insert(kids2->begin(), kid);
    else
        at(kids2, 1) = add_before_first_leaf(at(kids2, 1), kid);
    return make_node(node, kids2);
}

// Note: Runic's version never reports a change for an inner node (its
// any_kid_changed is only ever set to false) but mutates the kid vectors in
// place, which the callers see through the shared vectors.
ONode continue_newlines(const Node &node, bool leading, bool trailing) {
    if (node.leaf()) {
        if (node.kind == K("NewlineWs") && !has_tag(node, TAG_LINE_CONT)) return add_tag(node, TAG_LINE_CONT);
        return std::nullopt;
    }
    const KidsP &kids = node.kids;
    if (len(kids) == 1) return std::nullopt;
    auto skip = [](const Node &x) { return !KIn(x.kind, K("Whitespace"), K("Comment")); };
    if (leading) {
        int idx = 0;
        while (true) {
            idx = idx + 1 <= len(kids) ? findnext(skip, kids, idx + 1) : 0;
            if (idx == 0) break;
            Node k = at(kids, idx);
            ONode k2 = continue_newlines(k, leading, trailing);
            if (k2) at(kids, idx) = *k2;
            if (k.kind != K("NewlineWs")) break;
        }
    }
    if (trailing) {
        int idx = len(kids) + 1;
        while (true) {
            idx = idx - 1 >= 1 ? findprev(skip, kids, idx - 1) : 0;
            if (idx == 0) break;
            Node k = at(kids, idx);
            ONode k2 = continue_newlines(k, leading, trailing);
            if (k2) at(kids, idx) = *k2;
            if (k.kind != K("NewlineWs")) break;
        }
    }
    return std::nullopt;
}

bool is_multiline_between_idxs(Ctx &ctx, const Node &node, int opening_idx, int closing_idx) {
    JF_ASSERT(!node.leaf());
    const KidsP &kids = node.kids;
    for (int y = opening_idx + 1; y <= closing_idx - 1; ++y)
        if (any_leaf([](const Node &x) { return x.kind == K("NewlineWs"); }, at(kids, y))) return true;
    size_t pos = ctx.position();
    for (int i = 1; i <= opening_idx; ++i) ctx.accept(at(kids, i));
    for (int i = opening_idx + 1; i <= closing_idx - 1; ++i) {
        Node kid = at(kids, i);
        size_t ipos = ctx.position();
        if (contains_multiline_triple_string(ctx, kid)) {
            ctx.seek(pos);
            return true;
        }
        ctx.seek(ipos);
        ctx.accept(kid);
    }
    ctx.seek(pos);
    return false;
}

bool contains_multiline_triple_string(Ctx &ctx, const Node &node) {
    if (node.leaf()) {
        ctx.accept(node);
        return false;
    }
    const KidsP &kids = node.kids;
    size_t pos = ctx.position();
    if (KIn(node.kind, K("string"), K("cmdstring")) && has_flags(node, jlparse::TRIPLE_STRING_FLAG)) {
        const bool str = node.kind == K("string");
        Kind triplekind = str ? K("\"\"\"") : K("```");
        const char *triplestring = str ? "\"\"\"" : "```";
        Kind itemkind = str ? K("String") : K("CmdString");
        for (int i = 1; i <= len(kids); ++i) {
            Node kid = at(kids, i);
            if (i == 1 || i == len(kids)) {
                JF_ASSERT(kid.kind == triplekind);
                JF_ASSERT(ctx.read_bytes(kid) == triplestring);
            }
            if (kid.kind == itemkind) {
                std::string b = ctx.read_bytes(kid);
                if (!b.empty() && b.back() == '\n') return true;
            }
            ctx.accept(kid);
        }
        JF_ASSERT(ctx.position() == pos + node.span);
    } else {
        for (size_t i = 0; i < kids->size(); ++i) {
            Node kid = (*kids)[i];
            size_t kpos = ctx.position();
            if (contains_multiline_triple_string(ctx, kid)) return true;
            ctx.seek(kpos);
            ctx.accept(kid);
        }
    }
    return false;
}

namespace {
ONode first_leaf_predicate(const Node &node, bool (*pred)(const Node &)) {
    if (node.leaf()) return pred(node) ? ONode(node) : std::nullopt;
    for (const Node &k : *node.kids) {
        ONode r = first_leaf_predicate(k, pred);
        if (r) return r;
    }
    return std::nullopt;
}
}  // namespace

std::string macrocall_name(Ctx &ctx, const Node &node) {
    JF_ASSERT(node.kind == K("macrocall"));
    const KidsP &kids = verified_kids(node);
    ONode m = first_leaf_predicate(node, [](const Node &x) {
        return KIn(x.kind, K("MacroName"), K("StringMacroName"), K("CmdMacroName"), K("core_@cmd"));
    });
    if (!m) throw Refuse("TypeError: macro name ::Node");
    Kind mkind = m->kind;
    if (kmatch(kids, {K("@"), K("MacroName")})) {
        size_t p = ctx.position();
        size_t n = at(kids, 1).span + at(kids, 2).span;
        std::string b = ctx.io.buf.substr(p, n);
        return b;
    }
    if (kmatch(kids, {K(".")}) || kmatch(kids, {K("CmdMacroName")}) || kmatch(kids, {K("StringMacroName")})) {
        std::string b = ctx.read_bytes(at(kids, 1));
        if (mkind == K("CmdMacroName"))
            b += "_cmd";
        else if (mkind == K("StringMacroName"))
            b += "_str";
        return b;
    }
    if (kmatch(kids, {K("core_@cmd")})) {
        std::string b = ctx.read_bytes(at(kids, 1));
        JF_ASSERT(b.empty());
        return "core_@cmd";
    }
    return "<unknown macro>";
}

bool safe_to_insert_return(const Ctx &ctx) {
    static const char *const kSafe[] = {"@inline", "Base.@inline", "@noinline", "Base.@noinline",
                                        "@propagate_inbounds", "Base.@propagate_inbounds", "@generated",
                                        "Base.@generated", "@eval", "Base.@eval", "@assume_effects",
                                        "Base.@assume_effects", "@doc", "Base.@doc", "Core.@doc"};
    for (const std::string &m : ctx.lineage_macros) {
        bool ok = false;
        for (const char *s : kSafe)
            if (m == s) ok = true;
        if (!ok) return false;
    }
    return true;
}

//------------------------------------------------------------------------------
// IOBuffer

std::string Ctx::read_bytes(const Node &n) const {
    std::string b = io.pos <= io.buf.size() ? io.buf.substr(io.pos, n.span) : std::string();
    JF_ASSERT(b.size() == n.span);
    return b;
}

uint32_t Ctx::replace_bytes(std::string_view bytes, size_t size) {
    size_t pos = io.pos;
    size_t end = std::min(pos + size, io.buf.size());
    io.buf.replace(pos, end - pos, bytes);
    io.pos = pos;
    return static_cast<uint32_t>(bytes.size());
}

uint8_t Ctx::peek() const {
    if (io.pos >= io.buf.size()) throw Refuse("EOFError");
    return static_cast<uint8_t>(io.buf[io.pos]);
}

//------------------------------------------------------------------------------
// normalize_tree! (chisels.jl)

namespace {

// pop_if_whitespace!(popfirst! / pop!, node): (node′, ws) or nullopt.
bool pop_if_whitespace(bool first, const Node &node, Node &node2, Node &ws) {
    if (node.leaf()) return false;
    const KidsP &kids = node.kids;
    if (kids->empty() || !is_ws(first ? kids->front() : kids->back())) return false;
    if (first) {
        ws = kids->front();
        kids->erase(kids->begin());
    } else {
        ws = kids->back();
        kids->pop_back();
    }
    node2 = make_node(node, kids);
    return true;
}

ONode insert_into_last_else_block(const Node &node, const Node &ws) {
    JF_ASSERT(node.kind == K("elseif"));
    const KidsP &kids = verified_kids(node);
    int elseifidx = findfirst([](const Node &x) { return !x.leaf() && x.kind == K("elseif"); }, kids);
    if (elseifidx != 0) {
        JF_ASSERT(elseifidx == len(kids));
        ONode e = insert_into_last_else_block(at(kids, elseifidx), ws);
        JF_ASSERT(e.has_value());
        at(kids, elseifidx) = *e;
        return make_node(node, kids);
    }
    int elseifblockidx = need(findfirst(IsKind(K("block")), kids));
    int elseleafidx = need(findnext(IsKind(K("else")), kids, elseifblockidx + 1));
    int elseblockidx = need(findnext(IsKind(K("block")), kids, elseleafidx + 1));
    JF_ASSERT(elseblockidx == len(kids));
    Node elseblock = at(kids, elseblockidx);
    const KidsP &ek = verified_kids(elseblock);
    JF_ASSERT(!KIn(at(ek, len(ek)).kind, K("NewlineWs"), K("Whitespace")));
    ek->push_back(ws);
    at(kids, elseblockidx) = make_node(elseblock, ek);
    return make_node(node, kids);
}

ONode insert_into_last_catchlike_block(const Node &node, const Node &ws) {
    JF_ASSERT(KIn(node.kind, K("catch"), K("finally"), K("else")));
    const KidsP &kids = verified_kids(node);
    int idx = need(findfirst(IsKind(K("block")), kids));
    JF_ASSERT(idx == len(kids));
    Node block = at(kids, idx);
    const KidsP &bk = verified_kids(block);
    JF_ASSERT(!KIn(at(bk, len(bk)).kind, K("NewlineWs"), K("Whitespace")));
    bk->push_back(ws);
    at(kids, idx) = make_node(block, bk);
    return make_node(node, kids);
}

void normalize_tree(const Node &node) {
    if (node.leaf()) return;
    const KidsP &kids = node.kids;

    if (node.kind == K("let")) {
        int varsidx = need(findfirst(IsKind(K("block")), kids));
        int blockidx = need(findnext(IsKind(K("block")), kids, varsidx + 1));
        while (blockidx != varsidx + 1) {
            Node grandkid = at(kids, blockidx - 1);
            kids->erase(kids->begin() + (blockidx - 2));
            blockidx -= 1;
            Node block = at(kids, blockidx);
            const KidsP &bk = verified_kids(block);
            bk->insert(bk->begin(), grandkid);
            at(kids, blockidx) = make_node(block, bk);
        }
    }

    if (KIn(node.kind, K("function"), K("if"), K("elseif"), K("for"), K("while"), K("try"), K("do"), K("macro"),
            K("module"), K("baremodule"), K("let"), K("struct"))) {
        int blockidx = 0;
        while (true) {
            blockidx = blockidx + 1 <= len(kids) ? findnext(IsKind(K("block")), kids, blockidx + 1) : 0;
            if (!(blockidx != 0 && blockidx < len(kids))) break;
            if (at(kids, blockidx + 1).kind != K("Whitespace")) continue;
            Node block = at(kids, blockidx);
            const KidsP &bk = verified_kids(block);
            JF_ASSERT(!KIn(at(bk, len(bk)).kind, K("Whitespace"), K("NewlineWs")));
            Node ws = at(kids, blockidx + 1);
            kids->erase(kids->begin() + blockidx);
            bk->push_back(ws);
            at(kids, blockidx) = make_node(block, bk);
        }
    }

    if (node.kind == K("if")) {
        int elseifidx = findfirst(IsKind(K("elseif")), kids);
        if (elseifidx != 0) {
            int endidx = need(findnext(IsKind(K("end")), kids, elseifidx + 1));
            if (elseifidx + 2 == endidx && at(kids, elseifidx + 1).kind == K("Whitespace")) {
                Node ws = at(kids, elseifidx + 1);
                kids->erase(kids->begin() + elseifidx);
                ONode e = insert_into_last_else_block(at(kids, elseifidx), ws);
                JF_ASSERT(e.has_value());
                at(kids, elseifidx) = *e;
            }
        }
    }

    if (node.kind == K("try")) {
        int catchidx = 0;
        while (true) {
            catchidx = catchidx + 1 <= len(kids)
                           ? findnext([](const Node &x) { return KIn(x.kind, K("catch"), K("finally"), K("else")); },
                                      kids, catchidx + 1)
                           : 0;
            if (catchidx == 0) break;
            JF_ASSERT(catchidx + 1 <= len(kids));
            if (at(kids, catchidx + 1).kind == K("Whitespace")) {
                Node ws = at(kids, catchidx + 1);
                kids->erase(kids->begin() + catchidx);
                ONode c = insert_into_last_catchlike_block(at(kids, catchidx), ws);
                JF_ASSERT(c.has_value());
                at(kids, catchidx) = *c;
            }
        }
    }

    if (node.kind == K("do")) {
        int tupleidx = need(findfirst(IsKind(K("tuple")), kids));
        int blockidx = need(findnext(IsKind(K("block")), kids, tupleidx + 1));
        JF_ASSERT(tupleidx + 1 == blockidx);
        Node tuple = at(kids, tupleidx);
        const KidsP &tk = verified_kids(tuple);
        if (len(tk) > 0 && tk->back().kind == K("NewlineWs")) {
            Node block = at(kids, blockidx);
            const KidsP &bk = verified_kids(block);
            if (len(bk) > 0) JF_ASSERT(bk->front().kind != K("Whitespace"));
            Node nl = tk->back();
            tk->pop_back();
            bk->insert(bk->begin(), nl);
            at(kids, tupleidx) = make_node(tuple, tk);
            at(kids, blockidx) = make_node(block, bk);
        }
    }

    int i = 0;
    while (i < len(kids)) {
        i += 1;
        Node kid = at(kids, i);
        normalize_tree(kid);
        if (kid.kind == K("block")) {
            if (is_begin_block(kid)) {
                JF_ASSERT(at(kid.kids, 1).kind == K("begin"));
                JF_ASSERT(at(kid.kids, len(kid.kids)).kind == K("end"));
            } else if (is_begin_block(kid, K("quote"))) {
                JF_ASSERT(at(kid.kids, 1).kind == K("quote"));
                JF_ASSERT(at(kid.kids, len(kid.kids)).kind == K("end"));
            } else if (is_paren_block(kid)) {
                JF_ASSERT(at(kid.kids, 1).kind == K("("));
                JF_ASSERT(at(kid.kids, len(kid.kids)).kind == K(")"));
            }
            continue;
        }
        Node kid2, ws;
        while (pop_if_whitespace(true, kid, kid2, ws)) {
            at(kids, i) = kid = kid2;
            kids->insert(kids->begin() + (i - 1), ws);
            i += 1;
        }
        while (pop_if_whitespace(false, kid, kid2, ws)) {
            JF_ASSERT(KIn(kid.kind, K("row"), K("nrow"), K("parameters")));
            at(kids, i) = kid = kid2;
            kids->insert(kids->begin() + i, ws);
        }
    }
}

Node FromGreen(const jlparse::Tree &t, uint32_t i) {
    const jlparse::Node &g = t[i];
    if (g.leaf) return Leaf(g.kind, g.flags, g.span);
    KidsP kids = NewKids();
    kids->reserve(t.NumChildren(i));
    t.ForEachChild(i, [&](uint32_t c) { kids->push_back(FromGreen(t, c)); });
    return Node{g.kind, g.flags, g.span, std::move(kids), 0};
}

//------------------------------------------------------------------------------
// Format toggles

struct ToggleMatch {
    bool bang = false;
    std::string word;
    bool on = false;
};

// match(r"#(!)? (runic|format): (on|off)", str)
std::optional<ToggleMatch> MatchToggle(std::string_view s) {
    for (size_t p = 0; p < s.size(); ++p) {
        if (s[p] != '#') continue;
        size_t q = p + 1;
        ToggleMatch m;
        if (q < s.size() && s[q] == '!') {
            m.bang = true;
            ++q;
        }
        if (q >= s.size() || s[q] != ' ') continue;
        ++q;
        std::string_view rest = s.substr(q);
        if (rest.substr(0, 7) == "runic: ") {
            m.word = "runic";
            q += 7;
        } else if (rest.substr(0, 8) == "format: ") {
            m.word = "format";
            q += 8;
        } else {
            continue;
        }
        rest = s.substr(q);
        if (rest.substr(0, 2) == "on") {
            m.on = true;
        } else if (rest.substr(0, 3) == "off") {
            m.on = false;
        } else {
            continue;
        }
        return m;
    }
    return std::nullopt;
}

bool validate_toggle(const Ctx &ctx, const KidsP &kids, int i) {
    const bool toplevel = ctx.lineage_kinds.size() == 1 && ctx.lineage_kinds[0] == K("toplevel");
    bool valid = true;
    if (i - 1 < 1) {
        valid = valid && toplevel && i == 1;
    } else {
        Kind pk = at(kids, i - 1).kind;
        valid = valid && (pk == K("NewlineWs") || (toplevel && i == 1 && pk == K("Whitespace")));
    }
    if (i + 1 > len(kids)) {
        valid = valid && toplevel && i == len(kids);
    } else {
        valid = valid && at(kids, i + 1).kind == K("NewlineWs");
    }
    return valid;
}

constexpr int64_t kTypemax = INT64_MAX;
constexpr int64_t kTypemin = INT64_MIN;
constexpr int64_t kNoToggle = 0;  // check_format_toggle returned nothing

int64_t check_format_toggle(Ctx &ctx, const Node &node, const Node &kid, int i) {
    JF_ASSERT(ctx.format_on);
    const KidsP &kids = verified_kids(node);
    if (kid.kind != K("Comment")) return kNoToggle;
    std::optional<ToggleMatch> off = MatchToggle(ctx.read_bytes(kid));
    if (!off) return kNoToggle;
    if (off->on) return kNoToggle;
    if (!validate_toggle(ctx, kids, i)) return kNoToggle;
    size_t pos = ctx.position();
    ctx.accept(kid);
    for (int j = i + 1; j <= len(kids); ++j) {
        Node lkid = at(kids, j);
        if (lkid.kind != K("Comment")) {
            ctx.accept(lkid);
            continue;
        }
        std::optional<ToggleMatch> on = MatchToggle(ctx.read_bytes(lkid));
        if (!on || off->bang != on->bang || off->word != on->word || !on->on || !validate_toggle(ctx, kids, j)) {
            ctx.accept(lkid);
            continue;
        }
        ctx.seek(pos);
        return j;
    }
    ctx.seek(pos);
    if (ctx.lineage_kinds.size() == 1 && ctx.lineage_kinds[0] == K("toplevel")) return kTypemax;
    return kNoToggle;
}

//------------------------------------------------------------------------------
// The driver

ONode format_node_with_kids(Ctx &ctx, const Node &node) {
    if (node.leaf()) return std::nullopt;
    Kind next_sibling = ctx.next_sibling;
    ctx.next_sibling = kNoKind;
    ctx.lineage_kinds.push_back(node.kind);
    if (node.kind == K("macrocall")) ctx.lineage_macros.push_back(macrocall_name(ctx, node));

    const KidsP &kids = node.kids;
    KidsP kids2 = kids;
    bool any_kid_changed = false;
    JF_ASSERT(ctx.format_on);
    int64_t format_on_idx = kTypemin;

    for (int i = 1; i <= len(kids); ++i) {
        Node kid = at(kids, i);
        ctx.next_sibling = i + 1 <= len(kids) ? at(kids, i + 1).kind : kNoKind;
        Node kid2 = kid;
        bool this_kid_changed = false;
        int itr = 0;
        if (ctx.format_on && i > format_on_idx) {
            int64_t t = check_format_toggle(ctx, node, kid, i);
            if (t != kNoToggle) {
                ctx.format_on = false;
                format_on_idx = t;
            }
        } else if (!ctx.format_on && static_cast<int64_t>(i) > format_on_idx - 2) {
            ctx.format_on = true;
        }
        while (true) {
            size_t fmt_pos = ctx.position();
            ONode kid3 = format_node(ctx, kid2);
            if (!kid3) {
                JF_ASSERT(ctx.position() == fmt_pos + kid2.span);
                break;
            }
            this_kid_changed = true;
            ctx.seek(fmt_pos);
            kid2 = *kid3;
            if (++itr == 1000) throw Refuse("infinite loop?");
        }
        any_kid_changed = any_kid_changed || this_kid_changed;
        if (any_kid_changed) {
            if (kids2 == kids) kids2 = SliceKids(kids, i - 1);
            kids2->push_back(kid2);
        }
    }
    ctx.next_sibling = next_sibling;
    ctx.lineage_kinds.pop_back();
    if (node.kind == K("macrocall")) ctx.lineage_macros.pop_back();
    if (any_kid_changed) return make_node(node, kids2);
    return std::nullopt;
}

}  // namespace

ONode format_node(Ctx &ctx, const Node &node) {
    if (!ctx.format_on) {
        ctx.accept(node);
        return std::nullopt;
    }
    if (has_tag(node, TAG_INDENT)) ctx.indent_level += 1;
    if (has_tag(node, TAG_DEDENT)) ctx.indent_level -= 1;

#define JF_PASS(f)                       \
    do {                                 \
        ONode r = f(ctx, node);          \
        if (r) return r;                 \
    } while (0)
    JF_PASS(replace_tabs_with_four_spaces);
    JF_PASS(no_leading_and_single_trailing_newline);
    JF_PASS(max_three_consecutive_newlines);
    JF_PASS(insert_delete_mark_newlines);
    JF_PASS(remove_trailing_semicolon);
    JF_PASS(trim_trailing_whitespace);
    JF_PASS(format_hex_literals);
    JF_PASS(format_float_literals);
    JF_PASS(spaces_around_operators);
    JF_PASS(spaces_around_assignments);
    JF_PASS(spaces_around_anonymous_function);
    JF_PASS(spaces_around_ternary);
    JF_PASS(spaces_around_keywords);
    JF_PASS(spaces_in_import_using);
    JF_PASS(spaces_in_export_public);
    JF_PASS(spaces_in_let);
    JF_PASS(spaces_around_comments);
    JF_PASS(no_spaces_around_colon_etc);
    JF_PASS(parens_around_op_calls_in_colon);
    JF_PASS(for_loop_use_in);
    JF_PASS(explicit_return);
    JF_PASS(braces_around_where_rhs);
    JF_PASS(indent_multiline_strings);
    JF_PASS(four_space_indent);
    JF_PASS(spaces_in_listlike);
#undef JF_PASS

    if (node.leaf()) {
        ctx.accept(node);
        return std::nullopt;
    }
    return format_node_with_kids(ctx, node);
}

namespace {

// The leaves the self-check compares: kind + text, minus whitespace, and
// minus the tokens Runic adds/drops/respells on purpose.
struct Tok {
    Kind kind;
    std::string text;
};

void CollectToks(const jlparse::Tree &t, std::string_view src, std::vector<Tok> &out) {
    for (const jlparse::Node &n : t.nodes) {
        if (!n.leaf) continue;
        Kind k = n.kind;
        if (KIn(k, K("Whitespace"), K("NewlineWs"), K(","), K(";"), K("("), K(")"), K("{"), K("}"), K("return")))
            continue;
        std::string text(src.substr(n.offset, n.span));
        if (k == K("Comment")) {
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        } else if (KIn(k, K("Float"), K("Float32"), K("HexInt"))) {
            text.clear();
        } else if (KIn(k, K("in"), K("∈"), K("="))) {
            k = K("=");
            text.clear();
        }
        out.push_back({k, std::move(text)});
    }
}

// An error diagnostic that JuliaSyntax 0.4.10 (what Runic parses with) also
// reports. mep's parser reports Julia 1.12's (JuliaSyntax 1.0.2) diagnostics
// over 0.4.10's tree; "Ambiguous signature" (`function (@f(x))`) is a 1.0.2
// addition -- the tree is the same, and Runic itself writes `function ($x)`.
bool IsRunicParseError(const jlparse::Diagnostic &d) {
    return d.level == jlparse::Level::Error && d.message.rfind("Ambiguous signature", 0) != 0;
}

bool AnyRunicParseError(const jlparse::ParseResult &pr) {
    for (const jlparse::Diagnostic &d : pr.diagnostics)
        if (IsRunicParseError(d)) return true;
    return false;
}

int LineOf(std::string_view src, int64_t byte1) {
    int line = 1;
    for (int64_t i = 0; i < byte1 - 1 && i < static_cast<int64_t>(src.size()); ++i)
        if (src[static_cast<size_t>(i)] == '\n') ++line;
    return line;
}

}  // namespace
}  // namespace detail

Result Format(std::string_view src, const Options &opts) {
    using namespace detail;
    (void)opts;  // Runic has no options
    Result res;
    res.text = std::string(src);
    jlparse::ParseResult pr = jlparse::Parse(src);
    if (AnyRunicParseError(pr)) {
        for (const jlparse::Diagnostic &d : pr.diagnostics) {
            if (!IsRunicParseError(d)) continue;
            res.error = "parse error: " + d.message;
            res.error_line = LineOf(src, d.first_byte);
            break;
        }
        return res;
    }
    try {
        Node root = FromGreen(pr.tree, 0);
        normalize_tree(root);
        Ctx ctx;
        ctx.io.buf.assign(src.substr(0, root.span));
        Node root2 = root;
        int itr = 0;
        while (true) {
            ONode r = format_node(ctx, root2);
            if (!r) {
                JF_ASSERT(ctx.position() == root2.span);
                break;
            }
            ctx.seek(0);
            root2 = *r;
            if (++itr > 100) throw Refuse("root node modified more than 100 times?");
        }
        if (ctx.io.buf.size() > root2.span) ctx.io.buf.resize(root2.span);
        jlparse::ParseResult pr2 = jlparse::Parse(ctx.io.buf);
        if (AnyRunicParseError(pr2)) throw Refuse("re-parsing the formatted output failed");
        std::vector<Tok> a, b;
        CollectToks(pr.tree, src, a);
        CollectToks(pr2.tree, ctx.io.buf, b);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i) same = a[i].kind == b[i].kind && a[i].text == b[i].text;
        if (!same) throw Refuse("self-check: the formatted tokens differ from the input's");
        res.text = std::move(ctx.io.buf);
        res.ok = true;
    } catch (const Refuse &e) {
        res.error = e.what();
    }
    return res;
}

}  // namespace juliafmt
