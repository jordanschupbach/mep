#include "model3d_doc.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <utility>

#include "json.h"
#include "raylib.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Composite XYZ (intrinsic: rotate about X, then Y, then Z) Euler-degrees to
// quaternion, for glTF's node.rotation field (glTF has no Euler-angle node
// representation, only quaternions). Verified against each principal axis
// in isolation (e.g. deg = {90,0,0} -> (sin45,0,0,cos45), the expected
// quaternion for a 90-degree rotation about X).
void EulerXYZDegToQuat(const Vec3f &deg, float q[4]) {
    float rx = deg.x * (kPi / 180.0f) * 0.5f;
    float ry = deg.y * (kPi / 180.0f) * 0.5f;
    float rz = deg.z * (kPi / 180.0f) * 0.5f;
    float cx = cosf(rx);
    float sx = sinf(rx);
    float cy = cosf(ry);
    float sy = sinf(ry);
    float cz = cosf(rz);
    float sz = sinf(rz);
    q[0] = sx * cy * cz - cx * sy * sz;
    q[1] = cx * sy * cz + sx * cy * sz;
    q[2] = cx * cy * sz - sx * sy * cz;
    q[3] = cx * cy * cz + sx * sy * sz;
}

std::string Base64Encode(const unsigned char *data, size_t len) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        unsigned int n = (static_cast<unsigned int>(data[i]) << 16) | (static_cast<unsigned int>(data[i + 1]) << 8) |
                         static_cast<unsigned int>(data[i + 2]);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
    }
    size_t rem = len - i;
    if (rem == 1) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        unsigned int n = (static_cast<unsigned int>(data[i]) << 16) | (static_cast<unsigned int>(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

void AppendBytes(std::vector<unsigned char> &buf, const void *data, size_t len) {
    if (len == 0) return;
    const unsigned char *p = static_cast<const unsigned char *>(data);
    buf.insert(buf.end(), p, p + len);
}

// Copies a raylib Mesh's CPU-side vertex arrays into a plain MeshData --
// the one place this module reaches into raylib's types, immediately
// converted away so nothing else in this file (or its callers) needs to
// touch a raylib Mesh/Model handle.
void ExtractMeshFromRaylib(const Mesh &m, MeshData *out) {
    out->positions.assign(m.vertices, m.vertices + static_cast<size_t>(m.vertexCount) * 3);
    if (m.normals) out->normals.assign(m.normals, m.normals + static_cast<size_t>(m.vertexCount) * 3);
    if (m.texcoords) out->texcoords.assign(m.texcoords, m.texcoords + static_cast<size_t>(m.vertexCount) * 2);
    if (m.indices) {
        out->indices.reserve(static_cast<size_t>(m.triangleCount) * 3);
        for (int k = 0; k < m.triangleCount * 3; k++) out->indices.push_back(m.indices[k]);
    } else {
        // No index buffer -- raylib guarantees the vertex stream is already
        // triangle-list order in this case, so 0..vertexCount-1 is correct.
        out->indices.resize(static_cast<size_t>(m.vertexCount));
        for (int k = 0; k < m.vertexCount; k++) out->indices[static_cast<size_t>(k)] = static_cast<unsigned int>(k);
    }
}

// A right-triangular prism ("wedge"/ramp/doorstop shape) -- raylib has no
// GenMeshWedge, so this is a hand-built mesh in the exact style of raylib's
// own GenMeshCube (rmodels.c): RL_MALLOC'd flat, non-indexed triangle-soup
// arrays (one vertex per triangle-corner, so each triangle can carry its own
// flat face normal), then UploadMesh so it can go through the same
// ExtractMeshFromRaylib + UnloadMesh path every other primitive uses. Added
// after dogfooding a rocket build from primitives found flattened, rotated
// cubes an awkward stand-in for flat panel/fin shapes (MODEL3D_PLAN.md Part
// VIII/MODEL3D.md Phase 1.5's "no wedge primitive" gap).
//
// Base-pivoted like Cylinder/Cone (footprint centered in XZ at Y=0,
// extending up to Y=height), not centered like Cube/Sphere/Torus -- see
// MEP_AGENT_API.md's primitive pivot table. The vertical face sits at
// Z=-length/2 (full `height` tall); the mesh ramps down to Y=0 at
// Z=+length/2, so "forward" (+Z) is the downhill direction.
Mesh GenMeshWedge(float width, float height, float length) {
    Mesh mesh{};
    float hw = width * 0.5f;
    float hl = length * 0.5f;

    // Bottom rectangle (Y=0).
    Vector3 a{-hw, 0.0f, -hl};
    Vector3 b{hw, 0.0f, -hl};
    Vector3 c{hw, 0.0f, hl};
    Vector3 d{-hw, 0.0f, hl};
    // Top ridge (Y=height), running along X at the front (Z=-hl) edge.
    Vector3 e{-hw, height, -hl};
    Vector3 f{hw, height, -hl};

    // Each entry is one triangle's 3 corners, wound so cross(v1-v0, v2-v0)
    // points outward (worked out by hand per face against each vertex's
    // known position -- e.g. the bottom face's cross((b-a),(c-a)) reduces to
    // (0, -width*length, 0), i.e. straight down, confirming (a,b,c)/(a,c,d)
    // is the correct winding for that face's downward-facing normal).
    struct Tri {
        Vector3 v0, v1, v2;
        Vector3 n;
    };
    const Tri tris[8] = {
        {a, b, c, {0, -1, 0}},        // bottom
        {a, c, d, {0, -1, 0}},        // bottom
        {a, e, f, {0, 0, -1}},        // front (vertical) wall
        {a, f, b, {0, 0, -1}},        // front (vertical) wall
        {e, d, c, {0, 0, 0}},         // ramp (normal computed below)
        {e, c, f, {0, 0, 0}},         // ramp (normal computed below)
        {a, d, e, {-1, 0, 0}},        // left end cap
        {b, f, c, {1, 0, 0}},         // right end cap
    };
    // Ramp outward normal: proportional to (0, length, height) -- tilts up
    // and toward +Z (the downhill direction), derived from the same
    // cross-product-of-edges test as every other face here.
    float ramp_len = std::sqrt(length * length + height * height);
    Vector3 ramp_n = ramp_len > 1e-6f ? Vector3{0.0f, length / ramp_len, height / ramp_len} : Vector3{0.0f, 1.0f, 0.0f};

    const int kVertexCount = 24;  // 8 triangles * 3 corners, non-indexed
    mesh.vertices = static_cast<float *>(RL_MALLOC(static_cast<size_t>(kVertexCount) * 3 * sizeof(float)));
    mesh.normals = static_cast<float *>(RL_MALLOC(static_cast<size_t>(kVertexCount) * 3 * sizeof(float)));
    mesh.texcoords = static_cast<float *>(RL_MALLOC(static_cast<size_t>(kVertexCount) * 2 * sizeof(float)));
    int vi = 0;
    for (int t = 0; t < 8; t++) {
        Vector3 n = (t == 4 || t == 5) ? ramp_n : tris[t].n;
        const Vector3 corners[3] = {tris[t].v0, tris[t].v1, tris[t].v2};
        const float uvs[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
        for (int k = 0; k < 3; k++) {
            mesh.vertices[vi * 3 + 0] = corners[k].x;
            mesh.vertices[vi * 3 + 1] = corners[k].y;
            mesh.vertices[vi * 3 + 2] = corners[k].z;
            mesh.normals[vi * 3 + 0] = n.x;
            mesh.normals[vi * 3 + 1] = n.y;
            mesh.normals[vi * 3 + 2] = n.z;
            mesh.texcoords[vi * 2 + 0] = uvs[k][0];
            mesh.texcoords[vi * 2 + 1] = uvs[k][1];
            vi++;
        }
    }
    mesh.vertexCount = kVertexCount;
    mesh.triangleCount = 8;
    UploadMesh(&mesh, false);
    return mesh;
}

// Copies a raylib Image's CPU-side pixel data into a plain TextureData,
// normalizing to RGBA8 first -- the one place both texture-loading entry
// points (LoadTextureIntoScene's file decode, LoadModel3DFile's GPU
// texture readback) converge, so both fill out a TextureData the exact
// same way. Does not call UnloadImage -- that's the caller's own
// decode-vs-readback-specific cleanup.
void ExtractImageToTextureData(Image img, const std::string &name, TextureData *out) {
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    out->name = name;
    out->width = img.width;
    out->height = img.height;
    size_t byte_count = static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4;
    const unsigned char *bytes = static_cast<const unsigned char *>(img.data);
    out->pixels.assign(bytes, bytes + byte_count);
}

// Appends one (bufferView + accessor) pair describing `floats` (assumed
// tightly packed, `components`-per-element) to the growing glTF document,
// returning the new accessor's index, or -1 if `floats` is empty.
int AppendFloatAccessor(std::vector<unsigned char> &buffer, Json &buffer_views, Json &accessors, const std::vector<float> &floats,
                         int components, bool with_min_max) {
    if (floats.empty()) return -1;
    int count = static_cast<int>(floats.size()) / components;
    size_t offset = buffer.size();
    AppendBytes(buffer, floats.data(), floats.size() * sizeof(float));

    Json bv = Json::Object();
    bv["buffer"] = 0;
    bv["byteOffset"] = static_cast<int>(offset);
    bv["byteLength"] = static_cast<int>(floats.size() * sizeof(float));
    bv["target"] = 34962;  // ARRAY_BUFFER
    int bv_index = static_cast<int>(buffer_views.size());
    buffer_views.push_back(bv);

    Json acc = Json::Object();
    acc["bufferView"] = bv_index;
    acc["componentType"] = 5126;  // FLOAT
    acc["count"] = count;
    acc["type"] = components == 3 ? "VEC3" : "VEC2";
    if (with_min_max && components == 3) {
        float lo[3] = {floats[0], floats[1], floats[2]};
        float hi[3] = {floats[0], floats[1], floats[2]};
        for (int v = 0; v < count; v++) {
            for (int c = 0; c < 3; c++) {
                float val = floats[static_cast<size_t>(v) * 3 + static_cast<size_t>(c)];
                lo[c] = std::min(lo[c], val);
                hi[c] = std::max(hi[c], val);
            }
        }
        Json minj = Json::Array();
        minj.push_back(lo[0]);
        minj.push_back(lo[1]);
        minj.push_back(lo[2]);
        Json maxj = Json::Array();
        maxj.push_back(hi[0]);
        maxj.push_back(hi[1]);
        maxj.push_back(hi[2]);
        acc["min"] = minj;
        acc["max"] = maxj;
    }
    int acc_index = static_cast<int>(accessors.size());
    accessors.push_back(acc);
    return acc_index;
}

int AppendIndexAccessor(std::vector<unsigned char> &buffer, Json &buffer_views, Json &accessors, const std::vector<unsigned int> &indices) {
    if (indices.empty()) return -1;
    size_t offset = buffer.size();
    AppendBytes(buffer, indices.data(), indices.size() * sizeof(unsigned int));

    Json bv = Json::Object();
    bv["buffer"] = 0;
    bv["byteOffset"] = static_cast<int>(offset);
    bv["byteLength"] = static_cast<int>(indices.size() * sizeof(unsigned int));
    bv["target"] = 34963;  // ELEMENT_ARRAY_BUFFER
    int bv_index = static_cast<int>(buffer_views.size());
    buffer_views.push_back(bv);

    Json acc = Json::Object();
    acc["bufferView"] = bv_index;
    acc["componentType"] = 5125;  // UNSIGNED_INT
    acc["count"] = static_cast<int>(indices.size());
    acc["type"] = "SCALAR";
    int acc_index = static_cast<int>(accessors.size());
    accessors.push_back(acc);
    return acc_index;
}

}  // namespace

int Scene::AddObject(Object3D obj) {
    obj.id = next_object_id++;
    objects.push_back(std::move(obj));
    return objects.back().id;
}

Object3D *Scene::FindObject(int id) {
    for (auto &o : objects) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

const Object3D *Scene::FindObject(int id) const {
    for (const auto &o : objects) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

bool Scene::RemoveObject(int id) {
    auto it = std::find_if(objects.begin(), objects.end(), [id](const Object3D &o) { return o.id == id; });
    if (it == objects.end()) return false;
    objects.erase(it);
    return true;
}

std::vector<int> Scene::Descendants(int object_id) const {
    std::vector<int> result;
    std::vector<int> frontier{object_id};
    std::vector<int> visited{object_id};
    while (!frontier.empty()) {
        std::vector<int> next_frontier;
        for (const auto &obj : objects) {
            if (std::find(frontier.begin(), frontier.end(), obj.parent) == frontier.end()) continue;
            if (std::find(visited.begin(), visited.end(), obj.id) != visited.end()) continue;
            result.push_back(obj.id);
            visited.push_back(obj.id);
            next_frontier.push_back(obj.id);
        }
        frontier = std::move(next_frontier);
    }
    return result;
}

void MeshData::RemoveVertices(const std::vector<int> &vertex_indices) {
    int old_count = VertexCount();
    std::vector<bool> remove(static_cast<size_t>(old_count), false);
    for (int v : vertex_indices) {
        if (v >= 0 && v < old_count) remove[static_cast<size_t>(v)] = true;
    }

    // Old-index -> new-index remap: -1 for a removed vertex, else its
    // position in the compacted (kept-only) array.
    std::vector<int> remap(static_cast<size_t>(old_count), -1);
    int next_new_index = 0;
    for (int v = 0; v < old_count; v++) {
        if (!remove[static_cast<size_t>(v)]) remap[static_cast<size_t>(v)] = next_new_index++;
    }

    bool has_normals = !normals.empty();
    bool has_texcoords = !texcoords.empty();
    std::vector<float> new_positions, new_normals, new_texcoords;
    new_positions.reserve(static_cast<size_t>(next_new_index) * 3);
    if (has_normals) new_normals.reserve(static_cast<size_t>(next_new_index) * 3);
    if (has_texcoords) new_texcoords.reserve(static_cast<size_t>(next_new_index) * 2);
    for (int v = 0; v < old_count; v++) {
        if (remove[static_cast<size_t>(v)]) continue;
        new_positions.insert(new_positions.end(), positions.begin() + v * 3, positions.begin() + v * 3 + 3);
        if (has_normals) new_normals.insert(new_normals.end(), normals.begin() + v * 3, normals.begin() + v * 3 + 3);
        if (has_texcoords) new_texcoords.insert(new_texcoords.end(), texcoords.begin() + v * 2, texcoords.begin() + v * 2 + 2);
    }

    std::vector<unsigned int> new_indices;
    new_indices.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) >= old_count || static_cast<int>(b) >= old_count || static_cast<int>(c) >= old_count) {
            continue;  // defensive -- a malformed/out-of-range triangle can't be remapped, drop it
        }
        if (remove[a] || remove[b] || remove[c]) continue;  // the whole triangle goes if any corner did
        new_indices.push_back(static_cast<unsigned int>(remap[a]));
        new_indices.push_back(static_cast<unsigned int>(remap[b]));
        new_indices.push_back(static_cast<unsigned int>(remap[c]));
    }

    positions = std::move(new_positions);
    normals = std::move(new_normals);
    texcoords = std::move(new_texcoords);
    indices = std::move(new_indices);
}

void MeshData::MergeVertices(const std::vector<int> &vertex_indices) {
    int old_count = VertexCount();
    std::vector<bool> in_group(static_cast<size_t>(old_count), false);
    std::vector<int> group;
    for (int v : vertex_indices) {
        if (v >= 0 && v < old_count && !in_group[static_cast<size_t>(v)]) {
            in_group[static_cast<size_t>(v)] = true;
            group.push_back(v);
        }
    }
    if (group.size() < 2) return;  // nothing to merge
    std::sort(group.begin(), group.end());
    int keep_index = group.front();

    bool has_normals = !normals.empty();
    bool has_texcoords = !texcoords.empty();

    // Average the group's attributes into the kept vertex's own slot.
    float sum_pos[3] = {0.0f, 0.0f, 0.0f};
    float sum_norm[3] = {0.0f, 0.0f, 0.0f};
    float sum_uv[2] = {0.0f, 0.0f};
    for (int v : group) {
        for (int i = 0; i < 3; i++) sum_pos[i] += positions[static_cast<size_t>(v) * 3 + static_cast<size_t>(i)];
        if (has_normals) {
            for (int i = 0; i < 3; i++) sum_norm[i] += normals[static_cast<size_t>(v) * 3 + static_cast<size_t>(i)];
        }
        if (has_texcoords) {
            for (int i = 0; i < 2; i++) sum_uv[i] += texcoords[static_cast<size_t>(v) * 2 + static_cast<size_t>(i)];
        }
    }
    float n = static_cast<float>(group.size());
    for (int i = 0; i < 3; i++) positions[static_cast<size_t>(keep_index) * 3 + static_cast<size_t>(i)] = sum_pos[i] / n;
    if (has_normals) {
        for (int i = 0; i < 3; i++) normals[static_cast<size_t>(keep_index) * 3 + static_cast<size_t>(i)] = sum_norm[i] / n;
    }
    if (has_texcoords) {
        for (int i = 0; i < 2; i++) texcoords[static_cast<size_t>(keep_index) * 2 + static_cast<size_t>(i)] = sum_uv[i] / n;
    }

    // Every merged-away vertex now points at keep_index; everything else maps to itself.
    std::vector<int> merge_target(static_cast<size_t>(old_count));
    for (int v = 0; v < old_count; v++) merge_target[static_cast<size_t>(v)] = v;
    for (int v : group) merge_target[static_cast<size_t>(v)] = keep_index;

    // Remap triangles and drop any that became degenerate (two or more corners now equal).
    std::vector<unsigned int> remapped_indices;
    remapped_indices.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) >= old_count || static_cast<int>(b) >= old_count || static_cast<int>(c) >= old_count) {
            continue;  // defensive -- a malformed/out-of-range triangle can't be remapped, drop it
        }
        unsigned int ra = static_cast<unsigned int>(merge_target[a]);
        unsigned int rb = static_cast<unsigned int>(merge_target[b]);
        unsigned int rc = static_cast<unsigned int>(merge_target[c]);
        if (ra == rb || rb == rc || ra == rc) continue;  // degenerate after the weld -- drop it
        remapped_indices.push_back(ra);
        remapped_indices.push_back(rb);
        remapped_indices.push_back(rc);
    }

    // Compact away the merged-away vertices (everything in `group` except keep_index) --
    // same shape as RemoveVertices' own compaction step.
    std::vector<bool> remove(static_cast<size_t>(old_count), false);
    for (int v : group) {
        if (v != keep_index) remove[static_cast<size_t>(v)] = true;
    }
    std::vector<int> remap(static_cast<size_t>(old_count), -1);
    int next_new_index = 0;
    for (int v = 0; v < old_count; v++) {
        if (!remove[static_cast<size_t>(v)]) remap[static_cast<size_t>(v)] = next_new_index++;
    }

    std::vector<float> new_positions, new_normals, new_texcoords;
    new_positions.reserve(static_cast<size_t>(next_new_index) * 3);
    if (has_normals) new_normals.reserve(static_cast<size_t>(next_new_index) * 3);
    if (has_texcoords) new_texcoords.reserve(static_cast<size_t>(next_new_index) * 2);
    for (int v = 0; v < old_count; v++) {
        if (remove[static_cast<size_t>(v)]) continue;
        new_positions.insert(new_positions.end(), positions.begin() + v * 3, positions.begin() + v * 3 + 3);
        if (has_normals) new_normals.insert(new_normals.end(), normals.begin() + v * 3, normals.begin() + v * 3 + 3);
        if (has_texcoords) new_texcoords.insert(new_texcoords.end(), texcoords.begin() + v * 2, texcoords.begin() + v * 2 + 2);
    }

    std::vector<unsigned int> new_indices;
    new_indices.reserve(remapped_indices.size());
    for (unsigned int idx : remapped_indices) new_indices.push_back(static_cast<unsigned int>(remap[idx]));

    positions = std::move(new_positions);
    normals = std::move(new_normals);
    texcoords = std::move(new_texcoords);
    indices = std::move(new_indices);
}

void MeshData::RecalculateNormals() {
    int vertex_count = VertexCount();
    if (vertex_count == 0 || indices.empty()) return;

    normals.assign(static_cast<size_t>(vertex_count) * 3, 0.0f);
    auto accumulate = [&](unsigned int idx, float nx, float ny, float nz) {
        size_t base = static_cast<size_t>(idx) * 3;
        normals[base + 0] += nx;
        normals[base + 1] += ny;
        normals[base + 2] += nz;
    };
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int ia = indices[t], ib = indices[t + 1], ic = indices[t + 2];
        if (static_cast<int>(ia) >= vertex_count || static_cast<int>(ib) >= vertex_count ||
            static_cast<int>(ic) >= vertex_count) {
            continue;  // defensive -- a malformed/out-of-range triangle contributes nothing
        }
        float ax = positions[ia * 3 + 0], ay = positions[ia * 3 + 1], az = positions[ia * 3 + 2];
        float bx = positions[ib * 3 + 0], by = positions[ib * 3 + 1], bz = positions[ib * 3 + 2];
        float cx = positions[ic * 3 + 0], cy = positions[ic * 3 + 1], cz = positions[ic * 3 + 2];
        float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
        float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        // Unnormalized cross product -- its magnitude is proportional to
        // the triangle's area, so summing it directly (rather than
        // normalizing per-face first) gives an area-weighted average for
        // free.
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        accumulate(ia, nx, ny, nz);
        accumulate(ib, nx, ny, nz);
        accumulate(ic, nx, ny, nz);
    }

    for (int v = 0; v < vertex_count; v++) {
        size_t base = static_cast<size_t>(v) * 3;
        float x = normals[base + 0], y = normals[base + 1], z = normals[base + 2];
        float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1e-8f) {
            normals[base + 0] = x / len;
            normals[base + 1] = y / len;
            normals[base + 2] = z / len;
        } else {
            // Referenced by zero (or only degenerate) triangles -- nothing
            // to average, fall back to a fixed up-normal rather than NaN.
            normals[base + 0] = 0.0f;
            normals[base + 1] = 0.0f;
            normals[base + 2] = 1.0f;
        }
    }
}

std::vector<int> MeshData::SubdivideFaces(const std::vector<int> &vertex_indices) {
    int vertex_count = VertexCount();
    std::vector<bool> selected(static_cast<size_t>(vertex_count), false);
    for (int v : vertex_indices) {
        if (v >= 0 && v < vertex_count) selected[static_cast<size_t>(v)] = true;
    }

    bool has_normals = !normals.empty();
    bool has_texcoords = !texcoords.empty();
    std::vector<int> new_centroids;
    std::vector<unsigned int> new_indices;
    new_indices.reserve(indices.size());
    size_t original_triangle_count = indices.size() / 3;
    for (size_t tri = 0; tri < original_triangle_count; tri++) {
        size_t t = tri * 3;
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        bool full = static_cast<int>(a) < vertex_count && static_cast<int>(b) < vertex_count &&
                    static_cast<int>(c) < vertex_count && selected[a] && selected[b] && selected[c];
        if (!full) {
            new_indices.push_back(a);
            new_indices.push_back(b);
            new_indices.push_back(c);
            continue;
        }
        int centroid_index = VertexCount();
        positions.push_back((positions[a * 3 + 0] + positions[b * 3 + 0] + positions[c * 3 + 0]) / 3.0f);
        positions.push_back((positions[a * 3 + 1] + positions[b * 3 + 1] + positions[c * 3 + 1]) / 3.0f);
        positions.push_back((positions[a * 3 + 2] + positions[b * 3 + 2] + positions[c * 3 + 2]) / 3.0f);
        if (has_normals) {
            normals.push_back((normals[a * 3 + 0] + normals[b * 3 + 0] + normals[c * 3 + 0]) / 3.0f);
            normals.push_back((normals[a * 3 + 1] + normals[b * 3 + 1] + normals[c * 3 + 1]) / 3.0f);
            normals.push_back((normals[a * 3 + 2] + normals[b * 3 + 2] + normals[c * 3 + 2]) / 3.0f);
        }
        if (has_texcoords) {
            texcoords.push_back((texcoords[a * 2 + 0] + texcoords[b * 2 + 0] + texcoords[c * 2 + 0]) / 3.0f);
            texcoords.push_back((texcoords[a * 2 + 1] + texcoords[b * 2 + 1] + texcoords[c * 2 + 1]) / 3.0f);
        }
        new_centroids.push_back(centroid_index);
        unsigned int centroid = static_cast<unsigned int>(centroid_index);
        new_indices.push_back(a);
        new_indices.push_back(b);
        new_indices.push_back(centroid);
        new_indices.push_back(b);
        new_indices.push_back(c);
        new_indices.push_back(centroid);
        new_indices.push_back(c);
        new_indices.push_back(a);
        new_indices.push_back(centroid);
    }
    indices = std::move(new_indices);
    return new_centroids;
}

std::vector<int> MeshData::ExtrudeFaces(const std::vector<int> &vertex_indices, float distance) {
    int vertex_count = VertexCount();
    std::vector<bool> selected(static_cast<size_t>(vertex_count), false);
    for (int v : vertex_indices) {
        if (v >= 0 && v < vertex_count) selected[static_cast<size_t>(v)] = true;
    }

    struct Face {
        unsigned int a, b, c;
    };
    std::vector<Face> faces;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) < vertex_count && static_cast<int>(b) < vertex_count &&
            static_cast<int>(c) < vertex_count && selected[a] && selected[b] && selected[c]) {
            faces.push_back({a, b, c});
        }
    }
    if (faces.empty()) return {};

    // Area-weighted average of every selected triangle's own (unnormalized)
    // cross-product normal -- the geometric extrude direction, deliberately
    // not read from `normals` (which may be stale or absent).
    float sum[3] = {0.0f, 0.0f, 0.0f};
    for (const Face &f : faces) {
        float ax = positions[f.a * 3 + 0], ay = positions[f.a * 3 + 1], az = positions[f.a * 3 + 2];
        float bx = positions[f.b * 3 + 0], by = positions[f.b * 3 + 1], bz = positions[f.b * 3 + 2];
        float cx = positions[f.c * 3 + 0], cy = positions[f.c * 3 + 1], cz = positions[f.c * 3 + 2];
        float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
        float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        sum[0] += e1y * e2z - e1z * e2y;
        sum[1] += e1z * e2x - e1x * e2z;
        sum[2] += e1x * e2y - e1y * e2x;
    }
    float len = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (len < 1e-8f) return {};  // degenerate (zero-area) selection -- no sane direction to extrude along
    float dirx = sum[0] / len, diry = sum[1] / len, dirz = sum[2] / len;

    std::vector<bool> used(static_cast<size_t>(vertex_count), false);
    for (const Face &f : faces) {
        used[f.a] = true;
        used[f.b] = true;
        used[f.c] = true;
    }

    bool has_normals = !normals.empty();
    bool has_texcoords = !texcoords.empty();
    std::vector<int> dup(static_cast<size_t>(vertex_count), -1);
    std::vector<int> new_cap;
    for (int v = 0; v < vertex_count; v++) {
        if (!used[static_cast<size_t>(v)]) continue;
        int new_idx = VertexCount();
        dup[static_cast<size_t>(v)] = new_idx;
        new_cap.push_back(new_idx);
        positions.push_back(positions[static_cast<size_t>(v) * 3 + 0] + dirx * distance);
        positions.push_back(positions[static_cast<size_t>(v) * 3 + 1] + diry * distance);
        positions.push_back(positions[static_cast<size_t>(v) * 3 + 2] + dirz * distance);
        if (has_normals) {
            normals.push_back(normals[static_cast<size_t>(v) * 3 + 0]);
            normals.push_back(normals[static_cast<size_t>(v) * 3 + 1]);
            normals.push_back(normals[static_cast<size_t>(v) * 3 + 2]);
        }
        if (has_texcoords) {
            texcoords.push_back(texcoords[static_cast<size_t>(v) * 2 + 0]);
            texcoords.push_back(texcoords[static_cast<size_t>(v) * 2 + 1]);
        }
    }

    // A directed edge (a,b) of a selected triangle is on the group's
    // *boundary* unless its reverse (b,a) is also a directed edge of
    // some (other) selected triangle -- true exactly for an edge shared
    // between two selected triangles (consistent winding walks it in
    // both directions), e.g. a face's own internal diagonal.
    std::set<std::pair<unsigned int, unsigned int>> directed_edges;
    for (const Face &f : faces) {
        directed_edges.insert({f.a, f.b});
        directed_edges.insert({f.b, f.c});
        directed_edges.insert({f.c, f.a});
    }

    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) < vertex_count && static_cast<int>(b) < vertex_count &&
            static_cast<int>(c) < vertex_count && selected[a] && selected[b] && selected[c]) {
            indices[t] = static_cast<unsigned int>(dup[a]);
            indices[t + 1] = static_cast<unsigned int>(dup[b]);
            indices[t + 2] = static_cast<unsigned int>(dup[c]);
        }
    }

    for (const auto &e : directed_edges) {
        unsigned int a = e.first, b = e.second;
        if (directed_edges.count({b, a}) != 0) continue;  // interior edge, shared by two selected triangles
        unsigned int a2 = static_cast<unsigned int>(dup[a]);
        unsigned int b2 = static_cast<unsigned int>(dup[b]);
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(b2);
        indices.push_back(a);
        indices.push_back(b2);
        indices.push_back(a2);
    }

    std::sort(new_cap.begin(), new_cap.end());
    return new_cap;
}

bool MeshData::DissolveVertex(int vertex_index) {
    int vertex_count = VertexCount();
    if (vertex_index < 0 || vertex_index >= vertex_count) return false;
    unsigned int v = static_cast<unsigned int>(vertex_index);

    // Every incident triangle's "opposite" directed edge, in the same
    // rotational order the triangle already had relative to v (so the
    // fan triangulation below preserves winding).
    std::vector<std::pair<unsigned int, unsigned int>> ring_edges;
    std::vector<size_t> incident_tri_starts;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int p[3] = {indices[t], indices[t + 1], indices[t + 2]};
        for (int i = 0; i < 3; i++) {
            if (p[i] == v) {
                ring_edges.push_back({p[(i + 1) % 3], p[(i + 2) % 3]});
                incident_tri_starts.push_back(t);
                break;
            }
        }
    }
    if (ring_edges.empty()) {
        RemoveVertices({vertex_index});  // isolated vertex -- nothing to retriangulate
        return true;
    }

    // Try to walk ring_edges into one closed loop -- a proper manifold
    // fan around v. Any failure (a dangling edge, a branch, more than one
    // loop, or too short a ring to fan-triangulate) means v isn't a clean
    // interior vertex, so fall back to a plain removal rather than risk a
    // malformed retriangulation.
    std::vector<unsigned int> ring;
    bool ok = true;
    {
        std::vector<std::pair<unsigned int, unsigned int>> remaining = ring_edges;
        ring.push_back(remaining.front().first);
        unsigned int cur = remaining.front().second;
        remaining.erase(remaining.begin());
        while (!remaining.empty()) {
            auto it = std::find_if(remaining.begin(), remaining.end(),
                                    [cur](const std::pair<unsigned int, unsigned int> &e) { return e.first == cur; });
            if (it == remaining.end()) {
                ok = false;
                break;
            }
            ring.push_back(cur);
            cur = it->second;
            remaining.erase(it);
        }
        if (!ok || cur != ring.front() || ring.size() != ring_edges.size() || ring.size() < 3) ok = false;
    }
    if (!ok) {
        RemoveVertices({vertex_index});
        return true;
    }

    for (auto rit = incident_tri_starts.rbegin(); rit != incident_tri_starts.rend(); ++rit) {
        indices.erase(indices.begin() + static_cast<long>(*rit), indices.begin() + static_cast<long>(*rit) + 3);
    }
    for (size_t i = 1; i + 1 < ring.size(); i++) {
        indices.push_back(ring[0]);
        indices.push_back(ring[i]);
        indices.push_back(ring[i + 1]);
    }

    RemoveVertices({vertex_index});
    return true;
}

std::vector<int> MeshData::InsetFaces(const std::vector<int> &vertex_indices, float amount) {
    amount = std::max(0.0f, std::min(1.0f, amount));
    int vertex_count = VertexCount();
    std::vector<bool> selected(static_cast<size_t>(vertex_count), false);
    for (int v : vertex_indices) {
        if (v >= 0 && v < vertex_count) selected[static_cast<size_t>(v)] = true;
    }

    struct Face {
        unsigned int a, b, c;
    };
    std::vector<Face> faces;
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) < vertex_count && static_cast<int>(b) < vertex_count &&
            static_cast<int>(c) < vertex_count && selected[a] && selected[b] && selected[c]) {
            faces.push_back({a, b, c});
        }
    }
    if (faces.empty()) return {};

    std::vector<bool> used(static_cast<size_t>(vertex_count), false);
    for (const Face &f : faces) {
        used[f.a] = true;
        used[f.b] = true;
        used[f.c] = true;
    }

    // The face group's own centroid -- the average position of every
    // vertex it uses -- is what everything insets toward.
    float centroid[3] = {0.0f, 0.0f, 0.0f};
    int used_count = 0;
    for (int v = 0; v < vertex_count; v++) {
        if (!used[static_cast<size_t>(v)]) continue;
        centroid[0] += positions[static_cast<size_t>(v) * 3 + 0];
        centroid[1] += positions[static_cast<size_t>(v) * 3 + 1];
        centroid[2] += positions[static_cast<size_t>(v) * 3 + 2];
        used_count++;
    }
    centroid[0] /= static_cast<float>(used_count);
    centroid[1] /= static_cast<float>(used_count);
    centroid[2] /= static_cast<float>(used_count);

    bool has_normals = !normals.empty();
    bool has_texcoords = !texcoords.empty();
    std::vector<int> dup(static_cast<size_t>(vertex_count), -1);
    std::vector<int> new_cap;
    for (int v = 0; v < vertex_count; v++) {
        if (!used[static_cast<size_t>(v)]) continue;
        int new_idx = VertexCount();
        dup[static_cast<size_t>(v)] = new_idx;
        new_cap.push_back(new_idx);
        float ox = positions[static_cast<size_t>(v) * 3 + 0];
        float oy = positions[static_cast<size_t>(v) * 3 + 1];
        float oz = positions[static_cast<size_t>(v) * 3 + 2];
        positions.push_back(ox + (centroid[0] - ox) * amount);
        positions.push_back(oy + (centroid[1] - oy) * amount);
        positions.push_back(oz + (centroid[2] - oz) * amount);
        if (has_normals) {
            normals.push_back(normals[static_cast<size_t>(v) * 3 + 0]);
            normals.push_back(normals[static_cast<size_t>(v) * 3 + 1]);
            normals.push_back(normals[static_cast<size_t>(v) * 3 + 2]);
        }
        if (has_texcoords) {
            texcoords.push_back(texcoords[static_cast<size_t>(v) * 2 + 0]);
            texcoords.push_back(texcoords[static_cast<size_t>(v) * 2 + 1]);
        }
    }

    // Same boundary-edge convention as ExtrudeFaces: a directed edge is
    // walled only if its reverse isn't also a directed edge of some
    // (other) selected triangle -- an edge shared between two selected
    // triangles (e.g. a face's own diagonal) is left alone.
    std::set<std::pair<unsigned int, unsigned int>> directed_edges;
    for (const Face &f : faces) {
        directed_edges.insert({f.a, f.b});
        directed_edges.insert({f.b, f.c});
        directed_edges.insert({f.c, f.a});
    }

    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) < vertex_count && static_cast<int>(b) < vertex_count &&
            static_cast<int>(c) < vertex_count && selected[a] && selected[b] && selected[c]) {
            indices[t] = static_cast<unsigned int>(dup[a]);
            indices[t + 1] = static_cast<unsigned int>(dup[b]);
            indices[t + 2] = static_cast<unsigned int>(dup[c]);
        }
    }

    for (const auto &e : directed_edges) {
        unsigned int a = e.first, b = e.second;
        if (directed_edges.count({b, a}) != 0) continue;  // interior edge, shared by two selected triangles
        unsigned int a2 = static_cast<unsigned int>(dup[a]);
        unsigned int b2 = static_cast<unsigned int>(dup[b]);
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(b2);
        indices.push_back(a);
        indices.push_back(b2);
        indices.push_back(a2);
    }

    std::sort(new_cap.begin(), new_cap.end());
    return new_cap;
}

int MeshData::AddVertex(float x, float y, float z) {
    int new_index = VertexCount();
    positions.push_back(x);
    positions.push_back(y);
    positions.push_back(z);
    if (!normals.empty()) {
        normals.push_back(0.0f);
        normals.push_back(0.0f);
        normals.push_back(1.0f);
    }
    if (!texcoords.empty()) {
        texcoords.push_back(0.0f);
        texcoords.push_back(0.0f);
    }
    return new_index;
}

bool MeshData::MakeFace(const std::vector<int> &vertex_indices) {
    int vertex_count = VertexCount();
    std::vector<bool> seen(static_cast<size_t>(vertex_count), false);
    std::vector<int> distinct;
    for (int v : vertex_indices) {
        if (v >= 0 && v < vertex_count && !seen[static_cast<size_t>(v)]) {
            seen[static_cast<size_t>(v)] = true;
            distinct.push_back(v);
        }
    }
    if (distinct.size() < 3) return false;

    unsigned int pivot = static_cast<unsigned int>(distinct[0]);
    for (size_t i = 1; i + 1 < distinct.size(); i++) {
        indices.push_back(pivot);
        indices.push_back(static_cast<unsigned int>(distinct[i]));
        indices.push_back(static_cast<unsigned int>(distinct[i + 1]));
    }
    return true;
}

int MeshData::MergeByDistance(float threshold) {
    threshold = std::max(threshold, 0.0f);
    int vertex_count = VertexCount();
    if (vertex_count < 2) return 0;

    // Union-find over every pair within threshold -- grouping is
    // transitive through a chain of close-enough pairs. No path
    // compression during the unions themselves (kept simple; mesh sizes
    // here are small enough that it doesn't matter), just a plain find()
    // used both while unioning and for the final grouping pass below.
    std::vector<int> parent(static_cast<size_t>(vertex_count));
    for (int i = 0; i < vertex_count; i++) parent[static_cast<size_t>(i)] = i;
    auto find_root = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) x = parent[static_cast<size_t>(x)];
        return x;
    };

    float threshold_sq = threshold * threshold;
    for (int i = 0; i < vertex_count; i++) {
        for (int j = i + 1; j < vertex_count; j++) {
            float dx = positions[static_cast<size_t>(i) * 3 + 0] - positions[static_cast<size_t>(j) * 3 + 0];
            float dy = positions[static_cast<size_t>(i) * 3 + 1] - positions[static_cast<size_t>(j) * 3 + 1];
            float dz = positions[static_cast<size_t>(i) * 3 + 2] - positions[static_cast<size_t>(j) * 3 + 2];
            if (dx * dx + dy * dy + dz * dz > threshold_sq) continue;
            int ri = find_root(i), rj = find_root(j);
            if (ri != rj) parent[static_cast<size_t>(std::max(ri, rj))] = std::min(ri, rj);
        }
    }

    std::vector<std::vector<int>> groups(static_cast<size_t>(vertex_count));
    for (int i = 0; i < vertex_count; i++) groups[static_cast<size_t>(find_root(i))].push_back(i);

    bool has_normals = !normals.empty();
    bool has_texcoords = !texcoords.empty();
    std::vector<int> merge_target(static_cast<size_t>(vertex_count));
    for (int i = 0; i < vertex_count; i++) merge_target[static_cast<size_t>(i)] = i;
    int removed_count = 0;
    for (int root = 0; root < vertex_count; root++) {
        const std::vector<int> &group = groups[static_cast<size_t>(root)];
        if (group.size() < 2) continue;
        float sum_pos[3] = {0.0f, 0.0f, 0.0f};
        float sum_norm[3] = {0.0f, 0.0f, 0.0f};
        float sum_uv[2] = {0.0f, 0.0f};
        for (int v : group) {
            for (int k = 0; k < 3; k++) sum_pos[k] += positions[static_cast<size_t>(v) * 3 + static_cast<size_t>(k)];
            if (has_normals) {
                for (int k = 0; k < 3; k++) sum_norm[k] += normals[static_cast<size_t>(v) * 3 + static_cast<size_t>(k)];
            }
            if (has_texcoords) {
                for (int k = 0; k < 2; k++) sum_uv[k] += texcoords[static_cast<size_t>(v) * 2 + static_cast<size_t>(k)];
            }
        }
        float n = static_cast<float>(group.size());
        for (int k = 0; k < 3; k++) positions[static_cast<size_t>(root) * 3 + static_cast<size_t>(k)] = sum_pos[k] / n;
        if (has_normals) {
            for (int k = 0; k < 3; k++) normals[static_cast<size_t>(root) * 3 + static_cast<size_t>(k)] = sum_norm[k] / n;
        }
        if (has_texcoords) {
            for (int k = 0; k < 2; k++) texcoords[static_cast<size_t>(root) * 2 + static_cast<size_t>(k)] = sum_uv[k] / n;
        }
        for (int v : group) {
            merge_target[static_cast<size_t>(v)] = root;
            if (v != root) removed_count++;
        }
    }
    if (removed_count == 0) return 0;

    std::vector<unsigned int> remapped_indices;
    remapped_indices.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        unsigned int a = indices[t], b = indices[t + 1], c = indices[t + 2];
        if (static_cast<int>(a) >= vertex_count || static_cast<int>(b) >= vertex_count ||
            static_cast<int>(c) >= vertex_count) {
            continue;
        }
        unsigned int ra = static_cast<unsigned int>(merge_target[a]);
        unsigned int rb = static_cast<unsigned int>(merge_target[b]);
        unsigned int rc = static_cast<unsigned int>(merge_target[c]);
        if (ra == rb || rb == rc || ra == rc) continue;  // degenerate after the weld -- drop it
        remapped_indices.push_back(ra);
        remapped_indices.push_back(rb);
        remapped_indices.push_back(rc);
    }

    std::vector<bool> remove(static_cast<size_t>(vertex_count), false);
    for (int v = 0; v < vertex_count; v++) {
        if (merge_target[static_cast<size_t>(v)] != v) remove[static_cast<size_t>(v)] = true;
    }
    std::vector<int> remap(static_cast<size_t>(vertex_count), -1);
    int next_new_index = 0;
    for (int v = 0; v < vertex_count; v++) {
        if (!remove[static_cast<size_t>(v)]) remap[static_cast<size_t>(v)] = next_new_index++;
    }

    std::vector<float> new_positions, new_normals, new_texcoords;
    new_positions.reserve(static_cast<size_t>(next_new_index) * 3);
    if (has_normals) new_normals.reserve(static_cast<size_t>(next_new_index) * 3);
    if (has_texcoords) new_texcoords.reserve(static_cast<size_t>(next_new_index) * 2);
    for (int v = 0; v < vertex_count; v++) {
        if (remove[static_cast<size_t>(v)]) continue;
        new_positions.insert(new_positions.end(), positions.begin() + v * 3, positions.begin() + v * 3 + 3);
        if (has_normals) new_normals.insert(new_normals.end(), normals.begin() + v * 3, normals.begin() + v * 3 + 3);
        if (has_texcoords) new_texcoords.insert(new_texcoords.end(), texcoords.begin() + v * 2, texcoords.begin() + v * 2 + 2);
    }

    std::vector<unsigned int> new_indices;
    new_indices.reserve(remapped_indices.size());
    for (unsigned int idx : remapped_indices) new_indices.push_back(static_cast<unsigned int>(remap[idx]));

    positions = std::move(new_positions);
    normals = std::move(new_normals);
    texcoords = std::move(new_texcoords);
    indices = std::move(new_indices);
    return removed_count;
}

void MeshData::FlipNormals() {
    for (size_t t = 0; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
    for (size_t i = 0; i + 2 < normals.size(); i += 3) {
        normals[i + 0] = -normals[i + 0];
        normals[i + 1] = -normals[i + 1];
        normals[i + 2] = -normals[i + 2];
    }
}

int Scene::EnsureUniqueMesh(int object_id) {
    Object3D *obj = FindObject(object_id);
    if (!obj || obj->mesh_index < 0 || obj->mesh_index >= static_cast<int>(meshes.size())) return -1;
    int share_count = 0;
    for (const Object3D &o : objects) {
        if (o.mesh_index == obj->mesh_index) share_count++;
    }
    if (share_count <= 1) return obj->mesh_index;  // already exclusively this object's own
    MeshData clone = meshes[static_cast<size_t>(obj->mesh_index)];
    int new_index = static_cast<int>(meshes.size());
    meshes.push_back(std::move(clone));
    obj->mesh_index = new_index;
    return new_index;
}

int Scene::TotalTriangleCount() const {
    int total = 0;
    for (const auto &obj : objects) {
        if (obj.mesh_index >= 0 && obj.mesh_index < static_cast<int>(meshes.size())) {
            total += meshes[static_cast<size_t>(obj.mesh_index)].TriangleCount();
        }
    }
    return total;
}

bool IsModel3DPath(const std::string &path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "obj" || ext == "gltf" || ext == "glb" || ext == "iqm" || ext == "vox" || ext == "m3d";
}

bool LoadModel3DFile(const std::string &path, Scene *out, std::string *error) {
    Model model = LoadModel(path.c_str());
    if (model.meshCount <= 0) {
        if (error) *error = "no meshes loaded (missing file, or unsupported/corrupt format): " + path;
        UnloadModel(model);
        return false;
    }

    size_t slash = path.find_last_of('/');
    std::string base_name = slash == std::string::npos ? path : path.substr(slash + 1);

    for (int i = 0; i < model.meshCount; i++) {
        MeshData md;
        md.name = model.meshCount > 1 ? (base_name + "#" + std::to_string(i)) : base_name;
        ExtractMeshFromRaylib(model.meshes[i], &md);
        int mesh_index = static_cast<int>(out->meshes.size());
        out->meshes.push_back(std::move(md));

        Object3D obj;
        obj.name = model.meshCount > 1 ? (base_name + " " + std::to_string(i + 1)) : base_name;
        obj.mesh_index = mesh_index;
        obj.kind = PrimitiveKind::Imported;
        // Base color, plus a base-color/albedo texture if the material has
        // one bound -- still no normal/metallic-roughness/emissive maps or
        // metallic/roughness scalars (see Object3D::texture_index's own
        // comment on why: raylib's default shader has no lighting model to
        // apply them to). model.meshMaterial[i] indexes model.materials;
        // LoadModel always populates both (falling back to a single
        // default material) so this is safe whenever raylib reported any
        // meshes at all.
        if (model.meshMaterial && model.materialCount > 0) {
            int mat_idx = model.meshMaterial[i];
            if (mat_idx >= 0 && mat_idx < model.materialCount) {
                Color c = model.materials[mat_idx].maps[MATERIAL_MAP_ALBEDO].color;
                obj.color = RgbaColorF{static_cast<float>(c.r) / 255.0f, static_cast<float>(c.g) / 255.0f,
                                        static_cast<float>(c.b) / 255.0f, static_cast<float>(c.a) / 255.0f};
                Texture2D tex = model.materials[mat_idx].maps[MATERIAL_MAP_ALBEDO].texture;
                // tex.id > 0 alone isn't "has a real texture" -- raylib's
                // own glTF loader (rmodels.c) calls LoadMaterialDefault()
                // for *every* material before applying overrides, which
                // binds the shared rlgl default texture (always exactly
                // 1x1) whether or not the glTF material actually specified
                // a baseColorTexture. Filtering on width/height > 1 instead
                // of comparing texture ids avoids needing rlgl.h just for
                // rlGetTextureIdDefault() -- the only false negative this
                // could produce is a genuine hand-authored 1x1 texture,
                // which would render identically to no texture at all
                // under this shader anyway (a uniform-color 1x1 sample is
                // just a flat tint).
                if (tex.id > 0 && tex.width > 1 && tex.height > 1) {
                    // Reads the texture back from the GPU (it's already
                    // resident there -- LoadModel uploaded it as part of
                    // loading the material) into a plain TextureData, so
                    // the doc layer keeps owning real pixel bytes instead
                    // of a raylib GPU handle, same as every other texture
                    // path here. Each object gets its own TextureData copy
                    // even if several objects in this file share one
                    // material/texture -- a little redundant for a
                    // multi-mesh file with a shared texture, but simple
                    // and still correct; not deduplicated this pass.
                    Image img = LoadImageFromTexture(tex);
                    if (img.data) {
                        TextureData td;
                        ExtractImageToTextureData(img, base_name + "_tex", &td);
                        UnloadImage(img);
                        obj.texture_index = static_cast<int>(out->textures.size());
                        out->textures.push_back(std::move(td));
                    }
                }
            }
        }
        out->AddObject(std::move(obj));
    }
    out->source_path = path;
    UnloadModel(model);
    return true;
}

int AddPrimitiveToScene(Scene *scene, PrimitiveKind kind) {
    Mesh m{};
    const char *name = "";
    switch (kind) {
        case PrimitiveKind::Cube:
            m = GenMeshCube(1.0f, 1.0f, 1.0f);
            name = "Cube";
            break;
        case PrimitiveKind::Sphere:
            m = GenMeshSphere(0.5f, 16, 16);
            name = "Sphere";
            break;
        case PrimitiveKind::Cylinder:
            m = GenMeshCylinder(0.5f, 1.0f, 16);
            name = "Cylinder";
            break;
        case PrimitiveKind::Cone:
            m = GenMeshCone(0.5f, 1.0f, 16);
            name = "Cone";
            break;
        case PrimitiveKind::Plane:
            m = GenMeshPlane(1.0f, 1.0f, 1, 1);
            name = "Plane";
            break;
        case PrimitiveKind::Torus:
            // Ring radius 0.35, tube diameter 0.15 (tube radius 0.075) --
            // leaves a real hole (inner edge at 0.35-0.075=0.275) instead
            // of the tube overlapping itself through the center. The
            // original defaults (radius 0.2, size 0.5) had a tube radius
            // *bigger* than the ring radius, which doesn't read as a ring
            // at all -- caught live building a test scene (MODEL3D_PLAN.md
            // Part VIII/IX) and documented as a known quirk in
            // MEP_AGENT_API.md before this fix landed.
            m = GenMeshTorus(0.35f, 0.15f, 16, 16);
            name = "Torus";
            break;
        case PrimitiveKind::Wedge:
            // Base-pivoted like Cylinder/Cone (unlike Cube/Sphere/Torus,
            // which are centered) -- see GenMeshWedge's own comment and
            // MEP_AGENT_API.md's pivot table.
            m = GenMeshWedge(1.0f, 1.0f, 1.0f);
            name = "Wedge";
            break;
        case PrimitiveKind::None:
        case PrimitiveKind::Imported:
        default:
            return -1;
    }
    MeshData md;
    md.name = name;
    ExtractMeshFromRaylib(m, &md);
    UnloadMesh(m);

    int mesh_index = static_cast<int>(scene->meshes.size());
    scene->meshes.push_back(std::move(md));

    Object3D obj;
    obj.name = name;
    obj.mesh_index = mesh_index;
    obj.kind = kind;
    return scene->AddObject(std::move(obj));
}

int LoadTextureIntoScene(Scene *scene, const std::string &path, std::string *error) {
    Image img = LoadImage(path.c_str());
    if (!img.data) {
        if (error) *error = "failed to load image (missing file, or unsupported/corrupt format): " + path;
        return -1;
    }
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    TextureData td;
    ExtractImageToTextureData(img, name, &td);
    UnloadImage(img);
    int index = static_cast<int>(scene->textures.size());
    scene->textures.push_back(std::move(td));
    return index;
}

bool DescribePrimitiveKind(PrimitiveKind kind, std::string *out_pivot, std::string *out_dimensions) {
    switch (kind) {
        case PrimitiveKind::Cube:
            *out_pivot = "centered";
            *out_dimensions = "1 x 1 x 1";
            return true;
        case PrimitiveKind::Sphere:
            *out_pivot = "centered";
            *out_dimensions = "radius 0.5";
            return true;
        case PrimitiveKind::Cylinder:
            *out_pivot = "base (extends +Y)";
            *out_dimensions = "radius 0.5, height 1";
            return true;
        case PrimitiveKind::Cone:
            *out_pivot = "base (extends +Y, apex at top)";
            *out_dimensions = "radius 0.5, height 1";
            return true;
        case PrimitiveKind::Plane:
            *out_pivot = "centered, lies flat in XZ";
            *out_dimensions = "1 x 1";
            return true;
        case PrimitiveKind::Torus:
            *out_pivot = "centered, lies flat in XZ (hole faces +Y)";
            *out_dimensions = "ring radius 0.35, tube diameter 0.15";
            return true;
        case PrimitiveKind::Wedge:
            *out_pivot = "base (extends +Y); vertical face at local Z=-0.5, ramps down to Y=0 at Z=+0.5";
            *out_dimensions = "width 1, height 1, length 1";
            return true;
        case PrimitiveKind::None:
        case PrimitiveKind::Imported:
        default:
            return false;
    }
}

bool SaveModel3DGltf(const Scene &scene, const std::string &path, std::string *error) {
    std::vector<unsigned char> buffer;
    Json accessors = Json::Array();
    Json buffer_views = Json::Array();
    Json meshes_json = Json::Array();
    Json materials_json = Json::Array();
    Json nodes_json = Json::Array();
    Json scene_node_indices = Json::Array();

    // One (bufferView + accessor) triple per Scene::meshes entry, shared by
    // every Object3D that references it -- but a *separate* glTF mesh +
    // material per Object3D below, so two objects sharing one MeshData can
    // still each keep their own base color.
    std::vector<int> position_accessor(scene.meshes.size(), -1);
    std::vector<int> normal_accessor(scene.meshes.size(), -1);
    std::vector<int> texcoord_accessor(scene.meshes.size(), -1);
    std::vector<int> index_accessor(scene.meshes.size(), -1);

    for (size_t mi = 0; mi < scene.meshes.size(); mi++) {
        const MeshData &md = scene.meshes[mi];
        if (md.VertexCount() <= 0) continue;
        position_accessor[mi] = AppendFloatAccessor(buffer, buffer_views, accessors, md.positions, 3, true);
        normal_accessor[mi] = AppendFloatAccessor(buffer, buffer_views, accessors, md.normals, 3, false);
        texcoord_accessor[mi] = AppendFloatAccessor(buffer, buffer_views, accessors, md.texcoords, 2, false);
        index_accessor[mi] = AppendIndexAccessor(buffer, buffer_views, accessors, md.indices);
    }

    // One glTF image+texture per Scene::textures entry (not per-object the
    // way meshes/materials are -- unlike base color, pixel data has no
    // reason to be duplicated in the export just because our own in-memory
    // model might hold more than one copy of visually-identical bytes).
    // Encoded to real PNG bytes via ExportImageToMemory (raylib's bundled
    // stb_image_write) rather than embedding raw RGBA8 -- glTF images must
    // be an actual encoded image format, not a raw pixel dump.
    Json images_json = Json::Array();
    Json textures_json = Json::Array();
    std::vector<int> texture_gltf_index(scene.textures.size(), -1);
    for (size_t ti = 0; ti < scene.textures.size(); ti++) {
        const TextureData &td = scene.textures[ti];
        if (td.width <= 0 || td.height <= 0 || td.pixels.size() < static_cast<size_t>(td.width) * static_cast<size_t>(td.height) * 4) {
            continue;
        }
        Image img{};
        img.data = const_cast<unsigned char *>(td.pixels.data());
        img.width = td.width;
        img.height = td.height;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        int png_size = 0;
        unsigned char *png_data = ExportImageToMemory(img, ".png", &png_size);
        if (!png_data || png_size <= 0) continue;
        Json image_entry = Json::Object();
        image_entry["uri"] = std::string("data:image/png;base64,") + Base64Encode(png_data, static_cast<size_t>(png_size));
        MemFree(png_data);
        int image_index = static_cast<int>(images_json.size());
        images_json.push_back(image_entry);
        Json texture_entry = Json::Object();
        texture_entry["source"] = image_index;
        texture_gltf_index[ti] = static_cast<int>(textures_json.size());
        textures_json.push_back(texture_entry);
    }

    for (const Object3D &obj : scene.objects) {
        Json node = Json::Object();
        node["name"] = obj.name;
        Json extras = Json::Object();
        extras["visible"] = obj.visible;
        node["extras"] = extras;

        Json trans = Json::Array();
        trans.push_back(obj.position.x);
        trans.push_back(obj.position.y);
        trans.push_back(obj.position.z);
        node["translation"] = trans;

        float q[4];
        EulerXYZDegToQuat(obj.rotation_deg, q);
        Json rot = Json::Array();
        rot.push_back(q[0]);
        rot.push_back(q[1]);
        rot.push_back(q[2]);
        rot.push_back(q[3]);
        node["rotation"] = rot;

        Json scl = Json::Array();
        scl.push_back(obj.scale.x);
        scl.push_back(obj.scale.y);
        scl.push_back(obj.scale.z);
        node["scale"] = scl;

        if (obj.mesh_index >= 0 && obj.mesh_index < static_cast<int>(scene.meshes.size()) &&
            position_accessor[static_cast<size_t>(obj.mesh_index)] >= 0) {
            size_t mi = static_cast<size_t>(obj.mesh_index);

            Json pbr = Json::Object();
            Json base_color = Json::Array();
            base_color.push_back(obj.color.r);
            base_color.push_back(obj.color.g);
            base_color.push_back(obj.color.b);
            base_color.push_back(obj.color.a);
            pbr["baseColorFactor"] = base_color;
            pbr["metallicFactor"] = 0.0;
            pbr["roughnessFactor"] = 0.8;
            if (obj.texture_index >= 0 && obj.texture_index < static_cast<int>(texture_gltf_index.size()) &&
                texture_gltf_index[static_cast<size_t>(obj.texture_index)] >= 0) {
                Json base_color_tex = Json::Object();
                base_color_tex["index"] = texture_gltf_index[static_cast<size_t>(obj.texture_index)];
                pbr["baseColorTexture"] = base_color_tex;
            }
            Json material = Json::Object();
            material["pbrMetallicRoughness"] = pbr;
            int material_index = static_cast<int>(materials_json.size());
            materials_json.push_back(material);

            Json attributes = Json::Object();
            attributes["POSITION"] = position_accessor[mi];
            if (normal_accessor[mi] >= 0) attributes["NORMAL"] = normal_accessor[mi];
            if (texcoord_accessor[mi] >= 0) attributes["TEXCOORD_0"] = texcoord_accessor[mi];

            Json primitive = Json::Object();
            primitive["attributes"] = attributes;
            primitive["indices"] = index_accessor[mi];
            primitive["material"] = material_index;
            Json primitives = Json::Array();
            primitives.push_back(primitive);
            Json mesh_json = Json::Object();
            mesh_json["primitives"] = primitives;
            int mesh_json_index = static_cast<int>(meshes_json.size());
            meshes_json.push_back(mesh_json);
            node["mesh"] = mesh_json_index;
        }

        int node_index = static_cast<int>(nodes_json.size());
        nodes_json.push_back(node);
        scene_node_indices.push_back(node_index);
    }

    Json doc = Json::Object();
    Json asset = Json::Object();
    asset["version"] = "2.0";
    asset["generator"] = "mep 3D modeler";
    doc["asset"] = asset;
    doc["scene"] = 0;

    Json scene0 = Json::Object();
    scene0["nodes"] = scene_node_indices;
    Json scenes_arr = Json::Array();
    scenes_arr.push_back(scene0);
    doc["scenes"] = scenes_arr;

    doc["nodes"] = nodes_json;
    doc["meshes"] = meshes_json;
    doc["materials"] = materials_json;
    if (!images_json.items().empty()) {
        doc["images"] = images_json;
        doc["textures"] = textures_json;
    }
    doc["accessors"] = accessors;
    doc["bufferViews"] = buffer_views;

    Json buf = Json::Object();
    buf["byteLength"] = static_cast<int>(buffer.size());
    buf["uri"] = "data:application/octet-stream;base64," + Base64Encode(buffer.data(), buffer.size());
    Json buffers_arr = Json::Array();
    buffers_arr.push_back(buf);
    doc["buffers"] = buffers_arr;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error) *error = "could not open for writing: " + path;
        return false;
    }
    std::string text = doc.dump();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        if (error) *error = "write failed: " + path;
        return false;
    }
    return true;
}
