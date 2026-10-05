// PowerPoint (.pptx) and LibreOffice Impress (.odp) exports of a mepml
// document's slides (mepml_convert.h: WritePptx, WriteOdp).
//
// Both are built from one format-neutral model: each \slide becomes a
// DeckSlide of text, table and picture items, laid out once on a 16:9 page
// (Layout) -- a title across the top, the items stacked below it, or text
// beside a single picture -- with font sizes shrunk until the estimated
// content fits. The two writers only turn the placed shapes into their own
// XML. A title slide comes first when the header has a Title, Subtitle or
// Author. Maths is set as each format's own: a display equation is an
// Office equation (OMML) in the .pptx and a formula object (MathML) in the
// .odp; inline maths is an equation in the .pptx and, since an Impress
// text box cannot hold a formula, typeset text in the .odp. A .pptx reader
// that does not know Office equations gets that same text (math_markup.h).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>

#include "math_markup.h"
#include "mepml_convert.h"
#include "zip_archive.h"

namespace mepml {

namespace {

MathPictureRenderer &MathRenderer() {
    static MathPictureRenderer renderer;
    return renderer;
}

// --- The deck ------------------------------------------------------------------

struct Run {
    std::string text;
    bool bold = false, italic = false, underline = false, strike = false, mono = false, super = false, sub = false;
    std::string color;  // RRGGBB, "" for the default
    std::string link;
    std::string tex;  // inline maths: its TeX (`text` is its plain-text spelling)
    bool serif = false;  // maths set as text: in a serif, as maths is
    // Set where a style sheet (or an inner element of its own colour or
    // weight) has had its say: an outer element's change leaves it alone.
    bool color_set = false, bold_set = false, italic_set = false;
};

enum class ParaKind { Body, Bullet, Number, Heading, Code, Result, Caption, Math, Note, Callout, Title, Subtitle, Byline };

struct Para {
    ParaKind kind = ParaKind::Body;
    std::vector<Run> runs;
    int level = 0;   // list depth
    int number = 1;  // ParaKind::Number
    Para(ParaKind k = ParaKind::Body, std::vector<Run> r = {}) : kind(k), runs(std::move(r)) {}
};

Run Plain(std::string text) {
    Run r;
    r.text = std::move(text);
    return r;
}

// Output: a code block's results, set apart from its code (Code).
// Formula: one paragraph whose inline maths needs two dimensions, set as a
// formula of its own (text and maths together) in an Impress deck -- see
// SplitFormulaParagraphs.
enum class ItemKind { Text, Code, Output, Table, Image, Math, Formula };

struct Item {
    ItemKind kind = ItemKind::Text;
    std::vector<Para> paras;  // Text, Code
    std::vector<std::vector<std::vector<Run>>> rows;  // Table: rows of cells of runs
    int header_rows = 0;
    std::string bytes, ext, alt;  // Image
    int px_w = 0, px_h = 0;
    std::vector<Run> caption;  // Table, Image
    std::string tex;           // Math: a display equation's TeX
};

struct DeckSlide {
    std::vector<Run> title;
    std::vector<Item> items;
};

struct Deck {
    std::string title, subtitle, author, date;
    std::vector<DeckSlide> slides;
};

bool ReadBinary(const std::string &path, std::string *out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

std::string ResolveRel(const std::string &base_dir, const std::string &path) {
    if (path.empty() || path[0] == '/' || base_dir.empty()) return path;
    std::string p = path;
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    return base_dir + "/" + p;
}

// Pixel size from a PNG or JPEG header; false for anything else.
bool ImageSize(const std::string &b, int *w, int *h, std::string *ext) {
    auto u8 = [&](size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(b[i])); };
    if (b.size() > 24 && b.compare(1, 3, "PNG") == 0) {
        *w = static_cast<int>(u8(16) << 24 | u8(17) << 16 | u8(18) << 8 | u8(19));
        *h = static_cast<int>(u8(20) << 24 | u8(21) << 16 | u8(22) << 8 | u8(23));
        *ext = "png";
        return true;
    }
    if (b.size() > 4 && u8(0) == 0xFF && u8(1) == 0xD8) {
        size_t i = 2;
        while (i + 9 < b.size()) {
            if (u8(i) != 0xFF) return false;
            const unsigned marker = u8(i + 1);
            const size_t len = u8(i + 2) << 8 | u8(i + 3);
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                *h = static_cast<int>(u8(i + 5) << 8 | u8(i + 6));
                *w = static_cast<int>(u8(i + 7) << 8 | u8(i + 8));
                *ext = "jpeg";
                return true;
            }
            i += 2 + len;
        }
    }
    return false;
}

std::string Hex(std::uint32_t rgb) {
    static const char *d = "0123456789ABCDEF";
    std::string s(6, '0');
    for (int k = 5; k >= 0; --k, rgb >>= 4) s[static_cast<size_t>(k)] = d[rgb & 0xF];
    return s;
}

// Line breaks inside a paragraph's text read as spaces.
std::string Unwrap(const std::string &t) {
    std::string o;
    bool nl = false;
    for (char c : t) {
        if (c == '\n' || c == '\r') {
            // (Even a text that is nothing but the break: between `*bold*`
            // and `$maths$` on the next line, it is the only space there.)
            if (o.empty() || o.back() != ' ') o += ' ';
            nl = true;
            continue;
        }
        if (nl && (c == ' ' || c == '\t')) continue;
        nl = false;
        o += c;
    }
    return o;
}

struct Builder {
    const Document &doc;
    std::string base_dir;
    std::vector<std::vector<Run>> notes;  // this slide's footnotes, set below its content
    ExportTextStyler styler{doc};

    // What the document's style sheets change of an element's runs
    // (out[from..]): its colour, weight and slant, where nothing inside it
    // has already said its own.
    void ApplySheet(const Inline &x, const ExportTextStyler::Change &ch, size_t from, std::vector<Run> &out) {
        const bool own_color = x.kind == InlineKind::Color || x.kind == InlineKind::Insert || x.kind == InlineKind::Delete ||
                               x.kind == InlineKind::Highlight || x.kind == InlineKind::Link;
        for (size_t i = from; i < out.size(); ++i) {
            Run &r = out[i];
            if (!r.color_set && !ch.color.empty()) r.color = ch.color;
            if (!ch.color.empty() || own_color) r.color_set = true;
            if (!r.bold_set && ch.bold >= 0) r.bold = ch.bold == 1;
            if (ch.bold >= 0 || x.kind == InlineKind::Bold) r.bold_set = true;
            if (!r.italic_set && ch.italic >= 0) r.italic = ch.italic == 1;
            if (ch.italic >= 0 || x.kind == InlineKind::Italic) r.italic_set = true;
        }
    }

    void Runs(const std::vector<Inline> &ins, Run style, std::vector<Run> &out) {
        for (const Inline &x : ins) {
            if (styler.active() && x.kind != InlineKind::Text) {
                const ExportTextStyler::Change ch = styler.Enter(x);
                const size_t from = out.size();
                Runs1(x, style, out);
                styler.Leave();
                ApplySheet(x, ch, from, out);
            } else {
                Runs1(x, style, out);
            }
        }
    }
    void Runs1(const Inline &x, Run style, std::vector<Run> &out) {
        {
            Run s = style;
            switch (x.kind) {
                case InlineKind::Text:
                    s.text = Unwrap(x.text);
                    // (A space ending the run before already separates them.)
                    if (!out.empty() && !out.back().text.empty() && out.back().text.back() == ' ')
                        s.text.erase(0, s.text.find_first_not_of(' ') == std::string::npos ? s.text.size() : s.text.find_first_not_of(' '));
                    if (!s.text.empty()) out.push_back(s);
                    break;
                case InlineKind::Bold: s.bold = true; Runs(x.children, s, out); break;
                case InlineKind::Italic: s.italic = true; Runs(x.children, s, out); break;
                case InlineKind::Underline: s.underline = true; Runs(x.children, s, out); break;
                case InlineKind::Superscript: s.super = true; Runs(x.children, s, out); break;
                case InlineKind::Subscript: s.sub = true; Runs(x.children, s, out); break;
                case InlineKind::Mono: s.mono = true; Runs(x.children, s, out); break;
                case InlineKind::Strike: s.strike = true; Runs(x.children, s, out); break;
                case InlineKind::Highlight: s.bold = true; s.color = "9A6700"; Runs(x.children, s, out); break;
                case InlineKind::Insert: s.underline = true; s.color = "2F7D32"; Runs(x.children, s, out); break;
                case InlineKind::Delete: s.strike = true; s.color = "C62828"; Runs(x.children, s, out); break;
                case InlineKind::Small:
                case InlineKind::Big:
                case InlineKind::Font:
                case InlineKind::Class:
                case InlineKind::FontSize: Runs(x.children, s, out); break;
                case InlineKind::Color: {
                    std::uint32_t rgb = 0;
                    if (ParseColor(x.arg, &rgb)) s.color = Hex(rgb);
                    Runs(x.children, s, out);
                    break;
                }
                case InlineKind::Verbatim:
                    s.mono = true;
                    s.text = x.text;
                    out.push_back(s);
                    break;
                case InlineKind::Link:
                    s.link = x.arg;
                    if (s.color.empty()) s.color = "0B5CAD";
                    s.underline = true;
                    if (x.children.empty()) {
                        s.text = x.arg;
                        out.push_back(s);
                    } else {
                        Runs(x.children, s, out);
                    }
                    break;
                case InlineKind::Footnote: {
                    s.super = true;
                    s.text = std::to_string(x.number);
                    out.push_back(s);
                    std::vector<Run> note{Plain(std::to_string(x.number) + " ")};
                    Runs(x.children, Run(), note);
                    notes.push_back(std::move(note));
                    break;
                }
                case InlineKind::Cite:
                case InlineKind::CiteP:
                    s.text = CiteLabel(doc, x.text, x.kind == InlineKind::CiteP);
                    out.push_back(s);
                    break;
                case InlineKind::Math:
                    s.tex = x.text;
                    s.text = TexToPlainText(x.text);
                    out.push_back(s);
                    break;
                case InlineKind::Comment:
                // A deck is built for both .pptx and .odp, so there is no one
                // markup a \raw could be written in: it is left out.
                case InlineKind::Raw: break;
                case InlineKind::Command:  // no \define: as written
                    s.text = "\\" + x.arg + "(" + x.text + ")";
                    out.push_back(s);
                    break;
            }
        }
    }
    std::vector<Run> Runs(const std::vector<Inline> &ins) {
        std::vector<Run> out;
        Runs(ins, Run(), out);
        return out;
    }

    // The text item at the end of `items`, started if the last one is not.
    static std::vector<Para> &TextItem(std::vector<Item> &items) {
        if (items.empty() || items.back().kind != ItemKind::Text) items.push_back(Item{});
        return items.back().paras;
    }

    std::vector<Run> Caption(const Block &b, const std::string &label) {
        std::vector<Run> c;
        if (b.caption_inlines.empty()) return c;
        if (!label.empty()) {
            Run l;
            l.bold = true;
            l.text = label + ": ";
            c.push_back(l);
        }
        Runs(b.caption_inlines, Run(), c);
        return c;
    }

    void Picture(std::vector<Item> &items, const std::string &path, const std::string &alt, std::vector<Run> caption) {
        Item im;
        im.kind = ItemKind::Image;
        if (!ReadBinary(ResolveRel(base_dir, path), &im.bytes) || !ImageSize(im.bytes, &im.px_w, &im.px_h, &im.ext) ||
            im.px_w <= 0 || im.px_h <= 0) {
            Para p;
            p.kind = ParaKind::Caption;
            p.runs.push_back(Plain("[picture not found: " + path + "]"));
            TextItem(items).push_back(p);
            return;
        }
        im.alt = alt;
        im.caption = std::move(caption);
        items.push_back(std::move(im));
    }

    void AddBlock(const Block &b, const std::string &label, std::vector<Item> &items) {
        switch (b.kind) {
            case BlockKind::Paragraph: TextItem(items).push_back(Para{ParaKind::Body, Runs(b.inlines)}); break;
            case BlockKind::Heading: TextItem(items).push_back(Para{ParaKind::Heading, Runs(b.inlines)}); break;
            case BlockKind::List: {
                std::vector<int> indents;
                for (const ListItem &it : b.items) indents.push_back(it.indent);
                std::sort(indents.begin(), indents.end());
                indents.erase(std::unique(indents.begin(), indents.end()), indents.end());
                for (const ListItem &it : b.items) {
                    Para p{it.ordered ? ParaKind::Number : ParaKind::Bullet, {}};
                    p.level = static_cast<int>(std::lower_bound(indents.begin(), indents.end(), it.indent) - indents.begin());
                    p.number = it.number;
                    if (it.checkbox >= 0) p.runs.push_back(Plain(it.checkbox ? "☑ " : "☐ "));
                    Runs(it.content, Run(), p.runs);
                    TextItem(items).push_back(p);
                }
                break;
            }
            case BlockKind::MathBlock: {
                Item m;
                m.kind = ItemKind::Math;
                m.tex = b.code;
                items.push_back(std::move(m));
                std::vector<Run> cap = Caption(b, label);
                if (!cap.empty()) TextItem(items).push_back(Para{ParaKind::Caption, cap});
                break;
            }
            case BlockKind::Code: {
                bool show_code = true, show_results = true;
                CodeExports(doc, b, &show_code, &show_results);
                // The code and its results: two boxes, the results under the
                // code (DrawPane's cards, as a deck can show them).
                Item code, output;
                code.kind = ItemKind::Code;
                output.kind = ItemKind::Output;
                auto lines = [&](const std::string &text, ParaKind kind) {
                    std::istringstream ss(text);
                    std::string l;
                    while (std::getline(ss, l)) {
                        Run r;
                        r.mono = true;
                        r.text = l;
                        if (kind == ParaKind::Result) r.color = "57606A";
                        (kind == ParaKind::Result ? output : code).paras.push_back(Para{kind, {r}});
                    }
                };
                if (show_code) lines(b.code, ParaKind::Code);
                std::vector<std::string> figures;
                // Markdown results are blocks of their own; HTML ones have
                // no place on a slide but their words.
                if (b.result_line_start >= 0 && show_results && b.result_format != "markdown") {
                    std::string text;
                    for (const std::string &line : b.result_lines) {
                        std::string img;
                        if (b.result_format.empty() && ResultImagePath(line, &img)) figures.push_back(img);
                        else if (b.result_format != "html") text += line + "\n";
                    }
                    if (b.result_format == "html") text = "[HTML output]\n";
                    while (!text.empty() && text.back() == '\n') text.pop_back();
                    if (!text.empty()) lines(text, ParaKind::Result);
                }
                if (!code.paras.empty()) items.push_back(std::move(code));
                if (!output.paras.empty()) items.push_back(std::move(output));
                std::vector<Run> cap = Caption(b, label);
                for (size_t k = 0; k < figures.size(); ++k)
                    Picture(items, figures[k], b.alt, k + 1 == figures.size() ? cap : std::vector<Run>());
                if (figures.empty() && !cap.empty()) TextItem(items).push_back(Para{ParaKind::Caption, cap});
                break;
            }
            case BlockKind::Image: Picture(items, b.value, b.alt, Caption(b, label)); break;
            case BlockKind::Table: {
                Item t;
                t.kind = ItemKind::Table;
                t.header_rows = b.header_rows;
                for (const auto &row : b.rows) {
                    std::vector<std::vector<Run>> cells;
                    for (const TableCell &c : row) cells.push_back(Runs(c.content));
                    t.rows.push_back(std::move(cells));
                }
                t.caption = Caption(b, label);
                if (!t.rows.empty()) items.push_back(std::move(t));
                break;
            }
            case BlockKind::Abstract:
                for (const std::vector<Inline> &para : AbstractParagraphs(b)) TextItem(items).push_back(Para{ParaKind::Body, Runs(para)});
                break;
            case BlockKind::BoxBegin: {
                // A deck has no box: the heading in the box's colour, bold.
                const BoxKind *k = FindBoxKind(b.keyword);
                Run kind_run;
                kind_run.bold = true;
                // (Its label and colour are the style sheets' where they say.)
                const BoxLook look = ExportBoxLook(doc, b.keyword);
                kind_run.color = k ? look.color.substr(1) : "2C7FB8";
                kind_run.text = look.label + (b.caption_inlines.empty() ? "" : ": ");
                Para p{ParaKind::Body, {kind_run}};
                Run title_style;
                title_style.bold = true;
                if (!b.caption_inlines.empty()) Runs(b.caption_inlines, title_style, p.runs);
                TextItem(items).push_back(p);
                if (!b.inlines.empty()) TextItem(items).push_back(Para{ParaKind::Body, Runs(b.inlines)});
                break;
            }
            case BlockKind::Bibliography: {
                int n = 0;
                for (const std::string &key : doc.cite_order) {
                    const BibEntryParts e = BibEntry(doc.citations.at(key));
                    Para p{ParaKind::Body, {Plain("[" + std::to_string(++n) + "] " + e.lead)}};
                    Run t;
                    t.italic = true;
                    t.text = e.title;
                    if (!t.text.empty()) p.runs.push_back(t);
                    p.runs.push_back(Plain(e.rest));
                    TextItem(items).push_back(p);
                }
                break;
            }
            case BlockKind::Callout:  // (a comment: the author's own note)
            case BlockKind::Comment:
            case BlockKind::Meta:
            case BlockKind::Import:
            case BlockKind::Citation:
            case BlockKind::Rule:
            case BlockKind::TableOfContents:
            case BlockKind::SlideBegin:
            case BlockKind::SlideEnd:
            case BlockKind::BoxEnd:
            case BlockKind::LayoutBegin:  // (a picture beside text is set in two columns anyway: Layout)
            case BlockKind::LayoutEnd:
            case BlockKind::Define:
            case BlockKind::Raw: break;  // see Runs: no markup of its own
            case BlockKind::Command: {
                Run r;
                r.text = b.text;
                TextItem(items).push_back(Para{ParaKind::Body, {r}});
                break;
            }
        }
    }

    Deck Build() {
        Deck d;
        d.title = doc.title;
        d.subtitle = MetaValue(doc, "subtitle");
        d.author = MetaValue(doc, "author");
        d.date = MetaValue(doc, "date");
        const std::vector<std::string> labels = BlockLabels(doc);
        const std::vector<bool> hidden = ExportHidden(doc);
        for (const Slide &sl : Slides(doc, 0)) {
            DeckSlide s;
            notes.clear();
            bool titled = false;
            for (size_t i = sl.first_block; i < sl.last_block; ++i) {
                const Block &b = doc.blocks[i];
                if (hidden[i]) continue;
                if (!titled && b.kind == BlockKind::Heading) {
                    s.title = Runs(b.inlines);
                    titled = true;
                    continue;
                }
                AddBlock(b, labels[i], s.items);
            }
            for (std::vector<Run> &n : notes) TextItem(s.items).push_back(Para{ParaKind::Note, std::move(n)});
            d.slides.push_back(std::move(s));
        }
        return d;
    }
};

// --- Layout ---------------------------------------------------------------------
//
// In EMU (914400 to the inch) on a 13.333 x 7.5 in page. Heights are
// estimates -- no fonts are measured -- from a character's typical width.

constexpr long kEmuIn = 914400;
constexpr long kPageW = 12192000, kPageH = 6858000;
constexpr long kMarginX = kEmuIn * 55 / 100;
constexpr long kGap = kEmuIn * 12 / 100;
// A code block's results are marked by a bar this wide down their left
// edge, in the theme's accent1 (the editor's own output cards are set
// apart from the code the same way: another ground, no border).
constexpr long kOutputBarW = kEmuIn * 5 / 100;
constexpr const char *kOutputAccent = "6B8AFD";

double BasePt(ParaKind k) {
    switch (k) {
        case ParaKind::Heading: return 24;
        case ParaKind::Code:
        case ParaKind::Result: return 15;
        case ParaKind::Caption: return 14;
        case ParaKind::Note: return 12;
        case ParaKind::Math: return 22;
        case ParaKind::Title: return 40;
        case ParaKind::Subtitle: return 24;
        case ParaKind::Byline: return 20;
        default: return 22;
    }
}
constexpr double kTablePt = 16, kSlideTitlePt = 32;

int Chars(const std::vector<Run> &runs) {
    int n = 0;
    for (const Run &r : runs)
        for (char c : r.text) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// Height of `text` characters at `pt` in a box `width` wide.
long TextHeight(int chars, double pt, bool mono, long width, double spacing) {
    const double char_in = pt / 72.0 * (mono ? 0.6 : 0.5);
    const double line_chars = std::max(1.0, (static_cast<double>(width) / kEmuIn - 0.2) / char_in);
    const double lines = std::max(1.0, std::ceil(chars / line_chars));
    return static_cast<long>((lines * pt * 1.2 + pt * spacing) / 72.0 * kEmuIn);
}

bool MonoKind(ParaKind k) { return k == ParaKind::Code || k == ParaKind::Result; }

// Space above a paragraph, in ems: list items sit closer together.
double ParaSpacing(ParaKind k) {
    if (MonoKind(k)) return 0.0;
    return k == ParaKind::Bullet || k == ParaKind::Number ? 0.15 : 0.35;
}

long ParasHeight(const std::vector<Para> &ps, long width, double scale) {
    long h = kEmuIn / 10;  // insets
    for (const Para &p : ps)
        h += TextHeight(Chars(p.runs) + 3 * p.level, BasePt(p.kind) * scale, MonoKind(p.kind), width, ParaSpacing(p.kind));
    return h;
}

double SansEm(const std::string &text, bool bold, bool italic, bool mono);

// Column widths of a table, from its cells' widest text -- measured in the
// face the cells are set in, header rows bold, so that a header like
// "Estimate" is not a hair too narrow and broken over two lines (which
// makes the table taller than it was placed for).
std::vector<long> TableColumns(const Item &t, long width, double scale) {
    size_t cols = 0;
    for (const auto &row : t.rows) cols = std::max(cols, row.size());
    std::vector<double> want(cols, 1.0);
    for (size_t r = 0; r < t.rows.size(); ++r) {
        const bool head = static_cast<int>(r) < t.header_rows;
        for (size_t c = 0; c < t.rows[r].size(); ++c) {
            double em = 0;
            for (const Run &run : t.rows[r][c]) em += SansEm(run.text, run.bold || head, run.italic, run.mono);
            want[c] = std::max(want[c], em);
        }
    }
    const double em_emu = kTablePt * scale / 72.0 * kEmuIn;
    double natural = 0;
    // (The cell's own margins, 0.15 in, and a little slack.)
    for (double &w : want) natural += w = w * em_emu + kEmuIn * 0.25;
    const double k = std::min(1.0, static_cast<double>(width) / natural);
    std::vector<long> out;
    for (double w : want) out.push_back(static_cast<long>(w * k));
    return out;
}

long TableHeight(const Item &t, long width, double scale) {
    const std::vector<long> cols = TableColumns(t, width, scale);
    long h = 0;
    for (const auto &row : t.rows) {
        long rh = 0;
        for (size_t c = 0; c < row.size() && c < cols.size(); ++c)
            rh = std::max(rh, TextHeight(Chars(row[c]), kTablePt * scale, false, cols[c], 0.0));
        h += rh + kEmuIn / 10;
    }
    return h;
}

// A display equation's type scale: the slide's, or smaller when the
// equation would be wider than the box.
double MathScale(const std::string &tex, long width, double scale) {
    double wem = 0, hem = 0;
    TexMathExtent(tex, true, &wem, &hem);
    const double pt = BasePt(ParaKind::Math) * scale;
    const double width_in = wem * pt / 72.0, avail = static_cast<double>(width) / kEmuIn - 0.3;
    return width_in > avail && width_in > 0 ? scale * avail / width_in : scale;
}

long MathHeight(const std::string &tex, long width, double scale) {
    double wem = 0, hem = 0;
    TexMathExtent(tex, true, &wem, &hem);
    const double pt = BasePt(ParaKind::Math) * MathScale(tex, width, scale);
    return static_cast<long>((hem * pt + pt * 0.5) / 72.0 * kEmuIn) + kEmuIn / 10;
}

// --- A paragraph as a formula ------------------------------------------------------
//
// Impress text cannot hold a formula, so a paragraph whose inline maths
// needs two dimensions (TexNeedsLayout) becomes one formula object: its
// words and its maths set together by LibreOffice Math, from a StarMath
// annotation (the only way to give a formula's text the body's face).
// Formulas do not wrap, so the lines are broken here, from the real
// advance widths of that face; and since Impress stretches a formula to
// its frame, the frame is sized from the same measurements.

// Liberation Sans advance widths, in thousandths of an em, for U+0020..U+007E
// then U+00A0..U+00FF (regular, bold, italic, bold italic) -- the face
// Impress sets a formula's sans text in (tools: src/gfx/truetype.h).
const unsigned short kSansWidths[4][191] = {
    {277, 277, 354, 556, 556, 889, 666, 190, 333, 333, 389, 583, 277, 333, 277, 277, 556, 556, 556, 556, 556, 556, 556, 556,
     556, 556, 277, 277, 583, 583, 583, 556, 1015, 666, 666, 722, 722, 666, 610, 777, 722, 277, 500, 666, 556, 833, 722, 777,
     666, 777, 722, 666, 610, 722, 666, 943, 666, 666, 610, 277, 277, 277, 469, 556, 333, 556, 556, 500, 556, 556, 277, 556,
     556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 277, 556, 500, 722, 500, 500, 500, 333, 259, 333, 583, 277,
     333, 556, 556, 556, 556, 259, 556, 333, 736, 370, 556, 583, 333, 736, 552, 399, 548, 333, 333, 333, 576, 537, 333, 333,
     333, 365, 556, 833, 833, 833, 610, 666, 666, 666, 666, 666, 666, 1000, 722, 666, 666, 666, 666, 277, 277, 277, 277, 722,
     722, 777, 777, 777, 777, 777, 583, 777, 722, 722, 722, 722, 666, 666, 610, 556, 556, 556, 556, 556, 556, 889, 500, 556,
     556, 556, 556, 277, 277, 277, 277, 556, 556, 556, 556, 556, 556, 556, 548, 610, 556, 556, 556, 556, 500, 556, 500},  // regular
    {277, 333, 474, 556, 556, 889, 722, 237, 333, 333, 389, 583, 277, 333, 277, 277, 556, 556, 556, 556, 556, 556, 556, 556,
     556, 556, 333, 333, 583, 583, 583, 610, 975, 722, 722, 722, 722, 666, 610, 777, 722, 277, 556, 722, 610, 833, 722, 777,
     666, 777, 722, 666, 610, 722, 666, 943, 666, 666, 610, 333, 277, 333, 583, 556, 333, 556, 610, 556, 610, 556, 333, 610,
     610, 277, 277, 556, 277, 889, 610, 610, 610, 610, 389, 556, 333, 610, 556, 777, 556, 556, 500, 389, 279, 389, 583, 277,
     333, 556, 556, 556, 556, 279, 556, 333, 736, 370, 556, 583, 333, 736, 552, 399, 548, 333, 333, 333, 576, 556, 333, 333,
     333, 365, 556, 833, 833, 833, 610, 722, 722, 722, 722, 722, 722, 1000, 722, 666, 666, 666, 666, 277, 277, 277, 277, 722,
     722, 777, 777, 777, 777, 777, 583, 777, 722, 722, 722, 722, 666, 666, 610, 556, 556, 556, 556, 556, 556, 889, 556, 556,
     556, 556, 556, 277, 277, 277, 277, 610, 610, 610, 610, 610, 610, 610, 548, 610, 610, 610, 610, 610, 556, 610, 556},  // bold
    {277, 277, 354, 556, 556, 889, 666, 190, 333, 333, 389, 583, 277, 333, 277, 277, 556, 556, 556, 556, 556, 556, 556, 556,
     556, 556, 277, 277, 583, 583, 583, 556, 1015, 666, 666, 722, 722, 666, 610, 777, 722, 277, 500, 666, 556, 833, 722, 777,
     666, 777, 722, 666, 610, 722, 666, 943, 666, 666, 610, 277, 277, 277, 469, 556, 333, 556, 556, 500, 556, 556, 277, 556,
     556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 277, 556, 500, 722, 500, 500, 500, 333, 259, 333, 583, 277,
     333, 556, 556, 556, 556, 259, 556, 333, 736, 370, 556, 583, 333, 736, 552, 399, 548, 333, 333, 333, 576, 537, 333, 333,
     333, 365, 556, 833, 833, 833, 610, 666, 666, 666, 666, 666, 666, 1000, 722, 666, 666, 666, 666, 277, 277, 277, 277, 722,
     722, 777, 777, 777, 777, 777, 583, 777, 722, 722, 722, 722, 666, 666, 610, 556, 556, 556, 556, 556, 556, 889, 500, 556,
     556, 556, 556, 277, 277, 277, 277, 556, 556, 556, 556, 556, 556, 556, 548, 610, 556, 556, 556, 556, 500, 556, 500},  // italic
    {277, 333, 474, 556, 556, 889, 722, 237, 333, 333, 389, 583, 277, 333, 277, 277, 556, 556, 556, 556, 556, 556, 556, 556,
     556, 556, 333, 333, 583, 583, 583, 610, 975, 722, 722, 722, 722, 666, 610, 777, 722, 277, 556, 722, 610, 833, 722, 777,
     666, 777, 722, 666, 610, 722, 666, 943, 666, 666, 610, 333, 277, 333, 583, 556, 333, 556, 610, 556, 610, 556, 333, 610,
     610, 277, 277, 556, 277, 889, 610, 610, 610, 610, 389, 556, 333, 610, 556, 777, 556, 556, 500, 389, 279, 389, 583, 277,
     333, 556, 556, 556, 556, 279, 556, 333, 736, 370, 556, 583, 333, 736, 552, 399, 548, 333, 333, 333, 576, 556, 333, 333,
     333, 365, 556, 833, 833, 833, 610, 722, 722, 722, 722, 722, 722, 1000, 722, 666, 666, 666, 666, 277, 277, 277, 277, 722,
     722, 777, 777, 777, 777, 777, 583, 777, 722, 722, 722, 722, 666, 666, 610, 556, 556, 556, 556, 556, 556, 889, 556, 556,
     556, 556, 556, 277, 277, 277, 277, 610, 610, 610, 610, 610, 610, 610, 548, 610, 610, 610, 610, 610, 556, 610, 556},  // bolditalic
};

// A run of `text`'s width in ems of the formula's text face (Liberation
// Sans; Liberation Mono for code).
double SansEm(const std::string &text, bool bold, bool italic, bool mono) {
    const unsigned short *w = kSansWidths[(bold ? 1 : 0) + (italic ? 2 : 0)];
    double em = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int cp = c, len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        for (int k = 1; k < len && i + static_cast<size_t>(k) < text.size(); ++k)
            cp = cp << 6 | (static_cast<unsigned char>(text[i + static_cast<size_t>(k)]) & 0x3F);
        i += static_cast<size_t>(len);
        if (mono) em += 0.6;
        else if (cp >= 0x20 && cp <= 0x7E) em += w[cp - 0x20] / 1000.0;
        else if (cp >= 0xA0 && cp <= 0xFF) em += w[95 + cp - 0xA0] / 1000.0;
        else em += 0.6;
    }
    return em;
}

// One unbreakable piece of a formula paragraph: a word of text (with the
// space after it) or a whole inline expression. Sizes in ems.
// Heights are split at the baseline the line's text sits on, so a line
// holding a superscript and a subscript is as tall as both.
struct FormulaUnit {
    Run style;       // a word's run (`text` is the word), or the maths' (`tex`)
    double w = 0, asc = 0, desc = 0;
};
struct FormulaLine {
    std::vector<FormulaUnit> units;
    double w = 0, asc = 0, desc = 0;
    double h() const { return asc + desc; }
};
constexpr double kFormulaTextEm = 1.11;  // a line of text, as LibreOffice Math sets it
constexpr double kFormulaTextAscEm = 0.8;  // (math_markup.cpp's Extent::kAsc)
constexpr double kFormulaWidthScale = 1.03;  // LibreOffice Math's width for an em of ours (measured)

// Prose in StarMath: quoted, its quotes and backslashes escaped.
std::string StarMathQuote(const std::string &text) {
    std::string o = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
constexpr double kFormulaLineGapEm = 0.17;  // (measured)

// A paragraph's runs as the styles its text box would give them.
std::vector<Run> ParaRuns(const Para &p) {
    std::vector<Run> runs;
    for (Run r : p.runs) {
        if (p.kind == ParaKind::Heading || p.kind == ParaKind::Title) r.bold = true;
        if ((p.kind == ParaKind::Caption || p.kind == ParaKind::Note) && r.color.empty()) r.color = "59636E";
        if (p.kind == ParaKind::Caption && !r.bold) r.italic = true;
        if (MonoKind(p.kind)) r.mono = true;
        runs.push_back(std::move(r));
    }
    return runs;
}

// Breaks `p` into lines at most `avail_em` wide.
std::vector<FormulaLine> FormulaLines(const Para &p, double avail_em) {
    std::vector<FormulaUnit> units;
    for (const Run &r : ParaRuns(p)) {
        if (!r.tex.empty()) {
            FormulaUnit u;
            u.style = r;
            double h = 0;
            TexMathExtent(r.tex, false, &u.w, &h, &u.asc);
            u.desc = h - u.asc;
            units.push_back(std::move(u));
            continue;
        }
        size_t i = 0;
        while (i < r.text.size()) {
            size_t j = r.text.find(' ', i);
            j = j == std::string::npos ? r.text.size() : r.text.find_first_not_of(' ', j);
            if (j == std::string::npos) j = r.text.size();
            FormulaUnit u;
            u.style = r;
            u.style.text = r.text.substr(i, j - i);
            u.w = SansEm(u.style.text, r.bold, r.italic, r.mono);
            u.asc = kFormulaTextAscEm;
            u.desc = kFormulaTextEm - kFormulaTextAscEm;
            units.push_back(std::move(u));
            i = j;
        }
    }
    std::vector<FormulaLine> lines(1);
    for (FormulaUnit &u : units) {
        if (!lines.back().units.empty() && lines.back().w + u.w > avail_em) lines.emplace_back();
        FormulaLine &l = lines.back();
        l.w += u.w;
        l.asc = std::max(l.asc, u.asc);
        l.desc = std::max(l.desc, u.desc);
        l.units.push_back(std::move(u));
    }
    return lines;
}

// Where a formula paragraph's text starts, and how wide it may be: past
// the frame's padding (as a text box's), and a list item's label.
constexpr long kFormulaPadX = 90000, kFormulaPadY = 46800;  // 0.25 cm, 0.13 cm
long FormulaIndent(const Para &p) {
    const bool list = p.kind == ParaKind::Bullet || p.kind == ParaKind::Number;
    return kFormulaPadX + (list ? 324000L * p.level + 288000L : 0);  // OdpListStyles: 0.9 cm a level, 0.8 cm label
}

// A formula paragraph's lines at the slide's type scale, and its height.
std::vector<FormulaLine> FormulaParaLines(const Para &p, long width, double scale) {
    const double pt = BasePt(p.kind) * scale;
    const double avail = static_cast<double>(width - FormulaIndent(p) - kFormulaPadX) / kEmuIn * 72.0 / pt;
    return FormulaLines(p, std::max(4.0, avail));
}
double FormulaLinesHeightEm(const std::vector<FormulaLine> &lines) {
    double h = 0;
    for (const FormulaLine &l : lines) h += l.h();
    return h + kFormulaLineGapEm * static_cast<double>(lines.size() - 1);
}
long FormulaParaHeight(const Para &p, long width, double scale) {
    const double pt = BasePt(p.kind) * scale;
    const double em = FormulaLinesHeightEm(FormulaParaLines(p, width, scale)) + ParaSpacing(p.kind);
    return static_cast<long>(em * pt / 72.0 * kEmuIn) + 2 * kFormulaPadY;
}

long CaptionHeight(const std::vector<Run> &cap, long width, double scale) {
    return cap.empty() ? 0 : TextHeight(Chars(cap), BasePt(ParaKind::Caption) * scale, false, width, 0.2) + kEmuIn / 20;
}

enum class ShapeKind { Text, Code, Output, Table, Image, Caption, Math, Formula };

struct Shape {
    ShapeKind kind = ShapeKind::Text;
    long x = 0, y = 0, w = 0, h = 0;
    std::vector<Para> paras;  // Text, Code, Caption
    const Item *item = nullptr;  // Table, Image
    double scale = 1.0;  // font sizes
    bool center = false;
    std::string tex;  // Math
};

// The space after items[i]: less between code and its results, which
// belong together.
long GapAfter(const std::vector<Item> &items, size_t i) {
    return items[i].kind == ItemKind::Code && i + 1 < items.size() && items[i + 1].kind == ItemKind::Output ? kGap / 2 : kGap;
}

// Places `items` in the box (x, y, w, h), shrinking fonts until they fit.
void Stack(const std::vector<Item> &items, long x, long y, long w, long h, std::vector<Shape> &out) {
    const long min_image = kEmuIn;
    double scale = 1.0;
    auto fixed_height = [&](double s) {
        long total = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const Item &it = items[i];
            if (it.kind == ItemKind::Text || it.kind == ItemKind::Code || it.kind == ItemKind::Output) total += ParasHeight(it.paras, w, s);
            else if (it.kind == ItemKind::Table) total += TableHeight(it, w, s) + CaptionHeight(it.caption, w, s);
            else if (it.kind == ItemKind::Math) total += MathHeight(it.tex, w, s);
            else if (it.kind == ItemKind::Formula) total += FormulaParaHeight(it.paras[0], w, s);
            else total += min_image + CaptionHeight(it.caption, w, s);
            total += GapAfter(items, i);
        }
        return total;
    };
    while (scale > 0.3 && fixed_height(scale) > h) scale -= 0.025;
    // What is left goes to the pictures, each at most its full-width height.
    long images = 0, spare = h - fixed_height(scale);
    for (const Item &it : items) images += it.kind == ItemKind::Image;
    long cy = y;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item &it = items[i];
        // A table's caption goes above it, as in the Beamer deck and the
        // HTML slideshow; a picture's below.
        const bool caption_above = it.kind == ItemKind::Table && !it.caption.empty();
        if (caption_above) {
            Shape c;
            c.kind = ShapeKind::Caption;
            c.scale = scale;
            c.x = x;
            c.w = w;
            c.y = cy;
            c.h = CaptionHeight(it.caption, w, scale);
            c.center = true;
            c.paras.push_back(Para{ParaKind::Caption, it.caption});
            out.push_back(c);
            cy += c.h;
        }
        Shape s;
        s.scale = scale;
        s.x = x;
        s.w = w;
        s.y = cy;
        switch (it.kind) {
            case ItemKind::Text:
            case ItemKind::Code:
            case ItemKind::Output:
                s.kind = it.kind == ItemKind::Code ? ShapeKind::Code : it.kind == ItemKind::Output ? ShapeKind::Output : ShapeKind::Text;
                s.paras = it.paras;
                s.h = ParasHeight(it.paras, w, scale);
                out.push_back(s);
                break;
            case ItemKind::Formula:
                s.kind = ShapeKind::Formula;
                s.paras = it.paras;
                s.h = FormulaParaHeight(it.paras[0], w, scale);
                out.push_back(s);
                break;
            case ItemKind::Math:
                s.kind = ShapeKind::Math;
                s.tex = it.tex;
                s.scale = MathScale(it.tex, w, scale);
                s.h = MathHeight(it.tex, w, scale);
                out.push_back(s);
                break;
            case ItemKind::Table: {
                s.kind = ShapeKind::Table;
                s.item = &it;
                const std::vector<long> cols = TableColumns(it, w, scale);
                long tw = 0;
                for (long c : cols) tw += c;
                s.w = tw;
                s.x = x + (w - tw) / 2;
                s.h = TableHeight(it, w, scale);
                out.push_back(s);
                break;
            }
            case ItemKind::Image: {
                s.kind = ShapeKind::Image;
                s.item = &it;
                const long full = static_cast<long>(static_cast<double>(w) * it.px_h / it.px_w);
                long ih = std::min(full, min_image + std::max(0L, spare) / std::max(1L, images));
                ih = std::max(std::min(ih, full), std::min(full, kEmuIn / 2));
                s.h = ih;
                s.w = static_cast<long>(static_cast<double>(ih) * it.px_w / it.px_h);
                s.x = x + (w - s.w) / 2;
                out.push_back(s);
                break;
            }
        }
        cy += s.h;
        if (!it.caption.empty() && it.kind == ItemKind::Image) {
            Shape c;
            c.kind = ShapeKind::Caption;
            c.scale = scale;
            c.x = x;
            c.w = w;
            c.y = cy;
            c.h = CaptionHeight(it.caption, w, scale);
            c.center = true;
            c.paras.push_back(Para{ParaKind::Caption, it.caption});
            out.push_back(c);
            cy += c.h;
        }
        cy += GapAfter(items, i);
    }
    // Still too tall at the smallest type: squeeze the boxes into the page
    // (the text boxes shrink their own text to fit -- normAutofit,
    // shrink-to-fit -- in the programs that open them).
    const long bottom = y + h;
    for (Shape &s : out)
        if (s.y + s.h > bottom && s.kind != ShapeKind::Image) {
            s.y = std::min(s.y, bottom - kEmuIn / 3);
            s.h = bottom - s.y;
        }
}

struct PlacedSlide {
    std::vector<Shape> shapes;
};

PlacedSlide LayoutSlide(const DeckSlide &s) {
    PlacedSlide p;
    long top = kEmuIn / 2;
    if (!s.title.empty()) {
        Shape t;
        t.kind = ShapeKind::Text;
        t.x = kMarginX;
        t.y = kEmuIn * 3 / 10;
        t.w = kPageW - 2 * kMarginX;
        t.h = kEmuIn * 95 / 100;
        Para tp{ParaKind::Heading, s.title};
        tp.kind = ParaKind::Title;
        t.paras.push_back(tp);
        // A long title steps down in size to stay on one line (down to
        // 22 pt): a second line would run into the slide's content.
        double title_em = 0;
        for (const Run &r : s.title) title_em += SansEm(r.text, true, r.italic, r.mono);
        // (Measured in Liberation Sans; the face a reader substitutes is
        // often a little wider, and the box has its insets.)
        const double fit_pt = (static_cast<double>(t.w) / kEmuIn - 0.25) * 72.0 / std::max(1.0, title_em * 1.08);
        t.scale = std::max(22.0, std::min(kSlideTitlePt, fit_pt)) / BasePt(ParaKind::Title);
        p.shapes.push_back(t);
        top = kEmuIn * 135 / 100;
    }
    const long x = kMarginX, w = kPageW - 2 * kMarginX, h = kPageH - top - kEmuIn * 4 / 10;
    // One picture beside text (and no table): two columns, the picture
    // on the right -- unless it is so wide it wants the whole width.
    int images = 0, tables = 0, texts = 0;
    const Item *picture = nullptr;
    for (const Item &it : s.items) {
        if (it.kind == ItemKind::Image) {
            ++images;
            picture = &it;
        } else if (it.kind == ItemKind::Table) {
            ++tables;
        } else {
            ++texts;
        }
    }
    if (images == 1 && tables == 0 && texts > 0 && picture->px_w < picture->px_h * 16 / 10) {
        const long left_w = w * 55 / 100, right_w = w - left_w - kGap * 2;
        // (Only text is left of the picture, and text shapes keep copies
        // of their paragraphs, so `left` may go out of scope.)
        std::vector<Item> left;
        for (const Item &it : s.items)
            if (&it != picture) left.push_back(it);
        Stack(left, x, top, left_w, h, p.shapes);
        const long cap = CaptionHeight(picture->caption, right_w, 1.0);
        long ih = std::min(h - cap, static_cast<long>(static_cast<double>(right_w) * picture->px_h / picture->px_w));
        Shape im;
        im.kind = ShapeKind::Image;
        im.item = picture;
        im.h = ih;
        im.w = static_cast<long>(static_cast<double>(ih) * picture->px_w / picture->px_h);
        im.x = x + left_w + kGap * 2 + (right_w - im.w) / 2;
        im.y = top + (h - ih - cap) / 2;
        p.shapes.push_back(im);
        if (cap > 0) {
            Shape c;
            c.kind = ShapeKind::Caption;
            c.x = x + left_w + kGap * 2;
            c.w = right_w;
            c.y = im.y + ih;
            c.h = cap;
            c.center = true;
            c.paras.push_back(Para{ParaKind::Caption, picture->caption});
            p.shapes.push_back(c);
        }
        return p;
    }
    Stack(s.items, x, top, w, h, p.shapes);
    return p;
}

PlacedSlide LayoutTitle(const Deck &d) {
    PlacedSlide p;
    long y = kEmuIn * 22 / 10;
    auto add = [&](ParaKind kind, const std::string &text, long h) {
        if (text.empty()) return;
        Shape s;
        s.kind = ShapeKind::Text;
        s.x = kMarginX;
        s.w = kPageW - 2 * kMarginX;
        s.y = y;
        s.h = h;
        s.center = true;
        Run r;
        r.text = text;
        r.bold = kind == ParaKind::Title;
        if (kind != ParaKind::Title) r.color = "59636E";
        s.paras.push_back(Para{kind, {r}});
        p.shapes.push_back(s);
        y += h;
    };
    add(ParaKind::Title, d.title, kEmuIn * 12 / 10);
    add(ParaKind::Subtitle, d.subtitle, kEmuIn * 8 / 10);
    y += kEmuIn / 4;
    add(ParaKind::Byline, d.author, kEmuIn * 5 / 10);
    add(ParaKind::Byline, d.date, kEmuIn * 5 / 10);
    return p;
}

// Impress only: each paragraph whose inline maths needs two dimensions
// comes out of its text box to be a formula of its own (see above).
void SplitFormulaParagraphs(Deck &d) {
    auto needs = [](const Para &p) {
        if (MonoKind(p.kind) || p.kind == ParaKind::Title) return false;
        for (const Run &r : p.runs)
            if (!r.tex.empty() && TexNeedsLayout(r.tex)) return true;
        return false;
    };
    for (DeckSlide &s : d.slides) {
        std::vector<Item> items;
        for (Item &it : s.items) {
            if (it.kind != ItemKind::Text) {
                items.push_back(std::move(it));
                continue;
            }
            Item text;
            for (Para &p : it.paras) {
                if (!needs(p)) {
                    text.paras.push_back(std::move(p));
                    continue;
                }
                if (!text.paras.empty()) items.push_back(std::move(text));
                text = Item{};
                Item f;
                f.kind = ItemKind::Formula;
                f.paras.push_back(std::move(p));
                items.push_back(std::move(f));
            }
            if (!text.paras.empty()) items.push_back(std::move(text));
        }
        s.items = std::move(items);
    }
}

std::vector<PlacedSlide> Layout(const Deck &d) {
    std::vector<PlacedSlide> out;
    if (!d.title.empty() || !d.subtitle.empty() || !d.author.empty()) out.push_back(LayoutTitle(d));
    for (const DeckSlide &s : d.slides) out.push_back(LayoutSlide(s));
    return out;
}

// --- XML helpers ---------------------------------------------------------------------

std::string X(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default:
                // XML 1.0 has no place for other control characters.
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n') break;
                o += c;
        }
    }
    return o;
}

std::string Num(long v) { return std::to_string(v); }

// Inline maths as ordinary runs: its typeset text, in the run's own style.
std::vector<Run> MathRuns(const Run &r) {
    std::vector<Run> out;
    for (const MathTextRun &m : TexToTextRuns(r.tex)) {
        Run x = r;
        x.tex.clear();
        x.text = m.text;
        x.italic = m.italic;
        x.bold = r.bold || m.bold;
        x.super = r.super || m.script > 0;
        x.sub = !x.super && (r.sub || m.script < 0);
        x.serif = true;
        out.push_back(std::move(x));
    }
    return out;
}

// --- PowerPoint ---------------------------------------------------------------------------

const char *kPNs =
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
const char *kXmlHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
// A slide's equations: Office Math inside PowerPoint 2010's a14:m, behind
// markup compatibility's AlternateContent.
const char *kMathNs =
    "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" "
    "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"";
const std::string kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

const char *kGroupProps =
    "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
    "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";

struct PptxSlideWriter {
    std::vector<std::pair<std::string, std::string>> rels;  // (type, target) from rId2 on
    std::vector<std::pair<std::string, std::string>> media;  // (zip name, bytes)
    std::map<std::string, std::string> link_ids;
    int next_id = 2;
    int *image_counter = nullptr;

    std::string Rel(const std::string &type, const std::string &target) {
        rels.emplace_back(type, target);
        return "rId" + std::to_string(rels.size() + 1);  // rId1 is the layout
    }

    std::string RunXml(const Run &r, double pt) {
        std::string a = " lang=\"en-US\" sz=\"" + Num(std::lround(pt * 100)) + "\"";
        if (r.bold) a += " b=\"1\"";
        if (r.italic) a += " i=\"1\"";
        if (r.underline) a += " u=\"sng\"";
        if (r.strike) a += " strike=\"sngStrike\"";
        if (r.super) a += " baseline=\"30000\"";
        if (r.sub) a += " baseline=\"-25000\"";
        std::string inner;
        if (!r.color.empty()) inner += "<a:solidFill><a:srgbClr val=\"" + r.color + "\"/></a:solidFill>";
        // Every run names its face: a reader may carry a monospaced run's
        // face on into the runs after it (LibreOffice does).
        inner += r.mono ? "<a:latin typeface=\"Consolas\"/><a:cs typeface=\"Consolas\"/>"
                        : "<a:latin typeface=\"+mn-lt\"/><a:cs typeface=\"+mn-cs\"/>";
        if (!r.link.empty()) {
            auto it = link_ids.find(r.link);
            if (it == link_ids.end()) it = link_ids.emplace(r.link, Rel("hyperlink", r.link)).first;
            inner += "<a:hlinkClick r:id=\"" + it->second + "\"/>";
        }
        return "<a:r><a:rPr" + a + ">" + inner + "</a:rPr><a:t>" + X(r.text) + "</a:t></a:r>";
    }

    // Every run of an equation names Cambria Math, as PowerPoint's own do.
    static std::string OmmlRunProps(double pt) {
        return "<a:rPr lang=\"en-US\" sz=\"" + Num(std::lround(pt * 100)) +
               "\"><a:latin typeface=\"Cambria Math\"/><a:cs typeface=\"Cambria Math\"/></a:rPr>";
    }

    // A display equation's paragraph: an Office equation when `math`, else
    // the text it reads as (the shape's fallback).
    std::string MathParaXml(const std::string &tex, double scale, bool math) {
        const double pt = BasePt(ParaKind::Math) * scale;
        std::string o = "<a:p><a:pPr algn=\"ctr\"><a:buNone/></a:pPr>";
        if (math) {
            o += "<a14:m><m:oMathPara><m:oMathParaPr><m:jc m:val=\"centerGroup\"/></m:oMathParaPr>" + TexToOmml(tex, OmmlRunProps(pt)) +
                 "</m:oMathPara></a14:m>";
        } else {
            Run r;
            r.tex = tex;
            for (const Run &x : MathRuns(r)) o += RunXml(x, pt);
        }
        return o + "<a:endParaRPr lang=\"en-US\" sz=\"" + Num(std::lround(pt * 100)) + "\"/></a:p>";
    }

    // `math`: inline maths as Office equations rather than as text.
    std::string ParaXml(const Para &p, double scale, bool center, bool math = false) {
        const double pt = BasePt(p.kind) * scale;
        std::string ppr;
        // A number ("10.") needs a wider hang than a bullet, or it runs
        // into its text.
        const long indent = p.kind == ParaKind::Number ? kEmuIn * 2 / 5 : kEmuIn * 3 / 10;
        if (p.kind == ParaKind::Bullet || p.kind == ParaKind::Number) {
            ppr = " marL=\"" + Num(indent * (p.level + 1)) + "\" indent=\"-" + Num(indent) + "\"";
        } else if (center || p.kind == ParaKind::Math || p.kind == ParaKind::Caption) {
            ppr = " algn=\"ctr\"";
        }
        std::string inner;
        if (!MonoKind(p.kind) && p.kind != ParaKind::Title)
            inner += "<a:spcBef><a:spcPts val=\"" + Num(std::lround(pt * 100 * ParaSpacing(p.kind))) + "\"/></a:spcBef>";
        if (p.kind == ParaKind::Bullet) inner += "<a:buFont typeface=\"Arial\"/><a:buChar char=\"" + std::string(p.level % 2 ? "–" : "•") + "\"/>";
        else if (p.kind == ParaKind::Number) inner += "<a:buAutoNum type=\"arabicPeriod\" startAt=\"" + Num(std::max(1, p.number)) + "\"/>";
        else inner += "<a:buNone/>";
        std::string o = "<a:p><a:pPr" + ppr + ">" + inner + "</a:pPr>";
        for (Run r : p.runs) {
            if (p.kind == ParaKind::Heading || p.kind == ParaKind::Title) r.bold = true;
            if (p.kind == ParaKind::Caption || p.kind == ParaKind::Note) {
                if (r.color.empty()) r.color = "59636E";
            }
            if (p.kind == ParaKind::Caption && !r.bold) r.italic = true;
            if (MonoKind(p.kind)) r.mono = true;
            if (!r.tex.empty()) {
                if (math) {
                    o += "<a14:m>" + TexToOmml(r.tex, OmmlRunProps(pt)) + "</a14:m>";
                } else {
                    for (const Run &x : MathRuns(r)) o += RunXml(x, pt);
                }
                continue;
            }
            o += RunXml(r, pt);
        }
        o += "<a:endParaRPr lang=\"en-US\" sz=\"" + Num(std::lround(pt * 100)) + "\"/></a:p>";
        return o;
    }

    std::string Xfrm(const Shape &s, const char *ns) {
        return std::string("<") + ns + ":xfrm><a:off x=\"" + Num(s.x) + "\" y=\"" + Num(s.y) + "\"/><a:ext cx=\"" + Num(s.w) +
               "\" cy=\"" + Num(s.h) + "\"/></" + ns + ":xfrm>";
    }

    static bool HasMath(const Shape &s) {
        if (s.kind == ShapeKind::Math) return true;
        for (const Para &p : s.paras)
            for (const Run &r : p.runs)
                if (!r.tex.empty()) return true;
        return false;
    }

    // A shape holding maths twice over: with Office equations for the
    // readers that know them (a14), and with its maths as text for the rest.
    // A display equation's fallback is a picture of it when there is a
    // renderer (as PowerPoint's own is), centred in the shape's box.
    std::string MathTextShape(const Shape &s) {
        const int id = next_id;
        const std::string with = TextShape(s, true);
        next_id = id;
        std::string fallback;
        MathPicture pic;
        if (s.kind == ShapeKind::Math && MathRenderer() &&
            MathRenderer()(s.tex, true, BasePt(ParaKind::Math) * s.scale, &pic) && !pic.png.empty() && pic.width_pt > 0) {
            Shape f = s;
            f.w = static_cast<long>(pic.width_pt / 72.0 * kEmuIn);
            f.h = static_cast<long>(pic.height_pt / 72.0 * kEmuIn);
            if (f.w > s.w) {
                f.h = f.h * s.w / f.w;
                f.w = s.w;
            }
            f.x = s.x + (s.w - f.w) / 2;
            f.y = s.y + std::max(0L, (s.h - f.h) / 2);
            fallback = PictureXml(f, pic.png, "png", s.tex);
        } else {
            fallback = TextShape(s, false);
        }
        return "<mc:AlternateContent><mc:Choice Requires=\"a14\">" + with + "</mc:Choice><mc:Fallback>" + fallback +
               "</mc:Fallback></mc:AlternateContent>";
    }

    // A code block's results: a bar in the accent colour down their left
    // edge, the text inset past it (TextShape).
    std::string OutputBar(const Shape &s) {
        const int id = next_id++;
        Shape bar = s;
        bar.w = kOutputBarW;
        return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + Num(id) + "\" name=\"Output bar " + Num(id) +
               "\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>" + Xfrm(bar, "a") +
               "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val=\"" + kOutputAccent +
               "\"/></a:solidFill><a:ln><a:noFill/></a:ln></p:spPr></p:sp>";
    }

    std::string TextShape(const Shape &s, bool math = false) {
        const int id = next_id++;
        const bool code = s.kind == ShapeKind::Code, output = s.kind == ShapeKind::Output;
        std::string fill = code     ? "<a:solidFill><a:srgbClr val=\"F6F8FA\"/></a:solidFill><a:ln w=\"9525\"><a:solidFill><a:srgbClr val=\"D0D7DE\"/></a:solidFill></a:ln>"
                           : output ? "<a:noFill/><a:ln><a:noFill/></a:ln>"
                                    : "<a:noFill/>";
        std::string o = "<p:sp><p:nvSpPr><p:cNvPr id=\"" + Num(id) + "\" name=\"" + (code ? "Code " : output ? "Output " : "Text ") + Num(id) +
                        "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr><p:spPr>" + Xfrm(s, "a") +
                        "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>" + fill + "</p:spPr>";
        o += "<p:txBody><a:bodyPr wrap=\"square\" lIns=\"" + std::string(output ? Num(kOutputBarW + 137160) : "91440") + "\" tIns=\"45720\" rIns=\"91440\" bIns=\"45720\" anchor=\"" +
             std::string(s.paras.size() == 1 && s.paras[0].kind == ParaKind::Title ? "b" : "t") + "\"><a:normAutofit/></a:bodyPr><a:lstStyle/>";
        if (s.kind == ShapeKind::Math) o += MathParaXml(s.tex, s.scale, math);
        for (const Para &p : s.paras) o += ParaXml(p, s.scale, s.center, math);
        return o + "</p:txBody></p:sp>";
    }

    std::string ImageShape(const Shape &s) { return PictureXml(s, s.item->bytes, s.item->ext, s.item->alt); }

    std::string PictureXml(const Shape &s, const std::string &bytes, const std::string &ext, const std::string &alt) {
        const int id = next_id++;
        const std::string name = "image" + Num(++*image_counter) + "." + ext;
        media.emplace_back("ppt/media/" + name, bytes);
        const std::string rid = Rel("image", "../media/" + name);
        return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + Num(id) + "\" name=\"Picture " + Num(id) + "\" descr=\"" + X(alt) +
               "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"" +
               rid + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" + Xfrm(s, "a") +
               "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
    }

    std::string TableShape(const Shape &s) {
        const int id = next_id++;
        const Item &t = *s.item;
        const std::vector<long> cols = TableColumns(t, s.w + 1, s.scale);
        std::string o = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" + Num(id) + "\" name=\"Table " + Num(id) +
                        "\"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/></p:nvGraphicFramePr>" +
                        Xfrm(s, "p") +
                        "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl><a:tblPr firstRow=\"" +
                        std::string(t.header_rows > 0 ? "1" : "0") + "\"/><a:tblGrid>";
        for (long c : cols) o += "<a:gridCol w=\"" + Num(c) + "\"/>";
        o += "</a:tblGrid>";
        const long row_h = s.h / std::max<long>(1, static_cast<long>(t.rows.size()));
        const std::string line = "w=\"6350\"><a:solidFill><a:srgbClr val=\"BFC5CC\"/></a:solidFill>";
        for (size_t r = 0; r < t.rows.size(); ++r) {
            const bool head = static_cast<int>(r) < t.header_rows;
            o += "<a:tr h=\"" + Num(row_h) + "\">";
            for (size_t c = 0; c < cols.size(); ++c) {
                Para p{ParaKind::Body, c < t.rows[r].size() ? t.rows[r][c] : std::vector<Run>()};
                for (Run &run : p.runs) run.bold = run.bold || head;
                std::string para = ParaXml(p, kTablePt / BasePt(ParaKind::Body) * s.scale, false);
                // No space before a cell's only paragraph.
                const size_t sb = para.find("<a:spcBef>");
                if (sb != std::string::npos) para.erase(sb, para.find("</a:spcBef>") + 11 - sb);
                o += "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/>" + para + "</a:txBody><a:tcPr marL=\"68580\" marR=\"68580\" marT=\"34290\" marB=\"34290\">";
                for (const char *side : {"lnL", "lnR", "lnT", "lnB"})
                    o += std::string("<a:") + side + " " + line + "</a:" + side + ">";
                o += head ? "<a:solidFill><a:srgbClr val=\"E8EDF5\"/></a:solidFill>" : "<a:noFill/>";
                o += "</a:tcPr></a:tc>";
            }
            o += "</a:tr>";
        }
        return o + "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
    }

    std::string Slide(const PlacedSlide &ps) {
        std::string shapes;
        for (const Shape &s : ps.shapes) {
            if (s.kind == ShapeKind::Image) shapes += ImageShape(s);
            else if (s.kind == ShapeKind::Table) shapes += TableShape(s);
            else if (HasMath(s)) shapes += MathTextShape(s);
            else if (s.kind == ShapeKind::Output) shapes += OutputBar(s) + TextShape(s);
            else shapes += TextShape(s);
        }
        return std::string(kXmlHead) + "<p:sld " + kPNs + " " + kMathNs + "><p:cSld><p:spTree>" + kGroupProps + shapes +
               "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
    }

    std::string Rels() {
        std::string o = std::string(kXmlHead) +
                        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                        "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>";
        for (size_t k = 0; k < rels.size(); ++k) {
            o += "<Relationship Id=\"rId" + Num(static_cast<long>(k) + 2) + "\" Type=\"" + kRelNs + rels[k].first + "\" Target=\"" +
                 X(rels[k].second) + "\"" + (rels[k].first == "hyperlink" ? " TargetMode=\"External\"" : "") + "/>";
        }
        return o + "</Relationships>";
    }
};

std::string PptxTheme() {
    auto clr = [](const char *name, const char *rgb) {
        return std::string("<a:") + name + "><a:srgbClr val=\"" + rgb + "\"/></a:" + name + ">";
    };
    std::string three_fills, three_lines, three_effects;
    for (int k = 0; k < 3; ++k) {
        three_fills += "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
        three_lines += "<a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>";
        three_effects += "<a:effectStyle><a:effectLst/></a:effectStyle>";
    }
    // Arial, not Office's Calibri: the layout above is measured in Liberation
    // Sans, which is Arial's metrics, and every reader has one or the other
    // -- where Calibri is missing (Linux) it falls back to something far
    // wider, and table headers and titles break over lines.
    const std::string font = "<a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/>";
    return std::string(kXmlHead) +
           "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"mep\"><a:themeElements>"
           "<a:clrScheme name=\"mep\">" +
           clr("dk1", "1F2328") + clr("lt1", "FFFFFF") + clr("dk2", "24292F") + clr("lt2", "F6F8FA") + clr("accent1", "6B8AFD") +
           clr("accent2", "E5A50A") + clr("accent3", "2F7D32") + clr("accent4", "C678DD") + clr("accent5", "0B7285") +
           clr("accent6", "C62828") + clr("hlink", "0B5CAD") + clr("folHlink", "8250DF") +
           "</a:clrScheme><a:fontScheme name=\"mep\"><a:majorFont>" + font + "</a:majorFont><a:minorFont>" + font +
           "</a:minorFont></a:fontScheme><a:fmtScheme name=\"mep\"><a:fillStyleLst>" + three_fills + "</a:fillStyleLst><a:lnStyleLst>" +
           three_lines + "</a:lnStyleLst><a:effectStyleLst>" + three_effects + "</a:effectStyleLst><a:bgFillStyleLst>" + three_fills +
           "</a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";
}

std::string CoreProps(const Deck &d) {
    return std::string(kXmlHead) +
           "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
           "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:title>" +
           X(d.title) + "</dc:title><dc:creator>" + X(d.author) + "</dc:creator></cp:coreProperties>";
}

bool WriteFile(const std::string &path, const std::string &bytes, std::string *error) {
    std::ofstream o(path, std::ios::binary);
    o << bytes;
    if (!o) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

// --- Impress ---------------------------------------------------------------------------------

std::string Cm(long emu) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3fcm", static_cast<double>(emu) / 360000.0);
    return b;
}
std::string Pt(double pt) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1fpt", pt);
    return b;
}

// ODF text: runs of spaces as <text:s/>, tabs as <text:tab/>.
std::string OdfText(const std::string &t) {
    std::string o;
    size_t i = 0;
    while (i < t.size()) {
        const char c = t[i];
        if (c == '\t') {
            o += "<text:tab/>";
            ++i;
        } else if (c == ' ' && (i == 0 || i + 1 == t.size() || t[i + 1] == ' ')) {
            size_t j = i;
            while (j < t.size() && t[j] == ' ') ++j;
            // One space inside the text stays a space; the rest are counted.
            if (i > 0 && j < t.size()) {
                o += ' ';
                ++i;
            }
            if (j > i) o += "<text:s text:c=\"" + Num(static_cast<long>(j - i)) + "\"/>";
            i = j;
        } else {
            o += X(std::string(1, c));
            ++i;
        }
    }
    return o;
}

struct OdpWriter {
    std::map<std::string, std::string> text_styles;  // properties -> name
    std::map<std::string, std::string> para_styles;
    std::string auto_styles;
    std::vector<std::pair<std::string, std::string>> pictures;  // (zip name, bytes)
    struct Formula {
        std::string name, mathml;  // mathml: the whole <math> document
        double pt = 0;
    };
    std::vector<Formula> formulas;  // "Object N": a display equation's, or a formula paragraph's, object

    std::string TextStyle(const Run &r, double pt) {
        std::string props = "fo:font-size=\"" + Pt(pt) + "\"";
        if (r.bold) props += " fo:font-weight=\"bold\"";
        if (r.italic) props += " fo:font-style=\"italic\"";
        if (r.underline) props += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
        if (r.strike) props += " style:text-line-through-style=\"solid\"";
        if (r.super) props += " style:text-position=\"super 58%\"";
        if (r.sub) props += " style:text-position=\"sub 58%\"";
        if (!r.color.empty()) props += " fo:color=\"#" + r.color + "\"";
        if (r.mono) props += " fo:font-family=\"'Liberation Mono'\" style:font-family-generic=\"modern\" style:font-pitch=\"fixed\"";
        else if (r.serif) props += " fo:font-family=\"'Liberation Serif'\" style:font-family-generic=\"roman\"";
        auto it = text_styles.find(props);
        if (it != text_styles.end()) return it->second;
        const std::string name = "T" + Num(static_cast<long>(text_styles.size()) + 1);
        auto_styles += "<style:style style:name=\"" + name + "\" style:family=\"text\"><style:text-properties " + props + "/></style:style>";
        text_styles.emplace(props, name);
        return name;
    }

    // `pt`: the paragraph's own size, which a list's label is drawn at.
    std::string ParaStyle(const std::string &props, double pt = 0) {
        const std::string key = props + "|" + Pt(pt);
        auto it = para_styles.find(key);
        if (it != para_styles.end()) return it->second;
        const std::string name = "P" + Num(static_cast<long>(para_styles.size()) + 1);
        auto_styles += "<style:style style:name=\"" + name + "\" style:family=\"paragraph\"><style:paragraph-properties " + props + "/>" +
                       (pt > 0 ? "<style:text-properties fo:font-size=\"" + Pt(pt) + "\"/>" : "") + "</style:style>";
        para_styles.emplace(key, name);
        return name;
    }

    std::string ParaXml(const Para &p, double scale, bool center) {
        const double pt = BasePt(p.kind) * scale;
        std::string props = "fo:margin-top=\"" + (MonoKind(p.kind) || p.kind == ParaKind::Title ? std::string("0cm") : Pt(pt * ParaSpacing(p.kind))) +
                            "\" fo:margin-bottom=\"0cm\"";
        if (center || p.kind == ParaKind::Math || p.kind == ParaKind::Caption) props += " fo:text-align=\"center\"";
        std::string o = "<text:p text:style-name=\"" + ParaStyle(props, pt) + "\">";
        for (Run r : p.runs) {
            if (p.kind == ParaKind::Heading || p.kind == ParaKind::Title) r.bold = true;
            if ((p.kind == ParaKind::Caption || p.kind == ParaKind::Note) && r.color.empty()) r.color = "59636E";
            if (p.kind == ParaKind::Caption && !r.bold) r.italic = true;
            if (MonoKind(p.kind)) r.mono = true;
            // Maths: its typeset text (a text box has no room for a formula).
            if (!r.tex.empty()) {
                for (const Run &x : MathRuns(r)) o += "<text:span text:style-name=\"" + TextStyle(x, pt) + "\">" + OdfText(x.text) + "</text:span>";
                continue;
            }
            // Impress reads a link as a field of plain text: the span goes outside.
            std::string text = OdfText(r.text);
            if (!r.link.empty()) text = "<text:a xlink:type=\"simple\" xlink:href=\"" + X(r.link) + "\">" + text + "</text:a>";
            o += "<text:span text:style-name=\"" + TextStyle(r, pt) + "\">" + text + "</text:span>";
        }
        return o + "</text:p>";
    }

    // A list item: its paragraph inside `level + 1` nested lists, the
    // number carried on the innermost item.
    std::string ListXml(const Para &p, double scale) {
        const std::string style = p.kind == ParaKind::Number ? "LNum" : "LBul";
        std::string open, close;
        for (int k = 0; k <= p.level; ++k) {
            open += k == 0 ? "<text:list text:style-name=\"" + style + "\">" : "<text:list>";
            open += k == p.level && p.kind == ParaKind::Number ? "<text:list-item text:start-value=\"" + Num(std::max(1, p.number)) + "\">"
                                                                 : "<text:list-item>";
            close = "</text:list-item></text:list>" + close;
        }
        return open + ParaXml(p, scale, false) + close;
    }

    std::string Frame(const Shape &s, const std::string &style, const std::string &inner) {
        return "<draw:frame draw:style-name=\"" + style + "\" draw:layer=\"layout\" svg:x=\"" + Cm(s.x) + "\" svg:y=\"" + Cm(s.y) +
               "\" svg:width=\"" + Cm(s.w) + "\" svg:height=\"" + Cm(s.h) + "\">" + inner + "</draw:frame>";
    }

    std::string TextShape(const Shape &s) {
        std::string body;
        for (const Para &p : s.paras)
            body += p.kind == ParaKind::Bullet || p.kind == ParaKind::Number ? ListXml(p, s.scale) : ParaXml(p, s.scale, s.center);
        const bool title = s.paras.size() == 1 && s.paras[0].kind == ParaKind::Title;
        const std::string frame = Frame(s, s.kind == ShapeKind::Code ? "grCode" : s.kind == ShapeKind::Output ? "grOutput" : title ? "grTitle" : "grText",
                                        "<draw:text-box>" + body + "</draw:text-box>");
        if (s.kind != ShapeKind::Output) return frame;
        // The results' bar, down their left edge.
        return "<draw:rect draw:style-name=\"grOutputBar\" draw:layer=\"layout\" svg:x=\"" + Cm(s.x) + "\" svg:y=\"" + Cm(s.y) +
               "\" svg:width=\"" + Cm(kOutputBarW) + "\" svg:height=\"" + Cm(s.h) + "\"/>" + frame;
    }

    std::string ImageShape(const Shape &s) {
        const Item &im = *s.item;
        const std::string name = "Pictures/image" + Num(static_cast<long>(pictures.size()) + 1) + "." + im.ext;
        pictures.emplace_back(name, im.bytes);
        std::string inner = "<draw:image xlink:href=\"" + name + "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"><text:p/></draw:image>";
        if (!im.alt.empty()) inner += "<svg:title>" + X(im.alt) + "</svg:title><svg:desc>" + X(im.alt) + "</svg:desc>";
        return Frame(s, "grImage", inner);
    }

    // A display equation: a formula object of its own, centred in its box.
    // Impress stretches a formula to fill its frame, so the frame is the
    // formula's own size (TexMathExtent), not the box's.
    std::string FormulaShape(const Shape &s) {
        const std::string name = "Object " + Num(static_cast<long>(formulas.size()) + 1);
        const double pt = BasePt(ParaKind::Math) * s.scale;
        formulas.push_back({name, TexToLibreOfficeMathMl(s.tex, true), pt});
        double wem = 0, hem = 0;
        TexMathExtent(s.tex, true, &wem, &hem);
        Shape f = s;
        f.w = std::min(s.w, static_cast<long>(wem * pt / 72.0 * kEmuIn));
        f.h = std::min(s.h, static_cast<long>(hem * pt / 72.0 * kEmuIn));
        f.x = s.x + (s.w - f.w) / 2;
        f.y = s.y + (s.h - f.h) / 2;
        return Frame(f, "grMath", "<draw:object xlink:href=\"./" + name + "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>");
    }

    // A formula paragraph (SplitFormulaParagraphs): its object, and a list
    // item's label as text beside it.
    std::string FormulaParaShape(const Shape &s) {
        const Para &p = s.paras[0];
        const double pt = BasePt(p.kind) * s.scale;
        const std::vector<FormulaLine> lines = FormulaParaLines(p, s.w, s.scale);
        std::string star, mml = "<mtable columnalign=\"left\">";
        for (size_t k = 0; k < lines.size(); ++k) {
            star += (k ? " newline " : "") + std::string("alignl");
            mml += "<mtr><mtd><mrow>";
            // The line's words run together into one string per style: each
            // separate element would get Math's own gap on either side.
            std::vector<Run> pieces;
            for (const FormulaUnit &u : lines[k].units) {
                const Run &r = u.style;
                if (!pieces.empty() && r.tex.empty() && pieces.back().tex.empty() && pieces.back().bold == r.bold &&
                    pieces.back().italic == r.italic && pieces.back().mono == r.mono && pieces.back().underline == r.underline &&
                    pieces.back().strike == r.strike && pieces.back().color == r.color) {
                    pieces.back().text += r.text;
                    continue;
                }
                pieces.push_back(r);
            }
            for (const Run &r : pieces) {
                if (!r.tex.empty()) {
                    star += " {" + TexToStarMath(r.tex) + "}";
                    mml += TexToMathMlBody(r.tex, false);
                    continue;
                }
                std::string t = r.text;
                // (Trailing space at a line's end is not part of the line.)
                if (&r == &pieces.back()) while (!t.empty() && t.back() == ' ') t.pop_back();
                if (t.empty()) continue;
                std::string word = (r.mono ? "font fixed " : "font sans ") + std::string(r.bold ? "bold " : "") + (r.italic ? "ital " : "") +
                                   StarMathQuote(t);
                if (r.underline) word = "underline{" + word + "}";
                if (r.strike) word = "overstrike{" + word + "}";
                if (!r.color.empty()) word = "color hex " + r.color + " {" + word + "}";
                star += " " + word;
                const char *variant = r.mono ? "monospace" : r.bold && r.italic ? "sans-serif-bold-italic" : r.bold ? "bold-sans-serif" : r.italic ? "sans-serif-italic" : "sans-serif";
                std::string edge = t;
                if (edge.front() == ' ') edge.replace(0, 1, "\u00a0");
                if (edge.back() == ' ') edge.replace(edge.size() - 1, 1, "\u00a0");
                mml += std::string("<mtext mathvariant=\"") + variant + "\"" + (r.color.empty() ? "" : " mathcolor=\"#" + r.color + "\"") + ">" + X(edge) + "</mtext>";
            }
            mml += "</mrow></mtd></mtr>";
        }
        mml += "</mtable>";
        const std::string name = "Object " + Num(static_cast<long>(formulas.size()) + 1);
        formulas.push_back({name,
                            "<math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\"inline\"><semantics>" + mml +
                                "<annotation encoding=\"StarMath 5.0\">" + X(star) + "</annotation></semantics></math>",
                            pt});
        double wem = 0;
        for (const FormulaLine &l : lines) wem = std::max(wem, l.w);
        const long space = static_cast<long>(pt * ParaSpacing(p.kind) / 72.0 * kEmuIn);
        Shape f = s;
        f.x = s.x + FormulaIndent(p);
        f.y = s.y + kFormulaPadY + space;
        f.w = std::min(s.w - FormulaIndent(p), static_cast<long>(wem * kFormulaWidthScale * pt / 72.0 * kEmuIn));
        f.h = static_cast<long>(FormulaLinesHeightEm(lines) * pt / 72.0 * kEmuIn);
        std::string o = Frame(f, "grMath", "<draw:object xlink:href=\"./" + name + "\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>");
        if (p.kind == ParaKind::Bullet || p.kind == ParaKind::Number) {
            // The label, a text box of the list's own, level with the first line.
            Shape label = s;
            label.x = s.x + kFormulaPadX + 324000L * p.level;
            label.w = 288000L + 2 * kFormulaPadX;
            // On the first line's baseline: its text's top is that line's
            // ascent less a line of text's.
            const long text_h = static_cast<long>(kFormulaTextEm * pt / 72.0 * kEmuIn);
            label.y = f.y + static_cast<long>((lines[0].asc - kFormulaTextAscEm) * pt / 72.0 * kEmuIn) - kFormulaPadY;
            label.h = text_h + 2 * kFormulaPadY;
            Run r;
            r.text = p.kind == ParaKind::Number ? Num(std::max(1, p.number)) + "." : (p.level % 2 ? "–" : "•");
            label.x -= kFormulaPadX;
            o += Frame(label, "grTitle", "<draw:text-box><text:p text:style-name=\"" + ParaStyle("fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\"") +
                                             "\"><text:span text:style-name=\"" + TextStyle(r, pt) + "\">" + X(r.text) + "</text:span></text:p></draw:text-box>");
        }
        return o;
    }

    std::string TableShape(const Shape &s) {
        const Item &t = *s.item;
        const std::vector<long> cols = TableColumns(t, s.w + 1, s.scale);
        std::string o = "<table:table>";
        for (size_t c = 0; c < cols.size(); ++c) {
            const std::string name = "co" + Num(static_cast<long>(auto_styles.size())) + "_" + Num(static_cast<long>(c));
            auto_styles += "<style:style style:name=\"" + name + "\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"" +
                           Cm(cols[c]) + "\"/></style:style>";
            o += "<table:table-column table:style-name=\"" + name + "\"/>";
        }
        const long row_h = s.h / std::max<long>(1, static_cast<long>(t.rows.size()));
        const std::string row_style = "ro" + Num(static_cast<long>(auto_styles.size()));
        auto_styles += "<style:style style:name=\"" + row_style + "\" style:family=\"table-row\"><style:table-row-properties style:row-height=\"" +
                       Cm(row_h) + "\"/></style:style>";
        for (size_t r = 0; r < t.rows.size(); ++r) {
            const bool head = static_cast<int>(r) < t.header_rows;
            o += "<table:table-row table:style-name=\"" + row_style + "\">";
            for (size_t c = 0; c < cols.size(); ++c) {
                Para p{ParaKind::Body, c < t.rows[r].size() ? t.rows[r][c] : std::vector<Run>()};
                for (Run &run : p.runs) run.bold = run.bold || head;
                const double pt = kTablePt * s.scale;
                std::string para = "<text:p text:style-name=\"" + ParaStyle("fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\"") + "\">";
                for (const Run &run : p.runs)
                    for (const Run &x : run.tex.empty() ? std::vector<Run>{run} : MathRuns(run))
                        para += "<text:span text:style-name=\"" + TextStyle(x, pt) + "\">" + OdfText(x.text) + "</text:span>";
                para += "</text:p>";
                o += "<table:table-cell table:style-name=\"" + std::string(head ? "ceHead" : "ceBody") + "\">" + para + "</table:table-cell>";
            }
            o += "</table:table-row>";
        }
        return Frame(s, "grTable", o + "</table:table>");
    }

    std::string Page(const PlacedSlide &ps, int n) {
        std::string o = "<draw:page draw:name=\"page" + Num(n) + "\" draw:style-name=\"dp1\" draw:master-page-name=\"Default\">";
        for (const Shape &s : ps.shapes) {
            if (s.kind == ShapeKind::Image) o += ImageShape(s);
            else if (s.kind == ShapeKind::Table) o += TableShape(s);
            else if (s.kind == ShapeKind::Math) o += FormulaShape(s);
            else if (s.kind == ShapeKind::Formula) o += FormulaParaShape(s);
            else o += TextShape(s);
        }
        return o + "</draw:page>";
    }
};

const char *kOdfNs =
    "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
    "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
    "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
    "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
    "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" "
    "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" "
    "xmlns:presentation=\"urn:oasis:names:tc:opendocument:xmlns:presentation:1.0\" "
    "office:version=\"1.3\"";

std::string OdpListStyles() {
    std::string bul = "<text:list-style style:name=\"LBul\">", num = "<text:list-style style:name=\"LNum\">";
    for (int l = 1; l <= 9; ++l) {
        char props[160];
        std::snprintf(props, sizeof props,
                      "<style:list-level-properties text:space-before=\"%.2fcm\" text:min-label-width=\"0.8cm\"/>", (l - 1) * 0.9);
        bul += "<text:list-level-style-bullet text:level=\"" + Num(l) + "\" text:bullet-char=\"" + std::string(l % 2 ? "•" : "–") +
               "\" text:bullet-relative-size=\"100%\">" + props + "<style:text-properties fo:font-family=\"'Liberation Sans'\" fo:font-size=\"100%\"/></text:list-level-style-bullet>";
        num += "<text:list-level-style-number text:level=\"" + Num(l) + "\" style:num-format=\"1\" style:num-suffix=\".\">" + props +
               "<style:text-properties fo:font-size=\"100%\"/></text:list-level-style-number>";
    }
    return bul + "</text:list-style>" + num + "</text:list-style>";
}

}  // namespace

void SetMathPictureRenderer(MathPictureRenderer renderer) { MathRenderer() = std::move(renderer); }

bool WritePptx(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error) {
    const Deck deck = Builder{doc, base_dir, {}}.Build();
    if (deck.slides.empty()) {
        if (error) *error = "no \\slide in the document to export";
        return false;
    }
    const std::vector<PlacedSlide> placed = Layout(deck);
    std::vector<zip::EntryToWrite> entries;
    int images = 0;
    std::string slide_ids, pres_rels, overrides;
    for (size_t k = 0; k < placed.size(); ++k) {
        PptxSlideWriter w;
        w.image_counter = &images;
        const std::string n = Num(static_cast<long>(k) + 1);
        entries.push_back({"ppt/slides/slide" + n + ".xml", w.Slide(placed[k])});
        entries.push_back({"ppt/slides/_rels/slide" + n + ".xml.rels", w.Rels()});
        for (auto &m : w.media) entries.push_back({m.first, m.second, true});
        slide_ids += "<p:sldId id=\"" + Num(256 + static_cast<long>(k)) + "\" r:id=\"rId" + Num(static_cast<long>(k) + 10) + "\"/>";
        pres_rels += "<Relationship Id=\"rId" + Num(static_cast<long>(k) + 10) + "\" Type=\"" + kRelNs + "slide\" Target=\"slides/slide" + n + ".xml\"/>";
        overrides += "<Override PartName=\"/ppt/slides/slide" + n +
                     ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
    }
    const std::string ct = "application/vnd.openxmlformats-officedocument.presentationml.";
    const std::string rels_head = std::string(kXmlHead) + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    std::vector<zip::EntryToWrite> head = {
        {"[Content_Types].xml",
         std::string(kXmlHead) +
             "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
             "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
             "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
             "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
             "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" + ct + "presentation.main+xml\"/>"
             "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"" + ct + "slideMaster+xml\"/>"
             "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"" + ct + "slideLayout+xml\"/>"
             "<Override PartName=\"/ppt/presProps.xml\" ContentType=\"" + ct + "presProps+xml\"/>"
             "<Override PartName=\"/ppt/viewProps.xml\" ContentType=\"" + ct + "viewProps+xml\"/>"
             "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\"" + ct + "tableStyles+xml\"/>"
             "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
             "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
             "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>" +
             overrides + "</Types>"},
        {"_rels/.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "officeDocument\" Target=\"ppt/presentation.xml\"/>"
             "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
             "<Relationship Id=\"rId3\" Type=\"" + kRelNs + "extended-properties\" Target=\"docProps/app.xml\"/></Relationships>"},
        {"docProps/core.xml", CoreProps(deck)},
        {"docProps/app.xml", std::string(kXmlHead) +
                                 "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\"><Application>mep</Application><Slides>" +
                                 Num(static_cast<long>(placed.size())) + "</Slides></Properties>"},
        {"ppt/presentation.xml",
         std::string(kXmlHead) + "<p:presentation " + kPNs + " saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>" +
             slide_ids + "</p:sldIdLst><p:sldSz cx=\"" + Num(kPageW) + "\" cy=\"" + Num(kPageH) +
             "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>"},
        {"ppt/_rels/presentation.xml.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>"
             "<Relationship Id=\"rId2\" Type=\"" + kRelNs + "theme\" Target=\"theme/theme1.xml\"/>"
             "<Relationship Id=\"rId3\" Type=\"" + kRelNs + "presProps\" Target=\"presProps.xml\"/>"
             "<Relationship Id=\"rId4\" Type=\"" + kRelNs + "viewProps\" Target=\"viewProps.xml\"/>"
             "<Relationship Id=\"rId5\" Type=\"" + kRelNs + "tableStyles\" Target=\"tableStyles.xml\"/>" + pres_rels + "</Relationships>"},
        {"ppt/presProps.xml", std::string(kXmlHead) + "<p:presentationPr " + kPNs + "/>"},
        {"ppt/viewProps.xml", std::string(kXmlHead) + "<p:viewPr " + kPNs + "/>"},
        {"ppt/tableStyles.xml", std::string(kXmlHead) +
                                    "<a:tblStyleLst xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>"},
        {"ppt/slideMasters/slideMaster1.xml",
         std::string(kXmlHead) + "<p:sldMaster " + kPNs + "><p:cSld><p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg><p:spTree>" +
             kGroupProps +
             "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" "
             "accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
             "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
             "<p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr sz=\"3200\"/></a:lvl1pPr></p:titleStyle>"
             "<p:bodyStyle><a:lvl1pPr><a:defRPr sz=\"2200\"/></a:lvl1pPr></p:bodyStyle>"
             "<p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"/></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>"},
        {"ppt/slideMasters/_rels/slideMaster1.xml.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
             "<Relationship Id=\"rId2\" Type=\"" + kRelNs + "theme\" Target=\"../theme/theme1.xml\"/></Relationships>"},
        {"ppt/slideLayouts/slideLayout1.xml",
         std::string(kXmlHead) + "<p:sldLayout " + kPNs + " type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\"><p:spTree>" + kGroupProps +
             "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>"},
        {"ppt/slideLayouts/_rels/slideLayout1.xml.rels",
         rels_head + "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>"},
        {"ppt/theme/theme1.xml", PptxTheme()},
    };
    head.insert(head.end(), entries.begin(), entries.end());
    return WriteFile(path, zip::BuildArchive(head), error);
}

bool WriteOdp(const Document &doc, const std::string &path, const std::string &base_dir, std::string *error) {
    Deck deck = Builder{doc, base_dir, {}}.Build();
    SplitFormulaParagraphs(deck);
    if (deck.slides.empty()) {
        if (error) *error = "no \\slide in the document to export";
        return false;
    }
    const std::vector<PlacedSlide> placed = Layout(deck);
    OdpWriter w;
    std::string pages;
    for (size_t k = 0; k < placed.size(); ++k) pages += w.Page(placed[k], static_cast<int>(k) + 1);
    const std::string frame_base = "draw:stroke=\"none\" draw:fill=\"none\" draw:textarea-vertical-align=\"top\" draw:auto-grow-height=\"false\" "
                                   "fo:padding-top=\"0.13cm\" fo:padding-bottom=\"0.13cm\" fo:padding-left=\"0.25cm\" fo:padding-right=\"0.25cm\"";
    const std::string graphics =
        "<style:style style:name=\"dp1\" style:family=\"drawing-page\"><style:drawing-page-properties presentation:background-visible=\"true\"/></style:style>"
        "<style:style style:name=\"grText\" style:family=\"graphic\"><style:graphic-properties " + frame_base + " style:shrink-to-fit=\"true\"/></style:style>"
        "<style:style style:name=\"grTitle\" style:family=\"graphic\"><style:graphic-properties " + frame_base +
        "/></style:style>"
        "<style:style style:name=\"grCode\" style:family=\"graphic\"><style:graphic-properties draw:stroke=\"solid\" svg:stroke-color=\"#d0d7de\" "
        "svg:stroke-width=\"0.03cm\" draw:fill=\"solid\" draw:fill-color=\"#f6f8fa\" draw:textarea-vertical-align=\"top\" draw:auto-grow-height=\"false\" "
        "fo:padding-top=\"0.13cm\" fo:padding-bottom=\"0.13cm\" fo:padding-left=\"0.25cm\" fo:padding-right=\"0.25cm\" style:shrink-to-fit=\"true\"/></style:style>"
        "<style:style style:name=\"grOutput\" style:family=\"graphic\"><style:graphic-properties draw:stroke=\"none\" draw:fill=\"none\" "
        "draw:textarea-vertical-align=\"top\" draw:auto-grow-height=\"false\" fo:padding-top=\"0.13cm\" fo:padding-bottom=\"0.13cm\" "
        "fo:padding-left=\"" + Cm(kOutputBarW + 137160) + "\" fo:padding-right=\"0.25cm\" style:shrink-to-fit=\"true\"/></style:style>"
        "<style:style style:name=\"grOutputBar\" style:family=\"graphic\"><style:graphic-properties draw:stroke=\"none\" draw:fill=\"solid\" "
        "draw:fill-color=\"#" + std::string(kOutputAccent) + "\"/></style:style>"
        "<style:style style:name=\"grImage\" style:family=\"graphic\"><style:graphic-properties draw:stroke=\"none\" draw:fill=\"none\"/></style:style>"
        "<style:style style:name=\"grTable\" style:family=\"graphic\"><style:graphic-properties draw:stroke=\"none\" draw:fill=\"none\"/></style:style>"
        "<style:style style:name=\"grMath\" style:family=\"graphic\"><style:graphic-properties draw:stroke=\"none\" draw:fill=\"none\"/></style:style>"
        "<style:style style:name=\"ceHead\" style:family=\"table-cell\"><style:graphic-properties draw:fill=\"solid\" draw:fill-color=\"#e8edf5\"/>"
        "<style:paragraph-properties fo:border=\"0.03pt solid #bfc5cc\"/></style:style>"
        "<style:style style:name=\"ceBody\" style:family=\"table-cell\"><style:graphic-properties draw:fill=\"none\"/>"
        "<style:paragraph-properties fo:border=\"0.03pt solid #bfc5cc\"/></style:style>";
    const std::string content = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content ") + kOdfNs +
                                "><office:automatic-styles>" + graphics + w.auto_styles + OdpListStyles() +
                                "</office:automatic-styles><office:body><office:presentation>" + pages +
                                "</office:presentation></office:body></office:document-content>";
    const std::string styles =
        std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles ") + kOdfNs +
        "><office:styles><style:default-style style:family=\"graphic\"><style:text-properties fo:font-family=\"'Liberation Sans'\" "
        "fo:font-size=\"18pt\" fo:color=\"#1f2328\"/></style:default-style></office:styles><office:automatic-styles>"
        "<style:page-layout style:name=\"PM1\"><style:page-layout-properties fo:margin-top=\"0cm\" fo:margin-bottom=\"0cm\" fo:margin-left=\"0cm\" "
        "fo:margin-right=\"0cm\" fo:page-width=\"" + Cm(kPageW) + "\" fo:page-height=\"" + Cm(kPageH) +
        "\" style:print-orientation=\"landscape\"/></style:page-layout>"
        "<style:style style:name=\"Mdp1\" style:family=\"drawing-page\"><style:drawing-page-properties draw:background-size=\"border\" "
        "draw:fill=\"solid\" draw:fill-color=\"#ffffff\"/></style:style></office:automatic-styles>"
        "<office:master-styles><draw:layer-set><draw:layer draw:name=\"layout\"/><draw:layer draw:name=\"background\"/>"
        "<draw:layer draw:name=\"backgroundobjects\"/><draw:layer draw:name=\"controls\"/><draw:layer draw:name=\"measurelines\"/></draw:layer-set>"
        "<style:master-page style:name=\"Default\" style:page-layout-name=\"PM1\" draw:style-name=\"Mdp1\"/></office:master-styles></office:document-styles>";
    const std::string meta = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta ") + kOdfNs +
                             "><office:meta><meta:generator>mep</meta:generator><dc:title>" + X(deck.title) + "</dc:title><meta:initial-creator>" +
                             X(deck.author) + "</meta:initial-creator></office:meta></office:document-meta>";
    std::string manifest = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
                           "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.presentation\"/>"
                           "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                           "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
                           "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>";
    for (const auto &p : w.pictures)
        manifest += "<manifest:file-entry manifest:full-path=\"" + p.first + "\" manifest:media-type=\"image/" +
                    (p.first.size() > 4 && p.first.compare(p.first.size() - 4, 4, ".png") == 0 ? "png" : "jpeg") + "\"/>";
    for (const OdpWriter::Formula &f : w.formulas)
        manifest += "<manifest:file-entry manifest:full-path=\"" + f.name + "/\" manifest:version=\"1.3\" "
                    "manifest:media-type=\"application/vnd.oasis.opendocument.formula\"/>"
                    "<manifest:file-entry manifest:full-path=\"" + f.name + "/content.xml\" manifest:media-type=\"text/xml\"/>"
                    "<manifest:file-entry manifest:full-path=\"" + f.name + "/settings.xml\" manifest:media-type=\"text/xml\"/>";
    manifest += "</manifest:manifest>";
    std::vector<zip::EntryToWrite> entries = {
        {"mimetype", "application/vnd.oasis.opendocument.presentation", true},
        {"META-INF/manifest.xml", manifest},
        {"content.xml", content},
        {"styles.xml", styles},
        {"meta.xml", meta},
    };
    for (const auto &p : w.pictures) entries.push_back({p.first, p.second, true});
    // A formula object is MathML, and its settings carry the size it is
    // set at (LibreOffice Math's base font height, in points).
    for (const OdpWriter::Formula &f : w.formulas) {
        entries.push_back({f.name + "/content.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + f.mathml});
        entries.push_back({f.name + "/settings.xml",
                           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-settings "
                           "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                           "xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\" office:version=\"1.3\"><office:settings>"
                           "<config:config-item-set config:name=\"ooo:configuration-settings\"><config:config-item config:name=\"BaseFontHeight\" "
                           "config:type=\"short\">" + Num(std::lround(f.pt)) + "</config:config-item></config:config-item-set></office:settings>"
                           "</office:document-settings>"});
    }
    return WriteFile(path, zip::BuildArchive(entries), error);
}

}  // namespace mepml
