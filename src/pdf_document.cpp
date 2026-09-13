#include "pdf_document.h"

#include <algorithm>

namespace pdfdoc {

namespace {

pdfobj::Object Deref(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                      const pdfobj::Object &obj) {
    if (!obj.IsReference()) return obj;
    return pdfxref::ResolveObject(data, len, table, obj.ref_val.num, obj.ref_val.gen);
}

// Reads a /MediaBox- or /CropBox-shaped array (4 numbers, each possibly
// itself an indirect reference -- rare, but tolerated) into out[4].
// Leaves out[] untouched (caller already seeded it with the inherited/
// default value) if `box` isn't a well-formed 4-element numeric array.
// Returns whether it actually read something.
bool ReadBoxIfValid(const unsigned char *data, size_t len, const pdfxref::XrefTable &table, const pdfobj::Object &box,
                     double out[4]) {
    if (!box.IsArray() || box.array_val.size() != 4) return false;
    double values[4];
    for (int i = 0; i < 4; ++i) {
        pdfobj::Object v = Deref(data, len, table, box.array_val[static_cast<size_t>(i)]);
        if (!v.IsNumber()) return false;  // malformed element: keep the caller's existing box entirely
        values[i] = v.AsDouble();
    }
    // Normalize so [0]/[1] are the lower-left corner regardless of the
    // order a nonconforming producer wrote them in.
    out[0] = std::min(values[0], values[2]);
    out[1] = std::min(values[1], values[3]);
    out[2] = std::max(values[0], values[2]);
    out[3] = std::max(values[1], values[3]);
    return true;
}

// Intersects `box` with `bound` in place, clamping to whatever overlap
// exists (matches PDFium's own real behavior of never letting /CropBox
// extend a page beyond its physical /MediaBox, per spec 7.7.3.3). If
// there's no overlap at all (a degenerate/nonsensical CropBox), `bound`
// wins entirely rather than producing a zero/negative-size page.
void IntersectBox(double box[4], const double bound[4]) {
    double x0 = std::max(box[0], bound[0]);
    double y0 = std::max(box[1], bound[1]);
    double x1 = std::min(box[2], bound[2]);
    double y1 = std::min(box[3], bound[3]);
    if (x1 <= x0 || y1 <= y0) {
        std::copy(bound, bound + 4, box);
        return;
    }
    box[0] = x0;
    box[1] = y0;
    box[2] = x1;
    box[3] = y1;
}

int NormalizeRotate(long long value) {
    int r = static_cast<int>(value % 360);
    if (r < 0) r += 360;
    r = (r / 90) * 90;  // fold a non-multiple-of-90 value down rather than rejecting it
    return r % 360;
}

}  // namespace

void PdfDocument::Load(const unsigned char *data, size_t len) {
    pages_.clear();
    table_ = pdfxref::XrefTable();
    table_.Load(data, len);

    // PDFIUM_REMOVAL_PLAN.md Phase 12: a document that's encrypted but
    // couldn't be unlocked with an empty password (a real, non-empty
    // user password is required, or it uses an unsupported encryption
    // scheme) can't safely resolve anything past its own trailer --
    // every string/stream would come back as still-encrypted garbage.
    // Leaving pages_ empty here (PageCount() == 0) matches PDFium's own
    // FPDF_ERR_PASSWORD outcome; a caller that needs to distinguish this
    // from "just an empty/malformed document" checks
    // Xref().IsEncrypted() && !Xref().Encryption() directly.
    if (table_.IsEncrypted() && !table_.Encryption()) return;

    const pdfobj::Object *root_ref = table_.Trailer().Find("Root");
    if (!root_ref) return;
    pdfobj::Object catalog = Deref(data, len, table_, *root_ref);
    if (!catalog.IsDict()) return;
    const pdfobj::Object *pages_entry = catalog.Find("Pages");
    if (!pages_entry) return;

    InheritedAttrs root_attrs;
    std::vector<int> visited;

    if (pages_entry->IsReference()) {
        FlattenPageTree(data, len, pages_entry->ref_val, root_attrs, visited);
    } else if (pages_entry->IsDict()) {
        // Directly-embedded /Pages node (no indirection) -- rare but
        // tolerated; synthesize a Ref of {0,0} so cycle tracking still
        // has a slot, and recurse by hand since FlattenPageTree expects
        // to resolve a Ref itself.
        const pdfobj::Object &node = *pages_entry;
        const pdfobj::Object *kids = node.Find("Kids");
        if (kids && kids->IsArray()) {
            InheritedAttrs attrs = root_attrs;
            if (const pdfobj::Object *r = node.Find("Resources")) attrs.resources = Deref(data, len, table_, *r);
            if (const pdfobj::Object *mb = node.Find("MediaBox")) ReadBoxIfValid(data, len, table_, *mb, attrs.media_box);
            if (const pdfobj::Object *cb = node.Find("CropBox")) {
                if (ReadBoxIfValid(data, len, table_, *cb, attrs.crop_box)) attrs.has_crop_box = true;
            }
            if (const pdfobj::Object *rot = node.Find("Rotate")) attrs.rotate = NormalizeRotate(rot->AsInt());
            for (const auto &kid : kids->array_val) {
                if (kid.IsReference()) FlattenPageTree(data, len, kid.ref_val, attrs, visited);
            }
        }
    }
}

void PdfDocument::FlattenPageTree(const unsigned char *data, size_t len, pdfobj::Ref ref,
                                   const InheritedAttrs &inherited, std::vector<int> &visited) {
    if (std::find(visited.begin(), visited.end(), ref.num) != visited.end()) return;  // cyclic /Kids: stop
    visited.push_back(ref.num);
    if (visited.size() > 100000) return;  // pathological tree depth/breadth: bail rather than hang

    pdfobj::Object node = pdfxref::ResolveObject(data, len, table_, ref.num, ref.gen);
    if (!node.IsDict()) return;

    InheritedAttrs attrs = inherited;
    if (const pdfobj::Object *r = node.Find("Resources")) attrs.resources = Deref(data, len, table_, *r);
    if (const pdfobj::Object *mb = node.Find("MediaBox")) ReadBoxIfValid(data, len, table_, *mb, attrs.media_box);
    if (const pdfobj::Object *cb = node.Find("CropBox")) {
        if (ReadBoxIfValid(data, len, table_, *cb, attrs.crop_box)) attrs.has_crop_box = true;
    }
    if (const pdfobj::Object *rot = node.Find("Rotate")) attrs.rotate = NormalizeRotate(rot->AsInt());

    const pdfobj::Object *kids = node.Find("Kids");
    const pdfobj::Object *type = node.Find("Type");
    bool is_pages_node = (type && type->AsString("") == "Pages") || (!type && kids && kids->IsArray());

    if (is_pages_node && kids && kids->IsArray()) {
        for (const auto &kid : kids->array_val) {
            if (kid.IsReference()) FlattenPageTree(data, len, kid.ref_val, attrs, visited);
        }
        return;
    }

    // Leaf /Page (or something tolerated as one -- no /Kids means it
    // can't be descended into further regardless of what /Type claims).
    Page page;
    page.object_num = ref.num;
    page.dict = node;
    page.resources = attrs.resources;
    std::copy(attrs.media_box, attrs.media_box + 4, page.media_box);
    page.rotate = attrs.rotate;
    if (attrs.has_crop_box) {
        std::copy(attrs.crop_box, attrs.crop_box + 4, page.effective_box);
        IntersectBox(page.effective_box, attrs.media_box);
    } else {
        std::copy(attrs.media_box, attrs.media_box + 4, page.effective_box);
    }
    pages_.push_back(std::move(page));
}

const Page *PdfDocument::GetPage(int index) const {
    if (index < 0 || index >= static_cast<int>(pages_.size())) return nullptr;
    return &pages_[static_cast<size_t>(index)];
}

double PdfDocument::PageWidthPt(int index) const {
    const Page *page = GetPage(index);
    if (!page) return 0;
    double w = page->effective_box[2] - page->effective_box[0];
    double h = page->effective_box[3] - page->effective_box[1];
    return (page->rotate == 90 || page->rotate == 270) ? h : w;
}

double PdfDocument::PageHeightPt(int index) const {
    const Page *page = GetPage(index);
    if (!page) return 0;
    double w = page->effective_box[2] - page->effective_box[0];
    double h = page->effective_box[3] - page->effective_box[1];
    return (page->rotate == 90 || page->rotate == 270) ? w : h;
}

int PdfDocument::PageIndexForObjectNum(int object_num) const {
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (pages_[i].object_num == object_num) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace pdfdoc
