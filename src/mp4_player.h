#pragma once

// In-process MP4 playback for the YouTube pane: downloads a stream over
// HTTP (a `curl` range request per position -- HttpSource below), demuxes
// it as it arrives (mp4_demux.h) and decodes it on background threads with
// mep's own decoders (h264_decoder.h, aac_decoder.h). What comes out is
// what the pane's clock consumes: RGBA frames with their presentation
// times, and interleaved PCM16 starting exactly at the start position.
//
// One thread per distinct URL: a muxed file (YouTube's itag 18) is read
// once and feeds both queues; a separate video and audio URL (the adaptive
// formats) get a thread each. Each queue has a cap -- about a second of
// frames, a few seconds of audio -- and a thread waits when the queue its
// next sample belongs to is full, which pauses the download behind it (the
// pipe from curl fills and curl stops reading). A muxed thread may run a
// queue past its cap while the other one is starving, so a coarse
// interleave can't wedge both.
//
// Seeking is a fresh Start at the new position: the demuxer starts at the
// video sync sample before it, and frames/samples before the position are
// decoded but dropped.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp4 {

class ByteSource;

struct PlayerSource {
    std::string url;  // empty: no such track
    std::string user_agent;
};

/**
 * @brief A ByteSource reading an HTTP(S) URL through `curl -r <offset>-`, restarting the transfer to seek or when it drops.
 * @param url The resource.
 * @param user_agent What to send as User-Agent (googlevideo URLs are bound to the client that got them).
 * @param cancel Polled while waiting for data: once true, reads fail promptly.
 */
std::unique_ptr<ByteSource> MakeHttpSource(const std::string &url, const std::string &user_agent, const std::atomic<bool> *cancel);

class Player {
public:
    Player() = default;
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    /**
     * @brief Starts downloading and decoding from `start_sec` (stopping whatever ran before).
     * @param video The video stream; an empty url for none.
     * @param audio The audio stream; the same url as `video` for a muxed file, empty for none.
     * @param start_sec Where playback starts; earlier frames and samples are decoded but dropped.
     * @param source_factory How to open a URL (tests pass a file reader); null for HTTP.
     */
    using SourceFactory = std::unique_ptr<ByteSource> (*)(const PlayerSource &, const std::atomic<bool> *);
    void Start(const PlayerSource &video, const PlayerSource &audio, double start_sec, SourceFactory source_factory = nullptr);
    /** @brief Stops the threads and drops what they queued. */
    void Stop();

    /** @brief Whether a stream failed (download refused, not an MP4, undecodable); `error` says why. */
    bool Failed(std::string *error) const;
    /** @brief The audio format, once the audio track's setup is read (0 before, and without audio). */
    int AudioRate() const { return audio_rate_.load(); }
    int AudioChannels() const { return audio_channels_.load(); }

    /** @brief The oldest queued frame's presentation time; false when none is queued. */
    bool PeekFrame(double *pts) const;
    /** @brief Takes the oldest queued frame (`width` x `height` RGBA). */
    bool PopFrame(std::vector<uint8_t> *rgba, int *width, int *height, double *pts);
    size_t QueuedFrames() const;

    /** @brief Appends up to `max_frames` frames of queued PCM (interleaved int16) to `out`; returns how many frames. */
    size_t TakeAudio(std::vector<int16_t> *out, size_t max_frames);
    size_t QueuedAudioFrames() const;

    /** @brief True once the video (audio) has been decoded to its end -- or there is none -- and its queue is empty. */
    bool VideoDone() const;
    bool AudioDone() const;

private:
    struct Frame {
        double pts = 0.0;
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    void Run(PlayerSource source, bool want_video, bool want_audio, double start_sec, SourceFactory factory);
    // Waits until the queue a sample of this kind goes to has room; false when stopping.
    bool WaitForRoom(bool video, bool feeds_other);
    void Fail(const std::string &error);

    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::condition_variable room_;
    std::deque<Frame> frames_;
    std::deque<int16_t> audio_;
    std::atomic<int> audio_rate_{0}, audio_channels_{0};
    bool has_video_ = false, has_audio_ = false;
    bool video_eof_ = false, audio_eof_ = false;
    std::string error_;
};

}  // namespace mp4
