#include "pdf_filters.h"

#include "deflate.h"
#include "jpeg_codec.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace pdffilter {

namespace {

int PaethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// PNG-style predictors (10-15 all select this reversal -- the actual
// per-row filter type, 0-4, is carried in each row's own leading byte,
// same convention png_codec.cpp's Unfilter uses for real PNG IDAT data).
bool UnfilterPng(const std::string &inflated, int columns, int colors, int bpc, std::string *out) {
    int bpp = std::max(1, (colors * bpc) / 8);
    size_t row_bits = static_cast<size_t>(columns) * static_cast<size_t>(colors) * static_cast<size_t>(bpc);
    size_t row_bytes = (row_bits + 7) / 8;
    if (row_bytes == 0) return false;
    size_t stride = row_bytes + 1;
    if (inflated.size() % stride != 0) return false;
    size_t rows = inflated.size() / stride;

    out->assign(row_bytes * rows, '\0');
    const unsigned char *src = reinterpret_cast<const unsigned char *>(inflated.data());
    std::string prior(row_bytes, '\0');
    for (size_t y = 0; y < rows; ++y) {
        unsigned char filter_type = src[0];
        const unsigned char *filt = src + 1;
        unsigned char *row_out = reinterpret_cast<unsigned char *>(&(*out)[y * row_bytes]);
        const unsigned char *prior_bytes = reinterpret_cast<const unsigned char *>(prior.data());
        for (size_t x = 0; x < row_bytes; ++x) {
            int a = x >= static_cast<size_t>(bpp) ? row_out[x - static_cast<size_t>(bpp)] : 0;
            int b = prior_bytes[x];
            int c = x >= static_cast<size_t>(bpp) ? prior_bytes[x - static_cast<size_t>(bpp)] : 0;
            int value;
            switch (filter_type) {
                case 0:
                    value = filt[x];
                    break;
                case 1:
                    value = filt[x] + a;
                    break;
                case 2:
                    value = filt[x] + b;
                    break;
                case 3:
                    value = filt[x] + (a + b) / 2;
                    break;
                case 4:
                    value = filt[x] + PaethPredictor(a, b, c);
                    break;
                default:
                    return false;  // unrecognized filter type: caller falls back to no-predictor output
            }
            row_out[x] = static_cast<unsigned char>(value & 0xFF);
        }
        prior.assign(reinterpret_cast<const char *>(row_out), row_bytes);
        src += stride;
    }
    return true;
}

// TIFF predictor 2: horizontal differencing per component, no per-row
// tag byte -- each sample (bpc-wide) is the previous same-component
// sample's value plus a delta. Only 8-bit-per-component is handled
// (the overwhelming common case for PDF image/xref-stream data); other
// bit depths are tolerated as a no-op passthrough rather than a hard
// failure.
bool UnfilterTiff(const std::string &inflated, int columns, int colors, int bpc, std::string *out) {
    if (bpc != 8) {
        *out = inflated;
        return true;
    }
    int bpp = std::max(1, colors);
    size_t row_bytes = static_cast<size_t>(columns) * static_cast<size_t>(colors);
    if (row_bytes == 0 || inflated.size() % row_bytes != 0) {
        *out = inflated;
        return true;
    }
    size_t rows = inflated.size() / row_bytes;
    out->assign(inflated.size(), '\0');
    for (size_t y = 0; y < rows; ++y) {
        const unsigned char *src = reinterpret_cast<const unsigned char *>(inflated.data()) + y * row_bytes;
        unsigned char *dst = reinterpret_cast<unsigned char *>(&(*out)[y * row_bytes]);
        for (size_t x = 0; x < row_bytes; ++x) {
            int left = x >= static_cast<size_t>(bpp) ? dst[x - static_cast<size_t>(bpp)] : 0;
            dst[x] = static_cast<unsigned char>((src[x] + left) & 0xFF);
        }
    }
    return true;
}

// Shared by FlateDecode and LZWDecode -- both can carry a `/Predictor`
// in decode_parms (spec Table 8). Always succeeds in the sense of
// leaving *inout usable: an unrecognized predictor value or
// inconsistent row-length math is tolerated by leaving *inout as the
// plain (unpredicted) bytes, matching this codebase's "skip bad
// content" convention.
void ApplyPredictorIfAny(const pdfobj::Object *decode_parms, std::string *inout) {
    long long predictor = 1;
    if (decode_parms) {
        if (const pdfobj::Object *p = decode_parms->Find("Predictor")) predictor = p->AsInt(1);
    }
    if (predictor <= 1) return;

    int columns = 1, colors = 1, bpc = 8;
    if (const pdfobj::Object *c = decode_parms->Find("Columns")) columns = static_cast<int>(c->AsInt(1));
    if (const pdfobj::Object *c = decode_parms->Find("Colors")) colors = static_cast<int>(c->AsInt(1));
    if (const pdfobj::Object *c = decode_parms->Find("BitsPerComponent")) bpc = static_cast<int>(c->AsInt(8));

    std::string unfiltered;
    bool ok = predictor == 2 ? UnfilterTiff(*inout, columns, colors, bpc, &unfiltered)
                              : UnfilterPng(*inout, columns, colors, bpc, &unfiltered);
    if (ok) *inout = std::move(unfiltered);
}

// PDF's own LZW variant (spec 7.4.4.2): MSB-first variable-width codes,
// 9 bits growing to 12 as the dictionary fills, code 256 = Clear table,
// code 257 = EOD. `early_change` (default true) makes the code width
// grow one code earlier than the dictionary size would strictly demand
// -- the de facto standard behavior inherited from TIFF's original (and
// famously off-by-one) LZW implementation, which is why PDF's own
// default for this parameter is 1/true.
class LzwBitReader {
public:
    LzwBitReader(const unsigned char *data, size_t len) : data_(data), len_(len) {}
    // Returns -1 at end of input.
    int ReadCode(int width) {
        int value = 0;
        for (int i = 0; i < width; ++i) {
            size_t byte_index = bit_pos_ / 8;
            if (byte_index >= len_) return -1;
            int bit = (data_[byte_index] >> (7 - (bit_pos_ % 8))) & 1;
            value = (value << 1) | bit;
            ++bit_pos_;
        }
        return value;
    }

private:
    const unsigned char *data_;
    size_t len_;
    size_t bit_pos_ = 0;
};

bool LzwDecodeCore(const std::string &raw, bool early_change, std::string *out) {
    out->clear();
    LzwBitReader reader(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
    std::vector<std::string> table;
    auto reset_table = [&]() {
        table.clear();
        table.reserve(4096);
        for (int i = 0; i < 256; ++i) table.emplace_back(1, static_cast<char>(i));
        table.emplace_back();  // 256: Clear (placeholder, never indexed as data)
        table.emplace_back();  // 257: EOD (placeholder)
    };
    reset_table();

    int code_width = 9;
    auto bump_threshold = [&](int width) { return early_change ? (1 << width) - 1 : (1 << width); };

    std::string prev;
    while (true) {
        int code = reader.ReadCode(code_width);
        if (code < 0 || code == 257) break;  // EOD or ran out of input
        if (code == 256) {
            reset_table();
            code_width = 9;
            prev.clear();
            continue;
        }
        std::string entry;
        if (code < static_cast<int>(table.size()) && (code < 256 || code >= 258)) {
            entry = table[static_cast<size_t>(code)];
        } else if (code == static_cast<int>(table.size()) && !prev.empty()) {
            entry = prev + prev[0];  // the classic "KwKwK" special case
        } else {
            return false;  // corrupt stream: code references a not-yet-defined entry
        }
        out->append(entry);
        if (!prev.empty()) table.push_back(prev + entry[0]);
        prev = entry;
        if (table.size() >= 4096) continue;  // spec requires a Clear before code 4095 overflows; tolerate one that doesn't
        if (static_cast<int>(table.size()) == bump_threshold(code_width) && code_width < 12) ++code_width;
    }
    return true;
}

}  // namespace

bool FlateDecode(const std::string &raw, const pdfobj::Object *decode_parms, std::string *out) {
    std::string inflated;
    if (!deflate::InflateZlib(reinterpret_cast<const unsigned char *>(raw.data()), raw.size(), inflated)) {
        return false;
    }
    ApplyPredictorIfAny(decode_parms, &inflated);
    *out = std::move(inflated);
    return true;
}

bool ASCIIHexDecode(const std::string &raw, std::string *out) {
    out->clear();
    int hi = -1;
    for (char raw_c : raw) {
        unsigned char c = static_cast<unsigned char>(raw_c);
        if (c == '>') break;
        int v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            continue;  // whitespace/garbage: skip, matches pdf_object.cpp's own hex-string tolerance
        if (hi < 0) {
            hi = v;
        } else {
            out->push_back(static_cast<char>((hi << 4) | v));
            hi = -1;
        }
    }
    if (hi >= 0) out->push_back(static_cast<char>(hi << 4));
    return true;
}

bool ASCII85Decode(const std::string &raw, std::string *out) {
    out->clear();
    unsigned char group[5];
    int group_len = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '~') break;  // "~>" terminator
        if (c == 'z' && group_len == 0) {
            out->append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') continue;  // whitespace/garbage: skip
        group[group_len++] = static_cast<unsigned char>(c - '!');
        if (group_len == 5) {
            unsigned long value = 0;
            for (int i2 = 0; i2 < 5; ++i2) value = value * 85 + group[i2];
            out->push_back(static_cast<char>((value >> 24) & 0xFF));
            out->push_back(static_cast<char>((value >> 16) & 0xFF));
            out->push_back(static_cast<char>((value >> 8) & 0xFF));
            out->push_back(static_cast<char>(value & 0xFF));
            group_len = 0;
        }
    }
    if (group_len > 0) {
        int missing = 5 - group_len;
        for (int i2 = group_len; i2 < 5; ++i2) group[i2] = 84;  // pad with the highest-value digit, per spec
        unsigned long value = 0;
        for (int i2 = 0; i2 < 5; ++i2) value = value * 85 + group[i2];
        unsigned char bytes[4] = {static_cast<unsigned char>((value >> 24) & 0xFF),
                                   static_cast<unsigned char>((value >> 16) & 0xFF),
                                   static_cast<unsigned char>((value >> 8) & 0xFF),
                                   static_cast<unsigned char>(value & 0xFF)};
        out->append(reinterpret_cast<char *>(bytes), static_cast<size_t>(4 - missing));
    }
    return true;
}

bool RunLengthDecode(const std::string &raw, std::string *out) {
    out->clear();
    size_t pos = 0;
    while (pos < raw.size()) {
        unsigned char length = static_cast<unsigned char>(raw[pos]);
        ++pos;
        if (length == 128) break;  // EOD
        if (length < 128) {
            size_t count = static_cast<size_t>(length) + 1;
            size_t avail = std::min(count, raw.size() - pos);
            out->append(raw, pos, avail);
            pos += avail;
        } else {
            if (pos >= raw.size()) break;
            size_t count = 257 - static_cast<size_t>(length);
            out->append(count, raw[pos]);
            ++pos;
        }
    }
    return true;
}

bool LZWDecode(const std::string &raw, const pdfobj::Object *decode_parms, std::string *out) {
    bool early_change = true;
    if (decode_parms) {
        if (const pdfobj::Object *ec = decode_parms->Find("EarlyChange")) early_change = ec->AsInt(1) != 0;
    }
    std::string decoded;
    if (!LzwDecodeCore(raw, early_change, &decoded)) return false;
    ApplyPredictorIfAny(decode_parms, &decoded);
    *out = std::move(decoded);
    return true;
}

bool DCTDecode(const std::string &raw, std::string *out_rgba, int *out_width, int *out_height) {
    std::string error;
    unsigned char *rgba = jpeg::Decode(reinterpret_cast<const unsigned char *>(raw.data()), raw.size(), out_width,
                                        out_height, &error);
    if (!rgba) return false;
    out_rgba->assign(reinterpret_cast<char *>(rgba),
                      static_cast<size_t>(*out_width) * static_cast<size_t>(*out_height) * 4);
    std::free(rgba);
    return true;
}

bool DecodeStream(const std::string &raw, const pdfobj::Object *stream_dict, std::string *out) {
    *out = raw;
    if (!stream_dict) return true;
    const pdfobj::Object *filter = stream_dict->Find("Filter");
    if (!filter) return true;

    std::vector<std::string> names;
    if (filter->IsName()) {
        names.push_back(filter->str_val);
    } else if (filter->IsArray()) {
        for (const auto &f : filter->array_val) names.push_back(f.AsString(""));
    }

    const pdfobj::Object *parms = stream_dict->Find("DecodeParms");
    auto parms_for = [&](size_t index) -> const pdfobj::Object * {
        if (!parms) return nullptr;
        if (parms->IsDict()) return index == 0 ? parms : nullptr;
        if (parms->IsArray() && index < parms->array_val.size()) {
            const pdfobj::Object &p = parms->array_val[index];
            return p.IsDict() ? &p : nullptr;
        }
        return nullptr;
    };

    for (size_t i = 0; i < names.size(); ++i) {
        const std::string &name = names[i];
        const pdfobj::Object *dp = parms_for(i);
        std::string stage_out;
        if (name == "FlateDecode" || name == "Fl") {
            if (!FlateDecode(*out, dp, &stage_out)) return false;
        } else if (name == "ASCIIHexDecode" || name == "AHx") {
            ASCIIHexDecode(*out, &stage_out);
        } else if (name == "ASCII85Decode" || name == "A85") {
            ASCII85Decode(*out, &stage_out);
        } else if (name == "RunLengthDecode" || name == "RL") {
            RunLengthDecode(*out, &stage_out);
        } else if (name == "LZWDecode" || name == "LZW") {
            if (!LZWDecode(*out, dp, &stage_out)) return false;
        } else if (name == "DCTDecode" || name == "DCT" || name == "CCITTFaxDecode" || name == "CCF" ||
                   name == "JBIG2Decode" || name == "JPXDecode") {
            return false;  // image-format filters: see this function's own doc comment
        } else {
            stage_out = *out;  // unrecognized filter: tolerate, pass through unchanged
        }
        *out = std::move(stage_out);
    }
    return true;
}

}  // namespace pdffilter
