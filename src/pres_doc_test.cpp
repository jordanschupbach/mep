// Windowless test for pres_doc.h: a deck using every part of the model
// survives .pptx and .odp (and each through the other), and a PowerPoint
// file that leans on its master and layout -- placeholders with no
// position of their own, text sizes and bullets from the master's styles,
// theme colours -- reads as PowerPoint draws it.
// CHECK(), never assert(): the Release build strips assert() entirely.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "math_markup.h"
#include "pres_doc.h"
#include "zip_archive.h"

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using namespace pres;

// Everything the formats carry, as one string per shape (positions to the
// nearest 0.01 cm: .odp stores centimetres to three places).
std::string Signature(const Presentation &p) {
    std::string o = "size " + std::to_string(p.width / 3600) + "x" + std::to_string(p.height / 3600) + "\n";
    for (size_t i = 0; i < p.slides.size(); ++i) {
        const Slide &s = p.slides[i];
        o += "slide " + std::to_string(i) + " bg=" + s.background + "\n";
        for (const Shape &sh : s.shapes) {
            char geo[96];
            std::snprintf(geo, sizeof geo, "%ld,%ld %ldx%ld", sh.x / 3600, sh.y / 3600, sh.w / 3600, sh.h / 3600);
            o += "  shape kind=" + std::to_string(static_cast<int>(sh.kind)) + " " + geo + " fill=" + sh.fill + " line=" + sh.line +
                 " title=" + std::to_string(sh.is_title) + " valign=" + std::to_string(static_cast<int>(sh.valign));
            if (sh.kind == ShapeKind::Image) o += " image=" + std::to_string(sh.image ? sh.image->size() : 0) + "/" + sh.image_ext + " alt=" + sh.alt;
            o += "\n";
            if (!sh.HasText()) continue;
            for (const Paragraph &para : sh.paras) {
                o += "    p align=" + std::to_string(static_cast<int>(para.align)) + " lvl=" + std::to_string(para.level) +
                     " bullet=" + std::to_string(para.bullet) + " num=" + std::to_string(para.numbered) + ":";
                for (const TextRun &r : para.runs) {
                    if (r.IsMath()) {
                        // Its structure (the MathML it typesets as), not its spelling:
                        // Office equations keep maths, not the TeX it was typed as.
                        o += std::string(" [$") + TexToMathMlBody(r.tex, r.display) + (r.display ? "$ display]" : "$]");
                        continue;
                    }
                    char pt[16];
                    std::snprintf(pt, sizeof pt, "%.0f", r.size_pt > 0 ? r.size_pt : sh.font_pt);
                    o += " [" + r.text + "|" + pt + (r.bold ? "b" : "") + (r.italic ? "i" : "") + (r.underline ? "u" : "") + (r.strike ? "s" : "") + (r.baseline > 0 ? "^" : r.baseline < 0 ? "_" : "") +
                         "|" + (r.color.empty() ? sh.text_color : r.color) + "|" + r.link + "]";
                }
                o += "\n";
            }
        }
    }
    return o;
}

TextRun Run(const std::string &text) {
    TextRun r;
    r.text = text;
    return r;
}

Presentation Sample() {
    Presentation p = NewPresentation();
    p.title = "Sample deck";
    p.author = "A. Person";
    Slide s = NewSlide(p);
    // A body with every kind of paragraph and run.
    Shape &body = s.shapes[1];
    body.paras.clear();
    Paragraph a;
    a.bullet = true;
    a.runs = {Run("plain "), Run("bold"), Run(" and "), Run("big red link")};
    a.runs[1].bold = true;
    a.runs[3].size_pt = 32;
    a.runs[3].color = "C62828";
    a.runs[3].link = "https://example.com/";
    a.runs[3].underline = true;
    body.paras.push_back(a);
    Paragraph b;
    b.bullet = true;
    b.level = 1;
    b.runs = {Run("nested, "), Run("italic"), Run(" struck")};
    b.runs[1].italic = true;
    b.runs[2].strike = true;
    b.runs.push_back(Run("2"));
    b.runs.back().baseline = 1;
    body.paras.push_back(b);
    Paragraph c;
    c.numbered = true;
    c.runs = {Run("first")};
    body.paras.push_back(c);
    Paragraph d;
    d.align = Align::Right;
    d.runs = {Run("right  aligned\twith tab")};
    body.paras.push_back(d);
    // Shapes.
    Shape rect = NewShape(p, ShapeKind::Rect);
    rect.x = 1000000;
    rect.paras[0].runs = {Run("in a box")};
    s.shapes.push_back(rect);
    Shape ell = NewShape(p, ShapeKind::Ellipse);
    ell.x = 6000000;
    ell.fill = "2F7D32";
    s.shapes.push_back(ell);
    Shape rr = NewShape(p, ShapeKind::RoundRect);
    rr.y = 5000000;
    s.shapes.push_back(rr);
    Shape line = NewShape(p, ShapeKind::Line);
    line.y = 6000000;
    line.w = -2000000;  // drawn right to left
    line.h = 400000;
    s.shapes.push_back(line);
    Shape img;
    img.kind = ShapeKind::Image;
    img.x = 8000000;
    img.y = 3000000;
    img.w = 2000000;
    img.h = 1000000;
    img.image = std::make_shared<const std::string>(std::string("\x89PNG\r\n\x1a\n") + std::string(40, 'x'));
    img.image_ext = "png";
    img.alt = "a picture";
    s.shapes.push_back(img);
    s.background = "F6F8FA";
    p.slides.push_back(s);
    p.slides.push_back(NewSlide(p, false));
    return p;
}

bool Same(const std::string &want, const std::string &got, const char *what) {
    if (want == got) return true;
    std::fprintf(stderr, "%s differs\n---- want ----\n%s---- got ----\n%s", what, want.c_str(), got.c_str());
    return false;
}

void TestRoundTrips() {
    const Presentation p = Sample();
    const std::string want = Signature(p);
    std::string err;
    Presentation a, b, c, d;
    CHECK(LoadPptx(SavePptx(p), &a, &err));
    CHECK(Same(want, Signature(a), "pptx round trip"));
    CHECK(a.title == "Sample deck" && a.author == "A. Person");
    CHECK(LoadOdp(SaveOdp(p), &b, &err));
    CHECK(Same(want, Signature(b), "odp round trip"));
    CHECK(b.title == "Sample deck" && b.author == "A. Person");
    CHECK(LoadOdp(SaveOdp(a), &c, &err));
    CHECK(Same(want, Signature(c), "pptx -> odp"));
    CHECK(LoadPptx(SavePptx(b), &d, &err));
    CHECK(Same(want, Signature(d), "odp -> pptx"));
    // Not a deck.
    CHECK(!LoadPptx("not a zip", &a, &err) && !err.empty());
    CHECK(!LoadOdp(zip::BuildArchive({{"x.txt", "hi"}}), &a, &err));
}

// A PowerPoint package the way PowerPoint writes one: the slide's title and
// body are placeholders positioned only by the layout, sized by the
// master's text styles, and coloured from the theme.
std::string InheritingPptx() {
    const std::string head = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    const std::string ns = "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                           "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                           "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
    const std::string rel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
    auto rels = [&](const std::string &body) {
        return head + "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + body + "</Relationships>";
    };
    auto ph = [](const std::string &type, const std::string &xfrm, const std::string &lst) {
        return "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"" + type + "\"/><p:cNvSpPr/><p:nvPr><p:ph" + (type.empty() ? "" : " type=\"" + type + "\"") +
               " idx=\"" + (type == "title" ? "0" : "1") + "\"/></p:nvPr></p:nvSpPr><p:spPr>" + xfrm + "</p:spPr><p:txBody><a:bodyPr/><a:lstStyle>" + lst +
               "</a:lstStyle><a:p><a:r><a:t>prompt</a:t></a:r></a:p></p:txBody></p:sp>";
    };
    auto xfrm = [](long x, long y, long w, long h) {
        return "<a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" + std::to_string(w) + "\" cy=\"" +
               std::to_string(h) + "\"/></a:xfrm>";
    };
    const std::string master =
        head + "<p:sldMaster " + ns + "><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg2\"/></p:bgRef></p:bg><p:spTree>" +
        ph("title", xfrm(100, 200, 300, 400), "") + ph("", xfrm(500, 600, 700, 800), "") +
        "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
        "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
        "<p:txStyles><p:titleStyle><a:lvl1pPr algn=\"ctr\"><a:defRPr sz=\"4400\"><a:solidFill><a:schemeClr val=\"tx2\"/></a:solidFill></a:defRPr></a:lvl1pPr></p:titleStyle>"
        "<p:bodyStyle><a:lvl1pPr><a:buChar char=\"•\"/><a:defRPr sz=\"2800\"/></a:lvl1pPr><a:lvl2pPr><a:buChar char=\"–\"/><a:defRPr sz=\"2400\"/></a:lvl2pPr></p:bodyStyle>"
        "<p:otherStyle/></p:txStyles></p:sldMaster>";
    // The layout moves the body; the title stays where the master has it.
    const std::string layout = head + "<p:sldLayout " + ns + "><p:cSld><p:spTree>" + ph("", xfrm(1500, 1600, 1700, 1800), "") + "</p:spTree></p:cSld></p:sldLayout>";
    const std::string theme =
        head + "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:themeElements><a:clrScheme name=\"t\">"
               "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:srgbClr val=\"FFFFFF\"/></a:lt1>"
               "<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2><a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1>"
               "</a:clrScheme><a:fontScheme name=\"f\"><a:majorFont><a:latin typeface=\"Calibri Light\"/></a:majorFont><a:minorFont><a:latin typeface=\"Calibri\"/></a:minorFont></a:fontScheme>"
               "</a:themeElements></a:theme>";
    const std::string slide =
        head + "<p:sld " + ns + "><p:cSld><p:spTree>"
        "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr><p:spPr/>"
        "<p:txBody><a:bodyPr/><a:p><a:r><a:rPr lang=\"en-US\"/><a:t>Hello</a:t></a:r></a:p></p:txBody></p:sp>"
        "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Content 2\"/><p:cNvSpPr/><p:nvPr><p:ph idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/>"
        "<p:txBody><a:bodyPr/><a:p><a:r><a:t>one</a:t></a:r></a:p><a:p><a:pPr lvl=\"1\"/><a:r><a:t>two</a:t></a:r></a:p>"
        "<a:p><a:pPr><a:buNone/></a:pPr><a:r><a:rPr sz=\"1200\"><a:solidFill><a:schemeClr val=\"accent1\"><a:lumMod val=\"50000\"/></a:schemeClr></a:solidFill></a:rPr><a:t>three</a:t></a:r></a:p></p:txBody></p:sp>"
        "<p:sp><p:nvSpPr><p:cNvPr id=\"4\" name=\"Empty\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"subTitle\" idx=\"2\"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p/></p:txBody></p:sp>"
        "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"5\" name=\"G\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"1000\" y=\"1000\"/><a:ext cx=\"2000\" cy=\"2000\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"1000\" cy=\"1000\"/></a:xfrm></p:grpSpPr>"
        "<p:sp><p:nvSpPr><p:cNvPr id=\"6\" name=\"Box\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>" + xfrm(100, 100, 200, 300) +
        "<a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom></p:spPr><p:style><a:lnRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:lnRef>"
        "<a:fillRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:fillRef><a:effectRef idx=\"0\"><a:schemeClr val=\"accent1\"/></a:effectRef>"
        "<a:fontRef idx=\"minor\"><a:schemeClr val=\"lt1\"/></a:fontRef></p:style><p:txBody><a:bodyPr/><a:p><a:r><a:t>g</a:t></a:r></a:p></p:txBody></p:sp></p:grpSp>"
        "</p:spTree></p:cSld></p:sld>";
    return zip::BuildArchive({
        {"ppt/presentation.xml", head + "<p:presentation " + ns + "><p:sldIdLst><p:sldId id=\"256\" r:id=\"rId7\"/></p:sldIdLst><p:sldSz cx=\"9144000\" cy=\"6858000\"/></p:presentation>"},
        {"ppt/_rels/presentation.xml.rels", rels("<Relationship Id=\"rId7\" Type=\"" + rel + "slide\" Target=\"slides/slide1.xml\"/>"
                                                 "<Relationship Id=\"rId2\" Type=\"" + rel + "theme\" Target=\"theme/theme1.xml\"/>")},
        {"ppt/slides/slide1.xml", slide},
        {"ppt/slides/_rels/slide1.xml.rels", rels("<Relationship Id=\"rId1\" Type=\"" + rel + "slideLayout\" Target=\"../slideLayouts/slideLayout2.xml\"/>")},
        {"ppt/slideLayouts/slideLayout2.xml", layout},
        {"ppt/slideLayouts/_rels/slideLayout2.xml.rels", rels("<Relationship Id=\"rId1\" Type=\"" + rel + "slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>")},
        {"ppt/slideMasters/slideMaster1.xml", master},
        {"ppt/theme/theme1.xml", theme},
    });
}

void TestInheritance() {
    Presentation p;
    std::string err;
    CHECK(LoadPptx(InheritingPptx(), &p, &err));
    CHECK(p.width == 9144000 && p.height == 6858000);
    CHECK(p.slides.size() == 1);
    const Slide &s = p.slides[0];
    CHECK(s.background == "E7E6E6");  // the master's bgRef: bg2 -> lt2
    CHECK(s.shapes.size() == 3);      // the empty subtitle is a prompt, not content
    const Shape &title = s.shapes[0];
    CHECK(title.is_title && title.x == 100 && title.y == 200 && title.w == 300 && title.h == 400);  // the master's position
    CHECK(title.font_pt == 44 && title.text_color == "44546A" && title.paras[0].align == Align::Center);
    const Shape &body = s.shapes[1];
    CHECK(body.x == 1500 && body.y == 1600 && body.w == 1700 && body.h == 1800);  // the layout's, over the master's
    CHECK(body.paras.size() == 3);
    CHECK(body.paras[0].bullet && body.font_pt == 28);
    CHECK(body.paras[1].bullet && body.paras[1].level == 1 && body.paras[1].runs[0].size_pt == 24);
    CHECK(!body.paras[2].bullet && body.paras[2].runs[0].size_pt == 12 && body.paras[2].runs[0].color == "203864");  // accent1 at 50% (PowerPoint: 1F3864)
    const Shape &g = s.shapes[2];
    CHECK(g.kind == ShapeKind::Ellipse);
    CHECK(g.x == 1200 && g.y == 1200 && g.w == 400 && g.h == 600);  // the group's child space, doubled and moved
    CHECK(g.fill == "4472C4" && g.line == "4472C4" && g.text_color == "FFFFFF");
}

void TestHelpers() {
    CHECK(NormalizeColor("#abc") == "AABBCC" && NormalizeColor("1f2328") == "1F2328" && NormalizeColor("red").empty());
    CHECK(OdfLengthToEmu("2.54cm") == 914400 && OdfLengthToEmu("1in") == 914400 && OdfLengthToEmu("72pt") == 914400 &&
          OdfLengthToEmu("25.4mm") == 914400);
    CHECK(IsPresentationPath("a/b.PPTX") && IsPresentationPath("x.odp") && !IsPresentationPath("x.docx"));
    // A new deck and its slides are shaped for the page.
    const Presentation p = NewPresentation();
    CHECK(p.slides.size() == 1 && p.slides[0].shapes.size() == 2 && p.slides[0].shapes[0].is_title);
    for (const Shape &s : NewSlide(p).shapes) CHECK(s.x >= 0 && s.y >= 0 && s.x + s.w <= p.width && s.y + s.h <= p.height);
}
// Typing: text lands in the run it is typed into and takes its style;
// Enter splits a paragraph (the new one keeping its list); Backspace at a
// paragraph's start joins it to the one before; a character is UTF-8.
void TestTextEditing() {
    Shape s;
    Paragraph p;
    p.bullet = true;
    p.runs = {Run("ab"), Run("cd")};
    p.runs[1].bold = true;
    s.paras.push_back(p);
    int para = 0, off = 2;
    InsertText(s, &para, &off, "X");  // at the boundary: the earlier run's style
    CHECK(s.paras[0].runs[0].text == "abX" && s.paras[0].runs[1].text == "cd" && off == 3);
    off = 5;
    InsertText(s, &para, &off, "é");
    CHECK(s.paras[0].runs[1].text == "cdé" && off == 7);
    CHECK(DeleteChar(s, &para, &off, false) && s.paras[0].PlainText() == "abXcd" && off == 5);  // the whole two-byte é
    off = 4;
    SplitParagraph(s, &para, &off);  // between c and d, inside the bold run
    CHECK(s.paras.size() == 2 && para == 1 && off == 0);
    CHECK(s.paras[0].PlainText() == "abXc" && s.paras[1].PlainText() == "d");
    CHECK(s.paras[1].bullet && s.paras[1].runs[0].bold && s.paras[0].runs.back().bold);
    CHECK(DeleteChar(s, &para, &off, false));  // joins
    CHECK(s.paras.size() == 1 && para == 0 && off == 4 && s.paras[0].PlainText() == "abXcd");
    off = 5;
    CHECK(!DeleteChar(s, &para, &off, true));  // nothing after the end of the last paragraph
    off = 0;
    CHECK(DeleteChar(s, &para, &off, true) && s.paras[0].PlainText() == "bXcd");
    // An empty paragraph after bold text types in bold.
    off = 4;
    SplitParagraph(s, &para, &off);
    CHECK(s.paras[1].runs.size() == 1 && s.paras[1].runs[0].text.empty());
    InsertText(s, &para, &off, "new");
    CHECK(s.paras[1].PlainText() == "new" && s.paras[1].runs[0].bold);
    // Deleting a run empty keeps the paragraph's last run for its style.
    Shape t;
    t.paras.push_back(Paragraph{});
    t.paras[0].runs = {Run("a")};
    t.paras[0].runs[0].italic = true;
    para = 0;
    off = 1;
    CHECK(DeleteChar(t, &para, &off, false) && t.paras[0].runs.size() == 1 && t.paras[0].runs[0].italic);
    InsertText(t, &para, &off, "b");
    CHECK(t.paras[0].runs[0].text == "b" && t.paras[0].runs[0].italic);
}
// Maths: a .pptx carries it as Office equations (read back to the same
// TeX), a .odp an equation as a formula object (its TeX kept) and inline
// maths as text; a paragraph formula (mep's mepml export) reads back as a
// paragraph of words and inline maths.
void TestMath() {
    Presentation p = NewPresentation();
    Shape &sub = p.slides[0].shapes[1];
    sub.paras[0].runs.push_back(Run(" with "));
    sub.paras[0].runs.push_back(MathRun("\\alpha^2 + \\frac{1}{2}", false));
    sub.paras[0].runs.push_back(Run(" inline"));
    p.slides[0].shapes.push_back(NewEquation(p, "\\sum_{i=1}^{n} x_i = \\sqrt{y}"));
    CHECK(IsEquationShape(p.slides[0].shapes[2]) && !IsEquationShape(sub));
    std::string err;
    Presentation a;
    CHECK(LoadPptx(SavePptx(p), &a, &err));
    CHECK(Same(Signature(p), Signature(a), "pptx maths round trip"));
    // With a renderer (the editor's typesetter), an equation's fallback is
    // its picture; readers of Office equations still get the maths.
    int drawn = 0;
    SetMathPictureRenderer([&](const std::string &tex, double pt, std::string *png, double *w, double *h) {
        CHECK(tex.find("\\sum") != std::string::npos && pt > 20);
        ++drawn;
        *png = "\x89PNG";
        *w = 50;
        *h = 20;
        return true;
    });
    const std::string with_pictures = SavePptx(p);
    SetMathPictureRenderer(nullptr);
    CHECK(drawn == 1);
    std::string slide1;
    CHECK(zip::Extract(reinterpret_cast<const unsigned char *>(with_pictures.data()), with_pictures.size(), "ppt/slides/slide1.xml", slide1));
    CHECK(slide1.find("<mc:Fallback><p:pic>") != std::string::npos);
    Presentation a2;
    CHECK(LoadPptx(with_pictures, &a2, &err) && Same(Signature(p), Signature(a2), "pptx maths round trip, pictures"));
    Presentation b;
    CHECK(LoadOdp(SaveOdp(p), &b, &err));
    CHECK(b.slides[0].shapes.size() == 3);
    const Shape &eq = b.slides[0].shapes[2];
    CHECK(IsEquationShape(eq) && eq.paras[0].runs[0].tex == "\\sum_{i=1}^{n} x_i = \\sqrt{y}");  // (the TeX annotation: exact)
    CHECK(std::abs(eq.font_pt - 32) < 3);  // the size the frame was made for
    // (Inline maths in an Impress text box is text.)
    CHECK(b.slides[0].shapes[1].PlainText().find("α") != std::string::npos);

    // A paragraph formula, as mep's mepml export writes one.
    const std::string mml =
        "<?xml version=\"1.0\"?><math xmlns=\"http://www.w3.org/1998/Math/MathML\" display=\"inline\"><semantics><mtable columnalign=\"left\">"
        "<mtr><mtd><mrow><mtext mathvariant=\"sans-serif\">Half\u00a0</mtext><mfrac><mn>1</mn><mn>2</mn></mfrac>"
        "<mtext mathvariant=\"bold-sans-serif\">\u00a0and</mtext></mrow></mtd></mtr>"
        "<mtr><mtd><mrow><mtext mathvariant=\"sans-serif\">more</mtext></mrow></mtd></mtr></mtable>"
        "<annotation encoding=\"StarMath 5.0\">alignl font sans \"Half \"</annotation></semantics></math>";
    const std::string ns = "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
                           "xmlns:xlink=\"http://www.w3.org/1999/xlink\" xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\"";
    const std::string content = "<?xml version=\"1.0\"?><office:document-content " + ns +
                                "><office:body><office:presentation><draw:page><draw:frame svg:x=\"1cm\" svg:y=\"1cm\" svg:width=\"10cm\" svg:height=\"2cm\">"
                                "<draw:object xlink:href=\"./Object 1\"/></draw:frame></draw:page></office:presentation></office:body></office:document-content>";
    Presentation c;
    CHECK(LoadOdp(zip::BuildArchive({{"mimetype", "application/vnd.oasis.opendocument.presentation", true}, {"content.xml", content}, {"Object 1/content.xml", mml}}),
                  &c, &err));
    CHECK(c.slides.size() == 1 && c.slides[0].shapes.size() == 1);
    const Shape &pf = c.slides[0].shapes[0];
    CHECK(pf.paras.size() == 1 && pf.paras[0].runs.size() == 4);
    CHECK(pf.paras[0].runs[0].text == "Half " && pf.paras[0].runs[1].tex == "\\frac{1}{2}" && !pf.paras[0].runs[1].display);
    CHECK(pf.paras[0].runs[2].text == " and " && pf.paras[0].runs[2].bold && pf.paras[0].runs[3].text == "more");

    // Editing around maths: an equation is one character; typing beside it
    // does not go into it; deleting it takes its TeX too.
    Shape t;
    t.paras.push_back(Paragraph{});
    int para = 0, off = 0;
    InsertText(t, &para, &off, "ab");
    off = 1;
    InsertMath(t, &para, &off, "x^2");
    CHECK(t.paras[0].runs.size() == 3 && t.paras[0].runs[1].IsMath() && t.paras[0].PlainText() == std::string("a") + kMathChar + "b");
    CHECK(off == 4);
    InsertText(t, &para, &off, "c");  // right after the equation: into the text after it
    CHECK(t.paras[0].runs[1].tex == "x^2" && t.paras[0].runs[1].text == kMathChar && t.paras[0].runs[2].text == "cb");
    off = 4;
    CHECK(DeleteChar(t, &para, &off, false) && off == 1 && t.paras[0].runs.size() == 2 && !t.paras[0].runs[0].IsMath() && !t.paras[0].runs[1].IsMath());
    CHECK(t.paras[0].PlainText() == "acb");
    // An equation alone in its paragraph, typed after.
    Shape u;
    u.paras.push_back(Paragraph{});
    u.paras[0].runs.push_back(MathRun("y", false));
    para = 0;
    off = 3;
    InsertText(u, &para, &off, "!");
    CHECK(u.paras[0].runs.size() == 2 && u.paras[0].runs[0].tex == "y" && u.paras[0].runs[1].text == "!" && !u.paras[0].runs[1].IsMath());
}
}  // namespace

int main() {
    TestHelpers();
    TestRoundTrips();
    TestInheritance();
    TestTextEditing();
    TestMath();
    std::printf("pres_doc tests passed\n");
    return 0;
}
