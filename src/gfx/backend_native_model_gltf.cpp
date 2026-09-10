// glTF 2.0 (.gltf / .glb) importer for Stage B8 -- the last and, per the
// plan, hardest of the five import formats. Hand-written rather than
// vendored (e.g. cgltf.h, which raylib itself uses): the JSON half of
// this problem is already solved in-house by src/json.h (originally built
// for LSP/config, general enough to reuse here), which removes the one
// piece that would otherwise have made vendoring the pragmatic choice.
//
// Reads geometry only, same scope as this file's OBJ/IQM/VOX/M3D
// siblings -- materials, textures, images, animations and skins are all
// read by nothing downstream of LoadModel (see model3d_doc.cpp's own
// "decode, copy geometry out, discard" note), so none of that is parsed
// here. What IS implemented, because skipping it would make many
// ordinary glTF files come out visibly wrong: the scene node hierarchy is
// walked and each node's world transform (TRS or explicit matrix,
// composed through parents) is baked directly into its mesh's vertex
// positions/normals -- unlike this importer's siblings, glTF files
// routinely place their actual mesh data in node-local space and expect
// the scene graph to place it, especially anything exported from
// Blender.
//
// Container support: plain-JSON .gltf (buffers as external files or
// base64 data: URIs) and binary .glb (JSON + BIN chunks in one file),
// auto-detected by magic bytes rather than trusted from the extension.
// Only triangle-list primitives (mode 4, the default) are read, matching
// this importer's siblings' "triangles only" scoping.

#include "gfx/backend_native_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gfx/renderer2d.h"
#include "gfx/vecmath.h"
#include "image_codec.h"
#include "json.h"

import mep.gfx.model_read_util;

namespace gfx {

namespace {

// ---- small self-contained utilities (base64, URI decode, file IO) -----

std::string ReadWholeFile(const std::string &path, bool binary) {
    std::ifstream f(path, binary ? std::ios::binary : std::ios::in);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string UrlDecode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string &s) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        std::memset(table, -1, sizeof(table));
        const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) table[static_cast<uint8_t>(alphabet[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4 + 3);
    int val = 0, bits = -8;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        int8_t d = table[static_cast<uint8_t>(c)];
        if (d < 0) continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Resolves a glTF "uri" (buffer or, unused here, image) to raw bytes:
// either a base64 data: URI, or a path relative to the .gltf file itself.
std::vector<uint8_t> ResolveUri(const std::string &uri, const std::string &base_dir) {
    if (uri.rfind("data:", 0) == 0) {
        size_t comma = uri.find(',');
        if (comma == std::string::npos) return {};
        return Base64Decode(uri.substr(comma + 1));
    }
    std::string path = base_dir + UrlDecode(uri);
    std::string data = ReadWholeFile(path, true);
    return std::vector<uint8_t>(data.begin(), data.end());
}

// ---- glTF accessor decoding ---------------------------------------

int ComponentSize(int component_type) {
    switch (component_type) {
        case 5120: case 5121: return 1;  // BYTE, UNSIGNED_BYTE
        case 5122: case 5123: return 2;  // SHORT, UNSIGNED_SHORT
        case 5125: case 5126: return 4;  // UNSIGNED_INT, FLOAT
        default: return 0;
    }
}
int TypeComponentCount(const std::string &type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

struct GltfDoc {
    Json root;
    std::vector<std::vector<uint8_t>> buffers;  // resolved raw bytes, indexed like root["buffers"]
};

// Locates accessor `index`'s raw element `elem` (0-based) within its
// bufferView/buffer, honoring bufferView.byteStride for interleaved
// attributes. Returns nullptr if anything referenced is out of range
// (malformed file) rather than reading out of bounds.
const uint8_t *AccessorElementPtr(const GltfDoc &doc, const Json &accessor, int elem, int component_size,
                                   int component_count) {
    if (!accessor.contains("bufferView")) return nullptr;  // sparse/zero-filled accessor: unsupported, skip
    int bv_index = accessor.get("bufferView").as_int(-1);
    const Json &buffer_views = doc.root.get("bufferViews");
    if (bv_index < 0 || static_cast<size_t>(bv_index) >= buffer_views.size()) return nullptr;
    const Json &bv = buffer_views.items()[static_cast<size_t>(bv_index)];
    int buf_index = bv.get("buffer").as_int(-1);
    if (buf_index < 0 || static_cast<size_t>(buf_index) >= doc.buffers.size()) return nullptr;
    const std::vector<uint8_t> &buf = doc.buffers[static_cast<size_t>(buf_index)];

    size_t bv_offset = static_cast<size_t>(bv.get("byteOffset").as_int(0));
    size_t acc_offset = static_cast<size_t>(accessor.get("byteOffset").as_int(0));
    size_t element_size = static_cast<size_t>(component_size) * static_cast<size_t>(component_count);
    size_t stride = static_cast<size_t>(bv.get("byteStride").as_int(0));
    if (stride == 0) stride = element_size;

    size_t offset = bv_offset + acc_offset + stride * static_cast<size_t>(elem);
    if (offset + element_size > buf.size()) return nullptr;
    return buf.data() + offset;
}

float ReadComponentNormalized(const uint8_t *p, int component_type, bool normalized) {
    switch (component_type) {
        case 5120: {  // BYTE
            auto v = static_cast<int8_t>(p[0]);
            return normalized ? std::max(static_cast<float>(v) / 127.0f, -1.0f) : static_cast<float>(v);
        }
        case 5121: {  // UNSIGNED_BYTE
            uint8_t v = p[0];
            return normalized ? static_cast<float>(v) / 255.0f : static_cast<float>(v);
        }
        case 5122: {  // SHORT
            int16_t v;
            std::memcpy(&v, p, 2);
            return normalized ? std::max(static_cast<float>(v) / 32767.0f, -1.0f) : static_cast<float>(v);
        }
        case 5123: {  // UNSIGNED_SHORT
            uint16_t v;
            std::memcpy(&v, p, 2);
            return normalized ? static_cast<float>(v) / 65535.0f : static_cast<float>(v);
        }
        case 5125: {  // UNSIGNED_INT
            uint32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<float>(v);
        }
        case 5126: {  // FLOAT
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        default:
            return 0.0f;
    }
}

// Reads a POSITION/NORMAL/TEXCOORD-shaped accessor into a flat float
// array (count * component_count floats). Returns empty on any
// unsupported/malformed accessor -- callers treat that as "attribute
// absent".
std::vector<float> ReadAccessorFloats(const GltfDoc &doc, int accessor_index, int expected_components) {
    const Json &accessors = doc.root.get("accessors");
    if (accessor_index < 0 || static_cast<size_t>(accessor_index) >= accessors.size()) return {};
    const Json &acc = accessors.items()[static_cast<size_t>(accessor_index)];
    int count = acc.get("count").as_int(0);
    int component_type = acc.get("componentType").as_int(0);
    int component_count = TypeComponentCount(acc.get("type").as_string(""));
    int component_size = ComponentSize(component_type);
    if (count <= 0 || component_size == 0 || component_count != expected_components) return {};
    bool normalized = acc.get("normalized").as_bool(false);

    std::vector<float> out;
    out.reserve(static_cast<size_t>(count) * static_cast<size_t>(component_count));
    for (int i = 0; i < count; i++) {
        const uint8_t *p = AccessorElementPtr(doc, acc, i, component_size, component_count);
        if (p == nullptr) return {};
        for (int c = 0; c < component_count; c++) {
            out.push_back(ReadComponentNormalized(p + static_cast<size_t>(c) * static_cast<size_t>(component_size),
                                                    component_type, normalized));
        }
    }
    return out;
}

// Reads an "indices" accessor (always SCALAR, an unsigned integer type,
// never normalized) into a plain uint32 index list.
std::vector<uint32_t> ReadAccessorIndices(const GltfDoc &doc, int accessor_index) {
    const Json &accessors = doc.root.get("accessors");
    if (accessor_index < 0 || static_cast<size_t>(accessor_index) >= accessors.size()) return {};
    const Json &acc = accessors.items()[static_cast<size_t>(accessor_index)];
    int count = acc.get("count").as_int(0);
    int component_type = acc.get("componentType").as_int(0);
    int component_size = ComponentSize(component_type);
    if (count <= 0 || component_size == 0 || acc.get("type").as_string("") != "SCALAR") return {};

    std::vector<uint32_t> out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        const uint8_t *p = AccessorElementPtr(doc, acc, i, component_size, 1);
        if (p == nullptr) return {};
        out.push_back(static_cast<uint32_t>(ReadComponentNormalized(p, component_type, false)));
    }
    return out;
}

// ---- scene-graph transform baking ----------------------------------

Matrix MatrixFromQuaternion(float x, float y, float z, float w) {
    Matrix m = MatrixIdentity();
    m.m0 = 1 - 2 * (y * y + z * z); m.m4 = 2 * (x * y - w * z);     m.m8 = 2 * (x * z + w * y);
    m.m1 = 2 * (x * y + w * z);     m.m5 = 1 - 2 * (x * x + z * z); m.m9 = 2 * (y * z - w * x);
    m.m2 = 2 * (x * z - w * y);     m.m6 = 2 * (y * z + w * x);     m.m10 = 1 - 2 * (x * x + y * y);
    return m;
}

// Direction transform (no translation) -- used for normals. Not an
// inverse-transpose, so non-uniform scale skews normals slightly; the
// renderer has no lighting model to expose that, so this is deliberately
// not worth the extra complexity (matches this codebase's general "no
// lighting model" simplification, see backend_native_renderer3d.cpp).
Vector3 TransformDirection(Vector3 v, Matrix m) {
    return {m.m0 * v.x + m.m4 * v.y + m.m8 * v.z, m.m1 * v.x + m.m5 * v.y + m.m9 * v.z,
            m.m2 * v.x + m.m6 * v.y + m.m10 * v.z};
}

Matrix NodeLocalMatrix(const Json &node) {
    if (node.contains("matrix")) {
        const Json &a = node.get("matrix");
        if (a.is_array() && a.size() == 16) {
            Matrix m{};
            float *f = &m.m0;
            for (int i = 0; i < 16; i++) f[i] = static_cast<float>(a.items()[static_cast<size_t>(i)].as_double(0.0));
            return m;
        }
    }
    Vector3 t{0, 0, 0};
    if (node.contains("translation")) {
        const Json &a = node.get("translation");
        if (a.size() == 3) {
            t = {static_cast<float>(a.items()[0].as_double(0.0)), static_cast<float>(a.items()[1].as_double(0.0)),
                 static_cast<float>(a.items()[2].as_double(0.0))};
        }
    }
    float qx = 0, qy = 0, qz = 0, qw = 1;
    if (node.contains("rotation")) {
        const Json &a = node.get("rotation");
        if (a.size() == 4) {
            qx = static_cast<float>(a.items()[0].as_double(0.0));
            qy = static_cast<float>(a.items()[1].as_double(0.0));
            qz = static_cast<float>(a.items()[2].as_double(0.0));
            qw = static_cast<float>(a.items()[3].as_double(1.0));
        }
    }
    Vector3 s{1, 1, 1};
    if (node.contains("scale")) {
        const Json &a = node.get("scale");
        if (a.size() == 3) {
            s = {static_cast<float>(a.items()[0].as_double(1.0)), static_cast<float>(a.items()[1].as_double(1.0)),
                 static_cast<float>(a.items()[2].as_double(1.0))};
        }
    }
    // glTF: M = T * R * S, i.e. scale first, then rotate, then translate.
    Matrix sm = MatrixScale(s.x, s.y, s.z);
    Matrix rm = MatrixFromQuaternion(qx, qy, qz, qw);
    Matrix tm = MatrixTranslate(t.x, t.y, t.z);
    return MatrixMultiply(MatrixMultiply(sm, rm), tm);
}

// One (node, primitive) instance, geometry already baked into world
// space -- becomes one gfx::Mesh in the output Model.
struct BuiltPrimitive {
    std::vector<float> positions, texcoords, normals;
    int material_index = -1;  // glTF materials[] index, -1 for none
};

// Resolves a glTF image (by images[] index) to raw encoded bytes -- either
// a uri (external file or base64 data: URI, same as buffers) or a GLB-
// embedded bufferView -- then decodes it via image_codec. Empty Texture2D
// (id 0) on any failure; model3d_doc.cpp already treats that as "no
// texture", same as an untextured material.
gfx::Texture2D LoadGltfImageTexture(const GltfDoc &doc, int image_index, const std::string &base_dir) {
    const Json &images = doc.root.get("images");
    if (image_index < 0 || static_cast<size_t>(image_index) >= images.size()) return gfx::Texture2D{};
    const Json &img = images.items()[static_cast<size_t>(image_index)];

    std::vector<uint8_t> bytes;
    if (img.contains("uri")) {
        bytes = ResolveUri(img.get("uri").as_string(""), base_dir);
    } else if (img.contains("bufferView")) {
        int bv_index = img.get("bufferView").as_int(-1);
        const Json &bvs = doc.root.get("bufferViews");
        if (bv_index < 0 || static_cast<size_t>(bv_index) >= bvs.size()) return gfx::Texture2D{};
        const Json &bv = bvs.items()[static_cast<size_t>(bv_index)];
        int buf_index = bv.get("buffer").as_int(-1);
        if (buf_index < 0 || static_cast<size_t>(buf_index) >= doc.buffers.size()) return gfx::Texture2D{};
        const std::vector<uint8_t> &buf = doc.buffers[static_cast<size_t>(buf_index)];
        auto offset = static_cast<size_t>(bv.get("byteOffset").as_int(0));
        auto length = static_cast<size_t>(bv.get("byteLength").as_int(0));
        if (offset + length > buf.size()) return gfx::Texture2D{};
        bytes.assign(buf.begin() + static_cast<long>(offset), buf.begin() + static_cast<long>(offset + length));
    } else {
        return gfx::Texture2D{};
    }
    if (bytes.empty()) return gfx::Texture2D{};

    int w = 0, h = 0;
    std::string error;
    unsigned char *pixels = image_codec::Decode(bytes.data(), bytes.size(), &w, &h, &error);
    if (pixels == nullptr) return gfx::Texture2D{};
    gfx::Image image{pixels, w, h, 1, gfx::kPixelFormatR8G8B8A8};
    // Same "decode then upload, model3d_doc.cpp reads it back with
    // LoadImageFromTexture" dance the OBJ importer's map_Kd path uses.
    gfx::Texture2D tex = gfx::LoadTextureFromImage(image);
    std::free(pixels);
    return tex;
}

// Reads materials[material_index].pbrMetallicRoughness.baseColorFactor/
// baseColorTexture (Object3D's flat tint + albedo texture) plus, since
// CHESS_SET_BENCHMARK_PLAN.md Phase 1 gave Object3D real roughness/
// metallic scalars and a normal-map slot to put them in,
// pbrMetallicRoughness.roughnessFactor/metallicFactor and
// material.normalTexture. Default white, semi-matte, non-metal,
// textureless material for material_index < 0 (no material on the
// primitive) or any missing/malformed field, matching glTF's own
// defaults (baseColorFactor opaque white, roughnessFactor/metallicFactor
// both 1.0 per spec).
//
// Deliberately NOT read: pbrMetallicRoughness.metallicRoughnessTexture.
// glTF packs that as ONE image (G channel = roughness, B channel =
// metallic, "ORM" convention) -- unpacking it into this app's two
// independent single-channel roughness/metallic texture slots would mean
// decoding and re-splitting the source image at import time, real extra
// work for a case the benchmark task this plan targets (procedurally-
// generated or scalar-driven materials, not glTF ORM textures) doesn't
// need. The *scalar* factors are read regardless of whether the source
// asset also has that texture -- per spec they're always present as the
// texture's multiplier (or the material's own value when there's no
// texture), so this is never "silently wrong," only "doesn't pick up
// per-pixel roughness/metallic variation from that one packed texture."
gfx::Material LoadGltfMaterial(const GltfDoc &doc, int material_index, const std::string &base_dir) {
    gfx::Material mat{};
    auto *maps = new gfx::MaterialMap[gfx::kMaxMaterialMaps]();
    maps[gfx::kMaterialMapAlbedo].color = gfx::White;
    maps[gfx::kMaterialMapRoughness].value = 1.0f;
    maps[gfx::kMaterialMapMetalness].value = 1.0f;
    mat.maps = maps;
    if (material_index < 0) return mat;

    const Json &materials = doc.root.get("materials");
    if (static_cast<size_t>(material_index) >= materials.size()) return mat;
    const Json &m = materials.items()[static_cast<size_t>(material_index)];
    const Json &pbr = m.get("pbrMetallicRoughness");

    if (pbr.contains("baseColorFactor")) {
        const Json &bc = pbr.get("baseColorFactor");
        if (bc.size() == 4) {
            auto to_byte = [](const Json &v) {
                return static_cast<unsigned char>(std::clamp(v.as_double(1.0), 0.0, 1.0) * 255.0);
            };
            maps[gfx::kMaterialMapAlbedo].color = gfx::Color{to_byte(bc.items()[0]), to_byte(bc.items()[1]),
                                                               to_byte(bc.items()[2]), to_byte(bc.items()[3])};
        }
    }
    if (pbr.contains("baseColorTexture")) {
        int tex_index = pbr.get("baseColorTexture").get("index").as_int(-1);
        const Json &textures = doc.root.get("textures");
        if (tex_index >= 0 && static_cast<size_t>(tex_index) < textures.size()) {
            int image_index = textures.items()[static_cast<size_t>(tex_index)].get("source").as_int(-1);
            maps[gfx::kMaterialMapAlbedo].texture = LoadGltfImageTexture(doc, image_index, base_dir);
        }
    }
    if (pbr.contains("roughnessFactor")) {
        maps[gfx::kMaterialMapRoughness].value = static_cast<float>(std::clamp(pbr.get("roughnessFactor").as_double(1.0), 0.0, 1.0));
    }
    if (pbr.contains("metallicFactor")) {
        maps[gfx::kMaterialMapMetalness].value = static_cast<float>(std::clamp(pbr.get("metallicFactor").as_double(1.0), 0.0, 1.0));
    }
    if (m.contains("normalTexture")) {
        int tex_index = m.get("normalTexture").get("index").as_int(-1);
        const Json &textures = doc.root.get("textures");
        if (tex_index >= 0 && static_cast<size_t>(tex_index) < textures.size()) {
            int image_index = textures.items()[static_cast<size_t>(tex_index)].get("source").as_int(-1);
            maps[gfx::kMaterialMapNormal].texture = LoadGltfImageTexture(doc, image_index, base_dir);
        }
    }
    return mat;
}

void BuildPrimitiveMesh(const GltfDoc &doc, const Json &primitive, Matrix world, std::vector<BuiltPrimitive> *out) {
    int mode = primitive.contains("mode") ? primitive.get("mode").as_int(4) : 4;
    if (mode != 4) return;  // triangle lists only, see this file's top comment
    if (!primitive.contains("attributes")) return;
    const Json &attrs = primitive.get("attributes");
    if (!attrs.contains("POSITION")) return;

    std::vector<float> positions = ReadAccessorFloats(doc, attrs.get("POSITION").as_int(-1), 3);
    if (positions.empty()) return;
    std::vector<float> normals =
        attrs.contains("NORMAL") ? ReadAccessorFloats(doc, attrs.get("NORMAL").as_int(-1), 3) : std::vector<float>();
    std::vector<float> texcoords = attrs.contains("TEXCOORD_0") ? ReadAccessorFloats(doc, attrs.get("TEXCOORD_0").as_int(-1), 2)
                                                                 : std::vector<float>();

    size_t vertex_count = positions.size() / 3;
    std::vector<uint32_t> indices;
    if (primitive.contains("indices")) {
        indices = ReadAccessorIndices(doc, primitive.get("indices").as_int(-1));
        if (indices.empty()) return;
    } else {
        indices.resize(vertex_count);
        for (size_t i = 0; i < vertex_count; i++) indices[i] = static_cast<uint32_t>(i);
    }
    if (indices.size() % 3 != 0) return;

    BuiltPrimitive prim;
    prim.positions.reserve(indices.size() * 3);
    prim.texcoords.reserve(indices.size() * 2);
    prim.normals.reserve(indices.size() * 3);
    bool has_normals = normals.size() == vertex_count * 3;
    bool has_texcoords = texcoords.size() == vertex_count * 2;

    for (uint32_t idx32 : indices) {
        if (idx32 >= vertex_count) return;  // malformed index, bail on this primitive
        size_t idx = idx32;
        Vector3 p = Vector3Transform(Vector3{positions[idx * 3], positions[idx * 3 + 1], positions[idx * 3 + 2]}, world);
        prim.positions.push_back(p.x);
        prim.positions.push_back(p.y);
        prim.positions.push_back(p.z);

        if (has_normals) {
            Vector3 n = Vector3Normalize(
                TransformDirection(Vector3{normals[idx * 3], normals[idx * 3 + 1], normals[idx * 3 + 2]}, world));
            prim.normals.push_back(n.x);
            prim.normals.push_back(n.y);
            prim.normals.push_back(n.z);
        } else {
            prim.normals.push_back(0);
            prim.normals.push_back(1);
            prim.normals.push_back(0);
        }

        if (has_texcoords) {
            prim.texcoords.push_back(texcoords[idx * 2]);
            prim.texcoords.push_back(texcoords[idx * 2 + 1]);
        } else {
            prim.texcoords.push_back(0);
            prim.texcoords.push_back(0);
        }
    }
    prim.material_index = primitive.contains("material") ? primitive.get("material").as_int(-1) : -1;
    out->push_back(std::move(prim));
}

void WalkNode(const GltfDoc &doc, int node_index, Matrix parent_world, std::vector<BuiltPrimitive> *out) {
    const Json &nodes = doc.root.get("nodes");
    if (node_index < 0 || static_cast<size_t>(node_index) >= nodes.size()) return;
    const Json &node = nodes.items()[static_cast<size_t>(node_index)];
    Matrix world = MatrixMultiply(NodeLocalMatrix(node), parent_world);

    if (node.contains("mesh")) {
        int mesh_index = node.get("mesh").as_int(-1);
        const Json &meshes = doc.root.get("meshes");
        if (mesh_index >= 0 && static_cast<size_t>(mesh_index) < meshes.size()) {
            const Json &mesh = meshes.items()[static_cast<size_t>(mesh_index)];
            for (const Json &prim : mesh.get("primitives").items()) BuildPrimitiveMesh(doc, prim, world, out);
        }
    }
    if (node.contains("children")) {
        for (const Json &child : node.get("children").items()) WalkNode(doc, child.as_int(-1), world, out);
    }
}

}  // namespace

gfx::Model LoadGltfModel(const char *file_name) {
    std::string path(file_name);
    std::string raw = ReadWholeFile(path, true);
    if (raw.empty()) {
        std::fprintf(stderr, "gfx native: glTF import: couldn't open '%s'\n", file_name);
        return gfx::Model{};
    }

    GltfDoc doc;
    std::string json_text;
    std::vector<uint8_t> glb_bin;
    bool have_glb_bin = false;

    if (raw.size() >= 12 && std::memcmp(raw.data(), "glTF", 4) == 0) {
        // GLB container: 12-byte header, then a JSON chunk and an
        // optional BIN chunk.
        uint32_t total_length = 0;
        std::memcpy(&total_length, raw.data() + 8, 4);
        size_t pos = 12;
        while (pos + 8 <= raw.size() && pos + 8 <= total_length) {
            uint32_t chunk_length = 0, chunk_type_bytes = 0;
            std::memcpy(&chunk_length, raw.data() + pos, 4);
            std::memcpy(&chunk_type_bytes, raw.data() + pos + 4, 4);
            size_t data_start = pos + 8;
            if (data_start + chunk_length > raw.size()) break;
            if (std::memcmp(raw.data() + pos + 4, "JSON", 4) == 0) {
                json_text.assign(raw.data() + data_start, chunk_length);
            } else if (std::memcmp(raw.data() + pos + 4, "BIN\0", 4) == 0) {
                glb_bin.assign(raw.begin() + static_cast<long>(data_start),
                                raw.begin() + static_cast<long>(data_start + chunk_length));
                have_glb_bin = true;
            }
            (void)chunk_type_bytes;
            pos = data_start + chunk_length;
        }
        if (json_text.empty()) {
            std::fprintf(stderr, "gfx native: glTF import: '%s' is a GLB file with no JSON chunk\n", file_name);
            return gfx::Model{};
        }
    } else {
        json_text = raw;
    }

    if (!Json::Parse(json_text, &doc.root) || !doc.root.is_object()) {
        std::fprintf(stderr, "gfx native: glTF import: '%s' isn't valid JSON\n", file_name);
        return gfx::Model{};
    }

    std::string base_dir = DirOf(path);
    for (const Json &buf : doc.root.get("buffers").items()) {
        if (buf.contains("uri")) {
            doc.buffers.push_back(ResolveUri(buf.get("uri").as_string(""), base_dir));
        } else if (have_glb_bin) {
            doc.buffers.push_back(glb_bin);
        } else {
            doc.buffers.emplace_back();
        }
    }

    std::vector<BuiltPrimitive> built;
    if (doc.root.contains("scenes") && doc.root.contains("nodes")) {
        int scene_index = doc.root.get("scene").as_int(0);
        const Json &scenes = doc.root.get("scenes");
        if (scene_index >= 0 && static_cast<size_t>(scene_index) < scenes.size()) {
            const Json &scene = scenes.items()[static_cast<size_t>(scene_index)];
            for (const Json &n : scene.get("nodes").items()) WalkNode(doc, n.as_int(-1), MatrixIdentity(), &built);
        }
    } else if (doc.root.contains("meshes")) {
        // No scene graph (rare -- e.g. a bare mesh/material library):
        // import every mesh's primitives at identity transform.
        for (const Json &mesh : doc.root.get("meshes").items()) {
            for (const Json &prim : mesh.get("primitives").items()) BuildPrimitiveMesh(doc, prim, MatrixIdentity(), &built);
        }
    }

    if (built.empty()) {
        std::fprintf(stderr, "gfx native: glTF import: '%s' has no triangle mesh data\n", file_name);
        return gfx::Model{};
    }

    auto *meshes = new gfx::Mesh[built.size()];
    auto *mesh_material = new int[built.size()];
    // Resolve each distinct glTF material index at most once (a texture
    // used by several primitives shouldn't be decoded/uploaded more than
    // once) -- "no material" (-1) is its own entry too, the shared
    // default-white material every materialless primitive points at.
    std::vector<int> unique_gltf_indices;
    std::vector<gfx::Material> resolved_materials;
    for (size_t i = 0; i < built.size(); i++) {
        const BuiltPrimitive &prim = built[i];
        gfx::Mesh mesh{};
        auto vcount = static_cast<int>(prim.positions.size() / 3);
        mesh.vertexCount = vcount;
        mesh.triangleCount = vcount / 3;

        auto *verts = new float[prim.positions.size()];
        std::memcpy(verts, prim.positions.data(), prim.positions.size() * sizeof(float));
        mesh.vertices = verts;

        auto *uvs = new float[prim.texcoords.size()];
        std::memcpy(uvs, prim.texcoords.data(), prim.texcoords.size() * sizeof(float));
        mesh.texcoords = uvs;

        auto *norms = new float[prim.normals.size()];
        std::memcpy(norms, prim.normals.data(), prim.normals.size() * sizeof(float));
        mesh.normals = norms;

        meshes[i] = mesh;

        auto found = std::find(unique_gltf_indices.begin(), unique_gltf_indices.end(), prim.material_index);
        if (found == unique_gltf_indices.end()) {
            mesh_material[i] = static_cast<int>(resolved_materials.size());
            unique_gltf_indices.push_back(prim.material_index);
            resolved_materials.push_back(LoadGltfMaterial(doc, prim.material_index, base_dir));
        } else {
            mesh_material[i] = static_cast<int>(found - unique_gltf_indices.begin());
        }
    }

    auto *materials = new gfx::Material[resolved_materials.size()];
    std::copy(resolved_materials.begin(), resolved_materials.end(), materials);

    gfx::Model model{};
    model.transform = gfx::MatrixIdentity();
    model.meshCount = static_cast<int>(built.size());
    model.materialCount = static_cast<int>(resolved_materials.size());
    model.materials = materials;
    model.meshMaterial = mesh_material;
    model.meshes = meshes;
    return model;
}

}  // namespace gfx
