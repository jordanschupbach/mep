# Multi-light rendering + object animation

Attempts two features explicitly deferred as Non-goals earlier this
session: multiple light sources (`CHESS_SET_BENCHMARK_PLAN.md` Phase 1)
and object/mesh animation (`ANIMATION_VIDEO_PLAN.md`'s Non-goals).

**How to resume:** check the boxes below, `git log --oneline -- src/gfx/backend_native_renderer3d.cpp src/model3d_doc.cpp`
for what's landed, continue with the first unchecked part. Same rigor
as every other plan here: implement -> `nix develop --command cmake
--build build/native` -> `just test` -> live-verify via the real
running `mep` instance over the agent-control socket (screenshots + RPC
calls + independent `ffprobe`/`ffmpeg` checks where relevant) -> tick
the box with a "Verified via:" note -> next part, pausing for the
user's go-ahead unless told to keep going.

---

## Why this plan exists

`CHESS_SET_BENCHMARK_PLAN.md`'s Phase 1 Non-goals flagged "multiple
lights" as out of scope; `ANIMATION_VIDEO_PLAN.md`'s Non-goals flagged
"Object/mesh animation (moving pieces, not just the camera)" the same
way. The user asked to attempt both.

Research confirmed: the mesh fragment shader
(`backend_native_renderer3d.cpp`'s `kMeshFragmentSrc`) already does
real per-fragment Blinn-Phong with roughness/metallic, but the light
itself is one hardcoded C++ constant with no color uniform and no
scene-level light concept at all (`Scene`/`Object3D` have no light
type). The camera-keyframe system (`Model3DSession::CameraKeyframe`,
`Model3DSampleCameraAtTime`, `Editor::Model3DAnim*`,
`Model3DRenderAnimationToVideoFile`) is a clean, small pattern that
generalizes directly to per-object animation; `Object3D::id` is a
stable key across undo/redo for this purpose.

Design choice, made rather than asked (see the approved plan's own
Context section for the full reasoning): lights are real scene content
(`Scene::lights`, undo-aware, like any other object); object animation
tracks are session-only on `Model3DSession`, mirroring
`camera_keyframes` exactly, not saved/undoable.

---

## Part A: Multiple light sources

- [x] `struct Light` + `Scene::lights` in `model3d_doc.h` (id shares
  `next_object_id` with objects; undo-aware for free via the existing
  whole-`Scene`-snapshot undo).
- [x] Multi-light fragment shader (`backend_native_renderer3d.cpp`):
  `uLightCount`/`uLightType[]`/`uLightPosOrDir[]`/`uLightColor[]`/
  `uLightIntensity[]`/`uLightRange[]` (`MEP_MAX_LIGHTS = 8`), looped
  Blinn-Phong accumulation, simple linear point-light falloff.
  Empty-`lights` scenes fall back to today's single hardcoded key light
  -- must render byte-similar to before.
- [x] `Editor::Model3DAddLight`/`SetLight`/`DeleteLight` (editor.h/.cpp),
  undo-tracked. (`ListLights` turned out not to need a dedicated Editor
  method -- `model.listLights` reads `sess.scene.lights` directly via
  `RequireModel3D`, matching how `model.getSelection` already works.)
- [x] RPC: `model.addLight`, `model.setLight`, `model.deleteLight`,
  `model.listLights` (agent_rpc.cpp) + matching `mep_model_*` MCP tool
  entries (mcp_bridge.cpp).
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (new `gfx::SceneLight`/`gfx::SetSceneLights` backend API, plus
  `Uniform1iv`/`Uniform1fv`/`Uniform3fv` added to `gl_loader.h`/`.cpp`
  since only scalar uniform setters existed before), `just test` passes
  (same pre-existing unrelated `mep-collab-session-test` failure).
  Live, two checks: (1) **backward compatibility** -- opened the real
  saved chess set (0 lights), rendered, confirmed visually identical
  lighting/shading character to every prior render this session (the
  empty-`lights` fallback path). (2) **actual multi-light rendering**
  -- a plain gray sphere with a red point light at x=-3 and a blue
  point light at x=+3 (plus a dim green directional light from above)
  renders with a clean, correctly-positioned red highlight on its left
  side and blue highlight on its right, blending smoothly between --
  `model.listLights` also confirmed all 3 lights' full state round-
  tripped correctly through add/set/list.

## Part B: Object/mesh animation

- [x] `Model3DSession::ObjectKeyframe` + `object_keyframes` map
  (editor.h), mirroring `CameraKeyframe`/`camera_keyframes` exactly.
- [x] `Model3DSampleObjectAtTime` free function, mirroring
  `Model3DSampleCameraAtTime` exactly (empty-track fallback reads the
  live object's current transform).
- [x] `Editor::Model3DAnimAddObjectKeyframe`/`ClearObjectKeyframes`/
  `SetObjectTime`/`Model3DAnimMoveObject` (convenience two-keyframe
  linear move, mirroring `Model3DAnimOrbitCamera`'s role) -- none
  undoable, matching camera's own choice.
- [x] RPC: `model.animAddObjectKeyframe`, `model.animClearObjectKeyframes`,
  `model.animSetObjectTime`, `model.animMoveObject` + matching MCP tools.
- [x] `Model3DRenderAnimationToVideoFile` loosened to accept camera-only,
  object-only, or both; duration = max end time across every track;
  per-frame sample+substitute+restore for every animated object,
  alongside the existing camera substitution.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (same pre-existing unrelated
  `mep-collab-session-test` failure). Live, two checks against the real
  saved chess set (`test/chess_set_full.gltf`, re-imported -- confirmed
  live that re-imported glTF objects report `position: {0,0,0}` since
  import bakes transforms into mesh vertices, so `animMoveObject`'s
  `from`/`to` are relative offsets, not absolute board coordinates).
  (1) **object-only animation**: `model.animMoveObject` on one pawn
  (relative `z: 0 -> 2` over 3s) with a static camera, exported via
  `model.renderAnimationToVideo`, frames extracted with `ffmpeg` at
  start/mid/end -- the pawn visibly and smoothly separates from its
  rank, ending two squares forward. (2) **combined**: added
  `model.animOrbitCamera` (full revolution, same 3s duration) on top of
  the still-active pawn move and re-exported -- mid-clip frame shows the
  camera fully orbited to the opposite side of the board (black/white
  sides visually swapped) while the pawn continues its move; end frame
  shows the camera back at its start orientation with the pawn at its
  final moved position -- both animations run correctly together in one
  export.

## Part C: End-to-end + docs

- [x] Combined benchmark: real chess-set scene, 3 lights (warm
  directional key, cool dim directional fill, warm point accent)
  replacing the flat single key light, one pawn animated moving 2
  squares forward while the camera does a full 360 orbit, both over the
  same 4s duration -> `test/chess_lit_animated.mov` (30fps, 1280x960,
  121 frames).
- [x] `just test` clean (same pre-existing unrelated
  `mep-collab-session-test` failure -- it requires a websocket URL arg
  and always fails the same way outside that harness).
- [x] `MEP_AGENT_API.md` (new "Lighting" subsection covering
  `add_light`/`set_light`/`delete_light`/`list_lights`; "Animation &
  video export" extended with the object-animation tools and a combined
  worked example; "Building geometry" got a new glTF-import
  transform-baking caveat) + `.claude/skills/mep-3d-modeler/SKILL.md`
  updated to match (tool list, lighting-model bullet, animation bullet).
- [x] **Verified via:** full clean rebuild under `MEP_STRICT_FLAGS`
  after the doc changes (no code changes in this part, so this just
  confirms nothing broke), `just test` passes (same pre-existing
  failure as always). Live: opened the real saved chess set, added the
  3 lights (confirmed round-tripped via `model.listLights`), took a
  still render confirming the warm point-light rim glow and richer
  combined shading are visible. Then `model.animMoveObject` on one pawn
  (relative `z: 0 -> 2`, matching the transform-baking caveat) +
  `model.animOrbitCamera` (full revolution) together, exported via
  `model.renderAnimationToVideo`, and independently verified with
  `ffprobe` (121 frames, 4.03s, 30fps, 1280x960 -- matches the request)
  and `ffmpeg`-extracted frames at 0/30/60/120: frame 0 shows the
  starting board orientation with the lighting visible; frame 30 (~1/4
  through) shows the camera already 90 degrees around; frame 60 (~1/2
  through) shows white/black sides swapped (~180 degrees); frame 120
  (end, one full revolution) shows the camera back at its starting
  orientation *and* the animated pawn clearly separated two squares
  forward from its rank -- both animations run correctly together
  across the whole clip, not just at the endpoints.

### Non-goals

- Shadows, environment-map reflections, full Cook-Torrance/GGX PBR.
- Point-light falloff stays simple linear, not inverse-square/PBR.
- Ambient stays the existing flat baked-in floor term, not a separate
  configurable light/IBL source.
- Object animation tracks are session-only, not saved/undoable.
- No rotation/scale convenience wrapper beyond `Model3DAnimMoveObject`
  (position-only) -- `Model3DAnimAddObjectKeyframe` covers the general
  case.
- No skeletal/bone animation, no easing curves (linear only).
