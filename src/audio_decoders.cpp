// Vendored-decoder implementations for audiodec::DecodeCompressedFile. This
// translation unit is the ONE place the dr_libs / stb_vorbis implementations
// are compiled (the DR_*_IMPLEMENTATION defines + the stb_vorbis.c body), and
// it is built into the mep_audiodec static library WITHOUT mep's strict
// -Werror flags, since these public-domain single-header decoders are not
// warning-clean under -Wall -Werror. Keep this file free of any mep_core
// dependency so the isolation holds.

#include "audio_decoders.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

// Disable the decoders' SSE/NEON paths: dr_flac's SSE2 support pulls in
// <emmintrin.h>/<mmintrin.h>, which under this repo's nix clang toolchain
// resolves to gfortran's GCC intrinsic headers and fails to compile (clang
// doesn't provide GCC's __builtin_ia32_* MMX builtins). Scalar decode is
// plenty fast for one-shot per-track loading, so we just turn SIMD off.
#define DR_MP3_NO_SIMD
#define DRFLAC_NO_SSE2
#define DRFLAC_NO_SSE41
#define DRFLAC_NO_NEON

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

// stb_vorbis ships as a single .c file carrying both declarations and the
// implementation; include the whole thing here (compiled as part of this TU)
// rather than adding it as its own source, so all vendored audio code lands
// in this one non-strict object.
#include "stb_vorbis.c"

namespace audiodec {
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

bool DecodeCompressedFile(const std::string &path, std::vector<int16_t> &out_samples, int &out_channels, int &out_rate,
                          std::string &err) {
    out_samples.clear();
    const std::string ext = LowerExt(path);

    if (ext == "mp3") {
        drmp3_config cfg = {};
        drmp3_uint64 frames = 0;
        drmp3_int16 *pcm = drmp3_open_file_and_read_pcm_frames_s16(path.c_str(), &cfg, &frames, nullptr);
        if (pcm == nullptr) {
            err = "mp3 decode failed";
            return false;
        }
        out_channels = static_cast<int>(cfg.channels);
        out_rate = static_cast<int>(cfg.sampleRate);
        out_samples.assign(pcm, pcm + frames * cfg.channels);
        drmp3_free(pcm, nullptr);
        return out_channels > 0 && out_rate > 0;
    }

    if (ext == "flac") {
        unsigned int channels = 0, rate = 0;
        drflac_uint64 frames = 0;
        drflac_int16 *pcm = drflac_open_file_and_read_pcm_frames_s16(path.c_str(), &channels, &rate, &frames, nullptr);
        if (pcm == nullptr) {
            err = "flac decode failed";
            return false;
        }
        out_channels = static_cast<int>(channels);
        out_rate = static_cast<int>(rate);
        out_samples.assign(pcm, pcm + frames * channels);
        drflac_free(pcm, nullptr);
        return out_channels > 0 && out_rate > 0;
    }

    if (ext == "ogg") {
        int channels = 0, rate = 0;
        short *pcm = nullptr;
        int frames = stb_vorbis_decode_filename(path.c_str(), &channels, &rate, &pcm);
        if (frames < 0 || pcm == nullptr) {
            err = "ogg decode failed";
            return false;
        }
        out_channels = channels;
        out_rate = rate;
        out_samples.assign(pcm, pcm + static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
        std::free(pcm);
        return out_channels > 0 && out_rate > 0;
    }

    err = "unsupported audio extension: " + ext;
    return false;
}

}  // namespace audiodec
