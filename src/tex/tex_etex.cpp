// e-TeX's larger pieces for meptex::Engine (mep-etex.ch calls these):
// expressions (\numexpr, \dimexpr, \glueexpr, \muexpr), the status
// enquiries that return values (\currentgrouplevel, \fontcharwd ...),
// \showgroups and \showifs and the group and conditional tracing,
// \scantokens. Written from the e-TeX manual's description of each.

#include "tex/gen/tex_gen.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace meptex {

namespace {
constexpr int32_t kInfinity = 2147483647;  // \S108's infinity: the largest integer
constexpr int32_t kMaxDimen = 07777777777;  // \S421's max_dimen: 2^30-1 sp
}  // namespace

// --- Printing C strings ---------------------------------------------------------------
//
// The generated print routines take pool string numbers; text written here
// goes through print_char, which is what print does for a multi-character
// string anyway.

void Engine::print_str(const char *s) {
    for (; *s; ++s) print_char(static_cast<uint8_t>(*s));
}

void Engine::print_esc_str(const char *s) {
    const int32_t esc = int_par(tc::escape_char_code);
    if (esc >= 0 && esc < 256) print(esc);
    print_str(s);
}

void Engine::print_nl_str(const char *s) {
    if ((term_offset > 0 && podd(selector)) || (file_offset > 0 && selector >= tc::log_only)) print_ln();
    print_str(s);
}

void Engine::print_err_str(const char *s) {
    print_nl_str("! ");
    print_str(s);
}

int32_t Engine::pool_string(const char *s) {
    // Reused only while that number still holds this text: TeX gives the
    // numbers of flushed strings out again.
    const size_t len = std::strlen(s);
    auto it = pool_string_cache_.find(s);
    if (it != pool_string_cache_.end() && it->second < str_ptr) {
        const int32_t n = it->second;
        if (static_cast<size_t>(str_start[n + 1] - str_start[n]) == len &&
            std::memcmp(&str_pool[str_start[n]], s, len) == 0)
            return n;
    }
    for (const char *p = s; *p; ++p) {
        if (pool_ptr >= pool_size) break;
        str_pool[pool_ptr++] = static_cast<uint8_t>(*p);
    }
    const int32_t n = make_string();
    pool_string_cache_[s] = n;
    return n;
}

void Engine::set_help(std::initializer_list<const char *> lines) {
    // tex.web keeps help lines last-first: help_line[0] is the bottom line.
    help_ptr = static_cast<int32_t>(lines.size());
    int32_t k = help_ptr - 1;
    for (const char *l : lines) help_line[k--] = pool_string(l);
}

// --- Groups and conditionals ----------------------------------------------------------

void Engine::print_group(bool leaving) {
    using namespace tc;
    switch (cur_group) {
    case bottom_level:
        print_str("bottom level");
        return;
    case simple_group:
    case semi_simple_group:
        if (cur_group == semi_simple_group) print_str("semi ");
        print_str("simple");
        break;
    case hbox_group:
    case adjusted_hbox_group:
        if (cur_group == adjusted_hbox_group) print_str("adjusted ");
        print_str("hbox");
        break;
    case vbox_group: print_str("vbox"); break;
    case vtop_group: print_str("vtop"); break;
    case align_group:
    case no_align_group:
        if (cur_group == no_align_group) print_str("no ");
        print_str("align");
        break;
    case output_group: print_str("output"); break;
    case disc_group: print_str("disc"); break;
    case insert_group: print_str("insert"); break;
    case vcenter_group: print_str("vcenter"); break;
    case math_group:
    case math_choice_group:
    case math_shift_group:
    case math_left_group:
        print_str("math");
        if (cur_group == math_choice_group) print_str(" choice");
        else if (cur_group == math_shift_group) print_str(" shift");
        else if (cur_group == math_left_group) print_str(" left");
        break;
    default: break;
    }
    print_str(" group (level ");
    print_int((leaving ? cur_level + 1 : cur_level) - tc::level_one);
    print_char(')');
    // new_save_level put the starting line just below the boundary: two
    // below save_ptr when entering, one below when leaving (unsave has
    // stopped at the boundary).
    const int32_t line_at = save_stack[save_ptr - (leaving ? 1 : 2)].int_;
    if (line_at != 0) {
        print_str(leaving ? " entered at line " : " at line ");
        print_int(line_at);
    }
}

void Engine::group_trace(bool leaving) {
    begin_diagnostic();
    print_char('{');
    print_str(leaving ? "leaving " : "entering ");
    print_group(leaving);
    print_char('}');
    end_diagnostic(false);
}

void Engine::show_save_groups() {
    using namespace tc;
    // Walk the boundaries from the innermost group out, showing each the
    // way \tracinggroups names it, and the command that began it.
    const int32_t saved_group = cur_group, saved_level = cur_level, saved_boundary = cur_boundary;
    const int32_t saved_ptr = save_ptr;
    print_nl_str("");
    print_ln();
    while (cur_group != bottom_level) {
        save_ptr = cur_boundary;  // so print_group finds the line one below
        print_nl_str("### ");
        cur_level--;  // print_group(true) prints cur_level+1
        print_group(true);
        cur_level++;
        const char *begun = nullptr;
        switch (cur_group) {
        case simple_group: begun = "{"; break;
        case semi_simple_group: begun = "\\begingroup"; break;
        case hbox_group:
        case adjusted_hbox_group: begun = "\\hbox{"; break;
        case vbox_group: begun = "\\vbox{"; break;
        case vtop_group: begun = "\\vtop{"; break;
        case align_group: begun = "\\halign{"; break;
        case no_align_group: begun = "\\noalign{"; break;
        case output_group: begun = "\\output{"; break;
        case disc_group: begun = "\\discretionary{"; break;
        case insert_group: begun = "\\insert{"; break;
        case vcenter_group: begun = "\\vcenter{"; break;
        case math_group: begun = "{"; break;
        case math_choice_group: begun = "\\mathchoice{"; break;
        case math_shift_group: begun = "$"; break;
        case math_left_group: begun = "\\left"; break;
        default: break;
        }
        if (begun) {
            print_str(" (");
            print_str(begun);
            print_char(')');
        }
        // up one level: the boundary records the enclosing group and boundary
        const int32_t b = cur_boundary;
        cur_group = save_stack[b].hh.b1;
        cur_boundary = save_stack[b].hh.rh;
        cur_level--;
    }
    print_nl_str("### bottom level");
    cur_group = saved_group;
    cur_level = saved_level;
    cur_boundary = saved_boundary;
    save_ptr = saved_ptr;
}

void Engine::print_if_line(int32_t l) {
    if (l != 0) {
        print_str(" entered on line ");
        print_int(l);
    }
}

void Engine::show_if_status() {
    using namespace tc;
    int32_t n, l;
    print_str(": ");
    if (cur_cmd == fi_or_else) {
        print_cmd_chr(if_test, cur_if);
        print_char(' ');
        n = 0;
        l = if_line;
    } else {
        n = 1;
        l = line_;
    }
    for (int32_t q = cond_ptr; q != tc::null; q = link(q)) n++;
    print_str("(level ");
    print_int(n);
    print_char(')');
    print_if_line(l);
}

void Engine::show_if_list() {
    using namespace tc;
    int32_t n = 0;
    for (int32_t p = cond_ptr; p != tc::null; p = link(p)) n++;
    if (n == 0) {
        print_nl_str("### no active conditionals");
        return;
    }
    int32_t p = cond_ptr, t = cur_if, l = if_line, m = if_limit;
    do {
        print_nl_str("### level ");
        print_int(n);
        print_str(": ");
        print_cmd_chr(if_test, t);
        if (m == fi_code) print_esc_str("else");
        print_if_line(l);
        n--;
        t = subtype(p);
        l = mem[p + 1].int_;
        m = type(p);
        p = link(p);
    } while (p != tc::null);
}

// --- Status enquiries ---------------------------------------------------------------------

void Engine::print_etex_last_item(int32_t chr) {
    using namespace tc;
    if (chr > mu_expr_code) {
        print_pdf_last_item(chr);
        return;
    }
    const char *n = "badness";
    switch (chr) {
    case eTeX_version_code: n = "eTeXversion"; break;
    case current_group_level_code: n = "currentgrouplevel"; break;
    case current_group_type_code: n = "currentgrouptype"; break;
    case current_if_level_code: n = "currentiflevel"; break;
    case current_if_type_code: n = "currentiftype"; break;
    case current_if_branch_code: n = "currentifbranch"; break;
    case glue_stretch_order_code: n = "gluestretchorder"; break;
    case glue_shrink_order_code: n = "glueshrinkorder"; break;
    case last_node_type_code: n = "lastnodetype"; break;
    case font_char_wd_code: n = "fontcharwd"; break;
    case font_char_ht_code: n = "fontcharht"; break;
    case font_char_dp_code: n = "fontchardp"; break;
    case font_char_ic_code: n = "fontcharic"; break;
    case par_shape_length_code: n = "parshapelength"; break;
    case par_shape_indent_code: n = "parshapeindent"; break;
    case par_shape_dimen_code: n = "parshapedimen"; break;
    case glue_stretch_code: n = "gluestretch"; break;
    case glue_shrink_code: n = "glueshrink"; break;
    case mu_to_glue_code: n = "mutoglue"; break;
    case glue_to_mu_code: n = "gluetomu"; break;
    case num_expr_code: n = "numexpr"; break;
    case dimen_expr_code: n = "dimexpr"; break;
    case glue_expr_code: n = "glueexpr"; break;
    case mu_expr_code: n = "muexpr"; break;
    default: break;
    }
    print_esc_str(n);
}

// A glue result handed to scan_something_internal, which adds a reference
// to whatever it returns (tex.web's results there are borrowed from eqtb).
// These results are already the caller's, so give one back first.
void Engine::return_owned_glue(int32_t q, int32_t level) {
    glue_ref_count(q) -= 1;
    cur_val = q;
    cur_val_level = level;
}

void Engine::fetch_etex_item(int32_t m) {
    using namespace tc;
    if (m > mu_expr_code) {
        fetch_pdf_item(m);
        return;
    }
    switch (m) {
    case eTeX_version_code:
        cur_val = tc::eTeX_version;
        cur_val_level = int_val;
        return;
    case current_group_level_code:
        cur_val = cur_level - level_one;
        cur_val_level = int_val;
        return;
    case current_group_type_code:
        cur_val = cur_group;
        cur_val_level = int_val;
        return;
    case current_if_level_code: {
        int32_t n = 0;
        for (int32_t p = cond_ptr; p != tc::null; p = link(p)) n++;
        cur_val = n;
        cur_val_level = int_val;
        return;
    }
    case current_if_type_code:
        if (cond_ptr == tc::null) cur_val = 0;
        else if (cur_if < unless_code) cur_val = cur_if + 1;
        else cur_val = -(cur_if - unless_code + 1);
        cur_val_level = int_val;
        return;
    case current_if_branch_code:
        if (if_limit == or_code || if_limit == else_code) cur_val = 1;
        else if (if_limit == fi_code) cur_val = -1;
        else cur_val = 0;
        cur_val_level = int_val;
        return;
    case glue_stretch_order_code:
    case glue_shrink_order_code: {
        scan_glue(glue_val);
        const int32_t q = cur_val;
        cur_val = m == glue_stretch_order_code ? stretch_order(q) : shrink_order(q);
        delete_glue_ref(q);
        cur_val_level = int_val;
        return;
    }
    case last_node_type_code:
        cur_val_level = int_val;
        if (tail() == head() || mode() == 0) {
            cur_val = (mode() == vmode && tail() == head()) ? last_node_type : -1;
        } else if (is_char_node(tail())) {
            cur_val = 0;
        } else if (type(tail()) <= unset_node) {
            cur_val = type(tail()) + 1;
        } else {
            cur_val = unset_node + 2;
        }
        return;
    case font_char_wd_code:
    case font_char_ht_code:
    case font_char_dp_code:
    case font_char_ic_code: {
        scan_font_ident();
        const int32_t fnt = cur_val;
        scan_char_num();
        cur_val_level = dimen_val;
        if (font_bc[fnt] <= cur_val && font_ec[fnt] >= cur_val) {
            const four_quarters ci = char_info(fnt, cur_val);
            if (!char_exists(ci)) {
                cur_val = 0;
            } else if (m == font_char_wd_code) {
                cur_val = char_width(fnt, ci);
            } else if (m == font_char_ht_code) {
                cur_val = char_height(fnt, ci);
            } else if (m == font_char_dp_code) {
                cur_val = char_depth(fnt, ci);
            } else {
                cur_val = char_italic(fnt, ci);
            }
        } else {
            cur_val = 0;
        }
        return;
    }
    case par_shape_length_code:
    case par_shape_indent_code:
    case par_shape_dimen_code: {
        int32_t q = m - par_shape_length_code;  // 0 length, 1 indent, 2 dimen
        scan_int();
        const int32_t ps = equiv(par_shape_loc);
        cur_val_level = dimen_val;
        if (ps == tc::null || cur_val <= 0) {
            cur_val = 0;
            return;
        }
        int32_t n = cur_val;
        if (q == 2) {  // \parshapedimen: odd n the indent, even n the length
            q = n % 2;  // 1 indent, 0 length
            n = (n + q) / 2;
        }
        if (n > info(ps)) n = info(ps);
        cur_val = mem[ps + 2 * n - q].int_;
        return;
    }
    case glue_stretch_code:
    case glue_shrink_code: {
        scan_glue(glue_val);
        const int32_t q = cur_val;
        cur_val = m == glue_stretch_code ? stretch(q) : shrink(q);
        delete_glue_ref(q);
        cur_val_level = dimen_val;
        return;
    }
    case mu_to_glue_code:
        scan_glue(mu_val);
        return_owned_glue(cur_val, glue_val);
        return;
    case glue_to_mu_code:
        scan_glue(glue_val);
        return_owned_glue(cur_val, mu_val);
        return;
    case num_expr_code:
    case dimen_expr_code:
    case glue_expr_code:
    case mu_expr_code: {
        const int32_t l = m - num_expr_code;  // int_val .. mu_val
        const int32_t v = scan_expr(l);
        if (l >= glue_val) {
            return_owned_glue(v, l);
        } else {
            cur_val = v;
            cur_val_level = l;
        }
        return;
    }
    default:
        cur_val = 0;
        cur_val_level = int_val;
        return;
    }
}

int32_t Engine::fetch_penalties(int32_t m) {
    scan_int();
    const int32_t q = equiv(m);
    if (q == tc::null || cur_val < 0) return 0;
    const int32_t n = mem[q + 1].int_;
    if (cur_val == 0) return n;
    const int32_t k = cur_val > n ? n : cur_val;
    return mem[q + 1 + k].int_;
}

// --- Expressions --------------------------------------------------------------------------
//
// <expr> = <term> {+|- <term>}; <term> = <factor> {*|/ <integer factor>};
// <factor> = ( <expr> ) | a quantity of the expression's type. Every
// result is checked against the type's range; division rounds (halves
// away from zero); a multiplication followed directly by a division is
// done as one scaling with a 64-bit intermediate. Overflow sets
// arith_error, reported once by scan_expr.

namespace {

int64_t Bound(int32_t l) { return l == tc::int_val ? kInfinity : kMaxDimen; }

// n/d rounded, halves away from zero
int64_t RoundedQuotient(int64_t n, int64_t d) {
    const bool neg = (n < 0) != (d < 0);
    const int64_t an = n < 0 ? -n : n, ad = d < 0 ? -d : d;
    int64_t q = an / ad;
    if (2 * (an - q * ad) >= ad) q++;
    return neg ? -q : q;
}

}  // namespace

void Engine::expr_next() {
    do {
        get_x_token();
    } while (cur_cmd == tc::spacer);
}

bool Engine::expr_is(int chr) const { return cur_tok == tc::other_token + chr; }

int32_t Engine::expr_check(int64_t v, int32_t l) {
    if (v > Bound(l) || v < -Bound(l)) {
        arith_error = true;
        return 0;
    }
    return static_cast<int32_t>(v);
}

int32_t Engine::expr_quantity(int32_t l) {
    using namespace tc;
    switch (l) {
    case int_val: scan_int(); break;
    case dimen_val: scan_dimen(false, false, false); break;
    case glue_val: scan_glue(glue_val); break;
    default: scan_glue(mu_val); break;
    }
    return cur_val;
}

int32_t Engine::expr_factor(int32_t l) {
    expr_next();
    if (expr_is('(')) {
        const int32_t v = expr_sum(l);
        if (!expr_is(')')) {
            print_err_str("Missing ) inserted for expression");
            set_help({"I was expecting to see `+', `-', `*', `/', or `)'. Didn't."});
            back_error();
        }
        expr_next();
        return v;
    }
    back_input();
    const int32_t v = expr_quantity(l);
    expr_next();
    return v;
}

int32_t Engine::expr_scale(int32_t l, int32_t v, int64_t n, int64_t d) {
    // f*n/d (d==0: plain f*n) on one value of type l, glue componentwise
    auto one = [&](int32_t x, int32_t lv) -> int32_t {
        if (d == 0) return expr_check(static_cast<int64_t>(x) * n, lv);
        return expr_check(RoundedQuotient(static_cast<int64_t>(x) * n, d), lv);
    };
    if (l < tc::glue_val) return one(v, l);
    const int32_t r = new_spec(v);
    width(r) = one(width(v), tc::dimen_val);
    stretch(r) = one(stretch(v), tc::dimen_val);
    shrink(r) = one(shrink(v), tc::dimen_val);
    if (stretch(r) == 0) stretch_order(r) = tc::normal;
    if (shrink(r) == 0) shrink_order(r) = tc::normal;
    delete_glue_ref(v);
    return r;
}

int32_t Engine::expr_term(int32_t l) {
    int32_t v = expr_factor(l);
    for (;;) {
        if (expr_is('*')) {
            const int32_t n = expr_factor(tc::int_val);
            if (expr_is('/')) {
                const int32_t d = expr_factor(tc::int_val);
                if (d == 0) {
                    arith_error = true;
                    v = expr_scale(l, v, 0, 1);
                } else {
                    v = expr_scale(l, v, n, d);
                }
            } else {
                v = expr_scale(l, v, n, 0);
            }
        } else if (expr_is('/')) {
            const int32_t d = expr_factor(tc::int_val);
            if (d == 0) {
                arith_error = true;
                v = expr_scale(l, v, 0, 1);
            } else {
                v = expr_scale(l, v, 1, d);
            }
        } else {
            return v;
        }
    }
}

int32_t Engine::expr_add(int32_t l, int32_t a, int32_t b, bool subtract) {
    if (l < tc::glue_val) {
        const int64_t bb = subtract ? -static_cast<int64_t>(b) : b;
        return expr_check(static_cast<int64_t>(a) + bb, l);
    }
    const int32_t sg = subtract ? -1 : 1;
    const int32_t r = new_spec(a);
    width(r) = expr_check(static_cast<int64_t>(width(a)) + sg * static_cast<int64_t>(width(b)), tc::dimen_val);
    if (stretch_order(a) == stretch_order(b)) {
        stretch(r) = expr_check(static_cast<int64_t>(stretch(a)) + sg * static_cast<int64_t>(stretch(b)), tc::dimen_val);
    } else if (stretch_order(a) < stretch_order(b) && stretch(b) != 0) {
        stretch(r) = sg * stretch(b);
        stretch_order(r) = stretch_order(b);
    }
    if (shrink_order(a) == shrink_order(b)) {
        shrink(r) = expr_check(static_cast<int64_t>(shrink(a)) + sg * static_cast<int64_t>(shrink(b)), tc::dimen_val);
    } else if (shrink_order(a) < shrink_order(b) && shrink(b) != 0) {
        shrink(r) = sg * shrink(b);
        shrink_order(r) = shrink_order(b);
    }
    if (stretch(r) == 0) stretch_order(r) = tc::normal;
    if (shrink(r) == 0) shrink_order(r) = tc::normal;
    delete_glue_ref(a);
    delete_glue_ref(b);
    return r;
}

int32_t Engine::expr_sum(int32_t l) {
    int32_t e = expr_term(l);
    for (;;) {
        if (expr_is('+')) {
            e = expr_add(l, e, expr_term(l), false);
        } else if (expr_is('-')) {
            e = expr_add(l, e, expr_term(l), true);
        } else {
            return e;
        }
    }
}

int32_t Engine::scan_expr(int32_t l) {
    const bool outer_error = arith_error;
    arith_error = false;
    int32_t e = expr_sum(l);
    // One \relax ends the expression and is absorbed; anything else that
    // ended it is read again.
    if (cur_cmd != tc::relax) back_input();
    if (arith_error) {
        print_err_str("Arithmetic overflow");
        set_help({"I can't evaluate this expression,", "since the result is out of range."});
        error_();
        if (l >= tc::glue_val) {
            delete_glue_ref(e);
            e = tc::zero_glue;
            glue_ref_count(e) += 1;
        } else {
            e = 0;
        }
    }
    arith_error = outer_error;
    return e;
}

// --- \scantokens ------------------------------------------------------------------------

void Engine::pseudo_start() {
    using namespace tc;
    scan_general_text();
    // The tokens as characters, the way \write would write them, cut into
    // lines at each \newlinechar.
    const int32_t saved_selector = selector;
    selector = new_string;
    const int32_t b = pool_ptr;
    token_show(temp_head());
    selector = saved_selector;
    flush_list(link(temp_head()));
    std::vector<std::string> lines(1);
    const int32_t nl = int_par(new_line_char_code);
    for (int32_t k = b; k < pool_ptr; k++) {
        const int32_t ch = str_pool[k];
        if (ch == nl) {
            lines.emplace_back();
        } else {
            lines.back().push_back(static_cast<char>(ch));
        }
    }
    pool_ptr = b;
    begin_file_reading();
    line_ = 0;
    limit() = start();
    loc() = limit() + 1;  // the first line is read as the next one
    input_file[index_()].open_lines(std::move(lines));
    if (int_par(tracing_scan_tokens_code) > 0) {
        if (term_offset > max_print_line - 3) {
            print_ln();
        } else if (term_offset > 0 || file_offset > 0) {
            print_char(' ');
        }
        name() = traced_pseudo_name;
        print_str("( ");
        open_parens++;
        term_out.flush();
    } else {
        name() = pseudo_name;
    }
}

}  // namespace meptex

namespace meptex {

// --- Mark classes -------------------------------------------------------------------------
//
// Class 0 is TeX's own cur_mark array; the others live here, kept by the
// same rules (\S977-979 for \vsplit, \S1012-1016 for the page).

int32_t &Engine::class_mark_slot(int32_t cls, int32_t kind) {
    if (class_marks_.empty()) class_marks_.assign(static_cast<size_t>(5 * tc::number_regs), tc::null);
    if (cls > max_mark_class_) max_mark_class_ = cls;
    return class_marks_[static_cast<size_t>(cls * 5 + kind)];
}

int32_t Engine::class_mark(int32_t cls, int32_t kind) {
    if (class_marks_.empty() || cls > max_mark_class_) return tc::null;
    return class_marks_[static_cast<size_t>(cls * 5 + kind)];
}

void Engine::vsplit_marks_init() {
    for (int32_t k = 1; k <= max_mark_class_; k++) {
        for (int32_t kind : {tc::split_first_mark_code, tc::split_bot_mark_code}) {
            int32_t &slot = class_mark_slot(k, kind);
            if (slot != tc::null) {
                delete_token_ref(slot);
                slot = tc::null;
            }
        }
    }
}

void Engine::vsplit_class_mark(int32_t p) {
    const int32_t cls = mark_class(p);
    int32_t &fst = class_mark_slot(cls, tc::split_first_mark_code);
    int32_t &btm = class_mark_slot(cls, tc::split_bot_mark_code);
    if (fst == tc::null) {
        fst = mark_ptr(p);
        btm = fst;
        token_ref_count(fst) += 2;
    } else {
        delete_token_ref(btm);
        btm = mark_ptr(p);
        token_ref_count(btm) += 1;
    }
}

void Engine::fire_up_marks_init() {
    for (int32_t k = 1; k <= max_mark_class_; k++) {
        int32_t &btm = class_mark_slot(k, tc::bot_mark_code);
        if (btm == tc::null) continue;
        int32_t &tp = class_mark_slot(k, tc::top_mark_code);
        int32_t &fst = class_mark_slot(k, tc::first_mark_code);
        if (tp != tc::null) delete_token_ref(tp);
        tp = btm;
        token_ref_count(tp) += 1;
        if (fst != tc::null) delete_token_ref(fst);
        fst = tc::null;
    }
}

void Engine::page_class_mark(int32_t p) {
    const int32_t cls = mark_class(p);
    int32_t &fst = class_mark_slot(cls, tc::first_mark_code);
    int32_t &btm = class_mark_slot(cls, tc::bot_mark_code);
    if (fst == tc::null) {
        fst = mark_ptr(p);
        token_ref_count(fst) += 1;
    }
    if (btm != tc::null) delete_token_ref(btm);
    btm = mark_ptr(p);
    token_ref_count(btm) += 1;
}

void Engine::fire_up_marks_done() {
    for (int32_t k = 1; k <= max_mark_class_; k++) {
        int32_t &tp = class_mark_slot(k, tc::top_mark_code);
        int32_t &fst = class_mark_slot(k, tc::first_mark_code);
        if (tp != tc::null && fst == tc::null) {
            fst = tp;
            token_ref_count(tp) += 1;
        }
    }
}

void Engine::class_marks_cleanup() {
    for (int32_t k = 1; k <= max_mark_class_; k++) {
        for (int32_t kind = tc::top_mark_code; kind <= tc::split_bot_mark_code; kind++) {
            int32_t &slot = class_mark_slot(k, kind);
            if (slot != tc::null) {
                delete_token_ref(slot);
                slot = tc::null;
            }
        }
    }
}

}  // namespace meptex
