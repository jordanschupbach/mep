// Microphone-free tests for audio_capture.h: the resampler (length,
// frequency, passband gain, anti-aliasing, streaming == one-shot), the WAV
// writer (header bytes, round trip through WavDoc) and SegmentWriter
// (segment sizes and names, final partial segment).

#include "audio_capture.h"
#include "wav_doc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

constexpr double kPi = 3.14159265358979323846;

std::vector<float> Sine(double freq, int rate, size_t n, double amp = 0.5) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
    return v;
}

// Amplitude of the `freq` component (Goertzel-style single DFT bin) over
// v[from, to).
double ToneAmplitude(const std::vector<float> &v, size_t from, size_t to, double freq, int rate) {
    double re = 0, im = 0;
    for (size_t i = from; i < to; ++i) {
        const double ph = 2.0 * kPi * freq * static_cast<double>(i) / rate;
        re += static_cast<double>(v[i]) * std::cos(ph);
        im += static_cast<double>(v[i]) * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

double Rms(const std::vector<float> &v, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    return std::sqrt(s / static_cast<double>(to - from));
}

// Zero-crossing frequency estimate over v[from, to).
double ZeroCrossFreq(const std::vector<float> &v, size_t from, size_t to, int rate) {
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i)
        if ((v[i - 1] < 0) != (v[i] < 0)) ++crossings;
    return crossings / 2.0 / (static_cast<double>(to - from) / rate);
}

std::vector<float> ResampleAll(const std::vector<float> &in, int from, int to, size_t block) {
    mep::audio::Resampler r(from, to);
    std::vector<float> out;
    for (size_t i = 0; i < in.size(); i += block) r.Process(in.data() + i, std::min(block, in.size() - i), out);
    r.Flush(out);
    return out;
}

void TestResampler(int in_rate) {
    const int out_rate = 16000;
    const size_t n = static_cast<size_t>(in_rate) * 2;  // 2 s
    // 1 kHz: correct length, frequency and gain.
    const std::vector<float> in = Sine(1000.0, in_rate, n);
    const std::vector<float> out = ResampleAll(in, in_rate, out_rate, 997);
    const size_t expect = (n * out_rate + static_cast<size_t>(in_rate) / 2) / static_cast<size_t>(in_rate);
    std::printf("resample %d->%d: %zu in, %zu out (expected %zu)\n", in_rate, out_rate, n, out.size(), expect);
    CHECK(out.size() == expect);
    const size_t a = 1000, b = out.size() - 1000;  // skip edges
    const double f = ZeroCrossFreq(out, a, b, out_rate);
    const double amp = ToneAmplitude(out, a, b, 1000.0, out_rate);
    std::printf("  1 kHz tone: zero-cross freq %.2f Hz, amplitude %.5f (in 0.5)\n", f, amp);
    CHECK(std::fabs(f - 1000.0) < 1.0);
    CHECK(std::fabs(amp - 0.5) < 0.5 * 0.002);
    // Phase/delay: output sample k must equal the input sine at time k/out.
    double max_err = 0;
    for (size_t k = a; k < b; ++k) {
        const double ideal = 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(k) / out_rate);
        max_err = std::max(max_err, std::fabs(static_cast<double>(out[k]) - ideal));
    }
    std::printf("  max deviation from ideal 1 kHz at 16 kHz: %.2e\n", max_err);
    CHECK(max_err < 1e-3);

    // 3.4 kHz (top of the speech band): still passed.
    const std::vector<float> hi = ResampleAll(Sine(3400.0, in_rate, n), in_rate, out_rate, 4096);
    const double hi_amp = ToneAmplitude(hi, a, hi.size() - 1000, 3400.0, out_rate);
    std::printf("  3.4 kHz amplitude %.5f\n", hi_amp);
    CHECK(std::fabs(hi_amp - 0.5) < 0.5 * 0.005);

    // 11 kHz is above the 8 kHz output Nyquist: it must be filtered, not
    // aliased down to 5 kHz.
    const std::vector<float> alias = ResampleAll(Sine(11000.0, in_rate, n), in_rate, out_rate, 4096);
    const double alias_rms = Rms(alias, a, alias.size() - 1000);
    const double alias_db = 20.0 * std::log10(alias_rms / (0.5 / std::sqrt(2.0)) + 1e-30);
    std::printf("  11 kHz (out of band) residual: %.1f dB\n", alias_db);
    CHECK(alias_db < -70.0);

    // Just past the 8 kHz Nyquist (the transition band's far edge).
    const std::vector<float> edge = ResampleAll(Sine(8600.0, in_rate, n), in_rate, out_rate, 4096);
    const double edge_db = 20.0 * std::log10(Rms(edge, a, edge.size() - 1000) / (0.5 / std::sqrt(2.0)) + 1e-30);
    std::printf("  8.6 kHz (just out of band) residual: %.1f dB\n", edge_db);
    CHECK(edge_db < -60.0);

    // Streaming in odd blocks gives the same samples as one big block.
    const std::vector<float> one = ResampleAll(in, in_rate, out_rate, n);
    const std::vector<float> tiny = ResampleAll(in, in_rate, out_rate, 7);
    CHECK(one.size() == tiny.size());
    double diff = 0;
    for (size_t i = 0; i < std::min(one.size(), tiny.size()); ++i) diff = std::max(diff, static_cast<double>(std::fabs(one[i] - tiny[i])));
    CHECK(diff == 0.0);
}

void TestUpsample() {
    const size_t n = 8000 * 2;
    const std::vector<float> out = ResampleAll(Sine(440.0, 8000, n), 8000, 16000, 333);
    CHECK(out.size() == n * 2);
    const double f = ZeroCrossFreq(out, 1000, out.size() - 1000, 16000);
    std::printf("resample 8000->16000: %zu out, %.2f Hz\n", out.size(), f);
    CHECK(std::fabs(f - 440.0) < 1.0);
}

std::vector<unsigned char> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t Le32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8U | static_cast<uint32_t>(p[2]) << 16U |
           static_cast<uint32_t>(p[3]) << 24U;
}
uint16_t Le16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | p[1] << 8U); }

void TestWav(const std::filesystem::path &dir) {
    std::vector<int16_t> s = {0, 1, -1, 32767, -32768, 1234, -4321};
    const std::string path = (dir / "w.wav").string();
    std::string err;
    CHECK(WriteWavPcm16(path, s.data(), s.size(), 16000, 1, &err));
    CHECK(!std::filesystem::exists(path + ".part"));
    const std::vector<unsigned char> b = ReadFile(path);
    CHECK(b.size() == 44 + s.size() * 2);
    if (b.size() < 44) return;
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(Le32(b.data() + 4) == 36 + s.size() * 2);
    CHECK(std::memcmp(b.data() + 8, "WAVEfmt ", 8) == 0);
    CHECK(Le32(b.data() + 16) == 16);
    CHECK(Le16(b.data() + 20) == 1);      // PCM
    CHECK(Le16(b.data() + 22) == 1);      // mono
    CHECK(Le32(b.data() + 24) == 16000);  // rate
    CHECK(Le32(b.data() + 28) == 32000);  // byte rate
    CHECK(Le16(b.data() + 32) == 2);      // block align
    CHECK(Le16(b.data() + 34) == 16);     // bits
    CHECK(std::memcmp(b.data() + 36, "data", 4) == 0);
    CHECK(Le32(b.data() + 40) == s.size() * 2);
    WavDoc doc;
    CHECK(doc.LoadFromMemory(b.data(), b.size()));
    CHECK(doc.SampleRate() == 16000 && doc.Channels() == 1);
    CHECK(doc.Samples() == s);

    CHECK(mep::audio::FloatToPcm16(1.5f) == 32767);
    CHECK(mep::audio::FloatToPcm16(-1.5f) == -32767);
    CHECK(mep::audio::FloatToPcm16(0.5f) == 16384);
}

void TestSegments(const std::filesystem::path &dir) {
    mep::audio::SegmentOptions opt;
    opt.dir = (dir / "segs").string();  // does not exist yet
    opt.prefix = "lua_abc";
    opt.chunk_seconds = 4;
    std::vector<std::string> got;
    mep::audio::SegmentWriter w(opt, 48000, [&got](const std::string &p) { got.push_back(p); });
    const std::vector<float> in = Sine(500.0, 48000, 48000 * 9 + 24000);  // 9.5 s
    std::string err;
    for (size_t i = 0; i < in.size(); i += 4800) CHECK(w.Push(in.data() + i, std::min<size_t>(4800, in.size() - i), &err));
    CHECK(got.size() == 2);  // two full 4 s segments while recording
    CHECK(w.Finish(&err));
    CHECK(got.size() == 3);  // + the 1.5 s tail
    const size_t expect[3] = {64000, 64000, 24000};
    for (size_t i = 0; i < got.size() && i < 3; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "lua_abc_%03zu.wav", i);
        CHECK(std::filesystem::path(got[i]).filename() == name);
        const std::vector<unsigned char> b = ReadFile(got[i]);
        WavDoc doc;
        CHECK(doc.LoadFromMemory(b.data(), b.size()));
        CHECK(doc.SampleRate() == 16000 && doc.Channels() == 1);
        std::printf("segment %s: %zu samples\n", name, doc.Samples().size());
        CHECK(doc.Samples().size() == expect[i]);
    }
}

}  // namespace

int main() {
    TestResampler(48000);
    TestResampler(44100);
    TestUpsample();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("mep-audio-capture-test-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    TestWav(dir);
    TestSegments(dir);
    std::filesystem::remove_all(dir);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all audio capture tests passed\n");
    return 0;
}
