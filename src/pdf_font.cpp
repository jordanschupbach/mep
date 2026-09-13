#include "pdf_font.h"

#include "gfx/cff.h"
#include "gfx/truetype.h"
#include "office_font_data.h"
#include "office_font_data_mono.h"
#include "office_font_data_serif.h"
#include "pdf_encodings.h"
#include "pdf_filters.h"

#include <algorithm>
#include <cctype>

namespace pdffont {

namespace {

pdfobj::Object Deref(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                      const pdfobj::Object &obj) {
    if (!obj.IsReference()) return obj;
    return pdfxref::ResolveObject(data, len, table, obj.ref_val.num, obj.ref_val.gen);
}

enum class Engine { kNone, kTrueType, kCff };

// -- Standard-14 substitution -----------------------------------------
// Matches a /BaseFont name (subset tag already stripped by the caller)
// against mep's own already-vendored Liberation Sans/Serif/Mono (see
// src/office_font_data*.h) via a family/weight/style heuristic rather
// than an exhaustive alias table -- real-world PDF producers spell
// "Arial Bold"/"Arial,Bold"/"Arial-BoldMT"/etc. too many different ways
// to enumerate, and this heuristic covers all of them uniformly.

struct SubstituteFont {
    const unsigned char *data;
    unsigned int len;
};

bool ContainsCI(const std::string &haystack_lower, const char *needle) { return haystack_lower.find(needle) != std::string::npos; }

// Symbol/ZapfDingbats have no Liberation equivalent (custom, non-Latin
// glyph sets) -- returns true so the caller renders no glyphs for them
// (Scoping decision, see pdf_font.h's own top comment) while still
// tracking widths.
bool IsSymbolFont(const std::string &lower) { return ContainsCI(lower, "symbol") || ContainsCI(lower, "dingbat") || ContainsCI(lower, "wingding"); }

SubstituteFont PickStandard14Substitute(const std::string &base_font) {
    std::string lower = base_font;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool bold = ContainsCI(lower, "bold");
    bool italic = ContainsCI(lower, "italic") || ContainsCI(lower, "oblique");

    bool mono = ContainsCI(lower, "courier") || ContainsCI(lower, "mono") || ContainsCI(lower, "consol");
    bool serif = ContainsCI(lower, "times") || ContainsCI(lower, "serif") || ContainsCI(lower, "georgia") || ContainsCI(lower, "cambria") || ContainsCI(lower, "garamond") || ContainsCI(lower, "minion") || ContainsCI(lower, "roman");

    if (mono) {
        if (bold && italic) return {kLiberationMonoBoldItalicTtf, kLiberationMonoBoldItalicTtfLen};
        if (bold) return {kLiberationMonoBoldTtf, kLiberationMonoBoldTtfLen};
        if (italic) return {kLiberationMonoItalicTtf, kLiberationMonoItalicTtfLen};
        return {kLiberationMonoRegularTtf, kLiberationMonoRegularTtfLen};
    }
    if (serif) {
        if (bold && italic) return {kLiberationSerifBoldItalicTtf, kLiberationSerifBoldItalicTtfLen};
        if (bold) return {kLiberationSerifBoldTtf, kLiberationSerifBoldTtfLen};
        if (italic) return {kLiberationSerifItalicTtf, kLiberationSerifItalicTtfLen};
        return {kLiberationSerifRegularTtf, kLiberationSerifRegularTtfLen};
    }
    // Default: Sans (covers Helvetica/Arial and any unrecognized name --
    // the most common real-world fallback choice).
    if (bold && italic) return {kLiberationSansBoldItalicTtf, kLiberationSansBoldItalicTtfLen};
    if (bold) return {kLiberationSansBoldTtf, kLiberationSansBoldTtfLen};
    if (italic) return {kLiberationSansItalicTtf, kLiberationSansItalicTtfLen};
    return {kLiberationSansRegularTtf, kLiberationSansRegularTtfLen};
}

std::string StripSubsetTag(const std::string &name) {
    // Spec 9.6.4: a subsetted font's BaseFont is prefixed with exactly
    // 6 uppercase letters then '+' (e.g. "IHAIZV+LMRoman12-Bold").
    if (name.size() > 7 && name[6] == '+') {
        bool all_upper = true;
        for (int i = 0; i < 6; ++i) {
            if (!std::isupper(static_cast<unsigned char>(name[static_cast<size_t>(i)]))) {
                all_upper = false;
                break;
            }
        }
        if (all_upper) return name.substr(7);
    }
    return name;
}

// Resolves a CFF SID to its glyph-name text -- predefined (see
// pdf_encodings.h) or this font's own custom String INDEX.
std::string CffSidToName(const gfx::cff::FontInfo &info, int sid) {
    if (sid < pdfenc::CffStandardStringCount()) {
        const char *s = pdfenc::CffStandardString(sid);
        return s ? s : "";
    }
    int custom_index = sid - pdfenc::CffStandardStringCount();
    const unsigned char *p = nullptr;
    uint32_t len = 0;
    if (!gfx::cff::GetIndexItem(info.strings, custom_index, &p, &len)) return "";
    return std::string(reinterpret_cast<const char *>(p), len);
}

// -- UTF-8 / UTF-16BE helpers (PDFIUM_REMOVAL_PLAN.md Phase 11's
// /ToUnicode CMap support, and GetUnicodeText's own simple-font
// fallback encoding) -------------------------------------------------

void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes UTF-16BE bytes (the encoding /ToUnicode destination hex
// strings always use, per spec 9.10.3) to UTF-8, handling surrogate
// pairs for supplementary-plane codepoints.
std::string Utf16BeToUtf8(const std::vector<uint16_t> &units) {
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            ++i;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

std::vector<uint16_t> BytesToUtf16Units(const std::string &be_bytes) {
    std::vector<uint16_t> units;
    for (size_t i = 0; i + 1 < be_bytes.size(); i += 2) {
        units.push_back(static_cast<uint16_t>((static_cast<unsigned char>(be_bytes[i]) << 8) |
                                                static_cast<unsigned char>(be_bytes[i + 1])));
    }
    return units;
}

uint32_t BytesToCode(const std::string &bytes) {
    uint32_t v = 0;
    for (char c : bytes) v = (v << 8) | static_cast<unsigned char>(c);
    return v;
}

// -- Minimal CMap tokenizer (bfchar/bfrange blocks only -- everything
// else in a /ToUnicode CMap program, e.g. the surrounding PostScript
// dict/procset boilerplate, is simply skipped) -- reuses
// pdfobj::ParseObject for hex strings/arrays/numbers, same "bare
// letter-led run = keyword" convention as pdf_content.cpp's own
// content-stream tokenizer (a small deliberate duplication rather than
// sharing that file's private anonymous-namespace code).

bool IsCMapWs(unsigned char c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
bool IsCMapDelim(unsigned char c) {
    switch (c) {
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
            return true;
        default:
            return false;
    }
}

struct CMapToken {
    bool is_keyword = false;
    pdfobj::Object operand;
    std::string keyword;
};

bool NextCMapToken(const unsigned char *data, size_t len, size_t &pos, CMapToken *out) {
    pdfobj::SkipWhitespaceAndComments(data, len, pos);
    if (pos >= len) return false;
    unsigned char c = data[pos];
    if (c == '/' || c == '(' || c == '[' || c == '<' || c == '+' || c == '-' || c == '.' ||
        (c >= '0' && c <= '9')) {
        out->is_keyword = false;
        return pdfobj::ParseObject(data, len, pos, &out->operand);
    }
    size_t start = pos;
    while (pos < len && !IsCMapWs(data[pos]) && !IsCMapDelim(data[pos])) ++pos;
    if (pos == start) {
        ++pos;  // stray delimiter (e.g. a lone ']'): skip one byte, keep going
        return NextCMapToken(data, len, pos, out);
    }
    out->is_keyword = true;
    out->keyword.assign(reinterpret_cast<const char *>(data + start), pos - start);
    return true;
}

}  // namespace

struct PdfFont::Impl {
    Engine engine = Engine::kNone;
    std::string font_bytes;  // owns the decoded (or substitute) font program; tt_info/cff_info point into this
    gfx::tt::FontInfo tt_info;
    gfx::cff::FontInfo cff_info;
    double unit_scale = 0.001;  // font units -> text space (1/1000 em); TrueType: 1.0/unitsPerEm, CFF: its own FontMatrix[0]
    bool skip_glyphs = false;   // Symbol/ZapfDingbats substitution: track widths, draw nothing

    // Simple-font state (BytesPerCode() == 1).
    int code_to_unicode[256];      // -1 if unmapped; TrueType named-encoding path
    int code_to_gid[256];          // >=0 if resolved directly (CFF name->GID, or TrueType symbolic/no-cmap fallback); -1 otherwise
    double code_width[256];        // -1.0 sentinel = not in /Widths, falls back to code_engine_width then missing_width
    double code_engine_width[256]; // -1.0 sentinel = not computed (CFF, or an unmapped code); else the resolved glyph's own advance, in 1/1000-em -- see pdf_font.h's own "use the substitute's own metrics" design note
    double missing_width = 0;
    bool has_missing_width = false;  // true only if /FontDescriptor /MissingWidth was actually present -- see GetWidth's own precedence note

    // Composite-font state (BytesPerCode() == 2).
    bool cid_to_gid_identity = true;
    std::vector<uint16_t> cid_to_gid_map;  // only when !cid_to_gid_identity -- CIDFontType2 (TrueType) /CIDToGIDMap stream
    std::unordered_map<int, int> cff_cid_to_gid;  // CIDFontType0 (CFF) only -- see GidForCid's own comment
    std::unordered_map<int, double> cid_width;
    double default_width = 1000;

    // /ToUnicode CMap (spec 9.10.3), code -> UTF-8 text -- independent
    // of BytesPerCode()'s 1-vs-2-byte split (simple and composite fonts
    // both key this the same way, by whatever raw code value
    // BytesPerCode() already extracts). Populated by LoadToUnicode;
    // empty if the font dict has no /ToUnicode at all, in which case
    // GetUnicodeText falls back to the encoding-implied code_to_unicode
    // table above (simple fonts only).
    std::unordered_map<uint32_t, std::string> to_unicode;

    Impl() {
        std::fill(std::begin(code_to_unicode), std::end(code_to_unicode), -1);
        std::fill(std::begin(code_to_gid), std::end(code_to_gid), -1);
        std::fill(std::begin(code_width), std::end(code_width), -1.0);
        std::fill(std::begin(code_engine_width), std::end(code_engine_width), -1.0);
    }

    // **Real bug found and fixed here**: CID -> GID resolution is
    // fundamentally different between the 2 composite font subtypes,
    // and an earlier version of this function treated them the same
    // way (always identity or always the PDF's own /CIDToGIDMap),
    // silently producing garbage-or-out-of-range GIDs for every
    // CIDFontType0C font -- caught by a live end-to-end render of a
    // real Identity-H CIDFontType0C fixture showing a correctly
    // positioned/spaced but completely BLANK text run (every glyph
    // bitmap nullptr, since the raw CID was being used directly as an
    // out-of-range GID into a small subsetted font).
    //   - CIDFontType2 (TrueType): /CIDToGIDMap is the PDF's own
    //     explicit instruction (an embedded stream, or /Identity) --
    //     `cid_to_gid_map`/`cid_to_gid_identity` above, unchanged.
    //   - CIDFontType0 (CFF): spec 9.7.4.2 says there is NO
    //     /CIDToGIDMap for this subtype at all -- the CID is mapped to
    //     a GID via the CFF font's OWN internal charset, which for a
    //     CID-keyed CFF font holds CIDs (not SIDs) per GID (spec CFF
    //     Annex, same `gfx::cff::FontInfo::charset` field Phase 9
    //     already parses for the non-CID SID case) -- `cff_cid_to_gid`
    //     above, built once from that charset by LoadCompositeFont.
    int GidForCid(int cid) const {
        if (engine == Engine::kCff) {
            auto it = cff_cid_to_gid.find(cid);
            if (it != cff_cid_to_gid.end()) return it->second;
            return cid;  // no charset info at all (predefined charset, vanishingly rare): identity fallback
        }
        if (cid_to_gid_identity) return cid;
        if (cid < 0 || static_cast<size_t>(cid) >= cid_to_gid_map.size()) return 0;
        return cid_to_gid_map[static_cast<size_t>(cid)];
    }

    void LoadSimpleFontWidths(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                               const pdfobj::Object &font_dict);
    void LoadSimpleFontEngine(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                               const pdfobj::Object &font_dict);
    void LoadCompositeFont(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                            const pdfobj::Object &font_dict);
    void LoadToUnicode(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                        const pdfobj::Object &font_dict);
};

PdfFont::PdfFont() : impl_(std::make_unique<Impl>()) {}
PdfFont::~PdfFont() = default;
PdfFont::PdfFont(PdfFont &&) noexcept = default;
PdfFont &PdfFont::operator=(PdfFont &&) noexcept = default;

namespace {

// Builds code(0-255) -> glyph name from /Encoding (a Base name, or a
// dict with /BaseEncoding + /Differences). When /Encoding is absent
// entirely, spec 9.6.6.2's own default kicks in: StandardEncoding for a
// non-symbolic font (crucially including every standard-14 substitute
// this module picks, none of which are symbolic -- Symbol/ZapfDingbats
// already skip glyph rendering entirely via IsSymbolFont before this is
// ever called) -- ONLY a genuinely symbolic font gets `has_encoding =
// false` (empty table throughout), the real case Phase 9's own
// verification found (an embedded, subsetted, cmap-less TrueType font),
// where callers fall back to a direct code/GID path instead of name
// lookup.
//
// **Bug found and fixed here (not just documented as a scope note):**
// the first version of this function treated "no /Encoding" as always
// meaning "leave empty, use code-as-GID" regardless of the symbolic
// flag -- correct for the real embedded-symbolic case, but wrong for a
// standard-14 substitute (`Helvetica` with no /Encoding, the common
// case), where "code == GID" is nonsense against a freshly-loaded
// Liberation Sans's own unrelated internal glyph ordering: found via
// this phase's own live-render check producing a wildly wrong,
// enormous glyph bitmap (garbage contour data from an arbitrary GID),
// not a clean failure.
void BuildSimpleEncoding(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                          const pdfobj::Object *encoding_obj, bool symbolic, std::vector<std::string> &code_to_name,
                          bool &has_encoding) {
    code_to_name.assign(256, std::string());
    pdfenc::Base base = pdfenc::Base::kStandard;
    const pdfobj::Object *differences = nullptr;
    // `resolved` must outlive this whole function, NOT just the `else`
    // branch below: `differences` is a raw pointer into `resolved`'s own
    // dict_val map (via Find), so it dangles the moment `resolved` goes
    // out of scope. **Real bug found and fixed**: an earlier version
    // declared `resolved` block-local to the `else` branch -- undefined
    // behavior once `differences->IsArray()` ran after that block
    // closed, caught by a real /Differences override silently not
    // taking effect in a live test (code 65 kept resolving via the base
    // encoding instead of the Differences-overridden name), not a
    // crash -- exactly the kind of "looks like it mostly works" UB
    // symptom that makes this class of bug dangerous.
    pdfobj::Object resolved;

    if (!encoding_obj) {
        has_encoding = !symbolic;  // non-symbolic default: StandardEncoding; symbolic: empty, caller falls back to code-as-GID/cmap-direct
    } else {
        resolved = Deref(doc_data, doc_len, table, *encoding_obj);
        if (resolved.IsName()) {
            has_encoding = true;
            const std::string &n = resolved.str_val;
            if (n == "WinAnsiEncoding") base = pdfenc::Base::kWinAnsi;
            else if (n == "MacRomanEncoding") base = pdfenc::Base::kMacRoman;
        } else if (resolved.IsDict()) {
            has_encoding = true;
            if (const pdfobj::Object *be = resolved.Find("BaseEncoding")) {
                const std::string &n = be->AsString("");
                if (n == "WinAnsiEncoding") base = pdfenc::Base::kWinAnsi;
                else if (n == "MacRomanEncoding") base = pdfenc::Base::kMacRoman;
            }
            differences = resolved.Find("Differences");
        } else {
            has_encoding = false;
        }
    }
    if (!has_encoding) return;

    for (int c = 0; c < 256; ++c) {
        const char *name = pdfenc::EncodingName(base, c);
        if (name) code_to_name[static_cast<size_t>(c)] = name;
    }
    if (differences && differences->IsArray()) {
        int cur = 0;
        for (const auto &item : differences->array_val) {
            if (item.IsNumber()) {
                cur = static_cast<int>(item.AsInt());
            } else if (item.IsName() && cur >= 0 && cur < 256) {
                code_to_name[static_cast<size_t>(cur)] = item.str_val;
                ++cur;
            }
        }
    }
}

}  // namespace

void PdfFont::Impl::LoadSimpleFontWidths(const unsigned char *doc_data, size_t doc_len,
                                          const pdfxref::XrefTable &table, const pdfobj::Object &font_dict) {
    int first_char = 0;
    if (const pdfobj::Object *fc = font_dict.Find("FirstChar")) first_char = static_cast<int>(fc->AsInt());
    // **Real bug found and fixed here**: /Widths is virtually always an
    // indirect reference in real-world PDFs (confirmed via
    // test/pdf_fixtures/tectonic_test.pdf's own font objects) -- the
    // same class of bug already found and fixed for /Resources back in
    // Phase 8 (pdf_document.cpp), recurring here because this function
    // read the raw dict entry without dereferencing it. Caught by a
    // live end-to-end render: every glyph advanced by 0 (silently
    // falling through to code_engine_width or missing_width instead of
    // the PDF's own real per-glyph widths), producing severely
    // overlapping text -- not a crash, another instance of this
    // codebase's broader lesson that an un-dereferenced indirect
    // reference tends to fail silently (Find/IsArray simply return
    // false/empty on a bare Reference object) rather than loudly.
    const pdfobj::Object *widths_entry = font_dict.Find("Widths");
    pdfobj::Object widths = widths_entry ? Deref(doc_data, doc_len, table, *widths_entry) : pdfobj::Object();
    if (widths.IsArray()) {
        for (size_t i = 0; i < widths.array_val.size(); ++i) {
            int code = first_char + static_cast<int>(i);
            if (code < 0 || code > 255) continue;
            code_width[static_cast<size_t>(code)] = widths.array_val[i].AsDouble();
        }
    }
    // Note: FontDescriptor is resolved by the caller (LoadSimpleFontEngine
    // already derefs it for the engine/symbolic lookup) -- MissingWidth
    // itself is read there too, to avoid re-deriving the same
    // dereferenced dict a second time here.
}

void PdfFont::Impl::LoadSimpleFontEngine(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                          const pdfobj::Object &font_dict) {
    const pdfobj::Object *descriptor_ref = font_dict.Find("FontDescriptor");
    pdfobj::Object descriptor = descriptor_ref ? Deref(doc_data, doc_len, table, *descriptor_ref) : pdfobj::Object();

    // The FontDescriptor's Symbolic flag (bit 3, value 0x4) determines
    // BuildSimpleEncoding's default when /Encoding is entirely absent
    // (see that function's own doc comment for the real bug this
    // distinction fixes) -- beyond that one default choice, this module
    // doesn't separately implement full cmap-subtable-priority selection
    // for symbolic TrueType fonts (real symbolic subsetted fonts found
    // during Phase 9's own verification had no cmap at all, making the
    // direct code-as-GID fallback the one that actually matters there).
    bool symbolic = false;
    if (const pdfobj::Object *flags = descriptor.Find("Flags")) symbolic = (flags->AsInt() & 0x4) != 0;
    if (const pdfobj::Object *mw = descriptor.Find("MissingWidth")) {
        missing_width = mw->AsDouble();
        has_missing_width = true;
    }

    const pdfobj::Object *ff2 = descriptor.Find("FontFile2");
    const pdfobj::Object *ff3 = descriptor.Find("FontFile3");
    std::string raw, decoded;
    pdfobj::Object stream_dict;

    if (ff2 && ff2->IsReference() &&
        pdfxref::ResolveStream(doc_data, doc_len, table, ff2->ref_val.num, ff2->ref_val.gen, &stream_dict, &raw) &&
        pdffilter::DecodeStream(raw, &stream_dict, &decoded)) {
        font_bytes = std::move(decoded);
        if (gfx::tt::InitFont(&tt_info, reinterpret_cast<const unsigned char *>(font_bytes.data()),
                               static_cast<int>(font_bytes.size()))) {
            engine = Engine::kTrueType;
            unit_scale = tt_info.units_per_em > 0 ? 1.0 / tt_info.units_per_em : 0.001;
        }
    } else if (ff3 && ff3->IsReference() &&
               pdfxref::ResolveStream(doc_data, doc_len, table, ff3->ref_val.num, ff3->ref_val.gen, &stream_dict,
                                       &raw) &&
               pdffilter::DecodeStream(raw, &stream_dict, &decoded)) {
        font_bytes = std::move(decoded);
        if (gfx::cff::InitFont(&cff_info, reinterpret_cast<const unsigned char *>(font_bytes.data()),
                                static_cast<int>(font_bytes.size()))) {
            engine = Engine::kCff;
            unit_scale = cff_info.font_matrix[0];
        }
    }

    if (engine == Engine::kNone) {
        // No embedded font program: standard-14 substitution.
        std::string base_font;
        if (const pdfobj::Object *bf = font_dict.Find("BaseFont")) base_font = StripSubsetTag(bf->AsString(""));
        std::string lower = base_font;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        skip_glyphs = IsSymbolFont(lower);
        if (!skip_glyphs) {
            SubstituteFont sub = PickStandard14Substitute(base_font);
            font_bytes.assign(reinterpret_cast<const char *>(sub.data), sub.len);
            if (gfx::tt::InitFont(&tt_info, reinterpret_cast<const unsigned char *>(font_bytes.data()),
                                   static_cast<int>(font_bytes.size()))) {
                engine = Engine::kTrueType;
                unit_scale = tt_info.units_per_em > 0 ? 1.0 / tt_info.units_per_em : 0.001;
            }
        }
    }

    std::vector<std::string> code_to_name;
    bool has_encoding = false;
    const pdfobj::Object *encoding_obj = font_dict.Find("Encoding");
    BuildSimpleEncoding(doc_data, doc_len, table, encoding_obj, symbolic, code_to_name, has_encoding);

    if (engine == Engine::kTrueType) {
        for (int c = 0; c < 256; ++c) {
            if (has_encoding && !code_to_name[static_cast<size_t>(c)].empty()) {
                int unicode = pdfenc::GlyphNameToUnicode(code_to_name[static_cast<size_t>(c)]);
                if (unicode >= 0) code_to_unicode[c] = unicode;
            }
            // No usable name (symbolic-and-no-encoding, or an
            // unrecognized Differences name): fall back to using the
            // code directly as a glyph index -- correct for the real
            // no-cmap subsetted-font case this plan's own Phase 9
            // verification found, and a reasonable tolerant guess
            // otherwise (spec's own (3,0)-cmap-at-0xF000+code priority
            // for symbolic fonts is not separately implemented -- most
            // real symbolic subsetted fonts have no cmap at all, per
            // that same finding, making this fallback the one that
            // actually matters in practice).
            if (code_to_unicode[c] < 0) code_to_gid[c] = c;

            // Fallback advance width for this code, from the resolved
            // engine's own hmtx -- used by GetWidth only when the PDF's
            // own /Widths doesn't cover this code (pdf_font.h's own
            // design note: keep shape and spacing self-consistent by
            // using the SAME substitute font's own metrics, rather than
            // a flat 0/MissingWidth default. **Real gap found and
            // fixed here**: this fallback was documented in pdf_font.h
            // from the start but never actually implemented -- caught
            // by a live text-advance test where consecutive glyphs
            // rendered stacked on top of each other, since every
            // unwidthed code silently advanced by 0).
            int advance = 0, lsb = 0;
            if (code_to_unicode[c] >= 0) {
                gfx::tt::GetCodepointHMetrics(&tt_info, code_to_unicode[c], &advance, &lsb);
            } else if (code_to_gid[c] >= 0) {
                gfx::tt::GetGlyphHMetrics(&tt_info, code_to_gid[c], &advance, &lsb);
            }
            if (advance > 0) code_engine_width[c] = static_cast<double>(advance) * unit_scale * 1000.0;
        }
    } else if (engine == Engine::kCff) {
        for (int c = 0; c < 256; ++c) {
            if (!has_encoding || code_to_name[static_cast<size_t>(c)].empty()) {
                code_to_gid[c] = c;  // no PDF /Encoding: fall back to code-as-GID, same reasoning as TrueType above
                continue;
            }
            const std::string &name = code_to_name[static_cast<size_t>(c)];
            // Encoding-implied Unicode for GetUnicodeText's fallback
            // (PDFIUM_REMOVAL_PLAN.md Phase 11) -- entirely independent
            // of the GID resolution loop below (which drives what's
            // actually drawn), added here purely so simple CFF fonts
            // (mep's own flagship real fixture's font subtype) get the
            // same text-extraction fallback TrueType already had.
            int unicode = pdfenc::GlyphNameToUnicode(name);
            if (unicode >= 0) code_to_unicode[c] = unicode;
            for (int gid = 0; gid < cff_info.num_glyphs; ++gid) {
                // **Off-by-one bug found and fixed here**: `charset` is
                // indexed DIRECTLY by gid (gfx::cff::FontInfo::charset's
                // own ParseCharset sizes the array to num_glyphs and
                // leaves index 0 as an unused placeholder -- .notdef's
                // SID 0 is never written there, not omitted from the
                // array's length), NOT offset by one the way an earlier
                // version of this loop read it (`charset[gid-1]`).
                // That off-by-one silently resolved every simple CFF
                // font's code to an adjacent-but-wrong glyph -- caught
                // by a live end-to-end render showing real, legible-
                // looking-but-wrong text (most letters shifted by
                // +1 in the font's own glyph order, e.g. "Lua" ->
                // "Mvb"), not a crash or an empty page.
                int sid = gid == 0 ? 0
                                    : (gid < static_cast<int>(cff_info.charset.size()) ? cff_info.charset[static_cast<size_t>(gid)] : 0);
                if (CffSidToName(cff_info, sid) == name) {
                    code_to_gid[c] = gid;
                    break;
                }
            }
        }
    }

    LoadSimpleFontWidths(doc_data, doc_len, table, font_dict);
}

void PdfFont::Impl::LoadCompositeFont(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                       const pdfobj::Object &font_dict) {
    const pdfobj::Object *descendants = font_dict.Find("DescendantFonts");
    if (!descendants || !descendants->IsArray() || descendants->array_val.empty()) return;
    pdfobj::Object cid_font = Deref(doc_data, doc_len, table, descendants->array_val[0]);
    if (!cid_font.IsDict()) return;

    if (const pdfobj::Object *dw = cid_font.Find("DW")) default_width = dw->AsDouble(1000);
    // Same indirect-reference-not-dereferenced bug class as /Widths
    // above and /Resources back in Phase 8 -- confirmed for real via
    // test/pdf_fixtures/jpeg_test.pdf, whose CIDFontType0 dict has
    // `/W 20 0 R`, not an inline array.
    if (const pdfobj::Object *w_entry = cid_font.Find("W")) {
        pdfobj::Object w_obj = Deref(doc_data, doc_len, table, *w_entry);
        if (w_obj.IsArray()) {
            size_t i = 0;
            const auto &arr = w_obj.array_val;
            while (i < arr.size()) {
                if (!arr[i].IsNumber()) break;
                int c_first = static_cast<int>(arr[i].AsInt());
                if (i + 1 >= arr.size()) break;
                if (arr[i + 1].IsArray()) {
                    const auto &widths = arr[i + 1].array_val;
                    for (size_t k = 0; k < widths.size(); ++k) cid_width[c_first + static_cast<int>(k)] = widths[k].AsDouble();
                    i += 2;
                } else if (arr[i + 1].IsNumber() && i + 2 < arr.size() && arr[i + 2].IsNumber()) {
                    int c_last = static_cast<int>(arr[i + 1].AsInt());
                    double width = arr[i + 2].AsDouble();
                    for (int c = c_first; c <= c_last; ++c) cid_width[c] = width;
                    i += 3;
                } else {
                    break;
                }
            }
        }
    }

    if (const pdfobj::Object *c2g = cid_font.Find("CIDToGIDMap")) {
        if (c2g->IsReference()) {
            pdfobj::Object c2g_stream_dict;
            std::string c2g_raw, c2g_decoded;
            if (pdfxref::ResolveStream(doc_data, doc_len, table, c2g->ref_val.num, c2g->ref_val.gen, &c2g_stream_dict,
                                        &c2g_raw) &&
                pdffilter::DecodeStream(c2g_raw, &c2g_stream_dict, &c2g_decoded)) {
                cid_to_gid_identity = false;
                cid_to_gid_map.resize(c2g_decoded.size() / 2);
                for (size_t i = 0; i + 1 < c2g_decoded.size(); i += 2) {
                    cid_to_gid_map[i / 2] = static_cast<uint16_t>((static_cast<unsigned char>(c2g_decoded[i]) << 8) |
                                                                   static_cast<unsigned char>(c2g_decoded[i + 1]));
                }
            }
        }
        // A Name value (only legal value is /Identity) keeps the default identity mapping.
    }

    const pdfobj::Object *descriptor_ref = cid_font.Find("FontDescriptor");
    pdfobj::Object descriptor = descriptor_ref ? Deref(doc_data, doc_len, table, *descriptor_ref) : pdfobj::Object();
    const pdfobj::Object *ff2 = descriptor.Find("FontFile2");
    const pdfobj::Object *ff3 = descriptor.Find("FontFile3");
    std::string raw, decoded;
    pdfobj::Object stream_dict;

    if (ff2 && ff2->IsReference() &&
        pdfxref::ResolveStream(doc_data, doc_len, table, ff2->ref_val.num, ff2->ref_val.gen, &stream_dict, &raw) &&
        pdffilter::DecodeStream(raw, &stream_dict, &decoded)) {
        font_bytes = std::move(decoded);
        if (gfx::tt::InitFont(&tt_info, reinterpret_cast<const unsigned char *>(font_bytes.data()),
                               static_cast<int>(font_bytes.size()))) {
            engine = Engine::kTrueType;
            unit_scale = tt_info.units_per_em > 0 ? 1.0 / tt_info.units_per_em : 0.001;
        }
    } else if (ff3 && ff3->IsReference() &&
               pdfxref::ResolveStream(doc_data, doc_len, table, ff3->ref_val.num, ff3->ref_val.gen, &stream_dict,
                                       &raw) &&
               pdffilter::DecodeStream(raw, &stream_dict, &decoded)) {
        font_bytes = std::move(decoded);
        if (gfx::cff::InitFont(&cff_info, reinterpret_cast<const unsigned char *>(font_bytes.data()),
                                static_cast<int>(font_bytes.size()))) {
            engine = Engine::kCff;
            unit_scale = cff_info.font_matrix[0];
            // CID-keyed CFF: charset[gid] holds that glyph's CID (see
            // GidForCid's own comment) -- invert once into a CID->GID
            // map. gid 0 (.notdef) is always CID 0, matching the SID
            // convention Phase 9's ParseCharset already established.
            if (cff_info.is_cid) {
                cff_cid_to_gid[0] = 0;
                for (int gid = 1; gid < cff_info.num_glyphs && gid < static_cast<int>(cff_info.charset.size()); ++gid) {
                    cff_cid_to_gid[cff_info.charset[static_cast<size_t>(gid)]] = gid;
                }
            }
        }
    }
    // No embedded font on a composite font: standard-14 substitution is
    // out of scope here (composite non-embedded standard fonts are rare
    // to nonexistent in practice -- Identity-H composite fonts are
    // themselves almost always produced BECAUSE the font is embedded,
    // to access glyphs beyond 256 codes). Falls through with
    // engine==kNone, i.e. no glyphs drawn but widths still tracked.
}

void PdfFont::Impl::LoadToUnicode(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                   const pdfobj::Object &font_dict) {
    const pdfobj::Object *tu = font_dict.Find("ToUnicode");
    if (!tu || !tu->IsReference()) return;
    pdfobj::Object stream_dict;
    std::string raw;
    if (!pdfxref::ResolveStream(doc_data, doc_len, table, tu->ref_val.num, tu->ref_val.gen, &stream_dict, &raw)) {
        return;
    }
    std::string decoded;
    if (!pdffilter::DecodeStream(raw, &stream_dict, &decoded)) return;

    const unsigned char *data = reinterpret_cast<const unsigned char *>(decoded.data());
    size_t len = decoded.size();
    size_t pos = 0;
    enum class Mode { kNone, kBfChar, kBfRange } mode = Mode::kNone;
    std::vector<pdfobj::Object> stack;
    CMapToken tok;
    while (NextCMapToken(data, len, pos, &tok)) {
        if (!tok.is_keyword) {
            stack.push_back(std::move(tok.operand));
        } else if (tok.keyword == "beginbfchar") {
            mode = Mode::kBfChar;
            stack.clear();
        } else if (tok.keyword == "beginbfrange") {
            mode = Mode::kBfRange;
            stack.clear();
        } else if (tok.keyword == "endbfchar" || tok.keyword == "endbfrange") {
            mode = Mode::kNone;
            stack.clear();
        } else {
            stack.clear();  // any other keyword (dict/procset boilerplate): not accumulated across it
        }

        if (mode == Mode::kBfChar && stack.size() == 2) {
            if (stack[0].IsString() && stack[1].IsString()) {
                uint32_t code = BytesToCode(stack[0].str_val);
                to_unicode[code] = Utf16BeToUtf8(BytesToUtf16Units(stack[1].str_val));
            }
            stack.clear();
        } else if (mode == Mode::kBfRange && stack.size() == 3) {
            if (stack[0].IsString() && stack[1].IsString()) {
                uint32_t lo = BytesToCode(stack[0].str_val);
                uint32_t hi = BytesToCode(stack[1].str_val);
                if (stack[2].IsArray()) {
                    // <srcLo> <srcHi> [<dst0> <dst1> ...]: one explicit destination per code.
                    for (size_t i = 0; i < stack[2].array_val.size() && lo + i <= hi; ++i) {
                        if (stack[2].array_val[i].IsString()) {
                            to_unicode[lo + static_cast<uint32_t>(i)] =
                                Utf16BeToUtf8(BytesToUtf16Units(stack[2].array_val[i].str_val));
                        }
                    }
                } else if (stack[2].IsString() && hi >= lo && hi - lo < 65536) {
                    // <srcLo> <srcHi> <dst>: sequential codes increment only
                    // the destination's OWN LAST UTF-16 code unit (spec
                    // 9.10.3), leaving any preceding units (a multi-char
                    // dst prefix) unchanged.
                    std::vector<uint16_t> base = BytesToUtf16Units(stack[2].str_val);
                    for (uint32_t c = lo; c <= hi; ++c) {
                        std::vector<uint16_t> units = base;
                        if (!units.empty()) units.back() = static_cast<uint16_t>(units.back() + (c - lo));
                        to_unicode[c] = Utf16BeToUtf8(units);
                    }
                }
            }
            stack.clear();
        }
    }
}

std::string PdfFont::GetUnicodeText(uint32_t code) const {
    auto it = impl_->to_unicode.find(code);
    if (it != impl_->to_unicode.end()) return it->second;
    if (!is_composite_ && code <= 255 && impl_->code_to_unicode[code] >= 0) {
        std::string out;
        AppendUtf8(out, static_cast<uint32_t>(impl_->code_to_unicode[code]));
        return out;
    }
    return "";
}

void PdfFont::Load(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                    const pdfobj::Object &font_dict) {
    const pdfobj::Object *subtype = font_dict.Find("Subtype");
    is_composite_ = subtype && subtype->AsString("") == "Type0";
    if (is_composite_) {
        impl_->LoadCompositeFont(doc_data, doc_len, table, font_dict);
    } else {
        impl_->LoadSimpleFontEngine(doc_data, doc_len, table, font_dict);
    }
    // /ToUnicode lives on the top-level Font dict for BOTH simple and
    // composite fonts (spec 9.10.3) -- loaded once here rather than
    // duplicated in each of the 2 branches above.
    impl_->LoadToUnicode(doc_data, doc_len, table, font_dict);
}

double PdfFont::GetWidth(uint32_t code) const {
    if (is_composite_) {
        auto it = impl_->cid_width.find(static_cast<int>(code));
        return it != impl_->cid_width.end() ? it->second : impl_->default_width;
    }
    if (code > 255) return impl_->missing_width;
    if (impl_->code_width[code] >= 0) return impl_->code_width[code];
    // Precedence (see pdf_font.h's own design note): the PDF's own
    // explicit /MissingWidth, when given, is the document author's
    // real instruction and wins over this module's own added
    // substitute-font-metrics guess -- that guess exists only to avoid
    // a literal 0-width default (stacked glyphs) when the PDF gives no
    // guidance at all. **Precedence bug found and fixed here**: an
    // earlier version always preferred the engine-metrics fallback over
    // missing_width whenever it was available, backwards from what
    // pdf_font.h's own header comment already documented -- caught by
    // a real explicit-MissingWidth test expecting 250 and getting the
    // substitute font's own unrelated advance instead.
    if (impl_->has_missing_width) return impl_->missing_width;
    if (impl_->code_engine_width[code] >= 0) return impl_->code_engine_width[code];
    return impl_->missing_width;
}

unsigned char *PdfFont::GetGlyphBitmap(uint32_t code, float scale_x, float scale_y, int *width, int *height,
                                       int *xoff, int *yoff) const {
    *width = *height = *xoff = *yoff = 0;
    if (impl_->skip_glyphs || impl_->engine == Engine::kNone) return nullptr;

    // unit_scale converts the font's OWN internal glyph-outline units
    // (1/unitsPerEm for TrueType, FontMatrix[0] -- typically also 0.001
    // -- for CFF) to text-space em fractions, so `scale_x * unit_scale`
    // is "device pixels per font-internal-unit", exactly the convention
    // gfx::tt::GetCodepointBitmap/gfx::cff::GetGlyphBitmap's own
    // scale_x/scale_y already expect -- callers of THIS function pass
    // scale_x/scale_y as plain "device pixels per em" (e.g. `font_size *
    // ctm_scale`, no /1000 anywhere). **Bug found and fixed here**: an
    // earlier version multiplied by an extra 1000.0, wrongly conflating
    // this font-internal-units scale with GetWidth's UNRELATED "/Widths
    // values are in 1/1000-em text-space units" convention -- caught by
    // a live end-to-end render producing a wildly oversized, garbled
    // glyph bitmap (a ~1000x-too-large scale factor), not a clean
    // failure. Cross-checked against gfx::tt directly (bypassing this
    // module) to confirm the corrected formula, before and after.
    float fx = static_cast<float>(static_cast<double>(scale_x) * impl_->unit_scale);
    float fy = static_cast<float>(static_cast<double>(scale_y) * impl_->unit_scale);

    if (is_composite_) {
        int gid = impl_->GidForCid(static_cast<int>(code));
        if (impl_->engine == Engine::kTrueType) return gfx::tt::GetGlyphBitmap(&impl_->tt_info, fx, fy, gid, width, height, xoff, yoff);
        return gfx::cff::GetGlyphBitmap(&impl_->cff_info, fx, fy, gid, width, height, xoff, yoff);
    }

    if (code > 255) return nullptr;
    if (impl_->engine == Engine::kTrueType) {
        int unicode = impl_->code_to_unicode[code];
        if (unicode >= 0) return gfx::tt::GetCodepointBitmap(&impl_->tt_info, fx, fy, unicode, width, height, xoff, yoff);
        int gid = impl_->code_to_gid[code];
        if (gid >= 0) return gfx::tt::GetGlyphBitmap(&impl_->tt_info, fx, fy, gid, width, height, xoff, yoff);
        return nullptr;
    }
    if (impl_->engine == Engine::kCff) {
        int gid = impl_->code_to_gid[code];
        if (gid < 0) return nullptr;
        return gfx::cff::GetGlyphBitmap(&impl_->cff_info, fx, fy, gid, width, height, xoff, yoff);
    }
    return nullptr;
}

void PdfFont::FreeGlyphBitmap(unsigned char *bitmap) const {
    if (!bitmap) return;
    if (impl_->engine == Engine::kTrueType) {
        gfx::tt::FreeBitmap(bitmap);
    } else if (impl_->engine == Engine::kCff) {
        gfx::cff::FreeBitmap(bitmap);
    }
}

}  // namespace pdffont
