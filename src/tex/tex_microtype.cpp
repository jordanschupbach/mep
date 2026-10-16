// pdfTeX's micro-typography for mep's TeX engine: character protrusion
// (\pdfprotrudechars, \lpcode, \rpcode) and font expansion
// (\pdfadjustspacing, \pdffontexpand, \efcode). Written from the pdfTeX
// manual and checked against pdfTeX's own line breaks and \showbox
// output, not ported.
//
// With \pdfprotrudechars > 0, each line of a paragraph gets a margin kern
// at either end that pulls its first and last visible characters out into
// the margin, by \lpcode / \rpcode thousandths of the font's quad. With
// \pdfprotrudechars > 1 the line breaker counts that width too. The kerns
// are nodes of their own (margin_kern_node, mep-pdftex.ch) that otherwise
// act as kerns, and \leftmarginkern / \rightmarginkern read them back.

#include "tex/gen/tex_gen.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace meptex {

namespace {

constexpr int32_t kLeftSide = 0, kRightSide = 1;  // margin kern subtypes (left_side, right_side)

}  // namespace

bool Engine::margin_skippable(int32_t p) {
    // what may stand between a margin and the character that protrudes
    using namespace tc;
    if (is_char_node(p)) return false;
    switch (type(p)) {
    case ins_node:
    case mark_node:
    case adjust_node:
    case penalty_node: return true;
    case disc_node: return info(p + 1) == null && link(p + 1) == null && subtype(p) == 0;  // empty
    case math_node: return width(p) == 0;
    case kern_node: return width(p) == 0 || subtype(p) == normal;
    case glue_node: return glue_ptr(p) == zero_glue;
    case hlist_node: return width(p) == 0 && height(p) == 0 && depth(p) == 0 && list_ptr(p) == null;
    default: return false;
    }
}

int32_t Engine::margin_protrusion(int32_t p, int32_t side) {
    // how far node p sticks out at `side`: a character's (or ligature's)
    // \lpcode or \rpcode in thousandths of its font's quad, rounded
    using namespace tc;
    if (p == null) return 0;
    if (!is_char_node(p)) {
        if (type(p) != ligature_node) return 0;
        p = p + 1;  // lig_char
    }
    const int32_t fnt = type(p), chr = subtype(p);
    const int32_t code = font_char_code_of_base(side == kLeftSide ? lp_code_base : rp_code_base, fnt, chr);
    if (code == 0) return 0;
    const int64_t prod = static_cast<int64_t>(font_info[quad_code + param_base[fnt]].int_) * code;
    return static_cast<int32_t>(prod >= 0 ? (prod + 500) / 1000 : -((-prod + 500) / 1000));
}

int32_t Engine::margin_first_item(int32_t p, bool top) {
    // the first node of list p that is not skippable, looking into boxes;
    // a box holding nothing but skippable nodes is what stops the search.
    // At the top level, the last node if all are skippable; below it, null.
    using namespace tc;
    while (p != null) {
        if (!is_char_node(p) && type(p) == hlist_node && list_ptr(p) != null) {
            const int32_t q = margin_first_item(list_ptr(p), false);
            return q != null ? q : p;
        }
        if (!margin_skippable(p)) return p;
        if (link(p) == null) return top ? p : null;
        p = link(p);
    }
    return null;
}

int32_t Engine::margin_last_item(int32_t l, int32_t r, bool top) {
    // the same from the other end: the last node from l to r
    using namespace tc;
    if (l == null || r == null) return null;
    std::vector<int32_t> nodes;
    for (int32_t p = l;; p = link(p)) {
        if (p == null) return null;  // r is not in the list
        nodes.push_back(p);
        if (p == r) break;
    }
    for (size_t i = nodes.size(); i-- > 0;) {
        const int32_t p = nodes[i];
        if (!is_char_node(p) && type(p) == hlist_node && list_ptr(p) != null) {
            int32_t end = list_ptr(p);
            while (link(end) != null) end = link(end);
            const int32_t q = margin_last_item(list_ptr(p), end, false);
            return q != null ? q : p;
        }
        if (!margin_skippable(p)) return p;
        if (i == 0) return top ? p : null;
    }
    return null;
}

int32_t Engine::margin_line_start(int32_t l, bool discard) {
    // the node whose left protrusion counts for a line beginning at l: an
    // empty box first (\parindent=0pt) is passed over; at a break not yet
    // taken, so are the glue, kerns and penalties the break will discard
    using namespace tc;
    if (l == null) return null;
    if (link(l) != null && !is_char_node(l) && type(l) == hlist_node && width(l) == 0 && height(l) == 0 &&
        depth(l) == 0 && list_ptr(l) == null) {
        l = link(l);
    } else if (discard) {
        while (link(l) != null && !is_char_node(l) && type(l) >= math_node) l = link(l);
    }
    return margin_first_item(l, true);
}

int32_t Engine::line_protrusion(int32_t pass, int32_t brk) {
    // the protrusion of both ends of a line from the break `pass` (a
    // passive node, or null: the paragraph's start) to brk (null: its end)
    using namespace tc;
    const int32_t start = pass == null ? link(temp_head()) : link(pass + 1);  // cur_break(pass)
    int32_t right;
    if (brk != null && type(brk) == disc_node && info(brk + 1) != null) {
        right = info(brk + 1);  // a hyphen: the end of the pre-break text
        while (link(right) != null) right = link(right);
    } else {
        int32_t prev = start;
        while (prev != null && link(prev) != brk) prev = link(prev);
        right = margin_last_item(start, prev, true);
    }
    int32_t left = start;
    if (left != null && !is_char_node(left) && type(left) == disc_node) {
        if (link(left + 1) != null) {
            left = link(left + 1);  // the post-break text begins the line
            return margin_protrusion(left, kLeftSide) + margin_protrusion(right, kRightSide);
        }
        int32_t n = subtype(left);  // replace_count: the text a break removes
        left = link(left);
        for (; n > 0; n--)
            if (link(left) != null) left = link(left);
    }
    left = margin_line_start(left, true);
    return margin_protrusion(left, kLeftSide) + margin_protrusion(right, kRightSide);
}

int32_t Engine::new_margin_kern(int32_t w, int32_t side) {
    const int32_t k = get_node(tc::small_node_size);
    type(k) = static_cast<uint16_t>(tc::margin_kern_node);
    subtype(k) = static_cast<uint16_t>(side);
    width(k) = w;
    link(k) = tc::null;
    return k;
}

int32_t Engine::protrude_right(int32_t q, bool after) {
    // the margin kern at the end of the line link(temp_head)..q: after q
    // (the end of a hyphen's pre-break text), else just before it (the
    // break, or the paragraph's last node); returns the line's new last
    // node before the \rightskip
    using namespace tc;
    int32_t prev = temp_head();
    while (link(prev) != q && link(prev) != null) prev = link(prev);
    if (link(prev) != q) return q;
    const int32_t from = after ? q : prev;
    if (from == temp_head()) return q;
    const int32_t w = margin_protrusion(margin_last_item(link(temp_head()), from, true), kRightSide);
    if (w == 0) return q;
    const int32_t k = new_margin_kern(-w, kRightSide);
    if (after) {
        link(k) = link(q);
        link(q) = k;
        return k;
    }
    link(k) = q;
    link(prev) = k;
    return q;
}

int32_t Engine::protrude_left(int32_t q) {
    // the margin kern at the start of the line q (its discardable nodes
    // already gone); returns the line's new first node
    const int32_t w = margin_protrusion(margin_line_start(q, false), kLeftSide);
    if (w == 0) return q;
    const int32_t k = new_margin_kern(-w, kLeftSide);
    link(k) = q;
    return k;
}

int32_t Engine::margin_kern_of_box(int32_t b, bool right) {
    // \leftmarginkern / \rightmarginkern: the width of the margin kern at
    // that end of an hbox's list, past glue (\leftskip, \rightskip) and
    // skippable nodes; 0 if there is none
    using namespace tc;
    if (b == null || type(b) != hlist_node) return 0;
    std::vector<int32_t> nodes;
    for (int32_t p = list_ptr(b); p != null; p = link(p)) nodes.push_back(p);
    if (right) std::vector<int32_t>(nodes.rbegin(), nodes.rend()).swap(nodes);
    for (const int32_t p : nodes) {
        if (is_char_node(p)) return 0;
        if (type(p) == margin_kern_node) return subtype(p) == (right ? kRightSide : kLeftSide) ? width(p) : 0;
        if (type(p) != glue_node && !margin_skippable(p)) return 0;
    }
    return 0;
}

// --- Font expansion -------------------------------------------------------------------------------
//
// \pdffontexpand gives a font its limits (stretch and shrink, in thousandths
// of its widths, and the step between them). When a paragraph's line is
// packed with \pdfadjustspacing > 0, its characters are set in a copy of
// their font whose widths are expanded by the multiple of the step nearest
// what the line needs (each character by its \efcode thousandths of that),
// before the glue takes up the rest; a copy prints as `\tenrm (+20)' and
// is drawn with the PDF font scaled across. With \pdfadjustspacing > 1
// the line breaker counts the stretch and shrink fonts allow, so the
// breaks may move (mep-pdftex.ch).

int32_t Engine::font_base_of(int32_t fnt) {
    auto it = pdf_.expanded_of.find(fnt);
    return it == pdf_.expanded_of.end() ? fnt : it->second.first;
}

int32_t Engine::font_expansion_of(int32_t fnt) {
    auto it = pdf_.expanded_of.find(fnt);
    return it == pdf_.expanded_of.end() ? 0 : it->second.second;
}

int32_t Engine::font_char_code_of_base(int32_t code, int32_t fnt, int32_t chr) {
    return font_char_code(code, font_base_of(fnt), chr);
}

int32_t Engine::char_expand_amount(int32_t fnt, int32_t chr, bool stretching) {
    // how much character chr of fnt widens (or narrows) at its font's limit
    const int32_t base = font_base_of(fnt);
    auto it = pdf_.font_expand.find(base);
    if (it == pdf_.font_expand.end()) return 0;
    pdf_.expand_last = it->second;  // the limits the line breaker reckons steps by
    const int32_t lim = stretching ? it->second.stretch : it->second.shrink;
    if (lim == 0 || chr < font_bc[base] || chr > font_ec[base]) return 0;
    const four_quarters ci = char_info(base, chr);
    if (!char_exists(ci)) return 0;
    const int64_t ef = font_char_code(tc::ef_code_base, base, chr);
    const int64_t v = static_cast<int64_t>(char_width(base, ci)) * lim * ef;
    return static_cast<int32_t>((v + 500000) / 1000000);
}

int32_t Engine::char_stretch(int32_t fnt, int32_t chr) { return char_expand_amount(fnt, chr, true); }
int32_t Engine::char_shrink(int32_t fnt, int32_t chr) { return char_expand_amount(fnt, chr, false); }

int32_t Engine::expansion_shortfall(int32_t shortfall, int32_t fstretch, int32_t fshrink) {
    // what of a line's shortfall is left to its glue when its fonts expand:
    // all they cannot take, else what the step leaves (each font goes to its
    // nearest step, so at most half of one step's width)
    const auto &lim = pdf_.expand_last;
    if (shortfall > 0 && fstretch > 0) {
        if (fstretch <= shortfall) return shortfall - fstretch;
        return lim.stretch > 0 ? fstretch / (lim.stretch / lim.step) / 2 : 0;
    }
    if (shortfall < 0 && fshrink > 0) {
        if (fshrink <= -shortfall) return shortfall + fshrink;
        return lim.shrink > 0 ? -(fshrink / (lim.shrink / lim.step) / 2) : 0;
    }
    return shortfall;
}

int32_t Engine::make_font_copy(int32_t base, int32_t e, int32_t ls, bool own_params) {
    // a new TeX font sharing base's metrics but for its widths: each
    // character's widened by e thousandths (by its \efcode share of them)
    // and by ls; with own_params, \fontdimen parameters of its own too.
    // tex.web's own font arrays, so the rest of TeX takes it as any font.
    // null_font if there is no room.
    const int32_t bc = font_bc[base], ec = font_ec[base];
    const int32_t n = ec >= bc ? ec - bc + 1 : 0;
    const int32_t np = own_params ? font_params[base] : 0;
    if (font_ptr >= font_max || fmem_ptr + 2 * n + 1 + np > font_mem_size) return tc::null_font;
    const int32_t cp = ++font_ptr;
    font_check[cp] = font_check[base];
    font_size[cp] = font_size[base];
    font_dsize[cp] = font_dsize[base];
    font_params[cp] = font_params[base];
    font_name[cp] = font_name[base];
    font_area[cp] = font_area[base];
    font_bc[cp] = font_bc[base];
    font_ec[cp] = font_ec[base];
    font_glue[cp] = tc::null;
    font_used[cp] = false;
    hyphen_char[cp] = hyphen_char[base];
    skew_char[cp] = skew_char[base];
    bchar_label[cp] = bchar_label[base];
    font_bchar[cp] = font_bchar[base];
    font_false_bchar[cp] = font_false_bchar[base];
    height_base[cp] = height_base[base];
    depth_base[cp] = depth_base[base];
    italic_base[cp] = italic_base[base];
    lig_kern_base[cp] = lig_kern_base[base];
    kern_base[cp] = kern_base[base];
    exten_base[cp] = exten_base[base];
    param_base[cp] = param_base[base];
    text(tc::font_id_base + cp) = text(tc::font_id_base + base);
    // its own char_info words, each with a width of its own
    char_base[cp] = fmem_ptr - bc;
    width_base[cp] = fmem_ptr + n;
    font_info[width_base[cp]].int_ = 0;
    for (int32_t ch = bc; ch <= ec; ch++) {
        four_quarters ci = char_info(base, ch);
        if (char_exists(ci)) {
            const int64_t ef = e != 0 ? font_char_code(tc::ef_code_base, base, ch) : 0;
            const int64_t w = char_width(base, ci);
            const int64_t num = w * (1000000 + e * ef);
            font_info[width_base[cp] + ch - bc + 1].int_ =
                static_cast<int32_t>(num >= 0 ? (num + 500000) / 1000000 : -((-num + 500000) / 1000000)) + ls;
            ci.b0 = static_cast<uint16_t>(ch - bc + 1);
        }
        font_info[char_base[cp] + ch].qqqq = ci;
    }
    fmem_ptr += 2 * n + 1;
    if (own_params) {
        param_base[cp] = fmem_ptr - 1;  // parameter k is at param_base + k
        for (int32_t k = 1; k <= np; k++) font_info[param_base[cp] + k] = font_info[param_base[base] + k];
        fmem_ptr += np;
    }
    return cp;
}

int32_t Engine::expanded_font(int32_t base, int32_t e) {
    // base's copy with widths expanded by e thousandths (made the first time)
    if (e == 0) return base;
    if (auto it = pdf_.expanded_fonts.find({base, e}); it != pdf_.expanded_fonts.end()) return it->second;
    const int32_t cp = make_font_copy(base, e, 0, false);
    if (cp == tc::null_font) return base;  // no room: unexpanded
    pdf_.expanded_fonts[{base, e}] = cp;
    pdf_.expanded_of[cp] = {base, e};
    return cp;
}

int32_t Engine::font_cs_text(int32_t u) {
    // the name a font identifier u shows (tex.web §1257)
    using namespace tc;
    if (u >= hash_base) return text(u);
    if (u >= single_base) return u == null_cs ? pool_string("FONT") : u - single_base;
    const int32_t saved = selector;
    selector = new_string;
    print_str("FONT");
    print(u - active_base);
    selector = saved;
    return make_string();
}

void Engine::new_font_copy(bool letterspace, bool global) {
    // \letterspacefont cs font n: font with n thousandths of its quad added
    // to every width, the glyphs centred in the extra space; \pdfcopyfont
    // cs font: a copy of font (with \lpcode and the like of its own)
    using namespace tc;
    get_r_token();
    const int32_t u = cur_cs;
    const int32_t t = font_cs_text(u);
    auto define = [&](int32_t e) { global ? geq_define(u, set_font, e) : eq_define(u, set_font, e); };
    define(null_font);
    scan_font_ident();
    const int32_t base = cur_val;
    int32_t amount = 0, ls = 0;
    if (letterspace) {
        scan_int();
        amount = cur_val;
        const int64_t v = static_cast<int64_t>(font_info[tc::quad_code + param_base[base]].int_) * amount;
        ls = static_cast<int32_t>(v >= 0 ? (v + 500) / 1000 : -((-v + 500) / 1000));
    }
    const int32_t cp = make_font_copy(base, 0, ls, true);
    if (cp == null_font) {
        print_err_str("Font memory is full");
        set_help({"No room for another font."});
        error_();
        return;
    }
    if (letterspace)
        font_name[cp] = pool_string((pool_to_string(font_name[base]) + "+" + std::to_string(amount) + "ls").c_str());
    pdf_.font_copy_of[cp] = base;
    if (ls != 0) pdf_.letterspace[cp] = ls;
    define(cp);
    eqtb[font_id_base + cp] = eqtb[u];
    text(font_id_base + cp) = t;
}

int32_t Engine::pdf_font_root(int32_t fnt) {
    // the font a copy (expanded, letterspaced, \pdfcopyfont) is drawn with
    for (int guard = 0; guard < 8; guard++) {
        if (auto it = pdf_.expanded_of.find(fnt); it != pdf_.expanded_of.end()) fnt = it->second.first;
        else if (auto jt = pdf_.font_copy_of.find(fnt); jt != pdf_.font_copy_of.end()) fnt = jt->second;
        else break;
    }
    return fnt;
}

int32_t Engine::font_letterspace(int32_t fnt) {
    fnt = font_base_of(fnt);
    auto it = pdf_.letterspace.find(fnt);
    return it == pdf_.letterspace.end() ? 0 : it->second;
}

void Engine::print_font_expansion(int32_t fnt) {
    const int32_t e = font_expansion_of(fnt);
    if (e == 0) return;
    print_str(e > 0 ? " (+" : " (");
    print_int(e);
    print_char(')');
}

void Engine::expand_line(int32_t q, int32_t w) {
    // choose the expansion of the line q, to be packed to width w, and set
    // its characters in those fonts
    using namespace tc;
    int64_t x = 0, fstretch = 0, fshrink = 0;
    bool inf_stretch = false, inf_shrink = false;
    auto chr_of = [&](int32_t p) { return is_char_node(p) ? p : p + 1; };  // a ligature's lig_char
    for (int32_t p = q; p != null; p = link(p)) {
        if (is_char_node(p) || type(p) == ligature_node) {
            const int32_t cn = chr_of(p);
            x += char_width(type(cn), char_info(type(cn), subtype(cn)));
            fstretch += char_expand_amount(type(cn), subtype(cn), true);
            fshrink += char_expand_amount(type(cn), subtype(cn), false);
            continue;
        }
        switch (type(p)) {
        case hlist_node:
        case vlist_node:
        case rule_node:
        case unset_node:
        case kern_node:
        case math_node:
        case margin_kern_node: x += width(p); break;
        case glue_node: {
            const int32_t gs = glue_ptr(p);
            x += width(gs);
            if (stretch_order(gs) != normal && stretch(gs) != 0) inf_stretch = true;
            if (shrink_order(gs) != normal && shrink(gs) != 0) inf_shrink = true;
            break;
        }
        default: break;
        }
    }
    const int64_t d = w - x;
    double ratio;
    if (d > 0 && !inf_stretch && fstretch > 0) ratio = std::min(1.0, static_cast<double>(d) / static_cast<double>(fstretch));
    else if (d < 0 && !inf_shrink && fshrink > 0) ratio = -std::min(1.0, static_cast<double>(-d) / static_cast<double>(fshrink));
    else return;
    auto subst = [&](int32_t p) {
        const int32_t cn = chr_of(p);
        const int32_t base = font_base_of(type(cn));
        auto it = pdf_.font_expand.find(base);
        if (it == pdf_.font_expand.end()) return;
        const int32_t lim = ratio > 0 ? it->second.stretch : it->second.shrink, step = it->second.step;
        const auto steps = static_cast<int32_t>(std::lround(std::fabs(ratio) * lim / step));
        const auto ef = static_cast<uint16_t>(expanded_font(base, (ratio > 0 ? 1 : -1) * steps * step));
        type(cn) = ef;
        if (!is_char_node(p))  // a ligature's characters, which its display shows
            for (int32_t r = link(p + 1); r != null; r = link(r)) type(r) = ef;
    };
    for (int32_t p = q; p != null; p = link(p)) {
        if (is_char_node(p) || type(p) == ligature_node) {
            subst(p);
        } else if (type(p) == disc_node) {  // the texts of a break not taken
            for (int32_t r = info(p + 1); r != null; r = link(r))
                if (is_char_node(r) || type(r) == ligature_node) subst(r);
            for (int32_t r = link(p + 1); r != null; r = link(r))
                if (is_char_node(r) || type(r) == ligature_node) subst(r);
        }
    }
}

}  // namespace meptex
