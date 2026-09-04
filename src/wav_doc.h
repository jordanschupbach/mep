#ifndef MEP_WAV_DOC_H
#define MEP_WAV_DOC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Minimal in-tree RIFF/WAVE decoder.  It intentionally starts with PCM16,
// which is enough to establish a license-free media pipeline and can be fed
// directly to raylib once the browser media player owns an audio device.
class WavDoc {
public:
    bool LoadFromMemory(const unsigned char *bytes, size_t length);
    int SampleRate() const { return sample_rate_; }
    int Channels() const { return channels_; }
    const std::vector<int16_t> &Samples() const { return samples_; }
    const std::string &Error() const { return error_; }
private:
    int sample_rate_ = 0, channels_ = 0;
    std::vector<int16_t> samples_;
    std::string error_;
};

#endif
