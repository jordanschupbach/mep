# miniaudio Removal Plan

Replace `third_party/miniaudio.h` (vendored single-header, `MA_IMPLEMENTATION`
compiled into `src/gfx/backend_native_audio.cpp`) with an in-house audio
output backend behind the same `gfx::Sound`/`LoadSound`/`PlaySound`/
`PauseSound`/`ResumeSound`/`IsSoundPlaying`/`SetSoundVolume`/
`InitAudioDevice`/`IsAudioDeviceReady` API `gfx/audio.h` already defines --
zero call-site changes needed anywhere outside
`backend_native_audio.cpp` itself.

**How to resume:** check the boxes below, `git log --oneline -- src/gfx/backend_native_audio.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
`WORKSPACES_PLAN.md`: implement -> `nix develop --command cmake --build
build/native` -> verify -> tick the box with a short "Verified via..."
note -> next phase, pausing for the user's go-ahead unless told to keep
going.

---

## Why this is the easiest candidate

Reading `backend_native_audio.cpp` (147 lines) end to end: mep uses only
miniaudio's high-level `ma_engine`/`ma_sound` API (device management,
decode, and mixing all handled internally by miniaudio) -- 9 functions
total (`ma_engine_init`/`_uninit`, `ma_sound_init_from_file`/`_uninit`,
`_start`/`_stop`/`_seek_to_pcm_frame`/`_is_playing`/`_set_volume`/
`_get_length_in_pcm_frames`). And the actual *usage* of that API,
grepped across the codebase, is narrower still:

- **One call site**: `main.cpp`'s `SyncHtmlMediaPlayback` (~line 17933),
  backing `<audio>`/`<video>` element playback in mep's in-app HTML
  viewer (`html_doc.cpp`'s DOM). No standalone "UI notification sound"
  feature exists separately from this.
- **One format, in practice**: every real reference to an audio file
  anywhere in the codebase -- `html_doc_test.cpp`'s test fixture,
  `js_engine.cpp`'s `canPlayType` check (`audio/wav`/`audio/wave`/
  `audio/x-wav`/`audio/vnd.wave`), and the speech-to-text debug-recording
  Lua chunks in `main.cpp` -- is `.wav`. Nothing in-tree exercises MP3/
  FLAC/OGG despite miniaudio supporting the first two internally (via
  its bundled dr_mp3/dr_flac) and being extensible to the third. This
  plan scopes decode to WAV only; see **Non-goals**.
- **WAV decode already exists in-house**: `src/wav_doc.cpp`/`.h` (26
  lines, raylib-free per its own module comment) already parses WAV into
  plain PCM sample data. The only genuinely new work this plan requires
  is **audio output** -- getting decoded PCM frames to a real sound
  device -- not decode.

## Scoping decisions

1. **Linux/ALSA first**, matching how this sandbox and the project's own
   verified platform work everywhere else in the `gfx::` backend (GLFW's
   X11 usage, `agent_ui_input.cpp`'s XTest). PulseAudio/PipeWire users
   are covered transparently since both provide an ALSA compatibility
   shim (`pulseaudio-alsa`/`pipewire-alsa`) that intercepts `libasound`
   calls -- the same assumption raylib's own ALSA-via-miniaudio path
   already made. Direct PulseAudio/PipeWire native APIs are a possible
   follow-up, not required.
2. **WAV-only decode.** `wav_doc.cpp` already covers this; extending it
   (if it doesn't already handle every PCM sub-format mep's own speech-
   to-text recordings and any real-world `<audio src>` might use --
   check its actual format coverage in Phase 1) is in scope. MP3/FLAC/
   OGG decode is explicitly **not** -- see Non-goals.
3. **Windows (WASAPI) and macOS (CoreAudio) backends are stubs that fail
   gracefully** (`InitAudioDevice` leaves `ready = false`, matching
   `ma_engine_init`'s own failure path already handled in
   `backend_native_audio.cpp`), not implemented in this pass -- same
   "Linux-verified now, other platforms are documented future work"
   scoping the GLFW replacement plan uses. mep's own primary dev/build
   platform (this sandbox, and per `flake.nix`'s Linux-specific
   `buildInputs`) is Linux; nothing here currently ships a verified
   Windows/macOS native build to regress.

## Phases

### Phase 1: survey + `wav_doc.h` gap check
- [x] Read `wav_doc.cpp`/`.h` fully; confirm it exposes (or extend it to
  expose) sample rate, channel count, bit depth, and a flat PCM buffer --
  everything `ma_sound_init_from_file` currently gives
  `backend_native_audio.cpp` implicitly. **Verified via:** `WavDoc`
  already exposes `SampleRate()`/`Channels()`/`Samples()` (a flat
  `vector<int16_t>`); it hard-requires PCM16 (rejects anything else with
  `"only PCM16 WAVE is supported"`), which matches every real need -- no
  extension needed.
- [x] Confirm `html_doc_test.cpp`'s WAV fixture and mep's own STT debug
  recordings (`main.cpp`'s Lua chunks around line 11977) both decode
  correctly through `wav_doc.h` as-is. **Verified via:** reading both --
  the test fixture writes 8000 Hz mono PCM16 directly; the STT chunks
  record via `ffmpeg -ar 16000 -ac 1` (mono, no explicit codec means
  ffmpeg's own PCM16 WAV default) -- both squarely inside `WavDoc`'s
  existing scope, confirmed without needing a throwaway test program.

### Phase 2: ALSA output backend
- [x] Folded into the existing file (not a new
  `backend_native_audio_alsa.cpp` -- the whole implementation is small
  enough that a second file would just be indirection), gated
  `#if !defined(__EMSCRIPTEN__) && !defined(_WIN32) && !defined(__APPLE__)`
  as `MEP_AUDIO_ALSA`. **Verified via:** each `Sound` opens its own PCM
  device (`snd_pcm_open("default", ...)` + `snd_pcm_set_params`,
  configured to that WAV's own rate/channel count) lazily on first
  `PlaySound`, not eagerly at `LoadSound` -- simpler than a shared-device
  mixer and matches mep's real usage (never more than one `<audio>`/
  `<video>` element playing at once); a background thread per playing
  sound writes 1024-frame chunks via `snd_pcm_writei` until stopped,
  finished, or a fatal ALSA error (`snd_pcm_recover` handles the
  recoverable case first).
- [x] `gfx::Sound` gains a `SoundState` struct (PCM buffer + atomic frame
  cursor/paused/stop flags + volume + the playback `std::thread` itself)
  instead of miniaudio's opaque `ma_sound*`.
- [x] Implemented `LoadSound`/`UnloadSound`/`PlaySound`/`PauseSound`/
  `ResumeSound`/`IsSoundPlaying`/`SetSoundVolume`, preserving the exact
  semantics `backend_native_audio.cpp`'s own comments documented
  (`PlaySound` always restarts from frame 0 via `StopAndJoin` + resetting
  `frame_pos`; `PauseSound`/`ResumeSound` just toggle an atomic `paused`
  flag without touching `frame_pos`, so resume continues rather than
  restarts). **Verified via:** `mep-gfx-native-audio-smoke` (see Phase 5).
- [x] Volume: software gain multiply (`float` scale + clamp to the
  int16 range) applied per-sample in the writer thread before each
  `snd_pcm_writei` -- simple, and nothing in mep's actual usage (a media
  element's volume slider) demands hardware-mixer-level precision.

### Phase 3: CMake/flake wiring
- [x] `CMakeLists.txt`: `find_package(ALSA REQUIRED)` (Linux-only branch,
  mirroring the existing `NOT EMSCRIPTEN AND NOT WIN32 AND NOT APPLE`
  gating used for X11/Xtst and Threads), `ALSA::ALSA` linked into
  `mep_core`, `mep_add_gfx_native_smoke`'s targets, `mep-model3d-doc-test`,
  and `mep-amalgam`. `src/wav_doc.cpp` also had to be added explicitly to
  the standalone gfx-smoke/model3d-doc-test executables (they build
  `MEP_GFX_NATIVE_SOURCES` directly rather than linking `mep_core`, which
  already had it).
- [x] `flake.nix`: added `pkgs.alsa-lib` to both `buildInputs` (the
  `mepPackage` derivation) and `devShells.default`'s `packages`.
  **Verified via:** confirming it was *not* already present transitively
  (`gcc -lasound` against a trivial `<alsa/asoundlib.h>` probe failed
  with "No such file or directory" before this change, succeeded after
  re-entering `nix develop`) -- the "likely already present" guess above
  was wrong, good that this phase said verify rather than assume.
- [x] Removed `#include "../third_party/miniaudio.h"` and its
  `MA_IMPLEMENTATION`/pragma-suppression block from
  `backend_native_audio.cpp`; deleted `third_party/miniaudio.h` and
  `third_party_licenses/miniaudio-LICENSE.txt` (via `git rm`) once
  `grep -rn "miniaudio\|ma_"` across `src/`/`CMakeLists.txt`/`flake.nix`
  returned nothing but this plan's own prose.

### Phase 4: Emscripten/wasm parity check
- [x] Confirmed the wasm build stays a documented no-op stub for now
  (same branch as Windows/macOS), not a real Web Audio shim. **Verified
  via:** attempting `nix develop --command just build-web` to check
  wasm still compiles turned up that it **already fails at baseline**,
  on pre-existing errors entirely unrelated to audio (`lua_env.cpp`/
  `js_engine.cpp`/`editor.cpp` -Werror violations and a missing
  `AgentParticipant::terminal_buffer_id` member) -- matching the
  `env_offline_worktree_configure` memory's existing note that "wasm
  build + mep-amalgam fail at baseline, not a regression." A real Web
  Audio implementation would be unverifiable (and untested by anyone
  today) until that unrelated breakage is fixed first, so it's out of
  scope for this plan -- the no-op stub is honest about that, not a
  shortcut.

### Phase 5: cleanup + verification
- [x] `nix develop --command cmake --build build/native -j$(nproc)` --
  clean full build. `nix develop --command just test` -- pure-logic suite
  passes (the one failure, `mep-collab-session-test` run with no
  arguments, is pre-existing/unrelated, confirmed against a clean
  baseline in an earlier session).
- [x] `mep-gfx-native-audio-smoke` against a generated 2-second 440Hz
  PCM16 WAV: device-ready, load (88200 frames, correct for 44100Hz*2s),
  play/pause/resume state transitions, unload -- full lifecycle passed
  with no ALSA errors on stderr. This sandbox has a real PulseAudio
  server (`pactl info` succeeded) and real hardware playback devices
  (`aplay -l` listed HDMI outputs), so ALSA's `"default"` device is a
  genuine, working playback path here, not a stub.
- [x] Live verification: opened a real `.html` file with
  `<audio id="clip" src="tone.wav">` + a `clip.play()` script in a fresh
  `mep` instance via the agent socket. The HTML viewer rendered
  `[play audio: tone.wav 2.000000s]` -- the exact 2-second duration the
  fixture WAV has, computed by `WavDoc` and driven through
  `SyncHtmlMediaPlayback` into the new ALSA backend end-to-end, no
  errors. (Full audibility itself wasn't independently confirmed by ear
  in this sandbox -- the state-machine + duration-computation evidence
  above is the practical proxy this phase anticipated needing.)
- [x] Grepped the whole tree for `miniaudio`/`ma_` -- zero remaining
  references outside this plan's own prose and `DEPENDENCIES.md`/
  `OPENSSL_REMOVAL_PLAN.md`'s historical mentions. Stale doc comments in
  `gfx/backend.h`, `gfx/types.h`, and `gfx/backend_native.h` that still
  said "miniaudio" were also updated while here.

## Non-goals

- MP3/FLAC/OGG/Opus decode -- not exercised anywhere in-tree today (see
  above); adding real perceptual-codec decoders is a much larger, separate
  undertaking (MP3 alone needs Huffman decode + IMDCT + subband
  synthesis) that isn't earning its cost against zero current usage. If a
  real need shows up later (a user opens an actual `.mp3` in the HTML
  viewer), revisit as its own follow-up, not blocking this plan.
- Audio *capture*/recording -- mep's STT feature (`main.cpp`'s Lua
  chunks) shells out to an external recorder today (implied by the
  `mep_stt_debug.wav`/curl-upload code path), not through `gfx::audio.h`
  at all; out of scope here.
- Windows/macOS native audio backends -- documented future work, not
  blocking (see Scoping decision 3).
