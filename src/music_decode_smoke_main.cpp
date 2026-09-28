// Headless smoke test for DecodeAudioFile (music_decode.h): decodes each
// audio file passed on the command line and prints frames/channels/rate,
// exiting non-zero if any decode fails or yields no samples. No audio device
// is touched -- this only exercises the decoders. Built as the
// mep-music-decode-smoke target (CMakeLists.txt).

#include <cstdio>
#include <string>
#include <vector>

#include "music_decode.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <audio-file>...\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        std::vector<int16_t> samples;
        int channels = 0, rate = 0;
        std::string err;
        if (!DecodeAudioFile(argv[i], samples, channels, rate, err)) {
            std::fprintf(stderr, "FAIL  %s: %s\n", argv[i], err.c_str());
            failures++;
            continue;
        }
        if (samples.empty() || channels <= 0 || rate <= 0) {
            std::fprintf(stderr, "FAIL  %s: decoded empty/invalid (samples=%zu ch=%d rate=%d)\n", argv[i], samples.size(),
                         channels, rate);
            failures++;
            continue;
        }
        double seconds = static_cast<double>(samples.size() / static_cast<size_t>(channels)) / rate;
        std::printf("OK    %s: %zu samples, %d ch, %d Hz, %.2fs\n", argv[i], samples.size(), channels, rate, seconds);
    }
    return failures == 0 ? 0 : 1;
}
