#pragma once

// Extracts renderable geometry from a .blend file (see blend_file.h for
// the container/SDNA layer): every mesh object, with its world transform
// baked into the vertices, split into one part per material slot, plus a
// simple material (base colour, roughness, metallic, base-colour image).
//
// Output conventions match what Blender's own glTF exporter produced for
// the old `blender --background` import path: Y-up (Blender's Z-up is
// converted as (x, y, z) -> (x, z, -y)), triangles only, V flipped
// (v' = 1 - v), smooth or flat normals per face as Blender shades them.
//
// Mesh formats understood:
//   * Blender 5.x: Mesh.attribute_storage (AttributeStorage) attributes
//     "position", ".corner_vert", float2 corner UVs, "material_index",
//     "sharp_face", plus Mesh.poly_offset_indices (face offsets).
//   * Blender 3.x/4.x: the same attributes as CustomData layers
//     (vdata/pdata/ldata), and the transitional MVert/MPoly/MLoop/MLoopUV
//     layers of 2.63-3.4.
//   * Blender 2.4x-2.62: tessellated MFace/MTFace.
//
// Not evaluated: modifiers (subdivision, mirror, array, ...), shape
// keys, constraints, drivers and animation, armature deformation,
// collection instances, custom split normals, and node trees beyond
// reading the Principled/Diffuse BSDF input defaults and a directly linked
// Image Texture node.

#include <string>
#include <vector>

namespace blend {

struct SceneMaterial {
    std::string name;
    float color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    // Base-colour image, if one is linked: an on-disk path ("//" relative
    // paths already resolved against the .blend's directory) and/or the
    // packed file bytes when the image is packed into the .blend.
    std::string image_name;
    std::string image_path;
    std::vector<unsigned char> image_packed;
};

struct ScenePart {
    std::string name;  // object name, plus ".<material>" when the object has several
    std::vector<float> positions;       // 3 per vertex, world space, Y-up
    std::vector<float> normals;         // 3 per vertex
    std::vector<float> texcoords;       // 2 per vertex, or empty
    std::vector<unsigned int> indices;  // 3 per triangle
    int material = -1;                  // index into Scene::materials
};

struct Scene {
    int version = 0;            // e.g. 279, 405, 500
    std::string compression;    // "", "gzip" or "zstd"
    std::vector<ScenePart> parts;
    std::vector<SceneMaterial> materials;
    std::vector<std::string> warnings;  // things present but not converted (modifiers, ...)
};

// Reads `path`. Returns false and sets *error if the file cannot be
// parsed or contains no mesh geometry at all.
bool ReadScene(const std::string &path, Scene *out, std::string *error);

// Same, for file bytes already in memory; `base_dir` resolves "//"
// relative image paths (may be empty).
bool ReadSceneFromBytes(std::string bytes, const std::string &base_dir, Scene *out, std::string *error);

}  // namespace blend
