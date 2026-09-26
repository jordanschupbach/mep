// Inter-Quake Model (.iqm) importer for Stage B8. Hand-written from the
// public IQM format spec (Lee Salzman, used by Sauerbraten/Cube 2 and
// widely as a simple animated-model interchange format) -- a small,
// stable binary format, no vendored parser needed.
//
// Static geometry only: reads position/texcoord/normal vertex arrays and
// triangle indices, one gfx::Mesh per iqmmesh entry. Skeleton/joints/
// poses/animation frames are deliberately not read -- gfx::Mesh (and
// model3d_doc.h's Object3D above it) has no rigging/skinning
// representation to import them into; every mesh comes out as a plain
// static bind-pose shape, same scope limitation as the OBJ importer's
// own "geometry only" note.
//
// All multi-byte fields are read via memcpy into a local, never through
// a cast-and-dereference of a misaligned pointer into the raw file
// buffer -- avoids both real alignment UB and mep's own -Wcast-align.

#include "gfx/backend_native_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "gfx/vecmath.h"

import mep.gfx.model_read_util;

namespace gfx {

namespace {

constexpr uint32_t kIqmPosition = 0;
constexpr uint32_t kIqmTexcoord = 1;
constexpr uint32_t kIqmNormal = 2;
constexpr uint32_t kIqmFloat = 7;

// One IQM_POSITION/TEXCOORD/NORMAL vertex array descriptor, resolved
// from the file's iqmvertexarray list below.
struct VertexArrayInfo {
    bool present = false;
    uint32_t format = 0;
    uint32_t size = 0;
    uint32_t offset = 0;
};

}  // namespace

gfx::Model LoadIqmModel(const char *file_name) {
    std::ifstream file(file_name, std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "gfx native: IQM import: couldn't open '%s'\n", file_name);
        return gfx::Model{};
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Header layout (124 bytes total): 16-byte magic, then 27 uint32
    // fields. Named offsets rather than a struct overlay -- see this
    // file's own top comment on why.
    constexpr size_t kHeaderSize = 124;
    // Compare exactly the 16 bytes of the file's magic field -- the
    // string literal "INTERQUAKEMODEL\0" is itself 17 bytes (16 content
    // bytes plus the compiler's own auto-appended terminator), and byte
    // 16 of the *file* is the start of the version field, not a second
    // null; comparing 17 bytes here compared that against 0 and always
    // failed.
    if (buf.size() < kHeaderSize || std::memcmp(buf.data(), "INTERQUAKEMODEL\0", 16) != 0) {
        std::fprintf(stderr, "gfx native: IQM import: '%s' isn't a valid IQM file\n", file_name);
        return gfx::Model{};
    }
    uint32_t version = ReadU32(buf, 16);
    if (version != 2) {
        std::fprintf(stderr, "gfx native: IQM import: '%s' has unsupported version %u (need 2)\n", file_name,
                     version);
        return gfx::Model{};
    }
    uint32_t num_meshes = ReadU32(buf, 36), ofs_meshes = ReadU32(buf, 40);
    // num_vertexes (offset 48) isn't needed directly -- per-mesh vertex
    // counts come from each iqmmesh entry below instead.
    uint32_t num_vertexarrays = ReadU32(buf, 44), ofs_vertexarrays = ReadU32(buf, 52);
    uint32_t num_triangles = ReadU32(buf, 56), ofs_triangles = ReadU32(buf, 60);

    if (num_meshes == 0 || num_triangles == 0) {
        std::fprintf(stderr, "gfx native: IQM import: '%s' has no meshes/triangles\n", file_name);
        return gfx::Model{};
    }

    // -- Vertex arrays: find POSITION/TEXCOORD/NORMAL among them. -------
    VertexArrayInfo position, texcoord, normal;
    for (uint32_t i = 0; i < num_vertexarrays; i++) {
        size_t entry = ofs_vertexarrays + static_cast<size_t>(i) * 20;  // iqmvertexarray is 5 uint32s
        if (entry + 20 > buf.size()) break;
        uint32_t type = ReadU32(buf, entry);
        uint32_t format = ReadU32(buf, entry + 8);
        uint32_t size = ReadU32(buf, entry + 12);
        uint32_t offset = ReadU32(buf, entry + 16);
        if (type == kIqmPosition) position = {true, format, size, offset};
        else if (type == kIqmTexcoord) texcoord = {true, format, size, offset};
        else if (type == kIqmNormal) normal = {true, format, size, offset};
    }
    if (!position.present || position.format != kIqmFloat || position.size < 3) {
        std::fprintf(stderr, "gfx native: IQM import: '%s' has no float POSITION vertex array\n", file_name);
        return gfx::Model{};
    }

    // -- Triangles: shared across all meshes, sliced per-mesh below. ----
    std::vector<uint32_t> tri_indices(static_cast<size_t>(num_triangles) * 3);
    for (uint32_t t = 0; t < num_triangles; t++) {
        size_t entry = ofs_triangles + static_cast<size_t>(t) * 12;  // iqmtriangle is 3 uint32s
        if (entry + 12 > buf.size()) break;
        tri_indices[static_cast<size_t>(t) * 3 + 0] = ReadU32(buf, entry);
        tri_indices[static_cast<size_t>(t) * 3 + 1] = ReadU32(buf, entry + 4);
        tri_indices[static_cast<size_t>(t) * 3 + 2] = ReadU32(buf, entry + 8);
    }

    auto *meshes = new gfx::Mesh[num_meshes];
    for (uint32_t m = 0; m < num_meshes; m++) {
        // iqmmesh: name, material, first_vertex, num_vertexes, first_triangle,
        // num_triangles -- 6 uint32 fields, 24 bytes.
        size_t entry = ofs_meshes + static_cast<size_t>(m) * 24;
        uint32_t first_vertex = ReadU32(buf, entry + 8), mesh_num_vertexes = ReadU32(buf, entry + 12);
        uint32_t first_triangle = ReadU32(buf, entry + 16), mesh_num_triangles = ReadU32(buf, entry + 20);
        meshes[m] = gfx::Mesh{};
        meshes[m].vertexCount = static_cast<int>(mesh_num_vertexes);

        auto *verts = new float[static_cast<size_t>(mesh_num_vertexes) * 3];
        for (uint32_t v = 0; v < mesh_num_vertexes; v++) {
            size_t src = position.offset + static_cast<size_t>(first_vertex + v) * position.size * 4;
            verts[v * 3 + 0] = ReadF32(buf, src);
            verts[v * 3 + 1] = ReadF32(buf, src + 4);
            verts[v * 3 + 2] = ReadF32(buf, src + 8);
        }
        meshes[m].vertices = verts;

        if (texcoord.present && texcoord.format == kIqmFloat && texcoord.size >= 2) {
            auto *uvs = new float[static_cast<size_t>(mesh_num_vertexes) * 2];
            for (uint32_t v = 0; v < mesh_num_vertexes; v++) {
                size_t src = texcoord.offset + static_cast<size_t>(first_vertex + v) * texcoord.size * 4;
                uvs[v * 2 + 0] = ReadF32(buf, src);
                uvs[v * 2 + 1] = ReadF32(buf, src + 4);
            }
            meshes[m].texcoords = uvs;
        }
        if (normal.present && normal.format == kIqmFloat && normal.size >= 3) {
            auto *norms = new float[static_cast<size_t>(mesh_num_vertexes) * 3];
            for (uint32_t v = 0; v < mesh_num_vertexes; v++) {
                size_t src = normal.offset + static_cast<size_t>(first_vertex + v) * normal.size * 4;
                norms[v * 3 + 0] = ReadF32(buf, src);
                norms[v * 3 + 1] = ReadF32(buf, src + 4);
                norms[v * 3 + 2] = ReadF32(buf, src + 8);
            }
            meshes[m].normals = norms;
        }

        auto *indices = new unsigned int[static_cast<size_t>(mesh_num_triangles) * 3];
        for (uint32_t t = 0; t < mesh_num_triangles; t++) {
            size_t src_tri = static_cast<size_t>(first_triangle + t) * 3;
            for (int k = 0; k < 3; k++) {
                uint32_t global_vi = tri_indices[src_tri + static_cast<size_t>(k)];
                // No narrowing cast any more: gfx::Mesh::indices is 32-bit,
                // which is the same width IQM itself stores these in, so a
                // mesh with more than 65,536 vertices now survives the trip.
                indices[t * 3 + static_cast<size_t>(k)] = global_vi - first_vertex;
            }
        }
        meshes[m].indices = indices;
        meshes[m].triangleCount = static_cast<int>(mesh_num_triangles);
    }

    gfx::Model model{};
    model.transform = gfx::MatrixIdentity();
    model.meshCount = static_cast<int>(num_meshes);
    model.meshes = meshes;
    return model;
}

}  // namespace gfx
