// The PDF file mep-tex writes when \pdfoutput>0: a page for each shipped
// box (text, rules, literals, colour stacks, forms, annotations, links,
// destinations), Type 1 fonts found through the map file (pdftex.map) and
// embedded as subsets, outlines, and the objects pdfTeX's primitives made
// (tex_pdf.cpp). The whole file is built in memory and written when the
// job ends. Written from the pdfTeX manual and the PDF reference.

#include "tex/gen/tex_gen.h"
#include "tex/tex_resolver.h"

#include "deflate.h"
#include "gfx/type1.h"
#include "jpeg_codec.h"
#include "pdf_crypt.h"
#include "pdf_document.h"
#include "pdf_filters.h"
#include "pdf_object.h"
#include "pdf_xref.h"
#include "png_codec.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace meptex {

namespace {

// Scaled points per big point (72.27/72 * 65536).
constexpr double kSpPerBp = 65781.76;

// A length in sp as PDF units (big points), to 1/1000 bp.
int64_t Milli(double sp) { return std::llround(sp / kSpPerBp * 1000.0); }

std::string Num(int64_t milli) {
    std::string s;
    if (milli < 0) {
        s = "-";
        milli = -milli;
    }
    s += std::to_string(milli / 1000);
    int64_t frac = milli % 1000;
    if (frac != 0) {
        char buf[8];
        std::snprintf(buf, sizeof buf, ".%03d", static_cast<int>(frac));
        std::string fnt = buf;
        while (fnt.back() == '0') fnt.pop_back();
        s += fnt;
    }
    return s;
}

std::string NumD(double v) { return Num(std::llround(v * 1000.0)); }

std::string ReadFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool EndsWith(const std::string &s, const std::string &t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// --- Type 1 font files --------------------------------------------------------------------

// A Type 1 font split the way /FontFile wants it: the cleartext part and
// the (still encrypted) binary part.
struct Type1Parts {
    std::string clear, binary;
};

bool SplitType1(const std::string &data, Type1Parts &parts) {
    if (!data.empty() && static_cast<unsigned char>(data[0]) == 0x80) {
        // PFB: segments of ASCII (1) and binary (2) data, then 3 = end
        size_t i = 0;
        bool seen_binary = false;
        while (i + 6 <= data.size() && static_cast<unsigned char>(data[i]) == 0x80) {
            const int type = static_cast<unsigned char>(data[i + 1]);
            if (type == 3) break;
            const uint32_t len = static_cast<uint32_t>(static_cast<unsigned char>(data[i + 2])) |
                                 static_cast<uint32_t>(static_cast<unsigned char>(data[i + 3])) << 8 |
                                 static_cast<uint32_t>(static_cast<unsigned char>(data[i + 4])) << 16 |
                                 static_cast<uint32_t>(static_cast<unsigned char>(data[i + 5])) << 24;
            i += 6;
            if (i + len > data.size()) return false;
            if (type == 1 && !seen_binary) parts.clear.append(data, i, len);
            else if (type == 2) {
                parts.binary.append(data, i, len);
                seen_binary = true;
            }
            i += len;
        }
        return !parts.clear.empty() && !parts.binary.empty();
    }
    // PFA: cleartext up to `eexec`, then hex
    const size_t e = data.find("eexec");
    if (e == std::string::npos) return false;
    size_t i = e + 5;
    while (i < data.size() && (data[i] == '\r' || data[i] == '\n' || data[i] == ' ' || data[i] == '\t')) i++;
    parts.clear = data.substr(0, i);
    int nib = -1;
    int zeros = 0;
    for (; i < data.size(); i++) {
        const char ch = data[i];
        int v;
        if (ch >= '0' && ch <= '9') v = ch - '0';
        else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
        else if (ch == 'c') break;  // cleartomark
        else continue;
        zeros = v == 0 ? zeros + 1 : 0;
        if (zeros > 64) break;  // the trailer's zeros
        if (nib < 0) {
            nib = v;
        } else {
            parts.binary.push_back(static_cast<char>(nib * 16 + v));
            nib = -1;
        }
    }
    return !parts.binary.empty();
}

std::string Eexec(const std::string &in, bool encrypt) {
    uint16_t r = 55665;
    std::string out(in.size(), '\0');
    for (size_t k = 0; k < in.size(); k++) {
        const auto pc = static_cast<uint8_t>(in[k]);
        uint8_t p;
        if (encrypt) {
            p = static_cast<uint8_t>(pc ^ (r >> 8));
            r = static_cast<uint16_t>((p + r) * 52845u + 22719u);
        } else {
            p = static_cast<uint8_t>(pc ^ (r >> 8));
            r = static_cast<uint16_t>((pc + r) * 52845u + 22719u);
        }
        out[k] = static_cast<char>(p);
    }
    return out;
}

// What charstring `cs` uses: the glyphs seac (accented character)
// composes it from, and the Subrs it calls (directly, or through
// OtherSubr 3, hint replacement, which hands its argument to callsubr).
void ScanCharstring(const std::string &cs, std::set<std::string> &names, std::set<int32_t> &subrs) {
    std::vector<int32_t> stack;
    for (size_t i = 0; i < cs.size();) {
        const int v = static_cast<unsigned char>(cs[i]);
        if (v >= 32) {
            int32_t n;
            if (v <= 246) {
                n = v - 139;
                i += 1;
            } else if (v <= 250) {
                if (i + 1 >= cs.size()) return;
                n = (v - 247) * 256 + static_cast<unsigned char>(cs[i + 1]) + 108;
                i += 2;
            } else if (v <= 254) {
                if (i + 1 >= cs.size()) return;
                n = -(v - 251) * 256 - static_cast<unsigned char>(cs[i + 1]) - 108;
                i += 2;
            } else {
                if (i + 4 >= cs.size()) return;
                n = static_cast<int32_t>(static_cast<uint32_t>(static_cast<unsigned char>(cs[i + 1])) << 24 |
                                         static_cast<uint32_t>(static_cast<unsigned char>(cs[i + 2])) << 16 |
                                         static_cast<uint32_t>(static_cast<unsigned char>(cs[i + 3])) << 8 |
                                         static_cast<uint32_t>(static_cast<unsigned char>(cs[i + 4])));
                i += 5;
            }
            stack.push_back(n);
        } else if (v == 12) {
            const int op = i + 1 < cs.size() ? static_cast<unsigned char>(cs[i + 1]) : -1;
            if (op == 6 && stack.size() >= 2) {  // seac
                for (size_t k = stack.size() - 2; k < stack.size(); k++) {
                    if (const char *nm = gfx::t1::StandardEncodingName(stack[k])) names.insert(nm);
                }
            } else if (op == 16 && stack.size() >= 3 && stack.back() == 3) {  // callothersubr 3
                subrs.insert(stack[stack.size() - 3]);
            }
            if (op != 17) stack.clear();  // pop leaves a value for callsubr
            i += 2;
        } else if (v == 10) {  // callsubr
            if (!stack.empty()) subrs.insert(stack.back());
            stack.clear();
            i += 1;
        } else {
            stack.clear();
            i += 1;
        }
    }
}

std::string CharstringCrypt(const std::string &in, bool encrypt) {
    uint16_t r = 4330;
    std::string out(in.size(), '\0');
    for (size_t k = 0; k < in.size(); k++) {
        const auto c = static_cast<uint8_t>(in[k]);
        const auto p = static_cast<uint8_t>(c ^ (r >> 8));
        r = static_cast<uint16_t>(((encrypt ? p : c) + r) * 52845u + 22719u);
        out[k] = static_cast<char>(p);
    }
    return out;
}

// Rewrites the binary entries of one section of the private part
// (`/Subrs` or `/CharStrings`), from `from`: keep(entry) says which stay,
// and a dropped Subr becomes a bare `return` so the numbering holds.
// The font's private part with only the CharStrings named in `keep` and
// the Subrs numbered in `subrs` (the others become a bare `return`).
std::string SubsetType1Private(const std::string &plain, const std::set<std::string> &keep,
                               const std::set<int32_t> &subrs, int len_iv) {
    size_t i = 0;
    auto skip_space = [&]() {
        while (i < plain.size() && (plain[i] == ' ' || plain[i] == '\n' || plain[i] == '\r' || plain[i] == '\t')) i++;
    };
    auto number = [&]() {
        size_t n = 0;
        while (i < plain.size() && plain[i] >= '0' && plain[i] <= '9') n = n * 10 + static_cast<size_t>(plain[i++] - '0');
        return n;
    };
    // an entry's binary data: `len RD <bytes>`, leaving i after the bytes
    struct Bin {
        size_t len_at, rd_at, data_at, len;
    };
    auto binary = [&]() {
        Bin b{};
        skip_space();
        b.len_at = i;
        b.len = number();
        skip_space();
        b.rd_at = i;
        while (i < plain.size() && plain[i] != ' ') i++;  // RD or -|
        b.data_at = i + 1;
        i = b.data_at + b.len;
        return b;
    };
    auto to_eol = [&]() {
        while (i < plain.size() && plain[i] != '\n' && plain[i] != '\r') i++;
    };
    std::string out;
    // Subrs: `dup <n> <len> RD <bytes> NP`, count times
    const size_t sb = plain.find("/Subrs");
    size_t copied = 0;
    if (sb != std::string::npos) {
        i = sb + 6;
        skip_space();
        const size_t count = number();
        const size_t arr = plain.find("array", i);
        if (arr != std::string::npos) {
            i = arr + 5;
            out.append(plain, 0, i);
            std::string ret = std::string(static_cast<size_t>(std::max(len_iv, 0)), '\0') + "\x0b";
            if (len_iv >= 0) ret = CharstringCrypt(ret, true);
            for (size_t k = 0; k < count; k++) {
                const size_t entry = i;
                skip_space();
                if (plain.compare(i, 3, "dup") != 0) {
                    i = entry;
                    break;
                }
                i += 3;
                skip_space();
                const size_t n = number();
                const Bin b = binary();
                if (i > plain.size()) return plain;
                const size_t after = i;
                to_eol();
                if (n < 4 || subrs.count(static_cast<int32_t>(n))) {
                    out.append(plain, entry, i - entry);
                } else {
                    const std::string rd = plain.substr(b.rd_at, b.data_at - 1 - b.rd_at);
                    out.append(plain, entry, b.len_at - entry);
                    out += std::to_string(ret.size()) + " " + rd + " " + ret;
                    out.append(plain, after, i - after);
                }
            }
            copied = i;
        }
    }
    // CharStrings: `/<name> <len> RD <bytes> ND` until `end`
    const size_t cs = plain.find("/CharStrings", copied);
    if (cs == std::string::npos) return out + plain.substr(copied);
    out.append(plain, copied, cs - copied);
    i = cs + 12;
    skip_space();
    const size_t count_at = i;
    number();
    const size_t count_end = i;
    const size_t begin = plain.find("begin", i);
    if (begin == std::string::npos || count_end == count_at) return out + plain.substr(cs);
    i = begin + 5;
    const size_t entries_from = i;
    std::string kept;
    int n = 0;
    for (;;) {
        const size_t entry = i;
        skip_space();
        if (i >= plain.size() || plain[i] != '/') {
            // `end`: the rest of the font follows unchanged
            return out + plain.substr(cs, count_at - cs) + std::to_string(n) +
                   plain.substr(count_end, entries_from - count_end) + kept + plain.substr(entry);
        }
        size_t j = i + 1;
        while (j < plain.size() && !std::strchr(" \t\r\n/[]{}()<>%", plain[j])) j++;
        const std::string name = plain.substr(i + 1, j - i - 1);
        i = j;
        binary();
        if (i > plain.size()) return plain;
        to_eol();
        if (keep.count(name)) {
            kept.append(plain, entry, i - entry);
            n++;
        }
    }
}

std::vector<std::string> ReadEncoding(const std::string &text) {
    std::vector<std::string> names;
    size_t i = text.find('[');
    if (i == std::string::npos) return names;
    for (i++; i < text.size() && names.size() < 256;) {
        const char ch = text[i];
        if (ch == '%') {
            while (i < text.size() && text[i] != '\n') i++;
        } else if (ch == ']') {
            break;
        } else if (ch == '/') {
            size_t j = i + 1;
            while (j < text.size() && !std::strchr(" \t\r\n/[]{}()<>%", text[j])) j++;
            names.push_back(text.substr(i + 1, j - i - 1));
            i = j;
        } else {
            i++;
        }
    }
    names.resize(256, ".notdef");
    return names;
}

std::string FindCleartext(const std::string &clear, const char *key) {
    const size_t k = clear.find(key);
    if (k == std::string::npos) return "";
    size_t i = k + std::strlen(key);
    const size_t e = clear.find("def", i);
    if (e == std::string::npos) return "";
    std::string v = clear.substr(i, e - i);
    for (char &ch : v) {
        if (ch == '{' || ch == '}' || ch == '[' || ch == ']') ch = ' ';
    }
    const size_t ro = v.find("readonly");
    if (ro != std::string::npos) v.erase(ro);
    return v;
}

}  // namespace

// --- Content streams ----------------------------------------------------------------------------

// The state of one content stream being made: a page, or a form.
struct PdfContent {
    std::string s;
    int32_t page_h = 0;          // its height in sp: y = page_h - v
    bool in_text = false;        // inside BT ... ET
    bool in_array = false;       // inside [ ... ]TJ
    int32_t font = -1;           // the TeX font Tf selected
    int32_t pdf_font = -1;       // its PdfFont
    double size_sp = 0;          // its size
    double extend = 1, slant = 0;
    bool skewed = false;         // the text matrix set last was not a translation (Td would keep it)
    int64_t line_x = 0, line_y = 0;  // the text line's origin, in milli-bp
    double px = 0;               // where the PDF's text position is now, in sp along the line
    int32_t line_v = 0;          // the line's baseline (TeX's v)
    std::set<int32_t> fonts;     // PdfFont indices used
    std::set<int32_t> xforms, ximages;
    std::vector<int32_t> annots;
    std::string page_put;        // entries \special{pdf:put @thispage <<...>>} adds to the page
    // links begun by \pdfstartlink and not yet ended: a form's own, or
    // the pages' (where a link goes on from one page to the next)
    std::vector<PdfLink> own_links;
    std::vector<PdfLink> *links = &own_links;
    int32_t level = 0;  // nesting of the box being shipped

    PdfContent() = default;
    PdfContent(const PdfContent &) = delete;
    PdfContent &operator=(const PdfContent &) = delete;
};

namespace {

void EndArray(PdfContent &pc) {
    if (pc.in_array) {
        pc.s += ")]TJ\n";
        pc.in_array = false;
    }
}

void EndText(PdfContent &pc) {
    EndArray(pc);
    if (pc.in_text) {
        pc.s += "ET\n";
        pc.in_text = false;
    }
    pc.font = -1;
}

}  // namespace

int32_t Engine::pdf_font_index(int32_t fnt) {
    fnt = pdf_font_root(fnt);  // a copy is its font (scaled across, if expanded)
    const std::string tfm = pool_to_string(font_name[fnt]);
    auto it = pdf_.font_by_tfm.find(tfm);
    if (it != pdf_.font_by_tfm.end()) return it->second;
    PdfFont pf;
    pf.tfm = tfm;
    pf.obj = pdf_new_obj(PdfObject::Font);
    pf.res = static_cast<int32_t>(pdf_.fonts.size()) + 1;
    pf.tex_font = fnt;
    pdf_.fonts.push_back(pf);
    const int32_t k = static_cast<int32_t>(pdf_.fonts.size()) - 1;
    pdf_.font_by_tfm[tfm] = k;
    return k;
}

std::string Engine::pool_to_string(int32_t s) {
    std::string out;
    if (s < 0 || s >= str_ptr) return out;
    for (int32_t k = str_start[s]; k < str_start[s + 1]; k++) out.push_back(static_cast<char>(str_pool[k]));
    return out;
}

double Engine::pdf_char_width(int32_t pf, int32_t pc) {
    // the /Widths entry for c, in 1/1000 em, rounded as it is written
    const int32_t fnt = pdf_.fonts[static_cast<size_t>(pf)].tex_font;
    if (pc < font_bc[fnt] || pc > font_ec[fnt]) return 0;
    const four_quarters ci = char_info(fnt, pc);
    if (ci.b0 == 0) return 0;
    const double w = static_cast<double>(char_width(fnt, ci)) * 1000.0 / font_size[fnt];
    return static_cast<double>(std::llround(w * 10.0)) / 10.0;  // to 0.1, as pdfTeX writes them
}

void Engine::pdf_out_char(PdfContent &pc, int32_t fnt, int32_t ch, int32_t h, int32_t v) {
    h += font_letterspace(fnt) / 2;  // a letterspaced glyph sits in the middle of its widened box
    if (fnt != pc.font) {
        if (const int32_t vf = pdf_vf(fnt); vf >= 0) {
            pdf_out_vf_char(pc, vf, fnt, ch, h, v);
            return;
        }
        const int32_t pf = pdf_font_index(fnt);
        const PdfMapEntry *m = pdf_map_entry(pdf_.fonts[static_cast<size_t>(pf)].tfm);
        const double ext = (m ? m->extend : 1) * (1000 + font_expansion_of(fnt)) / 1000.0, sl = m ? m->slant : 0;
        const bool same = pc.in_text && pc.font >= 0 && pf == pc.pdf_font && ext == pc.extend && sl == pc.slant &&
                          font_size[fnt] == font_size[pc.font];
        if (same) {
            pc.font = fnt;  // a copy drawn just as its font is: nothing to change in the PDF
        } else {
            EndArray(pc);
            if (!pc.in_text || ext != pc.extend || sl != pc.slant) {
                // a fresh text matrix: the next character positions itself with Tm
                if (!pc.in_text) {
                    pc.s += "BT\n";
                    pc.line_x = pc.line_y = 0;  // BT starts from the identity matrix
                    pc.skewed = false;
                }
                pc.in_text = true;
                pc.extend = ext;
                pc.slant = sl;
                pc.line_v = v + 1;  // force positioning
            }
            pc.font = fnt;
            pc.pdf_font = pf;
            // the size in bp to 4 decimals, as pdfTeX writes it; positions
            // within the line are reckoned in the PDF's units, from that size
            const int64_t size4 = std::llround(font_size[fnt] / kSpPerBp * 10000.0);
            pc.size_sp = static_cast<double>(size4) / 10000.0 * kSpPerBp;
            pc.fonts.insert(pf);
            std::string size = std::to_string(size4 / 10000);
            if (size4 % 10000 != 0) {
                char buf[8];
                std::snprintf(buf, sizeof buf, ".%04d", static_cast<int>(size4 % 10000));
                size += buf;
                while (size.back() == '0') size.pop_back();
            }
            pc.s += "/F" + std::to_string(pdf_.fonts[static_cast<size_t>(pf)].res) + " " + size + " Tf\n";
        }
    }
    if (!pc.in_text) {
        pc.s += "BT\n";
        pc.line_x = pc.line_y = 0;
        pc.skewed = false;
        pc.in_text = true;
        pc.line_v = v + 1;
    }
    const double unit = pc.size_sp * pc.extend / 1000.0;  // sp per 1/1000 text unit
    const double delta = h - pc.px;
    if (v != pc.line_v || std::fabs(delta) > 100.0 * pc.size_sp) {
        EndArray(pc);
        const int64_t x = Milli(h), y = Milli(pc.page_h - v);
        if (pc.extend != 1 || pc.slant != 0 || pc.skewed) {
            pc.s += NumD(pc.extend) + " 0 " + NumD(pc.slant) + " 1 " + Num(x) + " " + Num(y) + " Tm\n";
            pc.skewed = pc.extend != 1 || pc.slant != 0;
        } else {
            pc.s += Num(x - pc.line_x) + " " + Num(y - pc.line_y) + " Td\n";
        }
        pc.line_x = x;
        pc.line_y = y;
        pc.line_v = v;
        pc.px = static_cast<double>(x) / 1000.0 * kSpPerBp;
    }
    if (!pc.in_array) {
        pc.s += "[(";
        pc.in_array = true;
    }
    const double d = h - pc.px;
    if (std::fabs(d) >= unit * 0.5) {
        const int64_t k = std::llround(-d / unit);
        pc.s += ")" + std::to_string(k) + "(";
        pc.px -= static_cast<double>(k) * unit;
    }
    const auto b = static_cast<unsigned char>(ch);
    if (b == '(' || b == ')' || b == '\\') {
        pc.s.push_back('\\');
        pc.s.push_back(static_cast<char>(b));
    } else if (b < 32 || b >= 127) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\%03o", b);
        pc.s += buf;
    } else {
        pc.s.push_back(static_cast<char>(b));
    }
    pdf_.fonts[static_cast<size_t>(pc.pdf_font)].used[b] = true;
    pc.px += pdf_char_width(pc.pdf_font, ch) * unit;
}

// --- Virtual fonts --------------------------------------------------------------------------------
//
// A font with a .vf file is virtual, as in pdfTeX (the VF is looked for
// before the map file): each character is drawn by its packet, a little
// DVI program setting characters of the VF's local fonts, rules and
// specials. The character's width is still its TFM's.

namespace {

uint32_t VfUnsigned(const std::string &d, size_t &i, int n) {
    uint32_t v = 0;
    for (int k = 0; k < n && i < d.size(); k++) v = (v << 8) | static_cast<unsigned char>(d[i++]);
    return v;
}

int32_t VfSigned(const std::string &d, size_t &i, int n) {
    const uint32_t v = VfUnsigned(d, i, n);
    if (n < 4 && (v & (1u << (8 * n - 1)))) return static_cast<int32_t>(v) - (1 << (8 * n));
    return static_cast<int32_t>(v);
}

// a fix_word times a size, rounded as TeX scales TFM dimensions (§571-572)
int32_t VfScaled(int32_t fw, int32_t size) {
    int32_t z = size, alpha = 16;
    while (z >= 0x800000) {
        z /= 2;
        alpha += alpha;
    }
    const int32_t beta = 256 / alpha;
    alpha *= z;
    const auto u = static_cast<uint32_t>(fw);
    const int32_t a = static_cast<int32_t>(u >> 24), b = static_cast<int32_t>((u >> 16) & 255),
                  c = static_cast<int32_t>((u >> 8) & 255), d = static_cast<int32_t>(u & 255);
    const int32_t sw = (((d * z) / 256 + c * z) / 256 + b * z) / beta;
    return a == 0 ? sw : a == 255 ? sw - alpha : 0;
}

}  // namespace

int32_t Engine::pdf_vf_local_font(const std::string &name, int32_t size) {
    for (int32_t lfont = tc::font_base + 1; lfont <= font_ptr; lfont++)
        if (font_size[lfont] == size && pool_to_string(font_name[lfont]) == name) return lfont;
    const int32_t lfont = read_font_info(tc::null_cs, pool_string(name.c_str()), 258 /* "" */, size);
    return lfont == tc::null_font ? -1 : lfont;
}

int32_t Engine::pdf_vf(int32_t fnt) {
    if (auto it = pdf_.vf_of_font.find(fnt); it != pdf_.vf_of_font.end()) return it->second;
    pdf_.vf_of_font[fnt] = -1;
    if (!resolver || fnt == tc::null_font) return -1;
    const std::string path = resolver->find(pool_to_string(font_name[fnt]) + ".vf", kOtherFile);
    if (path.empty()) return -1;
    const std::string d = ReadFile(path);
    if (d.size() < 11 || static_cast<unsigned char>(d[0]) != 247 || static_cast<unsigned char>(d[1]) != 202) return -1;
    size_t i = 2;
    i += 1 + static_cast<unsigned char>(d[i]) + 8;  // the comment, checksum and design size
    PdfVirtualFont vf;
    while (i < d.size()) {
        const int op = static_cast<unsigned char>(d[i++]);
        if (op <= 242) {
            uint32_t len, cc;
            if (op == 242) {
                len = VfUnsigned(d, i, 4);
                cc = VfUnsigned(d, i, 4);
                i += 4;
            } else {
                len = static_cast<uint32_t>(op);
                cc = VfUnsigned(d, i, 1);
                i += 3;
            }
            if (i + len > d.size()) break;
            vf.packets[static_cast<int32_t>(cc)] = d.substr(i, len);
            i += len;
        } else if (op >= 243 && op <= 246) {
            const auto k = static_cast<int32_t>(VfUnsigned(d, i, op - 242));
            i += 4;  // checksum
            const int32_t s = VfSigned(d, i, 4);
            i += 4;  // design size
            const int a = static_cast<int>(VfUnsigned(d, i, 1)), l = static_cast<int>(VfUnsigned(d, i, 1));
            if (i + static_cast<size_t>(a + l) > d.size()) break;
            const std::string name = d.substr(i + static_cast<size_t>(a), static_cast<size_t>(l));
            i += static_cast<size_t>(a + l);
            const int32_t lfont = pdf_vf_local_font(name, VfScaled(s, font_size[fnt]));
            if (lfont < 0) continue;
            vf.fonts[k] = lfont;
            if (vf.first < 0) vf.first = lfont;
        } else {
            break;  // the postamble
        }
    }
    pdf_.vfs.push_back(std::move(vf));
    const auto n = static_cast<int32_t>(pdf_.vfs.size()) - 1;
    pdf_.vf_of_font[fnt] = n;
    return n;
}

void Engine::pdf_out_vf_char(PdfContent &pc, int32_t vf, int32_t fnt, int32_t ch, int32_t h, int32_t v) {
    using namespace tc;
    std::string pkt;
    int32_t lfont;
    {
        const PdfVirtualFont &x = pdf_.vfs[static_cast<size_t>(vf)];
        auto it = x.packets.find(ch);
        if (it == x.packets.end() || pdf_.vf_depth >= 10) return;  // no such character; a VF drawing itself
        pkt = it->second;
        lfont = x.first;
    }
    const int32_t size = font_size[fnt];
    struct Regs {
        int32_t h, v, w, x, y, z;
    };
    Regs r{h, v, 0, 0, 0, 0};
    std::vector<Regs> stack;
    ++pdf_.vf_depth;
    auto set_char = [&](int32_t code, bool move) {
        if (lfont < 0) return;
        pdf_out_char(pc, lfont, code, r.h, r.v);
        if (move && code >= font_bc[lfont] && code <= font_ec[lfont]) r.h += char_width(lfont, char_info(lfont, code));
    };
    auto local_font = [&](int32_t k) {
        const PdfVirtualFont &x = pdf_.vfs[static_cast<size_t>(vf)];
        auto it = x.fonts.find(k);
        lfont = it == x.fonts.end() ? -1 : it->second;
    };
    size_t i = 0;
    while (i < pkt.size()) {
        const int op = static_cast<unsigned char>(pkt[i++]);
        if (op < 128) {
            set_char(op, true);
        } else if (op <= 131) {
            set_char(static_cast<int32_t>(VfUnsigned(pkt, i, op - 127)), true);
        } else if (op == 132 || op == 137) {
            const int32_t a = VfScaled(VfSigned(pkt, i, 4), size), b = VfScaled(VfSigned(pkt, i, 4), size);
            if (a > 0 && b > 0) pdf_out_rule(pc, r.h, r.v, b, a);
            if (op == 132) r.h += b;
        } else if (op <= 136) {
            set_char(static_cast<int32_t>(VfUnsigned(pkt, i, op - 132)), false);
        } else if (op == 141) {
            stack.push_back(r);
        } else if (op == 142) {
            if (!stack.empty()) {
                r = stack.back();
                stack.pop_back();
            }
        } else if (op >= 143 && op <= 146) {
            r.h += VfScaled(VfSigned(pkt, i, op - 142), size);
        } else if (op >= 147 && op <= 151) {
            if (op > 147) r.w = VfScaled(VfSigned(pkt, i, op - 147), size);
            r.h += r.w;
        } else if (op >= 152 && op <= 156) {
            if (op > 152) r.x = VfScaled(VfSigned(pkt, i, op - 152), size);
            r.h += r.x;
        } else if (op >= 157 && op <= 160) {
            r.v += VfScaled(VfSigned(pkt, i, op - 156), size);
        } else if (op >= 161 && op <= 165) {
            if (op > 161) r.y = VfScaled(VfSigned(pkt, i, op - 161), size);
            r.v += r.y;
        } else if (op >= 166 && op <= 170) {
            if (op > 166) r.z = VfScaled(VfSigned(pkt, i, op - 166), size);
            r.v += r.z;
        } else if (op >= 171 && op <= 234) {
            local_font(op - 171);
        } else if (op >= 235 && op <= 238) {
            local_font(static_cast<int32_t>(VfUnsigned(pkt, i, op - 234)));
        } else if (op >= 239 && op <= 242) {
            const uint32_t len = VfUnsigned(pkt, i, op - 238);
            if (i + len > pkt.size()) break;
            pdf_special(pc, pkt.substr(i, len), r.h, r.v);
            i += len;
        } else if (op != 138) {
            break;  // not a command a packet may hold
        }
    }
    --pdf_.vf_depth;
}

void Engine::pdf_out_rule(PdfContent &pc, int32_t h, int32_t v, int32_t w, int32_t ht) {
    // the rule's lower left corner is at (h, v)
    EndText(pc);
    pc.s += Num(Milli(h)) + " " + Num(Milli(pc.page_h - v)) + " " + Num(Milli(w)) + " " + Num(Milli(ht)) + " re f\n";
}

void Engine::pdf_out_literal(PdfContent &pc, const std::string &text, int32_t mode, int32_t h, int32_t v) {
    if (mode == 0) {
        EndText(pc);
        const std::string x = Num(Milli(h)), y = Num(Milli(pc.page_h - v));
        pc.s += "1 0 0 1 " + x + " " + y + " cm\n" + text + "\n1 0 0 1 " +
               (x[0] == '-' ? x.substr(1) : "-" + x) + " " + (y[0] == '-' ? y.substr(1) : "-" + y) + " cm\n";
    } else if (mode == 1) {
        EndText(pc);
        pc.s += text + "\n";
    } else {
        EndArray(pc);
        pc.s += text + "\n";
    }
}

void Engine::pdf_colorstack_emit(PdfContent &pc, int32_t stack, int32_t h, int32_t v) {
    PdfColorStack &cs = pdf_.colorstacks[static_cast<size_t>(stack)];
    const std::string &lit = cs.stack.empty() ? cs.initial : cs.stack.back();
    pdf_out_literal(pc, lit, cs.direct ? 2 : 1, h, v);
}

void Engine::pdf_out_whatsit(PdfContent &pc, int32_t p, int32_t h, int32_t v, int32_t this_box) {
    using namespace tc;
    const int32_t sub = subtype(p);
    if (sub == open_node || sub == write_node || sub == close_node) {
        out_what(p);
        return;
    }
    if (sub == special_node) {
        pdf_special(pc, tokens_to_string(link(link(p + 1))), h, v);  // write_tokens(p), after its reference count
        return;
    }
    if (sub == language_node) return;
    if (doing_leaders && sub != pdf_literal_node && sub != pdf_colorstack_node && sub != pdf_setmatrix_node &&
        sub != pdf_save_node && sub != pdf_restore_node && sub != pdf_refxform_node && sub != pdf_refximage_node)
        return;
    const PdfNodeData d = pdf_node(p);  // a copy: rendering a form can add nodes
    auto list_text = [&]() { return link(p + 5) != null ? tokens_to_string(link(link(p + 5))) : std::string(); };
    auto dim = [&](int32_t x, int32_t box_dim) { return x == null_flag ? box_dim : x; };
    switch (sub) {
    case pdf_literal_node:
        pdf_out_literal(pc, d.shipout ? pdf_expand_at_shipout(link(p + 5)) : list_text(), d.mode, h, v);
        return;
    case pdf_colorstack_node: {
        if (d.stack < 0 || d.stack >= static_cast<int32_t>(pdf_.colorstacks.size())) return;
        PdfColorStack &cs = pdf_.colorstacks[static_cast<size_t>(d.stack)];
        if (d.mode == 0) {
            if (cs.stack.empty()) cs.stack.push_back(list_text());
            else cs.stack.back() = list_text();
        } else if (d.mode == 1) {
            cs.stack.push_back(list_text());
        } else if (d.mode == 2) {
            if (!cs.stack.empty()) cs.stack.pop_back();
        }
        pdf_colorstack_emit(pc, d.stack, h, v);
        return;
    }
    case pdf_setmatrix_node: {
        EndText(pc);
        const std::string x = Num(Milli(h)), y = Num(Milli(pc.page_h - v));
        auto neg = [](const std::string &s) { return s[0] == '-' ? s.substr(1) : "-" + s; };
        pc.s += "1 0 0 1 " + x + " " + y + " cm\n" + list_text() + " 0 0 cm\n1 0 0 1 " + neg(x) + " " + neg(y) + " cm\n";
        return;
    }
    case pdf_save_node:
        EndText(pc);
        pc.s += "q\n";
        return;
    case pdf_restore_node:
        EndText(pc);
        pc.s += "Q\n";
        return;
    case pdf_save_pos_node:
        pdf_.last_x_pos = h;
        pdf_.last_y_pos = pc.page_h - v;
        return;
    case pdf_refxform_node: {
        PdfObject *o = pdf_obj(d.obj);
        if (!o) return;
        if (!o->written) pdf_write_obj(d.obj);
        EndText(pc);
        pc.xforms.insert(d.obj);
        pc.s += "q\n1 0 0 1 " + Num(Milli(h)) + " " + Num(Milli(pc.page_h - v - depth(p))) + " cm\n/Fm" +
               std::to_string(d.obj) + " Do\nQ\n";
        return;
    }
    case pdf_refximage_node: {
        PdfObject *o = pdf_obj(d.obj);
        if (!o) return;
        if (!o->written) pdf_write_obj(d.obj);
        o = pdf_obj(d.obj);  // writing may have made objects (and moved this one)
        if (!o->image_ok) return;  // an image we could not read
        EndText(pc);
        pc.ximages.insert(d.obj);
        const int32_t w = width(p), hd = height(p) + depth(p);
        pc.s += "q\n" + Num(Milli(w)) + " 0 0 " + Num(Milli(hd)) + " " + Num(Milli(h)) + " " +
               Num(Milli(pc.page_h - v - depth(p))) + " cm\n/Im" + std::to_string(d.obj) + " Do\nQ\n";
        return;
    }
    case pdf_annot_node: {
        const int32_t w = dim(width(p), width(this_box)), ht = dim(height(p), height(this_box)),
                      dp = dim(depth(p), depth(this_box));
        const std::string rect = Num(Milli(h)) + " " + Num(Milli(pc.page_h - v - dp)) + " " + Num(Milli(h + w)) + " " +
                                 Num(Milli(pc.page_h - v + ht));
        pdf_write_object_text(d.obj, "<< /Type /Annot " + list_text() + " /Rect [" + rect + "] >>");
        pc.annots.push_back(d.obj);
        return;
    }
    case pdf_start_link_node: {
        // a `user` action is the rest of the annotation, /Subtype and all
        const bool user = d.text.rfind("user ", 0) == 0;
        PdfLink l;
        l.level = pc.level;
        l.width = width(p);
        l.height = height(p);
        l.depth = depth(p);
        l.head = "<< /Type /Annot " + std::string(user ? "" : "/Subtype /Link ") + list_text();
        l.tail = (user ? d.text.substr(5) : pdf_action(d.text)) + " >>";
        l.obj = d.obj;
        l.h = h;
        l.v = v;
        l.box = this_box;
        l.open = true;
        pc.links->push_back(std::move(l));
        return;
    }
    case pdf_end_link_node: {
        if (pc.links->empty()) return;
        const PdfLink &l = pc.links->back();
        if (l.open) pdf_write_link_part(pc, pc.links->size() - 1, l.width != null_flag ? l.h + l.width : h);
        pc.links->pop_back();
        return;
    }
    case pdf_dest_node: {
        const int32_t m = dimen_par(pdf_dest_margin_code);
        const std::string pg = std::to_string(pdf_page_object(pdf_.total_pages + 1)) + " 0 R";
        const std::string x = Num(Milli(h - m)), y = Num(Milli(pc.page_h - v + m));
        std::string arr;
        switch (d.dest_type) {
        case 1: arr = "[" + pg + " /Fit]"; break;
        case 2: arr = "[" + pg + " /FitH " + y + "]"; break;
        case 3: arr = "[" + pg + " /FitV " + x + "]"; break;
        case 4: arr = "[" + pg + " /FitB]"; break;
        case 5: arr = "[" + pg + " /FitBH " + y + "]"; break;
        case 6: arr = "[" + pg + " /FitBV " + x + "]"; break;
        case 7: {
            const int32_t w = dim(width(p), width(this_box)), ht = dim(height(p), height(this_box)),
                          dp = dim(depth(p), depth(this_box));
            arr = "[" + pg + " /FitR " + Num(Milli(h - m)) + " " + Num(Milli(pc.page_h - v - dp - m)) + " " +
                  Num(Milli(h + w + m)) + " " + Num(Milli(pc.page_h - v + ht + m)) + "]";
            break;
        }
        default:
            arr = "[" + pg + " /XYZ " + x + " " + y + " " +
                  (d.zoom > 0 ? NumD(static_cast<double>(d.zoom) / 1000.0) : std::string("null")) + "]";
            break;
        }
        // a number a link already asked for has its object reserved
        int32_t obj = d.obj;
        if (!d.named) {
            if (auto it = pdf_.num_dests.find(d.num); it != pdf_.num_dests.end()) obj = it->second;
        } else if (auto it = pdf_.named_dests.find(d.text); it != pdf_.named_dests.end()) {
            obj = it->second;
        }
        if (PdfObject *o = pdf_obj(obj); o && o->written) {
            if (int_par(pdf_suppress_warning_dup_dest_code) <= 0) {
                print_nl_str("pdfTeX warning (ext4): destination with the same identifier (");
                print_str(d.named ? ("name{" + d.text + "}").c_str() : ("num" + std::to_string(d.num)).c_str());
                print_str(") has been already used, duplicate ignored");
                print_ln();
            }
            return;
        }
        pdf_write_object_text(obj, arr);
        if (d.named) pdf_.named_dests[d.text] = obj;
        else pdf_.num_dests[d.num] = obj;
        return;
    }
    default:
        return;  // threads, snapping: not drawn
    }
}

// The text of a token list expanded now, as \write expands its text when
// the page ships (\pdfliteral shipout).
std::string Engine::pdf_expand_at_shipout(int32_t toks) {
    using namespace tc;
    if (toks == null) return "";
    int32_t q = get_avail();
    info(q) = right_brace_token + '}';
    const int32_t r = get_avail();
    link(q) = r;
    info(r) = cs_token_flag + end_write;
    begin_token_list(q, inserted);
    begin_token_list(toks, write_text);
    q = get_avail();
    info(q) = left_brace_token + '{';
    begin_token_list(q, inserted);
    const int32_t old_mode = mode();
    mode() = 0;
    cur_cs = write_loc;
    q = scan_toks(false, true);
    get_token();
    if (cur_tok != cs_token_flag + end_write) {
        print_err_str("Unbalanced literal");
        set_help({"This \\pdfliteral shipout has fewer real {'s than }'s."});
        error_();
        do get_token();
        while (cur_tok != cs_token_flag + end_write);
    }
    mode() = old_mode;
    end_token_list();
    std::string text = tokens_to_string(link(def_ref));
    flush_list(def_ref);
    return text;
}

// One part of a link: its annotation, from where the part began to `right`.
void Engine::pdf_write_link_part(PdfContent &pc, size_t k, int32_t right) {
    using namespace tc;
    PdfLink &l = (*pc.links)[k];
    const int32_t ht = l.height == null_flag ? height(l.box) : l.height;
    const int32_t dp = l.depth == null_flag ? depth(l.box) : l.depth;
    const int32_t m = dimen_par(pdf_link_margin_code);
    const std::string rect = Num(Milli(l.h - m)) + " " + Num(Milli(pc.page_h - l.v - dp - m)) + " " +
                             Num(Milli(right + m)) + " " + Num(Milli(pc.page_h - l.v + ht + m));
    pdf_write_object_text(l.obj, l.head + " /Rect [" + rect + "] " + l.tail);
    pc.annots.push_back(l.obj);
    l.open = false;
}

// --- dvipdfmx's pdf: specials ---------------------------------------------------------------
//
// \special{pdf:...} as xdvipdfmx reads it, so that what LaTeX writes for
// that driver -- mep's own tagged-PDF macros (doc_export.cpp) among it --
// works here too: obj, stream, put, code, literal, content, docinfo, docview.
// Objects are named @name and referred to by that name anywhere in a
// special; @thispage is the page being shipped out, @catalog the catalog.

namespace {

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// The first word of `s` and the rest.
std::pair<std::string, std::string> Word(const std::string &s) {
    const std::string t = Trim(s);
    size_t i = 0;
    while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '<' && t[i] != '[' && t[i] != '(')
        i++;
    return {t.substr(0, i), Trim(t.substr(i))};
}

// The inside of a << ... >> dictionary.
std::string DictInside(const std::string &d) {
    const std::string t = Trim(d);
    if (t.size() >= 4 && t.compare(0, 2, "<<") == 0 && t.compare(t.size() - 2, 2, ">>") == 0) return t.substr(2, t.size() - 4);
    return t;
}

}  // namespace

int32_t Engine::pdf_special_name(const std::string &name) {
    if (name == "catalog") {
        if (pdf_.catalog_obj == 0) pdf_.catalog_obj = pdf_new_obj(PdfObject::Other);
        return pdf_.catalog_obj;
    }
    auto it = pdf_.special_names.find(name);
    if (it != pdf_.special_names.end()) return it->second;
    const int32_t n = pdf_new_obj(PdfObject::Other);
    pdf_.special_names[name] = n;
    return n;
}

std::string Engine::pdf_special_refs(const std::string &text) {
    // @name -> "N 0 R" (outside strings)
    std::string out;
    int paren = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char ch = text[i];
        if (ch == '\\' && paren > 0 && i + 1 < text.size()) {
            out += text.substr(i, 2);
            i++;
            continue;
        }
        if (ch == '(') paren++;
        if (ch == ')' && paren > 0) paren--;
        if (ch == '@' && paren == 0) {
            size_t j = i + 1;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || std::strchr("_.:-", text[j]))) j++;
            if (j > i + 1) {
                const std::string name = text.substr(i + 1, j - i - 1);
                const int32_t n = name == "thispage" ? pdf_page_object(pdf_.total_pages + 1) : pdf_special_name(name);
                out += std::to_string(n) + " 0 R";
                i = j - 1;
                continue;
            }
        }
        out.push_back(ch);
    }
    return out;
}

void Engine::pdf_special(PdfContent &pc, const std::string &special, int32_t h, int32_t v) {
    std::string s = Trim(special);
    if (s.rfind("pdf:", 0) != 0) {
        if (!pdf_.warned_special) {
            pdf_.warned_special = true;
            print_nl_str("pdfTeX warning: Non-PDF special ignored!");
            print_ln();
        }
        return;
    }
    auto [cmd, rest] = Word(s.substr(4));
    if (cmd == "code") {
        pdf_out_literal(pc, rest, 2, h, v);
    } else if (cmd == "literal") {
        auto [w, body] = Word(rest);
        if (w == "direct") pdf_out_literal(pc, body, 2, h, v);
        else pdf_out_literal(pc, rest, 0, h, v);
    } else if (cmd == "content") {
        pdf_out_literal(pc, "q " + rest + " Q", 0, h, v);
    } else if (cmd == "obj") {
        auto [name, body] = Word(rest);
        if (name.size() < 2 || name[0] != '@') return;
        const int32_t n = pdf_special_name(name.substr(1));
        if (pdf_.special_objs.count(n)) return;  // defined already: dvipdfmx keeps the first
        pdf_.special_objs[n] = pdf_special_refs(body);
    } else if (cmd == "put") {
        auto [name, body] = Word(rest);
        if (name.size() < 2 || name[0] != '@') return;
        const std::string target = name.substr(1);
        const std::string value = pdf_special_refs(body);
        if (target == "thispage") {
            pc.page_put += " " + DictInside(value);
        } else if (target == "catalog") {
            pdf_.catalog_extra += " " + DictInside(value);
        } else {
            const int32_t n = pdf_special_name(target);
            std::string &obj = pdf_.special_objs[n];
            const std::string t = Trim(obj);
            if (!t.empty() && t.back() == ']') {
                obj = t.substr(0, t.size() - 1) + " " + value + "]";  // an array: one more element
            } else if (t.size() >= 2 && t.compare(t.size() - 2, 2, ">>") == 0) {
                obj = t.substr(0, t.size() - 2) + " " + DictInside(value) + " >>";  // a dictionary: more entries
            } else {
                obj = value;
            }
        }
    } else if (cmd == "stream") {
        // pdf:stream @name (data)|<hex data> [<< dict >>]
        auto [name, body] = Word(rest);
        if (name.size() < 2 || name[0] != '@' || body.empty()) return;
        std::string data;
        size_t i = 0;
        if (body[0] == '<' && body.compare(0, 2, "<<") != 0) {
            const size_t e = body.find('>');
            if (e == std::string::npos) return;
            int nib = -1;
            for (size_t k = 1; k < e; k++) {
                const int d = std::isdigit(static_cast<unsigned char>(body[k])) ? body[k] - '0'
                              : std::isxdigit(static_cast<unsigned char>(body[k])) ? (std::tolower(body[k]) - 'a' + 10)
                                                                                    : -1;
                if (d < 0) continue;
                if (nib < 0) {
                    nib = d;
                } else {
                    data.push_back(static_cast<char>(nib * 16 + d));
                    nib = -1;
                }
            }
            i = e + 1;
        } else if (body[0] == '(') {
            int depth_ = 0;
            for (i = 0; i < body.size(); i++) {
                const char ch = body[i];
                if (ch == '\\' && i + 1 < body.size()) {
                    data.push_back(body[++i]);
                    continue;
                }
                if (ch == '(' && depth_++ == 0) continue;
                if (ch == ')' && --depth_ == 0) {
                    i++;
                    break;
                }
                data.push_back(ch);
            }
        } else {
            return;
        }
        const int32_t n = pdf_special_name(name.substr(1));
        pdf_.special_streams[n] = {DictInside(pdf_special_refs(body.substr(i))), data};
    } else if (cmd == "docinfo") {
        pdf_.info_extra += " " + DictInside(pdf_special_refs(rest));
    } else if (cmd == "docview") {
        pdf_.catalog_extra += " " + DictInside(pdf_special_refs(rest));
    }
    // others (dest, annot, bann ...): what hyperref's dvipdfmx driver uses;
    // with mep-tex LaTeX takes the pdfTeX driver instead
}

std::string Engine::pdf_action(const std::string &a) {
    // the action spec scan_pdf_action recorded, as a PDF /A entry
    auto field = [&](const std::string &key) -> std::string {
        const size_t k = a.find(" " + key + "{");
        if (k == std::string::npos) return "";
        const size_t b = k + key.size() + 2;
        int depth_ = 1;
        size_t e = b;
        for (; e < a.size(); e++) {
            if (a[e] == '{') depth_++;
            else if (a[e] == '}' && --depth_ == 0) break;
        }
        return a.substr(b, e - b);
    };
    if (a.rfind("user ", 0) == 0) return "/A " + a.substr(5);
    if (a.rfind("goto", 0) == 0) {
        const std::string file = field("file");
        const std::string nw = a.find(" newwindow") != std::string::npos   ? " /NewWindow true"
                               : a.find(" nonewwindow") != std::string::npos ? " /NewWindow false"
                                                                             : "";
        std::string dest;
        if (a.find(" name{") != std::string::npos) {
            dest = "(" + field("name") + ")";
        } else if (size_t k = a.find(" num "); k != std::string::npos) {
            const int32_t n = std::atoi(a.c_str() + k + 5);
            if (file.empty()) {
                auto it = pdf_.num_dests.find(n);
                int32_t obj;
                if (it != pdf_.num_dests.end()) {
                    obj = it->second;
                } else {
                    obj = pdf_new_obj(PdfObject::Dest);
                    pdf_.num_dests[n] = obj;  // filled when \pdfdest num n ships
                    pdf_obj(obj)->reserved = true;
                }
                dest = std::to_string(obj) + " 0 R";
            } else {
                dest = std::to_string(n);
            }
        } else if (size_t k2 = a.find(" page "); k2 != std::string::npos) {
            const int32_t n = std::atoi(a.c_str() + k2 + 6);
            const size_t b = a.find('{', k2);
            const size_t e = a.rfind('}');
            const std::string spec = b != std::string::npos && e != std::string::npos && e > b ? a.substr(b + 1, e - b - 1) : "";
            dest = "[" + (file.empty() ? std::to_string(pdf_page_object(n)) + " 0 R" : std::to_string(n - 1)) + " " + spec + "]";
        }
        if (!file.empty()) return "/A << /S /GoToR /F (" + file + ") /D " + dest + nw + " >>";
        return "/A << /S /GoTo /D " + dest + " >>";
    }
    if (a.rfind("thread", 0) == 0) {
        const std::string file = field("file");
        std::string d = a.find(" name{") != std::string::npos ? "(" + field("name") + ")" : "0";
        return "/A << /S /Thread /D " + d + (file.empty() ? std::string() : " /F (" + file + ")") + " >>";
    }
    return "";
}

void Engine::pdf_hlist_out(PdfContent &pc, int32_t this_box, int32_t &cur_h_, int32_t &cur_v_) {
    using namespace tc;
    double cur_glue = 0;
    int32_t cur_g = 0;
    const int32_t g_order = glue_order(this_box), g_sign = glue_sign(this_box);
    const int32_t base_line = cur_v_, left_edge = cur_h_;
    ++pc.level;
    for (PdfLink &l : *pc.links) {
        if (l.level != pc.level || l.open || l.width != tc::null_flag) continue;
        l.obj = pdf_new_obj(PdfObject::Link);
        l.h = left_edge;
        l.v = base_line;
        l.box = this_box;
        l.open = true;
    }
    int32_t p = list_ptr(this_box);
    while (p != null) {
        if (is_char_node(p)) {
            pdf_out_char(pc, type(p), subtype(p), cur_h_, cur_v_);
            cur_h_ += char_width(type(p), char_info(type(p), subtype(p)));
            p = link(p);
            continue;
        }
        int32_t rht = 0, rdp = 0, rwd = 0;
        bool do_rule = false;
        switch (type(p)) {
        case hlist_node:
        case vlist_node:
            if (list_ptr(p) == null) {
                cur_h_ += width(p);
            } else {
                const int32_t edge = cur_h_;
                cur_v_ = base_line + shift_amount(p);
                if (type(p) == vlist_node) pdf_vlist_out(pc, p, cur_h_, cur_v_);
                else pdf_hlist_out(pc, p, cur_h_, cur_v_);
                cur_h_ = edge + width(p);
                cur_v_ = base_line;
            }
            break;
        case rule_node:
            rht = height(p);
            rdp = depth(p);
            rwd = width(p);
            do_rule = true;
            break;
        case whatsit_node: pdf_out_whatsit(pc, p, cur_h_, cur_v_, this_box); break;
        case glue_node: {
            const int32_t gl = glue_ptr(p);
            rwd = width(gl) - cur_g;
            if (g_sign != normal) {
                if (g_sign == stretching) {
                    if (stretch_order(gl) == g_order) {
                        cur_glue += stretch(gl);
                        cur_g = static_cast<int32_t>(std::lround(std::clamp(glue_set(this_box) * cur_glue, -1e9, 1e9)));
                    }
                } else if (shrink_order(gl) == g_order) {
                    cur_glue -= shrink(gl);
                    cur_g = static_cast<int32_t>(std::lround(std::clamp(glue_set(this_box) * cur_glue, -1e9, 1e9)));
                }
            }
            rwd += cur_g;
            if (subtype(p) >= a_leaders) {
                const int32_t leader_box = leader_ptr(p);
                if (type(leader_box) == rule_node) {
                    rht = height(leader_box);
                    rdp = depth(leader_box);
                    do_rule = true;
                    break;
                }
                const int32_t leader_wd = width(leader_box);
                if (leader_wd > 0 && rwd > 0) {
                    rwd += 10;
                    const int32_t edge = cur_h_ + rwd;
                    int32_t lx = 0;
                    if (subtype(p) == a_leaders) {
                        const int32_t save_h = cur_h_;
                        cur_h_ = left_edge + leader_wd * ((cur_h_ - left_edge) / leader_wd);
                        if (cur_h_ < save_h) cur_h_ += leader_wd;
                    } else {
                        const int32_t nq = rwd / leader_wd, nr = rwd % leader_wd;
                        if (subtype(p) == c_leaders) {
                            cur_h_ += nr / 2;
                        } else {
                            lx = nr / (nq + 1);
                            cur_h_ += (nr - (nq - 1) * lx) / 2;
                        }
                    }
                    while (cur_h_ + leader_wd <= edge) {
                        cur_v_ = base_line + shift_amount(leader_box);
                        const int32_t save_h = cur_h_;
                        const bool outer = doing_leaders;
                        doing_leaders = true;
                        if (type(leader_box) == vlist_node) pdf_vlist_out(pc, leader_box, cur_h_, cur_v_);
                        else pdf_hlist_out(pc, leader_box, cur_h_, cur_v_);
                        doing_leaders = outer;
                        cur_v_ = base_line;
                        cur_h_ = save_h + leader_wd + lx;
                    }
                    cur_h_ = edge - 10;
                    p = link(p);
                    continue;
                }
            }
            cur_h_ += rwd;
            break;
        }
        case kern_node:
        case math_node:
        case margin_kern_node: cur_h_ += width(p); break;
        case ligature_node: {
            const int32_t q = p + 1;  // lig_char(p)
            pdf_out_char(pc, type(q), subtype(q), cur_h_, cur_v_);
            cur_h_ += char_width(type(q), char_info(type(q), subtype(q)));
            break;
        }
        default: break;
        }
        if (do_rule) {
            if (rht == null_flag) rht = height(this_box);
            if (rdp == null_flag) rdp = depth(this_box);
            rht += rdp;
            if (rht > 0 && rwd > 0) pdf_out_rule(pc, cur_h_, base_line + rdp, rwd, rht);
            cur_h_ += rwd;
        }
        p = link(p);
    }
    // a link still open at the end of its line runs to the line's end
    for (size_t k = 0; k < pc.links->size(); ++k) {
        const PdfLink &l = (*pc.links)[k];
        if (l.open && l.box == this_box && l.width == tc::null_flag)
            pdf_write_link_part(pc, k, left_edge + width(this_box));
    }
    --pc.level;
}

void Engine::pdf_vlist_out(PdfContent &pc, int32_t this_box, int32_t &cur_h_, int32_t &cur_v_) {
    using namespace tc;
    double cur_glue = 0;
    int32_t cur_g = 0;
    const int32_t g_order = glue_order(this_box), g_sign = glue_sign(this_box);
    const int32_t left_edge = cur_h_;
    cur_v_ -= height(this_box);
    const int32_t top_edge = cur_v_;
    ++pc.level;
    int32_t p = list_ptr(this_box);
    while (p != null) {
        if (is_char_node(p)) confusion(pool_string("vlistout"));
        int32_t rht = 0, rdp = 0, rwd = 0;
        bool do_rule = false;
        switch (type(p)) {
        case hlist_node:
        case vlist_node:
            if (list_ptr(p) == null) {
                cur_v_ += height(p) + depth(p);
            } else {
                cur_v_ += height(p);
                const int32_t save_v = cur_v_;
                cur_h_ = left_edge + shift_amount(p);
                if (type(p) == vlist_node) pdf_vlist_out(pc, p, cur_h_, cur_v_);
                else pdf_hlist_out(pc, p, cur_h_, cur_v_);
                cur_v_ = save_v + depth(p);
                cur_h_ = left_edge;
            }
            break;
        case rule_node:
            rht = height(p);
            rdp = depth(p);
            rwd = width(p);
            do_rule = true;
            break;
        case whatsit_node: pdf_out_whatsit(pc, p, cur_h_, cur_v_, this_box); break;
        case glue_node: {
            const int32_t gl = glue_ptr(p);
            rht = width(gl) - cur_g;
            if (g_sign != normal) {
                if (g_sign == stretching) {
                    if (stretch_order(gl) == g_order) {
                        cur_glue += stretch(gl);
                        cur_g = static_cast<int32_t>(std::lround(std::clamp(glue_set(this_box) * cur_glue, -1e9, 1e9)));
                    }
                } else if (shrink_order(gl) == g_order) {
                    cur_glue -= shrink(gl);
                    cur_g = static_cast<int32_t>(std::lround(std::clamp(glue_set(this_box) * cur_glue, -1e9, 1e9)));
                }
            }
            rht += cur_g;
            if (subtype(p) >= a_leaders) {
                const int32_t leader_box = leader_ptr(p);
                if (type(leader_box) == rule_node) {
                    rwd = width(leader_box);
                    rdp = 0;
                    do_rule = true;
                    break;
                }
                const int32_t leader_ht = height(leader_box) + depth(leader_box);
                if (leader_ht > 0 && rht > 0) {
                    rht += 10;
                    const int32_t edge = cur_v_ + rht;
                    int32_t lx = 0;
                    if (subtype(p) == a_leaders) {
                        const int32_t save_v = cur_v_;
                        cur_v_ = top_edge + leader_ht * ((cur_v_ - top_edge) / leader_ht);
                        if (cur_v_ < save_v) cur_v_ += leader_ht;
                    } else {
                        const int32_t nq = rht / leader_ht, nr = rht % leader_ht;
                        if (subtype(p) == c_leaders) {
                            cur_v_ += nr / 2;
                        } else {
                            lx = nr / (nq + 1);
                            cur_v_ += (nr - (nq - 1) * lx) / 2;
                        }
                    }
                    while (cur_v_ + leader_ht <= edge) {
                        cur_h_ = left_edge + shift_amount(leader_box);
                        cur_v_ += height(leader_box);
                        const int32_t save_v = cur_v_;
                        const bool outer = doing_leaders;
                        doing_leaders = true;
                        if (type(leader_box) == vlist_node) pdf_vlist_out(pc, leader_box, cur_h_, cur_v_);
                        else pdf_hlist_out(pc, leader_box, cur_h_, cur_v_);
                        doing_leaders = outer;
                        cur_h_ = left_edge;
                        cur_v_ = save_v - height(leader_box) + leader_ht + lx;
                    }
                    cur_v_ = edge - 10;
                    p = link(p);
                    continue;
                }
            }
            cur_v_ += rht;
            break;
        }
        case kern_node: cur_v_ += width(p); break;
        default: break;
        }
        if (do_rule) {
            if (rwd == null_flag) rwd = width(this_box);
            rht += rdp;
            cur_v_ += rht;
            if (rht > 0 && rwd > 0) pdf_out_rule(pc, cur_h_, cur_v_, rwd, rht);
        }
        p = link(p);
    }
    --pc.level;
}

// --- Objects -----------------------------------------------------------------------------------

void Engine::pdf_begin_file() {
    if (pdf_.started) return;
    pdf_.started = true;
    int32_t major = int_par(tc::pdf_major_version_code), minor = int_par(tc::pdf_minor_version_code);
    if (major < 1) major = 1;
    if (minor < 0 || minor > 9) minor = 4;
    pdf_.out = "%PDF-" + std::to_string(major) + "." + std::to_string(minor) + "\n%\xD0\xD4\xC5\xD8\n";
}

void Engine::pdf_begin_obj(int32_t n) {
    pdf_begin_file();
    if (static_cast<int32_t>(pdf_.offsets.size()) <= n) pdf_.offsets.resize(static_cast<size_t>(n) + 1, -1);
    pdf_.offsets[static_cast<size_t>(n)] = static_cast<int64_t>(pdf_.out.size());
    pdf_.out += std::to_string(n) + " 0 obj\n";
    if (PdfObject *o = pdf_obj(n)) o->written = true;
}

void Engine::pdf_write_object_text(int32_t n, const std::string &body) {
    pdf_begin_obj(n);
    pdf_.out += body;
    pdf_.out += "\nendobj\n";
}

void Engine::pdf_write_stream(int32_t n, const std::string &dict, const std::string &data, bool compress) {
    pdf_begin_obj(n);
    std::string bytes = data;
    std::string d = dict;
    if (compress && int_par(tc::pdf_compress_level_code) > 0 && d.find("/Filter") == std::string::npos) {
        bytes = deflate::DeflateZlib(reinterpret_cast<const unsigned char *>(data.data()), data.size());
        d += " /Filter /FlateDecode";
    }
    pdf_.out += "<< " + d + " /Length " + std::to_string(bytes.size()) + " >>\nstream\n";
    pdf_.out += bytes;
    pdf_.out += "\nendstream\nendobj\n";
}

bool Engine::pdf_writing() { return pdf_.started || int_par(tc::pdf_output_code) > 0; }

std::string Engine::pdf_resources(const PdfContent &pc, const std::string &extra) {
    std::string r = "<< /ProcSet [/PDF /Text /ImageB /ImageC /ImageI]";
    if (!pc.fonts.empty()) {
        r += " /Font <<";
        for (int32_t k : pc.fonts) {
            const PdfFont &fnt = pdf_.fonts[static_cast<size_t>(k)];
            r += " /F" + std::to_string(fnt.res) + " " + std::to_string(fnt.obj) + " 0 R";
        }
        r += " >>";
    }
    if (!pc.xforms.empty() || !pc.ximages.empty()) {
        r += " /XObject <<";
        for (int32_t k : pc.xforms) r += " /Fm" + std::to_string(k) + " " + std::to_string(k) + " 0 R";
        for (int32_t k : pc.ximages) r += " /Im" + std::to_string(k) + " " + std::to_string(k) + " 0 R";
        r += " >>";
    }
    if (!extra.empty()) r += " " + extra;
    return r + " >>";
}

void Engine::pdf_write_obj(int32_t n) {
    if (!pdf_writing()) return;
    PdfObject *o = pdf_obj(n);
    if (!o || o->written) return;
    switch (o->kind) {
    case PdfObject::Raw:
    case PdfObject::Stream: {
        if (o->reserved) return;
        std::string content = o->content;
        if (o->from_file) {
            const std::string path = find_input_file(content);
            content = ReadFile(path.empty() ? content : path);
        }
        if (o->kind == PdfObject::Stream) pdf_write_stream(n, o->attr, content, true);
        else pdf_write_object_text(n, content);
        return;
    }
    case PdfObject::Xform: {
        if (o->box == tc::null) return;
        PdfContent pc;
        const int32_t box = o->box;
        pc.page_h = height(box) + depth(box);
        int32_t h = 0, v = height(box);
        if (type(box) == tc::vlist_node) pdf_vlist_out(pc, box, h, v);
        else pdf_hlist_out(pc, box, h, v);
        EndText(pc);
        o = pdf_obj(n);  // rendering may have made objects
        const std::string dict = "/Type /XObject /Subtype /Form /BBox [0 0 " + Num(Milli(o->width)) + " " +
                                 Num(Milli(o->height + o->depth)) + "] /FormType 1 /Matrix [1 0 0 1 0 0] /Resources " +
                                 pdf_resources(pc, o->resources) + (o->attr.empty() ? "" : " " + o->attr);
        pdf_write_stream(n, dict, pc.s, true);
        flush_node_list(box);
        pdf_obj(n)->box = tc::null;
        return;
    }
    case PdfObject::Ximage: pdf_write_image(n); return;
    default: return;
    }
}

// --- Fonts --------------------------------------------------------------------------------------

void Engine::pdf_load_map_file(const std::string &spec) {
    // spec: [+|=|-]file
    char mode = ' ';
    std::string name = spec;
    if (!name.empty() && (name[0] == '+' || name[0] == '=' || name[0] == '-')) {
        mode = name[0];
        name = name.substr(1);
    }
    const std::string path = resolver ? resolver->find(name, kOtherFile) : name;
    std::string text = ReadFile(path.empty() ? name : path);
    if (text.empty()) {
        print_nl_str("pdfTeX warning: cannot open font map file (");
        print_str(name.c_str());
        print_char(')');
        print_ln();
        return;
    }
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        pdf_map_line(std::string(1, mode) + text.substr(i, e - i));
        i = e + 1;
    }
}

void Engine::pdf_map_line(const std::string &line_in) {
    char mode = line_in.empty() ? ' ' : line_in[0];
    std::string line = line_in.substr(1);
    if (mode != '+' && mode != '=' && mode != '-' && mode != ' ') {
        line = line_in;
        mode = ' ';
    }
    // tokens: tfm [psname] [flags] ["specials"] [<[enc] [<font]...
    size_t i = 0;
    auto skip = [&]() {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
    };
    skip();
    if (i >= line.size() || std::strchr("%#;*", line[i])) return;
    PdfMapEntry m;
    auto word = [&]() {
        const size_t b = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
        return line.substr(b, i - b);
    };
    m.tfm = word();
    for (;;) {
        skip();
        if (i >= line.size()) break;
        if (line[i] == '"') {
            const size_t e = line.find('"', i + 1);
            const std::string sp = line.substr(i + 1, (e == std::string::npos ? line.size() : e) - i - 1);
            i = e == std::string::npos ? line.size() : e + 1;
            // "<number> SlantFont", "<number> ExtendFont"
            std::vector<std::string> toks;
            for (size_t k = 0; k < sp.size();) {
                while (k < sp.size() && sp[k] == ' ') k++;
                const size_t b = k;
                while (k < sp.size() && sp[k] != ' ') k++;
                if (k > b) toks.push_back(sp.substr(b, k - b));
            }
            for (size_t k = 1; k < toks.size(); k++) {
                if (toks[k] == "SlantFont") m.slant = std::atof(toks[k - 1].c_str());
                else if (toks[k] == "ExtendFont") m.extend = std::atof(toks[k - 1].c_str());
            }
        } else if (line[i] == '<') {
            i++;
            bool enc = false;
            if (i < line.size() && line[i] == '<') i++;
            else if (i < line.size() && line[i] == '[') {
                i++;
                enc = true;
            }
            skip();
            const std::string fnt = word();
            if (enc || EndsWith(fnt, ".enc")) m.enc_file = fnt;
            else m.font_file = fnt;
        } else {
            const std::string w = word();
            if (m.ps_name.empty() && !(w[0] >= '0' && w[0] <= '9')) m.ps_name = w;
        }
    }
    if (m.ps_name.empty()) m.ps_name = m.tfm;
    if (mode == '-') {
        pdf_.map.erase(m.tfm);
    } else if (mode == '+') {
        if (!pdf_.map.count(m.tfm)) pdf_.map[m.tfm] = m;
    } else {
        pdf_.map[m.tfm] = m;
    }
}

const PdfMapEntry *Engine::pdf_map_entry(const std::string &tfm) {
    if (!pdf_.map_loaded) {
        pdf_.map_loaded = true;
        // a \pdfmapfile without +, =, - given before the first font replaces pdftex.map
        bool replaced = false;
        for (const std::string &l : pdf_.map_lines) {
            if (l.rfind("file:", 0) == 0 && l.size() > 5 && !std::strchr("+=-", l[5])) replaced = true;
        }
        if (!replaced) pdf_load_map_file("pdftex.map");
        for (const std::string &l : pdf_.map_lines) {
            if (l.rfind("file:", 0) == 0) pdf_load_map_file(l.substr(5));
            else if (l.rfind("line:", 0) == 0) pdf_map_line(l.substr(5));
        }
        pdf_.map_lines.clear();
        // mep's bundle draws the EC fonts with Latin Modern when nothing
        // else maps them (no cm-super): tools/tex_bundle.py
        if (resolver && !resolver->find("mep-fallback.map", kOtherFile).empty()) pdf_load_map_file("+mep-fallback.map");
    }
    auto it = pdf_.map.find(tfm);
    return it == pdf_.map.end() ? nullptr : &it->second;
}

void Engine::pdf_write_font(PdfFont &pf) {
    const int32_t fnt = pf.tex_font;
    int fc = 256, lc = -1;
    for (int k = 0; k < 256; k++) {
        if (pf.used[k]) {
            fc = std::min(fc, k);
            lc = std::max(lc, k);
        }
    }
    if (lc < 0) {
        fc = 0;
        lc = 0;
    }
    std::string widths;
    for (int k = fc; k <= lc; k++) {
        widths += NumD(pdf_char_width(static_cast<int32_t>(&pf - pdf_.fonts.data()), k));
        widths += (k - fc) % 16 == 15 ? "\n" : " ";
    }
    const PdfMapEntry *m = pdf_map_entry(pf.tfm);
    std::string base = m ? m->ps_name : pf.tfm;
    std::vector<std::string> enc;
    if (m && !m->enc_file.empty()) {
        const std::string path = resolver ? resolver->find(m->enc_file, kOtherFile) : m->enc_file;
        enc = ReadEncoding(ReadFile(path.empty() ? m->enc_file : path));
        if (enc.empty()) {
            print_nl_str("pdfTeX warning: cannot open encoding file (");
            print_str(m->enc_file.c_str());
            print_char(')');
            print_ln();
        }
    }
    std::string encoding;
    if (!enc.empty()) {
        encoding = " /Encoding << /Type /Encoding /Differences [" + std::to_string(fc);
        for (int k = fc; k <= lc; k++) encoding += " /" + enc[static_cast<size_t>(k)];
        encoding += "] >>";
    }
    std::string descriptor;
    std::vector<std::string> code_names = enc;  // each code's glyph name, for the ToUnicode map
    if (m && !m->font_file.empty() && m->embed) {
        const std::string path = resolver ? resolver->find(m->font_file, kOtherFile) : m->font_file;
        const std::string data = ReadFile(path.empty() ? m->font_file : path);
        Type1Parts parts;
        gfx::t1::FontInfo info;
        if (!data.empty() && SplitType1(data, parts) &&
            gfx::t1::InitFont(&info, reinterpret_cast<const unsigned char *>(data.data()), static_cast<int>(data.size()))) {
            if (code_names.empty()) {
                code_names.assign(256, "");
                for (int k = 0; k < 256; k++) {
                    const int gid = info.builtin_encoding[k];
                    if (gid >= 0 && gid < static_cast<int>(info.glyph_names.size()))
                        code_names[static_cast<size_t>(k)] = info.glyph_names[static_cast<size_t>(gid)];
                }
            }
            // the glyphs used, by name, and the ones seac builds them from
            std::set<std::string> keep = {".notdef"};
            for (int k = fc; k <= lc; k++) {
                if (!pf.used[k]) continue;
                if (!enc.empty()) {
                    keep.insert(enc[static_cast<size_t>(k)]);
                } else {
                    const int gid = info.builtin_encoding[k];
                    if (gid >= 0 && gid < static_cast<int>(info.glyph_names.size()))
                        keep.insert(info.glyph_names[static_cast<size_t>(gid)]);
                }
            }
            std::set<int32_t> subrs;
            for (const std::string &nm : std::set<std::string>(keep)) {
                const int gid = gfx::t1::GidForName(&info, nm);
                if (gid >= 0 && gid < static_cast<int>(info.charstrings.size()))
                    ScanCharstring(info.charstrings[static_cast<size_t>(gid)], keep, subrs);
            }
            for (const std::string &nm : keep) {  // the seac parts' own Subrs
                const int gid = gfx::t1::GidForName(&info, nm);
                if (gid >= 0 && gid < static_cast<int>(info.charstrings.size()))
                    ScanCharstring(info.charstrings[static_cast<size_t>(gid)], keep, subrs);
            }
            for (std::vector<int32_t> work(subrs.begin(), subrs.end()); !work.empty();) {
                const int32_t k = work.back();
                work.pop_back();
                if (k < 0 || k >= static_cast<int32_t>(info.subrs.size())) continue;
                std::set<int32_t> more;
                std::set<std::string> unused;
                ScanCharstring(info.subrs[static_cast<size_t>(k)], unused, more);
                for (int32_t m2 : more) {
                    if (subrs.insert(m2).second) work.push_back(m2);
                }
            }
            const std::string plain = Eexec(parts.binary, false);
            int len_iv = 4;
            if (const size_t li = plain.find("/lenIV"); li != std::string::npos) len_iv = std::atoi(plain.c_str() + li + 6);
            const std::string sub = SubsetType1Private(plain, keep, subrs, len_iv);
            const std::string binary = Eexec(sub, true);
            // the subset's tag: six letters from the glyphs it has
            std::string names;
            for (const std::string &nm : keep) names += nm + " ";
            const std::string md5 = pdfcrypt::Md5(names + base);
            std::string tag;
            for (size_t k = 0; k < 6; k++) tag.push_back(static_cast<char>('A' + static_cast<unsigned char>(md5[k]) % 26));
            base = tag + "+" + base;
            const int32_t file_obj = pdf_new_obj(PdfObject::Other);
            const int32_t desc_obj = pdf_new_obj(PdfObject::Other);
            pdf_write_stream(file_obj,
                             "/Length1 " + std::to_string(parts.clear.size()) + " /Length2 " +
                                 std::to_string(binary.size()) + " /Length3 0",
                             parts.clear + binary, true);
            std::string bbox = FindCleartext(parts.clear, "/FontBBox");
            std::vector<double> bb;
            for (size_t k = 0; k < bbox.size();) {
                while (k < bbox.size() && bbox[k] == ' ') k++;
                const size_t b = k;
                while (k < bbox.size() && bbox[k] != ' ') k++;
                if (k > b) bb.push_back(std::atof(bbox.substr(b, k - b).c_str()));
            }
            bb.resize(4, 0);
            const double italic = std::atof(FindCleartext(parts.clear, "/ItalicAngle").c_str());
            const double xh = static_cast<double>(font_info[tc::x_height_code + param_base[fnt]].int_) * 1000.0 / font_size[fnt];
            const std::string charset = [&]() {
                std::string cs;
                for (const std::string &nm : keep) {
                    if (nm != ".notdef") cs += "/" + nm;
                }
                return cs;
            }();
            pdf_write_object_text(desc_obj, "<< /Type /FontDescriptor /FontName /" + base + " /Flags 4 /FontBBox [" +
                                                NumD(bb[0]) + " " + NumD(bb[1]) + " " + NumD(bb[2]) + " " + NumD(bb[3]) +
                                                "] /Ascent " + NumD(bb[3]) + " /CapHeight " + NumD(bb[3]) +
                                                " /Descent " + NumD(bb[1]) + " /ItalicAngle " + NumD(italic) +
                                                " /StemV 80 /XHeight " + NumD(xh) + " /CharSet (" + charset +
                                                ") /FontFile " + std::to_string(file_obj) + " 0 R >>");
            descriptor = " /FontDescriptor " + std::to_string(desc_obj) + " 0 R";
        } else {
            print_nl_str("pdfTeX warning: cannot open Type 1 font file for reading (");
            print_str(m->font_file.c_str());
            print_char(')');
            print_ln();
        }
    } else if (!m) {
        print_nl_str("pdfTeX warning: no mapping for font ");
        print_str(pf.tfm.c_str());
        print_str(" (it is not embedded)");
        print_ln();
    }
    const std::string to_unicode = int_par(tc::pdf_gen_tounicode_code) > 0 ? pdf_write_to_unicode(pf, code_names) : "";
    pdf_write_object_text(pf.obj, "<< /Type /Font /Subtype /Type1 /BaseFont /" + base + " /FirstChar " +
                                      std::to_string(fc) + " /LastChar " + std::to_string(lc) + " /Widths [" + widths +
                                      "]" + descriptor + encoding + to_unicode + " >>");
}

// The text each code stands for, as a ToUnicode CMap (when
// \pdfgentounicode is on): from the \pdfglyphtounicode table (LaTeX loads
// glyphtounicode.tex), where `tfm:<font>/<glyph>` overrides `<glyph>` for
// one font, else from names that say it (uniXXXX, uXXXX[XX]).
// Returns the font dictionary's /ToUnicode entry, or "" if nothing maps.
std::string Engine::pdf_write_to_unicode(const PdfFont &pf, const std::vector<std::string> &names) {
    auto hex_of = [&pf](const std::string &name, const std::unordered_map<std::string, std::string> &table) -> std::string {
        auto it = table.find("tfm:" + pf.tfm + "/" + name);  // this font's own reading of the name first
        if (it == table.end()) it = table.find(name);
        std::string v;
        if (it != table.end()) {
            v = it->second;
        } else if (name.size() >= 7 && name.rfind("uni", 0) == 0) {
            v = name.substr(3);
        } else if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u') {
            v = name.substr(1);
        } else {
            return "";
        }
        std::string out;
        for (char ch : v) {
            if (std::isxdigit(static_cast<unsigned char>(ch))) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            else if (ch != ' ') return "";
        }
        // UTF-16: four digits a unit; a code point beyond the BMP as a surrogate pair
        if (out.size() == 5 || out.size() == 6) {
            const unsigned long cp = std::stoul(out, nullptr, 16) - 0x10000;
            char buf[16];
            std::snprintf(buf, sizeof buf, "%04lX%04lX", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            return buf;
        }
        return out.size() % 4 == 0 ? out : "";
    };
    std::vector<std::pair<int, std::string>> map;
    // every code the font has (as pdfTeX does), not only the ones used
    for (int k = 0; k < static_cast<int>(names.size()); k++) {
        if (names[static_cast<size_t>(k)].empty() || names[static_cast<size_t>(k)] == ".notdef") continue;
        const std::string h = hex_of(names[static_cast<size_t>(k)], pdf_.glyph_to_unicode);
        if (!h.empty()) map.emplace_back(k, h);
    }
    if (map.empty()) return "";
    std::string cmap = "%!PS-Adobe-3.0 Resource-CMap\n/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                       "/CIDSystemInfo << /Registry (TeX) /Ordering (" + pf.tfm + ") /Supplement 0 >> def\n"
                       "/CMapName /TeX-" + pf.tfm + "-0 def\n/CMapType 2 def\n"
                       "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n";
    for (size_t i = 0; i < map.size(); i += 100) {
        const size_t n = std::min<size_t>(100, map.size() - i);
        cmap += std::to_string(n) + " beginbfchar\n";
        for (size_t k = i; k < i + n; k++) {
            char code[8];
            std::snprintf(code, sizeof code, "<%02X>", map[k].first);
            cmap += std::string(code) + " <" + map[k].second + ">\n";
        }
        cmap += "endbfchar\n";
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    const int32_t obj = pdf_new_obj(PdfObject::Other);
    pdf_write_stream(obj, "", cmap, true);
    return " /ToUnicode " + std::to_string(obj) + " 0 R";
}

// --- Pages -------------------------------------------------------------------------------------

std::string Engine::pdf_toks(int32_t loc) {
    const int32_t r = equiv(loc);
    return r != tc::null ? tokens_to_string(link(r)) : std::string();
}

void Engine::pdf_ship_out(int32_t p) {
    using namespace tc;
    if (job_name == 0) open_log_file();
    pdf_begin_file();
    if (pdf_.pages_obj == 0) pdf_.pages_obj = pdf_new_obj(PdfObject::Other);
    PdfContent pc;
    const int32_t page_w = dimen_par(pdf_page_width_code) != 0 ? dimen_par(pdf_page_width_code)
                                                               : width(p) + 2 * dimen_par(pdf_h_origin_code);
    const int32_t page_h = dimen_par(pdf_page_height_code) != 0
                               ? dimen_par(pdf_page_height_code)
                               : height(p) + depth(p) + 2 * dimen_par(pdf_v_origin_code);
    pc.page_h = page_h;
    pc.links = &pdf_.page_links;
    // stacks marked `page` start each page with their current value
    for (size_t k = 0; k < pdf_.colorstacks.size(); k++) {
        if (pdf_.colorstacks[k].page_start && !pdf_.colorstacks[k].stack.empty())
            pdf_colorstack_emit(pc, static_cast<int32_t>(k), 0, 0);
    }
    // the box's reference point: the origin plus \hoffset and \voffset, as in DVI output
    int32_t h = dimen_par(pdf_h_origin_code) + dimen_par(h_offset_code),
            v = dimen_par(pdf_v_origin_code) + dimen_par(v_offset_code) + height(p);
    if (type(p) == vlist_node) pdf_vlist_out(pc, p, h, v);
    else pdf_hlist_out(pc, p, h, v);
    EndText(pc);
    pdf_.total_pages++;
    const int32_t page_obj = pdf_page_object(pdf_.total_pages);
    const int32_t content_obj = pdf_new_obj(PdfObject::Other);
    pdf_write_stream(content_obj, "", pc.s, true);
    std::string annots;
    if (!pc.annots.empty()) {
        annots = " /Annots [";
        for (int32_t a : pc.annots) annots += std::to_string(a) + " 0 R ";
        annots += "]";
    }
    const std::string attr = pdf_toks(pdf_page_attr_loc);
    pdf_write_object_text(page_obj, "<< /Type /Page /Contents " + std::to_string(content_obj) + " 0 R /Resources " +
                                        pdf_resources(pc, pdf_toks(pdf_page_resources_loc)) + " /MediaBox [0 0 " +
                                        Num(Milli(page_w)) + " " + Num(Milli(page_h)) + "] /Parent " +
                                        std::to_string(pdf_.pages_obj) + " 0 R" + annots +
                                        (attr.empty() ? "" : " " + attr) + pc.page_put + " >>");
    pdf_.page_list.push_back(page_obj);
}

int32_t Engine::pdf_page_object(int32_t pg) {
    auto it = pdf_.page_objs.find(pg);
    if (it != pdf_.page_objs.end()) return it->second;
    const int32_t n = pdf_new_obj(PdfObject::Page);
    pdf_.page_objs[pg] = n;
    return n;
}

int32_t Engine::pdf_font_resource_number(int32_t fnt) {
    return pdf_.fonts[static_cast<size_t>(pdf_font_index(fnt))].res;
}

int32_t Engine::pdf_font_object(int32_t fnt) { return pdf_.fonts[static_cast<size_t>(pdf_font_index(fnt))].obj; }

// --- The end of the job ---------------------------------------------------------------------------

bool Engine::pdf_output_used() { return pdf_.started && pdf_.total_pages > 0; }

int32_t Engine::pdf_write_outlines() {
    if (pdf_.outlines.empty()) return 0;
    const int32_t root = pdf_new_obj(PdfObject::Outline);
    // Each outline's |count| children follow it; build the tree.
    struct Node {
        int32_t index;
        std::vector<int32_t> kids;
    };
    std::vector<Node> nodes(pdf_.outlines.size());
    std::vector<int32_t> parent(pdf_.outlines.size(), -1);
    std::vector<int32_t> top;
    size_t next = 0;
    std::function<void(int32_t)> take = [&](int32_t par) {
        const int32_t k = static_cast<int32_t>(next++);
        nodes[static_cast<size_t>(k)].index = k;
        parent[static_cast<size_t>(k)] = par;
        if (par < 0) top.push_back(k);
        else nodes[static_cast<size_t>(par)].kids.push_back(k);
        const int32_t n = std::abs(pdf_.outlines[static_cast<size_t>(k)].count);
        for (int32_t j = 0; j < n && next < pdf_.outlines.size(); j++) take(k);
    };
    while (next < pdf_.outlines.size()) take(-1);
    auto obj = [&](int32_t k) { return pdf_.outlines[static_cast<size_t>(k)].obj; };
    for (size_t k = 0; k < nodes.size(); k++) {
        const PdfOutline &o = pdf_.outlines[k];
        const std::vector<int32_t> &sibs = parent[k] < 0 ? top : nodes[static_cast<size_t>(parent[k])].kids;
        const auto pos = static_cast<size_t>(std::find(sibs.begin(), sibs.end(), static_cast<int32_t>(k)) - sibs.begin());
        std::string d = "<< /Title (" + o.title + ") " + pdf_action(o.action) + " /Parent " +
                        std::to_string(parent[k] < 0 ? root : obj(parent[k])) + " 0 R";
        if (pos > 0) d += " /Prev " + std::to_string(obj(sibs[pos - 1])) + " 0 R";
        if (pos + 1 < sibs.size()) d += " /Next " + std::to_string(obj(sibs[pos + 1])) + " 0 R";
        if (!nodes[k].kids.empty()) {
            d += " /First " + std::to_string(obj(nodes[k].kids.front())) + " 0 R /Last " +
                 std::to_string(obj(nodes[k].kids.back())) + " 0 R /Count " + std::to_string(o.count);
        }
        if (!o.attr.empty()) d += " " + o.attr;
        pdf_write_object_text(o.obj, d + " >>");
    }
    pdf_write_object_text(root, "<< /Type /Outlines /First " + std::to_string(obj(top.front())) + " 0 R /Last " +
                                    std::to_string(obj(top.back())) + " 0 R /Count " + std::to_string(top.size()) +
                                    " >>");
    return root;
}

void Engine::pdf_finish_file() {
    using namespace tc;
    if (pdf_.total_pages == 0) {
        print_nl_str("No pages of output.");
        return;
    }
    // objects made and not yet written: \pdfobj's, forms never used, fonts
    for (int32_t n = 1; n < pdf_.next_obj; n++) {
        PdfObject *o = pdf_obj(n);
        if (o && !o->written && (o->kind == PdfObject::Raw || o->kind == PdfObject::Stream)) pdf_write_obj(n);
    }
    for (PdfFont &fnt : pdf_.fonts) pdf_write_font(fnt);
    const int32_t outlines = pdf_write_outlines();
    // the page tree: one level
    {
        std::string kids;
        for (int32_t k : pdf_.page_list) kids += std::to_string(k) + " 0 R ";
        const std::string attr = pdf_toks(pdf_pages_attr_loc);
        pdf_write_object_text(pdf_.pages_obj, "<< /Type /Pages /Count " + std::to_string(pdf_.page_list.size()) +
                                                  " /Kids [" + kids + "]" + (attr.empty() ? "" : " " + attr) + " >>");
    }
    std::string names = pdf_.names;
    if (!pdf_.named_dests.empty()) {
        const int32_t dests = pdf_new_obj(PdfObject::Other);
        std::string arr;
        for (const auto &[name, obj] : pdf_.named_dests) arr += "(" + name + ") " + std::to_string(obj) + " 0 R\n";
        pdf_write_object_text(dests, "<< /Names [" + arr + "] >>");
        names = "/Dests " + std::to_string(dests) + " 0 R " + names;
    }
    // objects made by pdf: specials, now that every @name has its number
    for (const auto &[n, body] : std::map<int32_t, std::string>(pdf_.special_objs))
        pdf_write_object_text(n, pdf_special_refs(body));
    for (const auto &[n, sd] : std::map<int32_t, std::pair<std::string, std::string>>(pdf_.special_streams))
        pdf_write_stream(n, pdf_special_refs(sd.first), sd.second, sd.first.find("/Metadata") == std::string::npos);
    const int32_t catalog = pdf_.catalog_obj != 0 ? pdf_.catalog_obj : pdf_new_obj(PdfObject::Other);
    std::string cat = "<< /Type /Catalog /Pages " + std::to_string(pdf_.pages_obj) + " 0 R" + pdf_.catalog_extra;
    if (outlines) cat += " /Outlines " + std::to_string(outlines) + " 0 R";
    if (!names.empty()) {
        const int32_t nobj = pdf_new_obj(PdfObject::Other);
        pdf_write_object_text(nobj, "<< " + names + " >>");
        cat += " /Names " + std::to_string(nobj) + " 0 R";
    }
    if (!pdf_.catalog_openaction.empty()) cat += " /OpenAction " + pdf_action(pdf_.catalog_openaction).substr(3);
    if (!pdf_.catalog.empty()) cat += " " + pdf_.catalog;
    pdf_write_object_text(catalog, cat + ">>");
    // the document information dictionary
    const int32_t info = pdf_new_obj(PdfObject::Other);
    {
        std::string d = "<< ";
        const std::string &u = pdf_.info;
        auto absent = [&](const char *key) { return u.find(key) == std::string::npos; };
        if (absent("/Producer")) d += "/Producer (mep-tex) ";
        if (absent("/Creator")) d += "/Creator (TeX) ";
        if (int_par(pdf_info_omit_date_code) <= 0) {
            std::time_t t;
            bool utc = false;
            if (const char *e = std::getenv("SOURCE_DATE_EPOCH"); e && *e) {
                t = static_cast<std::time_t>(std::atoll(e));
                utc = true;
            } else {
                t = std::time(nullptr);
            }
            std::tm tmv{};
            if (utc) gmtime_r(&t, &tmv);
            else localtime_r(&t, &tmv);
            char buf[64];
            std::snprintf(buf, sizeof buf, "D:%04d%02d%02d%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                          tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            std::string date = buf;
            const long off = utc ? 0 : tmv.tm_gmtoff;
            if (off == 0) {
                date += "Z";
            } else {
                const long a = off < 0 ? -off : off;
                std::snprintf(buf, sizeof buf, "%c%02ld'%02ld'", off < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
                date += buf;
            }
            if (absent("/CreationDate")) d += "/CreationDate (" + date + ") ";
            if (absent("/ModDate")) d += "/ModDate (" + date + ") ";
        }
        if (int_par(pdf_suppress_ptex_info_code) <= 0 && absent("/Trapped")) d += "/Trapped /False ";
        pdf_write_object_text(info, d + u + pdf_.info_extra + " >>");
    }
    // every number given out has an object, if only `null`
    for (int32_t n = 1; n < pdf_.next_obj; n++) {
        if (n >= static_cast<int32_t>(pdf_.offsets.size()) || pdf_.offsets[static_cast<size_t>(n)] < 0)
            pdf_write_object_text(n, "null");
    }
    const int64_t xref = static_cast<int64_t>(pdf_.out.size());
    pdf_.out += "xref\n0 " + std::to_string(pdf_.next_obj) + "\n0000000000 65535 f \n";
    for (int32_t n = 1; n < pdf_.next_obj; n++) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010lld 00000 n \n", static_cast<long long>(pdf_.offsets[static_cast<size_t>(n)]));
        pdf_.out += buf;
    }
    std::string id = pdf_.trailer_id;
    if (id.empty()) {
        std::string h;
        for (char ch : pdfcrypt::Md5(pdf_.out.substr(0, std::min<size_t>(pdf_.out.size(), 65536)) +
                                     std::to_string(pdf_.out.size()))) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned char>(ch));
            h += buf;
        }
        id = "[<" + h + "> <" + h + ">]";
    }
    pdf_.out += "trailer\n<< /Size " + std::to_string(pdf_.next_obj) + " /Root " + std::to_string(catalog) +
                " 0 R /Info " + std::to_string(info) + " 0 R /ID " + id + (pdf_.trailer.empty() ? "" : " " + pdf_.trailer) +
                " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    // the file: <jobname>.pdf in the output directory
    pack_job_name(pool_string(".pdf"));
    std::string path = current_file_name();
    if (!opt.output_dir.empty() && path.find('/') == std::string::npos) path = opt.output_dir + "/" + path;
    std::ofstream outf(path, std::ios::binary);
    outf.write(pdf_.out.data(), static_cast<std::streamsize>(pdf_.out.size()));
    if (!outf) {
        print_nl_str("! I can't write on file `");
        print_str(path.c_str());
        print_str("'.");
        return;
    }
    print_nl_str("Output written on ");
    print_str(path.c_str());
    print_str(" (");
    print_int(pdf_.total_pages);
    print_str(pdf_.total_pages != 1 ? " pages" : " page");
    print_str(", ");
    print_int(static_cast<int32_t>(pdf_.out.size()));
    print_str(" bytes).");
}

// --- Images: PNG, JPEG, and pages of PDF files ----------------------------------------------------
//
// Every image becomes an XObject drawn into the unit square, so a
// reference scales it to the node's width and height: a bitmap is that
// already, and a PDF page is a form whose /Matrix maps its box there.

namespace {

enum class ImageType { None, Png, Jpeg, Pdf };

ImageType SniffImage(const std::string &d) {
    if (d.size() >= 8 && d.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return ImageType::Png;
    if (d.size() >= 3 && static_cast<unsigned char>(d[0]) == 0xFF && static_cast<unsigned char>(d[1]) == 0xD8) return ImageType::Jpeg;
    if (d.size() >= 5 && d.find("%PDF-") < 1024) return ImageType::Pdf;
    return ImageType::None;
}

uint32_t Be32(const std::string &d, size_t i) {
    return static_cast<uint32_t>(static_cast<unsigned char>(d[i])) << 24 | static_cast<uint32_t>(static_cast<unsigned char>(d[i + 1])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(d[i + 2])) << 8 | static_cast<uint32_t>(static_cast<unsigned char>(d[i + 3]));
}

uint32_t Be16(const std::string &d, size_t i) {
    return static_cast<uint32_t>(static_cast<unsigned char>(d[i])) << 8 | static_cast<uint32_t>(static_cast<unsigned char>(d[i + 1]));
}

struct JpegInfo {
    int width = 0, height = 0, components = 0, bits = 8;
    double xdpi = 0, ydpi = 0;
    bool adobe = false;
};

bool ReadJpegInfo(const std::string &d, JpegInfo &info) {
    size_t i = 2;
    while (i + 4 <= d.size()) {
        if (static_cast<unsigned char>(d[i]) != 0xFF) return false;
        const int marker = static_cast<unsigned char>(d[i + 1]);
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            i += 2;
            continue;
        }
        const size_t len = Be16(d, i + 2);
        if (marker == 0xE0 && len >= 14 && d.compare(i + 4, 5, std::string("JFIF\0", 5)) == 0) {
            const int units = static_cast<unsigned char>(d[i + 11]);
            const double x = Be16(d, i + 12), y = Be16(d, i + 14);
            if (units == 1) {
                info.xdpi = x;
                info.ydpi = y;
            } else if (units == 2) {
                info.xdpi = x * 2.54;
                info.ydpi = y * 2.54;
            }
        } else if (marker == 0xEE && len >= 12 && d.compare(i + 4, 5, "Adobe") == 0) {
            info.adobe = true;
        } else if ((marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 9 >= d.size()) return false;
            info.bits = static_cast<unsigned char>(d[i + 4]);
            info.height = static_cast<int>(Be16(d, i + 5));
            info.width = static_cast<int>(Be16(d, i + 7));
            info.components = static_cast<unsigned char>(d[i + 9]);
            return info.width > 0 && info.height > 0;
        }
        i += 2 + len;
    }
    return false;
}

// A PDF file being included: its bytes stay alive with the parsed document.
struct IncludedPdf {
    std::string data;
    pdfdoc::PdfDocument doc;
};

std::map<std::string, std::shared_ptr<IncludedPdf>> &PdfCache() {
    static std::map<std::string, std::shared_ptr<IncludedPdf>> cache;
    return cache;
}

std::shared_ptr<IncludedPdf> LoadPdf(const std::string &path, const std::string &data) {
    auto &cache = PdfCache();
    auto it = cache.find(path);
    if (it != cache.end() && it->second->data == data) return it->second;  // and not changed since
    auto p = std::make_shared<IncludedPdf>();
    p->data = data;
    p->doc.Load(reinterpret_cast<const unsigned char *>(p->data.data()), p->data.size());
    cache[path] = p;
    return p;
}

void SerializeObject(const pdfobj::Object &v, std::string &out, const std::function<int32_t(int)> &renumber);

void SerializeString(const std::string &s, std::string &out) {
    out += "<";
    char buf[4];
    for (char ch : s) {
        std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned char>(ch));
        out += buf;
    }
    out += ">";
}

void SerializeName(const std::string &n, std::string &out) {
    out += "/";
    for (char ch : n) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 32 || c >= 127 || std::strchr("#%()/<>[]{}", c)) {
            char buf[4];
            std::snprintf(buf, sizeof buf, "#%02X", c);
            out += buf;
        } else {
            out.push_back(ch);
        }
    }
}

void SerializeObject(const pdfobj::Object &v, std::string &out, const std::function<int32_t(int)> &renumber) {
    using pdfobj::Type;
    switch (v.type) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.bool_val ? "true" : "false"; break;
    case Type::Int: out += std::to_string(v.int_val); break;
    case Type::Real: out += NumD(v.real_val); break;
    case Type::String: SerializeString(v.str_val, out); break;
    case Type::Name: SerializeName(v.str_val, out); break;
    case Type::Array:
        out += "[";
        for (const auto &e : v.array_val) {
            SerializeObject(e, out, renumber);
            out += " ";
        }
        out += "]";
        break;
    case Type::Dict:
        out += "<<";
        for (const auto &[k, e] : v.dict_val) {
            SerializeName(k, out);
            out += " ";
            SerializeObject(e, out, renumber);
            out += " ";
        }
        out += ">>";
        break;
    case Type::Reference: out += std::to_string(renumber(v.ref_val.num)) + " 0 R"; break;
    }
}

}  // namespace

void Engine::pdf_read_image_size(PdfObject &o) {
    o.width = o.height = o.depth = 0;
    o.pages = 1;
    o.image_ok = false;
    const std::string d = ReadFile(o.file);
    const int32_t res = int_par(tc::pdf_image_resolution_code) > 0 ? int_par(tc::pdf_image_resolution_code) : 72;
    auto px_to_sp = [](double px, double dpi) { return static_cast<int32_t>(std::llround(px * 4736287.0 / dpi)); };
    switch (SniffImage(d)) {
    case ImageType::Png: {
        if (d.size() < 33) break;
        const uint32_t w = Be32(d, 16), h = Be32(d, 20);
        double xdpi = res, ydpi = res;
        // pHYs: pixels per metre
        if (const size_t ph = d.find("pHYs"); ph != std::string::npos && ph + 13 <= d.size() && d[ph + 12] == 1) {
            const double xp = Be32(d, ph + 4), yp = Be32(d, ph + 8);
            if (xp > 0 && yp > 0) {
                xdpi = xp * 0.0254;
                ydpi = yp * 0.0254;
            }
        }
        o.width = px_to_sp(w, xdpi);
        o.height = px_to_sp(h, ydpi);
        o.colordepth = static_cast<unsigned char>(d[24]);
        o.image_ok = true;
        break;
    }
    case ImageType::Jpeg: {
        JpegInfo ji;
        if (!ReadJpegInfo(d, ji)) break;
        o.width = px_to_sp(ji.width, ji.xdpi > 0 ? ji.xdpi : res);
        o.height = px_to_sp(ji.height, ji.ydpi > 0 ? ji.ydpi : res);
        o.colordepth = ji.bits;
        o.image_ok = true;
        break;
    }
    case ImageType::Pdf: {
        auto pdf = LoadPdf(o.file, d);
        o.pages = pdf->doc.PageCount();
        const pdfdoc::Page *pg = pdf->doc.GetPage(std::clamp(o.page, 1, std::max(o.pages, 1)) - 1);
        if (!pg) break;
        const double *b = pg->effective_box;
        double w = b[2] - b[0], h = b[3] - b[1];
        if (pg->rotate == 90 || pg->rotate == 270) std::swap(w, h);
        for (int k = 0; k < 4; k++) o.bbox[k] = static_cast<int32_t>(std::llround(b[k] * kSpPerBp));
        o.width = static_cast<int32_t>(std::llround(w * kSpPerBp));
        o.height = static_cast<int32_t>(std::llround(h * kSpPerBp));
        o.image_ok = true;
        break;
    }
    case ImageType::None:
        break;
    }
    if (!o.image_ok) {
        print_err_str("pdfTeX error (ext1): cannot read image file `");
        print_str(o.file.c_str());
        print_str("'");
        set_help({"mep-tex reads PNG, JPEG and PDF images."});
        error_();
    }
}

void Engine::pdf_write_image(int32_t n) {
    PdfObject *o = pdf_obj(n);
    if (!o) return;
    o->written = true;
    if (!o->image_ok) {
        pdf_write_object_text(n, "null");
        return;
    }
    const std::string d = ReadFile(o->file);
    const std::string attr = o->attr.empty() ? "" : " " + o->attr;
    switch (SniffImage(d)) {
    case ImageType::Png: {
        int w = 0, h = 0;
        std::string err;
        unsigned char *px = png::Decode(reinterpret_cast<const unsigned char *>(d.data()), d.size(), &w, &h, &err);
        if (!px) {
            o->image_ok = false;
            pdf_write_object_text(n, "null");
            return;
        }
        const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        bool gray = true, alpha = false;
        for (size_t k = 0; k < count; k++) {
            const unsigned char *q = px + k * 4;
            if (q[0] != q[1] || q[1] != q[2]) gray = false;
            if (q[3] != 255) alpha = true;
        }
        std::string color, mask;
        color.reserve(count * (gray ? 1 : 3));
        for (size_t k = 0; k < count; k++) {
            const unsigned char *q = px + k * 4;
            color.push_back(static_cast<char>(q[0]));
            if (!gray) {
                color.push_back(static_cast<char>(q[1]));
                color.push_back(static_cast<char>(q[2]));
            }
            if (alpha) mask.push_back(static_cast<char>(q[3]));
        }
        std::free(px);
        const std::string dims = "/Width " + std::to_string(w) + " /Height " + std::to_string(h) + " /BitsPerComponent 8";
        std::string smask;
        if (alpha) {
            const int32_t m = pdf_new_obj(PdfObject::Other);
            pdf_write_stream(m, "/Type /XObject /Subtype /Image " + dims + " /ColorSpace /DeviceGray", mask, true);
            smask = " /SMask " + std::to_string(m) + " 0 R";
        }
        pdf_write_stream(n, "/Type /XObject /Subtype /Image " + dims + " /ColorSpace " +
                                (gray ? "/DeviceGray" : "/DeviceRGB") + smask + attr, color, true);
        return;
    }
    case ImageType::Jpeg: {
        JpegInfo ji;
        ReadJpegInfo(d, ji);
        const char *cs = ji.components == 1 ? "/DeviceGray" : ji.components == 4 ? "/DeviceCMYK" : "/DeviceRGB";
        std::string dict = "/Type /XObject /Subtype /Image /Width " + std::to_string(ji.width) + " /Height " +
                           std::to_string(ji.height) + " /BitsPerComponent " + std::to_string(ji.bits) +
                           " /ColorSpace " + cs + " /Filter /DCTDecode";
        if (ji.components == 4 && ji.adobe) dict += " /Decode [1 0 1 0 1 0 1 0]";
        pdf_write_stream(n, dict + attr, d, false);
        return;
    }
    case ImageType::Pdf: {
        auto pdf = LoadPdf(o->file, d);
        const pdfdoc::Page *pg = pdf->doc.GetPage(std::clamp(o->page, 1, std::max(o->pages, 1)) - 1);
        if (!pg) {
            pdf_write_object_text(n, "null");
            return;
        }
        const auto *data = reinterpret_cast<const unsigned char *>(pdf->data.data());
        const size_t len = pdf->data.size();
        const pdfxref::XrefTable &xref = pdf->doc.Xref();
        // the page's content, decoded and joined
        std::string content;
        auto add_stream = [&](const pdfobj::Object &ref) {
            if (!ref.IsReference()) return;
            pdfobj::Object dict;
            std::string raw, dec;
            if (pdfxref::ResolveStream(data, len, xref, ref.ref_val.num, ref.ref_val.gen, &dict, &raw) &&
                pdffilter::DecodeStream(raw, &dict, &dec)) {
                content += dec;
                content += "\n";
            }
        };
        if (const pdfobj::Object *contents = pg->dict.Find("Contents")) {
            pdfobj::Object cv = *contents;
            if (cv.IsReference()) {
                pdfobj::Object r = pdfxref::ResolveObject(data, len, xref, cv.ref_val.num, cv.ref_val.gen);
                if (r.IsArray()) cv = r;
            }
            if (cv.IsArray()) {
                for (const auto &e : cv.array_val) add_stream(e);
            } else {
                add_stream(cv);
            }
        }
        // the objects it uses, copied with numbers of this file
        std::map<int, int32_t> numbers;
        std::vector<int> queue;
        std::function<int32_t(int)> renumber = [&](int old) {
            auto it = numbers.find(old);
            if (it != numbers.end()) return it->second;
            const int32_t nn = pdf_new_obj(PdfObject::Other);
            numbers[old] = nn;
            queue.push_back(old);
            return nn;
        };
        std::string resources;
        SerializeObject(pg->resources, resources, renumber);
        if (pg->resources.IsNull()) resources = "<<>>";
        const double *b = pg->effective_box;
        const double w = b[2] - b[0], h = b[3] - b[1];
        // the box onto the unit square, turned by the page's /Rotate
        double m[6] = {1 / w, 0, 0, 1 / h, -b[0] / w, -b[1] / h};
        switch (pg->rotate) {
        case 90: m[0] = 0; m[1] = -1 / w; m[2] = 1 / h; m[3] = 0; m[4] = -b[1] / h; m[5] = 1 + b[0] / w; break;
        case 180: m[0] = -1 / w; m[3] = -1 / h; m[4] = 1 + b[0] / w; m[5] = 1 + b[1] / h; break;
        case 270: m[0] = 0; m[1] = 1 / w; m[2] = -1 / h; m[3] = 0; m[4] = 1 + b[1] / h; m[5] = -b[0] / w; break;
        default: break;
        }
        auto real = [](double v) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.8f", v);
            std::string t = buf;
            while (t.back() == '0') t.pop_back();
            if (t.back() == '.') t.pop_back();
            return t == "-0" ? std::string("0") : t;
        };
        std::string matrix;
        for (double v : m) matrix += real(v) + " ";
        pdf_write_stream(n, "/Type /XObject /Subtype /Form /FormType 1 /BBox [" + real(b[0]) + " " + real(b[1]) + " " +
                                real(b[2]) + " " + real(b[3]) + "] /Matrix [" + matrix + "] /Resources " + resources +
                                attr,
                         content, true);
        while (!queue.empty()) {
            const int old = queue.back();
            queue.pop_back();
            const int32_t nn = numbers[old];
            pdfobj::Object dict;
            std::string raw;
            if (pdfxref::ResolveStream(data, len, xref, old, 0, &dict, &raw)) {
                if (dict.IsDict()) dict.dict_val.erase("Length");
                std::string body;
                SerializeObject(dict, body, renumber);
                // the dictionary's own << >> wrap our /Length too
                body = body.substr(2, body.size() - 4);
                pdf_write_stream(nn, body, raw, false);
            } else {
                pdfobj::Object v = pdfxref::ResolveObject(data, len, xref, old, 0);
                if (v.IsDict()) v.dict_val.erase("Parent");
                std::string body;
                SerializeObject(v, body, renumber);
                pdf_write_object_text(nn, body);
            }
        }
        return;
    }
    case ImageType::None:
        pdf_write_object_text(n, "null");
        return;
    }
}

}  // namespace meptex
