#include "pdf_writer.h"

#include "deflate.h"
#include "pdf_crypt.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

namespace pdfwrite {

namespace {

using pdfobj::Object;

// -- small Object constructors ---------------------------------------
Object Name(const std::string &n) {
    Object o;
    o.type = pdfobj::Type::Name;
    o.str_val = n;
    return o;
}
Object Int(long long v) {
    Object o;
    o.type = pdfobj::Type::Int;
    o.int_val = v;
    return o;
}
Object Real(double v) {
    Object o;
    o.type = pdfobj::Type::Real;
    o.real_val = v;
    return o;
}
Object RawString(const std::string &bytes) {
    Object o;
    o.type = pdfobj::Type::String;
    o.str_val = bytes;
    return o;
}
Object Bool(bool b) {
    Object o;
    o.type = pdfobj::Type::Bool;
    o.bool_val = b;
    return o;
}
Object Ref(int num, int gen) {
    Object o;
    o.type = pdfobj::Type::Reference;
    o.ref_val.num = num;
    o.ref_val.gen = gen;
    return o;
}
Object Arr(std::vector<Object> items) {
    Object o;
    o.type = pdfobj::Type::Array;
    o.array_val = std::move(items);
    return o;
}

// A PDF *text* string (spec 7.9.2.2): pure-ASCII goes out as-is; anything
// with a byte >= 0x80 is emitted as UTF-16BE with a leading BOM so readers
// render Unicode correctly. The returned bytes go into a String Object and
// are literal-escaped by the serializer. (pdf_annots.cpp reverses this: a
// FEFF-prefixed /Contents is decoded back to UTF-8.)
std::string ToPdfTextBytes(const std::string &utf8) {
    bool ascii = true;
    for (char cc : utf8) {
        if (static_cast<unsigned char>(cc) >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii) return utf8;
    std::string out = "\xFE\xFF";  // UTF-16BE BOM
    size_t i = 0;
    auto put16 = [&](uint32_t u) {
        out.push_back(static_cast<char>((u >> 8) & 0xFF));
        out.push_back(static_cast<char>(u & 0xFF));
    };
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        int n = 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
        else { cp = 0xFFFD; n = 1; }
        if (i + static_cast<size_t>(n) > utf8.size()) { cp = 0xFFFD; n = 1; }
        for (int k = 1; k < n; ++k)
            cp = (cp << 6) | static_cast<uint32_t>(static_cast<unsigned char>(utf8[i + static_cast<size_t>(k)]) & 0x3F);
        i += static_cast<size_t>(n);
        if (cp <= 0xFFFF) {
            put16(cp);
        } else {
            cp -= 0x10000;
            put16(0xD800 + (cp >> 10));
            put16(0xDC00 + (cp & 0x3FF));
        }
    }
    return out;
}

Object TextStr(const std::string &utf8) { return RawString(ToPdfTextBytes(utf8)); }

bool IsRegularNameChar(unsigned char c) {
    if (c < 0x21 || c > 0x7E) return false;
    switch (c) {
        case '#':
        case '(':
        case ')':
        case '<':
        case '>':
        case '[':
        case ']':
        case '{':
        case '}':
        case '/':
        case '%':
            return false;
        default:
            return true;
    }
}

}  // namespace

std::string FormatReal(double x) {
    if (x == 0.0) return "0";  // also normalises -0
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.5f", x);
    std::string s = buf;
    // Trim trailing zeros, then a trailing '.'.
    size_t last = s.find_last_not_of('0');
    if (last != std::string::npos && s[last] == '.') --last;
    s.erase(last + 1);
    if (s == "-0") s = "0";
    return s;
}

std::string EncodeName(const std::string &name) {
    std::string out;
    char hex[8];
    for (char cc : name) {
        unsigned char c = static_cast<unsigned char>(cc);
        if (IsRegularNameChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            std::snprintf(hex, sizeof(hex), "#%02X", c);
            out += hex;
        }
    }
    return out;
}

std::string EncodeLiteralString(const std::string &bytes) {
    std::string out = "(";
    char oct[8];
    for (char cc : bytes) {
        unsigned char c = static_cast<unsigned char>(cc);
        switch (c) {
            case '(': out += "\\("; break;
            case ')': out += "\\)"; break;
            case '\\': out += "\\\\"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    std::snprintf(oct, sizeof(oct), "\\%03o", c);
                    out += oct;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back(')');
    return out;
}

std::string PdfDateString(std::time_t t) {
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "D:%04d%02d%02d%02d%02d%02d+00'00'", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                  tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    return buf;
}

void SerializeValue(const Object &v, std::string *out) {
    switch (v.type) {
        case pdfobj::Type::Null:
            *out += "null";
            break;
        case pdfobj::Type::Bool:
            *out += v.bool_val ? "true" : "false";
            break;
        case pdfobj::Type::Int:
            *out += std::to_string(v.int_val);
            break;
        case pdfobj::Type::Real:
            *out += FormatReal(v.real_val);
            break;
        case pdfobj::Type::Name:
            out->push_back('/');
            *out += EncodeName(v.str_val);
            break;
        case pdfobj::Type::String:
            *out += EncodeLiteralString(v.str_val);
            break;
        case pdfobj::Type::Reference:
            *out += std::to_string(v.ref_val.num);
            out->push_back(' ');
            *out += std::to_string(v.ref_val.gen);
            *out += " R";
            break;
        case pdfobj::Type::Array: {
            out->push_back('[');
            for (size_t i = 0; i < v.array_val.size(); ++i) {
                if (i) out->push_back(' ');
                SerializeValue(v.array_val[i], out);
            }
            out->push_back(']');
            break;
        }
        case pdfobj::Type::Dict: {
            *out += "<<";
            for (const auto &kv : v.dict_val) {
                *out += " /";
                *out += EncodeName(kv.first);
                out->push_back(' ');
                SerializeValue(kv.second, out);
            }
            *out += " >>";
            break;
        }
    }
}

std::string SerializeValue(const Object &v) {
    std::string out;
    SerializeValue(v, &out);
    return out;
}

std::string SerializeIndirectObject(int num, int gen, const Object &dict, const std::string *stream_body,
                                    bool compress) {
    std::string out = std::to_string(num) + " " + std::to_string(gen) + " obj\n";
    if (stream_body) {
        std::string payload = *stream_body;
        Object d = dict;  // augment a copy with /Length (+ /Filter)
        if (compress) {
            payload = deflate::DeflateZlib(reinterpret_cast<const unsigned char *>(payload.data()), payload.size());
            d.dict_val["Filter"] = Name("FlateDecode");
        }
        d.dict_val["Length"] = Int(static_cast<long long>(payload.size()));
        SerializeValue(d, &out);
        out += "\nstream\n";
        out += payload;
        out += "\nendstream\nendobj\n";
    } else {
        SerializeValue(dict, &out);
        out += "\nendobj\n";
    }
    return out;
}

namespace {

Object Deref(const unsigned char *data, size_t len, const pdfxref::XrefTable &table, const Object &o) {
    if (!o.IsReference()) return o;
    return pdfxref::ResolveObject(data, len, table, o.ref_val.num, o.ref_val.gen);
}

// The byte offset the file's most recent `startxref` points at -- the
// /Prev target for our appended section. Same backward scan pdf_xref.cpp
// uses. Returns -1 if none (should not happen for a loaded document).
long long FindLastStartxref(const unsigned char *data, size_t len) {
    std::string s(reinterpret_cast<const char *>(data), len);
    size_t p = s.rfind("startxref");
    if (p == std::string::npos) return -1;
    p += 9;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t')) ++p;
    long long v = 0;
    bool any = false;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
        v = v * 10 + (s[p] - '0');
        ++p;
        any = true;
    }
    return any ? v : -1;
}

std::vector<Object> QuadNumbers(const pdfannots::Quad &q) {
    return {Real(q.x1), Real(q.y1), Real(q.x2), Real(q.y2), Real(q.x3), Real(q.y3), Real(q.x4), Real(q.y4)};
}

Object RectArray(const double r[4]) { return Arr({Real(r[0]), Real(r[1]), Real(r[2]), Real(r[3])}); }
Object ColorArray(const double c[3]) { return Arr({Real(c[0]), Real(c[1]), Real(c[2])}); }

std::string DateOrNow(const pdfannots::PdfAnnot &a, std::time_t now) {
    return a.modified.empty() ? PdfDateString(now) : a.modified;
}

// Appearance-stream Form XObject for a highlight: one Multiply-blended
// filled rectangle per quad, in the annotation's own colour, so the
// underlying glyphs stay visible. BBox == /Rect, identity matrix, so the
// content coordinates are the page coordinates the QuadPoints use.
Object BuildHighlightApDict(const pdfannots::PdfAnnot &a) {
    Object ext;  // << /GS0 << /BM /Multiply /ca <opacity> >> >>
    ext.type = pdfobj::Type::Dict;
    Object gs0;
    gs0.type = pdfobj::Type::Dict;
    gs0.dict_val["BM"] = Name("Multiply");
    gs0.dict_val["ca"] = Real(a.opacity);
    ext.dict_val["GS0"] = gs0;
    Object resources;
    resources.type = pdfobj::Type::Dict;
    resources.dict_val["ExtGState"] = ext;
    Object group;
    group.type = pdfobj::Type::Dict;
    group.dict_val["S"] = Name("Transparency");
    group.dict_val["CS"] = Name("DeviceRGB");
    Object d;
    d.type = pdfobj::Type::Dict;
    d.dict_val["Type"] = Name("XObject");
    d.dict_val["Subtype"] = Name("Form");
    d.dict_val["FormType"] = Int(1);
    d.dict_val["BBox"] = RectArray(a.rect);
    d.dict_val["Group"] = group;
    d.dict_val["Resources"] = resources;
    return d;
}

std::string BuildHighlightApStream(const pdfannots::PdfAnnot &a) {
    std::string s = "/GS0 gs\n";
    s += FormatReal(a.color[0]) + " " + FormatReal(a.color[1]) + " " + FormatReal(a.color[2]) + " rg\n";
    for (const pdfannots::Quad &q : a.quads) {
        double minx = std::min({q.x1, q.x2, q.x3, q.x4});
        double maxx = std::max({q.x1, q.x2, q.x3, q.x4});
        double miny = std::min({q.y1, q.y2, q.y3, q.y4});
        double maxy = std::max({q.y1, q.y2, q.y3, q.y4});
        s += FormatReal(minx) + " " + FormatReal(miny) + " " + FormatReal(maxx - minx) + " " +
             FormatReal(maxy - miny) + " re\n";
    }
    s += "f\n";
    return s;
}

Object BuildHighlightDict(const pdfannots::PdfAnnot &a, int ap_num, std::time_t now) {
    Object d;
    d.type = pdfobj::Type::Dict;
    d.dict_val["Type"] = Name("Annot");
    d.dict_val["Subtype"] = Name("Highlight");
    d.dict_val["Rect"] = RectArray(a.rect);
    std::vector<Object> qp;
    for (const pdfannots::Quad &q : a.quads) {
        std::vector<Object> n = QuadNumbers(q);
        qp.insert(qp.end(), n.begin(), n.end());
    }
    d.dict_val["QuadPoints"] = Arr(std::move(qp));
    d.dict_val["C"] = ColorArray(a.color);
    d.dict_val["CA"] = Real(a.opacity);
    if (!a.contents.empty()) d.dict_val["Contents"] = TextStr(a.contents);
    if (!a.author.empty()) d.dict_val["T"] = TextStr(a.author);
    d.dict_val["M"] = RawString(DateOrNow(a, now));
    d.dict_val["F"] = Int(4);  // Print flag
    if (ap_num > 0) {
        Object ap;
        ap.type = pdfobj::Type::Dict;
        ap.dict_val["N"] = Ref(ap_num, 0);
        d.dict_val["AP"] = ap;
    }
    return d;
}

Object BuildTextDict(const pdfannots::PdfAnnot &a, std::time_t now) {
    Object d;
    d.type = pdfobj::Type::Dict;
    d.dict_val["Type"] = Name("Annot");
    d.dict_val["Subtype"] = Name("Text");
    d.dict_val["Rect"] = RectArray(a.rect);
    if (!a.contents.empty()) d.dict_val["Contents"] = TextStr(a.contents);
    if (!a.author.empty()) d.dict_val["T"] = TextStr(a.author);
    d.dict_val["M"] = RawString(DateOrNow(a, now));
    d.dict_val["Name"] = Name(a.icon.empty() ? "Note" : a.icon);
    d.dict_val["Open"] = Bool(a.open);
    d.dict_val["C"] = ColorArray(a.color);
    d.dict_val["F"] = Int(4);
    return d;
}

}  // namespace

std::string BuildIncrementalUpdate(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                   const pdfdoc::PdfDocument &document, const std::vector<pdfannots::PdfAnnot> &adds,
                                   const std::vector<pdfannots::PdfAnnot> &edits,
                                   const std::vector<AnnotDelete> &deletes, std::time_t now, std::string *err) {
    if (table.IsEncrypted()) {
        if (err) *err = "cannot add annotations to an encrypted PDF";
        return "";
    }
    if (adds.empty() && edits.empty() && deletes.empty()) {
        if (err) *err = "no annotation changes to write";
        return "";
    }

    std::string out(reinterpret_cast<const char *>(data), len);
    if (out.empty() || out.back() != '\n') out.push_back('\n');

    // Next free object number: past both the trailer /Size and the highest
    // xref key actually present.
    long long size = 0;
    if (const Object *sz = table.Trailer().Find("Size")) size = sz->AsInt();
    long long next_num = size;
    for (const auto &kv : table.Entries()) next_num = std::max(next_num, static_cast<long long>(kv.first) + 1);
    if (next_num < 1) next_num = 1;

    // Group each change kind by page; the set of touched pages is their union.
    std::map<int, std::vector<const pdfannots::PdfAnnot *>> adds_by_page, edits_by_page;
    std::map<int, std::set<int>> deletes_by_page;
    std::set<int> touched_pages;
    for (const pdfannots::PdfAnnot &a : adds) { adds_by_page[a.page].push_back(&a); touched_pages.insert(a.page); }
    for (const pdfannots::PdfAnnot &a : edits) { edits_by_page[a.page].push_back(&a); touched_pages.insert(a.page); }
    for (const AnnotDelete &d : deletes) { deletes_by_page[d.page].insert(d.obj_num); touched_pages.insert(d.page); }

    struct Written {
        int num, gen;
        size_t offset;
    };
    std::vector<Written> written;

    // Emits a highlight/text annotation dict (+ AP stream for a highlight)
    // at `obj_num` (0 = allocate a fresh number). Returns the object number.
    auto emit_annot = [&](const pdfannots::PdfAnnot &a, int obj_num) -> int {
        int ap_num = 0;
        if (a.kind == pdfannots::Kind::Highlight && !a.quads.empty()) {
            ap_num = static_cast<int>(next_num++);
            Object ap_dict = BuildHighlightApDict(a);
            std::string ap_stream = BuildHighlightApStream(a);
            written.push_back({ap_num, 0, out.size()});
            out += SerializeIndirectObject(ap_num, 0, ap_dict, &ap_stream, /*compress=*/false);
        }
        int num = obj_num > 0 ? obj_num : static_cast<int>(next_num++);
        Object adict =
            a.kind == pdfannots::Kind::Highlight ? BuildHighlightDict(a, ap_num, now) : BuildTextDict(a, now);
        written.push_back({num, 0, out.size()});
        out += SerializeIndirectObject(num, 0, adict);
        return num;
    };

    for (int page_index : touched_pages) {
        const pdfdoc::Page *page = document.GetPage(page_index);
        if (!page) continue;
        const std::set<int> &dels = deletes_by_page[page_index];

        // Preserve the page's existing /Annots refs, minus any being deleted.
        std::vector<Object> annot_array;
        if (const Object *ar = page->dict.Find("Annots")) {
            Object resolved = Deref(data, len, table, *ar);
            if (resolved.IsArray()) {
                for (const Object &r : resolved.array_val) {
                    if (r.IsReference() && dels.count(r.ref_val.num)) continue;  // deleted
                    annot_array.push_back(r);
                }
            }
        }

        // Edits: re-emit the rebuilt dict at the annotation's OWN object
        // number (its existing ref stays in annot_array, now shadowed).
        for (const pdfannots::PdfAnnot *a : edits_by_page[page_index]) {
            if (a->src_obj > 0) emit_annot(*a, a->src_obj);
        }
        // Adds: fresh object number + a new ref.
        for (const pdfannots::PdfAnnot *a : adds_by_page[page_index]) {
            int num = emit_annot(*a, 0);
            annot_array.push_back(Ref(num, 0));
        }

        // Rewrite the page object in place (same number) with the new
        // /Annots array. Pages that lived in an ObjStm become top-level;
        // this classic entry shadows the old compressed one.
        Object pdict = page->dict;
        pdict.dict_val["Annots"] = Arr(std::move(annot_array));
        written.push_back({page->object_num, 0, out.size()});
        out += SerializeIndirectObject(page->object_num, 0, pdict);
    }

    if (written.empty()) {
        if (err) *err = "no writable pages for the given annotation changes";
        return "";
    }

    // -- classic cross-reference section over exactly the written objects.
    std::sort(written.begin(), written.end(), [](const Written &a, const Written &b) { return a.num < b.num; });
    // De-dup by object number (a page can only appear once, but be safe):
    // keep the last write for any repeated number.
    std::map<int, Written> by_num;
    for (const Written &w : written) by_num[w.num] = w;

    size_t xref_off = out.size();
    out += "xref\n";
    // Emit contiguous runs.
    auto it = by_num.begin();
    while (it != by_num.end()) {
        int start = it->first;
        std::vector<Written> run;
        int expect = start;
        while (it != by_num.end() && it->first == expect) {
            run.push_back(it->second);
            ++it;
            ++expect;
        }
        out += std::to_string(start) + " " + std::to_string(run.size()) + "\n";
        char rec[32];
        for (const Written &w : run) {
            std::snprintf(rec, sizeof(rec), "%010lld %05d n \n", static_cast<long long>(w.offset), w.gen);
            out += rec;
        }
    }

    long long new_size = std::max(next_num, size);
    long long prev = FindLastStartxref(data, len);

    Object trailer;
    trailer.type = pdfobj::Type::Dict;
    trailer.dict_val["Size"] = Int(new_size);
    if (const Object *root = table.Trailer().Find("Root")) trailer.dict_val["Root"] = *root;
    if (const Object *info = table.Trailer().Find("Info")) trailer.dict_val["Info"] = *info;
    if (prev >= 0) trailer.dict_val["Prev"] = Int(prev);

    // /ID: keep id0 from the original (if any), regenerate id1 (spec 14.4).
    std::string id0, id1;
    if (const Object *id = table.Trailer().Find("ID"); id && id->IsArray() && id->array_val.size() == 2) {
        if (id->array_val[0].IsString()) id0 = id->array_val[0].str_val;
        if (id->array_val[1].IsString()) id1 = id->array_val[1].str_val;
    }
    std::string seed(reinterpret_cast<const char *>(data), len);
    seed += std::to_string(static_cast<long long>(now));
    id1 = pdfcrypt::Md5(seed);
    if (id0.empty()) id0 = id1;
    trailer.dict_val["ID"] = Arr({RawString(id0), RawString(id1)});

    out += "trailer\n";
    out += SerializeValue(trailer);
    out += "\nstartxref\n" + std::to_string(xref_off) + "\n%%EOF\n";
    return out;
}

}  // namespace pdfwrite
