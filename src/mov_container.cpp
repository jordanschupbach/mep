#include "mov_container.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

bool IsMovPath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "mov";
}

namespace mov {

namespace {

void AppendU8(std::string *out, uint8_t v) { out->push_back(static_cast<char>(v)); }
void AppendU16BE(std::string *out, uint16_t v) {
    out->push_back(static_cast<char>((v >> 8) & 0xFF));
    out->push_back(static_cast<char>(v & 0xFF));
}
void AppendU32BE(std::string *out, uint32_t v) {
    out->push_back(static_cast<char>((v >> 24) & 0xFF));
    out->push_back(static_cast<char>((v >> 16) & 0xFF));
    out->push_back(static_cast<char>((v >> 8) & 0xFF));
    out->push_back(static_cast<char>(v & 0xFF));
}
void AppendFourCC(std::string *out, const char fourcc[4]) { out->append(fourcc, 4); }
void AppendZeros(std::string *out, size_t n) { out->append(n, '\0'); }

// Wraps `payload` in a standard ISO-base-media-family box: a 4-byte
// big-endian size (payload + this 8-byte header) followed by the 4-byte
// type code. Every box in this file (ftyp/mdat/moov and everything
// nested inside moov) is built by nesting calls to this bottom-up --
// build each child's full bytes first, concatenate into the parent's
// payload, then wrap that.
std::string MakeBox(const char fourcc[4], const std::string &payload) {
    std::string box;
    AppendU32BE(&box, static_cast<uint32_t>(payload.size() + 8));
    AppendFourCC(&box, fourcc);
    box += payload;
    return box;
}

uint16_t ReadU16BE(const unsigned char *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t ReadU32BE(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// One immediate child box within a byte range: [payload_start,
// payload_start+payload_size) in whatever buffer it was parsed from,
// plus its own type code -- everything ParseBoxes/FindBox need to walk
// or recurse into the box tree without caring what's actually inside a
// box neither of them looks at.
struct RawBox {
    std::string fourcc;
    size_t payload_start = 0;
    size_t payload_size = 0;
};

// Walks the flat sequence of sibling boxes in `data[start,end)`. Stops
// (returning whatever was parsed so far) at the first malformed/
// truncated box rather than throwing -- callers that need a specific box
// present just check FindBox's result for null.
std::vector<RawBox> ParseBoxes(const unsigned char *data, size_t start, size_t end) {
    std::vector<RawBox> boxes;
    size_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = ReadU32BE(data + pos);
        if (size < 8 || pos + size > end) break;
        RawBox b;
        b.fourcc.assign(reinterpret_cast<const char *>(data + pos + 4), 4);
        b.payload_start = pos + 8;
        b.payload_size = size - 8;
        boxes.push_back(std::move(b));
        pos += size;
    }
    return boxes;
}

const RawBox *FindBox(const std::vector<RawBox> &boxes, const char *fourcc) {
    for (const RawBox &b : boxes) {
        if (b.fourcc == fourcc) return &b;
    }
    return nullptr;
}

// Standard unity 3x3 transform matrix (9 values, alternating 16.16 and
// 2.30 fixed-point per the spec) shared by mvhd and tkhd -- both use
// exactly this "no transform" identity.
void AppendIdentityMatrix(std::string *out) {
    AppendU32BE(out, 0x00010000);
    AppendU32BE(out, 0);
    AppendU32BE(out, 0);
    AppendU32BE(out, 0);
    AppendU32BE(out, 0x00010000);
    AppendU32BE(out, 0);
    AppendU32BE(out, 0);
    AppendU32BE(out, 0);
    AppendU32BE(out, 0x40000000);
}

}  // namespace

bool WriteMovFile(const std::string &path, int width, int height, int fps_num, int fps_den,
                   const std::vector<std::string> &jpeg_frames, std::string *out_error) {
    auto fail = [&](const char *msg) {
        if (out_error) *out_error = msg;
        return false;
    };
    if (jpeg_frames.empty()) return fail("mov: no frames to write");
    if (width <= 0 || height <= 0 || fps_num <= 0 || fps_den <= 0) return fail("mov: invalid dimensions/frame rate");
    uint32_t num_frames = static_cast<uint32_t>(jpeg_frames.size());

    std::string ftyp_payload;
    AppendFourCC(&ftyp_payload, "qt  ");  // major_brand
    AppendU32BE(&ftyp_payload, 0);        // minor_version
    AppendFourCC(&ftyp_payload, "qt  ");  // compatible_brands[0]
    std::string ftyp = MakeBox("ftyp", ftyp_payload);

    // Frame offsets are absolute file byte positions, so they can only be
    // computed once we know exactly how big everything written before
    // `mdat`'s payload is -- ftyp, then mdat's own 8-byte header.
    uint64_t cursor = ftyp.size() + 8;
    std::vector<uint64_t> offsets(num_frames);
    std::string mdat_payload;
    for (uint32_t i = 0; i < num_frames; i++) {
        offsets[i] = cursor;
        cursor += jpeg_frames[i].size();
        mdat_payload += jpeg_frames[i];
    }
    std::string mdat = MakeBox("mdat", mdat_payload);

    uint32_t duration = num_frames * static_cast<uint32_t>(fps_den);

    std::string mvhd_payload;
    AppendU32BE(&mvhd_payload, 0);                             // version(0)+flags
    AppendU32BE(&mvhd_payload, 0);                              // creation_time
    AppendU32BE(&mvhd_payload, 0);                              // modification_time
    AppendU32BE(&mvhd_payload, static_cast<uint32_t>(fps_num)); // timescale
    AppendU32BE(&mvhd_payload, duration);
    AppendU32BE(&mvhd_payload, 0x00010000);  // rate 1.0
    AppendU16BE(&mvhd_payload, 0x0100);      // volume 1.0
    AppendU16BE(&mvhd_payload, 0);           // reserved
    AppendZeros(&mvhd_payload, 8);           // reserved[2]
    AppendIdentityMatrix(&mvhd_payload);
    AppendZeros(&mvhd_payload, 24);          // pre_defined[6]
    AppendU32BE(&mvhd_payload, 2);           // next_track_ID
    std::string mvhd = MakeBox("mvhd", mvhd_payload);

    std::string tkhd_payload;
    AppendU8(&tkhd_payload, 0);
    tkhd_payload.push_back(0);
    tkhd_payload.push_back(0);
    tkhd_payload.push_back(0x07);  // flags: enabled | in_movie | in_preview
    AppendU32BE(&tkhd_payload, 0);  // creation_time
    AppendU32BE(&tkhd_payload, 0);  // modification_time
    AppendU32BE(&tkhd_payload, 1);  // track_ID
    AppendU32BE(&tkhd_payload, 0);  // reserved
    AppendU32BE(&tkhd_payload, duration);
    AppendZeros(&tkhd_payload, 8);  // reserved[2]
    AppendU16BE(&tkhd_payload, 0);  // layer
    AppendU16BE(&tkhd_payload, 0);  // alternate_group
    AppendU16BE(&tkhd_payload, 0);  // volume (0 for a video track)
    AppendU16BE(&tkhd_payload, 0);  // reserved
    AppendIdentityMatrix(&tkhd_payload);
    AppendU32BE(&tkhd_payload, static_cast<uint32_t>(width) << 16);
    AppendU32BE(&tkhd_payload, static_cast<uint32_t>(height) << 16);
    std::string tkhd = MakeBox("tkhd", tkhd_payload);

    std::string mdhd_payload;
    AppendU32BE(&mdhd_payload, 0);  // version+flags
    AppendU32BE(&mdhd_payload, 0);  // creation_time
    AppendU32BE(&mdhd_payload, 0);  // modification_time
    AppendU32BE(&mdhd_payload, static_cast<uint32_t>(fps_num));  // timescale
    AppendU32BE(&mdhd_payload, duration);
    AppendU16BE(&mdhd_payload, 0x55C4);  // language: packed ISO-639-2 "und" (undetermined)
    AppendU16BE(&mdhd_payload, 0);       // pre_defined
    std::string mdhd = MakeBox("mdhd", mdhd_payload);

    std::string hdlr_payload;
    AppendU32BE(&hdlr_payload, 0);  // version+flags
    AppendU32BE(&hdlr_payload, 0);  // pre_defined
    AppendFourCC(&hdlr_payload, "vide");
    AppendZeros(&hdlr_payload, 12);  // reserved[3]
    hdlr_payload += "mep video handler";
    hdlr_payload.push_back('\0');
    std::string hdlr = MakeBox("hdlr", hdlr_payload);

    std::string vmhd_payload;
    AppendU32BE(&vmhd_payload, 1);  // version(0)+flags(1) -- flags=1 is required for vmhd
    AppendU16BE(&vmhd_payload, 0);  // graphicsmode
    AppendZeros(&vmhd_payload, 6);  // opcolor[3]
    std::string vmhd = MakeBox("vmhd", vmhd_payload);

    std::string url_payload;
    AppendU32BE(&url_payload, 1);  // version(0)+flags(1) -- "media data is in this same file"
    std::string url_box = MakeBox("url ", url_payload);
    std::string dref_payload;
    AppendU32BE(&dref_payload, 0);  // version+flags
    AppendU32BE(&dref_payload, 1);  // entry_count
    dref_payload += url_box;
    std::string dref = MakeBox("dref", dref_payload);
    std::string dinf = MakeBox("dinf", dref);

    std::string sample_entry_payload;
    AppendZeros(&sample_entry_payload, 6);  // reserved
    AppendU16BE(&sample_entry_payload, 1);  // data_reference_index
    AppendU16BE(&sample_entry_payload, 0);  // pre_defined
    AppendU16BE(&sample_entry_payload, 0);  // reserved
    AppendZeros(&sample_entry_payload, 12);  // pre_defined[3]
    AppendU16BE(&sample_entry_payload, static_cast<uint16_t>(width));
    AppendU16BE(&sample_entry_payload, static_cast<uint16_t>(height));
    AppendU32BE(&sample_entry_payload, 0x00480000);  // horizresolution 72dpi
    AppendU32BE(&sample_entry_payload, 0x00480000);  // vertresolution 72dpi
    AppendU32BE(&sample_entry_payload, 0);           // reserved
    AppendU16BE(&sample_entry_payload, 1);           // frame_count
    AppendZeros(&sample_entry_payload, 32);          // compressorname (empty pascal string)
    AppendU16BE(&sample_entry_payload, 0x0018);      // depth: 24-bit color
    AppendU16BE(&sample_entry_payload, 0xFFFF);      // pre_defined
    std::string sample_entry = MakeBox("jpeg", sample_entry_payload);
    std::string stsd_payload;
    AppendU32BE(&stsd_payload, 0);  // version+flags
    AppendU32BE(&stsd_payload, 1);  // entry_count
    stsd_payload += sample_entry;
    std::string stsd = MakeBox("stsd", stsd_payload);

    std::string stts_payload;
    AppendU32BE(&stts_payload, 0);  // version+flags
    AppendU32BE(&stts_payload, 1);  // entry_count
    AppendU32BE(&stts_payload, num_frames);
    AppendU32BE(&stts_payload, static_cast<uint32_t>(fps_den));  // constant sample duration
    std::string stts = MakeBox("stts", stts_payload);

    std::string stsc_payload;
    AppendU32BE(&stsc_payload, 0);  // version+flags
    AppendU32BE(&stsc_payload, 1);  // entry_count
    AppendU32BE(&stsc_payload, 1);  // first_chunk
    AppendU32BE(&stsc_payload, 1);  // samples_per_chunk -- one sample per chunk throughout
    AppendU32BE(&stsc_payload, 1);  // sample_description_index
    std::string stsc = MakeBox("stsc", stsc_payload);

    std::string stsz_payload;
    AppendU32BE(&stsz_payload, 0);  // version+flags
    AppendU32BE(&stsz_payload, 0);  // sample_size=0 -- sizes vary, table follows
    AppendU32BE(&stsz_payload, num_frames);
    for (const std::string &frame : jpeg_frames) AppendU32BE(&stsz_payload, static_cast<uint32_t>(frame.size()));
    std::string stsz = MakeBox("stsz", stsz_payload);

    std::string stco_payload;
    AppendU32BE(&stco_payload, 0);  // version+flags
    AppendU32BE(&stco_payload, num_frames);
    for (uint64_t off : offsets) AppendU32BE(&stco_payload, static_cast<uint32_t>(off));
    std::string stco = MakeBox("stco", stco_payload);

    std::string stbl = MakeBox("stbl", stsd + stts + stsc + stsz + stco);
    std::string minf = MakeBox("minf", vmhd + dinf + stbl);
    std::string mdia = MakeBox("mdia", mdhd + hdlr + minf);
    std::string trak = MakeBox("trak", tkhd + mdia);
    std::string moov = MakeBox("moov", mvhd + trak);

    std::ofstream out(path, std::ios::binary);
    if (!out) return fail("mov: can't open output file");
    out.write(ftyp.data(), static_cast<std::streamsize>(ftyp.size()));
    out.write(mdat.data(), static_cast<std::streamsize>(mdat.size()));
    out.write(moov.data(), static_cast<std::streamsize>(moov.size()));
    if (!out) return fail("mov: write failed");
    return true;
}

bool OpenMovFile(const std::string &path, MovFile *out, std::string *out_error) {
    auto fail = [&](const char *msg) {
        if (out_error) *out_error = msg;
        return false;
    };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("mov: can't open file");
    f.seekg(0, std::ios::end);
    std::streamoff file_size_off = f.tellg();
    if (file_size_off < 8) return fail("mov: file too small");
    uint64_t file_size = static_cast<uint64_t>(file_size_off);

    // Walk top-level boxes via seeks, reading only `moov`'s bytes into
    // memory -- `mdat` (the bulk of the file) is never loaded here.
    std::vector<unsigned char> moov_bytes;
    uint64_t pos = 0;
    while (pos + 8 <= file_size) {
        f.seekg(static_cast<std::streamoff>(pos));
        unsigned char hdr[8];
        f.read(reinterpret_cast<char *>(hdr), 8);
        if (!f) return fail("mov: truncated box header");
        uint32_t size = ReadU32BE(hdr);
        std::string fourcc(reinterpret_cast<char *>(hdr + 4), 4);
        if (size < 8 || pos + size > file_size) return fail("mov: malformed box");
        if (fourcc == "moov") {
            moov_bytes.resize(size - 8);
            f.seekg(static_cast<std::streamoff>(pos + 8));
            f.read(reinterpret_cast<char *>(moov_bytes.data()), static_cast<std::streamsize>(moov_bytes.size()));
            if (!f) return fail("mov: truncated moov box");
        }
        pos += size;
    }
    if (moov_bytes.empty()) return fail("mov: no moov box found");

    const unsigned char *m = moov_bytes.data();
    size_t msize = moov_bytes.size();
    auto moov_children = ParseBoxes(m, 0, msize);
    const RawBox *trak = FindBox(moov_children, "trak");
    if (!trak) return fail("mov: no trak box found");
    auto trak_children = ParseBoxes(m, trak->payload_start, trak->payload_start + trak->payload_size);
    const RawBox *mdia = FindBox(trak_children, "mdia");
    if (!mdia) return fail("mov: no mdia box found");
    auto mdia_children = ParseBoxes(m, mdia->payload_start, mdia->payload_start + mdia->payload_size);
    const RawBox *mdhd = FindBox(mdia_children, "mdhd");
    const RawBox *minf = FindBox(mdia_children, "minf");
    if (!mdhd || mdhd->payload_size < 20 || !minf) return fail("mov: malformed mdia box");
    uint32_t timescale = ReadU32BE(m + mdhd->payload_start + 12);

    auto minf_children = ParseBoxes(m, minf->payload_start, minf->payload_start + minf->payload_size);
    const RawBox *stbl = FindBox(minf_children, "stbl");
    if (!stbl) return fail("mov: no stbl box found");
    auto stbl_children = ParseBoxes(m, stbl->payload_start, stbl->payload_start + stbl->payload_size);
    const RawBox *stsd = FindBox(stbl_children, "stsd");
    const RawBox *stts = FindBox(stbl_children, "stts");
    const RawBox *stsz = FindBox(stbl_children, "stsz");
    const RawBox *stco = FindBox(stbl_children, "stco");
    if (!stsd || !stts || !stsz || !stco) return fail("mov: missing sample table box");

    // stsd payload: version+flags(4) + entry_count(4) + one nested
    // VisualSampleEntry box, whose payload starts 6+2+2+2+12=24 bytes in
    // (reserved/data_reference_index/pre_defined/reserved/pre_defined[3])
    // and is followed immediately by width(2)/height(2).
    if (stsd->payload_size < 8) return fail("mov: malformed stsd box");
    auto stsd_entries = ParseBoxes(m, stsd->payload_start + 8, stsd->payload_start + stsd->payload_size);
    if (stsd_entries.empty() || stsd_entries[0].payload_size < 28) return fail("mov: malformed stsd sample entry");
    const RawBox &entry = stsd_entries[0];
    uint16_t width = ReadU16BE(m + entry.payload_start + 24);
    uint16_t height = ReadU16BE(m + entry.payload_start + 26);

    if (stts->payload_size < 16) return fail("mov: malformed stts box");
    uint32_t sample_delta = ReadU32BE(m + stts->payload_start + 12);
    if (sample_delta == 0 || timescale == 0) return fail("mov: invalid frame rate");
    double fps = static_cast<double>(timescale) / static_cast<double>(sample_delta);

    if (stsz->payload_size < 12) return fail("mov: malformed stsz box");
    uint32_t sample_count = ReadU32BE(m + stsz->payload_start + 8);
    if (stsz->payload_size < 12 + static_cast<size_t>(sample_count) * 4) return fail("mov: truncated stsz table");
    std::vector<uint32_t> sizes(sample_count);
    for (uint32_t i = 0; i < sample_count; i++) sizes[i] = ReadU32BE(m + stsz->payload_start + 12 + static_cast<size_t>(i) * 4);

    if (stco->payload_size < 8) return fail("mov: malformed stco box");
    uint32_t chunk_count = ReadU32BE(m + stco->payload_start + 4);
    if (stco->payload_size < 8 + static_cast<size_t>(chunk_count) * 4) return fail("mov: truncated stco table");
    // WriteMovFile always emits exactly one sample per chunk, so
    // sample_count and chunk_count line up index-for-index -- a real
    // general demuxer would walk stsc to map samples to chunks, but that
    // generality is explicitly out of scope (see mov_container.h).
    if (chunk_count != sample_count) return fail("mov: unsupported sample-to-chunk layout (not 1 sample/chunk)");
    std::vector<uint64_t> offsets(chunk_count);
    for (uint32_t i = 0; i < chunk_count; i++) offsets[i] = ReadU32BE(m + stco->payload_start + 8 + static_cast<size_t>(i) * 4);

    out->width = width;
    out->height = height;
    out->fps = fps;
    out->frame_index.resize(sample_count);
    for (uint32_t i = 0; i < sample_count; i++) out->frame_index[i] = {offsets[i], sizes[i]};
    return true;
}

std::vector<unsigned char> ReadMovFrameJpeg(const std::string &path, const MovFile &mov, int frame_index) {
    if (frame_index < 0 || frame_index >= static_cast<int>(mov.frame_index.size())) return {};
    auto [offset, size] = mov.frame_index[static_cast<size_t>(frame_index)];
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(static_cast<std::streamoff>(offset));
    std::vector<unsigned char> buf(size);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
    if (!f) return {};
    return buf;
}

}  // namespace mov
