#pragma once

// Facade over IAudioBackend (see gfx/backend.h). Usage in src/ is
// minimal today (a UI notification sound) so this stays small.

#include "gfx/backend.h"
#include "gfx/types.h"

namespace gfx {

inline void InitAudioDevice() { GetBackends().audio->InitAudioDevice(); }
inline bool IsAudioDeviceReady() { return GetBackends().audio->IsAudioDeviceReady(); }
inline Sound LoadSound(const char *file_name) { return GetBackends().audio->LoadSound(file_name); }
inline void UnloadSound(Sound sound) { GetBackends().audio->UnloadSound(sound); }
inline void PlaySound(Sound sound) { GetBackends().audio->PlaySound(sound); }
inline void PauseSound(Sound sound) { GetBackends().audio->PauseSound(sound); }
inline void ResumeSound(Sound sound) { GetBackends().audio->ResumeSound(sound); }
inline bool IsSoundPlaying(Sound sound) { return GetBackends().audio->IsSoundPlaying(sound); }
inline void SetSoundVolume(Sound sound, float volume) { GetBackends().audio->SetSoundVolume(sound, volume); }

}  // namespace gfx
