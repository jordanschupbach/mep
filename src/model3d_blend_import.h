#ifndef MEP_MODEL3D_BLEND_IMPORT_H
#define MEP_MODEL3D_BLEND_IMPORT_H

#include <string>
#include <vector>

struct Scene;

// Case-insensitive check for the one extension this module handles --
// Blender's own project format (contrast model3d_doc.h's IsModel3DPath,
// the formats the glTF/OBJ/... loaders take).
bool IsBlendPath(const std::string &path);

// How .blend files are imported (mep.opt.blend_importer):
//   Builtin -- mep's own .blend reader (blend_scene.h), no external
//              program. The default.
//   Auto    -- the builtin reader, falling back to the Blender CLI when
//              it fails and `blender` is on PATH.
//   Blender -- always the Blender CLI (`blender --background` exporting a
//              .glb), e.g. when modifiers must be applied.
enum class BlendImporter { Builtin, Auto, Blender };
void SetBlendImporter(BlendImporter mode);
BlendImporter GetBlendImporter();
// "builtin" / "auto" / "blender" <-> enum; unknown names return false.
bool ParseBlendImporter(const std::string &name, BlendImporter *out);
const char *BlendImporterName(BlendImporter mode);

// Imports `blend_path` with the in-house reader straight into `out`: one
// Object3D per (mesh object, material slot), world transforms baked into
// the vertices (Y-up, like the glTF path), base colour / roughness /
// metallic from each material, and its base-colour image when one is
// linked (from disk, or the packed copy inside the .blend). Synchronous
// and fast (no subprocess). `warnings` (optional) receives what the
// reader saw but could not convert -- modifiers that were not applied,
// bone parenting, linked libraries. Returns false and sets *error when
// the file cannot be read or holds no mesh geometry.
bool LoadBlendFile(const std::string &blend_path, Scene *out, std::string *error,
                   std::vector<std::string> *warnings = nullptr);

// ---- Blender CLI path (opt-in, see BlendImporter) ----------------------

// True if a `blender` executable is found on PATH.
bool BlenderAvailable();

// Converts `blend_path` to a temporary glTF binary (.glb) by shelling out to
// `blender --background --python <bundled export script>`. Blocks the
// calling thread until Blender exits (native-only; POSIX fork/exec, no
// shell involved so `blend_path` needs no escaping). On success,
// *out_glb_path names a fresh temp file the caller should remove after
// importing it via LoadModel3DFile. Returns false and sets *error (Blender
// missing, or its own stderr tail on a conversion failure) otherwise.
// Kept only for callers that genuinely want to block (currently none in
// the editor itself -- see PrepareBlendConversionJob below for the async
// path Editor::OpenModel3DInPlace uses when the CLI importer is selected).
bool ConvertBlendToGltf(const std::string &blend_path, std::string *out_glb_path, std::string *error);

// Async counterpart, split around a caller-owned process spawn (JobManager
// in Editor's case) instead of this module's own blocking fork+waitpid --
// so the UI thread never stalls for the several seconds a real Blender
// invocation takes.
//
// PrepareBlendConversionJob does everything before the process exists:
// checks BlenderAvailable(), writes the bundled export script to a temp
// file, and picks argv/an output path -- cheap, synchronous, safe to call
// from the UI thread. On success, *out_argv is ready to hand to a job
// runner (argv[0] == "blender"), *out_script_path/*out_glb_path name the
// temp files FinishBlendConversionJob below needs afterward. Returns false
// (Blender missing, or a temp-file failure) without starting anything.
bool PrepareBlendConversionJob(const std::string &blend_path, std::vector<std::string> *out_argv,
                                std::string *out_script_path, std::string *out_glb_path, std::string *error);

// Call once the process started from PrepareBlendConversionJob's argv has
// exited. Removes the temp script file unconditionally, then validates the
// exit code and that a non-empty .glb actually landed at `glb_path` --
// exactly ConvertBlendToGltf's own post-wait validation, factored out so
// both the sync and async paths share one implementation. `child_output` is
// the process's collected stdout+stderr (order/interleaving doesn't matter,
// only used for the failure tail). On failure, removes `glb_path` too (if
// anything partial got written) and sets *error the same way
// ConvertBlendToGltf does.
bool FinishBlendConversionJob(const std::string &script_path, const std::string &glb_path, int exit_code,
                               const std::string &child_output, std::string *error);

#endif
