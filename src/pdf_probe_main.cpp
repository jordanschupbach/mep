// mep-pdf-probe -- a headless CLI over mep's own PDF stack, so PDF
// behaviour is observable and measurable from a script instead of only
// through a running editor.
//
// Why this exists: every pdf_*_test.cpp builds its fixtures as
// hand-assembled byte strings, which is exactly right for unit-testing
// the parser but says nothing about how the stack does on a real
// typeset document. `mep` itself has no headless PDF mode (`mep --help`
// offers only --export-org), so measuring extraction or render accuracy
// over a corpus of real files had no entry point at all. This is it:
// one subcommand per thing worth measuring, text on stdout, so
// tools/pdf_accuracy.py can diff mep against poppler over hundreds of
// files.
//
// Deliberately built on the low-level modules (pdfdoc::PdfDocument +
// pdftext + pdfrender) rather than the pdf_doc.h facade, so its link
// set matches mep-pdf-text-test's and it pulls in no annotation/
// outline/a11y machinery it never uses. The one exception is `render`,
// which reproduces PdfDoc::RenderPage's exact composition (same
// effective_box, same PageToDeviceMatrix, same 8192 clamp) so what it
// writes out is what the viewer would show.

#include "pdf_content.h"
#include "pdf_document.h"
#include "pdf_text.h"
#include "text_quality.h"
#include "png_codec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

const unsigned char *B(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

bool ReadFile(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

// A string safe to put in one TSV field: the probe's output is parsed
// line-by-line by tools/pdf_accuracy.py, so a glyph whose own text is a
// tab or a newline must not be able to forge a column or a row.
std::string Esc(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            default: out += c;
        }
    }
    return out;
}

struct Args {
    std::string cmd;
    std::string path;
    int page = -1;  // -1 == every page
    double scale = 2.0;
    std::string out;
    std::string order = "visual";
    std::string query;
    double fx = 0, fy = 0, tx = 0, ty = 0;
    bool have_from = false, have_to = false;
};

bool ParsePair(const char *s, double *a, double *b) {
    const char *comma = std::strchr(s, ',');
    if (!comma) return false;
    *a = std::atof(std::string(s, static_cast<size_t>(comma - s)).c_str());
    *b = std::atof(comma + 1);
    return true;
}

void Usage() {
    std::fprintf(stderr,
                 "usage: mep-pdf-probe <command> <file.pdf> [options]\n"
                 "\n"
                 "commands:\n"
                 "  info                       page count and per-page size in points\n"
                 "  text    [--page N] [--order visual|stream]\n"
                 "                             extracted page text (visual order by default,\n"
                 "                             i.e. what PageText/the AI prompt would see)\n"
                 "  glyphs  --page N           one TSV row per glyph: box, text, actual_text,\n"
                 "                             mcid, artifact\n"
                 "  rows    --page N           visual rows, with the display_math flag\n"
                 "  select  --page N --from X,Y --to X,Y\n"
                 "                             the text a drag between two POINT-space\n"
                 "                             positions yields (what a yank would give)\n"
                 "  render  --page N --scale S --out f.png\n"
                 "                             rasterize one page exactly as the viewer would\n"
                 "  fonts                      per-page glyph tally: how many glyphs resolved\n"
                 "                             to text and how many came back empty\n"
                 "  search  --query TEXT       document-wide search hits\n"
                 "  quality [--page N]         whether a page's text looks like text at all,\n"
                 "                             and the numbers behind that verdict\n"
                 "\n"
                 "Point space is the page's own: y-up, origin bottom-left, 1 unit = 1/72in.\n");
}

// Every glyph on one page, in content-stream (reading) order, with the
// full TextGlyph payload -- pdftext::PageGlyphBoxes drops mcid/artifact
// on the floor (pdf_text.h's GlyphBox carries neither), and those are
// exactly what a structure-aware selection needs, so this goes to
// pdfrender directly.
std::vector<pdfrender::TextGlyph> RawGlyphs(const std::string &data, const pdfdoc::PdfDocument &doc,
                                            const pdfdoc::Page &page) {
    std::vector<pdfrender::TextGlyph> glyphs;
    pdfrender::Canvas scratch = pdfrender::Canvas::MakeWhite(1, 1);
    std::string content = pdfrender::GetPageContent(B(data), data.size(), doc.Xref(), page);
    pdfrender::ExtractContentStreamText(content, scratch, pdfrender::Mat2D{}, page.resources, B(data), data.size(),
                                        doc.Xref(), &glyphs);
    return glyphs;
}

int CmdInfo(const std::string &data, const pdfdoc::PdfDocument &doc) {
    (void)data;
    std::printf("pages\t%d\n", doc.PageCount());
    for (int i = 0; i < doc.PageCount(); ++i) {
        const pdfdoc::Page *p = doc.GetPage(i);
        std::printf("page\t%d\t%.2f\t%.2f\trotate=%d\n", i, doc.PageWidthPt(i), doc.PageHeightPt(i),
                    p ? p->rotate : 0);
    }
    return 0;
}

int CmdText(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    const bool visual = a.order != "stream";
    for (int i = 0; i < doc.PageCount(); ++i) {
        if (a.page >= 0 && i != a.page) continue;
        const pdfdoc::Page *p = doc.GetPage(i);
        if (!p) continue;
        std::string text;
        if (visual) {
            // The same composition PdfDoc::PageText uses: glyphs grouped
            // into visual rows, then every row's glyphs joined in row
            // order. Reading order would splice the running header into
            // the middle of a sentence.
            std::vector<pdftext::GlyphBox> glyphs = pdftext::PageGlyphBoxes(B(data), data.size(), doc.Xref(), *p);
            std::vector<pdftext::GlyphRow> rows = pdftext::PageGlyphRows(glyphs, p->rotate);
            std::vector<std::vector<int>> row_glyphs;
            row_glyphs.reserve(rows.size());
            for (const pdftext::GlyphRow &r : rows) row_glyphs.push_back(r.glyphs);
            text = pdftext::JoinGlyphRows(glyphs, row_glyphs, p->rotate);
        } else {
            text = pdftext::ExtractPageText(B(data), data.size(), doc.Xref(), *p);
        }
        if (a.page < 0) std::printf("=== page %d ===\n", i);
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::printf("\n");
    }
    return 0;
}

int CmdGlyphs(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    const pdfdoc::Page *p = doc.GetPage(a.page);
    if (!p) {
        std::fprintf(stderr, "no such page: %d\n", a.page);
        return 2;
    }
    std::vector<pdfrender::TextGlyph> glyphs = RawGlyphs(data, doc, *p);
    std::printf("#i\tleft\tbottom\tright\ttop\ttext\tactual_text\tmcid\tartifact\n");
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const pdfrender::TextGlyph &g = glyphs[i];
        std::printf("%zu\t%.3f\t%.3f\t%.3f\t%.3f\t%s\t%s\t%d\t%d\n", i, g.left, g.bottom, g.right, g.top,
                    Esc(g.utf8_text).c_str(), Esc(g.actual_text).c_str(), g.mcid, g.artifact ? 1 : 0);
    }
    return 0;
}

int CmdRows(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    const pdfdoc::Page *p = doc.GetPage(a.page);
    if (!p) {
        std::fprintf(stderr, "no such page: %d\n", a.page);
        return 2;
    }
    std::vector<pdftext::GlyphBox> glyphs = pdftext::PageGlyphBoxes(B(data), data.size(), doc.Xref(), *p);
    std::vector<pdftext::GlyphRow> rows = pdftext::PageGlyphRows(glyphs, p->rotate);
    std::printf("#row\ttop\tbottom\tnglyphs\tdisplay_math\ttext\n");
    for (size_t r = 0; r < rows.size(); ++r) {
        const pdftext::GlyphRow &row = rows[r];
        std::string text = pdftext::JoinGlyphRowText(glyphs, row.glyphs, p->rotate);
        std::printf("%zu\t%.3f\t%.3f\t%zu\t%d\t%s\n", r, row.top, row.bottom, row.glyphs.size(),
                    row.display_math ? 1 : 0, Esc(text).c_str());
    }
    return 0;
}

int CmdSelect(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    const pdfdoc::Page *p = doc.GetPage(a.page);
    if (!p) {
        std::fprintf(stderr, "no such page: %d\n", a.page);
        return 2;
    }
    if (!a.have_from || !a.have_to) {
        std::fprintf(stderr, "select needs --from X,Y and --to X,Y (point space)\n");
        return 2;
    }
    std::vector<pdftext::GlyphBox> glyphs = pdftext::PageGlyphBoxes(B(data), data.size(), doc.Xref(), *p);
    std::vector<int> range = pdftext::SelectionGlyphRange(glyphs, a.fx, a.fy, a.tx, a.ty);
    std::string text = pdftext::JoinGlyphText(glyphs, range);
    std::printf("glyphs\t%zu\n", range.size());
    std::printf("text\t%s\n", Esc(text).c_str());
    return 0;
}

int CmdRender(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    const pdfdoc::Page *p = doc.GetPage(a.page);
    if (!p) {
        std::fprintf(stderr, "no such page: %d\n", a.page);
        return 2;
    }
    if (a.out.empty()) {
        std::fprintf(stderr, "render needs --out <file.png>\n");
        return 2;
    }
    // Mirrors PdfDoc::RenderPage exactly, including its 8192 clamp, so
    // the PNG here is the viewer's own pixels and a diff against
    // pdftoppm measures the renderer rather than this tool.
    const double scale = a.scale;
    int w = static_cast<int>(std::lround(doc.PageWidthPt(a.page) * scale));
    int h = static_cast<int>(std::lround(doc.PageHeightPt(a.page) * scale));
    w = w < 1 ? 1 : (w > 8192 ? 8192 : w);
    h = h < 1 ? 1 : (h > 8192 ? 8192 : h);
    pdfrender::Canvas canvas = pdfrender::Canvas::MakeWhite(w, h);
    pdfrender::Mat2D ctm = pdfrender::PageToDeviceMatrix(p->effective_box[0], p->effective_box[1],
                                                          p->effective_box[2], p->effective_box[3], p->rotate, scale);
    pdfrender::PageContentStatus status;
    std::string content = pdfrender::GetPageContent(B(data), data.size(), doc.Xref(), *p, &status);
    pdfrender::RenderContentStream(content, canvas, ctm, p->resources, B(data), data.size(), doc.Xref());

    std::string png = png::Encode(w, h, 4, canvas.rgba.data(), w * 4);
    if (png.empty()) {
        std::fprintf(stderr, "PNG encode failed\n");
        return 1;
    }
    std::ofstream out(a.out, std::ios::binary);
    if (!out.good()) {
        std::fprintf(stderr, "cannot write %s\n", a.out.c_str());
        return 1;
    }
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    std::printf("wrote\t%s\t%d\t%d\n", a.out.c_str(), w, h);
    std::printf("streams\t%d\tfailed\t%d\n", status.streams_total, status.streams_failed);
    return 0;
}

// The cause-(2) metric: how many glyphs the interpreter *showed* versus
// how many survived into the text layer. pdf_content.cpp only pushes a
// TextGlyph when GetUnicodeText returned something non-empty, so a
// glyph whose font has no usable /ToUnicode and no AGL-resolvable name
// is simply absent here -- which is why this counts shown glyphs from
// the content stream separately rather than trusting the glyph list's
// own length.
int CmdFonts(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    long long total_kept = 0, total_empty_text = 0;
    std::printf("#page\tkept\tempty_text\n");
    for (int i = 0; i < doc.PageCount(); ++i) {
        if (a.page >= 0 && i != a.page) continue;
        const pdfdoc::Page *p = doc.GetPage(i);
        if (!p) continue;
        std::vector<pdfrender::TextGlyph> glyphs = RawGlyphs(data, doc, *p);
        long long empty_text = 0;
        for (const pdfrender::TextGlyph &g : glyphs)
            if (g.utf8_text.empty()) ++empty_text;
        total_kept += static_cast<long long>(glyphs.size());
        total_empty_text += empty_text;
        std::printf("%d\t%zu\t%lld\n", i, glyphs.size(), empty_text);
    }
    std::printf("total\t%lld\t%lld\n", total_kept, total_empty_text);
    return 0;
}

// Per page: whether its extracted text looks like text at all, and the
// numbers behind that. The threshold lives in text_quality.h; this is
// how it was chosen, by running it over a few hundred pages of real
// books and seeing where the garbage sat relative to everything else.
int CmdQuality(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    std::printf("#page\tdegenerate\tchars\tdistinct\tdigits\n");
    long long bad = 0, seen = 0;
    for (int i = 0; i < doc.PageCount(); ++i) {
        if (a.page >= 0 && i != a.page) continue;
        const pdfdoc::Page *p = doc.GetPage(i);
        if (!p) continue;
        std::vector<pdftext::GlyphBox> glyphs = pdftext::PageGlyphBoxes(B(data), data.size(), doc.Xref(), *p);
        std::vector<std::vector<int>> rows;
        for (pdftext::GlyphRow &r : pdftext::PageGlyphRows(glyphs, p->rotate)) rows.push_back(std::move(r.glyphs));
        const textq::Quality q = textq::Measure(pdftext::JoinGlyphRows(glyphs, rows, p->rotate));
        std::printf("%d\t%d\t%zu\t%zu\t%zu\n", i, q.Degenerate() ? 1 : 0, q.chars, q.distinct, q.digits);
        ++seen;
        if (q.Degenerate()) ++bad;
    }
    std::printf("total\t%lld\t%lld\n", bad, seen);
    return 0;
}

int CmdSearch(const Args &a, const std::string &data, const pdfdoc::PdfDocument &doc) {
    if (a.query.empty()) {
        std::fprintf(stderr, "search needs --query TEXT\n");
        return 2;
    }
    std::vector<pdftext::PdfTextMatch> matches = pdftext::Search(B(data), data.size(), doc, a.query);
    std::printf("matches\t%zu\n", matches.size());
    for (const pdftext::PdfTextMatch &m : matches) {
        for (const pdftext::PdfTextRectPt &r : m.rects_pt)
            std::printf("hit\t%d\t%.3f\t%.3f\t%.3f\t%.3f\n", m.page, r.left, r.bottom, r.right, r.top);
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        Usage();
        return 2;
    }
    Args a;
    a.cmd = argv[1];
    a.path = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string opt = argv[i];
        const bool has_next = (i + 1) < argc;
        if (opt == "--page" && has_next) {
            a.page = std::atoi(argv[++i]);
        } else if (opt == "--scale" && has_next) {
            a.scale = std::atof(argv[++i]);
        } else if (opt == "--out" && has_next) {
            a.out = argv[++i];
        } else if (opt == "--order" && has_next) {
            a.order = argv[++i];
        } else if (opt == "--query" && has_next) {
            a.query = argv[++i];
        } else if (opt == "--from" && has_next) {
            a.have_from = ParsePair(argv[++i], &a.fx, &a.fy);
        } else if (opt == "--to" && has_next) {
            a.have_to = ParsePair(argv[++i], &a.tx, &a.ty);
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", opt.c_str());
            return 2;
        }
    }

    std::string data;
    if (!ReadFile(a.path, &data)) {
        std::fprintf(stderr, "cannot read %s\n", a.path.c_str());
        return 1;
    }
    // PdfDocument::Load is void and tolerant by design; a file it could
    // make nothing of simply comes back with no pages.
    pdfdoc::PdfDocument doc;
    doc.Load(B(data), data.size());
    if (doc.PageCount() == 0) {
        std::fprintf(stderr, "cannot parse %s (no readable xref/trailer/page tree)\n", a.path.c_str());
        return 1;
    }

    // Commands that address one page need one; default to the first so
    // `glyphs f.pdf` means something useful rather than erroring.
    if (a.page < 0 && (a.cmd == "glyphs" || a.cmd == "rows" || a.cmd == "select" || a.cmd == "render")) a.page = 0;

    if (a.cmd == "info") return CmdInfo(data, doc);
    if (a.cmd == "text") return CmdText(a, data, doc);
    if (a.cmd == "glyphs") return CmdGlyphs(a, data, doc);
    if (a.cmd == "rows") return CmdRows(a, data, doc);
    if (a.cmd == "select") return CmdSelect(a, data, doc);
    if (a.cmd == "render") return CmdRender(a, data, doc);
    if (a.cmd == "fonts") return CmdFonts(a, data, doc);
    if (a.cmd == "search") return CmdSearch(a, data, doc);
    if (a.cmd == "quality") return CmdQuality(a, data, doc);

    std::fprintf(stderr, "unknown command: %s\n", a.cmd.c_str());
    Usage();
    return 2;
}
