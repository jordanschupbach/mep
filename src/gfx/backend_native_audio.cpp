// NativeAudioBackend: real audio for Stage B's native backend, via
// vendored miniaudio.h (third_party/miniaudio.h -- see
// third_party_licenses/miniaudio-LICENSE.txt). This is in fact the same
// library raylib's own audio module is built on internally, so this is a
// low-risk, behavior-preserving swap for the small amount of audio mep
// actually uses (main.cpp's <audio>/<video> element bridge, see
// HtmlMediaPlayback there) -- smallest and simplest of Stage B's
// subsystems, per PLAN's own "good warm-up" framing.
//
// Uses miniaudio's high-level ma_engine/ma_sound API (decode+mix+device
// management all handled internally) rather than the lower-level
// ma_device callback API, matching the level raylib's own Sound/
// PlaySound/PauseSound/ResumeSound/IsSoundPlaying/SetSoundVolume API
// operates at.

#include "gfx/backend_native_internal.h"

#include <cstdio>

// Vendored third-party header -- suppress mep's own strict -Wall/-Wextra/
// ... flags (MEP_STRICT_FLAGS in CMakeLists.txt) for just this include,
// same as image_doc.cpp does for stb_image.h. miniaudio's own C source is
// large and triggers most of the same categories stb's headers do, plus
// a few more (unused parameters in miniaudio's many optional-backend
// stubs compiled out on this platform).
#define MA_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-format-attribute"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wduplicated-cond"
#endif
#if defined(__clang__)
// Emscripten's clang is stricter than desktop gcc/clang here: miniaudio's
// web-audio backend (compiled in under __EMSCRIPTEN__) uses EM_ASM's
// `$0`/`$1`/... placeholder syntax and emscripten/version.h's now-
// deprecated __EMSCRIPTEN_minor__/__EMSCRIPTEN_tiny__ macros.
#pragma GCC diagnostic ignored "-Wdollar-in-identifier-extension"
#pragma GCC diagnostic ignored "-Wdeprecated-pragma"
#pragma GCC diagnostic ignored "-Wvariadic-macro-arguments-omitted"
#endif
#include "../third_party/miniaudio.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace gfx {

struct NativeAudioBackend::Impl {
    ma_engine engine{};
    bool ready = false;
};

NativeAudioBackend::NativeAudioBackend() : impl_(new Impl()) {}
NativeAudioBackend::~NativeAudioBackend() {
    if (impl_->ready) ma_engine_uninit(&impl_->engine);
    delete impl_;
}

void NativeAudioBackend::InitAudioDevice() {
    if (impl_->ready) return;
    ma_result result = ma_engine_init(nullptr, &impl_->engine);
    if (result != MA_SUCCESS) {
        std::fprintf(stderr, "gfx native: ma_engine_init failed (result %d)\n", static_cast<int>(result));
        return;
    }
    impl_->ready = true;
}

bool NativeAudioBackend::IsAudioDeviceReady() { return impl_->ready; }

gfx::Sound NativeAudioBackend::LoadSound(const char *file_name) {
    gfx::Sound out{};
    if (!impl_->ready) return out;
    auto *sound = new ma_sound();
    ma_result result = ma_sound_init_from_file(&impl_->engine, file_name, 0, nullptr, nullptr, sound);
    if (result != MA_SUCCESS) {
        std::fprintf(stderr, "gfx native: failed to load sound '%s' (result %d)\n", file_name,
                     static_cast<int>(result));
        delete sound;
        return out;
    }
    ma_uint64 frames = 0;
    ma_sound_get_length_in_pcm_frames(sound, &frames);
    out.backend_handle = sound;
    out.frameCount = static_cast<unsigned int>(frames);
    return out;
}

void NativeAudioBackend::UnloadSound(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<ma_sound *>(sound.backend_handle);
    ma_sound_uninit(s);
    delete s;
}

void NativeAudioBackend::PlaySound(gfx::Sound sound) {
    // raylib's PlaySound always (re)starts from the beginning, unlike
    // ResumeSound -- see main.cpp's own SyncHtmlMediaPlayback comment on
    // this exact distinction.
    if (sound.backend_handle == nullptr) return;
    auto *s = static_cast<ma_sound *>(sound.backend_handle);
    ma_sound_seek_to_pcm_frame(s, 0);
    ma_sound_start(s);
}

void NativeAudioBackend::PauseSound(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return;
    ma_sound_stop(static_cast<ma_sound *>(sound.backend_handle));
}

void NativeAudioBackend::ResumeSound(gfx::Sound sound) {
    // ma_sound_stop leaves the playback cursor where it was (no implicit
    // seek), so a plain start here resumes rather than restarts.
    if (sound.backend_handle == nullptr) return;
    ma_sound_start(static_cast<ma_sound *>(sound.backend_handle));
}

bool NativeAudioBackend::IsSoundPlaying(gfx::Sound sound) {
    if (sound.backend_handle == nullptr) return false;
    return ma_sound_is_playing(static_cast<const ma_sound *>(sound.backend_handle)) == MA_TRUE;
}

void NativeAudioBackend::SetSoundVolume(gfx::Sound sound, float volume) {
    if (sound.backend_handle == nullptr) return;
    ma_sound_set_volume(static_cast<ma_sound *>(sound.backend_handle), volume);
}

}  // namespace gfx
