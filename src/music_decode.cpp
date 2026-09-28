#include "music_decode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "audio_decoders.h"
#include "wav_doc.h"

namespace {

std::string LowerExt(const std::string &path) {
    std::string::size_type dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

}  // namespace

bool DecodeAudioFile(const std::string &path, std::vector<int16_t> &samples, int &channels, int &rate, std::string &err) {
    samples.clear();
    const std::string ext = LowerExt(path);

    if (ext == "wav") {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) {
            err = "failed to open '" + path + "'";
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(f);
            err = "empty file '" + path + "'";
            return false;
        }
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        WavDoc wav;
        if (read != bytes.size() || !wav.LoadFromMemory(bytes.data(), bytes.size())) {
            err = wav.Error().empty() ? "wav decode failed" : wav.Error();
            return false;
        }
        samples = wav.Samples();
        channels = wav.Channels();
        rate = wav.SampleRate();
        return channels > 0 && rate > 0;
    }

    return audiodec::DecodeCompressedFile(path, samples, channels, rate, err);
}
