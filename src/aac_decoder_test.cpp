// Coverage for aac_decoder.h: the standard tables' integrity, the
// AudioSpecificConfig parse (LC accepted, HE-AAC's LC core accepted, other
// object types and channel layouts refused), a hand-built silent access
// unit, and malformed input failing cleanly. Given an .m4a/.mp4 path it
// also decodes that file's whole AAC track (a small box walker below finds
// it), checks every access unit decodes, and with a second path writes the
// interleaved PCM16 there -- what the afconvert comparison reads. No
// network, no audio device.
//
// Usage: mep-aac-decoder-test [file.m4a [out.pcm]]

#include "aac_decoder.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// MSB-first bit writer for hand-built access units.
struct BitWriter {
    std::vector<uint8_t> bytes;
    int used = 8;
    void Put(uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (used == 8) {
                bytes.push_back(0);
                used = 0;
            }
            if ((value >> i) & 1u) bytes.back() = static_cast<uint8_t>(bytes.back() | (0x80u >> used));
            ++used;
        }
    }
};

void TestTables() {
    std::string error;
    const bool ok = aac::CheckTables(&error);
    if (!ok) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(ok);
}

void TestConfigure() {
    aac::Decoder dec;
    std::string error;
    const uint8_t lc_44k_stereo[] = {0x12, 0x10};  // AOT 2, index 4, config 2
    CHECK(dec.Configure(lc_44k_stereo, sizeof lc_44k_stereo, &error));
    CHECK(dec.SampleRate() == 44100 && dec.Channels() == 2);
    const uint8_t lc_48k_mono[] = {0x11, 0x88};  // AOT 2, index 3, config 1
    CHECK(dec.Configure(lc_48k_mono, sizeof lc_48k_mono, &error));
    CHECK(dec.SampleRate() == 48000 && dec.Channels() == 1);
    // Explicit HE-AAC: AOT 5, core rate 22050 (index 7), stereo, extension
    // rate 44100 (index 4), core AOT 2 -- decoded as its LC core.
    BitWriter he;
    he.Put(5, 5);
    he.Put(7, 4);
    he.Put(2, 4);
    he.Put(4, 4);
    he.Put(2, 5);
    he.Put(0, 3);
    CHECK(dec.Configure(he.bytes.data(), he.bytes.size(), &error));
    CHECK(dec.SampleRate() == 22050 && dec.Channels() == 2);
    const uint8_t main_profile[] = {0x0a, 0x10};  // AOT 1
    CHECK(!dec.Configure(main_profile, sizeof main_profile, &error) && !error.empty());
    const uint8_t six_channels[] = {0x12, 0x30};  // config 6
    CHECK(!dec.Configure(six_channels, sizeof six_channels, &error));
    CHECK(!dec.Configure(lc_44k_stereo, 1, &error));
}

// An SCE with max_sfb = 0 (no bands, all zero spectrum) then END.
std::vector<uint8_t> SilentMonoFrame() {
    BitWriter w;
    w.Put(0, 3);    // SCE
    w.Put(0, 4);    // element_instance_tag
    w.Put(100, 8);  // global_gain
    w.Put(0, 1);    // ics_reserved_bit
    w.Put(0, 2);    // ONLY_LONG
    w.Put(0, 1);    // sine window
    w.Put(0, 6);    // max_sfb
    w.Put(0, 1);    // predictor_data_present
    w.Put(0, 1);    // pulse_data_present
    w.Put(0, 1);    // tns_data_present
    w.Put(0, 1);    // gain_control_data_present
    w.Put(7, 3);    // END
    return w.bytes;
}

void TestSilentFrame() {
    aac::Decoder dec;
    std::string error;
    const uint8_t lc_48k_mono[] = {0x11, 0x88};
    CHECK(dec.Configure(lc_48k_mono, sizeof lc_48k_mono, &error));
    const std::vector<uint8_t> frame = SilentMonoFrame();
    std::vector<int16_t> pcm;
    for (int i = 0; i < 3; ++i) CHECK(dec.Decode(frame.data(), frame.size(), &pcm, &error));
    CHECK(pcm.size() == 3 * 1024);
    for (int16_t s : pcm) CHECK(s == 0);
    // A mono frame in a stereo stream plays on both channels.
    const uint8_t lc_44k_stereo[] = {0x12, 0x10};
    CHECK(dec.Configure(lc_44k_stereo, sizeof lc_44k_stereo, &error));
    pcm.clear();
    CHECK(dec.Decode(frame.data(), frame.size(), &pcm, &error));
    CHECK(pcm.size() == 2 * 1024);
}

void TestMalformed() {
    aac::Decoder dec;
    std::string error;
    std::vector<int16_t> pcm;
    const uint8_t junk[] = {0xff, 0xff};
    CHECK(!dec.Decode(junk, sizeof junk, &pcm, &error));  // not configured
    const uint8_t lc_44k_stereo[] = {0x12, 0x10};
    CHECK(dec.Configure(lc_44k_stereo, sizeof lc_44k_stereo, &error));
    std::vector<uint8_t> frame = SilentMonoFrame();
    frame.resize(1);  // cut off before END
    CHECK(!dec.Decode(frame.data(), frame.size(), &pcm, &error) && !error.empty());
    CHECK(!dec.Decode(nullptr, 0, &pcm, &error));
    // Random bytes must fail or decode, never crash.
    uint32_t seed = 12345;
    std::vector<uint8_t> noise(300);
    for (int round = 0; round < 2000; ++round) {
        for (uint8_t &b : noise) {
            seed = seed * 1103515245u + 12345u;
            b = static_cast<uint8_t>(seed >> 24);
        }
        dec.Decode(noise.data(), 1 + static_cast<size_t>(seed % noise.size()), &pcm, &error);
    }
    CHECK(pcm.size() % 2048 == 0);
}

// --- Test-only MP4 reading: the first sound track's ASC and samples. ----------

uint32_t Be32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

struct Box {
    std::string type;
    size_t body = 0, end = 0;
};

// The child boxes of [begin, end).
std::vector<Box> Children(const std::vector<uint8_t> &f, size_t begin, size_t end) {
    std::vector<Box> out;
    size_t pos = begin;
    while (pos + 8 <= end) {
        uint64_t size = Be32(&f[pos]);
        size_t header = 8;
        if (size == 1 && pos + 16 <= end) {
            size = static_cast<uint64_t>(Be32(&f[pos + 8])) << 32 | Be32(&f[pos + 12]);
            header = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < header || pos + size > end) break;
        out.push_back({std::string(reinterpret_cast<const char *>(&f[pos + 4]), 4), pos + header, pos + static_cast<size_t>(size)});
        pos += static_cast<size_t>(size);
    }
    return out;
}

// The first box of `type` (by value: callers often search a temporary list).
std::optional<Box> Find(const std::vector<Box> &boxes, const char *type) {
    for (const Box &b : boxes)
        if (b.type == type) return b;
    return std::nullopt;
}

// An MPEG-4 descriptor's length (up to four 7-bit groups).
size_t DescriptorLength(const std::vector<uint8_t> &f, size_t *pos) {
    size_t len = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t b = f[(*pos)++];
        len = (len << 7) | (b & 0x7fu);
        if (!(b & 0x80u)) break;
    }
    return len;
}

struct AacTrack {
    std::vector<uint8_t> asc;
    std::vector<std::pair<size_t, size_t>> samples;  // offset, size
};

bool ReadAacTrack(const std::vector<uint8_t> &f, AacTrack *out) {
    const std::optional<Box> moov = Find(Children(f, 0, f.size()), "moov");
    if (!moov) return false;
    for (const Box &trak : Children(f, moov->body, moov->end)) {
        if (trak.type != "trak") continue;
        const std::optional<Box> mdia = Find(Children(f, trak.body, trak.end), "mdia");
        if (!mdia) continue;
        const std::vector<Box> mdia_kids = Children(f, mdia->body, mdia->end);
        const std::optional<Box> hdlr = Find(mdia_kids, "hdlr");
        if (!hdlr || std::memcmp(&f[hdlr->body + 8], "soun", 4) != 0) continue;
        const std::optional<Box> minf = Find(mdia_kids, "minf");
        const std::optional<Box> stbl = minf ? Find(Children(f, minf->body, minf->end), "stbl") : std::nullopt;
        if (!stbl) return false;
        const std::vector<Box> st = Children(f, stbl->body, stbl->end);
        const std::optional<Box> stsd = Find(st, "stsd"), stsz = Find(st, "stsz"), stsc = Find(st, "stsc");
        const std::optional<Box> stco = Find(st, "stco"), co64 = Find(st, "co64");
        if (!stsd || !stsz || !stsc || (!stco && !co64)) return false;
        // stsd -> mp4a (28-byte sample entry body) -> esds -> ES_Descriptor -> DecoderConfig -> DecoderSpecificInfo.
        const std::optional<Box> mp4a = Find(Children(f, stsd->body + 8, stsd->end), "mp4a");
        if (!mp4a) return false;
        const std::optional<Box> esds = Find(Children(f, mp4a->body + 28, mp4a->end), "esds");
        if (!esds) return false;
        size_t pos = esds->body + 4;
        if (f[pos++] != 0x03) return false;
        DescriptorLength(f, &pos);
        const uint8_t es_flags = f[pos + 2];
        pos += 3;
        if (es_flags & 0x80u) pos += 2;
        if (es_flags & 0x40u) pos += 1 + f[pos];
        if (es_flags & 0x20u) pos += 2;
        if (f[pos++] != 0x04) return false;
        DescriptorLength(f, &pos);
        pos += 13;
        if (f[pos++] != 0x05) return false;
        const size_t asc_len = DescriptorLength(f, &pos);
        out->asc.assign(f.begin() + static_cast<long>(pos), f.begin() + static_cast<long>(pos + asc_len));
        // Sample sizes, chunk offsets, and the sample-to-chunk runs joining them.
        const uint32_t fixed_size = Be32(&f[stsz->body + 4]), count = Be32(&f[stsz->body + 8]);
        std::vector<size_t> sizes(count);
        for (uint32_t i = 0; i < count; ++i) sizes[i] = fixed_size ? fixed_size : Be32(&f[stsz->body + 12 + 4 * i]);
        std::vector<size_t> chunks;
        if (stco) {
            for (uint32_t i = 0, n = Be32(&f[stco->body + 4]); i < n; ++i) chunks.push_back(Be32(&f[stco->body + 8 + 4 * i]));
        } else {
            for (uint32_t i = 0, n = Be32(&f[co64->body + 4]); i < n; ++i)
                chunks.push_back(static_cast<size_t>(static_cast<uint64_t>(Be32(&f[co64->body + 8 + 8 * i])) << 32 |
                                                     Be32(&f[co64->body + 12 + 8 * i])));
        }
        // A fragmented file (YouTube's audio-only streams): the samples are
        // in each moof's track runs instead.
        if (count == 0) {
            for (const Box &moof : Children(f, 0, f.size())) {
                if (moof.type != "moof") continue;
                for (const Box &traf : Children(f, moof.body, moof.end)) {
                    if (traf.type != "traf") continue;
                    const std::vector<Box> tk = Children(f, traf.body, traf.end);
                    const std::optional<Box> tfhd = Find(tk, "tfhd");
                    if (!tfhd) continue;
                    const uint32_t tf = Be32(&f[tfhd->body]) & 0xffffffu;
                    size_t p = tfhd->body + 8, base = moof.body - 8;
                    if (tf & 0x1u) {
                        base = static_cast<size_t>(static_cast<uint64_t>(Be32(&f[p])) << 32 | Be32(&f[p + 4]));
                        p += 8;
                    }
                    if (tf & 0x2u) p += 4;
                    if (tf & 0x8u) p += 4;
                    const uint32_t default_size = (tf & 0x10u) ? Be32(&f[p]) : 0;
                    for (const Box &trun : tk) {
                        if (trun.type != "trun") continue;
                        const uint32_t rf = Be32(&f[trun.body]) & 0xffffffu, n = Be32(&f[trun.body + 4]);
                        size_t q = trun.body + 8, off = base;
                        if (rf & 0x1u) {
                            off = base + static_cast<size_t>(static_cast<int32_t>(Be32(&f[q])));
                            q += 4;
                        }
                        if (rf & 0x4u) q += 4;
                        for (uint32_t i = 0; i < n; ++i) {
                            if (rf & 0x100u) q += 4;
                            size_t size = default_size;
                            if (rf & 0x200u) {
                                size = Be32(&f[q]);
                                q += 4;
                            }
                            if (rf & 0x400u) q += 4;
                            if (rf & 0x800u) q += 4;
                            out->samples.push_back({off, size});
                            off += size;
                        }
                    }
                }
            }
            return true;
        }
        const uint32_t runs = Be32(&f[stsc->body + 4]);
        size_t sample = 0;
        for (uint32_t r = 0; r < runs; ++r) {
            const uint8_t *e = &f[stsc->body + 8 + 12 * r];
            const uint32_t first = Be32(e), per_chunk = Be32(e + 4);
            const uint32_t last = r + 1 < runs ? Be32(e + 12) : static_cast<uint32_t>(chunks.size()) + 1;
            for (uint32_t c = first; c < last && c <= chunks.size(); ++c) {
                size_t off = chunks[c - 1];
                for (uint32_t s = 0; s < per_chunk && sample < sizes.size(); ++s, ++sample) {
                    out->samples.push_back({off, sizes[sample]});
                    off += sizes[sample];
                }
            }
        }
        return true;
    }
    return false;
}

void TestFile(const char *path, const char *pcm_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::printf("aac_decoder_test: %s not found, skipping the file decode\n", path);
        return;
    }
    const std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    AacTrack track;
    CHECK(ReadAacTrack(f, &track));
    aac::Decoder dec;
    std::string error;
    CHECK(dec.Configure(track.asc.data(), track.asc.size(), &error));
    std::vector<int16_t> pcm;
    pcm.reserve(track.samples.size() * 1024 * static_cast<size_t>(dec.Channels()));
    int failures = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto &[off, size] : track.samples) {
        if (off + size > f.size() || !dec.Decode(&f[off], size, &pcm, &error)) {
            if (failures++ == 0) std::fprintf(stderr, "first failure: %s\n", error.c_str());
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("aac_decoder_test: %zu access units, %d Hz x %d, %d failed, %.0f ms\n", track.samples.size(), dec.SampleRate(),
                dec.Channels(), failures, ms);
    CHECK(failures == 0);
    if (pcm_out) {
        std::ofstream o(pcm_out, std::ios::binary);
        o.write(reinterpret_cast<const char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(int16_t)));
    }
}

}  // namespace

int main(int argc, char **argv) {
    TestTables();
    TestConfigure();
    TestSilentFrame();
    TestMalformed();
    if (argc > 1) TestFile(argv[1], argc > 2 ? argv[2] : nullptr);
    std::printf("aac_decoder_test: all checks passed\n");
    return 0;
}
