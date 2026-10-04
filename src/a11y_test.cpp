// Accessibility: the model (a11y_doc.h) and its three questions -- how a
// document reads, its structure, what a reader who cannot see it is
// missing -- then every way into it: mepml (mepml_a11y.h), HTML
// (a11y_html.h), a tagged PDF (pdf_struct.h + a11y_pdf.h, over PDFs built
// here by hand), the office formats by way of their import, and what the
// exports write for such a reader: the tagging macros in the LaTeX, the
// alt / description / language attributes everywhere else. With tectonic on
// PATH, a document also goes through its PDF and back, and must read the
// same on the other side.

#include "a11y_doc.h"
#include "a11y_file.h"
#include "a11y_html.h"
#include "a11y_pdf.h"
#include "doc_export.h"
#include "math_speech.h"
#include "mepml_a11y.h"
#include "mepml_convert.h"
#include "pdf_document.h"
#include "pdf_object.h"
#include "pdf_struct.h"
#include "png_codec.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Lines = std::vector<std::string>;

std::string ReadText(const a11y::Document &doc) { return a11y::Report(doc, "read"); }
bool Has(const std::string &text, const std::string &what) { return text.find(what) != std::string::npos; }
bool HasIssue(const a11y::Document &doc, const char *code) {
    for (const a11y::Issue &i : a11y::Check(doc))
        if (i.code == code) return true;
    return false;
}
int IssueLine(const a11y::Document &doc, const char *code) {
    for (const a11y::Issue &i : a11y::Check(doc))
        if (i.code == code) return i.line;
    return -2;
}

a11y::Node Text(const std::string &t) {
    a11y::Node n;
    n.role = a11y::Role::Text;
    n.text = t;
    return n;
}
a11y::Node Make(a11y::Role role, std::vector<a11y::Node> children = {}) {
    a11y::Node n;
    n.role = role;
    n.children = std::move(children);
    return n;
}

// ---------------------------------------------------------------------------

void TestModel() {
    using a11y::Role;
    a11y::Document doc;
    doc.title = "Report";
    doc.lang = "en";
    a11y::Node h = Make(Role::Heading, {Text("Results")});
    h.level = 1;
    a11y::Node fig = Make(Role::Figure);
    fig.has_alt = true;
    fig.alt = "A rising line";
    a11y::Node deco = Make(Role::Figure);
    deco.has_alt = deco.decorative = true;
    a11y::Node bare = Make(Role::Figure);
    a11y::Node formula = Make(Role::Formula, {Text("x^2")});
    formula.has_alt = true;
    formula.alt = "x squared";
    a11y::Node para = Make(Role::Paragraph, {Text("Take "), formula, Text(" as given.")});
    a11y::Node head_a = Make(Role::HeaderCell, {Text("Name")}), head_b = Make(Role::HeaderCell, {Text("Value")});
    a11y::Node table = Make(Role::Table, {Make(Role::Caption, {Text("Table 1: Values")}), Make(Role::Row, {head_a, head_b}),
                                          Make(Role::Row, {Make(Role::Cell, {Text("a")}), Make(Role::Cell, {Text("1")})})});
    table.summary = "One value";
    a11y::Node item = Make(Role::ListItem, {Make(Role::Label, {Text("1.")}), Text("first"),
                                            Make(Role::List, {Make(Role::ListItem, {Make(Role::Label, {Text("-")}), Text("inner")})})});
    doc.root.children = {h, para, fig, deco, bare, table, Make(Role::List, {item})};

    const std::string read = ReadText(doc);
    CHECK(Has(read, "Document: Report\nLanguage: en\n"));
    CHECK(Has(read, "Heading level 1: Results\n"));
    CHECK(Has(read, "Take x squared as given.\n"));  // a formula reads as its alt text
    CHECK(Has(read, "Figure: A rising line\n"));
    CHECK(Has(read, "Figure with no description.\n"));
    // (The decorative figure is not read at all: two figure lines, not three.)
    CHECK(std::count(read.begin(), read.end(), '\n') == 14);
    CHECK(Has(read, "Table: 2 rows, 2 columns. One value\n  Caption: Table 1: Values\n  Columns: Name, Value\n  Row 1: Name: a; Value: 1\n"));
    CHECK(Has(read, "List of 1 item\n  1. first\n    List of 1 item\n      - inner\n"));

    const std::vector<a11y::Issue> issues = a11y::Check(doc);
    CHECK(issues.size() == 1 && issues[0].code == "figure-alt" && issues[0].severity == a11y::Severity::Error);

    // What is missing at the level of the document, and in its tables and headings.
    a11y::Document poor;
    a11y::Node h1 = Make(Role::Heading, {Text("A")}), h3 = Make(Role::Heading, {Text("B")});
    h1.level = 1;
    h3.level = 3;
    poor.root.children = {h1, h3, Make(Role::Table, {Make(Role::Row, {Make(Role::Cell, {Text("x")})})}),
                          Make(Role::Formula, {Text("a")}), Make(Role::Formula, {Text("b")}), Make(Role::Link)};
    for (const char *code : {"doc-title", "doc-lang", "heading-skip", "table-header", "table-summary", "formula-alt", "link-text"})
        CHECK(HasIssue(poor, code));
    // (Formulas without alt text are one note for the document, not one each.)
    int formula_notes = 0;
    for (const a11y::Issue &i : a11y::Check(poor)) formula_notes += i.code == "formula-alt";
    CHECK(formula_notes == 1);
    poor.tagged = false;
    CHECK(HasIssue(poor, "doc-untagged"));
    CHECK(Has(a11y::Report(poor, "check"), "error: The document has no structure"));
    CHECK(Has(a11y::Report(doc, "tree"), "Figure alt=\"A rising line\"") && Has(a11y::Report(doc, "tree"), "Figure (decorative)"));

    // Glyph runs (a PDF's) are joined with spaces, markup's text is not.
    a11y::Document glyphs;
    glyphs.spaced = false;
    glyphs.root.children = {Make(Role::Paragraph, {Text("see"), Make(Role::Link, {Text("here")}), Text(".")})};
    CHECK(Has(ReadText(glyphs), "see here.\n"));
}

// A formula with no alt text of its own is read as its TeX said aloud.
void TestMathSpeech() {
    using mathspeech::Speak;
    CHECK(Speak("\\hat{\\boldsymbol{\\theta}}_1 = \\frac{a}{b}") == "theta hat 1 equals a over b");
    CHECK(Speak("(\\mathbf{X}^\\top \\mathbf{X})^{-1} \\mathbf{X}^\\top \\mathbf{y}") == "the quantity X transpose X, inverse X transpose y");
    CHECK(Speak("\\sum_{i=1}^{n} \\mathbf{x}_i^2") == "the sum from i equals 1 to n of x i squared");
    CHECK(Speak("\\| \\mathbf{y} - \\mathbf{X}\\boldsymbol{\\theta} \\|^2") == "the norm of y minus X theta, squared");
    CHECK(Speak("E[\\mathbf{Y} \\mid \\mathbf{x}] = \\boldsymbol{\\theta}_0 + \\boldsymbol{\\theta}_1 \\mathbf{x}") ==
          "E of Y given x, equals theta 0 plus theta 1 x");
    CHECK(Speak("\\boldsymbol{\\varepsilon}_i \\overset{\\text{iid}}{\\sim} N(0, \\sigma^2)") ==
          "epsilon i is distributed as, iid, N of 0, sigma squared");
    CHECK(Speak("\\frac{\\partial \\mathrm{SSE}}{\\partial \\boldsymbol{\\theta}_0}") == "the fraction partial SSE, over partial theta 0");
    CHECK(Speak("\\begin{pmatrix} 2 & 1 \\\\ 1 & 2 \\end{pmatrix}") == "the matrix with rows 2, 1; 1, 2");
    CHECK(Speak("\\begin{pmatrix} 16 \\\\ 47 \\end{pmatrix}") == "the vector 16, 47");
    CHECK(Speak("\\operatorname{tr}(\\mathbf{H}) = p + 1") == "the trace of H equals p plus 1");
    CHECK(Speak("8(0.5) + 20(1.4) = 32") == "8 times 0.5 plus 20 times 1.4 equals 32");
    CHECK(Speak("\\underbrace{0.2}_{\\mathrm{SSE}} / \\underbrace{4}_{n} = 0.05") == "0.2, which is SSE, over 4, which is n, equals 0.05");
    CHECK(Speak("\\sqrt{\\mathbf{a}^\\top \\mathbf{a}}") == "the square root of a transpose a");
    CHECK(Speak("\\boldsymbol{\\theta}^{(t+1)}") == "theta superscript t plus 1");
    CHECK(Speak("31^\\circ") == "31 degrees" && Speak("x^2 + y^2") == "x squared plus y squared");
    CHECK(Speak("\\begin{aligned} a &= b \\\\ c &= d \\end{aligned}") == "a equals b; c equals d");
    // Never fails, whatever it is given.
    CHECK(Speak("a } b \\unknowncmd{x} ^") == "a b unknowncmd x");
    CHECK(Speak("").empty() && Speak("{{{{").empty() && !Speak("\\frac{").empty());
    std::string deep(4000, '{');
    (void)Speak(deep + "x");
    (void)Speak(std::string(4000, '(') + "x");
    std::string envs;
    for (int i = 0; i < 2000; ++i) envs += "\\begin{pmatrix}";
    (void)Speak(envs + "x");
    (void)Speak(std::string(3000, '^') + std::string(3000, '_'));
    // A formula with no \alttext reads that way in the source's reading and in the LaTeX's /Alt.
    const mepml::Document doc = mepml::Parse({"The slope $\\hat{\\boldsymbol{\\theta}}_1$ is positive."});
    CHECK(Has(ReadText(mepml::Accessibility(doc)), "The slope theta hat 1 is positive.\n"));
    CHECK(HasIssue(mepml::Accessibility(doc), "formula-alt"));
    // ("theta hat 1" in UTF-16.)
    CHECK(Has(mepml::ToLatex(doc, "."), "{Formula}{/Alt <FEFF007400680065007400610020006800610074002000310>") ||
          Has(mepml::ToLatex(doc, "."), "{Formula}{/Alt <FEFF00740068006500740061002000680061007400200031>"));
}

const Lines kSample = {
    "//? Title: Accessible sample",
    "//? Lang: en-GB",
    "",
    "> Introduction",
    "",
    "Text with \\(x^2\\)\\alttext(x squared) and a [link|https://example.org].",
    "",
    "$$",
    "\\frac{a}{b}",
    "$$",
    "\\alttext(a over b)",
    "",
    "\\image(plot.png)",
    "\\caption(A plot)",
    "\\alttext(A blue rectangle)",
    "",
    "\\image(rule.png)",
    "\\alttext()",
    "",
    "\\image(plot.png)",
    "",
    "| Name | Value |",
    "|------|-------|",
    "| a    | 1     |",
    "\\caption(Values)",
    "\\alttext(One name and its value)",
    "",
    "- first",
    "  1. nested",
};

void TestMepml() {
    const mepml::Document parsed = mepml::Parse(kSample);
    CHECK(mepml::DocumentLanguage(parsed) == "en-GB");
    CHECK(mepml::DocumentLanguage(mepml::Parse({"//? Language: pt_BR (Brazil)"})) == "pt-BR");
    const a11y::Document doc = mepml::Accessibility(parsed);
    CHECK(doc.format == "mepml" && doc.title == "Accessible sample" && doc.lang == "en-GB" && doc.tagged);
    const std::string read = ReadText(doc);
    CHECK(Has(read, "Heading level 1: Introduction\n"));
    CHECK(Has(read, "Text with x squared and a link.\n"));
    CHECK(Has(read, "Formula: a over b\n"));
    CHECK(Has(read, "Figure: A blue rectangle\n  Caption: Figure 1: A plot\n"));
    CHECK(Has(read, "Figure with no description.\n"));
    CHECK(!Has(read, "rule.png"));
    CHECK(Has(read, "Table: 2 rows, 2 columns. One name and its value\n  Caption: Table 1: Values\n  Columns: Name, Value\n  Row 1: Name: a; Value: 1\n"));
    CHECK(Has(read, "List of 1 item\n"));
    CHECK(Has(read, "1. nested\n"));
    // The one thing missing is the third picture's alt text, and the check says where.
    const std::vector<a11y::Issue> issues = a11y::Check(doc);
    CHECK(issues.size() == 1 && issues[0].code == "figure-alt" && issues[0].line == 19);
    // An empty \alttext() is decoration; none at all is a problem.
    CHECK(!HasIssue(mepml::Accessibility(mepml::Parse({"//? Title: T", "//? Lang: en", "", "\\image(a.png)", "\\alttext()"})), "figure-alt"));
    CHECK(IssueLine(mepml::Accessibility(mepml::Parse({"\\image(a.png)"})), "figure-alt") == 0);
    // The editor's presentation view shows a slide to an audience: alt
    // text is not on the page (captions are).
    const std::vector<mepml::PresentationPage> pages = mepml::PresentationPages(
        "deck.mepml",
        {"\\slide(", "> One", "$$", "x^2", "$$", "\\alttext(x squared)", "", "\\image(a.png)", "\\caption(A plot)", "\\alttext(A curve)", "",
         "| a | b |", "|---|---|", "| 1 | 2 |", "\\alttext(One row)", ")"},
        [](const std::string &, std::vector<std::string> *) { return false; });
    CHECK(pages.size() == 1);
    bool caption_shown = false;
    for (const std::string &l : pages[0].lines) {
        CHECK(l.find("\\alttext") == std::string::npos);
        caption_shown = caption_shown || l.find("\\caption(A plot)") != std::string::npos;
    }
    CHECK(caption_shown);
    // A table with no header row.
    CHECK(HasIssue(mepml::Accessibility(mepml::Parse({"| a | b |", "| c | d |"})), "table-header"));
    // A code block's figure reads as the block's \alttext.
    const a11y::Document plot = mepml::Accessibility(mepml::Parse(
        {"```{r, file=\"p.png\"}", "plot(1)", "```", "// result_begin:", "// \\image(p.png)", "// result_end", "\\alttext(A single point)"}));
    CHECK(Has(ReadText(plot), "Code:\n  plot(1)\nFigure: A single point\n"));
}

void TestHtml() {
    const mepml::Document parsed = mepml::Parse(kSample);
    const std::string html = mepml::ToHtml(parsed);
    // What the page says for a screen reader.
    CHECK(Has(html, "<html lang=\"en-GB\">"));
    CHECK(Has(html, "<img src=\"plot.png\" alt=\"A blue rectangle\">"));
    CHECK(Has(html, "<img src=\"rule.png\" alt=\"\" role=\"presentation\">"));
    CHECK(Has(html, "<img src=\"plot.png\"></figure>"));  // not described: no alt at all, rather than an empty one
    CHECK(Has(html, "<table aria-description=\"One name and its value\">"));
    CHECK(Has(html, "<th scope=\"col\">Name</th>"));
    CHECK(Has(html, "role=\"math\" aria-label=\"a over b\""));

    const a11y::Document doc = a11y::FromHtml(html);
    CHECK(doc.format == "html" && doc.title == "Accessible sample" && doc.lang == "en-GB");
    const std::string read = ReadText(doc);
    CHECK(Has(read, "Text with x squared and a link.\n"));
    CHECK(Has(read, "Formula: a over b\n"));
    CHECK(Has(read, "Figure: A blue rectangle\n"));
    CHECK(Has(read, "Table: 2 rows, 2 columns. One name and its value\n"));
    CHECK(Has(read, "  Columns: Name, Value\n  Row 1: Name: a; Value: 1\n"));
    const std::vector<a11y::Issue> issues = a11y::Check(doc);
    CHECK(issues.size() == 1 && issues[0].code == "figure-alt");
    // Any page: aria-hidden content is left out, an aria-label names a picture.
    const a11y::Document page = a11y::FromHtml(
        "<html><head><title>T</title></head><body><h2>Head</h2><p>One <span aria-hidden=\"true\">hidden</span>two</p>"
        "<div role=\"img\" aria-label=\"A chart\"></div><ol><li>a</li><li>b</li></ol></body></html>");
    const std::string page_read = ReadText(page);
    CHECK(Has(page_read, "Heading level 2: Head\n") && Has(page_read, "One two\n") && !Has(page_read, "hidden"));
    CHECK(Has(page_read, "Figure: A chart\n") && Has(page_read, "List of 2 items\n  1. a\n  2. b\n"));
    CHECK(HasIssue(page, "doc-lang") && !HasIssue(page, "doc-title"));
    // And back into mepml: the description and the decoration survive an import.
    const std::string back = mepml::FromHtml(html);
    CHECK(Has(back, "//? Lang: en-GB"));
    CHECK(Has(back, "\\alttext(One name and its value)"));
    CHECK(Has(back, "\\image(rule.png)\n\\alttext()"));
}

// --- a tagged PDF, by hand -------------------------------------------------

const unsigned char *B(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }

std::string XrefLine(long long offset, int gen, char kind) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010lld %05d %c \n", offset, gen, kind);
    return buf;
}

std::string BuildDoc(const std::vector<std::pair<int, std::string>> &objects, const std::string &trailer_extra) {
    std::string doc = "%PDF-1.7\n";
    std::vector<std::pair<int, size_t>> offsets;
    int max_num = 0;
    for (const auto &obj : objects) {
        offsets.emplace_back(obj.first, doc.size());
        doc += std::to_string(obj.first) + " 0 obj\n" + obj.second + "\nendobj\n";
        max_num = std::max(max_num, obj.first);
    }
    const size_t xref_off = doc.size();
    doc += "xref\n0 " + std::to_string(max_num + 1) + "\n";
    doc += XrefLine(0, 65535, 'f');
    for (int n = 1; n <= max_num; ++n) {
        auto it = std::find_if(offsets.begin(), offsets.end(), [&](const auto &p) { return p.first == n; });
        doc += it == offsets.end() ? XrefLine(0, 0, 'f') : XrefLine(static_cast<long long>(it->second), 0, 'n');
    }
    doc += "trailer\n<< /Size " + std::to_string(max_num + 1) + " /Root 1 0 R " + trailer_extra + " >>\n";
    doc += "startxref\n" + std::to_string(xref_off) + "\n%%EOF\n";
    return doc;
}

std::string Stream(const std::string &content) {
    return "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream";
}

void TestPdf() {
    // Two pages. Page 1: a heading, a paragraph that holds a formula, a
    // figure (a filled rectangle), a page number marked as an artifact.
    // Page 2: the paragraph's last words (it runs over the page), and a
    // table whose header cell names its properties through /Properties.
    const std::string page1 =
        "/H1 <</MCID 0>> BDC BT /F1 14 Tf 20 170 Td (Results) Tj ET EMC\n"
        "/P <</MCID 1>> BDC BT /F1 10 Tf 20 150 Td (The value) Tj ET EMC\n"
        "/Formula <</MCID 2>> BDC BT /F1 10 Tf 70 150 Td (x2) Tj ET EMC\n"
        "/P <</MCID 3>> BDC BT /F1 10 Tf 85 150 Td (grows over) Tj ET EMC\n"
        "/Figure <</MCID 4>> BDC 20 60 100 50 re f EMC\n"
        "/Artifact <</Type /Pagination>> BDC BT /F1 8 Tf 95 10 Td (1) Tj ET EMC\n"
        "BT /F1 8 Tf 20 30 Td (untagged) Tj ET\n";
    const std::string page2 =
        "/P <</MCID 0>> BDC BT /F1 10 Tf 20 170 Td (time.) Tj ET EMC\n"
        "/TH /Cell BDC BT /F1 10 Tf 20 140 Td (Name) Tj ET EMC\n"
        "/TD <</MCID 2>> BDC BT /F1 10 Tf 20 125 Td (alpha) Tj ET EMC\n"
        "/Span <</MCID 3 /ActualText (fi)>> BDC BT /F1 10 Tf 20 100 Td (\\256) Tj ET EMC\n";
    auto build = [&](const std::string &parent_tree) {
      return BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 10 0 R /MarkInfo << /Marked true >> /Lang (en-GB) "
                "/ViewerPreferences << /DisplayDocTitle true >> >>"},
            {2, "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 /MediaBox [0 0 200 200] /Resources << /Font << /F1 4 0 R >> "
                "/Properties << /Cell << /MCID 1 >> >> >> >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R /StructParents 0 >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
            {5, Stream(page1)},
            {6, "<< /Type /Page /Parent 2 0 R /Contents 7 0 R /StructParents 1 >>"},
            {7, Stream(page2)},
            {8, "<< /Title <FEFF00540068006500200072006500730075006C0074> >>"},
            {10, "<< /Type /StructTreeRoot /K 11 0 R /RoleMap << /Heading1 /H1 /Chart /Figure >> " + parent_tree + " >>"},
            {11, "<< /Type /StructElem /S /Document /P 10 0 R /K [12 0 R 13 0 R 15 0 R 16 0 R 20 0 R] >>"},
            // (An integer kid is an MCID on the element's own page.)
            {12, "<< /Type /StructElem /S /Heading1 /P 11 0 R /Pg 3 0 R /K 0 >>"},
            {13, "<< /Type /StructElem /S /P /P 11 0 R /Pg 3 0 R /K [1 14 0 R 3 << /Type /MCR /Pg 6 0 R /MCID 0 >>] >>"},
            {14, "<< /Type /StructElem /S /Formula /P 13 0 R /Pg 3 0 R /K 2 /Alt (x squared) >>"},
            {15, "<< /Type /StructElem /S /Chart /P 11 0 R /Pg 3 0 R /K 4 /Alt <FEFF0041002000620061007200200063006800610072007400200028201400290020> >>"},
            {16, "<< /Type /StructElem /S /Table /P 11 0 R /K [17 0 R] /A << /O /Table /Summary (Names) >> >>"},
            {17, "<< /Type /StructElem /S /TR /P 16 0 R /K [18 0 R 19 0 R] >>"},
            {18, "<< /Type /StructElem /S /TH /P 17 0 R /Pg 6 0 R /K 1 /A [<< /O /Table /Scope /Column >>] >>"},
            {19, "<< /Type /StructElem /S /TD /P 17 0 R /Pg 6 0 R /K 2 >>"},
            {20, "<< /Type /StructElem /S /P /P 11 0 R /Pg 6 0 R /K 3 >>"},
        },
        "/Info 8 0 R");
    };
    const std::string doc = build("/ParentTree << /Nums [] >>");
    pdfdoc::PdfDocument document;
    document.Load(B(doc), doc.size());
    CHECK(document.PageCount() == 2);

    const pdfstruct::Tree tree = pdfstruct::Read(B(doc), doc.size(), document.Xref(), document);
    CHECK(tree.present && tree.marked && tree.parent_tree && tree.display_title);
    CHECK(tree.lang == "en-GB" && tree.title == "The result");
    CHECK(!tree.ParentsConsistent());  // (this file's parent tree is empty)
    CHECK(tree.roots.size() == 1 && tree.elements.size() == 10);
    const pdfstruct::Element &heading = tree.elements[1];
    CHECK(heading.type == "Heading1" && heading.role == "H1" && heading.page == 0);
    CHECK(heading.kids.size() == 1 && heading.kids[0].kind == pdfstruct::Kid::Kind::Content && heading.kids[0].mcid == 0);
    const pdfstruct::Element &para = tree.elements[2];
    CHECK(para.kids.size() == 4 && para.kids[1].kind == pdfstruct::Kid::Kind::Element);
    CHECK(para.kids[3].kind == pdfstruct::Kid::Kind::Content && para.kids[3].page == 1 && para.kids[3].mcid == 0);
    const pdfstruct::Element &chart = tree.elements[4];
    CHECK(chart.role == "Figure" && chart.has_alt && chart.alt == "A bar chart (\xE2\x80\x94) ");
    CHECK(tree.elements[5].summary == "Names" && tree.elements[7].scope == "Column");

    // The same file with its parent tree filled in agrees with itself; one
    // entry pointing at the wrong element does not.
    for (const bool wrong : {false, true}) {
        const std::string filled = build(std::string("/ParentTree << /Nums [0 [12 0 R 13 0 R 14 0 R 13 0 R 15 0 R] 1 [13 0 R 18 0 R 19 0 R ") +
                                         (wrong ? "19 0 R" : "20 0 R") + "]] >>");
        pdfdoc::PdfDocument d2;
        d2.Load(B(filled), filled.size());
        const pdfstruct::Tree t2 = pdfstruct::Read(B(filled), filled.size(), d2.Xref(), d2);
        CHECK(t2.page_parents.size() == 2 && t2.page_parents[0].size() == 5 && t2.ParentsConsistent() == !wrong);
    }

    const a11y::Document read_doc = a11y::FromPdf(B(doc), doc.size(), document);
    CHECK(read_doc.format == "pdf" && read_doc.tagged && read_doc.title == "The result" && read_doc.lang == "en-GB");
    const std::string read = ReadText(read_doc);
    CHECK(Has(read, "Heading level 1: Results\n"));
    // The paragraph is one line though it runs over two pages, and its formula reads as its /Alt.
    CHECK(Has(read, "The value x squared grows over time.\n"));
    CHECK(Has(read, "Figure: A bar chart"));
    CHECK(Has(read, "Table: 1 row, 2 columns. Names\n"));
    CHECK(Has(read, "Name") && Has(read, "alpha"));
    // An artifact and content outside the structure are not read; /ActualText replaces its glyphs.
    CHECK(!Has(read, "untagged") && !Has(read, "\n1\n"));
    CHECK(Has(read, "\nfi\n"));
    // Where the figure is: its painted rectangle, on the first page.
    const a11y::Node &figure = read_doc.root.children[2];
    CHECK(figure.role == a11y::Role::Figure && figure.page == 0 && figure.has_box);
    CHECK(figure.box[0] == 20 && figure.box[1] == 60 && figure.box[2] == 120 && figure.box[3] == 110);
    CHECK(a11y::Check(read_doc).empty());

    // The same pages with no structure tree: text in page order, and a check that says so.
    const std::string plain = BuildDoc(
        {
            {1, "<< /Type /Catalog /Pages 2 0 R >>"},
            {2, "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 200 200] /Resources << /Font << /F1 4 0 R >> >> >>"},
            {3, "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>"},
            {4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"},
            {5, Stream("BT /F1 10 Tf 20 170 Td (First line) Tj 0 -12 Td (second line.) Tj 0 -40 Td (Another block) Tj ET")},
        },
        "");
    pdfdoc::PdfDocument plain_document;
    plain_document.Load(B(plain), plain.size());
    const a11y::Document untagged = a11y::FromPdf(B(plain), plain.size(), plain_document);
    CHECK(!untagged.tagged && HasIssue(untagged, "doc-untagged"));
    const std::string untagged_read = ReadText(untagged);
    CHECK(Has(untagged_read, "First line second line.\n") && Has(untagged_read, "Another block\n"));
    // A file of nothing readable is refused rather than read as empty.
    a11y::Document none;
    std::string err;
    CHECK(!a11y::FromPdfBytes(B(std::string("not a pdf")), 9, &none, &err) && !err.empty());

    CHECK(pdfobj::TextStringToUtf8("plain") == "plain");
    CHECK(pdfobj::TextStringToUtf8(std::string("\xFE\xFF\x00\x41\xD8\x3D\xDE\x00", 8)) == "A\xF0\x9F\x98\x80");
    CHECK(pdfobj::TextStringToUtf8("caf\xE9") == "caf\xC3\xA9");
}

// --- the exports -----------------------------------------------------------

std::filesystem::path g_tmp;

void WritePng(const std::string &name, int w, int h) {
    std::vector<unsigned char> rgba(static_cast<size_t>(w * h * 4), 200);
    const std::string png = png::Encode(w, h, 4, rgba.data(), w * 4);
    std::ofstream f(g_tmp / name, std::ios::binary);
    f.write(png.data(), static_cast<std::streamsize>(png.size()));
}

void TestLatex() {
    const mepml::Document parsed = mepml::Parse(kSample);
    const std::string tex = mepml::ToLatex(parsed, g_tmp.string());
    // The document: its language and title where a reader looks for them.
    CHECK(Has(tex, "\\mepDoc{/Lang (en-GB) /ViewerPreferences << /DisplayDocTitle true >>}{\\special{pdf:docinfo << /Title <FEFF0041"));
    CHECK(Has(tex, "\\special{pdf:stream @mepxmp <") && Has(tex, "/Type /Metadata /Subtype /XML"));
    // A heading's content starts at its number; the contents line takes plain text.
    CHECK(Has(tex, "{H1}{}\\mepH{") && Has(tex, "\\section[{Introduction}]{\\mepHs{}Introduction\\mepE{}}"));
    // Inline maths: a Formula inside the paragraph, read as its \alttext ("x squared" in UTF-16).
    CHECK(Has(tex, "{Formula}{/Alt <FEFF007800200073007100750061007200650064>}"));
    CHECK(Has(tex, "{Formula}$x^2$\\mepE{}"));
    // Display maths is marked inside its display.
    CHECK(Has(tex, "\\[\\mepMb{") && Has(tex, "\\mepEb{}\\]"));
    // The described figure, the decoration (an artifact), and the one with no alt text.
    CHECK(Has(tex, "{Figure}{/Alt <FEFF004100200062006C00750065002000720065006300740061006E0067006C0065>}"));
    CHECK(Has(tex, "\\mepA{\\includegraphics"));
    CHECK(Has(tex, "{Figure}{}\\mepMb{"));
    CHECK(Has(tex, "\\begin{figure}[H]"));
    // The table: its summary, a caption, header cells with their scope.
    CHECK(Has(tex, "{Table}{/A << /O /Table /Summary <FEFF004F006E0065"));
    CHECK(Has(tex, "{TH}{/A << /O /Table /Scope /Column >>}") && Has(tex, "{TD}{}\\mepMb{"));
    CHECK(Has(tex, "{Caption}{}"));
    // A list: each item its label and its body.
    CHECK(Has(tex, "{L}{/A << /O /List /ListNumbering /Disc >>}\\begin{itemize}\\mepLfix{}"));
    CHECK(Has(tex, "{Lbl}{}\\mepLbl{") && Has(tex, "{LBody}{}"));
    // Every element opened is a child of one opened before it, and every
    // piece of content opened is closed.
    size_t begins = 0, ends = 0;
    for (size_t at = 0; (at = tex.find("\\mepM{", at)) != std::string::npos; ++at) ++begins;
    for (size_t at = 0; (at = tex.find("\\mepE{}", at)) != std::string::npos; ++at) ++ends;
    const size_t body = tex.find("\\begin{document}");
    size_t body_begins = 0;
    for (size_t at = body; (at = tex.find("\\mepM{", at)) != std::string::npos; ++at) ++body_begins;
    CHECK(body != std::string::npos && body_begins > 4 && begins >= body_begins && ends >= body_begins);
    size_t boxed_begins = 0, boxed_ends = 0;
    for (size_t at = body; (at = tex.find("\\mepMb{", at)) != std::string::npos; ++at) ++boxed_begins;
    for (size_t at = body; (at = tex.find("\\mepEb{}", at)) != std::string::npos; ++at) ++boxed_ends;
    CHECK(boxed_begins == boxed_ends && boxed_begins >= 5);
    // A deck: each frame a section, its title the heading.
    const mepml::Document deck = mepml::Parse({"//? Title: Deck", "//? Type: presentation", "//? Lang: fr", "", "\\slide(", "> One", "", "Text.", ")"});
    const std::string beamer = mepml::ToLatex(deck, g_tmp.string());
    CHECK(Has(beamer, "\\mepDoc{/Lang (fr)") && Has(beamer, "{Sect}{}\\begin{frame}[fragile]{") && Has(beamer, "{H1}One\\mepEb{}}"));
}

void TestOffice() {
    Lines lines = kSample;
    const mepml::Document parsed = mepml::Parse(lines);
    const std::string reference = ReadText(mepml::Accessibility(parsed));
    for (const char *ext : {"docx", "odt"}) {
        const std::string path = (g_tmp / (std::string("sample.") + ext)).string();
        std::string err;
        CHECK(mepml::ExportFile(parsed, path, g_tmp.string(), &err));
        a11y::Document doc;
        CHECK(a11y::FromFile(path, &doc, &err));
        const std::string read = ReadText(doc);
        // The pictures' descriptions, the decoration, the table's description and header.
        CHECK(doc.lang == "en-GB" && doc.title == "Accessible sample");
        CHECK(Has(read, "Figure: A blue rectangle\n"));
        CHECK(Has(read, "Table: 2 rows, 2 columns. One name and its value\n"));
        CHECK(Has(read, "  Columns: Name, Value\n"));
        const std::vector<a11y::Issue> issues = a11y::Check(doc);
        int figure_problems = 0;
        for (const a11y::Issue &i : issues) figure_problems += i.code == "figure-alt";
        CHECK(figure_problems == 1);  // the one picture that has no alt text, not the decorative one
    }
    CHECK(Has(reference, "Figure: A blue rectangle\n"));
    // Markdown and Org carry the language and a picture's alt text.
    const std::string md = mepml::ToMarkdown(parsed), org = mepml::ToOrg(parsed);
    CHECK(Has(mepml::FromMarkdown(md), "//? Lang: en-GB") && Has(md, "![A blue rectangle](plot.png)"));
    CHECK(Has(org, "#+ATTR_HTML: :alt A blue rectangle\n[[file:plot.png]]"));
    const std::string org_back = mepml::FromOrg(org);
    CHECK(Has(org_back, "//? Lang: en-GB") && Has(org_back, "\\alttext(A blue rectangle)") && Has(org_back, "\\image(rule.png)\n\\alttext()"));
}

// The whole way: mepml -> tagged PDF (tectonic) -> the PDF's own reading.
void TestPdfRoundTrip() {
    if (std::system("command -v tectonic >/dev/null 2>&1") != 0) {
        std::printf("a11y_test: tectonic not on PATH, PDF round trip skipped\n");
        return;
    }
    Lines lines = kSample;
    // (Enough text to run a paragraph over a page, and a box that breaks.)
    std::string filler;
    for (int i = 0; i < 260; ++i) filler += "Lorem ipsum dolor sit amet consectetur. ";
    lines.push_back("");
    lines.push_back("Start. " + filler + "End of the long paragraph.");
    lines.push_back("");
    lines.push_back("\\definition(Long,");
    lines.push_back("Boxed start. " + filler + "End of the box.");
    lines.push_back(")");
    const mepml::Document parsed = mepml::Parse(lines);
    const std::filesystem::path tex = g_tmp / "round.tex";
    {
        std::ofstream f(tex);
        f << mepml::ToLatex(parsed, g_tmp.string());
    }
    const std::string cmd = "tectonic -X compile '" + tex.string() + "' --outdir '" + g_tmp.string() + "' >'" + (g_tmp / "tectonic.log").string() + "' 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        // (No network for its bundle, say: nothing this test can judge.)
        std::printf("a11y_test: tectonic could not compile, PDF round trip skipped (see %s)\n", (g_tmp / "tectonic.log").string().c_str());
        return;
    }
    a11y::Document doc;
    std::string err;
    CHECK(a11y::FromFile((g_tmp / "round.pdf").string(), &doc, &err));
    CHECK(doc.tagged && doc.lang == "en-GB" && doc.title == "Accessible sample");
    const std::string read = ReadText(doc);
    CHECK(Has(read, "Title: Accessible sample\n"));
    CHECK(Has(read, "Heading level 1: 1 Introduction\n"));
    CHECK(Has(read, "Text with x squared and a link.\n"));
    CHECK(Has(read, "Formula: a over b\n"));
    CHECK(Has(read, "Figure: A blue rectangle\n"));
    CHECK(Has(read, "Caption: Figure 1: A plot\n"));
    CHECK(Has(read, "Table: 2 rows, 2 columns. One name and its value\n"));
    CHECK(Has(read, "  Columns: Name, Value\n  Row 1: Name: a; Value: 1\n"));
    CHECK(Has(read, "List of 1 item\n") && Has(read, "1. nested\n"));
    // The decorative picture is an artifact, the undescribed one a figure with no words.
    const std::vector<a11y::Issue> issues = a11y::Check(doc);
    CHECK(issues.size() == 1 && issues[0].code == "figure-alt");
    // A paragraph and a box that run over pages each read as one piece,
    // start to end, with no page number in between.
    pdfdoc::PdfDocument document;
    std::ifstream in(g_tmp / "round.pdf", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    document.Load(B(bytes), bytes.size());
    CHECK(document.PageCount() >= 4);
    bool long_para = false, long_box = false;
    size_t at = 0;
    while (at < read.size()) {
        size_t nl = read.find('\n', at);
        if (nl == std::string::npos) nl = read.size();
        const std::string line = read.substr(at, nl - at);
        if (Has(line, "Start. Lorem") && Has(line, "End of the long paragraph.")) long_para = true;
        if (Has(line, "Boxed start. Lorem") && Has(line, "End of the box.")) long_box = true;
        at = nl + 1;
    }
    CHECK(long_para && long_box);
    // Its parent tree is there, and every page's marked content counts from 0.
    const pdfstruct::Tree tree = pdfstruct::Read(B(bytes), bytes.size(), document.Xref(), document);
    CHECK(tree.present && tree.marked && tree.parent_tree && tree.display_title);
    // (From each page's content back to its element, and it is the same element.)
    CHECK(tree.ParentsConsistent());
    std::vector<std::vector<int>> mcids(static_cast<size_t>(document.PageCount()));
    for (const pdfstruct::Element &e : tree.elements)
        for (const pdfstruct::Kid &k : e.kids)
            if (k.kind == pdfstruct::Kid::Kind::Content && k.page >= 0) mcids[static_cast<size_t>(k.page)].push_back(k.mcid);
    for (std::vector<int> &page : mcids) {
        std::sort(page.begin(), page.end());
        for (size_t i = 0; i < page.size(); ++i) CHECK(page[i] == static_cast<int>(i));
    }
    // A deck: Beamer sets a frame's title after its body and puts it above;
    // it must still read first, as the frame's heading.
    const mepml::Document deck = mepml::Parse({"//? Title: A deck", "//? Type: presentation", "//? Lang: en", "", "\\slide(", "> First slide", "",
                                               "- a point", "", "\\image(plot.png)", "\\alttext(A blue rectangle)", ")"});
    {
        std::ofstream f(g_tmp / "deck.tex");
        f << mepml::ToLatex(deck, g_tmp.string());
    }
    const std::string deck_cmd = "tectonic -X compile '" + (g_tmp / "deck.tex").string() + "' --outdir '" + g_tmp.string() + "' >'" +
                                 (g_tmp / "tectonic.log").string() + "' 2>&1";
    CHECK(std::system(deck_cmd.c_str()) == 0);
    a11y::Document deck_doc;
    CHECK(a11y::FromFile((g_tmp / "deck.pdf").string(), &deck_doc, &err));
    CHECK(deck_doc.tagged && deck_doc.title == "A deck" && a11y::Check(deck_doc).empty());
    const std::string deck_read = ReadText(deck_doc);
    const size_t head_at = deck_read.find("Heading level 1: First slide\n"), list_at = deck_read.find("List of 1 item\n");
    CHECK(head_at != std::string::npos && list_at != std::string::npos && head_at < list_at);
    CHECK(Has(deck_read, "Title: A deck\n") && Has(deck_read, "Figure: A blue rectangle\n"));
    // Org's export goes through the same walker (its HTML, not mepml's):
    // highlighted code boxes, plain <pre>, quotes, rules.
    const std::string org_html =
        "<html lang=\"de\"><body><h1>Kapitel</h1><p>Text <b>fett</b>.</p>"
        "<div class=\"org-code-block\"><pre><code data-lang=\"python\">\n<span class=\"tok-purple\">def</span> f(): pass\nf()\n</code></pre></div>"
        "<pre>plain one\nplain two</pre><blockquote><p>Quoted.</p></blockquote><hr><p>After.</p></body></html>";
    {
        std::ofstream f(g_tmp / "org.tex");
        f << ExportHtmlToLatex(org_html, "Aus Org", "", g_tmp.string());
    }
    const std::string org_cmd = "tectonic -X compile '" + (g_tmp / "org.tex").string() + "' --outdir '" + g_tmp.string() + "' >'" +
                                (g_tmp / "tectonic.log").string() + "' 2>&1";
    CHECK(std::system(org_cmd.c_str()) == 0);
    a11y::Document org_doc;
    CHECK(a11y::FromFile((g_tmp / "org.pdf").string(), &org_doc, &err));
    CHECK(org_doc.tagged && org_doc.lang == "de" && org_doc.title == "Aus Org");
    const std::string org_read = ReadText(org_doc);
    CHECK(Has(org_read, "Heading level 1: 1 Kapitel\n") && Has(org_read, "Text fett.\n"));
    CHECK(Has(org_read, "Code:\n  def f(): pass\n  f()\n"));
    CHECK(Has(org_read, "Code:\n  plain one\n  plain two\n"));
    CHECK(Has(org_read, "Quote:\n  Quoted.\n") && Has(org_read, "After.\n"));
    CHECK(a11y::Check(org_doc).empty());
}

}  // namespace

int main() {
    std::error_code ec;
    g_tmp = std::filesystem::temp_directory_path(ec) / ("mep-a11y-test-" + std::to_string(std::rand()));
    std::filesystem::create_directories(g_tmp, ec);
    WritePng("plot.png", 120, 60);
    WritePng("rule.png", 200, 6);
    TestModel();
    TestMathSpeech();
    TestMepml();
    TestHtml();
    TestPdf();
    TestLatex();
    TestOffice();
    TestPdfRoundTrip();
    std::filesystem::remove_all(g_tmp, ec);
    std::printf("a11y_test: all checks passed\n");
    return 0;
}
