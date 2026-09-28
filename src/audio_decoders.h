#pragma once

// Thin wrapper over the vendored single-header audio decoders (dr_mp3,
// dr_flac, stb_vorbis) -- see src/audio_decoders.cpp. Kept in its own
// static library (mep_audiodec, CMakeLists.txt) compiled WITHOUT mep's
// -Werror/-Wall strict flags, because those third-party decoders are not
// warning-clean; this header itself stays clean so mep_core can include it.
// The .wav case is handled separately by WavDoc (src/wav_doc.h); this only
// covers the compressed formats.

#include <cstdint>
#include <string>
#include <vector>

namespace audiodec {

// Decode an entire compressed audio file to interleaved signed-16-bit PCM,
// dispatched on the lowercased extension of `path` (.mp3 / .flac / .ogg).
// On success fills out_samples/out_channels/out_rate and returns true; on
// any failure (unknown extension, open/parse error) sets `err` and returns
// false.
bool DecodeCompressedFile(const std::string &path, std::vector<int16_t> &out_samples, int &out_channels, int &out_rate,
                          std::string &err);

}  // namespace audiodec
