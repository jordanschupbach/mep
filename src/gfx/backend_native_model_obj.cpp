// Wavefront OBJ importer for Stage B8. Hand-written (no vendored parser
// needed -- OBJ is a simple, well-documented text format): parses v/vt/
// vn/f records, fan-triangulates polygons with more than 3 corners, and
// flattens OBJ's independently-indexed v/vt/vn face corners into one
// flat per-corner vertex array (no deduplication -- simpler and correct;
// a memory/performance optimization for later if large OBJ imports ever
// need it).
//
// Material scope: one material for the whole imported mesh (matching
// this importer's own "single flat mesh, no per-material split" scope --
// a multi-material OBJ would need per-material mesh splitting to look
// right, which is out of scope here same as it always was). Resolves
// mtllib's referenced .mtl file and takes whichever material the first
// `usemtl` line names (or the .mtl's first `newmtl` block if the OBJ
// never names one), reading just Kd (diffuse/base color) and map_Kd
// (diffuse/base color texture) -- matching model3d_doc.h's own
// Object3D::color/texture_index scope (flat tint + one texture, no
// normal/metallic-roughness/emissive maps).

#include "gfx/backend_native_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gfx/renderer2d.h"
#include "gfx/renderer3d.h"
#include "gfx/vecmath.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include "../third_party/stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace gfx {

namespace {
struct V3 {
    float x = 0, y = 0, z = 0;
};
struct V2 {
    float x = 0, y = 0;
};
struct FaceVertex {
    int vi = 0, ti = 0, ni = 0;  // 1-based OBJ indices, 0 = absent
};

// Parses one face-corner token: "v", "v/t", "v//n", or "v/t/n". Negative
// (relative-to-end) indices are resolved by the caller, which knows the
// current v/vt/vn counts.
FaceVertex ParseFaceToken(const std::string &tok) {
    FaceVertex fv;
    size_t p1 = tok.find('/');
    fv.vi = std::atoi(tok.substr(0, p1).c_str());
    if (p1 != std::string::npos) {
        size_t p2 = tok.find('/', p1 + 1);
        std::string ts = p2 != std::string::npos ? tok.substr(p1 + 1, p2 - p1 - 1) : tok.substr(p1 + 1);
        if (!ts.empty()) fv.ti = std::atoi(ts.c_str());
        if (p2 != std::string::npos) {
            std::string ns = tok.substr(p2 + 1);
            if (!ns.empty()) fv.ni = std::atoi(ns.c_str());
        }
    }
    return fv;
}

std::string DirOf(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Decodes an image file via stb_image and uploads it as a GPU texture --
// same "decode then upload, caller reads it back with LoadImageFromTexture
// later" dance model3d_doc.cpp's own texture-import path already does,
// so a material's texture here behaves identically once LoadModel returns.
gfx::Texture2D LoadTextureFromFile(const std::string &path) {
    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        std::fprintf(stderr, "gfx native: OBJ import: couldn't decode texture '%s'\n", path.c_str());
        return gfx::Texture2D{};
    }
    gfx::Image img{pixels, w, h, 1, gfx::kPixelFormatR8G8B8A8};
    gfx::Texture2D tex = gfx::LoadTextureFromImage(img);
    stbi_image_free(pixels);
    return tex;
}

// Reads Kd/map_Kd from the given .mtl file's `target_name` newmtl block
// (or its first block, if target_name is empty -- no usemtl line seen).
// Returns a default-white, textureless material if the file, block, or
// properties aren't found -- same fallback raylib's own OBJ loader uses.
gfx::Material LoadMaterialFromMtl(const std::string &mtl_path, const std::string &target_name) {
    gfx::Material mat{};
    auto *maps = new gfx::MaterialMap[gfx::kMaxMaterialMaps]();
    maps[gfx::kMaterialMapAlbedo].color = gfx::White;
    mat.maps = maps;

    std::ifstream file(mtl_path);
    if (!file.is_open()) return mat;
    std::string mtl_dir = DirOf(mtl_path);

    std::string current_name;
    bool in_target_block = target_name.empty();  // no usemtl -- take the first block
    bool matched_any_block = false;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        if (tag == "newmtl") {
            iss >> current_name;
            in_target_block = target_name.empty() ? !matched_any_block : (current_name == target_name);
            if (in_target_block) matched_any_block = true;
        } else if (tag == "Kd" && in_target_block) {
            float r = 1, g = 1, b = 1;
            iss >> r >> g >> b;
            maps[gfx::kMaterialMapAlbedo].color =
                gfx::Color{static_cast<unsigned char>(std::clamp(r, 0.0f, 1.0f) * 255.0f),
                           static_cast<unsigned char>(std::clamp(g, 0.0f, 1.0f) * 255.0f),
                           static_cast<unsigned char>(std::clamp(b, 0.0f, 1.0f) * 255.0f), 255};
        } else if (tag == "map_Kd" && in_target_block) {
            std::string tex_path;
            iss >> tex_path;
            if (!tex_path.empty()) maps[gfx::kMaterialMapAlbedo].texture = LoadTextureFromFile(mtl_dir + tex_path);
        }
    }
    return mat;
}

}  // namespace

gfx::Model LoadObjModel(const char *path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::fprintf(stderr, "gfx native: OBJ import: couldn't open '%s'\n", path);
        return gfx::Model{};
    }

    std::vector<V3> positions, normals;
    std::vector<V2> texcoords;
    std::vector<FaceVertex> face_verts;  // flattened, already fan-triangulated
    std::string mtllib_name, first_usemtl;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        if (tag == "v") {
            V3 p;
            iss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (tag == "vt") {
            V2 t;
            iss >> t.x >> t.y;
            texcoords.push_back(t);
        } else if (tag == "vn") {
            V3 n;
            iss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        } else if (tag == "f") {
            std::vector<FaceVertex> poly;
            std::string tok;
            while (iss >> tok) {
                FaceVertex fv = ParseFaceToken(tok);
                if (fv.vi < 0) fv.vi = static_cast<int>(positions.size()) + fv.vi + 1;
                if (fv.ti < 0) fv.ti = static_cast<int>(texcoords.size()) + fv.ti + 1;
                if (fv.ni < 0) fv.ni = static_cast<int>(normals.size()) + fv.ni + 1;
                poly.push_back(fv);
            }
            // Fan triangulation: correct for the convex polygons OBJ
            // exporters produce in practice, same assumption raylib's
            // own OBJ loader (via tinyobj_loader) makes.
            for (size_t i = 1; i + 1 < poly.size(); i++) {
                face_verts.push_back(poly[0]);
                face_verts.push_back(poly[i]);
                face_verts.push_back(poly[i + 1]);
            }
        } else if (tag == "mtllib" && mtllib_name.empty()) {
            iss >> mtllib_name;
        } else if (tag == "usemtl" && first_usemtl.empty()) {
            iss >> first_usemtl;
        }
        // o/g/s: skipped, no effect on this importer's single-mesh output.
    }

    if (face_verts.empty()) {
        std::fprintf(stderr, "gfx native: OBJ import: '%s' has no faces\n", path);
        return gfx::Model{};
    }

    auto vcount = static_cast<int>(face_verts.size());
    auto *verts = new float[static_cast<size_t>(vcount) * 3];
    auto *uvs = new float[static_cast<size_t>(vcount) * 2];
    auto *norms = new float[static_cast<size_t>(vcount) * 3];
    bool has_normals = !normals.empty();
    for (int i = 0; i < vcount; i++) {
        const FaceVertex &fv = face_verts[static_cast<size_t>(i)];
        V3 p = (fv.vi >= 1 && fv.vi <= static_cast<int>(positions.size())) ? positions[static_cast<size_t>(fv.vi - 1)]
                                                                            : V3{};
        verts[i * 3 + 0] = p.x;
        verts[i * 3 + 1] = p.y;
        verts[i * 3 + 2] = p.z;
        V2 t = (fv.ti >= 1 && fv.ti <= static_cast<int>(texcoords.size()))
                   ? texcoords[static_cast<size_t>(fv.ti - 1)]
                   : V2{};
        uvs[i * 2 + 0] = t.x;
        uvs[i * 2 + 1] = t.y;
        V3 n = (has_normals && fv.ni >= 1 && fv.ni <= static_cast<int>(normals.size()))
                   ? normals[static_cast<size_t>(fv.ni - 1)]
                   : V3{0, 1, 0};
        norms[i * 3 + 0] = n.x;
        norms[i * 3 + 1] = n.y;
        norms[i * 3 + 2] = n.z;
    }

    auto *meshes = new gfx::Mesh[1];
    meshes[0] = gfx::Mesh{};
    meshes[0].vertexCount = vcount;
    meshes[0].triangleCount = vcount / 3;
    meshes[0].vertices = verts;
    meshes[0].texcoords = uvs;
    meshes[0].normals = norms;
    // No index buffer: face_verts is already flat per-corner data (see
    // this file's own top comment on skipping deduplication), so this
    // mesh draws via glDrawArrays, not glDrawElements -- UploadMesh/
    // DrawMesh already handle a null mesh.indices this way.

    auto *materials = new gfx::Material[1];
    materials[0] = mtllib_name.empty() ? gfx::LoadMaterialDefault()
                                        : LoadMaterialFromMtl(DirOf(path) + mtllib_name, first_usemtl);
    auto *mesh_material = new int[1]{0};

    gfx::Model model{};
    model.transform = gfx::MatrixIdentity();
    model.meshCount = 1;
    model.meshes = meshes;
    model.materialCount = 1;
    model.materials = materials;
    model.meshMaterial = mesh_material;
    return model;
}

}  // namespace gfx
