// NativeAudioBackend: in-house audio for Stage B's native backend (see
// MINIAUDIO_REMOVAL_PLAN.md for the full removal writeup). Replaces
// third_party/miniaudio.h with a small ALSA playback backend + the
// existing in-tree WavDoc (wav_doc.h) decoder -- confirmed sufficient
// for every real call site: main.cpp's <audio>/<video> element bridge
// (HtmlMediaPlayback) only ever plays PCM16 WAV in practice (the html-
// doc-test fixture, and mep's own speech-to-text debug recordings via
// `ffmpeg -ar 16000 -ac 1`, both PCM16), which WavDoc already parses.
//
// Scope: Linux/ALSA only (matches every other Linux-first `gfx::`
// native-backend subsystem -- X11 for windowing/input, XTest for agent
// synthetic input). PulseAudio/PipeWire users are covered transparently
// via their ALSA compatibility shim, the same assumption raylib's own
// miniaudio-based ALSA path already made. Windows/macOS are a graceful
// no-op (InitAudioDevice leaves `ready = false`, matching how a real
// device-open failure already had to be handled) -- documented future
// work, not blocking; see the plan's own Scoping decision 3.
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

#else  // !MEP_AUDIO_ALSA -- Windows/macOS/Emscripten: graceful no-op, see this file's own top comment.

struct NativeAudioBackend::Impl {};

NativeAudioBackend::NativeAudioBackend() : impl_(new Impl()) {}
NativeAudioBackend::~NativeAudioBackend() { delete impl_; }
void NativeAudioBackend::InitAudioDevice() {}
bool NativeAudioBackend::IsAudioDeviceReady() { return false; }
gfx::Sound NativeAudioBackend::LoadSound(const char *) { return gfx::Sound{}; }
void NativeAudioBackend::UnloadSound(gfx::Sound) {}
void NativeAudioBackend::PlaySound(gfx::Sound) {}
void NativeAudioBackend::PauseSound(gfx::Sound) {}
void NativeAudioBackend::ResumeSound(gfx::Sound) {}
bool NativeAudioBackend::IsSoundPlaying(gfx::Sound) { return false; }
void NativeAudioBackend::SetSoundVolume(gfx::Sound, float) {}

#endif  // MEP_AUDIO_ALSA

}  // namespace gfx
