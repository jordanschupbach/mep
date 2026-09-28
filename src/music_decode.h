#pragma once

// Unified "decode any supported audio file to interleaved PCM16" entry point
// for the music-player pane. Dispatches .wav to the in-tree WavDoc decoder
// (src/wav_doc.h) and .mp3/.flac/.ogg to the vendored decoders behind
// audiodec::DecodeCompressedFile (src/audio_decoders.h). Lives in mep_core
// (strict-compiled) -- it only references clean headers, never the vendored
// single-header implementations directly.

#include <cstdint>
#include <string>
#include <vector>

// Decode an entire audio file (.wav/.mp3/.flac/.ogg) to interleaved signed
// 16-bit PCM. On success fills samples/channels/rate and returns true; on
// failure sets `err` and returns false.
bool DecodeAudioFile(const std::string &path, std::vector<int16_t> &samples, int &channels, int &rate, std::string &err);
