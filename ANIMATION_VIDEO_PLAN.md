# 3D scene animation + in-app video playback

Adds camera keyframe animation to the 3D modeler, exports it to a real,
standard QuickTime `.mov` file (Motion-JPEG, written fully in-house --
no ffmpeg/libav dependency), and plays that video back **inside mep
itself** via a new video-playback pane.

**How to resume:** check the boxes below, `git log --oneline -- src/jpeg_codec.cpp src/mov_container.cpp src/model3d_doc.cpp`
for what's landed, continue with the first unchecked phase. Same rigor
as every other plan here: implement -> `nix develop --command cmake
--build build/native` -> `just test` -> live-verify via the real
running `mep` instance over the agent-control socket (screenshots + RPC
calls) -> tick the box with a "Verified via:" note -> next phase,
pausing for the user's go-ahead unless told to keep going.

---

## Why this plan exists

mep's 3D modeler can already build and render complex scenes (see
`CHESS_SET_BENCHMARK_PLAN.md`) but has zero animation capability --
confirmed by direct code read: glTF/IQM/M3D importers explicitly
parse-and-discard any animation/skin data (`backend_native_model_gltf.cpp:8-9`,
`backend_native_model_iqm.cpp:8`, `backend_native_model_m3d.cpp:12`),
and no `AnimationClip`/`Keyframe`/`Timeline` struct exists anywhere.

Two research passes confirmed: `Model3DRenderToImageFile`
(`main.cpp:20069`) is a real offscreen-render-to-pixels path to reuse
per animation frame; no ffmpeg/libav is available in this project's nix
devShell (absent from `flake.nix`, only present via the user's personal
profile) and no video decode/encode exists anywhere in the codebase
(the HTML `<video>` tag is explicitly stubbed "no video decoder"); and
mep's JPEG codec is **decode-only** -- no encoder exists to reuse.

Given that, the user chose (explicitly, after being told the JPEG
encoder doesn't exist and would need to be written) to go fully
in-house rather than depend on ffmpeg: a real baseline JPEG encoder
plus a hand-written QuickTime `.mov` muxer/demuxer (Motion-JPEG). This
matches the project's existing convention of in-house PNG/JPEG/BMP/GIF
codecs and produces a real, standard, externally-playable file.

The `PdfSession` pattern (`editor.h:1265-1340`,
`Editor::EnsurePdfPagesRastered` at `editor.cpp:7640`,
`GetOrUpdatePdfPageTexture` at `main.cpp:16321`) is the template for
"a multi-frame browsable document with lazy decode + windowed cache +
generation-gated texture upload" that the new video pane follows.

---

## Phase 1: Camera keyframe/animation system

- [x] `Vector3Lerp` (+ plain float `Lerp`) added to `src/gfx/vecmath.h`.
- [x] `CameraKeyframe` struct + `camera_keyframes` vector on
  `Model3DSession` (`editor.h:1123`), time-sorted on insert.
- [x] Pure `Model3DSampleCameraAtTime(session, time)` sampling function
  (linear interpolation; shared by live preview and video export).
- [x] `Editor::Model3DAnimAddCameraKeyframe` / `Model3DAnimClearCamera`
  / `Model3DAnimOrbitCamera` / `Model3DAnimSetCameraTime`.
- [x] RPC: `model.animAddCameraKeyframe`, `model.animClearCamera`,
  `model.animOrbitCamera`, `model.animSetCameraTime`.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (same pre-existing unrelated
  `mep-collab-session-test` failure). Live: spawned a real `mep`
  instance, built a 2-object scene (red cube at x=+1.5, blue sphere at
  x=-1.5), called `model.animOrbitCamera` (4s duration, 1 revolution),
  then `model.animSetCameraTime` at t=0/1/2/3s -- `model.cameraGet`
  confirmed yaw sampled to exactly 0/90/180/270 as expected, and
  `model.renderToImage` at each time showed the cube/sphere correctly
  swapping screen sides between t=0 and t=2 (halfway around the
  orbit), i.e. the camera really moves, not just the reported yaw
  value.

## Phase 2: Baseline JPEG encoder

- [x] `jpeg::Encode(width, height, comp, pixels, stride, quality)` added
  to `src/jpeg_codec.h`/`.cpp` alongside the existing `Decode`.
- [x] Real baseline sequential DCT JPEG (RGB->YCbCr, 4:4:4, separable
  forward DCT, standard IJG quantization tables, zigzag, DC delta + AC
  RLE, standard Annex-K Huffman tables, full JFIF bitstream).
- [x] `src/jpeg_encoder_test.cpp`: encode->decode round-trip via mep's
  own `jpeg::Decode`, error-bound checks across several synthetic
  images including non-multiple-of-8 dimensions.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (new `mep-jpeg-codec-test` target wired into `just test`), all checks
  pass (solid color, grayscale, RGBA-with-alpha-dropped, non-multiple-
  of-8 gradient, checkerboard-at-quality-100 stress case exercising
  `ClampToCategory`, determinism, invalid-input rejection). Independent
  cross-checks beyond mep's own decoder: the system `file`/libmagic
  utility identifies both dumped sample files as well-formed "JPEG
  image data, JFIF standard 1.01, baseline, precision 8" -- confirming
  real standards conformance, not just self-consistency with mep's own
  reader -- and the `Read` tool's own (separately-implemented) image
  decoder displays both correctly: a smooth, artifact-free gradient and
  a clean alternating checkerboard.

## Phase 3: QuickTime `.mov` muxer + demuxer

- [x] `src/mov_container.h`/`.cpp`: `WriteMovFile` (single video track,
  fixed frame rate, one sample per chunk, `stsd('jpeg')`) and
  `OpenMovFile` / `ReadMovFrameJpeg` (atom-tree walk, on-demand
  per-frame reads).
- [x] `src/mov_container_test.cpp`: mux synthetic JPEG frames, demux
  back, confirm frame count/size/fps/byte-exact round-trip, confirm
  each frame still decodes.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (new `mep-mov-container-test` target, plus `mov_container.cpp` now
  linked into the main `mep` binary), `just test` passes (same
  pre-existing unrelated `mep-collab-session-test` failure). One real
  bug found and fixed along the way: the test's own frame-byte
  comparison (`std::equal` between `vector<unsigned char>` and
  `std::string`) spuriously failed on every byte >=0x80 due to a
  signed/unsigned promotion mismatch -- not a muxer bug -- fixed by
  comparing via `memcmp`. Independent verification well beyond mep's
  own demuxer: `ffprobe` (unrelated, pre-existing tool on this machine)
  correctly reports codec=mjpeg/Baseline, 64x48, `yuvj444p` (confirming
  4:4:4 sampling), 24/1 fps, 12 frames, 0.5s duration on a muxed
  sample; `ffmpeg` extracted frame 5 and it rendered as exactly the
  expected color from the test's per-frame formula.

## Phase 4: Render-animation-to-video export

- [x] `Model3DRenderToImageFile`'s core render-to-pixels logic factored
  into reusable `Model3DRenderFrameToPixels(...)`.
- [x] `Model3DRenderAnimationToVideoFile(...)`: sample camera -> render
  frame -> JPEG-encode -> collect -> mux once at the end.
- [x] RPC: `model.renderAnimationToVideo`.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (main `mep` binary now links `jpeg_codec.cpp`/`mov_container.cpp`
  directly), `just test` passes (same pre-existing unrelated
  `mep-collab-session-test` failure). Live: spawned a real `mep`
  instance, built a 2-object scene, `model.animOrbitCamera` (2s, 1
  revolution) + `model.renderAnimationToVideo` (12fps, 320x240) ->
  `anim_export.mov`. Independent verification via `ffprobe`
  (`probe_score=100`, codec=mjpeg, 320x240, 12fps, 25 frames matching
  the expected `duration*fps+1` frame count, handler_name="mep video
  handler") and `ffmpeg`-extracted frames: frame 0 shows the
  sphere/cube in their starting left/right positions, frame 12
  (~halfway through the 2s/1-revolution orbit) shows them swapped --
  confirming the exported video really shows the camera orbiting
  through the scene, not static or duplicated frames.

## Phase 5: In-app video playback pane

- [x] `Mode::Video` added to the `Mode` enum.
- [x] `VideoSession` (mirrors `PdfSession`'s windowed lazy-decode
  pattern) + storage map + `GetVideo`/`GetVideoMutable`.
- [x] `Editor::OpenVideoInPlace`, wired into `LoadFile`'s dispatch via
  a new `IsMovPath` branch.
- [x] `EnsureVideoFramesDecoded` + `GetOrUpdateVideoFrameTexture`
  (mirrors the Pdf equivalents).
- [x] `DrawVideoPane`: frame texture, scrub bar, play/pause,
  frame-step, `frame N/Total  time/duration` status.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (same pre-existing unrelated
  `mep-collab-session-test` failure). Live: spawned a real `mep`
  instance, built a scene, exported a `.mov` (Phase 4), opened it via
  `file.open` (routes through the new `IsMovPath`/`OpenVideoInPlace`
  branch), and confirmed via `pane.get` + screenshots that: the active
  pane's buffer became the video buffer, the status bar read "VIDEO"
  (`ModeName(Mode::Video)`), the pane header read
  "Video: ... (480x360, 31 frames @ 10fps)" with correct numbers, the
  displayed frame content exactly matched the exported animation's
  frame 0 (sphere left/cube right, matching the orbit's `yaw=0`), and
  the transport bar showed the correct "1/31  0.0s/3.1s" computed from
  fps/frame-count/duration.
  **Follow-up (same session): interactive play/pause/scrub/frame-step
  fully verified live, and two real bugs found and fixed along the
  way.** Synthetic input initially produced no visible effect at all
  (confirmed environment-wide via an unrelated pre-existing keybinding,
  `:`, also failing to register) -- root cause found via `xprop`/
  `xinput`/`awesome-client`: the `awesome` window manager running in
  this sandbox had a kitty terminal focused, not the `mep` window, and
  XTest keyboard/button events deliver based on real X11 focus/pointer
  routing, not just "the app is running" -- `mep::agent_ui`'s X11 XTest
  backend (`agent_ui_input.cpp`) was always correct, it simply never
  had a focused target window to deliver to. Focusing `mep` first
  (`awesome-client` Lua: `c:jump_to(); client.focus = c`) immediately
  made keyboard input work (arrow-key frame-step confirmed live:
  3 presses advanced frame 1->4, scrub bar visibly updated) and
  surfaced two real, previously-undetectable bugs:
  1. **Space/leader-key collision**: `HandleVideoInput` checked the
     leader-key branch before the Space-for-play/pause branch, and
     this app's *default* `leader_key_` is Space -- so with default
     settings, Space always opened WhichKey and play/pause was
     completely unreachable from the keyboard. Fixed by checking
     Space first specifically in `HandleVideoInput` (a deliberate,
     documented exception to the "leader always wins" ordering every
     other mode uses, justified by how strong the Space-for-play/pause
     convention is and that leader is still reachable everywhere else).
     Confirmed fixed: Space now toggles the icon between play/pause and
     playback genuinely advances (frame 1->23 in 1.5s at 15fps, exact
     match).
  2. **Play/pause button silently unclickable**: `DrawPane`'s existing
     catch-all "focus this pane on any content-area click" region
     (registered *before* `DrawVideoPane` runs) covered the whole pane
     including the transport bar, with no exclusion for it -- the exact
     same class of bug Model3D/ImageEditor/Office each already had to
     fix for their own menubar/toolbar/sidebar areas (see their own
     code comments), just not yet applied to the new video pane. First
     misdiagnosed as an imprecise click target (the button was also
     widened to a 44px floor, a genuine independent usability
     improvement, but that alone did not fix it) -- the scrub bar right
     next to it worked immediately throughout because it's driven by
     direct per-frame polling inside `DrawVideoPane`, not a registered
     click region, so it was never swallowed by the catch-all. Fixed by
     excluding the transport bar height (`kVideoTransportH`, promoted
     from a `DrawVideoPane`-local constant to a shared one) from that
     catch-all region, mirroring the existing Model3D exclusion exactly.
     Confirmed fixed: clicking the button from a known-paused state
     toggled to playing and advanced (frame 1->18 in 1.2s at 15fps,
     exact match).
  `just test` and a full rebuild both still clean after both fixes.

## Phase 6: End-to-end benchmark + docs

- [x] `model.animOrbitCamera` + `model.renderAnimationToVideo` around
  the real chess-set scene -> `test/chess_orbit.mov`, opened and
  scrubbed live in mep, screenshots confirming a real orbit.
- [x] `just test` clean (only the pre-existing unrelated
  `mep-collab-session-test` failure).
- [x] `MEP_AGENT_API.md` + `.claude/skills/mep-3d-modeler/SKILL.md`
  updated with every new RPC method.
- [x] **Verified via:** opened `test/chess_set_full.gltf` (73 objects,
  16154 triangles, matching CHESS_SET_BENCHMARK_PLAN.md's known
  count) live in `mep`, `model.animOrbitCamera` (6s, 1 revolution,
  distance 13, pitch 35) + `model.renderAnimationToVideo` (15fps,
  960x720) -> `test/chess_orbit.mov` (91 frames, 6.07s, confirmed via
  `ffprobe`). Extracted frames at 0/22/45/68 (0/~90/~180/~270 degrees
  of the orbit) via `ffmpeg` and visually confirmed a real, smoothly-
  progressing orbit: frame 0 is a straight-on view (white pieces far,
  black near), frame 22 shows the board edge-on (black left, white
  right), frame 45 shows white/black having swapped near/far sides
  (exactly the opposite of frame 0, as a half-revolution should), and
  frame 68 mirrors frame 22 (white left, black right) -- not a static
  or broken export. Full clean build under `MEP_STRICT_FLAGS`
  (including a new `mep-mcp` tool-table entry per new RPC method --
  `mcp_bridge.cpp`'s `kTools`, so every method is reachable via
  `mep_model_anim_*`/`mep_model_render_animation_to_video` MCP tools,
  not just the raw JSON-RPC methods), `just test` passes (same
  pre-existing unrelated `mep-collab-session-test` failure). One real
  bug caught by the build itself: a `\"..\"`-escaped quote pair inside
  a `mcp_bridge.cpp` raw string literal accidentally formed the `)"`
  raw-string terminator early, truncating the literal mid-schema --
  fixed by rephrasing to avoid embedded quotes rather than escaping
  (raw strings don't process escapes at all).
  `MEP_AGENT_API.md` gained a new "Animation & video export" section
  (mirroring its existing per-feature subsections) plus a second
  worked example; `.claude/skills/mep-3d-modeler/SKILL.md` gained a
  bullet covering the same tools plus an explicit note about the
  Phase 5 interactive-input verification gap, so a future session
  knows that gap exists before assuming click/keypress behavior was
  actually confirmed.

### Non-goals

- Object/mesh animation (only the camera is animatable this round).
- Chroma subsampling / optimized Huffman tables / progressive JPEG --
  a correct baseline encoder only.
- Audio tracks.
- Importing arbitrary external `.mov`/`.mp4` files -- the demuxer only
  needs to read what mep's own muxer writes.
