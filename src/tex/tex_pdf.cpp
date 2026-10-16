// pdfTeX's primitives for meptex::Engine (mep-pdftex.ch calls these): the
// expandable string and file primitives, \pdfprimitive, the font character
// codes, and the commands that make PDF objects and whatsit nodes. The PDF
// file itself is written by tex_pdf_out.cpp. Written from the pdfTeX
// manual's description of each primitive.

#include "tex/gen/tex_gen.h"
#include "pdf_crypt.h"
#include "tex/tex_resolver.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <string>
#include <system_error>

namespace meptex {

namespace {

constexpr int32_t kPdfNodeSize = 6;  // every pdfTeX whatsit: dims at +1..+3, data index at +4, tokens at +5

std::string Hex2(unsigned v) {
    static const char kDigits[] = "0123456789ABCDEF";
    std::string s(2, '0');
    s[0] = kDigits[(v >> 4) & 15];
    s[1] = kDigits[v & 15];
    return s;
}

// D:YYYYMMDDHHmmSS+HH'mm' (or Z), as pdfTeX writes dates.
std::string PdfDate(std::time_t t, bool utc) {
    std::tm tmv{};
    if (utc) {
        gmtime_r(&t, &tmv);
    } else {
        localtime_r(&t, &tmv);
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "D:%04d%02d%02d%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    std::string s = buf;
    if (utc) return s + "Z";
    const long off = tmv.tm_gmtoff;
    if (off == 0) return s + "Z";
    const long a = off < 0 ? -off : off;
    std::snprintf(buf, sizeof buf, "%c%02ld'%02ld'", off < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
    return s + buf;
}

// The time pdfTeX dates things by: SOURCE_DATE_EPOCH when it is set (as
// TeX Live does for the creation date), else now.
std::time_t PdfNow(bool *utc) {
    if (const char *e = std::getenv("SOURCE_DATE_EPOCH"); e && *e) {
        *utc = true;
        return static_cast<std::time_t>(std::atoll(e));
    }
    *utc = false;
    return std::time(nullptr);
}

// MD5 in uppercase hex, as \pdfmdfivesum gives it.
std::string Md5Hex(const std::string &data) {
    std::string out;
    for (char ch : pdfcrypt::Md5(data)) out += Hex2(static_cast<unsigned char>(ch));
    return out;
}

}  // namespace

// --- Small helpers ----------------------------------------------------------------------

std::string Engine::tokens_to_string(int32_t list) {
    const int32_t saved = selector;
    selector = tc::new_string;
    const int32_t b = pool_ptr;
    show_token_list(list, tc::null, pool_size - pool_ptr);
    selector = saved;
    std::string s;
    s.reserve(static_cast<size_t>(pool_ptr - b));
    for (int32_t k = b; k < pool_ptr; k++) s.push_back(static_cast<char>(str_pool[k]));
    pool_ptr = b;
    return s;
}

int32_t Engine::scan_toks_list(bool expand) {
    // scan_toks leaves its list in def_ref, which the command that is
    // expanding us (\message, \edef, \write ...) still needs.
    const int32_t saved = def_ref;
    scan_toks(false, expand);
    const int32_t r = def_ref;
    def_ref = saved;
    return r;
}

std::string Engine::scan_pdf_text(bool expand) {
    // A <general text>; the list starts with its reference count.
    const int32_t r = scan_toks_list(expand);
    std::string s = tokens_to_string(link(r));
    flush_list(r);
    return s;
}

void Engine::insert_string(const std::string &s) {
    const int32_t b = pool_ptr;
    for (char ch : s) {
        if (pool_ptr >= pool_size) break;
        str_pool[pool_ptr++] = static_cast<uint8_t>(ch);
    }
    link(garbage()) = str_toks(b);
    begin_token_list(link(temp_head()), tc::inserted);
}

bool Engine::scan_kw(const char *kw) { return scan_keyword(pool_string(kw)); }

void Engine::scan_pdf_rule_spec(int32_t &w, int32_t &h, int32_t &d) {
    for (;;) {
        if (scan_kw("width")) {
            scan_dimen(false, false, false);
            w = cur_val;
        } else if (scan_kw("height")) {
            scan_dimen(false, false, false);
            h = cur_val;
        } else if (scan_kw("depth")) {
            scan_dimen(false, false, false);
            d = cur_val;
        } else {
            break;
        }
    }
}

int32_t Engine::pdf_new_obj(int32_t kind) {
    const int32_t n = pdf_.next_obj++;
    if (static_cast<int32_t>(pdf_.objects.size()) <= n) pdf_.objects.resize(static_cast<size_t>(n) + 1);
    pdf_.objects[static_cast<size_t>(n)] = PdfObject{};
    pdf_.objects[static_cast<size_t>(n)].kind = static_cast<PdfObject::Kind>(kind);
    pdf_.objects[static_cast<size_t>(n)].num = n;
    return n;
}

PdfObject *Engine::pdf_obj(int32_t n) {
    if (n <= 0 || n >= static_cast<int32_t>(pdf_.objects.size())) return nullptr;
    return &pdf_.objects[static_cast<size_t>(n)];
}

int32_t Engine::new_pdf_node(int32_t sub) {
    new_whatsit(sub, kPdfNodeSize);
    const int32_t p = tail();
    width(p) = tc::null_flag;
    depth(p) = tc::null_flag;
    height(p) = tc::null_flag;
    int32_t k;
    if (!pdf_.free_nodes.empty()) {
        k = pdf_.free_nodes.back();
        pdf_.free_nodes.pop_back();
        pdf_.nodes[static_cast<size_t>(k)] = PdfNodeData{};
    } else {
        k = static_cast<int32_t>(pdf_.nodes.size());
        pdf_.nodes.emplace_back();
    }
    pdf_.nodes[static_cast<size_t>(k)].used = true;
    mem[p + 4].int_ = k;
    link(p + 5) = tc::null;
    info(p + 5) = 0;
    return p;
}

PdfNodeData &Engine::pdf_node(int32_t p) { return pdf_.nodes[static_cast<size_t>(mem[p + 4].int_)]; }

std::string Engine::find_input_file(const std::string &name) {
    // as kpathsea finds \input files for pdfTeX's file primitives and
    // images: `.tex' added when the name has no extension, then as given
    // (in tex/ and the font trees)
    if (!resolver) return name;
    std::string path = resolver->find(name, kTexFile);
    if (path.empty()) path = resolver->find(name, kOtherFile);
    return path;
}

// --- \pdfprimitive and \ifpdfprimitive ---------------------------------------------------------
//
// Each primitive gets a twin control sequence when it is defined (in
// INITEX, so the twins are in the format): the primitive's name behind a
// prefix no input can produce. \pdfprimitive\foo reads the twin of \foo;
// \ifpdfprimitive\foo asks whether \foo still means what its twin does.

static constexpr char kPrimPrefix[] = "\x7fprim:";

int32_t Engine::primitive_twin(int32_t cs, bool make) {
    std::string name;
    if (cs >= tc::single_base && cs < tc::null_cs) {
        name.push_back(static_cast<char>(cs - tc::single_base));
    } else if (cs >= tc::hash_base && cs < tc::undefined_control_sequence) {
        const int32_t s = text(cs);
        if (s <= 0 || s >= str_ptr) return tc::undefined_control_sequence;
        for (int32_t k = str_start[s]; k < str_start[s + 1]; k++) name.push_back(static_cast<char>(str_pool[k]));
    } else {
        return tc::undefined_control_sequence;
    }
    const std::string full = kPrimPrefix + name;
    if (first + static_cast<int32_t>(full.size()) >= buf_size) return tc::undefined_control_sequence;
    for (size_t k = 0; k < full.size(); k++) buffer[first + static_cast<int32_t>(k)] = static_cast<uint8_t>(full[k]);
    const bool saved = no_new_control_sequence;
    no_new_control_sequence = !make;
    const int32_t t = id_lookup(first, static_cast<int32_t>(full.size()));
    no_new_control_sequence = saved;
    return t;
}

void Engine::store_primitive(int32_t cs) {
    const int32_t t = primitive_twin(cs, true);
    if (t == tc::undefined_control_sequence) return;
    eq_level(t) = tc::level_one;
    eq_type(t) = eq_type(cs);
    equiv(t) = equiv(cs);
}

void Engine::pdf_primitive_expand() {
    const int32_t saved = scanner_status;
    scanner_status = tc::normal;
    get_token();
    scanner_status = saved;
    if (cur_cs != 0) {
        const int32_t t = primitive_twin(cur_cs, false);
        if (t != tc::undefined_control_sequence && eq_type(t) != tc::undefined_cs) {
            cur_tok = tc::cs_token_flag + t;
            back_input();
            return;
        }
    }
    print_err_str("Missing primitive name");
    set_help({"The control sequence marked <to be read again> does not",
              "represent any known primitive."});
    back_error();
}

bool Engine::pdf_if_primitive() {
    const int32_t saved = scanner_status;
    scanner_status = tc::normal;
    get_next();
    scanner_status = saved;
    if (cur_cs == 0) return false;
    const int32_t t = primitive_twin(cur_cs, false);
    if (t == tc::undefined_control_sequence || eq_type(t) == tc::undefined_cs) return false;
    return eq_type(cur_cs) == eq_type(t) && equiv(cur_cs) == equiv(t);
}

// --- Font character codes ------------------------------------------------------------------

int32_t Engine::font_char_code(int32_t code, int32_t fnt, int32_t chr) {
    const int64_t key = (static_cast<int64_t>(code) << 40) | (static_cast<int64_t>(fnt) << 8) | chr;
    auto it = pdf_.font_char_codes.find(key);
    if (it != pdf_.font_char_codes.end()) return it->second;
    return code == tc::ef_code_base ? 1000 : 0;  // \efcode defaults to 1000
}

void Engine::set_font_char_code(int32_t code, int32_t fnt, int32_t chr, int32_t v) {
    const int64_t key = (static_cast<int64_t>(code) << 40) | (static_cast<int64_t>(fnt) << 8) | chr;
    pdf_.font_char_codes[key] = v;
}

// --- Printing command names -------------------------------------------------------------------

void Engine::print_pdf_convert(int32_t chr) {
    using namespace tc;
    const char *n = "pdfstrcmp";
    switch (chr) {
    case expanded_code: n = "expanded"; break;
    case pdftex_revision_code: n = "pdftexrevision"; break;
    case pdftex_banner_code: n = "pdftexbanner"; break;
    case pdf_font_name_code: n = "pdffontname"; break;
    case pdf_font_objnum_code: n = "pdffontobjnum"; break;
    case pdf_font_size_code: n = "pdffontsize"; break;
    case pdf_page_ref_code: n = "pdfpageref"; break;
    case pdf_xform_name_code: n = "pdfxformname"; break;
    case pdf_escape_string_code: n = "pdfescapestring"; break;
    case pdf_escape_name_code: n = "pdfescapename"; break;
    case pdf_escape_hex_code: n = "pdfescapehex"; break;
    case pdf_unescape_hex_code: n = "pdfunescapehex"; break;
    case pdf_creation_date_code: n = "pdfcreationdate"; break;
    case pdf_file_mod_date_code: n = "pdffilemoddate"; break;
    case pdf_file_size_code: n = "pdffilesize"; break;
    case pdf_mdfive_sum_code: n = "pdfmdfivesum"; break;
    case pdf_file_dump_code: n = "pdffiledump"; break;
    case pdf_match_code: n = "pdfmatch"; break;
    case pdf_last_match_code: n = "pdflastmatch"; break;
    case pdf_uniform_deviate_code: n = "pdfuniformdeviate"; break;
    case pdf_normal_deviate_code: n = "pdfnormaldeviate"; break;
    case pdf_colorstack_init_code: n = "pdfcolorstackinit"; break;
    case left_margin_kern_code: n = "leftmarginkern"; break;
    case right_margin_kern_code: n = "rightmarginkern"; break;
    case pdf_ximage_bbox_code: n = "pdfximagebbox"; break;
    case pdf_insert_ht_code: n = "pdfinsertht"; break;
    default: break;
    }
    print_esc_str(n);
}

void Engine::print_pdf_last_item(int32_t chr) {
    using namespace tc;
    const char *n = "pdftexversion";
    switch (chr) {
    case pdf_last_obj_code: n = "pdflastobj"; break;
    case pdf_last_xform_code: n = "pdflastxform"; break;
    case pdf_last_ximage_code: n = "pdflastximage"; break;
    case pdf_last_ximage_pages_code: n = "pdflastximagepages"; break;
    case pdf_last_annot_code: n = "pdflastannot"; break;
    case pdf_last_link_code: n = "pdflastlink"; break;
    case pdf_last_x_pos_code: n = "pdflastxpos"; break;
    case pdf_last_y_pos_code: n = "pdflastypos"; break;
    case pdf_retval_code: n = "pdfretval"; break;
    case pdf_last_ximage_colordepth_code: n = "pdflastximagecolordepth"; break;
    case elapsed_time_code: n = "pdfelapsedtime"; break;
    case pdf_shell_escape_code: n = "pdfshellescape"; break;
    case random_seed_code: n = "pdfrandomseed"; break;
    case meptex_version_code: n = "meptexversion"; break;
    default: break;
    }
    print_esc_str(n);
}

void Engine::print_pdf_extension(int32_t chr) {
    using namespace tc;
    const char *n = "[unknown extension!]";
    switch (chr) {
    case pdf_literal_node: n = "pdfliteral"; break;
    case pdf_refobj_node: n = "pdfrefobj"; break;
    case pdf_refxform_node: n = "pdfrefxform"; break;
    case pdf_refximage_node: n = "pdfrefximage"; break;
    case pdf_annot_node: n = "pdfannot"; break;
    case pdf_start_link_node: n = "pdfstartlink"; break;
    case pdf_end_link_node: n = "pdfendlink"; break;
    case pdf_dest_node: n = "pdfdest"; break;
    case pdf_thread_node: n = "pdfthread"; break;
    case pdf_start_thread_node: n = "pdfstartthread"; break;
    case pdf_end_thread_node: n = "pdfendthread"; break;
    case pdf_save_pos_node: n = "pdfsavepos"; break;
    case pdf_colorstack_node: n = "pdfcolorstack"; break;
    case pdf_setmatrix_node: n = "pdfsetmatrix"; break;
    case pdf_save_node: n = "pdfsave"; break;
    case pdf_restore_node: n = "pdfrestore"; break;
    case pdf_snap_ref_point_node: n = "pdfsnaprefpoint"; break;
    case pdf_snapy_node: n = "pdfsnapy"; break;
    case pdf_snapy_comp_node: n = "pdfsnapycomp"; break;
    case pdf_obj_code: n = "pdfobj"; break;
    case pdf_xform_code: n = "pdfxform"; break;
    case pdf_ximage_code: n = "pdfximage"; break;
    case pdf_outline_code: n = "pdfoutline"; break;
    case pdf_info_code: n = "pdfinfo"; break;
    case pdf_catalog_code: n = "pdfcatalog"; break;
    case pdf_names_code: n = "pdfnames"; break;
    case pdf_trailer_code: n = "pdftrailer"; break;
    case pdf_trailer_id_code: n = "pdftrailerid"; break;
    case pdf_map_file_code: n = "pdfmapfile"; break;
    case pdf_map_line_code: n = "pdfmapline"; break;
    case pdf_font_attr_code: n = "pdffontattr"; break;
    case pdf_font_expand_code: n = "pdffontexpand"; break;
    case pdf_include_chars_code: n = "pdfincludechars"; break;
    case pdf_glyph_to_unicode_code: n = "pdfglyphtounicode"; break;
    case pdf_nobuiltin_tounicode_code: n = "pdfnobuiltintounicode"; break;
    case pdf_no_ligatures_code: n = "pdfnoligatures"; break;
    case pdf_interword_space_on_code: n = "pdfinterwordspaceon"; break;
    case pdf_interword_space_off_code: n = "pdfinterwordspaceoff"; break;
    case pdf_fake_space_code: n = "pdffakespace"; break;
    case pdf_space_font_code: n = "pdfspacefont"; break;
    case pdf_running_link_off_code: n = "pdfrunninglinkoff"; break;
    case pdf_running_link_on_code: n = "pdfrunninglinkon"; break;
    case pdf_reset_timer_code: n = "pdfresettimer"; break;
    case pdf_set_random_seed_code: n = "pdfsetrandomseed"; break;
    case quitvmode_code: n = "quitvmode"; break;
    case partokenname_code: n = "partokenname"; break;
    case letterspace_font_code: n = "letterspacefont"; break;
    case pdf_copy_font_code: n = "pdfcopyfont"; break;
    default:
        print_str(n);
        return;
    }
    print_esc_str(n);
}

// --- Random numbers and the timer -----------------------------------------------------------

namespace {
uint64_t SplitMix(uint64_t &s) {
    uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
}  // namespace

void Engine::pdf_random_init() {
    if (pdf_.random_init) return;
    pdf_.random_seed = static_cast<uint64_t>(std::time(nullptr) % 1000000000);
    pdf_.random_state = pdf_.random_seed;
    pdf_.random_init = true;
}

int32_t Engine::pdf_uniform(int32_t n) {
    pdf_random_init();
    if (n == 0) return 0;
    const uint64_t r = SplitMix(pdf_.random_state);
    const int64_t m = n < 0 ? -static_cast<int64_t>(n) : n;
    const int64_t v = static_cast<int64_t>(r % static_cast<uint64_t>(m));
    return static_cast<int32_t>(n < 0 ? -v : v);
}

int32_t Engine::pdf_normal() {
    pdf_random_init();
    const double u1 = (static_cast<double>(SplitMix(pdf_.random_state) >> 11) + 1.0) / 9007199254740993.0;
    const double u2 = static_cast<double>(SplitMix(pdf_.random_state) >> 11) / 9007199254740992.0;
    const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    return static_cast<int32_t>(std::lround(z * 65536.0));
}

double Engine::pdf_clock() {
    using namespace std::chrono;
    return static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1e6;
}

// --- Last items ------------------------------------------------------------------------------

void Engine::fetch_pdf_item(int32_t m) {
    using namespace tc;
    cur_val_level = int_val;
    switch (m) {
    case pdftex_version_code: cur_val = 140; return;
    case pdf_last_obj_code: cur_val = pdf_.last_obj; return;
    case pdf_last_xform_code: cur_val = pdf_.last_xform; return;
    case pdf_last_ximage_code: cur_val = pdf_.last_ximage; return;
    case pdf_last_ximage_pages_code: cur_val = pdf_.last_ximage_pages; return;
    case pdf_last_annot_code: cur_val = pdf_.last_annot; return;
    case pdf_last_link_code: cur_val = pdf_.last_link; return;
    case pdf_last_x_pos_code: cur_val = pdf_.last_x_pos; return;
    case pdf_last_y_pos_code: cur_val = pdf_.last_y_pos; return;
    case pdf_retval_code: cur_val = pdf_.retval; return;
    case pdf_last_ximage_colordepth_code: cur_val = pdf_.last_ximage_colordepth; return;
    case elapsed_time_code: {
        if (pdf_.timer_start == 0) pdf_.timer_start = pdf_clock();
        const double secs = pdf_clock() - pdf_.timer_start;
        const double v = secs * 65536.0;
        cur_val = v > 2147483647.0 ? 2147483647 : static_cast<int32_t>(v);
        return;
    }
    case pdf_shell_escape_code: cur_val = 0; return;
    case meptex_version_code: cur_val = 1; return;  // which documents may test for: \ifdefined\meptexversion
    case random_seed_code:
        pdf_random_init();
        cur_val = static_cast<int32_t>(pdf_.random_seed);
        return;
    default: cur_val = 0; return;
    }
}

// --- Expandable primitives (convert) -------------------------------------------------------

void Engine::pdf_convert(int32_t code) {
    using namespace tc;
    switch (code) {
    case expanded_code: {
        // the expansion of the general text, put back to be read (and so,
        // inside \edef and \message, expanded again: as in pdfTeX)
        const int32_t r = scan_toks_list(true);
        const int32_t list = link(r);
        link(r) = null;
        flush_list(r);
        if (list != null) begin_token_list(list, inserted);
        return;
    }
    case pdf_strcmp_code: {
        const std::string a = scan_pdf_text(true);
        const std::string b = scan_pdf_text(true);
        const int cmp = a.compare(b);
        insert_string(cmp < 0 ? "-1" : cmp > 0 ? "1" : "0");
        return;
    }
    case pdftex_revision_code: insert_string("27"); return;
    case pdftex_banner_code: insert_string("This is mep-tex, Version 3.141592653-2.6-1.40.27"); return;
    case pdf_escape_string_code: {
        const std::string s = scan_pdf_text(true);
        std::string out;
        for (char sc : s) {
            const auto ch = static_cast<unsigned char>(sc);
            if (ch == '(' || ch == ')' || ch == '\\') {
                out.push_back('\\');
                out.push_back(static_cast<char>(ch));
            } else if (ch <= 32 || ch >= 127) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03o", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
        insert_string(out);
        return;
    }
    case pdf_escape_name_code: {
        const std::string s = scan_pdf_text(true);
        std::string out;
        for (char sc : s) {
            const auto ch = static_cast<unsigned char>(sc);
            if (ch <= 32 || ch >= 127 || std::strchr("#%()/<>[]{}", ch)) {
                out.push_back('#');
                out += Hex2(ch);
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
        insert_string(out);
        return;
    }
    case pdf_escape_hex_code: {
        const std::string s = scan_pdf_text(true);
        std::string out;
        for (char sc : s) out += Hex2(static_cast<unsigned char>(sc));
        insert_string(out);
        return;
    }
    case pdf_unescape_hex_code: {
        const std::string s = scan_pdf_text(true);
        std::string out;
        int nib = -1;
        for (char ch : s) {
            int v;
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
            else continue;
            if (nib < 0) {
                nib = v;
            } else {
                out.push_back(static_cast<char>(nib * 16 + v));
                nib = -1;
            }
        }
        if (nib >= 0) out.push_back(static_cast<char>(nib * 16));
        insert_string(out);
        return;
    }
    case pdf_creation_date_code: {
        bool utc;
        const std::time_t t = PdfNow(&utc);
        insert_string(PdfDate(t, utc));
        return;
    }
    case pdf_file_mod_date_code:
    case pdf_file_size_code:
    case pdf_mdfive_sum_code:
    case pdf_file_dump_code: {
        int64_t offset = 0, length = -1;
        bool as_file = false;
        if (code == pdf_file_dump_code) {
            if (scan_kw("offset")) {
                scan_int();
                offset = cur_val;
            }
            if (scan_kw("length")) {
                scan_int();
                length = cur_val;
            }
        }
        if (code == pdf_mdfive_sum_code && scan_kw("file")) as_file = true;
        const std::string arg = scan_pdf_text(true);
        if (code == pdf_mdfive_sum_code && !as_file) {
            insert_string(Md5Hex(arg));
            return;
        }
        const std::string path = find_input_file(arg);
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
            insert_string("");
            return;
        }
        if (code == pdf_file_size_code) {
            insert_string(std::to_string(std::filesystem::file_size(path, ec)));
        } else if (code == pdf_file_mod_date_code) {
            bool utc;
            std::time_t t = PdfNow(&utc);
            const char *force = std::getenv("FORCE_SOURCE_DATE");
            if (!(force && std::strcmp(force, "1") == 0 && utc)) {
                struct stat st{};
                if (::stat(path.c_str(), &st) == 0) t = st.st_mtime;
                utc = false;
            }
            insert_string(PdfDate(t, utc));
        } else {
            std::ifstream in(path, std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (code == pdf_mdfive_sum_code) {
                insert_string(Md5Hex(bytes));
            } else {
                std::string out;
                if (offset < static_cast<int64_t>(bytes.size())) {
                    const int64_t n = length < 0 ? 0 : std::min<int64_t>(length, static_cast<int64_t>(bytes.size()) - offset);
                    for (int64_t k = 0; k < n; k++) out += Hex2(static_cast<unsigned char>(bytes[static_cast<size_t>(offset + k)]));
                }
                insert_string(out);
            }
        }
        return;
    }
    case pdf_match_code: {
        bool icase = scan_kw("icase");
        int32_t subcount = -1;
        if (scan_kw("subcount")) {
            scan_int();
            subcount = cur_val;
        }
        const std::string pattern = scan_pdf_text(true);
        const std::string subject = scan_pdf_text(true);
        pdf_.matches.clear();
        try {
            auto flags = std::regex::extended;
            if (icase) flags |= std::regex::icase;
            std::regex re(pattern, flags);
            std::smatch m;
            if (std::regex_search(subject, m, re)) {
                const size_t n = subcount >= 0 ? std::min<size_t>(m.size(), static_cast<size_t>(subcount)) : m.size();
                for (size_t k = 0; k < n; k++) {
                    if (m[k].matched)
                        pdf_.matches.emplace_back(static_cast<int32_t>(m.position(k)), m.str(k));
                    else
                        pdf_.matches.emplace_back(-1, "");
                }
                insert_string("1");
            } else {
                insert_string("0");
            }
        } catch (const std::regex_error &) {
            insert_string("-1");
        }
        return;
    }
    case pdf_last_match_code: {
        scan_int();
        const int32_t n = cur_val;
        if (n >= 0 && n < static_cast<int32_t>(pdf_.matches.size())) {
            const auto &mt = pdf_.matches[static_cast<size_t>(n)];
            insert_string(std::to_string(mt.first) + "->" + mt.second);
        } else {
            insert_string("-1->");
        }
        return;
    }
    case pdf_uniform_deviate_code:
        scan_int();
        insert_string(std::to_string(pdf_uniform(cur_val)));
        return;
    case pdf_normal_deviate_code: insert_string(std::to_string(pdf_normal())); return;
    case pdf_colorstack_init_code: {
        PdfColorStack cs;
        cs.page_start = scan_kw("page");
        cs.direct = scan_kw("direct");
        cs.initial = scan_pdf_text(true);
        pdf_.colorstacks.push_back(cs);
        insert_string(std::to_string(pdf_.colorstacks.size() - 1));
        return;
    }
    case pdf_font_name_code:
    case pdf_font_objnum_code:
    case pdf_font_size_code: {
        scan_font_ident();
        const int32_t fnt = cur_val;
        if (code == pdf_font_size_code) {
            const int32_t saved = selector;
            selector = new_string;
            const int32_t b = pool_ptr;
            print_scaled(font_size[fnt]);
            print_str("pt");
            selector = saved;
            link(garbage()) = str_toks(b);
            begin_token_list(link(temp_head()), inserted);
        } else if (code == pdf_font_name_code) {
            insert_string(std::to_string(pdf_font_resource_number(fnt)));
        } else {
            insert_string(std::to_string(pdf_font_object(fnt)));
        }
        return;
    }
    case pdf_page_ref_code: {
        scan_int();
        insert_string(std::to_string(pdf_page_object(cur_val)));
        return;
    }
    case pdf_xform_name_code: {
        scan_int();
        insert_string(std::to_string(cur_val));
        return;
    }
    case left_margin_kern_code:
    case right_margin_kern_code:
        scan_eight_bit_int();
        insert_string("0pt");
        return;
    case pdf_ximage_bbox_code: {
        scan_int();
        const int32_t n = cur_val;
        scan_int();
        const int32_t k = cur_val;
        int32_t v = 0;
        if (PdfObject *o = pdf_obj(n); o && o->kind == PdfObject::Ximage && k >= 1 && k <= 4) v = o->bbox[k - 1];
        const int32_t saved = selector;
        selector = new_string;
        const int32_t b = pool_ptr;
        print_scaled(v);
        print_str("pt");
        selector = saved;
        link(garbage()) = str_toks(b);
        begin_token_list(link(temp_head()), inserted);
        return;
    }
    case pdf_insert_ht_code: {
        scan_eight_bit_int();
        insert_string("0.0pt");
        return;
    }
    default:
        return;
    }
}

// --- Extensions: commands that make nodes or objects -----------------------------------------------

void Engine::scan_pdf_action(PdfNodeData &d) {
    // <action spec>: user {..} | goto [file {..}] (page n {..} | num n | name {..}) [newwindow|nonewwindow]
    //               | thread [file {..}] (num n | name {..})
    std::string a;
    if (scan_kw("user")) {
        a = "user " + scan_pdf_text(true);
    } else if (scan_kw("goto")) {
        a = "goto";
        if (scan_kw("file")) a += " file{" + scan_pdf_text(true) + "}";
        if (scan_kw("page")) {
            scan_int();
            a += " page " + std::to_string(cur_val) + " {" + scan_pdf_text(true) + "}";
        } else if (scan_kw("num")) {
            scan_int();
            a += " num " + std::to_string(cur_val);
        } else if (scan_kw("name")) {
            a += " name{" + scan_pdf_text(true) + "}";
        }
        if (scan_kw("newwindow")) a += " newwindow";
        else if (scan_kw("nonewwindow")) a += " nonewwindow";
    } else if (scan_kw("thread")) {
        a = "thread";
        if (scan_kw("file")) a += " file{" + scan_pdf_text(true) + "}";
        if (scan_kw("num")) {
            scan_int();
            a += " num " + std::to_string(cur_val);
        } else if (scan_kw("name")) {
            a += " name{" + scan_pdf_text(true) + "}";
        }
    } else {
        print_err_str("Action type missing");
        set_help({"Allowed action types are `user', `goto', and `thread'."});
        error_();
    }
    d.text = a;
}

void Engine::do_pdf_extension(int32_t chr) {
    using namespace tc;
    switch (chr) {
    case pdf_literal_node: {
        const int32_t p = new_pdf_node(pdf_literal_node);
        PdfNodeData &d = pdf_node(p);
        if (scan_kw("direct")) d.mode = 2;
        else if (scan_kw("page")) d.mode = 1;
        else if (scan_kw("raw")) d.mode = 3;
        d.shipout = scan_kw("shipout");
        link(p + 5) = scan_toks_list(!d.shipout);
        return;
    }
    case pdf_colorstack_node: {
        scan_int();
        const int32_t stack = cur_val;
        int32_t cmd = -1;
        if (scan_kw("set")) cmd = 0;
        else if (scan_kw("push")) cmd = 1;
        else if (scan_kw("pop")) cmd = 2;
        else if (scan_kw("current")) cmd = 3;
        if (stack < 0 || stack >= static_cast<int32_t>(pdf_.colorstacks.size())) {
            print_err_str("Unknown color stack number ");
            print_int(stack);
            set_help({"Allocate and initialize a color stack with \\pdfcolorstackinit.",
                      "I'll use the default color stack 0 here."});
            error_();
        }
        if (cmd < 0) {
            print_err_str("Color stack action is missing");
            set_help({"The expected actions for \\pdfcolorstack:", "    set, push, pop, current"});
            back_error();
            return;
        }
        const int32_t p = new_pdf_node(pdf_colorstack_node);
        PdfNodeData &d = pdf_node(p);
        d.stack = stack;
        d.mode = cmd;
        if (cmd <= 1) {
            link(p + 5) = scan_toks_list(true);
        }
        return;
    }
    case pdf_setmatrix_node: {
        const int32_t p = new_pdf_node(pdf_setmatrix_node);
        link(p + 5) = scan_toks_list(true);
        return;
    }
    case pdf_save_node:
    case pdf_restore_node:
    case pdf_save_pos_node:
    case pdf_end_link_node:
    case pdf_end_thread_node:
    case pdf_snap_ref_point_node:
        new_pdf_node(chr);
        return;
    case pdf_snapy_node:
    case pdf_snapy_comp_node: {
        const int32_t p = new_pdf_node(chr);
        if (chr == pdf_snapy_node) {
            scan_glue(glue_val);
            delete_glue_ref(cur_val);
        } else {
            scan_int();
        }
        (void)p;
        return;
    }
    case pdf_refobj_node: {
        scan_int();
        const int32_t n = cur_val;
        const int32_t p = new_pdf_node(pdf_refobj_node);
        pdf_node(p).obj = n;
        if (PdfObject *o = pdf_obj(n)) o->referenced = true;
        return;
    }
    case pdf_refxform_node:
    case pdf_refximage_node: {
        scan_int();
        const int32_t n = cur_val;
        PdfObject *o = pdf_obj(n);
        const auto want = chr == pdf_refxform_node ? PdfObject::Xform : PdfObject::Ximage;
        if (!o || o->kind != want) {
            print_err_str(chr == pdf_refxform_node ? "Invalid form object" : "Invalid image object");
            set_help({"I'm ignoring this reference."});
            error_();
            return;
        }
        const int32_t p = new_pdf_node(chr);
        pdf_node(p).obj = n;
        width(p) = o->width;
        height(p) = o->height;
        depth(p) = o->depth;
        o->referenced = true;
        return;
    }
    case pdf_annot_node: {
        int32_t obj = 0;
        if (scan_kw("reserveobjnum")) {
            pdf_.last_annot = pdf_new_obj(PdfObject::Annot);
            pdf_obj(pdf_.last_annot)->reserved = true;
            return;
        }
        if (scan_kw("useobjnum")) {
            scan_int();
            obj = cur_val;
        }
        const int32_t p = new_pdf_node(pdf_annot_node);
        int32_t w = null_flag, h = null_flag, dd = null_flag;
        scan_pdf_rule_spec(w, h, dd);
        width(p) = w;
        height(p) = h;
        depth(p) = dd;
        if (obj == 0) obj = pdf_new_obj(PdfObject::Annot);
        pdf_node(p).obj = obj;
        pdf_.last_annot = obj;
        link(p + 5) = scan_toks_list(true);
        return;
    }
    case pdf_start_link_node: {
        if (std::abs(mode()) == vmode) {
            print_err_str("\\pdfstartlink cannot be used in vertical mode");
            set_help({"I'm ignoring this link."});
            error_();
            return;
        }
        const int32_t p = new_pdf_node(pdf_start_link_node);
        int32_t w = null_flag, h = null_flag, dd = null_flag;
        scan_pdf_rule_spec(w, h, dd);
        width(p) = w;
        height(p) = h;
        depth(p) = dd;
        if (scan_kw("attr")) {
            link(p + 5) = scan_toks_list(true);
        }
        scan_pdf_action(pdf_node(p));
        const int32_t obj = pdf_new_obj(PdfObject::Link);
        pdf_node(p).obj = obj;
        pdf_.last_link = obj;
        return;
    }
    case pdf_dest_node: {
        const int32_t p = new_pdf_node(pdf_dest_node);
        PdfNodeData &d = pdf_node(p);
        if (scan_kw("num")) {
            scan_int();
            d.num = cur_val;
        } else if (scan_kw("name")) {
            d.named = true;
            d.text = scan_pdf_text(true);
        } else {
            print_err_str("Destination type missing");
            set_help({"A destination needs `num' or `name'."});
            error_();
        }
        if (scan_kw("xyz")) {
            d.dest_type = 0;
            if (scan_kw("zoom")) {
                scan_int();
                d.zoom = cur_val;
            }
        } else if (scan_kw("fitbh")) {
            d.dest_type = 5;
        } else if (scan_kw("fitbv")) {
            d.dest_type = 6;
        } else if (scan_kw("fitb")) {
            d.dest_type = 4;
        } else if (scan_kw("fith")) {
            d.dest_type = 2;
        } else if (scan_kw("fitv")) {
            d.dest_type = 3;
        } else if (scan_kw("fitr")) {
            d.dest_type = 7;
            int32_t w = null_flag, h = null_flag, dd = null_flag;
            scan_pdf_rule_spec(w, h, dd);
            width(p) = w;
            height(p) = h;
            depth(p) = dd;
        } else if (scan_kw("fit")) {
            d.dest_type = 1;
        } else {
            print_err_str("Destination type missing");
            set_help({"I'm going to use `xyz'."});
            error_();
        }
        d.obj = pdf_new_obj(PdfObject::Dest);
        return;
    }
    case pdf_thread_node:
    case pdf_start_thread_node: {
        const int32_t p = new_pdf_node(chr);
        int32_t w = null_flag, h = null_flag, dd = null_flag;
        scan_pdf_rule_spec(w, h, dd);
        width(p) = w;
        height(p) = h;
        depth(p) = dd;
        if (scan_kw("attr")) {
            link(p + 5) = scan_toks_list(true);
        }
        PdfNodeData &d = pdf_node(p);
        if (scan_kw("num")) {
            scan_int();
            d.num = cur_val;
        } else if (scan_kw("name")) {
            d.named = true;
            d.text = scan_pdf_text(true);
        }
        return;
    }
    case pdf_obj_code: pdf_scan_obj(false); return;
    case pdf_xform_code: pdf_scan_xform(false); return;
    case pdf_ximage_code: pdf_scan_ximage(false); return;
    case pdf_outline_code: {
        PdfOutline o;
        if (scan_kw("attr")) o.attr = scan_pdf_text(true);
        PdfNodeData d;
        scan_pdf_action(d);
        o.action = d.text;
        if (scan_kw("count")) {
            scan_int();
            o.count = cur_val;
        }
        o.title = scan_pdf_text(true);
        o.obj = pdf_new_obj(PdfObject::Outline);
        pdf_.outlines.push_back(o);
        return;
    }
    case pdf_info_code: pdf_.info += scan_pdf_text(true) + " "; return;
    case pdf_catalog_code:
        pdf_.catalog += scan_pdf_text(true) + " ";
        if (scan_kw("openaction")) {
            PdfNodeData d;
            scan_pdf_action(d);
            pdf_.catalog_openaction = d.text;
        }
        return;
    case pdf_names_code: pdf_.names += scan_pdf_text(true) + " "; return;
    case pdf_trailer_code: pdf_.trailer += scan_pdf_text(true) + " "; return;
    case pdf_trailer_id_code: pdf_.trailer_id = scan_pdf_text(true); return;
    case pdf_map_file_code: pdf_.map_lines.push_back("file:" + scan_pdf_text(true)); return;
    case pdf_map_line_code: pdf_.map_lines.push_back("line:" + scan_pdf_text(true)); return;
    case pdf_font_attr_code: {
        scan_font_ident();
        const int32_t fnt = cur_val;
        pdf_.font_attrs[fnt] = scan_pdf_text(true);
        return;
    }
    case pdf_font_expand_code: {
        scan_font_ident();
        scan_int();
        scan_int();
        scan_int();
        scan_kw("autoexpand");
        return;
    }
    case pdf_include_chars_code:
        scan_font_ident();
        scan_pdf_text(true);
        return;
    case pdf_glyph_to_unicode_code: {
        const std::string glyph = scan_pdf_text(true);
        const std::string uni = scan_pdf_text(true);
        pdf_.glyph_to_unicode[glyph] = uni;
        return;
    }
    case pdf_nobuiltin_tounicode_code:
    case pdf_no_ligatures_code:
        scan_font_ident();
        return;
    case pdf_interword_space_on_code:
    case pdf_interword_space_off_code:
    case pdf_fake_space_code:
    case pdf_running_link_off_code:
    case pdf_running_link_on_code:
        return;
    case pdf_space_font_code:
        scan_font_ident();
        return;
    case pdf_reset_timer_code: pdf_.timer_start = pdf_clock(); return;
    case pdf_set_random_seed_code:
        scan_int();
        pdf_.random_seed = static_cast<uint64_t>(static_cast<uint32_t>(cur_val));
        pdf_.random_state = pdf_.random_seed;
        pdf_.random_init = true;
        return;
    case quitvmode_code:
        if (std::abs(mode()) == vmode) new_graf(true);
        return;
    case partokenname_code: {
        get_r_token();
        par_loc = cur_cs;
        par_token = cs_token_flag + par_loc;
        return;
    }
    case letterspace_font_code:
    case pdf_copy_font_code:
        print_err_str("Sorry, this pdfTeX font command is not implemented yet");
        set_help({"mep-tex does not make letterspaced or copied fonts yet."});
        error_();
        return;
    default:
        confusion(pool_string("ext1"));
        return;
    }
}

void Engine::pdf_scan_obj(bool immediate) {
    if (scan_kw("reserveobjnum")) {
        const int32_t n = pdf_new_obj(PdfObject::Raw);
        pdf_obj(n)->reserved = true;
        pdf_.last_obj = n;
        return;
    }
    int32_t n = 0;
    if (scan_kw("useobjnum")) {
        scan_int();
        n = cur_val;
        if (!pdf_obj(n)) n = 0;
    }
    if (n == 0) n = pdf_new_obj(PdfObject::Raw);
    PdfObject &o = *pdf_obj(n);
    o.reserved = false;
    o.immediate = immediate;
    if (scan_kw("stream")) {
        o.kind = PdfObject::Stream;
        if (scan_kw("attr")) o.attr = scan_pdf_text(true);
    }
    o.from_file = scan_kw("file");
    o.content = scan_pdf_text(true);
    pdf_.last_obj = n;
    if (immediate) pdf_write_obj(n);
}

void Engine::pdf_scan_xform(bool immediate) {
    using namespace tc;
    std::string attr, resources;
    if (scan_kw("attr")) attr = scan_pdf_text(true);
    if (scan_kw("resources")) resources = scan_pdf_text(true);
    scan_eight_bit_int();
    const int32_t b = equiv(box_base + cur_val);
    if (b == null) {
        print_err_str("\\pdfxform cannot be used with a void box");
        set_help({"I'm ignoring it."});
        error_();
        return;
    }
    const int32_t n = pdf_new_obj(PdfObject::Xform);
    PdfObject &o = *pdf_obj(n);
    o.attr = attr;
    o.resources = resources;
    o.box = b;
    o.width = width(b);
    o.height = height(b);
    o.depth = depth(b);
    o.immediate = immediate;
    equiv(box_base + cur_val) = null;
    pdf_.last_xform = n;
    if (immediate) pdf_write_obj(n);
}

void Engine::pdf_scan_ximage(bool immediate) {
    using namespace tc;
    int32_t w = null_flag, h = null_flag, d = null_flag;
    scan_pdf_rule_spec(w, h, d);
    std::string attr;
    if (scan_kw("attr")) attr = scan_pdf_text(true);
    int32_t page = 1;
    std::string named;
    if (scan_kw("named")) {
        named = scan_pdf_text(true);
    } else if (scan_kw("page")) {
        scan_int();
        page = cur_val;
    }
    if (scan_kw("colorspace")) scan_int();
    for (const char *box : {"mediabox", "cropbox", "bleedbox", "trimbox", "artbox"}) {
        if (scan_kw(box)) break;
    }
    const std::string name = scan_pdf_text(true);
    const int32_t n = pdf_new_obj(PdfObject::Ximage);
    PdfObject &o = *pdf_obj(n);
    o.attr = attr;
    o.page = page;
    o.immediate = immediate;
    o.file = find_input_file(name);
    if (o.file.empty()) o.file = name;
    pdf_read_image_size(o);  // sets o.width/height/depth from the file, pages, bbox
    // explicit dimensions: the missing ones keep the image's aspect ratio
    const int32_t iw = o.width, ih = o.height;
    if (w != null_flag || h != null_flag || d != null_flag) {
        const int32_t hd_given = (h != null_flag ? h : 0) + (d != null_flag ? d : 0);
        if (w == null_flag && (h != null_flag || d != null_flag) && ih != 0) {
            w = static_cast<int32_t>(static_cast<double>(iw) * hd_given / ih);
        } else if (w != null_flag && h == null_flag && d == null_flag && iw != 0) {
            h = static_cast<int32_t>(static_cast<double>(ih) * w / iw);
        }
        o.width = w != null_flag ? w : iw;
        o.height = h != null_flag ? h : (d != null_flag ? 0 : ih);
        o.depth = d != null_flag ? d : 0;
    }
    pdf_.last_ximage = n;
    pdf_.last_ximage_pages = o.pages;
    pdf_.last_ximage_colordepth = o.colordepth;
    if (immediate) pdf_write_obj(n);
}

bool Engine::pdf_immediate_ok(int32_t chr) {
    return chr == tc::pdf_obj_code || chr == tc::pdf_xform_code || chr == tc::pdf_ximage_code ||
           chr == tc::pdf_annot_node || chr == tc::pdf_literal_node;
}

void Engine::pdf_immediate(int32_t chr) {
    using namespace tc;
    if (chr == pdf_obj_code) {
        pdf_scan_obj(true);
    } else if (chr == pdf_xform_code) {
        pdf_scan_xform(true);
    } else if (chr == pdf_ximage_code) {
        pdf_scan_ximage(true);
    } else {
        // \immediate\pdfannot / \immediate\pdfliteral: made and dropped
        const int32_t p = tail();
        do_pdf_extension(chr);
        if (tail() != p) {
            flush_node_list(tail());
            tail() = p;
            link(p) = null;
        }
    }
}

// --- Whatsit maintenance --------------------------------------------------------------------------

void Engine::display_pdf_whatsit(int32_t p) {
    using namespace tc;
    const PdfNodeData &d = pdf_node(p);
    auto dims = [&]() {
        print_char('(');
        print_rule_dimen(height(p));
        print_char('+');
        print_rule_dimen(depth(p));
        print_str(")x");
        print_rule_dimen(width(p));
    };
    switch (subtype(p)) {
    case pdf_literal_node:
        print_esc_str("pdfliteral");
        if (d.mode == 1) print_str(" page");
        else if (d.mode == 2) print_str(" direct");
        else if (d.mode == 3) print_str(" raw");
        if (d.shipout) print_str(" shipout");
        print_mark(link(p + 5));
        break;
    case pdf_colorstack_node:
        print_esc_str("pdfcolorstack ");
        print_int(d.stack);
        print_str(d.mode == 0 ? " set " : d.mode == 1 ? " push " : d.mode == 2 ? " pop" : " current");
        if (d.mode <= 1) print_mark(link(p + 5));
        break;
    case pdf_setmatrix_node:
        print_esc_str("pdfsetmatrix");
        print_mark(link(p + 5));
        break;
    case pdf_save_node: print_esc_str("pdfsave"); break;
    case pdf_restore_node: print_esc_str("pdfrestore"); break;
    case pdf_save_pos_node: print_esc_str("pdfsavepos"); break;
    case pdf_end_link_node: print_esc_str("pdfendlink"); break;
    case pdf_end_thread_node: print_esc_str("pdfendthread"); break;
    case pdf_snap_ref_point_node: print_esc_str("pdfsnaprefpoint"); break;
    case pdf_snapy_node: print_esc_str("pdfsnapy"); break;
    case pdf_snapy_comp_node: print_esc_str("pdfsnapycomp"); break;
    case pdf_refobj_node:
        print_esc_str("pdfrefobj");
        print_int(d.obj);
        break;
    case pdf_refxform_node:
    case pdf_refximage_node:
        print_esc_str(subtype(p) == pdf_refxform_node ? "pdfrefxform" : "pdfrefximage");
        dims();
        break;
    case pdf_annot_node:
        print_esc_str("pdfannot");
        dims();
        print_mark(link(p + 5));
        break;
    case pdf_start_link_node:
        print_esc_str("pdfstartlink");
        dims();
        if (link(p + 5) != null) {
            print_str(" attr");
            print_mark(link(p + 5));
        }
        print_str(" action ");
        print_str(d.text.c_str());
        break;
    case pdf_dest_node:
        print_esc_str("pdfdest");
        if (d.named) {
            print_str(" name{");
            print_str(d.text.c_str());
            print_char('}');
        } else {
            print_str(" num");
            print_int(d.num);
        }
        break;
    case pdf_thread_node:
    case pdf_start_thread_node:
        print_esc_str(subtype(p) == pdf_thread_node ? "pdfthread" : "pdfstartthread");
        dims();
        break;
    default:
        print_str("whatsit?");
        break;
    }
}

int32_t Engine::copy_pdf_whatsit(int32_t p, int32_t &words) {
    const int32_t r = get_node(kPdfNodeSize);
    for (int32_t k = 1; k < kPdfNodeSize; k++) mem[r + k] = mem[p + k];
    // its own copy of the data, and another reference to the token list
    int32_t k;
    if (!pdf_.free_nodes.empty()) {
        k = pdf_.free_nodes.back();
        pdf_.free_nodes.pop_back();
    } else {
        k = static_cast<int32_t>(pdf_.nodes.size());
        pdf_.nodes.emplace_back();
    }
    pdf_.nodes[static_cast<size_t>(k)] = pdf_.nodes[static_cast<size_t>(mem[p + 4].int_)];
    mem[r + 4].int_ = k;
    if (link(p + 5) != tc::null) token_ref_count(link(p + 5)) += 1;
    words = 1;  // copy_node_list copies word 0 (type, subtype)
    return r;
}

void Engine::free_pdf_whatsit(int32_t p) {
    if (link(p + 5) != tc::null) delete_token_ref(link(p + 5));
    const int32_t k = mem[p + 4].int_;
    if (k >= 0 && k < static_cast<int32_t>(pdf_.nodes.size()) && pdf_.nodes[static_cast<size_t>(k)].used) {
        pdf_.nodes[static_cast<size_t>(k)].used = false;
        pdf_.free_nodes.push_back(k);
    }
    free_node(p, kPdfNodeSize);
}

void Engine::pdf_out_what_dvi(int32_t p) {
    // In DVI mode pdfTeX's nodes have no effect, except that a saved
    // position is still recorded.
    if (subtype(p) == tc::pdf_save_pos_node) {
        pdf_.last_x_pos = cur_h + 4736287;
        pdf_.last_y_pos = cur_page_height_dvi() - cur_v - 4736287;
    }
}

int32_t Engine::cur_page_height_dvi() {
    return dimen_par(tc::pdf_page_height_code) != 0 ? dimen_par(tc::pdf_page_height_code) : 0;
}

// --- The format ----------------------------------------------------------------------------------
//
// What pdfTeX's primitives keep outside TeX's memory, when a format sets
// it up -- LaTeX's format loads glyphtounicode.tex, thousands of
// \pdfglyphtounicode -- goes into the format after TeX's own tables:
// strings as a length and then their bytes, four to a word.

namespace {
constexpr int32_t kPdfStateMagic = 0x70644631;  // "pdF1"
}

void Engine::pdf_dump_state() {
    auto put_int = [&](int32_t v) {
        fmt_file.buf().int_ = v;
        fmt_file.put();
    };
    auto put_str = [&](const std::string &str) {
        put_int(static_cast<int32_t>(str.size()));
        for (size_t k = 0; k < str.size(); k += 4) {
            uint32_t w = 0;
            for (size_t j = 0; j < 4 && k + j < str.size(); j++)
                w |= static_cast<uint32_t>(static_cast<unsigned char>(str[k + j])) << (8 * j);
            put_int(static_cast<int32_t>(w));
        }
    };
    put_int(kPdfStateMagic);
    std::map<std::string, std::string> glyphs(pdf_.glyph_to_unicode.begin(), pdf_.glyph_to_unicode.end());
    put_int(static_cast<int32_t>(glyphs.size()));
    for (const auto &[name, uni] : glyphs) {
        put_str(name);
        put_str(uni);
    }
    put_int(static_cast<int32_t>(pdf_.map_lines.size()));
    for (const std::string &l : pdf_.map_lines) put_str(l);
    put_int(static_cast<int32_t>(pdf_.colorstacks.size()));
    for (const PdfColorStack &cs : pdf_.colorstacks) {
        put_int((cs.page_start ? 1 : 0) | (cs.direct ? 2 : 0));
        put_str(cs.initial);
    }
    std::map<int64_t, int32_t> codes(pdf_.font_char_codes.begin(), pdf_.font_char_codes.end());
    put_int(static_cast<int32_t>(codes.size()));
    for (const auto &[key, v] : codes) {
        put_int(static_cast<int32_t>(key >> 32));
        put_int(static_cast<int32_t>(key & 0xffffffff));
        put_int(v);
    }
    put_int(kPdfStateMagic);
}

bool Engine::pdf_undump_state() {
    bool ok = true;
    auto get_int = [&]() -> int32_t {
        if (fmt_file.eof()) {
            ok = false;
            return 0;
        }
        fmt_file.get();
        return fmt_file.buf().int_;
    };
    auto get_str = [&]() -> std::string {
        const int32_t n = get_int();
        if (n < 0 || n > (1 << 24)) {
            ok = false;
            return "";
        }
        std::string str;
        str.reserve(static_cast<size_t>(n));
        for (int32_t k = 0; k < n && ok; k += 4) {
            const auto w = static_cast<uint32_t>(get_int());
            for (int32_t j = 0; j < 4 && k + j < n; j++) str.push_back(static_cast<char>((w >> (8 * j)) & 0xff));
        }
        return str;
    };
    auto count = [&]() {
        const int32_t n = get_int();
        if (n < 0 || n > (1 << 24)) ok = false;
        return ok ? n : 0;
    };
    if (get_int() != kPdfStateMagic) return false;
    pdf_.glyph_to_unicode.clear();
    for (int32_t k = count(); k > 0 && ok; k--) {
        std::string name = get_str();
        pdf_.glyph_to_unicode[name] = get_str();
    }
    pdf_.map_lines.clear();
    for (int32_t k = count(); k > 0 && ok; k--) pdf_.map_lines.push_back(get_str());
    pdf_.colorstacks.clear();
    for (int32_t k = count(); k > 0 && ok; k--) {
        PdfColorStack cs;
        const int32_t flags = get_int();
        cs.page_start = (flags & 1) != 0;
        cs.direct = (flags & 2) != 0;
        cs.initial = get_str();
        pdf_.colorstacks.push_back(cs);
    }
    pdf_.font_char_codes.clear();
    for (int32_t k = count(); k > 0 && ok; k--) {
        const auto hi = static_cast<uint32_t>(get_int());
        const auto lo = static_cast<uint32_t>(get_int());
        pdf_.font_char_codes[static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo)] = get_int();
    }
    return ok && get_int() == kPdfStateMagic;
}

void Engine::print_quoted_job_name() {
    const std::string name = pool_to_string(job_name);
    if (name.find(' ') == std::string::npos) {
        print(job_name);
        return;
    }
    print_char('"');
    print(job_name);
    print_char('"');
}

void Engine::log_openout(int32_t j) {
    if (!log_opened) return;  // the first line's \openout: no log yet
    const int32_t saved = selector;
    selector = int_par(tc::tracing_online_code) <= 0 ? tc::log_only : tc::term_and_log;
    print_nl_str("\\openout");
    print_int(j);
    print_str(" = `");
    print_file_name(cur_name, cur_area, cur_ext);
    print_str("'.");
    print_nl_str("");
    print_ln();
    selector = saved;
}

// --- \input{...} -----------------------------------------------------------------------------------

void Engine::scan_braced_file_name() {
    back_input();
    const int32_t saved = scanner_status;
    scanner_status = tc::normal;
    const std::string name = scan_pdf_text(true);
    scanner_status = saved;
    quoted_filename = true;  // spaces belong to a braced name
    for (char ch : name) {
        if (ch == '"') continue;
        more_name(static_cast<uint8_t>(ch));
    }
    quoted_filename = false;
}

}  // namespace meptex
