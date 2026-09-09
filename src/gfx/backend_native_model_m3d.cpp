// Model 3D (.m3d) importer for Stage B8. Hand-written from bzt's public
// binary format spec (https://gitlab.com/bztsrc/model3d) -- a chunked
// binary format ("3DMO" file wrapper, then a HEAD chunk declaring
// per-field integer widths, then CMAP/TMAP/VRTS/BONE/MTRL/MESH/... sibling
// chunks) whose defining trait is that most integer fields (vertex/color/
// texcoord/string-offset indices) are *variable width* -- 1, 2 or 4 bytes,
// chosen per-file by 2-bit fields packed into the HEAD chunk's `types`
// word -- to keep small models small.
//
// Reads geometry only, same scope as this file's OBJ/IQM/VOX siblings:
// every MESH chunk's triangle stream is flattened into one gfx::Mesh
// (M3D's per-material multi-mesh split and its BONE/skin/ACTN animation
// data are read by nothing downstream of LoadModel -- see this codebase's
// own note on model3d_doc.cpp's "decode, copy geometry out, discard"
// pattern -- so neither is worth the added complexity here). M3D's own
// voxel-chunk encoding (VOXT/VOXD, an alternate way to store what's
// already this codebase's dedicated VOX importer's job) is skipped for
// the same reason: out of scope, not the common case for this format.
//
// M3D's "distribution" flavor deflate-compresses everything after the
// HEAD lookup point; decoded via deflate.h's zlib-wrapped inflate (mep's
// own in-house DEFLATE codec -- see MINIZ_REMOVAL_PLAN.md) since M3D's
// compressed body uses the same zlib framing (RFC 1950) PNG's IDAT
// chunks do, not ZIP's -- one shared codec for both, not a separate
// implementation per container.

#include "gfx/backend_native_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "deflate.h"
#include "gfx/vecmath.h"

namespace gfx {

namespace {

constexpr uint32_t kUndef = 0xFFFFFFFFu;

uint32_t ReadU32(const std::vector<char> &buf, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}
uint16_t ReadU16(const std::vector<char> &buf, size_t offset) {
    uint16_t v = 0;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}
float ReadF32(const std::vector<char> &buf, size_t offset) {
    float v = 0.0f;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}
double ReadF64(const std::vector<char> &buf, size_t offset) {
    double v = 0.0;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}

bool ChunkIs(const std::vector<char> &buf, size_t offset, const char *magic) {
    return offset + 4 <= buf.size() && std::memcmp(buf.data() + offset, magic, 4) == 0;
}

// A variable-width (1/2/4-byte) index field: values near the top of the
// field's range mean "undefined", matching m3d.h's own _m3d_getidx().
uint32_t ReadIdx(const std::vector<char> &buf, size_t offset, int width) {
    switch (width) {
        case 1: {
            auto b = static_cast<uint8_t>(buf[offset]);
            return b >= 254 ? kUndef : b;
        }
        case 2: {
            uint16_t v = ReadU16(buf, offset);
            return v >= 65534 ? kUndef : v;
        }
        case 4:
            return ReadU32(buf, offset);
        default:
            return kUndef;
    }
}

// Vertex/texcoord coordinates are quantized to 1/2/4/8 bytes; signed
// (vertex positions, normals -- range roughly [-1,1]) vs. unsigned
// (texcoords -- range [0,1]) use different fixed-point conventions.
float ReadSignedNorm(const std::vector<char> &buf, size_t offset, int width) {
    switch (width) {
        case 1: return static_cast<float>(static_cast<int8_t>(buf[offset])) / 127.0f;
        case 2: return static_cast<float>(static_cast<int16_t>(ReadU16(buf, offset))) / 32767.0f;
        case 4: return ReadF32(buf, offset);
        case 8: return static_cast<float>(ReadF64(buf, offset));
        default: return 0.0f;
    }
}
float ReadUnsignedNorm(const std::vector<char> &buf, size_t offset, int width) {
    switch (width) {
        case 1: return static_cast<float>(static_cast<uint8_t>(buf[offset])) / 255.0f;
        case 2: return static_cast<float>(ReadU16(buf, offset)) / 65535.0f;
        case 4: return ReadF32(buf, offset);
        case 8: return static_cast<float>(ReadF64(buf, offset));
        default: return 0.0f;
    }
}

struct M3dVertex {
    float x = 0, y = 0, z = 0;
    unsigned char color[4] = {255, 255, 255, 255};
    bool has_color = false;
};

struct M3dFace {
    uint32_t vertex[3] = {kUndef, kUndef, kUndef};
    uint32_t texcoord[3] = {kUndef, kUndef, kUndef};
    uint32_t normal[3] = {kUndef, kUndef, kUndef};
};

struct M3dTexcoord {
    float u = 0, v = 0;
};

}  // namespace

gfx::Model LoadM3dModel(const char *file_name) {
    std::ifstream file(file_name, std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "gfx native: M3D import: couldn't open '%s'\n", file_name);
        return gfx::Model{};
    }
    std::vector<char> file_buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (file_buf.size() < 8 || std::memcmp(file_buf.data(), "3DMO", 4) != 0) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' isn't a valid M3D file\n", file_name);
        return gfx::Model{};
    }
    size_t declared_len = ReadU32(file_buf, 4);
    if (declared_len > file_buf.size()) declared_len = file_buf.size();
    size_t pos = 8;

    if (ChunkIs(file_buf, pos, "PRVW")) {
        uint32_t prvw_len = ReadU32(file_buf, pos + 4);
        if (prvw_len < 8) prvw_len = 8;
        pos += prvw_len;
    }
    if (pos > declared_len) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' has a malformed preview chunk\n", file_name);
        return gfx::Model{};
    }

    // From here on, everything is read out of `work` -- either the
    // original file bytes (uncompressed body) or an inflated copy
    // (compressed "distribution" body), always starting at the HEAD
    // chunk so the rest of this function doesn't need to know which.
    std::vector<char> inflated_storage;
    const std::vector<char> *workp = &file_buf;
    size_t head_start = pos;

    if (!ChunkIs(file_buf, pos, "HEAD")) {
        size_t comp_len = declared_len - pos;
        std::string inflated;
        bool ok = deflate::InflateZlib(reinterpret_cast<const unsigned char *>(file_buf.data() + pos), comp_len, inflated);
        if (!ok) {
            std::fprintf(stderr, "gfx native: M3D import: '%s' body isn't valid HEAD or zlib data\n", file_name);
            return gfx::Model{};
        }
        inflated_storage.assign(inflated.begin(), inflated.end());
        if (!ChunkIs(inflated_storage, 0, "HEAD")) {
            std::fprintf(stderr, "gfx native: M3D import: '%s' decompressed body has no HEAD chunk\n", file_name);
            return gfx::Model{};
        }
        workp = &inflated_storage;
        head_start = 0;
    }
    const std::vector<char> &work = *workp;

    if (head_start + 16 > work.size()) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' HEAD chunk is truncated\n", file_name);
        return gfx::Model{};
    }
    uint32_t head_length = ReadU32(work, head_start + 4);
    float scale = ReadF32(work, head_start + 8);
    if (scale <= 0.0f) scale = 1.0f;
    uint32_t types = ReadU32(work, head_start + 12);
    // Field widths, decoded the same way m3d.h's own loader does: a 2-bit
    // field per index kind, 1<<field gives the byte width; several kinds
    // (color/texcoord/string/skin index here -- others exist for
    // bones/actions/shapes/voxels, unused since those chunks are skipped)
    // use width 8 to mean "0: field doesn't exist in the file at all".
    int vc_s = 1 << ((types >> 0) & 3);
    int vi_s = 1 << ((types >> 2) & 3);
    int si_s = 1 << ((types >> 4) & 3);
    int ci_s = 1 << ((types >> 6) & 3);
    int ti_s = 1 << ((types >> 8) & 3);
    int sk_s = 1 << ((types >> 14) & 3);
    if (ci_s == 8) ci_s = 0;
    if (ti_s == 8) ti_s = 0;
    if (sk_s == 8) sk_s = 0;
    if (vi_s > 4 || si_s > 4 || vc_s > 8) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' uses an index/coordinate width this importer doesn't support\n",
                     file_name);
        return gfx::Model{};
    }

    size_t next_chunk = head_start + head_length;
    if (next_chunk > work.size()) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' HEAD chunk length overruns the file\n", file_name);
        return gfx::Model{};
    }

    std::vector<uint32_t> cmap;
    std::vector<M3dVertex> vertices;
    std::vector<M3dTexcoord> tmap;
    std::vector<M3dFace> faces;

    size_t chunk_pos = next_chunk;
    while (chunk_pos + 8 <= work.size() && !ChunkIs(work, chunk_pos, "OMD3")) {
        uint32_t chunk_len = ReadU32(work, chunk_pos + 4);
        if (chunk_len < 8 || chunk_pos + chunk_len > work.size()) break;
        size_t content_start = chunk_pos + 8;
        size_t content_len = chunk_len - 8;

        if (ChunkIs(work, chunk_pos, "CMAP")) {
            cmap.resize(content_len / 4);
            for (size_t i = 0; i < cmap.size(); i++) cmap[i] = ReadU32(work, content_start + i * 4);
        } else if (ChunkIs(work, chunk_pos, "TMAP") && vc_s <= 4) {
            size_t reclen = static_cast<size_t>(vc_s) * 2;
            if (reclen > 0) {
                size_t count = content_len / reclen;
                tmap.reserve(count);
                for (size_t i = 0; i < count; i++) {
                    size_t p = content_start + i * reclen;
                    M3dTexcoord tc;
                    tc.u = ReadUnsignedNorm(work, p, vc_s);
                    tc.v = ReadUnsignedNorm(work, p + static_cast<size_t>(vc_s), vc_s);
                    tmap.push_back(tc);
                }
            }
        } else if (ChunkIs(work, chunk_pos, "VRTS")) {
            size_t reclen = static_cast<size_t>(ci_s) + static_cast<size_t>(sk_s) + 4 * static_cast<size_t>(vc_s);
            if (reclen > 0) {
                size_t count = content_len / reclen;
                vertices.reserve(count);
                size_t p = content_start;
                for (size_t i = 0; i < count; i++) {
                    M3dVertex v;
                    v.x = ReadSignedNorm(work, p, vc_s);
                    v.y = ReadSignedNorm(work, p + static_cast<size_t>(vc_s), vc_s);
                    v.z = ReadSignedNorm(work, p + 2 * static_cast<size_t>(vc_s), vc_s);
                    // vertex.w (4th quantized field) is a bone-blend weight, unused here.
                    p += 4 * static_cast<size_t>(vc_s);
                    if (ci_s > 0) {
                        uint32_t packed = ci_s == 4 ? ReadU32(work, p)
                                           : ci_s == 2 ? (cmap.empty() ? 0 : cmap[ReadU16(work, p) % cmap.size()])
                                                       : (cmap.empty() ? 0 : cmap[static_cast<uint8_t>(work[p]) % cmap.size()]);
                        std::memcpy(v.color, &packed, 4);
                        v.has_color = (v.color[3] != 0);
                        p += static_cast<size_t>(ci_s);
                    }
                    p += static_cast<size_t>(sk_s);  // skin (bone weights) index -- unused, skipped
                    vertices.push_back(v);
                }
            }
        } else if (ChunkIs(work, chunk_pos, "MESH")) {
            size_t p = content_start;
            size_t chunk_end = chunk_pos + chunk_len;
            while (p < chunk_end) {
                auto ctrl = static_cast<uint8_t>(work[p++]);
                int n = ctrl >> 4;
                int k = ctrl & 15;
                if (n == 0) {
                    // "use material" / "use parameter" control record --
                    // both just carry a string-table offset (si_s bytes)
                    // this geometry-only importer has no use for.
                    p += static_cast<size_t>(si_s);
                    continue;
                }
                if (n != 3) {
                    std::fprintf(stderr, "gfx native: M3D import: '%s' has a non-triangle MESH primitive, skipping rest\n",
                                 file_name);
                    break;
                }
                M3dFace face;
                for (int j = 0; j < 3 && p + static_cast<size_t>(vi_s) <= chunk_end; j++) {
                    face.vertex[j] = ReadIdx(work, p, vi_s);
                    p += static_cast<size_t>(vi_s);
                    if (k & 1) {
                        face.texcoord[j] = ti_s > 0 ? ReadIdx(work, p, ti_s) : kUndef;
                        p += static_cast<size_t>(ti_s);
                    }
                    if (k & 2) {
                        face.normal[j] = ReadIdx(work, p, vi_s);
                        p += static_cast<size_t>(vi_s);
                    }
                    if (k & 4) p += static_cast<size_t>(vi_s);  // morph-target vertex index, unused
                }
                faces.push_back(face);
            }
        }
        // BONE/MTRL/ACTN/LBLS/ASET/SHPE/VOXT/VOXD/PRVW and any unknown
        // chunk: intentionally skipped, see this file's own top comment.
        chunk_pos += chunk_len;
    }

    if (faces.empty() || vertices.empty()) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' has no triangle mesh data\n", file_name);
        return gfx::Model{};
    }

    std::vector<float> positions, texcoords, normals;
    std::vector<unsigned char> colors;
    positions.reserve(faces.size() * 9);
    texcoords.reserve(faces.size() * 6);
    normals.reserve(faces.size() * 9);
    colors.reserve(faces.size() * 12);

    for (const M3dFace &face : faces) {
        bool valid = true;
        Vector3 pos3[3];
        for (int j = 0; j < 3; j++) {
            if (face.vertex[j] == kUndef || face.vertex[j] >= vertices.size()) { valid = false; break; }
            const M3dVertex &v = vertices[face.vertex[j]];
            pos3[j] = Vector3{v.x * scale, v.y * scale, v.z * scale};
        }
        if (!valid) continue;

        // Flat fallback normal (this face's own plane), used for any
        // corner that has no explicit normal index.
        Vector3 flat = Vector3Normalize(
            Vector3CrossProduct(Vector3Subtract(pos3[1], pos3[0]), Vector3Subtract(pos3[2], pos3[0])));

        for (int j = 0; j < 3; j++) {
            positions.push_back(pos3[j].x);
            positions.push_back(pos3[j].y);
            positions.push_back(pos3[j].z);

            const M3dVertex &v = vertices[face.vertex[j]];
            if (v.has_color) {
                colors.push_back(v.color[0]);
                colors.push_back(v.color[1]);
                colors.push_back(v.color[2]);
                colors.push_back(v.color[3]);
            } else {
                colors.push_back(255);
                colors.push_back(255);
                colors.push_back(255);
                colors.push_back(255);
            }

            if (face.texcoord[j] != kUndef && face.texcoord[j] < tmap.size()) {
                texcoords.push_back(tmap[face.texcoord[j]].u);
                texcoords.push_back(1.0f - tmap[face.texcoord[j]].v);
            } else {
                texcoords.push_back(0.0f);
                texcoords.push_back(0.0f);
            }

            if (face.normal[j] != kUndef && face.normal[j] < vertices.size()) {
                const M3dVertex &nv = vertices[face.normal[j]];
                normals.push_back(nv.x);
                normals.push_back(nv.y);
                normals.push_back(nv.z);
            } else {
                normals.push_back(flat.x);
                normals.push_back(flat.y);
                normals.push_back(flat.z);
            }
        }
    }

    if (positions.empty()) {
        std::fprintf(stderr, "gfx native: M3D import: '%s' has no usable triangles\n", file_name);
        return gfx::Model{};
    }

    auto *meshes = new gfx::Mesh[1];
    meshes[0] = gfx::Mesh{};
    auto vcount = static_cast<int>(positions.size() / 3);
    meshes[0].vertexCount = vcount;
    meshes[0].triangleCount = vcount / 3;

    auto *verts = new float[positions.size()];
    std::memcpy(verts, positions.data(), positions.size() * sizeof(float));
    meshes[0].vertices = verts;

    auto *uvs = new float[texcoords.size()];
    std::memcpy(uvs, texcoords.data(), texcoords.size() * sizeof(float));
    meshes[0].texcoords = uvs;

    auto *norms = new float[normals.size()];
    std::memcpy(norms, normals.data(), normals.size() * sizeof(float));
    meshes[0].normals = norms;

    auto *cols = new unsigned char[colors.size()];
    std::memcpy(cols, colors.data(), colors.size() * sizeof(unsigned char));
    meshes[0].colors = cols;

    gfx::Model model{};
    model.transform = gfx::MatrixIdentity();
    model.meshCount = 1;
    model.meshes = meshes;
    return model;
}

}  // namespace gfx
