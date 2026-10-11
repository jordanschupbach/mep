// Coverage for mp4_demux.h: a tiny progressive file and a tiny fragmented
// one built here box by box (sample tables, chunk spreading, edit lists,
// trex defaults, sidx, trun flags), read front to back and after seeks.
// With a path argument it also walks a real file -- e.g. a YouTube itag 18
// download or an adaptive .m4a -- and checks its packets come out in file
// order with sane times. No network, no display.
//
// Usage: mep-mp4-demux-test [file.mp4 ...]

#include "mp4_demux.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// A ByteSource over memory, counting the seeks it was asked for.
class MemorySource final : public mp4::ByteSource {
public:
    explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    bool Read(uint8_t *dst, size_t n) override {
        if (pos_ + n > bytes_.size()) return false;
        std::memcpy(dst, bytes_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    bool SeekTo(uint64_t offset) override {
        if (offset > bytes_.size()) return false;
        pos_ = static_cast<size_t>(offset);
        seeks_++;
        return true;
    }
    uint64_t Offset() const override { return pos_; }
    int seeks() const { return seeks_; }

private:
    std::vector<uint8_t> bytes_;
    size_t pos_ = 0;
    int seeks_ = 0;
};

class FileSource final : public mp4::ByteSource {
public:
    explicit FileSource(const char *path) : f_(std::fopen(path, "rb")) {}
    ~FileSource() override {
        if (f_) std::fclose(f_);
    }
    FileSource(const FileSource &) = delete;
    FileSource &operator=(const FileSource &) = delete;
    bool ok() const { return f_ != nullptr; }
    bool Read(uint8_t *dst, size_t n) override {
        if (std::fread(dst, 1, n, f_) != n) return false;
        pos_ += n;
        return true;
    }
    bool SeekTo(uint64_t offset) override {
        if (std::fseek(f_, static_cast<long>(offset), SEEK_SET) != 0) return false;
        pos_ = offset;
        return true;
    }
    uint64_t Offset() const override { return pos_; }

private:
    FILE *f_ = nullptr;
    uint64_t pos_ = 0;
};

// --- Building boxes ----------------------------------------------------------

using Bytes = std::vector<uint8_t>;

void Put32(Bytes &b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}
void Put16(Bytes &b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void PutBytes(Bytes &b, const Bytes &more) { b.insert(b.end(), more.begin(), more.end()); }

Bytes MakeBox(const char *type, const Bytes &payload) {
    Bytes b;
    Put32(b, static_cast<uint32_t>(payload.size() + 8));
    for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(type[i]));
    PutBytes(b, payload);
    return b;
}
// A full box: version and flags first.
Bytes MakeFull(const char *type, uint8_t version, uint32_t flags, const Bytes &payload) {
    Bytes p;
    p.push_back(version);
    p.push_back(static_cast<uint8_t>(flags >> 16));
    p.push_back(static_cast<uint8_t>(flags >> 8));
    p.push_back(static_cast<uint8_t>(flags));
    PutBytes(p, payload);
    return MakeBox(type, p);
}

Bytes Mdhd(uint32_t timescale, uint32_t duration) {
    Bytes p;
    Put32(p, 0);
    Put32(p, 0);
    Put32(p, timescale);
    Put32(p, duration);
    Put32(p, 0);  // language + pre_defined
    return MakeFull("mdhd", 0, 0, p);
}
Bytes Hdlr(const char *handler) {
    Bytes p;
    Put32(p, 0);
    for (int i = 0; i < 4; i++) p.push_back(static_cast<uint8_t>(handler[i]));
    for (int i = 0; i < 13; i++) p.push_back(0);
    return MakeFull("hdlr", 0, 0, p);
}
Bytes Tkhd(uint32_t id) {
    Bytes p;
    Put32(p, 0);
    Put32(p, 0);
    Put32(p, id);
    for (int i = 0; i < 68; i++) p.push_back(0);
    return MakeFull("tkhd", 0, 3, p);
}
Bytes VideoStsd() {
    Bytes entry;
    for (int i = 0; i < 6; i++) entry.push_back(0);
    Put16(entry, 1);
    for (int i = 0; i < 16; i++) entry.push_back(0);
    Put16(entry, 64);  // width
    Put16(entry, 48);  // height
    for (int i = 0; i < 50; i++) entry.push_back(0);
    PutBytes(entry, MakeBox("avcC", {1, 0x42, 0, 0x1e, 0xff, 0xe0, 0}));
    Bytes p;
    Put32(p, 1);
    PutBytes(p, MakeBox("avc1", entry));
    return MakeFull("stsd", 0, 0, p);
}
Bytes AudioStsd() {
    Bytes entry;
    for (int i = 0; i < 6; i++) entry.push_back(0);
    Put16(entry, 1);
    Put16(entry, 0);  // version
    for (int i = 0; i < 6; i++) entry.push_back(0);
    Put16(entry, 2);   // channels
    Put16(entry, 16);  // sample size
    Put32(entry, 0);
    Put32(entry, 44100u << 16);
    // esds: ES_Descriptor > DecoderConfigDescriptor > DecoderSpecificInfo {0x12, 0x10}.
    Bytes esds = {0x03, 0x19, 0x00, 0x01, 0x00, 0x04, 0x11, 0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x05, 0x02, 0x12, 0x10, 0x06, 0x01, 0x02};
    PutBytes(entry, MakeFull("esds", 0, 0, esds));
    Bytes p;
    Put32(p, 1);
    PutBytes(p, MakeBox("mp4a", entry));
    return MakeFull("stsd", 0, 0, p);
}

// Run-length tables: stts {count, delta}..., stsc {first_chunk, per_chunk, 1}...
Bytes Table(const char *type, const std::vector<uint32_t> &words, uint32_t entries) {
    Bytes p;
    Put32(p, entries);
    for (uint32_t w : words) Put32(p, w);
    return MakeFull(type, 0, 0, p);
}

Bytes Trak(uint32_t id, const char *handler, uint32_t timescale, uint32_t duration, const Bytes &stsd, const Bytes &tables,
           const Bytes &edts = {}) {
    Bytes minf = MakeBox("minf", MakeBox("stbl", [&] {
                             Bytes s = stsd;
                             PutBytes(s, tables);
                             return s;
                         }()));
    Bytes mdia = Mdhd(timescale, duration);
    PutBytes(mdia, Hdlr(handler));
    PutBytes(mdia, minf);
    Bytes trak = Tkhd(id);
    PutBytes(trak, edts);
    PutBytes(trak, MakeBox("mdia", mdia));
    return MakeBox("trak", trak);
}

Bytes Mvhd(uint32_t timescale, uint32_t duration) {
    Bytes p;
    Put32(p, 0);
    Put32(p, 0);
    Put32(p, timescale);
    Put32(p, duration);
    for (int i = 0; i < 80; i++) p.push_back(0);
    return MakeFull("mvhd", 0, 0, p);
}

// --- A progressive file -------------------------------------------------------
//
// Video: 4 frames at 10 fps (timescale 1000, delta 100), frames 0 and 2
// sync, sizes 10/11/12/13. Audio: 4 packets of 1024 at 44100 (delta 1024),
// sizes 5 each, with an edit list skipping 1024 samples of priming. Chunks
// interleave: [v0 v1] [a0 a1] [v2 v3] [a2 a3].
Bytes ProgressiveFile(uint64_t *mdat_start) {
    Bytes ftyp = MakeBox("ftyp", {'i', 's', 'o', 'm', 0, 0, 0, 0});
    // The mdat's payload layout, relative to its first byte.
    const uint32_t v_sizes[4] = {10, 11, 12, 13};
    Bytes payload;
    std::vector<uint32_t> v_chunk_rel, a_chunk_rel;
    auto put_samples = [&](uint8_t tag, const uint32_t *sizes, int count) {
        for (int i = 0; i < count; i++)
            for (uint32_t k = 0; k < sizes[i]; k++) payload.push_back(static_cast<uint8_t>(tag + i));
    };
    const uint32_t a_sizes[2] = {5, 5};
    v_chunk_rel.push_back(static_cast<uint32_t>(payload.size()));
    put_samples(0x10, v_sizes, 2);
    a_chunk_rel.push_back(static_cast<uint32_t>(payload.size()));
    put_samples(0xa0, a_sizes, 2);
    v_chunk_rel.push_back(static_cast<uint32_t>(payload.size()));
    put_samples(0x12, v_sizes + 2, 2);
    a_chunk_rel.push_back(static_cast<uint32_t>(payload.size()));
    put_samples(0xa2, a_sizes, 2);

    // The moov's size doesn't depend on the chunk offsets' values, so
    // build it once to measure, then again with the real offsets.
    auto moov_for = [&](uint32_t base) {
        Bytes vt;
        PutBytes(vt, Table("stts", {4, 100}, 1));
        PutBytes(vt, Table("stss", {1, 3}, 2));
        PutBytes(vt, Table("stsc", {1, 2, 1}, 1));
        {
            Bytes p;
            Put32(p, 0);
            Put32(p, 4);
            for (uint32_t s : v_sizes) Put32(p, s);
            PutBytes(vt, MakeFull("stsz", 0, 0, p));
        }
        PutBytes(vt, Table("stco", {base + v_chunk_rel[0], base + v_chunk_rel[1]}, 2));
        Bytes at;
        PutBytes(at, Table("stts", {4, 1024}, 1));
        PutBytes(at, Table("stsc", {1, 2, 1}, 1));
        {
            Bytes p;
            Put32(p, 5);
            Put32(p, 4);
            PutBytes(at, MakeFull("stsz", 0, 0, p));
        }
        PutBytes(at, Table("stco", {base + a_chunk_rel[0], base + a_chunk_rel[1]}, 2));
        Bytes elst;
        Put32(elst, 1);
        Put32(elst, 400);   // segment duration (movie timescale 1000)
        Put32(elst, 1024);  // media time: skip the priming
        Put32(elst, 0x00010000);
        Bytes edts = MakeBox("edts", MakeFull("elst", 0, 0, elst));
        Bytes moov = Mvhd(1000, 400);
        PutBytes(moov, Trak(1, "vide", 1000, 400, VideoStsd(), vt));
        PutBytes(moov, Trak(2, "soun", 44100, 4096, AudioStsd(), at, edts));
        return MakeBox("moov", moov);
    };
    const size_t moov_size = moov_for(0).size();
    const uint32_t base = static_cast<uint32_t>(ftyp.size() + moov_size + 8);
    *mdat_start = base;
    Bytes file = ftyp;
    PutBytes(file, moov_for(base));
    PutBytes(file, MakeBox("mdat", payload));
    return file;
}

void TestProgressive() {
    uint64_t mdat_start = 0;
    MemorySource src(ProgressiveFile(&mdat_start));
    mp4::Reader r;
    std::string err;
    CHECK(r.Open(&src, &err));
    const mp4::Movie &m = r.movie();
    CHECK(!m.fragmented);
    CHECK(m.VideoTrack() == 0 && m.AudioTrack() == 1);
    const mp4::Track &v = m.tracks[0];
    const mp4::Track &a = m.tracks[1];
    CHECK(v.codec == "avc1" && v.width == 64 && v.height == 48);
    CHECK(v.config.size() == 7 && v.config[1] == 0x42);
    CHECK(a.codec == "mp4a" && a.channels == 2 && a.sample_rate == 44100);
    CHECK((a.config == std::vector<uint8_t>{0x12, 0x10}));
    CHECK(v.samples.size() == 4 && a.samples.size() == 4);
    CHECK(v.samples[0].sync && !v.samples[1].sync && v.samples[2].sync && !v.samples[3].sync);
    CHECK(v.samples[1].offset == mdat_start + 10 && v.samples[2].offset == mdat_start + 31);
    CHECK(a.media_time == 1024);

    // File order: v0 v1 a0 a1 v2 v3 a2 a3, each sample's bytes intact.
    const int want_track[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    const uint8_t want_byte[8] = {0x10, 0x11, 0xa0, 0xa1, 0x12, 0x13, 0xa2, 0xa3};
    mp4::Packet p;
    for (int i = 0; i < 8; i++) {
        CHECK(r.Next(&p, &err));
        CHECK(p.track == want_track[i]);
        CHECK(!p.data.empty() && p.data[0] == want_byte[i] && p.data.back() == want_byte[i]);
    }
    CHECK(!r.Next(&p, &err) && err.empty());

    // Times: video at 0.0/0.1/..., audio shifted back by the priming.
    CHECK(std::fabs(v.Seconds(v.samples[2].dts) - 0.2) < 1e-9);
    CHECK(std::fabs(a.Seconds(a.samples[1].dts) - 0.0) < 1e-9);
    CHECK(std::fabs(a.Seconds(a.samples[2].dts) - 1024.0 / 44100.0) < 1e-9);

    // A seek to 0.35 s: the sync frame at or before it is v2 (0.2 s, file
    // offset 31) and the audio playing then is a3 (offset 61); reading
    // restarts at the earlier of the two.
    CHECK(r.Seek(0.35, &err));
    CHECK(r.Next(&p, &err));
    CHECK(p.track == 0 && p.data[0] == 0x12 && p.sync);
    CHECK(r.Next(&p, &err) && p.track == 0 && p.data[0] == 0x13);
    CHECK(r.Next(&p, &err) && p.track == 1 && p.data[0] == 0xa2);
}

// Seeking a progressive file restarts at the earlier of the two tracks'
// needed bytes.
void TestProgressiveSeekOrder() {
    uint64_t mdat_start = 0;
    MemorySource src(ProgressiveFile(&mdat_start));
    mp4::Reader r;
    std::string err;
    CHECK(r.Open(&src, &err));
    CHECK(r.Seek(0.0, &err));
    mp4::Packet p;
    CHECK(r.Next(&p, &err) && p.track == 0 && p.data[0] == 0x10);
}

// --- A fragmented file ---------------------------------------------------------
//
// One audio track (timescale 1000), trex default duration 20, default size
// 3; two fragments of three samples each, indexed by a sidx.
Bytes FragmentedFile(std::vector<uint64_t> *fragment_offsets) {
    Bytes ftyp = MakeBox("ftyp", {'d', 'a', 's', 'h', 0, 0, 0, 0});
    Bytes at;
    PutBytes(at, Table("stts", {}, 0));
    PutBytes(at, Table("stsc", {}, 0));
    {
        Bytes p;
        Put32(p, 0);
        Put32(p, 0);
        PutBytes(at, MakeFull("stsz", 0, 0, p));
    }
    PutBytes(at, Table("stco", {}, 0));
    Bytes trex;
    Put32(trex, 1);   // track id
    Put32(trex, 1);   // sample description
    Put32(trex, 20);  // duration
    Put32(trex, 3);   // size
    Put32(trex, 0);   // flags
    Bytes moov = Mvhd(1000, 120);
    PutBytes(moov, Trak(1, "soun", 1000, 0, AudioStsd(), at));
    PutBytes(moov, MakeBox("mvex", MakeFull("trex", 0, 0, trex)));
    Bytes moov_box = MakeBox("moov", moov);

    auto fragment = [&](uint8_t tag, uint32_t base_time) {
        // trun: 3 samples, data offset (set below), sizes from the trex
        // default except the middle one (flag 0x200 is all-or-nothing, so
        // sizes are listed for all three).
        auto build = [&](uint32_t data_offset) {
            Bytes tfhd;
            Put32(tfhd, 1);
            Bytes tfdt;
            Put32(tfdt, base_time);
            Bytes trun;
            Put32(trun, 3);
            Put32(trun, data_offset);
            Put32(trun, 3);
            Put32(trun, 4);
            Put32(trun, 3);
            Bytes traf = MakeFull("tfhd", 0, 0x020000, tfhd);
            PutBytes(traf, MakeFull("tfdt", 0, 0, tfdt));
            PutBytes(traf, MakeFull("trun", 0, 0x000201, trun));
            Bytes moof_payload = MakeFull("mfhd", 0, 0, {0, 0, 0, 1});
            PutBytes(moof_payload, MakeBox("traf", traf));
            return MakeBox("moof", moof_payload);
        };
        const size_t moof_size = build(0).size();
        Bytes out = build(static_cast<uint32_t>(moof_size + 8));
        Bytes data;
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < (i == 1 ? 4 : 3); k++) data.push_back(static_cast<uint8_t>(tag + i));
        PutBytes(out, MakeBox("mdat", data));
        return out;
    };
    Bytes f1 = fragment(0x30, 0);
    Bytes f2 = fragment(0x40, 60);
    // sidx: timescale 1000, two references of 60 each, first offset 0.
    Bytes sidx;
    Put32(sidx, 1);     // reference id
    Put32(sidx, 1000);  // timescale
    Put32(sidx, 0);     // earliest presentation time
    Put32(sidx, 0);     // first offset
    Put16(sidx, 0);
    Put16(sidx, 2);
    Put32(sidx, static_cast<uint32_t>(f1.size()));
    Put32(sidx, 60);
    Put32(sidx, 0x90000000u);
    Put32(sidx, static_cast<uint32_t>(f2.size()));
    Put32(sidx, 60);
    Put32(sidx, 0x90000000u);
    Bytes sidx_box = MakeFull("sidx", 0, 0, sidx);
    Bytes file = ftyp;
    PutBytes(file, moov_box);
    PutBytes(file, sidx_box);
    fragment_offsets->push_back(file.size());
    PutBytes(file, f1);
    fragment_offsets->push_back(file.size());
    PutBytes(file, f2);
    return file;
}

void TestFragmented() {
    std::vector<uint64_t> frags;
    MemorySource src(FragmentedFile(&frags));
    mp4::Reader r;
    std::string err;
    CHECK(r.Open(&src, &err));
    const mp4::Movie &m = r.movie();
    CHECK(m.fragmented && m.AudioTrack() == 0);
    CHECK(m.segments.size() == 2);
    CHECK(m.segments[0].offset == frags[0] && m.segments[1].offset == frags[1]);
    CHECK(std::fabs(m.segments[1].start_sec - 0.06) < 1e-9);
    mp4::Packet p;
    const uint8_t want[6] = {0x30, 0x31, 0x32, 0x40, 0x41, 0x42};
    for (int i = 0; i < 6; i++) {
        CHECK(r.Next(&p, &err));
        CHECK(p.data[0] == want[i]);
        CHECK(p.data.size() == (i % 3 == 1 ? 4u : 3u));
        CHECK(std::fabs(p.pts_sec - 0.02 * i) < 1e-9);
        CHECK(std::fabs(p.duration_sec - 0.02) < 1e-9);
    }
    CHECK(!r.Next(&p, &err) && err.empty());
    // A seek into the second fragment starts at its moof.
    CHECK(r.Seek(0.07, &err));
    CHECK(src.Offset() == frags[1]);
    CHECK(r.Next(&p, &err) && p.data[0] == 0x40);
}

// A real file: packets in file order, times sane, every sample read.
void TestFile(const char *path) {
    FileSource src(path);
    if (!src.ok()) {
        std::printf("  %s: not found, skipped\n", path);
        return;
    }
    mp4::Reader r;
    std::string err;
    CHECK(r.Open(&src, &err));
    const mp4::Movie &m = r.movie();
    size_t counts[2] = {0, 0};
    double last_pts[2] = {-1e9, -1e9};
    double max_pts = 0.0;
    size_t syncs = 0;
    mp4::Packet p;
    while (r.Next(&p, &err)) {
        const int kind = p.track == m.VideoTrack() ? 0 : 1;
        counts[kind]++;
        if (kind == 0 && p.sync) syncs++;
        if (kind == 1) CHECK(p.pts_sec > last_pts[1]);  // audio never reorders
        last_pts[kind] = p.pts_sec;
        max_pts = std::max(max_pts, p.pts_sec);
    }
    CHECK(err.empty());
    std::printf("  %s: %s, %zu video (%zu sync), %zu audio packets, last pts %.2f s of %.2f s\n", path,
                m.fragmented ? "fragmented" : "progressive", counts[0], syncs, counts[1], max_pts, m.duration_sec);
    CHECK(counts[0] + counts[1] > 0);
    CHECK(max_pts <= m.duration_sec + 1.0);
    // Seek to the middle: the first packet of each kind is at or before it,
    // and close to it.
    const double mid = m.duration_sec / 2;
    CHECK(r.Seek(mid, &err));
    bool seen[2] = {false, false};
    for (int i = 0; i < 2000 && !(seen[0] || m.VideoTrack() < 0) + !(seen[1] || m.AudioTrack() < 0) > 0; i++) {
        if (!r.Next(&p, &err)) break;
        const int kind = p.track == m.VideoTrack() ? 0 : 1;
        if (seen[kind]) continue;
        seen[kind] = true;
        CHECK(p.pts_sec <= mid + 0.05);
        CHECK(p.pts_sec >= mid - 15.0);
        if (kind == 0) CHECK(p.sync);
    }
}

}  // namespace

int main(int argc, char **argv) {
    TestProgressive();
    TestProgressiveSeekOrder();
    TestFragmented();
    for (int i = 1; i < argc; i++) TestFile(argv[i]);
    std::printf("mp4_demux_test: all checks passed\n");
    return 0;
}
