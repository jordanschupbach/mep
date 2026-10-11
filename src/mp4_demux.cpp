#include "mp4_demux.h"

#include <algorithm>
#include <cstring>

namespace mp4 {

namespace {

constexpr uint32_t FourCc(const char (&s)[5]) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(s[3]));
}

std::string FourCcString(uint32_t v) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; i++) s[static_cast<size_t>(i)] = static_cast<char>((v >> (24 - 8 * i)) & 0xff);
    return s;
}

// A bounds-checked big-endian cursor over one box's bytes. Reading past
// the end sets `bad` and yields zeros, so a parser can read a whole
// structure and check once.
struct Cursor {
    const uint8_t *p = nullptr;
    size_t len = 0;
    size_t pos = 0;
    bool bad = false;

    Cursor(const uint8_t *data, size_t n) : p(data), len(n) {}
    size_t Left() const { return pos <= len ? len - pos : 0; }
    bool Has(size_t n) {
        if (n > Left()) bad = true;
        return !bad;
    }
    uint8_t U8() { return Has(1) ? p[pos++] : 0; }
    uint16_t U16() {
        if (!Has(2)) return 0;
        const uint16_t v = static_cast<uint16_t>((p[pos] << 8) | p[pos + 1]);
        pos += 2;
        return v;
    }
    uint32_t U24() {
        if (!Has(3)) return 0;
        const uint32_t v = (static_cast<uint32_t>(p[pos]) << 16) | (static_cast<uint32_t>(p[pos + 1]) << 8) | p[pos + 2];
        pos += 3;
        return v;
    }
    uint32_t U32() {
        if (!Has(4)) return 0;
        const uint32_t v = (static_cast<uint32_t>(p[pos]) << 24) | (static_cast<uint32_t>(p[pos + 1]) << 16) |
                           (static_cast<uint32_t>(p[pos + 2]) << 8) | p[pos + 3];
        pos += 4;
        return v;
    }
    uint64_t U64() {
        const uint64_t hi = U32();
        return (hi << 32) | U32();
    }
    void Skip(size_t n) {
        if (Has(n)) pos += n;
    }
};

// A child box found by Boxes(): its type and payload.
struct Box {
    uint32_t type = 0;
    const uint8_t *data = nullptr;
    size_t len = 0;
};

// The boxes laid end to end in `data` (a container's payload). Stops at
// the first malformed header.
std::vector<Box> Boxes(const uint8_t *data, size_t len) {
    std::vector<Box> out;
    size_t pos = 0;
    while (len - pos >= 8) {
        Cursor c(data + pos, len - pos);
        uint64_t size = c.U32();
        const uint32_t type = c.U32();
        size_t header = 8;
        if (size == 1) {
            size = c.U64();
            header = 16;
        } else if (size == 0) {
            size = len - pos;
        }
        if (c.bad || size < header || size > len - pos) break;
        out.push_back({type, data + pos + header, static_cast<size_t>(size) - header});
        pos += static_cast<size_t>(size);
    }
    return out;
}

const Box *Find(const std::vector<Box> &boxes, uint32_t type) {
    for (const Box &b : boxes)
        if (b.type == type) return &b;
    return nullptr;
}

// An MPEG-4 descriptor's length: up to four 7-bit groups.
uint32_t DescriptorLength(Cursor &c) {
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t b = c.U8();
        n = (n << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return n;
}

// esds: ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo,
// whose bytes are the AudioSpecificConfig.
bool ParseEsds(const uint8_t *data, size_t len, std::vector<uint8_t> *config) {
    Cursor c(data, len);
    c.Skip(4);  // version + flags
    if (c.U8() != 0x03) return false;
    DescriptorLength(c);
    c.Skip(2);  // ES_ID
    const uint8_t flags = c.U8();
    if (flags & 0x80) c.Skip(2);              // streamDependenceFlag
    if (flags & 0x40) c.Skip(c.U8());         // URL_Flag
    if (flags & 0x20) c.Skip(2);              // OCRstreamFlag
    if (c.U8() != 0x04) return false;
    DescriptorLength(c);
    c.Skip(13);  // objectTypeIndication, streamType, bufferSizeDB, maxBitrate, avgBitrate
    if (c.U8() != 0x05) return false;
    const uint32_t n = DescriptorLength(c);
    if (c.bad || !c.Has(n)) return false;
    config->assign(c.p + c.pos, c.p + c.pos + n);
    return true;
}

// stsd: the first sample entry, for the codec four-cc and its setup.
bool ParseStsd(const uint8_t *data, size_t len, Track *t) {
    Cursor c(data, len);
    c.Skip(4);
    if (c.U32() == 0) return false;
    const std::vector<Box> entries = Boxes(c.p + c.pos, c.Left());
    if (entries.empty()) return false;
    const Box &e = entries[0];
    t->codec = FourCcString(e.type);
    Cursor ec(e.data, e.len);
    if (t->kind == TrackKind::Video) {
        // VisualSampleEntry: 6 reserved + data_reference_index, 16 bytes of
        // pre_defined/reserved, width, height, then 50 more bytes before
        // the child boxes.
        ec.Skip(8 + 16);
        t->width = ec.U16();
        t->height = ec.U16();
        ec.Skip(50);
        if (ec.bad) return false;
        for (const Box &b : Boxes(ec.p + ec.pos, ec.Left()))
            if (b.type == FourCc("avcC")) t->config.assign(b.data, b.data + b.len);
    } else if (t->kind == TrackKind::Audio) {
        // AudioSampleEntry: 6 reserved + data_reference_index, version
        // (+ 6 reserved), channelcount, samplesize, 4 more, samplerate
        // 16.16. Version 1/2 entries (QuickTime) carry more before the
        // child boxes.
        ec.Skip(8);
        const uint16_t version = ec.U16();
        ec.Skip(6);
        t->channels = ec.U16();
        ec.Skip(6);
        t->sample_rate = static_cast<int>(ec.U32() >> 16);
        if (version == 1) ec.Skip(16);
        if (version == 2) ec.Skip(36);
        if (ec.bad) return false;
        for (const Box &b : Boxes(ec.p + ec.pos, ec.Left()))
            if (b.type == FourCc("esds")) ParseEsds(b.data, b.len, &t->config);
    }
    return true;
}

// The sample table (stbl) into Track::samples.
bool ParseStbl(const std::vector<Box> &stbl, Track *t, std::string *error) {
    auto fail = [&](const char *msg) {
        if (error) *error = msg;
        return false;
    };
    const Box *stsd = Find(stbl, FourCc("stsd"));
    if (!stsd || !ParseStsd(stsd->data, stsd->len, t)) return fail("mp4: no sample description");

    // Sizes.
    std::vector<uint32_t> sizes;
    if (const Box *stsz = Find(stbl, FourCc("stsz"))) {
        Cursor c(stsz->data, stsz->len);
        c.Skip(4);
        const uint32_t fixed = c.U32();
        const uint32_t count = c.U32();
        if (c.bad || (fixed == 0 && c.Left() / 4 < count)) return fail("mp4: bad stsz");
        sizes.resize(count, fixed);
        if (fixed == 0)
            for (uint32_t i = 0; i < count; i++) sizes[i] = c.U32();
    } else if (const Box *stz2 = Find(stbl, FourCc("stz2"))) {
        Cursor c(stz2->data, stz2->len);
        c.Skip(7);
        const uint8_t field = c.U8();
        const uint32_t count = c.U32();
        if (c.bad || (field != 4 && field != 8 && field != 16)) return fail("mp4: bad stz2");
        sizes.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            if (field == 16) sizes[i] = c.U16();
            else if (field == 8) sizes[i] = c.U8();
            else sizes[i] = (i % 2 == 0) ? static_cast<uint32_t>(c.p[c.pos] >> 4) : (c.p[c.pos++] & 0x0fu);
        }
        if (c.bad) return fail("mp4: bad stz2");
    }
    const size_t n = sizes.size();
    if (n == 0) return true;  // a fragmented file's empty table
    t->samples.assign(n, Sample{});
    for (size_t i = 0; i < n; i++) t->samples[i].size = sizes[i];

    // Chunk offsets, then stsc spreads the samples over the chunks.
    std::vector<uint64_t> chunks;
    if (const Box *stco = Find(stbl, FourCc("stco"))) {
        Cursor c(stco->data, stco->len);
        c.Skip(4);
        const uint32_t count = c.U32();
        if (c.bad || c.Left() / 4 < count) return fail("mp4: bad stco");
        chunks.resize(count);
        for (uint32_t i = 0; i < count; i++) chunks[i] = c.U32();
    } else if (const Box *co64 = Find(stbl, FourCc("co64"))) {
        Cursor c(co64->data, co64->len);
        c.Skip(4);
        const uint32_t count = c.U32();
        if (c.bad || c.Left() / 8 < count) return fail("mp4: bad co64");
        chunks.resize(count);
        for (uint32_t i = 0; i < count; i++) chunks[i] = c.U64();
    }
    const Box *stsc = Find(stbl, FourCc("stsc"));
    if (chunks.empty() || !stsc) return fail("mp4: no chunk table");
    {
        Cursor c(stsc->data, stsc->len);
        c.Skip(4);
        const uint32_t count = c.U32();
        if (c.bad || c.Left() / 12 < count || count == 0) return fail("mp4: bad stsc");
        struct Run {
            uint32_t first_chunk, per_chunk;
        };
        std::vector<Run> runs(count);
        for (uint32_t i = 0; i < count; i++) {
            runs[i].first_chunk = c.U32();
            runs[i].per_chunk = c.U32();
            c.Skip(4);  // sample_description_index
        }
        size_t s = 0;
        for (uint32_t r = 0; r < count && s < n; r++) {
            const uint32_t first = runs[r].first_chunk;
            const uint32_t last = r + 1 < count ? runs[r + 1].first_chunk : static_cast<uint32_t>(chunks.size()) + 1;
            for (uint32_t chunk = first; chunk < last && s < n; chunk++) {
                if (chunk == 0 || chunk > chunks.size()) return fail("mp4: stsc names a missing chunk");
                uint64_t off = chunks[chunk - 1];
                for (uint32_t k = 0; k < runs[r].per_chunk && s < n; k++, s++) {
                    t->samples[s].offset = off;
                    off += t->samples[s].size;
                }
            }
        }
        if (s < n) return fail("mp4: chunk table shorter than the sample table");
    }

    // Decode times.
    if (const Box *stts = Find(stbl, FourCc("stts"))) {
        Cursor c(stts->data, stts->len);
        c.Skip(4);
        const uint32_t count = c.U32();
        int64_t dts = 0;
        size_t s = 0;
        for (uint32_t i = 0; i < count && !c.bad; i++) {
            const uint32_t run = c.U32();
            const uint32_t delta = c.U32();
            for (uint32_t k = 0; k < run && s < n; k++, s++) {
                t->samples[s].dts = dts;
                t->samples[s].duration = delta;
                dts += delta;
            }
        }
        for (; s < n; s++) t->samples[s].dts = dts;
    }
    // Composition offsets (signed in version 1, and in practice in 0 too).
    if (const Box *ctts = Find(stbl, FourCc("ctts"))) {
        Cursor c(ctts->data, ctts->len);
        c.Skip(4);
        const uint32_t count = c.U32();
        size_t s = 0;
        for (uint32_t i = 0; i < count && !c.bad; i++) {
            const uint32_t run = c.U32();
            const int32_t off = static_cast<int32_t>(c.U32());
            for (uint32_t k = 0; k < run && s < n; k++, s++) t->samples[s].cto = off;
        }
    }
    // Sync samples: every sample when there is no stss.
    if (const Box *stss = Find(stbl, FourCc("stss"))) {
        Cursor c(stss->data, stss->len);
        c.Skip(4);
        const uint32_t count = c.U32();
        for (uint32_t i = 0; i < count && !c.bad; i++) {
            const uint32_t k = c.U32();
            if (k >= 1 && k <= n) t->samples[k - 1].sync = true;
        }
    } else {
        for (Sample &s : t->samples) s.sync = true;
    }
    return true;
}

bool ParseTrak(const uint8_t *data, size_t len, uint32_t movie_timescale, Track *t, std::string *error) {
    const std::vector<Box> trak = Boxes(data, len);
    if (const Box *tkhd = Find(trak, FourCc("tkhd"))) {
        Cursor c(tkhd->data, tkhd->len);
        const uint8_t version = c.U8();
        c.Skip(3);
        c.Skip(version == 1 ? 16 : 8);  // creation/modification time
        t->id = c.U32();
    }
    // Edit list: the first non-empty edit's media time; an empty first
    // edit delays the track.
    if (const Box *edts = Find(trak, FourCc("edts"))) {
        const std::vector<Box> edts_boxes = Boxes(edts->data, edts->len);
        if (const Box *elst = Find(edts_boxes, FourCc("elst"))) {
            Cursor c(elst->data, elst->len);
            const uint8_t version = c.U8();
            c.Skip(3);
            const uint32_t count = c.U32();
            for (uint32_t i = 0; i < count && !c.bad; i++) {
                const uint64_t seg_duration = version == 1 ? c.U64() : c.U32();
                const int64_t media_time = version == 1 ? static_cast<int64_t>(c.U64()) : static_cast<int32_t>(c.U32());
                c.Skip(4);  // media_rate
                if (media_time == -1) {
                    if (movie_timescale > 0) t->delay_sec += static_cast<double>(seg_duration) / movie_timescale;
                    continue;
                }
                t->media_time = media_time;
                break;
            }
        }
    }
    const Box *mdia = Find(trak, FourCc("mdia"));
    if (!mdia) return true;
    const std::vector<Box> m = Boxes(mdia->data, mdia->len);
    if (const Box *mdhd = Find(m, FourCc("mdhd"))) {
        Cursor c(mdhd->data, mdhd->len);
        const uint8_t version = c.U8();
        c.Skip(3);
        c.Skip(version == 1 ? 16 : 8);
        t->timescale = c.U32();
        t->duration = version == 1 ? c.U64() : c.U32();
    }
    if (const Box *hdlr = Find(m, FourCc("hdlr"))) {
        Cursor c(hdlr->data, hdlr->len);
        c.Skip(8);
        const uint32_t handler = c.U32();
        if (handler == FourCc("vide")) t->kind = TrackKind::Video;
        else if (handler == FourCc("soun")) t->kind = TrackKind::Audio;
    }
    if (t->kind == TrackKind::Other || t->timescale == 0) return true;
    const Box *minf = Find(m, FourCc("minf"));
    if (!minf) return true;
    const std::vector<Box> minf_boxes = Boxes(minf->data, minf->len);
    const Box *stbl = Find(minf_boxes, FourCc("stbl"));
    if (!stbl) return true;
    return ParseStbl(Boxes(stbl->data, stbl->len), t, error);
}

}  // namespace

double Track::Seconds(int64_t t) const {
    if (timescale == 0) return 0.0;
    return static_cast<double>(t - media_time) / timescale + delay_sec;
}

int Movie::VideoTrack() const {
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].kind == TrackKind::Video && (tracks[i].codec == "avc1" || tracks[i].codec == "avc3")) return static_cast<int>(i);
    return -1;
}

int Movie::AudioTrack() const {
    for (size_t i = 0; i < tracks.size(); i++)
        if (tracks[i].kind == TrackKind::Audio && tracks[i].codec == "mp4a") return static_cast<int>(i);
    return -1;
}

bool ParseMoov(const uint8_t *data, size_t len, Movie *out, std::string *error) {
    Movie movie;
    const std::vector<Box> moov = Boxes(data, len);
    uint32_t movie_timescale = 0;
    uint64_t movie_duration = 0;
    if (const Box *mvhd = Find(moov, FourCc("mvhd"))) {
        Cursor c(mvhd->data, mvhd->len);
        const uint8_t version = c.U8();
        c.Skip(3);
        c.Skip(version == 1 ? 16 : 8);
        movie_timescale = c.U32();
        movie_duration = version == 1 ? c.U64() : c.U32();
    }
    for (const Box &b : moov) {
        if (b.type != FourCc("trak")) continue;
        Track t;
        if (!ParseTrak(b.data, b.len, movie_timescale, &t, error)) return false;
        movie.tracks.push_back(std::move(t));
    }
    if (const Box *mvex = Find(moov, FourCc("mvex"))) {
        movie.fragmented = true;
        for (const Box &b : Boxes(mvex->data, mvex->len)) {
            if (b.type != FourCc("trex")) continue;
            Cursor c(b.data, b.len);
            c.Skip(4);
            const uint32_t id = c.U32();
            c.Skip(4);  // default_sample_description_index
            const uint32_t duration = c.U32(), size = c.U32(), flags = c.U32();
            for (Track &t : movie.tracks) {
                if (t.id != id) continue;
                t.default_duration = duration;
                t.default_size = size;
                t.default_flags = flags;
            }
        }
    }
    if (movie_timescale > 0) movie.duration_sec = static_cast<double>(movie_duration) / movie_timescale;
    for (const Track &t : movie.tracks)
        if (t.timescale > 0 && t.kind != TrackKind::Other)
            movie.duration_sec = std::max(movie.duration_sec, static_cast<double>(t.duration) / t.timescale);
    if (movie.VideoTrack() < 0 && movie.AudioTrack() < 0) {
        if (error) *error = "mp4: no H.264 video or AAC audio track";
        return false;
    }
    *out = std::move(movie);
    return true;
}

bool ParseSidx(const uint8_t *data, size_t len, uint64_t box_end, Movie *movie, std::string *error) {
    Cursor c(data, len);
    const uint8_t version = c.U8();
    c.Skip(3);
    c.Skip(4);  // reference_ID
    const uint32_t timescale = c.U32();
    const uint64_t earliest = version == 0 ? c.U32() : c.U64();
    const uint64_t first_offset = version == 0 ? c.U32() : c.U64();
    c.Skip(2);
    const uint16_t count = c.U16();
    if (c.bad || timescale == 0) {
        if (error) *error = "mp4: bad sidx";
        return false;
    }
    uint64_t offset = box_end + first_offset;
    uint64_t t = earliest;
    movie->segments.clear();
    for (uint16_t i = 0; i < count && !c.bad; i++) {
        const uint32_t ref = c.U32();
        const uint32_t duration = c.U32();
        c.Skip(4);  // SAP flags
        Segment s;
        s.offset = offset;
        s.size = ref & 0x7fffffffu;
        s.start_sec = static_cast<double>(t) / timescale;
        s.duration_sec = static_cast<double>(duration) / timescale;
        // (A reference to another sidx -- the top bit -- isn't followed:
        // YouTube's index is one level.)
        movie->segments.push_back(s);
        offset += s.size;
        t += duration;
    }
    return !c.bad;
}

bool ParseMoof(const uint8_t *data, size_t len, uint64_t moof_offset, const Movie &movie, std::vector<FragmentSample> *out,
               std::string *error) {
    auto fail = [&](const char *msg) {
        if (error) *error = msg;
        return false;
    };
    for (const Box &traf : Boxes(data, len)) {
        if (traf.type != FourCc("traf")) continue;
        const std::vector<Box> boxes = Boxes(traf.data, traf.len);
        const Box *tfhd = Find(boxes, FourCc("tfhd"));
        if (!tfhd) return fail("mp4: traf without tfhd");
        Cursor h(tfhd->data, tfhd->len);
        const uint32_t tf_flags = h.U32() & 0xffffffu;
        const uint32_t id = h.U32();
        int track = -1;
        for (size_t i = 0; i < movie.tracks.size(); i++)
            if (movie.tracks[i].id == id) track = static_cast<int>(i);
        if (track < 0) continue;
        const Track &t = movie.tracks[static_cast<size_t>(track)];
        uint64_t base = moof_offset;  // default-base-is-moof, and what YouTube's files mean without a base offset
        uint32_t def_duration = t.default_duration, def_size = t.default_size, def_flags = t.default_flags;
        if (tf_flags & 0x01) base = h.U64();
        if (tf_flags & 0x02) h.Skip(4);
        if (tf_flags & 0x08) def_duration = h.U32();
        if (tf_flags & 0x10) def_size = h.U32();
        if (tf_flags & 0x20) def_flags = h.U32();
        if (h.bad) return fail("mp4: bad tfhd");
        int64_t dts = 0;
        if (const Box *tfdt = Find(boxes, FourCc("tfdt"))) {
            Cursor c(tfdt->data, tfdt->len);
            const uint8_t version = c.U8();
            c.Skip(3);
            dts = static_cast<int64_t>(version == 1 ? c.U64() : c.U32());
        }
        uint64_t next_offset = base;
        for (const Box &b : boxes) {
            if (b.type != FourCc("trun")) continue;
            Cursor c(b.data, b.len);
            const uint8_t version = c.U8();
            const uint32_t flags = c.U24();
            const uint32_t count = c.U32();
            uint64_t offset = next_offset;
            if (flags & 0x01) offset = base + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(c.U32())));
            const bool has_first_flags = (flags & 0x04) != 0;
            const uint32_t first_flags = has_first_flags ? c.U32() : 0;
            for (uint32_t i = 0; i < count && !c.bad; i++) {
                Sample s;
                s.duration = (flags & 0x100) ? c.U32() : def_duration;
                s.size = (flags & 0x200) ? c.U32() : def_size;
                uint32_t sflags = (flags & 0x400) ? c.U32() : def_flags;
                if (i == 0 && has_first_flags) sflags = first_flags;
                if (flags & 0x800) {
                    const uint32_t raw = c.U32();
                    s.cto = version == 0 ? static_cast<int32_t>(raw) : static_cast<int32_t>(raw);
                }
                s.sync = (sflags & 0x10000u) == 0;  // sample_is_non_sync_sample
                s.offset = offset;
                s.dts = dts;
                offset += s.size;
                dts += s.duration;
                out->push_back({track, s});
            }
            if (c.bad) return fail("mp4: bad trun");
            next_offset = offset;
        }
    }
    std::stable_sort(out->begin(), out->end(),
                     [](const FragmentSample &a, const FragmentSample &b) { return a.sample.offset < b.sample.offset; });
    return true;
}

bool ByteSource::Skip(uint64_t n) {
    if (n > (1u << 20)) return SeekTo(Offset() + n);
    uint8_t buf[16384];
    while (n > 0) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(n, sizeof buf));
        if (!Read(buf, take)) return false;
        n -= take;
    }
    return true;
}

bool Reader::ReadBoxHeader(uint64_t *size, uint32_t *type, uint64_t *header_len) {
    uint8_t h[8];
    if (!src_->Read(h, 8)) return false;
    Cursor c(h, 8);
    *size = c.U32();
    *type = c.U32();
    *header_len = 8;
    if (*size == 1) {
        uint8_t big[8];
        if (!src_->Read(big, 8)) return false;
        Cursor b(big, 8);
        *size = b.U64();
        *header_len = 16;
    }
    return true;
}

bool Reader::Open(ByteSource *src, std::string *error) {
    src_ = src;
    movie_ = Movie{};
    order_.clear();
    cursor_ = 0;
    fragment_.clear();
    fragment_cursor_ = 0;
    bool have_moov = false;
    uint64_t first_mdat = 0;  // a progressive file whose moov comes last: where to come back to
    for (;;) {
        const uint64_t at = src_->Offset();
        uint64_t size = 0, header = 0;
        uint32_t type = 0;
        if (!ReadBoxHeader(&size, &type, &header)) {
            if (error) *error = have_moov ? "" : "mp4: no moov box";
            if (have_moov) break;
            return false;
        }
        if (size == 0 || size < header) {
            if (error) *error = "mp4: a box runs to the end of the file before the moov";
            return false;
        }
        const uint64_t payload = size - header;
        if (type == FourCc("moov") || (type == FourCc("sidx") && have_moov)) {
            if (payload > (256u << 20)) {
                if (error) *error = "mp4: header box too large";
                return false;
            }
            std::vector<uint8_t> buf(static_cast<size_t>(payload));
            if (!src_->Read(buf.data(), buf.size())) {
                if (error) *error = "mp4: the download ended inside the header";
                return false;
            }
            if (type == FourCc("moov")) {
                if (!ParseMoov(buf.data(), buf.size(), &movie_, error)) return false;
                have_moov = true;
                if (!movie_.fragmented) break;
            } else {
                if (!ParseSidx(buf.data(), buf.size(), at + size, &movie_, error)) return false;
                data_start_ = at + size;
                break;
            }
            continue;
        }
        if (type == FourCc("moof") && have_moov) {
            // A fragmented file without a sidx: the fragments start here.
            data_start_ = at;
            if (!src_->SeekTo(at)) return false;
            break;
        }
        if (type == FourCc("mdat") && !have_moov && first_mdat == 0) first_mdat = at;
        if (!src_->Skip(payload)) {
            if (error) *error = "mp4: the download ended before the moov";
            return false;
        }
    }
    if (!movie_.fragmented) {
        for (size_t ti = 0; ti < movie_.tracks.size(); ti++) {
            const int track = static_cast<int>(ti);
            if (track != movie_.VideoTrack() && track != movie_.AudioTrack()) continue;
            const std::vector<Sample> &s = movie_.tracks[ti].samples;
            for (size_t i = 0; i < s.size(); i++) order_.push_back({s[i].offset, track, static_cast<uint32_t>(i)});
        }
        std::stable_sort(order_.begin(), order_.end(), [](const Entry &a, const Entry &b) { return a.offset < b.offset; });
        // Back to the first sample (a trailing moov left the source past it).
        if (!order_.empty() && order_[0].offset < src_->Offset() && !src_->SeekTo(order_[0].offset)) {
            if (error) *error = "mp4: cannot return to the media data";
            return false;
        }
        (void)first_mdat;
    } else if (data_start_ == 0) {
        data_start_ = src_->Offset();
    }
    return true;
}

bool Reader::Seek(double sec, std::string *error) {
    if (movie_.fragmented) {
        fragment_.clear();
        fragment_cursor_ = 0;
        mdat_end_ = 0;
        uint64_t offset = data_start_;
        for (const Segment &s : movie_.segments)
            if (s.start_sec <= sec) offset = s.offset;
        if (!src_->SeekTo(offset)) {
            if (error) *error = "mp4: seek failed";
            return false;
        }
        return true;
    }
    // Progressive: the earliest byte of the video's sync sample at or
    // before `sec` and of the audio sample playing at `sec`.
    uint64_t offset = UINT64_MAX;
    const int video = movie_.VideoTrack(), audio = movie_.AudioTrack();
    if (video >= 0) {
        const Track &t = movie_.tracks[static_cast<size_t>(video)];
        uint64_t key = t.samples.empty() ? 0 : t.samples[0].offset;
        for (const Sample &s : t.samples)
            if (s.sync && t.Seconds(s.dts + s.cto) <= sec + 1e-6) key = s.offset;
        offset = std::min(offset, key);
    }
    if (audio >= 0) {
        const Track &t = movie_.tracks[static_cast<size_t>(audio)];
        uint64_t at = t.samples.empty() ? 0 : t.samples[0].offset;
        for (const Sample &s : t.samples) {
            if (t.Seconds(s.dts + s.cto) > sec) break;
            at = s.offset;
        }
        offset = std::min(offset, at);
    }
    if (offset == UINT64_MAX) offset = 0;
    cursor_ = static_cast<size_t>(std::lower_bound(order_.begin(), order_.end(), offset,
                                                   [](const Entry &e, uint64_t o) { return e.offset < o; }) -
                                  order_.begin());
    if (cursor_ < order_.size() && !src_->SeekTo(order_[cursor_].offset)) {
        if (error) *error = "mp4: seek failed";
        return false;
    }
    return true;
}

bool Reader::Emit(int track, const Sample &s, Packet *out, std::string *error) {
    const uint64_t here = src_->Offset();
    if (s.offset < here) {
        // Overlapping samples: not something a sane muxer writes.
        if (error) *error = "mp4: samples overlap";
        return false;
    }
    if (s.offset > here && !src_->Skip(s.offset - here)) {
        if (error) *error = "";  // the download ended
        return false;
    }
    out->data.resize(s.size);
    if (s.size > 0 && !src_->Read(out->data.data(), s.size)) {
        if (error) *error = "";
        return false;
    }
    const Track &t = movie_.tracks[static_cast<size_t>(track)];
    out->track = track;
    out->sync = s.sync;
    out->pts_sec = t.Seconds(s.dts + s.cto);
    out->duration_sec = t.timescale ? static_cast<double>(s.duration) / t.timescale : 0.0;
    return true;
}

bool Reader::NextProgressive(Packet *out, std::string *error) {
    if (cursor_ >= order_.size()) {
        if (error) error->clear();
        return false;
    }
    const Entry &e = order_[cursor_++];
    return Emit(e.track, movie_.tracks[static_cast<size_t>(e.track)].samples[e.index], out, error);
}

bool Reader::NextFragmented(Packet *out, std::string *error) {
    for (;;) {
        if (fragment_cursor_ < fragment_.size()) {
            const FragmentSample &fs = fragment_[fragment_cursor_++];
            if (fs.track != movie_.VideoTrack() && fs.track != movie_.AudioTrack()) continue;
            return Emit(fs.track, fs.sample, out, error);
        }
        // Past the fragment's last sample: on to the next top-level box.
        if (mdat_end_ > src_->Offset() && !src_->Skip(mdat_end_ - src_->Offset())) {
            if (error) error->clear();
            return false;
        }
        const uint64_t at = src_->Offset();
        uint64_t size = 0, header = 0;
        uint32_t type = 0;
        if (!ReadBoxHeader(&size, &type, &header)) {
            if (error) error->clear();
            return false;
        }
        if (size < header) {
            if (error) *error = "mp4: bad box";
            return false;
        }
        const uint64_t payload = size - header;
        if (type == FourCc("moof")) {
            if (payload > (64u << 20)) {
                if (error) *error = "mp4: moof too large";
                return false;
            }
            std::vector<uint8_t> buf(static_cast<size_t>(payload));
            if (!src_->Read(buf.data(), buf.size())) {
                if (error) error->clear();
                return false;
            }
            fragment_.clear();
            fragment_cursor_ = 0;
            if (!ParseMoof(buf.data(), buf.size(), at, movie_, &fragment_, error)) return false;
            mdat_end_ = 0;
            continue;
        }
        if (type == FourCc("mdat")) {
            // Its samples are what the moof before it listed; Emit skips
            // to each one, and the next box starts at mdat_end_.
            mdat_end_ = at + size;
            continue;
        }
        if (!src_->Skip(payload)) {
            if (error) error->clear();
            return false;
        }
    }
}

bool Reader::Next(Packet *out, std::string *error) {
    if (!src_) return false;
    return movie_.fragmented ? NextFragmented(out, error) : NextProgressive(out, error);
}

}  // namespace mp4
