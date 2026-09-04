#include "wav_doc.h"

#include <cstring>

namespace {
uint16_t Read16(const unsigned char *p) { return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8U; }
uint32_t Read32(const unsigned char *p) { return static_cast<uint32_t>(Read16(p)) | static_cast<uint32_t>(Read16(p + 2)) << 16U; }
}

bool WavDoc::LoadFromMemory(const unsigned char *bytes, size_t length) {
    sample_rate_ = channels_ = 0; samples_.clear(); error_.clear();
    if (!bytes || length < 12 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0) { error_ = "not a RIFF/WAVE file"; return false; }
    bool format_seen = false; const unsigned char *audio = nullptr; size_t audio_size = 0;
    for (size_t offset = 12; offset + 8 <= length;) {
        const unsigned char *chunk = bytes + offset; uint32_t size = Read32(chunk + 4); offset += 8;
        if (size > length - offset) { error_ = "truncated WAVE chunk"; return false; }
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || Read16(bytes + offset) != 1 || Read16(bytes + offset + 14) != 16) { error_ = "only PCM16 WAVE is supported"; return false; }
            channels_ = static_cast<int>(Read16(bytes + offset + 2)); sample_rate_ = static_cast<int>(Read32(bytes + offset + 4)); format_seen = channels_ > 0 && sample_rate_ > 0;
        } else if (std::memcmp(chunk, "data", 4) == 0) { audio = bytes + offset; audio_size = size; }
        offset += size + (size & 1U);
    }
    if (!format_seen || !audio || audio_size % 2U != 0) { error_ = "missing WAVE format or data"; return false; }
    samples_.resize(audio_size / 2U); for (size_t i = 0; i < samples_.size(); ++i) samples_[i] = static_cast<int16_t>(Read16(audio + i * 2U));
    return true;
}
