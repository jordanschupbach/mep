#include "mp4_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "aac_decoder.h"
#include "h264_decoder.h"
#include "job.h"
#include "mp4_demux.h"

namespace mp4 {

namespace {

// Queue caps. A muxed file's thread may overrun one (up to kHardCap times
// it) while the other queue runs dry -- see the header comment.
constexpr size_t kMaxFrames = 24;
constexpr double kMaxAudioSec = 3.0;
constexpr size_t kHardCap = 4;
// A queue counts as starving below this: the clock is about to stop.
constexpr size_t kLowFrames = 2;
constexpr double kLowAudioSec = 0.5;

// How often a dropped transfer is restarted before the stream fails.
constexpr int kMaxRetries = 4;
constexpr int kCurlHttpError = 22;

class HttpSource final : public ByteSource {
public:
    HttpSource(std::string url, std::string user_agent, const std::atomic<bool> *cancel)
        : url_(std::move(url)), user_agent_(std::move(user_agent)), cancel_(cancel) {}

    bool Read(uint8_t *dst, size_t n) override {
        while (n > 0) {
            if (have_ > used_) {
                const size_t take = std::min(n, have_ - used_);
                std::memcpy(dst, buf_.data() + used_, take);
                used_ += take;
                offset_ += take;
                dst += take;
                n -= take;
                continue;
            }
            if (!Fill()) return false;
        }
        return true;
    }

    bool SeekTo(uint64_t offset) override {
        // Close ahead: read up to it rather than reconnect.
        if (offset >= offset_ && offset - offset_ <= (256u << 10) && job_) return Skip(offset - offset_);
        if (offset == offset_ && !job_) return true;
        job_.reset();
        buf_.clear();
        have_ = used_ = 0;
        offset_ = offset;
        return true;
    }

    uint64_t Offset() const override { return offset_; }
    std::string LastError() const override { return error_; }

    bool Skip(uint64_t n) override {
        // A long skip reconnects at the target instead of downloading the gap.
        if (n > (2u << 20)) return SeekTo(offset_ + n);
        while (n > 0) {
            if (have_ > used_) {
                const size_t take = static_cast<size_t>(std::min<uint64_t>(n, have_ - used_));
                used_ += take;
                offset_ += take;
                n -= take;
                continue;
            }
            if (!Fill()) return false;
        }
        return true;
    }

private:
    // Refills buf_ from the transfer (starting one at offset_ if none
    // runs), waiting for curl as needed. False at the end, on cancel, or
    // once the transfer has failed kMaxRetries times running.
    bool Fill() {
        buf_.clear();
        have_ = used_ = 0;
        for (;;) {
            if (cancel_ && cancel_->load()) return false;
            if (!job_) {
                job_ = std::make_unique<Job>(std::vector<std::string>{"curl", "-s", "-L", "--fail", "--connect-timeout", "15",
                                                                      "--speed-limit", "1", "--speed-time", "30", "-A", user_agent_,
                                                                      "-r", std::to_string(offset_) + "-", url_},
                                             "", /*raw_stdout=*/true);
                job_offset_ = offset_;
                if (job_->SpawnFailed()) {
                    job_.reset();
                    return false;
                }
            }
            for (std::string &chunk : job_->DrainRaw(1u << 20)) buf_ += chunk;
            if (!buf_.empty()) {
                have_ = buf_.size();
                retries_ = 0;
                return true;
            }
            if (job_->Finished()) {
                // Drained and done: the end of the resource, or a dropped
                // transfer to pick up where it stopped.
                if (job_->HasPendingRaw()) continue;
                const int code = job_->Killed() ? -1 : job_->ExitCode();
                const bool progressed = offset_ > job_offset_;
                job_.reset();
                if (code == 0) return false;
                // curl's 22 is an HTTP error status (--fail). Before any
                // data it is a refusal, not a dropped transfer: googlevideo
                // answers 403 for a URL it won't serve (an expired one, or
                // one that wants a PO token past its first few hundred KB).
                if (code == kCurlHttpError && !progressed) {
                    error_ = "YouTube refused the stream download at byte " + std::to_string(offset_) + " (HTTP error)";
                    return false;
                }
                if (progressed) retries_ = 0;
                if (++retries_ > kMaxRetries) {
                    error_ = "the stream download kept failing (curl exit " + std::to_string(code) + ")";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * retries_));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

    std::string url_, user_agent_;
    const std::atomic<bool> *cancel_ = nullptr;
    std::unique_ptr<Job> job_;
    uint64_t job_offset_ = 0;
    std::string buf_;
    size_t have_ = 0, used_ = 0;
    uint64_t offset_ = 0;
    int retries_ = 0;
    std::string error_;
};

std::unique_ptr<ByteSource> HttpFactory(const PlayerSource &s, const std::atomic<bool> *cancel) {
    return MakeHttpSource(s.url, s.user_agent, cancel);
}

}  // namespace

std::unique_ptr<ByteSource> MakeHttpSource(const std::string &url, const std::string &user_agent, const std::atomic<bool> *cancel) {
    return std::make_unique<HttpSource>(url, user_agent, cancel);
}

Player::~Player() { Stop(); }

void Player::Stop() {
    stop_ = true;
    room_.notify_all();
    for (std::thread &t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    stop_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    frames_.clear();
    audio_.clear();
    has_video_ = has_audio_ = false;
    video_eof_ = audio_eof_ = false;
    error_.clear();
    audio_rate_ = 0;
    audio_channels_ = 0;
}

void Player::Start(const PlayerSource &video, const PlayerSource &audio, double start_sec, SourceFactory source_factory) {
    Stop();
    SourceFactory factory = source_factory ? source_factory : &HttpFactory;
    {
        std::lock_guard<std::mutex> lk(mu_);
        has_video_ = !video.url.empty();
        has_audio_ = !audio.url.empty();
        video_eof_ = !has_video_;
        audio_eof_ = !has_audio_;
    }
    start_sec = std::max(0.0, start_sec);
    if (has_video_ && has_audio_ && video.url == audio.url) {
        threads_.emplace_back(&Player::Run, this, video, true, true, start_sec, factory);
        return;
    }
    if (has_video_) threads_.emplace_back(&Player::Run, this, video, true, false, start_sec, factory);
    if (has_audio_) threads_.emplace_back(&Player::Run, this, audio, false, true, start_sec, factory);
}

void Player::Fail(const std::string &error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (error_.empty()) error_ = error;
}

bool Player::Failed(std::string *error) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (error_.empty()) return false;
    if (error) *error = error_;
    return true;
}

bool Player::WaitForRoom(bool video, bool feeds_other) {
    std::unique_lock<std::mutex> lk(mu_);
    room_.wait(lk, [&] {
        if (stop_) return true;
        const int rate = audio_rate_.load(), channels = audio_channels_.load();
        const double audio_sec = rate > 0 && channels > 0 ? static_cast<double>(audio_.size()) / channels / rate : 0.0;
        const bool other_starving = feeds_other && (video ? audio_sec < kLowAudioSec : frames_.size() < kLowFrames);
        if (video) return frames_.size() < kMaxFrames || (other_starving && frames_.size() < kMaxFrames * kHardCap);
        return audio_sec < kMaxAudioSec || (other_starving && audio_sec < kMaxAudioSec * static_cast<double>(kHardCap));
    });
    return !stop_;
}

void Player::Run(PlayerSource source, bool want_video, bool want_audio, double start_sec, SourceFactory factory) {
    std::unique_ptr<ByteSource> src = factory(source, &stop_);
    Reader reader;
    std::string error;
    auto finish = [&] {
        std::lock_guard<std::mutex> lk(mu_);
        if (want_video) video_eof_ = true;
        if (want_audio) audio_eof_ = true;
    };
    // The source's own reason (a refused download) beats the demuxer's
    // view of the same failure ("no moov box", a truncated sample).
    auto why = [&](const std::string &demux_error) { return src && !src->LastError().empty() ? src->LastError() : demux_error; };
    if (!src || !reader.Open(src.get(), &error)) {
        if (!stop_) Fail(why(error.empty() ? "the stream could not be downloaded" : error));
        finish();
        return;
    }
    const Movie &movie = reader.movie();
    const int video_track = want_video ? movie.VideoTrack() : -1;
    const int audio_track = want_audio ? movie.AudioTrack() : -1;
    if (want_video && video_track < 0) Fail("the stream has no H.264 video");
    if (want_audio && audio_track < 0) Fail("the stream has no AAC audio");

    h264::Decoder video;
    aac::Decoder audio;
    bool video_ok = false, audio_ok = false;
    if (video_track >= 0) {
        const Track &t = movie.tracks[static_cast<size_t>(video_track)];
        video_ok = video.Configure(t.config.data(), t.config.size(), &error);
        if (!video_ok) Fail("video: " + error);
    }
    if (audio_track >= 0) {
        const Track &t = movie.tracks[static_cast<size_t>(audio_track)];
        audio_ok = audio.Configure(t.config.data(), t.config.size(), &error);
        if (audio_ok) {
            audio_rate_ = audio.SampleRate();
            audio_channels_ = audio.Channels();
        } else {
            Fail("audio: " + error);
        }
    }
    if (start_sec > 0.0 && !reader.Seek(start_sec, &error)) {
        if (!stop_) Fail(why(error));
        finish();
        return;
    }

    const bool feeds_both = video_ok && audio_ok;
    // A frame belongs to the start position once it would still be on
    // screen there; samples before it are cut off exactly.
    const double frame_slop = 0.5 / 30.0;
    Packet packet;
    std::vector<int16_t> pcm;
    h264::Picture picture;
    auto emit_pictures = [&]() -> bool {
        while (video.GetPicture(&picture)) {
            const double pts = static_cast<double>(picture.tag) / 1e6;
            if (pts + frame_slop < start_sec) continue;
            Frame f;
            f.pts = pts;
            f.width = picture.width;
            f.height = picture.height;
            f.rgba.resize(static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 4u);
            h264::ToRgba(picture, f.rgba.data());
            if (!WaitForRoom(true, feeds_both)) return false;
            std::lock_guard<std::mutex> lk(mu_);
            frames_.push_back(std::move(f));
        }
        return true;
    };
    while (!stop_) {
        if (!reader.Next(&packet, &error)) {
            if (!stop_ && !(error.empty() ? src->LastError() : error).empty()) Fail(why(error));
            break;
        }
        if (packet.track == video_track && video_ok) {
            const int64_t tag = static_cast<int64_t>(std::llround(packet.pts_sec * 1e6));
            video.Decode(packet.data.data(), packet.data.size(), tag, &error);  // a bad unit is skipped
            if (!emit_pictures()) break;
        } else if (packet.track == audio_track && audio_ok) {
            pcm.clear();
            if (!audio.Decode(packet.data.data(), packet.data.size(), &pcm, &error)) continue;
            const int channels = audio.Channels(), rate = audio.SampleRate();
            if (channels <= 0 || rate <= 0) continue;
            size_t skip = 0;
            if (packet.pts_sec < start_sec) {
                const double cut = (start_sec - packet.pts_sec) * rate;
                skip = std::min(pcm.size(), static_cast<size_t>(cut) * static_cast<size_t>(channels));
            }
            if (skip >= pcm.size()) continue;
            if (!WaitForRoom(false, feeds_both)) break;
            std::lock_guard<std::mutex> lk(mu_);
            audio_.insert(audio_.end(), pcm.begin() + static_cast<std::ptrdiff_t>(skip), pcm.end());
        }
    }
    if (video_ok && !stop_) {
        video.Flush();
        emit_pictures();
    }
    finish();
}

bool Player::PeekFrame(double *pts) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (frames_.empty()) return false;
    if (pts) *pts = frames_.front().pts;
    return true;
}

bool Player::PopFrame(std::vector<uint8_t> *rgba, int *width, int *height, double *pts) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (frames_.empty()) return false;
        Frame &f = frames_.front();
        if (rgba) *rgba = std::move(f.rgba);
        if (width) *width = f.width;
        if (height) *height = f.height;
        if (pts) *pts = f.pts;
        frames_.pop_front();
    }
    room_.notify_all();
    return true;
}

size_t Player::QueuedFrames() const {
    std::lock_guard<std::mutex> lk(mu_);
    return frames_.size();
}

size_t Player::TakeAudio(std::vector<int16_t> *out, size_t max_frames) {
    size_t frames = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const size_t channels = static_cast<size_t>(std::max(1, audio_channels_.load()));
        frames = std::min(max_frames, audio_.size() / channels);
        const size_t n = frames * channels;
        out->insert(out->end(), audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(n));
        audio_.erase(audio_.begin(), audio_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (frames > 0) room_.notify_all();
    return frames;
}

size_t Player::QueuedAudioFrames() const {
    std::lock_guard<std::mutex> lk(mu_);
    return audio_.size() / static_cast<size_t>(std::max(1, audio_channels_.load()));
}

bool Player::VideoDone() const {
    std::lock_guard<std::mutex> lk(mu_);
    return video_eof_ && frames_.empty();
}

bool Player::AudioDone() const {
    std::lock_guard<std::mutex> lk(mu_);
    return audio_eof_ && audio_.size() < static_cast<size_t>(std::max(1, audio_channels_.load()));
}

}  // namespace mp4
