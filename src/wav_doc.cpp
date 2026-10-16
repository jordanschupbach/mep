#include "wav_doc.h"

#include <cstdio>
#include <cstring>

namespace {
uint16_t Read16(const unsigned char *p) { return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8U)); }
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

namespace {
void Put16(std::vector<unsigned char> &out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFFU));
    out.push_back(static_cast<unsigned char>((v >> 8U) & 0xFFU));
}
void Put32(std::vector<unsigned char> &out, uint32_t v) {
    Put16(out, v & 0xFFFFU);
    Put16(out, v >> 16U);
}
void PutTag(std::vector<unsigned char> &out, const char *tag) { out.insert(out.end(), tag, tag + 4); }
}

std::vector<unsigned char> EncodeWavPcm16(const int16_t *samples, size_t count, int sample_rate, int channels) {
    const uint32_t data_bytes = static_cast<uint32_t>(count * 2U);
    const uint32_t ch = static_cast<uint32_t>(channels), rate = static_cast<uint32_t>(sample_rate);
    std::vector<unsigned char> out;
    out.reserve(44U + data_bytes);
    PutTag(out, "RIFF");
    Put32(out, 36U + data_bytes);
    PutTag(out, "WAVE");
    PutTag(out, "fmt ");
    Put32(out, 16U);
    Put16(out, 1U);  // PCM
    Put16(out, ch);
    Put32(out, rate);
    Put32(out, rate * ch * 2U);  // byte rate
    Put16(out, ch * 2U);         // block align
    Put16(out, 16U);             // bits per sample
    PutTag(out, "data");
    Put32(out, data_bytes);
    for (size_t i = 0; i < count; ++i) Put16(out, static_cast<uint16_t>(samples[i]));
    return out;
}

bool WriteWavPcm16(const std::string &path, const int16_t *samples, size_t count, int sample_rate, int channels,
                   std::string *error) {
    const std::vector<unsigned char> bytes = EncodeWavPcm16(samples, count, sample_rate, channels);
    const std::string tmp = path + ".part";
    std::FILE *f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot write " + tmp;
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        std::remove(tmp.c_str());
        if (error) *error = "short write to " + tmp;
        return false;
    }
    std::remove(path.c_str());  // Windows rename() does not replace
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        if (error) *error = "cannot rename " + tmp + " to " + path;
        return false;
    }
    return true;
}
