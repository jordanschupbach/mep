// mep-audio-capture-smoke: records from the default microphone through
// mep::audio::RecordSegments (the path speech-to-text uses) and prints
// each segment's stats. Needs a microphone and, on macOS, microphone
// permission for the app the binary runs under (the terminal).
//
//   mep-audio-capture-smoke [seconds=2] [out_dir=<tmp>/mep-audio-capture-smoke]

#include "audio_capture.h"
#include "wav_doc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 2.0;
    const std::string dir =
        argc > 2 ? argv[2] : (std::filesystem::temp_directory_path() / "mep-audio-capture-smoke").string();

    mep::audio::SegmentOptions opt;
    opt.dir = dir;
    opt.prefix = "smoke";
    opt.chunk_seconds = seconds;  // one segment for the whole recording
    std::vector<std::string> segments;
    mep::audio::RecordEvents ev;
    ev.on_segment = [&segments](const std::string &p) { segments.push_back(p); };
    ev.on_warning = [](const std::string &m) { std::fprintf(stderr, "warning: %s\n", m.c_str()); };

    std::atomic<bool> cancelled{false};
    std::thread stopper([&cancelled, seconds] {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0)));
        cancelled = true;
    });
    std::printf("recording %.1f s from the default input into %s ...\n", seconds, dir.c_str());
    std::string error;
    const int rc = mep::audio::RecordSegments(opt, ev, cancelled, &error);
    cancelled = true;
    stopper.join();
    if (rc != 0) {
        std::fprintf(stderr, "capture failed: %s\n", error.c_str());
        return 2;
    }
    if (segments.empty()) {
        std::fprintf(stderr, "no segment was written\n");
        return 3;
    }
    for (const std::string &path : segments) {
        std::ifstream f(path, std::ios::binary);
        const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        WavDoc doc;
        if (!doc.LoadFromMemory(bytes.data(), bytes.size())) {
            std::fprintf(stderr, "%s: invalid WAV: %s\n", path.c_str(), doc.Error().c_str());
            return 4;
        }
        const std::vector<int16_t> &s = doc.Samples();
        int peak = 0;
        double sum = 0;
        for (int16_t v : s) {
            peak = std::max(peak, std::abs(static_cast<int>(v)));
            sum += static_cast<double>(v) * v;
        }
        const double rms = s.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(s.size())) / 32768.0;
        const size_t frames = s.size() / static_cast<size_t>(std::max(1, doc.Channels()));
        std::printf("%s: %d Hz, %d ch, %zu frames (%.2f s), peak %.4f (%.1f dBFS), RMS %.4f (%.1f dBFS)\n",
                    path.c_str(), doc.SampleRate(), doc.Channels(), frames,
                    static_cast<double>(frames) / std::max(1, doc.SampleRate()), peak / 32768.0,
                    20.0 * std::log10(peak / 32768.0 + 1e-12), rms, 20.0 * std::log10(rms + 1e-12));
    }
    return 0;
}
