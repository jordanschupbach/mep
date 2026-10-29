#include "pdf_text.h"

#include "pdf_encodings.h"

#include <cstring>

#include "pdf_content.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>

namespace pdftext {

namespace {

// ASCII-only case fold, applied to both haystack and query before a
// plain byte-wise substring search: since every UTF-8 continuation/lead
// byte for a non-ASCII codepoint is >= 0x80, it can never fall in
// 'A'-'Z' and so is never touched here -- multi-byte sequences pass
// through unchanged and byte offsets into the folded string still line
// up 1:1 with the original.
std::string AsciiLower(const std::string &s) {
    std::string out = s;
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// One page's glyphs, flattened into a searchable string -- see
// pdf_text.h's own top comment for the line/word-gap heuristics used to
// decide where synthetic (rect-less) separators get inserted.
struct PageTextBuild {
    std::string text;
    std::vector<int> char_glyph;       // parallel to `text` (one entry per BYTE): originating glyph index, or -1 for a synthetic separator
    std::vector<bool> glyph_new_line;  // parallel to `glyphs`: true if this glyph starts a new line-group (both text-separator and rect-merge boundary)
};

// One glyph's box in the orientation the page is read in. Glyph boxes
// are in the page's own unrotated point space, so on a /Rotate 90 page
// the axis text advances along is not the one the line/word heuristics
// below assume -- without this, searching a landscape page for a word
// finds nothing, because a synthetic separator lands between every pair
// of its letters. Same mapping as RotateForReading further down; see its
// comment for where the axes come from.
struct ReadBox {
    double left, right, top, bottom;
};
ReadBox ReadingBox(const pdfrender::TextGlyph &g, int rotate) {
    switch (rotate) {
        case 90: return ReadBox{g.bottom, g.top, -g.left, -g.right};
        case 180: return ReadBox{-g.right, -g.left, -g.bottom, -g.top};
        case 270: return ReadBox{-g.top, -g.bottom, g.right, g.left};
        default: return ReadBox{g.left, g.right, g.top, g.bottom};
    }
}

PageTextBuild BuildPageText(const std::vector<pdfrender::TextGlyph> &glyphs, int rotate) {
    PageTextBuild b;
    b.glyph_new_line.assign(glyphs.size(), false);
    if (glyphs.empty()) return b;
    b.glyph_new_line[0] = true;

    for (size_t i = 0; i < glyphs.size(); ++i) {
        const ReadBox g = ReadingBox(glyphs[i], rotate);
        double height = std::max(g.top - g.bottom, 1e-6);

        if (i > 0) {
            const ReadBox prev = ReadingBox(glyphs[i - 1], rotate);
            double prev_height = std::max(prev.top - prev.bottom, 1e-6);
            double center = (g.top + g.bottom) / 2.0;
            double prev_center = (prev.top + prev.bottom) / 2.0;
            double line_thresh = 0.5 * std::max(height, prev_height);
            bool new_line = std::fabs(center - prev_center) > line_thresh;
            bool word_gap = false;
            if (!new_line) {
                double gap = g.left - prev.right;
                double word_thresh = 0.20 * std::max(height, prev_height);
                word_gap = gap > word_thresh;
            }
            b.glyph_new_line[i] = new_line;
            if ((new_line || word_gap) && (b.text.empty() || b.text.back() != ' ')) {
                b.text.push_back(' ');
                b.char_glyph.push_back(-1);
            }
        }

        for (char c : glyphs[i].utf8_text) {
            b.text.push_back(c);
            b.char_glyph.push_back(static_cast<int>(i));
        }
    }
    return b;
}

PageTextBuild ExtractPageBuild(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                const pdfdoc::Page &page, std::vector<pdfrender::TextGlyph> *out_glyphs) {
    std::string content = pdfrender::GetPageContent(doc_data, doc_len, table, page);
    if (content.empty()) return {};
    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(1, 1);
    pdfrender::ExtractContentStreamText(content, canvas, pdfrender::Mat2D{}, page.resources, doc_data, doc_len,
                                         table, out_glyphs);
    return BuildPageText(*out_glyphs, page.rotate);
}

}  // namespace

std::string ExtractPageText(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                             const pdfdoc::Page &page) {
    std::vector<pdfrender::TextGlyph> glyphs;
    return ExtractPageBuild(doc_data, doc_len, table, page, &glyphs).text;
}

SearchIndex BuildSearchIndex(const unsigned char *doc_data, size_t doc_len, const pdfdoc::PdfDocument &doc) {
    SearchIndex index;
    index.pages.resize(static_cast<size_t>(std::max(doc.PageCount(), 0)));
    for (int p = 0; p < doc.PageCount(); ++p) {
        const pdfdoc::Page *page = doc.GetPage(p);
        if (!page) continue;
        std::vector<pdfrender::TextGlyph> glyphs;
        PageTextBuild build = ExtractPageBuild(doc_data, doc_len, doc.Xref(), *page, &glyphs);
        SearchPage &out = index.pages[static_cast<size_t>(p)];
        out.text = std::move(build.text);
        out.lower = AsciiLower(out.text);
        out.char_glyph = std::move(build.char_glyph);
        out.glyph_new_line = std::move(build.glyph_new_line);
        out.glyph_boxes.reserve(glyphs.size());
        for (const pdfrender::TextGlyph &g : glyphs)
            out.glyph_boxes.push_back({static_cast<float>(g.left), static_cast<float>(g.top), static_cast<float>(g.right),
                                       static_cast<float>(g.bottom)});
    }
    return index;
}

namespace {

bool IsUtf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Up to ~kBefore bytes before and ~kAfter after [pos, end), cut at a space
// where there is one near the edge and never inside a UTF-8 sequence, with
// an ellipsis where the page's text goes on.
void FillContext(const std::string &text, size_t pos, size_t end, PdfTextMatch &m) {
    constexpr size_t kBefore = 16, kAfter = 100;
    size_t from = pos > kBefore ? pos - kBefore : 0;
    size_t to = std::min(text.size(), end + kAfter);
    if (from > 0) {
        const size_t sp = text.find(' ', from);
        if (sp != std::string::npos && sp < pos) from = sp + 1;
        while (from < pos && IsUtf8Continuation(text[from])) ++from;
    }
    if (to < text.size()) {
        const size_t sp = text.rfind(' ', to);
        if (sp != std::string::npos && sp >= end) to = sp;
        while (to > end && to < text.size() && IsUtf8Continuation(text[to])) --to;
    }
    const std::string lead = from > 0 ? "..." : "";
    m.context = lead + text.substr(from, to - from) + (to < text.size() ? "..." : "");
    m.context_hl_start = static_cast<int>(lead.size() + (pos - from));
    m.context_hl_end = static_cast<int>(lead.size() + (end - from));
}

}  // namespace

std::vector<PdfTextMatch> Search(const SearchIndex &index, const std::string &query, size_t context_limit) {
    std::vector<PdfTextMatch> results;
    if (query.empty()) return results;
    const std::string query_lower = AsciiLower(query);
    for (size_t p = 0; p < index.pages.size(); ++p) {
        const SearchPage &page = index.pages[p];
        if (page.text.empty()) continue;
        const std::string &haystack_lower = page.lower;

        size_t pos = 0;
        while ((pos = haystack_lower.find(query_lower, pos)) != std::string::npos) {
            const size_t end = pos + query_lower.size();

            std::vector<int> covered;
            for (size_t i = pos; i < end; ++i) {
                int gi = page.char_glyph[i];
                if (gi < 0) continue;
                if (covered.empty() || covered.back() != gi) covered.push_back(gi);
            }

            if (!covered.empty()) {
                PdfTextMatch m;
                m.page = static_cast<int>(p);
                size_t k = 0;
                while (k < covered.size()) {
                    size_t j = k + 1;
                    const SearchPage::Box &g0 = page.glyph_boxes[static_cast<size_t>(covered[k])];
                    double left = static_cast<double>(g0.left), right = static_cast<double>(g0.right), top = static_cast<double>(g0.top),
                           bottom = static_cast<double>(g0.bottom);
                    while (j < covered.size() && !page.glyph_new_line[static_cast<size_t>(covered[j])]) {
                        const SearchPage::Box &g = page.glyph_boxes[static_cast<size_t>(covered[j])];
                        left = std::min(left, static_cast<double>(g.left));
                        right = std::max(right, static_cast<double>(g.right));
                        top = std::max(top, static_cast<double>(g.top));
                        bottom = std::min(bottom, static_cast<double>(g.bottom));
                        ++j;
                    }
                    m.rects_pt.push_back(PdfTextRectPt{left, top, right, bottom});
                    k = j;
                }
                if (results.size() < context_limit) FillContext(page.text, pos, end, m);
                results.push_back(std::move(m));
            }

            pos = end > pos ? end : pos + 1;
        }
    }
    return results;
}

std::vector<PdfTextMatch> Search(const unsigned char *doc_data, size_t doc_len, const pdfdoc::PdfDocument &doc,
                                  const std::string &query) {
    if (query.empty()) return {};
    return Search(BuildSearchIndex(doc_data, doc_len, doc), query);
}

std::vector<GlyphBox> PageGlyphBoxes(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                     const pdfdoc::Page &page) {
    std::vector<GlyphBox> out;
    std::string content = pdfrender::GetPageContent(doc_data, doc_len, table, page);
    if (content.empty()) return out;
    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(1, 1);
    std::vector<pdfrender::TextGlyph> glyphs;
    pdfrender::ExtractContentStreamText(content, canvas, pdfrender::Mat2D{}, page.resources, doc_data, doc_len, table,
                                         &glyphs);
    out.reserve(glyphs.size());
    for (const pdfrender::TextGlyph &g : glyphs)
        out.push_back(GlyphBox{g.left, g.top, g.right, g.bottom, g.utf8_text, g.actual_text, g.artifact});
    return out;
}

std::string ExpandLigatures(const std::string &s) {
    static const char *const kLetters[] = {"ff", "fi", "fl", "ffi", "ffl"};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (i + 2 < s.size() && c0 == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xAC &&
            static_cast<unsigned char>(s[i + 2]) >= 0x80 && static_cast<unsigned char>(s[i + 2]) <= 0x84) {
            out += kLetters[static_cast<unsigned char>(s[i + 2]) - 0x80];
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool EndsWithLetterHyphen(const std::string &s) {
    return s.size() >= 2 && s.back() == '-' && std::isalpha(static_cast<unsigned char>(s[s.size() - 2])) != 0;
}

namespace {

// The same glyph boxes seen from the orientation the page is actually
// read in.
//
// Glyph boxes are, and must stay, in the page's own unrotated point
// space: annotation quads round-trip through that space losslessly, so
// changing it would change what a saved highlight means. But a
// /Rotate 90 page's text runs *vertically* in that space -- the content
// is laid out so that rotating the page for display makes it read
// across -- and the row grouper buckets glyphs by `bottom`, which on
// such a page is the axis text advances along rather than the one it
// steps down. The result was a landscape page extracted one character
// per line.
//
// So grouping happens on a rotated view of the geometry, chosen to
// satisfy what the grouper assumes: reading runs left to right by
// increasing `left`, lines step down the page by decreasing `top`, and
// `top >= bottom`. Indices are untouched, so every row still refers to
// the caller's own array, and each row's reported top/bottom is
// recomputed in the original space afterwards.
//
// Derived from pdf_content.cpp's PageToDeviceMatrix for each /Rotate
// value: that maps (x,y) to (device_x, device_y), and these are the
// axes it puts reading and line advance on.
std::vector<GlyphBox> RotateForReading(const std::vector<GlyphBox> &glyphs, int rotate) {
    std::vector<GlyphBox> out;
    out.reserve(glyphs.size());
    for (const GlyphBox &g : glyphs) {
        GlyphBox r = g;
        switch (rotate) {
            case 90:
                // device_x = y, device_y = x: read along +y, step along +x.
                r.left = g.bottom;
                r.right = g.top;
                r.top = -g.left;
                r.bottom = -g.right;
                break;
            case 180:
                // device_x = -x, device_y = y: read along -x, step along +y.
                r.left = -g.right;
                r.right = -g.left;
                r.top = -g.bottom;
                r.bottom = -g.top;
                break;
            case 270:
                // device_x = -y, device_y = -x: read along -y, step along -x.
                r.left = -g.top;
                r.right = -g.bottom;
                r.top = g.right;
                r.bottom = g.left;
                break;
            default:
                break;
        }
        out.push_back(r);
    }
    return out;
}

}  // namespace

namespace {

// Whether an /ActualText span should be taken at its word.
//
// Spec 14.9.4 makes /ActualText the text a span really is, and both this
// code and poppler honour it by emitting it once for the whole span
// instead of the glyphs' own text. That is right for what it is for --
// an "fi" ligature, a dropped cap, a decorative glyph standing in for a
// word.
//
// Real producers also do something else with it: a tagged list wraps a
// whole item, marker and body together, in one marked-content sequence
// whose /ActualText is just the bullet. Honouring that erases the item.
// It is where "I highlighted a bulleted item and the model only knew it
// was bulleted" comes from -- the passage genuinely was one bullet
// character and nothing else.
//
// So a span is honoured only when its replacement is a plausible
// stand-in for what it replaces: at most a handful of glyphs, or a
// replacement long enough to be standing in for them rather than
// labelling them. A two-character /ActualText across forty glyphs is a
// marker, not a transcription.
//
// Span identity here is "a consecutive run of glyphs carrying the same
// /ActualText", which is what the geometry gives us -- GlyphBox does not
// carry the marked-content id that would make it exact.
// Decided once per span, not per glyph: a span is a consecutive run of
// indices carrying the same /ActualText, and the whole run shares one
// answer. Deciding per glyph would look only at the glyphs still ahead,
// so the tail of a long span would look short and be honoured -- which
// turned the last word of a wrongly-tagged list item into the bullet.
std::vector<char> HonourActualTextPerIndex(const std::vector<GlyphBox> &glyphs, const std::vector<int> &indices) {
    std::vector<char> out(indices.size(), 0);
    size_t i = 0;
    while (i < indices.size()) {
        const int gi = indices[i];
        if (gi < 0 || gi >= static_cast<int>(glyphs.size())) {
            ++i;
            continue;
        }
        const std::string &actual = glyphs[static_cast<size_t>(gi)].actual_text;
        if (actual.empty()) {
            ++i;
            continue;
        }
        // The run's extent, and how much text its glyphs carry of their own.
        size_t j = i;
        size_t own_text = 0;
        while (j < indices.size()) {
            const int g = indices[j];
            if (g < 0 || g >= static_cast<int>(glyphs.size())) break;
            if (glyphs[static_cast<size_t>(g)].actual_text != actual) break;
            own_text += glyphs[static_cast<size_t>(g)].text.size();
            ++j;
        }
        const size_t span = j - i;
        const bool honour = (span <= 4) || (actual.size() * 2 >= own_text);
        for (size_t k = i; k < j; ++k) out[k] = honour ? 1 : 0;
        i = j;
    }
    return out;
}

}  // namespace

namespace {

// Minimal UTF-8, local to the accent composition below -- the rest of
// this file only ever concatenates already-encoded pieces.
int DecodeSingleUtf8(const std::string &s) {
    if (s.empty()) return -1;
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    size_t len = 1;
    int cp = c0;
    if (c0 >= 0xF0) { len = 4; cp = c0 & 0x07; }
    else if (c0 >= 0xE0) { len = 3; cp = c0 & 0x0F; }
    else if (c0 >= 0xC0) { len = 2; cp = c0 & 0x1F; }
    else if (c0 >= 0x80) return -1;
    if (s.size() != len) return -1;  // exactly one character, or not interesting
    for (size_t i = 1; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (c & 0x3F);
    }
    return cp;
}

void AppendUtf8(std::string *out, int cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The last whole character of `text`, and where it starts.
int LastCodepoint(const std::string &text, size_t *start) {
    if (text.empty()) return -1;
    size_t i = text.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) --i;
    *start = i;
    return DecodeSingleUtf8(text.substr(i));
}

// Whether `g` is an accent drawn *over* `prev` rather than beside it:
// its ink sits inside the other glyph's horizontal span and higher up.
// That is how a font without precomposed letters builds an accented
// one, and it is the only way to tell such a pair from a real
// apostrophe or prime that merely follows a letter.
bool SitsOver(const GlyphBox &accent, const GlyphBox &base) {
    const double base_w = base.right - base.left;
    if (base_w <= 0) return false;
    const double overlap = std::min(accent.right, base.right) - std::max(accent.left, base.left);
    if (overlap < 0.5 * (accent.right - accent.left)) return false;  // mostly inside
    return accent.top > base.top - 0.25 * (base.top - base.bottom);  // and riding high
}

}  // namespace

// One visual row as one line of text: word gaps inserted from the
// horizontal geometry, but no line breaks, because a row is by
// definition one line however many baselines its glyphs sit on.
std::string JoinGlyphRowText(const std::vector<GlyphBox> &glyphs, const std::vector<int> &indices, int rotate) {
    // The gap and baseline-step tests below are about the direction
    // text reads, so on a rotated page they need the rotated view of
    // the geometry -- otherwise they test the line-advance axis for
    // word gaps and a landscape page comes out as "T his page".
    const std::vector<GlyphBox> rot = (rotate == 0) ? std::vector<GlyphBox>{} : RotateForReading(glyphs, rotate);
    const std::vector<GlyphBox> &geo = (rotate == 0) ? glyphs : rot;
    const std::vector<char> honour = HonourActualTextPerIndex(glyphs, indices);
    std::string text;
    bool have_prev = false;
    GlyphBox prev;
    std::string last_actual;
    bool have_actual = false;
    for (size_t ii = 0; ii < indices.size(); ++ii) {
        const int gi = indices[ii];
        if (gi < 0 || gi >= static_cast<int>(glyphs.size())) continue;
        const GlyphBox &g = glyphs[static_cast<size_t>(gi)];
        // An /ActualText span replaces its glyphs' own text, but only
        // when it is a plausible replacement for them -- see
        // HonourActualText.
        const bool use_actual = honour[ii] != 0;
        const std::string piece = ExpandLigatures(use_actual ? g.actual_text : g.text);
        const bool repeat = use_actual && have_actual && g.actual_text == last_actual;
        last_actual = use_actual ? g.actual_text : std::string();
        have_actual = true;
        const GlyphBox &gg = geo[static_cast<size_t>(gi)];
        // An accent drawn over the letter before it becomes one
        // character with it. Fonts without precomposed glyphs build
        // "\u00e9" as "e" plus a floating acute, which extracted as
        // "FRE\u00b4DE\u00b4RIC" -- not a word any search finds, and not
        // what a reader copying the name wants.
        const bool over_previous =
            have_prev && !repeat && !piece.empty() && !text.empty() && SitsOver(gg, prev);
        if (over_previous) {
            const int accent = DecodeSingleUtf8(piece);
            size_t at = 0;
            const int base = LastCodepoint(text, &at);
            if (accent > 0 && base > 0) {
                const int composed = pdfenc::ComposeAccented(base, accent);
                if (composed > 0) {
                    text.resize(at);
                    AppendUtf8(&text, composed);
                    // prev stays the letter: the accent is part of it
                    // now, and the next glyph's gap should be measured
                    // from the letter's own box.
                    continue;
                }
            }
            // No single-character form for this pair (an accent over a
            // letter that has none, or something else drawn on top).
            // The two still belong together, so the separator logic
            // below is skipped: a mark sitting on a letter is not a new
            // word, and the baseline-step rule would otherwise put a
            // space inside it.
            text += piece;
            prev = gg;
            have_prev = true;
            continue;
        }
        if (have_prev && !repeat) {
            const double height = std::max(gg.top - gg.bottom, 1e-6);
            const double prev_height = std::max(prev.top - prev.bottom, 1e-6);
            // A gap wide enough to be a word break, measured against
            // the taller of the two glyphs -- the same unit the rest of
            // this header's heuristics use. Measuring against the
            // shorter one instead sets the bar so low that the ordinary
            // letter spacing beside a capital reads as a word break
            // ("T his page carries a /R otate entry").
            //
            // 0.20 of a glyph box, not the 0.25 this started at: a glyph
            // box is 0.95em tall (0.75 ascent + 0.2 descent), so 0.25 of
            // it is 0.2375em -- and TeX shrinks the spaces in a
            // justified line below that. A real justified line in
            // test/pdf_fixtures/cid_unicode.pdf has 0.232em spaces, so
            // every word on it ran together ("Thispageistypesetby").
            // Letter spacing within a word in the same fixture is
            // exactly 0, and kerning generally stays under 0.05em, so
            // there is a wide margin below a shrunk space to aim at.
            // The em, as well as it can be known. Normally the glyph
            // box's height is it (0.95em by construction), and it is
            // bigger than any single glyph's width, so this is just the
            // height.
            //
            // The widths are there because a Type 3 font can report a
            // nonsense height: its glyph space is defined by /FontMatrix
            // rather than the usual 1/1000 em, which the vertical
            // estimate does not account for. One real book's Type 3
            // glyphs measured 5.5pt wide and 0.114pt tall, which put the
            // word-gap threshold at 0.023pt -- so the ordinary 0.24pt
            // space between two letters read as a word break and
            // "number" came out as "numb er". Taking the larger
            // dimension costs nothing where the height is right and
            // rescues the case where it is not.
            const double unit = std::max({height, prev_height, g.right - g.left, prev.right - prev.left});
            const bool gap = gg.left - prev.right > 0.20 * unit;
            // Stepping onto a different baseline within the row, onto
            // something that is a separate structure rather than a
            // sub- or superscript of what precedes it.
            //
            // The telling difference is horizontal, not vertical. By
            // offset alone the two cases are indistinguishable: in real
            // pdflatex output an inline superscript sits 0.60 of its own
            // height off the baseline and a displayed limit 0.645, so no
            // threshold separates them. But a displayed limit is
            // *centred under its operator* and so overlaps it in x,
            // while a sub/superscript sits beside its base and does not.
            // Since a row is ordered main-baseline-first, that overlap
            // shows up as x stepping backwards.
            //
            // Getting this right is what turns "lim" and its own limit
            // from the single run-together word "lim\u2206t" into the two
            // words a reader (or a model being handed the passage) needs,
            // while leaving "x_i" as the word "xi" -- which is how
            // poppler reads it too.
            const bool off_baseline = std::fabs(gg.bottom - prev.bottom) > 0.3 * unit;
            const bool steps_back = gg.left < prev.right - 0.05 * unit;
            const bool step = off_baseline && steps_back;
            if ((gap || step) && !text.empty() && text.back() != ' ') text += ' ';
        }
        if (!repeat) text += piece;
        prev = gg;
        have_prev = true;
    }
    return text;
}

std::string JoinGlyphText(const std::vector<GlyphBox> &glyphs, const std::vector<int> &indices) {
    const std::vector<char> honour = HonourActualTextPerIndex(glyphs, indices);
    std::string text;
    bool have_prev = false;
    GlyphBox prev;
    std::string last_actual;
    for (size_t ii = 0; ii < indices.size(); ++ii) {
        const int gi = indices[ii];
        if (gi < 0 || gi >= static_cast<int>(glyphs.size())) continue;
        const GlyphBox &g = glyphs[static_cast<size_t>(gi)];
        // A sequence with /ActualText reads as that text, once -- every
        // glyph in the sequence carries the same string -- but only when
        // it is a plausible replacement for them (HonourActualText).
        const bool use_actual = honour[ii] != 0;
        const std::string piece = ExpandLigatures(use_actual ? g.actual_text : g.text);
        const bool repeat = use_actual && have_prev && g.actual_text == last_actual;
        last_actual = use_actual ? g.actual_text : std::string();
        if (have_prev) {
            const double height = std::max(g.top - g.bottom, 1e-6);
            const double prev_height = std::max(prev.top - prev.bottom, 1e-6);
            const double centre = (g.top + g.bottom) / 2, prev_centre = (prev.top + prev.bottom) / 2;
            const bool new_line = std::fabs(centre - prev_centre) > 0.5 * std::max(height, prev_height);
            const bool gap = !new_line && g.left - prev.right > 0.25 * std::max(height, prev_height);
            if (new_line && EndsWithLetterHyphen(text) && !piece.empty() &&
                std::islower(static_cast<unsigned char>(piece[0]))) {
                text.pop_back();  // "investiga-" + "tor" is one word
            } else if (new_line) {
                if (!text.empty() && text.back() == ' ') text.pop_back();
                text += '\n';
            } else if (gap && !text.empty() && text.back() != ' ' && text.back() != '\n') {
                text += ' ';
            }
        }
        if (!repeat) text += piece;
        prev = g;
        have_prev = true;
    }
    return text;
}

namespace {

// Order one visual row's glyphs the way the row reads, rather than
// purely left to right.
//
// Sorting a row by `left` alone is right for prose and wrong for
// anything with a subscript. A row holding more than one baseline --
// `\lim` with its limit beneath, a summation with its indices, a
// fraction's numerator over its denominator -- interleaves the two
// baselines when sorted by x, and the text assembled from that order
// comes out shredded: the limit row of a real thesis page extracted as
// "\u2206 / li / t / m / \u21920" rather than "lim \u2206t\u21920",
// because every hop between the two baselines reads as a line break.
//
// So: the glyphs on the row's dominant baseline are the main sequence,
// and everything off it is a satellite. Satellites are clustered by
// their own baseline and adjacency, and each cluster is emitted
// immediately after the main glyph it belongs to -- the last one that
// starts at or before the cluster ends, which is the base a sub- or
// superscript is attached to, and for a centred limit is the operator
// it sits under. Clusters sharing an anchor come out top to bottom, so
// a superscript precedes a subscript.
void OrderRowGlyphs(const std::vector<GlyphBox> &glyphs, double h_med, std::vector<int> *row) {
    const auto left_of = [&](int i) { return glyphs[static_cast<size_t>(i)].left; };
    const auto right_of = [&](int i) { return glyphs[static_cast<size_t>(i)].right; };
    const auto base_of = [&](int i) { return glyphs[static_cast<size_t>(i)].bottom; };

    std::sort(row->begin(), row->end(), [&](int l, int r) { return left_of(l) < left_of(r); });
    if (row->size() < 2) return;

    // The row's dominant baseline: the one the most glyphs share, to
    // within a tolerance tight enough that a superscript's own baseline
    // cannot merge with it.
    const double tol = 0.15 * h_med;
    std::vector<double> bases;
    bases.reserve(row->size());
    for (int gi : *row) bases.push_back(base_of(gi));
    std::vector<double> sorted = bases;
    std::sort(sorted.begin(), sorted.end());
    double dom = sorted.front();
    size_t best = 0;
    for (size_t i = 0; i < sorted.size();) {
        size_t j = i;
        while (j < sorted.size() && sorted[j] - sorted[i] <= tol) ++j;
        if (j - i > best) {
            best = j - i;
            dom = sorted[i];
        }
        i = j;
    }

    std::vector<int> main_seq;
    std::vector<int> satellites;
    for (int gi : *row) {
        if (std::fabs(base_of(gi) - dom) <= tol)
            main_seq.push_back(gi);
        else
            satellites.push_back(gi);
    }
    // Nothing off the baseline, or nothing on it: the x order already is
    // the reading order.
    if (satellites.empty() || main_seq.empty()) return;

    // Cluster the satellites: same baseline and horizontally adjacent
    // (no main-sequence-sized gap between them) means one run of
    // sub/superscript text, e.g. the three glyphs of "i=1".
    struct Cluster {
        std::vector<int> glyphs;
        double base = 0, right = 0, top = 0;
    };
    std::vector<Cluster> clusters;
    for (int gi : satellites) {
        Cluster *into = nullptr;
        for (Cluster &c : clusters) {
            if (std::fabs(c.base - base_of(gi)) <= tol && left_of(gi) - c.right <= 0.6 * h_med) {
                into = &c;
                break;
            }
        }
        if (into == nullptr) {
            clusters.push_back(Cluster{});
            into = &clusters.back();
            into->base = base_of(gi);
            into->top = glyphs[static_cast<size_t>(gi)].top;
        }
        into->glyphs.push_back(gi);
        into->right = std::max(into->right, right_of(gi));
        into->top = std::max(into->top, glyphs[static_cast<size_t>(gi)].top);
    }

    // Anchor each cluster to a main glyph, then emit the row.
    std::vector<std::vector<const Cluster *>> attached(main_seq.size());
    for (const Cluster &c : clusters) {
        size_t anchor_at = 0;
        for (size_t i = 0; i < main_seq.size(); ++i)
            if (left_of(main_seq[i]) <= c.right) anchor_at = i;
        attached[anchor_at].push_back(&c);
    }
    std::vector<int> out;
    out.reserve(row->size());
    for (size_t i = 0; i < main_seq.size(); ++i) {
        out.push_back(main_seq[i]);
        std::vector<const Cluster *> &here = attached[i];
        std::sort(here.begin(), here.end(), [](const Cluster *l, const Cluster *r) {
            if (l->top != r->top) return l->top > r->top;  // superscript before subscript
            return l->right < r->right;
        });
        for (const Cluster *c : here) out.insert(out.end(), c->glyphs.begin(), c->glyphs.end());
    }
    *row = std::move(out);
}

}  // namespace

// Join whole visual rows: exactly one newline between rows, and never
// one inside a row.
//
// JoinGlyphText has to infer line breaks from geometry, because it is
// handed a bare run of glyph indices with no structure. That inference
// is what shreds an equation: every step between a formula's baselines
// looks like a new line. A caller that already knows where the rows are
// should not pay for the guess, so this takes the rows and uses them.
namespace {

// Whether a row begins with a list marker -- a bullet of some kind, or
// a number or letter followed by a dot or bracket.
//
// Used only to decide whether that row's indentation is worth keeping.
// A paragraph's first line is indented too, and reproducing that would
// put stray spaces through ordinary prose; a list's indentation is the
// only thing that says which item is nested under which.
bool StartsWithListMarker(const std::string &line) {
    static const char *const kBullets[] = {"\u2022", "\u2013", "\u2014", "\u2023", "\u25e6",
                                           "\u00b7", "\u25aa", "\u2043", "*"};
    for (const char *b : kBullets)
        if (line.compare(0, std::strlen(b), b) == 0) return true;
    // "1." / "12)" / "a." / "iv)" -- a short run of alphanumerics then a
    // dot or bracket, then a space.
    size_t i = 0;
    while (i < line.size() && i < 5 && std::isalnum(static_cast<unsigned char>(line[i])) != 0) ++i;
    return i > 0 && i < line.size() && (line[i] == '.' || line[i] == ')');
}

}  // namespace

std::string JoinGlyphRows(const std::vector<GlyphBox> &glyphs, const std::vector<std::vector<int>> &rows,
                          int rotate) {
    // The leftmost edge of any row that starts with a list marker: the
    // outermost list level on this page, which deeper ones are measured
    // against.
    double base_indent = 0;
    bool have_base = false;
    for (const std::vector<int> &row : rows) {
        if (row.empty()) continue;
        const std::string line = JoinGlyphRowText(glyphs, row, rotate);
        if (line.empty() || !StartsWithListMarker(line)) continue;
        const double left = glyphs[static_cast<size_t>(row.front())].left;
        if (!have_base || left < base_indent) {
            base_indent = left;
            have_base = true;
        }
    }
    // One indent step, in points. Derived from the text size rather than
    // fixed, so it travels between a 9pt paper and a 14pt one.
    double step = 18.0;
    {
        std::vector<double> h;
        h.reserve(glyphs.size());
        for (const GlyphBox &g : glyphs) h.push_back(std::max(g.top - g.bottom, 1e-6));
        if (!h.empty()) {
            std::nth_element(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(h.size() / 2), h.end());
            step = std::max(6.0, h[h.size() / 2] * 1.5);
        }
    }

    std::string text;
    for (const std::vector<int> &row : rows) {
        std::string line = JoinGlyphRowText(glyphs, row, rotate);
        if (line.empty()) continue;
        // A nested list item keeps its indentation, so which item sits
        // under which survives being copied out. Only list rows: a
        // paragraph's own first-line indent is not structure, and
        // reproducing it would scatter spaces through prose.
        if (have_base && !row.empty() && StartsWithListMarker(line)) {
            const double left = glyphs[static_cast<size_t>(row.front())].left;
            const int depth = static_cast<int>((left - base_indent) / step + 0.5);
            if (depth > 0) line.insert(0, static_cast<size_t>(std::min(depth, 8)) * 2, ' ');
        }
        if (!text.empty()) {
            // A word broken over a row end is one word, same rule
            // JoinGlyphText applies between lines.
            if (EndsWithLetterHyphen(text) && std::islower(static_cast<unsigned char>(line[0]))) {
                text.pop_back();
            } else {
                text += '\n';
            }
        }
        text += line;
    }
    return text;
}

std::vector<GlyphRow> PageGlyphRows(const std::vector<GlyphBox> &glyphs, int rotate) {
    // 90/270 swap the reading and line-advance axes; 180 flips both.
    // Either way the grouping below wants the read orientation, and the
    // caller wants row extents back in its own space.
    if (rotate == 90 || rotate == 180 || rotate == 270) {
        const std::vector<GlyphBox> rotated = RotateForReading(glyphs, rotate);
        std::vector<GlyphRow> rows = PageGlyphRows(rotated, 0);
        for (GlyphRow &row : rows) {
            if (row.glyphs.empty()) continue;
            row.top = glyphs[static_cast<size_t>(row.glyphs.front())].top;
            row.bottom = glyphs[static_cast<size_t>(row.glyphs.front())].bottom;
            for (int gi : row.glyphs) {
                row.top = std::max(row.top, glyphs[static_cast<size_t>(gi)].top);
                row.bottom = std::min(row.bottom, glyphs[static_cast<size_t>(gi)].bottom);
            }
        }
        return rows;
    }

    std::vector<GlyphRow> rows;
    if (glyphs.empty()) return rows;

    // The page's dominant text size, which every threshold below is a
    // fraction of. Median rather than mean so a page's running header,
    // footnotes and footer (all smaller) don't drag it.
    std::vector<double> heights;
    heights.reserve(glyphs.size());
    for (const GlyphBox &g : glyphs) heights.push_back(std::max(g.top - g.bottom, 1e-6));
    std::nth_element(heights.begin(), heights.begin() + static_cast<std::ptrdiff_t>(heights.size() / 2), heights.end());
    const double h_med = heights[heights.size() / 2];

    // Step 1: bucket glyphs by baseline. Glyphs set on the same line of
    // type share a baseline exactly, so this needs only enough slack to
    // absorb a producer's rounding.
    struct Group {
        double base = 0;
        std::vector<int> glyphs;
    };
    std::vector<int> order(glyphs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int l, int r) { return glyphs[static_cast<size_t>(l)].bottom > glyphs[static_cast<size_t>(r)].bottom; });
    std::vector<Group> groups;
    for (int gi : order) {
        const double base = glyphs[static_cast<size_t>(gi)].bottom;
        if (!groups.empty() && groups.back().base - base <= 0.25 * h_med) {
            groups.back().glyphs.push_back(gi);
        } else {
            groups.push_back(Group{base, {gi}});
        }
    }

    // Step 2: fold satellites in. A sub/superscript, or the summation
    // sign it hangs off, is a *small* group close to a bigger one; two
    // lines of running text are two big groups a full line pitch apart.
    // Folding smallest-first into the nearest group at least its own
    // size, and only within 0.8 of the dominant text height, keeps those
    // two cases apart: leading is never that tight (a line pitch runs
    // ~1.3x the glyph box height even in dense typesetting), while a
    // subscript drops well under it.
    //
    // Smallest group first each round, so a sub-subscript settles onto
    // its subscript before that subscript settles onto the main line.
    // `host_must_be_bigger` is what stops two full text lines from
    // collapsing into each other: neither is a satellite of the other.
    auto fold = [&](double max_dist, bool host_must_be_bigger, size_t satellite_max) {
        for (;;) {
            std::vector<size_t> by_size(groups.size());
            for (size_t i = 0; i < by_size.size(); ++i) by_size[i] = i;
            std::stable_sort(by_size.begin(), by_size.end(),
                             [&](size_t l, size_t r) { return groups[l].glyphs.size() < groups[r].glyphs.size(); });
            bool merged = false;
            for (size_t from : by_size) {
                if (groups[from].glyphs.size() > satellite_max) continue;
                size_t to = groups.size();
                double best = max_dist;
                for (size_t j = 0; j < groups.size(); ++j) {
                    if (j == from) continue;
                    if (host_must_be_bigger && groups[j].glyphs.size() < groups[from].glyphs.size()) continue;
                    if (!host_must_be_bigger && groups[j].glyphs.size() <= satellite_max) continue;
                    const double d = std::fabs(groups[j].base - groups[from].base);
                    if (d > best) continue;
                    // A tie (equidistant neighbours above and below) goes
                    // to the bigger of the two, so the result doesn't
                    // depend on which was seen first.
                    if (d < best || to == groups.size() || groups[j].glyphs.size() > groups[to].glyphs.size()) {
                        best = d;
                        to = j;
                    }
                }
                if (to == groups.size()) continue;
                groups[to].glyphs.insert(groups[to].glyphs.end(), groups[from].glyphs.begin(), groups[from].glyphs.end());
                groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(from));
                merged = true;
                break;
            }
            if (!merged) return;
        }
    };
    fold(0.8 * h_med, true, std::numeric_limits<size_t>::max());
    // A lone leftover -- the index under a summation that ended up
    // further from its row than anything else is from anything -- joins
    // the nearest real row rather than becoming a row of its own that
    // the caret would stop on for one keystroke.
    fold(1.3 * h_med, false, 2);

    // Step 3: order, and classify.
    rows.reserve(groups.size());
    for (Group &g : groups) {
        OrderRowGlyphs(glyphs, h_med, &g.glyphs);
        GlyphRow row;
        row.glyphs = std::move(g.glyphs);
        row.top = glyphs[static_cast<size_t>(row.glyphs.front())].top;
        row.bottom = glyphs[static_cast<size_t>(row.glyphs.front())].bottom;
        for (int gi : row.glyphs) {
            row.top = std::max(row.top, glyphs[static_cast<size_t>(gi)].top);
            row.bottom = std::min(row.bottom, glyphs[static_cast<size_t>(gi)].bottom);
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const GlyphRow &l, const GlyphRow &r) { return l.top > r.top; });

    // The text block's own margins: the row start and row end shared by
    // more rows than any other, which on a page of justified prose is
    // the measure itself.
    // The value the most entries agree on, to within `tol`. Ties keep
    // the lowest, which only matters for a page too sparse to have a
    // real margin anyway.
    auto modal = [](std::vector<double> &vals, double tol) {
        if (vals.empty()) return 0.0;
        std::sort(vals.begin(), vals.end());
        double best = vals.front();
        size_t best_n = 0;
        for (size_t i = 0; i < vals.size();) {
            size_t j = i;
            while (j < vals.size() && vals[j] - vals[i] <= tol) ++j;
            if (j - i > best_n) {
                best_n = j - i;
                best = vals[i];
            }
            i = j;
        }
        return best;
    };
    std::vector<double> lefts, rights;
    lefts.reserve(rows.size());
    rights.reserve(rows.size());
    for (const GlyphRow &r : rows) {
        lefts.push_back(glyphs[static_cast<size_t>(r.glyphs.front())].left);
        double right = 0;
        for (int gi : r.glyphs) right = std::max(right, glyphs[static_cast<size_t>(gi)].right);
        rights.push_back(right);
    }
    // A point of slack on the margins: justified text lines up exactly,
    // but a producer's rounding can wobble the odd row. `modal` sorts
    // what it is given, so it gets copies -- `lefts`/`rights` stay
    // parallel to `rows` for the per-row test below.
    std::vector<double> lefts_sorted = lefts, rights_sorted = rights;
    const double margin_l = modal(lefts_sorted, 1.0);
    const double margin_r = modal(rights_sorted, 1.0);

    for (size_t i = 0; i < rows.size(); ++i) {
        GlyphRow &row = rows[i];
        // How far off the row's own dominant baseline its furthest glyph
        // sits. Running text with a subscript lands around 0.35 of the
        // text height; a displayed equation, with indices under big
        // operators, is comfortably past 0.7.
        std::vector<double> bases;
        bases.reserve(row.glyphs.size());
        for (int gi : row.glyphs) bases.push_back(glyphs[static_cast<size_t>(gi)].bottom);
        // Glyphs set on one line of type share a baseline exactly, so
        // this wants to be essentially an exact count, not a cluster --
        // slack here would let a superscript's own baseline merge with
        // the main one and hide the very thing being looked for.
        const double dom = modal(bases, 0.05 * h_med);
        double spread = 0;
        int off = 0;
        for (double b : bases) {
            spread = std::max(spread, std::fabs(b - dom));
            if (std::fabs(b - dom) > 0.3 * h_med) ++off;
        }
        // ...and whether the row is set apart from the text block at all.
        // A line of prose that happens to carry an inline fraction still
        // runs margin to margin; a displayed equation is indented,
        // centred, or both. Requiring this is what keeps the handful of
        // very formula-dense sentences in a maths book navigable.
        const double left = lefts[i];
        const bool full_measure = left <= margin_l + 0.5 * h_med && rights[i] >= margin_r - 0.5 * h_med;
        row.display_math = spread > 0.6 * h_med && off * 25 >= static_cast<int>(row.glyphs.size()) && !full_measure;
    }
    return rows;
}

std::vector<int> SelectionGlyphRange(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                     double by, int rotate) {
    std::vector<int> out;
    if (glyphs.empty()) return out;
    // Distance^2 from a point to a glyph box (0 if inside), y-up boxes.
    auto dist2 = [](const GlyphBox &g, double px, double py) {
        double dx = std::max({g.left - px, 0.0, px - g.right});
        double dy = std::max({g.bottom - py, 0.0, py - g.top});
        return dx * dx + dy * dy;
    };
    auto nearest = [&](double px, double py) {
        size_t best = 0;
        double best_d = dist2(glyphs[0], px, py);
        for (size_t i = 1; i < glyphs.size(); ++i) {
            double d = dist2(glyphs[i], px, py);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        return best;
    };
    const size_t ai = nearest(ax, ay), bi = nearest(bx, by);

    // The run between the two ends in VISUAL order, not in the order the
    // content stream happened to emit its glyphs.
    //
    // Taking the contiguous range of glyph *indices* is what this did,
    // and it is wrong on most real pages: a typeset page routinely emits
    // the body, then the running header, then the footnotes, so the
    // indices between two points in the body can include the header
    // (451 of the 485 pages of one real book jump back up the page at
    // least once -- see PageGlyphRows). A reader dragging over two
    // paragraphs got the page furniture in between as well.
    //
    // So the ends are located in the row structure and everything
    // between them, row by row, is taken. That is the same order the
    // keyboard caret moves in, so the two selection paths can no longer
    // disagree about what is selected.
    const std::vector<GlyphRow> rows = PageGlyphRows(glyphs, rotate);
    // Where each glyph sits visually: which row, and how far along it.
    std::vector<int> row_of(glyphs.size(), -1), col_of(glyphs.size(), -1);
    for (size_t r = 0; r < rows.size(); ++r) {
        const std::vector<int> &row = rows[r].glyphs;
        for (size_t c = 0; c < row.size(); ++c) {
            const size_t gi = static_cast<size_t>(row[c]);
            if (gi < row_of.size()) {
                row_of[gi] = static_cast<int>(r);
                col_of[gi] = static_cast<int>(c);
            }
        }
    }
    // A glyph PageGlyphRows somehow left out (it puts every glyph in
    // exactly one row, so this is belt and braces) falls back to the old
    // index-range behaviour rather than vanishing.
    if (row_of[ai] < 0 || row_of[bi] < 0) {
        const size_t lo = std::min(ai, bi), hi = std::max(ai, bi);
        out.reserve(hi - lo + 1);
        for (size_t i = lo; i <= hi; ++i) out.push_back(static_cast<int>(i));
        return out;
    }

    std::pair<int, int> start{row_of[ai], col_of[ai]}, end{row_of[bi], col_of[bi]};
    if (end < start) std::swap(start, end);
    for (int r = start.first; r <= end.first && r < static_cast<int>(rows.size()); ++r) {
        const std::vector<int> &row = rows[static_cast<size_t>(r)].glyphs;
        const int from = (r == start.first) ? start.second : 0;
        const int to = (r == end.first) ? end.second : static_cast<int>(row.size()) - 1;
        for (int c = from; c <= to && c < static_cast<int>(row.size()); ++c)
            if (c >= 0) out.push_back(row[static_cast<size_t>(c)]);
    }
    return out;
}

std::vector<PdfTextRectPt> SelectionRects(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                          double by, int rotate) {
    std::vector<PdfTextRectPt> out;
    const std::vector<int> range = SelectionGlyphRange(glyphs, ax, ay, bx, by, rotate);
    if (range.empty()) return out;
    const size_t lo = static_cast<size_t>(range.front()), hi = static_cast<size_t>(range.back());

    // Group the selected run into per-line rects, splitting on the same
    // vertical-center jump Search/BuildPageText use.
    PdfTextRectPt cur;
    bool have = false;
    double prev_center = 0, prev_height = 0;
    for (size_t i = lo; i <= hi; ++i) {
        const GlyphBox &g = glyphs[i];
        double height = std::max(g.top - g.bottom, 1e-6);
        double center = (g.top + g.bottom) / 2.0;
        bool new_line = have && std::fabs(center - prev_center) > 0.5 * std::max(height, prev_height);
        if (!have || new_line) {
            if (have) out.push_back(cur);
            cur = PdfTextRectPt{g.left, g.top, g.right, g.bottom};
            have = true;
        } else {
            cur.left = std::min(cur.left, g.left);
            cur.right = std::max(cur.right, g.right);
            cur.top = std::max(cur.top, g.top);
            cur.bottom = std::min(cur.bottom, g.bottom);
        }
        prev_center = center;
        prev_height = height;
    }
    if (have) out.push_back(cur);
    return out;
}

std::vector<PdfHighlightRect> MatchRectsForPage(const pdfdoc::PdfDocument &doc, int page_index, float px_per_pt,
                                                 const std::vector<PdfTextMatch> &matches) {
    std::vector<PdfHighlightRect> out;
    const pdfdoc::Page *page = doc.GetPage(page_index);
    if (!page) return out;
    pdfrender::Mat2D m = pdfrender::PageToDeviceMatrix(page->effective_box[0], page->effective_box[1],
                                                        page->effective_box[2], page->effective_box[3], page->rotate,
                                                        static_cast<double>(px_per_pt));
    for (size_t mi = 0; mi < matches.size(); ++mi) {
        if (matches[mi].page != page_index) continue;
        for (const PdfTextRectPt &r : matches[mi].rects_pt) {
            double x0, y0, x1, y1;
            pdfrender::Transform(m, r.left, r.top, &x0, &y0);
            pdfrender::Transform(m, r.right, r.bottom, &x1, &y1);
            PdfHighlightRect hr;
            hr.match_index = static_cast<int>(mi);
            hr.x0 = static_cast<float>(std::min(x0, x1));
            hr.x1 = static_cast<float>(std::max(x0, x1));
            hr.y0 = static_cast<float>(std::min(y0, y1));
            hr.y1 = static_cast<float>(std::max(y0, y1));
            out.push_back(hr);
        }
    }
    return out;
}

}  // namespace pdftext
