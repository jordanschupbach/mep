#ifndef MEP_AUDIO_CAPTURE_H
#define MEP_AUDIO_CAPTURE_H

// In-house microphone capture (DEPENDENCY_REMOVAL_PLAN.md Phase 5): what
// speech-to-text used to run `ffmpeg -f pulse ... -f segment` for.
//
//   Capture         default input device -> interleaved float frames on a
//                   backend thread. macOS: AudioQueue input (AudioToolbox)
//                   plus the AVFoundation microphone-permission check
//                   (audio_capture_macos.mm). Windows: WASAPI shared mode.
//                   Linux: ALSA snd_pcm capture (until libasound goes).
//                   Emscripten: always fails with an error.
//   Resampler       streaming Kaiser-windowed-sinc sample-rate converter.
//   SegmentWriter   mono float in, 16-bit WAV files of N seconds out, named
//                   <dir>/<prefix>_000.wav, _001.wav, ... -- the same names
//                   ffmpeg's segment muxer produced for the STT flow.
//   RecordSegments  Capture + SegmentWriter, blocking, for a Job task.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mep::audio {

class Capture {
public:
    // `frames` frames of Channels() interleaved samples in [-1, 1], called
    // on a backend thread. Never called again once Stop() has returned.
    using FrameCallback = std::function<void(const float *interleaved, size_t frames)>;

    Capture();
    ~Capture();
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

    // Opens the default input device and starts delivering frames. On
    // failure (no device, microphone permission denied, ...) returns false
    // with a user-facing message in *error.
    bool Start(FrameCallback callback, std::string *error);
    // Stops and closes the device; blocks until no callback is running.
    void Stop();
    // The format frames are delivered in; valid after a successful Start.
    int SampleRate() const;
    int Channels() const;
    // True once the device failed after a successful Start (unplugged,
    // stream error); *error gets the reason.
    bool Failed(std::string *error) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Streaming band-limited resampler for one channel. Each output sample is
// a Kaiser-windowed sinc (beta 9, 32 zero crossings each side) evaluated
// from a finely tabulated kernel, with the cutoff at 0.9 x the lower
// Nyquist, so downsampling does not alias. The filter's delay is
// compensated: output sample k corresponds to input time k * in/out, and
// after Flush() exactly round(inputs * out/in) samples have come out.
class Resampler {
public:
    Resampler(int in_rate, int out_rate);
    void Process(const float *in, size_t count, std::vector<float> &out);
    // Ends the stream: emits the samples still held back by the filter.
    void Flush(std::vector<float> &out);
    int InRate() const { return in_rate_; }
    int OutRate() const { return out_rate_; }

private:
    void Produce(std::vector<float> &out, uint64_t limit);
    int in_rate_, out_rate_;
    double step_;     // input samples per output sample
    double cutoff_;   // 2 * fc, fc in cycles per input sample
    int half_width_;  // filter half length, in input samples
    std::vector<float> table_;
    std::vector<float> buf_;  // input history; buf_[0] is input index base_
    long long base_ = 0;      // may start negative (the zero-padded lead-in)
    uint64_t total_in_ = 0, total_out_ = 0;
};

// Converts float to int16 with clipping and dither-free rounding.
int16_t FloatToPcm16(float v);

struct SegmentOptions {
    std::string dir;              // created if missing
    std::string prefix = "chunk"; // files are <dir>/<prefix>_%03d.wav
    double chunk_seconds = 4.0;
    int out_rate = 16000;
    // A final partial segment shorter than this is dropped rather than
    // written (Whisper rejects audio under 0.1 s).
    double min_final_seconds = 0.1;
};

class SegmentWriter {
public:
    using SegmentReady = std::function<void(const std::string &path)>;
    SegmentWriter(SegmentOptions options, int in_rate, SegmentReady ready);
    // Mono input at in_rate. Returns false (with *error) if a file write
    // failed.
    bool Push(const float *mono, size_t count, std::string *error);
    // Flushes the resampler and writes the final partial segment.
    bool Finish(std::string *error);
    int SegmentsWritten() const { return index_; }
    std::string SegmentPath(int index) const;

private:
    bool WriteSegment(const int16_t *samples, size_t count, std::string *error);
    SegmentOptions opt_;
    Resampler resampler_;
    SegmentReady ready_;
    size_t chunk_samples_;
    std::vector<float> scratch_;
    std::vector<int16_t> pending_;
    int index_ = 0;
};

struct RecordEvents {
    std::function<void(const std::string &path)> on_segment;
    // Non-fatal problems, e.g. the input is pure digital silence (what a
    // muted device or a silently refused permission produces).
    std::function<void(const std::string &message)> on_warning;
};

// Records from the default input device into rolling segment files until
// `cancelled` turns true, then writes the final partial segment and
// returns 0. On failure returns non-zero with a message in *error.
int RecordSegments(const SegmentOptions &options, const RecordEvents &events, const std::atomic<bool> &cancelled,
                   std::string *error);

}  // namespace mep::audio

#endif
