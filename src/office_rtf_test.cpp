// mep-office-rtf-test: the office editor's RTF loader and saver
// (src/office_rtf.cpp). A hand-written, Word-shaped RTF must load into the
// paragraph/span model with its headings, runs, list markers and table,
// and what the saver writes must load back as the same document.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "office_doc.h"

namespace {
void Check(bool ok, const char *expression, int line) {
    if (ok) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::exit(1);
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Load(const std::string &rtf, OfficeDoc *doc) {
    std::string err;
    return LoadRtfFromMemory(reinterpret_cast<const unsigned char *>(rtf.data()), rtf.size(), *doc, err);
}

const DocSpan *SpanAt(const DocParagraph &p, int col) {
    for (const DocSpan &s : p.spans)
        if (s.start <= col && col < s.end) return &s;
    return nullptr;
}

const char *kSample =
    "{\\rtf1\\ansi\\ansicpg1252\\deff0\n"
    "{\\fonttbl{\\f0\\fswiss Arial;}{\\f1\\froman Times New Roman;}{\\f2\\fmodern Courier New;}}\n"
    "{\\colortbl;\\red255\\green0\\blue0;\\red255\\green255\\blue0;}\n"
    "{\\stylesheet{\\s0\\fs24 Normal;}{\\s1\\b\\fs36 heading 1;}{\\s2\\b\\fs32 heading 2;}}\n"
    "{\\info{\\title Not body text}}\n"
    "\\pard\\plain\\s1\\b\\fs36 A title\\par\n"
    "\\pard\\plain\\fs24 Plain, {\\b bold}, {\\i italic}, {\\f2 mono}, {\\f1 serif}, {\\cf1 red}, {\\highlight2 marked}, "
    "{\\fs36 big}, caf\\'e9 and \\u8212?dash\\u20320?.\\par\n"
    "\\pard\\plain\\qc Centred\\par\n"
    "\\pard\\plain\\s2\\b\\fs32 Second\\outlinelevel1\\par\n"
    "\\pard\\plain{\\*\\pn\\pnlvlblt{\\pntxtb\\bullet}}\\fi-360\\li720{\\pntext\\bullet\\tab}First item\\par\n"
    "\\pard\\plain\\fi-360\\li720{\\listtext 1.\\tab}Numbered\\par\n"
    "\\trowd\\trgaph108\\cellx4320\\cellx8640\n"
    "\\pard\\plain\\intbl a\\cell\\pard\\plain\\intbl b\\cell\\row\n"
    "\\trowd\\trgaph108\\cellx4320\\cellx8640\n"
    "\\pard\\plain\\intbl c\\cell\\pard\\plain\\intbl d\\cell\\row\n"
    "\\pard\\plain After the table, a line\\line break and a\\tab tab.\\par\n"
    "{\\header Not body text either\\par}\n"
    "}\n";

void TestLoad() {
    OfficeDoc doc;
    CHECK(Load(kSample, &doc));
    CHECK(doc.source_format == "rtf");
    // Title, runs, centred, heading 2, bullet, numbered, table anchor, after.
    CHECK(doc.paragraphs.size() >= 8);
    const DocParagraph &h1 = doc.paragraphs[0];
    CHECK(h1.text == "A title");
    CHECK(h1.heading_level == 1);
    CHECK(h1.spans.empty());  // the heading's bold and size are the style's
    const DocParagraph &runs = doc.paragraphs[1];
    CHECK(runs.heading_level == 0);
    const std::string want_runs = "Plain, bold, italic, mono, serif, red, marked, big, caf\xc3\xa9 and \xe2\x80\x94" "dash\xe4\xbd\xa0.";
    if (runs.text != want_runs) std::fprintf(stderr, "runs: [%s]\n", runs.text.c_str());
    CHECK(runs.text == want_runs);
    CHECK(SpanAt(runs, 0) == nullptr);
    const DocSpan *b = SpanAt(runs, static_cast<int>(runs.text.find("bold")));
    CHECK(b && b->fmt.bold && !b->fmt.italic);
    const DocSpan *i = SpanAt(runs, static_cast<int>(runs.text.find("italic")));
    CHECK(i && i->fmt.italic && !i->fmt.bold);
    const DocSpan *mono = SpanAt(runs, static_cast<int>(runs.text.find("mono")));
    CHECK(mono && mono->fmt.font_family == OfficeFontFamily::Mono);
    const DocSpan *serif = SpanAt(runs, static_cast<int>(runs.text.find("serif")));
    CHECK(serif && serif->fmt.font_family == OfficeFontFamily::Serif);
    const DocSpan *red = SpanAt(runs, static_cast<int>(runs.text.find("red")));
    CHECK(red && red->fmt.has_color && red->fmt.color_r == 255 && red->fmt.color_g == 0 && red->fmt.color_b == 0);
    const DocSpan *mark = SpanAt(runs, static_cast<int>(runs.text.find("marked")));
    CHECK(mark && mark->fmt.has_highlight && mark->fmt.highlight_r == 255 && mark->fmt.highlight_g == 255 && mark->fmt.highlight_b == 0);
    const DocSpan *big = SpanAt(runs, static_cast<int>(runs.text.find("big")));
    CHECK(big && big->fmt.font_size_pt == 18.0f);
    CHECK(doc.paragraphs[2].text == "Centred" && doc.paragraphs[2].align == DocParagraph::Align::Center);
    CHECK(doc.paragraphs[3].text == "Second" && doc.paragraphs[3].heading_level == 2);
    CHECK(doc.paragraphs[4].text == "First item" && doc.paragraphs[4].list_kind == DocParagraph::ListKind::Bullet);
    CHECK(doc.paragraphs[5].text == "Numbered" && doc.paragraphs[5].list_kind == DocParagraph::ListKind::Numbered);
    CHECK(doc.tables.size() == 1);
    CHECK(doc.tables[0].rows == 2 && doc.tables[0].cols == 2);
    CHECK(doc.tables[0].Cell(0, 0) == "a" && doc.tables[0].Cell(0, 1) == "b" && doc.tables[0].Cell(1, 0) == "c" && doc.tables[0].Cell(1, 1) == "d");
    CHECK(doc.paragraphs[6].table_ref == 0 && doc.paragraphs[6].text.empty());
    CHECK(doc.paragraphs[7].text == "After the table, a line\nbreak and a\ttab.");
    for (const DocParagraph &p : doc.paragraphs) CHECK(p.text.find("Not body text") == std::string::npos);
    // Not RTF at all.
    OfficeDoc not_rtf;
    std::string err;
    CHECK(!LoadRtfFromMemory(reinterpret_cast<const unsigned char *>("hello"), 5, not_rtf, err));
    CHECK(!err.empty());
}

void TestRoundTrip() {
    OfficeDoc doc;
    CHECK(Load(kSample, &doc));
    // Edits the editor makes: a justified paragraph, a struck run, a
    // second table column's text.
    doc.paragraphs[7].align = DocParagraph::Align::Justify;
    DocFormat strike;
    strike.strike = true;
    strike.underline = true;
    doc.paragraphs[7].spans.push_back({0, 5, strike});
    doc.tables[0].Cell(1, 1) = "dee {braces} \\slash";
    std::vector<unsigned char> bytes;
    std::string err;
    CHECK(SaveRtfToMemory(doc, bytes, err));
    const std::string rtf(bytes.begin(), bytes.end());
    CHECK(rtf.rfind("{\\rtf1", 0) == 0);
    CHECK(rtf.find("heading 1;") != std::string::npos);  // the style sheet names the headings
    OfficeDoc back;
    CHECK(Load(rtf, &back));
    CHECK(back.paragraphs.size() == doc.paragraphs.size());
    for (size_t k = 0; k < doc.paragraphs.size(); ++k) {
        const DocParagraph &a = doc.paragraphs[k], &b = back.paragraphs[k];
        if (a.text != b.text) std::fprintf(stderr, "paragraph %zu: [%s] -> [%s]\n", k, a.text.c_str(), b.text.c_str());
        CHECK(a.text == b.text);
        CHECK(a.heading_level == b.heading_level);
        CHECK(a.align == b.align);
        CHECK(a.list_kind == b.list_kind);
        CHECK((a.table_ref >= 0) == (b.table_ref >= 0));
        // Every character's look survives (spans may be split differently).
        for (int col = 0; col < static_cast<int>(a.text.size()); ++col) {
            const DocSpan *sa = SpanAt(a, col), *sb = SpanAt(b, col);
            const DocFormat fa = sa ? sa->fmt : DocFormat(), fb = sb ? sb->fmt : DocFormat();
            if (fa != fb) std::fprintf(stderr, "paragraph %zu col %d: look differs\n", k, col);
            CHECK(fa == fb);
        }
    }
    CHECK(back.tables.size() == 1 && back.tables[0].rows == 2 && back.tables[0].cols == 2);
    CHECK(back.tables[0].Cell(1, 1) == "dee {braces} \\slash");
}

void TestEmptyAndEdges() {
    // An empty document is one empty paragraph, and saves as RTF that loads.
    OfficeDoc doc;
    CHECK(Load("{\\rtf1\\ansi }", &doc));
    CHECK(doc.paragraphs.size() == 1 && doc.paragraphs[0].text.empty());
    std::vector<unsigned char> bytes;
    std::string err;
    CHECK(SaveRtfToMemory(doc, bytes, err));
    OfficeDoc back;
    CHECK(Load(std::string(bytes.begin(), bytes.end()), &back));
    CHECK(back.paragraphs.size() == 1);
    // Text after the last \par is a paragraph of its own; an unknown {\*
    // destination and a field's instruction are skipped, its result kept.
    OfficeDoc two;
    CHECK(Load("{\\rtf1\\ansi one\\par {\\*\\mystery hidden}{\\field{\\*\\fldinst HYPERLINK \"x\"}{\\fldrslt shown}} two}", &two));
    CHECK(two.paragraphs.size() == 2);
    CHECK(two.paragraphs[0].text == "one");
    CHECK(two.paragraphs[1].text == "shown two");
}
}  // namespace

int main(int argc, char **argv) {
    // `mep-office-rtf-test IN.rtf OUT.rtf`: one file through the loader and
    // saver, for looking at what a save makes of a real document.
    if (argc == 3) {
        std::FILE *in = std::fopen(argv[1], "rb");
        if (!in) return 1;
        std::string bytes;
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, in)) > 0;) bytes.append(buf, n);
        std::fclose(in);
        OfficeDoc doc;
        std::string err;
        if (!LoadRtfFromMemory(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), doc, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        for (const DocParagraph &p : doc.paragraphs) std::printf("%d%s| %s\n", p.heading_level, p.table_ref >= 0 ? " [table]" : p.image_ref >= 0 ? " [image]" : "", p.text.c_str());
        std::vector<unsigned char> out;
        if (!SaveRtfToMemory(doc, out, err)) return 1;
        std::FILE *o = std::fopen(argv[2], "wb");
        if (!o) return 1;
        std::fwrite(out.data(), 1, out.size(), o);
        std::fclose(o);
        return 0;
    }
    TestLoad();
    TestRoundTrip();
    TestEmptyAndEdges();
    std::printf("office rtf tests passed\n");
    return 0;
}
