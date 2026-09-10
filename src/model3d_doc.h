#ifndef MEP_MODEL3D_DOC_H
#define MEP_MODEL3D_DOC_H

#include <string>
#include <vector>

// Deliberately kept free of raylib *types* in this public interface, same
// reasoning as ImageDoc/HtmlDoc (see their own header comments): a Scene is
// plain CPU-side data, usable and testable without a GL context. Unlike
// ImageDoc, LoadModel3DFile/AddPrimitiveToScene (model3d_doc.cpp) reach into
// raylib *internally* as a one-shot import/generation utility -- raylib is
// the only parser available in this codebase for OBJ/glTF/IQM/VOX/M3D and
// the only source of tested procedural mesh generation (par_shapes-backed
// GenMesh*), so re-deriving either from scratch has no payoff. Both
// functions load via raylib's own Model/Mesh types, copy the vertex arrays
// out into the plain structs below, and unload the raylib Model immediately
// -- nothing here ever stores a raylib handle. main.cpp is the only place a
// Scene's MeshData turns into a GPU-resident raylib Model for drawing
// (mirrors ImageDoc -> Texture2D, one level up in MODEL3D.md's Model3D-
// Session's own gpu cache).

// One mesh's CPU-side geometry: parallel arrays indexed by vertex, plus a
// flat triangle index list. `normals`/`texcoords` may be empty (some OBJ
// files, and every procedurally generated primitive with no need for UVs)
// -- main.cpp's GPU upload generates flat per-triangle normals on the fly
// if `normals` is empty, and treats a missing `texcoords` as all-zero.
struct MeshData {
    std::string name;
    std::vector<float> positions;       // 3 floats/vertex
    std::vector<float> normals;         // 3 floats/vertex, or empty
    std::vector<float> texcoords;       // 2 floats/vertex, or empty
    std::vector<unsigned int> indices;  // 3 indices per triangle

    int VertexCount() const { return static_cast<int>(positions.size() / 3); }
    int TriangleCount() const { return static_cast<int>(indices.size() / 3); }

    // Removes the given vertices (by index, duplicates/out-of-range values
    // ignored) along with every triangle that referenced any of them --
    // deliberately just leaves a hole rather than attempting to
    // retriangulate/fill it (a "dissolve vertex" that patches the
    // surrounding faces back together is a real, harder follow-up, not
    // this pass's scope). Remaining vertices and triangles are compacted
    // and re-indexed in their original relative order, so there are no
    // gaps in the resulting arrays.
    void RemoveVertices(const std::vector<int> &vertex_indices);

    // Welds the given vertices (by index, duplicates/out-of-range values
    // ignored; a no-op if fewer than 2 distinct valid indices are given)
    // into a single vertex at their averaged position/normal/texcoord,
    // kept at the lowest of the given indices. Every triangle referencing
    // any of the merged-away vertices is remapped to point at the kept
    // vertex instead; any triangle that becomes degenerate as a result
    // (two or more of its three corners now the same vertex) is dropped
    // rather than kept as a zero-area triangle. Remaining vertices and
    // triangles are compacted/re-indexed same as RemoveVertices, so there
    // are no gaps afterward. This is a "weld", not real edge-collapse
    // topology -- it doesn't attempt to detect or avoid creating
    // duplicate/overlapping triangles beyond dropping exact degenerates.
    void MergeVertices(const std::vector<int> &vertex_indices);

    // Recomputes every vertex's normal as the normalized sum of the
    // (unnormalized, so implicitly area-weighted) face normals of every
    // triangle that references it -- the standard "smooth" recalculation.
    // There's no separate flat/per-face mode: that would need splitting
    // each shared vertex into one copy per adjacent face, which is out of
    // scope here. A vertex referenced by zero triangles (degenerate/
    // orphaned data) gets a {0,0,1} fallback rather than being left
    // whatever it was. No-op if the mesh has no triangles.
    //
    // This exists purely to fix up normals gone stale after vertex
    // edits (move/delete/merge, see their own comments) for tools that
    // actually read them on export -- Blender, glTF viewers with real
    // lighting -- since this app's own renderer never references vertex
    // normals at all (raylib's default shaders don't), so it has zero
    // visible effect in-app.
    void RecalculateNormals();

    // "Face" here means every triangle whose all 3 corners are in
    // `vertex_indices` -- this app's established stand-in for real
    // face-selection tooling (see MergeVertices' own box-select-driven
    // workflow note in MODEL3D.md). Centroid-subdivides each such
    // triangle: adds one new vertex at the triangle's centroid (position/
    // normal/texcoord all averaged from its 3 corners, same "average now,
    // RecalculateNormals fixes it up properly later" convention as
    // MergeVertices) and replaces the triangle with 3 new ones fanning
    // out to it. A triangle with fewer than all 3 corners selected is
    // left untouched. Returns the newly-created centroid vertex indices,
    // one per subdivided triangle, in the order their triangles were
    // encountered (empty if no triangle was fully selected).
    std::vector<int> SubdivideFaces(const std::vector<int> &vertex_indices);

    // Extrudes the selected face (see SubdivideFaces' own note on what
    // "face" means here) by `distance` along its own geometrically-
    // derived normal (the area-weighted sum of every selected triangle's
    // own cross-product normal -- NOT read from `normals`, which may be
    // stale or absent). Every vertex used by a selected triangle is
    // duplicated and offset along that direction; the selected triangles
    // are re-pointed at the duplicates (lifting the "cap" into place)
    // while the original vertices stay put and get connected to the new
    // ring by a wall quad (2 triangles) per *boundary* edge of the
    // selected group -- an edge shared between two selected triangles
    // (the group's own internal edges, e.g. a face's diagonal) is left
    // alone, not walled. A vertex that's also used by a triangle outside
    // the selection (e.g. a cube's adjacent side face sharing a top
    // corner) naturally stays attached there too, since only the
    // duplicate moves -- this is what makes "extrude one face of a cube"
    // work without any special-casing. Extrusion respects actual mesh
    // *connectivity*, not spatial adjacency: raylib's own generated
    // primitives have unwelded per-face vertices (MergeVertices' own
    // note), so selecting an entire unwelded cube's vertices extrudes
    // each of its 6 disconnected faces independently -- weld first (see
    // MergeVertices) if that's not what's wanted. No-op (returns an empty
    // vector, touches nothing) if no triangle is fully selected, or if
    // the selected group is degenerate (zero-area, no sane normal to
    // extrude along). Returns the new "cap" vertex indices on success (in
    // ascending original-vertex order), so a caller can immediately
    // select/move the just-extruded face.
    std::vector<int> ExtrudeFaces(const std::vector<int> &vertex_indices, float distance);

    // "Dissolve" one vertex: removes it *and* patches the surrounding
    // faces back together, unlike RemoveVertices' own deliberately-left
    // hole. Only works cleanly on a proper interior/manifold vertex --
    // one whose incident triangles form a single closed fan around it,
    // i.e. walking each triangle's "opposite edge" (the two corners other
    // than this vertex, in the same rotational order the triangle already
    // had, so winding is preserved) traces exactly one cycle through
    // every incident triangle. When that holds, the incident triangles
    // are replaced by a simple fan triangulation of that ring (pivoting
    // on the ring's first vertex) -- not a "nicest possible" retriangulation,
    // just a correct, hole-free one; a very non-convex ring can produce a
    // visibly thin sliver triangle or two. When it doesn't hold (a mesh-
    // boundary vertex, a non-manifold fan, or a vertex with no incident
    // triangles at all) this silently falls back to a plain RemoveVertices
    // -- same "leaves a hole" behavior as that function -- rather than
    // risk a malformed retriangulation. Returns false only if
    // vertex_index is out of range; true otherwise, whichever path was
    // taken.
    bool DissolveVertex(int vertex_index);

    // Insets the selected face (see SubdivideFaces' own note on what
    // "face" means here): every vertex used by a fully-selected triangle
    // is duplicated and moved toward the face group's own centroid (the
    // average position of every vertex the group uses) by `amount`, a
    // fraction clamped to [0,1] -- 0 leaves the duplicate exactly where
    // the original was (a degenerate, zero-width inset), 1 collapses it
    // onto the centroid. The selected triangles are re-pointed at the
    // duplicates (shrinking the "cap" in place, with no lift along any
    // normal -- unlike ExtrudeFaces, which moves along one), and a wall
    // quad connects the untouched original ring to the new shrunk one
    // along each boundary edge of the group, same convention as
    // ExtrudeFaces. Returns the new cap vertex indices (empty if no
    // triangle was fully selected), so the result can be chained straight
    // into ExtrudeFaces for a raised-platform-with-border look (inset,
    // then extrude the returned cap).
    std::vector<int> InsetFaces(const std::vector<int> &vertex_indices, float amount);

    // Appends one new, isolated vertex at (x,y,z) -- no triangle
    // references it, so it won't render until connected via MakeFace or
    // similar (this is the deliberate counterpart to that: building
    // genuinely new geometry from scratch, rather than subdividing/
    // extruding/insetting existing triangles). If the mesh already has
    // normals/texcoords, the new vertex gets a default {0,0,1} normal /
    // {0,0} texcoord so the parallel arrays stay the same length as
    // positions -- RecalculateNormals can fix the normal up properly once
    // the vertex is actually part of a face. Takes raw floats rather than
    // Vec3f since that struct isn't declared until after MeshData in this
    // header. Returns the new vertex's index.
    int AddVertex(float x, float y, float z);

    // Creates new triangle(s) connecting existing vertices -- Blender's
    // own "Make Edge/Face" (F key) in spirit. Filters `vertex_indices` to
    // distinct, valid ones first (order preserved), then fan-triangulates
    // from the first one: 3 vertices become 1 new triangle, 4 become 2, a
    // pentagon 3, exactly the same fan shape SubdivideFaces' own centroid
    // uses, just connecting *existing* vertices directly with no new one
    // added. No new vertices, and no check for whether the resulting
    // triangle(s) duplicate or overlap ones that already exist -- winding
    // (and so which side ends up "front") follows the given vertex order.
    // Returns false (a no-op) if fewer than 3 distinct valid vertices
    // remain after filtering.
    bool MakeFace(const std::vector<int> &vertex_indices);

    // Automatically welds every group of vertices whose positions are all
    // mutually within `threshold` (clamped to >= 0) of each other --
    // Blender's own "Merge by Distance"/"Remove Doubles" -- rather than
    // requiring the caller to already know which vertex indices are
    // duplicates (see MergeVertices' own note on raylib's generated
    // primitives always shipping unwelded per-face duplicates at every
    // shared corner: this is the "just fix all of them" version of that).
    // Grouping is transitive through a chain of close-enough pairs (so 3
    // nearly-collinear points can end up one group even if the two
    // endpoints alone exceed threshold), not a single global any-to-any
    // cluster. Each group is welded to its averaged position/normal/
    // texcoord, kept at its lowest vertex index, same convention as
    // MergeVertices; a triangle that becomes degenerate as a result is
    // dropped, same as MergeVertices. threshold=0 welds only exact
    // (bit-identical) position duplicates. Vertices with no near neighbor
    // are left completely alone. Returns how many vertices were removed
    // (0 if nothing was within threshold of anything else).
    int MergeByDistance(float threshold);

    // Reverses every triangle's winding (swaps its 2nd and 3rd corners)
    // and negates every vertex normal -- flips which side of the mesh
    // renders as "front". The fix for geometry that came out inside-out,
    // e.g. a MakeFace call given vertices in the wrong order, or some
    // imported files. Operates on the whole mesh, not a selection --
    // there's no per-triangle "flip just this face" in this pass (that
    // would need real face-selection tooling, this app's own recurring
    // gap). No-op if the mesh has no triangles.
    void FlipNormals();
};

struct Vec3f {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A lathe profile point (CHESS_SET_BENCHMARK_PLAN.md Phase 2) -- radius
// from the Y axis and height along it, NOT a generic 2D point (there's no
// other 2D-point use in this file to share the name/shape with).
struct Vec2f {
    float radius = 0.0f, height = 0.0f;
};

// One texture's CPU-side pixel data, always normalized to RGBA8 (4
// bytes/pixel) regardless of the source file's format -- mirrors
// MeshData's own "raylib decodes it, we copy the raw bytes out and own
// them from then on" convention, so main.cpp's GPU upload path never has
// to branch on source format.
struct TextureData {
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;  // RGBA8, width*height*4 bytes
};

// 0..1 float color (glTF's native range) -- distinct from editor.h's
// RgbaColor (0..255 bytes, used by the raster image editor) since this
// module's only consumer of color is the glTF exporter, which wants floats
// either way; converted to/from 0..255 only at the Lua/RPC boundary.
struct RgbaColorF {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

// What generated `mesh_index` for an object -- lets the Inspector label an
// object usefully ("Cube", "Imported: chair.obj") without re-deriving it
// from the mesh name, and lets a future "regenerate at a new size" feature
// (not in this pass) know which primitives are safe to regenerate.
enum class PrimitiveKind { None, Cube, Sphere, Cylinder, Cone, Plane, Torus, Wedge, Imported };

// Which of Object3D's texture slots Editor::Model3DSetTexture targets
// (CHESS_SET_BENCHMARK_PLAN.md Phase 1) -- mirrors PrimitiveKind's own
// string-mapped-by-agent_rpc.cpp convention (RequireTextureMapKind).
enum class TextureMapKind { Albedo, Normal, Roughness, Metallic };

// One node in the scene. Deliberately flat, not a real parent/child scene
// graph: `parent` is an optional object id used only for the Outliner's
// nesting display and group-move semantics (mep_model_set_transform never
// walks it) -- an imported glTF file's own node hierarchy is flattened into
// this list at import time (transforms baked, one Object3D per mesh
// primitive), the same tradeoff raylib's own LoadModel already makes.
struct Object3D {
    int id = 0;
    std::string name;
    int mesh_index = -1;  // index into Scene::meshes, or -1 for an empty group node
    PrimitiveKind kind = PrimitiveKind::None;
    Vec3f position;
    Vec3f rotation_deg;  // Euler XYZ, degrees, applied X then Y then Z
    Vec3f scale{1.0f, 1.0f, 1.0f};
    RgbaColorF color{0.8f, 0.8f, 0.8f, 1.0f};
    // Index into Scene::textures, or -1 for no texture (flat `color` only,
    // the only material option before this field existed). When set, this
    // is glTF's baseColorTexture -- sampled and then tinted by `color`,
    // same as the renderer's default shading does (texelColor * colDiffuse
    // * lighting).
    int texture_index = -1;
    // Basic-PBR material slots (CHESS_SET_BENCHMARK_PLAN.md Phase 1) --
    // each *_map_index is an index into Scene::textures (-1 = none, same
    // convention as texture_index above); `roughness`/`metallic` are the
    // scalar used directly when the matching map is absent, or as a
    // uniform fallback value the shader reads unconditionally (see
    // SetModel3DObjectMaterial, main.cpp). Populated from glTF's own
    // native metallic-roughness material model on import
    // (backend_native_model_gltf.cpp) where present; every other
    // importer/primitive leaves these at their defaults (a plausible
    // semi-matte, non-metal material, not "no data").
    int normal_map_index = -1;
    int roughness_map_index = -1;
    int metallic_map_index = -1;
    float roughness = 0.5f;
    float metallic = 0.0f;
    bool visible = true;
    int parent = -1;
};

// A light in the scene (MULTILIGHT_ANIMATION_PLAN.md Part A) -- real
// scene content, undo-aware via the same whole-Scene-snapshot mechanism
// as Object3D, not a session-only view setting. `direction` matches the
// renderer's own existing `uLightDir` convention exactly: world-space,
// pointing FROM the surface TOWARD the light (not the direction light
// travels). `range` only affects Point lights (simple linear falloff --
// see backend_native_renderer3d.cpp's own comment on why this isn't
// inverse-square/physically-based).
enum class LightType { Directional, Point };
struct Light {
    int id = 0;
    std::string name;
    LightType type = LightType::Directional;
    Vec3f position;                     // Point lights only
    Vec3f direction{0.4f, 0.8f, 0.5f};   // Directional lights only; default matches the renderer's old hardcoded key light
    RgbaColorF color{1.0f, 1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;  // Point lights only
    bool visible = true;
};

// A full scene: the CPU-side, raylib-free model behind editor.h's
// Model3DSession. Copyable and comparison-free by design -- Model3DSession's
// undo/redo stack is a `std::vector<Scene>` of whole-scene snapshots
// (full-copy convention, mirroring OfficeSession/SheetSession's own
// std::vector<std::vector<...>> undo stacks), not a diff/patch structure.
struct Scene {
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    std::vector<Object3D> objects;
    // Lights (MULTILIGHT_ANIMATION_PLAN.md Part A). Empty is a real,
    // common, fully-supported state -- the renderer falls back to its
    // own single hardcoded key light when this is empty, so every scene
    // built before this field existed (including every saved .gltf file)
    // keeps rendering exactly as before.
    std::vector<Light> lights;
    // Shared by AddObject and AddLight -- one monotonic id space for
    // everything selectable/addressable in the scene, so an object and a
    // light can never collide even though they're stored in separate
    // vectors.
    int next_object_id = 1;
    std::string source_path;  // path this was imported/opened from, if any; empty for a new blank scene

    // Appends `obj` (its `id` field is overwritten with a freshly allocated
    // one) and returns that id.
    int AddObject(Object3D obj);
    Object3D *FindObject(int id);
    const Object3D *FindObject(int id) const;
    // Same shape as AddObject/FindObject/RemoveObject above, for lights.
    int AddLight(Light light);
    Light *FindLight(int id);
    const Light *FindLight(int id) const;
    bool RemoveLight(int id);
    // Removes the object with this id. Does NOT remove its mesh from
    // `meshes` (another object may share it) or reparent its children --
    // callers needing cascade-delete of children do that themselves.
    bool RemoveObject(int id);
    int TotalTriangleCount() const;
    // All descendants of `object_id` (children, grandchildren, ...), not
    // including `object_id` itself -- a plain linear-scan walk down
    // `Object3D::parent` links, visited-set guarded so a corrupted/cyclic
    // parent chain can't infinite-loop. Used both for cycle prevention when
    // reparenting (a node can't become its own descendant's child) and for
    // "move the group" semantics (translating a parent also translates
    // everything this returns).
    std::vector<int> Descendants(int object_id) const;
    // Ensures `object_id`'s mesh is exclusively its own, cloning it into a
    // new Scene::meshes slot first if any *other* object currently shares
    // the same mesh_index (radial arrays/mirrored duplicates/multi-object
    // imports commonly do) -- vertex-level edits mutate a MeshData in
    // place, which would otherwise silently deform every object sharing
    // it, not just the one being edited. A no-op (just returns the
    // existing index) when already single-user. Returns -1 if object_id
    // doesn't exist or has no mesh (mesh_index -1, an empty group node).
    int EnsureUniqueMesh(int object_id);
};

// Loads a 3D model file (.obj/.gltf/.glb/.iqm/.vox/.m3d) via raylib's
// LoadModel, extracting every sub-mesh's CPU vertex data into `out->meshes`
// and adding one Object3D per sub-mesh (a multi-mesh OBJ/glTF file becomes
// multiple objects, matching how raylib's own Model already separates
// them -- rmodels.c: "every [glTF] primitive loaded as a separate mesh").
// Native-only (raylib's LoadModel takes a filesystem path, no in-memory
// variant for these formats). Returns false and sets *error on a missing
// file, an unsupported/corrupt format, or a model with zero meshes.
bool LoadModel3DFile(const std::string &path, Scene *out, std::string *error);

// Generates one procedural primitive mesh (raylib's par_shapes-backed
// GenMeshCube/Sphere/Cylinder/Cone/Plane/Torus, extracted to plain arrays
// the same way LoadModel3DFile extracts imported meshes), appends it to
// `scene->meshes`, adds a matching Object3D, and returns the new object's
// id. Returns -1 (no-op) for PrimitiveKind::None or ::Imported.
int AddPrimitiveToScene(Scene *scene, PrimitiveKind kind);

// Revolves `profile` (radius/height pairs, bottom to top, at least 2
// points) around the Y axis into a new welded/indexed mesh + Object3D,
// added to `scene` the same way AddPrimitiveToScene's fixed shapes are
// (CHESS_SET_BENCHMARK_PLAN.md Phase 2 -- the primitive missing for
// chess pieces' turned silhouettes). `segments` is the number of
// angular divisions (e.g. 16-32 for a smooth turned look); `cap_top`/
// `cap_bottom` fan-triangulate the top/bottom ring closed (a no-op where
// that ring's radius is already ~0, i.e. the profile already tapers to a
// point there). Returns the new object's id, or -1 if `profile` has
// fewer than 2 points or `segments` < 3.
int AddLatheToScene(Scene *scene, const std::vector<Vec2f> &profile, int segments, bool cap_top, bool cap_bottom);

// Adds a new object built from an arbitrary hand-authored (or externally
// generated) vertex/triangle list -- the primitive/lathe generators above
// only ever produce a fixed family of shapes; this is the escape hatch
// for anything else (CHESS_SET_BENCHMARK_PLAN.md Phase 8, added when a
// knight's asymmetric horse-head silhouette turned out to need real
// custom geometry no amount of primitive-combining or lathe-revolving
// could produce). `positions` is one entry per vertex; `indices` is a
// flat triangle list, 3 indices per triangle, each an index into
// `positions` -- exactly `MeshData::indices`'s own convention, so a
// caller that already has flat position/index arrays (e.g. decoded
// straight from RPC JSON) doesn't need to repack them into anything
// fancier. Normals are computed automatically via
// `MeshData::RecalculateNormals()` (smooth, area-weighted) -- callers
// don't supply their own. `texcoords` is optional (default empty) --
// flat, 2 floats/vertex (u,v per vertex), exactly `MeshData::texcoords`'s
// own convention, same as `indices` already is for triangles: pass it
// when the mesh should sample a wrapped texture like any other object;
// omit it and the object falls back to a solid color/roughness/metallic
// from its own material, same as any object with texture_index left at
// -1 (Phase 8's own knight head started this way, before UV support
// existed here, and still works either way). Returns the new object's
// id, or -1 if `positions` is empty, `indices` isn't a multiple of 3,
// any index is out of range, or `texcoords` is non-empty but isn't
// exactly 2 floats per vertex.
int AddCustomMeshToScene(Scene *scene, const std::vector<Vec3f> &positions, const std::vector<unsigned int> &indices,
                          const std::string &name, const std::vector<float> &texcoords = {});

// Human-readable pivot/dimensions description for a primitive kind, kept in
// sync with what AddPrimitiveToScene actually generates (single source of
// truth for both the `model.primitiveInfo` RPC method and MEP_AGENT_API.md's
// pivot table -- added after MODEL3D.md Phase 1.5 flagged this info as
// documented but not queryable at runtime). Returns false (leaving the
// outputs untouched) for PrimitiveKind::None/::Imported, which
// AddPrimitiveToScene doesn't generate geometry for.
bool DescribePrimitiveKind(PrimitiveKind kind, std::string *out_pivot, std::string *out_dimensions);

// Loads an image file (PNG/JPG/BMP/GIF -- whatever gfx::LoadImage's
// backend decodes, see STB_IMAGE_REMOVAL_PLAN.md) via gfx::LoadImage,
// normalizes it to RGBA8, copies the pixel bytes out into a new
// TextureData (extraction pattern mirrors ExtractMeshFromRaylib -- the
// backend decodes, we own the raw bytes from then on), appends it to
// `scene->textures`, and returns its index.
// A pure CPU decode -- unlike LoadModel3DFile/AddPrimitiveToScene, this
// needs no GL context, so it's safe to call from a plain RPC/Lua handler
// with no live viewport. Returns -1 and sets *error on a missing file or
// unsupported/corrupt format.
int LoadTextureIntoScene(Scene *scene, const std::string &path, std::string *error);

// Serializes `scene` as glTF 2.0 JSON: one buffer (all mesh vertex/index
// data concatenated, base64-encoded inline as a data URI -- no external
// .bin sidecar file to keep track of), one glTF mesh+material per visible
// Object3D (so each object's own base color survives even when several
// objects share a Scene::meshes entry), one flat node per object. Pure
// text output with no raylib dependency at all, unlike the import side --
// native-only for this pass regardless (see MODEL3D.md), but the format
// choice is what would make a later wasm text-write-bridge save path
// possible. Returns false and sets *error on a write failure.
bool SaveModel3DGltf(const Scene &scene, const std::string &path, std::string *error);

// Case-insensitive extension check for the model formats this module
// imports directly via raylib (obj, gltf, glb, iqm, vox, m3d). Does NOT
// include .blend, which needs the separate Blender-CLI conversion path --
// see model3d_blend_import.h's IsBlendPath.
bool IsModel3DPath(const std::string &path);

#endif
