# Chess set visual realism pass

Follow-on to `CHESS_SET_BENCHMARK_PLAN.md` and
`MULTILIGHT_ANIMATION_PLAN.md`. The user asked for a visual-quality
assessment of the saved chess set (`test/chess_set_full.gltf`) followed
by as many improvement passes as needed to make it look as realistic as
possible, with explicit license to implement whatever
features/improvements seem useful along the way.

**How to resume:** check the boxes below, continue with the first
unchecked phase. Same rigor as every other plan here: implement ->
`nix develop --command cmake --build build/native` -> `just test` ->
live-verify via the real running `mep` instance over the agent-control
socket (renders/screenshots + RPC calls + independent `ffprobe`/
`ffmpeg` checks where relevant) -> tick the box with a "Verified via:"
note -> next phase, pausing for the user's go-ahead unless told to keep
going.

---

## Visual assessment (done)

Rendered the live scene from several angles and inspected each, then
cross-checked the most surprising finding against raw mesh data.
Findings, most to least impactful:

1. **Real bug: the knight-head mesh is broken.** All 4 knights render
   as a warped, twisted flat blade, not the horse-head silhouette
   `CHESS_SET_BENCHMARK_PLAN.md` Phases 8-10 built and verified.
   Confirmed via raw vertex/triangle data (ids 13/26/49/62): the 3
   extrusion layers are structurally present but the front layer's
   silhouette boundary points are scattered with no coherent order.
   Export/import round-trip ruled out as the cause.
2. Ruled out as NOT a bug: rook crenellations (32 merlon cubes) sit
   correctly on every rook; the 4 "extra" small cubes are the 2 kings'
   cross-bar toppers.
3. No anti-aliasing anywhere in the offscreen render path.
4. No shadows at all -- pieces look glued onto the board.
5. Board is a single flat 2-triangle quad, zero thickness, no border.
6. No environment/background context (flat near-black clear color).
7. Minor: rook merlon cubes read a bit "busy" (each independently
   wood-textured at full scale).

## Renderer internals research (done)

GL 3.3 core / GLSL 330 (GLES3/WebGL2 equivalent on Emscripten).
Supersampled AA has no blocker. Found and will fix alongside it: the
offscreen render's projection aspect ratio is computed from the live
window's framebuffer size, not the render-target's own dimensions.
Shadow map has no hard GL blocker but needs two new things: a
depth-texture-backed FBO helper (existing `LoadRenderTexture`'s depth
attachment is a non-sampleable renderbuffer), and this renderer's first
multi-pass draw. `Object3D` is single-material, so the board's frame
needs separate objects, not a multi-material box.

## Phase 1: Fix the knight-head mesh

- [x] Corrected 2D horse-head silhouette + proper ear-clipping
  triangulation (local Python helper, mirrors the existing 3-layer
  front/bulge/back extrusion approach -- that structural idea was
  sound, only the triangulation was broken).
- [x] Iterated on one knight first: rasterized the 2D silhouette to a
  PNG directly (fast local feedback loop, no mep round-trip) through
  several proportion passes before ever sending it to mep, then
  iterated the actual 3D placement (found and fixed two real issues
  live: `model.addCustomMesh` auto-offsets new objects' `position.x` to
  spread them out for interactive use, which had to be zeroed since
  these vertices are baked in absolute world space; and the silhouette
  needed a calibration-marker-cube pass, same methodology
  CHESS_SET_BENCHMARK_PLAN Phase 10 used, to confirm the camera-yaw ->
  world-axis mapping before trusting any "profile view" render).
- [x] Deleted the 4 broken heads (ids 13/26/49/62), added 4 corrected
  ones via `model.addCustomMesh` (with UVs) at the same
  positions/facings (white rotation=0, black rotation=180, derived from
  each old head's own bounding box), textured with a freshly generated
  wood-turned texture (light cream for white, dark walnut for black --
  the original per-object texture couldn't be reused directly, no RPC
  exposes reusing an existing texture_index by reference).
- [x] **Verified via:** close-up renders of all 4 (isolated single-knight
  close-ups plus full-board hero shots) confirming a recognizable
  horse-head silhouette (two ears with a notch, sloped forehead, jutting
  muzzle) from multiple angles, each sitting flush on its own stem, each
  facing its actual opponent. Saved back to `test/chess_set_full.gltf`
  -- caught and recovered from a real near-miss here: the agent-socket
  `file.save` RPC ignores its own `buffer_id` param and saves whatever
  buffer the GUI pane currently shows (not the buffer being edited),
  which silently truncated the file to 1 byte on the first save attempt.
  Recovered because the edited scene was still live in the running mep
  process (no data lost) -- fixed by forcing the GUI pane onto the
  correct buffer via `command.run` + `lua mep.buffer_switch(10)` before
  re-saving, then independently validated the saved file by parsing its
  JSON and confirming 73 nodes/meshes and all 4 `KnightHead_*` names
  present, before trusting it. (Filed as product feedback + a memory
  note for future sessions -- this is a real mep bug, out of scope to
  fix as part of this plan.)
- [x] **Follow-up fix** (user report after seeing the board: "the body
  of the horse looks weird (just like a pawn) and the horse is way too
  tall (taller than the king)"): both true. Measured every piece's real
  height for the first time (`model.listVertices` bounding boxes) --
  king 1.78 (lathe body 1.42 + cross topper), queen 1.55, bishop 1.35,
  pawn 1.08, rook 1.02 -- against the knight's own 1.798, confirming it
  was in fact the *tallest* piece on the board. Root cause: I'd copied
  the original `KNIGHT_BASE` lathe stem unexamined in Phase 1 and only
  fixed the head geometry -- a profile comparison (`listVertices`,
  radius by height) showed the knight's stem is structurally the same
  silhouette family as the pawn's own body (foot -> taper -> a bulbous
  mid-height flare), just shorter, which is exactly why it visually
  read as "a pawn with a rock on top." Fixed both problems together:
  replaced the stem with a new, shorter (0.95 -> 0.55), slender lathe
  profile with no bulbous flare (`model.addLathe`, 6 points), and kept
  the Phase 1 head geometry unscaled but moved it down onto the new,
  shorter stem -- bringing total height to 1.398, between bishop and
  queen, clearly under the king. **Verified via:** re-measured all 4
  knights' bounding boxes (1.398 each), close-up and full-board renders
  confirming a visibly slender stem (no pawn-like bulge) and correct
  relative height against the king/queen/bishop in the same shot.
  Re-saved (same verify-then-commit procedure as above).
- [x] **Light persistence, fixed as a second follow-up** (discovered
  while investigating the knight report, then fixed on request):
  `Scene::lights` was never written to or read from the glTF file at all
  -- confirmed by grepping `SaveModel3DGltf`/`LoadGltfModel` for any
  light-related code (none), and live: reopening the previously-saved
  file showed 0 lights despite Phase 5's own 3-light setup having been
  live and exported to video right before that save. Root cause of the
  gap: `LoadModel3DFile` gets its mesh/material data from
  `gfx::LoadModel`/`LoadGltfModel`, a format-agnostic (obj/iqm/vox/m3d/
  gltf) pipeline that returns a plain `gfx::Model` with no concept of
  lights at all (correctly, per the `gfx::`/document-model layering
  rule) -- so there was no path for light data to reach `Scene` on
  import even in principle. Fixed with a custom `extras.mep_lights`
  array in the glTF (a field-for-field dump of every `Light`, not the
  standard `KHR_lights_punctual` extension -- that encodes a directional
  light's direction via the owning node's rotation quaternion, real
  complexity this app's own `Light` struct has no need for since it
  already stores direction directly; `extras` is glTF-spec-legal
  free-form JSON for exactly this, the same bag `node.extras.visible`
  already uses elsewhere in this exporter). `SaveModel3DGltf` writes it;
  a new `LoadModel3DGltfLights` (model3d_doc.cpp) independently re-opens
  and re-parses the same file's raw JSON (deliberately outside the
  `gfx::LoadModel` pipeline) to read it back, a no-op for `.glb`/other
  formats/files with no lights. **Verified via:** full clean build under
  `MEP_STRICT_FLAGS`, `just test` passes. Live round-trip on a fresh
  scratch scene (2 lights, one directional + one invisible point with a
  non-default range) -- saved, reopened, `model.listLights` returned
  both with every field exactly matching, and a render confirmed the
  reloaded directional light's color/intensity genuinely drives shading
  (not just listable metadata). Then re-applied to the real chess set:
  added the same 3-light setup back, saved, and did a full fresh
  close/reopen of `mep` itself (not just a new buffer) -- confirmed 3
  lights and all 75 objects survive together, and a render shows the
  complete result (lit, shadowed, corrected knights, board frame/ground)
  now durable across restarts, not just within a live session.
  `MEP_AGENT_API.md` and the `mep-3d-modeler` skill's incorrect "saved
  implicitly as part of the scene" claim corrected in the same pass that
  discovered the gap, then updated again to describe the fix once it
  landed.

## Phase 2: Supersampled anti-aliasing + offscreen aspect-ratio fix

- [x] `gfx::BeginMode3D` gained explicit `render_width`/`render_height`
  params (0 = fall back to the live window's framebuffer size,
  preserving the live-viewport call site unchanged) threaded through
  `IRenderer3DBackend`/the native backend; the offscreen render path
  now passes its own actual render-target dimensions.
  `Model3DRenderFrameToPixels`'s callers (`Model3DRenderToImageFile`,
  `Model3DRenderAnimationToVideoFile`) render at `width*S, height*S`
  and box-downsample (`Model3DBoxDownsample`, plain CPU averaging) to
  the requested size before PNG/JPEG encode. Sparse `supersample?`
  param (default 2, clamped [1,4]) on `model.renderToImage`/
  `model.renderAnimationToVideo` (RPC + MCP schemas).
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (same pre-existing unrelated
  `mep-collab-session-test` failure). Live: rendered the same
  board-corner close-up at `supersample` 1/2/4 -- 2 and 4 show visibly
  smoother piece-silhouette and board-diagonal edges than 1, no other
  visual change. Rendered at a deliberately non-square 1600x500 to
  confirm the aspect fix: the board and background grid both render as
  true squares (not stretched/squashed), where before this fix the
  projection would have used the live window's own (different) aspect
  ratio regardless of the requested output size.

## Phase 3: Single directional-light shadow map

- [x] Depth-texture-backed shadow FBO helper (`shadow_fbo`/
  `shadow_depth_tex` in `NativeRenderer3DBackend::Impl`, distinct from
  `LoadRenderTexture`'s non-sampleable depth renderbuffer; paired with a
  throwaway color texture for framebuffer completeness). New
  `MatrixOrtho` (vecmath.h) for the directional light's orthographic
  frustum. `ComputeSceneWorldBounds` promoted from editor.cpp's
  anonymous namespace to external linkage (declared in editor.h) so
  main.cpp's shadow pass can fit the light frustum to the real scene
  bounds, same function `Model3DFrameAll` already trusted.
- [x] Shadow-casting light: light index 0 if a visible Directional
  light exists, else the legacy hardcoded key light's own direction
  (`Model3DRunShadowPass`, main.cpp).
- [x] New backend API (`gfx::BeginShadowPass`/`DrawMeshShadow`/
  `EndShadowPass`, a self-contained depth-only pass run before the
  frame's normal `BeginMode3D`/`DrawMesh` sequence, not nested inside
  it) wired into both `Model3DRenderFrameToPixels` (offscreen) and
  `DrawModel3DPane`'s live-viewport draw.
- [x] Fragment shader: manual-compare `uShadowMap` sample (bias 0.0025)
  with 3x3 texel-offset softening, blending toward a dim 0.35 floor (not
  pure black), multiplying only light index 0's own diffuse+specular
  contribution -- ambient floor and every other light unaffected.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (same pre-existing unrelated
  `mep-collab-session-test` failure). Live, three checks against the
  real saved chess set: (1) zero-lights (legacy key-light fallback)
  render shows clean, correctly-oriented soft contact shadows under
  every piece, no visible shadow acne or peter-panning at a close-up
  board-corner crop; (2) adding a custom directional light and
  re-rendering shows the shadows' direction/shape change to match the
  new light angle, confirming light index 0 (not just the legacy
  fallback) drives the shadow-casting light selection; (3) an
  orbit-camera + moving-object video export (reusing the proven
  `ffprobe`/`ffmpeg`-frame-extraction verification method), inspected at
  start/mid/end frames -- shadows stay fixed to the world as the camera
  orbits 360 degrees (not rotating with the camera), and the moving
  pawn's own shadow correctly follows it to its final position.

## Phase 4: Board thickness/frame + ground plane

- [x] Wooden frame/base object under/around the checker quad: a cube
  primitive scaled to 9x0.3x9 (board is 8x8, [-4,4]), positioned with
  its top just below y=0, textured with a freshly generated rich
  mahogany end-grain wood texture (`image.fillWood`) distinct from the
  checker squares -- gives the board real visible edge thickness at any
  oblique angle.
- [x] Large plain neutral ground/table plane (a `plane` primitive scaled
  to 30x30) beneath/around the frame, textured with a subtle muted
  warm-gray noise (`image.fillNoise`, fine-grained, felt/fabric-like) so
  shadows have somewhere to fall beyond the board and renders no longer
  look like pieces floating in a void. Skipped the background
  clear-color change from the original plan -- the ground plane alone
  fully solved the "floating in void" look in practice (confirmed via
  renders at both a normal 3/4 angle and a low, more horizon-revealing
  angle), so tuning the theme's shared background color (a broader,
  riskier change reaching outside the 3D modeler) turned out
  unnecessary.
- [x] Nice-to-have (wood-grain checkerboard texture instead of the
  existing flat two-color checker) -- skipped; the frame+ground+shadows
  combination already reads as convincingly realistic without it, and
  the existing checker squares aren't a focus of viewer attention next
  to the now much-more-detailed board edge/surroundings.
- [x] **Verified via:** live renders from three angles (normal 3/4,
  low/oblique revealing more horizon, straight top-down) against the
  real saved chess set -- confirmed visible board edge thickness with a
  believable wood-grain frame at every angle, a clearly visible textured
  ground plane extending to a horizon, and shadows (Phase 3) correctly
  landing on both the frame and the ground plane, not just the 64
  squares.

## Phase 5: Final polish + combined re-verification

- [x] Rook-crenellation texturing polish -- skipped (cut for time, as
  the plan allowed): the combined lighting/shadow/board improvements
  made this a much smaller relative issue than when first noted in the
  visual assessment.
- [x] Rebuilt the full corrected+enhanced scene on the real saved chess
  set: 4 fixed knight heads, wooden board frame + ground plane, the
  3-light setup (warm key + cool fill + warm point accent), shadows, and
  supersampled AA all together. Re-exported a new combined benchmark
  (`test/chess_lit_animated_realistic.mov`, 1280x960, 30fps, 121
  frames, one pawn animated moving across the board while the camera
  does a full orbit), verified via `ffprobe` + `ffmpeg`-extracted
  start/mid/end frames, each read and visually inspected -- confirms
  every phase's work rendering correctly together in one export, not
  just individually.
- [x] Full clean build under `MEP_STRICT_FLAGS`, `just test` passes
  (same pre-existing unrelated `mep-collab-session-test` failure).
- [x] `MEP_AGENT_API.md` (`supersample?` documented on both render
  tools' prose, not just their MCP schemas; the stale "No shadows" line
  under Lighting replaced with a real "Shadows" explanation) and the
  `mep-3d-modeler` skill (lighting bullet, render-to-image bullet)
  updated to match.
- [x] **Verified via:** the corrected scene (75 objects: the original
  73 minus 4 broken knight heads plus 4 corrected ones, plus the new
  frame and ground plane) saved back to `test/chess_set_full.gltf` --
  recovered from the same `file.save`/buffer-focus pitfall Phase 1 hit
  (forced the GUI pane onto the edited buffer via `command.run` + `lua
  mep.buffer_switch`), saved first to a scratch verification path and
  independently checked (glTF JSON parsed, node/mesh counts and the
  expected vertex/triangle-count histogram -- 4x (276,92) knight heads,
  1x (24,12) frame, 1x (4,2) ground plane, everything else unchanged --
  all confirmed) before overwriting the real file.

### Non-goals

- Shadows from only one designated directional light, not every light.
- No PBR-correct soft/area-light shadows, cascaded shadow maps, or
  ambient occlusion.
- No general per-triangle multi-material engine feature -- multiple
  single-material objects instead.
- Not re-litigating piece geometry beyond the knight-head bug fix.
- Rook-crenellation texturing left as-is (cut for time).
