# Professional-grade 3D modeler + image editor: chess-set benchmark

Makes mep's 3D modeler and image editor capable enough — both by hand and
driven entirely by an AI agent through the agent-control RPC socket — to
produce a professional-looking, realistically textured 3D asset.
Benchmark: "create a 3D model of a chess set, with textures and enough
detail to look realistic," attempted for real at the end (Phase 5), not
just assumed to work once the pieces are in place.

**How to resume:** check the boxes below, `git log --oneline -- src/gfx/backend_native_renderer3d.cpp src/model3d_doc.cpp src/image_procgen.cpp`
for what's landed, continue with the first unchecked phase. Same rigor as
every other plan here: implement -> `nix develop --command cmake --build
build/native` -> `just test` -> live-verify via the real running `mep`
instance over the agent-control socket (screenshots + RPC calls) -> tick
the box with a "Verified via:" note -> next phase, pausing for the user's
go-ahead unless told to keep going.

---

## Why this plan exists

Two research passes (direct code reads) assessed both subsystems against
the benchmark.

**3D modeler** — a genuinely strong document model and agent RPC surface
already exist: `Scene`/`Object3D`/`MeshData` (`src/model3d_doc.h`)
supports many objects per scene (a chess set's ~32 pieces + board is a
non-issue), real vertex/face editing (extrude/inset/subdivide/dissolve,
all connectivity-aware), OBJ/glTF/IQM/VOX/M3D/.blend import, and ~40
`model.*` RPC methods (`src/agent_rpc.cpp:878-1276`) already let an agent
build/transform/texture/render a scene with zero mouse/keyboard
simulation. Two hard gaps block the benchmark:
1. **The renderer has no lighting model at all.**
   `backend_native_renderer3d.cpp`'s mesh shader is exactly `FragColor =
   texture(uTex, vTexCoord) * uColor * vVertColor` -- flat, unlit, no
   normals even uploaded to the GPU. No mesh or texture quality can look
   realistic under this renderer.
2. **No lathe/revolve primitive.** Chess pieces are archetypally turned
   forms (king/queen/bishop/rook/pawn each need a distinct silhouette of
   revolution). Only 7 fixed primitives exist (Cube/Sphere/Cylinder/
   Cone/Plane/Torus/Wedge); building a smooth turned profile by
   hand-placing vertices is impractical.

**Image editor** — a real multi-layer RGBA8 model exists
(`ImageEditorSession`/`ImageEditorLayer`, `src/editor.h:986-1040`) with
working paint tools (Pencil/Eraser/Line/Rectangle/Ellipse/Bucket/
selection/Move), but two gaps block the benchmark:
3. **Zero procedural texture generation.** No noise, gradient, blur, or
   pattern generator exists anywhere in the codebase -- a wood-grain or
   marble material texture can currently only be hand-painted pixel by
   pixel.
4. **Zero agent-facing API.** Unlike the 3D modeler's ~40 `model.*`
   methods, there is not one `image.*` RPC method -- an agent's only
   access to the image editor today is simulating raw mouse clicks/
   drags, impractical for anything beyond trivial shapes.

**Scoping decision, put to the user directly**: how far to push renderer
realism. Between "simple lighting only" and "simple lighting + basic PBR
material slots," **the user chose basic PBR material slots** -- Phase 1
below includes a normal/roughness/metallic material model, not just
diffuse shading.

## Non-goals (explicit, so scope stays honest)

- Full Cook-Torrance/GGX microfacet PBR, multiple lights, shadows, or
  environment-map reflections -- Phase 1's specular is a deliberately
  cheap Blinn-Phong approximation, not a spec-compliant PBR pipeline.
- Sculptural/asymmetric modeling tools (needed for a fully faithful
  knight) -- the lathe primitive covers every *turned* piece; a knight
  remains a manual vertex-editing or import job, same as today.
- JPEG/BMP export from the image editor (PNG export is the only new
  export path Phase 4 adds).

## Phase 1: Renderer lighting + basic PBR materials (the blocker) -- DONE

- [x] **Shader** (`src/gfx/backend_native_renderer3d.cpp`): added `aNormal`
  (location 3) and `aTangent` (location 4, vec3 -- bitangent
  reconstructed in-shader via `cross(normal, tangent)`) to the mesh
  vertex format; `UploadMesh` extended to 6 VBOs (was 4), with a
  defensive area-weighted-accumulate-then-normalize fallback
  (`ComputeFallbackNormalsAndTangents`) for any caller that doesn't
  supply its own (`BuildModel3DGpuMesh`, main.cpp, already always did
  for normals; now covered for every direct `gfx::Mesh` builder too).
  Vertex shader passes world-space position and normal/tangent
  transformed by a proper normal matrix (`UploadNormalMatrix` --
  `transpose(inverse(model's upper 3x3))`, computed once per `DrawMesh`
  call). Fragment shader builds a TBN matrix, samples an optional
  tangent-space normal map through it; simplified metallic-roughness
  shading (Lambertian diffuse scaled by `(1 - metallic)`, Blinn-Phong
  specular with `shininess = mix(128, 4, roughness)` and specular tint
  `mix(vec3(0.04), albedo, metallic)`), one fixed world-space
  directional key light + a 0.35 ambient floor term.
- [x] **Geometry**: normals were already handled (`MeshData::
  RecalculateNormals`, pre-existing); tangent computation folded
  directly into `UploadMesh`'s own fallback (`ComputeFallbackNormalsAndTangents`)
  rather than a separate `MeshData::RecalculateTangents()` -- simpler,
  and covers every `gfx::Mesh` producer (imports, smoke mains, the
  document-editing path) in one place instead of needing each to opt in.
- [x] **Material model** (`Object3D`, model3d_doc.h): added
  `normal_map_index`/`roughness_map_index`/`metallic_map_index` (int,
  -1 = none) and `roughness`/`metallic` (float scalars, defaults 0.5/0.0).
  glTF importer picks up `pbrMetallicRoughness.baseColorTexture`/
  `roughnessFactor`/`metallicFactor` and `material.normalTexture`
  directly; deliberately does NOT unpack the combined
  `metallicRoughnessTexture` (glTF's G=roughness/B=metallic packed
  image) into this app's two independent single-channel slots --
  real extra import-time image-repacking work for a case the benchmark
  (procedural/scalar-driven materials) doesn't need; the scalar
  factors are still read regardless (see LoadGltfMaterial's own
  comment). OBJ/other importers leave scalars at their defaults.
- [x] **RPC** (`src/agent_rpc.cpp`): `model.setMaterial` accepts
  optional `roughness`/`metallic`; `model.setTexture` accepts an
  optional `kind` param (`"albedo"|"normal"|"roughness"|"metallic"`,
  default `"albedo"` -- preserves every existing caller).
- [x] **Verified via:** `mep-gfx-native-model-smoke` rendering a real
  glTF import (greenman.glb) before/after -- previously a flat solid-
  white silhouette (this session's earlier GLFW-verification
  screenshot), now shows real diffuse shading gradients and a visible
  specular highlight. Live RPC verification: `model.addPrimitive`
  (sphere) + `model.renderToImage` with default material shows a clear
  Lambertian shading gradient + soft highlight (not a flat disc);
  `roughness=0.1,metallic=1.0` renders a mostly-dark sphere with a
  tight, albedo-tinted (gold) specular highlight; `roughness=0.9,
  metallic=0.0` on the same color renders full diffuse gold with a
  broad/near-invisible highlight -- a stark, correct visual distinction
  between shiny-metal and rough-matte. `just test`'s model3d-doc-test
  and every other non-GUI test still pass; full native build clean
  under `MEP_STRICT_FLAGS`.

## Phase 2: Lathe/revolve primitive -- DONE

- [x] New `AddLatheToScene(Scene*, const std::vector<Vec2f> &profile,
  int segments, bool cap_top, bool cap_bottom)` in model3d_doc.cpp:
  revolves a 2D profile (radius, height pairs) around the Y axis,
  `segments` rings connected by quads, fan-triangulated caps where the
  profile doesn't already close to radius 0. Real per-vertex UVs (u =
  angle/2pi, v = normalized profile-height). Welded/indexed mesh with
  analytically-computed normals from the profile's own central-differenced
  tangent (not face-averaging), so welding costs nothing in shading
  smoothness. Result tagged `PrimitiveKind::Imported` (a variable-profile
  shape isn't "regenerate at a new size"-safe the way fixed `GenMesh*`
  primitives are -- documented reasoning, not an oversight).
- [x] `Editor::Model3DAddLathe(buffer_id, profile, segments, cap_top,
  cap_bottom)` (editor.h/.cpp), structurally identical to
  `Model3DAddPrimitive` (undo push, quick-add offset, selection/dirty).
- [x] New RPC `model.addLathe` (buffer_id, profile=[[radius,height],...],
  segments, cap_top, cap_bottom, optional transform) -- transform-apply
  code shared with `model.addPrimitive` via new `ApplyOptionalTransform`
  helper.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (sign-conversion fixes applied to the row-indexing math), all 5
  non-GUI tests pass. Live RPC: built an 11-point pawn-like profile
  (base/stem/collar/rounded head) via `model.addLathe`, set a
  brown/wood `model.setMaterial` (roughness=0.4, metallic=0.0),
  `model.renderToImage` -- resulting 500x500 PNG shows a smooth, closed,
  correctly-proportioned pawn silhouette (rounded base, narrow stem,
  collar ring, rounded head) with correct Phase-1 lit shading (bright
  highlight, soft gradient falloff, no inverted-normal dark bands, no
  visible cap holes at top or bottom).

## Phase 3: Image editor -- procedural texture generation -- DONE

New module `src/image_procgen.h`/`.cpp` (pure functions over an RGBA8
buffer + width/height, deterministic given a seed):
- [x] Value noise + fBm (fractal sum of octaves) -- shared building
  block for both patterns below. `ValueNoise2D` (hashed-lattice +
  smoothstep-eased bilinear interpolation) and `Fbm2D` (normalized sum
  of octaves at doubling frequency/halving amplitude).
- [x] Linear/radial gradient fill (`FillLinearGradient`/
  `FillRadialGradient`).
- [x] Wood grain (`FillWood`): concentric rings from the buffer center,
  radial distance warped by fBm, banded via `sin()` between two colors.
- [x] Marble (`FillMarble`): fBm turbulence fed through
  `sin((x+y)*scale + turbulence*fbm)`, mapped between two colors.
- [x] Simple box blur (`BoxBlur`): separable, 2-pass, edge-clamped.
- [x] Wired into the build: `image_procgen.cpp` added to `mep_core`
  (so Phase 4's RPC layer can call it) and to the standalone
  `mep-image-procgen-test` target/`just test` loop (windowless,
  pure-CPU, no GL context needed -- same shape as `mep-html-doc-test`).
- [x] **Verified via:** all under `MEP_STRICT_FLAGS`, clean build.
  `mep-image-procgen-test` checks: `Fbm2D`/`FillNoise` determinism
  (same seed -> byte-identical, different seed -> different), buffer
  sizing, linear-gradient edge values increase left-to-right, radial
  gradient darker at center than corner, wood/marble red-channel range
  > 50 (actually varies, not a flat fill), box blur reduces
  adjacent-pixel deltas vs. raw noise and is itself deterministic.
  Standalone sample dump (opt-in via an argv[1] output dir, so `just
  test`'s no-arg run doesn't litter the working directory) generated
  and visually inspected via the Read tool: linear/radial gradients
  show smooth correct falloff; noise and marble show organic mottled
  variation; wood shows a clear concentric-ring pattern; blurred noise
  is visibly softer than the raw noise sample.

## Phase 4: Image editor -- agent RPC surface + by-hand UI -- DONE

- [x] **RPC** (`src/agent_rpc.cpp`, new `RequireImageEditor` helper
  mirroring `RequireModel3D`, plus `RequireColorParam`/
  `RequireImageEditorSize`): `image.new` (headless buffer creation --
  new `Editor::NewImageEditorBuffer`, mirroring `NewModel3DScene`'s "no
  source file needed" shape, since `EnterImageEditor` always requires
  an already-open ImageDoc), `image.info`, `image.newLayer` (reuses the
  existing `ImageEditorAddLayer` + new `ImageEditorRenameLayer`),
  `image.setActiveLayer`, `image.exportPng` (new `Editor::
  ImageEditorExportPng` wrapping `GetImageEditorMutable` +
  `SaveImageEditorPng` for RPC callers with only a buffer id);
  procedural `image.fillGradient` (linear/radial)/`image.fillNoise`/
  `image.fillWood`/`image.fillMarble`/`image.blur` -- all funnel
  through one new `Editor::ImageEditorApplyPixels` (pushes undo,
  respects the active selection via the existing
  `ImageEditorSelectionContains`, else overwrites the whole layer) and
  `Editor::ImageEditorBlurLayer` (box-blurs a copy via
  `image_procgen.h`'s `BoxBlur` then reuses `ImageEditorApplyPixels`
  for the same undo/selection handling).
- [x] **By-hand UI** (main.cpp's `DrawImageEditorPane`): a new
  "Texture" menubar menu -- Fill Gradient (Linear/Radial), Fill Noise,
  Fill Wood, Fill Marble, Blur, each gathering 1-2 numeric params via
  chained `BeginPromptNative` prompts (the same pattern the Mesh menu's
  "Extrude Faces.../Inset Faces..." items already use) and using the
  existing primary/secondary color swatches for color_a/color_b: no
  new color-input UI needed. Calls the exact same `Editor::
  ImageEditorApplyPixels`/`ImageEditorBlurLayer` methods the RPC
  surface uses -- one implementation, two entry points.
- [x] **Bug found and fixed along the way**: the image editor's
  menubar dropdown (File/Edit/Layer/View, pre-existing, and now
  Texture) drew its popup panel immediately after registering it,
  *before* the toolbar/canvas/layers-sidebar below -- those later
  opaque draws painted right over it every frame, so no image-editor
  menu's dropdown was ever actually visible on screen despite the
  click-to-open state toggling correctly underneath (only discovered
  because this phase's own live verification actually looked at a
  screenshot instead of trusting the RPC call succeeding). Fixed by
  splitting into a registration half (unchanged position) and a
  drawing half moved to the very end of `DrawImageEditorPane`, the
  exact same "registration vs. drawing" split MODEL3D_PLAN.md's own
  menubar dropdown already uses (with an identical historical comment
  explaining why) -- this file just hadn't gotten the same fix.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`, all
  6 windowless tests pass. Live RPC: `image.new` -> `image.fillWood` ->
  `image.exportPng` -> `model.setTexture(kind="albedo")` onto a
  Phase-2 lathed pawn -> `model.renderToImage` -- the wood-grain
  texture wraps correctly around the lathe's UVs, producing a
  convincing turned-wood look under Phase 1's lighting. Also verified
  `image.newLayer`/`image.setActiveLayer`/`image.blur` (blurred layer
  0 only, left the blank overlay layer untouched, confirming
  layer-index targeting works). By-hand UI: screenshot-checked
  mouse/keyboard RPC (`ui.mouse_click`, `ui.key_press` with X11 keysym
  names like `"Return"`, not `"Enter"`) drove Texture > Fill Wood...,
  confirmed the dropdown now renders, both chained prompts appeared
  with correct labels/defaults, and accepting them painted a real
  wood-ring pattern onto the canvas live.

## Phase 5: End-to-end benchmark + documentation -- DONE

- [x] Attempted the benchmark for real: built a scene (`model.new`) with
  a board plane plus 3 lathed pieces (pawn, rook, bishop -- distinct
  hand-authored `[radius,height]` profiles per the Non-goals' own
  scoping, no knight since that needs asymmetric sculptural modeling
  this plan explicitly excludes), each textured via a real generated
  PNG (`image.new` -> `image.fillWood`/`image.fillMarble` ->
  `image.exportPng` -> `model.setTexture(kind="albedo")`) with tuned
  per-piece roughness/metallic (wood pieces ~0.4-0.45/0.0, marble rook
  0.15/0.0 for a glossier look), `model.frameAll` + `model.renderToImage`
  at 900x650.
  **Honest assessment of the render**: the wood grain and marble
  texturing read clearly and convincingly under Phase 1's lighting --
  genuinely the strongest part of the result. The lathed silhouettes are
  recognizable but stylized/simplified versions of real chess pieces
  (thinner waists than typical, the rook has no crenellations -- a
  lathe can only produce rotationally-symmetric forms, and crenellations
  aren't one), and the board is a plain colored plane with no checker
  pattern (no checker/tile generator exists in `image_procgen.h` --
  a real gap, not attempted). Net: not photorealistic, but a genuine,
  recognizable, well-textured turned-wood/marble chess set -- a fair
  result given the plan's own Non-goals (no full PBR, no shadows/
  reflections, no sculptural knight).
- [x] Wrote `MEP_AGENT_API.md` (restored from its pre-3D-modeler
  version at commit 86e5a64, deleted in 76f30ed "Add 3d modeller" and
  never replaced -- the dangling reference `.claude/skills/mep-3d-
  modeler/SKILL.md` and main.cpp's own built-in `kBuiltinAiTerminal`
  help text pointed at was real). Added two new major sections ("The
  in-pane 3D modeler", "The in-pane image editor" headless subsection)
  documenting every `model.*`/`image.*` method from Phases 1/2/4, plus
  a worked end-to-end example (lathe -> procedural texture -> render).
  Also **found and fixed the same staleness in two other places** while
  cross-checking this doc against the actual code: `kBuiltinAiTerminal`
  (main.cpp, injected into every `<leader>a<CR>` AI-terminal session)
  still claimed "no metallic/roughness -- this app's rendering has no
  lighting model to show them" and never mentioned `add_lathe`/
  `image.*` at all -- fixed both, plus added the missing
  `mep_model_add_lathe` and 10 `mep_image_*` tool registrations to
  `src/mcp_bridge.cpp`'s `kTools` table (verified live via the real MCP
  stdio protocol: `tools/list` now shows all 11 new tools, and
  `tools/call` on `mep_image_info`/`mep_model_add_lathe` round-trips
  correctly) -- without this fix, an agent driving mep through the
  standard MCP tool-calling interface (as opposed to raw socket
  JSON-RPC) literally could not have reached Phase 2/4's new
  capabilities at all, a real gap this cross-check caught. Also updated
  `.claude/skills/mep-3d-modeler/SKILL.md`'s own stale "no lighting
  model" claim and added `add_lathe`/`image.*` mentions.
- [x] Updated `IMAGE_EDITOR.md`'s Phase 3 checklist: checked off
  "Gradient tool (linear/radial)" and "Filters: ... noise" (with notes
  on how Phase 4's generator-based versions differ from a traditional
  interactive gradient tool / filter-over-existing-paint) -- left every
  other item (soft brushes, lasso/pen, text tool, blend modes, layer
  masks, adjustments, sharpen/pixelate, onion-skinning) unchecked since
  none of them were touched by this plan.

## Follow-up: exporting the models + GUI "Toggle Textures"/"Toggle Lighting" -- DONE

Requested after Phase 5: save the actual chess-set 3D models (not just a
render), and make the renderer's textured/lit look toggleable from the
3D modeler's own GUI, not just RPC params.

- [x] **glTF export bug fix** (`SaveModel3DGltf`, `model3d_doc.cpp`):
  previously hardcoded every exported material's `metallicFactor: 0.0`/
  `roughnessFactor: 0.8` regardless of the object's own tuned
  `roughness`/`metallic` (Phase 1 added those fields; the exporter was
  never updated to read them) -- any save would have silently discarded
  a piece's real material. Now writes `obj.roughness`/`obj.metallic`
  directly, and exports `normalTexture` (glTF's own dedicated slot) when
  a normal map is set. Deliberately does NOT pack roughness/metallic
  *maps* into glTF's combined `metallicRoughnessTexture` (no 1:1 slot
  for this app's two independent single-channel textures, same reasoning
  as `LoadGltfMaterial`'s existing import-side scope cut) -- a documented
  gap, not an oversight. Verified: saved the chess scene, parsed the
  resulting `.gltf`'s `materials[].pbrMetallicRoughness` back with a
  plain JSON reader -- the 4 objects' roughness values (0.7/0.4/0.15/
  0.45) match exactly what `model.setMaterial` set, and the 3 textured
  pieces' `baseColorTexture` round-trips too. Saved to
  `test/chess_set_scene.gltf` (self-contained, base64-embedded buffer +
  3 textures, 4 nodes) via `file.save`.
- [x] **"Toggle Textures"/"Toggle Lighting" GUI + RPC parity**: new
  `Model3DSession::show_textures`/`unlit` fields (editor.h), two new
  View-menu items in `DrawModel3DPane` mirroring "Toggle Wireframe"
  exactly, threaded through `SetModel3DObjectMaterial`'s new
  `apply_textures` param (skips every map, falling back to plain color)
  and a new `gfx::SetUnlitMode` renderer method (new `uUnlit` fragment-
  shader uniform, short-circuits straight to raw textured/tinted color
  before any lighting math runs) -- both the live viewport and
  `model.renderToImage`/`Model3DRenderToImageFile` read them the same
  way `show_grid`/`wireframe` already do, and `model.setView` gained
  matching sparse `show_textures`/`unlit` params (Lua/RPC/`mep_model_set_view`
  MCP schema all updated). **Verified via:** `model.renderToImage` with
  all 4 combinations of `show_textures`/`unlit` -- default (lit+textured,
  matches the original benchmark render), `show_textures:false` (flat
  per-object colors, still shaded), `unlit:true` (raw texture/color, no
  shading gradient), and both together (flat white silhouettes) all
  render exactly as designed.
- [x] **Pre-existing bug found (unrelated to this feature) -- root
  cause found and fixed**: the live 3D-modeler viewport never actually
  rendered visible content -- Outliner/Inspector/status bar all
  correctly reflected the scene and mouse clicks landed, but the
  render-texture-then-blit-to-screen viewport itself showed solid black
  (no grid, no objects), even in a brand-new split pane. Confirmed via
  `git stash` + rebuild that this reproduced identically on unmodified
  `main` (predates every change in this plan) -- not a regression from
  the toggle work. Root-caused by bisecting the pipeline with temporary
  diagnostics (dumped the live viewport's own render-texture to a PNG
  right after rendering -- came out perfectly correct, isolating the bug
  to the blit step; logged the blit's scissor-rect math -- also
  correct, ruling that out too): `DrawTextureRec` (`backend_native_
  renderer2d.cpp`) naively copied its `source` rectangle's width/height
  straight into the *destination* quad's size. The live viewport's own
  blit call deliberately passes a **negative** `source.height` (a
  common trick to flip which rows of a bottom-up-stored render-texture
  get sampled) -- but `DrawTextureRec` let that same negative value
  become the destination quad's height too, pushing the quad's geometry
  *upward*, off the top of its own scissor rect, instead of downward
  into the viewport -- so it was scissored away in full, every frame,
  rendering nothing while every surrounding computation (camera,
  render-to-texture, scissor bounds) was individually correct. This is
  why offscreen `model.renderToImage` was never affected: it reads the
  render-texture back directly via `glReadPixels`, never routing
  through `DrawTextureRec` at all. Fixed by taking the absolute value of
  `source.width`/`source.height` when building the destination rect
  (`std::fabs`), leaving the original signed values in `source` itself
  so the UV-flip behavior `DrawTextureRec`'s only caller relies on is
  unaffected. **Verified via:** live `ui.screenshot` of the 3D-modeler
  pane after the fix -- grid, sphere, and its selection outline all
  render correctly; clicked View > Toggle Lighting live and confirmed
  the on-screen sphere visibly flattens to unlit raw color with the
  selection-outline click also working as expected, end-to-end in the
  actual GUI (not just via the offscreen render path used for every
  earlier verification in this plan).

## Phase 7: full 32-piece set, real board, and a real texture-realism fix -- DONE

Requested after the single-piece benchmark: "make the chess set a full
set of pieces on a board," and fix "something off about these textures
and how they render on the pieces." Both landed together since the
texture bug turned out to be the actual root cause of the "off" look.

- [x] **Root-caused the texture complaint**: `FillWood`'s concentric
  rings are a *cross-section* pattern -- what a tree's end grain looks
  like face-on -- correct for a round tabletop, wrong for a lathe
  object's sides. Wrapping concentric rings around a cylinder via
  angle-mapped U turns them into a spiral/barber-pole, not wood grain,
  which is exactly what every piece in the original benchmark render
  showed once looked at closely. Documented the distinction directly in
  `image_procgen.h`'s own comments so the mistake doesn't get repeated.
- [x] **`FillWoodTurned`** (`image_procgen.h`/`.cpp`): a second wood
  generator built for the case `FillWood` doesn't cover -- a surface
  UV-wrapped circumferentially (u=angle/2pi, v=length, exactly
  `AddLatheToScene`'s own convention). Grain streaks run lengthwise
  along v (matching how real wood-turning grain looks, since a log's
  growth rings run parallel to the lathe axis) and are seamless across
  the u=0/u=1 wrap by construction: each streak's position comes from
  sampling `Fbm2D` at a point on a circle (`cos(u*2pi), sin(u*2pi)`)
  rather than at u directly, so u=0 and u=1 sample the identical point
  -- no visible seam, verified by a dedicated left/right-edge-continuity
  check in `mep-image-procgen-test` (not just eyeballed). Exposed as
  `image.fillWoodTurned` (RPC) / `mep_image_fill_wood_turned` (MCP) /
  "Fill Wood (Turned)..." (Texture menu), mirroring every existing
  `image.fill*` method's shape exactly.
- [x] **`FillCheckerboard`**: `squares_x` by `squares_y` alternating
  tiles -- the missing board-pattern generator flagged as a real gap
  back in Phase 5's benchmark assessment. Exposed as
  `image.fillCheckerboard` / `mep_image_fill_checkerboard` / "Fill
  Checkerboard..." -- same pattern as every other generator.
- [x] **New piece profiles** for the 3 piece types the original
  benchmark didn't attempt, plus a rounder pawn head (the original's
  sharp cone top read as a spike, not a ball):
  - Queen: a taller lathe profile with a flared crown bump before
    tapering to a point -- pure lathe geometry, no compound parts.
  - King: the tallest lathe body (a flat disc top, `cap_top: true`,
    to give the topper something to sit on) plus a small cross built
    from two `cube` primitives (a vertical + a horizontal bar) grouped
    onto it via `model.groupObjects` (organizational only -- each piece
    is still positioned independently in world space, not via parent-
    child transform composition, matching how grouping is documented to
    work everywhere else in this codebase).
  - Knight: **no literal horse-head sculpting** (still out of scope,
    per this plan's original Non-goals -- real sculptural modeling, not
    a quick add). Instead: the same lathe base/stem shape as the other
    pieces, topped with a `cone` primitive tilted forward via
    `rotation.x` (base-pivoted primitives lean naturally when rotated,
    since the rotation pivots around the object's own base) and
    Y-rotated per side to lean toward the opponent. Reads as a distinct,
    asymmetric silhouette -- the functionally important property for a
    playable-looking set (every piece type instantly distinguishable) --
    without claiming to be a realistic horse head. Iterated once on the
    exact tilt/scale after a close-up render showed the first attempt
    (a flat `wedge` primitive) looked like a plank, not a head.
- [x] **Full 32-piece scene**: standard starting position (back rank
  rook/knight/bishop/queen/king/bishop/knight/rook, 8 pawns per side),
  built via a batch script (`build_full_chess_set.py`, kept in the
  session scratchpad, not committed -- see its own header) that opens
  ONE persistent connection to the agent socket and issues all ~130
  RPC calls over it, instead of one `python3 rpc_client.py` subprocess
  per call (which would have meant ~130 process spawns). **Real gotcha
  hit and fixed while writing it**: a persistent connection can have
  server-initiated event notifications interleaved with RPC responses
  on the same socket, so a client that naively assumes "the next frame
  I read is the response to the request I just sent" can desync and
  crash (hit this firsthand -- the response to a `model.addPrimitive`
  call came back as a `KeyError` because an unrelated notification frame
  arrived first). Fixed by matching each response's `"id"` against the
  request that asked for it and skipping anything that doesn't match --
  worth remembering for any future batch-RPC script, and now called out
  in `MEP_AGENT_API.md`'s own Architecture section.
- [x] **Board**: a single `plane` primitive (scale 8x8 -- primitives are
  1x1 unit by default, confirmed via `model.primitiveInfo` rather than
  assumed) textured with `image.fillCheckerboard`, not 64 separate tile
  objects -- far fewer RPC calls, and a texture-based board was judged
  good enough visually (real per-tile 3D geometry remains a documented
  future option in `image_procgen.h`'s own `FillCheckerboard` comment
  for anyone who wants crisper tile edges later).
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (including a new seam-continuity check and checkerboard-tiling check
  in `mep-image-procgen-test`), all non-GUI tests pass. Live: built the
  full 32-piece set end to end, rendered at 1600x1150, and did 3 close-
  up camera renders (`model.cameraSet` + `model.renderToImage`) to judge
  the pawn head and knight silhouette specifically, iterating the
  profiles/knight design based on what those closeups actually showed
  rather than guessing from the wide shot alone. Saved the final model
  as `test/chess_set_full.gltf` (47 nodes: 41 real mesh objects + 6
  organizational group nodes from the 4 knights + 2 kings, all
  materials/textures verified present) and the render as
  `test/chess_set_full.png`.

### Non-goals, reaffirmed and extended

- Sculptural knight/horse-head modeling is still out of scope -- the
  tilted-cone abstraction is a deliberate stylized substitute, not a
  placeholder for "real" knight geometry that's still coming.
- Per-tile 3D board geometry (64 objects instead of 1 textured plane) --
  documented as a viable future option, not attempted.
- Full Cook-Torrance/GGX PBR, shadows, environment reflections: still
  out of scope, unchanged from Phase 1's original Non-goals.

## Phase 8: a real knight, and the backend capability it needed -- DONE

Requested after Phase 7: "the knight doesn't look right... I seem to
remember there being some new features needed to make that one look
good." Phase 7's Non-goals had flagged sculptural knight modeling as
out of scope; asked to revisit, so this phase builds the actual missing
capability rather than another primitive-combining trick.

- [x] **Researched what mep can already do** before writing anything,
  rather than guessing: confirmed (1) no file-import path adds a mesh
  into an *already-open* scene alongside existing objects -- every
  import (`OpenModel3DInPlace`/`Model3DFinishOpen`) replaces a whole
  buffer's scene; (2) `model.addVertex`/`model.makeFace` require an
  existing non-empty host object and would need 40-60+ blind RPC round-
  trips per knight with zero visual feedback -- technically possible,
  not practical; (3) no "submit a whole vertex/triangle list in one
  call" method existed at all. This confirmed a new capability was the
  right move, not a workaround.
- [x] **`AddCustomMeshToScene`** (`model3d_doc.h`/`.cpp`), mirroring
  `AddLatheToScene`'s exact shape: takes a flat vertex list and a flat
  triangle-index list (3 per triangle, `MeshData::indices`'s own
  convention) and adds it as a new object in an existing scene.
  Normals are computed automatically via `MeshData::RecalculateNormals()`
  -- callers never supply their own. Texcoords are left empty
  (deliberately -- see its own doc comment: no UV story for an
  arbitrary mesh without also demanding the caller hand-author UVs, so
  it renders with a solid color/roughness/metallic instead of a wrapped
  texture, same as any object with `texture_index` left at -1).
  Rejects empty vertex/triangle lists, an index count not a multiple of
  3, and any out-of-range index -- covered by a new dedicated test
  block in `mep-model3d-doc-test` (a hand-built unit tetrahedron,
  checked for correct vertex/triangle counts and unit-length computed
  normals, plus all four invalid-input cases). Exposed as
  `model.addCustomMesh` (RPC) / `mep_model_add_custom_mesh` (MCP) --
  the general-purpose escape hatch this benchmark needed, not a
  knight-specific method.
- [x] **A real horse-head knight**: hand-authored a 16-point 2D
  silhouette (ears, forehead, nose bridge, muzzle, jaw -- traced by eye
  to read as a horse head in profile), extruded flat with two Z-layers,
  triangulated with a from-scratch ear-clipping algorithm (the
  silhouette has real concave notches -- the ear valley, the jaw --
  so a naive fan-from-one-vertex triangulation would produce wrong/
  self-intersecting triangles; ear-clipping handles any simple polygon
  correctly). Sits on the same `KNIGHT_BASE` lathe stem the earlier
  cone/wedge attempts used, but with the neck's *base* widened to
  ~0.40 span (matching/exceeding the stem's 0.24 radius) after the
  first attempt's narrow base read as a thin blade stuck on a dowel,
  not a solid neck -- caught by comparing renders, not assumed.
  `rotation.y` (0 white, 180 black) turns it to face the opposing side,
  same convention every other asymmetric piece in this set uses.
  Iterated with dedicated close-up/profile renders (`model.cameraSet`
  with a level, narrow-FOV shot straight down the extrusion's normal
  axis) *before* rebuilding the full 32-piece set each time, so
  mistakes (initial too-narrow base; initially guessing the wrong yaw
  for the profile-facing axis) were caught and fixed cheaply rather
  than discovered only in a full 130-RPC-call rebuild.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (including the new `AddCustomMeshToScene` unit-test block), all non-
  GUI tests pass. Live: standalone `model.addCustomMesh` render (front,
  back, and 3/4 views -- confirmed no inverted-normal dark patches, no
  holes, both caps and all side walls shade correctly), combined-with-
  base render (confirmed a solid, gap-free connection to the lathe
  stem), then the full 32-piece set rebuilt end to end with all 4
  knights using the new mesh, saved to `test/chess_set_full.gltf`
  (verified: still 47 nodes/41 meshes, 4 `KnightHead` nodes present)
  and rendered to `test/chess_set_full.png`.

### Non-goals, reaffirmed

- Still not attempting a fully sculpted, anatomically detailed horse
  head -- 16 boundary points and a flat extrusion is a deliberate low-
  poly stylization matching the rest of the set's own faceted-but-
  smooth-shaded look, not a placeholder for something more detailed.
- `model.addCustomMesh`'s original ship had no UV/texture story by
  design -- Phase 9 below added optional UVs once a real need (the
  knight looking out of place, flat-colored, next to textured
  neighbors) showed up; this bullet is superseded, kept for history.

## Phase 9: knight facing/detail fixes, rook crenellations, custom-mesh UVs -- DONE

Requested after seeing Phase 8's knight on the actual board: "the knight
isn't facing the right way (face pointing forwards), and there isn't
enough detail. Also, I'd like the square posts on top of the rook,
which aren't there." Explicit invitation to make any other adjustments
that would help, too.

- [x] **Knight facing, determined from the actual rotation matrix, not
  guessed**: the knight's snout pointed along local +X at rotation.y=0;
  the earlier convention (0 white / 180 black) pointed it *sideways*
  along the board's file direction instead of *forward* toward the
  opponent. Rather than trial-and-error with renders (which are easy to
  misread at oblique angles -- confirmed by first getting a misleading
  read from one), read `gfx::MatrixRotateXYZ`'s actual matrix
  (`vecmath.h`) and derived that a pure Y-rotation by angle *a* maps
  local +X to world `(cos a, 0, sin a)` -- so `+90` points the snout to
  world +Z, `-90` to world -Z. White's back rank sits at z=-3.5, black's
  at z=+3.5, so "forward" (toward the opponent) is +Z for white, -Z for
  black: `face_angle` changed from `(0, 180)` to `(+90, -90)`. Verified
  empirically afterward (axis-marker cubes at world +X/+Z, rendered
  alongside the mesh) to confirm the derivation matched reality before
  trusting it in the full rebuild.
- [x] **Knight volume**: the flat 2-layer (front/back) extrusion read
  fine face-on but looked like a thin blade from any oblique angle --
  exactly what "not enough detail" was pointing at once seen on the
  actual board (the usual render camera is oblique, not face-on). Added
  a third, wider "equator" layer (`KNIGHT_HEAD_BULGE_SCALE = 1.35`,
  scaled outward from the silhouette's own centroid) between the front
  and back layers, turning the flat card into a rounded "loaf" cross-
  section -- real volume, reads reasonably from any angle. Front/back
  caps still use the original (unbulged) silhouette; only the new
  interior ring bulges.
- [x] **Rook crenellations**: a lathe can only produce rotationally
  symmetric shapes, so the classic castle-tower battlements have to be
  discrete objects -- 8 small `cube` primitives evenly spaced around the
  rook's own top rim radius/height (kept in sync by hand with the ROOK
  profile's last point; no RPC query for "this lathe's own top ring
  radius" exists), each rotated to face outward, textured/materialed to
  match the body, and grouped onto it.
- [x] **`model.addCustomMesh` gained optional UVs** (`AddCustomMeshToScene`
  in `model3d_doc.h`/`.cpp`, `Editor::Model3DAddCustomMesh`, the RPC
  handler, and the MCP tool schema, all extended together) -- a real
  backend addition, not a workaround: the knight head was still flat-
  colored (custom meshes had no UV story at all when first shipped in
  Phase 8) and visibly stood out next to every other heavily wood-
  grained piece once placed on the actual board, which is as much a
  "not enough detail" issue as the flat geometry was. `texcoords` is a
  new optional flat parameter (2 floats/vertex, `MeshData::texcoords`'s
  own convention, mirroring how `indices` already works) defaulting to
  empty for full backward compatibility -- every existing call site
  (including the model3d-doc-test's own tetrahedron test) keeps working
  unchanged. Covered by new test cases (a UV-carrying mesh's texcoords
  land correctly; a mesh built with no UVs still leaves them empty;
  mismatched-length UVs rejected with -1, same as every other malformed-
  input case). The knight head mesh now carries real UVs (u wraps once
  around its own closed perimeter -- seamless, matching how
  `FillWoodTurned` is already designed to wrap; v marks which of the 3
  extrusion layers a vertex belongs to) and gets `model.setTexture`'d
  with the same wood texture as the rest of its side, instead of a flat
  averaged color.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`
  (including the new UV test cases), all non-GUI tests pass. Live:
  rebuilt the full 32-piece set end to end with all fixes, confirmed via
  close-up renders that the rook shows clear castle crenellations, the
  knight shows real wood grain (no longer flat-colored) with visible
  volume from the bulge, and the whole set still assembles/saves/
  renders correctly (`test/chess_set_full.gltf`: 83 nodes now -- 73 real
  mesh objects, up from 41, from the 4x8 new rook merlons, plus 10
  organizational group nodes from 4 knights + 2 kings + 4 rooks now
  grouped; `images` count now equals `meshes` count exactly, confirming
  every object -- including the knight heads for the first time --
  actually has a texture assigned). Saved to `test/chess_set_full.gltf`
  and `test/chess_set_full.png`.

### Non-goals, reaffirmed

- Knight orientation is now geometrically correct (faces the opponent)
  but the mesh itself is still the same Phase 8 stylization -- not
  attempting a more anatomically detailed horse head this round either.
- Rook crenellations are simple axis-aligned cubes, not carved/chamfered
  merlons -- a further detail pass, not attempted.

## Phase 10: base width and a real knight-facing correction -- DONE

Requested after seeing Phase 9's set on the actual board: "the pieces
have too wide of a base and the knight is facing the opposite direction
(so needs to be rotated around 180 degrees)."

- [x] **Base width**: all six piece profiles (PAWN, ROOK, BISHOP,
  KNIGHT_BASE, QUEEN, KING) had their radius values scaled by a new
  `BASE_SCALE = 0.7` constant (heights untouched -- only the lathe
  profiles' radius component). The rook's crenellation ring, which
  derives its own placement from `ROOK[-1]` rather than a separate
  hardcoded number (a Phase-9 fix), stayed in sync automatically; the
  merlon cubes' own `scale`/`y`-offset were additionally multiplied by
  `BASE_SCALE` so the battlements shrink proportionally with the now-
  thinner rook body instead of looking oversized against it.
- [x] **Knight facing, actually wrong despite Phase 9's matrix
  derivation**: Phase 9 derived `face_angle = (+90, -90)` (white/black)
  from `gfx::MatrixRotateXYZ`'s matrix and confirmed it with axis-marker
  cubes, but the user reported the resulting knights faced *away* from
  the opponent -- the derivation (or the silhouette's own assumed
  "local +X = snout" direction) had a sign error somewhere despite
  looking rigorous. Rather than re-derive from scratch and risk the
  same mistake again, applied exactly the correction the user asked
  for: swapped to `face_angle = (-90, +90)`, a straight 180-degree flip.
  Re-verified empirically this time with a real calibration render
  rather than trusting the matrix a second time: placed a bright marker
  cube offset from each knight toward its actual opponent (+Z for
  white, -Z for black), rendered a clear side-profile shot showing the
  horse head's recognizable ears/muzzle silhouette, and confirmed the
  muzzle curves toward the marker (i.e. toward the opponent) for both
  colors. This is the methodology worth keeping for any future
  orientation question on this asset: an anatomically-recognizable
  profile render plus a ground-truth marker beats re-deriving rotation
  matrices by hand, which has now produced a wrong answer once already.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (only the pre-existing, unrelated
  `mep-collab-session-test` failure, which needs a websocket URL
  argument it isn't given). Live: rebuilt the full 32-piece set,
  confirmed via a wide board render that piece bases now have visible
  clearance from their own board squares and neighboring pieces (no
  longer edge-to-edge), and confirmed via calibrated marker-cube renders
  that both knights now face their actual opponents. Saved to
  `test/chess_set_full.gltf` (83 nodes, 73 meshes -- unchanged counts
  from Phase 9, as expected: this round only changed transforms/scale
  values, not topology) and `test/chess_set_full.png`.

### Non-goals, reaffirmed

- The knight head's own fixed dimensions (silhouette scale, bulge) were
  not rescaled by `BASE_SCALE` -- only the lathed body profiles were.
  Checked visually after the rebuild; the head still reads fine against
  the thinner stem, so no further change made.
- The king's cross-bar toppers are likewise fixed absolute scale,
  unrelated to `BASE_SCALE` -- checked visually, still proportionate
  against the thinner body, no change made.
