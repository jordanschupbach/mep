#include "office_doc.h"
#include "image_doc.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// RTF for the office editor: a .rtf opens as a laid-out page like a .docx
// or .odt (LoadRtfFromMemory -> OfficeDoc) and `:w` writes it back as RTF
// (SaveRtfToMemory). RTF is a text format with no container, so unlike the
// DOCX/ODT savers nothing of the original file is copied through: the
// document is written fresh from the model, which is also why a save loses
// what the model has no field for (headers and footers, footnotes, page
// setup) -- the same v1 scope the DOCX/ODT paths document.
//
// Reading follows the RTF spec's reader model: a stack of group states, a
// current destination (plain text, a table definition, a picture ...),
// control words with an optional numeric parameter, `\'hh` as cp1252 and
// `\uN` as Unicode with `\uc` skip characters after it. mepml's own RTF
// importer (src/mepml_import.cpp) reads the same constructs into a mepml
// document; this one reads them into the editor's flat paragraph/span
// model, so it is deliberately a small reader of its own rather than a
// detour through mepml and back.

namespace {

// --- Reading ---------------------------------------------------------------

std::string Utf8Of(unsigned cp) {
    std::string o;
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return o;
}

unsigned Cp1252Of(unsigned char c) {
    static const unsigned kHigh[32] = {0x20AC, 0x81,   0x201A, 0x192,  0x201E, 0x2026, 0x2020, 0x2021, 0x2C6,  0x2030, 0x160,
                                       0x2039, 0x152,  0x8D,   0x17D,  0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                       0x2013, 0x2014, 0x2DC,  0x2122, 0x161,  0x203A, 0x153,  0x9D,   0x17E,  0x178};
    return c >= 0x80 && c < 0xA0 ? kHigh[c - 0x80] : c;
}

std::string LowerStr(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimStr(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

struct RtfColor {
    bool is_auto = true;
    unsigned char r = 0, g = 0, b = 0;
};

struct RtfReader {
    enum Dest { Normal, Skip, FontTbl, ColorTbl, StyleSheet, ListText, Pict, FldInst };
    // What a group's state carries: the character format and the
    // destination. Paragraph properties are not part of it -- RTF resets
    // them with \pard, not at a group's end.
    struct State {
        bool b = false, i = false, ul = false, strike = false, super = false, sub = false;
        int font = 0, fs = 24, cf = 0, highlight = 0;
        Dest dest = Normal;
    };
    std::vector<State> stack;
    State st;
    int deff = 0;
    int default_fs = 24;  // the Normal style's \fs, else RTF's own default
    std::map<int, std::string> fonts;
    std::vector<RtfColor> colors;
    std::map<int, std::string> styles;  // \sN -> lower-case name
    int uc = 1;

    OfficeDoc *out = nullptr;

    // The paragraph being built.
    DocParagraph cur;
    bool cur_has_text = false;  // any text, tab or break added since \pard/\par
    int style = 0, outline = -1;
    bool in_list = false, intbl = false;
    std::string list_text;
    // A table being built: plain cell text, as DocTable holds it.
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    // Destination text.
    std::string dest_text, pict_hex, pict_ext;
    int font_cur = 0, style_cur = 0;
    RtfColor color_cur;
    bool color_seen = false;

    DocFormat Current() const {
        DocFormat f;
        f.bold = st.b;
        f.italic = st.i;
        f.underline = st.ul;
        f.strike = st.strike;
        f.superscript = st.super;
        f.subscript = st.sub;
        // The document's default font (\deff) is the model's default look,
        // whatever its name: a file set in Times throughout reads as body
        // text, and only a run in another face is marked.
        auto fit = fonts.find(st.font);
        if (fit != fonts.end() && st.font != deff) {
            const std::string fl = LowerStr(fit->second);
            if (fl.find("courier") != std::string::npos || fl.find("mono") != std::string::npos || fl.find("consolas") != std::string::npos ||
                fl.find("menlo") != std::string::npos)
                f.font_family = OfficeFontFamily::Mono;
            else if (fl.find("times") != std::string::npos || fl.find("serif") != std::string::npos || fl.find("georgia") != std::string::npos ||
                     fl.find("garamond") != std::string::npos || fl.find("cambria") != std::string::npos || fl.find("book") != std::string::npos ||
                     fl.find("palatino") != std::string::npos)
                f.font_family = OfficeFontFamily::Serif;
        }
        if (st.fs > 0 && st.fs != default_fs) f.font_size_pt = static_cast<float>(st.fs) / 2.0f;
        if (st.cf > 0 && st.cf < static_cast<int>(colors.size()) && !colors[static_cast<size_t>(st.cf)].is_auto) {
            const RtfColor &c = colors[static_cast<size_t>(st.cf)];
            f.has_color = true;
            f.color_r = c.r;
            f.color_g = c.g;
            f.color_b = c.b;
        }
        if (st.highlight > 0 && st.highlight < static_cast<int>(colors.size()) && !colors[static_cast<size_t>(st.highlight)].is_auto) {
            const RtfColor &c = colors[static_cast<size_t>(st.highlight)];
            f.has_highlight = true;
            f.highlight_r = c.r;
            f.highlight_g = c.g;
            f.highlight_b = c.b;
        }
        return f;
    }

    void AddText(const std::string &t) {
        if (t.empty()) return;
        switch (st.dest) {
            case Normal: {
                const DocFormat f = Current();
                const int start = static_cast<int>(cur.text.size());
                cur.text += t;
                cur_has_text = true;
                const int end = static_cast<int>(cur.text.size());
                if (f == DocFormat{}) return;
                if (!cur.spans.empty() && cur.spans.back().end == start && cur.spans.back().fmt == f) cur.spans.back().end = end;
                else cur.spans.push_back({start, end, f});
                break;
            }
            case ListText: list_text += t; break;
            case Pict: pict_hex += t; break;
            case FontTbl:
            case StyleSheet: dest_text += t; break;
            case ColorTbl:
            case FldInst:
            case Skip: break;
        }
    }

    // The paragraph's kind from its style and outline level, as the DOCX
    // loader reads w:pStyle "HeadingN" and the ODT loader text:h.
    int HeadingLevel() const {
        auto sit = styles.find(style);
        const std::string n = sit == styles.end() ? "" : sit->second;
        if (n.rfind("heading ", 0) == 0) return std::max(1, std::min(6, std::atoi(n.c_str() + 8)));
        if (n == "title") return 1;
        if (outline >= 0 && outline < 6) return outline + 1;
        return 0;
    }

    void FlushTable() {
        if (!row.empty()) {
            rows.push_back(row);
            row.clear();
        }
        if (rows.empty()) return;
        DocTable t;
        t.rows = static_cast<int>(rows.size());
        for (const auto &r : rows) t.cols = std::max(t.cols, static_cast<int>(r.size()));
        if (t.cols == 0) {
            rows.clear();
            return;
        }
        t.cells.assign(static_cast<size_t>(t.rows) * static_cast<size_t>(t.cols), "");
        for (int r = 0; r < t.rows; ++r)
            for (int c = 0; c < static_cast<int>(rows[static_cast<size_t>(r)].size()); ++c) t.Cell(r, c) = rows[static_cast<size_t>(r)][static_cast<size_t>(c)];
        rows.clear();
        out->tables.push_back(std::move(t));
        // Anchored on a paragraph of its own, as the DOCX loader anchors
        // a w:tbl.
        DocParagraph anchor;
        anchor.table_ref = static_cast<int>(out->tables.size()) - 1;
        out->paragraphs.push_back(std::move(anchor));
    }

    void EndPara() {
        if (intbl) {
            // A cell's paragraphs are joined by \cell: its text is one line.
            if (!cur.text.empty() && cur.text.back() != ' ') cur.text += ' ';
            return;
        }
        FlushTable();
        DocParagraph p;
        std::swap(p, cur);
        cur_has_text = false;
        const std::string lt = TrimStr(list_text);
        list_text.clear();
        p.heading_level = HeadingLevel();
        if (!lt.empty() || in_list) {
            p.list_kind = !lt.empty() && std::isdigit(static_cast<unsigned char>(lt[0])) ? DocParagraph::ListKind::Numbered : DocParagraph::ListKind::Bullet;
        }
        if (p.heading_level > 0) {
            // A heading's bold and size are the heading's own, not the
            // text's: a look every run shares is the style's.
            bool all_bold = !p.spans.empty(), same_size = !p.spans.empty();
            int covered = 0;
            for (const DocSpan &s : p.spans) {
                all_bold = all_bold && s.fmt.bold;
                same_size = same_size && s.fmt.font_size_pt == p.spans[0].fmt.font_size_pt;
                covered += s.end - s.start;
            }
            if (covered == static_cast<int>(p.text.size())) {
                for (DocSpan &s : p.spans) {
                    if (all_bold) s.fmt.bold = false;
                    if (same_size) s.fmt.font_size_pt = 0.0f;
                }
                std::vector<DocSpan> kept;
                for (const DocSpan &s : p.spans)
                    if (s.fmt != DocFormat{}) kept.push_back(s);
                p.spans = std::move(kept);
            }
        }
        out->paragraphs.push_back(std::move(p));
    }

    void EndPict() {
        std::string bytes;
        int hi = -1;
        for (char ch : pict_hex) {
            int v = -1;
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
            if (v < 0) continue;
            if (hi < 0) {
                hi = v;
            } else {
                bytes += static_cast<char>(hi * 16 + v);
                hi = -1;
            }
        }
        pict_hex.clear();
        pict_ext.clear();
        if (bytes.empty() || intbl) return;  // (a picture in a cell has nowhere to go: cells are text)
        ImageDoc probe;
        if (!probe.LoadFromMemory(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size())) return;
        DocImage img;
        img.bytes = std::move(bytes);
        img.natural_w = probe.Width();
        img.natural_h = probe.Height();
        out->images.push_back(std::move(img));
        const int ref = static_cast<int>(out->images.size()) - 1;
        // On the paragraph it sits in; a paragraph with a picture already
        // ends there, and the next one carries this picture.
        if (cur.image_ref >= 0) EndPara();
        cur.image_ref = ref;
    }

    void Word(const std::string &w, bool has_param, int param) {
        const bool on = !has_param || param != 0;
        if (st.dest == Pict) {
            if (w == "pngblip") pict_ext = "png";
            else if (w == "jpegblip") pict_ext = "jpg";
            else if (w == "bin") st.dest = Skip;  // binary picture data: not read
            return;
        }
        if (w == "par") {
            if (st.dest == Normal) EndPara();
        } else if (w == "line") {
            if (st.dest == Normal) {
                cur.text += '\n';
                cur_has_text = true;
            }
        } else if (w == "tab") {
            if (st.dest == Normal) {
                cur.text += '\t';
                cur_has_text = true;
            }
        } else if (w == "page" || w == "sect") {
            if (st.dest == Normal) EndPara();
        } else if (w == "cell") {
            if (st.dest != Normal) return;
            row.push_back(cur.text);
            cur = DocParagraph();
            cur_has_text = false;
        } else if (w == "row") {
            if (st.dest != Normal) return;
            rows.push_back(row);
            row.clear();
            cur = DocParagraph();
            cur_has_text = false;
        } else if (w == "intbl" || w == "trowd") {
            intbl = true;
        } else if (w == "pard") {
            if (st.dest == Normal) {
                intbl = false;
                outline = -1;
                style = 0;
                in_list = false;
                cur.align = DocParagraph::Align::Left;
            }
        } else if (w == "plain") {
            const Dest d = st.dest;
            st = State();
            st.dest = d;
            st.font = deff;
            st.fs = default_fs;
        } else if (w == "b") st.b = on;
        else if (w == "i") st.i = on;
        else if (w == "ul" || w == "uld" || w == "ulw" || w == "uldb" || w == "ulth") st.ul = on;
        else if (w == "ulnone") st.ul = false;
        else if (w == "strike" || w == "striked") st.strike = on;
        else if (w == "super") {
            st.super = on;
            st.sub = false;
        } else if (w == "sub") {
            st.sub = on;
            st.super = false;
        } else if (w == "nosupersub") st.super = st.sub = false;
        else if (w == "fs") {
            if (st.dest == StyleSheet) {
                if (style_cur == 0) default_fs = param;
            } else {
                st.fs = param;
            }
        } else if (w == "cf") st.cf = param;
        else if (w == "highlight" || w == "cb") st.highlight = param;
        else if (w == "f") {
            if (st.dest == FontTbl) font_cur = param;
            else st.font = param;
        } else if (w == "deff") {
            deff = param;
            st.font = param;
        } else if (w == "s") {
            if (st.dest == StyleSheet) style_cur = param;
            else if (st.dest == Normal) style = param;
        } else if (w == "outlinelevel") {
            if (st.dest == Normal) outline = param;
        } else if (w == "ls") {
            if (st.dest == Normal) in_list = true;
        } else if (w == "ql") cur.align = DocParagraph::Align::Left;
        else if (w == "qc") cur.align = DocParagraph::Align::Center;
        else if (w == "qr") cur.align = DocParagraph::Align::Right;
        else if (w == "qj") cur.align = DocParagraph::Align::Justify;
        else if (w == "uc") uc = param;
        else if (w == "red" || w == "green" || w == "blue") {
            if (st.dest == ColorTbl) {
                color_cur.is_auto = false;
                color_seen = true;
                (w == "red" ? color_cur.r : w == "green" ? color_cur.g : color_cur.b) = static_cast<unsigned char>(param & 255);
            }
        } else if (w == "fonttbl") st.dest = FontTbl;
        else if (w == "colortbl") st.dest = ColorTbl;
        else if (w == "stylesheet") st.dest = StyleSheet;
        else if (w == "info" || w == "footnote" || w == "header" || w == "footer" || w == "headerl" || w == "headerr" || w == "footerl" ||
                 w == "footerr" || w == "object" || w == "themedata" || w == "colorschememapping" || w == "latentstyles" || w == "datastore" ||
                 w == "xmlnstbl" || w == "listtable" || w == "listoverridetable" || w == "rsidtbl" || w == "generator" || w == "pgdsctbl" ||
                 w == "nonshppict" || w == "pn" || w == "pnseclvl" || w == "userprops" || w == "bkmkstart" || w == "bkmkend" || w == "picprop" ||
                 w == "xe" || w == "tc" || w == "atnid" || w == "annotation") {
            st.dest = Skip;
        } else if (w == "fldinst") st.dest = FldInst;
        else if (w == "fldrslt") st.dest = Normal;
        else if (w == "listtext" || w == "pntext") {
            st.dest = ListText;
            list_text.clear();
        } else if (w == "pict") {
            st.dest = Pict;
            pict_hex.clear();
        } else if (w == "emdash") AddText("\xe2\x80\x94");
        else if (w == "endash") AddText("\xe2\x80\x93");
        else if (w == "bullet") AddText("\xe2\x80\xa2");
        else if (w == "lquote") AddText("\xe2\x80\x98");
        else if (w == "rquote") AddText("\xe2\x80\x99");
        else if (w == "ldblquote") AddText("\xe2\x80\x9c");
        else if (w == "rdblquote") AddText("\xe2\x80\x9d");
        else if (w == "enspace" || w == "emspace" || w == "qmspace") AddText(" ");
    }

    void EndGroup(Dest ending, const State &parent) {
        std::string name = TrimStr(dest_text);
        switch (ending) {
            case FontTbl:
                if (!name.empty()) {
                    if (name.back() == ';') name.pop_back();
                    fonts[font_cur] = TrimStr(name);
                    dest_text.clear();
                }
                break;
            case StyleSheet:
                if (!name.empty() && parent.dest == StyleSheet) {
                    if (name.back() == ';') name.pop_back();
                    styles[style_cur] = LowerStr(TrimStr(name));
                    dest_text.clear();
                }
                break;
            case Pict:
                if (parent.dest != Pict) EndPict();
                break;
            case ColorTbl:
                // A table that ends without its last `;`.
                if (parent.dest != ColorTbl && color_seen) {
                    colors.push_back(color_cur);
                    color_cur = RtfColor();
                    color_seen = false;
                }
                break;
            default: break;
        }
    }

    void Read(const std::string &rtf) {
        size_t i = 0;
        const size_t n = rtf.size();
        int skip_chars = 0;
        while (i < n) {
            const char c = rtf[i];
            if (c == '{') {
                stack.push_back(st);
                if (st.dest == StyleSheet) style_cur = 0;  // a style with no \sN is Normal
                ++i;
                // {\* ...}: a destination the reader may not know; only
                // the known ones are read.
                if (i + 1 < n && rtf[i] == '\\' && rtf[i + 1] == '*') {
                    size_t j = i + 2;
                    while (j < n && std::isspace(static_cast<unsigned char>(rtf[j]))) ++j;
                    std::string w;
                    if (j < n && rtf[j] == '\\') {
                        size_t k = j + 1;
                        while (k < n && std::isalpha(static_cast<unsigned char>(rtf[k]))) w += rtf[k++];
                    }
                    if (w != "shppict") st.dest = Skip;
                    i += 2;
                }
                continue;
            }
            if (c == '}') {
                const Dest ending = st.dest;
                const State parent = stack.empty() ? State() : stack.back();
                if (!stack.empty()) stack.pop_back();
                EndGroup(ending, parent);
                st = parent;
                ++i;
                continue;
            }
            if (c == '\\') {
                ++i;
                if (i >= n) break;
                const char d = rtf[i];
                if (d == '\\' || d == '{' || d == '}') {
                    if (skip_chars > 0) --skip_chars;
                    else AddText(std::string(1, d));
                    ++i;
                    continue;
                }
                if (d == '\'') {
                    const unsigned v = static_cast<unsigned>(std::strtoul(rtf.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 3;
                    if (skip_chars > 0) {
                        --skip_chars;
                        continue;
                    }
                    AddText(Utf8Of(Cp1252Of(static_cast<unsigned char>(v))));
                    continue;
                }
                if (d == '~') {
                    AddText("\xc2\xa0");
                    ++i;
                    continue;
                }
                if (d == '_') {
                    AddText("\xe2\x80\x91");
                    ++i;
                    continue;
                }
                if (d == '-' || d == '*' || d == ':' || d == '|') {
                    ++i;
                    continue;
                }
                if (d == '\n' || d == '\r') {
                    Word("par", false, 0);
                    ++i;
                    continue;
                }
                if (!std::isalpha(static_cast<unsigned char>(d))) {
                    ++i;
                    continue;
                }
                std::string w;
                while (i < n && std::isalpha(static_cast<unsigned char>(rtf[i]))) w += rtf[i++];
                bool has_param = false;
                int param = 0;
                bool neg = false;
                if (i < n && rtf[i] == '-') {
                    neg = true;
                    ++i;
                }
                std::string num;
                while (i < n && std::isdigit(static_cast<unsigned char>(rtf[i]))) num += rtf[i++];
                if (!num.empty()) {
                    has_param = true;
                    param = std::atoi(num.substr(0, 9).c_str()) * (neg ? -1 : 1);
                }
                if (i < n && rtf[i] == ' ') ++i;  // the delimiter space
                if (w == "u" && has_param) {
                    AddText(Utf8Of(static_cast<unsigned>(param < 0 ? param + 65536 : param)));
                    skip_chars = uc;
                    continue;
                }
                Word(w, has_param, param);
                continue;
            }
            if (c == '\n' || c == '\r') {
                ++i;
                continue;
            }
            if (st.dest == ColorTbl && c == ';') {
                colors.push_back(color_cur);
                color_cur = RtfColor();
                color_seen = false;
                ++i;
                continue;
            }
            if (skip_chars > 0) {
                --skip_chars;
                ++i;
                continue;
            }
            // A plain text run.
            size_t j = i;
            while (j < n && rtf[j] != '\\' && rtf[j] != '{' && rtf[j] != '}' && rtf[j] != '\n' && rtf[j] != '\r' &&
                   !(st.dest == ColorTbl && rtf[j] == ';'))
                ++j;
            std::string utf;
            for (size_t k = i; k < j; ++k) utf += Utf8Of(Cp1252Of(static_cast<unsigned char>(rtf[k])));
            AddText(utf);
            i = j;
        }
        intbl = false;
        // Text after the last \par is a paragraph too; a document that ends
        // on \par has an empty one, as Word shows it.
        if (cur_has_text || !cur.text.empty() || cur.image_ref >= 0 || !rows.empty() || !row.empty() || out->paragraphs.empty()) EndPara();
        FlushTable();
    }
};

// --- Writing ---------------------------------------------------------------

// Text with RTF's three specials escaped and non-ASCII as \uN? -- the
// same encoding mepml's RTF export uses, so every RTF mep writes reads the
// same way everywhere.
std::string RtfEsc(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c == '\\' || c == '{' || c == '}') o += '\\';
            if (c == '\n') o += "\\line ";
            else if (c == '\t') o += "\\tab ";
            else o += static_cast<char>(c);
            ++i;
            continue;
        }
        const int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        std::uint32_t cp = c & (n == 4 ? 0x07u : n == 3 ? 0x0Fu : 0x1Fu);
        for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3Fu);
        i += static_cast<size_t>(n);
        if (cp > 0xFFFF) {
            // Outside the BMP: a surrogate pair, as Word writes one.
            cp -= 0x10000;
            const int hi = 0xD800 + static_cast<int>(cp >> 10), lo = 0xDC00 + static_cast<int>(cp & 0x3FF);
            o += "\\u" + std::to_string(hi - 0x10000) + "?\\u" + std::to_string(lo - 0x10000) + "?";
        } else {
            o += "\\u" + std::to_string(cp > 0x7FFF ? static_cast<int>(cp) - 0x10000 : static_cast<int>(cp)) + "?";
        }
    }
    return o;
}

struct RtfWriter {
    const OfficeDoc &doc;
    std::vector<std::uint32_t> colors;  // index + 1 = \cfN; 0 is auto

    int ColorIndex(unsigned char r, unsigned char g, unsigned char b) {
        const std::uint32_t rgb = (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
        for (size_t i = 0; i < colors.size(); ++i)
            if (colors[i] == rgb) return static_cast<int>(i) + 1;
        colors.push_back(rgb);
        return static_cast<int>(colors.size());
    }

    std::string Props(const DocFormat &f) {
        std::string o;
        if (f.bold) o += "\\b";
        if (f.italic) o += "\\i";
        if (f.underline) o += "\\ul";
        if (f.strike) o += "\\strike";
        if (f.superscript) o += "\\super";
        if (f.subscript) o += "\\sub";
        if (f.font_family == OfficeFontFamily::Serif) o += "\\f1";
        else if (f.font_family == OfficeFontFamily::Mono) o += "\\f2";
        if (f.font_size_pt > 0.0f) o += "\\fs" + std::to_string(static_cast<int>(f.font_size_pt * 2.0f + 0.5f));
        if (f.has_color) o += "\\cf" + std::to_string(ColorIndex(f.color_r, f.color_g, f.color_b));
        if (f.has_highlight) o += "\\highlight" + std::to_string(ColorIndex(f.highlight_r, f.highlight_g, f.highlight_b));
        return o;
    }

    // The paragraph's text, each span in a group of its own look; text
    // outside every span is the paragraph's plain text.
    std::string Runs(const DocParagraph &p) {
        std::string o;
        int pos = 0;
        const int len = static_cast<int>(p.text.size());
        for (const DocSpan &s : p.spans) {
            const int a = std::max(pos, std::min(len, s.start)), b = std::max(a, std::min(len, s.end));
            if (a > pos) o += RtfEsc(p.text.substr(static_cast<size_t>(pos), static_cast<size_t>(a - pos)));
            if (b > a) {
                const std::string props = Props(s.fmt);
                const std::string text = RtfEsc(p.text.substr(static_cast<size_t>(a), static_cast<size_t>(b - a)));
                o += props.empty() ? text : "{" + props + " " + text + "}";
            }
            pos = b;
        }
        if (pos < len) o += RtfEsc(p.text.substr(static_cast<size_t>(pos)));
        return o;
    }

    std::string Table(const DocTable &t) {
        if (t.rows <= 0 || t.cols <= 0) return "";
        std::string o;
        const int cw = 8640 / t.cols;
        for (int r = 0; r < t.rows; ++r) {
            o += "\\trowd\\trgaph108";
            for (int c = 0; c < t.cols; ++c) o += "\\clbrdrt\\brdrs\\clbrdrl\\brdrs\\clbrdrb\\brdrs\\clbrdrr\\brdrs\\cellx" + std::to_string(cw * (c + 1));
            o += "\n";
            for (int c = 0; c < t.cols; ++c) o += "\\pard\\plain\\intbl\\fs22 " + RtfEsc(t.Cell(r, c)) + "\\cell\n";
            o += "\\row\n";
        }
        return o;
    }

    std::string Picture(const DocImage &img) {
        const std::string ext = SniffImageExtension(img.bytes);
        // RTF carries PNG and JPEG as they are (\pngblip, \jpegblip); the
        // other encodings have no blip of their own and are left out.
        if (ext != "png" && ext != "jpeg" && ext != "jpg") return "";
        const int w = std::max(1, img.natural_w), h = std::max(1, img.natural_h);
        // At most 6 inches wide (8640 twips); 15 twips per pixel at 96 dpi.
        const double scale = std::min(1.0, 8640.0 / (w * 15.0));
        std::string hex;
        static const char *digits = "0123456789abcdef";
        for (size_t i = 0; i < img.bytes.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(img.bytes[i]);
            hex += digits[c >> 4];
            hex += digits[c & 15];
            if (i % 64 == 63) hex += '\n';
        }
        return "{\\pard\\plain\\qc {\\pict\\" + std::string(ext == "png" ? "pngblip" : "jpegblip") + "\\picw" + std::to_string(w) + "\\pich" +
               std::to_string(h) + "\\picwgoal" + std::to_string(static_cast<int>(w * 15 * scale)) + "\\pichgoal" +
               std::to_string(static_cast<int>(h * 15 * scale)) + "\n" + hex + "}\\par}\n";
    }

    std::string Write() {
        static const int kHeadSize[] = {36, 32, 28, 26, 24, 24};
        std::string body;
        int numbered = 0;  // position within the current run of numbered paragraphs
        for (const DocParagraph &p : doc.paragraphs) {
            if (p.list_kind != DocParagraph::ListKind::Numbered) numbered = 0;
            std::string props = "\\pard\\plain\\sa160\\fs22";
            const int lvl = std::min(6, std::max(0, p.heading_level));
            if (lvl > 0)
                props += "\\s" + std::to_string(lvl) + "\\sb240\\keepn\\b\\outlinelevel" + std::to_string(lvl - 1) + "\\fs" + std::to_string(kHeadSize[lvl - 1]);
            switch (p.align) {
                case DocParagraph::Align::Center: props += "\\qc"; break;
                case DocParagraph::Align::Right: props += "\\qr"; break;
                case DocParagraph::Align::Justify: props += "\\qj"; break;
                case DocParagraph::Align::Left: break;
            }
            std::string marker;
            if (p.list_kind == DocParagraph::ListKind::Bullet) {
                // \pn for readers that number for themselves, \pntext (the
                // marker as text) for those that do not -- and for mep.
                props += "\\fi-360\\li720";
                marker = "{\\*\\pn\\pnlvlblt\\pnf0\\pnindent360{\\pntxtb\\bullet}}{\\pntext\\bullet\\tab}";
            } else if (p.list_kind == DocParagraph::ListKind::Numbered) {
                ++numbered;
                props += "\\fi-360\\li720";
                marker = "{\\*\\pn\\pnlvlbody\\pndec\\pnstart1\\pnindent360{\\pntxta.}}{\\pntext " + std::to_string(numbered) + ".\\tab}";
            }
            const bool anchor_only = p.text.empty() && p.spans.empty() && (p.table_ref >= 0 || p.image_ref >= 0);
            if (!anchor_only) body += "{" + props + " " + marker + Runs(p) + "\\par}\n";
            if (p.table_ref >= 0 && p.table_ref < static_cast<int>(doc.tables.size())) body += Table(doc.tables[static_cast<size_t>(p.table_ref)]);
            if (p.image_ref >= 0 && p.image_ref < static_cast<int>(doc.images.size())) body += Picture(doc.images[static_cast<size_t>(p.image_ref)]);
        }
        std::string head = "{\\rtf1\\ansi\\ansicpg1252\\deff0\\deflang1033\n";
        head += "{\\fonttbl{\\f0\\fswiss\\fcharset0 Liberation Sans;}{\\f1\\froman\\fcharset0 Liberation Serif;}{\\f2\\fmodern\\fcharset0 Liberation Mono;}}\n";
        head += "{\\colortbl;";
        for (std::uint32_t c : colors)
            head += "\\red" + std::to_string((c >> 16) & 255) + "\\green" + std::to_string((c >> 8) & 255) + "\\blue" + std::to_string(c & 255) + ";";
        head += "}\n";
        head += "{\\stylesheet{\\s0\\fs22 Normal;}";
        for (int l = 1; l <= 6; ++l)
            head += "{\\s" + std::to_string(l) + "\\sb240\\keepn\\b\\outlinelevel" + std::to_string(l - 1) + "\\fs" + std::to_string(kHeadSize[l - 1]) +
                    " heading " + std::to_string(l) + ";}";
        head += "}\n";
        return head + body + "}\n";
    }
};

}  // namespace

bool IsRtfPath(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    return LowerStr(path.substr(dot + 1)) == "rtf";
}

bool LoadRtfFromMemory(const unsigned char *bytes, size_t len, OfficeDoc &out, std::string &error) {
    const std::string text(reinterpret_cast<const char *>(bytes), len);
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    if (text.compare(start, 5, "{\\rtf") != 0) {
        error = "not an RTF file (no {\\rtf header)";
        return false;
    }
    out = OfficeDoc();
    out.source_format = "rtf";
    RtfReader r;
    r.out = &out;
    r.Read(text);
    if (out.paragraphs.empty()) out.paragraphs.push_back(DocParagraph{});  // never a zero-paragraph document
    return true;
}

bool SaveRtfToMemory(const OfficeDoc &doc, std::vector<unsigned char> &out, std::string &error) {
    RtfWriter w{doc, {}};
    const std::string text = w.Write();
    if (text.empty()) {
        error = "nothing to write";
        return false;
    }
    out.assign(text.begin(), text.end());
    return true;
}
