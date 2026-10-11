// NativeAudioBackend: in-house audio for Stage B's native backend (see
// MINIAUDIO_REMOVAL_PLAN.md for the full removal writeup). Replaces
// third_party/miniaudio.h with a small ALSA playback backend + the
// existing in-tree WavDoc (wav_doc.h) decoder -- confirmed sufficient
// for every real call site: main.cpp's <audio>/<video> element bridge
// (HtmlMediaPlayback) only ever plays PCM16 WAV in practice (the html-
// doc-test fixture, and mep's own speech-to-text debug recordings via
// `ffmpeg -ar 16000 -ac 1`, both PCM16), which WavDoc already parses.
//
// Scope: Linux/ALSA and macOS/AudioToolbox. PulseAudio/PipeWire users
// are covered transparently via their ALSA compatibility shim, the same
// assumption raylib's own miniaudio-based ALSA path already made. On
// macOS each Sound/stream is an AudioQueue output in its own PCM16
// format (the queue converts to the device's rate itself); AudioToolbox
// is used for output only -- every decode stays in-house. Windows is a
// graceful no-op (InitAudioDevice leaves `ready = false`, matching how a
// real device-open failure already had to be handled) -- documented
// future work; see the plan's own Scoping decision 3.
//
// Design: each Sound owns its own ALSA PCM handle, opened lazily on
// first Play and configured for that WAV's own sample rate/channel
// count, with a small background thread writing frames until stopped or
// finished. No cross-sound mixing -- mep never plays more than one
// <audio>/<video> element's sound at a time in practice, so this avoids
// the real complexity of a shared-device software mixer for no current
// benefit. Software volume gain is applied per-buffer before each
// `snd_pcm_writei` call.

#include "gfx/backend_native_internal.h"

#include "wav_doc.h"

#include <cstdio>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32) && !defined(__APPLE__)
#define MEP_AUDIO_ALSA 1
#endif
#if !defined(__EMSCRIPTEN__) && defined(__APPLE__)
#define MEP_AUDIO_COREAUDIO 1
#endif

#if MEP_AUDIO_COREAUDIO
#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#endif

#if MEP_AUDIO_ALSA
#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#endif

namespace gfx {

#if MEP_AUDIO_ALSA

namespace {

// Runs on SoundState::thread: owns the PCM device for as long as
// playback is active, writing frames from `samples` until `stop`,
// finished, or a fatal ALSA error. Pausing (see PauseSound) just stops
// feeding the device -- the stream naturally underruns and goes silent,
// which is harmless for mep's own play/pause/resume usage (a media
// player's transport controls, not a low-latency real-time pipeline).
struct SoundState {
    std::vector<int16_t> samples;  // interleaved PCM16
    int channels = 0;
    int rate = 0;

    std::mutex control_mutex;  // serializes Play/Pause/Resume/Unload against each other and the thread's own exit
    std::thread thread;
    std::atomic<bool> thread_running{false};  // the playback thread is alive (still owns the PCM device)
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> paused{false};
    std::atomic<size_t> frame_pos{0};  // next frame index to write; read back by PlaySound's restart-from-0 reset
    std::atomic<float> volume{1.0f};
};

size_t TotalFrames(const SoundState &s) { return s.channels > 0 ? s.samples.size() / static_cast<size_t>(s.channels) : 0; }

void PlaybackThreadMain(SoundState *s) {
    snd_pcm_t *pcm = nullptr;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        s->thread_running = false;
        return;
    }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, static_cast<unsigned int>(s->channels),
                            static_cast<unsigned int>(s->rate), 1 /*allow resample*/, 200000 /*100ms-class latency, matches raylib/miniaudio's own default ballpark*/) < 0) {
        snd_pcm_close(pcm);
        s->thread_running = false;
        return;
    }

    const size_t total_frames = TotalFrames(*s);
    std::vector<int16_t> chunk;
    const snd_pcm_uframes_t kChunkFrames = 1024;
    chunk.resize(kChunkFrames * static_cast<size_t>(s->channels));

    while (!s->stop_requested.load()) {
        if (s->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        size_t pos = s->frame_pos.load();
        if (pos >= total_frames) break;  // finished playing
        size_t frames_left = total_frames - pos;
        snd_pcm_uframes_t frames_this_write = static_cast<snd_pcm_uframes_t>(std::min<size_t>(kChunkFrames, frames_left));
        const float vol = s->volume.load();
        const int16_t *src = s->samples.data() + pos * static_cast<size_t>(s->channels);
        size_t sample_count = frames_this_write * static_cast<size_t>(s->channels);
        for (size_t i = 0; i < sample_count; i++) {
            float v = static_cast<float>(src[i]) * vol;
            v = std::clamp(v, -32768.0f, 32767.0f);
            chunk[i] = static_cast<int16_t>(v);
        }
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, chunk.data(), frames_this_write);
        if (written < 0) {
            written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
            if (written < 0) break;  // unrecoverable device error -- stop, matching a failed LoadSound's silent-fail style
            continue;
        }
        s->frame_pos.fetch_add(static_cast<size_t>(written));
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    s->thread_running = false;
}

// Stops and joins the playback thread if one is running -- shared by
// PlaySound (restart) and UnloadSound (teardown). Caller holds
// `s->control_mutex`.
void StopAndJoin(SoundState *s) {
    if (!s->thread_running.load() && !s->thread.joinable()) return;
    s->stop_requested = true;
    if (s->thread.joinable()) s->thread.join();
    s->stop_requested = false;
}

// A push-fed stream (IAudioBackend::OpenAudioStream): the main thread
// appends decoded PCM16 as it arrives from a decoder pipe, the playback
// thread drains it in 1024-frame chunks. Starvation (queue empty -- the
// network hiccuped, or the decoder is still seeking) writes silence so
// the device stays open and the next real chunk plays without a
// reopen; that silence is tracked separately so the played-seconds
// clock (which a video track syncs to) only counts real audio.
struct StreamState {
    int channels = 0;
    int rate = 0;

    std::mutex queue_mutex;
    std::vector<int16_t> queue;  // interleaved PCM16, consumed from `read_pos`
    size_t read_pos = 0;

    std::thread thread;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> paused{false};
    std::atomic<float> volume{1.0f};
    std::atomic<size_t> frames_written{0};        // everything handed to snd_pcm_writei
    std::atomic<size_t> silence_written{0};       // the starvation subset of frames_written
    std::atomic<long> device_delay_frames{0};     // snd_pcm_delay after the last write
    std::atomic<bool> device_failed{false};
    double last_clock = 0.0;  // AudioStreamPlayedSeconds' monotonic floor (main thread only)
};

void StreamThreadMain(StreamState *s) {
    snd_pcm_t *pcm = nullptr;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        s->device_failed = true;
        return;
    }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, static_cast<unsigned int>(s->channels),
                            static_cast<unsigned int>(s->rate), 1 /*allow resample*/, 200000 /*same 200ms class as SoundState*/) < 0) {
        snd_pcm_close(pcm);
        s->device_failed = true;
        return;
    }
    const snd_pcm_uframes_t kChunkFrames = 1024;
    const size_t ch = static_cast<size_t>(s->channels);
    std::vector<int16_t> chunk(kChunkFrames * ch);
    while (!s->stop_requested.load()) {
        if (s->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        size_t frames_this_write = 0;
        bool silence = false;
        {
            std::lock_guard<std::mutex> lock(s->queue_mutex);
            size_t avail_samples = s->queue.size() - s->read_pos;
            size_t avail_frames = avail_samples / ch;
            frames_this_write = std::min<size_t>(kChunkFrames, avail_frames);
            if (frames_this_write > 0) {
                const float vol = s->volume.load();
                const int16_t *src = s->queue.data() + s->read_pos;
                size_t n = frames_this_write * ch;
                for (size_t i = 0; i < n; i++) {
                    float v = static_cast<float>(src[i]) * vol;
                    v = std::clamp(v, -32768.0f, 32767.0f);
                    chunk[i] = static_cast<int16_t>(v);
                }
                s->read_pos += n;
                // Compact once the consumed prefix dominates, so the queue
                // never grows past roughly twice what's buffered.
                if (s->read_pos > (1u << 20) && s->read_pos * 2 > s->queue.size()) {
                    s->queue.erase(s->queue.begin(), s->queue.begin() + static_cast<std::ptrdiff_t>(s->read_pos));
                    s->read_pos = 0;
                }
            }
        }
        if (frames_this_write == 0) {
            // Starved: a short run of silence keeps the device fed without
            // delaying the next real chunk by more than its own length.
            frames_this_write = 256;
            std::fill(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(frames_this_write * ch), int16_t{0});
            silence = true;
        }
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, chunk.data(), static_cast<snd_pcm_uframes_t>(frames_this_write));
        if (written < 0) {
            written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
            if (written < 0) {
                s->device_failed = true;
                break;
            }
            continue;
        }
        s->frames_written.fetch_add(static_cast<size_t>(written));
        if (silence) s->silence_written.fetch_add(static_cast<size_t>(written));
        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm, &delay) == 0 && delay >= 0) s->device_delay_frames = static_cast<long>(delay);
    }
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
}

}  // namespace

struct NativeAudioBackend::Impl {
    bool ready = false;
};

NativeAudioBackend::NativeAudioBackend() : impl_(new Impl()) {}
NativeAudioBackend::~NativeAudioBackend() { delete impl_; }

void NativeAudioBackend::InitAudioDevice() {
    if (impl_->ready) return;
    // Probe the default device (PulseAudio/PipeWire's ALSA compat shim,
    // or real hardware) once at init time, matching miniaudio's own
    // ma_engine_init failure semantics -- LoadSound/PlaySound don't
    // re-probe per call.
    snd_pcm_t *probe = nullptr;
    if (snd_pcm_open(&probe, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        std::fprintf(stderr, "gfx native: no ALSA playback device available\n");
        return;
    }
    snd_pcm_close(probe);
    impl_->ready = true;
}

bool NativeAudioBackend::IsAudioDeviceReady() { return impl_->ready; }

gfx::Sound NativeAudioBackend::LoadSound(const char *file_name) {
    gfx::Sound out{};
    if (!impl_->ready) return out;
    WavDoc wav;
    FILE *f = std::fopen(file_name, "rb");
    if (!f) {
        std::fprintf(stderr, "gfx native: failed to open sound '%s'\n", file_name);
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return out;
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (read != bytes.size() || !wav.LoadFromMemory(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "gfx native: failed to load sound '%s'%s%s\n", file_name, wav.Error().empty() ? "" : ": ",
                     wav.Error().c_str());
        return out;
    }
    auto *s = new SoundState();
    s->samples = wav.Samples();
    s->channels = wav.Channels();
    s->rate = wav.SampleRate();
    out.backend_handle = s;
    out.frameCount = static_cast<unsigned int>(TotalFrames(*s));
    return out;
}

gfx::Sound NativeAudioBackend::LoadSoundFromPcm(const int16_t *samples, size_t count, int channels, int rate) {
    gfx::Sound out{};
    if (!impl_->ready || samples == nullptr || count == 0 || channels <= 0 || rate <= 0) return out;
    auto *s = new SoundState();
    s->samples.assign(samples, samples + count);
    s->channels = channels;
    s->rate = rate;
    out.backend_handle = s;
    out.frameCount = static_cast<unsigned int>(TotalFrames(*s));
    return out;
}

void NativeAudioBackend::UnloadSound(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    {
        std::lock_guard<std::mutex> lock(s->control_mutex);
        StopAndJoin(s);
    }
    delete s;
}

void NativeAudioBackend::PlaySound(gfx::Sound sound) {
    // raylib's PlaySound always (re)starts from the beginning, unlike
    // ResumeSound -- see main.cpp's own SyncHtmlMediaPlayback comment on
    // this exact distinction, preserved here.
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    std::lock_guard<std::mutex> lock(s->control_mutex);
    StopAndJoin(s);
    s->frame_pos = 0;
    s->paused = false;
    s->thread_running = true;
    s->thread = std::thread(PlaybackThreadMain, s);
}

void NativeAudioBackend::PauseSound(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return;
    static_cast<SoundState *>(sound.backend_handle)->paused = true;
}

void NativeAudioBackend::ResumeSound(gfx::Sound sound) {
    // Mirrors miniaudio's ma_sound_stop/ma_sound_start distinction: pause
    // leaves the playback cursor (frame_pos) where it was, no implicit
    // seek, so clearing `paused` here resumes rather than restarts. If
    // playback already finished (thread exited), there's nothing to
    // resume -- matches ma_sound_start's own no-op-at-end-of-stream feel
    // closely enough for mep's actual play/pause/resume usage.
    if (sound.backend_handle == nullptr) return;
    static_cast<SoundState *>(sound.backend_handle)->paused = false;
}

bool NativeAudioBackend::IsSoundPlaying(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return false;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    return s->thread_running.load() && !s->paused.load();
}

void NativeAudioBackend::SetSoundVolume(gfx::Sound sound, float volume) {
    if (sound.backend_handle == nullptr) return;
    static_cast<SoundState *>(sound.backend_handle)->volume = volume;
}

double NativeAudioBackend::GetSoundTimePlayed(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return 0.0;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    if (s->rate <= 0) return 0.0;
    return static_cast<double>(s->frame_pos.load()) / static_cast<double>(s->rate);
}

gfx::AudioStream NativeAudioBackend::OpenAudioStream(int channels, int rate) {
    gfx::AudioStream out{};
    if (!impl_->ready || channels <= 0 || rate <= 0) return out;
    auto *s = new StreamState();
    s->channels = channels;
    s->rate = rate;
    s->queue.reserve(static_cast<size_t>(rate * channels) * 2);
    s->thread = std::thread(StreamThreadMain, s);
    out.backend_handle = s;
    return out;
}

void NativeAudioBackend::CloseAudioStream(gfx::AudioStream stream) {
    if (stream.backend_handle == nullptr) return;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    s->stop_requested = true;
    if (s->thread.joinable()) s->thread.join();
    delete s;
}

void NativeAudioBackend::PushAudioStream(gfx::AudioStream stream, const int16_t *samples, size_t count) {
    if (stream.backend_handle == nullptr || samples == nullptr || count == 0) return;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    std::lock_guard<std::mutex> lock(s->queue_mutex);
    s->queue.insert(s->queue.end(), samples, samples + count);
}

size_t NativeAudioBackend::AudioStreamQueuedFrames(gfx::AudioStream stream) {
    if (stream.backend_handle == nullptr) return 0;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    std::lock_guard<std::mutex> lock(s->queue_mutex);
    return (s->queue.size() - s->read_pos) / static_cast<size_t>(s->channels);
}

double NativeAudioBackend::AudioStreamPlayedSeconds(gfx::AudioStream stream) {
    if (stream.backend_handle == nullptr) return 0.0;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    if (s->device_failed.load()) return s->last_clock;
    double written = static_cast<double>(s->frames_written.load());
    double silence = static_cast<double>(s->silence_written.load());
    double delay = static_cast<double>(s->device_delay_frames.load());
    double heard = written - delay - silence;
    double secs = std::max(0.0, heard) / static_cast<double>(s->rate);
    if (secs > s->last_clock) s->last_clock = secs;
    return s->last_clock;
}

void NativeAudioBackend::SetAudioStreamPaused(gfx::AudioStream stream, bool paused) {
    if (stream.backend_handle == nullptr) return;
    static_cast<StreamState *>(stream.backend_handle)->paused = paused;
}

void NativeAudioBackend::SetAudioStreamVolume(gfx::AudioStream stream, float volume) {
    if (stream.backend_handle == nullptr) return;
    static_cast<StreamState *>(stream.backend_handle)->volume = std::clamp(volume, 0.0f, 2.0f);
}

#elif MEP_AUDIO_COREAUDIO

namespace {

// Three buffers in flight per queue, each this many frames: ~46ms apiece
// at 44.1kHz, so a few hundred ms of latency at most, the same class as
// the ALSA path's 200ms.
constexpr UInt32 kQueueBuffers = 3;
constexpr UInt32 kBufferFrames = 2048;
// A starved stream's filler buffer: short, so real audio arriving later
// waits behind no more than this much silence (ALSA path: 256 frames).
constexpr UInt32 kSilenceFrames = 512;

AudioStreamBasicDescription Pcm16Format(int channels, int rate) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = static_cast<Float64>(rate);
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = static_cast<UInt32>(channels);
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 2u * static_cast<UInt32>(channels);
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;
    return f;
}

// `n` samples of `src` into `dst` with software gain, as the ALSA path does.
void CopyWithGain(int16_t *dst, const int16_t *src, size_t n, float vol) {
    for (size_t i = 0; i < n; i++) {
        float v = static_cast<float>(src[i]) * vol;
        v = std::clamp(v, -32768.0f, 32767.0f);
        dst[i] = static_cast<int16_t>(v);
    }
}

// One Sound: the whole clip in memory, fed to its queue from frame_pos
// until it runs out. Pausing pauses the queue (its clock stops with it);
// the clip ending stops the queue once what was enqueued has played, and
// the IsRunning listener then marks it finished.
struct SoundState {
    std::vector<int16_t> samples;  // interleaved PCM16
    int channels = 0;
    int rate = 0;

    std::mutex control_mutex;  // serializes Play/Pause/Resume/Unload
    AudioQueueRef queue = nullptr;
    std::atomic<bool> running{false};   // playing or paused, not yet finished
    std::atomic<bool> paused{false};
    std::atomic<bool> stopping{false};  // out of data: AudioQueueStop(async) issued
    std::atomic<size_t> frame_pos{0};   // next frame to enqueue
    std::atomic<float> volume{1.0f};
};

size_t TotalFrames(const SoundState &s) { return s.channels > 0 ? s.samples.size() / static_cast<size_t>(s.channels) : 0; }

void SoundFill(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    auto *s = static_cast<SoundState *>(user);
    if (s->stopping.load()) return;
    const size_t total = TotalFrames(*s);
    const size_t pos = s->frame_pos.load();
    if (pos >= total) {
        // Everything is enqueued; let it play out, then the queue stops.
        s->stopping = true;
        AudioQueueStop(q, false);
        return;
    }
    const size_t frames = std::min<size_t>(kBufferFrames, total - pos);
    const size_t ch = static_cast<size_t>(s->channels);
    CopyWithGain(static_cast<int16_t *>(buf->mAudioData), s->samples.data() + pos * ch, frames * ch, s->volume.load());
    buf->mAudioDataByteSize = static_cast<UInt32>(frames * ch * 2u);
    if (AudioQueueEnqueueBuffer(q, buf, 0, nullptr) == noErr) s->frame_pos.fetch_add(frames);
}

void SoundRunningChanged(void *user, AudioQueueRef q, AudioQueuePropertyID) {
    auto *s = static_cast<SoundState *>(user);
    UInt32 is_running = 0;
    UInt32 size = sizeof(is_running);
    if (AudioQueueGetProperty(q, kAudioQueueProperty_IsRunning, &is_running, &size) == noErr && !is_running) s->running = false;
}

// Disposes the queue (synchronously: no callback runs after this), shared
// by PlaySound (restart) and UnloadSound. Caller holds control_mutex.
void DisposeQueue(SoundState *s) {
    if (s->queue) AudioQueueDispose(s->queue, true);
    s->queue = nullptr;
    s->running = false;
}

// A push-fed stream (IAudioBackend::OpenAudioStream): the main thread
// appends decoded PCM16 as it arrives, the queue's callback drains it a
// buffer at a time and fills a starved buffer's tail -- or, with nothing
// queued at all, a short buffer -- with silence so the queue keeps going.
// Every enqueued buffer is logged with where it starts on the queue's
// own timeline and how many of its frames are real, so the played-
// seconds clock (which a video track syncs to) counts only real audio
// that the hardware has actually played: AudioQueueGetCurrentTime is the
// device-side position, output latency included, and it stands still
// while the queue is paused.
struct StreamState {
    int channels = 0;
    int rate = 0;
    AudioQueueRef queue = nullptr;

    std::mutex queue_mutex;  // guards queue_pcm/read_pos and the buffer log
    std::vector<int16_t> queue_pcm;  // interleaved PCM16, consumed from `read_pos`
    size_t read_pos = 0;
    struct Logged {
        double start = 0.0;    // queue-timeline frame of its first frame
        size_t frames = 0;     // all frames in it
        size_t real = 0;       // the leading real (non-silence) ones
        double real_before = 0.0;  // real frames in every earlier buffer
    };
    std::deque<Logged> log;
    double timeline_end = 0.0;  // where the next enqueued buffer starts
    double real_total = 0.0;

    std::atomic<bool> paused{false};
    std::atomic<float> volume{1.0f};
    bool started = false;      // AudioQueueStart has run (main thread only)
    double last_clock = 0.0;   // AudioStreamPlayedSeconds' monotonic floor (main thread only)
};

void StreamFill(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    auto *s = static_cast<StreamState *>(user);
    const size_t ch = static_cast<size_t>(s->channels);
    auto *dst = static_cast<int16_t *>(buf->mAudioData);
    size_t real = 0, frames = 0;
    {
        std::lock_guard<std::mutex> lock(s->queue_mutex);
        const size_t avail = (s->queue_pcm.size() - s->read_pos) / ch;
        real = std::min<size_t>(kBufferFrames, avail);
        if (real > 0) {
            CopyWithGain(dst, s->queue_pcm.data() + s->read_pos, real * ch, s->volume.load());
            s->read_pos += real * ch;
            // Compact once the consumed prefix dominates (as the ALSA path).
            if (s->read_pos > (1u << 20) && s->read_pos * 2 > s->queue_pcm.size()) {
                s->queue_pcm.erase(s->queue_pcm.begin(), s->queue_pcm.begin() + static_cast<std::ptrdiff_t>(s->read_pos));
                s->read_pos = 0;
            }
        }
        // A partly-filled buffer is topped up with silence to a full one
        // (it ran dry mid-buffer); an empty one becomes a short silence.
        frames = real > 0 ? kBufferFrames : kSilenceFrames;
        std::fill(dst + real * ch, dst + frames * ch, int16_t{0});
        StreamState::Logged entry;
        entry.start = s->timeline_end;
        entry.frames = frames;
        entry.real = real;
        entry.real_before = s->real_total;
        s->log.push_back(entry);
        s->timeline_end += static_cast<double>(frames);
        s->real_total += static_cast<double>(real);
    }
    buf->mAudioDataByteSize = static_cast<UInt32>(frames * ch * 2u);
    AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
}

// Real frames played by queue-timeline position `t`. Drops log entries
// wholly behind `t` (keeping the newest, so `real_before` stays known).
double RealFramesPlayed(StreamState *s, double t) {
    std::lock_guard<std::mutex> lock(s->queue_mutex);
    while (s->log.size() > 1 && s->log[1].start <= t) s->log.pop_front();
    if (s->log.empty()) return s->real_total;
    const StreamState::Logged &e = s->log.front();
    if (t <= e.start) return e.real_before;
    return e.real_before + std::min(static_cast<double>(e.real), t - e.start);
}

}  // namespace

struct NativeAudioBackend::Impl {
    bool ready = false;
};

NativeAudioBackend::NativeAudioBackend() : impl_(new Impl()) {}
NativeAudioBackend::~NativeAudioBackend() { delete impl_; }

void NativeAudioBackend::InitAudioDevice() {
    if (impl_->ready) return;
    // Probe once that an output queue can be made at all (no output
    // device, or audio services unavailable), as the ALSA path probes
    // its default device.
    AudioStreamBasicDescription f = Pcm16Format(2, 44100);
    AudioQueueRef probe = nullptr;
    if (AudioQueueNewOutput(&f, [](void *, AudioQueueRef, AudioQueueBufferRef) {}, nullptr, nullptr, nullptr, 0, &probe) != noErr) {
        std::fprintf(stderr, "gfx native: no CoreAudio output available\n");
        return;
    }
    AudioQueueDispose(probe, true);
    impl_->ready = true;
}

bool NativeAudioBackend::IsAudioDeviceReady() { return impl_->ready; }

gfx::Sound NativeAudioBackend::LoadSound(const char *file_name) {
    gfx::Sound out{};
    if (!impl_->ready) return out;
    WavDoc wav;
    FILE *f = std::fopen(file_name, "rb");
    if (!f) {
        std::fprintf(stderr, "gfx native: failed to open sound '%s'\n", file_name);
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return out;
    }
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (read != bytes.size() || !wav.LoadFromMemory(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "gfx native: failed to load sound '%s'%s%s\n", file_name, wav.Error().empty() ? "" : ": ",
                     wav.Error().c_str());
        return out;
    }
    auto *s = new SoundState();
    s->samples = wav.Samples();
    s->channels = wav.Channels();
    s->rate = wav.SampleRate();
    out.backend_handle = s;
    out.frameCount = static_cast<unsigned int>(TotalFrames(*s));
    return out;
}

gfx::Sound NativeAudioBackend::LoadSoundFromPcm(const int16_t *samples, size_t count, int channels, int rate) {
    gfx::Sound out{};
    if (!impl_->ready || samples == nullptr || count == 0 || channels <= 0 || rate <= 0) return out;
    auto *s = new SoundState();
    s->samples.assign(samples, samples + count);
    s->channels = channels;
    s->rate = rate;
    out.backend_handle = s;
    out.frameCount = static_cast<unsigned int>(TotalFrames(*s));
    return out;
}

void NativeAudioBackend::UnloadSound(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    {
        std::lock_guard<std::mutex> lock(s->control_mutex);
        DisposeQueue(s);
    }
    delete s;
}

void NativeAudioBackend::PlaySound(gfx::Sound sound) {
    // Always (re)starts from the beginning, unlike ResumeSound -- the
    // same raylib distinction the ALSA path keeps.
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    std::lock_guard<std::mutex> lock(s->control_mutex);
    DisposeQueue(s);
    s->frame_pos = 0;
    s->paused = false;
    s->stopping = false;
    AudioStreamBasicDescription f = Pcm16Format(s->channels, s->rate);
    if (AudioQueueNewOutput(&f, SoundFill, s, nullptr, nullptr, 0, &s->queue) != noErr) {
        s->queue = nullptr;
        return;
    }
    AudioQueueAddPropertyListener(s->queue, kAudioQueueProperty_IsRunning, SoundRunningChanged, s);
    const UInt32 bytes = kBufferFrames * 2u * static_cast<UInt32>(s->channels);
    for (UInt32 i = 0; i < kQueueBuffers; i++) {
        AudioQueueBufferRef buf = nullptr;
        if (AudioQueueAllocateBuffer(s->queue, bytes, &buf) != noErr) break;
        SoundFill(s, s->queue, buf);
    }
    s->running = true;
    if (AudioQueueStart(s->queue, nullptr) != noErr) DisposeQueue(s);
}

void NativeAudioBackend::PauseSound(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    std::lock_guard<std::mutex> lock(s->control_mutex);
    s->paused = true;
    if (s->queue) AudioQueuePause(s->queue);
}

void NativeAudioBackend::ResumeSound(gfx::Sound sound) {
    // Resumes where it paused; a clip that already finished stays
    // finished (as the ALSA path's exited thread does).
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    std::lock_guard<std::mutex> lock(s->control_mutex);
    s->paused = false;
    if (s->queue && s->running.load()) AudioQueueStart(s->queue, nullptr);
}

bool NativeAudioBackend::IsSoundPlaying(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return false;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    return s->running.load() && !s->paused.load();
}

void NativeAudioBackend::SetSoundVolume(gfx::Sound sound, float volume) {
    if (sound.backend_handle == nullptr) return;
    static_cast<SoundState *>(sound.backend_handle)->volume = volume;
}

double NativeAudioBackend::GetSoundTimePlayed(gfx::Sound sound) {
    // Frames handed to the output, as the ALSA path counts them.
    if (sound.backend_handle == nullptr) return 0.0;
    auto *s = static_cast<SoundState *>(sound.backend_handle);
    if (s->rate <= 0) return 0.0;
    return static_cast<double>(s->frame_pos.load()) / static_cast<double>(s->rate);
}

gfx::AudioStream NativeAudioBackend::OpenAudioStream(int channels, int rate) {
    gfx::AudioStream out{};
    if (!impl_->ready || channels <= 0 || rate <= 0) return out;
    auto *s = new StreamState();
    s->channels = channels;
    s->rate = rate;
    s->queue_pcm.reserve(static_cast<size_t>(rate * channels) * 2);
    AudioStreamBasicDescription f = Pcm16Format(channels, rate);
    if (AudioQueueNewOutput(&f, StreamFill, s, nullptr, nullptr, 0, &s->queue) != noErr) {
        delete s;
        return out;
    }
    const UInt32 bytes = kBufferFrames * 2u * static_cast<UInt32>(channels);
    for (UInt32 i = 0; i < kQueueBuffers; i++) {
        AudioQueueBufferRef buf = nullptr;
        if (AudioQueueAllocateBuffer(s->queue, bytes, &buf) != noErr) break;
        StreamFill(s, s->queue, buf);  // primes with short silences
    }
    s->started = AudioQueueStart(s->queue, nullptr) == noErr;
    out.backend_handle = s;
    return out;
}

void NativeAudioBackend::CloseAudioStream(gfx::AudioStream stream) {
    if (stream.backend_handle == nullptr) return;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    if (s->queue) AudioQueueDispose(s->queue, true);  // synchronous: no callback after this
    delete s;
}

void NativeAudioBackend::PushAudioStream(gfx::AudioStream stream, const int16_t *samples, size_t count) {
    if (stream.backend_handle == nullptr || samples == nullptr || count == 0) return;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    std::lock_guard<std::mutex> lock(s->queue_mutex);
    s->queue_pcm.insert(s->queue_pcm.end(), samples, samples + count);
}

size_t NativeAudioBackend::AudioStreamQueuedFrames(gfx::AudioStream stream) {
    if (stream.backend_handle == nullptr) return 0;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    std::lock_guard<std::mutex> lock(s->queue_mutex);
    return (s->queue_pcm.size() - s->read_pos) / static_cast<size_t>(s->channels);
}

double NativeAudioBackend::AudioStreamPlayedSeconds(gfx::AudioStream stream) {
    if (stream.backend_handle == nullptr) return 0.0;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    AudioTimeStamp ts{};
    // Fails while the queue has not started rendering yet: keep the floor.
    if (s->queue && AudioQueueGetCurrentTime(s->queue, nullptr, &ts, nullptr) == noErr && (ts.mFlags & kAudioTimeStampSampleTimeValid)) {
        const double secs = RealFramesPlayed(s, std::max(0.0, ts.mSampleTime)) / static_cast<double>(s->rate);
        if (secs > s->last_clock) s->last_clock = secs;
    }
    return s->last_clock;
}

void NativeAudioBackend::SetAudioStreamPaused(gfx::AudioStream stream, bool paused) {
    if (stream.backend_handle == nullptr) return;
    auto *s = static_cast<StreamState *>(stream.backend_handle);
    if (s->paused.exchange(paused) == paused || !s->queue) return;
    if (paused) AudioQueuePause(s->queue);
    else s->started = AudioQueueStart(s->queue, nullptr) == noErr;
}

void NativeAudioBackend::SetAudioStreamVolume(gfx::AudioStream stream, float volume) {
    if (stream.backend_handle == nullptr) return;
    static_cast<StreamState *>(stream.backend_handle)->volume = std::clamp(volume, 0.0f, 2.0f);
}

#else  // Windows/Emscripten: graceful no-op, see this file's own top comment.

struct NativeAudioBackend::Impl {};

NativeAudioBackend::NativeAudioBackend() : impl_(new Impl()) {}
NativeAudioBackend::~NativeAudioBackend() { delete impl_; }
void NativeAudioBackend::InitAudioDevice() {}
bool NativeAudioBackend::IsAudioDeviceReady() { return false; }
gfx::Sound NativeAudioBackend::LoadSound(const char *) { return gfx::Sound{}; }
gfx::Sound NativeAudioBackend::LoadSoundFromPcm(const int16_t *, size_t, int, int) { return gfx::Sound{}; }
void NativeAudioBackend::UnloadSound(gfx::Sound) {}
void NativeAudioBackend::PlaySound(gfx::Sound) {}
void NativeAudioBackend::PauseSound(gfx::Sound) {}
void NativeAudioBackend::ResumeSound(gfx::Sound) {}
bool NativeAudioBackend::IsSoundPlaying(gfx::Sound) { return false; }
void NativeAudioBackend::SetSoundVolume(gfx::Sound, float) {}
double NativeAudioBackend::GetSoundTimePlayed(gfx::Sound) { return 0.0; }
gfx::AudioStream NativeAudioBackend::OpenAudioStream(int, int) { return gfx::AudioStream{}; }
void NativeAudioBackend::CloseAudioStream(gfx::AudioStream) {}
void NativeAudioBackend::PushAudioStream(gfx::AudioStream, const int16_t *, size_t) {}
size_t NativeAudioBackend::AudioStreamQueuedFrames(gfx::AudioStream) { return 0; }
double NativeAudioBackend::AudioStreamPlayedSeconds(gfx::AudioStream) { return 0.0; }
void NativeAudioBackend::SetAudioStreamPaused(gfx::AudioStream, bool) {}
void NativeAudioBackend::SetAudioStreamVolume(gfx::AudioStream, float) {}

#endif  // MEP_AUDIO_ALSA / MEP_AUDIO_COREAUDIO

}  // namespace gfx
