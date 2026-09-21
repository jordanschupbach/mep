#include "pdf_annots.h"

#include <algorithm>

namespace pdfannots {

namespace {

// Resolves an indirect reference to its target object; passes any other
// object through unchanged. Same helper shape pdf_links.cpp/pdf_content.cpp
// use.
pdfobj::Object Deref(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                     const pdfobj::Object &obj) {
    if (!obj.IsReference()) return obj;
    return pdfxref::ResolveObject(data, len, table, obj.ref_val.num, obj.ref_val.gen);
}

// A PDF /C colour array is 0 (transparent), 1 (gray), 3 (RGB) or 4
// (CMYK) numbers (spec 12.5.6.3). Normalises to RGB 0..1; leaves the
// caller's default in place for the 0-length ("no colour") case.
bool ReadColor(const pdfobj::Object &c, double out_rgb[3]) {
    if (!c.IsArray()) return false;
    const auto &a = c.array_val;
    if (a.size() == 1) {
        double g = a[0].AsDouble();
        out_rgb[0] = out_rgb[1] = out_rgb[2] = g;
        return true;
    }
    if (a.size() == 3) {
        out_rgb[0] = a[0].AsDouble();
        out_rgb[1] = a[1].AsDouble();
        out_rgb[2] = a[2].AsDouble();
        return true;
    }
    if (a.size() == 4) {
        double cy = a[0].AsDouble(), m = a[1].AsDouble(), ye = a[2].AsDouble(), k = a[3].AsDouble();
        out_rgb[0] = (1.0 - cy) * (1.0 - k);
        out_rgb[1] = (1.0 - m) * (1.0 - k);
        out_rgb[2] = (1.0 - ye) * (1.0 - k);
        return true;
    }
    return false;
}

}  // namespace

std::vector<PdfAnnot> GetPageAnnots(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                    const pdfdoc::PdfDocument &document, int page_index) {
    std::vector<PdfAnnot> out;
    const pdfdoc::Page *page = document.GetPage(page_index);
    if (!page) return out;

    const pdfobj::Object *annots_ref = page->dict.Find("Annots");
    if (!annots_ref) return out;
    pdfobj::Object annots = Deref(data, len, table, *annots_ref);
    if (!annots.IsArray()) return out;

    for (const pdfobj::Object &annot_ref : annots.array_val) {
        pdfobj::Object annot = Deref(data, len, table, annot_ref);
        if (!annot.IsDict()) continue;
        const pdfobj::Object *subtype = annot.Find("Subtype");
        if (!subtype || !subtype->IsName()) continue;

        PdfAnnot a;
        a.page = page_index;
        a.from_file = true;
        if (annot_ref.IsReference()) {
            a.src_obj = annot_ref.ref_val.num;
            a.src_gen = annot_ref.ref_val.gen;
        }

        if (subtype->str_val == "Highlight") {
            a.kind = Kind::Highlight;
        } else if (subtype->str_val == "Text") {
            a.kind = Kind::Text;
        } else {
            continue;  // not a markup annotation this reader handles
        }

        const pdfobj::Object *rect = annot.Find("Rect");
        if (!rect || !rect->IsArray() || rect->array_val.size() != 4) continue;
        double rx0 = rect->array_val[0].AsDouble(), ry0 = rect->array_val[1].AsDouble();
        double rx1 = rect->array_val[2].AsDouble(), ry1 = rect->array_val[3].AsDouble();
        a.rect[0] = std::min(rx0, rx1);
        a.rect[1] = std::min(ry0, ry1);
        a.rect[2] = std::max(rx0, rx1);
        a.rect[3] = std::max(ry0, ry1);

        if (const pdfobj::Object *c = annot.Find("C")) ReadColor(*c, a.color);
        if (const pdfobj::Object *ca = annot.Find("CA")) a.opacity = ca->AsDouble(a.opacity);
        if (const pdfobj::Object *contents = annot.Find("Contents"); contents && contents->IsString())
            a.contents = contents->str_val;
        if (const pdfobj::Object *t = annot.Find("T"); t && t->IsString()) a.author = t->str_val;
        if (const pdfobj::Object *m = annot.Find("M"); m && m->IsString()) a.modified = m->str_val;

        if (a.kind == Kind::Highlight) {
            const pdfobj::Object *qp = annot.Find("QuadPoints");
            if (!qp || !qp->IsArray() || qp->array_val.size() < 8) continue;
            const auto &q = qp->array_val;
            size_t nquads = q.size() / 8;
            for (size_t i = 0; i < nquads; ++i) {
                Quad quad;
                const size_t b = i * 8;
                quad.x1 = q[b + 0].AsDouble();
                quad.y1 = q[b + 1].AsDouble();
                quad.x2 = q[b + 2].AsDouble();
                quad.y2 = q[b + 3].AsDouble();
                quad.x3 = q[b + 4].AsDouble();
                quad.y3 = q[b + 5].AsDouble();
                quad.x4 = q[b + 6].AsDouble();
                quad.y4 = q[b + 7].AsDouble();
                a.quads.push_back(quad);
            }
            if (a.quads.empty()) continue;
        } else {  // Text
            if (const pdfobj::Object *name = annot.Find("Name"); name && name->IsName()) a.icon = name->str_val;
            if (const pdfobj::Object *open = annot.Find("Open"); open && open->type == pdfobj::Type::Bool)
                a.open = open->bool_val;
        }

        out.push_back(std::move(a));
    }
    return out;
}

}  // namespace pdfannots
