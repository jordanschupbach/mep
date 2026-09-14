#include "pdf_links.h"

#include "pdf_outline.h"

#include <algorithm>

namespace pdflinks {

namespace {

// Same [a b c d e f] affine transform and page-to-device derivation as
// pdf_content.h/.cpp's own pdfrender::Mat2D/PageToDeviceMatrix/Transform
// -- duplicated rather than shared, same tradeoff as pdf_outline.cpp's
// own AppendUtf8 comment explains: pdf_content.cpp pulls in the font/
// rasterizer stack (pdf_font.h, gfx/rasterizer.h) for its content-stream
// interpreter, none of which a Link-annotation-rect reader needs, and
// this transform is a self-contained ~15 lines with no other
// dependents worth a new shared header over. Keep any change to the
// derivation itself in sync with pdf_content.cpp's own copy.
struct Mat2D {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

void Transform(const Mat2D &m, double x, double y, double *out_x, double *out_y) {
    *out_x = m.a * x + m.c * y + m.e;
    *out_y = m.b * x + m.d * y + m.f;
}

Mat2D PageToDeviceMatrix(double llx, double lly, double urx, double ury, int rotate, double px_per_pt) {
    double w = urx - llx, h = ury - lly;
    Mat2D m;
    if (rotate == 90) {
        m = Mat2D{0, 1, 1, 0, 0, 0};
    } else if (rotate == 180) {
        m = Mat2D{-1, 0, 0, 1, w, 0};
    } else if (rotate == 270) {
        m = Mat2D{0, -1, -1, 0, h, w};
    } else {
        m = Mat2D{1, 0, 0, -1, 0, h};
    }
    m.e = m.e - m.a * llx - m.c * lly;
    m.f = m.f - m.b * llx - m.d * lly;
    m.a *= px_per_pt;
    m.b *= px_per_pt;
    m.c *= px_per_pt;
    m.d *= px_per_pt;
    m.e *= px_per_pt;
    m.f *= px_per_pt;
    return m;
}

}  // namespace

std::vector<PdfLinkAnnot> GetPageLinks(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                        const pdfdoc::PdfDocument &document, int page_index, float px_per_pt) {
    std::vector<PdfLinkAnnot> out;
    const pdfdoc::Page *page = document.GetPage(page_index);
    if (!page) return out;

    const pdfobj::Object *annots_ref = page->dict.Find("Annots");
    if (!annots_ref) return out;
    pdfobj::Object annots =
        annots_ref->IsReference()
            ? pdfxref::ResolveObject(data, len, table, annots_ref->ref_val.num, annots_ref->ref_val.gen)
            : *annots_ref;
    if (!annots.IsArray()) return out;

    Mat2D m = PageToDeviceMatrix(page->effective_box[0], page->effective_box[1], page->effective_box[2],
                                  page->effective_box[3], page->rotate, static_cast<double>(px_per_pt));

    for (const pdfobj::Object &annot_ref : annots.array_val) {
        pdfobj::Object annot = annot_ref.IsReference()
                                    ? pdfxref::ResolveObject(data, len, table, annot_ref.ref_val.num, annot_ref.ref_val.gen)
                                    : annot_ref;
        if (!annot.IsDict()) continue;
        const pdfobj::Object *subtype = annot.Find("Subtype");
        if (!subtype || !subtype->IsName() || subtype->str_val != "Link") continue;

        const pdfobj::Object *rect = annot.Find("Rect");
        if (!rect || !rect->IsArray() || rect->array_val.size() != 4) continue;
        double rx0 = rect->array_val[0].AsDouble();
        double ry0 = rect->array_val[1].AsDouble();
        double rx1 = rect->array_val[2].AsDouble();
        double ry1 = rect->array_val[3].AsDouble();

        double dx0, dy0, dx1, dy1;
        Transform(m, rx0, ry0, &dx0, &dy0);
        Transform(m, rx1, ry1, &dx1, &dy1);

        PdfLinkAnnot link;
        link.x0 = static_cast<float>(std::min(dx0, dx1));
        link.x1 = static_cast<float>(std::max(dx0, dx1));
        link.y0 = static_cast<float>(std::min(dy0, dy1));
        link.y1 = static_cast<float>(std::max(dy0, dy1));

        // A Link annotation's target is either its own /Dest, or an /A
        // action -- exactly the same two shapes WalkOutline checks for an
        // outline item (pdf_outline.cpp), except a Link's /A can also be
        // a /URI action rather than only /GoTo, checked for first since
        // ResolveDestPage has nothing useful to do with one (its /S is
        // "URI", not "GoTo", so it Find("D")s a key that doesn't exist
        // and correctly returns -1 anyway -- checking explicitly here
        // just avoids that pointless call and actually recovers the URI).
        const pdfobj::Object *action = annot.Find("A");
        if (action) {
            pdfobj::Object resolved_action =
                action->IsReference() ? pdfxref::ResolveObject(data, len, table, action->ref_val.num, action->ref_val.gen)
                                       : *action;
            const pdfobj::Object *subtype_s = resolved_action.IsDict() ? resolved_action.Find("S") : nullptr;
            if (subtype_s && subtype_s->IsName() && subtype_s->str_val == "URI") {
                const pdfobj::Object *uri = resolved_action.Find("URI");
                if (uri && uri->IsString()) link.uri = uri->str_val;
            } else {
                link.target_page = pdfoutline::ResolveDestPage(data, len, table, document, *action);
            }
        } else if (const pdfobj::Object *dest = annot.Find("Dest")) {
            link.target_page = pdfoutline::ResolveDestPage(data, len, table, document, *dest);
        }

        if (link.target_page < 0 && link.uri.empty()) continue;  // neither shape resolved: nothing to jump to
        out.push_back(link);
    }
    return out;
}

}  // namespace pdflinks
