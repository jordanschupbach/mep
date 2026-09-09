// Stage B audio smoke test: exercises the actual gfx::Audio* facade
// against the native backend (device init, load, play/pause/resume,
// IsSoundPlaying, volume, unload). Takes a PCM16 WAV path as argv[1] --
// the only format the in-house ALSA backend supports (see
// MINIAUDIO_REMOVAL_PLAN.md: every real call site only ever needs WAV).
// Can't visually "see" correctness the way the 2D/3D smoke tests screenshot
// their result, so this checks the state machine mechanically: frameCount
// is nonzero after a successful load, IsSoundPlaying reflects start/pause/
// resume transitions, and nothing crashes across the whole sequence.

#include <cstdio>
#include <thread>
#include <chrono>

#include "gfx/audio.h"
#include "gfx/backend_native.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <sound-file>\n", argv[0]);
        return 1;
    }
    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));

    if (gfx::IsAudioDeviceReady()) {
        std::fprintf(stderr, "smoke: FAILED -- device reports ready before InitAudioDevice\n");
        return 1;
    }
    gfx::InitAudioDevice();
    if (!gfx::IsAudioDeviceReady()) {
        std::fprintf(stderr, "smoke: FAILED -- IsAudioDeviceReady false after InitAudioDevice\n");
        return 1;
    }
    std::printf("smoke: audio device ready\n");

    gfx::Sound sound = gfx::LoadSound(argv[1]);
    if (sound.frameCount == 0) {
        std::fprintf(stderr, "smoke: FAILED -- LoadSound('%s') gave frameCount 0\n", argv[1]);
        return 1;
    }
    std::printf("smoke: loaded '%s', %u frames\n", argv[1], sound.frameCount);

    gfx::SetSoundVolume(sound, 0.3f);
    gfx::PlaySound(sound);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!gfx::IsSoundPlaying(sound)) {
        std::fprintf(stderr, "smoke: FAILED -- IsSoundPlaying false shortly after PlaySound\n");
        return 1;
    }
    std::printf("smoke: playing after PlaySound, as expected\n");

    gfx::PauseSound(sound);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (gfx::IsSoundPlaying(sound)) {
        std::fprintf(stderr, "smoke: FAILED -- IsSoundPlaying true after PauseSound\n");
        return 1;
    }
    std::printf("smoke: stopped after PauseSound, as expected\n");

    gfx::ResumeSound(sound);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!gfx::IsSoundPlaying(sound)) {
        std::fprintf(stderr, "smoke: FAILED -- IsSoundPlaying false after ResumeSound\n");
        return 1;
    }
    std::printf("smoke: playing again after ResumeSound, as expected\n");

    gfx::UnloadSound(sound);
    std::printf("smoke: OK -- full audio lifecycle behaved correctly\n");
    return 0;
}
