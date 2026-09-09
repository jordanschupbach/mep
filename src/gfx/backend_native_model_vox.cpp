// MagicaVoxel .vox importer for Stage B8. Hand-written from the public,
// well-documented VOX chunk format (RIFF-style chunked binary: 4-byte
// magic "VOX ", int32 version, then a MAIN chunk whose children include
// PACK/SIZE/XYZI/RGBA) -- a simple, stable voxel-art interchange format,
// no vendored parser needed.
//
// Reads geometry only, same scope as this file's OBJ/IQM siblings: one
// gfx::Mesh per SIZE+XYZI model pair. VOX has no UV/texture concept, so
// texcoords are left absent and per-voxel color instead goes through
// gfx::Mesh's vertex-color channel (see this module's earlier addition
// of vertex color to the mesh pipeline, added specifically for this
// importer). Faces are hidden-face-culled -- only a face touching empty
// space or the 0..255 grid boundary is emitted -- since naively emitting
// all 6 faces of every voxel would blow up vertex counts on any
// non-trivial model. Each mesh is built as flat (non-indexed) triangle
// data, same "skip the index buffer" choice backend_native_model_obj.cpp
// makes: an indexed unsigned short buffer caps out at 65535 vertices,
// which a moderately large voxel model's visible-face count can exceed;
// glDrawArrays has no such limit.
//
// Coordinate systems: VOX is Z-up (x,y is the footprint, z is height);
// mep's 3D viewport is Y-up, so every voxel corner is remapped
// (vx, vy, vz) -> (vx, vz, vy) on the way out.

#include "gfx/backend_native_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "gfx/vecmath.h"

namespace gfx {

namespace {

uint32_t ReadU32(const std::vector<char> &buf, size_t offset) {
    uint32_t v = 0;
    std::memcpy(&v, buf.data() + offset, sizeof(v));
    return v;
}

struct Voxel {
    uint8_t x = 0, y = 0, z = 0, color_index = 0;
};

// One SIZE+XYZI model pair, gathered while scanning MAIN's children.
// size_x/y/z are read but not otherwise used: XYZI itself stores each
// voxel's x/y/z as a single byte, so voxel coordinates are already
// bounded to 0..255 regardless of what SIZE declares.
struct VoxModel {
    std::vector<Voxel> voxels;
};

struct RgbaColor {
    uint8_t r, g, b, a;
};

// MagicaVoxel's built-in default palette, used whenever a .vox file has
// no RGBA chunk of its own. Public, documented part of the file format
// (every VOX reader ships the same table) -- palette[i] applies to
// voxel color byte value (i + 1); a color-index byte of 0 means "no
// voxel" and never appears in a well-formed XYZI entry.
constexpr RgbaColor kDefaultPalette[256] = {
    {0, 0, 0, 0},
    {255, 255, 255, 255}, {255, 255, 204, 255}, {255, 255, 153, 255}, {255, 255, 102, 255}, {255, 255, 51, 255}, {255, 255, 0, 255},
    {255, 204, 255, 255}, {255, 204, 204, 255}, {255, 204, 153, 255}, {255, 204, 102, 255}, {255, 204, 51, 255}, {255, 204, 0, 255},
    {255, 153, 255, 255}, {255, 153, 204, 255}, {255, 153, 153, 255}, {255, 153, 102, 255}, {255, 153, 51, 255}, {255, 153, 0, 255},
    {255, 102, 255, 255}, {255, 102, 204, 255}, {255, 102, 153, 255}, {255, 102, 102, 255}, {255, 102, 51, 255}, {255, 102, 0, 255},
    {255, 51, 255, 255}, {255, 51, 204, 255}, {255, 51, 153, 255}, {255, 51, 102, 255}, {255, 51, 51, 255}, {255, 51, 0, 255},
    {255, 0, 255, 255}, {255, 0, 204, 255}, {255, 0, 153, 255}, {255, 0, 102, 255}, {255, 0, 51, 255}, {255, 0, 0, 255},
    {204, 255, 255, 255}, {204, 255, 204, 255}, {204, 255, 153, 255}, {204, 255, 102, 255}, {204, 255, 51, 255}, {204, 255, 0, 255},
    {204, 204, 255, 255}, {204, 204, 204, 255}, {204, 204, 153, 255}, {204, 204, 102, 255}, {204, 204, 51, 255}, {204, 204, 0, 255},
    {204, 153, 255, 255}, {204, 153, 204, 255}, {204, 153, 153, 255}, {204, 153, 102, 255}, {204, 153, 51, 255}, {204, 153, 0, 255},
    {204, 102, 255, 255}, {204, 102, 204, 255}, {204, 102, 153, 255}, {204, 102, 102, 255}, {204, 102, 51, 255}, {204, 102, 0, 255},
    {204, 51, 255, 255}, {204, 51, 204, 255}, {204, 51, 153, 255}, {204, 51, 102, 255}, {204, 51, 51, 255}, {204, 51, 0, 255},
    {204, 0, 255, 255}, {204, 0, 204, 255}, {204, 0, 153, 255}, {204, 0, 102, 255}, {204, 0, 51, 255}, {204, 0, 0, 255},
    {153, 255, 255, 255}, {153, 255, 204, 255}, {153, 255, 153, 255}, {153, 255, 102, 255}, {153, 255, 51, 255}, {153, 255, 0, 255},
    {153, 204, 255, 255}, {153, 204, 204, 255}, {153, 204, 153, 255}, {153, 204, 102, 255}, {153, 204, 51, 255}, {153, 204, 0, 255},
    {153, 153, 255, 255}, {153, 153, 204, 255}, {153, 153, 153, 255}, {153, 153, 102, 255}, {153, 153, 51, 255}, {153, 153, 0, 255},
    {153, 102, 255, 255}, {153, 102, 204, 255}, {153, 102, 153, 255}, {153, 102, 102, 255}, {153, 102, 51, 255}, {153, 102, 0, 255},
    {153, 51, 255, 255}, {153, 51, 204, 255}, {153, 51, 153, 255}, {153, 51, 102, 255}, {153, 51, 51, 255}, {153, 51, 0, 255},
    {153, 0, 255, 255}, {153, 0, 204, 255}, {153, 0, 153, 255}, {153, 0, 102, 255}, {153, 0, 51, 255}, {153, 0, 0, 255},
    {102, 255, 255, 255}, {102, 255, 204, 255}, {102, 255, 153, 255}, {102, 255, 102, 255}, {102, 255, 51, 255}, {102, 255, 0, 255},
    {102, 204, 255, 255}, {102, 204, 204, 255}, {102, 204, 153, 255}, {102, 204, 102, 255}, {102, 204, 51, 255}, {102, 204, 0, 255},
    {102, 153, 255, 255}, {102, 153, 204, 255}, {102, 153, 153, 255}, {102, 153, 102, 255}, {102, 153, 51, 255}, {102, 153, 0, 255},
    {102, 102, 255, 255}, {102, 102, 204, 255}, {102, 102, 153, 255}, {102, 102, 102, 255}, {102, 102, 51, 255}, {102, 102, 0, 255},
    {102, 51, 255, 255}, {102, 51, 204, 255}, {102, 51, 153, 255}, {102, 51, 102, 255}, {102, 51, 51, 255}, {102, 51, 0, 255},
    {102, 0, 255, 255}, {102, 0, 204, 255}, {102, 0, 153, 255}, {102, 0, 102, 255}, {102, 0, 51, 255}, {102, 0, 0, 255},
    {51, 255, 255, 255}, {51, 255, 204, 255}, {51, 255, 153, 255}, {51, 255, 102, 255}, {51, 255, 51, 255}, {51, 255, 0, 255},
    {51, 204, 255, 255}, {51, 204, 204, 255}, {51, 204, 153, 255}, {51, 204, 102, 255}, {51, 204, 51, 255}, {51, 204, 0, 255},
    {51, 153, 255, 255}, {51, 153, 204, 255}, {51, 153, 153, 255}, {51, 153, 102, 255}, {51, 153, 51, 255}, {51, 153, 0, 255},
    {51, 102, 255, 255}, {51, 102, 204, 255}, {51, 102, 153, 255}, {51, 102, 102, 255}, {51, 102, 51, 255}, {51, 102, 0, 255},
    {51, 51, 255, 255}, {51, 51, 204, 255}, {51, 51, 153, 255}, {51, 51, 102, 255}, {51, 51, 51, 255}, {51, 51, 0, 255},
    {51, 0, 255, 255}, {51, 0, 204, 255}, {51, 0, 153, 255}, {51, 0, 102, 255}, {51, 0, 51, 255}, {51, 0, 0, 255},
    {0, 255, 255, 255}, {0, 255, 204, 255}, {0, 255, 153, 255}, {0, 255, 102, 255}, {0, 255, 51, 255}, {0, 255, 0, 255},
    {0, 204, 255, 255}, {0, 204, 204, 255}, {0, 204, 153, 255}, {0, 204, 102, 255}, {0, 204, 51, 255}, {0, 204, 0, 255},
    {0, 153, 255, 255}, {0, 153, 204, 255}, {0, 153, 153, 255}, {0, 153, 102, 255}, {0, 153, 51, 255}, {0, 153, 0, 255},
    {0, 102, 255, 255}, {0, 102, 204, 255}, {0, 102, 153, 255}, {0, 102, 102, 255}, {0, 102, 51, 255}, {0, 102, 0, 255},
    {0, 51, 255, 255}, {0, 51, 204, 255}, {0, 51, 153, 255}, {0, 51, 102, 255}, {0, 51, 51, 255}, {0, 51, 0, 255},
    {0, 0, 255, 255}, {0, 0, 204, 255}, {0, 0, 153, 255}, {0, 0, 102, 255}, {0, 0, 51, 255},
    {224, 0, 0, 255}, {192, 0, 0, 255}, {160, 0, 0, 255}, {128, 0, 0, 255}, {96, 0, 0, 255}, {64, 0, 0, 255}, {32, 0, 0, 255},
    {0, 224, 0, 255}, {0, 192, 0, 255}, {0, 160, 0, 255}, {0, 128, 0, 255}, {0, 96, 0, 255}, {0, 64, 0, 255}, {0, 32, 0, 255},
    {0, 0, 224, 255}, {0, 0, 192, 255}, {0, 0, 160, 255}, {0, 0, 128, 255}, {0, 0, 96, 255}, {0, 0, 64, 255}, {0, 0, 32, 255},
    {224, 224, 224, 255}, {192, 192, 192, 255}, {160, 160, 160, 255}, {128, 128, 128, 255}, {96, 96, 96, 255}, {64, 64, 64, 255}, {32, 32, 32, 255},
    {0, 0, 0, 255},
};

// Dense 256^3 occupancy grid (one byte per cell -- 16MiB) -- safe as a
// fixed upper bound because XYZI stores each voxel coordinate as a
// single byte, so no voxel can ever fall outside 0..255 regardless of
// what a file's SIZE chunk claims.
constexpr int kGridDim = 256;

size_t GridIndex(int x, int y, int z) {
    return (static_cast<size_t>(x) * kGridDim + static_cast<size_t>(y)) * kGridDim + static_cast<size_t>(z);
}

uint8_t GridAt(const std::vector<uint8_t> &grid, int x, int y, int z) {
    if (x < 0 || x >= kGridDim || y < 0 || y >= kGridDim || z < 0 || z >= kGridDim) return 0;
    return grid[GridIndex(x, y, z)];
}

// VOX (Z-up) -> mep (Y-up).
Vector3 ToMep(float x, float y, float z) { return Vector3{x, z, y}; }

// One face of a unit voxel cube: 4 corners (as fractional offsets from
// the voxel's minimum corner) in the winding that makes
// cross(c1-c0, c2-c1) point along `normal` -- verified by hand, not
// load-bearing for visibility since the renderer never enables
// GL_CULL_FACE, but correct winding costs nothing extra here.
struct FaceDef {
    int dx, dy, dz;  // neighbor offset that must be empty for this face to be drawn
    float corners[4][3];
};

constexpr FaceDef kFaces[6] = {
    {1, 0, 0, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}},   // +X
    {-1, 0, 0, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}},  // -X
    {0, 1, 0, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},   // +Y
    {0, -1, 0, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},  // -Y
    {0, 0, 1, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},   // +Z
    {0, 0, -1, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}},  // -Z
};

gfx::Mesh BuildVoxMesh(const VoxModel &vm, const RgbaColor *palette) {
    std::vector<uint8_t> grid(static_cast<size_t>(kGridDim) * kGridDim * kGridDim, 0);
    for (const Voxel &v : vm.voxels) {
        if (v.color_index == 0) continue;  // 0 means "no voxel" per format
        grid[GridIndex(v.x, v.y, v.z)] = v.color_index;
    }

    std::vector<float> positions, normals;
    std::vector<unsigned char> colors;

    for (const Voxel &v : vm.voxels) {
        if (v.color_index == 0) continue;
        RgbaColor c = palette[v.color_index - 1];
        for (const FaceDef &face : kFaces) {
            if (GridAt(grid, v.x + face.dx, v.y + face.dy, v.z + face.dz) != 0) continue;  // hidden, skip

            Vector3 corner[4];
            for (int i = 0; i < 4; i++) {
                corner[i] = ToMep(static_cast<float>(v.x) + face.corners[i][0], static_cast<float>(v.y) + face.corners[i][1],
                                   static_cast<float>(v.z) + face.corners[i][2]);
            }
            Vector3 normal = ToMep(static_cast<float>(face.dx), static_cast<float>(face.dy), static_cast<float>(face.dz));

            // Two triangles, flat (non-indexed): 0,1,2 and 0,2,3.
            static constexpr int kTriOrder[6] = {0, 1, 2, 0, 2, 3};
            for (int idx : kTriOrder) {
                positions.push_back(corner[idx].x);
                positions.push_back(corner[idx].y);
                positions.push_back(corner[idx].z);
                normals.push_back(normal.x);
                normals.push_back(normal.y);
                normals.push_back(normal.z);
                colors.push_back(c.r);
                colors.push_back(c.g);
                colors.push_back(c.b);
                colors.push_back(c.a);
            }
        }
    }

    gfx::Mesh mesh{};
    auto vcount = static_cast<int>(positions.size() / 3);
    mesh.vertexCount = vcount;
    mesh.triangleCount = vcount / 3;
    if (vcount > 0) {
        auto *verts = new float[positions.size()];
        std::memcpy(verts, positions.data(), positions.size() * sizeof(float));
        mesh.vertices = verts;

        auto *norms = new float[normals.size()];
        std::memcpy(norms, normals.data(), normals.size() * sizeof(float));
        mesh.normals = norms;

        auto *cols = new unsigned char[colors.size()];
        std::memcpy(cols, colors.data(), colors.size() * sizeof(unsigned char));
        mesh.colors = cols;
    }
    return mesh;
}

}  // namespace

gfx::Model LoadVoxModel(const char *file_name) {
    std::ifstream file(file_name, std::ios::binary);
    if (!file.is_open()) {
        std::fprintf(stderr, "gfx native: VOX import: couldn't open '%s'\n", file_name);
        return gfx::Model{};
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (buf.size() < 8 || std::memcmp(buf.data(), "VOX ", 4) != 0) {
        std::fprintf(stderr, "gfx native: VOX import: '%s' isn't a valid VOX file\n", file_name);
        return gfx::Model{};
    }
    // buf[4..8) is the format version (int32) -- every version to date
    // uses the same chunk framing read below, so it isn't checked.

    constexpr size_t kMainOffset = 8;
    if (kMainOffset + 12 > buf.size() || std::memcmp(buf.data() + kMainOffset, "MAIN", 4) != 0) {
        std::fprintf(stderr, "gfx native: VOX import: '%s' has no MAIN chunk\n", file_name);
        return gfx::Model{};
    }
    uint32_t main_content_size = ReadU32(buf, kMainOffset + 4);
    uint32_t main_children_size = ReadU32(buf, kMainOffset + 8);
    size_t children_start = kMainOffset + 12 + main_content_size;
    size_t children_end = children_start + main_children_size;
    if (children_end > buf.size()) children_end = buf.size();

    std::vector<VoxModel> vox_models;
    RgbaColor palette[256];
    std::memcpy(palette, kDefaultPalette, sizeof(palette));

    // MAIN's children are a flat sibling sequence: PACK (model count,
    // unused -- vox_models grows with every SIZE seen regardless), then
    // a SIZE+XYZI pair per model, then an optional trailing RGBA. Newer
    // scene-graph chunks (nTRN/nGRP/nSHP/MATL/LAYR/rOBJ/IMAP/NOTE) can
    // also appear here; this importer has no use for that scene/material
    // metadata (geometry-only scope, matching the OBJ/IQM importers) and
    // skips any chunk it doesn't recognize by its own declared
    // content+children size -- no need to recurse into it.
    size_t pos = children_start;
    VoxModel *pending = nullptr;  // SIZE seen, waiting for its XYZI
    while (pos + 12 <= children_end) {
        char id[5] = {0, 0, 0, 0, 0};
        std::memcpy(id, buf.data() + pos, 4);
        uint32_t content_size = ReadU32(buf, pos + 4);
        uint32_t chunk_children_size = ReadU32(buf, pos + 8);
        size_t content_start = pos + 12;
        if (content_start + content_size > buf.size()) break;

        if (std::strcmp(id, "SIZE") == 0 && content_size >= 12) {
            vox_models.emplace_back();
            pending = &vox_models.back();
        } else if (std::strcmp(id, "XYZI") == 0 && content_size >= 4 && pending != nullptr) {
            uint32_t num_voxels = ReadU32(buf, content_start);
            size_t vpos = content_start + 4;
            pending->voxels.reserve(num_voxels);
            for (uint32_t i = 0; i < num_voxels && vpos + 4 <= content_start + content_size; i++, vpos += 4) {
                Voxel v;
                v.x = static_cast<uint8_t>(buf[vpos]);
                v.y = static_cast<uint8_t>(buf[vpos + 1]);
                v.z = static_cast<uint8_t>(buf[vpos + 2]);
                v.color_index = static_cast<uint8_t>(buf[vpos + 3]);
                pending->voxels.push_back(v);
            }
            pending = nullptr;
        } else if (std::strcmp(id, "RGBA") == 0 && content_size >= 1024) {
            for (int i = 0; i < 256; i++) {
                size_t p = content_start + static_cast<size_t>(i) * 4;
                palette[i] = RgbaColor{static_cast<uint8_t>(buf[p]), static_cast<uint8_t>(buf[p + 1]),
                                        static_cast<uint8_t>(buf[p + 2]), static_cast<uint8_t>(buf[p + 3])};
            }
        }
        pos = content_start + content_size + chunk_children_size;
    }

    if (vox_models.empty()) {
        std::fprintf(stderr, "gfx native: VOX import: '%s' has no SIZE/XYZI model data\n", file_name);
        return gfx::Model{};
    }

    auto *meshes = new gfx::Mesh[vox_models.size()];
    int mesh_count = 0;
    for (const VoxModel &vm : vox_models) {
        gfx::Mesh mesh = BuildVoxMesh(vm, palette);
        if (mesh.vertexCount == 0) continue;  // empty model (all voxels culled or none present)
        meshes[mesh_count++] = mesh;
    }
    if (mesh_count == 0) {
        delete[] meshes;
        std::fprintf(stderr, "gfx native: VOX import: '%s' has no visible geometry\n", file_name);
        return gfx::Model{};
    }

    gfx::Model model{};
    model.transform = gfx::MatrixIdentity();
    model.meshCount = mesh_count;
    model.meshes = meshes;
    return model;
}

}  // namespace gfx
